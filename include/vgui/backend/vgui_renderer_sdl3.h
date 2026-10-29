#pragma once
/*
 * vgui_renderer_sdl3.h
 * vgui 的 SDL3 GPU 2D 矢量渲染后端。
 *
 * 与 third_party/ovg/ovg/ovg_renderer_sdl3.h 中的参考实现相比，这里只保留
 * vgui 真正使用的 2D 能力：
 *   - OVG 生成的填充/描边三角形
 *   - 纯色、线性/径向/扫描渐变
 *   - 文本图集与普通图片的纹理三角形
 *   - 矩形裁剪、MSAA、多 FBO/窗口交换链
 *
 * 不包含 3D 几何、路径模板裁剪、实例化、compute 特效。这样后端不依赖
 * depth/stencil 附件，也让 vgui 这一层的资源生命周期更明确。
 *
 * 该头文件仍然使用 OVG 的 ovg_draw_data_t，因此可以和 ovg 的绘制列表
 * 直接对接；vg_fbo_t 由本头文件定义，ovg_c.h 只做前置声明。
 */

#include <cstddef>
#include <cstdint>

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#include <glm/glm.hpp>

#include <ovg_c.h>

struct vgpu_device_t;
struct vgpu_ctx_t;
struct vgpu_texture_t;

// 一帧的渲染目标。窗口 FBO 使用 swapchain，离屏 FBO 使用 color_tex。
struct vg_fbo_t {
	uint32_t width = 0;
	uint32_t height = 0;
	glm::ivec2 display_size = {};
	vgpu_ctx_t* ctx = nullptr;
	SDL_Window* window = nullptr;
	SDL_GPUTexture* swapchain = nullptr;
	vgpu_texture_t* color_tex = nullptr;
	vgpu_texture_t* color_msaa_tex = nullptr;
	SDL_GPUCommandBuffer* cmd = nullptr;
	bool upsize = true;
};

// 设备：持有 shader 和默认白色纹理，生命周期跟随 SDL_GPUDevice。
vgpu_device_t* vgpu_create_device(SDL_GPUDevice* gpu_device);
void vgpu_destroy_device(vgpu_device_t* device);

// 上下文：持有管线、纹理表、GPU 缓冲和当前命令缓冲。
// depth_format 参数被保留用于兼容调用点，但后端不创建 depth/stencil 附件。
vgpu_ctx_t* vgpu_create_context(vgpu_device_t* device,
	SDL_GPUTextureFormat color_format,
	SDL_GPUTextureFormat depth_format,
	SDL_GPUSampleCount samples);
void vgpu_destroy_context(vgpu_ctx_t* ctx);

// FBO 与交换链。
vg_fbo_t vgpu_create_fbo(vgpu_ctx_t* ctx, int width, int height, SDL_Window* window = nullptr);
void vgpu_destroy_fbo(vg_fbo_t* fbo);
void vgpu_reset_fbo(vg_fbo_t* fbo, int width, int height);
int vgpu_acquire_swapchain(vgpu_ctx_t* ctx, vg_fbo_t* fbo, int count, bool wait);

// 获取 FBO 颜色纹理：窗口 FBO 返回 swapchain，离屏 FBO 返回 color_tex。
// 主要用于离屏测试的 SDL_DownloadFromGPUTexture 读回。
SDL_GPUTexture* vgpu_fbo_color_texture(vg_fbo_t* fbo);

// 原生纹理句柄：采样调用方已有的 SDL_GPUTexture。vgui 只借用纹理，从不
// 释放它；默认采样器为 linear + clamp。纹理必须带
// SDL_GPU_TEXTUREUSAGE_SAMPLER。
bool vgpu_texture_init_native(vgpu_ctx_t* ctx, ovg_texture_t* tex,
	SDL_GPUTexture* texture, int width, int height, SDL_GPUTextureFormat format);
// 原地替换纹理指针/尺寸，保持 image.id 不变；旧纹理由调用方在
// vgpu_wait_idle 之后释放。
bool vgpu_texture_update_native(vgpu_ctx_t* ctx, ovg_texture_t* tex,
	SDL_GPUTexture* texture, int width, int height, SDL_GPUTextureFormat format);
// 从后端移除并延迟释放采样器；不会释放借用的 SDL_GPUTexture。
void vgpu_texture_fini_native(vgpu_ctx_t* ctx, ovg_texture_t* tex);

// 提交一帧。data 中的 fbo 必须已经通过 vgpu_acquire_swapchain 或由本函数
// 自动获取交换链；函数返回后命令缓冲已经提交。
void vgpu_render_frame(vgpu_ctx_t* ctx, ovg_draw_data_t* data, size_t count);
void vgpu_wait_idle(vgpu_ctx_t* ctx);
