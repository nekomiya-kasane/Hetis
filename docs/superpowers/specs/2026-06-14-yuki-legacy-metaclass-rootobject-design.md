# Yuki Legacy — MetaClass 与 RootObject 设计

> 适用范围:Yuki 的 **Legacy**(基于继承树的过渡实现)。非 Legacy 的 inheritance-free
> 版本另行设计。本 spec 是对 CAA V5 `CATMetaClass` + `CATBaseUnknown` 的自顶向下重写,
> 保留全部架构属性(身份/单边演进/运行期可装载/族判定/共享所有权),清除全部技术债
> (宏、SIOF、字符串 `IsAKindOf`、metaobject chain、全员强制引用计数、`_var` 一接口一类)。

工具链:COCA clang-p2996(Clang 21, libc++),`-std=gnu++26 -freflection-latest`。行宽 ≤120。

## 1. 立论:类型(Type)与实例(Instance)是两种客观存在

CAA 把"类的元数据"与"实例的运行期身份"混在 `CATBaseUnknown` 一棵重型继承树里。本设计
把它们正交化为两个建模对象:

- **MetaClass** —— 柏拉图式的"型"。每个 OM 类一个,编译期由反射从注解烤制,进 `.rodata`。
- **RootObject** —— 具身的"物"的根。退化为纯多态锚点,只携带到达自身 MetaClass 的能力。

所有"获取 IID / ClassName / BaseMeta / 角色 / 族判定"的查询都**分两类入口**:静态(类型已知,
编译期折叠)与动态(类型擦除 `RootObject*`,经 vptr 到达)。两类共享**单一事实来源**
`MetaCore`,词根相同、语法形态不同,绝不双名同义。

## 2. 数据模型:MetaClass 三层

每个 OM 类的元对象按"可变性 + 并发模型"切成三层,物理分离、各自可独立测试:

```
MetaClass                       // 稳定外壳:把三层缝合为单一身份
├── const MetaCore&  _core      // ① 不变核   编译期烤进 .rodata,零运行期构造
├── MetaLinks*       _links     // ② 结构尾   加载期单线程 fold-in,运行期 RCU 只读
└── MetaDynamic*     _dynamic   // ③ 冷尾     懒分配,锁保护,仅诊断/脚本触碰
```

### 2.1 ① MetaCore — 不变核(`inline constexpr`,`.rodata`)

```cpp
struct MetaCore {
    ClassType                        type;          // 来自注解
    std::string_view                 qualifiedName; // display_string_of(^^T),静态串池
    Iid                              iid;           // IidOf<T>:注解 override 否则 StableHash
    std::span<const MetaCore* const> extends;       // Anno::Meta.extends 解析后的核引用
    std::span<const MetaCore* const> implements;    // Anno::Meta.implements 同理
    const MetaCore*                  omBase;        // C++ 直接基类的核;RootObject 时 nullptr
};
```

`extends`/`implements` 来自注解里的 `std::meta::info` span,编译期 splice 还原成类型后取各自
`&MetaCoreOf<那个类型>`,固化为指针 span(同进 `.rodata`)。运行期只见稳定指针,无字符串解析。

### 2.2 ② MetaLinks — 结构尾(加载期写,运行期只读)

后向参照与派发表。自型编译期无法知道"谁将来扩展我",故由 `load_plugin` 单线程 fold-in,
之后 RCU 只读。热路径(QI/方法调用)只读这层,不加锁。

```cpp
struct MetaLinks {
    std::span<const MetaCore* const> extendedBy;     // 哪些 Extension 扩展了我
    std::span<const MetaCore* const> implementedBy;  // 哪些类实现了我(我是接口时)
    std::span<const DispatchEntry>   dispatch;       // 完美哈希派发条目
};
```

### 2.3 ③ MetaDynamic — 冷尾(并发写,非热路径)

属性袋 + 运行期统计。仅诊断/脚本场景触碰,带锁,懒分配,绝不污染任何热路径。

```cpp
struct MetaDynamic {
    mutable std::shared_mutex                 mutex;
    std::flat_map<std::string_view, Property> properties;
    std::atomic<uint64_t>                     instanceCount;
};
```

## 3. 访问面:静态 / 动态两类,单一事实来源

| 概念 | 静态入口(类型,编译期折叠) | 动态入口(实例,1 vcall) | 共同来源 |
|---|---|---|---|
| 角色 | `ClassTypeOf<T>` | `mc.classType()` | `MetaCore::type` |
| 标识 | `IidOf<T>` | `mc.iid()` | `MetaCore::iid` |
| 名称 | `NameOf<T>` | `mc.name()` | `MetaCore::qualifiedName` |
| OM 基 | `BaseMetaOf<T>` | `mc.baseMeta()` | `MetaCore::omBase` |
| 族判定 | `std::derived_from<T,B>` | `mc.isAKindOf(b)` | C++ 继承 / omBase 链 |

命名约定:`XxxOf<T>`(读"此类型的")= 静态;裸方法 `xxx()`(读"此元对象的")= 动态。
两条路看到同一个 `MetaCore` 实例,这是"无同义反复 + 单一事实来源"的落地。

```cpp
template <typename T> inline constexpr ClassType       ClassTypeOf = MetaCoreOf<T>.type;
template <typename T> inline constexpr Iid             IidOf       = MetaCoreOf<T>.iid;
template <typename T> inline constexpr std::string_view NameOf     = MetaCoreOf<T>.qualifiedName;
template <typename T> inline constexpr const MetaCore* BaseMetaOf  = MetaCoreOf<T>.omBase;
```

## 4. 编译期流水线:注解 → MetaCore(全自动,零手写登记)

```cpp
template <typename T> inline constexpr MetaCore  MetaCoreOf  = Detail::BuildMetaCore<T>(); // 不变核
template <typename T> inline           MetaClass MetaClassOf{ MetaCoreOf<T> };             // 三层外壳
```

`BuildMetaCore<T>` 是 `consteval`,四步:① `ReadClassType(^^T)` 读角色;② 取 `Anno::Meta` 的
`extends`/`implements` info span;③ splice 还原为类型并取各自 `&MetaCoreOf<...>`,经
`std::define_static_array` 提升为静态 span;④ 算 `IidOf<T>`、`display_string_of` 取名、
`omBase` 取 C++ 直接基类的核。

"全自动"不依赖用户写 `consteval { register }`:RootObject 的到达机制(§5)在首次引用
`MetaClassOf<Self>` 时 ODR-use 一个 `[[gnu::section(...)]] inline constexpr` descriptor,
取地址即把它拉入链接器段。**constexpr 数据、无构造副作用、无 SIOF。**

错误处理(编译期拦截):`extends`/`implements` 指向非 OM 类型 → `BuildMetaCore` 内 `throw`
→ 编译错误,定位具体类型;角色注解冲突 → `ReadClassType` 已有 throw 路径;IID 碰撞 →
manifest 构建期 `static_assert`。

## 5. RootObject 与 MetaNode:纯锚点 + 零存储到达

### 5.1 RootObject — 退化为纯多态锚点(零数据成员)

```cpp
class RootObject {
public:
    virtual ~RootObject() = default;
    virtual const MetaClass& DynamicMetaClass() const noexcept = 0;  // 类型擦除入口
};
```

对比旧 `CATBaseUnknown`:**砍掉** `IDispatch` 基类、`QueryInterface`/`AddRef`/`Release`/
`GetMetaObject`/`IsA`/`IsAKindOf`/`IsEqual` 八个虚函数,**砍掉** per-instance 的
`m_cRef`/`NecessaryData`/`delegate`/`m_reserved`(每实例省 ~24B)。RootObject 只剩 1 个 vptr。

> 命名:Legacy 旧基类 `RootClass.h` 中的 `RootClass` 重命名/取代为 `RootObject`——"物"的根
> 是 Object,不是 Class。MetaClass 才是"类"。

### 5.2 MetaNode — CRTP 注入层(零手写 override + 零存储到达)

```cpp
template <typename Self, typename Base = RootObject>
struct MetaNode : Base {
    using Base::Base;
    const MetaClass& DynamicMetaClass() const noexcept override { return MetaClassOf<Self>; }
};

struct [[=Implementation]] CircleImpl : MetaNode<CircleImpl> { double radius; };
```

到达 MetaClass 复用已存在的 vptr,**per-instance 零额外字节**。`Base` 形参同时表达 C++ 基类
与 OM 继承链(`om_base`),最派生 override 胜出。两条到达路:

| 路径 | 形态 | per-instance | 到达成本 |
|---|---|---|---|
| non-virtual(静态已知 T) | 直接 `MetaClassOf<T>` | 0 | 0,编译期 |
| virtual(擦除 `RootObject*`) | `p->DynamicMetaClass()` | 0(复用 vptr) | 1 vcall |

## 6. 引用计数:复用 Mashiro 的 opt-in mixin(取代 AddRef/Release)

性能与内存双优:RootObject 不持 cRef;只有真正共享所有权的类 opt-in 引用计数 mixin。
栈/`unique_ptr`/`expected` 路径零计数开销。**不新建 `RefCounted.h`**——直接复用
`Mashiro/Core/RefCountedMixin.h` 已实现的两个 CRTP mixin:

- `Mashiro::RefCounted<Self, Bits=16>` —— 单线程计数(无原子,零同步成本)。
- `Mashiro::RefCountedAtomic<Self, Bits=16>` —— 线程安全计数,采用 `relaxed`-add /
  `acq_rel`-sub / 零时 `acquire`-fence 的 Boost 风格内存序,与本设计一致。

二者均为非虚、CRTP、直接公有基类,`AddRef()` / `Release()`(归零即 `delete static_cast<Self*>`)
/ `RefCount()`,初值 1。跨线程共享所有权的 Object 选 `RefCountedAtomic<Self>`,单线程场景选
`RefCounted<Self>`。`intrusive_ptr<T>` 单泛型 RAII 句柄取代一接口一 `_var`。

```cpp
#include <Mashiro/Core/RefCountedMixin.h>

struct [[=Anno::Meta{.type = ClassType::Implementation}]] CircleImpl
    : MetaNode<CircleImpl>, Mashiro::RefCountedAtomic<CircleImpl> { /*...*/ };
```

## 7. IID hasher:据 Polymorphism::Vtable 影响因素算 StableHash

无 `[[=Iid("DCE:...")]]` 注解 override 时,IID 由接口的 **ABI 摘要**算出,使 IID 精确随
vtable 形状变化——签名一变 IID 变,旧客户错连即暴露。影响因素与
`Mashiro::Polymorphism` 的方法选择**严格同源**:

- 选中方法集:非静态非特殊成员函数,`virtual`/pure-virtual 或 `[[=Anno::Force]]`,
  减去 `[[=Anno::Skip]]`,声明序。
- 每方法的 (标识符、返回类型、参数类型序列、this 的 const/volatile)。
- 引用限定**不喂**(Polymorphism 的 thunk 丢弃它)。
- token 间以分隔符喂入,防止拼接歧义。

```cpp
template <typename I> inline constexpr Iid StableHash =
    Iid{ Hashing::Uuid{ Detail::AbiDigest<I>() }.WithRfc4122() };

template <typename I> inline constexpr Iid IidOf = []consteval {
    auto a = std::meta::annotations_of(^^I, ^^Iid);
    return a.empty() ? StableHash<I> : std::meta::extract<Iid>(a[0]);  // 单一开关,非双轨
}();
```

`Iid` 增 consteval 构造器,既是结果类型又是注解载体:`Iid(Uuid)` 与
`Iid(std::string_view)`(解析 GUID 字面量,失败即编译错误)。

## 8. QueryInterface 与 Extension(Legacy:不用 Polymorphism::Adapter)

继承模型下三分支,DirectCast 是主路径:

| 关系 | 分支 | 成本 | 等价 |
|---|---|---|---|
| `T` C++ 继承接口 `I` | `static_cast<I*>(p)` | 0 | BOA |
| `I` 由 `T` 的 DataExtension 实现 | 访问扩展成员后 `static_cast` | 1 成员访问 | BOA+Ext |
| 运行期加载的扩展实现 `I` | registry 侧表查 `RootObject*→扩展` | 冷路径,1 哈希查+锁 | TIE |

静态版 `QueryInterface<I>(T*)` 用 `if constexpr` 选分支;擦除版 `QueryInterface<I>(RootObject*)`
走 `DynamicMetaClass()` → phf 查 `IidOf<I>`。DataExtension 存储采用混合:编译期已知走内联
成员,运行期加载走 registry 侧表(单边演进能力)。

## 9. 旧 CATMetaClass 字段 → 三层映射(技术债清除对照)

| 旧 `CATMetaClass` 字段 | 新归宿 | 改进 |
|---|---|---|
| `_guid` | `MetaCore::iid` | GUID→hash,内部派发单条 cmp |
| `_classname` | `MetaCore::qualifiedName` | 编译期 `display_string_of`,无运行期 set |
| `_meta_of_base` | `MetaCore::omBase` | 编译期解析,稳定指针 |
| `_implementations` | `MetaLinks::extendedBy` | 加载期单线程 fold-in,RCU 只读 |
| `_interface_head`+`_interface_number` | `MetaLinks::dispatch`(phf) | 链表 walk → O(1) |
| `_next` | 链接器 section `__start/__stop` | 无 SIOF,无运行期插入 |
| `_typeinfo_pointer`/`_alias`/`_FWname`/`_auth` | `MetaDynamic` | 懒分配,不污染热路径 |
| `IsAKindOf(const char*)` | `std::derived_from`(编译期)/omBase 链遍历 | 95% 编译期决议 |
| `IsEqual`(双 QI+指针比较) | 直接 `RootObject*` 指针比较 | 零开销 |

## 10. 阻塞修复:Identity.h 当前不一致

当前 `Identity.h` 处于半重构状态、**无法编译**:头注释与 `IdentityTest.cpp` 引用
`Kinds` 内联命名空间、`ClassTypeIndicator` 类型、`Interface`/`Implementation` 等指示常量,
但这些定义已缺失;`Detail::CollectIndicatorKinds`/`VerifyIndicatorCompleteness` 仍引用未定义
的 `^^Kinds` 与 `ClassTypeIndicator`,而 `ReadClassType` 改读 `Anno::Meta`,三者矛盾,
第169行 `static_assert` 必编译失败。

调和方案:保留 `Anno::Meta` 作为单一注解载体(携带 `type`/`extends`/`implements`);
删除 `Kinds`/`ClassTypeIndicator` 残骸及其完整性校验;`IdentityTest.cpp` 中对应用例改写为
基于 `Anno::Meta` 与 `ClassTypeOf` 的等价断言。`MetaObject`(旧的四 `vector<DumbPtr>` 草稿)
被三层 `MetaClass` 取代,移除。

## 11. 文件落点

```
Yuki/include/Yuki/Core/
├── Identity.h          调和:Anno::Meta + ClassTypeOf 角色查询(修复 §10)
├── MetaClass.h         MetaCore / MetaLinks / MetaDynamic / MetaClass + MetaCoreOf/MetaClassOf
│                       + 静/动访问面 + IID hasher(StableHash/IidOf)
├── RootObject.h        RootObject 纯锚点 + MetaNode 注入层
└── Legacy/RootClass.h  退场:转发到 RootObject.h(空壳 include shim)

引用计数复用 Mashiro/Core/RefCountedMixin.h(RefCounted / RefCountedAtomic),不新建文件。
```

## 12. 实施次序

1. 修复 `Identity.h`(§10)——解除编译阻塞,保留 `Anno::Meta`/`ClassTypeOf`。
2. `MetaClass.h`:三层结构 + `BuildMetaCore` + 静/动访问面 + IID hasher。
3. `RootObject.h`:`RootObject` + `MetaNode`。
4. 引用计数:复用 `Mashiro/Core/RefCountedMixin.h`(无新增文件)。
5. 全程 Doxygen 注释(`/** */`,`@brief` 等,行宽 ≤120)。

## 13. 测试策略

每个单元一个测试,沿用 `add_yuki_test(Core <Name>)` + Catch2 + `STATIC_REQUIRE`:

- **IdentityTest**(改写):`Anno::Meta` 驱动的 `ClassTypeOf`/角色 concept,去除 `Kinds` 残骸。
- **MetaClassTest**:`MetaCoreOf<T>` 各字段编译期正确;静/动访问面同源;`IidOf` 注解 override
  vs `StableHash`;签名变化导致 IID 变化;`extends`/`implements` 解析;`isAKindOf` 链遍历。
- **RootObjectTest**:non-virtual 与 virtual 两路返回同一 MetaClass;`MetaNode` 注入零手写;
  OM 继承链最派生胜出;per-instance 仅 vptr(sizeof 断言)。
- **RefCountedTest**:retain/release 计数;移动不增减;最后一个 release 触发析构;非 opt-in
  类零计数字段(sizeof 断言)。

性能断言:静态访问面编译期折叠(`STATIC_REQUIRE`);擦除到达 1 vcall;RootObject `sizeof`
等于单 vptr;非计数类无额外字节。

