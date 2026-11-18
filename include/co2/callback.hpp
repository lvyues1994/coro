#pragma once

#include <atomic>
#include <exception>
#include <new>
#include <system_error>
#include <type_traits>
#include <utility>

#include "co2/completion.hpp"
#include "co2/contract.hpp"
#include "co2/coroutine_handle.hpp"
#include "co2/detail/move_only_function.hpp"
#include "co2/detail/uninitialized.hpp"
#include "co2/env.hpp"
#include "co2/error.hpp"
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
// initiate(continuation[, stop_token]) 发起操作；操作完成时 API 调用续体的四个出口
// 之一，**恰好一次**：
//
//   continuation(args...)             有值：就地构造 T
//   continuation.setError(code)       出错：一个 std::error_code
//   continuation.setException(eptr)   出错：一个捕获的异常（禁用异常的构建里没有）
//   continuation.setStopped()         已停止
//
// 后三种经 await_completion() 按值交给等待它的协程，那个协程就此结束（见
// completion-channels.md）；不认识这个成员的调用方直接调用 await_resume()，错误在那里
// 抛出。
//
// 完成协议是两方 fetch_or、第二个到达者负责恢复：续体在 initiate 返回之前就到了
// （同步完成或另一线程更快）→ await_suspend 返回 false，协程不挂起、内联继续，不长栈；
// 续体后到 → 协程已挂起，续体所在线程 resume() 它。所有状态都在 awaiter 里——awaiter
// 被移进帧的 awaiter 槽后地址稳定——没有堆分配（initiate 的闭包超过 3 个指针的内联
// 容量时除外）。initiate 以 detail::MoveOnlyFunction 保存：闭包可以捕获 unique_ptr 之类
// 的 move-only 对象，只要求它可移动。
//
// 布局：全部标志共用一个原子字。结果的三种形态——值、错误码、捕获的异常——互斥，共用
// 一块没有标志的存储，哪一种活着由原子字里的标志说明。CallbackAwaitable<T> =
// MoveOnlyFunction（4 指针）+ 句柄 + 结果 + 状态字；结果那一块至少是一个
// std::error_code（2 个指针），所以 T 不超过 2 个指针宽时整个 awaiter 正好 8 个指针，
// 放得进内联 awaiter 槽。错误拆成两种形态分别存，而不是存一个 co2::Error：在
// exception_ptr 是 2 个指针的平台上（MSVC），Error 有 3 个指针宽，会把 awaiter 顶出槽。
//
// 取消：initiate 可以接收当前协程的 stop_token，用 stop_callback 接到 API 自己的
// cancel，API 随后以"已取消"回调走正常完成路径，在那个回调里调用
// continuation.setStopped()。因此不需要为迟到的回调保活任何共享状态。
//
// 契约：续体调用恰好一次；initiate 抛出之后 API 不得再调用续体；awaiter 在 initiate
// 之后不再移动（核心保证）。协程在续体被调用的线程上恢复（标准语义），要换线程用
// scheduleOn。
//
// 关于"恰好一次"：第一次调用一旦完成，协程就可能（在本线程内联地或在另一线程上）恢复
// 并销毁这个 awaiter，续体里的裸指针随之悬空。因此第二次调用只有在 awaiter 仍然存活时
// 才会被诊断为契约违规（contract violation）；一般情况下它是未定义行为，库无法保证
// 报告。就地构造 T 抛出时，异常被记为错误并照常完成等待，协程不会因此挂住。

namespace co2 {

template <class T> struct CallbackAwaitable;

// 一次性续体：可拷贝的小句柄，交给回调式 API。
template <class T> struct Continuation {
    Continuation() noexcept = default;

    // 就地构造结果并完成等待。T 为 void 时不带参数。T 的构造抛出时记为错误：等待仍然
    // 完成，否则协程会永远挂在这个 await 上（续体已被 claim，不能再调）。
    template <class... Args> void operator()(Args&&... args) const {
        CO2_CONTRACT_CHECK(operation != nullptr);
        operation->claim();
        unsigned outcome = CallbackAwaitable<T>::HasValue;
#if CO2_HAS_EXCEPTIONS
        try {
            operation->storage.value.construct(std::forward<Args>(args)...);
        } catch (...) {
            operation->storeException(std::current_exception());
            outcome = CallbackAwaitable<T>::HasException;
        }
#else
        operation->storage.value.construct(std::forward<Args>(args)...);
#endif
        operation->complete(outcome);
    }

    // 以错误码完成等待。前置条件：code 表示失败（非零）。
    void setError(std::error_code const code) const {
        CO2_CONTRACT_CHECK(operation != nullptr && static_cast<bool>(code));
        operation->claim();
        ::new (static_cast<void*>(&operation->storage.code)) std::error_code{code};
        operation->complete(CallbackAwaitable<T>::HasCode);
    }

#if CO2_HAS_EXCEPTIONS
    // 以异常完成等待。前置条件：error 非空。
    void setException(std::exception_ptr error) const {
        CO2_CONTRACT_CHECK(operation != nullptr && error != nullptr);
        operation->claim();
        operation->storeException(std::move(error));
        operation->complete(CallbackAwaitable<T>::HasException);
    }
#endif

    // 以已停止完成等待：操作被取消了，没有结果。
    void setStopped() const {
        CO2_CONTRACT_CHECK(operation != nullptr);
        operation->claim();
        operation->complete(CallbackAwaitable<T>::Stopped);
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

    // 值一般在 await_resume 里被取走；留在这里的只有 take 时移动构造抛出的那一份。
    // error_code 平凡析构；捕获的异常被取走之后留下的 exception_ptr 仍要析构。
    ~CallbackAwaitable() {
        auto const flags = state.load(std::memory_order_relaxed);
        if ((flags & HasValue) != 0U) storage.value.destroy();
#if CO2_HAS_EXCEPTIONS
        if ((flags & HasException) != 0U) storage.exception.~exception_ptr();
#endif
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

    // 不认识 await_completion() 的调用方直接走到这里：错误在这里抛出；已停止没有值
    // 可给，是契约违规。
    T await_resume() {
        auto const flags = state.load(std::memory_order_relaxed);
        if ((flags & (HasCode | HasException)) != 0U) takeError(flags).raise();
        CO2_CONTRACT_CHECK((flags & HasValue) != 0U);
        return take(std::is_void<T>{});
    }

    // 这次等待是怎么结束的。出错与已停止由恢复点交给等待它的协程。
    Completion await_completion() noexcept {
        auto const flags = state.load(std::memory_order_relaxed);
        if ((flags & Stopped) != 0U) return Completion::stopped();
        if ((flags & (HasCode | HasException)) != 0U)
            return Completion::error(takeError(flags));
        return Completion::value();
    }

  private:
    // 一个原子字承载全部状态：
    //   Armed         await_suspend 已收尾，协程挂起中（由等待方置位）
    //   Completed     续体已经到达（由完成方置位）
    //   Claimed       续体已被调用过——"恰好一次"的诊断依据
    //   HasValue      storage.value 里有一个已构造的 T
    //   HasCode       storage.code 里有一个错误码
    //   HasException  storage.exception 里有一个捕获的异常
    //   Stopped       操作以已停止完成，storage 里什么也没有
    // 后四个是结果，互斥，与 Completed 一起、以 release 发布。
    enum : unsigned {
        Armed = 1U,
        Completed = 2U,
        Claimed = 4U,
        HasValue = 8U,
        HasCode = 16U,
        HasException = 32U,
        Stopped = 64U
    };

    // 结果的三种形态共用的存储，自己不带标记。
    union Storage {
        Storage() noexcept {}
        ~Storage() {}

        detail::Uninitialized<T> value;
        std::error_code code;
#if CO2_HAS_EXCEPTIONS
        std::exception_ptr exception;
#endif
    };

    // 续体只能调用一次。
    void claim() noexcept {
        CO2_CONTRACT_CHECK(
            (state.fetch_or(Claimed, std::memory_order_acq_rel) & Claimed) == 0U);
    }

    // 第二个到达者负责恢复：await_suspend 已经收尾（Armed）则协程正挂着，在这里恢复它。
    // outcome 是四个结果标志之一，与 Completed 一次写入，结果的发布和完成信号不可分。
    void complete(unsigned const outcome) {
        if ((state.fetch_or(Completed | outcome, std::memory_order_acq_rel) & Armed) !=
            0U)
            waiter.resume();
    }

    // 圆括号是有意的：泛型 T 上花括号可能选中 initializer_list 构造函数。移动构造抛出
    // 时对象保留、HasValue 不清，析构函数仍会清理。
    T take(std::false_type) {
        T result(std::move(storage.value.get()));
        dropValue();
        return result;
    }

    void take(std::true_type) noexcept { dropValue(); }

    void dropValue() noexcept {
        storage.value.destroy();
        state.fetch_and(~HasValue, std::memory_order_relaxed);
    }

#if CO2_HAS_EXCEPTIONS
    void storeException(std::exception_ptr error) noexcept {
        ::new (static_cast<void*>(&storage.exception))
            std::exception_ptr{std::move(error)};
    }
#endif

    // 把存着的错误交出去，只可一次。捕获的异常被移走，留下的空壳由析构函数清理。
    Error takeError(unsigned const flags) noexcept {
#if CO2_HAS_EXCEPTIONS
        if ((flags & HasException) != 0U) return Error{std::move(storage.exception)};
#else
        static_cast<void>(flags);
#endif
        return Error{storage.code};
    }

    Initiate initiate;
    coroutine_handle<> waiter;
    Storage storage;
    std::atomic<unsigned> state{0U};

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
