# vgui 游戏编辑器组件路线图

> 状态：MVP 骨架开发中
>
> 适用版本：vgui 0.3.x
>
> 相关文档：[Phase 0：3D 场景编辑器基础](phase0_3d_editor.md)

## 1. 目标与边界

### 1.1 目标

把 vgui 从“轻量 GUI 库”扩展为“可构建游戏编辑器的两层组件体系”：

- 提供搭建场景编辑器 MVP 所需的编辑器外壳、属性编辑、场景树、资产浏览器和视口交互。
- 先补齐通用基础能力，再在其上构建编辑器专用组件，避免每个编辑器重复造轮子。
- 保持现有公共 C API 的稳定性，不把游戏场景、资产数据库或具体编辑器 schema 塞进核心。
- 用 `vgui_game_editor` 示例持续验证组件是否真正适合编辑器工作流。

`VGUI_IMAGE` 和 CPU/native texture 已经完成，本路线图不再重复列出。它们是资产缩略图和 3D 视口的基础能力。

### 1.2 非目标

- 不在 vgui 核心内实现游戏场景图、资产数据库、导入管线或序列化格式。
- 不在 vgui 核心内实现具体的 Gizmo 数学、拾取算法、命令语义；核心和 editor kit 只提供交互与绑定接口。
- 不把 Timeline、NodeGraph、CodeEditor 等大型专用编辑器塞进 `vgui.h`。
- 不承诺跨版本二进制兼容；新增控件枚举、结构字段仍按现有源码兼容策略处理。
- 文件解码、资源依赖分析、脚本编译等由应用层负责。

## 2. 现状与能力边界

### 2.1 已完成基础

- 基础控件：面板、标签、按钮、复选框、单选、开关、滑杆、进度条、编辑框、分隔线。
- 数据模型控件：列表、树、菜单，支持稳定 `vgui_item_id`、多选、键盘导航和滚动；列表/树可通过行源回调虚拟化。
- 编辑器外壳：`VGUI_DOCK` 支持左/右/下/中停靠区、标签组、浮动/再停靠、区域折叠与尺寸调整；`VGUI_SPLITTER` 支持水平/垂直嵌套与拖动比例，`VGUI_TABVIEW` 支持页切换/关闭/重排。
- 覆盖层：Tooltip、Modal、Alert/Confirm/Prompt、Toast，支持焦点陷阱、背景输入隔离和主题回退。
- 编辑器组件：`vgui_editor_kit` 已提供 Dock 外壳、命令/撤销栈、菜单与快捷键注册、布局持久化，以及类型化 PropertyGrid、Outliner、AssetBrowser 和 Viewport 边界组件。
- 布局：绝对定位、横向/纵向布局、自动换行、min/max、grow、margin、padding、gap、滚动面板。
- 输入：鼠标、键盘、文本输入、IME、剪贴板、焦点、悬停/按下/选中状态。
- 文本编辑：单行/多行、选择、复制/剪切/粘贴、编辑框内撤销/重做。
- 图形：CPU 纹理、SDL3 native texture、图片 source/fit/tint/flip/pixel snap。
- 扩展：`VGUI_CUSTOM` 可以用 OVG 自绘内容并处理事件。
- 主题：运行时主题切换、控件级背景/边框/圆角/文字颜色/透明度/字体大小。
- 国际化：UTF-8 文案表与运行时语言切换、RTL 布局/文字对齐镜像、多字体回退与 bidi 文本。

### 2.2 当前边界

| 领域 | 当前能力 | 对编辑器的影响 |
|---|---|---|
| 编辑器外壳 | 基础面板、菜单浮层/MenuBar、Splitter、TabView、Dock/浮动面板、Modal/Dialog/Toast；editor_shell 已基于 Dock，布局可保存/恢复 | 尚无工具栏/状态栏辅助组件，Dock 标签暂无图标与上下文菜单 |
| 属性编辑 | 类型化 PropertyGrid：文本/整数/浮点/布尔/枚举/颜色行、分类折叠、搜索、重置、只读、错误态、命令栈集成、选择绑定、行虚拟化 | 仍缺 Vector、资产/对象引用；颜色仅支持 Hex/预设/最近颜色，尚无 HSV 取色器和实时预览 |
| 列表/树 | 文本/详情、选择、展开、键盘导航、绘制裁剪、虚拟行源；行图标、文字颜色、行内按钮与自定义绘制（F04） | 行内重命名、拖放重排等编辑器语义仍在 O02/O03 |
| 菜单 | 上下文菜单、子菜单、复选、禁用、快捷键文本；MenuBar 与命令/快捷键注册 | 没有菜单图标、动态重建和原生菜单 |
| 文本 | 纯文本编辑，支持 IME 和编辑框内撤销 | 没有富文本、语法高亮、行号、查找替换 |
| 拖放 | 核心 C API：载荷/拖拽源/drop target/路由/拖影/插入指示/光标反馈；SDL3 OS 文件拖入已桥接（F03） | Outliner 重排的插入线精度、多选拖动与循环检测在 O03；资产引用行在 P06 |
| 撤销 | 编辑器级命令栈、事务（嵌套）/回滚、命令合并、失败命令协议；PropertyGrid 与 Outliner 移动/资产重命名已接入 | 连续拖拽聚合依赖 P07 的批量编辑事务；Gizmo/资产操作尚未接入 |
| 图片 | source/fit/tint/flip/pixel snap | 九宫格、旋转、任意矩阵只在 OVG 层暴露 |
| 视口 | Phase 0 单视口、LDR RGBA8 离屏纹理 | 没有相机导航、拾取、框选、Gizmo、多视口、HDR/MSAA/深度纹理 |
| DPI | SDL3 后端可获取 monitor 信息 | vgui 没有统一 UI 缩放 API |
| 现有 UI 编辑器 | `vgui_editor` 已能编辑 UI 文档，属性检查器已切换到通用 PropertyGrid | 层级面板仍直接使用 Tree；颜色/枚举等编辑器继续由 P02～P06 深化 |

## 3. 分层方案

```text
┌─────────────────────────────────────────────────────────────┐
│ host / application                                          │
│ 场景文档、资产数据库、导入/序列化、命令实现、Gizmo 数学     │
└──────────────────────────────┬──────────────────────────────┘
                               │ 使用
┌──────────────────────────────▼──────────────────────────────┐
│ vgui_editor_kit（建议新增）                                 │
│ PropertyGrid、Outliner、AssetBrowser、Dock/Tab、Viewport     │
│ Timeline、NodeGraph 等编辑器专用组件                        │
└──────────────────────────────┬──────────────────────────────┘
                               │ 依赖公开 API
┌──────────────────────────────▼──────────────────────────────┐
│ vgui_core（现有 libvgui）                                   │
│ 通用控件、布局、输入、文本、图片、拖放、虚拟数据模型、命令栈 │
└─────────────────────────────────────────────────────────────┘
```

约束：

- `vgui_core` 不依赖 `vgui_editor_kit`，也不认识游戏对象、资产 ID 或编辑器 schema。
- `vgui_editor_kit` 依赖 `vgui_core`，可以依赖 C++ 标准库，但对外仍以 C++ 接口为主。
- 应用层实现数据模型和命令语义，通过回调、属性描述或适配器接入 editor kit。
- 通用能力优先放 core；带“编辑器语义”的组件放 editor kit，避免核心无限膨胀。

建议落地位置：

| 路径 | 内容 |
|---|---|
| `gui/vgui.h`、`gui/vgui.cpp`、`gui/vgui_internal.h` | 核心控件、布局、输入、拖放、虚拟模型、命令基础设施 |
| `gui/editor/` | `vgui_editor_kit` 源码与独立 CMake 目标（已建立骨架） |
| `backends/vgui_sdl3.*` | DPI、OS 文件 drop、原生弹窗、视口输入桥接 |
| `sample/vgui_editor_kit_test.cpp` | editor kit 无窗口自测（已建立） |
| `sample/vgui_game_editor.cpp`（计划） | 游戏编辑器 dogfood 示例 |
| `sample/*_test.*` | 模型、C API、像素/截图回归测试 |
| `docs/` | 路线图、Phase 文档和 API 说明 |

## 4. 优先级与规模定义

### 4.1 优先级

- **P0**：编辑器 MVP 的阻塞项。没有它，场景树、属性、资产或视口工作流无法完整跑通。
- **P1**：完整编辑器需要。具备后体验接近商业编辑器，但不阻塞第一版。
- **P2**：打磨、规模化或可选能力。可以按项目实际需求延后。

### 4.2 规模

规模是相对复杂度，不是工期承诺：

- **S**：单个控件或小范围 API，通常只涉及少量文件。
- **M**：跨控件/布局/模型的行为，或需要新增公共 API。
- **L**：复杂交互、渲染、数据模型或架构性改造，建议单独里程碑跟踪。

### 4.3 状态

- `[ ]` 未开始
- `[x]` 已完成
- 进行中的工作建议在条目后追加 `（进行中）`，不要提前勾选。

## 5. 修改清单

### M0 基础设施（F）

目标：先补齐所有编辑器组件都会复用的基础能力，尤其是命令、拖放和虚拟数据模型。

- [x] **F01 建立 `vgui_editor_kit` 目标**（P0，S）
  - 新增独立的库目标和目录，不把编辑器组件混入 `vgui_core`。
  - 明确依赖方向：`editor kit -> core`，禁止反向依赖。
  - `vgui_core` 新增内容仍保持纯 C API。
  - 验收：空 editor kit 能独立构建并被示例链接；核心测试目标不受影响。
  - 已实现：`gui/editor/` + `vgui_editor_kit` 静态库 + `sample/vgui_editor_kit_test.cpp`。

- [x] **F02 命令/撤销栈**（P0，M）
  - `execute / undo / redo`、事务开始/提交/回滚、命令合并。
  - 命令状态可查询，供菜单、工具栏、快捷键更新 enabled/checked。
  - 属性修改、树重排、资产重命名等操作共享同一事务接口。
  - 验收：至少覆盖属性修改、树节点移动、资产重命名三类命令；嵌套事务和合并结果正确；失败命令不污染撤销栈。
  - 已实现：命令栈 `execute/undo/redo`（`execute` 返回 bool，失败命令不入栈、不清 redo）、事务 begin/commit/rollback（支持嵌套，内层组并入外层）、命令合并 `command::merge_with`（栈顶吸收连续命令，undo/redo/事务后合并链重置）；PropertyGrid 提交/重置走可合并的 `property_command`（同一行连续提交合并为一条，display-only 更新会断开合并链），`reset_all()` 包进事务；`outliner_view::move_node` 与 `asset_browser::rename_item` 经宿主回调接入命令栈，拒绝的移动/重命名不入栈。`vgui_editor_kit_test` 覆盖合并、合并链重置、嵌套事务、失败不入栈及三类命令的撤销/重做。

- [x] **F03 拖放框架**（P0，M）
  - 拖拽源、drop target、载荷类型（文本、文件、自定义 ID/指针）。
  - 拖动阈值、拖影、光标反馈、接受/拒绝状态、插入位置指示。
  - 支持跨面板拖放，并接入 SDL3 的 OS 文件 drop。
  - 验收：Outliner 内重排、资产拖到属性栏、资产拖到视口三条路径共用同一套 API。
  - 已实现：核心 C API `vgui_drag_payload`（text/files/custom，字符串由 begin_drag 深拷贝）、`vgui_begin_drag/cancel_drag/drag_active`、`vgui_widget_set_drop_target`（can_accept/hover/drop 回调 + 插入指示 hint）、`vgui_list/tree_set_row_drag` 行拖拽源（超过 `dock.drag_threshold` 发起）、`vgui_input_drop_files` 文件桥；拖拽期间悬停/tooltip/点击抑制，光标接受=Hand/拒绝=No，拖影复用 Dock 浮块样式，drop 路由命中测试自底向上找祖先 target。SDL3 后端重新启用 `SDL_EVENT_DROP_FILE`，聚合文件批次经 `ole_drop_e(fmt=1)` 桥接到 `vgui_input_drop_files`（DROP_TEXT 仍关闭）。editor kit `drag_bridge` 互转 kit/core 载荷并适配 drop_target；三条验收路径接线：`outliner_view::enable_drag_drop`（行拖出 scene_object、拖入重排走 `move_node` 命令栈可撤销）、`asset_browser::enable_row_drag`、`property_grid::set_asset_drop_handler`、`viewport_view::set_drop_listener`（记录 last_drop）。`vgui_model_test` 新增 17 项（现 615 项）覆盖阈值/路由/拒绝/文件桥；`vgui_editor_kit_test` 覆盖载荷往返、Outliner 拖放重排（可撤销）、资产拖到视口与属性栏；`vgui_demo` 新增"拖放"演示卡片（行拖拽 + drop zone + OS 文件拖入）。TabView 拖动阈值由硬编码 4.0 接入 style。多选拖放/循环父子检测归 O03，完整资产引用行归 P06，视口创建对象归 V06。

- [x] **F04 虚拟数据模型与自定义行**（P0，M）
  - 数据源回调：行数、按索引取行、稳定 ID、增量失效/刷新。
  - 列表/树/表格只请求可见行，不再要求一次性提交全量数据。
  - 行内图标、文字颜色、自定义绘制、局部命中区域。
  - 验收：大列表滚动时只查询可见范围；插入/删除/更新不会丢失选择；现有 list/tree 全量模型 API 保持兼容。
  - 已实现：列表/树行源回调、可见范围查询、稳定行 ID、选择/键盘/滚动虚拟化（此前完成）。本步补齐行扩展：`vgui_row_source` 新增 `row_icon`/`row_text_color`/`row_buttons`/`row_button_icon`/`row_button_click`/`row_draw` 回调；全量模型同步提供 `vgui_list/tree_set_item_icon/text_color/buttons/button_icon` 与 `on_item_button`（按钮命中不改选中、不触发展开）；行按钮几何自动避让可见滚动条命中区；`vgui_list/tree_source_rows_changed` 细粒度失效（不重建树的行投影）。`vgui_model_test` 新增 47 项（现 598 项）覆盖图标/颜色/按钮回调、命中、虚拟与全量模式及插入/删除保留选择；`vgui_c_api_test` 新增 9 项（现 20 项）。`row_draw` 自定义绘制仅虚拟模式提供（列表替换整行内容、树替换文本绘制，引导线/箭头/按钮仍由控件管理）。验收文本中的"表格"归 T01（Table/DataGrid 复用同一行源机制）。

- [x] **F05 编辑器布局持久化**（P0，S/M）
  - 保存/恢复 Splitter 尺寸、Tab 顺序、面板可见性和 Dock 状态。
  - 布局 JSON 与业务文档分离，避免打开场景时覆盖用户布局。
  - 验收：保存后重启恢复一致；布局损坏或版本不匹配时回退到默认布局。
  - 已实现：`editor_layout` 版本 2 JSON 保存 Dock 区域尺寸、每个区域的标签顺序、打开状态、活动页和浮动矩形，并保留面板可见性、底部 Tab 顺序与当前页；`editor_shell` 捕获/应用/保存/加载/重置布局。版本 1 文件仍可加载，其 Splitter 比例会映射到 Dock 区域尺寸；损坏或版本不支持时回退到 `build()` 后的默认布局。

### M1 编辑器外壳（S）

目标：形成“菜单栏 + 工具栏 + 可调整面板 + 标签页 + 弹层”的编辑器骨架。

依赖：F01、F02、F03；Dock 依赖 Splitter/Tab。

- [x] **S01 Splitter**（P0，S）
  - 水平/垂直、嵌套、最小尺寸、拖动改变比例、双击复位。
  - 拖动光标、悬停反馈、布局持久化。
  - 验收：三栏布局可拖动、不会把子面板压到最小尺寸以下；窗口 resize 后比例稳定。
  - 已实现：核心 `VGUI_SPLITTER` 控件、公共 API 和无窗口测试；`editor_shell` 已改用 Dock，Splitter 仍可用于其他宿主布局。布局持久化由 F05 覆盖。

- [x] **S02 TabView**（P0，M）
  - 多标签页、关闭按钮、未保存标记、禁用页。
  - 拖动重排、溢出滚动、右键菜单、键盘切换/关闭。
  - 验收：多文档打开/关闭/重排正确；关闭未保存页能触发确认；标签溢出可滚动。
  - 已实现：核心 `VGUI_TABVIEW`、关闭回调、未保存标记、禁用页、拖拽重排、滚轮溢出、键盘切换/关闭；右键通过带 item 的鼠标事件交给宿主菜单。

- [x] **S03 MenuBar + 命令/快捷键**（P0，M）
  - 顶层菜单、悬停切换、子菜单、分隔线、复选/禁用项。
  - 命令 ID、快捷键注册与分发、菜单/工具栏状态同步。
  - 验收：`Ctrl+S`、`Ctrl+Z` 等快捷键实际执行命令；菜单项 enabled 状态随选择变化；子菜单键盘可操作。
  - 已实现：核心 `VGUI_MENUBAR`（点击/悬停/方向键切换、弹层生命周期、样式与命中测试）和 context 级按键过滤器；editor kit `command_registry` 提供字符串命令 ID、快捷键解析/分发、enabled/checked 谓词，`editor_shell` 建立 File/Edit/View/Help 菜单并同步工具栏与菜单状态。子菜单键盘操作沿用核心 `VGUI_MENU`。

- [x] **S04 Tooltip**（P0，S）
  - 延迟显示、跟随/锚定定位、边界避让、多行文本。
  - 属性说明、工具栏按钮、树节点复用同一套 tooltip。
  - 验收：悬停延迟出现，移出消失；靠近屏幕边缘不越界；禁用控件可配置是否显示。
  - 已实现：context 级 tooltip 覆盖层，支持静态文本/动态 provider、逐控件延迟、指针跟随与控件锚定、靠近边缘自动翻转/钳制、多行换行；列表/树支持行级 tooltip；禁用控件默认保留并可关闭；菜单或模态打开、滚动、销毁 owner 时自动隐藏。

- [x] **S05 Modal / Confirm / Toast**（P0，M）
  - 窗口内模态遮罩、焦点陷阱、Esc 关闭、默认/危险按钮。
  - 确认框、输入框、错误提示、短暂通知。
  - 验收：模态打开时背景不可交互；Tab 不逃出模态；Esc/确认/取消行为一致。
  - 已实现：context 级模态 stage（遮罩、居中面板、焦点陷阱、背景鼠标/滚轮/键盘隔离、Esc/背景点击策略、关闭回调与焦点恢复）；`vgui_dialog_alert/confirm/prompt` 提供默认/危险按钮、输入框与结果回调；context 级 toast 支持六种位置、四种级别、自动过期/常驻、最大可见数和样式/主题回退。

- [x] **S06 Dock / 浮动面板**（P1，L）
  - 停靠区域、面板拖动、标签组、浮动窗口、关闭/恢复。
  - 与布局持久化、Tab、Splitter 协同。
  - 验收：面板可在左右/底部停靠、合并为标签组、浮出再停靠；重启后布局恢复。
  - 已实现：核心 `VGUI_DOCK` 提供左/右/底部/中央停靠区与浮动框架，区域即标签组并自动折叠；标签可重排、跨区移动、拖出浮出、拖到标签条再停靠；浮动框架可拖动/夹取/居中/提升，支持关闭/恢复与关闭按钮控制；区域尺寸、区域/标签/浮动矩形、活动页、打开状态和稳定字符串 id 均可查询，配合 `vgui_dock_panel_desc` 重建可恢复布局；支持标签条滚轮溢出、样式与主题回退。`editor_shell` 已切换到单个 Dock，`editor_layout` 版本 2 持久化区域尺寸、标签顺序、活动页、打开状态和浮动矩形。

- [x] **S07 工具栏辅助组件**（P1，S/M）
  - IconButton、ToggleButton、SegmentedControl。
  - Toolbar、StatusBar、SearchBox、Breadcrumb。
  - 验收：图标按钮有 hover/active/disabled 状态；StatusBar 可显示选中对象、坐标和错误信息。
  - 已实现：editor kit 新增 `icon_button`、`toggle_button`、`segmented_control`、`toolbar_view`、`status_bar`、`search_box` 和 `breadcrumb`；核心按钮 checked 状态绘制激活底色并触发重绘；工具栏可绑定 `command_registry` 同步标题/禁用/复选状态；状态栏支持消息、选中对象、坐标和错误；`editor_shell` 顶部工具栏（场景搜索、Undo/Redo、Grid、视图分段）和底部状态栏已接入，Properties 面板显示选择路径面包屑；headless 测试覆盖状态切换、命令刷新、搜索回调和面包屑导航。

### M2 属性编辑（P）

目标：把当前手工拼装的属性面板升级为可复用的 PropertyGrid 和类型化编辑器。

依赖：F02、F04、S04；P01 是 P08/P09 的前置。

- [x] **P01 PropertyGrid 骨架**（P0，M）（已完成）
  - 分类、折叠、搜索、行虚拟化、标签列 + 编辑列。
  - 单行重置、只读状态、错误提示、行级 tooltip。
  - 验收：能替换 `ui_editor` 中的手工属性面板；1000 行属性滚动流畅；分类和搜索状态可保持。
  - 已实现：分类/折叠/搜索、标签列 + 编辑列、单行重置、只读、错误提示、行级 tooltip；`clear()` 保留搜索、折叠和最近颜色。PropertyGrid 使用扁平显示列表 + 上下 spacer，只物化视口窗口（含 overscan）内的行，滚动/过滤/折叠时按需挂载与释放，`materialized_row_count()` 供无窗口断言；核心新增 `vgui_panel_set_scroll_listener()`，程序化滚动、滚轮与滚动条拖动都会驱动窗口更新。`vgui_editor` 的 UI 文档属性面板已切换到 PropertyGrid（ARGB ↔ RGBA 转换、descriptor setter 写回节点）。`vgui_editor_kit_test` 覆盖 1000 行虚拟化的有界物化、滚动到底部与滚回顶部，`vgui_editor --selftest` 覆盖属性行构建。

- [x] **P02 Number / Spinbox / DragNumber**（P0，M）（已完成）
  - int/float、范围、步进、拖拽调值、文本输入与校验。
  - 支持 Alt/Shift 精调等常用交互。
  - 验收：拖拽和键盘输入结果一致；越界被钳制或提示；修改进入命令栈。
  - 已实现：新增独立 `vgui_editor_kit::spinbox`（`integer`/`number`、范围、步进、可拖拽标签、加减按钮、Enter 文本校验、错误提示、只读/禁用、change/commit 回调）；拖动时按 Shift 精调、Alt 粗调，按下后跨行或移出控件仍持续更新，释放时只提交一次。PropertyGrid 数字行保持原有 edit + 标签拖拽行为，并补充跨行拖拽、移出控件、越界钳制和命令栈单次提交测试；Spinbox 独立 headless 测试覆盖整数/浮点范围、键盘 Enter/Esc/Up/Down、按钮步进、文本校验恢复和跨行拖拽捕获。

- [ ] **P03 ComboBox / Enum**（P0，S）（进行中）
  - 下拉列表、键盘选择、禁用项、空值/占位符。
  - 验收：属性枚举不再用一排按钮模拟；打开/关闭/选择不抢焦点；值改变可撤销。
  - 已实现：枚举行使用下拉菜单、显示标签、禁用项和命令栈撤销。待补充空值/占位符语义。

- [ ] **P04 Vector2 / Vector3 / Vector4**（P0，M）
  - 多字段联动、统一拖拽调值、可选颜色/坐标语义。
  - 验收：单个分量可独立编辑；拖拽时所有分量同步更新；与 Gizmo 数值双向绑定。

- [ ] **P05 ColorPicker**（P0，M）（进行中）
  - RGBA/HSV、Alpha、Hex 输入、色板、最近颜色。
  - 验收：颜色选择实时预览；取消可回滚；支持 sRGB/线性颜色转换策略由应用配置。
  - 已实现：PropertyGrid 颜色行支持 `#RRGGBB`/`#RRGGBBAA` Hex 输入、色块、预设和最近颜色，修改可撤销。待实现 HSV 取色器、Alpha 滑杆、实时预览/取消回滚和颜色空间策略。

- [ ] **P06 Asset / Object Reference**（P0，M）
  - 资源选择、对象选择、清空、拖放赋值、类型过滤。
  - 显示缩略图/类型图标/丢失资源状态。
  - 验收：AssetBrowser 拖入即可赋值；清空和撤销正确；丢失资源显示错误态。

- [ ] **P07 校验与命令集成**（P0，M）（进行中）
  - 属性描述结构：类型、get/set、范围、分类、tooltip、只读、默认值。
  - 错误显示、资源丢失提示、修改事务、撤销合并。
  - 验收：连续拖拽只产生合理数量的撤销记录；非法输入不写回模型；错误可定位到具体行。
  - 已实现：`property_desc`（类型、get/set、范围、分类、tooltip、只读、默认值、枚举选项）、行内错误、只读状态、提交/重置命令和撤销/重做、拖拽结束单次提交、重建后的陈旧命令隔离。待补充批量编辑事务/撤销合并、资源丢失提示和多选混合值。

- [ ] **P08 多选/混合值/复制粘贴/搜索**（P1，M）
  - 多对象编辑、混合值显示、批量赋值、属性复制粘贴。
  - 搜索过滤、常用属性/收藏。
  - 验收：多选不同值时显示混合态；批量修改可一次撤销；复制粘贴保持类型和引用。

- [ ] **P09 Curve / Gradient 编辑器**（P1，L）
  - 曲线、切线、插值模式；渐变条、色标、预设。
  - 验收：可作为 PropertyGrid 的自定义编辑器嵌入；修改实时预览并支持撤销。

### M3 场景树 / Outliner（O）

目标：把通用 Tree 升级为能承载场景层级的 Outliner。

依赖：F02、F03、F04；O01 是 O02/O03/O06 的前置。

- [ ] **O01 Tree 行扩展**（P0，M）
  - 行图标、文字颜色、自定义绘制、行内可见性/锁定按钮命中区域。
  - 验收：场景节点可显示类型图标和状态图标；点击可见性/锁定不会改变选中；自定义绘制不破坏滚动裁剪。

- [ ] **O02 行内重命名**（P0，S）
  - F2/双击进入编辑，失焦确认，Esc 取消，重名检测。
  - 验收：重命名进入命令栈；空名称/非法字符被拒绝；取消后恢复原名。

- [ ] **O03 拖放重排/改变父子关系**（P0，M）
  - 插入线、拖到节点上成为子节点、多选拖动、禁止循环父子关系。
  - 验收：拖放结果与视觉指示一致；操作可撤销；非法 drop 有拒绝反馈。

- [ ] **O04 搜索/过滤/选择同步**（P0，S/M）
  - 按名称/类型过滤，高亮匹配，保留展开状态。
  - 与 Viewport、PropertyGrid 双向选中同步。
  - 验收：搜索过滤后选择不丢失；Viewport 选中对象时 Outliner 自动展开并滚动到可见位置。

- [ ] **O05 虚拟 Outliner**（P0，M）
  - 大型场景按需展开/加载，稳定 ID，增量刷新。
  - 验收：万级节点只查询可见行；展开/折叠不重建整棵树；选择、滚动位置和展开状态保持。

- [ ] **O06 右键菜单与快捷键**（P0，S）
  - 新建、删除、复制、重命名、聚焦、显示/隐藏、锁定。
  - Delete/F2/Ctrl+D 等常用操作。
  - 验收：右键菜单作用于当前选中项；快捷键与菜单命令共用同一套命令实现。

### M4 资产浏览器与辅助面板（A）

目标：让资产查找、预览和拖入场景形成完整闭环。

依赖：F03、F04、S01/S02；A01/A04 依赖 Outliner/PropertyGrid 的 drop target。

- [ ] **A01 缩略图网格/列表**（P0，L）
  - Grid/List 两种视图，缩略图、名称、类型、选中态。
  - 虚拟滚动，支持大目录。
  - 验收：十万级资产目录只请求可见项；切换视图不丢失选择；键盘导航和框选可用。

- [ ] **A02 路径导航与搜索**（P0，S/M）
  - Breadcrumb、前进/后退、刷新、搜索、类型过滤。
  - 验收：路径跳转正确；返回后恢复滚动位置和选择；搜索有防抖和空状态。

- [ ] **A03 资产预览**（P0，M）
  - 选中资产预览面板，按类型扩展图片、模型、材质等预览。
  - 验收：预览与选中同步；大文件预览不阻塞主线程；加载失败有占位和错误提示。

- [ ] **A04 拖放资产到场景/属性栏**（P0，M）
  - 拖到 Viewport 创建对象，拖到 PropertyGrid 赋值，拖到 Outliner 建立父子关系。
  - 验收：三条路径都有明确落点反馈；drop 结果可撤销；OS 文件拖入等价于资产导入入口。

- [ ] **A05 异步缩略图加载与缓存**（P0，M）
  - 占位图、取消、失败重试、LRU/磁盘缓存策略由应用配置。
  - 验收：快速滚动不阻塞 UI；离开可见区域后取消低优先级请求；缓存命中不重复加载。

- [ ] **A06 资产操作**（P1，M）
  - 重命名、删除、复制、移动、导入、重新导入、依赖查看。
  - 验收：文件操作与资产数据库一致；冲突/只读/占用有错误提示；批量操作可撤销或明确不可撤销。

- [ ] **A07 Console / Log View**（P1，M）
  - 等级颜色、过滤、搜索、复制、自动滚动、增量追加、堆栈折叠。
  - 验收：大量日志追加不阻塞 UI；过滤和自动滚动可独立开关；错误可跳转到相关资产或脚本。

### M5 Viewport 与 Gizmo（V）

目标：从“显示 FBO 的图片”升级为可交互的场景视口。

依赖：F02、F03；V01/V02/V03 是 V05/V07 的前置；V08 是 T02/T03 的画布基础。

- [ ] **V01 Viewport 控件封装**（P0，M）
  - 绑定 native texture，处理 resize/FBO 重建。
  - 鼠标捕获、焦点、拖出控件边界时继续接收事件。
  - 验收：窗口缩放时视口不闪烁；拖拽 Gizmo 到视口外仍连续；失去焦点时正确结束操作。

- [ ] **V02 相机导航**（P0，M）
  - 中键平移、右键旋转、滚轮缩放、F 聚焦选中对象。
  - 验收：导航不与 Gizmo 操作冲突；聚焦后对象位于视口中心；相机状态可保存/恢复。

- [ ] **V03 拾取与选中**（P0，L）
  - 射线拾取、框选、选中描边/高亮。
  - 与 Outliner 选中同步。
  - 验收：点击/框选结果准确；多选和取消选择行为一致；选中高亮不遮挡 Gizmo。

- [ ] **V04 网格、坐标轴、吸附**（P0，M）
  - 世界网格、原点轴、平移/旋转/缩放吸附、网格尺寸切换。
  - 验收：吸附步长可配置；网格随相机缩放保持合理密度；关闭吸附后行为恢复自由移动。

- [ ] **V05 平移 Gizmo**（P0，L）
  - 三轴、平面手柄、局部/世界空间、多选中心。
  - 验收：轴/平面拖动方向正确；局部/世界切换生效；操作进入命令栈并可取消。

- [ ] **V06 Viewport 拖放目标**（P0，S/M）
  - 资产拖入创建对象，拖放时显示落点和预览。
  - 验收：拖放创建的对象类型正确；落点使用拾取位置；取消拖放不产生对象。

- [ ] **V07 旋转/缩放 Gizmo**（P1，L）
  - 旋转环、缩放轴、父空间/屏幕空间、数值输入联动。
  - 验收：旋转/缩放结果与数值面板一致；多选时围绕正确中心；支持吸附和撤销。

- [ ] **V08 2D Canvas**（P1，L）
  - 平移/缩放、框选、网格吸附、控制点、旋转手柄。
  - 2D 场景/UI 编辑器复用。
  - 验收：Canvas 可独立作为控件使用；选择/拖拽/缩放状态与命令栈集成。

- [ ] **V09 多视口 / HDR / MSAA / 深度纹理**（P1，L）
  - 多相机视口、线性空间和 HDR、MSAA resolve、深度纹理作为 UI 纹理。
  - 验收：多视口共享资源但不互相覆盖；HDR/MSAA 开关可运行时切换；深度纹理可用于后处理预览。

- [ ] **V10 `VGUI_IMAGE` 变换扩展**（P1，S/M）
  - 九宫格、旋转、任意矩阵。
  - 验收：与现有 source/fit/tint/flip 组合正确；裁剪和像素对齐不回归；C API 可调用。

### M6 专业编辑器与数据视图（T）

目标：补齐完整编辑器常见的表格、时间轴、节点图和代码编辑能力。

依赖：T01 依赖 F04；T02/T03 依赖 V08；T04 可独立排期。

- [ ] **T01 Table / DataGrid**（P1，M）
  - 列宽、表头、排序、虚拟行、单元格选择/编辑。
  - 验收：大数据表只渲染可见行；列宽和排序状态可保存；单元格编辑器复用 PropertyGrid 的类型化编辑器。

- [ ] **T02 Timeline / 动画编辑器**（P1，L）
  - 轨道、关键帧、时间尺、缩放/平移、框选、复制/粘贴、吸附、播放控制。
  - 验收：关键帧拖拽/复制/删除可撤销；缩放后时间精度正确；播放头与外部时钟同步。

- [ ] **T03 NodeGraph**（P1，L）
  - 节点、端口、连线、框选、缩放/平移、搜索、小地图、右键菜单。
  - 验收：节点拖动和连线命中准确；框选/多选/删除可撤销；大图缩放不丢失交互精度。

- [ ] **T04 CodeEditor / RichText**（P2，L）
  - 语法高亮、行号、查找替换、只读富文本日志。
  - 验收：可作为脚本编辑器或日志查看器嵌入；大文件打开不阻塞 UI；复制/选择行为与系统习惯一致。

### M7 工程化与打磨（E）

目标：让编辑器在 DPI、性能、可访问性和测试上达到可持续交付水平。

- [ ] **E01 DPI 缩放**（P1，M）
  - 统一 `ui_scale`，字体、间距、圆角、图标一起缩放。
  - 验收：100%/125%/150%/200% 缩放下布局不重叠；切换显示器后自动更新；截图基准可复现。

- [ ] **E02 性能与增量更新**（P1，M）
  - 属性行、树节点、资产缩略图的增量刷新和缓存。
  - 避免属性面板每次选中都清空重建。
  - 验收：选择切换、滚动、拖拽时无明显卡顿；性能基准纳入 CI 或固定测试场景。

- [ ] **E03 截图回归测试**（P1，M）
  - Dock、PropertyGrid、Outliner、AssetBrowser、Viewport 的关键状态截图对比。
  - 验收：主题/布局/控件状态变化有基线图；失败时输出差异图；CI 可运行。

- [ ] **E04 文档与示例**（P1，S）
  - 新增 `sample/vgui_game_editor.cpp`，持续 dogfood。
  - 更新 README、API 注释和本路线图状态。
  - 验收：新用户能按文档构建并运行编辑器示例；示例覆盖首批 MVP 主路径。

- [ ] **E05 无障碍**（P2，L）
  - 完整键盘导航、焦点陷阱、屏幕阅读器、高对比度主题。
  - 验收：核心编辑器流程可仅用键盘完成；焦点顺序可预测；高对比度主题满足基本可读性。

- [x] **E06 国际化 / RTL**（P2，M）
  - 文案表、RTL 布局、字体回退。
  - 验收：切换语言不破坏布局；RTL 下文本、图标和控件排列正确。
  - 已实现：`vgui_context_set_language/get_language/set_string/translate` 与 `vgui_widget_set_text_key/get_text_key` 提供 UTF-8 文案表（当前语言 → `en` → key 回退，空语言显示 key）；`vgui_context_set_direction` 切换 RTL 后 row/wrap 布局从右起排、column START/END 与文本对齐翻转，按钮（含图标侧）、复选框/单选、开关、滑杆、进度、编辑框（光标/选区/IME/滚动条）、列表、树（缩进/箭头）、菜单、菜单栏、标签页和滚动条同步镜像；底层 OVG 继续负责 Unicode bidi shaping 与多字体回退。`vgui_demo` 新增国际化/RTL 演示区块（中文/English/العربية 切换 + RTL 开关 + bidi 混排样例），`vgui_model_test` 新增 17 项 i18n/RTL 用例（现 551 项）。

## 6. 关键依赖关系

| 前置能力 | 依赖项 | 原因 |
|---|---|---|
| F01 editor kit 分层 | 所有 S/P/O/A/V/T 项 | 避免编辑器组件污染核心 C API |
| F02 命令栈 | S03、P07、O03、A06、V05、V07 | 菜单状态、属性修改、树/资产操作、Gizmo 都需要撤销 |
| F03 拖放框架 | O03、A04、V06 | 树重排、资产赋值、视口落点共用同一套拖放 |
| F04 虚拟数据模型 | O05、A01、T01 | 大场景、大资产目录、数据表必须按需取行 |
| F05 布局持久化 | S01、S02、S06 | Splitter/Tab/Dock 的状态需要保存和恢复 |
| S01 + S02 | S06 | Dock 需要 Splitter 和 Tab 的尺寸/标签能力 |
| P01 PropertyGrid | P08、P09 | 多选、混合值、自定义编辑器都挂在 PropertyGrid 上 |
| P02～P06 类型化编辑器 | P08、P09 | 多选批量编辑和曲线/渐变需要已有编辑器基础 |
| O01 Tree 行扩展 | O02、O03、O06 | 行内重命名、拖放指示、行内按钮都依赖自定义行 |
| V01 + V02 + V03 | V05、V07 | Gizmo 依赖视口输入、相机和拾取 |
| V08 2D Canvas | T02、T03 | Timeline 和 NodeGraph 共用画布交互基础 |
| V01 | V09 | 多视口建立在 Viewport 封装之上 |

## 7. 开发批次

### 7.1 第一批：编辑器 MVP

目标：能打开一个场景，看到层级、视口、属性和资产，并完成基础编辑与撤销。

范围：

- F01～F05：分层、命令、拖放、虚拟模型、布局持久化
- S01～S05：Splitter、Tab、MenuBar、Tooltip、Modal
- P01～P07：PropertyGrid、数字、Combo、Vector、Color、资源引用、校验/命令集成
- O01～O05：Outliner 行扩展、重命名、拖放、搜索、虚拟化
- A01～A05：资产浏览器、导航、预览、拖放、异步缩略图
- V01～V06：视口封装、相机、拾取、网格、平移 Gizmo、资产落点

首批不做，避免范围失控：

- Dock/浮动面板（已提前完成，见 S06）
- Table/DataGrid
- Console/Log
- 旋转/缩放 Gizmo
- 2D Canvas
- HDR/MSAA/深度纹理
- Timeline、NodeGraph、CodeEditor

MVP 验收场景：

1. 顶部菜单栏和工具栏可新建/保存/撤销/重做。
2. 左侧 Outliner 显示场景层级，支持搜索、重命名、显示/隐藏、拖放重排。
3. 中间 Viewport 显示 3D 场景，支持相机导航、点击拾取和平移 Gizmo。
4. 右侧 PropertyGrid 显示选中对象属性，修改可撤销。
5. 底部或侧边 AssetBrowser 显示缩略图，可拖入 Viewport、Outliner 和 PropertyGrid。
6. Splitter 可调整面板比例，Tab 可切换文档，布局可保存/恢复。
7. Tooltip 和确认框可正常使用，模态不会穿透背景。

### 7.2 第二批：完整编辑器

- S06 Dock / 浮动面板（核心与 editor_shell 接入均已完成）
- S07 工具栏辅助组件（已完成）
- P08 多选/混合值/复制粘贴/搜索
- P09 Curve/Gradient
- T01 Table/DataGrid
- A06 资产操作
- A07 Console/Log
- V07 旋转/缩放 Gizmo
- V08 2D Canvas
- V09 多视口/HDR/MSAA/深度纹理
- V10 `VGUI_IMAGE` 变换扩展
- E01 DPI 缩放
- E02 性能与增量更新
- E03 截图回归测试

### 7.3 第三批：专业编辑器与打磨

- T02 Timeline / 动画编辑器
- T03 NodeGraph
- T04 CodeEditor / RichText
- E05 无障碍
- E06 国际化 / RTL

第三批不应阻塞前两批；如果项目暂时不需要动画、节点图或脚本编辑，可以长期延后。

## 8. 完成定义（DoD）

每个条目完成时至少满足：

- **接口**：核心能力有公开 C API 或明确的 editor kit C++ API；新增枚举/结构字段遵守现有源码兼容约定。
- **交互**：鼠标、键盘、焦点、禁用、悬停、按下状态完整；需要文本输入的控件支持 IME 和剪贴板。
- **可撤销**：会修改文档状态的操作接入命令栈；取消/失败不留下半完成状态。
- **可测试**：模型/行为测试覆盖主路径和边界；核心 C API 有纯 C 冒烟测试；视觉组件有截图或像素测试。
- **可演示**：在 `vgui_demo` 或 `vgui_game_editor` 中有可操作入口。
- **可维护**：更新公共头文件注释、README/路线图状态；不把应用层逻辑反向塞进核心。

## 9. 风险与约束

- **范围风险**：Dock、Timeline、NodeGraph、CodeEditor 都是大型组件，不能和 MVP 混在一起做。
- **API 风险**：拖放、命令栈、虚拟数据模型会改变现有控件的扩展方式，应尽早定接口，避免后续重复迁移。
- **性能风险**：现有 list/tree 有绘制裁剪，但仍按全量 item 遍历；大场景/大资产目录必须走虚拟数据源。
- **兼容风险**：新增 `vgui_style` 字段会改变结构尺寸，使用方需按新头文件重新编译；不承诺跨版本二进制兼容。
- **架构风险**：不要让 `vgui_core` 依赖游戏对象或资产数据库；否则后续很难复用到其他编辑器/工具。
- **验证风险**：编辑器 UI 的回归很难只靠模型测试覆盖，需要尽早建立截图基线和示例场景。

## 10. 维护方式

- 每完成一项，把对应复选框改为 `[x]`，并在条目下补充实现位置和测试名称。
- 新增条目时沿用 F/S/P/O/A/V/T/E 前缀，并标注 P0/P1/P2 与 S/M/L。
- 调整优先级或批次时，同步更新“关键依赖关系”和“开发批次”，避免文档与实现脱节。
- 每个里程碑结束时，在 `vgui_game_editor` 示例中补一条可操作路径，确保组件不是“只在测试里存在”。

## 11. 当前骨架进度

已完成：

- `gui/editor/` 独立静态库目标 `vgui_editor_kit`，依赖 `libvgui`，不向核心反向渗透 (F01)。
- `command_stack`：execute / undo / redo、事务（支持嵌套）、提交、回滚、命令合并 `merge_with` 与失败命令协议 (F02)。
- `property_grid`：提交/重置命令可合并（同一行连续提交合一）、`reset_all()` 包事务；`outliner_view::move_node`、`asset_browser::rename_item` 接入命令栈 (F02)。
- `drag_bridge` + 核心拖放：载荷/拖拽源/drop target/路由/拖影/插入指示，行拖拽与 SDL3 文件拖入；Outliner 重排、资产拖到属性栏/视口三条路径共用同一套 API (F03)。
- 核心 list/tree 行扩展：`vgui_row_source` 的 icon/text color/inline buttons/自定义绘制回调与全量模型 setter，行按钮命中不改选中，细粒度失效 `source_rows_changed` (F04)。
- `command_registry`：字符串命令 ID、快捷键解析/分发、enabled/checked 谓词，菜单与工具栏共用同一命令状态 (S03)。
- `editor_context`：当前选择、选择监听、命令栈持有者。
- 核心 `VGUI_SPLITTER` 与 `VGUI_TABVIEW`：比例/最小尺寸/嵌套、拖动与双击复位；标签关闭/禁用/未保存/重排/溢出/键盘，均有公共 API 和无窗口测试 (S01/S02)。
- 核心 `VGUI_MENUBAR` 与 context 级按键过滤器：标题条、悬停/方向键切换、弹层生命周期，快捷键在焦点与菜单之后分发 (S03)。
- 核心 `VGUI_TOOLTIP`：静态/provider 文本、延迟、指针/控件锚定、边缘翻转/钳制、多行、列表/树行级提示、禁用控件策略与自动清理 (S04)。
- 核心 `VGUI_MODAL`、`vgui_dialog_alert/confirm/prompt` 与 Toast：遮罩/焦点陷阱/背景隔离、Esc/背景点击策略、结果回调与焦点恢复；Toast 六种位置、四级、过期/常驻/最大可见数 (S05)。
- 核心 `VGUI_DOCK`：四停靠区 + 浮动框架、标签组合并/重排/跨区/浮出/再停靠、关闭/恢复、区域尺寸与布局状态查询、标签条滚轮、样式/主题回退 (S06)。
- `editor_shell`：单个 `VGUI_DOCK` 承载 OUTLINER / VIEWPORT / PROPERTIES / PANELS 四个面板，底部 PANELS 内用 TabView 组织 ASSETS / CONSOLE；顶部 `toolbar_view` 包含场景搜索、Undo/Redo 命令按钮、Grid 开关和视图分段控件，底部 `status_bar` 同步 `set_status()` 与当前选择，Properties 面板顶部用 `breadcrumb` 显示选择路径；File/Edit/View/Help 菜单；Outliner、Viewport、PropertyGrid、AssetBrowser 组件边界保持不变。
- `editor_layout` 版本 2：保存 Dock 区域尺寸、标签顺序、活动页、打开状态、浮动矩形和底部 Tab；版本 1 文件兼容映射，损坏或版本不支持时回退默认布局 (F05)。
- `outliner_view`、`property_grid`、`asset_browser`、`viewport_view`：可构建的占位组件（属性面板为真实类型化实现并支持行虚拟化；行扩展与拖放已接入）。
- `sample/vgui_editor_kit_test.cpp`：无窗口自测通过，覆盖命令栈（合并/嵌套事务/失败协议）、选择同步、Dock 外壳（合并/浮出/关闭/区域尺寸）、菜单/快捷键、组件边界、行扩展、拖放、布局持久化、PropertyGrid 虚拟化窗口（物化有界/滚动释放/滚回恢复），以及 S07 工具栏/状态栏/搜索框/面包屑状态与回调。
- 国际化 / RTL：context 级文案表与语言切换、`vgui_direction` 方向切换（布局/文本/常用控件镜像）；`vgui_demo` 有可操作入口，`vgui_model_test` 覆盖切换与镜像布局 (E06)。
- 现有 `vgui_model_test`（619 项，含面板滚动监听通知）、`vgui_c_api_test`（20 项）、`vgui_texture_test` 基线保持通过；`vgui_editor --selftest` 覆盖 PropertyGrid 属性行构建、JSON 往返与代码生成。

下一步优先项：

1. PropertyGrid 的资产/对象引用行（P06）与颜色 HSV 取色器（P05），Vector 多字段行（P04）。
2. Outliner 行图标 / 可见性锁定按钮（O01，F04 行扩展已就绪）、行内重命名（O02）与拖放重排打磨（O03，F03 已就绪）。
3. 接入 AssetBrowser 缩略图、搜索和拖入场景的完整落点反馈（A01/A02/A04）。
4. 用 native texture 接入真实 Viewport，再补相机导航、拾取和平移 Gizmo（V01～V03）。
