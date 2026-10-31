// sample/ovg_texture_test.cpp
//
// Offscreen readback test for OVG's native SDL_GPUTexture path.
// Covers: direction, source crop, tint, horizontal/vertical flip, 9-slice,
// rotation, mat3x2, current clip, in-place update, and fini.
//
// The test creates its own SDL_GPUDevice (Vulkan) and a small offscreen FBO.
// It exits with 0 when every sampled pixel matches.

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

// ovg_c.h / ovg_renderer_sdl3.h use glm types without including glm.
#include <glm/glm.hpp>

#include "ovg.h"
#include "ovg_renderer_sdl3.h"
#include "gpu_test_util.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

struct test_context
{
	SDL_GPUDevice* device = nullptr;
	ovg_device_t* ovg_device = nullptr;
	ovg_ctx_t* ctx = nullptr;
	vg_fbo_t fbo = {};
	ovg_ctx_cb* cb = nullptr;
	rvg_t* vg = nullptr;
	int failures = 0;
	int checks = 0;
	const std::vector<uint32_t>* pixels = nullptr;
	int width = 0;
	int height = 0;
};

static void expect_pixel(test_context& t, int x, int y, uint32_t expected, const char* label)
{
	++t.checks;
	if (!t.pixels || x < 0 || y < 0 || x >= t.width || y >= t.height)
	{
		std::printf("FAIL %-24s out of range (%d,%d)\n", label, x, y);
		++t.failures;
		return;
	}
	const uint32_t actual = (*t.pixels)[(size_t)y * t.width + x];
	if (actual != expected)
	{
		std::printf("FAIL %-24s at (%d,%d): got 0x%08X expected 0x%08X\n",
			label, x, y, actual, expected);
		++t.failures;
	}
}

static void expect_not_pixel(test_context& t, int x, int y, uint32_t unexpected, const char* label)
{
	++t.checks;
	if (!t.pixels || x < 0 || y < 0 || x >= t.width || y >= t.height)
	{
		std::printf("FAIL %-24s out of range (%d,%d)\n", label, x, y);
		++t.failures;
		return;
	}
	const uint32_t actual = (*t.pixels)[(size_t)y * t.width + x];
	if (actual == unexpected)
	{
		std::printf("FAIL %-24s at (%d,%d): got 0x%08X (unexpected)\n",
			label, x, y, actual);
		++t.failures;
	}
}

static void add_texture(test_context& t, ovg_texture_t* texture, const glm::ivec4& rc,
	const glm::ivec4& dst, uint32_t color, int flip = 0, float angle = 0.0f,
	const glm::vec2& center = {}, bool center_set = false,
	const glm::mat3x2* matrix = nullptr, const glm::ivec4& sliced = {})
{
	ovg_texture_r r = {};
	r.tex = texture;
	r.rc = rc;
	r.sliced = sliced;
	r.dst = dst;
	r.color = color;
	r.flip = (int8_t)flip;
	r.center_set = center_set ? 1 : 0;
	r.angle = angle;
	r.center = center;
	r.matrix = matrix;
	t.cb->add_texture(t.vg, &r);
}

static bool begin_frame(test_context& t)
{
	t.cb->clear(t.vg);
	t.cb->reset_clip(t.vg, 1);
	return true;
}

static bool render_and_download(test_context& t)
{
	ovg_draw_data_t data = get_draw_list(t.vg);
	data.fbo = &t.fbo;
	ovg_render_frame(t.ctx, &data, 1);
	ovg_wait_idle(t.ctx);
	t.pixels = nullptr;
	static std::vector<uint32_t> pixels;
	if (!download_texture(t.device, ovg_fbo_color_texture(&t.fbo), t.width, t.height, pixels))
		return false;
	t.pixels = &pixels;
	return true;
}

int main(int argc, char** argv)
{
	(void)argc;
	(void)argv;
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

	test_context t;
	t.device = device;
	t.width = 256;
	t.height = 128;
	t.ovg_device = new_sdl3gpu_device(device);
	t.ctx = new_ovgctx_sdl3(t.ovg_device,
		SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
		SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT,
		SDL_GPU_SAMPLECOUNT_8);
	if (!t.ctx)
	{
		std::fprintf(stderr, "new_ovgctx_sdl3 failed\n");
		return 2;
	}
	t.fbo = new_vgfbo_sdl3(t.ctx, t.width, t.height, nullptr);
	t.fbo.display_size = { t.width, t.height };
	t.fbo.upsize = false;
	t.cb = new_ctx_cb();
	t.vg = t.cb->new_rvg(t.cb->ac);
	t.vg->width = t.width;
	t.vg->height = t.height;

	const uint32_t red = rgba(255, 0, 0);
	const uint32_t green = rgba(0, 255, 0);
	const uint32_t blue = rgba(0, 0, 255);
	const uint32_t yellow = rgba(255, 255, 0);
	const uint32_t magenta = rgba(255, 0, 255);
	const uint32_t cyan = rgba(0, 255, 255);
	const uint32_t orange = rgba(255, 128, 0);
	const uint32_t clear = rgba(0, 0, 0, 0);

	const uint32_t quad[4] = { red, green, blue, yellow };
	SDL_GPUTexture* src_quad = create_texture(device, 2, 2, quad);
	if (!src_quad)
	{
		std::fprintf(stderr, "create_texture(quad) failed: %s\n", SDL_GetError());
		return 2;
	}

	uint32_t border[16 * 16];
	for (int i = 0; i < 16 * 16; ++i)
		border[i] = magenta;
	for (int yy = 4; yy < 12; ++yy)
		for (int xx = 4; xx < 12; ++xx)
			border[yy * 16 + xx] = cyan;
	SDL_GPUTexture* src_nine = create_texture(device, 16, 16, border);
	if (!src_nine)
	{
		std::fprintf(stderr, "create_texture(9-slice) failed: %s\n", SDL_GetError());
		return 2;
	}

	ovg_texture_t tex = {};
	if (!ovg_texture_init_native(t.ctx, &tex, src_quad, 2, 2, VG_FORMAT_RGBA8))
	{
		std::fprintf(stderr, "ovg_texture_init_native failed\n");
		return 2;
	}
	ovg_texture_t tex_nine = {};
	if (!ovg_texture_init_native(t.ctx, &tex_nine, src_nine, 16, 16, VG_FORMAT_RGBA8))
	{
		std::fprintf(stderr, "ovg_texture_init_native(9-slice) failed\n");
		return 2;
	}

	begin_frame(t);
	add_texture(t, &tex, { 0, 0, 2, 2 }, { 0, 0, 32, 32 }, 0xFFFFFFFFu);
	add_texture(t, &tex, { 1, 0, 1, 2 }, { 32, 0, 16, 32 }, 0xFFFFFFFFu);
	add_texture(t, &tex, { 0, 0, 2, 2 }, { 48, 0, 32, 32 }, rgba(255, 0, 0));
	add_texture(t, &tex, { 0, 0, 2, 2 }, { 0, 32, 32, 32 }, 0xFFFFFFFFu, FLIP_HORIZONTAL);
	add_texture(t, &tex, { 0, 0, 2, 2 }, { 0, 64, 32, 32 }, 0xFFFFFFFFu, FLIP_VERTICAL);
	add_texture(t, &tex_nine, { 0, 0, 16, 16 }, { 96, 0, 32, 32 }, 0xFFFFFFFFu,
		0, 0.0f, {}, false, nullptr, { 4, 4, 4, 4 });
	add_texture(t, &tex, { 0, 0, 2, 2 }, { 128, 0, 32, 32 }, 0xFFFFFFFFu, 0, 90.0f);

	glm::mat3x2 translate(1.0f);
	translate[2] = glm::vec2(8.0f, 0.0f);
	add_texture(t, &tex, { 0, 0, 2, 2 }, { 0, 96, 16, 16 }, 0xFFFFFFFFu,
		0, 0.0f, {}, false, &translate);

	t.cb->clip_rect(t.vg, 168, 72, 16, 16);
	add_texture(t, &tex, { 0, 0, 2, 2 }, { 160, 64, 32, 32 }, 0xFFFFFFFFu);
	t.cb->reset_clip(t.vg, 1);

	if (!render_and_download(t))
	{
		std::fprintf(stderr, "render/download failed\n");
		return 2;
	}

	// Direction: source row 0 is the top row, Q0 is top-left.
	// Sample the outer half of each quadrant; bilinear filtering at a 2x2
	// texture's quadrant center is already on a texel boundary.
	expect_pixel(t, 4, 4, red, "direction top-left");
	expect_pixel(t, 28, 4, green, "direction top-right");
	expect_pixel(t, 4, 28, blue, "direction bottom-left");
	expect_pixel(t, 28, 28, yellow, "direction bottom-right");

	// Source crop: right column only.  A one-texel source crop still uses
	// bilinear filtering, so sample near its right edge to avoid blending
	// with the left texel.
	expect_pixel(t, 44, 4, green, "crop top");
	expect_pixel(t, 44, 28, yellow, "crop bottom");
	expect_pixel(t, 34, 40, clear, "crop outside-left");

	// Tint red: red texel survives, green texel becomes black.
	expect_pixel(t, 52, 4, red, "tint red texel");
	expect_pixel(t, 76, 4, rgba(0, 0, 0), "tint green texel");

	// Flips.
	expect_pixel(t, 4, 36, green, "flip h left");
	expect_pixel(t, 28, 36, red, "flip h right");
	expect_pixel(t, 4, 68, blue, "flip v top");
	expect_pixel(t, 4, 92, red, "flip v bottom");

	// 9-slice: 4 px source border and 4 px destination border.
	expect_pixel(t, 98, 2, magenta, "9-slice top border");
	expect_pixel(t, 112, 16, cyan, "9-slice center");
	expect_pixel(t, 126, 29, magenta, "9-slice right border");

	// +90 degrees: top-left -> top-right, top-right -> bottom-right,
	// bottom-left -> top-left, bottom-right -> bottom-left.
	expect_pixel(t, 130, 2, blue, "rotate top-left");
	expect_pixel(t, 158, 2, red, "rotate top-right");
	expect_pixel(t, 130, 30, yellow, "rotate bottom-left");
	expect_pixel(t, 158, 30, green, "rotate bottom-right");

	// mat3x2 translation by +8 px: left edge moves from x=0 to x=8.
	expect_pixel(t, 2, 104, clear, "matrix before");
	expect_pixel(t, 10, 98, red, "matrix after");
	expect_pixel(t, 22, 98, green, "matrix after-right");

	// Current clip: draw 160..192, clip 168..184.
	expect_pixel(t, 164, 68, clear, "clip outside-left");
	expect_not_pixel(t, 172, 76, clear, "clip inside");
	expect_pixel(t, 188, 92, clear, "clip outside-right");
	expect_pixel(t, 250, 120, clear, "untouched background");

	// In-place update: same handle id, new source pixels.
	const uint32_t update_pixels[4] = { orange, orange, orange, orange };
	SDL_GPUTexture* src_update = create_texture(device, 2, 2, update_pixels);
	if (!src_update)
	{
		std::fprintf(stderr, "create_texture(update) failed: %s\n", SDL_GetError());
		return 2;
	}
	const uint32_t old_id = tex.image.id;
	if (!ovg_texture_update_native(t.ctx, &tex, src_update, 2, 2, VG_FORMAT_RGBA8))
	{
		std::fprintf(stderr, "ovg_texture_update_native failed\n");
		return 2;
	}
	if (tex.image.id != old_id)
	{
		std::fprintf(stderr, "update changed image.id (%u -> %u)\n", old_id, tex.image.id);
		return 2;
	}

	begin_frame(t);
	add_texture(t, &tex, { 0, 0, 2, 2 }, { 192, 0, 16, 16 }, 0xFFFFFFFFu);
	if (!render_and_download(t))
	{
		std::fprintf(stderr, "update render/download failed\n");
		return 2;
	}
	expect_pixel(t, 196, 4, orange, "update new pixels");

	// fini is idempotent and clears the handle.
	ovg_texture_fini_native(t.ctx, &tex);
	ovg_texture_fini_native(t.ctx, &tex);
	if (tex.image.id != 0)
	{
		std::fprintf(stderr, "fini did not clear image.id\n");
		return 2;
	}
	ovg_texture_fini_native(t.ctx, &tex_nine);
	ovg_wait_idle(t.ctx);

	SDL_ReleaseGPUTexture(device, src_update);
	SDL_ReleaseGPUTexture(device, src_nine);
	SDL_ReleaseGPUTexture(device, src_quad);

	free_vgfbo_sdl3(&t.fbo);
	t.cb->destroy_rvg(t.vg);
	free_ctx_cb(t.cb);
	free_ovgctx_sdl3(t.ctx);
	free_sdl3gpu_device(t.ovg_device);
	SDL_DestroyGPUDevice(device);
	SDL_Quit();

	if (t.failures)
	{
		std::printf("ovg texture test: %d/%d checks failed\n", t.failures, t.checks);
		return 1;
	}
	std::printf("ovg texture test: ok (%d checks)\n", t.checks);
	return 0;
}
