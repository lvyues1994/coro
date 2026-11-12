#pragma once

#include <utility>

#include "co2/config.hpp"
#include "co2/coroutine_handle.hpp"
#include "co2/detail/awaitable.hpp"

// co2::unstoppable(awaitable)：对被包住的操作隐藏停止请求
// （std::execution::unstoppable 在 co2 里的拼写）。
//
//   CO2_AWAIT(co2::unstoppable(co2::scheduleOn(pool)));   // 停止请求之后仍然换线程
//   CO2_AWAIT(co2::unstoppable(flush(file)));             // flush 和它等待的一切都不被取消
//
// 用在收尾阶段：已经收到停止请求，但还有几步必须做完。它只隐藏外面的停止请求——被包住
// 的操作自己出错、或者自己以已停止结束，照样传给等待它的协程。
//
// 做法：把等待者的句柄类型擦除之后再交给内层的 await_suspend。环境是经带类型的句柄
// 查询的，内层于是拿不到等待者的停止令牌；被包住的是 Task 时，它和它的子 Task 的环境里
// 也就没有令牌。两点后果：
//   - 内层的 await_suspend 必须接受 coroutine_handle<>。
//   - 被隐藏的是整个环境，不只是停止令牌。std::execution::unstoppable 保留环境的其余
//     部分；今天环境里只有停止令牌，两者等价。
//
// 与 asResult 一样，unstoppable(x) 要直接写在等待它的地方。x 是右值时被搬进等待者的
// 帧；x 是左值时只记它的引用。

namespace co2 {
namespace detail {

template <class Inner> struct UnstoppableAwaiter {
    Inner inner;

    auto await_ready() -> decltype(std::declval<Inner&>().await_ready()) {
        return inner.await_ready();
    }

    template <class Promise>
    auto await_suspend(coroutine_handle<Promise> const awaiting)
        -> decltype(std::declval<Inner&>().await_suspend(coroutine_handle<>{})) {
        return inner.await_suspend(coroutine_handle<>{awaiting});
    }

    auto await_resume() -> decltype(std::declval<Inner&>().await_resume()) {
        return inner.await_resume();
    }

    // 内层有这个成员时才有：被包住的操作怎么结束的，原样转发。
    template <class I = Inner>
    auto await_completion() -> decltype(std::declval<I&>().await_completion()) {
        return inner.await_completion();
    }
};

} // namespace detail

// unstoppable(x) 的返回类型。Awaitable 对右值是值类型（拥有 x），对左值是引用类型。
template <class Awaitable> struct Unstoppable {
    Awaitable awaitable;

    detail::UnstoppableAwaiter<detail::AwaiterOf<Awaitable>> operator_co_await() && {
        return detail::UnstoppableAwaiter<detail::AwaiterOf<Awaitable>>{
            detail::AwaiterOf<Awaitable>{
                detail::getAwaiter(std::forward<Awaitable>(awaitable))}};
    }
};

template <class Awaitable> Unstoppable<Awaitable> unstoppable(Awaitable&& awaitable) {
    return Unstoppable<Awaitable>{std::forward<Awaitable>(awaitable)};
}

} // namespace co2
