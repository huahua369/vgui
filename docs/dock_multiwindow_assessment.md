# Dock 多窗口能力评估

评估日期：2026-11-04
范围：`libvgui` core（`gui/vgui.cpp` / `include/vgui/vgui.h`）、SDL3 后端（`backends/` / `include/vgui/backend/`）、editor kit（`gui/editor/` / `include/vgui/editor/`）、sample 宿主
方法：静态代码审计 + 运行时探针（临时插入 `sample/vgui_model_test.cpp` 的 headless 用例，验证后已按字节还原）

> 本文只做评估，不改动任何实现。所有行号对应本次评估时的源码。
>
> **实施更新（2026-11-04）**：目标 A 已落地，见 §11。§1–§10 保留评估时的原始判断作为依据；涉及「未接线 / 零覆盖 / 缺 API」的表述以 §11 为准。

---

## 1. 结论速览

「dock 多窗口」需先拆成三种目标，它们的可行性差别极大：

| 目标 | 含义 | 可行性 | 阻塞层 | 改动量 | 实施状态 |
|---|---|---|---|---|---|
| **A. 多窗口并存** | 一个进程开多个 OS 窗口，每窗口各有一棵 widget 树 / 一个 Dock，互不拖放面板 | **高** | 宿主循环、editor kit 多实例 | 中 | **已完成**（§11） |
| **B. 浮动面板 → 真 OS 窗口** | 面板可浮出为主窗口之外的独立原生窗口（可移出主窗口、可跨屏） | **中** | core Dock 数据模型 | 大 | 未实施 |
| **C. 跨窗口拖放停靠** | 面板可在不同 OS 窗口的 Dock 之间拖动、互相合并标签组 | **低** | A + B 全部，外加跨窗口 drag 协议 | 很大 | 未实施 |

按架构分层的判定：

| 层 | 判定 | 依据 |
|---|---|---|
| SDL3 后端 / 渲染器 | **已具备多窗口骨架** | `app_mgr::windows_` 是窗口向量，`create()/create2()/find(windowID)`、`process_event` 按 windowID 路由、每窗口独立 `gui_viewport`（含独立 `vgui_context`）与 `vg_fbo_t`；`vgpu_render_frame` 已支持一帧内多 FBO 分 pass 提交 |
| libvgui core `VGUI_DOCK` | **单窗口模型，B/C 的硬性阻塞** | `VGUI_DOCK_FLOATING` 是自绘浮层且被夹在 dock 矩形内；面板 id 是 per-dock 局部自增；`page` 是裸 `vgui_widget*` 且 `vgui_dock_add()` 显式拒绝跨 context |
| editor kit / 布局持久化 | **假定单根窗口、单份布局** | `editor_shell` 单一 `context_/root_/dock_`；`editor_layout` 单份 `dock_panels` / `dock_active` / zone 尺寸；JSON 只有单个 `root["dock"]` |
| 文档与测试 | **零覆盖** | 三份文档均未讨论多窗口；dock 测试全部是单 context / 单窗口 |

一句话：**「多 OS 窗口并存」在后端已经可行，缺的是宿主渲染循环、每窗口独立 io 与 editor kit 的多实例布局；「Dock 浮动面板变真窗口 / 跨窗口停靠」被 core 的数据模型从根上挡住，需要改 `VGUI_DOCK` 本身。**

---

## 2. 现状：`VGUI_DOCK` 到底是什么

### 2.1 数据模型

`gui/vgui.cpp:364-430` 的 `vgui_dock_state` 是**扁平四区 + 每区标签组 + 浮动列表**，不是可嵌套的 split 树：

```cpp
std::vector<vgui_dock_panel> panels;              // page 是裸 vgui_widget*
std::unordered_map<vgui_item_id, size_t> index;
std::vector<vgui_item_id> order[4];              // LEFT/RIGHT/BOTTOM/CENTER 各一个标签组
std::vector<vgui_item_id> float_order;           // 浮动层叠序
vgui_item_id active[4] = {};
vgui_item_id next_id = 1;                        // 每个 dock 局部自增
int drag_mode = 0;                               // 拖拽状态挂在这个 dock 上
vgui_dock_drop_target drop_targets[5];
float zone_x[4], zone_y[4], zone_w[4], zone_h[4];
```

布局是 `gui/vgui.cpp:5481-5534` 的固定四区算术：`left_w / right_w / bottom_h` + 中央剩余，没有递归节点。状态是 `vgui_widget` 的成员（`gui/vgui.cpp:634` `std::unique_ptr<vgui_dock_state> dock;`），**不是全局单例** —— 这点是好消息，多个 dock 实例彼此不干扰。

### 2.2 `VGUI_DOCK_FLOATING` 不是窗口

`include/vgui/vgui.h:1350-1358` 已经写明：

> Floating frames are confined to the dock rectangle.

实现证据：

- `gui/vgui.cpp:2688-2712` `vgui_dock_clamp_rect()` 把浮框宽高夹到 dock 内容矩形内，坐标夹到 `[bx, bx+bw-fw]`。
- `gui/vgui.cpp:2714-2737` `vgui_dock_float_internal()` 只做 `zone = FLOATING`、`open = 1`、`page->focus_trap = 1`、`clamp_rect`、压入 `float_order` —— **没有任何窗口创建调用**。
- `gui/vgui.cpp:13945-13954` 绘制时用 `vgui_clip_push` + `vgui_draw_widget(panel->page, ...)` 把页面画进**同一棵 widget 树**。

因此 README 与 `docs/editor_roadmap.md:239` 里的「独立 CODE 窗口 / 浮出窗口 / 拖动浮动框架标题可移动窗口」指的都是**主窗口内的自绘浮层**，不是 OS 窗口。措辞容易误导，但不构成实现缺陷。

### 2.3 API 面

`include/vgui/vgui.h:1379-1435` 共 37 个 `vgui_dock_*` API，句柄全部是 `vgui_widget* dock`，**没有任何 window / context / 屏幕坐标参数**。矩形语义（`vgui.h:1322/1600/2044/2100`）统一是「窗口内绝对坐标」，`vgui.h:9` 明确 "Coordinates are expressed in window pixels"。

### 2.4 拖拽

`gui/vgui.cpp:9600-9830`（事件）、`2515-2620`（drop target 计算）、`13930-14027`（罗盘与拖影绘制）。`drag_mode` 1→2→3 的状态机挂在「当前 active 的 dock widget」上，`drop_targets[5]` 是四区 + 中央浮出方块。全部是同一 context 内的命中测试（`vgui_dock_floating_at_point` / `vgui_dock_tab_at_point`），没有跨窗口协议。

---

## 3. 后端层：多窗口骨架其实已经存在

这一层是本评估最正面的发现，且比「只创建了窗口」更强：

| 能力 | 位置 | 状态 |
|---|---|---|
| 多窗口生命周期 | `include/vgui/backend/vgui_sdl3.h:220` `std::vector<std::unique_ptr<os_window>> windows_`；`:227-236` `create / create2 / find / destroy / window_count / windows`；`backends/vgui_sdl3.cpp:592/654/659` `SDL_CreateWindow` / `SDL_CreatePopupWindow` | 已实现 |
| 按窗口路由事件 | `backends/vgui_sdl3.cpp:936-941` `find(e->window.windowID)` → `active_viewport_ = pw->viewport`；`gui_viewport::trigger()` 把事件派发到 `viewport->ui` | 已实现 |
| 每窗口独立 UI 树与输入 | `vgui_sdl3.h:75-103` `gui_viewport { gui_io_state_t io; vgui_context* ui; ... drop_files; }`；`backends/vgui_sdl3.cpp:42-49` 析构时各销毁自己的 ctx | 已实现 |
| 每窗口独立渲染目标 | `include/vgui/backend/vgui_renderer_sdl3.h:29` `vg_fbo_t { SDL_Window* window; swapchain; ... }`；`backends/vgui_renderer_sdl3.cpp:1373-1398` `vgpu_create_fbo(..., window)` 为每个窗口建 swapchain FBO | 已实现 |
| **一帧内多 FBO 提交** | `backends/vgui_renderer_sdl3.cpp:1636-1660` 注释与实现：`ovg_draw_data_t` 每条自带 `fbo`，`vgpu_render_frame` 按 FBO 首次出现顺序「Encode one pass per FBO」，在同一个 command buffer 里依次编码 | **已实现** |
| `vgui_context` 可多实例 | `include/vgui/vgui.h:914-915` `vgui_create()/vgui_destroy()`；`gui/vgui.cpp:757-903` 输入、拖拽、字体、纹理、样式全在实例内；`vgui_draw()` 的剪裁栈是函数局部变量 | 已实现 |

`app_mgr` 里共享但**不阻止**多窗口的部分：`device / vgdev / vgctx / font_ctx / monitors`（进程级一个，一个 GPU device 服务多个 swapchain 属正常设计）、`_mouse_key`（`:193`，所有 `io.mks` 指向它）、`active_viewport_`（`:194`）、`main_window`（`:166`）、`docking_mgr`（`:152-159`）。

两点需要修正常见的误判：

1. `active_viewport_` 不是「全局当前窗口锁」。`io()` 的唯一调用者是 `process_event`（`backends/vgui_sdl3.cpp:942`），它**逐事件**被设置为事件所属窗口，然后立刻把 `dev.io` 指向该窗口的 io。多窗口下事件分发是安全的。
2. `vgpu_acquire_swapchain(ctx, fbo, count, wait)`（`vgui_renderer_sdl3.cpp:1541-1581`）要求 `fbo + i` 是**连续数组**，且 `ctx->current_cmd` 只有一份；第二次调用会先提交第一次的 command buffer。宿主**不能**简单地「对每个窗口各调一次 acquire、各画一次」。但 `vgpu_render_frame` 内部有 `ensure_swapchain()` 兜底（`1623-1626`），所以正确姿势是：一帧内收集所有窗口的 draw data（`draw_datas_cx::set(&win->fbo); add(vg);`，`backends/vgui_sdl3.cpp:1414-1427` 允许逐条切换 fbo），最后一次 `render()` 提交。

### 3.1 仍未接线 / 单窗口假定

- `docking_mgr::viewports_enabled` 默认 `false`，`set_viewports_enabled()` **全仓库无调用处**（只在 `vgui_sdl3.cpp:959/1117/1136` 三处被读取）。它是「鼠标坐标加窗口位置 → 全局屏幕坐标」的开关，正是跨窗口拖拽需要的能力，但目前是死代码。
- `sample/vgui_demo.cpp:3238/3264/3267/3283/3316/3320` 主循环硬编码单 `form->fbo`、单 `st.ui`、单 `rvg_t* vg`、单 `draw_datas_cx`。`vgui_game_editor.cpp` 同理。
- `backends/vgui_sdl3.cpp:1225-1229` `pump_gamepad()` 固定用 `main_window->viewport->ui`。
- `backends/vgui_sdl3_dialog.cpp:76-83` 模态父窗口取 `SDL_GetKeyboardFocus()` / `SDL_GetMouseFocus()` 全局焦点。
- `backends/vgui_sdl3.cpp:601/670` 所有 `io.mks = &_mouse_key`，按键/修饰键状态跨窗口共享。

---

## 4. core 层的硬阻塞（B/C 目标）

| # | 阻塞点 | 证据 |
|---|---|---|
| 1 | **跨 context 页面被显式拒绝** | `gui/vgui.cpp:17003` `if (page->ctx != dock->ctx ...) return 0;` —— `vgui_dock_add()` 会拒绝任何不属于本 context 的 page |
| 2 | **面板 id 是 per-dock 局部的** | `gui/vgui.cpp:398` `next_id = 1`；`17019-17023` `id = st->next_id++`。两个 dock 的首个面板 id 都是 1，跨 dock 传 id 会命中错误面板 |
| 3 | **浮动被几何夹取** | `gui/vgui.cpp:2688-2712`；`FLOATING` 只是 `vgui_dock_zone` 的第 5 个枚举值（`vgui.h:1359-1365`），不是窗口类型 |
| 4 | **拖拽状态与命中测试是单 context 的** | `gui/vgui.cpp:830-839` `drag_source` / `drop_hover` 是 `vgui_context` 内的裸 `vgui_widget*`；`9802` `ctx->alive.count(a)` 校验；`18284-18313` `vgui_input_drop_files` 只在 `ctx` 的树内 `vgui_drop_target_at(ctx,x,y)` |
| 5 | **输入 API 无窗口概念** | `include/vgui/vgui.h:1937-1952` 全部 `vgui_input_*(ctx, x, y, ...)`；`:2247-2253` "libvgui owns no windowing code"。坐标是窗口局部，没有屏幕坐标或 window id |

第 1、2 条是「所有权 + 标识」层面的，无法靠后端适配绕过，必须改 `VGUI_DOCK` 的模型或新增一层跨窗口管理层。

---

## 5. 运行时探针实证

临时在 `vgui_model_test.cpp` 加了一个 headless 用例（两个 `vgui_context` 各挂一个 Dock，共用同一个 `font_familys_t`），验证后已还原文件。输出：

| 探针 | 结果 | 结论 |
|---|---|---|
| 两个 context 各建一个 Dock 并各加一个面板 | `idA=1 idB=1 (numeric collision=1)` | 多 context / 多 dock 实例可并存；字体缓存可跨 context 共享 |
| 用 `idA` 去查 `dockB` 的标题 | `PB` | **id 碰撞导致跨 dock 误引用**，不是「查不到」而是「查到错误的面板」 |
| 把 ctxB 的 page 加进 ctxA 的 dock | `id=0` | 跨 context 迁移被 core 明确拒绝 |
| `vgui_dock_float(da, ia, 5000, 5000, 400, 300)` | `rect=(500,400,400,300)`，dock=900×700 | 浮框被夹到 dock 右下角，**无法移出 dock 矩形**（更不可能到另一块屏幕） |
| `detach()` 出的 page 直接加进 dockB | `id=0` | 页面所有权仍绑原 context，detach 不等于「可移植」 |

结论：A 目标的基础（多 context 并存）在 core 层没有障碍；B/C 目标的障碍是模型级的，不是渲染级的。

---

## 6. editor kit 与布局持久化

- `include/vgui/editor/editor_context.h:81` 绑定单个 `vgui_context*`；`include/vgui/editor/editor_shell.h:303-313` 单一 `context_ / root_ / dock_`。
- `gui/editor/editor_shell.cpp:786/795/842` build 时单链创建 `root_` → 单个 `dock_`，`:849-996` 往这唯一一个 dock 加 10 个面板。
- `include/vgui/editor/editor_layout.h:51-110` 单份 `dock_left_size / dock_right_size / dock_bottom_size`、`std::vector<editor_dock_panel_layout> dock_panels`、`std::array<std::string,4> dock_active`；`:41-43` 浮动矩形注释明确是「绝对像素、窗口内」。
- `gui/editor/editor_layout.cpp:155-180` 写**单个** `root["dock"]`，`:259-283` 读单对象。JSON 版本 7，无 window id / 窗口列表 / 多 dock 数组。

即：即使后端开了第二个窗口，kit 也没有「第二份布局」可存。

---

## 7. 文档与测试覆盖

- `README.md:225-254`「Dock 与浮动面板」「编辑器布局持久化」：描述准确（四区/标签组/浮动框架），但全文无 `多窗口 / 独立窗口 / 跨窗口 / OS window`。
- `docs/editor_roadmap.md:205-239`：S06/S09/S10 记录 dock 与 drop compass；`:239`「浮成独立 CODE 窗口」是自绘浮层的措辞，建议改成「浮动框架」以免误读。
- `docs/vgui_capability_assessment.md:93` 唯一 dock 短板是「标签图标与右键上下文菜单」；`:108` 的「单一渲染/OS 后端」指后端种类，不是单一窗口。**两份文档都没有把「浮动面板不是真 OS 窗口 / 无多窗口」记为能力缺口**。
- 测试：`sample/vgui_model_test.cpp` 的 `test_dock` / `test_dock_bottom_roundtrip` / `test_dock_float_square`；`sample/vgui_screenshot_test.cpp` 的 `dock_default_dark` / `dock_floating_dark` / `dock_drag_dark`；`vgui_editor_kit_test.cpp` 的布局往返。全部单 context、单窗口、headless 或离屏。**没有任何多窗口 / 跨窗口用例。**

---

## 8. 三种目标的实现路径

### 目标 A：多窗口并存（推荐先做）

core 的 `gui/vgui.cpp` **基本不用改**。改动面：

1. `sample/vgui_demo.cpp`（及 `vgui_game_editor.cpp` 等宿主主循环）：从「单窗口单 fbo 单 `vgui_draw`」改为遍历 `wg->windows()`，逐窗口同步尺寸、`vgui_update` / `vgui_draw`，用 `draw_datas_cx::set(&win->fbo)` 逐条收集，最后**一次** `render()` 提交。注意不要逐窗口调 `vgpu_acquire_swapchain`（见 §3.1）。
2. `include/vgui/backend/vgui_sdl3.h` + `backends/vgui_sdl3.cpp`：把「逐窗口渲染」提升为 `app_mgr` 的 API；`_mouse_key` 改为每窗口一份 `gui_io_mk_state`（或明确修饰键本来就是全局状态）；`pump_gamepad()` 用当前焦点窗口而非 `main_window`。
3. `include/vgui/editor/editor_shell.h` / `gui/editor/editor_shell.cpp`：允许多个 `editor_shell` 实例各自绑定不同 `vgui_context`（现在构造器接 `editor_context&`，多加一个实例即可，无需改类内部结构）。
4. `include/vgui/editor/editor_layout.h` / `gui/editor/editor_layout.cpp`：布局从「单份」改为 `window id → 布局` 的映射，JSON 升版（v8），保留 v1–v7 兼容；顶层改成 `root["windows"]` 数组，单窗口旧格式仍映射到第一个元素。
5. `backends/vgui_sdl3_dialog.cpp:76-83`：模态父窗口由调用方显式传入 `os_window*`。

验收建议：新增一个 `sample/vgui_multiwindow_demo.cpp`（两个独立 OS 窗口各挂一个 Dock），以及 `vgui_model_test` 里「两个 context 的 dock 互不干扰」的常驻断言。

### 目标 B：浮动面板变真 OS 窗口

语义变更，必须动 core：

1. `include/vgui/vgui.h` + `gui/vgui.cpp`：`VGUI_DOCK_FLOATING` 从「zone + `fx/fy/fw/fh` + clamp」升级为「面板所有权转移到另一个 `vgui_context` / `os_window`」。新增如 `vgui_dock_detach_to_window(dock, panel, host_window_cb, ...)` 的 API，以及「屏幕坐标 ↔ 窗口坐标」换算回调（保持 `vgui.h` 不含 SDL 类型）。
2. `vgui_dock_panel::page`（裸指针）与 `next_id`（局部自增）→ 全局稳定 id / 跨 context 句柄；否则跨窗口引用必失效（§4 第 1、2 条 + `ctx->alive` 校验）。
3. `vgui_dock_clamp_rect` 对真窗口模式禁用；改为屏幕/显示器边界夹取。
4. `backends/vgui_sdl3.cpp`：`create2()` 为浮出面板建独立窗口并迁移页面，处理 `close_with_parent`、focus trap、IME 位置、窗口关闭时把面板还回原 Dock。
5. editor kit：布局持久化改为「面板 → 窗口归属 + 屏幕坐标」，不再是 dock 内相对矩形。

关键取舍：**不要试图让 `VGUI_DOCK` 自己创建 OS 窗口** —— `vgui.h:2247-2253` 明确 core 不含窗口代码。应保留 provider / 回调桥接（与文件对话框、剪贴板同样的模式），由后端实现窗口创建。

### 目标 C：跨窗口拖放停靠

在 B 之上再加：

1. 跨窗口 drag 会话：拖拽状态从 `vgui_dock_state`（per-widget）提升到进程级会话对象，带「源窗口 / 源 dock / 面板全局 id / 屏幕坐标」。
2. 启用 `docking_mgr::viewports_enabled`（`vgui_sdl3.cpp:959/1117/1136` 的全局坐标分支），统一「屏幕坐标 → 各窗口局部坐标」换算。
3. drop target 命中测试跨窗口：遍历所有窗口的 Dock，把屏幕坐标换算后做 hit-test；罗盘高亮要画在目标窗口上。
4. 面板迁移协议：`detach` → 全局句柄 → 目标 context 重建 page / 重新 `vgui_dock_add`；期间要处理取消（还回原处）、崩溃/窗口关闭（面板不丢）。
5. `vgui_input_*` / `vgui_input_drop_files` 需带 window id 或由宿主在派发前绑定目标 context（`vgui_sdl3.cpp:1601-1694`）。

C 的复杂度主要来自「widget 不能跨 context 直接搬」，本质是**序列化 + 重建**，而非指针迁移。若面板内容复杂（Viewport 的 3D FBO、CodeEditor 的文档状态），需要宿主提供 `serialize/restore` 钩子。

---

## 9. 风险清单

| 风险 | 说明 |
|---|---|
| 共享 `vgctx` 与 MSAA 重建 | `app_mgr::set_msaa_samples()` 会重建 2D 向量上下文并「recreates every window FBO」；`retired_ctxs_` 延迟一帧销毁。多窗口下要确认所有窗口的 FBO 与 native texture 都一起重建，否则会出现某个窗口拿着旧 context 的纹理表 |
| 纹理表归属 | `vgui_sdl3_texture_create_native(vgui_context* ui, vgpu_ctx_t* gpu_ctx, ...)` 把纹理注册到 `ui`，但 `gpu_ctx` 是共享的。多窗口各自注册可正常工作，但 MSAA 重建时需逐 context 重新注册 |
| 字体缓存 | 探针已证实同一个 `font_familys_t*` 可给多个 `vgui_context` 使用，此项**不是风险** |
| 「当前窗口」类全局状态 | `active_viewport_`（事件级，安全）、`main_window`（gamepad）、`_mouse_key`（按键状态）、`SDL_GetKeyboardFocus()`（对话框父窗口）、剪贴板/IME 候选项位置。多窗口下逐项确认 |
| 坐标空间 | 默认窗口局部坐标；`viewports_enabled` 的全局坐标分支默认关且无调用方。B/C 目标必须启用，但启用后所有既有输入路径的坐标语义都会变，需整体验证 |
| 布局版本 | `editor_layout` v7 → v8 必须保持 v1–v7 可读；新增窗口数组时旧文件应映射成「单窗口」 |
| 生命周期 | `gui_viewport::~gui_viewport()` 销毁自己的 `vgui_context`；宿主若像 demo 那样手动 `vgui_destroy(st.ui)` 再置 `nullptr`，多窗口下每个窗口都要做同样处理，否则双重释放 |
| 单线程约束 | `docs/vgui_capability_assessment.md:111` 明确「纹理创建/更新/绘制须同线程」。多窗口仍应在同一线程顺序渲染，不要每窗口开线程 |

---

## 10. 建议

1. **先做 A**：它是纯宿主/kit 层改动，core 零改动，能立刻验证「多窗口并存」并把后端骨架真正跑起来；同时把 §9 的共享 GPU 状态风险暴露出来，为 B/C 打基础。
2. **补文档与措辞**：把「浮动面板不是独立 OS 窗口、无多窗口支持」写入 `docs/vgui_capability_assessment.md` 的短板清单；`docs/editor_roadmap.md:239` 的「独立窗口」改为「浮动框架」。
3. **B 分两步**：先做「API + provider 桥接 + 单面板浮出」的最小闭环（`vgui_dock_detach_to_window` + 一个只含简单控件的面板），再迁移 editor kit 的复杂面板。
4. **C 谨慎评估**：跨窗口拖放的价值主要在多屏编辑器工作流，但成本接近重写 Dock 的所有权模型。若只是想把面板挪到第二块屏幕，B + 「记住窗口位置」已能覆盖大部分需求，不必上 C。
5. **不建议**的做法：让 `VGUI_DOCK` 直接 `#include` SDL 或持有 `SDL_Window*`；用局部 `vgui_item_id` 当跨窗口句柄；把 `vgui_widget*` 直接塞进另一个 context 的树（会被 `alive` 校验拦下或产生悬垂）。

---

## 11. 实施记录：目标 A 多窗口并存（2026-11-04）

按 §10 的建议先做 A，**core 的 `gui/vgui.cpp` 与 `include/vgui/vgui.h` 零改动** —— 印证了 §1 的判断。

### 11.1 改动清单

| 文件 | 改动 |
|---|---|
| `include/vgui/backend/vgui_multiwindow.h`（新增） | header-only `multiwindow_host` + `multiwindow_slot`：`attach/detach/prune/begin_frame/draw_all/end_frame`。每个窗口单独 `new_rvg()`，逐条 `draw_datas_cx::set(&win->fbo)` 收集，最后一次 `render()` 提交 |
| `sample/vgui_multiwindow_demo.cpp`（新增） | 两个独立 OS 窗口（900×620 / 720×480），各挂一个 `vgui_context` + 一个 `VGUI_DOCK`（3 / 2 个面板）。`VGUI_MW_FRAMES=N` 有界自检，21 条断言；GPU/窗口创建失败返回 77 |
| `sample/CMakeLists.txt` | 新增 `vgui_multiwindow_demo` 目标。因依赖真实 GPU 窗口，**未注册到 ctest** |
| `include/vgui/backend/vgui_sdl3.h` | `gui_viewport` 增加独立 `key_state`；`_mouse_key` 保留但标注为兼容字段；新增 `focus_viewport()` |
| `backends/vgui_sdl3.cpp` | `create()/create2()` 把 `io.mks` 指向各自 viewport 的 `key_state`，并设置 `main_window`（popup/tooltip 不算）；`pump_gamepad()` 改用 `focus_viewport()`；`destroy()` 清理 `main_window` / `active_viewport_` / `children` 中的悬垂指针；实现 `focus_viewport()`（键盘焦点 → 事件目标 → main_window） |
| `include/vgui/editor/editor_layout.h` / `gui/editor/editor_layout.cpp` | 新增 `editor_window_layout`（id + 可选窗口矩形 + 一份 `editor_layout`）与 `editor_layout_set`（JSON v8、`windows` 数组、`find/ensure`、to/from json、load/save file）。v1–v7 单布局文件自动导入为 `id="main"` 的一个窗口，内嵌 layout 仍按自身版本校验 |
| `sample/vgui_model_test.cpp` | 常驻 `test_dock_multi_context()`：7 条断言锁定「两个 context 各一 dock、id 局部、跨 context 页面被拒、浮框被夹、detach 后仍属原 context」 |
| `sample/vgui_editor_kit_test.cpp` | 常驻 `test_layout_set_multiwindow()`：v8 往返、按窗口隔离的 dock 尺寸/活动页/面板、窗口矩形、v7 导入、畸形输入拒绝、文件 IO |
| `README.md` / `docs/vgui_capability_assessment.md` | 新增「多窗口宿主」用法与 §7.15 重评记录 |

### 11.2 两个关键实现决策

1. **每个窗口一个 `rvg_t`**。`get_draw_list()` 返回的 `ovg_draw_data_t` 全是指向 `rvg_cx` 内部 vector 的裸指针（`third_party/ovg/ovg/ovg.cpp:4621`），共用一个 rvg 会让所有 draw data 指向同一块缓冲。双窗口 demo 里实测 `get_draw_list(canvasA).d != get_draw_list(canvasB).d`、命令数 71 vs 49，确认缓冲与内容都独立。
2. **一帧一次提交**。不逐窗口调 `vgpu_acquire_swapchain()`（它只有一份 `current_cmd`，第二次会先提交第一次）；`end_frame()` 直接把带各自 fbo 的 draw data 交给 `vgpu_render_frame()`，由它在一个 command buffer 内按 FBO 编码多个 pass。尺寸用 `SDL_GetWindowSizeInPixels()` 在录制前同步，`ensure_swapchain()` 在提交时兜底。

`editor_layout_set::windows` 用 **`std::deque`** 而非 vector：`ensure()` 返回 `editor_layout&`，vector 扩容会让调用方持有的引用失效（新增的 editor_kit 测试第一版就抓到了这个问题）。deque 尾部插入保证元素地址稳定。

### 11.3 实测结果

`VGUI_MW_FRAMES=90 vgui_multiwindow_demo.exe`（真实 GPU 窗口，MSAA 4x）：

```
PASS app_mgr owns two OS windows / first window becomes main_window
PASS each viewport holds its own context / key state is per window
PASS each window has a private rvg canvas
PASS draw lists live in separate buffers / windows recorded different content
INFO draw commands: A=71 B=49
PASS one frame submits both windows in one command buffer
PASS prune drops the closed window's slot / main_window stays valid after close
PASS remaining window still renders / dock A survived window B closing
SELFTEST PASS (21 pass, 0 fail)
```

关闭副窗口后主窗口继续渲染、Dock 状态完好 —— §3.1 里 `destroy()` 的悬垂风险已消除。

### 11.4 回归

| 项 | 结果 |
|---|---|
| x64-rel3 全量构建 | 通过 |
| `vgui_model_test` | 908 passed（新增 7 条） |
| `vgui_editor_kit_test` | PASS（新增 `test_layout_set_multiwindow`） |
| ovg_tools / c_api / chart / runtime_embed / edit | 全 PASS |
| ctest（screenshots / chart / chart_render） | 3/3 通过，**截图基线零变化**（改动全在后端窗口管理与布局序列化，不影响绘制） |

### 11.5 本轮未做

- **B / C 目标**：`VGUI_DOCK` 模型未动，浮动面板仍是窗口内自绘浮层，面板仍不能跨窗口拖动。§4 的 5 条硬阻塞全部仍在。
- **文件对话框父窗口**：重新评估后判定 A 阶段无需改动。现有 `sdl3_dialog_parent()`（`backends/vgui_sdl3_dialog.cpp:76-83`）取 `SDL_GetKeyboardFocus()` / `SDL_GetMouseFocus()`，多窗口下通常就是正确的模态父窗口；要精确指定必须先把窗口句柄引入 `vgui.h` 的 provider 层，那属于 B 阶段。
- **`docking_mgr::viewports_enabled`**：仍默认关闭、仍无调用方。它是「鼠标坐标转屏幕全局坐标」的开关，只有 C 目标（跨窗口拖拽命中测试）才需要，A 不涉及。
- **现有单窗口宿主**：`vgui_demo.cpp` / `vgui_game_editor.cpp` 仍是单窗口主循环，未强行改成遍历 `windows()`；新的多窗口模式由 `multiwindow_host` 提供，宿主按需采用。

### 11.6 复现命令

```bash
source /tmp/vguienv/env.sh && cd out/build/x64-rel3 && ninja
cd ../../bin/x64
VGUI_MW_FRAMES=90 ./vgui_multiwindow_demo.exe   # 21 条自检
./vgui_model_test.exe | grep "dock:"             # 7 条多 context 断言
./vgui_editor_kit_test.exe                       # v8 布局分组
```
