#pragma once

#include <exception>
#include <type_traits>
#include <utility>

#include "co2/completion.hpp"
#include "co2/config.hpp"
#include "co2/coroutine_handle.hpp"
#include "co2/detail/awaitable.hpp"
#include "co2/error.hpp"
#include "co2/result.hpp"

// co2::asResult(awaitable)：把一次等待的三种结果都变成值。
//
//   CO2_AWAIT_SET(r, co2::asResult(readHeader(socket)));   // r 是 co2::Result<Header>
//
// 普通的 CO2_AWAIT 遇到错误或已停止会就此结束当前协程；套上 asResult 之后当前协程照常
// 往下执行，自己检查 r.hasValue() / r.hasError() / r.isStopped()。被包住的操作抛出的
// 异常也被接成错误。Result<T> 不能默认构造，而帧局部必须可以：用
// co2::optional<co2::Result<T>> 作为 CO2_AWAIT_SET 的目标。
//
// asResult(x) 要直接写在等待它的地方：它交出的 awaiter 取自一个右值。x 是右值时被搬进
// 等待者的帧；x 是左值时只记它的引用，x 必须活过这次等待。

namespace co2 {
namespace detail {

template <class Awaited> struct AsResultType {
    using type = Result<typename std::decay<Awaited>::type>;
};
template <> struct AsResultType<void> { using type = Result<void>; };

// 内层 awaiter 有 await_completion() 就问它——每次等待恰好一次——没有就一定有值。
template <class Awaiter>
Completion completionOf(Awaiter& awaiter, std::true_type) noexcept {
    return awaiter.await_completion();
}

template <class Awaiter> Completion completionOf(Awaiter&, std::false_type) noexcept {
    return Completion::value();
}

// 自己没有 await_completion()，所以从不向上传播。
template <class Inner> struct AsResultAwaiter {
    using Awaited = decltype(std::declval<Inner&>().await_resume());
    using ResultType = typename AsResultType<Awaited>::type;

    Inner inner;

    auto await_ready() -> decltype(std::declval<Inner&>().await_ready()) {
        return inner.await_ready();
    }

    // 带类型的句柄原样交给内层：它可能要读等待者的环境。
    template <class Promise>
    auto await_suspend(coroutine_handle<Promise> const awaiting)
        -> decltype(std::declval<Inner&>().await_suspend(awaiting)) {
        return inner.await_suspend(awaiting);
    }

    ResultType await_resume() {
        auto completion =
            completionOf(inner, typename HasAwaitCompletion<Inner>::type{});
        if (completion.isStopped()) return ResultType::stopped();
        if (completion.hasError()) return ResultType{std::move(completion).error()};
#if CO2_HAS_EXCEPTIONS
        try {
            return resumeInner(std::is_void<Awaited>{});
        } catch (...) {
            return ResultType{Error{std::current_exception()}};
        }
#else
        return resumeInner(std::is_void<Awaited>{});
#endif
    }

  private:
    ResultType resumeInner(std::false_type) { return ResultType{inner.await_resume()}; }

    ResultType resumeInner(std::true_type) {
        inner.await_resume();
        return ResultType{};
    }
};

} // namespace detail

// asResult(x) 的返回类型。Awaitable 对右值是值类型（拥有 x），对左值是引用类型。
template <class Awaitable> struct AsResult {
    Awaitable awaitable;

    detail::AsResultAwaiter<detail::AwaiterOf<Awaitable>> operator_co_await() && {
        return detail::AsResultAwaiter<detail::AwaiterOf<Awaitable>>{
            detail::AwaiterOf<Awaitable>{
                detail::getAwaiter(std::forward<Awaitable>(awaitable))}};
    }
};

template <class Awaitable> AsResult<Awaitable> asResult(Awaitable&& awaitable) {
    return AsResult<Awaitable>{std::forward<Awaitable>(awaitable)};
}

} // namespace co2
