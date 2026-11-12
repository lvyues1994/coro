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

返回类型是任何带 `promise_type` 的类型（或特化了 `co2::coroutine_traits<R>`）。

### 与语言级协程的差别

这些差别全部来自"没有编译器"，见 `docs/design/coroutine-core-v2.md` 第 6 节：

- 跨挂起点存活的局部必须写在 `CO2_BEG` 的局部列表里，在帧创建时构造（析构时机与
  标准一致）；挂起点不能出现在 `try` 块、lambda 或嵌套函数里，也不能出现在协程体自己的
  `switch` 语句里（编译错误：状态机的 `case` 标签会绑到最近一层 switch）。
- 协程体里直接声明的变量住在栈上，活不过挂起点。带初始化的声明跨越挂起点是编译错误；
  但**不带初始化的平凡类型**（`int sum;`）编译器不拦，恢复之后读到的是不确定的值——
  体内的声明一律带上初始化，需要跨挂起点的放进局部列表。
- 成员函数可以是协程，但协程体是一个局部类的成员函数，访问不到外层的 `this`：把对象
  作为参数传进来（`static auto run(Self& self, ...)`，体内写 `self.member`）。
- awaiter 会被移动进帧，所以必须可移动构造；左值 awaitable 同样被复制/移动进去，而
  不是像标准那样就地使用。
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
对称转移回等待者；异常在等待者处重抛。

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

## 执行层

### `Scheduler` 与 `scheduleOn`

```cpp
struct Scheduler {
    virtual void schedule(co2::coroutine_handle<> coroutine) noexcept = 0;
};
CO2_AWAIT(co2::scheduleOn(scheduler));   // 转移到 scheduler 上继续
```

队列元素就是一个句柄，一次 hop 稳态零分配。

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
T result = co2::syncWait(task);            // 阻塞到完成，重抛异常
T result = co2::syncWait(task, token);     // token 成为 Task 树的根 stop_token
```

Task 在调用线程上内联启动，完成时对称转移到一个不占堆的续体帧唤醒等待线程。与
`std::execution::sync_wait` 一样，它就是阻塞。

### `spawn` / `JoinHandle<T>`

```cpp
auto handle = co2::spawn(pool, work());          // 惰性 Task 直接排给 scheduler
handle.requestStop();                            // 协作式取消
T value = handle.join();                         // 阻塞
CO2_AWAIT_SET(value, handle);                    // 或在协程里等待（左值借用/右值拥有）
co2::spawn(pool, work(), parentToken);           // 父 token 的停止请求转发过来
```

未完成时析构 `JoinHandle`（与 P3149 `spawn_future` 一致）：请求停止并分离，Task 跑完
后帧被销毁，结果与异常丢弃。借用它的等待者还挂着时析构是契约违规。

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
`std::vector<T>`。任一子 Task 失败就向其余子 Task 请求停止，等**全部**完成后重抛时间上
第一个异常；父的 `stop_token` 转发给每个子 Task。子 Task 在等待者的线程上依次内联启动，
要并行就让它们各自 `scheduleOn(pool)`。

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
`initiate(continuation, stop_token)` 发起操作，API 完成时调用 `continuation(args...)`
（就地构造结果）或 `continuation.setException(error)`，**恰好一次**。协程在回调所在的
线程上恢复；回调在 `initiate` 返回之前就到达时不挂起。状态全部在 awaiter 里，通常没有
堆分配。表达式里有 lambda，所以用 `CO2_AWAIT_AS` 显式给出 awaitable 类型。

## 取消：`stop_token`

`co2::stop_source` / `stop_token` / `stop_callback<Callback = std::function<void()>>`
与 C++20 `[thread.stoptoken]` 逐条一致（`request_stop` 一次性、已请求时注册即执行、
析构阻塞到另一线程上的回调返回、回调可销毁自己）。

token 沿 Task 树传播：`syncWait`/`spawn` 提供根 token，子 Task 从父 promise 继承
（等待者的 `await_suspend` 收到带类型的父句柄，与 `std::execution::task` 查询父环境
的方式相同）。`AsyncGenerator` 的生产者同样在这条链上：每次 `next()` 使用发起它的
消费者的 token。体内读取：

```cpp
auto loop(co2::ThreadPool& pool) CO2_BEG(co2::Task<>, (pool), co2::stop_token token;) {
    CO2_AWAIT_SET(token, co2::getStopToken());
    while (not token.stop_requested()) CO2_AWAIT(co2::scheduleOn(pool));
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

## 性能基准

`benchmarks/co2-benchmarks`，GCC 13.3 `-O2`（`cmake --preset release-bench`），
i7-13700KF，每项 7 轮取中位数：

| 基准 | 中位 ns/op |
| --- | ---: |
| raw suspend/resume | 1.0 |
| Generator yield/next | 1.5 |
| elements_of 深度 8 yield/next | 1.9 |
| symmetric Task transfer（含帧分配） | 11.9 |
| syncWait ready Task | 25.4 |
| Task 等待子 Task，8 线程各自的 stop_token | 19.3 |
| Task 等待子 Task，8 线程共享一个 stop_token | 18.2 |
| ManualExecutor hop | 20.1 |
| ThreadPool hop（1 worker，1 Task） | 13.1 |
| ThreadPool hop（4 workers，32 Task 聚合） | 3.5 |
| ThreadPool hop（8 workers，1 Task，不饱和） | 14.5 |
| spawn + join（跨线程唤醒） | 2542 |

线程池的几项对机器负载敏感：同一台机器上有其他负载时，不饱和那一项在 17～30 ns 之间
浮动。数字只在同一台机器、同一份编译选项下可比。

## 构建与测试

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

也可以使用仓库中的 CMake Preset（需要 CMake 3.21 与 Ninja）：`debug-gcc`、
`debug-clang`、`asan`、`tsan`（构建与测试，例如 `cmake --preset debug-gcc &&
cmake --build --preset debug-gcc && ctest --preset debug-gcc`）、`release-bench`
（性能基准）。

测试包括规范用例套件（对 [dcl.fct.def.coroutine] / [expr.await] / [coroutine.handle]
逐条）、C++20 构建下与原生 `co_await` 的差分测试、一组通过契约处理器以退出码 86 结束
的契约违规测试、以及必须编译失败的用例（挂起点写进 `switch`）。测试默认以 C++14
构建，`-DCO2_TEST_CXX_STANDARD=20` 可以换成更新的标准；CI 覆盖 14 / 17 / 20 / 23。

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
