#pragma once

#include <atomic>
#include <exception>

namespace co2 {

// 一次契约违规的诊断信息。condition 是被违反的条件文本或说明。
struct ContractViolation {
    char const* condition;
    char const* file;
    unsigned line;
};

// 契约违规处理器。它可以记录日志、生成转储或直接结束进程；如果返回，库仍会调用
// std::terminate()——违规之后的状态不允许继续执行。
using ContractViolationHandler = void (*)(ContractViolation const& violation);

namespace detail {

inline std::atomic<ContractViolationHandler>& contractViolationHandlerSlot() noexcept {
    static std::atomic<ContractViolationHandler> handler{nullptr};
    return handler;
}

[[noreturn]] inline void reportContractViolation(char const* const condition,
                                                 char const* const file,
                                                 unsigned const line) noexcept {
    auto const handler = contractViolationHandlerSlot().load(std::memory_order_acquire);
    if (handler != nullptr) handler(ContractViolation{condition, file, line});
    std::terminate();
}

} // namespace detail

// 安装进程级契约违规处理器，返回之前的处理器。传入空指针恢复默认行为
// （直接 std::terminate()）。
inline ContractViolationHandler
setContractViolationHandler(ContractViolationHandler const handler) noexcept {
    return detail::contractViolationHandlerSlot().exchange(handler,
                                                           std::memory_order_acq_rel);
}

inline ContractViolationHandler contractViolationHandler() noexcept {
    return detail::contractViolationHandlerSlot().load(std::memory_order_acquire);
}

} // namespace co2

// 前置条件与不变量检查。违反契约不是可恢复错误：库不会为此抛异常，而是交给
// 契约违规处理器。所有用户可触达的前置条件都用 CO2_CONTRACT_CHECK，任何构建配置下
// 都保留。
#define CO2_CONTRACT_CHECK(condition)                                                  \
    (static_cast<bool>(condition)                                                      \
         ? static_cast<void>(0)                                                        \
         : ::co2::detail::reportContractViolation(#condition, __FILE__, __LINE__))

#define CO2_CONTRACT_FAIL(message)                                                     \
    ::co2::detail::reportContractViolation(message, __FILE__, __LINE__)

// 热路径上的内部不变量检查：它们只可能因库自身缺陷失败。NDEBUG 下编译掉，定义
// CO2_CONTRACT_CHECKS_ALWAYS 可以在发布构建中保留。
#if defined(NDEBUG) && !defined(CO2_CONTRACT_CHECKS_ALWAYS)
#define CO2_CONTRACT_ASSERT(condition) static_cast<void>(0)
#else
#define CO2_CONTRACT_ASSERT(condition) CO2_CONTRACT_CHECK(condition)
#endif
