#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>

// co2：与 C++20 [thread.stoptoken] 同形的协作式取消：stop_source / stop_token /
// stop_callback。语义逐条对齐标准：
//
//   - request_stop() 原子地"判断并置位"，只有真正发出请求的那次返回 true；
//   - 回调在 request_stop()
//   的调用线程上同步执行；注册时若已请求则在构造函数里立刻执行；
//   - ~stop_callback() 若回调正在另一线程上执行，阻塞到它返回；若回调正在本线程执行
//     （回调销毁了自己的 stop_callback），不阻塞，request_stop() 之后不再触碰该对象；
//   - stop_possible()：已请求，或仍有关联的 stop_source。
//
// 与标准的差别：stop_callback 的 Callback 默认为 std::function<void()>——C++14 没有
// CTAD，lambda 的类型无法拼写。

namespace co2 {

struct nostopstate_t {
    explicit nostopstate_t() = default;
};

constexpr nostopstate_t nostopstate{};

struct stop_token;
struct stop_source;
template <class Callback = std::function<void()>> struct stop_callback;

namespace detail {

// 侵入式回调节点。invoke 是 noexcept 的 thunk：回调抛出即 std::terminate（标准如此）。
struct StopCallbackNode {
    void (*invoke)(StopCallbackNode*) noexcept;
    StopCallbackNode* next{};
    StopCallbackNode* previous{};
    // request_stop
    // 正在执行本节点时指向它栈上的标志；回调若销毁了自己，析构函数把它置真。
    bool* destroyed{};
    bool linked{};
    bool finished{};
};

struct StopState {
    // 所有权引用：token + source + callback（callback 持有 token）。
    std::atomic<std::uint32_t> references{1U};
    std::atomic<std::uint32_t> sources{1U};
    std::atomic<bool> requested{false};

    std::mutex mutex;
    std::condition_variable callbackFinished;
    StopCallbackNode* head{};
    StopCallbackNode* executing{};
    std::thread::id requestingThread;

    void acquire() noexcept { references.fetch_add(1U, std::memory_order_relaxed); }

    void release() noexcept {
        if (references.fetch_sub(1U, std::memory_order_acq_rel) == 1U) delete this;
    }

    bool stopRequested() const noexcept {
        return requested.load(std::memory_order_acquire);
    }

    bool stopPossible() const noexcept {
        return stopRequested() || sources.load(std::memory_order_acquire) != 0U;
    }

    bool requestStop() noexcept {
        if (requested.exchange(true, std::memory_order_acq_rel)) return false;
        std::unique_lock<std::mutex> lock{mutex};
        requestingThread = std::this_thread::get_id();
        while (head != nullptr) {
            auto* const node = head;
            unlink(node);
            executing = node;
            auto destroyed = false;
            node->destroyed = &destroyed;
            lock.unlock();
            node->invoke(node);
            lock.lock();
            executing = nullptr;
            if (destroyed) continue; // 回调销毁了自己的 stop_callback：不再触碰节点
            node->destroyed = nullptr;
            node->finished = true;
            callbackFinished.notify_all();
        }
        return true;
    }

    // 返回 false 表示已经请求过：调用方在当前线程上同步执行回调。
    bool tryRegister(StopCallbackNode* const node) noexcept {
        std::lock_guard<std::mutex> lock{mutex};
        if (stopRequested()) return false;
        node->previous = nullptr;
        node->next = head;
        if (head != nullptr) head->previous = node;
        head = node;
        node->linked = true;
        return true;
    }

    void unregister(StopCallbackNode* const node) noexcept {
        std::unique_lock<std::mutex> lock{mutex};
        if (node->linked) {
            unlink(node);
            return;
        }
        if (executing != node) return; // 已执行完毕
        if (requestingThread == std::this_thread::get_id()) {
            // 回调正在本线程执行并销毁了自己：告诉 requestStop 不要再碰节点。
            *node->destroyed = true;
            return;
        }
        callbackFinished.wait(lock, [node] { return node->finished; });
    }

  private:
    void unlink(StopCallbackNode* const node) noexcept {
        if (node->previous != nullptr)
            node->previous->next = node->next;
        else
            head = node->next;
        if (node->next != nullptr) node->next->previous = node->previous;
        node->next = nullptr;
        node->previous = nullptr;
        node->linked = false;
    }
};

} // namespace detail

struct stop_token {
    stop_token() noexcept = default;

    stop_token(stop_token const& other) noexcept : state{other.state} {
        if (state != nullptr) state->acquire();
    }

    stop_token(stop_token&& other) noexcept : state{other.state} {
        other.state = nullptr;
    }

    stop_token& operator=(stop_token const& other) noexcept {
        stop_token{other}.swap(*this);
        return *this;
    }

    stop_token& operator=(stop_token&& other) noexcept {
        stop_token{std::move(other)}.swap(*this);
        return *this;
    }

    ~stop_token() {
        if (state != nullptr) state->release();
    }

    void swap(stop_token& other) noexcept { std::swap(state, other.state); }

    bool stop_requested() const noexcept {
        return state != nullptr && state->stopRequested();
    }

    bool stop_possible() const noexcept {
        return state != nullptr && state->stopPossible();
    }

    friend bool operator==(stop_token const& left, stop_token const& right) noexcept {
        return left.state == right.state;
    }

    friend bool operator!=(stop_token const& left, stop_token const& right) noexcept {
        return left.state != right.state;
    }

    friend void swap(stop_token& left, stop_token& right) noexcept { left.swap(right); }

  private:
    explicit stop_token(detail::StopState* const state_) noexcept : state{state_} {
        if (state != nullptr) state->acquire();
    }

    detail::StopState* state{};

    friend struct stop_source;
    template <class> friend struct stop_callback;
};

struct stop_source {
    // 分配一个新的停止状态。
    stop_source() : state{new detail::StopState} {}

    explicit stop_source(nostopstate_t) noexcept {}

    stop_source(stop_source const& other) noexcept : state{other.state} {
        if (state != nullptr) {
            state->acquire();
            state->sources.fetch_add(1U, std::memory_order_relaxed);
        }
    }

    stop_source(stop_source&& other) noexcept : state{other.state} {
        other.state = nullptr;
    }

    stop_source& operator=(stop_source const& other) noexcept {
        stop_source{other}.swap(*this);
        return *this;
    }

    stop_source& operator=(stop_source&& other) noexcept {
        stop_source{std::move(other)}.swap(*this);
        return *this;
    }

    ~stop_source() {
        if (state == nullptr) return;
        state->sources.fetch_sub(1U, std::memory_order_acq_rel);
        state->release();
    }

    void swap(stop_source& other) noexcept { std::swap(state, other.state); }

    stop_token get_token() const noexcept { return stop_token{state}; }

    bool stop_possible() const noexcept { return state != nullptr; }

    bool stop_requested() const noexcept {
        return state != nullptr && state->stopRequested();
    }

    // 本次调用真正发出了停止请求时返回 true；回调在本线程上同步执行完毕后返回。
    bool request_stop() noexcept { return state != nullptr && state->requestStop(); }

    friend bool operator==(stop_source const& left, stop_source const& right) noexcept {
        return left.state == right.state;
    }

    friend bool operator!=(stop_source const& left, stop_source const& right) noexcept {
        return left.state != right.state;
    }

    friend void swap(stop_source& left, stop_source& right) noexcept {
        left.swap(right);
    }

  private:
    detail::StopState* state{};
};

template <class Callback> struct stop_callback : private detail::StopCallbackNode {
    using callback_type = Callback;

    template <class C, class = typename std::enable_if<
                           std::is_constructible<Callback, C>::value>::type>
    explicit stop_callback(stop_token const& token_, C&& callback_) noexcept(
        std::is_nothrow_constructible<Callback, C>::value)
        : detail::StopCallbackNode{&invokeThunk}, token{token_}, callback{
                                                                     std::forward<C>(
                                                                         callback_)} {
        attach();
    }

    template <class C, class = typename std::enable_if<
                           std::is_constructible<Callback, C>::value>::type>
    explicit stop_callback(stop_token&& token_, C&& callback_) noexcept(
        std::is_nothrow_constructible<Callback, C>::value)
        : detail::StopCallbackNode{&invokeThunk}, token{std::move(token_)},
          callback{std::forward<C>(callback_)} {
        attach();
    }

    stop_callback(stop_callback const&) = delete;
    stop_callback& operator=(stop_callback const&) = delete;
    stop_callback(stop_callback&&) = delete;
    stop_callback& operator=(stop_callback&&) = delete;

    ~stop_callback() {
        if (registered) token.state->unregister(this);
    }

  private:
    void attach() {
        if (not token.stop_possible()) return; // 永远不会被请求：不注册
        if (token.state->tryRegister(this)) {
            registered = true;
            return;
        }
        invokeThunk(this); // 已经请求过：在当前线程上同步执行
    }

    static void invokeThunk(detail::StopCallbackNode* const node) noexcept {
        std::forward<Callback>(static_cast<stop_callback*>(node)->callback)();
    }

    stop_token token;
    Callback callback;
    bool registered{};
};

namespace detail {

// 把一个 token 的停止请求转发给另一个 source：stop_callback<ForwardStop> 就是父子
// 取消链的一节。
struct ForwardStop {
    stop_source* source;
    void operator()() const noexcept { source->request_stop(); }
};

} // namespace detail
} // namespace co2
