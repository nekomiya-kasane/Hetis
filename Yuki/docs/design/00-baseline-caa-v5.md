# 00 — CAA V5 基线：必须保留的语义与必须丢弃的形态

本章对 CATIA V5 / CAA V5 的元对象与 COM 体系做一份"考古级"梳理，作为后续重写的契约。本设计**原封保留**这里描述的所有架构属性（客户与实现脱耦、组件可独立演进、运行期可装载新扩展、跨语言可暴露），但**不保留任何**形态层细节（宏、字典文件、TIE 中介对象、链表式派发、`_var` 一接口一类、HRESULT、`__stdcall`、IDispatch DISPID 表）。

> 数据来源：Dassault Systèmes 官方 CAA Encyclopedia 在线镜像（maruf.ca、q-solid.com、plmcoach.com）。涉及非公开实现细节（TIE 宏体、`CATImplementClass` 静态注册器内部结构、`CATBaseDispatch::Invoke` 派发表）的部分，本章只描述行为级事实，不做内部结构假设。

---

## 0.1 顶层定位

CAA V5 是 CATIA 内部的**对象建模 (Object Modeler, OM) 中间件**，把 Microsoft COM 抽象在 UNIX/Windows 上等价化，并扩展了 COM 自身没有的概念：

- 组件 OM 继承（component-level inheritance）
- Extension（DataExtension / CodeExtension）
- TIE / BOA 双桥接技术
- 字典文件 (`.dico`) 驱动的运行期装配
- Automation 公开层（`IDispatch` → `CATBaseDispatch` → `CATIABase` + 类型库）

CAA 的"元对象"语义同时落在三个维度：

| 维度 | 元数据载体 | 作用 |
|---|---|---|
| 接口标识 | `IID_<interfaceName>`（GUID） | `QueryInterface` 派发 |
| 类身份 | `CATImplementClass` 宏 + `.dico` | 决定一个 C++ 类是 main / Extension / BOA，扩展谁、OM-derive 自谁 |
| 字典 | `<framework>.dico` 文本表 | 把"组件名 + 接口名"映射到要 `dlopen` 的 `.so/.dll` |

---

## 0.2 元对象根类 `CATBaseUnknown`

```
IUnknown                           // COM 自带；UNIX 由 CAA 自己提供同名等价物
   │
   ├── CATBaseUnknown              // 所有 CAA 接口与组件类的 C++ 基类
   │
   └── IDispatch                   // COM 标准 / CAA 在 UNIX 等价
           │
           └── CATBaseDispatch     // CAA 对 IDispatch 的默认实现
                   │
                   └── CATIABase   // 所有 Automation 公开接口的根
```

`IUnknown` 在 CAA 下与 COM 二进制等价：

```cpp
interface IUnknown {
    virtual HRESULT __stdcall QueryInterface(const IID& iid, void** ppv) = 0;
    virtual ULONG   __stdcall AddRef()  = 0;
    virtual ULONG   __stdcall Release() = 0;
};
```

`CATBaseUnknown` 在此之上：

- 给 `QueryInterface / AddRef / Release` 默认实现，**所有 CAA 类继承之，不得重写**。
- 提供"universal handle"语义：对同一组件实例反复 `QueryInterface(IID_IUnknown, …)` 永远返回**同一个指针**——CAA 唯一安全的实例同一性比较手段。
- 暴露元类反射 API（这是 CAA 区别于纯 COM 的关键差异点）：

| 方法 | 签名 | 语义 |
|---|---|---|
| `static ClassName()` | `char*` | 当前对象所属类名（编译期由 `CATImplementClass` 烙印进去） |
| `virtual IsAKindOf(const char*)` | `0/1` | 沿 OM 继承链向上查："我属不属于这一族？" |
| `virtual IsEqual(CATBaseUnknown*)` | `0/1` | 比较两个接口指针**背后的 impl 是否同一实例** |

`CATImplementClass` 不只是注册"我是谁、扩展谁、OM-derive 自谁"——它同时把这些元数据下沉到运行期对象，使任意 `CATBaseUnknown*` 都能自报家门。

---

## 0.3 接口（`CATI*`）

**纯抽象 C++ 类**，每个接口三件套：

| 文件 | 内容 |
|---|---|
| `CAAIXX.h` | `class CAAIXX : public CATBaseUnknown { CATDeclareInterface; … }`，全部方法 `pure virtual / __stdcall / HRESULT`，并 `extern IID IID_CAAIXX;` |
| `CAAIXX.cpp` | 给 `IID_CAAIXX` 赋 GUID 字面量，调用 `CATImplementInterface(CAAIXX, CATBaseUnknown);` 把接口纳入 OM |
| `TIE_CAAIXX.tsrc` | 由 mkmk 扫描生成 `TIE_CAAIXX.h`，里面是 `TIE_CAAIXX(impl)` 宏的展开模板 |

强制契约：

1. 方法**必须** `virtual HRESULT __stdcall foo(...) = 0`。`HRESULT + __stdcall` 是为了 OLE 兼容、可被 IDL 投影到 Automation。
2. **接口禁多继承**。OM-derive 必须与 C++-derive 同步：C++ 上 `class A : public B`、宏上 `CATImplementInterface(A, B)`。
3. **接口一旦发布永不修改**。只允许通过新增接口或新增 Extension 扩展。这是 CAA 二进制兼容承诺的核心。
4. `IID_*` 是 `extern` 全局变量，由 GUID 字面量初始化。

---

## 0.4 组件 — 客户视角 vs 供应商视角

CAA 显式区分两套视角：

- **客户应用程序员的逻辑视角**：组件是一个对象，对外只暴露若干接口。
- **组件供应商的物理视角**：一个组件由 1 个 main implementation class + N 个 Extension class 组成。

```
                     ┌─────────────────────────────────────┐
                     │            Component                │
                     │ ┌─────────────┐                     │
   Client ── CATIXX ─┤ │ MainImpl    │ implements CATIXX   │
   Client ── CATIYY ─┤ │ ExtensionA  │ implements CATIYY   │
   Client ── CATIZZ ─┤ │ ExtensionB  │ implements CATIZZ   │
                     │ └─────────────┘                     │
                     └─────────────────────────────────────┘
```

**Main class** 必须：

- C++-derive **且** OM-derive 自 `CATBaseUnknown` 或另一个组件 main class（两条链同步推进）。
- 头文件加 `CATDeclareClass`。
- 源文件加 `CATImplementClass(Cmp, Implementation, Base, CATNull)`。

实例化两条路：

| 方法 | 客户与组件耦合 |
|---|---|
| `::CATInstantiateComponent("CmpName", IID_…, ppv)` | **零构建期耦合**（推荐）。靠字典定位 `.dll/.so` |
| `new CmpClass()` 然后 cast 为 `IUnknown*` | 客户必须 `#include` 主类头并 link 到组件 lib |

实现 `CATICreateInstance`（用一个 `CodeExtension` 挂载）是让 `CATInstantiateComponent` 能创建你组件的前提。

---

## 0.5 扩展 — CAA 区别于 COM 的核心增量

> COM 没有"扩展"概念。CAA 的 Extension 是为了**让组件供应商在不破坏二进制兼容、不要求客户重编的前提下，向已发布组件追加新接口实现**。

Extension class 必须：

- C++-derive 自 `CATBaseUnknown`（TIE 实现）或自接口本身（BOA 实现）。
- OM-derive 标记为 `CATBaseUnknown` 或 `CATNull`（TIE）/ 接口名（BOA）。
- 头文件 `CATDeclareClass`，源文件 `CATImplementClass(Ext, <kind>, <OMbase>, ExtendedComponent)`。

**两种 Extension**：

- **DataExtension**：带数据成员。生命周期：**首次** `QueryInterface` 命中其接口时由 OM 创建，依附于宿主组件实例；当宿主和扩展所有引用计数都归零时一并销毁。
- **CodeExtension**：只有方法，无数据成员。**全进程唯一一个实例**，被同型组件的所有实例共享，永不销毁。

**共享扩展**两种声明方向：

- **扩展端声明**（`CATBeginImplementClass + CATAddClassExtension + CATEndImplementClass`）：让现有 Extension 同时成为多个 main class 的扩展，不重编 main class。
- **被扩展端声明**（`CATSupportImplementation(Ext, Cmp, Itf)`）：让现有 main class 加挂第三方 Extension，不重编 Extension。

这是 CAA 的"开放式架构"承诺——客户和供应商都能**单边演进**。

---

## 0.6 TIE vs BOA

### TIE（默认）

`TIE_CATIXX(impl_class)` 宏在编译期生成一个**中介类**，实现接口的所有 vtbl 方法，方法体把调用转发给 `impl_class`。

- `QueryInterface(IID_CATIXX, …)` 返回**TIE 实例的指针**，不是 impl 实例的指针。
- 优点：`impl_class` 不需要 C++-derive 自接口，**一个 impl 可以实现任意多个接口**（绕开 C++ 单继承）。
- 代价：每次 QI 都创建一个 TIE 中介对象——组件实例化数千次时累计内存压力可观。

### BOA — Basic Object Adapter

让扩展类**直接 C++-derive 自接口**，省掉中介对象。

```cpp
class MyDataExtension : public CATIData { CATDeclareClass; … };
CATImplementClass(MyDataExtension, DataExtension, CATIData, MyObject);
CATImplementBOA(CATIData, MyDataExtension);
```

- `QueryInterface` 返回的指针就是扩展实例本身。
- 由于 CAA 禁多继承，**一个类至多 BOA-实现一个接口**，其他必须走 TIE。
- BOA 不允许用于 `CodeExtension`。

---

## 0.7 双重继承体系：C++ vs OM

### 接口的 OM-继承

- 必须 C++ derive **且** `CATImplementInterface(Derived, Base)` 同步。
- 效果：组件即使没声明实现 `Base`，只要它实现了 `Derived`，`QueryInterface(IID_Base)` 也成功——但组件**必须自己提供 `Base` 方法的实现体**。

### 组件的 OM-继承

- 仅在 main implementation class 间存在。
- 必须 C++ derive **且** `CATImplementClass(Derived, Implementation, BaseCmp, CATNull)` 同步。
- 效果：派生组件**自动暴露基组件实现的所有接口**——无须声明 TIE、无须更新 dico、无须重写方法。
- 这是 COM 中没有的：COM 只有接口继承 + aggregation，没有"组件继承"。

### Extension 不参与 OM-继承

Extension 的 OM-base 参数对 TIE-DataExtension / CodeExtension 永远填 `CATBaseUnknown` 或 `CATNull`——只有 BOA-DataExtension 例外。

---

## 0.8 字典 (`.dico`) — 运行期装配中枢

每个 framework 对应一个 `<FrameworkName>.dico`，部署到 `CNext\code\dictionary\`。每行三列：

```
<ComponentMainClassName>   <InterfaceName>   <SharedLibName>
```

例：

```
CAASysComponent       CATICreateInstance    libCAASysComponentImpl
CAASysComponentBOA    CAAISysInterface      libCAASysComponentBOA
```

运行期 `QueryInterface` 流程（简化）：

1. 在已加载的 main / Extension 表中查"组件 → 接口"是否就绪。
2. 否则查 `.dico`，定位 lib，`dlopen / LoadLibrary`。
3. lib 入口里 `CATImplementClass` 的全局对象在装载时把"类身份+扩展关系"注册到 OM 的运行时类簿。
4. 依据 TIE / BOA 路径返回相应指针。

`.dico` 中的"组件名"**永远是 main class 名**，绝不写 Extension 类名。

`.dico` **不仅仅是装载映射，更是确定性派发的元数据来源**：当 TIE 代码与接口实现代码在不同 `.so/.dll` 时，dico 必须登记**装 TIE 的 lib**，否则 QI 在运行期可能跨组件错路由。

---

## 0.9 Determinism Principle

cpp_rules 文档把 CAA 的设计哲学命名为"Determinism Principle"，由它推三条强约束：

1. **同一组件内禁止重复实现同一接口**（不同 Extension 各实现同一 IB ⇒ QI 行为依赖 dico 行序与 dlopen 顺序，不可预测）。
2. **同一组件内禁止两个 OM-derive 自同一基接口的接口同时被实现**（QI 求基接口时无法选择走哪条扩展）。
3. **TIE 代码与接口实现代码若在不同 lib，dico 必须登记装 TIE 的 lib**。

这三条在 CAA 里靠 `mkCheckSource` + 代码评审拦截，本设计要把它们提升为编译错误。

文档还给出反直觉建议：**OM-derive 仅在你确实希望 QI 沿继承链解析时才用**；否则用纯 C++ 抽象基类做签名共享更稳——不写 `CATDeclareInterface`、不分 IID、不写 `.cpp`。

---

## 0.10 metaobject chain

cpp_rules 原话："如果把 Extension 的 OM-base 设成除 `CATBaseUnknown` 以外的类，会**在 metaobject chain 中引入一个不必要的额外节点**，只会降低性能。"

含义：CAA 在**运行期**为每个对象维护一条 **metaobject chain**，节点是各级"OM 基类"。`QueryInterface` 在这条链上沿 OM-derive 关系向上走，逐级问"你能不能给我 IID_X？"

- 链头：当前 impl/Extension 的元类节点。
- 链尾：`CATBaseUnknown`。
- 每多一个 OM-base 节点，QI 多一次表查找。

**TIE 的 chained vs unchained 也定义在这条链上**：unchained TIE 直接 vtbl 转发；chained TIE 进入 metaobject chain 解析路径。CodeExtension **不能用 chained TIE**，且 unchained TIE 在某些智能指针误用下会直接 core dump。

---

## 0.11 引用计数与 `_var` 智能指针

- `CATInstantiateComponent` / `new Cmp()` / `QueryInterface` **已经替你做了 `AddRef`**，不要再调一次。
- 任何复制 / 局部变量 / 输出参数 / 返回值 / 类成员存储接口指针时，必须显式 `AddRef`。
- 任何指针离开作用域、被覆盖、被销毁前，必须 `Release`。
- **绝对不要对接口指针 `delete`**。

每个接口 `CAAIXX` 同时有一个 `CATIXX_var` 智能指针类，重载 `=`、`!`、`==`、`!=`、`->`，构造时自动 `QueryInterface + AddRef`，析构 / 重新赋值时自动 `Release`，与 `NULL_var` 比较取代 `SUCCEEDED/FAILED`。

`_var` 与裸指针混用有一张五条避坑列表（详见 `using_components` 文档），错一处就泄漏或 core dump。这一节技术债是本设计要彻底解决的目标之一。

---

## 0.12 Automation 公开层

```
IDispatch                   // COM 标准 / CAA UNIX 等价
   └── CATBaseDispatch      // CAA 对 IDispatch 的默认实现
          └── CATIABase     // Automation 接口必须从此 derive
                 └── 你的 CATIA<Foo> 接口
```

派发机制：

- **vtbl binding**（C++ 客户）：编译期定位 vtbl 偏移，最快。
- **late binding**（脚本客户）：`IDispatch::GetIDsOfNames` 在类型库里把方法名查成 `DISPID`（long），`IDispatch::Invoke(DISPID, …)` 派发。

派发链路（基于 q-solid + catiawidgets 还原）：

1. 客户端拿到 `IDispatch*`（CAA 注册时给 `CATIA<Foo>` 一个 `AccessName` GUID 作为 ProgID 写进 Windows Registry）。
2. `IDispatch::GetTypeInfo(0, lcid)` → `ITypeInfo`。
3. `ITypeInfo::GetTypeAttr` → `TYPEATTR`：`.guid` 即接口 IID，`.cFuncs / .cVars` 是方法/属性数。
4. `GetFuncDesc(i) / GetVarDesc(i)` → `memid` 即 DISPID。
5. `IDispatch::Invoke(memid, …)` 经 `CATBaseDispatch::Invoke` 派发到 vtbl 槽 → TIE → impl。

q-solid 公开视图刻意屏蔽 `Invoke / GetIDsOfNames / GetTypeInfo*`——这些方法**不是给应用代码调的，是给 IDL 编译器生成的 stub 调的**。CAA 的 Automation 实质上是"IDL → 类型库 → IDispatch"的标准 OLE 链路，CAA 只补了 `CATBaseDispatch`（默认实现）和 `CATIABase`（对象通用元属性 `get_Application` / `get_Parent` / `get_Name` / `GetItem` / `ObjFromId`）。

类型库构建：

```idl
/*IDLREP*/
#pragma REPID    INFITF "DCE:14f197b2-0771-11d1-a5b100a0c9575177"
#pragma REPBEGIN INFITF
#pragma REPREQ   PreReqTypeLib
#include "CATIApplication.idl"
#include "CATIPageSetup.idl"
#include "CATIWindow.idl"
#pragma REPEND   INFITF
```

CAA IDL 编译器把这套源文件编译成运行期类型库（Win 下 `.dll`，UNIX 下 `.so`），脚本引擎从中读元数据走 late binding。

---

## 0.13 mkmk 目录约定（ABI 边界）

| 目录 | 可见性 | 用途 |
|---|---|---|
| `framework/PublicInterfaces/` | 跨 framework 公开 | 客户接口、Public TIE |
| `framework/ProtectedInterfaces/` | 仅同 brand 内部 framework | 保护接口 |
| `framework/PrivateInterfaces/` | 同 framework 内 module 间 | 框架私有 |
| `framework/<module>.m/LocalInterfaces/` | 单 module | impl/Extension 的私有头 |
| `framework/<module>.m/src/` | 不导出 | `.cpp / .c / .idl / .tsrc / .y / .l / .f / .sh` |

`tsrc` 文件里写 `//public` 决定 mkmk 把生成的 `TIE_<itf>.h` 放到 `PublicGenerated` 还是 `ProtectedGenerated`——这是 CAA 控制"客户能不能自己实现你接口"的开关。

---

## 0.14 必须保留的语义清单

下游各章设计的**契约**就是这张清单——任何一项被违反就视为方向性错误：

1. 客户与实现脱耦：客户只 include 接口头，零 link 到具体组件。
2. 单边演进：组件供应商或客户可独立追加接口/扩展，不要求对方重编。
3. 运行期可装载：插件包到位即生效，不重启。
4. 接口冻结：接口签名一旦发布永不修改。
5. Determinism：QI 路径在所有运行期上下文里同结果。
6. 实例同一可比：从同一组件的任意接口指针出发可比较"是不是同一个 impl"。
7. 跨语言可暴露：脚本/外语客户能按名调方法、按名读写属性。
8. 引用计数共享所有权：多客户共享对象，最后一个释放触发销毁。

---

## 0.15 必须丢弃的形态清单

下游各章设计的**自由度**就是这张清单——任何一项被原样保留就视为没真正重写：

1. 宏元编程（`CATDeclareInterface / CATImplementInterface / CATDeclareClass / CATImplementClass / TIE_*`）。
2. 静态构造器注册（导致 SIOF 风险，导致跨 lib 注册顺序敏感）。
3. `.dico` 文本表与字符串查找。
4. TIE 中介对象（运行期分配的桥接类）。
5. metaobject chain 链表式 QI walk。
6. `_var` 一接口一智能指针 + 五条避坑规则。
7. `HRESULT + __stdcall + SUCCEEDED/FAILED`（仅在 COM 互操作边界保留）。
8. IDispatch 运行期 `GetIDsOfNames + Invoke` 派发表（仅在 COM 互操作边界保留）。
9. `mkmk + .tsrc + .tplib` 外部代码生成器（被 consteval 替代）。
10. `PublicInterfaces / ProtectedInterfaces / PrivateInterfaces / LocalInterfaces` 头目录约定（被 C++20 modules 替代）。

下一章 `01-thesis.md` 给出整体立论与七大支柱总览。
