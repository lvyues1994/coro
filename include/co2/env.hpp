#pragma once

#include <type_traits>
#include <utility>

#include "co2/coroutine_handle.hpp"
#include "co2/stop_token.hpp"

// co2：协程环境查询（std::execution 的 read_env(get_stop_token) 在 co2 里的拼写）。
//
//   CO2_AWAIT_SET(token, co2::getStopToken());   // 取当前 Task 的 stop_token
//
// 环境沿结构化的等待链传播：Task 等待子 Task 时，子 promise 从父 promise 继承
// stop_token；根（syncWait、spawn）提供初始环境。promise 通过 get_stop_token() 成员
// 参与传播；没有它的 promise（自定义根）向子协程提供一个 stop_possible() 为假的 token。

namespace co2 {

struct GetStopToken {};

inline GetStopToken getStopToken() noexcept { return GetStopToken{}; }

namespace detail {

template <class Promise>
auto stopTokenOfPromise(Promise& promise, int) noexcept
    -> decltype(static_cast<stop_token>(promise.get_stop_token())) {
    return promise.get_stop_token();
}

template <class Promise> stop_token stopTokenOfPromise(Promise&, long) noexcept {
    return stop_token{};
}

template <class Promise>
stop_token stopTokenOfHandle(coroutine_handle<Promise> const handle,
                             std::false_type) noexcept {
    return stopTokenOfPromise(handle.promise(), 0);
}

template <class Promise>
stop_token stopTokenOfHandle(coroutine_handle<Promise>, std::true_type) noexcept {
    return stop_token{};
}

// 等待者的 stop_token：类型擦除的 coroutine_handle<> 没有 promise，得到空 token。
template <class Promise>
stop_token stopTokenOf(coroutine_handle<Promise> const handle) noexcept {
    return stopTokenOfHandle(handle, std::is_void<Promise>{});
}

// co_await getStopToken() 的 awaiter：不挂起，返回 promise 持有的 token。
struct StopTokenAwaiter {
    stop_token token;

    bool await_ready() const noexcept { return true; }
    void await_suspend(coroutine_handle<>) const noexcept {}
    stop_token await_resume() noexcept { return std::move(token); }
};

} // namespace detail
} // namespace co2
