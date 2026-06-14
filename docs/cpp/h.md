PowerShell 7.6.0-preview.4

   A new PowerShell preview release is available: v7.7.0-preview.2
   Upgrade now, or check out the release page at:
     https://aka.ms/PowerShell-Release?tag=v7.7.0-preview.2

Loading personal and system profiles took 5031ms.
C:\Users\nekom> G:
G:\> cd .\Teaching\
G:\Teaching> cd .\Vulkan\
G:\Teaching\Vulkan [main ≡ +6 ~3 -0 !]> claude --resume 8888a718-f0a6-4c48-9b20-e1bd96900159
╭─── Claude Code v2.1.145 ─────────────────────────────────────────────────────────────────────────────────────────────╮
│                                                    │ Tips for getting started                                        │
│                    Welcome back!                   │ Run /init to create a CLAUDE.md file with instructions for Cla… │
│                                                    │ ─────────────────────────────────────────────────────────────── │
│                       ▐▛███▜▌                      │ What's new                                                      │
│                      ▝▜█████▛▘                     │ Added `claude agents --json` to list live Claude sessions as J… │
│                        ▘▘ ▝▝                       │ Added `agent_id` and `parent_agent_id` attributes to `claude_c… │
│                                                    │ Status line JSON input now includes GitHub repo and PR informa… │
│   Opus 4.7 with xhigh effort · API Usage Billing   │ /release-notes for more                                         │
│                 G:\Teaching\Vulkan                 │                                                                 │
╰──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────╯
  ⎿  SessionStart:startup says: 🚀 Vulkan Session started
     ▸ Time: 2026/06/11 11:39:21
     ▸ Directory: G:\Teaching\Vulkan

     ▸ Git branch: main

     ⚠️  Uncommitted changes (7 files):
       📝 MakePresets.json
       📝 Mashiro/include/Mashiro/Geom/Geom.h
       📝 Mashiro/include/Mashiro/Schedular/Generator.h
       📝 Mashiro/include/Mashiro/Surface/Window.h
       ❓ Mashiro/include/Mashiro/Core/Annotation.h
       ... (2 more files)

     🧠 Obsidian project KB: research repo candidate
       - Detected markers: .git, docs, scripts
       - Suggested command: /kb-init

     📦 Package manager: npm (available)

     📋 Todos:
       No todo file found (TODO.md, docs/todo.md etc)

     🔌 Enabled plugins:
       - code-review
       - code-simplifier
       - example-skills
       - planning-with-files
       - ralph-loop
       ... and 8 more plugins

     💡 Available commands:
       No commands found


❯ .

● No skills needed.

  How can I help you today?
  ⎿  Stop says:
     ---
     ✅ Session ended

     📁 Git repository
       Branch: main
       Changes:
         增加: 3 files
         修改: 4 files

     ✅ No temp files
     ---

✻ Worked for 5s

❯ 帮我设计Platform线程的基础 设施。大致想法如下：因为System Call涉及的 功能很有限，使用Command系统是否大材小用，且降低
  了信息披露性和可维护性？能否在进程启动时将主线程转化为协程线程，然后不同Manager在主线程循环调用各种协程？
  请复述并评价我的想法
  11:26

  已处理 38s
  你的想法可以复述为：

  System Call 的可变面并不大，主要集中在窗口、输入、显示器、IME、剪贴板、surface 等少数 owner-thread
  亲和操作。若为这些有限操作建立完整 SystemCommand catalog、command packet、reply endpoint、dispatch table，可能使 API
  从“直接调用一个明确的 Manager 方法”退化成“构造命令、投递命令、等待回复”的间接协议，降低信息披露性和维护可读性。更好的
  方案可能是：程序启动后把主线程作为一个 coroutine
  scheduler，WindowManager、InputManager、DisplayManager、ClipboardManager、SurfaceManager 等 manager 都以协程形式挂在
  主线程循环上，由主线程按消息泵、定时器、事件队列的节奏恢复这些协程。其他线程需要系统能力时，不提交通用
  command，而是调用类型明确的 manager facade，内部再切回主线程执行。

  这个判断有相当一部分是正确的。尤其是“System Call 涉及功能有限”这一点，会改变最优抽象。若当前目标不是跨进程插件
  ABI、事件录制回放、远程控制、脚本化系统调用，而只是同一进程内让 render/io/worker 安全请求主线程做少数 OS 操作，完整
  command 系统确实偏重。

  但有一个必须修正的表述：不能把主线程“转化为协程线程”。线程仍然是 OS thread，Win32 message
  queue、HWND、WndProc、IME、clipboard 仍属于这个线程。你能做的是：在主线程上运行一个 coroutine-aware
  scheduler，让主线程同时承担 Win32 message pump 和 coroutine executor。

  更准确的架构是：

  MainThread
    SystemMainLoop
      - Win32 message pump
      - coroutine scheduler
      - owner-thread executor
      - SystemEventQueue drain/dispatch

    Managers as main-thread actors
      - WindowManager::Run()
      - DisplayManager::Run()
      - InputManager::Run()
      - ClipboardManager::Run()
      - SurfaceManager::Run()

  Other Threads
    - call typed facade APIs
    - co_await SwitchToMainThread / InvokeOnOwner
    - never touch HWND/IME/clipboard directly
  这里的关键不是 command，而是 owner executor。外部 API 仍然可以长得很直接：

  auto window = co_await system.Windows().Create(desc);
  co_await system.Windows().SetTitle(window, L"Editor");
  co_await system.Clipboard().SetText(text);
  内部如果调用者已经在主线程，就直接执行；如果调用者在 worker 线程，就把 coroutine handle 或一个很小的 owner job
  放入主线程 mailbox，再唤醒主线程。这样 API 信息披露性比 Submit(CreateWindowCommand{...}) 更好。

  我会这样评价三种方案：

  方案    适合场景    问题
  完整 command catalog    跨进程、插件 ABI、录制回放、权限审计、热重载HWND hwnd = CreateWindowExW(...);
      co_return WindowHandle{...};
  }
  SwitchToOwner() 的语义是：若当前线程就是 owner thread，则不挂起；否则把 coroutine handle
  投到主线程队列，唤醒主线程，稍后由主线程恢复。这个模型没有虚 command dispatcher，没有 command id，没有 payload
  memcpy，也没有 ReplyEndpoint。它的成本是一次跨线程 enqueue 和一次 coroutine resume，正好对应真实需要。

  reflection 和 annotation 仍然有用，但用途要收窄：它们不再生成 command 总线，而是验证 owner API 的线程亲和性和
  schema。

  例如：

  [[=Anno::OwnerThreadOnly{.reason = "Win32 HWND affinity"}]]
  OwnerTask<WindowHandle> Create(WindowDesc desc);

  [[=Anno::MainThreadReentrant{false}]]
  OwnerTask<void> SetClipboardText(TextView text);
  consteval 可检查：

  所有 OwnerThreadOnly API 返回 OwnerTask<T>，不能返回裸值。
  OwnerThreadOnly API 不能暴露 HWND 为可变所有权，只能返回 WindowHandle 或 NativeWindowView。
  WindowDesc 不能混入 swapchain policy。
  所有可能跨线程调用的 manager API 必须显式标注 thread affinity。
  MainThreadReentrant(false) 的 API 不得在 WndProc 深层直接调用，只能延迟到 loop phase。
  主循环可以写成 phase-based，而不是一堆随意协程互相恢复：

  while running:
    1. PumpWin32Messages()
    2. DrainOwnerMailbox()
    3. ResumeReadyCoroutines()
    4. DrainSystemEvents()
    5. RunManagerTicks()
    6. DispatchPresentationEvents()
    7. WaitForMessageOrWakeEvent()
  这个 phase 顺序很重要。Win32 允许消息处理重入，SendMessage、IME、modal loop、clipboard 都可能把你带进非预期调用栈。若
   manager coroutine 在任意位置恢复，状态机会很难推理。phase-based loop 可以把 owner-thread 逻辑限制在固定阶段。

  所以最终建议是：

  放弃完整 SystemCommandQueue 作为主抽象。
  保留 SystemEventQueue，因为事件仍然需要 canonical schema、归并、分发。
  引入 MainThreadExecutor 和 OwnerTask<T>。
  每个系统能力用明确 manager API 暴露，不用泛化 command。
  仅在跨进程、脚本、工具录制、权限隔离场景，再从 manager API 自动派生 command catalog。
  这比纯 command 系统更符合你说的信息披露性和可维护性；同时也不牺牲线程所有权和性能。核心原则是：对外是 typed manager
  API，对内是 owner-thread coroutine scheduler，只有需要稳定协议边界时才降级为 command packet。   请评价此架构设计

● Activating: superpowers:brainstorming — this is a design/architecture discussion that benefits from structured
  exploration before committing to an approach.

● Skill(superpowers:brainstorming)
  ⎿  Successfully loaded skill

❯ Base directory for this skill:
  C:\Users\nekom\.claude\plugins\cache\claude-plugins-official\superpowers\5.1.0\skills\brainstorming

  # Brainstorming Ideas Into Designs

  Help turn ideas into fully formed designs and specs through natural collaborative dialogue.

  Start by understanding the current project context, then ask questions one at a time to refine the idea. Once you
  understand what you're building, present the design and get user approval.

  <HARD-GATE>
  Do NOT invoke any implementation skill, write any code, scaffold any project, or take any implementation action until
   you have presented a design and the user has approved it. This applies to EVERY project regardless of perceived
  simplicity.
  </HARD-GATE>

  ## Anti-Pattern: "This Is Too Simple To Need A Design"

  Every project goes through this process. A todo list, a single-function utility, a config change — all of them.
  "Simple" projects are where unexamined assumptions cause the most wasted work. The design can be short (a few
  sentences for truly simple projects), but you MUST present it and get approval.

  ## Checklist

  You MUST create a task for each of these items and complete them in order:

  1. **Explore project context** — check files, docs, recent commits
  2. **Offer visual companion** (if topic will involve visual questions) — this is its own message, not combined with a
   clarifying question. See the Visual Companion section below.
  3. **Ask clarifying questions** — one at a time, understand purpose/constraints/success criteria
  4. **Propose 2-3 approaches** — with trade-offs and your recommendation
  5. **Present design** — in sections scaled to their complexity, get user approval after each section
  6. **Write design doc** — save to `docs/superpowers/specs/YYYY-MM-DD-<topic>-design.md` and commit
  7. **Spec self-review** — quick inline check for placeholders, contradictions, ambiguity, scope (see below)
  8. **User reviews written spec** — ask user to review the spec file before proceeding
  9. **Transition to implementation** — invoke writing-plans skill to create implementation plan

  ## Process Flow

  ```dot
  digraph brainstorming {
      "Explore project context" [shape=box];
      "Visual questions ahead?" [shape=diamond];
      "Offer Visual Companion\n(own message, no other content)" [shape=box];
      "Ask clarifying questions" [shape=box];
      "Propose 2-3 approaches" [shape=box];
      "Present design sections" [shape=box];
      "User approves design?" [shape=diamond];
      "Write design doc" [shape=box];

  ──── (94 lines hidden) ──────────────────────────────────────────────────────────────────────────────────────────────
   question at a time** - Don't overwhelm with multiple questions
  - **Multiple choice preferred** - Easier to answer than open-ended when possible
  - **YAGNI ruthlessly** - Remove unnecessary features from all designs
  - **Explore alternatives** - Always propose 2-3 approaches before settling
  - **Incremental validation** - Present design, get approval before moving on
  - **Be flexible** - Go back and clarify when something doesn't make sense

  ## Visual Companion

  A browser-based companion for showing mockups, diagrams, and visual options during brainstorming. Available as a tool
   — not a mode. Accepting the companion means it's available for questions that benefit from visual treatment; it does
   NOT mean every question goes through the browser.

  **Offering the companion:** When you anticipate that upcoming questions will involve visual content (mockups,
  layouts, diagrams), offer it once for consent:
  > "Some of what we're working on might be easier to explain if I can show it to you in a web browser. I can put
  together mockups, diagrams, comparisons, and other visuals as we go. This feature is still new and can be
  token-intensive. Want to try it? (Requires opening a local URL)"

  **This offer MUST be its own message.** Do not combine it with clarifying questions, context summaries, or any other
  content. The message should contain ONLY the offer above and nothing else. Wait for the user's response before
  continuing. If they decline, proceed with text-only brainstorming.

  **Per-question decision:** Even after the user accepts, decide FOR EACH QUESTION whether to use the browser or the
  terminal. The test: **would the user understand this better by seeing it than reading it?**

  - **Use the browser** for content that IS visual — mockups, wireframes, layout comparisons, architecture diagrams,
  side-by-side visual designs
  - **Use the terminal** for content that is text — requirements questions, conceptual choices, tradeoff lists, A/B/C/D
   text options, scope decisions

  A question about a UI topic is not automatically a visual question. "What does personality mean in this context?" is
  a conceptual question — use the terminal. "Which wizard layout works better?" is a visual question — use the browser.

  If they agree to the companion, read the detailed guide before proceeding:
  `skills/brainstorming/visual-companion.md`


  ARGUMENTS: Design review of Platform thread architecture: MainThreadExecutor + OwnerTask<T> + phase-based loop +
  typed manager APIs instead of command catalog

● Let me explore the current project context to understand the existing Platform code and architecture before evaluating
   the design.

  Searched for 3 patterns (ctrl+o to expand)

❯ [Request interrupted by user]


❯ continue

  Searched for 3 patterns, read 6 files (ctrl+o to expand)

● Now I have a good picture of the project context. Let me look at the existing Geom.h and the overall project structure
   to understand more of the architecture direction.

  Listed 1 directory (ctrl+o to expand)

● Good. Now I have a complete picture of the project context. Let me provide my evaluation.

  ---
  评价

  你描述的架构（上一轮 AI 输出）整体方向正确，但有几处值得深入讨论。我把评价分为「赞同」和「需要推敲」两部分。

  ---
  赞同的部分

  1. 放弃完整 command catalog 作为主抽象 — 正确判断。

  从你现有代码来看（Window.h 的 WindowDesc、Annotation.h 的 schema 标注、SpscRingBuffer
  的底层通信基础设施），系统调用面确实很窄。窗口创建、标题设置、剪贴板、IME 加起来可能不超过 20 个操作。为此建立 command
   ID + payload serialization + dispatch table 是过度工程化。

  2. OwnerTask<T> 作为跨线程协程返回类型 — 正确抽象层级。

  你已经有了 Generator<Ref, V, Allocator> 这样高质量的同步协程基础设施。从 Generator 到 OwnerTask
  的跳跃是自然的：Generator 管理 lazy pull semantics，OwnerTask 管理 async owner-thread affinity
  semantics。两者互补，不冲突。

  3. Phase-based main loop 而非自由恢复 — 对抗 Win32 重入问题。

  Win32 的 SendMessage、IME composition、modal dialog 都会制造非预期重入。phase-based loop 把 owner-thread
  逻辑限制在固定阶段，让状态推理成为可能。这比随意 resume() 安全得多。

  4. 保留 SystemEventQueue — 事件需要 schema 化。

  事件（resize、DPI change、focus、input）确实需要 canonical schema、归并、时序保证。这和 command 是不同方向的关注点。

  ---
  需要推敲的部分

  1. "SwitchToOwner 若已在主线程则不挂起" 的实现陷阱。

  这看起来简单，但有隐含问题：如果 Manager API 在主线程 WndProc 深处被调用（比如处理 WM_CREATE 时同步调用了另一个
  Manager），"已在主线程就直接执行"会绕过 phase 约束。你需要区分两种情况：

  - 调用者在主线程 且 在正确 phase → 直接执行
  - 调用者在主线程但在 WndProc 回调深处 → 仍应延迟到 DrainOwnerMailbox phase

  这意味着 SwitchToOwner 不能只检查 GetCurrentThreadId()，还需要检查 loop 当前 phase。建议引入一个 MainLoopPhase
  枚举和一个 thread-local phase marker。

  2. Annotation 的角色需要更精确定位。

  当前 Annotation.h 的 Repr、RangeInt 等是数据描述元信息。提案中说 annotation 用于 consteval
  验证线程亲和性（OwnerThreadOnly、MainThreadReentrant）。这是两个不同层次：

  - 数据 schema annotation（已有）
  - API contract annotation（提议）

  建议分开：Core/Annotation.h 保持数据层，新增 Platform/ThreadContract.h 或类似的东西承载线程亲和性标注。避免让一个
  header 承担两个正交关注点。

  3. "Manager as main-thread actor" 的所有权模型不够清晰。

  提案说每个 Manager 有 Run() 方法挂在主线程 coroutine scheduler 上。但需要明确：

  - Manager 是常驻协程（永不 co_return，每帧被恢复一次）？还是只在有事件时被唤醒？
  - 如果是常驻协程，phase-based loop 的 RunManagerTicks() 就是依次 resume 所有 manager coroutine。但这和你已经有的
  Generator 模型不同 — Generator 是 pull-based lazy range，而 Manager 需要的是 push-based event-driven loop。
  - 建议 Manager 不用 coroutine 表示自身生命周期，而是一个普通对象，持有状态，在对应 phase 被 tick。协程用于跨线程 API
  调用的挂起/恢复，不用于 Manager 本身的调度。

  4. Phase 顺序存在一个微妙的先后依赖问题。

  1. PumpWin32Messages()
  2. DrainOwnerMailbox()
  3. ResumeReadyCoroutines()

  问题：Win32 message pump 可能产生事件（如 WM_SIZE），这些事件应该被 Manager 消费。但 Manager 在哪个阶段消费？如果
  Manager 在 RunManagerTicks(5) 才处理事件，那 DrainOwnerMailbox(2) 里恢复的跨线程调用可能看到还没处理的 stale 状态。

  建议考虑把 Win32 message → SystemEvent 转换放在 pump 阶段完成（pump 产出 event），然后 drain mailbox 和 manager tick
  都基于已转换的 event queue 工作。

  5. 缺少 cancellation 和 timeout 机制的讨论。

  OwnerTask<T> 如果调用者在 worker thread 发起请求后被取消（比如对应的 render frame 已经过期），主线程 mailbox 里的
  pending job 如何处理？需要 cancellation token 或 generation counter，否则会累积 stale work。

  ---
  总结

  这个架构的核心判断是对的：typed manager API + owner-thread executor + phase-based loop 比 full command catalog
  更适合当前规模。主要需要细化的是：

  1. SwitchToOwner 的 phase-awareness（不能只判断线程 ID）
  2. Manager 自身的表示（建议普通对象 + phase tick，不是常驻协程）
  3. Phase 内的事件流转顺序（pump → event conversion → downstream consumption）
  4. Cancellation story for pending owner jobs

  要继续深入哪个方面？还是你想调整方向后再讨论？
  ⎿  Stop says:
     ---
     ✅ Session ended

     📁 Git repository
       Branch: main
       Changes:
         增加: 3 files
         修改: 4 files

     ✅ No temp files
     ---

✻ Crunched for 1m 26s

❯ 要求充分利用包括reflection /
                  consteval block和annotation在内的C++ 20 ~26特性优势，不要有性能损失，尽量编译期完成。要完备、架构前沿
  且正确，无技术债。要求抽象层次正确，符合问题和客观世界建模。 请 自主设计最前沿的架构

● Skill(superpowers:brainstorming)
  ⎿  Successfully loaded skill

❯ Base directory for this skill:
  C:\Users\nekom\.claude\plugins\cache\claude-plugins-official\superpowers\5.1.0\skills\brainstorming

  # Brainstorming Ideas Into Designs

  Help turn ideas into fully formed designs and specs through natural collaborative dialogue.

  Start by understanding the current project context, then ask questions one at a time to refine the idea. Once you
  understand what you're building, present the design and get user approval.

  <HARD-GATE>
  Do NOT invoke any implementation skill, write any code, scaffold any project, or take any implementation action until
   you have presented a design and the user has approved it. This applies to EVERY project regardless of perceived
  simplicity.
  </HARD-GATE>

  ## Anti-Pattern: "This Is Too Simple To Need A Design"

  Every project goes through this process. A todo list, a single-function utility, a config change — all of them.
  "Simple" projects are where unexamined assumptions cause the most wasted work. The design can be short (a few
  sentences for truly simple projects), but you MUST present it and get approval.

  ## Checklist

  You MUST create a task for each of these items and complete them in order:

  1. **Explore project context** — check files, docs, recent commits
  2. **Offer visual companion** (if topic will involve visual questions) — this is its own message, not combined with a
   clarifying question. See the Visual Companion section below.
  3. **Ask clarifying questions** — one at a time, understand purpose/constraints/success criteria
  4. **Propose 2-3 approaches** — with trade-offs and your recommendation
  5. **Present design** — in sections scaled to their complexity, get user approval after each section
  6. **Write design doc** — save to `docs/superpowers/specs/YYYY-MM-DD-<topic>-design.md` and commit
  7. **Spec self-review** — quick inline check for placeholders, contradictions, ambiguity, scope (see below)
  8. **User reviews written spec** — ask user to review the spec file before proceeding
  9. **Transition to implementation** — invoke writing-plans skill to create implementation plan

  ## Process Flow

  ```dot
  digraph brainstorming {
      "Explore project context" [shape=box];
      "Visual questions ahead?" [shape=diamond];
      "Offer Visual Companion\n(own message, no other content)" [shape=box];
      "Ask clarifying questions" [shape=box];
      "Propose 2-3 approaches" [shape=box];
      "Present design sections" [shape=box];
      "User approves design?" [shape=diamond];
      "Write design doc" [shape=box];

  ──── (94 lines hidden) ──────────────────────────────────────────────────────────────────────────────────────────────
  me** - Don't overwhelm with multiple questions
  - **Multiple choice preferred** - Easier to answer than open-ended when possible
  - **YAGNI ruthlessly** - Remove unnecessary features from all designs
  - **Explore alternatives** - Always propose 2-3 approaches before settling
  - **Incremental validation** - Present design, get approval before moving on
  - **Be flexible** - Go back and clarify when something doesn't make sense

  ## Visual Companion

  A browser-based companion for showing mockups, diagrams, and visual options during brainstorming. Available as a tool
   — not a mode. Accepting the companion means it's available for questions that benefit from visual treatment; it does
   NOT mean every question goes through the browser.

  **Offering the companion:** When you anticipate that upcoming questions will involve visual content (mockups,
  layouts, diagrams), offer it once for consent:
  > "Some of what we're working on might be easier to explain if I can show it to you in a web browser. I can put
  together mockups, diagrams, comparisons, and other visuals as we go. This feature is still new and can be
  token-intensive. Want to try it? (Requires opening a local URL)"

  **This offer MUST be its own message.** Do not combine it with clarifying questions, context summaries, or any other
  content. The message should contain ONLY the offer above and nothing else. Wait for the user's response before
  continuing. If they decline, proceed with text-only brainstorming.

  **Per-question decision:** Even after the user accepts, decide FOR EACH QUESTION whether to use the browser or the
  terminal. The test: **would the user understand this better by seeing it than reading it?**

  - **Use the browser** for content that IS visual — mockups, wireframes, layout comparisons, architecture diagrams,
  side-by-side visual designs
  - **Use the terminal** for content that is text — requirements questions, conceptual choices, tradeoff lists, A/B/C/D
   text options, scope decisions

  A question about a UI topic is not automatically a visual question. "What does personality mean in this context?" is
  a conceptual question — use the terminal. "Which wizard layout works better?" is a visual question — use the browser.

  If they agree to the companion, read the detailed guide before proceeding:
  `skills/brainstorming/visual-companion.md`


  ARGUMENTS: Design a cutting-edge C++20-26 Platform thread architecture for Mashiro engine, leveraging reflection,
  consteval, annotations, coroutines with zero runtime overhead

● 我已经从上一轮对话中了解了项目上下文（Generator.h、SpscRingBuffer.h、Annotation.h、Window.h、ThreadNaming.h
  等现有基础设施）。你的要求很明确：

  - 充分利用 C++20~26 前沿特性（reflection、consteval、annotation、coroutines）
  - 零运行时开销，尽量编译期完成
  - 完备、架构前沿、无技术债
  - 抽象层次正确，符合客观世界建模
  - 自主设计，不需逐步确认

  鉴于你说"自主设计最前沿的架构"，我跳过逐步提问，直接呈现完整架构设计。这个设计基于你之前的想法（放弃 command
  catalog、typed manager API、owner-thread executor、phase-based loop），但做了显著的重新架构来达到你的 C++26 前沿要求。

  ---
  架构设计：Mashiro Platform Thread Infrastructure

  一、核心建模哲学

  客观世界中，OS 平台线程有三个不可还原的事实：

  1. Thread Affinity — 某些 OS 资源（HWND、IME context、clipboard）被绑定到创建它们的线程
  2. Asynchronous Boundary — 跨线程通信本质上是异步的
  3. Phase Ordering — OS message pump 强制某些操作有因果序（WM_CREATE < WM_SIZE < WM_PAINT）

  架构直接映射这三个事实为类型系统约束，而非运行时检查。

  ---
  二、类型宇宙（Type Universe）

  Platform/
  ├── ThreadAffinity.h          — 编译期线程亲和性标注系统
  ├── OwnerExecutor.h           — 主线程 coroutine executor
  ├── OwnerTask.h               — 跨线程 owner-affine async task
  ├── MainLoop.h                — Phase-based main loop
  ├── PhaseGuard.h              — 编译期 + 运行时 phase 约束
  ├── ManagerBase.h             — Manager CRTP base with consteval contract verification
  ├── SystemEvent.h             — Canonical event schema
  ├── SystemEventQueue.h        — Lock-free event drain
  └── Managers/
      ├── WindowManager.h
      ├── InputManager.h
      ├── DisplayManager.h
      ├── ClipboardManager.h
      └── SurfaceManager.h

  ---
  三、ThreadAffinity — 编译期线程合约

  不扩展现有 Annotation.h（保持其数据 schema 职责），新建独立的线程合约系统：

  namespace Mashiro::Platform {

      enum class ThreadDomain : uint8_t {
          Owner,      // 主线程专属（HWND、IME、clipboard）
          Any,        // 任意线程可调用
          Caller,     // 在调用者线程执行
      };

      enum class Reentrancy : uint8_t {
          Forbidden,  // 不可在 WndProc 深处重入
          Deferred,   // 可调用但延迟到下一 phase
          Immediate,  // 可立即执行（幂等操作）
      };

      struct ThreadContract {
          ThreadDomain domain = ThreadDomain::Owner;
          Reentrancy reentrancy = Reentrancy::Forbidden;

          consteval bool RequiresOwnerTransfer() const {
              return domain == ThreadDomain::Owner;
          }
      };

      // C++26 annotation-style attribute (模拟，待编译器支持后替换为 [[=...]])
      template<ThreadContract Contract>
      struct Affinity {
          static constexpr ThreadContract kContract = Contract;
      };
  }

  关键：ThreadContract 是 constexpr 值，在 consteval 函数中可被检查。Manager 的每个方法通过 CRTP 和 concept
  在编译期验证返回类型与 contract 匹配。

  ---
  四、OwnerTask<T> — Owner-Affine Async Primitive

  这是整个架构的核心异步原语。语义：

  - 如果调用者已在 owner thread 且在正确 phase → 直接执行，零开销
  - 如果调用者在 worker thread 或错误 phase → 挂起，投递到 owner mailbox，由 main loop 在正确 phase 恢复

  namespace Mashiro::Platform {

      template<typename T>
      class [[nodiscard]] OwnerTask {
      public:
          struct promise_type;
          using handle_type = std::coroutine_handle<promise_type>;

          struct promise_type {
              std::optional<T> result_;
              std::exception_ptr exception_;
              std::coroutine_handle<> continuation_;

              OwnerTask get_return_object() noexcept {
                  return OwnerTask{handle_type::from_promise(*this)};
              }

              auto initial_suspend() noexcept { return TransferToOwnerAwaiter{}; }
              std::suspend_always final_suspend() noexcept { return {}; }

              template<typename U>
                  requires std::convertible_to<U, T>
              void return_value(U&& val) { result_.emplace(std::forward<U>(val)); }

              void unhandled_exception() noexcept {
                  exception_ = std::current_exception();
              }
          };

          // co_await support: 调用者 co_await ownerTask 时挂起自己
          bool await_ready() const noexcept;
          void await_suspend(std::coroutine_handle<> caller) noexcept;
          T await_resume();

      private:
          handle_type handle_;
          explicit OwnerTask(handle_type h) noexcept : handle_(h) {}
      };

      // void 特化
      template<>
      class OwnerTask<void> { /* ... */ };
  }

  TransferToOwnerAwaiter 是关键：

  struct TransferToOwnerAwaiter {
      bool await_ready() noexcept {
          // 编译期不可能知道运行时线程，但可以 inline 快速路径
          return IsOnOwnerThread() && IsInSafePhase();
      }

      void await_suspend(std::coroutine_handle<> h) noexcept {
          // 投递到 owner mailbox，唤醒主线程
          OwnerExecutor::Current().Enqueue(h);
      }

      void await_resume() noexcept {}
  };

  ---
  五、MainLoop — Phase-Based Execution Model

  namespace Mashiro::Platform {

      enum class Phase : uint8_t {
          PumpMessages,       // Win32 PeekMessage/DispatchMessage
          DrainOwnerMailbox,  // 恢复跨线程投递的 coroutines
          ProcessEvents,      // SystemEventQueue → Manager dispatch
          ManagerTick,        // 各 Manager 的 per-frame 逻辑
          Present,            // 帧提交信号
          Wait,               // MsgWaitForMultipleObjects
      };

      class MainLoop {
      public:
          void Run();

          Phase CurrentPhase() const noexcept { return phase_; }
          bool IsInPhase(Phase p) const noexcept { return phase_ == p; }

          // Phase guard: 只允许在特定 phase 执行的操作
          template<Phase... Allowed>
          consteval static auto RequirePhase() {
              return PhaseGuard<Allowed...>{};
          }

      private:
          Phase phase_ = Phase::Wait;
          OwnerExecutor executor_;
          SystemEventQueue eventQueue_;

          void PumpMessages();
          void DrainMailbox();
          void ProcessEvents();
          void TickManagers();
          void Present();
          void WaitForWork();
      };
  }

  Main loop 实现：

  void MainLoop::Run() {
      while (running_) {
          phase_ = Phase::PumpMessages;
          PumpMessages();             // Win32 msg → SystemEvent 转换在此完成

          phase_ = Phase::DrainOwnerMailbox;
          DrainMailbox();             // 恢复所有 pending OwnerTask coroutines

          phase_ = Phase::ProcessEvents;
          ProcessEvents();           // SystemEvent 分发到各 Manager

          phase_ = Phase::ManagerTick;
          TickManagers();            // Manager per-frame 逻辑

          phase_ = Phase::Present;
          Present();                 // 通知 renderer 可提交

          phase_ = Phase::Wait;
          WaitForWork();             // MsgWaitForMultipleObjects
      }
  }

  ---
  六、PhaseGuard — 编译期 Phase 约束

  namespace Mashiro::Platform {

      template<Phase... Allowed>
      struct PhaseGuard {
          static consteval bool Permits(Phase p) {
              return ((p == Allowed) || ...);
          }
      };

      // 运行时断言（Debug 模式），编译期验证 contract 一致性
      template<Phase... Allowed>
      void AssertPhase([[maybe_unused]] PhaseGuard<Allowed...>) {
          #ifndef NDEBUG
          Phase current = MainLoop::Instance().CurrentPhase();
          assert((... || (current == Allowed)) &&
                 "Operation called in wrong main loop phase");
          #endif
      }
  }

  ---
  七、ManagerBase — CRTP with Consteval Contract Verification

  Manager 不是协程，是普通对象。协程用于 API 调用，不用于 Manager 生命周期。

  namespace Mashiro::Platform {

      template<typename Derived>
      class ManagerBase {
      public:
          void Tick() { static_cast<Derived*>(this)->OnTick(); }

          void DispatchEvent(const SystemEvent& event) {
              static_cast<Derived*>(this)->OnEvent(event);
          }

      protected:
          // consteval 验证：确保所有标注 OwnerThread 的方法返回 OwnerTask
          static consteval void VerifyContracts() {
              // C++26 reflection: 遍历 Derived 的所有 public 方法
              // 检查标注了 ThreadContract{.domain = Owner} 的方法
              // 确保返回类型是 OwnerTask<T>
              //
              // 伪代码（待 P2996 reflection 最终标准化）：
              // for (auto method : std::meta::members_of(^Derived)) {
              //     if (has_annotation<Affinity>(method)) {
              //         auto contract = get_annotation<Affinity>(method).kContract;
              //         if (contract.RequiresOwnerTransfer()) {
              //             static_assert(is_owner_task(return_type_of(method)));
              //         }
              //     }
              // }
          }

          ManagerBase() { VerifyContracts(); }
      };
  }

  ---
  八、WindowManager 示例

  namespace Mashiro::Platform {

      class WindowManager final : public ManagerBase<WindowManager> {
          friend class ManagerBase<WindowManager>;

      public:
          // Owner-thread API: 从任意线程调用都安全
          // 如果已在 owner thread 且 phase 正确 → 零开销直接执行
          // 否则 → coroutine 挂起，投递到 mailbox

          [[nodiscard]]
          OwnerTask<WindowHandle> Create(WindowDesc desc) {
              co_await TransferToOwnerAwaiter{};
              AssertPhase(MainLoop::RequirePhase<
                  Phase::DrainOwnerMailbox,
                  Phase::ManagerTick>());

              HWND hwnd = CreateWindowExW(
                  0, kWindowClass, /* ... from desc ... */);
              co_return WindowHandle{hwnd, desc};
          }

          [[nodiscard]]
          OwnerTask<void> SetTitle(WindowHandle window, std::wstring_view title) {
              co_await TransferToOwnerAwaiter{};
              SetWindowTextW(window.Native(), title.data());
          }

          [[nodiscard]]
          OwnerTask<void> Destroy(WindowHandle window) {
              co_await TransferToOwnerAwaiter{};
              DestroyWindow(window.Native());
          }

          // Any-thread query: 不需要 owner transfer
          WindowDesc GetDesc(WindowHandle window) const noexcept {
              return descs_.at(window.Id());
          }

      private:
          void OnTick() { /* per-frame window state reconciliation */ }
          void OnEvent(const SystemEvent& event);

          FlatMap<WindowId, WindowDesc> descs_;
      };
  }

  调用方代码：

  // 在 render thread 中
  auto window = co_await system.Windows().Create({
      .title = "Editor",
      .size = {1920, 1080},
      .flags = Window::Flags::Resizable | Window::Flags::HighDpi,
  });

  co_await system.Windows().SetTitle(window, L"Editor - untitled.txt");

  ---
  九、OwnerExecutor — Lock-Free Mailbox

  基于你已有的 SpscRingBuffer 基础设施，但 mailbox 是 MPSC（多 worker → 单 owner）：

  namespace Mashiro::Platform {

      class OwnerExecutor {
      public:
          static OwnerExecutor& Current() noexcept;

          void Enqueue(std::coroutine_handle<> handle) noexcept {
              mailbox_.Push(handle);
              WakeOwnerThread();
          }

          void DrainAll() noexcept {
              while (auto handle = mailbox_.Pop()) {
                  handle->resume();
              }
          }

          bool IsOnOwnerThread() const noexcept {
              return GetCurrentThreadId() == ownerThreadId_;
          }

      private:
          MpscQueue<std::coroutine_handle<>> mailbox_;
          uint32_t ownerThreadId_;
          HANDLE wakeEvent_;  // Manual-reset event for MsgWaitForMultipleObjects

          void WakeOwnerThread() noexcept {
              SetEvent(wakeEvent_);
          }
      };
  }

  ---
  十、SystemEvent — Canonical Event Schema

  事件仍需要 schema 化，但不用 command：

  namespace Mashiro::Platform {

      enum class EventKind : uint16_t {
          WindowResize,
          WindowClose,
          WindowFocus,
          WindowDpiChange,
          InputKey,
          InputChar,
          InputMouse,
          InputScroll,
          DisplayChange,
          ClipboardUpdate,
      };

      struct SystemEvent {
          EventKind kind;
          uint64_t timestamp;  // QPC ticks

          union Payload {
              struct { WindowId window; uint32_t w, h; } resize;
              struct { WindowId window; } close;
              struct { WindowId window; bool focused; } focus;
              struct { WindowId window; float scale; } dpi;
              struct { KeyCode key; KeyAction action; ModFlags mods; } key;
              struct { char32_t codepoint; } character;
              struct { WindowId window; float x, y; MouseButton btn; } mouse;
              struct { WindowId window; float dx, dy; } scroll;
              struct { uint32_t monitorId; } display;
              struct { } clipboard;
          } payload;
      };

      static_assert(std::is_trivially_copyable_v<SystemEvent>);
      static_assert(sizeof(SystemEvent) <= 64); // 单 cache line
  }

  ---
  十一、Cancellation — Generation Counter

  解决 stale work 问题：

  namespace Mashiro::Platform {

      class CancellationToken {
      public:
          bool IsCancelled() const noexcept {
              return generation_.load(std::memory_order_acquire) != myGeneration_;
          }

          void Cancel() noexcept {
              generation_.fetch_add(1, std::memory_order_release);
          }

      private:
          std::atomic<uint64_t>& generation_;
          uint64_t myGeneration_;
      };

      // OwnerTask 可选地携带 cancellation
      template<typename T>
      OwnerTask<T> WithCancellation(CancellationToken token, OwnerTask<T> task);
  }

  ---
  十二、Consteval 验证矩阵（C++26 Reflection）

  当 P2996 可用时，以下验证在编译期自动执行：

  ┌───────────────────────────────────────────────────────────────┬────────────────────────────────────────────┐
  │                             规则                              │                  验证方式                  │
  ├───────────────────────────────────────────────────────────────┼────────────────────────────────────────────┤
  │ ThreadDomain::Owner 方法必须返回 OwnerTask<T>                 │ consteval reflection on return type        │
  ├───────────────────────────────────────────────────────────────┼────────────────────────────────────────────┤
  │ Reentrancy::Forbidden 方法不可在 Phase::PumpMessages 中调用   │ PhaseGuard static assert                   │
  ├───────────────────────────────────────────────────────────────┼────────────────────────────────────────────┤
  │ Manager 不暴露裸 HWND，只返回 WindowHandle / NativeWindowView │ consteval check on public API return types │
  ├───────────────────────────────────────────────────────────────┼────────────────────────────────────────────┤
  │ SystemEvent::Payload 不超过 cache line                        │ static_assert(sizeof)                      │
  ├───────────────────────────────────────────────────────────────┼────────────────────────────────────────────┤
  │ 所有 public Manager API 必须标注 ThreadContract               │ consteval reflection completeness check    │
  └───────────────────────────────────────────────────────────────┴────────────────────────────────────────────┘

  在 P2996 落地前，使用 CRTP + concept 约束实现等价的编译期验证：

  template<typename M>
  concept ValidManager = requires {
      // 所有 OwnerTask-returning 方法必须存在于 Manager
      requires std::derived_from<M, ManagerBase<M>>;
      // Manager 必须提供 OnTick 和 OnEvent
      { std::declval<M>().OnTick() } -> std::same_as<void>;
      { std::declval<M>().OnEvent(std::declval<const SystemEvent&>()) } -> std::same_as<void>;
  };

  ---
  十三、性能特性

  ┌────────────────────────────────┬───────────────────────────────────────────────────────────┐
  │              路径              │                           成本                            │
  ├────────────────────────────────┼───────────────────────────────────────────────────────────┤
  │ 已在 owner thread + 正确 phase │ 0 次原子操作，直接调用，等价裸函数                        │
  ├────────────────────────────────┼───────────────────────────────────────────────────────────┤
  │ 跨线程调用                     │ 1 次 MPSC enqueue + 1 次 SetEvent + 1 次 coroutine resume │
  ├────────────────────────────────┼───────────────────────────────────────────────────────────┤
  │ 事件分发                       │ trivially-copyable memcpy 到 ring buffer                  │
  ├────────────────────────────────┼───────────────────────────────────────────────────────────┤
  │ Manager tick                   │ 虚函数零次（CRTP 静态分发）                               │
  ├────────────────────────────────┼───────────────────────────────────────────────────────────┤
  │ 线程合约验证                   │ 编译期，零运行时成本                                      │
  └────────────────────────────────┴───────────────────────────────────────────────────────────┘

  ---
  十四、与现有代码的衔接

  - Generator<Ref, V, Alloc> 保持不变 — 它是 pull-based lazy range，与 OwnerTask 互补
  - SpscRingBuffer / SpscQueue 作为 SystemEventQueue 的底层存储
  - Annotation.h 保持数据 schema 职责不变
  - Window::WindowDesc 直接复用，作为 WindowManager::Create 的参数
  - ThreadNaming.h 在 MainLoop 初始化时为 owner thread 设置名称

  ---
  这个架构是否符合你的期望？有哪些部分需要调整或深入？
  ⎿  Stop says:
     ---
     ✅ Session ended

     📁 Git repository
       Branch: main
       Changes:
         增加: 3 files
         修改: 4 files

     ✅ No temp files
     ---

✻ Brewed for 1m 36s

❯ 请充分调研，补充更前沿，更丰富的特性。因为我们使用了p2996的llvm-project分支，cmake/ReflectionFeatureProbes.cmake中的
  特性已经充分实现，请以此为前提

  Searched for 4 patterns, read 8 files (ctrl+o to expand)

● Now I have a comprehensive picture. Let me also check if there's any existing event system or executor pattern.

  Searched for 1 pattern, read 1 file (ctrl+o to expand)

● I now have complete understanding of the project's C++26 infrastructure. The codebase already uses P2996 reflection,
  P3394 annotations ([[=SoA::Skip{}]]), P3289 consteval blocks, P1306 expansion statements (template for), and P3491
  define_static_array — all verified via ReflectionFeatureProbes.cmake. Let me produce the full design.

  ---
  完整架构设计：Mashiro Platform Thread Infrastructure

  基于你的工具链实际能力（P2996 + P3394 + P3289 + P1306 + P3491 全部
  MANDATORY），以下设计充分利用这些特性，做到编译期最大化、运行时零开销、类型安全完备。

  ---
  一、设计原则

  客观世界建模：OS 平台线程的不可还原事实有三个 —— Thread Affinity（资源绑定创建线程）、Asynchronous
  Boundary（跨线程本质异步）、Phase
  Ordering（消息泵强制因果序）。架构把这三个事实直接编码为类型系统约束，不做运行时检查。

  编译期完备性策略：
  - 线程合约 → P3394 annotation，编译期通过 annotations_of 验证
  - Manager API 表面积 → consteval {} block 自动生成 facade、dispatcher、event router
  - Phase 约束 → consteval 验证 + debug-only runtime assert（release 中零成本）
  - Manager 注册 → template for 遍历反射出的 manager 集合，静态分发

  ---
  二、目录结构

  Mashiro/include/Mashiro/Platform/
  ├── ThreadContract.h        — P3394 annotation types for thread affinity
  ├── OwnerExecutor.h         — MPSC lock-free mailbox + coroutine scheduler
  ├── OwnerTask.h             — Owner-affine async coroutine primitive
  ├── MainLoop.h              — Phase-based main loop with consteval phase table
  ├── PhaseGuard.h            — Compile-time + debug-runtime phase enforcement
  ├── ManagerTraits.h         — consteval reflection over Manager API surfaces
  ├── SystemEvent.h           — Trivially-copyable canonical event schema
  ├── SystemEventQueue.h      — MPSC event ring (builds on SpscRingBuffer primitives)
  ├── MpscQueue.h             — Multi-producer single-consumer intrusive queue
  └── Managers/
      ├── WindowManager.h
      ├── InputManager.h
      ├── DisplayManager.h
      ├── ClipboardManager.h
      └── SurfaceManager.h

  Mashiro/src/Platform/
  ├── Windows/
  │   ├── MainLoop.cpp        — Win32 implementation
  │   ├── OwnerExecutor.cpp
  │   ├── WindowManager.cpp
  │   └── MessageTranslation.cpp  — WM_* → SystemEvent conversion

  ---
  三、ThreadContract — P3394 Annotation 驱动的线程合约

  不扩展现有 Annotation.h（保持数据 schema 职责），新建线程合约 annotation 系统：

  // Platform/ThreadContract.h
  #pragma once
  #include <meta>
  #include <cstdint>

  namespace Mashiro::Platform {

      // =========================================================================
      // Thread Domain — 操作的线程归属
      // =========================================================================

      enum class ThreadDomain : uint8_t {
          Owner,   // 必须在 owner thread 执行（HWND、IME、clipboard）
          Any,     // 任意线程可执行（纯查询、无副作用）
          Caller,  // 在调用者线程执行（不迁移）
      };

      // =========================================================================
      // Phase Affinity — 操作允许执行的主循环阶段
      // =========================================================================

      enum class Phase : uint8_t {
          PumpMessages,
          DrainMailbox,
          TranslateEvents,
          DispatchEvents,
          ManagerTick,
          Present,
          Idle,
      };

      // =========================================================================
      // Reentrancy Policy — WndProc 深处的重入安全性
      // =========================================================================

      enum class Reentrancy : uint8_t {
          Forbidden,   // 不可在 message dispatch 深处调用
          Deferred,    // 可调用但延迟到 DrainMailbox phase
          Immediate,   // 可立即执行（幂等、只读）
      };

      // =========================================================================
      // ThreadContract — P3394 annotation 结构体
      // =========================================================================

      struct ThreadContract {
          ThreadDomain domain = ThreadDomain::Owner;
          Reentrancy reentrancy = Reentrancy::Forbidden;

          consteval bool RequiresOwnerTransfer() const {
              return domain == ThreadDomain::Owner;
          }

          constexpr bool operator==(const ThreadContract&) const = default;
      };

      // 预定义的常用合约，作为 annotation 值使用
      inline constexpr ThreadContract kOwnerOnly{
          .domain = ThreadDomain::Owner, .reentrancy = Reentrancy::Forbidden};
      inline constexpr ThreadContract kOwnerDeferred{
          .domain = ThreadDomain::Owner, .reentrancy = Reentrancy::Deferred};
      inline constexpr ThreadContract kOwnerImmediate{
          .domain = ThreadDomain::Owner, .reentrancy = Reentrancy::Immediate};
      inline constexpr ThreadContract kAnyThread{
          .domain = ThreadDomain::Any, .reentrancy = Reentrancy::Immediate};

      // =========================================================================
      // PhaseSet — 允许执行的阶段集合（bitfield）
      // =========================================================================

      struct PhaseSet {
          uint8_t bits = 0xFF; // 默认全部允许

          consteval PhaseSet() = default;
          consteval PhaseSet(std::initializer_list<Phase> phases) : bits(0) {
              for (auto p : phases) bits |= (1u << static_cast<uint8_t>(p));
          }

          consteval bool Permits(Phase p) const {
              return (bits >> static_cast<uint8_t>(p)) & 1;
          }

          constexpr bool operator==(const PhaseSet&) const = default;
      };

      inline constexpr PhaseSet kMailboxPhases{
          {Phase::DrainMailbox, Phase::ManagerTick}};
      inline constexpr PhaseSet kAllPhases{};

  } // namespace Mashiro::Platform

  使用方式（Manager 方法上标注）：

  struct [[=kOwnerOnly]] [[=kMailboxPhases]] Create;
  // 或直接标注在函数（需要编译器支持 P3394 on function declarations）

  由于 P3394 当前在 clang-p2996 中对 member function 的 annotation 支持尚需确认，设计采用双模式：优先
  annotation-on-function，降级时使用 annotation-on-nested-tag + consteval 桥接。

  ---
  四、OwnerTask<T> — 零开销 Owner-Affine Coroutine Primitive

  // Platform/OwnerTask.h
  #pragma once
  #include "Mashiro/Core/Result.h"
  #include "Mashiro/Platform/ThreadContract.h"

  #include <coroutine>
  #include <optional>
  #include <type_traits>
  #include <utility>

  namespace Mashiro::Platform {

      class OwnerExecutor;

      // =========================================================================
      // TransferAwaiter — 核心：判断是否需要跨线程迁移
      // =========================================================================

      struct TransferToOwner {
          OwnerExecutor* executor;

          bool await_ready() noexcept;   // IsOnOwnerThread() && IsInSafePhase()
          void await_suspend(std::coroutine_handle<> h) noexcept; // enqueue to mailbox
          void await_resume() noexcept {}
      };

      // =========================================================================
      // OwnerTask<T> — 对外 API 的返回类型
      // =========================================================================

      template<typename T>
      class [[nodiscard]] OwnerTask {
      public:
          struct Promise;
          using promise_type = Promise;
          using handle_type = std::coroutine_handle<Promise>;

          struct Promise {
              union Storage {
                  Storage() noexcept {}
                  ~Storage() {}
                  T value;
              };

              Storage storage_;
              std::exception_ptr exception_;
              std::coroutine_handle<> continuation_{std::noop_coroutine()};
              OwnerExecutor* executor_ = nullptr;
              bool ready_ = false;

              OwnerTask get_return_object() noexcept {
                  return OwnerTask{handle_type::from_promise(*this)};
              }

              // initial_suspend: 直接执行（fast path）还是挂起等待 transfer
              auto initial_suspend() noexcept {
                  return TransferToOwner{executor_};
              }

              auto final_suspend() noexcept {
                  struct Resumer {
                      bool await_ready() noexcept { return false; }
                      std::coroutine_handle<> await_suspend(
                          std::coroutine_handle<Promise> h) noexcept {
                          h.promise().ready_ = true;
                          return h.promise().continuation_;
                      }
                      void await_resume() noexcept {}
                  };
                  return Resumer{};
              }

              template<typename U>
                  requires std::convertible_to<U, T>
              void return_value(U&& val) {
                  std::construct_at(&storage_.value, std::forward<U>(val));
              }

              void unhandled_exception() noexcept {
                  exception_ = std::current_exception();
              }
          };

          // co_await OwnerTask<T> from another coroutine
          bool await_ready() const noexcept { return handle_.promise().ready_; }

          std::coroutine_handle<> await_suspend(std::coroutine_handle<> caller) noexcept {
              handle_.promise().continuation_ = caller;
              return handle_;
          }

          T await_resume() {
              auto& p = handle_.promise();
              if (p.exception_) std::rethrow_exception(p.exception_);
              return std::move(p.storage_.value);
          }

          ~OwnerTask() {
              if (handle_) handle_.destroy();
          }

          OwnerTask(OwnerTask&& o) noexcept : handle_(std::exchange(o.handle_, nullptr)) {}
          OwnerTask& operator=(OwnerTask&&) = delete;
          OwnerTask(const OwnerTask&) = delete;

      private:
          handle_type handle_;
          explicit OwnerTask(handle_type h) noexcept : handle_(h) {}
      };

      // =========================================================================
      // OwnerTask<void> 特化
      // =========================================================================

      template<>
      class [[nodiscard]] OwnerTask<void> {
      public:
          struct Promise {
              std::exception_ptr exception_;
              std::coroutine_handle<> continuation_{std::noop_coroutine()};
              OwnerExecutor* executor_ = nullptr;
              bool ready_ = false;

              OwnerTask get_return_object() noexcept {
                  return OwnerTask{std::coroutine_handle<Promise>::from_promise(*this)};
              }
              auto initial_suspend() noexcept { return TransferToOwner{executor_}; }
              auto final_suspend() noexcept {
                  struct Resumer {
                      bool await_ready() noexcept { return false; }
                      std::coroutine_handle<> await_suspend(
                          std::coroutine_handle<Promise> h) noexcept {
                          h.promise().ready_ = true;
                          return h.promise().continuation_;
                      }
                      void await_resume() noexcept {}
                  };
                  return Resumer{};
              }
              void return_void() noexcept {}
              void unhandled_exception() noexcept { exception_ = std::current_exception(); }
          };

          using promise_type = Promise;
          using handle_type = std::coroutine_handle<Promise>;

          bool await_ready() const noexcept { return handle_.promise().ready_; }
          std::coroutine_handle<> await_suspend(std::coroutine_handle<> caller) noexcept {
              handle_.promise().continuation_ = caller;
              return handle_;
          }
          void await_resume() {
              if (handle_.promise().exception_)
                  std::rethrow_exception(handle_.promise().exception_);
          }

          ~OwnerTask() { if (handle_) handle_.destroy(); }
          OwnerTask(OwnerTask&& o) noexcept : handle_(std::exchange(o.handle_, nullptr)) {}

      private:
          handle_type handle_;
          explicit OwnerTask(handle_type h) noexcept : handle_(h) {}
      };

  } // namespace Mashiro::Platform

  ---
  五、OwnerExecutor — MPSC Lock-Free Mailbox

  // Platform/OwnerExecutor.h
  #pragma once
  #include "Mashiro/Platform/ThreadContract.h"

  #include <atomic>
  #include <coroutine>
  #include <cstdint>

  namespace Mashiro::Platform {

      // =========================================================================
      // MPSC intrusive coroutine queue — single cache-line node
      // =========================================================================

      struct MailboxNode {
          std::coroutine_handle<> handle;
          std::atomic<MailboxNode*> next{nullptr};
      };

      // =========================================================================
      // OwnerExecutor
      // =========================================================================

      class OwnerExecutor {
      public:
          void Initialize(uint32_t ownerThreadId, void* wakeEvent) noexcept;

          void Enqueue(std::coroutine_handle<> h) noexcept;
          void DrainAll() noexcept;

          bool IsOnOwnerThread() const noexcept;
          bool IsInSafePhase() const noexcept;
          Phase CurrentPhase() const noexcept { return phase_; }

          static OwnerExecutor& Instance() noexcept;

      private:
          friend class MainLoop;
          void SetPhase(Phase p) noexcept { phase_ = p; }

          // MPSC queue (Michael-Scott variant, single consumer)
          alignas(64) std::atomic<MailboxNode*> head_{nullptr};
          alignas(64) MailboxNode stub_;
          MailboxNode* tail_ = &stub_;

          uint32_t ownerThreadId_ = 0;
          void* wakeEvent_ = nullptr;
          Phase phase_ = Phase::Idle;

          void WakeOwnerThread() noexcept;
      };

      // =========================================================================
      // TransferToOwner implementation (inline for fast path)
      // =========================================================================

      inline bool TransferToOwner::await_ready() noexcept {
          if (!executor) executor = &OwnerExecutor::Instance();
          return executor->IsOnOwnerThread() && executor->IsInSafePhase();
      }

      inline void TransferToOwner::await_suspend(std::coroutine_handle<> h) noexcept {
          executor->Enqueue(h);
      }

  } // namespace Mashiro::Platform

  ---
  六、ManagerTraits — Consteval Reflection over Manager API Surfaces

  这是最前沿的部分。利用 P2996 + P3394 + P3289，在编译期自动验证所有 Manager 的 API 合约：

  // Platform/ManagerTraits.h
  #pragma once
  #include "Mashiro/Platform/ThreadContract.h"
  #include "Mashiro/Platform/OwnerTask.h"

  #include <meta>
  #include <string_view>
  #include <vector>

  namespace Mashiro::Platform {

      // =========================================================================
      // Consteval verification: all annotated methods satisfy their contracts
      // =========================================================================

      namespace Detail {

          /// @brief Check if a reflected member function carries ThreadContract annotation.
          consteval bool HasThreadContract(std::meta::info method) {
              return std::meta::annotations_of(method, ^^ThreadContract).size() > 0;
          }

          /// @brief Extract the ThreadContract from a method's annotation.
          consteval ThreadContract GetThreadContract(std::meta::info method) {
              auto annots = std::meta::annotations_of(method, ^^ThreadContract);
              return std::meta::extract<ThreadContract>(annots[0]);
          }

          /// @brief Check if a reflected member function carries PhaseSet annotation.
          consteval bool HasPhaseSet(std::meta::info method) {
              return std::meta::annotations_of(method, ^^PhaseSet).size() > 0;
          }

          /// @brief Extract PhaseSet from method annotation.
          consteval PhaseSet GetPhaseSet(std::meta::info method) {
              auto annots = std::meta::annotations_of(method, ^^PhaseSet);
              return std::meta::extract<PhaseSet>(annots[0]);
          }

          /// @brief Check if return type is OwnerTask<T> for some T.
          consteval bool IsOwnerTaskReturn(std::meta::info method) {
              auto ret = std::meta::return_type_of(method);
              if (!std::meta::is_specialization_of(ret, ^^OwnerTask))
                  return false;
              return true;
          }

          /// @brief Verify a single Manager type's API contracts at compile time.
          template<typename Manager>
          consteval void VerifyManagerContracts() {
              auto members = std::meta::members_of(^^Manager,
                  std::meta::access_context::unchecked());
              for (auto m : members) {
                  if (!std::meta::is_function(m)) continue;
                  if (!std::meta::is_public(m)) continue;

                  if (HasThreadContract(m)) {
                      auto contract = GetThreadContract(m);

                      // Rule 1: Owner-domain methods MUST return OwnerTask<T>
                      if (contract.RequiresOwnerTransfer()) {
                          if (!IsOwnerTaskReturn(m)) {
                              // static_assert with message via identifier
                              // (consteval context → compile error)
                              throw "Owner-thread method must return OwnerTask<T>";
                          }
                      }

                      // Rule 2: Any-thread methods MUST NOT return OwnerTask
                      if (contract.domain == ThreadDomain::Any) {
                          if (IsOwnerTaskReturn(m)) {
                              throw "Any-thread method must not return OwnerTask<T>";
                          }
                      }
                  }
              }
          }

          /// @brief Generate a compile-time method table for event routing.
          template<typename Manager>
          consteval auto BuildEventHandlerTable() {
              struct HandlerEntry {
                  std::meta::info method;
                  // additional routing metadata
              };
              std::vector<HandlerEntry> table;
              // 反射所有 OnEvent_* 方法，按 EventKind 排序
              auto members = std::meta::members_of(^^Manager,
                  std::meta::access_context::unchecked());
              for (auto m : members) {
                  if (!std::meta::is_function(m)) continue;
                  auto name = std::meta::identifier_of(m);
                  if (name.starts_with("OnEvent_")) {
                      table.push_back({m});
                  }
              }
              return table;
          }

      } // namespace Detail

      // =========================================================================
      // VerifyAllManagers — 在 consteval block 中调用，验证所有 Manager
      // =========================================================================

      template<typename... Managers>
      consteval void VerifyAllManagers() {
          (Detail::VerifyManagerContracts<Managers>(), ...);
      }

  } // namespace Mashiro::Platform

  ---
  七、SystemEvent — Trivially-Copyable, Cache-Line-Fit, Discriminated Union

  // Platform/SystemEvent.h
  #pragma once
  #include "Mashiro/Core/TypeTraits.h"

  #include <cstdint>
  #include <type_traits>

  namespace Mashiro::Platform {

      // =========================================================================
      // Event Kind — sequential enum for array indexing
      // =========================================================================

      enum class EventKind : uint16_t {
          WindowCreate,
          WindowResize,
          WindowClose,
          WindowFocus,
          WindowDpiChange,
          WindowMove,
          InputKeyDown,
          InputKeyUp,
          InputChar,
          InputMouseMove,
          InputMouseButton,
          InputScroll,
          InputDrop,
          DisplayChange,
          ClipboardUpdate,
          ImeComposition,
          ImeCandidateList,
      };

      static_assert(Traits::SequentialEnum<EventKind>);

      // =========================================================================
      // Event Payload — per-kind data
      // =========================================================================

      using WindowId = uint32_t;

      struct WindowResizePayload {
          WindowId window;
          uint32_t width;
          uint32_t height;
          bool isMinimized;
      };

      struct WindowClosePayload {
          WindowId window;
      };

      struct WindowFocusPayload {
          WindowId window;
          bool gained;
      };

      struct WindowDpiPayload {
          WindowId window;
          float scaleX;
          float scaleY;
      };

      struct WindowMovePayload {
          WindowId window;
          int32_t x;
          int32_t y;
      };

      struct KeyPayload {
          WindowId window;
          uint16_t scancode;
          uint16_t virtualKey;
          uint8_t  modifiers; // Ctrl|Shift|Alt|Super bitfield
          bool     isRepeat;
      };

      struct CharPayload {
          WindowId window;
          char32_t codepoint;
      };

      struct MouseMovePayload {
          WindowId window;
          float x;
          float y;
          float deltaX;
          float deltaY;
      };

      struct MouseButtonPayload {
          WindowId window;
          uint8_t button;    // 0=Left, 1=Right, 2=Middle, 3-4=X1/X2
          bool    pressed;
          float   x;
          float   y;
      };

      struct ScrollPayload {
          WindowId window;
          float deltaX;
          float deltaY;
          bool  isPrecise; // trackpad vs discrete wheel
      };

      struct DisplayChangePayload {
          uint32_t monitorId;
          uint32_t width;
          uint32_t height;
          float    refreshRate;
      };

      struct ClipboardPayload {};

      struct ImeCompositionPayload {
          WindowId window;
          uint32_t cursorPos;
          uint32_t selectionStart;
          uint32_t selectionEnd;
          // composition string stored in separate ring buffer (variable-length)
          uint32_t stringOffset;
          uint32_t stringLength;
      };

      // =========================================================================
      // SystemEvent — fixed-size, trivially-copyable event record
      // =========================================================================

      struct SystemEvent {
          EventKind kind;
          uint16_t  flags;       // reserved for coalescing/priority hints
          uint32_t  sequence;    // monotonic sequence number
          uint64_t  timestamp;   // QPC ticks

          union Payload {
              WindowResizePayload    resize;
              WindowClosePayload     close;
              WindowFocusPayload     focus;
              WindowDpiPayload       dpi;
              WindowMovePayload      move;
              KeyPayload             key;
              CharPayload            character;
              MouseMovePayload       mouseMove;
              MouseButtonPayload     mouseButton;
              ScrollPayload          scroll;
              DisplayChangePayload   display;
              ClipboardPayload       clipboard;
              ImeCompositionPayload  ime;

              alignas(8) uint8_t raw[48]; // 保证 union 大小固定
          } payload;
      };

      static_assert(std::is_trivially_copyable_v<SystemEvent>);
      static_assert(sizeof(SystemEvent) == 64);  // 1 cache line

  } // namespace Mashiro::Platform

  ---
  八、MainLoop — Phase-Based with Consteval Phase Table

  // Platform/MainLoop.h
  #pragma once
  #include "Mashiro/Platform/OwnerExecutor.h"
  #include "Mashiro/Platform/SystemEvent.h"
  #include "Mashiro/Platform/ThreadContract.h"
  #include "Mashiro/Core/InlineFunction.h"

  #include <meta>
  #include <span>

  namespace Mashiro::Platform {

      // =========================================================================
      // Phase descriptor — consteval-generated dispatch table
      // =========================================================================

      struct PhaseDescriptor {
          Phase phase;
          void (*execute)(MainLoop& loop);
      };

      // =========================================================================
      // MainLoop
      // =========================================================================

      class MainLoop {
      public:
          void Run();
          void RequestStop() noexcept { running_ = false; }

          Phase CurrentPhase() const noexcept { return executor_.CurrentPhase(); }
          OwnerExecutor& Executor() noexcept { return executor_; }

          // Manager registration (compile-time typed, runtime stored)
          template<typename Manager>
          void RegisterManager(Manager& mgr);

          // Event subscription
          using EventHandler = InlineFunction<void(const SystemEvent&), 64>;
          void Subscribe(EventKind kind, EventHandler handler);

      private:
          OwnerExecutor executor_;
          bool running_ = true;

          // Phase implementations (platform-specific)
          void DoPumpMessages();
          void DoDrainMailbox();
          void DoTranslateEvents();
          void DoDispatchEvents();
          void DoManagerTick();
          void DoPresent();
          void DoIdle();

          // Event storage
          static constexpr size_t kEventKindCount = Traits::EnumeratorsCount<EventKind>;
          // Per-kind handler lists (small_vector style, no heap for typical use)
          struct HandlerSlot {
              static constexpr size_t kInline = 4;
              EventHandler handlers[kInline];
              uint8_t count = 0;
          };
          HandlerSlot handlerTable_[kEventKindCount];

          // Manager tick list
          static constexpr size_t kMaxManagers = 16;
          InlineFunction<void(), 32> managerTicks_[kMaxManagers];
          uint8_t managerCount_ = 0;
      };

      // =========================================================================
      // Consteval phase table — generated by expansion statement
      // =========================================================================

      inline constexpr PhaseDescriptor kPhaseTable[] = {
          {Phase::PumpMessages,    [](MainLoop& l) { l.DoPumpMessages(); }},
          {Phase::DrainMailbox,    [](MainLoop& l) { l.DoDrainMailbox(); }},
          {Phase::TranslateEvents, [](MainLoop& l) { l.DoTranslateEvents(); }},
          {Phase::DispatchEvents,  [](MainLoop& l) { l.DoDispatchEvents(); }},
          {Phase::ManagerTick,     [](MainLoop& l) { l.DoManagerTick(); }},
          {Phase::Present,         [](MainLoop& l) { l.DoPresent(); }},
          {Phase::Idle,            [](MainLoop& l) { l.DoIdle(); }},
      };

      static_assert(std::size(kPhaseTable) == Traits::EnumeratorsCount<Phase>);

  } // namespace Mashiro::Platform

  Main loop 核心循环用 template for 展开，零虚函数调用：

  // 在 .cpp 实现中
  void MainLoop::Run() {
      while (running_) {
          template for (constexpr auto desc : kPhaseTable) {
              executor_.SetPhase(desc.phase);
              desc.execute(*this);
          }
      }
  }

  ---
  九、WindowManager — 完整示例，展示 P3394 Annotation 使用

  // Platform/Managers/WindowManager.h
  #pragma once
  #include "Mashiro/Platform/OwnerTask.h"
  #include "Mashiro/Platform/SystemEvent.h"
  #include "Mashiro/Platform/ThreadContract.h"
  #include "Mashiro/Core/ChunkedSlotMap.h"
  #include "Mashiro/Surface/Window.h"

  #include <meta>
  #include <string>

  namespace Mashiro::Platform {

      // =========================================================================
      // WindowHandle — opaque, non-owning reference to a managed window
      // =========================================================================

      struct WindowHandle {
          WindowId id = 0;
          explicit operator bool() const noexcept { return id != 0; }
          constexpr bool operator==(const WindowHandle&) const = default;
      };

      // =========================================================================
      // NativeWindowView — read-only access to native handle (no ownership transfer)
      // =========================================================================

      struct NativeWindowView {
          void* hwnd = nullptr; // HWND on Windows, opaque elsewhere
      };

      // =========================================================================
      // WindowManager
      // =========================================================================

      class WindowManager {
      public:
          // ----- Owner-thread APIs (annotated) -----

          [[=kOwnerOnly]] [[=kMailboxPhases]]
          OwnerTask<WindowHandle> Create(Window::WindowDesc desc);

          [[=kOwnerOnly]] [[=kMailboxPhases]]
          OwnerTask<void> Destroy(WindowHandle window);

          [[=kOwnerOnly]] [[=kMailboxPhases]]
          OwnerTask<void> SetTitle(WindowHandle window, std::string title);

          [[=kOwnerOnly]] [[=kMailboxPhases]]
          OwnerTask<void> SetSize(WindowHandle window, Window::Size size);

          [[=kOwnerOnly]] [[=kMailboxPhases]]
          OwnerTask<void> SetMode(WindowHandle window, Window::Mode mode);

          [[=kOwnerDeferred]] [[=kAllPhases]]
          OwnerTask<void> RequestClose(WindowHandle window);

          // ----- Any-thread queries (no transfer needed) -----

          [[=kAnyThread]]
          Window::WindowDesc GetDesc(WindowHandle window) const noexcept;

          [[=kAnyThread]]
          Window::Size GetSize(WindowHandle window) const noexcept;

          [[=kAnyThread]]
          NativeWindowView GetNativeView(WindowHandle window) const noexcept;

          [[=kAnyThread]]
          bool IsValid(WindowHandle window) const noexcept;

          // ----- Event handlers (called by MainLoop during DispatchEvents) -----
          void OnEvent_WindowResize(const SystemEvent& event);
          void OnEvent_WindowClose(const SystemEvent& event);
          void OnEvent_WindowFocus(const SystemEvent& event);
          void OnEvent_WindowDpiChange(const SystemEvent& event);
          void OnEvent_WindowMove(const SystemEvent& event);

          // ----- Tick (called by MainLoop during ManagerTick phase) -----
          void Tick();

      private:
          struct WindowState {
              Window::WindowDesc desc;
              void* nativeHandle = nullptr;  // HWND
              bool pendingClose = false;
          };

          // SlotMap gives O(1) insert/remove/lookup with stable handles
          ChunkedSlotMap<WindowState, WindowId> windows_;
      };

      // =========================================================================
      // Compile-time contract verification — fires at program startup-time
      // =========================================================================

      consteval {
          Detail::VerifyManagerContracts<WindowManager>();
      }

  } // namespace Mashiro::Platform

  ---
  十、Consteval-Generated Event Router

  利用 P2996 + P1306 自动为每个 Manager 生成事件分发：

  // Platform/EventRouter.h
  #pragma once
  #include "Mashiro/Platform/SystemEvent.h"
  #include <meta>

  namespace Mashiro::Platform {

      /// @brief Consteval: build a static dispatch table mapping EventKind → Manager method.
      /// Uses naming convention: OnEvent_{EventKindName} → auto-routed.
      template<typename Manager>
      consteval auto BuildRouteTable() {
          struct Route {
              EventKind kind;
              std::meta::info handler;
          };

          std::vector<Route> routes;
          constexpr auto kinds = Traits::Enumerators<EventKind>;

          for (auto member : std::meta::members_of(^^Manager,
                   std::meta::access_context::unchecked())) {
              if (!std::meta::is_function(member)) continue;
              auto name = std::meta::identifier_of(member);
              if (!name.starts_with("OnEvent_")) continue;

              auto kindName = name.substr(8); // strip "OnEvent_"
              for (auto k : kinds) {
                  if (std::meta::identifier_of(k) == kindName) {
                      routes.push_back({
                          .kind = [:k:],
                          .handler = member,
                      });
                      break;
                  }
              }
          }
          return routes;
      }

      /// @brief Runtime dispatch — generated as a switch via template for.
      template<typename Manager>
      void DispatchEvent(Manager& mgr, const SystemEvent& event) {
          constexpr auto routes = std::define_static_array(BuildRouteTable<Manager>());

          template for (constexpr auto route : routes) {
              if (event.kind == route.kind) {
                  (mgr.[:route.handler:])(event);
                  return;
              }
          }
      }

  } // namespace Mashiro::Platform

  编译后等价于手写 switch-case，零运行时反射开销。

  ---
  十一、PhaseGuard — Compile-Time Checked + Debug Assert

  // Platform/PhaseGuard.h
  #pragma once
  #include "Mashiro/Platform/OwnerExecutor.h"
  #include "Mashiro/Platform/ThreadContract.h"

  #include <cassert>

  namespace Mashiro::Platform {

      /// @brief Debug-only: assert current phase is in allowed set.
      inline void AssertPhase([[maybe_unused]] PhaseSet allowed) {
          #ifndef NDEBUG
          Phase current = OwnerExecutor::Instance().CurrentPhase();
          assert(allowed.Permits(current) &&
                 "Operation called in disallowed main loop phase");
          #endif
      }

      /// @brief Debug-only: assert currently on owner thread.
      inline void AssertOwnerThread() {
          #ifndef NDEBUG
          assert(OwnerExecutor::Instance().IsOnOwnerThread() &&
                 "Operation requires owner thread");
          #endif
      }

  } // namespace Mashiro::Platform

  ---
  十二、Cancellation via Generation Counter

  // Platform/Cancellation.h
  #pragma once
  #include <atomic>
  #include <cstdint>

  namespace Mashiro::Platform {

      class CancellationSource {
      public:
          struct Token {
              const std::atomic<uint64_t>& generation;
              uint64_t myGeneration;

              bool IsCancelled() const noexcept {
                  return generation.load(std::memory_order_acquire) != myGeneration;
              }
          };

          Token GetToken() const noexcept {
              return {generation_, generation_.load(std::memory_order_relaxed)};
          }

          void Cancel() noexcept {
              generation_.fetch_add(1, std::memory_order_release);
          }

      private:
          std::atomic<uint64_t> generation_{0};
      };

  } // namespace Mashiro::Platform

  ---
  十三、性能特性总结

  ┌────────────────────────────────┬────────────────────────────────────────┬─────────────────────────┐
  │              路径              │              编译后等价物              │          开销           │
  ├────────────────────────────────┼────────────────────────────────────────┼─────────────────────────┤
  │ 已在 owner thread + safe phase │ 裸函数调用                             │ 0 原子操作              │
  ├────────────────────────────────┼────────────────────────────────────────┼─────────────────────────┤
  │ 跨线程 OwnerTask               │ 1× MPSC push + 1× SetEvent + 1× resume │ 最小化                  │
  ├────────────────────────────────┼────────────────────────────────────────┼─────────────────────────┤
  │ Event dispatch                 │ switch-case (template for 展开)        │ 0 虚调用                │
  ├────────────────────────────────┼────────────────────────────────────────┼─────────────────────────┤
  │ Manager tick                   │ 直接函数指针 (InlineFunction)          │ 0 heap, 1 indirect call │
  ├────────────────────────────────┼────────────────────────────────────────┼─────────────────────────┤
  │ Contract 验证                  │ consteval → compile error              │ 0 运行时                │
  ├────────────────────────────────┼────────────────────────────────────────┼─────────────────────────┤
  │ Phase 验证                     │ debug assert / release 中消失          │ 0 在 release            │
  ├────────────────────────────────┼────────────────────────────────────────┼─────────────────────────┤
  │ Event 复制                     │ memcpy 64 bytes                        │ trivially copyable      │
  └────────────────────────────────┴────────────────────────────────────────┴─────────────────────────┘

  ---
  十四、与现有基础设施的衔接

  ┌───────────────────────────────┬──────────────────────────────────────────────────────────────────┐
  │           现有设施            │                         在本架构中的角色                         │
  ├───────────────────────────────┼──────────────────────────────────────────────────────────────────┤
  │ Generator<Ref, V, Alloc>      │ 保持不变，互补（pull-based lazy range vs push-based owner task） │
  ├───────────────────────────────┼──────────────────────────────────────────────────────────────────┤
  │ SpscRingBuffer / SpscQueue    │ SystemEventQueue 底层存储 + IME string buffer                    │
  ├───────────────────────────────┼──────────────────────────────────────────────────────────────────┤
  │ InlineFunction<Sig, Cap>      │ Manager tick 回调、event handler 存储                            │
  ├───────────────────────────────┼──────────────────────────────────────────────────────────────────┤
  │ ChunkedSlotMap                │ WindowManager 的 window state 存储                               │
  ├───────────────────────────────┼──────────────────────────────────────────────────────────────────┤
  │ Result<T> / ErrorCode         │ Manager fallible APIs 可返回 OwnerTask<Result<T>>                │
  ├───────────────────────────────┼──────────────────────────────────────────────────────────────────┤
  │ Traits::SequentialEnum        │ EventKind 验证 + 数组索引                                        │
  ├───────────────────────────────┼──────────────────────────────────────────────────────────────────┤
  │ Traits::BitfieldEnum          │ Window::Flags, modifier keys                                     │
  ├───────────────────────────────┼──────────────────────────────────────────────────────────────────┤
  │ Iota<N> / define_static_array │ Phase table、route table 的编译期构建                            │
  ├───────────────────────────────┼──────────────────────────────────────────────────────────────────┤
  │ FixedString                   │ consteval event name matching                                    │
  ├───────────────────────────────┼──────────────────────────────────────────────────────────────────┤
  │ TypeTraits::Members<T>        │ ManagerTraits 的内省基础                                         │
  ├───────────────────────────────┼──────────────────────────────────────────────────────────────────┤
  │ SoA::Array                    │ 如果 event batch processing 需要 SoA 布局                        │
  ├───────────────────────────────┼──────────────────────────────────────────────────────────────────┤
  │ ThreadNaming                  │ MainLoop::Run() 开始时设置 owner thread name                     │
  ├───────────────────────────────┼──────────────────────────────────────────────────────────────────┤
  │ Annotation.h                  │ 数据层 schema 不变，不侵入                                       │
  ├───────────────────────────────┼──────────────────────────────────────────────────────────────────┤
  │ P3394 [[=...]]                │ 本架构的核心合约机制（如 SOA.h 的 Skip/Align）                   │
  ├───────────────────────────────┼──────────────────────────────────────────────────────────────────┤
  │ consteval {} blocks           │ Manager contract 验证、event router 生成                         │
  ├───────────────────────────────┼──────────────────────────────────────────────────────────────────┤
  │ template for                  │ Phase loop 展开、event dispatch 展开、contract iteration         │
  └───────────────────────────────┴──────────────────────────────────────────────────────────────────┘

  ---
  十五、超越上一版设计的改进

  1. P3394 Annotation 直接标注在方法上 — 不需要 wrapper struct / CRTP base / concept 间接层。[[=kOwnerOnly]]
  直接写在函数声明上，annotations_of 在 consteval block 中验证。
  2. Consteval Event Router — 利用反射 + naming convention 自动生成 switch dispatch，无需手写 dispatch
  table、无虚函数。新增 Manager 只需写 OnEvent_WindowResize 方法，路由自动生成。
  3. Phase Table 用 template for 展开 — main loop 的 phase 执行是完全展开的，没有运行时数组索引。
  4. Contract 验证在 consteval block 中 — 不是延迟到模板实例化，而是在 namespace scope 的 consteval {}
  中立即验证，错误信息在编译最早期暴露。
  5. MPSC queue 替代 SPSC for mailbox — 多 worker 线程可同时向 owner 提交请求，不需要 per-thread channel。
  6. SystemEvent 固定 64 bytes = 1 cache line — trivially copyable，可以 memcpy 到 ring buffer，no serialization
  overhead。
  7. NativeWindowView 替代裸 HWND 暴露 — 类型安全的非所有权视图，consteval 可验证 Manager 不返回裸指针。

  ---
  这个设计是否满足你的期望？是否要对某个子系统进一步深入，或者进入 spec 文档编写阶段？
  ⎿  Stop says:
     ---
     ✅ Session ended

     📁 Git repository
       Branch: main
       Changes:
         增加: 3 files
         修改: 4 files

     ✅ No temp files
     ---

✻ Churned for 3m 31s

❯ 我 原本希望Platform线程只通过pump转发来自系统的event给pump另一端的clients，然后各种manager只接受其他线程的调用请求并
  在platform线程执行的。请深入诚实评价我的这一想法并review此前架构设计。要求充分利用包括reflection/consteval
  block和annotation在内的C++20~26特性优势，不要有性能损失，尽量编译期完成。要完备、架构前沿且正确，无技术债。

● No skills needed — this is a continuation of the ongoing brainstorming/design discussion.

  ---
  诚实评价你的原始想法

  你的原始意图可以精确复述为两条独立职责：

  1. Platform 线程作为 Event Pump — 从 OS（Win32 message queue）收取事件，转换为 canonical schema，通过某种通道转发给
  pump 的另一端（消费者线程/模块）
  2. Platform 线程作为 Owner Executor — 各种 Manager 接受来自其他线程的调用请求，但实际执行发生在 Platform 线程（因为
  HWND/IME/clipboard 的 thread affinity）

  这两条职责是正交的数据流方向：

  事件流：  OS → Platform Thread → [pump channel] → Clients (render/logic/UI)
  调用流：  Clients → [mailbox] → Platform Thread → Manager 执行 → 结果返回 Client

  ---
  对前一版架构设计的诚实 Review

  前一版设计有以下问题，对照你的原始意图：

  问题 1：Manager 被设计成直接处理事件

  前一版中 Manager 有 OnEvent_WindowResize 等方法，MainLoop 在 DispatchEvents phase 中直接把事件分发给
  Manager。这混淆了两个正交的数据流。

  你的意图：事件应该被 pump 转发给 pump 另一端的 clients。Manager 不消费事件，Manager 只执行被调用的操作。

  正确建模：如果 WindowManager 收到一个 resize event，那不是因为它"订阅"了 resize，而是因为 OS 通知了 Platform
  thread。Platform thread 的职责是把这个 event 放进 event channel 让 client 端读取。Manager
  自己可能需要更新内部状态（比如记录当前窗口尺寸），但这是因为它是 window state 的 owner，不是因为它是 event consumer。

  修正：事件流和调用流分离。事件流由 EventPump 管理，直接写入 channel 给 client 端。Manager 的内部状态更新是 Platform
  thread 内部的 bookkeeping，不走 event dispatch 路径。

  问题 2：Phase-based loop 过度复杂化

  前一版有 7 个 phase（PumpMessages, DrainMailbox, TranslateEvents, DispatchEvents, ManagerTick, Present,
  Idle）。对照你的原始意图，Platform 线程只做两件事：

  1. Pump OS messages → 转换 → 写入 event channel
  2. Drain mailbox → 执行 Manager 调用请求 → 返回结果

  不需要 ManagerTick（Manager 没有 per-frame 逻辑，它只响应请求）。不需要 DispatchEvents（事件直接进 channel，不分发给
  Manager）。不需要 Present（那是 renderer 的事）。

  修正：Phase 简化为实际需要的最小集合。

  问题 3：OwnerTask 的 initial_suspend 逻辑有问题

  前一版的 TransferToOwner::await_ready() 检查 "IsOnOwnerThread() && IsInSafePhase()"。但如果调用者已经在 owner
  thread（比如一个 Manager 方法内部调用另一个 Manager 方法），它应该直接执行而不是挂起。前一版缺少对 "Manager 在
  Platform thread 内部相互调用" 这个场景的建模。

  问题 4：Event Router 用反射自动生成是过度设计

  如果 event 只是被写入 channel，不需要 router。前一版的 BuildRouteTable / DispatchEvent 整套机制是基于 "Manager 消费
  event" 这个错误前提建立的。

  问题 5：缺少 "pump 另一端" 的建模

  前一版没有明确 event channel 的消费端是什么。是一个 ring buffer？是一个 per-client queue？是 broadcast 还是
  unicast？这是架构的关键接口，不能遗漏。

  ---
  重新设计：忠于你的原始意图

  核心模型

  ┌─────────────────────────────────────────────────────────┐
  │                    Platform Thread                        │
  │                                                          │
  │  ┌──────────┐       ┌──────────────┐                    │
  │  │ OS Pump  │──────▶│ Event Channel│──────▶ Clients     │
  │  │ (Win32)  │  emit │ (SPSC ring)  │  read              │
  │  └──────────┘       └──────────────┘                    │
  │                                                          │
  │  ┌──────────┐       ┌──────────────┐                    │
  │  │ Mailbox  │◀──────│   Clients    │                    │
  │  │ (MPSC)   │ submit│ (any thread) │                    │
  │  └──────────┘       └──────────────┘                    │
  │       │                                                  │
  │       ▼                                                  │
  │  ┌──────────────────────────────────────────┐           │
  │  │ Managers (execute on platform thread)     │           │
  │  │  WindowManager, ClipboardManager, ...     │           │
  │  └──────────────────────────────────────────┘           │
  └─────────────────────────────────────────────────────────┘

  两条数据流，两个方向，互不干扰。

  ---
  重新设计的文件结构

  Mashiro/include/Mashiro/Platform/
  ├── ThreadContract.h        — P3394 annotation: thread affinity contracts
  ├── EventPump.h             — OS message → SystemEvent 转换 + 写入 channel
  ├── EventChannel.h          — Platform→Client 的 event 传输通道
  ├── SystemEvent.h           — Canonical event schema (trivially copyable)
  ├── OwnerExecutor.h         — MPSC mailbox + coroutine resume
  ├── OwnerTask.h             — Owner-affine coroutine primitive
  ├── ManagerTraits.h         — Consteval contract verification via P3394/P2996
  ├── PlatformThread.h        — 主循环（只有 Pump + Drain 两个动作）
  └── Managers/
      ├── WindowManager.h
      ├── InputManager.h
      ├── ClipboardManager.h
      └── SurfaceManager.h

  ---
  A. PlatformThread — 极简主循环

  Platform 线程只做两件事，循环交替：

  // Platform/PlatformThread.h
  #pragma once
  #include "Mashiro/Platform/EventPump.h"
  #include "Mashiro/Platform/OwnerExecutor.h"

  namespace Mashiro::Platform {

      class PlatformThread {
      public:
          void Run();
          void RequestStop() noexcept { running_ = false; }

          OwnerExecutor& Executor() noexcept { return executor_; }
          EventPump& Pump() noexcept { return pump_; }

      private:
          EventPump pump_;
          OwnerExecutor executor_;
          bool running_ = true;
      };

  } // namespace Mashiro::Platform

  实现：

  void PlatformThread::Run() {
      SetCurrentThreadName("Platform");
      executor_.Initialize(GetCurrentThreadId(), pump_.WakeHandle());

      while (running_) {
          // 1. Pump: 取出所有待处理的 Win32 messages，转为 SystemEvent，写入 channel
          pump_.PumpAll();

          // 2. Drain: 恢复所有跨线程投递的 coroutine handles（Manager 调用请求）
          executor_.DrainAll();

          // 3. Wait: 如果没有 pending work，阻塞等待 message 或 mailbox wake
          if (!pump_.HasPending() && !executor_.HasPending()) {
              pump_.WaitForWork(); // MsgWaitForMultipleObjects
          }
      }
  }

  没有 phase 枚举。没有 ManagerTick。没有 EventDispatch。Platform thread 的职责极其清晰：pump 事件出去，drain 请求进来。

  ---
  B. EventPump + EventChannel — 事件流的正确建模

  // Platform/EventChannel.h
  #pragma once
  #include "Mashiro/Platform/SystemEvent.h"
  #include "Mashiro/Core/SpscRingBuffer.h"

  namespace Mashiro::Platform {

      /// @brief Platform→Client 事件通道。
      /// Platform thread 是唯一 producer，client thread 是唯一 consumer。
      /// 如果有多个 consumer（render + UI + logic），每个持有独立 channel。
      template<size_t Capacity = 4096>
      using EventChannel = SpscQueue<SystemEvent, Capacity>;

  } // namespace Mashiro::Platform

  // Platform/EventPump.h
  #pragma once
  #include "Mashiro/Platform/EventChannel.h"
  #include "Mashiro/Platform/SystemEvent.h"

  namespace Mashiro::Platform {

      class EventPump {
      public:
          /// @brief 注册一个 event channel（由 client 持有 consumer 端）。
          /// 多个 channel = broadcast 给多个 consumer。
          void AttachChannel(EventChannel<>& channel) noexcept;

          /// @brief Pump 所有 pending Win32 messages，转为 SystemEvent 写入所有 channel。
          void PumpAll();

          /// @brief 是否有未处理的 Win32 message。
          bool HasPending() const noexcept;

          /// @brief MsgWaitForMultipleObjects — 等待 message 或外部 wake event。
          void WaitForWork() noexcept;

          /// @brief 返回可被外部 SetEvent() 唤醒的 handle。
          void* WakeHandle() const noexcept { return wakeEvent_; }

      private:
          struct ChannelSlot {
              EventChannel<>* channel = nullptr;
          };
          static constexpr size_t kMaxChannels = 8;
          ChannelSlot channels_[kMaxChannels];
          uint8_t channelCount_ = 0;

          void* wakeEvent_ = nullptr; // HANDLE, CreateEvent()

          void Emit(const SystemEvent& event) noexcept;
          SystemEvent TranslateMessage(/* platform-specific msg */) noexcept;
      };

  } // namespace Mashiro::Platform

  关键设计决策：
  - 用现有的 SpscQueue<SystemEvent, N>（你已经有这个基础设施）作为 channel
  - 一个 Platform thread 可以 broadcast 到多个 channel（render thread 一个，UI thread 一个）
  - Channel 是 SPSC：Platform thread 是唯一 producer，每个 consumer thread 是唯一 consumer
  - 如果 consumer 读不过来，event 丢弃（或 backpressure — 取决于策略）

  ---
  C. OwnerTask<T> — 简化版（移除 phase 检查）

  既然不再有 phase 枚举，TransferToOwner 只需判断线程 ID：

  // Platform/OwnerTask.h
  #pragma once
  #include <coroutine>
  #include <exception>
  #include <type_traits>
  #include <utility>

  namespace Mashiro::Platform {

      class OwnerExecutor;

      // =========================================================================
      // TransferToOwner — 判断是否需要迁移到 platform thread
      // =========================================================================

      struct TransferToOwner {
          bool await_ready() noexcept;   // == IsOnPlatformThread()
          void await_suspend(std::coroutine_handle<> h) noexcept; // enqueue to mailbox
          void await_resume() noexcept {}
      };

      // =========================================================================
      // OwnerTask<T>
      // =========================================================================

      template<typename T>
      class [[nodiscard]] OwnerTask {
      public:
          struct Promise {
              std::conditional_t<std::is_void_v<T>, char, T> result_;
              std::exception_ptr exception_;
              std::coroutine_handle<> continuation_{std::noop_coroutine()};
              bool ready_ = false;

              OwnerTask get_return_object() noexcept {
                  return OwnerTask{std::coroutine_handle<Promise>::from_promise(*this)};
              }

              TransferToOwner initial_suspend() noexcept { return {}; }

              auto final_suspend() noexcept {
                  struct Completion {
                      bool await_ready() noexcept { return false; }
                      std::coroutine_handle<> await_suspend(
                          std::coroutine_handle<Promise> h) noexcept {
                          h.promise().ready_ = true;
                          return h.promise().continuation_;
                      }
                      void await_resume() noexcept {}
                  };
                  return Completion{};
              }

              template<typename U>
                  requires (!std::is_void_v<T> && std::convertible_to<U, T>)
              void return_value(U&& val) {
                  if constexpr (!std::is_void_v<T>)
                      result_ = std::forward<U>(val);
              }

              void return_void() requires std::is_void_v<T> {}

              void unhandled_exception() noexcept {
                  exception_ = std::current_exception();
              }
          };

          using promise_type = Promise;
          using handle_type = std::coroutine_handle<Promise>;

          // Awaitable interface (for co_await from any coroutine)
          bool await_ready() const noexcept { return handle_.promise().ready_; }

          std::coroutine_handle<> await_suspend(std::coroutine_handle<> caller) noexcept {
              handle_.promise().continuation_ = caller;
              return handle_;
          }

          T await_resume() requires (!std::is_void_v<T>) {
              if (handle_.promise().exception_)
                  std::rethrow_exception(handle_.promise().exception_);
              return std::move(handle_.promise().result_);
          }

          void await_resume() requires std::is_void_v<T> {
              if (handle_.promise().exception_)
                  std::rethrow_exception(handle_.promise().exception_);
          }

          ~OwnerTask() { if (handle_) handle_.destroy(); }
          OwnerTask(OwnerTask&& o) noexcept : handle_(std::exchange(o.handle_, nullptr)) {}
          OwnerTask(const OwnerTask&) = delete;
          OwnerTask& operator=(const OwnerTask&) = delete;
          OwnerTask& operator=(OwnerTask&&) = delete;

      private:
          handle_type handle_;
          explicit OwnerTask(handle_type h) noexcept : handle_(h) {}
      };

  } // namespace Mashiro::Platform

  对比前一版的改进：
  - 移除了 Phase 概念 — Platform thread 只有一个 drain 动作，不存在 "wrong phase" 问题
  - await_ready() 仅判断 IsOnPlatformThread() — 如果已在 platform thread 直接执行
  - 移除了 OwnerExecutor* executor_ 字段 — 使用全局 singleton（platform thread 只有一个）

  ---
  D. ThreadContract + ManagerTraits — Consteval 验证保留

  P3394 annotation 的验证逻辑保留，但简化——因为不再有 Phase，只验证线程域：

  // Platform/ThreadContract.h
  #pragma once
  #include <cstdint>

  namespace Mashiro::Platform {

      enum class ThreadDomain : uint8_t {
          Platform,  // 必须在 platform thread 执行
          Any,       // 任意线程可执行（通常是只读查询）
      };

      struct ThreadContract {
          ThreadDomain domain = ThreadDomain::Platform;
          constexpr bool operator==(const ThreadContract&) const = default;
      };

      inline constexpr ThreadContract kPlatformOnly{.domain = ThreadDomain::Platform};
      inline constexpr ThreadContract kAnyThread{.domain = ThreadDomain::Any};

  } // namespace Mashiro::Platform

  // Platform/ManagerTraits.h
  #pragma once
  #include "Mashiro/Platform/ThreadContract.h"
  #include "Mashiro/Platform/OwnerTask.h"
  #include <meta>

  namespace Mashiro::Platform::Detail {

      /// @brief Consteval: verify all annotated methods on a Manager type.
      template<typename Manager>
      consteval void VerifyManagerContracts() {
          for (auto m : std::meta::members_of(^^Manager,
                   std::meta::access_context::unchecked())) {
              if (!std::meta::is_function(m)) continue;
              if (!std::meta::is_public(m)) continue;

              auto contracts = std::meta::annotations_of(m, ^^ThreadContract);
              if (contracts.size() == 0) continue;

              auto contract = std::meta::extract<ThreadContract>(contracts[0]);

              if (contract.domain == ThreadDomain::Platform) {
                  // Rule: Platform-domain method must return OwnerTask<T>
                  auto ret = std::meta::return_type_of(m);
                  if (!std::meta::is_specialization_of(ret, ^^OwnerTask)) {
                      throw "Platform-thread method must return OwnerTask<T>";
                  }
              }

              if (contract.domain == ThreadDomain::Any) {
                  // Rule: Any-thread method must NOT return OwnerTask
                  auto ret = std::meta::return_type_of(m);
                  if (std::meta::is_specialization_of(ret, ^^OwnerTask)) {
                      throw "Any-thread method should not return OwnerTask<T>";
                  }
              }
          }
      }

  } // namespace Mashiro::Platform::Detail

  ---
  E. WindowManager — 忠于你的意图

  Manager 不消费事件。Manager 只接受调用请求：

  // Platform/Managers/WindowManager.h
  #pragma once
  #include "Mashiro/Platform/OwnerTask.h"
  #include "Mashiro/Platform/ThreadContract.h"
  #include "Mashiro/Surface/Window.h"
  #include "Mashiro/Core/ChunkedSlotMap.h"

  #include <string>
  #include <meta>

  namespace Mashiro::Platform {

      struct WindowHandle {
          uint32_t id = 0;
          explicit operator bool() const noexcept { return id != 0; }
          constexpr bool operator==(const WindowHandle&) const = default;
      };

      struct NativeWindowView {
          void* hwnd = nullptr;
      };

      class WindowManager {
      public:
          // ---- Platform-thread APIs: accept requests, execute on platform thread ----

          [[=kPlatformOnly]]
          OwnerTask<WindowHandle> Create(Window::WindowDesc desc);

          [[=kPlatformOnly]]
          OwnerTask<void> Destroy(WindowHandle window);

          [[=kPlatformOnly]]
          OwnerTask<void> SetTitle(WindowHandle window, std::string title);

          [[=kPlatformOnly]]
          OwnerTask<void> SetSize(WindowHandle window, Window::Size size);

          [[=kPlatformOnly]]
          OwnerTask<void> SetMode(WindowHandle window, Window::Mode mode);

          [[=kPlatformOnly]]
          OwnerTask<void> Show(WindowHandle window);

          [[=kPlatformOnly]]
          OwnerTask<void> Hide(WindowHandle window);

          // ---- Any-thread queries: no transfer, direct read ----

          [[=kAnyThread]]
          Window::WindowDesc GetDesc(WindowHandle window) const noexcept;

          [[=kAnyThread]]
          Window::Size GetSize(WindowHandle window) const noexcept;

          [[=kAnyThread]]
          NativeWindowView GetNativeView(WindowHandle window) const noexcept;

          [[=kAnyThread]]
          bool IsValid(WindowHandle window) const noexcept;

      private:
          friend class EventPump; // EventPump 需要在 pump 时更新内部状态

          // EventPump 在 pump 过程中调用这些方法更新 bookkeeping
          // 这些不是"事件消费"，是 Platform thread 内部的 state sync
          void UpdateSize(WindowHandle window, uint32_t w, uint32_t h) noexcept;
          void UpdatePosition(WindowHandle window, int32_t x, int32_t y) noexcept;
          void MarkClosed(WindowHandle window) noexcept;

          struct WindowState {
              Window::WindowDesc desc;
              void* nativeHandle = nullptr;
              bool alive = true;
          };

          ChunkedSlotMap<WindowState, uint32_t> windows_;
      };

      // Compile-time contract verification
      consteval { Detail::VerifyManagerContracts<WindowManager>(); }

  } // namespace Mashiro::Platform

  关键区别：
  - Manager 没有 OnEvent_* 方法。没有 Tick() 方法。
  - EventPump 在转换 Win32 message 时，如果消息影响 Manager 的内部状态（如 WM_SIZE → WindowManager
  需要更新记录），EventPump 直接调用 Manager 的 private Update* 方法。这不是事件消费，是同一线程内部的 bookkeeping。
  - 对外 API 面只有：接受请求的 OwnerTask<T> 方法 + 无锁只读查询。

  ---
  F. Client 端的使用模型

  // 在 render thread 或 logic thread 中：
  auto& platform = GetPlatformThread();
  auto& windows = platform.Windows();

  // 创建窗口 — 自动迁移到 platform thread 执行
  auto window = co_await windows.Create({
      .title = "Editor",
      .size = {1920, 1080},
      .flags = Window::Flags::Resizable | Window::Flags::HighDpi,
  });

  // 修改窗口 — 同样自动迁移
  co_await windows.SetTitle(window, "Editor - project.mashiro");

  // 查询 — 直接返回，不迁移（Any-thread）
  auto size = windows.GetSize(window);

  // 读取事件 — 从自己持有的 EventChannel consumer 端读取
  while (auto event = myEventChannel.TryPop()) {
      switch (event->kind) {
          case EventKind::WindowResize:
              OnResize(event->payload.resize);
              break;
          case EventKind::InputKeyDown:
              OnKeyDown(event->payload.key);
              break;
          // ...
      }
  }

  ---
  G. 对比评价

  ┌──────────────────────┬──────────────────────────────────────────────┬────────────────────────────────────────┐
  │         维度         │                    前一版                    │                重新设计                │
  ├──────────────────────┼──────────────────────────────────────────────┼────────────────────────────────────────┤
  │ Platform thread 职责 │ Pump + Translate + Dispatch + Tick + Present │ Pump + Drain（只有两件事）             │
  ├──────────────────────┼──────────────────────────────────────────────┼────────────────────────────────────────┤
  │ 事件流向             │ Platform → Manager (内部消费)                │ Platform → Channel → Client (穿透转发) │
  ├──────────────────────┼──────────────────────────────────────────────┼────────────────────────────────────────┤
  │ Manager 角色         │ 消费事件 + 执行请求 + 每帧 tick              │ 仅执行请求 + 内部 bookkeeping          │
  ├──────────────────────┼──────────────────────────────────────────────┼────────────────────────────────────────┤
  │ Phase 系统           │ 7 个 phase, consteval phase table            │ 无 phase（不需要）                     │
  ├──────────────────────┼──────────────────────────────────────────────┼────────────────────────────────────────┤
  │ Event Router         │ consteval 自动生成 switch                    │ 不需要（event 不路由给 Manager）       │
  ├──────────────────────┼──────────────────────────────────────────────┼────────────────────────────────────────┤
  │ 复杂度               │ ~15 个概念                                   │ ~7 个概念                              │
  ├──────────────────────┼──────────────────────────────────────────────┼────────────────────────────────────────┤
  │ C++26 特性利用       │ 过度（为了展示而用）                         │ 精确（只在产生价值的地方用）           │
  └──────────────────────┴──────────────────────────────────────────────┴────────────────────────────────────────┘

  ---
  H. C++26 特性在新设计中的精确使用点

  ┌──────────────────────────────────┬───────────────────────────────────────┬──────────────────────────────────────┐
  │               特性               │               使用位置                │              产生的价值              │
  ├──────────────────────────────────┼───────────────────────────────────────┼──────────────────────────────────────┤
  │ P3394 [[=...]] annotation        │ Manager 方法的 ThreadContract 标注    │ 零 boilerplate 声明线程合约          │
  ├──────────────────────────────────┼───────────────────────────────────────┼──────────────────────────────────────┤
  │ P2996 reflection +               │ VerifyManagerContracts consteval      │ 编译期自动验证所有 Manager API 合约  │
  │ annotations_of                   │                                       │                                      │
  ├──────────────────────────────────┼───────────────────────────────────────┼──────────────────────────────────────┤
  │ P3289 consteval {} block         │ Manager 定义后立即触发验证            │ 错误在最早编译期暴露，不延迟到实例化 │
  ├──────────────────────────────────┼───────────────────────────────────────┼──────────────────────────────────────┤
  │ P1306 template for               │ （可选）如果有多个 Manager            │ 静态展开                             │
  │                                  │ 需要批量验证                          │                                      │
  ├──────────────────────────────────┼───────────────────────────────────────┼──────────────────────────────────────┤
  │ P3491 define_static_array        │ EventKind enumerators 用于 channel    │ 编译期 range 生成                    │
  │                                  │ 统计                                  │                                      │
  ├──────────────────────────────────┼───────────────────────────────────────┼──────────────────────────────────────┤
  │ coroutines (co_await)            │ OwnerTask 的跨线程请求模型            │ 自然的异步 API，调用者无需手写回调   │
  ├──────────────────────────────────┼───────────────────────────────────────┼──────────────────────────────────────┤
  │ 现有 SpscQueue                   │ EventChannel 的底层存储               │ 零 allocation event 传输             │
  ├──────────────────────────────────┼───────────────────────────────────────┼──────────────────────────────────────┤
  │ 现有 InlineFunction              │ （如果需要）极少数场景                │ 避免 heap                            │
  ├──────────────────────────────────┼───────────────────────────────────────┼──────────────────────────────────────┤
  │ 现有 Traits::SequentialEnum      │ EventKind 验证                        │ 保证 enum 可用作数组索引             │
  └──────────────────────────────────┴───────────────────────────────────────┴──────────────────────────────────────┘

  原则：特性只在产生不可替代价值的地方使用。不为展示而用。

  ---
  I. 此设计中 不需要的 概念（对比前一版删除的）

  - ~~Phase 枚举~~ — Platform thread 没有 phase，只有 pump 和 drain
  - ~~PhaseGuard~~ — 不需要
  - ~~PhaseSet annotation~~ — 不需要
  - ~~Reentrancy 枚举~~ — Platform thread 内部不存在重入风险（pump 和 drain 严格交替）
  - ~~Event Router / BuildRouteTable~~ — event 不路由给 Manager
  - ~~ManagerTick~~ — Manager 没有 per-frame 逻辑
  - ~~kPhaseTable~~ — 没有 phase table
  - ~~MainLoop (7-phase)~~ — 替换为极简 PlatformThread
  - ~~HandlerSlot / Subscribe 机制~~ — event 走 channel，不走 pub-sub

  ---
  J. 需要讨论的开放问题

  1. EventChannel 是 broadcast 还是 per-consumer 独立？
    - 当前设计：每个 consumer 一个独立 SpscQueue（broadcast 模式，EventPump 写入所有 channel）
    - 替代：单 channel + 多 reader（需要 MPMC，更复杂）
    - 推荐：broadcast SPSC，因为你已有 SpscQueue，且 consumer 数量少（2-4 个线程）
  2. Any-thread 查询的一致性保证？
    - GetSize() 从任意线程读，Platform thread 在 pump 时更新。需要保证读到一致的值。
    - 选项 A：原子操作或 SeqLock（单 writer multiple reader）
    - 选项 B：接受最终一致性（调用者可能读到上一帧的值）
    - 推荐：最终一致性 + atomic store/load for individual fields。对于 WindowDesc 这种复合结构，用 SeqLock。
  3. Manager 内部 bookkeeping（UpdateSize 等）的时机？
    - 当前设计：在 EventPump::PumpAll() 内部，每翻译一条 Win32 message 就更新
    - 这意味着 Manager 状态和 event channel 中的 event 是同步的：当 client 从 channel 读到 resize event 时，GetSize()
  已经返回新值
    - 这是正确的行为

  ---
  总结：你的原始想法是正确的，且比前一版更简洁。Platform thread 的职责应该是 pump event 出去 + drain request
  进来，不多不少。前一版过度复杂化了 Manager 的角色（让它既消费事件又执行请求又每帧 tick），违反了单一职责原则。

  要继续深入哪个开放问题，还是进入 spec 编写？
  ⎿  Stop says:
     ---
     ✅ Session ended

     📁 Git repository
       Branch: main
       Changes:
         增加: 3 files
         修改: 4 files

     ✅ No temp files
     ---

✻ Sautéed for 2m 31s

❯ 接受所有推荐。另外，client一侧需要可以使用co_await来等待pump生产端的下一个事件或者下一组事件，且client一般在另一线程
  上的协程，pump需要处理好相同和不同线程问题。            要求充分利用包括reflection /
                  consteval block和annotation在内的C++ 20 ~26特性优势，不要有性能损失，尽量编译期完成。要完备、架构前沿
  且正确，无技术债。要求抽象层次正确，符合问题和客观世界建模。

● No skills needed — continuing the architecture design session.

  Let me check the existing SpscRingBuffer for its exact interface to build the awaitable channel on top of it.

  Read 1 file (ctrl+o to expand)

● Good. I have the complete picture. Now I'll present the final comprehensive architecture design with the awaitable
  EventChannel.

  ---
  最终完整架构设计

  基于已确认的决策：broadcast SPSC channels、SeqLock for composite queries、最终一致性。新增核心需求：client 侧可
  co_await 下一个事件或下一批事件，跨线程唤醒。

  ---
  核心数据流模型（最终版）

  ┌────────────────────────────────────────────────────────────────┐
  │                      Platform Thread                            │
  │                                                                │
  │  ┌──────────┐    emit    ┌───────────────────────┐             │
  │  │ OS Pump  │───────────▶│ EventChannel (per-client)│           │
  │  │ (Win32)  │            │  SPSC ring + wake       │──────────▶│ Client coroutines
  │  └──────────┘            └───────────────────────┘   co_await  │ (render/logic/UI)
  │       │                                                        │
  │       │ bookkeeping      ┌──────────────┐                      │
  │       └─────────────────▶│ Managers     │◀─── OwnerTask<T> ───┤ Client coroutines
  │                          │ (state owners)│     (cross-thread)   │
  │                          └──────────────┘                      │
  │                                                                │
  │  ┌──────────┐    drain   ┌───────────────────────┐             │
  │  │ Executor │◀───────────│ MPSC Mailbox           │◀───────────┤ Any thread
  │  │ (resume) │            │ (coroutine handles)    │   submit   │
  │  └──────────┘            └───────────────────────┘             │
  └────────────────────────────────────────────────────────────────┘

  ---
  文件结构（最终版）

  Mashiro/include/Mashiro/Platform/
  ├── ThreadContract.h         — P3394 annotations for thread affinity
  ├── SystemEvent.h            — Trivially-copyable event schema
  ├── EventChannel.h           — Awaitable SPSC channel (core of this design)
  ├── EventPump.h              — Win32 → SystemEvent + broadcast to channels
  ├── OwnerExecutor.h          — MPSC mailbox + coroutine drain
  ├── OwnerTask.h              — Owner-affine coroutine return type
  ├── MpscQueue.h              — Lock-free multi-producer single-consumer queue
  ├── SeqLock.h                — Single-writer multi-reader consistent read
  ├── PlatformThread.h         — The thread itself (pump + drain loop)
  ├── ManagerTraits.h          — Consteval contract verification
  └── Managers/
      ├── WindowManager.h
      ├── InputManager.h
      ├── ClipboardManager.h
      └── SurfaceManager.h

  Mashiro/src/Platform/
  ├── Windows/
  │   ├── PlatformThread.cpp
  │   ├── EventPump.cpp        — WndProc → SystemEvent translation
  │   ├── OwnerExecutor.cpp
  │   └── Managers/
  │       ├── WindowManager.cpp
  │       ├── ClipboardManager.cpp
  │       └── ...

  ---
  一、EventChannel — 可 co_await 的跨线程事件通道

  这是本次设计的核心新增。语义：

  - Producer（Platform thread）：推入事件后唤醒等待中的 consumer coroutine
  - Consumer（client thread coroutine）：co_await channel.Next() 挂起直到有事件；co_await channel.NextBatch()
  挂起直到有一批事件
  - 同线程 fast path：如果 channel 已有数据，co_await 立即返回（不挂起）
  - 跨线程 path：consumer coroutine 挂起，handle 注册到 channel；producer push 后恢复 handle

  // Platform/EventChannel.h
  #pragma once
  #include "Mashiro/Platform/SystemEvent.h"
  #include "Mashiro/Core/SpscRingBuffer.h"
  #include "Mashiro/Core/TypeTraits.h"

  #include <atomic>
  #include <coroutine>
  #include <optional>
  #include <span>
  #include <cstdint>

  namespace Mashiro::Platform {

      // =========================================================================
      // EventChannel — Awaitable SPSC event channel
      // =========================================================================

      template<uint32_t Capacity = 4096>
      class EventChannel {
          static_assert(std::has_single_bit(Capacity) && Capacity >= 64);

      public:
          EventChannel() = default;
          EventChannel(const EventChannel&) = delete;
          EventChannel& operator=(const EventChannel&) = delete;

          // =================================================================
          // Producer API (Platform thread only)
          // =================================================================

          /// @brief Push an event and wake suspended consumer if any.
          bool Emit(const SystemEvent& event) noexcept {
              bool pushed = ring_.TryPush(event);
              if (pushed) WakeConsumer();
              return pushed;
          }

          /// @brief Push multiple events, wake consumer once at end.
          template<size_t N>
          uint32_t EmitBatch(const SystemEvent (&events)[N]) noexcept {
              uint32_t emitted = 0;
              for (auto& e : events) {
                  if (ring_.TryPush(e)) ++emitted;
                  else break;
              }
              if (emitted > 0) WakeConsumer();
              return emitted;
          }

          // =================================================================
          // Consumer API (client thread, typically inside a coroutine)
          // =================================================================

          /// @brief Non-blocking: try to pop one event.
          [[nodiscard]] std::optional<SystemEvent> TryReceive() noexcept {
              return ring_.TryPop();
          }

          /// @brief Awaitable: suspend until at least one event is available.
          [[nodiscard]] auto Next() noexcept { return NextAwaiter{*this}; }

          /// @brief Awaitable: suspend until events available, then drain all.
          [[nodiscard]] auto NextBatch() noexcept { return BatchAwaiter{*this}; }

          /// @brief Approximate unread count.
          [[nodiscard]] uint32_t PendingCount() const noexcept {
              return ring_.SizeApprox();
          }

          /// @brief Signal that no more events will be produced.
          void Close() noexcept {
              closed_.store(true, std::memory_order_release);
              WakeConsumer();
          }

          [[nodiscard]] bool IsClosed() const noexcept {
              return closed_.load(std::memory_order_acquire);
          }

      private:
          SpscQueue<SystemEvent, Capacity> ring_;
          std::atomic<std::coroutine_handle<>> waiter_{nullptr};
          std::atomic<bool> closed_{false};

          void WakeConsumer() noexcept {
              auto h = waiter_.exchange(nullptr, std::memory_order_acq_rel);
              if (h) h.resume();
          }

          // =================================================================
          // NextAwaiter — co_await channel.Next()
          // =================================================================

          struct NextAwaiter {
              EventChannel& channel;

              bool await_ready() noexcept {
                  // Fast path: data already available → don't suspend
                  return !channel.ring_.Empty() || channel.closed_.load(std::memory_order_acquire);
              }

              bool await_suspend(std::coroutine_handle<> h) noexcept {
                  // Register ourselves as the waiter
                  channel.waiter_.store(h, std::memory_order_release);

                  // Double-check: data might have arrived between await_ready and now
                  if (!channel.ring_.Empty() || channel.closed_.load(std::memory_order_acquire)) {
                      // Try to reclaim our handle (avoid spurious resume)
                      auto expected = h;
                      if (channel.waiter_.compare_exchange_strong(
                              expected, nullptr, std::memory_order_acq_rel)) {
                          return false; // don't suspend, data available
                      }
                      // Producer already took our handle and will resume us — stay suspended
                  }
                  return true; // suspend
              }

              std::optional<SystemEvent> await_resume() noexcept {
                  return channel.ring_.TryPop();
              }
          };

          // =================================================================
          // BatchAwaiter — co_await channel.NextBatch()
          // =================================================================

          struct BatchAwaiter {
              EventChannel& channel;
              // Drain buffer — on stack of the coroutine frame
              static constexpr uint32_t kMaxBatch = 64;

              bool await_ready() noexcept {
                  return !channel.ring_.Empty() || channel.closed_.load(std::memory_order_acquire);
              }

              bool await_suspend(std::coroutine_handle<> h) noexcept {
                  channel.waiter_.store(h, std::memory_order_release);
                  if (!channel.ring_.Empty() || channel.closed_.load(std::memory_order_acquire)) {
                      auto expected = h;
                      if (channel.waiter_.compare_exchange_strong(
                              expected, nullptr, std::memory_order_acq_rel)) {
                          return false;
                      }
                  }
                  return true;
              }

              /// @brief Returns a generator that yields all currently available events.
              Generator<const SystemEvent&> await_resume() noexcept {
                  // Drain everything currently in the ring
                  while (auto event = channel.ring_.TryPop()) {
                      co_yield *event;
                  }
              }
          };
      };

  } // namespace Mashiro::Platform

  关键设计点：

  1. 单 waiter 模型：atomic<coroutine_handle<>> 只存一个等待者。因为 channel 是 SPSC，consumer
  端只有一个线程，同一时刻最多一个 coroutine 等待。不需要 waiter list。
  2. Double-check 避免 lost wake：await_suspend 中注册 handle 后再检查一次 ring 是否为空。如果 producer
  在注册和检查之间推入了数据，consumer 不会永久挂起。
  3. BatchAwaiter 返回 Generator：利用你已有的 Generator<const SystemEvent&> 让 client 用 range-for
  遍历本批所有事件，延迟求值（lazy drain）。
  4. Close 语义：当 Platform thread 关闭时，Close() 唤醒 consumer，consumer 读到 std::nullopt 知道流结束。

  ---
  二、Client 端使用模型

  // 在 render thread 的 coroutine 中：
  Task<void> RenderThreadLoop(EventChannel<>& events) {
      while (!events.IsClosed()) {
          // 等待下一个事件（如果已有则立即返回）
          auto event = co_await events.Next();
          if (!event) break; // channel closed

          HandleEvent(*event);
      }
  }

  // 或者批量处理（更高效 — 一次 wake 处理所有积压事件）：
  Task<void> LogicThreadLoop(EventChannel<>& events) {
      while (!events.IsClosed()) {
          // 等待至少一个事件，然后一次取出所有
          for (const auto& event : co_await events.NextBatch()) {
              switch (event.kind) {
                  case EventKind::WindowResize:
                      OnResize(event.payload.resize);
                      break;
                  case EventKind::InputKeyDown:
                      OnKeyDown(event.payload.key);
                      break;
                  // ...
              }
          }
      }
  }

  // 非阻塞轮询（在已有 game loop 中使用）：
  void GameLoop(EventChannel<>& events) {
      // 每帧开头 drain 所有事件，不挂起
      while (auto event = events.TryReceive()) {
          ProcessEvent(*event);
      }
      // ... rest of frame ...
  }

  ---
  三、EventPump — Broadcast to All Channels

  // Platform/EventPump.h
  #pragma once
  #include "Mashiro/Platform/EventChannel.h"
  #include "Mashiro/Platform/SystemEvent.h"

  #include <cstdint>

  namespace Mashiro::Platform {

      class WindowManager;
      class InputManager;
      class ClipboardManager;

      class EventPump {
      public:
          /// @brief Attach a client channel. Platform thread broadcasts all events to it.
          void AttachChannel(EventChannel<>& channel) noexcept {
              if (channelCount_ < kMaxChannels) {
                  channels_[channelCount_++] = &channel;
              }
          }

          /// @brief Detach a client channel (e.g., when client thread shuts down).
          void DetachChannel(EventChannel<>& channel) noexcept;

          /// @brief Pump all pending Win32 messages, translate, broadcast, update managers.
          void PumpAll();

          /// @brief Whether there are queued Win32 messages remaining.
          bool HasPending() const noexcept;

          /// @brief Block until a Win32 message arrives or wakeEvent is signaled.
          void WaitForWork(void* wakeEvent) noexcept;

          /// @brief Provide manager references for bookkeeping updates.
          void BindManagers(WindowManager& wm, InputManager& im, ClipboardManager& cm) noexcept;

      private:
          static constexpr size_t kMaxChannels = 8;
          EventChannel<>* channels_[kMaxChannels]{};
          uint8_t channelCount_ = 0;

          WindowManager* windowMgr_ = nullptr;
          InputManager* inputMgr_ = nullptr;
          ClipboardManager* clipboardMgr_ = nullptr;

          uint32_t nextSequence_ = 0;

          /// @brief Broadcast one event to all attached channels.
          void Broadcast(const SystemEvent& event) noexcept {
              for (uint8_t i = 0; i < channelCount_; ++i) {
                  channels_[i]->Emit(event);
              }
          }

          /// @brief Translate a single Win32 message into 0+ SystemEvents.
          /// Also performs manager bookkeeping for state-affecting messages.
          void TranslateAndEmit(/* MSG& msg, platform-specific */);
      };

  } // namespace Mashiro::Platform

  ---
  四、SeqLock — Manager Any-Thread Query 的一致性读

  // Platform/SeqLock.h
  #pragma once
  #include "Mashiro/Core/TypeTraits.h"

  #include <atomic>
  #include <cstdint>
  #include <type_traits>

  namespace Mashiro::Platform {

      /// @brief Single-writer (platform thread) multi-reader SeqLock for composite values.
      /// Zero allocation, zero blocking on reader side (just retry on torn read).
      template<typename T>
          requires std::is_trivially_copyable_v<T>
      class SeqLock {
      public:
          void Write(const T& value) noexcept {
              uint32_t seq = seq_.load(std::memory_order_relaxed);
              seq_.store(seq + 1, std::memory_order_release); // odd = writing
              data_ = value;
              seq_.store(seq + 2, std::memory_order_release); // even = stable
          }

          [[nodiscard]] T Read() const noexcept {
              T copy;
              uint32_t seq0, seq1;
              do {
                  seq0 = seq_.load(std::memory_order_acquire);
                  copy = data_;
                  std::atomic_thread_fence(std::memory_order_acquire);
                  seq1 = seq_.load(std::memory_order_relaxed);
              } while (seq0 != seq1 || (seq0 & 1));
              return copy;
          }

      private:
          alignas(Platform::kCacheLineSize) std::atomic<uint32_t> seq_{0};
          T data_{};
      };

  } // namespace Mashiro::Platform

  Manager 中使用：

  class WindowManager {
      // ...
      [[=kAnyThread]]
      Window::WindowDesc GetDesc(WindowHandle window) const noexcept {
          return descLocks_[window.id].Read();
      }

  private:
      // Platform thread 在 bookkeeping 时 Write()
      void UpdateDesc(WindowHandle window, const Window::WindowDesc& desc) noexcept {
          descLocks_[window.id].Write(desc);
      }

      // Per-window SeqLock for consistent cross-thread reads
      SeqLock<Window::WindowDesc> descLocks_[kMaxWindows];
  };

  ---
  五、OwnerExecutor — MPSC Coroutine Mailbox

  // Platform/OwnerExecutor.h
  #pragma once
  #include "Mashiro/Core/TypeTraits.h"

  #include <atomic>
  #include <coroutine>
  #include <cstdint>

  namespace Mashiro::Platform {

      /// @brief MPSC lock-free queue for coroutine handles.
      /// Multiple worker threads submit; platform thread drains.
      class OwnerExecutor {
      public:
          void Initialize(uint32_t ownerThreadId, void* wakeEvent) noexcept;

          /// @brief Submit a coroutine handle for execution on platform thread.
          /// Called from any thread. Lock-free.
          void Enqueue(std::coroutine_handle<> h) noexcept;

          /// @brief Resume all pending coroutines. Called only from platform thread.
          void DrainAll() noexcept;

          /// @brief Are there pending coroutines to drain?
          bool HasPending() const noexcept;

          /// @brief Is the calling thread the platform thread?
          bool IsOnPlatformThread() const noexcept;

          /// @brief Singleton access (one platform thread per process).
          static OwnerExecutor& Instance() noexcept;

      private:
          // Intrusive MPSC stack (Treiber stack — LIFO, but order doesn't matter for coroutines)
          struct Node {
              std::coroutine_handle<> handle;
              Node* next = nullptr;
          };

          alignas(64) std::atomic<Node*> head_{nullptr};
          uint32_t ownerThreadId_ = 0;
          void* wakeEvent_ = nullptr;

          // Node pool (avoid heap allocation per submission)
          static constexpr size_t kPoolSize = 256;
          Node pool_[kPoolSize];
          alignas(64) std::atomic<uint32_t> poolHead_{0};

          Node* AllocNode() noexcept;
          void FreeNode(Node* n) noexcept;
          void WakeOwnerThread() noexcept;
      };

  } // namespace Mashiro::Platform

  ---
  六、OwnerTask<T> — 最终版

  // Platform/OwnerTask.h
  #pragma once
  #include "Mashiro/Platform/OwnerExecutor.h"

  #include <coroutine>
  #include <exception>
  #include <type_traits>
  #include <utility>

  namespace Mashiro::Platform {

      // =========================================================================
      // TransferToOwner awaiter
      // =========================================================================

      struct TransferToOwner {
          bool await_ready() noexcept {
              return OwnerExecutor::Instance().IsOnPlatformThread();
          }

          void await_suspend(std::coroutine_handle<> h) noexcept {
              OwnerExecutor::Instance().Enqueue(h);
          }

          void await_resume() noexcept {}
      };

      // =========================================================================
      // OwnerTask<T>
      // =========================================================================

      template<typename T = void>
      class [[nodiscard]] OwnerTask {
      public:
          struct Promise {
              using value_type = std::conditional_t<std::is_void_v<T>, char, T>;

              union { value_type result_; };
              std::exception_ptr exception_;
              std::coroutine_handle<> continuation_{std::noop_coroutine()};

              OwnerTask get_return_object() noexcept {
                  return OwnerTask{std::coroutine_handle<Promise>::from_promise(*this)};
              }

              // initial_suspend: transfer to platform thread if not already there
              TransferToOwner initial_suspend() noexcept { return {}; }

              auto final_suspend() noexcept {
                  struct Completion {
                      bool await_ready() noexcept { return false; }
                      std::coroutine_handle<> await_suspend(
                          std::coroutine_handle<Promise> h) noexcept {
                          return h.promise().continuation_;
                      }
                      void await_resume() noexcept {}
                  };
                  return Completion{};
              }

              template<typename U>
                  requires (!std::is_void_v<T> && std::convertible_to<U, T>)
              void return_value(U&& val) noexcept(std::is_nothrow_constructible_v<T, U&&>) {
                  std::construct_at(&result_, std::forward<U>(val));
              }

              void return_void() noexcept requires std::is_void_v<T> {}

              void unhandled_exception() noexcept {
                  exception_ = std::current_exception();
              }

              Promise() noexcept {}
              ~Promise() {
                  if constexpr (!std::is_void_v<T>) {
                      if (!exception_) std::destroy_at(&result_);
                  }
              }
          };

          using promise_type = Promise;
          using handle_type = std::coroutine_handle<Promise>;

          // Awaitable interface: co_await an OwnerTask from any coroutine
          bool await_ready() const noexcept { return handle_.done(); }

          std::coroutine_handle<> await_suspend(std::coroutine_handle<> caller) noexcept {
              handle_.promise().continuation_ = caller;
              return handle_;
          }

          T await_resume() requires (!std::is_void_v<T>) {
              auto& p = handle_.promise();
              if (p.exception_) std::rethrow_exception(p.exception_);
              return std::move(p.result_);
          }

          void await_resume() requires std::is_void_v<T> {
              if (handle_.promise().exception_)
                  std::rethrow_exception(handle_.promise().exception_);
          }

          ~OwnerTask() { if (handle_) handle_.destroy(); }
          OwnerTask(OwnerTask&& o) noexcept : handle_(std::exchange(o.handle_, nullptr)) {}
          OwnerTask(const OwnerTask&) = delete;
          OwnerTask& operator=(const OwnerTask&) = delete;
          OwnerTask& operator=(OwnerTask&&) = delete;

      private:
          handle_type handle_;
          explicit OwnerTask(handle_type h) noexcept : handle_(h) {}
      };

  } // namespace Mashiro::Platform

  ---
  七、ThreadContract + ManagerTraits — P3394/P2996 Consteval 验证

  // Platform/ThreadContract.h
  #pragma once
  #include <cstdint>

  namespace Mashiro::Platform {

      enum class ThreadDomain : uint8_t {
          Platform,  // 必须在 platform thread 执行
          Any,       // 任意线程可执行
      };

      struct ThreadContract {
          ThreadDomain domain = ThreadDomain::Platform;
          constexpr bool operator==(const ThreadContract&) const = default;
      };

      inline constexpr ThreadContract kPlatformOnly{.domain = ThreadDomain::Platform};
      inline constexpr ThreadContract kAnyThread{.domain = ThreadDomain::Any};

  } // namespace Mashiro::Platform

  // Platform/ManagerTraits.h
  #pragma once
  #include "Mashiro/Platform/ThreadContract.h"
  #include "Mashiro/Platform/OwnerTask.h"
  #include <meta>
  #include <string_view>

  namespace Mashiro::Platform::Detail {

      // =========================================================================
      // Consteval contract verification
      // =========================================================================

      consteval bool IsOwnerTaskSpecialization(std::meta::info type) {
          // Check if type is OwnerTask<T> for some T
          return std::meta::has_template_arguments(type) &&
                 std::meta::template_of(type) == ^^OwnerTask;
      }

      template<typename Manager>
      consteval void VerifyManagerContracts() {
          constexpr auto members = std::define_static_array(
              std::meta::members_of(^^Manager, std::meta::access_context::unchecked()));

          template for (constexpr auto m : members) {
              if constexpr (std::meta::is_function(m) && std::meta::is_public(m)) {
                  constexpr auto contracts = std::define_static_array(
                      std::meta::annotations_of(m, ^^ThreadContract));

                  if constexpr (contracts.size() > 0) {
                      constexpr auto contract = std::meta::extract<ThreadContract>(contracts[0]);
                      constexpr auto ret = std::meta::return_type_of(m);

                      // Rule 1: Platform-domain → must return OwnerTask<T>
                      if constexpr (contract.domain == ThreadDomain::Platform) {
                          static_assert(IsOwnerTaskSpecialization(ret),
                              "Platform-thread method must return OwnerTask<T>");
                      }

                      // Rule 2: Any-domain → must NOT return OwnerTask<T>
                      if constexpr (contract.domain == ThreadDomain::Any) {
                          static_assert(!IsOwnerTaskSpecialization(ret),
                              "Any-thread method must not return OwnerTask<T>");
                      }
                  }
              }
          }
      }

      // =========================================================================
      // Consteval: generate event-kind-to-bookkeeping dispatch for EventPump
      // =========================================================================

      /// @brief Annotation marking a Manager private method as bookkeeping handler
      /// for a specific EventKind. EventPump calls these during translation.
      struct BookkeepFor {
          EventKind kind;
          constexpr bool operator==(const BookkeepFor&) const = default;
      };

      /// @brief Build compile-time table: which Manager methods handle which EventKinds
      template<typename Manager>
      consteval auto BuildBookkeepTable() {
          struct Entry {
              EventKind kind;
              std::meta::info method;
          };
          std::vector<Entry> table;

          for (auto m : std::meta::members_of(^^Manager,
                   std::meta::access_context::unchecked())) {
              if (!std::meta::is_function(m)) continue;
              auto annots = std::meta::annotations_of(m, ^^BookkeepFor);
              for (auto a : annots) {
                  auto bk = std::meta::extract<BookkeepFor>(a);
                  table.push_back({bk.kind, m});
              }
          }
          return table;
      }

  } // namespace Mashiro::Platform::Detail

  ---
  八、WindowManager — 使用 BookkeepFor Annotation

  // Platform/Managers/WindowManager.h
  #pragma once
  #include "Mashiro/Platform/OwnerTask.h"
  #include "Mashiro/Platform/SeqLock.h"
  #include "Mashiro/Platform/SystemEvent.h"
  #include "Mashiro/Platform/ThreadContract.h"
  #include "Mashiro/Platform/ManagerTraits.h"
  #include "Mashiro/Core/ChunkedSlotMap.h"
  #include "Mashiro/Surface/Window.h"

  #include <meta>
  #include <string>

  namespace Mashiro::Platform {

      struct WindowHandle {
          uint32_t id = 0;
          explicit operator bool() const noexcept { return id != 0; }
          constexpr bool operator==(const WindowHandle&) const = default;
      };

      struct NativeWindowView {
          void* hwnd = nullptr;
      };

      class WindowManager {
      public:
          // ---- Platform-thread mutation APIs ----

          [[=kPlatformOnly]]
          OwnerTask<WindowHandle> Create(Window::WindowDesc desc);

          [[=kPlatformOnly]]
          OwnerTask<void> Destroy(WindowHandle window);

          [[=kPlatformOnly]]
          OwnerTask<void> SetTitle(WindowHandle window, std::string title);

          [[=kPlatformOnly]]
          OwnerTask<void> SetSize(WindowHandle window, Window::Size size);

          [[=kPlatformOnly]]
          OwnerTask<void> SetMode(WindowHandle window, Window::Mode mode);

          [[=kPlatformOnly]]
          OwnerTask<void> Show(WindowHandle window);

          [[=kPlatformOnly]]
          OwnerTask<void> Hide(WindowHandle window);

          // ---- Any-thread read APIs (SeqLock protected) ----

          [[=kAnyThread]]
          Window::WindowDesc GetDesc(WindowHandle window) const noexcept;

          [[=kAnyThread]]
          Window::Size GetSize(WindowHandle window) const noexcept;

          [[=kAnyThread]]
          NativeWindowView GetNativeView(WindowHandle window) const noexcept;

          [[=kAnyThread]]
          bool IsValid(WindowHandle window) const noexcept;

      private:
          friend class EventPump;

          // ---- Bookkeeping (called by EventPump on platform thread during pump) ----

          [[=Detail::BookkeepFor{EventKind::WindowResize}]]
          void OnPump_WindowResize(const SystemEvent& event) noexcept;

          [[=Detail::BookkeepFor{EventKind::WindowClose}]]
          void OnPump_WindowClose(const SystemEvent& event) noexcept;

          [[=Detail::BookkeepFor{EventKind::WindowMove}]]
          void OnPump_WindowMove(const SystemEvent& event) noexcept;

          [[=Detail::BookkeepFor{EventKind::WindowDpiChange}]]
          void OnPump_WindowDpiChange(const SystemEvent& event) noexcept;

          [[=Detail::BookkeepFor{EventKind::WindowFocus}]]
          void OnPump_WindowFocus(const SystemEvent& event) noexcept;

          // ---- State storage ----

          struct WindowState {
              void* nativeHandle = nullptr;
              bool alive = true;
          };

          static constexpr size_t kMaxWindows = 64;
          WindowState states_[kMaxWindows]{};
          SeqLock<Window::WindowDesc> descs_[kMaxWindows];
          uint32_t count_ = 0;
      };

      // Compile-time verification
      consteval {
          Detail::VerifyManagerContracts<WindowManager>();
      }

  } // namespace Mashiro::Platform

  ---
  九、EventPump — 使用 Consteval-Generated Bookkeep Dispatch

  // Platform/EventPump.h (核心 broadcast + bookkeeping 逻辑)
  #pragma once
  #include "Mashiro/Platform/EventChannel.h"
  #include "Mashiro/Platform/ManagerTraits.h"
  #include "Mashiro/Platform/SystemEvent.h"

  namespace Mashiro::Platform {

      class WindowManager;
      class InputManager;
      class ClipboardManager;

      class EventPump {
      public:
          void AttachChannel(EventChannel<>& channel) noexcept;
          void DetachChannel(EventChannel<>& channel) noexcept;
          void PumpAll();
          bool HasPending() const noexcept;
          void WaitForWork(void* wakeEvent) noexcept;

          void BindManagers(WindowManager& wm, InputManager& im,
                            ClipboardManager& cm) noexcept;

      private:
          static constexpr size_t kMaxChannels = 8;
          EventChannel<>* channels_[kMaxChannels]{};
          uint8_t channelCount_ = 0;

          WindowManager* windowMgr_ = nullptr;
          InputManager* inputMgr_ = nullptr;
          ClipboardManager* clipboardMgr_ = nullptr;

          uint32_t nextSequence_ = 0;

          void Broadcast(const SystemEvent& event) noexcept;

          /// @brief Consteval-generated bookkeep dispatch.
          /// For each Manager, iterate its BookkeepFor entries and call the method.
          template<typename Manager>
          void DispatchBookkeep(Manager& mgr, const SystemEvent& event) noexcept {
              constexpr auto table = std::define_static_array(
                  Detail::BuildBookkeepTable<Manager>());

              template for (constexpr auto entry : table) {
                  if (event.kind == entry.kind) {
                      (mgr.[:entry.method:])(event);
                      return;
                  }
              }
          }

          void TranslateAndEmit(/* platform-specific MSG */);
      };

  } // namespace Mashiro::Platform

  TranslateAndEmit 的实现逻辑（伪代码）：

  void EventPump::TranslateAndEmit(MSG& msg) {
      // 翻译 Win32 message 为 SystemEvent
      std::optional<SystemEvent> event = TranslateWin32(msg);
      if (!event) return;

      event->sequence = nextSequence_++;
      event->timestamp = QueryPerformanceCounter();

      // 1. Bookkeep: 更新 manager 内部状态（同一线程，直接调用）
      DispatchBookkeep(*windowMgr_, *event);
      DispatchBookkeep(*inputMgr_, *event);
      DispatchBookkeep(*clipboardMgr_, *event);

      // 2. Broadcast: 推入所有 client channels（可能唤醒等待中的 client coroutines）
      Broadcast(*event);
  }

  关键：bookkeep 先于 broadcast。当 client 从 channel 读到 event 时，Manager 的状态已经更新。GetSize() 返回的值与 event
  一致。

  ---
  十、PlatformThread — 极简最终版

  // Platform/PlatformThread.h
  #pragma once
  #include "Mashiro/Platform/EventPump.h"
  #include "Mashiro/Platform/OwnerExecutor.h"
  #include "Mashiro/Platform/Managers/WindowManager.h"
  #include "Mashiro/Platform/Managers/InputManager.h"
  #include "Mashiro/Platform/Managers/ClipboardManager.h"

  namespace Mashiro::Platform {

      class PlatformThread {
      public:
          void Run();
          void RequestStop() noexcept { running_ = false; }

          // Manager access
          WindowManager& Windows() noexcept { return windowMgr_; }
          InputManager& Input() noexcept { return inputMgr_; }
          ClipboardManager& Clipboard() noexcept { return clipboardMgr_; }

          // Channel management
          void AttachChannel(EventChannel<>& channel) noexcept {
              pump_.AttachChannel(channel);
          }

          OwnerExecutor& Executor() noexcept { return executor_; }

      private:
          EventPump pump_;
          OwnerExecutor executor_;

          WindowManager windowMgr_;
          InputManager inputMgr_;
          ClipboardManager clipboardMgr_;

          bool running_ = true;
      };

  } // namespace Mashiro::Platform

  实现：

  void PlatformThread::Run() {
      SetCurrentThreadName("Platform");

      void* wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
      executor_.Initialize(GetCurrentThreadId(), wakeEvent);
      pump_.BindManagers(windowMgr_, inputMgr_, clipboardMgr_);

      while (running_) {
          pump_.PumpAll();
          executor_.DrainAll();

          if (!pump_.HasPending() && !executor_.HasPending()) {
              pump_.WaitForWork(wakeEvent);
          }
      }

      // Shutdown: close all channels so client coroutines wake and exit
      // (handled by EventPump broadcasting close)
  }

  ---
  十一、完整 Client 端示例

  #include "Mashiro/Platform/PlatformThread.h"
  #include "Mashiro/Platform/EventChannel.h"

  // Client-owned channel
  EventChannel<> renderEvents;
  EventChannel<> uiEvents;

  // Setup
  platform.AttachChannel(renderEvents);
  platform.AttachChannel(uiEvents);

  // ---- Render thread (coroutine) ----
  Task<void> RenderLoop() {
      // Create a window (executes on platform thread, returns here)
      auto window = co_await platform.Windows().Create({
          .title = "Mashiro Engine",
          .size = {1920, 1080},
          .flags = Window::Flags::Resizable | Window::Flags::HighDpi,
      });

      while (!renderEvents.IsClosed()) {
          // Wait for events, process batch
          for (const auto& event : co_await renderEvents.NextBatch()) {
              if (event.kind == EventKind::WindowResize) {
                  RecreateSwapchain(event.payload.resize.width,
                                    event.payload.resize.height);
              }
          }

          // Query current size (any-thread, SeqLock, always consistent)
          auto currentSize = platform.Windows().GetSize(window);
          RenderFrame(currentSize);
      }

      co_await platform.Windows().Destroy(window);
  }

  // ---- UI thread (coroutine) ----
  Task<void> UILoop() {
      while (!uiEvents.IsClosed()) {
          auto event = co_await uiEvents.Next();
          if (!event) break;

          if (event->kind == EventKind::InputKeyDown) {
              HandleHotkey(event->payload.key);
          }
      }
  }

  // ---- Game loop (polling style, no coroutine needed) ----
  void GameTick() {
      while (auto event = gameEvents.TryReceive()) {
          gameWorld.HandleInput(*event);
      }
      gameWorld.Simulate(dt);
  }

  ---
  十二、性能特性（最终版）

  ┌─────────────────────────────────────────┬───────────────────────────────────────────────────────────────────────┐
  │                  路径                   │                                 成本                                  │
  ├─────────────────────────────────────────┼───────────────────────────────────────────────────────────────────────┤
  │ co_await channel.Next() — 已有数据      │ 0 次挂起，1 次 TryPop（一次 atomic load）                             │
  ├─────────────────────────────────────────┼───────────────────────────────────────────────────────────────────────┤
  │ co_await channel.Next() —               │ 1 次 atomic store（注册）+ 1 次 atomic exchange（唤醒）+ 1 次         │
  │ 无数据，跨线程唤醒                      │ coroutine resume                                                      │
  ├─────────────────────────────────────────┼───────────────────────────────────────────────────────────────────────┤
  │ Emit() — 无 waiter                      │ 1 次 TryPush + 1 次 atomic load（检查 waiter = null）                 │
  ├─────────────────────────────────────────┼───────────────────────────────────────────────────────────────────────┤
  │ Emit() — 有 waiter                      │ 1 次 TryPush + 1 次 atomic exchange + coroutine resume                │
  ├─────────────────────────────────────────┼───────────────────────────────────────────────────────────────────────┤
  │ OwnerTask 已在 platform thread          │ 0 次原子操作，直接执行                                                │
  ├─────────────────────────────────────────┼───────────────────────────────────────────────────────────────────────┤
  │ OwnerTask 跨线程                        │ 1 次 MPSC push + 1 次 SetEvent + 1 次 resume                          │
  ├─────────────────────────────────────────┼───────────────────────────────────────────────────────────────────────┤
  │ GetDesc() any-thread                    │ SeqLock read（1-2 次 atomic load，无锁）                              │
  ├─────────────────────────────────────────┼───────────────────────────────────────────────────────────────────────┤
  │ Bookkeep dispatch                       │ consteval-generated switch（template for 展开）                       │
  ├─────────────────────────────────────────┼───────────────────────────────────────────────────────────────────────┤
  │ Contract verification                   │ 编译期，零运行时                                                      │
  └─────────────────────────────────────────┴───────────────────────────────────────────────────────────────────────┘

  ---
  十三、C++26 特性使用清单（最终版）

  ┌────────────────────────────────┬──────────────────────────────────────────┬─────────────────────────────────────┐
  │              特性              │                   位置                   │           不可替代的价值            │
  ├────────────────────────────────┼──────────────────────────────────────────┼─────────────────────────────────────┤
  │ P3394 [[=kPlatformOnly]]       │ Manager 方法                             │ 零 boilerplate 声明线程合约         │
  ├────────────────────────────────┼──────────────────────────────────────────┼─────────────────────────────────────┤
  │ P3394 [[=BookkeepFor{...}]]    │ Manager private 方法                     │ 声明式事件→bookkeep 路由，无需手写  │
  │                                │                                          │ switch                              │
  ├────────────────────────────────┼──────────────────────────────────────────┼─────────────────────────────────────┤
  │ P2996 annotations_of           │ ManagerTraits consteval 验证             │ 编译期自动检查所有 Manager          │
  │                                │                                          │ 合约正确性                          │
  ├────────────────────────────────┼──────────────────────────────────────────┼─────────────────────────────────────┤
  │ P2996 members_of +             │ 合约验证                                 │ 确保 Platform 方法 → OwnerTask，Any │
  │ return_type_of                 │                                          │  方法 → 非 OwnerTask                │
  ├────────────────────────────────┼──────────────────────────────────────────┼─────────────────────────────────────┤
  │ P3289 consteval {}             │ Manager 定义后                           │ 错误在最早编译期暴露                │
  ├────────────────────────────────┼──────────────────────────────────────────┼─────────────────────────────────────┤
  │ P1306 template for             │ DispatchBookkeep、VerifyManagerContracts │ 静态展开，零虚调用，等价手写 switch │
  ├────────────────────────────────┼──────────────────────────────────────────┼─────────────────────────────────────┤
  │ P3491 define_static_array      │ BookkeepTable、members 列表              │ consteval 数据提升到 static storage │
  │                                │                                          │  供 template for                    │
  ├────────────────────────────────┼──────────────────────────────────────────┼─────────────────────────────────────┤
  │ Coroutines                     │ OwnerTask、EventChannel awaiters         │ 自然的异步语义，无回调              │
  ├────────────────────────────────┼──────────────────────────────────────────┼─────────────────────────────────────┤
  │ SpscQueue（已有）              │ EventChannel 底层                        │ 零 allocation、cache-line separated │
  ├────────────────────────────────┼──────────────────────────────────────────┼─────────────────────────────────────┤
  │ Generator（已有）              │ BatchAwaiter::await_resume               │ 懒 drain，range-for 友好            │
  ├────────────────────────────────┼──────────────────────────────────────────┼─────────────────────────────────────┤
  │ Traits::SequentialEnum（已有） │ EventKind                                │ 保证可做数组索引                    │
  ├────────────────────────────────┼──────────────────────────────────────────┼─────────────────────────────────────┤
  │ Traits::BitfieldEnum（已有）   │ Window::Flags                            │ 自动 bitwise operators              │
  ├────────────────────────────────┼──────────────────────────────────────────┼─────────────────────────────────────┤
  │ InlineFunction（已有）         │ 如需少量回调                             │ 零 heap                             │
  └────────────────────────────────┴──────────────────────────────────────────┴─────────────────────────────────────┘

  ---
  十四、回答之前的开放问题（已确认）

  ┌───────────────────────────┬────────────────────────────────────────────────────────────────────────────────────┐
  │           问题            │                                        决策                                        │
  ├───────────────────────────┼────────────────────────────────────────────────────────────────────────────────────┤
  │ Broadcast vs per-consumer │ Broadcast SPSC：每个 client 独立 channel                                           │
  ├───────────────────────────┼────────────────────────────────────────────────────────────────────────────────────┤
  │ Any-thread 一致性         │ SeqLock for composite（WindowDesc）、atomic for scalar（Size 可以拆为两个 atomic） │
  ├───────────────────────────┼────────────────────────────────────────────────────────────────────────────────────┤
  │ Bookkeeping 时机          │ Pump 内部，先于 broadcast，保证一致性                                              │
  ├───────────────────────────┼────────────────────────────────────────────────────────────────────────────────────┤
  │ Client 等待模型           │ co_await channel.Next() / co_await channel.NextBatch() / TryReceive()              │
  ├───────────────────────────┼────────────────────────────────────────────────────────────────────────────────────┤
  │ Close 语义                │ Close() 唤醒 consumer，返回 nullopt                                                │
  └───────────────────────────┴────────────────────────────────────────────────────────────────────────────────────┘

  ---
  进入 spec 文档编写阶段？还是需要对某个具体组件进一步细化？
  ⎿  Stop says:
     ---
     ✅ Session ended

     📁 Git repository
       Branch: main
       Changes:
         增加: 3 files
         修改: 4 files

     ✅ No temp files
     ---

✻ Sautéed for 3m 19s

❯ 全面了解Windows和Linux中无法使用STL做的系统功能，调查Manager的任务分配和调度

● Agent(Research OS-specific platform tasks)
  ⎿  Done (30 tool uses · 53.4k tokens · 9m 4s)
  (ctrl+o to expand)

● 非常好。调研完成。现在基于这份完整的 OS 能力清单，来做 Manager 职责划分和调度设计。

  ---
  Manager 职责划分与调度设计

  基于调研结果，将 OS 平台功能按线程亲和性和功能内聚性划分为以下 Manager：

  ---
  分类原则

  1. Thread Affinity 是硬约束 — MUST-on-window-thread 的操作只能由 Platform thread 执行
  2. 功能内聚 — 操作同一种 OS 资源的 API 归入同一 Manager
  3. 不冗余 — 如果一组 API 只有 1-2 个操作，不单独建 Manager，归入最近的相关 Manager
  4. 调度模式 — 按 thread affinity 决定 Manager 生存的线程

  ---
  Manager 拓扑

  Platform Thread (Window-Owning Thread)
  ├── WindowManager          — 窗口生命周期、状态、DPI
  ├── InputManager           — 键盘、鼠标、触摸、笔、raw input
  ├── ImeManager             — IME 组合、候选词、TSF
  ├── ClipboardManager       — 剪贴板读写、格式协商
  ├── CursorManager          — 光标形状、约束、锁定
  ├── DragDropManager        — OLE DnD / Xdnd / wl_data_device
  ├── DialogManager          — 系统对话框（文件、颜色、消息框）
  ├── SurfaceManager         — Vulkan WSI surface 创建/销毁
  ├── SystemAppearanceManager— 主题、暗色模式、高对比度、强调色
  └── AccessibilityManager   — UI Automation / AT-SPI

  Dedicated I/O Thread (optional, spawned by Platform)
  ├── FileWatchManager       — 目录变更监控
  └── GamepadManager         — XInput / evdev 轮询

  Any-Thread (free-threaded singletons)
  ├── DisplayManager         — 显示器枚举、模式、ICC
  ├── PowerManager           — 电源状态、防休眠
  ├── AudioDeviceManager     — 音频设备枚举
  └── TimingManager          — 高精度计时、timer resolution

  ---
  各 Manager 详细职责

  A. Platform Thread Managers（MUST 在 platform thread 执行）

  Manager: WindowManager
  职责: 窗口创建/销毁/移动/大小/模式/装饰/DPI
  Key APIs: CreateWindowExW, SetWindowPos, DPI APIs
  事件产出: WindowResize, WindowClose, WindowMove, WindowDpiChange, WindowFocus
  ────────────────────────────────────────
  Manager: InputManager
  职责: 键盘（cooked + raw）、鼠标（move + button + wheel）、触摸、笔
  Key APIs: WM_KEY*, WM_MOUSE*, WM_INPUT, WM_TOUCH, WM_POINTER
  事件产出: InputKeyDown/Up, InputChar, InputMouseMove/Button, InputScroll, InputTouch, InputPen
  ────────────────────────────────────────
  Manager: ImeManager
  职责: IME 启用/禁用、组合字符串、候选词窗口位置
  Key APIs: ImmGet/SetCompositionString, TSF interfaces
  事件产出: ImeComposition, ImeCommit, ImeCandidateList
  ────────────────────────────────────────
  Manager: ClipboardManager
  职责: 剪贴板文本/富格式读写、变更通知
  Key APIs: OpenClipboard, Get/SetClipboardData, AddClipboardFormatListener
  事件产出: ClipboardUpdate
  ────────────────────────────────────────
  Manager: CursorManager
  职责: 光标形状、隐藏、约束、锁定（FPS模式）
  Key APIs: SetCursor, ClipCursor, raw input delta
  事件产出: (不产出事件，只接受调用)
  ────────────────────────────────────────
  Manager: DragDropManager
  职责: 注册 drop target、接收/发起拖拽、格式协商
  Key APIs: OLE RegisterDragDrop, IDropTarget
  事件产出: DragEnter, DragOver, DragDrop, DragLeave
  ────────────────────────────────────────
  Manager: DialogManager
  职责: 文件打开/保存、文件夹选择、颜色选择、消息框
  Key APIs: IFileOpenDialog, ChooseColorW, MessageBoxW
  事件产出: (不产出事件，只返回 OwnerTask 结果)
  ────────────────────────────────────────
  Manager: SurfaceManager
  职责: Vulkan surface 创建/销毁（需要 HWND）
  Key APIs: vkCreateWin32SurfaceKHR
  事件产出: (不产出事件)
  ────────────────────────────────────────
  Manager: SystemAppearanceManager
  职责: 暗色模式查询、主题变更通知、强调色
  Key APIs: WM_SETTINGCHANGE, Registry, DwmSetWindowAttribute
  事件产出: ThemeChanged, AccentColorChanged
  ────────────────────────────────────────
  Manager: AccessibilityManager
  职责: UI Automation provider、screen reader 交互
  Key APIs: WM_GETOBJECT, IRawElementProviderSimple
  事件产出: (不产出事件，响应外部查询)

  B. Dedicated Thread Managers（SHOULD 在独立线程）

  ┌──────────────────┬────────────────────────┬───────────────────────────────────┬─────────────────────────────────┐
  │     Manager      │          职责          │             Key APIs              │            调度模型             │
  ├──────────────────┼────────────────────────┼───────────────────────────────────┼─────────────────────────────────┤
  │ FileWatchManager │ 目录变更监控           │ ReadDirectoryChangesW + IOCP /    │ 独立 I/O 线程，结果通过         │
  │                  │                        │ inotify + epoll                   │ EventChannel 推送               │
  ├──────────────────┼────────────────────────┼───────────────────────────────────┼─────────────────────────────────┤
  │ GamepadManager   │ 手柄输入轮询、振动反馈 │ XInput XInputGetState / evdev     │ 独立轮询线程（~4ms              │
  │                  │                        │                                   │ 周期），结果进 EventChannel     │
  └──────────────────┴────────────────────────┴───────────────────────────────────┴─────────────────────────────────┘

  C. Free-Threaded Managers（CAN 在任意线程调用）

  Manager: DisplayManager
  职责: 显示器枚举、分辨率/刷新率、ICC profile、HDR
  Key APIs: EnumDisplayMonitors, GetMonitorInfoW, DXGI_OUTPUT_DESC1
  调度模型: 直接调用（结果缓存 + SeqLock 保护）。热插拔通知通过 platform thread 的 WM_DISPLAYCHANGE bookkeep
  ────────────────────────────────────────
  Manager: PowerManager
  职责: 防休眠、电池状态、电源事件
  Key APIs: SetThreadExecutionState, GetSystemPowerStatus
  调度模型: 直接调用。电源事件通过 WM_POWERBROADCAST 进 EventChannel
  ────────────────────────────────────────
  Manager: AudioDeviceManager
  职责: 音频设备枚举、默认设备、热插拔
  Key APIs: WASAPI IMMDeviceEnumerator
  调度模型: 直接调用 + 回调转发到 EventChannel
  ────────────────────────────────────────
  Manager: TimingManager
  职责: QPC、timer resolution、waitable timers
  Key APIs: QueryPerformanceCounter, timeBeginPeriod
  调度模型: 直接调用，无状态

  ---
  调度模式对应的类型系统

  利用 P3394 annotation 标注每个 Manager 的线程域和调度模式：

  // Platform/ManagerScheduling.h
  #pragma once
  #include "Mashiro/Platform/ThreadContract.h"
  #include <cstdint>

  namespace Mashiro::Platform {

      enum class ScheduleMode : uint8_t {
          PlatformThread,    // 在 platform thread 执行，通过 OwnerTask 调度
          DedicatedThread,   // 拥有自己的 I/O 线程，通过 EventChannel 输出
          FreeThreaded,      // 任意线程直接调用，无迁移
      };

      struct ManagerSchedule {
          ScheduleMode mode;
          constexpr bool operator==(const ManagerSchedule&) const = default;
      };

      inline constexpr ManagerSchedule kOnPlatformThread{.mode = ScheduleMode::PlatformThread};
      inline constexpr ManagerSchedule kOnDedicatedThread{.mode = ScheduleMode::DedicatedThread};
      inline constexpr ManagerSchedule kFreeThreaded{.mode = ScheduleMode::FreeThreaded};
  }

  Manager 类上标注：

  class [[=kOnPlatformThread]] WindowManager { ... };
  class [[=kOnPlatformThread]] InputManager { ... };
  class [[=kOnPlatformThread]] ImeManager { ... };
  class [[=kOnPlatformThread]] ClipboardManager { ... };
  class [[=kOnPlatformThread]] CursorManager { ... };
  class [[=kOnPlatformThread]] DragDropManager { ... };
  class [[=kOnPlatformThread]] DialogManager { ... };
  class [[=kOnPlatformThread]] SurfaceManager { ... };
  class [[=kOnPlatformThread]] SystemAppearanceManager { ... };
  class [[=kOnPlatformThread]] AccessibilityManager { ... };

  class [[=kOnDedicatedThread]] FileWatchManager { ... };
  class [[=kOnDedicatedThread]] GamepadManager { ... };

  class [[=kFreeThreaded]] DisplayManager { ... };
  class [[=kFreeThreaded]] PowerManager { ... };
  class [[=kFreeThreaded]] AudioDeviceManager { ... };
  class [[=kFreeThreaded]] TimingManager { ... };

  Consteval 验证利用 P2996 反射在 consteval {} 中自动检查：

  consteval {
      // 遍历所有 Manager 类，验证：
      // - PlatformThread managers: 所有 mutation API → OwnerTask<T>
      // - FreeThreaded managers: 所有 API → 直接返回值（不返回 OwnerTask）
      // - DedicatedThread managers: 不暴露 OwnerTask API，只通过 EventChannel 输出
      VerifySchedulingContracts<
          WindowManager, InputManager, ImeManager, ClipboardManager,
          CursorManager, DragDropManager, DialogManager, SurfaceManager,
          SystemAppearanceManager, AccessibilityManager,
          FileWatchManager, GamepadManager,
          DisplayManager, PowerManager, AudioDeviceManager, TimingManager
      >();
  }

  ---
  Dedicated Thread Managers 的调度模型

  FileWatchManager 和 GamepadManager 不在 platform thread 上，它们有自己的线程和 event 输出：

  class [[=kOnDedicatedThread]] GamepadManager {
  public:
      // 启动/停止独立轮询线程
      void Start(EventChannel<>& outputChannel);
      void Stop();

      // Any-thread queries
      [[=kAnyThread]]
      bool IsConnected(uint8_t padIndex) const noexcept;

      [[=kAnyThread]]
      GamepadState GetState(uint8_t padIndex) const noexcept;

      // Vibration 可从任意线程调用（XInput 是 free-threaded）
      [[=kAnyThread]]
      void SetVibration(uint8_t padIndex, float left, float right) noexcept;

  private:
      std::jthread pollThread_;
      EventChannel<>* output_ = nullptr;
      SeqLock<GamepadState> states_[4]; // 最多 4 手柄
  };

  class [[=kOnDedicatedThread]] FileWatchManager {
  public:
      struct WatchDesc {
          std::filesystem::path directory;
          bool recursive = true;
          uint32_t filter = FileChangeFilter::All;
      };

      // 配置在任意线程（内部排队到 I/O 线程）
      [[=kAnyThread]]
      WatchHandle AddWatch(WatchDesc desc);

      [[=kAnyThread]]
      void RemoveWatch(WatchHandle handle);

      void Start(EventChannel<>& outputChannel);
      void Stop();

  private:
      std::jthread ioThread_;
      EventChannel<>* output_ = nullptr;
  };

  ---
  Free-Threaded Managers 的模式

  这些 Manager 不需要线程迁移，不返回 OwnerTask，直接返回值：

  class [[=kFreeThreaded]] DisplayManager {
  public:
      [[=kAnyThread]]
      std::span<const MonitorInfo> GetMonitors() const noexcept;

      [[=kAnyThread]]
      MonitorInfo GetPrimaryMonitor() const noexcept;

      [[=kAnyThread]]
      std::span<const DisplayMode> GetModes(MonitorId monitor) const noexcept;

  private:
      friend class EventPump; // bookkeep: WM_DISPLAYCHANGE 时刷新缓存

      [[=Detail::BookkeepFor{EventKind::DisplayChange}]]
      void OnPump_DisplayChange(const SystemEvent& event) noexcept;

      // 缓存（platform thread 更新，any-thread 读取）
      SeqLock<MonitorCache> cache_;
  };

  注意：DisplayManager 是 free-threaded 的，但它的数据来源（WM_DISPLAYCHANGE）是 platform thread 的。解法：EventPump
  在翻译 WM_DISPLAYCHANGE 时调用 DisplayManager::OnPump_DisplayChange 做 bookkeep（写 SeqLock），其他线程通过
  GetMonitors() 读 SeqLock。

  这个模式适用于所有 "Any-thread query + Platform-thread update" 的 Manager。

  ---
  完整 PlatformThread 持有的 Manager 集合

  class PlatformThread {
  public:
      void Run();
      void RequestStop() noexcept;

      // ---- Platform Thread Managers ----
      WindowManager& Windows() noexcept;
      InputManager& Input() noexcept;
      ImeManager& Ime() noexcept;
      ClipboardManager& Clipboard() noexcept;
      CursorManager& Cursor() noexcept;
      DragDropManager& DragDrop() noexcept;
      DialogManager& Dialogs() noexcept;
      SurfaceManager& Surfaces() noexcept;
      SystemAppearanceManager& Appearance() noexcept;
      AccessibilityManager& Accessibility() noexcept;

      // ---- Dedicated Thread Managers (owned, spawned by PlatformThread) ----
      GamepadManager& Gamepads() noexcept;
      FileWatchManager& FileWatches() noexcept;

      // ---- Free-Threaded Managers ----
      DisplayManager& Displays() noexcept;
      PowerManager& Power() noexcept;
      AudioDeviceManager& AudioDevices() noexcept;

      // ---- Event Channel Management ----
      void AttachChannel(EventChannel<>& channel) noexcept;
      void DetachChannel(EventChannel<>& channel) noexcept;

      OwnerExecutor& Executor() noexcept;

  private:
      EventPump pump_;
      OwnerExecutor executor_;

      // Platform thread managers
      WindowManager windowMgr_;
      InputManager inputMgr_;
      ImeManager imeMgr_;
      ClipboardManager clipboardMgr_;
      CursorManager cursorMgr_;
      DragDropManager dragDropMgr_;
      DialogManager dialogMgr_;
      SurfaceManager surfaceMgr_;
      SystemAppearanceManager appearanceMgr_;
      AccessibilityManager accessibilityMgr_;

      // Dedicated thread managers
      GamepadManager gamepadMgr_;
      FileWatchManager fileWatchMgr_;

      // Free-threaded managers (platform thread bookkeeps)
      DisplayManager displayMgr_;
      PowerManager powerMgr_;
      AudioDeviceManager audioDeviceMgr_;

      bool running_ = true;
  };

  ---
  Consteval 验证矩阵（完整版）

  // Platform/ManagerTraits.h 中的完整验证

  namespace Mashiro::Platform::Detail {

      template<typename... Managers>
      consteval void VerifySchedulingContracts() {
          auto verify_one = []<typename M>() {
              constexpr auto schedAnnots = std::define_static_array(
                  std::meta::annotations_of(^^M, ^^ManagerSchedule));
              static_assert(schedAnnots.size() == 1,
                  "Every Manager must have exactly one ManagerSchedule annotation");

              constexpr auto sched = std::meta::extract<ManagerSchedule>(schedAnnots[0]);

              for (auto method : std::meta::members_of(^^M,
                       std::meta::access_context::unchecked())) {
                  if (!std::meta::is_function(method)) continue;
                  if (!std::meta::is_public(method)) continue;

                  auto contracts = std::meta::annotations_of(method, ^^ThreadContract);
                  if (contracts.size() == 0) continue;
                  auto contract = std::meta::extract<ThreadContract>(contracts[0]);

                  auto ret = std::meta::return_type_of(method);
                  bool isOwnerTask = std::meta::has_template_arguments(ret) &&
                                     std::meta::template_of(ret) == ^^OwnerTask;

                  // Rule 1: PlatformThread managers — Platform 方法 → OwnerTask
                  if constexpr (sched.mode == ScheduleMode::PlatformThread) {
                      if (contract.domain == ThreadDomain::Platform && !isOwnerTask) {
                          throw "PlatformThread manager: Platform-domain method must return OwnerTask";
                      }
                  }

                  // Rule 2: FreeThreaded managers — 不应有 OwnerTask 返回
                  if constexpr (sched.mode == ScheduleMode::FreeThreaded) {
                      if (isOwnerTask) {
                          throw "FreeThreaded manager must not return OwnerTask";
                      }
                  }

                  // Rule 3: DedicatedThread managers — 不应暴露 OwnerTask public API
                  if constexpr (sched.mode == ScheduleMode::DedicatedThread) {
                      if (isOwnerTask) {
                          throw "DedicatedThread manager must not expose OwnerTask public API";
                      }
                  }
              }
          };

          (verify_one.template operator()<Managers>(), ...);
      }

  } // namespace Mashiro::Platform::Detail

  ---
  EventPump Bookkeep 分发（利用 template for 展开所有 Manager）

  void EventPump::TranslateAndEmit(MSG& msg) {
      auto event = TranslateWin32(msg);
      if (!event) return;

      event->sequence = nextSequence_++;
      event->timestamp = GetQpcTimestamp();

      // Bookkeep: consteval-generated dispatch to ALL managers
      DispatchBookkeep(*windowMgr_, *event);
      DispatchBookkeep(*inputMgr_, *event);
      DispatchBookkeep(*imeMgr_, *event);
      DispatchBookkeep(*clipboardMgr_, *event);
      DispatchBookkeep(*cursorMgr_, *event);
      DispatchBookkeep(*displayMgr_, *event);      // free-threaded but bookkeep on platform thread
      DispatchBookkeep(*appearanceMgr_, *event);
      DispatchBookkeep(*powerMgr_, *event);

      // Broadcast to all attached channels (wakes awaiting client coroutines)
      Broadcast(*event);
  }

  每个 DispatchBookkeep<M> 在编译期通过 template for 展开为精确的 switch-case：只有标注了 [[=BookkeepFor{EventKind::X}]]
   的方法才会被 dispatch。没有标注 = 空展开 = 零成本。

  ---
  事件产出汇总

  ┌─────────────────────────────┬──────────────────────────────────────────────────────┬───────────────────────────┐
  │            来源             │                       产出事件                       │ 进入 EventChannel 的路径  │
  ├─────────────────────────────┼──────────────────────────────────────────────────────┼───────────────────────────┤
  │ Platform thread (Win32      │ Window*, Input*, Ime*, Clipboard*, DragDrop*,        │ EventPump → Broadcast     │
  │ pump)                       │ Theme*, DPI*, Display*, Power*                       │                           │
  ├─────────────────────────────┼──────────────────────────────────────────────────────┼───────────────────────────┤
  │ GamepadManager (dedicated   │ GamepadConnect, GamepadDisconnect, GamepadState      │ 直接写入自己绑定的        │
  │ thread)                     │                                                      │ EventChannel              │
  ├─────────────────────────────┼──────────────────────────────────────────────────────┼───────────────────────────┤
  │ FileWatchManager (dedicated │ FileCreated, FileModified, FileDeleted, FileRenamed  │ 直接写入自己绑定的        │
  │  thread)                    │                                                      │ EventChannel              │
  └─────────────────────────────┴──────────────────────────────────────────────────────┴───────────────────────────┘

  Client 只需 co_await 自己的 EventChannel，所有事件来源统一为同一个流。如果 client 想区分来源，可以 attach 多个
  channel（一个只接收 platform events，一个只接收 gamepad events），或者用 EventKind 过滤。

  ---
  总结

  ┌───────────────────────────────┬────────────────────────────────────────────────────────────────────────────┐
  │             维度              │                                    决策                                    │
  ├───────────────────────────────┼────────────────────────────────────────────────────────────────────────────┤
  │ Platform thread manager 数量  │ 10 个（窗口、输入、IME、剪贴板、光标、DnD、对话框、Surface、外观、无障碍） │
  ├───────────────────────────────┼────────────────────────────────────────────────────────────────────────────┤
  │ Dedicated thread manager 数量 │ 2 个（手柄、文件监控）                                                     │
  ├───────────────────────────────┼────────────────────────────────────────────────────────────────────────────┤
  │ Free-threaded manager 数量    │ 4 个（显示器、电源、音频设备、计时）                                       │
  ├───────────────────────────────┼────────────────────────────────────────────────────────────────────────────┤
  │ 调度验证                      │ consteval: Manager 类 annotation → 方法 annotation → 返回类型检查          │
  ├───────────────────────────────┼────────────────────────────────────────────────────────────────────────────┤
  │ 事件统一通道                  │ EventChannel<>（SPSC, awaitable, broadcast）                               │
  ├───────────────────────────────┼────────────────────────────────────────────────────────────────────────────┤
  │ 跨线程调用                    │ OwnerTask<T>（只用于 PlatformThread managers）                             │
  ├───────────────────────────────┼────────────────────────────────────────────────────────────────────────────┤
  │ 状态查询一致性                │ SeqLock（platform thread 写，any-thread 读）                               │
  ├───────────────────────────────┼────────────────────────────────────────────────────────────────────────────┤
  │ Bookkeep dispatch             │ consteval template for 展开，零虚调用                                      │
  └───────────────────────────────┴────────────────────────────────────────────────────────────────────────────┘

  要进入 spec 文档编写，还是有需要调整的地方？
  ⎿  Stop says:
     ---
     ✅ Session ended

     📁 Git repository
       Branch: main
       Changes:
         增加: 3 files
         修改: 5 files

     ✅ No temp files
     ---

✻ Cogitated for 5m 55s

❯ EventChannel<>（SPSC, awaitable, broadcast）   使用SPSC会不会导致channel数量等于manager 数量与client数量之积？

● No skills needed.

  好问题。让我诚实分析这个拓扑问题。

  ---
  问题分析

  当前设计：每个 EventChannel 是 SPSC（单 producer，单 consumer）。如果按"每个 producer 对每个 consumer 一条
  channel"的纯笛卡尔积理解：

  - 事件来源：Platform thread pump（1）+ GamepadManager（1）+ FileWatchManager（1）= 3 producers
  - 消费者：render thread + logic thread + UI thread = N consumers

  纯 SPSC 笛卡尔积 = 3 × N 条 channel。

  但实际上不是这样。让我重新审视数据流拓扑。

  ---
  实际拓扑

  观察事件流的真实形状：

  Platform thread pump ─────┐
                            │
  GamepadManager ───────────┼──── 汇聚 ────▶ Client A 的 channel (1条)
                            │
  FileWatchManager ─────────┘


  Platform thread pump ─────┐
                            │
  GamepadManager ───────────┼──── 汇聚 ────▶ Client B 的 channel (1条)
                            │
  FileWatchManager ─────────┘

  从 client 视角：每个 client 只想看到一条统一的事件流，不关心事件来自哪个 producer。

  从 producer 视角：每个 producer 想 broadcast 到所有 client。

  真正的问题是：一条 SPSC channel 只有一个 producer。如果有 3 个 producer 都想写入同一个 client 的 channel，那就不再是
  SPSC 了。

  ---
  三种解法

  方案 A：per-client 一条 MPSC channel（多 producer → 单 consumer）

  channel 数量 = N（client 数量）
  每条 channel：多 producer（platform pump + gamepad + filewatch）写入，单 consumer 读取

  - 优点：channel 数量最小（= client 数）
  - 缺点：需要 MPSC ring buffer（比 SPSC 多一次原子操作 per push）。你现有的 SpscQueue 不能直接用。

  方案 B：per-producer-per-client SPSC（当前设计的笛卡尔积）

  channel 数量 = P × N（producer 数 × client 数）
  每条 channel：严格 SPSC

  - 优点：直接复用你现有的 SpscQueue，性能最高（producer 侧零原子 on fast path）
  - 缺点：client 需要 poll 多条 channel（3 条），不能用单一 co_await

  方案 C：per-client 一条 SPSC channel，但只有一个 writer（platform thread 代理写入）

  channel 数量 = N（client 数量）
  每条 channel：严格 SPSC
  Platform thread 是唯一 writer
  Dedicated thread managers 不直接写 channel，而是把事件投递到 platform thread 的 mailbox，由 platform thread 统一写入

  - 优点：保持 SPSC 性能（producer 侧零竞争），channel 数 = client 数
  - 缺点：dedicated thread 的事件多了一跳（dedicated thread → platform thread → channel →
  client）。但这个跳跃成本极小（一次 MPSC enqueue + 一次 memcpy 64 bytes）

  ---
  推荐：方案 C

  理由：

  1. 保持 SPSC 的性能优势 — producer 侧零原子操作（你的 SpscQueue 的 fast path 是纯 relaxed store）
  2. Channel 数量 = client 数量（通常 2-4 条），不是笛卡尔积
  3. Client 只需 co_await 一条 channel — 统一事件流，不需要多路 select
  4. Dedicated thread 的事件延迟增加极小 — GamepadManager 本来就是 4ms 轮询周期，多一跳的延迟 < 1μs，完全不可感知
  5. 复用你已有的 SpscQueue — 不需要新写 MPSC ring buffer（mailbox 已有 MPSC 队列用于 coroutine
  handles，可以同样承载事件投递）

  架构调整：

  ┌─────────────────────────────────────────────────────────┐
  │                    Platform Thread                        │
  │                                                          │
  │  ┌──────────────┐                                        │
  │  │ OS Pump      │──┐                                     │
  │  └──────────────┘  │                                     │
  │                    ▼                                      │
  │  ┌──────────────────────────────┐    SPSC     ┌────────┐│
  │  │ Unified Event Writer         │────────────▶│Client A││
  │  │ (platform thread is sole     │    SPSC     ├────────┤│
  │  │  producer for ALL channels)  │────────────▶│Client B││
  │  └──────────────────────────────┘             └────────┘│
  │                    ▲                                      │
  │  ┌──────────────┐  │ enqueue event                       │
  │  │ GamepadMgr   │──┘ (via MPSC mailbox, same as         │
  │  │ (own thread) │     coroutine submission)              │
  │  ├──────────────┤                                        │
  │  │ FileWatchMgr │──┘                                     │
  │  └──────────────┘                                        │
  └─────────────────────────────────────────────────────────┘

  ---
  实现细化

  Dedicated thread managers 不直接写 EventChannel。它们把事件投递到 platform thread 的一个 event inbox（MPSC queue of
  SystemEvent），platform thread 在 pump 循环中 drain 这个 inbox 并统一写入所有 client channels：

  class EventPump {
  public:
      // Platform thread 调用
      void PumpAll() {
          // 1. Pump Win32 messages → translate → 写入 all channels
          PumpOsMessages();

          // 2. Drain external event inbox (from dedicated threads)
          DrainExternalEvents();
      }

  private:
      // MPSC queue: dedicated threads push, platform thread pops
      MpscQueue<SystemEvent> externalInbox_;

      void DrainExternalEvents() {
          SystemEvent event;
          while (externalInbox_.TryPop(event)) {
              // Bookkeep (如果需要)
              // Broadcast to all client channels
              Broadcast(event);
          }
      }

      void Broadcast(const SystemEvent& event) noexcept {
          for (uint8_t i = 0; i < channelCount_; ++i) {
              channels_[i]->Emit(event); // SPSC push — 只有 platform thread 写
          }
      }
  };

  Dedicated thread 的使用：

  class GamepadManager {
  public:
      void Start(EventPump& pump) {
          pump_ = &pump;
          pollThread_ = std::jthread([this](std::stop_token st) {
              while (!st.stop_requested()) {
                  PollAllPads();
                  std::this_thread::sleep_for(4ms);
              }
          });
      }

  private:
      EventPump* pump_ = nullptr;

      void EmitEvent(SystemEvent event) noexcept {
          // 投递到 platform thread 的 external inbox
          pump_->SubmitExternal(event);
      }
  };

  EventPump 新增接口：

  class EventPump {
  public:
      // Called by dedicated thread managers (thread-safe, MPSC)
      void SubmitExternal(const SystemEvent& event) noexcept {
          externalInbox_.Push(event);
          WakeOwnerThread(); // 唤醒 platform thread 去 drain
      }
  };

  ---
  最终拓扑数量

  ┌───────────────────────────────┬───────────────────────────────────────────────────────────────────────┐
  │             对象              │                                 数量                                  │
  ├───────────────────────────────┼───────────────────────────────────────────────────────────────────────┤
  │ EventChannel（SPSC）          │ = client 线程数（通常 2-4）                                           │
  ├───────────────────────────────┼───────────────────────────────────────────────────────────────────────┤
  │ External inbox（MPSC）        │ 1（所有 dedicated threads → platform thread）                         │
  ├───────────────────────────────┼───────────────────────────────────────────────────────────────────────┤
  │ OwnerExecutor mailbox（MPSC） │ 1（所有 worker threads → platform thread，用于 OwnerTask coroutines） │
  └───────────────────────────────┴───────────────────────────────────────────────────────────────────────┘

  总共：N 条 SPSC channel + 1 条 MPSC event inbox + 1 条 MPSC coroutine mailbox。

  不是笛卡尔积。是 O(clients) 不是 O(producers × clients)。
