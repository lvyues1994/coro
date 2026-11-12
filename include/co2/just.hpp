#pragma once

#include <utility>

#include "co2/completion.hpp"
#include "co2/config.hpp"
#include "co2/coroutine_handle.hpp"
#include "co2/error.hpp"

// co2：在协程体里直接产生"错误"这种结果。名字取自 P2300 与 P3552。
//
//   CO2_AWAIT(co2::just_error(MyErrc::retry_later));   // 当前协程以这个错误结束
//   CO2_YIELD(co2::with_error(MyErrc::retry_later));   // 同上，P3552 的另一种拼写
//
// 两者效果相同：其后的语句不执行，局部析构，协程以错误结束，等待它的协程收到同一个
// 错误。just_error 是普通的 awaitable，在任何 promise 提供了 return_error() 的协程里都
// 能用；with_error 是一个标记值，由 promise 的 yield_value 接住，只在提供了这个重载的
// 协程里能写（Task）。
//
// 参数是 co2::Error：std::error_code 与错误码枚举可以直接传；一个捕获的异常要显式写成
// co2::Error{exceptionPointer}。

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

} // namespace detail

inline detail::JustError just_error(Error error) noexcept {
    return detail::JustError{std::move(error)};
}

struct WithError {
    Error error;
};

inline WithError with_error(Error error) noexcept { return WithError{std::move(error)}; }

} // namespace co2
