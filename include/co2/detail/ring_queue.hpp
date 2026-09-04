#pragma once

#include <cstddef>
#include <type_traits>
#include <utility>
#include <vector>

#include "co2/contract.hpp"

namespace co2 {
namespace detail {

// 环形 FIFO：只在满时扩容，容量保持 2 的幂（下标用掩码而不是取模）。std::deque 在
// "尾进头出"的稳态下每隔一个块就分配/释放一次，做不到零分配；环在容量够用后再也不碰
// 堆。非线程安全，调用方自己加锁。
template <class T> struct RingQueue {
    // pop() 是 noexcept 的：元素移动构造不得抛出。
    static_assert(std::is_nothrow_move_constructible<T>::value,
                  "RingQueue<T> requires nothrow-move-constructible elements: pop() is "
                  "noexcept");
    // grow() 用 std::vector<T>(capacity) 值初始化新缓冲区，再逐个移动赋值进去。
    static_assert(std::is_default_constructible<T>::value,
                  "RingQueue<T> requires default-constructible elements: grow() "
                  "value-initializes the new buffer");
    static_assert(std::is_nothrow_move_assignable<T>::value,
                  "RingQueue<T> requires nothrow-move-assignable elements: push() and "
                  "grow() move-assign into the buffer");

    bool empty() const noexcept { return count == 0U; }
    std::size_t size() const noexcept { return count; }

    void push(T value) {
        if (count == buffer.size()) grow();
        buffer[(head + count) & mask] = std::move(value);
        ++count;
    }

    T pop() noexcept {
        CO2_CONTRACT_CHECK(count != 0U);
        auto value = std::move(buffer[head]);
        head = (head + 1U) & mask;
        --count;
        return value;
    }

  private:
    void grow() {
        auto const oldCapacity = buffer.size();
        auto const newCapacity = oldCapacity == 0U ? std::size_t{16} : oldCapacity * 2U;
        auto grown = std::vector<T>(newCapacity);
        for (auto index = std::size_t{}; index != count; ++index)
            grown[index] = std::move(buffer[(head + index) & mask]);
        buffer.swap(grown);
        head = 0U;
        mask = newCapacity - 1U;
    }

    std::vector<T> buffer;
    std::size_t head{};
    std::size_t count{};
    std::size_t mask{};
};

} // namespace detail
} // namespace co2
