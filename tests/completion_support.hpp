#pragma once

// 三种完成方式的测试共用的 awaiter 与协程。completion_tests.cpp 测机制本身，
// completion_components_tests.cpp 测各个组件，两边都用这里的东西。

#include <stdexcept>
#include <vector>

#include "co2/completion.hpp"
#include "co2/coroutine.hpp"
#include "co2/task.hpp"
#include "sample_errc.hpp"

namespace sample {

inline co2::Completion stopped() { return co2::Completion::stopped(); }
inline co2::Completion failed() { return co2::Completion::error(Errc::gone); }

// 立刻就绪，以给定的方式结束；有值时 await_resume() 给出 7。
struct Ends {
    co2::Completion completion;

    bool await_ready() const noexcept { return true; }
    void await_suspend(co2::coroutine_handle<>) const noexcept {}
    int await_resume() const noexcept { return 7; }
    co2::Completion await_completion() noexcept { return completion; }
};

struct ExpectedError : std::runtime_error {
    ExpectedError() : std::runtime_error{"expected"} {}
};

enum class Leaf { returns, fails, stops, throws };

// 记下每一层的局部是按什么顺序析构的。
struct DepthGuard {
    std::vector<int>* order{};
    int depth{};
    ~DepthGuard() {
        if (order != nullptr) order->push_back(depth);
    }
};

// 一条 depth + 1 层的等待链：最里面一层按 leaf 结束，外面每一层等待里面一层、把结果
// 加一。`resumed` 数的是有多少层在等待之后继续执行了。
inline auto chain(int depth, Leaf leaf, int& resumed, std::vector<int>* order)
    CO2_BEG(co2::Task<int>, (depth, leaf, resumed, order), int value{};
            DepthGuard guard;) {
    guard.order = order;
    guard.depth = depth;
    if (depth == 0) {
        if (leaf == Leaf::throws) throw ExpectedError{};
        if (leaf == Leaf::fails) CO2_AWAIT(Ends{failed()});
        if (leaf == Leaf::stops) CO2_AWAIT(Ends{stopped()});
        CO2_RETURN(1);
    }
    CO2_AWAIT_SET(value, chain(depth - 1, leaf, resumed, order));
    ++resumed;
    CO2_RETURN(value + 1);
}
CO2_END

} // namespace sample
