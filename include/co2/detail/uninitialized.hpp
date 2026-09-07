#pragma once

#include <new>
#include <type_traits>
#include <utility>

#include "co2/config.hpp"

namespace co2 {
namespace detail {

// 一格无标志的原始存储：只提供对齐的字节和 construct / get / destroy，"里面是否有对象"
// 由持有者自己记录。ResultStorage 多带一个 engaged 标志；当持有者已经有一个状态字（例
// 如 CallbackAwaitable 的原子 state）时，这个标志是多余的一格填充。T 为 void 时退化为
// 空类型，三个操作都是空操作，调用方不必为 void 另写一份。
template <class T> struct Uninitialized {
    static_assert(std::is_object<T>::value, "Uninitialized<T> requires an object type");
    static_assert(not std::is_array<T>::value, "Uninitialized<T> cannot hold arrays");
    static_assert(not std::is_const<T>::value && not std::is_volatile<T>::value,
                  "Uninitialized<T> cannot hold cv-qualified objects");

    Uninitialized() noexcept = default;

    // 复制字节不等于复制对象；持有者按自己的状态决定怎么搬。
    Uninitialized(Uninitialized const&) = delete;
    Uninitialized& operator=(Uninitialized const&) = delete;

    template <class... Args> T& construct(Args&&... args) {
        return *::new (static_cast<void*>(bytes)) T(std::forward<Args>(args)...);
    }

    T& get() noexcept { return *reinterpret_cast<T*>(bytes); }
    T const& get() const noexcept { return *reinterpret_cast<T const*>(bytes); }

    void destroy() noexcept { get().~T(); }

  private:
    alignas(T) unsigned char bytes[sizeof(T)];
};

template <> struct Uninitialized<void> {
    Uninitialized() noexcept = default;
    Uninitialized(Uninitialized const&) = delete;
    Uninitialized& operator=(Uninitialized const&) = delete;

    void construct() noexcept {}
    void get() const noexcept {}
    void destroy() noexcept {}
};

} // namespace detail
} // namespace co2
