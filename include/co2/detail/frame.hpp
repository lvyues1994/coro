#pragma once

#include <cstddef>
#include <exception>
#include <memory>
#include <new>
#include <tuple>
#include <type_traits>
#include <utility>

#include "co2/contract.hpp"
#include "co2/coroutine_handle.hpp"
#include "co2/detail/await_slot.hpp"
#include "co2/detail/awaitable.hpp"

// 帧的恢复/收尾路径把状态机（Body::operator()）内联进来后，MSVC 会把 co_return 之后的
// 收尾判成不可达并报到本文件的行号上；C4702 以函数开括号处的状态为准，见 config.hpp。
CO2_DETAIL_MSVC_WARNING_PUSH_DISABLE(4702)

// co2 核心：协程帧与 ramp。执行 [dcl.fct.def.coroutine] 规定的协程体变换：
//
//   promise-type promise promise-constructor-arguments;
//   try { co_await promise.initial_suspend(); function-body }
//   catch (...) { if (!initial-await-resume-called) throw;
//   promise.unhandled_exception(); } final-suspend: co_await promise.final_suspend();
//
// 帧是一次分配的标准布局对象：{ header, promise } 前缀、参数副本（Params）、局部与
// 程序计数器（Body）、awaiter 槽、allocator。构造顺序 参数副本 → promise → 局部，
// 销毁顺序 awaiter → 局部 → promise → 参数副本，与标准一致。核心不含任何同步。

namespace co2 {
namespace detail {

// 帧中与 allocator 无关的部分。Body 的 operator() 以它为上下文，因此每个协程体只
// 实例化一份 step 逻辑，不随 allocator 变化。
template <class Promise, class Params, class Body> struct FrameCore {
    using InitialAwaiter =
        AwaiterOf<decltype(std::declval<Promise&>().initial_suspend())>;
    using FinalAwaiter = AwaiterOf<decltype(std::declval<Promise&>().final_suspend())>;

    // ---- 布局：prefix 必须是第一个成员 ----
    FramePrefix<Promise> prefix;
    alignas(Params) unsigned char paramsStorage[sizeof(Params)];
    alignas(Body) unsigned char bodyStorage[sizeof(Body)];
    AwaitSlot<> slot;
    unsigned suspendPoint;
    bool initialAwaitResumed;
    // 局部（Body）在离开协程体作用域时就销毁——co_return、掉出末尾或异常逃出——
    // 早于 final_suspend，与标准一致；destroy() 只销毁仍然存活的那一份。
    bool bodyAlive;

    // ---- Body 侧的访问 ----

    Promise& promise() noexcept { return *reinterpret_cast<Promise*>(prefix.promise); }
    Params& params() noexcept { return *reinterpret_cast<Params*>(paramsStorage); }
    Body& body() noexcept { return *reinterpret_cast<Body*>(bodyStorage); }

    coroutine_handle<Promise> handle() noexcept {
        return coroutine_handle<Promise>::from_address(&prefix.header);
    }

    template <class Awaitable>
    auto transform(Awaitable&& awaitable)
        -> decltype(transformAwaitable(std::declval<Promise&>(),
                                       std::forward<Awaitable>(awaitable))) {
        return transformAwaitable(promise(), std::forward<Awaitable>(awaitable));
    }

    template <class Awaiter, class Value> Awaiter& emplaceAwaiter(Value&& value) {
        return slot.template emplace<Awaiter>(std::forward<Value>(value));
    }

    template <class Awaiter> Awaiter& awaiter() noexcept {
        return slot.template get<Awaiter>();
    }

    void resetAwaiter() noexcept { slot.reset(); }

    // 一经调用 await_suspend，协程即视为挂起；另一线程可以在 await_suspend 返回前
    // 就 resume()。因此生成的代码在调用 await_suspend 之前就清掉 running。
    void markSuspended() noexcept { prefix.header.running = false; }
    void markRunning() noexcept { prefix.header.running = true; }

    // 初始挂起点之后的第一条语句：initial awaiter 的 await_resume()。它抛出时仍属于
    // "初始挂起点之前"，step 会销毁帧并把异常抛给恢复方。
    void finishInitialSuspend() {
        awaiter<InitialAwaiter>().await_resume();
        resetAwaiter();
        initialAwaitResumed = true;
    }

    // 掉出协程末尾：有 return_void 就调用它；没有时标准是未定义行为，这里报告契约违规。
    void returnVoidAtEnd() { returnVoidAtEnd(0); }

    // 离开协程体作用域：销毁局部。Body 正在执行自己的成员函数时也可以调用，只要之后
    // 不再访问它的成员。
    void destroyBody() noexcept {
        if (not bodyAlive) return;
        bodyAlive = false;
        body().~Body();
    }

    // co_await promise.final_suspend()。挂起时 done 置真、帧保留到 owner destroy()；
    // 不挂起时控制流掉出协程末尾，协程状态被销毁，返回后不得再访问帧。
    FrameHeader* enterFinalSuspend() noexcept {
        destroyBody();
        auto& awaiter =
            slot.template emplace<FinalAwaiter>(getAwaiter(promise().final_suspend()));
        if (awaiter.await_ready()) {
            destroySelf();
            return nullptr;
        }
        prefix.header.done = true;
        markSuspended();
        auto const outcome = suspendWith(awaiter, handle());
        if (not outcome.suspended) {
            prefix.header.done = false;
            destroySelf();
            return nullptr;
        }
        return outcome.next;
    }

  private:
    template <class P = Promise>
    auto returnVoidAtEnd(int) -> decltype(std::declval<P&>().return_void()) {
        return promise().return_void();
    }

    void returnVoidAtEnd(long) {
        CO2_CONTRACT_FAIL("co2 coroutine flowed off the end without return_void()");
    }

    void destroySelf() noexcept { prefix.header.destroy(&prefix.header); }
};

// 宏生成的 Body 的空基类：让成员初始化列表在没有参数时也有合法的首项。
struct BodyBase {};

// ---- promise 构造：能用参数副本构造就传入，否则默认构造 ----

// 宏生成的参数列表以它收尾，这样 0 个参数时也没有悬空逗号。
struct NoMoreParams {};

template <class Promise, class... Params>
void constructPromiseAt(void* const at, std::true_type, Params&... params) {
    ::new (at) Promise(params...);
}

template <class Promise, class... Params>
void constructPromiseAt(void* const at, std::false_type, Params&...) {
    ::new (at) Promise();
}

template <class Promise, class Tuple, std::size_t... Index>
void constructPromiseFromTuple(void* const at, Tuple& params,
                               std::index_sequence<Index...>) {
    constructPromiseAt<Promise>(
        at, std::is_constructible<Promise, decltype(std::get<Index>(params))...>{},
        std::get<Index>(params)...);
}

// 末尾实参是 NoMoreParams；其余是参数副本的左值。
template <class Promise, class... Args>
void constructPromise(void* const at, Args&&... args) {
    auto params = std::forward_as_tuple(args...);
    constructPromiseFromTuple<Promise>(
        at, params, std::make_index_sequence<sizeof...(Args) - 1U>{});
}

// 宏生成的参数副本类型提供 _co2_construct_promise(void*)（局部类不能有成员模板，
// promise 类型在生成时已知）；手写的（测试）没有时默认构造 promise。
template <class Promise, class Params>
auto constructPromiseFor(Params& params, void* const at, int)
    -> decltype(params._co2_construct_promise(at)) {
    return params._co2_construct_promise(at);
}

template <class Promise, class Params>
void constructPromiseFor(Params&, void* const at, long) {
    ::new (at) Promise();
}

// ---- 分配：promise 的 operator new / operator delete /
// get_return_object_on_allocation_failure ----

template <class Promise> struct HasPromiseOperatorNew {
    template <class P>
    static auto probe(int)
        -> decltype(P::operator new (std::size_t{}), std::true_type{});
    template <class> static std::false_type probe(long);
    static constexpr bool value = decltype(probe<Promise>(0))::value;
};

template <class Promise>
auto promiseAllocate(std::size_t const size, int)
    -> decltype(Promise::operator new(size)) {
    return Promise::operator new(size);
}

template <class Promise>
auto promiseDeallocate(void* const pointer, std::size_t const size, Rank<2>)
    -> decltype(Promise::operator delete(pointer, size)) {
    return Promise::operator delete(pointer, size);
}

template <class Promise>
auto promiseDeallocate(void* const pointer, std::size_t, Rank<1>)
    -> decltype(Promise::operator delete(pointer)) {
    return Promise::operator delete(pointer);
}

template <class Promise>
void promiseDeallocate(void* const pointer, std::size_t, Rank<0>) noexcept {
    ::operator delete(pointer); // promise 只提供 operator new 时回落到全局 delete
}

template <class Promise> struct HasAllocationFailureObject {
    template <class P>
    static auto probe(int)
        -> decltype(P::get_return_object_on_allocation_failure(), std::true_type{});
    template <class> static std::false_type probe(long);
    static constexpr bool value = decltype(probe<Promise>(0))::value;
};

// ---- 完整的帧 ----

template <class Promise, class Params, class Body, class Allocator>
struct CoroutineFrame {
    using Core = FrameCore<Promise, Params, Body>;
    using FrameAllocator = typename std::allocator_traits<
        Allocator>::template rebind_alloc<CoroutineFrame>;
    using AllocatorTraits = std::allocator_traits<FrameAllocator>;
    using UsesPromiseOperatorNew =
        std::integral_constant<bool, HasPromiseOperatorNew<Promise>::value>;
    using HasFailureObject =
        std::integral_constant<bool, HasAllocationFailureObject<Promise>::value>;

    Core core;
    alignas(FrameAllocator) unsigned char allocatorStorage[sizeof(FrameAllocator)];

    // 分配并初始化帧：参数副本 → promise（能用参数副本构造就传入）→ 局部（Body）。
    // 返回空表示分配失败且 promise 提供了 get_return_object_on_allocation_failure。
    static CoroutineFrame* create(Params params, Allocator const& allocator) {
        static_assert(
            std::is_standard_layout<CoroutineFrame>::value,
            "the coroutine frame must stay standard-layout so that the header, "
            "the prefix and the frame share one address");
        auto frameAllocator = FrameAllocator{allocator};
        auto* const storage = allocate(frameAllocator);
        if (storage == nullptr) return nullptr;

        // 默认初始化：只构造 AwaitSlot，字节数组保持未初始化。
        auto* const frame = ::new (static_cast<void*>(storage)) CoroutineFrame;
        ::new (static_cast<void*>(frame->allocatorStorage))
            FrameAllocator{frameAllocator};
        frame->core.prefix.header = FrameHeader{&destroy, &step, false, false};
        frame->core.suspendPoint = 0U;
        frame->core.initialAwaitResumed = false;
        frame->core.bodyAlive = false;

        try {
            ::new (static_cast<void*>(frame->core.paramsStorage))
                Params(std::move(params));
        } catch (...) {
            release(frame);
            throw;
        }
        try {
            constructPromiseFor<Promise>(frame->core.params(),
                                         frame->core.prefix.promise, 0);
        } catch (...) {
            frame->core.params().~Params();
            release(frame);
            throw;
        }
        try {
            ::new (static_cast<void*>(frame->core.bodyStorage))
                Body(frame->core.params());
        } catch (...) {
            frame->core.promise().~Promise();
            frame->core.params().~Params();
            release(frame);
            throw;
        }
        frame->core.bodyAlive = true;
        return frame;
    }

    // 运行到下一个挂起点。catch 块就是标准协程体变换里的那个 catch。
    static FrameHeader* step(FrameHeader* const header) {
        auto* const frame = fromHeader(header);
        auto& core = frame->core;
        core.markRunning();
        try {
            return core.body()(core);
        } catch (...) {
            // 异常离开协程体：等待中的 awaiter 临时对象与局部先于 catch 处理销毁。
            core.resetAwaiter();
            if (not core.initialAwaitResumed) {
                // 初始挂起点之前的异常：销毁协程状态，异常抛给调用方/恢复方。
                destroy(header);
                throw;
            }
            core.destroyBody();
            // unhandled_exception 再抛出时，协程停在 final suspend point。
            core.prefix.header.done = true;
            core.markSuspended();
            core.promise().unhandled_exception();
            core.prefix.header.done = false;
            core.markRunning();
            return core.enterFinalSuspend();
        }
    }

    // 销毁协程状态：等待中的 awaiter、仍存活的局部（Body）、promise、参数副本，然后
    // 释放帧。
    static void destroy(FrameHeader* const header) noexcept {
        auto* const frame = fromHeader(header);
        frame->core.slot.reset();
        frame->core.destroyBody();
        frame->core.promise().~Promise();
        frame->core.params().~Params();
        release(frame);
    }

  private:
    static CoroutineFrame* fromHeader(FrameHeader* const header) noexcept {
        // header 是 core.prefix 的首成员，core.prefix 是 core 的首成员，core 是帧的首
        // 成员；标准布局保证三者地址相同。
        return reinterpret_cast<CoroutineFrame*>(header);
    }

    static CoroutineFrame* allocate(FrameAllocator& frameAllocator) {
        return allocateWith(frameAllocator, UsesPromiseOperatorNew{});
    }

    static CoroutineFrame* allocateWith(FrameAllocator&, std::true_type) {
        return static_cast<CoroutineFrame*>(
            promiseAllocate<Promise>(sizeof(CoroutineFrame), 0));
    }

    static CoroutineFrame* allocateWith(FrameAllocator& frameAllocator,
                                        std::false_type) {
        return allocateFromAllocator(frameAllocator, HasFailureObject{});
    }

    static CoroutineFrame* allocateFromAllocator(FrameAllocator& frameAllocator,
                                                 std::false_type) {
        return AllocatorTraits::allocate(frameAllocator, 1);
    }

    static CoroutineFrame* allocateFromAllocator(FrameAllocator& frameAllocator,
                                                 std::true_type) {
        try {
            return AllocatorTraits::allocate(frameAllocator, 1);
        } catch (std::bad_alloc const&) {
            return nullptr;
        }
    }

    // 结束帧对象自身的生命周期并释放内存。
    static void release(CoroutineFrame* const frame) noexcept {
        auto& stored = *reinterpret_cast<FrameAllocator*>(frame->allocatorStorage);
        auto allocatorCopy = FrameAllocator{std::move(stored)};
        stored.~FrameAllocator();
        frame->~CoroutineFrame();
        deallocateWith(frame, allocatorCopy, UsesPromiseOperatorNew{});
    }

    static void deallocateWith(CoroutineFrame* const frame, FrameAllocator&,
                               std::true_type) noexcept {
        promiseDeallocate<Promise>(frame, sizeof(CoroutineFrame), Rank<2>{});
    }

    static void deallocateWith(CoroutineFrame* const frame,
                               FrameAllocator& allocatorCopy,
                               std::false_type) noexcept {
        AllocatorTraits::deallocate(allocatorCopy, frame, 1);
    }
};

using DefaultFrameAllocator = std::allocator<unsigned char>;

// ---- ramp ----
//
// 分配帧 → 构造 promise → get_return_object → co_await initial_suspend。不挂起的
// 协程在这里直接运行；初始挂起点之前的异常销毁帧并抛给调用方。

// 帧已分配：get_return_object → co_await initial_suspend。
template <class Return, class Promise, class Params, class Body, class Allocator>
Return runRamp(CoroutineFrame<Promise, Params, Body, Allocator>& frame) {
    using Core = FrameCore<Promise, Params, Body>;
    auto& core = frame.core;

    struct DestroyOnException {
        FrameHeader* header;
        ~DestroyOnException() {
            if (header != nullptr) header->destroy(header);
        }
    } guard{&core.prefix.header};

    auto returned = core.promise().get_return_object();
    auto& initial = core.slot.template emplace<typename Core::InitialAwaiter>(
        getAwaiter(core.promise().initial_suspend()));
    if (initial.await_ready()) {
        guard.header = nullptr;
        core.handle()
            .resume(); // 不挂起：直接进入 case 0（await_resume 之后是用户代码）
        return Return(std::move(returned));
    }
    auto const outcome = suspendWith(initial, core.handle());
    guard.header = nullptr;
    if (not outcome.suspended) {
        core.handle().resume();
    } else if (outcome.next != nullptr) {
        // 初始挂起点上的对称转移：驱动目标，本协程保持挂起。
        coroutine_handle<>::from_address(outcome.next).resume();
    }
    return Return(std::move(returned));
}

// 空指针检查只在 promise 提供 get_return_object_on_allocation_failure 时存在：没有它时
// Frame::create 以 bad_alloc 报告失败、从不返回空，MSVC 会把这个分支判成不可达（C4702）。
template <class Return, class Promise, class Params, class Body, class Allocator>
Return startCoroutineWith(CoroutineFrame<Promise, Params, Body, Allocator>* const frame,
                          std::true_type) {
    if (frame == nullptr) return Promise::get_return_object_on_allocation_failure();
    return runRamp<Return>(*frame);
}

template <class Return, class Promise, class Params, class Body, class Allocator>
Return startCoroutineWith(CoroutineFrame<Promise, Params, Body, Allocator>* const frame,
                          std::false_type) {
    return runRamp<Return>(*frame);
}

template <class Return, class Promise, class Body, class Params, class Allocator>
Return startCoroutine(Params params, Allocator const& allocator) {
    using Frame = CoroutineFrame<Promise, Params, Body, Allocator>;
    return startCoroutineWith<Return>(Frame::create(std::move(params), allocator),
                                      typename Frame::HasFailureObject{});
}

} // namespace detail
} // namespace co2

CO2_DETAIL_MSVC_WARNING_POP
