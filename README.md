# vgui 保留模式（retained-mode）GUI 库

vgui 是一个基于 [OVG](https://github.com/huahua369/ovg) 矢量渲染器的轻量级 **保留模式（retained-mode）GUI 库**。公共接口为纯 C（C99/C11），实现使用 C++23；内置控件树、Flexbox 风格布局、事件分发、主题系统和滚动面板，并通过 SDL3 后端完成窗口、输入与 GPU 渲染。

UI 侧维护控件树、布局和交互状态；绘制时遍历控件树，将矩形、圆角、文字、裁剪等矢量命令录制到 OVG，再交由 SDL3 GPU 后端提交渲染。`vgui` 自带的后端只实现 2D 所需的路径、渐变、文本图集、图片、矩形裁剪和 MSAA，并使用 SPIR-V（Vulkan）着色器；OVG 自带的完整渲染器仍保留在 `third_party/ovg` 供其示例使用。

> 当前 API 版本（`vgui_version()`）：`0.3.0` · 许可证：MIT · 示例主要在 Windows + MSVC + SDL3 环境下验证。
![项目截图](screenshot.png)
## 特性

| 模块 | 能力 |
|---|---|
| **公共接口** | 纯 C 头文件 `vgui.h`，不依赖 C++ 类型、模板或 glm；可直接从 C/C++ 或其他语言通过 FFI 调用 |
| **控件** | Panel、Label、Button、Checkbox、Radio、Switch、Slider（水平/垂直）、Progress、Edit、Separator、List、Tree、Menu、Splitter、TabView |
| **布局** | 绝对定位、横向/纵向布局；padding、margin、gap、min/max size、align、justify、grow；滚动面板 |
| **事件** | 鼠标移动/按下/抬起/滚轮、单击/双击/三击、进入/离开、键盘、文本输入、IME、值改变、提交；列表/树/菜单事件携带稳定 item id 和索引 |
| **主题与样式** | `vgui_theme` 颜色主题 + `vgui_style` 控件尺寸/间距/圆角/动画参数，均支持运行时切换；控件级背景、边框、圆角、文字颜色、透明度、字体大小；焦点环透明度 |
| **文本** | HarfBuzz 驱动的文本测量与渲染，UTF-8 文本，多字体族回退，水平/垂直对齐 |
| **滚动** | 面板横向/纵向滚动；滚轮、拖动滚动条、点击轨道、程序化滚动；支持嵌套滚动面板 |
| **渲染** | 基于 OVG 的矢量路径、圆角矩形、文本和裁剪；SDL3 GPU 后端 |
| **工程** | CMake + vcpkg 清单，附带完整控件演示和无需窗口/GPU 的自测程序 |

### 控件一览

| 控件 | 主要能力 |
|---|---|
| `VGUI_PANEL` | 容器；横向/纵向/绝对布局、裁剪、滚动、动态增删子控件 |
| `VGUI_LABEL` | 文本显示、字号、颜色、水平/垂直对齐 |
| `VGUI_BUTTON` | 单击/双击/三击事件、禁用态、自定义背景/边框/圆角 |
| `VGUI_CHECKBOX` / `VGUI_SWITCH` | 布尔值、选中状态、值改变回调 |
| `VGUI_RADIO` | 同一父节点下的单选组自动互斥 |
| `VGUI_SLIDER` | 水平/垂直方向、范围、步长、拖动和方向键调节 |
| `VGUI_PROGRESS` | 范围与当前值显示，可用于静态或动画进度 |
| `VGUI_EDIT` | 单行/多行文本编辑，详见下文 |
| `VGUI_SEPARATOR` | 分隔线 |
| `VGUI_LIST` | 数据模型列表；稳定 `vgui_item_id`、单选/多选、键盘导航、滚轮与滚动条 |
| `VGUI_TREE` | 数据模型树；展开/折叠、层级选择、键盘导航、可见行扁平化 |
| `VGUI_MENU` | 上下文菜单浮层；复选项、子菜单、快捷键文本、禁用项；打开时消费外部点击 |
| `VGUI_SPLITTER` | 双子面板分栏；水平/垂直、嵌套、比例、sash、最小尺寸、拖动改变比例、双击复位 |
| `VGUI_TABVIEW` | 标签页容器；关闭回调、未保存标记、禁用页、拖拽重排、滚轮溢出、键盘切换/关闭 |

### 文本编辑框

- 单行/多行编辑、占位符、密码掩码、只读模式。
- 鼠标拖拽选择、双击选词、三击选行；CJK 文本按词/字符边界处理。
- 复制、剪切、粘贴、撤销、重做；支持 `Ctrl+A/C/X/V/Z/Y`（`Ctrl+Shift+Z` 也可重做），并兼容 `Ctrl+Insert`、`Shift+Insert`、`Shift+Delete`。
- 键盘导航：方向键、Home/End、PageUp/PageDown、`Ctrl+左右` 按词移动、`Shift+方向键` 扩展选区。
- 多行编辑框内容滚动与内部滚动条，支持滚轮和拖动；内容宽度超出时自动显示水平滚动条。
- IME 组合文本显示、光标位置查询和输入框矩形上报。
- 文本光标宽度可配置，默认 1 像素；`vgui_edit_set_caret_width(edit, 2.0f)` 可加宽，设为 0 可隐藏。
- 选区高亮颜色与圆角可配置。多行选区按整体轮廓填充，共享边保持连续，所有外露拐角（包括行宽变化处的台阶/凹角）都会倒圆；`vgui_edit_set_selection_rounding(edit, 0)` 可关闭圆角。

### 列表、树与菜单

列表和树是**数据模型控件**：每一行是带稳定 `vgui_item_id` 的记录，而不是子控件；第一版只支持选择，不提供逐行复选框。两者复用面板的裁剪、滚动条和滚轮逻辑，并支持单选/多选、键盘导航、空状态文本和程序化滚动。

```c
vgui_widget* list = vgui_widget_add(root, VGUI_LIST, NULL);
vgui_item_id item = vgui_list_add_item(list, "打开文件", NULL);
vgui_list_set_item_detail(list, item, "Ctrl+O");
vgui_list_set_selection_mode(list, VGUI_SELECTION_SINGLE);
vgui_list_on_select(list, on_list_select, NULL);
vgui_list_on_activate(list, on_list_activate, NULL); /* 双击/回车 */

vgui_widget* tree = vgui_widget_add(root, VGUI_TREE, NULL);
vgui_item_id dir = vgui_tree_add_node(tree, 0, "src", NULL);
vgui_tree_add_node(tree, dir, "main.cpp", NULL);
vgui_tree_set_item_expanded(tree, dir, 1);
```

选择回调签名为 `vgui_item_select_fn(widget, item, selected, user)`；激活回调为 `vgui_item_fn(widget, item, user)`。普通事件中的 `vgui_event.item` 和 `vgui_event.index` 也会填入对应的模型项与索引。

菜单是上下文级浮层，不挂到布局子控件中；一个 `vgui_context` 同时只保留一条打开的菜单链。菜单项同样是模型记录，支持普通项、复选项、分隔线、禁用项和子菜单。点击菜单外部会关闭菜单并消费该次鼠标按下/抬起，不会穿透到下面的控件。

```c
vgui_widget* menu = vgui_widget_create(ui, VGUI_MENU, NULL);
vgui_menu_add_check_item(menu, "自动保存", 1);
vgui_item_id open = vgui_menu_add_item(menu, "打开设置");
vgui_menu_set_item_shortcut(menu, open, "Ctrl+,");
vgui_menu_add_separator(menu);

vgui_widget* more = NULL;
vgui_menu_add_submenu(menu, "更多", &more);
vgui_menu_add_item(more, "关于 vgui");

vgui_menu_on_activate(menu, on_menu_activate, NULL);
vgui_menu_open_below(menu, button);
```

打开菜单时 `vgui_context_wants_mouse()` / `vgui_context_wants_keyboard()` 返回 1，`vgui_context_wants_text_input()` 返回 0；Esc 关闭当前子菜单，重复按 Esc 关闭整条链。当前版本不绘制菜单图标。

### Splitter 与 TabView

`VGUI_SPLITTER` 是双子面板容器：`vertical=0` 左右分栏，`vertical=1` 上下分栏。`ratio` 表示第一个可见子面板在主轴上所占比例，拖动 sash 修改比例，双击 sash 复位；子面板的 `min_w` / `min_h` 会约束最终尺寸。`vgui_widget_on_value()` 可监听拖动后的比例变化。

```c
vgui_widget* splitter = vgui_widget_add(root, VGUI_SPLITTER, NULL);
vgui_splitter_set_vertical(splitter, 0);
vgui_splitter_set_ratio(splitter, 0.25f);
vgui_widget_add(splitter, VGUI_PANEL, NULL);
vgui_widget_add(splitter, VGUI_PANEL, NULL);

vgui_widget* tabs = vgui_widget_add(root, VGUI_TABVIEW, NULL);
vgui_widget* page = vgui_widget_create(ui, VGUI_PANEL, NULL);
vgui_item_id doc = vgui_tab_add(tabs, "Scene", page);
vgui_tab_set_unsaved(tabs, doc, 1);
vgui_tab_on_close(tabs, on_tab_close, NULL);
```

TabView 负责页面可见性和生命周期：`vgui_tab_add()` 接管页面，移除标签会销毁页面。设置 `vgui_tab_on_close()` 后由宿主决定是否调用 `vgui_tab_remove()`（便于未保存确认）；未设置回调时可直接关闭。标签支持禁用、未保存标记、关闭按钮、鼠标拖拽重排、滚轮横向溢出和键盘切换/关闭（`Ctrl+W`），右键会发送带 `item` 的鼠标事件供宿主打开菜单。

### 交互与焦点

- Tab / Shift+Tab 切换焦点，鼠标点击自动聚焦，可设置 `focusable`。
- 悬停、按下、选中等状态动画；可设置鼠标指针形状和禁用态。
- 命中测试、嵌套控件事件分发，输入函数会返回事件是否被消费。

## 快速开始

### 环境要求

- CMake 3.21+（项目最低要求 3.16，仓库中的 `CMakePresets.json` 需要 3.21+）
- C++23 编译器（MSVC 2022 或更新版本）
- vcpkg（推荐），仓库已包含 `vcpkg.json`
- 主要依赖：SDL3、HarfBuzz、Fontconfig、glm、zlib、Clipper2、EnTT（示例使用）
- OVG 以 git submodule 形式放在 `third_party/ovg`

### 构建

```bash
git clone --recursive https://github.com/huahua369/vgui.git
cd vgui

# 在 Visual Studio 开发者命令行中运行；需要 vcpkg 依赖可用
cmake --preset x64-debug
cmake --build out/build/x64-debug
```

如果克隆时没有带上子模块，执行 `git submodule update --init --recursive`。

如果 CMake 找不到 vcpkg 依赖，可显式指定工具链：

```bash
cmake --preset x64-debug -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
cmake --build out/build/x64-debug
```

发布构建使用 `x64-release`。`CMakePresets.json` 中另有 Linux/macOS 的配置骨架；跨平台构建需自行适配系统依赖和示例代码。

### 运行示例

Debug 输出目录为 `out/bin/x64d/`：

```text
out/bin/x64d/vgui_demo.exe             # 全部控件的交互演示
out/bin/x64d/vgui_edit_test.exe        # 编辑框、滚动、主题、焦点等无需窗口/GPU 的行为自测
out/bin/x64d/vgui_model_test.exe       # 列表、树、菜单模型与输入行为自测
out/bin/x64d/vgui_editor_kit_test.exe  # editor kit 骨架与命令栈自测
out/bin/x64d/vgui.exe                  # OVG 场景示例
```

`vgui_demo` 中可以体验按钮、选择控件、滑杆、进度条、编辑框、列表、树、菜单、滚动面板、动态增删控件、主题切换、焦点透明度和选区圆角等功能。

## 最小示例

假设 `ui` 已经通过 `vgui_create(font, 16.0f)` 创建：

```c
#include "vgui.h"

static void on_click(vgui_widget* widget, const vgui_event* event, void* user)
{
    (void)widget;
    (void)user;
    if (event->type == VGUI_EVENT_CLICK)
    {
        /* 处理点击 */
    }
}

void build_ui(vgui_context* ui)
{
    vgui_context_set_size(ui, 800, 600);

    /* 先取默认样式，只改需要调整的字段；未修改的字段保持当前外观。 */
    vgui_style style;
    vgui_style_default(&style);
    style.button.rounding = 8.0f;
    style.button.padding_x = 16.0f;
    style.edit.scrollbar_width = 6.0f;
    style.animation_speed = 12.0f;          /* 0 = 关闭悬停/勾选动画 */
    vgui_context_set_style(ui, &style);

    vgui_widget* root = vgui_root(ui);
    vgui_widget_set_layout(root, VGUI_LAYOUT_COLUMN);
    vgui_widget_set_padding(root, 16, 16, 16, 16);
    vgui_widget_set_gap(root, 8);

    vgui_widget_add(root, VGUI_LABEL, "Hello vgui");

    vgui_widget* button = vgui_widget_add(root, VGUI_BUTTON, "Click me");
    vgui_widget_on_event(button, on_click, NULL);

    vgui_widget* edit = vgui_widget_add(root, VGUI_EDIT, "");
    vgui_edit_set_multiline(edit, 1);
    vgui_edit_set_text(edit, "第一行\n第二行");
    vgui_edit_set_caret_width(edit, 2.0f);       /* 默认 1 像素，0 = 隐藏 */
    vgui_edit_set_selection_rounding(edit, 4.0f); /* 0 = 方角，>0 = 圆角半径 */
}
```

每帧更新和绘制：

```c
vgui_update(ui, delta_time);
vgui_draw(ui, ovg_cb, rvg);
```

字体、OVG 回调和 SDL3 窗口的初始化由后端/宿主程序负责，完整流程见 `sample/vgui_demo.cpp`。颜色格式统一为 `0xAARRGGBB`。

`vgui_style` 覆盖面板、标签、按钮、复选框、单选、开关、滑杆、进度条、编辑框、列表、树、菜单、焦点环、滚动条和动画参数；`vgui_context_set_style()` 会同步迁移仍在使用旧默认值的已有控件，控件级显式设置保持不变。`vgui_theme` 新增 `scrollbar_hover`、`checkmark`、`switch_thumb` 三个颜色字段，以及列表/树/菜单使用的 `list_bg`、`row_text`、`row_hover`、`row_selected`、`menu_bg`、`menu_hover` 等字段；旧主题把这些字段留为 0 时会自动回退到面板/文字/按钮色，不会破坏现有主题。列表/树的自动行高和菜单的自动最大高度也可以通过样式字段设为 0 来保持自动计算。

## 项目结构

```text
gui/
  vgui.h              公共纯 C API
  vgui.cpp            控件树、布局、事件与绘制实现（含 Splitter/TabView）
  vgui_internal.h     内部 C++ 接口（兼容旧版）
  vgui_legacy.cpp     旧版 C++ 接口适配
  editor/             vgui_editor_kit 编辑器组件层（MVP 骨架）
backends/
  vgui_sdl3.*          SDL3 窗口、输入、IME 与 GPU 视口
  vgui_renderer_sdl3.* vgui 专用 SDL3 GPU 2D 矢量后端
  mapView.*           视图/地图相关后端组件
third_party/
  ovg/                OVG 矢量渲染器（git submodule）
sample/
  vgui_demo.cpp             全部控件的交互演示
  vgui_edit_test.cpp        编辑框、滚动、主题、焦点自测
  vgui_model_test.cpp       列表、树、菜单模型自测
  vgui_editor_kit_test.cpp  editor kit 骨架与命令栈自测
  main.cpp                  OVG 场景示例
docs/
  phase0_3d_editor.md 3D 场景编辑器基础与图片/纹理能力
  editor_roadmap.md   游戏编辑器组件路线图
```

公共 API 和控件相关说明集中在 `gui/vgui.h`；内部 C++ 声明在 `gui/vgui_internal.h`。

## 路线图

游戏编辑器方向的组件分层、优先级、依赖和验收标准见 [`docs/editor_roadmap.md`](docs/editor_roadmap.md)。Phase 0 的图片/原生纹理基础见 [`docs/phase0_3d_editor.md`](docs/phase0_3d_editor.md)。

## 测试

```bash
out/bin/x64d/vgui_edit_test.exe
out/bin/x64d/vgui_model_test.exe
out/bin/x64d/vgui_editor_kit_test.exe
```

自测覆盖编辑框文本输入、选区、剪贴板、撤销/重做、多行滚动、内部滚动条拖动、面板滚动条、主题切换内容尺寸稳定性、焦点透明度等行为；模型测试覆盖列表/树的稳定 id、插入删除、单选/多选、展开折叠、键盘导航、菜单项激活/复选/子菜单、外部点击消费，以及 Splitter 的比例/最小尺寸/拖动/复位和 TabView 的切换/关闭/禁用/重排/溢出/键盘操作。`vgui_editor_kit_test` 覆盖 editor kit 的嵌套 Splitter + TabView Shell、EditorContext 选择、Outliner/PropertyGrid/AssetBrowser 占位组件、Viewport 图片边界以及命令栈的 undo/redo/事务/回滚。失败时返回非零退出码，适合接入 CI。

## 第三方与许可证

- 项目本体使用 MIT License，详见 [`LICENSE.txt`](LICENSE.txt)。
- 矢量渲染器 OVG 位于 `third_party/ovg`，遵循其仓库中的许可证。
- 完整依赖清单见 [`vcpkg.json`](vcpkg.json)。

## 链接

- 仓库：<https://github.com/huahua369/vgui>
- OVG：<https://github.com/huahua369/ovg>
