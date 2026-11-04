# vgui 开发能力评估报告

> 评估对象：vgui v0.3.0（MIT 许可）
> 评估维度：**普通应用** / **游戏运行时** / **编辑与开发工具**
> 信息来源：`include/vgui/**` 公共头、`gui/`、`backends/`、`docs/`（runtime_roadmap / editor_roadmap / phase0_3d_editor / chart）
> 评估日期：2026-11-03

---

## 1. 定位与架构

vgui 是一套 **C 接口、保留模式 + 即时录制** 的 GUI 库，分层如下：

- **`libvgui`（核心，纯 C API）**：控件树、布局、输入路由、矢量绘制录制；不含 SDL、不含编辑器语义。
- **`vgui_editor_kit`（C++ 静态库）**：Dock 外壳、PropertyGrid、Outliner、AssetBrowser、Viewport、Timeline、NodeGraph、CodeEditor 等编辑器专用组件。
- **`vgui_chart`（可选）**：折线/面积/柱/仪表盘/JSON 仪表板。
- **`ovg`**：自带矢量渲染器；**`SDL3 GPU`** 是唯一 OS/渲染后端（桌面 Win/Mac/Linux 自动走 D3D12/Vulkan/Metal）。
- 三套组件共用同一控件模型，不为运行时/编辑器分叉。

成熟度总览：

| 领域 | 成熟度 | 关键证据 |
|---|---|---|
| 编辑与开发工具 | ★★★★★ 最成熟，路线图已"收口" | editor kit 全组件完成，`vgui_game_editor` dogfood |
| 游戏运行时 | ★★★★ 完成 R0–R5 | HUD 合成、世界空间 UI、手柄导航、性能缓存已实现 |
| 普通应用 | ★★★☆ 控件/布局/i18n 强，配套弱（菜单图标/浮点纹理已补） | 控件齐全，但后端、对话框、富文本、无障碍缺位 |

---

## 2. 普通应用（桌面 GUI）

### 2.1 能做的
- **控件齐全**：panel / label / button / checkbox / radio / switch / slider / progress / edit / separator / list / tree / menu / image / custom / splitter / tabview / menubar / dock
- **布局**：absolute / row / column / wrap + align / justify / align_content / grow / min-max / margin / padding / gap + 滚动面板
- **覆盖层**：Tooltip / Modal / Alert / Confirm / Prompt / Toast / Popup（含焦点陷阱、背景输入隔离）
- **主题**：运行时切换、控件级背景/边框/圆角/文字色/透明度/字号
- **国际化**：UTF-8 文案表、运行时语言切换、RTL 布局/文字镜像、bidi、多字体回退
- **文本编辑**：单/多行、选择、复制/剪切/粘贴、编辑框内撤销重做、IME
- **交互**：拖放（核心 C API + OS 文件拖入）、键盘/手柄焦点导航
- **图片 / 纹理**：image 控件支持 source/fit/tint/flip/九宫格/旋转/任意 2D 矩阵；纹理格式已扩展为 **RGBA8 系列（RGBA8/BGRA8 + sRGB）** + **RGBA16F / RGBA32F** + **单通道 R16F / R32F**，可承载 HDR 图像、数据 / 高度图 / 掩码、以及引擎交来的浮点纹理直接合成
- **图表**：折线/面积/柱/径向仪表盘/JSON 仪表板，十万点 min-max 降采样、十字准线、悬浮提示、图例、平移缩放、CSV 导出

### 2.2 缺的功能 / 短板
- **文件打开/保存对话框未暴露**（仅桥接 OS 文件*拖入*）；需自建或借 SDL3 对话框 API
- **富文本 / 语法高亮 / 行号 / 查找替换**：核心 `VGUI_EDIT` 仅纯文本（IME/编辑框内撤销）；但 editor kit 的 CodeEditor（T04）已完整实现多语言语法高亮、行号、查找/替换、只读富文本日志，可在普通应用中单独嵌入
- **原生（OS）菜单**：仍缺（vgui 菜单为自绘浮层，无系统原生菜单）；但菜单项图标（图片纹理 / emoji）与运行时动态重建已实现，见 §4.2
- **跨平台后端有限**：唯一后端 SDL3 GPU；**无 iOS / Android、无 WebAssembly / WebGPU 导出、无游戏主机**后端
- **无障碍（Accessibility）**：仅键盘/手柄/焦点环 + 高对比主题，**无屏幕阅读器 / ARIA 语义**
- **数据绑定**：P0–P3 已提供可选的响应式绑定层（`vgui_bind` / `vgui_bind_cb` / C++ `vgui::bind`），含编辑日志与撤销集成（`vgui_set_edit_recorder` + editor kit `binding_undo`）、`vgui::model_write` 自动脏标记、PropertyGrid 字段绑定、`vgui::vector_rows` 集合适配器与依赖驱动的 computed 绑定；仍非声明式 UI 描述（无 JSX/模板式字符串 DSL、无依赖自动分析）
- **无多线程 UI**（明确单线程：纹理创建/更新/绘制须同线程）
- **无内嵌 WebView / 音视频播放 / 网络**；高级动画仅 hover/press/check 过渡
- 版本 0.3.0、仅源码兼容（无二进制兼容承诺）；构建依赖重（C++23 + vcpkg：SDL3 / harfbuzz / fontconfig / glm / nlohmann_json / Clipper2 / ovg）

---

## 3. 游戏运行时（HUD / 游戏内 UI）

### 3.1 能做的（R0–R5 全部完成）
- **无窗口嵌入**：`VGUI_CORE_ONLY` 去掉 SDL3，引擎消费 `ovg_draw_data_t` 自建渲染通路
- **HUD 合成**：LOAD-op 叠加在引擎 3D 画面之上；离屏 FBO → 纹理互借让引擎合成
- **世界空间 UI**：屏幕空间名牌投影、UI→3D 公告牌、世界拾取与选中联动
- **手柄 / 键盘方向导航**：`vgui_focus_navigate` + SDL_Gamepad 映射（A→Enter、B→取消、Y→菜单、摇杆/十字键导航）
- **性能**：静态子树显示列表缓存、离屏基准（40 面板 draw≈6.7ms）、图集代数失效安全网
- **HUD 控件**：冷却环（`set_radial`）、飘字（`spawn_float_text`）、安全区（`set_safe_area`）；数据绑定惯例（每帧直写 + 脏检查）
- **纹理格式**：支持 RGBA16F / RGBA32F 与单通道 R16F / R32F；引擎可将 HDR / 数据（高度图、掩码）纹理经 `ovg_draw_data_t` 直接交由 vgui 合成（单通道采样返回 `(r,0,0,1)`）

### 3.2 缺的功能 / 短板（多为设计性非目标）
- **多点触摸 / 捏合手势**：仅单指触摸，R4-3 暂缓为 P2
- **跨引擎抽象缺失**：非 SDL3 GPU 的引擎必须自己写 `ovg_draw_data_t` 消费通路（中等工作量）
- **无脚本 / 热重载层**（明确非目标）
- **不提供** 场景图 / 物理 / 音频 / 资源管线（明确非目标，按设计放宿主）
- 手柄焦点环仍待真机确认（R2-3）；120fps 需子树缓存 + 图集预热配合才达标
- 无 VR / 立体 UI、无粒子 / 特效型 UI 组件

---

## 4. 编辑与开发工具（最强项，已收口）

### 4.1 能做的
- **编辑器外壳**：`editor_shell` 基于 Dock，版本化 JSON 布局持久化（保存/恢复/重置，兼容旧版）；顶部工具栏（场景搜索/Undo-Redo/Grid/视图分段）与底部状态栏（消息/选中对象/坐标/错误）已接入（S07）
- **工具栏辅助组件**：IconButton、ToggleButton、SegmentedControl、Toolbar、StatusBar、SearchBox、Breadcrumb，可绑定 `command_registry` 同步标题/禁用/复选状态（S07）
- **命令 / 撤销栈**：execute / undo / redo、事务（嵌套）/ 回滚、命令合并、失败命令协议
- **类型化 PropertyGrid**：文本/整数/浮点/布尔/枚举/颜色/Vector2‑4/资产引用/曲线/渐变、分类折叠、搜索、重置、只读、错误态、多选混合值、复制粘贴、命令栈集成（同行合并 + 批量事务）
- **Outliner**：虚拟行源、行内重命名、拖放重排/改父子、过滤高亮、双向选中同步
- **AssetBrowser**：缩略图网格/列表、路径导航 + 防抖搜索、异步缩略图加载与缓存、预览、拖放赋值/清空/丢失态
- **Viewport**：FBO 尺寸通知、轨道相机（中键平移/右键旋转/滚轮缩放/F 取景/状态保存）、拾取/框选/多选、世界网格与平移/旋转/缩放吸附、平移/旋转/缩放 Gizmo（世界/局部/父级/屏幕空间）、2D Canvas、多视口 HDR(RGBA16F+tonemap)/MSAA/深度预览
- **Table / DataGrid**（虚拟行、列宽与排序持久化）、**Timeline**、**NodeGraph**、**CodeEditor / RichText**、曲线/渐变编辑器、Console/Log
- **DPI / HiDPI**：统一 `ui_scale` 贯通核心/kit/SDL3，显示器切换自动同步
- **i18n**：同核心
- **无障碍（E05）**：完整键盘导航、焦点陷阱（Tab/Shift+Tab 在陷阱祖先内循环，模态/弹出层/浮动 Dock 自动开启）、`F10` 菜单栏、`F6`/`Shift+F6` 焦点漫游、高对比度主题（`Ctrl+Shift+H`）；`vgui_a11y_role/state/info` 推导角色/名称/值/状态，屏幕阅读器经 `a11y_listener` 接收焦点/值/状态/选中/告警事件（Toast 自动公告）；不实现平台 UIA/AT-SPI provider

### 4.2 缺的功能 / 短板
- **Dock 标签图标与右键上下文菜单**：路线图明确仍缺（未实现）
- **菜单项图标（已实现）**：`vgui_menu_set_item_icon`（图片纹理）/ `vgui_menu_set_item_emoji`（emoji/UTF-8 字形），任一菜单有图标时统一预留图标列、LTR/RTL 自动镜像；菜单内容可经 add/remove/clear/set_item_* API 运行时动态重建
- **原生（OS）菜单**：仍缺（vgui 菜单为自绘浮层，无系统原生菜单）
- **可编辑富文本（WYSIWYG 行内格式）**：语法高亮/行号/查找替换/只读富文本日志已由 T04（CodeEditor/RichText）完成；任意行内加粗/斜体/混合字号的可编辑富文本文档仍非核心能力
- 颜色空间转换仅提供挂钩，预设策略需应用配置
- 3D 场景图 / 资源数据库 / 导入管线 / 序列化：明确不在 vgui 核心，由宿主负责
- NodeGraph / Timeline 的"专业"深度未经独立验证（仅 `vgui_game_editor` 示例 dogfood 覆盖 MVP）
- 无编辑器插件 / 扩展系统、无协作编辑、无脚本化编辑器

> ✅ **已修正**：`editor_roadmap` 早前"当前边界"曾称"尚无工具栏/状态栏辅助组件"，但 `gui/editor/toolbar.cpp` 与 `status_bar.cpp` 已实现并参与构建、且 S07 已标记完成，现已在路线图第 51 行更正——工具栏/状态栏确已实现（`editor_shell` 顶部工具栏与底部状态栏已接入）。注：Dock 标签的图标与右键上下文菜单仍缺，属实。

---

## 5. 跨三个领域共通的缺失

1. **单一渲染/OS 后端**（SDL3 GPU）：桌面可用，缺移动端 / Web / 主机后端
2. **无障碍**：无屏幕阅读器支持（仅键盘/手柄/高对比）
3. **响应式/声明式数据绑定**：P0–P3 已实现——`vgui_bind`/`vgui_bind_cb`/C++ `vgui::bind` 提供双向绑定（value/text/checked/progress/visible/enabled/selection），模型变更经版本号懒 flush、用户编辑自动回写、无回声；编辑日志（`vgui_set_edit_recorder`）把用户编辑变成可撤销命令，editor kit 的 `binding_undo` 直接接到 `command_stack`（滑杆拖动合并为一步），`vgui::model_write` / `VGUI_MODEL_SET` 自动标脏，PropertyGrid 可用 `field_property` 直接绑字段，`vgui::vector_rows` 把 `std::vector<Row>` 适配成虚拟行源，`vgui_binding_add_dependency` 支持依赖驱动的 computed 值；`vgui_demo` 有绑定+撤销演示卡片，`model_test`/`editor_kit_test` 有断言。仍缺字符串表达式 DSL 与依赖自动分析，详见 `docs/data_binding_roadmap.md`
4. **无多线程 UI**，单渲染线程约束
5. **原生文件对话框**：已实现——`vgui_file_dialog_open/save/select_folder`（异步、结果从 `vgui_update` 派发），实现走 SDL3 `SDL_ShowFileDialogWithProperties`（Windows `IFileDialog` / macOS `NSOpenPanel` / Linux XDG portal），**公共头 `vgui.h` 不含任何 SDL 类型**：接口只有 C 字符串 + 回调，后端通过 provider 桥接；`VGUI_CORE_ONLY` 下 `vgui_file_dialog_available()` 返回 0。仍缺平台内嵌预览（如缩略图选择器）与"最近使用目录"记忆
6. **无高级富文本/排版**（核心层）
7. **无 WebView / 多媒体 / 网络**
8. **C API 优先**：暂无 C# / Python 等语言绑定（C 接口便于自建，但非开箱）
9. **文档为路线图式**，无完整可浏览 API 参考；版本 0.3.0 仅源码兼容
10. **无插件/扩展体系**

---

## 6. 结论

- **游戏编辑器 / 开发工具**：vgui 最强、最现成的选择，组件几乎齐全，可直接 dogfood，`vgui_game_editor` 已验证全流程。
- **游戏运行时 HUD / 游戏内 UI**：能力完备且经过基准验证，但只适合"引擎拥有主循环与渲染器、vgui 负责控件树与绘制录制"的嵌入式模式；移动端 / Web / 跨引擎需额外接 `ovg_draw_data_t`。
- **普通桌面应用**：控件、布局、主题、i18n、图表、原生文件对话框都能打，但**富文本、无障碍、移动/Web 后端**仍是明显缺口；若要替代 Qt / Flutter 需自行补齐这些配套。

### 优先级建议（若要补齐普通应用短板）
1. ~~暴露 SDL3 文件打开/保存对话框封装~~（已完成，见 §5 第 5 条）
2. 核心层补富文本/语法高亮/行号/查找替换，或把 kit 的 CodeEditor 能力下沉
3. 增加屏幕阅读器/ARIA 语义（无障碍合规）
4. 评估 WebAssembly / WebGPU 与移动端后端需求（若目标平台包含 Web/移动）
5. 提供可选的响应式数据绑定层

---

## 7. 重评变更记录（对比原始评估）

> 本报告在首版评估后又做了一次源码层重评。以下为**代码层面已落地、从而改变评估结论**的改动（其余"共通缺失"维持不变）。

### 7.1 已消解的原"缺失项"
| 原评估结论 | 当前状态 | 改动 |
|---|---|---|
| 普通应用：菜单项无图标 API | ✅ 已实现 | 新增 `vgui_menu_set_item_icon`（图片纹理）/ `vgui_menu_set_item_emoji`（emoji/UTF-8 字形）；有图标时统一预留图标列、LTR/RTL 自动镜像、禁用态淡化 |
| 普通应用：菜单不能动态重建 | ✅ 已实现 | 原评估已确认 `add/remove/clear/set_item_*` API 支持；本轮仅文档同步 |

### 7.2 新增能力（原评估未覆盖）
| 能力 | 影响领域 | 改动 |
|---|---|---|
| 纹理格式 `RGBA16F` / `RGBA32F` | 普通应用图片、游戏运行时 HDR 合成 | 公共枚举 + `vgui_texture_bpp` + OVG/SDL3 映射；同时修掉 `thumbnail_cache`/`asset_preview` 中"所有格式 4 字节"的硬编码 stride 假设 |
| 单通道 `R16F` / `R32F` | 数据/高度图/掩码纹理、引擎数据纹理 | 因 OVG 原本无单通道格式，同步扩展 vendored `ovg_c.h` 枚举与 `vg_to_sdl_format`/`format_bpp`；采样返回 `(r,0,0,1)` |
| 响应式数据绑定 P0/P1（标量双向 + 声明式糖） | 普通应用、编辑器 | `vgui_bind` / `vgui_bind_cb` / C++ `vgui::bind`（成员指针糖）；支持 VALUE/TEXT/CHECKED/PROGRESS/VISIBLE/ENABLED/SELECTION 双向；脏版本号懒 flush、编辑自动回写、无回声；`vgui_demo` 有演示卡片，`model_test` 有断言 |
| 绑定撤销集成 P2 | 编辑器、工具类应用 | 核心编辑日志 `vgui_set_edit_recorder`（带新旧值、模型推送与程序化写入不上报）+ `vgui_model_write_raw`（回放不产生新记录）+ `vgui_binding_alive`；editor kit `binding_undo` 把日志接到 `command_stack`（500ms 合并窗口，RAII 摘除）；`vgui::model_write` / `VGUI_MODEL_SET` 自动标脏；`field_property` 让 PropertyGrid 直接绑结构体字段 |
| 集合/派生绑定 P3 | 数据密集界面、编辑器 | `vgui::vector_rows<Row>` 把 `std::vector<Row>` 适配成 `vgui_row_source`（attach/refresh/invalidate_rows）；`vgui_binding_add_dependency` 让 cb 绑定在依赖模型标脏时重算（computed 值）；`vgui_binding_count` / `vgui_binding_last_flush_count` 提供诊断计数 |

### 7.3 仍维持的"共通缺失"（未变）
单一后端（SDL3 GPU，缺移动/Web/主机）、屏幕阅读器/ARIA、多线程 UI、核心层高级富文本、WebView/多媒体/网络、语言绑定、插件体系——以上均未改动，维持原结论。数据绑定（§7.2）与原生文件对话框（§7.7）已从该清单移出；数据绑定的字符串表达式 DSL、依赖自动分析仍缺（核心功能已落地，见 roadmap P3）。

### 7.4 文档一致性修正
- 普通应用 §2.2：删除"菜单项无图标 API"的错误表述。
- 编辑器 §4.2：删除一处重复的"可编辑富文本"条目。
- `editor_roadmap.md` §2.2：删除一行误入表格的开发便签，恢复表格与 §3 的空行。

### 7.5 代码一致性修正
- `property_binding.h`：`field_property` 的整型/浮点重载原先只靠默认模板实参（`enable_if`）区分，等于对同一模板重复声明，GCC/Clang 直接报 `redefinition of default argument`；已合并为单个 `is_arithmetic && !is_bool` 重载，按 `is_floating_point` 选 `integer`/`number` 类型。
- `collection.h`：`vector_rows` 补 `detach()`，避免适配器先于控件销毁时控件残留悬垂行源指针。
- 全文贯通纹理格式能力到 §2.1（图片/纹理）与 §3.1（游戏运行时 HUD）。

### 7.6 实机回归（2026-11-03，MSVC 19.51 / VS 2026 + vcpkg `x64-windows-static`）
- 全量构建 95 个目标通过，含 `vgui_headers_test` 逐头自包含编译（覆盖新增的 `binding/collection.h`、`editor/binding_undo.h`、`editor/property_binding.h`）。
- `vgui_model_test` 850 通过 / 0 失败；`vgui_editor_kit_test` PASS；`vgui_c_api_test`、`vgui_chart_test`、`vgui_runtime_embed_test`、`ovg_texture_test` 均通过。
- 回归中修掉的真实缺陷：`VGUI_BIND_TEXT` 推送 `VGUI_EDIT` 时未同步 `edit_text`（控件显示旧值、回写读不到用户输入），详见 `data_binding_roadmap.md` P0。
- 顺带修正的过期断言：节点图行高 18→24 后的几何常量（节点高 70→82、端口 y 114→120 / 134→144 / 154→168、端口矩形 y 110→116）、编辑器外壳默认 MSAA 1→4 及其 toggle 顺序、`last_flush_count` 用例（computed 值不变时不计推送）、`model_write` 用例误用已解绑滑杆、派生绑定 9+2 写成 15。
- 已知遗留：`vgui_texture_test` 2/79 失败（`cpu rotation top-left`、`cpu center rotation tl` 取到 `0x7F7F0000`），为 CPU 旋转图元边界像素的半透明混合，属采样精度问题，与上述改动无关；GPU 截图回归未在本机执行。

### 7.7 原生文件对话框（2026-11-03）
- 公共 API（`include/vgui/vgui.h`「File dialogs」段）：`vgui_file_dialog_kind`（OPEN/SAVE/FOLDER）、`vgui_file_filter`、`vgui_file_dialog_desc`、`vgui_file_dialog_fn`，以及 `vgui_file_dialog_desc_init/open/save/select_folder/show/available`。**头文件里没有任何 SDL 类型**——只有 C 字符串、整数和函数指针，C 消费方与非 SDL 后端同样可用。
- 平台桥：`vgui_file_dialog_provider_fn` + `vgui_file_dialog_set_provider/get_provider`；provider 用 `vgui_file_dialog_post_result()` 回报（可从任意线程），核心用互斥队列暂存并在 `vgui_update()` 里派发，因此回调总在 UI 线程；`vgui_file_dialog_pending()` 暴露队列长度便于诊断/测试。provider 的惰性单例状态避开了静态初始化顺序问题。
- SDL3 实现（`backends/vgui_sdl3_dialog.cpp`，库加载时用静态初始化器安装）：把描述符复制成自持有请求（SDL 会一直读过滤器数组直到回调），再调 `SDL_ShowFileDialogWithProperties`，支持 title / accept / cancel / default_location / default_name / allow_many；过滤器把 `*.png;*.jpg` 与 `png;jpg` 归一化成 SDL 要求的 `png;jpg`；父窗口取当前键盘/鼠标焦点窗口。
- C++ 便捷头 `include/vgui/file_dialog.h`：`vgui::file_dialog_options` / `file_filter` / `file_dialog_result`（`ok/cancelled/failed/path()`）+ `open_file/save_file/select_folder`，`std::function` 由 trampoline 单次调用后释放。
- 示例与测试：`vgui_demo` 新增「文件对话框 File dialog」卡片（打开/导出/选目录 + 结果行）；`vgui_model_test::test_file_dialog` 用假 provider 覆盖请求透传、结果排队 → update 派发、取消/失败三态、非法请求被拒、无 provider 时不回调。

### 7.8 双击/三击改为按控件、按档位显式启用（2026-11-03）
- 开关：`vgui_widget_set_multi_click_flags(w, flags)` / `vgui_widget_get_multi_click_flags(w)`，标志位 `VGUI_MULTI_CLICK_DOUBLE` / `VGUI_MULTI_CLICK_TRIPLE`（`VGUI_MULTI_CLICK_ALL` 为两者之和）；便捷接口 `vgui_widget_set_multi_click_enabled(w, 0|1)`（一次开关两档）、`vgui_widget_get_multi_click_enabled(w)`（任一档开启即返回 1），以及 `vgui_widget_set_double_click_enabled()` / `vgui_widget_set_triple_click_enabled()` 单独控制某一档。
- **默认关闭**：450 ms 窗口内的第二、第三次按下不再升级成双击/三击事件，而是继续派发 `VGUI_EVENT_CLICK`，连击次数仍由 `event->clicks` 提供——只处理单击的宿主代码不会漏计点击，也不必为双击写兜底分支。
- 两档互不影响：只开双击时第三次点击退回 `CLICK`；只开三击时第二次是 `CLICK`、第三次才升级；两档都开时逐级升级。
- 派发点统一走 `vgui_click_event_type()`（鼠标抬起的三条路径：标签页、列表/树行、普通控件），因此控件级开关对 item 事件同样生效；列表/树的双击**激活**属于核心行为，与事件开关解耦，仍然照常触发。
- 内置控件按需打开各自需要的档位：代码编辑器与 Markdown 视图要双击选词 + 三击选行（两档），表格、Outliner、曲线编辑器、渐变编辑器、图表只用双击（单档）。
- 测试：`vgui_model_test::test_multi_click` 20 条断言（默认无标志位、只开双击时三击仍为 CLICK、只开三击时双击仍为 CLICK、两档齐开逐级升级、清位立即回退、列表行默认关闭但激活仍生效、列表 opt-in 后升级）；`vgui_runtime_embed_test` 增加默认值断言并显式开启，覆盖注入时钟下的双击窗口；`vgui_editor_kit_test` 断言三处组件内部控件的档位（code editor=ALL、curve/gradient=DOUBLE、根面板=0）。

### 7.9 `vgui_widget` 布尔状态改为位域（2026-11-03）
- 原来的 `int visible/enabled/focusable/hovered/pressed/focused/checked/vertical/multiline/password/readonly/...`（共 28 个 int + 2 个 bool，约 114 字节）压成一个 32 位的位域块：29 个单比特标志 + `multi_click : 2`，写法仍是 `uint32_t visible : 1 = 1;`，默认值就地声明，语义与原 `int = 0/1` 完全一致。
- `vgui_widget` 现在 `sizeof == 1096` 字节。被替换掉的 28 个 `int` + 2 个 `bool` 原本占 114 字节，合并成 4 字节后单个控件少约 110 字节（实际差值随对齐略有出入）；编辑器场景成百上千个控件时差别明显。
- 非布尔的多值字段保持 `int`（`edit_drag` 0–3、`scroll_drag_axis` 0–2、`edit_scroll_drag` 0/3/4、`pressed_button` 等），避免把状态机硬塞进 1 位。
- 由于位域不能被取地址，`vgui_widget` 内部不再对这些字段使用 `&`/引用绑定；属于该结构体的私有实现细节，公共头与 ABI 不受影响。

### 7.10 单选按钮描边内收（2026-11-04）
- 现象：`vgui_demo` 单选组里**最左侧**那个单选按钮的左侧看着像被削平了一块，右侧两个同尺寸的按钮是完整的圆。
- 根因是描边几何而不是裁剪：`vgui_draw_radio` 先按半径 `d/2` 填充实心圆、再按同一半径 `d/2` 描边，描边以路径为中心向内外各扩 `bw/2`，于是圆环外沿到了控件左边再往左 `bw/2` 的位置。单选组所在行 `padding` 为 0，第一个按钮的左边界正好等于这一行的左边界，父容器的裁剪就把外溢的半个线宽连同抗锯齿一起切掉；后面的按钮左侧还有行内空间，所以看不出问题。
- 修复：描边半径改为 `d/2 - bw/2`（`std::max(0, ...)` 兜底），圆环外沿正好落在控件矩形内。复选框的描边一直是这么做的（`bx + bw/2`、尺寸减 `bw`），现在两者一致。
- 验证：本机 1080×860 窗口 8x MSAA 截图逐像素比对，三个按钮的左右描边强度都是 85、外侧相邻像素都是纯背景 30，逐行宽度序列完全一致（`5,10,12,14,16,16,18×6,17,16,14,12,10,5`），最左侧按钮不再有平直切口。
### 7.11 渐变色胶囊按钮端头修复（2026-11-04）
- 现象：`vgui_demo` 按钮样式卡片里的「pill 胶囊」（`rounding = 20`、高 31）左右两端各挂着一两条游离的 1px 竖线，端头像是被削平；`rounding` 越大越明显，`rounding ≤ height/2` 时完全正常。
- 根因在 OVG 的圆角取整顺序：`ovg_rounded_rectangle()` 先把半径 `clamp` 到 `min(w,h)/2`、再 `round()`。31px 高的胶囊得到 15.5，`round(15.5) = 16` 又越过了半高，路径自交；填充按非零环绕仍像个胶囊，但描边轮廓退化，两侧各漏出一条竖线。`vgui_draw_button_gradient()` 又把这个未取整的 `rounding` 直接喂给 `rounded_rectangle()`，所以只有渐变色按钮中招。
- 修复：`vgui_draw_button_gradient()` 里新增 `pill_radius()`，按 `min(floor(r), floor(min(w,h)/2))` 取整，底色填充、渐变填充和两条描边统一使用；填充与描边的半径从此相差不超过 0，端头是真正的半圆，描边 1px 恰好贴在外沿。
- 验证：离屏 8x MSAA 截图（`vgui_screenshot_test` 临时探针场景，验证后已移除）对比 `rounding = 8 / 14 / 20 / 40` 与高度 `24 / 28 / 30 / 31 / 32 / 33 / 36` 的组合：修复前 `rounding > h/2` 的用例在端头出现 21px 长的游离竖线，修复后全部消失；每个高度的最外侧像素都是 `borderDark`，内外对称，无 1px 亮边。

### 7.12 OVG 圆角半径口径下沉（2026-11-04）
- 起因：7.11 的修复落在调用侧（`vgui_draw_button_gradient`），OVG 内部「先把半径夹到 `min(w,h)/2`、再 `round()`」的顺序没动，任何绕开 `vgui_tools` 直接调用 `rounded_rectangle()` 的代码仍会踩到同一个半像素倒挂。
- `ovg/ovg.cpp` 新增 `o_snap_radius(limit, radius)`：先 `round()` 到整数，再用 `floor(limit * 0.5)` 夹住上界。限值是 `x.5` 时 `round` 只会把半径顶出去（15.5 → 16），所以取整必须在前、夹取在后；顺带把 NaN 与负值统一归零，不再把 NaN 点写进路径。
  - `ovg_rounded_rectangle()` 用该函数替换原来的两行 clamp + round。
  - `ovg_rounded_rectangle2()` 原本既不夹取也没有 `w/h <= 0` 保护，现在 `rx` 夹到 `floor(w/2)`、`ry` 夹到 `floor(h/2)`，并补上漏掉的 `o_finish_path()`（与 `ovg_rounded_rectangle` 对齐）。
- `ovg/tools/tools.hpp` 抽出 `round_rect_radius(w,h,r)` / `round_rect_radius2(len,r)`，`fill_round_rect`、`stroke_round_rect`、`fill_round_rect2`、`stroke_round_rect2`、`fill_round_rect_gradient`、`swatch_alpha`、`round_rect4_path` 全部改走同一口径，不再各处各写一份 `std::clamp(r, 0, min(w,h)*0.5f)`。
- 回归：`ovg_tools_test` 新增 10 条断言（总检查 95 → 105）。用例是 `h = 31`（半高 15.5）、半径给到 20 与 `rx = 20 / ry = 99`，同时覆盖「直接调原生 API」和「走 tools 预取整」两条路径：端头圆弧、上下边、内部留空、圆角切除逐点比对。
- 说明：椭圆角版本（`rounded_rectangle2`）走 `_elliptic_arc` 折线近似，描边中心线比几何 `x/y` 内收约半像素，断言取实心像素而不是几何中心。

### 7.13 按钮描边与填充的圆角同心（2026-11-04）
- 现象：`vgui_demo` 按钮卡片里，带边框的按钮在四个圆角处露出一圈比填充更暗的边缘（放大到 8 倍才能看清），`border_width = 3` 的空心按钮最明显——边框像从填充轮廓里"鼓"出来一块，外沿还越过控件矩形半个线宽。
- 根因是描边和填充的圆角不同心：`vgui_draw_button()` 的纯色分支把描边路径内缩 `bi = border_width/2`、圆角却仍用原 `rounding`。两条弧半径相同、圆心差 `bi`，于是描边外沿在 45° 角部探到填充轮廓之外（`bi=0.5` 时约 0.15px，`bw=3` 时约 0.44px），中间留下一条半像素缝；MSAA 8x 把这条缝解析成一圈暗边。
- 修复：两处按钮绘制统一成「外轮廓半径 R → 各层按内缩量递减」。
  - `vgui_draw_button()` SOLID 分支：先算 `r_out = floor(min(rounding, min(w,h)/2))`，填充直接用 `r_out`；描边仍内缩 `bi`，半径改成 `floor(r_out - bi)`，圆心回到填充圆心。
  - `vgui_draw_button_gradient()`：原来的 `pill_radius(该层尺寸, rounding)` 换成 `ink_radius(inset, box_h) = clamp(floor(r_outer - inset), 0, floor(box_h/2))`，底色、渐变填充、上沿高光、下沿阴影四层共用 `r_outer`。
  - 半径只能向下取整（OVG 内部还会 `round` 一次），宁可让描边内收半像素，也不能让外沿顶出控件边界。
- 回归：`vgui_screenshot_test` 新增常驻场景 `button_styles_dark`（360×170，6 个按钮：SOLID 实心 r=12/r=20、SOLID 空心 bw=1/bw=3、GRADIENT r=12/pill bw=3）。逐像素对比修复前后：角部原本「亮边 + 灰暗晕圈」的两层结构变成单层贴合的亮边，暗晕消失。
- 影响：所有带边框的纯色/渐变按钮圆角半径在描边层内收 0.5px，7 个截图基线（dock ×3、property grid ×2、asset browser ×2）一并更新，diff 均在 0.1%–0.36%、max_delta ≤ 30（仅边缘像素亮度变化）。

### 7.14 描边同心口径全库统一（2026-11-04）
- 背景：§7.13 只修了按钮。全库还有 22 处 `vgui_stroke_rect()` 和 12 处直接调 `ovg_tools::stroke_round_rect()` 的地方，一类是「路径内缩 `bw/2`、半径不变」（不同心），另一类是「路径不内缩、描边骑在轮廓上」（半个线宽溢到形状外）。两种写法在圆角处都和填充对不齐。
- 口径收拢到一处：`ovg_tools::stroke_round_rect_inset(cb, vg, x, y, w, h, r, color, line_w, inset)` 定义规则——**外轮廓半径 `R = round_rect_radius(w, h, r)`，路径按 `inset` 内缩，半径同步取 `round_rect_radius(w - 2·inset, h - 2·inset, R - inset)`**；`ovg_tools::stroke_round_rect_frame(...)` 是 `inset = line_w/2` 的简写，即「描边完全落在形状内部、外沿与传入矩形重合」。
- `gui/vgui.cpp`：`vgui_stroke_rect()` 删除，换成两个封装 `vgui_stroke_inset()` / `vgui_stroke_frame()`（保留 `color == 0` 早退），全部 22 个调用点改为走封装。控件外框统一用 `vgui_stroke_frame(控件矩形, w->rounding, ...)`，共 17 处：label / edit / image / model_frame / menubar / menu / splitter / tabs / dock / dock 浮动面板 / panel / custom / checkbox / tooltip / toast / button(SOLID)；焦点环用 `vgui_stroke_inset(..., fs.width, fs.inset)`；拖放提示框、drop compass、拖拽幽灵卡等临时可视元素也一并改掉。
- 编辑器侧 12 处同步：`chart_draw`（提示框）、`asset_browser`（卡片选中/悬停环）、`asset_preview`（预览框、色板）、`canvas_view`（图元边框）、`markdown_view`（代码块、媒体框、焦点环）、`node_graph_view`（节点边框、小地图）、`table_view`（颜色色板）、`timeline_view`（运输按钮）。这些点原本都是「填充与外框同矩形」，改成 frame 后描边不再向形状外溢出半个线宽。
- 半径变化：描边层半径由 `R` 变为 `floor(R - bw/2)`，`bw=1` 时减 0.5、`bw=3` 时减 1.5，向下取整后小圆角（r=3~6）会瘦 1px——这是保持同心的必要代价，圆角越小越要减，否则角部仍会错开。
- 回归：`sample/ovg_tools_test.cpp` 新增 frame 场景（外轮廓 70×31、r=8、bw=3），7 条像素断言：外侧一列必须是背景、描边内侧为纯白、中部为填充色、角部被切掉。对照实验（把调用换回骑线写法）会让其中 5 条失败，证明断言有效。检查数 105 → 112。
- 影响：`vgui_screenshot_test` 10/12 场景基线更新，diff 0.053%–1.536%、max_delta ≤ 184，全部集中在边框轮廓（面板外沿、卡片环、drop compass 圆角）；`button_styles_dark`、`viewport_gizmo_dark` 无变化。ctest 3/3、ovg_tools 112 / model / editor_kit / c_api / chart / runtime_embed / edit 全 PASS。
