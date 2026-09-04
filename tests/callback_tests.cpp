// fromCallback / Continuation：回调式 API → awaiter。同步完成不挂起、异步完成在回调线程
// 恢复、两方竞争、异常两条通道、stop_token 接入 API 的 cancel、void 与 move-only 结果。

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "co2/callback.hpp"
#include "co2/coroutine.hpp"
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

struct Cancelled : std::runtime_error {
    Cancelled() : std::runtime_error{"cancelled"} {}
};

// 模拟的回调式 API：把 handler 存起来，由测试代码在任意线程上触发；支持 cancel。
struct FakeApi {
    void asyncGet(std::function<void(int)> handler) {
        std::lock_guard<std::mutex> lock{mutex};
        pending = std::move(handler);
    }

    void asyncGetWithCancel(std::function<void(bool, int)> handler) {
        std::lock_guard<std::mutex> lock{mutex};
        pendingCancellable = std::move(handler);
    }

    void fire(int value) {
        std::function<void(int)> handler;
        {
            std::lock_guard<std::mutex> lock{mutex};
            handler = std::move(pending);
            pending = nullptr;
        }
        if (handler) handler(value);
    }

    void cancel() {
        std::function<void(bool, int)> handler;
        {
            std::lock_guard<std::mutex> lock{mutex};
            handler = std::move(pendingCancellable);
            pendingCancellable = nullptr;
        }
        if (handler) handler(true, 0);
    }

    void fireCancellable(int value) {
        std::function<void(bool, int)> handler;
        {
            std::lock_guard<std::mutex> lock{mutex};
            handler = std::move(pendingCancellable);
            pendingCancellable = nullptr;
        }
        if (handler) handler(false, value);
    }

    bool hasPending() {
        std::lock_guard<std::mutex> lock{mutex};
        return static_cast<bool>(pending) || static_cast<bool>(pendingCancellable);
    }

    std::mutex mutex;
    std::function<void(int)> pending;
    std::function<void(bool, int)> pendingCancellable;
};

// ---------------------------------------------------------------------------
// 同步完成

auto immediate(int v) CO2_BEG(co2::Task<int>, (v), int got{};) {
    CO2_AWAIT_AS_SET(
        got, co2::CallbackAwaitable<int>,
        co2::fromCallback<int>([this](co2::Continuation<int> done) { done(v); }));
    CO2_RETURN(got);
}
CO2_END

void synchronousCompletionDoesNotSuspend() {
    // 同步完成的 Task 在 start 返回时就已经 done：说明 await_suspend 返回了 false。
    auto task = immediate(7);
    co2::detail::TaskAccess::start(task, co2::noop_coroutine());
    CHECK(co2::detail::TaskAccess::isDone(task));
    CHECK(co2::detail::TaskAccess::takeResult(task) == 7);
}

auto immediateVoid(int& counter) CO2_BEG(co2::Task<>, (counter)) {
    CO2_AWAIT_AS(co2::CallbackAwaitable<void>,
                 co2::fromCallback<void>([this](co2::Continuation<void> done) {
                     ++counter;
                     done();
                 }));
    ++counter;
}
CO2_END

void voidContinuationTakesNoArguments() {
    int counter = 0;
    co2::syncWait(immediateVoid(counter));
    CHECK(counter == 2);
}

using PairResult = std::pair<int, std::string>;

auto pairResult() CO2_BEG(co2::Task<PairResult>, (), PairResult got;) {
    // 类型与表达式都含逗号：各自用括号包裹。
    CO2_AWAIT_AS_SET(
        got, (co2::CallbackAwaitable<std::pair<int, std::string>>),
        (co2::fromCallback<PairResult>([](co2::Continuation<PairResult> done) {
            done(3, "three"); // 多参数就地构造 T
        })));
    CO2_RETURN(std::move(got));
}
CO2_END

auto uniqueResult()
    CO2_BEG(co2::Task<std::unique_ptr<int>>, (), std::unique_ptr<int> got;) {
    CO2_AWAIT_AS_SET(got, co2::CallbackAwaitable<std::unique_ptr<int>>,
                     co2::fromCallback<std::unique_ptr<int>>(
                         [](co2::Continuation<std::unique_ptr<int>> done) {
                             done(std::unique_ptr<int>{new int{9}});
                         }));
    CO2_RETURN(std::move(got));
}
CO2_END

void resultsAreConstructedInPlaceAndMovedOut() {
    auto const pair = co2::syncWait(pairResult());
    CHECK(pair.first == 3 && pair.second == "three");
    auto unique = co2::syncWait(uniqueResult());
    CHECK(unique != nullptr && *unique == 9);
}

// ---------------------------------------------------------------------------
// 异步完成：在回调线程上恢复

auto viaApi(FakeApi& api, std::thread::id& resumedOn)
    CO2_BEG(co2::Task<int>, (api, resumedOn), int got{};) {
    CO2_AWAIT_AS_SET(got, co2::CallbackAwaitable<int>,
                     co2::fromCallback<int>([this](co2::Continuation<int> done) {
                         api.asyncGet([done](int value) mutable { done(value); });
                     }));
    resumedOn = std::this_thread::get_id();
    CO2_RETURN(got);
}
CO2_END

void asynchronousCompletionResumesOnTheCallbackThread() {
    FakeApi api;
    auto resumedOn = std::thread::id{};
    auto task = viaApi(api, resumedOn);
    co2::detail::TaskAccess::start(task, co2::noop_coroutine());
    CHECK(not co2::detail::TaskAccess::isDone(task));
    CHECK(api.hasPending());

    std::thread callbackThread{[&] { api.fire(42); }};
    auto const callbackThreadId = callbackThread.get_id();
    callbackThread.join();
    CHECK(co2::detail::TaskAccess::isDone(task));
    CHECK(resumedOn == callbackThreadId);
    CHECK(co2::detail::TaskAccess::takeResult(task) == 42);
}

void syncWaitWakesFromACallbackThread() {
    FakeApi api;
    auto resumedOn = std::thread::id{};
    std::thread driver{[&] {
        while (not api.hasPending())
            std::this_thread::yield();
        api.fire(5);
    }};
    CHECK(co2::syncWait(viaApi(api, resumedOn)) == 5);
    driver.join();
}

// 两方竞争：initiate 里直接起线程完成，续体可能先于 await_suspend
// 收尾到达，也可能后到。
auto racing(std::vector<std::thread>& threads)
    CO2_BEG(co2::Task<int>, (threads), int got{};) {
    CO2_AWAIT_AS_SET(got, co2::CallbackAwaitable<int>,
                     co2::fromCallback<int>([this](co2::Continuation<int> done) {
                         threads.emplace_back([done]() mutable { done(1); });
                     }));
    CO2_RETURN(got);
}
CO2_END

void completionRacingWithSuspensionIsResolvedEitherWay() {
    for (int round = 0; round < 500; ++round) {
        std::vector<std::thread> threads;
        CHECK(co2::syncWait(racing(threads)) == 1);
        for (auto& thread : threads)
            thread.join();
    }
}

// ---------------------------------------------------------------------------
// 异常两条通道

auto initiateThrows() CO2_BEG(co2::Task<int>, (), int got{};) {
    CO2_AWAIT_AS_SET(
        got, co2::CallbackAwaitable<int>,
        co2::fromCallback<int>([](co2::Continuation<int>) { throw ExpectedError{}; }));
    CO2_RETURN(got);
}
CO2_END

void exceptionsFromInitiateAreDeliveredIntoTheCoroutine() {
    bool thrown = false;
    try {
        co2::syncWait(initiateThrows());
    } catch (ExpectedError const&) {
        thrown = true;
    }
    CHECK(thrown);
}

auto completesWithException(FakeApi& api) CO2_BEG(co2::Task<int>, (api), int got{};) {
    CO2_AWAIT_AS_SET(got, co2::CallbackAwaitable<int>,
                     co2::fromCallback<int>([this](co2::Continuation<int> done) {
                         api.asyncGet([done](int) mutable {
                             done.setException(
                                 std::make_exception_ptr(ExpectedError{}));
                         });
                     }));
    CO2_RETURN(got);
}
CO2_END

void setExceptionIsRethrownByAwaitResume() {
    FakeApi api;
    auto task = completesWithException(api);
    co2::detail::TaskAccess::start(task, co2::noop_coroutine());
    api.fire(0);
    CHECK(co2::detail::TaskAccess::isDone(task));
    bool thrown = false;
    try {
        co2::detail::TaskAccess::takeResult(task);
    } catch (ExpectedError const&) {
        thrown = true;
    }
    CHECK(thrown);
}

// ---------------------------------------------------------------------------
// stop_token 接到 API 的 cancel

auto cancellable(FakeApi& api, co2::ThreadPool& pool)
    CO2_BEG(co2::Task<int>, (api, pool), int got{};) {
    CO2_AWAIT(co2::scheduleOn(pool));
    CO2_AWAIT_AS_SET(
        got, co2::CallbackAwaitable<int>,
        co2::fromCallback<int>([this](co2::Continuation<int> done,
                                      co2::stop_token token) {
            // 用 shared_ptr 让 stop_callback 活到回调为止：回调到达时释放它。
            auto guard =
                std::make_shared<co2::stop_callback<>>(token, [this] { api.cancel(); });
            api.asyncGetWithCancel([done, guard](bool cancelled, int value) mutable {
                guard.reset();
                if (cancelled)
                    done.setException(std::make_exception_ptr(Cancelled{}));
                else
                    done(value);
            });
        }));
    CO2_RETURN(got);
}
CO2_END

void stopRequestReachesTheApiThroughTheInitiateToken() {
    co2::ThreadPool pool{1U};
    FakeApi api;
    auto handle = co2::spawn(pool, cancellable(api, pool));
    while (not api.hasPending())
        std::this_thread::yield();
    CHECK(not handle.isReady());
    CHECK(
        handle.requestStop()); // → stop_callback → api.cancel() → 续体以 Cancelled 完成
    bool thrown = false;
    try {
        handle.join();
    } catch (Cancelled const&) {
        thrown = true;
    }
    CHECK(thrown);
}

void normalCompletionWinsWhenNoStopIsRequested() {
    co2::ThreadPool pool{1U};
    FakeApi api;
    auto handle = co2::spawn(pool, cancellable(api, pool));
    while (not api.hasPending())
        std::this_thread::yield();
    api.fireCancellable(11);
    CHECK(handle.join() == 11);
}

auto tokenless(co2::stop_token& seen) CO2_BEG(co2::Task<int>, (seen), int got{};) {
    CO2_AWAIT_AS_SET(got, co2::CallbackAwaitable<int>,
                     co2::fromCallback<int>(
                         [this](co2::Continuation<int> done, co2::stop_token token) {
                             seen = token;
                             done(1);
                         }));
    CO2_RETURN(got);
}
CO2_END

void initiateReceivesTheCoroutinesToken() {
    co2::stop_source source;
    co2::stop_token seen;
    CHECK(co2::syncWait(tokenless(seen), source.get_token()) == 1);
    CHECK(seen == source.get_token());
    co2::stop_token rootless;
    CHECK(co2::syncWait(tokenless(rootless)) == 1);
    CHECK(not rootless.stop_possible());
}

// ---------------------------------------------------------------------------
// 泛型 lambda 与作为 API 包装函数被别的协程等待

auto genericInitiate(int v) CO2_BEG(co2::Task<int>, (v), int got{};) {
    CO2_AWAIT_AS_SET(got, co2::CallbackAwaitable<int>,
                     co2::fromCallback<int>([this](auto done) { done(v * 2); }));
    CO2_RETURN(got);
}
CO2_END

auto callsWrapper(FakeApi& api, std::thread::id& resumedOn)
    CO2_BEG(co2::Task<int>, (api, resumedOn), int a{}; int b{};) {
    CO2_AWAIT_SET(a, genericInitiate(4));
    CO2_AWAIT_SET(b, viaApi(api, resumedOn));
    CO2_RETURN(a + b);
}
CO2_END

void wrappedApisComposeLikeAnyTask() {
    FakeApi api;
    auto resumedOn = std::thread::id{};
    std::thread driver{[&] {
        while (not api.hasPending())
            std::this_thread::yield();
        api.fire(2);
    }};
    CHECK(co2::syncWait(callsWrapper(api, resumedOn)) == 10);
    driver.join();
}

// 续体被存进 ManualExecutor 驱动的回调里：完成在驱动线程上发生。
auto viaExecutorCallback(co2::ManualExecutor& executor,
                         std::vector<std::function<void()>>& queue)
    CO2_BEG(co2::Task<int>, (executor, queue), int got{};) {
    CO2_AWAIT_AS_SET(got, co2::CallbackAwaitable<int>,
                     co2::fromCallback<int>([this](co2::Continuation<int> done) {
                         queue.push_back([done]() mutable { done(21); });
                     }));
    CO2_AWAIT(co2::scheduleOn(executor)); // 回到"事件循环"线程的显式方式
    CO2_RETURN(got * 2);
}
CO2_END

void continuationCanBeStoredAndInvokedLater() {
    co2::ManualExecutor executor;
    std::vector<std::function<void()>> queue;
    auto task = viaExecutorCallback(executor, queue);
    co2::detail::TaskAccess::start(task, co2::noop_coroutine());
    CHECK(queue.size() == 1U);
    queue.front()();
    CHECK(not co2::detail::TaskAccess::isDone(task));
    CHECK(executor.run() == 1U);
    CHECK(co2::detail::TaskAccess::takeResult(task) == 42);
}

} // namespace

int main() {
    synchronousCompletionDoesNotSuspend();
    voidContinuationTakesNoArguments();
    resultsAreConstructedInPlaceAndMovedOut();
    asynchronousCompletionResumesOnTheCallbackThread();
    syncWaitWakesFromACallbackThread();
    completionRacingWithSuspensionIsResolvedEitherWay();
    exceptionsFromInitiateAreDeliveredIntoTheCoroutine();
    setExceptionIsRethrownByAwaitResume();
    stopRequestReachesTheApiThroughTheInitiateToken();
    normalCompletionWinsWhenNoStopIsRequested();
    initiateReceivesTheCoroutinesToken();
    wrappedApisComposeLikeAnyTask();
    continuationCanBeStoredAndInvokedLater();
    return 0;
}
