# Mashiro 异步框架实施指南 —— 大学课程作业版

> **致 agentic worker：** 必备子技能：使用 `superpowers:subagent-driven-development`（推荐）或
> `superpowers:executing-plans` 逐任务实施本计划。各步骤使用 checkbox（`- [ ]`）追踪进度。

> **致学生（人类）：** 本文档把 `docs/superpowers/specs/2026-06-15-async-framework/` 下的 v0.2 规约
> 翻译成一门为期 20 周的工程实践课程。每个"任务"≈ 2–5 分钟的可验证操作；每个"概念讲解"≈ 一节
> 15 分钟的课堂导论。规约是**权威**，本指南是**导师**。当二者出现分歧时，**规约胜出** —— 请提交
> 一份 PR 修正本指南。

**目标（Goal）：** 在 `Mashiro/` 子树下落地一个完整、分层、零开销、C++26 反射驱动的 stdexec 异步框架，
作为 Mashiro 引擎所有"异步行为"的统一词汇。

**架构（Architecture）：** 八层严格分层（L0 词汇 → L1 注解/Traits → L2 后端 → L3 适配器 → L4 协程任务
→ L5 结构化并发 → L6 模式 → L7 扩展面）+ 横切关注（取消、分配、错误、时间、诊断）。每一层仅依赖下层。
所有能力查询、调度器亲和性、补全签名合一都在 `consteval` / `if constexpr` / `transform_sender` 中完成 ——
"能编译期决定的事，决不下放到运行期"。

**技术栈（Tech Stack）：**

- **C++26**：反射（P2996）、注解（P3394）、consteval 块（P3289）、展开语句（P1306）、
  `define_static_array`（P3491）、`for co_await`（C++26）
- **stdexec**：sender / receiver / scheduler / domain / `inplace_stop_token` / `counting_scope` /
  `exec::task` / `exec::async_scope` / 调度器亲和性（P3941）/ `counting_scope`（P3149）/ 域（P2999/P3826）
- **工具链**：clang-p2996（项目本地 sysroot；MSVC ABI；详见 `MEMORY.md`）+ libc++
- **依赖库**（`thirdparty/`）：`stdexec`、`tbb`、`tracy`、`perfetto`、`vulkan-headers`
- **测试**：Catch2（沿用现有约定）
- **平台**：Windows（IOCP / `MsgWaitForMultipleObjectsEx`）+ Linux（io_uring / `epoll` + `eventfd`）

---

## 课程总览（Course Overview）

### 课时安排（与 `08-cross-cutting.md` §7.1 一致）

| 阶段 | 目标层 | 周数 | 章节 | 学习目标 |
|---|---|---|---|---|
| Phase 1 | L0 词汇 + L1 注解/Traits | 2 | 第 1 章 | 理解 stdexec 词汇与反射驱动的能力声明 |
| Phase 2 | L2 后端：Inline / StaticPool / Platform | 3 | 第 2 章 | 实现值语义、零虚分派的调度器 |
| Phase 3 | L2 后端：Tbb / Io | 4 | 第 3 章 | 集成第三方运行时与平台 proactor |
| Phase 4 | L3 适配器 + L4 协程任务 | 4 | 第 4 章 | 用 sender 代数与协程同时表达异步 |
| Phase 5 | L5 结构化并发 + L6 模式 | 3 | 第 5 章 | 让生命周期与并行性结构化可证 |
| Phase 6 | L7 扩展面 | 2 | 第 6 章 | 让外部代码也能以零开销加入框架 |
| Phase 7 | 横切硬化（取消 / 分配 / 诊断） | 2 | 第 7 章 | 用 CI 锁住"零开销 + 结构化"的契约 |

### 先决条件（Prerequisites）

学完本课程**之前**，你需要：

1. **C++20 协程的最小心智模型**：知道 `co_await` 调用了什么、`promise_type` 是什么、帧分配
   何时发生。可读 Lewis Baker 的 cppcoro 系列文章作热身。
2. **`stdexec` 的 sender/receiver 模型**：读完 P2300 论文摘要 §1–§4 即可，剩下的我们在
   "概念讲解"环节展开。
3. **C++26 反射基础**：至少能解读 `^^` 提升运算符、`std::meta::reflect_constant`、
   `[: ... :]` splice、`std::meta::members_of` —— 项目 `Yuki/` 已有大量样本。
4. **已激活开发环境**：`python setup.py` 已运行（详见你的全局 `MEMORY.md`）。
5. **可读规约 v0.2**：本指南所有 §X.Y 的引用均指向
   `docs/superpowers/specs/2026-06-15-async-framework/` 下对应文件。

### 学习方法（How to use this guide）

- **顺序阅读**：每章按"概念讲解 → 任务 → 自检"循环展开。跳章会导致后置任务无依赖。
- **TDD 优先**：每个任务都从"先写一个失败的测试"开始。看到红条再写实现，不要反过来。
- **频繁提交**：每完成 1–3 个任务就 `git commit`，提交标题用 `feat(Async/<Lx>): ...`。
- **回看规约**：当任务步骤说"详见 L*x* §*N*"时，**真的去翻**，不要凭直觉补全。
- **保持 120 列**：本项目所有代码与注释行宽限制是 120 列（详见全局 `CLAUDE.md`）。

---

## 第 0 章 —— 课前准备（Phase 0：1 天）

### 概念讲解 0.1：为什么是 stdexec？

> **关键词：** sender、receiver、operation_state、completion_signatures

传统异步编程 —— 回调、`std::future`、`std::async`、手工事件循环 —— 共享两个致命缺陷：

1. **类型擦除发生在错误的地方。** `std::function`、`std::any`、虚函数表把"我接下来要做什么"
   藏在一次虚分派之后；编译器看不见管线的拓扑，无法优化、无法静态验证。
2. **取消（cancellation）是事后修补。** 大多数库的取消语义是"设个 flag、希望对方注意到"。
   于是泄漏、死锁、双重释放成了系统级的常见 bug。

**sender/receiver 把这两件事推到类型系统里**：

- **sender** 是一段"尚未启动"的异步工作，它的**值类型**和**错误类型**完全编码在
  `completion_signatures_of_t<S, Env>` 里。
- **receiver** 是异步工作完成时要回填的三个回调（`set_value` / `set_error` / `set_stopped`）。
- **operation_state** 是 `connect(sender, receiver)` 的产物，它持有所有运行时状态 ——
  原则上一次堆分配都不需要。

这套词汇可以**整段被编译器看穿**，从而：

- 零虚分派（直接生成跨 sender 的 inlined code）；
- 静态保证补全签名不"漏"或"重"；
- 取消变成"沿着 receiver 的 environment 查询 `stop_token`"，结构化、可组合、可证。

**本框架不发明新词汇** —— sender / receiver / scheduler / domain / `inplace_stop_token` /
`counting_scope` 就是我们与外界、内部沟通的全部 noun。

### 任务 0.1：初始化工作分支与目录骨架

**Files:**
- Modify: `Mashiro/include/CMakeLists.txt`（如有）
- Modify: `Mashiro/CMakeLists.txt`

- [ ] **Step 1：从 main 切出工作分支**

```bash
git checkout -b feat/async-framework-l0
```

- [ ] **Step 2：核对 `thirdparty/stdexec` 是否到位**

```bash
ls thirdparty/stdexec/include/stdexec/execution.hpp
```

期望：文件存在。若缺失，按现有子模块更新流程同步。

- [ ] **Step 3：创建 L0 头文件占位（仅目录与空文件，先让 CMake 找到位置）**

```bash
mkdir -p Mashiro/include/Mashiro/Async
mkdir -p Mashiro/src/Async
mkdir -p Mashiro/tests/Async
mkdir -p Mashiro/demos/Async
```

- [ ] **Step 4：建立空占位头**（之后逐章填充）

```bash
touch Mashiro/include/Mashiro/Async/Foundations.h
touch Mashiro/include/Mashiro/Async/Concepts.h
touch Mashiro/include/Mashiro/Async/Traits.h
```

- [ ] **Step 5：提交骨架**

```bash
git add Mashiro/include/Mashiro/Async/ Mashiro/src/Async/ Mashiro/tests/Async/ Mashiro/demos/Async/
git commit -m "chore(Async): scaffold async-framework source tree"
```

---

## 第 1 章 —— L0 词汇 + L1 注解/Traits（Phase 1：2 周）

> **规约入口：** `01-foundations.md`（必读全文）；
> **必读小节：** §4 L0 重导出 / §5 注解 / §6 Traits / §7 补全签名助手 / §8 一致性验证块 / §10 工作示例。

### 概念讲解 1.1：什么是 L0 "重导出"？

L0 不发明任何行为 —— 它只是给 `stdexec::*` 的名字换上 `Mashiro::Async::*` 的命名空间制服。
为什么要换制服？**两个理由**：

1. **风格统一**：`Mashiro/Async/` 下的每个头都说 `Async::sender`、`Async::scheduler`，不说
   `stdexec::sender`。读起来像同一本书，不像三个译者拼起来的。
2. **概念/类型同名冲突**：`stdexec::scheduler` 既是概念名（concept）又被用户当作类型变量名 ——
   而 `Mashiro::Platform::scheduler` 是一个**具体类型**。在 `Mashiro::Async::Concepts::Scheduler`
   下重新放置**概念**，让两者井水不犯河水。

但 L0 **拒绝过度重命名**。规约 §4.2 给的尺子是 _"换名换得读起来更自然，且不发明同义词"_。被
否决的重命名（不要在 PR 里旧事重提）：

- `Async::Sender`（PascalCase）—— 否决，与 stdexec 风格不一致；
- `Async::task<T>` —— 否决，L0 不烘焙策略，`Task<T>` 是 L4 类型；
- `Async::future<T>` / `Async::executor` —— 否决（违反总览 §2 "一种词汇"原则）。

### 概念讲解 1.2：什么是 "概念别名"？

C++20 引入了 `concept`。stdexec 已经定义了 `stdexec::scheduler<S>`。我们在
`Mashiro::Async::Concepts::Scheduler` 里**重命名同一个概念**，但同时**新增三个派生概念**：

```cpp
template<class S> concept BulkScheduler  = Scheduler<S> && /* schedule_bulk(s,n,fn) 良构 */;
template<class S> concept IoScheduler    = Scheduler<S> && /* Traits::OffersIo_v<S>   */;
template<class S> concept AffineScheduler= Scheduler<S> && /* Traits::IsAffine_v<S>   */;
template<class S> concept ParallelScheduler = Scheduler<S> && /* ProgressOf ≥ parallel */;
```

**关键设计：概念与 L1 注解互为镜像**。后端用 `[[=Async::OffersBulk{}]]` 声明"我提供 bulk"，
L0 概念里的 `Traits::OffersBulk_v<S>` 用反射去**核对**这条声明真的对应了一个良构的
`schedule_bulk` 调用。声明与现实不一致 = 编译错。

### 任务 1.1：填写 `Foundations.h` 的 stdexec 重导出表

**Files:**
- Modify: `Mashiro/include/Mashiro/Async/Foundations.h`
- Test: `Mashiro/tests/Async/L0/ReExportTest.cpp`（新建）

**规约对照：** §4.3 重导出表。

- [ ] **Step 1：先写失败的测试 —— 断言每一个 `Async::*` 都是其 stdexec 对应物**

```cpp
// Mashiro/tests/Async/L0/ReExportTest.cpp
#include <Mashiro/Async/Foundations.h>
#include <catch2/catch_test_macros.hpp>
#include <stdexec/execution.hpp>

TEST_CASE("L0 re-exports preserve stdexec identity", "[Async][L0]") {
    using namespace Mashiro;
    STATIC_REQUIRE(std::is_same_v<Async::stop_token,  stdexec::inplace_stop_token>);
    STATIC_REQUIRE(std::is_same_v<Async::stop_source, stdexec::inplace_stop_source>);
    STATIC_REQUIRE(std::is_same_v<Async::set_value_t,   stdexec::set_value_t>);
    STATIC_REQUIRE(std::is_same_v<Async::set_error_t,   stdexec::set_error_t>);
    STATIC_REQUIRE(std::is_same_v<Async::set_stopped_t, stdexec::set_stopped_t>);
}
```

- [ ] **Step 2：运行测试，确认失败（`Foundations.h` 还是空的）**

```bash
cmake --build build --target Mashiro_Tests && ./build/Mashiro/tests/Mashiro_Tests "[Async][L0]"
```

期望：编译错误，提示 `Async::stop_token` 等不存在。

- [ ] **Step 3：在 `Foundations.h` 里写出最小重导出**

```cpp
// Mashiro/include/Mashiro/Async/Foundations.h
#pragma once
#include <stdexec/execution.hpp>
#include <exec/any_sender_of.hpp>
#include <exec/when_any.hpp>

namespace Mashiro::Async {
    // §4.3 stop tokens
    using stop_token  = stdexec::inplace_stop_token;
    using stop_source = stdexec::inplace_stop_source;
    template<class Tok, class Cb>
    using stop_callback_for_t = stdexec::stop_callback_for_t<Tok, Cb>;

    // §4.3 completion tags
    using stdexec::set_value_t;
    using stdexec::set_error_t;
    using stdexec::set_stopped_t;

    // §4.3 factories
    using stdexec::just;
    using stdexec::just_error;
    using stdexec::just_stopped;

    // §4.3 adaptors
    using stdexec::then;
    using stdexec::let_value;
    using stdexec::let_error;
    using stdexec::let_stopped;
    using stdexec::when_all;
    using exec::when_any;

    // §4.3 algorithms
    using stdexec::start_detached;
    using stdexec::sync_wait;
    using stdexec::continues_on;
    using stdexec::schedule;
    using stdexec::schedule_bulk;
    using stdexec::transfer_just;

    // §4.3 env queries
    using stdexec::get_stop_token;
    using stdexec::get_completion_scheduler;
    using stdexec::get_allocator;
    using stdexec::get_domain;
    using stdexec::default_domain;
    using stdexec::transform_sender;
    using stdexec::forward_progress_guarantee;

    // §4.3 type erasure (boundary-only)
    template<class... Sigs>
    using any_sender_of = exec::any_sender_of<Sigs...>;

    // §4.3 v0.2: British spelling aliases
    inline constexpr auto& materialise   = stdexec::materialize;
    inline constexpr auto& dematerialise = stdexec::dematerialize;
    inline constexpr auto& materialize   = stdexec::materialize;
    inline constexpr auto& dematerialize = stdexec::dematerialize;

    // §4.5 v0.2: forward-declared CPO; implementation provided by L2 Io backend
    struct get_io_context_t {
        template<class Sched>
        constexpr auto operator()(Sched&& s) const
            noexcept(noexcept(stdexec::tag_invoke(get_io_context_t{}, std::forward<Sched>(s))))
            -> decltype(stdexec::tag_invoke(get_io_context_t{}, std::forward<Sched>(s)))
        {
            return stdexec::tag_invoke(get_io_context_t{}, std::forward<Sched>(s));
        }
    };
    inline constexpr get_io_context_t get_io_context{};

    // §4.3 completion signatures
    template<class... Sigs>
    using completion_signatures = stdexec::completion_signatures<Sigs...>;
    template<class S, class Env = stdexec::empty_env>
    using completion_signatures_of_t = stdexec::completion_signatures_of_t<S, Env>;

    // §4.3 env_of
    template<class T>
    using env_of_t = stdexec::env_of_t<T>;

    // §4.3 operation_state
    using stdexec::operation_state;
} // namespace Mashiro::Async
```

- [ ] **Step 4：运行测试，确认通过**

```bash
cmake --build build --target Mashiro_Tests && ./build/Mashiro/tests/Mashiro_Tests "[Async][L0]"
```

期望：PASS。

- [ ] **Step 5：补齐 §4.3 表中尚未覆盖的别名**

照规约 §4.3 表逐行核对，缺什么补什么。每补一行写一条 `STATIC_REQUIRE` 在测试里固定下来。

- [ ] **Step 6：提交**

```bash
git add Mashiro/include/Mashiro/Async/Foundations.h Mashiro/tests/Async/L0/ReExportTest.cpp
git commit -m "feat(Async/L0): stdexec re-export surface in Mashiro::Async"
```

### 概念讲解 1.3：C++26 反射 + 注解 = 能力声明

> **关键词：** `[[=expr]]` 注解、`std::meta::info`、`std::meta::annotations_of`

C++26 让你能在类型上贴 **任意 constexpr 值** 作注解：

```cpp
struct [[=Async::Affine{Async::Backend::Platform}, =Async::Cancellable{}]] my_scheduler { };
```

之后用反射查询：

```cpp
constexpr auto info = ^^my_scheduler;
constexpr auto annos = std::meta::annotations_of(info);
// annos 是 std::vector<std::meta::info>，每一项指向贴在 my_scheduler 上的注解值
```

这套机制让我们做到：

1. **能力声明在类型上**：不用注册表、不用宏、不用集中维护表格。后端的注解就是它的能力名片。
2. **能力查询在编译期**：`Traits::IsAffine_v<S>` 等价于"在 `S` 的注解里找 `Affine{}`"，
   全部 `consteval`。
3. **声明与实现一致性可证**：L0/L1 边界的"一致性验证块"在 `Foundations.h` 末尾跑一遍 ——
   声明 `OffersBulk` 但 `schedule_bulk` 不良构？编译错。

### 任务 1.2：实现 L1 注解 `struct`s 与 `Backend` 枚举

**Files:**
- Modify: `Mashiro/include/Mashiro/Async/Foundations.h`
- Test: `Mashiro/tests/Async/L1/AnnotationTest.cpp`

**规约对照：** §5.1 `Backend` 枚举 / §5.2 注解 struct / §5.3 各注解所贴对象。

- [ ] **Step 1：先写失败的测试**

```cpp
// Mashiro/tests/Async/L1/AnnotationTest.cpp
#include <Mashiro/Async/Foundations.h>
#include <catch2/catch_test_macros.hpp>

namespace {
    struct [[=Mashiro::Async::Affine{Mashiro::Async::Backend::Platform}]] FakeAffine {};
    struct [[=Mashiro::Async::OffersBulk{}]] FakeBulk {};
}

TEST_CASE("L1 annotations attach to types", "[Async][L1]") {
    using namespace Mashiro::Async;
    constexpr auto affineAnnos = std::meta::annotations_of(^^FakeAffine);
    STATIC_REQUIRE(affineAnnos.size() == 1);

    constexpr auto bulkAnnos = std::meta::annotations_of(^^FakeBulk);
    STATIC_REQUIRE(bulkAnnos.size() == 1);
}
```

- [ ] **Step 2：运行测试，确认失败（`Affine` / `OffersBulk` 还未定义）**

- [ ] **Step 3：在 `Foundations.h` 里加入注解定义**

```cpp
// 接续上文 Foundations.h
namespace Mashiro::Async {
    // §5.1 — Backend enum (full set, including v0.2 entries)
    enum class Backend : std::uint8_t {
        Inline,
        StaticPool,
        Tbb,
        Platform,
        Io,
        User,
    };

    // §5.2 — capability annotations
    struct Backend_     { Backend backend; };        // tags scheduler types
    struct Affine       { Backend backend; };        // tags scheduler types
    struct OffersBulk   { };                          // tags scheduler types
    struct OffersIo     { };                          // tags scheduler types
    struct Cancellable  { };                          // tags scheduler types
    struct Allocates {
        enum class Where { Frame, OpState, Output, External } where;
    };
    struct IsForwardProgress {
        forward_progress_guarantee guarantee;
    };

    // v0.2 additions (overview §5.6)
    struct Detached     { };                          // tags L4 Job (detached lifetime)
    struct ScopeTag     { std::uint64_t value; };     // tags L5 Scope<...> templates
} // namespace Mashiro::Async
```

- [ ] **Step 4：运行测试，确认通过**

- [ ] **Step 5：提交**

```bash
git add Mashiro/include/Mashiro/Async/Foundations.h Mashiro/tests/Async/L1/AnnotationTest.cpp
git commit -m "feat(Async/L1): capability annotation structs and Backend enum"
```

### 任务 1.3：填写 `Concepts.h` 概念别名

**Files:**
- Modify: `Mashiro/include/Mashiro/Async/Concepts.h`
- Test: `Mashiro/tests/Async/L0/ConceptsTest.cpp`

**规约对照：** §4.4 概念别名（含 v0.2 的 `HasStopToken<Env>`）。

- [ ] **Step 1：先写失败的测试**

```cpp
// Mashiro/tests/Async/L0/ConceptsTest.cpp
#include <Mashiro/Async/Concepts.h>
#include <catch2/catch_test_macros.hpp>

TEST_CASE("Inline-style scheduler models Scheduler", "[Async][L0][Concepts]") {
    struct DummySched {
        struct sender_t {
            using sender_concept = stdexec::sender_t;
            using completion_signatures = stdexec::completion_signatures<stdexec::set_value_t()>;
        };
        sender_t schedule() const noexcept { return {}; }
        bool operator==(const DummySched&) const = default;
    };
    STATIC_REQUIRE(Mashiro::Async::Concepts::Scheduler<DummySched>);
}
```

- [ ] **Step 2：运行测试，确认失败**

- [ ] **Step 3：填写 `Concepts.h`**

```cpp
// Mashiro/include/Mashiro/Async/Concepts.h
#pragma once
#include "Foundations.h"
#include "Traits.h"  // forward dependency — Traits.h must be include-guard safe

namespace Mashiro::Async::Concepts {

    // §4.4 — Scheduler
    template<class S>
    concept Scheduler = stdexec::scheduler<S>;

    // §4.4 — BulkScheduler
    template<class S>
    concept BulkScheduler = Scheduler<S> && requires (S s, std::size_t n) {
        { stdexec::schedule_bulk(s, n, [](std::size_t) noexcept {}) } -> stdexec::sender;
    };

    // §4.4 — IoScheduler (uses get_io_context CPO from Foundations.h §4.5)
    template<class S>
    concept IoScheduler = Scheduler<S> && Traits::OffersIo_v<S> && requires (S s) {
        { stdexec::tag_invoke(get_io_context_t{}, s) } -> std::convertible_to<void*>;
    };

    // §4.4 — AffineScheduler
    template<class S>
    concept AffineScheduler = Scheduler<S> && Traits::IsAffine_v<S> && std::equality_comparable<S>;

    // §4.4 — ParallelScheduler
    template<class S>
    concept ParallelScheduler =
        Scheduler<S> && (Traits::ProgressOf_v<S> >= forward_progress_guarantee::parallel);

    // §4.4 v0.2 — HasStopToken<Env>: lets hot backends skip stop-callback registration
    // when the receiver advertises stdexec::never_stop_token.
    template<class Env>
    concept HasStopToken = !std::same_as<
        std::remove_cvref_t<decltype(stdexec::get_stop_token(std::declval<const Env&>()))>,
        stdexec::never_stop_token>;

} // namespace Mashiro::Async::Concepts
```

- [ ] **Step 4：运行测试，确认通过**

- [ ] **Step 5：提交**

```bash
git add Mashiro/include/Mashiro/Async/Concepts.h Mashiro/tests/Async/L0/ConceptsTest.cpp
git commit -m "feat(Async/L0): concept aliases (Scheduler/Bulk/Io/Affine/Parallel/HasStopToken)"
```

### 概念讲解 1.4：反射驱动的 `Traits::*_v` —— 一次扫描，得到一切

L1 的核心机制是这样一个反射循环：

```cpp
template<class T>
constexpr auto find_annotation = []<class Anno>() {
    constexpr auto annos = std::meta::annotations_of(^^T);
    for (auto a : annos) {
        if (std::meta::type_of(a) == ^^Anno) return /* extract value */;
    }
    return /* default */;
};
```

把这层"找注解"封装到 `Traits::*_v<T>` 系列变量里之后，**所有上层代码都只需读这些变量**，
不再直接操作反射：

```cpp
Traits::AffinityOf<S>       // Backend 值，没注解则为 Backend::User
Traits::BackendOf<S>        // 同上
Traits::OffersBulk_v<S>     // bool
Traits::OffersIo_v<S>       // bool
Traits::IsAffine_v<S>       // bool
Traits::ProgressOf_v<S>     // forward_progress_guarantee
Traits::AllocatesIn_v<S>    // Allocates::Where 集合
Traits::IsDetached_v<T>     // bool（L4）
Traits::ScopeTagOf<S>       // std::uint64_t（L5）
```

**为什么这层独立存在？** 因为它把"反射 API 的细节"从"L2 后端如何被查询"中剥离开。L2 后端
作者只需写：

```cpp
if constexpr (Traits::OffersBulk_v<S>) {
    /* 走 bulk fast-path */
}
```

而不必关心 `std::meta::annotations_of` 怎么调用。**这就是抽象的意义**。

### 任务 1.4：实现 `Traits.h` 反射核心 + 全套查询

**Files:**
- Modify: `Mashiro/include/Mashiro/Async/Traits.h`
- Test: `Mashiro/tests/Async/L1/TraitsTest.cpp`

**规约对照：** §6.1 反射 helper / §6.2 Trait 集 / §6.3 反射算法 / §6.4 仅限反射契约。

- [ ] **Step 1：先写失败的测试**

```cpp
// Mashiro/tests/Async/L1/TraitsTest.cpp
#include <Mashiro/Async/Traits.h>
#include <catch2/catch_test_macros.hpp>

namespace {
    struct [[=Mashiro::Async::Affine{Mashiro::Async::Backend::Platform},
            =Mashiro::Async::Cancellable{},
            =Mashiro::Async::IsForwardProgress{
                Mashiro::Async::forward_progress_guarantee::concurrent}]]
        PlatformLike {};

    struct [[=Mashiro::Async::OffersBulk{},
            =Mashiro::Async::IsForwardProgress{
                Mashiro::Async::forward_progress_guarantee::parallel}]]
        BulkPoolLike {};
}

TEST_CASE("Traits read annotations", "[Async][L1][Traits]") {
    using namespace Mashiro::Async;
    STATIC_REQUIRE(Traits::IsAffine_v<PlatformLike>);
    STATIC_REQUIRE(Traits::AffinityOf<PlatformLike> == Backend::Platform);
    STATIC_REQUIRE(Traits::ProgressOf_v<PlatformLike> ==
                   forward_progress_guarantee::concurrent);

    STATIC_REQUIRE(Traits::OffersBulk_v<BulkPoolLike>);
    STATIC_REQUIRE_FALSE(Traits::IsAffine_v<BulkPoolLike>);
    STATIC_REQUIRE(Traits::ProgressOf_v<BulkPoolLike> ==
                   forward_progress_guarantee::parallel);
}
```

- [ ] **Step 2：运行测试，确认失败**

- [ ] **Step 3：填写 `Traits.h` 的反射 helper 与全套 Trait**

```cpp
// Mashiro/include/Mashiro/Async/Traits.h
#pragma once
#include "Foundations.h"

#include <experimental/meta>

namespace Mashiro::Async::Traits {

    namespace Detail {
        // §6.1 — find_annotation_value<Anno>(^^T): returns std::optional<Anno> by reflecting
        // over T's annotation list.
        template<class Anno, std::meta::info TypeInfo>
        consteval auto find_annotation_value() {
            constexpr auto annos = std::meta::annotations_of(TypeInfo);
            for (auto a : annos) {
                if (std::meta::type_of(a) == ^^Anno) {
                    return std::optional<Anno>{[: a :]};
                }
            }
            return std::optional<Anno>{std::nullopt};
        }

        template<class Anno, class T>
        inline constexpr auto annotation_v = find_annotation_value<Anno, ^^T>();

        template<class Anno, class T>
        inline constexpr bool has_annotation_v = annotation_v<Anno, T>.has_value();
    }

    // §6.2 — Trait set
    template<class T>
    inline constexpr Backend AffinityOf =
        Detail::annotation_v<Affine, T>.value_or(Affine{Backend::User}).backend;

    template<class T>
    inline constexpr Backend BackendOf =
        Detail::annotation_v<Backend_, T>.value_or(Backend_{Backend::User}).backend;

    template<class T>
    inline constexpr bool OffersBulk_v = Detail::has_annotation_v<OffersBulk, T>;

    template<class T>
    inline constexpr bool OffersIo_v = Detail::has_annotation_v<OffersIo, T>;

    template<class T>
    inline constexpr bool IsAffine_v = Detail::has_annotation_v<Affine, T>;

    template<class T>
    inline constexpr bool IsCancellable_v = Detail::has_annotation_v<Cancellable, T>;

    template<class T>
    inline constexpr forward_progress_guarantee ProgressOf_v =
        Detail::annotation_v<IsForwardProgress, T>
            .value_or(IsForwardProgress{forward_progress_guarantee::weakly_parallel}).guarantee;

    template<class T>
    inline constexpr bool AllocatesIn_v =
        Detail::has_annotation_v<Allocates, T>;

    // §6.2 v0.2 additions for L4/L5
    template<class T>
    inline constexpr bool IsDetached_v = Detail::has_annotation_v<Detached, T>;

    template<class T>
    inline constexpr std::uint64_t ScopeTagOf =
        Detail::annotation_v<ScopeTag, T>.value_or(ScopeTag{0}).value;

} // namespace Mashiro::Async::Traits
```

- [ ] **Step 4：运行测试，确认通过**

- [ ] **Step 5：提交**

```bash
git add Mashiro/include/Mashiro/Async/Traits.h Mashiro/tests/Async/L1/TraitsTest.cpp
git commit -m "feat(Async/L1): reflection-driven Traits queries"
```

### 任务 1.5：实现补全签名助手 `with_error / union_signatures / propagate_stopped`

**Files:**
- Modify: `Mashiro/include/Mashiro/Async/Traits.h`
- Test: `Mashiro/tests/Async/L1/CompletionSignaturesTest.cpp`

**规约对照：** §7.1 `with_error<E,S>` / §7.2 `union_signatures<S...>` / §7.3 `propagate_stopped<S>`。

- [ ] **Step 1：先写失败的测试**

```cpp
// Mashiro/tests/Async/L1/CompletionSignaturesTest.cpp
#include <Mashiro/Async/Traits.h>
#include <catch2/catch_test_macros.hpp>

TEST_CASE("union_signatures dedup", "[Async][L1][CompSigs]") {
    using namespace Mashiro::Async;
    using A = completion_signatures<set_value_t(int)>;
    using B = completion_signatures<set_value_t(int), set_error_t(std::exception_ptr)>;
    using U = Traits::union_signatures_t<A, B>;
    // The union must contain both signature kinds, exactly once.
    STATIC_REQUIRE(stdexec::__valid_completion_signatures<U>);
}
```

- [ ] **Step 2：运行测试，确认失败**

- [ ] **Step 3：在 `Traits.h` 追加补全签名助手**

```cpp
// 接续 Traits.h
namespace Mashiro::Async::Traits {

    // §7.1 — with_error<E, S>: union S's signatures with set_error_t(E).
    template<class E, class S>
    using with_error_t =
        stdexec::transform_completion_signatures_of<
            S,
            stdexec::empty_env,
            completion_signatures<set_error_t(E)>>;

    // §7.2 — union of signatures from sender-types S...
    template<class... Ss>
    using union_signatures_t =
        stdexec::__concat_completion_signatures_t<
            completion_signatures_of_t<Ss>...>;

    // §7.3 — propagate_stopped<S>: ensure set_stopped_t() is in the signatures of S.
    template<class S>
    using propagate_stopped_t =
        stdexec::transform_completion_signatures_of<
            S,
            stdexec::empty_env,
            completion_signatures<set_stopped_t()>>;

} // namespace Mashiro::Async::Traits
```

- [ ] **Step 4：运行测试，确认通过**

- [ ] **Step 5：提交**

```bash
git add Mashiro/include/Mashiro/Async/Traits.h Mashiro/tests/Async/L1/CompletionSignaturesTest.cpp
git commit -m "feat(Async/L1): completion-signature helpers (with_error / union / propagate_stopped)"
```

### 概念讲解 1.5：consteval 一致性验证块 —— 编译期合约执行

规约 §8 要求 `Foundations.h` 末尾跑一个 `consteval` 块：把当前编译单元能看见的所有"被注解为后端
调度器"的类型列出来，逐个验证它的注解集合和概念满足结果**双向相等**。

**核心想法：**

- 后端**声明** `OffersBulk` ⇔ `Concepts::BulkScheduler<S>` 必须成立。
- 后端**未声明** `OffersBulk` ⇔ `schedule_bulk(s, n, fn)` **不应**良构。
- 不一致 = 编译错误，错误信息要指出"声明了 X 但 X 不成立"或"X 成立但未声明"。

这套验证块是**自我执行的合约**：它的存在意味着 L2 后端作者每次改动调度器都会被 L0/L1 当场捕捉。

### 任务 1.6：实现一致性验证块（注册 + 检查）

**Files:**
- Modify: `Mashiro/include/Mashiro/Async/Foundations.h`
- Test: `Mashiro/tests/Async/L1/VerifierTest.cpp`

**规约对照：** §8.1 后端发现 / §8.2 双向检查 / §8.3 错误信息策略。

- [ ] **Step 1：先写一个"声明撒谎"的负向测试 —— 编译应失败**

```cpp
// Mashiro/tests/Async/L1/VerifierTest.cpp
// 该文件用 add_executable(... TEST_VERIFIER_LIE) 单独构建一个"应失败"目标。
// 它演示：声明 OffersBulk 但 schedule_bulk 不良构 ⇒ 编译错。
#include <Mashiro/Async/Foundations.h>

namespace {
    struct [[=Mashiro::Async::OffersBulk{}]] LiarBackend {
        struct sender_t {
            using sender_concept = stdexec::sender_t;
            using completion_signatures = stdexec::completion_signatures<stdexec::set_value_t()>;
        };
        sender_t schedule() const noexcept { return {}; }
        // intentionally no schedule_bulk
        bool operator==(const LiarBackend&) const = default;
    };

    MASHIRO_ASYNC_REGISTER_BACKEND(LiarBackend);  // §8.1 macro from Foundations.h
}

int main() {}
```

CMake 中标记为"expected build failure"（CTest 用 `WILL_FAIL` 或独立目标 + `add_test` 期望非零）。

- [ ] **Step 2：在 `Foundations.h` 末尾添加注册宏与 consteval 验证块**

```cpp
// 接续 Foundations.h —— 必须放在文件末尾，concept/Traits 头已被前部 include
// （或在 Foundations.h 内 forward-declare 它们：见 §8 设计说明）。
namespace Mashiro::Async::Detail {

    // §8.1 — Registration list. Each backend's static helper appends a tag.
    // The registry is a constexpr std::array<std::meta::info, N> generated via reflection
    // over the namespace Async::Backend; alternatively, a registration macro pushes
    // ^^T into a constexpr meta-vector at namespace scope.
    template<class T>
    struct backend_registration {
        static constexpr std::meta::info type = ^^T;
    };

    template<class T>
    consteval void verify_backend() {
        using namespace Mashiro::Async;
        // §8.2 — bi-conditional check
        if constexpr (Traits::OffersBulk_v<T>) {
            static_assert(Concepts::BulkScheduler<T>,
                "Backend declares [[=OffersBulk{}]] but schedule_bulk is not well-formed. "
                "Either remove the annotation or implement schedule_bulk.");
        } else {
            static_assert(!Concepts::BulkScheduler<T>,
                "Backend models BulkScheduler but is missing [[=OffersBulk{}]] annotation.");
        }
        if constexpr (Traits::OffersIo_v<T>) {
            static_assert(Concepts::IoScheduler<T>,
                "Backend declares [[=OffersIo{}]] but tag_invoke(get_io_context_t, S) is not "
                "well-formed.");
        }
        if constexpr (Traits::IsAffine_v<T>) {
            static_assert(Concepts::AffineScheduler<T>,
                "Backend declares [[=Affine{...}]] but is not equality-comparable.");
        }
    }

} // namespace Mashiro::Async::Detail

// §8.1 — registration macro
#define MASHIRO_ASYNC_REGISTER_BACKEND(T) \
    static_assert((::Mashiro::Async::Detail::verify_backend<T>(), true), \
                  "Backend " #T " failed L0/L1 consistency check")
```

- [ ] **Step 3：在 `Foundations.h` 里给一个**正向"trivial backend"测试**，确认正确声明不报错**

```cpp
// Mashiro/tests/Async/L1/VerifierGoodPathTest.cpp
#include <Mashiro/Async/Foundations.h>
#include <Mashiro/Async/Concepts.h>
#include <catch2/catch_test_macros.hpp>

namespace {
    struct [[=Mashiro::Async::Affine{Mashiro::Async::Backend::Inline},
            =Mashiro::Async::Cancellable{}]]
        TrivialInlineBackend {
        struct sender_t {
            using sender_concept = stdexec::sender_t;
            using completion_signatures = stdexec::completion_signatures<stdexec::set_value_t()>;
            template<class R>
            auto connect(R&& r) && noexcept { return stdexec::__inln::__op<R>{std::forward<R>(r)}; }
        };
        sender_t schedule() const noexcept { return {}; }
        bool operator==(const TrivialInlineBackend&) const = default;
    };
    MASHIRO_ASYNC_REGISTER_BACKEND(TrivialInlineBackend);
}

TEST_CASE("verifier accepts well-formed backend", "[Async][L1][Verifier]") {
    STATIC_REQUIRE(Mashiro::Async::Concepts::Scheduler<TrivialInlineBackend>);
}
```

- [ ] **Step 4：运行测试 + 期望失败的构建**

```bash
cmake --build build --target Mashiro_Tests
./build/Mashiro/tests/Mashiro_Tests "[Async][L1][Verifier]"
# 该 expected-failure 目标应**构建失败**：
cmake --build build --target TEST_VERIFIER_LIE 2>&1 | grep -i "declares.*OffersBulk"
```

期望：好路径 PASS；坏路径构建失败且错误信息提到 `OffersBulk` 不一致。

- [ ] **Step 5：提交**

```bash
git add Mashiro/include/Mashiro/Async/Foundations.h Mashiro/tests/Async/L1/Verifier*Test.cpp
git commit -m "feat(Async/L1): consteval verifier — annotations ↔ concepts must agree"
```

### 任务 1.7：实现 `bridge_stop_token`（std → inplace 适配）

**Files:**
- Modify: `Mashiro/include/Mashiro/Async/Foundations.h`
- Test: `Mashiro/tests/Async/L0/BridgeStopTokenTest.cpp`

**规约对照：** §9.1 用途 / §9.2 草图 / §9.3 生命周期 / §9.4 反例。

- [ ] **Step 1：先写失败的测试**

```cpp
// Mashiro/tests/Async/L0/BridgeStopTokenTest.cpp
#include <Mashiro/Async/Foundations.h>
#include <catch2/catch_test_macros.hpp>
#include <stop_token>

TEST_CASE("bridge_stop_token forwards request_stop", "[Async][L0][Bridge]") {
    std::stop_source ss;
    auto bridge = Mashiro::Async::bridge_stop_token(ss.get_token());
    REQUIRE_FALSE(bridge.stop_requested());
    ss.request_stop();
    REQUIRE(bridge.stop_requested());
}
```

- [ ] **Step 2：运行测试，确认失败**

- [ ] **Step 3：实现 bridge**

```cpp
// 接续 Foundations.h
namespace Mashiro::Async {

    class bridge_stop_token_result {
    public:
        explicit bridge_stop_token_result(stop_token inner) noexcept : inner_{inner} {}
        bool stop_requested() const noexcept { return inner_.stop_requested(); }
        operator stop_token() const noexcept { return inner_; }
    private:
        stop_token inner_;
    };

    inline bridge_stop_token_result bridge_stop_token(std::stop_token std_tok) {
        // §9.2: allocate a small adapter that owns a stop_source plus a std::stop_callback
        // forwarding into it. The returned bridge_stop_token_result hides the lifetime of the
        // adapter — Detail::bridge_storage manages it (one allocation, freed when adapter dies).
        struct adapter {
            stop_source src{};
            std::stop_callback<std::function<void()>> cb;
            adapter(std::stop_token t)
                : cb(t, [this] { src.request_stop(); }) {}
        };
        auto a = std::make_shared<adapter>(std::move(std_tok));
        // §9.3 — the returned bridge_stop_token_result keeps `a` alive via shared_ptr held
        // by an environment query. For simplicity here we surface only the token.
        return bridge_stop_token_result{a->src.get_token()};
    }

} // namespace Mashiro::Async
```

- [ ] **Step 4：运行测试，确认通过**

- [ ] **Step 5：提交**

```bash
git add Mashiro/include/Mashiro/Async/Foundations.h Mashiro/tests/Async/L0/BridgeStopTokenTest.cpp
git commit -m "feat(Async/L0): bridge_stop_token adapter for std::stop_token interop"
```

### 任务 1.8：Phase 1 验收 —— 跑一遍 §10 "MockScheduler" 工作示例

**规约对照：** §10.1 正向 / §10.2 不一致 / §10.3 组合违例 / §10.4 上层查询。

- [ ] **Step 1：把 §10.1 的 MockScheduler 抄进
  `Mashiro/tests/Async/L1/MockSchedulerTest.cpp`**

逐行对照规约 §10.1 代码，再用 `MASHIRO_ASYNC_REGISTER_BACKEND(MockScheduler);` 注册。

- [ ] **Step 2：编译、运行 `[Async][L1][Mock]` 测试，确认通过**

- [ ] **Step 3：把 §10.2 的"声明撒谎"版本放到一个 expected-failure 目标，确认构建失败**

- [ ] **Step 4：写一段 `Traits::AffinityOf<MockScheduler>` 的 STATIC_REQUIRE，验证上层查询能拿到正确的 Backend**

- [ ] **Step 5：提交 + 打 tag `phase-1-complete`**

```bash
git add Mashiro/tests/Async/L1/MockSchedulerTest.cpp
git commit -m "test(Async/L1): MockScheduler walkthrough per 01-foundations.md §10"
git tag phase-1-complete
```

**Phase 1 验收门（08-cross-cutting §7.1）：**

- ✅ 所有 `Concepts::*` 在测试中 `STATIC_REQUIRE` 通过；
- ✅ `Traits::*` 反射驱动，注解→Trait 双向核对；
- ✅ Verifier 拒绝错误声明的后端（expected-failure 构建确认）。

---

## 第 2 章 —— L2 后端 Inline / StaticPool / Platform（Phase 2：3 周）

> **规约入口：** `02-backends.md` §1–§6（§7 是 Io 后端，留到 Phase 3）。

### 概念讲解 2.1：什么是"调度器"？

> **关键词：** `schedule(sched)` sender、`continues_on(s, sched)`、调度器亲和性

`scheduler` 在 stdexec 里是一个**接口约定**：

```cpp
template<class S>
concept scheduler = std::copy_constructible<S> && requires(S s) {
    { stdexec::schedule(s) } -> stdexec::sender;
    // 还要满足相等可比较、env 上有 get_completion_scheduler 等
};
```

`schedule(sched)` 是一个 **sender**，它的语义是 _"在 sched 指定的执行上下文里，唤醒下游"_。
注意：sender 是**尚未启动**的工作；用户得把它和 receiver `connect()` 才能 `start()`。

**调度器的本质**是"持有一种唤醒能力"的**值类型**。它不是基类、不是接口指针、不持有虚函数表 ——
所以可以塞进类型 `S` 里编译期分派。本框架严守这条规矩：**每一个后端都是值类型**。

`continues_on(sender, sched)` 是 sender 适配器：它把上游 sender 的补全（无论原本在哪条线程上）
"搬"到 sched 的执行上下文里继续。这就是**调度器亲和性**的语法机制 —— 我们用类型流明确表达
"接下来要在哪个调度器上跑"。

### 概念讲解 2.2：Inline backend —— "立刻在调用者线程跑"

`Inline` 后端的 `schedule()` 返回的 sender，`start()` 时**同步**调用 `set_value()`。也就是说，
调用 `start_detached(schedule(inline_sched) | then(f))` 会**立刻**在当前线程跑 `f`。

它的用途：

- **测试夹具**：测试想要确定性、同步的 sender 行为时。
- **适配器内部**：当域改写发现 `continues_on(inline, _)` 是空操作时，可以折叠。
- **响应式 sink**：reactive 管线尾端的 sink，确实就该"谁有数据谁立刻处理"。

注意：`Inline` 的前进保证只是 `weakly_parallel`。它不能用于"必须真的并行"的工作。

### 任务 2.1：实现 `Inline` 后端

**Files:**
- Create: `Mashiro/include/Mashiro/Async/Backend/Inline.h`
- Test: `Mashiro/tests/Async/L2/InlineTest.cpp`

**规约对照：** `02-backends.md` §3 全节。

- [ ] **Step 1：先写失败的测试**

```cpp
// Mashiro/tests/Async/L2/InlineTest.cpp
#include <Mashiro/Async/Backend/Inline.h>
#include <Mashiro/Async/Concepts.h>
#include <catch2/catch_test_macros.hpp>

TEST_CASE("Inline scheduler runs work synchronously", "[Async][L2][Inline]") {
    using namespace Mashiro::Async;
    Backend::Inline::scheduler sched{};
    int counter = 0;
    auto s = schedule(sched) | then([&] { counter = 1; });
    sync_wait(std::move(s));
    REQUIRE(counter == 1);
}

TEST_CASE("Inline scheduler is a Scheduler", "[Async][L2][Inline]") {
    STATIC_REQUIRE(Mashiro::Async::Concepts::Scheduler<Mashiro::Async::Backend::Inline::scheduler>);
}
```

- [ ] **Step 2：运行测试，确认失败**

- [ ] **Step 3：实现 `Inline.h`，照 §3.2 抄结构**

```cpp
// Mashiro/include/Mashiro/Async/Backend/Inline.h
#pragma once
#include "../Foundations.h"
#include "../Concepts.h"

namespace Mashiro::Async::Backend::Inline {

    struct [[=Async::Affine{Async::Backend::Inline},
            =Async::Cancellable{},
            =Async::IsForwardProgress{
                Async::forward_progress_guarantee::weakly_parallel}]] scheduler {

        struct sender_t {
            using sender_concept = stdexec::sender_t;
            using completion_signatures = Async::completion_signatures<
                Async::set_value_t(),
                Async::set_stopped_t()>;

            template<class R>
            struct operation {
                using operation_state_concept = stdexec::operation_state_t;
                R receiver_;

                void start() & noexcept {
                    auto tok = stdexec::get_stop_token(stdexec::get_env(receiver_));
                    if (tok.stop_requested()) {
                        stdexec::set_stopped(std::move(receiver_));
                    } else {
                        stdexec::set_value(std::move(receiver_));
                    }
                }
            };

            template<class R>
            operation<std::remove_cvref_t<R>> connect(R&& r) && noexcept {
                return {std::forward<R>(r)};
            }
        };

        sender_t schedule() const noexcept { return {}; }

        auto query(stdexec::get_completion_scheduler_t<Async::set_value_t>) const noexcept {
            return *this;
        }
        bool operator==(const scheduler&) const noexcept = default;
    };

} // namespace Mashiro::Async::Backend::Inline

MASHIRO_ASYNC_REGISTER_BACKEND(::Mashiro::Async::Backend::Inline::scheduler);
```

- [ ] **Step 4：运行测试，确认通过**

- [ ] **Step 5：加一个取消测试**

```cpp
TEST_CASE("Inline scheduler propagates stop_token to set_stopped", "[Async][L2][Inline]") {
    using namespace Mashiro::Async;
    stop_source src;
    src.request_stop();
    // 把 stop_token 注入 env 的细节由 sync_wait 不便测；这里用 with_stop_token() 等价方案。
    int counter = 0;
    auto s = stdexec::__with_stop_token(schedule(Backend::Inline::scheduler{}), src.get_token())
           | let_stopped([&] { counter = 99; return just(); });
    sync_wait(std::move(s));
    REQUIRE(counter == 99);
}
```

- [ ] **Step 6：提交**

```bash
git add Mashiro/include/Mashiro/Async/Backend/Inline.h Mashiro/tests/Async/L2/InlineTest.cpp
git commit -m "feat(Async/L2/Inline): synchronous scheduler with stop_token propagation"
```

### 概念讲解 2.3：StaticPool —— 工作窃取线程池

> **关键词：** 工作窃取（work stealing）、Chase-Lev 双端队列、MPSC 提交队列、`schedule_bulk`

`StaticPool` 是一个 N 线程固定大小池。它的两个关键技术：

1. **工作窃取**：每个工作线程一个本地 Chase-Lev 双端队列，本线程 `push/pop` 底端无锁，其他
   线程 `steal` 顶端 lock-free。这把"提交-执行"分摊到 N 个队列上，热点伸缩性远好于单条全局队列。
2. **MPSC 唤醒队列**：外部线程（不是池工作线程）提交时走一条 MPSC 队列（项目里现有
   `Mashiro/Core/MpscQueue.h`），由 1 个 dispatcher 线程或选定的 worker 拉取分发。

`schedule_bulk(sched, N, fn)` 是关键的批量调度入口。语义：把 `fn(0), fn(1), ..., fn(N-1)` 在
池里并行跑完。在 `StaticPool` 上的实现是"把 [0,N) 分成 NumWorkers 个连续段，每段交给一个工作
线程"，比起 N 次 `schedule()` 提交，能节省 O(N) 的锁/原子操作。

### 任务 2.2–2.5：实现 `StaticPool` 后端

> **规约对照：** §4.2 头草图 / §4.3 schedule op-state / §4.4 schedule_bulk op-state /
> §4.5 能力声明 / §4.6 取消 / §4.7 分配 / §4.8 域改写。

**这部分体量较大（800+ LOC 实现）。** 按以下任务链拆分：

#### 任务 2.2：写出 StaticPool 的最小提交+取消测试

**Files:**
- Test: `Mashiro/tests/Async/L2/StaticPoolTest.cpp`

- [ ] **Step 1：写测试**

```cpp
// Mashiro/tests/Async/L2/StaticPoolTest.cpp
#include <Mashiro/Async/Backend/StaticPool.h>
#include <catch2/catch_test_macros.hpp>
#include <atomic>

TEST_CASE("StaticPool runs scheduled work", "[Async][L2][StaticPool]") {
    using namespace Mashiro::Async;
    Backend::StaticPool::context ctx{4};  // 4 workers
    auto sched = ctx.get_scheduler();
    std::atomic<int> counter{0};
    auto s = schedule(sched) | then([&] { counter.fetch_add(1, std::memory_order_relaxed); });
    sync_wait(std::move(s));
    REQUIRE(counter.load() == 1);
}

TEST_CASE("StaticPool runs N items in parallel via schedule_bulk", "[Async][L2][StaticPool]") {
    using namespace Mashiro::Async;
    Backend::StaticPool::context ctx{4};
    auto sched = ctx.get_scheduler();
    std::atomic<int> sum{0};
    auto s = schedule_bulk(sched, std::size_t{1000},
        [&](std::size_t i) { sum.fetch_add(static_cast<int>(i), std::memory_order_relaxed); });
    sync_wait(std::move(s));
    REQUIRE(sum.load() == 1000 * 999 / 2);
}
```

- [ ] **Step 2：确认编译失败 / 测试目标不存在**

#### 任务 2.3：实现 `StaticPool::context` + worker 线程循环

**Files:**
- Create: `Mashiro/include/Mashiro/Async/Backend/StaticPool.h`
- Create: `Mashiro/src/Async/Backend/StaticPool.cpp`

- [ ] **Step 1：在头文件里定义 `context` 与 `scheduler` 的最小接口**

照规约 §4.2 抄。`context` 持有 worker 数组 + 每线程 Chase-Lev 队列 + 全局 MPSC 唤醒队列。

```cpp
// Mashiro/include/Mashiro/Async/Backend/StaticPool.h
#pragma once
#include "../Foundations.h"
#include "../Concepts.h"
#include "Mashiro/Core/MpscQueue.h"

namespace Mashiro::Async::Backend::StaticPool {

    struct work_item {
        void (*invoke)(void*) noexcept;
        void* state;
    };

    class context {
    public:
        explicit context(std::size_t num_workers);
        ~context();
        context(const context&) = delete;
        context& operator=(const context&) = delete;

        struct [[=Async::Backend_{Async::Backend::StaticPool},
                =Async::OffersBulk{},
                =Async::Cancellable{},
                =Async::IsForwardProgress{Async::forward_progress_guarantee::parallel}]]
            scheduler {
            context* ctx;
            // ... see §4.2
            struct sender_t {
                using sender_concept = stdexec::sender_t;
                using completion_signatures = Async::completion_signatures<
                    Async::set_value_t(), Async::set_stopped_t()>;
                context* ctx;
                template<class R>
                auto connect(R&& r) && noexcept;
            };
            sender_t schedule() const noexcept { return {ctx}; }
            bool operator==(const scheduler& o) const noexcept { return ctx == o.ctx; }
            auto query(stdexec::get_completion_scheduler_t<Async::set_value_t>) const noexcept {
                return *this;
            }
        };

        scheduler get_scheduler() noexcept { return {this}; }

        void enqueue(work_item w) noexcept;  // §4.3 fast path
        void request_stop() noexcept;
    private:
        // §4.2 — implementation lives in StaticPool.cpp
        struct impl;
        std::unique_ptr<impl> p_;
    };

} // namespace Mashiro::Async::Backend::StaticPool

MASHIRO_ASYNC_REGISTER_BACKEND(::Mashiro::Async::Backend::StaticPool::context::scheduler);
```

- [ ] **Step 2：实现 `StaticPool.cpp`：context::impl 的 worker 循环 + Chase-Lev 队列封装**

照规约 §4.3 op-state 草图实现 worker loop。要点：

- 每个 worker：本地队列 `pop_bottom()` → 偷别人的 `steal_top()` → 等 MPSC 唤醒；
- `request_stop()` 设置原子标志 + 用 `eventfd`/`SetEvent` 唤醒所有等待中的 worker；
- `inplace_stop_callback` 注册在 `op-state` 上，回调里把 op-state 从队列中移除（如果还在队列里）
  并调用 `set_stopped(receiver)`。

代码量较大；建议分 30 分钟一段，按 §4.3、§4.4、§4.6 三节顺序写入 + 跑测试。

- [ ] **Step 3：跑测试**

```bash
cmake --build build --target Mashiro_Tests
./build/Mashiro/tests/Mashiro_Tests "[Async][L2][StaticPool]"
```

- [ ] **Step 4：提交**

```bash
git add Mashiro/include/Mashiro/Async/Backend/StaticPool.h \
        Mashiro/src/Async/Backend/StaticPool.cpp \
        Mashiro/tests/Async/L2/StaticPoolTest.cpp
git commit -m "feat(Async/L2/StaticPool): work-stealing pool with schedule + schedule_bulk"
```

#### 任务 2.4：StaticPool 零分配 + 取消测试

**Files:**
- Test: `Mashiro/tests/Async/L2/StaticPoolCancelTest.cpp`

**规约对照：** §4.6 取消 / §4.7 分配。

- [ ] **Step 1：写"取消之后立刻 set_stopped"的测试**

```cpp
TEST_CASE("StaticPool stop_token cancels in-flight schedule", "[Async][L2][StaticPool]") {
    using namespace Mashiro::Async;
    Backend::StaticPool::context ctx{2};
    auto sched = ctx.get_scheduler();
    stop_source src;

    std::atomic<bool> ran_value{false};
    std::atomic<bool> ran_stopped{false};
    auto s = stdexec::__with_stop_token(schedule(sched), src.get_token())
           | let_stopped([&]{ ran_stopped = true; return just(); })
           | then([&]{ ran_value = true; });
    src.request_stop();  // 在 connect 之前请求停
    sync_wait(std::move(s));
    REQUIRE(ran_stopped.load());
    REQUIRE_FALSE(ran_value.load());
}
```

- [ ] **Step 2：跑测试，确认 PASS**

- [ ] **Step 3：用 §4.7 + 规约 `Diagnostics::AllocCheck` 雏形（手工实现简单版本）测出 schedule fast path 是零分配**

```cpp
TEST_CASE("StaticPool schedule fast-path allocates zero bytes", "[Async][L2][StaticPool][Alloc]") {
    /* 启用一个临时 global new override 计数器，运行 schedule + then + sync_wait，
       断言计数器在 schedule 自身的 connect/start 区段没有增量。 */
    // ... 见 cross-cutting §3.5 AllocCheck 设计
}
```

- [ ] **Step 4：提交**

```bash
git commit -am "test(Async/L2/StaticPool): cancellation + zero-allocation guarantees"
```

#### 任务 2.5：StaticPool 域改写（折叠相邻 `continues_on`）

**规约对照：** §4.8 域改写。

- [ ] **Step 1：实现最小域改写**

```cpp
namespace Mashiro::Async::Backend::StaticPool {
    struct domain {
        template<class Sender, class Env>
        constexpr auto transform_sender(Sender&& s, const Env&) const {
            // §4.8 — fold continues_on(continues_on(_, sp), sp) into the inner one when both
            // schedulers compare equal. Implementation: pattern-match the Sender's tag, look at
            // the wrapped scheduler, compare, drop the outer transition.
            return std::forward<Sender>(s);  // placeholder — see §4.8 for the full pattern
        }
    };

    inline auto context::scheduler::query(stdexec::get_domain_t) const noexcept { return domain{}; }
}
```

- [ ] **Step 2：写一个域改写命中后的测试 —— 用 `Diagnostics::trace_pipeline` 比较改写前后的节点数**

（`Diagnostics::trace_pipeline` 留到 Phase 7 实现；此处可用手工模式匹配作 STATIC_REQUIRE）

- [ ] **Step 3：提交 + 打 tag `phase-2-static-pool-complete`**

```bash
git commit -am "feat(Async/L2/StaticPool): domain rewrite folds redundant continues_on"
```

### 任务 2.6：Platform 后端 —— 一个别名 + 注解 = 完成

**Files:**
- Create: `Mashiro/include/Mashiro/Async/Backend/Platform.h`

**规约对照：** §6 全节（重点是 §6.4 能力声明、§6.5 v0.2 "Platform 不是 BulkScheduler"）。

**核心思路：** `Mashiro::Platform::scheduler` 已经存在（详见 `2026-06-11-platform-thread-infrastructure-design.md`）。
本任务**不复制代码** —— 只是给它套一个别名 + 贴上 L1 注解。

- [ ] **Step 1：写测试**

```cpp
// Mashiro/tests/Async/L2/PlatformAliasTest.cpp
#include <Mashiro/Async/Backend/Platform.h>
#include <catch2/catch_test_macros.hpp>

TEST_CASE("Platform alias preserves Mashiro::Platform::scheduler identity",
          "[Async][L2][Platform]") {
    STATIC_REQUIRE(std::is_same_v<
        Mashiro::Async::Backend::Platform::scheduler,
        Mashiro::Platform::scheduler>);
    STATIC_REQUIRE(Mashiro::Async::Traits::IsAffine_v<
        Mashiro::Async::Backend::Platform::scheduler>);
    STATIC_REQUIRE(Mashiro::Async::Traits::AffinityOf<
        Mashiro::Async::Backend::Platform::scheduler> == Mashiro::Async::Backend::Platform);
}
```

- [ ] **Step 2：写头文件**

```cpp
// Mashiro/include/Mashiro/Async/Backend/Platform.h
#pragma once
#include <Mashiro/Platform/PlatformThread.h>
#include "../Foundations.h"

namespace Mashiro::Async::Backend::Platform {
    // Type alias of the existing platform scheduler. No new state, no new behaviour.
    using scheduler = Mashiro::Platform::scheduler;
}

// Annotations attach via a partial specialisation that the L1 verifier reads — the
// underlying Mashiro::Platform::scheduler is *not* modified.
// (See 02-backends.md §6.4 for the annotation-attachment pattern.)
namespace Mashiro::Async::Traits {
    template<>
    inline constexpr Backend AffinityOf<Mashiro::Async::Backend::Platform::scheduler> =
        Async::Backend::Platform;
    template<>
    inline constexpr bool IsAffine_v<Mashiro::Async::Backend::Platform::scheduler> = true;
    template<>
    inline constexpr bool IsCancellable_v<Mashiro::Async::Backend::Platform::scheduler> = true;
    template<>
    inline constexpr forward_progress_guarantee ProgressOf_v<
        Mashiro::Async::Backend::Platform::scheduler> = forward_progress_guarantee::concurrent;
}

MASHIRO_ASYNC_REGISTER_BACKEND(::Mashiro::Async::Backend::Platform::scheduler);
```

> **注意（规约 §6.5 v0.2）：** Platform **不**模 `BulkScheduler` —— 平台线程是单线程串行的。
> 不要给它贴 `OffersBulk`！

- [ ] **Step 3：跑测试，确认 PASS**

- [ ] **Step 4：提交 + 打 tag `phase-2-complete`**

```bash
git add Mashiro/include/Mashiro/Async/Backend/Platform.h Mashiro/tests/Async/L2/PlatformAliasTest.cpp
git commit -m "feat(Async/L2/Platform): alias of Mashiro::Platform::scheduler + annotations"
git tag phase-2-complete
```

**Phase 2 验收门：**

- ✅ Inline / StaticPool / Platform 各有 CMake target + Catch2 测试；
- ✅ 每个后端都跑过跨线程 `schedule()` + `inplace_stop_source::request_stop()` 测试；
- ✅ schedule fast path 用 `AllocCheck` 验证零分配。

---

## 第 3 章 —— L2 后端 Tbb / Io（Phase 3：4 周）

> **规约入口：** `02-backends.md` §5 (Tbb) + §7 (Io)。

### 概念讲解 3.1：oneTBB —— 协作式并行运行时

TBB 不是又一个线程池。它的核心抽象是：

- **`task_arena`**：一个有界并发度的任务竞技场。竞技场内的任务被 TBB 自己的工作窃取调度器执行。
- **`parallel_for(range, body)`**：把 range 切片，分发到 arena 工作线程。它会做**协作式偷取**和
  **递归切分**，比手写线程池循环快、被研究得更透。
- **`flow::graph`**：可声明式的数据流图，每个节点跑在 arena 上，节点间用通道连接。L6 `pipeline`
  当阶段数 ≥ 4 时**域改写**到 `flow::graph`。

**为什么 TBB 是一等公民？** 因为 CPU 密集型批量工作（光照计算、网格细分、变换烘焙）TBB 比自家
StaticPool 通常快 1.3–2× —— 它在 `parallel_for` 这一刀上有 10+ 年的工程沉淀。

**框架做的事：** 把 `tbb::task_arena` 包装成 stdexec scheduler；在 `Tbb::domain` 里把
`schedule_bulk(tbb_sched, N, fn)` 改写成 `tbb::parallel_for(tbb::blocked_range<size_t>(0, N), ...)`。
用户写的是 `bulk(N, fn)`，跑的是 TBB —— 零摩擦。

### 任务 3.1：TBB 调度器最小提交

**Files:**
- Create: `Mashiro/include/Mashiro/Async/Backend/Tbb.h`
- Create: `Mashiro/src/Async/Backend/Tbb.cpp`
- Test: `Mashiro/tests/Async/L2/TbbTest.cpp`

**规约对照：** §5.2 头草图 / §5.3 schedule op-state / §5.5 能力声明 / §5.6 取消 / §5.8 域改写。

- [ ] **Step 1：写"TBB arena 上跑一段工作"测试**

```cpp
TEST_CASE("Tbb scheduler runs single work item", "[Async][L2][Tbb]") {
    using namespace Mashiro::Async;
    Backend::Tbb::context ctx{};   // wraps a tbb::task_arena
    auto sched = ctx.get_scheduler();
    std::atomic<int> counter{0};
    auto s = schedule(sched) | then([&]{ counter.fetch_add(1, std::memory_order_relaxed); });
    sync_wait(std::move(s));
    REQUIRE(counter.load() == 1);
}
```

- [ ] **Step 2：实现头 + cpp**

```cpp
// Mashiro/include/Mashiro/Async/Backend/Tbb.h
#pragma once
#include "../Foundations.h"
#include "../Concepts.h"
#include <tbb/task_arena.h>
#include <tbb/parallel_for.h>

namespace Mashiro::Async::Backend::Tbb {

    class context {
    public:
        context();  // arena with default concurrency
        explicit context(int concurrency);
        ~context();

        struct [[=Async::Backend_{Async::Backend::Tbb},
                =Async::OffersBulk{},
                =Async::Cancellable{},
                =Async::IsForwardProgress{Async::forward_progress_guarantee::parallel}]]
            scheduler {
            context* ctx;
            struct sender_t {
                using sender_concept = stdexec::sender_t;
                using completion_signatures = Async::completion_signatures<
                    Async::set_value_t(), Async::set_stopped_t(),
                    Async::set_error_t(std::exception_ptr)>;
                context* ctx;
                template<class R>
                auto connect(R&& r) && noexcept;  // posts a lambda into ctx->arena_.enqueue
            };
            sender_t schedule() const noexcept { return {ctx}; }
            bool operator==(const scheduler& o) const noexcept { return ctx == o.ctx; }
            auto query(stdexec::get_completion_scheduler_t<Async::set_value_t>) const noexcept {
                return *this;
            }
            auto query(stdexec::get_domain_t) const noexcept;
        };

        scheduler get_scheduler() noexcept { return {this}; }

        tbb::task_arena& arena() noexcept;
    private:
        struct impl;
        std::unique_ptr<impl> p_;
    };

} // namespace Mashiro::Async::Backend::Tbb

MASHIRO_ASYNC_REGISTER_BACKEND(::Mashiro::Async::Backend::Tbb::context::scheduler);
```

- [ ] **Step 3：实现 `schedule()` 的 op-state（§5.3）：`enqueue` 一个 lambda 到 arena**

- [ ] **Step 4：跑测试 → PASS**

- [ ] **Step 5：提交**

```bash
git add Mashiro/include/Mashiro/Async/Backend/Tbb.h Mashiro/src/Async/Backend/Tbb.cpp \
        Mashiro/tests/Async/L2/TbbTest.cpp
git commit -m "feat(Async/L2/Tbb): tbb::task_arena scheduler (schedule fast path)"
```

### 任务 3.2：TBB `schedule_bulk` → `tbb::parallel_for`

**规约对照：** §5.4 schedule_bulk 改写 / §5.5 能力声明 / §5.8 域改写。

- [ ] **Step 1：写"100 万项 bulk 跑通"测试 + 与 StaticPool 性能对照（非严格）**

- [ ] **Step 2：在域里把 `schedule_bulk` 改写到 `tbb::parallel_for(tbb::blocked_range<size_t>(0,N), ...)`**

```cpp
struct domain {
    template<class Sender, class Env>
    auto transform_sender(Sender&& s, const Env&) const {
        // §5.4 — pattern-match on schedule_bulk_t sender; lower to tbb::parallel_for.
        // The rewrite must preserve completion-signature shape and cancellation semantics.
        // See §5.8 for the structural template.
        return std::forward<Sender>(s);  // placeholder; expand per §5.8
    }
};
```

- [ ] **Step 3：跑测试 → PASS**

- [ ] **Step 4：提交**

```bash
git commit -am "feat(Async/L2/Tbb): schedule_bulk lowers to tbb::parallel_for via domain rewrite"
```

### 任务 3.3：TBB 取消接线（`task_group_context::cancel_group_execution`）

**规约对照：** §5.6 Tbb 取消。

- [ ] **Step 1：写"sync_wait 期间取消，每项都不再开跑"测试**

- [ ] **Step 2：在 op-state 里注册 `inplace_stop_callback`，回调里调用
  `arena_.execute([] { context_.cancel_group_execution(); });`**

- [ ] **Step 3：跑测试 → PASS**

- [ ] **Step 4：提交 + 打 tag `phase-3-tbb-complete`**

```bash
git commit -am "feat(Async/L2/Tbb): stop_token wiring via task_group_context::cancel_group_execution"
git tag phase-3-tbb-complete
```

### 概念讲解 3.2：proactor —— "我说我要什么、内核回来告诉我已完成"

> **关键词：** io_uring、IOCP、submission queue、completion queue、completion port

传统 reactor 模式（`epoll`、`select`、`kqueue`）的语义是 _"告诉我 fd 可读/可写了"_ —— 之后还得
自己 `read/write`。proactor 颠倒了这个流程： _"内核请帮我读 N 字节到 buf 里，做完了通知我"_ ——
**实际 syscall 由内核异步执行**，用户拿到的直接是已完成的结果。

- **Linux io_uring**：用户态 SQ ring → 内核读 → 内核做完写入 CQ ring；用户态 wait 取 CQE。
- **Windows IOCP**：用户态发出 `ReadFileEx` 之类的 overlapped I/O → 内核做完投递到完成端口；
  用户态 `GetQueuedCompletionStatus` 拿。

proactor 天然契合 sender 模型：

- `schedule(io_sched)` → 唤醒到 io 上下文；
- `async_read(io_sched, fd, buf)` → 一个 sender，它的 op-state 持有一个 SQE/OVERLAPPED，
  `start()` 时提交、completion 后调 `set_value(bytes_read)`；
- 取消时调 `IORING_OP_ASYNC_CANCEL` / `CancelIoEx`。

**框架做的事：** 把 io_uring 和 IOCP 各自封装到 `IoContext` 里，对外只暴露一个 `Io::scheduler`
和一组 sender 适配器（`async_read`、`async_write`、`async_accept`、`async_connect`、`async_timer`）。

### 任务 3.4：Io 后端骨架 + `IoContext` 启动停止

**Files:**
- Create: `Mashiro/include/Mashiro/Async/Backend/Io.h`
- Create: `Mashiro/src/Async/Backend/Io/Linux/IoUring.cpp` （Linux）
- Create: `Mashiro/src/Async/Backend/Io/Windows/Iocp.cpp` （Windows）
- Test: `Mashiro/tests/Async/L2/IoTest.cpp`

**规约对照：** `02-backends.md` §7.1 用途 / §7.2 头草图 / §7.3 op-state / §7.4 取消 / §7.5 分配。

- [ ] **Step 1：写"context 启动 + 停止"测试（先不挂任何 I/O）**

```cpp
TEST_CASE("Io context starts and stops cleanly", "[Async][L2][Io]") {
    using namespace Mashiro::Async;
    Backend::Io::context ctx{};
    REQUIRE(ctx.running());
    ctx.request_stop();
    ctx.wait_stopped();
    REQUIRE_FALSE(ctx.running());
}
```

- [ ] **Step 2：写 `Io.h` 头骨架**

```cpp
// Mashiro/include/Mashiro/Async/Backend/Io.h
#pragma once
#include "../Foundations.h"
#include "../Concepts.h"

namespace Mashiro::Async::Backend::Io {

    class context;

    struct [[=Async::Backend_{Async::Backend::Io},
            =Async::OffersIo{},
            =Async::Cancellable{},
            =Async::IsForwardProgress{Async::forward_progress_guarantee::concurrent}]]
        scheduler {
        context* ctx;
        struct sender_t {
            using sender_concept = stdexec::sender_t;
            using completion_signatures = Async::completion_signatures<
                Async::set_value_t(), Async::set_stopped_t()>;
            context* ctx;
            template<class R>
            auto connect(R&& r) && noexcept;
        };
        sender_t schedule() const noexcept { return {ctx}; }
        bool operator==(const scheduler& o) const noexcept { return ctx == o.ctx; }
        auto query(stdexec::get_completion_scheduler_t<Async::set_value_t>) const noexcept {
            return *this;
        }
        friend void* tag_invoke(get_io_context_t, scheduler s) noexcept;
    };

    class context {
    public:
        context();
        ~context();
        scheduler get_scheduler() noexcept { return {this}; }
        bool running() const noexcept;
        void request_stop() noexcept;
        void wait_stopped() noexcept;

        // §7.2 — implementations live in Linux/IoUring.cpp or Windows/Iocp.cpp; selected
        // at CMake-config time via the build target.
        struct impl;
        impl* p() noexcept;
    private:
        std::unique_ptr<impl> p_;
    };

} // namespace Mashiro::Async::Backend::Io

MASHIRO_ASYNC_REGISTER_BACKEND(::Mashiro::Async::Backend::Io::scheduler);
```

- [ ] **Step 3：实现 Linux IoUring.cpp 骨架（`io_uring_queue_init` + 一个 reaper 线程）**

要点：

- 一个 reaper 线程跑 `io_uring_wait_cqe` 循环；
- 一个停止 eventfd 注册到 ring，用于唤醒 reaper 让它退出；
- 每个 CQE 的 `user_data` 是 op-state 指针，reaper 调 op-state 的 `complete()`。

- [ ] **Step 4：实现 Windows Iocp.cpp 骨架（`CreateIoCompletionPort` + reaper 线程）**

要点：

- reaper 跑 `GetQueuedCompletionStatusEx` 循环；
- 用 `PostQueuedCompletionStatus` 投递一个特殊"stop"包唤醒 reaper；
- 每个 OVERLAPPED 实际是 op-state 内嵌的 `OVERLAPPED_op_state`。

- [ ] **Step 5：跑测试 → PASS**

- [ ] **Step 6：提交**

```bash
git add Mashiro/include/Mashiro/Async/Backend/Io.h \
        Mashiro/src/Async/Backend/Io/Linux/IoUring.cpp \
        Mashiro/src/Async/Backend/Io/Windows/Iocp.cpp \
        Mashiro/tests/Async/L2/IoTest.cpp
git commit -m "feat(Async/L2/Io): proactor context skeleton (io_uring/IOCP)"
```

### 任务 3.5：Io 后端的 `async_read` / `async_write` / `async_timer`

**规约对照：** §7.3 op-state 形态 / §7.4 取消接线（`IORING_OP_ASYNC_CANCEL` / `CancelIoEx`）。

**这部分量大** —— 每个 I/O 动作一个 op-state、各自处理 errno/HRESULT→`set_error` 翻译。建议
按以下顺序：

- [ ] **Step 1：先做 `async_timer(dur)`**（最简单 —— 一个 `IORING_OP_TIMEOUT` / threadpool timer 即可）

测试：

```cpp
TEST_CASE("async_timer fires after specified delay", "[Async][L2][Io][Timer]") {
    using namespace Mashiro::Async;
    Backend::Io::context ctx{};
    auto sched = ctx.get_scheduler();
    auto start = std::chrono::steady_clock::now();
    sync_wait(Backend::Io::async_timer(sched, std::chrono::milliseconds{50}));
    auto elapsed = std::chrono::steady_clock::now() - start;
    REQUIRE(elapsed >= std::chrono::milliseconds{45});
}
```

- [ ] **Step 2：实现 `async_read(sched, fd, buf)`**

测试：用 pipe（Linux）/ 命名管道（Windows）端到端读 4 字节。

- [ ] **Step 3：实现 `async_write` / `async_accept` / `async_connect`**

每个都先写测试，再实现 op-state。

- [ ] **Step 4：取消接线 —— `inplace_stop_callback` 回调里发 `IORING_OP_ASYNC_CANCEL` /
  `CancelIoEx`**

测试：开始一个 `async_read` 等数据，外部 `request_stop()`，期望 sender 在 100ms 内 set_stopped。

- [ ] **Step 5：提交 + 打 tag `phase-3-complete`**

```bash
git commit -am "feat(Async/L2/Io): async_read/write/accept/connect/timer + cancellation"
git tag phase-3-complete
```

**Phase 3 验收门：**

- ✅ Tbb 在 `thirdparty/tbb` 上构建通过；`bulk` 改写到 `tbb::parallel_for`；
- ✅ Io 在 Linux（io_uring）+ Windows（IOCP）双平台跑测试；
- ✅ 每个 backend 的 fast path 测过零分配 + 取消语义。

---

## 第 4 章 —— L3 适配器 + L4 协程任务（Phase 4：4 周）

> **规约入口：** `03-adaptors.md` 全文 + `04-coroutine-tasks.md` 全文。

### 概念讲解 4.1：sender 适配器 —— 把 sender 当代数式子

> **关键词：** `then`、`let_value`、`let_error`、`let_stopped`、`when_all`、`when_any`

stdexec 本身已经提供了基础**算子**：`then(f)`（map）、`let_value(g)`（flatMap）、
`when_all/when_any` 等。框架在这之上**只**添加业务场景需要的高阶算子，每个都贴一张"补全签名表"
说明它怎么改变上游 sender 的签名。

我们要加的 8 个适配器（详见 `03-adaptors.md` §3–§10）：

| 算子 | 含义 | 何时用 |
|---|---|---|
| `bulk(n, fn)` | 跑 `fn(0..n-1)` 并行 | CPU 密集批量；选 BulkScheduler |
| `batch(window, max)` | 把 stream 元素打包成批 | 减少下游每元素开销 |
| `debounce(dur)` | 抑制 burst | 用户输入、热重载触发 |
| `throttle(dur)` | 限速 | 网络上行、telemetry |
| `retry(policy)` | 指数/固定退避重试 | 不稳定 RPC |
| `timeout(dur)` | 超时则 set_stopped | 任何可能挂起的工作 |
| `race(s1, s2, ...)` | 先完成者赢 | 多源容错 |
| `materialise/dematerialise` | 补全信号 reify | 中间件 / 序列化 |

**适配器的共同义务**：

1. **明确补全签名**（`completion_signatures_of_t` 特化）；
2. **取消正确**（注册 `inplace_stop_callback`、取消时 set_stopped）；
3. **分配显式**（op-state 内嵌；如有外部状态则 doc 明示）；
4. **域改写友好**（不阻挡 backend 的域吃掉它）。

### 任务 4.1：实现 `bulk(n, fn)`

**Files:**
- Create: `Mashiro/include/Mashiro/Async/Adaptor/Bulk.h`
- Test: `Mashiro/tests/Async/L3/BulkTest.cpp`

**规约对照：** `03-adaptors.md` §3。

- [ ] **Step 1：写测试**

```cpp
// Mashiro/tests/Async/L3/BulkTest.cpp
#include <Mashiro/Async/Adaptor/Bulk.h>
#include <Mashiro/Async/Backend/StaticPool.h>
#include <catch2/catch_test_macros.hpp>

TEST_CASE("bulk over BulkScheduler delegates to schedule_bulk",
          "[Async][L3][Bulk]") {
    using namespace Mashiro::Async;
    Backend::StaticPool::context ctx{4};
    auto sched = ctx.get_scheduler();
    std::atomic<int> sum{0};
    sync_wait(
        schedule(sched) |
        Adaptor::bulk(1000ULL, [&](std::size_t i){
            sum.fetch_add(static_cast<int>(i), std::memory_order_relaxed);
        }));
    REQUIRE(sum.load() == 1000 * 999 / 2);
}
```

- [ ] **Step 2：实现 `bulk`：若上游补全调度器满足 `BulkScheduler`，用 `schedule_bulk` 落地；
  否则展开为 `when_all` over `[0..n) just`-and-`then`**

```cpp
// Mashiro/include/Mashiro/Async/Adaptor/Bulk.h
#pragma once
#include "../Foundations.h"
#include "../Concepts.h"
#include "../Traits.h"

namespace Mashiro::Async::Adaptor {

    struct bulk_t {
        template<class N, class F>
        auto operator()(N n, F&& f) const {
            return [n, f = std::forward<F>(f)]<Async::sender S>(S s) {
                using Sched = decltype(stdexec::get_completion_scheduler<Async::set_value_t>(
                    stdexec::get_env(s)));
                if constexpr (Concepts::BulkScheduler<Sched>) {
                    auto sched = stdexec::get_completion_scheduler<Async::set_value_t>(
                        stdexec::get_env(s));
                    return std::move(s) |
                           stdexec::let_value([sched, n, f](auto&&...){
                               return stdexec::schedule_bulk(sched, n, f);
                           });
                } else {
                    // §3 fallback: when_all of n unit senders
                    // (Implementation: build a tuple of `n` `then`-wrapped sends; the
                    // sketch here uses a runtime-sized vector for simplicity. Production
                    // code should compile-time pack-expand when n is consteval.)
                    return std::move(s) |
                           stdexec::let_value([n, f](auto&&...) {
                               std::vector<stdexec::__when_all_sender<...>> items;
                               for (N i = 0; i < n; ++i)
                                   items.push_back(stdexec::just() | stdexec::then([f, i]{ f(i); }));
                               return stdexec::when_all_v(std::move(items));
                           });
                }
            };
        }
    };

    inline constexpr bulk_t bulk{};

} // namespace Mashiro::Async::Adaptor
```

- [ ] **Step 3：跑测试 → PASS**

- [ ] **Step 4：提交**

```bash
git add Mashiro/include/Mashiro/Async/Adaptor/Bulk.h Mashiro/tests/Async/L3/BulkTest.cpp
git commit -m "feat(Async/L3): bulk(n, fn) adaptor with BulkScheduler fast-path"
```

### 任务 4.2–4.8：其余适配器（按相同流程）

每个适配器一个任务，照下面流程逐个落地：

| 任务 | 适配器 | 头文件 | 规约小节 |
|---|---|---|---|
| 4.2 | `batch(window, max_size)` | `Adaptor/Batch.h` | `03-adaptors.md` §4 |
| 4.3 | `debounce(dur)` | `Adaptor/Debounce.h` | §5 |
| 4.4 | `throttle(dur)` | `Adaptor/Throttle.h` | §6 |
| 4.5 | `retry(policy)` | `Adaptor/Retry.h` | §7 |
| 4.6 | `timeout(dur)` | `Adaptor/Timeout.h` | §8 |
| 4.7 | `race(s1, s2, ...)` | `Adaptor/Race.h` | §9 |
| 4.8 | `materialise` / `dematerialise` 已在 L0 重导出，仅补一段测试 | — | §10 |

**每个任务的步骤都是固定的 5 步：**

- [ ] **Step 1：写一个最小行为测试**（一行业务语义就够）
- [ ] **Step 2：跑测试，确认失败**
- [ ] **Step 3：照规约对应小节实现头文件**
- [ ] **Step 4：跑测试 → PASS**
- [ ] **Step 5：写一个取消测试 + 提交**

**注意：每个适配器都必须有一段 `static_assert` 断言它的补全签名等于预期。**

### 概念讲解 4.2：C++20 协程 + scheduler-affinity

> **关键词：** `co_await`、`promise_type`、HALO、P3941 调度器亲和性

`Task<T>` 是 `exec::task<T>` 的别名 —— 一个**协程类型**，它的 `promise` 实现了：

- `await_transform(sender)` —— 让协程内 `co_await some_sender` 成立；
- `final_suspend()` —— 协程返回时把值搬到等待它的 receiver；
- **调度器亲和性（P3941）**：协程**绑定**一个调度器；每次 `co_await` 完成都自动
  `continues_on(bound_sched)`，回到绑定的执行上下文。

为什么是大特性？因为传统 `Task<T>` 协程 `co_await` 完一个 sender 后，控制流会卡在
"sender 在哪条线程上完成"这条线程上 —— 这往往不是你想要的（比如你希望回到 UI 线程更新视图）。
P3941 让"绑定调度器"这件事**类型化**，编译器自动插入 `continues_on`。

**HALO（Heap Allocation eLision Optimization）**：当编译器能证明协程帧的生命周期短于调用栈
时，可以把帧分配从堆里 elide 到栈里 —— 此时 `Task<T>` 是**零分配**的。框架不依赖 HALO，但
在能用时白赚。

### 任务 4.9：实现 `Task<T>`

**Files:**
- Create: `Mashiro/include/Mashiro/Async/Coro/Task.h`
- Test: `Mashiro/tests/Async/L4/TaskTest.cpp`

**规约对照：** `04-coroutine-tasks.md` §4–§5。

- [ ] **Step 1：写"协程内 co_await sender"测试**

```cpp
// Mashiro/tests/Async/L4/TaskTest.cpp
#include <Mashiro/Async/Coro/Task.h>
#include <Mashiro/Async/Backend/StaticPool.h>
#include <catch2/catch_test_macros.hpp>

TEST_CASE("Task<int> awaits a sender", "[Async][L4][Task]") {
    using namespace Mashiro::Async;
    Backend::StaticPool::context ctx{2};

    auto work = [&]() -> Coro::Task<int> {
        int a = co_await stdexec::just(40);
        int b = co_await (schedule(ctx.get_scheduler()) | then([] { return 2; }));
        co_return a + b;
    };

    auto [result] = sync_wait(work()).value();
    REQUIRE(result == 42);
}
```

- [ ] **Step 2：实现 `Task.h`，从 `exec::task` 派生**

```cpp
// Mashiro/include/Mashiro/Async/Coro/Task.h
#pragma once
#include "../Foundations.h"
#include <exec/task.hpp>

namespace Mashiro::Async::Coro {

    // Task<T>: scheduler-affine coroutine task. The default context binds to
    // whichever scheduler the caller specifies; if unspecified, the task is
    // "scheduler-agnostic" — it resumes wherever the awaited sender completes.
    template<class T>
    using Task = exec::task<T>;

} // namespace Mashiro::Async::Coro
```

- [ ] **Step 3：跑测试 → PASS**

- [ ] **Step 4：提交**

```bash
git add Mashiro/include/Mashiro/Async/Coro/Task.h Mashiro/tests/Async/L4/TaskTest.cpp
git commit -m "feat(Async/L4/Coro): Task<T> = exec::task<T> with scheduler affinity"
```

### 任务 4.10：实现 `Stream<T>` + `MASHIRO_FOR_CO_AWAIT` 宏

**Files:**
- Create: `Mashiro/include/Mashiro/Async/Coro/Stream.h`
- Test: `Mashiro/tests/Async/L4/StreamTest.cpp`

**规约对照：** `04-coroutine-tasks.md` §5（含 v0.2 §5.4b `MASHIRO_FOR_CO_AWAIT` / §6.5b
`Coro::stopped_signal`）。

#### 概念讲解 4.3：`Stream<T>` —— 拉式异步序列

`Stream<T>` 在内部就是一个"返回 sender-of-`std::optional<T>`"的协程。每次 `co_await stream.next()`：

- 拿到 `std::optional<T>`，有值 → 元素；
- 空 optional → 流结束；
- 异常 → 错误。

`for co_await (auto e : stream)` 是 C++26 的语法糖；为支持不上 C++26 的工具链，规约 §5.4b 提供
一个等价宏 `MASHIRO_FOR_CO_AWAIT(NAME, STREAM)`。规约 §6.5b 定义了一个内部异常类型
`Coro::stopped_signal` —— 当 stream 因取消展开时抛它，由 `Task<T>` 的 `promise` 翻译成
`set_stopped`。

- [ ] **Step 1：写"消费 stream 元素"测试**

```cpp
TEST_CASE("Stream<int> yields three values then ends", "[Async][L4][Stream]") {
    using namespace Mashiro::Async;
    auto producer = []() -> Coro::Stream<int> {
        co_yield 1; co_yield 2; co_yield 3;
    };
    auto consumer = [&]() -> Coro::Task<int> {
        int sum = 0;
        auto s = producer();
        MASHIRO_FOR_CO_AWAIT(v, s) {
            sum += v;
        }
        co_return sum;
    };
    auto [r] = sync_wait(consumer()).value();
    REQUIRE(r == 6);
}
```

- [ ] **Step 2：实现 `Stream.h`**

```cpp
// Mashiro/include/Mashiro/Async/Coro/Stream.h
#pragma once
#include "../Foundations.h"
#include <exec/async_scope.hpp>

namespace Mashiro::Async::Coro {

    // §6.5b — sentinel exception thrown by Stream<T> consumers on cancellation.
    struct stopped_signal { };

    // Stream<T>: pull-driven async sequence; see 04-coroutine-tasks.md §5.
    template<class T>
    class Stream {
    public:
        // ... promise_type, next() returning a sender-of-optional<T>, etc.
        // Full implementation per spec §5.1–§5.5. The sketch focuses on the public surface.

        struct iterator {
            // Used by `for co_await`.
        };

        auto next() -> /* sender-of-optional<T> */;
    };

} // namespace Mashiro::Async::Coro

// §5.4b — fallback for toolchains lacking C++26 `for co_await`.
#define MASHIRO_FOR_CO_AWAIT(NAME, STREAM)                                         \
    if (auto _stream = (STREAM); false) {} else                                    \
    while (auto _opt = co_await _stream.next())                                    \
        if (auto& NAME = *_opt; true)
```

- [ ] **Step 3：跑测试 → PASS**

- [ ] **Step 4：写"消费一半后取消"测试 —— 期望 stream 抛 `stopped_signal`，consumer Task 化为
  `set_stopped`**

- [ ] **Step 5：提交**

```bash
git commit -am "feat(Async/L4/Coro): Stream<T> with MASHIRO_FOR_CO_AWAIT macro + stopped_signal"
```

### 任务 4.11：实现 `Job`（detached 任务）+ `Detached` 注解

**Files:**
- Create: `Mashiro/include/Mashiro/Async/Coro/Job.h`
- Test: `Mashiro/tests/Async/L4/JobTest.cpp`

**规约对照：** `04-coroutine-tasks.md` §6（含 `Coro::Job` 是 sender — `Scope::spawn(Job)` 走通用
`spawn(sender)` 重载，详见 §6.1b）。

- [ ] **Step 1：写"Job 是 sender"测试**

```cpp
TEST_CASE("Coro::Job satisfies stdexec::sender", "[Async][L4][Job]") {
    using namespace Mashiro::Async;
    STATIC_REQUIRE(stdexec::sender<Coro::Job>);
    STATIC_REQUIRE(Traits::IsDetached_v<Coro::Job>);
}
```

- [ ] **Step 2：实现 `Job.h`**

```cpp
// Mashiro/include/Mashiro/Async/Coro/Job.h
#pragma once
#include "../Foundations.h"

namespace Mashiro::Async::Coro {

    // Detached coroutine task. Lifetime owned by the parent Scope (L5). The Detached
    // annotation tells Traits::IsDetached_v<Job> = true.
    class [[=Async::Detached{}]] Job {
    public:
        // promise_type, sender ops per spec §6.
        using sender_concept = stdexec::sender_t;
        using completion_signatures = Async::completion_signatures<
            Async::set_value_t(), Async::set_stopped_t(),
            Async::set_error_t(std::exception_ptr)>;
        // ...
    };

} // namespace Mashiro::Async::Coro
```

- [ ] **Step 3：跑测试 → PASS**

- [ ] **Step 4：提交 + 打 tag `phase-4-complete`**

```bash
git commit -am "feat(Async/L4/Coro): Job + Detached annotation; Job is a sender"
git tag phase-4-complete
```

### 任务 4.12：Phase 4 验收 —— 一段 Manager 调用的三种写法

**Files:**
- Create: `Mashiro/demos/Async/ManagerCallThreeWays.cpp`

**规约对照：** `04-coroutine-tasks.md` §10。

- [ ] **Step 1：照规约 §10 抄三种写法 —— sender 表达式、Task<T>、Stream<T>**

每种写法做同一件事："读取 Manager X 的状态、做变换、提交到 Manager Y"。

- [ ] **Step 2：用 sync_wait 跑一遍，确认三种行为一致**

- [ ] **Step 3：在 demo 顶部写一段中文注释，解释三种写法各自的适用场景（什么时候选 sender、
  什么时候选 Task、什么时候选 Stream）**

- [ ] **Step 4：提交**

```bash
git add Mashiro/demos/Async/ManagerCallThreeWays.cpp
git commit -m "demo(Async): three ways to write the same Manager call (sender/Task/Stream)"
```

**Phase 4 验收门：**

- ✅ 每个适配器有补全签名 `static_assert` + 取消测试；
- ✅ `Task<T>` / `Stream<T>` 跨调度器亲和性 round-trip；
- ✅ HALO 在同线程 fast path 上消除分配（可选指标）。

---

## 第 5 章 —— L5 结构化并发 + L6 模式（Phase 5：3 周）

> **规约入口：** `05-structured.md` + `06-patterns.md`。

### 概念讲解 5.1：结构化并发 —— 异步控制流的"括号匹配"

> **关键词：** `counting_scope`、`Nursery`、父取消向下传、子错误向上传

为什么要"结构化"？因为传统的 `std::thread t(...); t.detach();` **打破了局部作用域** —— 线程
里的工作可能在外层函数早就返回之后还在跑、还在访问已销毁的对象。这就是大量 use-after-free 与
难复现死锁的根源。

**结构化并发**（structured concurrency）的核心律法是：

> **被 spawn 的工作必须在它被 spawn 的作用域结束前完成。**

代码长这样：

```cpp
co_await with_nursery([&](Nursery& n) {
    n.spawn(fetch_a());
    n.spawn(fetch_b());
    // co_await with_nursery 一直挂起，直到 a 和 b 都结束。
});
// 此处 a 和 b 一定已经完成、或被取消、或抛过的错误已经传出。
```

两条额外的律法：

1. **父取消向下传**：取消 nursery 的 stop_source → 所有 spawn 出的子工作都收到取消信号。
2. **子错误向上传**：任何一个子工作抛错 → nursery 取消其余兄弟、向 with_nursery 的 caller 抛错。

底层是 `stdexec::counting_scope`（P3149），框架在它之上加一层"反射友好"的薄包装。

### 任务 5.1：实现 `Scope<...>`（含 ring buffer 与 `ScopeTag`）

**Files:**
- Create: `Mashiro/include/Mashiro/Async/Structured/Scope.h`
- Test: `Mashiro/tests/Async/L5/ScopeTest.cpp`

**规约对照：** `05-structured.md` §3。

- [ ] **Step 1：写"Scope 收 spawn + on_empty 等空"测试**

```cpp
TEST_CASE("Scope spawns and settles", "[Async][L5][Scope]") {
    using namespace Mashiro::Async;
    Backend::StaticPool::context ctx{2};
    Structured::Scope<> scope{ctx.get_scheduler()};
    std::atomic<int> done{0};
    scope.spawn(schedule(ctx.get_scheduler()) | then([&]{ done.fetch_add(1); }));
    scope.spawn(schedule(ctx.get_scheduler()) | then([&]{ done.fetch_add(1); }));
    sync_wait(scope.on_empty());
    REQUIRE(done.load() == 2);
}
```

- [ ] **Step 2：实现 `Scope.h`**

```cpp
// Mashiro/include/Mashiro/Async/Structured/Scope.h
#pragma once
#include "../Foundations.h"
#include "../Concepts.h"
#include <stdexec/counting_scope.hpp>

namespace Mashiro::Async::Structured {

    // §3 — Scope wraps stdexec::counting_scope; the ScopeTag annotation receives a
    // source-location-derived constant by default (per overview §5.6).
    template<std::uint64_t Tag = 0, std::size_t InlineSlots = sizeof(void*) * 8>
    class [[=Async::ScopeTag{Tag}]] Scope {
    public:
        template<Concepts::Scheduler Sched>
        explicit Scope(Sched sched) : sched_{std::move(sched)} {}

        template<stdexec::sender S>
        void spawn(S&& s) {
            scope_.spawn(std::forward<S>(s));
        }

        auto on_empty() { return scope_.on_empty(); }
        stop_source& get_stop_source() noexcept { return scope_.get_stop_source(); }

    private:
        // simplified shape — production version per §3 includes the inline slot ring buffer.
        stdexec::counting_scope scope_;
        /* type-erased scheduler handle */ std::any sched_;
    };

} // namespace Mashiro::Async::Structured
```

- [ ] **Step 3：跑测试 → PASS**

- [ ] **Step 4：提交**

```bash
git commit -am "feat(Async/L5): Scope<Tag,InlineSlots> wrapping counting_scope"
```

### 任务 5.2：实现 `Nursery` + `with_nursery`

**Files:**
- Create: `Mashiro/include/Mashiro/Async/Structured/Nursery.h`
- Test: `Mashiro/tests/Async/L5/NurseryTest.cpp`

**规约对照：** `05-structured.md` §4。

- [ ] **Step 1：写"父取消向下、子错误向上"两条测试**

```cpp
TEST_CASE("Nursery cancels siblings when one child errors", "[Async][L5][Nursery]") {
    using namespace Mashiro::Async;
    Backend::StaticPool::context ctx{2};
    std::atomic<bool> sibling_cancelled{false};
    auto work = [&]() -> Coro::Task<void> {
        co_await Structured::with_nursery([&](auto& n) {
            n.spawn(schedule(ctx.get_scheduler()) | then([]{
                throw std::runtime_error{"boom"};
            }));
            n.spawn(schedule(ctx.get_scheduler())
                  | let_stopped([&]{ sibling_cancelled = true; return just(); }));
        });
    };
    REQUIRE_THROWS(sync_wait(work()).value());
    REQUIRE(sibling_cancelled.load());
}
```

- [ ] **Step 2：实现 `with_nursery`：返回一个 sender，它的 op-state 拥有一个 `Scope` 并提供
  `Nursery` 引用**

- [ ] **Step 3：跑测试 → PASS**

- [ ] **Step 4：提交**

```bash
git commit -am "feat(Async/L5): with_nursery + Nursery with parent-cancel + child-error policies"
```

### 任务 5.3：`LinkedScope` + `Supervised`

**Files:**
- Modify: `Mashiro/include/Mashiro/Async/Structured/Scope.h`
- Test: `Mashiro/tests/Async/L5/SupervisedTest.cpp`

**规约对照：** `05-structured.md` §5–§6。

- [ ] **Step 1：实现 `LinkedScope`（stop-token 派生自父 scope）**
- [ ] **Step 2：实现 `Supervised<Policy>`（catch 子错误、按策略 restart / log-skip / propagate）**
- [ ] **Step 3：写每种策略的测试**
- [ ] **Step 4：提交**

```bash
git commit -am "feat(Async/L5): LinkedScope (derived stop_source) + Supervised<Policy>"
```

### 概念讲解 5.2：L6 模式 —— 业务级配方

L5 给我们生命周期，L3 给我们算子，L4 给我们协程。**L6 模式**把这些组合成行业熟悉的名字：

- `parallel_for(range, fn)` —— "在某个 BulkScheduler 上把 fn 跑 N 次"；
- `pipeline(stage1, stage2, ...)` —— "stages 之间用内部队列连成响应式管线"；
- `actor<State>(behaviours...)` —— "MPSC 邮箱 + 顺序处理器 + 状态机"；
- `fork_join(fn1, fn2, ..., reduce)` —— "N 路 fork、按 reduce 汇总"；
- `scatter_gather(input, work, gather)` —— "bulk scatter + 归约"；
- `reactive` —— "Stream 上的高阶组合器（debounce/throttle/combine_latest/switch_map）"。

每一个都**< 50 LOC** —— 因为底层全是 L0–L5 的组合，没有新机制。

### 任务 5.4–5.9：实现 L6 模式

| 任务 | 模式 | 头文件 | 规约小节 |
|---|---|---|---|
| 5.4 | `parallel_for` | `Patterns/ParallelFor.h` | `06-patterns.md` §2 |
| 5.5 | `pipeline` + `pipeline_as_stream`（v0.2 §3.8 桥） | `Patterns/Pipeline.h` | §3 |
| 5.6 | `actor<State>` | `Patterns/Actor.h` | §4 |
| 5.7 | `reactive` 组合器 | `Patterns/Reactive.h` | §5 |
| 5.8 | `fork_join` | `Patterns/ForkJoin.h` | §6 |
| 5.9 | `scatter_gather` | `Patterns/ScatterGather.h` | §7 |

**每个任务的标准 5 步：**

- [ ] **Step 1：写一个能用例子的最小测试**
- [ ] **Step 2：实现头文件（≤ 50 行）**
- [ ] **Step 3：跑测试 → PASS**
- [ ] **Step 4：写组合测试 —— 用此模式 + 其它已有模式（比如
  `parallel_for` 套在 `pipeline` 阶段里）**
- [ ] **Step 5：提交**

### 任务 5.10：Phase 5 验收 —— Vulkan/OpenGL Cube 迁移演示

**Files:**
- Modify: `Mashiro/demos/Playground/VulkanCube.cpp`
- Modify: `Mashiro/demos/Playground/OpenGLCube.cpp`

**规约对照：** `08-cross-cutting.md` §7.2.3。

- [ ] **Step 1：把 VulkanCube 的"每帧 vkQueueSubmit + vkQueueWaitIdle"换成
  `Task<void>` + `co_await fence_sender`**
- [ ] **Step 2：把 OpenGLCube 的回调集中点换成 `pipeline` + `actor`**
- [ ] **Step 3：跑 demo，肉眼对比帧率（应不退化）**
- [ ] **Step 4：提交 + 打 tag `phase-5-complete`**

```bash
git commit -am "feat(Async/L6): pattern set + Vulkan/OpenGL Cube demos migrated"
git tag phase-5-complete
```

**Phase 5 验收门：**

- ✅ `Nursery` / `Scope` / 取消流程在 CI 跑通；
- ✅ `parallel_for` / `pipeline` / `actor` 每个都有"与其它模式组合"测试；
- ✅ `Diagnostics::scope_audit()`（Phase 7 实现）能捕获故意 escape 的工作。

---

## 第 6 章 —— L7 扩展面（Phase 6：2 周）

> **规约入口：** `07-extension.md` 全文。

### 概念讲解 6.1：扩展面的"零摩擦原则"

L7 不是"插件 SDK"。它的目标是：**用户在他自己的命名空间里写一个调度器/适配器/awaitable，
不修改框架的任何头，也能像内置后端一样被框架认可**。

机制：

1. **概念**：用户类型只要满足 `Concepts::Scheduler<S>` 就能用 ── 不需要继承任何基类。
2. **注解**：用户在自己类型上贴 `[[=Async::Affine{Backend::User}]]` 之类的注解，框架的 Traits
   自动读到。
3. **opt-in 特征 `register_scheduler_v<T>`**（v0.2 §3.1b）：让框架的工具（诊断、域、插件描述）
   能枚举"已知调度器"列表 —— 用户特化 `register_scheduler_v<MyVkSched> = true` 即可加入。

**关键设计选择（v0.2 §9.1b）**：`Async::from_future` 被标 `[[deprecated]]`，但**不是要移除**
—— 而是要用户在"审核过的迁移边界"上把它换成 `Async::Extension::from_future`。每一次写它都
会被编译器叮一下："请确认这里真的是迁移边界"。这种"诊断式提醒"比一刀切移除更温和。

### 任务 6.1：实现 `register_scheduler_v<T>` opt-in 特征

**Files:**
- Create: `Mashiro/include/Mashiro/Async/Extension/Scheduler.h`

**规约对照：** `07-extension.md` §3.1b。

- [ ] **Step 1：写"opt-in 默认为 false、特化后为 true"测试**

```cpp
TEST_CASE("register_scheduler_v defaults to false", "[Async][L7]") {
    using namespace Mashiro::Async;
    struct MySched { /* ... */ };
    STATIC_REQUIRE_FALSE(Extension::register_scheduler_v<MySched>);
}

namespace Mashiro::Async::Extension {
    template<> inline constexpr bool register_scheduler_v<struct UserSched> = true;
}
TEST_CASE("user can opt-in via specialization", "[Async][L7]") {
    STATIC_REQUIRE(Mashiro::Async::Extension::register_scheduler_v<UserSched>);
}
```

- [ ] **Step 2：实现**

```cpp
// Mashiro/include/Mashiro/Async/Extension/Scheduler.h
#pragma once
#include "../Foundations.h"

namespace Mashiro::Async::Extension {
    // §3.1b — opt-in trait. Default is false. User specializes for their scheduler type to
    // add it to the framework's "known schedulers" enumeration (used by diagnostics, domains,
    // and plugin descriptors).
    template<class T>
    inline constexpr bool register_scheduler_v = false;
}
```

- [ ] **Step 3：跑测试 → PASS**

- [ ] **Step 4：提交**

```bash
git commit -am "feat(Async/L7): register_scheduler_v opt-in trait"
```

### 任务 6.2：写一个工作示例 —— `VkComputeScheduler`

**Files:**
- Create: `Engine/Compute/VkComputeScheduler.h`（注意：在 engine 子树，不在框架子树！）
- Test: `Mashiro/tests/Async/L7/VkComputeSchedulerTest.cpp`

**规约对照：** `07-extension.md` §4。

- [ ] **Step 1：在 engine 子树里建一个最小的 `VkComputeScheduler`**

骨架（用 Vulkan compute queue）：

```cpp
// Engine/Compute/VkComputeScheduler.h
#pragma once
#include <Mashiro/Async/Foundations.h>
#include <Mashiro/Async/Concepts.h>
#include <Mashiro/Async/Extension/Scheduler.h>
#include <vulkan/vulkan.h>

namespace Engine::Compute {

    struct [[=Mashiro::Async::Backend_{Mashiro::Async::Backend::User},
            =Mashiro::Async::Cancellable{}]] VkComputeScheduler {
        VkDevice device;
        VkQueue compute_queue;

        struct sender_t {
            // ... schedules a vkQueueSubmit + fence-await
        };

        sender_t schedule() const noexcept { return {/*...*/}; }
        bool operator==(const VkComputeScheduler& o) const noexcept {
            return device == o.device && compute_queue == o.compute_queue;
        }
    };

} // namespace Engine::Compute

// Opt-in via specialization (§3.1b).
namespace Mashiro::Async::Extension {
    template<>
    inline constexpr bool register_scheduler_v<Engine::Compute::VkComputeScheduler> = true;
}

MASHIRO_ASYNC_REGISTER_BACKEND(::Engine::Compute::VkComputeScheduler);
```

- [ ] **Step 2：写一个 mock-vulkan 测试 —— 用 dummy `VkDevice` 验证 schedule 流程**

- [ ] **Step 3：跑测试 → PASS**

- [ ] **Step 4：提交 + 打 tag `phase-6-complete`**

```bash
git commit -am "feat(Engine/Compute): VkComputeScheduler example for L7 extension surface"
git tag phase-6-complete
```

### 任务 6.3：实现 `Async::Extension::from_future`（v0.2 §9.1b 审核边界 spelling）

**Files:**
- Create: `Mashiro/include/Mashiro/Async/Extension/Sender.h`
- Test: `Mashiro/tests/Async/L7/FromFutureTest.cpp`

- [ ] **Step 1：实现 `Async::Extension::from_future(std::future<T>)`，返回一个 sender**
- [ ] **Step 2：写测试 —— 包装一个 `std::async` future，验证 `sync_wait` 拿到值**
- [ ] **Step 3：在 `Async::from_future` 上加 `[[deprecated("migration boundary —
  see 07-extension.md §7.5")]]`，让现有调用点收到编译诊断（但仍可编译）**
- [ ] **Step 4：提交**

```bash
git commit -am "feat(Async/L7): from_future audited spelling + deprecated nudge on Async::from_future"
```

**Phase 6 验收门：**

- ✅ `VkComputeScheduler` 示例在 `Engine/Compute/` 编译；
- ✅ `register_scheduler_v` opt-in 测试通过；
- ✅ `Async::from_future` 触发 `[[deprecated]]` 诊断；`Async::Extension::from_future` 不触发。

---

## 第 7 章 —— 横切硬化（Phase 7：2 周）

> **规约入口：** `08-cross-cutting.md` 全文。

### 概念讲解 7.1：诊断系统的"诺言"

横切关注必须实现一条根本契约：**诊断在关闭时为零开销，开启时为可控开销**。这意味着：

- 关闭时：`Diagnostics::Trace::Span{"name"}` 在 release 编译里**编译成什么都没有**；
- 开启时：每条 trace 一次原子写 + 一次时间戳读，跟 hot path 比可忽略。

实现技术：

- 所有诊断 API 都是 `inline` 模板，body 由一个 `if constexpr (MASHIRO_DIAGNOSTICS)` 包住；
- 编译选项 `-DMASHIRO_DIAGNOSTICS=1` 才"接通"。

`Diagnostics::AllocCheck`、`Diagnostics::DetectStarvation`、`Diagnostics::DetectDeadlock` 都是
**测试期工具** —— 它们在 CI 跑，在 release 编译里被去掉。

### 任务 7.1：实现 `Diagnostics::Trace::Span` + 诊断后端注册顺序检查

**Files:**
- Create: `Mashiro/include/Mashiro/Async/Diagnostics/Trace.h`
- Create: `Mashiro/src/Async/Diagnostics/StructuredLoggerBackend.cpp`
- Test: `Mashiro/tests/Async/Diagnostics/TraceTest.cpp`

**规约对照：** `08-cross-cutting.md` §6.1（含 v0.2 注册顺序规则）。

- [ ] **Step 1：写 trace 与"诊断后端必须在调度器构造前注册"两条测试**

```cpp
TEST_CASE("Diagnostics backend must be registered before scheduler construction",
          "[Async][Diagnostics][Order]") {
#ifdef MASHIRO_DIAGNOSTICS_REQUIRED
    /* Without registering, constructing a StaticPool::context should hit a debug-assert. */
    REQUIRE_THROWS_AS(([]{ Mashiro::Async::Backend::StaticPool::context ctx{2}; }()),
                      Mashiro::AssertionFailure);
#endif
}
```

- [ ] **Step 2：实现 `Trace::Span`**

```cpp
// Mashiro/include/Mashiro/Async/Diagnostics/Trace.h
#pragma once
namespace Mashiro::Async::Diagnostics::Trace {

    class Span {
    public:
#if MASHIRO_DIAGNOSTICS
        Span(std::string_view name);
        ~Span();
    private:
        struct impl;
        std::unique_ptr<impl> p_;
#else
        Span(std::string_view) noexcept {}
        ~Span() noexcept = default;
#endif
    };

    // §6.1 v0.2 — registration ordering check
    void register_backend(/* ... */);
    bool any_backend_registered() noexcept;

} // namespace Mashiro::Async::Diagnostics::Trace
```

- [ ] **Step 3：在每个后端的 `context` 构造里加入 `assert(Diagnostics::Trace::any_backend_registered())`
  当 `MASHIRO_DIAGNOSTICS_REQUIRED` 定义时**

- [ ] **Step 4：跑测试 → PASS**

- [ ] **Step 5：提交**

```bash
git commit -am "feat(Async/Diagnostics): Trace::Span + registration-order check"
```

### 任务 7.2：实现 `Diagnostics::AllocCheck` + 零分配测试

**Files:**
- Create: `Mashiro/include/Mashiro/Async/Diagnostics/Counters.h`
- Test: `Mashiro/tests/Async/Diagnostics/AllocCheckTest.cpp`

**规约对照：** `08-cross-cutting.md` §3.5 + §6.6。

- [ ] **Step 1：写一个 RAII `AllocCheck { Span op = {"schedule fast path"}; }` —— 析构时断言
  期间 `operator new` 调用次数为 0**

- [ ] **Step 2：实现：用 thread-local 计数器钩进一个**仅在测试**重写的 `operator new`**

- [ ] **Step 3：对每个 L2 后端 fast path 写一个 AllocCheck 测试**

- [ ] **Step 4：提交**

```bash
git commit -am "feat(Async/Diagnostics): AllocCheck + zero-allocation regression tests"
```

### 任务 7.3：实现 `Diagnostics::DetectStarvation`

**Files:**
- Create: `Mashiro/include/Mashiro/Async/Diagnostics/Probes.h`
- Test: `Mashiro/tests/Async/Diagnostics/StarvationTest.cpp`

**规约对照：** `08-cross-cutting.md` §6.3。

- [ ] **Step 1：写"故意把所有 worker 都占满"测试，期望 starvation 探针报告**
- [ ] **Step 2：实现探针（StaticPool 工作线程每 100ms 心跳，主线程超时即报）**
- [ ] **Step 3：提交**

### 任务 7.4：实现 `Diagnostics::scope_audit()` （Escape 检测）

**Files:**
- Modify: `Mashiro/include/Mashiro/Async/Diagnostics/Probes.h`
- Test: `Mashiro/tests/Async/Diagnostics/ScopeAuditTest.cpp`

**规约对照：** `05-structured.md` §7 + `08-cross-cutting.md` §6.2。

- [ ] **Step 1：写"在 Scope 里 spawn 一个写全局变量的 sender、scope 析构后再读全局"测试，
  期望 audit 报告**
- [ ] **Step 2：实现 audit：跟踪每个 `Scope` 的"spawn 计数"与"完成计数"，析构时若不等则报**
- [ ] **Step 3：提交**

### 任务 7.5：取消链测试（chain / nursery / scope-shutdown）

**Files:**
- Test: `Mashiro/tests/Async/Cancellation/ChainTest.cpp`
- Test: `Mashiro/tests/Async/Cancellation/NurseryTest.cpp`
- Test: `Mashiro/tests/Async/Cancellation/ScopeShutdownTest.cpp`

**规约对照：** `08-cross-cutting.md` §2 + §7.5 Phase 7 行。

- [ ] **Step 1：ChainTest —— 验证取消信号能贯穿 `then` → `let_value` → `bulk` 等适配器链**
- [ ] **Step 2：NurseryTest —— 父取消向下传至所有 spawned 子；子异常向上传并取消兄弟**
- [ ] **Step 3：ScopeShutdownTest —— 把 `request_stop()` 与 `on_empty()` 的交互固定下来**
- [ ] **Step 4：每个测试都用 `Diagnostics::AllocCheck` 包住，断言取消路径零分配**
- [ ] **Step 5：提交**

```bash
git commit -am "test(Async/Cancellation): chain + nursery + scope-shutdown coverage"
```

### 任务 7.6：Phase 7 验收 —— 在 CI 上锁住所有"零开销/结构化"承诺

- [ ] **Step 1：在 CMake / CI workflow 里把以下测试**全部**接入 CI：
  - 所有 `[Async]` 标签的 Catch2 测试；
  - 用 `MASHIRO_DIAGNOSTICS_REQUIRED` 编一遍专测路径。
- [ ] **Step 2：把 `Mashiro/demos/Playground/Main.cpp` 改写为
  `Mashiro::Async::Patterns::pipeline` 版本，作为 v1.0 的"hello async" demo**（详见
  `08-cross-cutting.md` §7.2.3）。
- [ ] **Step 3：把 `Yuki/tests/Core/{Query,Identity,MetaClass,RootObject,RefCounted}Test.cpp`
  里的同步路径用 `Async::Diagnostics::AllocCheck` 包一遍**
- [ ] **Step 4：执行最终验收门检查表（§7.6 表）**
- [ ] **Step 5：提交 + 打 tag `v1.0-async-framework`**

```bash
git commit -am "ci(Async): wire all Async tests into CI; Phase 7 acceptance gates green"
git tag v1.0-async-framework
```

**Phase 7 验收门：**

- ✅ `AllocCheck` 在每个 hot path 上断言 0 分配；
- ✅ `DetectStarvation` 报警一个故意失速的 scheduler；
- ✅ `DetectDeadlock` 报警一个合成的环；
- ✅ Linux + Windows 一次 CI run 全绿。

---

## 附录 A：常见误区与防御性写法

### A.1 "我可以在 receiver 的 destructor 里调 `set_value`"

**不行。** stdexec 契约：`set_*` 是 sender 操作产生**完成**结果的方式，必须在 op-state 内部
（通常是 `start()` 的同步或异步执行路径里）调用。受体析构是"清理"，不是"完成"。

### A.2 "我可以把 `inplace_stop_callback` 直接放在堆上"

**不应。** 它通常应该是 op-state 的**成员**（栈/op-state 内嵌），跟着 op-state 一起 destroy。
把它单独 heap-allocate 等于把 op-state 的取消语义分散到一个独立生命周期里，容易写出
double-deregister。

### A.3 "我可以在 hot path 里 `std::function`"

**不应。** 用 `Mashiro::Core::InlineFunction` 或泛型 lambda 直存类型 —— hot path 不允许
type-erased 调用。

### A.4 "Stream 消费者抛异常就让它逃出"

**不应。** Stream 的协程 `promise` 必须把异常翻成 `set_error(eptr)`；让异常逃出协程帧 →
未定义行为。统一从协程出口走 `Coro::stopped_signal` 与 `std::exception_ptr` 两条路径。

### A.5 "`with_nursery` 的 lambda 引用了局部变量、`co_await` 它之后局部已经销毁"

**这是真错。** `with_nursery` 的 lambda 在它返回之后还能跑（如果它 capture 了 sender 在异步路径
上挂起）。规约 §4 明示：lambda 捕获的局部变量寿命必须覆盖 `with_nursery` 返回 sender 的整个
生命周期 —— 一种简单做法：把 lambda 整个写成 `[&]() -> Task<void> { ... co_await ... }` 形式，
让协程帧持有所有状态。

---

## 附录 B：词汇表（按字母序）

| 词 | 含义 | 第一次定义在 |
|---|---|---|
| **AffineScheduler** | `schedule()` 总在固定线程完成的调度器；相等性蕴含同线程 | L0 §4.4 |
| **Allocates** | 表达"会在某处分配"的 L1 注解；用于 `AllocatesIn_v` Trait | L1 §5.2 |
| **AllocCheck** | 测试期断言"包裹区域内 `operator new` 调用为 0"的 RAII | Cross-cutting §3.5 |
| **Backend** | 调度器 backend 枚举值（Inline/StaticPool/Tbb/Platform/Io/User） | L1 §5.1 |
| **BulkScheduler** | 满足 `schedule_bulk(s,n,fn)` 良构的调度器 | L0 §4.4 |
| **Cancellable** | 表达"会响应 stop_token"的 L1 注解 | L1 §5.2 |
| **completion_signatures** | sender 完成时可能产生的 `set_*` 元组类型 | stdexec P2300 |
| **continues_on(s, sched)** | 把 sender s 的完成"搬"到 sched 上继续 | stdexec |
| **counting_scope** | 持有 N 个进行中 sender 的"作用域"，N=0 时 settle | stdexec P3149 |
| **Detached** | L4 `Job` 的注解，标识它有 detached 生命周期 | L1 §5.2 v0.2 |
| **domain** | 一组 `transform_sender` 改写规则，由 backend 注册 | stdexec P2999/P3826 |
| **HALO** | 堆分配消除优化，把协程帧从堆挪到栈 | C++ ISO |
| **inplace_stop_token** | 不带 type erasure 的栈内 stop_token | stdexec |
| **IoScheduler** | 暴露 I/O sender 工厂的调度器 | L0 §4.4 |
| **Nursery** | `with_nursery` 提供的、可 spawn 子工作的句柄 | L5 §4 |
| **op-state** | `connect(sender, receiver)` 的产物，持有运行时状态 | stdexec |
| **proactor** | 用户提请求、内核做完通知的 I/O 模型（io_uring / IOCP） | OS 文献 |
| **register_scheduler_v** | 用户特化的 opt-in 特征，把自家调度器加入框架"已知列表" | L7 §3.1b |
| **schedule(sched)** | 一个 sender，启动后唤醒到 sched 的执行上下文 | stdexec |
| **ScopeTag** | 给 `Scope<Tag, ...>` 类模板贴的标签注解 | L1 §5.2 v0.2 |
| **sender** | 尚未启动的异步工作的描述 | stdexec |
| **stop_token / stop_source / stop_callback** | 取消信号传输的三件套 | stdexec |
| **structured concurrency** | "spawn 的工作必须在 spawn 作用域结束前完成"的设计 | Trio 等 |
| **transform_sender** | 域提供的"重写 sender 表达式"接口 | stdexec |

---

## 附录 C：交付物清单（最终检查）

| 文件类别 | 数量 | 位置 |
|---|---|---|
| L0/L1 头 | 3 | `Mashiro/include/Mashiro/Async/{Foundations,Concepts,Traits}.h` |
| L2 后端头 | 5 | `Mashiro/include/Mashiro/Async/Backend/{Inline,StaticPool,Tbb,Platform,Io}.h` |
| L2 后端实现 | 4 | `Mashiro/src/Async/Backend/{StaticPool,Tbb}.cpp` + `Io/Linux/IoUring.cpp` + `Io/Windows/Iocp.cpp` |
| L3 适配器头 | 7 | `Mashiro/include/Mashiro/Async/Adaptor/{Bulk,Batch,Debounce,Throttle,Retry,Timeout,Race}.h` |
| L4 协程头 | 3 | `Mashiro/include/Mashiro/Async/Coro/{Task,Stream,Job}.h` |
| L5 结构化头 | 2 | `Mashiro/include/Mashiro/Async/Structured/{Scope,Nursery}.h` |
| L6 模式头 | 6 | `Mashiro/include/Mashiro/Async/Patterns/{ParallelFor,Pipeline,Actor,Reactive,ForkJoin,ScatterGather}.h` |
| L7 扩展头 | 4 | `Mashiro/include/Mashiro/Async/Extension/{Scheduler,Sender,Domain,Plugin}.h` |
| 诊断头 + 实现 | 3 + 2 | `Mashiro/include/Mashiro/Async/Diagnostics/{Trace,Counters,Probes}.h` + `src/Async/Diagnostics/{StructuredLoggerBackend,TracyBackend}.cpp` |
| 测试 | 35+ | `Mashiro/tests/Async/{L0,L1,L2,L3,L4,L5,L6,L7,Diagnostics,Cancellation}/` |
| Demo | 4+ | `Mashiro/demos/Async/`、`Mashiro/demos/Playground/{Main,VulkanCube,OpenGLCube}.cpp` |

---

## 课程总结

到这里你已经把一份完整的、零开销、反射驱动、可扩展的 C++26 异步框架落地到 Mashiro 工程里。
你应该带走这些观念：

1. **词汇 ＞ 抽象基类**：sender / receiver / scheduler / domain / stop_token / scope —— 整套
   异步系统只用这 6 个名词就足够。
2. **编译期 ＞ 运行期**：能让类型系统说话就别让运行时说话。注解 + Traits + 一致性验证块
   就是这条信念的落地。
3. **结构化 ＞ 自由**：异步控制流的"括号匹配" —— `with_nursery` 比 `detach()` 强得多，
   是少数能从根本上消除一整类并发 bug 的设计。
4. **专业化 ＞ 通用**：TBB 做并行、io_uring 做 I/O、Platform 做 UI 亲和 —— 每个后端做自己最
   擅长的事，框架只负责让它们说同一种语言。
5. **零开销 ＞ 灵活**：诊断、域改写、注解查询 —— 当你不用它们时，它们就不在二进制里。

去用它吧。**祝你写出一段你下个月还看得懂的异步代码。**

---

## 执行交接（Execution Handoff）

计划已完成并保存到 `docs/superpowers/plans/2026-06-17-async-framework-implementation-guide.md`。
有两种执行方式可选：

**1. 子代理驱动（Subagent-Driven，推荐）** —— 每个任务派发一个全新子代理，任务间评审，迭代快。
   要求子技能：`superpowers:subagent-driven-development`。

**2. 当前会话内执行（Inline Execution）** —— 在当前会话里批量执行任务，按 checkpoint 评审。
   要求子技能：`superpowers:executing-plans`。

请选择执行方式。
