#pragma once

#include "co2/contract.hpp"
#include "co2/coroutine_handle.hpp"

// co2 执行层：Scheduler 是"把一个挂起的协程排队到某个执行上下文上恢复"的最小接口。
// 队列元素就是一个句柄——一个指针——因此一次 hop 在稳态下零分配、零原子门闩。
//
// 与标准模型一致：协程在完成它的那个线程上被内联恢复；想换线程就显式
// CO2_AWAIT(scheduleOn(s))。没有隐式的"当前 scheduler"。

namespace co2 {

struct Scheduler {
    Scheduler() = default;
    Scheduler(Scheduler const&) = delete;
    Scheduler& operator=(Scheduler const&) = delete;
    virtual ~Scheduler() = default;

    // 把一个挂起的协程排队，稍后在本 scheduler 的某个线程上 resume() 它。
    //
    // 前置条件：句柄非空，指向一个挂起、未 done 的协程；同一协程在被恢复前不会被再
    // 次排队。noexcept：队列扩容失败是致命错误（与 std::thread 创建失败同类）。
    virtual void schedule(coroutine_handle<> coroutine) noexcept = 0;
};

// co_await scheduleOn(s)：把当前协程转移到 s 上继续执行。awaiter 只持有 scheduler 的
// 指针，不借用表达式里的任何临时对象。
struct ScheduleOn {
    Scheduler* scheduler;

    bool await_ready() const noexcept { return false; }

    void await_suspend(coroutine_handle<> const coroutine) noexcept {
        scheduler->schedule(coroutine);
    }

    void await_resume() const noexcept {}
};

inline ScheduleOn scheduleOn(Scheduler& scheduler) noexcept {
    return ScheduleOn{&scheduler};
}

} // namespace co2
