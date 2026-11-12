#pragma once

// 测试用的错误枚举，按 <system_error> 的常规方式接入：一个错误类别、ADL 能找到的
// make_error_code、以及 std::is_error_code_enum 的特化。使用者自己的错误枚举也是
// 这样接入 co2::Error 的。

#include <string>
#include <system_error>

namespace sample {

enum class Errc { retry_later = 1, gone = 2 };

struct Category final : std::error_category {
    char const* name() const noexcept override { return "sample"; }

    std::string message(int const value) const override {
        return value == static_cast<int>(Errc::retry_later) ? "retry later" : "gone";
    }
};

inline std::error_category const& category() noexcept {
    static Category const instance{};
    return instance;
}

inline std::error_code make_error_code(Errc const code) noexcept {
    return std::error_code{static_cast<int>(code), category()};
}

} // namespace sample

namespace std {
template <> struct is_error_code_enum<sample::Errc> : true_type {};
} // namespace std
