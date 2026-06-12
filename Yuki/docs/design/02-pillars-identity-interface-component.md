# 02 — 支柱 1–3：身份、接口、组件

## 2.1 支柱 1 — 身份与 IID

### 2.1.1 设计目标

| 角色 | 需求 |
|---|---|
| 内部派发 | 极快比较，最好是单一整数；无字符串、无注册中心 |
| 跨 ABI 边界 | 稳定标识符，跨进程、跨 .so/.dll 装载、跨语言绑定一致 |
| COM 互操作 | 与现有 GUID 一一对应，可被 Windows Registry / Type Library 引用 |

CAA 把这三件事**合一**用 16-byte GUID 解决，结果是内部派发也付了 GUID 比较的代价。新设计**正交化**：

### 2.1.2 内部 IID

```cpp
struct InterfaceId {
    std::uint64_t hash;
    consteval bool operator==(InterfaceId) const = default;
    consteval auto operator<=>(InterfaceId) const = default;
};

template <typename I>
inline constexpr InterfaceId iid_v = []{
    consteval {
        // 完整限定名 + 接口 ABI 摘要（方法签名列表）→ SHA-256 → 截断 64-bit
        return InterfaceId{ stable_hash(
            qualified_name_of<I>(),
            abi_signature_of<I>())};
    }
}();
```

- `qualified_name_of` 用 `std::meta::display_string_of(^^I)` 配合 `parent_of` 链取出完整命名空间路径。
- `abi_signature_of` 用反射枚举接口的方法序列，对每个方法的返回类型 + 参数类型列表做哈希。**接口签名变 ⇒ 哈希变 ⇒ 不会与旧客户错连**。
- 哈希在编译期固化，跨进程稳定。
- 64-bit 比较是单条 `cmp` 指令；GUID 比较是 16-byte memcmp。

### 2.1.3 COM 边界 IID

需要与 COM 互操（写出 `.tlb`、注册到 Windows Registry、跨 IDispatch）时，用注解显式钉住 GUID：

```cpp
[[=Iid("DCE:7c1b4ba8-5c25-0000-0280020bcb000000")]]
[[=Interface]]
struct IDataLength { /* ... */ };
```

反射读取覆盖默认哈希。这是**单一开关**而非双轨——内部仍用 `iid_v<IDataLength>.hash` 做派发，只有在 `to_com<T>` 边界 thunk 里把 hash 换成 GUID。

### 2.1.4 哈希碰撞

64-bit 完美哈希在百万级接口数量下碰撞概率约 $2^{-44}$。manifest 构建期对所有已知接口做 consteval 全集校验：

```cpp
consteval void verify_no_iid_collision(std::span<const InterfaceDescriptor> all) {
    auto sorted = sort_by_hash(all);
    for (size_t i = 1; i < sorted.size(); ++i)
        if (sorted[i].hash == sorted[i-1].hash)
            report_error("IID collision: ", sorted[i].name, " vs ", sorted[i-1].name);
}
```

碰撞 = 编译错误。如发生，开发者把其中一个改名或加 `[[=Iid("…")]]` 显式钉死。

---

## 2.2 支柱 2 — 接口

### 2.2.1 抛弃

| CAA 形态 | 抛弃理由 |
|---|---|
| `CATDeclareInterface` 宏 | 注解 + consteval 块覆盖 |
| `CATImplementInterface(I, B)` 宏 | C++ 继承本身已表达；OM-derive 由反射读 |
| 独立 `.cpp` 写 IID | 反射哈希自动得；显式钉死走注解 |
| 独立 `.tsrc` 求生成 TIE 头 | 反射 + define_aggregate 在 consteval 期合成 adapter |
| `HRESULT __stdcall` 强制 | 内部用 `std::expected<T, Error>`；COM 边界单点转换 |

### 2.2.2 保留

- 接口是行为契约；`virtual` 派发；发布后不可改签名。
- 接口可以 OM-derive 自另一接口（C++ 继承同步表达）。

### 2.2.3 接口声明

```cpp
[[=Interface{.stability = Stability::Frozen}]]
struct IDataLength {
    virtual std::expected<int, Error>  length() const = 0;
    virtual std::expected<void, Error> set_length(int) = 0;

    consteval {
        verify_interface_invariants(^^IDataLength);
    }
};
```

`Interface` 是用户自定义注解类型（P3385）：

```cpp
struct Interface {
    Stability stability = Stability::Frozen;
    bool      automation_exposed = false;
    bool      automation_journalizable = false;
};
```

`verify_interface_invariants` 是 consteval 函数，用反射枚举成员检查：

- 全部成员函数 pure virtual。
- 返回类型形如 `std::expected<…, Error>` 或 `void`。
- 无 NSDM（non-static data member）。
- 单 C++ 继承，且基类要么是另一个 `[[=Interface]]` 类型，要么是 `Object`（运行期擦除时的根）。
- 标记 `Frozen` 的接口会在 manifest 上记录其 ABI 签名摘要——后续构建若签名变化，编译失败并报"已冻结接口被改"。

任何一项违反 ⇒ `static_assert` 失败，错误信息指向具体方法。**CAA 必须靠人工纪律 + mkCheckSource 才能保证的事情，编译失败即拦截**。

### 2.2.4 接口 OM-derive

```cpp
[[=Interface]]
struct IBase {
    virtual std::expected<void, Error> base_op() = 0;
};

[[=Interface]]
struct IDerived : IBase {
    virtual std::expected<void, Error> derived_op() = 0;
};
```

C++ 继承即 OM-derive，**单一事实来源**。CAA 那种"必须 C++ 上 derive **且** 宏上 `CATImplementInterface(D, B)` 同步"的双轨制消失。反射 `bases_of(^^IDerived)` 直接给出基接口集合。

`verify_interface_invariants` 中追加：基接口若不是 `[[=Interface]]` 标注（说明只是 C++ 抽象签名共享类，不进 OM 派发），允许编译，但派发表只为 `IDerived` 本身建条目，不为 `IBase` 建——这正好是 cpp_rules 推荐的"用纯 C++ 抽象基类做签名共享"路径。

---

## 2.3 支柱 3 — 组件与扩展

### 2.3.1 抛弃

| CAA 形态 | 抛弃理由 |
|---|---|
| `CATImplementClass(Cls, Implementation, OMBase, Extended)` | 关键字字符串、位置参数、对扩展无意义陷阱 |
| `Implementation` / `DataExtension` / `CodeExtension` 关键字 | 改用强类型 enum 注解 |
| 头文件 `CATDeclareClass` 宏 | 注解自带，反射可读 |
| Extension 第三参数填 `CATBaseUnknown` 或 `CATNull` 的歧义 | 反射推导，无第三参数 |
| 共享扩展三宏组合（`CATBeginImplementClass / CATAddClassExtension / CATEndImplementClass`） | 注解携带集合参数 |
| `CATSupportImplementation(Ext, Cmp, Itf)` | 同上 |

### 2.3.2 保留

- main impl + N 个 extension 的物理拆解。
- DataExtension（per-instance 状态）vs CodeExtension（无状态共享）的运行期区分。
- 共享扩展的双向声明能力（扩展端声明 / 被扩展端声明）。

### 2.3.3 组件声明

```cpp
struct Component {
    enum Kind { Main, DataExtension, CodeExtension };
    Kind kind = Main;
    std::meta::info om_base = ^^void;     // 仅 Main 有效
    std::meta::info extends = ^^void;     // 仅 *Extension 有效，单宿主
    std::span<const std::meta::info> extends_many = {};  // 仅 *Extension 有效，多宿主
};

[[=Component{.kind = Component::Main, .om_base = ^^Object}]]
struct CircleImpl : intrusive_ref_counted<CircleImpl> {
    consteval { verify_component(^^CircleImpl); }
    /* ... */
};

[[=Component{.kind = Component::DataExtension, .extends = ^^CircleImpl}]]
struct CircleMoveExt : ICircleMove {
    Vec3 _offset{};
    std::expected<void, Error> translate(Vec3 v) override {
        _offset += v; return {};
    }
};

[[=Component{.kind = Component::CodeExtension, .extends = ^^CircleImpl}]]
struct CircleDrawExt : ICircleDraw {
    consteval { verify_no_nsdm(^^CircleDrawExt); }
    std::expected<void, Error> draw() const override { /* ... */ }
};
```

要点：

1. **单一注解**取代 4 参宏，错误使用直接是类型错误（`extends` 是 `std::meta::info` 而非字符串，写错的类型名编译失败）。
2. **OM-base 仅对 Main 有效**：扩展若试图填 `om_base` ⇒ `verify_component` 报错。CAA 那条"对扩展无意义但必须填 `CATBaseUnknown`"的陷阱消失。
3. **CodeExtension 编译期校验无 NSDM**：CAA 文档要求但靠纪律；这里 `verify_no_nsdm` 强制。
4. **共享扩展直接用集合**：

```cpp
[[=Component{.kind = Component::DataExtension,
             .extends_many = std::array{^^Cmp1, ^^Cmp2, ^^Cmp3}}]]
struct SharedExt : IShared { /* ... */ };
```

完全消灭 `CATBeginImplementClass / CATAddClassExtension / CATEndImplementClass` 三宏。

### 2.3.4 类身份注册

CAA 走 `CATImplementClass` 全局静态对象的构造副作用做注册——`dlopen` 触发全局构造时把"类身份+扩展关系"登记进 OM 类簿。这条路有两个问题：

1. **Static Initialization Order Fiasco**：跨 lib 注册顺序敏感，`.dico` 和 lib 加载次序影响 QI 行为（cpp_rules 文档明确的雷区）。
2. **运行期开销**：每个 `CATImplementClass` 都是一个全局对象 + 一次容器插入。

新方案**完全不依赖**静态构造：

```cpp
// 每个组件/扩展类在其 consteval 块里把自身描述符注册到 inline constinit manifest section
[[=Component{...}]]
struct CircleImpl {
    consteval {
        register_component(^^CircleImpl);
    }
};
```

`register_component` 把当前类的 `ComponentDescriptor` 写进一个**链接器 section**（详细机制见第 04 章 manifest 设计）。链接期所有 TU 的 section 拼成一张表，运行期只读，无构造序、无插入。

类身份元数据（取代 `IsAKindOf` / `ClassName`）由反射直接给：

```cpp
template <typename T>
constexpr std::string_view class_name = std::meta::display_string_of(^^T);

template <typename T, typename Base>
constexpr bool is_a_kind_of = std::derived_from<T, Base>;  // 编译期决议
```

类型擦除场景下（拿到的是 `Object*`，不知道具体 T），运行期通过 manifest 的 `ComponentDescriptor::om_base_chain` 字段查询，**单次 hash 比较 + 单次数组遍历**，比 CAA 的 metaobject chain walk 短一个数量级。

### 2.3.5 实例化两条路

**静态已知**：

```cpp
auto c = make<CircleImpl>(/* ctor args */);  // 返回 intrusive_ptr<CircleImpl>
```

零字符串、零间接、可 inline；`make<T>` 内部就是 `new T(args)` + `intrusive_ptr` 包装。

**类型擦除**（来自配置、命令、脚本的请求）：

```cpp
auto obj = spawn(component_id_t{0xABCD…}, args_view);
```

`component_id_t` 是反射哈希得到的 64-bit 组件标识（与接口 IID 同机制）。`spawn` 用 manifest 的完美哈希查到工厂函数指针，直接调用。仍无字符串。

CAA `CATInstantiateComponent("CmpName", IID, …)` 那条字符串查找路径**消失**——客户即便从配置文件读组件名，也是在配置加载期通过 `consteval` 帮表把字符串映射到 `component_id_t`，运行期不再做字符串比较。

### 2.3.6 工厂

CAA 必须实现 `CATICreateInstance` 才能让 `CATInstantiateComponent` 工作——这本身是个绕路。新设计：

```cpp
template <typename Cmp>
constexpr Factory factory_of = []{
    consteval {
        // 反射枚举 Cmp 构造函数；对每个构造函数生成一个工厂 entry
        return build_factory<Cmp>();
    }
}();
```

manifest 里 `ComponentDescriptor::factory` 直接指向 `factory_of<Cmp>` 之一。无 `CATICreateInstance` 接口、无 CodeExtension 挂载、无客户感知。

### 2.3.7 OM 组件继承

CAA 允许 main class 间 OM 继承（派生组件自动暴露基组件实现的所有接口）。新设计直接用 C++ 继承表达：

```cpp
[[=Component{.kind = Component::Main, .om_base = ^^BaseCmp}]]
struct DerivedCmp : BaseCmp {
    /* ... */
};
```

`om_base` 必须等于 C++ 直接基类（`verify_component` 校验）。派发表构建期 (`dispatch_table<DerivedCmp>`) 通过反射 `bases_of(^^DerivedCmp)` 获取基类，把基类的接口实现自动并入 `DerivedCmp` 的派发表——**无需重声明 TIE，无需更新 dico**。这是 CAA 那条"OM 组件继承让 QI 自动支持基组件接口"语义的零开销实现。

---

## 2.4 关键设计取舍

### 接口 vs 抽象基类

cpp_rules 文档原话："只让 IA 是 C++ 抽象类，**不要让它是接口**"——这个反直觉建议在新设计里成为**默认行为**：

- 不带 `[[=Interface]]` 注解的抽象基类只参与 C++ 静态派发，不进入 OM。
- 带注解的才进入 manifest，才参与 QI。

开发者**主动选择**何时让一个抽象类进入 OM 派发——避免 CAA 那种"无意中让两个接口 OM-derive 自同一基"的不确定性。

### 注解 vs concept

注解 `[[=Component{...}]]` 携带数据；concept 描述类型必须满足的约束。两者不是替代关系，而是组合：

```cpp
template <typename T>
concept ComponentMain = std::meta::has_annotation_v<^^T, Component>
                     && component_kind_of<T> == Component::Main;
```

`make<T>` 用 `requires ComponentMain<T>` 约束模板实参——**未声明为 main 的类无法当 main 实例化**，错误信息直接定位到 `make<T>` 调用点。

下一章 `03` 展开支柱 4–7（派发、TIE/BOA 融合、所有权、Determinism）。
