#include <atomic>
#include <cstdlib>
#include <cstring>

#include "co2/completion.hpp"
#include "co2/coroutine.hpp"
#include "co2/sync_wait.hpp"
#include "co2/task.hpp"

namespace {

std::atomic<bool> atExpectedFailurePoint{};

// 预期的违规由 Task 的 await_resume() 报告（task.hpp）。别处报告的违规——例如"已停止"
// 根本到不了 Task 的 promise——不算数。
void contractViolationHandler(co2::ContractViolation const& violation) {
    auto const expected = atExpectedFailurePoint.load(std::memory_order_acquire) &&
                          std::strstr(violation.file, "task.hpp") != nullptr;
    std::_Exit(expected ? 86 : 87);
}

// 立刻就绪，以已停止结束。
struct Stops {
    bool await_ready() const noexcept { return true; }
    void await_suspend(co2::coroutine_handle<>) const noexcept {}
    void await_resume() const noexcept {}
    co2::Completion await_completion() noexcept { return co2::Completion::stopped(); }
};

auto stops(bool stop) CO2_BEG(co2::Task<int>, (stop)) {
    if (stop) CO2_AWAIT(Stops{});
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

auto awaitsThroughAWrapper(bool stop) CO2_BEG(co2::Task<int>, (stop), int value{};) {
    CO2_AWAIT_SET(value, Forwards{stops(stop)});
    CO2_RETURN(value);
}
CO2_END

} // namespace

// "已停止"只能经 await_completion() 传出来。不认识这个成员的调用方直接调用
// await_resume()，而一个已停止的 Task 没有值可以交给它：契约违规。包装 awaiter 需要把
// await_completion() 也转发出去。
int main() {
    co2::setContractViolationHandler(&contractViolationHandler);
    if (co2::syncWait(awaitsThroughAWrapper(false)) != 1) return EXIT_FAILURE;

    atExpectedFailurePoint.store(true, std::memory_order_release);
    co2::syncWait(awaitsThroughAWrapper(true));
    return EXIT_FAILURE;
}
