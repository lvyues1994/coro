#include <atomic>
#include <cstdlib>

#include "co2/coroutine.hpp"
#include "co2/manual_executor.hpp"
#include "co2/spawn.hpp"
#include "co2/task.hpp"

namespace {

std::atomic<bool> atExpectedFailurePoint{};

void contractViolationHandler(co2::ContractViolation const&) {
    std::_Exit(atExpectedFailurePoint.load(std::memory_order_acquire) ? 86 : 87);
}

auto answer() CO2_BEG(co2::Task<int>, ()) { CO2_RETURN(1); }
CO2_END

auto awaitsBorrowed(co2::JoinHandle<int>& handle)
    CO2_BEG(co2::Task<int>, (handle), int value{};) {
    CO2_AWAIT_SET(value, handle);
    CO2_RETURN(value);
}
CO2_END

} // namespace

// 借用 JoinHandle 的等待者还挂着时销毁 handle：等待者恢复时会访问已销毁的对象，这是
// 契约违规。（泄漏是有意的：进程在契约处理器里结束。）
int main() {
    co2::setContractViolationHandler(&contractViolationHandler);
    auto* const executor = new co2::ManualExecutor; // 永不 run：spawn 的 Task 不会完成
    auto* const handle = new co2::JoinHandle<int>{co2::spawn(*executor, answer())};
    auto* const waiter = new co2::Task<int>{awaitsBorrowed(*handle)};
    co2::detail::TaskAccess::start(*waiter, co2::noop_coroutine());
    if (co2::detail::TaskAccess::isDone(*waiter)) return EXIT_FAILURE;

    atExpectedFailurePoint.store(true, std::memory_order_release);
    delete handle;
    return EXIT_FAILURE;
}
