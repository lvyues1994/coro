#pragma once

#include <atomic>
#include <exception>
#include <functional>
#include <type_traits>
#include <utility>

#include "co2/contract.hpp"
#include "co2/coroutine_handle.hpp"
#include "co2/detail/result_storage.hpp"
#include "co2/env.hpp"
#include "co2/stop_token.hpp"

// co2：把回调式异步 API 变成 awaiter。
//
//   auto asyncRead(Socket& s, Buffer b) CO2_BEG(co2::Task<int>, (s, b), int n{};) {
//       CO2_AWAIT_AS_SET(n, co2::CallbackAwaitable<int>,
//                        co2::fromCallback<int>([&](co2::Continuation<int> done) {
//                            s.asyncRead(b, [done](int bytes) mutable { done(bytes);
//                            });
//                        }));
//       CO2_RETURN(n);
//   }
//   CO2_END
//
// initiate(continuation[, stop_token]) 发起操作；操作完成时 API 调用
// continuation(args...) （就地构造 T）或
// continuation.setException(eptr)，**恰好一次**。
//
// 完成协议是两方 exchange、第二个到达者负责恢复：续体在 initiate 返回之前就到了
// （同步完成或另一线程更快）→ await_suspend 返回 false，协程不挂起、内联继续，不长栈；
// 续体后到 → 协程已挂起，续体所在线程 resume() 它。所有状态都在 awaiter 里——awaiter
// 被移进帧的 awaiter 槽后地址稳定——没有堆分配（initiate 的闭包超过 std::function 的
// 内联容量时除外）。
//
// 取消：initiate 可以接收当前协程的 stop_token，用 stop_callback 接到 API 自己的
// cancel， API
// 随后以"已取消"回调走正常完成路径。因此不需要为迟到的回调保活任何共享状态。
//
// 契约：续体调用恰好一次（第二次是契约违规）；initiate 抛出之后 API 不得再调用续体；
// awaiter 在 initiate 之后不再移动（核心保证）。协程在续体被调用的线程上恢复（标准
// 语义），要换线程用 scheduleOn。

namespace co2 {

template <class T> struct CallbackAwaitable;

// 一次性续体：可拷贝的小句柄，交给回调式 API。
template <class T> struct Continuation {
    Continuation() noexcept = default;

    // 就地构造结果并完成等待。T 为 void 时不带参数。
    template <class... Args> void operator()(Args&&... args) const {
        CO2_CONTRACT_CHECK(operation != nullptr);
        operation->claim();
        operation->value.emplace(std::forward<Args>(args)...);
        operation->complete();
    }

    // 以异常完成等待：await_resume 重抛它。
    void setException(std::exception_ptr error) const {
        CO2_CONTRACT_CHECK(operation != nullptr && error != nullptr);
        operation->claim();
        operation->error = std::move(error);
        operation->complete();
    }

    explicit operator bool() const noexcept { return operation != nullptr; }

  private:
    explicit Continuation(CallbackAwaitable<T>* const operation_) noexcept
        : operation{operation_} {}

    CallbackAwaitable<T>* operation{};

    friend struct CallbackAwaitable<T>;
};

template <class T> struct CallbackAwaitable {
    using Initiate = std::function<void(Continuation<T>, stop_token)>;

    explicit CallbackAwaitable(Initiate initiate_) : initiate{std::move(initiate_)} {
        CO2_CONTRACT_CHECK(static_cast<bool>(initiate));
    }

    // 只允许在启动前移动（awaiter 被移进等待者的帧）。
    CallbackAwaitable(CallbackAwaitable&& other) noexcept
        : initiate{std::move(other.initiate)} {
        CO2_CONTRACT_CHECK(other.state.load(std::memory_order_relaxed) == Initial);
    }

    CallbackAwaitable(CallbackAwaitable const&) = delete;
    CallbackAwaitable& operator=(CallbackAwaitable const&) = delete;
    CallbackAwaitable& operator=(CallbackAwaitable&&) = delete;

    bool await_ready() const noexcept { return false; }

    // initiate 抛出时异常按标准落进协程体（等待者被视为已恢复）。
    template <class Parent>
    bool await_suspend(coroutine_handle<Parent> const awaiting) {
        waiter = awaiting;
        initiate(Continuation<T>{this}, detail::stopTokenOf(awaiting));
        // 续体已经到了：不挂起，内联继续。
        return state.exchange(Armed, std::memory_order_acq_rel) != Completed;
    }

    T await_resume() {
        if (error) std::rethrow_exception(error);
        CO2_CONTRACT_CHECK(value.hasValue());
        return value.take();
    }

  private:
    enum : unsigned { Initial, Armed, Completed };

    // 续体只能调用一次。
    void claim() noexcept {
        CO2_CONTRACT_CHECK(not claimed.exchange(true, std::memory_order_acq_rel));
    }

    // 第二个到达者负责恢复：await_suspend 已经收尾（Armed）则协程正挂着，在这里恢复它。
    void complete() {
        if (state.exchange(Completed, std::memory_order_acq_rel) == Armed)
            waiter.resume();
    }

    Initiate initiate;
    coroutine_handle<> waiter;
    detail::ResultStorage<T> value;
    std::exception_ptr error;
    std::atomic<unsigned> state{Initial};
    std::atomic<bool> claimed{false};

    friend struct Continuation<T>;
};

namespace detail {

template <class T, class F>
auto acceptsStopToken(int)
    -> decltype(std::declval<F&>()(std::declval<Continuation<T>>(),
                                   std::declval<stop_token>()),
                std::true_type{});

template <class, class> std::false_type acceptsStopToken(long);

template <class T, class F>
typename CallbackAwaitable<T>::Initiate wrapInitiate(F&& initiate, std::true_type) {
    return typename CallbackAwaitable<T>::Initiate{std::forward<F>(initiate)};
}

template <class T, class F> struct IgnoreStopToken {
    typename std::decay<F>::type initiate;
    void operator()(Continuation<T> continuation, stop_token) {
        initiate(std::move(continuation));
    }
};

template <class T, class F>
typename CallbackAwaitable<T>::Initiate wrapInitiate(F&& initiate, std::false_type) {
    return typename CallbackAwaitable<T>::Initiate{
        IgnoreStopToken<T, F>{std::forward<F>(initiate)}};
}

} // namespace detail

// initiate 的两种签名：initiate(continuation) 或 initiate(continuation, stop_token)。
template <class T, class F> CallbackAwaitable<T> fromCallback(F&& initiate) {
    return CallbackAwaitable<T>{detail::wrapInitiate<T>(
        std::forward<F>(initiate), decltype(detail::acceptsStopToken<T, F>(0)){})};
}

} // namespace co2
