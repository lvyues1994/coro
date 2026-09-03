// 差分测试（C++20）：同一份 promise / awaiter 类型（trace.hpp）分别由 co2
// 宏协程 与真正的 co_await 协程驱动，逐场景比对协议调用序列。协程体是两份（宏 vs
// 语法）， promise 与 awaiter 代码字面共享。

#include <coroutine>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "co2/coroutine.hpp"
#include "trace.hpp"

namespace {

using trace::EventLog;
using trace::Tracked;
using trace::TrackedLocal;

struct Co2Lib {
    template <class P = void> using Handle = co2::coroutine_handle<P>;
    using SuspendAlways = co2::suspend_always;
    using SuspendNever = co2::suspend_never;
};

struct StdLib {
    template <class P = void> using Handle = std::coroutine_handle<P>;
    using SuspendAlways = std::suspend_always;
    using SuspendNever = std::suspend_never;
};

using C = trace::Types<Co2Lib>;
using S = trace::Types<StdLib>;

// 真正拼作 operator co_await 的 awaitable：C++20 构建下 co2 也必须找到它们。
template <class Lib> struct RealMemberAwaitable {
    EventLog* log;
    typename trace::Types<Lib>::SuspendVoid operator co_await() const {
        log->add("member operator co_await");
        return typename trace::Types<Lib>::SuspendVoid{log, "wrapped", true, nullptr};
    }
};

template <class Lib> struct RealAdlAwaitable { EventLog* log; };

template <class Lib>
typename trace::Types<Lib>::SuspendVoid
operator co_await(RealAdlAwaitable<Lib> const& awaitable) {
    awaitable.log->add("adl operator co_await");
    return typename trace::Types<Lib>::SuspendVoid{awaitable.log, "wrapped", true,
                                                   nullptr};
}

void compare(char const* const name, EventLog const& co2Log, EventLog const& stdLog) {
    if (co2Log.events == stdLog.events) return;
    std::cerr << "differential mismatch in scenario " << name << "\n  co2:\n";
    for (auto const& event : co2Log.events)
        std::cerr << "    " << event << '\n';
    std::cerr << "  std:\n";
    for (auto const& event : stdLog.events)
        std::cerr << "    " << event << '\n';
    std::exit(EXIT_FAILURE);
}

// ---------------------------------------------------------------------------
// 场景 1：惰性协程的完整生命周期（参数副本、promise 构造、局部、co_return、destroy）

auto co2Lazy(EventLog& log, bool lazy, Tracked param)
    CO2_BEG(C::IntTask, (log, lazy, param), TrackedLocal local;) {
    local.bind(log);
    log.add("body start");
    CO2_AWAIT(co2::suspend_always{});
    log.add("body resumed");
    CO2_RETURN(42);
}
CO2_END

S::IntTask stdLazy(EventLog& log, [[maybe_unused]] bool lazy,
                   [[maybe_unused]] Tracked param) {
    TrackedLocal local;
    local.bind(log);
    log.add("body start");
    co_await std::suspend_always{};
    log.add("body resumed");
    co_return 42;
}

template <class Task, class Make> EventLog runLifecycle(Make make, bool const lazy) {
    EventLog log;
    {
        Task task = make(log, lazy, Tracked{log, "param"});
        log.add("-- returned to caller");
        if (lazy) {
            task.resume();
            log.add("-- first resume returned");
        }
        task.resume();
        log.add(task.done() ? "-- done" : "-- not done");
        log.add("result " + trace::intToString(task.promise().result));
    }
    log.add("-- owner destroyed");
    return log;
}

// ---------------------------------------------------------------------------
// 场景 2：await_suspend 的三种返回类型 + 停放的句柄

auto co2Suspends(EventLog& log, bool lazy, co2::coroutine_handle<>& parked,
                 bool boolResult)
    CO2_BEG(C::VoidTask, (log, lazy, parked, boolResult), int value{};) {
    CO2_AWAIT_SET(value, (C::SuspendVoid{&log, "v", false, &parked}));
    log.add("value " + trace::intToString(value));
    CO2_AWAIT((C::SuspendBool{&log, boolResult, &parked}));
    log.add("after bool");
    CO2_RETURN();
}
CO2_END

S::VoidTask stdSuspends(EventLog& log, [[maybe_unused]] bool lazy,
                        std::coroutine_handle<>& parked, bool boolResult) {
    int value = co_await S::SuspendVoid{&log, "v", false, &parked};
    log.add("value " + trace::intToString(value));
    co_await S::SuspendBool{&log, boolResult, &parked};
    log.add("after bool");
    co_return;
}

template <class Task, class Handle, class Make>
EventLog runSuspends(Make make, bool const boolResult) {
    EventLog log;
    auto parked = Handle{};
    Task task = make(log, false, parked, boolResult);
    log.add(task.done() ? "-- done" : "-- suspended");
    while (not task.done()) {
        log.add("-- resuming parked");
        auto const next = parked;
        parked = nullptr;
        next.resume();
    }
    return log;
}

// ---------------------------------------------------------------------------
// 场景 3：对称转移

auto co2Target(EventLog& log, bool lazy) CO2_BEG(C::VoidTask, (log, lazy)) {
    log.add("target body");
    CO2_RETURN();
}
CO2_END

S::VoidTask stdTarget(EventLog& log, [[maybe_unused]] bool lazy) {
    log.add("target body");
    co_return;
}

auto co2Transfer(EventLog& log, bool lazy, co2::coroutine_handle<> target,
                 co2::coroutine_handle<>& parked)
    CO2_BEG(C::VoidTask, (log, lazy, target, parked)) {
    CO2_AWAIT((C::SuspendTransfer{&log, target, &parked}));
    log.add("source resumed");
    CO2_RETURN();
}
CO2_END

S::VoidTask stdTransfer(EventLog& log, [[maybe_unused]] bool lazy,
                        std::coroutine_handle<> target,
                        std::coroutine_handle<>& parked) {
    co_await S::SuspendTransfer{&log, target, &parked};
    log.add("source resumed");
    co_return;
}

template <class Task, class Handle, class MakeTarget, class MakeSource>
EventLog runTransfer(MakeTarget makeTarget, MakeSource makeSource) {
    EventLog log;
    Task target = makeTarget(log, true);
    auto parked = Handle{};
    Task source = makeSource(log, false, target.handle, parked);
    log.add(target.done() ? "-- target done" : "-- target pending");
    log.add(source.done() ? "-- source done" : "-- source pending");
    parked.resume();
    log.add(source.done() ? "-- source done" : "-- source pending");
    return log;
}

// ---------------------------------------------------------------------------
// 场景 4：await_transform、co_yield、operator co_await

auto co2Transforms(EventLog& log, bool lazy, int& sum)
    CO2_BEG(C::IntTask, (log, lazy, sum), int value{};) {
    CO2_AWAIT_SET(value, 21);
    sum += value;
    CO2_YIELD(value);
    CO2_AWAIT_SET(value, (RealMemberAwaitable<Co2Lib>{&log}));
    sum += value;
    CO2_AWAIT_SET(value, (RealAdlAwaitable<Co2Lib>{&log}));
    sum += value;
    CO2_RETURN(sum);
}
CO2_END

S::IntTask stdTransforms(EventLog& log, [[maybe_unused]] bool lazy, int& sum) {
    int value = co_await 21;
    sum += value;
    co_yield value;
    value = co_await RealMemberAwaitable<StdLib>{&log};
    sum += value;
    value = co_await RealAdlAwaitable<StdLib>{&log};
    sum += value;
    co_return sum;
}

template <class Task, class Make> EventLog runTransforms(Make make) {
    EventLog log;
    auto sum = 0;
    Task task = make(log, false, sum);
    log.add(task.done() ? "-- done" : "-- suspended at yield");
    task.resume();
    log.add("result " + trace::intToString(task.promise().result));
    return log;
}

// ---------------------------------------------------------------------------
// 场景 5：异常离开协程体（局部先销毁，再 unhandled_exception）

auto co2Throws(EventLog& log, bool lazy)
    CO2_BEG(C::VoidTask, (log, lazy), TrackedLocal local;) {
    local.bind(log);
    throw std::runtime_error{"boom"};
}
CO2_END

S::VoidTask stdThrows(EventLog& log, [[maybe_unused]] bool lazy) {
    TrackedLocal local;
    local.bind(log);
    throw std::runtime_error{"boom"};
    co_return; // 让它成为协程（不可达）
}

template <class Task, class Make> EventLog runThrows(Make make) {
    EventLog log;
    Task task = make(log, false);
    log.add(task.done() ? "-- done" : "-- suspended");
    log.add(task.promise().error ? "-- exception captured" : "-- no exception");
    return log;
}

// ---------------------------------------------------------------------------
// 场景 6：destroy() 挂起中的协程

auto co2Parked(EventLog& log, bool lazy, Tracked param)
    CO2_BEG(C::VoidTask, (log, lazy, param), TrackedLocal local;) {
    local.bind(log);
    CO2_AWAIT(co2::suspend_always{});
    CO2_RETURN();
}
CO2_END

S::VoidTask stdParked(EventLog& log, [[maybe_unused]] bool lazy,
                      [[maybe_unused]] Tracked param) {
    TrackedLocal local;
    local.bind(log);
    co_await std::suspend_always{};
    co_return;
}

template <class Task, class Make> EventLog runDestroyWhileSuspended(Make make) {
    EventLog log;
    {
        Task task = make(log, false, Tracked{log, "param"});
        log.add("-- destroying");
    }
    return log;
}

// ---------------------------------------------------------------------------
// 场景 7：final_suspend 不挂起，协程状态自动销毁

auto co2Fire(EventLog& log, Tracked param)
    CO2_BEG(C::FireAndForget, (log, param), TrackedLocal local;) {
    local.bind(log);
    log.add("body");
    CO2_RETURN();
}
CO2_END

S::FireAndForget stdFire(EventLog& log, [[maybe_unused]] Tracked param) {
    TrackedLocal local;
    local.bind(log);
    log.add("body");
    co_return;
}

template <class Make> EventLog runFire(Make make) {
    EventLog log;
    make(log, Tracked{log, "param"});
    log.add("-- returned");
    return log;
}

} // namespace

int main() {
    compare("lifecycle (lazy)", runLifecycle<C::IntTask>(co2Lazy, true),
            runLifecycle<S::IntTask>(stdLazy, true));
    compare("lifecycle (eager)", runLifecycle<C::IntTask>(co2Lazy, false),
            runLifecycle<S::IntTask>(stdLazy, false));
    compare("await_suspend (bool false)",
            runSuspends<C::VoidTask, co2::coroutine_handle<>>(co2Suspends, false),
            runSuspends<S::VoidTask, std::coroutine_handle<>>(stdSuspends, false));
    compare("await_suspend (bool true)",
            runSuspends<C::VoidTask, co2::coroutine_handle<>>(co2Suspends, true),
            runSuspends<S::VoidTask, std::coroutine_handle<>>(stdSuspends, true));
    compare("symmetric transfer",
            runTransfer<C::VoidTask, co2::coroutine_handle<>>(co2Target, co2Transfer),
            runTransfer<S::VoidTask, std::coroutine_handle<>>(stdTarget, stdTransfer));
    compare("await_transform / co_yield / operator co_await",
            runTransforms<C::IntTask>(co2Transforms),
            runTransforms<S::IntTask>(stdTransforms));
    compare("unhandled_exception", runThrows<C::VoidTask>(co2Throws),
            runThrows<S::VoidTask>(stdThrows));
    compare("destroy while suspended", runDestroyWhileSuspended<C::VoidTask>(co2Parked),
            runDestroyWhileSuspended<S::VoidTask>(stdParked));
    compare("fire and forget", runFire(co2Fire), runFire(stdFire));
}
