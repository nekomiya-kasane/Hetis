# Yuki — 现代化 CAA 元对象体系设计文档

本目录是 Yuki 子项目的核心设计文档：把 CATIA V5 / CAA V5 那套始于 2000 年的"COM + 元对象 + 字典 + Automation"体系，在 C++26（P2996 反射、P3385 注解反射、P3289 consteval block、P1306 expansion statements、P3096 参数反射）下做完整重写，**保留全部架构属性，清除全部技术债**。

工具链假设：COCA clang-p2996（Bloomberg fork, Clang 21, libc++），编译标志 `-std=gnu++26 -freflection-latest`，与仓库根 AGENTS.md 一致。

## 章节

| 文件 | 主题 |
|---|---|
| [00-baseline-caa-v5.md](00-baseline-caa-v5.md) | CAA V5 基线：组件 / 接口 / 扩展 / TIE / BOA / OM 继承 / 字典 / Automation 全景与必须保留的语义 |
| [01-thesis.md](01-thesis.md) | 立论；2026 年 C# / C++/WinRT / ATL 参照系；七大支柱总览 |
| [02-pillars-identity-interface-component.md](02-pillars-identity-interface-component.md) | 支柱 1 身份与 IID；支柱 2 接口；支柱 3 组件与扩展 |
| [03-pillars-dispatch-tie-ownership-determinism.md](03-pillars-dispatch-tie-ownership-determinism.md) | 支柱 4 派发（替代 metaobject chain）；支柱 5 TIE/BOA 融合；支柱 6 所有权；支柱 7 Determinism 编译期化 |
| [04-system-mechanics.md](04-system-mechanics.md) | `.dico` → manifest；工厂；Automation/IDispatch；错误处理 |
| [05-reflection-and-perf.md](05-reflection-and-perf.md) | 反射 / 注解 / consteval 块各司其职；性能账本 |
| [06-interop-and-limits.md](06-interop-and-limits.md) | 与 CAA/COM 单向互操作；C++26 短板 |
| [07-roadmap.md](07-roadmap.md) | 实施路线（按依赖顺序的九层切分） |

## 阅读顺序

1. 先读 `00` 建立 CAA V5 词汇与语义。
2. `01` 给出整体立论与七支柱目录。
3. `02`–`03` 是核心设计正文。
4. `04` 处理跨编译期/运行期边界的系统机制。
5. `05` 把零散反射用法编织成单一映射表，配性能账本。
6. `06` 给出与遗留体系互操作以及 C++26 必须诚实承认的边界。
7. `07` 是落地次序。

## 一句话定位

**编译期可知的全部进 `consteval` 与反射；运行期才能知道的（动态加载的插件清单）只保留单一 manifest 符号导出这一最小化运行期 ABI。** 这套设计的全部紧张关系都围绕这条线展开。
