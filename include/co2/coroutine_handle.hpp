#pragma once

#include <cstddef>
#include <type_traits>

#include "co2/contract.hpp"

// co2 核心：与 C++20 [coroutine.handle] 同形的非拥有句柄。
//
// 帧头是所有协程帧的标准布局前缀（见 detail/frame.hpp）。句柄只保存指向帧头的指针；
// resume()/destroy() 通过帧头里的函数指针分派，promise() 与 from_promise() 通过
// 标准布局前缀的固定偏移计算，不依赖任何 ABI 假设。

namespace co2 {

template <class Promise = void> struct coroutine_handle;

namespace detail {

// 协程帧头。`step` 运行到下一个挂起点并返回对称转移目标（可能为空）；`destroy`
// 销毁协程状态。`done` 表示停在 final suspend point；`running` 只用于契约诊断：
// 恢复正在运行的协程、销毁正在运行的协程都是标准意义上的未定义行为，这里报告为
// 契约违规。
struct FrameHeader {
    void (*destroy)(FrameHeader*) noexcept;
    FrameHeader* (*step)(FrameHeader*);
    bool done;
    bool running;
};

struct HandleAccess;

} // namespace detail

template <> struct coroutine_handle<void> {
    constexpr coroutine_handle() noexcept = default;
    constexpr coroutine_handle(std::nullptr_t) noexcept {}

    coroutine_handle& operator=(std::nullptr_t) noexcept {
        frame = nullptr;
        return *this;
    }

    void* address() const noexcept { return frame; }

    static coroutine_handle from_address(void* const address) noexcept {
        auto handle = coroutine_handle{};
        handle.frame = static_cast<detail::FrameHeader*>(address);
        return handle;
    }

    explicit operator bool() const noexcept { return frame != nullptr; }

    // 前置条件：句柄非空。
    bool done() const noexcept {
        CO2_CONTRACT_CHECK(frame != nullptr);
        return frame->done;
    }

    void operator()() const { resume(); }

    // 前置条件：协程挂起且未 done。对称转移在这里以迭代实现：await_suspend 返回的
    // 目标由同一个循环继续驱动，栈深不随转移链增长（标准靠尾调用达到同样效果）。
    void resume() const {
        CO2_CONTRACT_CHECK(frame != nullptr && not frame->done && not frame->running);
        auto* current = frame;
        while (current != nullptr)
            current = current->step(current);
    }

    // 前置条件：协程挂起（含停在 final suspend point）。
    void destroy() const noexcept {
        CO2_CONTRACT_CHECK(frame != nullptr && not frame->running);
        frame->destroy(frame);
    }

    friend bool operator==(coroutine_handle const left,
                           coroutine_handle const right) noexcept {
        return left.frame == right.frame;
    }

    friend bool operator!=(coroutine_handle const left,
                           coroutine_handle const right) noexcept {
        return not(left == right);
    }

  protected:
    detail::FrameHeader* frame{};

    friend struct detail::HandleAccess;
};

namespace detail {

// 帧的标准布局前缀：帧头在偏移 0，promise 存储紧随其后。CoroutineFrame 把它作为
// 第一个成员，因此 FrameHeader*、FramePrefix* 与帧对象地址三者相同。
template <class Promise> struct FramePrefix {
    FrameHeader header;
    alignas(Promise) unsigned char promise[sizeof(Promise)];
};

template <class Promise> Promise& promiseOf(FrameHeader* const header) noexcept {
    static_assert(std::is_standard_layout<FramePrefix<Promise>>::value,
                  "FramePrefix must stay standard-layout");
    auto* const prefix = reinterpret_cast<FramePrefix<Promise>*>(header);
    return *reinterpret_cast<Promise*>(prefix->promise);
}

template <class Promise> FrameHeader* headerOfPromise(Promise& promise) noexcept {
    auto* const bytes = reinterpret_cast<unsigned char*>(&promise) -
                        offsetof(FramePrefix<Promise>, promise);
    return reinterpret_cast<FrameHeader*>(bytes);
}

struct HandleAccess {
    static FrameHeader* header(coroutine_handle<> const handle) noexcept {
        return handle.frame;
    }
};

} // namespace detail

template <class Promise> struct coroutine_handle : coroutine_handle<void> {
    constexpr coroutine_handle() noexcept = default;
    constexpr coroutine_handle(std::nullptr_t) noexcept {}

    coroutine_handle& operator=(std::nullptr_t) noexcept {
        this->frame = nullptr;
        return *this;
    }

    static coroutine_handle from_address(void* const address) noexcept {
        auto handle = coroutine_handle{};
        handle.frame = static_cast<detail::FrameHeader*>(address);
        return handle;
    }

    static coroutine_handle from_promise(Promise& promise) noexcept {
        return from_address(detail::headerOfPromise(promise));
    }

    Promise& promise() const noexcept {
        CO2_CONTRACT_CHECK(this->frame != nullptr);
        return detail::promiseOf<Promise>(this->frame);
    }
};

struct suspend_always {
    bool await_ready() const noexcept { return false; }
    void await_suspend(coroutine_handle<>) const noexcept {}
    void await_resume() const noexcept {}
};

struct suspend_never {
    bool await_ready() const noexcept { return true; }
    void await_suspend(coroutine_handle<>) const noexcept {}
    void await_resume() const noexcept {}
};

namespace detail {

inline void noopDestroy(FrameHeader*) noexcept {}
inline FrameHeader* noopStep(FrameHeader*) { return nullptr; }

} // namespace detail

// resume() 无操作、destroy() 无操作、done() 恒假。await_suspend 返回它等价于
// "挂起并把控制交回 resumer"。
inline coroutine_handle<> noop_coroutine() noexcept {
    static detail::FrameHeader noop{&detail::noopDestroy, &detail::noopStep, false,
                                    false};
    return coroutine_handle<>::from_address(&noop);
}

// 只能按返回类型解析 promise（宏拿不到完整参数类型列表）。默认取
// Return::promise_type，允许特化。
template <class Return, class = void> struct coroutine_traits {
    using promise_type = typename Return::promise_type;
};

} // namespace co2
