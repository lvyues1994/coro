#pragma once

#include <condition_variable>
#include <mutex>
#include <type_traits>
#include <utility>

#include "co2/contract.hpp"
#include "co2/coroutine_handle.hpp"
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

} // namespace co2
