#pragma once

#include <condition_variable>
#include <memory>
#include <mutex>
#include <type_traits>
#include <utility>

#include "co2/contract.hpp"
#include "co2/coroutine_handle.hpp"
#include "co2/detail/late_init.hpp"
#include "co2/scheduler.hpp"
#include "co2/stop_token.hpp"
#include "co2/task.hpp"

// co2：spawn——把一个惰性 Task 作为独立的根交给 scheduler 启动，返回 JoinHandle<T>。
//
//   auto handle = co2::spawn(pool, work());
//   ... handle.join() / CO2_AWAIT_SET(v, handle) / handle.requestStop()
//
// 每个 spawn 的根有自己的 stop_source：JoinHandle::requestStop() 请求它；传入父 token
// 时父的请求会转发过来。JoinHandle 析构而 Task 尚未完成时——与 P3149 的 spawn_future
// 一致——请求停止并分离：Task 跑完后其帧由共享状态销毁，结果与异常丢弃。Task 已完成
// 的 JoinHandle 析构只是销毁帧。
//
// 完成时等待中的协程在完成线程上被对称转移恢复（标准行为）；join() 阻塞调用线程。

namespace co2 {

template <class T> struct JoinHandle;

namespace detail {

struct JoinState;

// 续体帧（标准布局，不占堆）：spawn 的 Task 完成时对称转移到它。
struct JoinCompletion {
    FrameHeader header;
    JoinState* state;

    explicit JoinCompletion(JoinState& state_) noexcept
        : header{&noopDestroy, &complete, false, false}, state{&state_} {}

    coroutine_handle<> handle() noexcept {
        return coroutine_handle<>::from_address(&header);
    }

    static FrameHeader* complete(FrameHeader* header);
};

// JoinHandle 与完成帧共享的状态。两方都可能是最后离开的一方：Task 先完成则 JoinHandle
// 析构时释放；JoinHandle 先析构（分离）则完成时释放。
struct JoinState {
    explicit JoinState(stop_token parent) {
        if (parent.stop_possible())
            forwarding.emplace(std::move(parent), ForwardStop{&source});
    }

    JoinState(JoinState const&) = delete;
    JoinState& operator=(JoinState const&) = delete;

    JoinCompletion completion{*this};
    std::mutex mutex;
    std::condition_variable completed;
    coroutine_handle<> waiter;
    coroutine_handle<> detachedFrame;
    bool ready{};
    bool detached{};
    stop_source source;
    // stop_callback 不可移动，用 LateInit（与 WhenAllState 一致）而不是 ResultStorage：
    // 后者的移动构造对它无法实例化。必须晚于 source 构造、先于它析构。
    LateInit<stop_callback<ForwardStop>> forwarding;
};

inline FrameHeader* JoinCompletion::complete(FrameHeader* const header) {
    static_assert(std::is_standard_layout<JoinCompletion>::value,
                  "the completion frame must stay standard-layout");
    auto* const state = reinterpret_cast<JoinCompletion*>(header)->state;
    auto waiter = coroutine_handle<>{};
    auto destroyNow = false;
    {
        // 在锁内通知：JoinHandle 一旦看到 ready 就可能释放 state。
        std::lock_guard<std::mutex> lock{state->mutex};
        state->ready = true;
        waiter = state->waiter;
        state->waiter = nullptr;
        destroyNow = state->detached;
        state->completed.notify_all();
    }
    if (destroyNow) {
        state->detachedFrame.destroy();
        delete state;
        return nullptr;
    }
    return waiter ? HandleAccess::header(waiter) : nullptr;
}

struct JoinHandleAccess;

// 等待 JoinHandle：不拥有任何东西。就绪判断放在 await_suspend 里，只拿一次锁。
template <class T> struct JoinAwaiter {
    JoinState* state;
    Task<T>* task;

    bool await_ready() const noexcept { return false; }

    bool await_suspend(coroutine_handle<> const awaiting) noexcept {
        std::lock_guard<std::mutex> lock{state->mutex};
        if (state->ready) return false;
        CO2_CONTRACT_CHECK(not state->waiter);
        state->waiter = awaiting;
        return true;
    }

    T await_resume() { return TaskAccess::takeResult(*task); }
};

} // namespace detail

template <class T> struct JoinHandle {
    JoinHandle() noexcept = default;

    JoinHandle(JoinHandle&& other) noexcept
        : task{std::move(other.task)}, state{other.state} {
        other.state = nullptr;
    }

    JoinHandle& operator=(JoinHandle&& other) noexcept {
        if (this == &other) return *this;
        reset();
        task = std::move(other.task);
        state = other.state;
        other.state = nullptr;
        return *this;
    }

    JoinHandle(JoinHandle const&) = delete;
    JoinHandle& operator=(JoinHandle const&) = delete;

    ~JoinHandle() { reset(); }

    explicit operator bool() const noexcept { return state != nullptr; }

    bool isReady() const noexcept {
        CO2_CONTRACT_CHECK(state != nullptr);
        std::lock_guard<std::mutex> lock{state->mutex};
        return state->ready;
    }

    // 向 spawn 的 Task 请求停止（协作式）。
    bool requestStop() noexcept {
        CO2_CONTRACT_CHECK(state != nullptr);
        return state->source.request_stop();
    }

    stop_token getStopToken() const noexcept {
        CO2_CONTRACT_CHECK(state != nullptr);
        return state->source.get_token();
    }

    // 阻塞到 Task 完成并取走结果（只可一次）。在 scheduler
    // 的工作线程上调用会占住该线程。
    T join() {
        CO2_CONTRACT_CHECK(state != nullptr);
        {
            std::unique_lock<std::mutex> lock{state->mutex};
            state->completed.wait(lock, [this] { return state->ready; });
        }
        return detail::TaskAccess::takeResult(task);
    }

    // ---- 等待：右值时 JoinHandle 本身是 awaiter（拥有），左值时借用 ----

    bool await_ready() noexcept { return awaiter().await_ready(); }

    bool await_suspend(coroutine_handle<> const awaiting) noexcept {
        return awaiter().await_suspend(awaiting);
    }

    T await_resume() { return awaiter().await_resume(); }

    detail::JoinAwaiter<T> operator_co_await() & noexcept { return awaiter(); }

  private:
    JoinHandle(Task<T> task_, detail::JoinState* const state_) noexcept
        : task{std::move(task_)}, state{state_} {}

    detail::JoinAwaiter<T> awaiter() noexcept {
        CO2_CONTRACT_CHECK(state != nullptr);
        return detail::JoinAwaiter<T>{state, &task};
    }

    void reset() noexcept {
        if (state == nullptr) return;
        // 先在锁外请求停止：回调可能同步把 Task 推到完成（complete() 需要拿锁）。
        state->source.request_stop();
        auto destroyNow = false;
        {
            std::lock_guard<std::mutex> lock{state->mutex};
            // 借用本 handle 的等待者还挂着：销毁 handle 会让它恢复时访问已销毁的对象。
            CO2_CONTRACT_CHECK(not state->waiter);
            if (state->ready) {
                destroyNow = true;
            } else {
                state->detached = true;
                state->detachedFrame = detail::TaskAccess::release(task);
            }
        }
        if (destroyNow) {
            task = Task<T>{};
            delete state;
        }
        state = nullptr;
    }

    Task<T> task;
    detail::JoinState* state{};

    friend struct detail::JoinHandleAccess;
};

namespace detail {

struct JoinHandleAccess {
    template <class T>
    static JoinHandle<T> create(Task<T> task, JoinState* const state) noexcept {
        return JoinHandle<T>{std::move(task), state};
    }
};

} // namespace detail

// 消费惰性 Task，把它排到 scheduler 上启动。`parent` 的停止请求会转发给这个根。
template <class T>
JoinHandle<T> spawn(Scheduler& scheduler, Task<T> task, stop_token parent = {}) {
    CO2_CONTRACT_CHECK(task);
    auto state =
        std::unique_ptr<detail::JoinState>{new detail::JoinState{std::move(parent)}};
    auto const coroutine = detail::TaskAccess::arm(task, state->completion.handle(),
                                                   state->source.get_token());
    auto handle = detail::JoinHandleAccess::create(std::move(task), state.release());
    scheduler.schedule(coroutine);
    return handle;
}

} // namespace co2
