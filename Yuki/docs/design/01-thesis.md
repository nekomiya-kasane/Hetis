# 01 — 立论与七大支柱

## 1.1 历史约束 vs 现代工具

CAA V5 的设计冻结于 2000 年。彼时 C++ 没有反射、没有概念约束、没有模块、没有 attribute 元数据；唯一能做"编译期生成 + 运行期注册"的工具是宏 + 静态对象构造。CAA 的几乎全部技术债都源于这一历史约束：

- TIE 中介对象（解决"impl 想实现多接口但 C++ 单继承"——用宏生成转发桥）
- `.dico` 文本表（解决"动态发现实现"——用文件系统作为元数据库）
- `CATImplementClass` 静态注册器（解决"编译期类型描述要在运行期可见"——用 `dlopen` 触发的全局对象构造）
- `_var` 一接口一智能指针（解决"RAII"——用宏批量生成 wrapper 类）
- HRESULT（解决"C++ 异常跨 ABI 不可靠"——用整数错误码）
- metaobject chain 链表式 QI（解决"OM 继承的运行期表示"——用单链表 walk）
- IDispatch DISPID 派发表（解决"脚本按名调用"——用类型库 + 运行期查表）

C++26 改变了这些机制每一项的可能性边界：

| C++26 特性 | 取代的 CAA 机制 |
|---|---|
| P2996 反射（`^^E`、`std::meta::info`、`members_of`、`bases_of`） | 手工列接口列表、metaobject 链 |
| P3289 consteval 块 | 类作用域不变量校验、静态构造器 |
| P3385 注解反射（`[[=value]]`） | 宏关键字位置参数（`Implementation` / `DataExtension` / `CodeExtension`） |
| P1306 expansion statements（`template for`） | 宏批量展开（TIE 转发表、`_var` 类生成） |
| P3096 参数反射 | 跨语言绑定的参数 marshaling |
| `std::define_aggregate` | 反射驱动的 adapter 类合成（替代 TIE 宏类） |
| consteval STL（`vector` / `unordered_map` / `string`） | 编译期完美哈希构建 |
| C++20 concepts | 运行期 `IsAKindOf` 字符串比较 |
| C++20 modules | `Public/Protected/Private/Local Interfaces` 目录约定 |
| `std::expected<T, E>` | HRESULT |

---

## 1.2 2026 年的现代参照系

### C# `[GeneratedComInterface]`（.NET 8+）

```csharp
[GeneratedComInterface, Guid("…")]
partial interface IFoo
{
    int Length { get; set; }
}
```

由 Roslyn 源生成器编译期产出 vtable shim、QI/AddRef/Release、marshaling 代码。**无运行时反射，无 RCW/CCW 中转**，直接走 native COM ABI。所有"接口注册"事实落在编译期常量表里。这与 CAA 的 mkmk 在概念上同构——但 mkmk 是外部工具，源生成器是同语言能力。

### C++/WinRT

```cpp
struct MyImpl : winrt::implements<MyImpl, IFoo, IBar, IBaz> { /* ... */ };
```

`winrt::implements` 把 D 实现的多个接口打成单一对象，QI 表在编译期由模板展开生成。`winrt::com_ptr<T>` 取代逐接口 smart pointer。C++/WinRT 还有 `cppwinrt.exe` 工具——它本质是源生成器，C++26 反射下可以**完全消解**。

### ATL（反例）

```cpp
BEGIN_COM_MAP(CMyClass)
    COM_INTERFACE_ENTRY(IFoo)
    COM_INTERFACE_ENTRY(IBar)
END_COM_MAP()
```

这是宏组装 `(IID, offset)` 数组、QI 线性扫描——正是 P2996 要消灭的"宏元编程派发表"。**ATL 风格的 CAA 重写就是反向退步**。

### 关键趋势

| 旧路 | 新路 |
|---|---|
| 宏 + 静态对象注册 | 反射 + consteval 编译期表 |
| 字符串/GUID 字典 | 类型即身份 + 编译期完美哈希 |
| 运行期派发表线性扫描 | 编译期完美哈希或直接静态派发 |
| 每接口一个 smart pointer 类 | 单泛型 `intrusive_ptr<T>` |
| HRESULT | `std::expected<T, E>` |
| 外部 IDL 编译器 | 同语言 consteval 内省 |

---

## 1.3 七大支柱总览

完整重写沿七根支柱展开。每根支柱解决 CAA 一类历史负担，并保证不引入新的运行期开销。

| # | 支柱 | 替换的 CAA 机制 | 新机制 | 文档 |
|---|---|---|---|---|
| 1 | 身份与 IID | 16-byte GUID 比较 | `consteval` 计算的 64-bit 稳定哈希；GUID 仅在 COM 边界 | 02 |
| 2 | 接口 | `CATDeclareInterface` + `CATImplementInterface` + `IID_*` + `.tsrc` | 注解 + consteval 块 + 反射枚举 + 内置不变量校验 | 02 |
| 3 | 组件与扩展 | `CATImplementClass(…, Implementation/DataExtension/CodeExtension, …, …)` 宏 | `[[=Component{...}]]` 注解 + 反射推导关系 | 02 |
| 4 | 派发 | metaobject chain walk + TIE 中介对象 + `.dico` 字符串查找 | 编译期完美哈希派发表 + 类型擦除时单一间接 | 03 |
| 5 | TIE / BOA 融合 | 二分形态选择（每类至多 1 BOA + N TIE） | 反射检测，能 `static_cast` 就退化（"BOA"），否则编译期生成 adapter（"TIE"），客户无感 | 03 |
| 6 | 所有权 | 一接口一 `_var` + 五条避坑列表 | 单泛型 `intrusive_ptr<T>` + 概念约束让误用变编译错误 | 03 |
| 7 | Determinism | mkCheckSource 工具运行期不可测 | consteval 块在类型定义现场报编译错 | 03 |

---

## 1.4 系统级机制

四个跨编译期/运行期边界的子系统在第 04 章展开：

- `.dico` → manifest（编译期烤好的描述符 + 运行期合并）
- 工厂（静态已知时零间接，类型擦除时完美哈希查工厂指针）
- Automation/IDispatch（仅在 COM 互操作边界存在；同语言绑定走反射驱动 ahead-of-time 生成）
- 错误处理（内部 `std::expected<T, E>`，边界处单点转 HRESULT）

---

## 1.5 设计原则的最终形态

**编译期可知的全部进 `consteval` 与反射；运行期才能知道的（动态加载的插件清单）只保留单一 manifest 符号导出这一最小化运行期 ABI。**

这条原则推论出三件事：

1. **没有外部代码生成器**：CAA 必须的 `mkmk + .tsrc + .tplib + IDL compiler` 整套链路在新设计里被同语言反射替代。构建系统只做"编译 + 链接 + 收集 manifest section"三件事。
2. **没有静态构造器副作用注册**：CAA `CATImplementClass` 通过 dlopen 触发全局对象构造来登记类身份；新设计把所有类身份烤进 inline constinit 的 `PluginManifest` 符号，链接器或运行期 fold-in 时聚合。Static Initialization Order Fiasco 不存在，因为没有静态初始化序。
3. **没有运行期字符串元数据**：CAA 的 `IsAKindOf("CmpName")`、`.dico` 字符串查找、`GetIDsOfNames` 字符串方法名、ProgID 字符串注册——这些**全部**在编译期变 `std::meta::info` + 反射哈希，运行期只见 64-bit 比较。

---

## 1.6 必须诚实承认的设计紧张关系

不要假装 C++26 能解决所有问题。以下三处是真实工程边界，不是设计妥协：

### P3687 删除了 splice template argument

无法直接 `Adapter<[:^^I:], Impl>`。绕道：用 `meta::substitute(^^Adapter, {^^I, ^^Impl})` 拼模板实参得到 `^^info`，再 `[:…:]` 取类型。可行但写法不如 C++29 后干净。架构正确但实现期短期内多一层。

### 跨 TU 的 consteval 累积

标准本身没有"全局 consteval 数组"机制；只能依赖 link-time section（gcc/clang 的 `__attribute__((section))` + `__start_/__stop_`）或 C++26 模块的 init 顺序。可移植性比"理想中的反射宇宙"略弱——这是 ABI 现实，无法用语言特性独自解决。

### 编译期烤出的 manifest 进入二进制

对极致 binary size 敏感的目标（嵌入式 / WASM），需要按 framework 切片裁剪。CAA 的 `.dico` 是惰性加载，新方案是 eager 但编译期固化——大多数 desktop 场景净收益，特殊场景需要权衡。

这三条都不影响**架构正确性**，只影响**实现路径长度**。CAA 的所有架构债（metaobject chain walk、TIE 中介对象、字符串字典、`_var` 雷区列表、HRESULT、双 OM-derive 不可决定性）在新设计里全部消失——净收益压倒性。

---

## 1.7 与 CAA 互操作的策略

不要重写就开战。保留**单向桥**：

```cpp
template <typename I> intrusive_ptr<I> from_com(IUnknown* raw);
template <typename T> ComPtr<IDispatch> to_com(intrusive_ptr<T> obj);
```

桥接代码本身由反射生成（CAA 接口的 IDL 是同语言 reflection 的子集）。客户在新代码里**完全不接触** HRESULT / GUID / `_var`；遗留代码原样存在直到自然替换。详见第 06 章。

---

下一章 `02-pillars-identity-interface-component.md` 展开支柱 1–3。
