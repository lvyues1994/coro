# 三种完成方式：值、错误、已停止——以及禁用异常的构建

状态：设计已确认（2026-11-12）。第 10 节的第 1～5 步已经实现（词汇类型、根返回
optional、恢复点与 promise 的两个入口、`Task` 四态、`just_error` / `with_error` /
`asResult`）；第 6～10 步尚未实现。设计确认当天对照库的代码评审过一轮并修订，哪些
论断验证过、哪些没有，见第 12 节。

## 0. 问题

co2 现在只有一条"失败"通道：异常。协程体里抛出的异常存进 promise 的
`exception_ptr`，在等待者的 `await_resume()` 里重抛，逐层向上，最后由 `syncWait` /
`join` 抛给普通代码。这带来三个问题：

1. **禁用异常的工程用不了。** `-fno-exceptions` 下包含头文件即编译失败；而且
   `exception_ptr` 在那种构建里造不出来也抛不出去，不是加个开关就能解决——需要回答
   "没有异常时，一个 Task 失败用什么表示、怎么往上传"。
2. **取消没有专门的结果。** 协程只能轮询 `stop_token` 然后正常返回一个值，调用方分不清
   "算完了"和"被取消了"；`whenAll` 也无从知道某个 child 是被取消的。
3. **每层重抛很贵。** 一条 100～1000 层的 `Task<int>` 链，叶子抛异常、`syncWait` 处
   接住，`-O2` 下每层 0.55 微秒（GCC 13）到 0.9 微秒（Clang 18）；同一条链正常返回是
   每层 20 纳秒，含帧的分配与释放。而挂起点不能放进 `try` 块（核心设计稿 D1），中间
   各层本来就拦截不了它——这些抛接没有任何用户可见的作用。

目标：一个 Task（以及其他可等待的操作）有三种完成方式——**值**、**错误**、**已停止**
——三者都按值沿等待链传播，开不开异常是同一套机制；在此之上让库能在
`-fno-exceptions`（通常伴随 `-fno-rtti`）下编译和使用。

对照的提案：P2300 的三个完成通道（`set_value` / `set_error` / `set_stopped`）、
`sync_wait`、`when_all`、`unstoppable`，以及 P3552 `std::execution::task` 在协程体内
产生后两种结果的拼写。

## 1. 已确认的决策

| # | 决策 | 没有选的 |
| --- | --- | --- |
| 1 | 值 / 错误 / 已停止三种完成方式一起设计并实现 | 只做到"能编译"；只做错误通道 |
| 2 | 开不开异常共用一套按值传播的机制。开异常时，协程体抛出的异常被捕获成一个错误值，只在根上重新抛出 | 开异常时保持逐层重抛；两者并存 |
| 3 | 普通 `CO2_AWAIT` 默认向上传播，仍然得到 `T`；想自己处理时显式要 `Result` | 默认得到 `Result`、另用一个宏传播 |
| 4 | 错误类型是固定的 `co2::Error`：`std::error_code`，开异常时还能装 `exception_ptr` | `Task<T, E>`；类型擦除的任意值 |
| 5 | 取消点：显式的几个，加上 `scheduleOn`；清理阶段用 `unstoppable(...)` | 只有显式取消点；每个 await 自动检查 |
| 6 | 根上照 P2300 的 `sync_wait`：返回 optional，已停止为空，错误抛出；禁用异常时错误到达根是契约违规 | 另加 `try…` 形式；一律返回 `Result` |
| 7 | 传播时中间层的协程体不运行；promise 上的入口是 `return_stopped()` / `return_error()` | 在恢复点由用户可见的代码处理；沿用 P2300 的名字 `unhandled_stopped()` |
| 8 | 被跳过的层当场析构局部并走到 final suspend | 保持挂起、由拥有者以后销毁 |

各项的理由见第 8 节。

## 2. 词汇类型与接口

### 2.1 类型

```cpp
namespace co2 {

struct Unit {};                              // 空的值类型，void 结果在根上的占位

enum class Errc { unhandled_exception = 1 }; // co2 自己的错误类别，接入 <system_error>

struct Error {
    Error(std::error_code code) noexcept;    // 隐式。前置条件：code 表示失败（非零）
    template <class Enum>                    // 隐式，只接受 std::is_error_code_enum 的
    Error(Enum code) noexcept;               //   枚举：用户的错误枚举一步转换成 Error
    std::error_code code() const noexcept;   // 装的是异常时返回 Errc::unhandled_exception
    [[noreturn]] void raise() const;         // 变回异常；禁用异常时是契约违规

#if CO2_HAS_EXCEPTIONS
    explicit Error(std::exception_ptr exception) noexcept;
    std::exception_ptr exception() const noexcept;   // 装的是错误码时为空
#endif
};

struct Completion {                          // 一次等待怎么结束的：Result<T> 去掉值
    static Completion value() noexcept;
    static Completion stopped() noexcept;
    static Completion error(Error error) noexcept;
    bool isValue() const noexcept;
    bool isStopped() const noexcept;
    bool hasError() const noexcept;
    Error const& error() const noexcept;     // 前置条件：hasError()
};

template <class T> struct Result {           // 值 / Error / 已停止 三选一；T 可以是 void
    Result(T value);                         // 隐式（T 为 void 时是默认构造）
    Result(Error error) noexcept;            // 隐式
    template <class Enum>                    // 隐式，同 Error 的枚举构造
    Result(Enum code) noexcept;
    static Result stopped() noexcept;
    bool hasValue() const noexcept;
    bool hasError() const noexcept;
    bool isStopped() const noexcept;
    explicit operator bool() const noexcept; // hasValue()
    T& value() &;                            // 前置条件：hasValue()
    T&& value() &&;
    Error const& error() const noexcept;     // 前置条件：hasError()
};

template <class T> struct optional;          // std::optional 的子集，见下
struct nullopt_t;
extern nullopt_t const nullopt;

} // namespace co2
```

- `Error` 不分配，不需要 RTTI。GCC / Clang 上它是 16 字节：错误码的两个字段与
  `exception_ptr` 共用存储（原型上确认）。MSVC 的 `exception_ptr` 是两个指针，那里预计
  是 24 字节，未验证。
- 用户自己的错误枚举按 `<system_error>` 的常规方式（`std::is_error_code_enum` +
  `make_error_code`）接入。枚举先转成 `std::error_code` 再转成 `Error` 是两次用户定义
  转换，隐式转换走不通，所以 `Error` 与 `Result` 各有一个只接受错误码枚举的构造函数
  模板——`std::error_code` 自己也是这样做的。
- `raise()` 在开异常时：装着异常就重抛原异常，装着错误码就抛
  `std::system_error(code())`。
- `co2::optional<T>`：C++14 没有 `std::optional`，库自带一个同形的子集——构造、
  `has_value()`、`explicit operator bool`、`operator*`、`operator->`、`value()`、
  `value_or()`、`reset()`、`emplace()`，以及与 `optional` / `nullopt` / `T` 的比较。
  空的时候调用 `value()` 是契约违规。为了让头文件在各个标准下定义一致，C++17 起也不
  换成 `std::optional` 的别名——与 `co2::stop_token` 的做法相同。
- 这些类型用标准拼写还是 CamelCase，沿用现有约定：标准已有同名词汇的（`optional`、
  `nullopt`）用标准拼写，库自己的（`Error`、`Result`、`Completion`、`Unit`）用
  CamelCase。

### 2.2 在协程体里产生三种结果

```cpp
CO2_RETURN(value);                                   // 值
CO2_AWAIT(co2::just_error(MyErrc::retry_later));     // 错误
CO2_YIELD(co2::with_error(MyErrc::retry_later));     // 错误，P3552 的另一种拼写
CO2_AWAIT(co2::just_stopped());                      // 已停止
CO2_AWAIT(co2::stopIfRequested());                   // 取消点：收到停止请求才停，否则继续
throw std::runtime_error{"..."};                     // 仅开异常时：等同于以错误完成
```

名字取自 P2300 与 P3552：`just_error` / `just_stopped` 是 P2300 的 sender，
`with_error` 是 P3552 配 `co_yield` 用的标记。

- `just_error(e)`、`just_stopped()`、`stopIfRequested()` 是普通的 awaitable，在任何
  提供了对应入口（3.3）的 promise 里都能用。
- `with_error(e)` 只是一个标记值，由 promise 的 `yield_value` 接住，所以只在提供了这个
  重载的协程里能写（`Task`、`AsyncGenerator`）。它与 `just_error(e)` 效果相同。
- `just_error` 与 `with_error` 接受任何能转换成 `co2::Error` 的东西。

### 2.3 消费

```cpp
CO2_AWAIT_SET(n, readHeader(socket));                // 得到 T；错误 / 已停止向上传播
CO2_AWAIT_SET(r, co2::asResult(readHeader(socket))); // 得到 Result<T>，不传播
CO2_AWAIT(co2::unstoppable(co2::scheduleOn(pool)));  // 对被包住的操作隐藏停止请求
```

"向上传播"的意思：本协程到此为止，其后的语句不执行，局部按声明的逆序析构，本协程
以同样的结果完成，交给它的等待者。

### 2.4 根

```cpp
co2::optional<int> n = co2::syncWait(task);               // 已停止 → 空
co2::optional<co2::Unit> done = co2::syncWait(voidTask);  // Task<void> 与非 void 同形
auto r = co2::syncWait(co2::asResult(task));              // optional<Result<int>>
co2::optional<int> m = handle.join();                     // JoinHandle 同理
```

- 根遇到错误：开异常时 `Error::raise()`；禁用异常时是契约违规。
- 想把错误当值拿到，不靠另一套接口，而是在被等待的东西外面套 `asResult`。禁用异常的
  工程里，这是让可能失败的 Task 到达根而不终止程序的唯一写法。`asResult` 自己不会
  以已停止完成，所以 `syncWait(asResult(x))` 返回的 optional 总是有值的——这是为了
  与其他根同形付出的代价，使用者要多写一个 `*`。
- `syncWait` 的通用重载会把非 Task 的 awaitable 再包一层协程（多分配一个帧）。
  `asResult(Task<T>)` 是禁用异常的工程里的主路径，不能每次白付这一帧：`syncWait` 为它
  单设一个重载，直接读 Task 的结果。
- `JoinHandle` 本身可以被等待，按值拿它的结果同样写 `syncWait(asResult(handle))`；
  `join()` 不另设 `Result` 形式。
- `co2::syncWait(x) == 42` 这样的比较照常能写；取值的地方要加 `*`。结果是 `bool` 的
  根要当心：`if (co2::syncWait(task))` 改前改后都能编译，含义却从"值为真"变成"没有
  被停止"（列入第 6 节）。

## 3. 传播机制与两端的协议

核心只有一处：**恢复点**——一次等待结束、协程继续往下走的那个位置（`await_ready()`
为真、`await_suspend` 返回 `false`、或者被 `resume()` 之后）。生成的代码在调用
`await_resume()` 之前，先问 awaiter 这次等待是怎么结束的。

### 3.1 awaiter 一侧：一个可选成员

```cpp
struct MyAwaiter {
    bool await_ready();
    void await_suspend(co2::coroutine_handle<>);
    T await_resume();
    co2::Completion await_completion() noexcept;    // 可选
};
```

- 没有这个成员的 awaiter——标准写法的所有 awaiter——在编译期就没有这段检查，零开销，
  行为与今天完全相同。
- awaiter 在别的线程上以错误或已停止完成时不需要新接口：把结果记在自己身上，照常
  `resume()` 等待者。

**调用约定。** 宏生成的代码对每次等待恰好调用它一次，在恢复等待者的那个线程上，先于
`await_resume()`；结果是"有值"才接着调用 `await_resume()`。它不是可以重复调用的
查询：awaiter 可以在这次调用里把结果取走（`AsyncGenerator::next()` 就是这样，见
第 4 节）。包装别的 awaiter 的 awaiter 对内层遵守同一约定。

**`await_resume()` 保持自足。** 不认识 `await_completion()` 的调用方会直接调用
`await_resume()`：

- 只转发标准三个成员的手写包装 awaiter；
- 原生的 `co_await`——差分测试里 awaiter 与原生协程共用，C++20 互操作层也在核心
  设计稿 17.4 的待重建清单上。

所以库内每个 awaiter 的 `await_resume()` 在没有值时仍有确定的行为：

| 结果 | `await_resume()` 的行为 |
| --- | --- |
| 错误 | `Error::raise()`：开异常时抛出，与今天完全相同；禁用异常时契约违规 |
| 已停止 | 契约违规 |

由此得到两条性质：

- 错误照旧能穿过旧式的调用方。只把 `Task` 改成按值记录错误、其余组件一律不动，现有
  测试全部通过（第 12 节），第 10 节的落地顺序就建立在这一点上。
- **已停止这条通道只存在于宏协程里。** 被旧式包装 awaiter 包住的操作一旦以已停止
  完成，程序终止（列入第 6 节）。以后的 C++20 互操作层要在边界上把它翻译成标准的
  `unhandled_stopped()`。

**转发。** 库内转发别的 awaiter 的地方都要转发这个成员：`detail::AwaiterRef`（左值
awaiter 的帧内形式）、右值 `Task` / `JoinHandle` 自己当 awaiter 的那一组成员、
`unstoppable`。漏掉的后果是悄悄退回上面的兜底。

### 3.2 恢复点的顺序

`CO2_AWAIT`、`CO2_AWAIT_SET`、`CO2_AWAIT_AS` 与 `CO2_YIELD` 的恢复点
（`initial_suspend` / `final_suspend` 不参与）：

1. awaiter 有 `await_completion()` 就调用它。
2. 结果是"有值"，或者 awaiter 没有这个成员：照常 `await_resume()`，继续执行。
3. 否则，依次：销毁 awaiter → 析构局部（离开协程体作用域）→ 调用 promise 的入口
   （见 3.3）→ `co_await promise.final_suspend()`。不调用 `await_resume()`，其后的
   用户语句一行也不执行。

第 3 步的顺序与今天异常逃出协程体时完全相同（那时调用的是
`unhandled_exception()`）。因为每一层都由恢复循环驱动，十万层的等待链上传播也是常数
栈深；析构由内向外、当场发生。每个帧都走到 final suspend，"挂起在某个操作上的协程
不能销毁"这条契约原样成立，帧头与句柄的 ABI 不变。

### 3.3 promise 一侧：两个入口

```cpp
void return_stopped() noexcept;
void return_error(co2::Error) noexcept;
```

名字比照 `return_value` / `return_void`：协程以这种结果结束。两者只记录结果；向
等待者转交由随后的 `final_suspend` 完成。

**不沿用 P2300 的 `unhandled_stopped()`。** 标准里那个成员返回要恢复的句柄，调用它
之后协程不再被恢复，`as_awaitable` 还要求它的返回值能转换成句柄。这里的入口在协程体
收尾之后被调用，只做记录，之后协程照常走 `final_suspend`。同名而契约不同，按标准写的
promise 被 co2 的宏使用时会把等待者完成两次。名字分开之后，这样的 promise 落进下表
（已停止：契约违规），一个 promise 也可以两个成员都提供。

promise 没有对应入口时（用户自定义的返回类型）：

| 到来的结果 | 开异常 | 禁用异常 |
| --- | --- | --- |
| 已停止，没有 `return_stopped()` | 契约违规 | 契约违规 |
| 错误，没有 `return_error()` | 在协程体内 `raise()`，走 `unhandled_exception()` | 契约违规 |

- 没有停止令牌的协程收不到停止请求，所以在自定义 promise 的协程里等待 `scheduleOn`
  的现有代码不受影响。例外是提供了 `get_stop_token()` 却没有 `return_stopped()` 的
  自定义 promise：停止请求之后等待 `scheduleOn` 成为契约违规，需要补上入口，或者
  写成 `unstoppable(scheduleOn(s))`。
- 开异常时错误在体内重新抛出，就是今天的行为：为标准写的 promise 等待 co2 的 Task
  照常工作。

### 3.4 库内部

- `TaskPromise` 的结果变成四态：空 / 值 / 错误 / 已停止，值与错误共用一块存储
  （3.5）。`unhandled_exception()` 把当前异常包成 `Error` 存进去；新增
  `return_error`、`return_stopped`，以及接受 `with_error` 的 `yield_value`。
- `Task` 的 awaiter 提供 `await_completion()`，直接读子 promise 的状态。
- `syncWait` / `spawn` / `whenAll` 的手写续体帧不是协程，它们直接读子 Task 的状态。
- `asResult(x)` 包住内层的 awaiter，自己不提供 `await_completion()`，所以从不向上
  传播；它的 `await_resume()` 把三种结果装进 `Result<T>`。开异常时，内层
  `await_resume()` 抛出的异常也被接成错误。
- `unstoppable(x)` 调内层的 `await_suspend` 时传类型擦除的句柄，内层因此拿不到等待者
  的停止令牌（等待的是子 Task 时，子 Task 的环境里也就没有令牌）；内层的
  `await_suspend` 必须接受 `coroutine_handle<>`。它转发内层的 `await_completion()`：
  被包住的操作仍然可以出错，也仍然可以自己以已停止完成，只是看不到外面的停止请求。
  **已知限制**：这样隐藏的是整个环境，而 P2300 的 `unstoppable` 保留环境的其余部分、
  只替换停止令牌。今天环境里只有停止令牌，两者等价；加入调度器亲和时要换成只替换
  令牌的做法。

### 3.5 成本

1000 层的 `Task<int>` 链，`-O2`，每层的纳秒数。"原型"是第 12 节说的一次性原型，
不在仓库里；第 9 节的基准落地后以基准为准。

| | GCC 13 | Clang 18 |
| --- | --- | --- |
| 正常返回，改动前 | 20.4 | 21.4 |
| 正常返回，原型 | 21.3 | 21.9 |
| 叶子抛异常、根上接住，改动前 | 539 | 892 |
| 叶子抛异常、根上接住，原型 | 42 | 38 |
| 以错误码完成，原型 | 23 | 24 |
| 以已停止完成，原型 | 21 | 22 |

- 普通 awaiter：零，编译期就没有这段检查。
- 等待 Task：恢复点多一次分支。仓库基准 `symmetric Task transfer` 改动前 12.0、原型
  11.5～12.0 纳秒，`syncWait ready Task` 改动前 25.5、原型 25.7～25.9 纳秒。
- `scheduleOn`：恢复时多读一次停止状态（未测）。
- 链越短，异常路径的收益越小：抛出与根上的重抛各只发生一次，10 层的链上摊下来是每层
  约 250 纳秒（改动前 710 / 1100）。
- 原型里异常比错误码每层多约 20 纳秒。原因没有单独验证，推测是原型每层把
  `exception_ptr` 复制了两次；实现时 `Error` 一路移动。

**值与错误共用存储，钉死的大小不变。** 两者互斥，分开存是白占空间，而且会越过两条
现有的界线：

- `TaskPromise<int>` 现在 40 字节。分开存是 56 字节，`Task<int>(int)` 的帧不再是
  `allocation_tests` 钉死的 176 字节；共用存储仍是 40 字节，结果类型大于 16 字节时还比
  现在小 16 字节。
- `CallbackAwaitable<void*>` 现在 64 字节，正好等于内联 awaiter 槽的容量。再多 8 字节
  就落到堆上，每次等待多一次分配。

## 4. 各组件的行为

**`Task`**
等待它得到 `T`，错误与已停止向等待者传播。销毁契约不变。

**`whenAll`**（照 P2300 的 `when_all`）
- 全部有值：按参数顺序交付。
- 任一子 Task 出错：向其余请求停止，等全部结束后以**时间上第一个错误**完成。
- 没有错误但有子 Task 已停止：同样向其余请求停止，等全部结束后以已停止完成。
- 父的停止请求照常转发给每个子 Task。

**`spawn` / `JoinHandle`**
- 在协程里等待 `JoinHandle`：被 spawn 的 Task 出错或已停止，等待者同样出错或已停止。
- `join()` 返回 `optional<T>`：已停止为空；错误抛出或契约违规。
- 丢弃句柄：与今天一样请求停止并分离，结果无论是哪一种都丢弃。配上 `scheduleOn`
  取消点，被丢弃句柄的任务会在它的下一次 `scheduleOn` 结束；今天它不轮询令牌就会跑完
  （列入第 6 节）。

**`AsyncGenerator`**
- 生产者出错或已停止即流结束。正在等待的那次 `next()` 以同样的结果完成，在消费者
  那里默认向上传播；`asResult(stream.next())` 得到 `Result<bool>`。
- `next()` 的 `await_completion()` 把这个结果取走——今天取走发生在 `await_resume()`
  里——所以之后再调 `next()` 得到 `false`。
- 生产者体内同样可以 `CO2_YIELD(co2::with_error(e))`；它与 yield 一个元素靠重载区分。

**`Generator`（同步）**
体内不能等待，没有错误和已停止这两种结果。开异常时，体内抛出的异常照旧在
`begin()` / `++` 处抛给消费者——那是一次普通的同步调用；禁用异常时它不会失败。

**`fromCallback`**
- 续体多两个出口：`done.setError(std::error_code)` 与 `done.setStopped()`。
- `done.setException(std::exception_ptr)` 只在开异常时存在。
- 结果类型的构造函数抛出，照旧记为错误。
- "恰好调用一次"的契约覆盖全部出口。
- 值与错误共用存储（3.5）。

**`scheduleOn`**
- 取消点设在 hop **之后**：协程在目标 scheduler 上被恢复时，若它的停止令牌已有请求，
  就以已停止完成。收尾（局部析构、向上传播）因此发生在要去的那个线程上。hop 之前
  不检查。
- 它的 `await_suspend` 改为按等待者的 promise 类型取令牌，并且在调用
  `Scheduler::schedule()` **之前**记下：`schedule()` 一返回，协程可能已经在另一个线程
  上恢复，此后再写 awaiter 就是数据竞争。
- 没有停止令牌的协程（自定义 promise、类型擦除的句柄）永不被停止。
- 清理阶段用 `unstoppable(scheduleOn(s))`。

**`stopIfRequested()` / `just_stopped()` / `just_error(e)` / `with_error(e)`**
四者都不会真正挂起，只靠 `await_completion()` 报告结果，不需要任何特殊宏。
- `just_stopped()`：立刻就绪，结果是已停止。
- `just_error(e)`：立刻就绪，结果是错误。
- `stopIfRequested()`：在 `await_suspend` 里读等待它的协程的停止令牌（与其他
  awaitable 取令牌的方式相同），然后返回 `false` 让协程立刻继续；有请求则结果是
  已停止。没有令牌的协程里它什么也不做。
- `with_error(e)`：promise 的 `yield_value` 把它变成 `just_error(e)` 的 awaiter。

放在一个新的公共头文件里。

**契约违规（新增）**
- 禁用异常时，错误到达 `syncWait` / `join`。
- 已停止或错误被送到没有对应入口的 promise（3.3 的表）。
- 库内 awaiter 的 `await_resume()` 在已停止时被调用；禁用异常时，在错误时被调用
  （3.1 的表）。
- 用表示成功的 `std::error_code` 构造 `Error`。

## 5. 禁用异常的构建

- `config.hpp` 新增 `CO2_HAS_EXCEPTIONS`，由编译器宏自动得出（GCC / Clang 的
  `__cpp_exceptions`，MSVC 的 `_CPPUNWIND`）。不提供手动覆盖：关着异常却让库保留
  catch 没有意义，开着异常却让库去掉 catch 则是资源泄漏。
- 为 0 时消失的东西：
  - 内部回滚用的 `try` / `catch`（创建帧、超对齐分配、线程池构造）。没有人能抛，
    这些路径本来就走不到。
  - 协程体变换里的 catch。promise 不再需要 `unhandled_exception()`，与原生编译器在
    `-fno-exceptions` 下的要求一致。
  - `Error` 里装异常的那一半、`Error::exception()`、`Continuation::setException`。
  - `Error::raise()` 与根上"抛出"的那条路，换成契约违规。
  - 靠捕获 `bad_alloc` 触发 `get_return_object_on_allocation_failure` 的那条路；
    promise 的 `operator new` 返回空的那条路保留。
- 第 2～4 节的值通道在两种构建下是同一份代码，不随这个宏变化。
- 分配失败时全局 `operator new` 的行为由运行时决定（通常是终止），与任何禁用异常的
  程序相同。
- **混用**：同一个程序里一部分翻译单元开异常、一部分关，头文件里的内联函数就有了
  两份不同的定义，属于静默的 ODR 违规。文档写明"所有翻译单元的异常开关必须一致"；
  MSVC 上加一行 `#pragma detect_mismatch` 做链接期检查。

## 6. 不兼容改动与行为变化

现有代码会受影响的地方，全部列在这里：

1. `syncWait` / `JoinHandle::join` 的返回类型从 `T` 变成 `co2::optional<T>`，
   `void` 变成 `co2::optional<co2::Unit>`。
2. `scheduleOn` 成为取消点：轮询令牌后正常返回的协程，停止请求之后遇到下一个
   `scheduleOn` 就以已停止结束，不再走到 `CO2_RETURN`。需要在停止之后继续换线程的，
   改用 `unstoppable(scheduleOn(s))`。自定义 promise 若提供了 `get_stop_token()`，
   还要补上 `return_stopped()`（见 3.3）。
3. `whenAll` 的子 Task 已停止，整个 `whenAll` 就是已停止；以前只有失败才影响结果。
4. 子 Task 失败时，中间各层的 promise 不再依次经历 `unhandled_exception()`：
   `TaskPromise` 经 `return_error()` 收到它。用户代码观察不到差别（挂起点不能放进
   `try`），自定义 promise 按 3.3 的表处理。
5. 根上抛出的异常：协程体里抛出的异常原样重抛，与今天一致；以错误码完成的，抛的是
   `std::system_error`。
6. `Continuation::setException` 只在开异常时存在。
7. 丢弃 `JoinHandle` 的任务会在它的下一次 `scheduleOn` 被取消。今天丢弃只是发出停止
   请求，任务不轮询就会跑完；库里没有 detach 或 scope，"spawn 之后不保留句柄"的任务
   只要换线程就会被停掉。要让它跑完，保留句柄，或者在任务里用
   `unstoppable(scheduleOn(s))`。
8. 只转发标准三个成员的手写包装 awaiter：被包住的操作出错时行为不变（开异常时
   `await_resume()` 照旧抛出）；以已停止完成时是契约违规，需要给包装 awaiter 加上
   `await_completion()` 转发（3.1）。
9. 结果是 `bool` 的根：`if (co2::syncWait(task))`、`if (handle.join())` 照样能编译，
   含义从"值为真"变成"没有被停止"。仓库的测试、基准与文档里没有这种用法。

## 7. 与提案的对照

`run_loop`、`when_all`、`sync_wait`、`unstoppable`、`unhandled_stopped` 这几行核对过
C++ 工作草案的原文（第 12 节）。

| 提案里的东西 | co2 | 一致程度 |
| --- | --- | --- |
| 三个完成通道 `set_value` / `set_error` / `set_stopped` | 值 / 错误 / 已停止，经 `await_completion()` 报告 | 语义一致；co2 没有 receiver，通道表现为 awaiter 的一个查询 |
| awaiter 协议是三个成员 | 多一个可选的 `await_completion()` | 扩展：原生 `co_await` 不认识它，`await_resume()` 因此保持自足（3.1） |
| `co_yield with_error{e}`（P3552） | `CO2_YIELD(co2::with_error(e))` | 一致 |
| `co_await just_stopped()` | `CO2_AWAIT(co2::just_stopped())` | 一致 |
| `co_await just_error(e)` | `CO2_AWAIT(co2::just_error(e))` | 名字一致；标准里它在等待处抛出，这里按值结束协程（决策 2） |
| promise 的 `coroutine_handle<> unhandled_stopped()`：代替恢复被调用，协程不再恢复 | `void return_stopped()`：协程体收尾之后被调用，只做记录 | 有意偏离，名字也因此不同（3.3） |
| 被取消的协程"不再恢复"，由拥有者销毁 | 协程体不再运行，但帧当场收尾 | 有意偏离（决策 8）：局部析构得更早，换来常数栈深与不变的销毁契约 |
| `sync_wait` 返回 `optional<tuple<…>>`，错误抛出，错误码抛 `system_error` | `syncWait` 返回 `optional<T>` / `optional<Unit>` | 一致（单值，不包 tuple） |
| `when_all`：错误压过已停止，取第一个错误 | `whenAll` 同 | 一致 |
| `unstoppable(sndr)`：保留外层环境，只把停止令牌换成 `never_stop_token` | `unstoppable(awaitable)` | 今天等价；实现上隐藏的是整个环境（3.4 的已知限制） |
| `run_loop` 的 schedule：轮到它执行时若已有停止请求，以已停止完成 | `scheduleOn` 在 hop 之后检查 | 一致 |
| 错误类型由 completion signatures 声明，可以有多种 | 固定的 `co2::Error` | 有意偏离（决策 4） |
| 等待以错误完成的 sender 时在 `co_await` 处抛出 | 按值向上传播，只在根上抛出 | 有意偏离（决策 2）：挂起点不能放进 `try`，中间层的抛接没有可见作用 |

## 8. 被否决的方案

- **只做到"能编译"。** 库里就不存在"失败"这个概念了：使用者让 Task 返回自己的
  Expected 类型、逐层手写判断，`whenAll` 也没有"任一失败即取消其余"这回事。
- **两套机制（开异常时保持逐层重抛）。** 开异常的构建零变化，但两种配置的写法、测试、
  组合器实现都要两份。
- **普通 await 默认得到 `Result`。** 控制流全部显式，但现有代码全部要改，而且与
  "`co_await task` 得到 `T`"的标准形态不一致。
- **`Task<T, E>`。** 类型最严，但 `Task`、`whenAll`、`spawn`、`JoinHandle` 全部多一个
  模板参数，父子类型不同时每个 await 都要处理转换。
- **类型擦除的错误。** 取出时要 RTTI 或自建类型标签，大对象要分配；而禁用异常的工程
  通常也关掉了 RTTI。
- **每个 await 自动检查停止令牌。** 每次等待多一次原子读取，控制流处处隐含。
- **根上另加 `trySyncWait` / `tryJoin`。** `syncWait(asResult(x))` 已经能表达，它多出
  的那一帧由专门的重载消掉（2.4），不必再多一组接口。
- **被跳过的帧保持挂起（逐字照 P2300）。** 销毁契约要开口子；销毁沿拥有关系从外向内
  递归，十万层的链会栈溢出；被跳过的层里持有的资源要等处理方继续运行之后才释放。
- **`await_resume()` 直接返回 `Result<T>`，由宏拆开。** `await_resume()` 的类型变了，
  `CO2_AWAIT_SET(v, task)` 之外所有依赖它的地方都要跟着改；用一个额外的可选成员则
  完全不碰标准的三个成员。
- **promise 没有入口时报编译错误。** 做不到不误伤：`scheduleOn` 的 awaiter 能报告
  "已停止"，于是所有在自定义 promise 的协程里等待它的代码都会编不过，而这些协程没有
  停止令牌、实际永远收不到。要精确区分，awaiter 得静态声明自己可能产生哪几种结果，
  现在引入太重。
- **入口沿用 `unhandled_stopped()` 这个名字。** 与标准同名而契约不同（3.3）。让它像
  标准那样返回下一个句柄也不行：帧无论如何都要走 `final_suspend`，而它的 awaiter
  已经决定了转给谁，再返回一个句柄就有了两个来源。
- **用 inline namespace 把混用变成链接错误。** 要改动每一个头文件，还会让使用者对
  库类型的前置声明失效；库里另一个影响布局的宏（awaiter 槽容量）目前也没有这层保护。
- **`Task<void>` 的根返回 `bool`。** 与非 void 不同形，泛型代码要分两种写。

## 9. 测试

- 新增一个测试文件覆盖三种完成方式：沿 Task 链传播、`asResult`、`unstoppable`、四个
  小 awaitable、`scheduleOn` 取消点、`whenAll` 的三条规则、`JoinHandle`、
  `AsyncGenerator`、`fromCallback` 的两个新出口、析构由内向外的顺序、十万层链上错误
  与已停止的常数栈深。它在开、关异常两种构建下都跑。
- 旧式调用方（3.1）：只转发三个成员的包装 awaiter 包住失败的 Task，错误照旧从
  `await_resume()` 抛出；包住以已停止完成的操作是契约违规。按左值等待的 awaiter
  （经 `detail::AwaiterRef`）同样走新通道。
- `allocation_tests` 里钉死的帧大小与 `CallbackAwaitable` 大小不变（3.5）。
- 现有测试里依赖 `throw` / `catch` 的段落用 `CO2_HAS_EXCEPTIONS` 包起来，其余改成
  适应返回 optional 的根。
- 与原生协程的差分测试只在开异常时构建。原生协程不认识 `await_completion()`，新通道
  没有差分覆盖，只靠上面的测试。
- 新增 CMake 选项，给测试加 `-fno-exceptions -fno-rtti`；CI 增加 GCC 与 Clang 各一个
  这样的作业。
- 第 4 节列出的每一条新增契约违规各有一个测试。
- 基准新增两行：错误、已停止沿 Task 链每层的成本。错误那一行先于其他改动加入，用
  今天的异常路径跑出对照值——第 0 节与 3.5 的数字来自一次性的测量，不在仓库里。

## 10. 落地顺序

每一步单独提交，全部测试通过。能这样拆，靠的是 3.1 的兜底：一个组件还没有
`await_completion()` 时，错误照旧从它的 `await_resume()` 抛出；已停止则要等到最后
才有公开的产生方。

1. 基准：加入"失败沿 Task 链每层的成本"一行，记下今天的数字。
2. 词汇类型：`Error`、`Completion`、`Result<T>`、`optional<T>`、`Unit`。只加类型和
   单元测试，不改行为。
3. 根：`syncWait` / `join` 返回 optional（此时总是有值），同步修改测试与 README。
   纯粹的接口变更，先做掉。
4. 核心：恢复点的 `await_completion()`、promise 的两个入口与兜底规则、
   `detail::AwaiterRef` 的转发。用测试里自己写的 awaiter 和 promise 验证，库的组件
   还不产生新结果。
5. `Task` 四态（值与错误共用存储），Task 的 awaiter 与两个根读它；`just_error` /
   `with_error`、`asResult` 及其 `syncWait` 重载；开异常时异常改为捕获一次、按值
   传播。`whenAll`、等待 `JoinHandle`、`AsyncGenerator`、`fromCallback` 这一步不动。
6. 其余组件逐个接上 `await_completion()`，每个一个提交：等待 `JoinHandle`、
   `whenAll`、`AsyncGenerator`、`fromCallback`。已停止在这一步只由测试自己写的
   awaiter 产生。
7. 已停止的公开产生方：`just_stopped`、`stopIfRequested`、`unstoppable`。
8. `scheduleOn` 成为取消点，修改受影响的测试。
9. 禁用异常的构建：条件编译、CMake 选项、CI、对应的契约违规测试。
10. 文档：核心设计稿与 README 的对应章节、偏离清单、迁移说明；基准的"已停止"一行与
    新数字。

## 11. 不在这次范围内

响应停止的同步原语与定时器（库里目前还没有）、`whenAny` / `race`、调度器亲和。它们
以后直接用 `await_completion()` 协议即可：操作在别的线程上被取消时，把"已停止"记在
awaiter 上再恢复等待者。

## 12. 已经验证的与尚未验证的

设计确认之后对照库的代码评审过一轮，并在库的一份副本上做了一次性原型：恢复点的
检查、promise 的两个入口与兜底、`Task` 按值记录错误与已停止，约 100 行，只在开异常
的构建下。原型已删除，不在仓库里。

**原型上确认的**

- 机制可行，且不影响现有行为：GCC C++14、GCC C++20、Clang C++20 三个配置下，22 个
  测试各有 21 个通过，含差分测试。唯一失败的是帧大小的钉死检查——原型把值与错误分开
  存了（3.5）。这一轮里 `whenAll`、`JoinHandle`、`AsyncGenerator`、`fromCallback` 都
  没有改动，走的是 3.1 的兜底。
- 3.5 的成本数字。
- 十万层的链在 ASan + UBSan、默认栈下，已停止与错误都传到了根；局部由内向外析构，
  等待之后的语句没有执行。
- `Error` 在 GCC / Clang 上能做成 16 字节；错误码枚举不加构造函数模板就不能隐式转换
  成 `Error`。

**对照原文核对过的**

第 7 节里 `run_loop` 的停止检查、`when_all` 的优先级、`sync_wait` 的返回值与
`system_error`、`unstoppable` 的环境、`unhandled_stopped` 的签名与"协程不再恢复"，
对照的是 `cplusplus/draft` 的 `exec.tex`；`with_error`、`just_error`、`just_stopped`
的拼写对照的是 P3552 的文本。

**尚未验证的**

- 禁用异常的构建（第 5 节）没有任何实测。
- MSVC：`Error` 的大小、`#pragma detect_mismatch`、`_CPPUNWIND` 的判断都无法在本机
  验证。
- `asResult`、`unstoppable`、`whenAll`、`JoinHandle`、`AsyncGenerator`、
  `fromCallback`、`scheduleOn` 的新行为没有在原型里实现，第 4 节只对照了代码与草案。
