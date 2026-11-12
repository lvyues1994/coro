#pragma once

#include <new>
#include <type_traits>
#include <utility>

#include "co2/config.hpp"
#include "co2/contract.hpp"
#include "co2/error.hpp"

// co2::Result<T>：一个操作的三种结果之一——值、错误（co2::Error）、已停止。
// CO2_AWAIT_SET(r, co2::asResult(x)) 得到它；普通的 CO2_AWAIT 不产生它，而是把错误与
// 已停止向上传播。T 可以是 void。
//
// co2::Unit：空的值类型。Task<void> 的根返回 co2::optional<co2::Unit>，与非 void 的根
// 同形。

namespace co2 {

struct Unit {
    friend bool operator==(Unit, Unit) noexcept { return true; }
    friend bool operator!=(Unit, Unit) noexcept { return false; }
};

template <class T> struct Result {
    static_assert(std::is_object<T>::value && not std::is_array<T>::value,
                  "co2::Result<T> requires void or a non-array object type");
    static_assert(not std::is_const<T>::value && not std::is_volatile<T>::value,
                  "co2::Result<T> cannot hold cv-qualified objects");
    static_assert(not std::is_same<T, Error>::value,
                  "co2::Result<co2::Error> is not allowed: its value and its error "
                  "would be indistinguishable");

    using value_type = T;

    // 圆括号是有意的：泛型 T 上花括号可能选中 initializer_list 构造函数。
    Result(T value) noexcept(std::is_nothrow_move_constructible<T>::value)
        : state{State::value} {
        ::new (static_cast<void*>(&storage.value)) T(std::move(value));
    }

    Result(Error error) noexcept : state{State::error} {
        ::new (static_cast<void*>(&storage.error)) Error{std::move(error)};
    }

    // 错误码枚举直接构造出"错误"，理由同 Error 的枚举构造函数。
    template <class Enum, class = typename std::enable_if<
                              std::is_error_code_enum<Enum>::value>::type>
    Result(Enum const code) noexcept : Result{Error{code}} {}

    static Result stopped() noexcept { return Result{Stopped{}}; }

    Result(Result const& other) : state{other.state} {
        if (state == State::value)
            ::new (static_cast<void*>(&storage.value)) T(other.storage.value);
        else if (state == State::error)
            ::new (static_cast<void*>(&storage.error)) Error{other.storage.error};
    }

    Result(Result&& other) noexcept(std::is_nothrow_move_constructible<T>::value)
        : state{other.state} {
        if (state == State::value)
            ::new (static_cast<void*>(&storage.value)) T(std::move(other.storage.value));
        else if (state == State::error)
            ::new (static_cast<void*>(&storage.error))
                Error{std::move(other.storage.error)};
    }

    // 先销毁再构造。T 的移动构造抛出时本对象变为已停止，而不是留下一个半死的值。
    Result& operator=(Result other) noexcept(std::is_nothrow_move_constructible<T>::value) {
        destroy();
        state = State::stopped;
        if (other.state == State::value)
            ::new (static_cast<void*>(&storage.value)) T(std::move(other.storage.value));
        else if (other.state == State::error)
            ::new (static_cast<void*>(&storage.error))
                Error{std::move(other.storage.error)};
        state = other.state;
        return *this;
    }

    ~Result() { destroy(); }

    bool hasValue() const noexcept { return state == State::value; }
    bool hasError() const noexcept { return state == State::error; }
    bool isStopped() const noexcept { return state == State::stopped; }
    explicit operator bool() const noexcept { return state == State::value; }

    // 前置条件：hasValue()。
    T& value() & noexcept {
        CO2_CONTRACT_CHECK(state == State::value);
        return storage.value;
    }

    T const& value() const& noexcept {
        CO2_CONTRACT_CHECK(state == State::value);
        return storage.value;
    }

    T&& value() && noexcept {
        CO2_CONTRACT_CHECK(state == State::value);
        return std::move(storage.value);
    }

    // 前置条件：hasError()。
    Error const& error() const noexcept {
        CO2_CONTRACT_CHECK(state == State::error);
        return storage.error;
    }

  private:
    struct Stopped {};
    enum class State : unsigned char { value, error, stopped };

    explicit Result(Stopped) noexcept : state{State::stopped} {}

    void destroy() noexcept {
        if (state == State::value)
            storage.value.~T();
        else if (state == State::error)
            storage.error.~Error();
    }

    union Storage {
        Storage() noexcept {}
        ~Storage() {}
        T value;
        Error error;
    } storage;
    State state;
};

template <> struct Result<void> {
    using value_type = void;

    Result() noexcept : state{State::value} {}

    Result(Error error) noexcept : state{State::error} {
        ::new (static_cast<void*>(&storage.error)) Error{std::move(error)};
    }

    template <class Enum, class = typename std::enable_if<
                              std::is_error_code_enum<Enum>::value>::type>
    Result(Enum const code) noexcept : Result{Error{code}} {}

    static Result stopped() noexcept { return Result{Stopped{}}; }

    Result(Result const& other) noexcept : state{other.state} {
        if (state == State::error)
            ::new (static_cast<void*>(&storage.error)) Error{other.storage.error};
    }

    Result(Result&& other) noexcept : state{other.state} {
        if (state == State::error)
            ::new (static_cast<void*>(&storage.error))
                Error{std::move(other.storage.error)};
    }

    Result& operator=(Result other) noexcept {
        destroy();
        state = other.state;
        if (state == State::error)
            ::new (static_cast<void*>(&storage.error))
                Error{std::move(other.storage.error)};
        return *this;
    }

    ~Result() { destroy(); }

    bool hasValue() const noexcept { return state == State::value; }
    bool hasError() const noexcept { return state == State::error; }
    bool isStopped() const noexcept { return state == State::stopped; }
    explicit operator bool() const noexcept { return state == State::value; }

    // 前置条件：hasValue()。
    void value() const noexcept { CO2_CONTRACT_CHECK(state == State::value); }

    // 前置条件：hasError()。
    Error const& error() const noexcept {
        CO2_CONTRACT_CHECK(state == State::error);
        return storage.error;
    }

  private:
    struct Stopped {};
    enum class State : unsigned char { value, error, stopped };

    explicit Result(Stopped) noexcept : state{State::stopped} {}

    void destroy() noexcept {
        if (state == State::error) storage.error.~Error();
    }

    union Storage {
        Storage() noexcept {}
        ~Storage() {}
        Error error;
    } storage;
    State state;
};

} // namespace co2
