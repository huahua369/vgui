// sample/vgui_render_target_test.cpp
//
// Offscreen test for vgui_render_target:
//   vector -> target texture -> VGUI_IMAGE -> another target -> readback.
// It also covers context recording, resize-in-place, clear=0 preservation and
// the static submit() convenience.  No window is created; when no Vulkan device
// is available the test returns 77 so CTest marks it as skipped.

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include <vgui/vgui.h>
#include <vgui/backend/vgui_sdl3.h>
#include <vgui/backend/vgui_render_target.h>

#include "gpu_test_util.h"

namespace {

constexpr int kSkipReturnCode = 77;

struct gpu_state
{
	SDL_GPUDevice* device = nullptr;
	vgpu_device_t* vgpu_device = nullptr;
	vgpu_ctx_t* vgpu_ctx = nullptr;
};

int init_gpu(gpu_state& gpu)
{
	if (!SDL_Init(SDL_INIT_VIDEO))
	{
		std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
		return 2;
	}
	gpu.device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, true, "vulkan");
	if (!gpu.device)
	{
		std::fprintf(stderr, "SDL_CreateGPUDevice failed: %s\n", SDL_GetError());
		SDL_Quit();
		return kSkipReturnCode;
	}
	gpu.vgpu_device = vgpu_create_device(gpu.device);
	if (!gpu.vgpu_device)
	{
		std::fprintf(stderr, "vgpu_create_device failed\n");
		return 2;
	}
	gpu.vgpu_ctx = vgpu_create_context(gpu.vgpu_device,
		SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
		SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT,
		SDL_GPU_SAMPLECOUNT_1);
	if (!gpu.vgpu_ctx)
	{
		std::fprintf(stderr, "vgpu_create_context failed\n");
		return 2;
	}
	return 0;
}

void shutdown_gpu(gpu_state& gpu)
{
	if (gpu.vgpu_ctx)
		vgpu_destroy_context(gpu.vgpu_ctx);
	if (gpu.vgpu_device)
		vgpu_destroy_device(gpu.vgpu_device);
	if (gpu.device)
		SDL_DestroyGPUDevice(gpu.device);
	gpu = {};
	SDL_Quit();
}

void expect_pixel(const std::vector<uint32_t>& pixels, int width, int height,
	int x, int y, uint32_t expected, const char* label, int* failures)
{
	if (x < 0 || y < 0 || x >= width || y >= height ||
		pixels.size() < (size_t)width * height)
	{
		std::printf("FAIL %-24s out of range (%d,%d)\n", label, x, y);
		if (failures)
			++*failures;
		return;
	}
	const uint32_t actual = pixels[(size_t)y * width + x];
	if (actual != expected)
	{
		std::printf("FAIL %-24s at (%d,%d): got 0x%08X expected 0x%08X\n",
			label, x, y, actual, expected);
		if (failures)
			++*failures;
	}
	else
		std::printf("ok   %-24s at (%d,%d)\n", label, x, y);
}

void draw_full_red(void* user, ovg_ctx_cb* cb, rvg_t* vg, int width, int height)
{
	(void)user;
	cb->set_source_color(vg, rgba(255, 0, 0));
	cb->rectangle(vg, 0.0f, 0.0f, (float)width, (float)height);
	cb->fill(vg);
}

struct rect_spec
{
	float x;
	float y;
	float w;
	float h;
	uint32_t color;
};

void draw_rect(void* user, ovg_ctx_cb* cb, rvg_t* vg, int, int)
{
	const rect_spec* spec = static_cast<const rect_spec*>(user);
	if (!spec)
		return;
	cb->set_source_color(vg, spec->color);
	cb->rectangle(vg, spec->x, spec->y, spec->w, spec->h);
	cb->fill(vg);
}

bool submit_and_download(gpu_state& gpu, ovg_draw_data_t* passes,
	size_t count, vgui_render_target* readback, std::vector<uint32_t>& pixels)
{
	vgpu_render_frame(gpu.vgpu_ctx, passes, count);
	vgpu_wait_idle(gpu.vgpu_ctx);

	int width = 0;
	int height = 0;
	vgui_render_target_get_size(readback, &width, &height);
	return download_texture(gpu.device,
		vgui_render_target_native_texture(readback), width, height, pixels);
}

} // namespace

int main()
{
	gpu_state gpu;
	const int init = init_gpu(gpu);
	if (init != 0)
	{
		shutdown_gpu(gpu);
		return init;
	}

	int failures = 0;
	auto check = [&failures](bool ok, const char* label)
	{
		if (!ok)
		{
			std::printf("FAIL %s\n", label);
			++failures;
		}
		else
			std::printf("ok   %s\n", label);
	};

	ovg_ctx_cb* cb = new_ctx_cb();
	vgui_context* host = vgui_create(nullptr, 16.0f);
	vgui_context* panel = vgui_create(nullptr, 16.0f);
	check(cb != nullptr, "create ovg callback");
	check(host != nullptr, "create host context");
	check(panel != nullptr, "create panel context");
	if (!cb || !host || !panel)
	{
		if (host) vgui_destroy(host);
		if (panel) vgui_destroy(panel);
		if (cb) free_ctx_cb(cb);
		shutdown_gpu(gpu);
		return 1;
	}

	vgui_context_set_size(host, 64, 48);
	vgui_context_set_size(panel, 64, 48);
	vgui_widget_set_background(vgui_root(panel), 0xFF00FF00u);

	check(vgui_render_target_create(host, gpu.vgpu_ctx, cb, nullptr) == nullptr,
		"create with NULL desc fails");
	check(std::strlen(vgui_render_target_last_error(nullptr)) > 0,
		"create failure is reported");

	vgui_render_target_desc desc;
	vgui_render_target_desc_default(&desc);
	desc.width = 64;
	desc.height = 48;

	vgui_render_target* raw = vgui_render_target_create(host, gpu.vgpu_ctx, cb, &desc);
	check(raw != nullptr, "create raw target");
	if (!raw)
	{
		vgui_destroy(panel);
		vgui_destroy(host);
		free_ctx_cb(cb);
		shutdown_gpu(gpu);
		return 1;
	}

	check(std::strlen(vgui_render_target_last_error(raw)) == 0,
		"no error after create");
	check(vgui_render_target_texture(raw) != nullptr, "target exposes texture");

	int width = 0;
	int height = 0;
	vgui_render_target_get_size(raw, &width, &height);
	check(width == 64 && height == 48, "target size");

	ovg_draw_data_t pass = {};
	check(vgui_render_target_record(raw, draw_full_red, nullptr, &pass) == 1,
		"record raw vector");
	std::vector<uint32_t> pixels;
	check(submit_and_download(gpu, &pass, 1, raw, pixels), "download raw target");
	expect_pixel(pixels, width, height, 32, 24, rgba(255, 0, 0),
		"raw vector red", &failures);

	vgui_render_target* content = vgui_render_target_create(host, gpu.vgpu_ctx, cb, &desc);
	vgui_render_target* composite = vgui_render_target_create(host, gpu.vgpu_ctx, cb, &desc);
	check(content != nullptr && composite != nullptr, "create context targets");
	if (!content || !composite)
	{
		vgui_render_target_destroy(content);
		vgui_render_target_destroy(composite);
		vgui_render_target_destroy(raw);
		vgui_destroy(panel);
		vgui_destroy(host);
		free_ctx_cb(cb);
		shutdown_gpu(gpu);
		return 1;
	}

	ovg_draw_data_t content_pass = {};
	check(vgui_render_target_record_context(content, panel, &content_pass) == 1,
		"record panel context");

	vgui_widget* image = vgui_widget_add(vgui_root(host), VGUI_IMAGE, nullptr);
	check(image != nullptr, "create image widget");
	if (!image)
	{
		vgui_render_target_destroy(composite);
		vgui_render_target_destroy(content);
		vgui_render_target_destroy(raw);
		vgui_destroy(panel);
		vgui_destroy(host);
		free_ctx_cb(cb);
		shutdown_gpu(gpu);
		return 1;
	}
	vgui_widget_set_pos(image, 0.0f, 0.0f);
	vgui_widget_set_size(image, 64.0f, 48.0f);
	vgui_widget_set_background(image, 0);
	vgui_widget_set_border(image, 0, 0.0f);
	vgui_image_set_fit(image, VGUI_IMAGE_FIT_STRETCH);
	vgui_image_set_texture(image, vgui_render_target_texture(content));

	ovg_draw_data_t composite_pass = {};
	check(vgui_render_target_record_context(composite, host, &composite_pass) == 1,
		"record host context");
	ovg_draw_data_t passes[2] = { content_pass, composite_pass };
	check(submit_and_download(gpu, passes, 2, composite, pixels),
		"download composite target");
	expect_pixel(pixels, 64, 48, 32, 24, rgba(0, 255, 0),
		"texture sampled in UI", &failures);

	vgui_texture* content_texture = vgui_render_target_texture(content);
	check(vgui_render_target_resize(content, 32, 24) == 1, "resize target");
	check(vgui_render_target_texture(content) == content_texture,
		"texture pointer is stable");
	check(vgui_texture_get_width(content_texture) == 32 &&
		vgui_texture_get_height(content_texture) == 24,
		"texture size follows resize");

	check(vgui_render_target_record_context(content, panel, &content_pass) == 0,
		"context size mismatch is rejected");
	check(std::strstr(vgui_render_target_last_error(content), "mismatch") != nullptr,
		"size mismatch is described");

	vgui_context_set_size(panel, 32, 24);
	check(vgui_render_target_record_context(content, panel, &content_pass) == 1,
		"record after context resize");
	check(vgui_render_target_record_context(composite, host, &composite_pass) == 1,
		"record host after target resize");
	ovg_draw_data_t resized_passes[2] = { content_pass, composite_pass };
	check(submit_and_download(gpu, resized_passes, 2, composite, pixels),
		"download after resize");
	expect_pixel(pixels, 64, 48, 32, 24, rgba(0, 255, 0),
		"resized texture sampled in UI", &failures);

	vgui_render_target_desc keep_desc = desc;
	keep_desc.width = 16;
	keep_desc.height = 16;
	keep_desc.clear = 0;
	vgui_render_target* keep = vgui_render_target_create(host, gpu.vgpu_ctx, cb, &keep_desc);
	check(keep != nullptr, "create clear=0 target");

	const rect_spec red_rect = { 0.0f, 0.0f, 8.0f, 8.0f, rgba(255, 0, 0) };
	const rect_spec blue_rect = { 8.0f, 8.0f, 8.0f, 8.0f, rgba(0, 0, 255) };
	check(vgui_render_target_submit(keep, draw_rect, (void*)&red_rect) == 1,
		"submit red rect");
	vgpu_wait_idle(gpu.vgpu_ctx);
	check(vgui_render_target_submit(keep, draw_rect, (void*)&blue_rect) == 1,
		"submit blue rect");
	vgpu_wait_idle(gpu.vgpu_ctx);

	int keep_width = 0;
	int keep_height = 0;
	vgui_render_target_get_size(keep, &keep_width, &keep_height);
	check(download_texture(gpu.device,
		vgui_render_target_native_texture(keep), keep_width, keep_height, pixels),
		"download clear=0 target");
	expect_pixel(pixels, keep_width, keep_height, 2, 2, rgba(255, 0, 0),
		"clear=0 preserves old content", &failures);
	expect_pixel(pixels, keep_width, keep_height, 10, 10, rgba(0, 0, 255),
		"clear=0 draws new content", &failures);

	// Release the widget binding before the target that owns the native texture.
	vgui_image_set_texture(image, nullptr);
	vgui_render_target_destroy(keep);
	vgui_render_target_destroy(composite);
	vgui_render_target_destroy(content);
	vgui_render_target_destroy(raw);

	// Contexts outlive the targets so their draw_cb is still the shared cb.
	vgui_destroy(panel);
	vgui_destroy(host);
	free_ctx_cb(cb);
	shutdown_gpu(gpu);

	std::printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
	return failures ? 1 : 0;
}
