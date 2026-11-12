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

#include "co2/async_generator.hpp"
#include "co2/coroutine.hpp"
#include "co2/generator.hpp"
#include "co2/task.hpp"
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

// ---------------------------------------------------------------------------
// 场景 8：initial awaiter 的 await_resume 抛出（进入 unhandled_exception，帧保留）

auto co2StartThrows(EventLog& log, bool lazy)
    CO2_BEG(C::StartThrowsTask, (log, lazy)) {
    log.add("body");
    CO2_RETURN();
}
CO2_END

S::StartThrowsTask stdStartThrows(EventLog& log, [[maybe_unused]] bool lazy) {
    log.add("body");
    co_return;
}

// ---------------------------------------------------------------------------
// 场景 9：final_suspend 不挂起（await_ready 为真 / await_suspend 返回 false）时仍然
// 求值 final awaiter 的 await_resume

auto co2Detached(EventLog& log, bool finalReady)
    CO2_BEG(C::Detached, (log, finalReady)) {
    log.add("body");
    CO2_RETURN();
}
CO2_END

S::Detached stdDetached(EventLog& log, [[maybe_unused]] bool finalReady) {
    log.add("body");
    co_return;
}

template <class Make> EventLog runDetached(Make make, bool const finalReady) {
    EventLog log;
    make(log, finalReady);
    log.add("-- returned");
    return log;
}

// ---------------------------------------------------------------------------
// 场景 10：聚合类型的 promise。"能用参数副本构造 promise 就传入"在 C++20 起也包括圆括号
// 聚合初始化：GCC 与 Clang 的原生协程都会拿第一个参数去初始化聚合 promise 的第一个成员，
// co2 的 is_constructible 判断与它们一致。这也是库自己的 promise 类型不能是聚合的原因，
// 见本文件末尾的静态断言。

template <class Lib> struct AggregateTask {
    struct promise_type {
        int firstMember = -1;

        AggregateTask get_return_object() { return AggregateTask{firstMember}; }
        typename Lib::SuspendNever initial_suspend() noexcept { return {}; }
        typename Lib::SuspendNever final_suspend() noexcept { return {}; }
        void return_void() {}
        void unhandled_exception() {}
    };

    int promiseFirstMember;
};

auto co2Aggregate(int value) CO2_BEG(AggregateTask<Co2Lib>, (value)) {
    static_cast<void>(value);
    CO2_RETURN();
}
CO2_END

AggregateTask<StdLib> stdAggregate([[maybe_unused]] int value) { co_return; }

template <class Make> EventLog runAggregate(Make make) {
    EventLog log;
    log.add("promise first member " + trace::intToString(make(7).promiseFirstMember));
    return log;
}

// ---------------------------------------------------------------------------
// 场景 11：成员函数协程。隐式对象参数是第一个协程参数：promise 的构造函数先收到 *this
// （左值），再是其余参数；体内访问的是同一个对象。

template <class Lib> struct MemberProbe {
    struct promise_type {
        EventLog* log;

        template <class Object, class... Rest>
        promise_type(Object& object, EventLog& log_, int& amount, Rest&...)
            : log{&log_} {
            log->add("promise(object value=" + trace::intToString(object.value) +
                     ", amount=" + trace::intToString(amount) + ")");
        }

        MemberProbe get_return_object() { return {}; }
        typename Lib::SuspendNever initial_suspend() noexcept { return {}; }
        typename Lib::SuspendNever final_suspend() noexcept { return {}; }
        void return_void() { log->add("return_void"); }
        void unhandled_exception() {}
    };
};

struct Co2Widget {
    auto bump(EventLog& log, int amount)
        CO2_BEG_MEMBER(MemberProbe<Co2Lib>, (log, amount)) {
        self->value += amount;
        log.add("body value=" + trace::intToString(self->value));
        CO2_RETURN();
    }
    CO2_END

    auto read(EventLog& log, int amount) const
        CO2_BEG_MEMBER(MemberProbe<Co2Lib>, (log, amount)) {
        log.add("const body value=" + trace::intToString(self->value + amount));
        CO2_RETURN();
    }
    CO2_END

    int value{7};
};

struct StdWidget {
    MemberProbe<StdLib> bump(EventLog& log, int amount) {
        value += amount;
        log.add("body value=" + trace::intToString(value));
        co_return;
    }

    MemberProbe<StdLib> read(EventLog& log, int amount) const {
        log.add("const body value=" + trace::intToString(value + amount));
        co_return;
    }

    int value{7};
};

template <class Widget> EventLog runMember() {
    EventLog log;
    Widget widget;
    widget.bump(log, 3);
    log.add("-- after bump value=" + trace::intToString(widget.value));
    auto const& constWidget = widget;
    constWidget.read(log, 1);
    return log;
}

// ---------------------------------------------------------------------------
// 场景 12：co_await 左值。awaiter 就是那个对象本身，协议调用不落在副本上。

template <class Lib> struct IdentityAwaiter {
    explicit IdentityAwaiter(EventLog& log_) : log{&log_} {}
    IdentityAwaiter(IdentityAwaiter const&) = delete;
    IdentityAwaiter& operator=(IdentityAwaiter const&) = delete;

    bool await_ready() {
        log->add(std::string{"await_ready on "} + where());
        return false;
    }
    bool await_suspend(typename Lib::template Handle<>) {
        log->add(std::string{"await_suspend on "} + where());
        return false;
    }
    void await_resume() { log->add(std::string{"await_resume on "} + where()); }

    char const* where() const { return this == original ? "the original" : "a copy"; }

    EventLog* log;
    IdentityAwaiter const* original{this};
};

auto co2AwaitsLvalue(EventLog& log, bool lazy, IdentityAwaiter<Co2Lib>& awaiter)
    CO2_BEG(C::VoidTask, (log, lazy, awaiter)) {
    CO2_AWAIT(awaiter);
    CO2_AWAIT(awaiter);
    CO2_RETURN();
}
CO2_END

S::VoidTask stdAwaitsLvalue([[maybe_unused]] EventLog& log, [[maybe_unused]] bool lazy,
                            IdentityAwaiter<StdLib>& awaiter) {
    co_await awaiter;
    co_await awaiter;
    co_return;
}

template <class Lib, class Task, class Make> EventLog runAwaitsLvalue(Make make) {
    EventLog log;
    IdentityAwaiter<Lib> awaiter{log};
    Task task = make(log, false, awaiter);
    log.add(task.done() ? "-- done" : "-- not done");
    return log;
}

template <class Task, class Make> EventLog runStartThrows(Make make, bool const lazy) {
    EventLog log;
    {
        Task task = make(log, lazy);
        log.add("-- returned to caller");
        if (lazy) {
            try {
                task.resume();
                log.add("-- resume returned");
            } catch (std::runtime_error const&) {
                log.add("-- resume threw");
            }
        }
        log.add(task.done() ? "-- done" : "-- not done");
        log.add(task.promise().error ? "-- exception captured" : "-- no exception");
    }
    log.add("-- owner destroyed");
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
    compare("initial await_resume throws (lazy)",
            runStartThrows<C::StartThrowsTask>(co2StartThrows, true),
            runStartThrows<S::StartThrowsTask>(stdStartThrows, true));
    compare("initial await_resume throws (eager)",
            runStartThrows<C::StartThrowsTask>(co2StartThrows, false),
            runStartThrows<S::StartThrowsTask>(stdStartThrows, false));
    compare("final_suspend does not suspend (await_ready)",
            runDetached(co2Detached, true), runDetached(stdDetached, true));
    compare("final_suspend does not suspend (await_suspend -> false)",
            runDetached(co2Detached, false), runDetached(stdDetached, false));
    compare("aggregate promise is initialized from the parameters",
            runAggregate(co2Aggregate), runAggregate(stdAggregate));
    compare("member function coroutine", runMember<Co2Widget>(),
            runMember<StdWidget>());
    compare("co_await lvalue uses the object in place",
            runAwaitsLvalue<Co2Lib, C::VoidTask>(co2AwaitsLvalue),
            runAwaitsLvalue<StdLib, S::VoidTask>(stdAwaitsLvalue));
}

// 库自己的 promise 类型不能被协程的参数构造。它们若是聚合类型，C++20 起第一个参数只要能
// 转换成 promise 的第一个成员（续体句柄、当前元素指针）就会被拿去初始化它：GCC 报
// -Wmissing-field-initializers，AsyncGenerator 的消费者句柄还会因此一开始就非空。
static_assert(not std::is_constructible<co2::Task<int>::promise_type,
                                        co2::coroutine_handle<>&>::value,
              "TaskPromise must not be constructible from coroutine parameters");
static_assert(
    not std::is_constructible<co2::Generator<int>::promise_type, int*&>::value,
    "GeneratorPromise must not be constructible from coroutine parameters");
static_assert(not std::is_constructible<co2::AsyncGenerator<int>::promise_type,
                                        co2::coroutine_handle<>&>::value,
              "AsyncGeneratorPromise must not be constructible from coroutine parameters");
