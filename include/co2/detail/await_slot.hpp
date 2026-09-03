#pragma once

#include <cassert>
#include <cstddef>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

#include "co2/config.hpp"

namespace co2 {
namespace detail {

template <std::size_t Capacity, class T>
struct AwaitSlotInline
    : std::integral_constant<bool, sizeof(T) <= Capacity &&
                                       alignof(T) <= alignof(std::max_align_t)> {};

// 同一时间每个 frame 中只有一个 awaiter。小 awaiter 内联保存以维持单次分配的 frame
// 模型；超过内联容量或对齐要求的 awaiter 退化为一次堆分配，而不是要求全局重定义
// CO2_AWAIT_STORAGE_SIZE——那个宏在不同翻译单元取不同值会改变 frame 布局。
template <std::size_t Capacity = CO2_AWAIT_STORAGE_SIZE> struct AwaitSlot {
    static_assert(
        Capacity >= sizeof(void*),
        "AwaitSlot must be able to hold the pointer of a heap-allocated awaiter");

    AwaitSlot() noexcept = default;

    AwaitSlot(AwaitSlot&& other) noexcept {
        // frame 只会在首次执行前移动，此时不可能存在活动 awaiter。移动已占用的 slot
        // 需要增加另一种擦除操作，并会削弱该不变量。
        assert(not other.hasValue());
        static_cast<void>(other);
    }

    AwaitSlot& operator=(AwaitSlot&& other) noexcept {
        if (this == &other) return *this;
        assert(not hasValue());
        assert(not other.hasValue());
        return *this;
    }

    AwaitSlot(AwaitSlot const&) = delete;
    AwaitSlot& operator=(AwaitSlot const&) = delete;

    ~AwaitSlot() { reset(); }

    bool hasValue() const noexcept { return destroy != nullptr; }

    template <class T> static constexpr bool isInline() noexcept {
        return AwaitSlotInline<Capacity, T>::value;
    }

    template <class T, class Value> T& emplace(Value&& value) {
        assert(not hasValue());
        return emplaceImpl<T>(std::forward<Value>(value), InlineTag<T>{});
    }

    template <class T> T& get() noexcept {
        assert(destroy == &destroyObject<T>);
        return *pointer<T>(InlineTag<T>{});
    }

    template <class T> T const& get() const noexcept {
        assert(destroy == &destroyObject<T>);
        return *pointer<T>(InlineTag<T>{});
    }

    void reset() noexcept {
        auto* const operation = destroy;
        if (operation == nullptr) return;

        destroy = nullptr;
        operation(storage);
    }

  private:
    using Destroy = void (*)(void*);

    template <class T> using InlineTag = AwaitSlotInline<Capacity, T>;

    template <class T> static void destroyObject(void* const slot) noexcept {
        destroyImpl<T>(slot, InlineTag<T>{});
    }

    template <class T>
    static void destroyImpl(void* const slot, std::true_type) noexcept {
        static_cast<T*>(slot)->~T();
    }

    template <class T>
    static void destroyImpl(void* const slot, std::false_type) noexcept {
        delete *static_cast<T**>(slot);
    }

    template <class T, class Value> T& emplaceImpl(Value&& value, std::true_type) {
        auto* const result =
            ::new (static_cast<void*>(storage)) T(std::forward<Value>(value));
        destroy = &destroyObject<T>;
        return *result;
    }

    template <class T, class Value> T& emplaceImpl(Value&& value, std::false_type) {
        auto owned = std::unique_ptr<T>{new T(std::forward<Value>(value))};
        ::new (static_cast<void*>(storage)) T* {owned.get()};
        destroy = &destroyObject<T>;
        return *owned.release();
    }

    template <class T> T* pointer(std::true_type) noexcept {
        return reinterpret_cast<T*>(storage);
    }

    template <class T> T* pointer(std::false_type) noexcept {
        return *reinterpret_cast<T**>(storage);
    }

    template <class T> T const* pointer(std::true_type) const noexcept {
        return reinterpret_cast<T const*>(storage);
    }

    template <class T> T const* pointer(std::false_type) const noexcept {
        return *reinterpret_cast<T* const*>(storage);
    }

    alignas(std::max_align_t) unsigned char storage[Capacity];
    Destroy destroy{};
};

} // namespace detail
} // namespace co2
