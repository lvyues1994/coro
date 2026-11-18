# 三种完成方式 实施计划（三）：禁用异常的构建与文档收尾

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 让库在 `-fno-exceptions -fno-rtti` 下能编译和使用，全部测试在开、关异常两种构建下都跑，并把文档收尾。

**Architecture:** 计划（一）（二）已经让值、错误、已停止三种结果按值传播，这条路不依赖异常。剩下的是把库里与异常直接相关的几处用一个宏隔开：`config.hpp` 从编译器的开关得出 `CO2_HAS_EXCEPTIONS`，为 0 时协程体变换里的 catch、`Error` 里装异常的那一半、把异常接成错误的几处 catch 都不存在，`Error::raise()` 变成契约违规。初始化中途失败的回滚先改成守卫对象，于是不需要条件编译。测试一侧用一个 CMake 选项给全部测试加上 `-fno-exceptions -fno-rtti`。

**Tech Stack:** C++14，仅头文件；CMake 与 CTest；GitHub Actions；没有第三方依赖。

设计稿：`docs/design/completion-channels.md`。本计划覆盖它第 10 节的第 9、10 步，也是最后两步。前两份计划：`docs/design/completion-channels-plan-1.md`、`docs/design/completion-channels-plan-2.md`。

**状态：** 已执行完毕（2026-11-12），5 个任务对应提交 `2e6f1f4`、`05e1360`、`c3d3d6c`、`c380dd9`、`f16a09e`。每个任务都按这里的步骤做了，提交的代码和文档与本文逐字相同；任务 5 的 22 个配置与重复运行全部通过。CI 上的新作业还没有跑过（这些提交没有推送），MSVC 没有验证。执行时发现本文有一处写得不准：

- 任务 3 步骤 1 说禁用异常的完整构建失败时"错误都在 `tests/` 下的源文件里"。实际有几条报在 `include/co2/error.hpp` 第 68 行（`enable_if` 里没有 `type`）。那是测试代码拿 `exception_ptr` 去构造 `Error` 引出的模板实例化错误，起因仍在测试源码里；库的头文件自己在这种构建下能编译，任务 2 步骤 4 验过。

## Global Constraints

- 库与测试的最低语言标准是 C++14，不使用 C++17 及以后的特性。
- 仅头文件，不引入第三方依赖。
- 测试以 `-Wall -Wextra -Wpedantic -Werror -Wconversion -Wsign-conversion -Wold-style-cast -Wzero-as-null-pointer-constant` 等选项编译（见 `tests/CMakeLists.txt`），新代码不得有告警。禁用异常时另加 `-fno-exceptions -fno-rtti`。
- 每个公共头文件都必须能单独包含（`co2-header-self-containment-tests` 会检查），两种构建下都是。
- `CO2_HAS_EXCEPTIONS` 只由编译器的开关得出，不提供任何手动覆盖。
- 库的头文件里，`try` / `catch` / `throw` 以及 `std::exception_ptr`、`std::current_exception` 之类只能出现在 `#if CO2_HAS_EXCEPTIONS` 之内。
- 值、错误码、已停止三条通道的代码不加条件编译：两种构建是同一份。
- 开着异常的构建行为不变：每个任务结束时，默认配置下的全部测试都通过。
- 条件编译指令不能写在宏的实参里（那是未定义行为，`-Wpedantic` 会报）。协程体写在 `CO2_BEG(...) {` 与 `CO2_END` 之间，不是宏的实参，可以用；`CO2_AWAIT_AS_SET(...)` 之类的括号里不行。
- `tests/allocation_tests.cpp` 钉死的帧大小不得改变。
- 前置条件用 `CO2_CONTRACT_CHECK` / `CO2_CONTRACT_FAIL` 报告，不为它们抛异常。
- 每个任务一个本地提交，不推送。提交信息用中文，正文说明原因与验证。`git add` 写明路径，不要用 `git add -A`：仓库里有不属于本工作的未跟踪目录 `build/`。
- 构建目录放在仓库之外（`/tmp/co2-build/...`），不要动仓库里的 `build/`。

## 范围

设计稿第 10 节的第 9、10 步：

- 任务 1：初始化中途失败的回滚改用守卫对象（为第 9 步做准备，行为不变）。
- 任务 2（第 9 步）：`CO2_HAS_EXCEPTIONS`、库里的条件编译、CMake 选项、只在禁用异常时成立的契约违规测试。
- 任务 3（第 9 步）：现有测试在两种构建下都能编译和运行。
- 任务 4（第 9 步）：CI 作业与 CMake Preset。
- 任务 5（第 10 步）：README 与架构文档、升级说明、设计稿的状态与第 12 节；全配置验证。

不在范围内：MSVC 关掉异常的构建（`CO2_TEST_NO_EXCEPTIONS` 只为 GCC 与 Clang 准备）；基准程序在禁用异常下的构建（它的"失败"一项靠 `throw` 产生错误）。

本计划里的全部代码都在仓库的一份副本上按任务顺序实现并跑通过：

- 任务 1 的新测试在改动前后都通过；临时去掉守卫里的任何一步（不析构参数副本、不析构 promise、不释放帧、不归还超对齐块、成功之后不解除守卫），测试都会失败。
- 任务 2、3 都先确认只加测试时失败、再确认加上实现后通过。
- 最终状态下，开着异常的 13 个配置各 31 个测试、禁用异常的 9 个配置各 32 个测试全部通过（GCC 13 与 Clang 18；Debug / Release、C++14 / 17 / 20 / 23、libc++、ASan + UBSan、TSan）；并发相关的 9 个测试另在 Release 下重复 40 遍、TSan 下重复 5 遍，两种构建都是。
- 开着异常时基准没有可见变化：单线程各项与改动前相差在 0.1 ns 以内；"已停止沿 Task 链"一项快了大约 1 ns（19.6 → 18.6），原因没有查，可能只是代码布局不同。README 里的数字不动。

MSVC 没有验证；CI 上的新作业只在本机用同样的命令跑过。

实现时与设计稿的字面有四处不同，任务 5 会把它们同步进设计稿：

- 内部回滚没有做成"禁用异常时消失的 `try` / `catch`"，而是改成守卫对象，两种构建同一份代码（任务 1）。
- 随 `CO2_HAS_EXCEPTIONS` 消失的东西比设计稿列的多两项：`GeneratorPromise` 里存异常的成员，以及把异常接成错误的三处 catch（任务 2）。
- 使用者自己定义 `CO2_HAS_EXCEPTIONS` 是编译错误，有一个必须编译失败的测试守着（任务 2）。
- CI 的作业是四个而不是两个：两个编译器各加了 Release（任务 4）。

## 文件一览

| 文件 | 任务 | 职责 |
| --- | --- | --- |
| `include/co2/detail/frame.hpp` | 1、2 | 创建帧的回滚改用守卫；`step` 的 catch 与接 `bad_alloc` 的路加条件编译 |
| `include/co2/detail/over_aligned.hpp` | 1 | 超对齐块构造失败时的回滚改用守卫 |
| `include/co2/thread_pool.hpp` | 1 | 启动线程失败时的回滚改用守卫 |
| `include/co2/config.hpp` | 2 | `CO2_HAS_EXCEPTIONS`；MSVC 的 `detect_mismatch` |
| `include/co2/error.hpp` | 2 | `Error` 装异常的那一半加条件编译；`raise()` 在禁用异常时是契约违规 |
| `include/co2/task.hpp`、`generator.hpp`、`async_generator.hpp` | 2 | `unhandled_exception()` 等加条件编译 |
| `include/co2/as_result.hpp`、`callback.hpp` | 2 | 把异常接成错误的 catch、`setException` 加条件编译 |
| `tests/CMakeLists.txt` | 2 | 选项 `CO2_TEST_NO_EXCEPTIONS`；登记新测试 |
| `tests/no_exceptions_*_contract_violation_test.cpp`（新，3 个） | 2 | 只在禁用异常时成立的契约违规 |
| `tests/has_exceptions_override_compile_fail_test.cpp`（新） | 2 | 手动定义 `CO2_HAS_EXCEPTIONS` 必须编不过 |
| `tests/conformance_tests.cpp`、`allocation_tests.cpp` | 1、3 | 回滚路径的测试；依赖异常的段落加条件编译 |
| `tests/completion_support.hpp` | 3 | `sample::outcomeOf`、`sample::carriesAnException` |
| 其余 `tests/*_tests.cpp`、`tests/trace.hpp` | 3 | 依赖异常的段落加条件编译 |
| `.github/workflows/ci.yml`、`CMakePresets.json` | 4 | 禁用异常的作业与 Preset |
| `README.md`、`docs/architecture.md`、`docs/design/completion-channels.md` | 5 | 文档 |

## 环境与命令

**构建与测试。** "开着异常的构建"指默认配置：

```bash
cmake -S . -B /tmp/co2-build/gcc14 -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=g++
cmake --build /tmp/co2-build/gcc14 -j 12
ctest --test-dir /tmp/co2-build/gcc14 -j 8
```

"禁用异常的构建"多一个选项，GCC 与 Clang 各一份：

```bash
cmake -S . -B /tmp/co2-build/gcc14-noexc -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=g++ \
      -DCO2_TEST_NO_EXCEPTIONS=ON
cmake --build /tmp/co2-build/gcc14-noexc -j 12
ctest --test-dir /tmp/co2-build/gcc14-noexc -j 8

cmake -S . -B /tmp/co2-build/clang14-noexc -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=clang++ \
      -DCO2_TEST_NO_EXCEPTIONS=ON
cmake --build /tmp/co2-build/clang14-noexc -j 12
ctest --test-dir /tmp/co2-build/clang14-noexc -j 8
```

每组的第一条只需要在第一次、以及新增了测试文件之后执行。这台机器上的 ctest 在没有失败时打印 `100% tests passed out of N`。

单独运行一个测试程序（失败时它会打印出错的行号）：

```bash
/tmp/co2-build/gcc14-noexc/tests/co2-completion-tests
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

### Task 1: 初始化中途失败的回滚改用守卫对象

库里有三处"构造到一半失败就撤销"的代码用的是 `try` / `catch`：创建帧的三步（参数副本 → promise → 局部）、超对齐块里构造对象、线程池启动线程。`-fno-exceptions` 下它们编不过。给它们加条件编译是一种办法，但那样两种构建就是两份代码；改成局部的守卫对象（析构函数里撤销，成功之后解除），同一份代码两种构建都能编译，行为不变。

这几条回滚路径此前没有测试覆盖，所以先补测试，在旧实现上确认通过，再改实现。

**Files:**
- Modify: `tests/conformance_tests.cpp`
- Modify: `tests/allocation_tests.cpp`
- Modify: `include/co2/detail/frame.hpp`
- Modify: `include/co2/detail/over_aligned.hpp`
- Modify: `include/co2/thread_pool.hpp`

**Interfaces:**
- Consumes: `CoroutineFrame::create`、`HeapObject<T>::createImpl`、`ThreadPool` 的构造函数（都是现有代码）；测试用到 `co2::detail::AwaitSlot<>`。
- Produces: 没有接口变化。任务 2 依赖的是这一点：这三处从此不含 `try` / `catch`，不需要条件编译。

- [ ] **Step 1: 写测试**

两个测试描述的都是现有行为。帧的那一个让三步各自抛出，检查已经构造的部分按相反的顺序析构、帧被释放（用 promise 的 `operator new` / `operator delete` 计数）。超对齐的那一个先看构造成功时 `reset()` 归还那一块，再看构造抛出时那一块当场归还、槽保持为空。

修改 `tests/conformance_tests.cpp`：

````diff
diff --git a/tests/conformance_tests.cpp b/tests/conformance_tests.cpp
index 40ee58f..a1a9418 100644
--- a/tests/conformance_tests.cpp
+++ b/tests/conformance_tests.cpp
@@ -256,6 +256,113 @@ void exceptionsBeforeTheInitialAwaitResumePropagateToTheCaller() {
     }
 }
 
+// ---------------------------------------------------------------------------
+// 帧的三步构造（参数副本 → promise → 局部）里任何一步抛出：已经构造好的部分按相反的
+// 顺序析构，帧被释放，异常抛给调用方，协程体从未执行。
+
+int liveFrames = 0;
+
+// 移进帧时可以抛出的参数。
+struct ThrowsOnMove {
+    explicit ThrowsOnMove(bool const armed_) noexcept : armed{armed_} {}
+    ThrowsOnMove(ThrowsOnMove&& other) : armed{other.armed} {
+        if (armed) throw std::runtime_error{"parameter copy"};
+    }
+
+    bool armed;
+};
+
+// 帧创建时默认构造、可以抛出的局部。
+bool localsThrow = false;
+
+struct ThrowsOnConstruction {
+    ThrowsOnConstruction() {
+        if (localsThrow) throw std::runtime_error{"local"};
+    }
+};
+
+// promise 用自己的 operator new / delete 统计存活的帧。
+struct CountedFrameTask {
+    struct promise_type {
+        promise_type(EventLog& log_, bool const throws, Tracked&, ThrowsOnMove&)
+            : log{&log_} {
+            if (throws) throw std::runtime_error{"promise"};
+            log->add("promise constructed");
+        }
+        ~promise_type() { log->add("promise destroyed"); }
+
+        static void* operator new(std::size_t const size) {
+            ++liveFrames;
+            return ::operator new(size);
+        }
+        static void operator delete(void* const pointer) noexcept {
+            --liveFrames;
+            ::operator delete(pointer);
+        }
+
+        CountedFrameTask get_return_object() noexcept { return {}; }
+        co2::suspend_never initial_suspend() noexcept { return {}; }
+        co2::suspend_never final_suspend() noexcept { return {}; }
+        void return_void() noexcept {}
+        void unhandled_exception() { log->add("unhandled_exception"); }
+
+        EventLog* log;
+    };
+};
+
+auto buildsAFrame(EventLog& log, bool promiseThrows, Tracked first, ThrowsOnMove second)
+    CO2_BEG(CountedFrameTask, (log, promiseThrows, first, second),
+            ThrowsOnConstruction local;) {
+    log.add("body");
+}
+CO2_END
+
+bool throwsWhileBuildingAFrame(EventLog& log, bool const promiseThrows,
+                               bool const parameterThrows) {
+    try {
+        buildsAFrame(log, promiseThrows, Tracked{log, "first"},
+                     ThrowsOnMove{parameterThrows});
+    } catch (std::runtime_error const&) {
+        return true;
+    }
+    return false;
+}
+
+void aFrameThatFailsHalfwayThroughConstructionIsRolledBack() {
+    {
+        // 第二个参数移进帧时抛出：第一个参数副本已经构造好，要析构。
+        EventLog log;
+        CHECK(throwsWhileBuildingAFrame(log, false, true));
+        EXPECT_EVENTS(log, "first constructed", "first destroyed");
+        CHECK(liveFrames == 0);
+    }
+    {
+        // promise 的构造函数抛出：参数副本要析构；promise 没有构造成，不析构。
+        EventLog log;
+        CHECK(throwsWhileBuildingAFrame(log, true, false));
+        EXPECT_EVENTS(log, "first constructed", "first destroyed");
+        CHECK(liveFrames == 0);
+    }
+    {
+        // 局部的构造函数抛出：promise 已经构造好，先析构它，再析构参数副本。
+        EventLog log;
+        localsThrow = true;
+        CHECK(throwsWhileBuildingAFrame(log, false, false));
+        localsThrow = false;
+        EXPECT_EVENTS(log, "first constructed", "promise constructed",
+                      "promise destroyed", "first destroyed");
+        CHECK(liveFrames == 0);
+    }
+    {
+        // 对照：什么都不抛，协程照常运行到结束。
+        EventLog log;
+        CHECK(not throwsWhileBuildingAFrame(log, false, false));
+        EXPECT_EVENTS(log, "first constructed", "promise constructed", "body",
+                      "promise destroyed", "first destroyed");
+        CHECK(liveFrames == 0);
+    }
+}
+
 // initial awaiter 的 await_resume 抛出不属于"初始挂起点之前"：标准在求值 await-resume
 // 之前就把 initial-await-resume-called 置真（[dcl.fct.def.coroutine]/5），所以异常进入
 // unhandled_exception，协程走到 final suspend，帧仍由拥有者销毁。
@@ -1131,6 +1238,7 @@ int main() {
     localsAreDestroyedBeforeUnhandledException();
     rethrowingUnhandledExceptionLeavesTheCoroutineAtFinalSuspend();
     exceptionsBeforeTheInitialAwaitResumePropagateToTheCaller();
+    aFrameThatFailsHalfwayThroughConstructionIsRolledBack();
     initialAwaitResumeExceptionsReachUnhandledException();
     awaitSuspendReturningVoidSuspends();
     readyAwaiterNeverSuspends();
````

修改 `tests/allocation_tests.cpp`：

````diff
diff --git a/tests/allocation_tests.cpp b/tests/allocation_tests.cpp
index 1b17b6a..a676389 100644
--- a/tests/allocation_tests.cpp
+++ b/tests/allocation_tests.cpp
@@ -50,9 +50,14 @@ namespace {
 
 std::atomic<long> allocations{0};
 std::atomic<long> allocatedBytes{0};
+std::atomic<long> deallocations{0};
 
 long allocationsSoFar() noexcept { return allocations.load(std::memory_order_relaxed); }
 
+long deallocationsSoFar() noexcept {
+    return deallocations.load(std::memory_order_relaxed);
+}
+
 long bytesSoFar() noexcept { return allocatedBytes.load(std::memory_order_relaxed); }
 
 // 运行 `body` 期间经全局 operator new 分配的字节数。
@@ -365,6 +370,48 @@ void spawnIsTwoAllocationsBeyondTheFrame() {
     });
 }
 
+// 超对齐的 awaiter 在堆上手工对齐：先要一整块，再在块里构造。reset() 归还那一块；构造
+// 抛出时那一块也必须归还，槽保持为空。异常类型是空的结构体：std::runtime_error 自己会
+// 分配，干扰计数。
+struct ConstructionFailed {};
+
+struct alignas(64) OverAlignedThrowsOnMove {
+    explicit OverAlignedThrowsOnMove(bool const armed_) noexcept : armed{armed_} {}
+    OverAlignedThrowsOnMove(OverAlignedThrowsOnMove&& other) : armed{other.armed} {
+        if (armed) throw ConstructionFailed{};
+    }
+
+    bool armed;
+};
+
+void anOverAlignedAwaiterAlwaysReturnsItsBlock() {
+    static_assert(not co2::detail::AwaitSlot<>::isInline<OverAlignedThrowsOnMove>(),
+                  "over-aligned awaiters never stay inline");
+    co2::detail::AwaitSlot<> slot;
+    auto const allocationsBefore = allocationsSoFar();
+    auto const deallocationsBefore = deallocationsSoFar();
+
+    // 构造成功：那一块留在槽里，reset() 归还。
+    slot.emplace<OverAlignedThrowsOnMove>(OverAlignedThrowsOnMove{false});
+    CHECK(slot.hasValue());
+    CHECK(allocationsSoFar() - allocationsBefore == 1);
+    CHECK(deallocationsSoFar() - deallocationsBefore == 0);
+    slot.reset();
+    CHECK(deallocationsSoFar() - deallocationsBefore == 1);
+
+    // 构造抛出：那一块当场归还，槽保持为空。
+    auto thrown = false;
+    try {
+        slot.emplace<OverAlignedThrowsOnMove>(OverAlignedThrowsOnMove{true});
+    } catch (ConstructionFailed const&) {
+        thrown = true;
+    }
+    CHECK(thrown);
+    CHECK(not slot.hasValue());
+    CHECK(allocationsSoFar() - allocationsBefore == 2);
+    CHECK(deallocationsSoFar() - deallocationsBefore == 2);
+}
+
 } // namespace
 
 // 计数的全局 operator new/delete。
@@ -383,8 +430,15 @@ void* operator new(std::size_t const size) {
     throw std::bad_alloc{};
 }
 
-void operator delete(void* const pointer) noexcept { std::free(pointer); }
-void operator delete(void* const pointer, std::size_t) noexcept { std::free(pointer); }
+void operator delete(void* const pointer) noexcept {
+    if (pointer != nullptr) deallocations.fetch_add(1, std::memory_order_relaxed);
+    std::free(pointer);
+}
+
+void operator delete(void* const pointer, std::size_t) noexcept {
+    if (pointer != nullptr) deallocations.fetch_add(1, std::memory_order_relaxed);
+    std::free(pointer);
+}
 
 #if defined(__GNUC__)
 #pragma GCC diagnostic pop
@@ -414,6 +468,7 @@ int main() {
     oversizedCallbackAwaiterFallsBackToOneHeapAllocation();
     callbackAwaiterKeepsAThreePointerClosureInline();
     framesCarryNoUnusedFixedOverhead();
+    anOverAlignedAwaiterAlwaysReturnsItsBlock();
     spawnIsTwoAllocationsBeyondTheFrame();
     return 0;
 }
````

- [ ] **Step 2: 构建并运行，确认通过**

开着异常的构建，运行全部测试。

Expected: `100% tests passed out of 30`。新测试描述的是现有行为，在旧实现上就应当通过；不通过说明测试写错了，先改测试。

- [ ] **Step 3: 改实现**

修改 `include/co2/detail/frame.hpp`：

````diff
diff --git a/include/co2/detail/frame.hpp b/include/co2/detail/frame.hpp
index 54623d2..b9c95e7 100644
--- a/include/co2/detail/frame.hpp
+++ b/include/co2/detail/frame.hpp
@@ -499,29 +499,27 @@ struct CoroutineFrame
         frame->core.suspendPoint = SuspendPoint{};
         frame->core.bodyAlive = false;
 
-        try {
-            makeParams(static_cast<void*>(frame->core.paramsStorage));
-        } catch (...) {
-            release(frame);
-            throw;
-        }
-        try {
-            constructPromiseFor<Promise>(frame->core.params(),
-                                         frame->core.prefix.promise, 0);
-        } catch (...) {
-            frame->core.params().~Params();
-            release(frame);
-            throw;
-        }
-        try {
-            ::new (static_cast<void*>(frame->core.bodyStorage))
-                Body(frame->core.params());
-        } catch (...) {
-            frame->core.promise().~Promise();
-            frame->core.params().~Params();
-            release(frame);
-            throw;
-        }
+        // 三步构造中的任何一步抛出：按相反的顺序撤销已经完成的部分，释放帧，异常继续
+        // 向外。用守卫而不是 try / catch：同一份代码在禁用异常的构建里也能编译。
+        struct Rollback {
+            CoroutineFrame* frame;
+            bool paramsBuilt;
+            bool promiseBuilt;
+            ~Rollback() {
+                if (frame == nullptr) return;
+                if (promiseBuilt) frame->core.promise().~Promise();
+                if (paramsBuilt) frame->core.params().~Params();
+                release(frame);
+            }
+        } rollback{frame, false, false};
+
+        makeParams(static_cast<void*>(frame->core.paramsStorage));
+        rollback.paramsBuilt = true;
+        constructPromiseFor<Promise>(frame->core.params(), frame->core.prefix.promise, 0);
+        rollback.promiseBuilt = true;
+        ::new (static_cast<void*>(frame->core.bodyStorage)) Body(frame->core.params());
+        rollback.frame = nullptr;
+
         frame->core.bodyAlive = true;
         return frame;
     }
````

修改 `include/co2/detail/over_aligned.hpp`：

````diff
diff --git a/include/co2/detail/over_aligned.hpp b/include/co2/detail/over_aligned.hpp
index 84cdd2a..d990de1 100644
--- a/include/co2/detail/over_aligned.hpp
+++ b/include/co2/detail/over_aligned.hpp
@@ -69,13 +69,18 @@ template <class T> struct HeapObject {
     template <class Value> static T* createImpl(Value&& value, std::true_type) {
         void* const block =
             ::operator new(overAlignedBlockSize(sizeof(T), alignof(T)));
-        try {
-            return ::new (placeOverAligned(block, sizeof(T), alignof(T)))
-                T(std::forward<Value>(value));
-        } catch (...) {
-            ::operator delete(block);
-            throw;
-        }
+        // T 的构造抛出时归还整块。用守卫而不是 try / catch：同一份代码在禁用异常的
+        // 构建里也能编译。
+        struct ReleaseOnFailure {
+            void* block;
+            ~ReleaseOnFailure() {
+                if (block != nullptr) ::operator delete(block);
+            }
+        } guard{block};
+        auto* const object = ::new (placeOverAligned(block, sizeof(T), alignof(T)))
+            T(std::forward<Value>(value));
+        guard.block = nullptr;
+        return object;
     }
 
     static void destroyImpl(T* const object, std::false_type) noexcept {
````

修改 `include/co2/thread_pool.hpp`：

````diff
diff --git a/include/co2/thread_pool.hpp b/include/co2/thread_pool.hpp
index 6ac1995..2e4118d 100644
--- a/include/co2/thread_pool.hpp
+++ b/include/co2/thread_pool.hpp
@@ -205,16 +205,21 @@ struct ThreadPool final : Scheduler {
         for (auto index = 0U; index != threadCount; ++index)
             workers.push_back(
                 std::unique_ptr<Worker>{new Worker{*this, index, threadCount}});
-        try {
-            for (auto& worker : workers) {
-                auto* const target = worker.get();
-                target->thread = std::thread{[this, target] { workerMain(*target); }};
+        // 线程起到一半失败（std::thread 的构造抛出）：停下并回收已经起来的线程，异常
+        // 继续向外。用守卫而不是 try / catch：同一份代码在禁用异常的构建里也能编译。
+        struct StopOnFailure {
+            ThreadPool* pool;
+            ~StopOnFailure() {
+                if (pool == nullptr) return;
+                pool->requestStop();
+                pool->joinWorkers();
             }
-        } catch (...) {
-            requestStop();
-            joinWorkers();
-            throw;
+        } guard{this};
+        for (auto& worker : workers) {
+            auto* const target = worker.get();
+            target->thread = std::thread{[this, target] { workerMain(*target); }};
         }
+        guard.pool = nullptr;
     }
 
     ~ThreadPool() override {
````

- [ ] **Step 4: 构建并运行全部测试，确认仍然通过**

Expected: `100% tests passed out of 30`。

- [ ] **Step 5: 确认测试真的守着这些路径**

逐个做下面两处临时改动，每次重新构建并运行对应的测试，然后改回来：

1. `include/co2/detail/frame.hpp` 的 `Rollback::~Rollback()` 里删掉 `release(frame);` 一行。
   Expected: `co2-conformance-tests` 失败。
2. `include/co2/detail/over_aligned.hpp` 的 `ReleaseOnFailure::~ReleaseOnFailure()` 里把 `::operator delete(block);` 换成 `static_cast<void>(block);`。
   Expected: `co2-allocation-tests` 失败。

改回来之后 `git diff --stat` 应当只列出本任务的 5 个文件，重新构建并运行全部测试，仍是 `100% tests passed out of 30`。

线程池那一处在测试里覆盖不到（要让 `std::thread` 的构造失败）。写计划时用一次性的程序把进程的线程数上限压低，对比过改动前后的行为：一个线程都起不来、起到一半失败，两种情况下构造函数都抛出 `std::system_error`，已经起来的线程被回收，改动前后相同。那个程序没有留在仓库里。

- [ ] **Step 6: 提交**

```bash
git add include/co2/detail/frame.hpp include/co2/detail/over_aligned.hpp \
        include/co2/thread_pool.hpp tests/conformance_tests.cpp tests/allocation_tests.cpp
git commit -F - <<'EOF'
初始化中途失败的回滚改用守卫对象

创建帧的三步（参数副本、promise、局部）、超对齐块里构造对象、线程池启动线程，这三处
原来用 try / catch 回滚。改成局部的守卫对象：行为不变，但同一份代码在
-fno-exceptions 下也能编译。这是为禁用异常的构建做准备，省得在这三处加条件编译、
两种构建各走一份代码。

这几条回滚路径此前没有测试，所以先补上：帧的三步各自抛出时，已经构造的部分按相反
的顺序析构、帧被释放；超对齐的 awaiter 构造抛出时归还整块。新测试在改动前后都
通过；临时去掉守卫里的任何一步，测试会失败。线程池那一处在测试里覆盖不到，用一次性
的程序（压低线程数上限，让 std::thread 的构造失败）对比过改动前后的行为，相同。
EOF
```

### Task 2: `CO2_HAS_EXCEPTIONS` 与库里的条件编译

设计稿第 5 节。`config.hpp` 从编译器的开关得出 `CO2_HAS_EXCEPTIONS`；为 0 时库里不出现 `try` / `catch` / `throw`，`Error::raise()` 是契约违规。值、错误码、已停止三条通道不动。

这个任务结束时，库的头文件在 `-fno-exceptions -fno-rtti` 下全部能编译，新增的契约违规测试通过；但现有的测试源码还依赖 `throw` / `catch`，打开选项后的完整构建要到任务 3 才能通过。

**Files:**
- Create: `tests/no_exceptions_error_at_root_contract_violation_test.cpp`
- Create: `tests/no_exceptions_error_without_entry_contract_violation_test.cpp`
- Create: `tests/no_exceptions_error_through_legacy_awaiter_contract_violation_test.cpp`
- Create: `tests/has_exceptions_override_compile_fail_test.cpp`
- Modify: `tests/CMakeLists.txt`
- Modify: `include/co2/config.hpp`
- Modify: `include/co2/error.hpp`
- Modify: `include/co2/detail/frame.hpp`
- Modify: `include/co2/task.hpp`
- Modify: `include/co2/generator.hpp`
- Modify: `include/co2/async_generator.hpp`
- Modify: `include/co2/as_result.hpp`
- Modify: `include/co2/callback.hpp`

**Interfaces:**
- Consumes: 任务 1（三处回滚不含 `try` / `catch`）；`co2::Error`、`co2::Completion`、`co2::asResult`、`co2::just_error`（计划一、二）。
- Produces:
  - 宏 `CO2_HAS_EXCEPTIONS`（`co2/config.hpp`），值是 `0` 或 `1`，可以用在 `#if` 里。使用者定义它是编译错误。
  - 为 `0` 时：`co2::Error::exception()`、`explicit Error(std::exception_ptr)`、`Continuation<T>::setException` 不存在；promise 不需要 `unhandled_exception()`；`Error::raise()` 经 `CO2_CONTRACT_FAIL` 报告契约违规（文件名是 `error.hpp`）。
  - CMake 选项 `CO2_TEST_NO_EXCEPTIONS`（默认 `OFF`）：给全部测试目标加 `-fno-exceptions -fno-rtti`；与 MSVC 一起用是配置错误。

- [ ] **Step 1: 写测试与 CMake 选项**

三个契约违规测试只在禁用异常时构建，各对应一处"错误无处可抛"：根、没有 `return_error()` 的 promise、只转发标准三个成员的包装 awaiter。它们在开着异常的构建里会 `#error`，免得被误加进去。第四个是编译失败测试：手动定义 `CO2_HAS_EXCEPTIONS` 必须编不过。

新建 `tests/no_exceptions_error_at_root_contract_violation_test.cpp`：

````cpp
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <system_error>

#include "co2/as_result.hpp"
#include "co2/config.hpp"
#include "co2/coroutine.hpp"
#include "co2/just.hpp"
#include "co2/sync_wait.hpp"
#include "co2/task.hpp"

#if CO2_HAS_EXCEPTIONS
#error "this test is for builds with exceptions disabled (CO2_TEST_NO_EXCEPTIONS)"
#endif

namespace {

std::atomic<bool> atExpectedFailurePoint{};

// 预期的违规由 Error::raise() 报告（error.hpp）。
void contractViolationHandler(co2::ContractViolation const& violation) {
    auto const expected = atExpectedFailurePoint.load(std::memory_order_acquire) &&
                          std::strstr(violation.file, "error.hpp") != nullptr;
    std::_Exit(expected ? 86 : 87);
}

auto fails() CO2_BEG(co2::Task<int>, ()) {
    CO2_AWAIT(co2::just_error(std::make_error_code(std::errc::timed_out)));
    CO2_RETURN(0);
}
CO2_END

} // namespace

// 禁用异常的构建里，错误到达根（syncWait / join）是契约违规：没有异常可以抛。想在根上
// 拿到错误，写 syncWait(asResult(task))——它把三种结果都作为值交出来，不终止程序。
int main() {
    co2::setContractViolationHandler(&contractViolationHandler);

    auto const outcome = co2::syncWait(co2::asResult(fails()));
    if (not outcome.has_value() || not outcome->hasError()) return EXIT_FAILURE;
    if (outcome->error().code() != std::errc::timed_out) return EXIT_FAILURE;

    atExpectedFailurePoint.store(true, std::memory_order_release);
    co2::syncWait(fails());
    return EXIT_FAILURE;
}
````

新建 `tests/no_exceptions_error_without_entry_contract_violation_test.cpp`：

````cpp
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <system_error>

#include "co2/completion.hpp"
#include "co2/config.hpp"
#include "co2/coroutine.hpp"

#if CO2_HAS_EXCEPTIONS
#error "this test is for builds with exceptions disabled (CO2_TEST_NO_EXCEPTIONS)"
#endif

namespace {

std::atomic<bool> atExpectedFailurePoint{};

// 预期的违规由 Error::raise() 报告（error.hpp）。
void contractViolationHandler(co2::ContractViolation const& violation) {
    auto const expected = atExpectedFailurePoint.load(std::memory_order_acquire) &&
                          std::strstr(violation.file, "error.hpp") != nullptr;
    std::_Exit(expected ? 86 : 87);
}

// 立刻就绪，以给定的方式结束。
struct Ends {
    co2::Completion completion;

    bool await_ready() const noexcept { return true; }
    void await_suspend(co2::coroutine_handle<>) const noexcept {}
    void await_resume() const noexcept {}
    co2::Completion await_completion() noexcept { return completion; }
};

// 为标准写的 promise：没有 return_error()。禁用异常的构建里它也不需要
// unhandled_exception()。
struct Plain {
    struct promise_type {
        promise_type() = default;

        Plain get_return_object() noexcept { return {}; }
        co2::suspend_never initial_suspend() noexcept { return {}; }
        co2::suspend_never final_suspend() noexcept { return {}; }
        void return_void() noexcept {}
    };
};

auto endsWith(co2::Completion completion) CO2_BEG(Plain, (completion)) {
    CO2_AWAIT(Ends{completion});
}
CO2_END

} // namespace

// 开异常时，错误被送到一个没有 return_error() 的 promise，会在协程体内重新抛出，走
// unhandled_exception()。禁用异常的构建里没有这条退路：契约违规。
int main() {
    co2::setContractViolationHandler(&contractViolationHandler);
    endsWith(co2::Completion::value());

    atExpectedFailurePoint.store(true, std::memory_order_release);
    endsWith(co2::Completion::error(std::make_error_code(std::errc::timed_out)));
    return EXIT_FAILURE;
}
````

新建 `tests/no_exceptions_error_through_legacy_awaiter_contract_violation_test.cpp`：

````cpp
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <system_error>

#include "co2/config.hpp"
#include "co2/coroutine.hpp"
#include "co2/just.hpp"
#include "co2/sync_wait.hpp"
#include "co2/task.hpp"

#if CO2_HAS_EXCEPTIONS
#error "this test is for builds with exceptions disabled (CO2_TEST_NO_EXCEPTIONS)"
#endif

namespace {

std::atomic<bool> atExpectedFailurePoint{};

// 预期的违规由 Error::raise() 报告（error.hpp）。
void contractViolationHandler(co2::ContractViolation const& violation) {
    auto const expected = atExpectedFailurePoint.load(std::memory_order_acquire) &&
                          std::strstr(violation.file, "error.hpp") != nullptr;
    std::_Exit(expected ? 86 : 87);
}

auto mayFail(bool fail) CO2_BEG(co2::Task<int>, (fail)) {
    if (fail) CO2_AWAIT(co2::just_error(std::make_error_code(std::errc::timed_out)));
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

auto awaitsThroughAWrapper(bool fail) CO2_BEG(co2::Task<int>, (fail), int value{};) {
    CO2_AWAIT_SET(value, Forwards{mayFail(fail)});
    CO2_RETURN(value);
}
CO2_END

} // namespace

// 开异常时，不认识 await_completion() 的调用方从 await_resume() 拿到异常。禁用异常的
// 构建里 await_resume() 没有办法报告错误：契约违规。包装 awaiter 需要把
// await_completion() 也转发出去。
int main() {
    co2::setContractViolationHandler(&contractViolationHandler);
    if (co2::syncWait(awaitsThroughAWrapper(false)) != 1) return EXIT_FAILURE;

    atExpectedFailurePoint.store(true, std::memory_order_release);
    co2::syncWait(awaitsThroughAWrapper(true));
    return EXIT_FAILURE;
}
````

新建 `tests/has_exceptions_override_compile_fail_test.cpp`：

````cpp
// 编译失败测试：CO2_HAS_EXCEPTIONS 不能由使用者指定。
//
// 它由编译器的异常开关得出（config.hpp）。手动定成与开关不一致的值，库里要么留下编不过的
// try / catch，要么去掉了回滚用的 catch 而泄漏资源；所以一经定义就报错，而不是采信。
//
// 定义 CO2_EXPECT_COMPILE_FAILURE 时本文件必须编不过；不定义时是对照，必须能编过。

#ifdef CO2_EXPECT_COMPILE_FAILURE
#define CO2_HAS_EXCEPTIONS 1
#endif

#include "co2/config.hpp"

static_assert(CO2_HAS_EXCEPTIONS == 0 || CO2_HAS_EXCEPTIONS == 1,
              "CO2_HAS_EXCEPTIONS is either 0 or 1");
````

修改 `tests/CMakeLists.txt`：

````diff
diff --git a/tests/CMakeLists.txt b/tests/CMakeLists.txt
index c6d5642..64b76d0 100644
--- a/tests/CMakeLists.txt
+++ b/tests/CMakeLists.txt
@@ -28,8 +28,26 @@ set(co2_contract_violation_test_sources
     optional_empty_value_contract_violation_test.cpp
     error_from_success_code_contract_violation_test.cpp
     stopped_without_entry_contract_violation_test.cpp
-    stopped_through_legacy_awaiter_contract_violation_test.cpp
-    task_error_taken_twice_contract_violation_test.cpp)
+    stopped_through_legacy_awaiter_contract_violation_test.cpp)
+
+# 禁用异常的构建。打开之后，全部测试以 -fno-exceptions -fno-rtti 编译。库自己从编译器的
+# 开关判断有没有异常（CO2_HAS_EXCEPTIONS，见 config.hpp），这里不传任何宏给它。
+option(CO2_TEST_NO_EXCEPTIONS "Build the co2 tests with exceptions and RTTI disabled" OFF)
+if(CO2_TEST_NO_EXCEPTIONS AND MSVC)
+    message(FATAL_ERROR "CO2_TEST_NO_EXCEPTIONS is only set up for GCC and Clang")
+endif()
+
+if(CO2_TEST_NO_EXCEPTIONS)
+    # 只在没有异常时才成立的契约违规：错误无处可抛。
+    list(APPEND co2_contract_violation_test_sources
+         no_exceptions_error_at_root_contract_violation_test.cpp
+         no_exceptions_error_without_entry_contract_violation_test.cpp
+         no_exceptions_error_through_legacy_awaiter_contract_violation_test.cpp)
+else()
+    # 这一个测的是"带着异常的错误被取两次"，没有异常时无从谈起。
+    list(APPEND co2_contract_violation_test_sources
+         task_error_taken_twice_contract_violation_test.cpp)
+endif()
 
 # 构建测试用的 C++ 标准。库的最低要求是 14，默认就测它；CI 另以 17 / 20 / 23 各跑一遍，
 # 保证较新的标准下（弃用告警、更严格的规则）同样干净。
@@ -67,6 +85,9 @@ function(co2_configure_test_target target)
                 -Werror=uninitialized
                 -Werror=suggest-override
                 -Werror=vla)
+        if(CO2_TEST_NO_EXCEPTIONS)
+            target_compile_options(${target} PRIVATE -fno-exceptions -fno-rtti)
+        endif()
     endif()
 endfunction()
 
@@ -98,7 +119,8 @@ endforeach()
 # 宏的对照目标随正常构建一起编译，证明失败只来自被测的那一处，而不是别的笔误。
 set(co2_compile_fail_test_sources
     await_inside_switch_compile_fail_test.cpp
-    member_macro_outside_member_function_compile_fail_test.cpp)
+    member_macro_outside_member_function_compile_fail_test.cpp
+    has_exceptions_override_compile_fail_test.cpp)
 
 foreach(co2_test_source IN LISTS co2_compile_fail_test_sources)
     co2_test_target_name("${co2_test_source}" co2_test_target)
@@ -133,8 +155,9 @@ endforeach()
 add_library(co2-header-self-containment-tests OBJECT ${co2_header_test_sources})
 co2_configure_test_target(co2-header-self-containment-tests)
 
-# 与原生 co_await 的差分测试只在编译器支持标准协程时构建。
-if("cxx_std_20" IN_LIST CMAKE_CXX_COMPILE_FEATURES)
+# 与原生 co_await 的差分测试只在编译器支持标准协程时构建。它比对的事件序列里有异常
+# 路径，禁用异常的构建不参加。
+if("cxx_std_20" IN_LIST CMAKE_CXX_COMPILE_FEATURES AND NOT CO2_TEST_NO_EXCEPTIONS)
     foreach(co2_cpp20_source IN ITEMS differential_tests.cpp)
         co2_test_target_name("${co2_cpp20_source}" co2_test_target)
         add_executable(${co2_test_target} ${co2_cpp20_source})
````

- [ ] **Step 2: 构建，确认失败**

开着异常的构建（新增了测试文件，要重新执行配置那一条）。

Expected: 构建失败，报在 `tests/has_exceptions_override_compile_fail_test.cpp`：`'CO2_HAS_EXCEPTIONS' was not declared in this scope`——对照目标要求这个宏存在。

再配置禁用异常的构建（GCC），只构建头文件自包含的目标：

```bash
cmake -S . -B /tmp/co2-build/gcc14-noexc -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=g++ \
      -DCO2_TEST_NO_EXCEPTIONS=ON
cmake --build /tmp/co2-build/gcc14-noexc -j 12 --target co2-header-self-containment-tests
```

Expected: 构建失败，报在库的头文件里：`exception handling disabled, use '-fexceptions' to enable`。

- [ ] **Step 3: 实现**

`config.hpp` 定义这个宏；其余各处把与异常直接相关的代码放进 `#if CO2_HAS_EXCEPTIONS`。

修改 `include/co2/config.hpp`：

````diff
diff --git a/include/co2/config.hpp b/include/co2/config.hpp
index 9d1b5d8..9842072 100644
--- a/include/co2/config.hpp
+++ b/include/co2/config.hpp
@@ -21,6 +21,34 @@
 #error "co2 requires C++14 or newer"
 #endif
 
+// CO2_HAS_EXCEPTIONS：这个翻译单元有没有异常，由编译器的开关决定（GCC / Clang 的
+// -fno-exceptions，MSVC 不带 /EH）。为 0 时库里不出现 try / catch / throw：
+//   - 协程体变换里的 catch 不存在，promise 不需要 unhandled_exception()；
+//   - co2::Error 只装错误码，没有装异常的那一半，Continuation::setException 也没有；
+//   - Error::raise()——错误到达根、或者到达只能靠抛出来报告它的地方——是契约违规。
+// 值、错误码、已停止这三种完成方式在两种构建下是同一份代码
+// （docs/design/completion-channels.md）。
+//
+// 它不能手动指定：关着异常却让库保留 catch 没有意义，开着异常却让库去掉 catch 则是
+// 资源泄漏。同一个程序的所有翻译单元必须用同一个异常开关编译，否则头文件里的内联函数
+// 就有了两份不同的定义（ODR 违规）；MSVC 上由 detect_mismatch 在链接期检查。
+#if defined(CO2_HAS_EXCEPTIONS)
+#error "CO2_HAS_EXCEPTIONS follows the compiler's exception switch; do not define it"
+#endif
+#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
+#define CO2_HAS_EXCEPTIONS 1
+#else
+#define CO2_HAS_EXCEPTIONS 0
+#endif
+
+#if defined(_MSC_VER)
+#if CO2_HAS_EXCEPTIONS
+#pragma detect_mismatch("co2_exceptions", "enabled")
+#else
+#pragma detect_mismatch("co2_exceptions", "disabled")
+#endif
+#endif
+
 // 代码里用 not / and / or 替代记号。MSVC 只有在 /permissive- 下才把它们当关键字；
 // 默认模式下靠 <iso646.h> 的宏（_MSC_EXTENSIONS 开启时定义）兜底，消费方无需改编译选项。
 // 每个根头文件都包含本文件，保证任何单独包含的头都能拿到这些定义。
````

修改 `include/co2/error.hpp`：

````diff
diff --git a/include/co2/error.hpp b/include/co2/error.hpp
index 9c06ef4..557a4e6 100644
--- a/include/co2/error.hpp
+++ b/include/co2/error.hpp
@@ -14,6 +14,9 @@
 // （std::exception_ptr）。不分配，不需要 RTTI；错误码的两个字段与 exception_ptr 共用
 // 存储，所以在 exception_ptr 只有一个指针宽的平台上它与 std::error_code 一样大。
 //
+// 禁用异常的构建里（CO2_HAS_EXCEPTIONS 为 0）它只装错误码：接收 exception_ptr 的构造
+// 函数与 exception() 不存在，raise() 是契约违规。
+//
 // 用户自己的错误枚举按 <system_error> 的常规方式接入（std::is_error_code_enum 的特化
 // 加上 ADL 能找到的 make_error_code），之后可以直接隐式转换成 Error。
 
@@ -66,27 +69,35 @@ struct Error {
                               std::is_error_code_enum<Enum>::value>::type>
     Error(Enum const code) noexcept : Error{std::error_code{code}} {}
 
+#if CO2_HAS_EXCEPTIONS
     // 前置条件：exception 非空。
     explicit Error(std::exception_ptr exception) noexcept
         : number{0}, kind{Kind::exception} {
         CO2_CONTRACT_CHECK(exception != nullptr);
         ::new (static_cast<void*>(&captured)) std::exception_ptr{std::move(exception)};
     }
+#endif
 
     Error(Error const& other) noexcept : number{other.number}, kind{other.kind} {
-        if (kind == Kind::exception)
+#if CO2_HAS_EXCEPTIONS
+        if (kind == Kind::exception) {
             ::new (static_cast<void*>(&captured)) std::exception_ptr{other.captured};
-        else
-            category = other.category;
+            return;
+        }
+#endif
+        category = other.category;
     }
 
     // 被移动的一方只保证还能析构和赋值，内容不再有意义。
     Error(Error&& other) noexcept : number{other.number}, kind{other.kind} {
-        if (kind == Kind::exception)
+#if CO2_HAS_EXCEPTIONS
+        if (kind == Kind::exception) {
             ::new (static_cast<void*>(&captured))
                 std::exception_ptr{std::move(other.captured)};
-        else
-            category = other.category;
+            return;
+        }
+#endif
+        category = other.category;
     }
 
     Error& operator=(Error other) noexcept {
@@ -96,7 +107,9 @@ struct Error {
     }
 
     ~Error() {
+#if CO2_HAS_EXCEPTIONS
         if (kind == Kind::exception) captured.~exception_ptr();
+#endif
     }
 
     // 装着异常时返回 Errc::unhandled_exception。
@@ -105,15 +118,24 @@ struct Error {
         return std::error_code{number, *category};
     }
 
+#if CO2_HAS_EXCEPTIONS
     // 装着错误码时为空。
     std::exception_ptr exception() const noexcept {
         return kind == Kind::exception ? captured : std::exception_ptr{};
     }
+#endif
 
-    // 变回异常：装着异常就重抛原异常，装着错误码就抛 std::system_error。
+    // 变回异常：装着异常就重抛原异常，装着错误码就抛 std::system_error。禁用异常的
+    // 构建里没有异常可抛，走到这里就是契约违规：错误到达了根，或者到达了一个只能靠
+    // 抛出来报告它的地方。想把错误当值拿到，在等待的地方套 co2::asResult。
     [[noreturn]] void raise() const {
+#if CO2_HAS_EXCEPTIONS
         if (kind == Kind::exception) std::rethrow_exception(captured);
         throw std::system_error{code()};
+#else
+        CO2_CONTRACT_FAIL("an error had to be raised in a build without exceptions; "
+                          "take it as a value with co2::asResult instead");
+#endif
     }
 
   private:
@@ -121,7 +143,9 @@ struct Error {
 
     union {
         std::error_category const* category;
+#if CO2_HAS_EXCEPTIONS
         std::exception_ptr captured;
+#endif
     };
     int number;
     Kind kind;
````

修改 `include/co2/detail/frame.hpp`：

````diff
diff --git a/include/co2/detail/frame.hpp b/include/co2/detail/frame.hpp
index b9c95e7..6b23f57 100644
--- a/include/co2/detail/frame.hpp
+++ b/include/co2/detail/frame.hpp
@@ -33,6 +33,10 @@ CO2_DETAIL_MSVC_WARNING_PUSH_DISABLE(4702)
 // 变换里的 try 块分在两处：initial-await-resume-called 为假的那一段（promise 构造到
 // initial awaiter 的 await_suspend）在 ramp（runRamp）里，异常销毁帧并抛给调用方；为真
 // 的那一段（initial awaiter 的 await_resume 起）在 step 里，异常进入 unhandled_exception。
+//
+// 禁用异常的构建里（CO2_HAS_EXCEPTIONS 为 0）没有这个 try / catch，promise 也不需要
+// unhandled_exception()——与原生编译器在 -fno-exceptions 下的要求一致。前一段的回滚靠
+// 守卫对象而不是 catch，所以两种构建下是同一份代码。
 
 namespace co2 {
 namespace detail {
@@ -474,9 +478,10 @@ struct CoroutineFrame
     using Memory = FrameMemory<CoroutineFrame, Promise, FrameAllocator>;
     using HasFailureObject =
         std::integral_constant<bool, HasAllocationFailureObject<Promise>::value>;
-    // allocator 以 bad_alloc 报告失败；promise 的 operator new 以返回空报告。
+    // allocator 以 bad_alloc 报告失败；promise 的 operator new 以返回空报告。禁用异常的
+    // 构建里前一种无从接住，只剩后一种。
     using CatchesBadAlloc = std::integral_constant<
-        bool, HasAllocationFailureObject<Promise>::value &&
+        bool, CO2_HAS_EXCEPTIONS != 0 && HasAllocationFailureObject<Promise>::value &&
                   not HasPromiseOperatorNew<Promise>::value>;
 
     // 分配并初始化帧：参数副本 → promise（能用参数副本构造就传入）→ 局部（Body）。
@@ -531,6 +536,7 @@ struct CoroutineFrame
         auto* const frame = fromHeader(header);
         auto& core = frame->core;
         core.markRunning();
+#if CO2_HAS_EXCEPTIONS
         try {
             return core.body()(core);
         } catch (...) {
@@ -545,6 +551,9 @@ struct CoroutineFrame
             core.markRunning();
             return core.enterFinalSuspend();
         }
+#else
+        return core.body()(core);
+#endif
     }
 
     // 销毁协程状态：等待中的 awaiter、仍存活的局部（Body）、promise、参数副本，然后
@@ -574,6 +583,7 @@ struct CoroutineFrame
         return Memory::allocate(frameAllocator);
     }
 
+#if CO2_HAS_EXCEPTIONS
     static CoroutineFrame* allocateOrNull(FrameAllocator& frameAllocator,
                                           std::true_type) {
         try {
@@ -582,6 +592,7 @@ struct CoroutineFrame
             return nullptr;
         }
     }
+#endif
 
     // 结束帧对象自身的生命周期并释放内存。
     static void release(CoroutineFrame* const frame) noexcept {
````

修改 `include/co2/task.hpp`：

````diff
diff --git a/include/co2/task.hpp b/include/co2/task.hpp
index 3ca51ac..d901bf2 100644
--- a/include/co2/task.hpp
+++ b/include/co2/task.hpp
@@ -113,8 +113,10 @@ template <class T> struct TaskPromise {
         state = TaskState::value;
     }
 
+#if CO2_HAS_EXCEPTIONS
     // 异常只在这里被捕获一次；之后它是一个按值传的 Error，到根上才重新抛出。
     void unhandled_exception() noexcept { fail(Error{std::current_exception()}); }
+#endif
 
     // 等待的操作出错 / 已停止（或者体内 CO2_AWAIT(just_error(e)) 之类）：本 Task 以同样的
     // 结果结束。由恢复点在协程体收尾之后调用，见 FrameCore::finishUnlessValue。
@@ -395,11 +397,15 @@ struct TaskAccess {
 
     // 取值时 T 的移动构造抛出：与 asResult 的其他路径一样接成错误。
     template <class T> static Result<T> valueAsResult(Task<T>& task, std::false_type) {
+#if CO2_HAS_EXCEPTIONS
         try {
             return Result<T>{takeResult(task)};
         } catch (...) {
             return Result<T>{Error{std::current_exception()}};
         }
+#else
+        return Result<T>{takeResult(task)};
+#endif
     }
 
     static Result<void> valueAsResult(Task<void>& task, std::true_type) {
````

修改 `include/co2/generator.hpp`：

````diff
diff --git a/include/co2/generator.hpp b/include/co2/generator.hpp
index aeaf703..6170974 100644
--- a/include/co2/generator.hpp
+++ b/include/co2/generator.hpp
@@ -91,7 +91,9 @@ template <class Ref, class V> struct GeneratorPromise {
 
     void return_void() noexcept {}
 
+#if CO2_HAS_EXCEPTIONS
     void unhandled_exception() noexcept { error = std::current_exception(); }
+#endif
 
     // std::generator 删除 await_transform：生成器体内不允许 co_await。
     template <class Awaitable>
@@ -168,9 +170,12 @@ template <class Ref, class V> struct GeneratorPromise {
             return child;
         }
 
+        // 内层逃出的异常在这里（父层的 elements_of 表达式处）重新抛出。
         void await_resume() {
+#if CO2_HAS_EXCEPTIONS
             auto& childPromise = nested.handle.promise();
             if (childPromise.error) std::rethrow_exception(childPromise.error);
+#endif
         }
     };
 
@@ -180,11 +185,15 @@ template <class Ref, class V> struct GeneratorPromise {
                                          std::is_same<Range, Generator<Ref, V>>{})};
     }
 
+    // 生成器体内逃出的异常在 begin() / ++ 处抛给消费者。禁用异常的构建里生成器不会
+    // 失败：体内不能等待，也就没有错误与已停止这两种结果。
     void rethrowIfFailed() {
+#if CO2_HAS_EXCEPTIONS
         if (not error) return;
         auto pending = std::exception_ptr{};
         std::swap(pending, error);
         std::rethrow_exception(pending);
+#endif
     }
 
     Element* current{};
@@ -192,7 +201,9 @@ template <class Ref, class V> struct GeneratorPromise {
     GeneratorPromise* root{this};
     Handle parent;
     Generator<Ref, V>* owner{}; // 嵌套时：父层 awaiter 里拥有本帧的那个 Generator
+#if CO2_HAS_EXCEPTIONS
     std::exception_ptr error;
+#endif
     ResultStorage<Stored> stored;
 
   private:
````

修改 `include/co2/async_generator.hpp`：

````diff
diff --git a/include/co2/async_generator.hpp b/include/co2/async_generator.hpp
index 0c8054e..f5d97e5 100644
--- a/include/co2/async_generator.hpp
+++ b/include/co2/async_generator.hpp
@@ -100,8 +100,10 @@ template <class T> struct AsyncGeneratorPromise {
 
     void return_void() noexcept {}
 
+#if CO2_HAS_EXCEPTIONS
     // 异常只在这里被捕获一次，之后是一个按值传的 Error。
     void unhandled_exception() noexcept { fail(Error{std::current_exception()}); }
+#endif
 
     // 生产者等待的操作出错 / 已停止（或者它自己 with_error / just_error）：流到此为止。
     void return_error(Error error) noexcept { fail(std::move(error)); }
````

修改 `include/co2/as_result.hpp`：

````diff
diff --git a/include/co2/as_result.hpp b/include/co2/as_result.hpp
index 1476026..c5d4104 100644
--- a/include/co2/as_result.hpp
+++ b/include/co2/as_result.hpp
@@ -64,11 +64,15 @@ template <class Inner> struct AsResultAwaiter {
             completionOf(inner, typename HasAwaitCompletion<Inner>::type{});
         if (completion.isStopped()) return ResultType::stopped();
         if (completion.hasError()) return ResultType{std::move(completion).error()};
+#if CO2_HAS_EXCEPTIONS
         try {
             return resumeInner(std::is_void<Awaited>{});
         } catch (...) {
             return ResultType{Error{std::current_exception()}};
         }
+#else
+        return resumeInner(std::is_void<Awaited>{});
+#endif
     }
 
   private:
````

修改 `include/co2/callback.hpp`：

````diff
diff --git a/include/co2/callback.hpp b/include/co2/callback.hpp
index 9e52cfd..fb4c00f 100644
--- a/include/co2/callback.hpp
+++ b/include/co2/callback.hpp
@@ -33,7 +33,7 @@
 //
 //   continuation(args...)             有值：就地构造 T
 //   continuation.setError(code)       出错：一个 std::error_code
-//   continuation.setException(eptr)   出错：一个捕获的异常
+//   continuation.setException(eptr)   出错：一个捕获的异常（禁用异常的构建里没有）
 //   continuation.setStopped()         已停止
 //
 // 后三种经 await_completion() 按值交给等待它的协程，那个协程就此结束（见
@@ -81,12 +81,16 @@ template <class T> struct Continuation {
         CO2_CONTRACT_CHECK(operation != nullptr);
         operation->claim();
         unsigned outcome = CallbackAwaitable<T>::HasValue;
+#if CO2_HAS_EXCEPTIONS
         try {
             operation->storage.value.construct(std::forward<Args>(args)...);
         } catch (...) {
             operation->storeException(std::current_exception());
             outcome = CallbackAwaitable<T>::HasException;
         }
+#else
+        operation->storage.value.construct(std::forward<Args>(args)...);
+#endif
         operation->complete(outcome);
     }
 
@@ -98,6 +102,7 @@ template <class T> struct Continuation {
         operation->complete(CallbackAwaitable<T>::HasCode);
     }
 
+#if CO2_HAS_EXCEPTIONS
     // 以异常完成等待。前置条件：error 非空。
     void setException(std::exception_ptr error) const {
         CO2_CONTRACT_CHECK(operation != nullptr && error != nullptr);
@@ -105,6 +110,7 @@ template <class T> struct Continuation {
         operation->storeException(std::move(error));
         operation->complete(CallbackAwaitable<T>::HasException);
     }
+#endif
 
     // 以已停止完成等待：操作被取消了，没有结果。
     void setStopped() const {
@@ -148,7 +154,9 @@ template <class T> struct CallbackAwaitable {
     ~CallbackAwaitable() {
         auto const flags = state.load(std::memory_order_relaxed);
         if ((flags & HasValue) != 0U) storage.value.destroy();
+#if CO2_HAS_EXCEPTIONS
         if ((flags & HasException) != 0U) storage.exception.~exception_ptr();
+#endif
     }
 
     bool await_ready() const noexcept { return false; }
@@ -208,7 +216,9 @@ template <class T> struct CallbackAwaitable {
 
         detail::Uninitialized<T> value;
         std::error_code code;
+#if CO2_HAS_EXCEPTIONS
         std::exception_ptr exception;
+#endif
     };
 
     // 续体只能调用一次。
@@ -240,14 +250,20 @@ template <class T> struct CallbackAwaitable {
         state.fetch_and(~HasValue, std::memory_order_relaxed);
     }
 
+#if CO2_HAS_EXCEPTIONS
     void storeException(std::exception_ptr error) noexcept {
         ::new (static_cast<void*>(&storage.exception))
             std::exception_ptr{std::move(error)};
     }
+#endif
 
     // 把存着的错误交出去，只可一次。捕获的异常被移走，留下的空壳由析构函数清理。
     Error takeError(unsigned const flags) noexcept {
+#if CO2_HAS_EXCEPTIONS
         if ((flags & HasException) != 0U) return Error{std::move(storage.exception)};
+#else
+        static_cast<void>(flags);
+#endif
         return Error{storage.code};
     }
 
````

- [ ] **Step 4: 确认通过**

开着异常的构建，运行全部测试。Expected: `100% tests passed out of 31`（比任务 1 多一个编译失败测试）。

禁用异常的构建，GCC 与 Clang 各做一遍（Clang 的那一份先按"环境与命令"配置）。这时只构建已经能编过的目标，再运行对应的四个测试：

```bash
for b in gcc14-noexc clang14-noexc; do
  cmake --build /tmp/co2-build/$b -j 12 --target \
      co2-header-self-containment-tests \
      co2-no-exceptions-error-at-root-contract-violation-test \
      co2-no-exceptions-error-without-entry-contract-violation-test \
      co2-no-exceptions-error-through-legacy-awaiter-contract-violation-test \
      co2-has-exceptions-override-compile-fail-test-control
  ctest --test-dir /tmp/co2-build/$b -R "no-exceptions|has-exceptions"
done
```

Expected: 两个编译器都是 `100% tests passed out of 4`。

再确认编译失败测试败在预期的地方，而不是别的笔误：

```bash
cmake --build /tmp/co2-build/gcc14 --target co2-has-exceptions-override-compile-fail-test
```

Expected: 构建失败，错误是 `config.hpp` 里的 `#error "CO2_HAS_EXCEPTIONS follows the compiler's exception switch; do not define it"`。

- [ ] **Step 5: 提交**

```bash
git add include/co2/config.hpp include/co2/error.hpp include/co2/detail/frame.hpp \
        include/co2/task.hpp include/co2/generator.hpp include/co2/async_generator.hpp \
        include/co2/as_result.hpp include/co2/callback.hpp tests/CMakeLists.txt \
        tests/no_exceptions_error_at_root_contract_violation_test.cpp \
        tests/no_exceptions_error_without_entry_contract_violation_test.cpp \
        tests/no_exceptions_error_through_legacy_awaiter_contract_violation_test.cpp \
        tests/has_exceptions_override_compile_fail_test.cpp
git commit -F - <<'EOF'
禁用异常的构建：CO2_HAS_EXCEPTIONS 与库里的条件编译

config.hpp 从编译器的开关得出 CO2_HAS_EXCEPTIONS（__cpp_exceptions / __EXCEPTIONS /
_CPPUNWIND），不接受手动定义。为 0 时库里不出现 try / catch / throw：

- 协程体变换里的 catch 与各 promise 的 unhandled_exception() 不存在；
- Error 只装错误码：Error::exception()、Continuation::setException、
  GeneratorPromise 里存异常的成员不存在；
- asResult、Task 取值、Continuation::operator() 里把异常接成错误的 catch 不存在；
- 靠接住 bad_alloc 触发 get_return_object_on_allocation_failure 的那条路不存在；
- Error::raise() 是契约违规：错误到了根、到了没有 return_error() 的 promise、或者
  经过只转发三个成员的包装 awaiter，都无处可抛。

值、错误码、已停止三条通道不带条件编译，两种构建是同一份代码。开着异常的构建行为
不变。

测试一侧新增 CMake 选项 CO2_TEST_NO_EXCEPTIONS（给全部测试加 -fno-exceptions
-fno-rtti）、上面三条契约违规各一个测试，以及"手动定义 CO2_HAS_EXCEPTIONS 必须编
不过"的测试。

现有的测试源码还依赖 throw / catch，打开选项后的完整构建要到下一个提交才能通过。
这里验证的是：库的每个头文件在 GCC 与 Clang 的 -fno-exceptions -fno-rtti 下都能
单独编译，三个契约违规测试以退出码 86 结束。
EOF
```

### Task 3: 测试在两种构建下都能编译和运行

设计稿第 9 节："现有测试里依赖 `throw` / `catch` 的段落用 `CO2_HAS_EXCEPTIONS` 包起来"；三种完成方式的测试"在开、关异常两种构建下都跑"。

做法分三类：

- **只在有异常时成立的用例**（协程体里 `throw`、根上重抛、`unhandled_exception()`、`setException`……）连同它们用到的协程与类型，整段放进 `#if CO2_HAS_EXCEPTIONS`，`main()` 里的调用也是。
- **与用哪个出口无关的用例改成不依赖异常**，两种构建都跑：
  - `callback_tests.cpp` 与 `when_all_tests.cpp` 里各有一个"API 被取消"的用例，原来让续体以一个 `Cancelled` 异常完成，现在调用 `setStopped()`。它们测的是停止请求的转发，以及转发回调里拆掉 `whenAll` 时的生命周期。
  - `completion_tests.cpp` 里沿 Task 链传播错误的用例改成在根上按值看结果（新的 `sample::outcomeOf`，即 `syncWait(asResult(task))`）。"根上把错误码抛成 `std::system_error`"拆成一个单独的、只在开异常时跑的用例。
  - 判断"错误是按值传过来的，而不是途中抛出又被接住的"原来写 `error().exception() == nullptr`，禁用异常时没有这个成员；换成 `sample::carriesAnException`。
- **新增的用例**：只装错误码的 `Error` 的复制、移动与赋值（两种构建）；`Error::exception()`、接收 `exception_ptr` 的构造函数、`Continuation::setException` 恰好在有异常时存在（`static_assert`，两种构建）；禁用异常时没有 `unhandled_exception()` 的 promise 能用。

`Leaf::throws` 与 `Exit::exception` 这两个枚举项在禁用异常时不存在：误用它们的测试编不过，而不是悄悄跳过。

**Files:**
- Modify: `tests/completion_support.hpp`
- Modify: `tests/completion_tests.cpp`
- Modify: `tests/completion_components_tests.cpp`
- Modify: `tests/vocabulary_tests.cpp`
- Modify: `tests/callback_tests.cpp`
- Modify: `tests/when_all_tests.cpp`
- Modify: `tests/conformance_tests.cpp`
- Modify: `tests/trace.hpp`
- Modify: `tests/allocation_tests.cpp`
- Modify: `tests/return_types_tests.cpp`
- Modify: `tests/scheduler_tests.cpp`
- Modify: `tests/spawn_tests.cpp`
- Modify: `tests/thread_pool_tests.cpp`
- Modify: `tests/edge_case_tests.cpp`

**Interfaces:**
- Consumes: `CO2_HAS_EXCEPTIONS`、选项 `CO2_TEST_NO_EXCEPTIONS`（任务 2）；`co2::asResult`、`Continuation::setStopped`、`detail::TaskAccess::stateOf`（计划一、二）。
- Produces（测试支撑，命名空间 `sample`）:
  - `bool sample::carriesAnException(co2::Error const&) noexcept`：错误里装的是不是一个捕获的异常；禁用异常时恒为 `false`。
  - `template <class T> co2::Result<T> sample::outcomeOf(co2::Task<T>)`：在根上按值拿到 Task 的结果。
  - `sample::Leaf::throws` 只在 `CO2_HAS_EXCEPTIONS` 为 1 时存在。

- [ ] **Step 1: 确认现状——禁用异常的完整构建编不过**

```bash
cmake --build /tmp/co2-build/gcc14-noexc -j 12
```

Expected: 构建失败，错误都在 `tests/` 下的源文件里（`exception handling disabled`、`has no member named 'setException'`、`has no member named 'exception'` 等）。

- [ ] **Step 2: 三种完成方式的测试**

支撑代码与两个测试文件。`completion_components_tests.cpp` 里 `completesWith` 的 lambda 写在 `CO2_AWAIT_AS_SET(...)` 的括号里，条件编译进不去，所以把它的函数体搬进一个单独的函数 `completes`。

修改 `tests/completion_support.hpp`：

````diff
diff --git a/tests/completion_support.hpp b/tests/completion_support.hpp
index 32c3f3c..fb3027d 100644
--- a/tests/completion_support.hpp
+++ b/tests/completion_support.hpp
@@ -6,8 +6,10 @@
 #include <stdexcept>
 #include <vector>
 
+#include "co2/as_result.hpp"
 #include "co2/completion.hpp"
 #include "co2/coroutine.hpp"
+#include "co2/sync_wait.hpp"
 #include "co2/task.hpp"
 #include "sample_errc.hpp"
 
@@ -30,7 +32,25 @@ struct ExpectedError : std::runtime_error {
     ExpectedError() : std::runtime_error{"expected"} {}
 };
 
-enum class Leaf { returns, fails, stops, throws };
+// 禁用异常的构建里没有 throws：误用它的测试编不过，而不是悄悄跳过。
+enum class Leaf {
+    returns,
+    fails,
+    stops,
+#if CO2_HAS_EXCEPTIONS
+    throws,
+#endif
+};
+
+// 错误是按值传过来的错误码，还是途中抛出、被接住的异常？禁用异常的构建里只有前一种。
+inline bool carriesAnException(co2::Error const& error) noexcept {
+#if CO2_HAS_EXCEPTIONS
+    return error.exception() != nullptr;
+#else
+    static_cast<void>(error);
+    return false;
+#endif
+}
 
 // 记下每一层的局部是按什么顺序析构的。
 struct DepthGuard {
@@ -49,7 +69,9 @@ inline auto chain(int depth, Leaf leaf, int& resumed, std::vector<int>* order)
     guard.order = order;
     guard.depth = depth;
     if (depth == 0) {
+#if CO2_HAS_EXCEPTIONS
         if (leaf == Leaf::throws) throw ExpectedError{};
+#endif
         if (leaf == Leaf::fails) CO2_AWAIT(Ends{failed()});
         if (leaf == Leaf::stops) CO2_AWAIT(Ends{stopped()});
         CO2_RETURN(1);
@@ -60,4 +82,10 @@ inline auto chain(int depth, Leaf leaf, int& resumed, std::vector<int>* order)
 }
 CO2_END
 
+// 在根上按值看一个 Task 怎么结束的：错误不变成异常。两种构建都能用——禁用异常时让错误
+// 走到根上是契约违规，只能这样看。
+template <class T> co2::Result<T> outcomeOf(co2::Task<T> task) {
+    return std::move(*co2::syncWait(co2::asResult(std::move(task))));
+}
+
 } // namespace sample
````

修改 `tests/completion_tests.cpp`：

````diff
diff --git a/tests/completion_tests.cpp b/tests/completion_tests.cpp
index dd60e25..f3849a9 100644
--- a/tests/completion_tests.cpp
+++ b/tests/completion_tests.cpp
@@ -51,6 +51,7 @@ using sample::Ends;
 using sample::ExpectedError;
 using sample::failed;
 using sample::Leaf;
+using sample::outcomeOf;
 using sample::stopped;
 
 // ---------------------------------------------------------------------------
@@ -157,6 +158,7 @@ struct Recorder {
     co2::coroutine_handle<promise_type> handle;
 };
 
+#if CO2_HAS_EXCEPTIONS
 // 为标准写的 promise：没有这两个入口，只有 unhandled_exception()。
 struct Plain {
     struct promise_type {
@@ -188,6 +190,7 @@ struct Plain {
 
     co2::coroutine_handle<promise_type> handle;
 };
+#endif
 
 // final_suspend 不挂起：协程结束时帧自己销毁。
 int detachedStops = 0;
@@ -357,6 +360,7 @@ void aCoroutineThatDoesNotSuspendAtTheEndDestroysItself() {
     CHECK(destroyed == 1);
 }
 
+#if CO2_HAS_EXCEPTIONS
 // ---------------------------------------------------------------------------
 // promise 没有入口：错误在协程体内重新抛出，走 unhandled_exception()
 
@@ -402,6 +406,7 @@ void aCapturedExceptionReachesAStandardPromiseUnchanged() {
     }
     CHECK(sawOriginal);
 }
+#endif
 
 // ---------------------------------------------------------------------------
 // Task：错误与已停止按值沿等待链向上传，只在根上变回异常或空的 optional
@@ -412,20 +417,31 @@ void valuesStillFlowThroughTheChain() {
     CHECK(resumed == 3);
 }
 
-void anErrorPassesThroughEveryLayerAndIsThrownAtTheRoot() {
+void anErrorPassesThroughEveryLayer() {
     auto resumed = 0;
     auto order = std::vector<int>{};
+    auto const outcome = outcomeOf(chain(3, Leaf::fails, resumed, &order));
+    CHECK(outcome.hasError());
+    CHECK(outcome.error().code() == sample::Errc::gone);
+    CHECK(not sample::carriesAnException(outcome.error())); // 一路上没有抛过
+    CHECK(resumed == 0); // 没有一层在等待之后继续执行
+    auto const innerFirst = std::vector<int>{0, 1, 2, 3};
+    CHECK(order == innerFirst); // 局部由内向外析构
+}
+
+#if CO2_HAS_EXCEPTIONS
+void theRootThrowsTheErrorAsSystemError() {
+    auto resumed = 0;
     auto sawCode = false;
     try {
-        co2::syncWait(chain(3, Leaf::fails, resumed, &order));
+        co2::syncWait(chain(3, Leaf::fails, resumed, nullptr));
     } catch (std::system_error const& exception) {
         sawCode = exception.code() == sample::Errc::gone;
     }
     CHECK(sawCode);
-    CHECK(resumed == 0); // 没有一层在等待之后继续执行
-    auto const innerFirst = std::vector<int>{0, 1, 2, 3};
-    CHECK(order == innerFirst); // 局部由内向外析构
+    CHECK(resumed == 0);
 }
+#endif
 
 void aStoppedTaskEndsEveryAwaiterAndTheRootReturnsEmpty() {
     auto resumed = 0;
@@ -437,6 +453,7 @@ void aStoppedTaskEndsEveryAwaiterAndTheRootReturnsEmpty() {
     CHECK(order == innerFirst);
 }
 
+#if CO2_HAS_EXCEPTIONS
 void anExceptionIsRethrownUnchangedAtTheRoot() {
     auto resumed = 0;
     auto sawOriginal = false;
@@ -448,17 +465,21 @@ void anExceptionIsRethrownUnchangedAtTheRoot() {
     CHECK(sawOriginal);
     CHECK(resumed == 0);
 }
+#endif
 
 void joinSeesTheSameThreeOutcomes() {
     co2::ManualExecutor executor;
     auto resumed = 0;
     auto value = co2::spawn(executor, chain(1, Leaf::returns, resumed, nullptr));
     auto stops = co2::spawn(executor, chain(1, Leaf::stops, resumed, nullptr));
-    auto fails = co2::spawn(executor, chain(1, Leaf::fails, resumed, nullptr));
-    CHECK(executor.run() == 3U);
+    CHECK(executor.run() == 2U);
 
     CHECK(value.join() == 2);
     CHECK(not stops.join().has_value());
+
+#if CO2_HAS_EXCEPTIONS
+    auto fails = co2::spawn(executor, chain(1, Leaf::fails, resumed, nullptr));
+    CHECK(executor.run() == 1U);
     auto sawCode = false;
     try {
         fails.join();
@@ -466,6 +487,7 @@ void joinSeesTheSameThreeOutcomes() {
         sawCode = exception.code() == sample::Errc::gone;
     }
     CHECK(sawCode);
+#endif
 }
 
 // 每一层都由恢复循环驱动：十万层的链上传播不长栈，析构也不递归。
@@ -474,13 +496,9 @@ void aDeepChainPropagatesWithoutGrowingTheStack() {
     auto resumed = 0;
     CHECK(not co2::syncWait(chain(depth, Leaf::stops, resumed, nullptr)).has_value());
 
-    auto sawCode = false;
-    try {
-        co2::syncWait(chain(depth, Leaf::fails, resumed, nullptr));
-    } catch (std::system_error const& exception) {
-        sawCode = exception.code() == sample::Errc::gone;
-    }
-    CHECK(sawCode);
+    auto const outcome = outcomeOf(chain(depth, Leaf::fails, resumed, nullptr));
+    CHECK(outcome.hasError());
+    CHECK(outcome.error().code() == sample::Errc::gone);
     CHECK(resumed == 0);
 }
 
@@ -527,11 +545,13 @@ auto awaitsThroughAWrapper(Leaf leaf, int& resumed)
 }
 CO2_END
 
-// 错误照旧能穿过旧式的调用方：await_resume() 把它抛出来。
-void anErrorStillCrossesAnAwaiterThatOnlyKnowsThreeMembers() {
+// 值照旧穿过旧式的调用方。错误也能：await_resume() 把它抛出来——没有异常时这一步是
+// 契约违规，见 no_exceptions_error_through_legacy_awaiter_contract_violation_test.cpp。
+void anAwaiterThatOnlyKnowsThreeMembersStillWorks() {
     auto resumed = 0;
     CHECK(co2::syncWait(awaitsThroughAWrapper(Leaf::returns, resumed)) == 11);
 
+#if CO2_HAS_EXCEPTIONS
     auto sawCode = false;
     try {
         co2::syncWait(awaitsThroughAWrapper(Leaf::fails, resumed));
@@ -547,6 +567,7 @@ void anErrorStillCrossesAnAwaiterThatOnlyKnowsThreeMembers() {
         sawOriginal = true;
     }
     CHECK(sawOriginal);
+#endif
 }
 
 // ---------------------------------------------------------------------------
@@ -566,19 +587,16 @@ auto failsWithWithError(bool& reached) CO2_BEG(co2::Task<int>, (reached)) {
 }
 CO2_END
 
-bool throwsRetryLater(co2::Task<int> task) {
-    try {
-        co2::syncWait(std::move(task));
-    } catch (std::system_error const& exception) {
-        return exception.code() == sample::Errc::retry_later;
-    }
-    return false;
+bool failsWithRetryLater(co2::Task<int> task) {
+    auto const outcome = outcomeOf(std::move(task));
+    return outcome.hasError() && outcome.error().code() == sample::Errc::retry_later &&
+           not sample::carriesAnException(outcome.error());
 }
 
 void justErrorAndWithErrorEndTheTaskWithThatError() {
     auto reached = false;
-    CHECK(throwsRetryLater(failsWithJustError(reached)));
-    CHECK(throwsRetryLater(failsWithWithError(reached)));
+    CHECK(failsWithRetryLater(failsWithJustError(reached)));
+    CHECK(failsWithRetryLater(failsWithWithError(reached)));
     CHECK(not reached);
 }
 
@@ -597,6 +615,7 @@ void justErrorWorksInAnyPromiseThatHasTheEntry() {
     CHECK(not reached);
 }
 
+#if CO2_HAS_EXCEPTIONS
 // 不认识 await_completion() 的调用方直接调用 await_resume()：错误以异常的形式出现。
 void justErrorRaisesForACallerThatSkipsAwaitCompletion() {
     auto const awaiter = co2::just_error(sample::Errc::gone);
@@ -608,6 +627,7 @@ void justErrorRaisesForACallerThatSkipsAwaitCompletion() {
     }
     CHECK(sawCode);
 }
+#endif
 
 // ---------------------------------------------------------------------------
 // asResult：三种结果都变成值，当前协程照常往下执行
@@ -618,7 +638,7 @@ auto classifies(Leaf leaf, int& resumed)
     CO2_AWAIT_SET(result, co2::asResult(chain(1, leaf, resumed, nullptr)));
     if (result->isStopped()) CO2_RETURN(2);
     if (result->hasValue()) CO2_RETURN(result->value() == 2 ? 0 : -1);
-    if (result->error().exception() != nullptr) CO2_RETURN(3);
+    if (sample::carriesAnException(result->error())) CO2_RETURN(3);
     CO2_RETURN(result->error().code() == sample::Errc::gone ? 1 : -1);
 }
 CO2_END
@@ -628,7 +648,9 @@ void asResultTurnsEveryCompletionIntoAValue() {
     CHECK(co2::syncWait(classifies(Leaf::returns, resumed)) == 0);
     CHECK(co2::syncWait(classifies(Leaf::fails, resumed)) == 1);
     CHECK(co2::syncWait(classifies(Leaf::stops, resumed)) == 2);
+#if CO2_HAS_EXCEPTIONS
     CHECK(co2::syncWait(classifies(Leaf::throws, resumed)) == 3);
+#endif
 }
 
 // 标准写法的 awaiter：没有 await_completion()。
@@ -638,11 +660,13 @@ struct Seven {
     int await_resume() const noexcept { return 7; }
 };
 
+#if CO2_HAS_EXCEPTIONS
 struct Throws {
     bool await_ready() const noexcept { return true; }
     void await_suspend(co2::coroutine_handle<>) const noexcept {}
     int await_resume() const { throw ExpectedError{}; }
 };
+#endif
 
 auto wrapsPlainAwaiters(int& sum, bool& caught, bool& voidHasValue)
     CO2_BEG(co2::Task<>, (sum, caught, voidHasValue),
@@ -652,8 +676,10 @@ auto wrapsPlainAwaiters(int& sum, bool& caught, bool& voidHasValue)
     sum += number->value();
     CO2_AWAIT_SET(number, co2::asResult(lvalue)); // 左值：只记引用
     sum += number->value();
+#if CO2_HAS_EXCEPTIONS
     CO2_AWAIT_SET(number, co2::asResult(Throws{})); // await_resume() 的异常被接成错误
     caught = number->hasError() && number->error().exception() != nullptr;
+#endif
     CO2_AWAIT_SET(nothing, co2::asResult(co2::suspend_never{})); // 结果是 void
     voidHasValue = nothing->hasValue();
 }
@@ -665,7 +691,9 @@ void asResultWrapsAnyAwaitable() {
     auto voidHasValue = false;
     CHECK(co2::syncWait(wrapsPlainAwaiters(sum, caught, voidHasValue)).has_value());
     CHECK(sum == 14);
+#if CO2_HAS_EXCEPTIONS
     CHECK(caught);
+#endif
     CHECK(voidHasValue);
 }
 
@@ -701,11 +729,13 @@ void syncWaitOfAsResultHandsTheOutcomeToOrdinaryCode() {
         co2::syncWait(co2::asResult(chain(1, Leaf::stops, resumed, nullptr)));
     CHECK(stop.has_value() && stop->isStopped());
 
+#if CO2_HAS_EXCEPTIONS
     auto const thrown =
         co2::syncWait(co2::asResult(chain(1, Leaf::throws, resumed, nullptr)));
     CHECK(thrown.has_value() && thrown->hasError());
     CHECK(thrown->error().exception() != nullptr);
     CHECK(thrown->error().code() == co2::Errc::unhandled_exception);
+#endif
 
     CHECK(co2::syncWait(co2::asResult(voidLeaf(Leaf::returns)))->hasValue());
     CHECK(co2::syncWait(co2::asResult(voidLeaf(Leaf::stops)))->isStopped());
@@ -727,19 +757,26 @@ int main() {
     aYieldIsAResumePointToo();
     anLvalueAwaiterIsAskedInPlace();
     aCoroutineThatDoesNotSuspendAtTheEndDestroysItself();
+#if CO2_HAS_EXCEPTIONS
     anErrorCodeReachesAStandardPromiseAsSystemError();
     aCapturedExceptionReachesAStandardPromiseUnchanged();
+#endif
     valuesStillFlowThroughTheChain();
-    anErrorPassesThroughEveryLayerAndIsThrownAtTheRoot();
+    anErrorPassesThroughEveryLayer();
     aStoppedTaskEndsEveryAwaiterAndTheRootReturnsEmpty();
+#if CO2_HAS_EXCEPTIONS
+    theRootThrowsTheErrorAsSystemError();
     anExceptionIsRethrownUnchangedAtTheRoot();
+#endif
     joinSeesTheSameThreeOutcomes();
     aDeepChainPropagatesWithoutGrowingTheStack();
     anLvalueTaskPropagatesTheSameWay();
-    anErrorStillCrossesAnAwaiterThatOnlyKnowsThreeMembers();
+    anAwaiterThatOnlyKnowsThreeMembersStillWorks();
     justErrorAndWithErrorEndTheTaskWithThatError();
     justErrorWorksInAnyPromiseThatHasTheEntry();
+#if CO2_HAS_EXCEPTIONS
     justErrorRaisesForACallerThatSkipsAwaitCompletion();
+#endif
     asResultTurnsEveryCompletionIntoAValue();
     asResultWrapsAnyAwaitable();
     syncWaitOfAsResultHandsTheOutcomeToOrdinaryCode();
````

修改 `tests/completion_components_tests.cpp`：

````diff
diff --git a/tests/completion_components_tests.cpp b/tests/completion_components_tests.cpp
index 84bba2e..f30ec1c 100644
--- a/tests/completion_components_tests.cpp
+++ b/tests/completion_components_tests.cpp
@@ -73,7 +73,7 @@ void awaitingAFinishedHandleSeesHowTheTaskEnded() {
     // 等待者接住的异常。
     auto const failure = co2::syncWait(co2::asResult(awaitsAHandle(fails, reached)));
     CHECK(failure->hasError());
-    CHECK(failure->error().exception() == nullptr);
+    CHECK(not sample::carriesAnException(failure->error()));
     CHECK(failure->error().code() == sample::Errc::gone);
     CHECK(not reached);
 
@@ -152,7 +152,7 @@ void whenAllCompletesWithTheFirstErrorInTime() {
     CHECK(second.hasError() && second.error().code() == sample::Errc::gone);
 
     // 错误是按值传过来的：仍然是错误码，而不是被抛出又接住的异常。
-    CHECK(first.error().exception() == nullptr);
+    CHECK(not sample::carriesAnException(first.error()));
 }
 
 void whenAllCompletesStoppedWhenAChildStopsAndNoneFails() {
@@ -238,7 +238,7 @@ void whenAllOfAVectorFollowsTheSameRules() {
 
     auto const failure = outcomeOfAll(Leaf::stops, Leaf::fails, Leaf::returns);
     CHECK(failure.hasError());
-    CHECK(failure.error().exception() == nullptr);
+    CHECK(not sample::carriesAnException(failure.error()));
     CHECK(failure.error().code() == sample::Errc::gone);
 }
 
@@ -250,7 +250,9 @@ auto producesOneThen(Leaf leaf) CO2_BEG(co2::AsyncGenerator<int>, (leaf)) {
     CO2_YIELD(1);
     if (leaf == Leaf::fails) CO2_YIELD(co2::with_error(sample::Errc::gone));
     if (leaf == Leaf::stops) CO2_AWAIT(Ends{stopped()});
+#if CO2_HAS_EXCEPTIONS
     if (leaf == Leaf::throws) throw ExpectedError{};
+#endif
 }
 CO2_END
 
@@ -279,7 +281,7 @@ void aFailingProducerEndsItsConsumerTheSameWay() {
     auto const failure =
         co2::syncWait(co2::asResult(drains(Leaf::fails, seen, reachedTheEnd)));
     CHECK(failure->hasError());
-    CHECK(failure->error().exception() == nullptr); // 按值传过来的错误码
+    CHECK(not sample::carriesAnException(failure->error())); // 按值传过来的错误码
     CHECK(failure->error().code() == sample::Errc::gone);
     CHECK(seen == 1); // 出错之前的元素照常交付
     CHECK(not reachedTheEnd);
@@ -290,10 +292,12 @@ void aFailingProducerEndsItsConsumerTheSameWay() {
     CHECK(seen == 1);
     CHECK(not reachedTheEnd);
 
+#if CO2_HAS_EXCEPTIONS
     auto const thrown =
         co2::syncWait(co2::asResult(drains(Leaf::throws, seen, reachedTheEnd)));
     CHECK(thrown->hasError());
     CHECK(thrown->error().exception() != nullptr);
+#endif
 }
 
 // 返回 0：第二次 next() 有值；1：出错；2：已停止。
@@ -325,26 +329,40 @@ void theFailureIsDeliveredOnceAndThenTheStreamIsOver() {
 }
 
 // ---------------------------------------------------------------------------
-// fromCallback：续体的四个出口——值、错误码、异常、已停止
+// fromCallback：续体的四个出口——值、错误码、异常、已停止（禁用异常时没有异常那一个）
+
+enum class Exit {
+    value,
+    code,
+#if CO2_HAS_EXCEPTIONS
+    exception,
+#endif
+    stopped,
+};
 
-enum class Exit { value, code, exception, stopped };
+// 单独成一个函数：条件编译不能写在宏的实参里。
+void completes(co2::Continuation<int> done, Exit const how) {
+    if (how == Exit::value) done(5);
+    if (how == Exit::code) done.setError(sample::Errc::gone);
+#if CO2_HAS_EXCEPTIONS
+    if (how == Exit::exception)
+        done.setException(std::make_exception_ptr(ExpectedError{}));
+#endif
+    if (how == Exit::stopped) done.setStopped();
+}
 
 auto completesWith(Exit how, bool& reached)
     CO2_BEG(co2::Task<int>, (how, reached), int got{};) {
     CO2_AWAIT_AS_SET(got, co2::CallbackAwaitable<int>,
                      co2::fromCallback<int>([this](co2::Continuation<int> done) {
-                         if (how == Exit::value) done(5);
-                         if (how == Exit::code) done.setError(sample::Errc::gone);
-                         if (how == Exit::exception)
-                             done.setException(std::make_exception_ptr(ExpectedError{}));
-                         if (how == Exit::stopped) done.setStopped();
+                         completes(done, how);
                      }));
     reached = true;
     CO2_RETURN(got);
 }
 CO2_END
 
-void theContinuationHasFourExits() {
+void theContinuationHasEveryExit() {
     auto reached = false;
     CHECK(co2::syncWait(completesWith(Exit::value, reached)) == 5);
     CHECK(reached);
@@ -352,15 +370,17 @@ void theContinuationHasFourExits() {
     reached = false;
     auto const code = co2::syncWait(co2::asResult(completesWith(Exit::code, reached)));
     CHECK(code->hasError());
-    CHECK(code->error().exception() == nullptr); // 按值传过来的错误码
+    CHECK(not sample::carriesAnException(code->error())); // 按值传过来的错误码
     CHECK(code->error().code() == sample::Errc::gone);
     CHECK(not reached);
 
+#if CO2_HAS_EXCEPTIONS
     auto const exception =
         co2::syncWait(co2::asResult(completesWith(Exit::exception, reached)));
     CHECK(exception->hasError());
     CHECK(exception->error().exception() != nullptr);
     CHECK(not reached);
+#endif
 
     CHECK(co2::syncWait(co2::asResult(completesWith(Exit::stopped, reached)))
               ->isStopped());
@@ -393,6 +413,7 @@ void anApiThatFinishesLaterCanReportStopped() {
     CHECK(not reached);
 }
 
+#if CO2_HAS_EXCEPTIONS
 // 不认识 await_completion() 的调用方直接调用 await_resume()：错误码以异常的形式出现。
 void anErrorCodeFromACallbackRaisesForACallerThatSkipsAwaitCompletion() {
     auto awaitable = co2::fromCallback<int>(
@@ -406,6 +427,7 @@ void anErrorCodeFromACallbackRaisesForACallerThatSkipsAwaitCompletion() {
     }
     CHECK(sawCode);
 }
+#endif
 
 // ---------------------------------------------------------------------------
 // 已停止的产生方：just_stopped 与 stopIfRequested
@@ -540,7 +562,7 @@ void unstoppableStillForwardsHowTheOperationEnded() {
     reached = false;
     auto const failure = co2::syncWait(co2::asResult(shields(Leaf::fails, reached)));
     CHECK(failure->hasError());
-    CHECK(failure->error().exception() == nullptr);
+    CHECK(not sample::carriesAnException(failure->error()));
     CHECK(failure->error().code() == sample::Errc::gone);
 
     CHECK(co2::syncWait(co2::asResult(shields(Leaf::stops, reached)))->isStopped());
@@ -638,9 +660,11 @@ int main() {
     whenAllOfAVectorFollowsTheSameRules();
     aFailingProducerEndsItsConsumerTheSameWay();
     theFailureIsDeliveredOnceAndThenTheStreamIsOver();
-    theContinuationHasFourExits();
+    theContinuationHasEveryExit();
     anApiThatFinishesLaterCanReportStopped();
+#if CO2_HAS_EXCEPTIONS
     anErrorCodeFromACallbackRaisesForACallerThatSkipsAwaitCompletion();
+#endif
     justStoppedEndsTheTaskAsStopped();
     stopIfRequestedIsACancellationPoint();
     stopIfRequestedDoesNothingWithoutAToken();
````

修改 `tests/vocabulary_tests.cpp`：

````diff
diff --git a/tests/vocabulary_tests.cpp b/tests/vocabulary_tests.cpp
index ca9bb77..1cdabf0 100644
--- a/tests/vocabulary_tests.cpp
+++ b/tests/vocabulary_tests.cpp
@@ -177,6 +177,19 @@ static_assert(not std::is_convertible<int, co2::Error>::value,
 static_assert(not std::is_convertible<std::exception_ptr, co2::Error>::value,
               "capturing an exception as co2::Error must be explicit");
 
+// 装异常的那一半只在有异常的构建里存在（config.hpp 的 CO2_HAS_EXCEPTIONS）。
+template <class E, class = void> struct HasExceptionAccessor : std::false_type {};
+template <class E>
+struct HasExceptionAccessor<
+    E, decltype(static_cast<void>(std::declval<E const&>().exception()))>
+    : std::true_type {};
+
+static_assert(HasExceptionAccessor<co2::Error>::value == (CO2_HAS_EXCEPTIONS != 0),
+              "Error::exception() exists exactly when the build has exceptions");
+static_assert(std::is_constructible<co2::Error, std::exception_ptr>::value ==
+                  (CO2_HAS_EXCEPTIONS != 0),
+              "an Error captures an exception exactly when the build has exceptions");
+
 bool acceptsError(co2::Error const& error, std::error_code const expected) {
     return error.code() == expected;
 }
@@ -184,7 +197,9 @@ bool acceptsError(co2::Error const& error, std::error_code const expected) {
 void anErrorHoldsACode() {
     co2::Error const error = std::make_error_code(std::errc::timed_out);
     CHECK(error.code() == std::errc::timed_out);
+#if CO2_HAS_EXCEPTIONS
     CHECK(error.exception() == nullptr);
+#endif
 }
 
 void anErrorConvertsFromAnErrorCodeEnum() {
@@ -194,6 +209,25 @@ void anErrorConvertsFromAnErrorCodeEnum() {
     CHECK(acceptsError(sample::Errc::gone, sample::Errc::gone));
 }
 
+void errorCodesCopyMoveAndAssign() {
+    co2::Error first = sample::Errc::retry_later;
+    co2::Error second = sample::Errc::gone;
+
+    auto copy = first;
+    CHECK(copy.code() == sample::Errc::retry_later);
+    CHECK(first.code() == sample::Errc::retry_later);
+
+    auto moved = std::move(copy);
+    CHECK(moved.code() == sample::Errc::retry_later);
+
+    moved = second;
+    CHECK(moved.code() == sample::Errc::gone);
+
+    second = std::move(first);
+    CHECK(second.code() == sample::Errc::retry_later);
+}
+
+#if CO2_HAS_EXCEPTIONS
 void anErrorHoldsAnException() {
     auto const error = co2::Error{std::make_exception_ptr(std::runtime_error{"boom"})};
     CHECK(error.code() == co2::Errc::unhandled_exception);
@@ -242,6 +276,7 @@ void errorsCopyMoveAndAssignAcrossBothKinds() {
     code = exception;
     CHECK(code.exception() == exception.exception());
 }
+#endif
 
 // ---------------------------------------------------------------------------
 // Completion
@@ -359,9 +394,12 @@ int main() {
     optionalsOrderLikeStdOptional();
     anErrorHoldsACode();
     anErrorConvertsFromAnErrorCodeEnum();
+    errorCodesCopyMoveAndAssign();
+#if CO2_HAS_EXCEPTIONS
     anErrorHoldsAnException();
     raiseRethrowsTheOriginalExceptionOrThrowsSystemError();
     errorsCopyMoveAndAssignAcrossBothKinds();
+#endif
     aCompletionIsExactlyOneOfThreeKinds();
     aResultIsAValueAnErrorOrStopped();
     aResultOwnsExactlyOneValue();
````

- [ ] **Step 3: 回调与 `whenAll` 的测试**

修改 `tests/callback_tests.cpp`：

````diff
diff --git a/tests/callback_tests.cpp b/tests/callback_tests.cpp
index 9a84e1a..219788b 100644
--- a/tests/callback_tests.cpp
+++ b/tests/callback_tests.cpp
@@ -13,6 +13,7 @@
 #include <stdexcept>
 #include <string>
 #include <thread>
+#include <type_traits>
 #include <utility>
 #include <vector>
 
@@ -43,9 +44,16 @@ struct ExpectedError : std::runtime_error {
     ExpectedError() : std::runtime_error{"expected"} {}
 };
 
-struct Cancelled : std::runtime_error {
-    Cancelled() : std::runtime_error{"cancelled"} {}
-};
+// 续体的异常出口只在有异常的构建里存在；值、错误码、已停止三个出口两种构建都有。
+template <class C, class = void> struct HasSetException : std::false_type {};
+template <class C>
+struct HasSetException<C, decltype(static_cast<void>(std::declval<C&>().setException(
+                              std::declval<std::exception_ptr>())))>
+    : std::true_type {};
+
+static_assert(HasSetException<co2::Continuation<int>>::value ==
+                  (CO2_HAS_EXCEPTIONS != 0),
+              "Continuation::setException exists exactly when the build has exceptions");
 
 // 模拟的回调式 API：把 handler 存起来，由测试代码在任意线程上触发；支持 cancel。
 struct FakeApi {
@@ -231,6 +239,7 @@ void completionRacingWithSuspensionIsResolvedEitherWay() {
 // ---------------------------------------------------------------------------
 // 异常两条通道
 
+#if CO2_HAS_EXCEPTIONS
 auto initiateThrows() CO2_BEG(co2::Task<int>, (), int got{};) {
     CO2_AWAIT_AS_SET(
         got, co2::CallbackAwaitable<int>,
@@ -310,6 +319,7 @@ void throwingResultConstructorCompletesTheAwaitWithTheException() {
     }
     CHECK(thrown);
 }
+#endif
 
 // ---------------------------------------------------------------------------
 // 结果的生命周期：awaiter 里的 T 没有自己的 engaged 标志，"有没有对象"记在原子状态字
@@ -318,7 +328,9 @@ void throwingResultConstructorCompletesTheAwaitWithTheException() {
 struct Tracked {
     static std::atomic<int> live;
     static std::atomic<int> constructed;
+#if CO2_HAS_EXCEPTIONS
     static bool throwOnMove; // 只在单线程用例里改动
+#endif
 
     explicit Tracked(int const payload_) : payload{payload_} {
         ++live;
@@ -326,7 +338,9 @@ struct Tracked {
     }
 
     Tracked(Tracked&& other) : payload{other.payload} {
+#if CO2_HAS_EXCEPTIONS
         if (throwOnMove) throw ExpectedError{};
+#endif
         ++live;
         ++constructed;
     }
@@ -342,7 +356,9 @@ struct Tracked {
 
 std::atomic<int> Tracked::live{0};
 std::atomic<int> Tracked::constructed{0};
+#if CO2_HAS_EXCEPTIONS
 bool Tracked::throwOnMove = false;
+#endif
 
 auto trackedSynchronously(int v)
     CO2_BEG(co2::Task<int>, (v), co2::detail::ResultStorage<Tracked> got;) {
@@ -382,6 +398,7 @@ void resultIsConstructedOnceAndDestroyedOnce() {
     }
 }
 
+#if CO2_HAS_EXCEPTIONS
 // await_resume 移出结果时移动构造抛出：异常进入协程体，留在 awaiter 里的那一份由
 // awaiter 的析构函数清理，不重复析构。
 struct ThrowOnMoveScope {
@@ -414,6 +431,7 @@ auto voidCompletesWithException() CO2_BEG(co2::Task<>, ()) {
     CO2_RETURN();
 }
 CO2_END
+#endif
 
 auto voidCompletesFromAnotherThread(std::vector<std::thread>& threads, int& hits)
     CO2_BEG(co2::Task<>, (threads, hits)) {
@@ -427,6 +445,7 @@ auto voidCompletesFromAnotherThread(std::vector<std::thread>& threads, int& hits
 CO2_END
 
 void voidResultsTakeBothChannels() {
+#if CO2_HAS_EXCEPTIONS
     bool thrown = false;
     try {
         co2::syncWait(voidCompletesWithException());
@@ -434,6 +453,7 @@ void voidResultsTakeBothChannels() {
         thrown = true;
     }
     CHECK(thrown);
+#endif
 
     int hits = 0;
     for (int round = 0; round < 200; ++round) {
@@ -461,7 +481,7 @@ auto cancellable(FakeApi& api, co2::ThreadPool& pool)
             api.asyncGetWithCancel([done, guard](bool cancelled, int value) mutable {
                 guard.reset();
                 if (cancelled)
-                    done.setException(std::make_exception_ptr(Cancelled{}));
+                    done.setStopped();
                 else
                     done(value);
             });
@@ -477,15 +497,9 @@ void stopRequestReachesTheApiThroughTheInitiateToken() {
     while (not api.hasPending())
         std::this_thread::yield();
     CHECK(not handle.isReady());
-    CHECK(
-        handle.requestStop()); // → stop_callback → api.cancel() → 续体以 Cancelled 完成
-    bool thrown = false;
-    try {
-        handle.join();
-    } catch (Cancelled const&) {
-        thrown = true;
-    }
-    CHECK(thrown);
+    // → stop_callback → api.cancel() → API 以"已取消"回调 → 续体 setStopped()
+    CHECK(handle.requestStop());
+    CHECK(not handle.join().has_value());
 }
 
 void normalCompletionWinsWhenNoStopIsRequested() {
@@ -622,11 +636,15 @@ int main() {
     asynchronousCompletionResumesOnTheCallbackThread();
     syncWaitWakesFromACallbackThread();
     completionRacingWithSuspensionIsResolvedEitherWay();
+#if CO2_HAS_EXCEPTIONS
     exceptionsFromInitiateAreDeliveredIntoTheCoroutine();
     setExceptionIsRethrownByAwaitResume();
     throwingResultConstructorCompletesTheAwaitWithTheException();
+#endif
     resultIsConstructedOnceAndDestroyedOnce();
+#if CO2_HAS_EXCEPTIONS
     throwingMoveOutOfTheAwaiterIsCleanedUpExactlyOnce();
+#endif
     voidResultsTakeBothChannels();
     stopRequestReachesTheApiThroughTheInitiateToken();
     normalCompletionWinsWhenNoStopIsRequested();
````

修改 `tests/when_all_tests.cpp`：

````diff
diff --git a/tests/when_all_tests.cpp b/tests/when_all_tests.cpp
index 4b2da99..22a1c63 100644
--- a/tests/when_all_tests.cpp
+++ b/tests/when_all_tests.cpp
@@ -246,6 +246,7 @@ void childrenRunConcurrentlyOnAThreadPool() {
 // ---------------------------------------------------------------------------
 // 异常与停止传播
 
+#if CO2_HAS_EXCEPTIONS
 auto failsAfterGate(ManualEvent& gate, int which)
     CO2_BEG(co2::Task<int>, (gate, which)) {
     CO2_AWAIT(gate.wait());
@@ -298,6 +299,7 @@ void firstFailureInTimeIsRethrownAfterEveryChildFinishes() {
     whenAllTask = co2::Task<std::tuple<int, int, int>>{};
     CHECK(destroyed.load() == 1);
 }
+#endif
 
 auto observesStop(ManualEvent& gate, bool& sawStop)
     CO2_BEG(co2::Task<>, (gate, sawStop), co2::stop_token token;) {
@@ -346,10 +348,6 @@ struct CancellableApi {
     std::function<void(bool)> handler;
 };
 
-struct Cancelled : std::runtime_error {
-    Cancelled() : std::runtime_error{"cancelled"} {}
-};
-
 auto cancelsSynchronously(CancellableApi& api)
     CO2_BEG(co2::Task<int>, (api), int got{};) {
     CO2_AWAIT_AS_SET(
@@ -361,7 +359,7 @@ auto cancelsSynchronously(CancellableApi& api)
             api.handler = [done, guard](bool const cancelled) mutable {
                 guard.reset();
                 if (cancelled)
-                    done.setException(std::make_exception_ptr(Cancelled{}));
+                    done.setStopped();
                 else
                     done(1);
             };
@@ -393,15 +391,11 @@ void parentStopMayTearDownTheWhenAllFromInsideItsForwardingCallback() {
 
     CHECK(parent.request_stop());
     CHECK(co2::detail::TaskAccess::isDone(task));
-    bool thrown = false;
-    try {
-        co2::detail::TaskAccess::takeResult(task);
-    } catch (Cancelled const&) {
-        thrown = true;
-    }
-    CHECK(thrown);
+    // 两个 child 都以已停止完成，whenAll 与等待它的 Task 也就以已停止结束。
+    CHECK(co2::detail::TaskAccess::stateOf(task) == co2::detail::TaskState::stopped);
 }
 
+#if CO2_HAS_EXCEPTIONS
 auto failsImmediately(int which) CO2_BEG(co2::Task<int>, (which)) {
     throw ExpectedError{which};
     CO2_RETURN(0);
@@ -446,6 +440,7 @@ void vectorFailureRethrowsTheFirstInTime() {
     }
     CHECK(thrown);
 }
+#endif
 
 // ---------------------------------------------------------------------------
 // 嵌套与规模
@@ -515,12 +510,16 @@ int main() {
     emptyVectorCompletesImmediately();
     childrenStartInOrderAndCompleteInAnyOrder();
     childrenRunConcurrentlyOnAThreadPool();
+#if CO2_HAS_EXCEPTIONS
     firstFailureInTimeIsRethrownAfterEveryChildFinishes();
+#endif
     parentStopRequestReachesEveryChild();
     alreadyStoppedParentStartsChildrenStopped();
     parentStopMayTearDownTheWhenAllFromInsideItsForwardingCallback();
+#if CO2_HAS_EXCEPTIONS
     synchronousFailureStillWaitsForSiblingsAndRethrows();
     vectorFailureRethrowsTheFirstInTime();
+#endif
     nestedWhenAllOverManyChildrenOnAPool();
     whenAllWorksInsideASpawnedTask();
     repeatedWhenAllDoesNotLeakOrCorrupt();
````

- [ ] **Step 4: 规范用例与分配测试**

`trace.hpp` 是规范用例与差分测试共用的；它原来不包含任何 co2 的头文件，现在为了 `CO2_HAS_EXCEPTIONS` 包含 `co2/config.hpp`。`conformance_tests.cpp` 里依赖异常的两大段整段包起来，`#else` 分支是禁用异常时才有的用例。`allocation_tests.cpp` 里替换的全局 `operator new` 在分配失败时原来 `throw std::bad_alloc{}`，禁用异常时改为 `std::abort()`。

修改 `tests/trace.hpp`：

````diff
diff --git a/tests/trace.hpp b/tests/trace.hpp
index 8d0976e..2d26ee2 100644
--- a/tests/trace.hpp
+++ b/tests/trace.hpp
@@ -14,6 +14,8 @@
 #include <utility>
 #include <vector>
 
+#include "co2/config.hpp" // CO2_HAS_EXCEPTIONS：抛出的那几个类型只在有异常时存在
+
 namespace trace {
 
 struct EventLog {
@@ -133,6 +135,7 @@ template <class Lib> struct Types {
         void await_resume() const { log->add("transfer await_resume"); }
     };
 
+#if CO2_HAS_EXCEPTIONS
     // 各阶段可抛出。
     struct Throwing {
         EventLog* log;
@@ -149,6 +152,7 @@ template <class Lib> struct Types {
             if (throwAt == 3) throw std::runtime_error{"await_resume"};
         }
     };
+#endif
 
     // 标准惯用法：借 await_suspend 拿到自己的 promise，返回 false 立即恢复。
     template <class Promise> struct GetPromise {
@@ -166,8 +170,10 @@ template <class Lib> struct Types {
     template <class Return> struct PromiseBase {
         EventLog* log{};
         bool lazy{true};
+#if CO2_HAS_EXCEPTIONS
         bool rethrowUnhandled{};
         std::exception_ptr error;
+#endif
 
         // 能用参数副本构造时标准要求传入全部参数副本（左值）；多余的参数忽略。
         template <class... Rest>
@@ -220,8 +226,10 @@ template <class Lib> struct Types {
 
         void unhandled_exception() {
             log->add("unhandled_exception");
+#if CO2_HAS_EXCEPTIONS
             error = std::current_exception();
             if (rethrowUnhandled) throw;
+#endif
         }
 
         // await_transform：把 int 变成立即就绪、返回该值两倍的 awaiter。
@@ -299,6 +307,7 @@ template <class Lib> struct Types {
         using Owner<VoidPromise>::Owner;
     };
 
+#if CO2_HAS_EXCEPTIONS
     // initial_suspend 的 awaiter 在 await_resume 里抛出。标准在求值 await-resume 之前就把
     // initial-await-resume-called 置真，所以这个异常进入 unhandled_exception，协程走到
     // final suspend，帧仍由拥有者销毁。
@@ -328,6 +337,7 @@ template <class Lib> struct Types {
         using promise_type = StartThrowsPromise;
         using Owner<StartThrowsPromise>::Owner;
     };
+#endif
 
     // final_suspend 不挂起：协程状态自动销毁，返回对象不拥有任何东西。
     struct FireAndForget {
@@ -413,12 +423,14 @@ typename Types<Lib>::VoidTask Types<Lib>::VoidPromise::get_return_object() {
     return VoidTask{Handle<VoidPromise>::from_promise(*this)};
 }
 
+#if CO2_HAS_EXCEPTIONS
 template <class Lib>
 typename Types<Lib>::StartThrowsTask
 Types<Lib>::StartThrowsPromise::get_return_object() {
     this->log->add("get_return_object");
     return StartThrowsTask{Handle<StartThrowsPromise>::from_promise(*this)};
 }
+#endif
 
 // 带 operator co_await 的 awaitable。C++14 下拼作 operator_co_await；C++20 构建再补上
 // 真正的 operator co_await（见各测试文件）。
````

修改 `tests/conformance_tests.cpp`：

````diff
diff --git a/tests/conformance_tests.cpp b/tests/conformance_tests.cpp
index a1a9418..94fd24b 100644
--- a/tests/conformance_tests.cpp
+++ b/tests/conformance_tests.cpp
@@ -142,6 +142,7 @@ void returnVoidAndFallingOffTheEndBothCallReturnVoid() {
     }
 }
 
+#if CO2_HAS_EXCEPTIONS
 // ---------------------------------------------------------------------------
 // unhandled_exception
 
@@ -401,6 +402,35 @@ void initialAwaitResumeExceptionsReachUnhandledException() {
                       "final_suspend await_suspend");
     }
 }
+#else
+// ---------------------------------------------------------------------------
+// 禁用异常的构建：协程体变换里没有 catch，promise 不需要 unhandled_exception()——与原生
+// 编译器在 -fno-exceptions 下的要求一致。
+
+struct NoHandlerTask {
+    struct promise_type {
+        promise_type() = default; // 不拿协程的参数去初始化
+
+        NoHandlerTask get_return_object() noexcept { return {}; }
+        co2::suspend_never initial_suspend() noexcept { return {}; }
+        co2::suspend_never final_suspend() noexcept { return {}; }
+        void return_void() noexcept {}
+    };
+};
+
+auto runsWithoutAHandler(int& ran) CO2_BEG(NoHandlerTask, (ran)) {
+    ++ran;
+    CO2_AWAIT(co2::suspend_never{});
+    ++ran;
+}
+CO2_END
+
+void aPromiseNeedsNoUnhandledExceptionWithoutExceptions() {
+    auto ran = 0;
+    runsWithoutAHandler(ran);
+    CHECK(ran == 2);
+}
+#endif
 
 // ---------------------------------------------------------------------------
 // [expr.await]：await_suspend 的三种返回类型
@@ -524,6 +554,7 @@ void noopCoroutineReturnsControlToTheResumer() {
     CHECK(task.done());
 }
 
+#if CO2_HAS_EXCEPTIONS
 // ---------------------------------------------------------------------------
 // awaiter 抛出：异常在 co_await 处抛出，落入协程体 → unhandled_exception
 
@@ -553,6 +584,7 @@ void awaiterExceptionsAreDeliveredIntoTheBody() {
         CHECK(task.promise().error != nullptr);
     }
 }
+#endif
 
 // ---------------------------------------------------------------------------
 // await_transform 只作用于 co_await，不作用于 co_yield
@@ -1234,19 +1266,25 @@ int main() {
     lazyCoroutineFollowsTheStandardOrder();
     eagerCoroutineRunsInsideTheRamp();
     returnVoidAndFallingOffTheEndBothCallReturnVoid();
+#if CO2_HAS_EXCEPTIONS
     bodyExceptionsReachUnhandledException();
     localsAreDestroyedBeforeUnhandledException();
     rethrowingUnhandledExceptionLeavesTheCoroutineAtFinalSuspend();
     exceptionsBeforeTheInitialAwaitResumePropagateToTheCaller();
     aFrameThatFailsHalfwayThroughConstructionIsRolledBack();
     initialAwaitResumeExceptionsReachUnhandledException();
+#else
+    aPromiseNeedsNoUnhandledExceptionWithoutExceptions();
+#endif
     awaitSuspendReturningVoidSuspends();
     readyAwaiterNeverSuspends();
     awaitSuspendReturningFalseResumesImmediately();
     awaitSuspendReturningTrueSuspends();
     awaitSuspendReturningAHandleTransfersControl();
     noopCoroutineReturnsControlToTheResumer();
+#if CO2_HAS_EXCEPTIONS
     awaiterExceptionsAreDeliveredIntoTheBody();
+#endif
     awaitTransformAppliesToCoAwaitOnly();
     operatorCoAwaitIsLookedUpAsMemberThenAdl();
     lvalueAwaitersAreUsedInPlace();
````

修改 `tests/allocation_tests.cpp`：

````diff
diff --git a/tests/allocation_tests.cpp b/tests/allocation_tests.cpp
index a676389..59df85d 100644
--- a/tests/allocation_tests.cpp
+++ b/tests/allocation_tests.cpp
@@ -373,12 +373,16 @@ void spawnIsTwoAllocationsBeyondTheFrame() {
 // 超对齐的 awaiter 在堆上手工对齐：先要一整块，再在块里构造。reset() 归还那一块；构造
 // 抛出时那一块也必须归还，槽保持为空。异常类型是空的结构体：std::runtime_error 自己会
 // 分配，干扰计数。
+#if CO2_HAS_EXCEPTIONS
 struct ConstructionFailed {};
+#endif
 
 struct alignas(64) OverAlignedThrowsOnMove {
     explicit OverAlignedThrowsOnMove(bool const armed_) noexcept : armed{armed_} {}
     OverAlignedThrowsOnMove(OverAlignedThrowsOnMove&& other) : armed{other.armed} {
+#if CO2_HAS_EXCEPTIONS
         if (armed) throw ConstructionFailed{};
+#endif
     }
 
     bool armed;
@@ -399,6 +403,7 @@ void anOverAlignedAwaiterAlwaysReturnsItsBlock() {
     slot.reset();
     CHECK(deallocationsSoFar() - deallocationsBefore == 1);
 
+#if CO2_HAS_EXCEPTIONS
     // 构造抛出：那一块当场归还，槽保持为空。
     auto thrown = false;
     try {
@@ -410,6 +415,7 @@ void anOverAlignedAwaiterAlwaysReturnsItsBlock() {
     CHECK(not slot.hasValue());
     CHECK(allocationsSoFar() - allocationsBefore == 2);
     CHECK(deallocationsSoFar() - deallocationsBefore == 2);
+#endif
 }
 
 } // namespace
@@ -427,7 +433,11 @@ void* operator new(std::size_t const size) {
     allocations.fetch_add(1, std::memory_order_relaxed);
     allocatedBytes.fetch_add(static_cast<long>(size), std::memory_order_relaxed);
     if (auto* const pointer = std::malloc(size == 0U ? 1U : size)) return pointer;
+#if CO2_HAS_EXCEPTIONS
     throw std::bad_alloc{};
+#else
+    std::abort();
+#endif
 }
 
 void operator delete(void* const pointer) noexcept {
````

- [ ] **Step 5: 其余测试文件**

这五个文件都只是把依赖异常的用例包起来。

修改 `tests/return_types_tests.cpp`：

````diff
diff --git a/tests/return_types_tests.cpp b/tests/return_types_tests.cpp
index 57c52ec..86b2981 100644
--- a/tests/return_types_tests.cpp
+++ b/tests/return_types_tests.cpp
@@ -141,6 +141,7 @@ void voidTaskFlowsOffTheEnd() {
     CHECK(counter == 2);
 }
 
+#if CO2_HAS_EXCEPTIONS
 auto throwingLeaf() CO2_BEG(co2::Task<int>, ()) {
     throw ExpectedError{};
     CO2_RETURN(0);
@@ -171,6 +172,7 @@ void taskPropagatesExceptionsAndDestroysLocals() {
     }
     CHECK(thrown);
 }
+#endif
 
 auto chain(int depth) CO2_BEG(co2::Task<int>, (depth), int inner{};) {
     if (depth == 0) CO2_RETURN(0);
@@ -449,6 +451,7 @@ void lvalueGeneratorsAreTreatedAsRanges() {
     CHECK((seen == std::vector<int>{0, 1, 2}));
 }
 
+#if CO2_HAS_EXCEPTIONS
 auto failingInner(Counters& counters)
     CO2_BEG(co2::Generator<int>, (counters),
             co2::detail::ResultStorage<Tracked> local;) {
@@ -490,6 +493,7 @@ void nestedExceptionsPropagateThroughTheParentToTheConsumer() {
     // 两层的局部都已随各自的协程体销毁；帧本身留到 generator 析构。
     CHECK(counters.alive() == 0);
 }
+#endif
 
 auto holdsLocal(Counters& counters, int depth)
     CO2_BEG(co2::Generator<int>, (counters, depth),
@@ -619,6 +623,7 @@ void asyncGeneratorSuspendsTheConsumerWhileTheProducerWaits() {
     CHECK(co2::detail::TaskAccess::takeResult(task) == 2);
 }
 
+#if CO2_HAS_EXCEPTIONS
 auto failingTicks(Counters& counters)
     CO2_BEG(co2::AsyncGenerator<int>, (counters),
             co2::detail::ResultStorage<Tracked> local;) {
@@ -657,6 +662,7 @@ void asyncGeneratorExceptionsReachTheConsumer() {
     CHECK(thrown);
     CHECK(counters.alive() == 0);
 }
+#endif
 
 auto trackedTicks(Counters& counters) CO2_BEG(co2::AsyncGenerator<int>, (counters),
                                               co2::detail::ResultStorage<Tracked> local;
@@ -693,7 +699,9 @@ void asyncGeneratorCanBeDestroyedBetweenElements() {
 int main() {
     taskIsLazyAndRunsChildrenInline();
     voidTaskFlowsOffTheEnd();
+#if CO2_HAS_EXCEPTIONS
     taskPropagatesExceptionsAndDestroysLocals();
+#endif
     deepAwaitChainDoesNotGrowTheNativeStack();
     awaitingAnLvalueTaskBorrowsIt();
     moveOnlyResultsAreMovedOut();
@@ -711,14 +719,18 @@ int main() {
     deepNestingDoesNotGrowTheNativeStackWhileIterating();
     elementsOfAcceptsArbitraryRanges();
     lvalueGeneratorsAreTreatedAsRanges();
+#if CO2_HAS_EXCEPTIONS
     nestedExceptionsPropagateThroughTheParentToTheConsumer();
+#endif
     destroyingAGeneratorMidIterationDestroysEveryActiveFrame();
     destroyingADeeplyNestedGeneratorMidIterationDoesNotRecurse();
     generatorsAreMovable();
 
     asyncGeneratorYieldsSynchronously();
     asyncGeneratorSuspendsTheConsumerWhileTheProducerWaits();
+#if CO2_HAS_EXCEPTIONS
     asyncGeneratorExceptionsReachTheConsumer();
+#endif
     asyncGeneratorCanBeDestroyedBetweenElements();
     return 0;
 }
````

修改 `tests/scheduler_tests.cpp`：

````diff
diff --git a/tests/scheduler_tests.cpp b/tests/scheduler_tests.cpp
index 53a668c..333f6e7 100644
--- a/tests/scheduler_tests.cpp
+++ b/tests/scheduler_tests.cpp
@@ -95,11 +95,13 @@ auto nothing(int& counter) CO2_BEG(co2::Task<>, (counter)) {
 }
 CO2_END
 
+#if CO2_HAS_EXCEPTIONS
 auto failing() CO2_BEG(co2::Task<int>, ()) {
     throw ExpectedError{};
     CO2_RETURN(0);
 }
 CO2_END
+#endif
 
 void syncWaitRunsASynchronousTaskInline() {
     CHECK(co2::syncWait(answer(7)) == 7);
@@ -130,6 +132,7 @@ void syncWaitReturnsAnOptional() {
     CHECK(counter == 1);
 }
 
+#if CO2_HAS_EXCEPTIONS
 void syncWaitRethrowsTheTaskException() {
     bool thrown = false;
     try {
@@ -139,6 +142,7 @@ void syncWaitRethrowsTheTaskException() {
     }
     CHECK(thrown);
 }
+#endif
 
 auto uniqueAnswer() CO2_BEG(co2::Task<std::unique_ptr<int>>, ()) {
     CO2_RETURN(std::unique_ptr<int>{new int{3}});
@@ -172,6 +176,7 @@ void syncWaitWakesWhenTheTaskCompletesOnAnotherThread() {
     CHECK(resumedOn == driverId);
 }
 
+#if CO2_HAS_EXCEPTIONS
 auto failsElsewhere(co2::ManualExecutor& executor) CO2_BEG(co2::Task<int>, (executor)) {
     CO2_AWAIT(co2::scheduleOn(executor));
     throw ExpectedError{};
@@ -194,6 +199,7 @@ void syncWaitRethrowsExceptionsFromAnotherThread() {
     driver.join();
     CHECK(thrown);
 }
+#endif
 
 } // namespace
 
@@ -202,9 +208,13 @@ int main() {
     manualExecutorIsFifoAndRunDrainsRescheduledWork();
     syncWaitRunsASynchronousTaskInline();
     syncWaitReturnsAnOptional();
+#if CO2_HAS_EXCEPTIONS
     syncWaitRethrowsTheTaskException();
+#endif
     syncWaitMovesOutMoveOnlyResults();
     syncWaitWakesWhenTheTaskCompletesOnAnotherThread();
+#if CO2_HAS_EXCEPTIONS
     syncWaitRethrowsExceptionsFromAnotherThread();
+#endif
     return 0;
 }
````

修改 `tests/spawn_tests.cpp`：

````diff
diff --git a/tests/spawn_tests.cpp b/tests/spawn_tests.cpp
index 7abb467..69c5f79 100644
--- a/tests/spawn_tests.cpp
+++ b/tests/spawn_tests.cpp
@@ -103,6 +103,7 @@ void joinReturnsAnOptional() {
     CHECK(counter.load() == 1);
 }
 
+#if CO2_HAS_EXCEPTIONS
 auto failing() CO2_BEG(co2::Task<int>, ()) {
     throw ExpectedError{};
     CO2_RETURN(0);
@@ -120,6 +121,7 @@ void joinRethrowsTheTaskException() {
     }
     CHECK(thrown);
 }
+#endif
 
 auto uniqueAnswer() CO2_BEG(co2::Task<std::unique_ptr<int>>, ()) {
     CO2_RETURN(std::unique_ptr<int>{new int{9}});
@@ -354,7 +356,9 @@ int main() {
     spawnOnAManualExecutorStartsWhenItRuns();
     spawnOnAThreadPoolJoinsFromTheCallingThread();
     joinReturnsAnOptional();
+#if CO2_HAS_EXCEPTIONS
     joinRethrowsTheTaskException();
+#endif
     joinMovesOutMoveOnlyResults();
     joinHandlesAreAwaitable();
     awaitingAnAlreadyCompletedHandleDoesNotSuspend();
````

修改 `tests/thread_pool_tests.cpp`：

````diff
diff --git a/tests/thread_pool_tests.cpp b/tests/thread_pool_tests.cpp
index 9f647b4..77fdcd1 100644
--- a/tests/thread_pool_tests.cpp
+++ b/tests/thread_pool_tests.cpp
@@ -131,6 +131,7 @@ void scheduleOnMovesTheCoroutineToAWorkerThread() {
     CHECK(resumedOn != std::this_thread::get_id());
 }
 
+#if CO2_HAS_EXCEPTIONS
 auto failsOnWorker(co2::ThreadPool& pool) CO2_BEG(co2::Task<int>, (pool)) {
     CO2_AWAIT(co2::scheduleOn(pool));
     throw ExpectedError{};
@@ -148,6 +149,7 @@ void exceptionsFromWorkersReachSyncWait() {
     }
     CHECK(thrown);
 }
+#endif
 
 void injectedWorkIsSpreadAcrossWorkers() {
     co2::ThreadPool pool{4U};
@@ -460,7 +462,9 @@ int main() {
     everyWorkerIsRecruitedForLocallyQueuedWork();
     aWorkerBlockedOnItsOwnQueuedWorkIsRescuedByAnother();
     scheduleOnMovesTheCoroutineToAWorkerThread();
+#if CO2_HAS_EXCEPTIONS
     exceptionsFromWorkersReachSyncWait();
+#endif
     injectedWorkIsSpreadAcrossWorkers();
     localWorkIsStolenByIdleWorkers();
     localQueueOverflowSpillsToTheGlobalQueue();
````

修改 `tests/edge_case_tests.cpp`：

````diff
diff --git a/tests/edge_case_tests.cpp b/tests/edge_case_tests.cpp
index fefcc15..babe84e 100644
--- a/tests/edge_case_tests.cpp
+++ b/tests/edge_case_tests.cpp
@@ -206,6 +206,7 @@ void unstartedGeneratorReleasesItsParameters() {
     CHECK(counters.alive() == 0);
 }
 
+#if CO2_HAS_EXCEPTIONS
 auto throwsBeforeFirstYield() CO2_BEG(co2::Generator<int>, ()) {
     throw ExpectedError{};
     CO2_YIELD(0);
@@ -243,6 +244,7 @@ void nestedGeneratorThrowingBeforeItsFirstYieldPropagates() {
     CHECK(thrown);
     CHECK(it == generator.end());
 }
+#endif
 
 auto stringsByConstRef(std::vector<std::string> const& items)
     CO2_BEG((co2::Generator<std::string const&, std::string>), (items),
@@ -523,8 +525,10 @@ int main() {
     macroArgumentsMayContainManyTopLevelCommas();
     largeAwaitersFallBackToTheHeapTransparently();
     unstartedGeneratorReleasesItsParameters();
+#if CO2_HAS_EXCEPTIONS
     generatorThrowingBeforeTheFirstYieldThrowsFromBegin();
     nestedGeneratorThrowingBeforeItsFirstYieldPropagates();
+#endif
     explicitValueTypeCopiesOnlyWhenTheConsumerAsks();
     constRvaluesYieldedByConstReferenceAreKeptAlive();
     asyncGeneratorConsumerFollowsTheProducerAcrossThreads();
````

- [ ] **Step 6: 两种构建都通过**

开着异常的构建，运行全部测试。Expected: `100% tests passed out of 31`。

禁用异常的构建，GCC 与 Clang 各一遍，运行全部测试。Expected: 都是 `100% tests passed out of 32`——比开着异常时少差分测试和"错误被取两次"那一个，多三个只在禁用异常时成立的契约违规测试。

- [ ] **Step 7: 提交**

```bash
git add tests/completion_support.hpp tests/completion_tests.cpp \
        tests/completion_components_tests.cpp tests/vocabulary_tests.cpp \
        tests/callback_tests.cpp tests/when_all_tests.cpp tests/conformance_tests.cpp \
        tests/trace.hpp tests/allocation_tests.cpp tests/return_types_tests.cpp \
        tests/scheduler_tests.cpp tests/spawn_tests.cpp tests/thread_pool_tests.cpp \
        tests/edge_case_tests.cpp
git commit -F - <<'EOF'
测试在开、关异常两种构建下都能编译和运行

依赖 throw / catch 的用例连同它们用到的协程与类型，用 CO2_HAS_EXCEPTIONS 包起来；
其余的原样在两种构建下跑。为了让关掉异常时仍然测得到东西，有几处不只是加条件编译：

- 两个"API 被取消"的用例（fromCallback 接 stop_token、whenAll 在转发回调里被拆掉）
  原来让续体以一个 Cancelled 异常完成，现在调用 setStopped()。它们测的是停止请求的
  转发与生命周期，与用哪个出口无关。
- 沿 Task 链传播错误的用例改成在根上按值看结果（sample::outcomeOf，即
  syncWait(asResult(task))）；"根上把错误码抛成 system_error"单独成一个只在开异常时
  跑的用例。
- 新增：只装错误码的 Error 的复制、移动与赋值；Error::exception() 与
  Continuation::setException 恰好在有异常时存在；关掉异常时没有
  unhandled_exception() 的 promise 能用。

Leaf::throws 与 Exit::exception 这两个测试用的枚举项在关掉异常时不存在，误用它们的
测试会编不过，而不是悄悄跳过。与原生 co_await 的差分测试比对的事件序列里有异常
路径，关掉异常时不构建。

验证：GCC 与 Clang 的 -fno-exceptions -fno-rtti 下 32 个测试全部通过；开着异常的
31 个测试照旧通过。
EOF
```

### Task 4: CI 作业与 Preset

**Files:**
- Modify: `.github/workflows/ci.yml`
- Modify: `CMakePresets.json`

**Interfaces:**
- Consumes: 选项 `CO2_TEST_NO_EXCEPTIONS`（任务 2）；两种构建都通过的测试（任务 3）。
- Produces: CI 作业 `no-exceptions`（`g++` / `clang++` × `Debug` / `Release`）；CMake Preset `no-exceptions`（configure / build / test 三种都有）。

- [ ] **Step 1: 加作业与 Preset**

Release 也跑：没有异常边之后，优化器看到的控制流与开着异常时不同。

修改 `.github/workflows/ci.yml`：

````diff
diff --git a/.github/workflows/ci.yml b/.github/workflows/ci.yml
index dae15b9..31d2bea 100644
--- a/.github/workflows/ci.yml
+++ b/.github/workflows/ci.yml
@@ -105,6 +105,39 @@ jobs:
       - name: Test
         run: ctest --test-dir build --output-on-failure --timeout 120
 
+  # 禁用异常的构建：全部测试以 -fno-exceptions -fno-rtti 编译（CO2_TEST_NO_EXCEPTIONS）。
+  # 库从编译器的开关得出 CO2_HAS_EXCEPTIONS，去掉 try / catch 与装异常的那一半；值、
+  # 错误码、已停止三种完成方式照常工作，错误到达根则是契约违规。Release 也跑：没有
+  # 异常边之后优化器看到的控制流不同。
+  no-exceptions:
+    name: ${{ matrix.compiler }} ${{ matrix.build_type }} -fno-exceptions
+    runs-on: ubuntu-latest
+    strategy:
+      fail-fast: false
+      matrix:
+        compiler: [g++, clang++]
+        build_type: [Debug, Release]
+    steps:
+      - uses: actions/checkout@v4
+
+      - name: Install packages
+        run: |
+          sudo apt-get update
+          sudo apt-get install -y ninja-build clang
+
+      - name: Configure
+        run: |
+          cmake -S . -B build -G Ninja \
+            -DCMAKE_BUILD_TYPE=${{ matrix.build_type }} \
+            -DCMAKE_CXX_COMPILER=${{ matrix.compiler }} \
+            -DCO2_TEST_NO_EXCEPTIONS=ON
+
+      - name: Build
+        run: cmake --build build
+
+      - name: Test
+        run: ctest --test-dir build --output-on-failure --timeout 120
+
   msvc:
     name: MSVC ${{ matrix.toolset }} ${{ matrix.build_type }}
     runs-on: ${{ matrix.os }}
````

修改 `CMakePresets.json`：

````diff
diff --git a/CMakePresets.json b/CMakePresets.json
index 7f70205..10c71e6 100644
--- a/CMakePresets.json
+++ b/CMakePresets.json
@@ -48,6 +48,15 @@
                 "CMAKE_CXX_FLAGS": "-fsanitize=thread -fno-omit-frame-pointer -g"
             }
         },
+        {
+            "name": "no-exceptions",
+            "displayName": "Debug, -fno-exceptions -fno-rtti",
+            "inherits": "base",
+            "cacheVariables": {
+                "CMAKE_BUILD_TYPE": "Debug",
+                "CO2_TEST_NO_EXCEPTIONS": "ON"
+            }
+        },
         {
             "name": "release-bench",
             "displayName": "Release benchmarks (-O2)",
@@ -77,6 +86,10 @@
             "name": "tsan",
             "configurePreset": "tsan"
         },
+        {
+            "name": "no-exceptions",
+            "configurePreset": "no-exceptions"
+        },
         {
             "name": "release-bench",
             "configurePreset": "release-bench"
@@ -114,6 +127,13 @@
             "output": {
                 "outputOnFailure": true
             }
+        },
+        {
+            "name": "no-exceptions",
+            "configurePreset": "no-exceptions",
+            "output": {
+                "outputOnFailure": true
+            }
         }
     ]
 }
````

- [ ] **Step 2: 验证**

两个文件都能解析，作业与 Preset 在里面：

```bash
python3 - <<'EOF'
import json, yaml
jobs = yaml.safe_load(open('.github/workflows/ci.yml'))['jobs']
print(jobs['no-exceptions']['strategy']['matrix'])
presets = json.load(open('CMakePresets.json'))
print([[p['name'] for p in presets[k]].count('no-exceptions')
       for k in ('configurePresets', 'buildPresets', 'testPresets')])
EOF
```

Expected:

```
{'compiler': ['g++', 'clang++'], 'build_type': ['Debug', 'Release']}
[1, 1, 1]
```

用作业里的命令在本机跑那四个组合（构建目录换到仓库之外）：

```bash
for cxx in g++ clang++; do for type in Debug Release; do
  b=/tmp/co2-build/ci-noexc-$cxx-$type
  cmake -S . -B $b -G Ninja -DCMAKE_BUILD_TYPE=$type -DCMAKE_CXX_COMPILER=$cxx \
        -DCO2_TEST_NO_EXCEPTIONS=ON > $b.cfg.log 2>&1
  cmake --build $b > $b.build.log 2>&1
  echo "$cxx $type: $(ctest --test-dir $b -j 8 2>&1 | grep 'tests passed')"
done; done
```

Expected: 四行都是 `100% tests passed out of 32`。

Preset 的构建目录在仓库的 `build/` 下，那个目录不属于这次工作，所以在一份副本里验证它：

```bash
rm -rf /tmp/co2-build/preset-copy && mkdir -p /tmp/co2-build/preset-copy
git ls-files -z | xargs -0 cp --parents -t /tmp/co2-build/preset-copy
(cd /tmp/co2-build/preset-copy && cmake --preset no-exceptions > /dev/null &&
 cmake --build --preset no-exceptions > /dev/null && ctest --preset no-exceptions | tail -3)
```

Expected: `100% tests passed out of 32`。

- [ ] **Step 3: 提交**

```bash
git add .github/workflows/ci.yml CMakePresets.json
git commit -F - <<'EOF'
CI 与 Preset：禁用异常的构建

CI 新增 no-exceptions 作业：GCC 与 Clang 各以 Debug、Release 用
-DCO2_TEST_NO_EXCEPTIONS=ON 构建并跑全部测试。Release 也跑，是因为没有异常边之后
优化器看到的控制流不同。CMakePresets.json 加上同样用途的 no-exceptions。

本机用作业里的命令跑过这四个组合，也用 Preset 跑过一遍，都是 32 个测试全部通过。
CI 本身还没有跑过：这些提交没有推送。
EOF
```

### Task 5: 文档与全配置验证

设计稿第 10 步："核心设计稿与 README 的对应章节、偏离清单、迁移说明"。前两份计划已经写了 README 的"错误与已停止"一节、核心设计稿的偏离 D15 和基准的两行；这里补上禁用异常的构建、升级说明，并把设计稿标为全部实现。

**Files:**
- Modify: `README.md`
- Modify: `docs/architecture.md`
- Modify: `docs/design/completion-channels.md`

**Interfaces:**
- Consumes: 任务 1～4 的全部结果。
- Produces: 只有文档。

- [ ] **Step 1: README**

新增"禁用异常的构建"一节（紧跟在"错误与已停止"之后）与"升级说明"一节（在"性能基准"之前）；`fromCallback` 的四个出口里注明 `setException` 只在开异常时存在；"构建与测试"写明 `CO2_TEST_NO_EXCEPTIONS` 与新的 Preset。

修改 `README.md`：

````diff
diff --git a/README.md b/README.md
index 946f805..5d8ec3f 100644
--- a/README.md
+++ b/README.md
@@ -237,8 +237,40 @@ CO2_AWAIT(co2::unstoppable(co2::scheduleOn(pool)));   // 对这一次等待隐
   协程体内重新抛出，走 `unhandled_exception()`；没有 `return_stopped()` 时"已停止"是
   契约违规。
 
-完整的设计与落地顺序见 `docs/design/completion-channels.md`。禁用异常的构建
-（`-fno-exceptions`）还没有做，是那份设计的最后一步。
+完整的设计见 `docs/design/completion-channels.md`。
+
+## 禁用异常的构建
+
+库可以在 `-fno-exceptions`（通常连同 `-fno-rtti`）下编译和使用，不需要定义任何宏：
+`config.hpp` 从编译器的开关得出 `CO2_HAS_EXCEPTIONS`。值、错误码、已停止三种完成方式
+在两种构建下是同一份代码，上一节的写法原样适用：失败用 `co2::just_error(code)` 产生，
+想自己处理就套 `co2::asResult`。
+
+```cpp
+// 根上按值拿结果。asResult 把三种结果都装进 Result，所以 optional 总是有值。
+auto const outcome = co2::syncWait(co2::asResult(serve(socket)));
+if (outcome->hasError()) log(outcome->error().code());
+if (outcome->isStopped()) log("cancelled");
+```
+
+与开着异常时不同的地方：
+
+- **错误走到只能靠抛出来报告它的地方，是契约违规。** 这样的地方有三处：根
+  （`syncWait(task)` / `join()`）、没有 `return_error()` 的自定义 promise、没有转发
+  `await_completion()` 的包装 awaiter。在等待的地方套 `co2::asResult`，错误就是一个值。
+- **`co2::Error` 只装错误码。** `Error::exception()` 与 `Continuation::setException`
+  不存在，续体剩下值、错误码、已停止三个出口。
+- **promise 不需要 `unhandled_exception()`**，与原生编译器在 `-fno-exceptions` 下的
+  要求一致。`Generator` 体内不能等待，这种构建里它不会失败。
+- **`get_return_object_on_allocation_failure` 只在 promise 自己的 `operator new` 返回空
+  时生效。** allocator 与全局 `operator new` 以 `bad_alloc` 报告失败，没有异常时接不住，
+  后果由运行时决定（通常是终止）。
+
+**同一个程序的所有翻译单元必须用同一个异常开关编译。** 头文件里的内联函数在两种开关下
+是两份不同的定义，混用是静默的 ODR 违规。`CO2_HAS_EXCEPTIONS` 不能手动定义，定义了就
+是编译错误；MSVC 上另有一行 `#pragma detect_mismatch` 在链接期检查。
+
+GCC 与 Clang 上的这种构建由 CI 覆盖（见"构建与测试"）。MSVC 关掉异常的构建没有测试过。
 
 ## 执行层
 
@@ -348,7 +380,7 @@ CO2_END
 ```cpp
 done(args...);              // 有值：就地构造结果
 done.setError(code);        // 出错：一个 std::error_code
-done.setException(error);   // 出错：一个捕获的异常
+done.setException(error);   // 出错：一个捕获的异常（禁用异常的构建里没有）
 done.setStopped();          // 已停止：操作被取消了
 ```
 
@@ -435,6 +467,34 @@ v2 核心重写后，以下 v1 功能尚未在新模型上重建：事件 / 协
 形态）、C++20 `std::coroutine_handle` 互操作层。它们将在标准销毁契约与 `stop_token`
 之上重新实现。（`whenAll` 与回调适配已经重建，见上。）
 
+## 升级说明
+
+三种完成方式改动了几处已有的接口与行为。从更早的 `0.3.0-pre` 快照升级时逐条对照：
+
+1. **`syncWait` / `join()` 返回 `co2::optional<T>`**，`void` 是
+   `co2::optional<co2::Unit>`，已停止为空。取值写 `*co2::syncWait(task)`。结果是 `bool`
+   的根要留意：`if (co2::syncWait(task))` 照样能编译，判断的却是"有没有值"。
+2. **`scheduleOn` 是取消点。** 轮询令牌之后正常返回的协程，收到停止请求之后会先在下一次
+   `scheduleOn` 结束，走不到自己的判断。要在停止之后继续换线程，写
+   `co2::unstoppable(co2::scheduleOn(s))`。
+3. **丢弃 `JoinHandle` 的 Task 会在下一个取消点被停掉。** 从前丢弃只是发出停止请求，
+   不轮询的 Task 照样跑完。要让它跑完，保留句柄，或者把 Task 里的 hop 用 `unstoppable`
+   包住。
+4. **`whenAll` 的子 Task 已停止，整个 `whenAll` 就是已停止**（有出错的则以第一个错误
+   完成）。从前只有失败影响结果。
+5. **根上抛出的异常。** 协程体里抛出的异常原样重抛，与从前一样；以错误码结束的 Task
+   抛的是 `std::system_error`。
+6. **一个 Task 的错误只能取走一次**，与值相同。同一个失败的 Task 等待两次，第二次是
+   契约违规；从前第二次会再抛一遍。
+7. **自定义 promise。** 子 Task 失败时，中间各层不再经历 `unhandled_exception()`；
+   promise 提供 `return_error()` 就按值收到错误，不提供则错误在体内重新抛出、照旧走
+   `unhandled_exception()`。提供了 `get_stop_token()` 的 promise 还要补上
+   `return_stopped()`，否则协程在取消点结束时是契约违规。
+8. **只转发标准三个成员的包装 awaiter。** 被包住的操作出错时行为不变
+   （`await_resume()` 照旧抛出）；以已停止完成则是契约违规，要给包装加上
+   `await_completion()` 的转发。
+9. **`Continuation::setException` 只在开着异常的构建里存在。**
+
 ## 性能基准
 
 `benchmarks/co2-benchmarks`，GCC 13.3 `-O2`（`cmake --preset release-bench`），
@@ -473,14 +533,20 @@ ctest --test-dir build --output-on-failure
 ```
 
 也可以使用仓库中的 CMake Preset（需要 CMake 3.21 与 Ninja）：`debug-gcc`、
-`debug-clang`、`asan`、`tsan`（构建与测试，例如 `cmake --preset debug-gcc &&
-cmake --build --preset debug-gcc && ctest --preset debug-gcc`）、`release-bench`
-（性能基准）。
+`debug-clang`、`asan`、`tsan`、`no-exceptions`（构建与测试，例如
+`cmake --preset debug-gcc && cmake --build --preset debug-gcc &&
+ctest --preset debug-gcc`）、`release-bench`（性能基准）。
 
 测试包括规范用例套件（对 [dcl.fct.def.coroutine] / [expr.await] / [coroutine.handle]
 逐条）、C++20 构建下与原生 `co_await` 的差分测试、一组通过契约处理器以退出码 86 结束
-的契约违规测试、以及必须编译失败的用例（挂起点写进 `switch`）。测试默认以 C++14
-构建，`-DCO2_TEST_CXX_STANDARD=20` 可以换成更新的标准；CI 覆盖 14 / 17 / 20 / 23。
+的契约违规测试、以及必须编译失败的用例（挂起点写进 `switch`、手动定义
+`CO2_HAS_EXCEPTIONS`……）。测试默认以 C++14 构建，`-DCO2_TEST_CXX_STANDARD=20` 可以换成
+更新的标准；CI 覆盖 14 / 17 / 20 / 23。
+
+`-DCO2_TEST_NO_EXCEPTIONS=ON`（或 `no-exceptions` 这个 Preset）给全部测试加上
+`-fno-exceptions -fno-rtti`：依赖 `throw` / `catch` 的用例不参加，差分测试不构建，另外
+多出三个"错误无处可抛"的契约违规测试。CI 在 GCC 与 Clang 上各以 Debug、Release 跑
+一遍。这个选项只为 GCC 与 Clang 准备。
 
 作为子项目使用时链接仅含头文件的 CMake target：
 
````

- [ ] **Step 2: 架构文档**

"契约违规"一小节里有一句过时的话——"运行时条件（Task 失败、取消……）以异常报告"——三种完成方式之后已经不成立，一并改掉。

修改 `docs/architecture.md`：

````diff
diff --git a/docs/architecture.md b/docs/architecture.md
index 1e8ac00..1b83a05 100644
--- a/docs/architecture.md
+++ b/docs/architecture.md
@@ -86,7 +86,8 @@ Allocator（只有带状态的 allocator 才在帧里留副本）
 `await_resume`。`CO2_YIELD(e)` ≡ `co_await promise.yield_value(e)`（不经过
 `await_transform`），`CO2_RETURN` ≡ `return_void`/`return_value` + `co_await
 final_suspend()`。异常离开协程体走标准的 `unhandled_exception` 路径；初始挂起点之前的
-异常销毁帧并抛给调用方。
+异常销毁帧并抛给调用方。禁用异常的构建里协程体变换没有 catch，promise 不需要
+`unhandled_exception()`（见"契约"一节的"禁用异常的构建"）。
 
 恢复点比标准多一步（`docs/design/completion-channels.md`）：awaiter 有可选成员
 `await_completion()` 时，先问它这次等待是有值、出错还是已停止。有值照常
@@ -115,7 +116,35 @@ promise 不受影响。
 
 前置条件与不变量违规经由 `CO2_CONTRACT_CHECK` 交给进程级处理器
 （`setContractViolationHandler`），默认 `std::terminate()`。热路径的内部断言在 `NDEBUG`
-下编译掉。运行时条件（Task 失败、取消……）以异常报告。
+下编译掉。运行时条件不是契约违规：Task 失败是一个按值传的 `Error`，取消是"已停止"，
+到了根上错误才变回异常。
+
+### 禁用异常的构建
+
+`config.hpp` 从编译器的开关得出 `CO2_HAS_EXCEPTIONS`（GCC / Clang 的 `__cpp_exceptions`
+与 `__EXCEPTIONS`，MSVC 的 `_CPPUNWIND`），不接受手动定义。值、错误码、已停止三条通道
+不随它变化；为 0 时库里不出现 `try` / `catch` / `throw`，少掉的只有与异常直接相关的
+几处：
+
+- `CoroutineFrame::step` 里包住协程体的 catch，以及各个 promise 的
+  `unhandled_exception()`。
+- `Error` 里装 `exception_ptr` 的那一半、`Error::exception()`、
+  `Continuation::setException`、`GeneratorPromise` 里存异常的成员。
+- 把异常接成错误的三处：`asResult` 对 `await_resume()` 的 catch、`Task` 取值时移动构造
+  的 catch、`Continuation::operator()` 构造结果时的 catch。
+- 靠接住 `bad_alloc` 触发 `get_return_object_on_allocation_failure` 的那条路；promise
+  的 `operator new` 返回空的那条路保留。
+
+`Error::raise()` 在这种构建里是契约违规。它只在错误无处可去时被调用：到了根
+（`syncWait` / `join`）、到了没有 `return_error()` 的 promise、或者经由一个只转发标准
+三个成员的包装 awaiter 的 `await_resume()`。
+
+初始化中途失败时的回滚（创建帧的三步、超对齐块的构造、线程池启动线程）不在上面的
+清单里：它们用局部的守卫对象而不是 `try` / `catch`，两种构建下是同一份代码。
+
+同一个程序的所有翻译单元必须用同一个异常开关编译：头文件里的内联函数在两种开关下是
+两份不同的定义。MSVC 上由 `#pragma detect_mismatch("co2_exceptions", …)` 在链接期
+检查，GCC 与 Clang 上没有检查手段。
 
 ## 返回类型
 
````

- [ ] **Step 3: 设计稿**

状态改为十步全部实现；第 5 节按实现改写；第 9 节写明选项与作业；第 12 节记下第 9 步与原文不同的几处、禁用异常的构建实测过的配置，以及仍未验证的。

修改 `docs/design/completion-channels.md`：

````diff
diff --git a/docs/design/completion-channels.md b/docs/design/completion-channels.md
index 8506bfd..1e777bb 100644
--- a/docs/design/completion-channels.md
+++ b/docs/design/completion-channels.md
@@ -1,11 +1,10 @@
 # 三种完成方式：值、错误、已停止——以及禁用异常的构建
 
-状态：设计已确认（2026-11-12）。第 10 节的第 1～8 步已经实现：词汇类型、根返回
-optional、恢复点与 promise 的两个入口、`Task` 四态、`just_error` / `with_error` /
-`asResult`、四个组件接上 `await_completion()`、`just_stopped` / `stopIfRequested` /
-`unstoppable`、`scheduleOn` 取消点。第 9、10 步（禁用异常的构建、文档收尾）尚未实现。
-设计确认当天对照库的代码评审过一轮并修订；哪些论断验证过、哪些没有，以及实现时与
-本文不同的几处，见第 12 节。
+状态：设计已确认（2026-11-12），第 10 节的十步全部实现：词汇类型、根返回 optional、
+恢复点与 promise 的两个入口、`Task` 四态、`just_error` / `with_error` / `asResult`、
+四个组件接上 `await_completion()`、`just_stopped` / `stopIfRequested` / `unstoppable`、
+`scheduleOn` 取消点、禁用异常的构建、文档收尾。设计确认当天对照库的代码评审过一轮并
+修订；哪些论断验证过、哪些没有，以及实现时与本文不同的几处，见第 12 节。
 
 ## 0. 问题
 
@@ -400,17 +399,21 @@ promise 没有对应入口时（用户自定义的返回类型）：
 ## 5. 禁用异常的构建
 
 - `config.hpp` 新增 `CO2_HAS_EXCEPTIONS`，由编译器宏自动得出（GCC / Clang 的
-  `__cpp_exceptions`，MSVC 的 `_CPPUNWIND`）。不提供手动覆盖：关着异常却让库保留
-  catch 没有意义，开着异常却让库去掉 catch 则是资源泄漏。
+  `__cpp_exceptions` 与 `__EXCEPTIONS`，MSVC 的 `_CPPUNWIND`）。不提供手动覆盖：关着
+  异常却让库保留 catch 没有意义，开着异常却让库去掉 catch 则是资源泄漏。使用者自己
+  定义这个宏是编译错误。
 - 为 0 时消失的东西：
-  - 内部回滚用的 `try` / `catch`（创建帧、超对齐分配、线程池构造）。没有人能抛，
-    这些路径本来就走不到。
   - 协程体变换里的 catch。promise 不再需要 `unhandled_exception()`，与原生编译器在
     `-fno-exceptions` 下的要求一致。
-  - `Error` 里装异常的那一半、`Error::exception()`、`Continuation::setException`。
+  - `Error` 里装异常的那一半、`Error::exception()`、`Continuation::setException`、
+    `GeneratorPromise` 里存异常的成员。
+  - 把异常接成错误的三处 catch：`asResult` 包住 `await_resume()` 的、`Task` 取值时
+    包住移动构造的、`Continuation::operator()` 包住结果构造的。
   - `Error::raise()` 与根上"抛出"的那条路，换成契约违规。
   - 靠捕获 `bad_alloc` 触发 `get_return_object_on_allocation_failure` 的那条路；
     promise 的 `operator new` 返回空的那条路保留。
+- 内部的回滚（创建帧的三步、超对齐块的构造、线程池启动线程）不随这个宏变化：它们
+  改成了局部的守卫对象，不用 `try` / `catch`，两种构建下是同一份代码。
 - 第 2～4 节的值通道在两种构建下是同一份代码，不随这个宏变化。
 - 分配失败时全局 `operator new` 的行为由运行时决定（通常是终止），与任何禁用异常的
   程序相同。
@@ -511,8 +514,10 @@ C++ 工作草案的原文（第 12 节）。
   适应返回 optional 的根。
 - 与原生协程的差分测试只在开异常时构建。原生协程不认识 `await_completion()`，新通道
   没有差分覆盖，只靠上面的测试。
-- 新增 CMake 选项，给测试加 `-fno-exceptions -fno-rtti`；CI 增加 GCC 与 Clang 各一个
-  这样的作业。
+- 新增 CMake 选项 `CO2_TEST_NO_EXCEPTIONS`（以及同名用途的 Preset `no-exceptions`），
+  给测试加 `-fno-exceptions -fno-rtti`；CI 增加这样的作业，GCC 与 Clang 各以 Debug、
+  Release 跑一遍。只在这种构建下成立的三条契约违规（错误到达根、到达没有
+  `return_error()` 的 promise、经过只转发三个成员的包装 awaiter）各有一个测试。
 - 第 4 节列出的每一条新增契约违规各有一个测试。
 - 基准新增两行：错误、已停止沿 Task 链每层的成本。错误那一行先于其他改动加入，用
   今天的异常路径跑出对照值——第 0 节与 3.5 的数字来自一次性的测量，不在仓库里。
@@ -586,8 +591,25 @@ awaiter 上再恢复等待者。
 - `Result<T>` 不能默认构造，作帧局部时写成 `co2::optional<co2::Result<T>>`；
   `asResult(x)` 与 `unstoppable(x)` 要直接写在等待它们的地方。
 
+**第 9 步实现时与本文不同的几处**（已同步到正文）
+
+- 内部回滚没有做成"为 0 时消失的 `try` / `catch`"，而是先改成守卫对象，于是不需要
+  条件编译（第 5 节）。改之前先补了测试：帧的三步构造各自失败时的回滚、超对齐块构造
+  失败时归还内存，此前都没有覆盖。
+- 少掉的东西比原先列的多两项：`GeneratorPromise` 里存异常的成员，以及把异常接成错误的
+  三处 catch。
+- 使用者自己定义 `CO2_HAS_EXCEPTIONS` 是编译错误，有一个必须编译失败的测试守着。
+- CI 的作业是四个而不是两个：两个编译器各加了 Release。
+
+**禁用异常的构建实测过的**
+
+GCC 13 与 Clang 18，`-fno-exceptions -fno-rtti`：C++14 的 Debug 与 Release、GCC 的
+C++20、Clang 的 C++23、libc++、ASan + UBSan、TSan，共九个配置，各 32 个测试全部通过。
+同一份测试源码在开着异常的十三个配置下各 31 个测试也全部通过。
+
 **尚未验证的**
 
-- 禁用异常的构建（第 5 节）没有任何实测。
 - MSVC：`Error` 的大小、`#pragma detect_mismatch`、`_CPPUNWIND` 的判断都无法在本机
-  验证。
+  验证；MSVC 关掉异常的构建在测试里没有配置（`CO2_TEST_NO_EXCEPTIONS` 只为 GCC 与
+  Clang 准备）。
+- 基准程序没有在禁用异常的构建下编译过：它的"失败"一项靠 `throw` 产生错误。
````

- [ ] **Step 4: 全配置验证**

设计稿第 12 节里写了"九个配置各 32 个测试、十三个配置各 31 个测试全部通过"，提交之前在仓库的代码上重新跑一遍。把下面的脚本存成 `/tmp/co2-build/matrix.sh`：

```bash
#!/bin/bash
# matrix.sh <配置>...   在当前目录的源码上逐个配置、构建、跑全部测试，每个配置一行结论。
# 名字以 -noexc 结尾的是禁用异常的配置。
for cfg in "$@"; do
  type=Debug; extra=""; wrap=""; opt=""
  base=${cfg%-noexc}
  [ "$base" != "$cfg" ] && opt="-DCO2_TEST_NO_EXCEPTIONS=ON"
  case $base in
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
    *) echo "$cfg: unknown configuration"; continue;;
  esac
  b=/tmp/co2-build/$cfg
  cmake -S . -B $b -DCMAKE_BUILD_TYPE=$type -DCMAKE_CXX_COMPILER=$cxx \
        -DCO2_TEST_CXX_STANDARD=$std -DCMAKE_CXX_FLAGS="$extra" $opt > $b.cfg.log 2>&1 ||
      { echo "$cfg: CONFIGURE FAILED"; continue; }
  cmake --build $b -j 12 > $b.build.log 2>&1 || { echo "$cfg: BUILD FAILED"; continue; }
  (cd $b && ASAN_OPTIONS=detect_leaks=0 /tmp/co2-build/runlimit 900 $wrap \
       ctest --timeout 300 -j 8 > $b.test.log 2>&1)
  echo "$cfg: $(grep -E 'tests passed|tests failed' $b.test.log)"
done
```

在仓库根目录运行：

```bash
bash /tmp/co2-build/matrix.sh gcc14 clang14 gcc17 gcc20 gcc23 clang20 clang23 libcxx \
     gccrel clangrel asan gccasan tsan
bash /tmp/co2-build/matrix.sh gcc14-noexc clang14-noexc gcc20-noexc clang23-noexc \
     gccrel-noexc clangrel-noexc libcxx-noexc asan-noexc tsan-noexc
```

Expected: 前 13 行都是 `100% tests passed out of 31`，后 9 行都是 `100% tests passed out of 32`。

并发相关的测试再重复跑，两种构建都是：

```bash
for cfg in gccrel clangrel gccrel-noexc clangrel-noexc; do
  (cd /tmp/co2-build/$cfg && /tmp/co2-build/runlimit 1500 ctest --timeout 300 -j 4 \
       -R "thread-pool|when-all|spawn|callback|edge-case|scheduler|completion" \
       --repeat until-fail:40 | grep 'tests passed')
done
for cfg in tsan tsan-noexc; do
  (cd /tmp/co2-build/$cfg && /tmp/co2-build/runlimit 1500 setarch $(uname -m) -R \
       ctest --timeout 300 -j 4 \
       -R "thread-pool|when-all|spawn|callback|edge-case|scheduler|completion" \
       --repeat until-fail:5 | grep 'tests passed')
done
```

Expected: 六行都是 `100% tests passed out of 9`。

任何一个配置不通过，都先停下来查原因，不要提交；数字与第 12 节写的不一样，就改第 12 节。

- [ ] **Step 5: 提交**

```bash
git add README.md docs/architecture.md docs/design/completion-channels.md
git commit -F - <<'EOF'
文档：禁用异常的构建、升级说明；设计稿标为全部实现

README 新增"禁用异常的构建"一节（怎么用、与开着异常时的四点不同、所有翻译单元的
异常开关必须一致）和"升级说明"（三种完成方式带来的九处接口与行为变化）；"构建与
测试"写明 CO2_TEST_NO_EXCEPTIONS 与新的 Preset。architecture.md 新增同名小节，列出
随 CO2_HAS_EXCEPTIONS 消失的东西，并改掉"运行时条件以异常报告"这句已经过时的话。

设计稿：状态改为十步全部实现；第 5 节按实现改写（回滚用守卫对象而不是条件编译，
少掉的东西多了两项）；第 12 节记下与原文不同的几处和实测过的配置。

验证：开着异常的 13 个配置各 31 个测试、禁用异常的 9 个配置各 32 个测试全部通过
（GCC 13、Clang 18；Debug / Release、C++14～23、libc++、ASan + UBSan、TSan）；
并发相关的测试在 Release 下重复 40 遍、TSan 下重复 5 遍，两种构建都没有失败。
MSVC 没有验证。
EOF
```
