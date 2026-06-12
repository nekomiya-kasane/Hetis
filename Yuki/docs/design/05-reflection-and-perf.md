# 05 — 反射特性映射 + 性能账本

## 5.1 反射 / 注解 / consteval 块各司其职

CAA 用宏 + 静态对象 + mkmk 生成文件做的事，新设计分裂为两类：编译期可知的全部进 `consteval` 与反射；运行期才能知道的（动态加载的插件清单）只保留单一 manifest 符号导出这一最小化运行期 ABI。

下表是单点映射，每行对应 CAA 一个机制 → 新设计如何用具体 C++ 特性替代。

| C++26 工具 | 角色 | 替代的 CAA 形态 |
|---|---|---|
| `[[=Component{.kind, .om_base, .extends, .extends_many}]]` | 强类型组件元数据 | `CATImplementClass(…, Implementation/DataExtension/CodeExtension, …, …)` 关键字字符串与位置参数 |
| `[[=Interface{.stability, .automation_exposed, .automation_journalizable}]]` | 接口元数据 | `CATDeclareInterface` + `CATImplementInterface` |
| `[[=Iid("DCE:…")]]` | COM 边界 IID 钉死 | `extern IID IID_*` 全局变量 + `.cpp` 赋值 |
| `[[=Exposed]] / [[=ComExposed{.iid, .progid}]]` | Automation/COM 暴露开关 | `.idl` `#pragma ID/DUAL/ALIAS` + `.tplib` |
| 类作用域 `consteval { … }` 块（P3289） | 类定义现场跑不变量校验、组件登记、phf 构建 | `CATImplementClass` 全局静态对象副作用注册 |
| `std::meta::info` + `^^E` reflect 算子 | 类型/接口/成员的一等编译期值 | C++ 类型在运行期靠 `IsAKindOf("CmpName")` 字符串自报 |
| `std::meta::members_of` / `bases_of` / `nonstatic_data_members_of` | 反射枚举接口方法、OM 继承基、扩展数据成员 | 手写 TIE 宏对每个方法做转发；手列接口方法 |
| `std::meta::display_string_of` / `identifier_of` | 名称取得（IID 哈希源、绑定名） | mkmk 生成代码里的字符串字面量 |
| `std::meta::annotations_of` / `annotation_of_type` | 读组件/接口注解 | 读 `.dico` 文本表 |
| `std::define_aggregate` | 反射驱动合成 adapter / vtable / dispatch_table | TIE 宏展开生成中介类 |
| `template for` expansion statement（P1306） | adapter 内部对接口方法做编译期展开转发 | 宏批量展开 |
| `std::meta::substitute` | 间接 splice 模板实参（P3687 缺位的绕道） | 不适用 |
| 编译期完美哈希（基于 P2996 + constexpr STL） | 接口 ID → dispatch slot；DISPID → 方法表 | metaobject chain walk + `.dico` 字符串查找 + `GetIDsOfNames` 字符串查 |
| C++20 concepts（`ComponentMain` / `Interface` / `Adapter`） | 模板实参约束 | 运行期 `IsAKindOf` 字符串比较 |
| C++20 modules（`export module Yuki.*`） | 可见性边界 | `Public/Protected/Private/Local Interfaces` 头目录约定 |
| `std::expected<T, Error>` | 错误返回 | `HRESULT __stdcall + SUCCEEDED/FAILED` |
| `std::source_location` | Error 的源位置链 | 无（CAA Error 仅整数码） |
| inline `constinit` + 链接器 section | 跨 TU manifest 聚合 | 全局静态构造对象注册 |
| `std::atomic<uint32_t>` 非虚 retain/release | 引用计数 | virtual `AddRef`/`Release` |
| `intrusive_ptr<T>` 单泛型 | 共享所有权 | 一接口一 `_var` 类 |

## 5.2 关键模式：consteval 流水线

新设计的所有元数据从源码到 manifest 走同一条 consteval 流水线。下面给一份"端到端"骨架（伪代码，仅展示数据流）：

```cpp
// 1. 类定义现场
[[=Component{.kind = Component::Main, .om_base = ^^Object}]]
struct CircleImpl : intrusive_ref_counted<CircleImpl>, ICircle {
    consteval {
        verify_component(^^CircleImpl);
        // 校验通过即把 ComponentDescriptor 写进 section
        register_component_descriptor<CircleImpl>();
    }

    Result<double> radius() const override { return _r; }
private:
    double _r;
};

// 2. 编译期：register_component_descriptor 触发
template <typename Cmp>
consteval ComponentDescriptor build_descriptor() {
    return {
        .id            = component_id_v<Cmp>,
        .qualified_name = display_string_of(^^Cmp),
        .kind          = component_kind_v<Cmp>,
        .om_base       = component_id_v<om_base_t<Cmp>>,
        .implemented   = collect_directly_implemented_interfaces<Cmp>(),
        .factory       = factory_of_v<Cmp>,
        .vtable        = &dispatch_vtable_v<Cmp>,
    };
}

// 3. 链接期：所有 [[gnu::section("yuki_components")]] 拼成一段
//    宿主通过 __start_/__stop_ 符号读取整段

// 4. 运行期：registry::load_plugin fold-in 这段到全局 phf
```

每一步都是同语言、零外部工具、零生成文件落盘——这与 CAA `mkmk + .tsrc + .tplib + IDL compiler` 那条工具链路构成最大对比。

## 5.3 性能账本

下表逐项对比 CAA V5 与新设计在常见路径上的开销。所有"重写后"列假设静态类型已知时编译器做 LTO devirtualize，类型擦除时走 phf。

| 操作 | CAA V5 | 重写后 | 备注 |
|---|---|---|---|
| `QueryInterface` 同 lib 已知类型 | 1 vcall + N-step chain walk + 16-byte IID memcmp | 0 间接（编译期决议为 `static_cast`）或 1 数据成员访问 | N = OM-base 链长 |
| `QueryInterface` 同 lib 类型擦除 | 同上 | 1 phf lookup（≈3 cycle）+ 1 thunk 调用 | thunk 通常被 inline |
| `QueryInterface` 跨 lib 未加载 | 同上 + dlopen + 字典字符串查找 + 静态构造器序列 | 同上 + 1 manifest 完美哈希 + 1 dlopen | dlopen 不可避免；其他路径短两个数量级 |
| 接口方法调用（已持有接口指针） | TIE thunk vcall → impl vcall | 直接 vcall（adapter 退化）或 1 thunk 调用 | adapter 编译器可全 inline |
| `AddRef` / `Release` | virtual 调度 + `InterlockedIncrement` | 非虚 + `std::atomic` relaxed/acq_rel | 静态类型已知时编译器去虚化；同步语义按 `boost::intrusive_ptr` 标准内存序 |
| `IsAKindOf` | 字符串 `strcmp` + chain walk | concept check（编译期）或 1 bit-set / 短数组遍历（运行期擦除时） | 95% 用法是编译期 |
| IID 比较 | 16-byte memcmp | 8-byte 单条 `cmp` 指令 | 哈希碰撞由 manifest 构建期 static_assert |
| `IsEqual`（实例同一） | `QI(IID_IUnknown) × 2` + 指针比较 | 直接 `Object::impl ==` | 类型擦除 Object 的 `impl` 指针即 canonical id |
| 工厂调用（已知组件） | 字典查 + dlopen + `CATICreateInstance::CreateInstance` vcall | 静态 `make<T>` 零间接 | 静态时完全 inline |
| 工厂调用（类型擦除） | 同上 | 1 phf lookup + 函数指针调用 | `args_view` 解包代价正比于参数数 |
| `.tplib` / 类型库读取 | 启动时载入二进制 `.tlb` | 零（manifest 已在 binary 内） | 启动期消失 |
| Determinism violation 检测 | mkCheckSource 工具 + 运行期不可测 | 编译错误，错误位置精确 | 从工程纪律变为类型系统约束 |
| 接口方法返回值 | HRESULT 整数 + out param | `std::expected<T, Error>` ABI 等价 | 大多数情况 ABI 同 HRESULT；带源位置 |
| `IDispatch::Invoke`（仅 COM 边界） | `GetIDsOfNames` 字符串查 + `Invoke` vcall + CAA 内部派发 | DISPID phf 查 + 直接函数指针调 | 仅在 `[[=ComExposed]]` 接口生效 |

## 5.4 二进制大小账本

新设计在编译期烤出 `dispatch_vtable` / `PluginManifest` / phf 表 / abi_freeze_snapshot，进二进制 `.rodata`：

| 项 | 体积估计（每接口 / 每组件） |
|---|---|
| `InterfaceDescriptor` | ≈80 B 固定 + 名字字符串（共享池） + bases span 指针 |
| `ComponentDescriptor` | ≈64 B 固定 + 接口列表 |
| `dispatch_vtable` | phf 表 ≈ `2 × N × 8 B`（CHD displacement + value），N = 该组件接口数 |
| abi_freeze_snapshot | 32 B / 接口（SHA-256 截断） |

典型组件（10 接口）：约 1 KB / 组件。1000 个组件：约 1 MB。这与 CAA 的 `.dico` 文本（同等组件数约几百 KB 文本）相比略大，但**消除运行期字符串解析、消除静态构造序、消除 dlopen 顺序敏感**。

对极致 binary size 敏感的目标（嵌入式 / WASM），可按 framework 切片裁剪：未启用的 framework 的 manifest 段不进二进制。

## 5.5 编译期开销

完美哈希构造（CHD 算法）在 consteval 下能跑得动 ~10⁴ 键集。一个组件的接口数极少超过 50；典型项目所有接口加起来 <1000。实测（参考 Bloomberg fork clang-p2996 性能基线）：

- 反射枚举一个 50 接口组件的成员、生成 phf：< 100 ms。
- 全项目（1000 接口 + 500 组件）manifest 构建：增量编译 < 5 s，从零编译 < 30 s。

CAA mkmk 全量编译同等规模项目通常需要 5–15 分钟（包含 `.tsrc` / `.tplib` / IDL 编译多轮）。**新设计编译期开销至少快一个数量级**——这是消除外部代码生成器的直接收益。

## 5.6 运行时开销小结

把"重写后"列与 CAA V5 列做柱状图直觉化：

```
QI 已知类型      : CAA ████████████  vs  新 □
QI 类型擦除      : CAA ████████████  vs  新 ███
QI 跨 lib 未加载 : CAA ████████████████████  vs  新 ████ + dlopen
方法调用        : CAA ███  vs  新 ██ 或 □
AddRef/Release  : CAA ███  vs  新 ██
工厂(静态)      : CAA ████████  vs  新 □
工厂(擦除)      : CAA ████████████  vs  新 ███
启动(读类型库)  : CAA ████  vs  新 □
```

**所有热路径都比 CAA 更快或等价**；冷路径（首次 QI 触发 dlopen）也快若干量级。最重要的是没有任何"为兼容旧机制保留的运行期开销"——CAA 必须保留 metaobject chain 是因为 mkmk 可能给 Extension 选错 OM-base，新设计把这一类问题在编译期消灭。

## 5.7 与 C# `[GeneratedComInterface]` / C++/WinRT 的等位对照

| 维度 | C# .NET 8 GeneratedComInterface | C++/WinRT | Yuki 新设计 |
|---|---|---|---|
| 生成时机 | Roslyn 源生成器（编译期） | 模板 + cppwinrt.exe（编译期 + 工具） | C++26 反射 + consteval（编译期，零外部工具） |
| 接口注册 | 静态常量表 | `winrt::implements` 模板展开 | inline constinit + 链接器 section |
| QI 派发 | 直接 vtable | 模板展开成静态 if-chain | phf O(1) |
| 引用计数 | CCW/RCW | `winrt::com_ptr<T>` | `intrusive_ptr<T>` |
| 错误处理 | `HResultException` | `winrt::hresult_error` | `std::expected<T, Error>` |
| 跨语言绑定 | NativeAOT + COM | WinRT projection | 反射驱动 ahead-of-time 各语言生成 |
| 中间生成文件 | C# 源生成器输出 | `.h` / 投影文件 | 零 |

新设计的最大特点是**完全消解外部工具链**——这是 C++26 反射独有的能力，C# 源生成器和 cppwinrt 仍是独立工具。

下一章 `06` 处理与 CAA / COM 的互操作以及 C++26 必须诚实承认的边界。
