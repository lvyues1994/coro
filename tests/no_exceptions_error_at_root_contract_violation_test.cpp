#include <atomic>
#include <cstdlib>
#include <cstring>
#include <system_error>

#include "co2/as_result.hpp"
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

auto fails() CO2_BEG(co2::Task<int>, ()) {
    CO2_AWAIT(co2::just_error(std::make_error_code(std::errc::timed_out)));
    CO2_RETURN(0);
}
CO2_END

} // namespace

// 禁用异常的构建里，错误到达根（syncWait / join）是契约违规：没有异常可以抛。想在根上
// 拿到错误，写 syncWait(asResult(task))——它把三种结果都作为值交出来，不终止程序。
int main() {
    co2::setContractViolationHandler(&contractViolationHandler);

    auto const outcome = co2::syncWait(co2::asResult(fails()));
    if (not outcome.has_value() || not outcome->hasError()) return EXIT_FAILURE;
    if (outcome->error().code() != std::errc::timed_out) return EXIT_FAILURE;

    atExpectedFailurePoint.store(true, std::memory_order_release);
    co2::syncWait(fails());
    return EXIT_FAILURE;
}
