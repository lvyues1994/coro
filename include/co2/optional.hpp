#pragma once

#include <new>
#include <type_traits>
#include <utility>

#include "co2/config.hpp"
#include "co2/contract.hpp"

// co2::optional<T>：std::optional 的 C++14 子集。根（syncWait、JoinHandle::join）用它
// 表示"有值 / 已停止"。C++17 起也不换成 std::optional 的别名：头文件在各个标准下的定义
// 必须一致（与 co2::stop_token 相同）。
//
// 与 std::optional 的差别：
//   - 空的时候 value()、operator* 与 operator-> 是契约违规。std 里 value() 抛
//     bad_optional_access、后两者是未定义行为；这里统一，开不开异常行为相同。
//   - 赋值是"先销毁再构造"，不走 T 的赋值运算符；构造抛出时留下空的 optional。
//   - 不是 constexpr，不保留 T 的平凡性；没有 in_place 构造、optional<U> 的转换构造、
//     make_optional、hash 与 swap。

namespace co2 {

struct nullopt_t {
    explicit constexpr nullopt_t(int) noexcept {}
};

// 命名空间作用域的 constexpr 对象有内部链接；它只按值传递，不会被 odr-use。
constexpr nullopt_t nullopt{0};

template <class T> struct optional;

namespace detail {

template <class T> struct IsOptional : std::false_type {};
template <class T> struct IsOptional<optional<T>> : std::true_type {};

// 能与 optional<T> 做"值比较"的另一侧：不是 optional，也不是 nullopt_t。
template <class U>
using ComparableValue = typename std::enable_if<
    not IsOptional<U>::value && not std::is_same<U, nullopt_t>::value, int>::type;

} // namespace detail

template <class T> struct optional {
    static_assert(std::is_object<T>::value && not std::is_array<T>::value,
                  "co2::optional<T> requires a non-array object type");
    static_assert(not std::is_const<T>::value && not std::is_volatile<T>::value,
                  "co2::optional<T> cannot hold cv-qualified objects");
    static_assert(not std::is_same<T, nullopt_t>::value,
                  "co2::optional<nullopt_t> is not allowed");

    using value_type = T;

    optional() noexcept {}
    optional(nullopt_t) noexcept {}

    optional(optional const& other) {
        if (other.engaged) construct(other.get());
    }

    // 与 std 一致：被移动的一方仍然有值（一个被移走的 T）。
    optional(optional&& other) noexcept(std::is_nothrow_move_constructible<T>::value) {
        if (other.engaged) construct(std::move(other.get()));
    }

    template <class U = T,
              class = typename std::enable_if<
                  std::is_constructible<T, U&&>::value &&
                  not std::is_same<typename std::decay<U>::type, optional>::value &&
                  not std::is_same<typename std::decay<U>::type, nullopt_t>::value>::type>
    optional(U&& value) {
        construct(std::forward<U>(value));
    }

    optional& operator=(nullopt_t) noexcept {
        reset();
        return *this;
    }

    optional& operator=(optional const& other) {
        if (this == &other) return *this;
        reset();
        if (other.engaged) construct(other.get());
        return *this;
    }

    optional&
    operator=(optional&& other) noexcept(std::is_nothrow_move_constructible<T>::value) {
        if (this == &other) return *this;
        reset();
        if (other.engaged) construct(std::move(other.get()));
        return *this;
    }

    template <class U = T,
              class = typename std::enable_if<
                  std::is_constructible<T, U&&>::value &&
                  not std::is_same<typename std::decay<U>::type, optional>::value &&
                  not std::is_same<typename std::decay<U>::type, nullopt_t>::value>::type>
    optional& operator=(U&& value) {
        reset();
        construct(std::forward<U>(value));
        return *this;
    }

    ~optional() { reset(); }

    bool has_value() const noexcept { return engaged; }
    explicit operator bool() const noexcept { return engaged; }

    T& operator*() & noexcept { return checked(); }
    T const& operator*() const& noexcept { return checked(); }
    T&& operator*() && noexcept { return std::move(checked()); }

    T* operator->() noexcept { return &checked(); }
    T const* operator->() const noexcept { return &checked(); }

    T& value() & noexcept { return checked(); }
    T const& value() const& noexcept { return checked(); }
    T&& value() && noexcept { return std::move(checked()); }

    template <class U> T value_or(U&& fallback) const& {
        return engaged ? get() : static_cast<T>(std::forward<U>(fallback));
    }

    template <class U> T value_or(U&& fallback) && {
        return engaged ? std::move(get()) : static_cast<T>(std::forward<U>(fallback));
    }

    void reset() noexcept {
        if (not engaged) return;
        engaged = false;
        get().~T();
    }

    template <class... Args> T& emplace(Args&&... args) {
        reset();
        return construct(std::forward<Args>(args)...);
    }

  private:
    // 圆括号是有意的：泛型 T 上花括号可能选中 initializer_list 构造函数。
    template <class... Args> T& construct(Args&&... args) {
        auto* const object =
            ::new (static_cast<void*>(bytes)) T(std::forward<Args>(args)...);
        engaged = true;
        return *object;
    }

    T& get() noexcept { return *reinterpret_cast<T*>(bytes); }
    T const& get() const noexcept { return *reinterpret_cast<T const*>(bytes); }

    T& checked() noexcept {
        CO2_CONTRACT_CHECK(engaged);
        return get();
    }

    T const& checked() const noexcept {
        CO2_CONTRACT_CHECK(engaged);
        return get();
    }

    alignas(T) unsigned char bytes[sizeof(T)];
    bool engaged{};
};

// ---- 比较：语义与 std::optional 相同，空的 optional 小于任何有值的 ----

template <class T, class U>
bool operator==(optional<T> const& left, optional<U> const& right) {
    if (left.has_value() != right.has_value()) return false;
    return not left.has_value() || *left == *right;
}

template <class T, class U>
bool operator!=(optional<T> const& left, optional<U> const& right) {
    if (left.has_value() != right.has_value()) return true;
    return left.has_value() && *left != *right;
}

template <class T, class U>
bool operator<(optional<T> const& left, optional<U> const& right) {
    return right.has_value() && (not left.has_value() || *left < *right);
}

template <class T, class U>
bool operator>(optional<T> const& left, optional<U> const& right) {
    return left.has_value() && (not right.has_value() || *left > *right);
}

template <class T, class U>
bool operator<=(optional<T> const& left, optional<U> const& right) {
    return not left.has_value() || (right.has_value() && *left <= *right);
}

template <class T, class U>
bool operator>=(optional<T> const& left, optional<U> const& right) {
    return not right.has_value() || (left.has_value() && *left >= *right);
}

template <class T> bool operator==(optional<T> const& value, nullopt_t) noexcept {
    return not value.has_value();
}
template <class T> bool operator==(nullopt_t, optional<T> const& value) noexcept {
    return not value.has_value();
}
template <class T> bool operator!=(optional<T> const& value, nullopt_t) noexcept {
    return value.has_value();
}
template <class T> bool operator!=(nullopt_t, optional<T> const& value) noexcept {
    return value.has_value();
}
template <class T> bool operator<(optional<T> const&, nullopt_t) noexcept {
    return false;
}
template <class T> bool operator<(nullopt_t, optional<T> const& value) noexcept {
    return value.has_value();
}
template <class T> bool operator>(optional<T> const& value, nullopt_t) noexcept {
    return value.has_value();
}
template <class T> bool operator>(nullopt_t, optional<T> const&) noexcept {
    return false;
}
template <class T> bool operator<=(optional<T> const& value, nullopt_t) noexcept {
    return not value.has_value();
}
template <class T> bool operator<=(nullopt_t, optional<T> const&) noexcept {
    return true;
}
template <class T> bool operator>=(optional<T> const&, nullopt_t) noexcept {
    return true;
}
template <class T> bool operator>=(nullopt_t, optional<T> const& value) noexcept {
    return not value.has_value();
}

template <class T, class U, detail::ComparableValue<U> = 0>
bool operator==(optional<T> const& left, U const& right) {
    return left.has_value() && *left == right;
}
template <class T, class U, detail::ComparableValue<U> = 0>
bool operator==(U const& left, optional<T> const& right) {
    return right.has_value() && left == *right;
}
template <class T, class U, detail::ComparableValue<U> = 0>
bool operator!=(optional<T> const& left, U const& right) {
    return not left.has_value() || *left != right;
}
template <class T, class U, detail::ComparableValue<U> = 0>
bool operator!=(U const& left, optional<T> const& right) {
    return not right.has_value() || left != *right;
}
template <class T, class U, detail::ComparableValue<U> = 0>
bool operator<(optional<T> const& left, U const& right) {
    return not left.has_value() || *left < right;
}
template <class T, class U, detail::ComparableValue<U> = 0>
bool operator<(U const& left, optional<T> const& right) {
    return right.has_value() && left < *right;
}
template <class T, class U, detail::ComparableValue<U> = 0>
bool operator>(optional<T> const& left, U const& right) {
    return left.has_value() && *left > right;
}
template <class T, class U, detail::ComparableValue<U> = 0>
bool operator>(U const& left, optional<T> const& right) {
    return not right.has_value() || left > *right;
}
template <class T, class U, detail::ComparableValue<U> = 0>
bool operator<=(optional<T> const& left, U const& right) {
    return not left.has_value() || *left <= right;
}
template <class T, class U, detail::ComparableValue<U> = 0>
bool operator<=(U const& left, optional<T> const& right) {
    return right.has_value() && left <= *right;
}
template <class T, class U, detail::ComparableValue<U> = 0>
bool operator>=(optional<T> const& left, U const& right) {
    return left.has_value() && *left >= right;
}
template <class T, class U, detail::ComparableValue<U> = 0>
bool operator>=(U const& left, optional<T> const& right) {
    return not right.has_value() || left >= *right;
}

} // namespace co2
