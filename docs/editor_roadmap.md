# vgui 游戏编辑器组件路线图

> 状态：MVP、完整编辑器组件与工程化打磨（E01～E06）均已完成
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
- 编辑器组件：`vgui_editor_kit` 已提供 Dock 外壳、命令/撤销栈、菜单与快捷键注册、布局持久化，以及类型化 PropertyGrid（含 Vector2-4）、Outliner、AssetBrowser 和 Viewport 边界组件。
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
| 属性编辑 | 类型化 PropertyGrid：文本/整数/浮点/布尔/枚举/颜色/Vector2-4/资产/对象引用/曲线/渐变行、分类折叠、搜索、重置、只读、错误态、命令栈集成（同行合并 + begin/end_batch 批量事务）、选择绑定、行虚拟化；颜色行弹 HSV 取色器（实时预览/Esc 回滚/颜色空间策略钩子），曲线/渐变行显示紧凑预览并弹出完整编辑器，引用行支持拖放赋值/清空/丢失态 | 颜色空间转换仅提供挂钩，预设策略由应用配置 |
| 列表/树 | 文本/详情、选择、展开、键盘导航、绘制裁剪、虚拟行源；行图标、文字颜色、行内按钮与自定义绘制（F04）；全量树过滤 + 匹配子串高亮 + 行矩形查询 | 树虚拟模式的过滤由行源自理；行内重命名、拖放重排已由 editor kit 的 outliner_view 承载 |
| 菜单 | 上下文菜单、子菜单、复选、禁用、快捷键文本；MenuBar 与命令/快捷键注册 | 没有菜单图标、动态重建和原生菜单 |
| 文本 | 纯文本编辑，支持 IME 和编辑框内撤销 | 没有富文本、语法高亮、行号、查找替换 |
| 拖放 | 核心 C API：载荷/拖拽源/drop target/路由/拖影/插入指示/光标反馈；SDL3 OS 文件拖入已桥接（F03）；按在已选行且挂拖源时推迟改选（多选拖拽前提）；kit drop target 支持自定义 hover 提示；V06 视口悬停可轮询 `drop_hover`，Escape 取消拖拽 | 资产引用行在 P06；视口拖放落点/预览由宿主提供（V06 已在示例接入） |
| 撤销 | 编辑器级命令栈、事务（嵌套）/回滚、命令合并、失败命令协议；PropertyGrid 与 Outliner 移动/重命名、多选拖动事务、资产批量操作、V05 平移 Gizmo、V06 拖放创建、V07 旋转/缩放 Gizmo 已接入 | 连续拖拽聚合依赖 P07 的批量编辑事务 |
| 图片 | source/fit/tint/flip/pixel snap；V10 九宫格、旋转、自定义旋转中心与任意 2D 矩阵，原生/CPU 纹理共用同一变换语义 | 九宫格边宽按纹理像素；矩阵与旋转共享枢轴；不支持透视/3D 变换 |
| 视口 | Phase 0 单视口、LDR RGBA8 离屏纹理；V01 表面支持 FBO 尺寸通知、跨边界输入与失焦取消；V02 轨道相机（中键平移、右键旋转、滚轮缩放、F 取景、状态保存/恢复）；V03 左键点击/框选、多选集合与 Outliner 双向同步、屏幕空间选中高亮；V04 世界网格/原点轴生成、自适应密度与平移/旋转/缩放吸附；V05 世界/局部空间平移 Gizmo（三轴 + 平面手柄、多选中心、吸附、begin/preview/commit/cancel 命令栈接入）；V06 资产拖入落点求交、半透明幽灵预览/地面标记、多实例渲染与新建对象选中；V07 旋转环/缩放轴/统一缩放、世界/局部/父级/屏幕空间、数值面板联动与旋转/缩放吸附；V08 独立 2D Canvas；V09 多相机 view 共享管线、HDR（RGBA16F + tonemap）、MSAA resolve 与深度可视化纹理 | 深度纹理在 MSAA 开启时通过额外单采样深度通道生成，不支持直接解析 MSAA 深度；3D 描边、射线求交与网格 GPU 绘制由宿主提供 |
| DPI | 统一 `ui_scale` 贯通核心/editor kit/SDL3；逻辑 setter 与有效像素 getter 分离，字体/间距/圆角/图标同步缩放；显示器切换自动同步；布局持久化保存逻辑尺寸 | E03 已建立 9 个关键状态截图基线；自定义宿主控件需自行遵守逻辑单位约定 |
| 现有 UI 编辑器 | `vgui_editor` 已能编辑 UI 文档，属性检查器已切换到通用 PropertyGrid | 层级面板仍直接使用 Tree；颜色/枚举等编辑器继续由 P02～P06 深化 |
| shader要求使用slang来写 | 函数名不要分成多行写 |
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
| `gui/editor/` | `vgui_editor_kit` 源码与独立 CMake 目标（已完成，含完整编辑器组件） |
| `backends/vgui_sdl3.*` | DPI、OS 文件 drop、原生弹窗、视口输入桥接 |
| `sample/vgui_editor_kit_test.cpp` | editor kit 无窗口自测（已覆盖全组件） |
| `sample/vgui_game_editor.cpp` | 游戏编辑器 dogfood 示例（已实现，覆盖 MVP 主路径与专业编辑器页签） |
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

- [x] **P03 ComboBox / Enum**（P0，S）（已完成）
  - 下拉列表、键盘选择、禁用项、空值/占位符。
  - 验收：属性枚举不再用一排按钮模拟；打开/关闭/选择不抢焦点；值改变可撤销。
  - 已实现：新增独立 `vgui_editor_kit::combo_box`（标签/值/启用状态、禁用项、空标签占位、无选择状态、只读/禁用、菜单与关闭状态键盘导航、Alt+Down、change/commit 回调）；打开菜单时用核心新增的 `vgui_menu_set_cursor()` 把光标停在当前选择上，打开/关闭/选择不改变键盘焦点。PropertyGrid 枚举行保持下拉菜单编辑器，新增 `property_desc::allow_empty`：空值显示为 placeholder（缺省 `(None)`）并作为菜单第一项，禁用项仍不可选；点击标签会先把焦点移到枚举按钮再打开菜单。`vgui_editor_kit_test` 覆盖 ComboBox 与枚举空值、菜单光标/禁用跳过、焦点保持及命令栈撤销/重做，`vgui_model_test` 覆盖菜单光标 API。

- [x] **P04 Vector2 / Vector3 / Vector4**（P0，M）（已完成）
  - 多字段联动、统一拖拽调值、可选颜色/坐标语义。
  - 验收：单个分量可独立编辑；拖拽时所有分量同步更新；与 Gizmo 数值双向绑定。
  - 已实现：新增独立 `vgui_editor_kit::vector_editor`（2/3/4 分量，每个分量复用 `spinbox` 的窄轴标签 + 编辑框并默认隐藏加减按钮）；支持 `generic`/`position`/`color` 语义预设、自定义轴标签/颜色、范围/步进、只读/禁用、分量错误、change/commit 回调和 `set_value_source()` + `refresh()` 双向绑定。`spinbox` 新增 `set_label_width()` / `set_buttons_visible()` 供紧凑布局复用。PropertyGrid 新增 `vector2`/`vector3`/`vector4` 类型、`semantic` / `component_labels` / `component_colors` 描述字段和 `property_vector_editor()` 访问器；分量可独立输入或拖拽，拖动主标签按同一增量更新全部分量，拖动过程实时刷新分量控件，释放时只提交一条命令。`vgui_editor_kit_test` 覆盖向量文本解析、语义/自定义轴、分量编辑、统一拖拽、范围钳制、只读/禁用、错误恢复、值源刷新、撤销/重做、重置、Gizmo 式双向同步和过滤后编辑器重建。

- [x] **P05 ColorPicker**（P0，M）（已完成）
  - RGBA/HSV、Alpha、Hex 输入、色板、最近颜色。
  - 验收：颜色选择实时预览；取消可回滚；支持 sRGB/线性颜色转换策略由应用配置。
  - 已实现：新增核心 context 级通用弹出面板 `vgui_popup_open/close/active/content/on_close`（承载任意 detached 子树,外点/滚轮点外/Esc 关闭并区分原因,菜单/模态互斥,边缘钳制,焦点陷阱）与 editor kit `color_picker` 组件——SV 盒/色相条/Alpha 条（ovg tools.hpp 取色器原语,0xAABBGGRR↔0xAARRGGBB 换位）、new/current 预览、Hex 编辑（Enter 提交/Esc 恢复）、预设与最近色块行；change 每次手势实时预览、press/commit 协议一次手势一条可合并命令、Esc 回滚会话起点;`set_color_transform` 挂钩应用颜色空间策略。PropertyGrid 颜色行点色块弹 popup 取色器（取代原文本预设菜单）,原 Hex 输入/预设/最近色保留。测试:kit `test_color_picker`（hex/alpha 往返、策略转换、SV 拖拽 commit、popup 集成预览/合并/Esc 回滚）;核心 popup 12 项进 `vgui_model_test`（现 656 项）。

- [x] **P06 Asset / Object Reference**（P0，M）（已完成）
  - 资源选择、对象选择、清空、拖放赋值、类型过滤。
  - 显示缩略图/类型图标/丢失资源状态。
  - 验收：AssetBrowser 拖入即可赋值；清空和撤销正确；丢失资源显示错误态。
  - 已实现：`property_type` 新增 `asset`/`object_ref`（值为引用 id 或空,空显示 `(None)`）,`property_desc::asset_filter` 宿主类型过滤;行 UI = 缩略图槽（类型色块/丢失红块）+ 名称 + 清空(x) + 浏览(...) 按钮;宿主松耦合接口 `set_asset_query`（解析名称/类型/缩略图/丢失）`set_asset_browse_handler` `set_object_activated_handler`（点名跳转）`set_asset_filter_check`（drop 过滤）;行级 drop target 挂缩略图（按行类型收 asset/scene_object 载荷,赋值走命令栈可撤销）;丢失态复用行内错误（红名/tooltip）并触发 `set_missing_asset_handler`。kit `test_property_reference` 覆盖解析/丢失切换、拖放赋值/undo、filter 拒绝、清空+undo、browse、对象激活与拖放。

- [x] **P07 校验与命令集成**（P0，M）（已完成）
  - 属性描述结构：类型、get/set、范围、分类、tooltip、只读、默认值。
  - 错误显示、资源丢失提示、修改事务、撤销合并。
  - 验收：连续拖拽只产生合理数量的撤销记录；非法输入不写回模型；错误可定位到具体行。
  - 已实现：`property_desc`（类型/get/set/范围/分类/tooltip/只读/默认值/枚举/向量语义/asset_filter）、行内错误、只读、提交/重置命令和撤销/重做、拖拽结束单次提交、重建后的陈旧命令隔离（P01 阶段已有）;本步补 `property_grid::begin_batch/end_batch`（嵌套安全,跨属性多次提交并入一条 undo;`reset_all()` 改走 batch）与资源丢失提示（P06 `set_missing_asset_handler` + 行内错误态）。连续拖拽聚合 = 同行 merge 链（P05 取色器会话整段合成一条 undo）。kit `test_property_reference` 尾段覆盖批量一条 undo、嵌套并入、不平衡/无栈拒绝、reset_all 单记录。

- [x] **P08 多选/混合值/复制粘贴/搜索**（P1，M）（已完成）
  - 多对象编辑、混合值显示、批量赋值、属性复制粘贴。
  - 搜索过滤、常用属性/收藏。
  - 验收：多选不同值时显示混合态；批量修改可一次撤销；复制粘贴保持类型和引用。
  - 已实现：`property_grid::set_multi_targets/getter/setter`（多对象编辑,不改 editor_context——选择集经 `outliner_view::selection_objects()` 导出,grid 在刷新时逐 target 拉值自动检测混合态）；混合显示按类型(edit 占位 "(mixed)"、枚举按钮文本、颜色/引用中性灰块、bool/vector 主选值+行 tooltip),`property_mixed(id)` 可查；提交走 `begin_batch` + per-target `multi_command`（先应用后 push,一条 undo;undo/redo 经 getter 重刷行,各对象回旧值时自动回到混合态）；空 targets 完整退回单选 desc.set_value 路径。属性剪贴板：kit 级进程内 `copy_properties/paste_properties/clipboard_property_count`（同 id 且同类型才粘、引用按 id 原样、空批不产生 undo 记录）,shell Edit 菜单 "Copy/Paste Properties"（Ctrl+Shift+C/V,enabled 谓词接 command_registry）。收藏：`set_property_favorite/favorite_ids/set_favorites_only`（跨 clear 重建保持,按属性 id 存储）,行内星标按钮 + 搜索旁 "★" 过滤开关,与搜索/折叠在 `row_matches` 叠加。`vgui_game_editor` dogfood:Outliner Ctrl 多选 → 属性面板混合态 → 批量改 tint/网格一次撤销。测试:kit `test_property_multi/clipboard/favorites`（混合检测与恢复、per-target undo、单选退化、类型保持、缺行跳过、一条 undo、收藏过滤叠加与重建保持）。

- [x] **P09 Curve / Gradient 编辑器**（P1，L）（已完成）
  - 曲线、切线、插值模式；渐变条、色标、预设。
  - 验收：可作为 PropertyGrid 的自定义编辑器嵌入；修改实时预览并支持撤销。
  - 已实现：`curve_editor` 归一化 t/value 曲线模型，linear/step/cubic 插值、cubic 入/出切线手柄、双击加点、拖动关键帧/切线、Add/Del 与 T/V/In/Out spinbox，行预览 `draw_preview`；`gradient_editor` 渐变条 + 可拖动色标、双击插点（按相邻色标插值取色）、选中点时间/颜色编辑（内嵌 `color_picker`）、4 个预设（一键一次手势）、行预览。两者共用 canonical 文本（`cubic;0,0,0,0;0.5,1,0,0;1,0,0,0` / `0,#FF0000FF;0.5,#00FF00FF;1,#0000FFFF`）并暴露 press/change/commit 监听。PropertyGrid 新增 `property_type::curve/gradient`：自定义绘制预览、点击/标签打开 popup，统一 popup 会话做实时预览（不入栈）、手势结束 `commit_value()` 单条可合并 undo、Esc 回滚显示，多目标提交沿 P08 per-target 事务路径。`vgui_game_editor` 增加 Idle Curve / Color Ramp 行（值落到对象 props）。测试：kit `test_curve_editor`（格式/规范化/模式按钮/双击加点/关键帧与切线拖拽/只读）、`test_gradient_editor`（短写/alpha/0x 归一/取色/预设/渐变条拖放）、`test_property_grid_curve_gradient`（预览→popup、实时预览、合并 undo、Esc、显式关闭保留、多目标事务 undo/redo）。

### M3 场景树 / Outliner（O）

目标：把通用 Tree 升级为能承载场景层级的 Outliner。

依赖：F02、F03、F04；O01 是 O02/O03/O06 的前置。

- [x] **O01 Tree 行扩展**（P0，M）（已完成）
  - 行图标、文字颜色、自定义绘制、行内可见性/锁定按钮命中区域。
  - 验收：场景节点可显示类型图标和状态图标；点击可见性/锁定不会改变选中；自定义绘制不破坏滚动裁剪。
  - 已实现：F04 核心行扩展（icon/text_color/buttons/row_draw + 命中不改选中 + 滚动条避让）已完成；`outliner_view` 封装场景语义——`set_node_icon/set_node_text_color` 透传，`enable_row_buttons()` 每行挂可见性（按钮 0）/锁定（按钮 1）按钮，`set_object_visibility/locked` 维护状态与图标，`set_visibility_listener/set_lock_listener` 通知宿主，`set_row_button_icons` 支持四态图标（无图标仍可命中）。`vgui_editor_kit_test::test_outliner_row_extensions` 断言按钮点击不改选中/不折叠、正文区仍可选中。

- [x] **O02 行内重命名**（P0，S）（已完成）
  - F2/双击进入编辑，失焦确认，Esc 取消，重名检测。
  - 验收：重命名进入命令栈；空名称/非法字符被拒绝；取消后恢复原名。
  - 已实现：核心新增 `VGUI_EVENT_CANCEL`（单行编辑 Esc 先广播再失焦）与 `vgui_context_set_focus_listener`（失焦确认的通路）、`VGUI_KEY_F1..F12` 键码及 SDL 映射；`outliner_view::enable_inline_rename()`（F2 树持焦点时、双击时进入）+ `begin_rename()` 在 `vgui_tree_get_item_rect` 行矩形上叠 VGUI_EDIT，Enter=SUBMIT 提交、Esc=CANCEL 放弃、失焦提交；`rename_node()` 走 `rename_handler`（宿主重名/非法字符校验）+ 命令栈 "Rename node" 命令，空名/逗号拒绝不入栈。`test_outliner_inline_rename` 覆盖程序化改名、三种拒绝、Enter/Esc/失焦/F2/双击全路径。

- [x] **O03 拖放重排/改变父子关系**（P0，M）（已完成）
  - 插入线、拖到节点上成为子节点、多选拖动、禁止循环父子关系。
  - 验收：拖放结果与视觉指示一致；操作可撤销；非法 drop 有拒绝反馈。
  - 已实现：`outliner_view::drop_location_at()` 解析上 25%=before、下 25%=after、中 50%=onto（成为末子节点）、空白=roots；`emit_hover_hint()` 对应 LINE/RECT 提示，非法位置 NONE（拒绝反馈）；kit `drop_target` 增加可选 `hover()` 虚函数（drag_bridge 转发核心 hover 回调，默认实现保持横向插入线）；循环父子检测移入 kit（`would_cycle` 沿宿主 parent 链），多选拖动 payload 用逗号拼接 id 列表、drop 按视觉序在单个事务内应用（索引随移除修正，单次 undo）。核心配套：按在已选行且有拖源时推迟到抬起再改选择（`defer_click`），避免按下把多选折叠成单选。`test_outliner_drag_semantics` 覆盖位置解析、提示类型、onto 拖放可撤销、循环拒绝、多选单记录撤销。

- [x] **O04 搜索/过滤/选择同步**（P0，S/M）（已完成）
  - 按名称/类型过滤，高亮匹配，保留展开状态。
  - 与 Viewport、PropertyGrid 双向选中同步。
  - 验收：搜索过滤后选择不丢失；Viewport 选中对象时 Outliner 自动展开并滚动到可见位置。
  - 已实现：核心 `vgui_tree_set_filter`（全量树行投影改为命中+祖先闭包，忽略折叠但保留展开状态，清除后恢复）与 `vgui_tree_set_match_text/set_match_color`（首个大小写不敏感子串用 match 色重绘）；`outliner_view::set_filter`（名称子串）+ `set_filter_match_handler`（宿主类型过滤叠加）；`enable_selection_sync()` 注册 context 选择监听，外来选择 `expand_to`+select+`scroll_to_node(NEAREST)`，自身点击经 `syncing_selection_` 重入保护。`editor_shell` 场景搜索框已接 `set_filter`。`test_outliner_filter_sync` 覆盖命中+祖先、选择/展开保留、清除恢复、handler 叠加、同步展开选中。

- [x] **O05 虚拟 Outliner**（P0，M）（已完成）
  - 大型场景按需展开/加载，稳定 ID，增量刷新。
  - 验收：万级节点只查询可见行；展开/折叠不重建整棵树；选择、滚动位置和展开状态保持。
  - 已实现：`outliner_view::set_scene_source()` 宿主适配器（root_count/root_at/child_count/child_at/name 必需，parent_of/icon/enabled/tooltip/text_color/row_draw 可选）切换 `vgui_tree_set_row_source` 虚拟模式；kit 持有展开状态（`row_toggle` 翻转 + `source_changed`），row_id=object_id 稳定；折叠时只投影可见行（适配器仅对投影内节点查询 name/child_count）；`scene_changed()` 结构级失效、`scene_row_changed()` 单行 `source_rows_changed`；O01~O04 在虚拟模式同样生效（按钮/图标/重命名/拖放/过滤经适配器走闭包投影）。`test_virtual_outliner` 用 100×100=万级场景断言折叠查询有界、展开投影 200、选择跨展开/折叠/结构刷新保持、虚拟过滤投影与清除恢复、虚拟重命名/拖放可撤销。

- [x] **O06 右键菜单与快捷键**（P0，S）（已完成）
  - 新建、删除、复制、重命名、聚焦、显示/隐藏、锁定。
  - Delete/F2/Ctrl+D 等常用操作。
  - 验收：右键菜单作用于当前选中项；快捷键与菜单命令共用同一套命令实现。
  - 已实现：`outliner_view::enable_context_menu()` 树右键（button 3）先独占选中再弹出菜单（New Child / Rename F2 / Duplicate Ctrl+D / Delete Del / Focus / Visible / Locked 复选）；`set_action_handler()` 承接 new_child/duplicate/remove/focus（宿主实现、可包命令），rename 与两个开关走 kit 内部实现；Delete/F2/Ctrl+D 经 context key handler（树持焦点时）汇入同一 `dispatch_action`。`test_outliner_context_menu` 覆盖右键选中、菜单激活分发、三个快捷键共用实现、Visible 复选态与切换。

### M4 资产浏览器与辅助面板（A）

目标：让资产查找、预览和拖入场景形成完整闭环。

依赖：F03、F04、S01/S02；A01/A04 依赖 Outliner/PropertyGrid 的 drop target。

- [x] **A01 缩略图网格/列表**（P0，L）（已完成）
  - Grid/List 两种视图，缩略图、名称、类型、选中态。
  - 虚拟滚动，支持大目录。
  - 验收：十万级资产目录只请求可见项；切换视图不丢失选择；键盘导航和框选可用。
  - 已实现：`asset_browser` 自动构建 Grid/List 分段开关（`set_view()`/`set_view_switch_visible()`），列表复用核心行模型，网格在一个滚动面板内只绘制可见单元格；`asset_source` 提供 `count`/`item_at`/`id_at`/`index_of` 懒加载接口，默认 id 为 `index + 1`，十万级目录滚动到中段只查询可见范围；选择集由浏览器统一持有并在两种视图间同步，支持 Ctrl/Shift 多选、方向键/Home/End/PageUp/PageDown、空格反选与 Ctrl+A；网格框选只命中选择可见单元格，单元格拖拽复用 `begin_editor_drag()` 的 asset payload；`set_item_thumbnail()`/`item_thumbnail()` 管理借用纹理，列表行图标与网格缩略图共用。核心新增 `vgui_custom_draw_text()`/`vgui_custom_draw_texture()`，并让列表行纹理同时支持 CPU 纹理（`add_image`）与原生纹理（`add_texture`）。`test_asset_browser_views` 覆盖视图切换选择保持、缩略图/类型元数据、键盘导航、框选、网格拖拽、十万级虚拟源可见范围与懒查询、虚拟缩略图及 detach。

- [x] **A02 路径导航与搜索**（P0，S/M）（已完成）
  - Breadcrumb、前进/后退、刷新、搜索、类型过滤。
  - 验收：路径跳转正确；返回后恢复滚动位置和选择；搜索有防抖和空状态。
  - 已实现：`set_navigate_handler(path_id, asset_directory*)` 由宿主按 path id 返回 `{path, items/source, empty_text}`；`asset_browser` 构建返回/前进/刷新按钮与 `breadcrumb`，历史快照记录该目录的选择、主选、光标、锚点和列表/网格滚动，`go_back()`/`go_forward()` 重新解析目录后恢复，`reload()` 只刷新当前目录不改变历史；`set_activate_listener()` 承接列表 Enter/双击与网格 Enter/双击（示例用于进入文件夹）。搜索框输入只更新待处理查询，2x2 ticker 的绘制回调（或无窗口测试的 `tick()`）推进防抖，提交/立即设置则同步生效；过滤通过视图索引映射到原始模型，`item_count()/item_at()/id_at()/index_of()` 均作用于过滤结果，`total_item_count()` 保留原目录计数。类型下拉由 `set_type_options()`/`set_type_filter()` 驱动并与搜索叠加；全隐藏时使用 `No matches`，空目录使用宿主 `empty_text`，网格与列表共用空状态。`test_asset_browser_navigation` 覆盖目录提供者、拒绝跳转、面包屑/前进后退、列表与网格滚动及选择恢复、reload、搜索防抖/大小写不敏感、类型过滤叠加、空目录文案与激活回调。

- [x] **A03 资产预览**（P0，M）（已完成）
  - 选中资产预览面板，按类型扩展图片、模型、材质等预览。
  - 验收：预览与选中同步；大文件预览不阻塞主线程；加载失败有占位和错误提示。
  - 已实现：新增独立 `asset_preview` 组件。`set_load_handler()` 接收只在后台 worker 上运行的 CPU 加载器（禁止调用 vgui），每次请求携带 generation token；快速换选时旧结果被 `poll()`/`tick()` 丢弃，主线程只应用当前代结果并创建/销毁纹理。`data` 支持 image（RGBA/BGRA CPU 像素、stride）、model、material（详情/色板）、text、audio（波形）与 custom（纹理/文本回退）；stage/info 用 `VGUI_CUSTOM` + ovg_tools 自绘，包含加载动画、类型化预览、错误占位和错误文本，`set_texts()` 可定制空/加载/失败文案。`editor_shell` 在 PROPERTIES 面板底部嵌入预览，经 `asset_browser::add_selection_listener()` 自动跟随选中项；新增 `asset_browser::item_info()` 让被搜索/类型过滤隐藏的选中项仍能解析名称/类型，原 `set_selection_listener()` 保持兼容。示例新增 Materials 目录、Steel.mat、Broken.mesh 和后台 CPU 图像/模型/材质加载器。`test_asset_preview` 覆盖加载非阻塞、陈旧结果丢弃、图像纹理、失败占位与错误、无加载器状态、多监听器增删、过滤后 item_info，以及 shell 的选择同步与布局。

- [x] **A04 拖放资产到场景/属性栏**（P0，M）（已完成）
  - 拖到 Viewport 创建对象，拖到 PropertyGrid 赋值，拖到 Outliner 建立父子关系。
  - 验收：三条路径都有明确落点反馈；drop 结果可撤销；OS 文件拖入等价于资产导入入口。
  - 已实现：在 F03 的 `drag_payload`/`drop_target`/`drag_bridge` 上扩展三条资产路径与文件导入，不引入新依赖。核心新增 `vgui_drag_hint(ctx, hint)`，把当前悬停目标的实时提示（绝对坐标）提供给宿主/测试，拖拽结束或目标拒绝时返回 0 并清空输出。`viewport_view` 的 drop 监听器改为返回 `bool`（宿主拒绝时不覆盖上次结果），记录 `last_drop_x()/last_drop_y()`，并在悬停时绘制钳制在视口内的 80x48 RECT 落点框。`outliner_view` 的树 target 同时接受 `asset` 与 `scene_object`；新增 `set_asset_drop_handler(asset, parent, index)` 与 `apply_asset_drop()`，复用既有插入线/行高亮语义，宿主可据落点建立父子/兄弟关系并拒绝非法位置。`property_grid` 的引用行 target 锚定整行面板（缩略图、名称和按钮都能落），悬停高亮整行；`asset_drop_handler`/行级 `commit_reference()` 返回 `bool`，drop 经 `commit_value()` 进入命令栈；行控件虚拟化释放时清除 `drop_installed`，重新物化后重装 target。`asset_browser` 根控件新增 `VGUI_DRAG_FILES` target，`set_import_handler(files, path_id)` 与 `import_files(files)` 共用导入入口，OS 文件拖入与工具栏导入等价；整批拒绝时不导入任何文件。示例把三条路径接到 `editor_state`：Viewport/Outliner 落点新建对象并记录可撤销的 "Create …" 命令（undo 删除、redo 按稳定 id 和原位置重建），PropertyGrid 的 mesh 赋值写回 `scene_object::props` 并经 `set_value` 参与 undo/redo，OS 文件导入整批一条可撤销命令；资产引用改为动态索引，演示目录、查询、浏览菜单和缩略图共用同一数据源。`test_asset_drop_paths` 覆盖视口 RECT 跟随光标/拒绝保持上次结果、Outliner 行中部父子与空白根落点/插入线提示、属性行整行高亮与命令撤销重做、导入入口及 `vgui_input_drop_files()` 等价路径/整批拒绝；`vgui_model_test` 新增 `vgui_drag_hint` 的实时查询与落点清空断言（现 659 项）；`editor_shell` 默认状态反馈同步改为 bool 处理器，四个 Dock 面板布局契约保持不变。

- [x] **A05 异步缩略图加载与缓存**（P0，M）（已完成）
  - 占位图、取消、失败重试、LRU/磁盘缓存策略由应用配置。
  - 验收：快速滚动不阻塞 UI；离开可见区域后取消低优先级请求；缓存命中不重复加载。
  - 已实现：新增独立 `thumbnail_cache`（`gui/editor/thumbnail_cache.*`）：`set_load_handler()` 在后台 worker 执行 CPU 解码，`set_disk_cache(read, write)` 的读写回调也在 worker；主线程只由 `poll()`/`tick()` 上传纹理。`options` 配置 `memory_limit`（LRU 纹理数）、`max_attempts`/`retry_delay` 与 `disk_cache` 开关；稳定字符串 key 对应请求状态 `queued/loading/ready/failed`，`enqueue(..., priority)` 分高/低队列，`cancel()` 移除排队任务并用 generation token 丢弃在途结果，`retry()` 从第一次尝试重启失败项，`erase()` 同时释放纹理；`stats()` 暴露内存/磁盘命中、加载、上传、失败、重试与取消计数，`idle()` 供宿主/测试等待。`asset_browser` 在 `tick()` 中只对 `visible_first()/visible_last()` 范围发起请求，滚动离开后取消未完成项，网格显示 queued/loading 动画或 failed 占位并在工具提示中给出原因，宿主 `set_item_thumbnail()` 仍优先于异步结果；`thumbnails()`、`thumbnail_key()`、`thumbnail_state()`、`retry_thumbnail()` 构成宿主接口。示例把 loader 与临时目录 raw 磁盘缓存接到资产浏览器，`Broken.mesh` 前两次失败后自动重试成功；`test_asset_thumbnails` 覆盖非阻塞入队、优先级顺序、低优先级取消、陈旧结果丢弃、自动/手动重试、内存命中不重载、LRU 驱逐后的磁盘回源与 `disk_cache=false` 绕过、宿主缩略图优先、浏览器可见范围请求及离屏取消。

- [x] **A06 资产操作**（P1，M）（已完成）
  - 重命名、删除、复制、移动、导入、重新导入、依赖查看。
  - 验收：文件操作与资产数据库一致；冲突/只读/占用有错误提示；批量操作可撤销或明确不可撤销。
  - 已实现：`asset_browser` 新增宿主驱动的统一操作层——`set_operation_handler()` 接收结构化 `operation_request`（`kind`、`items`、`name`、`path_id`、`target_path_id`），宿主负责原子检查并应用整批改动，返回 `operation_result`；固定 `operation_error` 覆盖 `conflict`/`read_only`/`in_use`/`invalid_target`/`not_found`/`io_error` 等，浏览器合成 `last_message()`、`last_error()`、`last_undoable()`，并同时通过 `notice_listener`、状态栏和错误 Toast 反馈。成功时刷新当前目录并恢复/更新选择，未装导航处理器时本地镜像同步改名/删除/移动并清除重导入缩略图；旧 `rename_handler` + `rename_command` 仍作为兼容回退。整批操作只有在宿主设置 `undoable=true` 并同时给出 `undo`/`redo` 时才经 `operation_command` 记录为一条命令，否则消息明确追加 `(not undoable)` 并显示警告 Toast；撤销/重做回调统一刷新浏览器。`enable_context_menu()` 在列表行和网格单元上安装右键菜单，并与 F2 重命名、Delete 删除、Ctrl+D 复制、F5 重新导入、Ctrl+I 导入快捷键共用同一 dispatch；`Move To` 子菜单由 `set_move_targets_handler()` 填充且自动排除当前目录，依赖查看由 `set_dependency_handler()` 提供并以模态 Alert 展示，Import... 通过 `set_import_prompt_handler()` 打开宿主动态文件选择后仍调用 A04 `import_files()`。重命名/删除确认使用核心 Prompt/Confirm 对话框，空名称、外部空目标、同目录移动和未安装处理器都有前端校验。示例 `vgui_game_editor` 接入了命令栈快照、只读 `Ghost.mesh`、被相机引用的 `Steel.mat`、重名/移动冲突、`Broken.mesh` 重导入 I/O 失败、不可撤销重导入、依赖/移动目标与导入提示框；`test_asset_operations` 覆盖成功/失败操作、批次原子拒绝、批量单条撤销/重做、不可撤销标记与 Toast、列表/网格右键、Move To 子菜单、快捷键与焦点门控、重命名/删除对话框、导入通知和依赖模态框。

- [x] **A07 Console / Log View**（P1，M）（已完成）
  - 等级颜色、过滤、搜索、复制、自动滚动、增量追加、堆栈折叠。
  - 验收：大量日志追加不阻塞 UI；过滤和自动滚动可独立开关；错误可跳转到相关资产或脚本。
  - 已实现：新增独立 `console_view`（`gui/editor/console_view.*`）。生产者可在任意线程调用 `append(level, message, source)` / `append(entry)` / `append_batch()`，记录进入互斥保护的待处理队列并分配稳定 id；UI 线程通过 `poll()`/`tick()` 每次最多消费 `drain_batch()` 条（默认 256），因此突发大日志不会一次性重建模型或阻塞绘制。`set_max_entries()` 同时约束队列与可见记录，超限丢最旧记录并累计 `dropped_count()`；`pending_count()`/`idle()`/`last_entry_id()`/`entry_at()` 便于宿主观察。可见日志是虚拟树投影：只有当前行窗口会调用 `row_info` 等回调，每个错误默认只占一行，堆栈帧在 `set_stack_expanded()`/`toggle_stack()` 后才加入投影，展开状态按 entry 独立保存。等级复选框与搜索框相互独立，搜索大小写不敏感地匹配消息、来源、时间戳、等级名、位置路径及堆栈函数/路径；`filter_active()` 只在有等级关闭或搜索非空时为真。`set_auto_scroll()` 与过滤完全解耦，勾选状态同步，启用时滚动到最新行。复制支持选中行优先、否则全部可见行，`selected_text()`/`visible_text()` 可直接取文本，`copy_selection()` 经隐藏只读编辑控件写入系统剪贴板；树内 `Ctrl+C` 复制、`Ctrl+A` 全选。行激活（双击/Enter）优先调用宿主 `set_jump_handler()` 给出的资产 id 或脚本路径+行列，未安装处理器时错误行会切换堆栈折叠。每级默认色可通过 `set_level_color()` 覆盖，主题切换/未覆盖等级回退当前主题。`editor_shell` 在底部 CONSOLE 页托管一个 `console_view` 并暴露 `console()`；新增 View 菜单命令 `view.console`，可展开底部面板、切到 CONSOLE 并聚焦日志树。`vgui_game_editor` 启动时写入 trace/debug/info/warning/error 与带堆栈的错误，安装跳转处理器（资产定位/选中、脚本行列状态反馈），并在每帧 `tick()` 排空队列；资产通知、导入和视口落点也追加日志。测试 `test_console_view` 覆盖构建/默认值、生产队列与有界消费、等级与搜索独立/组合/大小写、堆栈折叠与帧/条目跳转、可见/选中文本、等级颜色、自动滚动、清空、保留上限与 dropped 计数，以及 4 线程并发 400 条追加；shell 集成断言 CONSOLE 为真实控件、`view.console` 切换与访问器追加可见。

### M5 Viewport 与 Gizmo（V）

目标：从“显示 FBO 的图片”升级为可交互的场景视口。

依赖：F02、F03；V01/V02/V03 是 V05/V07 的前置；V08 是 T02/T03 的画布基础。

- [x] **V01 Viewport 控件封装**（P0，M）（已完成）
  - 绑定 native texture，处理 resize/FBO 重建。
  - 鼠标捕获、焦点、拖出控件边界时继续接收事件。
  - 验收：窗口缩放时视口不闪烁；拖拽 Gizmo 到视口外仍连续；失去焦点时正确结束操作。
  - 已实现：`viewport_view` 从 `VGUI_IMAGE` 重构为可聚焦的 `VGUI_CUSTOM` 表面，纹理绘制在 1px 边框内侧；`poll_resize()` 在布局后给出整数渲染尺寸（隐藏/折叠页不触发 1x1 重建）并回调 `resize_listener`，宿主原地 `vgui_sdl3_texture_update_native` 重建 FBO，旧纹理保持绑定避免闪白。事件经 `set_event_listener` 全量透传，按下后核心 `press_target`/`active` 保证拖出控件边界仍有 MOUSE_MOVE/MOUSE_UP 且坐标为本地（可为负）；新增 `vgui_context_add_focus_listener`/`remove_focus_listener`（与旧单监听器并存，不覆盖 Outliner 的行内重命名提交）和 `vgui_input_cancel()`，焦点旁落时结束手势并向宿主转发 `VGUI_EVENT_CANCEL`。SDL3 后端在 `SDL_EVENT_WINDOW_FOCUS_LOST` 调用 `vgui_input_cancel`，窗口切走不会残留按住的交互。`vgui_game_editor` 用 `phase0_renderer` 接入真实 FBO + native texture，状态栏显示视口尺寸与拖拽/取消反馈。测试：kit `test_viewport_widget`（渲染尺寸边沿触发、纹理跨 resize 保持、跨边界拖拽与本地坐标、键盘、焦点取消、`vgui_input_cancel`、隐藏页不重建）。

- [x] **V02 相机导航**（P0，M）（已完成）
  - 中键平移、右键旋转、滚轮缩放、F 聚焦选中对象。
  - 验收：导航不与 Gizmo 操作冲突；聚焦后对象位于视口中心；相机状态可保存/恢复。
  - 已实现：新增 `viewport_camera`（与渲染器无关的轨道相机）——`camera_state`（target/yaw/pitch/distance/fov/near/far）与 `camera_focus`（center/radius）纯数据结构；中键按屏幕空间 1:1 平移 target，右键按 `yaw += dx*sens / pitch += dy*sens` 轨道旋转并把 pitch 钳制在 ±1.5 rad，滚轮 `distance *= exp(-wheel_y*speed)` 并受 min/max 限制，F 通过宿主 `focus_provider` 把 target 移到对象中心并按半径取景（朝向不变，对象落在视口中心）。相机只消费中/右键，左键完全留给 V03/V05；`set_navigation_enabled(false)` 可整体关闭并在 Gizmo 期间使用，`cancel_navigation()` 结束拖动但保留当前视角。`get_state/set_state/reset` 支持保存恢复，`set_state` 钳制非法 pitch/distance/fov/near/far。`viewport_view` 新增 `add_event_listener/remove_event_listener`（与宿主 `set_event_listener` 并存、token 安全分发），相机以其挂载；合成的取消事件也会经过附加监听器。`editor_shell` 默认 `camera_.attach(viewport_)` 并暴露 `camera()`，同时给视口补 `align_self=STRETCH` 修复 COLUMN 页面里只占 min-width 的问题。`phase0_renderer` 新增 `phase0_renderer_set_camera`（NULL 保持旧默认视角），`vgui_game_editor` 每帧把相机位置/目标/FOV 推给渲染器，状态栏显示 yaw/pitch/dist 并演示平移/旋转/缩放。测试：kit `test_viewport_camera`（派生位置、平移保持距离与越界连续、旋转方向与 pitch 钳制、缩放上下限、左键预留、焦点旁落取消、总开关、F 取景与半径距离、修改键 F 忽略、状态往返/钳制、多监听器并存与移除、重复 attach）。

- [x] **V03 拾取与选中**（P0，L）（已完成）
  - 射线拾取、框选、选中描边/高亮。
  - 与 Outliner 选中同步。
  - 验收：点击/框选结果准确；多选和取消选择行为一致；选中高亮不遮挡 Gizmo。
  - 已实现：`editor_context` 扩展为有序多选集合（`selection_ids/selection_count/is_selected`、`set_selection(ids,primary)`、add/remove/toggle/clear、`selection_set_listener`），主选 `selection()` 与旧单监听器完全兼容，通知时先主选后集合。新增 `viewport_selection`：左键按下记录内容区起点，超过阈值进入框选，拖出视口仍连续；`point_picker` 收到内容区本地坐标/尺寸/修饰键，`box_picker` 收到规范化矩形；无修饰替换、Ctrl 切换、Shift 追加、空白点击清空、Ctrl/Shift 空白保持，一次手势只通知一次，Esc 与 `VGUI_EVENT_CANCEL` 取消不改选择；`highlight_provider` 返回屏幕空间矩形，kit 绘制半透明填充 + 1px 描边（主选强调色）与跑马灯。`viewport_view` 新增 `get_content_rect` 和三段 overlay（selection → interaction → foreground），固定顺序保证 V05 Gizmo 始终绘制在高亮之上；`outliner_view::enable_selection_sync` 改用集合监听器，树到 context 同时发布选中与反选，纯追加不滚动、主选变化才展开滚动；`editor_shell` 默认挂载 `picking()`、启用 Outliner 多选并在状态栏显示 `Selected: N objects`。`vgui_game_editor` 新增 `Cube` 可拾取对象、宿主侧射线/AABB/投影求交、框选与屏幕高亮，属性面板改为监听完整选择集。测试：kit `test_editor_context_multi_selection`（集合语义/主选/通知）、`test_viewport_selection`（点击/框选/修饰键/取消/坐标/禁用）、`test_outliner_multi_selection_sync`（双向同步/反选/清空）。

- [x] **V04 网格、坐标轴、吸附**（P0，M）（已完成）
  - 世界网格、原点轴、平移/旋转/缩放吸附、网格尺寸切换。
  - 验收：吸附步长可配置；网格随相机缩放保持合理密度；关闭吸附后行为恢复自由移动。
  - 已实现：新增 `viewport_grid`——按相机 target 取整生成 XZ 平面线段（`half_cell_count` 控制范围、`plane_height` 控制高度），次线/主线/轴分别配色，X/Z 轴贯穿全范围、Y 轴从原点向上；`effective_cell_size(distance, height, fov)` 用 `height / (2 * distance * tan(fov/2))` 求世界单位→像素比例，按 0.1～1000 的梯度升降档，默认把格距钳在 24～96 px，`auto_density=false` 时使用固定 `cell_size`；`build(camera, w, h, out)` 在隐藏或参数非法时返回 false 并清空输出，宿主可每帧直接上传。新增 `snap_settings`——`translate/rotate/scale` 分通道开关、可配置步长，`snap_translation/snap_rotation_degrees/snap_scale` 统一 `round(v/step)*step`，任一开关或步长为零时返回原值。`editor_shell` 暴露 `grid()`/`snap()`，新增 `view.grid`/`view.snap` 复选命令与 `view.grid.auto`、`view.grid.0.5/1/2/5` 固定格距命令（切换固定格距时同步平移吸附步长），View 菜单加入 Grid、Snapping 与 Grid Size 子菜单，工具栏改用命令按钮共享同一状态。`phase0_renderer` 新增混合、深度测试/不写深度的网格管线与动态顶点缓冲，把线段扩成世界空间细四边形并在立方体之后绘制，遮挡与混合正确；`vgui_game_editor` 每帧生成线段、转换 `0xAARRGGBB→0xAABBGGRR` 后上传，Auto 模式把有效格距同步给 `snap().translate_step`。测试：kit `test_viewport_grid`（默认值、2/10/50 距离的密度梯度与像素范围、固定格距、线段数量/对齐/平面高度、主次线与轴颜色方向、`half_cell_count`、隐藏）、`test_snap_settings`（平移/旋转/缩放与向量吸附、总开关/单通道/零步长回退、自定义步长），并在 shell 块断言 `view.grid`/`view.snap`/`view.grid.auto`/`view.grid.2` 的状态与 checked。

- [x] **V05 平移 Gizmo**（P0，L）（已完成）
  - 三轴、平面手柄、局部/世界空间、多选中心。
  - 验收：轴/平面拖动方向正确；局部/世界切换生效；操作进入命令栈并可取消。
  - 已实现：新增 `viewport_gizmo`——宿主提供 `gizmo_frame`（世界枢轴 + 已按世界/局部解析的三个正交轴 + 可选屏幕长度）、世界→内容区投影与屏幕→世界射线；kit 在视口前景层按 80px 屏幕长度绘制三轴箭头和 32% 长度的三块平面手柄（悬停/拖动高亮，深色描边保证在任意背景上可读），命中测试先轴线（7px 容差）后平面四边形。拖动时轴约束用射线与轴的最近点参数，平面约束用射线与法向平面求交；增量按 `snap_settings` 的平移步长吸附（世界/局部轴均按标量投影吸附，不改变枢轴绝对位置），关闭吸附后完全自由。手势分为 `begin / preview / commit / cancel`：Esc、`VGUI_EVENT_CANCEL`（焦点旁落）、`cancel_drag()` 与隐藏/切换空间都会发出 cancel；拖动中每帧重新读取帧用于绘制，但拖动数学始终使用按下时快照，宿主可在 preview 中实时移动对象且不会形成反馈。`viewport_view` 的附加监听器改为“首个消费事件的监听器终止分发”，`editor_shell` 在拾取之前挂载 Gizmo，左键命中手柄时不会再启动框选；新增 `gizmo()` 访问器与 `view.gizmo`、`view.gizmo.local` 命令（View 菜单复选）。`phase0_renderer` 新增 `phase0_renderer_set_object_transform`（位置 + 偏航，未调用时保留原 Phase 0 自转），并修正离屏投影的垂直方向，使原生纹理显示与相机/拾取/叠加层数学一致；`vgui_game_editor` 的 Cube 带位置/偏航，射线-AABB、屏幕包围盒、F 取景均使用该变换，Gizmo 拖动通过宿主快照把多选位置写入 `function_command("Translate")`，一次拖动只产生一条 undo，取消时恢复并刷新 Undo/Redo 状态。测试：kit `test_viewport_gizmo`（三轴/平面命中、轴约束与吸附、关闭吸附后的自由增量、平面 Z 约束、Esc/焦点旁落/显式取消、隐藏与无效帧不参与命中、45° 局部轴拖动方向、空间状态往返、detach），shell 块断言 `view.gizmo` 与 `view.gizmo.local` 的状态和 checked。

- [x] **V06 Viewport 拖放目标**（P0，S/M）（已完成）
  - 资产拖入创建对象，拖放时显示落点和预览。
  - 验收：拖放创建的对象类型正确；落点使用拾取位置；取消拖放不产生对象。
  - 已实现：`viewport_view` 新增 `drop_hover(x,y,payload)`——宿主每帧轮询当前被接受的拖放悬停，核心仍持有拖拽状态时返回绝对坐标与载荷，离开视口/取消/释放后自动返回 false，无需额外的离开回调即可清理预览；内部 drop target 在 hover 时记录最新位置。核心 `vgui_key_impl` 在拖拽进行中优先消费 Escape，调用 `vgui_input_cancel` 取消拖拽，避免焦点控件或菜单先吃掉按键。`phase0_renderer` 新增 `phase0_renderer_set_instances`——按位置/偏航/缩放绘制多个立方体代理，`ghost` 实例使用半透明顶点缓冲和混合、不写深度的网格管线，在网格之后绘制；未调用时保留 Phase 0 单立方体路径。`vgui_game_editor` 用屏幕射线与网格平面求交得到落点（对象中心在地面上方 1 单位），拖拽悬停时绘制半透明幽灵立方体和按资产类型着色的地面方框，状态栏显示 `drop 资产 (类型)`；释放时以同一落点创建对象（Mesh 写 `mesh`、其他资产写 `asset`，并标记 `renderable`），对象立即渲染、加入 Outliner/选择集，并通过 `push_create_command` 记录一条可撤销命令；Esc 取消后幽灵/方框消失且不创建对象。视口拾取、框选、选中高亮和 Gizmo 已改为遍历所有 `renderable` 对象，支持新建对象的选中与平移。测试：kit `test_viewport_drop_hover`（拖拽前无悬停、悬停载荷/坐标、离开视口清除、Esc 取消不触发 drop、真实释放只触发一次并清除）。

- [x] **V07 旋转/缩放 Gizmo**（P1，L）（已完成）
  - 旋转环、缩放轴、父空间/屏幕空间、数值输入联动。
  - 验收：旋转/缩放结果与数值面板一致；多选时围绕正确中心；支持吸附和撤销。
  - 已实现：`viewport_gizmo` 扩展为三模式编辑器——`gizmo_mode::translate/rotate/scale` 与 `gizmo_space::world/local/parent/screen`，宿主仍负责把空间解析成 `gizmo_frame` 的正交轴。旋转在按下时用屏幕射线与环平面求交，按帧内两轴取带符号角度并跨 ±π 展开，支持多圈拖动；三个环按投影后的折线绘制/命中（7px 容差），按 `snap_settings::snap_rotation_degrees` 吸附。缩放提供三轴端点方块与中心统一缩放方块：轴向因子由鼠标沿投影轴方向的位移除以屏幕手柄长度得到，统一缩放由到枢轴的投影距离变化得到，经 `snap_settings::snap_scale` 吸附并钳制到 0.01，缩放不需要射线。`gizmo_transform` 统一载荷包含 mode/handle、按下时的 origin+axes、世界平移、世界轴角旋转和各帧轴缩放因子；`set_transform_handler` 接收 begin/preview/commit/cancel，旧 `set_drag_handler` 只对平移模式继续回调。模式/空间切换、隐藏、Esc、焦点旁落都会取消拖动。`editor_shell` 新增 `view.gizmo.translate/rotate/scale`（W/E/R）与 `view.gizmo.space.world/local/parent/screen` 命令及 View/Gizmo Space 菜单复选，保留 `view.gizmo.local` 兼容切换。示例场景对象升级为位置 + 四元数 + 分轴缩放，`sample/editor_transform.h` 统一 TRS 应用、轴角→四元数与 XYZ 欧拉角/向量文本格式；属性面板新增 Position/Rotation/Scale 行并读取同一份场景状态，Gizmo 拖动同样快照完整 TRS，每个手势只记录一条 Translate/Rotate/Scale 命令，取消时恢复并刷新 Undo/Redo。`phase0_renderer` 的实例改为四元数 + 分轴缩放，拾取、屏幕包围盒与 F 取景使用完整模型矩阵。测试：kit `test_viewport_gizmo` 增补模式/空间切换取消、旋转环命中和 15° 吸附、关闭吸附后的自由旋转、跨 ±π 展开、旋转取消、轴向/统一缩放与 0.1 吸附、缩放取消、宿主帧移动时保持 begin 快照，以及 `editor_transform` 的旋转/缩放数值文本往返和多选共享枢轴；shell 块断言 W/E/R 模式、四个空间命令、checked 状态与快捷键。

- [x] **V08 2D Canvas**（P1，L）（已完成）
  - 平移/缩放、框选、网格吸附、控制点、旋转手柄。
  - 2D 场景/UI 编辑器复用。
  - 验收：Canvas 可独立作为控件使用；选择/拖拽/缩放状态与命令栈集成。
  - 已实现：新增 `canvas_view`（`gui/editor/canvas_view.h/.cpp`），与 3D 视口解耦，自己持有轻量 item 模型——稳定 `object_id`、未旋转 `canvas_rect`、绕中心旋转、矩形/椭圆/菱形、填充/描边/文字色、边框宽度、圆角、标签、可见与锁定；`add_item/update_item/remove_item/clear_items/find_item` 支持显式 id（重复 id 返回 `invalid_object`）与自动分配。视图以内容区左上角对应的画布坐标 + 正缩放表示，中键平移、滚轮按光标锚点缩放，`set_view_state/reset_view/zoom_at` 与缩放上下限可保存恢复；自适应网格按像素范围升降档并绘制原点轴，`effective_grid_step()` 可供宿主导航/标尺复用。左键点击选中，Ctrl 切换、Shift 追加、空白拖动为框选（与可视图元相交即选中，锁定/隐藏项忽略），空白点击清空；单选显示八个缩放手柄与旋转手柄，手柄在内容区屏幕坐标中固定大小并有独立容差。拖动时快照选中项，移动按主选绝对包围框吸附、单选缩放保持对角锚点且支持任意旋转、旋转按中心角与 `snap_settings::rotate_step_deg` 吸附；Esc、`VGUI_EVENT_CANCEL`、失焦、禁用、`set_view_state` 与显式 `cancel_gesture()` 均恢复快照。每次提交只向 `command_stack` 压入一条 `Move`/`Resize`/`Rotate`，无变化不产生命令；命令保存前后完整 item 副本，通过 weak token 在画布销毁后自动失效。主题感知绘制复用 OVG tools 的网格、原点轴、选择框/手柄、旋转手柄和蚂蚁线；`selection_listener` 与 `change_listener` 分别通知宿主选择集和模型/视图变化。`editor_shell` 在底部新增 CANVAS 标签（底部页顺序为 ASSETS / CONSOLE / CANVAS / TABLE），暴露 `canvas()`，把共享 `snap_settings` 与 `context_.commands()` 接入画布，并新增 `view.canvas` 命令/View 菜单项用于展开、切换和聚焦；布局持久化自动记录底部标签顺序。`vgui_game_editor` 为画布覆盖为与 20 单位网格一致的独立吸附策略（不跟随 3D 视口相机格距），并预置锁定背景、面板、按钮、头像和徽章 item，选择监听器在状态栏反馈，可直接拖动/缩放/旋转并用 Undo/Redo 验证。测试：kit `test_canvas_view`（模型/显式 id/坐标/缩放钳制、自适应网格、点击/Ctrl/Shift/框选与清空、锁定/不可见过滤、悬停与旋转手柄、移动/缩放/旋转吸附及 undo/redo、Esc/CANCEL/失焦回滚、多选移动、中键平移、滚轮锚点缩放、禁用、选择监听器、删除选中项），shell 块断言 CANVAS 页、`view.canvas` 命令、访问器、聚焦与底部标签顺序。

- [x] **V09 多视口 / HDR / MSAA / 深度纹理**（P1，L）（已完成）
  - 多相机视口、线性空间和 HDR、MSAA resolve、深度纹理作为 UI 纹理。
  - 验收：多视口共享资源但不互相覆盖；HDR/MSAA 开关可运行时切换；深度纹理可用于后处理预览。
  - 已实现：`sample/phase0_renderer` 从一个 `phase0_renderer` 统一持有着色器、管线缓存、立方体/幽灵顶点缓冲与采样器，每个 `phase0_view` 独立持有相机、实例/网格数据、颜色/深度附件和渲染选项；旧的 `phase0_renderer_*` 单视口 API 全部转发到默认 view，示例与旧代码无需改动。`phase0_view_set_options()` 可运行时切换 `hdr`、`msaa_samples`（1/2/4/8，按设备能力钳制）和 `depth_preview`：HDR 先渲染到 RGBA16F 再经 tonemap 写入 RGBA8 显示纹理；MSAA 通过 SDL_GPU resolve 到显示纹理，HDR+MSAA 先 resolve 到中间纹理再 tonemap；每个 view 的附件按需重建，不会覆盖其他 view 的颜色/深度目标。深度可视化输出 RGBA8 预览纹理，关闭 MSAA 时直接采样单采样深度，开启 MSAA 时因 SDL_GPU 不支持深度 resolve，另用一个单采样、无颜色目标的深度专用管线和空片段着色器重新填充 `preview_depth`。`editor_shell` 在 VIEWPORT 停靠页内新增第二相机视口与深度预览视口、独立 `preview_camera()`、`viewport_render_settings` 和 `view.hdr` / `view.msaa` / `view.depth` 命令（View 菜单可勾选，深度预览命令同时显隐深度面板）。`vgui_game_editor` 主视口与第二相机共享实例列表和管线，但各自维护相机、网格缓冲和附件；深度视口绑定主视口的深度可视化纹理，状态栏反馈 `hdr on` / `msaa 4x on` / 深度预览开关。测试：kit shell 块断言两个新视口/第二相机、默认设置、命令切换与设置 setter/重置；运行示例验证 LDR、HDR、MSAA、HDR+MSAA 与 MSAA 深度预览均无 Vulkan 校验错误或设备丢失。

- [x] **V10 `VGUI_IMAGE` 变换扩展**（P1，S/M）（已完成）
  - 九宫格、旋转、任意矩阵。
  - 验收：与现有 source/fit/tint/flip 组合正确；裁剪和像素对齐不回归；C API 可调用。
  - 已实现：公共 C API 新增 `vgui_image_set_nine_slice` / `vgui_image_set_rotation` / `vgui_image_set_rotation_center` / `vgui_image_set_matrix`。变换在 source 裁剪、fit 与 pixel snap 之后按 flip → rotation → matrix 顺序应用；矩阵为 `{xx,yx,xy,yy,tx,ty}`，与旋转共享枢轴，`NULL` 清除。九宫格边宽按纹理像素解释，源边宽按源区域收缩，目标过小时压缩，可与旋转/矩阵组合。原生纹理通过 OVG `add_texture` 的 `sliced/angle/center/matrix` 绘制；CPU 纹理普通绘制走 `add_image`，变换/九宫格由 vgui 生成几何，两者结果一致，并修复 CPU 纹理首帧因 `image.id == 0` 被跳过的问题。`vgui_texture_test` 扩展到 512×256、79 项断言，覆盖原生/CPU 首帧绘制、旋转、自定义旋转中心、flip+旋转、矩阵平移/缩放、旋转+矩阵、九宫格、source+旋转、九宫格+旋转、父级裁剪、空纹理跳过及更新路径；`vgui_demo` 图片卡片新增旋转循环、九宫格与矩阵错切按钮。

### M6 专业编辑器与数据视图（T）

目标：补齐完整编辑器常见的表格、时间轴、节点图和代码编辑能力。

依赖：T01 依赖 F04；T02/T03 依赖 V08；T04 可独立排期。

- [x] **T01 Table / DataGrid**（P1，M）（已完成）
  - 列宽、表头、排序、虚拟行、单元格选择/编辑。
  - 验收：大数据表只渲染可见行；列宽和排序状态可保存；单元格编辑器复用 PropertyGrid 的类型化编辑器。
  - 已实现：新增独立 `table_view`（`gui/editor/table_view.*`），`editor_shell` 底部新增 TABLE 页并暴露 `table()` 与 `view.table` 命令。数据源是拉取式 `table_source`：`row_count` 每次刷新只查询一次，绘制只为可见窗口调用 `cell_text`，`last_render_cell_queries()` 可供测试断言；可选 `row_id` 让选中单元格跨排序/刷新跟随稳定行 id，`cell_style` 可覆盖单元格文字/背景色。列由 `table_column` 描述（id/label/宽度/最小宽度/对齐/可排序/可编辑/`property_desc`/自定义比较/`set_value`），表头点击切换升/降序，拖拽表头边界调整列宽；排序为稳定排序，数字/布尔按类型比较、文本大小写不敏感，排序前只读取当前排序列。支持横/纵向滚动条拖动与滚轮滚动、点击/双击命中、方向键/Home/End/PageUp/PageDown 导航、Enter/F2 或双击开始编辑、Esc 清除选择。单元格编辑器直接复用 PropertyGrid 的类型化组件（文本、Spinbox、Checkbox、枚举菜单、颜色、Vector2-4、曲线/渐变），写入前经 `property_normalize_value()` 归一与校验；文本/数字/向量在提交或弹窗关闭时写入，布尔/枚举立即写入，颜色/曲线/渐变在单元格内实时预览并在弹窗关闭时提交（Esc 回滚）；每次实际变化压入一条 `Set <列>` 命令，只读列、校验失败或宿主拒绝不产生记录。`table_state`（版本 1）保存列宽与排序列/方向，`table_state_to_json/from_json` 校验版本、数值和索引；`editor_layout` 升至版本 3，以不透明 `table` 字符串保存该状态，仍接受 v1/v2 文件，存在但非法的表格状态会使加载失败而不是静默覆盖。`editor_shell::capture_layout/apply_layout` 自动收集/恢复表格状态。`vgui_game_editor` 以场景对象为表格模型提供 Name/Parent/Visible/Locked/Position/Tint/Mesh 七列，布尔/向量/颜色/文本编辑复用类型化编辑器并同步属性面板、Outliner 与状态栏。测试 `test_table_view` 使用 1 万行数据断言行数只查一次、滚动与绘制只查询可见行、排序只读取排序列、稳定选择、宽度/排序 JSON 往返与非法状态拒绝、文本/整数（越界钳制与非法输入）/布尔/枚举/颜色/向量编辑、只读列拒绝、取消与 undo/redo；布局测试覆盖 TABLE 页顺序、`view.table` 命令及 v3 表格状态保存/恢复。

- [x] **T02 Timeline / 动画编辑器**（P1，L）（已完成）
  - 轨道、关键帧、时间尺、缩放/平移、框选、复制/粘贴、吸附、播放控制。
  - 验收：关键帧拖拽/复制/删除可撤销；缩放后时间精度正确；播放头与外部时钟同步。
  - 已实现：新增独立 `timeline_view`（`gui/editor/timeline_view.*`），`editor_shell` 底部新增 TIMELINE 页并暴露 `timeline()` 与 `view.timeline` 命令。模型由 `timeline_track`/`timeline_keyframe` 组成：轨道含稳定 id、名称、颜色、行高、可见/锁定/静音，关键帧含稳定 id、`double` 秒时间、标量值和 step/linear/smooth 插值；`add_track()`/`add_keyframe()` 支持显式 id 并拒绝冲突，`evaluate()` 负责采样。视图用 `view_start` + `pixels_per_second` 描述，时间换算全程 `double`；时间尺按缩放选择 1/30 s 到 10 min 的可读步长，中键平移、Ctrl+滚轮以光标为锚缩放、Shift+滚轮水平滚动、普通滚轮纵向滚动，缩放/框选/命中在极端缩放下仍保持亚帧精度。交互支持点选、Ctrl 切换、Shift 追加、空白拖动框选、双击插入、拖动选中集合、Esc/CANCEL/失焦回滚；快捷键包括 Space 播放、Left/Right 单帧、Home/End、Delete、Ctrl+C/V/X/A。吸附默认跟随帧率（30 fps），可切换为自定义步长；拖动、插入、粘贴、删除各生成一条 `Move keyframes`/`Add keyframe`/`Paste keyframes`/`Delete keyframes` 命令，多选移动只记录一条，剪贴板保存相对时间并在播放头处粘贴。播放由宿主 `tick(delta)` 驱动，支持 play/pause/stop/toggle、单帧步进、循环与到达末尾自动停止；`set_playhead()` 始终权威，`set_playhead_listener()` 让宿主采样动画。`timeline_state`（版本 1）保存视图范围/缩放/滚动、播放头、时长、帧率、吸附开关/步长、循环和选中轨道，`timeline_state_to_json/from_json` 校验版本、有限数值、正缩放/时长/帧率/步长与播放头范围；`editor_layout` 升至版本 4，以不透明 `timeline` 字符串保存该状态并继续接受 v1/v2/v3 文件，存在但非法的 TIMELINE 状态会使加载失败而不是静默覆盖。`vgui_game_editor` 用 Cube Height/Cube Yaw 两条轨道演示关键帧编辑，主循环调用 `timeline().tick(delta)`，播放头监听器把高度和偏航写回立方体。测试 `test_timeline_view` 覆盖模型/显式 id/插值采样、几何与命中、点选/Ctrl/Shift/框选、拖拽吸附与 Esc/CANCEL/失焦回滚、复制/剪切/粘贴相对时间、插入/删除/移动的 undo/redo、键盘快捷键、播放/循环/步进/外部 tick 与监听器、缩放锚点与亚帧精度、状态 JSON 往返与非法值拒绝；布局测试覆盖 TIMELINE 页顺序、`view.timeline` 命令及 v4 时间线状态保存/恢复。

- [x] **T03 NodeGraph**（P1，L）（已完成）
  - 节点、端口、连线、框选、缩放/平移、搜索、小地图、右键菜单。
  - 验收：节点拖动和连线命中准确；框选/多选/删除可撤销；大图缩放不丢失交互精度。
  - 已实现：新增独立 `node_graph_view`（`gui/editor/node_graph_view.*`），`editor_shell` 底部新增 NODEGRAPH 页并暴露 `node_graph()` 与 `view.nodegraph` 命令。模型由 `node_node`/`node_port`/`node_connection` 组成：节点含稳定 id、标题、分类、位置、宽度（120–600 钳制）、折叠/可见/锁定、标题色和输入/输出端口；端口含方向、类型（flow/number/string/boolean/color/vector/object/custom）和 `multi`；连线含稳定 id、源/目标节点与端口、颜色和可见性。节点、端口、连线共享一个 id 空间，显式冲突会被拒绝；`connect_ports()` 统一规范化输出→输入，类型相同或任一端为 `custom` 才能连接，`multi = false` 的输入会替换旧连线。所有图坐标、偏移与缩放使用 `double`；节点/端口/连线命中、6 屏幕像素连线容差和框选均在当前缩放下精确换算，连线绘制与命中共享图空间三次贝塞尔控制点。交互支持左键点选、Ctrl 切换、Shift 追加、拖动选中集合、锁定节点可选不可移、空白拖动框选节点与连线、从端口拖出创建连线、中键平移、滚轮以光标为锚缩放、Shift+滚轮平移；Esc、`VGUI_EVENT_CANCEL` 或失焦回滚手势快照。节点拖动、连接、断开、删除和复制分别压入一条 `Move nodes`/`Connect nodes`/`Disconnect nodes`/`Delete nodes`/`Duplicate nodes` 命令，无实际变化不记录。自适应网格按屏幕密度加倍有效格距，支持吸附、`F` 框选视图、`Home` 适配全图、`Ctrl+A` 全选和 `Ctrl+D` 复制；`Ctrl+F` 聚焦内嵌搜索框，匹配节点高亮、其余变暗，Enter 循环匹配并居中。右下角小地图显示节点边界与视口矩形，可点击/拖动导航；右键菜单提供 Add Node、Duplicate、Delete、Disconnect、Frame Selection、Fit All、Select All 及网格/小地图/吸附开关，Add Node 由宿主 `set_create_node_handler()` 决定节点类型。`node_graph_state`（版本 1）保存视图偏移/缩放、网格/小地图/吸附开关、网格步长、选择集与主选，`node_graph_state_to_json/from_json` 校验版本、有限数值、缩放/步长与选中 id 格式；`editor_layout` 升至版本 5，以不透明 `node_graph` 字符串保存该状态并继续接受 v1–v4 文件，存在但非法的 NODEGRAPH 状态会使加载失败而不是静默覆盖。`vgui_game_editor` 预置 Scene Color / Time / Multiply / Surface Output 节点和颜色连线，右键 Add Node 创建带颜色端口的 Utility 节点，选择监听同步状态栏。测试 `test_node_graph_view` 覆盖模型/id/端口类型与单输入替换、几何与命中、点选/Ctrl/Shift/框选、拖动与锁定节点、Esc/CANCEL/失焦回滚、高倍与极端偏移精度、连接/断开/复制/删除的 undo/redo、搜索/小地图/上下文动作、缩放/平移和状态 JSON 往返/非法值拒绝；布局测试覆盖 NODEGRAPH 页顺序、`view.nodegraph` 命令及 v5 节点图状态保存/恢复。

- [x] **T04 CodeEditor / RichText**（P2，L）（已完成）
  - 语法高亮、行号、查找替换、只读富文本日志。
  - 验收：可作为脚本编辑器或日志查看器嵌入；大文件打开不阻塞 UI；复制/选择行为与系统习惯一致。
  - 已实现：新增独立 `code_editor`（`gui/editor/code_editor.*`），`editor_shell` 底部新增 CODE 页并暴露 `code()` 与 `view.code` 命令。文档为按行存储的 UTF-8 模型，CRLF/CR 统一归一为 LF；每行缓存 token span，按需惰性词法分析并跨行保持 C 块注释、Python 三引号、Lua 长注释/长字符串和 Markdown 围栏状态。内置 plain/C++/C#/GLSL/JSON/Lua/Python/INI/Markdown 语言，支持按扩展名自动识别、token 配色覆盖，以及 `set_line_spans()` 宿主手工富文本 span。编辑支持插入/删除/回车自动缩进/Tab 缩进与 Shift+Tab 反缩进、全选、鼠标点选/拖动/双击选词/三击选行、方向键/Home/End/PageUp/PageDown、UTF-8 边界钳制和 IME 文本输入；未绑定命令栈时使用本地历史，绑定后每次编辑压入一条可合并的 `Insert`/`Backspace`/`Delete`/`New Line`/`Indent`/`Outdent`/`Replace` 命令。复制/剪切/粘贴默认经隐藏 `VGUI_EDIT` 桥接系统剪贴板，`set_clipboard_handlers()` 可注入测试或宿主编解码。查找栏支持 `Ctrl+F`、替换模式 `Ctrl+H`、大小写切换、上/下一个、当前替换和全部替换，`find_all()`/`matches()` 暴露匹配模型。只读模式可作富文本日志：`append_log()` 带时间戳与 trace/debug/info/warning/error 等级颜色，支持 `set_max_lines()` 保留上限、`dropped_lines()` 统计和自动滚动；`set_read_only(true)` 仍保留选择与复制。大文件通过 `open_file()`/`begin_incremental_load()` 分块读取，`process_load_step()` 每次最多消费 `load_chunk_size()` 字节，`tick(delta)` 在宿主主循环推进加载与日志自动滚动，并提供 `load_progress()` 与 `cancel_load()`。`code_editor_state`（版本 1）保存语言、只读/行号开关、Tab 宽度、光标和滚动；`code_editor_state_to_json/from_json` 校验版本、语言、非负光标/滚动与有限值。`editor_layout` 升至版本 6，以不透明 `code_editor` 字符串保存该状态并继续接受 v1–v5 文件，非法 CODE 状态会使加载失败而不是静默覆盖；`capture_layout/apply_layout` 自动收集/恢复。`vgui_game_editor` 在 CODE 页预置 C++ 示例脚本并每帧调用 `code().tick(delta)`。测试 `test_code_editor` 覆盖语言识别、状态 JSON 往返/非法值、构建、换行归一、各语言与跨行词法、惰性高亮、手工 span、编辑/自动缩进/缩进撤销、本地与命令栈 undo/redo、UTF-8 光标/选区、剪贴板、只读、查找/大小写/替换、富文本日志颜色/保留/自动滚动、分块加载（含跨块 CRLF）、文件打开/失败保留、取消加载和状态应用；布局测试覆盖 CODE 页顺序、`view.code` 命令及 v6 编辑器状态保存/恢复。

### M7 工程化与打磨（E）

目标：让编辑器在 DPI、性能、可访问性和测试上达到可持续交付水平。

- [x] **E01 DPI 缩放**（P1，M）
  - 统一 `ui_scale`，字体、间距、圆角、图标一起缩放。
  - 验收：100%/125%/150%/200% 缩放下布局不重叠；切换显示器后自动更新；截图基准可复现。
  - 已实现：核心新增 `vgui_context_set_ui_scale/get_ui_scale`、`vgui_context_get_font_size`、`vgui_widget_get_font_size`、`vgui_style_scale` 与 `vgui_ui_scale_fn` 监听器。公共几何/字体/样式 setter 接收逻辑单位并乘以 `ui_scale`；`vgui_widget_get_rect`、`vgui_context_get_style` 和字体 getter 返回有效像素。上下文保存 `base_style`/`base_font_size`，比例变化时按新旧比值重算整棵控件树（Splitter/Tab/Dock 分区尺寸、浮动矩形、滚动偏移与文本缓存一并处理），重复切换不累积误差；比例钳制在 `[0.25, 4.0]`，非有限值/0/负数回退 `1.0`。`vgui_style_scale` 只缩放几何字段，动画速度、透明度、混合系数保持不变。SDL3 后端新增 `content_scale()`/`pixel_ratio()`/`sync_ui_scale()`，在创建窗口、显示器变化和每帧 `get_event()` 时同步；鼠标/按钮/文件 drop 坐标从窗口点换算到渲染像素，IME 矩形再换算回窗口点。editor kit 通过 `gui/editor/ui_scale.h` 在逻辑/像素边界换算：Table 的模型列宽/行高保持逻辑、滚动偏移保持像素，PropertyGrid 视口/滚动、AssetBrowser 网格、Outliner 行内重命名及编辑器布局持久化（Dock 分区尺寸/浮动矩形）均按逻辑单位存取。Timeline 轨道高度、NodeGraph 图坐标、Table 模型列宽保持逻辑，绘制/命中测试使用有效像素；Canvas 文档坐标不变而边框/手柄/选择线框按比例缩放；AssetPreview、Curve/Gradient/ColorPicker、Viewport、CodeEditor 的固定 chrome 与命中半径随 `ui_scale` 缩放，曲线/渐变值等内容语义不变。测试：`vgui_model_test` 757 项、`vgui_c_api_test` 32 项；`vgui_editor_kit_test` 的 `test_shell_ui_scale` 覆盖 100%/125%/150%/200% 外壳与停靠区不重叠、面板不越界，以及 200%→125%→100% 的布局往返；`test_custom_view_dpi_input` 在 200% 下验证 AssetPreview 整树缩放、Curve/Gradient 命中半径和 ColorPicker 的 SV 归一化坐标。布局/几何结果确定，截图基线将在 E03 基于同一逻辑尺寸建立。

- [x] **E02 性能与增量更新**（P1，M）（已完成）
  - 属性行、树节点、资产缩略图的增量刷新和缓存。
  - 避免属性面板每次选中都清空重建。
  - 验收：选择切换、滚动、拖拽时无明显卡顿；性能基准纳入 CI 或固定测试场景。
  - 已实现：PropertyGrid 新增 `set_properties()`，按 `id + type` 复用行：仅新增、删除和类型变化的行创建/释放控件，其余行原位刷新描述、值、标签、向量语义/轴标签/范围/步进/只读与多目标混合态；返回值就是需要重建的行数（同 schema 对象切换为 `0`），行顺序跟随输入，滚动位置、分类折叠、过滤和收藏状态保持不变。`add_property()` 的校验/默认值逻辑抽到 `prepare_desc()`，两条入口使用同一套接受规则；非法或重复 id 被跳过。Outliner 的虚拟 `scene_source` 原已按可见行投影，E02 在 `vgui_game_editor` 中完成 dogfood：结构变化走 `scene_changed()`，重命名/可见性/锁定走 `scene_row_changed()`，图标由 source 回调提供，flag 镜像按差异回写，表格只刷新对应行，不再为选择或名称变化 clear/add 整棵树。AssetBrowser 沿用虚拟列表/网格行源与 A05 缩略图缓存：`refresh()` 只重建视图索引并通知行源变化，可见范围缺图才入队，内存/磁盘命中不重新加载。测试：`test_property_grid_set_properties` 覆盖 5→0→0→1→1→1→0 的重建计数、行 widget 身份、向量元数据、分类重排、非法描述拒绝与 `bind_selection()` 端到端复用；`test_virtual_outliner` 断言折叠子树不查询名称、单行刷新/结构变化后选择保持；A05 的浏览器缓存命中测试断言缩略图复用；全部为固定计数/身份断言，不依赖墙钟时间。

- [x] **E03 截图回归测试**（P1，M）（已完成）
  - Dock、PropertyGrid、Outliner、AssetBrowser、Viewport 的关键状态截图对比。
  - 验收：主题/布局/控件状态变化有基线图；失败时输出差异图；CI 可运行。
  - 已实现：新增 `sample/vgui_screenshot_test.cpp` 与 `vgui_screenshot_test` 目标。测试沿用 `vgui_texture_test` 的离屏管线（SDL3 + SDL_GPU Vulkan → OVG FBO → `vgpu_render_frame` → 读回），不创建窗口；字体固定为 `sample/res/fonts/DejaVuSans.ttf`，逻辑尺寸与 `ui_scale=1` 固定。共 9 个场景：`dock_default_dark` / `dock_floating_dark`（1440×900，editor_shell 默认布局、分区尺寸变化、属性面板浮出、CONSOLE 日志页）、`property_grid_dark`（全类型行与 P08 混合值）、`property_grid_states_dark`（分类折叠、错误行、只读、收藏）、`outliner_selected_dark` / `outliner_filtered_light`（行图标、可见/锁定按钮、多选、过滤高亮、浅色主题）、`asset_browser_grid_light` / `asset_browser_list_dark`（网格/列表、CPU 缩略图、搜索与类型过滤、选中）、`viewport_gizmo_dark`（合成场景纹理、视野密度世界网格、选中框、真实 `viewport_gizmo` 平移 Gizmo）。渲染固定 `vgui_update(0.5)` + `vgui_layout()` 后连绘两帧，第二帧用于让列表/树/资产网格的 CPU 纹理获得 OVG 句柄后再读回。基线位于 `sample/screenshots/`，对比默认每通道容差 2、差异像素比例上限 0.05%（`--tolerance` / `--max-diff-ratio` 可调）；失败时在 `--out-dir` 写 `<scene>_actual.png` 与 `<scene>_diff.png`（差异像素标红）并返回 1；`--update` 重建基线，`--filter` 只跑子集，`--list` 列出场景。根 CMake 启用 `enable_testing()`，`sample/CMakeLists.txt` 注册 CTest `vgui_screenshots`（传入源目录基线与构建目录差异输出，`SKIP_RETURN_CODE=77`）；无 Vulkan 设备时测试返回 77 由 CTest 标记 SKIP。`ctest -R vgui_screenshots` 可直接在 CI 运行。同机重复运行 9 个场景均为 `diff=0`；`vgui_editor_kit_test` 与 `vgui_texture_test` 保持通过。

- [x] **E04 文档与示例**（P1，S）（已完成）
  - 新增 `sample/vgui_game_editor.cpp`，持续 dogfood。
  - 更新 README、示例/API 注释和本路线图状态。
  - 验收：新用户能按文档构建并运行编辑器示例；示例覆盖首批 MVP 主路径。
  - 已实现：`sample/vgui_game_editor.cpp` 已作为 editor_shell 的持续 dogfood 示例，覆盖外壳菜单/工具栏/状态栏、Outliner 行扩展/重命名/拖放/过滤/多选同步、Viewport 相机/拾取/框选/网格/吸附/平移旋转缩放 Gizmo/资产落点预览/HDR/MSAA/深度预览、Properties 类型化与多选混合值、Assets 目录导航/搜索/异步缩略图/导入与资产操作、Dock 重排/浮出，以及 CONSOLE/CANVAS/TABLE/TIMELINE/NODEGRAPH/CODE 页签和命令栈。README 新增“游戏编辑器示例”操作路径、控件说明与构建/运行命令，补充国际化/RTL、截图回归与 CTest 段落，并修正运行列表与项目结构（移除不存在的 `vgui.exe`/`vgui_legacy.cpp`，加入 `vgui_editor`、`vgui_phase0`、`vgui_screenshot_test`、`sample/screenshots/`）。`vgui_game_editor.cpp` 顶部注释同步为当前完整 dogfood 范围与常用操作。示例中的场景、资产库和命令历史均为内存模型，File > Save 只是状态占位；文档序列化、资产数据库和文件对话框由宿主实现，布局保存/恢复由 `editor_shell` API 提供并在 `vgui_editor_kit_test` 覆盖。新用户可按 README 的 vcpkg + CMake 命令构建并运行 `out/bin/x64d/vgui_game_editor.exe`；公共头注释已在各里程碑同步，本次补充示例顶部注释与 README 操作说明。

- [x] **E05 无障碍**（P2，L）（已完成）
  - 完整键盘导航、焦点陷阱、屏幕阅读器、高对比度主题。
  - 验收：核心编辑器流程可仅用键盘完成；焦点顺序可预测；高对比度主题满足基本可读性。
  - 已实现：核心新增 `vgui_a11y_role` / `vgui_a11y_state` / `vgui_a11y_info` 与 `vgui_widget_get_a11y_info`，角色按控件类型推导，名称优先显式覆盖、其次文本（编辑框不取正文，改用 tooltip/placeholder）、值按编辑文本/复选状态/数值/当前选中项生成，状态位覆盖焦点/选中/禁用/只读/展开；`vgui_widget_set_a11y_name` / `set_a11y_role` 提供覆盖，`vgui_context_a11y_walk` 按预序访问可见控件（根树、打开菜单、模态 stage、context 弹出层），模型行不单独成节点而由 `info.item` 暴露当前选中。`vgui_widget_set_focus_trap` 让 `Tab`/`Shift+Tab` 在当前焦点最近的陷阱祖先内循环，模态 stage、context 弹出层和浮动 Dock 页自动开启；`F10` 打开首个可见菜单栏的首个可用标题/再按关闭，`command_registry` 支持 `F1`～`F12` 快捷键名。屏幕阅读器桥接通过 `vgui_context_add/remove_a11y_listener` 接收焦点/值/状态/选中/告警事件，Toast 自动写入公告并触发 `VGUI_A11Y_EVENT_ALERT`，宿主可用 `vgui_context_announce/get_a11y_announcement` 推送自定义公告（不实现平台 UIA/AT-SPI provider）。`vgui_theme_high_contrast` 提供黑底/白字/黄色强调调色板；`editor_shell` 为菜单栏/工具栏/状态栏/搜索/面包屑/四个停靠面板/底部页签及各组件设置语义名称，新增 `view.focus.next/prev`（`F6`/`Shift+F6`，展开并聚焦 Outliner→Viewport→Properties→底部页签的首个可聚焦后代）与 `view.high_contrast`（`Ctrl+Shift+H`，保存/恢复主题并同步外壳硬编码配色）。核心 `vgui_edit_key` 对未处理按键返回未消费，使 `F6`/全局快捷键可达 context 处理器。测试：`vgui_model_test` 新增 `test_accessibility_e05`（角色/名称/值/状态、可见树、事件与监听器移除、自定义焦点陷阱、浮动 Dock 自动陷阱、F10 开关、高对比度 sRGB 对比度），现 757 项；`vgui_editor_kit_test` 新增 `test_shell_accessibility_e05`（外壳/组件名称与角色、属性行/资产过滤条目、`F6`/`Shift+F6` 顺序、快捷键、焦点事件、高对比度命令/主题/对比度/恢复）；`vgui_demo` 主题区新增高对比度入口，`vgui_screenshot_test` 9 个场景保持 `diff=0`。

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
- Console/Log（已在第二批 A07 完成）
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
- P09 Curve/Gradient（已完成）
- T01 Table/DataGrid（已完成）
- A06 资产操作（已完成）
- A07 Console/Log（已完成）
- V07 旋转/缩放 Gizmo（已完成）
- V08 2D Canvas（已完成）
- V09 多视口/HDR/MSAA/深度纹理（已完成）
- V10 `VGUI_IMAGE` 变换扩展（已完成）
- E01 DPI 缩放（已完成）
- E02 性能与增量更新（已完成）
- E03 截图回归测试（已完成）
- E04 文档与示例（已完成）

### 7.3 第三批：专业编辑器与打磨

- T02 Timeline / 动画编辑器（已完成）
- T03 NodeGraph（已完成）
- T04 CodeEditor / RichText（已完成）
- E05 无障碍（已完成）
- E06 国际化 / RTL（已完成）

第三批剩余项不应阻塞前两批；如果项目暂时不需要代码编辑，可以长期延后。

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
- `editor_shell`：单个 `VGUI_DOCK` 承载 OUTLINER / VIEWPORT / PROPERTIES / PANELS 四个面板，底部 PANELS 内用 TabView 组织 ASSETS / CONSOLE / CANVAS / TABLE / TIMELINE / NODEGRAPH；顶部 `toolbar_view` 包含场景搜索、Undo/Redo 命令按钮、Grid 开关和视图分段控件，底部 `status_bar` 同步 `set_status()` 与当前选择，Properties 面板顶部用 `breadcrumb` 显示选择路径；File/Edit/View/Help 菜单；Outliner、Viewport、PropertyGrid、AssetBrowser 组件边界保持不变。
- `editor_layout` 版本 5：保存 Dock 区域尺寸、标签顺序、活动页、打开状态、浮动矩形、底部 Tab、TABLE 列宽/排序、TIMELINE 视图/播放状态与 NODEGRAPH 视图/网格/小地图/吸附/选择；v1–v4 文件兼容映射，损坏或版本不支持时回退默认布局 (F05)。
- `outliner_view`、`property_grid`、`asset_browser`、`viewport_view`：可构建的占位组件（属性面板为真实类型化实现，支持行虚拟化、Vector2-4 多分量编辑与统一拖拽；行扩展与拖放已接入）。
- M2 P08：多选/混合值/复制粘贴/收藏——`set_multi_targets/getter/setter` 自动混合检测与 per-target 批量命令（一条 undo）、`copy/paste_properties` 进程内剪贴板（类型保持、shell Edit 菜单 Ctrl+Shift+C/V）、行级收藏星标与 "★" 过滤（跨重建保持）、`outliner_view::selection_objects()` 多选导出；`vgui_game_editor` Ctrl 多选混合值 dogfood。
- M2 P09：Curve / Gradient 编辑器——`curve_editor`（linear/step/cubic、关键帧与切线拖拽、双击加点、T/V/In/Out spinbox、紧凑预览）、`gradient_editor`（渐变条/色标拖放、双击插点、内嵌 HSV 取色器、4 预设、紧凑预览）；PropertyGrid `property_type::curve/gradient` 共用 popup 会话（实时预览、单条可合并 undo、Esc 回滚、多目标事务）；`vgui_game_editor` 增加 Idle Curve / Color Ramp 可操作路径。
- M2 收口（P05～P07）：核心 context 级通用弹出面板 `vgui_popup_*`（外点/Esc/滚轮点外关闭并区分原因、菜单/模态互斥、边缘钳制、焦点陷阱、close 回调）；kit `color_picker`（ovg tools 取色器原语 + press/change/commit 手势协议 + `set_color_transform` 颜色空间策略）；PropertyGrid 颜色行经 popup 取色（实时预览、会话单条 undo、Esc 回滚）、`asset`/`object_ref` 引用行（asset_query/browse/filter_check/object_activated/missing 五个宿主接口、行级 drop、清空、丢失态）、`begin_batch/end_batch` 批量事务（`reset_all` 改用）；`vgui_game_editor` 属性面板按选择重建（Name/Tint/颜色/网格/目标行）、资产拖到属性行赋值、丢失资产演示。
- M3 Outliner（O01～O06）：`outliner_view` 完整编辑器语义——行图标/可见性锁定按钮（O01）、F2/双击行内重命名 + Enter/Esc/失焦与命令栈（O02，依赖核心 `VGUI_EVENT_CANCEL`、`vgui_context_set_focus_listener`、`VGUI_KEY_F*`、`vgui_tree_get_item_rect`）、插入线/onto/roots 拖放语义 + RECT 提示 + 循环拒绝 + 多选事务（O03，kit `drop_target` 增可选 hover 虚函数；核心按在已选行且挂拖源时延迟改选）、名称过滤 + 匹配高亮 + 选择同步（O04，核心 `vgui_tree_set_filter/set_match_text`；editor_shell 场景搜索框已接）、虚拟场景源 + 稳定 id + 增量刷新（O05）、右键菜单与 Delete/F2/Ctrl+D 共用 dispatch（O06）；`sample/vgui_game_editor.cpp` dogfood 示例（editor_shell + 玩具场景全流程接线）。
- M4 A06 资产操作：`asset_browser` 统一 `operation_handler` 处理重命名/删除/复制/移动/重新导入，结构化错误覆盖冲突/只读/占用/非法目标/I/O；整批请求只有在宿主提供 `undo`/`redo` 时记录为一条命令，否则明确标记 `(not undoable)` 并警告；右键菜单覆盖列表/网格，F2/Delete/Ctrl+D/F5/Ctrl+I 与菜单共用 dispatch，Move To 子菜单、依赖模态框、Import... 提示框与通知/Toast 全部接通；旧 `rename_handler` 路径保留。`vgui_game_editor` 演示宿主数据库原子事务、快照撤销/重做、只读/占用/冲突/损坏资源与不可撤销重导入。
- M4 A07 日志控制台：`console_view` 任意线程生产者队列 + UI 线程有界 `poll/tick`、稳定 entry id、保留上限与 dropped 计数；虚拟树行投影只查询可见行，堆栈按 entry 折叠；等级复选框与大小写不敏感搜索（消息/来源/时间/等级/位置/堆栈）独立过滤，自动滚动独立开关；选中/可见文本复制经隐藏编辑控件写系统剪贴板，Ctrl+C/Ctrl+A；错误行/堆栈帧激活调用宿主 `jump_handler`（资产 id 或脚本路径+行列），无处理器时切换折叠。`editor_shell` 在底部 CONSOLE 页托管并暴露 `console()`，View 菜单 `view.console` 展开/切换/聚焦；`vgui_game_editor` 启动分级日志、带堆栈错误、资产/脚本跳转、每帧排空，并把导入/落点/资产通知写入日志。
- M5 V01 Viewport 表面：`viewport_view` 改为可聚焦 `VGUI_CUSTOM`，`poll_resize()` + `resize_listener` 驱动宿主原地重建 FBO/native texture，跨边界鼠标捕获、`vgui_context_add_focus_listener` 焦点取消、`vgui_input_cancel()` 与 SDL3 窗口失焦接线；`vgui_game_editor` 用 `phase0_renderer` 接入真实 3D 视口。
- M5 V02 相机导航：`viewport_camera` 轨道相机（中键平移/右键旋转/滚轮缩放/F 取景、pitch 与距离钳制、左键预留、`set_navigation_enabled`/`cancel_navigation`、`get_state/set_state` 保存恢复）；`viewport_view` 支持附加事件监听器，`editor_shell` 默认挂载并暴露 `camera()`，视口横向拉伸铺满中心区；`phase0_renderer_set_camera` 让示例按相机出图，状态栏反馈 yaw/pitch/dist。
- M5 V03 拾取与选中：`editor_context` 有序多选集合与集合监听器；`viewport_selection` 左键点击/框选、Ctrl 切换/Shift 追加/空白清空、Esc/CANCEL 取消、宿主 point/box picker 与屏幕高亮 provider；`viewport_view` 三段 overlay（selection/interaction/foreground）与内容区坐标；`outliner_view` 多选双向同步；`editor_shell` 默认挂载 `picking()` 并显示选择数量；`vgui_game_editor` 用宿主射线/AABB/投影演示点击、框选、高亮与属性面板多目标。
- M5 V04 网格与吸附：`viewport_grid` 生成 XZ 世界网格与原点轴，`effective_cell_size()` 按相机距离/FOV/视口高度在 0.1～1000 梯度间自适应（默认 24～96 px），支持固定格距、范围、平面高度、轴长与配色；`snap_settings` 提供平移/旋转/缩放分通道吸附（关闭或零步长回退原值）；`editor_shell` 暴露 `grid()`/`snap()` 并新增 Grid/Snapping/Grid Size 菜单与命令；`phase0_renderer` 新增混合且不写深度的网格管线，`vgui_game_editor` 每帧上传线段并按 Auto 格距同步平移吸附。
- M5 V05 平移 Gizmo：`viewport_gizmo` 以宿主的帧/投影/射线为输入，绘制三轴箭头与平面手柄，按 7px 轴线容差命中，轴用射线-轴最近点、平面用射线-平面求交约束拖动，增量经 `snap_settings` 吸附；`begin/preview/commit/cancel` 四阶段回调让宿主一次拖动只记录一条 `Translate` 命令，Esc/焦点旁落/显隐/空间切换均可回滚；`viewport_view` 改为首个消费监听器终止分发，`editor_shell` 在拾取前挂载 Gizmo 并提供 `view.gizmo`/`view.gizmo.local`；`phase0_renderer_set_object_transform` 驱动 Cube 位置/偏航并修正投影垂直方向，`vgui_game_editor` 的拾取、高亮、F 取景和 Gizmo 全部使用同一变换，实机验证拖动、吸附、局部空间与 Undo/Redo。
- M5 V06 视口拖放：`viewport_view::drop_hover` 从核心拖拽状态轮询被接受的悬停坐标/载荷，离开/取消/释放自动失效；核心拖拽中 Escape 优先取消；`phase0_renderer_set_instances` 支持多立方体代理与半透明 `ghost` 实例，`vgui_game_editor` 用屏幕射线-地面平面求交得到落点，拖拽时绘制幽灵立方体和类型色地面方框，释放后以同一落点创建 Mesh/资产对象并写入命令栈；视口拾取/框选/高亮/Gizmo 改为遍历所有 `renderable` 对象。实机验证预览、落点创建、类型/属性、选中与 Esc 取消不创建对象。
- M5 V07 旋转/缩放 Gizmo：`viewport_gizmo` 增加 `gizmo_mode`（平移/旋转/缩放）、`gizmo_space`（世界/局部/父级/屏幕）、投影环与缩放手柄，统一 `gizmo_transform` 载荷和 `set_transform_handler` 四阶段回调，旋转按环平面求交后展开带符号角度，缩放按投影位移计算轴向/统一因子，两者分别经 `snap_settings` 吸附；模式/空间切换、隐藏、Esc 与焦点旁落均取消拖动，旧 `set_drag_handler` 保持只对平移回调。`editor_shell` 增加 W/E/R 模式命令与四个空间命令及菜单复选；`vgui_game_editor` 的对象改为位置+四元数+分轴缩放，`sample/editor_transform.h` 共享 TRS/欧拉角/数值文本逻辑，属性面板 Position/Rotation/Scale 与 Gizmo 读写同一状态，一次拖动只记录一条命令并在取消时完整回滚；`phase0_renderer` 实例使用四元数与分轴缩放，拾取/包围盒/F 取景共用完整模型矩阵。kit 自测覆盖旋转/缩放手势、吸附、模式/空间切换取消、begin 快照、数值文本往返与多选共享枢轴；示例的撤销/重做路径与属性面板读写同一场景状态。
- M5 V08 2D Canvas：`canvas_view` 自带轻量 item 模型与选择集，支持中键平移、滚轮锚点缩放、自适应网格与原点轴、点击/Ctrl/Shift/框选、锁定/不可见过滤、八个缩放手柄与旋转手柄；移动按主选包围框吸附，单选缩放保留旋转后的对角锚点，旋转按中心角吸附；Esc/CANCEL/失焦回滚快照，每次手势只压入一条 `Move`/`Resize`/`Rotate` 命令，无变化不记录。`editor_shell` 底部新增 CANVAS 页并暴露 `canvas()`，共享 `snap_settings`/`command_stack`，`view.canvas` 命令展开并聚焦；`vgui_game_editor` 用独立 20 单位吸附策略预置 UI 场景 item，并提供状态栏/选择监听反馈。
- M5 V10 `VGUI_IMAGE` 变换：`vgui_image_state` 增加九宫格边宽、旋转角、旋转中心和可清除矩阵；公共 API 为 `vgui_image_set_nine_slice` / `vgui_image_set_rotation` / `vgui_image_set_rotation_center` / `vgui_image_set_matrix`。原生纹理把 `sliced/angle/center/matrix` 直接交给 OVG `add_texture`，vgui 矩阵按旋转枢轴共轭到绝对坐标；CPU 纹理普通绘制走 OVG `add_image`（与 `add_texture` 一样应用当前矩形裁剪），变换/九宫格走生成的四边形/九宫格 `add_geometry` 路径，实现相同的 flip → rotation → matrix 顺序、源区域钳制、目标边宽压缩、tint 与裁剪，并允许首帧在 `image.id == 0` 时先录制命令、提交时再解析纹理 id。`sample/vgui_texture_test.cpp` 扩展为 512×256 的 79 项断言，覆盖原生/CPU 首帧绘制、旋转、自定义旋转中心、flip+旋转、矩阵平移/缩放、旋转+矩阵、九宫格、source+旋转、九宫格+旋转、父级裁剪、空纹理跳过与原地更新；`vgui_demo` 图片卡片增加旋转循环、九宫格和矩阵错切入口。
- `sample/vgui_editor_kit_test.cpp`：无窗口自测通过，覆盖命令栈（合并/嵌套事务/失败协议）、选择同步、Dock 外壳（合并/浮出/关闭/区域尺寸）、菜单/快捷键、组件边界、行扩展、拖放、布局持久化、PropertyGrid 虚拟化窗口（物化有界/滚动释放/滚回恢复）、VectorEditor（语义/自定义轴、分量编辑、统一拖拽、值源双向绑定）、PropertyGrid 向量行、S07 工具栏/状态栏/搜索框/面包屑状态与回调、M3 Outliner 六项（行扩展按钮、行内重命名、拖放语义/多选/循环、过滤/同步、虚拟万级场景、右键菜单/快捷键）、M4 AssetBrowser A01/A02（双视图与十万级虚拟源、目录提供者、Breadcrumb/前进后退、选择与滚动恢复、reload、防抖搜索、类型过滤与空状态）与 A03 AssetPreview（后台加载非阻塞、陈旧结果丢弃、图像纹理、失败占位/错误、多监听器/过滤外查找与 shell 选择同步）、A04/A05 拖放导入与异步缩略图、A06 资产操作（宿主事务与结构化错误、批量单条 undo/redo、不可撤销标记、列表/网格右键、快捷键、重命名/删除对话框、依赖与导入通知）、A07 Console（有界排空/队列与模型分离、等级+搜索独立过滤、堆栈折叠、条目/帧跳转、可见/选中文本、等级颜色、自动滚动、清空/保留上限/dropped、4 线程并发追加、shell CONSOLE 页与 `view.console`），以及 M2 收口四项——`test_color_picker`（HSV 取色器 hex/alpha/策略往返、SV 手势 commit、popup 集成实时预览/命令合并/Esc 回滚）、`test_property_reference`（asset/object_ref 解析与丢失切换、拖放赋值/undo、filter 拒绝、清空+undo、browse、对象激活、批量事务嵌套与单条 undo、reset_all 单记录）、`test_property_multi/clipboard/favorites`（P08：混合检测与恢复、per-target 单条 undo、单选退化、剪贴板类型保持与一条 undo、收藏过滤叠加与重建保持）、`test_curve_editor`/`test_gradient_editor`/`test_property_grid_curve_gradient`（P09：文本归一、模式/切线/色标/预设手势、缩放预览 popup 实时预览、合并 undo、Esc 回滚、多目标事务）、`test_viewport_widget`（V01：渲染尺寸边沿触发、纹理跨 resize 保持、跨边界鼠标捕获与本地坐标、键盘、焦点取消、`vgui_input_cancel`、隐藏页不重建）、`test_viewport_camera`（V02：派生位置、平移/旋转/缩放与钳制、左键预留、焦点取消、总开关、F 取景、状态往返、多监听器与重复 attach）、`test_editor_context_multi_selection`/`test_viewport_selection`/`test_outliner_multi_selection_sync`（V03：集合语义/通知、点击/框选/修饰键/取消/坐标、Outliner 双向同步与反选）、`test_viewport_grid`/`test_snap_settings`（V04：自适应密度与像素范围、固定格距、线段数量/对齐/平面高度、主次线与轴配色方向、隐藏；平移/旋转/缩放吸附、总开关/单通道/零步长回退、shell 网格命令与 checked 状态）、`test_viewport_gizmo`（V05/V07：三轴/平面命中、轴约束与吸附、关闭吸附自由增量、平面 Z 约束、Esc/焦点旁落/显式取消、隐藏与无效帧、45° 局部轴方向、空间状态与 detach；旋转环命中/15° 吸附/跨 ±π 展开、轴向与统一缩放/0.1 吸附、模式与空间切换取消、变换载荷 begin 快照、数值文本往返与多选共享枢轴）、`test_viewport_drop_hover`（V06：拖拽前无悬停、悬停载荷/坐标、离开清除、Escape 取消、真实释放只触发一次）、`test_canvas_view`（V08：模型/显式 id/坐标、缩放与网格自适应、点击/Ctrl/Shift/框选、锁定/不可见过滤、手柄命中、移动/缩放/旋转吸附与 undo/redo、Esc/CANCEL/失焦回滚、多选移动、中键平移、滚轮锚点缩放、禁用、选择监听器与删除 prune；shell CANVAS 页与 `view.canvas` 命令）、`test_table_view`（T01：1 万行虚拟窗口/滚动绘制只查可见行、稳定排序与选择跟随、列宽/排序 JSON 校验与恢复、文本/整数/布尔/枚举/颜色/向量编辑、只读/非法输入/取消、undo/redo；布局与 shell 覆盖 TABLE 页、`view.table` 和 v3 表格状态保存/恢复）。
- M6 T03 NodeGraph：`node_graph_view` 以稳定 id 管理节点/端口/连线，支持类型/方向/单输入替换校验、节点拖动、端口连线、连线点选、空白框选、Ctrl/Shift 多选、锁定节点、中键平移与光标锚点缩放；Esc/CANCEL/失焦回滚，移动/连接/断开/删除/复制各记录一条命令。自适应网格与吸附、`Ctrl+F` 搜索高亮、小地图导航、右键 Add Node 宿主处理器和 `node_graph_state` JSON 全部接通；`editor_shell` 新增 NODEGRAPH 页、`view.nodegraph` 命令与 `editor_layout` v5 持久化。`vgui_game_editor` 以材质节点图 dogfood 拖动、连线、搜索和右键创建，`test_node_graph_view` 覆盖命中/撤销/极端缩放/状态校验。
- M6 T04 CodeEditor：`code_editor` 以按行 UTF-8 模型和惰性 token span 提供 C++/C#/GLSL/JSON/Lua/Python/INI/Markdown 语法高亮、行号、光标/选区/鼠标/键盘编辑、自动缩进、命令栈 undo/redo、系统剪贴板桥接与 `Ctrl+F`/`Ctrl+H` 查找替换；同一控件在只读模式下以 `append_log()` 提供带时间戳/等级颜色/保留上限/自动滚动的富文本日志。`open_file()`/`begin_incremental_load()` 分块加载并通过 `tick()` 推进，不阻塞 UI；`code_editor_state` JSON 由 `editor_layout` v6 持久化。`editor_shell` 新增 CODE 页与 `view.code` 命令，`vgui_game_editor` 预置 C++ 脚本示例，`test_code_editor` 覆盖词法/编辑/查找/日志/分块加载/状态校验。
- 国际化 / RTL：context 级文案表与语言切换、`vgui_direction` 方向切换（布局/文本/常用控件镜像）；`vgui_demo` 有可操作入口，`vgui_model_test` 覆盖切换与镜像布局 (E06，模型测试现 708 项)。
- M7 E05 无障碍：核心语义树（角色/名称/值/状态 + 预序可见遍历）、显式名称/角色覆盖、焦点陷阱与自动陷阱、F10 菜单、F1～F12 快捷键、焦点/值/状态/选中/公告事件与公告 API、高对比度主题；`editor_shell` 为外壳与各组件设置语义名称，新增 `F6`/`Shift+F6` 面板循环与 `Ctrl+Shift+H` 高对比度命令。`vgui_model_test` 的 `test_accessibility_e05` 与 `vgui_editor_kit_test` 的 `test_shell_accessibility_e05` 覆盖上述行为及 WCAG 对比度。
- M7 E03 截图回归：`sample/vgui_screenshot_test.cpp` 用无窗口 SDL_GPU/OVG 离屏管线渲染 9 个关键状态（Dock 默认/浮动、PropertyGrid 混合值/错误折叠只读、Outliner 深色选中/浅色过滤、AssetBrowser 网格/列表、Viewport 网格+Gizmo），基线保存在 `sample/screenshots/`；默认每通道容差 2、差异像素比例上限 0.05%，`--update` 重建基线，`--filter`/`--list` 便于本地调试，失败输出 `_actual.png` 与 `_diff.png`。根 CMake 启用 CTest，`vgui_screenshots` 可直接在 CI 运行，无 Vulkan 时返回 77 标记 SKIP。
- M7 E04 文档与示例：README 新增“游戏编辑器示例”的 MVP 主路径与常用操作、构建/运行/测试命令、国际化/RTL 与截图回归说明，并修正运行列表和项目结构；`vgui_game_editor.cpp` 顶部注释同步为当前完整 dogfood 范围。新用户可按 README 使用 vcpkg + CMake 构建并运行 `vgui_game_editor`。
- 现有 `vgui_model_test`（757 项，含树过滤/行矩形/编辑取消/焦点监听、context popup 层开关/外点/Esc/边缘钳制/焦点/互斥、E01 样式/字体/几何缩放与 100%/125%/150%/200% 列布局不重叠、E05 语义树/焦点陷阱/公告事件/高对比度对比度）、`vgui_c_api_test`（32 项，含 V01 附加焦点监听与 `vgui_input_cancel`、E01 纯 C UI 缩放/监听器/整树重算）、`vgui_texture_test`（79 项，含 V10 原生/CPU 首帧绘制、九宫格、旋转、矩阵、父级裁剪及组合）、`vgui_screenshot_test`（E03：9 个场景对比，同机 `diff=0`）保持通过；`vgui_editor_kit_test` 另覆盖 `test_shell_ui_scale` 的四档 DPI 外壳/停靠区布局与逻辑尺寸持久化往返；`vgui_editor --selftest` 覆盖 PropertyGrid 属性行构建、JSON 往返与代码生成。

下一步优先项：

1. V01～V10 已完成（真实 native texture 视口、resize/FBO 重建、跨边界输入、相机导航、点击/框选与多选同步、世界网格与吸附、平移/旋转/缩放 Gizmo 与撤销、资产拖入落点预览与创建、独立 2D Canvas 与命令栈/网格吸附、多视口/HDR/MSAA/深度纹理、`VGUI_IMAGE` 九宫格/旋转/矩阵），T01 Table/DataGrid、T02 Timeline、T03 NodeGraph、T04 CodeEditor、E01 DPI 缩放、E02 性能与增量更新、E03 截图回归、E04 文档与示例、E05 无障碍均已完成；当前路线图条目已全部收口，后续需求按新条目追加。
2. V08 2D Canvas 的画布/手柄/框选/吸附已复用于 T02 Timeline 与 T03 NodeGraph；T01 的拉取式行源与类型化单元格编辑器可被后续数据视图复用。
3. 第三批专业编辑器与打磨 E05 无障碍已完成；E03 的 9 张基线可作为后续 UI 改动的视觉回归入口，新增组件时按同目录约定补充 `sample/screenshots/<scene>.png` 并用 `--update` 生成。
