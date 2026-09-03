// 帧内存储构件：ResultStorage<T> 与 AwaitSlot（内联 + 堆回落）。

#include <cstdlib>
#include <iostream>
#include <utility>

#include "co2/config.hpp"
#include "co2/detail/await_slot.hpp"
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
        auto slot = co2::detail::AwaitSlot<>{};
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

} // namespace

int main() {
    resultStorageOwnsExactlyOneValue();
    awaitSlotFallsBackToTheHeapForLargeAwaiters();
    return 0;
}
