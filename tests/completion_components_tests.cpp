// 三种完成方式在各个组件上的表现（docs/design/completion-channels.md 第 4 节）。
// 机制本身——恢复点、promise 的两个入口、Task——在 completion_tests.cpp 里测。

#include <cstdlib>
#include <iostream>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

#include "co2/as_result.hpp"
#include "co2/coroutine.hpp"
#include "co2/env.hpp"
#include "co2/just.hpp"
#include "co2/manual_executor.hpp"
#include "co2/optional.hpp"
#include "co2/result.hpp"
#include "co2/scheduler.hpp"
#include "co2/spawn.hpp"
#include "co2/stop_token.hpp"
#include "co2/sync_wait.hpp"
#include "co2/task.hpp"
#include "co2/when_all.hpp"
#include "completion_support.hpp"
#include "sample_errc.hpp"

namespace {

void fail(char const* expression, int line) {
    std::cerr << "check failed at line " << line << ": " << expression << '\n';
    std::exit(EXIT_FAILURE);
}

#define CHECK(expression)                                                              \
    do {                                                                               \
        if (not(expression)) fail(#expression, __LINE__);                              \
    } while (false)

using sample::chain;
using sample::Leaf;

// ---------------------------------------------------------------------------
// 等待 JoinHandle：被 spawn 的 Task 出错或已停止，等待它的协程以同样的结果结束

auto awaitsAHandle(co2::JoinHandle<int>& handle, bool& reached)
    CO2_BEG(co2::Task<int>, (handle, reached), int value{};) {
    CO2_AWAIT_SET(value, handle); // 左值：借用
    reached = true;
    CO2_RETURN(value);
}
CO2_END

void awaitingAFinishedHandleSeesHowTheTaskEnded() {
    co2::ManualExecutor executor;
    auto resumed = 0;
    auto stops = co2::spawn(executor, chain(0, Leaf::stops, resumed, nullptr));
    auto fails = co2::spawn(executor, chain(0, Leaf::fails, resumed, nullptr));
    auto returns = co2::spawn(executor, chain(0, Leaf::returns, resumed, nullptr));
    CHECK(executor.run() == 3U);

    auto reached = false;
    CHECK(not co2::syncWait(awaitsAHandle(stops, reached)).has_value());
    CHECK(not reached);

    // 错误是按值传过来的：它仍然是那个错误码，而不是在 await_resume() 里被抛出、又被
    // 等待者接住的异常。
    auto const failure = co2::syncWait(co2::asResult(awaitsAHandle(fails, reached)));
    CHECK(failure->hasError());
    CHECK(failure->error().exception() == nullptr);
    CHECK(failure->error().code() == sample::Errc::gone);
    CHECK(not reached);

    CHECK(co2::syncWait(awaitsAHandle(returns, reached)) == 1);
    CHECK(reached);
}

auto spawnsAndAwaits(co2::Scheduler& scheduler, Leaf leaf, int& resumed, bool& reached)
    CO2_BEG(co2::Task<int>, (scheduler, leaf, resumed, reached), int value{};) {
    // 右值：JoinHandle 自己是 awaiter，被搬进本协程的帧。
    CO2_AWAIT_SET(value, co2::spawn(scheduler, chain(0, leaf, resumed, nullptr)));
    reached = true;
    CO2_RETURN(value);
}
CO2_END

void aSuspendedAwaiterOfAHandleIsResumedWithTheOutcome() {
    co2::ManualExecutor awaiting;
    co2::ManualExecutor spawned;
    auto resumed = 0;
    auto reached = false;
    auto outer =
        co2::spawn(awaiting, spawnsAndAwaits(spawned, Leaf::stops, resumed, reached));
    CHECK(awaiting.run() == 1U); // 外层启动，spawn 了内层，挂在它的 JoinHandle 上
    CHECK(not outer.isReady());
    CHECK(spawned.run() == 1U); // 内层以已停止结束，就地恢复外层
    CHECK(outer.isReady());
    CHECK(not outer.join().has_value());
    CHECK(not reached);
}

// ---------------------------------------------------------------------------
// whenAll：出错优先于已停止，取时间上第一个错误（std::execution::when_all 的规则）

auto failsWith(sample::Errc code) CO2_BEG(co2::Task<int>, (code)) {
    CO2_AWAIT(co2::just_error(code));
    CO2_RETURN(0);
}
CO2_END

auto awaitsBoth(co2::Task<int> first, co2::Task<int> second, bool& reached)
    CO2_BEG(co2::Task<int>, (first, second, reached), std::tuple<int, int> values;) {
    CO2_AWAIT_SET(values, co2::whenAll(std::move(first), std::move(second)));
    reached = true;
    CO2_RETURN(std::get<0>(values) + std::get<1>(values));
}
CO2_END

// 等待 whenAll 的那个协程是怎么结束的，按值取出来。
co2::Result<int> outcomeOfBoth(co2::Task<int> first, co2::Task<int> second) {
    auto reached = false;
    auto outcome = *co2::syncWait(
        co2::asResult(awaitsBoth(std::move(first), std::move(second), reached)));
    CHECK(reached == outcome.hasValue()); // 没有值时，等待之后的语句不执行
    return outcome;
}

co2::Task<int> leaf(Leaf const how) {
    static auto resumed = 0;
    return chain(0, how, resumed, nullptr);
}

void whenAllDeliversValuesWhenEveryChildHasOne() {
    auto const both = outcomeOfBoth(leaf(Leaf::returns), leaf(Leaf::returns));
    CHECK(both.hasValue() && both.value() == 2);
}

void whenAllCompletesWithTheFirstErrorInTime() {
    // child 按参数顺序内联启动，所以排在前面的先出错。
    auto const first =
        outcomeOfBoth(failsWith(sample::Errc::retry_later), failsWith(sample::Errc::gone));
    CHECK(first.hasError() && first.error().code() == sample::Errc::retry_later);

    auto const second =
        outcomeOfBoth(failsWith(sample::Errc::gone), failsWith(sample::Errc::retry_later));
    CHECK(second.hasError() && second.error().code() == sample::Errc::gone);

    // 错误是按值传过来的：仍然是错误码，而不是被抛出又接住的异常。
    CHECK(first.error().exception() == nullptr);
}

void whenAllCompletesStoppedWhenAChildStopsAndNoneFails() {
    CHECK(outcomeOfBoth(leaf(Leaf::returns), leaf(Leaf::stops)).isStopped());
    CHECK(outcomeOfBoth(leaf(Leaf::stops), leaf(Leaf::returns)).isStopped());
    CHECK(outcomeOfBoth(leaf(Leaf::stops), leaf(Leaf::stops)).isStopped());
}

void whenAllPrefersAnErrorOverStopped() {
    auto const stoppedFirst =
        outcomeOfBoth(leaf(Leaf::stops), failsWith(sample::Errc::retry_later));
    CHECK(stoppedFirst.hasError());
    CHECK(stoppedFirst.error().code() == sample::Errc::retry_later);

    auto const failedFirst =
        outcomeOfBoth(failsWith(sample::Errc::retry_later), leaf(Leaf::stops));
    CHECK(failedFirst.hasError());
    CHECK(failedFirst.error().code() == sample::Errc::retry_later);
}

// 把等待者停放起来，句柄交给测试，由测试决定什么时候恢复。
struct Parks {
    co2::coroutine_handle<>* parked;

    bool await_ready() const noexcept { return false; }
    void await_suspend(co2::coroutine_handle<> const handle) const noexcept {
        *parked = handle;
    }
    void await_resume() const noexcept {}
};

auto parksThenReportsStop(co2::coroutine_handle<>& parked, bool& sawStop)
    CO2_BEG(co2::Task<int>, (parked, sawStop), co2::stop_token token;) {
    CO2_AWAIT_SET(token, co2::getStopToken());
    CO2_AWAIT(Parks{&parked});
    sawStop = token.stop_requested();
    CO2_RETURN(1);
}
CO2_END

// 一个 child 已停止，与出错一样向其余 child 请求停止，然后等它们全部结束。
void whenAllRequestsStopFromTheOthersWhenAChildStops() {
    co2::ManualExecutor executor;
    auto parked = co2::coroutine_handle<>{};
    auto sawStop = false;
    auto reached = false;
    auto outer = co2::spawn(
        executor, awaitsBoth(parksThenReportsStop(parked, sawStop), leaf(Leaf::stops),
                             reached));
    CHECK(executor.run() == 1U);
    CHECK(parked);              // 第一个 child 停在那里
    CHECK(not outer.isReady()); // whenAll 还在等它

    parked.resume();
    CHECK(sawStop); // 它恢复时看到了停止请求
    CHECK(outer.isReady());
    CHECK(not outer.join().has_value());
    CHECK(not reached);
}

auto awaitsAll(std::vector<co2::Task<int>> tasks, bool& reached)
    CO2_BEG(co2::Task<int>, (tasks, reached), std::vector<int> values;) {
    CO2_AWAIT_SET(values, co2::whenAll(std::move(tasks)));
    reached = true;
    CO2_RETURN(static_cast<int>(values.size()));
}
CO2_END

co2::Result<int> outcomeOfAll(Leaf const first, Leaf const second, Leaf const third) {
    auto tasks = std::vector<co2::Task<int>>{};
    tasks.push_back(leaf(first));
    tasks.push_back(leaf(second));
    tasks.push_back(leaf(third));
    auto reached = false;
    return *co2::syncWait(co2::asResult(awaitsAll(std::move(tasks), reached)));
}

void whenAllOfAVectorFollowsTheSameRules() {
    auto const values = outcomeOfAll(Leaf::returns, Leaf::returns, Leaf::returns);
    CHECK(values.hasValue() && values.value() == 3);

    CHECK(outcomeOfAll(Leaf::returns, Leaf::stops, Leaf::returns).isStopped());

    auto const failure = outcomeOfAll(Leaf::stops, Leaf::fails, Leaf::returns);
    CHECK(failure.hasError());
    CHECK(failure.error().exception() == nullptr);
    CHECK(failure.error().code() == sample::Errc::gone);
}

} // namespace

int main() {
    awaitingAFinishedHandleSeesHowTheTaskEnded();
    aSuspendedAwaiterOfAHandleIsResumedWithTheOutcome();
    whenAllDeliversValuesWhenEveryChildHasOne();
    whenAllCompletesWithTheFirstErrorInTime();
    whenAllCompletesStoppedWhenAChildStopsAndNoneFails();
    whenAllPrefersAnErrorOverStopped();
    whenAllRequestsStopFromTheOthersWhenAChildStops();
    whenAllOfAVectorFollowsTheSameRules();
    return 0;
}
