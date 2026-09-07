#pragma once

#include <cassert>
#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

#include "co2/config.hpp"

namespace co2 {
namespace detail {

// 一格可空的结果存储：就地构造、借出引用、整体取走。T 为 void 时退化为一个"已完成"
// 标志，因此每个 promise、每个组合器和每个协程体只需要写一份，不再为 void 手写第二
// 套。它也是帧局部结果的首选容器：帧成员必须可默认构造，而 T 未必可以。
template <class T> struct ResultStorage {
    static_assert(std::is_object<T>::value, "ResultStorage<T> requires an object type");
    static_assert(not std::is_array<T>::value, "ResultStorage<T> cannot hold arrays");
    static_assert(not std::is_const<T>::value && not std::is_volatile<T>::value,
                  "ResultStorage<T> cannot hold cv-qualified objects");

    ResultStorage() noexcept = default;

    ResultStorage(ResultStorage&& other) noexcept(
        std::is_nothrow_move_constructible<T>::value) {
        if (other.engaged) {
            emplace(std::move(other.get()));
            other.reset();
        }
    }

    ResultStorage& operator=(ResultStorage&& other) noexcept(
        std::is_nothrow_move_constructible<T>::value) {
        if (this == &other) return *this;
        reset();
        if (other.engaged) {
            emplace(std::move(other.get()));
            other.reset();
        }
        return *this;
    }

    // 让 `CO2_AWAIT_SET(storage, awaiter)` 能把 await_resume() 的返回值直接构造进来。
    ResultStorage& operator=(T&& value) {
        reset();
        emplace(std::move(value));
        return *this;
    }

    ResultStorage(ResultStorage const&) = delete;
    ResultStorage& operator=(ResultStorage const&) = delete;

    ~ResultStorage() { reset(); }

    bool hasValue() const noexcept { return engaged; }
    explicit operator bool() const noexcept { return engaged; }

    template <class... Args> T& emplace(Args&&... args) {
        assert(not engaged);
        auto* const result =
            ::new (static_cast<void*>(bytes)) T(std::forward<Args>(args)...);
        engaged = true;
        return *result;
    }

    T& get() noexcept {
        assert(engaged);
        return *reinterpret_cast<T*>(bytes);
    }

    T const& get() const noexcept {
        assert(engaged);
        return *reinterpret_cast<T const*>(bytes);
    }

    // 移动构造抛出时对象保持 engaged，析构仍会清理。圆括号是有意的：泛型 T 上花括号
    // 可能选中 initializer_list 构造函数。
    T take() {
        assert(engaged);
        T result(std::move(get()));
        reset();
        return result;
    }

    void reset() noexcept {
        if (not engaged) return;
        engaged = false;
        reinterpret_cast<T*>(bytes)->~T();
    }

  private:
    alignas(T) unsigned char bytes[sizeof(T)];
    bool engaged{};
};

template <> struct ResultStorage<void> {
    ResultStorage() noexcept = default;

    ResultStorage(ResultStorage&& other) noexcept : engaged{other.engaged} {
        other.engaged = false;
    }

    ResultStorage& operator=(ResultStorage&& other) noexcept {
        engaged = other.engaged;
        if (this != &other) other.engaged = false;
        return *this;
    }

    ResultStorage(ResultStorage const&) = delete;
    ResultStorage& operator=(ResultStorage const&) = delete;

    bool hasValue() const noexcept { return engaged; }
    explicit operator bool() const noexcept { return engaged; }

    void emplace() noexcept {
        assert(not engaged);
        engaged = true;
    }

    void get() const noexcept { assert(engaged); }

    void take() noexcept {
        assert(engaged);
        engaged = false;
    }

    void reset() noexcept { engaged = false; }

  private:
    bool engaged{};
};

} // namespace detail
} // namespace co2
