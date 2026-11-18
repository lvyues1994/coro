// co2 v2 执行层：Scheduler / scheduleOn / ManualExecutor / syncWait。

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "co2/coroutine.hpp"
#include "co2/manual_executor.hpp"
#include "co2/optional.hpp"
#include "co2/result.hpp"
#include "co2/scheduler.hpp"
#include "co2/sync_wait.hpp"
#include "co2/task.hpp"

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

// ---------------------------------------------------------------------------
// ManualExecutor + scheduleOn

auto hops(co2::ManualExecutor& executor, std::vector<std::string>& log,
          std::string name) CO2_BEG(co2::Task<int>, (executor, log, name)) {
    log.push_back(name + " start");
    CO2_AWAIT(co2::scheduleOn(executor));
    log.push_back(name + " after hop");
    CO2_AWAIT(co2::scheduleOn(executor));
    log.push_back(name + " done");
    CO2_RETURN(static_cast<int>(log.size()));
}
CO2_END

void scheduleOnSuspendsUntilTheExecutorRuns() {
    co2::ManualExecutor executor;
    std::vector<std::string> log;
    auto task = hops(executor, log, "a");
    co2::detail::TaskAccess::start(task, co2::noop_coroutine());
    CHECK((log == std::vector<std::string>{"a start"}));
    CHECK(executor.pending() == 1U);

    CHECK(executor.runOne());
    CHECK((log == std::vector<std::string>{"a start", "a after hop"}));
    CHECK(executor.pending() == 1U);
    CHECK(not co2::detail::TaskAccess::isDone(task));

    CHECK(executor.runOne());
    CHECK(co2::detail::TaskAccess::isDone(task));
    CHECK(executor.pending() == 0U);
    CHECK(not executor.runOne());
    CHECK(co2::detail::TaskAccess::takeResult(task) == 3);
}

void manualExecutorIsFifoAndRunDrainsRescheduledWork() {
    co2::ManualExecutor executor;
    std::vector<std::string> log;
    auto first = hops(executor, log, "a");
    auto second = hops(executor, log, "b");
    co2::detail::TaskAccess::start(first, co2::noop_coroutine());
    co2::detail::TaskAccess::start(second, co2::noop_coroutine());
    CHECK(executor.pending() == 2U);
    // 每个任务 hop 两次：run() 一直跑到排空，包括运行期间新排入的。
    CHECK(executor.run() == 4U);
    CHECK((log == std::vector<std::string>{"a start", "b start", "a after hop",
                                           "b after hop", "a done", "b done"}));
}

// ---------------------------------------------------------------------------
// syncWait

auto answer(int value) CO2_BEG(co2::Task<int>, (value)) { CO2_RETURN(value); }
CO2_END

auto nothing(int& counter) CO2_BEG(co2::Task<>, (counter)) {
    ++counter;
    CO2_RETURN();
}
CO2_END

#if CO2_HAS_EXCEPTIONS
auto failing() CO2_BEG(co2::Task<int>, ()) {
    throw ExpectedError{};
    CO2_RETURN(0);
}
CO2_END
#endif

void syncWaitRunsASynchronousTaskInline() {
    CHECK(co2::syncWait(answer(7)) == 7);
    int counter = 0;
    co2::syncWait(nothing(counter));
    CHECK(counter == 1);
}

// 根的返回值照 std::execution::sync_wait：结果放在 optional 里，void 用 Unit 占位。
void syncWaitReturnsAnOptional() {
    static_assert(
        std::is_same<decltype(co2::syncWait(answer(7))), co2::optional<int>>::value,
        "syncWait(Task<int>) must return co2::optional<int>");
    static_assert(std::is_same<decltype(co2::syncWait(std::declval<co2::Task<>>())),
                               co2::optional<co2::Unit>>::value,
                  "syncWait(Task<void>) must return co2::optional<co2::Unit>");
    static_assert(std::is_same<decltype(co2::syncWait(co2::suspend_never{})),
                               co2::optional<co2::Unit>>::value,
                  "syncWait(awaitable) must return an optional as well");

    co2::optional<int> const value = co2::syncWait(answer(7));
    CHECK(value.has_value());
    CHECK(*value == 7);

    int counter = 0;
    co2::optional<co2::Unit> const done = co2::syncWait(nothing(counter));
    CHECK(done.has_value());
    CHECK(counter == 1);
}

#if CO2_HAS_EXCEPTIONS
void syncWaitRethrowsTheTaskException() {
    bool thrown = false;
    try {
        co2::syncWait(failing());
    } catch (ExpectedError const&) {
        thrown = true;
    }
    CHECK(thrown);
}
#endif

auto uniqueAnswer() CO2_BEG(co2::Task<std::unique_ptr<int>>, ()) {
    CO2_RETURN(std::unique_ptr<int>{new int{3}});
}
CO2_END

void syncWaitMovesOutMoveOnlyResults() {
    auto result = *co2::syncWait(uniqueAnswer());
    CHECK(result != nullptr && *result == 3);
}

auto completesElsewhere(co2::ManualExecutor& executor, std::thread::id& resumedOn)
    CO2_BEG(co2::Task<int>, (executor, resumedOn)) {
    CO2_AWAIT(co2::scheduleOn(executor));
    resumedOn = std::this_thread::get_id();
    CO2_RETURN(11);
}
CO2_END

void syncWaitWakesWhenTheTaskCompletesOnAnotherThread() {
    co2::ManualExecutor executor;
    auto resumedOn = std::thread::id{};
    std::thread driver{[&] {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
        while (executor.run() == 0U)
            std::this_thread::yield();
    }};
    auto const driverId = driver.get_id();
    CHECK(co2::syncWait(completesElsewhere(executor, resumedOn)) == 11);
    driver.join();
    CHECK(resumedOn == driverId);
}

#if CO2_HAS_EXCEPTIONS
auto failsElsewhere(co2::ManualExecutor& executor) CO2_BEG(co2::Task<int>, (executor)) {
    CO2_AWAIT(co2::scheduleOn(executor));
    throw ExpectedError{};
    CO2_RETURN(0);
}
CO2_END

void syncWaitRethrowsExceptionsFromAnotherThread() {
    co2::ManualExecutor executor;
    std::thread driver{[&] {
        while (executor.run() == 0U)
            std::this_thread::yield();
    }};
    bool thrown = false;
    try {
        co2::syncWait(failsElsewhere(executor));
    } catch (ExpectedError const&) {
        thrown = true;
    }
    driver.join();
    CHECK(thrown);
}
#endif

} // namespace

int main() {
    scheduleOnSuspendsUntilTheExecutorRuns();
    manualExecutorIsFifoAndRunDrainsRescheduledWork();
    syncWaitRunsASynchronousTaskInline();
    syncWaitReturnsAnOptional();
#if CO2_HAS_EXCEPTIONS
    syncWaitRethrowsTheTaskException();
#endif
    syncWaitMovesOutMoveOnlyResults();
    syncWaitWakesWhenTheTaskCompletesOnAnotherThread();
#if CO2_HAS_EXCEPTIONS
    syncWaitRethrowsExceptionsFromAnotherThread();
#endif
    return 0;
}
