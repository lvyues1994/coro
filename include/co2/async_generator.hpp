#pragma once

#include <exception>
#include <type_traits>
#include <utility>

#include "co2/contract.hpp"
#include "co2/coroutine.hpp"
#include "co2/detail/result_storage.hpp"
#include "co2/env.hpp"
#include "co2/stop_token.hpp"

// co2：异步生成器。生产者体内可以 CO2_AWAIT，也可以 CO2_YIELD；消费者：
//
//   CO2_AWAIT_SET(hasValue, stream.next());   // true：stream.value() 可用
//
// 每一步都是生产者与消费者之间的对称转移：next() 记下消费者并转移进生产者；生产者
// yield（或结束）时转移回消费者。同一时刻只有一方在运行，因此没有任何同步——消费者
// 在生产者 yield 所在的线程上恢复，与 cppcoro::async_generator 一致。
//
// 环境：生产者属于消费者的等待链。每次 next() 把消费者的 stop_token 交给生产者的
// promise，生产者体内 CO2_AWAIT_SET(token, co2::getStopToken()) 读到它，生产者等待的子
// Task 也从这里继承——与 Task 等待子 Task 的传播方式相同。
//
// 销毁契约：有 next() 未完成时（生产者正在运行或挂起在某个操作上）不能销毁；停在
// yield 点或从未启动时可以随时销毁。

namespace co2 {

template <class T> struct AsyncGenerator;

namespace detail {

template <class T> struct AsyncGeneratorPromise {
    static_assert(std::is_object<T>::value && not std::is_const<T>::value,
                  "AsyncGenerator<T> requires a cv-unqualified object type");

    using Handle = coroutine_handle<AsyncGeneratorPromise>;

    // 声明构造函数是为了让它不是聚合类型：C++20 起聚合可以用圆括号初始化，协程的第一个
    // 参数若是句柄，就会被拿去初始化 consumer——生成器一开始就像有一个未完成的 next()。
    AsyncGeneratorPromise() = default;

    AsyncGenerator<T> get_return_object() noexcept;

    suspend_always initial_suspend() noexcept { return {}; }

    struct YieldAwaiter {
        bool await_ready() const noexcept { return false; }

        coroutine_handle<> await_suspend(Handle const self) noexcept {
            return self.promise().takeConsumer();
        }

        void await_resume() const noexcept {}
    };

    struct FinalAwaiter {
        bool await_ready() const noexcept { return false; }

        coroutine_handle<> await_suspend(Handle const self) noexcept {
            auto& promise = self.promise();
            promise.finished = true;
            return promise.takeConsumer();
        }

        void await_resume() const noexcept {}
    };

    FinalAwaiter final_suspend() noexcept { return {}; }

    YieldAwaiter yield_value(T&& value) {
        stored.emplace(std::move(value));
        return {};
    }

    template <class U = T, class = typename std::enable_if<
                               std::is_constructible<U, U const&>::value>::type>
    YieldAwaiter yield_value(T const& value) {
        stored.emplace(value);
        return {};
    }

    void return_void() noexcept {}

    void unhandled_exception() noexcept { error = std::current_exception(); }

    // ---- 环境 ----

    stop_token get_stop_token() const noexcept { return stopToken.get(); }

    // 生产者等待的子协程从这里借用，不拷贝 token。
    EnvStopToken const& envStopToken() const noexcept { return stopToken; }

    // co_await getStopToken()：不挂起，交出当前这次 next() 的消费者的 token。
    StopTokenAwaiter await_transform(GetStopToken) noexcept {
        return StopTokenAwaiter{stopToken.get()};
    }

    // 其余 awaitable 原样等待（标准：promise 一旦有 await_transform 就必须覆盖全部）。
    template <class Awaitable>
    Awaitable&& await_transform(Awaitable&& awaitable) noexcept {
        return std::forward<Awaitable>(awaitable);
    }

    coroutine_handle<> takeConsumer() noexcept {
        auto const pending = consumer;
        CO2_CONTRACT_CHECK(pending);
        consumer = nullptr;
        return pending;
    }

    coroutine_handle<> consumer;
    ResultStorage<T> stored;
    std::exception_ptr error;
    // 每次 next() 重新取自那一次的消费者；消费者在这一步里一直挂着，借用即可。两次
    // next() 之间它可能已经失效，但那时生产者不在运行，没有人读它。
    EnvStopToken stopToken;
    bool finished{};
};

} // namespace detail

template <class T> struct AsyncGenerator {
    using promise_type = detail::AsyncGeneratorPromise<T>;

    struct NextAwaiter {
        AsyncGenerator* owner;

        bool await_ready() const noexcept {
            CO2_CONTRACT_CHECK(owner->handle);
            return owner->handle.promise().finished;
        }

        // 记下消费者，转移进生产者（首次：从初始挂起点启动；之后：从 yield 点继续）。
        // 带类型的消费者句柄：生产者在这一步里使用这个消费者的环境。
        template <class ConsumerPromise>
        coroutine_handle<>
        await_suspend(coroutine_handle<ConsumerPromise> const consumer) noexcept {
            auto& promise = owner->handle.promise();
            CO2_CONTRACT_CHECK(not promise.consumer);
            promise.stored.reset();
            promise.consumer = consumer;
            detail::inheritStopToken(promise.stopToken, consumer);
            return owner->handle;
        }

        bool await_resume() {
            auto& promise = owner->handle.promise();
            if (promise.error) {
                auto pending = std::exception_ptr{};
                std::swap(pending, promise.error);
                std::rethrow_exception(pending);
            }
            return not promise.finished;
        }
    };

    AsyncGenerator() noexcept = default;

    AsyncGenerator(AsyncGenerator&& other) noexcept : handle{other.handle} {
        other.handle = nullptr;
    }

    AsyncGenerator& operator=(AsyncGenerator&& other) noexcept {
        if (this == &other) return *this;
        reset();
        handle = other.handle;
        other.handle = nullptr;
        return *this;
    }

    AsyncGenerator(AsyncGenerator const&) = delete;
    AsyncGenerator& operator=(AsyncGenerator const&) = delete;

    ~AsyncGenerator() { reset(); }

    explicit operator bool() const noexcept { return static_cast<bool>(handle); }

    // 推进到下一个元素：await 结果为 true 时 value() 可用，直到下一次 next()。
    NextAwaiter next() noexcept { return NextAwaiter{this}; }

    T& value() noexcept {
        CO2_CONTRACT_CHECK(handle && handle.promise().stored.hasValue());
        return handle.promise().stored.get();
    }

  private:
    explicit AsyncGenerator(coroutine_handle<promise_type> const handle_) noexcept
        : handle{handle_} {}

    void reset() noexcept {
        if (not handle) return;
        // 有 next() 未完成：生产者正在运行或挂起在某个操作上，销毁是未定义行为。
        CO2_CONTRACT_CHECK(not handle.promise().consumer);
        handle.destroy();
        handle = nullptr;
    }

    coroutine_handle<promise_type> handle;

    friend struct detail::AsyncGeneratorPromise<T>;
};

namespace detail {

template <class T>
AsyncGenerator<T> AsyncGeneratorPromise<T>::get_return_object() noexcept {
    return AsyncGenerator<T>{Handle::from_promise(*this)};
}

} // namespace detail
} // namespace co2
