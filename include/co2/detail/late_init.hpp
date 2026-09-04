#pragma once

#include <new>
#include <type_traits>
#include <utility>

#include "co2/contract.hpp"

namespace co2 {
namespace detail {

// 稍后就地构造一次的载荷，载荷本身可以不可移动（如 stop_callback）。持有者只允许在
// 载荷尚未构造时被移动——awaiter 被移进帧的 awaiter 槽发生在启动之前，正是这种情形。
template <class T> struct LateInit {
    static_assert(std::is_object<T>::value, "LateInit<T> requires an object type");

    LateInit() noexcept = default;

    LateInit(LateInit&& other) noexcept { CO2_CONTRACT_CHECK(not other.engaged); }

    LateInit(LateInit const&) = delete;
    LateInit& operator=(LateInit const&) = delete;
    LateInit& operator=(LateInit&&) = delete;

    ~LateInit() { reset(); }

    bool hasValue() const noexcept { return engaged; }

    template <class... Args> T& emplace(Args&&... args) {
        CO2_CONTRACT_CHECK(not engaged);
        auto* const value =
            ::new (static_cast<void*>(bytes)) T(std::forward<Args>(args)...);
        engaged = true;
        return *value;
    }

    T& get() noexcept {
        CO2_CONTRACT_CHECK(engaged);
        return *reinterpret_cast<T*>(bytes);
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

} // namespace detail
} // namespace co2
