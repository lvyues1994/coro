#pragma once

// co2 v2 核心测试共用的"记录一切协议调用"的 promise / awaiter 类型。
//
// 以 Lib 参数化：Lib::Handle<P> 是句柄模板，Lib::SuspendAlways/SuspendNever 是标准
// 的两个平凡 awaiter。C++14 构建下 Lib 只能是 Co2Lib；C++20 差分测试同时用 StdLib
// 实例化同一份类型，让宏协程与真正的 co_await 协程共享 promise 与 awaiter 代码。

#include <cstddef>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace trace {

struct EventLog {
    std::vector<std::string> events;

    void add(std::string event) { events.push_back(std::move(event)); }
};

// 不用 std::to_string：clang 14 配 libstdc++ 14 在 C++20 模式下无法实例化它。
inline std::string intToString(int value) {
    if (value == 0) return "0";
    auto negative = value < 0;
    auto digits = std::string{};
    while (value != 0) {
        digits.insert(digits.begin(),
                      static_cast<char>('0' + (negative ? -(value % 10) : value % 10)));
        value /= 10;
    }
    return negative ? "-" + digits : digits;
}

// 析构时记录事件的值类型：用作参数副本与局部，观察销毁顺序。
struct Tracked {
    Tracked(EventLog& log_, std::string name_) : log{&log_}, name{std::move(name_)} {
        log->add(name + " constructed");
    }

    Tracked(Tracked&& other) noexcept : log{other.log}, name{std::move(other.name)} {
        other.log = nullptr;
    }

    Tracked(Tracked const&) = delete;
    Tracked& operator=(Tracked const&) = delete;
    Tracked& operator=(Tracked&&) = delete;

    ~Tracked() {
        if (log != nullptr) log->add(name + " destroyed");
    }

    EventLog* log;
    std::string name;
};

// 局部对象：默认构造（帧成员），可以稍后 bind 到日志。
struct TrackedLocal {
    TrackedLocal() noexcept = default;

    ~TrackedLocal() {
        if (log != nullptr) log->add("local destroyed");
    }

    void bind(EventLog& log_) {
        log = &log_;
        log->add("local bound");
    }

    EventLog* log{};
};

template <class Lib> struct Types {
    template <class P = void> using Handle = typename Lib::template Handle<P>;

    // ---- awaiter 家族 ----

    // await_suspend 返回 void；可选择是否立即就绪。
    struct SuspendVoid {
        EventLog* log;
        char const* name;
        bool ready;
        Handle<>* parked;

        bool await_ready() const {
            log->add(std::string{name} + " await_ready");
            return ready;
        }
        void await_suspend(Handle<> handle) const {
            log->add(std::string{name} + " await_suspend(void)");
            if (parked != nullptr) *parked = handle;
        }
        int await_resume() const {
            log->add(std::string{name} + " await_resume");
            return 7;
        }
    };

    // await_suspend 返回 bool。
    struct SuspendBool {
        EventLog* log;
        bool result;
        Handle<>* parked;

        bool await_ready() const {
            log->add("bool await_ready");
            return false;
        }
        bool await_suspend(Handle<> handle) const {
            log->add(result ? "bool await_suspend -> true"
                            : "bool await_suspend -> false");
            if (parked != nullptr) *parked = handle;
            return result;
        }
        void await_resume() const { log->add("bool await_resume"); }
    };

    // await_suspend 返回句柄：对称转移。
    struct SuspendTransfer {
        EventLog* log;
        Handle<> target;
        Handle<>* parked;

        bool await_ready() const { return false; }
        Handle<> await_suspend(Handle<> handle) const {
            log->add("transfer await_suspend");
            if (parked != nullptr) *parked = handle;
            return target;
        }
        void await_resume() const { log->add("transfer await_resume"); }
    };

    // 各阶段可抛出。
    struct Throwing {
        EventLog* log;
        int throwAt; // 1: await_ready, 2: await_suspend, 3: await_resume

        bool await_ready() const {
            if (throwAt == 1) throw std::runtime_error{"await_ready"};
            return false;
        }
        void await_suspend(Handle<>) const {
            if (throwAt == 2) throw std::runtime_error{"await_suspend"};
        }
        void await_resume() const {
            if (throwAt == 3) throw std::runtime_error{"await_resume"};
        }
    };

    // 标准惯用法：借 await_suspend 拿到自己的 promise，返回 false 立即恢复。
    template <class Promise> struct GetPromise {
        Promise** out;
        bool await_ready() const noexcept { return false; }
        bool await_suspend(Handle<Promise> handle) const noexcept {
            *out = &handle.promise();
            return false;
        }
        void await_resume() const noexcept {}
    };

    // ---- promise：记录每次协议调用 ----

    template <class Return> struct PromiseBase {
        EventLog* log{};
        bool lazy{true};
        bool rethrowUnhandled{};
        std::exception_ptr error;

        // 能用参数副本构造时标准要求传入全部参数副本（左值）；多余的参数忽略。
        template <class... Rest>
        PromiseBase(EventLog& log_, bool const lazy_, Rest&...)
            : log{&log_}, lazy{lazy_} {
            log->add("promise(params)");
        }

        PromiseBase() = default;

        ~PromiseBase() {
            if (log != nullptr) log->add("promise destroyed");
        }

        struct InitialAwaiter {
            PromiseBase* promise;
            bool await_ready() const {
                promise->log->add("initial_suspend await_ready");
                return not promise->lazy;
            }
            void await_suspend(Handle<>) const {
                promise->log->add("initial_suspend await_suspend");
            }
            void await_resume() const {
                promise->log->add("initial_suspend await_resume");
            }
        };

        struct FinalAwaiter {
            PromiseBase* promise;
            bool await_ready() const noexcept {
                promise->log->add("final_suspend await_ready");
                return false;
            }
            void await_suspend(Handle<>) const noexcept {
                promise->log->add("final_suspend await_suspend");
            }
            void await_resume() const noexcept {}
        };

        InitialAwaiter initial_suspend() {
            log->add("initial_suspend");
            return InitialAwaiter{this};
        }

        FinalAwaiter final_suspend() noexcept {
            log->add("final_suspend");
            return FinalAwaiter{this};
        }

        void unhandled_exception() {
            log->add("unhandled_exception");
            error = std::current_exception();
            if (rethrowUnhandled) throw;
        }

        // await_transform：把 int 变成立即就绪、返回该值两倍的 awaiter。
        struct Doubled {
            int value;
            bool await_ready() const noexcept { return true; }
            void await_suspend(Handle<>) const noexcept {}
            int await_resume() const noexcept { return value * 2; }
        };
        Doubled await_transform(int const value) {
            log->add("await_transform(int)");
            return Doubled{value};
        }
        // 其余 awaitable 原样通过（不记录，避免淹没其他事件）。
        template <class Other> Other&& await_transform(Other&& other) noexcept {
            return std::forward<Other>(other);
        }

        // co_yield：记录值，挂起。
        struct YieldAwaiter {
            PromiseBase* promise;
            bool await_ready() const { return false; }
            void await_suspend(Handle<>) const {
                promise->log->add("yield await_suspend");
            }
            void await_resume() const { promise->log->add("yield await_resume"); }
        };
        YieldAwaiter yield_value(int const value) {
            log->add("yield_value(" + trace::intToString(value) + ")");
            return YieldAwaiter{this};
        }
    };

    // 返回类型：拥有句柄，析构时 destroy。
    template <class Promise> struct Owner {
        Handle<Promise> handle;

        explicit Owner(Handle<Promise> handle_) noexcept : handle{handle_} {}
        Owner(Owner&& other) noexcept : handle{other.handle} { other.handle = nullptr; }
        Owner(Owner const&) = delete;
        Owner& operator=(Owner const&) = delete;
        Owner& operator=(Owner&&) = delete;
        ~Owner() {
            if (handle) handle.destroy();
        }

        void resume() { handle.resume(); }
        bool done() const { return handle.done(); }
        Promise& promise() const { return handle.promise(); }
    };

    struct IntTask;
    struct IntPromise : PromiseBase<IntTask> {
        using PromiseBase<IntTask>::PromiseBase;
        int result{-1};
        IntTask get_return_object();
        void return_value(int const value) {
            this->log->add("return_value(" + trace::intToString(value) + ")");
            result = value;
        }
    };
    struct IntTask : Owner<IntPromise> {
        using promise_type = IntPromise;
        using Owner<IntPromise>::Owner;
    };

    struct VoidTask;
    struct VoidPromise : PromiseBase<VoidTask> {
        using PromiseBase<VoidTask>::PromiseBase;
        VoidTask get_return_object();
        void return_void() { this->log->add("return_void"); }
    };
    struct VoidTask : Owner<VoidPromise> {
        using promise_type = VoidPromise;
        using Owner<VoidPromise>::Owner;
    };

    // final_suspend 不挂起：协程状态自动销毁，返回对象不拥有任何东西。
    struct FireAndForget {
        struct promise_type {
            EventLog* log{};
            template <class... Rest>
            promise_type(EventLog& log_, Rest&...) : log{&log_} {
                log->add("promise(params)");
            }
            promise_type() = default;
            ~promise_type() {
                if (log != nullptr) log->add("promise destroyed");
            }
            FireAndForget get_return_object() {
                log->add("get_return_object");
                return {};
            }
            typename Lib::SuspendNever initial_suspend() {
                log->add("initial_suspend");
                return {};
            }
            typename Lib::SuspendNever final_suspend() noexcept {
                log->add("final_suspend");
                return {};
            }
            void return_void() { log->add("return_void"); }
            void unhandled_exception() { log->add("unhandled_exception"); }
        };
    };
};

template <class Lib>
typename Types<Lib>::IntTask Types<Lib>::IntPromise::get_return_object() {
    this->log->add("get_return_object");
    return IntTask{Handle<IntPromise>::from_promise(*this)};
}

template <class Lib>
typename Types<Lib>::VoidTask Types<Lib>::VoidPromise::get_return_object() {
    this->log->add("get_return_object");
    return VoidTask{Handle<VoidPromise>::from_promise(*this)};
}

// 带 operator co_await 的 awaitable。C++14 下拼作 operator_co_await；C++20 构建再补上
// 真正的 operator co_await（见各测试文件）。
template <class Lib> struct MemberAwaitable {
    EventLog* log;
    typename Types<Lib>::SuspendVoid operator_co_await() const {
        log->add("member operator co_await");
        return typename Types<Lib>::SuspendVoid{log, "wrapped", true, nullptr};
    }
};

template <class Lib> struct AdlAwaitable { EventLog* log; };

template <class Lib>
typename Types<Lib>::SuspendVoid operator_co_await(AdlAwaitable<Lib> const& awaitable) {
    awaitable.log->add("adl operator co_await");
    return typename Types<Lib>::SuspendVoid{awaitable.log, "wrapped", true, nullptr};
}

} // namespace trace
