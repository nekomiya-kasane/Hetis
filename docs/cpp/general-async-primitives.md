# 通用异步框架的复用原语：超越 stdexec 基础集

> 抽象层次的正确性，不来自命名艺术，而来自把已经存在却隐藏的实体显化。本文枚举的是
> 真实异步系统里反复手抄、却长期没有一等命名的形态。

## 引言

stdexec 提供 sender / receiver / scheduler / env 四件套，覆盖"调度 + 完成 + 取消 + 环境"。
它有意保持极简，二阶抽象留给上层框架——结果是几乎每个真实异步系统都在重写同一套
模式。本文给出值得提升为一等原语的形态。

判别"是否值得提升"的标准三条：

1. **形态稳定性**：同形态在三处以上独立出现，字面差异在变量名级别。
2. **语义独立性**：能脱离具体业务上下文进行定义与讨论。
3. **现有词汇不足**：复用 stdexec 现有词会撞车（如 `environment`），借 OOP 词会引导
   错误的实现模型（`Observer` 暗示虚函数与动态派发）。

三条全过才提升；否则保留为局部模式即可。C++26 的 reflection / consteval / annotation
是工具不是目的——*合适处*用，能让契约从注释升格为类型；*不合适处*硬塞会把简单组合
子复杂化。下面每个原语都标注了"该不该用反射这类设施"。

## 已讨论过的原语（实现展开）

下列原语在前序对话中已介绍语义；这里给出与本文其他原语风格一致的具体实现要点，让本
文自包含。

### A. `Apartment<Tag>` —— 线程隶属性的形式化

**核心结构：**

```cpp
namespace Async {

    template <auto Tag>
    struct Apartment {
        using scheduler_type = /* impl-defined: 该 apartment 的对外 scheduler */;

        // env 接口：让任何 receiver 通过 query 拿到 apartment 标识
        struct Env {
            constexpr auto query(stdexec::get_scheduler_t) const noexcept;
            constexpr Apartment<Tag> query(GetApartmentT) const noexcept { return {}; }
        };

        Env GetEnv() const noexcept { return {}; }
    };

    // 对象层 concept：T 声明自己属于某 apartment
    template <class T, auto A>
    concept ConfinedTo = requires {
        typename T::apartment_type;
        requires std::same_as<typename T::apartment_type, Apartment<A>>;
    };

    // sender adaptor：把执行迁移到 apartment（语义上比 continues_on 更强：
    // 它断言"接下去的所有 ConfinedTo<TargetApt> 操作都是安全的"）
    template <auto Target, stdexec::sender S>
    auto MigrateTo(S s) noexcept {
        return stdexec::continues_on(std::move(s),
                                     Apartment<Target>{}.GetEnv().query(stdexec::get_scheduler))
             | stdexec::then([]<class... V>(V&&... v) noexcept {
                   return std::tuple{std::forward<V>(v)...};   // 占位：可加 apartment 断言
               });
    }
}
```

**Annotation 写法：**

```cpp
struct GuiAptTag {};
struct [[=Async::ConfineTo<GuiAptTag{}>]] WindowHandle {
    using apartment_type = Async::Apartment<GuiAptTag{}>;
    /* ... */
};

// 编译期检查：sender 链消费 WindowHandle 时必须在 GUI apartment
template <stdexec::sender S, class T>
concept SafeToConsume = requires {
    requires (!Confined<T> ||
              std::same_as<typename S::env_type::apartment_type,
                           typename T::apartment_type>);
};
```

C++26 杠杆：annotation 把对象的 apartment 写在类型上；reflection 在 sender 接 receiver
时检查 apartment 一致性，不一致编译期失败。

### B. `Tether<Acquire, Release>` —— 异步 RAII

**核心实现：**

```cpp
namespace Async {

    template <stdexec::sender AcquireS, std::invocable<typename AcquireS::value_type&> ReleaseFn>
    class Tether {
        AcquireS  acquire_;
        ReleaseFn release_;

    public:
        Tether(AcquireS a, ReleaseFn r) : acquire_(std::move(a)), release_(std::move(r)) {}

        template <class Body>
        [[nodiscard]] auto Use(Body body) && noexcept {
            return std::move(acquire_)
                 | stdexec::let_value([body = std::move(body),
                                       release = std::move(release_)]
                                      (auto& resource) mutable {
                       return std::move(body)(resource)
                            | stdexec::then([&resource, &release]<class U>(U&& u) noexcept {
                                  // 成功路径：release 完成后透传 value
                                  return std::tuple{std::forward<U>(u)};
                              })
                            | stdexec::let_value([&resource, &release](auto& tup) {
                                  return release(resource)
                                       | stdexec::then([&tup] { return std::get<0>(std::move(tup)); });
                              })
                            | stdexec::upon_error([&resource, &release](std::exception_ptr e) {
                                  // 错误路径：尽力 release，再抛原错误
                                  // (release 自身错误聚合到 TetherCompositeError)
                                  RunDiscard(release(resource));
                                  std::rethrow_exception(e);
                              })
                            | stdexec::let_stopped([&resource, &release]() {
                                  RunDiscard(release(resource));
                                  return stdexec::just_stopped();
                              });
                   });
        }
    };

    template <class A, class R>
    auto MakeTether(A acquire, R release) {
        return Tether<A, R>{std::move(acquire), std::move(release)};
    }
}
```

完成路径分三轨且 release 必跑——这是 Tether 的整个核心契约。

### C. `Sluice<Policy>` 家族 —— 背压

**核心：策略类型决定 channel 行为。** 三种策略各自实现 `BeforePush` / `AfterDrain`：

```cpp
namespace Async {

    template <class P, class T>
    concept SluicePolicy = requires(P& p) {
        { p.template BeforePush<T>() } -> stdexec::sender;     // 满则等
        { p.template AfterDrain<T>() } -> std::same_as<void>;
    };

    namespace Policy {
        template <std::size_t N>
        class Credit {
            detail::AsyncSemaphore sem_{N};
        public:
            template <class T> auto BeforePush() noexcept {
                return AcquireSlot{&sem_};   // 见 Bulkhead
            }
            template <class T> void AfterDrain() noexcept { sem_.Release(); }
        };

        template <std::size_t High, std::size_t Low>
        class Watermark { /* size 跨 High → 暂停；跨 Low → 恢复 */ };

        template <auto Rate, auto Action>
        class Throttle { /* 单位时间最多 Rate 次；超出按 Action：drop/coalesce */ };
    }

    template <class Pol, class T, std::size_t Capacity = 256>
    class Sluice {
        Pol            policy_;
        Stream<T, Capacity> stream_;
    public:
        [[nodiscard]] auto Push(T v) noexcept {
            return policy_.template BeforePush<T>()
                 | stdexec::then([this, v = std::move(v)]() mutable {
                       return stream_.Emit(std::move(v));   // bool: admitted
                   });
        }
        [[nodiscard]] auto Pull() noexcept {
            return stream_.Next()
                 | stdexec::then([this]<class V>(V&& v) noexcept {
                       policy_.template AfterDrain<T>();
                       return std::forward<V>(v);
                   });
        }
    };
}
```

每种 Policy 在编译期被实例化，op_state 形态各异——Credit 用信号量，Watermark 多一个
boolean 状态，Throttle 用环形时间戳数组。零 type erasure。

### D. `Beacon<Schema>` —— 诊断的环境传播

**关键：Schema 用 reflection 展开为 env 字段；Wrap 用 receiver 包装把 beacon 注入 env。**

```cpp
namespace Async {

    template <class Schema>
    class Beacon {
        Schema fields_;

    public:
        explicit Beacon(Schema s) noexcept : fields_(std::move(s)) {}

        template <stdexec::sender Snd>
        [[nodiscard]] auto Wrap(Snd s) const & {
            return BeaconSender<Schema, Snd>{ &fields_, std::move(s) };
        }

        // 取 Schema 的某个字段；reflection 把 Name → 偏移
        template <std::string_view Name>
        const auto& Field() const noexcept {
            constexpr auto F = [&]{
                for (auto m : nonstatic_data_members_of(^^Schema))
                    if (identifier_of(m) == Name) return m;
                std::unreachable();
            }();
            return fields_.[: F :];
        }
    };

    // BeaconSender: connect 时把自己塞进 receiver env；set_value/error/stopped
    // 都被劫持到 beacon.Close(...)
    template <class Schema, class Snd>
    struct BeaconSender { /* ... */ };
}
```

Schema annotation 校验：

```cpp
struct GuiSpan {
    [[=Beacon::Required]] std::string_view trace_id;
    [[=Beacon::Optional]] std::string_view widget;
    [[=Beacon::Counter]]  std::uint64_t    frame_no;
};

// 构造 Beacon<GuiSpan>(s) 时 consteval 检查 Required 字段都已被填值
```

### E. `Chronograph<Clock, Sched>` —— 时间作为 sender 资源

```cpp
namespace Async {

    template <class C, class S>
    concept SupportsTimedSchedule = requires(S sched, typename C::time_point t) {
        { stdexec::schedule_at(sched, t) } -> stdexec::sender;
    };

    template <class Clock, class Sched>
        requires SupportsTimedSchedule<Clock, Sched>
    class Chronograph {
        Sched sched_;

    public:
        [[nodiscard]] auto At(typename Clock::time_point t) const noexcept {
            return stdexec::schedule_at(sched_, t);
        }
        [[nodiscard]] auto After(typename Clock::duration d) const noexcept {
            return At(Clock::now() + d);
        }
        [[nodiscard]] auto Periodic(typename Clock::duration p) const noexcept;   // -> AsyncSeq

        template <stdexec::sender S>
        [[nodiscard]] auto Deadline(S s, typename Clock::time_point t) const noexcept {
            // s 与 At(t) race；先到的 At(t) 触发对 s 的 stop
            return stdexec::stop_when(std::move(s), At(t));
        }
    };
}
```

Mock 时钟：把 `Chronograph<MockClock, NullScheduler>` 换进去，时间相关 sender 立即可
控——这是普通 `std::chrono::steady_clock` 拿不到的解耦。

### F. `Outcome<S>` —— 完成形格的统一

```cpp
namespace Async::detail {

    // 从 sender S 的 completion_signatures 折出 variant 的 alternatives
    template <stdexec::sender S>
    consteval auto BuildOutcomeVariant() {
        // 收集所有 set_value(Vs...) → tuple<Vs...>
        // 收集所有 set_error(E)   → E
        // 收集 set_stopped()       → struct Stopped {}
        // 加上框架内置 TimedOut / Throttled / Overflowed
        // 用 substitute 构造 std::variant<...>
    }
}

namespace Async {

    template <stdexec::sender S>
    using Outcome = decltype(detail::BuildOutcomeVariant<S>());

    template <stdexec::sender S>
    [[nodiscard]] auto IntoOutcome(S s) noexcept {
        return std::move(s)
             | stdexec::then([]<class... Vs>(Vs&&... v) -> Outcome<S> {
                   return Outcome<S>{ std::in_place_type<std::tuple<Vs...>>,
                                       std::forward<Vs>(v)... };
               })
             | stdexec::upon_error([](auto e) -> Outcome<S> { return Outcome<S>{e}; })
             | stdexec::let_stopped([]() -> stdexec::sender auto {
                   return stdexec::just(Outcome<S>{Stopped{}});
               });
    }
}
```

下游访问用 `template for` 走 variant 的 alternatives，零 dispatch。

### G. `AsyncSeq<T>` —— 拉式异步序列

```cpp
namespace Async {

    template <class T>
    concept AsyncSeq = requires(T t) {
        typename T::value_type;
        { t.Next() } -> stdexec::sender_of<
            stdexec::set_value_t(std::optional<typename T::value_type>)>;
    };

    // 函数式族——每个返回的也是 AsyncSeq
    template <AsyncSeq Src, std::invocable<typename Src::value_type> F>
    auto Transform(Src src, F f) {
        struct Impl {
            using value_type = std::invoke_result_t<F, typename Src::value_type>;
            Src src; F f;
            auto Next() noexcept {
                return src.Next() | stdexec::then(
                    [&f = f](std::optional<typename Src::value_type> opt)
                        -> std::optional<value_type> {
                        if (!opt) return std::nullopt;
                        return f(std::move(*opt));
                    });
            }
        };
        return Impl{std::move(src), std::move(f)};
    }

    template <AsyncSeq Src, std::predicate<typename Src::value_type> P>
    auto Filter(Src src, P p);     // 协程实现：循环拉取直到命中

    template <AsyncSeq Src>
    auto Take(Src src, std::size_t n);

    template <std::ranges::range R>
    auto FromRange(R r);            // 把 range 适配为 AsyncSeq；Next 即同步返回下一项

    template <AsyncSeq Src>
    auto ToVector(Src src);         // sender<std::vector<value_type>>；终结操作
}
```

Transform/Filter 的实现可以是协程（`Task<std::optional<U>>` 形式），让 `Filter` 里"循环
拉取直到 predicate 命中"自然写成 while + co_await——这是协程在异步序列上的天然映射。

### H. `Edge<S, Hooks...>` —— 钩子化 sender adaptor

```cpp
namespace Async {

    struct OnStart   {}; struct OnValue   {}; struct OnError {};
    struct OnStopped {}; struct OnFinally {};

    template <class H, class Tag>
    concept HookFor = /* H 有带 [[=Tag{}]] 的成员函数 */ ;

    template <stdexec::sender S, class... Hooks>
    [[nodiscard]] auto Edge(S s, Hooks... hooks) {
        return EdgeSender<S, Hooks...>{ std::move(s), std::move(hooks)... };
    }
}
```

`EdgeSender::connect` 构造的 receiver 在每条完成轨上 reflection 扫描 hooks，调用带匹配
annotation 的方法。`if constexpr` 剪枝：未提供该轨 hook 的 hook 类型在该轨上塌成 noop。

```cpp
// 用户写：
struct LoggingHooks {
    [[=Async::OnStart  {}]] void Begin()                       noexcept;
    [[=Async::OnError  {}]] void Trip(std::exception_ptr)      noexcept;
    [[=Async::OnFinally{}]] void End()                         noexcept;
};

auto traced = Async::Edge(DoWork(), LoggingHooks{});
```

Hooks 是值语义、没有虚函数；reflection 把 annotation → 方法名映射，整个 hook 派发是
直接调用，可被内联。

---


## 一、`Rendezvous` —— 多方约定汇合

### 场景

- 服务启动：N 个组件初始化完成后才开放对外端口。
- 帧同步：多线程各自完成本帧贡献后才能呈现。
- 测试夹具：所有 worker 抵达检查点后再放行。
- 关停握手：所有订阅者确认接收到 shutdown 后主控才退出。

### 问题

`std::latch` / `std::barrier` 是阻塞 API，与 sender 模型不兼容。当下做法要么手抄
`atomic<int> count + condition_variable + sender 适配`，要么用 `when_all` 凑合——但
`when_all` 是*事先静态已知*的 N 个 sender 的并行汇合，不是*动态加入*的 N 方约定。

### 形式与实现

核心数据结构是 *count + 侵入式等待者链表*。等待者节点保存在 op_state 内部（栈上分
配），`arrive` 减一到零时一次性把链表 splice 出来逐个 `set_value`；后续 `wait()` 上的
op_state 直接 set_value 不挂表。

```cpp
namespace Async::detail {

    // Intrusive waiter base. Concrete op_state 继承它，wake() 内含具体的 set_value 行为。
    struct LatchWaiterBase {
        LatchWaiterBase*           next  = nullptr;
        LatchWaiterBase*           prev  = nullptr;
        virtual void Wake() noexcept = 0;     // 完成在调用线程上同步发生
        virtual void Cancel() noexcept = 0;   // stop_token 触发
    };

    // 状态机：count_ 用 sign bit 编码"已完成"标志，避免单独 atomic<bool>。
    //   count_ > 0  : 还差 count_ 个 arrive
    //   count_ == 0 : 完成时刻；arrive 在该刻 publish；之后转入 RELEASED
    //   count_ < 0  : RELEASED；waiter 立即 Wake；arrive 是 no-op（设计选择）
    inline constexpr std::ptrdiff_t kReleased = std::numeric_limits<std::ptrdiff_t>::min() / 2;
}

namespace Async {

    class Latch {
    public:
        explicit Latch(std::ptrdiff_t count) noexcept : count_(count) {
            // count <= 0 直接进 RELEASED 态；构造同步、无锁
            if (count <= 0) count_.store(detail::kReleased, std::memory_order_relaxed);
        }
        Latch(const Latch&) = delete;
        Latch& operator=(const Latch&) = delete;
        ~Latch() {
            // 析构契约：所有 waiter 已被唤醒；上层用 scope 保证此点
            assert(waiters_.Empty());
        }

        void Arrive(std::ptrdiff_t n = 1) noexcept {
            auto prev = count_.fetch_sub(n, std::memory_order_acq_rel);
            if (prev > 0 && prev - n <= 0) {
                // 这一刻是发布点；CAS 把 count_ 设为 RELEASED，并 splice 出整张 waiter 表
                Release();
            }
        }

        // 返回 sender<> ；语义：count == 0 之前挂起；之后立刻 set_value。
        // stop_token 在挂起期间生效；触发时 set_stopped。
        [[nodiscard]] auto Wait() noexcept;

        [[nodiscard]] auto ArriveAndWait() noexcept {
            return stdexec::just()
                 | stdexec::let_value([this]() noexcept {
                     Arrive();
                     return Wait();
                 });
        }

    private:
        friend struct WaitOpStateBase;
        // count + waiter list 用一对儿原子位读+小锁组合，仅在 release 边界用一次。
        // 稳态路径上 wait/arrive 都是 lock-free。
        std::atomic<std::ptrdiff_t>            count_;
        detail::IntrusiveList<LatchWaiterBase> waiters_;
        std::atomic_flag                        listLock_ = ATOMIC_FLAG_INIT;

        void Release() noexcept;        // 把 count_ 写为 kReleased 并唤醒整张表
        bool TryEnqueue(detail::LatchWaiterBase&) noexcept;
        void TryDequeue(detail::LatchWaiterBase&) noexcept;
    };

    // Wait() 的实现：返回一个自定义 sender，connect 时构造 op_state（继承 LatchWaiterBase）
    namespace detail {
        struct WaitSender {
            using sender_concept = stdexec::sender_t;
            using completion_signatures = stdexec::completion_signatures<
                stdexec::set_value_t(),
                stdexec::set_stopped_t()>;

            Latch* l_;
            template <stdexec::receiver R>
            auto connect(R r) const noexcept;     // -> WaitOpState<R>
        };

        template <stdexec::receiver R>
        struct WaitOpState : LatchWaiterBase {
            R                                                   recv_;
            Latch*                                              latch_;
            std::optional<stdexec::stop_callback_for_t<
                stdexec::stop_token_of_t<stdexec::env_of_t<R>>,
                StopFn>>                                        stopCb_;

            WaitOpState(Latch* l, R r) noexcept : recv_(std::move(r)), latch_(l) {}

            friend void tag_invoke(stdexec::start_t, WaitOpState& self) noexcept {
                // 1) fast path: latch already released
                if (self.latch_->count_.load(std::memory_order_acquire) <= 0) {
                    stdexec::set_value(std::move(self.recv_));
                    return;
                }
                // 2) install stop callback before enqueue
                self.stopCb_.emplace(
                    stdexec::get_stop_token(stdexec::get_env(self.recv_)),
                    StopFn{&self});
                // 3) enqueue; if release happened concurrently, TryEnqueue returns false
                //    and we synchronously complete with set_value
                if (!self.latch_->TryEnqueue(self)) {
                    self.stopCb_.reset();
                    stdexec::set_value(std::move(self.recv_));
                }
            }

            void Wake() noexcept override {
                stopCb_.reset();
                stdexec::set_value(std::move(recv_));
            }
            void Cancel() noexcept override {
                latch_->TryDequeue(*this);
                stdexec::set_stopped(std::move(recv_));
            }

            struct StopFn { WaitOpState* self; void operator()() const noexcept { self->Cancel(); } };
        };
    } // namespace detail

    inline auto Latch::Wait() noexcept { return detail::WaitSender{this}; }
}
```

`Barrier` / `Phaser` 沿用同形态：

- `Barrier`：内部维持 `(arrived, generation)` 二元组，`arrive_and_wait` 用 generation 区分
  轮次。op_state 记录入场时的 generation；`Release` 把 generation+1 并把这一代的 waiter
  全部 Wake。`OnPhase` callback 在 arrived 凑齐到 release 之间同步运行——这是 phase action
  的唯一安全位置。
- `Phaser`：`register_party` 返回带 generation 的 token；`arrive_and_advance` 与 Barrier 类似，
  额外允许参与者数动态变化（`registered` 计数与 `arrived` 计数同时维护）。

### 示例

```cpp
Async::Latch ready{services.size()};
for (auto& s : services)
    scope.Spawn(stdexec::on(pool, s.Init()) | stdexec::then([&]{ ready.Arrive(); }));
co_await ready.Wait();
co_await OpenListenSocket();
```

### C++26 杠杆点

- *concept-only*。op_state 全部走具体类型，不用 type erasure，所以 `set_value` 是一次直接
  调用、无虚函数（`Wake/Cancel` 是基类虚函数但 *只在跨 op_state 唤醒时*调用一次，与
  `std::condition_variable::notify_all` 同量级）。
- `Phaser` 的 `PartyToken` 用 nominal class 而非 `int`：

```cpp
class Phaser {
public:
    class [[nodiscard]] PartyToken {        // 不能跨 Phaser 实例混用
        Phaser* owner_;
        std::uint32_t  gen_;
        friend class Phaser;
    };
};
```


---

## 二、`Race` & `Quorum` —— "第一个达成"族

### 场景

- 多镜像下载：三个镜像同时拉，谁先返回用谁。
- 容错查询：N 个副本，K 个返回即可信。
- UI 加载竞速：网络版本与缓存版本谁先到用谁。
- 故障转移：主节点查询失败立即并行打到从节点。

### 问题

stdexec 的 `when_any` 在 P3149 中存在，但只回答了"取第一个"。真实需求里：

- "取前 K 个值，丢弃剩余" → 需要 `Quorum`；
- "第一个 set_value 算赢，error 不算" → 需要值过滤的 `Race`；
- "赢家取消其他参与者" → 需要 race 自动 stop_request 余下；
- "K-of-N 内含投票/合并" → 需要 reducer。

把这些零碎组合手抄出来错误率极高，尤其是 stop_token 的传播与"赢者通吃"的取消语义。

### 形式与实现

`Race` 是 P3149 `when_any` 的强化版本：第一名落定后**主动**对其他参与者发 stop。op_state 是
变长 tuple；每个分支自带 `inplace_stop_source`，与外部 stop_token 经
`inplace_stop_callback` 串联，赢家落地的 atomic CAS 决定胜负，败者收到 stop。

```cpp
namespace Async::detail {

    // 把变长 sender pack 的 value 类型折叠为共同类型；不一致则 static_assert 失败
    template <stdexec::sender... Ss>
    using common_value_t = /* reflect on completion_signatures<Ss...> */
        std::common_type_t<stdexec::value_types_of_t<Ss, stdexec::__single_t>...>;

    template <class V, std::size_t N>
    struct RaceState {
        std::atomic<std::uint8_t>           winner_{kNone};         // 0..N-1, or kNone
        std::array<inplace_stop_source, N>  childStops_;
        std::optional<V>                    payload_;               // 写者只有赢家，无锁
        std::atomic<std::uint8_t>           completed_{0};          // 收齐 N 个完成才回收
        // 与外部 env 的 stop_token 串联：外部 stop → 全员 child stop
        std::optional<inplace_stop_callback<ExternalStopFn>> outerStopCb_;

        bool TryWin(std::uint8_t k, V v) noexcept {
            std::uint8_t expected = kNone;
            if (!winner_.compare_exchange_strong(expected, k, std::memory_order_acq_rel)) {
                return false;            // 已经有赢家
            }
            payload_.emplace(std::move(v));
            // 给所有非赢家发 stop；它们要么尚未启动（child op_state 自己看到 stop 立即 set_stopped）
            // 要么在飞，受 sender 自身实现的 stop 协议影响
            for (std::uint8_t i = 0; i < N; ++i)
                if (i != k) childStops_[i].request_stop();
            return true;
        }

        void OnChildDone(stdexec::receiver auto& parent) noexcept {
            if (completed_.fetch_add(1, std::memory_order_acq_rel) + 1 == N) {
                // 全部分支结束，向 parent 派发结果
                outerStopCb_.reset();
                if (winner_.load(std::memory_order_acquire) == kNone) {
                    // 全员失败/取消；按 receiver env 的 stop_token 决定 stopped vs error
                    if (stdexec::get_stop_token(stdexec::get_env(parent)).stop_requested())
                        stdexec::set_stopped(std::move(parent));
                    else
                        stdexec::set_error(std::move(parent),
                                           std::make_exception_ptr(RaceAllFailed{}));
                } else {
                    stdexec::set_value(std::move(parent), std::move(*payload_));
                }
            }
        }

        static constexpr std::uint8_t kNone = std::uint8_t(-1);
    };
}

namespace Async {

    template <stdexec::sender... Ss>
    auto Race(Ss... ss) {
        using V = detail::common_value_t<Ss...>;
        return RaceSender<V, Ss...>{std::move(ss)...};
    }
}
```

`RaceSender::connect` 用 *consteval-driven index expansion* 把每个分支 wire 到带索引的
receiver：

```cpp
template <stdexec::receiver R> auto connect(R r) const&& noexcept {
    return [&]<std::size_t... I>(std::index_sequence<I...>) {
        return RaceOpState<R, I...>{ std::move(r), std::move(std::get<I>(senders_))... };
    }(std::make_index_sequence<sizeof...(Ss)>{});
}
```

每个内层 op_state 的 receiver 看到 `set_value` 时调 `state_.TryWin(I, value)`；看到
`set_stopped` 或 `set_error` 都只调 `OnChildDone`。

`Quorum<K>` 是同形态的 K-计数版本：把 `winner_` 换成 `std::array<std::optional<V>, K>` +
`successes_` 计数；第 K 个 set_value 触发 stop 余下、完成。

`QuorumReducer` 概念用于自定义聚合：

```cpp
template <class R, class V>
concept QuorumReducer = requires(R& r, V v) {
    { r.Feed(std::move(v)) } -> std::same_as<bool>;   // true 表示已达成阈值
    { r.Finalize() }         -> std::movable;
};

// 例：MajorityVote
struct MajorityVote {
    std::map<int, int> tally_;
    std::optional<int> winner_;
    bool Feed(int v) noexcept {
        if (++tally_[v] >= /*majority*/ 2) { winner_ = v; return true; }
        return false;
    }
    int Finalize() && noexcept { return *winner_; }
};
```

### 取消语义

赢家落定 → 败者 stop → 等所有败者完成（无论以 set_stopped 还是 set_value）→ 向 parent
set_value。**不**让赢家提前完成，因为那样会让 RaceState 提前析构而败者还在飞。这是
"`when_any` 之外，race 必须自己管"的部分。

外部 stop_token 通过 `outerStopCb_` 桥接到所有 `childStops_`，停机时统一传播。

### C++26 杠杆点

- 共同 value 类型用 `value_types_of_t<S, __single_t>` + `std::common_type_t`，纯
  consteval。不一致时编译期报错而非运行时。
- 无虚函数。每个分支的 receiver 是不同类型；wire-up 在 `connect` 时编译期展开。
- 不需要反射。

---


## 三、`Singleflight<K, V>` —— 在飞请求去重

### 场景

- 缓存击穿：同一 key 的并发请求挤进数据库，N 个等价 query 应当合并成 1 个。
- 配置加载：多线程同时触发 `LoadConfig()`，应共享一次 IO 结果。
- DNS 解析：高并发下同名解析合一。
- 模块懒初始化：N 个 caller 撞到首次 `Init()`，只跑一次、所有人取同一结果。

### 问题

`std::call_once` 是一次性、不可失败重试的；`stdexec::split` 共享一个 sender 的结果，
但要求 caller 已经持有同一个 sender 实例。"按 key 索引在飞工作"是个独立维度——caller
端只有 key、没有现成 sender；框架要负责"已在飞→挂上、未在飞→新建"。手抄出来都是
`unordered_map<K, weak_ptr<shared_state>> + mutex`，错误率高（早析构、生命周期穿透）。

### 形式与实现

核心是一张 `unordered_map<K, SharedState*>` + per-state 引用计数。同 key 的并发请求挂到
同一个 `SharedState`；首位请求负责跑 factory；其他请求做"挂载等待者"。完成后
`SharedState` 把结果广播给所有等待者，再从 map 移除。

```cpp
namespace Async::detail {

    // type-erased 等待者：每个挂载方在自身 op_state 内构造一个，串到 SharedState 链表。
    struct SfWaiterBase {
        SfWaiterBase* next = nullptr;
        virtual void OnValue(const std::any& v) noexcept = 0;
        virtual void OnError(std::exception_ptr) noexcept = 0;
        virtual void OnStopped() noexcept = 0;
    };

    template <class V>
    struct SharedState {
        // 完成态：variant<empty, V, exception_ptr, stopped>
        std::variant<std::monostate, V, std::exception_ptr, struct Stopped {}> outcome_;
        std::atomic<std::uint32_t> refs_{1};      // 1 = factory 一份；waiter 加挂时 +1
        std::atomic<std::uint8_t>  state_{kPending};
        std::atomic_flag           listLock_ = ATOMIC_FLAG_INIT;
        SfWaiterBase*              waiters_ = nullptr;

        enum : std::uint8_t { kPending, kCompleting, kCompleted };

        void AddRef() noexcept { refs_.fetch_add(1, std::memory_order_relaxed); }
        void Release() noexcept {
            if (refs_.fetch_sub(1, std::memory_order_acq_rel) == 1) delete this;
        }
    };
}

namespace Async {

    template <class K, class V, class Hash = std::hash<K>>
    class Singleflight {
    public:
        // 返回 sender<V>。
        // - 同 key 没有在飞工作 → 启动 factory 的 sender；map 内插入新 SharedState。
        // - 同 key 有在飞工作 → 把 op_state 挂到现有 SharedState 的 waiter 链。
        // 完成时把结果分发给所有挂载方，再从 map 移除（GC）。
        template <std::invocable<const K&> Factory>
            requires stdexec::sender_of<std::invoke_result_t<Factory, const K&>,
                                        stdexec::set_value_t(V)>
        [[nodiscard]] auto Do(K key, Factory factory) noexcept;

        void Forget(const K& key) noexcept;
        void Clear() noexcept;

    private:
        std::shared_mutex                                          mtx_;
        std::unordered_map<K, detail::SharedState<V>*, Hash>       inflight_;
    };
}
```

`Do` 的关键路径：

```cpp
template <class K, class V, class Hash>
template <std::invocable<const K&> Factory>
auto Singleflight<K, V, Hash>::Do(K key, Factory factory) noexcept {
    return stdexec::let_value(
        stdexec::just(std::move(key), std::move(factory)),
        [this](K& key, Factory& factory) {
            // 1) 试找现有；找不到则插入并标记自己为 leader
            detail::SharedState<V>* state;
            bool isLeader = false;
            {
                std::unique_lock lk{mtx_};
                if (auto it = inflight_.find(key); it != inflight_.end()) {
                    state = it->second;
                    state->AddRef();
                } else {
                    state = new detail::SharedState<V>{};
                    inflight_.emplace(key, state);
                    isLeader = true;
                }
            }

            // 2) leader 启动 factory；非 leader 挂载等待
            if (isLeader) {
                return RunLeader(state, key, std::move(factory));
            } else {
                return AttachFollower(state);
            }
        });
}
```

leader sender：跑 factory 的 sender，并把每条完成路径折射回 SharedState。

```cpp
template <class V>
auto RunLeader(detail::SharedState<V>* st, auto key, auto factory) noexcept {
    return std::move(factory)(std::as_const(key))
         | stdexec::then([st](V v) noexcept {
               st->outcome_.template emplace<V>(std::move(v));
               PublishCompleted(st);          // 唤醒全员、把 self 从 map 摘除
               return std::get<V>(st->outcome_);
           })
         | stdexec::upon_error([st](std::exception_ptr e) noexcept {
               st->outcome_.template emplace<std::exception_ptr>(e);
               PublishCompleted(st);
               std::rethrow_exception(e);
           })
         | stdexec::let_stopped([st]() noexcept {
               st->outcome_.template emplace<detail::SharedState<V>::Stopped>();
               PublishCompleted(st);
               return stdexec::just_stopped();
           });
}
```

`PublishCompleted` 在 `state_` CAS 到 `kCompleted` 后，把 waiter 链整张取下，逐个分发；
然后从 map 移除 SharedState（leader 释放最后一份引用计数）。错误传播策略写进类型：

```cpp
struct SinkError    { /* 错误广播给所有等待者 */ };
struct RetryOnError { /* 错误只通知 leader；下一次同 key 调用重新 factory */ };

template <class K, class V, class Policy = SinkError, class Hash = std::hash<K>>
class Singleflight;
```

### 示例

```cpp
Async::Singleflight<std::string, ConfigDoc> sf;

auto LoadConfig = [&](std::string name) {
    return sf.Do(std::move(name), [&](const std::string& k) {
        return ReadFile(k) | ParseYaml();      // 只跑一次
    });
};

co_await stdexec::when_all(LoadConfig("a.yaml"),
                           LoadConfig("a.yaml"),
                           LoadConfig("a.yaml"));   // IO 一次，三方同结果
```

### C++26 杠杆点

- *concept 收紧 K*：

```cpp
template <class K>
concept SingleflightKey =
    std::regular<K> && std::movable<K> && requires(const K& k) { std::hash<K>{}(k); };
```

- 错误策略用模板参数（type tag）而非 enum：编译期分支，不同策略不共享代码体积。
- map 的 alloc 用 `std::pmr` 注入；高 QPS 系统可换 concurrent hashmap 后端。这部分通过
  `Hash`/`Bucket` 模板参数对外开放，不强制锁实现。
- 不需要反射。

---


## 四、`Pool<T>` —— 异步资源租借

### 场景

- 数据库连接池
- HTTP/2 连接复用
- GPU command-buffer pool
- 线程池（可视为 Pool<Worker>）
- 大 buffer 池（避免 alloc/free 抖动）

### 问题

每个领域都自己写一份"环形 + 信号量 + 健康检查 + 驱逐"。语义上漂移：有的池在空时阻塞、
有的失败、有的扩容；有的归还时验存活、有的不验。租约外泄（acquire 后忘记 release）
是反复出现的 bug。当前 stdexec 没有任何对 *资源持有* 的统一表述。

### 形式与实现

`Pool` 内部结构：空闲链表 + 等候队列 + 总数计数 + Policy 钩子。`Acquire()` 返回的
sender 完成时给出一个 `Lease<T>`——一个移动语义的 RAII 句柄；析构归还。

```cpp
namespace Async::detail {

    struct PoolWaiterBase {
        PoolWaiterBase* next = nullptr;
        virtual void HandOut(void* obj) noexcept = 0;   // obj 类型由 op_state 还原
        virtual void Cancel() noexcept = 0;
    };

    template <class T>
    struct PoolNode {
        T              value;
        PoolNode*      next;
        std::uint64_t  generation;          // 每次取/还递增；让 Lease 检测 use-after-return
    };
}

namespace Async {

    template <class T> class Lease;

    template <class T, class Policy = DefaultPoolPolicy<T>>
    class Pool {
    public:
        struct Config {
            std::size_t              maxTotal     = 16;
            std::size_t              maxIdle      = 8;
            std::chrono::nanoseconds idleTimeout  = std::chrono::seconds{60};
        };

        template <std::invocable Factory>
        Pool(Config cfg, Factory factory, Policy policy = {}) noexcept;

        // 等价于 Tether(Acquire(), Release)：sender<Lease<T>>
        [[nodiscard]] auto Acquire() noexcept;

        // body : (T&) -> sender<U>
        template <std::invocable<T&> Body>
        [[nodiscard]] auto Use(Body body) noexcept;

        std::size_t Size() const noexcept;
        std::size_t Idle() const noexcept;
        void DrainIdle() noexcept;
        void Close() noexcept;          // 拒绝新 Acquire；让在等的 set_stopped

    private:
        friend class Lease<T>;
        Config                                            cfg_;
        Policy                                            policy_;
        std::function<T()>                                factory_;
        std::mutex                                        mtx_;
        detail::PoolNode<T>*                              idleHead_ = nullptr;
        std::size_t                                       total_    = 0;
        std::size_t                                       idle_     = 0;
        detail::IntrusiveList<detail::PoolWaiterBase>     waiters_;
        bool                                              closed_   = false;

        detail::PoolNode<T>* TryTakeIdleLocked() noexcept;
        bool ReturnNodeLocked(detail::PoolNode<T>*) noexcept;
        void OnRelease(detail::PoolNode<T>*) noexcept;        // Lease 析构调
    };

    template <class T>
    class Lease {
    public:
        Lease() = default;
        Lease(Lease&& o) noexcept : node_(std::exchange(o.node_, nullptr)),
                                    owner_(std::exchange(o.owner_, nullptr)) {}
        Lease& operator=(Lease&& o) noexcept {
            Reset();
            node_ = std::exchange(o.node_, nullptr);
            owner_ = std::exchange(o.owner_, nullptr);
            return *this;
        }
        ~Lease() { Reset(); }

        T& operator*()  noexcept { return node_->value; }
        T* operator->() noexcept { return &node_->value; }

        // 显式提前归还
        void Reset() noexcept {
            if (node_) { owner_->OnRelease(node_); node_ = nullptr; owner_ = nullptr; }
        }

    private:
        template <class, class> friend class Pool;
        detail::PoolNode<T>* node_  = nullptr;
        Pool<T>*             owner_ = nullptr;
    };
}
```

### Policy：annotation 驱动的钩子织入

不同资源对"获取后 / 归还前"要做的事不同：DB 连接需要 ping；HTTP 连接需要看
keep-alive；buffer 需要清零。手写 Policy 类型很啰嗦，annotation 更自然：

```cpp
struct DbConn {
    [[=Async::PoolValidate]]  bool IsAlive() const noexcept;
    [[=Async::PoolReset]]     void Reset() noexcept;
    [[=Async::PoolOnAcquire]] void Touch() noexcept;
};

template <class T>
struct DefaultPoolPolicy {
    // consteval 在实例化时探测 T 上的 annotation，决定是否生成对应钩子调用
    void OnAcquire(T& obj) noexcept {
        if constexpr (Reflect::HasAnnotation<T, PoolOnAcquireTag>)
            Reflect::CallAnnotated<T, PoolOnAcquireTag>(obj);
    }
    bool Validate(T& obj) noexcept {
        if constexpr (Reflect::HasAnnotation<T, PoolValidateTag>)
            return Reflect::CallAnnotated<T, PoolValidateTag>(obj);
        else
            return true;
    }
    void OnRelease(T& obj) noexcept {
        if constexpr (Reflect::HasAnnotation<T, PoolResetTag>)
            Reflect::CallAnnotated<T, PoolResetTag>(obj);
    }
};
```

`Reflect::HasAnnotation<T, Tag>` 是 consteval helper：扫描 `T` 的成员，找带该 annotation
的成员函数；找到就内联调用，找不到就编译期消解为 noop。**没有运行期分支**——一个
`DbConn` 的 Policy 与一个 `int` 的 Policy 在 `OnRelease` 路径上是不同代码体积。

`Acquire` 的 sender：

```cpp
auto Acquire() noexcept {
    return AcquireSender{this};   // sender<Lease<T>>
}

struct AcquireSender {
    Pool<T>* pool_;
    using sender_concept = stdexec::sender_t;
    using completion_signatures = stdexec::completion_signatures<
        stdexec::set_value_t(Lease<T>),
        stdexec::set_stopped_t(),
        stdexec::set_error_t(std::exception_ptr)>;

    template <stdexec::receiver R>
    auto connect(R r) const noexcept -> AcquireOpState<R>;
};
```

`AcquireOpState::start` 的两路：

1. 锁内尝试拿 idle 节点；命中 → policy.Validate → policy.OnAcquire → set_value(Lease)。
2. 锁内创建新节点（total < maxTotal）；同上。
3. 否则把自己挂入 waiters；离锁 → 等回调 / stop。

`Use(body)` 直接基于 `Acquire`：

```cpp
template <std::invocable<T&> Body>
auto Pool<T, Policy>::Use(Body body) noexcept {
    return Acquire()
         | stdexec::let_value([body = std::move(body)](Lease<T>& lease) mutable {
               return body(*lease)
                    | stdexec::then([&lease]<class U>(U&& u) noexcept {
                          // Lease 在外层 let_value 的 op_state 内还活；离作用域归还
                          return std::forward<U>(u);
                      });
           });
}
```

### 与 `Tether` 的关系（澄清非同义反复）

- `Tether<A, R>`: *单一资源*的 RAII 模式（acquire 一次，release 一次）。
- `Pool<T>`: *资源池工厂*，每次 `Acquire()` 产出一个新 `Lease`，可视为"对池的多次 Tether"。

实现上 `Pool::Use` ≡ `Tether(pool.Acquire(), [&](auto& l){ pool.Release(l); }).Use(body)`。
两者形态相同但解决不同的问题：Tether 处理"如何安全消费"，Pool 处理"谁来产、谁来管"。

### 示例

```cpp
Async::Pool<DbConn> conns{
    {.maxTotal = 16, .maxIdle = 8},
    [&]{ return Connect(dsn); }
};

co_await conns.Use([&](DbConn& c) -> Task<Row> {
    co_return co_await c.Query(sql);
});
```

### C++26 杠杆点

- *合适处*用反射：annotation 驱动 Policy 钩子。否则要给每种资源类型手写 PolicyTraits，重复
  且容易漏。
- 不用反射的部分：等候队列（intrusive list）、引用计数、stop_callback 桥接。这些是结构化
  数据，照常写就好。

---


## 五、`Bulkhead<Tag, N>` —— 分舱并发上限

### 场景

- 三方 API 各自限并发：A 服务最多 8 个在飞、B 服务最多 32 个、彼此互不影响。
- 数据库各表并发：分区限流。
- 渲染管线各 pass：上传 pass 与计算 pass 各 4 个并发。
- 失败爆炸隔离：某一类操作堵塞时不波及其他。

### 问题

`Sluice<Credit<N>>` 是 *producer-consumer 通道* 的限流，是 1:1 数据流；`Bulkhead` 是
*sender 提交集合* 的并发上限——不传数据、只控并发度。它和 Sluice 形态接近但语义不
同：Sluice 决定"消息能不能进队列"，Bulkhead 决定"work 能不能立即开始"。

`std::counting_semaphore` 是同步原语；放进 sender 链里要手抄 `let_value_with` + acquire
+ release，并要正确处理 stop 触发时的 release。

### 形式与实现

`Bulkhead` 内部就是一个 *async semaphore*：原子计数 + 等候队列。`Enter(s)` 包成 sender：
入口 acquire 一个 slot，s 的所有完成路径 release。区别于普通信号量的两点：

1. acquire 失败时挂入队列（FIFO）；stop_token 触发 → 出队 + set_stopped。
2. release 路径在 sender 的*所有*完成轨上（value/error/stopped）触发——不能写漏。

```cpp
namespace Async::detail {

    struct BhWaiterBase {
        BhWaiterBase* next = nullptr;
        virtual void Admit() noexcept = 0;
        virtual void Cancel() noexcept = 0;
    };

    class AsyncSemaphore {
    public:
        explicit AsyncSemaphore(std::size_t n) noexcept : free_(n) {}

        // 试图同步获取一个 slot。返回 true 即抢到；false 则需要挂入 q_。
        bool TryAcquire() noexcept {
            for (;;) {
                auto cur = free_.load(std::memory_order_relaxed);
                if (cur == 0) return false;
                if (free_.compare_exchange_weak(cur, cur - 1,
                                                std::memory_order_acq_rel)) return true;
            }
        }

        void Release() noexcept {
            BhWaiterBase* nxt = nullptr;
            {
                std::lock_guard lk{mtx_};
                if (q_.head) {
                    nxt = q_.head;
                    q_.head = nxt->next;
                    if (!q_.head) q_.tail = nullptr;
                } else {
                    free_.fetch_add(1, std::memory_order_acq_rel);
                }
            }
            if (nxt) nxt->Admit();          // 出锁后回调，避免回调中再加锁
        }

        bool Enqueue(BhWaiterBase& w) noexcept {
            std::lock_guard lk{mtx_};
            // 二次检查：避免锁前 Release 与锁后 Enqueue 互错过
            if (free_.load(std::memory_order_acquire) > 0) {
                free_.fetch_sub(1, std::memory_order_acq_rel);
                return false;       // 调用方应直接 Admit 自己
            }
            if (!q_.head) q_.head = &w;
            else          q_.tail->next = &w;
            q_.tail = &w;
            w.next = nullptr;
            return true;
        }

        void Dequeue(BhWaiterBase& w) noexcept;     // O(N)，仅取消路径上调用

    private:
        std::atomic<std::size_t> free_;
        std::mutex               mtx_;
        struct Q { BhWaiterBase* head = nullptr; BhWaiterBase* tail = nullptr; } q_;
    };
}

namespace Async {

    template <auto Tag, std::size_t N>
    class Bulkhead {
    public:
        Bulkhead() noexcept = default;
        Bulkhead(const Bulkhead&) = delete;

        template <stdexec::sender S>
        [[nodiscard]] auto Enter(S s) noexcept;

        std::size_t InFlight() const noexcept { return N - sem_.Free(); }
        std::size_t Queued()   const noexcept { return sem_.Queued(); }

    private:
        detail::AsyncSemaphore sem_{N};
    };
}
```

`Enter(s)` 把"acquire-slot sender"与"用户 sender"链接，再用 `let_value` / `upon_error` /
`let_stopped` 三轨保证 release 一定跑：

```cpp
template <auto Tag, std::size_t N>
template <stdexec::sender S>
auto Bulkhead<Tag, N>::Enter(S s) noexcept {
    return AcquireSender{&sem_}                     // sender<>，slot 抢到则 set_value
         | stdexec::let_value([this, s = std::move(s)]() mutable {
               // 内层 sender：用户工作 + 结束时 release
               return std::move(s)
                    | stdexec::then([this]<class... Vs>(Vs&&... vs) noexcept {
                          sem_.Release();
                          // 把 value 透传：用 tuple<Vs...> 兜住、外层 let_value 解构
                          return Forward<Vs...>(std::forward<Vs>(vs)...);
                      })
                    | stdexec::upon_error([this](std::exception_ptr e) noexcept {
                          sem_.Release();
                          std::rethrow_exception(e);
                      })
                    | stdexec::let_stopped([this]() noexcept {
                          sem_.Release();
                          return stdexec::just_stopped();
                      });
           });
}
```

`AcquireSender` 是一个细粒度的自定义 sender：start 时 `TryAcquire`；命中即同步 set_value；
miss 则把自己挂入 sem_ 队列并安装 stop_callback：

```cpp
template <stdexec::receiver R>
struct AcquireOpState : detail::BhWaiterBase {
    R recv_;
    detail::AsyncSemaphore* sem_;
    std::optional<stdexec::stop_callback_for_t<
        stdexec::stop_token_of_t<stdexec::env_of_t<R>>, StopFn>> cb_;

    friend void tag_invoke(stdexec::start_t, AcquireOpState& self) noexcept {
        if (self.sem_->TryAcquire()) {
            stdexec::set_value(std::move(self.recv_));
            return;
        }
        self.cb_.emplace(stdexec::get_stop_token(stdexec::get_env(self.recv_)),
                         StopFn{&self});
        if (!self.sem_->Enqueue(self)) {     // 锁内二次检查后命中
            self.cb_.reset();
            stdexec::set_value(std::move(self.recv_));
        }
    }
    void Admit() noexcept override {
        cb_.reset();
        stdexec::set_value(std::move(recv_));
    }
    void Cancel() noexcept override {
        sem_->Dequeue(*this);
        stdexec::set_stopped(std::move(recv_));
    }
    struct StopFn { AcquireOpState* self; void operator()() const noexcept { self->Cancel(); } };
};
```

### Tag 的语义价值

Tag 是 NTTP（C++20 起 `auto` NTTP 支持类类型，C++26 加入字符串字面）。`Bulkhead<ApiA{}, 8>`
与 `Bulkhead<ApiB{}, 32>` 是不同的*类型*——这避免了"运行期 string key 区分分舱"的错误模
型，并允许同一 Tag 的多个 Bulkhead 实例共享 N。常见用法是把 Bulkhead 放在 ServiceRegistry
里，按服务标签静态索引：

```cpp
struct ApiA {};   struct ApiB {};
inline Async::Bulkhead<ApiA{}, 8>  apiA;
inline Async::Bulkhead<ApiB{}, 32> apiB;

auto resA = co_await apiA.Enter(CallA(req));
auto resB = co_await apiB.Enter(CallB(req));
```

### 与 `Sluice<Credit<N>>` 的语义边界

- `Sluice<Credit<N>>`: *值流*的限流——每条消息消耗一个 credit，消费侧返还。值与并发度
  *绑定在通道上*。
- `Bulkhead<Tag, N>`: *任务集合*的并发限——slot 与值无关，只与"开始/结束"绑定。

二者在 atomic counter 这一层结构相同，但 Sluice 的 counter 嵌入在 channel 协议里、Bulkhead
的 counter 是独立的。形态相近不等于同义，正如 `mutex` 与 `counting_semaphore<1>` 都用
counter 但语义截然不同。

### C++26 杠杆点

- NTTP `auto Tag`：每条分舱在类型上独立，避免运行期 key dispatch。
- 不需要反射。

---


## 六、`Retry<Backoff>` & `CircuitBreaker` —— 失败-恢复策略

### 场景

- 网络抖动：暂时性失败应当指数退避重试。
- 第三方 API：5xx 重试，4xx 立即失败。
- 故障隔离：连续失败到阈值切到熔断，半开探测后恢复。
- 后台 worker：crash 后等待一段时间再起。

### 问题

每个项目都自己写一遍，各种 bug：忘记尊重 stop_token（停机时还在重试）、忘记区分
"哪些 error 该重试"（4xx 重试是 bug，5xx 不重试也是 bug）、忘记给重试加上 jitter（雷
鸣群效应）。`CircuitBreaker` 状态机三态（closed/open/half-open）每家都重写。

### 形式与实现

`Retry` 是一个 sender adaptor：捕获原 sender 的 *factory*（不是结果），失败时重新跑。
`CircuitBreaker` 是一个有状态的 *gate*：根据近期失败率决定要不要让请求过去。两者形态独立、
组合自由。

```cpp
namespace Async {

    template <class B>
    concept Backoff = requires(B& b, std::size_t attempt) {
        { b.Next(attempt) } -> std::same_as<std::optional<std::chrono::nanoseconds>>;
    };

    // 退避策略
    struct ExponentialJitter {
        std::chrono::nanoseconds base;
        std::chrono::nanoseconds cap;
        std::uint32_t            seed = 0xA5A5A5A5;

        std::optional<std::chrono::nanoseconds> Next(std::size_t attempt) noexcept {
            if (attempt > 16) return std::nullopt;     // 上限
            auto exp = std::min(cap, base * (1ull << std::min<std::size_t>(attempt, 30)));
            // Decorrelated jitter (AWS 推荐)：unif(base, prev*3)
            return std::chrono::nanoseconds{Rand(seed, base.count(), exp.count() * 3)};
        }
    };
    struct FixedDelay { std::chrono::nanoseconds d; std::size_t maxAttempts;
        std::optional<std::chrono::nanoseconds> Next(std::size_t a) noexcept {
            return a < maxAttempts ? std::optional{d} : std::nullopt;
        }
    };

    // 错误判别
    template <class P, class E>
    concept RetryPredicate = requires(P& p, E e) {
        { p(e) } -> std::same_as<bool>;     // true = 重试
    };
    inline constexpr auto AnyError = [](std::exception_ptr) noexcept { return true; };
    inline constexpr auto Never    = [](auto&&) noexcept { return false; };

    template <Backoff B, class Predicate, class Sched, stdexec::sender_factory F>
    [[nodiscard]] auto Retry(B backoff, Predicate pred, Sched sched, F factory);
}
```

注意 *sender_factory*——重试需要重新启动一次工作，所以入参不是已经 connect 的 sender，
而是产生 sender 的可调用对象。把这点写进类型避免误用：

```cpp
template <class F>
concept SenderFactory = requires(F& f) {
    { f() } -> stdexec::sender;
};
```

实现走"递归 let_value"：

```cpp
template <Backoff B, class Predicate, class Sched, SenderFactory F>
auto Retry(B backoff, Predicate pred, Sched sched, F factory) {
    return stdexec::let_value(
        stdexec::just(std::size_t{0}),
        [backoff = std::move(backoff),
         pred    = std::move(pred),
         sched   = std::move(sched),
         factory = std::move(factory)](std::size_t& attempt) mutable {
            // 单次尝试 + 失败回到 let_value 自身
            auto loop = [&](auto& self) -> stdexec::sender auto {
                return factory()
                     | stdexec::let_error([&](std::exception_ptr e) {
                           if (!pred(e)) std::rethrow_exception(e);
                           auto wait = backoff.Next(attempt++);
                           if (!wait) std::rethrow_exception(e);
                           return stdexec::schedule_after(sched, *wait)
                                | stdexec::let_value([&]{ return self(self); });
                       });
            };
            return loop(loop);
        });
}
```

> 这里要的是 `Y combinator` 形态，让 lambda 自调用；C++26 之前用辅助 fixed_point 模板。

`CircuitBreaker` 三态机：

```cpp
namespace Async {

    class CircuitBreaker {
    public:
        struct Config {
            std::size_t              failureThreshold;     // 连败 N 次 → open
            std::chrono::nanoseconds openTimeout;          // open 持续时长
            std::size_t              halfOpenProbes;       // half-open 期允许的探测请求数
            std::size_t              successesToClose;     // half-open 中累计成功数 → close
        };

        explicit CircuitBreaker(Config cfg) noexcept;

        template <stdexec::sender S>
        [[nodiscard]] auto Guard(S s) noexcept;

        enum class State : std::uint8_t { Closed, Open, HalfOpen };
        State CurrentState() const noexcept;

    private:
        Config cfg_;
        std::atomic<State>                                    state_{State::Closed};
        std::atomic<std::size_t>                              consecFails_{0};
        std::atomic<std::chrono::steady_clock::time_point>    openedAt_{};
        std::atomic<std::size_t>                              halfOpenInflight_{0};
        std::atomic<std::size_t>                              halfOpenSuccesses_{0};

        // 状态转换：Closed→Open  (consecFails_ >= threshold)
        //          Open  →HalfOpen (now - openedAt_ >= openTimeout)
        //          HalfOpen→Closed (halfOpenSuccesses_ >= successesToClose)
        //          HalfOpen→Open   (任何失败)
        bool TryAdmit() noexcept;       // 决定要不要放行；half-open 限流
        void OnSuccess() noexcept;
        void OnFailure() noexcept;
    };

    struct BreakerOpen final : std::runtime_error {
        BreakerOpen() : std::runtime_error{"circuit breaker open"} {}
    };
}
```

`Guard(s)` 实现：

```cpp
template <stdexec::sender S>
auto CircuitBreaker::Guard(S s) noexcept {
    return stdexec::let_value(stdexec::just(), [this, s = std::move(s)]() mutable {
        if (!TryAdmit()) {
            return stdexec::just_error(std::make_exception_ptr(BreakerOpen{}))
                 | stdexec::__as_sender_of_S<S>{};       // 折回兼容签名
        }
        return std::move(s)
             | stdexec::then([this]<class... Vs>(Vs&&... vs) noexcept {
                   OnSuccess();
                   return Forward<Vs...>(std::forward<Vs>(vs)...);
               })
             | stdexec::upon_error([this](std::exception_ptr e) noexcept {
                   OnFailure();
                   std::rethrow_exception(e);
               })
             | stdexec::let_stopped([this]() noexcept {
                   // stopped 不算失败，也不算成功；half-open 计数回滚
                   OnNeutral();
                   return stdexec::just_stopped();
               });
    });
}
```

### 组合

`Retry` 与 `Breaker` 都是 sender adaptor，组合是直接的：

```cpp
auto resilient =
    Async::Retry(
        Async::ExponentialJitter{50ms, 5s},
        [](std::exception_ptr e){ return Is5xx(e); },
        timer,
        [&]{ return breaker.Guard(CallApi(req)); });

auto resp = co_await resilient;
```

注意顺序：Breaker 在内、Retry 在外——Breaker open 时立即抛出，Retry 看到 BreakerOpen
按 predicate 决定是否重试（通常不该重试 BreakerOpen，因为它就是设计来快速失败的）。

### 与 stop_token 的协作

- Retry 在 backoff 等待期间通过 `schedule_after` 自动尊重 stop_token——`schedule_after`
  本身就是 stop-aware 的 sender，停机时直接 set_stopped 不再继续重试。
- Breaker 不持有 stop，只观察 sender 的完成轨。

### C++26 杠杆点

- `Backoff` / `RetryPredicate` 是 concept，编译期约束策略类型；不同策略生成的 op_state
  形态不同。
- 不需要反射。`ExponentialJitter` 这类策略写成 *值类型*（小、可拷贝、无虚函数）即可。

---


## 七、`Pipe<Stages...>` —— 静态拓扑数据管线

### 场景

- 视频编解码：解封装 → 解码 → 滤镜 → 编码 → 封装。
- 编译管线：词法 → 语法 → 语义 → IR → 优化 → 后端。
- 数据 ETL：source → transform → sink，每段独立速率。
- 渲染：culling → command-record → submit。

### 问题

`AsyncSeq` 是单链拉式序列；多阶段并行 + 各阶段独立背压 + 各阶段独立调度的"管线"是
另一个层次的实体。手抄出来都是 N 个 `Stream<T>` + N 个 worker，绕得很复杂；阶段间速
率不匹配时常常死锁或丢数据。

### 形式与实现

`Pipe<Stages...>` 把 N 个 stage 函数组合成一条多阶段管道，**阶段间各自带缓冲、各自可
独立调度**。这是 reflection 真正发挥的地方：编译期校验邻接阶段的输入/输出类型对接，标
注每段的目标 scheduler，自动织入 `continues_on`。

#### 1. Stage 概念与类型对接

```cpp
namespace Async {

    template <class S>
    concept Stage = requires(S& s, typename S::input_type in) {
        typename S::input_type;
        typename S::output_type;
        { s(std::move(in)) } -> stdexec::sender_of<
            stdexec::set_value_t(typename S::output_type)>;
    };

    // 对纯 lambda 的适配：从 operator() 反射推断 input/output
    template <class F>
    struct LambdaStage {
        F fn;
        using input_type  = /* reflect: first parameter type of operator() */;
        using output_type = /* reflect: value_type of return-type sender */;

        auto operator()(input_type in) noexcept { return fn(std::move(in)); }
    };
}
```

`input_type` / `output_type` 用 reflection 提取：

```cpp
namespace Async::detail {

    consteval auto stage_input_of(std::meta::info F) {
        // F 是某个仿函数类型；取其 operator()(X) 的 X
        constexpr auto opcall = members_of(F, std::meta::is_function)
                              | std::views::filter(/* identifier == "operator()" */)
                              | first();
        return parameters_of(opcall)[0].type();
    }
    consteval auto stage_output_of(std::meta::info F) {
        // 取 operator() 的返回类型；它必须是某个 sender；从 completion_signatures
        // 里抓 set_value_t(X) 的 X
        constexpr auto ret = return_type_of(/*opcall*/);
        return single_value_type_of(ret);          // helper：拆 stdexec::value_types_of_t
    }
}
```

这个推断在 *构造时一次性*完成；运行期没有任何反射开销。

#### 2. 邻接对接的编译期检查

```cpp
template <Stage... Ss>
consteval bool ValidatePipeChain() {
    constexpr std::array stages = { ^^Ss... };
    for (std::size_t i = 0; i + 1 < stages.size(); ++i) {
        // Stages[i].output_type == Stages[i+1].input_type
        if (!std::is_same_v<
                [: type_of(member_of(stages[i],     "output_type")) :],
                [: type_of(member_of(stages[i + 1], "input_type"))  :]>)
        {
            return false;     // 触发 static_assert 失败，错误消息指向不匹配的两段
        }
    }
    return true;
}

template <Stage... Ss>
class Pipe {
    static_assert(ValidatePipeChain<Ss...>(),
                  "Pipe stage chain has an input/output type mismatch; "
                  "see consteval diagnostic for offending pair.");
public:
    using input_type  = typename std::tuple_element_t<0, std::tuple<Ss...>>::input_type;
    using output_type = typename std::tuple_element_t<sizeof...(Ss) - 1,
                                                      std::tuple<Ss...>>::output_type;
};
```

这把"管道串接错位"从运行期段错误变成编译期错误，错误消息直指具体两段。

#### 3. 阶段调度的 annotation 织入

不同 stage 可能要在不同 scheduler 上跑（IO 在 io_pool、CPU-bound 在 cpu_pool）。用
annotation 标注，reflection 在构造时读出来：

```cpp
inline constexpr struct OnIo  {} kOnIo;
inline constexpr struct OnCpu {} kOnCpu;

template <auto Sched, class F>
struct StageWithSched : LambdaStage<F> {
    static constexpr auto kSched = Sched;
};

// 用户写：
auto pipe = Async::MakePipe(
    [[=kOnIo ]] [](Packet p) -> Task<Frame> { co_return co_await Decode(p); },
    [[=kOnCpu]] [](Frame f)  -> Task<Frame> { co_return Filter(f); },
    [[=kOnIo ]] [](Frame f)  -> Task<Packet> { co_return co_await Encode(f); });
```

`MakePipe` 在实例化时为每段调用 `continues_on(...)`：

```cpp
namespace Async::detail {

    template <class Stage, class Input>
    auto WrapStage(Stage& stage, Input in, auto& schedReg) {
        if constexpr (HasAnnotation<Stage, OnIo>)
            return stdexec::continues_on(stdexec::just(std::move(in)), schedReg.io)
                 | stdexec::let_value([&stage](Input& v){ return stage(std::move(v)); });
        else if constexpr (HasAnnotation<Stage, OnCpu>)
            return stdexec::continues_on(stdexec::just(std::move(in)), schedReg.cpu)
                 | stdexec::let_value([&stage](Input& v){ return stage(std::move(v)); });
        else
            return stdexec::just(std::move(in))
                 | stdexec::let_value([&stage](Input& v){ return stage(std::move(v)); });
    }
}
```

#### 4. 阶段间缓冲（独立背压）

每个相邻阶段对之间塞一个 `Stream<T>`（前文原语之一）；前段把输出 push 进，后段从中
pull——前段快后段慢时前段被自然背压（Stream 满则 Push set_value 延迟）。这把 Pipe
的"独立速率"语义落到了具体数据结构上，不依赖额外协调。

```cpp
template <Stage... Ss>
class Pipe {
public:
    Pipe(SchedRegistry sr, Ss... stages) noexcept;

    [[nodiscard]] auto Push(input_type v) noexcept;       // sender<>
    [[nodiscard]] auto Pull()             noexcept;       // sender<output_type>

    // 启动每个 stage 的 worker 协程；返回的 sender 在 Close() 后所有 stage 排空才完成。
    [[nodiscard]] auto Run() noexcept;

    void Close() noexcept;                                // 不再接受 Push；让 worker 收尾

private:
    SchedRegistry                                       sched_;
    std::tuple<Ss...>                                   stages_;
    // N-1 个内部 Stream，类型按 stages 的 input/output 推
    detail::PipeBuffers<Ss...>                          buffers_;
};
```

`PipeBuffers<Ss...>` 也用 reflection 推：相邻段 `Stages[k].output == Stages[k+1].input`，
buffer 类型取该 input/output 类型。`std::tuple<Stream<T1>, Stream<T2>, ...>`。

`Run()` 用 `when_all` 把 N 个 stage worker 汇聚：

```cpp
auto Pipe::Run() noexcept {
    return [&]<std::size_t... I>(std::index_sequence<I...>) {
        return stdexec::when_all(StageWorker<I>()...);
    }(std::make_index_sequence<sizeof...(Ss)>{});
}

template <std::size_t I> auto StageWorker() noexcept {
    return [this]() -> Task<> {
        for (;;) {
            auto opt = co_await buffers_.template In<I>().Next();
            if (!opt) co_return;        // 上游关闭
            auto out = co_await detail::WrapStage(get<I>(stages_), *std::move(opt), sched_);
            if (!buffers_.template Out<I>().Emit(std::move(out))) co_return;  // 下游关闭
        }
    }();
}
```

### 示例

```cpp
struct SchedRegistry {
    Async::Scheduler io;
    Async::Scheduler cpu;
};

auto pipe = Async::MakePipe(SchedRegistry{ioPool, cpuPool},
    [[=kOnIo ]] [](RawPacket p) -> Task<Packet>   { co_return co_await Demux(p); },
    [[=kOnIo ]] [](Packet p)    -> Task<Frame>    { co_return co_await Decode(p); },
    [[=kOnCpu]] [](Frame f)     -> Task<Frame>    { co_return Filter(f); },
    [[=kOnIo ]] [](Frame f)     -> Task<Packet>   { co_return co_await Encode(f); },
    [[=kOnIo ]] [](Packet p)    -> Task<RawPacket>{ co_return co_await Mux(p); });

scope.Spawn(pipe.Run());

co_await pipe.Push(rawIn);
auto out = co_await pipe.Pull();
```

### C++26 杠杆点

- 反射用得最深的一个原语。output→input 对接验证、stage worker 的索引展开、annotation 驱
  动调度器选择，三件事都*否则要让用户手抄一份目录*；反射让目录直接来自类型本身。
- `template for` over `std::index_sequence<...>`：每个 stage worker 是不同协程，没有 type
  erasure。
- 错误聚合策略写成模板参数 *策略对象*：FailFast（任一 stage error 全管道 stop）/ DropAndContinue
  （记录但继续）/ Restart（重启出错段）。

---


## 八、`Topic<T>` —— 多生产者多消费者发布订阅

### 场景

- 事件总线：UI 事件、ECS 事件、领域事件。
- 监控指标分发：N 个采集源，M 个观察者。
- 日志多通道分发：app 日志同时进文件、Console、远程聚合。
- ECS 系统间通信。

### 问题

`Stream<T>` 是 SPSC——单生产单消。`Topic<T>` 是 MPMC，且语义上偏重 *广播* 而非 *消
费*：每个订阅者独立消费完整事件流，订阅者断流不影响其他订阅者。这里不能用 Sluice
（非广播），不能用 Stream（不支持多消费者各拥独立游标）。手抄都是 `mutex + vector<callback>`
然后到处忘记拷贝、忘记移除、忘记同步，bug 集中地。

### 形式与实现

`Topic<T>` 与 `Stream<T>`（前文中作为 EventChannel 的内核）的本质差别：Stream 单消费者
独占游标；Topic 每个订阅者拿到一个**独立游标**，订阅者间互不干扰。常见做法是单写多读
环 + 各订阅者 cursor + 留存策略。

```cpp
namespace Async::detail {

    // 环形缓冲：单写者（Publish）多读者（Subscription）；写者独占 head_，读者各持
    // 自己的 cursor。版本号用 generation 防 ABA。
    template <class T, std::size_t Capacity>
    class TopicRing {
    public:
        struct Slot {
            std::atomic<std::uint64_t> generation;     // 偶数 = 空闲；奇数 = 写中
            T                          value;
        };

        // 单写：先把目标 slot 的 generation +1（odd=写中）→ 写值 → +1（even=可读）
        bool Publish(T v, std::uint64_t& outSeq) noexcept {
            auto seq = nextSeq_.fetch_add(1, std::memory_order_acq_rel);
            auto& s  = slots_[seq % Capacity];
            // 等读者把这个 slot 让出（防止覆盖未读消息——按策略决定）
            // ...策略点 1：覆盖 vs 阻塞写
            s.generation.store(2 * seq + 1, std::memory_order_release);  // 写中
            s.value = std::move(v);
            s.generation.store(2 * seq + 2, std::memory_order_release);  // 完成
            outSeq = seq;
            return true;
        }

        // 读者：按自己的 cursor seq 读对应 slot。如果 slot 的 generation 已超过 seq*2+2，
        // 说明该 slot 已被新一轮覆盖；按策略决定是返回 MissedCount 还是 set_stopped。
        std::optional<T> Read(std::uint64_t seq) noexcept;

    private:
        alignas(64) std::array<Slot, Capacity> slots_{};
        alignas(64) std::atomic<std::uint64_t> nextSeq_{0};
    };
}

namespace Async {

    template <class T, class RetentionPolicy = LiveOnly>
    class Topic {
    public:
        explicit Topic(RetentionPolicy = {}) noexcept;

        bool Publish(T value) noexcept;       // 任意线程；策略决定满时行为

        // 返回 sender<Subscription<T>>。订阅者起点由 RetentionPolicy 决定：
        //   LiveOnly: 当前 nextSeq_
        //   LastN:    nextSeq_ - N（取既存最近 N 条）
        //   Replay:   0（看到全量；GC 由策略管）
        [[nodiscard]] auto Subscribe() noexcept;

        std::size_t SubscriberCount() const noexcept;
    };

    // Subscription：每个订阅者独立游标
    template <class T>
    class Subscription {
    public:
        // sender<std::optional<T>>：nullopt 表示流被 Close 或自身被 stop
        [[nodiscard]] auto Next()      noexcept;
        [[nodiscard]] auto NextBatch() noexcept;       // sender<BatchView<T>>
        std::size_t MissedCount() const noexcept;       // 慢消费者计数

    private:
        Topic<T>*           topic_;
        std::uint64_t       cursor_;
        std::uint64_t       missed_ = 0;
    };
}
```

### 留存策略

```cpp
namespace Async {

    // 订阅之后才能收到；订阅之前的消息看不见。无额外存储。
    struct LiveOnly {
        template <class T> static std::uint64_t StartCursor(const Topic<T>& t) noexcept {
            return t.NextSeq();
        }
        // 写者覆盖空 slot 时不阻塞——LiveOnly 容忍丢失
        static constexpr bool kBlockOnFull = false;
    };

    // 订阅时立即重放最近 N 条；环大小至少 N。
    template <std::size_t N>
    struct LastN {
        template <class T> static std::uint64_t StartCursor(const Topic<T>& t) noexcept {
            auto cur = t.NextSeq();
            return cur > N ? cur - N : 0;
        }
        static constexpr bool kBlockOnFull = false;
    };

    // 全量重放；带显式 GC（按订阅者最小 cursor 推进）。
    struct Replay {
        template <class T> static std::uint64_t StartCursor(const Topic<T>&) noexcept { return 0; }
        static constexpr bool kBlockOnFull = true;
    };
}
```

`kBlockOnFull` 决定写者在所有订阅者都还没消费前如何处理：LiveOnly/LastN 直接覆盖（订阅
者下次 Next 时自增 missed_）；Replay 阻塞写者直到最慢订阅者推进。

### `Subscribe` 与 `Next` 的实现

```cpp
template <class T, class Policy>
auto Topic<T, Policy>::Subscribe() noexcept {
    return stdexec::just(this) | stdexec::then([](Topic<T, Policy>* t) {
        Subscription<T> sub;
        sub.topic_  = t;
        sub.cursor_ = Policy::template StartCursor<T>(*t);
        std::scoped_lock lk{t->subMtx_};
        t->subs_.push_back(&sub);                    // 用于 Close 全员通知
        return sub;
    });
}

template <class T>
auto Subscription<T>::Next() noexcept {
    return stdexec::let_value(stdexec::just(), [this]() noexcept -> stdexec::sender auto {
        // fast path：已有可读条目
        if (auto v = topic_->Ring().Read(cursor_)) {
            ++cursor_;
            return stdexec::just(std::optional<T>{std::move(*v)});
        }
        // slow path：挂入 topic_->waiters_，写者 Publish 完后唤醒
        return WaitForNextSender{this};
    });
}
```

`WaitForNextSender::start` 安装 stop_callback、把自己挂入 topic 的 waiters。Publish 完
成后 splice 出整张 waiter 表统一唤醒。

### 与 Stream / Channel 的边界（再次澄清）

- `Stream<T>` (SPSC)：一个生产者、一个消费者；游标隐含在通道内部。EventPump 用它。
- `Topic<T>` (单写多读)：一个生产者、N 个消费者各自带游标；Replay/LastN 让"晚来的"看
  见历史。事件总线、metrics 分发用它。
- `Channel<T>` (MPMC 双向)：N 写 N 读；关闭是一等公民；select 在多 channel 上等任一就
  绪。actor 通信用它。

三者形态相近，但订阅者数量、关闭语义、游标可见性各不同；分别命名后，组合使用时不会
混淆"该选哪个"。

### C++26 杠杆点

- `RetentionPolicy` 是 type tag，编译期分支：LiveOnly 不分配 ring（用 link-cut tree 简
  化）、LastN 用定长 ring、Replay 用动态 ring；不同策略生成不同代码体积。
- `kBlockOnFull` 是 `static constexpr bool`，不是运行期 if——写者路径在 LiveOnly 与
  Replay 下是不同代码。
- 不需要反射。

---


## 九、`Channel<T>` —— 异步双向通道

### 场景

- Goroutine 风格的并发通信。
- actor 间消息传递。
- 协程间生产者-消费者。
- 取消信号通道（`Channel<Unit>` 关闭即广播）。

### 问题

Stream 是单向 SPSC；Topic 是 MPMC 广播；都不是 Go 那种"既能 send 也能 recv，关闭可
观察"的通道。Channel 与 Stream 看似重叠，但语义独立：Channel 强调 *双方对等通信*，关
闭信号是一等公民；Stream 强调 *单向数据流*，关闭隐含在 stop_token。

### 形式与实现

`Channel<T, Capacity>` 内部是有界缓冲 + 两侧等候队列：发送队列（缓冲满时挂这）、接收
队列（缓冲空时挂这）。关闭是一等公民——`Close()` 后所有未完成的 send/recv 都得到确定结
局。`Capacity = 0` 触发 *rendezvous*（同步交接）模式。

```cpp
namespace Async::detail {

    struct ChWaiterBase {
        ChWaiterBase* next = nullptr;
        virtual void Wake() noexcept = 0;        // value/admit
        virtual void NotifyClosed() noexcept = 0;
        virtual void Cancel() noexcept = 0;
    };

    template <class T, std::size_t Capacity>
    class ChannelCore {
    public:
        bool TryPush(T& v) noexcept;             // 缓冲非满时直接入；满则 false
        bool TryPop (T& out) noexcept;
        bool TryRendezvous(T& v) noexcept;       // Capacity == 0：直接对接接收方

        // 加锁版本：把等候挂入；调用方持有 op_state，自身负责构造 ChWaiterBase
        bool EnqueueSender  (ChWaiterBase&, T& slot) noexcept;
        bool EnqueueReceiver(ChWaiterBase&, T& slot) noexcept;
        void DequeueSender  (ChWaiterBase&) noexcept;
        void DequeueReceiver(ChWaiterBase&) noexcept;

        void Close() noexcept;            // 把两侧 waiters 全 NotifyClosed；之后 send 必败、recv 排空后空返回
        bool IsClosed() const noexcept;

    private:
        std::mutex                    mtx_;
        RingBuffer<T, Capacity>       buf_;
        IntrusiveList<ChWaiterBase>   sendQ_;
        IntrusiveList<ChWaiterBase>   recvQ_;
        bool                          closed_ = false;
    };
}

namespace Async {

    template <class T, std::size_t Capacity = 0>
    class Channel {
    public:
        // sender<>: 缓冲满时挂起；缓冲空 + 关闭时直接 set_error(Closed)；stop → set_stopped
        [[nodiscard]] auto Send(T value) noexcept;

        // sender<std::optional<T>>: 缓冲空时挂起；关闭后排空缓冲再返回 nullopt
        [[nodiscard]] auto Recv() noexcept;

        bool TrySend(T& value) noexcept;
        bool TryRecv(T& out)   noexcept;

        void Close() noexcept;
        bool IsClosed() const noexcept;

    private:
        detail::ChannelCore<T, Capacity> core_;
    };

    struct ChannelClosed final : std::runtime_error {
        ChannelClosed() : std::runtime_error{"channel closed"} {}
    };
}
```

### `Send` / `Recv` 的 op_state

```cpp
template <class T, std::size_t Cap, stdexec::receiver R>
struct SendOpState : detail::ChWaiterBase {
    R                              recv_;
    detail::ChannelCore<T, Cap>*   core_;
    T                              slot_;          // 待发值；唤醒时由发送方移走
    std::optional<stdexec::stop_callback_for_t<
        stdexec::stop_token_of_t<stdexec::env_of_t<R>>, StopFn>> cb_;

    friend void tag_invoke(stdexec::start_t, SendOpState& self) noexcept {
        if (self.core_->IsClosed()) {
            stdexec::set_error(std::move(self.recv_),
                               std::make_exception_ptr(ChannelClosed{}));
            return;
        }
        if (self.core_->TryPush(self.slot_)) {
            stdexec::set_value(std::move(self.recv_));
            return;
        }
        // 否则挂入 sendQ_
        self.cb_.emplace(stdexec::get_stop_token(stdexec::get_env(self.recv_)),
                         StopFn{&self});
        if (!self.core_->EnqueueSender(self, self.slot_)) {
            // 锁内二次检查命中
            self.cb_.reset();
            stdexec::set_value(std::move(self.recv_));
        }
    }
    void Wake()         noexcept override { cb_.reset(); stdexec::set_value(std::move(recv_)); }
    void NotifyClosed() noexcept override { cb_.reset(); stdexec::set_error(std::move(recv_),
                                                std::make_exception_ptr(ChannelClosed{})); }
    void Cancel()       noexcept override { core_->DequeueSender(*this);
                                            stdexec::set_stopped(std::move(recv_)); }
    struct StopFn { SendOpState* self; void operator()() const noexcept { self->Cancel(); } };
};
```

`RecvOpState` 对称：缓冲非空时直接 pop；空但 closed 则 `set_value(nullopt)`；空且未关闭
则挂入 `recvQ_`。

### `select` —— 多 channel 多路等待

```cpp
namespace Async {

    // Case 描述：哪个 channel、做什么操作（Send 还是 Recv）
    template <class Op, class Ch> struct CaseImpl;

    template <class T, std::size_t Cap>
    auto OnRecv(Channel<T, Cap>& ch) {
        return CaseImpl<RecvOp, Channel<T, Cap>>{ &ch };
    }
    template <class T, std::size_t Cap>
    auto OnSend(Channel<T, Cap>& ch, T v) {
        return CaseImpl<SendOp, Channel<T, Cap>>{ &ch, std::move(v) };
    }

    // 返回一个 sender；其 set_value 是 std::variant<Cases 的结果...>
    template <class... Cases>
    [[nodiscard]] auto Select(Cases... cases);
}
```

实现关键：每个 case 各自构造一个内嵌 op_state；启动时同时尝试 fast-path（所有 case 都
TrySend/TryRecv）；都失败则全部挂入对应队列；任一被唤醒后**主动**把其它 case 从队列摘
除（dequeue）并向 parent 派发结果。所以 `Select` 的 op_state 是定长 tuple，构造时由
reflection 展开各 case 的 op_state 类型——这是反射的合适点：让 `select` 的 op_state 大
小由 case 数量精确决定，不走 type erasure。

```cpp
template <class... Cases>
struct SelectOpState {
    std::tuple<typename Cases::template Op<Cases>...> ops_;
    std::atomic_flag winner_ = ATOMIC_FLAG_INIT;     // 第一个 wake 的 case 抢到；其它退出

    template <std::size_t I, class Result>
    void OnCaseWin(Result&& r) noexcept {
        if (winner_.test_and_set(std::memory_order_acq_rel)) return;  // 已有人赢
        // 把其它 ops_ 从队列摘除
        [&]<std::size_t... J>(std::index_sequence<J...>) {
            ((J != I ? std::get<J>(ops_).Detach() : void()), ...);
        }(std::make_index_sequence<sizeof...(Cases)>{});
        stdexec::set_value(std::move(parent_),
                           std::variant<typename Cases::Result...>{
                               std::in_place_index<I>, std::forward<Result>(r)});
    }
};
```

### 示例

```cpp
Async::Channel<Job, 16>      jobs;
Async::Channel<Async::Unit>  cancel;

scope.Spawn([&]() -> Task<> {
    for (;;) {
        auto pick = co_await Async::Select(
            Async::OnRecv(jobs),
            Async::OnRecv(cancel));
        if (pick.index() == 1) co_return;        // 收到取消
        Process(std::get<0>(pick).value());
    }
}());
```

`Channel<Unit>` 关闭即广播取消：所有 `Recv()` 排空后看到 `nullopt`，作为"无消息且通道
已关"的明确信号。这比共享一个 `atomic<bool> stopped` 信息密度更高（"未来不会再有消息"
比"现在被请求停"更精确）。

### 与 stop_token 的关系

- `Channel::Close()` 是显式的 *协议级*关闭：把"未来不会再有消息"作为业务事实编码进通道。
- `stop_token` 是 *fabric 级*取消：影响在飞 op_state 的完成形态。
- 两者正交、各司其职。一个 sender 既可能被 stop_token 取消，也可能因为通道被 Close 而
  得到 nullopt。

### C++26 杠杆点

- *合适处*用反射：`Select` 的 op_state tuple 由 case pack 编译期展开，避免 type erasure。
- `Capacity = 0` 是 NTTP 分支：rendezvous 模式不分配 ring，走 `TryRendezvous` 的直接对接
  路径。

---


## 十、`Saga<Steps...>` —— 补偿事务

### 场景

- 分布式事务：跨多个服务的"看似 ACID"操作，没有 2PC 时用补偿模式。
- 业务流：下单 → 扣库存 → 扣款 → 发货；任何一步失败前序步骤要逆操作。
- 配置变更：N 个组件依次应用，任一失败回滚已应用的。
- 资源迁移：跨节点迁移失败时复原原状。

### 问题

补偿操作的写法历来是噩梦：每步要写 do + undo 两个函数，失败时要按相反顺序调用已成
功步骤的 undo。手抄的代码极易在异常路径上漏调用 undo、或者调用顺序错误。

### 形式与实现

`Saga` 把"do/undo 对偶"从开发者纪律提升为类型契约。三种构造路径：

1. **手写 Step 列表**——直接给出 forward + compensate 对（最一般形式）。
2. **从类反射**——用 annotation 标注成员函数，reflection 自动配对。
3. **链式 builder**——更接近 DSL 的写法。

```cpp
namespace Async {

    template <class Forward, class Compensate>
    struct Step {
        Forward      forward;       // () -> sender<V_k>
        Compensate   compensate;    // (V_k) -> sender<>     // 收到 forward 的产出，做反向
    };

    template <class... Steps>
    class Saga {
    public:
        explicit Saga(Steps... steps) noexcept : steps_(std::move(steps)...) {}

        // sender<>: 顺序执行 forward；任一失败 → 反序对已成功步骤跑 compensate。
        // 主错误（首个 forward error）优先抛出；compensate 自身 error 聚合到 SagaCompensationError。
        [[nodiscard]] auto Run() noexcept;

    private:
        std::tuple<Steps...> steps_;
    };

    struct SagaCompensationError final : std::runtime_error {
        std::exception_ptr             primary;       // 主错误（forward 失败）
        std::vector<std::exception_ptr> compensations; // compensate 中又失败的
        SagaCompensationError() : std::runtime_error{"saga compensation failed"} {}
    };
}
```

`Run()` 的实现用 *可变形态的 `let_value` 链*：每跑完一步把 forward 的结果存到一个状态
对象，到失败点回滚。结果存储要按步骤索引，所以最自然的写法是 op_state 内嵌的 tuple：

```cpp
template <class... Steps>
auto Saga<Steps...>::Run() noexcept {
    return RunSender<Steps...>{ &steps_ };
}

template <class... Steps>
struct RunSender {
    std::tuple<Steps...>* steps_;
    using sender_concept = stdexec::sender_t;
    using completion_signatures = stdexec::completion_signatures<
        stdexec::set_value_t(),
        stdexec::set_error_t(std::exception_ptr),
        stdexec::set_stopped_t()>;

    template <stdexec::receiver R>
    auto connect(R r) const noexcept;
};

template <class... Steps>
template <stdexec::receiver R>
struct RunOpState {
    R                                                   recv_;
    std::tuple<Steps...>*                               steps_;

    // 每个 forward 的产出值；只有跑过的步骤里有值（用 optional 承载）
    using ResultsTuple = std::tuple<std::optional<
        stdexec::value_types_of_t<
            std::invoke_result_t<typename Steps::Forward>,
            stdexec::__single_t>>...>;
    ResultsTuple    results_;
    std::size_t     committed_ = 0;       // 已成功提交的步骤数
    std::exception_ptr primaryError_;
    std::vector<std::exception_ptr> compensateErrors_;

    void StartForward() noexcept;
    void OnForwardSuccess(std::size_t i, auto v) noexcept;
    void OnForwardError(std::size_t i, std::exception_ptr) noexcept;
    void RollbackFrom(std::size_t i) noexcept;     // 反序跑 compensate[i-1..0]
};
```

forward 链和 rollback 链都通过 `template for` 展开到具体索引——没有 type erasure：

```cpp
void StartForward() noexcept {
    template for (constexpr auto I : std::make_index_sequence<sizeof...(Steps)>{}) {
        auto& step = std::get<I>(*steps_);
        // ... 启动 step.forward()，收到 set_value 时调 OnForwardSuccess(I, v)
    }
}

void RollbackFrom(std::size_t firstFailed) noexcept {
    // 反序走 [firstFailed - 1 ... 0]，每个跑 compensate(*results_[k])
    // 收集 compensate 自身的错误进 compensateErrors_
    // 全部完成后 set_error(primaryError_) 或 set_error(SagaCompensationError{...})
}
```

### Annotation 驱动的 `SagaFromClass`

更高的抽象：把 forward 与 compensate 写成普通成员函数，annotation 标记同序号的对偶：

```cpp
class OrderFlow {
public:
    [[=Saga::Forward(0)]]    auto Reserve()        -> Task<ReservationId>;
    [[=Saga::Compensate(0)]] auto Release(ReservationId) -> Task<>;

    [[=Saga::Forward(1)]]    auto Charge()         -> Task<TxnId>;
    [[=Saga::Compensate(1)]] auto Refund(TxnId)    -> Task<>;

    [[=Saga::Forward(2)]]    auto Ship()           -> Task<TrackingId>;
    [[=Saga::Compensate(2)]] auto CancelShipment(TrackingId) -> Task<>;
};

// 编译期展开 = 手写 Saga{Step{...}, Step{...}, Step{...}}
auto saga = Async::SagaFromClass<OrderFlow>(flow);
```

`SagaFromClass` 的 reflection：

```cpp
namespace Async::detail {

    consteval auto CollectSagaSteps(std::meta::info Class) {
        struct StepInfo {
            std::size_t      index;
            std::meta::info  forward;
            std::meta::info  compensate;
        };
        std::map<std::size_t, StepInfo> map;

        for (auto m : nonstatic_member_functions_of(Class)) {
            // forward 标注：[[=Saga::Forward(k)]]
            if (auto fk = annotation_of<Saga::ForwardTag>(m)) {
                map[fk->index].index   = fk->index;
                map[fk->index].forward = m;
            }
            // compensate 标注：[[=Saga::Compensate(k)]]
            if (auto ck = annotation_of<Saga::CompensateTag>(m)) {
                map[ck->index].index      = ck->index;
                map[ck->index].compensate = m;
            }
        }

        // 校验：每个 forward 有对偶 compensate；每个 compensate 也有对偶 forward
        for (auto& [k, info] : map) {
            if (!info.forward || !info.compensate) {
                // 直接 consteval 失败，把缺失方位写进错误消息
                std::unreachable();
            }
            // forward 的返回类型（拆 sender 的 value）必须能传给 compensate 的参数
            // 这是 Saga 的关键不变量：compensate 接收 forward 产出的"句柄"
        }
        return map;
    }

    template <class C>
    auto SagaFromClass(C& obj) noexcept {
        constexpr auto steps = CollectSagaSteps(^^C);
        return [&]<std::size_t... I>(std::index_sequence<I...>) {
            return Saga<Step<...>>(
                Step{
                    [&]() noexcept { return obj.[: steps.at(I).forward    :](); },
                    [&](auto v) noexcept { return obj.[: steps.at(I).compensate :](std::move(v)); }
                }...);
        }(std::make_index_sequence<steps.size()>{});
    }
}
```

序号错位、缺失对偶、forward 返回类型与 compensate 参数类型不匹配——任一在 consteval 阶
段失败，错误消息直指具体方法。这把"do/undo 配对维护"从约定提升为类型系统强制契约。

### 错误聚合

主错误优先：用户代码看到的 `set_error` 永远是 forward 失败的那个原因，因为这是业务关心
的根因。compensate 失败聚合到 `SagaCompensationError::compensations`，便于诊断；这是从
经验得出的设计——业务语义上 forward 失败更常见也更需要被传上来，compensate 失败需要
被 *观察*但通常不可恢复。

### 与 `Tether` 的关系

`Tether<A, R>` 是单一资源的 do/undo；`Saga` 是多步骤事务的 do/undo。语义对偶：

- Tether: acquire-once / release-once，release 之后不再可见。
- Saga: forward-N-times / compensate-K≤N-times（从最后成功的步骤往回推），整体观察一致。

### 示例

```cpp
auto saga = Async::Saga{
    Async::Step{ [&]{ return ReserveInventory(item); },
                 [](InvId id){ return ReleaseInventory(id); } },
    Async::Step{ [&]{ return ChargePayment(amt); },
                 [](TxnId id){ return RefundPayment(id); } },
    Async::Step{ [&]{ return ShipOrder(addr); },
                 [](TrackId id){ return CancelShipment(id); } },
};

co_await saga.Run();    // 任一失败自动反向
```

### C++26 杠杆点

- 反射用得最深的另一个原语。`SagaFromClass` 把 annotation 标注的成员函数对配对——人工
  维护这种对偶在所有补偿事务系统里都是 bug 集中地。
- 没有虚函数、没有 type erasure：每个 step 是不同类型，op_state 由 `template for` 编译期
  展开。

---


## 十一、`Cache<K, V, Eviction>` —— 异步 memoize

### 场景

- HTTP 响应缓存
- ORM 查询缓存
- 计算 memoize（递归算法）
- 异步配置/秘钥缓存（带 TTL）

### 问题

`Singleflight` 是"在飞去重"，*完成后即丢弃*；`Cache` 是"完成后保留"，下次直接命中。
两者经常被叠加（cache miss 时单飞 + 写回 cache）。手抄的版本反复在 TTL、LRU、并发安
全、stop_token 传播上出错。

### 形式与实现

`Cache<K, V, Eviction>` 在结构上是 *Singleflight + 完成结果留存*：cache miss 时单飞跑
factory，结果回填后下次直接命中。Eviction policy 是 type tag，编译期决定数据结构：

- `Lru<N>`: 哈希表 + 侵入式双向链表（O(1) 命中、O(1) 提升、O(1) 淘汰）。
- `Ttl<Clock, Duration>`: 哈希表 + 最小堆（按到期时间）。
- `LruWithTtl<N, Duration>`: 同时维护 LRU 链与 TTL 堆。

每个 Eviction policy 暴露同一组钩子：

```cpp
namespace Async {

    template <class P, class K, class V>
    concept EvictionPolicy = requires(P& p, const K& k) {
        { p.OnInsert(k) }     -> std::same_as<void>;
        { p.OnHit(k)    }     -> std::same_as<void>;
        { p.PickVictim() }    -> std::same_as<std::optional<K>>;
        { p.OnRemove(k) }     -> std::same_as<void>;
        { p.AtCapacity() }    -> std::same_as<bool>;
    };

    template <std::size_t N>
    struct Lru { /* policy methods */ };

    template <class Clock, auto Ttl>
    struct TtlPolicy { /* policy methods */ };

    template <std::size_t N, auto Ttl>
    struct LruWithTtl { /* combines both */ };
}
```

主体：

```cpp
namespace Async {

    template <class K, class V, class Eviction = Lru<128>, class Hash = std::hash<K>>
    class Cache {
    public:
        explicit Cache(Eviction ev = {}) noexcept;

        // 命中即返回（共享）；否则单飞 factory，结果回填，再返回。
        // 同 key 的并发调用合并到同一次 factory（singleflight 集成）。
        template <std::invocable<const K&> Factory>
            requires stdexec::sender_of<std::invoke_result_t<Factory, const K&>,
                                        stdexec::set_value_t(V)>
        [[nodiscard]] auto Get(K key, Factory factory) noexcept;

        void Invalidate(const K& key) noexcept;
        void Clear() noexcept;
        std::size_t Size() const noexcept;

    private:
        // 完成态条目：hold V + Eviction 的元数据
        struct Entry {
            V                                              value;
            typename Eviction::template Hook<K, V>         evHook;   // intrusive
        };

        // 进行中的工作：与 Singleflight 内部相同的 SharedState
        struct InflightEntry { detail::SharedState<V>* state; };

        // 同一 key 在两表中至多在一个：completed_ 或 inflight_
        std::shared_mutex                                  mtx_;
        std::unordered_map<K, Entry,         Hash>         completed_;
        std::unordered_map<K, InflightEntry, Hash>         inflight_;

        Eviction policy_;
    };
}
```

### `Get` 的三路决策

```cpp
template <class K, class V, class Eviction, class Hash>
template <std::invocable<const K&> Factory>
auto Cache<K, V, Eviction, Hash>::Get(K key, Factory factory) noexcept {
    return stdexec::let_value(
        stdexec::just(std::move(key), std::move(factory)),
        [this](K& key, Factory& factory) -> stdexec::sender auto {
            // 1) 已完成 → 直接返回
            {
                std::shared_lock lk{mtx_};
                if (auto it = completed_.find(key); it != completed_.end()) {
                    policy_.OnHit(key);
                    return stdexec::just(it->second.value);    // value 类型可拷贝/共享
                }
            }
            // 2) 进行中 → 挂载（Singleflight 路径）
            // 3) 未在飞 → 启动 factory 的 sender，完成后回填 completed_
            std::unique_lock lk{mtx_};
            if (auto it = completed_.find(key); it != completed_.end()) {
                policy_.OnHit(key);
                return stdexec::just(it->second.value);        // 锁内二次检查
            }
            if (auto it = inflight_.find(key); it != inflight_.end()) {
                auto* st = it->second.state;
                st->AddRef();
                lk.unlock();
                return AttachFollower<V>(st);                  // 见 Singleflight
            }
            auto* st = new detail::SharedState<V>{};
            inflight_.emplace(key, InflightEntry{st});
            lk.unlock();
            return RunLeader<V>(st, key, std::move(factory))
                 | stdexec::then([this, key](V v) noexcept {
                       Insert(key, v);            // 写入 completed_，从 inflight_ 摘除
                       return v;
                   });
        });
}

template <class K, class V, class Eviction, class Hash>
void Cache<K, V, Eviction, Hash>::Insert(const K& k, const V& v) noexcept {
    std::unique_lock lk{mtx_};
    inflight_.erase(k);
    if (policy_.AtCapacity()) {
        if (auto victim = policy_.PickVictim()) {
            policy_.OnRemove(*victim);
            completed_.erase(*victim);
        }
    }
    auto [it, _] = completed_.emplace(k, Entry{v, {}});
    policy_.OnInsert(k);
}
```

### TTL 的实现

`TtlPolicy::PickVictim` 从最小堆顶部取过期项；`OnHit` 不刷新（语义上 TTL 是绝对到期，不
是闲置回收）。这是与 LRU 的关键差别——LRU 的 `OnHit` 会把节点提到链表头，TTL 不会。

```cpp
template <class Clock, auto Ttl>
struct TtlPolicy {
    using TimePoint = typename Clock::time_point;
    struct HookData { TimePoint expiresAt; };
    template <class K, class V> using Hook = HookData;
    std::priority_queue<std::pair<TimePoint, K>,
                        std::vector<std::pair<TimePoint, K>>,
                        std::greater<>>             expQueue_;
    std::size_t                                     size_ = 0;
    std::size_t                                     cap_  = std::numeric_limits<std::size_t>::max();

    void OnInsert(const auto& k) noexcept {
        expQueue_.push({Clock::now() + Ttl, k});
        ++size_;
    }
    void OnHit(const auto&) noexcept {}
    std::optional<auto> PickVictim() noexcept {
        while (!expQueue_.empty() && expQueue_.top().first <= Clock::now()) {
            auto k = std::move(expQueue_.top().second);
            expQueue_.pop();
            return k;
        }
        return std::nullopt;
    }
    bool AtCapacity() const noexcept { return size_ >= cap_; }
};
```

### Stale-While-Revalidate

加 `stdexec::just(it->second.value)` 之外的另一条路径：返回当前值的同时在后台异步刷新。
这是常见的 *stale-while-revalidate* 模式，用一个模板参数选择启用：

```cpp
template <class K, class V, class Eviction = Lru<128>, class Hash = std::hash<K>,
          class Freshness = NoBackgroundRefresh>
class Cache;
```

`Freshness::ShouldRefresh(entry)` 在命中时被调；返回 true 则 spawn 一个后台 sender 跑
factory 并替换 entry。对于带 TTL 的缓存，常见策略是"过半 TTL 时触发后台刷新"。

### 与 `Singleflight` 的关系

Cache 的 inflight 路径**就是** Singleflight。区别只在于 leader 的成功路径多了一步"把结
果写入 completed_ 表"。两者不重叠：Singleflight 解决"在飞合并"，Cache 解决"完成留存"。
Cache 的实现可以直接持有一个 `Singleflight<K, V>` 成员，把 inflight 路径委托给它。

### 示例

```cpp
Async::Cache<std::string, ConfigDoc,
             Async::Ttl<std::chrono::system_clock, std::chrono::minutes{5}>> cache;

auto cfg = co_await cache.Get("db.yaml", [&](const auto& k) {
    return LoadFromDisk(k);
});
```

### C++26 杠杆点

- Eviction 用 type tag：编译期决定数据结构形态，零运行期 dispatch。
- `Freshness` 同样是 type tag：是否启用 SWR 在编译期决定，未启用时不生成相关代码。
- 不需要反射（policy 钩子是 concept 检查的具体方法名）。

---


## 十二、`Snapshot<T>` —— 一致性读视图

### 场景

- 渲染线程：取一份"此刻所有窗口大小"的快照渲染当帧。
- 可观测：dump 一份当前所有 metric 的瞬时值。
- 序列化：保存当前状态到磁盘时需要可重复读。
- ECS 系统读：当前帧组件值快照。

### 问题

SeqLock 解决单字段一致性；多字段联合的"一致快照"需要 N 字段同时一致。手抄都是 RCU
或写时复制，又粗又重。

### 形式与实现

`Snapshot<T>` 提供"单写多读、读到的 T 是一致的"语义。三类实现路径，按 T 大小与硬件特
性选择：

1. **小 POD（≤ 16 字节）**：直接 `std::atomic<T>`，硬件原子。零开销。
2. **中等 POD**：SeqLock——写者递增偶数版本号、写、再递增；读者两次读版本号包夹值，
   不一致则重试。无锁、读热路无竞争。
3. **大对象 / 含字符串**：双缓冲 + 引用计数（hazard pointer 或 RCU 风格）。读者拿到的
   是 `Held<T>`——RAII 引用，析构归还。

模板特化按 `sizeof(T)` 与 `std::is_trivially_copyable_v<T>` 自动选择：

```cpp
namespace Async::detail {

    template <class T>
    constexpr auto SelectSnapshotImpl() {
        if constexpr (std::is_trivially_copyable_v<T> && sizeof(T) <= 16
                      && alignof(T) <= alignof(std::max_align_t)) {
            return std::type_identity<AtomicSnapshot<T>>{};
        } else if constexpr (std::is_trivially_copyable_v<T>) {
            return std::type_identity<SeqLockSnapshot<T>>{};
        } else {
            return std::type_identity<RcuSnapshot<T>>{};
        }
    }
}

namespace Async {

    template <class T>
    using Snapshot = typename decltype(detail::SelectSnapshotImpl<T>())::type;
}
```

#### 实现 1：SeqLock

```cpp
namespace Async::detail {

    // T 必须 trivially_copyable，且 sizeof <= L1 行（避免读者横跨 cache line 撕裂概率）
    template <class T>
    class SeqLockSnapshot {
        static_assert(std::is_trivially_copyable_v<T>);
    public:
        // 单写者；多写时上层用 mutex 串行化（语义就是"单写"，不为多写设计）
        void Publish(const T& v) noexcept {
            auto seq = seq_.load(std::memory_order_relaxed);
            seq_.store(seq + 1, std::memory_order_release);  // 奇数 = 写中
            std::memcpy(&value_, &v, sizeof(T));
            seq_.store(seq + 2, std::memory_order_release);  // 偶数 = 完成
        }

        // 任意读者；返回 by-value（小到中等 T 的合理选择）
        T Observe() const noexcept {
            for (;;) {
                auto s1 = seq_.load(std::memory_order_acquire);
                if (s1 & 1) { std::this_thread::yield(); continue; }
                T snap;
                std::memcpy(&snap, &value_, sizeof(T));
                std::atomic_thread_fence(std::memory_order_acquire);
                auto s2 = seq_.load(std::memory_order_relaxed);
                if (s1 == s2) return snap;
            }
        }

    private:
        alignas(64) mutable std::atomic<std::uint64_t> seq_{0};
        alignas(T) T                                    value_{};
    };
}
```

`memcpy` + 版本号包夹是 SeqLock 的标准形态。两次读版本号一致才接受值；不一致说明读取
期间撕裂，重试。读路径无锁、无原子 RMW，只有两次 load 加一次 memcpy——L1 命中时几纳秒。

#### 实现 2：RCU 风格的 `Held<T>`

T 包含 `std::string` / 容器时不能 SeqLock；改用引用计数 + 版本切换：

```cpp
namespace Async::detail {

    template <class T>
    struct RcuVersion {
        T                        value;
        std::atomic<std::int32_t> refs;     // 0 = 可回收
    };

    template <class T>
    class RcuSnapshot {
    public:
        RcuSnapshot(): cur_(new RcuVersion<T>{T{}, 1}) {}
        ~RcuSnapshot() { /* 等待所有 Held 归还 */ }

        // 单写；旧版本由读者引用计数自然回收
        void Publish(T newValue) noexcept {
            auto* fresh = new RcuVersion<T>{std::move(newValue), 1};
            auto* old   = cur_.exchange(fresh, std::memory_order_acq_rel);
            // old 的引用计数从此不再增长（读者只看 cur_）；当现存读者归还后由 retire 列表回收
            Retire(old);
        }

        // 读取
        struct Held {
            RcuVersion<T>* v;
            ~Held() { if (v && v->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) delete v; }
            const T& operator*()  const noexcept { return v->value; }
            const T* operator->() const noexcept { return &v->value; }
        };

        [[nodiscard]] Held Observe() const noexcept {
            for (;;) {
                auto* v = cur_.load(std::memory_order_acquire);
                auto refs = v->refs.fetch_add(1, std::memory_order_acq_rel);
                if (refs > 0) return Held{v};         // 抢到引用
                v->refs.fetch_sub(1, std::memory_order_acq_rel);  // 已被 retire；重读 cur_
            }
        }

    private:
        std::atomic<RcuVersion<T>*> cur_;
        // retire 列表：旧版本进这里等读者归还；周期性 GC
        void Retire(RcuVersion<T>*) noexcept;
    };
}
```

`Held` 是 move-only RAII；读者持有期间该版本不会被回收。这把"读到的 T 是某一致版本"的
不变量写进了 RAII 句柄。

### Diff 与字段级反射（可选增强）

T 是聚合体（多个字段）时，有时想知道"两次发布之间哪些字段变了"。reflection 自然提供：

```cpp
namespace Async::Snapshot {

    template <class T>
    auto DiffFields(const T& a, const T& b) {
        std::vector<std::string_view> changed;
        template for (constexpr auto F : nonstatic_data_members_of(^^T)) {
            if (a.[:F:] != b.[:F:]) changed.push_back(identifier_of(F));
        }
        return changed;
    }

    template <class T>
    auto Watch(Snapshot<T>& s) {
        // 每次 Publish 后回调列出变化字段；适合做调试/记录/事件触发
        ...
    }
}
```

这是 reflection *打开了一个新维度*，而不是把已有事情用反射重写——没有反射时 `DiffFields`
就要让用户为每个 T 手写 if 链或宏展开，反射让它直接来自类型。

### 与 SeqLock 在 Mashiro 项目里的关系

Mashiro 已有 `SeqLock<T>` 用于 Manager 的单字段读视图（窗口大小、模式等）。`Snapshot<T>`
是它的语义延伸：

- `SeqLock<int>` ≡ `Snapshot<int>`（小 POD，落到 AtomicSnapshot 路径）
- `SeqLock<WindowSize>` ≡ `Snapshot<WindowSize>`（中等 POD，落到 SeqLockSnapshot 路径）
- `Snapshot<WorldState>`（大对象、含 string/容器）需要 RCU 路径——SeqLock 无法处理

把它们统一在 `Snapshot<T>` 名字下，用类型大小自动选择实现，避免上层用户根据 T 形态手
工挑 SeqLock vs RCU。

### 示例

```cpp
struct WorldState {
    Vec3 cameraPos;
    Mat4 viewProj;
    int  activeWindowId;
};

Async::Snapshot<WorldState> world;          // 这种 sizeof，落到 SeqLock 路径

// 平台线程发布
world.Publish({camPos, vp, winId});

// 渲染线程读
auto snap = world.Observe();                 // by-value（SeqLock 路径）
RenderFrame(snap.cameraPos, snap.viewProj);
```

### C++26 杠杆点

- `consteval` + `if constexpr` 选实现：每个 `T` 走最优路径，不分配多余内存、不引虚函数。
- *合适处*用反射：`DiffFields`、`Watch` 之类的字段级操作。Snapshot 主路径不需要反射。
- 读路径上没有原子 RMW（SeqLock 路径）或只有单次 increment（RCU 路径），与手抄 SeqLock
  同性能。

---


## 十三、合适的反射 vs 不合适的反射

把上面的原语按"反射杠杆"归类：

| 原语 | 反射用得上吗 | 用在哪 |
|---|---|---|
| `Rendezvous` | 否 | 纯运行期计数 |
| `Race` / `Quorum` | 否 | sender 组合 |
| `Singleflight` | 否 | hash map + 引用计数 |
| `Pool` | 部分 | annotation 标 validate/reset 钩子 |
| `Bulkhead` | 否 | 信号量 + sender 包装 |
| `Retry` / `CircuitBreaker` | 否 | 重试逻辑 |
| `Pipe<Stages...>` | 强 | stage 类型对接验证、annotation 标调度 |
| `Topic<T>` | 否 | MPMC 数据结构 |
| `Channel<T>` | 部分 | `select` 的 op_state 生成 |
| `Saga<Steps...>` | 强 | annotation 配对 forward/compensate |
| `Cache` | 否 | 数据结构 + 策略 |
| `Snapshot` | 部分 | 字段级 diff |

合适处用反射的判据是：**反射在做"否则就要让用户手工写一份目录或表"的事**。Pipe 的
stage 链类型对接是一份目录、Saga 的 do/undo 对偶是一份目录、字段 diff 是一份目录——
这些目录手工维护必然漂移。反射让它们直接来自类型本身。

而 `Race` 这种纯组合子，反射进去就是炫技：op_state 的形态可以从 sender 的 completion
signatures 直接派生（这是 stdexec 已经提供的），不需要反射再插一手。

## 十四、一个前瞻方向：作用列代数

上面所有原语都是"组合 sender"或"包装 sender"。还有一个更深的方向值得提到——**作
用追踪**。

每个 sender 都隐含携带一组"它会做什么"的语义标签：会读 IO、会写文件、会调用网络、
会持有锁、会获取 GPU 资源……这些标签当前没有名字，但它们决定了 sender 链的*静态
合规性*：

- 一个标记为 `[[=PureCpu]]` 的 sender 链不允许出现 `[[=NetworkIO]]` 的子 sender；
- 一个标记为 `[[=NoBlocking]]` 的 sender 链不允许 `[[=BlocksThread]]`；
- 一个标记为 `[[=NoAlloc]]` 的 sender 链不允许 `[[=Allocates]]`。

C++26 的 annotation + reflection 让这种 *作用类型* 的静态合规检查变得现实可行：

```cpp
template <class S, class... Forbidden>
concept EffectFree = !( /* reflection: walk S's effect annotations */
                        contains<Effect, S, Forbidden> || ... );

[[=PureCpu, NoBlocking, NoAlloc]]
auto realtime_audio_dsp() -> Task<>;  // 编译期保证不踩雷区
```

这一层是这十几个原语的*语义底座*。它本身可能还要等 C++26 落地后社区试错几年。但它
是合理的下一步。

## 十五、在 Mashiro/Yuki 项目里的落点

把上面的通用原语对回到本仓库已经存在的具体类型，可以直接看出**哪些原语已经物化、
哪些只差一层薄壳、哪些尚是空白**。这一节按"项目里现有的最贴近实体 → 原语身份 →
缺口"三段式列出，避免在抽象空中打转。

### 15.1 已经物化的原语：现成的就是本体

| 通用原语 | 项目内实体 | 头文件 | 关系 |
|---|---|---|---|
| `Snapshot<T>` (SeqLock 路径) | `Mashiro::SeqLock<T>` | `Mashiro/Core/SeqLock.h` | 完全等同 |
| `Pool<T>` 数据结构层 | `Mashiro::ConcurrentObjectPool<T, Config>` | `Mashiro/Core/ConcurrentObjectPool.h` | `Pool::Acquire` 即 `Emplace`/`Release` |
| `Channel<T>` (MPSC) | `Mashiro::MpscQueue<T, Capacity>` | `Mashiro/Core/MpscQueue.h` | 数据通道层完全等同 |
| `Channel<T>` (SPSC) | `Mashiro::SpscQueue<T, Capacity>` | `Mashiro/Core/SpscRingBuffer.h` | 同上，N→1 退化为 1→1 |
| `Beacon<Schema>` 写入端 | `Mashiro::StructuredLogger` | `Mashiro/Core/StructuredLogger.h` | producer→drain→sink 即 Beacon |
| `AsyncSeq<T>` | `Mashiro::Generator<T>` | `Mashiro/Async/Generator.h` | 同步惰性序列；异步版只差 `awaitable_traits` |
| `Outcome<T,E>` | `Mashiro::Result<T>` | `Mashiro/Core/Result.h` | `std::expected<T, ErrorCode>` 别名 |
| `Edge` 类型化判别 | `Mashiro::Event` reflection-driven `Traits` | `Mashiro/Platform/SystemEvent.h` | 用反射枚举 payload 类型而非 enum |
| `Apartment<Tag>` 分类标记 | `Mashiro::Platform::ScheduleMode` / `ThreadContract` | `Mashiro/Platform/ThreadContract.h` | 三档：Platform / Dedicated / FreeThreaded |

这些原语**不应再造**——它们已经在仓库里以正确形态存在。后续把通用原语介绍给协作者
时，第一句就该是"这就是我们现在用的 X"，而不是"我们将引入一个新的 Y"。

### 15.2 只差一层薄壳：上层算子尚未提取

下表每一项的"项目内实体"已经具备**全部数据结构和并发原语**，缺的只是把通用原语
暴露的那个公共接口（sender adaptor / 反射 `tuple_for` 等）切出来。

| 通用原语 | 现有底座 | 还缺什么 |
|---|---|---|
| `Snapshot<T>` (RCU/Atomic 路径) | `SeqLock<T>` 已就绪 | `consteval` 选择器：`is_trivially_copyable && sizeof<=8` 走 `atomic`；其余走 `SeqLock`；指针/含 `unique_ptr` 走 RCU |
| `Pool<T>` 策略层 | `ConcurrentObjectPool` 数据层 | `[[=Validate]]` / `[[=Reset]]` annotation + `Lease<T>` RAII 句柄 + 回池脚手架 |
| `Beacon<Schema>` 读取端 | `StructuredLogger` 已写入 ring | `Subscribe(scheduler, sink)` sender 适配器 + `Snapshot` 接口 |
| `Topic<T>` (MPMC, retention) | 多个 `SpscQueue` + 写入端的 `Unified Event Writer` | 把"一写多读"的 `EventChannel` 形态统一成模板，加 retention 策略 |
| `AsyncSeq<T>` (异步版) | `Generator<T>` (sync 版) | `co_await` 的下一帧；`promise_type::await_transform` 接 stdexec sender |
| `Pipe<Stages...>` 编译期校验 | `EventPump` 静态决定 bookkeep→broadcast 序列 | `ValidatePipeChain` consteval 在 stage 间核对 sender 的 `value_types`/`error_types` |
| `Singleflight` | `MpscQueue` 已能做等待者列表 | hash map + 引用计数；首个请求建桩、其余挂队列 |
| `Rendezvous` (Latch/Barrier) | `std::latch`/`std::barrier` 已可用 | 包成 sender，让 `co_await` 不需要单独同步原语 |

每一项的实现成本是"百行级"——不是新组件，而是为已有底座补对外算子。

### 15.3 当前空白：暂不引入，给出落点

下面的原语在仓库里**没有现成实体也暂无强用例**，但已经能指出未来一旦需要它们时
的"该挂在哪里"——避免日后被随手贴在错误的位置。

| 通用原语 | 未来落点 | 触发条件 |
|---|---|---|
| `Saga<Steps...>` | 业务流（如 Asset 导入：拷贝→解析→落库；任一步失败要回滚前序） | 第三个出现 do/undo 对偶的流程时 |
| `Cache<K,V>` | 资源加载层：纹理/着色器编译产物 | Asset 子系统进入实现阶段 |
| `Bulkhead<Tag>` | GPU command 提交、磁盘 IO 提交 | 出现首个被并发淹没的下游服务 |
| `Retry`/`CircuitBreaker` | 平台拓展（外部 SDK、网络元数据） | 第一个跨进程/跨主机依赖 |
| `Sluice` | 高频输入事件的合并（鼠标移动、缩放） | 出现 input lag 的具体性能问题 |
| `Chronograph` | 性能采样、profile 时间轴 | 接入 Tracy 或自研 timeline 时 |
| `Race`/`Quorum` | 多源平台事件优先选择（多窗口同时关闭） | 出现"以最早/最多者为准"的具体诉求 |

这些原语**不是技术债**——是预留的命名空间。提前把"以后那个东西叫什么"定下来，
能避免半年后用 `TaskQueue2`、`AsyncManager`、`PolicyHelper` 这种迁就性命名。

### 15.4 EventPump 这一个案的展开：通用原语如何拼出整个组件

EventPump 是本仓库**最稠密**的异步原语聚合点。把上面分散的对应关系按 EventPump
内部数据流串起来，能看到通用原语并不是"将来用"，而是早已**在结构上**支撑着这个
组件——只是没起一等命名：

```
平台事件源              管理器                       客户端
  (Win32 / Wayland)      (Window/Input/IME/...)       (任意线程订阅)
        │                       ▲ co_await OwnerTask    ▲ co_await
        │                       │                       │
        ▼                       │                       │
  ┌─────────────────────────────┼───────────────────────┼─────────┐
  │  Platform thread            │                       │         │
  │                             │                       │         │
  │  ┌──────────┐   Pipe        │                       │         │
  │  │ Win32    │──────────────┐│                       │         │
  │  │ Pump     │              ▼▼                       │         │
  │  └──────────┘   ┌────────────────────────┐          │         │
  │       ▲         │ Bookkeep + Broadcast   │  Topic   │         │
  │       │         │ (Pipe<Stages...>)      │──────────┴─────────┤
  │       │ wake    └────────────────────────┘  (SpscRing 一写多读)│
  │       │                ▲                                       │
  │  ┌────┴─────┐ Channel  │   Tether (OwnerTask)                  │
  │  │ MPSC     │──────────┘                                       │
  │  │ Inbox    │                                                  │
  │  └──────────┘                                                  │
  └────────────────────────────────────────────────────────────────┘
```

逐条对回原语：

- **`Channel<SystemEvent>` (MPSC inbox)**——对应 `MpscQueue<SystemEvent, 256>`：
  Dedicated-thread Manager（Gamepad、FileWatch）把事件提交到此处。**形态：N 写
  1 读**。
- **`Pipe<Bookkeep, Broadcast>` (内部链)**——`EventPump::Drain` 中的
  `BookkeepFor(SystemEvent)` → `BroadcastTo(EventChannels)` 是字面意义上的两阶段
  Pipe，其 stage 类型对接由反射在 `consteval` 阶段验证（`Traits::HandlesBookkeep<M, P>`
  决定哪些 manager 在哪些 payload 上挂钩，是 `Edge` 在用类型而非 if 实现的形态）。
- **`Topic<SystemEvent>` (Unified Event Writer 出口)**——平台线程是唯一写者，
  N 个 `EventChannel<>`(`SpscQueue<SystemEvent, ...>`) 是 N 个订阅者。**形态：1 写
  N 读 + 持久化历史**（最近 K 条用于新订阅者的 backfill）。这是当前仓库里"一层薄
  壳"清单中的代表项：实体已存在，缺的是"`Topic<>` 本身作为模板"的命名。
- **`Tether<Args...>` (OwnerTask)**——任意线程的 worker 通过 `co_await` 一个跨线
  程 awaiter，把执行权移交到 Manager 所在 Apartment 的 `OwnerExecutor`，然后回到
  原线程拿结果。这条路径是 `Tether` 的教科书实现，背后是 `ConcurrentObjectPool` 上
  的协程节点 mailbox（见 `MpscQueue` 头文档对二者分工的论述）。
- **`Apartment<Tag>` (Platform / Dedicated / FreeThreaded)**——`ScheduleMode` 用
  annotation 标在每个 Manager 上，`Traits::GetScheduleMode<M>()` 在 `consteval`
  阶段读取后决定线程归属。这是 `Apartment` 形态的项目化实现：tag 不是 `enum class`
  而是反射元数据。
- **`Edge` (类型化判别)**——`SystemEvent` 是 `std::variant`，
  `Traits::HandlesBookkeep<M, P>` 在编译期为每个 (Manager, Payload) 对决定是否插入
  bookkeep 调用。运行期 `std::visit` 落到对应 case，没有"`if (kind == ...)`"枚举开
  关。
- **`Beacon<Schema>` (诊断回路)**——`StructuredLogger` 的 thread-local
  `SpscByteRing` + 后台 drain 完整对应 Beacon "热路径写入仅碰 TLS、冷路径串行处
  理"的本意。其 schema 由 `LogAnno::DefaultLevel` / `LogAnno::LevelColor` 驱动，
  也是反射用得**合适处**的样本。

EventPump 因此不是"一个有点复杂的事件分发器"——它是 **Channel + Pipe + Topic +
Tether + Apartment + Edge** 六个原语在一个具体物理位置上的合奏。一旦把这六个名字
分别立住，EventPump 自身的实现细节（哪个 manager 挂在哪、哪条边是热路径、哪个
hash 表持久化、哪个 ring 的容量怎么定）就全部可以**沿原语而非沿组件**讨论。

### 15.5 Yuki 元类与原语反射的供给方

许多原语（`Pipe`、`Saga`、`Beacon<Schema>`、`Cache` 的字段级序列化、`Snapshot`
的 `DiffFields`）都依赖**类型本身的结构性元数据**。Yuki 的 `MetaClass` /
`MetaCore` 体系（`Yuki/Core/MetaClass.h`）是这些原语的**结构性元数据供给侧**：

- **`Iid` 与 `MetaCore`**——`Beacon` 的 schema、`Saga` 的 do/undo 表都需要"对类
  的引用要稳"。Yuki 的 IID = `name | AbiDigest` 的 FNV-1a128，**ABI 一变 IID 就
  变**，这是落地"schema 变化即可见"的天然底座。
- **annotation 折叠到 `MetaCore`**——`BuildMetaCore` 已经把 `Anno::Meta` 的内容
  在 `consteval` 阶段折成 `.rodata` 常量。`Saga` 的 `[[=Forward, Compensate]]`、
  `Pipe` 的 `[[=Stage(0), OnScheduler<Gpu>]]` 都可以走同一管道，不必再各自搭一套
  反射读取代码。
- **静态 vs 动态双面**——`ClassTypeOf<T>` 与 `obj->classType()` 是同一份事实的
  两个观测面。`Snapshot<T>::DiffFields` 既能在编译期（已知类型）展开，也能通过
  `MetaClass` 在运行期反射，**两条路通向同一答案**。

下游原语**不应**自行实现"读 annotation"或"算 ID"——它们应该把类型交给
`MetaClass`、读取统一的 `MetaCore`。这是把"反射用得合适"沉淀为基础设施的方式：
反射只在 `Yuki::MetaClass` 层做一次，下游原语全部受益。

### 15.6 Mashiro/Yuki 命名空间的边界（落点的硬约束）

一个具体而硬的项目约束（已在 `feedback_yuki_mashiro_namespace.md` 中记录）：
**Yuki 与 Mashiro 都各自定义 `Anno` 与 `NameOf`**，两边的 `Anno::*` 注解互不相
通（`Mashiro::Anno::Required` 与 `Yuki::Anno::Iid` 是不同字面）。所以在为本项目
落地原语时：

- **Mashiro 内部的原语注解**（如 `Pool` 的 `[[=Validate]]`、`Pipe` 的
  `[[=OnScheduler<...>]]`）放在 `Mashiro::Anno` 命名空间。
- **Yuki 对象模型层的原语注解**（`Iid`、`Extends`、`Implements`）放在 `Yuki::Anno`
  命名空间。
- 二者都需要时，**显式限定**而不是 `using namespace`。

这条约束直接影响 §15.5 的实现：原语的 annotation 类型一定要**和它落地所属的层**
对齐，否则会造成 `Anno::Required` 这类符号在两个命名空间里同时被引用，触发歧义
解析。

### 15.7 简表：原语 → 项目落点

最后给出一份单页总览，方便协作者把通用原语和项目实体快速对回。**"="表示完全等
同；"⊃"表示项目实体提供了更多功能；"⊂"表示项目实体是原语的子集；"○"表示需要
一层薄壳；"–"表示当前留空。**

| 原语 | 关系 | 项目落点 |
|---|---|---|
| Apartment | = | `Platform::ScheduleMode` (annotation) |
| Tether | = | Manager 的 `OwnerTask<T>` + `OwnerExecutor` |
| Sluice | – | （未引入；Input 合并是候选） |
| Beacon (write) | ⊃ | `StructuredLogger` (含 sinks/drain) |
| Beacon (subscribe) | ○ | 缺 sender 适配层 |
| Chronograph | – | （未引入；接 Tracy 时引入） |
| Outcome | = | `Result<T>` (`std::expected`) |
| AsyncSeq (sync) | = | `Generator<T>` |
| AsyncSeq (async) | ○ | `Generator` + `await_transform` 接 stdexec |
| Edge | = | `Event::Traits` 反射 |
| Rendezvous | ○ | `std::latch`/`std::barrier` 包 sender |
| Race / Quorum | – | （未引入） |
| Singleflight | ○ | `MpscQueue` + hash map |
| Pool (data) | = | `ConcurrentObjectPool` |
| Pool (policy) | ○ | 缺 `Lease<T>` + `[[=Validate/Reset]]` |
| Bulkhead | – | （未引入） |
| Retry / CircuitBreaker | – | （未引入） |
| Pipe | ⊂ | `EventPump` 的 bookkeep+broadcast 是 2-stage 实例 |
| Topic | ⊂ | "Unified Event Writer + N×SpscRing" 是固定 fan-out 实例 |
| Channel (MPSC) | = | `MpscQueue` |
| Channel (SPSC) | = | `SpscQueue` / `SpscByteRing` |
| Saga | – | （未引入） |
| Cache | – | （未引入） |
| Snapshot (SeqLock) | = | `SeqLock<T>` |
| Snapshot (RCU/Atomic) | ○ | 缺 `consteval` 路径选择器 |
| 反射元数据供给 | = | `Yuki::MetaClass` / `MetaCore` |

把这张表当作"通用原语在本项目里**实际**长什么样"的字典：评审 PR、写新 Manager、
拆解 issue 时优先按这张表索引，避免把已有原语再造一遍，也避免把空白的原语顺手实
现成局部 helper。

---

## 十六、总结

不要把这些原语看作"功能列表"——它们各自命名了一种*已经存在但没有名字的形态*。命
名的价值是：

1. **沟通成本下降。** "这里加个 Saga" 比 "这里写一组 do/undo 对" 信息密度高一个数量级。
2. **正确性下沉。** 一旦形态被命名，错误就能在原语级别被一次性修对，所有使用者得益。
3. **抽象边界清楚。** Stream / Topic / Channel 三者形态相近但语义不同；分别命名后各自
   的边界稳定，组合时不再混淆。

一个通用异步框架不是"这些原语的总和"，而是"这些原语**互不重叠**的最小覆盖"。框架
的成熟度，靠的不是原语数量，而是这些原语之间是否清晰地各司其职。

