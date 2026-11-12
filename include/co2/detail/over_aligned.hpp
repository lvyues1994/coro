#pragma once

#include <cstddef>
#include <cstring>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

#include "co2/config.hpp"

namespace co2 {
namespace detail {

// 超对齐对象的手工对齐。
//
// 对齐要求超过 alignof(std::max_align_t) 的类型，C++14 的 operator new、std::allocator 和
// promise 的 operator new(size) 都给不出保证——它们拿不到对齐参数（C++17 的对齐 new 也只
// 覆盖其中一部分路径）。这里的做法与语言版本、分配来源都无关：多要
// alignment - 1 + sizeof(void*) 个字节，在块内取对齐的位置，并把块首指针记在对象前面，
// 释放时再找回来。

template <class T>
struct IsOverAligned
    : std::integral_constant<bool, (alignof(T) > alignof(std::max_align_t))> {};

constexpr std::size_t overAlignedBlockSize(std::size_t const size,
                                           std::size_t const alignment) noexcept {
    return size + alignment - 1U + sizeof(void*);
}

// `block` 是 overAlignedBlockSize(size, alignment) 字节的原始内存。返回其中对齐到
// `alignment`、可容纳 `size` 字节的地址，并在它前面记下 `block`。
inline void* placeOverAligned(void* const block, std::size_t const size,
                              std::size_t const alignment) noexcept {
    void* cursor = static_cast<unsigned char*>(block) + sizeof(void*);
    auto space = size + alignment - 1U;
    // space 留足了最坏情况的填充，std::align 不会失败。
    void* const object = std::align(alignment, size, cursor, space);
    std::memcpy(static_cast<unsigned char*>(object) - sizeof(void*), &block,
                sizeof(void*));
    return object;
}

// placeOverAligned 返回的地址 → 它所在的块。
inline void* overAlignedBlockOf(void* const object) noexcept {
    void* block{};
    std::memcpy(&block, static_cast<unsigned char*>(object) - sizeof(void*),
                sizeof(void*));
    return block;
}

// 全局堆上的单个对象。普通类型就是 new / delete；超对齐类型走手工对齐，不交给 new
// 表达式。
template <class T> struct HeapObject {
    template <class Value> static T* create(Value&& value) {
        return createImpl(std::forward<Value>(value), IsOverAligned<T>{});
    }

    static void destroy(T* const object) noexcept {
        destroyImpl(object, IsOverAligned<T>{});
    }

  private:
    template <class Value> static T* createImpl(Value&& value, std::false_type) {
        return new T(std::forward<Value>(value));
    }

    template <class Value> static T* createImpl(Value&& value, std::true_type) {
        void* const block =
            ::operator new(overAlignedBlockSize(sizeof(T), alignof(T)));
        try {
            return ::new (placeOverAligned(block, sizeof(T), alignof(T)))
                T(std::forward<Value>(value));
        } catch (...) {
            ::operator delete(block);
            throw;
        }
    }

    static void destroyImpl(T* const object, std::false_type) noexcept {
        delete object;
    }

    static void destroyImpl(T* const object, std::true_type) noexcept {
        object->~T();
        ::operator delete(overAlignedBlockOf(object));
    }
};

} // namespace detail
} // namespace co2
