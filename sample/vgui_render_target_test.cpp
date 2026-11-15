// sample/vgui_render_target_test.cpp
//
// Offscreen test for vgui_render_target:
//   vector -> target texture -> VGUI_IMAGE -> another target -> readback.
// It also covers context recording, resize-in-place, clear=0 preservation, the
// static submit() convenience and the optional depth attachment (the UI pass
// output is pixel-identical with and without depth; the depth handle survives
// resize).  No window is created; when no Vulkan device is available the test
// returns 77 so CTest marks it as skipped.

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#include <cmath>
#include <cstdint>
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

// IEEE-754 binary16 -> float (enough for test assertions: normals and zeros).
float half_to_float(uint16_t h)
{
	const uint32_t sign = static_cast<uint32_t>(h & 0x8000u) << 16;
	const uint32_t exp = (h >> 10) & 0x1Fu;
	const uint32_t man = h & 0x3FFu;
	if (exp == 0)
	{
		const uint32_t zero = sign;
		float out = 0.0f;
		std::memcpy(&out, &zero, sizeof(out));
		return out;
	}
	if (exp == 0x1Fu)
	{
		const uint32_t inf = sign | 0x7F800000u | (man << 13);
		float out = 0.0f;
		std::memcpy(&out, &inf, sizeof(out));
		return out;
	}
	const uint32_t bits = sign | ((exp - 15u + 127u) << 23) | (man << 13);
	float out = 0.0f;
	std::memcpy(&out, &bits, sizeof(out));
	return out;
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

	// 上下文重建(set_msaa_samples 场景):rebuild_all 把 raw 换绑到新 ctx,
	// 纹理指针不变,重录 + 新 ctx 提交 + 回读仍红;再换绑回旧 ctx。
	{
		vgui_texture* raw_texture = vgui_render_target_texture(raw);
		vgpu_ctx_t* ctx2 = vgpu_create_context(gpu.vgpu_device,
			SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
			SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT,
			SDL_GPU_SAMPLECOUNT_1);
		check(ctx2 != nullptr, "create second gpu context");
		if (ctx2)
		{
			check(vgui_render_target_rebuild_all(gpu.vgpu_ctx, ctx2) == 1,
				"rebuild_all rebinds every target of the old context");
			check(vgui_render_target_texture(raw) == raw_texture,
				"texture pointer survives a context rebuild");

			ovg_draw_data_t ctx2_pass = {};
			check(vgui_render_target_record(raw, draw_full_red, nullptr,
				&ctx2_pass) == 1, "record on the rebuilt target");
			vgpu_render_frame(ctx2, &ctx2_pass, 1);
			vgpu_wait_idle(ctx2);
			std::vector<uint32_t> ctx2_pixels;
			check(download_texture(gpu.device,
				vgui_render_target_native_texture(raw), width, height,
				ctx2_pixels), "download from the rebuilt target");
			expect_pixel(ctx2_pixels, width, height, 32, 24, rgba(255, 0, 0),
				"rebuilt target renders UI", &failures);
			check(vgui_render_target_rebuild(raw, ctx2) == 1,
				"same-context rebuild is a no-op");
			check(vgui_render_target_rebuild(raw, gpu.vgpu_ctx) == 1,
				"rebuild back to the original context");
			vgpu_destroy_context(ctx2);
			ovg_draw_data_t back_pass = {};
			check(vgui_render_target_record(raw, draw_full_red, nullptr,
				&back_pass) == 1, "target usable after ctx2 teardown");
		}
	}

	// 异步 resize:交替尺寸连续 10 轮 resize → 立即 record → submit →
	// download。旧 FBO 由 fence 退休队列守护,不等待 GPU idle,新内容不受
	// 在飞工作影响。
	{
		bool stress_ok = true;
		const int sizes[2][2] = { { 64, 48 }, { 32, 24 } };
		for (int i = 0; i < 10 && stress_ok; ++i)
		{
			const int w = sizes[i % 2][0];
			const int h = sizes[i % 2][1];
			if (vgui_render_target_resize(raw, w, h) != 1)
			{
				stress_ok = false;
				break;
			}
			ovg_draw_data_t stress_pass = {};
			if (vgui_render_target_record(raw, draw_full_red, nullptr,
				&stress_pass) != 1)
			{
				stress_ok = false;
				break;
			}
			vgpu_render_frame(gpu.vgpu_ctx, &stress_pass, 1);
			vgpu_wait_idle(gpu.vgpu_ctx);
			std::vector<uint32_t> stress_pixels;
			if (!download_texture(gpu.device,
				vgui_render_target_native_texture(raw), w, h, stress_pixels))
			{
				stress_ok = false;
				break;
			}
			if (stress_pixels[(size_t)(h / 2) * w + w / 2] != rgba(255, 0, 0))
				stress_ok = false;
		}
		check(stress_ok, "async resize stress: 10 rounds correct output");
		// 恢复到原始尺寸并重绘,后续断言以 64x48 红色内容为前提。
		if (vgui_render_target_resize(raw, width, height) == 1)
		{
			ovg_draw_data_t restore_pass = {};
			vgui_render_target_record(raw, draw_full_red, nullptr,
				&restore_pass);
			vgpu_render_frame(gpu.vgpu_ctx, &restore_pass, 1);
			vgpu_wait_idle(gpu.vgpu_ctx);
		}
	}

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

	// 深度附件:desc.depth=1 创建 D32 深度句柄;UI pass 不绑深度,同一份内容
	// 在"有深度/无深度"两个目标上的颜色输出必须逐像素一致。
	check(desc.depth == 0, "desc default has no depth");
	check(vgui_render_target_native_depth_texture(raw) == nullptr,
		"no depth handle without desc.depth");
	vgui_render_target_desc depth_desc = desc;
	depth_desc.depth = 1;
	vgui_render_target* depth_target = vgui_render_target_create(host,
		gpu.vgpu_ctx, cb, &depth_desc);
	check(depth_target != nullptr, "create depth=1 target");
	if (depth_target)
	{
		check(vgui_render_target_native_depth_texture(depth_target) != nullptr,
			"depth handle exists");

		vgui_context* depth_panel = vgui_create(nullptr, 16.0f);
		check(depth_panel != nullptr, "create depth panel context");
		if (depth_panel)
		{
			vgui_context_set_size(depth_panel, 64, 48);
			vgui_widget_set_background(vgui_root(depth_panel), 0xFF0000FFu);

			vgui_render_target* plain = vgui_render_target_create(host,
				gpu.vgpu_ctx, cb, &desc);
			check(plain != nullptr, "create plain twin target");
			if (plain)
			{
				ovg_draw_data_t depth_pass = {};
				ovg_draw_data_t plain_pass = {};
				check(vgui_render_target_record_context(depth_target,
					depth_panel, &depth_pass) == 1, "record into depth target");
				check(vgui_render_target_record_context(plain, depth_panel,
					&plain_pass) == 1, "record into plain twin");
				vgpu_render_frame(gpu.vgpu_ctx, &depth_pass, 1);
				vgpu_render_frame(gpu.vgpu_ctx, &plain_pass, 1);
				vgpu_wait_idle(gpu.vgpu_ctx);

				std::vector<uint32_t> depth_pixels;
				std::vector<uint32_t> plain_pixels;
				check(download_texture(gpu.device,
					vgui_render_target_native_texture(depth_target), width,
					height, depth_pixels), "download depth target");
				check(download_texture(gpu.device,
					vgui_render_target_native_texture(plain), width, height,
					plain_pixels), "download plain twin");
				check(depth_pixels.size() == plain_pixels.size() &&
					std::memcmp(depth_pixels.data(), plain_pixels.data(),
						depth_pixels.size() * sizeof(uint32_t)) == 0,
					"UI output identical with and without depth");

				// resize 后深度句柄仍有效,颜色输出可继续读回。
				vgui_texture* kept_texture =
					vgui_render_target_texture(depth_target);
				check(vgui_render_target_resize(depth_target, 32, 24) == 1,
					"resize depth target");
				check(vgui_render_target_texture(depth_target) == kept_texture,
					"depth target texture pointer is stable");
				check(vgui_render_target_native_depth_texture(depth_target) !=
					nullptr, "depth handle survives resize");
				vgui_context_set_size(depth_panel, 32, 24);
				ovg_draw_data_t resized_depth_pass = {};
				check(vgui_render_target_record_context(depth_target,
					depth_panel, &resized_depth_pass) == 1,
					"record into resized depth target");
				vgpu_render_frame(gpu.vgpu_ctx, &resized_depth_pass, 1);
				vgpu_wait_idle(gpu.vgpu_ctx);
				int rw = 0, rh = 0;
				vgui_render_target_get_size(depth_target, &rw, &rh);
				std::vector<uint32_t> resized_pixels;
				check(download_texture(gpu.device,
					vgui_render_target_native_texture(depth_target), rw, rh,
					resized_pixels), "download resized depth target");
				expect_pixel(resized_pixels, rw, rh, 2, 2, rgba(0, 0, 255),
					"resized depth target renders UI", &failures);
				vgui_render_target_destroy(plain);
			}
			vgui_destroy(depth_panel);
		}
		vgui_render_target_destroy(depth_target);
	}

	// 显式格式(HDR):desc.format = RGBA16F 的目标,pipeline 按格式惰性创建,
	// UI 输出以 fp16 读回并解码断言;非法格式在 create 时被拒绝。
	{
		const auto nearf = [](float a, float b)
		{
			return std::fabs(a - b) <= 1.0e-4f;
		};
		vgui_render_target_desc hdr_desc = desc;
		hdr_desc.format = VGUI_TEXTURE_FORMAT_RGBA16F;
		vgui_render_target* hdr = vgui_render_target_create(host, gpu.vgpu_ctx,
			cb, &hdr_desc);
		check(hdr != nullptr, "create RGBA16F target");
		if (hdr)
		{
			vgui_context* hdr_panel = vgui_create(nullptr, 16.0f);
			check(hdr_panel != nullptr, "create hdr panel context");
			if (hdr_panel)
			{
				vgui_context_set_size(hdr_panel, 64, 48);
				vgui_widget_set_background(vgui_root(hdr_panel), 0xFFFF0000u);
				ovg_draw_data_t hdr_pass = {};
				check(vgui_render_target_record_context(hdr, hdr_panel,
					&hdr_pass) == 1, "record into RGBA16F target");
				vgpu_render_frame(gpu.vgpu_ctx, &hdr_pass, 1);
				vgpu_wait_idle(gpu.vgpu_ctx);

				std::vector<uint8_t> hdr_bytes;
				check(download_texture_bytes(gpu.device,
					vgui_render_target_native_texture(hdr), width, height, 8,
					hdr_bytes), "download RGBA16F target");
				const auto sample_half = [&](int x, int y, int ch) -> float
				{
					const size_t base =
						((size_t)y * width + (size_t)x) * 8u + (size_t)ch * 2u;
					uint16_t h16 = 0;
					if (base + 2 <= hdr_bytes.size())
						std::memcpy(&h16, hdr_bytes.data() + base, sizeof(h16));
					return half_to_float(h16);
				};
				check(nearf(sample_half(32, 24, 0), 1.0f) &&
					nearf(sample_half(32, 24, 1), 0.0f) &&
					nearf(sample_half(32, 24, 2), 0.0f) &&
					nearf(sample_half(32, 24, 3), 1.0f),
					"RGBA16F UI output is float HDR");

				check(vgui_render_target_resize(hdr, 32, 24) == 1,
					"resize RGBA16F target");
				vgui_context_set_size(hdr_panel, 32, 24);
				ovg_draw_data_t hdr_pass2 = {};
				check(vgui_render_target_record_context(hdr, hdr_panel,
					&hdr_pass2) == 1, "record into resized RGBA16F target");
				vgpu_render_frame(gpu.vgpu_ctx, &hdr_pass2, 1);
				vgpu_wait_idle(gpu.vgpu_ctx);
				int rw = 0, rh = 0;
				vgui_render_target_get_size(hdr, &rw, &rh);
				std::vector<uint8_t> hdr_bytes2;
				check(download_texture_bytes(gpu.device,
					vgui_render_target_native_texture(hdr), rw, rh, 8,
					hdr_bytes2), "download resized RGBA16F target");
				const auto sample16 = [&](int x, int y, int ch) -> float
				{
					const size_t base =
						((size_t)y * rw + (size_t)x) * 8u + (size_t)ch * 2u;
					uint16_t h16 = 0;
					if (base + 2 <= hdr_bytes2.size())
						std::memcpy(&h16, hdr_bytes2.data() + base, sizeof(h16));
					return half_to_float(h16);
				};
				check(nearf(sample16(16, 12, 0), 1.0f) &&
					nearf(sample16(16, 12, 1), 0.0f) &&
					nearf(sample16(16, 12, 2), 0.0f) &&
					nearf(sample16(16, 12, 3), 1.0f),
					"resized RGBA16F target renders UI");

				/* 公共回读 API:字节数、内容与测试基建互验,短缓冲被拒。 */
				check(vgui_render_target_bytes_per_pixel(hdr) == 8,
					"bytes per pixel reports 8 for RGBA16F");
				const size_t want_bytes = (size_t)rw * (size_t)rh * 8u;
				std::vector<uint8_t> public_bytes(want_bytes + 8, 0xAB);
				const size_t wrote = vgui_render_target_download(hdr,
					public_bytes.data(), want_bytes + 8);
				check(wrote == want_bytes,
					"public download writes exactly w*h*bpp bytes");
				check(public_bytes[want_bytes] == 0xAB,
					"public download does not write past the payload");
				check(wrote == want_bytes &&
					std::memcmp(public_bytes.data(), hdr_bytes2.data(),
						want_bytes) == 0,
					"public download matches the test helper");
				check(vgui_render_target_download(hdr, public_bytes.data(),
					want_bytes - 1) == 0, "download rejects a short buffer");
				vgui_destroy(hdr_panel);
			}
			vgui_render_target_destroy(hdr);
		}

		vgui_render_target_desc bad_desc = desc;
		bad_desc.format = 0x7FFFFFFF;
		vgui_render_target* bad = vgui_render_target_create(host, gpu.vgpu_ctx,
			cb, &bad_desc);
		check(bad == nullptr, "create with invalid format fails");
		check(std::strstr(vgui_render_target_last_error(nullptr), "format") !=
			nullptr, "invalid format is described");
	}

	// MSAA 采样数选择:ctx 为 1x,目标可显式请求 4x(resolve 后输出与 1x
	// 逐像素一致)、显式 1x、非法值拒绝,以及 4x + 深度组合。
	{
		vgui_render_target_desc msaa_desc = desc;
		msaa_desc.sample_count = 4;
		vgui_render_target* msaa = vgui_render_target_create(host, gpu.vgpu_ctx,
			cb, &msaa_desc);
		check(msaa != nullptr, "create 4x target on 1x context");
		if (msaa)
		{
			vgui_context* msaa_panel = vgui_create(nullptr, 16.0f);
			check(msaa_panel != nullptr, "create msaa panel context");
			if (msaa_panel)
			{
				vgui_context_set_size(msaa_panel, 64, 48);
				vgui_widget_set_background(vgui_root(msaa_panel), 0xFFFF0000u);

				vgui_render_target* plain = vgui_render_target_create(host,
					gpu.vgpu_ctx, cb, &desc);
				check(plain != nullptr, "create 1x twin target");
				if (plain)
				{
					ovg_draw_data_t msaa_pass = {};
					ovg_draw_data_t plain_pass = {};
					check(vgui_render_target_record_context(msaa, msaa_panel,
						&msaa_pass) == 1, "record into 4x target");
					check(vgui_render_target_record_context(plain, msaa_panel,
						&plain_pass) == 1, "record into 1x twin");
					vgpu_render_frame(gpu.vgpu_ctx, &msaa_pass, 1);
					vgpu_render_frame(gpu.vgpu_ctx, &plain_pass, 1);
					vgpu_wait_idle(gpu.vgpu_ctx);

					std::vector<uint32_t> msaa_pixels;
					std::vector<uint32_t> plain_pixels;
					check(download_texture(gpu.device,
						vgui_render_target_native_texture(msaa), width, height,
						msaa_pixels), "download 4x target");
					check(download_texture(gpu.device,
						vgui_render_target_native_texture(plain), width, height,
						plain_pixels), "download 1x twin");
					check(msaa_pixels.size() == plain_pixels.size() &&
						std::memcmp(msaa_pixels.data(), plain_pixels.data(),
							msaa_pixels.size() * sizeof(uint32_t)) == 0,
						"4x resolve output matches 1x output");
					vgui_render_target_destroy(plain);
				}

				// 4x + 深度组合:深度附件按目标采样数创建。深度目标的采样数
				// 支持不是 Vulkan 的强制保证(只保证颜色目标 4x),按设备能力
				// 分支断言:支持时必须成功,不支持时必须干净失败并给出原因。
				vgui_render_target_desc msaa_depth_desc = desc;
				msaa_depth_desc.sample_count = 4;
				msaa_depth_desc.depth = 1;
				vgui_render_target* msaa_depth = vgui_render_target_create(host,
					gpu.vgpu_ctx, cb, &msaa_depth_desc);
				const bool depth4x = SDL_GPUTextureSupportsSampleCount(
					gpu.device, SDL_GPU_TEXTUREFORMAT_D32_FLOAT,
					SDL_GPU_SAMPLECOUNT_4);
				if (depth4x)
				{
					check(msaa_depth != nullptr, "create 4x + depth target");
					check(msaa_depth != nullptr &&
						vgui_render_target_native_depth_texture(msaa_depth) !=
							nullptr, "depth handle on 4x target");
				}
				else
				{
					check(msaa_depth == nullptr,
						"4x + depth fails cleanly when device lacks D32 4x");
					check(msaa_depth == nullptr && std::strstr(
						vgui_render_target_last_error(nullptr), "depth") !=
						nullptr, "4x depth failure is described");
				}
				vgui_render_target_destroy(msaa_depth);
				vgui_destroy(msaa_panel);
			}
			vgui_render_target_destroy(msaa);
		}

		// 显式禁 MSAA(ctx 本身 1x,输出应与默认一致)。
		vgui_render_target_desc no_msaa_desc = desc;
		no_msaa_desc.sample_count = 1;
		vgui_render_target* no_msaa = vgui_render_target_create(host,
			gpu.vgpu_ctx, cb, &no_msaa_desc);
		check(no_msaa != nullptr, "create explicit 1x target");
		vgui_render_target_destroy(no_msaa);

		// 非法采样数。
		vgui_render_target_desc bad_samples_desc = desc;
		bad_samples_desc.sample_count = 3;
		vgui_render_target* bad_samples = vgui_render_target_create(host,
			gpu.vgpu_ctx, cb, &bad_samples_desc);
		check(bad_samples == nullptr, "create with invalid sample count fails");
		check(std::strstr(vgui_render_target_last_error(nullptr),
			"sample count") != nullptr, "invalid sample count is described");
	}

	// 多目标链路:三个 target 依赖排序(每个 target 的 UI 采样上一个 target
	// 的纹理),三项 pass 放进同一次 vgpu_render_frame() 提交——"多目标渲染
	// 图"由既有批量提交模式覆盖,数组顺序即依赖顺序。
	{
		vgui_context* chain_ctx[3] = {};
		vgui_render_target* chain_target[3] = {};
		vgui_widget* chain_image[3] = {};
		const uint32_t chain_bg[3] = { 0xFFFF0000u, 0xFF00FF00u, 0xFF0000FFu };
		bool chain_ok = true;
		for (int i = 0; i < 3 && chain_ok; ++i)
		{
			chain_ctx[i] = vgui_create(nullptr, 16.0f);
			if (!chain_ctx[i])
			{
				std::printf("  chain step %d: create context failed\n", i);
				chain_ok = false;
				break;
			}
			vgui_context_set_size(chain_ctx[i], 64, 48);
			vgui_widget_set_background(vgui_root(chain_ctx[i]), chain_bg[i]);
			chain_target[i] = vgui_render_target_create(host, gpu.vgpu_ctx,
				cb, &desc);
			if (!chain_target[i])
			{
				std::printf("  chain step %d: create target failed: %s\n", i,
					vgui_render_target_last_error(nullptr));
				chain_ok = false;
			}
		}
		// 第 i+1 层的 UI 显示第 i 层的纹理:层 1 半幅(留出自身背景),
		// 层 2 全幅——末端同时可见"穿透两级的层 0 颜色"与"层 1 背景"。
		for (int i = 0; i + 1 < 3 && chain_ok; ++i)
		{
			chain_image[i] = vgui_widget_add(vgui_root(chain_ctx[i + 1]),
				VGUI_IMAGE, nullptr);
			if (!chain_image[i])
			{
				std::printf("  chain step %d: create image failed\n", i);
				chain_ok = false;
				break;
			}
			vgui_widget_set_pos(chain_image[i], 0.0f, 0.0f);
			vgui_widget_set_size(chain_image[i],
				i == 0 ? 32.0f : 64.0f, i == 0 ? 24.0f : 48.0f);
			vgui_widget_set_background(chain_image[i], 0);
			vgui_widget_set_border(chain_image[i], 0, 0.0f);
			vgui_image_set_fit(chain_image[i], VGUI_IMAGE_FIT_STRETCH);
		}
		check(chain_ok, "create 3-target chain");
		if (chain_ok)
		{
			// 先绑定纹理再录制:第 i+1 层引用第 i 层的 vgui_texture。
			for (int i = 0; i + 1 < 3; ++i)
				vgui_image_set_texture(chain_image[i],
					vgui_render_target_texture(chain_target[i]));

			ovg_draw_data_t chain_passes[3] = {};
			bool recorded = true;
			for (int i = 0; i < 3 && recorded; ++i)
				recorded = vgui_render_target_record_context(chain_target[i],
					chain_ctx[i], &chain_passes[i]) == 1;
			check(recorded, "record 3-target chain in dependency order");
			if (recorded)
			{
				vgpu_render_frame(gpu.vgpu_ctx, chain_passes, 3);
				vgpu_wait_idle(gpu.vgpu_ctx);
				std::vector<uint32_t> chain_pixels;
				check(download_texture(gpu.device,
					vgui_render_target_native_texture(chain_target[2]), width,
					height, chain_pixels), "download chain end target");
				// 末端 (8,6) ← 层1(16,12) ← 层0 红:穿透两级;(40,30) ←
				// 层1(40,30) 绿:层 1 自身背景(其图外区域)穿过一级。
				expect_pixel(chain_pixels, width, height, 8, 6, rgba(255, 0, 0),
					"chain carries layer 0 content", &failures);
				expect_pixel(chain_pixels, width, height, 40, 30, rgba(0, 255, 0),
					"chain carries layer 1 background", &failures);
			}
		}
		for (int i = 0; i < 3; ++i)
		{
			vgui_render_target_destroy(chain_target[i]);
			if (chain_ctx[i])
				vgui_destroy(chain_ctx[i]);
		}
	}

	// 公共回读 API(8-bit 路径):字节数与容量校验。
	{
		check(vgui_render_target_bytes_per_pixel(raw) == 4,
			"bytes per pixel reports 4 for RGBA8");
		const size_t want_bytes = (size_t)width * (size_t)height * 4u;
		std::vector<uint8_t> public_bytes(want_bytes, 0);
		check(vgui_render_target_download(raw, public_bytes.data(),
			want_bytes) == want_bytes, "public download writes RGBA8 payload");
		check(vgui_render_target_download(raw, nullptr, want_bytes) == 0,
			"download rejects a null buffer");
	}

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
