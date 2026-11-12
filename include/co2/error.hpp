#pragma once

#include <exception>
#include <new>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>

#include "co2/config.hpp"
#include "co2/contract.hpp"

// co2::Error：一次失败。它要么是一个 std::error_code，要么是一个被捕获的异常
// （std::exception_ptr）。不分配，不需要 RTTI；错误码的两个字段与 exception_ptr 共用
// 存储，所以在 exception_ptr 只有一个指针宽的平台上它与 std::error_code 一样大。
//
// 用户自己的错误枚举按 <system_error> 的常规方式接入（std::is_error_code_enum 的特化
// 加上 ADL 能找到的 make_error_code），之后可以直接隐式转换成 Error。

namespace co2 {

// co2 自己的错误类别。Error 装着异常时，code() 报告 unhandled_exception。
enum class Errc { unhandled_exception = 1 };

namespace detail {

struct ErrorCategory final : std::error_category {
    char const* name() const noexcept override { return "co2"; }

    std::string message(int const value) const override {
        if (value == static_cast<int>(Errc::unhandled_exception))
            return "unhandled exception";
        return "unknown co2 error";
    }
};

inline std::error_category const& errorCategory() noexcept {
    static ErrorCategory const instance{};
    return instance;
}

} // namespace detail

inline std::error_code make_error_code(Errc const code) noexcept {
    return std::error_code{static_cast<int>(code), detail::errorCategory()};
}

} // namespace co2

namespace std {
template <> struct is_error_code_enum<co2::Errc> : true_type {};
} // namespace std

namespace co2 {

struct Error {
    // 前置条件：code 表示失败（非零）。"成功"不是一种错误。
    Error(std::error_code const code) noexcept
        : category{&code.category()}, number{code.value()}, kind{Kind::code} {
        CO2_CONTRACT_CHECK(static_cast<bool>(code));
    }

    // 错误码枚举一步转换成 Error。枚举先转 std::error_code 再转 Error 是两次用户定义
    // 转换，隐式转换走不通，所以需要这个模板——std::error_code 自己也是这样做的。
    template <class Enum, class = typename std::enable_if<
                              std::is_error_code_enum<Enum>::value>::type>
    Error(Enum const code) noexcept : Error{std::error_code{code}} {}

    // 前置条件：exception 非空。
    explicit Error(std::exception_ptr exception) noexcept
        : number{0}, kind{Kind::exception} {
        CO2_CONTRACT_CHECK(exception != nullptr);
        ::new (static_cast<void*>(&captured)) std::exception_ptr{std::move(exception)};
    }

    Error(Error const& other) noexcept : number{other.number}, kind{other.kind} {
        if (kind == Kind::exception)
            ::new (static_cast<void*>(&captured)) std::exception_ptr{other.captured};
        else
            category = other.category;
    }

    // 被移动的一方只保证还能析构和赋值，内容不再有意义。
    Error(Error&& other) noexcept : number{other.number}, kind{other.kind} {
        if (kind == Kind::exception)
            ::new (static_cast<void*>(&captured))
                std::exception_ptr{std::move(other.captured)};
        else
            category = other.category;
    }

    Error& operator=(Error other) noexcept {
        this->~Error();
        ::new (static_cast<void*>(this)) Error{std::move(other)};
        return *this;
    }

    ~Error() {
        if (kind == Kind::exception) captured.~exception_ptr();
    }

    // 装着异常时返回 Errc::unhandled_exception。
    std::error_code code() const noexcept {
        if (kind == Kind::exception) return make_error_code(Errc::unhandled_exception);
        return std::error_code{number, *category};
    }

    // 装着错误码时为空。
    std::exception_ptr exception() const noexcept {
        return kind == Kind::exception ? captured : std::exception_ptr{};
    }

    // 变回异常：装着异常就重抛原异常，装着错误码就抛 std::system_error。
    [[noreturn]] void raise() const {
        if (kind == Kind::exception) std::rethrow_exception(captured);
        throw std::system_error{code()};
    }

  private:
    enum class Kind : unsigned { code, exception };

    union {
        std::error_category const* category;
        std::exception_ptr captured;
    };
    int number;
    Kind kind;
};

} // namespace co2
