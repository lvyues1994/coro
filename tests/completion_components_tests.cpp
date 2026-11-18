// 三种完成方式在各个组件上的表现（docs/design/completion-channels.md 第 4 节）。
// 机制本身——恢复点、promise 的两个入口、Task——在 completion_tests.cpp 里测。

#include <cstdlib>
#include <exception>
#include <iostream>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

#include "co2/as_result.hpp"
#include "co2/async_generator.hpp"
#include "co2/callback.hpp"
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
#include "co2/unstoppable.hpp"
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
using sample::Ends;
using sample::ExpectedError;
using sample::Leaf;
using sample::stopped;

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
    CHECK(not sample::carriesAnException(failure->error()));
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
    CHECK(not sample::carriesAnException(first.error()));
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
    CHECK(not sample::carriesAnException(failure.error()));
    CHECK(failure.error().code() == sample::Errc::gone);
}

// ---------------------------------------------------------------------------
// AsyncGenerator：生产者出错或已停止即流结束，正在等待的那次 next() 以同样的结果完成

// 产生一个元素，然后按 leaf 结束。
auto producesOneThen(Leaf leaf) CO2_BEG(co2::AsyncGenerator<int>, (leaf)) {
    CO2_YIELD(1);
    if (leaf == Leaf::fails) CO2_YIELD(co2::with_error(sample::Errc::gone));
    if (leaf == Leaf::stops) CO2_AWAIT(Ends{stopped()});
#if CO2_HAS_EXCEPTIONS
    if (leaf == Leaf::throws) throw ExpectedError{};
#endif
}
CO2_END

auto drains(Leaf leaf, int& seen, bool& reachedTheEnd)
    CO2_BEG(co2::Task<int>, (leaf, seen, reachedTheEnd),
            co2::AsyncGenerator<int> stream; bool has{};) {
    stream = producesOneThen(leaf);
    for (;;) {
        CO2_AWAIT_SET(has, stream.next());
        if (not has) break;
        seen += stream.value();
    }
    reachedTheEnd = true;
    CO2_RETURN(seen);
}
CO2_END

void aFailingProducerEndsItsConsumerTheSameWay() {
    auto seen = 0;
    auto reachedTheEnd = false;
    CHECK(co2::syncWait(drains(Leaf::returns, seen, reachedTheEnd)) == 1);
    CHECK(reachedTheEnd);

    seen = 0;
    reachedTheEnd = false;
    auto const failure =
        co2::syncWait(co2::asResult(drains(Leaf::fails, seen, reachedTheEnd)));
    CHECK(failure->hasError());
    CHECK(not sample::carriesAnException(failure->error())); // 按值传过来的错误码
    CHECK(failure->error().code() == sample::Errc::gone);
    CHECK(seen == 1); // 出错之前的元素照常交付
    CHECK(not reachedTheEnd);

    seen = 0;
    CHECK(co2::syncWait(co2::asResult(drains(Leaf::stops, seen, reachedTheEnd)))
              ->isStopped());
    CHECK(seen == 1);
    CHECK(not reachedTheEnd);

#if CO2_HAS_EXCEPTIONS
    auto const thrown =
        co2::syncWait(co2::asResult(drains(Leaf::throws, seen, reachedTheEnd)));
    CHECK(thrown->hasError());
    CHECK(thrown->error().exception() != nullptr);
#endif
}

// 返回 0：第二次 next() 有值；1：出错；2：已停止。
auto looksPastTheFailure(Leaf leaf, bool& hasAfterwards)
    CO2_BEG(co2::Task<int>, (leaf, hasAfterwards), co2::AsyncGenerator<int> stream;
            co2::optional<co2::Result<bool>> second; bool has{};) {
    stream = producesOneThen(leaf);
    CO2_AWAIT_SET(has, stream.next());
    CO2_AWAIT_SET(second, co2::asResult(stream.next())); // 生产者在这一步里结束
    CO2_AWAIT_SET(has, stream.next());                   // 流已经结束
    hasAfterwards = has;
    CO2_RETURN(second->isStopped() ? 2 : second->hasError() ? 1 : 0);
}
CO2_END

// 那次失败只交付一次：之后的 next() 只报告流已结束。
void theFailureIsDeliveredOnceAndThenTheStreamIsOver() {
    auto hasAfterwards = true;
    CHECK(co2::syncWait(looksPastTheFailure(Leaf::fails, hasAfterwards)) == 1);
    CHECK(not hasAfterwards);

    hasAfterwards = true;
    CHECK(co2::syncWait(looksPastTheFailure(Leaf::stops, hasAfterwards)) == 2);
    CHECK(not hasAfterwards);

    hasAfterwards = true;
    CHECK(co2::syncWait(looksPastTheFailure(Leaf::returns, hasAfterwards)) == 0);
    CHECK(not hasAfterwards);
}

// ---------------------------------------------------------------------------
// fromCallback：续体的四个出口——值、错误码、异常、已停止（禁用异常时没有异常那一个）

enum class Exit {
    value,
    code,
#if CO2_HAS_EXCEPTIONS
    exception,
#endif
    stopped,
};

// 单独成一个函数：条件编译不能写在宏的实参里。
void completes(co2::Continuation<int> done, Exit const how) {
    if (how == Exit::value) done(5);
    if (how == Exit::code) done.setError(sample::Errc::gone);
#if CO2_HAS_EXCEPTIONS
    if (how == Exit::exception)
        done.setException(std::make_exception_ptr(ExpectedError{}));
#endif
    if (how == Exit::stopped) done.setStopped();
}

auto completesWith(Exit how, bool& reached)
    CO2_BEG(co2::Task<int>, (how, reached), int got{};) {
    CO2_AWAIT_AS_SET(got, co2::CallbackAwaitable<int>,
                     co2::fromCallback<int>([this](co2::Continuation<int> done) {
                         completes(done, how);
                     }));
    reached = true;
    CO2_RETURN(got);
}
CO2_END

void theContinuationHasEveryExit() {
    auto reached = false;
    CHECK(co2::syncWait(completesWith(Exit::value, reached)) == 5);
    CHECK(reached);

    reached = false;
    auto const code = co2::syncWait(co2::asResult(completesWith(Exit::code, reached)));
    CHECK(code->hasError());
    CHECK(not sample::carriesAnException(code->error())); // 按值传过来的错误码
    CHECK(code->error().code() == sample::Errc::gone);
    CHECK(not reached);

#if CO2_HAS_EXCEPTIONS
    auto const exception =
        co2::syncWait(co2::asResult(completesWith(Exit::exception, reached)));
    CHECK(exception->hasError());
    CHECK(exception->error().exception() != nullptr);
    CHECK(not reached);
#endif

    CHECK(co2::syncWait(co2::asResult(completesWith(Exit::stopped, reached)))
              ->isStopped());
    CHECK(not reached);
}

auto waitsForTheApi(co2::Continuation<int>& stored, bool& reached)
    CO2_BEG(co2::Task<int>, (stored, reached), int got{};) {
    CO2_AWAIT_AS_SET(
        got, co2::CallbackAwaitable<int>,
        co2::fromCallback<int>([this](co2::Continuation<int> done) { stored = done; }));
    reached = true;
    CO2_RETURN(got);
}
CO2_END

// API 的取消回调里这样报告"已取消"：等待它的协程以已停止结束。
void anApiThatFinishesLaterCanReportStopped() {
    co2::ManualExecutor executor;
    auto stored = co2::Continuation<int>{};
    auto reached = false;
    auto handle = co2::spawn(executor, waitsForTheApi(stored, reached));
    CHECK(executor.run() == 1U);
    CHECK(stored);
    CHECK(not handle.isReady());

    stored.setStopped();
    CHECK(handle.isReady());
    CHECK(not handle.join().has_value());
    CHECK(not reached);
}

#if CO2_HAS_EXCEPTIONS
// 不认识 await_completion() 的调用方直接调用 await_resume()：错误码以异常的形式出现。
void anErrorCodeFromACallbackRaisesForACallerThatSkipsAwaitCompletion() {
    auto awaitable = co2::fromCallback<int>(
        [](co2::Continuation<int> done) { done.setError(sample::Errc::gone); });
    CHECK(not awaitable.await_suspend(co2::noop_coroutine())); // 已经完成：不挂起
    auto sawCode = false;
    try {
        awaitable.await_resume();
    } catch (std::system_error const& exception) {
        sawCode = exception.code() == sample::Errc::gone;
    }
    CHECK(sawCode);
}
#endif

// ---------------------------------------------------------------------------
// 已停止的产生方：just_stopped 与 stopIfRequested

auto stopsItself(bool& reached) CO2_BEG(co2::Task<int>, (reached)) {
    CO2_AWAIT(co2::just_stopped());
    reached = true;
    CO2_RETURN(0);
}
CO2_END

void justStoppedEndsTheTaskAsStopped() {
    auto reached = false;
    CHECK(not co2::syncWait(stopsItself(reached)).has_value());
    CHECK(not reached);
}

auto passesTwoCancellationPoints(co2::stop_source& source, int& passed)
    CO2_BEG(co2::Task<int>, (source, passed)) {
    CO2_AWAIT(co2::stopIfRequested()); // 还没有请求：继续
    ++passed;
    source.request_stop();
    CO2_AWAIT(co2::stopIfRequested()); // 有请求了：到此为止
    ++passed;
    CO2_RETURN(passed);
}
CO2_END

void stopIfRequestedIsACancellationPoint() {
    co2::stop_source source;
    auto passed = 0;
    auto const result =
        co2::syncWait(passesTwoCancellationPoints(source, passed), source.get_token());
    CHECK(not result.has_value());
    CHECK(passed == 1);
}

// 这棵 Task 树的根没有拿到令牌：别处的停止请求与它无关，取消点什么也不做。
void stopIfRequestedDoesNothingWithoutAToken() {
    co2::stop_source unrelated;
    auto passed = 0;
    CHECK(co2::syncWait(passesTwoCancellationPoints(unrelated, passed)) == 2);
}

auto stopsWhenAsked() CO2_BEG(co2::Task<int>, ()) {
    CO2_AWAIT(co2::stopIfRequested());
    CO2_RETURN(1);
}
CO2_END

// 父的停止请求经 whenAll 转发给每个 child；child 在取消点结束，whenAll 以已停止完成。
void aParentsStopRequestStopsWhenAllThroughItsChildren() {
    co2::stop_source source;
    auto reached = false;
    CHECK(co2::syncWait(awaitsBoth(stopsWhenAsked(), stopsWhenAsked(), reached),
                        source.get_token()) == 2);
    CHECK(reached);

    CHECK(source.request_stop());
    reached = false;
    CHECK(not co2::syncWait(awaitsBoth(stopsWhenAsked(), stopsWhenAsked(), reached),
                            source.get_token())
                  .has_value());
    CHECK(not reached);
}

// ---------------------------------------------------------------------------
// unstoppable：对被包住的操作隐藏停止请求

auto hasAStopToken() CO2_BEG(co2::Task<bool>, (), co2::stop_token token;) {
    CO2_AWAIT_SET(token, co2::getStopToken());
    CO2_RETURN(token.stop_possible());
}
CO2_END

auto awaitsWithAndWithoutTheToken(bool& plain, bool& shielded)
    CO2_BEG(co2::Task<int>, (plain, shielded), co2::Task<bool> kept;) {
    CO2_AWAIT_SET(plain, hasAStopToken());
    CO2_AWAIT_SET(shielded, co2::unstoppable(hasAStopToken()));
    if (shielded) CO2_RETURN(-1);
    kept = hasAStopToken();
    CO2_AWAIT_SET(shielded, co2::unstoppable(kept)); // 左值同样可以
    CO2_RETURN(0);
}
CO2_END

// 被包住的子 Task 的环境里没有停止令牌。
void anUnstoppableChildSeesNoStopToken() {
    co2::stop_source source;
    auto plain = false;
    auto shielded = true;
    CHECK(co2::syncWait(awaitsWithAndWithoutTheToken(plain, shielded),
                        source.get_token()) == 0);
    CHECK(plain);
    CHECK(not shielded);
}

auto passesAShieldedCancellationPoint(int& passed) CO2_BEG(co2::Task<int>, (passed)) {
    CO2_AWAIT(co2::unstoppable(co2::stopIfRequested())); // 看不到请求：继续
    ++passed;
    CO2_AWAIT(co2::unstoppable(co2::suspend_never{})); // 标准写法的 awaiter 也能包
    ++passed;
    CO2_AWAIT(co2::stopIfRequested()); // 没有包住：到此为止
    ++passed;
    CO2_RETURN(passed);
}
CO2_END

void unstoppableHidesTheRequestFromACancellationPoint() {
    co2::stop_source source;
    CHECK(source.request_stop());
    auto passed = 0;
    CHECK(not co2::syncWait(passesAShieldedCancellationPoint(passed), source.get_token())
                  .has_value());
    CHECK(passed == 2);
}

auto shields(Leaf leaf, bool& reached)
    CO2_BEG(co2::Task<int>, (leaf, reached), int value{}; int resumed{};) {
    CO2_AWAIT_SET(value, co2::unstoppable(chain(0, leaf, resumed, nullptr)));
    reached = true;
    CO2_RETURN(value);
}
CO2_END

// 它只隐藏外面的停止请求：被包住的操作自己出错或自己停止，照样传出来。
void unstoppableStillForwardsHowTheOperationEnded() {
    auto reached = false;
    CHECK(co2::syncWait(shields(Leaf::returns, reached)) == 1);
    CHECK(reached);

    reached = false;
    auto const failure = co2::syncWait(co2::asResult(shields(Leaf::fails, reached)));
    CHECK(failure->hasError());
    CHECK(not sample::carriesAnException(failure->error()));
    CHECK(failure->error().code() == sample::Errc::gone);

    CHECK(co2::syncWait(co2::asResult(shields(Leaf::stops, reached)))->isStopped());
    CHECK(not reached);
}

// ---------------------------------------------------------------------------
// scheduleOn 是取消点：hop 之后，等待者的停止令牌上有请求就以已停止结束

auto hopsForever(co2::Scheduler& scheduler, int& hops)
    CO2_BEG(co2::Task<int>, (scheduler, hops)) {
    for (;;) {
        CO2_AWAIT(co2::scheduleOn(scheduler));
        ++hops;
    }
}
CO2_END

void aHoppingTaskStopsAtItsNextHop() {
    co2::ManualExecutor executor;
    auto hops = 0;
    auto handle = co2::spawn(executor, hopsForever(executor, hops));
    CHECK(executor.runOne()); // 启动，第一次 hop 排进队列
    CHECK(executor.runOne()); // hop 回来：没有停止请求，继续
    CHECK(hops == 1);
    CHECK(not handle.isReady());

    CHECK(handle.requestStop());
    CHECK(executor.runOne()); // 检查发生在 hop 之后：这一次 hop 照常执行，回来时结束
    CHECK(hops == 1);
    CHECK(handle.isReady());
    CHECK(executor.pending() == 0U);
    CHECK(not handle.join().has_value());
}

auto finishesItsCleanup(co2::Scheduler& scheduler, bool& cleanedUp)
    CO2_BEG(co2::Task<int>, (scheduler, cleanedUp)) {
    CO2_AWAIT(co2::unstoppable(co2::scheduleOn(scheduler))); // 收尾阶段：不被取消
    cleanedUp = true;
    CO2_AWAIT(co2::scheduleOn(scheduler)); // 普通的 hop：取消点
    CO2_RETURN(1);
}
CO2_END

void anUnstoppableHopIsNotACancellationPoint() {
    co2::ManualExecutor executor;
    auto cleanedUp = false;
    auto handle = co2::spawn(executor, finishesItsCleanup(executor, cleanedUp));
    CHECK(handle.requestStop());
    CHECK(executor.run() == 3U); // 启动、第一次 hop、第二次 hop
    CHECK(cleanedUp);
    CHECK(not handle.join().has_value());
}

// 只转发标准三个成员的包装 awaiter。
struct ForwardsTheHop {
    co2::ScheduleOn hop;

    bool await_ready() const noexcept { return hop.await_ready(); }

    template <class Promise>
    void await_suspend(co2::coroutine_handle<Promise> const awaiting) noexcept {
        hop.await_suspend(awaiting);
    }

    void await_resume() const noexcept { hop.await_resume(); }
};

auto hopsThroughAWrapper(co2::Scheduler& scheduler)
    CO2_BEG(co2::Task<int>, (scheduler)) {
    CO2_AWAIT(ForwardsTheHop{co2::scheduleOn(scheduler)});
    CO2_RETURN(1);
}
CO2_END

// 不认识 await_completion() 的调用方不被取消：hop 照常完成，协程继续。
void aCallerThatSkipsAwaitCompletionIsNotCancelledByAHop() {
    co2::ManualExecutor executor;
    auto handle = co2::spawn(executor, hopsThroughAWrapper(executor));
    CHECK(handle.requestStop());
    CHECK(executor.run() == 2U);
    CHECK(handle.join() == 1);
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
    aFailingProducerEndsItsConsumerTheSameWay();
    theFailureIsDeliveredOnceAndThenTheStreamIsOver();
    theContinuationHasEveryExit();
    anApiThatFinishesLaterCanReportStopped();
#if CO2_HAS_EXCEPTIONS
    anErrorCodeFromACallbackRaisesForACallerThatSkipsAwaitCompletion();
#endif
    justStoppedEndsTheTaskAsStopped();
    stopIfRequestedIsACancellationPoint();
    stopIfRequestedDoesNothingWithoutAToken();
    aParentsStopRequestStopsWhenAllThroughItsChildren();
    anUnstoppableChildSeesNoStopToken();
    unstoppableHidesTheRequestFromACancellationPoint();
    unstoppableStillForwardsHowTheOperationEnded();
    aHoppingTaskStopsAtItsNextHop();
    anUnstoppableHopIsNotACancellationPoint();
    aCallerThatSkipsAwaitCompletionIsNotCancelledByAHop();
    return 0;
}
