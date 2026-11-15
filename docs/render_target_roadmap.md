# vgui 持久离屏渲染目标路线图

> 状态：P0 已完成（2026-11-08）
>
> 适用版本：vgui 0.3.x（仅源码兼容）
>
> 相关文档：[游戏运行时 UI 路线图](runtime_roadmap.md)、[Phase 0：3D 场景编辑器基础](phase0_3d_editor.md)

## 1. 目标与边界

### 1.1 目标

把"把 vgui 画面渲染到一张持久离屏纹理，再当普通纹理使用"从示例代码提升为**正式的公共后端 API**。目标场景：

- 编辑器/引擎把独立的面板、HUD、缩略图或预览界面渲染到离屏纹理；
- 该纹理直接绑定到 `VGUI_IMAGE`，或交给引擎的合成 pass 采样；
- 宿主每帧只负责录制和提交，不自己维护 FBO、`rvg_t` 和原生纹理桥；
- 与现有 `vgpu_render_frame()` 的一帧多 pass 提交模型兼容，不引入第二次提交。

P0 交付范围：

- 持久目标：一个 `vg_fbo_t` + 一个稳定的 `vgui_texture` + 一个私有 `rvg_t`；
- 原始矢量回调录制（`vgui_render_target_record`）；
- `vgui_context` 录制（`vgui_render_target_record_context`）；
- 批量 draw data 输出，由宿主并入自己的 `vgpu_render_frame()`；
- 原地 resize，`vgui_texture*` 指针保持不变；
- 纹理/原生纹理访问器与错误查询；
- 静态/独立场景的 `submit()` 便捷入口；
- 核心 `include/vgui/vgui.h` 不引入 SDL/C++/GPU 类型，`VGUI_CORE_ONLY` 不编译该接口。

### 1.2 非目标

- 不是通用渲染图/合成器，不管理引擎的 pass 依赖关系——多 target 依赖排序由宿主以数组顺序表达并合并为一次提交（§4.1、§4.7），不引入图 API。
- 多颜色附件仍不提供。深度/模板附件已由 P1 补齐（2026-11-14，`desc.depth` + `native_depth_texture()`，见 §4.4）——仅作为宿主 3D pass 的附件，vgui 的 UI pass 自身不使用深度。显式颜色格式已由 P1 补齐（2026-11-14，`desc.format`，见 §4.5）——HDR/sRGB 目标可直接烘入矢量/UI 内容，pipeline 按格式惰性创建。MSAA 采样数选择已由 P1 补齐（2026-11-14，`desc.sample_count`，见 §4.6）——按目标禁用/选择 1/2/4/8x。
- 不提供 GPU 回读、PNG 编码或呈现 API；回读用 `SDL_DownloadFromGPUTexture`，呈现用引擎自己的 pass。
- 不自动推进动画或布局：`record_context()` 不调用 `vgui_update()`。
- 异步 resize 已由 P2 覆盖（2026-11-15，fence + 延迟 FBO 释放，见 §4.8）——resize 不再等待 GPU idle；destroy/rebuild 仍同步（罕见路径）。设备丢失/上下文重建已由 P2 覆盖（2026-11-15，`rebuild/rebuild_all`）——仅针对 `vgpu_ctx_t` 重建（MSAA 切换）；SDL 设备级丢失仍由宿主整体重建。

## 2. 公共 API

头文件：`include/vgui/backend/vgui_render_target.h`（由 `vgui_sdl3.h` 一并包含）。

```c
typedef struct vgui_render_target vgui_render_target;

typedef struct vgui_render_target_desc {
    uint32_t struct_size;
    int width;      /* 物理像素，> 0 */
    int height;
    int clear;      /* 默认 1；0 = 保留已有内容 */
    int depth;      /* 非 0 = 离屏 FBO 附带 D32_FLOAT 深度附件（宿主 3D pass 用） */
    int format;     /* vgui_texture_format；0 = 跟随 vgpu_color_format()，非 0 = 显式颜色格式（HDR/sRGB） */
    int sample_count; /* MSAA；0 = 跟随上下文，1 = 禁用，2/4/8 = 显式 */
} vgui_render_target_desc;

void vgui_render_target_desc_default(vgui_render_target_desc* desc);

vgui_render_target* vgui_render_target_create(
    vgui_context* owner, vgpu_ctx_t* gpu_ctx, ovg_ctx_cb* cb,
    const vgui_render_target_desc* desc);
void vgui_render_target_destroy(vgui_render_target* target);

int vgui_render_target_resize(vgui_render_target* target, int width, int height);
void vgui_render_target_set_clear(vgui_render_target* target, int clear);
void vgui_render_target_get_size(const vgui_render_target* target,
    int* width, int* height);

vgui_texture* vgui_render_target_texture(vgui_render_target* target);
SDL_GPUTexture* vgui_render_target_native_texture(vgui_render_target* target);
SDL_GPUTexture* vgui_render_target_native_depth_texture(vgui_render_target* target);
int vgui_render_target_bytes_per_pixel(vgui_render_target* target);
size_t vgui_render_target_download(vgui_render_target* target, void* out,
    size_t cap);
int vgui_render_target_rebuild(vgui_render_target* target,
    vgpu_ctx_t* new_ctx);
int vgui_render_target_rebuild_all(vgpu_ctx_t* old_ctx, vgpu_ctx_t* new_ctx);
/* resize 不等 GPU idle:旧 FBO 由最新帧 fence 退休,稍后自动释放 */
const char* vgui_render_target_last_error(const vgui_render_target* target);

int vgui_render_target_record(vgui_render_target* target,
    vgui_render_target_draw_fn draw, void* user, ovg_draw_data_t* out_data);
int vgui_render_target_record_context(vgui_render_target* target,
    vgui_context* ui, ovg_draw_data_t* out_data);
int vgui_render_target_submit(vgui_render_target* target,
    vgui_render_target_draw_fn draw, void* user);
int vgui_render_target_submit_context(vgui_render_target* target,
    vgui_context* ui);
```

约定：

- 所有可扩展结构保持 `uint32_t struct_size` + `*_default()` 模式；调用方先 `_default()` 再改字段。
- `vgui_render_target` 是不透明句柄；核心头文件不出现该类型。
- 创建失败返回 `NULL`，`vgui_render_target_last_error(NULL)` 给出原因；每个目标另有自己的错误字符串。
- 纹理格式默认由 `vgpu_color_format(gpu_ctx)` 反推，`desc.format` 可显式覆盖（创建时校验设备支持）。

## 3. 生命周期与所有权

```text
vgui_context owner ──注册/持有──> vgui_texture（target 的内部纹理）
vgpu_ctx_t gpu_ctx ──提交/访问──> vg_fbo_t
ovg_ctx_cb* cb     ──借用────────> target 的 rvg_t
```

- **`owner` 必须晚于 target 销毁**。target 的 `vgui_texture` 注册在 owner 上下文里；先 `vgui_destroy(owner)` 会让 target 持有悬垂纹理指针。
- **`gpu_ctx` 必须晚于 target 销毁**。所有 FBO、纹理和提交都访问它。
- **`cb` 是借用指针，不由 target 释放**。`vgui_draw()` 会把 `cb` 存进 `vgui_context::draw_cb`，用于以后销毁缓存录制；因此 `cb` 必须比所有通过该 target 录制的上下文活得更久。窗口宿主可直接传已有的 `app_mgr` 回调；纯离屏宿主用 `new_ctx_cb()` 创建，并在所有上下文/target 销毁后再 `free_ctx_cb()`。
- `vgui_render_target_destroy()` 先 `vgpu_wait_idle()`，再立即解绑原生纹理（`release_native`），最后释放纹理、录制画布和 FBO。仍绑定该纹理的 `VGUI_IMAGE` 之后会画空，不会采样已释放的 `SDL_GPUTexture`。
- `vgui_render_target_resize()` 先创建新的 FBO，成功后才等待 idle 并原地更新纹理句柄；失败时旧目标保持可用。纹理指针与 image id 不变。

推荐的销毁顺序：

```c
vgui_image_set_texture(image, NULL);   /* 或销毁 image */
vgui_render_target_destroy(target);
vgui_destroy(ui);
vgpu_destroy_context(gpu_ctx);
free_ctx_cb(cb);
```

## 4. 录制、提交与帧顺序

### 4.1 `record()` 是主路径

```c
ovg_draw_data_t content = {};
vgui_render_target_record_context(content_target, panel_ui, &content);

/* 同帧、同一次提交：先写离屏目标，再让主 UI/合成 pass 采样它。 */
ovg_draw_data_t composite = {};
vgui_render_target_record_context(composite_target, host_ui, &composite);

ovg_draw_data_t passes[] = { content, composite };
vgpu_render_frame(gpu_ctx, passes, 2);
```

- `record()` 会清空 target 的 `rvg_t`、重置裁剪，再调用绘制回调；返回的 `ovg_draw_data_t::fbo` 已指向 target 内部 FBO。
- 返回数据在 target 下一次 `record()` / `resize()` / `destroy()` 前有效，必须在本帧提交。
- **一帧内所有 pass 必须放进同一次 `vgpu_render_frame()`**。SDL3 的自动屏障只在同一次提交内保证"离屏写入 → 采样"的顺序；拆成多次提交会在共享顶点缓冲和渲染目标上产生竞态。
- `record_context()` 要求 `vgui_context` 尺寸与 target 完全一致，否则返回 0 并给出 `"context size mismatch..."`。它不调用 `vgui_update()`，动画/时间推进仍由宿主负责。
- `record_context()` 也不要求 target 的 owner 就是传入的 context；可以在一个上下文里显示另一个上下文渲染出的纹理。

### 4.2 `submit()` 只是静态/独立场景的便利入口

`vgui_render_target_submit()` / `submit_context()` 等价于 `record()` + `vgpu_render_frame(..., 1)`。适用于：

- 一次性烘焙缩略图/静态预览；
- 独立小工具，没有需要合并的引擎 pass。

如果提交后还要在后续提交中采样结果，先调用 `vgpu_wait_idle()`；否则请改用 `record()` 并和其它 pass 合并。

### 4.3 `clear` 与 `cycle_target`

- `clear = 1`（默认）：每次 pass 清成透明黑，并设置 `cycle_target = true`。目标每帧重写并同帧采样时，SDL3 可在目标仍被上一帧占用时自动轮换后备存储。
- `clear = 0`：以 `LOAD` 打开，保留纹理已有内容，用于叠加/累积绘制；同时关闭 `cycle_target`。这种目标不要在同一帧里既写又采样，除非前一次提交已经 `vgpu_wait_idle()`。
- 运行时可调用 `vgui_render_target_set_clear()` 切换，下一 pass 生效。

### 4.4 深度附件（P1，2026-11-14）

`desc.depth != 0` 时离屏 FBO 额外创建一个 `D32_FLOAT` 深度附件（`vg_fbo_t::depth_tex/depth_format`；采样数跟随目标的有效 MSAA 采样数，见 §4.6，创建前用 `SDL_GPUTextureSupportsSampleCount` 校验，不支持则创建失败并报错）。宿主经 `vgui_render_target_native_depth_texture()` 取句柄，在自己的 3D render pass 里用 `SDL_GPUDepthStencilTargetInfo` 绑定。

- **vgui 的 UI pass 不绑深度**：`begin_pass` 保持 `depth_stencil_target_info = nullptr`，所有 vgpu pipeline 保持 `has_depth_stencil_target = false`——深度在场与否不影响 UI 输出（测试逐像素验证）。深度的 clear/store 策略完全由宿主 pass 决定，参考 `sample/phase0_renderer.cpp` `draw_scene_target()`：`clear_depth = 1.0f`、`LOADOP_CLEAR`、不需要采样时 `STOREOP_DONT_CARE`（要采样/预览则 `STORE`）、`cycle = true`。
- **3D 场景叠加 UI 的典型组合是 `clear = 0`**（R1 模式 A 语义）：宿主 3D pass 先渲进目标的颜色 + 深度，UI pass 以 LOAD 打开保留场景并叠加矢量内容。`clear = 1` 时 UI pass 清掉颜色，宿主先渲的 3D 会被抹掉。
- 3D pass 与 UI pass 必须在同一命令缓冲/同一次 `vgpu_render_frame()` 提交内（§4.1 帧顺序规则不变）。
- MSAA 时 `depth_tex` 即多采样深度——SDL_GPU 无深度 resolve，读回/预览深度需像 phase0 那样重渲一份单采样深度（depth-only pass）。
- 深度附件不可采样（仅 `DEPTH_STENCIL_TARGET` usage）；深度可视化是宿主自己的职责（phase0 的 preview 管线）。

### 4.5 显式颜色格式与 HDR 目标（P1，2026-11-14）

`desc.format` 取 `vgui_texture_format`（0 = 跟随上下文）时，离屏颜色附件按请求格式创建（RGBA16F/RGBA32F/R16F/R32F/RGBA8/BGRA8 及 sRGB 变体），创建前经 `vgpu_supports_color_format()` 校验（`SDL_GPUTextureSupportsFormat` 2D + COLOR_TARGET|SAMPLER + MSAA 采样数），不支持时报错。典型用途：把 UI/矢量内容直接烘进引擎自己的 HDR 合成管线目标（RGBA16F），无需先落 8-bit。

- **pipeline 按格式惰性创建**：`vg_pipelines[4]` 快路径仍是上下文自身格式；异格式 FBO 首次绘制时按（格式，采样数）建一套 4 条 pipeline 缓存在 `vgpu_ctx_t::alt_vg_pipelines`，geom 管线缓存的 key 同样纳入格式与采样数。采样器/着色器/混合/裁剪全部格式无关，零改动。
- **格式与 FBO 绑定**：`vg_fbo_t::color_format` 记录附件格式，`vgpu_fbo_color_format()` 解析（INVALID→上下文格式）；绘制时按 FBO 格式选 pipeline——这是 pipeline/attachment 兼容性要求的直接实现。
- **sRGB 目标**：固定功能混合在线性空间完成后做 sRGB 写出转换，与交换链 sRGB 行为一致；vgui 的 0xAARRGGBB 颜色按原样写入。
- **回读**：HDR 格式为 8 字节/像素（RGBA16F 用 half 解码），测试基建 `download_texture_bytes()` 支持任意字节宽。
- 与 `desc.depth` 正交可组合；与 `clear=0`（保留宿主 3D 内容）同样可组合。

### 4.8 异步 resize 与退休队列（P2，2026-11-15）

`resize()` 不再 `vgpu_wait_idle()`：旧 FBO 推入模块级退休队列，由**最新帧提交的 fence** 守护，`SDL_QueryGPUFence` signalled 后才销毁。机制镜像 ovg 渲染器的 `tex_free_fence` 模式。

- **vgpu 层**：`vgpu_ctx_t::last_frame_fence` 保存最近一次真实提交的 fence（静态助手 `submit_with_fence()` 接入 blit/render_frame flush/主提交三个提交点）；同队列提交按序完成——**单条最新 fence 的 signal 即蕴含此前全部提交完成**。`vgpu_take_frame_fence()` 转移所有权给退休队列；`vgpu_destroy_context` 释放未认领 fence。
- **drain 点**：`record_internal` 开头（每帧一次，通常即刻清空）、`resize` 开头、`download` 结尾（已 idle → 全清）。fence 不可得（无历史提交，或前次 resize 已取走）时退回 `vgpu_wait_idle` + 立即销毁——正确性优先于无阻塞。
- **只让 resize 走异步**：destroy/rebuild 保持同步（罕见路径；rebuild 的调用方 set_msaa_samples 本就先 idle）——缩小 fence 生命周期的风险面，避免应用退出时退休条目泄漏。
- 生命周期安全：退休条目只持 FBO（其纹理自带 device 指针）与 device 级 fence；native 桥的释放本就经 `ctx->pending_destroy` 延迟一帧（与 `retired_ctxs_` 契约一致）。

### 4.6 MSAA 采样数选择（P1，2026-11-14）

`desc.sample_count`：0 = 跟随 `vgpu_ctx_t` 采样数（默认），1 = 显式禁用 MSAA（pass 直接渲进 color_tex、无 resolve——回读/比对目标的推荐配置），2/4/8 = 显式采样数。非法值（如 3）在 create 时报 `"invalid sample count"`；(格式，采样数) 组合经 `vgpu_supports_color_format()` 校验，不支持报 `"sample count not supported for color target"`。

- 有效采样数存于 `vg_fbo_t::samples`（`vgpu_fbo_samples()` 解析）；`begin_pass` 零改动——MSAA 附件的存在性已隐含采样数，resolve 到 color_tex 的路径不变。
- **注意 SDL_GPUSampleCount 是序数枚举**（1→0、2→1、4→2、8→3）：原始计数必须经映射转枚举，直接 cast 会得到越界值并被 SDL 静默降级为 1x（实现中的 `to_sdl_sample_count()`）。
- pipeline 按既有（格式，采样数）键惰性选择（§4.5 的缓存机制）；深度附件采样数自动跟随目标有效值。
- 典型用途：回读/比对目标禁 MSAA；引擎按面板内容复杂度为各离屏目标选 4x/8x。

### 4.7 多目标链路（P1，2026-11-14——以既有能力关闭，不引入图 API）

多 target 渲染"图"由既有批量提交模式覆盖，无需新 API：每个 target 各自 `record()`/`record_context()` 产出一项 `ovg_draw_data_t`，宿主把依赖顺序排进数组，合并为**一次** `vgpu_render_frame()` 提交（§4.1）；SDL3 的自动屏障在同一次提交内保证"离屏写入 → 采样"顺序，与深度/格式/采样数选项全部可组合。`vgui_render_target_test` 的三级链路用例验证：target A（红）→ target B 的 UI 采样 A → target C 的 UI 采样 B，三项 pass 单次提交，末端回读同时命中"穿透两级的 A 颜色"与"穿过一级的 B 背景"。路线图原条目（`record()` 输出多 target 或显式依赖排序）判定为被该模式覆盖：数组顺序即依赖顺序，`ovg_draw_data_t::fbo` 已逐项携带目标；独立的依赖排序/图结构没有表达需求，不做。

## 5. 实现要点

| 主题 | 实现 |
|---|---|
| FBO 创建 | `vgpu_create_fbo(gpu_ctx, w, h, nullptr, depth, color_format, sample_count)`；`nullptr` 窗口表示纯离屏，颜色纹理带 `SAMPLER \| COLOR_TARGET`；`depth != 0` 时按需创建 `D32_FLOAT` 深度附件（`DEPTH_STENCIL_TARGET` usage，采样数跟随目标有效值）；`color_format` 显式覆盖附件格式（INVALID 跟随上下文）；`sample_count` 显式覆盖 MSAA（0 跟随/1 禁用/2/4/8，原始计数经 `to_sdl_sample_count()` 映射为序数枚举） |
| 格式 | 默认 `vgpu_color_format()` 反推 `vgui_texture_format`；`desc.format` 显式覆盖并经 `vgpu_supports_color_format()` 前置校验（深度固定 D32_FLOAT） |
| 异格式 pipeline | `vgpu_ctx_t::alt_vg_pipelines` 按（格式，采样数）惰性缓存 4 条 vg pipeline；geom 缓存 key 含格式与采样数；绘制按 `vg_fbo_t::color_format` 选集 |
| 深度生命周期 | create/resize/destroy/reset 全部经 `vg_fbo_t` 深度字段处理；`depth_format != INVALID ⟺ depth_tex 在场`；`vgpu_fbo_depth_texture()` / `vgui_render_target_native_depth_texture()` 暴露句柄 |
| 纹理桥 | `vgui_sdl3_texture_create_native()` 注册借用句柄，target 通过 `vgui_texture` 暴露稳定指针 |
| resize | 新建 FBO → 等待 idle → `vgui_sdl3_texture_update_native()` 原地更新 → 销毁旧 FBO |
| destroy | 等待 idle → `texture->release_native()` → `vgui_texture_destroy()` → 销毁 `rvg_t` → 销毁 FBO |
| 录制画布 | 每个 target 一个私有 `rvg_t`；`record()` 复用并清空，不跨目标共享 |
| 错误 | create 失败写入 thread-local 错误；其它失败写入目标自己的 `std::string` |

实现位置：

- 公共头：`include/vgui/backend/vgui_render_target.h`
- 实现：`backends/vgui_render_target.cpp`
- 构建：`backends/CMakeLists.txt` 的 `NOT VGUI_CORE_ONLY` 源列表

## 6. 测试与验收

`sample/vgui_render_target_test.cpp`，CTest 名称 `vgui_render_target`；无 Vulkan 设备时返回 77（SKIP）。

覆盖：

1. 非法创建（NULL desc）返回 `NULL`，错误可查询；
2. 原始矢量回调 → 离屏 FBO → 像素回读（纯红）；
3. `vgui_context` 录制 → target 纹理 → `VGUI_IMAGE` → 另一 target 合成 → 像素回读（纯绿），验证上下文尺寸校验和同帧多 pass；
4. `resize()` 后 `vgui_texture*` 不变、宽高更新，旧内容不再使用；
5. 上下文尺寸与 target 不一致时 `record_context()` 拒绝并给出明确错误；
6. `clear = 0` 时两次 `submit()` 的累积结果（红块保留、蓝块叠加）；
7. `submit()` 便捷入口可独立工作；
8. 深度附件（P1）：`desc.depth = 1` 创建后深度句柄在场、无 `depth` 的目标句柄为 NULL；同一份 UI 内容在"有深度/无深度"两个目标上颜色输出逐像素一致；resize 后深度句柄仍有效且可继续录制/读回。
9. 显式格式（P1）：`desc.format = RGBA16F` 目标创建成功，纯红 UI 烘入后以 fp16 读回解码断言（R≈1、G/B≈0、A≈1）；resize 后仍可录制/读回；非法格式值 create 失败并给出错误描述；`format = 0` 既有 8-bit 全部用例不回归。
10. MSAA 采样数选择（P1）：1x 上下文上 `desc.sample_count = 4` 目标创建成功，resolve 后输出与 1x 孪生目标逐像素一致；显式 `1` 创建成功；`3` 报 `"invalid sample count"`；`4 + depth` 组合成功且深度句柄在场（按设备 D32 4x 能力分支断言，不支持时必须干净失败）。
11. 多目标链路（P1）：三个 target 按依赖顺序各自 `record_context()`，三项 pass 合并一次 `vgpu_render_frame()` 提交；末端回读同时命中穿透两级的层 0 颜色（红）与穿过一级的层 1 背景（绿）。
12. 回读 API（P2）：`bytes_per_pixel()` 对 RGBA8/RGBA16F 分别报 4/8；公共 `download()` 与测试基建（transfer buffer + copy pass）内容逐字节一致、不越界写、短缓冲与 NULL 缓冲被拒并写入错误串。
13. 上下文重建（P2）：第二 `vgpu_ctx_t` 上 `rebuild_all` 精确重绑旧 ctx 的全部目标（计数==1）、`vgui_texture*` 指针不变、重录 + 新 ctx 提交 + 回读内容正确；同 ctx rebuild 为 no-op；换绑回旧 ctx 后目标继续可用。
14. 异步 resize（P2）：交替尺寸连续 10 轮 resize → 立即 record → submit → download（不等 GPU idle），中心像素每轮全红——fence 退休队列守护旧 FBO，新内容不受在飞工作影响；resize 头注释同步更新为"不等 idle"。

运行：

```powershell
out\bin\x64d\vgui_render_target_test.exe
# 或
ctest --test-dir out\build\x64-debug -R vgui_render_target --output-on-failure
```

构建验收：

- 全量构建（`bld_full.bat`）通过，全量 ctest 5/5（含 `vgui_render_target`）；
- `VGUI_CORE_ONLY=ON`（`bld_core.bat`）通过，core-only ctest 2/2，且构建图里不出现 `vgui_render_target.cpp` / `vgui_render_target_test`；
- 公共头自包含检查覆盖新头文件；
- 既有纹理、截图、模型与 C API 测试不回归（core-only 编辑器关闭路径一并修正，见 §8）。

## 7. 后续（P1/P2）

| 优先级 | 事项 | 说明 |
|---|---|---|
| ~~P1~~ | ~~MSAA 采样数选择~~ | **已完成（2026-11-14）**：`desc.sample_count`（0 跟随/1 禁用/2/4/8 显式），见 §4.6 |
| ~~P1~~ | ~~显式格式/色彩空间~~ | **已完成（2026-11-14）**：`desc.format`（HDR/sRGB）+ 按格式惰性 pipeline，见 §4.5 |
| ~~P1~~ | ~~深度/模板附件~~ | **已完成（2026-11-14）**：`desc.depth` + `native_depth_texture()`，复用 `vg_fbo_t` 深度字段，见 §4.4 |
| ~~P1~~ | ~~多目标渲染图~~ | **以既有能力关闭（2026-11-14）**：数组顺序即依赖顺序，N 个 target 各自 record 后合并一次 `vgpu_render_frame()` 提交，三级链路测试覆盖；不引入图 API，见 §4.7 |
| ~~P2~~ | ~~异步 resize/退休队列~~ | **已完成（2026-11-15）**：fence + 延迟 FBO 释放（镜像 ovg `tex_free_fence` 模式），resize 不再等 GPU idle，见 §4.8 |
| ~~P2~~ | ~~设备丢失/重建~~ | **已完成（2026-11-15）**：`rebuild/rebuild_all`（后端注册表，`set_msaa_samples` 自动触发），见 §4.8 |
| ~~P2~~ | ~~回读/编码辅助~~ | **已完成（2026-11-14）**：`vgui_render_target_download()`（原生格式 CPU 回读，含 GPU idle 等待）+ `bytes_per_pixel()`；PNG 编码留在宿主侧（chart 已有 PNG 路径） |

## 8. 变更记录

- **2026-11-15：P2 异步 resize 落地**。`resize()` 不再 `vgpu_wait_idle()`：旧 FBO 由最新帧 fence 守护、退休队列延迟销毁（镜像 ovg `tex_free_fence` 模式）。vgpu 层新增 `vgpu_ctx_t::last_frame_fence` + `submit_with_fence()`（blit/flush/主提交三个真实提交点接入）+ `vgpu_take_frame_fence()` 公共访问器；`vgpu_destroy_context` 释放未认领 fence。退休队列 drain 于每帧 record/resize/download；fence 不可得时回退 idle+立即销毁。destroy/rebuild 保持同步（缩小风险面）。测试：交替尺寸 10 轮 resize→record→submit→download 压力块（不等 idle，输出每轮正确）。**render_target 路线图 §7 P1/P2 至此全部清零。**
- **2026-11-15：P2 设备丢失/重建（上下文重建自动恢复）落地**。真实破坏场景：`app_mgr::set_msaa_samples` 重建共享 `vgpu_ctx_t` 时，离屏目标的 `gpu_ctx`/FBO/纹理桥全部悬空。修复：`vgui_render_target.cpp` 维护存活目标注册表（create 注册、destroy 注销），新 API `vgui_render_target_rebuild(target, new_ctx)`（镜像 resize 体：旧 ctx 存活期 `release_native` → idle → 新 FBO → `update_native` 重建分支 → 销毁旧 FBO；**失败也换 ctx**——旧 ctx 即将销毁，损坏态以错误串表达）与 `vgui_render_target_rebuild_all(old_ctx, new_ctx)`（遍历注册表）。`set_msaa_samples` 在窗口 FBO 重建后调用 `rebuild_all(retired, vgctx)`（GPU 已 idle、旧 ctx 退休未销毁，时序正好）；`vgui_sdl3.h` 契约注释同步。重建后 FBO 内容为空（同 resize 语义），需重录。测试：双 ctx 上 rebuild_all 精确重绑（计数==1）、纹理指针稳定、新 ctx 提交/回读内容正确、同 ctx no-op、换绑回旧 ctx 后可用。§7 仅剩异步 resize 一行 P2。
- **2026-11-14：P2 回读 API 落地**。新公共 API `vgui_render_target_bytes_per_pixel()`（RGBA8/BGRA8/sRGB/R32F=4、RGBA16F=8、RGBA32F=16、R16F=2）与 `vgui_render_target_download(target, out, cap)`——transfer buffer + copy pass + 提交 + `vgpu_wait_idle()` 的 CPU 回读，原生格式行紧凑排布；cap 不足/NULL 缓冲/无效目标拒绝并写目标错误串。支撑改动：`vgpu_renderer_sdl3.h` 新增 `vgpu_context_device()`（`vgui_render_target.cpp` 无法访问不透明 `vgpu_ctx_t` 的 device）。实现与 `gpu_test_util.h` 的 download 基建同源；测试互验（RGBA16F/RGBA8 内容逐字节一致、不越界写、短缓冲拒绝）。PNG 编码留宿主侧（chart 已有 PNG 路径），P2 该行关闭。
- **2026-11-14：P1 多目标渲染图以既有能力关闭**。核查结论：N 个 target 各自 `record()` 产出一项、宿主按依赖顺序排数组、合并一次 `vgpu_render_frame()` 提交——这正是 §4.1 已文档化的批量模式，`ovg_draw_data_t::fbo` 逐项携带目标，SDL3 同提交自动屏障保证 写→采样 顺序；与深度/格式/采样数选项可组合。判定"显式依赖排序/图结构"没有独立表达需求，**不引入新 API**（路线图原条目"一个 record() 输出多个 target"是同一能力的低效形态）。`vgui_render_target_test` 新增三级链路用例：target A（红背景）→ target B 的 UI 采样 A（半幅）→ target C 的 UI 采样 B（全幅），三项 pass 单次提交，末端回读同时命中穿透两级的 A 红（(8,6)）与穿过一级的 B 绿（(40,30)）。§7 P1 行全部清零，剩 P2 三行（异步 resize、设备丢失、回读辅助）。
- **2026-11-14：P1 MSAA 采样数选择落地**。`vg_fbo_t` 新增 `samples`（有效采样数）字段；`vgpu_create_fbo()` 加 `sample_count` 参数（0 跟随/1 禁用/2/4/8 显式，原始计数经新 `to_sdl_sample_count()` 映射为 SDL 序数枚举——直接 cast 会越界并被 SDL 静默降级 1x，本次实现曾踩此坑并加测试防回归），MSAA 颜色附件与深度附件均按有效值创建；`vgpu_reset_fbo()` 保留有效值、`vgpu_acquire_swapchain()` 为窗口 FBO 标注上下文采样数；新 API `vgpu_fbo_samples()`。`get_vg_pipeline()/get_geom_pipeline()` 的采样数维度改用 FBO 有效值，异采样数自动落入既有（格式，采样数）惰性缓存。`vgui_render_target_desc` 尾部追加 `sample_count`，create 解析 + `vgpu_supports_color_format()` 组合校验（非法值报 "invalid sample count"，不支持报 "sample count not supported for color target"），resize 携带重建。`vgui_render_target_test` 新增：4x 目标 resolve 输出与 1x 孪生逐像素一致、显式 1x、非法值拒绝、4x+深度组合（按设备 D32 4x 能力分支断言）。`begin_pass` 零改动。§7 仅剩多目标渲染图一行 P1。
- **2026-11-14：P1 显式颜色格式落地**。`vg_fbo_t` 新增 `color_format` 字段；`vgpu_create_fbo()` 加 `color_format` 参数（INVALID 跟随上下文，仅离屏生效），`vgpu_reset_fbo()` 携带重建，`vgpu_acquire_swapchain()` 为窗口 FBO 标注交换链格式；新 API `vgpu_fbo_color_format()` / `vgpu_supports_color_format()`（2D + COLOR_TARGET|SAMPLER + MSAA 采样数校验）。vgpu 侧新增按（格式，采样数）惰性缓存的 `alt_vg_pipelines` 集合与 `create_vg_pipeline_set()`，`get_vg_pipeline()` 改按 FBO 格式选集，geom 缓存 key 纳入格式与采样数——**采样器/着色器/混合/裁剪零改动**。`vgui_render_target_desc` 尾部追加 `format`，create 经 `to_sdl_texture_format()` 映射并前置校验（失败报 "unsupported target color format"/"color format not supported for color target"），resize 携带格式重建。`vgui_render_target_test` 用 `download_texture_bytes()`（新，任意字节宽读回）+ 本地 half 解码覆盖 RGBA16F 创建/烘入/读回/resize 与非法格式拒绝。
- **2026-11-14：P1 深度/模板附件落地**。`vg_fbo_t` 新增 `depth_tex/depth_format` 字段（路线图方向"复用 vg_fbo_t 深度字段"）；`vgpu_create_fbo()` 加默认参数 `depth`（离屏 + `SDL_GPUTextureSupportsSampleCount` 校验后创建 `D32_FLOAT`/`DEPTH_STENCIL_TARGET`，MSAA 跟随上下文），`vgpu_destroy_fbo()`/`vgpu_reset_fbo()` 同步处理，新访问器 `vgpu_fbo_depth_texture()`。`vgui_render_target_desc` 尾部追加 `depth`，创建/resize 校验深度在场（失败报 "failed to create depth attachment"），新 API `vgui_render_target_native_depth_texture()`。**UI pass 与所有 vgpu pipeline 零改动**（不绑深度），深度 clear/store 策略由宿主 pass 自理（phase0 `draw_scene_target` 配方，见 §4.4）。`vgui_render_target_test` 新增 17 项断言：句柄在场/缺失、同内容有无深度输出逐像素一致、resize 后句柄存活并可继续录制读回。
- **2026-11-08：P0 落地**。新增 `vgui_render_target` 公共后端 API、实现、CMake 接入、离屏回读测试与 CTest 注册。`vgui_sdl3.h` 转包含新头。实现中使用借用 `ovg_ctx_cb*`，与 `vgui_context::draw_cb` 的生存期要求一致。
- **2026-11-09：core-only 编辑器关闭修正**。同时暴露并修复了 core-only 下 `vgui_editor_c_api_test` 在 shell 销毁后崩溃的问题：编辑器组件先于控件树销毁，虚拟行源/回调仍持有 `this`。`asset_browser`、`console_view`、`outliner_view` 析构时解绑相关回调；core-only ctest 2/2 恢复通过。
