#include <atomic>
#include <cstdlib>

#include "co2/callback.hpp"
#include "co2/coroutine.hpp"
#include "co2/sync_wait.hpp"
#include "co2/task.hpp"

namespace {

std::atomic<bool> atExpectedFailurePoint{};

void contractViolationHandler(co2::ContractViolation const&) {
    std::_Exit(atExpectedFailurePoint.load(std::memory_order_acquire) ? 86 : 87);
}

auto once() CO2_BEG(co2::Task<int>, (), int got{};) {
    CO2_AWAIT_AS_SET(
        got, co2::CallbackAwaitable<int>,
        co2::fromCallback<int>([](co2::Continuation<int> done) { done(1); }));
    CO2_RETURN(got);
}
CO2_END

auto twice() CO2_BEG(co2::Task<int>, (), int got{};) {
    CO2_AWAIT_AS_SET(got, co2::CallbackAwaitable<int>,
                     co2::fromCallback<int>([](co2::Continuation<int> done) {
                         done(1);
                         atExpectedFailurePoint.store(true, std::memory_order_release);
                         done(2); // 续体只能调用一次
                     }));
    CO2_RETURN(got);
}
CO2_END

} // namespace

int main() {
    co2::setContractViolationHandler(&contractViolationHandler);
    if (co2::syncWait(once()) != 1) return EXIT_FAILURE;
    static_cast<void>(co2::syncWait(twice()));
    return EXIT_FAILURE;
}
