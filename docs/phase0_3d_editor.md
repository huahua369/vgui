# Phase 0：3D 场景编辑器基础

Phase 0 只建立后续场景编辑器需要的底层能力，不实现编辑器 UI。目标是让一个由应用创建、由独立 3D 管线渲染的离屏纹理，能够作为普通 `VGUI_IMAGE` 参与 vgui 的布局、裁剪、焦点和输入体系，同时补齐 CPU 纹理和自动换行布局。

## 范围

已实现：

- `vgui_texture`：CPU 像素纹理，RGBA/BGRA、LDR 8-bit、sRGB 与非 sRGB。
- `VGUI_IMAGE`：可聚焦图片控件，支持 source rect、fit、tint、flip、pixel snap。
- `VGUI_LAYOUT_WRAP`：基于 OVG flex 的自动换行，vgui 负责 gap、每行 justify 和多行 align-content 的最终坐标。
- SDL3 native texture bridge：把应用创建的 `SDL_GPUTexture` 借给 vgui 采样。
- `sample/vgui_phase0.cpp`：单窗口、单视口、固定布局的最小 3D FBO 示例。
- `sample/phase0_renderer.*`：最小 3D 渲染器，支持多个独立相机 view 共享管线、HDR/tonemap、MSAA resolve 与深度可视化纹理。
- `sample/vgui_c_api_test.c`：以 C 编译并链接的公开头文件/API 冒烟测试。

明确不包含：

- splitter、tabs、dock、property grid、number/vector/color 控件、combo。
- 2D canvas、gizmo、场景树编辑 UI。
- UI 编辑器中的图片/3D 节点 schema。
- 多采样 native texture（显示纹理始终是 RGBA8，MSAA 在 GPU 内 resolve）。
- 线程安全。纹理创建/更新和绘制应在同一个 GPU/主线程调用。

## 帧顺序

3D 离屏纹理和 vgui 共用一个 `SDL_GPUDevice`，但各自提交命令缓冲。顺序必须是：

1. `vgui_update()`
2. `vgui_layout()`
3. 从 `VGUI_IMAGE` 查询实际矩形
4. 必要时重建 3D FBO
5. 渲染 3D 并提交
6. 录制 UI，采样第 5 步完成的纹理
7. 提交 UI

`vgpu_render_frame()` 使用 `CLEAR` 开始窗口通道，因此 3D 内容不能依赖在同一窗口 pass 中先画再叠加；本阶段通过独立 FBO + 纹理采样组合，避免修改渲染器的 overlay/LOAD 行为。

### V09 多视口与渲染选项

一个 `phase0_renderer` 统一持有可共享的 GPU 资源，`phase0_view` 持有相机、实例/网格数据和独立的颜色/深度附件。每个 view 都可以单独设置：

```cpp
phase0_view_options options = phase0_view_get_options(view);
options.hdr = true;            /* RGBA16F -> tonemap -> RGBA8 */
options.msaa_samples = 4;      /* 1/2/4/8；设备不支持时会钳制 */
options.depth_preview = true;  /* 生成 RGBA8 深度可视化 */
phase0_view_set_options(view, &options);
```

`phase0_view_color_texture()` 返回可借给 vgui 的 RGBA8 纹理；HDR/MSAA 变化会重建附件，宿主应重新查询句柄并调用 `vgui_sdl3_texture_update_native()`。`phase0_view_depth_preview_texture()` 是深度可视化纹理，句柄在仅切换 `depth_preview` 时保持稳定。SDL_GPU 没有深度 resolve，因此 MSAA 模式额外用一个单采样、无颜色目标的深度专用通道填充预览深度；非 MSAA 模式直接采样场景深度。`sample/vgui_phase0.cpp` 仍使用旧单视口接口，`vgui_game_editor` 演示主视图、第二相机和深度预览三张纹理同帧渲染。

示例中的关键循环：

```cpp
vgui_update(ui, delta);
vgui_layout(ui);
vgui_widget_get_rect(viewport, &x, &y, &w, &h);
phase0_renderer_resize(renderer, (int)w, (int)h);
phase0_renderer_render(renderer, time);
// 若 FBO 重建，调用 vgui_sdl3_texture_update_native(...)

cb->clear(vg);
cb->reset_clip(vg, 1);
vgui_draw(ui, cb, vg);
drawable.set(fbo);
drawable.add(vg);
drawable.render(wg->vgctx);
```

## CPU 纹理

```c
vgui_texture_desc desc = {0};
desc.width = 256;
desc.height = 128;
desc.format = VGUI_TEXTURE_FORMAT_RGBA8;
desc.pixels = rgba_pixels;
desc.stride = 256 * 4;
vgui_texture* texture = vgui_texture_create(ui, &desc);
```

- 输入像素在 `vgui_texture_create/update` 内复制并整理为紧凑 stride，调用方可以立即释放原缓冲。
- `pixels == NULL` 可以创建空纹理，之后用 `vgui_texture_update` 填充。
- 首次绘制时惰性上传；同一帧多次绘制同一纹理只入队一次上传。上传由绘制提交阶段完成，因此新 CPU 纹理通常从下一帧开始可见；native 纹理当帧即可见。
- `vgui_texture` 引用计数由 vgui 内部管理。`VGUI_IMAGE` 持有纹理引用；`vgui_texture_destroy` 只释放调用方自己的引用。
- CPU 纹理的 GPU 句柄延迟到下一次绘制时销毁，因为 OVG 的 `image_destroy` 描述符需要在帧结束前保持有效。

文件解码不在 vgui 内完成；应用解码后以 RGBA/BGRA 传入。

## 图片控件

`vgui_widget_add(parent, VGUI_IMAGE, NULL)` 创建图片控件。未设置纹理时默认尺寸来自 `vgui_style::image`（240x160，边框 1）；设置纹理后自动采用纹理或 source rect 的尺寸，显式调用 `vgui_widget_set_size` 仍可覆盖。

```c
vgui_image_set_texture(image, texture);
vgui_image_set_source(image, 0, 0, 0, 0); /* 0 表示整张纹理 */
vgui_image_set_fit(image, VGUI_IMAGE_FIT_CONTAIN);
vgui_image_set_tint(image, 0xFF80C0FF);
vgui_image_set_flip(image, 1, 0);
vgui_image_set_pixel_snap(image, 1);
```

`sample/vgui_demo.cpp` 的“图片 VGUI_IMAGE”卡片用 CPU 纹理演示了上述接口，并可通过按钮切换 flip / tint / source / fit。

fit 语义：

- `STRETCH`：填满控件。
- `CONTAIN`：保持宽高比，完整显示，居中。
- `COVER`：保持宽高比，从 source 中心裁切后填满控件。
- `NONE`：1:1 像素尺寸，居中，超出控件部分裁剪。

翻转通过 `add_geometry` 使用翻转 UV 实现，因为 OVG `add_image` 不接受负的目标尺寸。图片控件本身参与焦点/禁用状态；禁用时按 `image.disabled_mix` 向主题禁用色混合。

## 自动换行布局

`vgui_widget_set_layout(widget, VGUI_LAYOUT_WRAP)` 开启换行。支持：

- `gap`：主轴与行间距。
- `justify`：每行的主轴分布。
- `align_items` / `align_self`：行内交叉轴对齐，`STRETCH` 在自动高度子项上生效。
- `align_content`：多行整体分布（start/center/end/space-between/space-around/space-evenly/stretch）。
- 子项 `grow`：在每行剩余空间中分配。

实现分两步：

1. 将单层子项尺寸、margin 和主方向 gap 送入 OVG 的 `new_flex_run()` / `flex_run_layout()`，由 OVG 决定换行位置和 grow 后的宽度。
2. vgui 根据 OVG 输出的行分组重新计算精确 x/y：实际 gap、每行 justify、行内 align-items、多行 align-content。

限制：当前只处理单层子项的 wrap；嵌套 wrap 面板按普通测量流程递归工作，但外层不会把内层行高作为特殊 flex 项处理。

## SDL3 native texture

应用创建的 GPU 纹理通过 backend API 绑定：

```cpp
vgui_texture* tex = vgui_sdl3_texture_create_native(
    ui, wg->vgctx, fbo_color_texture,
    width, height, VGUI_TEXTURE_FORMAT_RGBA8);
vgui_image_set_texture(image, tex);

/* FBO 重建后： */
vgui_sdl3_texture_update_native(tex, wg->vgctx,
    new_fbo_color_texture, new_width, new_height,
    VGUI_TEXTURE_FORMAT_RGBA8);
```

所有权与采样规则：

- vgui **只借用** `SDL_GPUTexture`，不会调用 `SDL_ReleaseGPUTexture`。
- 默认采样器为 **linear + clamp**；CPU 图片仍使用 nearest，保持原采样行为。
- 纹理必须带 `SDL_GPU_TEXTUREUSAGE_SAMPLER`；作为 3D 颜色附件时还要有 `SDL_GPU_TEXTUREUSAGE_COLOR_TARGET`。
- FBO 重建时先 `SDL_WaitForGPUIdle`，再释放旧颜色纹理并更新句柄。vgui 在下一帧绘制开始时才注销旧的 native wrapper；旧 wrapper 进入渲染器延迟销毁队列，只释放采样器。
- 纹理格式当前支持 RGBA8/BGRA8 及 sRGB 变体，映射到对应的 `SDL_GPUTextureFormat`。
- 若 `vgpu_ctx_t` 先于 `vgui_destroy` 销毁，native 注销会访问失效上下文；应用必须保证先销毁 UI，再销毁渲染上下文。

底层接口在 `backends/vgui_renderer_sdl3.h`，与 OVG 的 `ovg_texture_*_native` 同形：

```c
bool vgpu_texture_init_native(vgpu_ctx_t*, ovg_texture_t*,
    SDL_GPUTexture*, int width, int height, SDL_GPUTextureFormat);
bool vgpu_texture_update_native(vgpu_ctx_t*, ovg_texture_t*,
    SDL_GPUTexture*, int width, int height, SDL_GPUTextureFormat);
void vgpu_texture_fini_native(vgpu_ctx_t*, ovg_texture_t*);
```

`vgui_texture` 内部保存 `ovg_texture_t native` 值类型句柄，`VGUI_IMAGE` 绘制时走 `ovg_ctx_cb::add_texture`，因此源裁剪、tint、flip 和当前 clip 都由 OVG 统一处理。九宫格、旋转、任意矩阵目前只通过 OVG 的 `add_texture` 暴露；`VGUI_IMAGE` 只映射其已有的 source/tint/flip 属性。

## 兼容性

- 新增控件类型、布局类型、枚举和 `vgui_style` 字段都追加在现有枚举/结构末尾；已有枚举值和已有字段偏移不变。
- 没有修改任何现有函数签名或参数顺序。
- `vgui_style` 结构尺寸会因新增 `image` 字段而变化，使用方需要按新头文件重新编译；这是源码兼容，不承诺跨版本二进制兼容。
- `vgui_version()` 仍为 `0.3.0`，本阶段没有改动现有列表、树、菜单、选择或上下文菜单行为。

## 示例与测试

构建后运行：

```powershell
out\bin\x64d\vgui_phase0.exe
out\bin\x64d\vgui_phase0.exe --smoke
out\bin\x64d\ovg_texture_test.exe
out\bin\x64d\vgui_texture_test.exe
out\bin\x64d\vgui_model_test.exe
out\bin\x64d\vgui_c_api_test.exe
```

`ovg_texture_test` 用离屏 FBO + `SDL_DownloadFromGPUTexture` 读回，覆盖 `add_texture` 的方向、源裁剪、tint、水平/垂直 flip、九宫格、旋转、`mat3x2`、当前 clip、原地 update 和 fini。

`vgui_texture_test` 覆盖 `vgui_sdl3_texture_create_native` → `VGUI_IMAGE` → 源裁剪/tint/flip → `vgui_sdl3_texture_update_native` → 释放的完整路径。

`vgui_model_test` 覆盖：纹理创建/更新/stride/非法参数、图片控件、纹理引用生命周期、wrap 换行位置、justify、align-content 和 grow。

`vgui_c_api_test` 以 C 编译，验证公开头文件不依赖 C++，并检查新纹理/图片/wrap API 可被纯 C 调用。

`vgui_phase0 --smoke` 连续渲染 20 帧，要求 native texture 成功创建并绑定；退出码非 0 表示 GPU 管线、FBO 或桥接失败。

## 后续阶段接口预留

游戏编辑器方向的组件拆解、优先级和验收标准见 [`editor_roadmap.md`](editor_roadmap.md)。

- 多视口可以复用同一套 `VGUI_IMAGE` + native texture，只需为每个视口维护独立 FBO。
- 场景编辑器属性面板、dock、tab 等继续使用现有控件体系；本阶段没有引入新的 UI 容器。
- 纹理过滤/寻址枚举已加入公共头文件供后续扩展，当前 image 绘制固定使用后端采样器。
- 原生纹理已通过 OVG `add_texture` 接入；后续可把九宫格、旋转和任意矩阵暴露为 `VGUI_IMAGE` 的样式属性。
