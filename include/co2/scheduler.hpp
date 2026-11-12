#pragma once

#include "co2/completion.hpp"
#include "co2/contract.hpp"
#include "co2/coroutine_handle.hpp"
#include "co2/env.hpp"
#include "co2/stop_token.hpp"

// co2 执行层：Scheduler 是"把一个挂起的协程排队到某个执行上下文上恢复"的最小接口。
// 队列元素就是一个句柄——一个指针——因此一次 hop 在稳态下零分配、零原子门闩。
//
// 与标准模型一致：协程在完成它的那个线程上被内联恢复；想换线程就显式
// CO2_AWAIT(scheduleOn(s))。没有隐式的"当前 scheduler"。
//
// scheduleOn 是取消点（见 completion-channels.md）：协程在目标 scheduler 上被恢复时，
// 它环境里的停止令牌上若已有请求，就以已停止结束，不再往下执行——与 std::execution 的
// run_loop 在轮到一项工作时检查停止请求相同。检查在 hop 之后，所以收尾（局部析构、
// 向上传播）发生在要去的那个线程上。没有停止令牌的协程不受影响；收到停止请求之后还要
// 换线程做收尾，写 CO2_AWAIT(co2::unstoppable(co2::scheduleOn(s)))。

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
// 指针与等待者的停止令牌，不借用表达式里的任何临时对象。
struct ScheduleOn {
    Scheduler* scheduler;
    detail::EnvStopToken token; // 等待者的停止令牌：hop 之后看它上面有没有请求

    bool await_ready() const noexcept { return false; }

    // 令牌必须在 schedule() 之前记下：schedule() 一返回，协程可能已经在另一个线程上
    // 恢复，此后再写这个 awaiter 就是数据竞争。co2 自己的 promise 只是被借用——等待者
    // 在 hop 期间一直挂着——不碰引用计数。
    template <class Promise>
    void await_suspend(coroutine_handle<Promise> const coroutine) noexcept {
        detail::inheritStopToken(token, coroutine);
        scheduler->schedule(coroutine);
    }

    // 不认识 await_completion() 的调用方不被取消：hop 照常完成，协程继续。
    void await_resume() const noexcept {}

    Completion await_completion() const noexcept {
        return token.stopRequested() ? Completion::stopped() : Completion::value();
    }
};

inline ScheduleOn scheduleOn(Scheduler& scheduler) noexcept {
    return ScheduleOn{&scheduler, detail::EnvStopToken{}};
}

} // namespace co2
