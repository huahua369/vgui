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
| 普通应用 | ★★★☆ 控件/布局/i18n 强，配套弱 | 控件齐全，但后端、对话框、富文本、无障碍缺位 |

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
- **图表**：折线/面积/柱/径向仪表盘/JSON 仪表板，十万点 min-max 降采样、十字准线、悬浮提示、图例、平移缩放、CSV 导出

### 2.2 缺的功能 / 短板
- **文件打开/保存对话框未暴露**（仅桥接 OS 文件*拖入*）；需自建或借 SDL3 对话框 API
- **富文本 / 语法高亮 / 行号 / 查找替换**：核心文本编辑明确缺失（CodeEditor 在 kit 中，但高亮深度未详述、未收口到核心）
- **原生菜单**：菜单无图标、无动态重建、无原生菜单
- **跨平台后端有限**：唯一后端 SDL3 GPU；**无 iOS / Android、无 WebAssembly / WebGPU 导出、无游戏主机**后端
- **无障碍（Accessibility）**：仅键盘/手柄/焦点环 + 高对比主题，**无屏幕阅读器 / ARIA 语义**
- **无数据绑定框架**：靠每帧直写控件状态（runtime 已给出"绑定惯例"，但非响应式）
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

### 4.2 缺的功能 / 短板
- **Dock 标签图标与右键上下文菜单**：路线图明确仍缺（未实现）
- 菜单图标 / 动态重建 / 原生菜单：仍缺
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
3. **无响应式/声明式数据绑定**，UI 状态需手动同步
4. **无多线程 UI**，单渲染线程约束
5. **无原生文件对话框**（仅文件拖入）
6. **无高级富文本/排版**（核心层）
7. **无 WebView / 多媒体 / 网络**
8. **C API 优先**：暂无 C# / Python 等语言绑定（C 接口便于自建，但非开箱）
9. **文档为路线图式**，无完整可浏览 API 参考；版本 0.3.0 仅源码兼容
10. **无插件/扩展体系**

---

## 6. 结论

- **游戏编辑器 / 开发工具**：vgui 最强、最现成的选择，组件几乎齐全，可直接 dogfood，`vgui_game_editor` 已验证全流程。
- **游戏运行时 HUD / 游戏内 UI**：能力完备且经过基准验证，但只适合"引擎拥有主循环与渲染器、vgui 负责控件树与绘制录制"的嵌入式模式；移动端 / Web / 跨引擎需额外接 `ovg_draw_data_t`。
- **普通桌面应用**：控件、布局、主题、i18n、图表都能打，但**文件对话框、富文本、无障碍、移动/Web 后端**是明显缺口；若要替代 Qt / Flutter 需自行补齐这些配套。

### 优先级建议（若要补齐普通应用短板）
1. 暴露 SDL3 文件打开/保存对话框封装（低成本、高收益）
2. 核心层补富文本/语法高亮/行号/查找替换，或把 kit 的 CodeEditor 能力下沉
3. 增加屏幕阅读器/ARIA 语义（无障碍合规）
4. 评估 WebAssembly / WebGPU 与移动端后端需求（若目标平台包含 Web/移动）
5. 提供可选的响应式数据绑定层
