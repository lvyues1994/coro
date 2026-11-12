#include <atomic>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

#include "co2/as_result.hpp"
#include "co2/coroutine.hpp"
#include "co2/optional.hpp"
#include "co2/result.hpp"
#include "co2/sync_wait.hpp"
#include "co2/task.hpp"

namespace {

std::atomic<bool> atExpectedFailurePoint{};

// 预期的违规由 Task 的 await_resume() 报告（task.hpp）。
void contractViolationHandler(co2::ContractViolation const& violation) {
    auto const expected = atExpectedFailurePoint.load(std::memory_order_acquire) &&
                          std::strstr(violation.file, "task.hpp") != nullptr;
    std::_Exit(expected ? 86 : 87);
}

auto fails() CO2_BEG(co2::Task<int>, ()) {
    throw std::runtime_error{"boom"};
    CO2_RETURN(0);
}
CO2_END

auto awaitsTheSameTaskTwice()
    CO2_BEG(co2::Task<int>, (), co2::Task<int> child;
            co2::optional<co2::Result<int>> first; int second{};) {
    child = fails();
    CO2_AWAIT_SET(first, co2::asResult(child)); // 第一次：错误作为值被取走
    if (not first->hasError()) CO2_RETURN(-1);
    atExpectedFailurePoint.store(true, std::memory_order_release);
    CO2_AWAIT_SET(second, child); // 第二次：没有东西可给
    CO2_RETURN(second);
}
CO2_END

} // namespace

// 一个 Task 的结果只可取走一次，错误与值一样。第一次等待把错误取走之后，再等待同一个
// Task 是契约违规——而不是把一个已经被搬空的错误再交出去一次。
int main() {
    co2::setContractViolationHandler(&contractViolationHandler);
    try {
        co2::syncWait(awaitsTheSameTaskTwice());
    } catch (...) {
    }
    return EXIT_FAILURE;
}
