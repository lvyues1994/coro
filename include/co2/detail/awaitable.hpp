#pragma once

#include <type_traits>
#include <utility>

#include "co2/coroutine_handle.hpp"

// [expr.await] 的库内实现：operator co_await 查找、await_transform、以及按
// await_suspend 返回类型分派的挂起。

namespace co2 {
namespace detail {

template <class T> struct IsCoroutineHandle : std::false_type {};
template <class P> struct IsCoroutineHandle<coroutine_handle<P>> : std::true_type {};

// ---- operator co_await 查找 ----
//
// C++14 没有 co_await 关键字，`operator co_await` 无法拼写；co2 以 `operator_co_await`
// 作为同义拼写（成员或 ADL 非成员）。C++20 构建下两种拼写都会被查找，因此为 std::
// 写的、带 operator co_await 的 awaitable 也能直接等待。

// 重载优先级标签：Rank<N> 派生自 Rank<N-1>，重载决议选择最派生的可行候选。
template <int N> struct Rank : Rank<N - 1> {};
template <> struct Rank<0> {};

template <class Awaitable>
auto toAwaiter(Awaitable&& awaitable, Rank<4>)
    -> decltype(std::forward<Awaitable>(awaitable).operator_co_await()) {
    return std::forward<Awaitable>(awaitable).operator_co_await();
}

template <class Awaitable>
auto toAwaiter(Awaitable&& awaitable, Rank<3>)
    -> decltype(operator_co_await(std::forward<Awaitable>(awaitable))) {
    return operator_co_await(std::forward<Awaitable>(awaitable));
}

#if defined(__cpp_impl_coroutine)
template <class Awaitable>
auto toAwaiter(Awaitable&& awaitable, Rank<2>)
    -> decltype(std::forward<Awaitable>(awaitable).operator co_await()) {
    return std::forward<Awaitable>(awaitable).operator co_await();
}

template <class Awaitable>
auto toAwaiter(Awaitable&& awaitable, Rank<1>)
    -> decltype(operator co_await(std::forward<Awaitable>(awaitable))) {
    return operator co_await(std::forward<Awaitable>(awaitable));
}
#endif

template <class Awaitable> Awaitable&& toAwaiter(Awaitable&& awaitable, Rank<0>) {
    return std::forward<Awaitable>(awaitable);
}

// awaitable → awaiter。查找顺序与标准一致：成员 operator co_await，非成员
// operator co_await，否则 awaitable 本身就是 awaiter。
template <class Awaitable>
auto getAwaiter(Awaitable&& awaitable)
    -> decltype(toAwaiter(std::forward<Awaitable>(awaitable), Rank<4>{})) {
    return toAwaiter(std::forward<Awaitable>(awaitable), Rank<4>{});
}

// awaiter 的存储类型：prvalue 结果按值保存；lvalue awaitable 也会被复制/移动进帧
// （见设计稿 D9：标准会直接使用 lvalue 本身）。
template <class Awaitable>
using AwaiterOf =
    typename std::decay<decltype(getAwaiter(std::declval<Awaitable>()))>::type;

// ---- await_transform ----

template <class Promise, class Awaitable>
auto awaitTransform(Promise& promise, Awaitable&& awaitable, int)
    -> decltype(promise.await_transform(std::forward<Awaitable>(awaitable))) {
    return promise.await_transform(std::forward<Awaitable>(awaitable));
}

template <class Promise, class Awaitable>
Awaitable&& awaitTransform(Promise&, Awaitable&& awaitable, long) {
    return std::forward<Awaitable>(awaitable);
}

// 由 co_await 表达式产生的 awaitable 先经过 await_transform（若 promise 提供）。
// yield/initial/final 隐式产生的 awaitable 不经过它，见 [expr.await]/3.2。
template <class Promise, class Awaitable>
auto transformAwaitable(Promise& promise, Awaitable&& awaitable)
    -> decltype(awaitTransform(promise, std::forward<Awaitable>(awaitable), 0)) {
    return awaitTransform(promise, std::forward<Awaitable>(awaitable), 0);
}

template <class Promise, class Awaitable>
using TransformedAwaitable =
    decltype(transformAwaitable(std::declval<Promise&>(), std::declval<Awaitable>()));

// `co_await e` 最终使用的 awaiter 类型。
template <class Promise, class Awaitable>
using AwaiterFor = AwaiterOf<TransformedAwaitable<Promise, Awaitable>>;

// ---- 按 await_suspend 返回类型分派 ----

struct SuspendOutcome {
    bool suspended;
    FrameHeader* next; // 对称转移目标；空表示把控制交回 resumer
};

template <class Result> struct SuspendKind;

template <> struct SuspendKind<void> {
    template <class Awaiter, class Promise>
    static SuspendOutcome apply(Awaiter& awaiter,
                                coroutine_handle<Promise> const handle) {
        awaiter.await_suspend(handle);
        return SuspendOutcome{true, nullptr};
    }
};

template <> struct SuspendKind<bool> {
    template <class Awaiter, class Promise>
    static SuspendOutcome apply(Awaiter& awaiter,
                                coroutine_handle<Promise> const handle) {
        // 返回 false：不算挂起，协程立即恢复。
        return SuspendOutcome{awaiter.await_suspend(handle), nullptr};
    }
};

template <class Target> struct SuspendKind<coroutine_handle<Target>> {
    template <class Awaiter, class Promise>
    static SuspendOutcome apply(Awaiter& awaiter,
                                coroutine_handle<Promise> const handle) {
        coroutine_handle<> const next = awaiter.await_suspend(handle);
        // 对空句柄 resume() 是未定义行为，这里报告为契约违规。
        CO2_CONTRACT_CHECK(next);
        return SuspendOutcome{true, HandleAccess::header(next)};
    }
};

// 调用 await_suspend 并解释其返回值。返回类型只能是 void、bool 或 coroutine_handle<Z>。
template <class Awaiter, class Promise>
SuspendOutcome suspendWith(Awaiter& awaiter, coroutine_handle<Promise> const handle) {
    using Result = decltype(awaiter.await_suspend(handle));
    static_assert(std::is_void<Result>::value || std::is_same<Result, bool>::value ||
                      IsCoroutineHandle<Result>::value,
                  "await_suspend must return void, bool, or coroutine_handle<Z>");
    return SuspendKind<Result>::apply(awaiter, handle);
}

} // namespace detail
} // namespace co2
