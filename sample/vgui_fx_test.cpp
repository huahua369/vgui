// sample/vgui_fx_test.cpp
//
// vgui 后端特效管线（vg_effect_t）的离屏像素回归。
//
// OVG 自带渲染器一直有完整的特效实现，而 vgui 的精简后端（backends/
// vgui_renderer_sdl3.cpp）按设计砍掉了 compute，导致 set_effect 静默失效。
// 补齐后这条链路是：mask 写入 R8 → compute 两趟模糊到 R16F → 主 pass 合成，
// 本测试直接对着像素验证它真的出图，而不是只验证"没崩"：
//
//   - 带 OUTER_GLOW 的实心矩形：矩形外 8px 处必须比背景亮（有辉光）
//   - 同一位置如果不带特效，8px 外必须就是背景色（没有辉光泄漏）
//   - 清除特效后紧接着画的矩形不能再带辉光（effect 是粘滞状态，必须清干净）
//   - 远离所有图形的角落必须还是背景色（ROI 之外不能被污染）
//
// 硬件不支持（R8/RGBA8 不能做 color target，或 R16F 不能做 compute 写）时
// 打印 SKIP 并返回 77，与仓库里其它 GPU 测试一致。

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#include <glm/glm.hpp>

#include "gpu_test_util.h"
#include "ovg.h"
#include "ovg_fonts.h"

#include <vgui/vgui.h>
#include <vgui/backend/vgui_renderer_sdl3.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

constexpr int kWidth = 220;
constexpr int kHeight = 160;
constexpr int kSkipReturnCode = 77;

struct test_context {
	SDL_GPUDevice* device = nullptr;
	vgpu_device_t* vgpu_device = nullptr;
	vgpu_ctx_t* vgpu_ctx = nullptr;
	vg_fbo_t fbo = {};
	ovg_ctx_cb* cb = nullptr;
	rvg_t* vg = nullptr;
	std::vector<uint32_t> pixels;
	int checks = 0;
	int failures = 0;
};

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const char* what)
{
	++g_checks;
	if (ok)
		std::printf("PASS %s\n", what);
	else
	{
		std::printf("FAIL %s\n", what);
		++g_failures;
	}
}

// 读回像素是 0xAABBGGRR（RGBA8 小端），这里只关心通道值和亮度。
int channel(uint32_t px, int index)
{
	return (int)((px >> (index * 8)) & 0xFFu);
}

bool near_gray(uint32_t px, int value, int tolerance)
{
	for (int i = 0; i < 3; ++i)
	{
		if (channel(px, i) < value - tolerance || channel(px, i) > value + tolerance)
			return false;
	}
	return true;
}

uint32_t at(const test_context& t, int x, int y)
{
	if (x < 0 || y < 0 || x >= kWidth || y >= kHeight)
		return 0xFFFFFFFFu;
	return t.pixels[(size_t)y * kWidth + x];
}

// 场景里三块不透明白矩形：
//   control  (20, 20, 30, 20)   从不带特效
//   glow     (90, 60, 40, 30)   带 OUTER_GLOW（sigma 4，spread 1）
//   after    (20, 110, 20, 20)  紧跟在 clear_effect() 之后画
constexpr float kControlX = 20.0f, kControlY = 20.0f, kControlW = 30.0f, kControlH = 20.0f;
constexpr float kGlowX = 90.0f, kGlowY = 60.0f, kGlowW = 40.0f, kGlowH = 30.0f;
constexpr float kAfterX = 20.0f, kAfterY = 110.0f, kAfterW = 20.0f, kAfterH = 20.0f;

void draw_scene(vgui_widget* w, ovg_ctx_cb* cb, rvg_t* vg,
	float x, float y, float ww, float hh, float dt, void* user)
{
	(void)w;
	(void)dt;
	(void)user;
	(void)x;
	(void)y;
	(void)ww;
	(void)hh;

	// 背景先铺满，保证"背景色"是可预期的纯黑。
	cb->set_source_color(vg, 0xFF000000u);
	cb->rectangle(vg, 0.0f, 0.0f, (float)kWidth, (float)kHeight);
	cb->fill(vg);

	// 对照组：没有特效。
	cb->set_source_color(vg, 0xFFFFFFFFu);
	cb->rectangle(vg, kControlX, kControlY, kControlW, kControlH);
	cb->fill(vg);

	// 带外发光。遮罩取的是几何自身的 alpha，所以必须实心色。
	// 1px 级别的细线覆盖度太低，这里的 spread 用来放大合成强度。
	vg_effect_t e = {};
	e.type = VG_EFFECT_OUTER_GLOW;
	e.color = 0xFFFFFFFFu;
	e.sigma = 4.0f;
	e.spread = 1.0f;
	e.opacity = 1.0f;
	e.offset = glm::vec2(0.0f, 0.0f);
	cb->set_effect(vg, &e);
	cb->set_source_color(vg, 0xFFFFFFFFu);
	cb->rectangle(vg, kGlowX, kGlowY, kGlowW, kGlowH);
	cb->fill(vg);
	cb->set_effect(vg, nullptr);

	// 特效清除后必须立刻恢复正常：这块周围不能有光。
	cb->set_source_color(vg, 0xFFFFFFFFu);
	cb->rectangle(vg, kAfterX, kAfterY, kAfterW, kAfterH);
	cb->fill(vg);
}

bool init_renderer(test_context& t)
{
	if (!SDL_Init(SDL_INIT_VIDEO))
	{
		std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
		return false;
	}
	t.device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, true, "vulkan");
	if (!t.device)
	{
		std::fprintf(stderr, "SKIP: SDL_CreateGPUDevice(Vulkan) failed: %s\n",
			SDL_GetError());
		return false;
	}
	t.vgpu_device = vgpu_create_device(t.device);
	if (!t.vgpu_device)
	{
		std::fprintf(stderr, "vgpu_create_device failed\n");
		return false;
	}
	// 与产品一致用 MSAA 8：合成管线是按 FBO 的采样数选的，这里顺带覆盖该分支。
	t.vgpu_ctx = vgpu_create_context(t.vgpu_device,
		SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
		SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT,
		SDL_GPU_SAMPLECOUNT_8);
	if (!t.vgpu_ctx)
	{
		std::fprintf(stderr, "vgpu_create_context failed\n");
		return false;
	}
	t.fbo = vgpu_create_fbo(t.vgpu_ctx, kWidth, kHeight, nullptr);
	if (!t.fbo.ctx)
	{
		std::fprintf(stderr, "vgpu_create_fbo failed\n");
		return false;
	}
	t.cb = new_ctx_cb();
	t.vg = t.cb ? t.cb->new_rvg(t.cb->ac) : nullptr;
	if (!t.cb || !t.vg)
	{
		std::fprintf(stderr, "new_ctx_cb/new_rvg failed\n");
		return false;
	}
	t.vg->width = kWidth;
	t.vg->height = kHeight;
	return true;
}

void shutdown_renderer(test_context& t)
{
	if (t.vg && t.cb)
		t.cb->destroy_rvg(t.vg);
	if (t.cb)
		free_ctx_cb(t.cb);
	t.vg = nullptr;
	t.cb = nullptr;
	if (t.vgpu_ctx)
	{
		vgpu_destroy_fbo(&t.fbo);
		vgpu_destroy_context(t.vgpu_ctx);
	}
	if (t.vgpu_device)
		vgpu_destroy_device(t.vgpu_device);
	if (t.device)
		SDL_DestroyGPUDevice(t.device);
	t.vgpu_ctx = nullptr;
	t.vgpu_device = nullptr;
	t.device = nullptr;
	SDL_Quit();
}

bool init_font(font_cache_cx*& cache, font_familys_t*& family)
{
	cache = new_font_cache();
	if (!cache)
		return false;
	const std::string path =
		std::string(VGUI_SOURCE_DIR) + "/sample/res/fonts/DejaVuSans.ttf";
	if (!cache->load_font_from_path(path.c_str(), nullptr))
		std::fprintf(stderr, "warning: could not load %s\n", path.c_str());
	cache->get_sys_family();
	family = new_font_family(cache, "DejaVu Sans", nullptr);
	return family && family->count > 0;
}

bool render_frame(test_context& t, vgui_context* ui)
{
	// 两趟：第一趟可能填充字形图集，第二趟采样完成后的图集。
	for (int pass = 0; pass < 2; ++pass)
	{
		t.cb->clear(t.vg);
		t.cb->reset_clip(t.vg, 1);
		vgui_draw(ui, t.cb, t.vg);

		ovg_draw_data_t data = get_draw_list(t.vg);
		data.fbo = &t.fbo;
		vgpu_render_frame(t.vgpu_ctx, &data, 1);
		vgpu_wait_idle(t.vgpu_ctx);
	}
	return download_texture(t.device, vgpu_fbo_color_texture(&t.fbo),
		kWidth, kHeight, t.pixels);
}

} // namespace

int main()
{
	test_context t;
	if (!init_renderer(t))
	{
		shutdown_renderer(t);
		return kSkipReturnCode;
	}

	if (!vgpu_effects_supported(t.vgpu_ctx))
	{
		std::printf("SKIP: this device lacks the formats the effect pipeline needs\n");
		shutdown_renderer(t);
		return kSkipReturnCode;
	}

	font_cache_cx* cache = nullptr;
	font_familys_t* family = nullptr;
	if (!init_font(cache, family))
	{
		std::fprintf(stderr, "font init failed\n");
		shutdown_renderer(t);
		return 1;
	}

	vgui_context* ui = vgui_create(family, 15.0f);
	if (!ui)
	{
		std::fprintf(stderr, "vgui_create failed\n");
		shutdown_renderer(t);
		return 1;
	}
	vgui_context_set_size(ui, kWidth, kHeight);
	vgui_context_set_ui_scale(ui, 1.0f);

	vgui_widget* canvas = vgui_widget_add(vgui_root(ui), VGUI_CUSTOM, nullptr);
	vgui_widget_set_background(canvas, 0);
	vgui_widget_set_border(canvas, 0, 0);
	vgui_widget_set_size(canvas, (float)kWidth, (float)kHeight);
	vgui_custom_set_draw(canvas, draw_scene, nullptr);
	vgui_update(ui, 1.0f / 60.0f);
	vgui_layout(ui);

	const bool ok = render_frame(t, ui);
	check(ok, "download framebuffer");
	if (ok)
	{
		// 1) 对照组：矩形本体白，外侧就是背景。
		check(near_gray(at(t, 35, 30), 255, 6), "control rect body is white");
		check(near_gray(at(t, (int)kControlX + (int)kControlW + 8, 30), 0, 6),
			"control rect has no glow");

		// 2) 带特效的矩形：本体白，外侧 8px 必须被辉光提亮。
		check(near_gray(at(t, 110, 75), 255, 6), "glow rect body is white");
		const uint32_t just_outside = at(t, (int)kGlowX + (int)kGlowW + 8, 75);
		check(channel(just_outside, 0) > 12,
			"glow rect brightens 8px outside its edge");
		// 越远越暗：ROI 边缘附近应当已经衰减到接近背景。
		const uint32_t far_corner = at(t, 205, 15);
		check(near_gray(far_corner, 0, 6), "far corner stays background");

		// 3) 清除特效后画的那块不能再有光（effect 是粘滞状态）。
		check(near_gray(at(t, 30, 120), 255, 6), "cleared rect body is white");
		check(near_gray(at(t, (int)kAfterX + (int)kAfterW + 8, 120), 0, 6),
			"cleared rect has no glow");
	}

	vgui_destroy(ui);
	if (cache)
		free_font_cache(cache);
	shutdown_renderer(t);

	std::printf("vgui_fx_test: %d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
