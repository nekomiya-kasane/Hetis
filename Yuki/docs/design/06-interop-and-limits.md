# 06 — 互操作与 C++26 短板

## 6.1 与 CAA / COM 的单向互操作

不要重写就开战。新设计与 CAA / COM 之间保留**单向桥**：客户在新代码里完全不接触 HRESULT / GUID / `_var`，遗留代码原样存在直到自然替换。

### 6.1.1 从 COM 拿对象进来

```cpp
template <typename I>
intrusive_ptr<I> from_com(IUnknown* raw, com_iid_tag_t<...> = {});
```

需求：

- `I` 必须有 `[[=ComInterop{.iid = "DCE:…"}]]` 注解，否则编译错。
- `from_com` 内部用 `raw->QueryInterface(GUID_from_iid_string(...), …)` 拿到 COM 接口指针。
- 包装为 `intrusive_ptr<I>`，`retain` 走 COM `AddRef`，`release` 走 COM `Release`。

```cpp
template <typename I>
class com_ref_counted {
    IUnknown* _raw;
public:
    void retain()  noexcept { _raw->AddRef(); }
    void release() noexcept { _raw->Release(); }
};
```

`intrusive_ptr<I>` 在这条边界上**透明工作**——客户不知道底下是 COM，但所有规则一致。

### 6.1.2 把对象暴露给 COM

```cpp
template <typename T>
ComPtr<IDispatch> to_com(intrusive_ptr<T> obj);
```

仅当 `T` 实现的接口里有至少一个 `[[=ComExposed]]` 注解时编译通过。`to_com` 返回的 `ComPtr<IDispatch>` 是 consteval 生成的 `dispatch_shim<T>` 实例（详见第 04 章 4.3.3），实现 COM `IDispatch` 四方法，把调用转发到 `T` 的反射方法表。

### 6.1.3 跨边界类型映射

| 新设计内 | COM 边界 |
|---|---|
| `Result<T>` (`std::expected<T, Error>`) | `HRESULT` + out param |
| `intrusive_ptr<I>` | `IUnknown*` 引用计数 |
| `std::string_view` | `BSTR` |
| `std::span<T>` | `SAFEARRAY` |
| `std::optional<T>` | `VARIANT` 的 VT_EMPTY |
| `std::variant<…>` | `VARIANT` 联合类型 |
| `std::filesystem::path` | `BSTR`（UTF-16 转 path） |

每个映射在编译期由反射 + concept 驱动生成 marshaling thunk。客户写普通 C++，COM 边界处的 marshaling 完全自动。

### 6.1.4 桥接代码的来源

桥接代码本身**也由反射生成**。CAA 接口的 IDL 文件是同语言 reflection 的子集——解析 `.idl` 不在新设计范围（如果项目需要消费已有 `.idl`，建议用一个一次性的 IDL→C++ 翻译工具产出 `[[=Interface, =Iid]]` 注解的现代接口声明，之后所有维护工作回到反射体系）。

### 6.1.5 与 ATL / WinRT 的边界

不主动整合 ATL（反例，宏 + IDL 派发表）。但允许在边界上互通：

- ATL `CComPtr<I>` 与 `intrusive_ptr<I>` 通过 `from_com` / `to_com` 互转。
- WinRT `winrt::com_ptr<T>` 与 `intrusive_ptr<T>` 通过 IUnknown 间接互转。

新设计内部不使用这些类型，只在外层接口上接受/返回。

---

## 6.2 与遗留 CAA 项目共存

如果 Yuki 子项目需要在 CATIA 进程内运行（例如作为新一代插件），需要双向兼容：

### 6.2.1 让 CAA 调新组件

CAA 通过 `CATInstantiateComponent("YukiCircle", IID_CATIPoint, …)` 想调用新设计的 `CircleImpl`：

1. 在 CAA 端写一个**薄 shim 组件** `YukiCircle`，实现 `CATICreateInstance`。
2. shim 的 `CreateInstance` 内部调 `make<CircleImpl>()`，包装为 `Object*`。
3. 用 `to_com<CircleImpl>` 暴露 IDispatch；CAA 的 `CATIPoint` 通过 ComExposed 注解的 IID 映射查到。
4. CAA 客户感知到的就是一个普通 CAA 组件——**全新内部，CAA 完全不知**。

### 6.2.2 让新组件调 CAA

新设计的代码想用 CAA `CATIA Sketcher` 的某个接口：

1. 给 CAA 接口写一份 `[[=Interface, =ComInterop{.iid = "…"}]]` 声明（一次性，可由工具批量生成）。
2. 用 `from_com<ICATIASketcher>(rawIUnknown)` 拿到 `intrusive_ptr<ICATIASketcher>`。
3. 调用方法走 `Result<T>` 返回值，错误链自动捕获 HRESULT。

### 6.2.3 渐进迁移路径

从一个 CAA framework 开始：

1. 把该 framework 的所有接口翻译成 `[[=Interface, =ComInterop]]` 声明（一次性）。
2. 把 main impl + Extension 用新注解重写——内部代码不变，外层包装变了。
3. 通过 IDispatch shim 同时暴露给 CAA 旧客户。
4. 新客户直接用反射版接口。
5. 当所有客户都迁移完毕，移除 `[[=ComInterop]]` 注解，IDispatch shim 自动消失。

**没有大爆炸式重写**——每个 framework 自由选择迁移节奏。

---

## 6.3 C++26 必须诚实承认的短板

不要假装 C++26 能解决所有问题。以下三处是真实工程边界，不是设计妥协。

### 6.3.1 P3687 删除了 splice template argument

**问题**：无法直接 `Adapter<[:^^I:], Impl>` 把反射拼为模板实参。需要此能力时只能用模板元函数间接转发。

**绕道**：

```cpp
template <std::meta::info IfaceInfo, typename Impl>
using adapter_t = [: substitute(^^adapter, {IfaceInfo, ^^Impl}) :];
```

`std::meta::substitute` 把模板与实参列表组合得到 `std::meta::info`，再 `[:…:]` 取类型。可行；写法不如 C++29 后的版本干净——多一层间接、错误信息略差。

**影响范围**：adapter 生成、dispatch_vtable 模板化场景。其他地方不受影响。

**预期**：C++29 标准可能放开此限制；届时改回直接 splice，零代码外行为变化。

### 6.3.2 跨 TU 的 consteval 累积

**问题**：标准本身没有"全局 consteval 数组"机制；要让所有 TU 的 `ComponentDescriptor` 拼成单一 manifest，只能依赖 link-time 机制：

- gcc/clang：`__attribute__((section, used))` + `__start_/__stop_` 符号。
- MSVC：`#pragma section + __declspec(allocate)` + 头/尾哨兵符号。
- C++26 模块的 init 顺序：标准定义但工具链支持仍在演进。

**影响范围**：manifest 收集机制需要平台抽象层（一份 Linux/macOS、一份 Windows MSVC、一份 MinGW）。三平台实测可工作，但**不是单一 portable 写法**。

**对设计的影响**：零——都是工程实现细节，不影响接口设计。

**长期演进**：如未来 C++ 标准化"link-time consteval 累积"机制（已有提案），可一次性切换到标准方式。

### 6.3.3 编译期烤出的 manifest 进入二进制

**问题**：consteval 生成的 manifest 与 phf 表会进入二进制 `.rodata`。对极致 binary size 敏感的目标（嵌入式 / WASM）需要权衡。

**量级**：
- 典型组件（10 接口）：约 1 KB / 组件。
- 1000 个组件：约 1 MB。
- CAA `.dico` 同等组件数约几百 KB 文本（但需要运行期解析、需要静态构造器）。

**净效益判断**：
- Desktop 应用：1 MB 可忽略，换得运行期零字符串、零静态构造、零字典查找。
- 嵌入式：按 framework 切片裁剪，未启用 framework 不进二进制。
- WASM：同嵌入式策略；额外考虑 phf 表压缩（CHD 已经接近最优）。

**对设计的影响**：零；只影响部署策略。

### 6.3.4 编译器 conformance

**问题**：P2996/P3289/P3385 在 2026 年成熟实现是 COCA clang-p2996（Bloomberg fork）/ Clang 21+ trunk。生产部署需要锁定编译器版本。

**对策**：

- 主开发链路用 COCA clang-p2996（与仓库根 AGENTS.md 锁定一致）。
- CI 跑双链路：clang-p2996 + 主线 Clang 最新版（捕捉上游变更）。
- 构建脚本检测 `__cpp_reflection`、`__cpp_consteval_blocks` 等特性宏，缺失则在最早期 fail。
- 文档明确：**不支持 GCC、MSVC 直到它们实现 P2996**。

**与 CAA 锁定 mkmk 同性质**——CAA 把工具链锁在 DS 内部 fork 上多年。新设计同样需要锁定，但锁定的是**上游开源 fork**，可观测性远高于 mkmk。

### 6.3.5 关于反射元信息的二进制扩展性

**问题**：当组件数量极大（>10⁴）时，单一 manifest 段会变大；运行期 fold-in 耗时增长。

**对策**：

- manifest 分段：按 framework 划分多个独立 section，加载时按需 fold。
- phf 分级：每 framework 一份本地 phf；跨 framework 查询走两级 phf。
- 增量加载：插件加载时只 fold 该插件的描述符，不重建全局 phf；用 RCU 风格无锁更新。

**对设计的影响**：未来可演进，当前规模下不必预先实现。

---

## 6.4 不引入的复杂度

明确**不做**的事：

1. **运行期反射 API**：新设计不提供 `RuntimeType::method_by_name(...)` 之类 API。所有反射在编译期完成；运行期只见 phf + 函数指针。理由：性能、二进制大小、防止把 CAA `IsAKindOf` 字符串比较的债务复刻到新系统。
2. **接口版本号**：CAA 靠"接口冻结"承诺；新设计同。不引入"v1/v2 同名接口"——要演进就开新接口，配合 `[[deprecated]]` 标注旧接口。
3. **跨进程对象**：CAA `CATInstantiateComponent` 暗示分布式能力但实际很少用。新设计不内置；需要时由上层 RPC（gRPC / Cap'n Proto / 自定义）封装，与对象系统正交。
4. **持久化 / 序列化**：与对象系统正交，由独立模块处理。可复用反射枚举字段，但不在本设计范围。
5. **GC**：引用计数即所有权管理；不引入跟踪式 GC。CAA 也只用引用计数。

---

## 6.5 总结

| 维度 | 状态 |
|---|---|
| 与 CAA 互操作 | 单向桥（`from_com` / `to_com`），渐进迁移可行 |
| 与 COM 互操作 | 边界 thunk + IDispatch shim，仅在 `[[=ComExposed]]` 接口生效 |
| 与 ATL / WinRT | 不主动整合，但通过 IUnknown 互通 |
| P3687 splice 限制 | 已知绕道（`meta::substitute`），C++29 修复 |
| 跨 TU consteval | 平台抽象层（三份实现），不影响设计 |
| 二进制大小 | 增量约几百 KB ~ 几 MB，按 framework 可裁剪 |
| 编译器 conformance | 锁定 COCA clang-p2996，CI 跑双链路 |
| 不做 | 运行期反射 API、接口版本号、跨进程对象、持久化、GC |

下一章 `07` 给出按依赖顺序的九层实施路线。
