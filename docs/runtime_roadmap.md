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

## 5. R2 手柄/键盘方向导航（待规划）

- [ ] **R2-1 core 方向导航**：`vgui_focus_navigate(ctx, dir)` 按几何最近邻在可聚焦控件间移动（上/下/左/右 + 环绕），覆盖 button/checkbox/radio/switch/slider/tabs/list 行；model_test 断言导航次序。
- [ ] **R2-2 SDL_Gamepad 映射**：`app_mgr` 启用 gamepad 事件；左摇杆/十字键→方向导航（带重复率），A→激活焦点控件，B→`vgui_input_cancel`，Y→context 菜单，Start→menubar。
- [ ] **R2-3 焦点环**：手柄模式下主题化的焦点高亮（当前 focus_alpha 之上的可视状态），demo 提供开关。

验收：demo 全程可只用手柄操作；`vgui_runtime_hud` 热条支持手柄导航。

## 6. R3 运行时性能（待规划）

- [ ] **R3-1 静态子树显示列表缓存**：接入 ovg `start_recording/stop_recording/replay`（`ovg_c.h`），对无活动动画、无脏标记的子树回放缓存的录制；失效条件：布局/样式/文本/内容变化。
- [ ] **R3-2 运行时基准**：`vgui_runtime_bench`（OVG_MAX_FRAMES 风格环境变量），HUD 场景分 record/update/render 三段计时，输出 avg/max；作为 R3 前后对比与回归基线。
- [ ] **R3-3 帧预算文档**：60 FPS 与 120 FPS 预算下的实测数字（控件规模 × 模式）。

## 7. R4 HUD 控件包（待规划）

- [ ] **R4-1 运行时数据绑定惯例**：每帧 `vgui_widget_set_value/set_text` 的轻量绑定模式文档 + 脏检查（值不变则跳过，避免无谓重排）。
- [ ] **R4-2 HUD 型控件**：冷却环（径向进度）、飘字（有限生命周期标签池）、安全区 inset 支持。
- [ ] **R4-3 多点触摸**：当前仅单指映射，多指手势（双指缩放/拖动）按需补。

## 8. R5 世界空间 UI（待规划）

- [ ] **R5-1 UI→3D 面板**：离屏 HUD 纹理贴到世界空间四边形（R1-3 的纹理路径延伸），含 3D 采样参数。
- [ ] **R5-2 世界拾取**：引擎射线命中面板后把命中点换算为 UI 坐标并注入 `vgui_input_mouse_*`。

## 9. 关键依赖与批次

- R0/R1 已完成，无外部依赖。
- R2 依赖 R0 的输入注入面（`vgui_input_*`）与 core 焦点系统。
- R3 依赖 ovg 录制/回放 API（已存在，未接入）。
- R4/R5 依赖 R1 的合成模式与纹理路径。

## 10. 维护方式

与 [editor_roadmap.md 第 10 节](editor_roadmap.md)一致：每完成一项把复选框改为 `[x]` 并补充实现位置与测试名；新条目沿用 R 前缀，标注 P0/P1/P2 与 S/M/L；每个里程碑在 `vgui_runtime_hud` 示例中补一条可操作路径，确保能力不是"只在测试里存在"。
