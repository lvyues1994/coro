#include <atomic>
#include <cstdlib>
#include <system_error>

#include "co2/contract.hpp"
#include "co2/error.hpp"

namespace {

std::atomic<bool> atExpectedFailurePoint{};

void contractViolationHandler(co2::ContractViolation const&) {
    std::_Exit(atExpectedFailurePoint.load(std::memory_order_acquire) ? 86 : 87);
}

} // namespace

// "成功"不是一种错误：用值为零的 std::error_code 构造 co2::Error 是契约违规。
int main() {
    co2::setContractViolationHandler(&contractViolationHandler);

    co2::Error const failure = std::make_error_code(std::errc::timed_out);
    if (failure.code() != std::errc::timed_out) return EXIT_FAILURE;

    atExpectedFailurePoint.store(true, std::memory_order_release);
    co2::Error const success = std::error_code{};
    static_cast<void>(success);
    return EXIT_FAILURE;
}
