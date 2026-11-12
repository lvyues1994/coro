// 帧内存储构件：ResultStorage<T>、AwaitSlot（内联 + 堆回落）与 MoveOnlyFunction。

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

#include "co2/config.hpp"
#include "co2/detail/await_slot.hpp"
#include "co2/detail/move_only_function.hpp"
#include "co2/detail/result_storage.hpp"

namespace {

void fail(char const* expression, int line) {
    std::cerr << "check failed at line " << line << ": " << expression << '\n';
    std::exit(EXIT_FAILURE);
}

#define CHECK(expression)                                                              \
    do {                                                                               \
        if (not(expression)) fail(#expression, __LINE__);                              \
    } while (false)

struct TrackedValue {
    explicit TrackedValue(int& liveCount_) noexcept : liveCount{&liveCount_} {
        ++*liveCount;
    }

    TrackedValue(TrackedValue&& other) noexcept : liveCount{other.liveCount} {
        ++*liveCount;
    }

    TrackedValue(TrackedValue const&) = delete;
    TrackedValue& operator=(TrackedValue const&) = delete;
    TrackedValue& operator=(TrackedValue&&) = delete;

    ~TrackedValue() { --*liveCount; }

    int* liveCount;
};

void resultStorageOwnsExactlyOneValue() {
    auto live = 0;
    {
        auto storage = co2::detail::ResultStorage<TrackedValue>{};
        CHECK(not storage.hasValue());

        storage.emplace(live);
        CHECK(storage.hasValue());
        CHECK(live == 1);

        auto moved = std::move(storage);
        CHECK(not storage.hasValue());
        CHECK(moved.hasValue());
        CHECK(live == 1);

        auto taken = moved.take();
        CHECK(not moved.hasValue());
        CHECK(live == 1);
        static_cast<void>(taken);
    }
    CHECK(live == 0);

    auto voidStorage = co2::detail::ResultStorage<void>{};
    CHECK(not voidStorage.hasValue());
    voidStorage.emplace();
    CHECK(voidStorage.hasValue());
    voidStorage.take();
    CHECK(not voidStorage.hasValue());
}

struct LargeAwaiter {
    explicit LargeAwaiter(int& destroyed_) noexcept : destroyed{&destroyed_} {}
    LargeAwaiter(LargeAwaiter&& other) noexcept : destroyed{other.destroyed} {
        other.destroyed = nullptr;
    }
    ~LargeAwaiter() {
        if (destroyed != nullptr) ++*destroyed;
    }

    int* destroyed;
    unsigned char padding[CO2_AWAIT_STORAGE_SIZE * 2U]{};
};

// 超过内联容量的 awaiter 退化为堆分配，而不是要求全局改宏。
void awaitSlotFallsBackToTheHeapForLargeAwaiters() {
    static_assert(not co2::detail::AwaitSlot<>::isInline<LargeAwaiter>(),
                  "test awaiter must exceed the inline capacity");
    static_assert(co2::detail::AwaitSlot<>::isInline<int>(),
                  "small awaiters stay inline");

    auto destroyed = 0;
    {
        co2::detail::AwaitSlot<> slot;
        auto& stored = slot.emplace<LargeAwaiter>(LargeAwaiter{destroyed});
        CHECK(slot.hasValue());
        CHECK(&slot.get<LargeAwaiter>() == &stored);
        CHECK(stored.destroyed == &destroyed);
        slot.reset();
        CHECK(destroyed == 1);
        CHECK(not slot.hasValue());

        slot.emplace<int>(7);
        CHECK(slot.get<int>() == 7);
    }
    CHECK(destroyed == 1);
}

struct SmallTrackedAwaiter {
    explicit SmallTrackedAwaiter(int& destroyed_) noexcept : destroyed{&destroyed_} {}
    SmallTrackedAwaiter(SmallTrackedAwaiter&& other) noexcept
        : destroyed{other.destroyed} {
        other.destroyed = nullptr;
    }
    ~SmallTrackedAwaiter() {
        if (destroyed != nullptr) ++*destroyed;
    }

    int* destroyed;
};

// 类型已知的复位：await 正常结束时宏知道 awaiter 的类型，直接析构，不经函数指针；内联且
// 平凡析构的 awaiter 根本不登记析构，复位是空操作。类型擦除的 reset() 留给异常与
// destroy() 路径，两者可以混用。
void awaitSlotTypedResetDestroysExactlyOnce() {
    static_assert(co2::detail::AwaitSlot<>::isInline<SmallTrackedAwaiter>(),
                  "the small awaiter stays inline");
    auto destroyed = 0;
    co2::detail::AwaitSlot<> slot;

    slot.emplace<SmallTrackedAwaiter>(SmallTrackedAwaiter{destroyed});
    CHECK(slot.hasValue());
    slot.resetAs<SmallTrackedAwaiter>();
    CHECK(destroyed == 1);
    CHECK(not slot.hasValue());
    slot.reset(); // 已经复位：空操作
    CHECK(destroyed == 1);

    slot.emplace<LargeAwaiter>(LargeAwaiter{destroyed}); // 堆回落
    slot.resetAs<LargeAwaiter>();
    CHECK(destroyed == 2);
    CHECK(not slot.hasValue());

    // 平凡析构的内联 awaiter 没有需要登记的析构：槽在它之后可以直接复用。
    slot.emplace<int>(7);
    CHECK(slot.get<int>() == 7);
    CHECK(not slot.hasValue());
    slot.resetAs<int>();
    slot.emplace<int>(8);
    CHECK(slot.get<int>() == 8);
    slot.reset();

    // 类型擦除的 reset() 仍然销毁需要析构的 awaiter。
    slot.emplace<SmallTrackedAwaiter>(SmallTrackedAwaiter{destroyed});
    slot.reset();
    CHECK(destroyed == 3);
}

struct alignas(64) OverAlignedAwaiter {
    explicit OverAlignedAwaiter(int& destroyed_) noexcept : destroyed{&destroyed_} {}
    OverAlignedAwaiter(OverAlignedAwaiter&& other) noexcept
        : destroyed{other.destroyed} {
        other.destroyed = nullptr;
    }
    ~OverAlignedAwaiter() {
        if (destroyed != nullptr) ++*destroyed;
    }

    int* destroyed;
};

// 对齐要求超过 alignof(std::max_align_t) 的 awaiter 放不进内联缓冲，回落到堆；C++14 的
// new 表达式不保证超对齐，槽必须自己对齐。
void awaitSlotAlignsOverAlignedAwaitersOnTheHeap() {
    static_assert(not co2::detail::AwaitSlot<>::isInline<OverAlignedAwaiter>(),
                  "over-aligned awaiters never stay inline");

    constexpr int count = 16;
    auto destroyed = 0;
    {
        // 同时保留多个槽：单个分配碰巧对齐说明不了问题。
        auto slots = std::vector<std::unique_ptr<co2::detail::AwaitSlot<>>>{};
        for (auto index = 0; index != count; ++index) {
            slots.emplace_back(new co2::detail::AwaitSlot<>{});
            auto& stored =
                slots.back()->emplace<OverAlignedAwaiter>(OverAlignedAwaiter{destroyed});
            CHECK(reinterpret_cast<std::uintptr_t>(&stored) %
                      alignof(OverAlignedAwaiter) ==
                  0U);
            CHECK(&slots.back()->get<OverAlignedAwaiter>() == &stored);
        }
        CHECK(destroyed == 0);
        slots.front()->reset();
        CHECK(destroyed == 1);
    }
    CHECK(destroyed == count);
}

// ---------------------------------------------------------------------------
// MoveOnlyFunction

using Fn = co2::detail::MoveOnlyFunction<int(int)>;

struct ThreePointers {
    void* pointers[3];
    int operator()(int const v) const noexcept { return v; }
};

struct FourPointers {
    void* pointers[4];
    int operator()(int const v) const noexcept { return v; }
};

struct ThrowingMove {
    ThrowingMove() = default;
    ThrowingMove(ThrowingMove&&) {} // 非 noexcept：不能内联重定位
    int operator()(int const v) const noexcept { return v; }
};

void moveOnlyFunctionLayoutAndInlineBoundary() {
    static_assert(sizeof(Fn) == 4U * sizeof(void*),
                  "MoveOnlyFunction must be the size of std::function");
    static_assert(std::is_nothrow_move_constructible<Fn>::value &&
                      std::is_nothrow_move_assignable<Fn>::value,
                  "moving a MoveOnlyFunction must not throw");
    static_assert(Fn::isInline<ThreePointers>(), "three pointers fit inline");
    static_assert(not Fn::isInline<FourPointers>(), "four pointers spill to the heap");
    static_assert(not Fn::isInline<ThrowingMove>(),
                  "a throwing move constructor forces heap storage");
    static_assert(not std::is_copy_constructible<Fn>::value, "move-only");
}

// 内联与堆两种存储都要正确地：调用、重定位（移动后源为空、目标可用）、析构恰好一次。
void moveOnlyFunctionOwnsItsCallableInBothStorages() {
    auto live = 0;
    {
        auto tracked = TrackedValue{live};
        // 捕获一个 TrackedValue（8 字节）：内联。
        auto inlineFn = Fn{[t = std::move(tracked)](int const v) { return v + 1; }};
        CHECK(live == 2); // 原 tracked（已移出但仍存活）+ 闭包里的一份
        CHECK(static_cast<bool>(inlineFn));
        CHECK(inlineFn(1) == 2);

        auto moved = std::move(inlineFn);
        CHECK(not inlineFn);
        CHECK(live == 2); // 重定位：目标构造、源销毁，净数不变
        CHECK(moved(2) == 3);

        // 堆存储：闭包超过内联容量。
        auto heapFn = Fn{[t = std::move(tracked), pad = FourPointers{}](int const v) {
            static_cast<void>(pad);
            return v * 10;
        }};
        CHECK(live == 3);
        CHECK(heapFn(4) == 40);
        auto heapMoved = std::move(heapFn);
        CHECK(not heapFn);
        CHECK(live == 3); // 偷指针，不构造不销毁
        CHECK(heapMoved(5) == 50);

        // 移动赋值先销毁目标原有的闭包。
        heapMoved = std::move(moved);
        CHECK(not moved);
        CHECK(live == 2);
        CHECK(heapMoved(6) == 7);

        heapMoved.reset();
        CHECK(not heapMoved);
        CHECK(live == 1);
    }
    CHECK(live == 0);

    auto empty = Fn{};
    CHECK(not empty);
    auto fromEmpty = std::move(empty);
    CHECK(not fromEmpty);
}

void moveOnlyFunctionAcceptsMoveOnlyCallables() {
    auto owned = std::unique_ptr<int>{new int{41}};
    auto fn = co2::detail::MoveOnlyFunction<int()>{
        [p = std::move(owned)]() mutable { return ++*p; }};
    CHECK(owned == nullptr);
    CHECK(fn() == 42);
    CHECK(fn() == 43);

    auto throwingMove = co2::detail::MoveOnlyFunction<int(int)>{ThrowingMove{}};
    auto relocated = std::move(throwingMove); // 堆存储：即便 F 的移动会抛，这里也不抛
    CHECK(relocated(9) == 9);
}

} // namespace

int main() {
    resultStorageOwnsExactlyOneValue();
    awaitSlotFallsBackToTheHeapForLargeAwaiters();
    awaitSlotTypedResetDestroysExactlyOnce();
    awaitSlotAlignsOverAlignedAwaitersOnTheHeap();
    moveOnlyFunctionLayoutAndInlineBoundary();
    moveOnlyFunctionOwnsItsCallableInBothStorages();
    moveOnlyFunctionAcceptsMoveOnlyCallables();
    return 0;
}
