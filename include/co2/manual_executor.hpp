#pragma once

#include <cstddef>
#include <deque>
#include <mutex>

#include "co2/contract.hpp"
#include "co2/scheduler.hpp"

namespace co2 {

// 由调用方驱动的 FIFO scheduler：schedule() 可以来自任何线程，run()/runOne() 在驱动
// 线程上恢复排队的协程。测试与单线程事件循环的基础件。
//
// 销毁契约：排队中的协程等着被恢复，带着它们销毁 executor 会让它们的拥有者永远等
// 不到完成——这是契约违规。
struct ManualExecutor final : Scheduler {
    ManualExecutor() = default;

    ~ManualExecutor() override { CO2_CONTRACT_CHECK(ready.empty()); }

    void schedule(coroutine_handle<> const coroutine) noexcept override {
        CO2_CONTRACT_CHECK(coroutine);
        std::lock_guard<std::mutex> lock{mutex};
        ready.push_back(coroutine);
    }

    // 恢复队首的一个协程；队列为空时返回 false。
    bool runOne() {
        auto next = coroutine_handle<>{};
        {
            std::lock_guard<std::mutex> lock{mutex};
            if (ready.empty()) return false;
            next = ready.front();
            ready.pop_front();
        }
        next.resume();
        return true;
    }

    // 运行到队列排空（包括运行期间新排入的），返回恢复次数。
    std::size_t run() {
        auto count = std::size_t{};
        while (runOne())
            ++count;
        return count;
    }

    std::size_t pending() const noexcept {
        std::lock_guard<std::mutex> lock{mutex};
        return ready.size();
    }

  private:
    mutable std::mutex mutex;
    std::deque<coroutine_handle<>> ready;
};

} // namespace co2
