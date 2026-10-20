# vgui 保留模式（retained-mode）GUI 库

vgui 是一个基于 [OVG](https://github.com/huahua369/ovg) 矢量渲染器的轻量级 **保留模式（retained-mode）GUI 库**。公共接口为纯 C（C99/C11），实现使用 C++23；内置控件树、Flexbox 风格布局、事件分发、主题系统和滚动面板，并通过 SDL3 后端完成窗口、输入与 GPU 渲染。

UI 侧维护控件树、布局和交互状态；绘制时遍历控件树，将矩形、圆角、文字、裁剪等矢量命令录制到 OVG，再交由 SDL3 GPU 后端提交渲染。

> 当前 API 版本（`vgui_version()`）：`0.2.0` · 许可证：MIT · 示例主要在 Windows + MSVC + SDL3 环境下验证。

## 特性

| 模块 | 能力 |
|---|---|
| **公共接口** | 纯 C 头文件 `vgui.h`，不依赖 C++ 类型、模板或 glm；可直接从 C/C++ 或其他语言通过 FFI 调用 |
| **控件** | Panel、Label、Button、Checkbox、Radio、Switch、Slider（水平/垂直）、Progress、Edit、Separator |
| **布局** | 绝对定位、横向/纵向布局；padding、margin、gap、min/max size、align、justify、grow；滚动面板 |
| **事件** | 鼠标移动/按下/抬起/滚轮、单击/双击/三击、进入/离开、键盘、文本输入、IME、值改变、提交 |
| **主题与样式** | `vgui_theme` 颜色主题，运行时切换；控件级背景、边框、圆角、文字颜色、透明度、字体大小；焦点环透明度 |
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

### 文本编辑框

- 单行/多行编辑、占位符、密码掩码、只读模式。
- 鼠标拖拽选择、双击选词、三击选行；CJK 文本按词/字符边界处理。
- 复制、剪切、粘贴、撤销、重做；支持 `Ctrl+A/C/X/V/Z/Y`（`Ctrl+Shift+Z` 也可重做），并兼容 `Ctrl+Insert`、`Shift+Insert`、`Shift+Delete`。
- 键盘导航：方向键、Home/End、PageUp/PageDown、`Ctrl+左右` 按词移动、`Shift+方向键` 扩展选区。
- 多行编辑框内容滚动与内部滚动条，支持滚轮和拖动。
- IME 组合文本显示、光标位置查询和输入框矩形上报。
- 选区高亮颜色与圆角可配置。多行选区按整体轮廓填充，共享边保持连续，所有外露拐角（包括行宽变化处的台阶/凹角）都会倒圆；`vgui_edit_set_selection_rounding(edit, 0)` 可关闭圆角。

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
out/bin/x64d/vgui_demo.exe        # 全部控件的交互演示
out/bin/x64d/vgui_edit_test.exe   # 编辑框、滚动、主题、焦点等无需窗口/GPU 的行为自测
out/bin/x64d/vgui.exe             # OVG 场景示例
```

`vgui_demo` 中可以体验按钮、选择控件、滑杆、进度条、编辑框、滚动面板、动态增删控件、主题切换、焦点透明度和选区圆角等功能。

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
    vgui_edit_set_selection_rounding(edit, 4.0f); /* 0 = 方角，>0 = 圆角半径 */
}
```

每帧更新和绘制：

```c
vgui_update(ui, delta_time);
vgui_draw(ui, ovg_cb, rvg);
```

字体、OVG 回调和 SDL3 窗口的初始化由后端/宿主程序负责，完整流程见 `sample/vgui_demo.cpp`。颜色格式统一为 `0xAARRGGBB`。

## 项目结构

```text
gui/
  vgui.h              公共纯 C API
  vgui.cpp            控件树、布局、事件与绘制实现
  vgui_internal.h     内部 C++ 接口（兼容旧版）
  vgui_legacy.cpp     旧版 C++ 接口适配
backends/
  vgui_sdl3.*         SDL3 窗口、输入、IME 与 GPU 视口
  mapView.*           视图/地图相关后端组件
third_party/
  ovg/                OVG 矢量渲染器（git submodule）
sample/
  vgui_demo.cpp       全部控件的交互演示
  vgui_edit_test.cpp  编辑框、滚动、主题、焦点自测
  main.cpp            OVG 场景示例
```

公共 API 和控件相关说明集中在 `gui/vgui.h`；内部 C++ 声明在 `gui/vgui_internal.h`。

## 测试

```bash
out/bin/x64d/vgui_edit_test.exe
```

自测覆盖编辑框文本输入、选区、剪贴板、撤销/重做、多行滚动、内部滚动条拖动、面板滚动条、主题切换内容尺寸稳定性、焦点透明度等行为；失败时返回非零退出码，适合接入 CI。

## 第三方与许可证

- 项目本体使用 MIT License，详见 [`LICENSE.txt`](LICENSE.txt)。
- 矢量渲染器 OVG 位于 `third_party/ovg`，遵循其仓库中的许可证。
- 完整依赖清单见 [`vcpkg.json`](vcpkg.json)。

## 链接

- 仓库：<https://github.com/huahua369/vgui>
- OVG：<https://github.com/huahua369/ovg>
