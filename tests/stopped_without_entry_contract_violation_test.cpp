#include <atomic>
#include <cstdlib>

#include "co2/completion.hpp"
#include "co2/coroutine.hpp"

namespace {

std::atomic<bool> atExpectedFailurePoint{};

void contractViolationHandler(co2::ContractViolation const&) {
    std::_Exit(atExpectedFailurePoint.load(std::memory_order_acquire) ? 86 : 87);
}

// 立刻就绪，以给定的方式结束。
struct Ends {
    co2::Completion completion;

    bool await_ready() const noexcept { return true; }
    void await_suspend(co2::coroutine_handle<>) const noexcept {}
    void await_resume() const noexcept {}
    co2::Completion await_completion() noexcept { return completion; }
};

// 为标准写的 promise：没有 return_stopped()。
struct Plain {
    struct promise_type {
        promise_type() = default;

        Plain get_return_object() noexcept { return {}; }
        co2::suspend_never initial_suspend() noexcept { return {}; }
        co2::suspend_never final_suspend() noexcept { return {}; }
        void return_void() noexcept {}
        void unhandled_exception() noexcept {}
    };
};

auto endsWith(co2::Completion completion) CO2_BEG(Plain, (completion)) {
    CO2_AWAIT(Ends{completion});
}
CO2_END

} // namespace

// "已停止"被送到一个没有 return_stopped() 的 promise：它无处可去——既不是值，也不是
// 可以抛进协程体的错误——所以是契约违规。有值的等待在同一个 promise 里照常工作。
int main() {
    co2::setContractViolationHandler(&contractViolationHandler);
    endsWith(co2::Completion::value());

    atExpectedFailurePoint.store(true, std::memory_order_release);
    endsWith(co2::Completion::stopped());
    return EXIT_FAILURE;
}
