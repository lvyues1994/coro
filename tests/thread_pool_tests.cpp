// co2 v2 执行层：工作窃取线程池。

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "co2/coroutine.hpp"
#include "co2/scheduler.hpp"
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

// 即发即弃的根：帧在协程体结束时自毁。
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

struct ThreadIds {
    void record() {
        std::lock_guard<std::mutex> lock{mutex};
        ids.insert(std::this_thread::get_id());
    }

    std::size_t distinct() {
        std::lock_guard<std::mutex> lock{mutex};
        return ids.size();
    }

    std::mutex mutex;
    std::set<std::thread::id> ids;
};

auto job(co2::ThreadPool& pool, Completion& completion, ThreadIds& ids, int sleepMs,
         int extraHops) CO2_BEG(Detached, (pool, completion, ids, sleepMs, extraHops)) {
    CO2_AWAIT(co2::scheduleOn(pool));
    while (extraHops-- > 0)
        CO2_AWAIT(co2::scheduleOn(pool));
    ids.record();
    if (sleepMs > 0) std::this_thread::sleep_for(std::chrono::milliseconds{sleepMs});
    completion.signal();
}
CO2_END

// ---------------------------------------------------------------------------

auto hopAndReport(co2::ThreadPool& pool, bool& onWorkerBefore, bool& onWorkerAfter,
                  std::thread::id& resumedOn)
    CO2_BEG(co2::Task<int>, (pool, onWorkerBefore, onWorkerAfter, resumedOn)) {
    onWorkerBefore = pool.isWorkerThread();
    CO2_AWAIT(co2::scheduleOn(pool));
    onWorkerAfter = pool.isWorkerThread();
    resumedOn = std::this_thread::get_id();
    CO2_RETURN(42);
}
CO2_END

void scheduleOnMovesTheCoroutineToAWorkerThread() {
    co2::ThreadPool pool{2U};
    CHECK(pool.threadCount() == 2U);
    CHECK(not pool.isWorkerThread());
    bool before = true;
    bool after = false;
    auto resumedOn = std::thread::id{};
    CHECK(co2::syncWait(hopAndReport(pool, before, after, resumedOn)) == 42);
    CHECK(not before);
    CHECK(after);
    CHECK(resumedOn != std::this_thread::get_id());
}

auto failsOnWorker(co2::ThreadPool& pool) CO2_BEG(co2::Task<int>, (pool)) {
    CO2_AWAIT(co2::scheduleOn(pool));
    throw ExpectedError{};
    CO2_RETURN(0);
}
CO2_END

void exceptionsFromWorkersReachSyncWait() {
    co2::ThreadPool pool{2U};
    bool thrown = false;
    try {
        co2::syncWait(failsOnWorker(pool));
    } catch (ExpectedError const&) {
        thrown = true;
    }
    CHECK(thrown);
}

void injectedWorkIsSpreadAcrossWorkers() {
    co2::ThreadPool pool{4U};
    Completion completion;
    ThreadIds ids;
    constexpr int count = 64;
    for (int i = 0; i < count; ++i)
        job(pool, completion, ids, 2, 0);
    completion.waitFor(count);
    CHECK(ids.distinct() >= 2U);
}

auto launchFromWorker(co2::ThreadPool& pool, Completion& completion, ThreadIds& ids,
                      int count, int sleepMs)
    CO2_BEG(co2::Task<>, (pool, completion, ids, count, sleepMs), int index{};) {
    CO2_AWAIT(co2::scheduleOn(pool));
    CHECK(pool.isWorkerThread());
    for (index = 0; index < count; ++index)
        job(pool, completion, ids, sleepMs, 0);
}
CO2_END

void localWorkIsStolenByIdleWorkers() {
    co2::ThreadPool pool{4U};
    Completion completion;
    ThreadIds ids;
    constexpr int count = 64;
    co2::syncWait(launchFromWorker(pool, completion, ids, count, 2));
    completion.waitFor(count);
    CHECK(ids.distinct() >= 2U);
}

void localQueueOverflowSpillsToTheGlobalQueue() {
    co2::ThreadPool pool{2U};
    Completion completion;
    ThreadIds ids;
    // 远超本地队列容量（1024）的一次性提交。
    constexpr int count = 5000;
    co2::syncWait(launchFromWorker(pool, completion, ids, count, 0));
    completion.waitFor(count);
    CHECK(completion.done == count);
}

void destructorDrainsQueuedWork() {
    Completion completion;
    ThreadIds ids;
    constexpr int count = 1000;
    {
        co2::ThreadPool pool{3U};
        // 每个作业在排空期间再 hop 两次：排空期间新排入的工作也要跑完。
        for (int i = 0; i < count; ++i)
            job(pool, completion, ids, 0, 2);
    }
    CHECK(completion.done == count);
}

void singleThreadedPoolRunsEverything() {
    co2::ThreadPool pool{1U};
    Completion completion;
    ThreadIds ids;
    constexpr int count = 200;
    for (int i = 0; i < count; ++i)
        job(pool, completion, ids, 0, 1);
    completion.waitFor(count);
    CHECK(ids.distinct() == 1U);
}

auto crossPools(co2::ThreadPool& first, co2::ThreadPool& second, int& stage)
    CO2_BEG(co2::Task<>, (first, second, stage)) {
    CO2_AWAIT(co2::scheduleOn(first));
    CHECK(first.isWorkerThread() && not second.isWorkerThread());
    stage = 1;
    CO2_AWAIT(co2::scheduleOn(second));
    CHECK(second.isWorkerThread() && not first.isWorkerThread());
    stage = 2;
    CO2_AWAIT(co2::scheduleOn(first));
    CHECK(first.isWorkerThread());
    stage = 3;
}
CO2_END

void schedulingFromAnotherPoolsWorkerGoesThroughTheGlobalQueue() {
    co2::ThreadPool first{2U};
    co2::ThreadPool second{2U};
    int stage = 0;
    co2::syncWait(crossPools(first, second, stage));
    CHECK(stage == 3);
}

auto recorder(co2::ThreadPool& pool, std::vector<std::string>& log, char const* name)
    CO2_BEG(Detached, (pool, log, name)) {
    CO2_AWAIT(co2::scheduleOn(pool));
    log.push_back(name);
}
CO2_END

auto yieldsToLocalWork(co2::ThreadPool& pool, std::vector<std::string>& log)
    CO2_BEG(co2::Task<>, (pool, log)) {
    CO2_AWAIT(co2::scheduleOn(pool));
    recorder(pool, log, "a");
    recorder(pool, log, "b");
    log.push_back("yield");
    CO2_AWAIT(co2::scheduleOn(pool));
    log.push_back("resumed");
}
CO2_END

// 本地队列是 FIFO：在工作线程上 scheduleOn(pool) 让排在前面的本地工作先跑。
void reschedulingOnAWorkerYieldsToEarlierLocalWork() {
    co2::ThreadPool pool{1U};
    std::vector<std::string> log;
    co2::syncWait(yieldsToLocalWork(pool, log));
    CHECK((log == std::vector<std::string>{"yield", "a", "b", "resumed"}));
}

auto hopper(co2::ThreadPool& pool, Completion& completion, ThreadIds& ids, int hops)
    CO2_BEG(Detached, (pool, completion, ids, hops)) {
    while (hops-- > 0) {
        CO2_AWAIT(co2::scheduleOn(pool));
        ids.record();
    }
    completion.signal();
}
CO2_END

void manyConcurrentHoppersFinishWithoutLosingWakeups() {
    co2::ThreadPool pool{4U};
    Completion completion;
    ThreadIds ids;
    constexpr int hoppers = 16;
    constexpr int hopsEach = 5000;
    for (int i = 0; i < hoppers; ++i)
        hopper(pool, completion, ids, hopsEach);
    completion.waitFor(hoppers);
    // 只断言"全部完成"：线程分布已由带 sleep 的两个用例覆盖；不 sleep 的 hopper 在
    // valgrind 这类串行化线程的环境下可能全部落在一个工作线程上。
    CHECK(ids.distinct() >= 1U);
}

} // namespace

int main() {
    scheduleOnMovesTheCoroutineToAWorkerThread();
    exceptionsFromWorkersReachSyncWait();
    injectedWorkIsSpreadAcrossWorkers();
    localWorkIsStolenByIdleWorkers();
    localQueueOverflowSpillsToTheGlobalQueue();
    destructorDrainsQueuedWork();
    singleThreadedPoolRunsEverything();
    schedulingFromAnotherPoolsWorkerGoesThroughTheGlobalQueue();
    reschedulingOnAWorkerYieldsToEarlierLocalWork();
    manyConcurrentHoppersFinishWithoutLosingWakeups();
    return 0;
}
