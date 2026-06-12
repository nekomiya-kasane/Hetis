# 07 — 实施路线

按依赖顺序切分为九层。每一层完成后下一层可独立开发与测试，无环依赖。这是把第 02–06 章的设计落到 Yuki 子项目的具体次序。

## 7.1 拓扑

```
Layer 1  基元
   ↓
Layer 2  反射工具
   ↓
Layer 3  注解
   ↓
Layer 4  类作用域 verifier
   ↓
Layer 5  派发
   ↓
Layer 6  adapter 生成器
   ↓
Layer 7  manifest / 插件
   ↓
Layer 8  COM 互操作
   ↓
Layer 9  绑定生成器
```

每层都可独立测试。CAA 的"main + extension + TIE + dico + IDispatch"那张错综大网在新设计里被分解成九层正交模块。

---

## 7.2 Layer 1 — 基元

**目标**：建立类型系统的最底层词汇。

**交付**：

- `InterfaceId` / `component_id_t`（强类型 64-bit hash 包装）。
- `Error` 结构体 + `Result<T> = std::expected<T, Error>` 别名。
- `intrusive_ref_counted<Self>` CRTP 基类。
- `intrusive_ptr<T>` 单泛型智能指针。
- `Object` 类型擦除根（含 `dispatch_vtable*` 与 `void* impl`）。

**测试**：

- `intrusive_ptr` 的 retain/release 计数正确。
- 移动语义不增减计数。
- 上转编译通过、跨接口转编译失败（无 `query<>` 调用时）。
- `Result<T>` 与 `void` 返回值的反射可读。

**依赖**：仅 C++26 标准库。

---

## 7.3 Layer 2 — 反射工具

**目标**：把 P2996 反射封装成本设计专用的查询函数。

**交付**：

- `qualified_name_of<T>()` / `display_string_of(^^T)` 包装。
- `stable_hash(...)` consteval 哈希函数（基于 SHA-256 截断到 64-bit）。
- `abi_signature_of<I>()` 计算接口的 ABI 摘要。
- `methods_of(^^I)` / `bases_of(^^I)` / `nonstatic_data_members_of(^^T)` 包装与过滤器。
- `implemented_interfaces_of(^^Cmp)` / `om_base_of(^^I)` / `collect_extensions(^^Cmp)`。

**测试**：

- 给定接口与实现，反射返回的方法序列与手写一致。
- `stable_hash` 跨编译稳定（同输入同输出）。
- `abi_signature_of` 检测出方法签名变化。

**依赖**：Layer 1。

---

## 7.4 Layer 3 — 注解

**目标**：定义所有注解类型，作为元数据载体。

**交付**：

- `Component { kind, om_base, extends, extends_many }`。
- `Interface { stability, automation_exposed, automation_journalizable }`。
- `Iid { string_view }`。
- `Exposed`、`ComExposed { iid, progid }`。
- 配套 concept：`ComponentMain<T>` / `ComponentExtension<T>` / `InterfaceType<T>`。

**测试**：

- 用注解标注的类型可被反射读出。
- concept 在错误标注时编译失败，错误信息可读。
- 注解携带的 `std::meta::info` 字段可被 splice 还原成类型。

**依赖**：Layer 2。

---

## 7.5 Layer 4 — 类作用域 verifier

**目标**：把 cpp_rules 文档里的纪律提升为编译错误。

**交付**：

- `verify_interface_invariants(^^I)`：检查方法 pure virtual、返回 `Result<T>` 或 `void`、无 NSDM、单继承、稳定性标记一致。
- `verify_component(^^Cmp)`：检查 `om_base` 仅 Main 有效、`extends` 仅 Extension 有效、CodeExtension 无 NSDM、Determinism 三规则。
- `verify_no_iid_collision(span<InterfaceDescriptor>)`：manifest 构建期校验 IID 哈希无碰撞。
- 可读的 `report_error(...)` 工具（基于 `static_assert` + `std::source_location`）。

**测试**：

- 故意写出违反每条规则的样例，确认编译失败且错误指向正确位置。
- 通过样例确认正常代码不误报。

**依赖**：Layer 3。

---

## 7.6 Layer 5 — 派发

**目标**：替代 metaobject chain；编译期完美哈希派发表。

**交付**：

- `PerfectHashIndex<K, V>` 类模板（CHD 算法的 consteval 实现）。
- `dispatch_table<Cmp>` 模板（编译期烤出的 phf 派发表）。
- `dispatch_vtable` 数据结构（运行期类型擦除入口）。
- `query_interface<I>(Cmp*)` 静态版本（`static_cast` / 数据成员访问 / adapter 三分支）。
- `query_interface<I>(Object*)` 类型擦除版本（phf 查表）。

**测试**：

- 静态版本对所有三分支生成正确代码（用 LLVM IR 检查无运行期间接）。
- 类型擦除版本 phf 命中率 100%、错误 IID 返回 nullptr。
- 性能基准：与手写直接调用对比，开销在期望范围内（≤ 5 ns / call）。

**依赖**：Layer 2、3、4。

---

## 7.7 Layer 6 — adapter 生成器

**目标**：消解 TIE/BOA 二分；编译期为 `(Iface, Impl)` 对生成转发 adapter。

**交付**：

- `adapter<Iface, Impl>` 类模板（用 `define_aggregate` 合成 vtable）。
- `build_thunk_table<Iface, Impl>()` consteval 函数（用 `template for` 展开方法）。
- `make_thunk_for<method, Impl>()` consteval 工厂（生成单方法 thunk）。
- `meta::substitute` 包装，应对 P3687 splice 限制。

**测试**：

- adapter 对 0 / 1 / N 方法接口都能正确生成。
- LTO 后 adapter 调用被 inline 到客户代码。
- 对继承自接口的 impl，dispatch_table 选 `DirectCast` 而不生成 adapter。

**依赖**：Layer 5。

---

## 7.8 Layer 7 — manifest / 插件

**目标**：跨 TU 元数据聚合 + 运行期插件加载。

**交付**：

- `InterfaceDescriptor` / `ComponentDescriptor` / `ExtensionDescriptor` / `PluginManifest` 数据结构（第 04 章 4.1.3 定义）。
- `[[gnu::section("yuki_components")]]` 跨平台抽象（Linux/macOS/MSVC/MinGW 四份）。
- `__start_/__stop_` 符号读取的 `all_components()` / `all_interfaces()`。
- `registry` 类（运行期全局查询 + RCU 插件加载）。
- `load_plugin(path)` 实现（dlopen + dlsym + abi_freeze 校验 + RCU fold-in）。

**测试**：

- 单 TU 的描述符正确进入 section。
- 多 TU 链接后所有描述符可枚举。
- 跨平台 section 抽象在四种工具链上工作。
- 插件加载/卸载的并发安全（RCU 读端无锁）。
- abi_freeze 不匹配时插件加载失败且错误信息明确。

**依赖**：Layer 5、6。

---

## 7.9 Layer 8 — COM 互操作

**目标**：与 CAA / 老 COM 客户的边界。

**交付**：

- `from_com<I>(IUnknown*)` 包装。
- `com_ref_counted<I>` 让 `intrusive_ptr<I>` 走 COM AddRef/Release。
- `dispatch_shim<T>` 模板（IDispatch 四方法的反射生成实现）。
- `to_com<T>(intrusive_ptr<T>)` 工厂。
- 类型 marshaling：`Result<T> ↔ HRESULT`、`string_view ↔ BSTR`、`span ↔ SAFEARRAY`、`optional ↔ VARIANT`、`variant ↔ VARIANT` 等。
- DISPID phf 表生成。

**测试**：

- 从 VBA / VBScript 调用新组件，方法/属性按名工作。
- HRESULT 错误码与 Result<T> 错误链双向无损映射。
- 引用计数跨边界正确传递（无泄漏、无双 release）。
- 与一个真实 CAA framework 互操作的小型集成测试。

**依赖**：Layer 7。

---

## 7.10 Layer 9 — 绑定生成器

**目标**：跨语言 ahead-of-time 绑定。

**交付**：

- `generate_lua_binding(^^I) -> string` consteval 函数（输出 Lua 绑定 C++ 桩）。
- `generate_python_binding(^^I) -> string`（输出 pybind11/nanobind 风格）。
- `generate_csharp_binding(^^I) -> string`（输出 NativeAOT C# 端 partial class）。
- 每个绑定后端独立 module，按需启用。

**测试**：

- 各语言端能调用新组件方法、读写属性。
- 性能基准：与手写 binding 对比开销可忽略。
- 错误传播：Result<T> 在脚本端表现为各语言原生异常或 Result 类型。

**依赖**：Layer 7（不依赖 Layer 8——绑定与 COM 正交）。

---

## 7.11 里程碑

| 里程碑 | 内容 |
|---|---|
| M1 | Layer 1–4 完成；可声明接口与组件，编译期校验全部规则 |
| M2 | Layer 5–6 完成；静态 + 类型擦除派发可用，性能达标 |
| M3 | Layer 7 完成；插件加载/卸载工作；与 CAA 完全无关的纯反射体系跑通 |
| M4 | Layer 8 完成；可从 CAA / VBA 双向互操作 |
| M5 | Layer 9 至少一种语言完成（建议 Python 优先，验证反射→脚本路径） |
| M6 | 第一个真实 framework 用新设计重写完毕，端到端运行 |

每个里程碑独立交付价值。M1–M3 可在不接触任何 CAA 代码的情况下完成（纯 Yuki 内部）。M4 起开始接触 CAA。

---

## 7.12 测试策略

每层都有三类测试：

1. **编译期单元测试**：用 `static_assert` 在 consteval 函数里直接断言。失败 = 编译失败。
2. **运行期单元测试**：标准框架（GoogleTest / Catch2），覆盖 phf 查表、引用计数、插件加载。
3. **集成测试**：跨 module、跨插件、跨 COM 边界的端到端场景。

**性能基准**：每层完成后跑微基准（QI 调用、方法调用、引用计数操作），与 CAA V5 同等场景对比。性能回归直接 fail PR。

**ABI 兼容性测试**：CI 保留前一版的 abi_freeze_snapshot；任何接口签名变化触发显式确认（`[[deprecated]]` + 新接口，而非修改）。

---

## 7.13 与仓库现有代码的关系

仓库根目录已有 Mashiro / Nova 等子项目。Yuki 与它们的关系：

- **不依赖**：Mashiro / Nova 的代码不进 Yuki 的依赖图。
- **不打扰**：Yuki 的设计与实现独立演进，不要求其他子项目跟进。
- **可借鉴**：Mashiro 已有的 P2996 反射使用模式（参考 AGENTS.md 第 1 节）应在 Yuki 实现层复用，避免重复发明 `define_static_array` 等基础工具。

CMake 集成：

```
Yuki/
├── docs/design/         (本目录，已就位)
├── include/yuki/        (公开头)
├── src/                 (实现)
├── tests/               (单元 + 集成)
└── CMakeLists.txt
```

待第一行实现代码落地时再建立。本目录的设计文档先行。

---

## 7.14 失败信号

以下任一条出现，停下来重审设计：

1. **某层无法独立测试**：说明依赖切分错误，回到拓扑图重新切。
2. **运行期热路径出现字符串比较**：违反"运行期只见 64-bit 比较"原则，必须移到编译期。
3. **任何一处需要静态构造副作用**：违反"不依赖 SIOF"原则，改用 inline constinit + section。
4. **接口签名变化未触发构建失败**：abi_freeze 校验失效，必须修复才能继续。
5. **adapter 生成的代码无法被 LTO inline**：派发性能不达标，回到 Layer 6 优化 thunk 形态。
6. **从 CAA 迁移一个 framework 需要 mkmk 风格的外部生成器**：违反"零外部工具链"原则，反思设计。

每个信号对应一类**架构正确性**问题——出现就修复，不放过。

---

## 7.15 结语

CAA V5 在 2000 年解决了一个真实工程问题：让 CATIA 在 UNIX/Windows 上有一套统一的组件模型，让客户与供应商单边演进，让插件生态可以跨发布版本兼容。它的所有形态——TIE 中介对象、`.dico` 文本表、metaobject chain、`_var` 一接口一类、IDispatch 派发表——都是在那个时代约束下的合理工程取舍。

26 年后，C++26 反射 + consteval 块 + 注解 + concept 让我们能**保留 CAA 全部架构属性，丢掉 CAA 全部技术债**。这不是渐进改良——是一次基础设施级别的重写。Yuki 子项目的目标是给出这个重写的参照实现，并在真实 framework 上验证可行性。

设计文档到此为止。第一层实现代码落地时再回来更新。
