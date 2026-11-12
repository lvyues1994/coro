// 三种完成方式：值、错误、已停止（docs/design/completion-channels.md）。
//
// 一次等待结束时，恢复点先问 awaiter 的可选成员 await_completion()：有值就照常
// await_resume() 并继续；出错或已停止就此结束协程——销毁 awaiter、析构局部、把结果交给
// promise 的 return_error() / return_stopped()，然后 co_await final_suspend()。

#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "co2/completion.hpp"
#include "co2/coroutine.hpp"
#include "co2/error.hpp"
#include "co2/manual_executor.hpp"
#include "co2/optional.hpp"
#include "co2/spawn.hpp"
#include "co2/sync_wait.hpp"
#include "co2/task.hpp"
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

// 事件日志：协议调用与析构按发生的顺序记在这里。
std::vector<std::string> events;

void record(char const* const event) { events.emplace_back(event); }

co2::Completion stopped() { return co2::Completion::stopped(); }
co2::Completion failed() { return co2::Completion::error(sample::Errc::gone); }

// ---------------------------------------------------------------------------
// 测试自己的 awaiter

// 立刻就绪，以给定的方式结束；有值时 await_resume() 给出 7。
struct Ends {
    co2::Completion completion;

    bool await_ready() const noexcept { return true; }
    void await_suspend(co2::coroutine_handle<>) const noexcept {}
    int await_resume() const noexcept { return 7; }
    co2::Completion await_completion() noexcept { return completion; }
};

// 同上，并把协议调用与自己的析构记进事件日志。临时对象被移进帧之后析构，那一次不记：
// 只有被问过 await_completion() 的那个对象才记。
struct Logs {
    explicit Logs(co2::Completion completion_) noexcept
        : completion{std::move(completion_)} {}
    Logs(Logs&&) = default;
    ~Logs() {
        if (asked) record("~awaiter");
    }

    bool await_ready() const noexcept { return true; }
    void await_suspend(co2::coroutine_handle<>) const noexcept {}
    void await_resume() const { record("await_resume"); }
    co2::Completion await_completion() noexcept {
        asked = true;
        record("await_completion");
        return completion;
    }

    co2::Completion completion;
    bool asked{};
};

// 真的挂起：把等待者的句柄交给测试，结果由测试在恢复它之前决定。
struct Parks {
    co2::coroutine_handle<>* parked;
    co2::Completion const* outcome;

    bool await_ready() const noexcept { return false; }
    void await_suspend(co2::coroutine_handle<> const handle) const noexcept {
        *parked = handle;
    }
    void await_resume() const noexcept {}
    co2::Completion await_completion() const noexcept { return *outcome; }
};

// 帧里的局部：析构时计数并记进事件日志。
struct Local {
    int* destroyed{};
    ~Local() {
        if (destroyed == nullptr) return;
        ++*destroyed;
        record("~local");
    }
};

// ---------------------------------------------------------------------------
// 测试自己的协程返回类型

enum class Outcome { running, returned, stopped, failed, threw };

// promise 提供两个入口，记下自己是怎么结束的。停在 final suspend，由 Recorder 销毁。
struct Recorder {
    struct promise_type {
        promise_type() = default; // 不是聚合类型：不拿协程的参数去初始化成员

        Recorder get_return_object() noexcept {
            return Recorder{co2::coroutine_handle<promise_type>::from_promise(*this)};
        }
        co2::suspend_never initial_suspend() noexcept { return {}; }
        co2::suspend_always final_suspend() noexcept {
            record("final_suspend");
            return {};
        }
        void return_void() noexcept { outcome = Outcome::returned; }
        void unhandled_exception() noexcept { outcome = Outcome::threw; }

        void return_stopped() noexcept {
            record("return_stopped");
            outcome = Outcome::stopped;
        }

        void return_error(co2::Error error) noexcept {
            record("return_error");
            outcome = Outcome::failed;
            code = error.code();
        }

        // co_yield 的恢复点同样参与：yield 出去的值在这里被变成一次失败的等待。
        Ends yield_value(int) noexcept { return Ends{failed()}; }

        Outcome outcome{Outcome::running};
        std::error_code code;
    };

    explicit Recorder(co2::coroutine_handle<promise_type> const handle_) noexcept
        : handle{handle_} {}
    Recorder(Recorder&& other) noexcept : handle{other.handle} { other.handle = nullptr; }
    Recorder(Recorder const&) = delete;
    Recorder& operator=(Recorder const&) = delete;
    Recorder& operator=(Recorder&&) = delete;
    ~Recorder() {
        if (handle) handle.destroy();
    }

    promise_type& promise() const noexcept { return handle.promise(); }
    bool done() const noexcept { return handle.done(); }

    co2::coroutine_handle<promise_type> handle;
};

// 为标准写的 promise：没有这两个入口，只有 unhandled_exception()。
struct Plain {
    struct promise_type {
        promise_type() = default;

        Plain get_return_object() noexcept {
            return Plain{co2::coroutine_handle<promise_type>::from_promise(*this)};
        }
        co2::suspend_never initial_suspend() noexcept { return {}; }
        co2::suspend_always final_suspend() noexcept { return {}; }
        void return_void() noexcept { returned = true; }
        void unhandled_exception() noexcept { exception = std::current_exception(); }

        std::exception_ptr exception;
        bool returned{};
    };

    explicit Plain(co2::coroutine_handle<promise_type> const handle_) noexcept
        : handle{handle_} {}
    Plain(Plain&& other) noexcept : handle{other.handle} { other.handle = nullptr; }
    Plain(Plain const&) = delete;
    Plain& operator=(Plain const&) = delete;
    Plain& operator=(Plain&&) = delete;
    ~Plain() {
        if (handle) handle.destroy();
    }

    promise_type& promise() const noexcept { return handle.promise(); }

    co2::coroutine_handle<promise_type> handle;
};

// final_suspend 不挂起：协程结束时帧自己销毁。
int detachedStops = 0;

struct Detached {
    struct promise_type {
        promise_type() = default;

        Detached get_return_object() noexcept { return {}; }
        co2::suspend_never initial_suspend() noexcept { return {}; }
        co2::suspend_never final_suspend() noexcept { return {}; }
        void return_void() noexcept {}
        void unhandled_exception() noexcept {}
        void return_stopped() noexcept { ++detachedStops; }
    };
};

// ---------------------------------------------------------------------------
// 已停止与错误就此结束协程

auto endsWith(co2::Completion completion, bool& reached, int& destroyed)
    CO2_BEG(Recorder, (completion, reached, destroyed), Local local;) {
    local.destroyed = &destroyed;
    CO2_AWAIT(Ends{completion});
    reached = true;
}
CO2_END

void aStoppedAwaitEndsTheCoroutine() {
    auto reached = false;
    auto destroyed = 0;
    auto const coroutine = endsWith(stopped(), reached, destroyed);
    CHECK(coroutine.done());
    CHECK(coroutine.promise().outcome == Outcome::stopped);
    CHECK(not reached);     // 等待之后的语句没有执行
    CHECK(destroyed == 1);  // 局部已经析构，不等到帧被销毁
}

void aFailedAwaitDeliversTheErrorToThePromise() {
    auto reached = false;
    auto destroyed = 0;
    auto const coroutine = endsWith(failed(), reached, destroyed);
    CHECK(coroutine.done());
    CHECK(coroutine.promise().outcome == Outcome::failed);
    CHECK(coroutine.promise().code == sample::Errc::gone);
    CHECK(not reached);
    CHECK(destroyed == 1);
}

auto takesTheValue(int& taken) CO2_BEG(Recorder, (taken)) {
    CO2_AWAIT_SET(taken, Ends{co2::Completion::value()});
}
CO2_END

void aValueCompletionContinuesAsUsual() {
    auto taken = 0;
    auto const coroutine = takesTheValue(taken);
    CHECK(taken == 7);
    CHECK(coroutine.promise().outcome == Outcome::returned);
}

// ---------------------------------------------------------------------------
// 调用约定与收尾顺序

auto logsTheAwait(co2::Completion completion, int& destroyed)
    CO2_BEG(Recorder, (completion, destroyed), Local local;) {
    local.destroyed = &destroyed;
    CO2_AWAIT(Logs{completion});
    record("after the await");
}
CO2_END

void awaitCompletionIsAskedOnceBeforeAwaitResume() {
    events.clear();
    auto destroyed = 0;
    {
        auto const coroutine = logsTheAwait(co2::Completion::value(), destroyed);
        CHECK(coroutine.promise().outcome == Outcome::returned);
    }
    auto const expected = std::vector<std::string>{
        "await_completion", "await_resume", "~awaiter",
        "after the await",  "~local",       "final_suspend"};
    CHECK(events == expected);
}

// 与异常逃出协程体时的顺序相同：awaiter → 局部 → promise 的入口 → final_suspend。
void aNonValueCompletionCleansUpInTheSameOrderAsAnException() {
    events.clear();
    auto destroyed = 0;
    {
        auto const coroutine = logsTheAwait(stopped(), destroyed);
        CHECK(coroutine.promise().outcome == Outcome::stopped);
    }
    auto const expected = std::vector<std::string>{
        "await_completion", "~awaiter", "~local", "return_stopped", "final_suspend"};
    CHECK(events == expected);
}

// ---------------------------------------------------------------------------
// 恢复点的各种形态

auto parksThenEnds(co2::coroutine_handle<>& parked, co2::Completion const& outcome,
                   bool& reached)
    CO2_BEG(Recorder, (parked, outcome, reached)) {
    CO2_AWAIT((Parks{&parked, &outcome})); // 花括号里的逗号要用圆括号包住
    reached = true;
}
CO2_END

// awaiter 在别处完成、之后才恢复等待者：结果记在 awaiter 上，恢复时被问到。
void aSuspendedAwaitIsAskedWhenItIsResumed() {
    auto parked = co2::coroutine_handle<>{};
    auto outcome = co2::Completion::value();
    auto reached = false;
    auto const coroutine = parksThenEnds(parked, outcome, reached);
    CHECK(parked);
    CHECK(not coroutine.done());

    outcome = stopped();
    parked.resume();
    CHECK(coroutine.done());
    CHECK(coroutine.promise().outcome == Outcome::stopped);
    CHECK(not reached);
}

auto yieldsOnce(bool& reached) CO2_BEG(Recorder, (reached)) {
    CO2_YIELD(1);
    reached = true;
}
CO2_END

void aYieldIsAResumePointToo() {
    auto reached = false;
    auto const coroutine = yieldsOnce(reached);
    CHECK(coroutine.promise().outcome == Outcome::failed);
    CHECK(coroutine.promise().code == sample::Errc::gone);
    CHECK(not reached);
}

auto awaitsAnLvalue(Ends& awaiter, bool& reached) CO2_BEG(Recorder, (awaiter, reached)) {
    CO2_AWAIT(awaiter);
    reached = true;
}
CO2_END

// 左值 awaiter 在帧里只记地址（detail::AwaiterRef），这个成员同样要转发过去。
void anLvalueAwaiterIsAskedInPlace() {
    auto awaiter = Ends{stopped()};
    auto reached = false;
    auto const coroutine = awaitsAnLvalue(awaiter, reached);
    CHECK(coroutine.promise().outcome == Outcome::stopped);
    CHECK(not reached);
}

auto detachedEndsWith(co2::Completion completion, int& destroyed)
    CO2_BEG(Detached, (completion, destroyed), Local local;) {
    local.destroyed = &destroyed;
    CO2_AWAIT(Ends{completion});
}
CO2_END

void aCoroutineThatDoesNotSuspendAtTheEndDestroysItself() {
    detachedStops = 0;
    auto destroyed = 0;
    detachedEndsWith(stopped(), destroyed);
    CHECK(detachedStops == 1);
    CHECK(destroyed == 1);
}

// ---------------------------------------------------------------------------
// promise 没有入口：错误在协程体内重新抛出，走 unhandled_exception()

auto plainEndsWith(co2::Completion completion, bool& reached, int& destroyed)
    CO2_BEG(Plain, (completion, reached, destroyed), Local local;) {
    local.destroyed = &destroyed;
    CO2_AWAIT(Ends{completion});
    reached = true;
}
CO2_END

void anErrorCodeReachesAStandardPromiseAsSystemError() {
    auto reached = false;
    auto destroyed = 0;
    auto const coroutine = plainEndsWith(failed(), reached, destroyed);
    CHECK(not reached);
    CHECK(destroyed == 1);
    CHECK(not coroutine.promise().returned);

    auto sawCode = false;
    try {
        std::rethrow_exception(coroutine.promise().exception);
    } catch (std::system_error const& exception) {
        sawCode = exception.code() == sample::Errc::gone;
    }
    CHECK(sawCode);
}

void aCapturedExceptionReachesAStandardPromiseUnchanged() {
    auto reached = false;
    auto destroyed = 0;
    auto const original = std::make_exception_ptr(std::runtime_error{"boom"});
    auto const coroutine = plainEndsWith(
        co2::Completion::error(co2::Error{original}), reached, destroyed);
    CHECK(not reached);
    CHECK(destroyed == 1);

    auto sawOriginal = false;
    try {
        std::rethrow_exception(coroutine.promise().exception);
    } catch (std::runtime_error const& exception) {
        sawOriginal = std::string{exception.what()} == "boom";
    }
    CHECK(sawOriginal);
}

// ---------------------------------------------------------------------------
// Task：错误与已停止按值沿等待链向上传，只在根上变回异常或空的 optional

struct ExpectedError : std::runtime_error {
    ExpectedError() : std::runtime_error{"expected"} {}
};

enum class Leaf { returns, fails, stops, throws };

// 记下每一层的局部是按什么顺序析构的。
struct DepthGuard {
    std::vector<int>* order{};
    int depth{};
    ~DepthGuard() {
        if (order != nullptr) order->push_back(depth);
    }
};

// 一条 depth + 1 层的等待链：最里面一层按 leaf 结束，外面每一层等待里面一层、把结果
// 加一。`resumed` 数的是有多少层在等待之后继续执行了。
auto chain(int depth, Leaf leaf, int& resumed, std::vector<int>* order)
    CO2_BEG(co2::Task<int>, (depth, leaf, resumed, order), int value{};
            DepthGuard guard;) {
    guard.order = order;
    guard.depth = depth;
    if (depth == 0) {
        if (leaf == Leaf::throws) throw ExpectedError{};
        if (leaf == Leaf::fails) CO2_AWAIT(Ends{failed()});
        if (leaf == Leaf::stops) CO2_AWAIT(Ends{stopped()});
        CO2_RETURN(1);
    }
    CO2_AWAIT_SET(value, chain(depth - 1, leaf, resumed, order));
    ++resumed;
    CO2_RETURN(value + 1);
}
CO2_END

void valuesStillFlowThroughTheChain() {
    auto resumed = 0;
    CHECK(co2::syncWait(chain(3, Leaf::returns, resumed, nullptr)) == 4);
    CHECK(resumed == 3);
}

void anErrorPassesThroughEveryLayerAndIsThrownAtTheRoot() {
    auto resumed = 0;
    auto order = std::vector<int>{};
    auto sawCode = false;
    try {
        co2::syncWait(chain(3, Leaf::fails, resumed, &order));
    } catch (std::system_error const& exception) {
        sawCode = exception.code() == sample::Errc::gone;
    }
    CHECK(sawCode);
    CHECK(resumed == 0); // 没有一层在等待之后继续执行
    auto const innerFirst = std::vector<int>{0, 1, 2, 3};
    CHECK(order == innerFirst); // 局部由内向外析构
}

void aStoppedTaskEndsEveryAwaiterAndTheRootReturnsEmpty() {
    auto resumed = 0;
    auto order = std::vector<int>{};
    auto const result = co2::syncWait(chain(3, Leaf::stops, resumed, &order));
    CHECK(not result.has_value());
    CHECK(resumed == 0);
    auto const innerFirst = std::vector<int>{0, 1, 2, 3};
    CHECK(order == innerFirst);
}

void anExceptionIsRethrownUnchangedAtTheRoot() {
    auto resumed = 0;
    auto sawOriginal = false;
    try {
        co2::syncWait(chain(3, Leaf::throws, resumed, nullptr));
    } catch (ExpectedError const&) {
        sawOriginal = true;
    }
    CHECK(sawOriginal);
    CHECK(resumed == 0);
}

void joinSeesTheSameThreeOutcomes() {
    co2::ManualExecutor executor;
    auto resumed = 0;
    auto value = co2::spawn(executor, chain(1, Leaf::returns, resumed, nullptr));
    auto stops = co2::spawn(executor, chain(1, Leaf::stops, resumed, nullptr));
    auto fails = co2::spawn(executor, chain(1, Leaf::fails, resumed, nullptr));
    CHECK(executor.run() == 3U);

    CHECK(value.join() == 2);
    CHECK(not stops.join().has_value());
    auto sawCode = false;
    try {
        fails.join();
    } catch (std::system_error const& exception) {
        sawCode = exception.code() == sample::Errc::gone;
    }
    CHECK(sawCode);
}

// 每一层都由恢复循环驱动：十万层的链上传播不长栈，析构也不递归。
void aDeepChainPropagatesWithoutGrowingTheStack() {
    constexpr int depth = 100000;
    auto resumed = 0;
    CHECK(not co2::syncWait(chain(depth, Leaf::stops, resumed, nullptr)).has_value());

    auto sawCode = false;
    try {
        co2::syncWait(chain(depth, Leaf::fails, resumed, nullptr));
    } catch (std::system_error const& exception) {
        sawCode = exception.code() == sample::Errc::gone;
    }
    CHECK(sawCode);
    CHECK(resumed == 0);
}

auto awaitsAnLvalueTask(Leaf leaf, int& resumed, bool& reached)
    CO2_BEG(co2::Task<int>, (leaf, resumed, reached), co2::Task<int> child;
            int value{};) {
    child = chain(0, leaf, resumed, nullptr);
    CO2_AWAIT_SET(value, child);
    reached = true;
    CO2_RETURN(value);
}
CO2_END

void anLvalueTaskPropagatesTheSameWay() {
    auto resumed = 0;
    auto reached = false;
    CHECK(not co2::syncWait(awaitsAnLvalueTask(Leaf::stops, resumed, reached))
                  .has_value());
    CHECK(not reached);
    CHECK(co2::syncWait(awaitsAnLvalueTask(Leaf::returns, resumed, reached)) == 1);
    CHECK(reached);
}

// 只转发标准三个成员的包装 awaiter：它不认识 await_completion()，所以被包住的 Task 的
// 结果只能经 await_resume() 出来。
struct Forwards {
    co2::Task<int> task;

    bool await_ready() const noexcept { return task.await_ready(); }

    template <class Promise>
    co2::coroutine_handle<>
    await_suspend(co2::coroutine_handle<Promise> const awaiting) noexcept {
        return task.await_suspend(awaiting);
    }

    int await_resume() { return task.await_resume(); }
};

auto awaitsThroughAWrapper(Leaf leaf, int& resumed)
    CO2_BEG(co2::Task<int>, (leaf, resumed), int value{};) {
    CO2_AWAIT_SET(value, Forwards{chain(0, leaf, resumed, nullptr)});
    CO2_RETURN(value + 10);
}
CO2_END

// 错误照旧能穿过旧式的调用方：await_resume() 把它抛出来。
void anErrorStillCrossesAnAwaiterThatOnlyKnowsThreeMembers() {
    auto resumed = 0;
    CHECK(co2::syncWait(awaitsThroughAWrapper(Leaf::returns, resumed)) == 11);

    auto sawCode = false;
    try {
        co2::syncWait(awaitsThroughAWrapper(Leaf::fails, resumed));
    } catch (std::system_error const& exception) {
        sawCode = exception.code() == sample::Errc::gone;
    }
    CHECK(sawCode);

    auto sawOriginal = false;
    try {
        co2::syncWait(awaitsThroughAWrapper(Leaf::throws, resumed));
    } catch (ExpectedError const&) {
        sawOriginal = true;
    }
    CHECK(sawOriginal);
}

} // namespace

int main() {
    aStoppedAwaitEndsTheCoroutine();
    aFailedAwaitDeliversTheErrorToThePromise();
    aValueCompletionContinuesAsUsual();
    awaitCompletionIsAskedOnceBeforeAwaitResume();
    aNonValueCompletionCleansUpInTheSameOrderAsAnException();
    aSuspendedAwaitIsAskedWhenItIsResumed();
    aYieldIsAResumePointToo();
    anLvalueAwaiterIsAskedInPlace();
    aCoroutineThatDoesNotSuspendAtTheEndDestroysItself();
    anErrorCodeReachesAStandardPromiseAsSystemError();
    aCapturedExceptionReachesAStandardPromiseUnchanged();
    valuesStillFlowThroughTheChain();
    anErrorPassesThroughEveryLayerAndIsThrownAtTheRoot();
    aStoppedTaskEndsEveryAwaiterAndTheRootReturnsEmpty();
    anExceptionIsRethrownUnchangedAtTheRoot();
    joinSeesTheSameThreeOutcomes();
    aDeepChainPropagatesWithoutGrowingTheStack();
    anLvalueTaskPropagatesTheSameWay();
    anErrorStillCrossesAnAwaiterThatOnlyKnowsThreeMembers();
    return 0;
}
