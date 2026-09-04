#include <atomic>
#include <cstdlib>

#include "co2/coroutine.hpp"
#include "co2/task.hpp"

namespace {

std::atomic<bool> atExpectedFailurePoint{};

void contractViolationHandler(co2::ContractViolation const&) {
    std::_Exit(atExpectedFailurePoint.load(std::memory_order_acquire) ? 86 : 87);
}

// 把自己的句柄交给体内代码的 awaiter。
struct ExposeHandle {
    co2::coroutine_handle<>* slot;
    bool await_ready() const noexcept { return false; }
    bool await_suspend(co2::coroutine_handle<> const self) const noexcept {
        *slot = self;
        return false; // 不真正挂起
    }
    void await_resume() const noexcept {}
};

auto resumesItself(co2::coroutine_handle<>& self) CO2_BEG(co2::Task<int>, (self)) {
    CO2_AWAIT(ExposeHandle{&self});
    // 协程正在运行：resume() 自己是标准意义上的未定义行为，co2 报告契约违规。
    atExpectedFailurePoint.store(true, std::memory_order_release);
    self.resume();
    CO2_RETURN(0);
}
CO2_END

} // namespace

int main() {
    co2::setContractViolationHandler(&contractViolationHandler);
    co2::coroutine_handle<> self;
    auto task = resumesItself(self);
    co2::detail::TaskAccess::start(task, co2::noop_coroutine());
    return EXIT_FAILURE;
}
