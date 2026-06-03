# C++26 Reflection-Based ImGui Attribute System

## 1. 问题定义

目标不是写一组 `ImGui::SliderFloat()` 的包装函数，而是建立一个从 C++ 数据结构到即时模式编辑界面的结构保持映射：

```text
C++ data type
  -> reflection schema
  -> annotation-resolved UI model
  -> ImGui / ImPlot / ImGuizmo rendering backend
  -> validation / transaction / preset / documentation
```

此系统将 data struct 视为唯一事实源。字段、类型、枚举、约束、默认值、分组、可见性、验证规则、编辑事务和显示策略都从 C++ 类型系统和注解中推出，ImGui 只作为一个具体渲染后端存在。换言之，ImGui 从“手写命令序列”被重新刻画为“C++ 数据模式的即时投影”。

## 2. P2996.md 对设计边界的约束

Bloomberg `clang-p2996` 的 `P2996.md` 表明，此 fork 不是单一 P2996 核心实现，而是把多项反射相关实验特性聚合在同一分支中：P2996 静态反射、P1306 expansion statements、P3293 静态存储辅助、P3385 attribute reflection、P3394 annotations 等。设计必须区分三层：

| 层次 | 作用 | 在系统中的位置 |
|---|---|---|
| P2996 core reflection | `^^`、`std::meta::info`、`members_of`、`type_of`、`identifier_of`、`display_string_of`、splicer `[: :]` | 构造字段表、访问成员、推导类型、生成绘制逻辑 |
| P3385 attribute reflection | 反射传统 C++ attribute | 兼容已有 `[[vendor::attr]]` 或编译器属性 |
| P3394 annotations | 用强类型常量表达式标注声明 | 承载 ImGui 语义，应作为首选 DSL |

公开文档可以称此系统为 attribute system，但实现上应优先使用 P3394 annotation。原因是 ImGui 元数据不是编译器提示，而是用户态语义对象：它需要类型、默认值、组合规则、策略对象、谓词、函数对象和可验证结构。传统 attribute 更像语法标签，annotation 更像可反射的常量数据。

## 3. 总体设计原则

此系统的核心原则有五个：

1. **数据结构优先**：用户先写业务 struct，再用少量 annotation 描述 UI。没有 annotation 时系统仍能给出可用默认界面。
2. **强类型元数据优先**：所有 UI 标记都是 C++ 类型，而不是字符串解析协议。
3. **默认推导与显式覆盖并存**：标量、枚举、vector、matrix、string、range、optional、variant、struct 有默认 widget；用户可以在字段、类型、项目、运行期四个层级覆盖。
4. **即时模式不等于无状态**：ImGui 的绘制是即时的，但编辑器系统必须维护 stable id、dirty state、undo stack、validation state、preset state 和 per-field UI state。
5. **反射 schema 可复用**：同一套 schema 不只服务 ImGui，还应服务序列化、文档、搜索、命令面板、脚本绑定、热重载和调试器。

## 4. 用户侧 API 形态

推荐用户写法如下：

```cpp
namespace ui = Mashiro::UI::anno;

enum class ToneMapper {
    Reinhard,
    ACES,
    Filmic,
};

struct [[=ui::panel{"Renderer"}]]
       [[=ui::title{"Render Settings"}]]
       [[=ui::preset_domain{"renderer"}]]
RendererSettings {
    [[=ui::group{"Exposure"}]]
    [[=ui::label{"Enable HDR"}]]
    bool hdr = true;

    [[=ui::group{"Exposure"}]]
    [[=ui::slider{.min = -10.0f, .max = 10.0f, .speed = 0.01f}]]
    [[=ui::format{"%.2f EV"}]]
    [[=ui::tooltip{"Log exposure bias in EV units."}]]
    float exposure = 0.0f;

    [[=ui::combo{}]]
    [[=ui::label{"Tone Mapper"}]]
    ToneMapper tone_mapper = ToneMapper::ACES;

    [[=ui::group{"Bloom"}]]
    [[=ui::visible_if<[](RendererSettings const& s) {
        return s.hdr;
    }>{}]]
    [[=ui::slider{.min = 0.0f, .max = 8.0f, .speed = 0.01f}]]
    float bloom_radius = 1.5f;

    [[=ui::color{.mode = ui::color_mode::rgb, .alpha = false, .hdr = true}]]
    Mashiro::Vec3f tint = {1.0f, 1.0f, 1.0f};
};
```

用户调用端保持极小：

```cpp
RendererSettings settings{};

if (Mashiro::UI::Draw("Renderer", settings)) {
    renderer.Apply(settings);
}
```

复杂场景中可以传入上下文：

```cpp
Mashiro::UI::DrawContext ctx{
    .mode = Mashiro::UI::edit_mode::deferred,
    .preset_store = &preset_store,
    .undo_stack = &undo_stack,
    .asset_registry = &asset_registry,
};

Mashiro::UI::Draw(ctx, settings);
```

## 5. Annotation 词汇表

Annotation 应被设计为一个小型代数系统。每个 annotation 是一个独立语义原子，resolver 负责合并它们。

### 5.1 展示语义

| Annotation | 作用 |
|---|---|
| `label{text}` | 覆盖字段显示名 |
| `tooltip{text}` | 字段说明 |
| `title{text}` | 类型或面板标题 |
| `description{text}` | 长说明，可进入文档和 hover popup |
| `icon{kind}` | UI 图标标记 |
| `unit{text}` | 物理单位或显示单位 |
| `format{text}` | `printf` 风格格式或项目自定义格式 |
| `localization_key{text}` | 多语言系统的稳定 key |

默认 label 来自 `identifier_of(member)`。若成员没有 identifier，使用 `display_string_of(member)` 作为诊断文本，不作为稳定 ID。

### 5.2 布局语义

| Annotation | 作用 |
|---|---|
| `panel{name}` | 类型对应顶层窗口或 inspector 面板 |
| `group{path}` | 字段分组，支持 `Renderer/Bloom` 路径 |
| `tab{name}` | 放入 tab bar |
| `columns{n}` | 属性表布局 |
| `order{n}` | 字段排序 |
| `same_line{}` | 与前一项同排 |
| `tree{open_by_default}` | 嵌套结构以树节点显示 |
| `advanced{}` | 默认折叠到 Advanced 区域 |
| `hidden{}` | 默认不显示，但 schema 仍保留 |

布局不是写死在字段遍历顺序里。resolver 先把成员集合映射为 field graph，再根据 `order`、`group`、`tab`、`advanced` 进行稳定排序。

### 5.3 Widget 语义

| Annotation | 默认适用类型 | ImGui 后端 |
|---|---|---|
| `input{}` | arithmetic、string | `InputScalar`、`InputText` |
| `drag{speed, min, max}` | arithmetic、vector | `DragScalar`、`DragFloatN` |
| `slider{min, max, speed}` | arithmetic、vector | `SliderScalar`、`SliderFloatN` |
| `checkbox{}` | bool | `Checkbox` |
| `combo{}` | enum、variant | `BeginCombo` / `Selectable` |
| `radio{}` | enum | `RadioButton` |
| `color{mode, alpha, hdr}` | Vec3/Vec4/array | `ColorEdit3/4` |
| `flags{}` | bitmask enum | checkbox list |
| `asset{kind}` | handle/path/string/id | asset picker |
| `file_path{filters}` | path/string | file picker |
| `curve{}` | vector of points | custom curve editor |
| `plot{}` | numeric sequence | ImPlot |
| `gizmo{space}` | transform/matrix | ImGuizmo |
| `texture_preview{}` | texture handle | ImGui image preview |
| `table{}` | range of struct | `BeginTable` |

Widget 选择遵守偏序：

```text
field annotation
  > type annotation
  > user type adapter
  > project policy
  > built-in default
```

### 5.4 约束与验证

| Annotation | 作用 |
|---|---|
| `range{min, max}` | 硬范围，提交前校验 |
| `soft_range{min, max}` | UI 滑动范围，但允许手动输入越界 |
| `step{value}` | 数值步长 |
| `precision{n}` | 小数显示精度 |
| `log_scale{}` | 对数滑条 |
| `normalized{}` | 约束到 `[0, 1]` |
| `regex{pattern}` | 字符串校验 |
| `choices{...}` | 离散取值集合 |
| `validate<Predicate>{}` | 字段级谓词 |
| `invariant<Predicate>{}` | 类型级跨字段不变量 |
| `warning<Predicate>{}` | 非阻塞警告 |

`Predicate` 应是 captureless callable，作为 structural NTTP 或 annotation 对象保存：

```cpp
[[=ui::validate<[](float v) { return v >= 0.0f; }>{}]]
float radius = 1.0f;
```

跨字段约束接收完整对象：

```cpp
struct [[=ui::invariant<[](auto const& s) {
    return s.min_value <= s.max_value;
}>{}]]
RangeConfig {
    float min_value;
    float max_value;
};
```

### 5.5 行为语义

| Annotation | 作用 |
|---|---|
| `readonly{}` | 显示但不允许编辑 |
| `readonly_if<Predicate>{}` | 条件只读 |
| `visible_if<Predicate>{}` | 条件可见 |
| `enabled_if<Predicate>{}` | 条件启用 |
| `deferred{}` | 编辑临时副本，失焦或 Apply 后提交 |
| `immediate{}` | 每帧直接写回 |
| `commit_on_enter{}` | 文本输入按 Enter 提交 |
| `undoable{}` | 生成 undo command |
| `transient{}` | 不参与 preset/serialization |
| `resettable{}` | 显示 reset to default |
| `dangerous{}` | 需要确认或醒目标识 |

## 6. 核心元模型

系统内部应构造四类描述符。

```cpp
namespace Mashiro::UI {

struct TypeDesc {
    std::string_view display_name;
    std::span<const Annotation> annotations;
    std::span<const FieldDesc> fields;
};

struct FieldDesc {
    std::string_view stable_name;
    std::string_view label;
    std::meta::info member;
    std::meta::info type;
    std::span<const Annotation> annotations;
    FieldPath path;
};

struct WidgetDesc {
    widget_kind kind;
    WidgetPolicy policy;
};

struct DrawResult {
    bool changed;
    bool committed;
    bool valid;
};

}
```

实际实现中不一定要把 `std::meta::info` 存入运行期结构。快速路径可以完全模板化：

```cpp
template <class T>
bool DrawStruct(DrawContext& ctx, T& object) {
    bool changed = false;

    template for (constexpr auto member :
        std::define_static_array(
            std::meta::nonstatic_data_members_of(
                ^^T,
                std::meta::access_context::current()))) {

        changed |= DrawField<member>(ctx, object, object.[:member:]);
    }

    return changed;
}
```

描述符路径和模板路径应同时存在：

1. 模板路径用于最高性能、最少运行期开销的绘制。
2. 描述符路径用于搜索、文档、preset diff、属性表过滤、远程编辑和脚本绑定。

## 7. Schema 构建流程

`SchemaOf<T>()` 是一个 consteval/meta-level 管线：

```text
reflect type T
  -> collect members
  -> collect member annotations
  -> collect type annotations
  -> infer default widgets
  -> merge explicit overrides
  -> validate annotation compatibility
  -> emit static schema
```

伪代码：

```cpp
template <class T>
consteval auto BuildSchema() {
    auto type = ^^T;
    auto members = std::meta::nonstatic_data_members_of(
        type,
        std::meta::access_context::current());

    // clang-p2996 中 vector 不能直接逃出常量求值边界，
    // 需要通过 define_static_array 固化为静态数组视图。
    return MakeSchema<T>(std::define_static_array(members));
}
```

字段名规则：

```text
stable_name = ui::id annotation
           or identifier_of(member) if has_identifier(member)
           or hash(parent type + member order + display_string_of(member))

label = ui::label annotation
     or humanize(stable_name)
```

类型显示名规则：

```text
if has_identifier(^^T) and T is a named entity:
    identifier_of(^^T) may be used for compact display
else:
    display_string_of(^^T)
```

通用类型名不得使用 `identifier_of(^^T)`，因为指针、引用、cv 组合、匿名类型、模板特化都没有通用 identifier。

## 8. Widget 分发系统

Widget 分发应使用 CPO，而不是把所有逻辑写进一个巨大 `if constexpr`。建议提供三层扩展点。

### 8.1 Built-in renderer

内置 renderer 处理常见类型：

```text
bool -> Checkbox
integer -> InputScalar / DragScalar / SliderScalar
float/double -> InputScalar / Drag / Slider
enum -> Combo / Radio / Flags
string -> InputText
Vec<N> -> scalar N / color / direction
Mat<R,C> -> matrix table / transform gizmo
optional<T> -> enabled checkbox + nested T
variant<Ts...> -> active alternative combo + nested editor
range<T> -> list/table/tree
struct -> recursive property tree
```

### 8.2 Type adapter

第三方类型不能改源码时，用户写 adapter：

```cpp
template <>
struct Mashiro::UI::TypeAdapter<glm::vec3> {
    static constexpr auto annotation = ui::vector{.components = 3};

    static bool Draw(DrawContext& ctx, std::string_view label, glm::vec3& v) {
        return ImGui::DragFloat3(label.data(), &v.x, 0.01f);
    }
};
```

### 8.3 Field-level custom widget

用户可以在字段上直接指定 widget 对象：

```cpp
struct ExposureWidget {
    bool operator()(Mashiro::UI::DrawContext& ctx,
                    std::string_view label,
                    float& value) const;
};

struct CameraSettings {
    [[=ui::widget<ExposureWidget>{}]]
    float exposure;
};
```

### 8.4 ADL/CPO fallback

为了最大自由度，保留 ADL 定制点，但它不是首选路径：

```cpp
namespace User {

struct PhysicalQuantity {
    float value;
};

bool DrawImGui(Mashiro::UI::DrawTag,
               Mashiro::UI::DrawContext& ctx,
               std::string_view label,
               PhysicalQuantity& q);

}
```

分发优先级：

```text
field widget annotation
  > TypeAdapter<T>
  > ADL DrawImGui
  > built-in widget by type category
  > reflected struct recursion
  > fallback readonly display
```

## 9. ImGui 被重新刻画后的能力边界

此系统使 ImGui 从手写 UI 库变成一个数据流投影器。关键变化如下：

| 传统 ImGui 使用方式 | 反射属性系统 |
|---|---|
| 每个窗口手写 `Begin/End` 和字段控件 | data struct 自动生成窗口和字段 |
| UI 名称散落在字符串字面量中 | 字段 identifier、annotation label、localization key 统一 |
| 约束逻辑写在控件附近 | 约束成为 schema 的一部分 |
| UI、序列化、文档各写一套 | schema 同时驱动 UI、JSON、文档、preset |
| 改字段名容易漏 UI | 反射遍历字段，不需要同步手写控件 |
| 调试面板是临时代码 | 调试面板成为类型系统的自然视图 |

因此 ImGui 可以承担六类新角色：

1. **配置编辑器**：renderer、camera、post-process、physics、AI、input、audio 的实时配置窗口。
2. **资源 inspector**：texture、material、shader、mesh、animation、scene graph 的属性面板。
3. **Vulkan debug console**：pipeline state、descriptor binding、render pass、queue、timeline semaphore 的结构化查看器。
4. **实验参数面板**：算法参数、训练参数、benchmark 参数从 struct 直接生成。
5. **文档生成器**：每个 field 的 label、unit、range、default、tooltip 自动形成 reference docs。
6. **远程控制协议**：同一 schema 可导出为 JSON schema 或 RPC schema，由远程工具修改本地对象。

## 10. 面向 Vulkan/Renderer 的专用注解

当前仓库已有 Vulkan、ImGui、ImGuizmo、ImPlot、RenderDoc、Tracy 等调试和图形基础设施，因此 UI annotation 不应局限于常规表单。建议加入图形领域注解：

| Annotation | 用途 |
|---|---|
| `gpu_resource{kind}` | 显示 Vulkan handle、lifetime、debug name |
| `descriptor_binding{set, binding}` | 把字段和 descriptor 绑定关系联通 |
| `shader_param{name}` | 连接 shader uniform/push constant 名称 |
| `color_space{linear/srgb/hdr}` | 控制颜色编辑和显示转换 |
| `texture_preview{size}` | 显示纹理缩略图 |
| `matrix{rows, cols, convention}` | 显示矩阵表格或 transform 分解 |
| `transform_gizmo{space, operation}` | 调用 ImGuizmo 编辑 transform |
| `plot_history{samples}` | 将数值字段作为 ImPlot 时间序列 |
| `perf_counter{unit}` | 连接 Tracy 或内部 profiler |
| `renderdoc_capture_button{}` | 在相关面板上挂 RenderDoc capture |

示例：

```cpp
struct Material {
    [[=ui::color{.mode = ui::color_mode::rgb, .hdr = false}]]
    Mashiro::Vec3f base_color;

    [[=ui::slider{.min = 0.0f, .max = 1.0f, .speed = 0.01f}]]
    float roughness = 0.5f;

    [[=ui::asset{.kind = ui::asset_kind::texture}]]
    [[=ui::texture_preview{.size = {128, 128}}]]
    TextureHandle albedo;
};
```

## 11. 编辑事务模型

ImGui 控件通常每帧返回 `changed`。对于引擎配置，这不够。系统需要四种状态：

```text
edited   - UI 临时值被修改
changed  - 对象值已写回
committed - 用户完成一次编辑事务
valid    - 当前值满足约束
```

支持三种模式：

| 模式 | 行为 | 适用场景 |
|---|---|---|
| `immediate` | 控件改变后立即写对象 | 相机、调试参数、实时 shader 参数 |
| `deferred` | 改临时副本，Apply 后写回 | 资源导入、项目设置、昂贵重建 |
| `transactional` | 每个字段形成 command，可 undo/redo | 编辑器、scene graph、material authoring |

字段绘制返回：

```cpp
struct FieldEditResult {
    bool edited;
    bool changed;
    bool committed;
    ValidationResult validation;
};
```

在 `transactional` 模式下，系统生成 command：

```cpp
struct FieldSetCommand {
    ObjectId object;
    FieldPath path;
    Value before;
    Value after;
};
```

## 12. Preset 与序列化

每个字段可以有 preset 语义：

```cpp
struct [[=ui::preset_domain{"camera"}]]
CameraConfig {
    [[=ui::preset_key{"fov"}]]
    [[=ui::range{20.0f, 120.0f}]]
    float fov = 60.0f;

    [[=ui::transient{}]]
    bool debug_draw_frustum = false;
};
```

Preset 输出不应依赖 label。稳定路径为：

```text
type domain / stable type id / field stable id / nested field stable id
```

示例：

```json
{
  "domain": "camera",
  "type": "CameraConfig",
  "fields": {
    "fov": 75.0
  }
}
```

同一 schema 可生成：

1. ImGui 编辑器。
2. JSON preset。
3. 文档表格。
4. 命令面板条目。
5. 远程编辑 RPC。

## 13. 访问控制策略

反射 API 提供 `access_context`。UI 系统不应默认越过封装边界。建议策略如下：

| 策略 | 行为 |
|---|---|
| `public_only` | 默认，只显示当前上下文可访问字段 |
| `friend_ui` | 类型显式 friend UI schema builder，允许访问私有字段 |
| `unchecked_debug` | 仅 debug/devtools 下使用 `access_context::unchecked()` |
| `explicit_members` | 私有字段必须带 `ui::expose{}` 才进入 UI |

示例：

```cpp
class Camera {
    friend struct Mashiro::UI::Reflect<Camera>;

    [[=ui::expose{}]]
    [[=ui::slider{.min = 20.0f, .max = 120.0f}]]
    float fov_ = 60.0f;
};
```

## 14. 错误模型

错误分为三类。

| 错误 | 发现阶段 | 示例 |
|---|---|---|
| annotation 结构错误 | 编译期 | `slider` 用在 `std::string` 上且无 adapter |
| 约束不满足 | 运行期/提交期 | 用户输入超出硬范围 |
| 后端不可用 | 运行期 | `texture_preview` 缺少 renderer texture registry |

编译期错误应给出字段路径：

```text
RendererSettings.exposure:
  ui::slider requires arithmetic or vector-like value.
```

这要求 schema builder 在 consteval 阶段生成带 `display_string_of(type)` 和 `identifier_of(member)` 的诊断。

## 15. 编译期实现要点

P2996 风格代码要遵守四条规则：

1. `identifier_of` 只用于确实有 identifier 的实体，先检查 `has_identifier`。
2. 任意类型显示使用 `display_string_of`，不把它作为稳定 key。
3. `members_of`、`nonstatic_data_members_of`、`enumerators_of` 返回的容器只在常量求值内瞬时可用，跨边界时用 `std::define_static_array`。
4. `template for` 的迭代变量应写成 `constexpr auto`，使 expansion size 和元素值都保持常量表达式。

典型模式：

```cpp
template <class T>
void DrawReflected(DrawContext& ctx, T& object) {
    template for (constexpr auto member :
        std::define_static_array(
            std::meta::nonstatic_data_members_of(
                ^^T,
                std::meta::access_context::current()))) {

        auto label = [] consteval {
            if constexpr (std::meta::has_identifier(member)) {
                return std::meta::identifier_of(member);
            } else {
                return std::meta::display_string_of(member);
            }
        }();

        DrawField<member>(ctx, label, object.[:member:]);
    }
}
```

枚举 combo：

```cpp
template <class E>
    requires std::is_enum_v<std::remove_cvref_t<E>>
bool DrawEnumCombo(std::string_view label, E& value) {
    bool changed = false;

    if (ImGui::BeginCombo(label.data(), EnumName(value).data())) {
        template for (constexpr auto e :
            std::define_static_array(
                std::meta::enumerators_of(
                    std::meta::dealias(^^std::remove_cvref_t<E>)))) {

            constexpr auto name = std::meta::identifier_of(e);
            auto candidate = std::meta::extract<std::remove_cvref_t<E>>(e);
            bool selected = value == candidate;

            if (ImGui::Selectable(name.data(), selected)) {
                value = candidate;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }

    return changed;
}
```

## 16. 运行期覆盖系统

为给予用户最大自由度，annotation 不能成为唯一配置源。需要一个 override lattice：

```text
runtime session override
  > user preset override
  > project theme override
  > TypeAdapter override
  > field annotation
  > type annotation
  > built-in default
```

示例：项目希望所有 `float` 默认使用 drag，但某字段用 slider：

```cpp
Mashiro::UI::Theme theme;
theme.for_type<float>().default_widget = ui::widget_kind::drag;
theme.for_path("RendererSettings.exposure").default_widget = ui::widget_kind::slider;
```

这使同一 data struct 可以在多个 UI 中呈现不同形态：

1. 精简调试 overlay。
2. 完整属性检查器。
3. 只读文档视图。
4. 远程控制面板。
5. 面向美术的 material editor。

## 17. 搜索、过滤与命令面板

由于 schema 中有稳定路径、label、tooltip、type、unit、range、group，系统可以自动提供：

```text
Ctrl+P command palette
field search
modified-only filter
invalid-only filter
advanced-only filter
copy field path
reset group
save preset from current group
```

命令条目从 schema 生成：

```text
Renderer / Exposure / exposure
Renderer / Bloom / bloom_radius
Material / Textures / albedo
```

这比手写 ImGui 面板强，因为命令系统与 UI 控件共享同一字段身份。

## 18. 与现有 Mashiro 代码的关系

当前 `Mashiro` 已经在使用 C++26 reflection、`std::define_static_array`、`template for`、`Vec`/`Mat` 等基础设施。新系统可以自然放入：

```text
Mashiro/include/Mashiro/UI/
  Annotation.h
  Schema.h
  Draw.h
  Widget.h
  Validation.h
  Preset.h
  ImGuiBackend.h
  ImPlotBackend.h
  ImGuizmoBackend.h
```

推荐模块边界：

| 模块 | 责任 | 依赖 |
|---|---|---|
| `Annotation.h` | annotation 类型定义 | 标准库 |
| `Schema.h` | P2996/P3394 反射收集 | `<meta>` |
| `Widget.h` | widget CPO 和 TypeAdapter | schema |
| `Validation.h` | 约束和错误模型 | schema |
| `Preset.h` | stable path 和持久化 | nlohmann_json 可选 |
| `ImGuiBackend.h` | ImGui 控件绘制 | imgui |
| `ImPlotBackend.h` | plot 类型绘制 | implot |
| `ImGuizmoBackend.h` | transform gizmo | imguizmo |

`Schema.h` 不应依赖 ImGui。这样 schema 可以服务 CLI、测试、文档和远程协议。

## 19. 示例：完整配置窗口

```cpp
namespace ui = Mashiro::UI::anno;

struct [[=ui::panel{"Post Process"}]]
PostProcessConfig {
    [[=ui::tab{"Bloom"}]]
    bool bloom = true;

    [[=ui::tab{"Bloom"}]]
    [[=ui::enabled_if<[](PostProcessConfig const& c) { return c.bloom; }>{}]]
    [[=ui::slider{.min = 0.0f, .max = 10.0f, .speed = 0.01f}]]
    float bloom_intensity = 1.0f;

    [[=ui::tab{"Color"}]]
    [[=ui::combo{}]]
    ToneMapper tone_mapper = ToneMapper::ACES;

    [[=ui::tab{"Debug"}]]
    [[=ui::plot_history{.samples = 240}]]
    float frame_time_ms = 0.0f;
};

void DrawEditor(PostProcessConfig& cfg) {
    if (ImGui::Begin("Post Process")) {
        Mashiro::UI::Draw(cfg);
    }
    ImGui::End();
}
```

这一段用户代码不包含字段遍历、控件选择、枚举名转换、分组、禁用逻辑、plot 缓冲管理和稳定 ID 生成。这些都由 schema 和 backend 推出。

## 20. 测试策略

测试分四层：

| 层次 | 目标 |
|---|---|
| compile-time schema tests | 验证字段数量、字段名、annotation 合并、widget 推导 |
| unit widget tests | 验证 enum、range、string、optional、variant 的 DrawResult |
| golden schema tests | 将 schema 导出为 JSON，对比稳定输出 |
| runtime smoke tests | 创建 ImGui context，运行一帧绘制，检查无崩溃和状态变化 |

示例：

```cpp
STATIC_REQUIRE(Mashiro::UI::FieldCount<RendererSettings>() == 4);
STATIC_REQUIRE(Mashiro::UI::Field<RendererSettings, "exposure">().widget == ui::widget_kind::slider);
```

对于 UI 视觉结果，不应一开始追求像素级 golden test。更可行的是先验证 schema 和 DrawResult，然后在关键 editor 面板上加入截图测试。

## 21. 风险与规避

| 风险 | 影响 | 规避 |
|---|---|---|
| clang-p2996 实验特性变化 | annotation API 会随 fork 演进而变动 | 封装 `AnnotationReader`，不让业务代码直接依赖底层名字 |
| 编译时间上升 | 大 struct 反射展开成本高 | schema 缓存、分模块实例化、显式实例化常用类型 |
| UI identity 不稳定 | ImGui state 丢失 | `ui::id` 和 stable field path，不依赖 label |
| 私有字段泄漏 | 破坏封装 | 默认 `access_context::current()`，unchecked 只在 debug policy 下使用 |
| annotation 冲突 | UI 语义不确定 | resolver 编译期报错，例如 `slider` 与 `combo` 冲突 |
| `display_string_of` 不稳定 | preset 失效 | display string 只用于显示，不用于存储 key |
| string 编辑缓冲复杂 | `InputText` 需要可变 buffer | 后端封装 `std::string` buffer 和 callback resize |
| enum alias | 多个枚举项同值 | combo 显示第一个声明项，可用 annotation 指定 alias 策略 |

## 22. 实施路线

建议分六个阶段：

1. **M0 schema skeleton**：实现 `SchemaOf<T>`、字段遍历、默认 label、字段路径，不接 ImGui。
2. **M1 scalar widgets**：bool、int、float、double、string、enum、Vec2/3/4 的 ImGui 绘制。
3. **M2 annotation resolver**：实现 label、group、order、range、slider、drag、combo、readonly、hidden。
4. **M3 nested data model**：struct recursion、optional、variant、range、table、tree。
5. **M4 editor semantics**：validation、deferred edit、undo/redo、preset、search/filter。
6. **M5 graphics tooling**：texture preview、material editor、transform gizmo、ImPlot history、Vulkan resource inspector。

每个阶段都有独立验收标准：

```text
M0: 不链接 ImGui，仅靠 compile-time tests 验证 schema。
M1: 一个 RendererSettings struct 可生成可交互窗口。
M2: annotation 能改变 widget、label、group、range。
M3: 嵌套对象和数组能形成稳定 tree/table。
M4: preset 和 undo 能独立工作。
M5: 能编辑 material、camera、post-process，并显示 GPU resource preview。
```

## 23. 最终结论

这套系统的核心不是“用反射少写几行 UI 代码”，而是把 C++ 类型系统提升为编辑器知识库。P2996 提供结构观测和代码拼接，P3394 提供强类型语义标注，ImGui 提供即时反馈的呈现后端。三者结合后，data struct 不再只是运行期数据容器，而是同时携带可编辑性、可验证性、可持久化性、可文档化性和可远程控制性的 schema。

在这个设计下，用户拥有四级自由度：字段级 annotation、类型级 adapter、项目级 theme/policy、运行期 override。系统则保持两个不变量：稳定身份不依赖显示文本，业务 schema 不依赖 ImGui 后端。前者保证 preset、undo、搜索和远程协议稳定，后者保证未来可以复用同一套元数据生成 CLI、Web 面板、脚本绑定或离线文档。

因此，反射式 ImGui attribute system 应当被定位为 Mashiro/Nova 编辑器基础设施，而不是一个 UI helper。它将成为 renderer、resource、debugger、profiler、shader parameter、material authoring 和实验配置系统的共同元模型。
