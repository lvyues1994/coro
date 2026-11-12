# 三种完成方式：值、错误、已停止——以及禁用异常的构建

状态：设计已确认（2026-11-12），尚未实现。本文只描述要做成什么样；实施计划另写。

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
| 7 | 传播时中间层的协程体不运行，promise 上的入口用 P2300 的名字 `unhandled_stopped()` | 在恢复点由用户可见的代码处理 |
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

- `Error` 在 64 位平台上是 16 字节，不分配，不需要 RTTI。用户自己的错误枚举按
  `<system_error>` 的常规方式（`std::is_error_code_enum` + `make_error_code`）接入，
  可以直接隐式转换成 `Error`。`raise()` 在开异常时：装着异常就重抛原异常，装着错误码
  就抛 `std::system_error(code())`。
- `co2::optional<T>`：C++14 没有 `std::optional`，库自带一个同形的子集——构造、
  `has_value()`、`explicit operator bool`、`operator*`、`operator->`、`value()`、
  `value_or()`、`reset()`、`emplace()`，以及与 `optional` / `nullopt` / `T` 的比较。
  空的时候调用 `value()` 是契约违规。为了让头文件在各个标准下定义一致，C++17 起也不
  换成 `std::optional` 的别名——与 `co2::stop_token` 的做法相同。
- 这些类型用标准拼写还是 CamelCase，沿用现有约定：标准已有同名词汇的（`optional`、
  `nullopt`）用标准拼写，库自己的（`Error`、`Result`、`Completion`、`Unit`）用
  CamelCase。

### 2.2 在协程体里产生三种结果

拼写取自 P3552：以错误完成是 `co_yield with_error{e}`，以已停止完成是
`co_await just_stopped()`。

```cpp
CO2_RETURN(value);                                   // 值
CO2_YIELD(co2::with_error(MyErrc::retry_later));     // 错误
CO2_AWAIT(co2::just_stopped());                      // 已停止
CO2_AWAIT(co2::stopIfRequested());                   // 取消点：收到停止请求才停，否则继续
throw std::runtime_error{"..."};                     // 仅开异常时：等同于以错误完成
```

`with_error(e)` 接受任何能转换成 `co2::Error` 的东西。它只是一个标记值，由 promise 的
`yield_value` 接住，所以只在提供了这个重载的协程里能写（`Task`、`AsyncGenerator`）。
`just_stopped()` 与 `stopIfRequested()` 是普通的 awaitable。

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
  以已停止完成，所以 `syncWait(asResult(x))` 返回的 optional 总是有值的。
- `JoinHandle` 本身可以被等待，按值拿它的结果同样写 `syncWait(asResult(handle))`；
  `join()` 不另设 `Result` 形式。
- `co2::syncWait(x) == 42` 这样的比较照常能写；取值的地方要加 `*`。

## 3. 传播机制与两端的协议

核心只有一处：**恢复点**——一次等待结束、协程继续往下走的那个位置（`await_ready()`
为真、`await_suspend` 返回 `false`、或者被 `resume()` 之后）。生成的代码在调用
`await_resume()` 之前，先问 awaiter 这次等待是怎么结束的。

### 3.1 awaiter 一侧：一个可选成员

```cpp
struct MyAwaiter {
    bool await_ready();
    void await_suspend(co2::coroutine_handle<>);
    T await_resume();                               // 只在"有值"时被调用
    co2::Completion await_completion() noexcept;    // 可选
};
```

- 没有这个成员的 awaiter——标准写法的所有 awaiter——在编译期就没有这段检查，零开销，
  行为与今天完全相同。
- awaiter 在别的线程上以错误或已停止完成时不需要新接口：把结果记在自己身上，照常
  `resume()` 等待者。

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
void unhandled_stopped() noexcept;          // P2300 的名字
void unhandled_error(co2::Error) noexcept;  // 与之对称
```

两者只记录结果；向等待者转交由随后的 `final_suspend` 完成。它们与
`unhandled_exception()` 是同一类东西：协程体以"没有值"结束时通知 promise。

promise 没有对应入口时（用户自定义的返回类型）：

| 到来的结果 | 开异常 | 禁用异常 |
| --- | --- | --- |
| 已停止，没有 `unhandled_stopped()` | 契约违规 | 契约违规 |
| 错误，没有 `unhandled_error()` | 在协程体内 `raise()`，走 `unhandled_exception()` | 契约违规 |

- 没有停止令牌的协程收不到停止请求，所以在自定义 promise 的协程里等待 `scheduleOn`
  的现有代码不受影响。例外是提供了 `get_stop_token()` 却没有 `unhandled_stopped()`
  的自定义 promise：停止请求之后等待 `scheduleOn` 成为契约违规，需要补上入口，或者
  写成 `unstoppable(scheduleOn(s))`。
- 开异常时错误在体内重新抛出，就是今天的行为：为标准写的 promise 等待 co2 的 Task
  照常工作。

### 3.4 库内部

- `TaskPromise` 的结果变成四态：空 / 值 / 错误 / 已停止。`unhandled_exception()`
  把当前异常包成 `Error` 存进去；新增 `unhandled_error`、`unhandled_stopped`，以及
  接受 `with_error` 的 `yield_value`。
- `Task` 的 awaiter 提供 `await_completion()`，直接读子 promise 的状态。
- `syncWait` / `spawn` / `whenAll` 的手写续体帧不是协程，它们直接读子 Task 的状态。
- `asResult(x)` 包住内层的 awaiter，自己不提供 `await_completion()`，所以从不向上
  传播；它的 `await_resume()` 把三种结果装进 `Result<T>`。开异常时，内层
  `await_resume()` 抛出的异常也被接成错误。
- `unstoppable(x)` 调内层的 `await_suspend` 时传类型擦除的句柄，内层因此拿不到等待者
  的停止令牌（等待的是子 Task 时，子 Task 的环境里也就没有令牌）；内层的
  `await_suspend` 必须接受 `coroutine_handle<>`。它转发内层的 `await_completion()`：
  被包住的操作仍然可以出错，也仍然可以自己以已停止完成，只是看不到外面的停止请求。

### 3.5 成本

- 普通 awaiter：零。
- 等待 Task：恢复点多一次分支。
- `scheduleOn`：恢复时多读一次停止状态。
- 错误或已停止经过每一层：一次恢复加一次分支，预计与正常返回同一量级（每层 20 纳秒
  左右），对照今天异常重抛的 0.55～0.9 微秒。这是预计，不是实测——第 9 节的两行基准
  用来验证它。
- `Task` 的 promise 里 `exception_ptr`（8 字节）换成 `Error`（16 字节）与一个状态标记。

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
- 丢弃句柄：与今天一致——请求停止并分离，结果无论是哪一种都丢弃。

**`AsyncGenerator`**
- 生产者出错或已停止即流结束。正在等待的那次 `next()` 以同样的结果完成，在消费者
  那里默认向上传播；`asResult(stream.next())` 得到 `Result<bool>`。
- 之后再调 `next()` 得到 `false`。
- 生产者体内同样可以 `CO2_YIELD(co2::with_error(e))`；它与 yield 一个元素靠重载区分。

**`Generator`（同步）**
体内不能等待，没有错误和已停止这两种结果。开异常时，体内抛出的异常照旧在
`begin()` / `++` 处抛给消费者——那是一次普通的同步调用；禁用异常时它不会失败。

**`fromCallback`**
- 续体多两个出口：`done.setError(std::error_code)` 与 `done.setStopped()`。
- `done.setException(std::exception_ptr)` 只在开异常时存在。
- 结果类型的构造函数抛出，照旧记为错误。
- "恰好调用一次"的契约覆盖全部出口。

**`scheduleOn`**
- 取消点设在 hop **之后**：协程在目标 scheduler 上被恢复时，若它的停止令牌已有请求，
  就以已停止完成。收尾（局部析构、向上传播）因此发生在要去的那个线程上。hop 之前
  不检查。
- 没有停止令牌的协程（自定义 promise、类型擦除的句柄）永不被停止。
- 清理阶段用 `unstoppable(scheduleOn(s))`。

**`stopIfRequested()` / `just_stopped()` / `with_error(e)`**
三者都不会真正挂起，只靠 `await_completion()` 报告结果，不需要任何特殊宏。
- `just_stopped()`：立刻就绪，结果是已停止。
- `stopIfRequested()`：在 `await_suspend` 里读等待它的协程的停止令牌（与其他
  awaitable 取令牌的方式相同），然后返回 `false` 让协程立刻继续；有请求则结果是
  已停止。没有令牌的协程里它什么也不做。
- `with_error(e)`：promise 的 `yield_value` 把它变成一个立刻就绪、结果是错误的
  awaiter。

放在一个新的公共头文件里。

**契约违规（新增）**
- 禁用异常时，错误到达 `syncWait` / `join`。
- 已停止或错误被送到没有对应入口的 promise（3.3 的表）。
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
   还要补上 `unhandled_stopped()`（见 3.3）。
3. `whenAll` 的子 Task 已停止，整个 `whenAll` 就是已停止；以前只有失败才影响结果。
4. 子 Task 失败时，中间各层的 promise 不再依次经历 `unhandled_exception()`：
   `TaskPromise` 经 `unhandled_error()` 收到它。用户代码观察不到差别（挂起点不能放进
   `try`），自定义 promise 按 3.3 的表处理。
5. 根上抛出的异常：协程体里抛出的异常原样重抛，与今天一致；以错误码完成的，抛的是
   `std::system_error`。
6. `Continuation::setException` 只在开异常时存在。

## 7. 与提案的对照

| 提案里的东西 | co2 | 一致程度 |
| --- | --- | --- |
| 三个完成通道 `set_value` / `set_error` / `set_stopped` | 值 / 错误 / 已停止，经 `await_completion()` 报告 | 语义一致；co2 没有 receiver，通道表现为 awaiter 的一个查询 |
| `co_yield with_error{e}`（P3552） | `CO2_YIELD(co2::with_error(e))` | 一致 |
| `co_await just_stopped()` | `CO2_AWAIT(co2::just_stopped())` | 一致 |
| promise 的 `unhandled_stopped()` | 同名 | 有意偏离：这里只记录、返回 `void`，转交由 `final_suspend` 完成；P2300 里它返回下一个要恢复的句柄 |
| 被取消的协程"不再恢复"，由拥有者销毁 | 协程体不再运行，但帧当场收尾 | 有意偏离（决策 8）：局部析构得更早，换来常数栈深与不变的销毁契约 |
| `sync_wait` 返回 `optional<tuple<…>>`，错误抛出 | `syncWait` 返回 `optional<T>` / `optional<Unit>` | 一致（单值，不包 tuple） |
| `when_all`：首个错误优先，其次已停止 | `whenAll` 同 | 一致 |
| `unstoppable(sndr)` | `unstoppable(awaitable)` | 一致 |
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
- **根上另加 `trySyncWait` / `tryJoin`。** `syncWait(asResult(x))` 已经能表达，不必再
  多一组接口。
- **被跳过的帧保持挂起（逐字照 P2300）。** 销毁契约要开口子；销毁沿拥有关系从外向内
  递归，十万层的链会栈溢出；被跳过的层里持有的资源要等处理方继续运行之后才释放。
- **`await_resume()` 直接返回 `Result<T>`，由宏拆开。** `await_resume()` 的类型变了，
  `CO2_AWAIT_SET(v, task)` 之外所有依赖它的地方都要跟着改；用一个额外的可选成员则
  完全不碰标准的三个成员。
- **promise 没有入口时报编译错误。** 做不到不误伤：`scheduleOn` 的 awaiter 能报告
  "已停止"，于是所有在自定义 promise 的协程里等待它的代码都会编不过，而这些协程没有
  停止令牌、实际永远收不到。要精确区分，awaiter 得静态声明自己可能产生哪几种结果，
  现在引入太重。
- **`unhandled_stopped()` 返回下一个句柄。** 帧无论如何都要走 `final_suspend`，而它的
  awaiter 已经决定了转给谁；再返回一个句柄就有了两个来源。
- **用 inline namespace 把混用变成链接错误。** 要改动每一个头文件，还会让使用者对
  库类型的前置声明失效；库里另一个影响布局的宏（awaiter 槽容量）目前也没有这层保护。
- **`Task<void>` 的根返回 `bool`。** 与非 void 不同形，泛型代码要分两种写。

## 9. 测试

- 新增一个测试文件覆盖三种完成方式：沿 Task 链传播、`asResult`、`unstoppable`、三个
  小 awaitable、`scheduleOn` 取消点、`whenAll` 的三条规则、`JoinHandle`、
  `AsyncGenerator`、`fromCallback` 的两个新出口、析构由内向外的顺序、十万层链上错误
  与已停止的常数栈深。它在开、关异常两种构建下都跑。
- 现有测试里依赖 `throw` / `catch` 的段落用 `CO2_HAS_EXCEPTIONS` 包起来，其余改成
  适应返回 optional 的根。
- 与原生协程的差分测试只在开异常时构建。
- 新增 CMake 选项，给测试加 `-fno-exceptions -fno-rtti`；CI 增加 GCC 与 Clang 各一个
  这样的作业。
- 第 4 节列出的每一条新增契约违规各有一个测试。
- 基准新增两行：错误、已停止沿 Task 链每层的成本。错误那一行先于其他改动加入，用
  今天的异常路径跑出对照值——第 0 节的数字来自一次性的测量程序，不在仓库里。

## 10. 落地顺序

每一步单独提交，全部测试通过。提交的边界以"不留未定义的中间状态"为准：某种结果
一旦能被产生，所有能收到它的地方在同一个提交里就有确定的行为。

1. 基准：加入"失败沿 Task 链每层的成本"一行，记下今天的数字。
2. 词汇类型：`Error`、`Completion`、`Result<T>`、`optional<T>`、`Unit`。只加类型和
   单元测试，不改行为。
3. 根：`syncWait` / `join` 返回 optional（此时总是有值），同步修改测试与 README。
   纯粹的接口变更，先做掉。
4. 核心：恢复点的 `await_completion()`、promise 的两个入口与兜底规则。用测试里自己
   写的 awaiter 和 promise 验证，库的组件还不产生新结果。
5. `Task` 四态，连同所有直接读 Task 结果的地方——Task 的 awaiter、`syncWait`、
   `spawn` / `JoinHandle`、`whenAll`；三个小 awaitable、`asResult`、`unstoppable`；
   开异常时异常改为捕获一次、按值传播。这一步最大，实施计划里再细分。
6. `AsyncGenerator`、`fromCallback` 的三种完成方式。在此之前，`AsyncGenerator` 的
   生产者按 3.3 的兜底规则处理它等到的错误与已停止。
7. `scheduleOn` 成为取消点，修改受影响的测试。
8. 禁用异常的构建：条件编译、CMake 选项、CI、对应的契约违规测试。
9. 文档：核心设计稿与 README 的对应章节、偏离清单；基准的"已停止"一行与新数字。

## 11. 不在这次范围内

响应停止的同步原语与定时器（库里目前还没有）、`whenAny` / `race`、调度器亲和。它们
以后直接用 `await_completion()` 协议即可：操作在别的线程上被取消时，把"已停止"记在
awaiter 上再恢复等待者。
