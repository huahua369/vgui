#pragma once
/*
 * vgui_renderer_sdl3.h
 * SDL3 GPU 2D vector renderer backend for vgui.
 *
 * Compared with the reference OVG renderer, this backend keeps only the 2D
 * features vgui uses: OVG fill/stroke triangles, solid and linear/radial/sweep
 * gradients, text-atlas and image triangles, rectangular clipping, MSAA and
 * multi-FBO/window swapchains.  It has no 3D geometry, path-template clipping,
 * instancing or compute effects, and the UI pass itself binds no depth/stencil
 * attachment; offscreen FBOs may still carry an optional depth attachment for
 * the host's own 3D passes.  Resource ownership stays explicit.  It consumes
 * OVG's ovg_draw_data_t directly; vg_fbo_t is defined here while ovg_c.h only
 * forward-declares it.
 *
 * See backends/vgui_renderer_sdl3.cpp for the implementation notes.
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

// Per-frame render target.  Window FBOs use the swapchain; offscreen FBOs use
// color_tex.  Offscreen FBOs can optionally carry a depth attachment
// (vgpu_create_fbo(..., depth=1)) that the host binds in its own 3D pass;
// the vgui UI pass never touches it.
struct vg_fbo_t {
	uint32_t width = 0;
	uint32_t height = 0;
	glm::ivec2 display_size = {};
	vgpu_ctx_t* ctx = nullptr;
	SDL_Window* window = nullptr;
	SDL_GPUTexture* swapchain = nullptr;
	vgpu_texture_t* color_tex = nullptr;
	vgpu_texture_t* color_msaa_tex = nullptr;
	// Optional depth attachment (D32_FLOAT).  With MSAA this already holds the
	// multi-sampled depth: SDL_GPU has no depth resolve, so sampling the depth
	// contents requires a single-sample re-render (see sample/phase0_renderer).
	// NULL when the FBO was created without depth or depth creation failed.
	vgpu_texture_t* depth_tex = nullptr;
	SDL_GPUTextureFormat depth_format = SDL_GPU_TEXTUREFORMAT_INVALID;
	// Color attachment format.  INVALID means "follow vgpu_ctx_t::color_format"
	// (the accessor resolves it); offscreen FBOs may override it per creation
	// for HDR/sRGB targets.  Pipelines are selected to match this format at
	// draw time.
	SDL_GPUTextureFormat color_format = SDL_GPU_TEXTUREFORMAT_INVALID;
	SDL_GPUCommandBuffer* cmd = nullptr;
	bool upsize = true;
	// Load-op of the UI render pass.  true (default) clears the target before
	// drawing; false keeps the existing content and blends the UI on top -- a
	// game engine renders its 3D scene into the same texture first, then runs
	// vgui/ovg recording into it as an overlay (docs/runtime_roadmap.md R1).
	// Must be set before vgpu_render_frame(); within a frame each FBO is
	// rendered by exactly one pass.
	bool clear = true;
	// Cycle the color target when it is still bound by a pending command
	// buffer (SDL_GPUColorTargetInfo::cycle).  Set this on offscreen FBOs that
	// are rewritten every frame and sampled later in the same frame; never on
	// window FBOs (SDL3 cycles the swapchain itself).
	bool cycle_target = false;
};

// Device: owns shaders and the default white texture; follows the
// SDL_GPUDevice lifetime.
vgpu_device_t* vgpu_create_device(SDL_GPUDevice* gpu_device);
void vgpu_destroy_device(vgpu_device_t* device);

// Context: owns pipelines, the texture table, GPU buffers and the current
// command buffer.  depth_format is kept for call-site compatibility; the UI
// pass itself never uses depth.  Offscreen FBOs may carry a depth attachment
// for the host's own 3D passes (vgpu_create_fbo(..., depth=1)).
vgpu_ctx_t* vgpu_create_context(vgpu_device_t* device,
	SDL_GPUTextureFormat color_format,
	SDL_GPUTextureFormat depth_format,
	SDL_GPUSampleCount samples);
void vgpu_destroy_context(vgpu_ctx_t* ctx);

// FBO and swapchain.
// depth != 0 creates a D32_FLOAT depth attachment on an offscreen FBO
// (ignored for window FBOs; swapchains carry their own).  The depth target
// uses the context's MSAA sample count when MSAA is enabled.  Depth
// clear/store policy is entirely up to the host pass that binds it.
// color_format overrides the color attachment format (INVALID follows
// vgpu_ctx_t::color_format); only honored for offscreen FBOs.
vg_fbo_t vgpu_create_fbo(vgpu_ctx_t* ctx, int width, int height,
	SDL_Window* window = nullptr, int depth = 0,
	SDL_GPUTextureFormat color_format = SDL_GPU_TEXTUREFORMAT_INVALID);
void vgpu_destroy_fbo(vg_fbo_t* fbo);
void vgpu_reset_fbo(vg_fbo_t* fbo, int width, int height);
int vgpu_acquire_swapchain(vgpu_ctx_t* ctx, vg_fbo_t* fbo, int count, bool wait);

// Returns the FBO color texture: the swapchain for window FBOs, color_tex for
// offscreen FBOs.  Mainly used by SDL_DownloadFromGPUTexture readback in
// offscreen tests.
SDL_GPUTexture* vgpu_fbo_color_texture(vg_fbo_t* fbo);

// Resolves the FBO color attachment format: the explicit override, the
// context's color format when the FBO has no override, or INVALID for a
// context-less FBO.
SDL_GPUTextureFormat vgpu_fbo_color_format(vg_fbo_t* fbo);

// Checks that a color format is renderable on this device: 2D texture with
// COLOR_TARGET | SAMPLER usage, and MSAA sample count support when samples
// != 1.  Call before creating an FBO with an explicit color format.
int vgpu_supports_color_format(vgpu_ctx_t* ctx, SDL_GPUTextureFormat format,
	SDL_GPUSampleCount samples);

// Returns the FBO depth attachment created by vgpu_create_fbo(..., depth=1),
// or NULL when the FBO has none.  The host binds it in its own render pass
// via SDL_GPUDepthStencilTargetInfo (recipe: sample/phase0_renderer.cpp
// draw_scene_target -- clear_depth=1, LOADOP_CLEAR, STOREOP_DONT_CARE unless
// a later pass samples it, cycle=true).
SDL_GPUTexture* vgpu_fbo_depth_texture(vg_fbo_t* fbo);

// The command buffer vgpu is currently encoding into: acquired by
// vgpu_acquire_swapchain(), submitted by vgpu_render_frame().  Game engines
// use it to encode their own work between the two -- e.g. SDL_BlitGPUTexture
// of the 3D scene into the swapchain texture before the UI pass -- so blit
// and UI stay ordered inside one submission.  NULL outside a frame.
SDL_GPUCommandBuffer* vgpu_current_command_buffer(vgpu_ctx_t* ctx);

// The color format all vgpu targets use: the swapchain format the context was
// created with.  Offscreen FBOs created by vgpu_create_fbo() and native
// textures borrowed through vgpu_texture_init_native() must use it.
SDL_GPUTextureFormat vgpu_color_format(vgpu_ctx_t* ctx);

// MSAA sample count the context was actually created with: the requested value
// may have been clamped down to what the device supports.  For the ovg/2D
// backend this is the *only* anti-aliasing control -- the vector passes get
// their edge smoothing from the multisampled render target, so a context
// created with SAMPLECOUNT_1 draws aliased geometry no matter what the caller
// asks for at draw time.
SDL_GPUSampleCount vgpu_context_samples(vgpu_ctx_t* ctx);

// Native texture handle: samples an existing caller-owned SDL_GPUTexture.
// vgui only borrows it and never releases it; the default sampler is
// linear + clamp.  The texture must have SDL_GPU_TEXTUREUSAGE_SAMPLER.
bool vgpu_texture_init_native(vgpu_ctx_t* ctx, ovg_texture_t* tex,
	SDL_GPUTexture* texture, int width, int height, SDL_GPUTextureFormat format);
// Replaces the texture pointer/size in place while keeping image.id stable;
// the caller releases the old texture after vgpu_wait_idle.
bool vgpu_texture_update_native(vgpu_ctx_t* ctx, ovg_texture_t* tex,
	SDL_GPUTexture* texture, int width, int height, SDL_GPUTextureFormat format);
// Removes the backend entry and defers sampler release; the borrowed
// SDL_GPUTexture is not released.
void vgpu_texture_fini_native(vgpu_ctx_t* ctx, ovg_texture_t* tex);

// 独立命令缓冲的 GPU 线性缩小拷贝（src 全图 → dst 全图，双线性滤波）。
// 静态烘焙用：先离屏渲一个超采样 pass，再把结果缩小到最终纹理。
// 不要在帧 pass 序列中间调用（本函数自行获取并提交命令缓冲）。
void vgpu_blit_texture(vgpu_ctx_t* ctx, SDL_GPUTexture* dst, int dst_w,
	int dst_h, SDL_GPUTexture* src, int src_w, int src_h);

// Submits one frame.  The fbo in data must already have a swapchain from
// vgpu_acquire_swapchain, or this function acquires one; the command buffer is
// submitted when the function returns.
//
// Frame-order rule: put ALL vgpu passes of a frame into ONE
// vgpu_render_frame call (one ovg_draw_data_t per pass target, ordered) so
// they encode into a single submission.  SDL3 does not serialize separate
// submissions against each other -- splitting a frame's passes across
// submissions races on the shared vertex/index buffers and on render targets
// written in one submission and sampled in the next.  Within one submission
// SDL3's automatic barriers order everything (offscreen write -> sampled,
// blit -> LOAD); offscreen FBOs rewritten every frame should set
// vg_fbo_t::cycle_target.
void vgpu_render_frame(vgpu_ctx_t* ctx, ovg_draw_data_t* data, size_t count);
void vgpu_wait_idle(vgpu_ctx_t* ctx);
