# co2

`co2` 是一个独立、仅含头文件的 C++14 无栈协程库。它不依赖编译器协程支持和第三方
库，用宏生成的 switch 状态机模拟协程的挂起与恢复——但协议、帧布局与语义**逐条对齐
C++20 协程规范**（[dcl.fct.def.coroutine]、[expr.await]、[coroutine.handle]）以及
C++23 `std::generator`、C++20 `std::stop_token` 和 P2300/P3149 的执行模型。

当前版本是内部预览版 `0.3.0-pre`（v2 核心）。项目尚未声明公开发布许可证。

## 设计原则

- **与标准同形。** `co2::coroutine_handle<P>`、`suspend_always`、`noop_coroutine`、
  `coroutine_traits`、promise 协议（`get_return_object` / `initial_suspend` /
  `final_suspend` / `return_value` / `unhandled_exception` / `yield_value` /
  `await_transform`）、awaiter 协议（`await_ready` / `await_suspend` 三种返回类型 /
  `await_resume`）与 `operator co_await` 查找都按标准实现。为标准写的 promise 与
  awaiter 类型可以逐字用于 co2；C++20 构建下的差分测试证明两者事件序列相同。
- **标准的销毁契约。** 挂起在某个操作上的协程不能销毁；取消是协作式的
  （`stop_token`），操作提前完成后协程走到 final suspend，再由拥有者销毁。这让核心
  没有任何原子量、每次异步等待零分配。co2 把这类未定义行为报告为**契约违规**而不是
  静默 UB。
- **显式调度。** 协程在完成它的那个线程上被内联恢复；换线程是显式的
  `CO2_AWAIT(scheduleOn(s))`。没有隐式的"当前 scheduler"。
- **零分配热路径。** 一个协程一次分配（帧）；`scheduleOn` 一次 hop、`Generator` 一次
  `yield`、Task 之间的对称转移都不分配。

## 最小协程

```cpp
#include <co2/co2.hpp>

auto twice(int value) CO2_BEG(co2::Task<int>, (value), int doubled{};) {
    doubled = value * 2;
    CO2_AWAIT(co2::suspend_never{});
    CO2_RETURN(doubled);
}
CO2_END

int main() { return co2::syncWait(twice(21)) == 42 ? 0 : 1; }
```

`CO2_BEG(返回类型, (捕获的参数...), 帧局部声明...)` 开始协程体，`CO2_END` 结束。
捕获列表里的名字必须是函数参数（按值参数被移动进帧，引用保持引用）；捕获列表之后
的声明成为帧成员，因此能跨挂起点存活。体内可用：

| 宏 | 对应的标准语法 |
| --- | --- |
| `CO2_AWAIT(e)` | `co_await e;` |
| `CO2_AWAIT_SET(v, e)` | `v = co_await e;` |
| `CO2_AWAIT_AS(T, e)` / `CO2_AWAIT_AS_SET(v, T, e)` | 同上，显式给出 awaitable 类型（表达式含 lambda 时） |
| `CO2_YIELD(e)` | `co_yield e;` |
| `CO2_RETURN()` / `CO2_RETURN(e)` | `co_return;` / `co_return e;` |
| `CO2_BEG_WITH_ALLOCATOR(R, alloc, (...), ...)` | `std::allocator_arg` 约定的帧分配 |
| `CO2_BEG_MEMBER(R, (...), ...)` / `CO2_BEG_MEMBER_WITH_ALLOCATOR(...)` | 非静态成员函数的协程体：体内用 `self` 代替 `this` |

返回类型是任何带 `promise_type` 的类型（或特化了 `co2::coroutine_traits<R>`）。

### 与语言级协程的差别

这些差别全部来自"没有编译器"，见 `docs/design/coroutine-core-v2.md` 第 6 节：

- 跨挂起点存活的局部必须写在 `CO2_BEG` 的局部列表里，在帧创建时构造（析构时机与
  标准一致）；挂起点不能出现在 `try` 块、lambda 或嵌套函数里，也不能出现在协程体自己的
  `switch` 语句里（编译错误：状态机的 `case` 标签会绑到最近一层 switch）。
- 协程体里直接声明的变量住在栈上，活不过挂起点。带初始化的声明跨越挂起点是编译错误；
  但**不带初始化的平凡类型**（`int sum;`）编译器不拦，恢复之后读到的是不确定的值——
  体内的声明一律带上初始化，需要跨挂起点的放进局部列表。
- 成员函数的协程体用 `CO2_BEG_MEMBER`。协程体是一个局部类的成员函数，访问不到外层的
  `this` 和成员名，所以体内用 `self`（与 `this` 同类型的指针）代替：`self->member`。
  promise 的构造函数先收到 `*this` 再是其余参数，与标准一致。带 `override` / `final`
  的虚函数在类内声明、类外用宏定义（说明符跟在尾置返回类型之后，宏插不进去）。
- 右值 awaiter 会被移动进帧，所以必须可移动构造。左值 awaiter 与标准一样就地使用，
  帧里只记地址：不可移动的 awaiter 可以声明成帧局部再等待，但它所指的对象必须活过
  挂起点——由临时对象得到的左值（`*makeUnique()`）不行，那时用 `CO2_AWAIT_AS(T, e)`
  要一份副本。
- `co_await` 只能是语句或赋值右侧；一行一个挂起点（标签取 `__LINE__`，保证头文件
  里的 inline/模板协程体不违反 ODR）。
- 宏的上限：捕获列表最多 8 个参数；`CO2_BEG` 的局部列表和 `CO2_RETURN` 的表达式里，
  每个顶层逗号（模板实参、花括号初始化里的）都算一个宏实参，总数不超过 64。
- awaitable 表达式里的**临时对象活不过挂起点**，只有 awaiter 本身被放进帧。awaiter
  不能借用表达式里其他临时对象；`Generator` 因此把 yield 的 prvalue 移进帧保存。
- `operator co_await` 拼作 `operator_co_await`（C++20 构建下两种拼写都被查找）。
- 掉出没有 `return_void` 的协程末尾是运行时契约违规，而不是 UB。

## 返回类型

### `Task<T>`

惰性、单消费者、只可移动。等待子 Task 是对称转移（栈深不随链长增长）；子 Task 完成时
对称转移回等待者。子 Task 出错时，等待它的协程以同一个错误结束，逐层向上，到根上才
变回异常——见"错误：按值向上传"。

```cpp
#include <co2/task.hpp>

auto child(int x) CO2_BEG(co2::Task<int>, (x)) { CO2_RETURN(x + 1); }
CO2_END

auto parent() CO2_BEG(co2::Task<int>, (), co2::Task<int> kept; int a{}; int b{};) {
    CO2_AWAIT_SET(a, child(1));   // 右值：Task 被移进等待者的帧，完成后随之销毁
    kept = child(10);
    CO2_AWAIT_SET(b, kept);       // 左值：借用，kept 继续拥有帧
    CO2_RETURN(a + b);
}
CO2_END
```

`Task` 的析构销毁帧。**已启动、未完成**的 Task 挂起在某个操作上，销毁它是契约违规；
未启动或已完成的可随时销毁。

### `Generator<Ref, V = void>`

与 `std::generator` 同形：`value_type`/`reference`/`yielded` 推导规则相同、`begin()`
只能调用一次、体内禁止 `co_await`、`elements_of` 嵌套。

```cpp
#include <co2/generator.hpp>

auto range(int n) CO2_BEG(co2::Generator<int>, (n), int i{};) {
    for (i = 0; i < n; ++i) CO2_YIELD(i);
}
CO2_END

auto tree(int depth) CO2_BEG(co2::Generator<int>, (depth)) {
    if (depth == 0) {
        CO2_YIELD(0);
    } else {
        CO2_YIELD(depth);
        CO2_YIELD(co2::elements_of(tree(depth - 1)));   // 嵌套：栈深不随深度增长
        CO2_YIELD(co2::elements_of(range(depth)));      // 任意范围
    }
}
CO2_END

for (auto value : tree(3)) use(value);
```

`Generator<T>` 的 `reference` 是 `T&&`（可以移走元素）；`Generator<T const&>` yield 左值
不复制；`Generator<Ref, V>` 显式 value type。内层逃出的异常在父层的 `elements_of`
处重抛，最终由 `begin()`/`++` 抛给消费者。中途销毁生成器会沿链销毁所有活动帧
（从最内层往外迭代，栈深同样不随深度增长）。

### `AsyncGenerator<T>`

生产者体内既可 `CO2_AWAIT` 也可 `CO2_YIELD`；消费者：

```cpp
auto consume(co2::AsyncGenerator<int>& stream)
    CO2_BEG(co2::Task<int>, (stream), bool has{}; int total{};) {
    CO2_AWAIT_SET(has, stream.next());
    while (has) {
        total += stream.value();               // 到下一次 next() 之前有效
        CO2_AWAIT_SET(has, stream.next());
    }
    CO2_RETURN(total);
}
CO2_END
```

每一步都是生产者与消费者之间的对称转移，没有同步；消费者在生产者 yield 所在的线程
上恢复。有 `next()` 未完成时销毁是契约违规。

生产者出错或已停止即流结束：正在等待的那次 `next()` 以同样的结果完成，消费者默认也
就此结束（想自己处理就写 `co2::asResult(stream.next())`）。那次失败只交付一次，之后的
`next()` 得到 `false`。

## 错误与已停止：按值向上传

一个 Task 以值、错误或已停止结束。错误是一个 `co2::Error`：要么是一个
`std::error_code`，要么是一个被捕获的异常。已停止表示操作被取消了，没有结果。

```cpp
#include <co2/as_result.hpp>
#include <co2/just.hpp>

auto readHeader(Socket& socket) CO2_BEG(co2::Task<Header>, (socket), Header header;) {
    if (not socket.isOpen())
        CO2_AWAIT(co2::just_error(NetErrc::closed));   // 本协程以这个错误结束
    CO2_AWAIT_SET(header, socket.read());
    CO2_RETURN(header);
}
CO2_END

auto serve(Socket& socket)
    CO2_BEG(co2::Task<>, (socket), Header header;
            co2::optional<co2::Result<Header>> attempt;) {
    CO2_AWAIT_SET(header, readHeader(socket));   // 出错：到此为止，以同一个错误结束
    CO2_AWAIT_SET(attempt, co2::asResult(readHeader(socket)));   // 想自己处理
    if (attempt->hasError()) log(attempt->error().code());
}
CO2_END
```

- **普通的 `CO2_AWAIT` 向上传播。** 被等待的操作出错时，其后的语句不执行，局部按逆序
  析构，当前协程以同一个错误结束并交给等待它的协程。中间各层既不抛也不接——挂起点
  本来就不能放进 `try` 块。
- **协程体里 `throw` 等同于以错误结束。** 异常只被捕获一次，此后按值传。
- **到根上才变回异常。** `syncWait` / `join()` 重抛原来的异常；错误码则抛
  `std::system_error`。不想要异常就写 `co2::syncWait(co2::asResult(task))`，得到
  `co2::optional<co2::Result<T>>`，三种结果都是值。
- **想自己处理就套 `asResult`。** 得到 `co2::Result<T>`：`hasValue()` / `hasError()` /
  `isStopped()`。它不能默认构造，作帧局部时写成 `co2::optional<co2::Result<T>>`。
- **自己的错误枚举**按 `<system_error>` 的常规方式接入（`std::is_error_code_enum` 的
  特化加 `make_error_code`），之后可以直接传给 `just_error`。
  `CO2_YIELD(co2::with_error(e))` 是 P3552 的拼写，效果相同。

已停止走的是同一条路，只是到了根上不抛异常，而是空的 optional：

```cpp
#include <co2/unstoppable.hpp>

CO2_AWAIT(co2::stopIfRequested());                    // 取消点：有停止请求才停，否则继续
CO2_AWAIT(co2::scheduleOn(pool));                     // 也是取消点，见"执行层"
CO2_AWAIT(co2::just_stopped());                       // 本协程以已停止结束
CO2_AWAIT(co2::unstoppable(co2::scheduleOn(pool)));   // 对这一次等待隐藏停止请求
```

- **协程在取消点结束。** 取消是协作式的：停止请求只是一个标志，协程走到取消点才看它。
  结束的方式与出错相同——其后的语句不执行，局部析构，等待它的协程以已停止结束。
- **`unstoppable(x)` 用在收尾阶段。** 已经收到停止请求、但还有几步必须做完时，把那几步
  包起来：被包住的操作（以及它等待的子 Task）看不到停止请求。它只隐藏外面的请求，
  被包住的操作自己出错或自己停止照样传出来。
- **组件都遵守同一套规则。** 等待 `JoinHandle`、`whenAll`、`AsyncGenerator` 的 `next()`、
  `fromCallback`，被等待的一方出错或已停止，等待它的协程就以同样的结果结束；各自的
  细节见对应的小节。

写 awaiter 与 promise 的人各多一个可选的扩展点：

- awaiter 可以提供 `co2::Completion await_completion() noexcept`，报告这次等待是有值、
  出错还是已停止。恢复点先问它，有值才调用 `await_resume()`。没有这个成员的 awaiter
  行为不变。**包装别的 awaiter 的 awaiter 要把它一并转发**：不转发时，被包住的操作
  出错仍然从 `await_resume()` 抛出（与从前相同），以已停止完成则是契约违规。
  `scheduleOn` 与 `stopIfRequested()` 例外：它们的 `await_resume()` 照常返回，不转发
  只是让它们不再是取消点。
- promise 可以提供 `void return_error(co2::Error) noexcept` 与
  `void return_stopped() noexcept` 接收后两种结果。没有 `return_error()` 时错误在
  协程体内重新抛出，走 `unhandled_exception()`；没有 `return_stopped()` 时"已停止"是
  契约违规。

完整的设计见 `docs/design/completion-channels.md`。

## 禁用异常的构建

库可以在 `-fno-exceptions`（通常连同 `-fno-rtti`）下编译和使用，不需要定义任何宏：
`config.hpp` 从编译器的开关得出 `CO2_HAS_EXCEPTIONS`。值、错误码、已停止三种完成方式
在两种构建下是同一份代码，上一节的写法原样适用：失败用 `co2::just_error(code)` 产生，
想自己处理就套 `co2::asResult`。

```cpp
// 根上按值拿结果。asResult 把三种结果都装进 Result，所以 optional 总是有值。
auto const outcome = co2::syncWait(co2::asResult(serve(socket)));
if (outcome->hasError()) log(outcome->error().code());
if (outcome->isStopped()) log("cancelled");
```

与开着异常时不同的地方：

- **错误走到只能靠抛出来报告它的地方，是契约违规。** 这样的地方有三处：根
  （`syncWait(task)` / `join()`）、没有 `return_error()` 的自定义 promise、没有转发
  `await_completion()` 的包装 awaiter。在等待的地方套 `co2::asResult`，错误就是一个值。
- **`co2::Error` 只装错误码。** `Error::exception()` 与 `Continuation::setException`
  不存在，续体剩下值、错误码、已停止三个出口。
- **promise 不需要 `unhandled_exception()`**，与原生编译器在 `-fno-exceptions` 下的
  要求一致。`Generator` 体内不能等待，这种构建里它不会失败。
- **`get_return_object_on_allocation_failure` 只在 promise 自己的 `operator new` 返回空
  时生效。** allocator 与全局 `operator new` 以 `bad_alloc` 报告失败，没有异常时接不住，
  后果由运行时决定（通常是终止）。

**同一个程序的所有翻译单元必须用同一个异常开关编译。** 头文件里的内联函数在两种开关下
是两份不同的定义，混用是静默的 ODR 违规。`CO2_HAS_EXCEPTIONS` 不能手动定义，定义了就
是编译错误；MSVC 上另有一行 `#pragma detect_mismatch` 在链接期检查。

GCC 与 Clang 上的这种构建由 CI 覆盖（见"构建与测试"）。MSVC 关掉异常的构建没有测试过。

## 执行层

### `Scheduler` 与 `scheduleOn`

```cpp
struct Scheduler {
    virtual void schedule(co2::coroutine_handle<> coroutine) noexcept = 0;
};
CO2_AWAIT(co2::scheduleOn(scheduler));   // 转移到 scheduler 上继续
```

队列元素就是一个句柄，一次 hop 稳态零分配。

`scheduleOn` 是取消点：协程在目标 scheduler 上恢复时，它的停止令牌上若已有请求，就以
已停止结束，不再往下执行。检查在 hop 之后，收尾发生在要去的那个线程上。没有停止令牌的
协程不受影响。收到请求之后还要换线程做收尾，写
`co2::unstoppable(co2::scheduleOn(s))`。

### `ThreadPool`

工作窃取线程池：每线程一个固定容量的本地 FIFO 队列 + 互斥全局队列；本地 → 全局 →
随机窃取；工作线程上的 `scheduleOn(pool)` 是真正的让步（排在前面的本地工作先跑）。
窃取一次搬走受害者队列的一半。唤醒是节流的：已经有线程在找活时，新提交的工作不再
叫醒别人；找活的线程停车前先自旋一小段。满载热路径不拿锁。析构排空所有队列后 join。

```cpp
co2::ThreadPool pool{4};                    // 默认 hardware_concurrency()
pool.isWorkerThread();                       // 当前线程是否是它的工作线程
```

### `ManualExecutor`

任意线程 `schedule()`，驱动线程 `runOne()` / `run()`。带着排队工作销毁是契约违规。

### `syncWait`

```cpp
co2::optional<T> result = co2::syncWait(task);         // 阻塞到完成，重抛异常
co2::optional<T> result = co2::syncWait(task, token);  // token 成为 Task 树的根 stop_token
```

Task 在调用线程上内联启动，完成时对称转移到一个不占堆的续体帧唤醒等待线程。与
`std::execution::sync_wait` 一样，它就是阻塞。

返回值也照 `sync_wait`：结果放在 `co2::optional` 里，`Task<void>` 是
`co2::optional<co2::Unit>`。取值写 `*result`；与值比较可以直接写
`co2::syncWait(task) == 42`。空的 optional 表示 Task 以已停止结束；Task 出错时在这里
抛出。结果是 `bool` 的根不要直接当条件用：`if (co2::syncWait(task))` 判断的是"有没有
值"，不是值本身。

### `spawn` / `JoinHandle<T>`

```cpp
auto handle = co2::spawn(pool, work());          // 惰性 Task 直接排给 scheduler
handle.requestStop();                            // 协作式取消
co2::optional<T> value = handle.join();          // 阻塞；返回值与 syncWait 同形
CO2_AWAIT_SET(value, handle);                    // 或在协程里等待（左值借用/右值拥有）
co2::spawn(pool, work(), parentToken);           // 父 token 的停止请求转发过来
```

在协程里等待 `JoinHandle`：被 spawn 的 Task 出错或已停止，等待它的协程以同样的结果
结束。`join()` 对已停止返回空的 optional，出错时抛出。

未完成时析构 `JoinHandle`（与 P3149 `spawn_future` 一致）：请求停止并分离，Task 结束
后帧被销毁，结果丢弃。借用它的等待者还挂着时析构是契约违规。注意停止请求是真的：被
丢弃句柄的 Task 会在它的下一个取消点结束，比如下一次 `scheduleOn`。想让它跑完，就
保留句柄，或者在 Task 里把那些 hop 写成 `co2::unstoppable(co2::scheduleOn(s))`。

### `whenAll`

```cpp
#include <co2/when_all.hpp>

std::tuple<int, std::string> r;
CO2_AWAIT_SET(r, co2::whenAll(count(), name(), flush()));   // Task<int>, Task<std::string>, Task<>
std::vector<int> all;
CO2_AWAIT_SET(all, co2::whenAll(std::move(tasks)));          // std::vector<Task<int>>
```

并发启动一组 Task，全部完成后一起交付结果，语义取自 `std::execution::when_all`：非 void
的结果按参数顺序拼成 tuple（全部 void 则结果是 void），`std::vector<Task<T>>` 给出
`std::vector<T>`。任一子 Task 出错或已停止就向其余子 Task 请求停止，等**全部**结束后：
有出错的，以时间上第一个错误完成；否则有已停止的，以已停止完成。父的 `stop_token` 转发
给每个子 Task。子 Task 在等待者的线程上依次内联启动，要并行就让它们各自
`scheduleOn(pool)`。

### `fromCallback`

```cpp
#include <co2/callback.hpp>

auto asyncRead(Socket& socket, Buffer buffer) CO2_BEG(co2::Task<int>, (socket, buffer), int n{};) {
    CO2_AWAIT_AS_SET(n, co2::CallbackAwaitable<int>,
                     co2::fromCallback<int>([&](co2::Continuation<int> done) {
                         socket.asyncRead(buffer, [done](int bytes) { done(bytes); });
                     }));
    CO2_RETURN(n);
}
CO2_END
```

把回调式异步 API 变成 awaiter：`initiate(continuation)` 或
`initiate(continuation, stop_token)` 发起操作，API 完成时调用续体的四个出口之一，
**恰好一次**：

```cpp
done(args...);              // 有值：就地构造结果
done.setError(code);        // 出错：一个 std::error_code
done.setException(error);   // 出错：一个捕获的异常（禁用异常的构建里没有）
done.setStopped();          // 已停止：操作被取消了
```

后三种让等待它的协程以同样的结果结束。要支持取消，就在 `initiate` 里用收到的
`stop_token` 注册一个 `stop_callback` 去调 API 自己的 cancel，API 随后以"已取消"回调，
在那个回调里调用 `done.setStopped()`。

协程在回调所在的线程上恢复；回调在 `initiate` 返回之前就到达时不挂起。状态全部在
awaiter 里，通常没有堆分配。表达式里有 lambda，所以用 `CO2_AWAIT_AS` 显式给出
awaitable 类型。

## 取消：`stop_token`

`co2::stop_source` / `stop_token` / `stop_callback<Callback = std::function<void()>>`
与 C++20 `[thread.stoptoken]` 逐条一致（`request_stop` 一次性、已请求时注册即执行、
析构阻塞到另一线程上的回调返回、回调可销毁自己）。

token 沿 Task 树传播：`syncWait`/`spawn` 提供根 token，子 Task 从父 promise 继承
（等待者的 `await_suspend` 收到带类型的父句柄，与 `std::execution::task` 查询父环境
的方式相同）。`AsyncGenerator` 的生产者同样在这条链上：每次 `next()` 使用发起它的
消费者的 token。

取消是协作式的：协程在取消点结束（见"错误与已停止"），通常不需要自己读令牌：

```cpp
auto loop(co2::ThreadPool& pool) CO2_BEG(co2::Task<>, (pool)) {
    for (;;) {
        CO2_AWAIT(co2::scheduleOn(pool));   // 取消点：收到停止请求之后在这里结束
        step();
    }
}
CO2_END
```

想自己决定什么时候停、停之前做什么，照旧在体内读令牌。这时循环里的 hop 要用
`unstoppable` 包住，否则协程会先在 hop 那里结束，走不到自己的判断：

```cpp
auto loop(co2::ThreadPool& pool) CO2_BEG(co2::Task<int>, (pool), co2::stop_token token;) {
    CO2_AWAIT_SET(token, co2::getStopToken());
    while (not token.stop_requested())
        CO2_AWAIT(co2::unstoppable(co2::scheduleOn(pool)));
    CO2_RETURN(flush());   // 正常返回一个值
}
CO2_END
```

## 契约

前置条件与不变量违规（销毁挂起中的 Task、恢复正在运行的协程、带着排队工作销毁
executor……）不是可恢复错误，交给进程级契约处理器，默认 `std::terminate()`：

```cpp
co2::setContractViolationHandler([](co2::ContractViolation const& v) {
    log(v.condition, v.file, v.line);
    std::_Exit(86);
});
```

## 头文件

| 头文件 | 内容 |
| --- | --- |
| `co2/co2.hpp` | 全部公共头 |
| `co2/coroutine.hpp` | 核心聚合：`coroutine_handle.hpp` + `dsl.hpp`（+ `detail/awaitable.hpp`、`detail/frame.hpp`） |
| `co2/coroutine_handle.hpp` | `coroutine_handle<P>`、`suspend_always/never`、`noop_coroutine`、`coroutine_traits` |
| `co2/dsl.hpp` | `CO2_*` 宏 |
| `co2/task.hpp` / `generator.hpp` / `async_generator.hpp` | 返回类型 |
| `co2/error.hpp` / `completion.hpp` / `result.hpp` / `optional.hpp` | `Error`、`Completion`、`Result<T>` 与 `Unit`、`optional<T>` |
| `co2/just.hpp` / `as_result.hpp` / `unstoppable.hpp` | `just_error` / `with_error` / `just_stopped` / `stopIfRequested`、`asResult`、`unstoppable` |
| `co2/scheduler.hpp` / `manual_executor.hpp` / `thread_pool.hpp` | 执行层 |
| `co2/sync_wait.hpp` / `spawn.hpp` | 根适配器 |
| `co2/when_all.hpp` / `callback.hpp` | 组合器 `whenAll`、回调适配 `fromCallback` |
| `co2/stop_token.hpp` / `env.hpp` | 取消与协程环境 |
| `co2/contract.hpp` / `config.hpp` | 契约处理器、版本与配置宏 |

每个头都能独立包含。awaiter 默认内联保存在帧内（8 个指针宽度，
`CO2_AWAIT_STORAGE_SIZE`；生成器帧只留 2 个指针宽度），更大的退化为一次堆分配。

## 尚未提供

v2 核心重写后，以下 v1 功能尚未在新模型上重建：事件 / 协程互斥锁 / WorkGroup、
定时器与超时、`whenAny` / `race`、`SharedTask`、`TaskScope`（P3149 `async_scope`
形态）、C++20 `std::coroutine_handle` 互操作层。它们将在标准销毁契约与 `stop_token`
之上重新实现。（`whenAll` 与回调适配已经重建，见上。）

## 升级说明

三种完成方式改动了几处已有的接口与行为。从更早的 `0.3.0-pre` 快照升级时逐条对照：

1. **`syncWait` / `join()` 返回 `co2::optional<T>`**，`void` 是
   `co2::optional<co2::Unit>`，已停止为空。取值写 `*co2::syncWait(task)`。结果是 `bool`
   的根要留意：`if (co2::syncWait(task))` 照样能编译，判断的却是"有没有值"。
2. **`scheduleOn` 是取消点。** 轮询令牌之后正常返回的协程，收到停止请求之后会先在下一次
   `scheduleOn` 结束，走不到自己的判断。要在停止之后继续换线程，写
   `co2::unstoppable(co2::scheduleOn(s))`。
3. **丢弃 `JoinHandle` 的 Task 会在下一个取消点被停掉。** 从前丢弃只是发出停止请求，
   不轮询的 Task 照样跑完。要让它跑完，保留句柄，或者把 Task 里的 hop 用 `unstoppable`
   包住。
4. **`whenAll` 的子 Task 已停止，整个 `whenAll` 就是已停止**（有出错的则以第一个错误
   完成）。从前只有失败影响结果。
5. **根上抛出的异常。** 协程体里抛出的异常原样重抛，与从前一样；以错误码结束的 Task
   抛的是 `std::system_error`。
6. **一个 Task 的错误只能取走一次**，与值相同。同一个失败的 Task 等待两次，第二次是
   契约违规；从前第二次会再抛一遍。
7. **自定义 promise。** 子 Task 失败时，中间各层不再经历 `unhandled_exception()`；
   promise 提供 `return_error()` 就按值收到错误，不提供则错误在体内重新抛出、照旧走
   `unhandled_exception()`。提供了 `get_stop_token()` 的 promise 还要补上
   `return_stopped()`，否则协程在取消点结束时是契约违规。
8. **只转发标准三个成员的包装 awaiter。** 被包住的操作出错时行为不变
   （`await_resume()` 照旧抛出）；以已停止完成则是契约违规，要给包装加上
   `await_completion()` 的转发。
9. **`Continuation::setException` 只在开着异常的构建里存在。**

## 性能基准

`benchmarks/co2-benchmarks`，GCC 13.3 `-O2`（`cmake --preset release-bench`），
i7-13700KF，每项 7 轮取中位数：

| 基准 | 中位 ns/op |
| --- | ---: |
| raw suspend/resume | 1.0 |
| Generator yield/next | 1.5 |
| elements_of 深度 8 yield/next | 1.9 |
| symmetric Task transfer（含帧分配） | 12.0 |
| syncWait ready Task | 26.7 |
| Task 链上的失败，100 层，每层 | 44.4 |
| Task 链上的已停止，100 层，每层 | 20.3 |
| Task 等待子 Task，8 线程各自的 stop_token | 19.3 |
| Task 等待子 Task，8 线程共享一个 stop_token | 18.2 |
| ManualExecutor hop | 22.3 |
| ThreadPool hop（1 worker，1 Task） | 13.4 |
| ThreadPool hop（4 workers，32 Task 聚合） | 3.6 |
| ThreadPool hop（8 workers，1 Task，不饱和） | 15.5 |
| spawn + join（跨线程唤醒） | 2542 |

"Task 链上的失败"在错误改为按值传播之前是 558：那时每一层都要重抛一次异常。
`scheduleOn` 成为取消点之后，每次 hop 多读一次停止状态：`ManualExecutor hop` 从 20.1
变为 22.3，线程池的三项分别从 13.1、3.5、14.5 变为 13.4、3.6、15.5。

线程池的几项对机器负载敏感：同一台机器上有其他负载时，不饱和那一项在 17～30 ns 之间
浮动。数字只在同一台机器、同一份编译选项下可比。

## 构建与测试

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

也可以使用仓库中的 CMake Preset（需要 CMake 3.21 与 Ninja）：`debug-gcc`、
`debug-clang`、`asan`、`tsan`、`no-exceptions`（构建与测试，例如
`cmake --preset debug-gcc && cmake --build --preset debug-gcc &&
ctest --preset debug-gcc`）、`release-bench`（性能基准）。

测试包括规范用例套件（对 [dcl.fct.def.coroutine] / [expr.await] / [coroutine.handle]
逐条）、C++20 构建下与原生 `co_await` 的差分测试、一组通过契约处理器以退出码 86 结束
的契约违规测试、以及必须编译失败的用例（挂起点写进 `switch`、手动定义
`CO2_HAS_EXCEPTIONS`……）。测试默认以 C++14 构建，`-DCO2_TEST_CXX_STANDARD=20` 可以换成
更新的标准；CI 覆盖 14 / 17 / 20 / 23。

`-DCO2_TEST_NO_EXCEPTIONS=ON`（或 `no-exceptions` 这个 Preset）给全部测试加上
`-fno-exceptions -fno-rtti`：依赖 `throw` / `catch` 的用例不参加，差分测试不构建，另外
多出三个"错误无处可抛"的契约违规测试。CI 在 GCC 与 Clang 上各以 Debug、Release 跑
一遍。这个选项只为 GCC 与 Clang 准备。

作为子项目使用时链接仅含头文件的 CMake target：

```cmake
add_subdirectory(path/to/co2)
target_link_libraries(my-target PRIVATE co2::co2)
```

安装后的包可通过以下方式使用：

```cmake
find_package(co2 CONFIG REQUIRED)
target_link_libraries(my-target PRIVATE co2::co2)
```

MSVC 需要 VS 2019 16.5 或更新版本，且协程 DSL 依赖一致预处理器 `/Zc:preprocessor`。
通过 CMake 链接 `co2::co2` 会自动带上该选项；手工配置的工程需自行添加，否则
`preprocessor.hpp` 会以明确的 `#error` 提示。

当前 `0.3.0-pre` 是预览包。CMake 的数字版本请求无法表达 `-pre`，因此该包允许不带
版本的 CONFIG 查找，但会拒绝 `find_package(co2 0.3.0 EXACT)`，不会伪装成正式
`0.3.0`。0.x 稳定包也只承诺精确版本匹配。

构建并运行可选 Release 基准：

```sh
cmake --preset release-bench
cmake --build --preset release-bench
./build/presets/release-bench/benchmarks/co2-benchmarks
```
