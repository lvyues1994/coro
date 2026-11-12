#pragma once

#include <cassert>
#include <cstddef>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

#include "co2/config.hpp"
#include "co2/detail/over_aligned.hpp"

namespace co2 {
namespace detail {

template <std::size_t Capacity, class T>
struct AwaitSlotInline
    : std::integral_constant<bool, sizeof(T) <= Capacity &&
                                       alignof(T) <= alignof(std::max_align_t)> {};

// 同一时间每个 frame 中只有一个 awaiter。小 awaiter 内联保存以维持单次分配的 frame
// 模型；超过内联容量或对齐要求的 awaiter 退化为一次堆分配，而不是要求全局重定义
// CO2_AWAIT_STORAGE_SIZE——那个宏在不同翻译单元取不同值会改变 frame 布局。超对齐的
// awaiter 在堆上手工对齐（HeapObject）。
template <std::size_t Capacity = CO2_AWAIT_STORAGE_SIZE> struct AwaitSlot {
    static_assert(
        Capacity >= sizeof(void*),
        "AwaitSlot must be able to hold the pointer of a heap-allocated awaiter");

    AwaitSlot() noexcept = default;

    // 槽随帧一起原地构造、原地销毁，从不搬动：awaiter 可以持有指向自身的指针。
    AwaitSlot(AwaitSlot const&) = delete;
    AwaitSlot& operator=(AwaitSlot const&) = delete;

    ~AwaitSlot() { reset(); }

    // 槽里有一个需要析构的 awaiter。内联且平凡析构的 awaiter（suspend_always、只含指针
    // 的 awaiter……）不登记析构：它们的"销毁"什么也不做，登记只会让每次 await 多写一个
    // 函数指针、多一次间接调用。
    bool hasValue() const noexcept { return destroy != nullptr; }

    template <class T> static constexpr bool isInline() noexcept {
        return AwaitSlotInline<Capacity, T>::value;
    }

    template <class T, class Value> T& emplace(Value&& value) {
        assert(not hasValue());
        return emplaceImpl<T>(std::forward<Value>(value), InlineTag<T>{});
    }

    template <class T> T& get() noexcept {
        assert(NeedsNoDestroy<T>::value || destroy == &destroyObject<T>);
        return *pointer<T>(InlineTag<T>{});
    }

    template <class T> T const& get() const noexcept {
        assert(NeedsNoDestroy<T>::value || destroy == &destroyObject<T>);
        return *pointer<T>(InlineTag<T>{});
    }

    // 类型擦除的复位：异常离开协程体、destroy() 挂起中的协程时不知道槽里是什么类型。
    void reset() noexcept {
        auto* const operation = destroy;
        if (operation == nullptr) return;

        destroy = nullptr;
        operation(storage);
    }

    // 类型已知的复位：await 正常结束时生成的代码知道 awaiter 的类型，直接析构，不经函数
    // 指针。前置条件：槽里是一个 T。
    template <class T> void resetAs() noexcept { resetAsImpl<T>(NeedsNoDestroy<T>{}); }

  private:
    using Destroy = void (*)(void*);

    template <class T> using InlineTag = AwaitSlotInline<Capacity, T>;

    template <class T>
    using NeedsNoDestroy =
        std::integral_constant<bool, AwaitSlotInline<Capacity, T>::value &&
                                         std::is_trivially_destructible<T>::value>;

    template <class T> void resetAsImpl(std::true_type) noexcept {}

    template <class T> void resetAsImpl(std::false_type) noexcept {
        assert(destroy == &destroyObject<T>);
        destroy = nullptr;
        destroyImpl<T>(storage, InlineTag<T>{});
    }

    template <class T> void registerDestroy(std::true_type) noexcept {}

    template <class T> void registerDestroy(std::false_type) noexcept {
        destroy = &destroyObject<T>;
    }

    template <class T> static void destroyObject(void* const slot) noexcept {
        destroyImpl<T>(slot, InlineTag<T>{});
    }

    template <class T>
    static void destroyImpl(void* const slot, std::true_type) noexcept {
        static_cast<T*>(slot)->~T();
    }

    template <class T>
    static void destroyImpl(void* const slot, std::false_type) noexcept {
        HeapObject<T>::destroy(*static_cast<T**>(slot));
    }

    template <class T, class Value> T& emplaceImpl(Value&& value, std::true_type) {
        auto* const result =
            ::new (static_cast<void*>(storage)) T(std::forward<Value>(value));
        registerDestroy<T>(NeedsNoDestroy<T>{});
        return *result;
    }

    template <class T, class Value> T& emplaceImpl(Value&& value, std::false_type) {
        auto* const object = HeapObject<T>::create(std::forward<Value>(value));
        ::new (static_cast<void*>(storage)) T* {object};
        destroy = &destroyObject<T>;
        return *object;
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
