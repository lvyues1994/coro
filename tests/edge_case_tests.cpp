// 边角用例：宏的逃生口、大 awaiter 的堆回落、生成器与 stop_token 的生命周期边界、
// 跨线程的 AsyncGenerator、外部多线程并发提交、嵌套 spawn。

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "co2/async_generator.hpp"
#include "co2/coroutine.hpp"
#include "co2/detail/result_storage.hpp"
#include "co2/generator.hpp"
#include "co2/manual_executor.hpp"
#include "co2/scheduler.hpp"
#include "co2/spawn.hpp"
#include "co2/stop_token.hpp"
#include "co2/sync_wait.hpp"
#include "co2/task.hpp"
#include "co2/thread_pool.hpp"

namespace {

void fail(char const* expression, int line) {
    std::cerr << "check failed at line " << line << ": " << expression << '\n';
    std::exit(EXIT_FAILURE);
}

#define CHECK(expression)                                                              \
    do {                                                                               \
        if (not(expression)) fail(#expression, __LINE__);                              \
    } while (false)

struct ExpectedError : std::runtime_error {
    ExpectedError() : std::runtime_error{"expected"} {}
};

struct Counters {
    int constructed{};
    int destroyed{};
    int alive() const noexcept { return constructed - destroyed; }
};

struct Tracked {
    explicit Tracked(Counters& counters_) noexcept : counters{&counters_} {
        ++counters->constructed;
    }
    Tracked(Tracked&& other) noexcept : counters{other.counters} {
        ++counters->constructed;
    }
    Tracked(Tracked const&) = delete;
    Tracked& operator=(Tracked const&) = delete;
    Tracked& operator=(Tracked&&) = delete;
    ~Tracked() { ++counters->destroyed; }
    Counters* counters;
};

// ---------------------------------------------------------------------------
// CO2_AWAIT_AS：表达式含 lambda 时 decltype 不可用，显式给出 awaitable 类型。

struct IntCompute {
    int (*function)(int);
    int argument;
    bool await_ready() const noexcept { return true; }
    void await_suspend(co2::coroutine_handle<>) const noexcept {}
    int await_resume() const { return function(argument); }
};

int triple(int v) { return v * 3; }

// 花括号不保护宏参数里的逗号，含逗号的表达式要再包一层圆括号。
auto usesAwaitAs(int seed) CO2_BEG(co2::Task<int>, (seed), int value{};) {
    CO2_AWAIT_AS(IntCompute, (IntCompute{&triple, seed}));
    CO2_AWAIT_AS_SET(value, IntCompute,
                     (IntCompute{[](int v) { return v + 1; }, seed}));
    CO2_RETURN(value);
}
CO2_END

void awaitAsAcceptsExpressionsWithLambdas() {
    CHECK(co2::syncWait(usesAwaitAs(41)) == 42);
}

// ---------------------------------------------------------------------------
// 大 awaiter 经宏路径回落到堆，语义不变。

struct LargeReady {
    explicit LargeReady(int v) noexcept : value{v} {}
    bool await_ready() const noexcept { return false; }
    bool await_suspend(co2::coroutine_handle<>) const noexcept { return false; }
    int await_resume() const noexcept { return value; }
    int value;
    unsigned char padding[CO2_AWAIT_STORAGE_SIZE * 4U]{};
};

auto awaitsLarge(int v) CO2_BEG(co2::Task<int>, (v), int result{};) {
    static_assert(not co2::detail::AwaitSlot<>::isInline<LargeReady>(),
                  "the awaiter must exceed the inline slot");
    CO2_AWAIT_SET(result, LargeReady{v});
    CO2_AWAIT_SET(result, LargeReady{result + 1});
    CO2_RETURN(result);
}
CO2_END

void largeAwaitersFallBackToTheHeapTransparently() {
    CHECK(co2::syncWait(awaitsLarge(1)) == 2);
}

// ---------------------------------------------------------------------------
// Generator 边界

auto trackedGenerator(Tracked tracked, int limit)
    CO2_BEG(co2::Generator<int>, (tracked, limit), int i{};) {
    for (i = 0; i < limit; ++i)
        CO2_YIELD(i);
}
CO2_END

void unstartedGeneratorReleasesItsParameters() {
    Counters counters;
    {
        auto generator = trackedGenerator(Tracked{counters}, 3);
        CHECK(counters.alive() == 1);
    }
    CHECK(counters.alive() == 0);
}

auto throwsBeforeFirstYield() CO2_BEG(co2::Generator<int>, ()) {
    throw ExpectedError{};
    CO2_YIELD(0);
}
CO2_END

void generatorThrowingBeforeTheFirstYieldThrowsFromBegin() {
    auto generator = throwsBeforeFirstYield();
    bool thrown = false;
    try {
        generator.begin();
    } catch (ExpectedError const&) {
        thrown = true;
    }
    CHECK(thrown);
}

auto nestedThrowsBeforeFirstYield() CO2_BEG(co2::Generator<int>, ()) {
    CO2_YIELD(1);
    CO2_YIELD(co2::elements_of(throwsBeforeFirstYield()));
    CO2_YIELD(2);
}
CO2_END

void nestedGeneratorThrowingBeforeItsFirstYieldPropagates() {
    auto generator = nestedThrowsBeforeFirstYield();
    auto it = generator.begin();
    CHECK(*it == 1);
    bool thrown = false;
    try {
        ++it;
    } catch (ExpectedError const&) {
        thrown = true;
    }
    CHECK(thrown);
    CHECK(it == generator.end());
}

auto stringsByConstRef(std::vector<std::string> const& items)
    CO2_BEG((co2::Generator<std::string const&, std::string>), (items),
            std::size_t i{};) {
    for (i = 0; i < items.size(); ++i)
        CO2_YIELD(items[i]);
}
CO2_END

void explicitValueTypeCopiesOnlyWhenTheConsumerAsks() {
    std::vector<std::string> const items{"a", "bb"};
    std::vector<std::string> copies;
    for (auto const& item : stringsByConstRef(items))
        copies.push_back(item);
    CHECK((copies == items));
    static_assert(
        std::is_same<co2::Generator<std::string const&, std::string>::value_type,
                     std::string>::value,
        "explicit value type");
}

// ---------------------------------------------------------------------------
// AsyncGenerator：生产者在线程池上 yield，消费者在生产者所在线程恢复。

auto poolTicks(co2::ThreadPool& pool, int count)
    CO2_BEG(co2::AsyncGenerator<int>, (pool, count), int i{};) {
    for (i = 0; i < count; ++i) {
        CO2_AWAIT(co2::scheduleOn(pool));
        CO2_YIELD(i);
    }
}
CO2_END

auto consumePoolTicks(co2::ThreadPool& pool, int count,
                      std::set<std::thread::id>& threads)
    CO2_BEG(co2::Task<int>, (pool, count, threads), co2::AsyncGenerator<int> stream;
            bool has{}; int total{};) {
    stream = poolTicks(pool, count);
    CO2_AWAIT_SET(has, stream.next());
    while (has) {
        threads.insert(std::this_thread::get_id());
        CHECK(pool.isWorkerThread());
        total += stream.value();
        CO2_AWAIT_SET(has, stream.next());
    }
    CO2_RETURN(total);
}
CO2_END

void asyncGeneratorConsumerFollowsTheProducerAcrossThreads() {
    co2::ThreadPool pool{2U};
    std::set<std::thread::id> threads;
    CHECK(co2::syncWait(consumePoolTicks(pool, 100, threads)) == 4950);
    CHECK(not threads.empty());
    CHECK(threads.count(std::this_thread::get_id()) == 0U);
}

// ---------------------------------------------------------------------------
// stop_token 生命周期边界

void tokensAndCallbacksOutliveTheirSource() {
    int calls = 0;
    co2::stop_token token;
    std::unique_ptr<co2::stop_callback<>> callback;
    {
        co2::stop_source source;
        token = source.get_token();
        callback.reset(new co2::stop_callback<>{token, [&] { ++calls; }});
        CHECK(token.stop_possible());
    }
    // 源已销毁：不可能再请求；回调永不执行；状态由 token/callback 保活并在最后释放。
    CHECK(not token.stop_possible());
    CHECK(not token.stop_requested());
    callback.reset();
    CHECK(calls == 0);
    auto copy = token;
    token = co2::stop_token{};
    CHECK(not copy.stop_possible());
}

void requestAfterEveryTokenIsGoneIsStillWellDefined() {
    co2::stop_source source;
    { auto token = source.get_token(); }
    CHECK(source.request_stop());
    CHECK(source.stop_requested());
    auto late = source.get_token();
    CHECK(late.stop_requested());
}

// ---------------------------------------------------------------------------
// syncWait 接受任意 awaitable

void syncWaitAcceptsBareAwaiters() {
    co2::syncWait(co2::suspend_never{});
    co2::ManualExecutor executor;
    std::thread driver{[&] {
        while (executor.run() == 0U)
            std::this_thread::yield();
    }};
    co2::syncWait(co2::scheduleOn(executor));
    driver.join();
}

// ---------------------------------------------------------------------------
// ThreadPool：默认线程数、外部多线程并发提交、嵌套 spawn

void defaultThreadCountFollowsHardwareConcurrency() {
    co2::ThreadPool pool;
    auto const detected = std::thread::hardware_concurrency();
    CHECK(pool.threadCount() >= 1U);
    if (detected != 0U) CHECK(pool.threadCount() == detected);
}

struct Detached {
    struct promise_type {
        Detached get_return_object() noexcept { return Detached{}; }
        co2::suspend_never initial_suspend() noexcept { return {}; }
        co2::suspend_never final_suspend() noexcept { return {}; }
        void return_void() noexcept {}
        void unhandled_exception() noexcept { std::terminate(); }
    };
};

struct Completion {
    void signal() {
        std::lock_guard<std::mutex> lock{mutex};
        ++done;
        changed.notify_all();
    }
    void waitFor(int const count) {
        std::unique_lock<std::mutex> lock{mutex};
        changed.wait(lock, [&] { return done >= count; });
    }
    std::mutex mutex;
    std::condition_variable changed;
    int done{};
};

auto externalJob(co2::ThreadPool& pool, Completion& completion, std::atomic<long>& sum,
                 int v) CO2_BEG(Detached, (pool, completion, sum, v)) {
    CO2_AWAIT(co2::scheduleOn(pool));
    sum.fetch_add(v);
    CO2_AWAIT(co2::scheduleOn(pool));
    completion.signal();
}
CO2_END

void manyExternalThreadsSubmitConcurrently() {
    co2::ThreadPool pool{3U};
    Completion completion;
    std::atomic<long> sum{0};
    constexpr int producers = 8;
    constexpr int perProducer = 500;
    std::vector<std::thread> threads;
    for (int p = 0; p < producers; ++p) {
        threads.emplace_back([&, p] {
            for (int i = 0; i < perProducer; ++i)
                externalJob(pool, completion, sum, p * perProducer + i);
        });
    }
    for (auto& thread : threads)
        thread.join();
    completion.waitFor(producers * perProducer);
    long expected = 0;
    for (int i = 0; i < producers * perProducer; ++i)
        expected += i;
    CHECK(sum.load() == expected);
}

auto inner(co2::ThreadPool& pool, int v) CO2_BEG(co2::Task<int>, (pool, v)) {
    CO2_AWAIT(co2::scheduleOn(pool));
    CO2_RETURN(v * 2);
}
CO2_END

auto outer(co2::ThreadPool& pool, int v)
    CO2_BEG(co2::Task<int>, (pool, v), int a{}; int b{};) {
    CO2_AWAIT(co2::scheduleOn(pool));
    CO2_AWAIT_SET(a, co2::spawn(pool, inner(pool, v)));
    CO2_AWAIT_SET(b, co2::spawn(pool, inner(pool, v + 1)));
    CO2_RETURN(a + b);
}
CO2_END

void spawnedTasksMaySpawnAndAwaitOnTheSamePool() {
    co2::ThreadPool pool{2U};
    CHECK(co2::spawn(pool, outer(pool, 1)).join() == 6);
}

} // namespace

int main() {
    awaitAsAcceptsExpressionsWithLambdas();
    largeAwaitersFallBackToTheHeapTransparently();
    unstartedGeneratorReleasesItsParameters();
    generatorThrowingBeforeTheFirstYieldThrowsFromBegin();
    nestedGeneratorThrowingBeforeItsFirstYieldPropagates();
    explicitValueTypeCopiesOnlyWhenTheConsumerAsks();
    asyncGeneratorConsumerFollowsTheProducerAcrossThreads();
    tokensAndCallbacksOutliveTheirSource();
    requestAfterEveryTokenIsGoneIsStillWellDefined();
    syncWaitAcceptsBareAwaiters();
    defaultThreadCountFollowsHardwareConcurrency();
    manyExternalThreadsSubmitConcurrently();
    spawnedTasksMaySpawnAndAwaitOnTheSamePool();
    return 0;
}
