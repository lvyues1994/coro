// co2 v2 执行层：工作窃取线程池。

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "co2/coroutine.hpp"
#include "co2/scheduler.hpp"
#include "co2/spawn.hpp"
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
        ++recorded;
    }

    std::size_t distinct() {
        std::lock_guard<std::mutex> lock{mutex};
        return ids.size();
    }

    // record() 被调用的总次数。
    std::size_t total() {
        std::lock_guard<std::mutex> lock{mutex};
        return recorded;
    }

    bool contains(std::thread::id const id) {
        std::lock_guard<std::mutex> lock{mutex};
        return ids.count(id) != 0U;
    }

    std::mutex mutex;
    std::set<std::thread::id> ids;
    std::size_t recorded{};
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
    // 结构性断言，任何环境下都成立：每一跳恰好被记录一次（没有丢失的唤醒、没有重复
    // 恢复），且全部落在工作线程上。线程分布不在这里断言——它已由带 sleep 的两个用例
    // 覆盖；不 sleep 的 hopper 在 valgrind 这类串行化线程的环境下可能全部落在一个
    // 工作线程上。
    CHECK(ids.total() ==
          static_cast<std::size_t>(hoppers) * static_cast<std::size_t>(hopsEach));
    CHECK(not ids.contains(std::this_thread::get_id()));
}

// ---------------------------------------------------------------------------
// 本地队列：批量窃取

void* itemAt(std::size_t const index) {
    static int cells[64];
    return &cells[index];
}

void stealingMovesHalfOfTheVictimsQueueInOrder() {
    using co2::detail::LocalQueue;
    auto const victim = std::unique_ptr<LocalQueue>{new LocalQueue{}};
    auto const thief = std::unique_ptr<LocalQueue>{new LocalQueue{}};

    CHECK(victim->stealInto(*thief).status == LocalQueue::TakeStatus::Empty);

    for (auto index = std::size_t{}; index != 10U; ++index)
        CHECK(victim->push(itemAt(index)));
    CHECK(victim->snapshot().size == 10);

    // 取走一半（向上取整）：最老的一项交给调用方立刻运行，其余按原顺序进窃取者的队列。
    auto const stolen = victim->stealInto(*thief);
    CHECK(stolen.status == LocalQueue::TakeStatus::Taken);
    CHECK(stolen.item == itemAt(0U));
    CHECK(thief->snapshot().size == 4);
    CHECK(victim->snapshot().size == 5);
    for (auto index = std::size_t{1}; index != 5U; ++index)
        CHECK(thief->take().item == itemAt(index));
    for (auto index = std::size_t{5}; index != 10U; ++index)
        CHECK(victim->take().item == itemAt(index));
    CHECK(thief->looksEmpty() && victim->looksEmpty());

    // 只有一项时就取那一项，窃取者的队列不变。
    CHECK(victim->push(itemAt(20U)));
    auto const single = victim->stealInto(*thief);
    CHECK(single.status == LocalQueue::TakeStatus::Taken && single.item == itemAt(20U));
    CHECK(thief->looksEmpty() && victim->looksEmpty());

    // 队首位置随每次取走单调前进：窃取者靠它判断"主人是否在自己消化队列"。
    CHECK(victim->snapshot().head == 11);
}

// ---------------------------------------------------------------------------
// 活性：新的工作必须让足够多的工作线程醒着。

// `parties` 个参与者都到达之后才一起放行。
struct Barrier {
    explicit Barrier(int const parties_) : parties{parties_} {}

    void arriveAndWait() {
        std::unique_lock<std::mutex> lock{mutex};
        ++arrived;
        changed.notify_all();
        changed.wait(lock, [this] { return arrived >= parties; });
    }

    std::mutex mutex;
    std::condition_variable changed;
    int parties;
    int arrived{};
};

auto barrierJob(co2::ThreadPool& pool, Barrier& barrier, Completion& completion)
    CO2_BEG(Detached, (pool, barrier, completion)) {
    CO2_AWAIT(co2::scheduleOn(pool));
    barrier.arriveAndWait();
    completion.signal();
}
CO2_END

void spinFor(std::chrono::nanoseconds const duration) {
    auto const until = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < until) {
    }
}

// 每一轮新建一个线程池，等 `delay` 之后才让 `submit` 提交工作。delay 从 0 扫到 100 微秒：
// 刚启动的工作线程先自旋找活、找不到再停车，扫描让提交落在这个过渡期的各个位置上——
// 丢唤醒的窗口就在"最后一个搜索者正要去停车"的那几十纳秒里，固定的时序碰不到它。
template <class Submit> void sweepSubmissionTiming(Submit submit) {
    constexpr int workers = 4;
    constexpr int sweeps = 6;
    constexpr int steps = 200;
    for (auto round = 0; round != sweeps * steps; ++round) {
        co2::ThreadPool pool{static_cast<unsigned>(workers)};
        Barrier barrier{workers};
        Completion completion;
        spinFor(std::chrono::nanoseconds{(round % steps) * 500});
        submit(pool, barrier, completion, workers);
        completion.waitFor(workers);
    }
}

// 外部一口气提交 N 个会阻塞的作业：只有 N 个工作线程同时各持一个，才能全部放行。提交方
// 看到"已经有线程在找活"就不再唤醒，后续的线程要靠"找到活的最后一个搜索者再叫醒一个"
// 这条链逐个醒来；链断了，这个用例就永远等不到。
void everyWorkerIsRecruitedForInjectedWork() {
    sweepSubmissionTiming([](co2::ThreadPool& pool, Barrier& barrier,
                             Completion& completion, int const workers) {
        for (auto index = 0; index != workers; ++index)
            barrierJob(pool, barrier, completion);
    });
}

// 同上，但分两批：先到的一批把一部分线程叫醒并占住，第二批落在"有的线程在跑、最后一个
// 搜索者正要去停车"的状态上。第一批的大小与两批的间隔都在扫描。
void everyWorkerIsRecruitedForWorkArrivingInTwoBursts() {
    auto round = 0;
    sweepSubmissionTiming([&round](co2::ThreadPool& pool, Barrier& barrier,
                                   Completion& completion, int const workers) {
        auto const firstBurst = 1 + round % (workers - 1);
        auto const gap = std::chrono::nanoseconds{(round / (workers - 1) % 100) * 1000};
        ++round;
        for (auto index = 0; index != firstBurst; ++index)
            barrierJob(pool, barrier, completion);
        spinFor(gap);
        for (auto index = firstBurst; index != workers; ++index)
            barrierJob(pool, barrier, completion);
    });
}

auto launchesBarrierJobs(co2::ThreadPool& pool, Barrier& barrier, Completion& completion,
                         int count)
    CO2_BEG(Detached, (pool, barrier, completion, count), int index{};) {
    CO2_AWAIT(co2::scheduleOn(pool));
    for (index = 0; index < count; ++index)
        barrierJob(pool, barrier, completion);
    // 发起者自己也是一个参与者：它占着这个工作线程不放，排在它本地队列里的作业只能
    // 靠别的工作线程偷走。
    barrier.arriveAndWait();
    completion.signal();
}
CO2_END

void everyWorkerIsRecruitedForLocallyQueuedWork() {
    sweepSubmissionTiming([](co2::ThreadPool& pool, Barrier& barrier,
                             Completion& completion, int const workers) {
        launchesBarrierJobs(pool, barrier, completion, workers - 1);
    });
}

auto doubles(co2::ThreadPool& pool, int v) CO2_BEG(co2::Task<int>, (pool, v)) {
    CO2_AWAIT(co2::scheduleOn(pool));
    CO2_RETURN(v * 2);
}
CO2_END

auto blocksOnItsChild(co2::ThreadPool& pool, int v) CO2_BEG(co2::Task<int>, (pool, v)) {
    CO2_AWAIT(co2::scheduleOn(pool));
    // 在工作线程上阻塞式 join：子 Task 排在本线程的本地队列里，只剩它一项，而本线程
    // 不会回来取——必须有另一个工作线程被叫醒并把它偷走。
    CO2_RETURN(co2::spawn(pool, doubles(pool, v)).join());
}
CO2_END

void aWorkerBlockedOnItsOwnQueuedWorkIsRescuedByAnother() {
    co2::ThreadPool pool{2U};
    for (auto round = 0; round != 200; ++round)
        CHECK(co2::spawn(pool, blocksOnItsChild(pool, round)).join() == round * 2);
}

} // namespace

int main() {
    stealingMovesHalfOfTheVictimsQueueInOrder();
    everyWorkerIsRecruitedForInjectedWork();
    everyWorkerIsRecruitedForWorkArrivingInTwoBursts();
    everyWorkerIsRecruitedForLocallyQueuedWork();
    aWorkerBlockedOnItsOwnQueuedWorkIsRescuedByAnother();
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
