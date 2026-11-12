#pragma once

#include <exception>
#include <type_traits>
#include <utility>

#include <memory>
#include <new>

#include "co2/completion.hpp"
#include "co2/contract.hpp"
#include "co2/coroutine.hpp"
#include "co2/env.hpp"
#include "co2/error.hpp"
#include "co2/just.hpp"
#include "co2/optional.hpp"
#include "co2/result.hpp"
#include "co2/stop_token.hpp"

// co2：惰性、单消费者的 Task<T>。promise 与 cppcoro::task 逐字同形：
//   initial_suspend = suspend_always；等待者在 await_suspend 里记下自己的句柄并对称
//   转移进 child；child 的 final_suspend 对称转移回等待者。惰性启动让"完成 vs 挂起"
//   没有竞争窗口，因此 Task 不需要任何原子量。
//
// 环境：promise 持有一个 stop_token。等待子 Task 时子 promise 从父 promise 继承它
// （await_suspend 收到的是带类型的 coroutine_handle<ParentPromise>，与 std::execution
// 的 task 查询父环境的方式相同）；根由 syncWait / spawn 提供。体内用
// CO2_AWAIT_SET(token, co2::getStopToken()) 读取。继承是借用而不是拷贝：父在子运行期间
// 一直挂着，子 promise 只记下父的停止状态，不碰引用计数（detail::EnvStopToken）。
//
// 三种完成方式（docs/design/completion-channels.md）：Task 以值、错误或已停止结束。
// 错误与已停止按值沿等待链向上传——等待它的协程不再往下执行，以同样的结果结束——只在
// 根（syncWait、JoinHandle::join）上变回异常或空的 optional。协程体里抛出的异常在
// unhandled_exception() 里被捕获成一个 Error，此后同样按值传，中间各层不再重抛。
//
// 销毁契约（标准规则）：一个已经启动、尚未完成的 Task 不能销毁——它挂起在某个操作
// 上，而操作只有完成后才能销毁其状态。co2 把这条未定义行为报告为契约违规。取消是
// 协作式的：请求 stop → 操作提前完成 → Task 走到 final suspend → owner 销毁。

namespace co2 {

template <class T = void> struct Task;

namespace detail {

struct TaskAccess;

// Task 的结果。empty：还没结束，或者值已经被取走。
enum class TaskState : unsigned char { empty, value, error, stopped };

// 值与错误共用的一块存储，自己不带标记：哪个成员活着由 TaskPromise::state 说明。两者
// 互斥，分开存是白占空间；标记放在 promise 里则是为了和 started 挨在一起，共占一个对齐
// 单位。于是 promise 与从前只存 exception_ptr 的时候一样大，帧不变大。
template <class T> union TaskStorage {
    TaskStorage() noexcept {}
    ~TaskStorage() {}

    T value;
    Error error;
};

template <> union TaskStorage<void> {
    TaskStorage() noexcept {}
    ~TaskStorage() {}

    Error error;
};

template <class T> struct TaskPromise {
    // 声明构造函数是为了让它不是聚合类型。聚合在 C++20 起可以用圆括号初始化，"能用参数
    // 副本构造 promise 就传入"的规则会因此拿协程的第一个参数去初始化 continuation。
    TaskPromise() = default;
    TaskPromise(TaskPromise const&) = delete;
    TaskPromise& operator=(TaskPromise const&) = delete;

    ~TaskPromise() {
        if (state == TaskState::value)
            destroyValue(std::is_void<T>{});
        else if (state == TaskState::error)
            storage.error.~Error();
    }

    Task<T> get_return_object() noexcept;

    suspend_always initial_suspend() noexcept { return {}; }

    struct FinalAwaiter {
        bool await_ready() const noexcept { return false; }

        coroutine_handle<>
        await_suspend(coroutine_handle<TaskPromise> const self) noexcept {
            auto const next = self.promise().continuation;
            return next ? next : noop_coroutine();
        }

        void await_resume() const noexcept {}
    };

    FinalAwaiter final_suspend() noexcept { return {}; }

    // 圆括号是有意的：泛型 T 上花括号可能选中 initializer_list 构造函数。
    template <class Value, class U = T,
              class = typename std::enable_if<not std::is_void<U>::value>::type>
    void return_value(Value&& value) {
        ::new (static_cast<void*>(std::addressof(storage.value)))
            U(std::forward<Value>(value));
        state = TaskState::value;
    }

    template <class U = T,
              class = typename std::enable_if<std::is_void<U>::value>::type>
    void return_void() noexcept {
        state = TaskState::value;
    }

    // 异常只在这里被捕获一次；之后它是一个按值传的 Error，到根上才重新抛出。
    void unhandled_exception() noexcept { fail(Error{std::current_exception()}); }

    // 等待的操作出错 / 已停止（或者体内 CO2_AWAIT(just_error(e)) 之类）：本 Task 以同样的
    // 结果结束。由恢复点在协程体收尾之后调用，见 FrameCore::finishUnlessValue。
    void return_error(Error error) noexcept { fail(std::move(error)); }
    void return_stopped() noexcept { state = TaskState::stopped; }

    // CO2_YIELD(co2::with_error(e))：P3552 的拼写，效果同 CO2_AWAIT(co2::just_error(e))。
    // Task 不能 yield 别的东西。
    JustError yield_value(WithError failure) noexcept {
        return JustError{std::move(failure.error)};
    }

    // ---- 环境 ----

    stop_token get_stop_token() const noexcept { return stopToken.get(); }

    // 子协程从这里借用，不拷贝 token（见 env.hpp 的 inheritStopToken）。
    EnvStopToken const& envStopToken() const noexcept { return stopToken; }

    // co_await getStopToken()：不挂起，交出本 Task 的 token。
    StopTokenAwaiter await_transform(GetStopToken) noexcept {
        return StopTokenAwaiter{stopToken.get()};
    }

    // 其余 awaitable 原样等待（标准：promise 一旦有 await_transform 就必须覆盖全部）。
    template <class Awaitable>
    Awaitable&& await_transform(Awaitable&& awaitable) noexcept {
        return std::forward<Awaitable>(awaitable);
    }

    // 这个 Task 是怎么结束的。给认识 await_completion() 的等待者用：错误被移出去，所以
    // 每次等待只问一次。值已被取走（empty）时也报告"有值"，由随后的 result() 诊断。
    Completion completion() noexcept {
        if (state == TaskState::stopped) return Completion::stopped();
        if (state == TaskState::error) return Completion::error(std::move(storage.error));
        return Completion::value();
    }

    // 取走值，只可一次。它同时是不认识 await_completion() 的调用方的兜底（手写的包装
    // awaiter、原生 co_await 都直接调用 await_resume()）：错误在这里变回异常，与从前
    // 一样；已停止没有值可给，是契约违规。
    T result() {
        if (state == TaskState::error) storage.error.raise();
        CO2_CONTRACT_CHECK(state == TaskState::value);
        return takeValue(std::is_void<T>{});
    }

    coroutine_handle<> continuation;
    TaskStorage<T> storage;
    EnvStopToken stopToken; // 根持有引用；被等待的子 Task 借用等待者的
    TaskState state{TaskState::empty};
    bool started{};

  private:
    void fail(Error error) noexcept {
        ::new (static_cast<void*>(std::addressof(storage.error))) Error{std::move(error)};
        state = TaskState::error;
    }

    // 移动构造抛出时值留在原处、state 不变，析构函数仍会清理。
    template <class U = T> U takeValue(std::false_type) {
        U result(std::move(storage.value));
        storage.value.~U();
        state = TaskState::empty;
        return result;
    }

    void takeValue(std::true_type) noexcept { state = TaskState::empty; }

    template <class U = T> void destroyValue(std::false_type) noexcept {
        storage.value.~U();
    }

    void destroyValue(std::true_type) noexcept {}
};

// 等待一个 Task：记下等待者，对称转移进 Task；Task 完成时 FinalAwaiter 转移回来。
// 它不拥有帧——拥有者是 Task 对象（右值等待时 Task 本身就在 awaiter 槽里）。
template <class T> struct TaskAwaiter {
    coroutine_handle<TaskPromise<T>> handle;

    bool await_ready() const noexcept { return not handle || handle.done(); }

    // 带类型的父句柄：子 Task 从父 promise 继承环境。
    template <class ParentPromise>
    coroutine_handle<>
    await_suspend(coroutine_handle<ParentPromise> const awaiting) noexcept {
        auto& promise = handle.promise();
        CO2_CONTRACT_CHECK(not promise.started);
        promise.continuation = awaiting;
        inheritStopToken(promise.stopToken, awaiting);
        promise.started = true;
        return handle;
    }

    T await_resume() {
        CO2_CONTRACT_CHECK(handle);
        return handle.promise().result();
    }

    Completion await_completion() noexcept {
        CO2_CONTRACT_CHECK(handle);
        return handle.promise().completion();
    }
};

} // namespace detail

template <class T> struct Task {
    using promise_type = detail::TaskPromise<T>;

    Task() noexcept = default;

    Task(Task&& other) noexcept : handle{other.handle} { other.handle = nullptr; }

    Task& operator=(Task&& other) noexcept {
        if (this == &other) return *this;
        reset();
        handle = other.handle;
        other.handle = nullptr;
        return *this;
    }

    Task(Task const&) = delete;
    Task& operator=(Task const&) = delete;

    ~Task() { reset(); }

    explicit operator bool() const noexcept { return static_cast<bool>(handle); }

    // ---- 等待 ----
    //
    // 右值（CO2_AWAIT(makeTask()) / CO2_AWAIT(std::move(task))）：Task 本身就是
    // awaiter，被移进等待者的 awaiter 槽，child 帧随槽一起在 await_resume 之后销毁。
    // 宏展开里的临时对象活不过挂起点（设计稿 D10），所以右值路径必须由 awaiter 拥有帧。
    bool await_ready() const noexcept { return awaiter().await_ready(); }

    template <class ParentPromise>
    coroutine_handle<>
    await_suspend(coroutine_handle<ParentPromise> const awaiting) noexcept {
        return awaiter().await_suspend(awaiting);
    }

    T await_resume() { return awaiter().await_resume(); }

    Completion await_completion() noexcept { return awaiter().await_completion(); }

    // 左值（CO2_AWAIT(task)）：借用句柄，Task 继续拥有帧。
    detail::TaskAwaiter<T> operator_co_await() & noexcept { return awaiter(); }

  private:
    explicit Task(coroutine_handle<promise_type> const handle_) noexcept
        : handle{handle_} {}

    detail::TaskAwaiter<T> awaiter() const noexcept {
        return detail::TaskAwaiter<T>{handle};
    }

    void reset() noexcept {
        if (not handle) return;
        // 已启动却未完成的 Task 挂起在某个操作上，销毁它是未定义行为。
        CO2_CONTRACT_CHECK(not handle.promise().started || handle.done());
        handle.destroy();
        handle = nullptr;
    }

    coroutine_handle<promise_type> handle;

    friend struct detail::TaskPromise<T>;
    friend struct detail::TaskAccess;
};

namespace detail {

template <class T> Task<T> TaskPromise<T>::get_return_object() noexcept {
    return Task<T>{coroutine_handle<TaskPromise>::from_promise(*this)};
}

// 根（syncWait、JoinHandle::join）交给普通代码的类型：结果放进 optional，空表示已停止。
// void 没有值可放，用 Unit 占位，这样 Task<void> 的根与其他根同形。
template <class T> struct RootValue { using type = T; };
template <> struct RootValue<void> { using type = Unit; };
template <class T> using RootResult = optional<typename RootValue<T>::type>;

// 根适配器（syncWait、spawn）启动 Task 的唯一入口：设置续体与根环境，然后恢复
// （或交给 scheduler 恢复）。
struct TaskAccess {
    template <class T>
    static void start(Task<T>& task, coroutine_handle<> const continuation,
                      stop_token token = {}) {
        arm(task, continuation, std::move(token)).resume();
    }

    // 惰性 Task 停在初始挂起点，本身就是一个可排队的句柄：第一次恢复直接发生在
    // scheduler 的线程上，不需要额外的跳板帧。
    template <class T>
    static coroutine_handle<> arm(Task<T>& task, coroutine_handle<> const continuation,
                                  stop_token token = {}) noexcept {
        CO2_CONTRACT_CHECK(task.handle && not task.handle.promise().started);
        auto& promise = task.handle.promise();
        promise.continuation = continuation;
        promise.stopToken.own(std::move(token));
        promise.started = true;
        return task.handle;
    }

    template <class T> static bool isDone(Task<T> const& task) noexcept {
        return task.handle && task.handle.done();
    }

    template <class T> static T takeResult(Task<T>& task) {
        CO2_CONTRACT_CHECK(task.handle && task.handle.done());
        return task.handle.promise().result();
    }

    // 根取走结果（只可一次）：已停止是空的 optional，错误在这里变回异常。
    template <class T> static RootResult<T> takeRootResult(Task<T>& task) {
        CO2_CONTRACT_CHECK(task.handle && task.handle.done());
        if (task.handle.promise().state == TaskState::stopped) return RootResult<T>{};
        return takeRootResult(task, std::is_void<T>{});
    }

    template <class T> static bool hasError(Task<T> const& task) noexcept {
        return task.handle && task.handle.promise().state == TaskState::error;
    }

    template <class T> static void rethrowError(Task<T>& task) {
        CO2_CONTRACT_CHECK(hasError(task));
        task.handle.promise().storage.error.raise();
    }

    // 交出帧的所有权（类型擦除）：调用方负责在完成后 destroy()。
    template <class T> static coroutine_handle<> release(Task<T>& task) noexcept {
        auto const handle = task.handle;
        task.handle = nullptr;
        return handle;
    }

  private:
    template <class T>
    static RootResult<T> takeRootResult(Task<T>& task, std::false_type) {
        return RootResult<T>{takeResult(task)};
    }

    static RootResult<void> takeRootResult(Task<void>& task, std::true_type) {
        takeResult(task);
        return RootResult<void>{Unit{}};
    }
};

} // namespace detail
} // namespace co2
