#include <atomic>
#include <cstdlib>
#include <cstring>
#include <system_error>

#include "co2/config.hpp"
#include "co2/coroutine.hpp"
#include "co2/just.hpp"
#include "co2/sync_wait.hpp"
#include "co2/task.hpp"

#if CO2_HAS_EXCEPTIONS
#error "this test is for builds with exceptions disabled (CO2_TEST_NO_EXCEPTIONS)"
#endif

namespace {

std::atomic<bool> atExpectedFailurePoint{};

// 预期的违规由 Error::raise() 报告（error.hpp）。
void contractViolationHandler(co2::ContractViolation const& violation) {
    auto const expected = atExpectedFailurePoint.load(std::memory_order_acquire) &&
                          std::strstr(violation.file, "error.hpp") != nullptr;
    std::_Exit(expected ? 86 : 87);
}

auto mayFail(bool fail) CO2_BEG(co2::Task<int>, (fail)) {
    if (fail) CO2_AWAIT(co2::just_error(std::make_error_code(std::errc::timed_out)));
    CO2_RETURN(1);
}
CO2_END

// 只转发标准三个成员的包装 awaiter：它不认识 await_completion()。
struct Forwards {
    co2::Task<int> task;

    bool await_ready() const noexcept { return task.await_ready(); }

    template <class Promise>
    co2::coroutine_handle<>
    await_suspend(co2::coroutine_handle<Promise> const awaiting) noexcept {
        return task.await_suspend(awaiting);
    }

    int await_resume() { return task.await_resume(); }
};

auto awaitsThroughAWrapper(bool fail) CO2_BEG(co2::Task<int>, (fail), int value{};) {
    CO2_AWAIT_SET(value, Forwards{mayFail(fail)});
    CO2_RETURN(value);
}
CO2_END

} // namespace

// 开异常时，不认识 await_completion() 的调用方从 await_resume() 拿到异常。禁用异常的
// 构建里 await_resume() 没有办法报告错误：契约违规。包装 awaiter 需要把
// await_completion() 也转发出去。
int main() {
    co2::setContractViolationHandler(&contractViolationHandler);
    if (co2::syncWait(awaitsThroughAWrapper(false)) != 1) return EXIT_FAILURE;

    atExpectedFailurePoint.store(true, std::memory_order_release);
    co2::syncWait(awaitsThroughAWrapper(true));
    return EXIT_FAILURE;
}
