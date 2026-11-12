// co2 性能基准：无第三方依赖，每个基准跑若干轮，报告每次操作的中位耗时。
//
//   co2-benchmarks            运行全部
//   co2-benchmarks pool       只运行名字里含 "pool" 的
//
// 数字只在同一台机器、同一份编译选项下可比。线程池相关的几项对机器负载敏感，看量级与
// 前后对比，不要当作绝对值。

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <exception>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "co2/co2.hpp"

namespace {

using Clock = std::chrono::steady_clock;

// 阻止编译器把被测循环的结果整体消除。
volatile long sink;

struct Benchmark {
    char const* name;
    double (*run)(); // 返回一轮的 ns/op
};

template <class Body> double nanosecondsPerOp(std::size_t const ops, Body&& body) {
    auto const start = Clock::now();
    body();
    auto const elapsed =
        std::chrono::duration<double, std::nano>(Clock::now() - start).count();
    return elapsed / static_cast<double>(ops);
}

// ---------------------------------------------------------------------------
// 核心：原始的挂起/恢复

struct Raw {
    struct promise_type {
        Raw get_return_object() noexcept {
            return Raw{co2::coroutine_handle<promise_type>::from_promise(*this)};
        }
        co2::suspend_always initial_suspend() noexcept { return {}; }
        co2::suspend_always final_suspend() noexcept { return {}; }
        void return_void() noexcept {}
        void unhandled_exception() noexcept { std::terminate(); }
    };
    co2::coroutine_handle<promise_type> handle;
};

auto suspendsForever(long& counter) CO2_BEG(Raw, (counter)) {
    for (;;) {
        ++counter;
        CO2_AWAIT(co2::suspend_always{});
    }
}
CO2_END

double rawSuspendResume() {
    constexpr std::size_t ops = 20000000U;
    long counter = 0;
    auto coroutine = suspendsForever(counter);
    auto const result = nanosecondsPerOp(ops, [&] {
        for (auto index = std::size_t{}; index != ops; ++index)
            coroutine.handle.resume();
    });
    sink = counter;
    coroutine.handle.destroy();
    return result;
}

// ---------------------------------------------------------------------------
// Generator

auto counting(int n) CO2_BEG(co2::Generator<int>, (n), int i{};) {
    for (i = 0; i < n; ++i)
        CO2_YIELD(i);
}
CO2_END

double generatorYieldNext() {
    constexpr int ops = 20000000;
    return nanosecondsPerOp(static_cast<std::size_t>(ops), [&] {
        long total = 0;
        for (auto const value : counting(ops))
            total += value;
        sink = total;
    });
}

auto nestedCounting(int depth, int n) CO2_BEG(co2::Generator<int>, (depth, n)) {
    if (depth == 0) {
        CO2_YIELD(co2::elements_of(counting(n)));
    } else {
        CO2_YIELD(co2::elements_of(nestedCounting(depth - 1, n)));
    }
}
CO2_END

double elementsOfDepth8() {
    constexpr int ops = 20000000;
    return nanosecondsPerOp(static_cast<std::size_t>(ops), [&] {
        long total = 0;
        for (auto const value : nestedCounting(7, ops))
            total += value;
        sink = total;
    });
}

// ---------------------------------------------------------------------------
// Task

auto leaf(int v) CO2_BEG(co2::Task<int>, (v)) { CO2_RETURN(v); }
CO2_END

auto awaitsLeaves(int n) CO2_BEG(co2::Task<long>, (n), int i{}; long total{}; int v{};) {
    for (i = 0; i < n; ++i) {
        CO2_AWAIT_SET(v, leaf(i));
        total += v;
    }
    CO2_RETURN(total);
}
CO2_END

double symmetricTaskTransfer() {
    constexpr int ops = 5000000;
    return nanosecondsPerOp(static_cast<std::size_t>(ops),
                            [&] { sink = co2::syncWait(awaitsLeaves(ops)); });
}

double syncWaitReadyTask() {
    constexpr int ops = 3000000;
    return nanosecondsPerOp(static_cast<std::size_t>(ops), [&] {
        long total = 0;
        for (auto index = 0; index != ops; ++index)
            total += co2::syncWait(leaf(index));
        sink = total;
    });
}

// 多个线程各自跑一条"等待子 Task"的循环。sharedToken 为真时所有线程的 Task 树共用同一个
// stop_token——whenAll / spawn 的各个分支落在不同线程上时就是这种形态。
double awaitChildOnThreads(int const threads, bool const sharedToken) {
    constexpr int ops = 2000000;
    co2::stop_source shared;
    return nanosecondsPerOp(static_cast<std::size_t>(ops), [&] {
        auto workers = std::vector<std::thread>{};
        for (auto index = 0; index != threads; ++index) {
            workers.emplace_back([&shared, sharedToken] {
                co2::stop_source own;
                auto token = sharedToken ? shared.get_token() : own.get_token();
                sink = co2::syncWait(awaitsLeaves(ops), std::move(token));
            });
        }
        for (auto& worker : workers)
            worker.join();
    });
}

double awaitChildOwnToken() { return awaitChildOnThreads(8, false); }
double awaitChildSharedToken() { return awaitChildOnThreads(8, true); }

// ---------------------------------------------------------------------------
// 执行层

auto hopper(co2::Scheduler& scheduler, int n)
    CO2_BEG(co2::Task<int>, (scheduler, n), int i{};) {
    for (i = 0; i < n; ++i)
        CO2_AWAIT(co2::scheduleOn(scheduler));
    CO2_RETURN(n);
}
CO2_END

double manualExecutorHop() {
    constexpr int ops = 5000000;
    co2::ManualExecutor executor;
    return nanosecondsPerOp(static_cast<std::size_t>(ops), [&] {
        auto handle = co2::spawn(executor, hopper(executor, ops));
        executor.run();
        sink = handle.join();
    });
}

// `tasks` 个 Task 各 hop `hopsPerTask` 次：墙上时间 / 总 hop 数。
double threadPoolHops(unsigned const workers, int const tasks, int const hopsPerTask) {
    co2::ThreadPool pool{workers};
    // 让工作线程先停车：测的是稳态，不是线程启动。
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
    auto const ops = static_cast<std::size_t>(tasks) * static_cast<std::size_t>(hopsPerTask);
    return nanosecondsPerOp(ops, [&] {
        auto handles = std::vector<co2::JoinHandle<int>>{};
        handles.reserve(static_cast<std::size_t>(tasks));
        for (auto index = 0; index != tasks; ++index)
            handles.push_back(co2::spawn(pool, hopper(pool, hopsPerTask)));
        long total = 0;
        for (auto& handle : handles)
            total += handle.join();
        sink = total;
    });
}

double threadPoolHopOneWorker() { return threadPoolHops(1U, 1, 2000000); }
double threadPoolHopSaturated() { return threadPoolHops(4U, 32, 100000); }
// 不饱和：一个 Task 在 8 个工作线程的池里反复让步，其余 7 个线程无事可做。
double threadPoolHopUnsaturated() { return threadPoolHops(8U, 1, 1000000); }

double spawnAndJoin() {
    constexpr int ops = 20000;
    co2::ThreadPool pool{1U};
    return nanosecondsPerOp(static_cast<std::size_t>(ops), [&] {
        long total = 0;
        for (auto index = 0; index != ops; ++index)
            total += co2::spawn(pool, leaf(index)).join();
        sink = total;
    });
}

// ---------------------------------------------------------------------------

Benchmark const benchmarks[] = {
    {"raw suspend/resume", &rawSuspendResume},
    {"Generator yield/next", &generatorYieldNext},
    {"elements_of depth 8 yield/next", &elementsOfDepth8},
    {"symmetric Task transfer (with frame allocation)", &symmetricTaskTransfer},
    {"syncWait ready Task", &syncWaitReadyTask},
    {"Task await child, 8 threads, own stop_token", &awaitChildOwnToken},
    {"Task await child, 8 threads, shared stop_token", &awaitChildSharedToken},
    {"ManualExecutor hop", &manualExecutorHop},
    {"ThreadPool hop (1 worker, 1 task)", &threadPoolHopOneWorker},
    {"ThreadPool hop (4 workers, 32 tasks)", &threadPoolHopSaturated},
    {"ThreadPool hop (8 workers, 1 task, unsaturated)", &threadPoolHopUnsaturated},
    {"spawn + join (cross-thread wakeup)", &spawnAndJoin},
};

struct Summary {
    double median;
    double best;
};

Summary summarize(Benchmark const& benchmark, int const rounds) {
    auto samples = std::vector<double>{};
    for (auto round = 0; round != rounds; ++round)
        samples.push_back(benchmark.run());
    std::sort(samples.begin(), samples.end());
    return Summary{samples[samples.size() / 2U], samples.front()};
}

} // namespace

int main(int const argc, char** const argv) {
    constexpr int rounds = 7;
    char const* const filter = argc > 1 ? argv[1] : "";
    std::printf("%-50s %12s %12s\n", "benchmark", "median ns/op", "best ns/op");
    for (auto const& benchmark : benchmarks) {
        if (std::strstr(benchmark.name, filter) == nullptr) continue;
        auto const summary = summarize(benchmark, rounds);
        std::printf("%-50s %12.1f %12.1f\n", benchmark.name, summary.median,
                    summary.best);
        std::fflush(stdout);
    }
    return 0;
}
