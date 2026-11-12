#include <atomic>
#include <cstdlib>

#include "co2/contract.hpp"
#include "co2/optional.hpp"

namespace {

std::atomic<bool> atExpectedFailurePoint{};

void contractViolationHandler(co2::ContractViolation const&) {
    std::_Exit(atExpectedFailurePoint.load(std::memory_order_acquire) ? 86 : 87);
}

} // namespace

// 空的 co2::optional 上取值是契约违规。std::optional 在这里抛 bad_optional_access；co2
// 不为前置条件抛异常，开不开异常行为相同。
int main() {
    co2::setContractViolationHandler(&contractViolationHandler);

    co2::optional<int> engaged = 1;
    if (engaged.value() != 1) return EXIT_FAILURE;

    auto const empty = co2::optional<int>{};
    atExpectedFailurePoint.store(true, std::memory_order_release);
    static_cast<void>(empty.value());
    return EXIT_FAILURE;
}
