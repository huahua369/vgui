#pragma once
/*
 * vgui_renderer_sdl3.h
 * SDL3 GPU 2D vector renderer backend for vgui.
 *
 * Compared with the reference OVG renderer, this backend keeps only the 2D
 * features vgui uses: OVG fill/stroke triangles, solid and linear/radial/sweep
 * gradients, text-atlas and image triangles, rectangular clipping, MSAA and
 * multi-FBO/window swapchains.  It has no 3D geometry, path-template clipping,
 * instancing or compute effects, so it needs no depth/stencil attachments and
 * keeps resource ownership explicit.  It consumes OVG's ovg_draw_data_t
 * directly; vg_fbo_t is defined here while ovg_c.h only forward-declares it.
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
// color_tex.
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
	// Load-op of the UI render pass.  true (default) clears the target before
	// drawing; false keeps the existing content and blends the UI on top -- a
	// game engine renders its 3D scene into the same texture first, then runs
	// vgui/ovg recording into it as an overlay (docs/runtime_roadmap.md R1).
	// Must be set before vgpu_render_frame(); within a frame each FBO is
	// rendered by exactly one pass.
	bool clear = true;
};

// Device: owns shaders and the default white texture; follows the
// SDL_GPUDevice lifetime.
vgpu_device_t* vgpu_create_device(SDL_GPUDevice* gpu_device);
void vgpu_destroy_device(vgpu_device_t* device);

// Context: owns pipelines, the texture table, GPU buffers and the current
// command buffer.  depth_format is kept for call-site compatibility; the
// backend creates no depth/stencil attachments.
vgpu_ctx_t* vgpu_create_context(vgpu_device_t* device,
	SDL_GPUTextureFormat color_format,
	SDL_GPUTextureFormat depth_format,
	SDL_GPUSampleCount samples);
void vgpu_destroy_context(vgpu_ctx_t* ctx);

// FBO and swapchain.
vg_fbo_t vgpu_create_fbo(vgpu_ctx_t* ctx, int width, int height, SDL_Window* window = nullptr);
void vgpu_destroy_fbo(vg_fbo_t* fbo);
void vgpu_reset_fbo(vg_fbo_t* fbo, int width, int height);
int vgpu_acquire_swapchain(vgpu_ctx_t* ctx, vg_fbo_t* fbo, int count, bool wait);

// Returns the FBO color texture: the swapchain for window FBOs, color_tex for
// offscreen FBOs.  Mainly used by SDL_DownloadFromGPUTexture readback in
// offscreen tests.
SDL_GPUTexture* vgpu_fbo_color_texture(vg_fbo_t* fbo);

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

// Submits one frame.  The fbo in data must already have a swapchain from
// vgpu_acquire_swapchain, or this function acquires one; the command buffer is
// submitted when the function returns.
void vgpu_render_frame(vgpu_ctx_t* ctx, ovg_draw_data_t* data, size_t count);
void vgpu_wait_idle(vgpu_ctx_t* ctx);
