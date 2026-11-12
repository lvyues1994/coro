#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "co2/completion.hpp"
#include "co2/contract.hpp"
#include "co2/coroutine_handle.hpp"
#include "co2/detail/late_init.hpp"
#include "co2/env.hpp"
#include "co2/stop_token.hpp"
#include "co2/task.hpp"

// co2：whenAll——并发启动一组 Task，全部完成后一起交付结果。语义取自
// std::execution::when_all：
//
//   - 值通道拼接：whenAll(Task<int>, Task<void>, Task<string>) 的结果是
//     std::tuple<int, std::string>；全部 void 时结果是 void；vector<Task<T>> 给出
//     vector<T>（T 为 void 时 void）。
//   - 任一 child 出错即向其余 child 请求停止（whenAll 自己的 stop_source），等**全部**
//     child 结束后以**时间上第一个**错误完成。
//   - 没有 child 出错、但有 child 已停止：同样向其余 child 请求停止，等全部结束后以
//     已停止完成。出错优先于已停止。
//   - 父的 stop_token 转发给 child。
//
// 后两种结果经 await_completion() 按值交给等待它的协程（见 completion-channels.md）；
// 不认识这个成员的调用方直接调用 await_resume()，错误在那里抛出。
//
// 实现：awaiter 自身就是操作状态。每个 child 一个内嵌的手写续体帧（标准布局，不占堆），
// child 的 FinalAwaiter 对称转移到它；计数初值 N+1，启动方最后放下自己的一份——
// 全部 child 同步完成时 await_suspend 返回 false 不挂起，否则最后到达的 child 把等待者
// 作为对称转移目标恢复。tuple 版本除 child 帧与 stop 状态外零分配。

namespace co2 {
namespace detail {

struct WhenAllState {
    static constexpr std::size_t noFailure = ~std::size_t{0};

    WhenAllState() = default;

    // 只允许在启动前移动（awaiter 被移进等待者的帧）。
    WhenAllState(WhenAllState&& other) noexcept : source{std::move(other.source)} {
        CO2_CONTRACT_CHECK(not other.awaiting);
    }

    WhenAllState(WhenAllState const&) = delete;
    WhenAllState& operator=(WhenAllState const&) = delete;
    WhenAllState& operator=(WhenAllState&&) = delete;

    void begin(coroutine_handle<> const awaiting_, stop_token parent,
               std::size_t const count) {
        awaiting = awaiting_;
        remaining.store(count + 1U, std::memory_order_relaxed);
        if (parent.stop_possible())
            forwarding.emplace(std::move(parent), ForwardStop{&source});
    }

    // child 结束：出错则记下时间上第一个出错者，已停止则记一笔；两种情况都请求兄弟
    // 停止。最后到达者拿到等待者。
    coroutine_handle<> childCompleted(std::size_t const index,
                                      TaskState const outcome) noexcept {
        if (outcome == TaskState::error) {
            auto expected = noFailure;
            firstFailure.compare_exchange_strong(expected, index,
                                                 std::memory_order_acq_rel);
            source.request_stop();
        } else if (outcome == TaskState::stopped) {
            stopped.store(true, std::memory_order_release);
            source.request_stop();
        }
        if (remaining.fetch_sub(1U, std::memory_order_acq_rel) == 1U) return awaiting;
        return nullptr;
    }

    // 启动方放下自己的那一份计数：返回 true 表示 child 全部已经完成，不必挂起。
    bool finishStart() noexcept {
        return remaining.fetch_sub(1U, std::memory_order_acq_rel) == 1U;
    }

    std::size_t failureIndex() const noexcept {
        return firstFailure.load(std::memory_order_acquire);
    }

    bool anyStopped() const noexcept { return stopped.load(std::memory_order_acquire); }

    std::atomic<std::size_t> remaining{0U};
    std::atomic<std::size_t> firstFailure{noFailure};
    std::atomic<bool> stopped{false};
    coroutine_handle<> awaiting;
    stop_source source;
    LateInit<stop_callback<ForwardStop>> forwarding; // 晚于 source 构造、先于它析构
};

template <class T> TaskState whenAllTaskState(void* const task) noexcept {
    return TaskAccess::stateOf(*static_cast<Task<T>*>(task));
}

// 每个 child 的续体帧：标准布局，首成员是帧头。
struct WhenAllChild {
    FrameHeader header;
    WhenAllState* state;
    void* task;
    TaskState (*outcome)(void*) noexcept;
    std::size_t index;

    coroutine_handle<> handle() noexcept {
        return coroutine_handle<>::from_address(&header);
    }

    static FrameHeader* complete(FrameHeader* const header) {
        static_assert(std::is_standard_layout<WhenAllChild>::value,
                      "the completion frame must stay standard-layout");
        auto* const self = reinterpret_cast<WhenAllChild*>(header);
        auto const next =
            self->state->childCompleted(self->index, self->outcome(self->task));
        return next ? HandleAccess::header(next) : nullptr;
    }
};

template <class T>
void startWhenAllChild(WhenAllChild& child, WhenAllState& state, Task<T>& task,
                       std::size_t const index) {
    CO2_CONTRACT_CHECK(task);
    child =
        WhenAllChild{FrameHeader{&noopDestroy, &WhenAllChild::complete, false, false},
                     &state, &task, &whenAllTaskState<T>, index};
    TaskAccess::arm(task, child.handle(), state.source.get_token()).resume();
}

// ---- 结果类型：值通道拼接 ----

template <class T> struct WhenAllValueTuple { using type = std::tuple<T>; };
template <> struct WhenAllValueTuple<void> { using type = std::tuple<>; };

template <class Tuple> struct VoidIfEmptyTuple { using type = Tuple; };
template <> struct VoidIfEmptyTuple<std::tuple<>> { using type = void; };

template <class... T>
using WhenAllTupleResult = typename VoidIfEmptyTuple<decltype(std::tuple_cat(
    std::declval<typename WhenAllValueTuple<T>::type>()...))>::type;

template <class T> std::tuple<T> takeAsTuple(Task<T>& task) {
    return std::tuple<T>(TaskAccess::takeResult(task));
}

inline std::tuple<> takeAsTuple(Task<void>& task) {
    TaskAccess::takeResult(task);
    return std::tuple<>{};
}

template <class... T> struct WhenAllTuple {
    using Result = WhenAllTupleResult<T...>;
    static constexpr std::size_t count = sizeof...(T);

    explicit WhenAllTuple(Task<T>... tasks_) : tasks{std::move(tasks_)...} {}

    WhenAllTuple(WhenAllTuple&&) = default;
    WhenAllTuple(WhenAllTuple const&) = delete;
    WhenAllTuple& operator=(WhenAllTuple const&) = delete;
    WhenAllTuple& operator=(WhenAllTuple&&) = delete;

    bool await_ready() const noexcept { return count == 0U; }

    template <class Parent>
    bool await_suspend(coroutine_handle<Parent> const awaiting) {
        state.begin(awaiting, stopTokenOf(awaiting), count);
        startAll(std::index_sequence_for<T...>{});
        return not state.finishStart();
    }

    Result await_resume() {
        rethrowFailure(std::index_sequence_for<T...>{});
        return collect(std::is_void<Result>{}, std::index_sequence_for<T...>{});
    }

    // 全部 child 结束之后被问到。出错优先于已停止；错误从第一个出错的 child 那里取走。
    Completion await_completion() noexcept {
        return completionOfAll(std::index_sequence_for<T...>{});
    }

  private:
    template <std::size_t... Index>
    Completion completionOfAll(std::index_sequence<Index...>) noexcept {
        auto const failure = state.failureIndex();
        if (failure == WhenAllState::noFailure)
            return state.anyStopped() ? Completion::stopped() : Completion::value();
        auto completion = Completion::value();
        int const ordered[] = {
            0, (Index == failure
                    ? (completion = TaskAccess::completionOf(std::get<Index>(tasks)), 0)
                    : 0)...};
        static_cast<void>(ordered);
        return completion;
    }

    template <std::size_t... Index> void startAll(std::index_sequence<Index...>) {
        // 花括号初始化列表保证从左到右求值：child 按参数顺序启动。
        int const ordered[] = {0, (startWhenAllChild(children[Index], state,
                                                     std::get<Index>(tasks), Index),
                                   0)...};
        static_cast<void>(ordered);
    }

    template <std::size_t... Index> void rethrowFailure(std::index_sequence<Index...>) {
        auto const failure = state.failureIndex();
        if (failure == WhenAllState::noFailure) return;
        int const ordered[] = {
            0, (Index == failure ? (TaskAccess::rethrowError(std::get<Index>(tasks)), 0)
                                 : 0)...};
        static_cast<void>(ordered);
    }

    template <std::size_t... Index>
    Result collect(std::false_type, std::index_sequence<Index...>) {
        return std::tuple_cat(takeAsTuple(std::get<Index>(tasks))...);
    }

    template <std::size_t... Index>
    void collect(std::true_type, std::index_sequence<Index...>) {
        int const ordered[] = {0,
                               (TaskAccess::takeResult(std::get<Index>(tasks)), 0)...};
        static_cast<void>(ordered);
    }

    std::tuple<Task<T>...> tasks;
    WhenAllState state;
    std::array<WhenAllChild, count> children; // 启动时填充
};

template <class T> struct WhenAllRangeResult { using type = std::vector<T>; };
template <> struct WhenAllRangeResult<void> { using type = void; };

template <class T> struct WhenAllRange {
    using Result = typename WhenAllRangeResult<T>::type;

    explicit WhenAllRange(std::vector<Task<T>> tasks_)
        : tasks{std::move(tasks_)}, children(tasks.size()) {}

    WhenAllRange(WhenAllRange&&) = default;
    WhenAllRange(WhenAllRange const&) = delete;
    WhenAllRange& operator=(WhenAllRange const&) = delete;
    WhenAllRange& operator=(WhenAllRange&&) = delete;

    bool await_ready() const noexcept { return tasks.empty(); }

    template <class Parent>
    bool await_suspend(coroutine_handle<Parent> const awaiting) {
        state.begin(awaiting, stopTokenOf(awaiting), tasks.size());
        for (auto index = std::size_t{}; index != tasks.size(); ++index)
            startWhenAllChild(children[index], state, tasks[index], index);
        return not state.finishStart();
    }

    Result await_resume() {
        auto const failure = state.failureIndex();
        if (failure != WhenAllState::noFailure)
            TaskAccess::rethrowError(tasks[failure]);
        return collect(std::is_void<Result>{});
    }

    Completion await_completion() noexcept {
        auto const failure = state.failureIndex();
        if (failure != WhenAllState::noFailure)
            return TaskAccess::completionOf(tasks[failure]);
        return state.anyStopped() ? Completion::stopped() : Completion::value();
    }

  private:
    Result collect(std::false_type) {
        auto results = Result{};
        results.reserve(tasks.size());
        for (auto& task : tasks)
            results.push_back(TaskAccess::takeResult(task));
        return results;
    }

    void collect(std::true_type) {
        for (auto& task : tasks)
            TaskAccess::takeResult(task);
    }

    std::vector<Task<T>> tasks;
    WhenAllState state;
    std::vector<WhenAllChild> children;
};

} // namespace detail

// 并发运行全部 Task，全部完成后按参数顺序交付非 void 结果的 tuple（全部 void 则为
// void）。任一 child 出错或已停止即向其余 child 请求停止；全部结束后，有 child 出错就
// 以时间上第一个错误完成，否则有 child 已停止就以已停止完成。
template <class... T> detail::WhenAllTuple<T...> whenAll(Task<T>... tasks) {
    return detail::WhenAllTuple<T...>{std::move(tasks)...};
}

// 同上，同构的一组 Task：结果是 std::vector<T>（T 为 void 时 void）。
template <class T> detail::WhenAllRange<T> whenAll(std::vector<Task<T>> tasks) {
    return detail::WhenAllRange<T>{std::move(tasks)};
}

} // namespace co2
