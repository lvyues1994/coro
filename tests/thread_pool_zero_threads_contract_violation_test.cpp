#include <atomic>
#include <cstdlib>

#include "co2/thread_pool.hpp"

namespace {

std::atomic<bool> atExpectedFailurePoint{};

void contractViolationHandler(co2::ContractViolation const&) {
    std::_Exit(atExpectedFailurePoint.load(std::memory_order_acquire) ? 86 : 87);
}

} // namespace

// 线程池至少要有一个工作线程。
int main() {
    co2::setContractViolationHandler(&contractViolationHandler);
    {
        co2::ThreadPool one{1U};
        if (one.threadCount() != 1U) return EXIT_FAILURE;
    }
    atExpectedFailurePoint.store(true, std::memory_order_release);
    co2::ThreadPool none{0U};
    return EXIT_FAILURE;
}
