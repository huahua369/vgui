// sample/vgui_texture_test.cpp
//
// Offscreen readback test for the vgui native-texture path:
// create -> VGUI_IMAGE draw -> source crop / tint / flip -> update -> release.
//
// The image widget geometry and vgui clip state are handled by vgui; the
// sampled pixels verify that VGUI_IMAGE maps its state onto ovg::add_texture
// correctly.  All texture sampling is linear + clamp.

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#include <glm/glm.hpp>

#include "ovg.h"
#include "vgui.h"
#include "vgui_sdl3.h"
#include "vgui_renderer_sdl3.h"
#include "gpu_test_util.h"

#include <cstdio>
#include <vector>

struct vgui_test_context
{
	SDL_GPUDevice* device = nullptr;
	vgpu_device_t* vgpu_device = nullptr;
	vgpu_ctx_t* vgpu_ctx = nullptr;
	vg_fbo_t fbo = {};
	ovg_ctx_cb* cb = nullptr;
	rvg_t* vg = nullptr;
	int failures = 0;
	int checks = 0;
	int width = 0;
	int height = 0;
	std::vector<uint32_t> pixels;
};

static void expect_pixel(vgui_test_context& t, int x, int y, uint32_t expected, const char* label)
{
	++t.checks;
	if (x < 0 || y < 0 || x >= t.width || y >= t.height ||
		t.pixels.size() < (size_t)t.width * t.height)
	{
		std::printf("FAIL %-24s out of range (%d,%d)\n", label, x, y);
		++t.failures;
		return;
	}
	const uint32_t actual = t.pixels[(size_t)y * t.width + x];
	if (actual != expected)
	{
		std::printf("FAIL %-24s at (%d,%d): got 0x%08X expected 0x%08X\n",
			label, x, y, actual, expected);
		++t.failures;
	}
}

static vgui_widget* add_image(vgui_context* ui, vgui_texture* texture,
	float x, float y, float w, float h)
{
	vgui_widget* image = vgui_widget_add(vgui_root(ui), VGUI_IMAGE, nullptr);
	vgui_widget_set_pos(image, x, y);
	vgui_widget_set_size(image, w, h);
	vgui_widget_set_background(image, 0);
	vgui_widget_set_border(image, 0, 0);
	vgui_image_set_fit(image, VGUI_IMAGE_FIT_STRETCH);
	vgui_image_set_pixel_snap(image, 1);
	vgui_image_set_texture(image, texture);
	return image;
}

static bool render_vgui(vgui_test_context& t, vgui_context* ui)
{
	t.cb->clear(t.vg);
	t.cb->reset_clip(t.vg, 1);
	vgui_draw(ui, t.cb, t.vg);

	ovg_draw_data_t data = get_draw_list(t.vg);
	data.fbo = &t.fbo;
	vgpu_render_frame(t.vgpu_ctx, &data, 1);
	vgpu_wait_idle(t.vgpu_ctx);

	return download_texture(t.device, vgpu_fbo_color_texture(&t.fbo),
		t.width, t.height, t.pixels);
}

int main()
{
	if (!SDL_Init(SDL_INIT_VIDEO))
	{
		std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
		return 2;
	}
	SDL_GPUDevice* device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, true, "vulkan");
	if (!device)
	{
		std::fprintf(stderr, "SDL_CreateGPUDevice failed: %s\n", SDL_GetError());
		SDL_Quit();
		return 2;
	}

	vgui_test_context t;
	t.device = device;
	t.width = 256;
	t.height = 128;
	t.vgpu_device = vgpu_create_device(device);
	t.vgpu_ctx = vgpu_create_context(t.vgpu_device,
		SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
		SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT,
		SDL_GPU_SAMPLECOUNT_1);
	if (!t.vgpu_ctx)
	{
		std::fprintf(stderr, "vgpu_create_context failed\n");
		return 2;
	}
	t.fbo = vgpu_create_fbo(t.vgpu_ctx, t.width, t.height, nullptr);
	t.cb = new_ctx_cb();
	t.vg = t.cb->new_rvg(t.cb->ac);
	t.vg->width = t.width;
	t.vg->height = t.height;

	const uint32_t red = rgba(255, 0, 0);
	const uint32_t green = rgba(0, 255, 0);
	const uint32_t blue = rgba(0, 0, 255);
	const uint32_t yellow = rgba(255, 255, 0);
	const uint32_t orange = rgba(255, 128, 0);
	const uint32_t clear = rgba(0, 0, 0, 0);

	const uint32_t quad[4] = { red, green, blue, yellow };
	SDL_GPUTexture* src_quad = create_texture(device, 2, 2, quad);
	if (!src_quad)
	{
		std::fprintf(stderr, "create_texture(quad) failed: %s\n", SDL_GetError());
		return 2;
	}

	vgui_context* ui = vgui_create(nullptr, 16.0f);
	vgui_context_set_size(ui, t.width, t.height);
	vgui_texture* native = vgui_sdl3_texture_create_native(ui, t.vgpu_ctx,
		src_quad, 2, 2, VGUI_TEXTURE_FORMAT_RGBA8);
	if (!native)
	{
		std::fprintf(stderr, "vgui_sdl3_texture_create_native failed\n");
		return 2;
	}

	vgui_widget* dir = add_image(ui, native, 0, 0, 32, 32);
	(void)dir;
	vgui_widget* crop = add_image(ui, native, 32, 0, 16, 32);
	vgui_image_set_source(crop, 1, 0, 1, 2);
	vgui_widget* tinted = add_image(ui, native, 48, 0, 32, 32);
	// vgui_color is public 0xAARRGGBB; the draw path converts it to OVG's
	// channel order.  The texture pixels use memory RGBA order, so `red` below
	// is the texture's red texel, not the same literal as the public tint.
	vgui_image_set_tint(tinted, 0xFFFF0000u);
	vgui_widget* flip_h = add_image(ui, native, 0, 32, 32, 32);
	vgui_image_set_flip(flip_h, 1, 0);
	vgui_widget* flip_v = add_image(ui, native, 0, 64, 32, 32);
	vgui_image_set_flip(flip_v, 0, 1);
	add_image(ui, native, 192, 0, 16, 16);

	vgui_update(ui, 0.0f);
	vgui_layout(ui);
	if (!render_vgui(t, ui))
	{
		std::fprintf(stderr, "first render/download failed\n");
		return 2;
	}

	expect_pixel(t, 4, 4, red, "direction top-left");
	expect_pixel(t, 28, 4, green, "direction top-right");
	expect_pixel(t, 4, 28, blue, "direction bottom-left");
	expect_pixel(t, 28, 28, yellow, "direction bottom-right");

	expect_pixel(t, 44, 4, green, "crop top");
	expect_pixel(t, 44, 28, yellow, "crop bottom");

	expect_pixel(t, 52, 4, red, "tint red texel");
	expect_pixel(t, 76, 4, rgba(0, 0, 0), "tint green texel");

	expect_pixel(t, 4, 36, green, "flip h left");
	expect_pixel(t, 28, 36, red, "flip h right");
	expect_pixel(t, 4, 68, blue, "flip v top");
	expect_pixel(t, 4, 92, red, "flip v bottom");
	expect_pixel(t, 250, 120, clear, "untouched background");

	// In-place update: same vgui_texture handle, new SDL_GPUTexture pointer.
	const uint32_t update_pixels[4] = { orange, orange, orange, orange };
	SDL_GPUTexture* src_update = create_texture(device, 2, 2, update_pixels);
	if (!src_update)
	{
		std::fprintf(stderr, "create_texture(update) failed: %s\n", SDL_GetError());
		return 2;
	}
	if (!vgui_sdl3_texture_update_native(native, t.vgpu_ctx, src_update,
		2, 2, VGUI_TEXTURE_FORMAT_RGBA8))
	{
		std::fprintf(stderr, "vgui_sdl3_texture_update_native failed\n");
		return 2;
	}
	if (!render_vgui(t, ui))
	{
		std::fprintf(stderr, "update render/download failed\n");
		return 2;
	}
	expect_pixel(t, 196, 4, orange, "update new pixels");

	// Drop the creator reference first; the widgets still hold references.
	vgui_texture_destroy(native);

	// Release through VGUI.  vgui defers the backend release until the next
	// draw; vgpu_destroy_context flushes anything still pending.
	vgui_destroy(ui);
	t.cb->destroy_rvg(t.vg);
	free_ctx_cb(t.cb);
	vgpu_destroy_fbo(&t.fbo);
	vgpu_destroy_context(t.vgpu_ctx);
	vgpu_destroy_device(t.vgpu_device);
	SDL_ReleaseGPUTexture(device, src_update);
	SDL_ReleaseGPUTexture(device, src_quad);
	SDL_DestroyGPUDevice(device);
	SDL_Quit();

	if (t.failures)
	{
		std::printf("vgui texture test: %d/%d checks failed\n", t.failures, t.checks);
		return 1;
	}
	std::printf("vgui texture test: ok (%d checks)\n", t.checks);
	return 0;
}
