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

- 不是通用渲染图/合成器，不管理引擎的 pass 依赖关系。
- P0 不提供深度/模板附件、多颜色附件、MSAA 采样数选择；目标格式跟随 `vgpu_ctx_t`，MSAA 也继承上下文设置。
- 不提供 GPU 回读、PNG 编码或呈现 API；回读用 `SDL_DownloadFromGPUTexture`，呈现用引擎自己的 pass。
- 不自动推进动画或布局：`record_context()` 不调用 `vgui_update()`。
- 不做设备丢失恢复和异步 resize；P0 的 resize/destroy 会等待 GPU idle。

## 2. 公共 API

头文件：`include/vgui/backend/vgui_render_target.h`（由 `vgui_sdl3.h` 一并包含）。

```c
typedef struct vgui_render_target vgui_render_target;

typedef struct vgui_render_target_desc {
    uint32_t struct_size;
    int width;      /* 物理像素，> 0 */
    int height;
    int clear;      /* 默认 1；0 = 保留已有内容 */
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
- 纹理格式由 `vgpu_color_format(gpu_ctx)` 反推，不需要用户选择。

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

## 5. 实现要点

| 主题 | 实现 |
|---|---|
| FBO 创建 | `vgpu_create_fbo(gpu_ctx, w, h, nullptr)`；`nullptr` 窗口表示纯离屏，颜色纹理带 `SAMPLER | COLOR_TARGET` |
| 格式 | `vgpu_color_format()` 反推 `vgui_texture_format`；不额外暴露格式字段 |
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
7. `submit()` 便捷入口可独立工作。

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
| P1 | MSAA 采样数选择 | 当前继承 `vgpu_ctx_t` 的采样数；可加 `desc.sample_count` 与 resolve 纹理策略 |
| P1 | 显式格式/色彩空间 | 当前跟随上下文；HDR 离屏目标可能需要 `desc.format` |
| P1 | 深度/模板附件 | 3D 场景离屏目标需要；应复用既有 `vg_fbo_t` 深度字段，而不是在 target 内另起一套 |
| P1 | 多目标渲染图 | 一个 `record()` 输出多个 target，或显式依赖排序；仍以一次 `vgpu_render_frame()` 提交 |
| P2 | 异步 resize/退休队列 | 去掉 `vgpu_wait_idle()`，用 fence + 延迟 FBO 释放 |
| P2 | 设备丢失/重建 | 上下文重建后自动重建目标的 FBO 与纹理桥 |
| P2 | 回读/编码辅助 | 可选的 `download()` / PNG 包装，避免每个宿主重复写 |

## 8. 变更记录

- **2026-11-08：P0 落地**。新增 `vgui_render_target` 公共后端 API、实现、CMake 接入、离屏回读测试与 CTest 注册。`vgui_sdl3.h` 转包含新头。实现中使用借用 `ovg_ctx_cb*`，与 `vgui_context::draw_cb` 的生存期要求一致。
- **2026-11-09：core-only 编辑器关闭修正**。同时暴露并修复了 core-only 下 `vgui_editor_c_api_test` 在 shell 销毁后崩溃的问题：编辑器组件先于控件树销毁，虚拟行源/回调仍持有 `this`。`asset_browser`、`console_view`、`outliner_view` 析构时解绑相关回调；core-only ctest 2/2 恢复通过。
