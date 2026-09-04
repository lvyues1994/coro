// whenAll：值通道拼接、并发启动、异常与停止传播、父 token 转发、vector 版本。

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "co2/coroutine.hpp"
#include "co2/detail/result_storage.hpp"
#include "co2/env.hpp"
#include "co2/manual_executor.hpp"
#include "co2/scheduler.hpp"
#include "co2/spawn.hpp"
#include "co2/stop_token.hpp"
#include "co2/sync_wait.hpp"
#include "co2/task.hpp"
#include "co2/thread_pool.hpp"
#include "co2/when_all.hpp"

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
    explicit ExpectedError(int which_)
        : std::runtime_error{"expected"}, which{which_} {}
    int which;
};

struct Guard {
    explicit Guard(std::atomic<int>& destroyed_) noexcept : destroyed{&destroyed_} {}
    Guard(Guard const&) = delete;
    Guard& operator=(Guard const&) = delete;
    ~Guard() { destroyed->fetch_add(1); }
    std::atomic<int>* destroyed;
};

// 手动事件：把等待者停放起来，由测试代码决定何时恢复。
struct ManualEvent {
    struct Awaiter {
        ManualEvent* event;
        bool await_ready() const noexcept { return event->ready; }
        void await_suspend(co2::coroutine_handle<> const waiter) noexcept {
            event->waiter = waiter;
        }
        void await_resume() const noexcept {}
    };

    Awaiter wait() noexcept { return Awaiter{this}; }

    void set() {
        ready = true;
        auto const pending = waiter;
        waiter = nullptr;
        if (pending) pending.resume();
    }

    co2::coroutine_handle<> waiter;
    bool ready{};
};

// ---------------------------------------------------------------------------
// 结果类型与同步完成

auto value(int v) CO2_BEG(co2::Task<int>, (v)) { CO2_RETURN(v); }
CO2_END

auto text(std::string s) CO2_BEG(co2::Task<std::string>, (s)) {
    CO2_RETURN(std::move(s));
}
CO2_END

auto nothing(std::atomic<int>& counter) CO2_BEG(co2::Task<>, (counter)) {
    counter.fetch_add(1);
    CO2_RETURN();
}
CO2_END

auto unique(int v) CO2_BEG(co2::Task<std::unique_ptr<int>>, (v)) {
    CO2_RETURN(std::unique_ptr<int>{new int{v}});
}
CO2_END

void resultTypesConcatenateValueChannels() {
    static_assert(
        std::is_same<decltype(co2::whenAll(value(1), text("")).await_resume()),
                     std::tuple<int, std::string>>::value,
        "non-void results form a tuple in argument order");
    static_assert(
        std::is_same<decltype(co2::whenAll(value(1), std::declval<co2::Task<>>(),
                                           text(""))
                                  .await_resume()),
                     std::tuple<int, std::string>>::value,
        "void children contribute nothing");
    static_assert(std::is_same<decltype(co2::whenAll(std::declval<co2::Task<>>(),
                                                     std::declval<co2::Task<>>())
                                            .await_resume()),
                               void>::value,
                  "all-void is void");
    static_assert(std::is_same<decltype(co2::whenAll().await_resume()), void>::value,
                  "empty is void");
    static_assert(
        std::is_same<decltype(co2::whenAll(std::declval<std::vector<co2::Task<int>>>())
                                  .await_resume()),
                     std::vector<int>>::value,
        "vector<Task<T>> gives vector<T>");
    static_assert(
        std::is_same<decltype(co2::whenAll(std::declval<std::vector<co2::Task<>>>())
                                  .await_resume()),
                     void>::value,
        "vector<Task<void>> gives void");
}

void readyChildrenCompleteWithoutSuspending() {
    std::atomic<int> counter{0};
    auto const results =
        co2::syncWait(co2::whenAll(value(1), nothing(counter), text("x")));
    CHECK(std::get<0>(results) == 1);
    CHECK(std::get<1>(results) == "x");
    CHECK(counter.load() == 1);

    co2::syncWait(co2::whenAll(nothing(counter), nothing(counter)));
    CHECK(counter.load() == 3);
    co2::syncWait(co2::whenAll());
}

void moveOnlyResultsFlowThroughTupleAndVector() {
    auto tuple = co2::syncWait(co2::whenAll(unique(1), unique(2)));
    CHECK(*std::get<0>(tuple) == 1 && *std::get<1>(tuple) == 2);

    std::vector<co2::Task<std::unique_ptr<int>>> tasks;
    for (int i = 0; i < 5; ++i)
        tasks.push_back(unique(i));
    auto values = co2::syncWait(co2::whenAll(std::move(tasks)));
    CHECK(values.size() == 5U);
    for (int i = 0; i < 5; ++i)
        CHECK(*values[static_cast<std::size_t>(i)] == i);
}

void emptyVectorCompletesImmediately() {
    auto values = co2::syncWait(co2::whenAll(std::vector<co2::Task<int>>{}));
    CHECK(values.empty());
    co2::syncWait(co2::whenAll(std::vector<co2::Task<>>{}));
}

// ---------------------------------------------------------------------------
// 并发与完成顺序

auto gated(ManualEvent& gate, int v, std::vector<int>& order)
    CO2_BEG(co2::Task<int>, (gate, v, order)) {
    order.push_back(v);
    CO2_AWAIT(gate.wait());
    CO2_RETURN(v * 10);
}
CO2_END

auto awaitsGated(ManualEvent& first, ManualEvent& second, std::vector<int>& order)
    CO2_BEG(co2::Task<int>, (first, second, order), std::tuple<int, int> results;) {
    CO2_AWAIT_SET(results,
                  co2::whenAll(gated(first, 1, order), gated(second, 2, order)));
    order.push_back(std::get<0>(results) + std::get<1>(results));
    CO2_RETURN(std::get<0>(results));
}
CO2_END

void childrenStartInOrderAndCompleteInAnyOrder() {
    ManualEvent first;
    ManualEvent second;
    std::vector<int> order;
    auto task = awaitsGated(first, second, order);
    co2::detail::TaskAccess::start(task, co2::noop_coroutine());
    CHECK((order == std::vector<int>{1, 2})); // 两个 child 都已启动并挂起
    CHECK(not co2::detail::TaskAccess::isDone(task));
    second.set(); // 完成顺序与启动顺序无关
    CHECK(not co2::detail::TaskAccess::isDone(task));
    first.set(); // 最后一个完成者恢复等待者
    CHECK(co2::detail::TaskAccess::isDone(task));
    CHECK((order == std::vector<int>{1, 2, 30}));
    CHECK(co2::detail::TaskAccess::takeResult(task) == 10);
}

// 记录线程与同时在飞的 child 数：并发用结构性的"最大同时在飞数"断言，而不是计时——
// 计时在 valgrind 这类慢环境下不可靠。
struct Concurrency {
    void enter() {
        std::lock_guard<std::mutex> lock{mutex};
        ids.insert(std::this_thread::get_id());
        ++inFlight;
        if (inFlight > maxInFlight) maxInFlight = inFlight;
    }
    void leave() {
        std::lock_guard<std::mutex> lock{mutex};
        --inFlight;
    }
    std::mutex mutex;
    std::set<std::thread::id> ids;
    int inFlight{};
    int maxInFlight{};
};

auto slowOnPool(co2::ThreadPool& pool, Concurrency& concurrency, int v)
    CO2_BEG(co2::Task<int>, (pool, concurrency, v)) {
    CO2_AWAIT(co2::scheduleOn(pool));
    concurrency.enter();
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
    concurrency.leave();
    CO2_RETURN(v);
}
CO2_END

void childrenRunConcurrentlyOnAThreadPool() {
    co2::ThreadPool pool{4U};
    Concurrency concurrency;
    std::vector<co2::Task<int>> tasks;
    for (int i = 0; i < 8; ++i)
        tasks.push_back(slowOnPool(pool, concurrency, i));
    auto const values = co2::syncWait(co2::whenAll(std::move(tasks)));
    CHECK(values.size() == 8U);
    for (int i = 0; i < 8; ++i)
        CHECK(values[static_cast<std::size_t>(i)] == i);
    CHECK(concurrency.ids.size() >= 2U);
    CHECK(concurrency.maxInFlight >= 2);
}

// ---------------------------------------------------------------------------
// 异常与停止传播

auto failsAfterGate(ManualEvent& gate, int which)
    CO2_BEG(co2::Task<int>, (gate, which)) {
    CO2_AWAIT(gate.wait());
    throw ExpectedError{which};
    CO2_RETURN(0);
}
CO2_END

auto untilStopped(ManualEvent& gate, std::atomic<int>& destroyed, bool& sawStop)
    CO2_BEG(co2::Task<int>, (gate, destroyed, sawStop),
            co2::detail::ResultStorage<Guard> guard;
            co2::stop_token token;) {
    guard.emplace(destroyed);
    CO2_AWAIT_SET(token, co2::getStopToken());
    CO2_AWAIT(gate.wait());
    sawStop = token.stop_requested();
    CO2_RETURN(7);
}
CO2_END

void firstFailureInTimeIsRethrownAfterEveryChildFinishes() {
    ManualEvent gateA;
    ManualEvent gateB;
    ManualEvent gateC;
    std::atomic<int> destroyed{0};
    bool sawStop = false;
    auto whenAllTask = co2::detail::wrapAwaitable<std::tuple<int, int, int>>(
        co2::whenAll(failsAfterGate(gateA, 1), failsAfterGate(gateB, 2),
                     untilStopped(gateC, destroyed, sawStop)),
        std::false_type{});
    co2::detail::TaskAccess::start(whenAllTask, co2::noop_coroutine());
    CHECK(not co2::detail::TaskAccess::isDone(whenAllTask));

    gateB.set(); // 时间上第一个失败者是 B（参数顺序第二）
    CHECK(not co2::detail::TaskAccess::isDone(whenAllTask));
    gateA.set();
    CHECK(not co2::detail::TaskAccess::isDone(whenAllTask));
    CHECK(destroyed.load() == 0); // 第三个 child 仍在运行：等待它
    gateC.set();
    CHECK(co2::detail::TaskAccess::isDone(whenAllTask));
    CHECK(sawStop); // 兄弟失败后它看到了停止请求
    bool thrown = false;
    try {
        co2::detail::TaskAccess::takeResult(whenAllTask);
    } catch (ExpectedError const& error) {
        thrown = true;
        CHECK(error.which == 2);
    }
    CHECK(thrown);
    whenAllTask = co2::Task<std::tuple<int, int, int>>{};
    CHECK(destroyed.load() == 1);
}

auto observesStop(ManualEvent& gate, bool& sawStop)
    CO2_BEG(co2::Task<>, (gate, sawStop), co2::stop_token token;) {
    CO2_AWAIT_SET(token, co2::getStopToken());
    CO2_AWAIT(gate.wait());
    sawStop = token.stop_requested();
}
CO2_END

void parentStopRequestReachesEveryChild() {
    ManualEvent gateA;
    ManualEvent gateB;
    bool sawA = false;
    bool sawB = false;
    co2::stop_source parent;
    auto whenAllTask = co2::detail::wrapAwaitable<void>(
        co2::whenAll(observesStop(gateA, sawA), observesStop(gateB, sawB)),
        std::true_type{});
    co2::detail::TaskAccess::start(whenAllTask, co2::noop_coroutine(),
                                   parent.get_token());
    CHECK(parent.request_stop());
    gateA.set();
    gateB.set();
    CHECK(co2::detail::TaskAccess::isDone(whenAllTask));
    CHECK(sawA && sawB);
    co2::detail::TaskAccess::takeResult(whenAllTask);
}

void alreadyStoppedParentStartsChildrenStopped() {
    co2::stop_source parent;
    parent.request_stop();
    ManualEvent gate;
    gate.ready = true;
    bool saw = false;
    co2::syncWait(co2::whenAll(observesStop(gate, saw)), parent.get_token());
    CHECK(saw);
}

auto failsImmediately(int which) CO2_BEG(co2::Task<int>, (which)) {
    throw ExpectedError{which};
    CO2_RETURN(0);
}
CO2_END

void synchronousFailureStillWaitsForSiblingsAndRethrows() {
    std::atomic<int> destroyed{0};
    bool sawStop = false;
    ManualEvent gate;
    gate.ready = true;
    bool thrown = false;
    try {
        co2::syncWait(
            co2::whenAll(failsImmediately(3), untilStopped(gate, destroyed, sawStop)));
    } catch (ExpectedError const& error) {
        thrown = true;
        CHECK(error.which == 3);
    }
    CHECK(thrown);
    CHECK(sawStop);
    CHECK(destroyed.load() == 1);
}

void vectorFailureRethrowsTheFirstInTime() {
    ManualEvent gates[3];
    std::vector<co2::Task<int>> tasks;
    for (int i = 0; i < 3; ++i)
        tasks.push_back(failsAfterGate(gates[i], i));
    auto whenAllTask = co2::detail::wrapAwaitable<std::vector<int>>(
        co2::whenAll(std::move(tasks)), std::false_type{});
    co2::detail::TaskAccess::start(whenAllTask, co2::noop_coroutine());
    gates[2].set();
    gates[0].set();
    gates[1].set();
    bool thrown = false;
    try {
        co2::detail::TaskAccess::takeResult(whenAllTask);
    } catch (ExpectedError const& error) {
        thrown = true;
        CHECK(error.which == 2);
    }
    CHECK(thrown);
}

// ---------------------------------------------------------------------------
// 嵌套与规模

auto sumOf(std::vector<co2::Task<int>> tasks)
    CO2_BEG(co2::Task<int>, (tasks), std::vector<int> values; int total{};) {
    CO2_AWAIT_SET(values, co2::whenAll(std::move(tasks)));
    for (auto const v : values)
        total += v;
    CO2_RETURN(total);
}
CO2_END

auto hopAndValue(co2::ThreadPool& pool, int v) CO2_BEG(co2::Task<int>, (pool, v)) {
    CO2_AWAIT(co2::scheduleOn(pool));
    CO2_RETURN(v);
}
CO2_END

void nestedWhenAllOverManyChildrenOnAPool() {
    co2::ThreadPool pool{4U};
    std::vector<co2::Task<int>> groups;
    long long expected = 0;
    for (int g = 0; g < 8; ++g) {
        std::vector<co2::Task<int>> tasks;
        for (int i = 0; i < 250; ++i) {
            tasks.push_back(hopAndValue(pool, g * 1000 + i));
            expected += g * 1000 + i;
        }
        groups.push_back(sumOf(std::move(tasks)));
    }
    auto const sums = co2::syncWait(co2::whenAll(std::move(groups)));
    long long total = 0;
    for (auto const s : sums)
        total += s;
    CHECK(total == expected);
}

auto whenAllInsideSpawn(co2::ThreadPool& pool)
    CO2_BEG(co2::Task<int>, (pool), std::tuple<int, int> results;) {
    CO2_AWAIT_SET(results, co2::whenAll(hopAndValue(pool, 1), hopAndValue(pool, 2)));
    CO2_RETURN(std::get<0>(results) + std::get<1>(results));
}
CO2_END

void whenAllWorksInsideASpawnedTask() {
    co2::ThreadPool pool{2U};
    CHECK(co2::spawn(pool, whenAllInsideSpawn(pool)).join() == 3);
}

void repeatedWhenAllDoesNotLeakOrCorrupt() {
    co2::ThreadPool pool{2U};
    for (int round = 0; round < 200; ++round) {
        auto const results = co2::syncWait(co2::whenAll(
            hopAndValue(pool, round), hopAndValue(pool, -round), value(1)));
        CHECK(std::get<0>(results) + std::get<1>(results) == 0);
        CHECK(std::get<2>(results) == 1);
    }
}

} // namespace

int main() {
    resultTypesConcatenateValueChannels();
    readyChildrenCompleteWithoutSuspending();
    moveOnlyResultsFlowThroughTupleAndVector();
    emptyVectorCompletesImmediately();
    childrenStartInOrderAndCompleteInAnyOrder();
    childrenRunConcurrentlyOnAThreadPool();
    firstFailureInTimeIsRethrownAfterEveryChildFinishes();
    parentStopRequestReachesEveryChild();
    alreadyStoppedParentStartsChildrenStopped();
    synchronousFailureStillWaitsForSiblingsAndRethrows();
    vectorFailureRethrowsTheFirstInTime();
    nestedWhenAllOverManyChildrenOnAPool();
    whenAllWorksInsideASpawnedTask();
    repeatedWhenAllDoesNotLeakOrCorrupt();
    return 0;
}
