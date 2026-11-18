#include <atomic>
#include <cstdlib>
#include <cstring>
#include <system_error>

#include "co2/completion.hpp"
#include "co2/config.hpp"
#include "co2/coroutine.hpp"

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

// 立刻就绪，以给定的方式结束。
struct Ends {
    co2::Completion completion;

    bool await_ready() const noexcept { return true; }
    void await_suspend(co2::coroutine_handle<>) const noexcept {}
    void await_resume() const noexcept {}
    co2::Completion await_completion() noexcept { return completion; }
};

// 为标准写的 promise：没有 return_error()。禁用异常的构建里它也不需要
// unhandled_exception()。
struct Plain {
    struct promise_type {
        promise_type() = default;

        Plain get_return_object() noexcept { return {}; }
        co2::suspend_never initial_suspend() noexcept { return {}; }
        co2::suspend_never final_suspend() noexcept { return {}; }
        void return_void() noexcept {}
    };
};

auto endsWith(co2::Completion completion) CO2_BEG(Plain, (completion)) {
    CO2_AWAIT(Ends{completion});
}
CO2_END

} // namespace

// 开异常时，错误被送到一个没有 return_error() 的 promise，会在协程体内重新抛出，走
// unhandled_exception()。禁用异常的构建里没有这条退路：契约违规。
int main() {
    co2::setContractViolationHandler(&contractViolationHandler);
    endsWith(co2::Completion::value());

    atExpectedFailurePoint.store(true, std::memory_order_release);
    endsWith(co2::Completion::error(std::make_error_code(std::errc::timed_out)));
    return EXIT_FAILURE;
}
