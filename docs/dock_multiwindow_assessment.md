# Dock 多窗口能力评估

评估日期：2026-11-04
范围：`libvgui` core（`gui/vgui.cpp` / `include/vgui/vgui.h`）、SDL3 后端（`backends/` / `include/vgui/backend/`）、editor kit（`gui/editor/` / `include/vgui/editor/`）、sample 宿主
方法：静态代码审计 + 运行时探针（临时插入 `sample/vgui_model_test.cpp` 的 headless 用例，验证后已按字节还原）

> 本文只做评估，不改动任何实现。所有行号对应本次评估时的源码。
>
> **实施更新（2026-11-04）**：目标 A 已落地，见 §11。§1–§10 保留评估时的原始判断作为依据；涉及「未接线 / 零覆盖 / 缺 API」的表述以 §11 为准。
>
> **实施更新（2026-11-14）**：目标 B 第一步已落地（后端库化组件 `dock_floater_host`，core 零改动），见 §12。§4 的"5 条硬阻塞"对应 §8.2 的 core 深改路线——原型/组件路线（detach + 占位页 + 宿主建窗）全部绕开，见 §12.1。
>
> **实施更新（2026-11-16）**：目标 C1–C4 已落地（§13），一次连续拖放也已落地（§14）。§8.3 关于"一次连续拖放需进程级会话 + `viewports_enabled` + core 深改"的判断经 SDL 捕获探针实证被推翻：core 只需 +1 绘制 API，其余在组件层完成。
>
> **实施更新（2026-11-16）**：目标 B 第一步补齐多浮动窗口恢复（FIFO tear-off 队列）并让 `vgui_game_editor` 真正读写 v9 布局（`floaters` 屏幕矩形），见 §12.6。

---

## 1. 结论速览

「dock 多窗口」需先拆成三种目标，它们的可行性差别极大：

| 目标 | 含义 | 可行性 | 阻塞层 | 改动量 | 实施状态 |
|---|---|---|---|---|---|
| **A. 多窗口并存** | 一个进程开多个 OS 窗口，每窗口各有一棵 widget 树 / 一个 Dock，互不拖放面板 | **高** | 宿主循环、editor kit 多实例 | 中 | **已完成**（§11） |
| **B. 浮动面板 → 真 OS 窗口** | 面板可浮出为主窗口之外的独立原生窗口（可移出主窗口、可跨屏） | **中** | core Dock 数据模型 | 大 | **第一步已完成**（§12） |
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
- **现有单窗口宿主**：`vgui_demo.cpp` 仍是单窗口主循环；`vgui_game_editor.cpp` 已改为**默认两窗口**（见 §11.7），但宿主仍是各自手写的循环，没有引入统一框架。

### 11.6 复现命令

```bash
source /tmp/vguienv/env.sh && cd out/build/x64-rel3 && ninja
cd ../../bin/x64
VGUI_MW_FRAMES=90 ./vgui_multiwindow_demo.exe   # 21 条自检
./vgui_model_test.exe | grep "dock:"             # 7 条多 context 断言
./vgui_editor_kit_test.exe                       # v8 布局分组
```

### 11.7 目标 A 的第二个宿主：`vgui_game_editor` 默认两窗口（2026-11-04）

在真实的编辑器宿主上落地，而不是只停留在 demo。`sample/vgui_game_editor.cpp` 现在默认开两个 OS 窗口。

| 项 | 内容 |
|---|---|
| 主窗口 | 原有完整编辑器（1280×800，`editor_shell` 一套布局不动） |
| 副窗口 | 760×470，独立 `vgui_context` + 独立 `rvg` + 自己的 `VGUI_DOCK`（`GAME VIEW` / `STATS` 两块） |
| 画面来源 | 副窗口**专属**的第三个 `phase0_view`，按面板像素尺寸重建附件；不是把主窗口 363×148 的预览条放大（放大会糊） |
| 纹理 | `vgui_sdl3_texture_create_native()` 在副窗口上下文注册一份；MSAA 重建 vgpu 上下文时先 `release()`、重建后 `sync()` 自动按新句柄再注册 |
| 统计 | 每帧刷新 FPS / 帧时 / MSAA / 对象数 / 窗口数 / 编辑器与预览尺寸；文本不变时不调用 `set_text`，避免逐帧重排 |

关闭语义也要改：副窗口关闭时编辑器继续跑。SDL 只在**最后一个**窗口关闭时发 `SDL_EVENT_QUIT`，所以在此之前 `WINDOW_CLOSE_REQUESTED` 没人消费。后端在 `app_mgr::get_event()` 的 switch 里新增一条：把该窗口的 `os_window::pending_close` 置位（这个字段此前是死字段），由宿主决定销毁。单窗口宿主行为不变——最后一个窗口关闭时 SDL 仍会发 QUIT。

另外修掉一个布局陷阱：`vgui_context` 的根控件默认是 `VGUI_LAYOUT_ABSOLUTE`，直接往里加 `Dock` 并设置 `grow` 不会生效，副窗口的 Dock 只占了内容所需的高度、下面留一大片黑。必须像 `editor_shell` 那样先把根改成 `COLUMN` + `align_items = STRETCH`。

自检（`VGUI_EDITOR_FRAMES=120`）：

```
windows=2 gameview=on
INFO draw commands: editor=477 gameview=57
INFO gameview: dock=1 panels=2 texture=bound
PROBE request MSAA 2x at frame 40
INFO after MSAA rebuild: gameview texture=bound
PROBE pushed close request for the game view window
frames=120 windows_at_exit=1 gameview_closed=1
```

即：两窗口同时出图（绘制命令 477 / 57，各自缓冲）、MSAA 切换后两套上下文的纹理都重新注册成功、关闭副窗口后主窗口继续渲染 40 帧并正常退出。单窗口回退用 `VGUI_EDITOR_SINGLE_WINDOW=1`，仍走同一条帧循环。

落地时又踩到两个小坑，记在这里免得下次重犯：

- **`app_mgr::create2()` 的 x/y 只对 popup/tooltip 生效**，普通窗口走 `SDL_CreateWindow(title, w, h, ...)`，位置由 WM 叠放决定。要指定位置必须自己补一句 `SDL_SetWindowPosition(win->window, x, y)`（`vgui_multiwindow_demo.cpp` 里本来就这么写）。
- **窗口默认位置**：先算主窗口右侧剩余宽度（`SDL_GetDisplayUsableBounds`），够 420px 就贴右侧、不够就先收窄到剩余宽度；连 420 都没有（主窗口很宽时常见）就贴屏幕右下角，避免盖住主窗口上方的 3D 视口。实测本机 1920×1080、主窗口 1280×800 走的是右下角分支，副窗口落在 `(1152, 554)`。

---

## 12. 实施记录：目标 B 第一步——库化 floater 组件（2026-11-14）

### 12.1 路径决策：后端组件路线，不走 §8.2 的 core 深改路线

`vgui_game_editor.cpp:3994-4469` 在 §11.7 之后已实现目标 B 的完整宿主原型（拖出→无边框 OS 窗口→live drag 跟随→合并回收，约 450 行），core 零改动。它的关键做法是：**真实页面不跨 context**——`vgui_dock_detach()` 把页面留在主上下文，浮动窗口的 context 里由宿主钩子建页（占位或重建真实内容），合并时孤儿页 `vgui_dock_add` 挂回主 Dock。在这一路线下，§4 的 5 条"硬阻塞"逐条被绕开：

| # | 阻塞点（§4） | 原型/组件路线下的状态 |
|---|---|---|
| 1 | 跨 context 页面被拒 | **绕开**：detach 不迁移页面，浮动窗口用宿主钩子重建/占位 |
| 2 | 面板 id per-dock 局部 | **绕开**：宿主以主 dock 的 item id 为键维护 floater 表，id 不跨 dock 传递 |
| 3 | 浮动被几何夹取 | **不适用**：真 OS 窗口由 WM 管理，`vgui_dock_clamp_rect` 只作用于窗口内 FLOATING 区 |
| 4 | 拖拽状态单 context | **绕开**：跨窗口部分由宿主轮询全局鼠标 + SDL 窗口位置完成，core 拖拽状态机只管窗口内 |
| 5 | 输入 API 无窗口概念 | **绕开**（B 范围内）：坐标换算在宿主/组件内完成；C 的跨窗口命中测试才需要全局坐标 |

结论：**B 不需要改 core 数据模型**；只有 C（跨窗口 Dock 间拖放、罗盘画在目标窗口）才真正触及 §4。§8.2 草图（`vgui_dock_detach_to_window` + 全局稳定 id + clamp 屏幕化）保留为"C 启动时"的参考，本阶段不实施。§10.4 的判断成立：B + 记住窗口位置已覆盖大部分多屏需求。

### 12.2 组件 API（`include/vgui/backend/vgui_dock_windows.h`，header-only）

`dock_floater_host`——把 game_editor 原型泛化为可复用组件（与 `multiwindow_host`、`window_dock_manager` 同风格）：

- `attach(app_mgr*, multiwindow_host*, vgui_widget* main_dock, os_window* main_window, font_familys_t* familys, font_size, opts, hooks)`：注册 `vgui_dock_on_tear_off/on_drag_out`（回调只入队，遵守 core 契约）；浮动窗口经宿主现有 `multiwindow_host` 提交——**一帧仍只有一次 `vgpu_render_frame()`**。
- `tick()`：帧末调用。处理入队 tear-off（`create2(..., ef_borderless|ef_resizable)` 建无边框窗 → 新 `vgui_context` → 根 COLUMN+STRETCH → 标题条（兼 SDL hit-test 拖动区）→ Dock → 宿主钩子页/内置占位页 → chrome 实测校正，撕出前后面板尺寸一致）→ live drag 逐帧 `SDL_SetWindowPosition` 跟随 → 合并判定（内容区全含/中心点兜底，容差可配）→ 系统模态拖动结束的"位置变化 > 6px"检测（settle 过滤建窗抖动）→ 浮动窗口 `pending_close` 合并回收 → 失败路径回滚面板 + `vgui_dock_end_drag` 防入队循环。
- 宿主钩子：`build_page`（nullptr = 内置占位页；真实内容面板如 3D 视口在此重建并处理原生纹理 release/sync，见 game_editor GAME VIEW）、`merged`（面板挂回后通知）。
- 布局持久化接口：`capture_floaters()` 返回 `std::vector<dock_floater_rect>`（`panel` item id、`id` 面板字符串 id、`x/y/width/height` 为含 chrome 的 OS 窗口屏幕矩形）；`request_tear_off(panel, x, y, w, h)` 在下一 `tick()` 按保存的矩形建窗，配合 `vgui_dock_find()` 完成 editor layout v9 的恢复。
- 关键不变量（原型踩坑直接内建）：title/id 字符串 detach 前拷贝（`c_str()` 悬空）；页面矩形 detach 前采样；销毁顺序 `vgui_destroy(floater ctx)` → `viewport->ui = nullptr` → `wg->destroy(window)`。

### 12.3 demo 与自测结果

`sample/vgui_dock_floater_demo.cpp`（CMake target `vgui_dock_floater_demo`，不进 ctest，GPU 失败返回 77）：主窗口 3 面板，`VGUI_FLOATER_SIM=1` 注入拖拽做有界自检，22 项断言全绿——tear-off 建窗、占位页、单次提交（host.size()==2）、面板离开主 Dock、**合并后真实页面 widget 指针原样恢复**、二次 tear-off 幂等、`capture_floaters()` 返回稳定 id 与屏幕矩形、**FIFO tear-off 队列一次恢复多个浮动窗口**（连续入队后每个 tick 建一个，最终 3 个 floater 全部存在且 `capture_floaters()` 逐一覆盖）；`VGUI_FLOATER_SIM=0` 手动体验模式。本批次 core / editor kit **零改动**。

### 12.4 后续批次

1. ~~`vgui_game_editor.cpp` 原型迁移到 `dock_floater_host`~~ **已完成（2026-11-15，批次二）**：宿主引擎（~450 行）删除，接入 4 个新生命周期钩子——GAME VIEW 面板走 **remove 模式**（页面不可跨 context 保留：`teardown_page` 先 release 纹理 → `remove_page` true → `vgui_dock_remove` → 浮动窗口由 `build_page` 重建真实页 → 合并/失败路径由 `restore_page` 在主 context 重建 + `floater_closing` 先于浮动 ctx 销毁释放纹理），其余面板走 detach + 内置占位页默认路径。迁移中发现并修复组件缺陷：remove 模式漏调 `vgui_dock_remove`（面板滞留主 Dock，合并时字符串 id 重复被拒）。集成自测：`VGUI_EDITOR_FRAMES=90 VGUI_ALLOW_TEAROFF=1 VGUI_ALLOW_MERGE=1` → 浮动窗口创建、**MSAA 重建后纹理重绑（floaters=1 期间跨上下文）**、关闭合并后 `panel=<新 id> texture=bound`。新增公共 `request_tear_off(panel)`（探针/测试入队）。批次四把请求改成 FIFO 队列并接入布局 v9，见 §12.6。
2. ~~`editor_layout_set` 持久化"面板 → 浮动窗口屏幕坐标"（JSON v9）~~ **已完成（2026-11-15，批次三）**，见 §12.5。
3. C（跨窗口拖放）：本阶段不做；若启动需按 §8.3 评估 core 模型改造 + `docking_mgr::viewports_enabled` 接线。

### 12.5 实施记录：布局 v9 保存浮动窗口屏幕坐标（2026-11-15，批次三）

目标 B 第一步之后，被拖出的面板已经离开主 Dock，`editor_layout::dock_panels` 里的 `VGUI_DOCK_FLOATING` 条目只覆盖窗口内自绘浮层，重开编辑器时真实浮动窗口的位置会丢。v9 把「面板字符串 id → 浮动 OS 窗口屏幕矩形」放进每个窗口的布局槽：

```json
{
  "version": 9,
  "windows": [
    {
      "id": "main",
      "rect": { "x": 0, "y": 0, "width": 1280, "height": 800 },
      "floaters": [
        { "panel": "gameview", "rect": { "x": 1180, "y": 90, "width": 640, "height": 420 } },
        { "panel": "stats" }
      ],
      "layout": { "version": 7, "...": "..." }
    }
  ]
}
```

| 项 | 决策 |
|---|---|
| 坐标语义 | `rect` 是浮动 OS 窗口的**外框**（含标题条 + 标签条 chrome），单位是屏幕点，与 `dock_floater_host::capture_floaters()` / `request_tear_off(panel,x,y,w,h)` 一致 |
| 位置 | 挂在 `editor_window_layout::floaters` 下，因为浮动窗口属于某个主窗口；内嵌 `editor_layout` 仍保持 v7，不把 OS 窗口坐标混进 Dock 的窗口内坐标 |
| 兼容 | 文件版本 8/9 都接受；v8 没有 `floaters`，解析为空。v1–v7 仍整份导入为 `id="main"`。未知版本（≥10）拒绝 |
| 校验 | `floaters` 必须是数组；`panel` 非空且同一窗口内不重复；`rect` 必须是对象且宽高为正。无 `rect` 的条目保留（面板已知、窗口尺寸尚未采样） |
| 恢复 | 宿主遍历 `layout->floaters`：`vgui_dock_find(dock, panel.c_str())` → `floater.request_tear_off(panel, x, y, w, h)`。`request_tear_off` 只入 FIFO 队列，每个 `tick()` 建一个窗口，因此一批保存的 floater 一次遍历即可全部恢复；面板不在主 Dock 时静默丢弃 |
| 示例宿主 | `vgui_game_editor.cpp` 启动时在 `dock_floater_host::attach()` 之后加载布局并逐个入队恢复，退出前（销毁 shell/浮动窗口之前）用 `capture_floaters()` 写回；默认路径 `SDL_GetPrefPath("vgui","game_editor")/layout.json`，`VGUI_EDITOR_LAYOUT` 覆盖（空/`0` 关闭），有界 `VGUI_EDITOR_FRAMES` 忽略默认文件但尊重显式路径 |

测试：`vgui_editor_kit_test::test_layout_set_multiwindow()` 覆盖 v9 往返、浮动矩形/无矩形条目、v8 导入、重复 floater panel、空 panel、空/畸形 rect、`floaters` 非数组、文件 IO。`capture_floaters()` / 新 `request_tear_off` 为 header-only，随 `vgui_dock_floater_demo` 与 `vgui_game_editor` 的构建覆盖编译。

### 12.6 实施记录：多浮动窗口恢复与示例宿主落盘（2026-11-16，批次四）

v9 格式落地时组件的 tear-off 请求仍是单槽：`request_tear_off()` 覆盖前一个请求，宿主一次性恢复多个保存的 floater 时只有最后一个能建窗。批次四把请求改成 FIFO 队列，并让 `vgui_game_editor` 真正读写布局文件：

- **请求队列**：`std::deque<tear_off_request>`，`on_tear_off_thunk` / `on_drag_out_thunk` / `poll_dock_drag` 与两个 `request_tear_off()` 重载统一 `push_back`；`tick()` 只处理队首一个，`pending_tear_off()` 返回队首面板，`poll_dock_drag` 以队列非空为去重条件。拖动/关闭路径每次仍只产生一个请求，语义不变。
- **显式收尾**：新增 `close_all()`——销毁全部浮动窗口但不合并（每个都触发 `floater_closing`），并清空待处理队列。`vgui_game_editor` 的 `editor_state` 是静态对象，若依赖 `dock_floater_host` 析构会在 `main` 返回后才执行，那时 `app_mgr` / 窗口注册表已销毁（实测退出码 `0xC0000005`）；现在保存后、拆机前显式调用。
- **示例宿主接线**：`editor_layout_path()` 默认 `SDL_GetPrefPath("vgui","game_editor")/layout.json`，`VGUI_EDITOR_LAYOUT` 覆盖（空/`0` 关闭），有界 `VGUI_EDITOR_FRAMES` 忽略默认文件以保持确定性、但尊重显式路径。`load_editor_layout()` 在 floater host attach 之后执行：`shell->apply_layout()` 恢复 Dock，再对 `window->floaters` 逐个 `vgui_dock_find()` → `request_tear_off()`。`save_editor_layout()` 在销毁 shell / 浮动窗口之前执行：`shell->capture_layout()` + `capture_floaters()` 写入 v9 `windows[0]`。
- **验证**：`vgui_dock_floater_demo` 自测 22/22（含 FIFO 与多 floater capture）。`VGUI_EDITOR_FRAMES=90 VGUI_ALLOW_TEAROFF=1 VGUI_EDITOR_LAYOUT=<tmp>\layout.json vgui_game_editor` 退出码 0，JSON 为 v9 且 `floaters:[{panel:"gameview",rect:{x:1296,y:158,width:300,height:724}}]`；用同一文件跑 `VGUI_EDITOR_FRAMES=30`（不注入 tear-off）打印 `layout loaded (floaters=1)`，第 3 帧绘制命令 `floaters=1`，退出后矩形与保存值一致。
- **边界**：主窗口自身的 OS 矩形当时未保存/恢复；`editor_layout` 内嵌版本仍为 7。**主窗口矩形持久化已于本节之后的实施记录（§17）补齐**——宿主层的 `capture_window_rect` / `restore_window_rect` 把 `editor_window_layout::rect` 接上，无需升格式版本（rect 字段自 v8 就在 JSON 中）。

---

## 13. 实施记录：目标 C 跨窗口 Dock 间拖放停靠（2026-11-16）

> 完整规划见 `.zcode/plans/plan-dock-c-crosswindow-docking.md`。**路径决策：走宿主组件路线，不走 §8.2/§8.3 的 core 深改。** C = B 的 tear-off（源→浮动 OS 窗口作运输载体）+ 泛化 merge（浮动→**任意**已注册 Dock），§4 的 5 条 core 硬阻塞继续被绕开。`gui/vgui.cpp` / `include/vgui/vgui.h` **零改动**。

### 13.1 分层改动

| 批次 | 文件 | 改动 |
|---|---|---|
| C1 命中 | `include/vgui/backend/vgui_dock_coords.h`（新增） | 屏幕点↔窗口渲染像素换算的唯一来源（`pixel_ratio` / `screen_to_render` / `render_to_screen` / `window_rect`）。B 组件的内联换算暂留，待 C2 触及合并路径时已一并改用本口径。 |
| C1 命中 | `include/vgui/backend/vgui_crosswindow_dock.h`（新增） | `crosswindow_dock_host`：window→dock/ui 注册表（`register_target`/`unregister`/`prune`）+ `resolve_drop(sx,sy)` 屏幕命中（用现有 `vgui_dock_get_zone_rect`/`get_tab_rect`，四区逆序测试、tab 命中给 index）+ `screen_point_for()` 反查（自测用）。drop_result 带 window/dock/**ui**/window_id/zone/index。 |
| C2 迁移 | `include/vgui/backend/vgui_dock_windows.h` | `dock_floater_host` 增 3 钩子 `resolve_target`/`migrate_page`/`migrated`（+`crosswindow_user`）；三处合并判定（live-drag down/up、release、系统标题栏位置变化）改为**先问跨窗口、否则原样 merge_back**。`migrate_to()`：关浮动窗（`floater_closing` 释放浮 ctx 资源）→ 销毁源孤儿页 → 目标 ctx 重建页（`migrate_page` 或内置占位）→ `vgui_dock_add(drop.dock,{zone,index})`；失败 `roll_back` 回源。新增 `request_migrate(panel,drop)` 显式队列（`tick()` 末尾处理，探针/恢复用）。全程用**字符串 id**（数值 id per-dock 局部会碰撞）。 |
| C3 复杂页 | 同上 | `last_resolve()` 诊断：命中结果供宿主在浮动窗绘高亮（§6 方案 A）。复杂页（如 GAME VIEW）的纹理 release/rebuild 由既有 `teardown_page`/`floater_closing`/`migrate_page`/`restore_page` 四钩子覆盖——demo 用生命周期探针验证其在正确 context 触发。 |
| C4 布局 | `include/vgui/editor/editor_layout.h` / `gui/editor/editor_layout.cpp` | JSON **v10**：`editor_floater_layout` 增 `owner`/`source`（跨窗口归属/来源），缺省为空（=所在窗口槽）以兼容 v8/v9；≥11 拒绝；其他校验不变。内嵌 `editor_layout` 仍 v7。 |

### 13.2 交互模型与取舍

两段式拖放：① 源 Dock 标签拖出窗口 → `on_tear_off` 入队建浮动 OS 窗口（B 现状）；② 系统级移动浮动窗到目标窗口 → 命中测试+**拖入即迁移**（与 B "拖回即合并"一致，松手不是必需）。这是组件路线相对"一次连续拖放"的体验代价。`vgui_crosswindow_dock_demo` 手写实测后接受；若要一次连续拖放，可按 §8.3 升级段 1（进程级 drag 会话 + `viewports_enabled`），本阶段不动。

未启用 `docking_mgr::viewports_enabled`（全局坐标注入改所有输入语义，组件路线不需要）。每 dock 仅一个 tear_off/drag_out 回调槽且 B 已占用——`crosswindow_dock_host` 不注册 core 回调，由 floater 单入口调用。

### 13.3 明确未做

- ~~**game_editor 未接跨窗口 target**~~ → **已接线，见 §15**（2026-11-16）。注意本节原文的依据（"GAME VIEW 专用副窗口、同 id 面板"）已是历史状态：该副窗口在 commit `bb0b445` 被删除，当前只有主窗口一个常驻 Dock。跨窗口迁移的通用验证仍在 `vgui_crosswindow_dock_demo` 完成。
- ~~**目标区罗盘画在目标 Dock 内**~~ → **已实现，见 §14.2**（core 增 `vgui_dock_set_drop_preview`，D2）。
- core 数据模型改造（全局稳定 id / widget 序列化 / 跨 context 指针搬运）仍未做，§8.2 草图保留作后续参考。

### 13.4 验证

- `vgui_crosswindow_dock_demo`（`VGUI_XDOCK_SIM=1`，真实 GPU 窗口，失败返回 77、不进 ctest）：**47/47**——C1 四区/标签命中、窗口外 invalid、unregister/prune；C2 迁移闭环（离开浮动、进 B 指定 zone/index、稳定 id 保留）+ 失败回滚（跨 ctx 页被引擎拒绝→回源）+ 非法 drop 不入队；C3 资源生命周期在正确 context（migrate=B / restore=A / teardown+closing 均触发）。
- `vgui_editor_kit_test`：v10 往返 + owner/source + v9 兼容（无归属）+ ≥11 拒绝 + 畸形输入，全 PASS。
- 回归：全量构建 0 新警告；`vgui_model_test` 920/0；`vgui_dock_floater_demo`（B）22/22；`vgui_multiwindow_demo`（A）21/21；`vgui_game_editor` 冒烟（MSAA 重建/纹理重绑/合并）正常；ctest 5/5，**截图基线零变化**。
- `vgui_game_editor` v10 布局往返：保存生成 `"version":10`（内嵌 7），重开 `layout loaded (floaters=1)` 并恢复浮动窗矩形。

---

## 14. 实施记录：一次连续拖放（2026-11-16）

> 本节落实规划 `.zcode/plans/plan-dock-c-crosswindow-docking.md` §11"后续升级段 1"。**结论先行**：§8.3/§13.2 曾判断"一次连续拖放需要进程级 drag 会话 + `viewports_enabled` + 改输入语义"——**该前提经 SDL 探针实证被证伪**。实际只需 core 增加一个可选绘制 API，其余全部在宿主组件层完成。

### 14.1 关键实证：SDL 捕获让跨窗口事件回到源窗口

最小 SDL3 探针（`out/probe`，两窗口 + 系统级 `SetCursorPos` + `SDL_CaptureMouse`）实测：

| 场景 | 鼠标物理位置 | 事件 windowID | 事件局部坐标 |
|---|---|---|---|
| 无捕获 | B 窗口上方 | **B**（正常路由） | B 局部，未越界 |
| 捕获开（项目现状） | B 窗口上方 | **A**（捕获窗口） | A 局部且越界（如 `(760,150)`，A 宽 400） |

项目在 `backends/vgui_sdl3.cpp` 的 `SDL_EVENT_MOUSE_BUTTON_DOWN` 分支调用 `pw->set_capture()`，按下期间鼠标事件（含 mousemove/mouseup）全部锁到源窗口。含义：

1. core 的 `drag_mode` 会话挂在源 context，但**跨窗口的 mouseup 仍回到源 ctx**——会话不会断，不需要进程级会话重写。
2. 越界的源窗口局部坐标经 `window_pos + local/pixel_ratio` 即得屏幕点（`render_to_screen`），与 `crosswindow_dock_host::resolve_drop` 同口径。
3. `viewports_enabled` 是**不需要**的：它改的是全局坐标注入语义，而捕获路径已经提供了等价信息。启用它反而要回归所有输入路径（§9 风险），徒增成本。

### 14.2 改动清单

| 批次 | 改动 | core 影响 |
|---|---|---|
| D1 连续直迁 | `dock_floater_opts::continuous_docking`（默认关）；`on_tear_off_thunk` 在松手点命中异窗口 Dock 时入队 `direct_migrate_request`；`direct_migrate()` 从**仍停靠**的源面板 detach → 目标 ctx 重建页 → add → 失败 `roll_back`；连续模式下 `on_drag_out_thunk`/`poll_dock_drag` 抑制提前建浮动窗 | 无（纯组件层） |
| D1 端到端 | `vgui_input_*` 注入真实 drag（down→越界 move→up），验证 core 回调链走到连续分支 | 无 |
| D2 目标高亮 | core 增 `vgui_dock_set_drop_preview(dock, zone, index)`（纯视觉状态，zone<0 清除）+ `vgui_dock_state::preview_zone/index` + `vgui_draw_dock` 的目标区填充/描边/插入线；`dock_floater_host` 的 `set/clear_drop_preview` 在拖拽期间逐帧维护目标 dock | **+1 API +2 字段 +1 绘制分支** |

**交互模型**：一次连续拖放 = 在源 tab 按下 → 拖过窗口边界（无浮动窗产生，目标 Dock 实时显示罗盘）→ 在目标 Dock 松手 → 面板直接从源 Dock 迁移到目标 Dock。命中 miss 或迁移失败时面板留在源 Dock（不丢）。两段式（B/C 现状）仍默认保留，连续模式按宿主 opt-in 开启。

### 14.3 对旧结论的修正

- §8.3 第 1 步「drag 会话提升到进程级」、第 2 步「启用 `viewports_enabled`」、第 4 步「`vgui_input_*` 带 window id」——**一次连续拖放不需要**（SDL 捕获已满足）。`viewports_enabled` / `docking_mgr` 已于 §16 物理删除。
- §8.3 第 3 步「跨窗口 hit-test」仍需要，且已由 `crosswindow_dock_host::resolve_drop` 提供。
- §13.2「一次连续拖放需 core 深改」「体验代价」——**已撤销**：`continuous_docking` 模式以极小 core 影响实现。
- §13.3「目标区罗盘画在目标 Dock 内需改 core」——**已实现**（`vgui_dock_set_drop_preview`）。
- §4 的 5 条硬阻塞与 §8.2 core 数据模型草图仍是"全局稳定 id / widget 序列化"层面的升级参考，本轮一次连续拖放**继续用字符串 id + 目标 ctx 重建**绕开，未触碰该模型。

### 14.4 验证

- `vgui_crosswindow_dock_demo`（`VGUI_XDOCK_SIM=1`）：**74/74**——在 §13.4 的 47 断言基础上新增 D1（连续直迁无浮动窗 / 源 dock 直接失去 / 目标 dock 直接获得 / 稳定 id / 目标 ctx 重建 / miss 不动 / 失败回滚）、D1 端到端（真实 `vgui_input_*` 拖拽 8 断言）、D2（目标预览 set/switch/clear）。
- 回归：全量构建 0 警告；`ctest` 5/5（**截图基线零变化**——preview 分支默认不触发）；`vgui_model_test` 920/0；`vgui_editor_kit_test` PASS；`vgui_c_api_test` 32/0；B 22/22；A 21/21；`vgui_game_editor` 冒烟正常。

---

## 15. 实施记录：game_editor 跨窗口接线（2026-11-16）

> 本节落实 §13.3 "game_editor 未接跨窗口 target" 的遗留项，同时修正其事实依据。

### 15.1 事实修正：副窗口已不存在

§13.3 当初给的理由是"GAME VIEW 专用副窗口（已含同 id 面板）语义混乱"。但当前工作区的副窗口已在 commit `bb0b445`（refactor: game_editor 面板拖出引擎迁移到 dock_floater_host，删宿主原型 ~450 行）中删除——GAME VIEW 现在是主窗口 Dock 里的一个普通面板，全文件只有一个 `wg->create()`。因此：

- 原来的"同 id 面板 / 副窗口作为通用停靠目标语义冲突"前提**已不成立**；
- 但 game_editor 现在**只有一个常驻窗口**（另一窗口是运行时浮动窗），没有第二个常驻 Dock 就无法真正演示跨窗口停靠。

### 15.2 决策：仅接线，不新建窗口

评估过三个方向（恢复通用第二窗口 / 恢复 GAME VIEW 专用副窗口 / 仅接线不建窗口），选择**仅接线**——不重新引入曾被有意删除的窗口架构。理由：恢复窗口属于架构决策、改动面大，且跨窗口能力已有 `vgui_crosswindow_dock_demo` 做完整通用验证；game_editor 的价值定位为"宿主如何接入跨窗口组件"的可运行范例。

### 15.3 改动

仅 `sample/vgui_game_editor.cpp`，核心组件与 core 均零改动：

| 项 | 内容 |
|---|---|
| include / 成员 | 引入 `vgui_crosswindow_dock.h`；`editor_state` 持 `std::unique_ptr<crosswindow_dock_host> xdock`（延迟到 main 用 `wg` 构造） |
| 注册 | `g.xdock->register_target(form, g.shell->dock(), "main", ui)`——唯一 target 即主 Dock，显式传 `ui` 不依赖 `viewport->ui` 时序 |
| 钩子 | `resolve_target` 转发 `xdock.resolve_drop()`；`migrate_page` 按 id 特判 gameview，在目标 context `build_gameview_page()` 并同步 `gameview.ui/window/dock`（纹理生命周期复用既有 teardown/closing hook + `sync_texture`）；`migrated` 同步归属指针；`crosswindow_user=&g` |
| 开关 | `dock_floater_opts::continuous_docking` 由 env `VGUI_XDOCK_WIRING=1` 开启，**默认关闭**——保持既有 B 两段式行为与所有现有自测不变 |

### 15.4 安全退化

只注册主 Dock 时，任何 drop 命中都满足 `drop.dock == main_dock_`，而 `try_migrate_at` / `direct_migrate` 明确排除源 Dock——跨窗口迁移路径**永不触发**，行为退化为 B 的"拖回即合并"。这是有意的：钩子完整接线，未来对第二个带 Dock 的窗口补一次 `register_target()` 即可激活跨窗口迁移 / 一次连续拖放，无需改任何钩子。

### 15.5 验证

- 默认 / `VGUI_XDOCK_WIRING=1` 两模式下 `VGUI_SIM_DRAG=1 VGUI_ALLOW_MERGE=1` 的 tear-off→MSAA→merge 闭环输出**逐字一致**：`after merge: floaters=0 gameview_ui_is_main=1 texture=bound`，证明接线对现有行为零影响。
- 全量构建 0 警告；`ctest` 5/5（截图基线零变化）；`crosswindow_demo` 74/74；model 920/0；kit PASS；c_api 32/0；B 22/22；A 21/21。

---

## 16. 实施记录：删除 viewports_enabled 死代码链（2026-11-16）

> 落实 §15 之后的"清理确认死代码"请求。§3.1/§11.5 曾记录该开关是死代码——本次将其从源码**物理删除**，而非仅标注。

### 16.1 删除范围（完整链，无残骸）

| 位置 | 删除内容 |
|---|---|
| `include/vgui/backend/vgui_sdl3.h` | `class docking_mgr`（整个空壳类，唯一成员即本死字段）；`app_mgr::docking_` 成员；`app_mgr::docking()` getter 声明 |
| `backends/vgui_sdl3.cpp` | `docking()` getter 定义；`process_event` 里 `bool viewports_enabled = docking().viewports_enabled` 局部拷贝；MOUSE_MOTION / DROP_POSITION / DROP_COMPLETE 三处 `if (viewports_enabled){ 加窗口偏移 }` 分支 |

### 16.2 为什么行为不变

setter `set_viewports_enabled()` 全仓库无调用（`docking()` 的唯一调用点就是被删的局部拷贝），字段恒为 `false`，三个 `if` 分支从未执行。删除后保留的是原 else 行为——直接用 SDL 事件的窗口局部坐标 `e->*.x/y` 乘 `pixel_ratio`，不做窗口偏移。跨窗口坐标换算早已由 `vgui_dock_coords.h` 的 `render_to_screen` + SDL 捕获路径承担（§14.1），不依赖此开关。

### 16.3 验证

全量构建 43/43 target 通过（含全部 sample，证明仓库内无残留调用方）；残留引用 grep 全空；回归 crosswindow 74/74、model 920/0、kit PASS、c_api 32/0、B 22/22、A 21/21、game_editor 冒烟正常；`ctest` 5/5，**截图基线零变化**。

> 说明：这是公开头文件里的 API 移除。仓库内无调用方，但若存在仓库外宿主直接调用 `set_viewports_enabled()` / `docking()`，需同步移除该调用（本就无效果）。

---

## 17. 实施记录：主窗口 OS 矩形持久化（2026-11-16）

> 落实审计确认的遗留项 §12.6「边界」——主窗口自身 OS 矩形此前未保存/恢复，只持久化了浮动窗口矩形。

### 17.1 缺口定位：序列化层已有，宿主层未接线

- **序列化层（无需改动）**：`editor_window_layout` 早有 `has_rect/x/y/width/height`，`editor_layout_set_to_json` / `_from_json` 已完整读写 `windows[i].rect`（v8 起就是格式字段）。v9/v10 文件兼容，v1–v7 导入为 `id="main"` 且无 rect。
- **宿主层（本次改动）**：`vgui_game_editor.cpp` 的 `save_editor_layout()` 未采集主窗口矩形，`load_editor_layout()` 未应用它。补的正是注释所说「restoring them is the host's job」。

### 17.2 改动清单（仅 sample/vgui_game_editor.cpp，+73 行）

| 函数 | 作用 |
|---|---|
| `capture_window_rect(win,…)` | SDL_GetWindowPosition/Size 采集屏幕点矩形（含边框），过滤最小化窗口（SDL 返回 -32000 之类的屏幕外坐标），复用 `editor_state::main_window` 无需改签名 |
| `restore_window_rect(win,x,y,w,h)` | 遍历显示器可见区（SDL_GetDisplayUsableBounds）：矩形与任一可见区有交集才应用（SDL_SetWindowSize + SetWindowPosition），否则返回 false 由调用方忽略——防换分辨率/拔屏后窗口消失 |
| `save_editor_layout()` | 主窗口矩形写入 `windows[0].rect` |
| `load_editor_layout()` | 先恢复 OS 矩形，再 `apply_layout()`；`host.begin_frame()` 每帧从窗口像素尺寸同步 UI context，当帧按新尺寸重排各 Dock 区 |

**不升格式版本**：rect 字段自 v8 就在 JSON 中，v10 `current_version` 不变；旧文件 `has_rect=false` 时 load 跳过，行为不变。core / 序列化层 / 其他 demo 零改动。

### 17.3 验证（真实系统级窗口操作，非合成事件）

用系统 API `MoveWindow` 把主窗口移到自定义矩形 → `WM_CLOSE` 正常退出 → 检查 JSON `windows[0].rect` → 重启检查恢复 → 手改 JSON 为 (-50000,-50000) 检查忽略：

- **SAVE PASS**：rect 落盘（x/y/width/height 均非默认）。
- **LOAD PASS**：重启后主窗口恢复到保存矩形（窗口外框含标题栏，与保存的 client 矩形差一个装饰边距，符合预期）。
- **OFFSCREEN PASS**：越界矩形打印 `layout ignored off-screen main window rect` 并被忽略，窗口保持在可见范围。

回归：`ctest` 5/5；B floater demo 22/22；crosswindow 74/74（序列化层未动）；game_editor SIM_DRAG floaters=1；kit PASS（debug）；release/debug 全量构建通过。

> 坑：Python `SetProcessDPIAware()` 后 `MoveWindow` 用物理像素，而 SDL 的 `GetWindowSize` 是 client 点且不含边框——验证数值会有固定偏移，断言要比对「保存值 → 恢复值」而非「注入值 → 保存值」。
