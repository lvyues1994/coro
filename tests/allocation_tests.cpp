// 分配计数：把"零分配热路径"从口头承诺变成断言。全局 operator new 计数，逐个操作
// 检查分配次数。

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <new>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "co2/callback.hpp"
#include "co2/coroutine.hpp"
#include "co2/detail/await_slot.hpp"
#include "co2/generator.hpp"
#include "co2/manual_executor.hpp"
#include "co2/scheduler.hpp"
#include "co2/spawn.hpp"
#include "co2/stop_token.hpp"
#include "co2/sync_wait.hpp"
#include "co2/task.hpp"
#include "co2/thread_pool.hpp"
#include "co2/when_all.hpp"

// ThreadSanitizer 的运行时自带 operator new/delete，不能再替换；分配计数在它下面也
// 没有意义，直接跳过。
#if defined(__SANITIZE_THREAD__)
#define CO2_TEST_UNDER_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define CO2_TEST_UNDER_TSAN 1
#endif
#endif

#if defined(CO2_TEST_UNDER_TSAN)

int main() {
    std::cout << "allocation tests skipped under ThreadSanitizer\n";
    return 0;
}

#else

namespace {

std::atomic<long> allocations{0};

long allocationsSoFar() noexcept { return allocations.load(std::memory_order_relaxed); }

void fail(char const* expression, int line) {
    std::cerr << "check failed at line " << line << ": " << expression << '\n';
    std::exit(EXIT_FAILURE);
}

#define CHECK(expression)                                                              \
    do {                                                                               \
        if (not(expression)) fail(#expression, __LINE__);                              \
    } while (false)

#define CHECK_ALLOCATIONS(expected, ...)                                               \
    do {                                                                               \
        auto const before_ = allocationsSoFar();                                       \
        __VA_ARGS__;                                                                   \
        auto const count_ = allocationsSoFar() - before_;                              \
        if (count_ != (expected)) {                                                    \
            std::cerr << "allocation mismatch at line " << __LINE__ << ": expected "   \
                      << (expected) << ", got " << count_ << '\n';                     \
            std::exit(EXIT_FAILURE);                                                   \
        }                                                                              \
    } while (false)

// ---------------------------------------------------------------------------

auto counting(int limit) CO2_BEG(co2::Generator<int>, (limit), int index{};) {
    for (index = 0; index < limit; ++index)
        CO2_YIELD(index);
}
CO2_END

void generatorIterationDoesNotAllocate() {
    auto generator = counting(10000); // 一次：帧
    long total = 0;
    CHECK_ALLOCATIONS(0, for (auto value : generator) total += value);
    CHECK(total == 10000L * 9999L / 2L);
}

auto leaf(int v) CO2_BEG(co2::Task<int>, (v)) { CO2_RETURN(v); }
CO2_END

auto awaitsLeaf(int v) CO2_BEG(co2::Task<int>, (v), int r{};) {
    CO2_AWAIT_SET(r, leaf(v)); // 一次：子帧
    CO2_RETURN(r);
}
CO2_END

void taskCreationIsExactlyOneAllocationPerFrame() {
    CHECK_ALLOCATIONS(1, { auto task = leaf(1); });
    // 外层帧 + 子帧 = 2；syncWait 自身不分配（续体帧在栈上）。
    CHECK_ALLOCATIONS(2, CHECK(co2::syncWait(awaitsLeaf(2)) == 2));
}

auto hops(co2::Scheduler& scheduler, int count)
    CO2_BEG(co2::Task<int>, (scheduler, count), int done{};) {
    while (done < count) {
        CO2_AWAIT(co2::scheduleOn(scheduler));
        ++done;
    }
    CO2_RETURN(done);
}
CO2_END

void manualExecutorHopsDoNotAllocateInSteadyState() {
    co2::ManualExecutor executor;
    // 预热：deque 的第一个块。
    {
        auto task = hops(executor, 1);
        co2::detail::TaskAccess::start(task, co2::noop_coroutine());
        executor.run();
    }
    auto task = hops(executor, 1000);
    co2::detail::TaskAccess::start(task, co2::noop_coroutine());
    CHECK_ALLOCATIONS(0, CHECK(executor.run() == 1000U));
    CHECK(co2::detail::TaskAccess::takeResult(task) == 1000);
}

void threadPoolHopsDoNotAllocateInSteadyState() {
    co2::ThreadPool pool{2U};
    co2::spawn(pool, hops(pool, 100)).join(); // 预热：全局队列的第一个块
    // 工作线程上的 10000 次 hop 走本地队列，零分配；spawn 本身是帧 + JoinState +
    // stop 状态三次。
    CHECK_ALLOCATIONS(3, CHECK(co2::spawn(pool, hops(pool, 10000)).join() == 10000));
}

void stopTokenOperationsAllocateOnlyTheState() {
    co2::stop_source source; // 一次：状态
    CHECK_ALLOCATIONS(0, {
        auto token = source.get_token();
        auto copy = token;
        auto other = source;
        CHECK(copy.stop_possible() && other.stop_possible());
    });
    int calls = 0;
    struct Callback {
        int* calls;
        void operator()() const noexcept { ++*calls; }
    };
    CHECK_ALLOCATIONS(0, {
        co2::stop_callback<Callback> callback{source.get_token(), Callback{&calls}};
        CHECK(source.request_stop());
    });
    CHECK(calls == 1);
}

void whenAllTupleAllocatesChildFramesStopStateAndAwaiterSlot() {
    // 3 个子帧 + 1 个 stop 状态 + 1 次 awaiter 槽回落（awaiter 内嵌每个 child
    // 的续体帧， 远大于 64 字节的内联槽）；syncWait(awaitable) 另需 1 个包装 Task 帧。
    CHECK_ALLOCATIONS(6, {
        auto const results = co2::syncWait(co2::whenAll(leaf(1), leaf(2), leaf(3)));
        CHECK(std::get<0>(results) + std::get<1>(results) + std::get<2>(results) == 6);
    });
}

auto awaitsWhenAll() CO2_BEG(co2::Task<int>, (), std::tuple<int, int> r;) {
    CO2_AWAIT_SET(r, co2::whenAll(leaf(1), leaf(2)));
    CO2_RETURN(std::get<0>(r) + std::get<1>(r));
}
CO2_END

void whenAllInsideATaskAllocatesChildFramesStopStateAndAwaiterSlot() {
    // 外层帧 1 + 子帧 2 + stop 状态 1 + awaiter 槽回落 1。
    CHECK_ALLOCATIONS(5, CHECK(co2::syncWait(awaitsWhenAll()) == 3));
}

auto viaCallback(int v) CO2_BEG(co2::Task<int>, (v), int got{};) {
    // 闭包只捕获 this：落在 MoveOnlyFunction 的内联缓冲里，不分配；awaiter 本身也在
    // 帧的内联 awaiter 槽里。
    static_assert(co2::detail::AwaitSlot<>::isInline<co2::CallbackAwaitable<int>>(),
                  "the callback awaiter must stay within the inline awaiter slot");
    CO2_AWAIT_AS_SET(
        got, co2::CallbackAwaitable<int>,
        co2::fromCallback<int>([this](co2::Continuation<int> done) { done(v); }));
    CO2_RETURN(got);
}
CO2_END

void callbackAwaiterAllocatesNothingBeyondTheFrame() {
    CHECK_ALLOCATIONS(1, CHECK(co2::syncWait(viaCallback(6)) == 6));
}

auto viaCallbackWithLargeClosure(std::unique_ptr<int> payload, int a, int b)
    CO2_BEG(co2::Task<int>, (payload, a, b), int got{};) {
    // 捕获 unique_ptr + 一个指针 + 一个 int（补齐后 24 字节）：正好是 3 个指针的内联
    // 容量上限，不分配；move-only 捕获也照常编译。
    CO2_AWAIT_AS_SET(
        got, co2::CallbackAwaitable<int>,
        co2::fromCallback<int>(
            [p = std::move(payload), pad = static_cast<void*>(nullptr), sum = a + b](
                co2::Continuation<int> done, co2::stop_token) mutable {
                done(*p + sum + (pad == nullptr ? 0 : 1));
            }));
    CO2_RETURN(got);
}
CO2_END

void callbackAwaiterKeepsAThreePointerClosureInline() {
    // 帧 1 次；unique_ptr 的 new int 在计数块之外。
    auto payload = std::unique_ptr<int>{new int{1}};
    CHECK_ALLOCATIONS(
        1, CHECK(co2::syncWait(viaCallbackWithLargeClosure(std::move(payload), 2, 3)) ==
                 6));
}

void spawnIsTwoAllocationsBeyondTheFrame() {
    // 帧 + JoinState + 它自己的 stop 状态。
    co2::ManualExecutor executor;
    {
        auto warmUp = co2::spawn(executor, leaf(0)); // 队列的首次扩容
        executor.run();
        warmUp.join();
    }
    CHECK_ALLOCATIONS(3, {
        auto handle = co2::spawn(executor, leaf(4));
        executor.run();
        CHECK(handle.join() == 4);
    });
}

} // namespace

// 计数的全局 operator new/delete。
// GCC 在 -O2/-O3 下会把下面的 operator delete 内联，看到 free() 作用于 operator new 返回的
// 指针就报 -Wmismatched-new-delete——它不知道这里的 operator new 本身就是 malloc 实现的。
// 这是替换全局分配函数时的已知误报，只在本文件里关掉。
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif

void* operator new(std::size_t const size) {
    allocations.fetch_add(1, std::memory_order_relaxed);
    if (auto* const pointer = std::malloc(size == 0U ? 1U : size)) return pointer;
    throw std::bad_alloc{};
}

void operator delete(void* const pointer) noexcept { std::free(pointer); }
void operator delete(void* const pointer, std::size_t) noexcept { std::free(pointer); }

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

int main() {
    // valgrind 等工具会用自己的 operator new 插入到本文件的替换之前：那时计数为零，
    // 断言没有意义，跳过。
    {
        auto const before = allocationsSoFar();
        std::unique_ptr<int> probe{new int{0}};
        if (allocationsSoFar() == before) {
            std::cout << "allocation tests skipped: operator new is interposed\n";
            return 0;
        }
    }
    generatorIterationDoesNotAllocate();
    taskCreationIsExactlyOneAllocationPerFrame();
    manualExecutorHopsDoNotAllocateInSteadyState();
    threadPoolHopsDoNotAllocateInSteadyState();
    stopTokenOperationsAllocateOnlyTheState();
    whenAllTupleAllocatesChildFramesStopStateAndAwaiterSlot();
    whenAllInsideATaskAllocatesChildFramesStopStateAndAwaiterSlot();
    callbackAwaiterAllocatesNothingBeyondTheFrame();
    callbackAwaiterKeepsAThreePointerClosureInline();
    spawnIsTwoAllocationsBeyondTheFrame();
    return 0;
}

#endif
