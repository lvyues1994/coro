# 三种完成方式 实施计划（一）：错误按值传播

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 让 `Task` 以值、错误或已停止结束，错误与已停止按值沿等待链向上传，只在根上变回异常或空的 optional。

**Architecture:** 恢复点（一次等待结束、协程继续往下走的位置）在 `await_resume()` 之前询问 awaiter 的可选成员 `await_completion()`；出错或已停止时就此结束协程，把结果交给 promise 的 `return_error()` / `return_stopped()`。`Task` 的 promise 把结果存成四态，它的 awaiter 提供 `await_completion()`，于是错误逐层向上而中间各层不抛不接。不认识这个成员的调用方照旧从 `await_resume()` 拿到异常，所以其余组件在本计划里不需要改动。

**Tech Stack:** C++14，仅头文件；CMake 与 CTest；没有第三方依赖。

设计稿：`docs/design/completion-channels.md`。本计划覆盖它第 10 节的第 1～5 步。

## Global Constraints

- 库与测试的最低语言标准是 C++14，不使用 C++17 及以后的特性。
- 仅头文件，不引入第三方依赖。
- 测试以 `-Wall -Wextra -Wpedantic -Werror -Wconversion -Wsign-conversion -Wold-style-cast -Wzero-as-null-pointer-constant` 等选项编译（见 `tests/CMakeLists.txt`），新代码不得有告警。
- 每个公共头文件都必须能单独包含（`co2-header-self-containment-tests` 会检查）。
- 协议上的名字照设计稿，一字不改：awaiter 的可选成员 `co2::Completion await_completion() noexcept`；promise 的可选入口 `void return_stopped() noexcept` 与 `void return_error(co2::Error) noexcept`（不是 `unhandled_stopped`）。
- 库内 awaiter 的 `await_resume()` 保持自足：出错时抛出（`Error::raise()`），已停止是契约违规。
- 值与错误共用存储：`tests/allocation_tests.cpp` 钉死的帧大小（`Task<int>(int)` 是 176 字节）与 `CallbackAwaitable` 的大小不得改变。
- 前置条件用 `CO2_CONTRACT_CHECK` / `CO2_CONTRACT_FAIL` 报告，不为它们抛异常。
- `whenAll`、等待 `JoinHandle`、`AsyncGenerator`、`fromCallback`、`scheduleOn` 在本计划里不改动。
- 每个任务一个本地提交，不推送。提交信息用中文，正文说明原因与验证。`git add` 写明路径，不要用 `git add -A`：仓库里有不属于本工作的未跟踪目录 `build/`。
- 构建目录放在仓库之外（`/tmp/co2-build/...`），不要动仓库里的 `build/`。

## 范围

设计稿第 10 节有 10 步。本计划做第 1～5 步，做完之后库处于一个完整可用的状态：

- 根（`syncWait`、`JoinHandle::join`）返回 `co2::optional`。
- 错误沿 `Task` 链按值向上传；协程体里的异常只被捕获一次。
- 可以用 `just_error` / `with_error` 直接以错误结束，用 `asResult` 把结果当值处理。
- 自己写的 awaiter 可以经 `await_completion()` 报告出错或已停止。

第 6～10 步另写计划：

- 计划（二）：`JoinHandle`、`whenAll`、`AsyncGenerator`、`fromCallback` 接上 `await_completion()`；已停止的公开产生方 `just_stopped`、`stopIfRequested`、`unstoppable`；`scheduleOn` 成为取消点。
- 计划（三）：禁用异常的构建（`CO2_HAS_EXCEPTIONS`、CMake 选项、CI）；文档收尾。

它们的代码依赖本计划落地后的具体接口，等本计划做完再写。

本计划里的全部代码都在仓库的一份副本上按任务顺序实现并跑通过：有测试的任务（2～7）都先确认只加测试时失败、再确认加上实现后通过；最终状态在 13 个配置下通过全部 28 个测试（GCC 13 与 Clang 18 的 Debug / Release、C++14 / 17 / 20 / 23、libc++、ASan + UBSan、TSan）。MSVC 没有验证。

## 文件一览

| 文件 | 任务 | 职责 |
| --- | --- | --- |
| `include/co2/optional.hpp`（新） | 2 | `co2::optional<T>`、`co2::nullopt` |
| `include/co2/error.hpp`（新） | 2 | `co2::Errc`、`co2::Error` |
| `include/co2/completion.hpp`（新） | 2 | `co2::Completion` |
| `include/co2/result.hpp`（新） | 2 | `co2::Unit`、`co2::Result<T>` |
| `include/co2/just.hpp`（新） | 6 | `co2::just_error`、`co2::with_error` |
| `include/co2/as_result.hpp`（新） | 7 | `co2::asResult` |
| `include/co2/detail/awaitable.hpp` | 4 | 探测 `await_completion()`；`AwaiterRef` 转发它 |
| `include/co2/detail/frame.hpp` | 4 | 恢复点的收尾逻辑；探测 promise 的两个入口 |
| `include/co2/dsl.hpp` | 4 | 恢复点多出的一步 |
| `include/co2/task.hpp` | 3、5、6、7 | 根的返回类型；四态结果；`yield_value(WithError)`；`takeOutcome` |
| `include/co2/sync_wait.hpp` | 3、7 | 返回 optional；`asResult(Task)` 的重载 |
| `include/co2/spawn.hpp` | 3 | `join()` 返回 optional |
| `include/co2/co2.hpp` | 2、6、7 | 总头文件加上新头 |
| `tests/sample_errc.hpp`（新） | 2 | 测试共用的错误枚举 |
| `tests/vocabulary_tests.cpp`（新） | 2 | 词汇类型的测试 |
| `tests/completion_tests.cpp`（新） | 4、5、6、7 | 三种完成方式的测试 |
| `tests/*_contract_violation_test.cpp`（新，4 个） | 2、4、5 | 新增的契约违规 |
| `tests/CMakeLists.txt` | 2、4、5 | 登记新测试 |
| 其余 `tests/*.cpp`、`benchmarks/co2_benchmarks.cpp` | 1、3、7 | 新基准；适应返回 optional 的根 |
| `README.md`、`docs/` | 1、3、8 | 文档 |

## 环境与命令

**构建与测试。** 每个任务的"构建并运行全部测试"都指下面三条命令（第一条只需要在第一次、以及新增了测试文件之后执行）：

```bash
cmake -S . -B /tmp/co2-build/gcc14 -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=g++
cmake --build /tmp/co2-build/gcc14 -j 12
ctest --test-dir /tmp/co2-build/gcc14 -j 8
```

单独运行一个测试程序（失败时它会打印出错的行号）：

```bash
/tmp/co2-build/gcc14/tests/co2-completion-tests
```

**应用代码改动。** 新文件给出完整内容。对已有文件的修改以 `diff` 块给出，它是 `git diff` 的原样输出：可以照着手工修改，也可以把块里的内容存成文件后执行 `git apply <文件>`。

**这台机器上的注意事项。**

- 沙箱里 `kill` 被禁止，`timeout` 与 `ctest --timeout` 杀不掉卡住的进程。需要限时的时候用一个基于 `alarm` 的小包装：

  ```c
  /* /tmp/co2-build/runlimit.c —— cc -O1 runlimit.c -o runlimit；用法：runlimit <秒> <命令...> */
  #include <stdlib.h>
  #include <unistd.h>
  int main(int argc, char** argv) {
      if (argc < 3) return 2;
      alarm((unsigned)atoi(argv[1]));
      execvp(argv[2], argv + 2);
      return 127;
  }
  ```

- 沙箱里 LeakSanitizer 不能工作：跑 ASan 配置时设 `ASAN_OPTIONS=detect_leaks=0`。
- TSan 需要关闭地址随机化：`setarch $(uname -m) -R ctest ...`。
- `git commit` 需要 `git_write` 权限；提交命令里只放 `git add` 与 `git commit`，不要和别的命令捆在一起。

---

### Task 1: 基准——失败沿 Task 链每层的成本

设计稿第 10 节第 1 步。先把对照数字记下来：之后的改动要把它降下来。

**Files:**
- Modify: `benchmarks/co2_benchmarks.cpp`
- Modify: `README.md`（性能基准表）

**Interfaces:**
- Consumes: 基准程序里已有的 `nanosecondsPerOp(std::size_t ops, Body&&)`、`sink`、`benchmarks[]`；`co2::syncWait(co2::Task<int>)`（此时返回 `int`，任务 3 之后返回 `co2::optional<int>`，这里只把它当语句用，两种都能编译）。
- Produces: 基准行 `"Task failure through a 100-deep chain, per layer"`（函数 `taskFailureThroughChain()`、协程 `failsAtDepth(int depth)`）。任务 5 与任务 8 用它对比前后。

- [ ] **Step 1: 加入基准**

修改 `benchmarks/co2_benchmarks.cpp`：

````diff
diff --git a/benchmarks/co2_benchmarks.cpp b/benchmarks/co2_benchmarks.cpp
index 3acd321..817a9b0 100644
--- a/benchmarks/co2_benchmarks.cpp
+++ b/benchmarks/co2_benchmarks.cpp
@@ -12,6 +12,7 @@
 #include <cstdio>
 #include <cstring>
 #include <exception>
+#include <stdexcept>
 #include <string>
 #include <thread>
 #include <utility>
@@ -145,6 +146,30 @@ double syncWaitReadyTask() {
     });
 }
 
+// 失败沿等待链向上传：叶子失败，根上接住。报告的是每经过一层的成本。
+auto failsAtDepth(int depth) CO2_BEG(co2::Task<int>, (depth), int v{};) {
+    if (depth == 0) throw std::runtime_error{"leaf"};
+    CO2_AWAIT_SET(v, failsAtDepth(depth - 1));
+    CO2_RETURN(v + 1);
+}
+CO2_END
+
+double taskFailureThroughChain() {
+    constexpr int depth = 100;
+    constexpr int rounds = 5000;
+    return nanosecondsPerOp(static_cast<std::size_t>(depth) * rounds, [&] {
+        long caught = 0;
+        for (auto round = 0; round != rounds; ++round) {
+            try {
+                co2::syncWait(failsAtDepth(depth));
+            } catch (std::runtime_error const&) {
+                ++caught;
+            }
+        }
+        sink = caught;
+    });
+}
+
 // 多个线程各自跑一条"等待子 Task"的循环。sharedToken 为真时所有线程的 Task 树共用同一个
 // stop_token——whenAll / spawn 的各个分支落在不同线程上时就是这种形态。
 double awaitChildOnThreads(int const threads, bool const sharedToken) {
@@ -230,6 +255,7 @@ Benchmark const benchmarks[] = {
     {"elements_of depth 8 yield/next", &elementsOfDepth8},
     {"symmetric Task transfer (with frame allocation)", &symmetricTaskTransfer},
     {"syncWait ready Task", &syncWaitReadyTask},
+    {"Task failure through a 100-deep chain, per layer", &taskFailureThroughChain},
     {"Task await child, 8 threads, own stop_token", &awaitChildOwnToken},
     {"Task await child, 8 threads, shared stop_token", &awaitChildSharedToken},
     {"ManualExecutor hop", &manualExecutorHop},
````

- [ ] **Step 2: 构建并运行这一行**

```bash
cmake -S . -B /tmp/co2-build/bench -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++ \
      -DCMAKE_CXX_FLAGS_RELEASE="-O2 -DNDEBUG" -DCO2_BUILD_BENCHMARKS=ON -DCO2_BUILD_TESTS=OFF
cmake --build /tmp/co2-build/bench -j 12
/tmp/co2-build/bench/benchmarks/co2-benchmarks failure
```

Expected: 一行 `Task failure through a 100-deep chain, per layer`，中位数在 550～570 ns 之间（这台机器上是 558）。每一层都在 `await_resume()` 里重抛一次异常，所以是几百纳秒的量级。

- [ ] **Step 3: 把数字记进 README**

修改 `README.md`：

````diff
diff --git a/README.md b/README.md
index 7c5015b..ccb916c 100644
--- a/README.md
+++ b/README.md
@@ -317,6 +317,7 @@ i7-13700KF，每项 7 轮取中位数：
 | elements_of 深度 8 yield/next | 1.9 |
 | symmetric Task transfer（含帧分配） | 11.9 |
 | syncWait ready Task | 25.4 |
+| Task 链上的失败，100 层，每层 | 558 |
 | Task 等待子 Task，8 线程各自的 stop_token | 19.3 |
 | Task 等待子 Task，8 线程共享一个 stop_token | 18.2 |
 | ManualExecutor hop | 20.1 |
````

如果 Step 2 量到的中位数与 558 相差超过 5%，写量到的数字。

- [ ] **Step 4: 提交**

```bash
git add benchmarks/co2_benchmarks.cpp README.md
git commit -F - <<'EOF'
基准：失败沿 Task 链向上传时每层的成本

新增一行基准：100 层的 Task<int> 链，最里面一层抛异常，根上接住，报告每经过一层
的耗时。今天是 558 ns（GCC 13.3 -O2）：每一层都要在 await_resume() 里重抛一次。

这一行先于"错误按值传播"的改动加入，作为对照
（docs/design/completion-channels.md 第 10 节第 1 步）。
EOF
```

### Task 2: 词汇类型

设计稿 2.1 节、第 10 节第 2 步。只加类型和测试，不改任何现有行为。

**Files:**
- Create: `include/co2/optional.hpp`、`include/co2/error.hpp`、`include/co2/completion.hpp`、`include/co2/result.hpp`
- Create: `tests/sample_errc.hpp`、`tests/vocabulary_tests.cpp`
- Create: `tests/optional_empty_value_contract_violation_test.cpp`、`tests/error_from_success_code_contract_violation_test.cpp`
- Modify: `tests/CMakeLists.txt`、`include/co2/co2.hpp`

**Interfaces:**
- Consumes: `CO2_CONTRACT_CHECK`（`co2/contract.hpp`）。
- Produces（后面的任务按这些名字和签名使用）:
  - `co2::nullopt_t`、`co2::nullopt`；`template <class T> struct co2::optional`，成员 `has_value()`、`explicit operator bool`、`operator*`、`operator->`、`value()`、`value_or(U&&)`、`reset()`、`emplace(Args&&...)`，以及与 `optional` / `nullopt` / 值的六种比较。空的时候 `value()`、`operator*`、`operator->` 是契约违规。
  - `enum class co2::Errc { unhandled_exception = 1 }`；`std::error_code co2::make_error_code(co2::Errc) noexcept`。
  - `struct co2::Error`：可由 `std::error_code`（隐式）、任何 `std::is_error_code_enum` 的枚举（隐式）、`std::exception_ptr`（`explicit`）构造；`std::error_code code() const noexcept`；`std::exception_ptr exception() const noexcept`；`[[noreturn]] void raise() const`。
  - `struct co2::Completion`：`static Completion value() noexcept`、`static Completion stopped() noexcept`、`static Completion error(Error) noexcept`；`bool isValue() / isStopped() / hasError() const noexcept`；`Error const& error() const& noexcept`、`Error&& error() && noexcept`。
  - `struct co2::Unit`；`template <class T> struct co2::Result`（`T` 可以是 `void`）：可由 `T`、`Error`、错误码枚举隐式构造；`static Result stopped() noexcept`；`hasValue() / hasError() / isStopped()`、`explicit operator bool`、`value()`、`Error const& error() const noexcept`。
  - 测试共用：`tests/sample_errc.hpp` 里的 `sample::Errc { retry_later = 1, gone = 2 }`。

- [ ] **Step 1: 写测试**

新建 `tests/sample_errc.hpp`：

````cpp
#pragma once

// 测试用的错误枚举，按 <system_error> 的常规方式接入：一个错误类别、ADL 能找到的
// make_error_code、以及 std::is_error_code_enum 的特化。使用者自己的错误枚举也是
// 这样接入 co2::Error 的。

#include <string>
#include <system_error>

namespace sample {

enum class Errc { retry_later = 1, gone = 2 };

struct Category final : std::error_category {
    char const* name() const noexcept override { return "sample"; }

    std::string message(int const value) const override {
        return value == static_cast<int>(Errc::retry_later) ? "retry later" : "gone";
    }
};

inline std::error_category const& category() noexcept {
    static Category const instance{};
    return instance;
}

inline std::error_code make_error_code(Errc const code) noexcept {
    return std::error_code{static_cast<int>(code), category()};
}

} // namespace sample

namespace std {
template <> struct is_error_code_enum<sample::Errc> : true_type {};
} // namespace std
````

新建 `tests/vocabulary_tests.cpp`：

````cpp
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

bool acceptsError(co2::Error const& error, std::error_code const expected) {
    return error.code() == expected;
}

void anErrorHoldsACode() {
    co2::Error const error = std::make_error_code(std::errc::timed_out);
    CHECK(error.code() == std::errc::timed_out);
    CHECK(error.exception() == nullptr);
}

void anErrorConvertsFromAnErrorCodeEnum() {
    co2::Error const error = sample::Errc::retry_later;
    CHECK(error.code() == sample::Errc::retry_later);
    CHECK(error.code().message() == "retry later");
    CHECK(acceptsError(sample::Errc::gone, sample::Errc::gone));
}

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
    anErrorHoldsAnException();
    raiseRethrowsTheOriginalExceptionOrThrowsSystemError();
    errorsCopyMoveAndAssignAcrossBothKinds();
    aCompletionIsExactlyOneOfThreeKinds();
    aResultIsAValueAnErrorOrStopped();
    aResultOwnsExactlyOneValue();
    aResultHoldsMoveOnlyValuesAndVoid();
    unitIsAnEmptyValue();
    return 0;
}
````

新建 `tests/optional_empty_value_contract_violation_test.cpp`：

````cpp
#include <atomic>
#include <cstdlib>

#include "co2/contract.hpp"
#include "co2/optional.hpp"

namespace {

std::atomic<bool> atExpectedFailurePoint{};

void contractViolationHandler(co2::ContractViolation const&) {
    std::_Exit(atExpectedFailurePoint.load(std::memory_order_acquire) ? 86 : 87);
}

} // namespace

// 空的 co2::optional 上取值是契约违规。std::optional 在这里抛 bad_optional_access；co2
// 不为前置条件抛异常，开不开异常行为相同。
int main() {
    co2::setContractViolationHandler(&contractViolationHandler);

    co2::optional<int> engaged = 1;
    if (engaged.value() != 1) return EXIT_FAILURE;

    auto const empty = co2::optional<int>{};
    atExpectedFailurePoint.store(true, std::memory_order_release);
    static_cast<void>(empty.value());
    return EXIT_FAILURE;
}
````

新建 `tests/error_from_success_code_contract_violation_test.cpp`：

````cpp
#include <atomic>
#include <cstdlib>
#include <system_error>

#include "co2/contract.hpp"
#include "co2/error.hpp"

namespace {

std::atomic<bool> atExpectedFailurePoint{};

void contractViolationHandler(co2::ContractViolation const&) {
    std::_Exit(atExpectedFailurePoint.load(std::memory_order_acquire) ? 86 : 87);
}

} // namespace

// "成功"不是一种错误：用值为零的 std::error_code 构造 co2::Error 是契约违规。
int main() {
    co2::setContractViolationHandler(&contractViolationHandler);

    co2::Error const failure = std::make_error_code(std::errc::timed_out);
    if (failure.code() != std::errc::timed_out) return EXIT_FAILURE;

    atExpectedFailurePoint.store(true, std::memory_order_release);
    co2::Error const success = std::error_code{};
    static_cast<void>(success);
    return EXIT_FAILURE;
}
````

修改 `tests/CMakeLists.txt`：

````diff
diff --git a/tests/CMakeLists.txt b/tests/CMakeLists.txt
index 1dad38f..a79dd99 100644
--- a/tests/CMakeLists.txt
+++ b/tests/CMakeLists.txt
@@ -10,7 +10,8 @@ set(co2_test_sources
     when_all_tests.cpp
     callback_tests.cpp
     allocation_tests.cpp
-    edge_case_tests.cpp)
+    edge_case_tests.cpp
+    vocabulary_tests.cpp)
 
 # 契约违规测试：程序在预期的失败点经由契约处理器以退出码 86 结束。
 set(co2_contract_violation_test_sources
@@ -21,7 +22,9 @@ set(co2_contract_violation_test_sources
     resume_running_contract_violation_test.cpp
     thread_pool_zero_threads_contract_violation_test.cpp
     async_generator_destroy_contract_violation_test.cpp
-    continuation_twice_contract_violation_test.cpp)
+    continuation_twice_contract_violation_test.cpp
+    optional_empty_value_contract_violation_test.cpp
+    error_from_success_code_contract_violation_test.cpp)
 
 # 构建测试用的 C++ 标准。库的最低要求是 14，默认就测它；CI 另以 17 / 20 / 23 各跑一遍，
 # 保证较新的标准下（弃用告警、更严格的规则）同样干净。
````

- [ ] **Step 2: 构建，确认失败**

构建并运行全部测试（见"环境与命令"；新增了测试文件，要重新执行配置那一条）。

Expected: 构建失败，三个新测试各报一条 `fatal error`，说找不到 `co2/optional.hpp`、`co2/completion.hpp`、`co2/error.hpp`。

- [ ] **Step 3: 实现 `co2::optional`**

新建 `include/co2/optional.hpp`：

````cpp
#pragma once

#include <new>
#include <type_traits>
#include <utility>

#include "co2/config.hpp"
#include "co2/contract.hpp"

// co2::optional<T>：std::optional 的 C++14 子集。根（syncWait、JoinHandle::join）用它
// 表示"有值 / 已停止"。C++17 起也不换成 std::optional 的别名：头文件在各个标准下的定义
// 必须一致（与 co2::stop_token 相同）。
//
// 与 std::optional 的差别：
//   - 空的时候 value()、operator* 与 operator-> 是契约违规。std 里 value() 抛
//     bad_optional_access、后两者是未定义行为；这里统一，开不开异常行为相同。
//   - 赋值是"先销毁再构造"，不走 T 的赋值运算符；构造抛出时留下空的 optional。
//   - 不是 constexpr，不保留 T 的平凡性；没有 in_place 构造、optional<U> 的转换构造、
//     make_optional、hash 与 swap。

namespace co2 {

struct nullopt_t {
    explicit constexpr nullopt_t(int) noexcept {}
};

// 命名空间作用域的 constexpr 对象有内部链接；它只按值传递，不会被 odr-use。
constexpr nullopt_t nullopt{0};

template <class T> struct optional;

namespace detail {

template <class T> struct IsOptional : std::false_type {};
template <class T> struct IsOptional<optional<T>> : std::true_type {};

// 能与 optional<T> 做"值比较"的另一侧：不是 optional，也不是 nullopt_t。
template <class U>
using ComparableValue = typename std::enable_if<
    not IsOptional<U>::value && not std::is_same<U, nullopt_t>::value, int>::type;

} // namespace detail

template <class T> struct optional {
    static_assert(std::is_object<T>::value && not std::is_array<T>::value,
                  "co2::optional<T> requires a non-array object type");
    static_assert(not std::is_const<T>::value && not std::is_volatile<T>::value,
                  "co2::optional<T> cannot hold cv-qualified objects");
    static_assert(not std::is_same<T, nullopt_t>::value,
                  "co2::optional<nullopt_t> is not allowed");

    using value_type = T;

    optional() noexcept {}
    optional(nullopt_t) noexcept {}

    optional(optional const& other) {
        if (other.engaged) construct(other.get());
    }

    // 与 std 一致：被移动的一方仍然有值（一个被移走的 T）。
    optional(optional&& other) noexcept(std::is_nothrow_move_constructible<T>::value) {
        if (other.engaged) construct(std::move(other.get()));
    }

    template <class U = T,
              class = typename std::enable_if<
                  std::is_constructible<T, U&&>::value &&
                  not std::is_same<typename std::decay<U>::type, optional>::value &&
                  not std::is_same<typename std::decay<U>::type, nullopt_t>::value>::type>
    optional(U&& value) {
        construct(std::forward<U>(value));
    }

    optional& operator=(nullopt_t) noexcept {
        reset();
        return *this;
    }

    optional& operator=(optional const& other) {
        if (this == &other) return *this;
        reset();
        if (other.engaged) construct(other.get());
        return *this;
    }

    optional&
    operator=(optional&& other) noexcept(std::is_nothrow_move_constructible<T>::value) {
        if (this == &other) return *this;
        reset();
        if (other.engaged) construct(std::move(other.get()));
        return *this;
    }

    template <class U = T,
              class = typename std::enable_if<
                  std::is_constructible<T, U&&>::value &&
                  not std::is_same<typename std::decay<U>::type, optional>::value &&
                  not std::is_same<typename std::decay<U>::type, nullopt_t>::value>::type>
    optional& operator=(U&& value) {
        reset();
        construct(std::forward<U>(value));
        return *this;
    }

    ~optional() { reset(); }

    bool has_value() const noexcept { return engaged; }
    explicit operator bool() const noexcept { return engaged; }

    T& operator*() & noexcept { return checked(); }
    T const& operator*() const& noexcept { return checked(); }
    T&& operator*() && noexcept { return std::move(checked()); }

    T* operator->() noexcept { return &checked(); }
    T const* operator->() const noexcept { return &checked(); }

    T& value() & noexcept { return checked(); }
    T const& value() const& noexcept { return checked(); }
    T&& value() && noexcept { return std::move(checked()); }

    template <class U> T value_or(U&& fallback) const& {
        return engaged ? get() : static_cast<T>(std::forward<U>(fallback));
    }

    template <class U> T value_or(U&& fallback) && {
        return engaged ? std::move(get()) : static_cast<T>(std::forward<U>(fallback));
    }

    void reset() noexcept {
        if (not engaged) return;
        engaged = false;
        get().~T();
    }

    template <class... Args> T& emplace(Args&&... args) {
        reset();
        return construct(std::forward<Args>(args)...);
    }

  private:
    // 圆括号是有意的：泛型 T 上花括号可能选中 initializer_list 构造函数。
    template <class... Args> T& construct(Args&&... args) {
        auto* const object =
            ::new (static_cast<void*>(bytes)) T(std::forward<Args>(args)...);
        engaged = true;
        return *object;
    }

    T& get() noexcept { return *reinterpret_cast<T*>(bytes); }
    T const& get() const noexcept { return *reinterpret_cast<T const*>(bytes); }

    T& checked() noexcept {
        CO2_CONTRACT_CHECK(engaged);
        return get();
    }

    T const& checked() const noexcept {
        CO2_CONTRACT_CHECK(engaged);
        return get();
    }

    alignas(T) unsigned char bytes[sizeof(T)];
    bool engaged{};
};

// ---- 比较：语义与 std::optional 相同，空的 optional 小于任何有值的 ----

template <class T, class U>
bool operator==(optional<T> const& left, optional<U> const& right) {
    if (left.has_value() != right.has_value()) return false;
    return not left.has_value() || *left == *right;
}

template <class T, class U>
bool operator!=(optional<T> const& left, optional<U> const& right) {
    if (left.has_value() != right.has_value()) return true;
    return left.has_value() && *left != *right;
}

template <class T, class U>
bool operator<(optional<T> const& left, optional<U> const& right) {
    return right.has_value() && (not left.has_value() || *left < *right);
}

template <class T, class U>
bool operator>(optional<T> const& left, optional<U> const& right) {
    return left.has_value() && (not right.has_value() || *left > *right);
}

template <class T, class U>
bool operator<=(optional<T> const& left, optional<U> const& right) {
    return not left.has_value() || (right.has_value() && *left <= *right);
}

template <class T, class U>
bool operator>=(optional<T> const& left, optional<U> const& right) {
    return not right.has_value() || (left.has_value() && *left >= *right);
}

template <class T> bool operator==(optional<T> const& value, nullopt_t) noexcept {
    return not value.has_value();
}
template <class T> bool operator==(nullopt_t, optional<T> const& value) noexcept {
    return not value.has_value();
}
template <class T> bool operator!=(optional<T> const& value, nullopt_t) noexcept {
    return value.has_value();
}
template <class T> bool operator!=(nullopt_t, optional<T> const& value) noexcept {
    return value.has_value();
}
template <class T> bool operator<(optional<T> const&, nullopt_t) noexcept {
    return false;
}
template <class T> bool operator<(nullopt_t, optional<T> const& value) noexcept {
    return value.has_value();
}
template <class T> bool operator>(optional<T> const& value, nullopt_t) noexcept {
    return value.has_value();
}
template <class T> bool operator>(nullopt_t, optional<T> const&) noexcept {
    return false;
}
template <class T> bool operator<=(optional<T> const& value, nullopt_t) noexcept {
    return not value.has_value();
}
template <class T> bool operator<=(nullopt_t, optional<T> const&) noexcept {
    return true;
}
template <class T> bool operator>=(optional<T> const&, nullopt_t) noexcept {
    return true;
}
template <class T> bool operator>=(nullopt_t, optional<T> const& value) noexcept {
    return not value.has_value();
}

template <class T, class U, detail::ComparableValue<U> = 0>
bool operator==(optional<T> const& left, U const& right) {
    return left.has_value() && *left == right;
}
template <class T, class U, detail::ComparableValue<U> = 0>
bool operator==(U const& left, optional<T> const& right) {
    return right.has_value() && left == *right;
}
template <class T, class U, detail::ComparableValue<U> = 0>
bool operator!=(optional<T> const& left, U const& right) {
    return not left.has_value() || *left != right;
}
template <class T, class U, detail::ComparableValue<U> = 0>
bool operator!=(U const& left, optional<T> const& right) {
    return not right.has_value() || left != *right;
}
template <class T, class U, detail::ComparableValue<U> = 0>
bool operator<(optional<T> const& left, U const& right) {
    return not left.has_value() || *left < right;
}
template <class T, class U, detail::ComparableValue<U> = 0>
bool operator<(U const& left, optional<T> const& right) {
    return right.has_value() && left < *right;
}
template <class T, class U, detail::ComparableValue<U> = 0>
bool operator>(optional<T> const& left, U const& right) {
    return left.has_value() && *left > right;
}
template <class T, class U, detail::ComparableValue<U> = 0>
bool operator>(U const& left, optional<T> const& right) {
    return not right.has_value() || left > *right;
}
template <class T, class U, detail::ComparableValue<U> = 0>
bool operator<=(optional<T> const& left, U const& right) {
    return not left.has_value() || *left <= right;
}
template <class T, class U, detail::ComparableValue<U> = 0>
bool operator<=(U const& left, optional<T> const& right) {
    return right.has_value() && left <= *right;
}
template <class T, class U, detail::ComparableValue<U> = 0>
bool operator>=(optional<T> const& left, U const& right) {
    return left.has_value() && *left >= right;
}
template <class T, class U, detail::ComparableValue<U> = 0>
bool operator>=(U const& left, optional<T> const& right) {
    return not right.has_value() || left >= *right;
}

} // namespace co2
````

- [ ] **Step 4: 实现 `co2::Error`**

新建 `include/co2/error.hpp`：

````cpp
#pragma once

#include <exception>
#include <new>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>

#include "co2/config.hpp"
#include "co2/contract.hpp"

// co2::Error：一次失败。它要么是一个 std::error_code，要么是一个被捕获的异常
// （std::exception_ptr）。不分配，不需要 RTTI；错误码的两个字段与 exception_ptr 共用
// 存储，所以在 exception_ptr 只有一个指针宽的平台上它与 std::error_code 一样大。
//
// 用户自己的错误枚举按 <system_error> 的常规方式接入（std::is_error_code_enum 的特化
// 加上 ADL 能找到的 make_error_code），之后可以直接隐式转换成 Error。

namespace co2 {

// co2 自己的错误类别。Error 装着异常时，code() 报告 unhandled_exception。
enum class Errc { unhandled_exception = 1 };

namespace detail {

struct ErrorCategory final : std::error_category {
    char const* name() const noexcept override { return "co2"; }

    std::string message(int const value) const override {
        if (value == static_cast<int>(Errc::unhandled_exception))
            return "unhandled exception";
        return "unknown co2 error";
    }
};

inline std::error_category const& errorCategory() noexcept {
    static ErrorCategory const instance{};
    return instance;
}

} // namespace detail

inline std::error_code make_error_code(Errc const code) noexcept {
    return std::error_code{static_cast<int>(code), detail::errorCategory()};
}

} // namespace co2

namespace std {
template <> struct is_error_code_enum<co2::Errc> : true_type {};
} // namespace std

namespace co2 {

struct Error {
    // 前置条件：code 表示失败（非零）。"成功"不是一种错误。
    Error(std::error_code const code) noexcept
        : category{&code.category()}, number{code.value()}, kind{Kind::code} {
        CO2_CONTRACT_CHECK(static_cast<bool>(code));
    }

    // 错误码枚举一步转换成 Error。枚举先转 std::error_code 再转 Error 是两次用户定义
    // 转换，隐式转换走不通，所以需要这个模板——std::error_code 自己也是这样做的。
    template <class Enum, class = typename std::enable_if<
                              std::is_error_code_enum<Enum>::value>::type>
    Error(Enum const code) noexcept : Error{std::error_code{code}} {}

    // 前置条件：exception 非空。
    explicit Error(std::exception_ptr exception) noexcept
        : number{0}, kind{Kind::exception} {
        CO2_CONTRACT_CHECK(exception != nullptr);
        ::new (static_cast<void*>(&captured)) std::exception_ptr{std::move(exception)};
    }

    Error(Error const& other) noexcept : number{other.number}, kind{other.kind} {
        if (kind == Kind::exception)
            ::new (static_cast<void*>(&captured)) std::exception_ptr{other.captured};
        else
            category = other.category;
    }

    // 被移动的一方只保证还能析构和赋值，内容不再有意义。
    Error(Error&& other) noexcept : number{other.number}, kind{other.kind} {
        if (kind == Kind::exception)
            ::new (static_cast<void*>(&captured))
                std::exception_ptr{std::move(other.captured)};
        else
            category = other.category;
    }

    Error& operator=(Error other) noexcept {
        this->~Error();
        ::new (static_cast<void*>(this)) Error{std::move(other)};
        return *this;
    }

    ~Error() {
        if (kind == Kind::exception) captured.~exception_ptr();
    }

    // 装着异常时返回 Errc::unhandled_exception。
    std::error_code code() const noexcept {
        if (kind == Kind::exception) return make_error_code(Errc::unhandled_exception);
        return std::error_code{number, *category};
    }

    // 装着错误码时为空。
    std::exception_ptr exception() const noexcept {
        return kind == Kind::exception ? captured : std::exception_ptr{};
    }

    // 变回异常：装着异常就重抛原异常，装着错误码就抛 std::system_error。
    [[noreturn]] void raise() const {
        if (kind == Kind::exception) std::rethrow_exception(captured);
        throw std::system_error{code()};
    }

  private:
    enum class Kind : unsigned { code, exception };

    union {
        std::error_category const* category;
        std::exception_ptr captured;
    };
    int number;
    Kind kind;
};

} // namespace co2
````

- [ ] **Step 5: 实现 `co2::Completion`**

新建 `include/co2/completion.hpp`：

````cpp
#pragma once

#include <new>
#include <utility>

#include "co2/config.hpp"
#include "co2/contract.hpp"
#include "co2/error.hpp"

// co2::Completion：一次等待是怎么结束的——有值、出错、还是已停止。它是 Result<T> 去掉
// 值的那一部分：awaiter 的可选成员 await_completion() 返回它，恢复点据此决定是继续
// 执行（调用 await_resume() 取值）还是就此结束协程。

namespace co2 {

struct Completion {
    static Completion value() noexcept { return Completion{Kind::value}; }
    static Completion stopped() noexcept { return Completion{Kind::stopped}; }

    static Completion error(Error failure) noexcept {
        auto completion = Completion{Kind::error};
        ::new (static_cast<void*>(&completion.failure)) Error{std::move(failure)};
        return completion;
    }

    Completion(Completion const& other) noexcept : kind{other.kind} {
        if (kind == Kind::error)
            ::new (static_cast<void*>(&failure)) Error{other.failure};
    }

    Completion(Completion&& other) noexcept : kind{other.kind} {
        if (kind == Kind::error)
            ::new (static_cast<void*>(&failure)) Error{std::move(other.failure)};
    }

    Completion& operator=(Completion other) noexcept {
        this->~Completion();
        ::new (static_cast<void*>(this)) Completion{std::move(other)};
        return *this;
    }

    ~Completion() {
        if (kind == Kind::error) failure.~Error();
    }

    bool isValue() const noexcept { return kind == Kind::value; }
    bool isStopped() const noexcept { return kind == Kind::stopped; }
    bool hasError() const noexcept { return kind == Kind::error; }

    // 前置条件：hasError()。
    Error const& error() const& noexcept {
        CO2_CONTRACT_CHECK(kind == Kind::error);
        return failure;
    }

    Error&& error() && noexcept {
        CO2_CONTRACT_CHECK(kind == Kind::error);
        return std::move(failure);
    }

  private:
    enum class Kind : unsigned char { value, stopped, error };

    explicit Completion(Kind const kind_) noexcept : kind{kind_} {}

    Kind kind;
    union {
        Error failure;
    };
};

} // namespace co2
````

- [ ] **Step 6: 实现 `co2::Result` 与 `co2::Unit`**

新建 `include/co2/result.hpp`：

````cpp
#pragma once

#include <new>
#include <type_traits>
#include <utility>

#include "co2/config.hpp"
#include "co2/contract.hpp"
#include "co2/error.hpp"

// co2::Result<T>：一个操作的三种结果之一——值、错误（co2::Error）、已停止。
// CO2_AWAIT_SET(r, co2::asResult(x)) 得到它；普通的 CO2_AWAIT 不产生它，而是把错误与
// 已停止向上传播。T 可以是 void。
//
// co2::Unit：空的值类型。Task<void> 的根返回 co2::optional<co2::Unit>，与非 void 的根
// 同形。

namespace co2 {

struct Unit {
    friend bool operator==(Unit, Unit) noexcept { return true; }
    friend bool operator!=(Unit, Unit) noexcept { return false; }
};

template <class T> struct Result {
    static_assert(std::is_object<T>::value && not std::is_array<T>::value,
                  "co2::Result<T> requires void or a non-array object type");
    static_assert(not std::is_const<T>::value && not std::is_volatile<T>::value,
                  "co2::Result<T> cannot hold cv-qualified objects");
    static_assert(not std::is_same<T, Error>::value,
                  "co2::Result<co2::Error> is not allowed: its value and its error "
                  "would be indistinguishable");

    using value_type = T;

    // 圆括号是有意的：泛型 T 上花括号可能选中 initializer_list 构造函数。
    Result(T value) noexcept(std::is_nothrow_move_constructible<T>::value)
        : state{State::value} {
        ::new (static_cast<void*>(&storage.value)) T(std::move(value));
    }

    Result(Error error) noexcept : state{State::error} {
        ::new (static_cast<void*>(&storage.error)) Error{std::move(error)};
    }

    // 错误码枚举直接构造出"错误"，理由同 Error 的枚举构造函数。
    template <class Enum, class = typename std::enable_if<
                              std::is_error_code_enum<Enum>::value>::type>
    Result(Enum const code) noexcept : Result{Error{code}} {}

    static Result stopped() noexcept { return Result{Stopped{}}; }

    Result(Result const& other) : state{other.state} {
        if (state == State::value)
            ::new (static_cast<void*>(&storage.value)) T(other.storage.value);
        else if (state == State::error)
            ::new (static_cast<void*>(&storage.error)) Error{other.storage.error};
    }

    Result(Result&& other) noexcept(std::is_nothrow_move_constructible<T>::value)
        : state{other.state} {
        if (state == State::value)
            ::new (static_cast<void*>(&storage.value)) T(std::move(other.storage.value));
        else if (state == State::error)
            ::new (static_cast<void*>(&storage.error))
                Error{std::move(other.storage.error)};
    }

    // 先销毁再构造。T 的移动构造抛出时本对象变为已停止，而不是留下一个半死的值。
    Result& operator=(Result other) noexcept(std::is_nothrow_move_constructible<T>::value) {
        destroy();
        state = State::stopped;
        if (other.state == State::value)
            ::new (static_cast<void*>(&storage.value)) T(std::move(other.storage.value));
        else if (other.state == State::error)
            ::new (static_cast<void*>(&storage.error))
                Error{std::move(other.storage.error)};
        state = other.state;
        return *this;
    }

    ~Result() { destroy(); }

    bool hasValue() const noexcept { return state == State::value; }
    bool hasError() const noexcept { return state == State::error; }
    bool isStopped() const noexcept { return state == State::stopped; }
    explicit operator bool() const noexcept { return state == State::value; }

    // 前置条件：hasValue()。
    T& value() & noexcept {
        CO2_CONTRACT_CHECK(state == State::value);
        return storage.value;
    }

    T const& value() const& noexcept {
        CO2_CONTRACT_CHECK(state == State::value);
        return storage.value;
    }

    T&& value() && noexcept {
        CO2_CONTRACT_CHECK(state == State::value);
        return std::move(storage.value);
    }

    // 前置条件：hasError()。
    Error const& error() const noexcept {
        CO2_CONTRACT_CHECK(state == State::error);
        return storage.error;
    }

  private:
    struct Stopped {};
    enum class State : unsigned char { value, error, stopped };

    explicit Result(Stopped) noexcept : state{State::stopped} {}

    void destroy() noexcept {
        if (state == State::value)
            storage.value.~T();
        else if (state == State::error)
            storage.error.~Error();
    }

    union Storage {
        Storage() noexcept {}
        ~Storage() {}
        T value;
        Error error;
    } storage;
    State state;
};

template <> struct Result<void> {
    using value_type = void;

    Result() noexcept : state{State::value} {}

    Result(Error error) noexcept : state{State::error} {
        ::new (static_cast<void*>(&storage.error)) Error{std::move(error)};
    }

    template <class Enum, class = typename std::enable_if<
                              std::is_error_code_enum<Enum>::value>::type>
    Result(Enum const code) noexcept : Result{Error{code}} {}

    static Result stopped() noexcept { return Result{Stopped{}}; }

    Result(Result const& other) noexcept : state{other.state} {
        if (state == State::error)
            ::new (static_cast<void*>(&storage.error)) Error{other.storage.error};
    }

    Result(Result&& other) noexcept : state{other.state} {
        if (state == State::error)
            ::new (static_cast<void*>(&storage.error))
                Error{std::move(other.storage.error)};
    }

    Result& operator=(Result other) noexcept {
        destroy();
        state = other.state;
        if (state == State::error)
            ::new (static_cast<void*>(&storage.error))
                Error{std::move(other.storage.error)};
        return *this;
    }

    ~Result() { destroy(); }

    bool hasValue() const noexcept { return state == State::value; }
    bool hasError() const noexcept { return state == State::error; }
    bool isStopped() const noexcept { return state == State::stopped; }
    explicit operator bool() const noexcept { return state == State::value; }

    // 前置条件：hasValue()。
    void value() const noexcept { CO2_CONTRACT_CHECK(state == State::value); }

    // 前置条件：hasError()。
    Error const& error() const noexcept {
        CO2_CONTRACT_CHECK(state == State::error);
        return storage.error;
    }

  private:
    struct Stopped {};
    enum class State : unsigned char { value, error, stopped };

    explicit Result(Stopped) noexcept : state{State::stopped} {}

    void destroy() noexcept {
        if (state == State::error) storage.error.~Error();
    }

    union Storage {
        Storage() noexcept {}
        ~Storage() {}
        Error error;
    } storage;
    State state;
};

} // namespace co2
````

- [ ] **Step 7: 总头文件加上新头**

修改 `include/co2/co2.hpp`：

````diff
diff --git a/include/co2/co2.hpp b/include/co2/co2.hpp
index 29e31ad..44628c9 100644
--- a/include/co2/co2.hpp
+++ b/include/co2/co2.hpp
@@ -3,12 +3,16 @@
 // co2 的全部公共头。按需包含单个头文件同样可行，每个头都能独立包含。
 #include "co2/async_generator.hpp"
 #include "co2/callback.hpp"
+#include "co2/completion.hpp"
 #include "co2/config.hpp"
 #include "co2/contract.hpp"
 #include "co2/coroutine.hpp"
 #include "co2/env.hpp"
+#include "co2/error.hpp"
 #include "co2/generator.hpp"
 #include "co2/manual_executor.hpp"
+#include "co2/optional.hpp"
+#include "co2/result.hpp"
 #include "co2/scheduler.hpp"
 #include "co2/spawn.hpp"
 #include "co2/stop_token.hpp"
````

- [ ] **Step 8: 构建并运行全部测试，确认通过**

Expected: `100% tests passed, 0 tests failed out of 25`（原有 22 个，加上 `co2-vocabulary-tests` 与两个契约违规测试）。

- [ ] **Step 9: 提交**

```bash
git add include/co2/optional.hpp include/co2/error.hpp include/co2/completion.hpp \
        include/co2/result.hpp include/co2/co2.hpp tests/CMakeLists.txt \
        tests/sample_errc.hpp tests/vocabulary_tests.cpp \
        tests/optional_empty_value_contract_violation_test.cpp \
        tests/error_from_success_code_contract_violation_test.cpp
git commit -F - <<'EOF'
新增词汇类型：optional、Error、Completion、Result、Unit

三种完成方式（值 / 错误 / 已停止）要用到的类型。这一步只加类型和测试，不改任何
现有行为（docs/design/completion-channels.md 第 2.1 节、第 10 节第 2 步）。

- co2::optional<T>：std::optional 的 C++14 子集。空的时候取值是契约违规而不是抛
  异常，开不开异常行为相同。
- co2::Error：一个 std::error_code 或一个被捕获的异常，两者共用存储，GCC / Clang
  上与 error_code 一样是 16 字节。错误码枚举可以隐式转换成它：枚举先转
  error_code 再转 Error 是两次用户定义转换，所以单设了构造函数模板。
- co2::Completion：一次等待是怎么结束的。
- co2::Result<T>：值 / 错误 / 已停止三选一，T 可以是 void。
- co2::Unit：void 结果在根上的占位。

两条新的契约违规各有一个测试：空的 optional 上取值；用表示成功的 error_code 构造
Error。libc++ 的 exception_ptr 移动之后不置空，所以不对被移动的 Error 的内容做
任何保证。
EOF
```

### Task 3: 根返回 `co2::optional`

设计稿 2.4 节、第 6 节第 1 条、第 10 节第 3 步。不兼容改动：`syncWait` 与 `JoinHandle::join` 的返回类型从 `T` 变成 `co2::optional<T>`，`void` 变成 `co2::optional<co2::Unit>`。这一步里 optional 总是有值——纯粹的接口变更。

**Files:**
- Modify: `include/co2/task.hpp`、`include/co2/sync_wait.hpp`、`include/co2/spawn.hpp`
- Modify（新测试与调用点）: `tests/scheduler_tests.cpp`、`tests/spawn_tests.cpp`
- Modify（只有调用点）: `tests/allocation_tests.cpp`、`tests/callback_tests.cpp`、`tests/edge_case_tests.cpp`、`tests/thread_pool_tests.cpp`、`tests/when_all_tests.cpp`、`benchmarks/co2_benchmarks.cpp`
- Modify: `README.md`、`docs/architecture.md`

**Interfaces:**
- Consumes: `co2::optional<T>`、`co2::Unit`（任务 2）。
- Produces:
  - `template <class T> using co2::detail::RootResult`：`T` 非 void 时是 `co2::optional<T>`，void 时是 `co2::optional<co2::Unit>`。
  - `template <class T> static RootResult<T> co2::detail::TaskAccess::takeRootResult(Task<T>&)`。
  - `template <class T> detail::RootResult<T> co2::syncWait(Task<T>, stop_token = {})`；通用重载 `detail::RootResult<detail::AwaitResultOf<Awaitable>> co2::syncWait(Awaitable&&, stop_token = {})`。
  - `detail::RootResult<T> co2::JoinHandle<T>::join()`。

调用点的规则只有一条：**取用结果的地方在调用前加 `*`**（直接取成员的写成 `->`）；与值比较的（`co2::syncWait(x) == 42`）和不用结果的不必改。

- [ ] **Step 1: 写测试，并把取用结果的调用点改成新写法**

修改 `tests/scheduler_tests.cpp`、`tests/spawn_tests.cpp`：

````diff
diff --git a/tests/scheduler_tests.cpp b/tests/scheduler_tests.cpp
index bfe65b4..53a668c 100644
--- a/tests/scheduler_tests.cpp
+++ b/tests/scheduler_tests.cpp
@@ -7,11 +7,14 @@
 #include <stdexcept>
 #include <string>
 #include <thread>
+#include <type_traits>
 #include <utility>
 #include <vector>
 
 #include "co2/coroutine.hpp"
 #include "co2/manual_executor.hpp"
+#include "co2/optional.hpp"
+#include "co2/result.hpp"
 #include "co2/scheduler.hpp"
 #include "co2/sync_wait.hpp"
 #include "co2/task.hpp"
@@ -105,6 +108,28 @@ void syncWaitRunsASynchronousTaskInline() {
     CHECK(counter == 1);
 }
 
+// 根的返回值照 std::execution::sync_wait：结果放在 optional 里，void 用 Unit 占位。
+void syncWaitReturnsAnOptional() {
+    static_assert(
+        std::is_same<decltype(co2::syncWait(answer(7))), co2::optional<int>>::value,
+        "syncWait(Task<int>) must return co2::optional<int>");
+    static_assert(std::is_same<decltype(co2::syncWait(std::declval<co2::Task<>>())),
+                               co2::optional<co2::Unit>>::value,
+                  "syncWait(Task<void>) must return co2::optional<co2::Unit>");
+    static_assert(std::is_same<decltype(co2::syncWait(co2::suspend_never{})),
+                               co2::optional<co2::Unit>>::value,
+                  "syncWait(awaitable) must return an optional as well");
+
+    co2::optional<int> const value = co2::syncWait(answer(7));
+    CHECK(value.has_value());
+    CHECK(*value == 7);
+
+    int counter = 0;
+    co2::optional<co2::Unit> const done = co2::syncWait(nothing(counter));
+    CHECK(done.has_value());
+    CHECK(counter == 1);
+}
+
 void syncWaitRethrowsTheTaskException() {
     bool thrown = false;
     try {
@@ -121,7 +146,7 @@ auto uniqueAnswer() CO2_BEG(co2::Task<std::unique_ptr<int>>, ()) {
 CO2_END
 
 void syncWaitMovesOutMoveOnlyResults() {
-    auto result = co2::syncWait(uniqueAnswer());
+    auto result = *co2::syncWait(uniqueAnswer());
     CHECK(result != nullptr && *result == 3);
 }
 
@@ -176,6 +201,7 @@ int main() {
     scheduleOnSuspendsUntilTheExecutorRuns();
     manualExecutorIsFifoAndRunDrainsRescheduledWork();
     syncWaitRunsASynchronousTaskInline();
+    syncWaitReturnsAnOptional();
     syncWaitRethrowsTheTaskException();
     syncWaitMovesOutMoveOnlyResults();
     syncWaitWakesWhenTheTaskCompletesOnAnotherThread();
diff --git a/tests/spawn_tests.cpp b/tests/spawn_tests.cpp
index 9b7a659..d748e73 100644
--- a/tests/spawn_tests.cpp
+++ b/tests/spawn_tests.cpp
@@ -9,12 +9,15 @@
 #include <mutex>
 #include <stdexcept>
 #include <thread>
+#include <type_traits>
 #include <utility>
 
 #include "co2/coroutine.hpp"
 #include "co2/detail/result_storage.hpp"
 #include "co2/env.hpp"
 #include "co2/manual_executor.hpp"
+#include "co2/optional.hpp"
+#include "co2/result.hpp"
 #include "co2/scheduler.hpp"
 #include "co2/spawn.hpp"
 #include "co2/stop_token.hpp"
@@ -79,6 +82,26 @@ void spawnOnAThreadPoolJoinsFromTheCallingThread() {
     CHECK(counter.load() == 1);
 }
 
+// join() 的返回值与 syncWait 同形：结果放在 optional 里，void 用 Unit 占位。
+void joinReturnsAnOptional() {
+    co2::ManualExecutor executor;
+    std::atomic<int> counter{0};
+    auto value = co2::spawn(executor, answer(5));
+    auto done = co2::spawn(executor, touch(counter));
+    static_assert(std::is_same<decltype(value.join()), co2::optional<int>>::value,
+                  "JoinHandle<int>::join() must return co2::optional<int>");
+    static_assert(
+        std::is_same<decltype(done.join()), co2::optional<co2::Unit>>::value,
+        "JoinHandle<void>::join() must return co2::optional<co2::Unit>");
+    CHECK(executor.run() == 2U);
+
+    co2::optional<int> const joined = value.join();
+    CHECK(joined.has_value());
+    CHECK(*joined == 5);
+    CHECK(done.join().has_value());
+    CHECK(counter.load() == 1);
+}
+
 auto failing() CO2_BEG(co2::Task<int>, ()) {
     throw ExpectedError{};
     CO2_RETURN(0);
@@ -104,7 +127,7 @@ CO2_END
 
 void joinMovesOutMoveOnlyResults() {
     co2::ThreadPool pool{1U};
-    auto result = co2::spawn(pool, uniqueAnswer()).join();
+    auto result = *co2::spawn(pool, uniqueAnswer()).join();
     CHECK(result != nullptr && *result == 9);
 }
 
@@ -309,6 +332,7 @@ void joinHandlesAreMovable() {
 int main() {
     spawnOnAManualExecutorStartsWhenItRuns();
     spawnOnAThreadPoolJoinsFromTheCallingThread();
+    joinReturnsAnOptional();
     joinRethrowsTheTaskException();
     joinMovesOutMoveOnlyResults();
     joinHandlesAreAwaitable();
````

修改 `tests/allocation_tests.cpp`、`tests/callback_tests.cpp`、`tests/edge_case_tests.cpp`、`tests/thread_pool_tests.cpp`、`tests/when_all_tests.cpp`：

````diff
diff --git a/tests/allocation_tests.cpp b/tests/allocation_tests.cpp
index adee909..17b96b0 100644
--- a/tests/allocation_tests.cpp
+++ b/tests/allocation_tests.cpp
@@ -169,7 +169,7 @@ void whenAllTupleAllocatesChildFramesStopStateAndAwaiterSlot() {
     // 3 个子帧 + 1 个 stop 状态 + 1 次 awaiter 槽回落（awaiter 内嵌每个 child
     // 的续体帧， 远大于 64 字节的内联槽）；syncWait(awaitable) 另需 1 个包装 Task 帧。
     CHECK_ALLOCATIONS(6, {
-        auto const results = co2::syncWait(co2::whenAll(leaf(1), leaf(2), leaf(3)));
+        auto const results = *co2::syncWait(co2::whenAll(leaf(1), leaf(2), leaf(3)));
         CHECK(std::get<0>(results) + std::get<1>(results) + std::get<2>(results) == 6);
     });
 }
@@ -255,7 +255,7 @@ CO2_END
 
 void oversizedCallbackAwaiterFallsBackToOneHeapAllocation() {
     // 帧 1 + awaiter 槽回落 1。
-    CHECK_ALLOCATIONS(2, CHECK(co2::syncWait(viaWideCallback()).words[7] == &wideSentinel));
+    CHECK_ALLOCATIONS(2, CHECK(co2::syncWait(viaWideCallback())->words[7] == &wideSentinel));
 }
 
 auto viaCallbackWithLargeClosure(std::unique_ptr<int> payload, int a, int b)
diff --git a/tests/callback_tests.cpp b/tests/callback_tests.cpp
index 01cb70c..9a84e1a 100644
--- a/tests/callback_tests.cpp
+++ b/tests/callback_tests.cpp
@@ -159,9 +159,9 @@ auto uniqueResult()
 CO2_END
 
 void resultsAreConstructedInPlaceAndMovedOut() {
-    auto const pair = co2::syncWait(pairResult());
+    auto const pair = *co2::syncWait(pairResult());
     CHECK(pair.first == 3 && pair.second == "three");
-    auto unique = co2::syncWait(uniqueResult());
+    auto unique = *co2::syncWait(uniqueResult());
     CHECK(unique != nullptr && *unique == 9);
 }
 
diff --git a/tests/edge_case_tests.cpp b/tests/edge_case_tests.cpp
index d203c3b..fefcc15 100644
--- a/tests/edge_case_tests.cpp
+++ b/tests/edge_case_tests.cpp
@@ -156,7 +156,7 @@ auto manyCommasWithAllocator()
 CO2_END
 
 void macroArgumentsMayContainManyTopLevelCommas() {
-    auto const values = co2::syncWait(manyCommas());
+    auto const values = *co2::syncWait(manyCommas());
     for (auto index = std::size_t{}; index != values.size(); ++index)
         CHECK(values[index] == static_cast<int>(index) + 1);
     CHECK(co2::syncWait(manyCommasWithAllocator()) == 1 + 3 + 5 + 7 + 9 + 12);
diff --git a/tests/thread_pool_tests.cpp b/tests/thread_pool_tests.cpp
index b04fc10..9f647b4 100644
--- a/tests/thread_pool_tests.cpp
+++ b/tests/thread_pool_tests.cpp
@@ -441,7 +441,7 @@ auto blocksOnItsChild(co2::ThreadPool& pool, int v) CO2_BEG(co2::Task<int>, (poo
     CO2_AWAIT(co2::scheduleOn(pool));
     // 在工作线程上阻塞式 join：子 Task 排在本线程的本地队列里，只剩它一项，而本线程
     // 不会回来取——必须有另一个工作线程被叫醒并把它偷走。
-    CO2_RETURN(co2::spawn(pool, doubles(pool, v)).join());
+    CO2_RETURN(*co2::spawn(pool, doubles(pool, v)).join());
 }
 CO2_END
 
diff --git a/tests/when_all_tests.cpp b/tests/when_all_tests.cpp
index 2448cf1..4b2da99 100644
--- a/tests/when_all_tests.cpp
+++ b/tests/when_all_tests.cpp
@@ -135,7 +135,7 @@ void resultTypesConcatenateValueChannels() {
 void readyChildrenCompleteWithoutSuspending() {
     std::atomic<int> counter{0};
     auto const results =
-        co2::syncWait(co2::whenAll(value(1), nothing(counter), text("x")));
+        *co2::syncWait(co2::whenAll(value(1), nothing(counter), text("x")));
     CHECK(std::get<0>(results) == 1);
     CHECK(std::get<1>(results) == "x");
     CHECK(counter.load() == 1);
@@ -146,20 +146,20 @@ void readyChildrenCompleteWithoutSuspending() {
 }
 
 void moveOnlyResultsFlowThroughTupleAndVector() {
-    auto tuple = co2::syncWait(co2::whenAll(unique(1), unique(2)));
+    auto tuple = *co2::syncWait(co2::whenAll(unique(1), unique(2)));
     CHECK(*std::get<0>(tuple) == 1 && *std::get<1>(tuple) == 2);
 
     std::vector<co2::Task<std::unique_ptr<int>>> tasks;
     for (int i = 0; i < 5; ++i)
         tasks.push_back(unique(i));
-    auto values = co2::syncWait(co2::whenAll(std::move(tasks)));
+    auto values = *co2::syncWait(co2::whenAll(std::move(tasks)));
     CHECK(values.size() == 5U);
     for (int i = 0; i < 5; ++i)
         CHECK(*values[static_cast<std::size_t>(i)] == i);
 }
 
 void emptyVectorCompletesImmediately() {
-    auto values = co2::syncWait(co2::whenAll(std::vector<co2::Task<int>>{}));
+    auto values = *co2::syncWait(co2::whenAll(std::vector<co2::Task<int>>{}));
     CHECK(values.empty());
     co2::syncWait(co2::whenAll(std::vector<co2::Task<>>{}));
 }
@@ -235,7 +235,7 @@ void childrenRunConcurrentlyOnAThreadPool() {
     std::vector<co2::Task<int>> tasks;
     for (int i = 0; i < 8; ++i)
         tasks.push_back(slowOnPool(pool, concurrency, i));
-    auto const values = co2::syncWait(co2::whenAll(std::move(tasks)));
+    auto const values = *co2::syncWait(co2::whenAll(std::move(tasks)));
     CHECK(values.size() == 8U);
     for (int i = 0; i < 8; ++i)
         CHECK(values[static_cast<std::size_t>(i)] == i);
@@ -477,7 +477,7 @@ void nestedWhenAllOverManyChildrenOnAPool() {
         }
         groups.push_back(sumOf(std::move(tasks)));
     }
-    auto const sums = co2::syncWait(co2::whenAll(std::move(groups)));
+    auto const sums = *co2::syncWait(co2::whenAll(std::move(groups)));
     long long total = 0;
     for (auto const s : sums)
         total += s;
@@ -499,7 +499,7 @@ void whenAllWorksInsideASpawnedTask() {
 void repeatedWhenAllDoesNotLeakOrCorrupt() {
     co2::ThreadPool pool{2U};
     for (int round = 0; round < 200; ++round) {
-        auto const results = co2::syncWait(co2::whenAll(
+        auto const results = *co2::syncWait(co2::whenAll(
             hopAndValue(pool, round), hopAndValue(pool, -round), value(1)));
         CHECK(std::get<0>(results) + std::get<1>(results) == 0);
         CHECK(std::get<2>(results) == 1);
````

- [ ] **Step 2: 构建，确认失败**

Expected: 构建失败。`tests/scheduler_tests.cpp` 报 `static assertion failed: syncWait(Task<int>) must return co2::optional<int>`；其他测试文件里取用结果的新写法（`*`、`->`）在旧的返回类型上也编不过。

- [ ] **Step 3: 实现**

修改 `include/co2/task.hpp`、`include/co2/sync_wait.hpp`、`include/co2/spawn.hpp`：

````diff
diff --git a/include/co2/spawn.hpp b/include/co2/spawn.hpp
index f83ca23..7e4937d 100644
--- a/include/co2/spawn.hpp
+++ b/include/co2/spawn.hpp
@@ -158,15 +158,16 @@ template <class T> struct JoinHandle {
         return state->source.get_token();
     }
 
-    // 阻塞到 Task 完成并取走结果（只可一次）。在 scheduler
-    // 的工作线程上调用会占住该线程。
-    T join() {
+    // 阻塞到 Task 完成并取走结果（只可一次）。返回值与 syncWait 同形：结果放在
+    // co2::optional 里，空表示已停止，Task 的异常在这里重抛。在 scheduler 的工作线程上
+    // 调用会占住该线程。
+    detail::RootResult<T> join() {
         CO2_CONTRACT_CHECK(state != nullptr);
         {
             std::unique_lock<std::mutex> lock{state->mutex};
             state->completed.wait(lock, [this] { return state->ready; });
         }
-        return detail::TaskAccess::takeResult(task);
+        return detail::TaskAccess::takeRootResult(task);
     }
 
     // ---- 等待：右值时 JoinHandle 本身是 awaiter（拥有），左值时借用 ----
diff --git a/include/co2/sync_wait.hpp b/include/co2/sync_wait.hpp
index 955b6e2..d2e6014 100644
--- a/include/co2/sync_wait.hpp
+++ b/include/co2/sync_wait.hpp
@@ -56,19 +56,23 @@ struct SyncWaitCompletion {
 
 } // namespace detail
 
-// 在调用线程上阻塞直到 Task 完成，返回其结果或重抛其异常。Task 在调用线程上内联启动；
-// 之后它在哪里运行由它自己的 scheduleOn 决定，完成时通过对称转移唤醒等待线程。
-// `token` 成为 Task 树的根 stop_token（体内 CO2_AWAIT_SET(t, getStopToken()) 可见）。
+// 在调用线程上阻塞直到 Task 完成。Task 在调用线程上内联启动；之后它在哪里运行由它自己
+// 的 scheduleOn 决定，完成时通过对称转移唤醒等待线程。`token` 成为 Task 树的根
+// stop_token（体内 CO2_AWAIT_SET(t, getStopToken()) 可见）。
 //
-// 与 std::execution::sync_wait 一样，它就是阻塞：在 scheduler 的工作线程上调用会占住
-// 该线程，Task 若依赖这个线程就会死锁。
-template <class T> T syncWait(Task<T> task, stop_token token = {}) {
+// 返回值照 std::execution::sync_wait：结果放在 co2::optional 里，Task<void> 是
+// co2::optional<co2::Unit>；空的 optional 表示已停止；Task 的异常在这里重抛。
+//
+// 与 sync_wait 一样，它就是阻塞：在 scheduler 的工作线程上调用会占住该线程，Task 若依赖
+// 这个线程就会死锁。
+template <class T>
+detail::RootResult<T> syncWait(Task<T> task, stop_token token = {}) {
     CO2_CONTRACT_CHECK(task);
     detail::SyncWaitState state;
     detail::SyncWaitCompletion completion{state};
     detail::TaskAccess::start(task, completion.handle(), std::move(token));
     state.wait();
-    return detail::TaskAccess::takeResult(task);
+    return detail::TaskAccess::takeRootResult(task);
 }
 
 namespace detail {
@@ -112,11 +116,11 @@ Task<void> wrapAwaitable(Awaitable&& awaitable, std::true_type) {
 // 任意 awaitable 的同步等待：在一个临时 Task 里 co_await 它。
 template <class Awaitable, class = typename std::enable_if<not detail::IsTask<
                                typename std::decay<Awaitable>::type>::value>::type>
-detail::AwaitResultOf<Awaitable> syncWait(Awaitable&& awaitable,
-                                          stop_token token = {}) {
-    using Result = detail::AwaitResultOf<Awaitable>;
-    return syncWait(detail::wrapAwaitable<Result>(std::forward<Awaitable>(awaitable),
-                                                  std::is_void<Result>{}),
+detail::RootResult<detail::AwaitResultOf<Awaitable>> syncWait(Awaitable&& awaitable,
+                                                              stop_token token = {}) {
+    using Awaited = detail::AwaitResultOf<Awaitable>;
+    return syncWait(detail::wrapAwaitable<Awaited>(std::forward<Awaitable>(awaitable),
+                                                   std::is_void<Awaited>{}),
                     std::move(token));
 }
 
diff --git a/include/co2/task.hpp b/include/co2/task.hpp
index 4aada83..31ac9fd 100644
--- a/include/co2/task.hpp
+++ b/include/co2/task.hpp
@@ -8,6 +8,8 @@
 #include "co2/coroutine.hpp"
 #include "co2/detail/result_storage.hpp"
 #include "co2/env.hpp"
+#include "co2/optional.hpp"
+#include "co2/result.hpp"
 #include "co2/stop_token.hpp"
 
 // co2：惰性、单消费者的 Task<T>。promise 与 cppcoro::task 逐字同形：
@@ -197,6 +199,12 @@ template <class T> Task<T> TaskPromise<T>::get_return_object() noexcept {
     return Task<T>{coroutine_handle<TaskPromise>::from_promise(*this)};
 }
 
+// 根（syncWait、JoinHandle::join）交给普通代码的类型：结果放进 optional，空表示已停止。
+// void 没有值可放，用 Unit 占位，这样 Task<void> 的根与其他根同形。
+template <class T> struct RootValue { using type = T; };
+template <> struct RootValue<void> { using type = Unit; };
+template <class T> using RootResult = optional<typename RootValue<T>::type>;
+
 // 根适配器（syncWait、spawn）启动 Task 的唯一入口：设置续体与根环境，然后恢复
 // （或交给 scheduler 恢复）。
 struct TaskAccess {
@@ -228,6 +236,11 @@ struct TaskAccess {
         return task.handle.promise().result();
     }
 
+    // 根取走结果（只可一次）。
+    template <class T> static RootResult<T> takeRootResult(Task<T>& task) {
+        return takeRootResult(task, std::is_void<T>{});
+    }
+
     template <class T> static bool hasError(Task<T> const& task) noexcept {
         return task.handle && task.handle.promise().error != nullptr;
     }
@@ -243,6 +256,17 @@ struct TaskAccess {
         task.handle = nullptr;
         return handle;
     }
+
+  private:
+    template <class T>
+    static RootResult<T> takeRootResult(Task<T>& task, std::false_type) {
+        return RootResult<T>{takeResult(task)};
+    }
+
+    static RootResult<void> takeRootResult(Task<void>& task, std::true_type) {
+        takeResult(task);
+        return RootResult<void>{Unit{}};
+    }
 };
 
 } // namespace detail
````

- [ ] **Step 4: 构建并运行全部测试，确认通过**

Expected: `100% tests passed, 0 tests failed out of 25`。

- [ ] **Step 5: 更新基准里的调用点**

修改 `benchmarks/co2_benchmarks.cpp`：

````diff
diff --git a/benchmarks/co2_benchmarks.cpp b/benchmarks/co2_benchmarks.cpp
index 817a9b0..556f545 100644
--- a/benchmarks/co2_benchmarks.cpp
+++ b/benchmarks/co2_benchmarks.cpp
@@ -133,7 +133,7 @@ CO2_END
 double symmetricTaskTransfer() {
     constexpr int ops = 5000000;
     return nanosecondsPerOp(static_cast<std::size_t>(ops),
-                            [&] { sink = co2::syncWait(awaitsLeaves(ops)); });
+                            [&] { sink = *co2::syncWait(awaitsLeaves(ops)); });
 }
 
 double syncWaitReadyTask() {
@@ -141,7 +141,7 @@ double syncWaitReadyTask() {
     return nanosecondsPerOp(static_cast<std::size_t>(ops), [&] {
         long total = 0;
         for (auto index = 0; index != ops; ++index)
-            total += co2::syncWait(leaf(index));
+            total += *co2::syncWait(leaf(index));
         sink = total;
     });
 }
@@ -181,7 +181,7 @@ double awaitChildOnThreads(int const threads, bool const sharedToken) {
             workers.emplace_back([&shared, sharedToken] {
                 co2::stop_source own;
                 auto token = sharedToken ? shared.get_token() : own.get_token();
-                sink = co2::syncWait(awaitsLeaves(ops), std::move(token));
+                sink = *co2::syncWait(awaitsLeaves(ops), std::move(token));
             });
         }
         for (auto& worker : workers)
@@ -209,7 +209,7 @@ double manualExecutorHop() {
     return nanosecondsPerOp(static_cast<std::size_t>(ops), [&] {
         auto handle = co2::spawn(executor, hopper(executor, ops));
         executor.run();
-        sink = handle.join();
+        sink = *handle.join();
     });
 }
 
@@ -226,7 +226,7 @@ double threadPoolHops(unsigned const workers, int const tasks, int const hopsPer
             handles.push_back(co2::spawn(pool, hopper(pool, hopsPerTask)));
         long total = 0;
         for (auto& handle : handles)
-            total += handle.join();
+            total += *handle.join();
         sink = total;
     });
 }
@@ -242,7 +242,7 @@ double spawnAndJoin() {
     return nanosecondsPerOp(static_cast<std::size_t>(ops), [&] {
         long total = 0;
         for (auto index = 0; index != ops; ++index)
-            total += co2::spawn(pool, leaf(index)).join();
+            total += *co2::spawn(pool, leaf(index)).join();
         sink = total;
     });
 }
````

确认它能编译、数字没有变坏：

```bash
cmake --build /tmp/co2-build/bench -j 12
/tmp/co2-build/bench/benchmarks/co2-benchmarks Task
```

Expected: `symmetric Task transfer` 约 12 ns，`syncWait ready Task` 约 26 ns，`Task failure through a 100-deep chain` 仍在 550～570 ns。

`tests/install_consumer/main.cpp` 里是 `co2::syncWait(answer()) == 42`，不需要改。

- [ ] **Step 6: 更新文档**

修改 `README.md`、`docs/architecture.md`：

````diff
diff --git a/README.md b/README.md
index ccb916c..68c534a 100644
--- a/README.md
+++ b/README.md
@@ -191,19 +191,25 @@ pool.isWorkerThread();                       // 当前线程是否是它的工
 ### `syncWait`
 
 ```cpp
-T result = co2::syncWait(task);            // 阻塞到完成，重抛异常
-T result = co2::syncWait(task, token);     // token 成为 Task 树的根 stop_token
+co2::optional<T> result = co2::syncWait(task);         // 阻塞到完成，重抛异常
+co2::optional<T> result = co2::syncWait(task, token);  // token 成为 Task 树的根 stop_token
 ```
 
 Task 在调用线程上内联启动，完成时对称转移到一个不占堆的续体帧唤醒等待线程。与
 `std::execution::sync_wait` 一样，它就是阻塞。
 
+返回值也照 `sync_wait`：结果放在 `co2::optional` 里，`Task<void>` 是
+`co2::optional<co2::Unit>`。取值写 `*result`；与值比较可以直接写
+`co2::syncWait(task) == 42`。空的 optional 留给"已停止"这种结果——目前还没有任何操作
+会以已停止完成，所以它总是有值。结果是 `bool` 的根不要直接当条件用：
+`if (co2::syncWait(task))` 判断的是"有没有值"，不是值本身。
+
 ### `spawn` / `JoinHandle<T>`
 
 ```cpp
 auto handle = co2::spawn(pool, work());          // 惰性 Task 直接排给 scheduler
 handle.requestStop();                            // 协作式取消
-T value = handle.join();                         // 阻塞
+co2::optional<T> value = handle.join();          // 阻塞；返回值与 syncWait 同形
 CO2_AWAIT_SET(value, handle);                    // 或在协程里等待（左值借用/右值拥有）
 co2::spawn(pool, work(), parentToken);           // 父 token 的停止请求转发过来
 ```
diff --git a/docs/architecture.md b/docs/architecture.md
index 5e43da4..ad7fcf9 100644
--- a/docs/architecture.md
+++ b/docs/architecture.md
@@ -164,7 +164,9 @@ token 的基准：240 → 20 ns/await）。等待者不是 co2 自己的 promise
   join。
 - **`ManualExecutor`**：互斥 FIFO；带排队工作销毁是契约违规。
 - **`syncWait`**：Task 在调用线程上内联启动；续体是一个不占堆的手写标准布局帧，被对称
-  转移到时标记完成并唤醒等待线程。
+  转移到时标记完成并唤醒等待线程。返回值照 `std::execution::sync_wait` 放在
+  `co2::optional` 里（`Task<void>` 是 `co2::optional<co2::Unit>`），`JoinHandle::join()`
+  同形。
 - **`spawn`**：给 Task 设好续体与 token 后直接把它的句柄排给 scheduler（惰性 Task
   本身就是可排队句柄，无跳板帧）。`JoinHandle` 与内嵌续体帧共享一个堆分配的
   `JoinState`；未完成时析构 = 请求停止并分离（P3149 `spawn_future`），Task 跑完后由
````

- [ ] **Step 7: 提交**

```bash
git add include/co2/task.hpp include/co2/sync_wait.hpp include/co2/spawn.hpp \
        tests/scheduler_tests.cpp tests/spawn_tests.cpp tests/allocation_tests.cpp \
        tests/callback_tests.cpp tests/edge_case_tests.cpp tests/thread_pool_tests.cpp \
        tests/when_all_tests.cpp benchmarks/co2_benchmarks.cpp README.md \
        docs/architecture.md
git commit -F - <<'EOF'
syncWait 与 JoinHandle::join 返回 co2::optional

不兼容改动（docs/design/completion-channels.md 第 2.4 节、第 6 节第 1 条）。根的
返回值照 std::execution::sync_wait：结果放在 optional 里，Task<void> 是
optional<Unit>；空的 optional 留给"已停止"。这一步里它总是有值——纯粹的接口变更，
先于机制落地。

与值比较的写法不受影响（syncWait(task) == 42）；取用结果的地方加 *。测试、基准与
README 里的调用点已全部更新。
EOF
```

### Task 4: 核心——恢复点询问 `await_completion()`

设计稿第 3 节、第 10 节第 4 步。这一步库里还没有任何组件产生或接收新结果，用测试自己写的 awaiter 和 promise 验证。

**Files:**
- Create: `tests/completion_tests.cpp`、`tests/stopped_without_entry_contract_violation_test.cpp`
- Modify: `tests/CMakeLists.txt`
- Modify: `include/co2/detail/awaitable.hpp`、`include/co2/detail/frame.hpp`、`include/co2/dsl.hpp`

**Interfaces:**
- Consumes: `co2::Completion`、`co2::Error`（任务 2）；`FrameCore` 已有的 `awaiter<A>()`、`resetAwaiterAs<A>()`、`destroyBody()`、`promise()`、`enterFinalSuspend()`。
- Produces:
  - 协议：awaiter 的可选成员 `co2::Completion await_completion() noexcept`；promise 的可选成员 `void return_stopped() noexcept`、`void return_error(co2::Error) noexcept`。
  - `template <class Awaiter> struct co2::detail::HasAwaitCompletion`，`::type` 是 `std::true_type` 或 `std::false_type`（任务 7 用它）。
  - `co2::detail::AwaiterRef<Awaiter>::await_completion()`（原对象有这个成员时才存在）。
  - `template <class Promise> struct co2::detail::HasReturnStopped` / `HasReturnError`。
  - `template <class Awaiter> bool co2::detail::FrameCore<...>::finishUnlessValue(FrameHeader*& next)`：返回 `true` 表示这次等待没有值、协程已经收尾，`next` 是要转移到的目标。
  - 测试里后面的任务要用的东西（都在 `tests/completion_tests.cpp` 的匿名命名空间里）：awaiter `Ends { co2::Completion completion; }`（立刻就绪，有值时 `await_resume()` 给出 7）；`co2::Completion stopped()`、`co2::Completion failed()`（后者的错误码是 `sample::Errc::gone`）；返回类型 `Recorder`（promise 有两个入口，`promise().outcome` 是 `Outcome::running / returned / stopped / failed / threw`，`promise().code` 是收到的错误码）。

- [ ] **Step 1: 写测试**

新建 `tests/completion_tests.cpp`：

````cpp
// 三种完成方式：值、错误、已停止（docs/design/completion-channels.md）。
//
// 一次等待结束时，恢复点先问 awaiter 的可选成员 await_completion()：有值就照常
// await_resume() 并继续；出错或已停止就此结束协程——销毁 awaiter、析构局部、把结果交给
// promise 的 return_error() / return_stopped()，然后 co_await final_suspend()。

#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "co2/completion.hpp"
#include "co2/coroutine.hpp"
#include "co2/error.hpp"
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

// 事件日志：协议调用与析构按发生的顺序记在这里。
std::vector<std::string> events;

void record(char const* const event) { events.emplace_back(event); }

co2::Completion stopped() { return co2::Completion::stopped(); }
co2::Completion failed() { return co2::Completion::error(sample::Errc::gone); }

// ---------------------------------------------------------------------------
// 测试自己的 awaiter

// 立刻就绪，以给定的方式结束；有值时 await_resume() 给出 7。
struct Ends {
    co2::Completion completion;

    bool await_ready() const noexcept { return true; }
    void await_suspend(co2::coroutine_handle<>) const noexcept {}
    int await_resume() const noexcept { return 7; }
    co2::Completion await_completion() noexcept { return completion; }
};

// 同上，并把协议调用与自己的析构记进事件日志。临时对象被移进帧之后析构，那一次不记：
// 只有被问过 await_completion() 的那个对象才记。
struct Logs {
    explicit Logs(co2::Completion completion_) noexcept
        : completion{std::move(completion_)} {}
    Logs(Logs&&) = default;
    ~Logs() {
        if (asked) record("~awaiter");
    }

    bool await_ready() const noexcept { return true; }
    void await_suspend(co2::coroutine_handle<>) const noexcept {}
    void await_resume() const { record("await_resume"); }
    co2::Completion await_completion() noexcept {
        asked = true;
        record("await_completion");
        return completion;
    }

    co2::Completion completion;
    bool asked{};
};

// 真的挂起：把等待者的句柄交给测试，结果由测试在恢复它之前决定。
struct Parks {
    co2::coroutine_handle<>* parked;
    co2::Completion const* outcome;

    bool await_ready() const noexcept { return false; }
    void await_suspend(co2::coroutine_handle<> const handle) const noexcept {
        *parked = handle;
    }
    void await_resume() const noexcept {}
    co2::Completion await_completion() const noexcept { return *outcome; }
};

// 帧里的局部：析构时计数并记进事件日志。
struct Local {
    int* destroyed{};
    ~Local() {
        if (destroyed == nullptr) return;
        ++*destroyed;
        record("~local");
    }
};

// ---------------------------------------------------------------------------
// 测试自己的协程返回类型

enum class Outcome { running, returned, stopped, failed, threw };

// promise 提供两个入口，记下自己是怎么结束的。停在 final suspend，由 Recorder 销毁。
struct Recorder {
    struct promise_type {
        promise_type() = default; // 不是聚合类型：不拿协程的参数去初始化成员

        Recorder get_return_object() noexcept {
            return Recorder{co2::coroutine_handle<promise_type>::from_promise(*this)};
        }
        co2::suspend_never initial_suspend() noexcept { return {}; }
        co2::suspend_always final_suspend() noexcept {
            record("final_suspend");
            return {};
        }
        void return_void() noexcept { outcome = Outcome::returned; }
        void unhandled_exception() noexcept { outcome = Outcome::threw; }

        void return_stopped() noexcept {
            record("return_stopped");
            outcome = Outcome::stopped;
        }

        void return_error(co2::Error error) noexcept {
            record("return_error");
            outcome = Outcome::failed;
            code = error.code();
        }

        // co_yield 的恢复点同样参与：yield 出去的值在这里被变成一次失败的等待。
        Ends yield_value(int) noexcept { return Ends{failed()}; }

        Outcome outcome{Outcome::running};
        std::error_code code;
    };

    explicit Recorder(co2::coroutine_handle<promise_type> const handle_) noexcept
        : handle{handle_} {}
    Recorder(Recorder&& other) noexcept : handle{other.handle} { other.handle = nullptr; }
    Recorder(Recorder const&) = delete;
    Recorder& operator=(Recorder const&) = delete;
    Recorder& operator=(Recorder&&) = delete;
    ~Recorder() {
        if (handle) handle.destroy();
    }

    promise_type& promise() const noexcept { return handle.promise(); }
    bool done() const noexcept { return handle.done(); }

    co2::coroutine_handle<promise_type> handle;
};

// 为标准写的 promise：没有这两个入口，只有 unhandled_exception()。
struct Plain {
    struct promise_type {
        promise_type() = default;

        Plain get_return_object() noexcept {
            return Plain{co2::coroutine_handle<promise_type>::from_promise(*this)};
        }
        co2::suspend_never initial_suspend() noexcept { return {}; }
        co2::suspend_always final_suspend() noexcept { return {}; }
        void return_void() noexcept { returned = true; }
        void unhandled_exception() noexcept { exception = std::current_exception(); }

        std::exception_ptr exception;
        bool returned{};
    };

    explicit Plain(co2::coroutine_handle<promise_type> const handle_) noexcept
        : handle{handle_} {}
    Plain(Plain&& other) noexcept : handle{other.handle} { other.handle = nullptr; }
    Plain(Plain const&) = delete;
    Plain& operator=(Plain const&) = delete;
    Plain& operator=(Plain&&) = delete;
    ~Plain() {
        if (handle) handle.destroy();
    }

    promise_type& promise() const noexcept { return handle.promise(); }

    co2::coroutine_handle<promise_type> handle;
};

// final_suspend 不挂起：协程结束时帧自己销毁。
int detachedStops = 0;

struct Detached {
    struct promise_type {
        promise_type() = default;

        Detached get_return_object() noexcept { return {}; }
        co2::suspend_never initial_suspend() noexcept { return {}; }
        co2::suspend_never final_suspend() noexcept { return {}; }
        void return_void() noexcept {}
        void unhandled_exception() noexcept {}
        void return_stopped() noexcept { ++detachedStops; }
    };
};

// ---------------------------------------------------------------------------
// 已停止与错误就此结束协程

auto endsWith(co2::Completion completion, bool& reached, int& destroyed)
    CO2_BEG(Recorder, (completion, reached, destroyed), Local local;) {
    local.destroyed = &destroyed;
    CO2_AWAIT(Ends{completion});
    reached = true;
}
CO2_END

void aStoppedAwaitEndsTheCoroutine() {
    auto reached = false;
    auto destroyed = 0;
    auto const coroutine = endsWith(stopped(), reached, destroyed);
    CHECK(coroutine.done());
    CHECK(coroutine.promise().outcome == Outcome::stopped);
    CHECK(not reached);     // 等待之后的语句没有执行
    CHECK(destroyed == 1);  // 局部已经析构，不等到帧被销毁
}

void aFailedAwaitDeliversTheErrorToThePromise() {
    auto reached = false;
    auto destroyed = 0;
    auto const coroutine = endsWith(failed(), reached, destroyed);
    CHECK(coroutine.done());
    CHECK(coroutine.promise().outcome == Outcome::failed);
    CHECK(coroutine.promise().code == sample::Errc::gone);
    CHECK(not reached);
    CHECK(destroyed == 1);
}

auto takesTheValue(int& taken) CO2_BEG(Recorder, (taken)) {
    CO2_AWAIT_SET(taken, Ends{co2::Completion::value()});
}
CO2_END

void aValueCompletionContinuesAsUsual() {
    auto taken = 0;
    auto const coroutine = takesTheValue(taken);
    CHECK(taken == 7);
    CHECK(coroutine.promise().outcome == Outcome::returned);
}

// ---------------------------------------------------------------------------
// 调用约定与收尾顺序

auto logsTheAwait(co2::Completion completion, int& destroyed)
    CO2_BEG(Recorder, (completion, destroyed), Local local;) {
    local.destroyed = &destroyed;
    CO2_AWAIT(Logs{completion});
    record("after the await");
}
CO2_END

void awaitCompletionIsAskedOnceBeforeAwaitResume() {
    events.clear();
    auto destroyed = 0;
    {
        auto const coroutine = logsTheAwait(co2::Completion::value(), destroyed);
        CHECK(coroutine.promise().outcome == Outcome::returned);
    }
    auto const expected = std::vector<std::string>{
        "await_completion", "await_resume", "~awaiter",
        "after the await",  "~local",       "final_suspend"};
    CHECK(events == expected);
}

// 与异常逃出协程体时的顺序相同：awaiter → 局部 → promise 的入口 → final_suspend。
void aNonValueCompletionCleansUpInTheSameOrderAsAnException() {
    events.clear();
    auto destroyed = 0;
    {
        auto const coroutine = logsTheAwait(stopped(), destroyed);
        CHECK(coroutine.promise().outcome == Outcome::stopped);
    }
    auto const expected = std::vector<std::string>{
        "await_completion", "~awaiter", "~local", "return_stopped", "final_suspend"};
    CHECK(events == expected);
}

// ---------------------------------------------------------------------------
// 恢复点的各种形态

auto parksThenEnds(co2::coroutine_handle<>& parked, co2::Completion const& outcome,
                   bool& reached)
    CO2_BEG(Recorder, (parked, outcome, reached)) {
    CO2_AWAIT((Parks{&parked, &outcome})); // 花括号里的逗号要用圆括号包住
    reached = true;
}
CO2_END

// awaiter 在别处完成、之后才恢复等待者：结果记在 awaiter 上，恢复时被问到。
void aSuspendedAwaitIsAskedWhenItIsResumed() {
    auto parked = co2::coroutine_handle<>{};
    auto outcome = co2::Completion::value();
    auto reached = false;
    auto const coroutine = parksThenEnds(parked, outcome, reached);
    CHECK(parked);
    CHECK(not coroutine.done());

    outcome = stopped();
    parked.resume();
    CHECK(coroutine.done());
    CHECK(coroutine.promise().outcome == Outcome::stopped);
    CHECK(not reached);
}

auto yieldsOnce(bool& reached) CO2_BEG(Recorder, (reached)) {
    CO2_YIELD(1);
    reached = true;
}
CO2_END

void aYieldIsAResumePointToo() {
    auto reached = false;
    auto const coroutine = yieldsOnce(reached);
    CHECK(coroutine.promise().outcome == Outcome::failed);
    CHECK(coroutine.promise().code == sample::Errc::gone);
    CHECK(not reached);
}

auto awaitsAnLvalue(Ends& awaiter, bool& reached) CO2_BEG(Recorder, (awaiter, reached)) {
    CO2_AWAIT(awaiter);
    reached = true;
}
CO2_END

// 左值 awaiter 在帧里只记地址（detail::AwaiterRef），这个成员同样要转发过去。
void anLvalueAwaiterIsAskedInPlace() {
    auto awaiter = Ends{stopped()};
    auto reached = false;
    auto const coroutine = awaitsAnLvalue(awaiter, reached);
    CHECK(coroutine.promise().outcome == Outcome::stopped);
    CHECK(not reached);
}

auto detachedEndsWith(co2::Completion completion, int& destroyed)
    CO2_BEG(Detached, (completion, destroyed), Local local;) {
    local.destroyed = &destroyed;
    CO2_AWAIT(Ends{completion});
}
CO2_END

void aCoroutineThatDoesNotSuspendAtTheEndDestroysItself() {
    detachedStops = 0;
    auto destroyed = 0;
    detachedEndsWith(stopped(), destroyed);
    CHECK(detachedStops == 1);
    CHECK(destroyed == 1);
}

// ---------------------------------------------------------------------------
// promise 没有入口：错误在协程体内重新抛出，走 unhandled_exception()

auto plainEndsWith(co2::Completion completion, bool& reached, int& destroyed)
    CO2_BEG(Plain, (completion, reached, destroyed), Local local;) {
    local.destroyed = &destroyed;
    CO2_AWAIT(Ends{completion});
    reached = true;
}
CO2_END

void anErrorCodeReachesAStandardPromiseAsSystemError() {
    auto reached = false;
    auto destroyed = 0;
    auto const coroutine = plainEndsWith(failed(), reached, destroyed);
    CHECK(not reached);
    CHECK(destroyed == 1);
    CHECK(not coroutine.promise().returned);

    auto sawCode = false;
    try {
        std::rethrow_exception(coroutine.promise().exception);
    } catch (std::system_error const& exception) {
        sawCode = exception.code() == sample::Errc::gone;
    }
    CHECK(sawCode);
}

void aCapturedExceptionReachesAStandardPromiseUnchanged() {
    auto reached = false;
    auto destroyed = 0;
    auto const original = std::make_exception_ptr(std::runtime_error{"boom"});
    auto const coroutine = plainEndsWith(
        co2::Completion::error(co2::Error{original}), reached, destroyed);
    CHECK(not reached);
    CHECK(destroyed == 1);

    auto sawOriginal = false;
    try {
        std::rethrow_exception(coroutine.promise().exception);
    } catch (std::runtime_error const& exception) {
        sawOriginal = std::string{exception.what()} == "boom";
    }
    CHECK(sawOriginal);
}

} // namespace

int main() {
    aStoppedAwaitEndsTheCoroutine();
    aFailedAwaitDeliversTheErrorToThePromise();
    aValueCompletionContinuesAsUsual();
    awaitCompletionIsAskedOnceBeforeAwaitResume();
    aNonValueCompletionCleansUpInTheSameOrderAsAnException();
    aSuspendedAwaitIsAskedWhenItIsResumed();
    aYieldIsAResumePointToo();
    anLvalueAwaiterIsAskedInPlace();
    aCoroutineThatDoesNotSuspendAtTheEndDestroysItself();
    anErrorCodeReachesAStandardPromiseAsSystemError();
    aCapturedExceptionReachesAStandardPromiseUnchanged();
    return 0;
}
````

新建 `tests/stopped_without_entry_contract_violation_test.cpp`：

````cpp
#include <atomic>
#include <cstdlib>

#include "co2/completion.hpp"
#include "co2/coroutine.hpp"

namespace {

std::atomic<bool> atExpectedFailurePoint{};

void contractViolationHandler(co2::ContractViolation const&) {
    std::_Exit(atExpectedFailurePoint.load(std::memory_order_acquire) ? 86 : 87);
}

// 立刻就绪，以给定的方式结束。
struct Ends {
    co2::Completion completion;

    bool await_ready() const noexcept { return true; }
    void await_suspend(co2::coroutine_handle<>) const noexcept {}
    void await_resume() const noexcept {}
    co2::Completion await_completion() noexcept { return completion; }
};

// 为标准写的 promise：没有 return_stopped()。
struct Plain {
    struct promise_type {
        promise_type() = default;

        Plain get_return_object() noexcept { return {}; }
        co2::suspend_never initial_suspend() noexcept { return {}; }
        co2::suspend_never final_suspend() noexcept { return {}; }
        void return_void() noexcept {}
        void unhandled_exception() noexcept {}
    };
};

auto endsWith(co2::Completion completion) CO2_BEG(Plain, (completion)) {
    CO2_AWAIT(Ends{completion});
}
CO2_END

} // namespace

// "已停止"被送到一个没有 return_stopped() 的 promise：它无处可去——既不是值，也不是
// 可以抛进协程体的错误——所以是契约违规。有值的等待在同一个 promise 里照常工作。
int main() {
    co2::setContractViolationHandler(&contractViolationHandler);
    endsWith(co2::Completion::value());

    atExpectedFailurePoint.store(true, std::memory_order_release);
    endsWith(co2::Completion::stopped());
    return EXIT_FAILURE;
}
````

修改 `tests/CMakeLists.txt`：

````diff
diff --git a/tests/CMakeLists.txt b/tests/CMakeLists.txt
index a79dd99..728238e 100644
--- a/tests/CMakeLists.txt
+++ b/tests/CMakeLists.txt
@@ -11,7 +11,8 @@ set(co2_test_sources
     callback_tests.cpp
     allocation_tests.cpp
     edge_case_tests.cpp
-    vocabulary_tests.cpp)
+    vocabulary_tests.cpp
+    completion_tests.cpp)
 
 # 契约违规测试：程序在预期的失败点经由契约处理器以退出码 86 结束。
 set(co2_contract_violation_test_sources
@@ -24,7 +25,8 @@ set(co2_contract_violation_test_sources
     async_generator_destroy_contract_violation_test.cpp
     continuation_twice_contract_violation_test.cpp
     optional_empty_value_contract_violation_test.cpp
-    error_from_success_code_contract_violation_test.cpp)
+    error_from_success_code_contract_violation_test.cpp
+    stopped_without_entry_contract_violation_test.cpp)
 
 # 构建测试用的 C++ 标准。库的最低要求是 14，默认就测它；CI 另以 17 / 20 / 23 各跑一遍，
 # 保证较新的标准下（弃用告警、更严格的规则）同样干净。
````

- [ ] **Step 2: 构建并运行，确认失败**

Expected: 能编译（多出来的成员只是没人调用）。`ctest` 报两个失败：`co2-completion-tests` 与 `co2-stopped-without-entry-contract-violation-test`。单独运行前者：

```bash
/tmp/co2-build/gcc14/tests/co2-completion-tests
```

Expected: `check failed at line 219: coroutine.promise().outcome == Outcome::stopped`——等待之后的语句照常执行了，promise 没有收到"已停止"。

- [ ] **Step 3: 探测 `await_completion()`，`AwaiterRef` 转发它**

修改 `include/co2/detail/awaitable.hpp`：

````diff
diff --git a/include/co2/detail/awaitable.hpp b/include/co2/detail/awaitable.hpp
index e8a7722..9b2e16a 100644
--- a/include/co2/detail/awaitable.hpp
+++ b/include/co2/detail/awaitable.hpp
@@ -88,10 +88,31 @@ template <class Awaiter> struct AwaiterRef {
         return target->await_resume();
     }
 
+    // 可选成员也要转发：没有它，按左值等待的 awaiter 会悄悄丢掉"出错 / 已停止"这两种
+    // 结果。原对象没有这个成员时，这里也没有。
+    template <class A = Awaiter>
+    auto await_completion() -> decltype(std::declval<A&>().await_completion()) {
+        return target->await_completion();
+    }
+
   private:
     Awaiter* target;
 };
 
+// ---- await_completion()：co2 对 awaiter 协议的扩展 ----
+//
+// awaiter 可以多提供一个成员 `co2::Completion await_completion() noexcept`，报告这次等待
+// 是怎么结束的：有值、出错、还是已停止。宏生成的代码对每次等待恰好调用它一次，先于
+// await_resume()；只有"有值"才接着调用 await_resume()。没有这个成员的 awaiter——标准
+// 写法的所有 awaiter——在编译期就没有这段检查。
+template <class Awaiter> struct HasAwaitCompletion {
+    template <class A>
+    static auto probe(int)
+        -> decltype(std::declval<A&>().await_completion(), std::true_type{});
+    template <class> static std::false_type probe(long);
+    using type = decltype(probe<Awaiter>(0));
+};
+
 template <class AwaiterExpression> struct AwaiterStorage {
     using type = typename std::decay<AwaiterExpression>::type;
 };
````

- [ ] **Step 4: 恢复点的收尾逻辑**

修改 `include/co2/detail/frame.hpp`：

````diff
diff --git a/include/co2/detail/frame.hpp b/include/co2/detail/frame.hpp
index ed2be8f..54623d2 100644
--- a/include/co2/detail/frame.hpp
+++ b/include/co2/detail/frame.hpp
@@ -8,6 +8,7 @@
 #include <type_traits>
 #include <utility>
 
+#include "co2/completion.hpp"
 #include "co2/contract.hpp"
 #include "co2/coroutine_handle.hpp"
 #include "co2/detail/await_slot.hpp"
@@ -49,6 +50,31 @@ template <class Promise>
 struct AwaitStorageSize
     : std::integral_constant<std::size_t, CO2_AWAIT_STORAGE_SIZE> {};
 
+// promise 上接收"没有值"的两个入口，都是可选的：
+//
+//   void return_stopped() noexcept;          // 协程以已停止结束
+//   void return_error(co2::Error) noexcept;  // 协程以错误结束
+//
+// 名字比照 return_value / return_void。它们只记录结果，向等待者转交由随后的
+// final_suspend 完成。与 P2300 的 unhandled_stopped() 不同名是有意的：那个成员返回要
+// 恢复的句柄，调用它之后协程不再被恢复；这里的入口在协程体收尾之后被调用。
+template <class Promise> struct HasReturnStopped {
+    template <class P>
+    static auto probe(int)
+        -> decltype(std::declval<P&>().return_stopped(), std::true_type{});
+    template <class> static std::false_type probe(long);
+    using type = decltype(probe<Promise>(0));
+};
+
+template <class Promise> struct HasReturnError {
+    template <class P>
+    static auto probe(int)
+        -> decltype(std::declval<P&>().return_error(std::declval<Error>()),
+                    std::true_type{});
+    template <class> static std::false_type probe(long);
+    using type = decltype(probe<Promise>(0));
+};
+
 // 帧中与 allocator 无关的部分。Body 的 operator() 以它为上下文，因此每个协程体只
 // 实例化一份 step 逻辑，不随 allocator 变化。
 template <class Promise, class Params, class Body> struct FrameCore {
@@ -152,7 +178,66 @@ template <class Promise, class Params, class Body> struct FrameCore {
         return outcome.next;
     }
 
+    // 恢复点：在 await_resume() 之前问 awaiter 这次等待是怎么结束的。
+    //
+    // 返回 false：有值（或者 awaiter 没有 await_completion()，那就一定有值），生成的代码
+    // 照常调用 await_resume() 并继续。返回 true：出错或已停止，协程已经就此结束——
+    // awaiter 与局部已销毁，promise 已收到结果，final_suspend 已经求值——`next` 是要转移
+    // 到的目标，Body 必须立刻返回它，不能再碰自己的任何成员。
+    template <class Awaiter> bool finishUnlessValue(FrameHeader*& next) {
+        return finishUnlessValue<Awaiter>(next,
+                                          typename HasAwaitCompletion<Awaiter>::type{});
+    }
+
   private:
+    template <class Awaiter>
+    bool finishUnlessValue(FrameHeader*&, std::false_type) noexcept {
+        return false;
+    }
+
+    template <class Awaiter>
+    bool finishUnlessValue(FrameHeader*& next, std::true_type) {
+        Completion completion = awaiter<Awaiter>().await_completion();
+        if (completion.isValue()) return false;
+        next = completion.isStopped()
+                   ? finishStopped<Awaiter>(typename HasReturnStopped<Promise>::type{})
+                   : finishFailed<Awaiter>(std::move(completion).error(),
+                                           typename HasReturnError<Promise>::type{});
+        return true;
+    }
+
+    // 收尾的顺序与异常逃出协程体时相同：awaiter → 局部 → promise 的入口 →
+    // final_suspend。
+    template <class Awaiter> FrameHeader* finishStopped(std::true_type) noexcept {
+        resetAwaiterAs<Awaiter>();
+        destroyBody();
+        promise().return_stopped();
+        return enterFinalSuspend();
+    }
+
+    // promise 没有 return_stopped()：已停止既不是值，也不是能抛进协程体的错误。
+    template <class Awaiter>
+    [[noreturn]] FrameHeader* finishStopped(std::false_type) noexcept {
+        CO2_CONTRACT_FAIL("a stopped completion reached a promise that has no "
+                          "return_stopped()");
+    }
+
+    template <class Awaiter>
+    FrameHeader* finishFailed(Error&& error, std::true_type) noexcept {
+        resetAwaiterAs<Awaiter>();
+        destroyBody();
+        promise().return_error(std::move(error));
+        return enterFinalSuspend();
+    }
+
+    // promise 没有 return_error()：在协程体内重新抛出，和 await_resume() 抛出一样落进
+    // step 的 catch——awaiter 与局部由那里销毁，promise 收到 unhandled_exception()。为
+    // 标准写的 promise 因此照常工作。
+    template <class Awaiter>
+    [[noreturn]] FrameHeader* finishFailed(Error&& error, std::false_type) {
+        error.raise();
+    }
+
     template <class P = Promise>
     auto returnVoidAtEnd(int) -> decltype(std::declval<P&>().return_void()) {
         return promise().return_void();
````

- [ ] **Step 5: 宏的恢复点多出一步**

修改 `include/co2/dsl.hpp`：

````diff
diff --git a/include/co2/dsl.hpp b/include/co2/dsl.hpp
index 53a1dc6..dd8a6e3 100644
--- a/include/co2/dsl.hpp
+++ b/include/co2/dsl.hpp
@@ -198,6 +198,10 @@
 // await_ready → await_suspend（void / bool / coroutine_handle）→ await_resume。
 // `awaiter_type` 是最终 awaiter 的类型（括号包裹，允许含逗号的模板实参）；
 // `awaitable_expression` 已经完成 await_transform。
+//
+// 恢复点（case 标签之后）比标准多一步：awaiter 有 await_completion() 时先问它这次等待
+// 是怎么结束的；出错或已停止就此结束协程，不调用 await_resume()，见
+// FrameCore::finishUnlessValue。
 #define CO2_DETAIL_AWAIT_IMPL(awaiter_type, resume, awaitable_expression, unique)      \
     do {                                                                               \
         using _co2_awaiter_type = CO2_DETAIL_PP_REMOVE_PARENS_I awaiter_type;          \
@@ -214,6 +218,11 @@
             _co2_context.markRunning();                                                \
         }                                                                              \
     case CO2_DETAIL_SUSPEND_POINT(unique):                                             \
+        {                                                                              \
+            ::co2::detail::FrameHeader* _co2_next = nullptr;                           \
+            if (_co2_context.template finishUnlessValue<_co2_awaiter_type>(_co2_next)) \
+                return _co2_next;                                                      \
+        }                                                                              \
         resume(_co2_context.template awaiter<_co2_awaiter_type>().await_resume());     \
         _co2_context.template resetAwaiterAs<_co2_awaiter_type>();                     \
     } while (false)
````

- [ ] **Step 6: 构建并运行全部测试，确认通过**

Expected: `100% tests passed, 0 tests failed out of 27`。

差分测试（`co2-differential-tests`）在其中：它用的 awaiter 没有 `await_completion()`，promise 没有那两个入口，走的仍是标准路径。再用 C++20 确认一次：

```bash
cmake -S . -B /tmp/co2-build/gcc20 -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=g++ -DCO2_TEST_CXX_STANDARD=20
cmake --build /tmp/co2-build/gcc20 -j 12 && ctest --test-dir /tmp/co2-build/gcc20 -j 8
```

Expected: `100% tests passed, 0 tests failed out of 27`。

- [ ] **Step 7: 提交**

```bash
git add include/co2/detail/awaitable.hpp include/co2/detail/frame.hpp include/co2/dsl.hpp \
        tests/CMakeLists.txt tests/completion_tests.cpp \
        tests/stopped_without_entry_contract_violation_test.cpp
git commit -F - <<'EOF'
恢复点询问 await_completion()，promise 新增两个入口

核心机制（docs/design/completion-channels.md 第 3 节）。awaiter 可以多提供一个成员
co2::Completion await_completion() noexcept；CO2_AWAIT / CO2_YIELD 的恢复点在
await_resume() 之前调用它。有值照常继续；出错或已停止就此结束协程：销毁 awaiter、
析构局部、把结果交给 promise 的 return_error() / return_stopped()、co_await
final_suspend()——顺序与异常逃出协程体时相同。

- 没有这个成员的 awaiter 在编译期就没有这段检查。
- promise 没有 return_error() 时错误在协程体内重新抛出，走 unhandled_exception()，
  为标准写的 promise 不受影响；没有 return_stopped() 时"已停止"是契约违规。
- detail::AwaiterRef（左值 awaiter 的帧内形式）转发这个成员。

入口不叫 unhandled_stopped()：标准里那个成员返回要恢复的句柄、调用之后协程不再
恢复，契约不同。

这一步库里还没有任何组件产生或接收新结果，用测试自己写的 awaiter 和 promise 验证。
EOF
```

### Task 5: `Task` 以值、错误或已停止结束

设计稿 3.4、3.5 节、第 10 节第 5 步的主体。

**Files:**
- Create: `tests/stopped_through_legacy_awaiter_contract_violation_test.cpp`
- Modify: `tests/completion_tests.cpp`、`tests/CMakeLists.txt`
- Modify: `include/co2/task.hpp`

**Interfaces:**
- Consumes: 任务 4 的恢复点与两个入口；任务 3 的 `TaskAccess::takeRootResult`；测试里的 `Ends`、`stopped()`、`failed()`、`CHECK`（任务 4）。
- Produces:
  - `enum class co2::detail::TaskState : unsigned char { empty, value, error, stopped }`；`template <class T> union co2::detail::TaskStorage`（成员 `value`、`error`；`void` 的特化只有 `error`）。
  - `TaskPromise<T>` 的成员：`void return_error(Error) noexcept`、`void return_stopped() noexcept`、`Completion completion() noexcept`（把错误移出去，每次等待只调用一次）、`T result()`（出错时 `raise()`，已停止或没有值时契约违规）；数据成员 `TaskStorage<T> storage`、`TaskState state`。原来的 `error`（`exception_ptr`）与 `storage`（`ResultStorage<T>`）不再存在。
  - `Completion co2::detail::TaskAwaiter<T>::await_completion() noexcept`、`Completion co2::Task<T>::await_completion() noexcept`。
  - `TaskAccess::takeRootResult`：已停止返回空的 optional，出错时抛出。`TaskAccess::hasError`、`TaskAccess::rethrowError` 的含义不变（`whenAll` 在用）。
  - 测试里后面的任务要用的东西：`ExpectedError`；`enum class Leaf { returns, fails, stops, throws }`；协程 `chain(int depth, Leaf leaf, int& resumed, std::vector<int>* order)`，返回 `co2::Task<int>`，值是 `depth + 1`。

`whenAll`、等待 `JoinHandle`、`AsyncGenerator`、`fromCallback` 在这一步不改：它们调用的是 `await_resume()` 那条路，出错时照旧拿到异常。

- [ ] **Step 1: 写测试**

修改 `tests/completion_tests.cpp`：

````diff
diff --git a/tests/completion_tests.cpp b/tests/completion_tests.cpp
index 7396ebf..175b0c8 100644
--- a/tests/completion_tests.cpp
+++ b/tests/completion_tests.cpp
@@ -16,6 +16,11 @@
 #include "co2/completion.hpp"
 #include "co2/coroutine.hpp"
 #include "co2/error.hpp"
+#include "co2/manual_executor.hpp"
+#include "co2/optional.hpp"
+#include "co2/spawn.hpp"
+#include "co2/sync_wait.hpp"
+#include "co2/task.hpp"
 #include "sample_errc.hpp"
 
 namespace {
@@ -398,6 +403,186 @@ void aCapturedExceptionReachesAStandardPromiseUnchanged() {
     CHECK(sawOriginal);
 }
 
+// ---------------------------------------------------------------------------
+// Task：错误与已停止按值沿等待链向上传，只在根上变回异常或空的 optional
+
+struct ExpectedError : std::runtime_error {
+    ExpectedError() : std::runtime_error{"expected"} {}
+};
+
+enum class Leaf { returns, fails, stops, throws };
+
+// 记下每一层的局部是按什么顺序析构的。
+struct DepthGuard {
+    std::vector<int>* order{};
+    int depth{};
+    ~DepthGuard() {
+        if (order != nullptr) order->push_back(depth);
+    }
+};
+
+// 一条 depth + 1 层的等待链：最里面一层按 leaf 结束，外面每一层等待里面一层、把结果
+// 加一。`resumed` 数的是有多少层在等待之后继续执行了。
+auto chain(int depth, Leaf leaf, int& resumed, std::vector<int>* order)
+    CO2_BEG(co2::Task<int>, (depth, leaf, resumed, order), int value{};
+            DepthGuard guard;) {
+    guard.order = order;
+    guard.depth = depth;
+    if (depth == 0) {
+        if (leaf == Leaf::throws) throw ExpectedError{};
+        if (leaf == Leaf::fails) CO2_AWAIT(Ends{failed()});
+        if (leaf == Leaf::stops) CO2_AWAIT(Ends{stopped()});
+        CO2_RETURN(1);
+    }
+    CO2_AWAIT_SET(value, chain(depth - 1, leaf, resumed, order));
+    ++resumed;
+    CO2_RETURN(value + 1);
+}
+CO2_END
+
+void valuesStillFlowThroughTheChain() {
+    auto resumed = 0;
+    CHECK(co2::syncWait(chain(3, Leaf::returns, resumed, nullptr)) == 4);
+    CHECK(resumed == 3);
+}
+
+void anErrorPassesThroughEveryLayerAndIsThrownAtTheRoot() {
+    auto resumed = 0;
+    auto order = std::vector<int>{};
+    auto sawCode = false;
+    try {
+        co2::syncWait(chain(3, Leaf::fails, resumed, &order));
+    } catch (std::system_error const& exception) {
+        sawCode = exception.code() == sample::Errc::gone;
+    }
+    CHECK(sawCode);
+    CHECK(resumed == 0); // 没有一层在等待之后继续执行
+    auto const innerFirst = std::vector<int>{0, 1, 2, 3};
+    CHECK(order == innerFirst); // 局部由内向外析构
+}
+
+void aStoppedTaskEndsEveryAwaiterAndTheRootReturnsEmpty() {
+    auto resumed = 0;
+    auto order = std::vector<int>{};
+    auto const result = co2::syncWait(chain(3, Leaf::stops, resumed, &order));
+    CHECK(not result.has_value());
+    CHECK(resumed == 0);
+    auto const innerFirst = std::vector<int>{0, 1, 2, 3};
+    CHECK(order == innerFirst);
+}
+
+void anExceptionIsRethrownUnchangedAtTheRoot() {
+    auto resumed = 0;
+    auto sawOriginal = false;
+    try {
+        co2::syncWait(chain(3, Leaf::throws, resumed, nullptr));
+    } catch (ExpectedError const&) {
+        sawOriginal = true;
+    }
+    CHECK(sawOriginal);
+    CHECK(resumed == 0);
+}
+
+void joinSeesTheSameThreeOutcomes() {
+    co2::ManualExecutor executor;
+    auto resumed = 0;
+    auto value = co2::spawn(executor, chain(1, Leaf::returns, resumed, nullptr));
+    auto stops = co2::spawn(executor, chain(1, Leaf::stops, resumed, nullptr));
+    auto fails = co2::spawn(executor, chain(1, Leaf::fails, resumed, nullptr));
+    CHECK(executor.run() == 3U);
+
+    CHECK(value.join() == 2);
+    CHECK(not stops.join().has_value());
+    auto sawCode = false;
+    try {
+        fails.join();
+    } catch (std::system_error const& exception) {
+        sawCode = exception.code() == sample::Errc::gone;
+    }
+    CHECK(sawCode);
+}
+
+// 每一层都由恢复循环驱动：十万层的链上传播不长栈，析构也不递归。
+void aDeepChainPropagatesWithoutGrowingTheStack() {
+    constexpr int depth = 100000;
+    auto resumed = 0;
+    CHECK(not co2::syncWait(chain(depth, Leaf::stops, resumed, nullptr)).has_value());
+
+    auto sawCode = false;
+    try {
+        co2::syncWait(chain(depth, Leaf::fails, resumed, nullptr));
+    } catch (std::system_error const& exception) {
+        sawCode = exception.code() == sample::Errc::gone;
+    }
+    CHECK(sawCode);
+    CHECK(resumed == 0);
+}
+
+auto awaitsAnLvalueTask(Leaf leaf, int& resumed, bool& reached)
+    CO2_BEG(co2::Task<int>, (leaf, resumed, reached), co2::Task<int> child;
+            int value{};) {
+    child = chain(0, leaf, resumed, nullptr);
+    CO2_AWAIT_SET(value, child);
+    reached = true;
+    CO2_RETURN(value);
+}
+CO2_END
+
+void anLvalueTaskPropagatesTheSameWay() {
+    auto resumed = 0;
+    auto reached = false;
+    CHECK(not co2::syncWait(awaitsAnLvalueTask(Leaf::stops, resumed, reached))
+                  .has_value());
+    CHECK(not reached);
+    CHECK(co2::syncWait(awaitsAnLvalueTask(Leaf::returns, resumed, reached)) == 1);
+    CHECK(reached);
+}
+
+// 只转发标准三个成员的包装 awaiter：它不认识 await_completion()，所以被包住的 Task 的
+// 结果只能经 await_resume() 出来。
+struct Forwards {
+    co2::Task<int> task;
+
+    bool await_ready() const noexcept { return task.await_ready(); }
+
+    template <class Promise>
+    co2::coroutine_handle<>
+    await_suspend(co2::coroutine_handle<Promise> const awaiting) noexcept {
+        return task.await_suspend(awaiting);
+    }
+
+    int await_resume() { return task.await_resume(); }
+};
+
+auto awaitsThroughAWrapper(Leaf leaf, int& resumed)
+    CO2_BEG(co2::Task<int>, (leaf, resumed), int value{};) {
+    CO2_AWAIT_SET(value, Forwards{chain(0, leaf, resumed, nullptr)});
+    CO2_RETURN(value + 10);
+}
+CO2_END
+
+// 错误照旧能穿过旧式的调用方：await_resume() 把它抛出来。
+void anErrorStillCrossesAnAwaiterThatOnlyKnowsThreeMembers() {
+    auto resumed = 0;
+    CHECK(co2::syncWait(awaitsThroughAWrapper(Leaf::returns, resumed)) == 11);
+
+    auto sawCode = false;
+    try {
+        co2::syncWait(awaitsThroughAWrapper(Leaf::fails, resumed));
+    } catch (std::system_error const& exception) {
+        sawCode = exception.code() == sample::Errc::gone;
+    }
+    CHECK(sawCode);
+
+    auto sawOriginal = false;
+    try {
+        co2::syncWait(awaitsThroughAWrapper(Leaf::throws, resumed));
+    } catch (ExpectedError const&) {
+        sawOriginal = true;
+    }
+    CHECK(sawOriginal);
+}
+
 } // namespace
 
 int main() {
@@ -412,5 +597,13 @@ int main() {
     aCoroutineThatDoesNotSuspendAtTheEndDestroysItself();
     anErrorCodeReachesAStandardPromiseAsSystemError();
     aCapturedExceptionReachesAStandardPromiseUnchanged();
+    valuesStillFlowThroughTheChain();
+    anErrorPassesThroughEveryLayerAndIsThrownAtTheRoot();
+    aStoppedTaskEndsEveryAwaiterAndTheRootReturnsEmpty();
+    anExceptionIsRethrownUnchangedAtTheRoot();
+    joinSeesTheSameThreeOutcomes();
+    aDeepChainPropagatesWithoutGrowingTheStack();
+    anLvalueTaskPropagatesTheSameWay();
+    anErrorStillCrossesAnAwaiterThatOnlyKnowsThreeMembers();
     return 0;
 }
````

新建 `tests/stopped_through_legacy_awaiter_contract_violation_test.cpp`：

````cpp
#include <atomic>
#include <cstdlib>
#include <cstring>

#include "co2/completion.hpp"
#include "co2/coroutine.hpp"
#include "co2/sync_wait.hpp"
#include "co2/task.hpp"

namespace {

std::atomic<bool> atExpectedFailurePoint{};

// 预期的违规由 Task 的 await_resume() 报告（task.hpp）。别处报告的违规——例如"已停止"
// 根本到不了 Task 的 promise——不算数。
void contractViolationHandler(co2::ContractViolation const& violation) {
    auto const expected = atExpectedFailurePoint.load(std::memory_order_acquire) &&
                          std::strstr(violation.file, "task.hpp") != nullptr;
    std::_Exit(expected ? 86 : 87);
}

// 立刻就绪，以已停止结束。
struct Stops {
    bool await_ready() const noexcept { return true; }
    void await_suspend(co2::coroutine_handle<>) const noexcept {}
    void await_resume() const noexcept {}
    co2::Completion await_completion() noexcept { return co2::Completion::stopped(); }
};

auto stops(bool stop) CO2_BEG(co2::Task<int>, (stop)) {
    if (stop) CO2_AWAIT(Stops{});
    CO2_RETURN(1);
}
CO2_END

// 只转发标准三个成员的包装 awaiter：它不认识 await_completion()。
struct Forwards {
    co2::Task<int> task;

    bool await_ready() const noexcept { return task.await_ready(); }

    template <class Promise>
    co2::coroutine_handle<>
    await_suspend(co2::coroutine_handle<Promise> const awaiting) noexcept {
        return task.await_suspend(awaiting);
    }

    int await_resume() { return task.await_resume(); }
};

auto awaitsThroughAWrapper(bool stop) CO2_BEG(co2::Task<int>, (stop), int value{};) {
    CO2_AWAIT_SET(value, Forwards{stops(stop)});
    CO2_RETURN(value);
}
CO2_END

} // namespace

// "已停止"只能经 await_completion() 传出来。不认识这个成员的调用方直接调用
// await_resume()，而一个已停止的 Task 没有值可以交给它：契约违规。包装 awaiter 需要把
// await_completion() 也转发出去。
int main() {
    co2::setContractViolationHandler(&contractViolationHandler);
    if (co2::syncWait(awaitsThroughAWrapper(false)) != 1) return EXIT_FAILURE;

    atExpectedFailurePoint.store(true, std::memory_order_release);
    co2::syncWait(awaitsThroughAWrapper(true));
    return EXIT_FAILURE;
}
````

修改 `tests/CMakeLists.txt`：

````diff
diff --git a/tests/CMakeLists.txt b/tests/CMakeLists.txt
index 728238e..b320dde 100644
--- a/tests/CMakeLists.txt
+++ b/tests/CMakeLists.txt
@@ -26,7 +26,8 @@ set(co2_contract_violation_test_sources
     continuation_twice_contract_violation_test.cpp
     optional_empty_value_contract_violation_test.cpp
     error_from_success_code_contract_violation_test.cpp
-    stopped_without_entry_contract_violation_test.cpp)
+    stopped_without_entry_contract_violation_test.cpp
+    stopped_through_legacy_awaiter_contract_violation_test.cpp)
 
 # 构建测试用的 C++ 标准。库的最低要求是 14，默认就测它；CI 另以 17 / 20 / 23 各跑一遍，
 # 保证较新的标准下（弃用告警、更严格的规则）同样干净。
````

- [ ] **Step 2: 构建并运行，确认失败**

Expected: 能编译。`ctest` 报两个失败：

- `co2-completion-tests` 异常终止（`terminate called without an active exception`）：`Task` 的 promise 还没有 `return_stopped()`，"已停止"送过去是契约违规。
- `co2-stopped-through-legacy-awaiter-contract-violation-test`：退出码是 87 而不是 86——违规确实发生了，但不是由 `task.hpp` 报告的。

- [ ] **Step 3: 实现**

修改 `include/co2/task.hpp`：

````diff
diff --git a/include/co2/task.hpp b/include/co2/task.hpp
index 31ac9fd..d9bcb06 100644
--- a/include/co2/task.hpp
+++ b/include/co2/task.hpp
@@ -4,10 +4,14 @@
 #include <type_traits>
 #include <utility>
 
+#include <memory>
+#include <new>
+
+#include "co2/completion.hpp"
 #include "co2/contract.hpp"
 #include "co2/coroutine.hpp"
-#include "co2/detail/result_storage.hpp"
 #include "co2/env.hpp"
+#include "co2/error.hpp"
 #include "co2/optional.hpp"
 #include "co2/result.hpp"
 #include "co2/stop_token.hpp"
@@ -23,6 +27,11 @@
 // CO2_AWAIT_SET(token, co2::getStopToken()) 读取。继承是借用而不是拷贝：父在子运行期间
 // 一直挂着，子 promise 只记下父的停止状态，不碰引用计数（detail::EnvStopToken）。
 //
+// 三种完成方式（docs/design/completion-channels.md）：Task 以值、错误或已停止结束。
+// 错误与已停止按值沿等待链向上传——等待它的协程不再往下执行，以同样的结果结束——只在
+// 根（syncWait、JoinHandle::join）上变回异常或空的 optional。协程体里抛出的异常在
+// unhandled_exception() 里被捕获成一个 Error，此后同样按值传，中间各层不再重抛。
+//
 // 销毁契约（标准规则）：一个已经启动、尚未完成的 Task 不能销毁——它挂起在某个操作
 // 上，而操作只有完成后才能销毁其状态。co2 把这条未定义行为报告为契约违规。取消是
 // 协作式的：请求 stop → 操作提前完成 → Task 走到 final suspend → owner 销毁。
@@ -35,10 +44,40 @@ namespace detail {
 
 struct TaskAccess;
 
+// Task 的结果。empty：还没结束，或者值已经被取走。
+enum class TaskState : unsigned char { empty, value, error, stopped };
+
+// 值与错误共用的一块存储，自己不带标记：哪个成员活着由 TaskPromise::state 说明。两者
+// 互斥，分开存是白占空间；标记放在 promise 里则是为了和 started 挨在一起，共占一个对齐
+// 单位。于是 promise 与从前只存 exception_ptr 的时候一样大，帧不变大。
+template <class T> union TaskStorage {
+    TaskStorage() noexcept {}
+    ~TaskStorage() {}
+
+    T value;
+    Error error;
+};
+
+template <> union TaskStorage<void> {
+    TaskStorage() noexcept {}
+    ~TaskStorage() {}
+
+    Error error;
+};
+
 template <class T> struct TaskPromise {
     // 声明构造函数是为了让它不是聚合类型。聚合在 C++20 起可以用圆括号初始化，"能用参数
     // 副本构造 promise 就传入"的规则会因此拿协程的第一个参数去初始化 continuation。
     TaskPromise() = default;
+    TaskPromise(TaskPromise const&) = delete;
+    TaskPromise& operator=(TaskPromise const&) = delete;
+
+    ~TaskPromise() {
+        if (state == TaskState::value)
+            destroyValue(std::is_void<T>{});
+        else if (state == TaskState::error)
+            storage.error.~Error();
+    }
 
     Task<T> get_return_object() noexcept;
 
@@ -58,19 +97,28 @@ template <class T> struct TaskPromise {
 
     FinalAwaiter final_suspend() noexcept { return {}; }
 
+    // 圆括号是有意的：泛型 T 上花括号可能选中 initializer_list 构造函数。
     template <class Value, class U = T,
               class = typename std::enable_if<not std::is_void<U>::value>::type>
     void return_value(Value&& value) {
-        storage.emplace(std::forward<Value>(value));
+        ::new (static_cast<void*>(std::addressof(storage.value)))
+            U(std::forward<Value>(value));
+        state = TaskState::value;
     }
 
     template <class U = T,
               class = typename std::enable_if<std::is_void<U>::value>::type>
     void return_void() noexcept {
-        storage.emplace();
+        state = TaskState::value;
     }
 
-    void unhandled_exception() noexcept { error = std::current_exception(); }
+    // 异常只在这里被捕获一次；之后它是一个按值传的 Error，到根上才重新抛出。
+    void unhandled_exception() noexcept { fail(Error{std::current_exception()}); }
+
+    // 等待的操作出错 / 已停止（或者体内 CO2_AWAIT(just_error(e)) 之类）：本 Task 以同样的
+    // 结果结束。由恢复点在协程体收尾之后调用，见 FrameCore::finishUnlessValue。
+    void return_error(Error error) noexcept { fail(std::move(error)); }
+    void return_stopped() noexcept { state = TaskState::stopped; }
 
     // ---- 环境 ----
 
@@ -90,18 +138,50 @@ template <class T> struct TaskPromise {
         return std::forward<Awaitable>(awaitable);
     }
 
-    // 只可消费一次：值被移出。
+    // 这个 Task 是怎么结束的。给认识 await_completion() 的等待者用：错误被移出去，所以
+    // 每次等待只问一次。值已被取走（empty）时也报告"有值"，由随后的 result() 诊断。
+    Completion completion() noexcept {
+        if (state == TaskState::stopped) return Completion::stopped();
+        if (state == TaskState::error) return Completion::error(std::move(storage.error));
+        return Completion::value();
+    }
+
+    // 取走值，只可一次。它同时是不认识 await_completion() 的调用方的兜底（手写的包装
+    // awaiter、原生 co_await 都直接调用 await_resume()）：错误在这里变回异常，与从前
+    // 一样；已停止没有值可给，是契约违规。
     T result() {
-        if (error) std::rethrow_exception(error);
-        CO2_CONTRACT_CHECK(storage.hasValue());
-        return storage.take();
+        if (state == TaskState::error) storage.error.raise();
+        CO2_CONTRACT_CHECK(state == TaskState::value);
+        return takeValue(std::is_void<T>{});
     }
 
     coroutine_handle<> continuation;
-    std::exception_ptr error;
-    ResultStorage<T> storage;
+    TaskStorage<T> storage;
     EnvStopToken stopToken; // 根持有引用；被等待的子 Task 借用等待者的
+    TaskState state{TaskState::empty};
     bool started{};
+
+  private:
+    void fail(Error error) noexcept {
+        ::new (static_cast<void*>(std::addressof(storage.error))) Error{std::move(error)};
+        state = TaskState::error;
+    }
+
+    // 移动构造抛出时值留在原处、state 不变，析构函数仍会清理。
+    template <class U = T> U takeValue(std::false_type) {
+        U result(std::move(storage.value));
+        storage.value.~U();
+        state = TaskState::empty;
+        return result;
+    }
+
+    void takeValue(std::true_type) noexcept { state = TaskState::empty; }
+
+    template <class U = T> void destroyValue(std::false_type) noexcept {
+        storage.value.~U();
+    }
+
+    void destroyValue(std::true_type) noexcept {}
 };
 
 // 等待一个 Task：记下等待者，对称转移进 Task；Task 完成时 FinalAwaiter 转移回来。
@@ -127,6 +207,11 @@ template <class T> struct TaskAwaiter {
         CO2_CONTRACT_CHECK(handle);
         return handle.promise().result();
     }
+
+    Completion await_completion() noexcept {
+        CO2_CONTRACT_CHECK(handle);
+        return handle.promise().completion();
+    }
 };
 
 } // namespace detail
@@ -168,6 +253,8 @@ template <class T> struct Task {
 
     T await_resume() { return awaiter().await_resume(); }
 
+    Completion await_completion() noexcept { return awaiter().await_completion(); }
+
     // 左值（CO2_AWAIT(task)）：借用句柄，Task 继续拥有帧。
     detail::TaskAwaiter<T> operator_co_await() & noexcept { return awaiter(); }
 
@@ -236,18 +323,20 @@ struct TaskAccess {
         return task.handle.promise().result();
     }
 
-    // 根取走结果（只可一次）。
+    // 根取走结果（只可一次）：已停止是空的 optional，错误在这里变回异常。
     template <class T> static RootResult<T> takeRootResult(Task<T>& task) {
+        CO2_CONTRACT_CHECK(task.handle && task.handle.done());
+        if (task.handle.promise().state == TaskState::stopped) return RootResult<T>{};
         return takeRootResult(task, std::is_void<T>{});
     }
 
     template <class T> static bool hasError(Task<T> const& task) noexcept {
-        return task.handle && task.handle.promise().error != nullptr;
+        return task.handle && task.handle.promise().state == TaskState::error;
     }
 
     template <class T> static void rethrowError(Task<T>& task) {
         CO2_CONTRACT_CHECK(hasError(task));
-        std::rethrow_exception(task.handle.promise().error);
+        task.handle.promise().storage.error.raise();
     }
 
     // 交出帧的所有权（类型擦除）：调用方负责在完成后 destroy()。
````

- [ ] **Step 4: 构建并运行全部测试，确认通过**

Expected: `100% tests passed, 0 tests failed out of 28`。其中 `co2-allocation-tests` 检查 `Task<int>(int)` 的帧仍是 176 字节：它通过，说明值与错误共用存储之后 promise 没有变大。

- [ ] **Step 5: 在 ASan + UBSan 下确认十万层的链**

```bash
cmake -S . -B /tmp/co2-build/asan -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=clang++ \
      -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
cmake --build /tmp/co2-build/asan -j 12
ASAN_OPTIONS=detect_leaks=0 ctest --test-dir /tmp/co2-build/asan -j 8
```

Expected: `100% tests passed, 0 tests failed out of 28`。`aDeepChainPropagatesWithoutGrowingTheStack` 在默认的 8 MB 栈上通过，说明传播与析构都不递归。

- [ ] **Step 6: 量一下基准**

```bash
cmake --build /tmp/co2-build/bench -j 12
/tmp/co2-build/bench/benchmarks/co2-benchmarks Task
```

Expected: `Task failure through a 100-deep chain, per layer` 从约 558 ns 降到约 45 ns；`symmetric Task transfer` 仍约 12 ns；`syncWait ready Task` 约 26～27 ns。

- [ ] **Step 7: 提交**

```bash
git add include/co2/task.hpp tests/CMakeLists.txt tests/completion_tests.cpp \
        tests/stopped_through_legacy_awaiter_contract_violation_test.cpp
git commit -F - <<'EOF'
Task 以值、错误或已停止结束，错误按值向上传

docs/design/completion-channels.md 第 3.4 节、第 10 节第 5 步。

- TaskPromise 的结果变成四态。值与错误共用一块存储，状态标记与 started 相邻，
  promise 与只存 exception_ptr 时一样大：Task<int>(int) 的帧仍是 176 字节。
- 协程体里抛出的异常在 unhandled_exception() 里被捕获一次，之后按值传。等待它的
  协程经 await_completion() 收到，不再执行其后的语句，以同一个错误结束。到根上
  才重新抛出：原异常原样重抛，错误码抛 std::system_error。
- 已停止同样逐层向上，syncWait / join 返回空的 optional。
- await_resume() 保持自足：不认识 await_completion() 的调用方（手写的三成员包装
  awaiter，以及还没接上的 whenAll、JoinHandle 的等待、AsyncGenerator）照旧从它
  那里拿到异常；已停止没有值可给，是契约违规。

失败沿 100 层的链每层 558 ns → 45 ns（GCC 13.3 -O2），值路径不变。十万层的链上
错误与已停止的传播不长栈（ASan + UBSan 下通过）。
EOF
```

### Task 6: `just_error` 与 `with_error`

设计稿 2.2 节。

**Files:**
- Create: `include/co2/just.hpp`
- Modify: `include/co2/task.hpp`、`include/co2/co2.hpp`
- Modify: `tests/completion_tests.cpp`

**Interfaces:**
- Consumes: `co2::Error`、`co2::Completion`（任务 2）；任务 4 的恢复点；测试里的 `Recorder`、`Outcome`（任务 4）。
- Produces:
  - `struct co2::detail::JustError { Error error; ... }`：立刻就绪；`await_completion()` 返回 `Completion::error(...)`；`await_resume()` 是 `[[noreturn]]`，调用它等于 `error.raise()`。
  - `co2::detail::JustError co2::just_error(co2::Error) noexcept`。
  - `struct co2::WithError { Error error; }`；`co2::WithError co2::with_error(co2::Error) noexcept`。
  - `co2::detail::JustError co2::detail::TaskPromise<T>::yield_value(co2::WithError) noexcept`。

- [ ] **Step 1: 写测试**

修改 `tests/completion_tests.cpp`：

````diff
diff --git a/tests/completion_tests.cpp b/tests/completion_tests.cpp
index 175b0c8..fedebdf 100644
--- a/tests/completion_tests.cpp
+++ b/tests/completion_tests.cpp
@@ -16,6 +16,7 @@
 #include "co2/completion.hpp"
 #include "co2/coroutine.hpp"
 #include "co2/error.hpp"
+#include "co2/just.hpp"
 #include "co2/manual_executor.hpp"
 #include "co2/optional.hpp"
 #include "co2/spawn.hpp"
@@ -583,6 +584,66 @@ void anErrorStillCrossesAnAwaiterThatOnlyKnowsThreeMembers() {
     CHECK(sawOriginal);
 }
 
+// ---------------------------------------------------------------------------
+// just_error 与 with_error：在协程体里直接以错误结束
+
+auto failsWithJustError(bool& reached) CO2_BEG(co2::Task<int>, (reached)) {
+    CO2_AWAIT(co2::just_error(sample::Errc::retry_later));
+    reached = true;
+    CO2_RETURN(0);
+}
+CO2_END
+
+auto failsWithWithError(bool& reached) CO2_BEG(co2::Task<int>, (reached)) {
+    CO2_YIELD(co2::with_error(sample::Errc::retry_later));
+    reached = true;
+    CO2_RETURN(0);
+}
+CO2_END
+
+bool throwsRetryLater(co2::Task<int> task) {
+    try {
+        co2::syncWait(std::move(task));
+    } catch (std::system_error const& exception) {
+        return exception.code() == sample::Errc::retry_later;
+    }
+    return false;
+}
+
+void justErrorAndWithErrorEndTheTaskWithThatError() {
+    auto reached = false;
+    CHECK(throwsRetryLater(failsWithJustError(reached)));
+    CHECK(throwsRetryLater(failsWithWithError(reached)));
+    CHECK(not reached);
+}
+
+auto recorderFailsWithJustError(bool& reached) CO2_BEG(Recorder, (reached)) {
+    CO2_AWAIT(co2::just_error(std::make_error_code(std::errc::timed_out)));
+    reached = true;
+}
+CO2_END
+
+// just_error 是普通的 awaitable：任何提供了 return_error() 的 promise 都能用。
+void justErrorWorksInAnyPromiseThatHasTheEntry() {
+    auto reached = false;
+    auto const coroutine = recorderFailsWithJustError(reached);
+    CHECK(coroutine.promise().outcome == Outcome::failed);
+    CHECK(coroutine.promise().code == std::errc::timed_out);
+    CHECK(not reached);
+}
+
+// 不认识 await_completion() 的调用方直接调用 await_resume()：错误以异常的形式出现。
+void justErrorRaisesForACallerThatSkipsAwaitCompletion() {
+    auto const awaiter = co2::just_error(sample::Errc::gone);
+    auto sawCode = false;
+    try {
+        awaiter.await_resume();
+    } catch (std::system_error const& exception) {
+        sawCode = exception.code() == sample::Errc::gone;
+    }
+    CHECK(sawCode);
+}
+
 } // namespace
 
 int main() {
@@ -605,5 +666,8 @@ int main() {
     aDeepChainPropagatesWithoutGrowingTheStack();
     anLvalueTaskPropagatesTheSameWay();
     anErrorStillCrossesAnAwaiterThatOnlyKnowsThreeMembers();
+    justErrorAndWithErrorEndTheTaskWithThatError();
+    justErrorWorksInAnyPromiseThatHasTheEntry();
+    justErrorRaisesForACallerThatSkipsAwaitCompletion();
     return 0;
 }
````

- [ ] **Step 2: 构建，确认失败**

Expected: 构建失败，`tests/completion_tests.cpp` 报 `fatal error`，说找不到 `co2/just.hpp`。

- [ ] **Step 3: 实现**

新建 `include/co2/just.hpp`：

````cpp
#pragma once

#include <utility>

#include "co2/completion.hpp"
#include "co2/config.hpp"
#include "co2/coroutine_handle.hpp"
#include "co2/error.hpp"

// co2：在协程体里直接产生"错误"这种结果。名字取自 P2300 与 P3552。
//
//   CO2_AWAIT(co2::just_error(MyErrc::retry_later));   // 当前协程以这个错误结束
//   CO2_YIELD(co2::with_error(MyErrc::retry_later));   // 同上，P3552 的另一种拼写
//
// 两者效果相同：其后的语句不执行，局部析构，协程以错误结束，等待它的协程收到同一个
// 错误。just_error 是普通的 awaitable，在任何 promise 提供了 return_error() 的协程里都
// 能用；with_error 是一个标记值，由 promise 的 yield_value 接住，只在提供了这个重载的
// 协程里能写（Task）。
//
// 参数是 co2::Error：std::error_code 与错误码枚举可以直接传；一个捕获的异常要显式写成
// co2::Error{exceptionPointer}。

namespace co2 {
namespace detail {

// 立刻就绪、以错误结束的 awaiter。
struct JustError {
    Error error;

    bool await_ready() const noexcept { return true; }
    void await_suspend(coroutine_handle<>) const noexcept {}

    // 只有不认识 await_completion() 的调用方才会调到这里：错误以异常的形式出现。
    [[noreturn]] void await_resume() const { error.raise(); }

    Completion await_completion() noexcept { return Completion::error(std::move(error)); }
};

} // namespace detail

inline detail::JustError just_error(Error error) noexcept {
    return detail::JustError{std::move(error)};
}

struct WithError {
    Error error;
};

inline WithError with_error(Error error) noexcept { return WithError{std::move(error)}; }

} // namespace co2
````

修改 `include/co2/task.hpp`、`include/co2/co2.hpp`：

````diff
diff --git a/include/co2/co2.hpp b/include/co2/co2.hpp
index 44628c9..2b90da0 100644
--- a/include/co2/co2.hpp
+++ b/include/co2/co2.hpp
@@ -10,6 +10,7 @@
 #include "co2/env.hpp"
 #include "co2/error.hpp"
 #include "co2/generator.hpp"
+#include "co2/just.hpp"
 #include "co2/manual_executor.hpp"
 #include "co2/optional.hpp"
 #include "co2/result.hpp"
diff --git a/include/co2/task.hpp b/include/co2/task.hpp
index d9bcb06..b1ad2ae 100644
--- a/include/co2/task.hpp
+++ b/include/co2/task.hpp
@@ -12,6 +12,7 @@
 #include "co2/coroutine.hpp"
 #include "co2/env.hpp"
 #include "co2/error.hpp"
+#include "co2/just.hpp"
 #include "co2/optional.hpp"
 #include "co2/result.hpp"
 #include "co2/stop_token.hpp"
@@ -120,6 +121,12 @@ template <class T> struct TaskPromise {
     void return_error(Error error) noexcept { fail(std::move(error)); }
     void return_stopped() noexcept { state = TaskState::stopped; }
 
+    // CO2_YIELD(co2::with_error(e))：P3552 的拼写，效果同 CO2_AWAIT(co2::just_error(e))。
+    // Task 不能 yield 别的东西。
+    JustError yield_value(WithError failure) noexcept {
+        return JustError{std::move(failure.error)};
+    }
+
     // ---- 环境 ----
 
     stop_token get_stop_token() const noexcept { return stopToken.get(); }
````

- [ ] **Step 4: 构建并运行全部测试，确认通过**

Expected: `100% tests passed, 0 tests failed out of 28`。

- [ ] **Step 5: 提交**

```bash
git add include/co2/just.hpp include/co2/task.hpp include/co2/co2.hpp \
        tests/completion_tests.cpp
git commit -F - <<'EOF'
新增 just_error 与 with_error

在协程体里直接以错误结束（docs/design/completion-channels.md 第 2.2 节）：

    CO2_AWAIT(co2::just_error(MyErrc::retry_later));
    CO2_YIELD(co2::with_error(MyErrc::retry_later));   // P3552 的拼写

just_error 是普通的 awaitable，任何提供了 return_error() 的 promise 都能用；
with_error 由 Task 的 yield_value 接住，效果相同。两者都不抛异常。
EOF
```

### Task 7: `asResult`

设计稿 2.3、2.4 节。

**Files:**
- Create: `include/co2/as_result.hpp`
- Modify: `include/co2/task.hpp`、`include/co2/sync_wait.hpp`、`include/co2/co2.hpp`
- Modify: `tests/completion_tests.cpp`、`tests/allocation_tests.cpp`

**Interfaces:**
- Consumes: `co2::Result<T>`、`co2::optional<T>`、`co2::Error`、`co2::Completion`（任务 2）；`co2::detail::HasAwaitCompletion`、`co2::detail::AwaiterOf`、`co2::detail::getAwaiter`（任务 4 与已有代码）；`TaskPromise` 的 `state`、`storage`（任务 5）；测试里的 `chain`、`Leaf`、`ExpectedError`（任务 5）。
- Produces:
  - `template <class Awaitable> struct co2::AsResult { Awaitable awaitable; ... operator_co_await() &&; }`；`template <class Awaitable> AsResult<Awaitable> co2::asResult(Awaitable&&)`。等待它得到 `co2::Result<R>`，`R` 是内层 `await_resume()` 的类型去掉引用与 cv（`void` 保持 `void`）。
  - `template <class Inner> struct co2::detail::AsResultAwaiter`。
  - `template <class T> static Result<T> co2::detail::TaskAccess::takeOutcome(Task<T>&)`：三种结果都不抛。
  - `template <class T> co2::optional<co2::Result<T>> co2::syncWait(co2::AsResult<co2::Task<T>>, stop_token = {})`。

- [ ] **Step 1: 写测试**

修改 `tests/completion_tests.cpp`、`tests/allocation_tests.cpp`：

````diff
diff --git a/tests/allocation_tests.cpp b/tests/allocation_tests.cpp
index 17b96b0..d96d501 100644
--- a/tests/allocation_tests.cpp
+++ b/tests/allocation_tests.cpp
@@ -13,6 +13,7 @@
 #include <utility>
 #include <vector>
 
+#include "co2/as_result.hpp"
 #include "co2/callback.hpp"
 #include "co2/coroutine.hpp"
 #include "co2/detail/await_slot.hpp"
@@ -113,6 +114,12 @@ void taskCreationIsExactlyOneAllocationPerFrame() {
     CHECK_ALLOCATIONS(2, CHECK(co2::syncWait(awaitsLeaf(2)) == 2));
 }
 
+void syncWaitOfAsResultTaskAddsNoFrame() {
+    // 只有 Task 自己的帧。syncWait(awaitable) 的通用重载会为非 Task 的 awaitable 再包
+    // 一层协程；asResult(Task) 有专门的重载，直接读 Task 的结果。
+    CHECK_ALLOCATIONS(1, CHECK(co2::syncWait(co2::asResult(leaf(2)))->value() == 2));
+}
+
 auto hops(co2::Scheduler& scheduler, int count)
     CO2_BEG(co2::Task<int>, (scheduler, count), int done{};) {
     while (done < count) {
@@ -393,6 +400,7 @@ int main() {
     }
     generatorIterationDoesNotAllocate();
     taskCreationIsExactlyOneAllocationPerFrame();
+    syncWaitOfAsResultTaskAddsNoFrame();
     manualExecutorHopsDoNotAllocateInSteadyState();
     threadPoolHopsDoNotAllocateInSteadyState();
     stopTokenOperationsAllocateOnlyTheState();
diff --git a/tests/completion_tests.cpp b/tests/completion_tests.cpp
index fedebdf..5550b3c 100644
--- a/tests/completion_tests.cpp
+++ b/tests/completion_tests.cpp
@@ -10,15 +10,18 @@
 #include <stdexcept>
 #include <string>
 #include <system_error>
+#include <type_traits>
 #include <utility>
 #include <vector>
 
+#include "co2/as_result.hpp"
 #include "co2/completion.hpp"
 #include "co2/coroutine.hpp"
 #include "co2/error.hpp"
 #include "co2/just.hpp"
 #include "co2/manual_executor.hpp"
 #include "co2/optional.hpp"
+#include "co2/result.hpp"
 #include "co2/spawn.hpp"
 #include "co2/sync_wait.hpp"
 #include "co2/task.hpp"
@@ -644,6 +647,112 @@ void justErrorRaisesForACallerThatSkipsAwaitCompletion() {
     CHECK(sawCode);
 }
 
+// ---------------------------------------------------------------------------
+// asResult：三种结果都变成值，当前协程照常往下执行
+
+// 返回 0：值；1：错误码；2：已停止；3：捕获的异常。
+auto classifies(Leaf leaf, int& resumed)
+    CO2_BEG(co2::Task<int>, (leaf, resumed), co2::optional<co2::Result<int>> result;) {
+    CO2_AWAIT_SET(result, co2::asResult(chain(1, leaf, resumed, nullptr)));
+    if (result->isStopped()) CO2_RETURN(2);
+    if (result->hasValue()) CO2_RETURN(result->value() == 2 ? 0 : -1);
+    if (result->error().exception() != nullptr) CO2_RETURN(3);
+    CO2_RETURN(result->error().code() == sample::Errc::gone ? 1 : -1);
+}
+CO2_END
+
+void asResultTurnsEveryCompletionIntoAValue() {
+    auto resumed = 0;
+    CHECK(co2::syncWait(classifies(Leaf::returns, resumed)) == 0);
+    CHECK(co2::syncWait(classifies(Leaf::fails, resumed)) == 1);
+    CHECK(co2::syncWait(classifies(Leaf::stops, resumed)) == 2);
+    CHECK(co2::syncWait(classifies(Leaf::throws, resumed)) == 3);
+}
+
+// 标准写法的 awaiter：没有 await_completion()。
+struct Seven {
+    bool await_ready() const noexcept { return true; }
+    void await_suspend(co2::coroutine_handle<>) const noexcept {}
+    int await_resume() const noexcept { return 7; }
+};
+
+struct Throws {
+    bool await_ready() const noexcept { return true; }
+    void await_suspend(co2::coroutine_handle<>) const noexcept {}
+    int await_resume() const { throw ExpectedError{}; }
+};
+
+auto wrapsPlainAwaiters(int& sum, bool& caught, bool& voidHasValue)
+    CO2_BEG(co2::Task<>, (sum, caught, voidHasValue),
+            co2::optional<co2::Result<int>> number;
+            co2::optional<co2::Result<void>> nothing; Seven lvalue;) {
+    CO2_AWAIT_SET(number, co2::asResult(Seven{})); // 没有 await_completion()：一定有值
+    sum += number->value();
+    CO2_AWAIT_SET(number, co2::asResult(lvalue)); // 左值：只记引用
+    sum += number->value();
+    CO2_AWAIT_SET(number, co2::asResult(Throws{})); // await_resume() 的异常被接成错误
+    caught = number->hasError() && number->error().exception() != nullptr;
+    CO2_AWAIT_SET(nothing, co2::asResult(co2::suspend_never{})); // 结果是 void
+    voidHasValue = nothing->hasValue();
+}
+CO2_END
+
+void asResultWrapsAnyAwaitable() {
+    auto sum = 0;
+    auto caught = false;
+    auto voidHasValue = false;
+    CHECK(co2::syncWait(wrapsPlainAwaiters(sum, caught, voidHasValue)).has_value());
+    CHECK(sum == 14);
+    CHECK(caught);
+    CHECK(voidHasValue);
+}
+
+auto voidLeaf(Leaf leaf) CO2_BEG(co2::Task<>, (leaf)) {
+    if (leaf == Leaf::stops) CO2_AWAIT(Ends{stopped()});
+}
+CO2_END
+
+// 根上按值拿到三种结果，一个也不抛：禁用异常的工程在根上就是这样写。
+void syncWaitOfAsResultHandsTheOutcomeToOrdinaryCode() {
+    auto resumed = 0;
+    static_assert(
+        std::is_same<decltype(co2::syncWait(
+                         co2::asResult(chain(0, Leaf::returns, resumed, nullptr)))),
+                     co2::optional<co2::Result<int>>>::value,
+        "syncWait(asResult(Task<int>)) must return optional<Result<int>>");
+    static_assert(
+        std::is_same<decltype(co2::syncWait(co2::asResult(voidLeaf(Leaf::returns)))),
+                     co2::optional<co2::Result<void>>>::value,
+        "syncWait(asResult(Task<void>)) must return optional<Result<void>>");
+
+    auto const value =
+        co2::syncWait(co2::asResult(chain(1, Leaf::returns, resumed, nullptr)));
+    CHECK(value.has_value() && value->hasValue() && value->value() == 2);
+
+    auto const failure =
+        co2::syncWait(co2::asResult(chain(1, Leaf::fails, resumed, nullptr)));
+    CHECK(failure.has_value() && failure->hasError());
+    CHECK(failure->error().code() == sample::Errc::gone);
+
+    // asResult 自己不会以已停止完成：optional 总是有值，"已停止"在 Result 里。
+    auto const stop =
+        co2::syncWait(co2::asResult(chain(1, Leaf::stops, resumed, nullptr)));
+    CHECK(stop.has_value() && stop->isStopped());
+
+    auto const thrown =
+        co2::syncWait(co2::asResult(chain(1, Leaf::throws, resumed, nullptr)));
+    CHECK(thrown.has_value() && thrown->hasError());
+    CHECK(thrown->error().exception() != nullptr);
+    CHECK(thrown->error().code() == co2::Errc::unhandled_exception);
+
+    CHECK(co2::syncWait(co2::asResult(voidLeaf(Leaf::returns)))->hasValue());
+    CHECK(co2::syncWait(co2::asResult(voidLeaf(Leaf::stops)))->isStopped());
+
+    // 不是 Task 的 awaitable 走通用重载，结果同形。
+    auto const plain = co2::syncWait(co2::asResult(Seven{}));
+    CHECK(plain.has_value() && plain->value() == 7);
+}
+
 } // namespace
 
 int main() {
@@ -669,5 +778,8 @@ int main() {
     justErrorAndWithErrorEndTheTaskWithThatError();
     justErrorWorksInAnyPromiseThatHasTheEntry();
     justErrorRaisesForACallerThatSkipsAwaitCompletion();
+    asResultTurnsEveryCompletionIntoAValue();
+    asResultWrapsAnyAwaitable();
+    syncWaitOfAsResultHandsTheOutcomeToOrdinaryCode();
     return 0;
 }
````

- [ ] **Step 2: 构建，确认失败**

Expected: 构建失败，两个测试文件都报 `fatal error`，说找不到 `co2/as_result.hpp`。

- [ ] **Step 3: 实现 `asResult`**

新建 `include/co2/as_result.hpp`：

````cpp
#pragma once

#include <exception>
#include <type_traits>
#include <utility>

#include "co2/completion.hpp"
#include "co2/config.hpp"
#include "co2/coroutine_handle.hpp"
#include "co2/detail/awaitable.hpp"
#include "co2/error.hpp"
#include "co2/result.hpp"

// co2::asResult(awaitable)：把一次等待的三种结果都变成值。
//
//   CO2_AWAIT_SET(r, co2::asResult(readHeader(socket)));   // r 是 co2::Result<Header>
//
// 普通的 CO2_AWAIT 遇到错误或已停止会就此结束当前协程；套上 asResult 之后当前协程照常
// 往下执行，自己检查 r.hasValue() / r.hasError() / r.isStopped()。被包住的操作抛出的
// 异常也被接成错误。Result<T> 不能默认构造，而帧局部必须可以：用
// co2::optional<co2::Result<T>> 作为 CO2_AWAIT_SET 的目标。
//
// asResult(x) 要直接写在等待它的地方：它交出的 awaiter 取自一个右值。x 是右值时被搬进
// 等待者的帧；x 是左值时只记它的引用，x 必须活过这次等待。

namespace co2 {
namespace detail {

template <class Awaited> struct AsResultType {
    using type = Result<typename std::decay<Awaited>::type>;
};
template <> struct AsResultType<void> { using type = Result<void>; };

// 内层 awaiter 有 await_completion() 就问它——每次等待恰好一次——没有就一定有值。
template <class Awaiter>
Completion completionOf(Awaiter& awaiter, std::true_type) noexcept {
    return awaiter.await_completion();
}

template <class Awaiter> Completion completionOf(Awaiter&, std::false_type) noexcept {
    return Completion::value();
}

// 自己没有 await_completion()，所以从不向上传播。
template <class Inner> struct AsResultAwaiter {
    using Awaited = decltype(std::declval<Inner&>().await_resume());
    using ResultType = typename AsResultType<Awaited>::type;

    Inner inner;

    auto await_ready() -> decltype(std::declval<Inner&>().await_ready()) {
        return inner.await_ready();
    }

    // 带类型的句柄原样交给内层：它可能要读等待者的环境。
    template <class Promise>
    auto await_suspend(coroutine_handle<Promise> const awaiting)
        -> decltype(std::declval<Inner&>().await_suspend(awaiting)) {
        return inner.await_suspend(awaiting);
    }

    ResultType await_resume() {
        auto completion =
            completionOf(inner, typename HasAwaitCompletion<Inner>::type{});
        if (completion.isStopped()) return ResultType::stopped();
        if (completion.hasError()) return ResultType{std::move(completion).error()};
        try {
            return resumeInner(std::is_void<Awaited>{});
        } catch (...) {
            return ResultType{Error{std::current_exception()}};
        }
    }

  private:
    ResultType resumeInner(std::false_type) { return ResultType{inner.await_resume()}; }

    ResultType resumeInner(std::true_type) {
        inner.await_resume();
        return ResultType{};
    }
};

} // namespace detail

// asResult(x) 的返回类型。Awaitable 对右值是值类型（拥有 x），对左值是引用类型。
template <class Awaitable> struct AsResult {
    Awaitable awaitable;

    detail::AsResultAwaiter<detail::AwaiterOf<Awaitable>> operator_co_await() && {
        return detail::AsResultAwaiter<detail::AwaiterOf<Awaitable>>{
            detail::AwaiterOf<Awaitable>{
                detail::getAwaiter(std::forward<Awaitable>(awaitable))}};
    }
};

template <class Awaitable> AsResult<Awaitable> asResult(Awaitable&& awaitable) {
    return AsResult<Awaitable>{std::forward<Awaitable>(awaitable)};
}

} // namespace co2
````

- [ ] **Step 4: `syncWait(asResult(task))` 的重载**

修改 `include/co2/task.hpp`、`include/co2/sync_wait.hpp`、`include/co2/co2.hpp`：

````diff
diff --git a/include/co2/co2.hpp b/include/co2/co2.hpp
index 2b90da0..51cc4e3 100644
--- a/include/co2/co2.hpp
+++ b/include/co2/co2.hpp
@@ -1,6 +1,7 @@
 #pragma once
 
 // co2 的全部公共头。按需包含单个头文件同样可行，每个头都能独立包含。
+#include "co2/as_result.hpp"
 #include "co2/async_generator.hpp"
 #include "co2/callback.hpp"
 #include "co2/completion.hpp"
diff --git a/include/co2/sync_wait.hpp b/include/co2/sync_wait.hpp
index d2e6014..964c30d 100644
--- a/include/co2/sync_wait.hpp
+++ b/include/co2/sync_wait.hpp
@@ -5,6 +5,7 @@
 #include <type_traits>
 #include <utility>
 
+#include "co2/as_result.hpp"
 #include "co2/contract.hpp"
 #include "co2/coroutine.hpp"
 #include "co2/coroutine_handle.hpp"
@@ -75,6 +76,21 @@ detail::RootResult<T> syncWait(Task<T> task, stop_token token = {}) {
     return detail::TaskAccess::takeRootResult(task);
 }
 
+// asResult(Task) 的同步等待：三种结果都作为值交给调用方，不抛异常。它直接读 Task 的
+// 结果，不像下面的通用重载那样再包一层协程——禁用异常的工程里这是根上的主路径，不能
+// 每次多分配一个帧。asResult 自己不会以已停止完成，所以返回的 optional 总是有值的；
+// 保留 optional 是为了与其他根同形。
+template <class T>
+optional<Result<T>> syncWait(AsResult<Task<T>> wrapped, stop_token token = {}) {
+    auto& task = wrapped.awaitable;
+    CO2_CONTRACT_CHECK(task);
+    detail::SyncWaitState state;
+    detail::SyncWaitCompletion completion{state};
+    detail::TaskAccess::start(task, completion.handle(), std::move(token));
+    state.wait();
+    return optional<Result<T>>{detail::TaskAccess::takeOutcome(task)};
+}
+
 namespace detail {
 
 template <class T> struct IsTask : std::false_type {};
diff --git a/include/co2/task.hpp b/include/co2/task.hpp
index b1ad2ae..55feece 100644
--- a/include/co2/task.hpp
+++ b/include/co2/task.hpp
@@ -337,6 +337,16 @@ struct TaskAccess {
         return takeRootResult(task, std::is_void<T>{});
     }
 
+    // 把结果整个取成值（只可一次）：三种结果都不抛。asResult(Task) 的根用它。
+    template <class T> static Result<T> takeOutcome(Task<T>& task) {
+        CO2_CONTRACT_CHECK(task.handle && task.handle.done());
+        auto& promise = task.handle.promise();
+        if (promise.state == TaskState::stopped) return Result<T>::stopped();
+        if (promise.state == TaskState::error)
+            return Result<T>{std::move(promise.storage.error)};
+        return valueAsResult(task, std::is_void<T>{});
+    }
+
     template <class T> static bool hasError(Task<T> const& task) noexcept {
         return task.handle && task.handle.promise().state == TaskState::error;
     }
@@ -363,6 +373,20 @@ struct TaskAccess {
         takeResult(task);
         return RootResult<void>{Unit{}};
     }
+
+    // 取值时 T 的移动构造抛出：与 asResult 的其他路径一样接成错误。
+    template <class T> static Result<T> valueAsResult(Task<T>& task, std::false_type) {
+        try {
+            return Result<T>{takeResult(task)};
+        } catch (...) {
+            return Result<T>{Error{std::current_exception()}};
+        }
+    }
+
+    static Result<void> valueAsResult(Task<void>& task, std::true_type) {
+        takeResult(task);
+        return Result<void>{};
+    }
 };
 
 } // namespace detail
````

- [ ] **Step 5: 构建并运行全部测试，确认通过**

Expected: `100% tests passed, 0 tests failed out of 28`。

`co2-allocation-tests` 里的 `syncWaitOfAsResultTaskAddsNoFrame` 检查的就是 Step 4 的重载：没有它时，这个测试报 `allocation mismatch ...: expected 1, got 2`（通用重载多包了一层协程）。

- [ ] **Step 6: 提交**

```bash
git add include/co2/as_result.hpp include/co2/task.hpp include/co2/sync_wait.hpp \
        include/co2/co2.hpp tests/completion_tests.cpp tests/allocation_tests.cpp
git commit -F - <<'EOF'
新增 asResult，syncWait(asResult(task)) 不多分配帧

docs/design/completion-channels.md 第 2.3、2.4 节。

    CO2_AWAIT_SET(r, co2::asResult(readHeader(socket)));   // co2::Result<Header>

套上 asResult 之后三种结果都是值，当前协程照常往下执行；被包住的操作抛出的异常
也被接成错误。

syncWait(asResult(task)) 把三种结果都交给普通代码而不抛异常——禁用异常的工程在
根上就是这样写。它有专门的重载，直接读 Task 的结果：通用重载会为非 Task 的
awaitable 再包一层协程，多分配一个帧。
EOF
```

### Task 8: 文档与全配置验证

**Files:**
- Modify: `README.md`、`docs/architecture.md`、`docs/design/coroutine-core-v2.md`、`docs/design/completion-channels.md`

**Interfaces:**
- Consumes: 前七个任务的全部结果。
- Produces: 无代码。

- [ ] **Step 1: 更新文档**

修改 `README.md`：

````diff
diff --git a/README.md b/README.md
index 68c534a..8200abf 100644
--- a/README.md
+++ b/README.md
@@ -87,7 +87,8 @@ int main() { return co2::syncWait(twice(21)) == 42 ? 0 : 1; }
 ### `Task<T>`
 
 惰性、单消费者、只可移动。等待子 Task 是对称转移（栈深不随链长增长）；子 Task 完成时
-对称转移回等待者；异常在等待者处重抛。
+对称转移回等待者。子 Task 出错时，等待它的协程以同一个错误结束，逐层向上，到根上才
+变回异常——见"错误：按值向上传"。
 
 ```cpp
 #include <co2/task.hpp>
@@ -159,6 +160,62 @@ CO2_END
 每一步都是生产者与消费者之间的对称转移，没有同步；消费者在生产者 yield 所在的线程
 上恢复。有 `next()` 未完成时销毁是契约违规。
 
+## 错误：按值向上传
+
+一个 Task 以值、错误或已停止结束。错误是一个 `co2::Error`：要么是一个
+`std::error_code`，要么是一个被捕获的异常。
+
+```cpp
+#include <co2/as_result.hpp>
+#include <co2/just.hpp>
+
+auto readHeader(Socket& socket) CO2_BEG(co2::Task<Header>, (socket), Header header;) {
+    if (not socket.isOpen())
+        CO2_AWAIT(co2::just_error(NetErrc::closed));   // 本协程以这个错误结束
+    CO2_AWAIT_SET(header, socket.read());
+    CO2_RETURN(header);
+}
+CO2_END
+
+auto serve(Socket& socket)
+    CO2_BEG(co2::Task<>, (socket), Header header;
+            co2::optional<co2::Result<Header>> attempt;) {
+    CO2_AWAIT_SET(header, readHeader(socket));   // 出错：到此为止，以同一个错误结束
+    CO2_AWAIT_SET(attempt, co2::asResult(readHeader(socket)));   // 想自己处理
+    if (attempt->hasError()) log(attempt->error().code());
+}
+CO2_END
+```
+
+- **普通的 `CO2_AWAIT` 向上传播。** 被等待的操作出错时，其后的语句不执行，局部按逆序
+  析构，当前协程以同一个错误结束并交给等待它的协程。中间各层既不抛也不接——挂起点
+  本来就不能放进 `try` 块。
+- **协程体里 `throw` 等同于以错误结束。** 异常只被捕获一次，此后按值传。
+- **到根上才变回异常。** `syncWait` / `join()` 重抛原来的异常；错误码则抛
+  `std::system_error`。不想要异常就写 `co2::syncWait(co2::asResult(task))`，得到
+  `co2::optional<co2::Result<T>>`，三种结果都是值。
+- **想自己处理就套 `asResult`。** 得到 `co2::Result<T>`：`hasValue()` / `hasError()` /
+  `isStopped()`。它不能默认构造，作帧局部时写成 `co2::optional<co2::Result<T>>`。
+- **自己的错误枚举**按 `<system_error>` 的常规方式接入（`std::is_error_code_enum` 的
+  特化加 `make_error_code`），之后可以直接传给 `just_error`。
+  `CO2_YIELD(co2::with_error(e))` 是 P3552 的拼写，效果相同。
+
+写 awaiter 与 promise 的人各多一个可选的扩展点：
+
+- awaiter 可以提供 `co2::Completion await_completion() noexcept`，报告这次等待是有值、
+  出错还是已停止。恢复点先问它，有值才调用 `await_resume()`。没有这个成员的 awaiter
+  行为不变。**包装别的 awaiter 的 awaiter 要把它一并转发**：不转发时，被包住的操作
+  出错仍然从 `await_resume()` 抛出（与从前相同），以已停止完成则是契约违规。
+- promise 可以提供 `void return_error(co2::Error) noexcept` 与
+  `void return_stopped() noexcept` 接收后两种结果。没有 `return_error()` 时错误在
+  协程体内重新抛出，走 `unhandled_exception()`；没有 `return_stopped()` 时"已停止"是
+  契约违规。
+
+"已停止"目前只有自己写的 awaiter 能产生；`whenAll`、`JoinHandle`、`AsyncGenerator`、
+`fromCallback` 也还没有接上 `await_completion()`——它们遇到错误仍然从
+`await_resume()` 抛出，结果相同，只是多一次抛接。完整的设计与落地顺序见
+`docs/design/completion-channels.md`。
+
 ## 执行层
 
 ### `Scheduler` 与 `scheduleOn`
@@ -200,9 +257,9 @@ Task 在调用线程上内联启动，完成时对称转移到一个不占堆的
 
 返回值也照 `sync_wait`：结果放在 `co2::optional` 里，`Task<void>` 是
 `co2::optional<co2::Unit>`。取值写 `*result`；与值比较可以直接写
-`co2::syncWait(task) == 42`。空的 optional 留给"已停止"这种结果——目前还没有任何操作
-会以已停止完成，所以它总是有值。结果是 `bool` 的根不要直接当条件用：
-`if (co2::syncWait(task))` 判断的是"有没有值"，不是值本身。
+`co2::syncWait(task) == 42`。空的 optional 表示 Task 以已停止结束；Task 出错时在这里
+抛出。结果是 `bool` 的根不要直接当条件用：`if (co2::syncWait(task))` 判断的是"有没有
+值"，不是值本身。
 
 ### `spawn` / `JoinHandle<T>`
 
@@ -295,6 +352,8 @@ co2::setContractViolationHandler([](co2::ContractViolation const& v) {
 | `co2/coroutine_handle.hpp` | `coroutine_handle<P>`、`suspend_always/never`、`noop_coroutine`、`coroutine_traits` |
 | `co2/dsl.hpp` | `CO2_*` 宏 |
 | `co2/task.hpp` / `generator.hpp` / `async_generator.hpp` | 返回类型 |
+| `co2/error.hpp` / `completion.hpp` / `result.hpp` / `optional.hpp` | `Error`、`Completion`、`Result<T>` 与 `Unit`、`optional<T>` |
+| `co2/just.hpp` / `as_result.hpp` | `just_error` / `with_error`、`asResult` |
 | `co2/scheduler.hpp` / `manual_executor.hpp` / `thread_pool.hpp` | 执行层 |
 | `co2/sync_wait.hpp` / `spawn.hpp` | 根适配器 |
 | `co2/when_all.hpp` / `callback.hpp` | 组合器 `whenAll`、回调适配 `fromCallback` |
@@ -321,9 +380,9 @@ i7-13700KF，每项 7 轮取中位数：
 | raw suspend/resume | 1.0 |
 | Generator yield/next | 1.5 |
 | elements_of 深度 8 yield/next | 1.9 |
-| symmetric Task transfer（含帧分配） | 11.9 |
-| syncWait ready Task | 25.4 |
-| Task 链上的失败，100 层，每层 | 558 |
+| symmetric Task transfer（含帧分配） | 12.0 |
+| syncWait ready Task | 26.7 |
+| Task 链上的失败，100 层，每层 | 45.3 |
 | Task 等待子 Task，8 线程各自的 stop_token | 19.3 |
 | Task 等待子 Task，8 线程共享一个 stop_token | 18.2 |
 | ManualExecutor hop | 20.1 |
@@ -332,6 +391,8 @@ i7-13700KF，每项 7 轮取中位数：
 | ThreadPool hop（8 workers，1 Task，不饱和） | 14.5 |
 | spawn + join（跨线程唤醒） | 2542 |
 
+"Task 链上的失败"在错误改为按值传播之前是 558：那时每一层都要重抛一次异常。
+
 线程池的几项对机器负载敏感：同一台机器上有其他负载时，不饱和那一项在 17～30 ns 之间
 浮动。数字只在同一台机器、同一份编译选项下可比。
 
````

修改 `docs/architecture.md`、`docs/design/coroutine-core-v2.md`、`docs/design/completion-channels.md`：

````diff
diff --git a/docs/architecture.md b/docs/architecture.md
index ad7fcf9..26acd84 100644
--- a/docs/architecture.md
+++ b/docs/architecture.md
@@ -88,6 +88,13 @@ Allocator（只有带状态的 allocator 才在帧里留副本）
 final_suspend()`。异常离开协程体走标准的 `unhandled_exception` 路径；初始挂起点之前的
 异常销毁帧并抛给调用方。
 
+恢复点比标准多一步（`docs/design/completion-channels.md`）：awaiter 有可选成员
+`await_completion()` 时，先问它这次等待是有值、出错还是已停止。有值照常
+`await_resume()`；否则协程就此结束——销毁 awaiter、析构局部、把结果交给 promise 的
+`return_error()` / `return_stopped()`、`co_await final_suspend()`，顺序与异常逃出
+协程体时相同。promise 没有 `return_error()` 时错误在体内重新抛出，所以为标准写的
+promise 不受影响。
+
 `await_suspend` 收到的是**带类型的** `coroutine_handle<Promise>`，因此 awaiter 可以像
 标准一样以模板 `await_suspend` 查询等待者的 promise（co2 用它传播环境）。
 
@@ -116,8 +123,11 @@ final_suspend()`。异常离开协程体走标准的 `unhandled_exception` 路
   `await_suspend` 里写入 `continuation` 后对称转移进 child；`FinalAwaiter` 转移回等待者。
   惰性 + 先写续体再转移，让"完成 vs 挂起"没有竞争窗口，Task 不需要原子量。右值
   Task 自身是 awaiter（拥有 child 帧——宏展开里的临时对象活不过挂起点，见设计稿
-  D10），左值经 `operator_co_await() &` 借用。`T`/`void` 只有一份 promise
-  （`ResultStorage<T>` + SFINAE 二选一的 `return_value`/`return_void`）。
+  D10），左值经 `operator_co_await() &` 借用。`T`/`void` 只有一份 promise（SFINAE
+  二选一的 `return_value`/`return_void`）。结果是四态——空、值、错误、已停止——值与
+  错误共用一块存储（`TaskStorage<T>`），状态标记与 `started` 相邻，promise 与只存
+  `exception_ptr` 时一样大。错误与已停止按值沿等待链向上传，根上才变回异常或空的
+  optional；体内的异常在 `unhandled_exception()` 里被捕获一次。
 - **`Generator<Ref, V>`**：`std::generator` 同形。所有嵌套层共享根 promise 上的
   `current`（当前元素指针）与 `active`（最内层活动协程）；消费者的 `++` 直接恢复
   `active`，内层 `final_suspend` 把 `active` 改回父层并对称转移过去。嵌套生成器由父层
diff --git a/docs/design/completion-channels.md b/docs/design/completion-channels.md
index a833fbb..c127a1d 100644
--- a/docs/design/completion-channels.md
+++ b/docs/design/completion-channels.md
@@ -1,7 +1,9 @@
 # 三种完成方式：值、错误、已停止——以及禁用异常的构建
 
-状态：设计已确认（2026-11-12），尚未实现。同日对照库的代码评审过一轮并修订，哪些论断
-验证过、哪些没有，见第 12 节。本文只描述要做成什么样；实施计划另写。
+状态：设计已确认（2026-11-12）。第 10 节的第 1～5 步已经实现（词汇类型、根返回
+optional、恢复点与 promise 的两个入口、`Task` 四态、`just_error` / `with_error` /
+`asResult`）；第 6～10 步尚未实现。设计确认当天对照库的代码评审过一轮并修订，哪些
+论断验证过、哪些没有，见第 12 节。
 
 ## 0. 问题
 
diff --git a/docs/design/coroutine-core-v2.md b/docs/design/coroutine-core-v2.md
index 21a0445..d1dbc89 100644
--- a/docs/design/coroutine-core-v2.md
+++ b/docs/design/coroutine-core-v2.md
@@ -528,6 +528,13 @@ harden、trampoline 账本、每帧原子门闩、每次异步等待 1～2 次
   类内声明、类外定义。在没有 `this` 的函数里用 `CO2_BEG_MEMBER` 是编译错误。
 - **D14 宏实参的个数**：捕获列表最多 8 个参数；`CO2_BEG` 的局部列表与 `CO2_RETURN` 的
   表达式里每个顶层逗号都多算一个宏实参，总数上限 64（`detail/preprocessor.hpp`）。
+- **D15 `await_completion()` 与 promise 的两个入口**：这一条不是"没有编译器"逼出来的，
+  而是有意的扩展（`completion-channels.md`）。awaiter 可以多一个成员
+  `co2::Completion await_completion() noexcept`；恢复点在 `await_resume()` 之前调用它，
+  出错或已停止时不调用 `await_resume()`，而是结束协程并把结果交给 promise 的
+  `return_error()` / `return_stopped()`。没有这个成员的 awaiter、没有这两个入口的
+  promise 走的仍是标准路径，差分测试不受影响；原生 `co_await` 不认识这个成员，所以
+  库内 awaiter 的 `await_resume()` 在出错时照旧抛出。
 
 除此之外的任何差异都应视为缺陷。
 
````

- [ ] **Step 2: 核对 README 基准表里的数字**

```bash
cmake --build /tmp/co2-build/bench -j 12
/tmp/co2-build/bench/benchmarks/co2-benchmarks Task
```

Expected（这台机器，GCC 13.3 `-O2`）：`symmetric Task transfer` 12.0，`syncWait ready Task` 26.7，`Task failure through a 100-deep chain, per layer` 45.3。量到的中位数与表里相差超过 5% 的那一行，改成量到的数字。

- [ ] **Step 3: 全配置验证**

把下面的脚本存为 `/tmp/co2-build/check.sh` 并在仓库根目录执行（`runlimit` 见"环境与命令"，先编译好放在 `/tmp/co2-build/`）：

```bash
#!/bin/bash
# 用法：在仓库根目录执行 /tmp/co2-build/check.sh。每个配置输出一行结论。
src=$(pwd)
for cfg in gcc14 clang14 gcc17 gcc20 gcc23 clang20 clang23 libcxx gccrel clangrel asan gccasan tsan; do
  type=Debug; extra=""; wrap=""
  case $cfg in
    gcc14)    cxx=g++;     std=14;;
    clang14)  cxx=clang++; std=14;;
    gcc17)    cxx=g++;     std=17;;
    gcc20)    cxx=g++;     std=20;;
    gcc23)    cxx=g++;     std=23;;
    clang20)  cxx=clang++; std=20;;
    clang23)  cxx=clang++; std=23;;
    libcxx)   cxx=clang++; std=20; extra="-stdlib=libc++";;
    gccrel)   cxx=g++;     std=14; type=Release;;
    clangrel) cxx=clang++; std=14; type=Release;;
    asan)     cxx=clang++; std=14; extra="-fsanitize=address,undefined -fno-omit-frame-pointer";;
    gccasan)  cxx=g++;     std=14; extra="-fsanitize=address,undefined -fno-omit-frame-pointer";;
    tsan)     cxx=clang++; std=14; extra="-fsanitize=thread"; wrap="setarch $(uname -m) -R";;
  esac
  b=/tmp/co2-build/matrix-$cfg
  cmake -S "$src" -B $b -DCMAKE_BUILD_TYPE=$type -DCMAKE_CXX_COMPILER=$cxx \
        -DCO2_TEST_CXX_STANDARD=$std -DCMAKE_CXX_FLAGS="$extra" > $b.configure.log 2>&1 \
    || { echo "$cfg: CONFIGURE FAILED"; continue; }
  cmake --build $b -j 12 > $b.build.log 2>&1 || { echo "$cfg: BUILD FAILED (see $b.build.log)"; continue; }
  (cd $b && ASAN_OPTIONS=detect_leaks=0 /tmp/co2-build/runlimit 900 $wrap ctest -j 8 > $b.test.log 2>&1)
  echo "$cfg: $(grep -E 'tests passed|tests failed' $b.test.log)"
done
```

Expected: 13 行，每行都是 `100% tests passed, 0 tests failed out of 28`。

- [ ] **Step 4: 提交**

```bash
git add README.md docs/architecture.md docs/design/coroutine-core-v2.md \
        docs/design/completion-channels.md
git commit -F - <<'EOF'
文档：错误按值传播

README 新增"错误：按值向上传"一节与两行头文件说明；架构文档补上恢复点多出的
一步与 Task 的四态存储；核心设计稿的偏离清单新增 D15；三种完成方式的设计稿标明
第 10 节的第 1～5 步已实现。

基准表按现在的数字更新：失败沿 100 层的链每层 558 → 45.3 ns；syncWait ready
Task 25.4 → 26.7 ns，这条路径上根的返回值包了一层 optional、Task 的结果改成了
四态。
EOF
```
