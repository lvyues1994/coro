// co2 v2：spawn / JoinHandle，以及 stop_token 沿 Task 树的传播。

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

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

struct Guard {
    explicit Guard(std::atomic<int>& destroyed_) noexcept : destroyed{&destroyed_} {}
    Guard(Guard const&) = delete;
    Guard& operator=(Guard const&) = delete;
    ~Guard() { destroyed->fetch_add(1); }
    std::atomic<int>* destroyed;
};

// ---------------------------------------------------------------------------
// 基本 spawn / join

auto answer(int value) CO2_BEG(co2::Task<int>, (value)) { CO2_RETURN(value); }
CO2_END

auto touch(std::atomic<int>& counter) CO2_BEG(co2::Task<>, (counter)) {
    counter.fetch_add(1);
    CO2_RETURN();
}
CO2_END

void spawnOnAManualExecutorStartsWhenItRuns() {
    co2::ManualExecutor executor;
    auto handle = co2::spawn(executor, answer(5));
    CHECK(handle);
    CHECK(not handle.isReady());
    CHECK(executor.pending() == 1U);
    CHECK(executor.run() == 1U);
    CHECK(handle.isReady());
    CHECK(handle.join() == 5);
}

void spawnOnAThreadPoolJoinsFromTheCallingThread() {
    co2::ThreadPool pool{2U};
    std::atomic<int> counter{0};
    auto first = co2::spawn(pool, answer(7));
    auto second = co2::spawn(pool, touch(counter));
    CHECK(first.join() == 7);
    second.join();
    CHECK(counter.load() == 1);
}

auto failing() CO2_BEG(co2::Task<int>, ()) {
    throw ExpectedError{};
    CO2_RETURN(0);
}
CO2_END

void joinRethrowsTheTaskException() {
    co2::ThreadPool pool{1U};
    auto handle = co2::spawn(pool, failing());
    bool thrown = false;
    try {
        handle.join();
    } catch (ExpectedError const&) {
        thrown = true;
    }
    CHECK(thrown);
}

auto uniqueAnswer() CO2_BEG(co2::Task<std::unique_ptr<int>>, ()) {
    CO2_RETURN(std::unique_ptr<int>{new int{9}});
}
CO2_END

void joinMovesOutMoveOnlyResults() {
    co2::ThreadPool pool{1U};
    auto result = co2::spawn(pool, uniqueAnswer()).join();
    CHECK(result != nullptr && *result == 9);
}

// ---------------------------------------------------------------------------
// 在协程里等待 JoinHandle

auto awaitsHandles(co2::ThreadPool& pool)
    CO2_BEG(co2::Task<int>, (pool), co2::JoinHandle<int> borrowed; int first{};
            int second{};) {
    borrowed = co2::spawn(pool, answer(1));
    CO2_AWAIT_SET(first, borrowed);                     // 左值：借用
    CO2_AWAIT_SET(second, co2::spawn(pool, answer(2))); // 右值：拥有
    CHECK(borrowed.isReady());
    CO2_RETURN(first + second);
}
CO2_END

void joinHandlesAreAwaitable() {
    co2::ThreadPool pool{2U};
    CHECK(co2::syncWait(awaitsHandles(pool)) == 3);
}

auto awaitsReadyHandle(co2::JoinHandle<int>& handle)
    CO2_BEG(co2::Task<int>, (handle), int value{};) {
    CO2_AWAIT_SET(value, handle);
    CO2_RETURN(value);
}
CO2_END

void awaitingAnAlreadyCompletedHandleDoesNotSuspend() {
    co2::ManualExecutor executor;
    auto handle = co2::spawn(executor, answer(4));
    CHECK(executor.run() == 1U);
    CHECK(co2::syncWait(awaitsReadyHandle(handle)) == 4);
}

// ---------------------------------------------------------------------------
// stop_token：请求、观察、传播

auto spinUntilStopped(co2::ThreadPool& pool)
    CO2_BEG(co2::Task<int>, (pool), co2::stop_token token; int hops{};) {
    CO2_AWAIT_SET(token, co2::getStopToken());
    CHECK(token.stop_possible());
    while (not token.stop_requested()) {
        ++hops;
        CO2_AWAIT(co2::scheduleOn(pool));
    }
    CO2_RETURN(hops);
}
CO2_END

void requestStopIsObservedThroughGetStopToken() {
    co2::ThreadPool pool{2U};
    auto handle = co2::spawn(pool, spinUntilStopped(pool));
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
    CHECK(not handle.isReady());
    CHECK(handle.requestStop());
    CHECK(handle.getStopToken().stop_requested());
    CHECK(handle.join() >= 0);
}

auto guardedSpin(co2::ThreadPool& pool, std::atomic<int>& destroyed)
    CO2_BEG(co2::Task<>, (pool, destroyed), co2::detail::ResultStorage<Guard> guard;
            co2::stop_token token;) {
    guard.emplace(destroyed);
    CO2_AWAIT_SET(token, co2::getStopToken());
    while (not token.stop_requested())
        CO2_AWAIT(co2::scheduleOn(pool));
}
CO2_END

void droppingAJoinHandleRequestsStopAndDetaches() {
    std::atomic<int> destroyed{0};
    {
        co2::ThreadPool pool{2U};
        for (int i = 0; i < 8; ++i)
            co2::spawn(pool, guardedSpin(pool, destroyed));
        // 句柄已全部丢弃：每个 Task 收到停止请求、跑完、帧被销毁——线程池析构会排空。
    }
    CHECK(destroyed.load() == 8);
}

auto reportsToken(co2::stop_token& seen) CO2_BEG(co2::Task<>, (seen)) {
    CO2_AWAIT_SET(seen, co2::getStopToken());
}
CO2_END

auto parentOf(co2::stop_token& parentSeen, co2::stop_token& childSeen)
    CO2_BEG(co2::Task<>, (parentSeen, childSeen)) {
    CO2_AWAIT_SET(parentSeen, co2::getStopToken());
    CO2_AWAIT(reportsToken(childSeen));
}
CO2_END

void childTasksInheritTheParentsToken() {
    co2::stop_source source;
    co2::stop_token parentSeen;
    co2::stop_token childSeen;
    co2::syncWait(parentOf(parentSeen, childSeen), source.get_token());
    CHECK(parentSeen == source.get_token());
    CHECK(childSeen == source.get_token());

    co2::stop_token rootless;
    co2::syncWait(reportsToken(rootless));
    CHECK(not rootless.stop_possible());
}

void spawnForwardsTheParentsStopRequest() {
    co2::ThreadPool pool{2U};
    co2::stop_source parent;
    auto handle = co2::spawn(pool, spinUntilStopped(pool), parent.get_token());
    CHECK(handle.getStopToken() != parent.get_token());
    std::this_thread::sleep_for(std::chrono::milliseconds{2});
    CHECK(parent.request_stop());
    CHECK(handle.getStopToken().stop_requested());
    CHECK(handle.join() >= 0);
}

void spawnWithAnAlreadyStoppedParentStartsStopped() {
    co2::ThreadPool pool{1U};
    co2::stop_source parent;
    parent.request_stop();
    auto handle = co2::spawn(pool, spinUntilStopped(pool), parent.get_token());
    CHECK(handle.join() == 0);
}

void joinHandlesAreMovable() {
    co2::ThreadPool pool{1U};
    auto first = co2::spawn(pool, answer(3));
    co2::JoinHandle<int> second;
    CHECK(first && not second);
    second = std::move(first);
    CHECK(not first && second);
    auto third{std::move(second)};
    CHECK(third.join() == 3);
}

} // namespace

int main() {
    spawnOnAManualExecutorStartsWhenItRuns();
    spawnOnAThreadPoolJoinsFromTheCallingThread();
    joinRethrowsTheTaskException();
    joinMovesOutMoveOnlyResults();
    joinHandlesAreAwaitable();
    awaitingAnAlreadyCompletedHandleDoesNotSuspend();
    requestStopIsObservedThroughGetStopToken();
    droppingAJoinHandleRequestsStopAndDetaches();
    childTasksInheritTheParentsToken();
    spawnForwardsTheParentsStopRequest();
    spawnWithAnAlreadyStoppedParentStartsStopped();
    joinHandlesAreMovable();
    return 0;
}
