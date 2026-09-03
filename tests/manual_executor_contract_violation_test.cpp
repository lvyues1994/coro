#include <atomic>
#include <cstdlib>
#include <memory>

#include "co2/coroutine.hpp"
#include "co2/manual_executor.hpp"
#include "co2/scheduler.hpp"
#include "co2/task.hpp"

namespace {

std::atomic<bool> atExpectedFailurePoint{};

void contractViolationHandler(co2::ContractViolation const&) {
    std::_Exit(atExpectedFailurePoint.load(std::memory_order_acquire) ? 86 : 87);
}

auto hopsOnce(co2::ManualExecutor& executor) CO2_BEG(co2::Task<int>, (executor)) {
    CO2_AWAIT(co2::scheduleOn(executor));
    CO2_RETURN(1);
}
CO2_END

} // namespace

// 排队中的协程等着被恢复：带着它们销毁 executor 是契约违规。排空后的销毁没有问题。
int main() {
    co2::setContractViolationHandler(&contractViolationHandler);
    {
        co2::ManualExecutor executor;
        auto task = hopsOnce(executor);
        co2::detail::TaskAccess::start(task, co2::noop_coroutine());
        if (executor.run() != 1U) return EXIT_FAILURE;
        if (co2::detail::TaskAccess::takeResult(task) != 1) return EXIT_FAILURE;
    }

    // 泄漏是有意的：进程在契约处理器里结束，Task 帧不会被回收。
    auto* const executor = new co2::ManualExecutor;
    auto* const task = new co2::Task<int>{hopsOnce(*executor)};
    co2::detail::TaskAccess::start(*task, co2::noop_coroutine());
    if (executor->pending() != 1U) return EXIT_FAILURE;

    atExpectedFailurePoint.store(true, std::memory_order_release);
    delete executor;
    return EXIT_FAILURE;
}
