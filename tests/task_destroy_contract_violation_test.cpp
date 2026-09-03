#include <atomic>
#include <cstdlib>
#include <utility>

#include "co2/coroutine.hpp"
#include "co2/task.hpp"

namespace {

std::atomic<bool> atExpectedFailurePoint{};

void contractViolationHandler(co2::ContractViolation const&) {
    std::_Exit(atExpectedFailurePoint.load(std::memory_order_acquire) ? 86 : 87);
}

// 把等待者停放起来、永不恢复的操作。
struct Parked {
    bool await_ready() const noexcept { return false; }
    void await_suspend(co2::coroutine_handle<>) const noexcept {}
    void await_resume() const noexcept {}
};

auto waitsForever() CO2_BEG(co2::Task<int>, ()) {
    CO2_AWAIT(Parked{});
    CO2_RETURN(0);
}
CO2_END

auto finishes() CO2_BEG(co2::Task<int>, ()) { CO2_RETURN(1); }
CO2_END

} // namespace

// 销毁一个已启动、尚未完成的 Task 是标准意义上的未定义行为（它挂起在某个操作上）；
// co2 v2 把它报告为契约违规。未启动的与已完成的 Task 可以随时销毁。
int main() {
    co2::setContractViolationHandler(&contractViolationHandler);
    {
        auto unstarted = waitsForever();
        static_cast<void>(unstarted);
    }
    {
        auto done = finishes();
        co2::detail::TaskAccess::start(done, co2::noop_coroutine());
        if (co2::detail::TaskAccess::takeResult(done) != 1) return EXIT_FAILURE;
    }

    auto pending = waitsForever();
    co2::detail::TaskAccess::start(pending, co2::noop_coroutine());
    if (co2::detail::TaskAccess::isDone(pending)) return EXIT_FAILURE;

    atExpectedFailurePoint.store(true, std::memory_order_release);
    pending = co2::Task<int>{};
    return EXIT_FAILURE;
}
