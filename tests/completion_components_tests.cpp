// 三种完成方式在各个组件上的表现（docs/design/completion-channels.md 第 4 节）。
// 机制本身——恢复点、promise 的两个入口、Task——在 completion_tests.cpp 里测。

#include <cstdlib>
#include <iostream>
#include <system_error>
#include <utility>

#include "co2/as_result.hpp"
#include "co2/coroutine.hpp"
#include "co2/manual_executor.hpp"
#include "co2/optional.hpp"
#include "co2/result.hpp"
#include "co2/scheduler.hpp"
#include "co2/spawn.hpp"
#include "co2/sync_wait.hpp"
#include "co2/task.hpp"
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

} // namespace

int main() {
    awaitingAFinishedHandleSeesHowTheTaskEnded();
    aSuspendedAwaiterOfAHandleIsResumedWithTheOutcome();
    return 0;
}
