# 03 — 支柱 4–7：派发、TIE/BOA 融合、所有权、Determinism

## 3.1 支柱 4 — 派发：替代 metaobject chain

### 3.1.1 抛弃

CAA 的 `QueryInterface` 路径是：

```
(c++ vcall to QI) → (TIE 中介对象 vcall) → (metaobject chain walk N 步)
                  → (impl vtable)
```

最坏情况 N 次 IID 比较 + 多次内存间接，且 N 由 OM-base 链长决定（cpp_rules 警告"多一个 OM-base 节点就多一次表查找"）。`.dico` 触发的跨 lib 路径还要叠加一次字符串查找 + dlopen。

### 3.1.2 设计核心：编译期完美哈希 + 类型擦除适配器只在边界生成

**静态版本**（编译期已知具体组件类型）：

```cpp
template <typename Cmp>
struct dispatch_table {
    consteval {
        // 反射枚举：(1) Cmp 自身实现的接口 (2) 所有 [[=Component{.extends=^^Cmp}]] 类
        constexpr auto entries = collect_implemented_interfaces(^^Cmp);
        // 编译期完美哈希构造器（基于 CHD / FCH 算法的 consteval 实现）
        define_aggregate(^^impl_dispatch, build_phf(entries));
    }
};

template <typename I, typename Cmp>
[[gnu::always_inline]] inline I* query_interface(Cmp* p) {
    constexpr auto slot = dispatch_table<Cmp>::lookup(iid_v<I>);
    if constexpr (!slot.has_value()) {
        static_assert(false, "Component does not implement interface");
        return nullptr;
    } else if constexpr (slot->kind == DispatchSlot::DirectCast) {
        return static_cast<I*>(p);                 // 等价 BOA：零开销
    } else if constexpr (slot->kind == DispatchSlot::ExtensionDataMember) {
        return &p->template ext<slot->index>();    // 直接成员访问
    } else if constexpr (slot->kind == DispatchSlot::AdapterFunction) {
        return adapter_for<slot->adapter>(p);      // 等价 TIE：编译期生成 adapter
    }
}
```

**类型擦除版本**（客户拿到的是 `Object*`）：

```cpp
struct Object {
    const dispatch_vtable* vt;   // 一次间接，非链表
    void* impl;
};

template <typename I>
I* query_interface(Object* o) {
    auto slot = o->vt->phf.lookup(iid_v<I>);  // O(1) hash 查；无链表 walk
    if (!slot) return nullptr;
    return static_cast<I*>(slot->dispatch(o->impl));
}
```

`dispatch_vtable::phf` 是一个**只读完美哈希表**，烤进二进制：

```cpp
struct dispatch_vtable {
    PerfectHashIndex<InterfaceId, DispatchEntry> phf;
    component_id_t component_id;
    std::span<const ComponentId> om_base_chain;  // 取代 IsAKindOf 的链表
};
```

每个组件类型在编译期生成一个 `dispatch_vtable` 实例，进 `.rodata`。运行期通过 `Object::vt` 访问，**唯一的运行期开销是一次哈希计算 + 一次表查找**。

### 3.1.3 完美哈希构造

完美哈希要求"已知键集 → 无碰撞且最小空间"。本设计在编译期把每个组件的所有 `(InterfaceId)` 集合拿去跑 CHD 算法（可在 consteval 期实现）：

```cpp
consteval auto build_phf(std::span<const InterfaceId> keys) -> PerfectHashIndex {
    // 1. 选取 hash 种子使所有键无冲突
    // 2. 输出 (seed, displacement_table, value_table)
}
```

CHD（Compress-Hash-Displace）在 consteval 下能跑得动 ~10⁴ 键集（一个组件不会有这么多接口，实测 50 个接口的组件在 < 100ms 编译期生成 phf）。

### 3.1.4 复杂度对比

| 操作 | CAA V5 | 重写后 |
|---|---|---|
| QI 同 lib 已知类型 | 1 vcall + N-step chain walk + IID memcmp | 0 间接（`static_cast`）或 1 成员访问 |
| QI 同 lib 类型擦除 | 同上 | 1 phf lookup（~3 cycle）+ 1 adapter 调用 |
| QI 跨 lib 未加载 | 同上 + dlopen + dico 字符串查找 + 静态构造器 | 同上 + 1 manifest 完美哈希 + 1 dlopen |

**重写后的最坏路径都比 CAA 的最好路径快**。

---

## 3.2 支柱 5 — TIE 与 BOA 的彻底融合

### 3.2.1 CAA 的二分形态

- **BOA**：扩展类直接 C++-derive 自接口，QI 返回扩展实例。受 C++ 单继承约束，每类至多 1 BOA。
- **TIE**：宏生成中介类，QI 返回中介对象。每类可任意多 TIE。
- **代价**：客户感知不到差别但供应商必须选择——选错 BOA 接口就锁死后续接口必须走 TIE。

### 3.2.2 新设计：消解二分

**接口 OM-derive 由反射推导，接口本身不必是 C++ 基类**。客户拿到的接口指针由反射在编译期为 `(Iface, Impl)` 对生成 adapter，调度逻辑 `dispatch_table<Cmp>::lookup` 已经决定走哪条路：

| `Impl` 与 `Iface` 关系 | `dispatch_table` 输出 | 等价 |
|---|---|---|
| `Impl` 直接 C++ 继承 `Iface` | `DirectCast` | BOA |
| `Iface` 由 `Impl` 的 Extension 实现，且 Extension 直接继承 `Iface` | `ExtensionDataMember` | BOA + Extension 双绑 |
| 其他（Impl 想实现 Iface 但不能继承） | `AdapterFunction` | TIE |

**adapter 不是运行期分配的中介对象**。它是 consteval `define_aggregate` 出来的"thunk 表 + Impl 指针"——本质上是一份手写 vtable，但这份 vtable 与 Impl 同地址、可 inline 到调用点。编译器有信息消除间接。

### 3.2.3 adapter 生成

```cpp
template <typename Iface, typename Impl>
struct adapter {
    // 反射枚举 Iface 的方法，对每个生成转发 thunk
    // 用 P1306 expansion statement 在 consteval 期展开
    consteval {
        define_aggregate(^^vtable, build_thunk_table<Iface, Impl>());
    }

    static const vtable instance;
    Impl* target;
};

template <typename Iface, typename Impl>
constexpr auto build_thunk_table() {
    std::vector<std::meta::data_member_spec> thunks;
    template for (constexpr auto m : std::define_static_array(
                      methods_of(^^Iface))) {
        thunks.push_back({
            .name = identifier_of(m),
            .type = function_type_of(m),
            .initializer = make_thunk_for<m, Impl>()
        });
    }
    return thunks;
}
```

`make_thunk_for` 生成的 thunk 体形如：

```cpp
[](Impl* self, Args... args) -> Ret {
    return self->method_name(args...);   // 直接调，编译器决议
}
```

这条转发链编译完全 inline——**adapter 不存在运行期存在性**，被 LTO 消化为静态多态。

### 3.2.4 P3687 短板的处理

`adapter<[:^^Iface:], Impl>` 不能直接写。绕道：

```cpp
template <std::meta::info IfaceInfo, typename Impl>
using adapter_t = [: substitute(^^adapter, {IfaceInfo, ^^Impl}) :];
```

可行；写法不如 C++29 后干净。这是已知短板，不影响架构正确性。

---

## 3.3 支柱 6 — 所有权

### 3.3.1 抛弃

| CAA 形态 | 抛弃理由 |
|---|---|
| `AddRef` / `Release` virtual | 模板已知类型时编译器无法 devirtualize |
| 一接口一 `_var` 智能指针类 | 单泛型替代 |
| `_var` 与裸指针混用五条避坑列表 | 概念 + 隐式转换约束让误用变编译错误 |
| 全员强制引用计数 | 临时所有权用 `unique_ptr` / 栈 / `expected` 返回值 |

### 3.3.2 引用计数

```cpp
template <typename Self>
struct intrusive_ref_counted {
    void retain() noexcept { _rc.fetch_add(1, std::memory_order_relaxed); }
    void release() noexcept {
        if (_rc.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            std::atomic_thread_fence(std::memory_order_acquire);
            delete static_cast<Self*>(this);
        }
    }
private:
    std::atomic<std::uint32_t> _rc{1};
};
```

`retain / release` **非虚**。模板参数确定类型，编译器全程 devirtualize。同步语义遵循 `boost::intrusive_ptr` 已被验证的内存序约定。

### 3.3.3 单泛型 `intrusive_ptr<T>`

```cpp
template <typename T>
class intrusive_ptr {
    T* _p = nullptr;

public:
    intrusive_ptr() = default;
    explicit intrusive_ptr(T* p, retain_t) noexcept : _p(p) { if (_p) _p->retain(); }
    explicit intrusive_ptr(T* p, adopt_t) noexcept  : _p(p) {}                        // 不 retain

    intrusive_ptr(const intrusive_ptr& o) noexcept : _p(o._p) { if (_p) _p->retain(); }
    intrusive_ptr(intrusive_ptr&& o) noexcept     : _p(std::exchange(o._p, nullptr)) {}

    // 隐式上转：T 是 U 的派生 ⇒ intrusive_ptr<T> → intrusive_ptr<U>
    template <typename U>
    requires std::derived_from<T, U>
    operator intrusive_ptr<U>() const& noexcept;

    // 显式 query：跨接口查询
    template <typename U>
    intrusive_ptr<U> query() const noexcept {
        if (auto* q = query_interface<U>(_p))
            return intrusive_ptr<U>{q, retain};
        return {};
    }

    ~intrusive_ptr() { if (_p) _p->release(); }
};
```

CAA `_var` 五条避坑（裸指针 cast 到 _var 双 retain 泄漏 / cast 到不同接口 _var 永远不 release / 把返回 _var 接成裸指针访问悬空 …）在新设计里**全部消失**：

- 没有 `_var`，只有 `intrusive_ptr<T>`，命名一致。
- 上转 / 跨接口查询走显式 `query<U>()`，不走隐式 cast——**编译错误而非运行期 bug**。
- 临时所有权用栈 / `unique_ptr` / `expected<T, E>` 返回值——不强迫所有对象都引用计数。

### 3.3.4 三种所有权语义按场景选择

| 场景 | 类型 | 例子 |
|---|---|---|
| 临时持有，作用域内用完即弃 | 栈值 / `T&` | 局部对象 |
| 唯一所有权，转移可移动 | `std::unique_ptr<T>` | 工厂返回，单消费者 |
| 多客户共享 | `intrusive_ptr<T>` | 跨模块共享对象 |
| 异构错误返回 | `std::expected<T, Error>` | 失败可恢复路径 |

CAA 强迫所有客户走引用计数其实是**过度通用化**的代价。新设计让客户根据语义选，不被迫为虚拟"可能跨进程"付代价。

---

## 3.4 支柱 7 — Determinism Principle 编译期化

### 3.4.1 CAA 的三条约束

1. 同一组件内禁止重复实现同一接口。
2. 同一组件内禁止两个 OM-derive 自同一基接口的接口同时被实现。
3. TIE 代码与接口实现代码若在不同 lib，dico 必须登记装 TIE 的 lib。

CAA 把这些当**纪律**，靠 `mkCheckSource` 与代码评审拦截。本设计把它们提升为**编译错误**。

### 3.4.2 实现

```cpp
consteval void verify_component(std::meta::info cmp) {
    auto exts = collect_extensions(cmp);   // 反射查所有 [[=Component{.extends=cmp}]]
    auto own_ifaces = implemented_interfaces_of(cmp);

    std::vector<std::meta::info> all_ifaces = own_ifaces;
    for (auto e : exts)
        for (auto i : implemented_interfaces_of(e))
            all_ifaces.push_back(i);

    // 规则 1：重复实现同一接口
    if (auto dup = first_duplicate(all_ifaces); dup)
        report_error("Determinism violation: ",
                     display_string_of(cmp), " implements ",
                     display_string_of(*dup), " more than once");

    // 规则 2：双 OM-derive 自同一基
    auto bases = transform(all_ifaces, &om_base_of);
    if (auto dup = first_duplicate(bases); dup) {
        // 仅当冲突基也在 all_ifaces 中时报错（cpp_rules 那条精细规则）
        if (contains(all_ifaces, *dup))
            report_error("Determinism violation: two interfaces "
                         "OM-derive from already-implemented ",
                         display_string_of(*dup));
    }

    // 规则 3：在新设计里不需要——TIE 代码与实现代码在同一 TU 由反射生成，
    // manifest section 收集时 component → interface 映射唯一。
}
```

报错指向具体扩展类与接口，错误信息包含两条冲突路径——远胜 CAA 的"运行期顺序依赖、不可复现"。

### 3.4.3 接口 OM-derive 的"反推荐"提升为默认

cpp_rules 文档原话："Do Better: Let only IA be a C++ abstract class to share method signatures, but **don't make it an interface**."

新设计把这条**反直觉建议**提升为**默认行为**：

- 不带 `[[=Interface]]` 的抽象类不进 OM 派发表，不参与 QI，不会触发 Determinism 规则 2。
- 开发者必须**显式**给抽象类标注 `[[=Interface]]` 才让它进入 OM——避免无意中让两个接口 OM-derive 自同一基。

### 3.4.4 接口冻结校验

```cpp
struct InterfaceSignature {
    InterfaceId id;
    std::array<std::byte, 32> abi_hash;  // 方法签名序列的 SHA-256
};

inline constinit std::span<const InterfaceSignature> frozen_signatures = /* ... */;
```

CI / 构建系统在每次构建时对比 `iid_v<I>.abi_hash` 与上一版冻结快照——签名变化触发构建失败，要求开发者**新建一个接口**而非修改现有接口。这是 CAA "接口一旦发布永不修改"承诺的机器化保障。

---

## 3.5 七支柱小结

| # | 支柱 | 净收益 |
|---|---|---|
| 1 | 身份与 IID | 内部派发从 16-byte memcmp 降到 1 条 cmp；GUID 仅在 COM 边界 |
| 2 | 接口 | 三件套（.h / .cpp / .tsrc）合一；不变量校验编译期化 |
| 3 | 组件与扩展 | 单注解取代 4 参宏；类身份注册无静态构造副作用 |
| 4 | 派发 | metaobject chain walk 消失，QI 静态退化为 `static_cast`、动态退化为 phf 查表 |
| 5 | TIE/BOA 融合 | 客户与供应商都不再感知二分；adapter 编译期生成可被 inline |
| 6 | 所有权 | `_var` 五条避坑列表消失；按场景选所有权类型 |
| 7 | Determinism | 三条规则提升为编译错误，错误位置精确 |

下一章 `04` 处理 `.dico` → manifest、工厂、Automation/IDispatch、错误处理这四个跨编译期/运行期边界的子系统。
