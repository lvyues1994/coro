#pragma once

#include <exception>
#include <type_traits>
#include <utility>

#include "co2/contract.hpp"
#include "co2/coroutine.hpp"
#include "co2/detail/result_storage.hpp"
#include "co2/env.hpp"
#include "co2/stop_token.hpp"

// co2：惰性、单消费者的 Task<T>。promise 与 cppcoro::task 逐字同形：
//   initial_suspend = suspend_always；等待者在 await_suspend 里记下自己的句柄并对称
//   转移进 child；child 的 final_suspend 对称转移回等待者。惰性启动让"完成 vs 挂起"
//   没有竞争窗口，因此 Task 不需要任何原子量。
//
// 环境：promise 持有一个 stop_token。等待子 Task 时子 promise 从父 promise 继承它
// （await_suspend 收到的是带类型的 coroutine_handle<ParentPromise>，与 std::execution
// 的 task 查询父环境的方式相同）；根由 syncWait / spawn 提供。体内用
// CO2_AWAIT_SET(token, co2::getStopToken()) 读取。
//
// 销毁契约（标准规则）：一个已经启动、尚未完成的 Task 不能销毁——它挂起在某个操作
// 上，而操作只有完成后才能销毁其状态。co2 把这条未定义行为报告为契约违规。取消是
// 协作式的：请求 stop → 操作提前完成 → Task 走到 final suspend → owner 销毁。

namespace co2 {

template <class T = void> struct Task;

namespace detail {

struct TaskAccess;

template <class T> struct TaskPromise {
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

    template <class Value, class U = T,
              class = typename std::enable_if<not std::is_void<U>::value>::type>
    void return_value(Value&& value) {
        storage.emplace(std::forward<Value>(value));
    }

    template <class U = T,
              class = typename std::enable_if<std::is_void<U>::value>::type>
    void return_void() noexcept {
        storage.emplace();
    }

    void unhandled_exception() noexcept { error = std::current_exception(); }

    // ---- 环境 ----

    stop_token get_stop_token() const noexcept { return stopToken; }

    // co_await getStopToken()：不挂起，交出本 Task 的 token。
    StopTokenAwaiter await_transform(GetStopToken) noexcept {
        return StopTokenAwaiter{stopToken};
    }

    // 其余 awaitable 原样等待（标准：promise 一旦有 await_transform 就必须覆盖全部）。
    template <class Awaitable>
    Awaitable&& await_transform(Awaitable&& awaitable) noexcept {
        return std::forward<Awaitable>(awaitable);
    }

    // 只可消费一次：值被移出。
    T result() {
        if (error) std::rethrow_exception(error);
        CO2_CONTRACT_CHECK(storage.hasValue());
        return storage.take();
    }

    coroutine_handle<> continuation;
    std::exception_ptr error;
    ResultStorage<T> storage;
    stop_token stopToken;
    bool started{};
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
        promise.stopToken = stopTokenOf(awaiting);
        promise.started = true;
        return handle;
    }

    T await_resume() {
        CO2_CONTRACT_CHECK(handle);
        return handle.promise().result();
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
        promise.stopToken = std::move(token);
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

    template <class T> static bool hasError(Task<T> const& task) noexcept {
        return task.handle && task.handle.promise().error != nullptr;
    }

    template <class T> static void rethrowError(Task<T>& task) {
        CO2_CONTRACT_CHECK(hasError(task));
        std::rethrow_exception(task.handle.promise().error);
    }

    // 交出帧的所有权（类型擦除）：调用方负责在完成后 destroy()。
    template <class T> static coroutine_handle<> release(Task<T>& task) noexcept {
        auto const handle = task.handle;
        task.handle = nullptr;
        return handle;
    }
};

} // namespace detail
} // namespace co2
