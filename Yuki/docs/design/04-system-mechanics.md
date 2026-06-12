# 04 — 系统机制：manifest、工厂、Automation、错误处理

本章处理四个跨编译期/运行期边界的子系统。每一个都是 CAA 大规模技术债的源头，也都是 C++26 反射 + consteval 块能彻底重写的对象。

## 4.1 `.dico` → 编译期组件登记 + 运行期插件清单

### 4.1.1 CAA 的两件事

`.dico` 文本表同时承担两件事：

1. **静态注册**：哪个组件实现哪个接口，由哪个 lib 提供 TIE 代码。
2. **动态加载**：客户调 `CATInstantiateComponent` 时按需 `dlopen`。

这两件事的耦合是 CAA 设计的关键弱点——开发者必须手工保持 `.dico` 行与 `.cpp` 里 `CATImplementClass + TIE_*` 调用一致；不一致就触发 cpp_rules 文档里那张警告图（`Cmp2->QI(IID_IA)` 错走 `Cmp1` 的 TIE_IA）。

### 4.1.2 新设计：编译期完成 (1)，运行期完成 (2)

**(1) 编译期**：每个翻译单元的 `consteval` block 把组件描述符写入 `inline constinit` 数组，链接器把所有 TU 的描述符聚合成一张完美哈希索引的 manifest，作为静态库元段嵌入产物。

**(2) 运行期**：宿主启动扫描 `plugins/*.so`，每个 `.so` 导出一个 `extern "C" const PluginManifest caa3_plugin_manifest;`——这本身是 consteval 烤出来的常量；宿主合并到运行期 manifest。

**不读文本文件，不写注册器全局对象，不依赖静态构造序。**

### 4.1.3 数据结构

```cpp
struct InterfaceDescriptor {
    InterfaceId  id;
    std::string_view qualified_name;
    std::array<std::byte, 32> abi_hash;       // ABI 摘要，冻结校验用
    std::span<const InterfaceId> bases;       // OM-derive 自的接口
    InterfaceFlags flags;                     // exposed / journalizable / com-interop
    std::string_view com_iid_string;          // 仅 com-interop 接口非空
};

struct ComponentDescriptor {
    component_id_t id;
    std::string_view qualified_name;
    Component::Kind kind;
    component_id_t  om_base;                  // 仅 Main
    component_id_t  extends;                  // 仅 Extension
    std::span<const InterfaceId> implemented; // 自身直接实现
    Factory factory;                          // 静态已知的 ctor 入口
    const dispatch_vtable* vtable;            // 已编译期烤好
};

struct ExtensionDescriptor {
    component_id_t  extension_class;
    std::span<const component_id_t> hosts;    // extends_many 集合
    Component::Kind kind;                     // DataExtension / CodeExtension
    std::span<const InterfaceId> implemented;
};

struct PluginManifest {
    std::uint32_t schema_version;
    std::span<const InterfaceDescriptor> interfaces;
    std::span<const ComponentDescriptor> components;
    std::span<const ExtensionDescriptor> extensions;
    std::span<const std::byte> abi_freeze_snapshot;  // 接口冻结快照
};
```

### 4.1.4 链接器 section 收集

每个组件类型在编译时把自己的 `ComponentDescriptor` 写进固定 section：

```cpp
template <typename Cmp>
[[gnu::section("yuki_components"), gnu::used]]
inline constexpr ComponentDescriptor descriptor_of_v = build_descriptor<Cmp>();
```

`gnu::section + __start_yuki_components / __stop_yuki_components` 让链接器把所有 TU 的 descriptor 拼成一段连续内存：

```cpp
extern "C" const ComponentDescriptor __start_yuki_components[];
extern "C" const ComponentDescriptor __stop_yuki_components[];

inline std::span<const ComponentDescriptor> all_components() noexcept {
    return {__start_yuki_components,
            std::size_t(__stop_yuki_components - __start_yuki_components)};
}
```

**无静态构造序、无插入操作、零启动开销**——section 在 lib 加载时就直接可读。

跨平台抽象：

| 平台 | 机制 |
|---|---|
| Linux / macOS | `__attribute__((section, used))` + section start/stop 符号 |
| Windows MSVC | `#pragma section + __declspec(allocate)` + 头/尾哨兵符号 |
| Windows MinGW | 与 Linux 同 |

### 4.1.5 host 端聚合

```cpp
class registry {
    PerfectHashIndex<component_id_t, const ComponentDescriptor*> _components;
    PerfectHashIndex<InterfaceId,    const InterfaceDescriptor*> _interfaces;

public:
    void load_plugin(std::filesystem::path so_path);  // dlopen → 读 manifest 符号 → fold-in
    auto find(component_id_t) const -> const ComponentDescriptor*;
};
```

加载流程：

1. `dlopen(so_path)` → handle.
2. `dlsym(handle, "yuki_plugin_manifest")` 拿到 `const PluginManifest&`。
3. 校验 schema_version 与 abi_freeze_snapshot 兼容性（不兼容则 unload + 报错）。
4. 把 manifest 内容 RCU-style fold 进 `registry`——读端无锁（运行期热路径），写端单线程串行。

**与 CAA `.dico` 对比**：

| 维度 | CAA `.dico` | 新 manifest |
|---|---|---|
| 元数据格式 | 文本三列 | consteval 烤出的二进制结构 |
| 解析开销 | 每行字符串 split + 哈希 | 零（已是 phf 索引） |
| 完整性校验 | 运行期发现错配仅在 QI 失败时表现 | 加载时 abi_freeze_snapshot 一次校验 |
| 行序敏感性 | 有（cpp_rules 警告） | 无（顺序无关） |
| 可由人工编辑 | 是 | 否（消除一类错误来源） |

---

## 4.2 工厂

### 4.2.1 抛弃

- 实现 `CATICreateInstance` 接口。
- `CATInstantiateComponent("CmpName", IID, …)` 字符串查找路径。
- CodeExtension 挂载 `CATICreateInstance` 的绕路。

### 4.2.2 静态已知

```cpp
template <typename Cmp, typename... Args>
requires ComponentMain<Cmp> && std::constructible_from<Cmp, Args...>
inline intrusive_ptr<Cmp> make(Args&&... args) {
    return intrusive_ptr<Cmp>{new Cmp(std::forward<Args>(args)...), adopt};
}
```

零字符串、零间接、可 inline。

### 4.2.3 类型擦除

```cpp
intrusive_ptr<Object> spawn(component_id_t id, args_view args) {
    auto* d = registry::instance().find(id);
    if (!d || !d->factory) return {};
    return d->factory(args);  // 直接函数指针调用
}
```

`Factory` 是函数指针，在 `ComponentDescriptor` 编译期烤好：

```cpp
using Factory = intrusive_ptr<Object>(*)(args_view);

template <typename Cmp>
constexpr Factory factory_of_v = [](args_view args) -> intrusive_ptr<Object> {
    // 反射枚举 Cmp 构造函数，对 args 做 type-checked 解包
    return build_factory_thunk<Cmp>(args);
};
```

`args_view` 是一种轻量类型擦除参数容器（类似 `std::any` span，但带类型 tag），由前端配置/脚本系统填充。`build_factory_thunk` 在编译期为每个组件生成一个解包-构造函数。

### 4.2.4 component_id_t 的来源

客户即使从配置文件读组件名，字符串映射也在配置加载期通过 `consteval` 帮表完成：

```cpp
inline constexpr auto string_to_component_id_table =
    build_lookup_table<all_components()>();

component_id_t resolve(std::string_view name) {
    return string_to_component_id_table.at(name);  // 失败抛 expected
}
```

**运行期热路径不再做字符串比较**。

---

## 4.3 Automation / IDispatch

### 4.3.1 拆分两职责

CAA 的 `IDispatch / CATBaseDispatch / CATIABase / .tplib / DISPID` 同时解决两件事：

1. **跨语言绑定**：脚本 / 外语客户按名调方法、按名读写属性。
2. **COM 互操作**：和 VBA / VBScript / 老 OLE 客户通信。

新设计**正交化**两者。

### 4.3.2 跨语言绑定（同语言反射驱动）

不再依赖 `IDispatch` 这套动态派发协议。反射枚举 `[[=Exposed]]` 标注的接口，consteval 生成各语言的 binding：

```cpp
[[=Interface{.automation_exposed = true}]]
struct ICircle {
    virtual std::expected<double, Error> radius() const = 0;
    virtual std::expected<void, Error>   set_radius(double) = 0;
};
```

绑定生成器（按需生成 Lua / Python / C# / NodeJS …）在编译期跑：

```cpp
consteval auto generate_python_binding(std::meta::info iface) -> std::string;
consteval auto generate_lua_binding(std::meta::info iface)    -> std::string;
consteval auto generate_csharp_binding(std::meta::info iface) -> std::string;
```

输出落在编译期生成的 `.cpp` / `.cs` / `.py.h`（用 `std::define_static_string` 或编译期文本输出 + 后处理工具）。

**每个绑定都是 ahead-of-time 的，不用运行期 `GetIDsOfNames`**。`DISPID` 概念消失——名字到方法槽的映射在编译期由反射的 `std::meta::identifier_of` + 完美哈希解决。

### 4.3.3 COM 互操作（仅在边界）

仅在需要喂给老 VBA / VBScript / 老 OLE 客户的边界处生成 IDispatch shim：

```cpp
[[=Interface{.automation_exposed = true}]]
[[=ComExposed{.iid = "DCE:7c1b4ba8-…", .progid = "Yuki.Circle"}]]
struct ICircle { /* ... */ };
```

`ComExposed` 注解触发 consteval 同时生成：

- C++ 端的 IDispatch vtable shim（实现 `Invoke / GetIDsOfNames / GetTypeInfoCount / GetTypeInfo`）。
- 嵌入二进制的 `.tlb` 等价 metadata blob。
- DISPID → 方法表偏移的完美哈希（替代 CAA `CATBaseDispatch::Invoke` 的内部派发表）。

```cpp
template <typename Iface>
struct dispatch_shim : IDispatch {
    HRESULT __stdcall Invoke(DISPID id, /* ... */) override {
        constexpr auto table = build_dispid_phf<Iface>();
        auto slot = table.lookup(id);
        if (!slot) return DISP_E_MEMBERNOTFOUND;
        return slot->invoke(_target, /* args ... */);
    }
    // GetIDsOfNames / GetTypeInfoCount / GetTypeInfo 同理由反射生成
};
```

**`CATBaseDispatch` 这层基类消失**——consteval 自动接管"实现 IDispatch 四方法"职责。无人需要再继承 `CATBaseDispatch`。

### 4.3.4 `CATIABase` 的"对象通用元属性"

`get_Application` / `get_Parent` / `get_Name` / `GetItem` / `ObjFromId` 仍有意义，但作为**可选 mixin** 而不是强制基类：

```cpp
[[=Interface]] struct INamed     { virtual std::string_view name() const = 0; };
[[=Interface]] struct IHasParent { virtual intrusive_ptr<Object> parent() const = 0; };
[[=Interface]] struct IIndexed   { virtual intrusive_ptr<Object> item(std::string_view) const = 0; };
```

组件按需声明实现。CAA 把它们硬塞进 `CATIABase` 是因为 OLE 自动化客户需要统一查询面，但在反射体系下"统一查询面"自然由 `query_interface<IHasParent>` 完成——拿不到就是真的没父对象，与"没实现"语义统一。

---

## 4.4 错误处理

### 4.4.1 抛弃

- `HRESULT __stdcall foo(out T*)` 强制约定。
- `SUCCEEDED / FAILED` 宏检查。
- `CATErrorDef.h` 错误码常量大杂烩。
- 接口方法**必须**返回 HRESULT 的契约。

### 4.4.2 内部层

```cpp
struct Error {
    ErrorDomain          domain;       // 命名空间分类
    std::int32_t         code;
    std::string_view     message;      // 静态字符串或编译期字面量
    std::source_location location;
    const Error*         cause = nullptr;  // 错误链
};

template <typename T>
using Result = std::expected<T, Error>;
```

**`std::expected<T, Error>` 替换 HRESULT**：

- 类型安全：拿不到值就拿不到，无忘记检查。
- 零开销：`expected` 在小 T 下与裸返回值同 ABI。
- 携带源位置 + 错误链——CAA HRESULT 全无此能力。

### 4.4.3 接口方法签名

```cpp
[[=Interface]]
struct IDataLength {
    virtual Result<int>  length() const = 0;
    virtual Result<void> set_length(int) = 0;
};
```

`verify_interface_invariants` 校验每个方法返回类型必须是 `Result<T>` 或 `void`（极少数例外：`virtual void retain()` / `release()` 等所有权操作）。

### 4.4.4 边界转换

COM 互操作 shim 在 thunk 内做单点转换：

```cpp
template <typename T>
HRESULT to_hresult(Result<T> r, T* out) noexcept {
    if (!r) return error_to_hresult(r.error());
    if constexpr (!std::is_void_v<T>) *out = std::move(*r);
    return S_OK;
}
```

`error_to_hresult` 根据 `Error::domain + code` 映射到 HRESULT。映射表是 consteval 表，`ErrorDomain` 枚举与 HRESULT 段一一对应。

### 4.4.5 异常 vs `expected`

新设计**不在跨 ABI 边界使用异常**——异常的 unwinding 表跨 .so 不可靠（与 CAA 同样的工程现实）。**库内部模块边界**可用异常，**接口方法**一律 `Result<T>`。这是与 CAA HRESULT 同等保守的工程选择，避免引入新风险。

---

## 4.5 头目录约定 → C++20 modules

CAA `PublicInterfaces / ProtectedInterfaces / PrivateInterfaces / LocalInterfaces` 是用文件系统位置表达可见性。新设计用 module 划分：

| CAA 目录 | C++20 module 等价 |
|---|---|
| `framework/PublicInterfaces/` | `export module Yuki.Foo.Public;` 中的 `export` 实体 |
| `framework/ProtectedInterfaces/` | `export module Yuki.Foo.Internal;` 仅 `Yuki.*` 兄弟模块 import |
| `framework/PrivateInterfaces/` | module partition `Yuki.Foo:detail` |
| `framework/<module>.m/LocalInterfaces/` | module 实现单元（不 export 的部分） |

mkmk 的"`tsrc` 加 `//public` 决定 TIE 头放 `PublicGenerated` 还是 `ProtectedGenerated`"消失——adapter 由反射在 import 端按需生成，不存在中间生成文件。

---

下一章 `05` 给出反射特性的统一映射表与性能账本。
