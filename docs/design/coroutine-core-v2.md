# co2 v2 协程核心设计：以 C++20 协程规范为蓝本的 C++14 宏实现

状态：第 12 节四项决策已确认（标准销毁契约、标准拼写、对齐 `std::generator`、先做
阶段 1）；阶段 1（核心）、阶段 2（`Task`/`Generator`/`AsyncGenerator`）与阶段 3 的大部分
（`Scheduler`/`scheduleOn`/`ThreadPool`/`syncWait`/`stop_token`/`spawn`）已在本分支实现于
本分支，见第 13～16 节；旧核心已整体删除，v2 头文件位于 `include/co2/` 顶层（第 17
节）。本文主体只讨论"无栈协程核心"——帧、句柄、
promise 与 awaiter 协议、以及宏代码生成。Task、Generator、Scheduler 等上层类型在第 9
节以"新核心是否足以承载它们"的角度出现，第 14 节记录其实现。

## 0. 问题

在标准 C++14 中提供无栈协程，使得：

1. 返回类型、promise 类型、awaiter 类型的**协议与 C++20 [dcl.fct.def.coroutine]
   完全一致**——为 co2 写的 promise 与为 `co_await` 写的 promise 是同一份代码，
   任何 C++20 awaiter（cppcoro、asio、libunifex、用户自己的）可以不经适配直接被
   co2 协程等待；
2. 代码生成仍然只用宏（`CO2_BEG` / `CO2_AWAIT` / `CO2_YIELD` / `CO2_RETURN` /
   `CO2_END`），不依赖编译器协程；
3. 与标准的偏离只来自"没有编译器参与"这一个根因，并且全部列成清单。

## 1. 标准是怎么定义无栈协程的

以下是设计必须逐条对应的规范要点（C++20，C++23 无核心变更；`std::generator` 是
C++23 库部分）。

**协程体变换** [dcl.fct.def.coroutine]/5：

```cpp
{
    promise-type promise promise-constructor-arguments;   // 若能用参数副本构造则传入，否则默认构造
    try {
        co_await promise.initial_suspend();
        function-body
    } catch (...) {
        if (!initial-await-resume-called) throw;         // 初始挂起点之前的异常直接抛给调用方
        promise.unhandled_exception();
    }
final-suspend:
    co_await promise.final_suspend();                    // 必须 noexcept
}
```

- 协程状态（帧）含：promise 对象、参数副本（by-value 参数被移动进帧，引用参数按引用
  保存）、跨挂起点存活的局部对象与临时对象。
- `get_return_object()` 在 `initial_suspend` 之前调用；协程第一次挂起或完成时把它
  返回给调用方（允许隐式转换到函数返回类型）。
- 协程状态在两种情况下销毁：控制流掉出协程末尾（即 `final_suspend` 不挂起），或对
  其句柄调用 `destroy()`。因此 `final_suspend` 返回 `suspend_always` 的协程完成后帧
  仍在，owner 可以读取 promise 里的结果再销毁；返回 `suspend_never` 的协程自行销毁。
- `co_return e;` → `promise.return_value(e)`；`co_return;` 或掉出末尾 →
  `promise.return_void()`（掉出末尾且没有 `return_void` 是 UB）。
- `co_yield e` ≡ `co_await promise.yield_value(e)`。
- `unhandled_exception()` 若再抛出，协程被视为停在 final suspend point，异常从
  `resume()` 传出。
- 帧的分配：若 promise 有 `operator new`，用它（参数副本作为额外实参，若有匹配重载）；
  若有静态 `get_return_object_on_allocation_failure()`，分配失败时返回它而不抛。
- 分配来源约定：`std::allocator_arg_t` 首参数 + allocator 次参数（`std::generator`
  与 P2300 采用同一约定）。

**`co_await` 表达式** [expr.await]：

1. 若 promise 有 `await_transform`，`a = promise.await_transform(e)`，否则 `a = e`；
2. 若存在 `operator co_await`（先成员，再 ADL 非成员），awaiter 是其返回值，否则
   awaiter 就是 `a`；awaiter 临时对象的生命周期覆盖整次挂起；
3. `await_ready()` 为假则调用 `await_suspend(handle)`，其返回类型决定行为：
   `void` → 挂起；`bool` → `false` 时立即恢复（不算挂起）；`coroutine_handle<Z>`
   → 挂起并恢复该句柄（对称转移）；
4. 一旦调用 `await_suspend`，协程即视为挂起：**另一线程可以在 `await_suspend` 返回
   之前就 `resume()` 它**，因此 awaiter 在把句柄交出后不得再访问自身或帧；
5. `await_suspend` 抛出 → 协程视为恢复，异常在 `co_await` 处抛出（落入协程体的
   catch）；
6. 恢复后 `await_resume()` 的返回值是表达式的值。

**`coroutine_handle`** [coroutine.handle]：非拥有；`resume()`/`operator()`、
`destroy()`、`done()`、`promise()`、`from_promise()`、`address()`/`from_address()`、
`explicit operator bool`、到 `coroutine_handle<>` 的隐式转换。`resume()` 前置条件是
协程挂起且未 done；对 final suspend 处的协程只能 `destroy()`。`destroy()` 销毁
作用域内的局部对象、promise、参数副本并释放帧。`noop_coroutine()` 的 `resume()`
无操作、`done()` 恒假。**标准不为句柄提供任何线程安全**：同一协程上 `resume` 与
`destroy` 的竞争是使用者的责任。

**`std::suspend_always` / `std::suspend_never`**：三个平凡成员函数。

**C++23 `std::generator<Ref, V, Alloc>`**：promise 提供 `yield_value(Ref)`、
`yield_value(elements_of<...>)`（嵌套展开，深度递归以迭代方式恢复最内层）、
`final_suspend` 返回 `suspend_always`、`unhandled_exception` 保存 `exception_ptr`、
以 `allocator_arg` 约定分配；返回类型是 input range，`begin()` 只能调用一次。

## 2. 映射表

| 标准构造 | co2 v2 | 一致程度 |
| --- | --- | --- |
| `promise_type` 嵌套类型 / `std::coroutine_traits` | `Return::promise_type` / `co2::coroutine_traits<Return>` | 一致（traits 只能按返回类型特化，见偏离 D4） |
| `get_return_object` / `initial_suspend` / `final_suspend` / `return_value` / `return_void` / `yield_value` / `unhandled_exception` / `await_transform` | 同名、同签名、同调用时机 | 一致 |
| promise 构造：优先用参数副本 | `std::is_constructible<Promise, Params&...>` 分派 | 一致 |
| `operator new` / `get_return_object_on_allocation_failure` / `allocator_arg` | 同名协议；宏额外提供 `CO2_BEG_WITH_ALLOCATOR` | 一致 |
| `std::coroutine_handle<P>` | `co2::coroutine_handle<P>`，同成员集 | 一致（`resume()` 内部是 trampoline 循环，见 D2） |
| awaiter：`await_ready` / `await_suspend(handle)` 三种返回 / `await_resume` | 完全一致，`await_suspend` 参数是 `co2::coroutine_handle<P>` | 一致 |
| `operator co_await`（成员 / ADL） | `CO2_AWAIT` 展开时同序查找 | 一致 |
| `std::suspend_always` / `suspend_never` / `noop_coroutine` | `co2::suspend_always` / `suspend_never` / `noop_coroutine` | 一致 |
| `co_await e` / `co_yield e` / `co_return e` | `CO2_AWAIT(e)` `CO2_AWAIT_SET(v, e)` / `CO2_YIELD(e)` / `CO2_RETURN(e)` | 语义一致、形式不同（D1、D3） |
| 跨挂起点的局部对象 | `CO2_BEG` 局部列表（帧成员） | 有意偏离（D1、D5） |
| 参数自动进入帧 | 显式捕获列表 `(a, b)` | 有意偏离（D3） |
| `resume()` 的尾调用对称转移 | 迭代 trampoline | 观察上一致（D2） |
| 句柄无线程安全 | 同 | 一致 |

## 3. 组件

五个盒子，比现在少两个（`ResumeOperation`/恢复门 与 `FrameExecution` 原子状态机
在标准模型下没有对应物，见第 7、8 节）。

1. **`coroutine_handle<P>`**——对帧头的非拥有视图。责任：`resume` 的 trampoline、
   `destroy`、`done`、`promise`/`from_promise` 的地址计算。它是核心里唯一的公开
   运行时类型。
2. **帧（`detail::CoroutineFrame<Promise, Body, Allocator>`）**——一次分配，标准布局
   前缀 `{header, promise}` 之后跟随 body（参数副本、局部、程序计数器、awaiter 槽）
   与 allocator。责任：执行第 1 节的协程体变换（initial/final suspend、
   `unhandled_exception`、`return_void` 补全）、分配与销毁协议。
3. **ramp（`detail::startCoroutine`）**——`CO2_END` 生成的函数尾：分配帧、构造
   promise、`get_return_object`、等待 `initial_suspend`、决定是返回还是立即运行。
4. **`co_await` 展开（`detail::awaitTransform` / `getAwaiter` / `suspendWith`）**——
   把 [expr.await] 的六步变成 `CO2_AWAIT` 里的直线代码：`await_transform` →
   `operator co_await` → `await_ready` → 按 `await_suspend` 返回类型分派 →
   `await_resume`。awaiter 存在帧内的小缓冲槽（超容量退化为堆）。
5. **宏 DSL**——`CO2_BEG`/`CO2_END` 生成 Body 与 ramp；`CO2_AWAIT`/`CO2_YIELD`/
   `CO2_RETURN` 生成挂起点。标签取 `__LINE__`。

为什么不是更少：句柄与帧必须分开（非拥有视图 vs 存储）；ramp 与 body 的异常规则
不同（初始挂起前的异常抛给调用方，之后的进 `unhandled_exception`），拆开才能各自
写对；`co_await` 展开是独立的查找规则集合，和帧无关。为什么不是更多：不再有
"外部恢复注册"组件——标准把跨线程恢复的正确性完全交给 awaiter 与句柄的使用规则
（第 5.3 节），核心不需要为此持有任何状态。

## 4. 契约

### 4.1 `coroutine_handle`

```cpp
namespace co2 {

namespace detail {
// 帧头。前两项与 Itanium 协程帧 ABI 同形（恢复、销毁），第三项是 trampoline 需要的
// 单步函数：运行到下一个挂起点，返回对称转移目标或空。
struct FrameHeader {
    void (*destroy)(FrameHeader*) noexcept;
    FrameHeader* (*step)(FrameHeader*);   // 可能抛出：unhandled_exception 再抛出时
    bool done;                            // 停在 final suspend point
};
}

template <class Promise = void> struct coroutine_handle;

template <> struct coroutine_handle<void> {
    constexpr coroutine_handle() noexcept = default;
    constexpr coroutine_handle(std::nullptr_t) noexcept {}

    void* address() const noexcept;
    static coroutine_handle from_address(void* address) noexcept;

    explicit operator bool() const noexcept;
    bool done() const noexcept;          // 前置条件：非空

    void operator()() const { resume(); }
    void resume() const;                 // 前置条件：挂起且 !done()；trampoline 见 5.2
    void destroy() const noexcept;       // 前置条件：挂起

  protected:
    detail::FrameHeader* frame{};
};

template <class Promise> struct coroutine_handle : coroutine_handle<void> {
    Promise& promise() const noexcept;
    static coroutine_handle from_promise(Promise& promise) noexcept;
    static coroutine_handle from_address(void* address) noexcept;
};

struct suspend_always {
    bool await_ready() const noexcept { return false; }
    void await_suspend(coroutine_handle<>) const noexcept {}
    void await_resume() const noexcept {}
};
struct suspend_never { /* await_ready 返回 true，其余同上 */ };

coroutine_handle<> noop_coroutine() noexcept;   // step 返回空，done 恒假

// 只按返回类型解析 promise；默认取 Return::promise_type，允许特化。
template <class Return, class = void> struct coroutine_traits {
    using promise_type = typename Return::promise_type;
};

} // namespace co2
```

这些名字采用标准拼写（`coroutine_handle`、`suspend_always`、`noop_coroutine`），
因为它们**就是**标准词汇：目标是让针对 `std::` 写的代码只改命名空间前缀就能落到
co2 上。库自己的类型（`Task`、`Generator`）继续用 CamelCase。

### 4.2 帧布局与地址计算

```cpp
namespace co2 { namespace detail {

// 标准布局前缀：header 在偏移 0，promise 存储紧随其后。from_promise 与 promise()
// 的地址计算只依赖这个前缀，因此对任意 Promise 都是良定义的。
template <class Promise> struct FramePrefix {
    FrameHeader header;
    alignas(Promise) unsigned char promise[sizeof(Promise)];
};

template <class Promise, class Body, class Allocator> struct CoroutineFrame {
    FramePrefix<Promise> prefix;                       // 必须是第一个成员
    alignas(Body) unsigned char body[sizeof(Body)];    // 参数副本 + 局部 + pc + awaiter 槽
    alignas(Allocator) unsigned char allocator[sizeof(Allocator)];
    bool initialAwaitResumed;
};
static_assert(std::is_standard_layout<CoroutineFrame<P, B, A>>::value, "...");

}}
```

所有成员都是标准布局类型（字节数组、一个 POD header、一个 bool），因此
`CoroutineFrame` 本身是标准布局：`FrameHeader*` 与 `CoroutineFrame*` 指针可互转
（header 是递归首成员），`offsetof(FramePrefix<P>, promise)` 良定义。这就是
`from_promise` 不需要任何 ABI 假设的原因：

```cpp
template <class P> coroutine_handle<P> coroutine_handle<P>::from_promise(P& p) noexcept {
    auto* const bytes = reinterpret_cast<unsigned char*>(&p) - offsetof(detail::FramePrefix<P>, promise);
    return from_address(bytes);   // FramePrefix 首成员是 header，地址相同
}
```

### 4.3 Promise 概念（与标准逐字一致）

一个类型 `Return` 可以作为 `CO2_BEG` 的返回类型，当且仅当
`coroutine_traits<Return>::promise_type` 满足：

```cpp
struct promise_type {
    // 必需
    Return   get_return_object();                 // 或任何可隐式转换到 Return 的类型
    Awaitable initial_suspend();
    Awaitable final_suspend() noexcept;
    void      unhandled_exception();
    // 二选一（同时存在是错误）
    void return_void();
    void return_value(T);
    // 可选
    Awaitable yield_value(...);
    Awaitable await_transform(...);
    static void* operator new(std::size_t, ...);  static void operator delete(void*, std::size_t);
    static Return get_return_object_on_allocation_failure();
};
```

库对这些成员**只做标准要求的调用**，不追加任何非标准成员（现在的
`onCompleted` / `hardenContinuation` / `setCancelled` 都消失）。

### 4.4 Awaiter 概念（与标准逐字一致）

```cpp
bool await_ready();
void | bool | coroutine_handle<Z> await_suspend(coroutine_handle<P>);
R await_resume();
```

以及 `operator co_await`（成员或 ADL 非成员）。因为 `co2::coroutine_handle<P>` 是
一个具体类型而不是 `std::coroutine_handle`，为 `std::` 写的 awaiter 需要
`await_suspend` 是模板或接受 `co2::coroutine_handle<>`；第 9.4 节的
`compat/cpp20.hpp` 负责把接受 `std::coroutine_handle<>` 的 awaiter 包一层。

### 4.5 宏展开

```cpp
auto count(int& value) CO2_BEG(Counter, (value), int local{};) {
    ...
    CO2_AWAIT(co2::suspend_always{});
    ...
} CO2_END
```

展开（示意，省略预处理细节）：

```cpp
auto count(int& value) -> Counter {
    using Promise = co2::coroutine_traits<Counter>::promise_type;
    struct Body {
        int& value;                       // 捕获：by-value 参数被移动进来，引用保持引用
        int local{};                      // 局部列表
        unsigned pc{};
        co2::detail::AwaitSlot<> slot;

        // 运行到下一个挂起点。返回值：空 = 挂起或到达 final suspend；否则对称转移目标。
        co2::detail::FrameHeader* operator()(co2::detail::BodyContext<Promise>& ctx) {
            switch (pc) {
            case 0:
                ctx.finishInitialSuspend();     // initial awaiter 的 await_resume()；成功后置 initialAwaitResumed
                ...用户代码...
                // 掉出末尾：
                ctx.returnVoidIfPresent();      // 没有 return_void 时是编译错误（标准是 UB，这里更严）
                return ctx.enterFinalSuspend();
            }
            CO2_CONTRACT_FAIL("resumed at an invalid suspend point");
        }
    };
    return co2::detail::startCoroutine<Counter, Body>(
        Body{std::forward<decltype(value)>(value)}, /*allocator*/ co2::detail::DefaultAllocator{});
}
```

`CO2_AWAIT(e)` 在 Body 内展开为：

```cpp
do {
    pc = __LINE__ + 1;
    auto& awaiter = slot.emplace(co2::detail::getAwaiter(ctx.awaitTransform((e))));
    if (!awaiter.await_ready()) {
        auto const outcome = co2::detail::suspendWith(awaiter, ctx.handle());   // 分派三种返回类型
        if (outcome.suspended) return outcome.next;                               // 空或转移目标
    }
case __LINE__ + 1:
    /*resume(*/ awaiter.await_resume() /*)*/;
    slot.reset();
} while (false)
```

`CO2_YIELD(e)` ≡ `CO2_AWAIT(ctx.promise().yield_value(e))`；`CO2_RETURN(e)` ≡
`ctx.promise().return_value(e); return ctx.enterFinalSuspend();`；`CO2_RETURN()` ≡
`return_void()` 同理。

帧的 `step` 函数把 Body 调用包在标准要求的 try/catch 中：

```cpp
static FrameHeader* step(FrameHeader* header) {
    auto& frame = *reinterpret_cast<CoroutineFrame*>(header);
    try {
        return frame.bodyRef()(frame.context());
    } catch (...) {
        frame.slot().reset();                       // 销毁等待中的 awaiter 临时对象
        if (!frame.initialAwaitResumed) {           // 初始挂起点之前（含 initial awaiter 的 await_resume）：
            destroyFrame(frame);                    // 销毁协程状态，异常抛给调用方/恢复方
            throw;
        }
        frame.header().done = true;                 // 若 unhandled_exception 再抛出，协程停在 final suspend point
        frame.promise().unhandled_exception();
        frame.header().done = false;
        return frame.context().enterFinalSuspend(); // co_await final_suspend()
    }
}
```

`enterFinalSuspend()` 等待 `final_suspend()`：挂起时置 `done = true`，帧保留到 owner
`destroy()`（`Task`、`Generator` 都靠这一点在完成后读取 promise 里的结果）；
`await_ready` 为真或 `await_suspend` 返回 `false` 时**控制流掉出协程末尾，协程状态被
自动销毁**（局部、promise、参数副本析构，帧释放），此后任何句柄都是悬空的——这正是
标准对 `suspend_never` 式 final suspend 的规定，也是自销毁的 fire-and-forget 协程的
实现方式；返回句柄时对称转移。`final_suspend()` 是 `noexcept`，这里不需要 catch。

### 4.6 ramp

```cpp
template <class Return, class Body, class Allocator>
Return startCoroutine(Body body, Allocator allocator) {
    using Promise = typename coroutine_traits<Return>::promise_type;
    auto* frame = allocateFrame<Promise, Body>(allocator, body);   // operator new / allocator / on_allocation_failure
    if (frame == nullptr) return Promise::get_return_object_on_allocation_failure();
    constructPromise(frame, body);          // 能用参数副本构造就传参数副本
    auto guard = destroyOnException(frame); // 初始挂起点之前的异常：销毁帧、抛给调用方
    auto result = Return(frame->promise().get_return_object());
    auto& initial = frame->slot().emplace(frame->promise().initial_suspend());
    if (initial.await_ready() || !suspendWith(initial, frame->handle()).suspended) {
        guard.release();
        frame->handle().resume();           // 不挂起：在 ramp 里直接进入 case 0（先 await_resume，再跑用户代码）
    } else {
        guard.release();                    // 挂起：await_resume 留到第一次 resume() 时执行
    }
    return result;
}
```

`initial_suspend()` 的 awaiter 保存在帧的 awaiter 槽中；`case 0` 的第一条语句是它的
`await_resume()`，成功后置 `initialAwaitResumed`。这样"初始挂起点之前的异常"
（含 `await_resume` 抛出）无论发生在 ramp 还是第一次 `resume()`，都走 `step` 的
"销毁帧并重抛"分支，与标准一致。

## 5. 数据与状态

### 5.1 帧内布局

```text
+---------------------------------------------+ <- coroutine_handle::address()
| FrameHeader { destroy, step, done, running }|   标准布局前缀
| Promise                                     |
+---------------------------------------------+
| Params { 参数副本... }                        |   宏生成
| Body   { 参数引用..., 局部... }                |   宏生成
| AwaitSlot（内联 8 指针，超出退化为堆）          |
| unsigned suspendPoint                       |
| bool initialAwaitResumed, bodyAlive         |
+---------------------------------------------+
| Allocator                                   |   rebind 后的分配器副本
+---------------------------------------------+
```

一个协程一次分配；awaiter 内联在 `AwaitSlot`（默认 8 指针，超出退化为堆）；
没有原子量、没有 `shared_ptr`。`Generator<int>` 的帧从当前的约 170 字节回到
"header + promise + body"的最小值。

### 5.2 生命周期

```text
ramp: 分配 → 构造 promise → get_return_object → co_await initial_suspend
          │ 挂起                         │ 不挂起
          v                              v
      Suspended  <──────────────────  Running ──── co_await 挂起 ──> Suspended
          │ resume()                     │
          │                              │ co_return / 掉出末尾 / unhandled_exception
          v                              v
      Running                                                       co_await final_suspend
                                          │ 挂起（done = true）        │ 不挂起：掉出协程末尾
                                          v                            v
                                  Done（只能 destroy）            协程状态自动销毁
                                          │ owner destroy()             │
                                          v                            v
                                       Destroyed：局部、awaiter 槽、promise、参数副本析构，释放帧
```

`resume()` 的 trampoline：

```cpp
void coroutine_handle<>::resume() const {
    auto* current = frame;
    while (current != nullptr) current = current->step(current);   // 对称转移目标继续跑，栈深恒定
}
```

`noop_coroutine()` 的 `step` 返回空，因此 `await_suspend` 返回 `noop_coroutine()`
等价于"挂起并回到 resumer"，与标准一致。

### 5.3 线程模型

与标准相同：**核心没有任何同步**。

- 同一协程上，`resume()`、`destroy()` 彼此之间以及与"该协程正在运行"之间不得竞争；
- `await_suspend` 一经调用协程即视为挂起，另一线程可以立即 `resume()`；因此生成的
  代码在调用 `await_suspend` 之前就写好 `pc`，之后不再触碰帧；awaiter 作者遵守
  "交出句柄后不再访问 `this`"；
- 跨线程完成的正确性由 awaiter 与上层（Task、Scheduler）负责——这与 cppcoro、
  libunifex、P2300 的分工一致。

由此推出本设计最重要的**契约变化**（第 12 节决策 1）：一个挂起在外部操作上的协程
**不能被 `destroy()`**，直到该操作完成——这正是标准的规则（P2300 把它表述为
"operation state 只能在完成后销毁"）。取消是协作式的：请求 stop → 操作提前完成
（以取消结果恢复协程）→ 协程走到 final suspend → owner 销毁。现库"任何时刻可放弃"
的保证在标准模型下没有对应物；它是当前核心一半复杂度的来源（恢复门、租约、
harden、trampoline 账本、每帧原子门闩、每次异步等待 1～2 次分配）。

## 6. 与标准的有意偏离（全部来自"没有编译器"）

- **D1 局部对象**：跨挂起点存活的局部必须写在 `CO2_BEG` 的局部列表里，成为帧成员，
  **在帧创建时构造**（标准是在声明点构造）。析构时机与标准一致：离开协程体作用域
  时——`co_return`、掉出末尾、异常逃出——先于 `final_suspend`；挂起中被 `destroy()`
  时在 promise 之前。协程体内用 `auto` 声明的变量不能跨越挂起点（`case` 标签不能
  跨过初始化，这是编译错误而非静默错误）。挂起点不能出现在 `try` 块、lambda 或嵌套
  函数内。
- **D2 对称转移**：标准靠尾调用；这里靠 `resume()` 内的迭代循环。可观察行为相同
  （栈深不随转移链增长），差别是 `await_suspend` 返回的句柄由**当前 resumer 的**
  循环执行，而不是被调函数尾跳转——对 awaiter 作者没有影响。
- **D3 语法**：`co_await` 只能作为语句（`CO2_AWAIT(e)`）或赋值右侧
  （`CO2_AWAIT_SET(v, e)`）出现，不能嵌在任意表达式里；参数需显式列出；一行一个
  挂起点。`CO2_AWAIT` 对表达式求值一次，但在未求值的 `decltype` 里再出现一次，因此
  表达式里不能有 lambda（`CO2_AWAIT_AS(Type, e)` 是逃生口）。
- **D4 `coroutine_traits`**：标准按 `(Return, Args...)` 特化；宏拿不到完整参数类型
  列表，只按 `Return` 特化。
- **D5 掉出末尾**：没有 `return_void` 时标准是 UB，这里是运行时契约违规（生成的代码
  总含有掉出末尾的路径，无法在编译期判定可达性）。
- **D6 `initial_suspend` 的 awaiter**：标准里它是临时对象；这里与其他 awaiter 一样放在
  帧的 awaiter 槽中，生命周期覆盖到恢复后的 `await_resume`——可观察行为相同。
- **D7 `operator co_await` 的拼写**：C++14 没有 `co_await` 关键字，成员/非成员
  `operator co_await` 以 `operator_co_await` 拼写；C++20 构建下两种拼写都被查找，
  为 `std::` 写的 awaitable 无需改动。
- **D8 lvalue awaitable**：`co_await lvalue` 在标准里直接使用该对象；这里 awaiter 会被
  复制/移动进帧的 awaiter 槽（move-only 的 awaitable 需要 `std::move`）。
- **D9 宽松之处**：`await_transform` 只对能匹配的重载生效，匹配失败时原样等待
  （标准：只要 promise 有 `await_transform`，不匹配就是编译错误）；`get_return_object`
  的结果允许显式转换到返回类型；带协程参数的 `operator new(size, params...)` 重载
  暂不支持，只调用 `operator new(size)`。
- **D10 awaitable 表达式里的临时对象**：标准里 `co_await e` 整个全表达式的临时对象
  活到恢复之后（编译器把它们放进帧）；这里 `e` 在生成的 `if` 条件里求值，临时对象在
  调用 `await_suspend` 之前就已销毁，**只有 awaiter 本身**被放进帧。因此 awaiter 不能
  借用表达式里其他临时对象（例如 `CO2_AWAIT(op(std::string{"x"}))` 里 awaiter 存
  `string_view`）。库自身受此影响的地方都改为由 awaiter 拥有：右值 `Task` 直接作为
  awaiter 移进槽里（`elements_of` 的嵌套生成器同理）。
- **D11 yield 的 prvalue**：`std::generator` 的 `yield_value(T&&)` 只存指针（临时对象
  由帧保活）；这里由 D10 可知临时对象活不过挂起点，所以 `Generator<T>` /
  `AsyncGenerator<T>` 把 prvalue **移进帧内的存储**，恢复时销毁——多一次移动，析构
  时机与标准相同。`Generator<T&>` / `Generator<T const&>` yield 左值时仍只记地址。

除此之外的任何差异都应视为缺陷。

## 7. 被否决的替代方案

- **保留 `FrameContext&` 作为 `await_suspend` 参数**。它让 awaiter 无法与任何 C++20
  代码互通，也让 promise 协议无法与标准对齐——这是现设计与标准"有出入"的根源。
- **虚函数表的 `Frame` 基类**（现设计）。句柄需要一个非拥有、可 `from_address`、
  可 `from_promise` 的视图，标准布局前缀 + 函数指针是最小且可证明良定义的形式；
  vtable 会让帧不再是标准布局，`from_promise` 只能靠 ABI 假设。
- **与 Itanium 协程帧 ABI 逐字节兼容**（前两个字是 `resume`/`destroy`，promise 紧随），
  从而在 C++20 构建中让 `std::coroutine_handle::from_address` 直接驱动 co2 帧。诱人，
  但依赖 libstdc++/libc++ 的内部实现（`done()` 靠 `resume == nullptr`），MSVC 不适用，
  且 trampoline 需要第三个函数指针，会把 promise 偏移推到 24。作为可选实验保留，不
  作为设计约束。
- **递归 `resume()` 实现对称转移**。同步 Task 链会消耗本机栈，标准之所以要求尾调用
  正是为了避免这一点。
- **CPS / 嵌套 lambda 代码生成**代替 switch 状态机。每个挂起点一个闭包，局部变量要
  在闭包间传递，分配更多且难以保持单次分配；switch 是 C++14 里唯一保持"一帧一分配"
  的方案。
- **保留"任何时刻可放弃"的安全保证**（恢复门 + 租约）。见第 8 节的代价；标准模型
  下它只能作为可选的上层适配器存在，而不是核心契约。
- **每帧原子执行门闩**。标准帧没有；有了协作式取消与"完成后才销毁"的契约，唯一
  真正的并发点（`await_suspend` 交出句柄后被另一线程恢复）不需要帧内状态。

## 8. 对现有实现的评判

以第 1～5 节为尺子。

**做错的（与标准有出入的根源）**

- awaiter 协议接收 `FrameContext&`，promise 协议是一组自定义成员
  （`returnValue` / `finishWithoutReturn` / `setCancelled` / `onCompleted` /
  `hardenContinuation`），没有 `initial_suspend` / `final_suspend` /
  `await_transform` / `unhandled_exception`。结果是：返回类型只能由库内的
  `ReturnFactory` 特化提供，用户不能像 C++20 那样自定义返回类型；任何 C++20 awaiter
  都要适配。
- 取消被烙进了代码生成：生成的 catch 特判 `TaskCancelled`，promise 有 `setCancelled`。
  标准协程对取消一无所知，取消属于上层（stop token）。
- 帧所有权是 `Coroutine` owner + `abandon()`，承诺"任意时刻可销毁"。为兑现它引入了
  `ResumeOperation`/`ResumeGateState`/`ResumeLease`/`ResumeTrampoline`/`FrameExecution`
  五个组件、每个并发帧约 64 字节控制块与两次 acq_rel CAS、每次异步等待 1～2 次堆
  分配、以及 harden-on-unwind 这类只有作者才能维护的协议。这些都是**一条非标准契约
  的后果**，不是无栈协程本身的复杂度。
- `Coroutine`（owner）与 `CoroutineHandle`（借用）的分工是自创的；标准只有非拥有句柄，
  所有权由返回类型决定。

**做对的（应当保留，因为按需求推导也会得到它们）**

- 一帧一次分配、promise 内嵌于帧、awaiter 内联在帧内（SBO）。
- 迭代驱动器实现对称转移——D2 在宏世界是必需的。
- 惰性 Task、`__LINE__` 标签、allocator 感知、`std::stop_token` 形态的取消、结构化
  组合器（收敛所有败者后再完成）。

**承重 vs 偶然**

承重：switch 状态机、帧内 promise、迭代 trampoline。偶然：整套外部恢复注册机制
——去掉那条契约，它整块消失，而且带走 `Frame` 的 vtable、`FrameExecution`、
`ExternalResumeSlot`、`TerminationSlot`、`SingleThreaded/Concurrent` 策略拆分（新核心
里所有帧天然就是"单线程帧"）。

## 9. 上层在新核心上的形态（验证核心的充分性）

只勾勒，不定稿。

### 9.1 `Task<T>`

cppcoro `task` 的 promise，一字不改：

```cpp
struct promise_type {
    suspend_always initial_suspend() noexcept;                 // 惰性
    FinalAwaiter   final_suspend() noexcept;                   // await_suspend 返回 continuation：对称转移回等待者
    Task get_return_object();
    void return_value(T); void unhandled_exception();          // 结果 / exception_ptr 存在 promise 里
    coroutine_handle<> continuation;
};
// Task 的 awaiter：await_suspend(h) { promise.continuation = h; return self; }  // 对称转移启动 child
```

惰性启动 + 在 `await_suspend` 内设置 continuation 再转移，使"完成 vs 挂起"没有竞争，
Task 不需要任何原子量。`Task` 析构 `destroy()` 帧；前置条件是帧未启动或已 done。

### 9.2 `Generator<T>`

C++23 `std::generator` 的 promise：`yield_value(T&&)` 保存指针、`elements_of` 嵌套时
新帧接管 `yield`、`final_suspend` 返回 `suspend_always`、`unhandled_exception` 存
`exception_ptr` 在 `begin()`/`++` 时重抛。递归生成器不再是独立类型。

### 9.3 执行层

- `Scheduler::schedule(coroutine_handle<>)`——队列元素是一个指针，**零分配**；
  `scheduleOn(s)` 的 awaiter：`await_suspend(h) { s.schedule(h); }`。
- `StopSource/StopToken/StopCallback`——现有 `cancellation.hpp` 的形态，改名对齐标准。
- `syncWait`——run loop（现有 `SyncWaitLoop`），队列元素改为句柄。
- Event/Mutex/Timer——awaiter 内侵入式节点保存 `coroutine_handle<>`；取消通过
  stop token：awaiter 注册 stop callback，被请求时把自己从队列摘除并以取消结果恢复
  协程。**awaiter 在完成前不会被销毁**（契约），所以不再需要控制块。
- `spawn` 返回的 handle 必须被 join；`TaskScope` 析构前必须 join 完（P3149
  `async_scope` 的规则）。`race`/`withTimeout` 向败者请求 stop 并等待其完成——这和
  现在"收敛所有败者"的做法相同，只是不再依赖硬放弃。

### 9.4 与 `std::coroutine_handle` 的互通

C++20 构建下，`compat/cpp20.hpp` 提供：把接受 `std::coroutine_handle<>` 的 awaiter
包成接受 `co2::coroutine_handle<>` 的 awaiter（借一个自动销毁的标准协程桥转发
`resume`），以及反方向让标准协程 `co_await co2::Task`。协议一致后这两个适配器都只有
十几行。

## 10. 验证策略

1. **规范用例套件**：把 [dcl.fct.def.coroutine]、[expr.await]、[coroutine.handle] 的
   每条规则写成一个测试：promise 构造参数传递、`get_return_object` 时机、初始挂起前
   异常抛给调用方、`unhandled_exception` 再抛出停在 final suspend、`await_suspend`
   三种返回类型、`await_suspend` 抛出落入协程体、`await_transform`、`operator co_await`
   成员/ADL 优先级、分配失败返回 `get_return_object_on_allocation_failure`、
   `destroy()` 析构顺序、`noop_coroutine`、`from_promise` 往返。
2. **差分测试**：同一份 promise 类型与同一组场景，分别用 `CO2_BEG` 宏（C++14 构建）和
   真正的 `co_await`（C++20 构建）实现，比对事件序列。因为协议一致，promise 与
   awaiter 代码可以字面共享；只有协程体两份。这是"与标准一致"最有说服力的证据。
3. **成本基准**：`Generator` 的 `next()`、同步 Task 链、`scheduleOn` hop 的分配次数
   （目标：0）与 ns/op，和现库对照。
4. sanitizers 与现有的契约违规测试模式沿用。

## 11. 实施计划

- 阶段 1（已完成，第 13 节）：核心——`coroutine_handle`、帧、ramp、`CO2_*` 宏、
  `suspend_*`、`noop_coroutine`、`coroutine_traits`、规范用例套件、差分测试骨架。新
  代码放在 `include/co2/core/`，旧核心暂不删除，两者互不包含。
- 阶段 2（已完成，第 14 节）：`Task<T>`、`Generator<Ref, V>`（含 `elements_of`）、
  `AsyncGenerator<T>`。
- 阶段 3：执行层——`Scheduler(handle)`、`scheduleOn`、`syncWait`、`ManualExecutor`、
  工作窃取 `ThreadPool`（已完成，第 15 节）；`stop_token`、协程环境、`spawn`（已完成，
  第 16 节）；Event/Mutex/WorkGroup、timer（待做）。
- 阶段 4：结构化并发——`whenAll`/`whenAny`/`race`/`withTimeout`/`TaskScope`（改为
  必须 join）。
- 阶段 5（已提前完成，第 17 节）：删除旧核心与 `ResumeOperation` 层，文档与
  CHANGELOG 收口。

每个阶段独立可编译、可测试；阶段 1 结束即可评审"与标准一致"的核心结论。

## 12. 已确认的决策

1. **销毁契约**：采用标准规则——挂起在外部操作上的协程不能 `destroy()`，取消一律
   协作式（stop token），组合器等待败者完成。放弃现库"任意时刻可放弃"的保证；核心
   去掉五个组件、每帧零原子、异步等待零分配、与 C++20 生态互通。
2. **标准词汇的拼写**：`coroutine_handle`/`suspend_always`/`noop_coroutine`/
   `coroutine_traits` 用标准的 snake_case，库自有类型保持 CamelCase。
3. **`Generator` 对齐 C++23 `std::generator`**（`elements_of` 取代
   `RecursiveGenerator`，`begin()` 单次调用）。
4. **实施范围**：先做阶段 1（核心 + 规范/差分测试）并评审。

## 13. 阶段 1 实现说明

代码最初放在 `include/co2/core/`，与旧核心并存；旧核心删除后移到 `include/co2/`
顶层（第 17 节，下表已按新路径列出）：

| 文件 | 内容 |
| --- | --- |
| `coroutine_handle.hpp` | `FrameHeader`、`coroutine_handle<>`/`<P>`、`FramePrefix`、`suspend_always`/`suspend_never`、`noop_coroutine`、`coroutine_traits` |
| `detail/awaitable.hpp` | `getAwaiter`（`operator_co_await` / C++20 `operator co_await`，成员优先于 ADL）、`AwaiterOf`/`AwaiterFor`、`transformAwaitable`、`suspendWith`（按 `await_suspend` 返回类型分派） |
| `detail/frame.hpp` | `FrameCore`（prefix、参数副本、局部、awaiter 槽、挂起点）、`CoroutineFrame`（+allocator，`step`/`destroy`）、promise 构造与 `operator new`/`get_return_object_on_allocation_failure`、ramp `startCoroutine` |
| `dsl.hpp` | `CO2_BEG`/`CO2_BEG_WITH_ALLOCATOR`/`CO2_END`/`CO2_AWAIT`/`CO2_AWAIT_SET`/`CO2_AWAIT_AS`/`CO2_AWAIT_AS_SET`/`CO2_YIELD`/`CO2_RETURN` |
| `coroutine.hpp` | 聚合头 |

与第 4 节草图的差别：参数副本（`_co2_capture_pack`）与局部（`_co2_body`）是帧里
两个独立对象，Body 以引用成员访问参数（每个参数 8 字节），换来与标准完全一致的
构造/析构顺序（参数副本 → promise → 局部；局部 → promise → 参数副本）。帧头多一个
`running` 标志，用来把"恢复正在运行的协程"、"销毁正在运行的协程"这两类未定义行为
报告为契约违规；它在调用 `await_suspend` 之前就被清掉，因此不会误判标准允许的
"另一线程在 `await_suspend` 返回前 resume"。

验证：

- `tests/conformance_tests.cpp`（C++14）：22 个用例覆盖第 1 节列出的规则。
- `tests/differential_tests.cpp`（C++20）：9 个场景，同一份 promise/awaiter
  （`tests/trace.hpp`）分别由 co2 宏与真正的 `co_await` 驱动，事件序列逐条
  相等；GCC 14 与 Clang 14 两个独立的编译器实现都与 co2 一致。
- 两者在 ASan+UBSan（含泄漏检测）下通过；核心头文件在 GCC 的 C++14/17/20/23 下
  `-Werror` 编译。

## 14. 阶段 2 实现说明：`Task<T>`、`Generator<Ref, V>`、`AsyncGenerator<T>`

三个返回类型都只依赖 v2 核心与 `detail/result_storage.hpp`，不含任何原子量、不含
任何分配（帧之外）。

| 文件 | 内容 |
| --- | --- |
| `task.hpp` | `Task<T = void>`、`detail::TaskPromise<T>`、`detail::TaskAwaiter<T>`、`detail::TaskAccess`（根适配器入口） |
| `generator.hpp` | `Generator<Ref, V = void>`、`elements_of(...)`、`default_sentinel_t`、`detail::GeneratorPromise` |
| `async_generator.hpp` | `AsyncGenerator<T>`、`detail::AsyncGeneratorPromise<T>` |

### 14.1 `Task<T>`

与 9.1 的草图一致（cppcoro `task`）：`initial_suspend = suspend_always`；等待者在
`await_suspend` 里写入 `continuation` 后对称转移进 child；`FinalAwaiter::await_suspend`
返回 `continuation`（无续体时 `noop_coroutine()`）。`return_value`/`return_void` 用
SFINAE 二选一，`T`/`void` 只有一份 promise（`ResultStorage<T>`）。

- **等待右值**（`CO2_AWAIT(child())`、`CO2_AWAIT(std::move(t))`）：`Task` 自身是
  awaiter，被移进等待者的槽，child 帧随 `await_resume` 之后的槽重置一起销毁（D10）。
- **等待左值**（`CO2_AWAIT(t)`）：`operator_co_await() &` 返回借用句柄的
  `TaskAwaiter`，`t` 继续拥有帧。
- **销毁契约的诊断**：promise 记录 `started`；析构一个 `started && !done()` 的 Task
  是契约违规（它挂起在某个操作上，标准里是 UB）。未启动、已完成的 Task 可随时销毁。
  `tests/task_destroy_contract_violation_test.cpp` 验证三种情形。
- `detail::TaskAccess::start(task, continuation)` 是根适配器（阶段 3 的 `syncWait`、
  `spawn`）启动 Task 的唯一入口。

### 14.2 `Generator<Ref, V>`

与 C++23 `std::generator<Ref, V>` 同形：`value_type`/`reference`/`yielded` 的推导规则
逐字相同（`Generator<int>` 的 `reference` 是 `int&&`，`Generator<T const&>` 不复制，
`Generator<Ref, V>` 显式 value type）；`begin()` 只能调用一次；`await_transform` 禁止
体内 `co_await`；`operator*` 之外的所有观察都在根 promise 上完成。

- **yield 重载**：`yielded` 为左值引用时 `yield_value(yielded)` 只记地址；`yielded` 为
  右值引用或 `T const&` 时 `yield_value(T&&)` 移进帧（D11）；`yielded` 为右值引用且 `T`
  可复制时 `yield_value(T const&)` 复制进帧（std 的 copy 路径）。存储随下一次恢复销毁。
- **嵌套**：`CO2_YIELD(co2::elements_of(inner()))`。所有层共享根 promise 上的
  `current`（当前元素）与 `active`（最内层活动协程）；消费者的 `++` 直接恢复
  `active`，内层的 `final_suspend` 把 `active` 改回父层并对称转移过去。栈深不随嵌套
  深度增长（测试：20000 层）。`elements_of(range)` 接受任意有 `begin/end` 的范围
  （左值按引用持有），包成一个同型生成器；左值生成器也按范围处理（与 std 相同，只有
  右值同型生成器才被直接接管）。
- **异常**：内层的异常存在内层 promise，`final_suspend` 转移回父层后在父层的
  `elements_of` 表达式处重新抛出（父层可捕获——但受 D1 限制不能把挂起点放进 `try`，
  所以实际上会继续上抛），未捕获时逐层到根，由 `begin()`/`++` 抛给消费者。
- **所有权**：嵌套生成器由父层帧里的 awaiter 拥有；销毁根即沿链销毁全部活动帧
  （递归深度 = 嵌套深度，与 std 相同）。中途销毁不是契约违规——生成器体内没有外部
  操作。
- **C++14 的 range-for**：`begin()`/`end()` 必须同型，`end()` 返回哨兵迭代器；同时提供
  `co2::default_sentinel_t` 比较。
- 未做：`std::generator` 的 `Allocator` 模板参数（帧分配走 `CO2_BEG_WITH_ALLOCATOR`）。

### 14.3 `AsyncGenerator<T>`

cppcoro `async_generator` 的无同步版本：`next()` 返回的 awaiter 记下消费者并对称转移
进生产者；生产者 `yield_value`/`final_suspend` 转移回消费者。任一时刻只有一方运行，
消费者在生产者 yield 所在的线程上恢复。`value()` 在 `next()` 得到 `true` 之后、下一次
`next()` 之前有效。销毁契约：有 `next()` 未完成时销毁是契约违规；停在 yield 点或未
启动时可随时销毁。

### 14.4 验证

- `tests/return_types_tests.cpp`（C++14，26 个用例）：Task 的惰性/链式转移/异常/
  深链（100000 层）/左值借用/move-only 结果/异步完成；Generator 的三种 `Ref` 形态/
  嵌套/范围/异常传播/中途销毁/深嵌套；AsyncGenerator 的同步与异步交替/异常/中途销毁。
- 在 GCC 14 与 Clang 14、ASan+UBSan 下全部通过；GCC C++14/17/20/23 与 Clang
  C++14/17/20 下 `-Werror` 编译（Clang 14 的 `-std=c++2b` 与 libstdc++ 14 的
  `<iostream>` 本身不兼容，与 co2 无关）。
- 顺带修复：`CO2_DETAIL_PP_REMOVE_PARENS` 对含逗号的括号返回类型
  （`CO2_BEG((Generator<int const&, int>), ...)`）展开错误。

## 15. 阶段 3（一）实现说明：`Scheduler`、`scheduleOn`、`ThreadPool`、`syncWait`

| 文件 | 内容 |
| --- | --- |
| `scheduler.hpp` | `Scheduler::schedule(coroutine_handle<>) noexcept`、`ScheduleOn` awaiter、`scheduleOn(s)` |
| `manual_executor.hpp` | `ManualExecutor`：任意线程 `schedule`，驱动线程 `runOne()`/`run()`；带着排队工作销毁是契约违规 |
| `thread_pool.hpp` | `ThreadPool`：工作窃取线程池；`detail::LocalQueue` |
| `sync_wait.hpp` | `syncWait(Task<T>)` |

### 15.1 模型

- 与标准一致：协程在完成它的那个线程上被内联恢复；换线程是显式的
  `CO2_AWAIT(scheduleOn(s))`。**没有隐式的"当前 scheduler"**（旧核心的
  `SchedulerExecutionScope`/`captureContinuation` 整套删除）。
- `Scheduler::schedule` 的队列元素就是句柄——一个指针：一次 hop 在稳态下**零分配**
  （实测 100000 次 `scheduleOn` 期间 `operator new` 调用为 0）、零原子门闩。`noexcept`：
  队列扩容失败是致命错误，与 `std::thread` 创建失败同类。
- `syncWait(Task<T>)`：Task 在调用线程上内联启动，续体是一个**手写的标准布局帧**
  （`FrameHeader` + `SyncWaitState*`，不占堆）——Task 的 `FinalAwaiter` 对称转移到它，
  它标记完成并唤醒等待线程。不再有 run loop：既然没有隐式 scheduler，也就没有"回到
  阻塞线程执行续体"的需求；需要时以后可以作为可选 scheduler 加回。
- awaiter 都只持有指针（`Scheduler*`、`SyncWaitState*`），不借用表达式里的临时对象
  （D10）。

### 15.2 `ThreadPool`

- 每个工作线程一个固定容量（1024）的**本地 FIFO 队列**：owner 在 tail 端 push（release
  存储），owner 与 thief 都在 head 端以 CAS 认领（单生产者多消费者环）。外部线程提交
  与本地溢出进入一个互斥锁保护的全局队列。找活顺序：本地 → 全局 → 随机起点轮询窃取
  （最多两轮，遇到 CAS 竞争才重试第二轮）；每 64 次循环强制先看全局队列，外部提交
  不会被本地工作饿死。
- **为什么不是 Chase-Lev（LIFO）**：协程在工作线程上 `scheduleOn(pool)` 是让步，LIFO
  会让它立刻被同一线程重新弹出，让步形同虚设（测试
  `reschedulingOnAWorkerYieldsToEarlierLocalWork`）。FIFO 与 Go 的 runq、Tokio 的
  local queue 一致；代价是 owner 取本地工作也要一次 CAS（4 线程 16 个 hopper 的
  聚合吞吐从 61M hop/s 降到 42M hop/s，仍是 ~24 ns/hop）。Go/Tokio 的"最近唤醒者
  LIFO 槽"是后续可加的局部性优化。
- **停车/唤醒**是 Dekker 式协议，没有共享计数器：提交方 push 之后 `seq_cst` 栅栏再读
  `idleWorkers`，非零才拿锁 `notify_one`；停车方在锁内 `idleWorkers++`、`seq_cst` 栅栏、
  复查所有本地队列与全局队列，都空才等待。按 [atomics.fences] 的栅栏-栅栏规则二者
  总有一方看到另一方，热路径（满载）上没有任何共享 RMW，也不拿锁。唤醒用计数
  （`pendingWakeups`，上限为线程数）而不是裸 `notify_one`，避免"通知早于等待"丢失。
- **销毁**：析构请求停止并 join；工作线程只在"正在停止且所有队列都空"时退出，排空
  期间新排入的工作也跑完（测试 `destructorDrainsQueuedWork`：每个作业在排空期间再 hop
  两次）。析构期间从外部线程再 `schedule` 是使用者的错误。
- 本地队列以填充而非 `alignas(64)` 隔离 head/tail：C++14 的 `new` 不保证超默认对齐。
- `push` 用 release 存储而不是原论文的 release 栅栏 + relaxed 存储：语义相同，但
  ThreadSanitizer 不建模独立栅栏，前者让 TSan 能跟踪帧从提交线程到窃取线程的
  happens-before（否则报假阳性）。

### 15.3 验证

- `tests/scheduler_tests.cpp`：`ManualExecutor` 的挂起/FIFO/排空、`syncWait` 的
  同步/void/move-only/异常、以及在另一线程上完成与抛出的唤醒。
- `tests/thread_pool_tests.cpp`：hop 到工作线程、异常跨线程到 `syncWait`、外部
  提交分散到多个线程、本地 push 被空闲线程窃取、本地队列溢出（5000 个）、析构排空、
  单线程池、跨两个池的 hop、FIFO 让步、16 个 hopper × 5000 hop 的唤醒压力。
- 契约违规测试：带排队工作销毁 `ManualExecutor`。
- 线程池测试在 TSan 下连续 15 轮、普通构建连续 30 轮无失败；ASan+UBSan、GCC、Clang
  全套通过。
- 未做（阶段 3 剩余）：stop token 改名与 Event/Mutex/WorkGroup/timer 迁移、`spawn`。

## 16. 阶段 3（二）实现说明：`stop_token`、协程环境、`spawn`

| 文件 | 内容 |
| --- | --- |
| `stop_token.hpp` | `stop_source` / `stop_token` / `stop_callback<Callback = std::function<void()>>` / `nostopstate` |
| `env.hpp` | `getStopToken()`（`read_env(get_stop_token)` 的 co2 拼写）、`detail::stopTokenOf(handle)` |
| `task.hpp` | promise 持有 `stop_token`；`await_transform(GetStopToken)`；带类型父句柄的 `await_suspend` 继承环境；`TaskAccess::arm/release` |
| `spawn.hpp` | `spawn(scheduler, task[, parentToken])`、`JoinHandle<T>` |
| `sync_wait.hpp` | `syncWait(task[, token])` |

### 16.1 `stop_token`：与 [thread.stoptoken] 逐条一致

- `request_stop()` 以 `exchange` 原子地"判断并置位"，只有真正发出请求的那次返回 true，
  回调在调用线程上同步执行完毕后返回；注册时若已请求则在构造函数里立刻执行；
  `stop_possible()` = 已请求或仍有关联的 `stop_source`（源计数，token 只计引用）。
- `~stop_callback()` 的三种情形：仍在链上——摘除；回调正在另一线程执行——阻塞到它
  返回（条件变量）；回调正在本线程执行并销毁了自己——不阻塞，`request_stop` 通过节点
  上的 `destroyed` 栈标志得知不再触碰它。回调抛出即 `std::terminate`（thunk 是 noexcept）。
- 实现：一个引用计数的 `StopState`（`references`/`sources`/`requested` 三个原子量 +
  一把互斥锁保护侵入式双向回调链表）。回调链表操作只在注册/注销/请求时发生，不在
  协程热路径上。
- 与标准的差别只有一处：`Callback` 默认为 `std::function<void()>`——C++14 没有 CTAD，
  lambda 类型无法拼写，`co2::stop_callback<> cb{token, [&]{...}}` 是可用的形式。
- 未做：C++20 构建下与 `std::stop_token` 互通的别名/适配器。

### 16.2 协程环境：stop_token 沿等待链传播

采用 std::execution 的模型而不是线程局部量或显式参数：Task 的 promise 持有一个
`stop_token`；`co_await` 子 Task 时 `TaskAwaiter::await_suspend` 收到的是**带类型的**
`coroutine_handle<ParentPromise>`（`suspendWith` 传的就是 `coroutine_handle<Promise>`，
与标准一致），于是子 promise 从 `parent.promise().get_stop_token()` 继承；没有
`get_stop_token()` 的 promise（自定义根）给子协程一个 `stop_possible()` 为假的 token；
类型擦除的 `coroutine_handle<>` 同理。根由 `syncWait(task, token)` 与 `spawn` 提供。

体内读取：`CO2_AWAIT_SET(token, co2::getStopToken())`——`TaskPromise::await_transform(
GetStopToken)` 返回一个不挂起的 awaiter。为此 `TaskPromise` 有了 `await_transform`，
按标准规则同时提供了原样转发其余 awaitable 的泛型重载。

取消本身是协作式的：本阶段没有任何 awaitable 响应 stop（Event/timer 迁移时它们会
注册 `stop_callback` 并提前完成）；Task 体内以 `token.stop_requested()` 检查。

### 16.3 `spawn` / `JoinHandle<T>`

- `spawn(scheduler, task, parent = {})`：给 Task 设好续体与 token 后**直接把它的句柄
  排给 scheduler**——惰性 Task 停在初始挂起点，本身就是可排队的句柄，第一次恢复就发生
  在 scheduler 的线程上，没有跳板帧。每个 spawn 一次堆分配（`JoinState`）。
- 每个根有自己的 `stop_source`：`JoinHandle::requestStop()` 请求它；传入 `parent` 时
  `JoinState` 里的 `stop_callback<ForwardStop>` 把父的请求转发过来（父已请求则构造时
  立即转发，Task 一启动就看到已请求）。
- 完成：Task 的 `FinalAwaiter` 对称转移到 `JoinState` 内嵌的手写续体帧
  `JoinCompletion`（标准布局，不占堆）；它在锁内置 `ready`、通知阻塞的 `join()`，并把
  等待中的协程句柄作为对称转移目标返回——等待者在完成线程上恢复（标准行为）。
- 等待：右值 `CO2_AWAIT(spawn(...))` 时 `JoinHandle` 自身是 awaiter（拥有），左值时
  `operator_co_await() &` 借用；`await_suspend` 返回 `bool`（已完成则不挂起），一次拿锁。
- **析构语义**（与 P3149 `spawn_future` 一致）：Task 已完成——销毁帧；未完成——先在锁
  外 `request_stop()`（回调可能同步把 Task 推到完成，而完成需要拿锁），再分离：帧的
  所有权交给 `JoinState`，Task 跑完时由 `JoinCompletion` 销毁帧并释放状态，结果与异常
  丢弃。既不阻塞（`jthread` 会）、也不违反销毁契约、也不 UB。借用本 handle 的等待者还
  挂着时析构是契约违规（否则等待者恢复后访问已销毁对象）。
- 两方共享 `JoinState`，谁最后离开谁释放：`ready`/`detached` 两个布尔在同一把锁下判定。

### 16.4 验证

- `tests/stop_token_tests.cpp`（11 例）：一次性 `request_stop`、已请求时同步执行、
  销毁后不执行、`stop_possible` 随最后一个源消失、拷贝/移动共享状态、回调内嵌套请求与
  注册、回调销毁自己、跨线程析构阻塞到回调返回、4 线程 × 50 次注册/注销与请求并发
  20 轮。
- `tests/spawn_tests.cpp`（12 例）：ManualExecutor/ThreadPool 上 spawn 与
  join、异常、move-only 结果、左值/右值等待、等待已完成句柄、`requestStop` 经
  `getStopToken` 被观察、丢弃句柄→请求停止并分离（8 个帧全部销毁）、子 Task 继承
  token、无根时 `stop_possible()` 为假、父 token 转发、父已停止时启动即停止、移动。
- 契约违规：借用等待者挂着时销毁 `JoinHandle`。
- TSan 各 15 轮、普通构建 40 轮无失败；ASan+UBSan、GCC、Clang 全套 32 个 ctest 通过；
  GCC C++14/17/20/23、Clang C++14/17 `-Werror` 编译。
- 环境备注：本机（Linux 5.15，LLVM 14 sanitizer）在 ASLR 熵较高时 sanitizer 运行时会
  随机报 "unexpected memory mapping" / 段错误于启动阶段，与被测代码无关；用
  `setarch $(uname -m) -R` 关闭 ASLR 后全部稳定通过。
- 阶段 3 剩余：Event/Mutex/WorkGroup/timer 迁移到句柄模型并响应 stop_token。

## 17. 目录收口：删除 v1，v2 成为唯一实现

阶段 5 提前执行：在 Event/Mutex/timer/组合器尚未重建之前就删除旧核心，因为两套核心
并存的成本（两套 DSL、两套 `Task`、文档双轨）已经高于暂时缺失这些组件的成本；缺失的
组件在 v2 契约之上重建比在 v1 上维护更省。

### 17.1 目录结构

```
include/co2/
  co2.hpp                 全部公共头
  coroutine.hpp           核心聚合（coroutine_handle + dsl + detail/awaitable + detail/frame）
  coroutine_handle.hpp    coroutine_handle<P> / suspend_* / noop_coroutine / coroutine_traits
  dsl.hpp                 CO2_* 宏
  task.hpp  generator.hpp  async_generator.hpp
  scheduler.hpp  manual_executor.hpp  thread_pool.hpp
  sync_wait.hpp  spawn.hpp
  stop_token.hpp  env.hpp
  contract.hpp  config.hpp
  detail/
    awaitable.hpp         operator co_await 查找、await_transform、suspendWith（全部 detail::）
    frame.hpp             FrameCore / CoroutineFrame / ramp（全部 detail::）
    await_slot.hpp  result_storage.hpp  preprocessor.hpp
tests/
  conformance_tests.cpp  differential_tests.cpp (C++20)  trace.hpp
  storage_tests.cpp  return_types_tests.cpp  scheduler_tests.cpp  thread_pool_tests.cpp
  stop_token_tests.cpp  spawn_tests.cpp
  task_destroy_ / manual_executor_ / join_handle_contract_violation_test.cpp
  install_consumer/
benchmarks/co2_benchmarks.cpp
```

划分规则：`co2/*.hpp` 是公共 API，每个头独立可包含；`co2/detail/*.hpp` 只含
`co2::detail`，公共头通过包含它们工作，用户不直接包含。`core/` 目录不再存在。

### 17.2 删除的 v1 文件

- 核心与恢复层：`coroutine_core.hpp`、`resume_operation.hpp`、旧 `dsl.hpp`/
  `coroutine.hpp`、`detail/frame.hpp`（旧）、`frame_model.hpp`、`frame_execution.hpp`、
  `frame_termination.hpp`、`scheduled_resume.hpp`。
- 组件：`blocking.hpp`、旧 `task.hpp`/`generator.hpp`/`async_generator.hpp`/
  `spawn.hpp`/`scheduler.hpp`/`manual_executor.hpp`、`thread_pool_executor.hpp`、
  `recursive_generator.hpp`、`shared_task.hpp`、`task_scope.hpp`、`cancellation.hpp`、
  `cancellation_wait.hpp`、`abandon_on_cancellation.hpp`、`callback.hpp`、`timer.hpp`、
  `timeout.hpp`、`steady_timer_service.hpp`、`manual_timer_service.hpp`、`unit.hpp`、
  `sync/{event,mutex,work_group,when_all,when_any,race}.hpp`、`detail/{join_set,
  wait_queue,timer_queue,race_with_signal,task_sequence,first_error}.hpp`、
  `compat/cpp20.hpp`。
- 测试：v1 的全部测试；`core_lifetime_tests.cpp` 中仍适用的 `ResultStorage`/`AwaitSlot`
  用例移入 `storage_tests.cpp`。
- `code_review_report.md`（针对 v1 代码的评审记录）。

### 17.3 保留的构件

`contract.hpp`、`config.hpp`、`detail/await_slot.hpp`、`detail/result_storage.hpp`、
`detail/preprocessor.hpp` 是无核心依赖的叶子，v1/v2 共用，原样保留。

### 17.4 待重建（按依赖顺序）

1. Event / Mutex / WorkGroup：awaiter 内侵入式节点保存 `coroutine_handle<>`；取消通过
   `stop_callback` 摘除节点并以取消结果恢复。
2. Timer：`TimerService` 接口 + steady/manual 实现；`withTimeout` 建立在 `race` 之上。
3. `whenAll` / `whenAny` / `race`：向败者请求 stop 并等待其完成。
4. `TaskScope`（P3149 `async_scope`）：析构前必须 join；`SharedTask`。
5. C++20 互操作层：`std::coroutine_handle` ↔ `co2::coroutine_handle` 的桥。
6. 版本号：v2 是不兼容重写，`0.2.0-pre` 应升为新的主/次版本，与包版本策略一并决定。
