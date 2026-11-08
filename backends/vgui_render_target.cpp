/*
 * vgui_render_target.cpp
 * SDL3 backend: persistent offscreen vector/UI render target.
 *
 * The implementation deliberately keeps one rvg_t per target.  A target is
 * expected to be recorded once per frame; record() reuses the canvas and
 * clears it so the returned ovg_draw_data_t only contains that pass.
 *
 * Lifetime of the native bridge:
 *   vgpu_create_fbo() creates the color texture with SAMPLER | COLOR_TARGET.
 *   vgui_sdl3_texture_create_native() registers a borrowed native entry in the
 *   vgpu context and returns the stable vgui_texture exposed by the target.
 *   resize() recreates the FBO and updates that wrapper in place, so image
 *   widgets keep the same vgui_texture pointer.
 */

#include <vgui/backend/vgui_render_target.h>

#include <vgui/backend/vgui_sdl3.h>
#include <vgui_internal.h>

#include <cstddef>
#include <cstring>
#include <string>

struct vgui_render_target
{
	vgui_context* owner = nullptr;
	vgpu_ctx_t* gpu_ctx = nullptr;

	vgui_texture* texture = nullptr;
	ovg_ctx_cb* cb = nullptr;
	rvg_t* vg = nullptr;

	vg_fbo_t fbo = {};
	int width = 0;
	int height = 0;
	vgui_texture_format format = VGUI_TEXTURE_FORMAT_RGBA8;
	bool clear = true;

	std::string error;
};

namespace {

thread_local std::string g_create_error;

void set_create_error(const char* message)
{
	g_create_error = message ? message : "";
}

bool to_vgui_texture_format(SDL_GPUTextureFormat format,
	vgui_texture_format* out)
{
	if (!out)
		return false;
	switch (format)
	{
	case SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM:
		*out = VGUI_TEXTURE_FORMAT_RGBA8;
		return true;
	case SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM:
		*out = VGUI_TEXTURE_FORMAT_BGRA8;
		return true;
	case SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB:
		*out = VGUI_TEXTURE_FORMAT_RGBA8_SRGB;
		return true;
	case SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB:
		*out = VGUI_TEXTURE_FORMAT_BGRA8_SRGB;
		return true;
	case SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT:
		*out = VGUI_TEXTURE_FORMAT_RGBA16F;
		return true;
	case SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT:
		*out = VGUI_TEXTURE_FORMAT_RGBA32F;
		return true;
	case SDL_GPU_TEXTUREFORMAT_R16_FLOAT:
		*out = VGUI_TEXTURE_FORMAT_R16F;
		return true;
	case SDL_GPU_TEXTUREFORMAT_R32_FLOAT:
		*out = VGUI_TEXTURE_FORMAT_R32F;
		return true;
	default:
		return false;
	}
}

void destroy_partial(vgui_render_target* target)
{
	if (!target)
		return;
	if (target->cb && target->vg)
		target->cb->destroy_rvg(target->vg);
	target->vg = nullptr;
	// target->cb is borrowed and owned by the host; do not free it here.
	target->cb = nullptr;
	vgpu_destroy_fbo(&target->fbo);
	delete target;
}

void record_context_draw(void* user, ovg_ctx_cb* cb, rvg_t* vg,
	int /*width*/, int /*height*/)
{
	vgui_draw(static_cast<vgui_context*>(user), cb, vg);
}

int record_internal(vgui_render_target* target,
	vgui_render_target_draw_fn draw, void* user,
	ovg_draw_data_t* out_data)
{
	if (out_data)
		*out_data = {};
	if (!target)
		return 0;
	if (!out_data)
	{
		target->error = "out_data is NULL";
		return 0;
	}
	if (!draw)
	{
		target->error = "draw callback is NULL";
		return 0;
	}
	if (!target->cb || !target->vg)
	{
		target->error = "recording canvas is not initialized";
		return 0;
	}
	if (target->width <= 0 || target->height <= 0)
	{
		target->error = "target has no valid size";
		return 0;
	}

	target->vg->width = target->width;
	target->vg->height = target->height;
	target->cb->clear(target->vg);
	target->cb->reset_clip(target->vg, 1);
	draw(user, target->cb, target->vg, target->width, target->height);

	*out_data = get_draw_list(target->vg);
	out_data->fbo = &target->fbo;
	target->error.clear();
	return 1;
}

} // namespace

void vgui_render_target_desc_default(vgui_render_target_desc* desc)
{
	if (!desc)
		return;
	std::memset(desc, 0, sizeof(*desc));
	desc->struct_size = static_cast<uint32_t>(sizeof(*desc));
	desc->clear = 1;
}

vgui_render_target* vgui_render_target_create(vgui_context* owner,
	vgpu_ctx_t* gpu_ctx, ovg_ctx_cb* cb,
	const vgui_render_target_desc* desc)
{
	set_create_error("");
	if (!owner)
	{
		set_create_error("owner context is NULL");
		return nullptr;
	}
	if (!gpu_ctx)
	{
		set_create_error("gpu context is NULL");
		return nullptr;
	}
	if (!cb)
	{
		set_create_error("ovg callback is NULL");
		return nullptr;
	}
	if (!desc)
	{
		set_create_error("desc is NULL");
		return nullptr;
	}

	vgui_render_target_desc settings;
	vgui_render_target_desc_default(&settings);
	std::size_t bytes = sizeof(settings);
	if (desc->struct_size && desc->struct_size < bytes)
		bytes = desc->struct_size;
	std::memcpy(&settings, desc, bytes);

	if (settings.width <= 0 || settings.height <= 0)
	{
		set_create_error("width and height must be > 0");
		return nullptr;
	}

	vgui_texture_format format = VGUI_TEXTURE_FORMAT_RGBA8;
	if (!to_vgui_texture_format(vgpu_color_format(gpu_ctx), &format))
	{
		set_create_error("unsupported gpu color format");
		return nullptr;
	}

	vgui_render_target* target = new vgui_render_target();
	target->owner = owner;
	target->gpu_ctx = gpu_ctx;
	target->width = settings.width;
	target->height = settings.height;
	target->format = format;
	target->clear = settings.clear != 0;

	target->fbo = vgpu_create_fbo(gpu_ctx, settings.width, settings.height,
		nullptr);
	target->fbo.clear = target->clear;
	target->fbo.cycle_target = target->clear;
	if (!vgpu_fbo_color_texture(&target->fbo))
	{
		target->error = "failed to create offscreen fbo";
		set_create_error(target->error.c_str());
		destroy_partial(target);
		return nullptr;
	}

	target->cb = cb;
	target->vg = cb->new_rvg(cb->ac);
	if (!target->vg)
	{
		target->error = "failed to create recording canvas";
		set_create_error(target->error.c_str());
		destroy_partial(target);
		return nullptr;
	}

	target->texture = vgui_sdl3_texture_create_native(owner, gpu_ctx,
		vgpu_fbo_color_texture(&target->fbo), settings.width, settings.height,
		format);
	if (!target->texture)
	{
		target->error = "failed to create native vgui_texture";
		set_create_error(target->error.c_str());
		destroy_partial(target);
		return nullptr;
	}

	return target;
}

void vgui_render_target_destroy(vgui_render_target* target)
{
	if (!target)
		return;

	// No submitted GPU work may still reference the FBO or the native entry.
	if (target->gpu_ctx)
		vgpu_wait_idle(target->gpu_ctx);

	if (target->texture)
	{
		// Detach the backend handle immediately.  Widgets that still retain the
		// vgui_texture will simply draw nothing instead of sampling a freed
		// SDL_GPUTexture.
		if (target->texture->release_native)
		{
			target->texture->release_native(target->texture);
			target->texture->release_native = nullptr;
		}
		vgui_texture_destroy(target->texture);
		target->texture = nullptr;
	}

	if (target->cb && target->vg)
		target->cb->destroy_rvg(target->vg);
	target->vg = nullptr;
	// target->cb is borrowed and owned by the host; do not free it here.
	target->cb = nullptr;

	vgpu_destroy_fbo(&target->fbo);
	delete target;
}

int vgui_render_target_resize(vgui_render_target* target, int width, int height)
{
	if (!target)
		return 0;
	if (width <= 0 || height <= 0)
	{
		target->error = "width and height must be > 0";
		return 0;
	}
	if (width == target->width && height == target->height)
		return 1;
	if (!target->gpu_ctx)
	{
		target->error = "gpu context is NULL";
		return 0;
	}

	// Build the replacement first so a failed resize leaves the old target
	// intact.
	vg_fbo_t next = vgpu_create_fbo(target->gpu_ctx, width, height, nullptr);
	next.clear = target->fbo.clear;
	next.cycle_target = target->fbo.cycle_target;
	if (!vgpu_fbo_color_texture(&next))
	{
		target->error = "failed to create offscreen fbo";
		vgpu_destroy_fbo(&next);
		return 0;
	}

	if (!target->texture)
	{
		target->error = "target has no native texture";
		vgpu_destroy_fbo(&next);
		return 0;
	}

	vgpu_wait_idle(target->gpu_ctx);
	if (!vgui_sdl3_texture_update_native(target->texture, target->gpu_ctx,
		vgpu_fbo_color_texture(&next), width, height, target->format))
	{
		target->error = "failed to update native vgui_texture";
		vgpu_destroy_fbo(&next);
		return 0;
	}

	vgpu_destroy_fbo(&target->fbo);
	target->fbo = next;
	target->width = width;
	target->height = height;
	target->error.clear();
	return 1;
}

void vgui_render_target_set_clear(vgui_render_target* target, int clear)
{
	if (!target)
		return;
	target->clear = clear != 0;
	target->fbo.clear = target->clear;
	target->fbo.cycle_target = target->clear;
}

void vgui_render_target_get_size(const vgui_render_target* target,
	int* width, int* height)
{
	if (width)
		*width = target ? target->width : 0;
	if (height)
		*height = target ? target->height : 0;
}

vgui_texture* vgui_render_target_texture(vgui_render_target* target)
{
	return target ? target->texture : nullptr;
}

SDL_GPUTexture* vgui_render_target_native_texture(vgui_render_target* target)
{
	return target ? vgpu_fbo_color_texture(&target->fbo) : nullptr;
}

const char* vgui_render_target_last_error(const vgui_render_target* target)
{
	if (!target)
		return g_create_error.c_str();
	return target->error.c_str();
}

int vgui_render_target_record(vgui_render_target* target,
	vgui_render_target_draw_fn draw, void* user,
	ovg_draw_data_t* out_data)
{
	return record_internal(target, draw, user, out_data);
}

int vgui_render_target_record_context(vgui_render_target* target,
	vgui_context* ui, ovg_draw_data_t* out_data)
{
	if (out_data)
		*out_data = {};
	if (!target)
		return 0;
	if (!ui)
	{
		target->error = "context is NULL";
		return 0;
	}

	int width = 0;
	int height = 0;
	vgui_context_get_size(ui, &width, &height);
	if (width != target->width || height != target->height)
	{
		target->error =
			"context size mismatch: call vgui_context_set_size(ui, target_w, target_h)";
		return 0;
	}

	return record_internal(target, record_context_draw, ui, out_data);
}

int vgui_render_target_submit(vgui_render_target* target,
	vgui_render_target_draw_fn draw, void* user)
{
	ovg_draw_data_t data;
	if (!record_internal(target, draw, user, &data))
		return 0;
	vgpu_render_frame(target->gpu_ctx, &data, 1);
	return 1;
}

int vgui_render_target_submit_context(vgui_render_target* target,
	vgui_context* ui)
{
	ovg_draw_data_t data;
	if (!vgui_render_target_record_context(target, ui, &data))
		return 0;
	vgpu_render_frame(target->gpu_ctx, &data, 1);
	return 1;
}
