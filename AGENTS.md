# AGENTS.md — C++23/26 前沿工程范式 (Cutting-Edge C++23/26 Engineering Paradigms)

本文是一份独立于任何具体项目的 C++23/26 编码知识库，面向 2026 年的工程标准。它约束 AI Agent 在生成、重构、审查 C++ 代码时的范式选择，核心诉求有三条，按优先级降序排列：

1. 编译期优先。能在 translation 阶段确定的计算、类型派发、代码生成，一律前移到编译期，运行时只承担真正依赖运行时数据的工作。
2. 运行时高性能且缓存友好。运行时路径以数据布局和访存模式为第一设计变量，而非以抽象层次为第一设计变量。
3. 零技术债。不为向后兼容做架构妥协，不保留 SFINAE、宏元编程、手写 trait 偏特化等已被语言新特性取代的旧范式。

工具链假设为 COCA clang-p2996（Bloomberg fork, Clang 21, libc++），编译标志 `-std=gnu++26 -freflection-latest`。`-freflection-latest` 一并启用 P2996 反射、P3096 参数反射、P1306 expansion statements、P3289 consteval blocks、P3381 反射语法、P3385 attribute reflection。其他语言取最新稳定版。

---

## 1. C++26 静态反射 (P2996)

反射是 C++26 最大的语言变更，2025 年 6 月正式并入标准。它把源码实体的元数据提升为编译期一等值，使代码生成、序列化、ORM、命令行解析等过去依赖宏或外部代码生成器的任务，回归到语言内部。

### 1.1 两个原语

反射的全部能力建立在两个原语之上。

| 原语 | 语法 | 语义 |
|------|------|------|
| reflect operator | `^^E` | 对实体 `E`（类型、变量、函数、模板、命名空间、成员、常量值）求其反射，结果类型为 `std::meta::info` |
| splice | `[: r :]` | 将反射 `r` 拼接回源码语境，还原为类型、表达式、模板参数或成员访问 |

`std::meta::info` 是定义于 `<meta>` 的不透明 consteval 标量类型。它在同一类型、同名但不同作用域下比较不相等，其拷贝比较相等，标识语义类同于 lvalue 的机器地址唯一性。绑定反射须带 `consteval` 或 `constexpr` 限定。

注意 reflect operator 在 P3381 后由单 `^` 改为双 `^^`，因单 `^` 与 Clang 既有扩展冲突。COCA 启用 `-freflection-latest`，使用 `^^`。

### 1.2 内省与代码生成的标准模式

最常见的模式是遍历类型的非静态数据成员，对每个成员做统一处理。下例为一个泛型成员遍历骨架：

```cpp
#include <meta>

template <typename T>
consteval std::vector<std::meta::info> data_members() {
    return std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::current());
}

template <typename T>
void for_each_member(const T& obj, auto&& fn) {
    template for (constexpr std::meta::info m : std::define_static_array(data_members<T>())) {
        fn(std::meta::identifier_of(m), obj.[:m:]);
    }
}
```

其中 `obj.[:m:]` 是成员 splice，`std::meta::identifier_of(m)` 取成员名（`std::string_view`）。`template for` 是 expansion statement，在编译期对静态范围逐元素实例化循环体，每次迭代 `m` 是独立的 constexpr 值，因此 `[:m:]` 合法。

`define_static_array` 把一个 consteval 产生的 `std::vector<info>` 提升为静态存储期数组，这是把编译期容器喂给 `template for` 的标准桥接。直接对 `std::vector` 做 `template for` 在当前实现不被支持。

### 1.3 注解 (Annotations, P3385)

P3385 扩展 `[[...]]` 语法，允许把强类型的编译期值附着到声明上，再由反射读取。这取代了过去用空标签类型或宏做元数据标记的做法。

```cpp
enum class HashPolicy { ignore };

struct Entity {
    float position[3];
    [[=HashPolicy::ignore]] Cache cache;   // 标注 cache 不参与哈希
};
```

读取注解用 `std::meta::annotations_of` 或按类型查询的 `annotation_of_type`。命令行解析、序列化字段重命名、ORM 列映射均以此为基础。一个完整的反射驱动 CLI 解析库（clap）即通过注解声明 help 文本与长短选项名，解析逻辑全部由反射在编译期生成。

### 1.4 聚合类型生成 (define_aggregate)

`std::meta::define_aggregate` 在编译期根据一组 `data_member_spec` 合成新的类定义。它使 struct-of-arrays 变换、proxy 类型、编译期 schema 到类型的映射成为可能，是反射区别于纯内省的生成能力所在。

### 1.5 C++26 未纳入的反射特性

P3687 在最终定稿阶段移除了 splice template arguments（拼接模板参数），推迟至 C++29。即不能写 `C<[:r:]>` 直接把反射拼为模板实参。需要此能力时，改用模板元函数间接转发，或等待 C++29。P3294（token sequence 代码注入）不在 C++26，COCA fork 亦未实现。

---

## 2. 编译期计算

### 2.1 关键字与限定符的精确选择

| 构造 | 语义 | 使用场景 |
|------|------|----------|
| `consteval` | 立即函数，调用必在常量求值语境 | 元函数、反射查询、编译期工厂；不希望退化到运行时的逻辑 |
| `constexpr` | 可在编译期亦可在运行时 | 双路径函数；多数泛型数值算法 |
| `constinit` | 强制静态初始化在编译期完成 | 全局/静态变量，消除 static initialization order fiasco |
| `if consteval` | 区分当前是否处于常量求值 | 编译期走多项式核、运行时走硬件指令的双实现 |
| `if constexpr` | 编译期分支裁剪 | 按 concept/trait 选择实现分支，未选中分支不实例化 |

`if consteval` 优于旧的 `std::is_constant_evaluated()`，因为后者在 `if constexpr` 条件中恒为 true，是常见陷阱。需要编译期与运行时不同实现时用 `if consteval`：

```cpp
constexpr float fast_sqrt(float x) {
    if consteval {
        return poly_sqrt(x);        // 编译期多项式核
    } else {
        return std::sqrt(x);        // 运行时硬件指令
    }
}
```

### 2.2 Expansion Statements (`template for`, P1306)

`template for` 对编译期已知长度的范围做静态展开，循环体对每个元素独立实例化，循环变量为 constexpr。它取代了 `std::index_sequence` + 折叠表达式的索引展开惯用法，可读性和可维护性显著提升。

适用对象：`std::define_static_array` 提升的静态数组、聚合体的结构化绑定、expansion-init-list。对运行时容器或一般 constexpr range 的展开当前实现不支持。

向量、矩阵的逐分量运算应一律用 `template for` 展开，编译器对固定小维度（2/3/4）完全展开后能向量化：

```cpp
template <HomogeneousVec V>
constexpr V operator+(V a, V b) {
    V r;
    template for (constexpr int i : std::define_static_array(std::views::iota(0, VecDim<V>))) {
        r[i] = a[i] + b[i];
    }
    return r;
}
```

### 2.3 静态存储提升 (P3491)

`define_static_string`、`define_static_object`、`define_static_array` 把编译期容器或字符串提升为静态存储期对象，返回指向它的引用或指针。这是连接 consteval 计算结果与需要稳定地址的运行时代码的桥梁，也是 `template for` 消费编译期 `std::vector` 的前置步骤。

### 2.4 constexpr 能力的扩张

C++26 进一步放宽 constexpr 限制：constexpr 异常（可在常量求值中 throw/catch）、constexpr placement new、`<cmath>` 多数函数 constexpr 化。这意味着过去因 `std::vector`、异常、placement new 而无法 constexpr 的代码现在可以前移到编译期。审查代码时应主动识别这类可前移逻辑。

---

## 3. Concepts 与约束

### 3.1 彻底取代 SFINAE

`enable_if`、标签派发、表达式 SFINAE 在 C++26 语境下属于技术债，一律用 concept 与 `requires` 重写。类型选择不再用 trait 偏特化堆叠，而用 consteval 函数加 `if constexpr` 返回 `std::type_identity`：

```cpp
template <typename T>
concept HasScalarType = requires { typename T::scalar_type; };

template <typename T>
consteval auto scalar_type_impl() {
    if constexpr (HasScalarType<T>) {
        return std::type_identity<typename T::scalar_type>{};
    } else if constexpr (requires(T v) { v[0]; }) {
        return std::type_identity<std::remove_cvref_t<decltype(std::declval<T>()[0])>>{};
    }
}

template <typename T>
    requires requires { scalar_type_impl<T>(); }
using ScalarType = typename decltype(scalar_type_impl<T>())::type;
```

此写法相对 `enable_if` 偏特化的优势：单次求值而非多重特化匹配，分支由 `if constexpr` 剪枝，无互斥守卫，无歧义风险，错误信息可读。

### 3.2 约束设计原则

| 原则 | 说明 |
|------|------|
| 最小约束 | 算法只约束它实际依赖的结构。向量加法约束 `AdditiveGroup`，点积约束 `InnerProductSpace`，不在加法上要求内积 |
| 概念分层 | 按代数/拓扑结构建立 concept 偏序，如 `AdditiveGroup → VectorSpace → InnerProductSpace → NormedSpace → MetricSpace`，泛型算法挂载到恰当层 |
| subsumption | 利用 concept 的归并关系做重载决议，更约束的版本自动优先，无需手工 tag |
| ADL 桥接 | 通过 ADL 可见的自由函数让外部类型 opt-in 一个 concept，而非要求其继承基类或特化 trait |

### 3.3 概念分层与函数级约束的分工

concept 表达类型在某结构下的成员资格，函数级 `requires` 表达该实现对标量域的额外要求。二者分工明确：`InnerProductSpace<ivec2>` 成立（整数向量有内积），但 `Norm` 函数级要求 `floating_point`（需要开方）。不要把开方约束塞进 `InnerProductSpace` 概念本身，那会污染概念语义。

---

## 4. Deducing This (P0847)

C++23 的显式对象形参（deducing this）取代 CRTP，并消解 const/非 const、左值/右值重载的四份拷贝。

### 4.1 取代 CRTP

CRTP 的 `static_cast<Derived*>(this)` 样板被 `this auto&& self` 直接消除：

```cpp
struct Shape {
    void render(this auto&& self) {
        self.draw();          // 静态分派到最派生类型的 draw
    }
};
struct Circle : Shape {
    void draw() const { /* ... */ }
};
```

派生类型在 `self` 的推导类型中直接可得，无需把派生类型作为模板参数回传给基类。

### 4.2 重载去重与递归 lambda

一个 `operator[](this auto&& self, ...)` 同时覆盖 const/非 const、左值/右值四种调用，返回类型由 `auto&&` 按 self 的 cv-ref 资格推导。递归 lambda 不再需要 Y-combinator，直接 `[](this auto&& self, int n) { return n < 2 ? n : self(n-1) + self(n-2); }`。

值类别敏感的访问器、向量的 `operator[]`、状态机的链式接口，均应用 deducing this 实现单一定义。

---

## 5. Contracts (P2900)

C++26 引入契约，2025 年 6 月并入标准。它把前条件、后条件、断言提升为语言构造，替代手写 `assert` 宏与防御性 `if-throw`。

| 构造 | 语法 | 语义 |
|------|------|------|
| precondition | `void f(int x) [[pre: x > 0]];` | 函数入口检查 |
| postcondition | `int g() [[post r: r != 0]];` | 函数返回检查，`r` 绑定返回值 |
| assertion | `contract_assert(inv());` | 语句级断言 |

契约违反的处理由实现定义的 contract-violation handler 接管，支持 observe/enforce 等求值语义。库的公共接口应以契约表达不变量，而非在文档里写自然语言约定。注意契约表达式不应有可观测副作用。

---

## 6. 协程与异步 (C++23/26)

### 6.1 std::generator (P2502, C++23)

`std::generator<T>` 是标准库提供的协程惰性序列，取代手写 iterator 或回调式遍历。它与 ranges 无缝衔接，是表达惰性流、树遍历、组合生成的首选：

```cpp
std::generator<int> fib() {
    int a = 0, b = 1;
    while (true) {
        co_yield a;
        std::tie(a, b) = std::tuple{b, a + b};
    }
}
```

### 6.2 std::execution senders/receivers (P2300, C++26)

P2300 提供标准化的异步执行框架，以 sender、receiver、scheduler 三个概念为核心，是结构化并发的语言级基础设施。

| 概念 | 角色 |
|------|------|
| scheduler | 执行资源的句柄，`schedule(sched)` 产生在该资源上起步的 sender |
| sender | 描述一项异步工作及其后继，惰性，组合后才提交 |
| receiver | 三通道回调，接收 value/error/stopped 三类完成信号 |

sender 通过 `then`、`let_value`、`when_all`、`bulk` 等 adaptor 组合成异步管线，最后由 `sync_wait` 或提交到 scheduler 触发。

结构化并发的核心约束：异步操作的生命周期必须严格嵌套，子操作不得超出父操作存活期。这直接影响参数传递。P3801 指出协程式 `task` 不应按引用或 `string_view` 等引用语义类型传参，亦不应捕获 lambda 作为协程，因为悬垂风险在异步语境被放大。senders/receivers 的结构化嵌套天然保证引用存活，是优于裸协程 task 的设计。

---

## 7. 核心语言新特性 (C++26)

| 特性 | 提案 | 用途 |
|------|------|------|
| pack indexing | P2662 | `pack...[I]` 直接索引参数包，取代递归或 `tuple` 间接 |
| `#embed` | P1967 | 编译期把二进制文件嵌入为字节数组，取代 `xxd` 外部生成 |
| structured bindings as pack | P1061 | `auto [...xs] = tuple;` 把绑定引入参数包 |
| constexpr structured bindings | P2686 | 结构化绑定可用于 constexpr 语境 |
| structured bindings in conditions | — | `if (auto [ok, v] = f(); ok)` |
| `= delete("reason")` | P2573 | 删除函数带诊断理由 |
| `[[assume(expr)]]` | P1774 | 向优化器断言不变量，无运行时检查 |
| erroneous behavior | P2795 | 未初始化读取从 UB 降级为 erroneous behavior，行为确定且可诊断 |
| static `operator()` / `operator[]` | P1169 | 无状态可调用对象的调用免去隐式 this |

pack indexing 与 structured binding packs 共同消除了元组递归处理的样板。`#embed` 把 shader、字体、查找表等资源在编译期固化为常量数据，配合 `constexpr` 可全程编译期处理。erroneous behavior 是安全性方向的实质改进，未初始化变量读取不再是 UB，但仍是错误，应配合 Profiles 与 sanitizer 排查。

---

## 8. 标准库新组件

| 组件 | 提案/版本 | 取代的旧做法 |
|------|-----------|-------------|
| `std::expected<T,E>` | C++23 | 错误码、异常、`optional` + 外部错误变量 |
| `std::mdspan` | C++23 | 手写多维索引、裸指针 + stride 计算 |
| `std::print` / `std::println` | C++23 | `printf`、`iostream` 链式 `<<` |
| `std::flat_map` / `std::flat_set` | C++23 | 红黑树 `map`（缓存不友好）；扁平容器连续存储 |
| `std::generator` | C++23 | 手写惰性 iterator |
| `std::inplace_vector` | C++26 | 固定容量栈上向量，取代 `boost::static_vector` |
| `std::hive` | C++26 | 稳定地址的高吞吐对象池 |
| `std::function_ref` | C++26 | 非拥有可调用引用，取代按值 `std::function` 的开销 |
| `std::simd` | C++26 | 手写 intrinsics 的可移植 SIMD 抽象 |
| `std::linalg` | C++26 | BLAS 风格线性代数，基于 mdspan |
| `std::span` 增强 | C++26 | 连续序列的非拥有视图 |

接口返回值优先 `std::expected`，把错误纳入类型系统而非控制流。热路径的小可调用对象用 `std::function_ref` 避免 `std::function` 的堆分配与类型擦除开销。多维数值数据用 `mdspan` 表达，布局策略（layout_right/left/stride）作为模板参数显式控制访存。可移植向量化用 `std::simd` 而非平台 intrinsics。

---

## 9. 运行时性能与缓存

编译期做完类型层面的工作后，运行时路径的唯一一等设计变量是访存模式。

### 9.1 数据布局原则

| 原则 | 说明 |
|------|------|
| 数据导向设计 | 按访问模式而非按概念实体组织数据。热字段与冷字段分离 |
| SoA 优于 AoS | 批量处理同一字段时，structure-of-arrays 使每次访存填满 cache line 且可向量化 |
| 对齐控制 | 向量类型按 std430/SIMD 宽度对齐；避免 false sharing 时按 cache line（通常 64 字节）对齐 |
| 紧凑性 | 用最小够用的标量宽度；结构体字段按大小降序排列以减少 padding |

反射使 AoS 到 SoA 的变换可在编译期由 `define_aggregate` 自动生成，无需手写两套结构。

### 9.2 访存与分支

热循环避免指针追逐（pointer chasing），用连续容器（`vector`、`flat_map`、`mdspan`）替代节点式容器（`list`、`map`）。可预测分支用 `[[likely]]`/`[[unlikely]]` 标注，编译期可证的不变量用 `[[assume]]` 喂给优化器。除法在浮点路径改为乘以倒数，开方路径用 rsqrt 模式（先算平方再 `1/sqrt`，省去一次冗余开方）。

### 9.3 编译期换运行时的判据

凡满足以下条件的计算应前移编译期：输入在编译期已知、无运行时数据依赖、结果可表示为常量或静态数据。查找表、多项式系数、类型 schema、配置解析、字符串到枚举的映射，均属此类。判据是：该计算是否依赖只有运行时才存在的值。若否，前移。

---

## 10. COCA clang-p2996 工具链注意事项

| 事项 | 说明 |
|------|------|
| 编译标志 | `-std=gnu++26 -freflection-latest`，后者启用全部反射相关实验特性 |
| 标准库 | libc++（非 libstdc++）。注意 libc++ 对部分 C++26 库特性的实现进度 |
| 反射语法 | 使用 `^^`（P3381），非旧 `^` |
| expansion statements | 仅支持 expansion-init-list 与可结构化绑定的表达式；不支持对一般 constexpr range 展开，须经 `define_static_array` 提升 |
| 未实现 | P3294 token sequence 代码注入不可用；splice template arguments 不可用（C++26 已移除，推迟 C++29） |
| 已知现象 | 反射相关错误信息可能冗长；元函数在 `Sema` 层求值，复杂 consteval 可能拖慢编译 |

验证流程：功能实现前充分调研前沿特性，实现后在 ASan + UBSan 配置下编译并跑全量测试，确认零内存错误、零 UB、零回归，再做 review，修复全部可见问题直至无可 review 项。

---

## 11. 范式取代速查

下表汇总应被淘汰的旧范式及其 C++23/26 替代，审查代码时据此识别技术债。

| 旧范式 | C++23/26 替代 |
|--------|--------------|
| `enable_if` / 表达式 SFINAE | concept + `requires` + consteval `type_identity` 派发 |
| CRTP | deducing this (`this auto&& self`) |
| trait 偏特化堆叠做类型选择 | consteval 函数 + `if constexpr` |
| `std::index_sequence` + 折叠展开 | `template for` (expansion statements) |
| 宏元编程 / 外部代码生成器 | 反射 (P2996) + `define_aggregate` |
| 空标签类型做元数据标记 | annotations (`[[=value]]`, P3385) |
| `assert` 宏 / 防御性 if-throw | contracts (`[[pre:]]` / `[[post:]]` / `contract_assert`) |
| 错误码 / 裸异常做可恢复错误 | `std::expected<T,E>` |
| 按值 `std::function` 传回调 | `std::function_ref`（非拥有热路径） |
| 节点式容器做热数据 | `flat_map` / `inplace_vector` / `mdspan`（连续存储） |
| 手写 SIMD intrinsics | `std::simd` |
| `printf` / `iostream` | `std::print` / `std::println` |
| `std::is_constant_evaluated()` 配 `if constexpr` | `if consteval` |
| 递归元组处理 | pack indexing (`pack...[I]`) + structured binding packs |
| `xxd` 嵌入二进制 | `#embed` |

---

## 8. Doxygen 注释规范

本项目使用标准 Doxygen 注释生成 API 文档。所有 `@` 命令统一使用 `@` 前缀（而非 `\`）。

### 8.1 注释块格式

| 场景 | 格式 |
|------|------|
| 短描述（一行能说清） | `/** @brief Xxx. */` |
| 中长描述 | 多行 `/** ... */` 块，含 `@brief`、`@details`、`@tparam`、`@param` 等 |
| 成员后注释 | `int x; ///< Brief description.` |

**禁止**：`/// @brief` 三斜线前置形式（统一用 `/** */`）；`//!` Qt 形式。

### 8.2 文件头

每个 `.h` 文件顶部必须有：

```cpp
/**
 * @file FileName.h
 * @brief One-line summary.
 * @ingroup ModuleName
 */
```

`@ingroup` 指定所属模块（`Core`、`Math`、`Geom`、`Surface`、`Testing` 等）。

### 8.3 分组 (Grouping)

**类内成员分组**使用单行 banner 格式 — 将 `@name` 与 `@{` 合并到一行，尾部用破折号 `—` 填充至视觉醒目，兼具可读性与 Doxygen 语义：

```cpp
/** @name Constructors @{ —————————————————————————————————————————————— */

/** @brief Default: empty string. */
constexpr FixedString() = default;

/** @brief From a string literal. */
consteval FixedString(const char (&s)[N + 1]) : len_(N) { ... }

/** @} —————————————————————————————————————————————————————————————————— */
```

规则：
- 开头 `/** @name Title @{ ——...—— */`，尾部破折号填充到约 80 列。
- 结尾 `/** @} ——...—— */`，同样填充对齐。
- 一个 section 只占 **1 行开 + 1 行关**，比三行版节省空间且比 `// ----` 具备语义。

**跨文件模块分组**使用 `@defgroup` / `@addtogroup` + `@{` / `@}`：

```cpp
/** @addtogroup Core @{ */
// ... declarations ...
/** @} */
```

优先级：`@ingroup` > `@defgroup` > `@addtogroup` > `@weakgroup`。

### 8.4 常用命令速查

| 命令 | 用途 |
|------|------|
| `@brief` | 一句话简介（必填） |
| `@details` | 详细描述（空行后自动成 details） |
| `@tparam T` | 模板参数 |
| `@param[in/out] name` | 函数参数 |
| `@return` / `@retval` | 返回值 |
| `@pre` / `@post` | 前后置条件 |
| `@note` / `@warning` / `@deprecated` | 提示 |
| `@sa` / `@see` | 参见 |
| `@code{.cpp}` ... `@endcode` | 内嵌代码示例 |
| `@since` | 版本引入 |
| `@p name` | 引用参数名（高亮） |

### 8.5 原则

1. 短注释优先一行：`/** @brief Clear contents. */`，不拆行。
2. 只要函数签名不自解释，就写 `@brief`。
3. 参数用 `@p` 行内引用代替反引号，如 `Remove @p suffix from the back`。
4. 代码示例用 `@code{.cpp}` ... `@endcode` 而非 markdown 三反引号。
