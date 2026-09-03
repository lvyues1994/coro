#pragma once

#include <cstddef>
#include <exception>
#include <iterator>
#include <memory>
#include <type_traits>
#include <utility>

#include "co2/contract.hpp"
#include "co2/coroutine.hpp"
#include "co2/detail/result_storage.hpp"

// co2：与 std::generator<Ref, V> 同形的同步生成器。
//
//   Generator<int>          yield 值：prvalue 被移进帧，左值被复制（std 的 copy 路径）
//   Generator<T const&>     yield 左值：只记地址，不复制
//   Generator<T, V>         显式 value_type
//
// 嵌套：CO2_YIELD(co2::elements_of(inner())) 把 inner 的元素直接交给根的消费者。所有
// 层共享根 promise 上的簿记（当前元素、最内层活动协程）；消费者的 ++ 直接恢复最内层，
// 内层结束时对称转移回父层，栈深不随嵌套深度增长。内层逃出的异常先在父层的
// elements_of 表达式处重新抛出（父层可以捕获），未捕获时逐层上抛到消费者。
//
// 与 std::generator 的差别：co2 不能让宏展开里的临时对象活过挂起点，因此 yield 的
// prvalue 被移进帧保存（多一次移动；设计稿 D11）。C++14 的 range-for 要求 begin/end
// 同型，因此 end() 返回一个哨兵迭代器，同时也提供 default_sentinel_t 比较。

namespace co2 {

template <class Ref, class V = void> struct Generator;

struct default_sentinel_t {};

namespace detail {

template <class Range> struct ElementsOf { Range range; };

template <class Ref, class V> struct GeneratorTraits {
    static_assert(std::is_void<V>::value ||
                      (std::is_object<V>::value &&
                       std::is_same<V, typename std::remove_cv<V>::type>::value),
                  "Generator<Ref, V>: V must be void or a cv-unqualified object type");
    static_assert(std::is_reference<Ref>::value || std::is_object<Ref>::value,
                  "Generator<Ref, V>: Ref must be a reference or an object type");

    using value_type = typename std::conditional<
        std::is_void<V>::value,
        typename std::remove_cv<typename std::remove_reference<Ref>::type>::type,
        V>::type;
    using reference =
        typename std::conditional<std::is_void<V>::value, Ref&&, Ref>::type;
    using yielded = typename std::conditional<std::is_reference<reference>::value,
                                              reference, reference const&>::type;
};

template <class> struct AlwaysFalse : std::false_type {};

struct NoAwaitInGenerator {};

template <class Ref, class V> struct GeneratorPromise {
    using Traits = GeneratorTraits<Ref, V>;
    using yielded = typename Traits::yielded;
    using Element = typename std::remove_reference<yielded>::type;
    using Stored = typename std::remove_cv<Element>::type;
    using Handle = coroutine_handle<GeneratorPromise>;

    Generator<Ref, V> get_return_object() noexcept;

    suspend_always initial_suspend() noexcept { return {}; }

    struct FinalAwaiter {
        bool await_ready() const noexcept { return false; }

        // 内层结束：把活动协程改回父层并对称转移过去；根结束：停在 final suspend。
        coroutine_handle<> await_suspend(Handle const self) noexcept {
            auto& promise = self.promise();
            if (not promise.parent) return noop_coroutine();
            promise.root->active = promise.parent;
            return promise.parent;
        }

        void await_resume() const noexcept {}
    };

    FinalAwaiter final_suspend() noexcept { return {}; }

    void return_void() noexcept {}

    void unhandled_exception() noexcept { error = std::current_exception(); }

    // std::generator 删除 await_transform：生成器体内不允许 co_await。
    template <class Awaitable>
    NoAwaitInGenerator await_transform(Awaitable&&) noexcept {
        static_assert(AlwaysFalse<Awaitable>::value,
                      "co_await is not allowed inside a co2::Generator");
        return {};
    }

    // ---- yield 左值（yielded 是左值引用）：只记地址 ----

    template <class U = yielded,
              class = typename std::enable_if<std::is_lvalue_reference<U>::value>::type>
    suspend_always yield_value(yielded value) noexcept {
        root->current = std::addressof(value);
        return {};
    }

    // ---- yield prvalue / 需要复制的左值：保存在本层帧里，恢复时销毁 ----

    struct StoredYieldAwaiter {
        GeneratorPromise* promise;

        bool await_ready() const noexcept { return false; }
        void await_suspend(coroutine_handle<>) const noexcept {}
        void await_resume() const noexcept { promise->stored.reset(); }
    };

    template <class U = yielded,
              class = typename std::enable_if<
                  not std::is_lvalue_reference<U>::value ||
                  std::is_const<typename std::remove_reference<U>::type>::value>::type>
    StoredYieldAwaiter yield_value(Stored&& value) {
        return store(std::move(value));
    }

    template <class U = yielded,
              class = typename std::enable_if<
                  std::is_rvalue_reference<U>::value &&
                  std::is_constructible<Stored, Stored const&>::value>::type>
    StoredYieldAwaiter yield_value(Stored const& value) {
        return store(value);
    }

    // ---- yield elements_of(...)：嵌套生成器或任意范围 ----

    struct NestedAwaiter {
        Generator<Ref, V> nested;

        bool await_ready() const noexcept { return false; }

        coroutine_handle<> await_suspend(Handle const parent) noexcept {
            auto& parentPromise = parent.promise();
            auto const child = nested.handle;
            CO2_CONTRACT_CHECK(child && not nested.started);
            nested.started = true;
            auto& childPromise = child.promise();
            childPromise.root = parentPromise.root;
            childPromise.parent = parent;
            parentPromise.root->active = child;
            return child;
        }

        void await_resume() {
            auto& childPromise = nested.handle.promise();
            if (childPromise.error) std::rethrow_exception(childPromise.error);
        }
    };

    // 右值的同型生成器直接接管；其他任何范围（含左值生成器）包成一个新的同型生成器。
    template <class Range> NestedAwaiter yield_value(ElementsOf<Range> elements) {
        return NestedAwaiter{toGenerator(std::forward<Range>(elements.range),
                                         std::is_same<Range, Generator<Ref, V>>{})};
    }

    void rethrowIfFailed() {
        if (not error) return;
        auto pending = std::exception_ptr{};
        std::swap(pending, error);
        std::rethrow_exception(pending);
    }

    Element* current{};
    Handle active;
    GeneratorPromise* root{this};
    Handle parent;
    std::exception_ptr error;
    ResultStorage<Stored> stored;

  private:
    template <class Value> StoredYieldAwaiter store(Value&& value) {
        stored.reset();
        root->current = std::addressof(stored.emplace(std::forward<Value>(value)));
        return StoredYieldAwaiter{this};
    }

    template <class Range>
    static Generator<Ref, V> toGenerator(Range&& range, std::true_type) noexcept {
        return std::move(range);
    }

    template <class Range>
    static Generator<Ref, V> toGenerator(Range&& range, std::false_type);
};

} // namespace detail

// std::ranges::elements_of 的 C++14 拼写（函数：没有 CTAD）。左值范围按引用持有。
template <class Range> detail::ElementsOf<Range> elements_of(Range&& range) {
    return detail::ElementsOf<Range>{std::forward<Range>(range)};
}

template <class Ref, class V> struct Generator {
    using promise_type = detail::GeneratorPromise<Ref, V>;
    using value_type = typename detail::GeneratorTraits<Ref, V>::value_type;
    using reference = typename detail::GeneratorTraits<Ref, V>::reference;
    using yielded = typename detail::GeneratorTraits<Ref, V>::yielded;

    struct iterator {
        using value_type = typename Generator::value_type;
        using reference = typename Generator::reference;
        using difference_type = std::ptrdiff_t;
        using pointer = void;
        using iterator_category = std::input_iterator_tag;

        iterator() noexcept = default;

        reference operator*() const noexcept {
            CO2_CONTRACT_CHECK(not atEnd());
            return static_cast<reference>(*root->current);
        }

        // 恢复最内层活动协程。
        iterator& operator++() {
            CO2_CONTRACT_CHECK(not atEnd());
            root->active.resume();
            root->rethrowIfFailed();
            return *this;
        }

        void operator++(int) { ++*this; }

        friend bool operator==(iterator const& left, iterator const& right) noexcept {
            return left.atEnd() == right.atEnd();
        }

        friend bool operator!=(iterator const& left, iterator const& right) noexcept {
            return not(left == right);
        }

        friend bool operator==(iterator const& it, default_sentinel_t) noexcept {
            return it.atEnd();
        }

        friend bool operator==(default_sentinel_t, iterator const& it) noexcept {
            return it.atEnd();
        }

        friend bool operator!=(iterator const& it, default_sentinel_t) noexcept {
            return not it.atEnd();
        }

        friend bool operator!=(default_sentinel_t, iterator const& it) noexcept {
            return not it.atEnd();
        }

      private:
        explicit iterator(promise_type* const root_) noexcept : root{root_} {}

        bool atEnd() const noexcept { return root == nullptr || root->active.done(); }

        promise_type* root{};

        friend struct Generator;
    };

    Generator() noexcept = default;

    Generator(Generator&& other) noexcept
        : handle{other.handle}, started{other.started} {
        other.handle = nullptr;
        other.started = false;
    }

    Generator& operator=(Generator&& other) noexcept {
        if (this == &other) return *this;
        reset();
        handle = other.handle;
        started = other.started;
        other.handle = nullptr;
        other.started = false;
        return *this;
    }

    Generator(Generator const&) = delete;
    Generator& operator=(Generator const&) = delete;

    ~Generator() { reset(); }

    explicit operator bool() const noexcept { return static_cast<bool>(handle); }

    // 只能调用一次：运行到第一个 yield。
    iterator begin() {
        CO2_CONTRACT_CHECK(handle && not started);
        started = true;
        auto& promise = handle.promise();
        promise.active = handle;
        handle.resume();
        promise.rethrowIfFailed();
        return iterator{&promise};
    }

    iterator end() const noexcept { return iterator{}; }

  private:
    explicit Generator(coroutine_handle<promise_type> const handle_) noexcept
        : handle{handle_} {}

    void reset() noexcept {
        if (not handle) return;
        handle.destroy();
        handle = nullptr;
        started = false;
    }

    coroutine_handle<promise_type> handle;
    bool started{};

    friend struct detail::GeneratorPromise<Ref, V>;
};

namespace detail {

template <class Ref, class V>
Generator<Ref, V> GeneratorPromise<Ref, V>::get_return_object() noexcept {
    return Generator<Ref, V>{Handle::from_promise(*this)};
}

// elements_of(range)：把任意范围包成同型生成器。左值范围按引用捕获。
template <class Ref, class V, class Range>
auto rangeGenerator(Range range)
    CO2_BEG((Generator<Ref, V>), (range), decltype(std::begin(range)) first{};
            decltype(std::end(range)) last{};) {
    for (first = std::begin(range), last = std::end(range); first != last; ++first) {
        CO2_YIELD(*first);
    }
}
CO2_END

template <class Ref, class V>
template <class Range>
Generator<Ref, V> GeneratorPromise<Ref, V>::toGenerator(Range&& range,
                                                        std::false_type) {
    return rangeGenerator<Ref, V, Range>(std::forward<Range>(range));
}

} // namespace detail
} // namespace co2
