# 三种完成方式 实施计划（二）：各组件、已停止与取消点

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 让 `JoinHandle`、`whenAll`、`AsyncGenerator`、`fromCallback` 遵守三种完成方式，提供"已停止"的公开产生方，并让 `scheduleOn` 成为取消点。

**Architecture:** 计划（一）已经有了机制：恢复点询问 awaiter 的 `await_completion()`，`Task` 把结果存成四态。这一批给四个组件的 awaiter 各加上 `await_completion()`，让被等待的一方出错或已停止时，等待它的协程以同样的结果结束。已停止的产生方是几个小 awaitable：`just_stopped()`、`stopIfRequested()`，以及在 hop 之后检查停止令牌的 `scheduleOn`；`unstoppable(x)` 靠把类型擦除的句柄交给内层来隐藏停止请求。

**Tech Stack:** C++14，仅头文件；CMake 与 CTest；没有第三方依赖。

设计稿：`docs/design/completion-channels.md`。本计划覆盖它第 10 节的第 6～8 步。前一份计划：`docs/design/completion-channels-plan-1.md`。

**状态：** 已执行完毕（2026-11-12），10 个任务对应提交 `9057cf4`、`a552083`、`7b0067a`、`56fa850`、`f8f5d4e`、`5c2d209`、`84956d2`、`72c7805`、`3a5a28f`、`adcdcd2`。每个任务都按这里的步骤做了；任务 9 的重复运行与任务 10 的 13 个配置全部通过。代码与本文逐字相同，只有任务 10 的文档有一处与本文不同：

- 本文说 `scheduleOn` 成为取消点之后"线程池的几项变化在噪声以内"。那是写计划时机器上有别的负载造成的误判。执行任务 10 时机器安静，取消点前后交替各跑 6 轮，差别是稳定的：每次 hop 多 0.1～1.2 ns。提交的 README 因此与下文任务 10 的 diff 不同——线程池三行的数字从 13.1、3.5、14.5 改成了 13.4、3.6、15.5，说明文字也相应改了；设计稿 3.5 节"未测"的那一条换成了实测数字。

## Global Constraints

- 库与测试的最低语言标准是 C++14，不使用 C++17 及以后的特性。
- 仅头文件，不引入第三方依赖。
- 测试以 `-Wall -Wextra -Wpedantic -Werror -Wconversion -Wsign-conversion -Wold-style-cast -Wzero-as-null-pointer-constant` 等选项编译（见 `tests/CMakeLists.txt`），新代码不得有告警。
- 每个公共头文件都必须能单独包含（`co2-header-self-containment-tests` 会检查）。
- 协议上的名字照设计稿：awaiter 的可选成员 `co2::Completion await_completion() noexcept`；promise 的可选入口 `void return_stopped() noexcept` 与 `void return_error(co2::Error) noexcept`。
- 库内 awaiter 的 `await_resume()` 保持自足：出错时抛出（`Error::raise()`），已停止是契约违规。`scheduleOn` 与 `stopIfRequested()` 例外：它们的 `await_resume()` 照常返回，不认识 `await_completion()` 的调用方只是不被取消。
- `tests/allocation_tests.cpp` 钉死的帧大小不得改变（`Task<int>(int)` 是 176 字节）；`CallbackAwaitable<int>` / `<void>` 必须留在内联 awaiter 槽里。
- 前置条件用 `CO2_CONTRACT_CHECK` / `CO2_CONTRACT_FAIL` 报告，不为它们抛异常。
- 每个任务一个本地提交，不推送。提交信息用中文，正文说明原因与验证。`git add` 写明路径，不要用 `git add -A`：仓库里有不属于本工作的未跟踪目录 `build/`。
- 构建目录放在仓库之外（`/tmp/co2-build/...`），不要动仓库里的 `build/`。

## 范围

设计稿第 10 节的第 6、7、8 步，外加写这份计划时发现的一个缺陷（任务 1）：

- 任务 1：修正计划（一）留下的缺陷——同一个失败的 `Task` 被等待两次会崩溃。
- 任务 2：把测试支撑代码搬进共用头文件，为后面的任务做准备。
- 任务 3～6（第 6 步）：等待 `JoinHandle`、`whenAll`、`AsyncGenerator`、`fromCallback` 各一个提交。
- 任务 7、8（第 7 步）：`just_stopped` / `stopIfRequested`，`unstoppable`。
- 任务 9（第 8 步）：`scheduleOn` 成为取消点。
- 任务 10：文档、基准的"已停止"一行、全配置验证。

做完之后还剩设计稿的第 9、10 步——禁用异常的构建（`CO2_HAS_EXCEPTIONS`、CMake 选项、CI）与文档收尾——另写计划（三）。

本计划里的全部代码都在仓库的一份副本上按任务顺序实现并跑通过：有新行为的任务（1、3～9）都先确认只加测试时失败、再确认加上实现后通过；最终状态在 13 个配置下通过全部 30 个测试（GCC 13 与 Clang 18 的 Debug / Release、C++14 / 17 / 20 / 23、libc++、ASan + UBSan、TSan），线程相关的 6 个测试另在 Release 下重复 40 遍、TSan 下重复 5 遍。MSVC 没有验证。

实现时有三处与设计稿的字面不同，任务 10 会把它们同步进设计稿：

- `scheduleOn` 与 `stopIfRequested()` 的 `await_resume()` 照常返回，而不是契约违规（见上面的约束）。
- `CallbackAwaitable<int>` 从 56 字节变成 64 字节。它仍在内联槽里，帧大小与分配次数不变；设计稿原来说的是"钉死的大小不变"。原因见任务 6。
- `Task` 的错误只可取走一次（任务 1）。

## 文件一览

| 文件 | 任务 | 职责 |
| --- | --- | --- |
| `include/co2/task.hpp` | 1、3、4 | 错误只可取走一次；`TaskAccess::completionOf`、`stateOf` |
| `include/co2/spawn.hpp` | 3 | `JoinHandle` 的 awaiter 提供 `await_completion()` |
| `include/co2/when_all.hpp` | 4 | 出错优先于已停止；`await_completion()` |
| `include/co2/async_generator.hpp` | 5 | 生产者的错误与已停止；`next()` 的 `await_completion()` |
| `include/co2/callback.hpp` | 6 | 续体的四个出口；结果共用存储 |
| `include/co2/just.hpp` | 7 | `just_stopped`、`stopIfRequested` |
| `include/co2/env.hpp` | 7 | `detail::stopRequestedOf` |
| `include/co2/stop_token.hpp` | 7、9 | `EnvStopToken::stopRequested()`；移动构造 |
| `include/co2/unstoppable.hpp`（新） | 8 | `co2::unstoppable` |
| `include/co2/scheduler.hpp` | 9 | `scheduleOn` 是取消点 |
| `include/co2/co2.hpp` | 8 | 总头文件加上新头 |
| `tests/completion_support.hpp`（新） | 2 | 两个测试文件共用的 awaiter 与协程 |
| `tests/completion_tests.cpp` | 2 | 机制本身的测试（内容不变，只搬走支撑代码） |
| `tests/completion_components_tests.cpp`（新） | 3～9 | 各组件、已停止的产生方、取消点的测试 |
| `tests/task_error_taken_twice_contract_violation_test.cpp`（新） | 1 | 新增的契约违规 |
| `tests/allocation_tests.cpp` | 6 | 回调 awaiter 的大小断言 |
| `tests/spawn_tests.cpp` | 9 | 受取消点影响的测试 |
| `tests/CMakeLists.txt` | 1、3 | 登记新测试 |
| `benchmarks/co2_benchmarks.cpp`、`README.md`、`docs/` | 10 | 基准与文档 |

## 环境与命令

**构建与测试。** 每个任务的"构建并运行全部测试"都指下面三条命令（第一条只需要在第一次、以及新增了测试文件之后执行）：

```bash
cmake -S . -B /tmp/co2-build/gcc14 -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=g++
cmake --build /tmp/co2-build/gcc14 -j 12
ctest --test-dir /tmp/co2-build/gcc14 -j 8
```

单独运行一个测试程序（失败时它会打印出错的行号）：

```bash
/tmp/co2-build/gcc14/tests/co2-completion-components-tests
```

这台机器上的 ctest 在没有失败时打印 `100% tests passed out of N`。

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

### Task 1: 修正——`Task` 的错误只可取走一次

写这份计划时发现的缺陷，出在计划（一）的任务 5（提交 `8a73db4`）：等待者经 `await_completion()` 取走错误时，`TaskPromise` 只是把它移出去，自己仍然记着"有错误"。同一个失败的 Task 被等待两次——例如先 `co2::asResult(task)` 再 `CO2_AWAIT(task)`——第二次就把一个已经搬空的错误再交出去；带着异常的那一种到了根上会重抛一个空的 `exception_ptr`，进程崩溃。取值两次本来就是契约违规，取错误两次应当一样。

**Files:**
- Create: `tests/task_error_taken_twice_contract_violation_test.cpp`
- Modify: `tests/CMakeLists.txt`
- Modify: `include/co2/task.hpp`

**Interfaces:**
- Consumes: `TaskPromise<T>` 的 `state`、`storage`、`completion()`（计划一）；`co2::asResult`。
- Produces: `Error co2::detail::TaskPromise<T>::takeError() noexcept`——取走错误，之后 `state` 是 `TaskState::empty`；`completion()` 与 `TaskAccess::takeOutcome` 都经它取。后面的任务不直接调用它。

- [ ] **Step 1: 写测试**

新建 `tests/task_error_taken_twice_contract_violation_test.cpp`：

````cpp
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

#include "co2/as_result.hpp"
#include "co2/coroutine.hpp"
#include "co2/optional.hpp"
#include "co2/result.hpp"
#include "co2/sync_wait.hpp"
#include "co2/task.hpp"

namespace {

std::atomic<bool> atExpectedFailurePoint{};

// 预期的违规由 Task 的 await_resume() 报告（task.hpp）。
void contractViolationHandler(co2::ContractViolation const& violation) {
    auto const expected = atExpectedFailurePoint.load(std::memory_order_acquire) &&
                          std::strstr(violation.file, "task.hpp") != nullptr;
    std::_Exit(expected ? 86 : 87);
}

auto fails() CO2_BEG(co2::Task<int>, ()) {
    throw std::runtime_error{"boom"};
    CO2_RETURN(0);
}
CO2_END

auto awaitsTheSameTaskTwice()
    CO2_BEG(co2::Task<int>, (), co2::Task<int> child;
            co2::optional<co2::Result<int>> first; int second{};) {
    child = fails();
    CO2_AWAIT_SET(first, co2::asResult(child)); // 第一次：错误作为值被取走
    if (not first->hasError()) CO2_RETURN(-1);
    atExpectedFailurePoint.store(true, std::memory_order_release);
    CO2_AWAIT_SET(second, child); // 第二次：没有东西可给
    CO2_RETURN(second);
}
CO2_END

} // namespace

// 一个 Task 的结果只可取走一次，错误与值一样。第一次等待把错误取走之后，再等待同一个
// Task 是契约违规——而不是把一个已经被搬空的错误再交出去一次。
int main() {
    co2::setContractViolationHandler(&contractViolationHandler);
    try {
        co2::syncWait(awaitsTheSameTaskTwice());
    } catch (...) {
    }
    return EXIT_FAILURE;
}
````

修改 `tests/CMakeLists.txt`：

````diff
diff --git a/tests/CMakeLists.txt b/tests/CMakeLists.txt
index b320dde..2993db5 100644
--- a/tests/CMakeLists.txt
+++ b/tests/CMakeLists.txt
@@ -27,7 +27,8 @@ set(co2_contract_violation_test_sources
     optional_empty_value_contract_violation_test.cpp
     error_from_success_code_contract_violation_test.cpp
     stopped_without_entry_contract_violation_test.cpp
-    stopped_through_legacy_awaiter_contract_violation_test.cpp)
+    stopped_through_legacy_awaiter_contract_violation_test.cpp
+    task_error_taken_twice_contract_violation_test.cpp)
 
 # 构建测试用的 C++ 标准。库的最低要求是 14，默认就测它；CI 另以 17 / 20 / 23 各跑一遍，
 # 保证较新的标准下（弃用告警、更严格的规则）同样干净。
````

- [ ] **Step 2: 构建并运行，确认失败**

构建并运行全部测试（新增了测试文件，要重新执行配置那一条）。

Expected: `ctest` 报一个失败：`co2-task-error-taken-twice-contract-violation-test`。单独运行它会崩溃（段错误），而不是以退出码 86 结束。

- [ ] **Step 3: 实现**

修改 `include/co2/task.hpp`：

````diff
diff --git a/include/co2/task.hpp b/include/co2/task.hpp
index 55feece..b5bace4 100644
--- a/include/co2/task.hpp
+++ b/include/co2/task.hpp
@@ -145,14 +145,22 @@ template <class T> struct TaskPromise {
         return std::forward<Awaitable>(awaitable);
     }
 
-    // 这个 Task 是怎么结束的。给认识 await_completion() 的等待者用：错误被移出去，所以
-    // 每次等待只问一次。值已被取走（empty）时也报告"有值"，由随后的 result() 诊断。
+    // 这个 Task 是怎么结束的。给认识 await_completion() 的等待者用：错误在这里被取走。
+    // 结果已被取走（empty）时报告"有值"，由随后的 result() 诊断为契约违规。
     Completion completion() noexcept {
         if (state == TaskState::stopped) return Completion::stopped();
-        if (state == TaskState::error) return Completion::error(std::move(storage.error));
+        if (state == TaskState::error) return Completion::error(takeError());
         return Completion::value();
     }
 
+    // 取走错误，只可一次，与取值相同：之后本 Task 没有结果可给。前置条件：state 是 error。
+    Error takeError() noexcept {
+        Error taken{std::move(storage.error)};
+        storage.error.~Error();
+        state = TaskState::empty;
+        return taken;
+    }
+
     // 取走值，只可一次。它同时是不认识 await_completion() 的调用方的兜底（手写的包装
     // awaiter、原生 co_await 都直接调用 await_resume()）：错误在这里变回异常，与从前
     // 一样；已停止没有值可给，是契约违规。
@@ -342,8 +350,7 @@ struct TaskAccess {
         CO2_CONTRACT_CHECK(task.handle && task.handle.done());
         auto& promise = task.handle.promise();
         if (promise.state == TaskState::stopped) return Result<T>::stopped();
-        if (promise.state == TaskState::error)
-            return Result<T>{std::move(promise.storage.error)};
+        if (promise.state == TaskState::error) return Result<T>{promise.takeError()};
         return valueAsResult(task, std::is_void<T>{});
     }
 
````

- [ ] **Step 4: 构建并运行全部测试，确认通过**

Expected: `100% tests passed out of 29`。

- [ ] **Step 5: 提交**

```bash
git add include/co2/task.hpp tests/CMakeLists.txt \
        tests/task_error_taken_twice_contract_violation_test.cpp
git commit -F - <<'EOF'
修正：Task 的错误只可取走一次

上一批改动（8a73db4）里，等待者经 await_completion() 取走错误时只是把它移出去，
Task 仍然记着"有错误"。同一个失败的 Task 被等待两次——比如先 asResult(task) 再
CO2_AWAIT(task)——第二次就会把一个已经搬空的错误再交出去；带着异常的那一种到了
根上重抛一个空的 exception_ptr，进程崩溃。

现在取走错误与取走值一样：之后 Task 没有结果可给，再等待它是契约违规。新增一个
契约违规测试。
EOF
```

### Task 2: 测试支撑代码搬进共用头文件

只是搬动，不改任何测试的内容。后面各组件的测试放在另一个文件里，需要与 `tests/completion_tests.cpp` 共用 `Ends`、`chain` 等。

**Files:**
- Create: `tests/completion_support.hpp`
- Modify: `tests/completion_tests.cpp`

**Interfaces:**
- Consumes: `tests/completion_tests.cpp` 里现有的定义。
- Produces（都在命名空间 `sample` 里，后面的任务用 `using sample::...;` 引入）:
  - `co2::Completion sample::stopped()`、`co2::Completion sample::failed()`（后者的错误码是 `sample::Errc::gone`）。
  - awaiter `sample::Ends { co2::Completion completion; }`：立刻就绪，有值时 `await_resume()` 给出 7。
  - `sample::ExpectedError`；`enum class sample::Leaf { returns, fails, stops, throws }`。
  - 协程 `sample::chain(int depth, Leaf leaf, int& resumed, std::vector<int>* order)`，返回 `co2::Task<int>`：`depth + 1` 层的等待链，最里面一层按 `leaf` 结束，有值时结果是 `depth + 1`。

- [ ] **Step 1: 先确认现在全部通过**

构建并运行全部测试。Expected: `100% tests passed out of 29`。

- [ ] **Step 2: 搬动**

新建 `tests/completion_support.hpp`：

````cpp
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
````

修改 `tests/completion_tests.cpp`：

````diff
diff --git a/tests/completion_tests.cpp b/tests/completion_tests.cpp
index 5550b3c..dd60e25 100644
--- a/tests/completion_tests.cpp
+++ b/tests/completion_tests.cpp
@@ -25,6 +25,7 @@
 #include "co2/spawn.hpp"
 #include "co2/sync_wait.hpp"
 #include "co2/task.hpp"
+#include "completion_support.hpp"
 #include "sample_errc.hpp"
 
 namespace {
@@ -44,24 +45,19 @@ std::vector<std::string> events;
 
 void record(char const* const event) { events.emplace_back(event); }
 
-co2::Completion stopped() { return co2::Completion::stopped(); }
-co2::Completion failed() { return co2::Completion::error(sample::Errc::gone); }
+// 与 completion_components_tests.cpp 共用的 awaiter 与协程，见 completion_support.hpp。
+using sample::chain;
+using sample::Ends;
+using sample::ExpectedError;
+using sample::failed;
+using sample::Leaf;
+using sample::stopped;
 
 // ---------------------------------------------------------------------------
 // 测试自己的 awaiter
 
-// 立刻就绪，以给定的方式结束；有值时 await_resume() 给出 7。
-struct Ends {
-    co2::Completion completion;
-
-    bool await_ready() const noexcept { return true; }
-    void await_suspend(co2::coroutine_handle<>) const noexcept {}
-    int await_resume() const noexcept { return 7; }
-    co2::Completion await_completion() noexcept { return completion; }
-};
-
-// 同上，并把协议调用与自己的析构记进事件日志。临时对象被移进帧之后析构，那一次不记：
-// 只有被问过 await_completion() 的那个对象才记。
+// 立刻就绪，以给定的方式结束，并把协议调用与自己的析构记进事件日志。临时对象被移进帧
+// 之后析构，那一次不记：只有被问过 await_completion() 的那个对象才记。
 struct Logs {
     explicit Logs(co2::Completion completion_) noexcept
         : completion{std::move(completion_)} {}
@@ -410,40 +406,6 @@ void aCapturedExceptionReachesAStandardPromiseUnchanged() {
 // ---------------------------------------------------------------------------
 // Task：错误与已停止按值沿等待链向上传，只在根上变回异常或空的 optional
 
-struct ExpectedError : std::runtime_error {
-    ExpectedError() : std::runtime_error{"expected"} {}
-};
-
-enum class Leaf { returns, fails, stops, throws };
-
-// 记下每一层的局部是按什么顺序析构的。
-struct DepthGuard {
-    std::vector<int>* order{};
-    int depth{};
-    ~DepthGuard() {
-        if (order != nullptr) order->push_back(depth);
-    }
-};
-
-// 一条 depth + 1 层的等待链：最里面一层按 leaf 结束，外面每一层等待里面一层、把结果
-// 加一。`resumed` 数的是有多少层在等待之后继续执行了。
-auto chain(int depth, Leaf leaf, int& resumed, std::vector<int>* order)
-    CO2_BEG(co2::Task<int>, (depth, leaf, resumed, order), int value{};
-            DepthGuard guard;) {
-    guard.order = order;
-    guard.depth = depth;
-    if (depth == 0) {
-        if (leaf == Leaf::throws) throw ExpectedError{};
-        if (leaf == Leaf::fails) CO2_AWAIT(Ends{failed()});
-        if (leaf == Leaf::stops) CO2_AWAIT(Ends{stopped()});
-        CO2_RETURN(1);
-    }
-    CO2_AWAIT_SET(value, chain(depth - 1, leaf, resumed, order));
-    ++resumed;
-    CO2_RETURN(value + 1);
-}
-CO2_END
-
 void valuesStillFlowThroughTheChain() {
     auto resumed = 0;
     CHECK(co2::syncWait(chain(3, Leaf::returns, resumed, nullptr)) == 4);
````

- [ ] **Step 3: 构建并运行全部测试，确认仍然通过**

Expected: `100% tests passed out of 29`。

- [ ] **Step 4: 提交**

```bash
git add tests/completion_support.hpp tests/completion_tests.cpp
git commit -F - <<'EOF'
测试：三种完成方式的支撑代码搬进共用头文件

Ends、stopped()、failed()、Leaf、chain 等从 completion_tests.cpp 搬到
tests/completion_support.hpp。后面各组件的测试放在另一个文件里，两边共用。只是
搬动，测试内容不变。
EOF
```

### Task 3: 等待 `JoinHandle`

设计稿第 4 节、第 10 节第 6 步的第一项。

**Files:**
- Create: `tests/completion_components_tests.cpp`
- Modify: `tests/CMakeLists.txt`
- Modify: `include/co2/task.hpp`、`include/co2/spawn.hpp`

**Interfaces:**
- Consumes: `TaskPromise<T>::completion()`（任务 1 之后它把错误取走）；`sample::chain`、`sample::Leaf`（任务 2）。
- Produces:
  - `template <class T> static Completion co2::detail::TaskAccess::completionOf(Task<T>&) noexcept`——这个 Task 是怎么结束的，错误在这里被取走。任务 4 也用它。
  - `Completion co2::detail::JoinAwaiter<T>::await_completion() noexcept`、`Completion co2::JoinHandle<T>::await_completion() noexcept`。
  - 新测试文件 `tests/completion_components_tests.cpp`：自带 `fail` / `CHECK`，匿名命名空间里 `using sample::chain; using sample::Leaf;`。后面的任务往它的末尾（`} // namespace` 之前）加小节，并在 `main()` 里登记。

- [ ] **Step 1: 写测试**

新建 `tests/completion_components_tests.cpp`：

````cpp
// 三种完成方式在各个组件上的表现（docs/design/completion-channels.md 第 4 节）。
// 机制本身——恢复点、promise 的两个入口、Task——在 completion_tests.cpp 里测。

#include <cstdlib>
#include <iostream>
#include <system_error>
#include <utility>

#include "co2/as_result.hpp"
#include "co2/coroutine.hpp"
#include "co2/manual_executor.hpp"
#include "co2/optional.hpp"
#include "co2/result.hpp"
#include "co2/scheduler.hpp"
#include "co2/spawn.hpp"
#include "co2/sync_wait.hpp"
#include "co2/task.hpp"
#include "completion_support.hpp"
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

using sample::chain;
using sample::Leaf;

// ---------------------------------------------------------------------------
// 等待 JoinHandle：被 spawn 的 Task 出错或已停止，等待它的协程以同样的结果结束

auto awaitsAHandle(co2::JoinHandle<int>& handle, bool& reached)
    CO2_BEG(co2::Task<int>, (handle, reached), int value{};) {
    CO2_AWAIT_SET(value, handle); // 左值：借用
    reached = true;
    CO2_RETURN(value);
}
CO2_END

void awaitingAFinishedHandleSeesHowTheTaskEnded() {
    co2::ManualExecutor executor;
    auto resumed = 0;
    auto stops = co2::spawn(executor, chain(0, Leaf::stops, resumed, nullptr));
    auto fails = co2::spawn(executor, chain(0, Leaf::fails, resumed, nullptr));
    auto returns = co2::spawn(executor, chain(0, Leaf::returns, resumed, nullptr));
    CHECK(executor.run() == 3U);

    auto reached = false;
    CHECK(not co2::syncWait(awaitsAHandle(stops, reached)).has_value());
    CHECK(not reached);

    // 错误是按值传过来的：它仍然是那个错误码，而不是在 await_resume() 里被抛出、又被
    // 等待者接住的异常。
    auto const failure = co2::syncWait(co2::asResult(awaitsAHandle(fails, reached)));
    CHECK(failure->hasError());
    CHECK(failure->error().exception() == nullptr);
    CHECK(failure->error().code() == sample::Errc::gone);
    CHECK(not reached);

    CHECK(co2::syncWait(awaitsAHandle(returns, reached)) == 1);
    CHECK(reached);
}

auto spawnsAndAwaits(co2::Scheduler& scheduler, Leaf leaf, int& resumed, bool& reached)
    CO2_BEG(co2::Task<int>, (scheduler, leaf, resumed, reached), int value{};) {
    // 右值：JoinHandle 自己是 awaiter，被搬进本协程的帧。
    CO2_AWAIT_SET(value, co2::spawn(scheduler, chain(0, leaf, resumed, nullptr)));
    reached = true;
    CO2_RETURN(value);
}
CO2_END

void aSuspendedAwaiterOfAHandleIsResumedWithTheOutcome() {
    co2::ManualExecutor awaiting;
    co2::ManualExecutor spawned;
    auto resumed = 0;
    auto reached = false;
    auto outer =
        co2::spawn(awaiting, spawnsAndAwaits(spawned, Leaf::stops, resumed, reached));
    CHECK(awaiting.run() == 1U); // 外层启动，spawn 了内层，挂在它的 JoinHandle 上
    CHECK(not outer.isReady());
    CHECK(spawned.run() == 1U); // 内层以已停止结束，就地恢复外层
    CHECK(outer.isReady());
    CHECK(not outer.join().has_value());
    CHECK(not reached);
}

} // namespace

int main() {
    awaitingAFinishedHandleSeesHowTheTaskEnded();
    aSuspendedAwaiterOfAHandleIsResumedWithTheOutcome();
    return 0;
}
````

修改 `tests/CMakeLists.txt`：

````diff
diff --git a/tests/CMakeLists.txt b/tests/CMakeLists.txt
index 2993db5..c6d5642 100644
--- a/tests/CMakeLists.txt
+++ b/tests/CMakeLists.txt
@@ -12,7 +12,8 @@ set(co2_test_sources
     allocation_tests.cpp
     edge_case_tests.cpp
     vocabulary_tests.cpp
-    completion_tests.cpp)
+    completion_tests.cpp
+    completion_components_tests.cpp)
 
 # 契约违规测试：程序在预期的失败点经由契约处理器以退出码 86 结束。
 set(co2_contract_violation_test_sources
````

- [ ] **Step 2: 构建并运行，确认失败**

构建并运行全部测试（新增了测试文件，要重新执行配置那一条）。

Expected: `ctest` 报一个失败：`co2-completion-components-tests` 异常终止（`terminate called without an active exception`）。被 spawn 的 Task 已停止，而 `JoinHandle` 的 awaiter 还只有 `await_resume()`，那是契约违规。

- [ ] **Step 3: 实现**

修改 `include/co2/task.hpp`、`include/co2/spawn.hpp`：

````diff
diff --git a/include/co2/spawn.hpp b/include/co2/spawn.hpp
index 7e4937d..0fa8c56 100644
--- a/include/co2/spawn.hpp
+++ b/include/co2/spawn.hpp
@@ -6,6 +6,7 @@
 #include <type_traits>
 #include <utility>
 
+#include "co2/completion.hpp"
 #include "co2/contract.hpp"
 #include "co2/coroutine_handle.hpp"
 #include "co2/detail/late_init.hpp"
@@ -24,6 +25,9 @@
 // 的 JoinHandle 析构只是销毁帧。
 //
 // 完成时等待中的协程在完成线程上被对称转移恢复（标准行为）；join() 阻塞调用线程。
+//
+// 三种完成方式：在协程里等待 JoinHandle 时，被 spawn 的 Task 出错或已停止，等待它的
+// 协程以同样的结果结束；join() 对已停止返回空的 optional，出错时抛出。
 
 namespace co2 {
 
@@ -113,6 +117,9 @@ template <class T> struct JoinAwaiter {
     }
 
     T await_resume() { return TaskAccess::takeResult(*task); }
+
+    // 被 spawn 的 Task 出错或已停止：等待它的协程以同样的结果结束。
+    Completion await_completion() noexcept { return TaskAccess::completionOf(*task); }
 };
 
 } // namespace detail
@@ -180,6 +187,8 @@ template <class T> struct JoinHandle {
 
     T await_resume() { return awaiter().await_resume(); }
 
+    Completion await_completion() noexcept { return awaiter().await_completion(); }
+
     detail::JoinAwaiter<T> operator_co_await() & noexcept { return awaiter(); }
 
   private:
diff --git a/include/co2/task.hpp b/include/co2/task.hpp
index b5bace4..9b69fcb 100644
--- a/include/co2/task.hpp
+++ b/include/co2/task.hpp
@@ -345,6 +345,13 @@ struct TaskAccess {
         return takeRootResult(task, std::is_void<T>{});
     }
 
+    // 这个 Task 是怎么结束的；错误在这里被取走。持有 Task 的 awaiter（JoinHandle、
+    // whenAll）用它实现自己的 await_completion()。
+    template <class T> static Completion completionOf(Task<T>& task) noexcept {
+        CO2_CONTRACT_CHECK(task.handle && task.handle.done());
+        return task.handle.promise().completion();
+    }
+
     // 把结果整个取成值（只可一次）：三种结果都不抛。asResult(Task) 的根用它。
     template <class T> static Result<T> takeOutcome(Task<T>& task) {
         CO2_CONTRACT_CHECK(task.handle && task.handle.done());
````

- [ ] **Step 4: 构建并运行全部测试，确认通过**

Expected: `100% tests passed out of 30`。

- [ ] **Step 5: 提交**

```bash
git add include/co2/task.hpp include/co2/spawn.hpp tests/CMakeLists.txt \
        tests/completion_components_tests.cpp
git commit -F - <<'EOF'
等待 JoinHandle：被 spawn 的 Task 出错或已停止，等待者以同样的结果结束

docs/design/completion-channels.md 第 4 节、第 10 节第 6 步。JoinHandle 的 awaiter
提供 await_completion()，直接读被 spawn 的 Task 的结果。之前错误是在 await_resume()
里抛出、再被等待者接住的，已停止则是契约违规。

各组件的测试放在新文件 tests/completion_components_tests.cpp 里。
EOF
```

### Task 4: `whenAll`

设计稿第 4 节，规则取自 `std::execution::when_all`：任一 child 出错或已停止都向其余 child 请求停止，等全部结束；有出错的，以时间上第一个错误完成；否则有已停止的，以已停止完成。

**Files:**
- Modify: `tests/completion_components_tests.cpp`
- Modify: `include/co2/task.hpp`、`include/co2/when_all.hpp`

**Interfaces:**
- Consumes: `TaskAccess::completionOf`（任务 3）；`co2::detail::TaskState`（计划一）；`co2::just_error`。
- Produces:
  - `template <class T> static TaskState co2::detail::TaskAccess::stateOf(Task<T> const&) noexcept`。
  - `coroutine_handle<> co2::detail::WhenAllState::childCompleted(std::size_t index, TaskState outcome) noexcept`（第二个参数原来是 `bool failed`）；`bool WhenAllState::anyStopped() const noexcept`。
  - `Completion await_completion() noexcept`，在 `co2::detail::WhenAllTuple<T...>` 与 `co2::detail::WhenAllRange<T>` 上。
  - 测试里后面的任务要用的东西（`tests/completion_components_tests.cpp` 的匿名命名空间里）：协程 `awaitsBoth(co2::Task<int> first, co2::Task<int> second, bool& reached)`，返回 `co2::Task<int>`，等待 `whenAll(first, second)` 后把两个结果相加；`co2::Result<int> outcomeOfBoth(co2::Task<int>, co2::Task<int>)`；`co2::Task<int> leaf(Leaf)`。

- [ ] **Step 1: 写测试**

修改 `tests/completion_components_tests.cpp`：

````diff
diff --git a/tests/completion_components_tests.cpp b/tests/completion_components_tests.cpp
index b877536..649d601 100644
--- a/tests/completion_components_tests.cpp
+++ b/tests/completion_components_tests.cpp
@@ -4,17 +4,23 @@
 #include <cstdlib>
 #include <iostream>
 #include <system_error>
+#include <tuple>
 #include <utility>
+#include <vector>
 
 #include "co2/as_result.hpp"
 #include "co2/coroutine.hpp"
+#include "co2/env.hpp"
+#include "co2/just.hpp"
 #include "co2/manual_executor.hpp"
 #include "co2/optional.hpp"
 #include "co2/result.hpp"
 #include "co2/scheduler.hpp"
 #include "co2/spawn.hpp"
+#include "co2/stop_token.hpp"
 #include "co2/sync_wait.hpp"
 #include "co2/task.hpp"
+#include "co2/when_all.hpp"
 #include "completion_support.hpp"
 #include "sample_errc.hpp"
 
@@ -92,10 +98,153 @@ void aSuspendedAwaiterOfAHandleIsResumedWithTheOutcome() {
     CHECK(not reached);
 }
 
+// ---------------------------------------------------------------------------
+// whenAll：出错优先于已停止，取时间上第一个错误（std::execution::when_all 的规则）
+
+auto failsWith(sample::Errc code) CO2_BEG(co2::Task<int>, (code)) {
+    CO2_AWAIT(co2::just_error(code));
+    CO2_RETURN(0);
+}
+CO2_END
+
+auto awaitsBoth(co2::Task<int> first, co2::Task<int> second, bool& reached)
+    CO2_BEG(co2::Task<int>, (first, second, reached), std::tuple<int, int> values;) {
+    CO2_AWAIT_SET(values, co2::whenAll(std::move(first), std::move(second)));
+    reached = true;
+    CO2_RETURN(std::get<0>(values) + std::get<1>(values));
+}
+CO2_END
+
+// 等待 whenAll 的那个协程是怎么结束的，按值取出来。
+co2::Result<int> outcomeOfBoth(co2::Task<int> first, co2::Task<int> second) {
+    auto reached = false;
+    auto outcome = *co2::syncWait(
+        co2::asResult(awaitsBoth(std::move(first), std::move(second), reached)));
+    CHECK(reached == outcome.hasValue()); // 没有值时，等待之后的语句不执行
+    return outcome;
+}
+
+co2::Task<int> leaf(Leaf const how) {
+    static auto resumed = 0;
+    return chain(0, how, resumed, nullptr);
+}
+
+void whenAllDeliversValuesWhenEveryChildHasOne() {
+    auto const both = outcomeOfBoth(leaf(Leaf::returns), leaf(Leaf::returns));
+    CHECK(both.hasValue() && both.value() == 2);
+}
+
+void whenAllCompletesWithTheFirstErrorInTime() {
+    // child 按参数顺序内联启动，所以排在前面的先出错。
+    auto const first =
+        outcomeOfBoth(failsWith(sample::Errc::retry_later), failsWith(sample::Errc::gone));
+    CHECK(first.hasError() && first.error().code() == sample::Errc::retry_later);
+
+    auto const second =
+        outcomeOfBoth(failsWith(sample::Errc::gone), failsWith(sample::Errc::retry_later));
+    CHECK(second.hasError() && second.error().code() == sample::Errc::gone);
+
+    // 错误是按值传过来的：仍然是错误码，而不是被抛出又接住的异常。
+    CHECK(first.error().exception() == nullptr);
+}
+
+void whenAllCompletesStoppedWhenAChildStopsAndNoneFails() {
+    CHECK(outcomeOfBoth(leaf(Leaf::returns), leaf(Leaf::stops)).isStopped());
+    CHECK(outcomeOfBoth(leaf(Leaf::stops), leaf(Leaf::returns)).isStopped());
+    CHECK(outcomeOfBoth(leaf(Leaf::stops), leaf(Leaf::stops)).isStopped());
+}
+
+void whenAllPrefersAnErrorOverStopped() {
+    auto const stoppedFirst =
+        outcomeOfBoth(leaf(Leaf::stops), failsWith(sample::Errc::retry_later));
+    CHECK(stoppedFirst.hasError());
+    CHECK(stoppedFirst.error().code() == sample::Errc::retry_later);
+
+    auto const failedFirst =
+        outcomeOfBoth(failsWith(sample::Errc::retry_later), leaf(Leaf::stops));
+    CHECK(failedFirst.hasError());
+    CHECK(failedFirst.error().code() == sample::Errc::retry_later);
+}
+
+// 把等待者停放起来，句柄交给测试，由测试决定什么时候恢复。
+struct Parks {
+    co2::coroutine_handle<>* parked;
+
+    bool await_ready() const noexcept { return false; }
+    void await_suspend(co2::coroutine_handle<> const handle) const noexcept {
+        *parked = handle;
+    }
+    void await_resume() const noexcept {}
+};
+
+auto parksThenReportsStop(co2::coroutine_handle<>& parked, bool& sawStop)
+    CO2_BEG(co2::Task<int>, (parked, sawStop), co2::stop_token token;) {
+    CO2_AWAIT_SET(token, co2::getStopToken());
+    CO2_AWAIT(Parks{&parked});
+    sawStop = token.stop_requested();
+    CO2_RETURN(1);
+}
+CO2_END
+
+// 一个 child 已停止，与出错一样向其余 child 请求停止，然后等它们全部结束。
+void whenAllRequestsStopFromTheOthersWhenAChildStops() {
+    co2::ManualExecutor executor;
+    auto parked = co2::coroutine_handle<>{};
+    auto sawStop = false;
+    auto reached = false;
+    auto outer = co2::spawn(
+        executor, awaitsBoth(parksThenReportsStop(parked, sawStop), leaf(Leaf::stops),
+                             reached));
+    CHECK(executor.run() == 1U);
+    CHECK(parked);              // 第一个 child 停在那里
+    CHECK(not outer.isReady()); // whenAll 还在等它
+
+    parked.resume();
+    CHECK(sawStop); // 它恢复时看到了停止请求
+    CHECK(outer.isReady());
+    CHECK(not outer.join().has_value());
+    CHECK(not reached);
+}
+
+auto awaitsAll(std::vector<co2::Task<int>> tasks, bool& reached)
+    CO2_BEG(co2::Task<int>, (tasks, reached), std::vector<int> values;) {
+    CO2_AWAIT_SET(values, co2::whenAll(std::move(tasks)));
+    reached = true;
+    CO2_RETURN(static_cast<int>(values.size()));
+}
+CO2_END
+
+co2::Result<int> outcomeOfAll(Leaf const first, Leaf const second, Leaf const third) {
+    auto tasks = std::vector<co2::Task<int>>{};
+    tasks.push_back(leaf(first));
+    tasks.push_back(leaf(second));
+    tasks.push_back(leaf(third));
+    auto reached = false;
+    return *co2::syncWait(co2::asResult(awaitsAll(std::move(tasks), reached)));
+}
+
+void whenAllOfAVectorFollowsTheSameRules() {
+    auto const values = outcomeOfAll(Leaf::returns, Leaf::returns, Leaf::returns);
+    CHECK(values.hasValue() && values.value() == 3);
+
+    CHECK(outcomeOfAll(Leaf::returns, Leaf::stops, Leaf::returns).isStopped());
+
+    auto const failure = outcomeOfAll(Leaf::stops, Leaf::fails, Leaf::returns);
+    CHECK(failure.hasError());
+    CHECK(failure.error().exception() == nullptr);
+    CHECK(failure.error().code() == sample::Errc::gone);
+}
+
 } // namespace
 
 int main() {
     awaitingAFinishedHandleSeesHowTheTaskEnded();
     aSuspendedAwaiterOfAHandleIsResumedWithTheOutcome();
+    whenAllDeliversValuesWhenEveryChildHasOne();
+    whenAllCompletesWithTheFirstErrorInTime();
+    whenAllCompletesStoppedWhenAChildStopsAndNoneFails();
+    whenAllPrefersAnErrorOverStopped();
+    whenAllRequestsStopFromTheOthersWhenAChildStops();
+    whenAllOfAVectorFollowsTheSameRules();
     return 0;
 }
````

- [ ] **Step 2: 构建并运行，确认失败**

```bash
cmake --build /tmp/co2-build/gcc14 -j 12
/tmp/co2-build/gcc14/tests/co2-completion-components-tests
```

Expected: `check failed at line 141: first.hasError() && first.error().code() == sample::Errc::retry_later`。错误此时是在 `await_resume()` 里抛出、又被等待者接住的，已经不是原来那个错误码。

- [ ] **Step 3: 实现**

修改 `include/co2/task.hpp`、`include/co2/when_all.hpp`：

````diff
diff --git a/include/co2/task.hpp b/include/co2/task.hpp
index 9b69fcb..3ca51ac 100644
--- a/include/co2/task.hpp
+++ b/include/co2/task.hpp
@@ -345,6 +345,11 @@ struct TaskAccess {
         return takeRootResult(task, std::is_void<T>{});
     }
 
+    template <class T> static TaskState stateOf(Task<T> const& task) noexcept {
+        CO2_CONTRACT_CHECK(task.handle);
+        return task.handle.promise().state;
+    }
+
     // 这个 Task 是怎么结束的；错误在这里被取走。持有 Task 的 awaiter（JoinHandle、
     // whenAll）用它实现自己的 await_completion()。
     template <class T> static Completion completionOf(Task<T>& task) noexcept {
diff --git a/include/co2/when_all.hpp b/include/co2/when_all.hpp
index 05c579d..07f74ce 100644
--- a/include/co2/when_all.hpp
+++ b/include/co2/when_all.hpp
@@ -8,6 +8,7 @@
 #include <utility>
 #include <vector>
 
+#include "co2/completion.hpp"
 #include "co2/contract.hpp"
 #include "co2/coroutine_handle.hpp"
 #include "co2/detail/late_init.hpp"
@@ -21,10 +22,15 @@
 //   - 值通道拼接：whenAll(Task<int>, Task<void>, Task<string>) 的结果是
 //     std::tuple<int, std::string>；全部 void 时结果是 void；vector<Task<T>> 给出
 //     vector<T>（T 为 void 时 void）。
-//   - 任一 child 失败即向其余 child 请求停止（whenAll 自己的 stop_source），等**全部**
-//     child 完成后重抛**时间上第一个**失败的异常。
+//   - 任一 child 出错即向其余 child 请求停止（whenAll 自己的 stop_source），等**全部**
+//     child 结束后以**时间上第一个**错误完成。
+//   - 没有 child 出错、但有 child 已停止：同样向其余 child 请求停止，等全部结束后以
+//     已停止完成。出错优先于已停止。
 //   - 父的 stop_token 转发给 child。
 //
+// 后两种结果经 await_completion() 按值交给等待它的协程（见 completion-channels.md）；
+// 不认识这个成员的调用方直接调用 await_resume()，错误在那里抛出。
+//
 // 实现：awaiter 自身就是操作状态。每个 child 一个内嵌的手写续体帧（标准布局，不占堆），
 // child 的 FinalAwaiter 对称转移到它；计数初值 N+1，启动方最后放下自己的一份——
 // 全部 child 同步完成时 await_suspend 返回 false 不挂起，否则最后到达的 child 把等待者
@@ -55,14 +61,18 @@ struct WhenAllState {
             forwarding.emplace(std::move(parent), ForwardStop{&source});
     }
 
-    // child 完成：失败则记下时间上第一个失败者并请求兄弟停止；最后到达者拿到等待者。
+    // child 结束：出错则记下时间上第一个出错者，已停止则记一笔；两种情况都请求兄弟
+    // 停止。最后到达者拿到等待者。
     coroutine_handle<> childCompleted(std::size_t const index,
-                                      bool const failed) noexcept {
-        if (failed) {
+                                      TaskState const outcome) noexcept {
+        if (outcome == TaskState::error) {
             auto expected = noFailure;
             firstFailure.compare_exchange_strong(expected, index,
                                                  std::memory_order_acq_rel);
             source.request_stop();
+        } else if (outcome == TaskState::stopped) {
+            stopped.store(true, std::memory_order_release);
+            source.request_stop();
         }
         if (remaining.fetch_sub(1U, std::memory_order_acq_rel) == 1U) return awaiting;
         return nullptr;
@@ -77,15 +87,18 @@ struct WhenAllState {
         return firstFailure.load(std::memory_order_acquire);
     }
 
+    bool anyStopped() const noexcept { return stopped.load(std::memory_order_acquire); }
+
     std::atomic<std::size_t> remaining{0U};
     std::atomic<std::size_t> firstFailure{noFailure};
+    std::atomic<bool> stopped{false};
     coroutine_handle<> awaiting;
     stop_source source;
     LateInit<stop_callback<ForwardStop>> forwarding; // 晚于 source 构造、先于它析构
 };
 
-template <class T> bool whenAllTaskFailed(void* const task) noexcept {
-    return TaskAccess::hasError(*static_cast<Task<T>*>(task));
+template <class T> TaskState whenAllTaskState(void* const task) noexcept {
+    return TaskAccess::stateOf(*static_cast<Task<T>*>(task));
 }
 
 // 每个 child 的续体帧：标准布局，首成员是帧头。
@@ -93,7 +106,7 @@ struct WhenAllChild {
     FrameHeader header;
     WhenAllState* state;
     void* task;
-    bool (*failed)(void*) noexcept;
+    TaskState (*outcome)(void*) noexcept;
     std::size_t index;
 
     coroutine_handle<> handle() noexcept {
@@ -105,7 +118,7 @@ struct WhenAllChild {
                       "the completion frame must stay standard-layout");
         auto* const self = reinterpret_cast<WhenAllChild*>(header);
         auto const next =
-            self->state->childCompleted(self->index, self->failed(self->task));
+            self->state->childCompleted(self->index, self->outcome(self->task));
         return next ? HandleAccess::header(next) : nullptr;
     }
 };
@@ -116,7 +129,7 @@ void startWhenAllChild(WhenAllChild& child, WhenAllState& state, Task<T>& task,
     CO2_CONTRACT_CHECK(task);
     child =
         WhenAllChild{FrameHeader{&noopDestroy, &WhenAllChild::complete, false, false},
-                     &state, &task, &whenAllTaskFailed<T>, index};
+                     &state, &task, &whenAllTaskState<T>, index};
     TaskAccess::arm(task, child.handle(), state.source.get_token()).resume();
 }
 
@@ -166,7 +179,26 @@ template <class... T> struct WhenAllTuple {
         return collect(std::is_void<Result>{}, std::index_sequence_for<T...>{});
     }
 
+    // 全部 child 结束之后被问到。出错优先于已停止；错误从第一个出错的 child 那里取走。
+    Completion await_completion() noexcept {
+        return completionOfAll(std::index_sequence_for<T...>{});
+    }
+
   private:
+    template <std::size_t... Index>
+    Completion completionOfAll(std::index_sequence<Index...>) noexcept {
+        auto const failure = state.failureIndex();
+        if (failure == WhenAllState::noFailure)
+            return state.anyStopped() ? Completion::stopped() : Completion::value();
+        auto completion = Completion::value();
+        int const ordered[] = {
+            0, (Index == failure
+                    ? (completion = TaskAccess::completionOf(std::get<Index>(tasks)), 0)
+                    : 0)...};
+        static_cast<void>(ordered);
+        return completion;
+    }
+
     template <std::size_t... Index> void startAll(std::index_sequence<Index...>) {
         // 花括号初始化列表保证从左到右求值：child 按参数顺序启动。
         int const ordered[] = {0, (startWhenAllChild(children[Index], state,
@@ -232,6 +264,13 @@ template <class T> struct WhenAllRange {
         return collect(std::is_void<Result>{});
     }
 
+    Completion await_completion() noexcept {
+        auto const failure = state.failureIndex();
+        if (failure != WhenAllState::noFailure)
+            return TaskAccess::completionOf(tasks[failure]);
+        return state.anyStopped() ? Completion::stopped() : Completion::value();
+    }
+
   private:
     Result collect(std::false_type) {
         auto results = Result{};
@@ -254,7 +293,8 @@ template <class T> struct WhenAllRange {
 } // namespace detail
 
 // 并发运行全部 Task，全部完成后按参数顺序交付非 void 结果的 tuple（全部 void 则为
-// void）。任一 child 失败即向其余 child 请求停止，全部完成后重抛时间上第一个异常。
+// void）。任一 child 出错或已停止即向其余 child 请求停止；全部结束后，有 child 出错就
+// 以时间上第一个错误完成，否则有 child 已停止就以已停止完成。
 template <class... T> detail::WhenAllTuple<T...> whenAll(Task<T>... tasks) {
     return detail::WhenAllTuple<T...>{std::move(tasks)...};
 }
````

- [ ] **Step 4: 构建并运行全部测试，确认通过**

Expected: `100% tests passed out of 30`。`co2-when-all-tests` 与 `co2-allocation-tests` 在其中：原有的行为（值的拼接、第一个错误、父的停止请求转发、分配次数）没有变。

- [ ] **Step 5: 提交**

```bash
git add include/co2/task.hpp include/co2/when_all.hpp tests/completion_components_tests.cpp
git commit -F - <<'EOF'
whenAll：出错优先于已停止，按值交给等待者

docs/design/completion-channels.md 第 4 节，规则取自 std::execution::when_all：

- 任一 child 出错或已停止，都向其余 child 请求停止，等全部结束。
- 有 child 出错：以时间上第一个错误完成。
- 没有出错、但有 child 已停止：以已停止完成。以前只有出错才影响结果。

结果经 await_completion() 按值交给等待它的协程；不认识这个成员的调用方照旧从
await_resume() 拿到异常。
EOF
```

### Task 5: `AsyncGenerator`

设计稿第 4 节：生产者出错或已停止即流结束，正在等待的那次 `next()` 以同样的结果完成；那次失败只交付一次，之后的 `next()` 得到 `false`。

**Files:**
- Modify: `tests/completion_components_tests.cpp`
- Modify: `include/co2/async_generator.hpp`

**Interfaces:**
- Consumes: `co2::detail::JustError`、`co2::WithError`（计划一）；`co2::detail::Uninitialized<T>`（`co2/detail/uninitialized.hpp`，已有）；`sample::Ends`、`sample::stopped`、`sample::ExpectedError`（任务 2）。
- Produces:
  - `AsyncGeneratorPromise<T>` 的成员：`void return_error(Error) noexcept`、`void return_stopped() noexcept`、`JustError yield_value(WithError) noexcept`、`Completion takeEnding() noexcept`；`enum class Ending : unsigned char { none, error, stopped }`。原来的数据成员 `error`（`exception_ptr`）不再存在。
  - `Completion co2::AsyncGenerator<T>::NextAwaiter::await_completion() noexcept`。

- [ ] **Step 1: 写测试**

修改 `tests/completion_components_tests.cpp`：

````diff
diff --git a/tests/completion_components_tests.cpp b/tests/completion_components_tests.cpp
index 649d601..534ce68 100644
--- a/tests/completion_components_tests.cpp
+++ b/tests/completion_components_tests.cpp
@@ -9,6 +9,7 @@
 #include <vector>
 
 #include "co2/as_result.hpp"
+#include "co2/async_generator.hpp"
 #include "co2/coroutine.hpp"
 #include "co2/env.hpp"
 #include "co2/just.hpp"
@@ -37,7 +38,10 @@ void fail(char const* expression, int line) {
     } while (false)
 
 using sample::chain;
+using sample::Ends;
+using sample::ExpectedError;
 using sample::Leaf;
+using sample::stopped;
 
 // ---------------------------------------------------------------------------
 // 等待 JoinHandle：被 spawn 的 Task 出错或已停止，等待它的协程以同样的结果结束
@@ -235,6 +239,88 @@ void whenAllOfAVectorFollowsTheSameRules() {
     CHECK(failure.error().code() == sample::Errc::gone);
 }
 
+// ---------------------------------------------------------------------------
+// AsyncGenerator：生产者出错或已停止即流结束，正在等待的那次 next() 以同样的结果完成
+
+// 产生一个元素，然后按 leaf 结束。
+auto producesOneThen(Leaf leaf) CO2_BEG(co2::AsyncGenerator<int>, (leaf)) {
+    CO2_YIELD(1);
+    if (leaf == Leaf::fails) CO2_YIELD(co2::with_error(sample::Errc::gone));
+    if (leaf == Leaf::stops) CO2_AWAIT(Ends{stopped()});
+    if (leaf == Leaf::throws) throw ExpectedError{};
+}
+CO2_END
+
+auto drains(Leaf leaf, int& seen, bool& reachedTheEnd)
+    CO2_BEG(co2::Task<int>, (leaf, seen, reachedTheEnd),
+            co2::AsyncGenerator<int> stream; bool has{};) {
+    stream = producesOneThen(leaf);
+    for (;;) {
+        CO2_AWAIT_SET(has, stream.next());
+        if (not has) break;
+        seen += stream.value();
+    }
+    reachedTheEnd = true;
+    CO2_RETURN(seen);
+}
+CO2_END
+
+void aFailingProducerEndsItsConsumerTheSameWay() {
+    auto seen = 0;
+    auto reachedTheEnd = false;
+    CHECK(co2::syncWait(drains(Leaf::returns, seen, reachedTheEnd)) == 1);
+    CHECK(reachedTheEnd);
+
+    seen = 0;
+    reachedTheEnd = false;
+    auto const failure =
+        co2::syncWait(co2::asResult(drains(Leaf::fails, seen, reachedTheEnd)));
+    CHECK(failure->hasError());
+    CHECK(failure->error().exception() == nullptr); // 按值传过来的错误码
+    CHECK(failure->error().code() == sample::Errc::gone);
+    CHECK(seen == 1); // 出错之前的元素照常交付
+    CHECK(not reachedTheEnd);
+
+    seen = 0;
+    CHECK(co2::syncWait(co2::asResult(drains(Leaf::stops, seen, reachedTheEnd)))
+              ->isStopped());
+    CHECK(seen == 1);
+    CHECK(not reachedTheEnd);
+
+    auto const thrown =
+        co2::syncWait(co2::asResult(drains(Leaf::throws, seen, reachedTheEnd)));
+    CHECK(thrown->hasError());
+    CHECK(thrown->error().exception() != nullptr);
+}
+
+// 返回 0：第二次 next() 有值；1：出错；2：已停止。
+auto looksPastTheFailure(Leaf leaf, bool& hasAfterwards)
+    CO2_BEG(co2::Task<int>, (leaf, hasAfterwards), co2::AsyncGenerator<int> stream;
+            co2::optional<co2::Result<bool>> second; bool has{};) {
+    stream = producesOneThen(leaf);
+    CO2_AWAIT_SET(has, stream.next());
+    CO2_AWAIT_SET(second, co2::asResult(stream.next())); // 生产者在这一步里结束
+    CO2_AWAIT_SET(has, stream.next());                   // 流已经结束
+    hasAfterwards = has;
+    CO2_RETURN(second->isStopped() ? 2 : second->hasError() ? 1 : 0);
+}
+CO2_END
+
+// 那次失败只交付一次：之后的 next() 只报告流已结束。
+void theFailureIsDeliveredOnceAndThenTheStreamIsOver() {
+    auto hasAfterwards = true;
+    CHECK(co2::syncWait(looksPastTheFailure(Leaf::fails, hasAfterwards)) == 1);
+    CHECK(not hasAfterwards);
+
+    hasAfterwards = true;
+    CHECK(co2::syncWait(looksPastTheFailure(Leaf::stops, hasAfterwards)) == 2);
+    CHECK(not hasAfterwards);
+
+    hasAfterwards = true;
+    CHECK(co2::syncWait(looksPastTheFailure(Leaf::returns, hasAfterwards)) == 0);
+    CHECK(not hasAfterwards);
+}
+
 } // namespace
 
 int main() {
@@ -246,5 +332,7 @@ int main() {
     whenAllPrefersAnErrorOverStopped();
     whenAllRequestsStopFromTheOthersWhenAChildStops();
     whenAllOfAVectorFollowsTheSameRules();
+    aFailingProducerEndsItsConsumerTheSameWay();
+    theFailureIsDeliveredOnceAndThenTheStreamIsOver();
     return 0;
 }
````

- [ ] **Step 2: 构建，确认失败**

Expected: 构建失败，`tests/completion_components_tests.cpp` 里的 `producesOneThen` 报 `no matching function for call to 'co2::detail::AsyncGeneratorPromise<int>::yield_value(co2::WithError)'`。

- [ ] **Step 3: 实现**

修改 `include/co2/async_generator.hpp`：

````diff
diff --git a/include/co2/async_generator.hpp b/include/co2/async_generator.hpp
index df2fb8d..0c8054e 100644
--- a/include/co2/async_generator.hpp
+++ b/include/co2/async_generator.hpp
@@ -4,10 +4,14 @@
 #include <type_traits>
 #include <utility>
 
+#include "co2/completion.hpp"
 #include "co2/contract.hpp"
 #include "co2/coroutine.hpp"
 #include "co2/detail/result_storage.hpp"
+#include "co2/detail/uninitialized.hpp"
 #include "co2/env.hpp"
+#include "co2/error.hpp"
+#include "co2/just.hpp"
 #include "co2/stop_token.hpp"
 
 // co2：异步生成器。生产者体内可以 CO2_AWAIT，也可以 CO2_YIELD；消费者：
@@ -22,6 +26,10 @@
 // promise，生产者体内 CO2_AWAIT_SET(token, co2::getStopToken()) 读到它，生产者等待的子
 // Task 也从这里继承——与 Task 等待子 Task 的传播方式相同。
 //
+// 三种完成方式：生产者出错或已停止即流结束，正在等待的那次 next() 以同样的结果完成
+// ——等待它的消费者默认也就此结束，想自己处理就写 asResult(stream.next())。那次失败只
+// 交付一次，之后的 next() 得到 false。生产者体内可以 CO2_YIELD(co2::with_error(e))。
+//
 // 销毁契约：有 next() 未完成时（生产者正在运行或挂起在某个操作上）不能销毁；停在
 // yield 点或从未启动时可以随时销毁。
 
@@ -41,6 +49,10 @@ template <class T> struct AsyncGeneratorPromise {
     // 参数若是句柄，就会被拿去初始化 consumer——生成器一开始就像有一个未完成的 next()。
     AsyncGeneratorPromise() = default;
 
+    ~AsyncGeneratorPromise() {
+        if (ending == Ending::error) failure.destroy();
+    }
+
     AsyncGenerator<T> get_return_object() noexcept;
 
     suspend_always initial_suspend() noexcept { return {}; }
@@ -81,9 +93,30 @@ template <class T> struct AsyncGeneratorPromise {
         return {};
     }
 
+    // CO2_YIELD(co2::with_error(e))：生产者以错误 e 结束。
+    JustError yield_value(WithError failed) noexcept {
+        return JustError{std::move(failed.error)};
+    }
+
     void return_void() noexcept {}
 
-    void unhandled_exception() noexcept { error = std::current_exception(); }
+    // 异常只在这里被捕获一次，之后是一个按值传的 Error。
+    void unhandled_exception() noexcept { fail(Error{std::current_exception()}); }
+
+    // 生产者等待的操作出错 / 已停止（或者它自己 with_error / just_error）：流到此为止。
+    void return_error(Error error) noexcept { fail(std::move(error)); }
+    void return_stopped() noexcept { ending = Ending::stopped; }
+
+    // 生产者是怎么结束的，由正在等待的那次 next() 取走，只交付一次：之后流只是"结束了"。
+    Completion takeEnding() noexcept {
+        auto const how = ending;
+        ending = Ending::none;
+        if (how == Ending::none) return Completion::value();
+        if (how == Ending::stopped) return Completion::stopped();
+        auto completion = Completion::error(std::move(failure.get()));
+        failure.destroy();
+        return completion;
+    }
 
     // ---- 环境 ----
 
@@ -110,13 +143,23 @@ template <class T> struct AsyncGeneratorPromise {
         return pending;
     }
 
+    // 生产者以什么方式失败了、消费者还没取走。none：没有失败，或者已经取走。
+    enum class Ending : unsigned char { none, error, stopped };
+
     coroutine_handle<> consumer;
     ResultStorage<T> stored;
-    std::exception_ptr error;
+    Uninitialized<Error> failure; // ending 为 error 时有效
     // 每次 next() 重新取自那一次的消费者；消费者在这一步里一直挂着，借用即可。两次
     // next() 之间它可能已经失效，但那时生产者不在运行，没有人读它。
     EnvStopToken stopToken;
     bool finished{};
+    Ending ending{Ending::none};
+
+  private:
+    void fail(Error error) noexcept {
+        failure.construct(std::move(error));
+        ending = Ending::error;
+    }
 };
 
 } // namespace detail
@@ -145,15 +188,20 @@ template <class T> struct AsyncGenerator {
             return owner->handle;
         }
 
+        // 不认识 await_completion() 的调用方直接走到这里：生产者的错误在这里抛出（同样
+        // 只抛一次）；已停止没有值可给，是契约违规。
         bool await_resume() {
             auto& promise = owner->handle.promise();
-            if (promise.error) {
-                auto pending = std::exception_ptr{};
-                std::swap(pending, promise.error);
-                std::rethrow_exception(pending);
-            }
+            using Ending = typename promise_type::Ending;
+            if (promise.ending == Ending::error) promise.takeEnding().error().raise();
+            CO2_CONTRACT_CHECK(promise.ending != Ending::stopped);
             return not promise.finished;
         }
+
+        // 生产者出错或已停止：这一次 next() 以同样的结果完成。
+        Completion await_completion() noexcept {
+            return owner->handle.promise().takeEnding();
+        }
     };
 
     AsyncGenerator() noexcept = default;
````

- [ ] **Step 4: 构建并运行全部测试，确认通过**

Expected: `100% tests passed out of 30`。

- [ ] **Step 5: 提交**

```bash
git add include/co2/async_generator.hpp tests/completion_components_tests.cpp
git commit -F - <<'EOF'
AsyncGenerator：生产者出错或已停止即流结束

docs/design/completion-channels.md 第 4 节。正在等待的那次 next() 以同样的结果
完成，消费者默认也就此结束；那次失败只交付一次，之后的 next() 得到 false。生产者
体内可以 CO2_YIELD(co2::with_error(e))。

promise 里的 exception_ptr 换成一个 Error 加一个字节的标志，标志与 finished 相邻。
EOF
```

### Task 6: `fromCallback`

设计稿第 4 节：续体多两个出口 `setError(std::error_code)` 与 `setStopped()`。

结果的存储与设计稿的字面不同。设计稿说"值与错误共用存储"；这里把结果的三种形态——值、错误码、捕获的异常——放进同一块存储，由状态字里的标志区分，而不是存一个 `co2::Error`。原因是 MSVC：那里 `exception_ptr` 是两个指针，`co2::Error` 有三个指针宽，awaiter 会是 9 个指针，掉出 8 个指针的内联槽（每次等待多一次堆分配，`allocation_tests` 里的 `static_assert` 也会在 MSVC 上失败）。分开存之后，任何平台上结果不超过两个指针宽的 awaiter 都正好是 8 个指针。代价是 `CallbackAwaitable<int>` 从 56 字节变成 64 字节——仍在槽里，帧大小与分配次数不变——所以 `allocation_tests` 里钉死它大小的断言要跟着改。

**Files:**
- Modify: `tests/completion_components_tests.cpp`、`tests/allocation_tests.cpp`
- Modify: `include/co2/callback.hpp`

**Interfaces:**
- Consumes: `co2::Error`、`co2::Completion`（计划一）；`co2::detail::Uninitialized<T>`。
- Produces:
  - `void co2::Continuation<T>::setError(std::error_code) const`（前置条件：非零）、`void co2::Continuation<T>::setStopped() const`。`operator()` 与 `setException` 的签名不变。
  - `Completion co2::CallbackAwaitable<T>::await_completion() noexcept`。
  - `CallbackAwaitable<T>` 的状态标志新增 `HasCode`、`HasException`、`Stopped`；数据成员 `value`（`Uninitialized<T>`）与 `error`（`exception_ptr`）换成一个联合体 `storage`。

- [ ] **Step 1: 写测试**

修改 `tests/completion_components_tests.cpp`、`tests/allocation_tests.cpp`：

````diff
diff --git a/tests/allocation_tests.cpp b/tests/allocation_tests.cpp
index d96d501..1b17b6a 100644
--- a/tests/allocation_tests.cpp
+++ b/tests/allocation_tests.cpp
@@ -192,23 +192,26 @@ void whenAllInsideATaskAllocatesChildFramesStopStateAndAwaiterSlot() {
     CHECK_ALLOCATIONS(5, CHECK(co2::syncWait(awaitsWhenAll()) == 3));
 }
 
-// CallbackAwaitable<T> 的布局：MoveOnlyFunction（4 指针）+ 句柄 + T + 原子状态字 +
-// exception_ptr，四个标志共用一个状态字，结果不带自己的 engaged 标志。int 与 void 的
-// awaiter 在 exception_ptr 为 1 指针（libstdc++/libc++）或 2 指针（MSVC）时都放得进
-// 8 指针的内联 awaiter 槽；超过 4 字节的 T 在 MSVC 上回落到堆，见下方的 largeResult。
-template <class T>
-using CallbackAwaiterSize =
-    std::integral_constant<std::size_t, 5U * sizeof(void*) + sizeof(T) +
-                                            sizeof(std::atomic<unsigned>) +
-                                            sizeof(std::exception_ptr)>;
-static_assert(sizeof(co2::CallbackAwaitable<int>) == CallbackAwaiterSize<int>::value,
-              "CallbackAwaitable<int> must carry no padding beyond its five words");
-static_assert(sizeof(co2::CallbackAwaitable<void>) <= CallbackAwaiterSize<int>::value,
-              "CallbackAwaitable<void> must be no larger than the int one");
+// CallbackAwaitable<T> 的布局：MoveOnlyFunction（4 指针）+ 句柄 + 结果 + 原子状态字。
+// 结果的三种形态——值、错误码、捕获的异常——共用一块存储，哪一种活着由状态字里的标志
+// 说明；这块存储至少有一个 std::error_code 那么大（2 个指针）。所以结果不超过 2 个指针
+// 宽的 awaiter 都正好是 8 个指针，放得进内联 awaiter 槽，exception_ptr 是 1 个指针
+// （libstdc++ / libc++）还是 2 个（MSVC）都一样。
+struct TwoWords {
+    void const* words[2];
+};
+static_assert(sizeof(co2::CallbackAwaitable<int>) == 8U * sizeof(void*),
+              "CallbackAwaitable<int> must be exactly eight words");
+static_assert(sizeof(co2::CallbackAwaitable<void>) == 8U * sizeof(void*),
+              "CallbackAwaitable<void> must be exactly eight words");
+static_assert(sizeof(co2::CallbackAwaitable<TwoWords>) == 8U * sizeof(void*),
+              "a two-word result must share its storage with the error");
 static_assert(co2::detail::AwaitSlot<>::isInline<co2::CallbackAwaitable<int>>(),
               "CallbackAwaitable<int> must stay within the inline awaiter slot");
 static_assert(co2::detail::AwaitSlot<>::isInline<co2::CallbackAwaitable<void>>(),
               "CallbackAwaitable<void> must stay within the inline awaiter slot");
+static_assert(co2::detail::AwaitSlot<>::isInline<co2::CallbackAwaitable<TwoWords>>(),
+              "a two-word result must stay within the inline awaiter slot");
 
 auto viaCallback(int v) CO2_BEG(co2::Task<int>, (v), int got{};) {
     // 闭包只捕获 this：落在 MoveOnlyFunction 的内联缓冲里，不分配。
diff --git a/tests/completion_components_tests.cpp b/tests/completion_components_tests.cpp
index 534ce68..9a2e66c 100644
--- a/tests/completion_components_tests.cpp
+++ b/tests/completion_components_tests.cpp
@@ -2,6 +2,7 @@
 // 机制本身——恢复点、promise 的两个入口、Task——在 completion_tests.cpp 里测。
 
 #include <cstdlib>
+#include <exception>
 #include <iostream>
 #include <system_error>
 #include <tuple>
@@ -10,6 +11,7 @@
 
 #include "co2/as_result.hpp"
 #include "co2/async_generator.hpp"
+#include "co2/callback.hpp"
 #include "co2/coroutine.hpp"
 #include "co2/env.hpp"
 #include "co2/just.hpp"
@@ -321,6 +323,89 @@ void theFailureIsDeliveredOnceAndThenTheStreamIsOver() {
     CHECK(not hasAfterwards);
 }
 
+// ---------------------------------------------------------------------------
+// fromCallback：续体的四个出口——值、错误码、异常、已停止
+
+enum class Exit { value, code, exception, stopped };
+
+auto completesWith(Exit how, bool& reached)
+    CO2_BEG(co2::Task<int>, (how, reached), int got{};) {
+    CO2_AWAIT_AS_SET(got, co2::CallbackAwaitable<int>,
+                     co2::fromCallback<int>([this](co2::Continuation<int> done) {
+                         if (how == Exit::value) done(5);
+                         if (how == Exit::code) done.setError(sample::Errc::gone);
+                         if (how == Exit::exception)
+                             done.setException(std::make_exception_ptr(ExpectedError{}));
+                         if (how == Exit::stopped) done.setStopped();
+                     }));
+    reached = true;
+    CO2_RETURN(got);
+}
+CO2_END
+
+void theContinuationHasFourExits() {
+    auto reached = false;
+    CHECK(co2::syncWait(completesWith(Exit::value, reached)) == 5);
+    CHECK(reached);
+
+    reached = false;
+    auto const code = co2::syncWait(co2::asResult(completesWith(Exit::code, reached)));
+    CHECK(code->hasError());
+    CHECK(code->error().exception() == nullptr); // 按值传过来的错误码
+    CHECK(code->error().code() == sample::Errc::gone);
+    CHECK(not reached);
+
+    auto const exception =
+        co2::syncWait(co2::asResult(completesWith(Exit::exception, reached)));
+    CHECK(exception->hasError());
+    CHECK(exception->error().exception() != nullptr);
+    CHECK(not reached);
+
+    CHECK(co2::syncWait(co2::asResult(completesWith(Exit::stopped, reached)))
+              ->isStopped());
+    CHECK(not reached);
+}
+
+auto waitsForTheApi(co2::Continuation<int>& stored, bool& reached)
+    CO2_BEG(co2::Task<int>, (stored, reached), int got{};) {
+    CO2_AWAIT_AS_SET(
+        got, co2::CallbackAwaitable<int>,
+        co2::fromCallback<int>([this](co2::Continuation<int> done) { stored = done; }));
+    reached = true;
+    CO2_RETURN(got);
+}
+CO2_END
+
+// API 的取消回调里这样报告"已取消"：等待它的协程以已停止结束。
+void anApiThatFinishesLaterCanReportStopped() {
+    co2::ManualExecutor executor;
+    auto stored = co2::Continuation<int>{};
+    auto reached = false;
+    auto handle = co2::spawn(executor, waitsForTheApi(stored, reached));
+    CHECK(executor.run() == 1U);
+    CHECK(stored);
+    CHECK(not handle.isReady());
+
+    stored.setStopped();
+    CHECK(handle.isReady());
+    CHECK(not handle.join().has_value());
+    CHECK(not reached);
+}
+
+// 不认识 await_completion() 的调用方直接调用 await_resume()：错误码以异常的形式出现。
+void anErrorCodeFromACallbackRaisesForACallerThatSkipsAwaitCompletion() {
+    auto awaitable = co2::fromCallback<int>(
+        [](co2::Continuation<int> done) { done.setError(sample::Errc::gone); });
+    CHECK(not awaitable.await_suspend(co2::noop_coroutine())); // 已经完成：不挂起
+    auto sawCode = false;
+    try {
+        awaitable.await_resume();
+    } catch (std::system_error const& exception) {
+        sawCode = exception.code() == sample::Errc::gone;
+    }
+    CHECK(sawCode);
+}
+
 } // namespace
 
 int main() {
@@ -334,5 +419,8 @@ int main() {
     whenAllOfAVectorFollowsTheSameRules();
     aFailingProducerEndsItsConsumerTheSameWay();
     theFailureIsDeliveredOnceAndThenTheStreamIsOver();
+    theContinuationHasFourExits();
+    anApiThatFinishesLaterCanReportStopped();
+    anErrorCodeFromACallbackRaisesForACallerThatSkipsAwaitCompletion();
     return 0;
 }
````

- [ ] **Step 2: 构建，确认失败**

Expected: 构建失败。`tests/allocation_tests.cpp` 报 `static assertion failed: CallbackAwaitable<int> must be exactly eight words`（现在是 7 个）与 `a two-word result must stay within the inline awaiter slot`（现在是 9 个，落在堆上）；`tests/completion_components_tests.cpp` 报 `'struct co2::Continuation<int>' has no member named 'setError'`。

- [ ] **Step 3: 实现**

修改 `include/co2/callback.hpp`：

````diff
diff --git a/include/co2/callback.hpp b/include/co2/callback.hpp
index c15f953..9e52cfd 100644
--- a/include/co2/callback.hpp
+++ b/include/co2/callback.hpp
@@ -2,14 +2,18 @@
 
 #include <atomic>
 #include <exception>
+#include <new>
+#include <system_error>
 #include <type_traits>
 #include <utility>
 
+#include "co2/completion.hpp"
 #include "co2/contract.hpp"
 #include "co2/coroutine_handle.hpp"
 #include "co2/detail/move_only_function.hpp"
 #include "co2/detail/uninitialized.hpp"
 #include "co2/env.hpp"
+#include "co2/error.hpp"
 #include "co2/stop_token.hpp"
 
 // co2：把回调式异步 API 变成 awaiter。
@@ -24,9 +28,17 @@
 //   }
 //   CO2_END
 //
-// initiate(continuation[, stop_token]) 发起操作；操作完成时 API 调用
-// continuation(args...) （就地构造 T）或
-// continuation.setException(eptr)，**恰好一次**。
+// initiate(continuation[, stop_token]) 发起操作；操作完成时 API 调用续体的四个出口
+// 之一，**恰好一次**：
+//
+//   continuation(args...)             有值：就地构造 T
+//   continuation.setError(code)       出错：一个 std::error_code
+//   continuation.setException(eptr)   出错：一个捕获的异常
+//   continuation.setStopped()         已停止
+//
+// 后三种经 await_completion() 按值交给等待它的协程，那个协程就此结束（见
+// completion-channels.md）；不认识这个成员的调用方直接调用 await_resume()，错误在那里
+// 抛出。
 //
 // 完成协议是两方 fetch_or、第二个到达者负责恢复：续体在 initiate 返回之前就到了
 // （同步完成或另一线程更快）→ await_suspend 返回 false，协程不挂起、内联继续，不长栈；
@@ -35,14 +47,16 @@
 // 容量时除外）。initiate 以 detail::MoveOnlyFunction 保存：闭包可以捕获 unique_ptr 之类
 // 的 move-only 对象，只要求它可移动。
 //
-// 布局：Armed / Completed / Claimed / HasValue 四个标志共用一个原子字，结果放在无标志的
-// Uninitialized<T> 里。CallbackAwaitable<T> = MoveOnlyFunction（4 指针）+ 句柄 + T +
-// 状态字 + exception_ptr；T 不超过 4 字节时，即使 exception_ptr 是 2 个指针（MSVC），
-// 也放得进 8 指针的内联 awaiter 槽。
+// 布局：全部标志共用一个原子字。结果的三种形态——值、错误码、捕获的异常——互斥，共用
+// 一块没有标志的存储，哪一种活着由原子字里的标志说明。CallbackAwaitable<T> =
+// MoveOnlyFunction（4 指针）+ 句柄 + 结果 + 状态字；结果那一块至少是一个
+// std::error_code（2 个指针），所以 T 不超过 2 个指针宽时整个 awaiter 正好 8 个指针，
+// 放得进内联 awaiter 槽。错误拆成两种形态分别存，而不是存一个 co2::Error：在
+// exception_ptr 是 2 个指针的平台上（MSVC），Error 有 3 个指针宽，会把 awaiter 顶出槽。
 //
 // 取消：initiate 可以接收当前协程的 stop_token，用 stop_callback 接到 API 自己的
-// cancel， API
-// 随后以"已取消"回调走正常完成路径。因此不需要为迟到的回调保活任何共享状态。
+// cancel，API 随后以"已取消"回调走正常完成路径，在那个回调里调用
+// continuation.setStopped()。因此不需要为迟到的回调保活任何共享状态。
 //
 // 契约：续体调用恰好一次；initiate 抛出之后 API 不得再调用续体；awaiter 在 initiate
 // 之后不再移动（核心保证）。协程在续体被调用的线程上恢复（标准语义），要换线程用
@@ -51,8 +65,7 @@
 // 关于"恰好一次"：第一次调用一旦完成，协程就可能（在本线程内联地或在另一线程上）恢复
 // 并销毁这个 awaiter，续体里的裸指针随之悬空。因此第二次调用只有在 awaiter 仍然存活时
 // 才会被诊断为契约违规（contract violation）；一般情况下它是未定义行为，库无法保证
-// 报告。就地构造 T 抛出时，异常会被存进异常通道并照常完成等待——await_resume 重抛它，
-// 协程不会因此挂住。
+// 报告。就地构造 T 抛出时，异常被记为错误并照常完成等待，协程不会因此挂住。
 
 namespace co2 {
 
@@ -62,27 +75,42 @@ template <class T> struct CallbackAwaitable;
 template <class T> struct Continuation {
     Continuation() noexcept = default;
 
-    // 就地构造结果并完成等待。T 为 void 时不带参数。T 的构造抛出时改走异常通道：
-    // 等待仍然完成，否则协程会永远挂在这个 await 上（续体已被 claim，不能再调）。
+    // 就地构造结果并完成等待。T 为 void 时不带参数。T 的构造抛出时记为错误：等待仍然
+    // 完成，否则协程会永远挂在这个 await 上（续体已被 claim，不能再调）。
     template <class... Args> void operator()(Args&&... args) const {
         CO2_CONTRACT_CHECK(operation != nullptr);
         operation->claim();
         unsigned outcome = CallbackAwaitable<T>::HasValue;
         try {
-            operation->value.construct(std::forward<Args>(args)...);
+            operation->storage.value.construct(std::forward<Args>(args)...);
         } catch (...) {
-            operation->error = std::current_exception();
-            outcome = 0U;
+            operation->storeException(std::current_exception());
+            outcome = CallbackAwaitable<T>::HasException;
         }
         operation->complete(outcome);
     }
 
-    // 以异常完成等待：await_resume 重抛它。
+    // 以错误码完成等待。前置条件：code 表示失败（非零）。
+    void setError(std::error_code const code) const {
+        CO2_CONTRACT_CHECK(operation != nullptr && static_cast<bool>(code));
+        operation->claim();
+        ::new (static_cast<void*>(&operation->storage.code)) std::error_code{code};
+        operation->complete(CallbackAwaitable<T>::HasCode);
+    }
+
+    // 以异常完成等待。前置条件：error 非空。
     void setException(std::exception_ptr error) const {
         CO2_CONTRACT_CHECK(operation != nullptr && error != nullptr);
         operation->claim();
-        operation->error = std::move(error);
-        operation->complete(0U);
+        operation->storeException(std::move(error));
+        operation->complete(CallbackAwaitable<T>::HasException);
+    }
+
+    // 以已停止完成等待：操作被取消了，没有结果。
+    void setStopped() const {
+        CO2_CONTRACT_CHECK(operation != nullptr);
+        operation->claim();
+        operation->complete(CallbackAwaitable<T>::Stopped);
     }
 
     explicit operator bool() const noexcept { return operation != nullptr; }
@@ -115,9 +143,12 @@ template <class T> struct CallbackAwaitable {
     CallbackAwaitable& operator=(CallbackAwaitable const&) = delete;
     CallbackAwaitable& operator=(CallbackAwaitable&&) = delete;
 
-    // 结果一般在 await_resume 里被取走；留在这里的只有 take 时移动构造抛出的那一份。
+    // 值一般在 await_resume 里被取走；留在这里的只有 take 时移动构造抛出的那一份。
+    // error_code 平凡析构；捕获的异常被取走之后留下的 exception_ptr 仍要析构。
     ~CallbackAwaitable() {
-        if (hasValue()) value.destroy();
+        auto const flags = state.load(std::memory_order_relaxed);
+        if ((flags & HasValue) != 0U) storage.value.destroy();
+        if ((flags & HasException) != 0U) storage.exception.~exception_ptr();
     }
 
     bool await_ready() const noexcept { return false; }
@@ -132,19 +163,53 @@ template <class T> struct CallbackAwaitable {
         return (state.fetch_or(Armed, std::memory_order_acq_rel) & Completed) == 0U;
     }
 
+    // 不认识 await_completion() 的调用方直接走到这里：错误在这里抛出；已停止没有值
+    // 可给，是契约违规。
     T await_resume() {
-        if (error) std::rethrow_exception(error);
-        CO2_CONTRACT_CHECK(hasValue());
+        auto const flags = state.load(std::memory_order_relaxed);
+        if ((flags & (HasCode | HasException)) != 0U) takeError(flags).raise();
+        CO2_CONTRACT_CHECK((flags & HasValue) != 0U);
         return take(std::is_void<T>{});
     }
 
+    // 这次等待是怎么结束的。出错与已停止由恢复点交给等待它的协程。
+    Completion await_completion() noexcept {
+        auto const flags = state.load(std::memory_order_relaxed);
+        if ((flags & Stopped) != 0U) return Completion::stopped();
+        if ((flags & (HasCode | HasException)) != 0U)
+            return Completion::error(takeError(flags));
+        return Completion::value();
+    }
+
   private:
     // 一个原子字承载全部状态：
-    //   Armed      await_suspend 已收尾，协程挂起中（由等待方置位）
-    //   Completed  续体已经到达（由完成方置位）
-    //   Claimed    续体已被调用过——"恰好一次"的诊断依据
-    //   HasValue   value 里有一个已构造的 T（与 Completed 一起、以 release 发布）
-    enum : unsigned { Armed = 1U, Completed = 2U, Claimed = 4U, HasValue = 8U };
+    //   Armed         await_suspend 已收尾，协程挂起中（由等待方置位）
+    //   Completed     续体已经到达（由完成方置位）
+    //   Claimed       续体已被调用过——"恰好一次"的诊断依据
+    //   HasValue      storage.value 里有一个已构造的 T
+    //   HasCode       storage.code 里有一个错误码
+    //   HasException  storage.exception 里有一个捕获的异常
+    //   Stopped       操作以已停止完成，storage 里什么也没有
+    // 后四个是结果，互斥，与 Completed 一起、以 release 发布。
+    enum : unsigned {
+        Armed = 1U,
+        Completed = 2U,
+        Claimed = 4U,
+        HasValue = 8U,
+        HasCode = 16U,
+        HasException = 32U,
+        Stopped = 64U
+    };
+
+    // 结果的三种形态共用的存储，自己不带标记。
+    union Storage {
+        Storage() noexcept {}
+        ~Storage() {}
+
+        detail::Uninitialized<T> value;
+        std::error_code code;
+        std::exception_ptr exception;
+    };
 
     // 续体只能调用一次。
     void claim() noexcept {
@@ -153,21 +218,17 @@ template <class T> struct CallbackAwaitable {
     }
 
     // 第二个到达者负责恢复：await_suspend 已经收尾（Armed）则协程正挂着，在这里恢复它。
-    // outcome 是 HasValue 或 0，与 Completed 一次写入，结果的发布和完成信号不可分。
+    // outcome 是四个结果标志之一，与 Completed 一次写入，结果的发布和完成信号不可分。
     void complete(unsigned const outcome) {
         if ((state.fetch_or(Completed | outcome, std::memory_order_acq_rel) & Armed) !=
             0U)
             waiter.resume();
     }
 
-    bool hasValue() const noexcept {
-        return (state.load(std::memory_order_relaxed) & HasValue) != 0U;
-    }
-
     // 圆括号是有意的：泛型 T 上花括号可能选中 initializer_list 构造函数。移动构造抛出
     // 时对象保留、HasValue 不清，析构函数仍会清理。
     T take(std::false_type) {
-        T result(std::move(value.get()));
+        T result(std::move(storage.value.get()));
         dropValue();
         return result;
     }
@@ -175,15 +236,25 @@ template <class T> struct CallbackAwaitable {
     void take(std::true_type) noexcept { dropValue(); }
 
     void dropValue() noexcept {
-        value.destroy();
+        storage.value.destroy();
         state.fetch_and(~HasValue, std::memory_order_relaxed);
     }
 
+    void storeException(std::exception_ptr error) noexcept {
+        ::new (static_cast<void*>(&storage.exception))
+            std::exception_ptr{std::move(error)};
+    }
+
+    // 把存着的错误交出去，只可一次。捕获的异常被移走，留下的空壳由析构函数清理。
+    Error takeError(unsigned const flags) noexcept {
+        if ((flags & HasException) != 0U) return Error{std::move(storage.exception)};
+        return Error{storage.code};
+    }
+
     Initiate initiate;
     coroutine_handle<> waiter;
-    detail::Uninitialized<T> value;
+    Storage storage;
     std::atomic<unsigned> state{0U};
-    std::exception_ptr error;
 
     friend struct Continuation<T>;
 };
````

- [ ] **Step 4: 构建并运行全部测试，确认通过**

Expected: `100% tests passed out of 30`。`co2-callback-tests` 与 `co2-allocation-tests` 在其中：原有的行为与分配次数没有变。

- [ ] **Step 5: 提交**

```bash
git add include/co2/callback.hpp tests/completion_components_tests.cpp \
        tests/allocation_tests.cpp
git commit -F - <<'EOF'
fromCallback：续体新增 setError 与 setStopped

docs/design/completion-channels.md 第 4 节。续体现在有四个出口：值、错误码、异常、
已停止，后三种按值交给等待它的协程。API 的取消回调里调用 setStopped()，等待它的
协程就以已停止结束。

结果的三种形态（值、错误码、捕获的异常）共用一块存储，由状态字里的标志区分。没有
直接存一个 co2::Error：exception_ptr 是两个指针的平台上（MSVC）Error 有三个指针
宽，会把 awaiter 顶出 8 指针的内联槽。结果不超过两个指针宽的 awaiter 现在都正好
是 8 个指针：CallbackAwaitable<int> 从 56 字节变成 64 字节（仍在槽内，帧大小与
分配次数不变），两个指针宽的结果从 72 字节（落到堆上）变成 64 字节。
EOF
```

### Task 7: `just_stopped` 与 `stopIfRequested`

设计稿 2.2 节、第 10 节第 7 步。

**Files:**
- Modify: `tests/completion_components_tests.cpp`
- Modify: `include/co2/just.hpp`、`include/co2/env.hpp`、`include/co2/stop_token.hpp`

**Interfaces:**
- Consumes: `co2::detail::EnvStopToken`、`co2::detail::inheritStopToken`（已有）；测试里的 `awaitsBoth`（任务 4）。
- Produces:
  - `bool co2::detail::EnvStopToken::stopRequested() const noexcept`——没有令牌时为假。任务 9 也用它。
  - `template <class Promise> bool co2::detail::stopRequestedOf(coroutine_handle<Promise>) noexcept`。
  - `co2::detail::JustStopped co2::just_stopped() noexcept`：立刻就绪，`await_completion()` 返回已停止；`await_resume()` 是契约违规。
  - `co2::detail::StopIfRequested co2::stopIfRequested() noexcept`：取消点；`await_resume()` 什么也不做。

- [ ] **Step 1: 写测试**

修改 `tests/completion_components_tests.cpp`：

````diff
diff --git a/tests/completion_components_tests.cpp b/tests/completion_components_tests.cpp
index 9a2e66c..d1e450d 100644
--- a/tests/completion_components_tests.cpp
+++ b/tests/completion_components_tests.cpp
@@ -406,6 +406,71 @@ void anErrorCodeFromACallbackRaisesForACallerThatSkipsAwaitCompletion() {
     CHECK(sawCode);
 }
 
+// ---------------------------------------------------------------------------
+// 已停止的产生方：just_stopped 与 stopIfRequested
+
+auto stopsItself(bool& reached) CO2_BEG(co2::Task<int>, (reached)) {
+    CO2_AWAIT(co2::just_stopped());
+    reached = true;
+    CO2_RETURN(0);
+}
+CO2_END
+
+void justStoppedEndsTheTaskAsStopped() {
+    auto reached = false;
+    CHECK(not co2::syncWait(stopsItself(reached)).has_value());
+    CHECK(not reached);
+}
+
+auto passesTwoCancellationPoints(co2::stop_source& source, int& passed)
+    CO2_BEG(co2::Task<int>, (source, passed)) {
+    CO2_AWAIT(co2::stopIfRequested()); // 还没有请求：继续
+    ++passed;
+    source.request_stop();
+    CO2_AWAIT(co2::stopIfRequested()); // 有请求了：到此为止
+    ++passed;
+    CO2_RETURN(passed);
+}
+CO2_END
+
+void stopIfRequestedIsACancellationPoint() {
+    co2::stop_source source;
+    auto passed = 0;
+    auto const result =
+        co2::syncWait(passesTwoCancellationPoints(source, passed), source.get_token());
+    CHECK(not result.has_value());
+    CHECK(passed == 1);
+}
+
+// 这棵 Task 树的根没有拿到令牌：别处的停止请求与它无关，取消点什么也不做。
+void stopIfRequestedDoesNothingWithoutAToken() {
+    co2::stop_source unrelated;
+    auto passed = 0;
+    CHECK(co2::syncWait(passesTwoCancellationPoints(unrelated, passed)) == 2);
+}
+
+auto stopsWhenAsked() CO2_BEG(co2::Task<int>, ()) {
+    CO2_AWAIT(co2::stopIfRequested());
+    CO2_RETURN(1);
+}
+CO2_END
+
+// 父的停止请求经 whenAll 转发给每个 child；child 在取消点结束，whenAll 以已停止完成。
+void aParentsStopRequestStopsWhenAllThroughItsChildren() {
+    co2::stop_source source;
+    auto reached = false;
+    CHECK(co2::syncWait(awaitsBoth(stopsWhenAsked(), stopsWhenAsked(), reached),
+                        source.get_token()) == 2);
+    CHECK(reached);
+
+    CHECK(source.request_stop());
+    reached = false;
+    CHECK(not co2::syncWait(awaitsBoth(stopsWhenAsked(), stopsWhenAsked(), reached),
+                            source.get_token())
+                  .has_value());
+    CHECK(not reached);
+}
+
 } // namespace
 
 int main() {
@@ -422,5 +487,9 @@ int main() {
     theContinuationHasFourExits();
     anApiThatFinishesLaterCanReportStopped();
     anErrorCodeFromACallbackRaisesForACallerThatSkipsAwaitCompletion();
+    justStoppedEndsTheTaskAsStopped();
+    stopIfRequestedIsACancellationPoint();
+    stopIfRequestedDoesNothingWithoutAToken();
+    aParentsStopRequestStopsWhenAllThroughItsChildren();
     return 0;
 }
````

- [ ] **Step 2: 构建，确认失败**

Expected: 构建失败，`tests/completion_components_tests.cpp` 报 `'just_stopped' is not a member of 'co2'`。

- [ ] **Step 3: 实现**

修改 `include/co2/stop_token.hpp`、`include/co2/env.hpp`、`include/co2/just.hpp`：

````diff
diff --git a/include/co2/env.hpp b/include/co2/env.hpp
index 9b4eb47..162da3f 100644
--- a/include/co2/env.hpp
+++ b/include/co2/env.hpp
@@ -84,6 +84,15 @@ void inheritStopToken(EnvStopToken& into,
     inheritFromHandle(into, awaiting, std::is_void<Promise>{});
 }
 
+// 等待者的环境里有没有停止请求。等待者没有令牌（自定义 promise、类型擦除的句柄）时
+// 为假。取消点用它：stopIfRequested()。
+template <class Promise>
+bool stopRequestedOf(coroutine_handle<Promise> const awaiting) noexcept {
+    EnvStopToken token;
+    inheritStopToken(token, awaiting);
+    return token.stopRequested();
+}
+
 // co_await getStopToken() 的 awaiter：不挂起，返回 promise 持有的 token。
 struct StopTokenAwaiter {
     stop_token token;
diff --git a/include/co2/just.hpp b/include/co2/just.hpp
index 6349ccd..cdab490 100644
--- a/include/co2/just.hpp
+++ b/include/co2/just.hpp
@@ -4,21 +4,28 @@
 
 #include "co2/completion.hpp"
 #include "co2/config.hpp"
+#include "co2/contract.hpp"
 #include "co2/coroutine_handle.hpp"
+#include "co2/env.hpp"
 #include "co2/error.hpp"
 
-// co2：在协程体里直接产生"错误"这种结果。名字取自 P2300 与 P3552。
+// co2：在协程体里直接产生"错误"与"已停止"这两种结果。名字取自 P2300 与 P3552。
 //
 //   CO2_AWAIT(co2::just_error(MyErrc::retry_later));   // 当前协程以这个错误结束
 //   CO2_YIELD(co2::with_error(MyErrc::retry_later));   // 同上，P3552 的另一种拼写
+//   CO2_AWAIT(co2::just_stopped());                    // 当前协程以已停止结束
+//   CO2_AWAIT(co2::stopIfRequested());                 // 取消点：有停止请求才停
 //
-// 两者效果相同：其后的语句不执行，局部析构，协程以错误结束，等待它的协程收到同一个
-// 错误。just_error 是普通的 awaitable，在任何 promise 提供了 return_error() 的协程里都
-// 能用；with_error 是一个标记值，由 promise 的 yield_value 接住，只在提供了这个重载的
-// 协程里能写（Task）。
+// 结束的意思都一样：其后的语句不执行，局部析构，等待它的协程收到同样的结果。
 //
-// 参数是 co2::Error：std::error_code 与错误码枚举可以直接传；一个捕获的异常要显式写成
-// co2::Error{exceptionPointer}。
+// just_error 是普通的 awaitable，在任何 promise 提供了 return_error() 的协程里都能用；
+// with_error 是一个标记值，由 promise 的 yield_value 接住，只在提供了这个重载的协程里
+// 能写（Task、AsyncGenerator），两者效果相同。参数是 co2::Error：std::error_code 与
+// 错误码枚举可以直接传；一个捕获的异常要显式写成 co2::Error{exceptionPointer}。
+//
+// just_stopped 与 stopIfRequested 需要 promise 提供 return_stopped()。stopIfRequested
+// 看的是当前协程环境里的停止令牌（co2::getStopToken() 读到的那一个）：没有令牌的协程
+// 里它什么也不做。
 
 namespace co2 {
 namespace detail {
@@ -36,12 +43,53 @@ struct JustError {
     Completion await_completion() noexcept { return Completion::error(std::move(error)); }
 };
 
+// 立刻就绪、以已停止结束的 awaiter。
+struct JustStopped {
+    bool await_ready() const noexcept { return true; }
+    void await_suspend(coroutine_handle<>) const noexcept {}
+
+    // 只有不认识 await_completion() 的调用方才会调到这里：已停止没有值可给。
+    [[noreturn]] void await_resume() const noexcept {
+        CO2_CONTRACT_FAIL("just_stopped() awaited by a caller that does not ask "
+                          "await_completion()");
+    }
+
+    Completion await_completion() const noexcept { return Completion::stopped(); }
+};
+
+// 取消点。它不挂起：await_suspend 返回 false，协程立刻继续；借这次调用拿到带类型的
+// 句柄，读等待者环境里的停止令牌。
+struct StopIfRequested {
+    bool requested{};
+
+    bool await_ready() const noexcept { return false; }
+
+    template <class Promise>
+    bool await_suspend(coroutine_handle<Promise> const awaiting) noexcept {
+        requested = stopRequestedOf(awaiting);
+        return false;
+    }
+
+    // 不认识 await_completion() 的调用方不被取消，照常继续。
+    void await_resume() const noexcept {}
+
+    Completion await_completion() const noexcept {
+        return requested ? Completion::stopped() : Completion::value();
+    }
+};
+
 } // namespace detail
 
 inline detail::JustError just_error(Error error) noexcept {
     return detail::JustError{std::move(error)};
 }
 
+inline detail::JustStopped just_stopped() noexcept { return detail::JustStopped{}; }
+
+inline detail::StopIfRequested stopIfRequested() noexcept {
+    return detail::StopIfRequested{};
+}
+
 struct WithError {
     Error error;
 };
diff --git a/include/co2/stop_token.hpp b/include/co2/stop_token.hpp
index 5f22b8e..9f74a70 100644
--- a/include/co2/stop_token.hpp
+++ b/include/co2/stop_token.hpp
@@ -358,6 +358,12 @@ struct EnvStopToken {
     // 一个独立的、持有引用的 token：交给用户代码、stop_callback 或另起的根。
     stop_token get() const noexcept { return stop_token{state()}; }
 
+    // 有没有停止请求。没有令牌时为假。只读一次原子量，不碰引用计数。
+    bool stopRequested() const noexcept {
+        auto* const shared = state();
+        return shared != nullptr && shared->stopRequested();
+    }
+
     void reset() noexcept {
         if ((bits & ownedBit) != 0U) state()->release();
         bits = 0U;
````

- [ ] **Step 4: 构建并运行全部测试，确认通过**

Expected: `100% tests passed out of 30`。

- [ ] **Step 5: 提交**

```bash
git add include/co2/just.hpp include/co2/env.hpp include/co2/stop_token.hpp \
        tests/completion_components_tests.cpp
git commit -F - <<'EOF'
新增 just_stopped 与 stopIfRequested

docs/design/completion-channels.md 第 2.2 节、第 10 节第 7 步。

    CO2_AWAIT(co2::just_stopped());      // 当前协程以已停止结束
    CO2_AWAIT(co2::stopIfRequested());   // 取消点：有停止请求才停，否则继续

stopIfRequested 看的是当前协程环境里的停止令牌，借用而不拷贝；没有令牌的协程里它
什么也不做。不认识 await_completion() 的调用方不会被它取消。
EOF
```

### Task 8: `unstoppable`

设计稿 2.3、3.4 节。

**Files:**
- Create: `include/co2/unstoppable.hpp`
- Modify: `include/co2/co2.hpp`
- Modify: `tests/completion_components_tests.cpp`

**Interfaces:**
- Consumes: `co2::detail::AwaiterOf`、`co2::detail::getAwaiter`（已有）；`co2::stopIfRequested`（任务 7）。
- Produces:
  - `template <class Awaitable> struct co2::Unstoppable { Awaitable awaitable; ... operator_co_await() &&; }`；`template <class Awaitable> Unstoppable<Awaitable> co2::unstoppable(Awaitable&&)`。等待它得到内层 `await_resume()` 的结果。
  - `template <class Inner> struct co2::detail::UnstoppableAwaiter`：把类型擦除的 `coroutine_handle<>` 交给内层的 `await_suspend`；内层有 `await_completion()` 时原样转发。

- [ ] **Step 1: 写测试**

修改 `tests/completion_components_tests.cpp`：

````diff
diff --git a/tests/completion_components_tests.cpp b/tests/completion_components_tests.cpp
index d1e450d..6c40a6c 100644
--- a/tests/completion_components_tests.cpp
+++ b/tests/completion_components_tests.cpp
@@ -23,6 +23,7 @@
 #include "co2/stop_token.hpp"
 #include "co2/sync_wait.hpp"
 #include "co2/task.hpp"
+#include "co2/unstoppable.hpp"
 #include "co2/when_all.hpp"
 #include "completion_support.hpp"
 #include "sample_errc.hpp"
@@ -471,6 +472,81 @@ void aParentsStopRequestStopsWhenAllThroughItsChildren() {
     CHECK(not reached);
 }
 
+// ---------------------------------------------------------------------------
+// unstoppable：对被包住的操作隐藏停止请求
+
+auto hasAStopToken() CO2_BEG(co2::Task<bool>, (), co2::stop_token token;) {
+    CO2_AWAIT_SET(token, co2::getStopToken());
+    CO2_RETURN(token.stop_possible());
+}
+CO2_END
+
+auto awaitsWithAndWithoutTheToken(bool& plain, bool& shielded)
+    CO2_BEG(co2::Task<int>, (plain, shielded), co2::Task<bool> kept;) {
+    CO2_AWAIT_SET(plain, hasAStopToken());
+    CO2_AWAIT_SET(shielded, co2::unstoppable(hasAStopToken()));
+    if (shielded) CO2_RETURN(-1);
+    kept = hasAStopToken();
+    CO2_AWAIT_SET(shielded, co2::unstoppable(kept)); // 左值同样可以
+    CO2_RETURN(0);
+}
+CO2_END
+
+// 被包住的子 Task 的环境里没有停止令牌。
+void anUnstoppableChildSeesNoStopToken() {
+    co2::stop_source source;
+    auto plain = false;
+    auto shielded = true;
+    CHECK(co2::syncWait(awaitsWithAndWithoutTheToken(plain, shielded),
+                        source.get_token()) == 0);
+    CHECK(plain);
+    CHECK(not shielded);
+}
+
+auto passesAShieldedCancellationPoint(int& passed) CO2_BEG(co2::Task<int>, (passed)) {
+    CO2_AWAIT(co2::unstoppable(co2::stopIfRequested())); // 看不到请求：继续
+    ++passed;
+    CO2_AWAIT(co2::unstoppable(co2::suspend_never{})); // 标准写法的 awaiter 也能包
+    ++passed;
+    CO2_AWAIT(co2::stopIfRequested()); // 没有包住：到此为止
+    ++passed;
+    CO2_RETURN(passed);
+}
+CO2_END
+
+void unstoppableHidesTheRequestFromACancellationPoint() {
+    co2::stop_source source;
+    CHECK(source.request_stop());
+    auto passed = 0;
+    CHECK(not co2::syncWait(passesAShieldedCancellationPoint(passed), source.get_token())
+                  .has_value());
+    CHECK(passed == 2);
+}
+
+auto shields(Leaf leaf, bool& reached)
+    CO2_BEG(co2::Task<int>, (leaf, reached), int value{}; int resumed{};) {
+    CO2_AWAIT_SET(value, co2::unstoppable(chain(0, leaf, resumed, nullptr)));
+    reached = true;
+    CO2_RETURN(value);
+}
+CO2_END
+
+// 它只隐藏外面的停止请求：被包住的操作自己出错或自己停止，照样传出来。
+void unstoppableStillForwardsHowTheOperationEnded() {
+    auto reached = false;
+    CHECK(co2::syncWait(shields(Leaf::returns, reached)) == 1);
+    CHECK(reached);
+
+    reached = false;
+    auto const failure = co2::syncWait(co2::asResult(shields(Leaf::fails, reached)));
+    CHECK(failure->hasError());
+    CHECK(failure->error().exception() == nullptr);
+    CHECK(failure->error().code() == sample::Errc::gone);
+
+    CHECK(co2::syncWait(co2::asResult(shields(Leaf::stops, reached)))->isStopped());
+    CHECK(not reached);
+}
+
 } // namespace
 
 int main() {
@@ -491,5 +567,8 @@ int main() {
     stopIfRequestedIsACancellationPoint();
     stopIfRequestedDoesNothingWithoutAToken();
     aParentsStopRequestStopsWhenAllThroughItsChildren();
+    anUnstoppableChildSeesNoStopToken();
+    unstoppableHidesTheRequestFromACancellationPoint();
+    unstoppableStillForwardsHowTheOperationEnded();
     return 0;
 }
````

- [ ] **Step 2: 构建，确认失败**

Expected: 构建失败，`tests/completion_components_tests.cpp` 报 `fatal error`，说找不到 `co2/unstoppable.hpp`。

- [ ] **Step 3: 实现**

新建 `include/co2/unstoppable.hpp`：

````cpp
#pragma once

#include <utility>

#include "co2/config.hpp"
#include "co2/coroutine_handle.hpp"
#include "co2/detail/awaitable.hpp"

// co2::unstoppable(awaitable)：对被包住的操作隐藏停止请求
// （std::execution::unstoppable 在 co2 里的拼写）。
//
//   CO2_AWAIT(co2::unstoppable(co2::scheduleOn(pool)));   // 停止请求之后仍然换线程
//   CO2_AWAIT(co2::unstoppable(flush(file)));             // flush 和它等待的一切都不被取消
//
// 用在收尾阶段：已经收到停止请求，但还有几步必须做完。它只隐藏外面的停止请求——被包住
// 的操作自己出错、或者自己以已停止结束，照样传给等待它的协程。
//
// 做法：把等待者的句柄类型擦除之后再交给内层的 await_suspend。环境是经带类型的句柄
// 查询的，内层于是拿不到等待者的停止令牌；被包住的是 Task 时，它和它的子 Task 的环境里
// 也就没有令牌。两点后果：
//   - 内层的 await_suspend 必须接受 coroutine_handle<>。
//   - 被隐藏的是整个环境，不只是停止令牌。std::execution::unstoppable 保留环境的其余
//     部分；今天环境里只有停止令牌，两者等价。
//
// 与 asResult 一样，unstoppable(x) 要直接写在等待它的地方。x 是右值时被搬进等待者的
// 帧；x 是左值时只记它的引用。

namespace co2 {
namespace detail {

template <class Inner> struct UnstoppableAwaiter {
    Inner inner;

    auto await_ready() -> decltype(std::declval<Inner&>().await_ready()) {
        return inner.await_ready();
    }

    template <class Promise>
    auto await_suspend(coroutine_handle<Promise> const awaiting)
        -> decltype(std::declval<Inner&>().await_suspend(coroutine_handle<>{})) {
        return inner.await_suspend(coroutine_handle<>{awaiting});
    }

    auto await_resume() -> decltype(std::declval<Inner&>().await_resume()) {
        return inner.await_resume();
    }

    // 内层有这个成员时才有：被包住的操作怎么结束的，原样转发。
    template <class I = Inner>
    auto await_completion() -> decltype(std::declval<I&>().await_completion()) {
        return inner.await_completion();
    }
};

} // namespace detail

// unstoppable(x) 的返回类型。Awaitable 对右值是值类型（拥有 x），对左值是引用类型。
template <class Awaitable> struct Unstoppable {
    Awaitable awaitable;

    detail::UnstoppableAwaiter<detail::AwaiterOf<Awaitable>> operator_co_await() && {
        return detail::UnstoppableAwaiter<detail::AwaiterOf<Awaitable>>{
            detail::AwaiterOf<Awaitable>{
                detail::getAwaiter(std::forward<Awaitable>(awaitable))}};
    }
};

template <class Awaitable> Unstoppable<Awaitable> unstoppable(Awaitable&& awaitable) {
    return Unstoppable<Awaitable>{std::forward<Awaitable>(awaitable)};
}

} // namespace co2
````

修改 `include/co2/co2.hpp`：

````diff
diff --git a/include/co2/co2.hpp b/include/co2/co2.hpp
index 51cc4e3..c8c32fd 100644
--- a/include/co2/co2.hpp
+++ b/include/co2/co2.hpp
@@ -21,4 +21,5 @@
 #include "co2/sync_wait.hpp"
 #include "co2/task.hpp"
 #include "co2/thread_pool.hpp"
+#include "co2/unstoppable.hpp"
 #include "co2/when_all.hpp"
````

- [ ] **Step 4: 构建并运行全部测试，确认通过**

Expected: `100% tests passed out of 30`。

- [ ] **Step 5: 提交**

```bash
git add include/co2/unstoppable.hpp include/co2/co2.hpp tests/completion_components_tests.cpp
git commit -F - <<'EOF'
新增 unstoppable

docs/design/completion-channels.md 第 2.3、3.4 节，std::execution::unstoppable 在
co2 里的拼写：对被包住的操作隐藏停止请求，用在收尾阶段。

    CO2_AWAIT(co2::unstoppable(co2::scheduleOn(pool)));

做法是把类型擦除的句柄交给内层的 await_suspend，内层于是查不到等待者的环境。它只
隐藏外面的停止请求：被包住的操作自己出错或自己停止，照样传出来。已知限制：被隐藏的
是整个环境，不只是停止令牌；今天环境里只有停止令牌，两者等价。
EOF
```

### Task 9: `scheduleOn` 成为取消点

设计稿第 4 节、第 6 节第 2、7 条、第 10 节第 8 步。行为变化：协程在目标 scheduler 上恢复时，它的停止令牌上若已有请求，就以已停止结束。

**Files:**
- Modify: `tests/completion_components_tests.cpp`、`tests/spawn_tests.cpp`
- Modify: `include/co2/scheduler.hpp`、`include/co2/stop_token.hpp`

**Interfaces:**
- Consumes: `EnvStopToken::stopRequested()`（任务 7）；`co2::detail::inheritStopToken`（已有）；`co2::unstoppable`（任务 8）。
- Produces:
  - `co2::ScheduleOn` 多一个数据成员 `detail::EnvStopToken token`；`await_suspend` 变成按等待者的 promise 类型的模板 `template <class Promise> void await_suspend(coroutine_handle<Promise>) noexcept`；新增 `Completion await_completion() const noexcept`。`await_resume()` 仍然什么也不做。
  - `co2::detail::EnvStopToken` 的移动构造函数（`ScheduleOn` 靠它被搬进帧里）。

**注意红色阶段的做法。** `tests/spawn_tests.cpp` 的新测试在没有实现时会永远等下去（一个不停 hop 的 Task 收不到取消），而沙箱里杀不掉卡住的进程。所以这个任务分两段：先只加 `tests/completion_components_tests.cpp` 里的测试并单独运行它；`tests/spawn_tests.cpp` 等实现之后再改。

- [ ] **Step 1: 写测试（只有 `completion_components_tests.cpp`）**

修改 `tests/completion_components_tests.cpp`：

````diff
diff --git a/tests/completion_components_tests.cpp b/tests/completion_components_tests.cpp
index 6c40a6c..84bba2e 100644
--- a/tests/completion_components_tests.cpp
+++ b/tests/completion_components_tests.cpp
@@ -547,6 +547,84 @@ void unstoppableStillForwardsHowTheOperationEnded() {
     CHECK(not reached);
 }
 
+// ---------------------------------------------------------------------------
+// scheduleOn 是取消点：hop 之后，等待者的停止令牌上有请求就以已停止结束
+
+auto hopsForever(co2::Scheduler& scheduler, int& hops)
+    CO2_BEG(co2::Task<int>, (scheduler, hops)) {
+    for (;;) {
+        CO2_AWAIT(co2::scheduleOn(scheduler));
+        ++hops;
+    }
+}
+CO2_END
+
+void aHoppingTaskStopsAtItsNextHop() {
+    co2::ManualExecutor executor;
+    auto hops = 0;
+    auto handle = co2::spawn(executor, hopsForever(executor, hops));
+    CHECK(executor.runOne()); // 启动，第一次 hop 排进队列
+    CHECK(executor.runOne()); // hop 回来：没有停止请求，继续
+    CHECK(hops == 1);
+    CHECK(not handle.isReady());
+
+    CHECK(handle.requestStop());
+    CHECK(executor.runOne()); // 检查发生在 hop 之后：这一次 hop 照常执行，回来时结束
+    CHECK(hops == 1);
+    CHECK(handle.isReady());
+    CHECK(executor.pending() == 0U);
+    CHECK(not handle.join().has_value());
+}
+
+auto finishesItsCleanup(co2::Scheduler& scheduler, bool& cleanedUp)
+    CO2_BEG(co2::Task<int>, (scheduler, cleanedUp)) {
+    CO2_AWAIT(co2::unstoppable(co2::scheduleOn(scheduler))); // 收尾阶段：不被取消
+    cleanedUp = true;
+    CO2_AWAIT(co2::scheduleOn(scheduler)); // 普通的 hop：取消点
+    CO2_RETURN(1);
+}
+CO2_END
+
+void anUnstoppableHopIsNotACancellationPoint() {
+    co2::ManualExecutor executor;
+    auto cleanedUp = false;
+    auto handle = co2::spawn(executor, finishesItsCleanup(executor, cleanedUp));
+    CHECK(handle.requestStop());
+    CHECK(executor.run() == 3U); // 启动、第一次 hop、第二次 hop
+    CHECK(cleanedUp);
+    CHECK(not handle.join().has_value());
+}
+
+// 只转发标准三个成员的包装 awaiter。
+struct ForwardsTheHop {
+    co2::ScheduleOn hop;
+
+    bool await_ready() const noexcept { return hop.await_ready(); }
+
+    template <class Promise>
+    void await_suspend(co2::coroutine_handle<Promise> const awaiting) noexcept {
+        hop.await_suspend(awaiting);
+    }
+
+    void await_resume() const noexcept { hop.await_resume(); }
+};
+
+auto hopsThroughAWrapper(co2::Scheduler& scheduler)
+    CO2_BEG(co2::Task<int>, (scheduler)) {
+    CO2_AWAIT(ForwardsTheHop{co2::scheduleOn(scheduler)});
+    CO2_RETURN(1);
+}
+CO2_END
+
+// 不认识 await_completion() 的调用方不被取消：hop 照常完成，协程继续。
+void aCallerThatSkipsAwaitCompletionIsNotCancelledByAHop() {
+    co2::ManualExecutor executor;
+    auto handle = co2::spawn(executor, hopsThroughAWrapper(executor));
+    CHECK(handle.requestStop());
+    CHECK(executor.run() == 2U);
+    CHECK(handle.join() == 1);
+}
+
 } // namespace
 
 int main() {
@@ -570,5 +648,8 @@ int main() {
     anUnstoppableChildSeesNoStopToken();
     unstoppableHidesTheRequestFromACancellationPoint();
     unstoppableStillForwardsHowTheOperationEnded();
+    aHoppingTaskStopsAtItsNextHop();
+    anUnstoppableHopIsNotACancellationPoint();
+    aCallerThatSkipsAwaitCompletionIsNotCancelledByAHop();
     return 0;
 }
````

- [ ] **Step 2: 构建并单独运行这个测试，确认失败**

```bash
cmake --build /tmp/co2-build/gcc14 -j 12
/tmp/co2-build/gcc14/tests/co2-completion-components-tests
```

Expected: `check failed at line 573: hops == 1`。停止请求之后，那个 Task 还在继续 hop。

- [ ] **Step 3: 实现**

修改 `include/co2/stop_token.hpp`、`include/co2/scheduler.hpp`：

````diff
diff --git a/include/co2/scheduler.hpp b/include/co2/scheduler.hpp
index 98470cd..fc98ec9 100644
--- a/include/co2/scheduler.hpp
+++ b/include/co2/scheduler.hpp
@@ -1,13 +1,22 @@
 #pragma once
 
+#include "co2/completion.hpp"
 #include "co2/contract.hpp"
 #include "co2/coroutine_handle.hpp"
+#include "co2/env.hpp"
+#include "co2/stop_token.hpp"
 
 // co2 执行层：Scheduler 是"把一个挂起的协程排队到某个执行上下文上恢复"的最小接口。
 // 队列元素就是一个句柄——一个指针——因此一次 hop 在稳态下零分配、零原子门闩。
 //
 // 与标准模型一致：协程在完成它的那个线程上被内联恢复；想换线程就显式
 // CO2_AWAIT(scheduleOn(s))。没有隐式的"当前 scheduler"。
+//
+// scheduleOn 是取消点（见 completion-channels.md）：协程在目标 scheduler 上被恢复时，
+// 它环境里的停止令牌上若已有请求，就以已停止结束，不再往下执行——与 std::execution 的
+// run_loop 在轮到一项工作时检查停止请求相同。检查在 hop 之后，所以收尾（局部析构、
+// 向上传播）发生在要去的那个线程上。没有停止令牌的协程不受影响；收到停止请求之后还要
+// 换线程做收尾，写 CO2_AWAIT(co2::unstoppable(co2::scheduleOn(s)))。
 
 namespace co2 {
 
@@ -25,21 +34,32 @@ struct Scheduler {
 };
 
 // co_await scheduleOn(s)：把当前协程转移到 s 上继续执行。awaiter 只持有 scheduler 的
-// 指针，不借用表达式里的任何临时对象。
+// 指针与等待者的停止令牌，不借用表达式里的任何临时对象。
 struct ScheduleOn {
     Scheduler* scheduler;
+    detail::EnvStopToken token; // 等待者的停止令牌：hop 之后看它上面有没有请求
 
     bool await_ready() const noexcept { return false; }
 
-    void await_suspend(coroutine_handle<> const coroutine) noexcept {
+    // 令牌必须在 schedule() 之前记下：schedule() 一返回，协程可能已经在另一个线程上
+    // 恢复，此后再写这个 awaiter 就是数据竞争。co2 自己的 promise 只是被借用——等待者
+    // 在 hop 期间一直挂着——不碰引用计数。
+    template <class Promise>
+    void await_suspend(coroutine_handle<Promise> const coroutine) noexcept {
+        detail::inheritStopToken(token, coroutine);
         scheduler->schedule(coroutine);
     }
 
+    // 不认识 await_completion() 的调用方不被取消：hop 照常完成，协程继续。
     void await_resume() const noexcept {}
+
+    Completion await_completion() const noexcept {
+        return token.stopRequested() ? Completion::stopped() : Completion::value();
+    }
 };
 
 inline ScheduleOn scheduleOn(Scheduler& scheduler) noexcept {
-    return ScheduleOn{&scheduler};
+    return ScheduleOn{&scheduler, detail::EnvStopToken{}};
 }
 
 } // namespace co2
diff --git a/include/co2/stop_token.hpp b/include/co2/stop_token.hpp
index 9f74a70..ac135cb 100644
--- a/include/co2/stop_token.hpp
+++ b/include/co2/stop_token.hpp
@@ -338,6 +338,9 @@ struct EnvStopToken {
     EnvStopToken(EnvStopToken const&) = delete;
     EnvStopToken& operator=(EnvStopToken const&) = delete;
 
+    // 交出自己的那一份引用（或借用关系），变成空的。带着它的 awaiter 靠这个被搬进帧。
+    EnvStopToken(EnvStopToken&& other) noexcept : bits{other.bits} { other.bits = 0U; }
+
     ~EnvStopToken() { reset(); }
 
     // 接管 token 的那一份引用。
````

- [ ] **Step 4: 构建并运行全部测试，看哪个现有测试受影响**

Expected: 只有 `co2-spawn-tests` 失败，`check failed at line 189: handle.join() >= 0`。那个协程自己轮询令牌、想正常返回一个值，现在它在 hop 回来时就以已停止结束了，`join()` 得到空的 optional。（这个失败不是每次都出现：停止请求偶尔会恰好落在 hop 回来之后、循环条件之前。）

- [ ] **Step 5: 更新受影响的测试，并加上线程池上的取消测试**

修改 `tests/spawn_tests.cpp`：

````diff
diff --git a/tests/spawn_tests.cpp b/tests/spawn_tests.cpp
index d748e73..7abb467 100644
--- a/tests/spawn_tests.cpp
+++ b/tests/spawn_tests.cpp
@@ -24,6 +24,7 @@
 #include "co2/sync_wait.hpp"
 #include "co2/task.hpp"
 #include "co2/thread_pool.hpp"
+#include "co2/unstoppable.hpp"
 
 namespace {
 
@@ -167,13 +168,15 @@ void awaitingAnAlreadyCompletedHandleDoesNotSuspend() {
 // ---------------------------------------------------------------------------
 // stop_token：请求、观察、传播
 
+// 自己轮询令牌、自己决定什么时候结束。scheduleOn 是取消点，所以这里的 hop 用
+// unstoppable 包住：否则协程会在 hop 回来时就以已停止结束，走不到下面的 CO2_RETURN。
 auto spinUntilStopped(co2::ThreadPool& pool)
     CO2_BEG(co2::Task<int>, (pool), co2::stop_token token; int hops{};) {
     CO2_AWAIT_SET(token, co2::getStopToken());
     CHECK(token.stop_possible());
     while (not token.stop_requested()) {
         ++hops;
-        CO2_AWAIT(co2::scheduleOn(pool));
+        CO2_AWAIT(co2::unstoppable(co2::scheduleOn(pool)));
     }
     CO2_RETURN(hops);
 }
@@ -189,6 +192,23 @@ void requestStopIsObservedThroughGetStopToken() {
     CHECK(handle.join() >= 0);
 }
 
+auto hopsForever(co2::ThreadPool& pool) CO2_BEG(co2::Task<int>, (pool)) {
+    for (;;)
+        CO2_AWAIT(co2::scheduleOn(pool));
+}
+CO2_END
+
+// 不轮询也能取消：scheduleOn 是取消点，停止请求之后的下一次 hop 回来时 Task 以已停止
+// 结束，join() 返回空的 optional。
+void requestStopEndsAHoppingTaskAtItsNextHop() {
+    co2::ThreadPool pool{2U};
+    auto handle = co2::spawn(pool, hopsForever(pool));
+    std::this_thread::sleep_for(std::chrono::milliseconds{5});
+    CHECK(not handle.isReady());
+    CHECK(handle.requestStop());
+    CHECK(not handle.join().has_value());
+}
+
 auto guardedSpin(co2::ThreadPool& pool, std::atomic<int>& destroyed)
     CO2_BEG(co2::Task<>, (pool, destroyed), co2::detail::ResultStorage<Guard> guard;
             co2::stop_token token;) {
@@ -205,7 +225,8 @@ void droppingAJoinHandleRequestsStopAndDetaches() {
         co2::ThreadPool pool{2U};
         for (int i = 0; i < 8; ++i)
             co2::spawn(pool, guardedSpin(pool, destroyed));
-        // 句柄已全部丢弃：每个 Task 收到停止请求、跑完、帧被销毁——线程池析构会排空。
+        // 句柄已全部丢弃：每个 Task 收到停止请求，在下一次 hop 回来时以已停止结束，帧被
+        // 销毁——线程池析构会排空。
     }
     CHECK(destroyed.load() == 8);
 }
@@ -338,6 +359,7 @@ int main() {
     joinHandlesAreAwaitable();
     awaitingAnAlreadyCompletedHandleDoesNotSuspend();
     requestStopIsObservedThroughGetStopToken();
+    requestStopEndsAHoppingTaskAtItsNextHop();
     droppingAJoinHandleRequestsStopAndDetaches();
     childTasksInheritTheParentsToken();
     descendantsOfAForeignPromiseKeepTheInheritedTokenAlive();
````

- [ ] **Step 6: 构建并运行全部测试，确认通过**

Expected: `100% tests passed out of 30`。

- [ ] **Step 7: 在 TSan 下运行，并重复运行线程相关的测试**

```bash
cmake -S . -B /tmp/co2-build/tsan -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=clang++ \
      -DCMAKE_CXX_FLAGS="-fsanitize=thread"
cmake --build /tmp/co2-build/tsan -j 12
(cd /tmp/co2-build/tsan && /tmp/co2-build/runlimit 900 setarch $(uname -m) -R ctest -j 8)
(cd /tmp/co2-build/tsan && /tmp/co2-build/runlimit 900 setarch $(uname -m) -R \
    ctest -R "spawn|thread-pool|completion-components|when-all|callback" --repeat until-fail:5 -j 4)

cmake -S . -B /tmp/co2-build/gccrel -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++
cmake --build /tmp/co2-build/gccrel -j 12
(cd /tmp/co2-build/gccrel && /tmp/co2-build/runlimit 600 \
    ctest -R "spawn|thread-pool|completion-components|when-all|callback" --repeat until-fail:40 -j 4)
```

Expected: 第一条 `ctest` 是 `100% tests passed out of 30`，后两条是 `100% tests passed out of 6`。`runlimit` 没有因为超时把它们杀掉。

- [ ] **Step 8: 提交**

```bash
git add include/co2/scheduler.hpp include/co2/stop_token.hpp \
        tests/completion_components_tests.cpp tests/spawn_tests.cpp
git commit -F - <<'EOF'
scheduleOn 成为取消点

行为变化（docs/design/completion-channels.md 第 4 节、第 6 节第 2、7 条）。协程在
目标 scheduler 上恢复时，它的停止令牌上若已有请求，就以已停止结束，不再往下执行。
与 std::execution 的 run_loop 一样，检查在 hop 之后。

- awaiter 在调用 schedule() 之前借下等待者的停止令牌：schedule() 一返回，协程可能
  已经在另一个线程上恢复，此后不能再写 awaiter。
- 没有停止令牌的协程不受影响；不认识 await_completion() 的调用方也不被取消。
- 收到停止请求之后还要换线程，写 unstoppable(scheduleOn(s))。

受影响的现有测试只有一处：spawn_tests 里自己轮询令牌的那个协程，hop 改用
unstoppable 包住。丢弃 JoinHandle 的 Task 现在会在下一次 hop 结束。

线程相关的测试在 Release 下重复 40 遍、TSan 下重复 5 遍通过。
EOF
```

### Task 10: 文档、基准与全配置验证

**Files:**
- Modify: `benchmarks/co2_benchmarks.cpp`
- Modify: `README.md`、`docs/architecture.md`、`docs/design/completion-channels.md`

**Interfaces:**
- Consumes: 前九个任务的全部结果。
- Produces: 基准行 `"Task stopped through a 100-deep chain, per layer"`（函数 `taskStoppedThroughChain()`、协程 `stopsAtDepth(int depth)`）。

- [ ] **Step 1: 基准加上"已停止"一行**

修改 `benchmarks/co2_benchmarks.cpp`：

````diff
diff --git a/benchmarks/co2_benchmarks.cpp b/benchmarks/co2_benchmarks.cpp
index 556f545..8acfe2a 100644
--- a/benchmarks/co2_benchmarks.cpp
+++ b/benchmarks/co2_benchmarks.cpp
@@ -170,6 +170,25 @@ double taskFailureThroughChain() {
     });
 }
 
+// 已停止沿等待链向上传：最里面一层以已停止结束，根上得到空的 optional。
+auto stopsAtDepth(int depth) CO2_BEG(co2::Task<int>, (depth), int v{};) {
+    if (depth == 0) CO2_AWAIT(co2::just_stopped());
+    CO2_AWAIT_SET(v, stopsAtDepth(depth - 1));
+    CO2_RETURN(v + 1);
+}
+CO2_END
+
+double taskStoppedThroughChain() {
+    constexpr int depth = 100;
+    constexpr int rounds = 50000;
+    return nanosecondsPerOp(static_cast<std::size_t>(depth) * rounds, [&] {
+        long stopped = 0;
+        for (auto round = 0; round != rounds; ++round)
+            if (not co2::syncWait(stopsAtDepth(depth)).has_value()) ++stopped;
+        sink = stopped;
+    });
+}
+
 // 多个线程各自跑一条"等待子 Task"的循环。sharedToken 为真时所有线程的 Task 树共用同一个
 // stop_token——whenAll / spawn 的各个分支落在不同线程上时就是这种形态。
 double awaitChildOnThreads(int const threads, bool const sharedToken) {
@@ -256,6 +275,7 @@ Benchmark const benchmarks[] = {
     {"symmetric Task transfer (with frame allocation)", &symmetricTaskTransfer},
     {"syncWait ready Task", &syncWaitReadyTask},
     {"Task failure through a 100-deep chain, per layer", &taskFailureThroughChain},
+    {"Task stopped through a 100-deep chain, per layer", &taskStoppedThroughChain},
     {"Task await child, 8 threads, own stop_token", &awaitChildOwnToken},
     {"Task await child, 8 threads, shared stop_token", &awaitChildSharedToken},
     {"ManualExecutor hop", &manualExecutorHop},
````

- [ ] **Step 2: 构建并运行基准**

```bash
cmake -S . -B /tmp/co2-build/bench -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++ \
      -DCMAKE_CXX_FLAGS_RELEASE="-O2 -DNDEBUG" -DCO2_BUILD_BENCHMARKS=ON -DCO2_BUILD_TESTS=OFF
cmake --build /tmp/co2-build/bench -j 12
/tmp/co2-build/bench/benchmarks/co2-benchmarks
```

Expected（这台机器，GCC 13.3 `-O2`，中位数）：`Task stopped through a 100-deep chain, per layer` 约 20 ns，与正常返回同一量级；`Task failure through a 100-deep chain, per layer` 约 44～45 ns；`ManualExecutor hop` 约 22 ns（取消点之前是 20～21）。线程池的几项对机器负载敏感，看量级。

- [ ] **Step 3: 更新文档**

修改 `README.md`：

````diff
diff --git a/README.md b/README.md
index 8200abf..9b20721 100644
--- a/README.md
+++ b/README.md
@@ -160,10 +160,14 @@ CO2_END
 每一步都是生产者与消费者之间的对称转移，没有同步；消费者在生产者 yield 所在的线程
 上恢复。有 `next()` 未完成时销毁是契约违规。
 
-## 错误：按值向上传
+生产者出错或已停止即流结束：正在等待的那次 `next()` 以同样的结果完成，消费者默认也
+就此结束（想自己处理就写 `co2::asResult(stream.next())`）。那次失败只交付一次，之后的
+`next()` 得到 `false`。
+
+## 错误与已停止：按值向上传
 
 一个 Task 以值、错误或已停止结束。错误是一个 `co2::Error`：要么是一个
-`std::error_code`，要么是一个被捕获的异常。
+`std::error_code`，要么是一个被捕获的异常。已停止表示操作被取消了，没有结果。
 
 ```cpp
 #include <co2/as_result.hpp>
@@ -200,21 +204,41 @@ CO2_END
   特化加 `make_error_code`），之后可以直接传给 `just_error`。
   `CO2_YIELD(co2::with_error(e))` 是 P3552 的拼写，效果相同。
 
+已停止走的是同一条路，只是到了根上不抛异常，而是空的 optional：
+
+```cpp
+#include <co2/unstoppable.hpp>
+
+CO2_AWAIT(co2::stopIfRequested());                    // 取消点：有停止请求才停，否则继续
+CO2_AWAIT(co2::scheduleOn(pool));                     // 也是取消点，见"执行层"
+CO2_AWAIT(co2::just_stopped());                       // 本协程以已停止结束
+CO2_AWAIT(co2::unstoppable(co2::scheduleOn(pool)));   // 对这一次等待隐藏停止请求
+```
+
+- **协程在取消点结束。** 取消是协作式的：停止请求只是一个标志，协程走到取消点才看它。
+  结束的方式与出错相同——其后的语句不执行，局部析构，等待它的协程以已停止结束。
+- **`unstoppable(x)` 用在收尾阶段。** 已经收到停止请求、但还有几步必须做完时，把那几步
+  包起来：被包住的操作（以及它等待的子 Task）看不到停止请求。它只隐藏外面的请求，
+  被包住的操作自己出错或自己停止照样传出来。
+- **组件都遵守同一套规则。** 等待 `JoinHandle`、`whenAll`、`AsyncGenerator` 的 `next()`、
+  `fromCallback`，被等待的一方出错或已停止，等待它的协程就以同样的结果结束；各自的
+  细节见对应的小节。
+
 写 awaiter 与 promise 的人各多一个可选的扩展点：
 
 - awaiter 可以提供 `co2::Completion await_completion() noexcept`，报告这次等待是有值、
   出错还是已停止。恢复点先问它，有值才调用 `await_resume()`。没有这个成员的 awaiter
   行为不变。**包装别的 awaiter 的 awaiter 要把它一并转发**：不转发时，被包住的操作
   出错仍然从 `await_resume()` 抛出（与从前相同），以已停止完成则是契约违规。
+  `scheduleOn` 与 `stopIfRequested()` 例外：它们的 `await_resume()` 照常返回，不转发
+  只是让它们不再是取消点。
 - promise 可以提供 `void return_error(co2::Error) noexcept` 与
   `void return_stopped() noexcept` 接收后两种结果。没有 `return_error()` 时错误在
   协程体内重新抛出，走 `unhandled_exception()`；没有 `return_stopped()` 时"已停止"是
   契约违规。
 
-"已停止"目前只有自己写的 awaiter 能产生；`whenAll`、`JoinHandle`、`AsyncGenerator`、
-`fromCallback` 也还没有接上 `await_completion()`——它们遇到错误仍然从
-`await_resume()` 抛出，结果相同，只是多一次抛接。完整的设计与落地顺序见
-`docs/design/completion-channels.md`。
+完整的设计与落地顺序见 `docs/design/completion-channels.md`。禁用异常的构建
+（`-fno-exceptions`）还没有做，是那份设计的最后一步。
 
 ## 执行层
 
@@ -229,6 +253,11 @@ CO2_AWAIT(co2::scheduleOn(scheduler));   // 转移到 scheduler 上继续
 
 队列元素就是一个句柄，一次 hop 稳态零分配。
 
+`scheduleOn` 是取消点：协程在目标 scheduler 上恢复时，它的停止令牌上若已有请求，就以
+已停止结束，不再往下执行。检查在 hop 之后，收尾发生在要去的那个线程上。没有停止令牌的
+协程不受影响。收到请求之后还要换线程做收尾，写
+`co2::unstoppable(co2::scheduleOn(s))`。
+
 ### `ThreadPool`
 
 工作窃取线程池：每线程一个固定容量的本地 FIFO 队列 + 互斥全局队列；本地 → 全局 →
@@ -271,8 +300,13 @@ CO2_AWAIT_SET(value, handle);                    // 或在协程里等待（左
 co2::spawn(pool, work(), parentToken);           // 父 token 的停止请求转发过来
 ```
 
-未完成时析构 `JoinHandle`（与 P3149 `spawn_future` 一致）：请求停止并分离，Task 跑完
-后帧被销毁，结果与异常丢弃。借用它的等待者还挂着时析构是契约违规。
+在协程里等待 `JoinHandle`：被 spawn 的 Task 出错或已停止，等待它的协程以同样的结果
+结束。`join()` 对已停止返回空的 optional，出错时抛出。
+
+未完成时析构 `JoinHandle`（与 P3149 `spawn_future` 一致）：请求停止并分离，Task 结束
+后帧被销毁，结果丢弃。借用它的等待者还挂着时析构是契约违规。注意停止请求是真的：被
+丢弃句柄的 Task 会在它的下一个取消点结束，比如下一次 `scheduleOn`。想让它跑完，就
+保留句柄，或者在 Task 里把那些 hop 写成 `co2::unstoppable(co2::scheduleOn(s))`。
 
 ### `whenAll`
 
@@ -287,9 +321,10 @@ CO2_AWAIT_SET(all, co2::whenAll(std::move(tasks)));          // std::vector<Task
 
 并发启动一组 Task，全部完成后一起交付结果，语义取自 `std::execution::when_all`：非 void
 的结果按参数顺序拼成 tuple（全部 void 则结果是 void），`std::vector<Task<T>>` 给出
-`std::vector<T>`。任一子 Task 失败就向其余子 Task 请求停止，等**全部**完成后重抛时间上
-第一个异常；父的 `stop_token` 转发给每个子 Task。子 Task 在等待者的线程上依次内联启动，
-要并行就让它们各自 `scheduleOn(pool)`。
+`std::vector<T>`。任一子 Task 出错或已停止就向其余子 Task 请求停止，等**全部**结束后：
+有出错的，以时间上第一个错误完成；否则有已停止的，以已停止完成。父的 `stop_token` 转发
+给每个子 Task。子 Task 在等待者的线程上依次内联启动，要并行就让它们各自
+`scheduleOn(pool)`。
 
 ### `fromCallback`
 
@@ -307,10 +342,23 @@ CO2_END
 ```
 
 把回调式异步 API 变成 awaiter：`initiate(continuation)` 或
-`initiate(continuation, stop_token)` 发起操作，API 完成时调用 `continuation(args...)`
-（就地构造结果）或 `continuation.setException(error)`，**恰好一次**。协程在回调所在的
-线程上恢复；回调在 `initiate` 返回之前就到达时不挂起。状态全部在 awaiter 里，通常没有
-堆分配。表达式里有 lambda，所以用 `CO2_AWAIT_AS` 显式给出 awaitable 类型。
+`initiate(continuation, stop_token)` 发起操作，API 完成时调用续体的四个出口之一，
+**恰好一次**：
+
+```cpp
+done(args...);              // 有值：就地构造结果
+done.setError(code);        // 出错：一个 std::error_code
+done.setException(error);   // 出错：一个捕获的异常
+done.setStopped();          // 已停止：操作被取消了
+```
+
+后三种让等待它的协程以同样的结果结束。要支持取消，就在 `initiate` 里用收到的
+`stop_token` 注册一个 `stop_callback` 去调 API 自己的 cancel，API 随后以"已取消"回调，
+在那个回调里调用 `done.setStopped()`。
+
+协程在回调所在的线程上恢复；回调在 `initiate` 返回之前就到达时不挂起。状态全部在
+awaiter 里，通常没有堆分配。表达式里有 lambda，所以用 `CO2_AWAIT_AS` 显式给出
+awaitable 类型。
 
 ## 取消：`stop_token`
 
@@ -321,12 +369,29 @@ CO2_END
 token 沿 Task 树传播：`syncWait`/`spawn` 提供根 token，子 Task 从父 promise 继承
 （等待者的 `await_suspend` 收到带类型的父句柄，与 `std::execution::task` 查询父环境
 的方式相同）。`AsyncGenerator` 的生产者同样在这条链上：每次 `next()` 使用发起它的
-消费者的 token。体内读取：
+消费者的 token。
+
+取消是协作式的：协程在取消点结束（见"错误与已停止"），通常不需要自己读令牌：
+
+```cpp
+auto loop(co2::ThreadPool& pool) CO2_BEG(co2::Task<>, (pool)) {
+    for (;;) {
+        CO2_AWAIT(co2::scheduleOn(pool));   // 取消点：收到停止请求之后在这里结束
+        step();
+    }
+}
+CO2_END
+```
+
+想自己决定什么时候停、停之前做什么，照旧在体内读令牌。这时循环里的 hop 要用
+`unstoppable` 包住，否则协程会先在 hop 那里结束，走不到自己的判断：
 
 ```cpp
-auto loop(co2::ThreadPool& pool) CO2_BEG(co2::Task<>, (pool), co2::stop_token token;) {
+auto loop(co2::ThreadPool& pool) CO2_BEG(co2::Task<int>, (pool), co2::stop_token token;) {
     CO2_AWAIT_SET(token, co2::getStopToken());
-    while (not token.stop_requested()) CO2_AWAIT(co2::scheduleOn(pool));
+    while (not token.stop_requested())
+        CO2_AWAIT(co2::unstoppable(co2::scheduleOn(pool)));
+    CO2_RETURN(flush());   // 正常返回一个值
 }
 CO2_END
 ```
@@ -353,7 +418,7 @@ co2::setContractViolationHandler([](co2::ContractViolation const& v) {
 | `co2/dsl.hpp` | `CO2_*` 宏 |
 | `co2/task.hpp` / `generator.hpp` / `async_generator.hpp` | 返回类型 |
 | `co2/error.hpp` / `completion.hpp` / `result.hpp` / `optional.hpp` | `Error`、`Completion`、`Result<T>` 与 `Unit`、`optional<T>` |
-| `co2/just.hpp` / `as_result.hpp` | `just_error` / `with_error`、`asResult` |
+| `co2/just.hpp` / `as_result.hpp` / `unstoppable.hpp` | `just_error` / `with_error` / `just_stopped` / `stopIfRequested`、`asResult`、`unstoppable` |
 | `co2/scheduler.hpp` / `manual_executor.hpp` / `thread_pool.hpp` | 执行层 |
 | `co2/sync_wait.hpp` / `spawn.hpp` | 根适配器 |
 | `co2/when_all.hpp` / `callback.hpp` | 组合器 `whenAll`、回调适配 `fromCallback` |
@@ -382,16 +447,19 @@ i7-13700KF，每项 7 轮取中位数：
 | elements_of 深度 8 yield/next | 1.9 |
 | symmetric Task transfer（含帧分配） | 12.0 |
 | syncWait ready Task | 26.7 |
-| Task 链上的失败，100 层，每层 | 45.3 |
+| Task 链上的失败，100 层，每层 | 44.4 |
+| Task 链上的已停止，100 层，每层 | 20.3 |
 | Task 等待子 Task，8 线程各自的 stop_token | 19.3 |
 | Task 等待子 Task，8 线程共享一个 stop_token | 18.2 |
-| ManualExecutor hop | 20.1 |
+| ManualExecutor hop | 22.3 |
 | ThreadPool hop（1 worker，1 Task） | 13.1 |
 | ThreadPool hop（4 workers，32 Task 聚合） | 3.5 |
 | ThreadPool hop（8 workers，1 Task，不饱和） | 14.5 |
 | spawn + join（跨线程唤醒） | 2542 |
 
 "Task 链上的失败"在错误改为按值传播之前是 558：那时每一层都要重抛一次异常。
+`scheduleOn` 成为取消点之后，每次 hop 多读一次停止状态，约 1 ns：`ManualExecutor hop`
+从 20.1 变为 22.3，线程池的几项变化在噪声以内。
 
 线程池的几项对机器负载敏感：同一台机器上有其他负载时，不饱和那一项在 17～30 ns 之间
 浮动。数字只在同一台机器、同一份编译选项下可比。
````

修改 `docs/architecture.md`、`docs/design/completion-channels.md`：

````diff
diff --git a/docs/architecture.md b/docs/architecture.md
index 26acd84..1e8ac00 100644
--- a/docs/architecture.md
+++ b/docs/architecture.md
@@ -133,7 +133,8 @@ promise 不受影响。
   `active`，内层 `final_suspend` 把 `active` 改回父层并对称转移过去。嵌套生成器由父层
   帧里的 awaiter 拥有。yield 的 prvalue 移进帧保存（D11），左值引用只记地址。
 - **`AsyncGenerator<T>`**：`next()` 记下消费者并转移进生产者；`yield_value` /
-  `final_suspend` 转移回消费者。任一时刻只有一方运行，没有同步。
+  `final_suspend` 转移回消费者。任一时刻只有一方运行，没有同步。生产者出错或已停止时，
+  结果记在 promise 上，由正在等待的那次 `next()` 的 `await_completion()` 取走一次。
 
 ## 环境与取消
 
@@ -145,6 +146,11 @@ promise（自定义根）与类型擦除的句柄给出 `stop_possible()` 为假
 `AsyncGeneratorPromise` 以同样的方式参与：`next()` 的 awaiter 在 `await_suspend` 里把
 消费者的 token 交给生产者，生产者体内与它等待的子 Task 看到的都是这一份。
 
+取消点（`docs/design/completion-channels.md`）：停止请求只是令牌上的一个标志，协程走到
+取消点才以已停止结束——`stopIfRequested()`、`scheduleOn`，以及任何以已停止完成的操作
+（`fromCallback` 的 `setStopped()`、被取消的子 Task、`whenAll`）。`unstoppable(x)` 把
+类型擦除的句柄交给 x 的 `await_suspend`，x 于是查不到等待者的环境，看不到停止请求。
+
 继承是**借用**而不是拷贝。promise 里存的是 `detail::EnvStopToken`（一个指针宽）：根 Task
 持有一份引用；被等待的子协程只记下等待者的停止状态，不碰引用计数——等待者在子协程
 运行期间一直挂着，它的 promise 活得更长。这样每次 `co_await` 子 Task 都没有原子操作，
@@ -162,6 +168,9 @@ token 的基准：240 → 20 ns/await）。等待者不是 co2 自己的 promise
 
 - **`Scheduler::schedule(coroutine_handle<>) noexcept`**：队列元素就是句柄，一次 hop
   稳态零分配。协程在完成它的线程上被内联恢复；换线程显式 `scheduleOn(s)`。
+  `scheduleOn` 是取消点：awaiter 在调用 `schedule()` **之前**借下等待者的停止令牌
+  （之后协程可能已经在别的线程上恢复，不能再写 awaiter），恢复时令牌上有请求就以
+  已停止完成——与 `std::execution` 的 `run_loop` 在轮到一项工作时检查停止请求相同。
 - **`ThreadPool`**：每线程一个固定容量（1024）的本地 FIFO 队列（owner 尾部 push，
   owner/thief 头部 CAS 认领——单生产者多消费者环）+ 互斥全局队列（外部提交、本地溢出）。
   找活 本地 → 全局 → 随机起点窃取（一次搬走受害者队列的一半；只剩一项且主人在自己
diff --git a/docs/design/completion-channels.md b/docs/design/completion-channels.md
index c127a1d..f335975 100644
--- a/docs/design/completion-channels.md
+++ b/docs/design/completion-channels.md
@@ -1,9 +1,11 @@
 # 三种完成方式：值、错误、已停止——以及禁用异常的构建
 
-状态：设计已确认（2026-11-12）。第 10 节的第 1～5 步已经实现（词汇类型、根返回
+状态：设计已确认（2026-11-12）。第 10 节的第 1～8 步已经实现：词汇类型、根返回
 optional、恢复点与 promise 的两个入口、`Task` 四态、`just_error` / `with_error` /
-`asResult`）；第 6～10 步尚未实现。设计确认当天对照库的代码评审过一轮并修订，哪些
-论断验证过、哪些没有，见第 12 节。
+`asResult`、四个组件接上 `await_completion()`、`just_stopped` / `stopIfRequested` /
+`unstoppable`、`scheduleOn` 取消点。第 9、10 步（禁用异常的构建、文档收尾）尚未实现。
+设计确认当天对照库的代码评审过一轮并修订；哪些论断验证过、哪些没有，以及实现时与
+本文不同的几处，见第 12 节。
 
 ## 0. 问题
 
@@ -213,6 +215,10 @@ struct MyAwaiter {
 | 错误 | `Error::raise()`：开异常时抛出，与今天完全相同；禁用异常时契约违规 |
 | 已停止 | 契约违规 |
 
+`scheduleOn` 与 `stopIfRequested()` 不在此列：它们"已停止"与否只是恢复点对停止令牌的
+一次检查，hop 本身已经完成，有值可给。它们的 `await_resume()` 照常返回——不认识
+`await_completion()` 的调用方只是不被取消。
+
 由此得到两条性质：
 
 - 错误照旧能穿过旧式的调用方。只把 `Task` 改成按值记录错误、其余组件一律不动，现有
@@ -312,14 +318,18 @@ promise 没有对应入口时（用户自定义的返回类型）：
 - 原型里异常比错误码每层多约 20 纳秒。原因没有单独验证，推测是原型每层把
   `exception_ptr` 复制了两次；实现时 `Error` 一路移动。
 
-**值与错误共用存储，钉死的大小不变。** 两者互斥，分开存是白占空间，而且会越过两条
-现有的界线：
+**值与错误共用存储，帧不变大，回调 awaiter 不掉出内联槽。** 两者互斥，分开存是白占
+空间，而且会越过两条现有的界线：
 
 - `TaskPromise<int>` 现在 40 字节。分开存是 56 字节，`Task<int>(int)` 的帧不再是
   `allocation_tests` 钉死的 176 字节；共用存储仍是 40 字节，结果类型大于 16 字节时还比
   现在小 16 字节。
 - `CallbackAwaitable<void*>` 现在 64 字节，正好等于内联 awaiter 槽的容量。再多 8 字节
-  就落到堆上，每次等待多一次分配。
+  就落到堆上，每次等待多一次分配。实现时它把结果的三种形态——值、错误码、捕获的
+  异常——放进同一块存储，由状态字里的标志区分，而不是存一个 `co2::Error`：MSVC 上
+  `Error` 有 3 个指针宽，会把 awaiter 顶出槽。结果不超过 2 个指针宽的 awaiter 于是都
+  正好是 8 个指针：`CallbackAwaitable<int>` 从 56 字节变成 64 字节（仍在槽内，帧大小
+  不变），2 个指针宽的结果从 72 字节（落到堆上）变成 64 字节。
 
 ## 4. 各组件的行为
 
@@ -562,10 +572,20 @@ awaiter 上再恢复等待者。
 对照的是 `cplusplus/draft` 的 `exec.tex`；`with_error`、`just_error`、`just_stopped`
 的拼写对照的是 P3552 的文本。
 
+**实现时与本文不同的几处**（第 1～8 步，已同步到正文）
+
+- `scheduleOn` 与 `stopIfRequested()` 的 `await_resume()` 不是契约违规，而是照常返回
+  （3.1）。
+- `CallbackAwaitable<int>` 的大小变了：56 → 64 字节，仍在内联槽里（3.5）。
+- `Task` 的错误与值一样只可取走一次。第 5 步落地的代码在这里有个缺陷：同一个失败的
+  Task 被等待两次时，第二次会把一个已经搬空的错误再交出去，带着异常的那一种会在根上
+  崩溃。现在第二次等待是契约违规，与取值两次相同。
+- 被移动之后的 `Error` 不保证内容：libc++ 的 `exception_ptr` 移动后不置空。
+- `Result<T>` 不能默认构造，作帧局部时写成 `co2::optional<co2::Result<T>>`；
+  `asResult(x)` 与 `unstoppable(x)` 要直接写在等待它们的地方。
+
 **尚未验证的**
 
 - 禁用异常的构建（第 5 节）没有任何实测。
 - MSVC：`Error` 的大小、`#pragma detect_mismatch`、`_CPPUNWIND` 的判断都无法在本机
   验证。
-- `asResult`、`unstoppable`、`whenAll`、`JoinHandle`、`AsyncGenerator`、
-  `fromCallback`、`scheduleOn` 的新行为没有在原型里实现，第 4 节只对照了代码与草案。
````

README 基准表里的数字是这台机器上量到的。Step 2 量到的中位数与表里相差超过 5% 的那一行（线程池的几项除外），改成量到的数字。

- [ ] **Step 4: 全配置验证**

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

Expected: 13 行，每行都是 `100% tests passed out of 30`。

- [ ] **Step 5: 提交**

```bash
git add benchmarks/co2_benchmarks.cpp README.md docs/architecture.md \
        docs/design/completion-channels.md
git commit -F - <<'EOF'
文档与基准：已停止、取消点、各组件的三种完成方式

README：按值传播一节扩成"错误与已停止"，补上 just_stopped、stopIfRequested、
unstoppable；scheduleOn、JoinHandle、whenAll、fromCallback、AsyncGenerator 与
stop_token 各节按新行为更新。架构文档与设计稿同步；设计稿标明第 10 节的第 1～8 步
已实现，并记下实现时与它不同的几处。

基准新增"已停止沿 100 层的链每层的成本"：20.3 ns，与正常返回同一量级。scheduleOn
成为取消点之后每次 hop 多读一次停止状态：ManualExecutor hop 20.1 → 22.3 ns，线程池
的几项在噪声以内。
EOF
```
