// co2 v2 核心的规范用例套件：[dcl.fct.def.coroutine]、[expr.await]、[coroutine.handle]
// 的每条规则对应一个用例。

#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "co2/coroutine.hpp"
#include "trace.hpp"

namespace {

void fail(char const* expression, int line) {
    std::cerr << "check failed at line " << line << ": " << expression << '\n';
    std::exit(EXIT_FAILURE);
}

#define CHECK(expression)                                                              \
    do {                                                                               \
        if (not(expression)) fail(#expression, __LINE__);                              \
    } while (false)

void expectEvents(trace::EventLog const& log, std::vector<std::string> const& expected,
                  int const line) {
    if (log.events == expected) return;
    std::cerr << "event mismatch at line " << line << "\n  actual:\n";
    for (auto const& event : log.events)
        std::cerr << "    " << event << '\n';
    std::cerr << "  expected:\n";
    for (auto const& event : expected)
        std::cerr << "    " << event << '\n';
    std::exit(EXIT_FAILURE);
}

#define EXPECT_EVENTS(log, ...) expectEvents(log, {__VA_ARGS__}, __LINE__)

struct Co2Lib {
    template <class P = void> using Handle = co2::coroutine_handle<P>;
    using SuspendAlways = co2::suspend_always;
    using SuspendNever = co2::suspend_never;
};

using T = trace::Types<Co2Lib>;
using trace::EventLog;
using trace::Tracked;
using trace::TrackedLocal;

// ---------------------------------------------------------------------------
// [dcl.fct.def.coroutine]：promise 构造、get_return_object、initial_suspend 的顺序

auto lazyBody(EventLog& log, bool lazy, Tracked param)
    CO2_BEG(T::IntTask, (log, lazy, param), TrackedLocal local;) {
    local.bind(log);
    log.add("body start");
    CO2_AWAIT(co2::suspend_always{});
    log.add("body resumed");
    CO2_RETURN(42);
}
CO2_END

void lazyCoroutineFollowsTheStandardOrder() {
    EventLog log;
    {
        auto task = lazyBody(log, true, Tracked{log, "param"});
        // 参数副本 → promise(参数) → get_return_object → initial_suspend 挂起，body
        // 未执行。
        EXPECT_EVENTS(log, "param constructed", "promise(params)", "get_return_object",
                      "initial_suspend", "initial_suspend await_ready",
                      "initial_suspend await_suspend");
        CHECK(not task.done());

        task.resume();
        EXPECT_EVENTS(log, "param constructed", "promise(params)", "get_return_object",
                      "initial_suspend", "initial_suspend await_ready",
                      "initial_suspend await_suspend", "initial_suspend await_resume",
                      "local bound", "body start");
        CHECK(not task.done());

        task.resume();
        CHECK(task.done());
        CHECK(task.promise().result == 42);
        // co_return 离开协程体作用域：局部先于 final_suspend 销毁。
        EXPECT_EVENTS(log, "param constructed", "promise(params)", "get_return_object",
                      "initial_suspend", "initial_suspend await_ready",
                      "initial_suspend await_suspend", "initial_suspend await_resume",
                      "local bound", "body start", "body resumed", "return_value(42)",
                      "local destroyed", "final_suspend", "final_suspend await_ready",
                      "final_suspend await_suspend");
        log.events.clear();
    }
    // destroy()：promise → 参数副本。
    EXPECT_EVENTS(log, "promise destroyed", "param destroyed");
}

void eagerCoroutineRunsInsideTheRamp() {
    EventLog log;
    auto task = lazyBody(log, false, Tracked{log, "param"});
    EXPECT_EVENTS(log, "param constructed", "promise(params)", "get_return_object",
                  "initial_suspend", "initial_suspend await_ready",
                  "initial_suspend await_resume", "local bound", "body start");
    task.resume();
    CHECK(task.done());
    CHECK(task.promise().result == 42);
}

// ---------------------------------------------------------------------------
// return_void / 掉出末尾

auto voidBody(EventLog& log, bool lazy) CO2_BEG(T::VoidTask, (log, lazy)) {
    log.add("body");
    CO2_RETURN();
}
CO2_END

auto fallOffTheEnd(EventLog& log, bool lazy) CO2_BEG(T::VoidTask, (log, lazy)) {
    log.add("body");
}
CO2_END

void returnVoidAndFallingOffTheEndBothCallReturnVoid() {
    {
        EventLog log;
        auto task = voidBody(log, false);
        CHECK(task.done());
        EXPECT_EVENTS(log, "promise(params)", "get_return_object", "initial_suspend",
                      "initial_suspend await_ready", "initial_suspend await_resume",
                      "body", "return_void", "final_suspend",
                      "final_suspend await_ready", "final_suspend await_suspend");
    }
    {
        EventLog log;
        auto task = fallOffTheEnd(log, false);
        CHECK(task.done());
        CHECK(log.events.size() >= 7U && log.events[6] == "return_void");
    }
}

// ---------------------------------------------------------------------------
// unhandled_exception

auto throwingBody(EventLog& log, bool lazy, bool rethrow)
    CO2_BEG(T::IntTask, (log, lazy, rethrow), T::IntPromise* self{};) {
    CO2_AWAIT((T::GetPromise<T::IntPromise>{&self}));
    self->rethrowUnhandled = rethrow;
    log.add("body");
    throw std::runtime_error{"boom"};
}
CO2_END

void bodyExceptionsReachUnhandledException() {
    EventLog log;
    auto task = throwingBody(log, false, false);
    CHECK(task.done());
    CHECK(task.promise().error != nullptr);
    EXPECT_EVENTS(log, "promise(params)", "get_return_object", "initial_suspend",
                  "initial_suspend await_ready", "initial_suspend await_resume", "body",
                  "unhandled_exception", "final_suspend", "final_suspend await_ready",
                  "final_suspend await_suspend");
}

auto throwingWithLocal(EventLog& log, bool lazy)
    CO2_BEG(T::VoidTask, (log, lazy), TrackedLocal local;) {
    local.bind(log);
    throw std::runtime_error{"boom"};
}
CO2_END

// 异常离开协程体作用域时局部先销毁，然后才是 unhandled_exception。
void localsAreDestroyedBeforeUnhandledException() {
    EventLog log;
    auto task = throwingWithLocal(log, false);
    CHECK(task.done());
    EXPECT_EVENTS(log, "promise(params)", "get_return_object", "initial_suspend",
                  "initial_suspend await_ready", "initial_suspend await_resume",
                  "local bound", "local destroyed", "unhandled_exception",
                  "final_suspend", "final_suspend await_ready",
                  "final_suspend await_suspend");
}

void rethrowingUnhandledExceptionLeavesTheCoroutineAtFinalSuspend() {
    EventLog log;
    auto task = throwingBody(log, true, true);
    auto propagated = false;
    try {
        task.resume();
    } catch (std::runtime_error const&) {
        propagated = true;
    }
    CHECK(propagated);
    CHECK(task.done()); // 停在 final suspend point，只能 destroy
    CHECK(log.events.back() == "unhandled_exception");
}

// ---------------------------------------------------------------------------
// 初始挂起点之前的异常抛给调用方，协程状态销毁

struct ThrowingInitialTask {
    struct promise_type {
        EventLog* log{};
        int throwAt{};
        promise_type(EventLog& log_, int const throwAt_, Tracked&)
            : log{&log_}, throwAt{throwAt_} {}
        promise_type() = default;
        ~promise_type() {
            if (log != nullptr) log->add("promise destroyed");
        }
        ThrowingInitialTask get_return_object() {
            if (throwAt == 0) throw std::runtime_error{"get_return_object"};
            return ThrowingInitialTask{
                co2::coroutine_handle<promise_type>::from_promise(*this)};
        }
        T::Throwing initial_suspend() { return T::Throwing{log, throwAt}; }
        co2::suspend_never final_suspend() noexcept { return {}; }
        void return_void() {}
        void unhandled_exception() { log->add("unhandled_exception"); }
    };

    // 不拥有：本用例里协程状态总是被库销毁。
    co2::coroutine_handle<promise_type> handle;
};

auto throwsBeforeInitialResume(EventLog& log, int throwAt, Tracked param)
    CO2_BEG(ThrowingInitialTask, (log, throwAt, param)) {
    log.add("body");
}
CO2_END

void checkDestroyedWithoutRunning(EventLog const& log) {
    // 协程体从未执行，unhandled_exception 未被调用，协程状态已销毁。
    for (auto const& event : log.events) {
        CHECK(event != "body" && event != "unhandled_exception");
    }
    CHECK(log.events.back() == "param destroyed");
    CHECK(log.events[log.events.size() - 2U] == "promise destroyed");
}

void exceptionsBeforeTheInitialAwaitResumePropagateToTheCaller() {
    // ramp 阶段：get_return_object、await_ready、await_suspend 抛出。
    for (auto const throwAt : {0, 1, 2}) {
        EventLog log;
        auto propagated = false;
        try {
            throwsBeforeInitialResume(log, throwAt, Tracked{log, "param"});
        } catch (std::runtime_error const&) {
            propagated = true;
        }
        CHECK(propagated);
        checkDestroyedWithoutRunning(log);
    }
    // 第一次 resume() 阶段：initial awaiter 的 await_resume 抛出，同样销毁协程状态。
    {
        EventLog log;
        auto task = throwsBeforeInitialResume(log, 3, Tracked{log, "param"});
        auto propagated = false;
        try {
            task.handle.resume();
        } catch (std::runtime_error const&) {
            propagated = true;
        }
        CHECK(propagated);
        checkDestroyedWithoutRunning(log);
    }
}

// ---------------------------------------------------------------------------
// [expr.await]：await_suspend 的三种返回类型

auto awaitVoid(EventLog& log, bool lazy, bool ready, co2::coroutine_handle<>& parked)
    CO2_BEG(T::IntTask, (log, lazy, ready, parked), int value{};) {
    CO2_AWAIT_SET(value, (T::SuspendVoid{&log, "v", ready, &parked}));
    log.add("after await " + trace::intToString(value));
    CO2_RETURN(value);
}
CO2_END

void awaitSuspendReturningVoidSuspends() {
    EventLog log;
    auto parked = co2::coroutine_handle<>{};
    auto task = awaitVoid(log, false, false, parked);
    CHECK(not task.done());
    CHECK(static_cast<bool>(parked));
    CHECK(log.events.back() == "v await_suspend(void)");

    parked.resume();
    CHECK(task.done());
    CHECK(task.promise().result == 7);
}

void readyAwaiterNeverSuspends() {
    EventLog log;
    auto parked = co2::coroutine_handle<>{};
    auto task = awaitVoid(log, false, true, parked);
    CHECK(task.done());
    CHECK(not parked);
    for (auto const& event : log.events)
        CHECK(event != "v await_suspend(void)");
}

auto awaitBool(EventLog& log, bool lazy, bool result, co2::coroutine_handle<>& parked)
    CO2_BEG(T::VoidTask, (log, lazy, result, parked)) {
    CO2_AWAIT((T::SuspendBool{&log, result, &parked}));
    log.add("after await");
    CO2_RETURN();
}
CO2_END

void awaitSuspendReturningFalseResumesImmediately() {
    EventLog log;
    auto parked = co2::coroutine_handle<>{};
    auto task = awaitBool(log, false, false, parked);
    CHECK(task.done());
    auto const& e = log.events;
    auto found = false;
    for (std::size_t i = 0; i + 2 < e.size(); ++i) {
        if (e[i] == "bool await_suspend -> false" && e[i + 1] == "bool await_resume" &&
            e[i + 2] == "after await") {
            found = true;
        }
    }
    CHECK(found);
}

void awaitSuspendReturningTrueSuspends() {
    EventLog log;
    auto parked = co2::coroutine_handle<>{};
    auto task = awaitBool(log, false, true, parked);
    CHECK(not task.done());
    CHECK(log.events.back() == "bool await_suspend -> true");
    parked.resume();
    CHECK(task.done());
}

auto transferTarget(EventLog& log, bool lazy) CO2_BEG(T::VoidTask, (log, lazy)) {
    log.add("target body");
    CO2_RETURN();
}
CO2_END

auto awaitTransfer(EventLog& log, bool lazy, co2::coroutine_handle<> target,
                   co2::coroutine_handle<>& parked)
    CO2_BEG(T::VoidTask, (log, lazy, target, parked)) {
    CO2_AWAIT((T::SuspendTransfer{&log, target, &parked}));
    log.add("source resumed");
    CO2_RETURN();
}
CO2_END

void awaitSuspendReturningAHandleTransfersControl() {
    EventLog log;
    auto target = transferTarget(log, true);
    auto parked = co2::coroutine_handle<>{};
    auto source = awaitTransfer(log, false, target.handle, parked);
    // source 挂起后 target 在同一个 resume() 循环里运行，然后控制回到调用方。
    CHECK(not source.done());
    CHECK(target.done());
    auto const& e = log.events;
    auto transferIndex = std::size_t{};
    auto targetIndex = std::size_t{};
    for (std::size_t i = 0; i < e.size(); ++i) {
        if (e[i] == "transfer await_suspend") transferIndex = i;
        if (e[i] == "target body") targetIndex = i;
    }
    CHECK(transferIndex < targetIndex);
    parked.resume();
    CHECK(source.done());
}

auto awaitNoop(EventLog& log, bool lazy, co2::coroutine_handle<>& parked)
    CO2_BEG(T::VoidTask, (log, lazy, parked)) {
    CO2_AWAIT((T::SuspendTransfer{&log, co2::noop_coroutine(), &parked}));
    CO2_RETURN();
}
CO2_END

void noopCoroutineReturnsControlToTheResumer() {
    EventLog log;
    auto parked = co2::coroutine_handle<>{};
    auto task = awaitNoop(log, false, parked);
    CHECK(not task.done());
    CHECK(not co2::noop_coroutine().done());
    co2::noop_coroutine().resume();
    co2::noop_coroutine().destroy();
    parked.resume();
    CHECK(task.done());
}

// ---------------------------------------------------------------------------
// awaiter 抛出：异常在 co_await 处抛出，落入协程体 → unhandled_exception

auto awaitThrowing(EventLog& log, bool lazy, int throwAt)
    CO2_BEG(T::VoidTask, (log, lazy, throwAt)) {
    CO2_AWAIT((T::Throwing{&log, throwAt}));
    log.add("not reached");
    CO2_RETURN();
}
CO2_END

void awaiterExceptionsAreDeliveredIntoTheBody() {
    for (auto const throwAt : {1, 2}) {
        EventLog log;
        auto task = awaitThrowing(log, false, throwAt);
        CHECK(task.done());
        CHECK(task.promise().error != nullptr);
        for (auto const& event : log.events)
            CHECK(event != "not reached");
    }
    {
        EventLog log;
        auto task = awaitThrowing(log, false, 3); // 挂起后恢复时 await_resume 抛出
        CHECK(not task.done());
        task.resume();
        CHECK(task.done());
        CHECK(task.promise().error != nullptr);
    }
}

// ---------------------------------------------------------------------------
// await_transform 只作用于 co_await，不作用于 co_yield

auto transformed(EventLog& log, bool lazy)
    CO2_BEG(T::IntTask, (log, lazy), int value{};) {
    CO2_AWAIT_SET(value, 21); // await_transform(int) → Doubled
    CO2_YIELD(value);         // yield_value 的结果不经过 await_transform
    CO2_RETURN(value);
}
CO2_END

void awaitTransformAppliesToCoAwaitOnly() {
    EventLog log;
    auto task = transformed(log, false);
    CHECK(not task.done()); // 停在 yield
    auto transforms = 0;
    for (auto const& event : log.events) {
        if (event == "await_transform(int)") ++transforms;
    }
    CHECK(transforms == 1);
    CHECK(log.events.back() == "yield await_suspend");
    task.resume();
    CHECK(task.done());
    CHECK(task.promise().result == 42);
}

// ---------------------------------------------------------------------------
// operator co_await：成员优先于 ADL

auto awaitWrapped(EventLog& log, bool lazy, int& sum)
    CO2_BEG(T::VoidTask, (log, lazy, sum), int value{};) {
    CO2_AWAIT_SET(value, (trace::MemberAwaitable<Co2Lib>{&log}));
    sum += value;
    CO2_AWAIT_SET(value, (trace::AdlAwaitable<Co2Lib>{&log}));
    sum += value;
    CO2_RETURN();
}
CO2_END

void operatorCoAwaitIsLookedUpAsMemberThenAdl() {
    EventLog log;
    auto sum = 0;
    auto task = awaitWrapped(log, false, sum);
    CHECK(task.done());
    CHECK(sum == 14);
    auto member = false;
    auto adl = false;
    for (auto const& event : log.events) {
        if (event == "member operator co_await") member = true;
        if (event == "adl operator co_await") adl = true;
    }
    CHECK(member && adl);
}

// ---------------------------------------------------------------------------
// final_suspend 不挂起：协程状态自动销毁

auto fireAndForget(EventLog& log, Tracked param)
    CO2_BEG(T::FireAndForget, (log, param), TrackedLocal local;) {
    local.bind(log);
    log.add("body");
    CO2_RETURN();
}
CO2_END

void notSuspendingAtFinalSuspendDestroysTheCoroutineState() {
    EventLog log;
    fireAndForget(log, Tracked{log, "param"});
    EXPECT_EVENTS(log, "param constructed", "promise(params)", "get_return_object",
                  "initial_suspend", "local bound", "body", "return_void",
                  "local destroyed", "final_suspend", "promise destroyed",
                  "param destroyed");
}

// ---------------------------------------------------------------------------
// destroy() 挂起中的协程：awaiter → 局部 → promise → 参数副本

struct TrackedAwaiter {
    EventLog* log;
    ~TrackedAwaiter() { log->add("awaiter destroyed"); }
    bool await_ready() const noexcept { return false; }
    void await_suspend(co2::coroutine_handle<>) const noexcept {}
    void await_resume() const noexcept {}
};

auto suspendedForever(EventLog& log, bool lazy, Tracked param)
    CO2_BEG(T::VoidTask, (log, lazy, param), TrackedLocal local;) {
    local.bind(log);
    CO2_AWAIT((TrackedAwaiter{&log}));
    CO2_RETURN();
}
CO2_END

void destroyingASuspendedCoroutineRunsDestructorsInStandardOrder() {
    EventLog log;
    {
        auto task = suspendedForever(log, false, Tracked{log, "param"});
        CHECK(not task.done());
        log.events.clear();
    }
    // 宏把 awaitable 移动进帧，临时对象的析构发生在 clear() 之前；这里看到的是
    // destroy() 销毁帧内那份 awaiter，然后是局部、promise、参数副本。
    EXPECT_EVENTS(log, "awaiter destroyed", "local destroyed", "promise destroyed",
                  "param destroyed");
}

// ---------------------------------------------------------------------------
// coroutine_handle：from_promise / address 往返、done()

void handleRoundTripsThroughPromiseAndAddress() {
    EventLog log;
    auto task = lazyBody(log, true, Tracked{log, "param"});
    auto& promise = task.promise();
    auto const fromPromise =
        co2::coroutine_handle<T::IntPromise>::from_promise(promise);
    CHECK(fromPromise == task.handle);
    CHECK(fromPromise.address() == task.handle.address());
    auto const fromAddress =
        co2::coroutine_handle<T::IntPromise>::from_address(task.handle.address());
    CHECK(&fromAddress.promise() == &promise);
    CHECK(static_cast<bool>(fromAddress));
    CHECK(not co2::coroutine_handle<>{}.operator bool());
}

// ---------------------------------------------------------------------------
// 分配：promise 的 operator new / operator delete /
// get_return_object_on_allocation_failure

struct AllocationStats {
    int allocations{};
    int deallocations{};
    bool failNext{};
};

AllocationStats& allocationStats() {
    static AllocationStats stats;
    return stats;
}

struct CustomAllocTask {
    struct promise_type {
        // 与 get_return_object_on_allocation_failure 配对的 operator new 必须不抛出。
        static void* operator new(std::size_t const size) noexcept {
            if (allocationStats().failNext) {
                allocationStats().failNext = false;
                return nullptr;
            }
            ++allocationStats().allocations;
            return ::operator new(size);
        }
        static void operator delete(void* const pointer, std::size_t) {
            ++allocationStats().deallocations;
            ::operator delete(pointer);
        }
        static CustomAllocTask get_return_object_on_allocation_failure() {
            return CustomAllocTask{nullptr, true};
        }
        CustomAllocTask get_return_object() {
            return CustomAllocTask{
                co2::coroutine_handle<promise_type>::from_promise(*this), false};
        }
        co2::suspend_always initial_suspend() { return {}; }
        co2::suspend_always final_suspend() noexcept { return {}; }
        void return_void() {}
        void unhandled_exception() {}
    };

    co2::coroutine_handle<promise_type> handle;
    bool allocationFailed;

    CustomAllocTask(co2::coroutine_handle<promise_type> handle_,
                    bool const failed) noexcept
        : handle{handle_}, allocationFailed{failed} {}
    CustomAllocTask(CustomAllocTask&& other) noexcept
        : handle{other.handle}, allocationFailed{other.allocationFailed} {
        other.handle = nullptr;
    }
    ~CustomAllocTask() {
        if (handle) handle.destroy();
    }
};

auto customAllocated() CO2_BEG(CustomAllocTask, ()) { CO2_RETURN(); }
CO2_END

void promiseOperatorNewAndAllocationFailureAreHonored() {
    allocationStats() = AllocationStats{};
    {
        auto task = customAllocated();
        CHECK(not task.allocationFailed);
        CHECK(allocationStats().allocations == 1);
        task.handle.resume();
        CHECK(task.handle.done());
    }
    CHECK(allocationStats().deallocations == 1);

    allocationStats().failNext = true;
    auto failed = customAllocated();
    CHECK(failed.allocationFailed);
    CHECK(not failed.handle);
    CHECK(allocationStats().allocations == 1);
}

template <class Value> struct CountingAllocator {
    using value_type = Value;
    AllocationStats* stats;
    explicit CountingAllocator(AllocationStats& stats_) noexcept : stats{&stats_} {}
    template <class U>
    CountingAllocator(CountingAllocator<U> const& other) noexcept
        : stats{other.stats} {}
    Value* allocate(std::size_t const count) {
        ++stats->allocations;
        return std::allocator<Value>{}.allocate(count);
    }
    void deallocate(Value* const pointer, std::size_t const count) noexcept {
        ++stats->deallocations;
        std::allocator<Value>{}.deallocate(pointer, count);
    }
    template <class U> struct rebind { using other = CountingAllocator<U>; };
};

template <class Allocator>
auto allocatorAware(Allocator allocator, EventLog& log, bool lazy)
    CO2_BEG_WITH_ALLOCATOR(T::VoidTask, allocator, (log, lazy)) {
    CO2_RETURN();
}
CO2_END

void explicitAllocatorIsUsedForTheFrame() {
    AllocationStats stats;
    EventLog log;
    {
        auto task = allocatorAware(CountingAllocator<unsigned char>{stats}, log, true);
        CHECK(stats.allocations == 1);
        task.resume();
        CHECK(task.done());
    }
    CHECK(stats.deallocations == 1);
}

// ---------------------------------------------------------------------------
// 局部跨挂起点存活；参数以引用捕获时是同一个对象

auto accumulate(EventLog& log, bool lazy, int& external)
    CO2_BEG(T::IntTask, (log, lazy, external), int total{};) {
    total += 1;
    external += 10;
    CO2_AWAIT(co2::suspend_always{});
    total += 2;
    external += 10;
    CO2_AWAIT(co2::suspend_always{});
    CO2_RETURN(total);
}
CO2_END

void localsAndReferenceParametersSurviveSuspension() {
    EventLog log;
    auto external = 0;
    auto task = accumulate(log, false, external);
    task.resume();
    task.resume();
    CHECK(task.done());
    CHECK(task.promise().result == 3);
    CHECK(external == 20);
}

} // namespace

int main() {
    lazyCoroutineFollowsTheStandardOrder();
    eagerCoroutineRunsInsideTheRamp();
    returnVoidAndFallingOffTheEndBothCallReturnVoid();
    bodyExceptionsReachUnhandledException();
    localsAreDestroyedBeforeUnhandledException();
    rethrowingUnhandledExceptionLeavesTheCoroutineAtFinalSuspend();
    exceptionsBeforeTheInitialAwaitResumePropagateToTheCaller();
    awaitSuspendReturningVoidSuspends();
    readyAwaiterNeverSuspends();
    awaitSuspendReturningFalseResumesImmediately();
    awaitSuspendReturningTrueSuspends();
    awaitSuspendReturningAHandleTransfersControl();
    noopCoroutineReturnsControlToTheResumer();
    awaiterExceptionsAreDeliveredIntoTheBody();
    awaitTransformAppliesToCoAwaitOnly();
    operatorCoAwaitIsLookedUpAsMemberThenAdl();
    notSuspendingAtFinalSuspendDestroysTheCoroutineState();
    destroyingASuspendedCoroutineRunsDestructorsInStandardOrder();
    handleRoundTripsThroughPromiseAndAddress();
    promiseOperatorNewAndAllocationFailureAreHonored();
    explicitAllocatorIsUsedForTheFrame();
    localsAndReferenceParametersSurviveSuspension();
}
