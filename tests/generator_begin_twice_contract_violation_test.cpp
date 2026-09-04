#include <atomic>
#include <cstdlib>

#include "co2/coroutine.hpp"
#include "co2/generator.hpp"

namespace {

std::atomic<bool> atExpectedFailurePoint{};

void contractViolationHandler(co2::ContractViolation const&) {
    std::_Exit(atExpectedFailurePoint.load(std::memory_order_acquire) ? 86 : 87);
}

auto counting() CO2_BEG(co2::Generator<int>, ()) {
    CO2_YIELD(1);
    CO2_YIELD(2);
}
CO2_END

} // namespace

// 与 std::generator 一致：begin() 只能调用一次。
int main() {
    co2::setContractViolationHandler(&contractViolationHandler);
    auto generator = counting();
    auto it = generator.begin();
    if (*it != 1) return EXIT_FAILURE;

    atExpectedFailurePoint.store(true, std::memory_order_release);
    static_cast<void>(generator.begin());
    return EXIT_FAILURE;
}
