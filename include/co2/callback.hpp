#pragma once

#include <atomic>
#include <exception>
#include <type_traits>
#include <utility>

#include "co2/contract.hpp"
#include "co2/coroutine_handle.hpp"
#include "co2/detail/move_only_function.hpp"
#include "co2/detail/uninitialized.hpp"
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
// 完成协议是两方 fetch_or、第二个到达者负责恢复：续体在 initiate 返回之前就到了
// （同步完成或另一线程更快）→ await_suspend 返回 false，协程不挂起、内联继续，不长栈；
// 续体后到 → 协程已挂起，续体所在线程 resume() 它。所有状态都在 awaiter 里——awaiter
// 被移进帧的 awaiter 槽后地址稳定——没有堆分配（initiate 的闭包超过 3 个指针的内联
// 容量时除外）。initiate 以 detail::MoveOnlyFunction 保存：闭包可以捕获 unique_ptr 之类
// 的 move-only 对象，只要求它可移动。
//
// 布局：Armed / Completed / Claimed / HasValue 四个标志共用一个原子字，结果放在无标志的
// Uninitialized<T> 里。CallbackAwaitable<T> = MoveOnlyFunction（4 指针）+ 句柄 + T +
// 状态字 + exception_ptr；T 不超过 4 字节时，即使 exception_ptr 是 2 个指针（MSVC），
// 也放得进 8 指针的内联 awaiter 槽。
//
// 取消：initiate 可以接收当前协程的 stop_token，用 stop_callback 接到 API 自己的
// cancel， API
// 随后以"已取消"回调走正常完成路径。因此不需要为迟到的回调保活任何共享状态。
//
// 契约：续体调用恰好一次；initiate 抛出之后 API 不得再调用续体；awaiter 在 initiate
// 之后不再移动（核心保证）。协程在续体被调用的线程上恢复（标准语义），要换线程用
// scheduleOn。
//
// 关于"恰好一次"：第一次调用一旦完成，协程就可能（在本线程内联地或在另一线程上）恢复
// 并销毁这个 awaiter，续体里的裸指针随之悬空。因此第二次调用只有在 awaiter 仍然存活时
// 才会被诊断为契约违规（contract violation）；一般情况下它是未定义行为，库无法保证
// 报告。就地构造 T 抛出时，异常会被存进异常通道并照常完成等待——await_resume 重抛它，
// 协程不会因此挂住。

namespace co2 {

template <class T> struct CallbackAwaitable;

// 一次性续体：可拷贝的小句柄，交给回调式 API。
template <class T> struct Continuation {
    Continuation() noexcept = default;

    // 就地构造结果并完成等待。T 为 void 时不带参数。T 的构造抛出时改走异常通道：
    // 等待仍然完成，否则协程会永远挂在这个 await 上（续体已被 claim，不能再调）。
    template <class... Args> void operator()(Args&&... args) const {
        CO2_CONTRACT_CHECK(operation != nullptr);
        operation->claim();
        unsigned outcome = CallbackAwaitable<T>::HasValue;
        try {
            operation->value.construct(std::forward<Args>(args)...);
        } catch (...) {
            operation->error = std::current_exception();
            outcome = 0U;
        }
        operation->complete(outcome);
    }

    // 以异常完成等待：await_resume 重抛它。
    void setException(std::exception_ptr error) const {
        CO2_CONTRACT_CHECK(operation != nullptr && error != nullptr);
        operation->claim();
        operation->error = std::move(error);
        operation->complete(0U);
    }

    explicit operator bool() const noexcept { return operation != nullptr; }

  private:
    explicit Continuation(CallbackAwaitable<T>* const operation_) noexcept
        : operation{operation_} {}

    CallbackAwaitable<T>* operation{};

    friend struct CallbackAwaitable<T>;
};

template <class T> struct CallbackAwaitable {
    using Initiate = detail::MoveOnlyFunction<void(Continuation<T>, stop_token)>;

    explicit CallbackAwaitable(Initiate initiate_) noexcept
        : initiate{std::move(initiate_)} {
        CO2_CONTRACT_CHECK(static_cast<bool>(initiate));
    }

    // 只允许在启动前移动（awaiter 被移进等待者的帧）。noexcept 依赖 Initiate 的移动
    // noexcept——它对内联闭包要求 nothrow 移动，否则把闭包放到堆上。
    CallbackAwaitable(CallbackAwaitable&& other) noexcept
        : initiate{std::move(other.initiate)} {
        CO2_CONTRACT_CHECK(other.state.load(std::memory_order_relaxed) == 0U);
    }

    CallbackAwaitable(CallbackAwaitable const&) = delete;
    CallbackAwaitable& operator=(CallbackAwaitable const&) = delete;
    CallbackAwaitable& operator=(CallbackAwaitable&&) = delete;

    // 结果一般在 await_resume 里被取走；留在这里的只有 take 时移动构造抛出的那一份。
    ~CallbackAwaitable() {
        if (hasValue()) value.destroy();
    }

    bool await_ready() const noexcept { return false; }

    // initiate 抛出时异常按标准落进协程体（等待者被视为已恢复）。
    template <class Parent>
    bool await_suspend(coroutine_handle<Parent> const awaiting) {
        waiter = awaiting;
        initiate(Continuation<T>{this}, detail::stopTokenOf(awaiting));
        // 续体已经到了：不挂起，内联继续。acquire 与 complete() 的 release 配对，让
        // 结果与 HasValue 对本线程可见。
        return (state.fetch_or(Armed, std::memory_order_acq_rel) & Completed) == 0U;
    }

    T await_resume() {
        if (error) std::rethrow_exception(error);
        CO2_CONTRACT_CHECK(hasValue());
        return take(std::is_void<T>{});
    }

  private:
    // 一个原子字承载全部状态：
    //   Armed      await_suspend 已收尾，协程挂起中（由等待方置位）
    //   Completed  续体已经到达（由完成方置位）
    //   Claimed    续体已被调用过——"恰好一次"的诊断依据
    //   HasValue   value 里有一个已构造的 T（与 Completed 一起、以 release 发布）
    enum : unsigned { Armed = 1U, Completed = 2U, Claimed = 4U, HasValue = 8U };

    // 续体只能调用一次。
    void claim() noexcept {
        CO2_CONTRACT_CHECK(
            (state.fetch_or(Claimed, std::memory_order_acq_rel) & Claimed) == 0U);
    }

    // 第二个到达者负责恢复：await_suspend 已经收尾（Armed）则协程正挂着，在这里恢复它。
    // outcome 是 HasValue 或 0，与 Completed 一次写入，结果的发布和完成信号不可分。
    void complete(unsigned const outcome) {
        if ((state.fetch_or(Completed | outcome, std::memory_order_acq_rel) & Armed) !=
            0U)
            waiter.resume();
    }

    bool hasValue() const noexcept {
        return (state.load(std::memory_order_relaxed) & HasValue) != 0U;
    }

    // 圆括号是有意的：泛型 T 上花括号可能选中 initializer_list 构造函数。移动构造抛出
    // 时对象保留、HasValue 不清，析构函数仍会清理。
    T take(std::false_type) {
        T result(std::move(value.get()));
        dropValue();
        return result;
    }

    void take(std::true_type) noexcept { dropValue(); }

    void dropValue() noexcept {
        value.destroy();
        state.fetch_and(~HasValue, std::memory_order_relaxed);
    }

    Initiate initiate;
    coroutine_handle<> waiter;
    detail::Uninitialized<T> value;
    std::atomic<unsigned> state{0U};
    std::exception_ptr error;

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
