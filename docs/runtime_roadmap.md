# vgui 游戏运行时 UI 路线图

> 状态：R0、R1 已完成
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
- [x] **R1-4 HUD 示例**：`sample/vgui_runtime_hud.cpp`——3D 场景 + 血条/护盾、小地图与准星（`VGUI_CUSTOM`）、快捷栏冷却条、状态面板；TAB 切换两种合成模式；`--smoke` 渲染 30 帧（两种模式各 15 帧）自动退出。

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

## 5. R2 手柄/键盘方向导航（R2-1/R2-2 已实现；R2-3 待实机验证）

> **实施记录**：R2-1 实际 API 为 `vgui_nav_direction`（UP/DOWN/LEFT/RIGHT，`vgui_direction` 名已被 RTL 方向占用）+ `vgui_focus_navigate`；环绕含横向偏移 tie-break（同主轴距离取横轴最小者，否则网格环绕落到 0 列）。验收：model_test test_focus_navigate 12 断言（独立上下文 3x3 网格，避免残留控件成为环绕候选）全过。R2-2 如设计实现（pump_gamepad 每帧泵）。

目标：主机式 UI 基础——手柄/键盘在不依赖指针的情况下遍历并激活全部 HUD 控件。

### 5.1 R2-1 core 方向导航（P0，M）

- **API**：`vgui_direction`（UP/DOWN/LEFT/RIGHT）+ `VGUI_API int vgui_focus_navigate(vgui_context* ctx, vgui_direction dir)`；返回 1 表示焦点移动了。
- **实现**（gui/vgui.cpp，与 vgui_focus_next_impl 同层）：
  - 候选集：全树 `focusable && visible && enabled` 控件的中心点（abs 几何），先 `vgui_ensure_layout` 保证几何有效；枚举方式对齐 vgui_focus_next_impl 的现有做法。
  - 打分：以焦点控件中心为原点，主轴位移 >0 且 |横轴位移| ≤ 主轴位移 ×1.5 的候选入锥；得分 = 主轴位移 + 2×|横轴位移|，最小者胜。
  - 环绕：锥内无候选时，取反方向最远者（屏幕边缘环绕）。
  - 边界语义：纯几何方向，不做 RTL 特判（几何"左"即视觉左）；list/tree/slider 聚焦时的方向键行为维持现状（进入控件内部导航，见 R2-2 的宿主侧约定）。
  - **不做**：不改核心对方向键的现有绑定（列表/滑杆的方向键已有语义）；`vgui_focus_navigate` 仅供宿主/手柄层显式调用。
- **验收**：model_test 新增 test_focus_navigate——固定坐标按钮网格断言四方向序列、环绕、跳过 disabled/hidden/0 尺寸控件；RTL 下几何语义不变。

### 5.2 R2-2 SDL_Gamepad 映射（P0，M）

- **实现**（backends/vgui_sdl3.cpp，app_mgr）：
  - `SDL_InitSubSystem(SDL_INIT_GAMEPAD)`；处理 SDL_EVENT_GAMEPAD_ADDED/REMOVED（开/关手柄）；每帧在 get_event 内泵状态。
  - 映射表（纯函数，可单测）：A(南)→注入 `VGUI_KEY_ENTER`（复用现有 Enter 激活路径）；B(东)→`vgui_input_cancel`；Y(西)→菜单键（复用 context menu 路径）；Start→`VGUI_KEY_F10`（menubar）；十字键+左摇杆（死区 0.35 数字化）→`vgui_focus_navigate`，重复率：首延迟 350ms、重复 140ms。
  - 摇杆聚焦滑杆时的约定：焦点控件为 slider 且方向沿其轴向时，宿主层改为 `vgui_widget_set_value` 步进（文档化，不做进 core）。
- **验收**：映射表单测（按钮状态→动作序列）；接手柄人工验证 HUD 示例全程可手柄操作（导航/激活/返回/菜单/热键栏聚焦）。

### 5.3 R2-3 焦点环确认（P1，S）

- 手柄导航经既有焦点系统设置焦点，现有 focus ring（focus_alpha）自动生效；本项仅做 HUD 示例验证 + 必要的样式微调，无新 API。

验收（里程碑）：`vgui_runtime_hud` 不碰鼠标键盘、仅手柄可完成模式切换/技能/导航/激活。

## 6. R3 运行时性能（R3-2/R3-1 已实现；R3-3 数字待补）

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

### 6.3 R3-3 帧预算文档（P1，S）

- roadmap 内附表：控件规模（1x/4x）× 模式（A/B）× 缓存（开/关）的 update/draw/submit avg/max（60/120 FPS 预算对照）。数据来自 R3-2。

## 7. R4 HUD 控件包（R4-1 已实现；R4-2/R4-3 待实施）

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

## 8. R5 世界空间 UI（部分完成）

- [x] **R5-0 屏幕空间名牌（世界锚点投影）**：宿主每帧用与 3D 相机一致的投影矩阵把世界锚点投到屏幕坐标，`vgui_widget_set_pos` 移动对应控件组，背面/出屏用 `vgui_widget_set_visible` 隐藏。配套新增 `vgui_widget_set_accent / set_track`（每控件条形图配色，0=继承主题）。参考实现：`sample/vgui_runtime_hud.cpp`（12 个 Unit 名牌 + 血条/蓝条跟随方块公转，投影参数与 phase0 相机一致：RH_ZO + Y 翻转）。
- [x] **R5-1 UI→3D 面板**：离屏 UI 纹理贴到世界空间公告牌。phase0 新增 `phase0_renderer_set_billboard(renderer, texture, pos, w, h)`——复用场景片元着色器（p0_vertex 本就带 uv + scene_tex 采样）与 grid 混合管线（alpha 混合、深度测试开/写入关），CPU 侧按相机基（view 矩阵行向量）重建四边形顶点，每帧上传。宿主侧：独立小 UI 上下文渲进专用 FBO（`cycle_target=true`，phase0 采样上一帧内容，一帧延迟无碍）；与主 HUD/模式 B 合并进**一次 `vgpu_render_frame` 多 item 提交**（面板 item 在前）。注意：离屏 rvg 的 `width/height` 必须手动设置（与 FBO 尺寸一致），否则正交映射错位、文字落到视口外。参考实现：`sample/vgui_runtime_hud.cpp`（"ARENA STATUS" 聚合血条/蓝条公告牌）。
- [x] **R5-2 世界拾取与选中**：核心新增 `vgui_widget_set_input_enabled(w, 0)`——控件及子树整体跳过命中测试（保留正常视觉，区别于 set_enabled(0) 的禁用态），世界悬浮指示器靠它放行点击。宿主侧：根面板 `vgui_widget_on_event` 收未命中的 CLICK，反投影成射线（inverse VP，near/far 两点）对实体包围球求交（glm::intersectRaySphere），最近者 `select_unit` 联动名牌高亮（border + 名字色）；右键清除选中。自测内嵌 smoke（注入点击→断言选中→右键→断言取消）。参考实现：`sample/vgui_runtime_hud.cpp`。

## 9. 关键依赖与批次

- R0/R1/R5 已完成，无外部依赖。
- R2 批次：R2-1（core 导航，P0）→ R2-2（手柄映射，依赖 R2-1）→ R2-3（确认，S）。R2-2 的映射表可先行单测。
- R3 批次：R3-2（基准，**先行建立基线**）→ R3-1（子树缓存，L，依赖基线与 ovg 录制/回放 API——已存在未接入）→ R3-3（文档，依赖 R3-2）。
- R4 批次：R4-1（绑定惯例核查，S，先行）→ R4-2（冷却环/飘字/安全区）→ R4-3（多点触摸，P2 按需）。
- 跨项：R4-2 冷却环与 R5-0 名牌共用 set_accent/set_track；R3-1 缓存对 R4 飘字/动态控件默认不生效（opt-in 语义）。

## 10. 维护方式

与 [editor_roadmap.md 第 10 节](editor_roadmap.md)一致：每完成一项把复选框改为 `[x]` 并补充实现位置与测试名；新条目沿用 R 前缀，标注 P0/P1/P2 与 S/M/L；每个里程碑在 `vgui_runtime_hud` 示例中补一条可操作路径，确保能力不是"只在测试里存在"。
