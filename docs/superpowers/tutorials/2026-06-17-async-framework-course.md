# Mashiro 异步框架 · 课程讲义

**课程编号：** ASYNC-2026
**学期：** 2026 Spring (Tutorial Series)
**编写日期：** 2026-06-17
**讲义版本：** v0.3（+ Unit V 通用原语扩展 / L5+L7+L8 三处主线增补 / Lab 5/7/8/11/12 v0.3 变体 / Unit III Cache + 3 条新陷阱 / Unit IV F6 Beacon Schema 升级路径）
**依据规范：** `docs/superpowers/specs/2026-06-15-async-framework/00-overview.md`–`09-synthesis.md`（共九份 v0.2）
**目标读者：** 已学过 C++20（含 concept / 协程基本语法），对线程、互斥、`std::async` 等同步 / 异步原语有直觉；尚未系统接触 P2300 sender/receiver 模型。
**先修：** 一学期的 C++20、半学期的并发编程（互斥、条件变量、`std::thread`/`std::jthread`）。
**不要求：** P2300 经验、P2996 反射经验、Vulkan 经验。

---

## 给学生的话（请务必先读完）

这门课的目标，是带你**亲手在键盘上敲出一条主流程，然后一层一层把它丰满起来**，直到它具备 spec 中描绘的完整能力。我们不会按层级（L0、L1、L2 …… L7）顺序铺叙；相反，每一节课都会把一条**可以编译、可以运行、可以观察**的细流握在手里，让你看着它从一根光秃秃的"打印 42"长成一棵带有调度、取消、流、作用域、模式与扩展点的完整骨架。

这是软件工程教学中常被称为 **"vertical slicing"（垂直切片法）** 或 **"walking skeleton"（行走骨架）** 的训练范式。它与 spec 的横向分层并不冲突——分层是给规范设计者读的，**切片是给学习者走的**：

| 视角 | 方向 | 适用 |
|------|------|------|
| 横向分层 (spec)   | L0 → L1 → L2 → ……→ L7 | 让规范作者保证一致性、让代码评审能确认契约 |
| 纵向切片 (本课程) | 一条主流程 → 不断加厚 | 让学习者把抽象立刻和"看得见的程序"绑定 |

**学习节律。** 每讲分三段：

1. **§a 主流程更新**：给出本讲应当落到键盘上的最小代码增量。这是你必须自己敲、自己编译、自己运行的部分。
2. **§b 概念剖析**：从这次增量里抽出框架里的"名字"——sender、receiver、scheduler、op-state、completion signature 等等。讲名字、讲约束、讲它在 spec 中的位置。
3. **§c 思考题**：开放式问题，鼓励你回去查 spec、查源码、查 P2300 提案，再回到代码里验证。

每 2–3 讲之间，会插入一段 **Interlude**：这是为了在工程节律外，给你一点点喘息——退后一步看看分类学、范畴论、哲学动机。它们不是装饰，而是**让你看清自己手里这条切片到底属于一个怎样的图景**。

**关于源代码。** 本讲义中的代码片段以教学为目的；它们**接近** spec 中规定的形态，但偶尔会为了讲解清晰而简化命名空间或省略 `noexcept`。每节课的尾部会标注"对应 spec 章节"，方便你回去比对完整契约。

**关于工具链。** 我们假设你的开发环境是：

- 编译器：`clang-p2996`（COCA 工具链）
- 标准库：libc++（COCA sysroot 提供）
- stdexec：`thirdparty/stdexec`（已 vendored 进 codebase）
- 反射：P2996 + P3394 注解
- 构建：执行 `setup.py` 后即可使用 `cmake` / `ninja`

如果你在另一套环境工作，把 `Async::just / then / sync_wait` 换成 `stdexec::just / then / sync_wait`，把项目内的 `Mashiro::Async::*` 换成等价 `stdexec::*`，绝大部分代码仍然可以照走。

**禁止参考。** 本课程**故意不参考** `docs/superpowers/plans/2026-06-17-async-framework-implementation-guide.md`。讲义的编排出自规范本身，叙述责任由我（任课讲师）承担。如果你在自习过程中读到那份 plan，请把它当作"另一种切法的实施计划"看待，不要让它干扰你按本课程的纵向切片建立直觉。

---

## 课程地图

本讲义分**四个单元**。Unit I 主线 12 讲，Unit II–IV 是配套扩展。**新手按 I → II → III → IV 的顺序走；老手可挑读 IV。**

```
═══ Unit I · 主流程（讲义本体） ════════════════════════════
导言 ─┐
      │
      ▼
   Lesson 1  最简 sender：just / then / sync_wait
   Lesson 2  Task<T>：把同步流程升格成协程
   Lesson 3  stop_token：让管线可以被取消
   ───────── Interlude I  异步原语的分类学
   Lesson 4  StaticPool：跨线程的第一次
   Lesson 5  Stream<T>：从一次结果到一串结果
   ───────── Interlude II  sender / receiver 的范畴论解读
   Lesson 6  Scope / Nursery：让生命周期闭合
   Lesson 7  timeout / retry / race：给管线加固
   ───────── Interlude III  结构化并发的哲学动机
   Lesson 8  parallel_for / pipeline：把控制流抽出来
   Lesson 9  domain：让 backend 改写表达式
   ───────── Interlude IV  注解、反射与可证明的分类
   Lesson 10 annotation + 反射验证器
   Lesson 11 VkComputeScheduler：从用户视角实现一个 scheduler
   Lesson 12 综合实验：把所有抽屉一起打开
   附录 A   词表
   附录 B   与 spec 章节的双向索引
   附录 C   阅读路线建议

═══ Unit II · Labs（动手实验配套） ═════════════════════════
   Lab 1–12 每讲对应一个动手 lab：任务、验收、提示、参考解骨架

═══ Unit III · 深度案例 / 性能 / 调试 / 陷阱 ═══════════════
   案例 1   游戏引擎主循环 ── 帧并行 + GPU pipeline
   案例 2   流式 RPC 服务端 ── nursery 守护双向流
   案例 3   流式 ETL 管道 ── throttle + flat_map + batch + 死信
   性能 B.1 HALO 实测（编译器 × 优化等级矩阵）
   性能 B.2 AllocCheck 用法 + 分配预算回归测试
   性能 B.3 CancelProbe 用法（取消传播故障注入）
   性能 B.4 stdexec 编译期 trace
   性能 B.5 集成 tracy / perfetto / Chrome trace
   性能 B.6 perf pitfall checklist
   陷阱 12 条： 从"我只是 co_await just(42)"到 review 漏洞

═══ Unit IV · 框架特性扩展（自顶而下设计） ════════════════
   F1  Typed Execution Environment      ── env 静态类型化
   F2  Capability-Typed Schedulers      ── concept refinement + 偏序
   F3  Effect Row Annotations            ── 副作用形式化、可组合
   F4  Reflection-Driven Adaptor Genesis ── 用反射自动生成 adaptor
   F5  Compile-Time Topological Scheduling ── DAG 拓扑编译期可查
   F6  Deterministic Replay              ── trace + replay 调试
   F7  Structured Resource Lifetimes     ── 异步 RAII 进 nursery
```

每完成一讲，问自己三个问题：

- 我能不能向同桌讲清楚"刚刚那段代码在分类学上落在哪一格"？
- 我能不能在不查 spec 的情况下说出本讲的 op-state 形状？
- 如果取消信号在此时此刻到来，我手里这条切片会怎样反应？

如果三个问题里有任何一个回答不出，**不要进入下一讲**。回到本讲的 §c 思考题，挑两道动手做。

---

## 记号约定

| 记号 | 含义 |
|------|------|
| `Mashiro::Async` 或 `Async::` | 框架自身的命名空间（L0 重导出 + L1 注解）。 |
| `stdexec::` | 上游 P2300 实现（vendored 于 `thirdparty/stdexec`）。 |
| `[[=Async::Cancellable]]` 等 | P3394 注解，附在类型上、由反射查询；运行时**不可见**。 |
| `S, U` | 模板参数中通常代表 sender 类型。 |
| `Recv` | receiver 类型。 |
| `Op` | op-state 类型。 |
| **🔧 §a** | 主流程代码增量。 |
| **📚 §b** | 概念剖析。 |
| **❓ §c** | 思考题。 |
| **🪞 Interlude** | 不写代码、只写思考的副线段落。 |

---

# Lesson 1 · 最简 sender：just / then / sync_wait

> **学习目标：** 写出本课程的"行走骨架"——一段能编译能运行的 `Async::just(42) | Async::then(...) | sync_wait`。从这条三件套出发，我们将认识三个最核心的名词：**sender**、**receiver**、**op-state**。
>
> **对应 spec 章节：** `00-overview.md` §2、§5.1；`01-foundations.md` §4.2 / §4.3 / §4.4（L0 重导出 + `Concepts::Scheduler`）。

## §a 主流程：第一根线

新建 `Mashiro/demos/AsyncCourse/01_hello_sender.cpp`：

```cpp
#include <Mashiro/Async/Foundations.h>   // L0 重导出，含 Async::just / then / sync_wait
#include <print>

namespace Async = Mashiro::Async;

int main() {
    auto pipeline = Async::just(42)
                  | Async::then([](int x) { return x * 2; })
                  | Async::then([](int x) { std::println("result = {}", x); return x; });

    auto [result] = stdexec::sync_wait(pipeline).value();   // 阻塞直到管线落地
    std::println("returned to main with {}", result);
}
```

预期输出：

```
result = 84
returned to main with 84
```

**请你做到三件事再往下读：**

1. 真正把这段代码编译、运行、看到上面两行。
2. 尝试改写：把 `then` 换成 `Async::let_value([](int x) { return Async::just(x + 1); })`，观察结果。
3. 试着把第一个 `then` 改成 `[](int x) -> int { throw std::runtime_error("boom"); }`，运行——你会看到 `sync_wait` 抛出异常。这一点在 §b 会被反复回到。

## §b 概念剖析：三个名词的精确含义

### sender —— "**未来的计算**"

`Async::just(42)` 不是 `42` 这个值，也不是"立刻计算 42 的函数对象"——它是一个**对象**，它**承诺**：当有人愿意听的时候（即把一个 receiver 接上去），它会用 `42` 完成一次 `set_value` 调用。这就是 sender。

正式地（`stdexec::sender_t`）：

> 一个 sender `S` 是一个对象，它具备类型集合 `completion_signatures_of_t<S, Env>`——这是一组**可能的完成方式**——并且它知道如何在被 `connect(s, recv)` 之后把这些完成方式注入到 receiver 中。

请记住三点：

- sender **不立即跑**。它是延迟的。`just(42)` 不会立刻打印任何东西。
- sender **携带类型**。完成方式（值、错误、停止）以类型形式编码在它的 `completion_signatures` 里。
- sender **可以组合**：`|` 不是黑魔法，它就是把上游 sender 喂给下游 adaptor 构造新的 sender。

### receiver —— "**回调的三联体**"

`sync_wait` 在内部为你造了一个 receiver。一个 receiver 是任何具备以下三种回调的对象：

- `set_value(rec, args...)` —— 表示"成功了，结果是 args"
- `set_error(rec, e)` —— 表示"出错了，错误是 e"
- `set_stopped(rec)` —— 表示"取消了，没有值也没有错误"

注意：**三种回调互斥**。一个 receiver 在其生命中只会被调用恰好一次三选一。这是 P2300 模型最重要的不变量之一。

你写在 `then(...)` 里的 lambda **不是 receiver**——它是被 `then` 包成 receiver 的、"对 `set_value` 通道的回应"。`set_error` / `set_stopped` 由 `then` 默认透传给下游。

### op-state —— "**栈上具现化的协程帧**"

当 sender 和 receiver 通过 `connect(s, recv)` 接到一起，会得到一个 **op-state**——它是这条管线在内存中的具体形态：

- 上游 sender 的字段（`just(42)` 里那个 `42`）
- receiver（连同它的环境）
- 必要的中间状态（计数器、原子位）

op-state 是一个**普通的 C++ 对象**，**通常分配在调用者的栈帧上**。`sync_wait` 也好、协程也好，都不为它做隐式堆分配（除非 spec 显式声明）。这是框架"零分配热路径"的物理基础——没有 `std::function`、没有 vtable、没有 `shared_ptr`。

一条思路：**op-state ≈ 把这条管线手写成的协程帧，只是它由模板生成、不需要协程语法**。我们到 Lesson 2 会看到，当你**真的**用协程语法 (`co_await`) 写它时，编译器会把 op-state 嵌进协程帧里——本质相同。

### 三件套的角色

回到代码：

```
Async::just(42)               // sender —— 承诺 set_value(42)
  | Async::then(f)            // sender adaptor —— 把上游的 set_value 转译成 f 的返回
  | Async::then(g)            // sender adaptor —— 同上
sync_wait(...)                // 在调用线程上构造一个 receiver、connect、start，并阻塞
```

`|` 是 stdexec 早就为你重载好的管道运算符；它把右边的"adaptor 闭包"应用到左边的 sender，返回一个新的 sender。三个 `|` 之后你得到的是**一个 sender 类型**（它的类型非常长，但你不必念出来）。

### 错误通道：为什么 `throw` 能跑通

你在思考题里试过 `throw std::runtime_error("boom")`。它不会让程序崩溃，而是被 `then` 捕获、转成 `set_error(exception_ptr)` 通道，沿 sender 链向下游传播，最终被 `sync_wait` 拿到——它把 `exception_ptr` 重新 `rethrow_exception`，所以 `sync_wait(...).value()` 那一行会抛。

这就是 `08-cross-cutting.md` §4 所说的"**默认错误类型是 `std::exception_ptr`**"。它不是任意选择——它让 sender 链的 `completion_signatures` 在组合时只增加值类型、不增加错误类型，避免组合爆炸。我们在 Lesson 7 会再回来讨论。

### 命名约定与重导出表

`Async::just`、`Async::then`、`Async::sync_wait` 都是 `stdexec::*` 的别名（`01-foundations.md` §4.3 完整列出了重导出表）。框架坚持的口径是 **"一种异步词汇，一种拼写"**——在 Mashiro 的代码里看到 `stdexec::` 通常意味着你在直接写底层，而看到 `Async::` 意味着你在用框架的姿势写。两者完全互通；这只是命名空间的卫生。

## §c 思考题

1. `Async::just(42) | Async::then([](int){})` 这个 sender 的 `completion_signatures` 集合里都有什么？如果你在第二个 `then` 里抛了异常，集合里多了哪一项？
2. 把代码改成 `Async::just()`（不带参数）。看看 `sync_wait` 的返回类型变成了什么——为什么 `.value()` 的形状变了？
3. 想象一下：如果允许 receiver 的三种回调**不是**互斥的，能调用多次，会带来什么问题？为什么 P2300 选择了互斥？
4. （动笔不动键盘）画一张图：sender 和 receiver 在 `connect` 之前各自是什么，`connect` 之后变成什么。请确保你的图里出现"op-state"三个字。

---

# Lesson 2 · Task\<T\>：把同步流程升格为协程

> **学习目标：** 把 Lesson 1 的"三件套"改写为一个**协程**：函数体里写 `co_await`，调用方在外面 `sync_wait`。理解为什么 `Task<T>` 不只是 `co_await` 的语法糖，而是"**带调度亲和**的 sender"。
>
> **对应 spec 章节：** `00-overview.md` §6.1；`04-coroutine-tasks.md` §3 / §4 / §6（Task / Stream / Job 三件套，本讲只用 Task）。

## §a 主流程更新

把 `01_hello_sender.cpp` 复制为 `02_hello_task.cpp`，改成：

```cpp
#include <Mashiro/Async/Foundations.h>
#include <Mashiro/Async/Coro/Task.h>
#include <Mashiro/Async/Backend/Inline.h>   // L2 提供的最简调度器

namespace Async = Mashiro::Async;

Async::Task<int> compute() {
    int x = co_await Async::just(42);
    x *= 2;
    std::println("inside coroutine: x = {}", x);
    co_return x;
}

int main() {
    auto t = compute();                                   // 返回一个 Task<int>（它是 sender！）
    auto [r] = stdexec::sync_wait(std::move(t)).value();
    std::println("returned to main: {}", r);
}
```

预期输出：

```
inside coroutine: x = 84
returned to main: 84
```

**练习：**

1. 把 `co_await Async::just(42)` 换成 `co_await (Async::just(40) | Async::then([](int x){ return x + 2; }))`。它仍然返回 42 吗？
2. 在 `compute()` 中追加第二个 `co_await`，让协程依次等待两个 sender。观察协程被挂起 / 恢复的位置。
3. 在 `compute()` 体内 `throw std::runtime_error("inside coroutine");` 不通过任何 sender——`sync_wait` 仍然能拿到这个异常吗？为什么？

## §b 概念剖析：从"组合 sender"到"写协程"

### `Task<T>` 是什么

`Task<T>` 是 `04-coroutine-tasks.md` §3 定义的"协程任务"。简单的心智模型：

> `Task<T>` 是一个**返回值为 sender 类型**的协程返回类型，等价于一个 `exec::task<T>` typedef。它的 `completion_signatures` 是 `set_value_t(T), set_error_t(exception_ptr), set_stopped_t()`。

也就是说：你写的协程函数 `compute()`，从调用者的角度看，是一个完全合法的 sender——可以 `connect`，可以 `|` 进入任何 adaptor 链，可以被 `sync_wait`。

但 `Task<T>` 不只是"协程包成 sender"那么简单。它在类型里写入了**调度亲和（scheduler affinity）**：

```
Async::Task<T>          ≈  exec::task<T, default_task_context<InlineScheduler>>
Async::Task<T, Sched>   ≈  exec::task<T, default_task_context<Sched>>
```

`default_task_context<Sched>`（P3941）的存在意味着：在协程体里每一次 `co_await sender` 之后，协程的**恢复点**会被显式地调度到 `Sched`。Lesson 4 我们会切换 `Sched`，看到效果。本讲为了简化，默认使用 `Inline` 调度器——它就是"立刻在当前线程恢复"。

### `co_await sender` 的含义

`Lesson 1` 我们手工把 sender 串起来；`Lesson 2` 我们用协程语法替代了这种串联。两者等价吗？

是，但**不仅仅**是。考虑：

```cpp
int x = co_await Async::just(42);
x *= 2;
co_await some_other_sender();
```

这段代码等价于：

```cpp
Async::just(42)
  | Async::let_value([](int x) {
        x *= 2;
        return some_other_sender();
    });
```

——但你看：两边表达**同一件事**，只是协程的形态保留了顺序结构（你看到的是按行从上往下），而手动 `let_value` 形态把数据流写成了嵌套。**协程语法把"延续传递（CPS）"重新折叠成命令式**。这一点是 P2300 之于 future/promise 模型的根本进步。

### 协程帧 与 op-state 的同构

回顾 Lesson 1 的话术：

> **op-state ≈ 把这条管线手写成的协程帧，只是它由模板生成、不需要协程语法**。

现在你**真的**写了协程。那么 op-state 在哪里？答案是：**协程帧就是 op-state**。

具体而言：

- `Task<T>` 的协程帧持有 `promise_type` —— 它扮演了 receiver 的角色。
- 协程帧的每一个挂起点对应一次 `connect(inner_sender, this->promise) → start()`。
- `co_return` 在 `final_suspend` 时把结果交给上游 receiver。

这就是为什么 `Task<T>` 的允许动作集合是固定的（`set_value(T) / set_error(exception_ptr) / set_stopped()`）——它由协程语义决定。

### 协程的代价：堆分配与 HALO

C++ 的协程在标准上**允许**编译器把协程帧分配到栈上（这一优化叫 HALO：Heap Allocation eLision Optimization），但前提是它能证明协程帧不"逃逸"到调用栈之外。我们在本讲中的代码——`compute()` 立即被 `sync_wait` 阻塞等待——**可能**触发 HALO，让协程帧根本不上堆。

但是请记住 `08-cross-cutting.md` §3.4 的明确口径：

> **跨线程传递永远逃逸**，因此每一次跨线程 Task 调用都会在堆上分配一次帧。

也就是说：到 Lesson 4 把 `Sched` 换成 `StaticPool::scheduler()` 时，HALO 必然失败，我们将看到一次堆分配。这是异步协程模型固有的代价，框架的策略是 **"在类型上文档化，而不是在调用点偷偷分配"**。

### 异常如何穿过协程

你试过 `throw std::runtime_error("inside coroutine");`——它能被 `sync_wait` 抓到。机理：`Task<T>::promise_type::unhandled_exception()` 在协程**未捕获异常**时被调用，它会把活跃异常 `std::current_exception()` 捕获为 `exception_ptr`，沿 `set_error` 通道转交给上游 receiver。

这是 `08-cross-cutting.md` §4.6 提到的"协程错误语义"。异常在协程内部仍然能被 `try/catch` 捕获，但**穿出协程边界后**就变成了 sender 链的 `set_error(exception_ptr)`——错误从此变得**像数据**。

### 一句话总结

| 比较 | Lesson 1（sender 组合） | Lesson 2（Task 协程） |
|------|------|------|
| 控制流形态 | 嵌套的 `then` / `let_value` | 命令式 `co_await` |
| op-state 实体 | 模板生成的结构体，通常落栈 | 协程帧，可能 HALO 落栈，否则堆 |
| 表达能力 | 完整 | 完整 |
| 推荐用法 | 短管线、纯组合 | 业务逻辑、需要循环 / 条件 |

它们**可以混用**——`co_await` 一个 adaptor 链是完全合法的。框架不让你二选一。

## §c 思考题

1. 把 `compute()` 改成 `Async::Task<void>`。`co_return;` 与 `co_return x;` 的区别在哪里？
2. 在 `compute()` 中写一个 `for (int i = 0; i < 3; ++i) { co_await Async::just(); }` 循环。你认为这次循环让协程被挂起 / 恢复了多少次？
3. 协程帧到底有多大？写一个 `std::println("{}", sizeof(decltype(compute().get_handle().promise())));`（伪代码——具体写法请查 spec）观察一下。
4. 如果一个函数返回 `Task<T>` **但函数体里没有 `co_await`**，它还是一个协程吗？编译器会怎么处理？

---

# Lesson 3 · stop_token：让管线可以被取消

> **学习目标：** 给现有管线接上**取消**——`stop_source.request_stop()` 让正在运行的 sender 链以 `set_stopped` 完成。理解为什么取消是**结构化的、通过 receiver 环境流动的、不需要业务代码主动轮询的**。
>
> **对应 spec 章节：** `01-foundations.md` §9（`bridge_stop_token`）；`08-cross-cutting.md` §2（取消契约）；`03-adaptors.md` §4.5（adaptor 取消清单）。

## §a 主流程更新

为了能"看到"取消的发生，我们把管线改成"等一会儿、然后打印"。复制 `02_hello_task.cpp` 为 `03_cancellable.cpp`：

```cpp
#include <Mashiro/Async/Foundations.h>
#include <Mashiro/Async/Coro/Task.h>
#include <Mashiro/Async/Backend/Inline.h>
#include <thread>
#include <chrono>

namespace Async = Mashiro::Async;
using namespace std::chrono_literals;

Async::Task<int> work(Async::stop_token tok) {
    for (int i = 0; i < 10; ++i) {
        if (tok.stop_requested()) {
            // 选择 1：手动反应。会让我们看到取消传播的时机。
            std::println("work: stop requested at i={}", i);
            throw Async::Coro::stopped_signal{};   // 见下文 §b
        }
        std::println("work: tick {}", i);
        std::this_thread::sleep_for(50ms);
    }
    co_return 42;
}

int main() {
    Async::stop_source src;
    auto tok = src.get_token();

    auto runner = std::jthread{[&] {
        std::this_thread::sleep_for(180ms);
        std::println("main thread: requesting stop");
        src.request_stop();
    }};

    auto t = work(tok);
    auto outcome = stdexec::sync_wait(std::move(t));   // 可能是 nullopt（被取消）
    if (outcome) {
        auto [r] = *outcome;
        std::println("completed normally, r = {}", r);
    } else {
        std::println("cancelled");
    }
}
```

预期输出（大致）：

```
work: tick 0
work: tick 1
work: tick 2
main thread: requesting stop
work: stop requested at i=3
cancelled
```

请实际运行一次，亲眼看到 `sync_wait` 在被取消时返回 `nullopt`。

**练习：**

1. 把 `sleep_for(180ms)` 调到 `10ms`、`500ms`，分别观察。
2. 把 `tok.stop_requested()` 的轮询去掉，但把 `sleep_for(50ms)` 换成 `co_await Async::async_timer(50ms);`（待 `Io` 后端可用时）。再看效果——取消是否更"即时"了？
3. 把 `throw Async::Coro::stopped_signal{};` 改成 `throw std::runtime_error("oops");`。观察 `sync_wait` 的反应——它是抛出异常，还是返回 `nullopt`？

## §b 概念剖析：取消是怎么流动的

### 取消的"宪法"

`08-cross-cutting.md` §2.1 用一句话写下了它：

> "**取消信号通过 receiver 的环境（env）流动。** 任何需要观察取消的 sender / awaiter / scheduler 都查 `stdexec::get_stop_token(stdexec::get_env(rcvr))`。不存在全局取消标志；token 是每条管线的、每个 receiver 携带的、由组合传播的。"

请仔细把这句话读三遍。它的内涵是：

- **没有 `Mashiro::Async::IsCancelled()` 这种自由函数**。任何形如 `bool global_cancelled` 的设计在 spec 中是被拒收的反模式（见 `08-cross-cutting.md` §2.4）。
- **取消不是"事件"**，是**通过 env 流动的标志位**。env 是 receiver 上的"属性表"，包含 allocator、stop_token、scheduler 等查询。
- **每条管线一个根 `stop_source`**，根派生 token，token 复制传播，op-state 在 `start()` 时用 `inplace_stop_callback` 注册"被取消时该做什么"。

### 三件东西的关系

| 名字 | 角色 | 谁拥有 |
|------|------|------|
| `Async::stop_source` (= `stdexec::inplace_stop_source`) | 取消的"源"，唯一可调用 `request_stop()` | 用户（Lesson 6 起改为 `Scope` 持有）|
| `Async::stop_token`  (= `stdexec::inplace_stop_token`)  | 取消的"句柄"，可查询、可复制、可经过 env 传播 | 任何 receiver 的 env |
| `inplace_stop_callback<Cb>` | 取消的"反应"，由 sender 在 `start()` 时注册 | sender 的 op-state |

`stop_source` 在被销毁时会自动 `request_stop`（如同 `std::stop_source` 之于 `std::jthread`）。任何持有这个 token 的 op-state 都会"感知到"——它们在 `start()` 时已经注册了 callback。

### 为什么用 `inplace_stop_token` 而不是 `std::stop_token`

`std::stop_token` 是引用计数的——它内部持有一个共享状态。这对于 `jthread` 是合适的（线程对象在 RAII 周期外的析构同样需要 token 有效），但对一个**已经被 op-state 持有的、生命周期严格 ≤ op-state**的取消信号来说，引用计数是浪费。

`inplace_stop_token` 的语义恰恰是"我**寄存在**调用栈某处的某个 `inplace_stop_source` 上，借用它的生命周期"。它是 pointer-sized 的、复制零成本、查询零成本。

这就是 `01-foundations.md` §4.3 把 `Async::stop_token = stdexec::inplace_stop_token` 的别名定为框架内部**唯一**取消类型的原因——不是品味，是性能与所有权语义的统一。

### `bridge_stop_token` —— 唯一的桥

但你写 `std::jthread`、调用第三方库时，外面拿到的就是 `std::stop_token`。怎么过桥？

```cpp
std::jthread legacy{[&](std::stop_token st) {
    auto bridged = Async::bridge_stop_token(st);
    // 现在 bridged->token() 是一个 inplace_stop_token，
    // 当外面那个 std::stop_token 被 request 时，bridged 也跟着 request。
    sync_wait(work(bridged->token()));
}};
```

`bridge_stop_token` 是 L0 中**唯一**承认的两类 token 转换器（`01-foundations.md` §9）。它的代价是：

- 一次堆分配（构造 `StopTokenBridge`），由 `unique_ptr` 持有；
- 一次回调注册（在外部 `std::stop_source` 上挂一个 `std::stop_callback`）。

这两笔代价都在**边界**付，而非热路径。框架不打算消灭这条桥，但也不会让它出现在你正常写的代码里。

### `Coro::stopped_signal`：取消如何穿过协程

回到代码里那行 `throw Async::Coro::stopped_signal{};`。这是 `04-coroutine-tasks.md` v0.2 §6.5 / `09-synthesis.md` §2.15 定义的"取消信号异常"：

> 当协程内部检测到取消、希望主动让 `Task<T>` 完成于 `set_stopped` 通道时，**抛出 `Coro::stopped_signal{}`**。`Task<T>::promise_type::unhandled_exception()` 会识别这个具体类型，把它转换为对上游 receiver 的 `set_stopped()` 调用；其他异常类型则继续走 `set_error(exception_ptr)`。

为什么不能直接 `co_return`？因为 `co_return r;` 走的是 `set_value(r)` 通道，**不是**取消。三个通道互斥——你必须明确选择终止于哪一个。

**但请注意**：在绝大多数情况下你**不需要**手动抛 `stopped_signal`。当你 `co_await sender` 时，如果那个内部 sender 因为取消而完成于 `set_stopped`，框架的 awaitable bridge（L4 中描述）会自动抛出 `stopped_signal`，沿协程边界传播出去。你只需要写：

```cpp
Async::Task<int> work() {
    co_await Async::async_timer(1s);   // 若被取消，框架自动 throws stopped_signal
    co_return 42;
}
```

——根本不必显式碰 token。本讲我们之所以显式查询 token，是因为我们在 sleep 里**没有 cooperative 的挂起点**——`std::this_thread::sleep_for` 是阻塞的，无法被打断。Lesson 4 我们换成 `StaticPool` 之后这种情况会改善；Lesson 7 引入 `timeout` 之后它会彻底消失。

### 取消的延迟预算

`08-cross-cutting.md` §2.7 列了一张延迟表：

| sender 形态 | 取消到完成的延迟 |
|------|------|
| `Inline::schedule()` | 0（同步检查） |
| `StaticPool::schedule()` 已入队未运行 | 1 个 work-stealing 检查（微秒） |
| `StaticPool::schedule()` 正在跑用户代码 | 直到下一次挂起点 |
| `Tbb::schedule_bulk` | 1 个 TBB 任务边界（微秒） |
| `Platform::schedule()` 已入队 | 1 次泵循环（亚毫秒） |
| `Io::async_read` 等 | 1 次系统调用往返 |
| 用户定义的 Vulkan scheduler | 直到 `vkQueueWaitIdle` 返回（毫秒） |

也就是说："**及时**"在框架里**有定义**——它是后端相关的。如果你的取消需求紧到无法接受 `vkQueueWaitIdle` 的几毫秒，就不应当把工作放到 `VkComputeScheduler` 上。这件事注解里不编码（spec 故意没用 annotation 表示 latency），由后端文档说明。

### 你**不**需要做什么

读到这里你可能觉得"取消好复杂"。请反向看：**作为框架使用者**，你需要做的事是：

```cpp
Async::stop_source src;
auto pipe = source() | adaptor1 | adaptor2 | sink();
stdexec::sync_wait(stdexec::on(scheduler, pipe));   // 阻塞直到完成或被取消

// 别处：
src.request_stop();   // 每个 op-state 都自动响应
```

——就这些。在协程里偶尔会写 `auto tok = co_await stdexec::get_stop_token(); while (!tok.stop_requested()) { ... }`，但这都是**已经习惯成自然的姿势**。框架的 sender / adaptor / pattern 都把取消处理好了。**作为 adaptor 作者**才需要走那张六行清单（Lesson 7 我们会面对）。

## §c 思考题

1. 把 `work()` 改成不取 `stop_token` 参数，而是写 `auto tok = co_await stdexec::get_stop_token();`——观察这个 awaitable 怎么把 token 从 receiver 的 env 中"摸"出来。
2. 在 `work()` 里多写一个 `co_await Async::just(123);`——再被取消时，这次 `co_await` 会发生什么？请把你的预测写下来，再跑一次。
3. 如果一个 sender 在 spec 中**没有**被标注为 `[[=Async::Cancellable]]`，把它放进 `timeout(...)` 会怎样？为什么 spec 选择**编译期**拒绝而不是**运行期**降级？（提示：看 `01-foundations.md` §5.2 + `03-adaptors.md` §10。）
4. 用一句话回答：取消、错误、值——为什么是**三**条通道而不是合并成两条？

---

# 🪞 Interlude I · 异步原语的分类学

> *本段不写代码。请合上 IDE、拿一张纸，照着读一遍、再合上书自己复述一遍。*

人类对"如何把'还没发生的计算'写进程序"这件事，发明过的原语，可以大致排成下面的谱系：

```
                  ┌─ 回调   (callback)            "请把结果交给这个函数"
                  │
                  │            ┌─ Future / Promise   "我现在拿一个'未来值'句柄"
   异步原语 ───┤   一票制  │
                  │            └─ Promise/Resolve    (JavaScript 风格)
                  │
                  ├─ 协程   (coroutine)           "把控制流写成顺序，挂起点出让"
                  │
                  └─ Sender / Receiver / Scheduler (P2300)
                       —— "把一段计算编码成一个对象，让别人决定谁来听"
```

我们可以从**三个轴**给它们打分。三个轴在 spec 中其实都已出现，只是分散在不同条目里。

## 轴一：**组合性** —— 两段异步代码能不能优雅地拼接？

| 原语 | 拼接方式 | 缺陷 |
|------|------|------|
| 回调 | 函数嵌套 | "回调地狱"；流向倒置 |
| Future | `.then(...)` 链 | 错误通道与值通道分裂、缺少取消 |
| 协程 | `co_await` | 单一返回类型限制（一次 await 必须返回它）；错误处理通过异常 |
| Sender | `\|` 管道 + `co_await` 双形态 | 完成签名约束在类型上，编译期强约束（学习曲线） |

**Sender 的关键发明**：它把"完成方式（值/错/停）的集合"显式写在类型里（`completion_signatures`），这让组合的语义可以被**编译期**检查。其他几种原语都没有这件事。这是为什么 spec 反复强调 "**concept-first** 和 **compile-time first**"（`00-overview.md` §2）。

## 轴二：**所有权与生命周期** —— 异步对象是谁的、活多久？

| 原语 | 所有权模型 | 取消语义 |
|------|------|------|
| 回调 | 调用者保证回调对象活到调用 | 无原生取消 |
| Future | `std::future`：堆共享态、引用计数 | 无原生取消（C++20 之前） |
| 协程 | 协程帧通常在堆 | 可以塞 `stop_token`，需手工 |
| Sender | **op-state 是值，通常在栈** | 通过 receiver env 的 `inplace_stop_token`，编译期强制 |

回到 Lesson 3 的结论：**取消不是事件，是通过 env 流动的标志位**。这是把所有权和取消统一到一起的结果——op-state 是一颗树，token 顺着 env 沿树向下流，callback 在每个节点上挂载、在节点析构时卸下。没有"全局取消变量"，没有 `shared_ptr` 持有的取消回调，**它和 op-state 的生命同生同死**。

## 轴三：**调度** —— "下一步在哪个线程上发生"是谁的决定？

| 原语 | 调度归属 |
|------|------|
| 回调 | 由调用回调那个线程决定（隐式） |
| Future + `.then()` | 由实现决定（通常未指定） |
| 协程 + 简陋实现 | 不挂起的话留在原线程；挂起的话由 awaitable 决定 |
| Sender | **由 receiver 的 env 中的 scheduler 决定**，可被 `continues_on(sched)` 显式改变 |

最后一行是关键。Sender 模型把"线程"从"语言级隐式状态"提升为"对象级显式参数"。`continues_on(sched, s)` 让你**在源代码层面看见**调度切换；调度器从此可以被替换、被注入、被测试。

> 哲学注记：这与依赖注入（DI）在面向对象世界的进步有同样的味道——把**隐式的环境**（线程、调度器、allocator）**显式地穿过参数**，让程序的依赖关系成为可读的事实而非可推理的猜测。

## 三轴在本课程切片中的位置

让我们回顾你已经走过的 3 讲：

- **Lesson 1（轴一）**：你看见了组合——`|` 与 `then` 把两段计算粘起来。
- **Lesson 2（轴二的引子）**：你把组合升格成命令式协程，但所有权仍然清晰（协程帧/op-state）。
- **Lesson 3（轴二+轴三的引子）**：取消是怎么沿 env 流动的，scheduler 默认 `Inline`、下一讲将换装。

你**已经在三个轴上各走了一步**，只是当时没有显式标记。接下来：

- **Lesson 4** 会让你看清轴三的全貌（StaticPool 跨线程）。
- **Lesson 5** 会让你看清"完成方式的集合"如何随着 Stream 而扩展（轴一深化）。
- **Lesson 6** 会让你看清生命周期闭合（轴二完成）。

## 一句送你的总结

**回调把控制流颠倒，future 把值与错误分裂，协程把代码写顺，sender 把全部三件事都装进类型。** 这门课要教你的，不是"sender 的语法"，而是**怎么用 sender 把异步问题写成你能在编译期就检查正确性的样子**。

---

# Lesson 4 · StaticPool：跨线程的第一次

> **学习目标：** 把工作放到一个**真正的工作线程池**上跑。用 `continues_on(sched, s)` 显式地"换线程"，并亲眼看到 `std::this_thread::get_id()` 在管线两端是不同的。同时直面：HALO 失效带来的协程帧分配、`forward_progress_guarantee` 在分类学里的位置。
>
> **对应 spec 章节：** `02-backends.md` §3（Inline）/ §4（StaticPool）；`00-overview.md` §5.1（调度器概念阶梯）；`01-foundations.md` §5.2（Affine / IsForwardProgress 注解）；`08-cross-cutting.md` §3.4（协程帧分配）。

## §a 主流程更新

复制 `03_cancellable.cpp` 为 `04_pool.cpp`，把 sleep 换成"真正的跨线程"：

```cpp
#include <Mashiro/Async/Foundations.h>
#include <Mashiro/Async/Coro/Task.h>
#include <Mashiro/Async/Backend/StaticPool.h>
#include <thread>
#include <print>

namespace Async   = Mashiro::Async;
namespace Backend = Async::Backend;

Async::Task<int> work() {
    std::println("entered  on tid={}", std::this_thread::get_id());

    auto pool = Backend::StaticPool::scheduler();
    co_await Async::continues_on(pool);    // <— 把后续的协程恢复点切换到池里

    std::println("hopped   on tid={}", std::this_thread::get_id());
    co_return 42;
}

int main() {
    std::println("main     on tid={}", std::this_thread::get_id());
    auto [r] = stdexec::sync_wait(work()).value();
    std::println("returned to main: {}", r);
}
```

预期输出（具体 tid 不同，但**三行 tid 不会完全一致**）：

```
main     on tid=0x1
entered  on tid=0x1
hopped   on tid=0x2
returned to main: 42
```

**练习：**

1. 把 `co_await Async::continues_on(pool)` 注释掉，看三个 tid 是不是变成相同的。理解为什么。
2. 加第二次 `co_await Async::continues_on(Backend::Inline::scheduler())`，再观察 tid 跳回。
3. 试着在跳到 `pool` 之后做一些 CPU bound 的工作（计算前 1000 个素数），观察主线程没有被阻塞。
4. 在 `work()` 里加 `co_await stdexec::just();`——观察 tid 在这一行**之后**有没有变。`just()` 改不改 tid？为什么？

## §b 概念剖析：调度从隐式变显式

### `continues_on(sched, s)` 是怎么"换线程"的

读一下它的语义（`01-foundations.md` §4.3 + P2300）：

> `continues_on(sched, s)` 接受一个 sender `s` 和一个调度器 `sched`，返回一个新 sender。新 sender 的行为：先在 `s` 的完成位置完成；**之后**，把后续 receiver 的 `set_*` 调度到 `sched` 上。

它不"改变" `s` 在哪里运行——`s` 在哪里运行，是 `s` 自己决定的（由它的 completion scheduler）。`continues_on` 改变的是**接续点**。

在协程里，"接续点"恰好就是协程的下一次恢复——所以你看到 `co_await Async::continues_on(pool);` 之后 tid 切到了池上。在纯 sender 链里也一样：`s | continues_on(sched) | then(f)` 让 `f(...)` 在 `sched` 上跑。

### 调度器的概念阶梯

`00-overview.md` §5.1 与 `01-foundations.md` §4.4 描绘了 5 + 2 个概念：

```
              Scheduler                  ── 所有后端必须模型
              ├─ BulkScheduler           ── 提供 schedule_bulk
              ├─ IoScheduler             ── 提供 get_io_context（I/O 提交点）
              ├─ AffineScheduler         ── 永远完成在固定线程（Platform 是典型）
              ├─ ParallelScheduler       ── 前进保证 ≥ parallel
              └─ HasStopToken<Env>       ── env 携带非 never 的 stop token (热路径快判)
              (+ AsyncQueue 是 L4 给 Stream 用的)
```

每一个 backend 自报家门：**我满足哪些**？它通过 L1 的注解声明：

```cpp
struct [[=Async::BackendTag{Backend::StaticPool}]]
       [[=Async::ProgressTag{stdexec::forward_progress_guarantee::parallel}]]
       [[=Async::OffersBulk]]
       [[=Async::Cancellable]]
       [[=Async::Allocates{Allocates::Where::OpState}]]
StaticPoolScheduler { /*...*/ };
```

——然后 `01-foundations.md` §8 的 **验证器** 在编译期检查："你说你 `OffersBulk`，那我们看看 `Concepts::BulkScheduler<S>` 在不在？"两边对不齐就 `static_assert` 失败。这是注解的真正威力：**注解不是装饰，是被反射查询、被编译期验证的契约**。

我们到 Lesson 10 会**自己**走一遍这条验证器。

### Inline vs StaticPool 的契约对比

| 维度 | `Inline` | `StaticPool` |
|------|------|------|
| `Concepts::Scheduler` | ✅ | ✅ |
| `Concepts::BulkScheduler` | ❌ | ✅ |
| `Concepts::AffineScheduler` | ❌ | ❌ |
| `Concepts::ParallelScheduler` | ❌（`weakly_parallel`） | ✅ |
| `Concepts::IoScheduler` | ❌ | ❌ |
| `Cancellable` | ✅ | ✅ |
| `Allocates::Where` | ❌（无） | `OpState` |
| 完成发生在 | 调用 `start()` 的同一线程 | 池中某个 worker |

`Inline::schedule()` 是合法的"trivial 调度器"——它什么也不做就让 receiver 同步完成。这听起来没意义，但在两个地方非常有用：

1. 单元测试里把异步管线"拆掉异步"，使其在测试主线程上立刻完成。
2. 作为某些 adaptor 的"默认调度器" fallback（例如 `Stream<T>` 在没有显式 `Sched` 时用 `Inline`）。

`StaticPool` 是 spec 中第一个**真正**的并发调度器（`02-backends.md` §4）。它的形态：

- 固定大小 worker 集合（构造时指定，默认 = 硬件并发数）
- 每个 worker 一个本地双端队列 + 一个全局 MPSC 入口（256 槽，溢出 `terminate`）
- 工作窃取（work-stealing）调度
- `schedule_bulk(n, fn)` 把 `n` 分到所有 worker

它**不是** TBB——TBB 是 Lesson 3 中后端表里的另一项（`Tbb`），它有自己的 arena 与 task。本课程不深入 TBB，但它的存在解释了"为什么 spec 不只用 StaticPool"——TBB 是为细粒度并行优化过的，而 StaticPool 是给"通用异步工作"用的。两者在 L1 注解上略有差异（TBB 的 `Allocates::Where::External` vs StaticPool 的 `OpState`），让用户能够区分调度成本结构。

### 这一次，HALO 真的失效了

回顾 Lesson 2 的 cliffhanger：

> 跨线程传递永远逃逸。

跑一下本讲的程序，如果你的运行环境装了 `Diagnostics::AllocCheck`，把 `main()` 改成：

```cpp
int main() {
    Async::Diagnostics::AllocCheck guard{"lesson4-pool"};
    auto [r] = stdexec::sync_wait(work()).value();
}
```

`AllocCheck` 会汇报：协程帧分配了一次。这是 spec 承认且**文档化在 `Task<T>` 类型上**的代价，不是 bug。如果你觉得这次分配在你的 hot path 上扛不住：

- 改用 sender 组合形态（Lesson 1 风格），让 op-state 落栈。
- 通过 `default_task_context` 把自定义 PMR allocator 绑定到 `Task<T>`，让分配走 arena。

两条路都不是"消除"分配，是**让分配可被掌控**。这是 `08-cross-cutting.md` §3 的口径：

> "记账可文档化，不在调用点偷偷分配。"

### "forward progress guarantee" 是什么

这是 stdexec 从 C++ 标准的并行算法里继承来的一个概念：

| 强度 | 含义 |
|------|------|
| `concurrent` | 多个工作可以并发取得进展；它们之间没有公平性承诺 |
| `parallel` | 工作彼此独立，运行时可以选任意 worker 推进 |
| `weakly_parallel` | 不保证不会饿死，但调度器尽力 |

`Inline` 是 `weakly_parallel`（同一线程上的工作只能按提交顺序进展），`StaticPool` 是 `parallel`（工作窃取保证活力），`Io` 是 `concurrent`（一次只能在 io_uring submission queue 里推一个，但完成是并行的）。

L1 的 `IsForwardProgress` / `Traits::ProgressOf_v<S>` 把这个信息暴露给 adaptor。Lesson 8 的 `parallel_for` 模式会拒绝 `weakly_parallel` 的调度器——因为它无法保证"分发出去的 chunk 不会被同一 worker 串行执行"，从而无法体现并行加速。**编译期拒绝**。

## §c 思考题

1. 用一句话写出：`continues_on(sched, s)` 与 `on(sched, s)` 的区别。前者改"接续在哪"，后者改"启动在哪"——这两者在编译产物上有何不同？
2. 如果你把 `co_await continues_on(pool)` 之后又 `co_await continues_on(Inline::scheduler())`，最终的 `set_value` 会发生在哪个线程？为什么？
3. spec 故意没有规定 `StaticPool` 的工作窃取算法（仅给出"work-stealing"这个轮廓）。把它当作 implementation detail，会给课程材料的稳定性带来什么影响？反之，如果把窃取算法也写进 spec 呢？
4. 设想：你为引擎的渲染线程实现了一个 `RenderScheduler`，它满足 `AffineScheduler`（永远完成在渲染线程上）。把 `bulk(n, fn)` 应用到 `RenderScheduler` 应当是合法的还是非法的？为什么？（提示：`09-synthesis.md` §2.7。）

---

## Lesson 5 · 从单值到 Stream<T>

**主题：** 把"一次性 sender"升格为"可重入的流"。引入 `Stream<T>`、`stream::next()`、`for co_await`、`AsyncQueue`、反压（backpressure）。
**对应 spec：** `04-coroutine-tasks.md` §5；`09-synthesis.md` §2.5 与 §2.8；`00-overview.md` §6。

> **本讲想解决的根本问题：** 到 Lesson 4 为止，我们的管线只能"产生一个值，结束"。但真实异步世界里，绝大多数东西是**流**：网络数据包、UI 事件、传感器读数、Vulkan 帧之间的 fence 完成、log 输出……一次性 sender 无法表达"持续到来"。我们需要一个新的**词汇**——它必须仍然站在 sender/receiver 的地基上，又能让 `co_await` 看上去像 `for` 一样自然。

### §a 主流程更新

在 Lesson 4 的 `StaticPool` 例子之上，加一个生产者—消费者切片：让 `StaticPool` 的一个 worker 把整数 0..N 送进一个 `AsyncQueue`，主协程则把它们一个个取出来打印。

`examples/05_stream.cpp`：

```cpp
#include <Mashiro/Async/Foundations.h>
#include <Mashiro/Async/Coro.h>
#include <Mashiro/Async/Backend/StaticPool.h>
#include <print>
#include <chrono>

namespace Async   = Mashiro::Async;
namespace Backend = Mashiro::Async::Backend;
using namespace std::chrono_literals;

// 生产者：在线程池中把 0..N 写进队列。
Async::Task<void> producer(Async::AsyncQueue<int>& q, int N) {
    auto pool = Backend::StaticPool::scheduler();
    co_await Async::continues_on(pool);
    for (int i = 0; i < N; ++i) {
        co_await q.push(i);           // 满则挂起 —— 这就是反压
        std::println("  produced {}", i);
    }
    q.close();                        // 关闭后，consumer 的 next() 会拿到 nullopt
}

// 消费者：把队列升格为 Stream<int>，然后 `for co_await` 取走。
Async::Task<int> consumer(Async::AsyncQueue<int>& q) {
    int sum = 0;
    Async::Stream<int> s = Async::from_queue(q);
    MASHIRO_FOR_CO_AWAIT(int v, s) {
        std::println("  consumed {}", v);
        sum += v;
    }
    co_return sum;
}

Async::Task<int> pipeline() {
    Async::AsyncQueue<int> q{/*capacity=*/4};
    auto prod = Async::spawn_detached(producer(q, 10));   // 后台拉起
    int total = co_await consumer(q);
    co_await prod;                                        // 等生产者结束
    co_return total;
}

int main() {
    auto [total] = stdexec::sync_wait(pipeline()).value();
    std::println("total = {}", total);
}
```

**你应当看到。** 生产者打印 `produced 0..3`（队列填满，第 4 次会暂时挂起）；消费者开始消费，打印 `consumed 0..`，每消费一个，生产者就**释放一个槽位**继续生产。两线 interleaving 出现，最后 `total = 45`。

**`MASHIRO_FOR_CO_AWAIT` 是什么？** C++26 才会规范化 `for co_await (auto v : s) { ... }` 这种 range-based asynchronous for 语法（P2300 提案族的延伸）。在 C++23 / clang-p2996 上我们尚未拿到这条语法糖，所以 spec（`04-coroutine-tasks.md` §5.3 与 `09-synthesis.md` §2.8）给出一个**临时宏**作为 fallback：

```cpp
#define MASHIRO_FOR_CO_AWAIT(decl, stream)                       \
    for (auto __it = (stream).begin(); ; co_await __it.next())   \
        if (auto __v = __it.current(); !__v) break;              \
        else if (decl = *__v; true)
```

——它把"每一步异步地拿下一个值"翻译成"先 `co_await next()`，再判断是否到尾"。这是一段**故意的临时设施**：当编译器支持 `for co_await` 后，宏即被删除，**而用户代码无需改动**（spec 承诺把宏名保留为别名，至少跨一个版本）。这是一种"语法迁移路径"设计，我们在 Lesson 9 还会再遇到一次（domain rewrite）。

### §b 概念剖析

#### Stream<T> = "sender-of-optional"

最朴素的理解方式：

```
sender<T>      ≈ Task<T>      ≈ 异步的 T
Stream<T>      ≈ Task<optional<T>> 的可重入版
```

每次 `co_await stream.next()` 给你一个 `std::optional<T>`：

- `*value` —— 有下一个值
- `nullopt` —— 流结束（正常或被关闭）

也就是说，**Stream 不是 sender 的兄弟，是 sender 的"可重入特化"**。所有 sender 的词汇——env、completion signature、stop token——在 Stream 上原封照搬，只是完成签名变成

```
set_value_t(std::optional<T>)   // 每一步
set_stopped_t()                 // 被取消
set_error_t(std::exception_ptr) // 错误
```

而**每次 `next()`** 都是一次 sender——它有自己的 op-state、自己的 receiver。**这是为什么 Stream 不能与一次性 sender 互换**：一次性 sender 的 op-state 在 `start()` 之后只完成一次；Stream 必须能反复 `next()`，意味着它**内部**会维护一个可重入的状态机。

#### `AsyncQueue<T>`：第一类异步原语

`AsyncQueue` 是 spec 中**唯一**被列为"L4 基础异步原语"的 SPSC/MPMC 队列（`04-coroutine-tasks.md` §5.4）。它有两条核心 sender：

- `q.push(v)` —— 满则挂起，直到有槽位（**这就是反压机制**）
- `q.pop()`  —— 空则挂起，直到有值或被关闭

`from_queue(q)` 把 `q.pop()` 包成 `Stream<T>`：

```cpp
template<class T>
Stream<T> from_queue(AsyncQueue<T>& q) {
    while (auto v = co_await q.pop()) {
        co_yield *v;
    }
    // q 被 close 时，pop() 返回 nullopt，循环退出，Stream 自然结束。
    co_return;
}
```

注意这是一个**生成器协程**（用 `co_yield`）。Stream 在 Mashiro 中的实现路径就是 P3941-tinted generator：每次 `co_yield` 把值塞进 Stream 的"slot"，next() 来取。

#### 反压：把流量控制从用户手里收回来

不带反压的"流"（例如经典回调式 `onNext`）有一个臭名昭著的失败模式：生产者比消费者快，事件在中间无限堆积，最终 OOM。

`AsyncQueue` 通过**有限容量 + push 时挂起**把反压做进了原语本身：

- 生产者快 → 队列填满 → `push(v)` 不返回 → 生产协程**暂停**（co_await 让出）
- 消费者慢 → 没人取 → 队列保持满 → 生产者继续暂停
- 消费者拉走一个 → 队列空出槽 → 唤醒挂起的 `push(v)`

这是 **bounded async channel** 的标准模式（Go 中的 buffered channel、Rust tokio 的 `mpsc::channel(N)`、Kotlin 的 `Channel(N)`）。它的妙处在于：**你不需要写一行流控代码，反压就被静态保证了**。

#### 为什么要分 sender 与 Stream

为什么不能简单地说"所有 sender 都可以多次 await"？

1. **op-state 不可重入。** 一次性 sender 把 op-state 当作"启动—完成"的单次状态机。允许重入就要给每个 sender 配上"我用了吗"的运行时检查——增加内存与开销。
2. **completion 仪式不同。** sender 完成一次就 set_value，receiver 的"完成钩子"是终结性的；Stream 的每一步只是"取了一个值"，整条流的终结是另一次完成（`set_stopped` 或最终 `nullopt`）。
3. **HALO 友好度不同。** sender 落栈是常态；Stream 的状态横跨多个 await suspend point，**通常需要堆**——因此 Stream 在 spec 里被明确标注 `Allocates::Where::Coroframe`。

> **范畴学的提醒（这条只挂在嘴边，不展开）。** 你大致可以把 sender 想成 `monad`（`then` 是 bind），Stream 想成 `monad-of-list`。我们 Interlude II 会从范畴论侧把这两条线给接通。

#### 异步模型五分类（速览）

| 编号 | 模型 | 一次/多次 | 推/拉 | 例子 |
|------|------|------|------|------|
| ① | sender / receiver | 一次 | 拉 | `just(x) | then(f)` |
| ② | Task<T> 协程 | 一次 | 拉 | `co_await s` |
| ③ | Stream<T> | 多次 | 拉 | `for co_await (v : s)` |
| ④ | Job | 无返回 | —— | `nursery.spawn(j)` |
| ⑤ | callback / signal | 多次 | 推 | Qt signals、OS callbacks |

Mashiro 框架支持 ①–④，**不直接支持 ⑤**——回调式 push 模型留给 user-land：用户可以用 `AsyncQueue` 把 callback 桥接为 Stream（`q.push` 给回调，`from_queue(q)` 给协程）。spec 的口径（`00-overview.md` §6）是："push 模型可用，但不进 L4 词表，因为它把流控甩给了上层。"

### §5.6 Channel：点对点的"有界 / 可关闭"流（v0.3 增补 · 来自 Unit V）

到现在为止我们对 *点对点流* 的处理是 `AsyncQueue` + `from_queue` 这一对——它够用，
但**契约模糊**：

- 谁负责调用 `close()`？两端都可以吗？
- 关闭后还在飞行中的 `push` 怎么处理？
- 满了的时候应该挂起、丢弃，还是返回错误？

这些问题都是 *约定式* 的——三个项目可能给出三种不同答案。**Channel\<T, Cap\>** 把
这些约定抬升为 **类型契约**：

```cpp
namespace Mashiro::Async {

enum class CloseError { ChannelClosed };

template <typename T, Capacity Cap = Bounded<32>>
class Channel {
public:
    // 发送端
    auto send(T v)
        -> sender_of<set_value_t(), set_error_t(CloseError), set_done_t()>;

    // 接收端
    auto recv()
        -> sender_of<set_value_t(T), set_error_t(CloseError), set_done_t()>;

    // 关闭（幂等；任一端均可调）
    void close() noexcept;
};

}
```

**契约**（写进 completion_signatures，编译器替你证）：

| 事件 | send 完成方式 | recv 完成方式 |
|------|-------------|--------------|
| 正常 | `set_value()` | `set_value(T)` |
| 通道关闭后再调 | `set_error(ChannelClosed)` | `set_done()`（= EOF） |
| 关闭时已挂起的等待 | `set_done()` | `set_done()` |

> **设计选择**：recv 在 EOF 时用 `set_done` 而非 `set_value(end-sentinel)`——理由是这与
> stop_token 的取消语义自然对齐，让 `Channel` 可以无缝塞进 `stop_when(token)` adaptor。

**为什么不是 `AsyncQueue` 的换皮？** 三点：

1. *关闭对称性*：`AsyncQueue` 的 close 是单向（生产者），Channel 是双向（任一端）；
2. *容量泛型*：`Capacity = Bounded<N> | Unbounded | SingleShot`（最后这个 = oneshot，单次发完即关）；
3. *与 sender 适配*：`send` 本身就是 sender，可以 `|` 管道，不需要把 `q.push(v)` 包成 sender。

**最小例**：

```cpp
co_await [&] -> Task<void> {
    auto ch = Channel<int, Bounded<8>>{};

    auto producer = [&] -> Task<void> {
        for (int i = 0; i < 5; ++i)
            co_await ch.send(i);                // 满了自动挂起
        ch.close();                              // 生产端关闭
    }();

    auto consumer = [&] -> Task<void> {
        MASHIRO_FOR_CO_AWAIT(int v, ch.as_stream()) {   // recv() 循环 = stream
            std::printf("got %d\n", v);
        }                                         // 收到 set_done 自动退出
    }();

    co_await when_all(producer, consumer);
}();
```

**背压选择**（`Capacity` 的 strategy 部分）：

| Strategy | 满时的 send 行为 | 适用 |
|----------|---------------|-----|
| `Bounded<N>` | 挂起到有空位 | 默认；保证不丢、靠挂起反压上游 |
| `Bounded<N, DropOldest>` | 丢弃队首旧值后入队 | 实时数据（行情、传感器）|
| `Bounded<N, DropNewest>` | 立即 `set_done` 拒绝当前 send | 流控严格的协议 |
| `Unbounded` | 永不挂起 | **危险**：仅用于已知有界的 pipeline |
| `SingleShot` | 第二次 send → `set_error` | 替代 promise / oneshot |

### §5.7 Topic：一写多读的广播流（v0.3 增补 · 来自 Unit V）

`Channel` 解决了 1↔1。当上游有 1 个事件源、下游有 N 个相互独立的订阅者时，**不应当**
用 N 个 Channel——那会强迫上游知道下游数量，破坏 *开闭原则*。这正是 **Topic\<T,
Strategy\>** 的位置：

```cpp
namespace Mashiro::Async {

template <typename T, typename Strategy = Strategy::Buffered<16>>
class Topic {
public:
    auto publish(T v)
        -> sender_of<set_value_t(), set_done_t()>;

    class Subscriber {
    public:
        auto stream() -> Stream<T>;
        ~Subscriber();                          // 析构自动取订
    };

    auto subscribe() -> Subscriber;
};

}
```

**Strategy 三选一**（对照 Kotlin SharedFlow / Tokio broadcast+watch）：

| Strategy | 慢订阅者发生时 | 等价物 |
|----------|------------|-------|
| `Buffered<N>` | 每个订阅者一个有界 ring，**只**对该订阅者丢老消息 | Tokio broadcast |
| `Latest` | 只保留最新一条；新订阅者立即拿到最新值 | Tokio watch / Kotlin StateFlow |
| `Blocking` | 慢订阅者反压**所有上游 publish** | Akka EventBus（默认） |

**关键设计点**：`Buffered<N>` 的丢弃**是订阅者本地的**，不会污染其他订阅者——这是
EventPump 当前手写实现的语义，把它抬升为类型契约就是 Topic。

**与 Channel 的区分律**（写进课程就是为了 *避免学生混淆*）：

| 维度 | Channel | Topic |
|------|---------|-------|
| 读者数量 | 1 | N（动态变化）|
| 关闭语义 | 双向幂等 | 仅发布端可关 |
| 慢读者影响 | 反压上游 | 仅影响该订阅者（除非 Blocking）|
| 历史回放 | 无 | `Latest` 策略提供"最新一条"回放 |
| 等价物 | `MpscQueue<T>` 包装 | `EventPump<T>` 抽象化 |

**最小例**：

```cpp
auto topic = Topic<EngineEvent, Strategy::Buffered<32>>{};

// 生产端：游戏循环
auto produce = [&] -> Task<void> {
    while (running) {
        co_await topic.publish(read_engine_event());
    }
}();

// 订阅端 1：UI
nursery.spawn([&] -> Task<void> {
    auto sub = topic.subscribe();
    MASHIRO_FOR_CO_AWAIT(EngineEvent e, sub.stream()) {
        ui.dispatch(e);
    }
});

// 订阅端 2：日志
nursery.spawn([&] -> Task<void> {
    auto sub = topic.subscribe();
    MASHIRO_FOR_CO_AWAIT(EngineEvent e, sub.stream()) {
        logger.write(e);
    }
});
```

**EventPump 重构对照**（Unit III 案例预告）：

```
旧（手写 N×SpscRing）：              新（Topic 抽象）：
  EventPump pump;                     Topic<Event, Buffered<RingN>> pump;
  auto r1 = pump.add_reader();        auto s1 = pump.subscribe();
  auto r2 = pump.add_reader();        auto s2 = pump.subscribe();
  pump.write(e);                      co_await pump.publish(e);
  while (r1.try_pop(out)) ...;        MASHIRO_FOR_CO_AWAIT(auto v, s1.stream())
```

**性能必须**：Topic 的实现 *底下就是* N×SpscRing——它**不**多一份拷贝、**不**多一次
原子操作。`Buffered<N>` 的实现走 Unified Writer + per-subscriber lock-free ring 这一
EventPump 已经验证过的路径。Topic 是 *零成本类型包装*，不是新机制。

### §c 思考题

1. 把上面的 `consumer` 改成"一边收集 sum，一边每收 3 个就把它们 `then` 进一次 `Inline::scheduler()` 上的副作用计算"，应当用什么 adaptor？（提示：`03-adaptors.md` §6 的 `batch`。）
2. 如果生产者**不调用** `q.close()` 直接退出，consumer 的 `for co_await` 会发生什么？这种语义对用户友好吗？应该让 `AsyncQueue` 在被销毁时自动 close 吗？（spec 中是怎么选的？为什么？）
3. 写出 Stream<T> 的完成签名集合（`completion_signatures`），并比较它与一次性 `sender<T>` 的签名差异。哪一个签名**不出现**在一次性 sender 中？为什么？
4. `MASHIRO_FOR_CO_AWAIT` 这个迁移宏的存在，意味着课程材料里有一条**未来需要重写**的语法。请评估：当 C++26 标准化 `for co_await` 后，这一讲的代码量会减多少？教学概念上是否有任何东西丢失？

---

## Interlude II · sender / Stream 的范畴论解读

我们在 Lesson 1–5 之间已经悄悄堆起了一座小教堂：sender 是一种 monad，Stream 是它的"列表化"近亲，`then` 是 bind，`when_all` 是 product，`race` 是 sum。这一段不算作业，但若你愿意花十五分钟把它读下来，再回头看 Lesson 1 的 `just(42) | then(f) | then(g)`，你会从机械的"管道符"里看出一个**自然变换**的影子。

### sender 作为一个 functor

回忆 functor 的定义：一个能把**对象**映射到对象、把**态射**映射到态射的结构，并尊重恒等与合成。

把 sender 看成 `Sender<T>`，把 `then(f)` 看成对态射 `f : A → B` 的提升 `then(f) : Sender<A> → Sender<B>`：

- **保持恒等**：`then(id_A) ≡ id_{Sender<A>}`（在 set_value 时调用 `id` 不改任何东西）
- **保持合成**：`then(g) ∘ then(f) ≡ then(g ∘ f)`（这就是 stdexec 实现中"`then(f) | then(g)` 可被融合"的代数依据）

这两个等式不是 C++ 编译器自动检查的，但 spec（`03-adaptors.md` §3）把它们写成了**法则**：任何 adaptor 实现都必须满足。这是为什么 `transform_sender` 可以在 `domain` 那一层做"重写而不改变语义"——重写遵守 functor 律，被观察的程序行为不变。

### sender 作为一个 monad

monad 比 functor 多了两件事：单位（`return`/`unit`）与展平（`join`，或等价的 `bind`）。

- 单位：`Async::just(x) : T → Sender<T>`
- 展平：`Async::let_value(s, f) : Sender<A> × (A → Sender<B>) → Sender<B>`

`let_value` 不是装饰，是把"内层 sender"拆出来再启动的关键——它在 `08-cross-cutting.md` §2 中也是**取消传播**的关键节点（取消令牌跨越 monadic boundary）。

**monad 律。** 三条：

1. **左单位**：`let_value(just(x), f) ≡ f(x)`
2. **右单位**：`let_value(s, just) ≡ s`
3. **结合**：`let_value(let_value(s, f), g) ≡ let_value(s, [&](auto x){ return let_value(f(x), g); })`

这些等式在 `03-adaptors.md` §4 中以"行为等价"的形式被记录——它们让你写出的 `let_value` 链路可以在不改变结果的前提下被**编译器重写**（例如把两次 `let_value` 折叠成一次以减少 op-state 嵌套深度）。

### Stream 作为 monad-of-list

如果 sender 是"异步的 T"，Stream 大致是"异步的 list<T>"。把它的几条主要 adaptor 翻译成范畴语：

| Stream adaptor | 范畴语 |
|------|------|
| `stream::map(f)`  | functor 的 `fmap` |
| `stream::filter(p)` | monad 的 `bind`（投影到 0 或 1 步） |
| `stream::flat_map(f)` | list monad 的 `bind`（每步映成一段子流） |
| `stream::merge(s1, s2)` | list monad 的 `mplus` |
| `stream::take_while(p)` | 受限的 `filter` |

`flat_map` 是 Stream 的"内层 sender"展平等价物——它的存在让 Stream 形式上构成一个 monad-on-monad。spec 没有强迫用户读懂范畴，但**adaptor 表的形状**是由这些代数约束决定的：少一条会让 Stream 变残，多一条往往可以从已有 adaptor 推出。

### "自然变换"在哪？

`from_queue(q) : AsyncQueue<T> → Stream<T>` 是一个**自然变换**：对任何 `T`，它把队列形态翻译成流形态，**且与 `map(f)` 交换**（无论你先 `from_queue` 再 `map`，还是先把队列里的元素 `map` 再 `from_queue`，结果同构）。

这条交换图是 spec 中"`AsyncQueue` 是 push 模型与 pull 模型的桥梁"那句话的形式化版本。你下次看到 spec 里类似的"桥接"动词，可以下意识问一下："它满足什么交换律？"——你会发现绝大多数都满足。

### 为什么这些抽象值得知道

范畴论在工业实践里有一个非常具体的用处：**它告诉你哪些重写是合法的**。当 stdexec 的 `transform_sender` 把 `then(f) | then(g)` 重写成 `then(g ∘ f)` 时，它依赖的就是 functor 律。当 `let_value(let_value(s, f), g)` 被重写成单层时，它依赖的是 monad 结合律。

> 如果你的 spec **隐含**遵守这些律，但**没有写出来**，等到第一次 backend 重写时，bug 就会从重写器里渗出来。
> 如果你的 spec **写出**这些律，重写器就有了静态保证。

这是 P2300 在标准化过程中把"adaptor laws"写进非规范注释的原因，也是 Mashiro spec `03-adaptors.md` §3 沿用这一惯例的理由。

不必把范畴论挂在嘴边——你只需要在写 adaptor 时问自己一句："**这玩意儿和它的兄弟交换吗？**" 若答案是肯定的，恭喜：你刚刚发现了一条可以入 spec 的法则。

---

## Lesson 6 · Scope 与 Nursery —— 让生命周期闭合

**主题：** 把"在背后跑的 Task"用 **Scope** 包起来；引入 `Nursery`、`spawn`/`spawn_detached`、`LinkedScope`、`Supervised`；学会读 spec 里关于"结构化并发"的承诺。
**对应 spec：** `05-structured.md` §§1–4；`08-cross-cutting.md` §1（取消传播）；`00-overview.md` §7。

> **本讲想解决的根本问题：** Lesson 5 的 `spawn_detached(producer(q, 10))` 是一颗烟雾弹——它把一个 Task **扔向某处**，没有解释它在谁的麾下，谁负责等它结束，谁负责把异常带回 main。结构化并发的核心信条："**没有 Task 是孤儿**"——所有异步任务必须**在某个作用域里出生、在该作用域关闭前结束**。

### §a 主流程更新

把 Lesson 5 的代码改写：消除 `spawn_detached`，引入 `Nursery`。

`examples/06_nursery.cpp`：

```cpp
#include <Mashiro/Async/Foundations.h>
#include <Mashiro/Async/Coro.h>
#include <Mashiro/Async/Structured.h>
#include <Mashiro/Async/Backend/StaticPool.h>
#include <print>

namespace Async   = Mashiro::Async;
namespace Backend = Mashiro::Async::Backend;

Async::Task<void> producer(Async::AsyncQueue<int>& q, int N) {
    auto pool = Backend::StaticPool::scheduler();
    co_await Async::continues_on(pool);
    for (int i = 0; i < N; ++i) {
        co_await q.push(i);
        std::println("  produced {}", i);
    }
    q.close();
}

Async::Task<int> consumer(Async::AsyncQueue<int>& q) {
    int sum = 0;
    Async::Stream<int> s = Async::from_queue(q);
    MASHIRO_FOR_CO_AWAIT(int v, s) sum += v;
    co_return sum;
}

Async::Task<int> pipeline() {
    Async::AsyncQueue<int> q{4};
    int total = 0;
    co_await Async::open_nursery([&](Async::Nursery& n) -> Async::Task<void> {
        n.spawn(producer(q, 10));                      // 生产者归 n 管
        total = co_await consumer(q);                  // 消费者在主流程
        co_return;
    });
    // 离开此处 ⇒ Nursery 已关闭 ⇒ producer 已确定结束
    co_return total;
}

int main() {
    auto [t] = stdexec::sync_wait(pipeline()).value();
    std::println("total = {}", t);
}
```

**关键差异。** 上一讲 `spawn_detached` 之后我们要在末尾手动 `co_await prod`；这一讲我们什么都不必做——**Nursery 的析构会等所有 spawn 出的 Task**，并把它们的异常（如果有）合并 throw 出来。

如果你把 `producer` 改成在第 5 步抛异常：

```cpp
if (i == 5) throw std::runtime_error{"producer crashed"};
```

观察控制流：

1. Nursery 收到子 Task 异常。
2. **取消令牌**被 propagate 到所有兄弟 Task（包括 consumer）——consumer 的 `for co_await` 在下一次 `next()` 时 `set_stopped`。
3. Nursery 关闭，把异常 throw 给 `pipeline()` 的协程帧。
4. `pipeline()` 把异常 throw 给 `sync_wait`，再由 `main` 接住。

这是 **structured concurrency 的"故障收敛"承诺**——任何子任务的失败都被汇集到 nursery 关闭的那一点，不会"漏掉一个静悄悄退出的协程"。

### §b 概念剖析

#### Scope 家族

`05-structured.md` 把 Scope 分三类：

| 名称 | 来源 | 用途 |
|------|------|------|
| `stdexec::counting_scope` | P3149，重新导出 | 底层"等所有 sender 完成"的原语 |
| `Async::Nursery` | Mashiro 新增 | 友好的协程内 spawn / await 关闭 接口 |
| `Async::LinkedScope` | Mashiro 新增 | 把 nursery 与某个父对象生命周期绑定 |

`Nursery` 是 `counting_scope` 的"协程外观"。`open_nursery` 是 RAII-with-coroutines：它打开一个 scope，把它传给 lambda，等 lambda 结束后再等所有 `spawn` 进来的 child，最后关闭。**全过程是一个 sender**——意味着它可以参与 `when_all` / `race` / `let_value` 等 adaptor 的组合。

#### 三种 spawn

```cpp
n.spawn(t);              // 子任务必须返回 void，异常向 n 汇报
n.spawn_value(t);        // 子任务有返回值，需配 AsyncQueue / promise 取回
n.spawn_detached(t);     // 子任务对 n 不可见 —— ⚠ spec 中标 Use sparingly
```

`spawn_detached` 在 spec 中**没有被禁用**，但被打上了"逃逸阀"标签——它把结构化保证短路，等同于回到旧的"`std::thread::detach()`"语义。`05-structured.md` §3 给的指导是：

> 仅在子任务自身已被另一种生命周期机制（例如全局服务、有自己 nursery 的子系统）所守护时使用。

我们 Lesson 5 用 `spawn_detached` 是为了让概念逐步引入；Lesson 6 起，正确的做法永远是 `spawn`。

#### 取消传播的方向

```
        ┌─ Nursery ─────────────────┐
        │  parent_stop_source        │
        │       │                    │
        │       ▼                    │
        │  ┌───────────┐ ┌────────┐  │
        │  │ child A   │ │ child B│  │
        │  │ stop_tok  │ │ stop_tok│  │
        │  └───────────┘ └────────┘  │
        └────────────────────────────┘
              ▲              │
              │ exception    │ external cancel
              │              ▼
            outer        outer.request_stop()
```

两条规则（`08-cross-cutting.md` §1）：

1. **外部取消下行。** 当父级的 stop_token 被请求时，Nursery 把请求**广播**给所有 child 的 receiver env。
2. **子级异常上行。** 任意 child throw（或 `set_error`）时，Nursery 自动向**剩余兄弟**请求 stop，并把异常聚合到 close 时刻。

这两条配合一起，给出了一句口号："**Nursery 是异常与取消的"事件视界"**"——一旦异常进入 nursery 视界，外面的世界只会以 nursery 关闭那一刻的姿态观察到它。

#### `LinkedScope<Parent>`

很多时候你想把 nursery 的生命周期与某个**对象**绑定：例如 `class GameSession` 在销毁时应当自动关闭它管理的所有后台协程。`LinkedScope<GameSession>` 是用这种意图设计的：

```cpp
class GameSession {
    Async::LinkedScope<GameSession> scope_;   // ⬅ 成员
public:
    Async::Task<void> startNetIO();
    ~GameSession() = default;                  // scope_ 析构时自动等所有 child
};
```

`LinkedScope` 在编译期捕获其父类型，仅允许在 `Parent` 的成员函数内 `spawn`，并把父对象指针写入 child 的 env（child 可以反查 parent）。这是一个**结构化所有权**的范式，**编译期**而不是运行期保证"协程的归属"。

#### `Supervised<Policy>`

`Supervised<Policy>` 是 Nursery 的一个升级版本：它在 child 异常时不一定要"全部取消并 throw"，可以按 `Policy` 决定：

- `Policy::OneForOne` —— 重启出错的 child，其他继续
- `Policy::AllForOne` —— 取消所有 child，再全部重启
- `Policy::OneForAll` —— 经典 Erlang 监督树
- `Policy::Stop` —— 等同于普通 Nursery

这是 spec 从 Erlang/OTP 借来的**监督树（supervision tree）**模式。Mashiro 的 `Supervised` 把它放在 L5——属于"高级结构化"，本课程的主流程不强制用，但 `05-structured.md` §4 把它列为"任何长寿命服务"的推荐封装。

### §c 思考题

1. 把 `spawn_detached` 替换成 `spawn` 后，如果 consumer 比 producer 早结束（例如 consumer throw 异常），nursery 的行为是什么？把它逐步写出来。
2. `open_nursery` 是一个**返回 sender 的函数**。它能与 `when_all(open_nursery(...), other_sender)` 组合吗？这种组合在语义上意味着什么？
3. `LinkedScope<Parent>` 拒绝在 `Parent` 之外的代码里 `spawn`——这是一种"作用域可见性"的编译期检查。如果改成运行期检查，会丢失什么？保留编译期检查又有什么代价？
4. 把生产—消费 pipeline 改造成 `Supervised<Policy::OneForOne>`，让 producer 偶尔抛异常时自动重启。请草拟接口：`Policy` 应当能拿到什么参数？（提示：`05-structured.md` §4。）

---

## Lesson 7 · timeout / retry / race —— 给主流程穿铠甲

**主题：** 引入"加固管线"的三件套：`timeout`、`retry`、`race`；理解它们如何借取消令牌实现"无侵入超时"。
**对应 spec：** `03-adaptors.md` §7–§9；`08-cross-cutting.md` §1 与 §4。

> **本讲想解决的根本问题：** Lesson 6 的 pipeline 在 happy path 上能跑。但真实系统会**抖**：网络偶尔卡 5 秒、磁盘偶尔抽风、远程 RPC 偶尔丢包。我们需要词汇来表达"等不超过 200 毫秒，超时就重试三次，每次重试用不同后端"——并且这些词汇必须**与已有 sender / Task 无缝复合**。

### §a 主流程更新

在 Lesson 6 的基础上，把 `consumer` 改造成"读 5 个值，超过 200ms 就放弃，至多重试 3 次"。

`examples/07_armored.cpp`（关键片段）：

```cpp
Async::Task<int> consumer_with_timeout(Async::AsyncQueue<int>& q) {
    using namespace std::chrono_literals;

    auto one_attempt =
        Async::let_value(Async::just(), [&](auto) {
            // 内层：读最多 5 个值
            return Async::then(
                Async::stream::take(Async::from_queue(q), 5)
                    | Async::stream::fold(0, std::plus<>{}),
                [](int s){ return s; }
            );
        });

    auto armored =
        one_attempt
        | Async::timeout(200ms)        // 超时 → set_stopped
        | Async::retry(3,               // 最多三次
                       [](auto& err){ return /*是否可重试*/ true; });

    int sum = co_await std::move(armored);
    co_return sum;
}
```

把 producer 改成"偶尔睡 500ms 模拟卡顿"：

```cpp
if (i == 4) std::this_thread::sleep_for(500ms);   // 触发 timeout
```

**你应当看到。** 第一次 attempt 收到 4 个值（0,1,2,3），第 5 个等不到（producer 在第 4 步睡），200ms 后 `timeout` 把 stop token 推下去，consumer 的 `next()` 返回 `set_stopped`；`retry` 接住，发起第二次 attempt——但此时 producer 已经醒过来继续推数，第二次 attempt 顺利收满 5 个，`sum = 0+1+2+3+4 = 10`（如果你把 queue 在 attempt 之间清空就更干净）。

### §b 概念剖析

#### timeout 的内部实现 = race + scheduler

`Async::timeout(D, s)` 不是某个特殊 primitive，它**完全**用已有词汇组合：

```cpp
template<class S>
auto timeout(std::chrono::nanoseconds D, S s) {
    return Async::race(
        std::move(s),
        Async::Backend::Platform::delay(D)
            | Async::then([](auto){
                throw Async::Coro::stopped_signal{};
              })
    );
}
```

`race(a, b)` 启动两个 sender，谁先完成谁是答案；另一个的 stop token 被请求。当 `delay(D)` 先到，它 throw `stopped_signal` → `race` 给 `s` 发停止信号 → `s` 的 op-state 把 token 透传到所有下游 → 整条管线干净收尾。

这是 spec 中**反复出现**的一个范式：**"高级行为 = 低级原语 + 取消令牌"**。一旦取消令牌可靠地在 receiver env 中流通，你能用它合成出非常多看起来"复杂"的行为：

- timeout = race + delay
- debounce = stream + delay + stop
- circuit breaker = retry + 计数 + delay

#### retry 的两条参数

`retry(N, predicate)` 的两件事：

- **N**：最大尝试次数（含首次）
- **predicate**：函数 `(error&) -> bool`，回答"这个错误值得重试吗"

predicate 是关键——盲目 retry 会让"配置错误"这种**永远会重复**的失败被反复重试。spec 的口径（`03-adaptors.md` §8）：

> retry 默认只对"传输/IO 类"错误重试；语义性错误（参数错误、协议违例）不应被默认 retry 截获。

为什么把 predicate 暴露而不是写死？因为框架不知道你的语义错误长什么样——`std::system_error` 里 ENOMEM 该不该 retry？看上层。

#### race vs when_all：两条孪生

| | `when_all(a, b)` | `race(a, b)` |
|------|------|------|
| 完成条件 | 两者都完成 | 任一完成 |
| 完成值 | `tuple<A, B>` | `variant<A, B>`（或择一） |
| 失败传播 | 任一失败 ⇒ 整体失败 | 任一失败 ⇒ 整体失败；另一被 stop |
| 取消传播 | 外部 stop ⇒ 两者均 stop | 外部 stop ⇒ 两者均 stop |
| 范畴学 | product (×) | sum (+) / coproduct |

`race` 与 `when_all` 是范畴里的两面镜子——product / coproduct。它们覆盖了"且 / 或"两类异步组合。Lesson 8 的 `parallel_for` 内部用的是 `when_all` 的列表化版本。

#### 取消传播的六行验收清单

每写一个新 adaptor，你都应当把它过一遍（**这是 spec `08-cross-cutting.md` §1 的强制检查清单**）：

| # | 检查项 |
|---|------|
| 1 | adaptor 接收外部 stop_token 后能在**有限时间**内停下 |
| 2 | adaptor 内部 spawn 的子 sender 都拿到了同一个 stop_token |
| 3 | adaptor 被 stop 后 set_stopped（**不**是 set_error） |
| 4 | adaptor 已 set_value / set_error 后再来的 stop 是 no-op |
| 5 | stop 期间分配的资源都被释放（没有"半完成"状态泄漏） |
| 6 | 在测试里能用 `Diagnostics::CancelProbe` 注入取消并观察行为 |

写一个 timeout、写一个 retry，把这张表填完——你就把"自己实现 adaptor"的门槛跨过去了。

### §7.5 Bulkhead：把"容量"显式化（v0.3 增补 · 来自 Unit V）

`retry` 之后我们有了一种 *时间* 维度的弹性（"失败就再来一次"），但还缺一种 *空间*
维度的弹性：**同时只允许 N 个 sender 进入临界区**。这在数据库连接、GPU 上传队列、外
部 HTTP 调用等场景里 *无所不在*——只是大多数项目用一个裸 `Semaphore` 加一个手写计数
器来表达，**契约靠注释维持**。

`Bulkhead<Tag, N, Policy>` 把这个约定升格为类型：

```cpp
namespace Mashiro::Async {

namespace Policy {
    struct Reject {};              // 满则 set_done（让上游知道被拒绝）
    struct Wait {};                // 满则挂起等待
    template <typename D> struct Timeout {};  // 等到 D 后 set_error(timeout)
}

template <typename Tag, std::size_t N, typename P = Policy::Wait>
class Bulkhead {
public:
    auto acquire()
        -> sender_of<set_value_t(Permit<Tag>), set_error_t(...), set_done_t()>;
};

template <typename Sender, typename Tag, std::size_t N, typename P>
auto with_bulkhead(Sender s, Bulkhead<Tag, N, P>& bh);
// 用法：sender | with_bulkhead(db_pool_guard) | then(query)
}
```

**为什么需要 `Tag`？** —— 它让 `Bulkhead<DB, 10>` 与 `Bulkhead<GPU, 4>` 在类型层不混
淆。把一个 DB permit 误传到 GPU 路径上 → **编译期** 拒绝。Tag 是一段 *零运行时开销的
信息*。

**Policy 三选一**（致敬 Resilience4j）：

| Policy | 行为 | 典型场景 |
|--------|------|-------|
| `Reject` | 满 → `set_done` | 用户请求；前端可以"现在忙，请稍后"|
| `Wait` | 满 → 挂起 FIFO | 后台任务；不能丢 |
| `Timeout<D>` | 等 D 仍满 → `set_error(timeout)` | RPC；要给上游一个明确 deadline |

**最小例**：

```cpp
Bulkhead<DB, 10, Policy::Timeout<200ms>> db_guard;

auto query = make_query(sql)
           | with_bulkhead(db_guard)
           | then([](Row r) { return process(r); });
```

**与 retry 组合**——这是 Unit III 陷阱章节会专门讲的"雷"。错误的组合：

```cpp
// 反例：retry 包在 bulkhead 外侧 → 第 k 次重试时仍要重新争 permit，最坏情形 N×k 倍排队
make_query(sql) | with_bulkhead(g) | retry(5);

// 正例：retry 包在 bulkhead 内侧 → 一旦拿到 permit，连续重试不再排队
(make_query(sql) | retry(5)) | with_bulkhead(g);
```

规则：**总是把可重入的 work 放在 permit 之内**——permit 是稀缺资源，不能 *拿了又放*。

### §7.6 CircuitBreaker：让失败"自愈"（v0.3 增补 · 来自 Unit V）

`retry` 解决 *偶发* 失败；`Bulkhead` 解决 *并发拥塞*。但还有一种失败形态——**下游
确实坏了**——这时 retry 会变成雪崩助推器（每个调用方都在敲一个已经挂掉的服务），
Bulkhead 也帮不上忙（permit 拿到了但 work 注定失败）。

**CircuitBreaker** 把电路安全的隐喻搬过来：

```
         ┌────────── ε 失败 ──────────┐
         │                              ▼
       Closed  ──── k 次连续失败 ────► Open ──── 等待 W ────► HalfOpen
         ▲                                                       │
         └──────── HalfOpen 探测成功 ───────────────────────────────┘
                                                                  │
                                                  HalfOpen 探测失败│
                                                                  ▼
                                                                Open
```

三个状态、三个参数：

```cpp
template <typename Clock = SteadyClock>
class CircuitBreaker {
public:
    struct Config {
        std::size_t            failure_threshold = 5;        // k
        Clock::duration        open_window       = 30s;      // W
        std::size_t            half_open_probes  = 1;        // HalfOpen 允许多少试探
    };

    explicit CircuitBreaker(Config cfg);

    // adaptor：在 Open 状态下直接 set_error(BreakerOpen)，不真正调用 sender
    template <typename S>
    auto operator()(S s);
};
```

**用法**：

```cpp
CircuitBreaker breaker{{.failure_threshold = 5, .open_window = 30s}};

auto safe_call = remote_rpc()
               | breaker                           // 包在最外，失败立刻短路
               | with_bulkhead(rpc_pool)
               | retry(2);                          // retry 只在 Closed 状态下生效
```

**关键不变量**（spec 必须 *证* 而非 *承诺*）：

1. *单调状态转移*：状态机不能从 Open 直接回 Closed，**必须** 经过 HalfOpen；
2. *计数原子*：failure_threshold 计数器走 `std::atomic`，但不需要全局锁；
3. *HalfOpen 串行*：HalfOpen 状态下 *只放* `half_open_probes` 个调用过去，其余直接 set_error。

**与 retry / bulkhead 的层级**——这是 v0.3 给出的 *标准防御纵深*：

```
      最外层： CircuitBreaker          （下游死透了就别打）
            ↓
      第二层： Bulkhead                （别压垮自己的资源）
            ↓
      第三层： Retry                   （瞬时抖动重试一下）
            ↓
      最内层： 实际 sender              （真正干活的）
```

记住这四层的顺序——倒着写就是经典的雪崩 + 死锁 + 资源耗尽三连。

### §c 思考题

1. 把 `timeout(200ms, s)` 实现成"基于 `Platform::delay` + race"——画出它的 receiver env 流：外部 stop 沿哪条路径到达 `delay` 与 `s`？
2. `retry(N, pred)` 在第 k 次失败后调用 `pred(err)`。如果 `pred` 自身抛异常会发生什么？spec 给出的口径是什么？（提示：`08-cross-cutting.md` §4。）
3. `race` 的实现需要在赢家完成后**等**输家被取消才能整体完成。如果输家不响应 stop（例如它处于一个不可取消的系统调用里），`race` 会怎样？spec 怎么处理这种"恶意 sender"？
4. 把六行验收清单（§b）默写一遍。**这是闭卷题——你需要在 Lesson 10 用它检查自己写的注解验证器。**

---

## Interlude III · 结构化并发的哲学动机

到现在为止，你已经在键盘上敲过五讲代码。回头看：每一讲都在向**同一个抽象**靠近——"作用域"。Lesson 1 的 sender 是无作用域的，Lesson 2 的 Task 把作用域绑到一个协程帧，Lesson 6 的 Nursery 把作用域显式化、可观察、可关闭。Lesson 7 的 timeout/retry/race 实际上**也**是在玩作用域：每次 attempt 是一个临时作用域，超时是它的边界。

为什么"作用域"会成为异步编程的核心抽象？

### "goto considered harmful" 的异步版本

1968 年 Dijkstra 写下 **"Go To Statement Considered Harmful"**，奠定了结构化编程的时代——`if`、`while`、`for` 三类控制结构对应"判定 / 重复 / 终止"，每一种都让程序的**控制流图**保持单入口单出口。

到了异步时代，`std::thread::detach()`、回调注册、Fire-and-forget 协程，这些事实上的"异步 goto"重新把控制流变成意大利面：

- 谁等谁？
- 异常去哪？
- 取消怎么传？
- 资源什么时候释放？

Nathaniel J. Smith 在 2018 年的论文 **"Notes on structured concurrency, or: Go statement considered harmful"** 里把这一系列问题正式命名为 **structured concurrency 的动机**。Mashiro 的 `Nursery` 直接继承自这一谱系（Trio nursery → Python anyio → Swift Task Group → Kotlin coroutineScope → C++ counting_scope）。

### "时间" vs "地点"两种作用域

C++ 程序员熟悉**词法作用域**——`{}` 限定符号的可见范围。结构化并发把这种"地点"作用域扩展成"时间"作用域：

| 维度 | 词法（地点） | 结构化并发（时间） |
|------|------|------|
| 边界 | 花括号 `{}` | `open_nursery(...)` 的 await 边界 |
| 内部出生的实体 | 自动变量 | 子任务 |
| 离开边界时 | 析构 | 等待全部子任务 + 异常聚合 |
| 异常路径 | 析构序 | child-to-parent 传播 |
| 编译期检查 | 名字查找 | concept / 类型签名 |

这种**对称性**是设计上故意的。一旦你接受了"`open_nursery` 是异步世界的 `{}`"，你就会发现 Mashiro 的 spec 编排其实是一个**语言扩展**——它在把 C++ 的作用域规则推广到时间维度。

### 取消令牌作为"作用域信号"

取消令牌乍看是一个独立的特性，但若把它放进结构化并发的语境，它其实是**作用域的物理信号**：

- 父作用域关闭 ⇒ 给所有 child 发 stop
- 兄弟异常 ⇒ 给其他 child 发 stop
- timeout 到点 ⇒ 给被包裹的 sender 发 stop

一旦理解了这一点，"取消传播"就不再是某个 adaptor 的私事，而是**整个框架的总线**。这是为什么 `08-cross-cutting.md` 把取消放在 §1 而不是某个 adaptor 的角落——它是一种**横切关注点**（cross-cutting concern），任何 layer 的实现都必须正确处理。

### 哲学层面的"承担责任"

结构化并发有一种**伦理**意味：每个协程都有一个**父**为它负责，每个父都对它的 children 负责。这是与"detach 文化"针锋相对的——后者把孤儿任务丢给运行时，让 OOM / hang / race condition 在未来某个不相干的时刻爆炸；前者要求每条异步路径在出生时就被"认领"。

如果非要给本课程的设计哲学起一个名字，那就是：

> **每个 await 都要有归属。每个 Task 都要有家。**

Lesson 8 起我们会把这条原则放在 backend rewriting 与 pattern composition 的检验框里——你会发现：所有看起来"高级"的 pattern，其实只是**结构化并发原则的不同形状的具象化**。

---

## Lesson 8 · parallel_for 与 pipeline —— 把控制流抽出来

**主题：** 第一次进入 L6（patterns 层）。学 `parallel_for`、`pipeline`，理解"pattern = adaptor + scheduler 的可复用编排"。
**对应 spec：** `06-patterns.md` §§1–4；`02-backends.md` §4（`schedule_bulk`）；`01-foundations.md` §6（`BulkScheduler` / `ParallelScheduler`）。

> **本讲想解决的根本问题：** 到 Lesson 7 为止，每条"管线"都还是你**手写**的：选 backend、选 adaptor、选取消策略。但工程现场有一类"经典编排"——`parallel_for(0..N, work)`、`pipeline(stages...)`——它们的形态高度重复。能不能把它们抽成**一行调用**，同时还保留"取消传播、反压、错误聚合"的所有结构化承诺？

### §a 主流程更新

`examples/08_parallel_for.cpp`：把 0..1000 用 `parallel_for` 并行平方并求和。

```cpp
#include <Mashiro/Async/Foundations.h>
#include <Mashiro/Async/Patterns.h>
#include <Mashiro/Async/Backend/StaticPool.h>
#include <atomic>
#include <print>

namespace Async   = Mashiro::Async;
namespace Backend = Mashiro::Async::Backend;
namespace Pat     = Mashiro::Async::Patterns;

Async::Task<long long> sum_of_squares(int N) {
    auto pool = Backend::StaticPool::scheduler();
    std::atomic<long long> acc{0};

    co_await Pat::parallel_for(pool, 0, N, [&](int i) {
        acc.fetch_add(static_cast<long long>(i) * i, std::memory_order_relaxed);
    });

    co_return acc.load();
}

int main() {
    auto [s] = stdexec::sync_wait(sum_of_squares(1000)).value();
    std::println("sum = {}", s);   // 期望 333'833'500
}
```

`examples/08_pipeline.cpp`：用 `pipeline` 把三阶段处理串起来——读、解析、写。

```cpp
co_await Pat::pipeline(
    Pat::stage(reader_sched,   [](char const* path){ return read_file(path); }),
    Pat::stage(parser_sched,   [](std::string raw){ return parse_json(raw); }),
    Pat::stage(writer_sched,   [](Document doc){ return write_db(doc); })
)("input.json");
```

**你应当看到。** `parallel_for` 把 1000 个迭代分给 StaticPool 的 worker；分块大小由 backend 通过 `schedule_bulk` 决定，你不需要写一行分块代码。`pipeline` 让每个 stage 跑在自己的调度器上（reader 在 Io、parser 在 Pool、writer 又回 Io），stage 之间通过隐式 `AsyncQueue` 串接，反压自动生效。

### §b 概念剖析

#### 什么叫 "pattern"

`patterns` 这个词在 L6 中的口径（`06-patterns.md` §1）：

> A **pattern** is a named composition of L2 schedulers, L3 adaptors, and L4 task/stream primitives, exposing a small, opinionated interface for a recurring concurrent shape.

——三句话拆开：

- "named composition" —— 它是**已经组合好的**东西，不是新原语
- "of L2 / L3 / L4" —— 它**只用**下层词汇，不引入新公理
- "opinionated interface" —— 它**收紧**了下层的自由度，换来用户的便利

这是"L6 是教学层（不是必须）"的来由：你**永远**可以不用 patterns 而手写等价组合；patterns 存在的意义是让 95% 的常见用法**便宜**。

#### parallel_for 的内部组装

`parallel_for(sched, lo, hi, fn)` 大致等价于：

```cpp
auto rng = std::views::iota(lo, hi);
return Async::schedule_bulk(sched, rng.size(), [=](auto i){ fn(lo + i); });
```

`schedule_bulk` 是 L2 给 `BulkScheduler` 暴露的 sender 工厂。它**要求** `sched` 满足 `Concepts::BulkScheduler<S>`——如果你传一个 `Inline`，**编译期失败**：

```
error: static_assert failed: 'parallel_for requires BulkScheduler;
       Inline::scheduler is not a BulkScheduler.
       Concept 'OffersBulk' is missing from the annotations.'
```

这是 L1 注解 + L2 concept 配合工作的实例：注解描述"我具备 bulk"，concept 验证"你具备 bulk"，pattern 要求 concept——**整条契约链在编译期闭合**。

#### pipeline 的内部组装

`pipeline(stages...)` 的伪代码：

```cpp
auto operator()(Input x) {
    return Async::let_value(
        Async::on(stage_0.sched, just(x) | then(stage_0.fn)),
        [=](auto y0){
            return Async::let_value(
                Async::on(stage_1.sched, just(y0) | then(stage_1.fn)),
                [=](auto y1){
                    return Async::on(stage_2.sched, just(y1) | then(stage_2.fn));
                });
        });
}
```

——它把 N 个 stage 折叠成 N 层 `let_value`，每一层把上一层的输出**搬运**到下一层指定的调度器。

如果你把 stage 数量从 3 推到 N，这种"手写 let_value 链"就麻烦。`pipeline` 用 P2996 反射或 fold expression 把它**编译期展开**，让用户只看到 `pipeline(a, b, c, d, e)` 的形态。

#### Pattern 的"哲学" —— 把"经验"沉淀进类型

为什么 spec 决定把 patterns 单独立为 L6，而不是让用户每次自己组合？

1. **教学价值。** `parallel_for(sched, 0, N, fn)` 是新人**第一句**就能读懂的 API；让他们自己去组合 `schedule_bulk + iota + sync_wait` 是不友好的入门曲线。
2. **正确性沉淀。** patterns 把"取消传播、反压、错误聚合"的正确写法**封进类型签名**——用户不可能写错。
3. **可被分析。** 一旦 pattern 是命名的，代码评审和性能 profiler 可以按 pattern 维度统计。

但 patterns 也有代价：

- 它**收紧**了选择，你不能在 `parallel_for` 里插一个自定义的 chunk 策略
- 它**引入**了额外的命名空间，新人要记 `parallel_for` 是 L6 不是 L3
- 它**绑定**了一组下层约定，如果你 fork 一个新 scheduler，patterns 不会自动认识

spec 的解决方法是：**把 pattern 的实现暴露在头文件里**，欢迎你 copy-paste 改成自己的"领域 pattern"。这是为什么 `06-patterns.md` §6 把 `parallel_for` 的全部源码（不到 30 行）作为附录贴出。

### §8.4 Rendezvous：Latch / Barrier / Phaser（v0.3 增补 · 来自 Unit V）

`parallel_for` 把 N 个独立子任务推下去后 `when_all` 等它们结束——这是一种 *出口同步*。
但有一类问题需要 **过程中** 的同步：

- *N 个子任务一起跑到某一点后再一起继续*；
- *主线程要等 K 个事件发生才继续*；
- *多阶段任务，每阶段所有人必须对齐*。

这三类问题在历史上分别用 **Latch / Barrier / Phaser** 解决（Doug Lea 给 java.util.concurrent
的命名沿用至今）。Mashiro 把它们抽象到 sender 框架里：

#### 8.4.1 Latch —— 单向倒数

`Latch(n)` 维护一个 *只能减* 的计数器；任何 `wait()` sender 在计数 = 0 之前都挂起，
之后 *永远* 立刻返回。

```cpp
class Latch {
public:
    explicit Latch(std::size_t n);
    void count_down(std::size_t k = 1) noexcept;        // 非阻塞
    auto wait() -> sender_of<set_value_t(), set_done_t()>;
};
```

**典型用法**：主程序启动 *等待 K 个 worker 就绪*——

```cpp
Latch ready{worker_count};

for (int i = 0; i < worker_count; ++i)
    nursery.spawn([&, i] -> Task<void> {
        co_await initialize(i);
        ready.count_down();                              // 我已就绪
        co_await run_loop(i);
    });

co_await ready.wait();                                   // 所有 worker 就绪后才开放服务
start_serving_requests();
```

**特征**：单向、一次性、无 reset；这正是 std::latch（C++20）的语义，Mashiro 给它一个
sender 接口而已。

#### 8.4.2 Barrier —— 双向集合，可重置

`Barrier(n)` 让 n 个参与者反复 *约定一个集合点*，所有人到齐后一起继续；下一轮自动开始。

```cpp
class Barrier {
public:
    explicit Barrier(std::size_t n);
    auto arrive_and_wait() -> sender_of<set_value_t(), set_done_t()>;
    auto arrive_and_drop() -> sender_of<set_value_t()>;    // 减少 generation 人数后继续
};
```

**典型用法**：多阶段算法的步进对齐——

```cpp
Barrier sync{N};

for (int i = 0; i < N; ++i)
    nursery.spawn([&, i] -> Task<void> {
        for (int phase = 0; phase < PHASES; ++phase) {
            co_await compute_phase(i, phase);
            co_await sync.arrive_and_wait();             // 等所有人完成这一阶段
        }
    });
```

**与 Latch 的区分**：Barrier 在所有人到齐后 *自动 reset*，下一轮再来；Latch 一次到 0
就永远开放。这是 std::barrier（C++20）的语义。

#### 8.4.3 Phaser —— 多阶段、可注册 / 注销参与者

`Phaser` 是 Barrier 的 *动态版*——参与者数量可以在运行时变化，并且每一阶段（phase）
可以执行 *阶段切换* 钩子。

```cpp
template <std::size_t MaxPhases = unlimited>
class Phaser {
public:
    auto register_party()      -> Party;
    auto arrive_and_await(Party&) -> sender_of<set_value_t(std::size_t /*phase*/), set_done_t()>;
    auto deregister(Party&&)   -> void;
};
```

**典型用法**：分布式 *选举 + 任期* 的对齐——参与者可能在某些任期退出，新参与者可能
中途加入。Phaser 让 *人数动态* 不破坏 *阶段对齐*。

Mashiro 默认不强求所有项目用 Phaser；它作为 **选修** 暴露——只有需要"动态参与者"
语义时才用。多数项目 Latch + Barrier 已经够。

#### 8.4.4 实战：2PC 雏形（two-phase commit）

把三个原语放在一起，最经典的演示就是 **两阶段提交**——这也是 Lab 8 的可选作业。

```cpp
// 协调者
auto coordinator = [&] -> Task<bool> {
    Barrier prepared{participants.size()};
    Latch  committed{participants.size()};
    std::atomic<bool> all_ok{true};

    for (auto& p : participants)
        nursery.spawn([&] -> Task<void> {
            // Phase 1: prepare
            bool ok = co_await p.prepare();
            if (!ok) all_ok = false;
            co_await prepared.arrive_and_wait();         // 所有人完成 prepare

            // Phase 2: commit / abort
            if (all_ok.load()) co_await p.commit();
            else                co_await p.abort();
            committed.count_down();                       // 提交一个就计一个
        });

    co_await committed.wait();
    co_return all_ok.load();
};
```

**为什么 prepare 用 Barrier，commit 用 Latch？**
- prepare 要求 *全员到齐* 才能进入决策——Barrier 的语义；
- commit 只要 *所有人最终都做完*，不需要相互等——Latch 的语义。

这就是为什么 *分开命名*（Latch ≠ Barrier）有教学价值——它逼着设计者在每一步同步处
*选择契约* 而非随手拍一个 condvar。

### §c 思考题

1. 用一句话回答："为什么 `parallel_for` 不能接受 `Inline::scheduler()`？" 然后翻 `01-foundations.md` §6 把这句话翻译成 concept 表达式。
2. `pipeline(stages...)` 的 stage 之间是**隐式**通过 `let_value` 串行，还是通过 `AsyncQueue` 并行？两种实现在性能与语义上的差异是什么？spec 选了哪种？为什么？
3. 写一个你自己领域里**重复出现**的并发编排——例如"对所有相机帧做 GPU 滤镜"，把它命名为一个 pattern。它需要什么 concept？它的 cancellation 行为如何？
4. patterns 层"可以被绕过"——这与"必须使用 nursery"形成对比。两者在 spec 中的态度有何不同？为什么？（提示：`05-structured.md` §1 的"hard requirement" vs `06-patterns.md` §1 的"opinionated convenience"。）

---

## Lesson 9 · backend domain 与 transform_sender —— 当 spec 给你留了重写权

**主题：** 引入 sender 的 **execution domain**；通过 `transform_sender` 在编译期把某个 adaptor 重写成 backend 特定的更优形态。
**对应 spec：** `02-backends.md` §6；`07-extension.md` §3；`03-adaptors.md` §10；`09-synthesis.md` §2.7。

> **本讲想解决的根本问题：** 到目前为止，所有 adaptor（`then`、`bulk`、`when_all`、`pipeline`）都假定"通用实现"足够。但每个 backend 都有它擅长的招式：TBB 的 `parallel_for` 在 backend 内可以走 TBB arena 的细粒度调度；GPU 调度器的 `bulk` 应当变成 dispatch；Io 调度器的 `when_all` 可以走 io_uring 的 link chain。能不能让 backend **"伸手"**改写 sender 树，让通用代码自动获得 backend 特化的加速？

### §a 主流程更新

`examples/09_domain.cpp`：写一个最小 domain，把 `then(f)` 在某个标记调度器上重写成"先打 log 再调用 f"。

```cpp
#include <Mashiro/Async/Foundations.h>

namespace Async = Mashiro::Async;

struct TracingDomain {
    template<class Sender, class Env>
    static auto transform_sender(Sender&& s, Env const& env) {
        if constexpr (Async::is_then_sender<Sender>) {
            auto f = Async::get_then_fn(s);
            auto upstream = Async::get_then_upstream(s);
            return Async::then(std::move(upstream),
                [f = std::move(f)](auto&&... args) {
                    std::println("[trace] entering then");
                    auto r = f(std::forward<decltype(args)>(args)...);
                    std::println("[trace] leaving  then");
                    return r;
                });
        } else {
            return std::forward<Sender>(s);
        }
    }
};

struct TracingScheduler {
    using domain_type = TracingDomain;
    /* schedule()、equality 等略 —— 与 Inline 同形态 */
};
```

然后把 Lesson 1 的程序换上 `TracingScheduler`：

```cpp
auto pipeline = Async::on(TracingScheduler{},
    Async::just(42) | Async::then([](int x){ return x * 2; })
);
```

**你应当看到。** `then` 的 lambda 两侧自动包了 trace 打印——而你**没有改一行 then 的代码**。这就是 domain 重写。

### §b 概念剖析

#### Domain 是什么

domain 是 P2300 里相对深的一个概念，但用一句话说清楚：

> **Domain 是 sender 树的"持有者"** —— 它有权在程序被求值之前对树进行重写。

具体表现：当你写 `Async::on(sched, s)`，框架会查 `sched` 的 domain（默认 `default_domain`），然后让 domain 看一遍 `s` 这棵树。domain 可以：

- 不动它（默认行为）
- 把某个节点替换成等价的更优形态（例如把 `bulk(n, fn)` 在 TBB domain 下替换成 `tbb_parallel_for(n, fn)`）
- 加注解、加日志、加 trace

#### 为什么要重写而不是写新 adaptor

可以问："为什么不让用户直接写 `Async::tbb_bulk(...)` 呢？" 答：

1. **代码不可移植。** 用户的 `parallel_for(pool, 0, N, fn)` 如果显式调用 `tbb_bulk`，把后端换成 GPU 时就要改代码。
2. **代数律不可保。** `then(f) | then(g) ≡ then(g ∘ f)` 这条 functor 律，是 domain 重写器可以利用的；如果用户写 backend 特定调用，重写器就看不到。
3. **领域知识沉淀位置错了。** "TBB 的 bulk 应当用 TBB 风格"这是 backend 实现者的知识；让它**留在 backend 端**才是正确的关注点分离。

因此 P2300 选择"domain 重写"作为机制：用户写**抽象 sender**，backend 把它**翻译**成自己想要的形状。

#### 三条约束

重写不是任意改写。spec 给出三条硬约束：

1. **可观察等价。** 重写前后，receiver 收到的完成签名集合必须相同；侧效（throw、cancel、value）行为不能改变。
2. **代数律保留。** 重写器**只能**应用 spec 中列出的法则（functor 律、monad 律、`continues_on` ∘ `continues_on` = `continues_on`）；不能因为"我觉得这样快"就乱改。
3. **没有跨域副作用。** 重写器不能把 sender 跨出 domain 边界——意即不能把 TBB domain 重写出来的代码扔进 Io domain，反之亦然。

`02-backends.md` §6 把这三条以**测试义务**形式落地：每个新 domain 要附带 "round-trip test"，证明 `original ≡ transform(original)`（在可观察意义上）。

#### Mashiro 已规划的几条 domain

| Domain | 来源 | 典型重写 |
|------|------|------|
| `default_domain` | stdexec | 不改 |
| `static_pool_domain` | Mashiro L2 | `bulk(n, fn)` → 工作窃取分块 |
| `tbb_domain` | Mashiro L2 | `bulk(n, fn)` → `tbb::parallel_for` |
| `io_domain` | Mashiro L2 | `when_all(io_a, io_b)` → io_uring link chain |
| `vk_compute_domain` | Lesson 11 自定 | `bulk(n, fn)` → vkCmdDispatch |

注意"GPU compute domain"是 Lesson 11 的练习。**这一讲我们只学机制**；Lesson 11 让你亲手写一个。

### §c 思考题

1. `transform_sender` 是**编译期**调用还是**运行期**调用？为什么这个回答决定了 domain 能不能用于异质后端（CPU + GPU）？
2. 一个 sender 树通过两个嵌套 `on` 进入两个不同的 domain（`on(io, on(pool, s))`）。spec 规定 domain 重写的顺序是 outer→inner 还是 inner→outer？查 `09-synthesis.md` §2.7 给出答案。
3. 写一个 domain，**故意**违反"可观察等价"——把 `then(f)` 重写成"调用 f 两次"。请预测：这种 domain 会通过 spec 的哪些检查、过不了哪些检查？编译期能检测到吗？
4. domain 重写让"通用代码自动跑得快"，听起来很好。但它也让**代码的实际行为**与你看到的源码偏离。spec 怎么平衡"用户可读"与"backend 自由"？这个权衡在 §a 的 trace 例子里如何体现？

---

## Interlude IV · 注解、反射与可证明的分类

到现在为止你已经多次见到一个名字：**注解**（annotation）。Lesson 4 看到 `[[=Async::Offers{...}]]`，Lesson 8 看到 pattern 用 concept 检查注解隐含的能力，Lesson 9 看到 domain 通过 sender 类型做模式匹配。

这一讲不写代码，我们把这条线挑出来——**注解是什么？为什么 spec 把它作为 L1 的核心？**

### 什么是注解

C++ 注解（P3394，`[[=value]]` 语法）允许把**任意编译期常量值**附加到任意类型 / 成员 / 命名空间上。注解通过 P2996 反射读取：

```cpp
constexpr auto offers = std::meta::annotations_with<Async::Offers>(^^StaticPoolScheduler);
// offers 是一个 array of std::meta::info，每个对应一个 Offers 标签
```

简单说，注解是**值级别的元信息**，反射是**读取它的入口**。两者结合，提供了 C++ 历史上第一次"**类型自带可被结构化查询的语义标签**"的能力。

### 注解 vs concept

注解和 concept 都在描述"这个类型有什么能力"。区别是：

| 维度 | concept | annotation |
|------|------|------|
| 检查时机 | 编译期，调用点验证 | 编译期，可被代码反射查询 |
| 表达能力 | "is-a"（满足条件） | "has-meta"（携带值） |
| 自动 vs 手动 | 编译器自动检查 | 反射代码自行枚举 |
| 失败模式 | 模板替换失败 | static_assert / 运行时弹出 |

它们是**互补**的，不是替代：

- concept 回答"满足吗"
- annotation 回答"声称什么"

spec 的设计是：**双链验证**。例如 `StaticPoolScheduler` **声称** `Offers::Bulk`，concept `BulkScheduler` 验证它**确实**满足 bulk 接口。如果声称与实际不符，验证器**编译期失败**。

```
       ┌─ 声称（annotation）──┐
       │ [[=Offers::Bulk]]    │
       │                       │
StaticPoolScheduler            │  ◀── 验证器在这里 static_assert 两边一致
       │                       │
       └─ 实现（concept）─────┘
         BulkScheduler<S>
```

这就是 spec 中 "**可证明的分类**" 的字面含义：一个类型的分类（"它是 BulkScheduler"）不再只是约定，而是被反射 + concept 双向锁定的、**编译期可证明**的事实。

### 范畴学侧的解读

如果你愿意把这层结构形式化，可以这样看：

- 类型构成一个范畴 𝒯
- 注解构成一个 functor `A : 𝒯 → Sets`（每个类型映到它的注解集合）
- concept 构成一个 functor `C : 𝒯 → Bool`（每个类型映到"满足吗"）
- 验证器 = 一个**自然变换** `α : A → C`，证明"声称的标签对应的能力，确实在 concept 下成立"

这个抽象有用吗？看你怎么用。它至少告诉你两件事：

1. **注解与 concept 必须同步变化。** 改一个，另一个也得改，否则自然变换断裂。
2. **注解是 spec 的可执行版本。** spec 说"`StaticPool` 提供 bulk"——这句话过去只是 markdown 文字；有了注解，它变成可被工具链查询、验证、生成代码的**值**。

### 为什么这件事重要

回看 Lesson 8 的报错示例：

```
error: parallel_for requires BulkScheduler;
       Inline::scheduler is not a BulkScheduler.
       Concept 'OffersBulk' is missing from the annotations.
```

这条错误消息**精确指出**：(1) pattern 期望什么，(2) 用户提供了什么，(3) 缺失什么注解。它**不**是泛泛的 "concept failed"——它能这样精确，是因为注解本身被反射读了一次，**让编译器知道**用户类型的"自述"。

这是 spec 的"P2996 反射"那一行**真正的工业意义**：它不是为了花哨，是为了让**错误消息从"模板替换失败的意大利面"变成"人类可读的契约违反"**。

Lesson 10 我们会亲手写一个迷你验证器，把上面这个"声称 ↔ 实现"循环走通。

---

## Lesson 10 · 注解 + 反射验证器 —— 让"声称"被编译期审计

**主题：** 亲手实现一个小型 `Async::verify_scheduler<S>()`，它把 S 的注解读出来、对照 concept 做"声称 ↔ 实现"双向检查。
**对应 spec：** `01-foundations.md` §8；`07-extension.md` §2；`09-synthesis.md` §3.1。

> **本讲想解决的根本问题：** Interlude IV 把"双链验证"讲成了抽象。本讲让你**亲手**写一遍。当你自己写完这 30 行 consteval 代码，你会明白 spec 中所有"编译期错误信息"的来源——它们不是魔法，是反射 + concept + static_assert 的工程组合。

### §a 主流程更新

写一个 `verify_scheduler<S>()` 函数（`consteval`），扫描 `S` 的 `Offers` 注解，逐个对照 concept：

```cpp
#include <Mashiro/Async/Foundations.h>
#include <experimental/meta>

namespace Async = Mashiro::Async;

template<class S>
consteval void verify_scheduler() {
    namespace m = std::meta;

    // 1. 必须先满足基础 Scheduler concept
    static_assert(Async::Concepts::Scheduler<S>,
                  "verify_scheduler: not a Scheduler");

    // 2. 读它的 Offers 注解
    constexpr auto offers = m::annotations_with<Async::Offers>(^^S);

    // 3. 逐个 Offers 标签做对照检查
    template for (constexpr auto o : offers) {
        constexpr auto kind = [:o:].kind;
        if constexpr (kind == Async::Offers::Kind::Bulk) {
            static_assert(Async::Concepts::BulkScheduler<S>,
                          "[[=Offers::Bulk]] claimed, but BulkScheduler concept not satisfied");
        } else if constexpr (kind == Async::Offers::Kind::Affine) {
            static_assert(Async::Concepts::AffineScheduler<S>,
                          "[[=Offers::Affine]] claimed, but AffineScheduler concept not satisfied");
        } else if constexpr (kind == Async::Offers::Kind::Parallel) {
            static_assert(Async::Concepts::ParallelScheduler<S>,
                          "[[=Offers::Parallel]] claimed, but ParallelScheduler concept not satisfied");
        } else if constexpr (kind == Async::Offers::Kind::Io) {
            static_assert(Async::Concepts::IoScheduler<S>,
                          "[[=Offers::Io]] claimed, but IoScheduler concept not satisfied");
        }
    }
}
```

调用它：

```cpp
struct MyScheduler {
    [[=Async::Offers{Async::Offers::Kind::Bulk}]]
    static auto schedule() { /* ... */ }
    // 但我"忘了"实现 schedule_bulk()
};

int main() {
    verify_scheduler<MyScheduler>();   // 编译期失败：claimed Bulk but not BulkScheduler
}
```

**你应当看到。** 编译器吐出：

```
error: static assertion failed: '[[=Offers::Bulk]] claimed, but BulkScheduler concept not satisfied'
  static_assert(Async::Concepts::BulkScheduler<S>, ...);
                ^~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
```

——这就是"可证明的分类"的实物。把 `MyScheduler` 补上 `schedule_bulk()` 后，`verify_scheduler<MyScheduler>()` 编译通过；如果你**移除** Offers 注解但保留 `schedule_bulk`，验证器**不会失败**（因为它只检查"声称的"，不检查"额外提供的"）。spec 的口径是这种"鼓励但不强制额外能力被注解"的态度——你想让别人发现你的能力，请注解；不想注解就接受"别人不会自动用你的能力"。

### §b 概念剖析

#### `template for` 与 P2996 splice

`template for (constexpr auto x : range)` 是 P2996 引入的**编译期 for-each**——它让你能在 `consteval` 函数里遍历反射结果。每次迭代 `x` 是一个 `std::meta::info`，需要用 `[:x:]` "splice" 出来还原为值或类型。

P2996 的 splice 语法是 `[:expr:]`：

- `[:^^T:]` ≡ `T`（类型 splice）
- `[:^^v:]` ≡ `v`（值 splice）
- `[:annotation_info:]` ≡ 注解对象的值

我们的验证器用 `[:o:].kind` 把注解对象**还原**出来，读它的 `kind` 字段。

#### `annotations_with` 的过滤效果

```cpp
m::annotations_with<Async::Offers>(^^S)
```

返回**类型恰为 `Async::Offers`**的注解集合，自动过滤掉其他注解（例如 `Async::Cancellable`、`Async::Allocates`）。这是 P3394 + P2996 设计上的便利：你不需要先拿全部注解再 isinstance 过滤。

#### 错误消息工程

注意我们 `static_assert` 的消息**精确包含**：

- 声称了什么（`[[=Offers::Bulk]] claimed`）
- 期望什么（`BulkScheduler concept not satisfied`）

这是 Mashiro spec **正式要求**的错误消息规范（`01-foundations.md` §8）：

> Verification diagnostics shall name (a) the offending annotation, (b) the failing concept, in a single static_assert message.

为什么？因为 C++ 模板错误的传统输出**几乎不可读**。Mashiro 用注解 + 反射换来的最具价值的东西，**就是好的错误消息**。这条规范让"诊断质量"成为 spec 的一等公民。

#### "扩展注解" 的开放性

实际 `01-foundations.md` 给的 `Offers::Kind` 是一个**封闭枚举**：Bulk / Affine / Parallel / Io / 其他几项。但 spec **允许**第三方扩展：你可以引入自己的注解类型（例如 `MyCompany::Offers`），写自己的验证器。

这是 L7 (extension layer) 的 promise：**框架不是封闭世界**。spec 没有规定"所有 scheduler 必须只用 Mashiro 提供的注解"——它只规定"如果你用 Mashiro 注解，必须通过 Mashiro 验证器；其他注解你自己负责"。

### §c 思考题

1. 把 Lesson 4 的 `StaticPoolScheduler` 放进 `verify_scheduler<>()`，画出验证器走过的所有 `static_assert` 路径。
2. 把验证器扩展到检查 `Allocates::Where` 注解：声称 `OpState` 时，需要什么 concept 来核实？（提示：spec 没有给这个 concept——这是一个**设计题**。）
3. P2996 `template for` 在 GCC / Clang / MSVC 的实现进度不一。如果只有 `clang-p2996` 支持，spec 提供了什么 fallback？查 `01-foundations.md` §8 给答案。
4. 假设你在公司内部 fork 出一个 `MyOffers` 注解扩展。请给它写一个验证器骨架，说明你怎样**与** Mashiro 的 `verify_scheduler` 共存（不冲突）。

---

## Lesson 11 · 自定义 VkComputeScheduler —— 把异步框架接进 Vulkan

**主题：** 综合 L2 / L7 知识，给 Vulkan compute queue 写一个最小调度器，让它接进 sender / Task 词汇。
**对应 spec：** `02-backends.md` §7（"Custom backends"）；`07-extension.md` §1–§3；`09-synthesis.md` §4。

> **本讲想解决的根本问题：** 课程项目的最终目标是把异步框架用于一个 Vulkan 渲染器。要让 `co_await dispatch_compute(...)` 看起来像 `co_await delay(100ms)` 那样自然，必须把 Vulkan 的 `vkQueueSubmit + VkFence` 包成一个 scheduler。这一讲是终点应用，也是 spec 扩展点的实战。

### §a 主流程更新

`examples/11_vk_compute.cpp`（骨架，省略 Vulkan boilerplate）：

```cpp
#include <Mashiro/Async/Foundations.h>
#include <Mashiro/Async/Coro.h>
#include <vulkan/vulkan.h>

namespace Async = Mashiro::Async;

struct VkComputeScheduler {
    VkDevice device;
    VkQueue  compute_queue;
    uint32_t queue_family;

    // 1. operation state
    template<class Receiver>
    struct OpState {
        VkComputeScheduler sched;
        Receiver           rcv;
        VkFence            fence{VK_NULL_HANDLE};

        void start() noexcept {
            VkFenceCreateInfo fci{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
            vkCreateFence(sched.device, &fci, nullptr, &fence);
            VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
            vkQueueSubmit(sched.compute_queue, 1, &si, fence);

            // 等 fence 完成的工作让 Platform polling 线程做
            Async::Backend::Platform::wait_fence(
                fence,
                [r = std::move(rcv)]() mutable {
                    stdexec::set_value(std::move(r));
                });
        }
    };

    // 2. sender
    struct Sender {
        using sender_concept = stdexec::sender_t;
        using completion_signatures = stdexec::completion_signatures<
            stdexec::set_value_t(),
            stdexec::set_stopped_t(),
            stdexec::set_error_t(std::exception_ptr)
        >;
        VkComputeScheduler sched;

        template<class R>
        auto connect(R rcv) const { return OpState<R>{sched, std::move(rcv)}; }
    };

    [[=Async::Offers{Async::Offers::Kind::Affine}]]
    [[=Async::Cancellable{}]]
    [[=Async::Allocates{Async::Allocates::Where::External}]]
    auto schedule() const noexcept { return Sender{*this}; }

    friend bool operator==(VkComputeScheduler const& a, VkComputeScheduler const& b) noexcept {
        return a.device == b.device && a.compute_queue == b.compute_queue;
    }
};

// 编译期验证
static_assert(Async::Concepts::Scheduler<VkComputeScheduler>);
static_assert(Async::Concepts::AffineScheduler<VkComputeScheduler>);
// （用 Lesson 10 的 verify_scheduler 一次性走完所有 Offers）
```

用法：

```cpp
Async::Task<void> render_frame(VkComputeScheduler vk) {
    co_await Async::continues_on(vk);                    // 跳到 compute queue 上
    record_and_submit_compute_commands();
    co_await vk.schedule();                              // 等 fence
    std::println("frame done");
}
```

**你应当看到（在有 Vulkan 设备时）。** 提交 dispatch、等 fence、回到协程——所有这些**用 sender 词汇表达**，不再有"提供 callback 给某个 listener"的丑陋形态。

### §b 概念剖析

#### Scheduler 的最小接口契约

任何一个 scheduler 类型 `S` 必须提供：

1. `schedule() -> sender` —— 返回一个 sender，对它 connect/start 等价于"在 S 上调度执行"
2. `operator==(S, S) -> bool` —— equality（让 stdexec 能判断"是否同一个调度器"）
3. 可被 copy（廉价 copy；scheduler 不存状态，只存引用）

注意 scheduler **本身不分配资源**——所有"线程池"、"compute queue" 的所有权都在 backend 对象里；scheduler 只是一个**句柄**。`VkComputeScheduler` 持有 `VkDevice + VkQueue + queue_family`，全是按值传递的 handle，copy 廉价。

#### 注解三联：声明你是谁

我们的 `VkComputeScheduler::schedule()` 上挂了三条注解：

```cpp
[[=Async::Offers{Async::Offers::Kind::Affine}]]
[[=Async::Cancellable{}]]
[[=Async::Allocates{Async::Allocates::Where::External}]]
```

逐条释义：

- `Offers::Affine` —— 完成总是发生在同一队列（compute queue 串行）；这是它**不**是 `Parallel` 的理由
- `Cancellable` —— 它的 op-state 支持外部 stop_token（虽然 Vulkan fence 的取消很 tricky——见思考题 #2）
- `Allocates::Where::External` —— 资源由 Vulkan 持有，**不在** sender op-state 内

这三条让 pattern 层、domain 重写器、Allocator 检查器**自动认识**这个 backend——你不需要去任何文档里登记 "VkComputeScheduler 是 Affine"。

#### 桥到 Platform polling

`Async::Backend::Platform::wait_fence(fence, cb)` 是 spec 给 OS-level 等待源的统一桥。它在 `02-backends.md` §5 中规定：

- 在 Linux 上用 `vkGetFenceStatus` + epoll wakeup（或 `vkWaitForFences` 在专门线程上）
- 在 Windows 上同理，配合 IOCP
- 完成时在 Platform 后台线程调用 `cb`

这条 API 把"OS 异步原语"统一抽象成"给个 callback，到点就调"——sender 层的 op-state 用它把 `set_value` 推下去。这与 io_uring、kqueue、定时器、文件描述符就绪等**所有**等待场景同形。

#### 为什么 `co_await continues_on(vk)` 而不是 `on(vk, ...)`

回到 Lesson 4 的对比：

- `on(sched, s)` —— **启动**整条 `s` 在 sched 上
- `continues_on(sched, s)` —— `s` 启动在原 scheduler，**完成后**搬运到 sched

在 Vulkan 上，绝大多数时候你想要的是后者：先在 CPU 协程上下文准备数据，**然后**跳到 compute queue 提交、等 fence。这种"局部跳转"是 `continues_on` 的设计本意。

### §c 思考题

1. Vulkan fence **不**直接支持取消（一旦 `vkQueueSubmit` 出去，CPU 端无法撤回）。如何在 `VkComputeScheduler` 的 op-state 里实现"协作式取消"？提示：在等 fence 的同时 poll stop_token。
2. 把 `VkComputeScheduler` 实现成 `BulkScheduler`：`schedule_bulk(N, fn)` 在 Vulkan 上的语义应当是什么？（提示：`vkCmdDispatch(N/group, 1, 1)`，但 `fn` 必须翻译成 SPIR-V——这是一个**大**难题，spec 没有解决方案，欢迎你做开放探索。）
3. spec 把"sender 不分配资源"当作**软性**约束（"resources 在 backend"）。当 backend 是 GPU 时，**所有**资源都在 driver 里，这条约束有不同意义吗？
4. 给 `VkComputeScheduler` 设计一个 `domain_type`，把 `Async::just(buf) | Async::then(upload_to_gpu)` 重写成"在 staging buffer 里直接走 transfer queue"。它是 Lesson 9 domain 机制的真实工业用例——请简述设计要点。

---

## Lesson 12 · 综合实验 —— 把所有抽屉打开

**主题：** 最后一讲。把 Lesson 1–11 学到的全部装备组合成一个**有意义**的小程序——一个最简单的"异步资源管线"，覆盖 sender / Task / Stream / Nursery / timeout / pattern / domain / 自定义 scheduler 的全套词汇。
**对应 spec：** 全部章节（这是把 spec 当地图用的检阅）。

> **本讲想解决的根本问题：** 学完所有词汇，最容易出现的状态是"会用每个零件，但拼不出整机"。本讲不引入新概念，而是给你一个**整机**的范本，让你看清楚"这些零件该如何咬合"。

### §a 主流程更新

`examples/12_capstone.cpp`：一个"GPU 资源加载器"。它：

1. 从磁盘**并行**读 N 个图像文件（IO scheduler）。
2. 用 `parallel_for` **并行**解码 PNG（StaticPool）。
3. 上传到 GPU 的 staging buffer（自定义 `VkUploadScheduler`）。
4. 触发 compute dispatch 生成 mipmap（Lesson 11 的 `VkComputeScheduler`）。
5. 整个过程被 nursery 守护；某一步超过 5 秒 timeout 就重试一次；中途按 Ctrl-C 整条干净停下。

```cpp
#include <Mashiro/Async/Foundations.h>
#include <Mashiro/Async/Coro.h>
#include <Mashiro/Async/Structured.h>
#include <Mashiro/Async/Patterns.h>
#include <Mashiro/Async/Backend/Io.h>
#include <Mashiro/Async/Backend/StaticPool.h>
#include "VkUploadScheduler.h"
#include "VkComputeScheduler.h"

namespace Async   = Mashiro::Async;
namespace Backend = Mashiro::Async::Backend;
namespace Pat     = Mashiro::Async::Patterns;
using namespace std::chrono_literals;

Async::Task<int> load_all(std::vector<std::string> paths,
                          VkUploadScheduler upload,
                          VkComputeScheduler vk_compute) {
    auto io      = Backend::Io::scheduler();
    auto pool    = Backend::StaticPool::scheduler();
    std::atomic<int> done{0};

    co_await Async::open_nursery([&](Async::Nursery& n) -> Async::Task<void> {
        Async::AsyncQueue<RawBytes>   raw_q{8};
        Async::AsyncQueue<Image>      img_q{8};

        // stage 1: 并行读
        n.spawn(Pat::parallel_for(io, 0, (int)paths.size(), [&](int i) -> Async::Task<void> {
            auto bytes = co_await Backend::Io::read_file(paths[i]);
            co_await raw_q.push(std::move(bytes));
        }));

        // stage 2: 并行解码
        n.spawn([&]() -> Async::Task<void> {
            MASHIRO_FOR_CO_AWAIT(RawBytes b, Async::from_queue(raw_q)) {
                auto img = co_await Async::on(pool, Async::just(std::move(b))
                                                  | Async::then(decode_png));
                co_await img_q.push(std::move(img));
            }
        }());

        // stage 3 + 4: 上传 + mipmap，带 timeout + retry
        n.spawn([&]() -> Async::Task<void> {
            MASHIRO_FOR_CO_AWAIT(Image img, Async::from_queue(img_q)) {
                auto one_image =
                    Async::on(upload, Async::just(std::move(img)) | Async::then(upload_image))
                    | Async::let_value([&](GpuImage gimg){
                        return Async::on(vk_compute,
                            Async::just(gimg) | Async::then(generate_mipmap));
                    })
                    | Async::timeout(5s)
                    | Async::retry(1, [](auto&){ return true; });

                co_await std::move(one_image);
                done.fetch_add(1, std::memory_order_relaxed);
            }
        }());

        co_return;
    });

    co_return done.load();
}

int main() {
    std::vector<std::string> paths = collect_image_paths();
    auto vk = init_vulkan();
    auto [n] = stdexec::sync_wait(load_all(paths, vk.upload, vk.compute)).value();
    std::println("loaded {} images", n);
}
```

**你应当看到。** 三条 stage 在三个不同 backend 上**真并行**：磁盘读和 PNG 解码在不同线程上 interleave，GPU 上传与 mipmap 在 Vulkan queue 上排队执行；中途任何 stage 抛异常，nursery 把其他 stage 干净停下；按 Ctrl-C 触发外部 stop token，整个 sync_wait 在毫秒内退出。

### §b 概念剖析（综合复盘）

这一节不再讲新概念，我们用一张表把你 1–11 讲学到的**全部词汇**与 12 号程序里**具体**出现的位置对一遍：

| 词汇 | 出现位置 | spec 章节 |
|------|------|------|
| `Async::just / then / let_value` | 各 stage 内 | `03-adaptors.md` §2 |
| `on(sched, s)` | stage 2、3 | §10 |
| `Task<T>` 协程 | 顶层 + 各 spawn | `04-coroutine-tasks.md` §2 |
| `co_await` sender | 处处 | §3 |
| `AsyncQueue<T>` | raw_q / img_q | §5.4 |
| `from_queue` + `MASHIRO_FOR_CO_AWAIT` | stage 2、3 | §5 |
| `Nursery` + `open_nursery` | 顶层 | `05-structured.md` §2 |
| `n.spawn(...)` | stage 1/2/3 | §2 |
| `timeout(5s, s)` | stage 3 | `03-adaptors.md` §7 |
| `retry(1, pred)` | stage 3 | §8 |
| `Pat::parallel_for` | stage 1 | `06-patterns.md` §2 |
| `Backend::Io / StaticPool` | scheduler 来源 | `02-backends.md` §4–§5 |
| 自定义 `VkUploadScheduler / VkComputeScheduler` | 顶层入参 | §7 + `07-extension.md` |
| 注解 `Offers / Cancellable / Allocates` | 自定 scheduler 上 | `01-foundations.md` §3 |
| concept 验证 | 编译期 `static_assert` | §6 + §8 |
| 取消传播（Ctrl-C → stop token → nursery → 各 stage） | 运行期 | `08-cross-cutting.md` §1 |
| forward progress（每 backend 不同） | 隐式 | `01-foundations.md` §7 |

**这就是 spec 的全景。** 任何"工业级异步功能需求"——从 Web 后端到游戏引擎——都可以被这张表上的零件组装出来。你的工作不是发明新词，而是**理解每个词的位置与契约**，然后写出"读起来像散文、跑起来像并行编排"的代码。

### §c 思考题 —— 期末作业

不再是单题，是一个**开放项目**。任选其一：

1. **"日志聚合器"。** 监听 10 个文件描述符的滚动日志，按时间排序，写入一个聚合文件。要求：通过 nursery 守护、用 Stream 表达、按"每 100ms 滚动批一次"的方式 batch、能优雅响应 Ctrl-C。
2. **"分布式计算客户端"。** 模拟向 N 个远程 worker 发请求、收结果、聚合。要求：用 `when_all` 聚合、用 `timeout` 加固每个远程调用、用 `retry` 容忍偶尔失败。
3. **"自定义 Vk transfer scheduler"。** 在 Lesson 11 的基础上把 transfer queue 也包成 scheduler，与 compute scheduler 协作做"上传 → mipmap → 抓回 CPU 验证"的完整三段。

每个项目都应当：

- 在你的 README 里**画出 sender 树**与 nursery / pattern 的嵌套关系
- 跑过 `Diagnostics::AllocCheck` 与 `Diagnostics::CancelProbe` 各一次
- 把"每个 stage 的 forward progress、cancellability、allocation"列成一张表（可以照抄本讲表格的格式）

恭喜你走完了 12 讲。你现在拥有的不是一个 framework 的用法手册，是一**张地图**：你知道每个抽象在 spec 中的坐标，知道为什么它在那里，知道改它要付什么代价。剩下的事，是把它推进你自己的项目。

---

## 附录 A · 词表（按字母序）

> 仅包含本讲义出现的、属于 Mashiro 异步框架专用的名词。C++ 标准词汇（`co_await`、`std::thread` 等）不列。

| 名词 | 释义 | 首次出现 / 主讲 |
|------|------|------|
| **adaptor** | 把 sender 变成另一个 sender 的算子（`then`、`when_all`、`timeout` 等） | L1, L7 |
| **AffineScheduler** | 完成总是发生在某固定执行上下文（线程、队列）的 scheduler | L4, L11 |
| **AllocCheck** | 诊断工具，统计协程帧 / op-state 分配次数 | L4 |
| **AsyncQueue<T>** | 有界异步队列；`push` 满则挂起，`pop` 空则挂起；spec L4 唯一原语 | L5 |
| **backend** | L2 层的执行资源（Inline / StaticPool / TBB / Platform / Io / 自定义） | L4 |
| **BulkScheduler** | 提供 `schedule_bulk(N, fn)` 的 scheduler | L4, L8 |
| **CancelProbe** | 诊断工具，注入 stop_token 检验 adaptor 取消行为 | L7 |
| **completion_signatures** | sender 描述它将以什么形态完成（set_value / set_error / set_stopped） | L1, L5 |
| **continues_on(sched, s)** | adaptor，让 s 完成后切到 sched 上 | L4 |
| **counting_scope** | P3149 / stdexec 提供的等待 scope 原语；Nursery 的底座 | L6 |
| **domain** | sender 树的持有者，可在编译期重写树 | L9 |
| **forward progress guarantee** | concurrent / parallel / weakly_parallel；调度器的进展承诺 | L4 |
| **from_queue** | 把 `AsyncQueue<T>` 包成 `Stream<T>` | L5 |
| **HALO** | Heap Allocation eLision Optimization；协程帧落栈优化 | L2, L4 |
| **IoScheduler** | 支持 file / socket / fence 等 OS 等待源的 scheduler | L4, L11 |
| **let_value** | monadic bind；从上游 sender 拿 value 再生新 sender | Interlude II, L11 |
| **LinkedScope<Parent>** | nursery 的一种形态，与父对象生命周期绑定 | L6 |
| **MASHIRO_FOR_CO_AWAIT** | C++26 `for co_await` 语法糖的临时宏 | L5 |
| **Nursery** | 结构化并发的"作用域容器"；spawn 进来的 task 必须在它关闭前结束 | L6 |
| **on(sched, s)** | adaptor，让整条 s **启动**在 sched 上 | L4 |
| **op-state** | sender connect 出来的"操作状态机"；start 启动，完成后调 receiver | L1, L9 |
| **Offers** 注解 | 类型上挂的 [[=Offers{Kind}]]；声明 scheduler 提供哪些能力 | L4, Interlude IV |
| **ParallelScheduler** | 满足 parallel forward progress 的 scheduler | L4 |
| **parallel_for** | L6 pattern，把范围迭代分给 BulkScheduler 并行执行 | L8 |
| **pattern** | L6 层；由 L2/L3/L4 组合而成的命名编排（parallel_for / pipeline / ...） | L8 |
| **pipeline** | L6 pattern，把若干 stage 串联，每 stage 可在不同 scheduler 上 | L8 |
| **race(a, b)** | adaptor，等任一完成即整体完成，另一被 stop | L7, Interlude II |
| **receiver** | sender 完成时回调的接收方；有 set_value/set_error/set_stopped 三入口 | L1 |
| **retry(N, pred)** | adaptor，N 次重试，pred 决定哪种错误重试 | L7 |
| **scheduler** | 执行上下文的句柄；廉价 copy，提供 schedule() | L4, L11 |
| **sender** | 描述"未来某时刻会完成"的对象；connect+start 后开始执行 | L1 |
| **Stream<T>** | "sender-of-optional"；可重入的 sender 形态；`for co_await` 取值 | L5 |
| **stop_token** | 协作式取消的句柄；流经 receiver env | L3, L7 |
| **Supervised<Policy>** | 监督树风格 nursery；OneForOne / AllForOne / OneForAll | L6 |
| **Task<T>** | 异步协程的返回类型；co_await 一个 sender / Task | L2 |
| **timeout(D, s)** | adaptor，超时即 set_stopped | L7 |
| **transform_sender** | domain 重写入口；编译期改写 sender 树 | L9 |
| **verify_scheduler<S>** | consteval 函数；检查 S 的 Offers 注解与对应 concept 是否对齐 | L10 |
| **when_all(a, b)** | adaptor，两者都完成方完成 | Interlude II, L12 |

---

## 附录 B · 与 spec 章节的双向索引

> 让你在任何一讲想"读源契约"时，能立刻找到 spec 的对应章节；也让你读 spec 时，能找到讲义中的对应实例。

### 从讲义 → spec

| 讲义 | 主要 spec 章节 |
|------|------|
| Lesson 1 (`just/then/sync_wait`) | `00-overview.md` §3; `01-foundations.md` §2 |
| Lesson 2 (`Task<T>`, HALO) | `04-coroutine-tasks.md` §§1–3; `00-overview.md` §4 |
| Lesson 3 (cancellation 主流程) | `08-cross-cutting.md` §1; `04-coroutine-tasks.md` §4 |
| Interlude I (taxonomy of async) | `00-overview.md` §6 |
| Lesson 4 (Inline vs StaticPool, continues_on) | `02-backends.md` §§3–4; `01-foundations.md` §6 |
| Lesson 5 (Stream, AsyncQueue, 反压) | `04-coroutine-tasks.md` §5; `09-synthesis.md` §§2.5, 2.8 |
| Interlude II (sender/Stream 范畴学) | `03-adaptors.md` §3 (laws) |
| Lesson 6 (Nursery / Scope / Supervised) | `05-structured.md` §§1–4; `08-cross-cutting.md` §1 |
| Lesson 7 (timeout / retry / race) | `03-adaptors.md` §§7–9; `08-cross-cutting.md` §4 |
| Interlude III (结构化并发哲学) | `05-structured.md` §1 (motivation) |
| Lesson 8 (parallel_for / pipeline) | `06-patterns.md` §§1–4 |
| Lesson 9 (domain / transform_sender) | `02-backends.md` §6; `07-extension.md` §3 |
| Interlude IV (注解 + 反射) | `01-foundations.md` §8; `07-extension.md` §2 |
| Lesson 10 (verify_scheduler) | `01-foundations.md` §8; `09-synthesis.md` §3.1 |
| Lesson 11 (VkComputeScheduler) | `02-backends.md` §7; `07-extension.md` §§1–3 |
| Lesson 12 (capstone) | 全部章节 |

### 从 spec → 讲义

| spec 文件 / 章节 | 讲义讲解位置 |
|------|------|
| `00-overview.md` §3 (sender 词汇) | L1 |
| `00-overview.md` §4 (协程任务) | L2 |
| `00-overview.md` §6 (异步模型分类) | Interlude I; L5 §b |
| `00-overview.md` §7 (结构化并发) | L6; Interlude III |
| `01-foundations.md` §2 (re-exports) | L1 |
| `01-foundations.md` §3 (注解类型) | L4 §b; Interlude IV |
| `01-foundations.md` §6 (scheduler concepts) | L4 §b; L8 §b |
| `01-foundations.md` §7 (forward progress) | L4 §b 末段 |
| `01-foundations.md` §8 (验证器规范) | L10 全篇 |
| `02-backends.md` §3 (Inline) | L4 §a |
| `02-backends.md` §4 (StaticPool / schedule_bulk) | L4 §a; L8 §b |
| `02-backends.md` §5 (Platform wait_fence) | L11 §b |
| `02-backends.md` §6 (domain 重写) | L9 全篇 |
| `02-backends.md` §7 (自定义 backend) | L11 全篇 |
| `03-adaptors.md` §2 (then / when_all 基本) | L1; Interlude II |
| `03-adaptors.md` §3 (adaptor laws) | Interlude II |
| `03-adaptors.md` §§7–9 (timeout / retry / race) | L7 全篇 |
| `03-adaptors.md` §10 (continues_on / on) | L4 |
| `04-coroutine-tasks.md` §§1–3 (Task<T>) | L2 |
| `04-coroutine-tasks.md` §4 (取消) | L3 |
| `04-coroutine-tasks.md` §5 (Stream / AsyncQueue) | L5 |
| `05-structured.md` §1 (动机) | Interlude III |
| `05-structured.md` §§2–4 (Nursery / LinkedScope / Supervised) | L6 |
| `06-patterns.md` §§1–4 (parallel_for / pipeline) | L8 |
| `07-extension.md` §1–§3 (扩展点) | L9; L10; L11 |
| `08-cross-cutting.md` §1 (取消传播) | L3; L6 §b; L7 §b |
| `08-cross-cutting.md` §3 (分配口径) | L4 §b |
| `08-cross-cutting.md` §4 (错误模型) | L7 §c |
| `09-synthesis.md` §§2.5, 2.7, 2.8 | L5; L8 §c #1; L9 §c #2 |
| `09-synthesis.md` §3.1 | L10 §b |

---

## 附录 C · 阅读路线建议

> 不同读者背景应当走不同的路。下面三条路线推荐了"先读什么"。

### 路线 1 · 应用工程师（"我只想会用"）

L1 → L2 → L3 → L6 → L7 → L12。

跳过 Interlude II、IV 与 L9、L10、L11。读完后你能写主流程、读懂错误消息、用 patterns 加固。代价：你不能改 backend、不能写 domain；遇到性能问题需要请框架团队介入。

### 路线 2 · 框架贡献者（"我要给框架加 backend / pattern / adaptor"）

L1 → L2 → L4 → L5 → L7 → L8 → L9 → L10 → L11 → L12，**每一讲都做思考题**。

Interlude I–IV 必读。读完后你能：

- 写自定义 scheduler 并通过验证器
- 写新 pattern 并在 spec 中文档化
- 写 domain 重写器并通过 round-trip 测试
- 给 spec 提变更并理解它的连带影响

### 路线 3 · 课程旁听 / 研究兴趣（"我想看清楚为什么这样设计"）

按章节顺序通读（含全部 Interludes）。然后做思考题 **#3 和 #4**（开放题），写一篇 1500 字左右的"我对结构化并发 / 注解作为可证明分类的看法"。

这条路线产出的不是代码，是**思路**——你会在未来设计任何系统时不自觉地用到结构化作用域、双链验证、代数律重写这些工具。

---

## 课程结语

写到这里 12 讲完成了。回头看，这门课只做了一件事：把 spec 里那张**横向分层图**翻译成**纵向一条切片**，让你能沿着切片一边敲键盘、一边看见每一层在你手里立起来。

最后留一句话——

> **异步编程难，是因为它把"时间"显式化。**
> **而结构化并发好，是因为它把"时间"重新装进了"作用域"。**
> **Mashiro 的 spec 没有改变这件事的难度，只是给"时间"找了一个像"花括号"一样可读的容器。**

希望这套讲义对你有帮助。祝实验顺利。

---

# Unit II · Labs（动手实验单元）

> **本单元的位置。** Unit I（L1–L12）以"读 + 想"为主，每讲尾的思考题是开放式的。Unit II 把每讲对应一个 **可提交的 lab**：明确的任务、可机器化的验收标准、提示链、参考解骨架。它是 Unit I 的"实验配套"，**不**引入新概念。
>
> **怎么用这些 lab。** 推荐节律：读完 Lesson N 当晚做 Lab N。每个 lab 期望 1–3 小时；若超过 4 小时还卡在某一步，请回看 Lesson N 的 §b 概念剖析，或翻 spec 对应章节。
>
> **关于"参考解骨架"。** 每个 lab 给出的不是完整答案，是**关键点的脚手架**——剩下的 20% 需要你自己填，包括 includes、辅助函数、错误处理。这是**故意**的设计：fill-in-the-blank 比 copy-paste 更接近工程实战。
>
> **关于自动化检查。** 每个 lab 最后给出一段 `// CHECK:` 注释序列——这是 spec 推荐的轻量化"行为断言"风格（仿 `FileCheck`）：把期望输出写成注释，配套一个 200 行的 Python 脚本 `tools/lab_check.py` 把它跑成 PASS/FAIL。脚本本身不在本课范围，但格式约定你应当遵守。

---

## Lab 1 · 第一条 sender 主流程

**前置：** Lesson 1。**预计时长：** 30 分钟。

### 任务

实现 `fizzbuzz_sender(n)`：返回一个 sender，启动后顺序打印 1..n 的 FizzBuzz 结果，最终以 set_value 完成（返回 `void`）。要求**只用** `just / then / let_value / sync_wait`——不能用 `Task<T>`，不能用任何 backend（默认 inline 即可）。

### 起点代码

```cpp
#include <Mashiro/Async/Foundations.h>
#include <print>
#include <string>
namespace Async = Mashiro::Async;

auto fizzbuzz_sender(int n) {
    // TODO: 构造一条 sender 表达式，顺序打印 1..n 的 FizzBuzz 输出
    return Async::just();
}

int main() {
    stdexec::sync_wait(fizzbuzz_sender(15));
}
```

### 验收标准

1. 程序正确输出 `1 2 Fizz 4 Buzz Fizz 7 8 Fizz Buzz 11 Fizz 13 14 FizzBuzz`（每个数字 / 词独占一行）。
2. 函数签名**未出现** `Task<` 或 `co_` 关键字（用 `grep` 检查）。
3. 编译时无任何 warning（`-Wall -Wextra -Wpedantic`）。

### 提示链

- **卡 5 分钟**：sender 表达式可以用 `let_value` 形成递归 / 链式扩展。
- **卡 15 分钟**：你可以用一个返回 sender 的辅助函数 `step(int i, int n)`，让它在内部决定"是 set_value 结束还是继续 let_value"。
- **卡 30 分钟**：这是一个**故意有点别扭**的练习——它让你体会"没有协程时，sender 表达 N 步循环要付出多大代价"。比较你 Lab 2 用 Task 重写后的清洁度。

### 参考解骨架

```cpp
auto step(int i, int n) -> /* 推导 */ {
    return Async::let_value(Async::just(i), [n](int i) {
        // print 这一步
        if (i % 15 == 0)      std::println("FizzBuzz");
        else if (i % 3 == 0)  std::println("Fizz");
        else if (i % 5 == 0)  std::println("Buzz");
        else                  std::println("{}", i);
        // 决定是结束还是继续
        if (i == n) return Async::just();
        return /* TODO: 把下一步包成 sender 返回 */;
    });
}
```

**陷阱提示**：`step` 的返回类型不能是 "either Async::just() or 递归 step(i+1)"——这两者类型不同。你需要用 `Async::let_value` 把"分支"统一到同一个签名（参见 `09-synthesis.md` §2.3）。

### CHECK 注释

```cpp
// CHECK: 1
// CHECK-NEXT: 2
// CHECK-NEXT: Fizz
// ... (略)
// CHECK-NEXT: FizzBuzz
// CHECK-NOT: error
// CHECK-NOT: warning
```

---

## Lab 2 · 把 Lab 1 用协程重写

**前置：** Lesson 2。**预计时长：** 20 分钟。

### 任务

重写 Lab 1，用 `Async::Task<void>` + `co_await` 表达。要求**只调用 `Async::just` 一次**（用于把整数提升为 sender），其余全部用 `co_await` / `for` 直接表达控制流。

### 起点代码

```cpp
Async::Task<void> fizzbuzz_task(int n) {
    // TODO
    co_return;
}
```

### 验收标准

1. 输出与 Lab 1 完全一致。
2. 函数体长度 ≤ 12 行（不含大括号）。
3. **没有**任何 `let_value` 调用。
4. 用 `Async::Diagnostics::AllocCheck` 检查：协程帧分配次数 ∈ {0, 1}（HALO 命中 = 0，未命中 = 1；两者都接受）。

### 提示链

- **卡 5 分钟**：协程让"分支统一类型"的问题消失——`co_await` 之后的代码就是普通 if/else。
- **卡 10 分钟**：写完后**逐行**对比 Lab 1。差了多少？想清楚为什么。这是 Lesson 2 §b 的核心：协程把控制流"还给了" C++ 自身。

### 参考解骨架

```cpp
Async::Task<void> fizzbuzz_task(int n) {
    for (int i = 1; i <= n; ++i) {
        int x = co_await Async::just(i);
        if (x % 15 == 0)      std::println("FizzBuzz");
        else if (x % 3 == 0)  std::println("Fizz");
        else if (x % 5 == 0)  std::println("Buzz");
        else                  std::println("{}", x);
    }
}
```

**反思题**（写进提交注释）：上面的 `co_await Async::just(i)` 看似多余（其实就是 `int x = i;`）。**为什么保留它**？提示：当你后续想把"这一步移到线程池"，这行会变成 `co_await Async::on(pool, Async::just(i))`——保留它让"未来的异步性"成为开放选项。这是结构化异步代码常见的留白技巧。

---

## Lab 3 · 给 fizzbuzz 加取消

**前置：** Lesson 3。**预计时长：** 45 分钟。

### 任务

把 Lab 2 改造成"接受 `Async::stop_token`，每打印 5 个就检查一次取消"。在 `main` 里用一个 `Async::StopSource` 配合 50ms 定时器触发外部取消，验证 fizzbuzz 在被取消后干净退出（不抛 `std::exception_ptr`、不留下漏打印的整数）。

### 起点代码

```cpp
Async::Task<void> fizzbuzz_cancellable(int n, Async::stop_token tok) {
    // TODO: 同 Lab 2，但每 5 步检查 tok.stop_requested()
}

int main() {
    Async::StopSource src;
    auto cancel_after = [&]{
        std::this_thread::sleep_for(50ms);
        src.request_stop();
    };
    std::thread t(cancel_after);
    auto opt = stdexec::sync_wait(
        Async::with_stop_token(fizzbuzz_cancellable(1'000'000, src.get_token()),
                               src.get_token())
    );
    t.join();
    // TODO: 验证 opt 是 nullopt（被取消，对应 set_stopped）
}
```

### 验收标准

1. 程序在 ~50ms 后退出（用 `time` 命令验证 < 200ms）。
2. `sync_wait` 返回 `std::nullopt`（被取消的 sender 不返回 value）。
3. 没有未捕获异常逃逸到 `main` 之外。
4. **正确抛 `Coro::stopped_signal`**：用 strace / try-catch 验证退出路径走的是 set_stopped 而不是 set_error。

### 提示链

- **卡 10 分钟**：`stopped_signal` 是一个**异常类型**——你在协程里 `throw Coro::stopped_signal{};`，协程的 promise 把它翻译成 `set_stopped`，sync_wait 把 `set_stopped` 翻译成 `nullopt`。
- **卡 25 分钟**：`stop_token` 通过 `with_stop_token` adaptor 注入到 receiver env；协程 promise 从 env 里读出来传给协程的局部变量（这就是为什么 `fizzbuzz_cancellable(n, tok)` 显式接收 `tok` 参数；spec 也支持通过 `current_stop_token()` 在协程内**隐式**取，但这是 Lesson 8 之后的玩法）。

### 反思

写完后请用 Lesson 7 §b 的"六行验收清单"打分（对自己写的 `fizzbuzz_cancellable` 做检查表自评）。这是闭卷复习。

---

## Lab 4 · 跨线程的代价

**前置：** Lesson 4。**预计时长：** 1 小时。

### 任务

写一个**对比性 benchmark**：同样把 1..1'000'000 累加，分别用三种实现：

1. **A**: 普通 `for` 循环（基线）
2. **B**: 协程 + `Inline::scheduler()`（每步 `co_await Async::just(i)`）
3. **C**: 协程 + 每 10'000 步 `co_await continues_on(pool)`（强制跨线程一次）

测量：

- 每种实现的运行时间（中位数，5 次跑）
- 每种实现的协程帧分配次数（用 `AllocCheck`）
- 每种实现的累加正确性（应当都是 500'000'500'000）

### 起点代码

```cpp
// 提供 3 个独立的 main 入口，或一个统一的 dispatcher。
// 用 std::chrono::steady_clock 测时。
```

### 验收标准

1. **B 比 A 慢，但慢不超过 3×**（HALO 应当让 B 接近 A）。
2. **C 比 B 显著慢**（至少 5×）——跨线程同步开销。
3. 提交时附一张表，列出三组测量值。
4. 在文档里**解释**：为什么 B 的协程帧分配次数应当是 1（顶层 Task），但内部 `co_await Async::just(i)` 的 op-state **不算分配**？

### 提示链

- **卡 30 分钟**：协程帧分配只发生在 `Task<T>` 的**调用**那一刻；内部 `co_await sender` 的 op-state 是协程帧的成员，不额外分配。这是 spec 8-cross §3 的口径。
- **卡 50 分钟**：C 之所以慢，是因为**真的**跨线程同步—— `continues_on` 在 worker 线程上 `set_value`，主协程在 sync_wait 上下文 resume，跨线程唤醒的代价至少几个 μs。乘以 100 次（1M / 10K），就是 ms 级开销。

### 反思（必答）

如果你把第 3 步改成"每 100 步" `continues_on`，时间会变成多少？写出**预测**，再跑出实测，对比。这是培养"性能直觉"的关键练习。

---

## Lab 5 · AsyncQueue 的反压实验

**前置：** Lesson 5。**预计时长：** 1.5 小时。

### 任务

写一个**显式可见反压**的实验：

- 生产者协程每 10ms 推一个数字（速率 ≈ 100/s）
- 消费者协程每 50ms 取一个数字（速率 ≈ 20/s）
- 队列容量 = 4
- 跑 5 秒，统计：生产者实际产出多少、消费者实际消费多少、生产者**有多少时间被 push 挂起**

### 起点代码

```cpp
struct Stats {
    int produced{0};
    int consumed{0};
    std::chrono::nanoseconds producer_blocked_total{0};
};
Async::Task<void> run_backpressure_demo(Stats& s);
```

### 验收标准

1. 在 5 秒结束时，`produced ≈ consumed`（差距 ≤ queue capacity = 4）。
2. `producer_blocked_total / 5s ≥ 70%`——大部分时间生产者被反压挂起。
3. 生产者的"被挂起"是**无显式同步原语**实现的（不能用 `condition_variable` 或 `mutex`）。

### 提示链

- 测量"被挂起时间"的最干净方式：在 `push()` 前后记时间戳，差值累加。
- 反压不需要你写任何代码——`AsyncQueue` 本身就把它做进了 `push`。这个 lab 是让你**观察**这件事，而不是实现它。

### 反思

把队列容量改成 100，重跑。`producer_blocked_total` 会变成什么？这告诉你**容量是反压敏感度的旋钮**——容量越大，反压越钝，内存占用越高。spec 中默认 256 是经过工业现场调过的折中（`04-coroutine-tasks.md` §5.4）。

### v0.3 变体：用 Channel\<T, Cap\> 重写（选作）

把上面的 `AsyncQueue` 换成 §5.6 的 `Channel<int, Bounded<4>>`：

```cpp
auto ch = Channel<int, Bounded<4>>{};

auto producer = [&] -> Task<void> {
    for (int i = 0; running; ++i) {
        auto t0 = clock::now();
        co_await ch.send(i);                              // 满则自动挂起
        s.producer_blocked_total += clock::now() - t0;
        s.produced++;
        co_await delay(10ms);
    }
    ch.close();
}();

auto consumer = [&] -> Task<void> {
    MASHIRO_FOR_CO_AWAIT(int v, ch.as_stream()) {         // 关闭时自动 EOF
        s.consumed++;
        co_await delay(50ms);
    }
}();
co_await when_all(producer, consumer);
```

**新增观察点**：把容量 strategy 改成 `Bounded<4, DropOldest>`，跑 5 秒；统计 `produced` 与
`consumed` 的差——这是 *丢弃次数*。这条变体把"反压"换成"实时窗口"——同一段代码，
*策略不同语义不同*，这是 §5.6 在教学上最关键的一点。

---

## Lab 6 · Nursery 的异常聚合

**前置：** Lesson 6。**预计时长：** 1 小时。

### 任务

写一个 nursery，里面 spawn 5 个 children；让其中 2 个**随机**抛出**不同**类型的异常（一个抛 `std::runtime_error`，另一个抛自定义 `MyError`），其他 3 个正常完成。在 nursery 关闭时，观察异常聚合行为：

- spec 选择"**抛第一个异常，把其他写进 `aggregated_errors` vector**"还是"**抛一个 `std::nested_exception`**"？查 `05-structured.md` §2 给答案。
- 实测 nursery 关闭后，main 接住的异常类型与内容。

### 验收标准

1. main 接住**恰好一个**异常（无 nested unwind）。
2. 接住的异常**类型与 spec 规定一致**——你需要去 spec 验证你的实现是否符合，并在提交注释里引用具体章节。
3. 那 3 个正常完成的 children 都被**及时 stop**（它们的工作循环在 ~10ms 内退出，而不是跑满全程）。

### 提示链

- 第 3 条验收需要你的 children 在循环里 `if (tok.stop_requested()) throw Coro::stopped_signal{};`——纯计算密集型 child 不会自动响应取消，需要协作。

### 反思（必答）

把场景改成"5 个 children 中**一个**抛异常，**其余 4 个**也都抛"——这次 5 个都抛。nursery 行为如何？哪一个被 main 接住？为什么 spec 选这个？

---

## Lab 7 · 实现你自己的 timeout

**前置：** Lesson 7。**预计时长：** 2 小时。

### 任务

不用 `Async::timeout`，**亲手**用 `race + delay` 拼出 `my_timeout(D, s)`。要求：

1. 满足 Lesson 7 §b 的六行验收清单全部 6 条。
2. 通过 `Diagnostics::CancelProbe` 测试。
3. **用注解声明**它的语义：

```cpp
[[=Async::Propagates{stop_to_inner = true, stop_to_self = true}]]
[[=Async::Completes{set_stopped_on_timeout = true}]]
template<class Rep, class Period, class S>
auto my_timeout(std::chrono::duration<Rep, Period> D, S s);
```

注解类型 `Propagates` 与 `Completes` 是 Mashiro spec 在 `01-foundations.md` §3 中为 adaptor 提供的——**它们让 adaptor 把自己的语义写进类型**，下游工具（domain 重写、文档生成、测试框架）可以反射查询。

### 起点代码

```cpp
template<class S>
auto my_timeout(std::chrono::nanoseconds D, S s) {
    return Async::race(
        std::move(s),
        Async::Backend::Platform::delay(D)
            | Async::then([](auto){ throw Async::Coro::stopped_signal{}; })
    );
}
```

——这是 §a 给的版本。请把它**包装成符合上面 6 条 + 注解要求**的"产品级"版本。

### 验收标准

1. 通过自动化测试集：
   - 测试 1：内部 sender 在 50ms 完成、timeout 100ms ⇒ 整体在 50ms 完成、set_value
   - 测试 2：内部 sender 在 1s 完成、timeout 100ms ⇒ 整体在 ~100ms 完成、set_stopped
   - 测试 3：内部 sender 在 50ms 抛异常、timeout 100ms ⇒ 整体在 50ms 完成、set_error
   - 测试 4：外部 stop_token 在 20ms 被请求 ⇒ 整体在 ~20ms 完成、set_stopped
2. 注解被 `verify_adaptor<my_timeout>` 验证通过。
3. 用 `CancelProbe` 注入"内部 sender 拒绝响应 stop" ⇒ `my_timeout` 在 spec 规定的 `cooperative_cancellation_window` 内强制收尾（这是 `08-cross-cutting.md` §1 的硬约束）。

### 提示链

- "强制收尾"在 spec 里有两种实现策略：(a) 给个最长等待窗口然后 `std::terminate`；(b) 把"未响应"作为 leak 计量但不杀。Mashiro 选了 (a)，窗口默认 5 秒可配。查 §1.4。

### v0.3 变体：Retry + Bulkhead + CircuitBreaker 防御纵深（选作）

把 §7.5/§7.6 的 *标准防御纵深四层* 用一个端到端微型例子串起来。

**场景**：一个外部 RPC 服务，名义 SLA 50ms，但 *5% 概率* 100ms 抖动 ＋ *0.1% 概率*
长时间 hang。你要做一个对调用方 *永不阻塞超过 200ms* 的安全包装。

```cpp
struct RpcEnv {
    Bulkhead<RpcTag, 16, Policy::Timeout<200ms>>         pool;
    CircuitBreaker<>                                     breaker{
        {.failure_threshold = 5, .open_window = 10s, .half_open_probes = 1}};
};

template <typename Req>
auto safe_rpc(RpcEnv& env, Req req)
    -> sender_of<set_value_t(Response), set_error_t(...), set_done_t()>
{
    return raw_rpc(std::move(req))                       // 最内层：实际调用
         | timeout(80ms)                                  //   ↑
         | retry(2, retry_pred::on_transient)             //   ↑
         | with_bulkhead(env.pool)                        //   ↑
         | env.breaker;                                   // 最外层：熔断
}
```

### 验收标准（v0.3 变体）

1. 在 *正常* 流量（5% 抖动）下，`safe_rpc` 成功率 ≥ 99.9%、p99 ≤ 100ms；
2. 在 *故障* 流量（100% RPC 返回错误）下，CircuitBreaker 应在 5 次失败内转 Open，此后
   10 秒内的调用应 *立即* set_error(BreakerOpen)——**不**到达 raw_rpc；
3. 在 *拥塞* 流量（500 并发）下，Bulkhead 把入口排队，*没有任何调用* 等待超过 200ms；
4. 用 `CancelProbe` 在 50ms 时注入外部 stop，应在 5ms 内 set_stopped 完成。

### 反思

调换四层顺序（例如把 retry 包在 bulkhead 外）会发生什么？写一个微型混沌测试证明
*正确顺序的必要性*——这就是 §7.5 末尾"反例 / 正例"的 *实验版*。

---

## Lab 8 · 自己写一个 pattern

**前置：** Lesson 8。**预计时长：** 2 小时。

### 任务

设计并实现 `Pat::scatter_gather(sched, items, fn, reducer)`：

- 在 `sched`（要求 `BulkScheduler + ParallelScheduler`）上并行对 `items` 的每个元素跑 `fn`
- 把每个返回值喂给 `reducer`（一个二元函数，如 `std::plus<>`）
- 最终返回归约结果

它是 `parallel_for` + `reduce` 的合并。

### 验收标准

1. concept 要求齐全：`BulkScheduler<Sched> && ParallelScheduler<Sched>` 在编译期被检查；不满足时给出**人类可读**的错误消息。
2. 行为正确：`scatter_gather(pool, 1..100, [](int x){return x*x;}, std::plus<>{})` 返回 338'350。
3. 取消传播正确：外部 stop ⇒ 所有 worker 在 spec window 内退出。
4. 异常聚合正确：任一 worker 抛异常 ⇒ 其他被 stop，异常聚合方式与 nursery 一致（同一种实现）。
5. 提供注解：

```cpp
[[=Pat::Composes{of = {"schedule_bulk", "when_all", "reduce"}}]]
[[=Pat::Requires{concepts = {"BulkScheduler", "ParallelScheduler"}}]]
```

### 提示链

- 这是 spec 中**没有规定**的 pattern——你的实现是开放设计。但**应当模仿** `parallel_for` 的形态（看 `06-patterns.md` 附录的 30 行源码）。
- 取消聚合 / 异常聚合**不必重新实现**——把它包在一个内部 nursery 里，让 nursery 替你做。

### v0.3 变体：两阶段提交（2PC）雏形（选作）

直接复刻 §8.4.4 的协调者代码，用 `Barrier` + `Latch` 真打一次 2PC：

```cpp
struct FakeParticipant {
    int  id;
    bool will_vote_yes;
    int  prepare_delay_ms;

    auto prepare() -> Task<bool> {
        co_await delay(std::chrono::milliseconds{prepare_delay_ms});
        co_return will_vote_yes;
    }
    auto commit() -> Task<void> { co_await delay(5ms); std::printf("p%d commit\n", id); }
    auto abort()  -> Task<void> { co_await delay(5ms); std::printf("p%d abort\n",  id); }
};

auto run_2pc(std::span<FakeParticipant> ps) -> Task<bool>;
```

### 验收标准（v0.3 变体）

1. *全员 yes*：3 个参与者全部投 yes ⇒ 全部 commit、返回 true；
2. *单人 no*：3 个里有 1 个投 no ⇒ 全部 abort、返回 false（**不能有人 commit、不能有人 abort 漏掉**）；
3. *prepare 延迟不均*：参与者延迟 = {10ms, 50ms, 200ms} ⇒ Phase 2 必须等到最慢的 prepare
   完成才开始（这是 Barrier 的契约）；
4. *prepare 中途取消*：50ms 时外部 stop ⇒ 已 prepare 的参与者 *不能* 进入 commit，整体 set_stopped。

### 反思

把 Barrier 换成 Latch 会怎样？把 Latch 换成 Barrier 会怎样？写出 *两种错误替换* 各自
破坏哪一条 2PC 不变量——这正是 §8.4 末尾"为什么 prepare 用 Barrier、commit 用 Latch"
的*反向考核*。

---

## Lab 9 · 写一个 trace domain

**前置：** Lesson 9。**预计时长：** 2 小时。

### 任务

实现 Lesson 9 §a 的 `TracingDomain`，但增强：

1. 给每个 `then` / `let_value` / `when_all` 节点打 trace（不只是 `then`）。
2. trace 输出包括：节点类型、入栈时间戳、出栈时间戳。
3. 输出到 Chrome `chrome://tracing` 兼容的 JSON 格式。
4. 通过 round-trip 测试：被 trace 包过的 sender 与原 sender **行为等价**（同 value、同异常、同取消）。

### 验收标准

1. 把 Lesson 12 的 capstone 用 `TracingDomain` 重新跑一遍，打开 chrome://tracing 能看到完整 sender 树的执行火焰图。
2. trace 关掉（替换为 `default_domain`）时，capstone 的运行时间**变化 < 1%**——因为 trace 是 compile-time 注入的，不打开等于零成本。
3. round-trip 测试覆盖：value 路径、error 路径、stop 路径。

### 提示链

- "compile-time 注入但默认零成本"的关键是：trace domain 的 `transform_sender` 在 `constexpr if (TRACING_ENABLED)` 里**条件性**改写——`TRACING_ENABLED = false` 时整个改写被 dead-code-eliminate 掉。
- chrome://tracing JSON 的格式参考 [Trace Event Format](https://docs.google.com/document/d/1CvAClvFfyA5R-PhYUmn5OOQtYMH4h6I0nSsKchNAySU)。每个 event 是 `{"name": ..., "ph": "B" or "E", "ts": ..., "pid": 0, "tid": ...}`。

---

## Lab 10 · 扩展 verify_scheduler

**前置：** Lesson 10。**预计时长：** 1.5 小时。

### 任务

在 Lesson 10 的基础上，把 `verify_scheduler<S>()` 扩展为 `verify_async_entity<E>()`，能处理：

- scheduler
- sender adaptor
- pattern
- coroutine task type

每种实体有不同的注解期望表，验证器**自动**分派。

### 验收标准

1. 对 Lesson 11 的 `VkComputeScheduler` 走 `verify_async_entity` ⇒ 编译通过。
2. 对 Lab 7 的 `my_timeout` 走 `verify_async_entity` ⇒ 编译通过。
3. 对**故意造的反例**（声称 `BulkScheduler` 但 missing `schedule_bulk`）⇒ 编译期失败，错误消息**精确指明**缺失方法名。
4. 验证器代码总长 ≤ 150 行（强制简洁）。

### 提示链

- 用 `std::meta::is_class_type` / `std::meta::has_default_member_initializer` 等反射查询，配合 `template for` 遍历期望表。
- "自动分派"靠：反射出 entity 上挂的**第一个**注解，看它属于哪类（`Offers` ⇒ scheduler；`Propagates` ⇒ adaptor；`Composes` ⇒ pattern；`TaskContext` ⇒ coroutine task）。

---

## Lab 11 · 你自己的 backend

**前置：** Lesson 11。**预计时长：** 3–4 小时。

### 任务

挑一种你**有访问权限**的执行资源——例如：

- 一个嵌入式 RTOS 任务（FreeRTOS task）
- 一个 Web Worker（浏览器环境）
- 一个 DPDK poll mode driver 线程
- 一个 NUMA-aware thread pool
- 一个 GPU compute queue（Vulkan / CUDA / Metal）

为它写一个 scheduler，让 `Async::just(x) | Async::on(your_sched, ...)` 能跑通。

### 验收标准

1. 通过 `verify_scheduler<YourSched>()`。
2. 通过 round-trip 测试：把 Lab 1 / Lab 2 的 fizzbuzz 程序换上你的 scheduler，输出仍然正确。
3. 写一份不超过 500 字的"backend 设计文档"，包括：
   - 注解三联（Offers / Cancellable / Allocates）的选择理由
   - 取消传播怎么实现
   - 错误传播怎么实现
   - 已知限制 / 未来工作

### 提示链

- 最大坑：**equality**。scheduler 必须支持 `operator==`，并且 stdexec 用 equality 来决定"是否需要 schedule"。如果你的 scheduler 持有"指向某线程的指针"，两个指向同一线程的 scheduler 必须相等。

### v0.3 变体：Singleflight\<K,V\> 选讲（选作）

读 Go 的 `singleflight.Group` 源码，把它移植到 sender 框架里：

> **契约**：同一 key 的并发 *请求合并*——第一个发起者真正去执行，其余调用者等待并
> 共享同一结果。

```cpp
template <typename K, typename V, typename Hash = std::hash<K>>
class Singleflight {
public:
    // do_or_join：若 key 已有 inflight，等待并共享；否则发起新的
    template <typename Producer>
    auto do_or_join(K key, Producer make)
        -> sender_of<set_value_t(V), set_error_t(...)>;
};

// 用法：
Singleflight<std::string, Config> cfg_cache;
auto cfg = co_await cfg_cache.do_or_join("global", [] {
    return fetch_config_from_remote();                    // 100 并发→只发 1 次
});
```

### 验收标准

1. 100 个协程同时 `do_or_join("k", ...)` → producer 被调用 *恰好一次*；
2. 100 个协程都收到 *同一份* V（按值复制或共享 `shared_ptr<V>` 均可）；
3. producer 抛异常 → *所有* 等待者都收到该异常的副本；
4. 完成（成功 / 失败）后，*下一次* `do_or_join("k", ...)` *会重新* 调用 producer。

### 反思

Singleflight 与 §C.4 选修的 *Cache\<K,V\>* 是什么关系？提示：Singleflight 解决"穿透"
（cache miss 时的并发雪崩），Cache 解决"命中"（已有结果的复用）。两者组合就是
**Caffeine** 的核心定理。

---

## Lab 12 · 把所有零件组成你自己的项目

**前置：** Lesson 12。**预计时长：** 8–20 小时。

### 任务

从 Lesson 12 §c 给出的三个开放题里**选一个**（或自拟），完成一个**端到端**项目。

### 验收标准

1. 项目跑得起来（README 含一行编译命令）。
2. 包含：自定义 scheduler / pattern / 至少 3 个 adaptor 的组合 / Nursery / timeout + retry / 至少一处反压。
3. 用 trace domain（Lab 9 成果）跑出一张火焰图截图。
4. 用 `AllocCheck` 跑出分配报告，**逐项解释**每次分配。
5. 写一份 1500 字的 design doc：sender 树拓扑、forward progress、cancellation、allocation budget。

### 反思（必答）

回看你写的代码。其中有多少行是**结构性的**（描述"做什么"），有多少行是**事务性的**（处理"怎么做"）？理想比例应当 7:3 偏向结构性——若你的事务性代码远多于此，说明你正在与框架打架，回去检查是不是没用 patterns、没用 nursery。

### v0.3 变体：Saga 选讲（选作）

如果你的端到端项目里有 *多步骤、需要补偿* 的业务流程（订单 → 扣款 → 发货；上传 →
转码 → 通知 CDN；建索引 → 写元数据 → 推搜索），就值得用 Saga 来 *编译期编排* 它。

**契约**：N 个 step 顺序执行，第 k 步失败 → 已执行的 1..k-1 步按 *逆序* 调用各自的
补偿函数，最终 set_error(SagaFailed{at_step = k})。

```cpp
template <typename... Steps>
class Saga {
public:
    // 每个 Step = { auto forward() -> Sender; auto compensate() -> Sender; }
    auto run() -> sender_of<set_value_t(), set_error_t(SagaFailed)>;
};

struct ReserveInventory {
    Order order;
    auto forward()    -> Task<void> { co_await inventory_reserve(order);     }
    auto compensate() -> Task<void> { co_await inventory_release(order);     }
};
struct ChargePayment {
    Order order;
    auto forward()    -> Task<void> { co_await payment_charge(order);        }
    auto compensate() -> Task<void> { co_await payment_refund(order);        }
};
struct ShipOrder {
    Order order;
    auto forward()    -> Task<void> { co_await shipping_dispatch(order);     }
    auto compensate() -> Task<void> { co_await shipping_cancel(order);       }
};

auto place_order(Order o) {
    return Saga{ReserveInventory{o}, ChargePayment{o}, ShipOrder{o}}.run();
}
```

### 验收标准（v0.3 变体）

1. *全部成功*：3 个 step 全部成功 ⇒ set_value，0 次补偿；
2. *中间失败*：ChargePayment 失败 ⇒ ReserveInventory.compensate() 被调用 *恰好一次*，
   ShipOrder *不被* 调用；
3. *失败逆序*：第 3 步失败 ⇒ 补偿调用顺序必须是 2 → 1（不是 1 → 2）；
4. *补偿不可失败*：若 compensate 自身抛异常，记录到 `Saga::compensation_errors[]`，
   *仍然继续* 执行剩余补偿（这是 *最重要的* Saga 不变量，对照 §C.4）。

### 反思

Saga 与 Nursery 在 *失败传播* 上有什么本质区别？提示：Nursery 是 *协程作用域* 内的
异常聚合（兄弟一起死）；Saga 是 *业务作用域* 内的逆向补偿（前面的还要 *撤销*）。
两者**不可互相替代**——Nursery 不管"已经做的怎么撤销"，Saga 不管"还在做的怎么停"。

---

# Unit III · 深度案例 / 性能 / 调试 / 陷阱

> **本单元的位置。** Unit I 教概念，Unit II 教动手。Unit III 教**判断**——把"我会写"提升到"我会读、会改、会救火"。
>
> 三块内容：**(A) 深度案例**——拿三个真实系统当解剖标本；**(B) 性能与可观测性**——HALO 实测、AllocCheck、CancelProbe、trace 集成；**(C) 12 大陷阱手册**——你将会犯但最好提前知道的错误。

---

## A. 深度案例研究

### 案例 1 · 游戏引擎主循环

**背景设定。** 一个典型的 PC / Console 引擎主循环，目标 60 FPS（即每帧 ~16.6ms）。每帧的工作分四类：

- **Sim**（gameplay simulation，CPU 重）
- **Anim**（骨骼动画、IK，CPU 中等可向量化）
- **Render**（命令构造、GPU 提交，CPU 轻 + GPU 重）
- **Audio**（混音、3D positional，CPU 中等延迟敏感）

#### 朴素实现的痛点

传统实现把 Sim/Anim/Render/Audio 写在同一个线程的 4 个函数里，串行调用。问题：

1. 帧时不稳定——任一阶段抖动直接拖累整帧。
2. 多核 CPU 资源闲置。
3. GPU 与 CPU 无法 pipeline。

#### sender 树重写

```cpp
Async::Task<void> frame(GameState& g, EngineSched& es) {
    auto sim_done   = Async::on(es.worker_pool,
                                Async::just(std::ref(g))
                              | Async::then(simulate));

    // anim 依赖 sim
    auto anim_done  = Async::let_value(sim_done, [&](auto&){
        return Async::on(es.worker_pool,
                         Async::just(std::ref(g))
                       | Async::then(animate));
    });

    // render 依赖 anim；提交到 GPU 是异步等 fence
    auto render_done = Async::let_value(anim_done, [&](auto&){
        return Async::on(es.render_thread,
                         Async::just(std::ref(g))
                       | Async::then(build_commands)
                       | Async::then(submit_to_gpu));
    });

    // audio 独立于上面，与 sim/anim 并行
    auto audio_done  = Async::on(es.audio_thread,
                                 Async::just(std::ref(g))
                               | Async::then(mix_audio));

    // 等所有路径
    co_await Async::when_all(std::move(render_done), std::move(audio_done));
}
```

**sender 树拓扑。**

```
                    when_all
                    /      \
              render_done   audio_done
                 │             │
              let_value      on(audio_thread)
                 │
              anim_done
                 │
              let_value
                 │
              sim_done
                 │
              on(worker_pool)
```

**forward progress 分析。**

| 阶段 | scheduler | progress | 取消传播 |
|------|------|------|------|
| sim | StaticPool | parallel | ✓ |
| anim | StaticPool | parallel | ✓ |
| render | render_thread (Affine) | concurrent | ✓ |
| audio | audio_thread (Affine) | concurrent | ✓ |

**关键收益。**

- audio 与 sim/anim **真并行**——音频抖动不再被 sim 抢占。
- render 提交到 GPU 后协程让出，CPU 与 GPU **真 pipeline**——这一帧的 CPU 工作可以与上一帧的 GPU 工作并行。
- 帧时方差从 ±5ms 降到 ±1ms（典型数字，具体看引擎）。

#### 失败案例：共享可变状态

第一版实现中，`simulate` 写 `g.entities`，`animate` 读 `g.entities`——它们在不同线程上跑，理论上有 race。`simulate` 一旦还没写完 `entities[i]`，`animate` 就读到半完成状态，会导致角色抖动。

**修正。** spec 强制建议在每个 stage 间显式给一个**snapshot 节点**：

```cpp
auto sim_done = ... | Async::then([](GameState& g){ return snapshot_for_anim(g); });
```

snapshot 把"sim 写的部分"复制成 immutable view，传给 anim。这是结构化并发与不可变数据共同维护的契约：**跨调度器边界禁止共享可变引用**。

#### 教训

1. sender 树让阶段间依赖**显式化**——画出拓扑就能看出哪些路径可并行。
2. 取消传播让"中途按 ESC 退出游戏"变成一行 `stop_source.request_stop()`，nursery 替你停掉所有 stage。
3. **共享可变状态**是 sender 树之外的关注点——框架不解决它，但 sender 树把它**可见化**，让你不会"无意识"地引入 race。

---

### 案例 2 · 流式 RPC 服务端

**背景设定。** 一个 grpc-style 流式 RPC 服务，每个连接一个 bidirectional stream。客户端推请求、服务端推响应，两个方向独立。

#### 朴素实现的痛点

回调地狱：`onRequest` callback、`onResponse` callback、`onClose` callback、`onError` callback——四个回调散落，状态机靠 connection 对象上的成员变量维护。代码量大、bug 多、流控难。

#### sender 树重写

```cpp
Async::Task<void> handle_connection(Connection conn, ServerSched& ss) {
    co_await Async::open_nursery([&](Async::Nursery& n) -> Async::Task<void> {
        // 上行：客户端 → 服务端
        Async::Stream<Request> requests = Async::from_queue(conn.in_q);
        // 下行：服务端 → 客户端
        Async::AsyncQueue<Response> out_q{32};

        // 业务处理协程
        n.spawn([&]() -> Async::Task<void> {
            MASHIRO_FOR_CO_AWAIT(Request r, requests) {
                auto resp = co_await Async::on(ss.business_pool,
                                               Async::just(std::move(r))
                                             | Async::then(process_request));
                co_await out_q.push(std::move(resp));
            }
            out_q.close();   // 上行结束 ⇒ 下行也准备结束
        }());

        // 下行写出协程
        n.spawn([&]() -> Async::Task<void> {
            MASHIRO_FOR_CO_AWAIT(Response r, Async::from_queue(out_q)) {
                co_await Async::on(ss.io_sched,
                                   Async::just(std::move(r))
                                 | Async::then([&](auto r){ return write_to_socket(conn, r); }));
            }
        }());

        co_return;
    });
    // nursery 关闭 ⇒ 上下行都干净结束
}
```

**关键设计点。**

1. **上行直接用 `from_queue` 包成 Stream**——客户端推过来的请求是天然的流。
2. **业务处理与网络写解耦**——通过中间的 `out_q` 隔开，业务慢不会卡网络（在 queue 容量内）。
3. **nursery 守护两条协程**——其中任一失败（业务异常、socket 断），nursery 把另一条 stop。
4. **out_q 关闭**自然终止下行——业务循环结束 ⇒ close out_q ⇒ 下行 `for co_await` 自然退出。

#### 失败案例：drop on close

第一版实现里，业务循环 `break` 出去（业务决定结束）后**没有** `close(out_q)`。下行协程卡在 `for co_await out_q`（等不到 close）。整个 nursery 卡死直到外部 stop。

**修正。** 引入 RAII 包装：

```cpp
struct QueueCloseGuard {
    Async::AsyncQueue<Response>* q;
    ~QueueCloseGuard() { if (q) q->close(); }
};
```

——但这只是补丁。**根因**是设计上没有强制 "producer 退出必须 close"。spec 在 `09-synthesis.md` §2.8 把这条提升为**约定**：spec 鼓励 `AsyncQueue` 提供 `producer_handle` 与 `consumer_handle` 两套句柄，producer_handle 的析构自动 close。这是把"易错" rewrite 成"易对"。

#### 教训

1. Stream 把 callback 地狱**压平**——`for co_await` 是顺序代码的形状。
2. nursery 让"一条协程挂掉，整个连接干净 teardown"变成默认行为。
3. **生命周期边界**（什么时候 close）必须显式设计——RAII 句柄是一种好的形态。

---

### 案例 3 · 流式 ETL 管道

**背景设定。** 数据工程场景。每天处理 100M 行日志：解析 → 过滤 → 富化（查询外部服务） → 聚合 → 写入。

#### 性能要求

- 端到端吞吐 ≥ 50K rows/sec
- 富化阶段需要远程 RPC，但 RPC 服务有 QPS 限制（≤ 1000 reqs/sec）
- 失败行需要重试 3 次，永久失败的写入死信队列

#### sender 树

```cpp
auto pipeline =
    parse_stream(input_files)                          // Stream<RawLog>
  | Async::stream::filter(is_valid)                    // Stream<RawLog>
  | Async::stream::throttle(1000_per_sec)              // 限流，保护远程服务
  | Async::stream::flat_map([&](RawLog r) {
        return Async::on(rpc_pool,
                         Async::just(r)
                       | Async::then(call_enrichment)
                       | Async::timeout(2s)
                       | Async::retry(3, is_retriable)
                       | Async::on_error([&](auto e){
                             dead_letter_q.push_sync(r);
                             return Async::just(Enriched{});   // 替换为空
                         }));
    })                                                  // Stream<Enriched>
  | Async::stream::batch(1000, 500ms)                   // 攒批
  | Async::stream::for_each([&](std::vector<Enriched> batch){
        return write_to_warehouse(batch);
    });

co_await pipeline;
```

#### 关键技术点

1. **throttle**——限流是流的"反压"在时间维度的镜像。它保证下游 RPC 不会被打爆。
2. **flat_map 触发并发**——每个元素被 map 成一个 sender，这些 sender 在 rpc_pool 上并行执行。并发度由 rpc_pool 大小 + throttle 共同决定。
3. **on_error 转向死信**——错误不是 throw，是被 adaptor 接住转成"空 Enriched 替换 + push 到死信队列"。整条 pipeline 不被一行错误打断。
4. **batch 攒批**——按 size 或 timeout 任一触发，保证延迟与吞吐的平衡。

#### 失败案例：throttle 在 flat_map 之后

第一版实现把 throttle 放在 flat_map **之后**。结果：throttle 限速没用——flat_map 内部已经把 1000 个 RPC 全打出去了，throttle 只是限了 flat_map 输出的速率，对上游 RPC 服务无保护。

**修正。** throttle 必须在 flat_map **之前**，让"还没发出的请求"被排队，而不是"已发出的响应"被排队。这是流编程里一个**反复出现**的陷阱：限流的位置决定限的是"输入"还是"输出"。

#### 教训

1. ETL 管道的形状是 Stream/sender 词汇的天然主场——adaptor 一对一映射到管道阶段。
2. **限流 / 攒批 / 反压**是流的"非功能性属性"，而 Mashiro 把它们做成正式的 adaptor，不是用户自己用 mutex 拼。
3. 失败处理设计**应当在树里显式表达**——`on_error` adaptor 让"哪些错误吞 / 哪些错误传"变成结构性问题，不是散落在 catch 块里。

---

## B. 性能与可观测性

### B.1 HALO 实测

Heap Allocation eLision Optimization（堆分配消除）是协程帧能否落栈的关键。spec 中**承诺**满足下列条件时 HALO 100% 命中（`04-coroutine-tasks.md` §3）：

1. 协程的**调用者**类型在编译期已知
2. 协程**没有**跨调用者上下文转移（不跨线程）
3. 协程没有被 `co_await` 之外的方式调用（例如不能存进 `std::function`）

但**这是理论保证**。实际编译器实现各异。本节给一个实测方法。

#### 实测脚本

```cpp
template<class T>
Async::Task<T> measured_task(T x) {
    co_return x + 1;   // 最简单协程
}

int main() {
    Async::Diagnostics::AllocCheck guard{"halo-test"};
    auto [r] = stdexec::sync_wait(measured_task(42)).value();
}
```

预期：`AllocCheck` 报告 **0 次分配**（HALO 命中）。

#### 实测结果

| 编译器 | -O0 | -O1 | -O2 | -O3 |
|------|------|------|------|------|
| clang-p2996 | 1 | 1 | 0 | 0 |
| gcc-15 (实验 P2996 fork) | 1 | 1 | 1 | 0 |

——HALO 在 `-O2` 起命中（clang），`-O3` 起命中（gcc）。**这是 debug 配置下你看到分配增加**的来源。spec 在 `04-coroutine-tasks.md` §3 把这条以"qualification clause"形式列入：

> HALO is guaranteed at `-O2` and above for the qualified call sites.

——意即 spec 不承诺 debug 配置下的零分配。如果你测试时纠结 debug 下的分配数，那是**错读 spec**。

#### 何时 HALO 必失效

1. **跨线程**：`co_await Async::on(pool, ...)` 把协程移到 pool worker；协程帧必须能在 worker 上 resume ⇒ 必须能在 heap 上（worker 不持有调用者栈）。
2. **类型擦除**：把 Task 存进 `std::function` / `std::any` / `Async::AnyTask`。
3. **存储进容器**：`std::vector<Task<int>>` 同理——容器需要稳定地址。
4. **`spawn` 进 nursery**：spec 在 §3.4 明确"nursery spawn 的 task 必然 escape"。

记住这四条，HALO 命中与否你能**预判**。

### B.2 AllocCheck 用法

`Async::Diagnostics::AllocCheck` 是 spec L0 暴露的诊断工具：

```cpp
{
    Async::Diagnostics::AllocCheck guard{"region-name"};
    // ... 异步代码 ...
}   // 析构时打印分配报告
```

报告示例：

```
[alloc-check region-name]
  allocations: 3
  total bytes: 256
  by source:
    Task<int>::promise_type        × 2  (192 bytes)
    AsyncQueue::node_pool          × 1  (64 bytes)
  by call stack:
    ... (mini stack trace per allocation)
```

实现机制：AllocCheck 通过覆写 `operator new` + 一个 thread_local 计数器实现。它**只**统计在 `guard` 生命周期内、由 sender / Task / Stream 机制触发的分配——不统计用户业务代码的 `std::vector::push_back`。

#### 用 AllocCheck 做 budget 测试

CI 中可以做"分配预算回归测试"：

```cpp
TEST(Pipeline, AllocationBudget) {
    Async::Diagnostics::AllocCheck guard{"pipeline-test"};
    run_pipeline();
    ASSERT_LE(guard.allocations(), 5);    // 不准超过 5 次
}
```

这把"性能回归"从"跑 10 次取中位数"的 flaky 测试变成"分配数比上次多就 fail"的 deterministic 测试。**deterministic > statistical** 是性能测试的金科玉律。

### B.3 CancelProbe 用法

`CancelProbe` 是给取消传播做"故障注入测试"的工具：

```cpp
Async::Diagnostics::CancelProbe probe;
probe.inject_after(50ms);
auto result = stdexec::sync_wait(probe.wrap(your_pipeline));
ASSERT_EQ(result, std::nullopt);   // 取消应当让 sync_wait 返回 nullopt
ASSERT_LE(probe.cooperative_window(), 100ms);   // 取消到完成的时间
```

它做两件事：

1. 在指定时刻向 sender 树注入 stop_token。
2. 测量从 stop 到整条管线收尾的时间——`cooperative_window`。

spec 把"cooperative_window ≤ 5s"作为硬约束。CancelProbe 让这条约束**可测试**。

### B.4 stdexec 编译期 trace

stdexec 提供一个未文档化但极有用的诊断：

```cpp
#define STDEXEC_DEBUG_SENDERS 1
#include <stdexec/execution.hpp>
```

启用后，每个 `connect` 调用会打印 sender 树的 `type_name`：

```
[stdexec] connecting: then_sender<just_sender<int>, lambda#1>
[stdexec] connecting: when_all_sender<...>
```

——读起来像 Java 的 `toString()` 调试输出，但**类型层面**。当你被一个嵌套了 12 层 adaptor 的错误消息搞晕时，这个开关让 sender 树可读。

### B.5 集成 tracy / perfetto

trace domain（Lab 9）输出的 Chrome JSON 可以直接被 perfetto.dev 读取。tracy 集成要稍微多一点工作——你需要在 `transform_sender` 里调用 `tracy::ZoneNamedN` 而不是 JSON 写入。spec 把 tracy 集成放在 examples/ 目录，不进 spec 正文。

### B.6 Cache\<K,V,Eviction\>：把"重算"挡在门外（v0.3 增补 · 来自 Unit V）

性能优化里第一条最朴素的法则是 **不要算两次**。`Cache<K, V, Eviction>` 在 sender
框架里把这条法则变成 *零样板代码* 的 adaptor：

```cpp
template <typename K, typename V,
          typename Eviction = Eviction::LRU<256>,
          typename Clock    = SteadyClock>
class Cache {
public:
    auto get_or_compute(K key, auto producer)
        -> sender_of<set_value_t(V), set_error_t(...)>;

    void invalidate(const K& key) noexcept;
};
```

**Eviction 三策略**（致敬 Caffeine）：

| Strategy | 行为 | 适用 |
|----------|------|-----|
| `LRU<N>` | 容量满时驱逐最久未访问 | 通用默认 |
| `LFU<N>` | 容量满时驱逐访问最少 | 重尾分布（80/20）|
| `TTL<D>` | 写入后 D 时间过期 | 数据有 staleness 上限 |
| `W_TinyLFU<N>` | LFU + 窗口准入 + Bloom filter | Caffeine 默认；命中率最高 |

**与 Singleflight 的合体**——Caffeine 的核心定理：

```cpp
template <typename K, typename V>
class CachingSingleflight {
    Cache<K, V>          cache;
    Singleflight<K, V>   sf;
public:
    auto fetch(K k, auto producer) {
        // 1. 命中 → 直接返回    （Cache 路径）
        // 2. miss → do_or_join   （Singleflight 路径，合并并发请求）
        // 3. 完成后写回 cache
        return cache.get_or_compute(k, [&] {
            return sf.do_or_join(k, producer);
        });
    }
};
```

这两个原语 *独立* 都有意义，*合体* 就是高 QPS 服务的标配——一次请求穿过 Cache（命
中→0 次后端调用），未命中时穿过 Singleflight（合并→1 次后端调用），无论上游来 1 个
还是 1000 个调用，*后端最多被打一次*。

### B.7 perf pitfall checklist

| pitfall | 症状 | 修复 |
|------|------|------|
| 协程帧分配在 hot path | AllocCheck 报告几百次/秒 | 换成 sender 表达；或绑定 arena allocator |
| 跨线程 hop 过多 | trace 显示协程在 pool/main 之间频繁切换 | 用 `let_value` 合并相邻 stage 到同一调度器 |
| 反压不生效 | 内存稳定增长 | 检查 AsyncQueue 容量 + producer 是否真挂在 push |
| timeout 过短 | 错误率高、retry 频繁 | 测量 P99 延迟再设 timeout = P99 × 2 |
| race 输家不死 | 整条管线无法结束 | 输家的 sender 必须 Cancellable；查 spec §1 |
| schedule_bulk chunk 太小 | bulk 反而比 single 慢 | chunk_size 至少 1000 元素，让缓存命中起来 |

---

## C. 12 大陷阱手册

> **本节体例。** 每个陷阱给出：症状（看起来像什么）、根因（实际上是什么）、修复（怎么改）、预防（下次怎么避免）。
>
> 排序按"在 12 讲里出现的早晚"。

### 陷阱 1 · "我只是 `co_await just(42)`，怎么会有协程帧分配？"

**症状。** Lesson 2 §a 的 `compute()` 一调用 `AllocCheck` 就报 1 次分配。困惑：我没干什么异步的事。

**根因。** `Task<int>` 是协程类型；任何返回 `Task` 的函数都是协程；协程一定有 promise_type，promise_type 要存放在协程帧。Debug 配置下 HALO 不命中，帧就在 heap 上。

**修复。** 切到 `-O2`+，确认 HALO 命中；或如果你确实不需要协程，把代码改回 sender 表达式形式（Lab 1 风格）。

**预防。** 心里挂着一条规则：**"返回 Task 的函数，调用一次就有一次潜在协程帧。HALO 命中与否取决于编译器看不看得见调用点。"**

---

### 陷阱 2 · "我取消了，但程序没退出"

**症状。** Lesson 3 改进版，`stop_source.request_stop()` 调了，但程序还跑半秒才出来。

**根因。** 协程内的工作循环**没检查** `stop_token`——它仍然在跑 `for (int i = 0; i < 100; ++i) ...`。stop_token 是协作式的，**框架不能强制中断你的循环**。

**修复。** 在循环里加 `if (tok.stop_requested()) throw Coro::stopped_signal{};`。或更优雅地用 `Async::cancellable_loop(N, [&](int i){...}, tok)` 这种封装。

**预防。** 任何**长时间运行**的协程都需要至少一个 check point。spec 推荐每 1ms～10ms 的工作量插一个 check（太频繁是噪声，太稀疏是 sluggish）。

---

### 陷阱 3 · "`spawn_detached` 真好用，但 main 退出后 crash"

**症状。** 你 fire-and-forget 一个后台任务，main 结束后系统报 abort：协程在销毁的全局对象上 callback。

**根因。** `spawn_detached` 把任务从所有结构化作用域剥离——没有人等它。main 返回后全局析构开始，任务还在跑，它访问的对象（例如 logger）被析构，crash。

**修复。** 永远把任务挂在某个 nursery 下。如果你必须跨整个程序生命周期，挂在 `Async::g_app_scope`（程序级 nursery，由 main 初始化时创建、退出时 close）。

**预防。** **永远不用 `spawn_detached`**。spec 留它只是为了向旧 codebase 渐进迁移。

---

### 陷阱 4 · "Inline scheduler 应当便宜，怎么我跑得这么慢？"

**症状。** 用 `Inline::scheduler()` 跑一个 100K 步的协程链，时间是裸 `for` 循环的 50×。

**根因。** Inline scheduler 每一步都走 sender connect / start / set_value 的全套机制；HALO 不一定救得了每一层。如果你把 100K 个 `co_await just(i) | then(...)` 串成一条链，编译器需要生成 100K 层 op-state 的嵌套，编译都跑不动，更别说运行。

**修复。** 用 `Async::stream::iota(0, 100'000) | Async::stream::for_each([](int i){...})`——一个 stream 替你做循环，而不是 100K 个 sender。

**预防。** sender 是用来表达**异步边界**的，不是用来代替 `for` 循环。**异步边界数应当与"真异步事件数"同数量级**——不超过几千。

---

### 陷阱 5 · "nursery 自动等所有 child，为什么我的程序挂了？"

**症状。** nursery close 那一刻程序卡死。

**根因。** child 之一在等一个永不到来的事件（AsyncQueue 没人 close、远程 RPC 没回包），nursery 在等它。

**修复。** 给每条 child 加 timeout 兜底：

```cpp
n.spawn(your_task() | Async::timeout(30s));
```

**预防。** **任何外部 IO 必须 timeout**。这条规则只有一个例外：被 `Async::Backend::Platform::delay` 这种你**完全控制**的等待。

---

### 陷阱 6 · "我加了 timeout，但管线挂得更久了"

**症状。** 加 timeout 之前管线挂 30s，加了 5s timeout 之后挂 25s——而不是 5s。

**根因。** timeout 触发了，但**内部 sender 不响应 stop**（例如它在 `vkWaitForFences(..., UINT64_MAX)` 里）。spec 规定 cooperative_window = 5s——5s 后强制收尾——所以你看到的总时长 = 实际等的时长 + 5s 强制窗口，仍然短于 30s 但远大于 5s。

**修复。** 检查内部 sender 是否注解了 `Cancellable`。如果它**说**自己 Cancellable 但实际不是，这是 bug，需要修正它的实现。如果它确实**不能**取消（硬件限制），考虑把它包在专门的 fire-and-forget worker 里，timeout 只取消"等待这个 worker"而不是"等待 fence 本身"。

**预防。** 注解的真实性要被验证（Lesson 10）。`CancelProbe` 是验证手段。

---

### 陷阱 7 · "when_all 的 tuple 顺序对不上"

**症状。** `when_all(a, b, c)` 返回 `tuple<A, B, C>`，但你拿到的 `<A>` 元素是 b 的结果。

**根因。** 你 90% 是把 `when_all` 与 `race` 写混了。race 返回 variant 表示"谁赢了"；when_all 返回 tuple 按调用顺序。检查 binding：

```cpp
auto [ra, rb, rc] = co_await Async::when_all(a, b, c);
```

——`ra` 是 `a` 的结果，无论它是第一个完成的还是最后一个。

**修复 + 预防。** 看见 `auto [...] = co_await ...` 时**默念**: "tuple 按调用顺序，不按完成顺序。" 这条与人的直觉相反——直觉是"先完成的先出现"。

---

### 陷阱 8 · "parallel_for 把 Inline 给我了"

**症状。** 编译期错误 50 行，红字海。

**根因。** `parallel_for(Inline::scheduler(), ...)` ——Inline 不是 BulkScheduler。

**修复。** 换成 `StaticPool::scheduler()` 或 `Tbb::scheduler()`。

**预防。** Lesson 10 的 verify_scheduler 用在这里：编译错误信息会精确指出"缺 OffersBulk"。如果你的工具链没启用 verify，**手动**在 `parallel_for` 那行上面加一个 `static_assert(Async::Concepts::BulkScheduler<Sched>);`，让错误信息提前出现。

---

### 陷阱 9 · "domain 重写让我的代码行为变了"

**症状。** 加了一个 trace domain 之后，某个 sender 的完成顺序变了。

**根因。** domain 重写**应当**保持可观察等价，但**你的** domain 实现 bug 了——它把异步 sender 重写成了同步执行，导致依赖完成顺序的下游代码错乱。

**修复。** 跑 round-trip 测试：原 sender 与重写后 sender 在 1000 次随机输入下行为一致。bug 会立刻冒出来。

**预防。** **永远**给 domain 写 round-trip 测试。spec §6 强制要求。

---

### 陷阱 10 · "我的 scheduler 编译过了 verify，但实际跑不对"

**症状。** verify_scheduler 通过，但 fizzbuzz 走它跑出乱序输出。

**根因。** verify 只检查"注解 ↔ concept"，不检查"实现的语义正确性"。你的 scheduler 可能注解了 `Offers::Affine` 而实现是 multi-thread，concept `AffineScheduler` 只检查类型签名，不检查"运行时是否真的 affine"。

**修复。** 用 `Async::Diagnostics::SchedulerProbe`（spec §5）跑行为级测试——它真的在 scheduler 上发提交、观察实际执行的线程。

**预防。** 编译期 verify + 运行期 probe **都做**。编译期保证签名，运行期保证语义。两者不能互替。

---

### 陷阱 11 · "我用 Stream 写代码，内存稳定增长"

**症状。** Stream 跑了 10 分钟，内存涨到 10GB。

**根因。** 你的 Stream 内部有一个 `AsyncQueue<HeavyObj>(capacity = unlimited)`——`unlimited` 关闭了反压。

**修复。** 容量是必填——0 表示无缓冲（rendezvous），N 表示 bounded。spec 在 §5.4 把"unlimited"标为**反模式**，要求 API 不接受它。

**预防。** 看见 queue 创建立刻问："容量是多少？"

---

### 陷阱 12 · "我的代码看起来对，但 review 时 senior 一眼看出 bug"

**症状。** 各种各样。

**根因。** sender / 协程代码有一类**视觉上像同步代码**的写法，但语义上不同。常见举几例：

```cpp
// (a) 看起来像变量赋值，其实有 sender 副本
auto s = Async::just(huge_object());   // huge_object 被存进 s
// 修复：std::move 进 just，或避免 huge object 流过 sender

// (b) 看起来像顺序，其实是 when_all 的两个独立子树
co_await Async::when_all(step_a(), step_b());
// step_a 与 step_b 真并行——如果它们之间有 data dep，crash
// 修复：用 let_value 串行
```

**修复 + 预防。** 看 sender 代码时多默念一句："**这条线在哪条调度器上，下一行是不是同一条？**" 如果你不能回答，先别提交。

---

### 陷阱 13 · "我的 Topic 内存为什么稳定增长？慢订阅者拖垮全场"（v0.3 增补）

**症状。** 用 `Topic<Event, Strategy::Buffered<N>>`，运行一段时间后内存稳定增长。慢
订阅者的 ring 一直在涨。

**根因。** 你以为 `Buffered<N>` 是 *每个订阅者一个 ring*，但它确实是；问题在 *发布
端* —— Topic 在写入每个 ring 之前需要 *持有 Event 的一份引用* 直到所有 ring 都写完。
如果某个慢订阅者的 ring 一直满（队列丢弃但 *引用计数* 没释放），event 就栈积在
publish() 的临时对象里。

**修复。** 把 `Event` 改成 *引用计数 + COW* 或 *trivially copyable*；或者把
`Buffered<N>` 换成 `Strategy::Latest`（如果只关心最新值）。再次明确：`Buffered<N>` 是
*每订阅者* 的容量上限，**不是** Topic 的总内存上限。

**预防。** Topic 的 *压力测试* 永远要包含 "1 个快订阅者 + 1 个故意停 100ms 的慢订阅
者" 的场景；对照 §C.4 给的 EventPump 现场参数。

---

### 陷阱 14 · "我的 Channel 死锁了，两端都在 await"（v0.3 增补）

**症状。** `Channel<int, Bounded<N>>`，producer 在 `send` 上挂起，consumer 在 `recv`
上挂起，*同一个* 协程上下文内的两件事相互等。

**根因。** 你写了类似这样的代码：

```cpp
co_await ch.send(42);           // 容量已满 → 挂起，等 recv
// 永远到不了这一行
co_await ch.recv();
```

——同一个协程同时是 producer 与 consumer，单纯地把 send 写在 recv 前，*没人能跑出去
让 channel 腾出空位*。Channel 不死锁的前提是 *send 与 recv 在不同执行单元上*。

**修复。** 把 producer / consumer 拆成 nursery 的两个 spawn——`when_all(producer,
consumer)`。或者改用 `Unbounded`（但承担内存风险）、`SingleShot`（如果只发一次）。

**预防。** Channel 用法的检查清单第一条：*send 端与 recv 端是否在不同协程？* 不是，
重写。

---

### 陷阱 15 · "我加了 Retry，结果把下游打挂了"（v0.3 增补）

**症状。** 偶发抖动场景下 Retry 工作良好；但下游真正出故障时，*整个集群都在 retry*，
连带着把刚刚恢复的下游又打挂。这就是 *Retry storm / 重试雪崩*。

**根因。** 三件事同时出错：

1. *没有 backoff*：retry 是 `retry(N)` 而不是 `retry(N, exponential_backoff(...))`，
   *瞬时* 5 次重打；
2. *没有 jitter*：所有客户端的 backoff 时钟对齐，*同一刻* 5 次重打；
3. *没有 CircuitBreaker*：下游确实 down 时仍然 retry，*正反馈* 把下游打死。

**修复。** §7.5 + §7.6 给的 *标准防御纵深* 一次性解决：

```cpp
raw_call() | timeout(D) | retry(N, exponential_jitter(...)) | with_bulkhead(g) | breaker;
```

**预防。** Retry 的检查清单：(a) 有 backoff 吗？(b) backoff 有 jitter 吗？(c) 外面包
了 CircuitBreaker 吗？三个 *都要是* yes 才能上线。这是为什么 §7.6 末尾的"标准防御纵
深四层"图必须背下来。

---

## Unit III 结语

这三块——案例、性能、陷阱——是把"会用 spec"提升到"会用 spec **救场**"的关键。Unit I 给你**词汇**，Unit II 给你**肌肉记忆**，Unit III 给你**判断力**。

工程能力的差距，70% 在判断力。

---

# Unit IV · 框架特性扩展（自顶而下设计）

> **本单元的位置。** Unit I–III 都在**教 spec 已有的东西**。Unit IV 把视角反过来——**站在 spec 之上**，问"还应当有什么"。每个 feature 都先讲**问题**（spec 当前的什么不够好），再讲**设计**（如何利用 C++20–26 把它做对、做净、做廉价），最后讲**代价与边界**（这个 feature 不能解决什么）。
>
> **设计原则（贯穿全单元）。**
>
> 1. **抽象层次正确。** 每个 feature 都落在 L0–L7 中**最合适**的那一层；不会因为方便就把高层 concern 漏进低层。
> 2. **充分但不过度地利用 C++20–26。** 反射 / consteval / concept / annotation / coroutine / stdexec 是工具，不是炫技目标。每个特性的使用都有具体动机。
> 3. **无性能损失。** 默认不开启的特性必须**编译期零成本**（被 dead-code 消去）；开启的特性必须**优于** spec 现有写法的最佳手写实现。
> 4. **编译期完成的优先编译期完成。** 任何能在编译期决定的事（类型分派、能力检查、拓扑排序）都不进运行期。
> 5. **语义干净，不同义反复。** 避免引入与现有词汇语义重叠的新词；若新词必然出现，必须给出"为什么旧词不够"。
> 6. **建模符合客观世界。** 类型层次应当映射到问题域的结构，而不是为了实现方便而扭曲。
>
> **本单元的 7 个 feature（按建议实现顺序）。**
>
> - **F1**: Typed Execution Environment（让 sender env 成为静态类型化的资源容器）
> - **F2**: Capability-Typed Schedulers（用 concept refinement 表达 backend 能力的偏序关系）
> - **F3**: Effect Row Annotations（用注解元组表达 adaptor 的副作用集合，编译期检查兼容性）
> - **F4**: Reflection-Driven Adaptor Genesis（从注解 + concept 自动**生成**adaptor 包装）
> - **F5**: Compile-Time Topological Scheduling（把 sender 树的拓扑信息在编译期算出来，运行期为零）
> - **F6**: Deterministic Replay（基于 effect rows + env 的可重放调度，调试与测试两吃）
> - **F7**: Structured Resource Lifetimes（把 RAII 与 nursery 在类型层面统一）

---

## F1 · Typed Execution Environment

### 问题

stdexec 的 `env` 是一个 type-erased 的属性包（用 `get_xxx(env)` 查询）。它解决了"sender 能从上下文取信息"的问题——例如 `get_stop_token(env)`、`get_scheduler(env)`。但它有三个**已知的不足**：

1. **查询失败是运行时**（默认值 fallback）：你写 `get_scheduler(env)`，env 里没有，得到一个 `inline_scheduler`——这种"静默 fallback"是 bug 沃土。
2. **属性集合不可枚举**：你不能在编译期问"这个 env 有哪些属性"——属性是 ad-hoc 的 query 类型，没有清单。
3. **跨边界传递无类型安全**：`continues_on` 把上游 env 传给下游，但任何"扩展属性"（用户自定义查询）必须靠运行期约定。

### 设计

引入 **typed environment**：env 不再是 type-erased 属性包，而是一个**编译期已知**的 `TypedEnv<Tag1, Tag2, ...>` 结构。每个 Tag 是一个"属性键 + 类型"的二元组。

```cpp
namespace Async {

// 每个 Tag 是一个空类型，配上一个"值类型"别名。
struct CurrentScheduler { template<class Sched> using value = Sched; };
struct CurrentStopToken { using value = stop_token; };
struct CurrentAllocator { template<class A> using value = A; };

// TypedEnv 是异构 map，键是 Tag，值是 Tag::value。
template<class... Tags>
class TypedEnv;

// 查询：编译期失败是编译错误，不是静默 fallback。
template<class Tag, class... Ts>
constexpr auto& get(TypedEnv<Ts...>& env) {
    static_assert((std::is_same_v<Tag, Ts> || ...),
                  "TypedEnv does not carry this Tag; add it explicitly upstream.");
    return env.template at<Tag>();
}

// 扩展：with<Tag>(env, value) 返回一个**新类型**的 env。
template<class Tag, class V, class... Ts>
constexpr auto with(TypedEnv<Ts...> env, V value) -> TypedEnv<Tag, Ts...>;

} // namespace Async
```

#### sender 端集成

sender 在 `connect(rcv)` 时 query receiver 的 env；env 类型流经整个 op-state。adaptor 想**扩充** env，调 `with<Tag>`：

```cpp
template<class Sender>
auto with_request_id(Sender s, RequestId rid) {
    return Async::transform_env(std::move(s), [=](auto env) {
        return Async::with<RequestIdTag>(env, rid);
    });
}
```

下游任何 sender / 协程都可以在编译期保证 `Async::get<RequestIdTag>(env)` 存在——若**忘记**在上游 `with_request_id`，下游编译失败，错误消息：

```
error: TypedEnv does not carry RequestIdTag; add it explicitly upstream.
note: lookup originated in `do_logging` at logger.cpp:42
```

#### consteval / 反射的角色

`TypedEnv` 内部用 P2996 反射枚举 `Ts...`，把"已携带 Tag 清单"暴露：

```cpp
template<class Env>
consteval auto carried_tags() -> std::vector<std::meta::info> {
    return std::meta::members_of(^^Env, std::meta::access::public_);
}
```

这让**任何工具**（错误消息生成、文档生成、IDE 智能提示）都能在编译期问"这条 sender 链上的 env 现在有什么"。

#### 与现有 stdexec env 的关系

`TypedEnv<...>` **是** stdexec env 的子类型：它满足所有 stdexec env 的 query 协议（提供 `query(get_xxx)` 等），只是**多**了编译期枚举与编译期失败保证。任何接受 stdexec env 的 adaptor **不需要改动**——TypedEnv 是渐进的增强，而非破坏性替代。

### 代价与边界

- **编译期类型膨胀**：env 类型在每次 `with<Tag>` 后变化；嵌套 N 次扩展导致 N 个不同类型。spec 应当限制 Tag 数量上限（建议 ≤ 16）。
- **不解决"运行时配置"**：例如"按命令行参数决定 env 里放什么"——这种 env 必须 fall back 到 type-erased。TypedEnv 是**已知静态结构**的优化。

### 抽象层归属

L1（foundations）。它是 L1 vocabulary 的强化，不引入新原语。

---

## F2 · Capability-Typed Schedulers

### 问题

Lesson 4 的注解三联（Offers / Cancellable / Allocates）描述"scheduler 提供什么"。但**注解是平的**——`Offers::Bulk + Offers::Parallel + Offers::Affine + Offers::Io` 之间的**偏序关系**没有表达：

- `BulkScheduler` 是 `Scheduler` 的 refinement（前者推后者）
- `Io + Parallel` 蕴含 `Concurrent`
- `Affine` 与 `Parallel` 互斥（前者完成在同一上下文，后者承诺并行执行）

当用户写 `parallel_for(sched, ...)` 时，框架希望接受**任何满足 BulkScheduler 的**——但目前 concept 写起来是"and 的列表"，组合爆炸。

### 设计

用 **concept refinement**（C++20 concept 继承）+ **annotation lattice**（用注解表达类型偏序）+ **consteval lattice 求值**，让 capability 成为**偏序集**。

```cpp
namespace Async::Capabilities {

// 1. 基础能力。它们组成一个 lattice（半格）。
struct Schedules    {};   // 提供 schedule()
struct Bulk         {};   // 提供 schedule_bulk()
struct Parallel     {};   // 工作彼此独立，并行
struct Affine       {};   // 完成在固定上下文
struct Io           {};   // 提供 IO 等待源
struct Cancellable  {};   // 支持外部 stop_token

// 2. lattice 关系（编译期，用反射可查）。
template<class Cap>
struct refines : std::type_identity<std::tuple<>> {};
template<>
struct refines<Bulk> : std::type_identity<std::tuple<Schedules>> {};
template<>
struct refines<Parallel> : std::type_identity<std::tuple<Schedules>> {};
template<>
struct refines<Affine> : std::type_identity<std::tuple<Schedules>> {};
// ... 等等

// 3. 互斥关系。
template<class A, class B>
constexpr bool conflicts = false;
template<>
constexpr bool conflicts<Affine, Parallel> = true;

// 4. consteval 求传递闭包。
template<class... Caps>
consteval auto closure() -> std::tuple<...>;

// 5. concept 自动生成。
template<class S, class... RequiredCaps>
concept SchedulerWith =
    Scheduler<S> &&
    (annotated_with_capability<S, RequiredCaps> && ...) &&
    !(any_conflict_in<S, RequiredCaps...>);

} // namespace Async::Capabilities
```

#### 用法

```cpp
template<class Sched>
    requires Async::Capabilities::SchedulerWith<Sched, Bulk, Parallel>
auto parallel_for(Sched sched, /*...*/);
```

——`parallel_for` 不再要求"BulkScheduler + ParallelScheduler"两个独立 concept 都满足，而是声明"我需要 Bulk 与 Parallel 这两条 capability"。框架自动验证：

1. Sched 注解里**直接或间接（refines 链）** 携带 Bulk 与 Parallel。
2. Bulk 与 Parallel **不冲突**（互斥检查）。
3. 缺失任一时给出"缺哪个 / 谁与谁冲突"的人类可读消息。

#### 错误消息工程

```
error: SchedulerWith<RenderScheduler, Bulk, Parallel> failed:
  - RenderScheduler annotates Capabilities::{Affine, Cancellable}
  - Capabilities::Parallel is required but not provided
  - Capabilities::Affine (carried) conflicts with Capabilities::Parallel (required)
suggestion: use a Parallel scheduler (StaticPool, Tbb) instead.
```

这条错误**完整指出**了：

- 实际具有什么能力
- 缺什么能力
- 是否有 capability 冲突
- 给出可行替代

工程价值：**新手第一次遇到 capability mismatch 时不需要看 spec**——错误消息自带答案。

#### 与 F1 的协作

`SchedulerWith` 的 concept 求值过程中，consteval 函数读取 scheduler 类型上的注解；这些注解经过 F1 的 TypedEnv 可以跨 adaptor 传递。**这两条 feature 互补**：F1 让 env 有静态类型，F2 让能力有偏序结构。

### 代价与边界

- **lattice 必须封闭**：一旦定义了 refines 与 conflicts，新增 capability 需要重新审查所有现有关系。spec 应当给 lattice 一个**版本号**与"扩展规则"。
- **不能表达运行时能力切换**：例如 "我的 scheduler 在某些机器上有 GPU，某些机器上没有"——这种 runtime polymorphism 必须落到 type-erased 句柄上，capability 只能描述"静态承诺"。

### 抽象层归属

L1（capability lattice）+ L0（annotations）。

---

## F3 · Effect Row Annotations

### 问题

Lesson 7 的六行验收清单是 adaptor 实现者的"良心检查"。但这种 informal checklist 有两个问题：

1. **不可检查**——你说你的 adaptor 满足"6/6"，谁验证？
2. **不可组合**——把两个 adaptor 串起来时，它们的"副作用"如何组合？没有计算规则。

更深的问题：adaptor 的"副作用"（throws、分配、stop 传播、scheduler 变更、值变换）目前散落在文档里，**没有一种结构化的表达**。

### 设计

借鉴 PL 中的 **effect systems**（Koka, Eff），引入 **effect row** —— adaptor 用注解声明它对 sender 树的"作用集合"，编译期可被组合 / 验证。

```cpp
namespace Async::Effects {

// 每条 effect 是一个空标签 + 可选参数。
struct Throws       { /* exception types */ };
struct Allocates    { Allocates::Where where; };
struct PropagatesStop { bool downstream = true; bool sibling = false; };
struct ChangesScheduler { /* ... */ };
struct CompletesOn  { /* set_value | set_stopped | set_error */ };

// effect row = 一组 effects 的有序元组。
template<class... Es>
struct Row;

// 注解形态
template<class... Es>
struct WithEffects { /*...*/ };

} // namespace Async::Effects

// adaptor 端
template<class S>
[[=Async::Effects::WithEffects<
    Async::Effects::PropagatesStop{},
    Async::Effects::CompletesOn::set_stopped_on_timeout>{}]]
auto timeout(std::chrono::nanoseconds, S);
```

#### Row Algebra

两条 adaptor 串起来 `f | g`，整体 effect row = `g.effects ⊕ f.effects`（其中 ⊕ 是 row 合并）。合并规则：

| effect A | effect B | A ⊕ B |
|------|------|------|
| Allocates(OpState) | Allocates(External) | Allocates(OpState ∪ External) |
| PropagatesStop(down) | PropagatesStop(down + sibling) | PropagatesStop(down + sibling) |
| Throws(set<E1>) | Throws(set<E2>) | Throws(set<E1 ∪ E2>) |
| CompletesOn(value) | CompletesOn(stopped) | CompletesOn(value ∪ stopped) |

⊕ 是**幂等、可结合、交换**的 monoid 运算（spec 选择"无序合并"以适应未来 reorder）。

#### 编译期检查

一个 sender 树通过 effect row 的合并，最终在 sync_wait 处得到一个**总效应集合**。可以做的检查：

1. **No-throw 路径**：在 noexcept 上下文里嵌一个 sender 树，编译期检查 effect row 不含 `Throws`。
2. **No-alloc 路径**：实时音频回调里嵌一个 sender 树，编译期检查 effect row 不含 `Allocates`（任何位置）。
3. **Cancellation contract**：要求最外层是 `PropagatesStop` 完整覆盖——否则编译期警告"unreachable from cancellation"。

```cpp
auto pipeline = ...;
Async::assert_no_alloc(pipeline);          // consteval check
Async::assert_no_throw(pipeline);           // consteval check
Async::assert_stop_reachable(pipeline);     // consteval check
```

每条 assert 失败时给精确错误：

```
error: assert_no_alloc failed for sender:
  effect Allocates::Where::OpState introduced by `Async::let_value<lambda#12>`
  at pipeline.cpp:48
```

### 与 F1 / F2 的协作

- F1 (TypedEnv) 让 env 信息静态化；effect rows 可以反过来**约束** env（如 `PropagatesStop` 要求 env 中带 `CurrentStopToken`）。
- F2 (Capability lattice) 描述 scheduler 的能力；effect rows 描述 adaptor 的副作用——两者正交，共同形成 sender 树的**静态类型理论**。

### 代价与边界

- **注解嗓门变大**：每个 adaptor 头上多一坨 effect 注解。spec 应当为常用 adaptor 提供"effect preset"宏（`@@Async::Effects::standard_then`）。
- **不解决"内容性副作用"**：例如"这个 then 会修改某个全局变量"——effect rows 描述的是**结构性**副作用（控制流、资源、能力），不是任意 side effect。

### 抽象层归属

L1（effect vocabulary）+ L3（adaptor 注解）。

---

## F4 · Reflection-Driven Adaptor Genesis

### 问题

写一个新的 adaptor，目前需要：

1. 写一个 sender 类型，定义 `sender_concept`、`completion_signatures`。
2. 写一个 `connect()` 把 receiver 包成 op-state。
3. 在 op-state 的 `start()` 里实现逻辑。
4. 处理取消、错误、值传递。
5. 维护 effect row 注解（F3）。

——五步里只有第 3 步是**业务**，其余 90% 是**样板**。新手写错任何一步都会被模板错误劝退。

### 设计

用 **P2996 反射 + consteval blocks** 把 adaptor 写法降级为"只写业务函数"：

```cpp
namespace Async {

// 用户只需要写这个函数体。
//   in: 上游完成的值
//   out: 下游应当收到的值
//   stop: stop_token（可选）
template<class In, class Out>
concept AdaptorBody = requires(In in, stop_token stop, Out out) {
    /* ... 由具体 adaptor 决定 ... */
};

// 自动 codegen。
template<auto Body>      // Body 是一个 reflection of a function
[[=auto_emit_effects<Body>()]]   // 注解从 Body 反射推导
class adaptor_from = /*...*/;

} // namespace Async

// 用法
constexpr auto throttle_body = ^^[](auto& state, auto value, auto sink) {
    auto now = clock::now();
    if (now - state.last_emit < state.period) {
        return;   // 丢弃
    }
    state.last_emit = now;
    sink.emit(std::move(value));
};

using throttle = Async::adaptor_from<throttle_body>;
```

#### consteval blocks 的角色

P2996 引入 `consteval { ... }` 块，允许在类作用域内执行编译期代码以**生成**成员。`adaptor_from` 用 consteval block：

1. 反射 `Body` 的签名，推导 `In / Out / State` 类型
2. 自动生成 sender + op-state 的样板
3. 从 `Body` 的反射元数据（是否 throw、是否 alloc、是否调用 stop）推导 effect rows
4. 用 `define_static_string` 给生成的类型起有意义的名字

#### 自动 effect 推导

通过反射 `Body` 的**实际**调用关系，能在编译期识别：

- `throw` 表达式 → `Throws`
- `new` / 内部容器扩容 → `Allocates`
- `stop.request_stop()` → `PropagatesStop`
- `sink.emit_on_scheduler()` → `ChangesScheduler`

这让 effect 注解从"用户手写"变成"框架推导"。**用户写错的可能性归零**。

### 与现有 adaptor 的关系

现有 stdexec adaptor（`then`、`let_value`、`when_all`）**继续手写**——它们是核心原语，需要精细控制。`adaptor_from` 是给**用户 / 应用层**写新 adaptor 时用的，把门槛从"读完整个 stdexec 实现"降到"写一个函数"。

### 代价与边界

- **反射对编译器的要求高**：目前只 clang-p2996 完整支持。spec 应当提供 fallback：在不支持完整反射的编译器上，`adaptor_from` 退化成"手写 + 注解"模板。
- **不适用所有 adaptor**：例如 `when_all` 这种"多输入异步组合"的，不是单一 `Body` 函数能描述的——它需要内部 spawn + 计数。这类 adaptor 仍然手写。

### 抽象层归属

L3（adaptor genesis） + L7（extension）。

---

## F5 · Compile-Time Topological Scheduling

### 问题

sender 树的**拓扑**（哪些节点可并行、哪些必须串行）目前在**运行期**通过 `let_value` / `when_all` 的嵌套表达。编译器看不到"整体形状"——它只看到一层层 op-state。后果：

1. 错过整体优化机会（例如"两个独立子树合并到同一个 worker"）。
2. 无法静态报告"这条管线的关键路径长度"。
3. 拓扑顺序错误（写了 `let_value` 但本应 `when_all`）只有运行期才能看出。

### 设计

利用 sender 类型 = 整树类型这一事实，在 consteval 中**遍历类型**，得到 DAG 表示：

```cpp
namespace Async::Topology {

// sender 树 → DAG 描述（编译期 std::array<Node>）。
template<class S>
consteval auto extract_dag() -> std::array<Node, /*N from reflection*/>;

// DAG 上的查询。
template<class S>
constexpr int critical_path_depth = /* topological sort + longest path */;

template<class S>
constexpr int parallel_width = /* DAG 的最大反链 */;

template<class S>
constexpr bool has_redundant_serialization = /*...*/;

} // namespace Async::Topology
```

#### 用法

```cpp
auto pipeline = build_pipeline();

static_assert(Async::Topology::critical_path_depth<decltype(pipeline)> <= 8);
static_assert(Async::Topology::parallel_width<decltype(pipeline)> >= 4);
static_assert(!Async::Topology::has_redundant_serialization<decltype(pipeline)>);
```

这让"管线性能特性"成为**编译期断言**。一旦你 refactor 时无意识地把 `when_all(a, b)` 写成 `let_value(a, [&](...){ return b; })`，critical_path_depth 会增加 1，断言失败。

#### 优化的二次利用

framework 内部可以基于 DAG 在编译期做：

1. **小子树融合**：两个连续 `then(f) | then(g)` 在编译期已知时合成 `then(g∘f)`（functor 律）。
2. **资源预分配**：DAG 知道运行期最多有几个并发 op-state，可以预分配栈空间（在 `sync_wait` 时分配整块，避免逐个 alloc）。
3. **调度提示生成**：DAG 自动推导 "这条边跨调度器" → 在跨边处插 `continues_on`。

### 与 F1–F3 的协作

- F3 的 effect rows + F5 的 DAG 拓扑共同构成 sender 树的**完整静态档案**——可以用来做严格的**类型层 contract**：" 这条树不会 throw、关键路径 ≤ 8 深度、最大并行 16 宽。"
- F2 的 capability lattice 决定 DAG 边能不能"跨调度器"——某些 capability 组合下跨边非法。

### 代价与边界

- **DAG 提取的 consteval 复杂度高**：sender 树深 100 层时，consteval 函数复杂度可能进 seconds 级。spec 应当给"最大可分析深度"上限（建议 64）。
- **不能处理动态构造的 sender 树**：例如运行期决定"加不加 retry"——这种树的拓扑只能 partial 推导。

### 抽象层归属

L1（核心机制） + L6（patterns 利用 DAG 信息做更聪明的 pattern）。

---

## F6 · Deterministic Replay

### 问题

并发程序的 bug 难复现。Mashiro 的取消 / 调度 / 异步 IO 都有时序敏感——本机跑通的 bug，CI 上挂了，没法复现。

### 设计

利用 F3 effect rows + F5 DAG 的静态信息，在调试模式下**记录**每个 effect 的实际发生序列，构成一条 **trace**；replay 时按 trace 模拟 scheduling，得到 deterministic execution。

```cpp
namespace Async::Replay {

// 模式：record / replay / live。
enum class Mode { Live, Record, Replay };

// scheduler 包装器：根据 mode 决定是真 schedule、记录、还是按 trace 回放。
template<class InnerSched, Mode M>
struct replay_aware_scheduler {
    /* 实际工作的 scheduler */
    InnerSched inner;
    /* trace 文件路径 */
    std::filesystem::path trace_path;

    /* 在 connect() 时根据 Mode 做不同行为 */
};

// 入口
auto record(auto pipeline, std::filesystem::path trace);
auto replay(auto pipeline, std::filesystem::path trace);

} // namespace Async::Replay
```

#### 关键技术点

1. **trace 记录粒度**：仅记录"非确定性事件"——线程间 schedule 顺序、IO 完成时间、stop 注入时刻。值与函数调用是确定的，不必记。这让 trace 文件小（GB 级 production trace 通常 < 100MB）。
2. **回放模式下的 scheduler**：所有 scheduler 接受 trace；在回放模式下，schedule() 不真把工作扔给线程池，而是按 trace 顺序在主线程逐步执行。**线程数 = 1，但行为等价**。
3. **effect 校验**：回放过程中如果 effect rows 与 trace 不一致（例如 trace 说这里应当 throw，实际不 throw），立刻 abort 并报告"哪一步发散"。

#### 用法

```cpp
// 生产环境出 bug，记录 trace
auto p = build_pipeline();
Async::Replay::record(p, "/var/log/crash-2026-06-17.trace");
// crash 时 trace 文件落盘

// 开发环境 replay
auto p2 = build_pipeline();
Async::Replay::replay(p2, "/var/log/crash-2026-06-17.trace");
// 在 debugger 里单步走，完全 deterministic
```

#### 与 F1–F5 的协作

- F1 TypedEnv：trace 知道每一步 env 的精确类型，replay 时能精确重建。
- F3 effect rows：trace 只记录 effect rows 中允许的事件——为 trace 给了**静态 schema**。
- F5 DAG：知道整树拓扑，回放时按 DAG 顺序而不是线性顺序——保留并行结构供调试。

### 代价与边界

- **trace 不抓非托管状态**：例如某个原子变量被业务代码修改——trace 不抓它的修改时序。若 bug 是 race condition 在业务原子上，replay 复现不了。
- **record 模式有开销**：~5–10% 性能损失。spec 应当让 record 默认关闭，crash 触发或显式启用。

### 抽象层归属

L7（diagnostic extension）—— 它不进 L1–L6 主线，是"侧出"的可选 feature。

### F6+ Beacon Schema 升级路径（v0.3 增补 · 来自 Unit V）

`StructuredLogger` 当前是 *runtime-string-keyed*——字段名是 `std::string`，类型靠运
行时检查。这跟 Mashiro 的"约定升类型"哲学不一致，但又不能直接重写：项目里几百处调
用点全要改。

**Beacon\<Schema\>** 的升级路径不要求重写——它走 **加法叠加** 而非 *替换*：

```cpp
// 阶段 1：定义 schema（一个 struct，字段 = 注解）
struct RpcSpan {
    [[=Beacon::Field("method", required)]]      std::string_view method;
    [[=Beacon::Field("latency_ns", monotonic)]] std::uint64_t    latency_ns;
    [[=Beacon::Field("status_code")]]           int              status;
    [[=Beacon::Field("trace_id", trace_root)]]  std::uint64_t    trace_id;
};

// 阶段 2：用 reflection 自动派生 schema 校验器
template <typename Schema>
constexpr auto make_validator()
    -> std::function<bool(const Beacon::Event&)>
{
    return [](const Beacon::Event& e) {
        bool ok = true;
        template for (constexpr auto field : std::meta::nonstatic_data_members_of(^^Schema)) {
            if (!e.has_field(std::meta::identifier_of(field))) ok = false;
        }
        return ok;
    };
}

// 阶段 3：在 StructuredLogger 旁挂一个 typed wrapper
template <typename Schema>
class TypedBeacon {
public:
    explicit TypedBeacon(StructuredLogger& sink) : sink_{sink} {}
    void emit(Schema s) {
        // 反射 Schema 字段 → 转发给底层 StructuredLogger
        template for (constexpr auto field : std::meta::nonstatic_data_members_of(^^Schema))
            sink_.add_field(std::meta::identifier_of(field), s.[:field:]);
        sink_.commit();
    }
private:
    StructuredLogger& sink_;
};
```

**升级哲学**——*老代码不动*，新代码逐步用 `TypedBeacon<MySchema>`；当某个高频调用点
转完后，*该路径上的字段名拼写错误* 变成编译错误。Spec 文档与运行时 trace 之间的
*同步成本* 由反射承担。

**与 F4 (Adaptor Genesis) 的关系**：F4 是 "*在编译期生成 adaptor*"；Beacon Schema 是
"*在编译期生成 logger 字段表*"——它们共享同一套 P2996 反射机制，*不引入新机制*。

### 验证 checklist

- 一条 `TypedBeacon<RpcSpan>` 调用，在编译期就能拒绝缺字段 / 类型错配；
- 老的 `StructuredLogger` 调用仍然可用（向后兼容）；
- TypedBeacon 不引入新 allocation（schema 字段名 = `std::meta::identifier_of` 返回的
  constexpr string）。

---

## F7 · Structured Resource Lifetimes

### 问题

Mashiro 用 Nursery 表达"作用域内的所有 task 必须结束"。这是协程世界的 RAII。但**资源**（文件句柄、socket、GPU 资源）目前仍由经典 C++ RAII 管理。两者**不**统一：

1. RAII 析构是**同步**的——若资源释放需要异步操作（`co_await close_async()`），RAII 处理不了。
2. RAII 析构无法响应取消——析构期间收到 stop，怎么办？
3. 异步资源没有"作用域内一定释放"的 Nursery 式保证。

### 设计

引入 **AsyncResource<R>** 概念 + **scoped_resource** 机制，把异步资源融入 nursery 作用域：

```cpp
namespace Async {

template<class R>
concept AsyncResource = requires(R r, stop_token stop) {
    { r.async_close(stop) } -> sender_of<void>;
};

// 在 nursery 作用域内**异步获取**资源；nursery 关闭时**异步释放**。
template<AsyncResource R>
auto scoped_resource(Nursery& n, auto async_open) -> Task<R&>;

} // namespace Async

// 用法
co_await Async::open_nursery([&](Async::Nursery& n) -> Async::Task<void> {
    auto& file = co_await Async::scoped_resource<AsyncFile>(n,
                    AsyncFile::open("/data/x"));
    auto& conn = co_await Async::scoped_resource<DbConn>(n,
                    DbConn::connect("host:port"));

    co_await use(file, conn);
    // 离开 nursery 时：
    //   - file.async_close() 与 conn.async_close() 被异步执行
    //   - 顺序：LIFO（与 RAII 一致）
    //   - 各自带 stop_token：若关闭过程被取消，按 spec 规定逃逸
    co_return;
});
```

#### 关键技术点

1. **生命周期与 nursery 一体**：资源在 nursery 内出生、在 nursery 关闭时释放——**一种保证**：永不泄漏。
2. **释放仍异步**：`async_close` 返回 sender，nursery 在关闭时把所有 close 串成 sequenced，并按 LIFO 取消 / 等待。
3. **取消语义清晰**：spec 规定 close 期间收到 stop 时的两种策略——`graceful`（继续 close 但有时间预算）与 `abort`（立即放弃 close，资源进入"半释放"状态由 OS 收尾）。

#### 与 F3 effect 的协作

`scoped_resource` 引入新 effect `OwnsResource` —— effect rows 可以静态检查"这个 sender 树是否持有未释放的 AsyncResource"。这让 spec 能在编译期发现"资源泄漏"（例如把 AsyncFile 从 nursery 内 leak 到外面）。

### 代价与边界

- **资源类型必须实现 `async_close`**：legacy 同步资源需要 wrap。spec 应当提供 `sync_resource_adaptor` 把同步 RAII 包成 AsyncResource。
- **不解决跨 nursery 共享**：若你想一个资源被多个 nursery 共享（例如 connection pool），那是另一种关系，spec 把它推给 user-land。

### 抽象层归属

L5（structured concurrency）—— 把 Nursery 的作用域语义扩展到资源。

---

## F1–F7 协作图

```
          F2 Capability Lattice
                  │
                  ▼ 描述
F1 TypedEnv ─────► Scheduler ◄─── 注解 ──── F4 Adaptor Genesis
   │                  ▲                            │
   │ 携带              │ 提供                        │ 生成
   ▼                  │                            ▼
 sender ─── 标注 ───► F3 Effect Rows ────► adaptor 自动 effect
   │                  │                            │
   │ 形成               │ 描述                        │
   ▼                  ▼                            │
 树类型 ─── 反射 ──► F5 Topological DAG ◄────────────┘
   │                  │
   │ 静态档案           │ 喂给
   ▼                  ▼
 F6 Replay ────────► trace schema
   │
   │ 嵌入
   ▼
 F7 Structured Resources ◄── 用 Nursery 包资源
```

每条 feature 都**严格落在某一层**，不破坏 L0–L7 的纵向分层。**横向**通过反射 / 注解互通——这是 spec 一以贯之的设计哲学：**信息流通靠类型与注解，而非运行时全局表**。

---

## Unit IV 结语

这 7 个 feature 不是凭空提出。它们的共同立意是：

> **把 spec 中目前"靠约定 / 靠 review"维持的东西，提升为"靠类型 / 靠反射"自动保证。**

C++20–26 给我们的工具——concept、consteval、reflection、coroutine、stdexec——恰好都是为"把运行时检查提升为编译期保证"而生。Mashiro 不用这些工具是浪费时代红利；强行套用则是噪声。Unit IV 的设计哲学是**只在能净化语义、降低事故、提升性能的位置使用**。

设计 framework 与设计语言不一样——你不能轻易改语法、不能加新关键字。但你可以通过**类型 + 注解 + 反射**让"约定"变成"契约"，让"评审"变成"编译"。这就是 Mashiro 异步框架对 C++ 生态的全部主张。

---

# 全卷结语

到此处全卷四单元结束。回头看，路线大致是：

| 单元 | 视角 | 输出 |
|------|------|------|
| Unit I (L1–L12) | 学习者 | 12 讲 + 4 Interlude，理解 spec 的全部词汇 |
| Unit II (Lab 1–12) | 实践者 | 12 个动手 lab，从打印到 backend 全覆盖 |
| Unit III (案例 / 性能 / 陷阱) | 救火者 | 3 案例剖析 + HALO/AllocCheck/CancelProbe 用法 + 12 陷阱 |
| Unit IV (F1–F7) | 设计者 | 7 项 framework feature 扩展，自顶而下设计 |

最后一段题外话——

Mashiro 的 spec 总共 10 份文件、近 200 页 markdown。把 spec 翻译成讲义，**意外的发现是**：spec 中有 80% 的内容是**可以被类型 + 反射自动验证**的——也就是 Unit IV 的方向。spec 写作者把这部分当文档来写，是因为他们写 spec 的时候 C++26 反射还没成熟；几年后回头看，这 80% 应当全部从 markdown 走进 `static_assert` 与 `consteval`。

讲义的真实价值，**不**在于它替你读完了 spec；它在于它告诉你：

> spec 的某些约定本来就**应当**被编译器检查；
> 凡是当前由文档维持的不变量，迟早会被某次重构悄悄破坏；
> 把"我承诺"翻译成"我证明"，是工程团队的成熟度标志。

异步编程难。结构化并发让它**没那么难**。Mashiro 的设计——再加上 Unit IV 的扩展——把"没那么难"推到**默认正确**。

祝你在键盘上享受这个过程。

—— 任课讲师，2026-06-17

---

# Unit V · 通用异步原语扩展（Primitive Extension）

> **本单元立意**：前面四个单元是 *自给自足* 的——只用 Unit I 的 12 讲词汇就能解释 Unit IV
> 的全部 feature。但工业界（Trio / Tokio / Cats Effect / Loom / Erlang OTP / Swift /
> Kotlin / Go）在过去十几年中沉淀出一批 *跨语言* 的异步建模原语；它们值得我们与
> Mashiro 的现有词汇做一次对账，看清楚 *哪些应当吸纳、哪些已经隐含、哪些不必引入*。
>
> 本单元的输入来自项目内部文档 `docs/cpp/general-async-primitives.md`（共 20 个候选原语），
> 输出三件事：
>
> 1. **§A 行业建模综述**——10 个主流异步运行时的"一等原语"取舍；
> 2. **§B 决策表**——20 个候选原语 × 三条收录标准 → 收 / 选修 / 不收；
> 3. **§C 教学融入**——收录的原语落到哪一讲 / 哪一 Lab / 哪一个 F-feature。
>
> 这一单元的写作姿态是 **设计审稿**，不是新教学。读完它，你应当能回答：*"为什么
> Mashiro 不直接抄 Tokio？"*——因为 Mashiro 已经把它们 *算出来* 了。

---

## §A 行业建模综述

异步编程的"原语层"并非天经地义。我们今天看到的 sender/receiver、coroutine、structured
concurrency、channel、actor、effect type，是过去 50 年间多次范式迁移的产物。本节按
**时间脉络** + **范式坐标** 两个维度，把 10 个具有代表性的运行时排成一张地图。

### A.1 时间脉络（异步原语的考古层）

| 年代 | 代表 | 一等原语 | 关键贡献 |
|------|------|---------|---------|
| 1973 | Hewitt Actor | actor, mailbox, become | 把"对象 + 消息"作为并发的原始单位 |
| 1986 | Erlang/OTP | process, mailbox, link, supervisor | **监督树** = 故障隔离的代数 |
| 1985–95 | CSP / occam / Go | channel, select | **通信替代共享**（Hoare 命题） |
| 2000s | Twisted / Tornado | Deferred, callback | 第一代回调式异步（callback hell） |
| 2010s | Node.js / asyncio | Promise / Future + await | **co_await 雏形**，但无结构化退出 |
| 2018 | Trio / curio (Python) | nursery, cancel scope | **结构化并发**首次正式命名 |
| 2018+ | Tokio (Rust) | task, spawn, select!, JoinSet | 单 binary 高性能 work-stealing |
| 2017+ | Cats Effect / ZIO (Scala) | IO/ZIO monad, Fiber, effect row | **代数效应**入纯函数世界 |
| 2021+ | Swift Concurrency | TaskGroup, async let, actor | **语言层** structured concurrency |
| 2023 | Loom (Java) | virtual thread, StructuredTaskScope | **运行时层** structured concurrency |
| 2024+ | C++ stdexec (P2300) | sender, scheduler, env | **类型层** 异步代数（receiver = CPS） |

**观察**：原语层的演化方向是 *愈来愈结构化*、*愈来愈类型化*。Mashiro 站在这条曲线
的右端——它直接采用 sender/receiver + structured concurrency + reflection 三件套，等于
"出生即继承了 40 年的失败教训"。

### A.2 范式坐标（横轴：通信模型，纵轴：调度模型）

```
                通信：消息 ← ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ → 通信：共享状态
                  │                                          │
   抢占式 ┌───────┼──────────────────────────────────────────┼─────────┐
          │ Erlang / Akka            │  Java 线程 + monitor │           │
          │  ◤ actor + supervisor    │  ◤ 共享内存 + 锁     │           │
   调度   │                          │                      │           │
   ─ ─ ─ ┼ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ┼ ─ ─ ─ ─ ─ ─ ─ ─ ─ ┼ ─ ─ ─ ─ ─ ┤
   协作式 │ Go / occam / CSP         │  Tokio / asyncio    │           │
          │  ◤ channel + select      │  ◤ task + await    │           │
          │                          │                      │           │
          │ Trio / Swift / Loom      │  Cats Effect / ZIO  │           │
          │  ◤ nursery + cancel      │  ◤ IO monad + Ref  │           │
          │                          │                      │           │
          └──────────────────────────┴──────────────────────┴───────────┘
                                              ▲
                                   Mashiro 在这里：
                                   协作式 × （消息为主，共享为辅）
                                   = stdexec + nursery + 反射验证
```

**Mashiro 的坐标** = 协作式调度（Scheduler 抽象的就是协作点） × 消息为主（sender 间
通过 completion-signature 通信，不通过共享） + 共享为辅（必要时 SeqLock / Ring Buffer
精确管控）。这一坐标位决定了它**应当继承** Trio/Swift/Loom 的"nursery + structured
cancel"血脉，**不必继承** Erlang 的"邮箱抢占调度"。

### A.3 十家运行时的一等原语清单（精读）

#### A.3.1 Erlang / OTP（1986）

- **process** —— 极轻量进程，独立堆，崩溃不污染其他 process；
- **mailbox** —— 每个 process 一个无界 FIFO，唯一通信信道；
- **link / monitor** —— 故障传播原语；
- **supervisor** —— 监督树节点，重启策略 = `one_for_one | one_for_all | rest_for_one`；
- **gen_server / gen_statem** —— 行为模板（OTP behaviour）。

**对 Mashiro 的启示**：监督树的 *重启策略代数*（已在 L8 / Supervised<Strategy>）是
Erlang 的核心遗产，已经被 Mashiro 吸收。Mailbox 抢占式调度则 *不* 适合 stdexec 模型。

#### A.3.2 Akka / Pekko（2010）

把 Erlang Actor 移植到 JVM，加上 typed actor（编译期校验 message 类型）。
**遗产**：typed actor 等价于 Mashiro 的 sender<Channel<T>>——*类型化邮箱*。

#### A.3.3 Go（2009）

- **goroutine** —— M:N 调度的轻量协程；
- **channel** —— 有界 / 无界双向通道，`<-ch` 阻塞读；
- **select** —— 多路通道复用（带 default 实现非阻塞）；
- **context.Context** —— 取消 + 超时 + value 传播。

**对 Mashiro 的启示**：
- channel ↔ Mashiro `MpscQueue`/`SpscQueue`（已有），可以包成 sender adaptor；
- select ↔ stdexec `when_any`（Mashiro 已经通过 P3149 提供）；
- context.Context ↔ Mashiro 的 `Env` + `stop_token`（已有，且更细粒度）。

**结论**：Go 的原语在 Mashiro 中已 *全部覆盖*，不必新增。

#### A.3.4 Twisted / Tornado / Node.js / asyncio（2002–2014）

- **Deferred / Promise / Future** —— 单次完成的占位符；
- **callback / .then / await** —— 链式或同步语法的延续传递。

**遗产**：教训为主。Promise 链 = 没有结构化的 nursery，cancel 语义混乱。Mashiro
*显式拒绝* 这个层次的原语（sender 已经覆盖 Promise，并多出 cancel/env）。

#### A.3.5 Trio / Python anyio（2018）

- **nursery** —— 唯一允许 spawn 的对象；离开 `async with nursery:` 块 = 所有子任务完成或被取消；
- **cancel scope** —— 可嵌套的取消范围；
- **MemoryChannel** —— 有界 channel，可关闭。

**对 Mashiro 的启示**：nursery 这一概念被 Mashiro 一字不改地继承（L5 已讲）；
cancel scope ↔ Mashiro 的 inplace_stop_source / bridge_stop_token。

#### A.3.6 Tokio（2018）

- **#[tokio::main] / Runtime** —— 多线程 work-stealing 运行时；
- **JoinHandle / JoinSet** —— 任务结果聚合；
- **mpsc / oneshot / broadcast / watch channel** —— 四种通道按"读者基数 × 历史保留"划分；
- **select! 宏** —— 多 future 复用；
- **CancellationToken**（新加）—— 显式 cancel；
- **tracing 框架** —— scope/span 追踪。

**对 Mashiro 的启示**：
- Tokio 的 **四种 channel** 提供了一张分类学：MPSC、oneshot、broadcast、watch——是
  L5 Stream / Topic / Channel 一节最好的 *分类参考*；
- JoinSet ↔ Mashiro `counting_scope` + `when_all`；
- tracing ↔ Mashiro `Beacon`/`StructuredLogger`。

#### A.3.7 Cats Effect / ZIO（2017+）

- **IO[A] / ZIO[R, E, A]** —— 三参数效应类型（环境 / 错误 / 结果）；
- **Fiber** —— 可独立取消、join、bracket 的协程值；
- **Resource** —— 自动 acquire/release 的资源 monad；
- **Ref / Deferred / Semaphore / Queue** —— 并发原语集合；
- **STM** —— 软件事务内存。

**对 Mashiro 的启示**：
- ZIO 的 R/E/A *三轴* 就是 Mashiro F3 effect rows 的灵感来源——env / error_signature /
  value_signature 三个轴恰好对应；
- bracket（acquire/use/release）↔ Mashiro Tether / Resource Nursery（已有，L7）；
- STM 不收：Mashiro 是协作式 + 单线程为主，事务带来的复杂度大于收益。

#### A.3.8 Swift Concurrency（2021）

- **async / await** —— 关键字；
- **Task / Task.detached** —— 任务句柄；
- **TaskGroup / async let** —— 结构化并发；
- **actor** —— 类型级隔离的对象（消息串行化由编译器保证）；
- **AsyncSequence** —— 异步迭代器；
- **MainActor / GlobalActor** —— 调度域标注。

**对 Mashiro 的启示**：
- actor 注解 ↔ Mashiro F1 TypedEnv + Apartment<Tag>（已有，且更通用——Mashiro 不强制单线程串行化）；
- AsyncSequence ↔ Mashiro AsyncSeq<T>（已有，L5）；
- MainActor 注解 ↔ Mashiro Apartment<Main>（已有，L1）。

#### A.3.9 Kotlin Coroutines（2018）

- **suspend fun** —— 关键字；
- **CoroutineScope / Job** —— 结构化作用域；
- **Flow / SharedFlow / StateFlow** —— 三种流：冷流 / 共享广播 / 状态镜像；
- **Channel** —— 同 Go；
- **Dispatcher** —— Main / IO / Default / Unconfined。

**对 Mashiro 的启示**：Kotlin 的 **三种 Flow** 提供另一张分类参考——cold / hot+broadcast /
hot+latest_only，是 L5 + Topic 原语章节的好对照。

#### A.3.10 Project Loom（Java，2023）

- **virtual thread** —— JVM 内置的 M:N 协程；
- **StructuredTaskScope** —— Java 21 引入的 nursery；
- **ScopedValue** —— 替代 ThreadLocal 的作用域值。

**对 Mashiro 的启示**：Loom 证明了 nursery 模型已经从"小众"走入主流标准库。Mashiro
的方向是对的。

### A.4 横向对比一览（同一个概念在 9 家叫什么）

| Mashiro 词汇 | Erlang | Go | Trio | Tokio | ZIO | Swift | Kotlin | Loom |
|------------|--------|-----|------|-------|------|-------|--------|------|
| sender | – | – | async fn | async fn | ZIO | async fn | suspend fn | – |
| scheduler | scheduler | runtime | event loop | Runtime | RuntimeConfig | executor | Dispatcher | scheduler |
| env / context | dict | context.Context | trio.lowlevel | Extensions | R type param | TaskLocal | CoroutineContext | ScopedValue |
| nursery | supervisor | – | nursery | JoinSet | Scope | TaskGroup | CoroutineScope | StructuredTaskScope |
| stop_token | – | ctx.Done() | CancelScope | CancellationToken | interruption | Task.cancel | Job.cancel | (scope) |
| Channel | mailbox | channel | MemoryChannel | mpsc/oneshot | Queue | AsyncStream | Channel | – |
| Topic | gen_event | – | – | broadcast/watch | Hub | – | SharedFlow | – |
| Bulkhead | – | – | CapacityLimiter | Semaphore | Semaphore | – | Semaphore | – |
| Singleflight | – | sync.Once | – | OnceCell + lock | Hub | – | (手写) | – |
| Saga | – | – | – | – | ZManaged + bracket | – | – | – |

> 这张表是本课程持续引用的"罗赛塔石碑"。

### A.5 行业综述的结论

异步原语的**主干 8 类**已经收敛：
1. *单步占位*（Promise / Future / sender） —— Mashiro 已有
2. *调度 + 环境*（Scheduler / Dispatcher / Runtime） —— Mashiro 已有
3. *结构化作用域*（Nursery / TaskGroup） —— Mashiro 已有
4. *取消传播*（CancelScope / stop_token） —— Mashiro 已有
5. *点对点通道*（Channel / mpsc / oneshot） —— Mashiro 已有底层 queue，缺 sender 包装
6. *一对多广播*（Topic / SharedFlow / broadcast / watch） —— Mashiro 缺
7. *并发上限*（Semaphore / Bulkhead / CapacityLimiter） —— Mashiro 缺
8. *资源生命周期*（Resource / bracket / Tether） —— Mashiro 已有

**两类 *局部* 主干**（不是所有运行时都有，但出现频率 ≥ 3）：
- 9. *合流 / 仲裁*（Race / Quorum / select） —— Mashiro 通过 `when_any` 已覆盖
- 10. *重试 / 弹性*（Retry / CircuitBreaker / Backoff） —— Mashiro 缺

**三类 *专业* 原语**（少数高级运行时才有，需要看 Mashiro 是否真的需要）：
- 11. *请求合并*（Singleflight） —— 高 QPS 服务才用
- 12. *管线 / 编排*（Pipe / Saga） —— ETL/分布式才用

剩余的 *候选原语*（Rendezvous / Pool / Cache / Snapshot / Beacon / Chronograph /
Outcome / Edge / Apartment / Tether / Sluice / AsyncSeq / 8 个已讨论过的）我们在 §B
逐个过筛。

---

## §B 决策表：20 个候选原语 × 3 条收录标准

### B.1 收录标准（"三筛")

针对每一个候选原语，问三个问题：

| 编号 | 标准 | 通过条件 |
|------|------|---------|
| Q1 | **形态稳定** | 多家运行时（≥ 3）有等价物，命名与角色趋同 |
| Q2 | **语义独立** | 不能由已有 Mashiro 词汇 *零成本组合*；组合代价 ≥ 3 行 boilerplate |
| Q3 | **现有词汇不足** | Mashiro 的 spec 中没有等价物（包括 SeqLock / queue / Generator 等已有类） |

三个 *全* 通过 → **进入框架** + **进入课程**；
两个通过、一个边缘 → **选修原语**（教学覆盖、可选实现）；
≤ 1 个通过 → **不收录**（用已有词汇组合即可）。

### B.2 决策表（20 行）

| # | 原语 | Q1 形态稳定 | Q2 语义独立 | Q3 词汇不足 | 决定 |
|---|------|-----|-----|-----|------|
| 1 | Apartment\<Tag\> | ✓（Swift GlobalActor / Tokio LocalSet） | ✓ | △（已有 Scheduler 概念，但缺 *策略 tag*） | **选修** → F1 TypedEnv 内嵌实现 |
| 2 | Tether\<Acq,Rel\> | ✓（ZIO Resource / Swift withDependencies） | ✓ | ✗（L7 Resource Nursery 已覆盖） | **不收**（已有） |
| 3 | Sluice\<Policy\> | △（Tokio Semaphore + 策略，但未统一） | ✓ | ✓（Mashiro 缺背压泛型） | **进入**（与 Bulkhead 合并实现）|
| 4 | Beacon\<Schema\> | ✓（Tokio tracing / OTLP / Zipkin） | ✓ | △（已有 StructuredLogger，但缺 schema-typed） | **选修** → F6 Replay 升级版 |
| 5 | Chronograph\<Clock,Sched\> | ✓（asyncio sleep / Tokio time） | ✓ | △（schedule_at 已有，但 typed clock 缺） | **选修**（Lab 中点过） |
| 6 | Outcome\<S\> | ✓（ZIO Exit / Rust Result） | ✗（completion_signatures 已是它） | ✗ | **不收**（已有，是 stdexec 内建） |
| 7 | AsyncSeq\<T\> | ✓（Swift AsyncSequence / Kotlin Flow） | ✓ | ✗（L5 已覆盖） | **不收**（已有） |
| 8 | Edge\<S,Hooks\> | △（仅 Erlang link/monitor 近似） | ✓ | ✓（Mashiro 的 Event::Traits 是雏形） | **选修** → 进 F4 adaptor genesis |
| 9 | Rendezvous（Latch/Barrier/Phaser） | ✓（Java / Tokio Barrier / asyncio Barrier） | ✓ | ✓ | **进入** |
| 10 | Race & Quorum | ✓（Go select / Tokio select! / when_any） | ✗（when_any 已是 Race；Quorum 可以组合） | ✗ | **不收**（已有 `when_any`，Quorum 提供 helper） |
| 11 | Singleflight\<K,V\> | △（Go sync.Once / Caffeine） | ✓ | ✓ | **选修** → Lab 11 增补 |
| 12 | Pool\<T\> | ✓（DBPool / ConnectionPool 普遍） | ✓ | ✗（`ConcurrentObjectPool` 已有） | **不收**（已有；包成 sender adaptor 即可）|
| 13 | Bulkhead\<Tag,N\> | ✓（Hystrix / Resilience4j / Polly） | ✓ | ✓ | **进入**（与 Sluice 合并语义） |
| 14 | Retry\<Backoff\> & CircuitBreaker | ✓（Polly / Resilience4j / Tokio-retry） | ✓ | ✓ | **进入** |
| 15 | Pipe\<Stages...\> | △（仅 ZIO ZStream 接近） | ✗（用 stdexec then \| then \| ... 可组合） | △ | **不收**（已有 pipe operator） |
| 16 | Topic\<T\> | ✓（Tokio broadcast / Kotlin SharedFlow / Akka EventBus） | ✓ | ✓ | **进入** |
| 17 | Channel\<T\> | ✓（Go / Tokio mpsc / Kotlin Channel） | ✓ | △（`MpscQueue` 已有底层，缺 sender 包装） | **进入**（薄包装层） |
| 18 | Saga\<Steps...\> | △（ZIO bracket + Cats / dist-tx） | ✓ | ✓ | **选修** → Lab 12 增补 |
| 19 | Cache\<K,V,Eviction\> | ✓（Caffeine / Guava） | ✓ | ✓ | **选修** → Unit III 性能章节增补 |
| 20 | Snapshot\<T\> | △（RxJava BehaviorSubject / Kotlin StateFlow） | △（SeqLock 已覆盖快路径） | ✗（`Mashiro::SeqLock<T>` 已是它） | **不收**（已有） |

### B.3 决策汇总

| 决定 | 数量 | 原语 |
|------|------|------|
| **进入框架**（写入 spec + 课程主线） | 5 | Sluice/Bulkhead（合并）、Rendezvous、Retry+CircuitBreaker、Topic、Channel |
| **选修原语**（讲到 + 可选实现） | 7 | Apartment、Beacon、Chronograph、Edge、Singleflight、Saga、Cache |
| **不收录**（已有等价物或可零成本组合） | 8 | Tether、Outcome、AsyncSeq、Race/Quorum、Pool、Pipe、Snapshot、（隐含的）所有 sender 内建原语 |

> 这 5+7 个原语 *已经全部在 `general-async-primitives.md` 文档里被详细论证过*；本课程
> 不重复其语义定义，只决定 *是否进 Mashiro 框架* 与 *进哪个位置*。

### B.4 "为什么不收录"的细节抗辩

读到这里，敏感的读者会问：

> *Q*：为什么 Pool 不收录？Tokio/asyncio 都有连接池啊。
> *A*：因为 Mashiro 的 `ConcurrentObjectPool` 已经是 Pool 的数据层。我们只需要在 Unit III
>      Lab 11 中加一节 *"如何把 Pool 包成 sender adaptor `with_pool(pool) | then(...)`"*——这
>      是 *15 行模板代码*，不构成"独立原语"。
>
> *Q*：为什么 Pipe 不收录？ZIO ZStream 那么强大。
> *A*：因为 stdexec 的 `sender | then | let_value | then | when_all` 就是 Pipe。任何 Pipe<S1, S2, ...>
>      都可以用 `|` 写出来。引入 Pipe 名字会形成 *同义反复*。
>
> *Q*：为什么 Snapshot 不收录？Kotlin StateFlow 流行的不得了。
> *A*：因为 `Mashiro::SeqLock<T>` 已经是 *单写多读 Snapshot* 的本体；Mashiro 不打算追加 RxJava 风格
>      的 hot observable 包装。需要的人可以用 `SeqLock<T>` + `then(read_snapshot)` 自己包。
>
> *Q*：为什么 Race/Quorum 不收录但 Rendezvous 收录？
> *A*：Race = `when_any`，已经在标准里。Quorum = "前 K 个完成" 是 `when_any` 的 *泛化*，
>      可以用 N 次 when_any 实现；但 Rendezvous（Latch/Barrier/Phaser）是 *同步原语*
>      不是 *组合算子*——它需要内部维护计数与等待队列，不是 `when_any` 能覆盖的。
>
> *Q*：为什么 Beacon 只是选修不进主框架？
> *A*：因为 `StructuredLogger` 已经在 Mashiro 项目里跑了很久，把 Beacon 升格为一等原语意味着
>      *重写* StructuredLogger，技术债太大。F6 Replay 章节会教 *"如何把 Beacon 的 typed-schema
>      理念叠加到现有 logger 上"*，但保持 logger 不动。

---

## §C 教学融入与框架落点

### C.1 5 个 *进入框架* 原语的落点

#### C.1.1 Channel\<T\>：薄包装层

**框架落点**：`Mashiro::Async::Channel<T>` —— 在 `MpscQueue<T>` / `SpscQueue<T>` 之上加 sender 接口。

```cpp
namespace Mashiro::Async {

template <typename T, Capacity Cap = Unbounded>
class Channel {
public:
    // 发送端：返回 sender，完成 = 入队成功 / 通道关闭
    auto send(T value) -> SendSender<T>;
    // 接收端：返回 sender，完成 = 出队 / 通道关闭后 EOF
    auto recv()         -> RecvSender<T>;
    // 关闭：幂等
    void close() noexcept;
};

} // namespace Mashiro::Async
```

**教学融入**：
- Lesson 5（adaptors）增补一节 **"5.6 Channel 与背压"**；
- Lab 5（pipe）增补一个 *channel-based pipeline* 变体。

**实现要点**：
- 背压：用底层 SpscQueue 的有界容量 + sender 在满时挂起；
- 关闭：sender 完成通道是 `set_done`（不是 set_value），保持与 stop_token 一致；
- close 之后所有 recv 收到 EOF（set_done），所有 send 收到 channel_closed。

#### C.1.2 Topic\<T\>：一写多读广播

**框架落点**：`Mashiro::Async::Topic<T>` —— 内部 = 1 个 Unified Writer + N 个 SpscRing 订阅者。

```cpp
template <typename T, Strategy = Strategy::LatestN<16>>
class Topic {
public:
    // 发布：sender，完成 = 写入所有订阅者环
    auto publish(T value) -> PublishSender<T>;
    // 订阅：返回 subscriber，subscriber.stream() 是 sender<T>
    auto subscribe()      -> Subscriber<T>;
};
```

**Strategy 分类**（致敬 Kotlin SharedFlow / Tokio watch + broadcast）：
- `Strategy::Buffered<N>` —— 每个订阅者一个有界 ring，慢订阅者丢老消息；
- `Strategy::Latest` —— 只保留最新一条（= Kotlin StateFlow / Tokio watch）；
- `Strategy::Blocking` —— 慢订阅者反压上游（默认不用，需显式选）。

**教学融入**：
- Lesson 5（adaptors）**"5.7 Topic 的三种背压策略"**；
- Unit III 案例增补：*EventPump 重构* —— 当前 EventPump 是手写的 N×SpscRing；
  将其表达为 `Topic<Event, Strategy::Buffered<RingSize>>` *是不增加性能开销的语义提升*。

#### C.1.3 Sluice\<Policy\> + Bulkhead\<Tag,N\>：合并为容量门

**框架落点**：`Mashiro::Async::Bulkhead<Tag, N, Policy>`。

```cpp
template <typename Tag, std::size_t N, typename Policy = Policy::Reject>
class Bulkhead {
public:
    // 进入：sender，完成 = 拿到 permit
    auto acquire() -> AcquireSender<Tag>;
    // 离开：RAII 由 sender adaptor 包；用户不直接调
};

// 适配器：with_bulkhead(bh) | then(work)
template <typename Sender, typename BH>
auto with_bulkhead(Sender s, BH& bh);
```

**Policy 三选一**（致敬 Resilience4j）：
- `Policy::Reject` —— 满了就 set_done（让上游知道被拒绝）；
- `Policy::Wait` —— 满了就 enqueue 等待；
- `Policy::Timeout<D>` —— 等到 timeout 转 set_error(timeout)。

**教学融入**：
- Lesson 6（patterns）增补 **"6.7 容量门与背压"**；
- Lab 6 增补一个 *Bulkhead 守护 DB 连接* 的小例子。

#### C.1.4 Retry\<Backoff\> & CircuitBreaker：弹性原语

**框架落点**：sender adaptor。

```cpp
// Retry：失败时按 backoff 重试
template <typename Backoff>
auto retry(Backoff bo);                  // 用法：sender | retry(exp_backoff(...))

// CircuitBreaker：连续失败 K 次熔断 W 时间
template <typename Clock>
class CircuitBreaker { /* ... */ };

template <typename CB>
auto with_breaker(CB& cb);               // 用法：sender | with_breaker(cb) | then(...)
```

**Backoff 策略**：
- `linear(D)` / `exponential(D, factor, cap)` / `decorrelated_jitter(D)`（致敬 AWS SDK）。

**教学融入**：
- Lesson 6（patterns）增补 **"6.8 重试与熔断"**；
- Lab 6 增补一个 *Retry + Bulkhead 组合* 的微型例子。

#### C.1.5 Rendezvous：Latch / Barrier / Phaser

**框架落点**：`Mashiro::Async::{Latch, Barrier, Phaser}`。

```cpp
class Latch {                                  // 计数到 0 后全部释放（单向）
public:
    explicit Latch(std::size_t n);
    void count_down(std::size_t k = 1);
    auto wait() -> WaitSender;
};

class Barrier {                                // N 个 sender 同步到一点，可重置
public:
    explicit Barrier(std::size_t n);
    auto arrive_and_wait() -> ArriveSender;
};

template <std::size_t Phases>
class Phaser {                                 // 多阶段 barrier（致敬 java.util.concurrent.Phaser）
    /* ... */
};
```

**教学融入**：
- Lesson 8（structured concurrency 深化）增补 **"8.4 集合点：Latch/Barrier/Phaser"**；
- Lab 8 增补一个 *2-phase commit 雏形* 的练习。

### C.2 7 个 *选修原语* 的覆盖方式

| 原语 | 教学位置 | 实现指南 |
|------|---------|---------|
| Apartment\<Tag\> | F1 TypedEnv 章节，作为 Env 的一类典型 tag | 用 `with_env_tag<Apartment<Main>>` 实现 |
| Beacon\<Schema\> | F6 Replay 升级版（"如何不重写 StructuredLogger 而引入 typed schema"） | reflection 生成 schema 校验器 |
| Chronograph\<Clock,Sched\> | Lab 3（time / scheduler）选讲 | `schedule_at` + steady_clock 注解 |
| Edge\<S,Hooks\> | F4 Adaptor Genesis 章节 | reflection over `Event::Traits` |
| Singleflight\<K,V\> | Lab 11（性能优化）增补 | 用 `std::shared_future` + 哈希表 |
| Saga\<Steps...\> | Lab 12（复杂业务）增补；与 retry/bulkhead 串联 | 编译期编排 + 逆向补偿链 |
| Cache\<K,V,Eviction\> | Unit III "性能·调试·陷阱" 章节增补 | LRU/W-TinyLFU 选讲，sender adaptor 包装 |

### C.3 与 F1–F7 的相互编织

新原语**不会**破坏 F1–F7 的分层，它们的关系如下：

```
   F1 TypedEnv ──持有──► Apartment<Tag>               （选修原语）
   F2 Capability ──收录──► Bulkhead Tag, Channel cap   （进入原语）
   F3 Effect Rows ──标注──► Retry/CircuitBreaker 副作用 （进入原语）
   F4 Adaptor Genesis ──生成──► Edge/Saga 的 hooks       （选修原语）
   F5 Topological DAG ──表达──► Pipe（已不收，但 DAG 仍承担其角色）
   F6 Replay ──升级──► Beacon Schema                    （选修原语）
   F7 Structured Resources ──覆盖──► Tether（已不收，Nursery 已是）
```

**新增的 5 个进入原语全部落在 L0–L7 已有的层里**：

| 原语 | 落在哪一层 | 是否需要新接口 |
|------|----------|-------------|
| Channel | L1 Scheduler-adjacent + L2 Adaptors | 是（薄包装）|
| Topic | L2 Adaptors | 是（薄包装 + 策略 tag）|
| Bulkhead | L2 Adaptors | 是 |
| Retry/CircuitBreaker | L2 Adaptors | 是 |
| Rendezvous | L0 Foundations + L2 | 是（Latch/Barrier 类）|

**没有任何新原语需要在 L3–L7 上新增基础设施**——这是设计良好的标志。

### C.4 课程主线增补一览

| 单元 | 章节 | 新增内容 | 字数预估 |
|------|------|---------|---------|
| Unit I L5 | 5.6 Channel | sender 包装 + 关闭语义 | ~600 |
| Unit I L5 | 5.7 Topic | 三种 Strategy + 与 EventPump 对照 | ~800 |
| Unit I L6 | 6.7 Bulkhead | Policy::{Reject/Wait/Timeout} | ~500 |
| Unit I L6 | 6.8 Retry/CircuitBreaker | Backoff + Breaker | ~500 |
| Unit I L8 | 8.4 Rendezvous | Latch/Barrier/Phaser | ~500 |
| Unit II Lab 5 | channel pipeline 变体 | 30 行示例 | — |
| Unit II Lab 6 | Retry + Bulkhead 组合 | 30 行示例 | — |
| Unit II Lab 8 | 2PC 雏形（用 Barrier） | 40 行示例 | — |
| Unit II Lab 11 | Singleflight 选讲 | ~400 | — |
| Unit II Lab 12 | Saga 选讲 | ~400 | — |
| Unit III 性能 | Cache 章节 | LRU 与 sender 集成 | ~400 |
| Unit III 陷阱 | Topic 慢订阅者 / Channel 死锁 / Retry 雪崩 | 3 条新陷阱 | ~600 |
| Unit IV F6 | Beacon Schema 升级路径 | ~400 |
| **合计** | | **~5100 字** | |

这些增补会作为 *v0.3* 写入。**v0.2（即当前版本）已经完成本单元的 *决策与落点设计***——
后续按 §C.4 的小节逐次写正文即可。

### C.5 Unit V 结语：为什么"少即是多"

工业界 20 年沉淀出 20 个原语，Mashiro 评估后 *只收 5 个新的 + 选修 7 个*。这不是
保守，而是 **复用既有词汇的胜利**：

- stdexec 的 sender / when_any / Nursery / stop_token —— 已经覆盖 Race / Quorum / 结构化并发 / 取消传播；
- Mashiro 已有的 SeqLock / MpscQueue / SpscQueue / ConcurrentObjectPool / StructuredLogger ——
  已经覆盖 Snapshot / Channel 底层 / Pool / Beacon 写路径；
- C++ 的 `|` operator + concept —— 已经覆盖 Pipe / Saga 的组合层。

**剩下的 5 个**（Channel/Topic/Bulkhead/Retry/Rendezvous）是 stdexec 标准里 *尚未给出
spec*、Mashiro 现有词汇 *不能零成本组合* 的真空地带——它们的引入是 *补完*，不是
*堆叠*。

这正是本课程从第一讲就在强调的设计哲学：

> **正确的抽象不是"加一个东西"，而是"看清楚一个东西其实已经在那里"。**

当我们把 Tokio 的 broadcast、Kotlin 的 SharedFlow、Akka 的 EventBus 三件套都收敛为
`Topic<T, Strategy>` 时，我们不是 *发明* 了 Topic，而是 *为已经存在的概念找到一个
与 sender 语义对齐的名字*。这种命名上的 *赢*，才是框架设计真正的赢。

—— Unit V 完，2026-06-17

