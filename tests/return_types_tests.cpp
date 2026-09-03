// co2 v2 的返回类型：Task<T>、Generator<Ref, V>（含 elements_of）、AsyncGenerator<T>。
// 全部只依赖 v2 核心，不需要调度器。

#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "co2/async_generator.hpp"
#include "co2/coroutine.hpp"
#include "co2/generator.hpp"
#include "co2/task.hpp"

namespace {

void fail(char const* expression, int line) {
    std::cerr << "check failed at line " << line << ": " << expression << '\n';
    std::exit(EXIT_FAILURE);
}

#define CHECK(expression)                                                              \
    do {                                                                               \
        if (not(expression)) fail(#expression, __LINE__);                              \
    } while (false)

struct ExpectedError : std::runtime_error {
    ExpectedError() : std::runtime_error{"expected"} {}
};

// 计数器：跟踪帧局部与元素的构造/复制/析构。
struct Counters {
    int constructed{};
    int copied{};
    int moved{};
    int destroyed{};
    int alive() const noexcept { return constructed + copied + moved - destroyed; }
};

struct Tracked {
    explicit Tracked(Counters& counters_, int value_ = 0) noexcept
        : counters{&counters_}, value{value_} {
        ++counters->constructed;
    }
    Tracked(Tracked const& other) noexcept
        : counters{other.counters}, value{other.value} {
        ++counters->copied;
    }
    Tracked(Tracked&& other) noexcept : counters{other.counters}, value{other.value} {
        ++counters->moved;
    }
    Tracked& operator=(Tracked const&) = delete;
    Tracked& operator=(Tracked&&) = delete;
    ~Tracked() { ++counters->destroyed; }

    Counters* counters;
    int value;
};

// 手动事件：把等待者的句柄停放起来，由测试代码决定何时恢复。
struct ManualEvent {
    struct Awaiter {
        ManualEvent* event;
        bool await_ready() const noexcept { return event->ready; }
        void await_suspend(co2::coroutine_handle<> const waiter) noexcept {
            event->waiter = waiter;
        }
        void await_resume() const noexcept {}
    };

    Awaiter wait() noexcept { return Awaiter{this}; }

    void set() {
        ready = true;
        auto const pending = waiter;
        waiter = nullptr;
        if (pending) pending.resume();
    }

    co2::coroutine_handle<> waiter;
    bool ready{};
};

// 根驱动器：以 noop 续体启动 Task，完成后取结果。
template <class T> T runTask(co2::Task<T> task) {
    co2::detail::TaskAccess::start(task, co2::noop_coroutine());
    CHECK(co2::detail::TaskAccess::isDone(task));
    return co2::detail::TaskAccess::takeResult(task);
}

// ---------------------------------------------------------------------------
// Task<T>

auto leaf(std::vector<std::string>& log, int value)
    CO2_BEG(co2::Task<int>, (log, value)) {
    log.push_back(std::string{"leaf "} + static_cast<char>('0' + value));
    CO2_RETURN(value * 2);
}
CO2_END

auto parent(std::vector<std::string>& log)
    CO2_BEG(co2::Task<int>, (log), int first{}; int second{};) {
    log.push_back("parent start");
    CO2_AWAIT_SET(first, leaf(log, 1));
    log.push_back("parent after first");
    CO2_AWAIT_SET(second, leaf(log, 2));
    log.push_back("parent after second");
    CO2_RETURN(first + second);
}
CO2_END

void taskIsLazyAndRunsChildrenInline() {
    std::vector<std::string> log;
    auto task = parent(log);
    CHECK(log.empty());
    CHECK(runTask(std::move(task)) == 6);
    CHECK(
        (log == std::vector<std::string>{"parent start", "leaf 1", "parent after first",
                                         "leaf 2", "parent after second"}));
}

auto voidLeaf(int& counter) CO2_BEG(co2::Task<>, (counter)) {
    ++counter;
    CO2_RETURN();
}
CO2_END

auto voidParent(int& counter) CO2_BEG(co2::Task<void>, (counter)) {
    CO2_AWAIT(voidLeaf(counter));
    CO2_AWAIT(voidLeaf(counter));
}
CO2_END

void voidTaskFlowsOffTheEnd() {
    int counter = 0;
    runTask(voidParent(counter));
    CHECK(counter == 2);
}

auto throwingLeaf() CO2_BEG(co2::Task<int>, ()) {
    throw ExpectedError{};
    CO2_RETURN(0);
}
CO2_END

auto propagating(Counters& counters)
    CO2_BEG(co2::Task<int>, (counters), co2::detail::ResultStorage<Tracked> local;
            int value{};) {
    local.emplace(counters, 7);
    CO2_AWAIT_SET(value, throwingLeaf());
    CO2_RETURN(value);
}
CO2_END

void taskPropagatesExceptionsAndDestroysLocals() {
    Counters counters;
    auto task = propagating(counters);
    co2::detail::TaskAccess::start(task, co2::noop_coroutine());
    CHECK(co2::detail::TaskAccess::isDone(task));
    // 异常离开协程体时局部已销毁，早于 final suspend。
    CHECK(counters.alive() == 0);
    bool thrown = false;
    try {
        co2::detail::TaskAccess::takeResult(task);
    } catch (ExpectedError const&) {
        thrown = true;
    }
    CHECK(thrown);
}

auto chain(int depth) CO2_BEG(co2::Task<int>, (depth), int inner{};) {
    if (depth == 0) CO2_RETURN(0);
    CO2_AWAIT_SET(inner, chain(depth - 1));
    CO2_RETURN(inner + 1);
}
CO2_END

void deepAwaitChainDoesNotGrowTheNativeStack() {
    CHECK(runTask(chain(100000)) == 100000);
}

auto awaitingLvalue(std::vector<std::string>& log)
    CO2_BEG(co2::Task<int>, (log), co2::Task<int> child; int value{};) {
    child = leaf(log, 5);
    CO2_AWAIT_SET(value, child);
    CHECK(co2::detail::TaskAccess::isDone(child));
    CO2_RETURN(value);
}
CO2_END

void awaitingAnLvalueTaskBorrowsIt() {
    std::vector<std::string> log;
    CHECK(runTask(awaitingLvalue(log)) == 10);
}

auto uniqueResult() CO2_BEG(co2::Task<std::unique_ptr<int>>, ()) {
    CO2_RETURN(std::unique_ptr<int>{new int{9}});
}
CO2_END

auto forwardsUnique()
    CO2_BEG(co2::Task<std::unique_ptr<int>>, (), std::unique_ptr<int> value;) {
    CO2_AWAIT_SET(value, uniqueResult());
    CO2_RETURN(std::move(value));
}
CO2_END

void moveOnlyResultsAreMovedOut() {
    auto result = runTask(forwardsUnique());
    CHECK(result != nullptr && *result == 9);
}

auto trackedParam(Counters& counters, Tracked param)
    CO2_BEG(co2::Task<int>, (counters, param),
            co2::detail::ResultStorage<Tracked> local;) {
    local.emplace(counters, 1);
    CO2_AWAIT(co2::suspend_never{});
    CO2_RETURN(param.value);
}
CO2_END

void destroyingAnUnstartedTaskReleasesParametersAndPromise() {
    Counters counters;
    {
        auto task = trackedParam(counters, Tracked{counters, 3});
        CHECK(counters.alive() == 1); // 帧里的参数副本
    }
    CHECK(counters.alive() == 0);
}

auto waitsForEvent(ManualEvent& event, std::vector<std::string>& log)
    CO2_BEG(co2::Task<int>, (event, log)) {
    log.push_back("child waiting");
    CO2_AWAIT(event.wait());
    log.push_back("child resumed");
    CO2_RETURN(1);
}
CO2_END

auto awaitsAsyncChild(ManualEvent& event, std::vector<std::string>& log)
    CO2_BEG(co2::Task<int>, (event, log), int value{};) {
    CO2_AWAIT_SET(value, waitsForEvent(event, log));
    log.push_back("parent resumed");
    CO2_RETURN(value + 1);
}
CO2_END

void asynchronousCompletionTransfersBackToTheAwaiter() {
    ManualEvent event;
    std::vector<std::string> log;
    auto task = awaitsAsyncChild(event, log);
    co2::detail::TaskAccess::start(task, co2::noop_coroutine());
    CHECK(not co2::detail::TaskAccess::isDone(task));
    CHECK((log == std::vector<std::string>{"child waiting"}));
    event.set();
    CHECK(co2::detail::TaskAccess::isDone(task));
    CHECK((log == std::vector<std::string>{"child waiting", "child resumed",
                                           "parent resumed"}));
    CHECK(co2::detail::TaskAccess::takeResult(task) == 2);
}

void tasksAreMovable() {
    std::vector<std::string> log;
    auto first = leaf(log, 1);
    co2::Task<int> second;
    CHECK(first && not second);
    second = std::move(first);
    CHECK(not first && second);
    auto third{std::move(second)};
    CHECK(not second && third);
    CHECK(runTask(std::move(third)) == 2);
}

// ---------------------------------------------------------------------------
// Generator<Ref, V>

auto counting(int limit) CO2_BEG(co2::Generator<int>, (limit), int index{};) {
    for (index = 0; index < limit; ++index)
        CO2_YIELD(index);
}
CO2_END

void generatorYieldsValuesInOrder() {
    std::vector<int> seen;
    for (auto value : counting(5))
        seen.push_back(value);
    CHECK((seen == std::vector<int>{0, 1, 2, 3, 4}));
}

void emptyGeneratorIsImmediatelyDone() {
    auto empty = counting(0);
    auto it = empty.begin();
    CHECK(it == empty.end());
    CHECK(it == co2::default_sentinel_t{});
    CHECK(co2::default_sentinel_t{} == it);
}

auto words() CO2_BEG(co2::Generator<std::string>, ()) {
    CO2_YIELD(std::string{"alpha"});
    CO2_YIELD("beta");
    CO2_YIELD(std::string{"gamma"} + "!");
}
CO2_END

void prvalueYieldsAreStoredInTheFrame() {
    std::vector<std::string> seen;
    for (auto&& word : words())
        seen.push_back(std::move(word));
    CHECK((seen == std::vector<std::string>{"alpha", "beta", "gamma!"}));
}

auto borrowed(std::vector<Tracked> const& items)
    CO2_BEG(co2::Generator<Tracked const&>, (items), std::size_t index{};) {
    for (index = 0; index < items.size(); ++index)
        CO2_YIELD(items[index]);
}
CO2_END

void lvalueYieldsThroughAReferenceGeneratorDoNotCopy() {
    Counters counters;
    std::vector<Tracked> items;
    items.reserve(3);
    for (int i = 0; i < 3; ++i)
        items.emplace_back(counters, i);
    auto const before = counters;
    auto generator = borrowed(items);
    std::size_t index = 0;
    for (auto const& item : generator) {
        CHECK(&item == &items[index]);
        ++index;
    }
    CHECK(index == 3);
    CHECK(counters.copied == before.copied && counters.moved == before.moved);
}

auto copying(Tracked const& item) CO2_BEG(co2::Generator<Tracked>, (item)) {
    CO2_YIELD(item);
}
CO2_END

void lvalueYieldsThroughAValueGeneratorAreCopied() {
    Counters counters;
    {
        Tracked item{counters, 4};
        auto generator = copying(item);
        auto it = generator.begin();
        CHECK(counters.copied == 1);
        CHECK((*it).value == 4);
        Tracked taken{*it}; // reference 是 Tracked&&：可以移走
        CHECK(counters.moved == 1);
        ++it;
        CHECK(it == generator.end());
    }
    CHECK(counters.alive() == 0);
}

auto explicitValueType(int base)
    CO2_BEG((co2::Generator<int const&, int>), (base), int current{};) {
    current = base;
    CO2_YIELD(current);
    current = base + 1;
    CO2_YIELD(current);
}
CO2_END

void explicitValueTypeControlsTheReference() {
    static_assert(
        std::is_same<co2::Generator<int const&, int>::reference, int const&>::value,
        "reference stays Ref when V is given");
    static_assert(std::is_same<co2::Generator<int const&, int>::value_type, int>::value,
                  "value_type is V");
    static_assert(std::is_same<co2::Generator<int>::reference, int&&>::value,
                  "value generators hand out rvalue references");
    std::vector<int> seen;
    for (auto const& value : explicitValueType(10))
        seen.push_back(value);
    CHECK((seen == std::vector<int>{10, 11}));
}

auto nested(int depth) CO2_BEG(co2::Generator<int>, (depth)) {
    if (depth == 0) {
        CO2_YIELD(0);
    } else {
        CO2_YIELD(depth);
        CO2_YIELD(co2::elements_of(nested(depth - 1)));
        CO2_YIELD(-depth);
    }
}
CO2_END

void elementsOfFlattensNestedGenerators() {
    std::vector<int> seen;
    for (auto value : nested(3))
        seen.push_back(value);
    CHECK((seen == std::vector<int>{3, 2, 1, 0, -1, -2, -3}));
}

auto deepNesting(int depth) CO2_BEG(co2::Generator<int>, (depth)) {
    if (depth == 0) {
        CO2_YIELD(0);
    } else {
        CO2_YIELD(co2::elements_of(deepNesting(depth - 1)));
    }
}
CO2_END

void deepNestingDoesNotGrowTheNativeStackWhileIterating() {
    // 每一步 ++ 直接恢复最内层；内层结束的转移链逐层迭代，不递归。
    int count = 0;
    for (auto value : deepNesting(20000)) {
        CHECK(value == 0);
        ++count;
    }
    CHECK(count == 1);
}

auto fromRanges(std::vector<int> const& borrowedRange)
    CO2_BEG(co2::Generator<int>, (borrowedRange)) {
    CO2_YIELD(co2::elements_of(borrowedRange));
    CO2_YIELD(co2::elements_of(std::vector<int>{7, 8}));
    CO2_YIELD(9);
}
CO2_END

void elementsOfAcceptsArbitraryRanges() {
    std::vector<int> const source{5, 6};
    std::vector<int> seen;
    for (auto value : fromRanges(source))
        seen.push_back(value);
    CHECK((seen == std::vector<int>{5, 6, 7, 8, 9}));
}

auto reusesStartedGenerator()
    CO2_BEG(co2::Generator<int>, (), co2::Generator<int> inner;) {
    inner = counting(3);
    // 左值生成器按范围处理：包成新的同型生成器逐个转发。
    CO2_YIELD(co2::elements_of(inner));
}
CO2_END

void lvalueGeneratorsAreTreatedAsRanges() {
    std::vector<int> seen;
    for (auto value : reusesStartedGenerator())
        seen.push_back(value);
    CHECK((seen == std::vector<int>{0, 1, 2}));
}

auto failingInner(Counters& counters)
    CO2_BEG(co2::Generator<int>, (counters),
            co2::detail::ResultStorage<Tracked> local;) {
    local.emplace(counters, 1);
    CO2_YIELD(1);
    throw ExpectedError{};
}
CO2_END

auto failingOuter(Counters& counters, std::vector<std::string>& log)
    CO2_BEG(co2::Generator<int>, (counters, log),
            co2::detail::ResultStorage<Tracked> local;) {
    local.emplace(counters, 2);
    CO2_YIELD(0);
    CO2_YIELD(co2::elements_of(failingInner(counters)));
    log.push_back("outer continued"); // 内层的异常在 elements_of 处重新抛出，不会到这里
    CO2_YIELD(2);
}
CO2_END

void nestedExceptionsPropagateThroughTheParentToTheConsumer() {
    Counters counters;
    std::vector<std::string> log;
    auto generator = failingOuter(counters, log);
    auto it = generator.begin();
    CHECK(*it == 0);
    ++it;
    CHECK(*it == 1);
    CHECK(counters.alive() == 2);
    bool thrown = false;
    try {
        ++it;
    } catch (ExpectedError const&) {
        thrown = true;
    }
    CHECK(thrown);
    CHECK(log.empty());
    CHECK(it == generator.end());
    // 两层的局部都已随各自的协程体销毁；帧本身留到 generator 析构。
    CHECK(counters.alive() == 0);
}

auto holdsLocal(Counters& counters, int depth)
    CO2_BEG(co2::Generator<int>, (counters, depth),
            co2::detail::ResultStorage<Tracked> local;) {
    local.emplace(counters, depth);
    CO2_YIELD(depth);
    if (depth > 0) CO2_YIELD(co2::elements_of(holdsLocal(counters, depth - 1)));
    CO2_YIELD(-1);
}
CO2_END

void destroyingAGeneratorMidIterationDestroysEveryActiveFrame() {
    Counters counters;
    {
        auto generator = holdsLocal(counters, 2);
        auto it = generator.begin();
        ++it;
        ++it;
        CHECK(*it == 0);
        CHECK(counters.alive() == 3);
    }
    CHECK(counters.alive() == 0);
}

void generatorsAreMovable() {
    auto first = counting(2);
    co2::Generator<int> second;
    CHECK(first && not second);
    second = std::move(first);
    CHECK(not first && second);
    std::vector<int> seen;
    for (auto value : second)
        seen.push_back(value);
    CHECK((seen == std::vector<int>{0, 1}));
}

// ---------------------------------------------------------------------------
// AsyncGenerator<T>

auto ticks(int limit) CO2_BEG(co2::AsyncGenerator<int>, (limit), int index{};) {
    for (index = 0; index < limit; ++index)
        CO2_YIELD(index);
}
CO2_END

auto sumTicks(int limit)
    CO2_BEG(co2::Task<int>, (limit), co2::AsyncGenerator<int> stream; bool hasValue{};
            int total{};) {
    stream = ticks(limit);
    CO2_AWAIT_SET(hasValue, stream.next());
    while (hasValue) {
        total += stream.value();
        CO2_AWAIT_SET(hasValue, stream.next());
    }
    CO2_RETURN(total);
}
CO2_END

void asyncGeneratorYieldsSynchronously() {
    CHECK(runTask(sumTicks(5)) == 10);
    CHECK(runTask(sumTicks(0)) == 0);
}

auto gatedTicks(ManualEvent& gate, std::vector<std::string>& log)
    CO2_BEG(co2::AsyncGenerator<std::string>, (gate, log)) {
    CO2_YIELD(std::string{"first"});
    log.push_back("producer waiting");
    CO2_AWAIT(gate.wait());
    log.push_back("producer resumed");
    CO2_YIELD(std::string{"second"});
}
CO2_END

auto consumeGated(ManualEvent& gate, std::vector<std::string>& log)
    CO2_BEG(co2::Task<int>, (gate, log), co2::AsyncGenerator<std::string> stream;
            bool hasValue{}; int count{};) {
    stream = gatedTicks(gate, log);
    CO2_AWAIT_SET(hasValue, stream.next());
    while (hasValue) {
        log.push_back("consumer got " + stream.value());
        ++count;
        CO2_AWAIT_SET(hasValue, stream.next());
    }
    log.push_back("consumer done");
    CO2_RETURN(count);
}
CO2_END

void asyncGeneratorSuspendsTheConsumerWhileTheProducerWaits() {
    ManualEvent gate;
    std::vector<std::string> log;
    auto task = consumeGated(gate, log);
    co2::detail::TaskAccess::start(task, co2::noop_coroutine());
    CHECK(not co2::detail::TaskAccess::isDone(task));
    CHECK((log == std::vector<std::string>{"consumer got first", "producer waiting"}));
    gate.set();
    CHECK(co2::detail::TaskAccess::isDone(task));
    CHECK((log == std::vector<std::string>{"consumer got first", "producer waiting",
                                           "producer resumed", "consumer got second",
                                           "consumer done"}));
    CHECK(co2::detail::TaskAccess::takeResult(task) == 2);
}

auto failingTicks(Counters& counters)
    CO2_BEG(co2::AsyncGenerator<int>, (counters),
            co2::detail::ResultStorage<Tracked> local;) {
    local.emplace(counters, 1);
    CO2_YIELD(1);
    throw ExpectedError{};
}
CO2_END

auto consumeFailing(Counters& counters, std::vector<int>& seen)
    CO2_BEG(co2::Task<int>, (counters, seen), co2::AsyncGenerator<int> stream;
            bool hasValue{};) {
    stream = failingTicks(counters);
    CO2_AWAIT_SET(hasValue, stream.next());
    while (hasValue) {
        seen.push_back(stream.value());
        CO2_AWAIT_SET(hasValue, stream.next());
    }
    CO2_RETURN(0);
}
CO2_END

void asyncGeneratorExceptionsReachTheConsumer() {
    Counters counters;
    std::vector<int> seen;
    auto task = consumeFailing(counters, seen);
    co2::detail::TaskAccess::start(task, co2::noop_coroutine());
    CHECK(co2::detail::TaskAccess::isDone(task));
    CHECK((seen == std::vector<int>{1}));
    bool thrown = false;
    try {
        co2::detail::TaskAccess::takeResult(task);
    } catch (ExpectedError const&) {
        thrown = true;
    }
    CHECK(thrown);
    CHECK(counters.alive() == 0);
}

auto trackedTicks(Counters& counters) CO2_BEG(co2::AsyncGenerator<int>, (counters),
                                              co2::detail::ResultStorage<Tracked> local;
                                              int index{};) {
    local.emplace(counters, 1);
    for (index = 0;; ++index)
        CO2_YIELD(index);
}
CO2_END

auto takesTwo(Counters& counters)
    CO2_BEG(co2::Task<int>, (counters), co2::AsyncGenerator<int> stream;
            bool hasValue{}; int total{};) {
    stream = trackedTicks(counters);
    CO2_AWAIT_SET(hasValue, stream.next());
    total += stream.value();
    CO2_AWAIT_SET(hasValue, stream.next());
    total += stream.value();
    CHECK(counters.alive() == 1);
    // 停在 yield 点的生产者可以随时销毁：局部随之销毁。
    stream = co2::AsyncGenerator<int>{};
    CHECK(counters.alive() == 0);
    CO2_RETURN(total);
}
CO2_END

void asyncGeneratorCanBeDestroyedBetweenElements() {
    Counters counters;
    CHECK(runTask(takesTwo(counters)) == 1);
}

} // namespace

int main() {
    taskIsLazyAndRunsChildrenInline();
    voidTaskFlowsOffTheEnd();
    taskPropagatesExceptionsAndDestroysLocals();
    deepAwaitChainDoesNotGrowTheNativeStack();
    awaitingAnLvalueTaskBorrowsIt();
    moveOnlyResultsAreMovedOut();
    destroyingAnUnstartedTaskReleasesParametersAndPromise();
    asynchronousCompletionTransfersBackToTheAwaiter();
    tasksAreMovable();

    generatorYieldsValuesInOrder();
    emptyGeneratorIsImmediatelyDone();
    prvalueYieldsAreStoredInTheFrame();
    lvalueYieldsThroughAReferenceGeneratorDoNotCopy();
    lvalueYieldsThroughAValueGeneratorAreCopied();
    explicitValueTypeControlsTheReference();
    elementsOfFlattensNestedGenerators();
    deepNestingDoesNotGrowTheNativeStackWhileIterating();
    elementsOfAcceptsArbitraryRanges();
    lvalueGeneratorsAreTreatedAsRanges();
    nestedExceptionsPropagateThroughTheParentToTheConsumer();
    destroyingAGeneratorMidIterationDestroysEveryActiveFrame();
    generatorsAreMovable();

    asyncGeneratorYieldsSynchronously();
    asyncGeneratorSuspendsTheConsumerWhileTheProducerWaits();
    asyncGeneratorExceptionsReachTheConsumer();
    asyncGeneratorCanBeDestroyedBetweenElements();
    return 0;
}
