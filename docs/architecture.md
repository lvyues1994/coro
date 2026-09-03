# 架构

本文描述 co2 当前（v2 核心）的架构：组件、分层、契约与生命周期。设计动机、与标准的
逐条对照、被否决的替代方案以及各阶段的实现说明见
`docs/design/coroutine-core-v2.md`。

## 问题

C++14 没有语言级协程。co2 用宏生成的 switch 状态机模拟挂起与恢复，但**不发明自己的
协程模型**：帧布局、promise 与 awaiter 协议、句柄语义、销毁契约全部取自 C++20
[dcl.fct.def.coroutine] / [expr.await] / [coroutine.handle]，返回类型取自 C++23
`std::generator`、cppcoro `task`、C++20 `std::stop_token` 与 P2300/P3149 的执行模型。
好处是双向的：为标准写的 promise/awaiter 类型可以逐字用在 co2 上，co2 用户学到的
一切也都能迁移到语言级协程。

## 分层

```
┌────────────────────────────────────────────────────────────────┐
│ 根适配器          sync_wait.hpp   spawn.hpp                      │
├────────────────────────────────────────────────────────────────┤
│ 执行层            scheduler.hpp  manual_executor.hpp  thread_pool.hpp │
├────────────────────────────────────────────────────────────────┤
│ 返回类型          task.hpp  generator.hpp  async_generator.hpp   │
│ 环境 / 取消       env.hpp   stop_token.hpp                       │
├────────────────────────────────────────────────────────────────┤
│ 核心              coroutine_handle.hpp  dsl.hpp                  │
│                   detail/awaitable.hpp  detail/frame.hpp         │
├────────────────────────────────────────────────────────────────┤
│ 构件              detail/await_slot.hpp  detail/result_storage.hpp │
│                   detail/preprocessor.hpp  contract.hpp  config.hpp │
└────────────────────────────────────────────────────────────────┘
```

依赖只向下。核心不含任何同步原语、线程或分配器以外的东西；`stop_token.hpp` 与核心
互不依赖。

## 核心

### 帧

一个协程一次分配，帧是标准布局对象（`detail::CoroutineFrame`）：

```
FrameHeader { destroy, step, done, running }   ← 句柄指向这里（偏移 0）
Promise                                        ← 紧随其后（FramePrefix，固定偏移）
参数副本（_co2_capture_pack）
局部 + 程序计数器（_co2_body）
AwaitSlot（awaiter 槽：内联 8 个指针宽度，超出退化为堆）
Allocator
```

构造顺序 参数副本 → promise → 局部；销毁顺序 awaiter → 局部 → promise → 参数副本，
与标准一致。局部在离开协程体作用域时（`co_return`、掉出末尾、异常逃出）就销毁，早于
`final_suspend`。

### 句柄

`co2::coroutine_handle<P>` 是非拥有的：只保存 `FrameHeader*`。`resume()` 通过帧头里的
`step` 函数指针分派，`destroy()` 通过 `destroy`；`promise()` / `from_promise()` 用标准
布局前缀的固定偏移计算。

**对称转移以迭代实现**：`resume()` 是一个循环，`step` 返回下一个要驱动的帧头
（`await_suspend` 返回的句柄），栈深不随转移链长度增长。`noop_coroutine()` 的 `step`
返回空，等价于"挂起并把控制交回 resumer"。

### 宏与协议

`CO2_BEG` 展开出参数副本类型、协程体类型（局部为成员，`operator()` 是 switch 状态机）
与 ramp 调用；`CO2_AWAIT` 展开为标准的 `co_await` 序列：`await_transform`（若 promise
提供）→ `operator co_await` 查找（成员优先于 ADL；C++14 拼作 `operator_co_await`）→
`await_ready` → `await_suspend`（按 `void` / `bool` / `coroutine_handle<Z>` 分派）→
`await_resume`。`CO2_YIELD(e)` ≡ `co_await promise.yield_value(e)`（不经过
`await_transform`），`CO2_RETURN` ≡ `return_void`/`return_value` + `co_await
final_suspend()`。异常离开协程体走标准的 `unhandled_exception` 路径；初始挂起点之前的
异常销毁帧并抛给调用方。

`await_suspend` 收到的是**带类型的** `coroutine_handle<Promise>`，因此 awaiter 可以像
标准一样以模板 `await_suspend` 查询等待者的 promise（co2 用它传播环境）。

## 契约

### 销毁契约（标准规则）

- 帧只能在挂起时销毁；恢复正在运行的协程、销毁正在运行的协程是契约违规（帧头的
  `running` 位只服务于这条诊断）。
- **挂起在某个操作上的协程不能销毁**：操作持有它的句柄，会在完成时恢复它。取消是
  协作式的（`stop_token`）——请求停止 → 操作提前完成 → 协程走到 final suspend → 拥有者
  销毁。`Task` 把"已启动且未完成时析构"报告为契约违规；`Generator` 体内没有外部操作，
  可以随时销毁；`AsyncGenerator` 有 `next()` 未完成时不能销毁。
- 这条契约是核心零原子、异步等待零分配的前提：唯一真正的并发点（`await_suspend` 交出
  句柄后被另一线程恢复）不需要帧内状态。

### 契约违规

前置条件与不变量违规经由 `CO2_CONTRACT_CHECK` 交给进程级处理器
（`setContractViolationHandler`），默认 `std::terminate()`。热路径的内部断言在 `NDEBUG`
下编译掉。运行时条件（Task 失败、取消……）以异常报告。

## 返回类型

- **`Task<T>`**：cppcoro `task` 形态。`initial_suspend = suspend_always`；等待者在
  `await_suspend` 里写入 `continuation` 后对称转移进 child；`FinalAwaiter` 转移回等待者。
  惰性 + 先写续体再转移，让"完成 vs 挂起"没有竞争窗口，Task 不需要原子量。右值
  Task 自身是 awaiter（拥有 child 帧——宏展开里的临时对象活不过挂起点，见设计稿
  D10），左值经 `operator_co_await() &` 借用。`T`/`void` 只有一份 promise
  （`ResultStorage<T>` + SFINAE 二选一的 `return_value`/`return_void`）。
- **`Generator<Ref, V>`**：`std::generator` 同形。所有嵌套层共享根 promise 上的
  `current`（当前元素指针）与 `active`（最内层活动协程）；消费者的 `++` 直接恢复
  `active`，内层 `final_suspend` 把 `active` 改回父层并对称转移过去。嵌套生成器由父层
  帧里的 awaiter 拥有。yield 的 prvalue 移进帧保存（D11），左值引用只记地址。
- **`AsyncGenerator<T>`**：`next()` 记下消费者并转移进生产者；`yield_value` /
  `final_suspend` 转移回消费者。任一时刻只有一方运行，没有同步。

## 环境与取消

`TaskPromise` 持有一个 `stop_token`；`TaskAwaiter::await_suspend(coroutine_handle<
ParentPromise>)` 让子 promise 从父 promise 的 `get_stop_token()` 继承；没有该成员的
promise（自定义根）与类型擦除的句柄给出 `stop_possible()` 为假的 token。根由
`syncWait(task, token)` 与 `spawn` 提供。体内 `CO2_AWAIT_SET(t, getStopToken())` 经
`TaskPromise::await_transform(GetStopToken)` 读取（其余 awaitable 由泛型重载原样转发）。

`stop_source`/`stop_token`/`stop_callback` 与 `[thread.stoptoken]` 逐条一致：一个引用
计数的状态（`references`/`sources`/`requested` 三个原子量 + 一把互斥锁保护的侵入式
回调链表）。取消是协作式的：本阶段没有 awaitable 自动响应 stop，Task 体内以
`token.stop_requested()` 检查；后续的 Event/timer 会注册 `stop_callback` 并提前完成。

## 执行层

- **`Scheduler::schedule(coroutine_handle<>) noexcept`**：队列元素就是句柄，一次 hop
  稳态零分配。协程在完成它的线程上被内联恢复；换线程显式 `scheduleOn(s)`。
- **`ThreadPool`**：每线程一个固定容量（1024）的本地 FIFO 队列（owner 尾部 push，
  owner/thief 头部 CAS 认领——单生产者多消费者环）+ 互斥全局队列（外部提交、本地溢出）。
  找活 本地 → 全局 → 随机起点窃取，每 64 次强制先看全局。选 FIFO 而不是 Chase-Lev
  LIFO 是为了让工作线程上的 `scheduleOn(pool)` 成为真正的让步。停车/唤醒是 Dekker 式
  栅栏协议：提交方 push → `seq_cst` 栅栏 → 读 `idleWorkers`；停车方 `idleWorkers++` →
  栅栏 → 复查所有队列；满载热路径无共享 RMW、不拿锁。析构排空后 join。
- **`ManualExecutor`**：互斥 FIFO；带排队工作销毁是契约违规。
- **`syncWait`**：Task 在调用线程上内联启动；续体是一个不占堆的手写标准布局帧，被对称
  转移到时标记完成并唤醒等待线程。
- **`spawn`**：给 Task 设好续体与 token 后直接把它的句柄排给 scheduler（惰性 Task
  本身就是可排队句柄，无跳板帧）。`JoinHandle` 与内嵌续体帧共享一个堆分配的
  `JoinState`；未完成时析构 = 请求停止并分离（P3149 `spawn_future`），Task 跑完后由
  续体帧销毁帧并释放状态。

## 线程模型

- 帧内没有同步。同一协程任一时刻只在一个线程上运行；`await_suspend` 交出句柄之后
  另一线程即可恢复它（生成的代码在调用 `await_suspend` 之前就清掉 `running`）。
- 跨线程可见性由传递句柄的机制负责：`ThreadPool` 本地队列的 release 存储 / acquire
  读取、全局队列的互斥锁、`JoinState`/`SyncWaitState` 的互斥锁。
- 同步 Task 链、生成器、异步生成器全程单线程，不付任何并发代价。

## 尚未重建的 v1 能力

事件 / 协程互斥锁 / WorkGroup、定时器与超时、`whenAll` / `whenAny` / `race`、
`SharedTask`、`TaskScope`（P3149 `async_scope`）、回调适配、C++20
`std::coroutine_handle` 互操作层。它们将在标准销毁契约与 `stop_token` 之上重建：
awaiter 内侵入式节点保存 `coroutine_handle<>`，取消通过注册 `stop_callback` 把自己
从队列摘除并以取消结果恢复协程；`race`/`withTimeout` 向败者请求 stop 并等待其完成；
`TaskScope` 析构前必须 join。
