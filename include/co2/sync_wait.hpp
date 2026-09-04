#pragma once

#include <condition_variable>
#include <mutex>
#include <type_traits>
#include <utility>

#include "co2/contract.hpp"
#include "co2/coroutine.hpp"
#include "co2/coroutine_handle.hpp"
#include "co2/detail/result_storage.hpp"
#include "co2/task.hpp"

namespace co2 {
namespace detail {

struct SyncWaitState {
    std::mutex mutex;
    std::condition_variable wakeup;
    bool finished{};

    void markFinished() noexcept {
        // 在锁内通知：state 住在 syncWait 的栈上，等待线程一旦看到 finished
        // 就会销毁它。
        std::lock_guard<std::mutex> lock{mutex};
        finished = true;
        wakeup.notify_one();
    }

    void wait() {
        std::unique_lock<std::mutex> lock{mutex};
        wakeup.wait(lock, [this] { return finished; });
    }
};

// Task 的续体：一个手写的、不占堆的协程帧——被 resume 时标记完成并把控制交回 resumer。
// 标准布局，FrameHeader 是首成员，因此句柄地址就是帧地址。
struct SyncWaitCompletion {
    FrameHeader header;
    SyncWaitState* state;

    explicit SyncWaitCompletion(SyncWaitState& state_) noexcept
        : header{&noopDestroy, &complete, false, false}, state{&state_} {}

    coroutine_handle<> handle() noexcept {
        return coroutine_handle<>::from_address(&header);
    }

    static FrameHeader* complete(FrameHeader* const header) {
        static_assert(std::is_standard_layout<SyncWaitCompletion>::value,
                      "the completion frame must stay standard-layout");
        reinterpret_cast<SyncWaitCompletion*>(header)->state->markFinished();
        return nullptr;
    }
};

} // namespace detail

// 在调用线程上阻塞直到 Task 完成，返回其结果或重抛其异常。Task 在调用线程上内联启动；
// 之后它在哪里运行由它自己的 scheduleOn 决定，完成时通过对称转移唤醒等待线程。
// `token` 成为 Task 树的根 stop_token（体内 CO2_AWAIT_SET(t, getStopToken()) 可见）。
//
// 与 std::execution::sync_wait 一样，它就是阻塞：在 scheduler 的工作线程上调用会占住
// 该线程，Task 若依赖这个线程就会死锁。
template <class T> T syncWait(Task<T> task, stop_token token = {}) {
    CO2_CONTRACT_CHECK(task);
    detail::SyncWaitState state;
    detail::SyncWaitCompletion completion{state};
    detail::TaskAccess::start(task, completion.handle(), std::move(token));
    state.wait();
    return detail::TaskAccess::takeResult(task);
}

namespace detail {

template <class T> struct IsTask : std::false_type {};
template <class T> struct IsTask<Task<T>> : std::true_type {};

// awaitable 的 await_resume() 类型（不经过任何 await_transform）。
template <class Awaitable>
using AwaitResultOf = decltype(std::declval<AwaiterOf<Awaitable>&>().await_resume());

// 把任意 awaitable 包成 Task：syncWait(whenAll(...)) 这类用法多一个帧。
template <class R, class Awaitable>
auto awaitableToTask(Awaitable awaitable)
    CO2_BEG((Task<R>), (awaitable), ResultStorage<R> result;) {
    CO2_AWAIT_SET(result, std::move(awaitable));
    CO2_RETURN(result.take());
}
CO2_END

template <class R, class Awaitable>
auto awaitableToVoidTask(Awaitable awaitable) CO2_BEG(Task<void>, (awaitable)) {
    CO2_AWAIT(std::move(awaitable));
}
CO2_END

template <class R, class Awaitable>
Task<R> wrapAwaitable(Awaitable&& awaitable, std::false_type) {
    return awaitableToTask<R, typename std::decay<Awaitable>::type>(
        std::forward<Awaitable>(awaitable));
}

template <class R, class Awaitable>
Task<void> wrapAwaitable(Awaitable&& awaitable, std::true_type) {
    return awaitableToVoidTask<R, typename std::decay<Awaitable>::type>(
        std::forward<Awaitable>(awaitable));
}

} // namespace detail

// 任意 awaitable 的同步等待：在一个临时 Task 里 co_await 它。
template <class Awaitable, class = typename std::enable_if<not detail::IsTask<
                               typename std::decay<Awaitable>::type>::value>::type>
detail::AwaitResultOf<Awaitable> syncWait(Awaitable&& awaitable,
                                          stop_token token = {}) {
    using Result = detail::AwaitResultOf<Awaitable>;
    return syncWait(detail::wrapAwaitable<Result>(std::forward<Awaitable>(awaitable),
                                                  std::is_void<Result>{}),
                    std::move(token));
}

} // namespace co2
