#pragma once

#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

#include "co2/contract.hpp"

namespace co2 {
namespace detail {

// F 能否以 Args... 调用并把结果当作 R（R 为 void 时丢弃结果）。C++14 没有 is_invocable_r。
template <class R, class F, class... Args> struct IsCallableAs {
  private:
    template <class G>
    static auto probe(int) -> std::integral_constant<
        bool, std::is_void<R>::value ||
                  std::is_convertible<
                      decltype(std::declval<G&>()(std::declval<Args>()...)), R>::value>;
    template <class> static std::false_type probe(long);

  public:
    static constexpr bool value = decltype(probe<F>(0))::value;
};

template <class Signature> struct MoveOnlyFunction;

// 只可移动的类型擦除可调用对象——C++14 没有 std::move_only_function，而 std::function
// 要求可调用对象可拷贝，捕获了 unique_ptr 之类 move-only 对象的闭包放不进去。
//
// 大小与 std::function 相同（4 个指针），内联容量 3 个指针（libstdc++ 的 std::function
// 只有 2 个）。闭包更大、对齐更严、或移动构造会抛出时退化为一次堆分配。移动构造
// noexcept：内联对象靠 F 的 nothrow 移动重定位，堆上的偷指针——这正是内联条件要求
// nothrow-move 的原因，也是 CallbackAwaitable 能在被移进帧槽时保持 noexcept 的依据。
template <class R, class... Args> struct MoveOnlyFunction<R(Args...)> {
    static constexpr std::size_t inlineCapacity = 3U * sizeof(void*);

    template <class F> static constexpr bool isInline() noexcept {
        return sizeof(F) <= inlineCapacity && alignof(F) <= alignof(void*) &&
               std::is_nothrow_move_constructible<F>::value;
    }

    MoveOnlyFunction() noexcept = default;

    // 与 std::function 一样隐式：任何可调用对象都能直接交给它。
    template <class F, class Decayed = typename std::decay<F>::type,
              class = typename std::enable_if<
                  not std::is_same<Decayed, MoveOnlyFunction>::value &&
                  IsCallableAs<R, Decayed, Args...>::value>::type>
    MoveOnlyFunction(F&& callable) {
        using Storage = StorageFor<Decayed>;
        Storage::construct(storage, std::forward<F>(callable));
        ops = Storage::table();
    }

    MoveOnlyFunction(MoveOnlyFunction&& other) noexcept { takeFrom(other); }

    MoveOnlyFunction& operator=(MoveOnlyFunction&& other) noexcept {
        if (this == &other) return *this;
        reset();
        takeFrom(other);
        return *this;
    }

    MoveOnlyFunction(MoveOnlyFunction const&) = delete;
    MoveOnlyFunction& operator=(MoveOnlyFunction const&) = delete;

    ~MoveOnlyFunction() { reset(); }

    explicit operator bool() const noexcept { return ops != nullptr; }

    R operator()(Args... args) {
        CO2_CONTRACT_CHECK(ops != nullptr);
        return ops->invoke(storage, std::forward<Args>(args)...);
    }

    void reset() noexcept {
        if (ops == nullptr) return;
        auto const* const table = ops;
        ops = nullptr;
        table->destroy(storage);
    }

  private:
    // 每个 F 一份静态表；对象里只放一个指针，其余空间全给内联缓冲。
    struct Ops {
        R (*invoke)(void*, Args&&...);
        void (*relocate)(void* to, void* from) noexcept; // 移动构造到 to 并销毁 from
        void (*destroy)(void*) noexcept;
    };

    template <class F> struct InlineStorage {
        static F& at(void* const slot) noexcept { return *static_cast<F*>(slot); }

        template <class G> static void construct(void* const slot, G&& callable) {
            ::new (slot) F(std::forward<G>(callable));
        }

        static R invoke(void* const slot, Args&&... args) {
            return at(slot)(std::forward<Args>(args)...);
        }

        static void relocate(void* const to, void* const from) noexcept {
            ::new (to) F(std::move(at(from)));
            at(from).~F();
        }

        static void destroy(void* const slot) noexcept { at(slot).~F(); }

        static Ops const* table() noexcept {
            static Ops const ops{&invoke, &relocate, &destroy};
            return &ops;
        }
    };

    template <class F> struct HeapStorage {
        static F*& at(void* const slot) noexcept { return *static_cast<F**>(slot); }

        template <class G> static void construct(void* const slot, G&& callable) {
            ::new (slot) F* {new F(std::forward<G>(callable))};
        }

        static R invoke(void* const slot, Args&&... args) {
            return (*at(slot))(std::forward<Args>(args)...);
        }

        static void relocate(void* const to, void* const from) noexcept {
            ::new (to) F* {at(from)};
            at(from) = nullptr;
        }

        static void destroy(void* const slot) noexcept { delete at(slot); }

        static Ops const* table() noexcept {
            static Ops const ops{&invoke, &relocate, &destroy};
            return &ops;
        }
    };

    template <class F>
    using StorageFor = typename std::conditional<isInline<F>(), InlineStorage<F>,
                                                 HeapStorage<F>>::type;

    // 前置条件：本对象为空。
    void takeFrom(MoveOnlyFunction& other) noexcept {
        ops = other.ops;
        if (ops == nullptr) return;
        other.ops = nullptr;
        ops->relocate(storage, other.storage);
    }

    alignas(void*) unsigned char storage[inlineCapacity];
    Ops const* ops{};
};

} // namespace detail
} // namespace co2
