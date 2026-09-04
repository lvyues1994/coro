#include <atomic>
#include <cstdlib>

#include "co2/async_generator.hpp"
#include "co2/coroutine.hpp"
#include "co2/task.hpp"

namespace {

std::atomic<bool> atExpectedFailurePoint{};

void contractViolationHandler(co2::ContractViolation const&) {
    std::_Exit(atExpectedFailurePoint.load(std::memory_order_acquire) ? 86 : 87);
}

// 永不完成的操作：生产者挂在它上面时，消费者的 next() 悬而未决。
struct Parked {
    bool await_ready() const noexcept { return false; }
    void await_suspend(co2::coroutine_handle<>) const noexcept {}
    void await_resume() const noexcept {}
};

auto blockedProducer() CO2_BEG(co2::AsyncGenerator<int>, ()) {
    CO2_YIELD(1);
    CO2_AWAIT(Parked{});
    CO2_YIELD(2);
}
CO2_END

auto consumer(co2::AsyncGenerator<int>& stream)
    CO2_BEG(co2::Task<int>, (stream), bool has{};) {
    CO2_AWAIT_SET(has, stream.next());
    if (not has || stream.value() != 1) CO2_RETURN(-1);
    CO2_AWAIT_SET(has, stream.next()); // 生产者挂在 Parked 上：这个 next() 不会完成
    CO2_RETURN(stream.value());
}
CO2_END

} // namespace

// 有 next() 未完成时销毁 AsyncGenerator 是契约违规；停在 yield 点时可以销毁。
int main() {
    co2::setContractViolationHandler(&contractViolationHandler);
    {
        auto idle = blockedProducer();
        auto probe = consumer(idle);
        static_cast<void>(probe); // 未启动：生产者从未运行，可随时销毁
    }

    auto* const stream = new co2::AsyncGenerator<int>{blockedProducer()};
    auto* const waiter = new co2::Task<int>{consumer(*stream)};
    co2::detail::TaskAccess::start(*waiter, co2::noop_coroutine());
    if (co2::detail::TaskAccess::isDone(*waiter)) return EXIT_FAILURE;

    atExpectedFailurePoint.store(true, std::memory_order_release);
    delete stream;
    return EXIT_FAILURE;
}
