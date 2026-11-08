#pragma once
/*
 * vgui_render_target.h
 * SDL3 backend: persistent offscreen vector/UI render target.
 *
 * A target owns one offscreen vg_fbo_t, one vgui_texture bound to its color
 * attachment and one private rvg_t recording canvas.  The vgui_texture pointer
 * is stable for the target lifetime: resizing updates the native handle in
 * place, so widgets do not need to be rebound.
 *
 * The ovg_ctx_cb* passed to create() is borrowed, not owned.  It must outlive
 * every vgui_context used with the target, because vgui_draw() stores it in
 * vgui_context::draw_cb and later uses it to destroy cached subtree recordings.
 * A host that draws a window already owns such a callback; a pure offscreen
 * host can create one with new_ctx_cb() and free it after all contexts and
 * targets are gone.
 *
 * Rendering is split into record and submit steps:
 *
 *     ovg_draw_data_t pass;
 *     vgui_render_target_record_context(target, ui, &pass);
 *     ...
 *     vgpu_render_frame(gpu_ctx, passes, pass_count);
 *
 * record() only produces draw data; it never submits.  Put every pass of a
 * frame into one vgpu_render_frame() call so SDL3's automatic barriers order
 * the offscreen write before the pass that samples it.  submit() is a
 * convenience for static/standalone baking; sampling its result in a later
 * submission requires vgpu_wait_idle() first.
 *
 * The target is a backend object: vgui.h stays free of SDL and GPU types and
 * VGUI_CORE_ONLY builds do not compile this interface.
 */

#include <cstdint>

#include <vgui/vgui.h>
#include <vgui/backend/vgui_renderer_sdl3.h>

struct vgui_render_target;

typedef struct vgui_render_target_desc {
	uint32_t struct_size;

	// Target size in physical pixels.  Both must be > 0.
	int width;
	int height;

	// 1 (default): clear the color attachment to transparent black before
	// each pass, and cycle the color target so a texture rewritten every
	// frame can be sampled later in the same submission.
	// 0: preserve the existing texture contents (overlay/accumulation).  This
	// also disables cycle_target; do not sample such a target in the same
	// frame while a previous submission may still be reading it.
	int clear;
} vgui_render_target_desc;

typedef void (*vgui_render_target_draw_fn)(void* user, ovg_ctx_cb* cb,
	rvg_t* vg, int width, int height);

// Fills desc with struct_size = sizeof(*desc), width = height = 0, clear = 1.
// Callers must still set width/height.
void vgui_render_target_desc_default(vgui_render_target_desc* desc);

// owner is the vgui_context that owns/display the resulting vgui_texture.
// gpu_ctx must be the vgpu context used to submit the target's draw data.
// cb is borrowed and must outlive every context recorded through this target;
// see the lifetime note at the top of this header.  The target allocates its
// rvg_t from cb but never frees cb itself.
// Returns NULL on failure; vgui_render_target_last_error(NULL) describes why.
vgui_render_target* vgui_render_target_create(vgui_context* owner,
	vgpu_ctx_t* gpu_ctx, ovg_ctx_cb* cb,
	const vgui_render_target_desc* desc);

// Waits for GPU idle, detaches the native texture handle and releases the FBO.
// Any widget that still displays the texture will draw nothing afterwards;
// clear those bindings first (or destroy the widgets) for a clean shutdown.
void vgui_render_target_destroy(vgui_render_target* target);

// Recreates the FBO and updates the existing vgui_texture in place.  The
// vgui_texture pointer and its image id stay stable.  Waits for GPU idle, so
// call it at a frame boundary.  On failure the old target remains usable.
int vgui_render_target_resize(vgui_render_target* target, int width, int height);

// Updates the load-op used by the next pass.  See vgui_render_target_desc::clear.
void vgui_render_target_set_clear(vgui_render_target* target, int clear);
void vgui_render_target_get_size(const vgui_render_target* target,
	int* width, int* height);

// Borrowed texture owned by the target.  Do not pass it to
// vgui_texture_destroy().  The pointer survives vgui_render_target_resize().
vgui_texture* vgui_render_target_texture(vgui_render_target* target);

// Raw FBO color texture (never a swapchain).  Useful for SDL_BlitGPUTexture,
// readback and engine-side sampling.  The pointer changes after resize().
SDL_GPUTexture* vgui_render_target_native_texture(vgui_render_target* target);

// Last failure for this target.  target may be NULL to query the most recent
// create() failure.  Returns an empty string when there is no error; the
// returned pointer is valid until the next call on the same target/thread.
const char* vgui_render_target_last_error(const vgui_render_target* target);

// Records one pass worth of commands into target->rvg.  The canvas is cleared
// and the clip reset before draw() runs.  out_data.fbo is already filled in;
// pass it to vgpu_render_frame as-is.  The data is valid until the next
// record/resize/destroy call on the same target and must be submitted in the
// same frame.
int vgui_render_target_record(vgui_render_target* target,
	vgui_render_target_draw_fn draw, void* user,
	ovg_draw_data_t* out_data);

// Convenience wrapper around record() for a vgui_context.  The context size
// must already match the target size; call vgui_context_set_size() first.
// Does not call vgui_update(): animation/time advancement stays with the host.
int vgui_render_target_record_context(vgui_render_target* target,
	vgui_context* ui, ovg_draw_data_t* out_data);

// Static/standalone convenience: record + vgpu_render_frame(..., 1).
// Do not call this between an engine's own passes; use record() and one
// batched vgpu_render_frame() instead.
int vgui_render_target_submit(vgui_render_target* target,
	vgui_render_target_draw_fn draw, void* user);
int vgui_render_target_submit_context(vgui_render_target* target,
	vgui_context* ui);
