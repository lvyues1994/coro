#pragma once

#include <utility>

#include "co2/completion.hpp"
#include "co2/config.hpp"
#include "co2/contract.hpp"
#include "co2/coroutine_handle.hpp"
#include "co2/env.hpp"
#include "co2/error.hpp"

// co2：在协程体里直接产生"错误"与"已停止"这两种结果。名字取自 P2300 与 P3552。
//
//   CO2_AWAIT(co2::just_error(MyErrc::retry_later));   // 当前协程以这个错误结束
//   CO2_YIELD(co2::with_error(MyErrc::retry_later));   // 同上，P3552 的另一种拼写
//   CO2_AWAIT(co2::just_stopped());                    // 当前协程以已停止结束
//   CO2_AWAIT(co2::stopIfRequested());                 // 取消点：有停止请求才停
//
// 结束的意思都一样：其后的语句不执行，局部析构，等待它的协程收到同样的结果。
//
// just_error 是普通的 awaitable，在任何 promise 提供了 return_error() 的协程里都能用；
// with_error 是一个标记值，由 promise 的 yield_value 接住，只在提供了这个重载的协程里
// 能写（Task、AsyncGenerator），两者效果相同。参数是 co2::Error：std::error_code 与
// 错误码枚举可以直接传；一个捕获的异常要显式写成 co2::Error{exceptionPointer}。
//
// just_stopped 与 stopIfRequested 需要 promise 提供 return_stopped()。stopIfRequested
// 看的是当前协程环境里的停止令牌（co2::getStopToken() 读到的那一个）：没有令牌的协程
// 里它什么也不做。

namespace co2 {
namespace detail {

// 立刻就绪、以错误结束的 awaiter。
struct JustError {
    Error error;

    bool await_ready() const noexcept { return true; }
    void await_suspend(coroutine_handle<>) const noexcept {}

    // 只有不认识 await_completion() 的调用方才会调到这里：错误以异常的形式出现。
    [[noreturn]] void await_resume() const { error.raise(); }

    Completion await_completion() noexcept { return Completion::error(std::move(error)); }
};

// 立刻就绪、以已停止结束的 awaiter。
struct JustStopped {
    bool await_ready() const noexcept { return true; }
    void await_suspend(coroutine_handle<>) const noexcept {}

    // 只有不认识 await_completion() 的调用方才会调到这里：已停止没有值可给。
    [[noreturn]] void await_resume() const noexcept {
        CO2_CONTRACT_FAIL("just_stopped() awaited by a caller that does not ask "
                          "await_completion()");
    }

    Completion await_completion() const noexcept { return Completion::stopped(); }
};

// 取消点。它不挂起：await_suspend 返回 false，协程立刻继续；借这次调用拿到带类型的
// 句柄，读等待者环境里的停止令牌。
struct StopIfRequested {
    bool requested{};

    bool await_ready() const noexcept { return false; }

    template <class Promise>
    bool await_suspend(coroutine_handle<Promise> const awaiting) noexcept {
        requested = stopRequestedOf(awaiting);
        return false;
    }

    // 不认识 await_completion() 的调用方不被取消，照常继续。
    void await_resume() const noexcept {}

    Completion await_completion() const noexcept {
        return requested ? Completion::stopped() : Completion::value();
    }
};

} // namespace detail

inline detail::JustError just_error(Error error) noexcept {
    return detail::JustError{std::move(error)};
}

inline detail::JustStopped just_stopped() noexcept { return detail::JustStopped{}; }

inline detail::StopIfRequested stopIfRequested() noexcept {
    return detail::StopIfRequested{};
}

struct WithError {
    Error error;
};

inline WithError with_error(Error error) noexcept { return WithError{std::move(error)}; }

} // namespace co2
