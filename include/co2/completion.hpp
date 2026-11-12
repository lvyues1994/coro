#pragma once

#include <new>
#include <utility>

#include "co2/config.hpp"
#include "co2/contract.hpp"
#include "co2/error.hpp"

// co2::Completion：一次等待是怎么结束的——有值、出错、还是已停止。它是 Result<T> 去掉
// 值的那一部分：awaiter 的可选成员 await_completion() 返回它，恢复点据此决定是继续
// 执行（调用 await_resume() 取值）还是就此结束协程。

namespace co2 {

struct Completion {
    static Completion value() noexcept { return Completion{Kind::value}; }
    static Completion stopped() noexcept { return Completion{Kind::stopped}; }

    static Completion error(Error failure) noexcept {
        auto completion = Completion{Kind::error};
        ::new (static_cast<void*>(&completion.failure)) Error{std::move(failure)};
        return completion;
    }

    Completion(Completion const& other) noexcept : kind{other.kind} {
        if (kind == Kind::error)
            ::new (static_cast<void*>(&failure)) Error{other.failure};
    }

    Completion(Completion&& other) noexcept : kind{other.kind} {
        if (kind == Kind::error)
            ::new (static_cast<void*>(&failure)) Error{std::move(other.failure)};
    }

    Completion& operator=(Completion other) noexcept {
        this->~Completion();
        ::new (static_cast<void*>(this)) Completion{std::move(other)};
        return *this;
    }

    ~Completion() {
        if (kind == Kind::error) failure.~Error();
    }

    bool isValue() const noexcept { return kind == Kind::value; }
    bool isStopped() const noexcept { return kind == Kind::stopped; }
    bool hasError() const noexcept { return kind == Kind::error; }

    // 前置条件：hasError()。
    Error const& error() const& noexcept {
        CO2_CONTRACT_CHECK(kind == Kind::error);
        return failure;
    }

    Error&& error() && noexcept {
        CO2_CONTRACT_CHECK(kind == Kind::error);
        return std::move(failure);
    }

  private:
    enum class Kind : unsigned char { value, stopped, error };

    explicit Completion(Kind const kind_) noexcept : kind{kind_} {}

    Kind kind;
    union {
        Error failure;
    };
};

} // namespace co2
