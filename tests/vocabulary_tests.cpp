// 三种完成方式用到的词汇类型：co2::optional<T>、co2::Error、co2::Completion、
// co2::Result<T> 与 co2::Unit。这里只测类型本身，不涉及协程。

#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>

#include "co2/completion.hpp"
#include "co2/error.hpp"
#include "co2/optional.hpp"
#include "co2/result.hpp"
#include "sample_errc.hpp"

namespace {

void fail(char const* expression, int line) {
    std::cerr << "check failed at line " << line << ": " << expression << '\n';
    std::exit(EXIT_FAILURE);
}

#define CHECK(expression)                                                              \
    do {                                                                               \
        if (not(expression)) fail(#expression, __LINE__);                              \
    } while (false)

// 统计存活对象数：每次构造 +1，每次析构 -1。
struct Tracked {
    explicit Tracked(int& live_) noexcept : live{&live_} { ++*live; }
    Tracked(Tracked const& other) noexcept : live{other.live} { ++*live; }
    Tracked& operator=(Tracked const&) = delete;
    ~Tracked() { --*live; }

    int* live;
};

// ---------------------------------------------------------------------------
// optional

void anEmptyOptionalHasNoValue() {
    auto const byDefault = co2::optional<int>{};
    co2::optional<int> const fromNullopt = co2::nullopt;
    CHECK(not byDefault.has_value());
    CHECK(not byDefault);
    CHECK(not fromNullopt.has_value());
    CHECK(byDefault == co2::nullopt);
    CHECK(co2::nullopt == byDefault);
    CHECK(byDefault == fromNullopt);
    CHECK(byDefault.value_or(7) == 7);
}

void anOptionalConvertsFromAndComparesWithItsValue() {
    co2::optional<int> answer = 42;
    CHECK(answer.has_value());
    CHECK(answer);
    CHECK(*answer == 42);
    CHECK(answer.value() == 42);
    CHECK(answer == 42);
    CHECK(42 == answer);
    CHECK(answer != 41);
    CHECK(answer != co2::nullopt);
    CHECK(answer.value_or(7) == 42);

    *answer = 43;
    CHECK(answer == 43);

    co2::optional<std::string> text = std::string{"abc"};
    CHECK(text->size() == 3U);
    CHECK(text == std::string{"abc"});
}

void anOptionalOwnsExactlyOneValue() {
    auto live = 0;
    {
        co2::optional<Tracked> first = Tracked{live};
        CHECK(live == 1);

        auto copy = first;
        CHECK(live == 2);

        // 与 std 一致：被移动的一方仍然有值。
        auto moved = std::move(copy);
        CHECK(live == 3);
        CHECK(copy.has_value());

        moved.reset();
        CHECK(live == 2);
        CHECK(not moved.has_value());
        moved.reset();
        CHECK(live == 2);

        moved.emplace(live);
        CHECK(live == 3);
        moved.emplace(live);
        CHECK(live == 3);

        moved = co2::nullopt;
        CHECK(live == 2);

        moved = first;
        CHECK(live == 3);
        moved = std::move(first);
        CHECK(live == 3);
        moved = Tracked{live};
        CHECK(live == 3);
    }
    CHECK(live == 0);
}

void anOptionalHoldsMoveOnlyValues() {
    co2::optional<std::unique_ptr<int>> owner = std::unique_ptr<int>{new int{5}};
    CHECK(owner.has_value());
    CHECK(**owner == 5);

    auto moved = std::move(owner);
    CHECK(**moved == 5);
    CHECK(owner.has_value() && *owner == nullptr);

    auto taken = *std::move(moved);
    CHECK(*taken == 5);
    auto const fallback =
        co2::optional<std::unique_ptr<int>>{}.value_or(std::unique_ptr<int>{});
    CHECK(fallback == nullptr);
}

void optionalsOrderLikeStdOptional() {
    auto const empty = co2::optional<int>{};
    co2::optional<int> const one = 1;
    co2::optional<int> const two = 2;

    // 空的 optional 小于任何有值的。
    CHECK(empty < one);
    CHECK(one > empty);
    CHECK(empty <= empty);
    CHECK(empty >= empty);
    CHECK(one < two);
    CHECK(two >= one);
    CHECK(one != two);
    CHECK(not(one == empty));

    CHECK(empty < 0);
    CHECK(not(empty >= 0));
    CHECK(one >= 1);
    CHECK(one <= 1);
    CHECK(0 < one);
    CHECK(2 > one);
    CHECK(empty != 0);

    CHECK(not(empty < co2::nullopt));
    CHECK(co2::nullopt < one);
    CHECK(one > co2::nullopt);
    CHECK(empty <= co2::nullopt);
    CHECK(co2::nullopt >= empty);

    // 不同的值类型之间也能比较。
    CHECK(co2::optional<long>{1L} == one);
}

// ---------------------------------------------------------------------------
// Error

#if defined(__x86_64__) && (defined(__GLIBCXX__) || defined(_LIBCPP_VERSION))
// 错误码的两个字段与 exception_ptr 共用存储：Error 不比 error_code 大。
static_assert(sizeof(co2::Error) == sizeof(std::error_code),
              "co2::Error must stay as small as std::error_code");
#endif

// 只有 std::error_code 与错误码枚举能隐式变成 Error。
static_assert(not std::is_convertible<int, co2::Error>::value,
              "an integer must not convert to co2::Error");
static_assert(not std::is_convertible<std::exception_ptr, co2::Error>::value,
              "capturing an exception as co2::Error must be explicit");

// 装异常的那一半只在有异常的构建里存在（config.hpp 的 CO2_HAS_EXCEPTIONS）。
template <class E, class = void> struct HasExceptionAccessor : std::false_type {};
template <class E>
struct HasExceptionAccessor<
    E, decltype(static_cast<void>(std::declval<E const&>().exception()))>
    : std::true_type {};

static_assert(HasExceptionAccessor<co2::Error>::value == (CO2_HAS_EXCEPTIONS != 0),
              "Error::exception() exists exactly when the build has exceptions");
static_assert(std::is_constructible<co2::Error, std::exception_ptr>::value ==
                  (CO2_HAS_EXCEPTIONS != 0),
              "an Error captures an exception exactly when the build has exceptions");

bool acceptsError(co2::Error const& error, std::error_code const expected) {
    return error.code() == expected;
}

void anErrorHoldsACode() {
    co2::Error const error = std::make_error_code(std::errc::timed_out);
    CHECK(error.code() == std::errc::timed_out);
#if CO2_HAS_EXCEPTIONS
    CHECK(error.exception() == nullptr);
#endif
}

void anErrorConvertsFromAnErrorCodeEnum() {
    co2::Error const error = sample::Errc::retry_later;
    CHECK(error.code() == sample::Errc::retry_later);
    CHECK(error.code().message() == "retry later");
    CHECK(acceptsError(sample::Errc::gone, sample::Errc::gone));
}

void errorCodesCopyMoveAndAssign() {
    co2::Error first = sample::Errc::retry_later;
    co2::Error second = sample::Errc::gone;

    auto copy = first;
    CHECK(copy.code() == sample::Errc::retry_later);
    CHECK(first.code() == sample::Errc::retry_later);

    auto moved = std::move(copy);
    CHECK(moved.code() == sample::Errc::retry_later);

    moved = second;
    CHECK(moved.code() == sample::Errc::gone);

    second = std::move(first);
    CHECK(second.code() == sample::Errc::retry_later);
}

#if CO2_HAS_EXCEPTIONS
void anErrorHoldsAnException() {
    auto const error = co2::Error{std::make_exception_ptr(std::runtime_error{"boom"})};
    CHECK(error.code() == co2::Errc::unhandled_exception);
    CHECK(std::strcmp(error.code().category().name(), "co2") == 0);
    CHECK(error.code().message() == "unhandled exception");
    CHECK(error.exception() != nullptr);
}

void raiseRethrowsTheOriginalExceptionOrThrowsSystemError() {
    auto const captured =
        co2::Error{std::make_exception_ptr(std::runtime_error{"boom"})};
    auto sawOriginal = false;
    try {
        captured.raise();
    } catch (std::runtime_error const& exception) {
        sawOriginal = std::strcmp(exception.what(), "boom") == 0;
    }
    CHECK(sawOriginal);

    co2::Error const code = sample::Errc::gone;
    auto sawSystemError = false;
    try {
        code.raise();
    } catch (std::system_error const& exception) {
        sawSystemError = exception.code() == sample::Errc::gone;
    }
    CHECK(sawSystemError);
}

void errorsCopyMoveAndAssignAcrossBothKinds() {
    auto exception = co2::Error{std::make_exception_ptr(std::runtime_error{"boom"})};
    co2::Error code = sample::Errc::retry_later;

    auto copy = exception;
    CHECK(copy.exception() == exception.exception());

    // 被移动的一方只保证还能析构和赋值：它的内容取决于标准库的 exception_ptr 移动之后
    // 留下什么，这里不检查。
    auto moved = std::move(copy);
    CHECK(moved.exception() == exception.exception());

    moved = code;
    CHECK(moved.code() == sample::Errc::retry_later);
    CHECK(moved.exception() == nullptr);

    code = exception;
    CHECK(code.exception() == exception.exception());
}
#endif

// ---------------------------------------------------------------------------
// Completion

void aCompletionIsExactlyOneOfThreeKinds() {
    auto const value = co2::Completion::value();
    CHECK(value.isValue() && not value.isStopped() && not value.hasError());

    auto const stopped = co2::Completion::stopped();
    CHECK(stopped.isStopped() && not stopped.isValue() && not stopped.hasError());

    auto failed = co2::Completion::error(sample::Errc::gone);
    CHECK(failed.hasError() && not failed.isValue() && not failed.isStopped());
    CHECK(failed.error().code() == sample::Errc::gone);

    auto copy = failed;
    CHECK(copy.hasError() && copy.error().code() == sample::Errc::gone);

    copy = stopped;
    CHECK(copy.isStopped());
    copy = std::move(failed);
    CHECK(copy.hasError());

    co2::Error const taken = std::move(copy).error();
    CHECK(taken.code() == sample::Errc::gone);
}

// ---------------------------------------------------------------------------
// Result 与 Unit

co2::Result<int> parse(int const input) {
    if (input < 0) return sample::Errc::gone; // 错误码枚举直接构造出"错误"
    if (input == 0) return co2::Result<int>::stopped();
    return input * 2;
}

void aResultIsAValueAnErrorOrStopped() {
    auto const value = parse(21);
    CHECK(value.hasValue() && not value.hasError() && not value.isStopped());
    CHECK(value);
    CHECK(value.value() == 42);

    auto const error = parse(-1);
    CHECK(error.hasError() && not error.hasValue() && not error.isStopped());
    CHECK(not error);
    CHECK(error.error().code() == sample::Errc::gone);

    auto const stopped = parse(0);
    CHECK(stopped.isStopped() && not stopped.hasValue() && not stopped.hasError());
    CHECK(not stopped);
}

void aResultOwnsExactlyOneValue() {
    auto live = 0;
    {
        co2::Result<Tracked> first = Tracked{live};
        CHECK(live == 1);

        auto copy = first;
        CHECK(live == 2);

        auto moved = std::move(copy);
        CHECK(live == 3);

        moved = co2::Result<Tracked>::stopped();
        CHECK(live == 2);
        CHECK(moved.isStopped());

        moved = co2::Error{sample::Errc::retry_later};
        CHECK(live == 2);
        CHECK(moved.hasError());

        moved = first;
        CHECK(live == 3);
        CHECK(moved.hasValue());
    }
    CHECK(live == 0);
}

void aResultHoldsMoveOnlyValuesAndVoid() {
    co2::Result<std::unique_ptr<int>> owner = std::unique_ptr<int>{new int{5}};
    auto moved = std::move(owner);
    auto taken = std::move(moved).value();
    CHECK(*taken == 5);

    auto const done = co2::Result<void>{};
    CHECK(done.hasValue());
    done.value();

    co2::Result<void> failed = sample::Errc::retry_later;
    CHECK(failed.hasError());
    CHECK(failed.error().code() == sample::Errc::retry_later);

    failed = co2::Result<void>::stopped();
    CHECK(failed.isStopped());
    failed = done;
    CHECK(failed.hasValue());
}

void unitIsAnEmptyValue() {
    CHECK(co2::Unit{} == co2::Unit{});
    CHECK(not(co2::Unit{} != co2::Unit{}));
    co2::optional<co2::Unit> const done = co2::Unit{};
    CHECK(done.has_value());
    CHECK(done == co2::Unit{});
}

} // namespace

int main() {
    anEmptyOptionalHasNoValue();
    anOptionalConvertsFromAndComparesWithItsValue();
    anOptionalOwnsExactlyOneValue();
    anOptionalHoldsMoveOnlyValues();
    optionalsOrderLikeStdOptional();
    anErrorHoldsACode();
    anErrorConvertsFromAnErrorCodeEnum();
    errorCodesCopyMoveAndAssign();
#if CO2_HAS_EXCEPTIONS
    anErrorHoldsAnException();
    raiseRethrowsTheOriginalExceptionOrThrowsSystemError();
    errorsCopyMoveAndAssignAcrossBothKinds();
#endif
    aCompletionIsExactlyOneOfThreeKinds();
    aResultIsAValueAnErrorOrStopped();
    aResultOwnsExactlyOneValue();
    aResultHoldsMoveOnlyValuesAndVoid();
    unitIsAnEmptyValue();
    return 0;
}
