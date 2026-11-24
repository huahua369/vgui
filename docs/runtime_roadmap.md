# vgui 游戏运行时 UI 路线图

> 状态：R0–R6 主体已完成（R2-3 待实机手柄确认；R4-3 按需）
>
> 适用版本：vgui 0.3.x
>
> 相关文档：[游戏编辑器组件路线图](editor_roadmap.md)（编辑器方向，已收口）、[Phase 0：3D 场景编辑器基础](phase0_3d_editor.md)

## 1. 目标与边界

### 1.1 目标

把 vgui 用作 **2D/3D 游戏引擎的运行时 UI**（HUD、游戏内菜单、暂停界面、调试覆盖层）：

- 引擎拥有主循环与渲染器，vgui 只负责控件树、布局、输入路由与矢量绘制录制。
- 渲染对接走两条路：直接消费 OVG 的 `ovg_draw_data_t`（引擎自有渲染器），或在 SDL3 GPU 下复用 vgpu 并通过纹理互借与引擎交换画面。
- 运行时只依赖核心（`vgui::vgui`，不含 SDL3 后端与 editor kit）；chart 可选用于仪表/小地图。
- 与编辑器方向共用同一套核心 C API，不为运行时分叉出第二套控件模型。

### 1.2 非目标

- 不提供游戏场景图、物理、音频或资源管线。
- 不在核心内实现游戏逻辑绑定框架（数据绑定由宿主每帧写控件状态完成，见 R4）。
- 不承诺脚本/热重载层。
- 不追求跨引擎抽象渲染层；SDL3 GPU 之外的自定义后端由引擎按 `ovg_draw_data_t` 自行消费。

## 2. 现状（R0 之前的差距盘点）

| 能力 | 状态 |
|---|---|
| 核心 C API 无窗口运行 | 基本可用，但双击计时依赖 `SDL_GetTicks`，后端源码被强制编进 `libvgui` |
| HUD 合成 | UI pass 固定 `LOADOP_CLEAR`，无法叠加在引擎 3D 画面之上 |
| 手柄输入 | 无（仅鼠标/键盘/滚轮/单指触摸/IME/文件拖放） |
| 每帧开销 | 保留模式布局按需重排，但绘制每帧全量重录，无显示列表缓存 |
| 帧顺序约束 | phase0_3d_editor.md 已有 3D 离屏纹理→UI 的模式，HUD 反向合成未打通 |

## 3. R0 运行时基座（已完成）

- [x] **R0-1 时间源注入**：`vgui_context_set_time_fn(ctx, fn, user)` 注入单调时钟（秒），双击计时不再触碰 OS API；缺省回退 `std::chrono::steady_clock`。实现于 `gui/vgui.cpp`（`context_now_seconds`）。
- [x] **R0-2 核心去 SDL 化**：`gui/vgui.cpp` 不再包含 SDL/后端头；`gui_viewport::trigger`、`vgui_dispatch_dev_event`、SDL 键码翻译搬入 `backends/vgui_sdl3.cpp`；剪贴板经 `vgui_internal.h` 的函数指针桥接（core-only 下复制/粘贴为空操作）。
- [x] **R0-3 VGUI_CORE_ONLY 构建**：CMake 选项 `VGUI_CORE_ONLY=ON` 时 `libvgui` 仅编 core（anim/vgui/mapView），不编 SDL3 后端与 win_core，不链 SDL3，且 ovg 自带的 SDL3 渲染器一并关闭；headless 测试（embed/c_api/scene_json/chart/kit）在该配置下照常构建运行。见 `backends/CMakeLists.txt`、`CMakeLists.txt`、`bld_core.bat`。
- [x] **R0-5 字体 C 接口与打包字体**：ovg_c.h 新增 `load_font_from_path / add_font_dir / load_font_from_memory / get_sys_family` 四个 C 函数（操作 `font_cache_cx`）。`load_font_from_memory` 由桩实现补齐：harfbuzz 从内存建 face（DUPLICATE 拷贝，调用后即可释放缓冲），直接登记到 `_familys_name[name]`，`new_font_family` 无需刷新即可按 name 找到，**窗口模式出字形已验证**（runtime_hud 示例即用打包字体）。`load_font_from_path` 给出 name 时同步按 name 登记；path/dir 的 fontconfig 登记需 `get_sys_family()` 刷新后按内部族名查找。
- [x] **R0-4 无窗口嵌入验证**：`sample/vgui_runtime_embed_test.cpp`——无窗口/无 GPU/无字体/无 SDL，跑通"建树→注入事件→update→draw→消费 `ovg_draw_data_t`"最小嵌入循环（含注入时钟驱动的双击、取消按下状态、绘制命令数断言）。

验收：`VGUI_CORE_ONLY=ON` 与 OFF 两种配置构建通过；`vgui_runtime_embed_test` 全绿；既有测试基线不回归。

## 4. R1 HUD 合成（已完成）

- [x] **R1-1 load-op 公共 API**：`vg_fbo_t::clear`（默认 true）。置 false 后 UI pass 以 `SDL_GPU_LOADOP_LOAD` 打开，HUD 直接 blend 在引擎已渲染的 swapchain 内容之上（`backends/vgui_renderer_sdl3.cpp` `begin_pass`）。
- [x] **R1-2 引擎侧帧内编码接口**：`vgpu_current_command_buffer(ctx)` 返回 vgpu 当前命令缓冲（`vgpu_acquire_swapchain` 获取、`vgpu_render_frame` 提交），引擎可在两者之间用 `SDL_BlitGPUTexture` 把 3D 场景 blit 进 swapchain，保证 blit 与 UI pass 同一提交内有序；`vgpu_color_format(ctx)` 供离屏 FBO 与原生纹理借用对齐格式。
- [x] **R1-3 UI→引擎纹理路径**：HUD 渲入离屏 FBO（`vgpu_create_fbo(ctx, w, h, nullptr)`），再经 `vgpu_texture_init_native/update_native` 把 `vgpu_fbo_color_texture()` 借为 ovg 纹理，由引擎合成 pass 当普通图像采样。
- [x] **R1-4 HUD 示例**：[vgui_runtime_hud.cpp](../sample/vgui_runtime_hud.cpp)——编辑器 JSON 驱动血量/护盾、状态栏、技能栏和冷却条；3D 场景、小地图与准星由宿主绘制。M 切换两种合成模式，`--smoke` 渲染 40 帧后退出。

### 4.1 帧顺序约束（运行时集成必读）

- 3D 与 UI 共用一个 `SDL_GPUDevice`，各自提交命令缓冲；帧内顺序：3D pass → （blit/合成）→ UI pass。
- **一帧内的所有 vgpu pass 走一次 `vgpu_render_frame()` 调用**（每个 pass 目标一个 `ovg_draw_data_t` 条目，按渲染顺序排列）。SDL3 不保证多次提交之间的执行串行——把一帧的 pass 拆成多次提交，会在共享顶点缓冲（每次上传都从偏移 0 开始）和"上一提交写入、下一提交采样"的渲染目标上产生竞态（表现为错位乱块/闪烁）。单次提交内 SDL3 的自动屏障会正确排序（离屏写入→采样、blit→LOAD）。
- 每帧重写并在同帧采样的离屏 FBO，设置 `vg_fbo_t::cycle_target = true`（SDL3 在目标仍被上一帧占用时自动轮换后备存储）。
- **MSAA 下 LOAD 的实现细节**：窗口 FBO 带 MSAA 时 UI 渲进 `color_msaa_tex`、pass 结束 resolve 整体覆盖 swapchain——若引擎先 blit/渲染场景进 swapchain，UI pass 必须先把它 preload 进 MSAA 纹理，否则每帧背景为未定义内容（闪烁）。vgpu 的 `begin_pass` 已内置该 preload（MSAA + LOAD 时自动 blit swapchain→MSAA 纹理），无需宿主操作。
- **不要用 `SDL_UploadToGPUBuffer(..., cycle=true)` 修顶点上传竞态**：实测本机/驱动上 cycling 会把窗口内容整体清黑（机制未查明）；顶点/索引上传保持 offset-0 + cycle=false，依赖 vsync 帧排空；若未来需要一帧多次提交，改用 vgpu 侧环形缓冲。
- **UI pass 以 LOAD 打开时不得 CLEAR**：`vg_fbo_t::clear = false` 后，该 FBO 的既有内容由宿主负责（模式 A 下 = 引擎 blit 进 swapchain 的场景）。
- 模式 B 下离屏 HUD FBO 每帧 `clear=true`（清成透明黑），由合成 pass 混合。
- 纹理创建/更新与绘制保持在同一线程；native 纹理句柄地址在其 draw data 被消费前保持稳定（见 `ovg_c.h` `ovg_texture_t` 注释）。
- 引擎若用自己的渲染器：在 `vgui_draw(ctx, cb, vg)` 后取 `get_draw_list(vg)` 获得 `ovg_draw_data_t`，按 `gcmd_t` 列表自建管线消费（参考 `backends/vgui_renderer_sdl3.cpp`）。

### 4.2 引擎最小嵌入循环（无 SDL）

```c
vgui_context* ui = vgui_create(font_or_null, 16.0f);
vgui_context_set_time_fn(ui, game_clock, engine);   /* 可选：游戏时间驱动双击 */
vgui_context_set_size(ui, w, h);
/* 建树 … */
/* 每帧： */
vgui_input_mouse_move(ui, x, y);                    /* 引擎喂输入 */
vgui_update(ui, delta);
vgui_draw(ui, cb, vg);
ovg_draw_data_t dd = get_draw_list(vg);             /* 交给引擎渲染器 */
```

参考实现：`sample/vgui_runtime_embed_test.cpp`。

### 4.3 用编辑器设计替换 HUD

在编辑器中打开 [game-hud.vgui.json](../sample/res/game-hud.vgui.json) 并保存，或用 **文件 → 导出交付包** 导出成目录或单文件 JSON。示例不依赖编辑器外壳。无参数时加载仓库里的设计文件；传路径时可以直接加载设计文件、交付目录或 `*.vgui.package.json`。后两种会先验证交付包。

```text
bld_full.bat vgui_runtime_hud
out/bin/x64d/vgui_runtime_hud.exe
out/bin/x64d/vgui_runtime_hud.exe path/to/arena.vgui.package.json
out/bin/x64d/vgui_runtime_hud.exe --smoke path/to/arena-delivery
```

沿用模板中的运行时 ID：`hp`、`shield`、`ult`、`cooldown1` 至 `cooldown6` 是进度条（值域 0–1），`status` 是状态标签。`minimap`、`crosshair` 为留给宿主绘图的面板（纵向布局，保留尺寸）；根节点保持 `absolute`，X/Y 和宽高为 0。技能栏贴底居中、小地图在安全区内、准星居全屏中心，这些规则都来自设计文件的 `anchor_x`、`anchor_y` 和 `anchor_safe_area`，宿主不再逐个控件计算坐标。自定义宿主提供视口尺寸、DPI 和逻辑单位的安全边距（`vgui_context_set_safe_area`），然后调用 `runtime::layout()`；与编辑器预览使用同一套布局规则。

点击技能按钮会把 `hud.skill.1` 至 `hud.skill.6` 交给宿主；对应 Q/W/E/R/T/Y 快捷键，U 触发大招冷却，B 在 0、60、100 逻辑单位的安全边距间切换。宿主只挂载一次并更新实时进度，不写回设计。世界空间名牌和公告牌仍属于 3D 示例的宿主代码。`ctest -R runtime_hud_design` 无需打开窗口，检查技能动作、普通宽屏和 21:9、非对称安全区、准星位置及重排前后的运行态值。

技能冷却、血量和护盾现在由 HUD 的 `bind_value` 指定字段。宿主在输入派发结束后发布数据，例如：

```cpp
hud.set_data("player.hp_ratio", vgui::ui_design::value{0.65});
hud.set_data("player.hp_ratio", std::nullopt); // 回到设计进度
```

文本、进度、显隐、启用分别接受字符串、数值、布尔值、布尔值。不认识的字段或错误类型返回 `false`；相同值不会重复刷新。更新不重挂、不写回设计，重新挂载时由宿主重新发布。技能规则、冷却计时和输入映射继续属于游戏宿主，设计稿只描述界面用哪些数据。`--check-design` 检查本示例的绑定合约；旧 HUD 设计未绑定的进度条仍可在正常运行和窗口烟测中使用。

绑定的进度条可以用 `bind_value_transition_ms` 单独设置显示过渡（0–5000 毫秒，默认 0）。可给 `hp` 和 `shield` 分别设置，冷却条继续即时显示。`set_data()` 立即记录字段值；变化中的进度条从当前显示值接续，保留各自的限幅和步进。宿主用 `advance_animations(dt)` 推进（秒），也可只调用 `advance_data_transitions(dt)`；同一帧不要重复调用两者。需要跳过过渡时用 `data_update::immediate`，清除字段则直接恢复设计值。`inspect_data()` 可区分发布值和当前显示值。

## 5. R2 手柄/键盘方向导航（已实现；焦点环待接实机手柄确认）

> **实施记录**：R2-1 实际 API 为 `vgui_nav_direction`（UP/DOWN/LEFT/RIGHT，`vgui_direction` 名已被 RTL 方向占用）+ `vgui_focus_navigate`；环绕含横向偏移 tie-break（同主轴距离取横轴最小者，否则网格环绕落到 0 列）。验收：model_test test_focus_navigate 12 断言（独立上下文 3x3 网格，避免残留控件成为环绕候选）全过。R2-2 如设计实现（pump_gamepad 每帧泵）。

目标：主机式 UI 基础——手柄/键盘在不依赖指针的情况下遍历并激活全部 HUD 控件。

### 5.1 R2-1 core 方向导航（P0，M）

- **API**：`vgui_nav_direction`（UP/DOWN/LEFT/RIGHT）+ `vgui_focus_navigate(ctx, dir)`；返回 1 表示焦点移动了。
- **实现**（gui/vgui.cpp，与 vgui_focus_next_impl 同层）：
  - 候选集：全树 `focusable && visible && enabled` 控件的中心点（abs 几何），先 `vgui_ensure_layout` 保证几何有效；枚举方式对齐 vgui_focus_next_impl 的现有做法。
  - 打分：以焦点控件中心为原点，主轴位移 >0 且 |横轴位移| ≤ 主轴位移 ×1.5 的候选入锥；得分 = 主轴位移 + 2×|横轴位移|，最小者胜。
  - 环绕：锥内无候选时，取反方向最远者（屏幕边缘环绕）。
  - 边界语义：纯几何方向，不做 RTL 特判（几何"左"即视觉左）；list/tree/slider 聚焦时的方向键行为维持现状（进入控件内部导航，见 R2-2 的宿主侧约定）。
  - **不做**：不改核心对方向键的现有绑定（列表/滑杆的方向键已有语义）；`vgui_focus_navigate` 仅供宿主/手柄层显式调用。
- **验收**：model_test 新增 test_focus_navigate——固定坐标按钮网格断言四方向序列、环绕、跳过 disabled/hidden/0 尺寸控件；RTL 下几何语义不变。

设计稿可在根节点设置 `nav_start`，在控件上设置 `nav_up` / `nav_down` / `nav_left` / `nav_right`，值为当前页的节点 ID。原生手柄方向输入优先采用当前可用的显式目标，其余情况使用原几何算法，不改变 Tab 顺序。宿主也可在首次布局完成后调用 `runtime::focus_default()` 尝试默认焦点；如果已经有焦点或目标不可用，不会抢走焦点。[`game-hud.vgui.json`](../sample/res/game-hud.vgui.json) 示例以 `Q` 为默认焦点，并配置 `Q` 与 `Y` 两端的方向跳转。

游戏模态使用 `modal`（根节点直属面板）、`open_modal`（外部按钮指定面板 ID）、`close_modal`（内部按钮）和可选 `modal_return_focus`。宿主应在派发完输入、开始绘制前调用 `runtime::service_modals()`（如果使用页面栈，则调用 `runtime::service_navigation()`）；Esc 与手柄 B 复用原生返回路径。关闭时按指定目标、打开按钮、页面初始焦点的顺序恢复，不往隐藏、禁用或被裁剪的目标赋予焦点。`vgui_runtime_hud` 与 `vgui_ui_json_demo` 已接入，配置随交付包保留，打开状态不保存。

#### 独立页面切换和返回

每页仍是独立的设计或交付文件。宿主先 `mount` 首页并 `layout`，可用 `focus_default` 应用首页的 `nav_start`。在 `on_action` 回调中只记录导航意图；派发完成后由宿主按自己的动作映射选择文档，再调用 `push_page(doc, revision, bindings, &error)`。页面动作键不解释为路径；每一页单独提供资源根或内嵌资源加载器。`mount` 成功时替换整栈；`push_page` 失败不改变当前页。

入栈后，只挂接目标页并尝试其 `nav_start`。上一页保留输入、滚动、已发布绑定值和控件实例；挂起时不会推进运行时的数据过渡和渐变动画。`widget`、布局快照、诊断和绑定 API 都只面向活动页，即使不同页面复用控件 ID 也彼此独立。最多保留 16 页（含首页）。

`pop_page()` 返回并销毁离开的页面，按最新视口重新布局上一页；优先恢复离开前的焦点，不可用则尝试 `nav_start`，否则留空。弹窗、菜单和浮层打开时不会切页。每帧在派发完输入后调用一次 `service_navigation()`（它也处理模态命令）；无修饰键的 Esc 或 SDL 手柄 B 优先交给原生控件和覆盖层，未被处理且存在上一页时才返回。回到首页后不再拦截返回键。参见 [C++ JSON 示例](ui_editor_roadmap.md#直接运行设计-jsonc-示例)。

### 5.2 R2-2 SDL_Gamepad 映射（P0，M）

- **实现**（backends/vgui_sdl3.cpp，app_mgr）：
  - `SDL_InitSubSystem(SDL_INIT_GAMEPAD)`；处理 SDL_EVENT_GAMEPAD_ADDED/REMOVED（开/关手柄）；每帧在 get_event 内泵状态。
  - 映射表（纯函数，可单测）：A(南)→注入 `VGUI_KEY_ENTER`（复用现有 Enter 激活路径）；B(东)→注入 Esc 按下（未处理时回退到 `vgui_input_cancel`）；Y(西)→菜单键（复用 context menu 路径）；Start→`VGUI_KEY_F10`（menubar）；十字键+左摇杆（死区 0.35 数字化）→`vgui_focus_navigate`，重复率：首延迟 350ms、重复 140ms。
  - 摇杆聚焦滑杆时的约定：焦点控件为 slider 且方向沿其轴向时，宿主层改为 `vgui_widget_set_value` 步进（文档化，不做进 core）。
- **验收**：映射表单测（按钮状态→动作序列）；接手柄人工验证 HUD 示例全程可手柄操作（导航/激活/返回/菜单/热键栏聚焦）。

### 5.3 R2-3 焦点环确认（P1，S）

- 手柄导航经既有焦点系统设置焦点，现有 focus ring（focus_alpha）自动生效；本项仅做 HUD 示例验证 + 必要的样式微调，无新 API。

验收（里程碑）：`vgui_runtime_hud` 不碰鼠标键盘、仅手柄可完成模式切换/技能/导航/激活。

## 6. R3 运行时性能（已实现）

> **实施记录与实测结论**：R3-2 `vgui_runtime_bench` 已交付（离屏 FBO、BENCH_FRAMES/BENCH_WIDGETS/BENCH_CACHE）。R3-1 机制完整落地（hint opt-in、外层 replay/录制包装、链式失效、图集代数失效安全网），**实测 40 静态面板 draw 约 17% 收益（7.2→6.0ms）**——简单矩形+文本内容收益有限（回放 memcpy ≈ 重录成本），重收益预期在 tess2 重的圆角/复杂路径内容。**实施中发现的两个坑**：(1) 录制段内图集页增长会触发 ovg 向量越界 abort——宿主必须先无 hint 预热一帧再启用录制；(2) 图集上传帧须失效全部缓存（vgui_recording_atlas_check 已内建于 vgui_draw）。基线（widgets=0）：draw 3.3ms / submit 7.0ms。

目标：把每帧全量重录的 CPU 成本压到 HUD 规模可控，并给出可复查的数字。

### 6.1 R3-2 运行时基准（P0，S，**先行**——先有基线再优化）

- **实现**：新增 `sample/vgui_runtime_bench.cpp` + CMake target（不注册 CTest，避免 CI 抖动）。无窗口离屏 FBO 渲染合成 HUD 场景（状态板 + 热键栏 + 24 名牌 + N 个静态面板，N 可调）。环境变量：`BENCH_FRAMES`（默认 600）、`BENCH_CACHE`（0/1）、`BENCH_WIDGETS`。runtime_cx 分 update/nameplates/draw/submit 四段计时，输出 avg/max。
- **验收**：跑通并产出基线数字表（初始版本入 roadmap）。

### 6.2 R3-1 静态子树显示列表缓存（P0，L）

- **API**：`vgui_widget_set_recording_hint(w, int)`——按子树 opt-in（宿主标记静态面板），不做全局自动缓存。
- **机制**（gui/vgui.cpp 绘制路径）：
  - 进入带 hint 的控件时，若子树"干净"且已有录制 → `cb->replay(vg, rec)` 替代递归重录；否则 `cb->start_recording` 录制子树、`stop_recording` 存 `ovg_recording_t*`（ovg_ctx_cb 已有 start/stop/replay/recording_destroy）。
  - 失效/校验（保守）：录制时记录 (abs_x, abs_y, 当前裁剪矩形, ctx 级 epoch)；每帧重放前校验——ctx epoch（主题/样式/语言/ui_scale 变更递增）、控件 abs 几何、裁剪矩形一致；子树内任一控件有活动动画（hover/press/check anim 处于 (0,1)）或焦点环在子树内 → 本帧不重放、重录。
  - 生命周期：invalidate/控件销毁时 `recording_destroy`。
- **验收**：headless 测试断言"二次绘制走 replay、set_text 后重录"；E03 截图回归在 runtime_hud 热键栏开缓存后 diff=0（重放渲染与逐帧重录像素一致）；R3-2 基准对比数字。

### 6.3 R3-3 帧预算基线（P1，S，已完成）

- [x] 离屏 FBO 1280x800、MSAA 1、24 名牌 + 热键栏 + 状态板 + DejaVu 16px；单次跑分含机器波动，A/B 结论需交错跑：

  | 场景 | update | draw | submit |
  |---|---|---|---|
  | 无静态面板 | 0.07 ms | 2.6 ms | 6.0 ms |
  | 20 静态面板 | 0.10 ms | 4.7 ms | 9.7 ms |
  | 40 静态面板 | 0.13 ms | 6.7 ms | 13.5 ms |
  | 80 静态面板 | 0.16 ms | 5.6 ms | 11.5 ms |
  | 40 面板 + 子树缓存 | 0.11 ms | ~5.4-7.2 ms | 11.9 ms |

  结论：update 全程 <0.2ms 可忽略；draw/submit 随面板数近线性，40 面板约 20ms 合计（60fps 预算内、120fps 需子树缓存与图集预热配合）；子树缓存对简单矩形+文本内容收益约 17%（draw），重收益需 tess2 重的圆角/复杂路径内容。

## 7. R4 HUD 控件包（R4-1/R4-2 已实现；R4-3 按需）

> **实施记录**：R4-1 `vgui_widget_set_text` 已补同值短路（相同文本且无 text_key 时直接返回，不 mark_dirty、不失效录制缓存）；set_value 的 1e-6 脏检查为既有行为。绑定惯例已在 runtime_hud 名牌/血条/蓝条全帧直写模式上验证。R4-2 三件全部落地并截屏验证：(1) **冷却环**——`vgui_widget_set_radial(w, 1)`，progress 绘制分支切换为圆环轨道 + 12 点钟起顺时针 accent 弧 + 居中百分比文字（runtime_hud 热键栏右侧 ult 环演示，U 键触发 5s 冷却）；(2) **飘字**——`vgui_spawn_float_text(ctx, x, y, text, color, lifetime, rise)`，上浮 + 按 life/max_life 透明度衰减 + 过期回收，上限 64 条，绘制于全部 HUD 内容之上（技能键触发冷却时在槽位上方飘 "3.0s"，SendKeys 注入已验证）；(3) **安全区**——`vgui_context_set_safe_area/get`（逻辑 px，负值钳 0），`vgui_widget_set_anchor` 可选择在父内容区与安全区的交集中布局，HUD 用 B 键循环演示。

> **实施记录**：R4-1 `vgui_widget_set_text` 已补同值短路（相同文本且无 text_key 时直接返回，不 mark_dirty、不失效录制缓存）；set_value 的 1e-6 脏检查为既有行为。绑定惯例已在 runtime_hud 名牌/血条/蓝条全帧直写模式上验证。

### 7.1 R4-1 运行时数据绑定惯例（P0，S）

- 核查并（如缺）补齐：`vgui_widget_set_value` 已按 1e-6 脏检查（现有 changed 分支）；确认 `vgui_widget_set_text` 同值短路，未短路则补（避免无谓重排/重录）。
- **验收**：model_test 断言"同值 set 1000 次不置 layout_dirty"；roadmap/README 记录绑定惯例（每帧直写控件状态 + 脏检查）。

### 7.2 R4-2 HUD 型控件（P1，M）

- **冷却环**：progress 的径向模式——`vgui_widget_set_radial(w, int)`（或 progress style 字段）；绘制：圆环轨道（描边圆）+ 按值扫过的弧（ovg arc API）+ 可选居中文字。验收：截图回归场景 + model_test 值映射断言。
- **飘字**：context 级 API `vgui_float_text_spawn(ctx, x, y, text, color, lifetime, rise)`；vgui_update 内推进（上浮 + 透明度衰减 + 过期回收），vgui_draw 末尾绘制（覆盖层之下、tooltip 之上），上限 64 条。验收：model_test 生成/过期断言；HUD 示例热键触发冷却时飘 "+3s"。
- **安全区**：`vgui_context_set_safe_area(ctx, l, t, r, b)` + get；宿主锚定自行钳制（HUD 示例 layout_anchors 接入）。核心覆盖层（toast/modal）避让为 P2。

### 7.3 R4-3 多点触摸（P2，L，按需）

- SDL 多指 → 手势识别（双指距离增量=捏合、质心位移=平移），注入为滚轮/缩放事件；前置条件是可缩放表面，暂缓。

## 8. R5 世界空间 UI（已完成）

- [x] **R5-0 屏幕空间名牌（世界锚点投影）**：宿主每帧用与 3D 相机一致的投影矩阵把世界锚点投到屏幕坐标，`vgui_widget_set_pos` 移动对应控件组，背面/出屏用 `vgui_widget_set_visible` 隐藏。配套新增 `vgui_widget_set_accent / set_track`（每控件条形图配色，0=继承主题）。参考实现：`sample/vgui_runtime_hud.cpp`（12 个 Unit 名牌 + 血条/蓝条跟随方块公转，投影参数与 phase0 相机一致：RH_ZO + Y 翻转）。
- [x] **R5-1 UI→3D 面板**：离屏 UI 纹理贴到世界空间公告牌。phase0 新增 `phase0_renderer_set_billboard(renderer, texture, pos, w, h)`——复用场景片元着色器（p0_vertex 本就带 uv + scene_tex 采样）与 grid 混合管线（alpha 混合、深度测试开/写入关），CPU 侧按相机基（view 矩阵行向量）重建四边形顶点，每帧上传。宿主侧：独立小 UI 上下文渲进专用 FBO（`cycle_target=true`，phase0 采样上一帧内容，一帧延迟无碍）；与主 HUD/模式 B 合并进**一次 `vgpu_render_frame` 多 item 提交**（面板 item 在前）。注意：离屏 rvg 的 `width/height` 必须手动设置（与 FBO 尺寸一致），否则正交映射错位、文字落到视口外。参考实现：`sample/vgui_runtime_hud.cpp`（"ARENA STATUS" 聚合血条/蓝条公告牌）。
- [x] **R5-2 世界拾取与选中**：核心新增 `vgui_widget_set_input_enabled(w, 0)`——控件及子树整体跳过命中测试（保留正常视觉，区别于 set_enabled(0) 的禁用态），世界悬浮指示器靠它放行点击。宿主侧：根面板 `vgui_widget_on_event` 收未命中的 CLICK，反投影成射线（inverse VP，near/far 两点）对实体包围球求交（glm::intersectRaySphere），最近者 `select_unit` 联动名牌高亮（border + 名字色）；右键清除选中。自测内嵌 smoke（注入点击→断言选中→右键→断言取消）。参考实现：`sample/vgui_runtime_hud.cpp`。

## 9. R6 持久离屏渲染目标（P0 已完成）

> **实施记录**：新增后端公共 API `include/vgui/backend/vgui_render_target.h` 与实现 `backends/vgui_render_target.cpp`。一个目标拥有一个离屏 `vg_fbo_t`、一个稳定的 `vgui_texture` 和一个私有 `rvg_t`；`record()` 输出可直接并入宿主的一次 `vgpu_render_frame()`，`resize()` 原地更新纹理句柄，`destroy()` 等待 idle 后立即解绑原生纹理。2026-11-14 起离屏目标可选携带 D32 深度附件（`desc.depth` + `native_depth_texture()`），供宿主 3D pass 绑定；详见 [`render_target_roadmap.md`](render_target_roadmap.md) §4.4。

- [x] **R6-1 目标生命周期**：`create / destroy / resize / set_clear / get_size`；格式跟随 `vgpu_color_format()`，纹理指针在 resize 后保持不变；创建失败可通过 `vgui_render_target_last_error(NULL)` 查询。
- [x] **R6-2 录制接口**：`record()`（原始矢量回调）与 `record_context()`（`vgui_context`，尺寸必须匹配）只产生 `ovg_draw_data_t`，不提交；`submit()` / `submit_context()` 仅作为静态/独立场景的便利入口。
- [x] **R6-3 帧顺序与所有权**：一帧内所有 pass 仍需一次 `vgpu_render_frame()`；`clear=1` 默认清屏并启用 `cycle_target`，`clear=0` 保留内容且关闭轮换；`ovg_ctx_cb*` 为借用，必须比所有录制过的上下文活得久。
- [x] **R6-4 验证**：`sample/vgui_render_target_test.cpp` 覆盖非法创建、原始矢量、上下文→纹理→图片→合成、resize 指针稳定、尺寸不匹配拒绝、`clear=0` 累积与 `submit()`；CTest 名称 `vgui_render_target`，无 Vulkan 设备返回 77。
- [x] **R6-5 构建隔离**：`vgui_render_target.cpp` 只在 `NOT VGUI_CORE_ONLY` 下编译；公共头自包含检查通过；`VGUI_CORE_ONLY=ON` 不出现该接口。

验收：全量构建与 core-only 构建通过；`vgui_render_target` 测试通过；既有测试/截图基线不回归。

## 10. 关键依赖与批次

- R0/R1/R5/R6 已完成，无外部依赖。
- R2 批次：R2-1（core 导航，P0）→ R2-2（手柄映射，依赖 R2-1）→ R2-3（确认，S）。R2-2 的映射表可先行单测。
- R3 批次：R3-2（基准，**先行建立基线**）→ R3-1（子树缓存，L，依赖基线与 ovg 录制/回放 API——已存在未接入）→ R3-3（文档，依赖 R3-2）。
- R4 批次：R4-1（绑定惯例核查，S，先行）→ R4-2（冷却环/飘字/安全区）→ R4-3（多点触摸，P2 按需）。
- R6 批次：R6-1（目标生命周期）→ R6-2（录制接口）→ R6-3（帧顺序文档）→ R6-4（离屏测试）→ R6-5（core-only 隔离）。
- 跨项：R4-2 冷却环与 R5-0 名牌共用 set_accent/set_track；R3-1 缓存对 R4 飘字/动态控件默认不生效（opt-in 语义）；R6 可替代 R1-3/R5-1 中手写的 FBO + native texture 桥。

## 11. 维护方式

与 [editor_roadmap.md 第 10 节](editor_roadmap.md)一致：每完成一项把复选框改为 `[x]` 并补充实现位置与测试名；新条目沿用 R 前缀，标注 P0/P1/P2 与 S/M/L；每个里程碑在 `vgui_runtime_hud` 示例中补一条可操作路径，确保能力不是"只在测试里存在"。
