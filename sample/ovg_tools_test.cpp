// sample/ovg_tools_test.cpp
//
// Offscreen pixel test for ovg/tools/tools.hpp.
// Covers: basic shapes (rect/round4/circle/ring/star/pie/capsule), gradients,
// dashed strokes, glyphs, selection frame, timeline pieces (ruler/playhead/
// keyframe/lane/range bar), bezier curves + handles, color-picker widgets,
// shadow effect, fill-rule routing (compound paths / even-odd / bowtie /
// compound clip). Renders one 512x768 frame into an offscreen FBO, reads it
// back and samples pixels. Also dumps ovg_tools_test.png for eyeballing.
//
// Exits 0 when every sampled pixel matches (near comparisons use a tolerance
// for 8-bit gradient quantization).

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

// ovg_c.h / tools.hpp use glm types without including glm.
#include <glm/glm.hpp>

#include "ovg.h"
#include "ovg_renderer_sdl3.h"
#include "gpu_test_util.h"
#include "tools/tools.hpp"

#include <cmath>
#include <cstdio>
#include <vector>

// ovg_fonts.h 导出，libovg 内实现；直接 extern 避免引入字体头
extern "C++" bool write_png_bgra(const char* path, const uint8_t* bgra, int w, int h);

namespace ot = ovg_tools;

static int g_fail = 0;
static int g_checks = 0;

static const std::vector<uint32_t>* g_pixels = nullptr;
static int g_w = 0, g_h = 0;

static uint32_t px(int x, int y)
{
	return (*g_pixels)[(size_t)y * g_w + x];
}
static void expect_pixel(int x, int y, uint32_t expected, const char* label)
{
	++g_checks;
	if (x < 0 || y < 0 || x >= g_w || y >= g_h)
	{
		std::printf("FAIL %-28s out of range (%d,%d)\n", label, x, y);
		++g_fail;
		return;
	}
	uint32_t actual = px(x, y);
	if (actual != expected)
	{
		std::printf("FAIL %-28s at (%d,%d): got 0x%08X expected 0x%08X\n",
			label, x, y, actual, expected);
		++g_fail;
	}
}
// 每通道容差比较（渐变量化/AA 用）
static void expect_near(int x, int y, uint32_t expected, int tol, const char* label)
{
	++g_checks;
	if (x < 0 || y < 0 || x >= g_w || y >= g_h)
	{
		std::printf("FAIL %-28s out of range (%d,%d)\n", label, x, y);
		++g_fail;
		return;
	}
	uint32_t a = px(x, y);
	for (int i = 0; i < 4; ++i)
	{
		int shift = i * 8;
		int d = (int)((a >> shift) & 0xFF) - (int)((expected >> shift) & 0xFF);
		if (d < -tol || d > tol)
		{
			std::printf("FAIL %-28s at (%d,%d): got 0x%08X expected ~0x%08X (tol %d)\n",
				label, x, y, a, expected, tol);
			++g_fail;
			return;
		}
	}
}
static void expect_not_pixel(int x, int y, uint32_t unexpected, const char* label)
{
	++g_checks;
	if (x < 0 || y < 0 || x >= g_w || y >= g_h)
	{
		std::printf("FAIL %-28s out of range (%d,%d)\n", label, x, y);
		++g_fail;
		return;
	}
	uint32_t a = px(x, y);
	if (a == unexpected)
	{
		std::printf("FAIL %-28s at (%d,%d): got 0x%08X (unexpected)\n", label, x, y, a);
		++g_fail;
	}
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

	const int W = 512, H = 768;
	ovg_device_t* ovg_device = new_sdl3gpu_device(device);
	ovg_ctx_t* ctx = new_ovgctx_sdl3(ovg_device,
		SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
		SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT,
		SDL_GPU_SAMPLECOUNT_1);
	if (!ctx)
	{
		std::fprintf(stderr, "new_ovgctx_sdl3 failed\n");
		return 2;
	}
	vg_fbo_t fbo = new_vgfbo_sdl3(ctx, W, H, nullptr);
	fbo.display_size = { W, H };
	fbo.upsize = false;
	ovg_ctx_cb* cb = new_ctx_cb();
	rvg_t* vg = cb->new_rvg(cb->ac);
	vg->width = W;
	vg->height = H;

	// ---- 调色板（ovg 原生 0xAABBGGRR） ----
	const uint32_t BG = ot::rgba(43, 43, 48);
	const uint32_t BLUE = ot::rgba(74, 134, 240);
	const uint32_t RED = ot::rgba(232, 72, 72);
	const uint32_t YELLOW = ot::rgba(242, 212, 80);
	const uint32_t GREEN = ot::rgba(92, 202, 112);
	const uint32_t GREEN2 = ot::rgba(70, 170, 110);
	const uint32_t ORANGE = ot::rgba(250, 150, 60);
	const uint32_t CYAN = ot::rgba(84, 204, 232);
	const uint32_t WHITE = ot::rgba(255, 255, 255);
	const uint32_t MAGENTA = ot::rgba(224, 72, 220);
	const uint32_t LIME = ot::rgba(150, 230, 110);
	const uint32_t KF = ot::rgba(210, 210, 214);
	const uint32_t KF_SEL = ot::rgba(74, 196, 250);
	const uint32_t RED2 = ot::rgba(240, 80, 80);
	const uint32_t HANDLE = ot::rgba(255, 170, 60);
	const uint32_t LANE0 = ot::rgba(58, 58, 64);
	const uint32_t LANE1 = ot::rgba(48, 48, 54);
	const uint32_t MINOR = ot::rgba(70, 70, 78);
	const uint32_t MAJOR = ot::rgba(120, 120, 130);
	const uint32_t GRAY_R = ot::rgba(160, 160, 168);
	const uint32_t GRID_MIN = ot::rgba(70, 72, 80);
	const uint32_t GRID_MAJ = ot::rgba(104, 108, 120);
	const uint32_t CHK0 = ot::rgba(216, 216, 216);
	const uint32_t CHK1 = ot::rgba(248, 248, 248);
	const uint32_t RED3 = ot::rgba(220, 50, 60);
	const uint32_t BLUE2 = ot::rgba(60, 140, 230);
	const uint32_t TOP = ot::rgba(255, 60, 60);
	const uint32_t BOT = ot::rgba(60, 60, 255);
	const uint32_t RED_AX = ot::rgba(235, 90, 90);
	const uint32_t GREEN_AX = ot::rgba(100, 215, 110);

	// ---- 录制一帧 ----
	cb->clear(vg);
	cb->reset_clip(vg, 1);
	ot::fill_rect(cb, vg, 0, 0, W, H, BG);

	// 基础形状行
	{
		const float radii[4] = { 24, 10, 0, 14 };
		ot::fill_round_rect4(cb, vg, 12, 12, 120, 110, radii, BLUE);
		ot::stroke_round_rect4(cb, vg, 140, 12, 0, 0, radii, WHITE); // 退化尺寸，仅验证不崩溃
	}
	ot::fill_ring(cb, vg, 196, 66, 44, 26, RED);
	ot::fill_star(cb, vg, 300, 66, 46, 18, 5, YELLOW);
	ot::fill_pie(cb, vg, 396, 66, 42, -ot::kPi * 0.5f, 0, GREEN);
	ot::stroke_arc(cb, vg, 462, 66, 34, -ot::kPi * 0.75f, -ot::kPi * 0.25f, ORANGE, 8.f);

	// 胶囊 / 渐变 / 棋盘 / 虚线矩形
	ot::fill_capsule(cb, vg, 12, 132, 120, 26, GREEN2);
	ot::fill_rect_gradient(cb, vg, 152, 132, 100, 40, TOP, BOT, false);
	ot::checkerboard(cb, vg, 264, 132, 64, 64, 8, CHK0, CHK1);
	ot::dashed_rect(cb, vg, 340, 132.5f, 80, 40, WHITE, 1.f, 8.f, 8.f);

	// 阴影特效（严格方向断言：offset (0,15)，正 y = 屏幕向下）
	ot::set_shadow(cb, vg, ot::rgba(0, 0, 0), 6.f, 0.f, 15.f);
	ot::fill_round_rect(cb, vg, 436, 132, 68, 50, 8, WHITE);
	ot::clear_effect(cb, vg);
	ot::fill_rect(cb, vg, 436, 132, 10, 10, ORANGE); // 特效关闭后恢复正常绘制

	// 箭头 / 符号
	ot::arrow(cb, vg, 12, 184, 128, 216, ORANGE, 1.5f, 10.f);
	ot::check(cb, vg, 160, 200, 12, LIME, 2.5f);
	ot::xmark(cb, vg, 196, 200, 10, CYAN, 2.5f);
	ot::chevron(cb, vg, 232, 200, 11, 0, WHITE, 2.5f);
	ot::tri_glyph(cb, vg, 268, 200, 10, 1, KF);
	ot::cross(cb, vg, 296, 200, 8, WHITE, 1.5f);

	// 选择框 + 旋转柄
	ot::selection_frame(cb, vg, 376.5f, 186.5f, 120, 60, CYAN, 6.f, WHITE, false);
	ot::rotate_handle(cb, vg, 436.5f, 186.5f, 18, 4, CYAN);

	// Timeline 块
	ot::time_ruler(cb, vg, 320.5f, 248, 184, 20, 0.0, 1.0, 0.05, MINOR, MAJOR, nullptr);
	ot::track_lane(cb, vg, 320, 270, 184, 14, 0, LANE0, LANE1);
	ot::track_lane(cb, vg, 320, 284, 184, 14, 1, LANE0, LANE1);
	ot::keyframe_link(cb, vg, 352, 277, 392, 277, MINOR);
	ot::keyframe(cb, vg, 352, 277, 5, ot::kf_state::hollow, KF, KF_SEL);
	ot::keyframe(cb, vg, 392, 277, 5, ot::kf_state::solid, KF, KF_SEL);
	ot::keyframe(cb, vg, 432, 277, 5, ot::kf_state::selected, KF, KF_SEL);
	ot::playhead(cb, vg, 412, 248, 298, RED2);
	ot::range_bar(cb, vg, 320, 304.5f, 184, 24, GRAY_R);

	// 曲线块（先画曲线再画控制柄，锚点在曲线之上）
	{
		cubic_v_t c = { vec2(20, 320), vec2(90, 256), vec2(170, 338), vec2(250, 272) };
		ot::bezier3(cb, vg, c, MAGENTA, 3.f);
		ot::curve_handles(cb, vg, c, MINOR, HANDLE);
		quadratic_v_t q = { vec2(180, 316), vec2(240, 340), vec2(255, 300) };
		ot::bezier2(cb, vg, q, CYAN, 3.f);
	}
	ot::origin_axes(cb, vg, 12, 352, 60);
	ot::insertion_marker(cb, vg, 12, 360, 140, ORANGE);
	ot::canvas_grid(cb, vg, 12.5f, 420.5f, 240, 80, 16, GRID_MIN, 4, GRID_MAJ);
	ot::crosshair(cb, vg, 280.5f, 440.5f, 5, 14, CYAN);
	ot::measure(cb, vg, 12, 504.5f, 200, 504.5f, WHITE, nullptr);

	// 取色器块
	ot::hue_bar(cb, vg, 300, 344, 204, 20);
	ot::sv_box(cb, vg, 300, 372, 112, 96, 210.f);
	ot::alpha_bar(cb, vg, 420, 372, 36, 96, RED3, 5.f);
	ot::color_wheel(cb, vg, 478, 398, 32, 18);
	ot::swatch(cb, vg, 300, 478, 28, 18, BLUE2, false);
	ot::swatch(cb, vg, 332, 478, 28, 18, BLUE2, true);

	// ---- 填充规则回归（不开 tess2，验证默认路径自动路由到 tess2） ----
	// 双子路径圆环：嵌套 bbox 重叠 -> 自动路由，出洞
	cb->new_path(vg);
	cb->arc(vg, 60, 570, 40, 0, ot::kPi2);
	cb->new_sub_path(vg);
	cb->arc_negative(vg, 60, 570, 24, 0, -ot::kPi2);
	cb->set_source_color(vg, GREEN);
	cb->fill(vg);
	// EVEN_ODD 交叠双圆：交叠区（透镜）透空
	cb->set_fill_rule(vg, VG_FILL_RULE_EVEN_ODD);
	cb->new_path(vg);
	cb->circle(vg, 170, 556, 28);
	cb->new_sub_path(vg);
	cb->circle(vg, 206, 584, 28);
	cb->set_source_color(vg, YELLOW);
	cb->fill(vg);
	// NON_ZERO 半透明交叠双圆：交叠区单次合成（耳切路径会双重混合）
	cb->set_fill_rule(vg, VG_FILL_RULE_NON_ZERO);
	cb->new_path(vg);
	cb->circle(vg, 280, 556, 28);
	cb->new_sub_path(vg);
	cb->circle(vg, 316, 584, 28);
	cb->set_source_color(vg, ot::with_alpha(RED, 0x80));
	cb->fill(vg);
	// 蝴蝶结（自交单轮廓）
	cb->new_path(vg);
	cb->move_to(vg, 380, 540);
	cb->line_to(vg, 420, 540);
	cb->line_to(vg, 380, 570);
	cb->line_to(vg, 420, 570);
	cb->close_path(vg);
	cb->set_source_color(vg, MAGENTA);
	cb->fill(vg);
	// 互不相交双子路径：继续走耳切，结果正确
	cb->new_path(vg);
	cb->rectangle(vg, 12, 620, 40, 30);
	cb->rectangle(vg, 60, 624, 40, 30);
	cb->set_source_color(vg, WHITE);
	cb->fill(vg);
	// 复合路径裁剪：环外/内孔都被裁掉
	// 注：路径裁剪生效需要先 reset_clip(0) 清模板（帧初始为全通过 1）
	cb->reset_clip(vg, 0);
	cb->new_path(vg);
	cb->arc(vg, 170, 655, 42, 0, ot::kPi2);
	cb->new_sub_path(vg);
	cb->arc_negative(vg, 170, 655, 24, 0, -ot::kPi2);
	cb->clip(vg);
	ot::fill_rect(cb, vg, 130, 620, 100, 80, CYAN);
	cb->reset_clip(vg, 1);
	// 对照：单圆路径裁剪（不路由 tess2，验证渲染器路径裁剪本身）
	cb->reset_clip(vg, 0);
	cb->new_path(vg);
	cb->circle(vg, 350, 655, 40);
	cb->clip(vg);
	ot::fill_rect(cb, vg, 300, 620, 100, 80, MAGENTA);
	cb->reset_clip(vg, 1);

	// ---- 提交并读回 ----
	ovg_draw_data_t data = get_draw_list(vg);
	data.fbo = &fbo;
	ovg_render_frame(ctx, &data, 1);
	ovg_wait_idle(ctx);
	{
		ovg_fx_stats_t fx = {};
		ovg_fx_get_stats(ctx, &fx);
		std::printf("fx stats: effects=%zu roi_px=%zu frame_bytes=%zu pool_sets=%zu pending_sets=%zu\n",
			fx.frame_effects, fx.frame_roi_pixels, fx.frame_bytes, fx.pool_sets, fx.pending_sets);
	}
	std::vector<uint32_t> pixels;
	if (!download_texture(device, ovg_fbo_color_texture(&fbo), W, H, pixels))
	{
		std::fprintf(stderr, "download failed\n");
		return 2;
	}
	g_pixels = &pixels;
	g_w = W;
	g_h = H;

	// ---- 断言 ----
	// 四角圆角
	expect_pixel(72, 67, BLUE, "round4 center");
	expect_pixel(14, 14, BG, "round4 TL outside");
	expect_pixel(126, 16, BLUE, "round4 TR inside");
	expect_pixel(130, 120, BLUE, "round4 BR sharp");
	expect_pixel(18, 116, BLUE, "round4 BL inside");
	expect_pixel(14, 120, BG, "round4 BL outside");
	// 圆环
	expect_pixel(196, 66, BG, "ring hole");
	expect_pixel(196, 31, RED, "ring top");
	expect_pixel(196, 101, RED, "ring bottom");
	// 星形
	expect_pixel(300, 66, YELLOW, "star center");
	expect_pixel(300, 28, YELLOW, "star tip axis");
	expect_pixel(323, 34, BG, "star between arms");
	// 扇形
	expect_pixel(417, 45, GREEN, "pie inside");
	expect_pixel(375, 45, BG, "pie outside");
	expect_pixel(434, 64, GREEN, "pie east axis");
	// 弧带
	expect_pixel(462, 34, ORANGE, "arc band");
	// 胶囊
	expect_pixel(16, 145, GREEN2, "capsule left-cap");
	expect_pixel(14, 134, BG, "capsule outside");
	// 垂直渐变（期望值按像素中心 t 计算，量化容差）
	expect_near(200, 135, ot::lerp_color(TOP, BOT, 3.5f / 40.f), 3, "gradient top");
	expect_near(200, 169, ot::lerp_color(TOP, BOT, 37.5f / 40.f), 3, "gradient bottom");
	expect_near(200, 152, ot::lerp_color(TOP, BOT, 20.5f / 40.f), 3, "gradient mid");
	// 棋盘（实现：i+j 为偶数的格子画 c0）
	expect_pixel(268, 136, CHK0, "checker cell00");
	expect_pixel(276, 136, CHK1, "checker cell10");
	// 虚线矩形（路径自 (340,132) 起，on=8/off=8）
	expect_pixel(344, 132, WHITE, "dash on-segment");
	expect_pixel(354, 132, BG, "dash gap");
	// 阴影特效：offset(0,15) 向下——下方有阴影核、上方为纯背景
	expect_pixel(470, 157, WHITE, "shadow rect body");
	expect_not_pixel(470, 202, BG, "shadow below rect");
	expect_pixel(470, 110, BG, "no shadow above rect");
	expect_pixel(441, 137, ORANGE, "effect cleared");
	// 箭头 / 符号
	expect_pixel(17, 185, ORANGE, "arrow shaft");
	expect_pixel(125, 215, ORANGE, "arrow head");
	expect_pixel(159, 205, LIME, "check glyph");
	expect_pixel(196, 200, CYAN, "xmark glyph");
	expect_pixel(237, 200, WHITE, "chevron vertex");
	expect_pixel(268, 202, KF, "tri glyph");
	expect_pixel(296, 200, WHITE, "cross center");
	// 选择框
	expect_pixel(436, 186, CYAN, "sel frame top edge");
	expect_pixel(376, 186, WHITE, "sel handle TL");
	expect_pixel(496, 240, CYAN, "sel frame right edge");
	expect_pixel(372, 182, BG, "sel outside corner");
	expect_pixel(436, 160, CYAN, "rotate handle circle");
	// Timeline
	expect_pixel(366, 251, MAJOR, "ruler major tick");
	expect_pixel(322, 277, LANE0, "lane 0 bg");
	expect_pixel(322, 291, LANE1, "lane 1 bg");
	expect_pixel(352, 277, LANE0, "keyframe hollow");
	expect_pixel(392, 277, KF, "keyframe solid");
	expect_pixel(432, 277, KF_SEL, "keyframe selected");
	expect_pixel(412, 270, RED2, "playhead line");
	expect_pixel(412, 251, RED2, "playhead cap");
	expect_pixel(412, 316, BG, "range bar hollow");
	expect_pixel(412, 304, GRAY_R, "range bar edge");
	// 曲线
	{
		cubic_v_t c = { vec2(20, 320), vec2(90, 256), vec2(170, 338), vec2(250, 272) };
		vec2 mid = ot::sample_bezier3(c, 0.5f);
		expect_pixel((int)mid.x, (int)mid.y, MAGENTA, "bezier3 midpoint");
		quadratic_v_t q = { vec2(180, 316), vec2(240, 340), vec2(255, 300) };
		vec2 mid2 = ot::sample_bezier2(q, 0.5f);
		expect_pixel((int)mid2.x, (int)mid2.y, CYAN, "bezier2 midpoint");
	}
	expect_pixel(20, 320, WHITE, "curve anchor");
	expect_pixel(90, 256, HANDLE, "curve control dot");
	// 坐标轴 / 网格 / 十字 / 插入指示 / 标注
	expect_pixel(40, 352, RED_AX, "origin X axis");
	expect_pixel(12, 382, GREEN_AX, "origin Y axis");
	expect_pixel(28, 430, GRID_MIN, "grid minor");
	expect_pixel(76, 430, GRID_MAJ, "grid major");
	expect_pixel(280, 428, CYAN, "crosshair arm");
	expect_pixel(280, 440, BG, "crosshair gap");
	expect_pixel(80, 360, ORANGE, "insertion line");
	expect_pixel(12, 366, ORANGE, "insertion tri");
	expect_pixel(100, 504, WHITE, "measure line");
	expect_pixel(197, 504, WHITE, "measure head");
	// 取色器
	expect_near(300, 354, ot::hsva(0.f, 1.f, 1.f), 4, "hue bar red");
	expect_near(402, 354, ot::hsva(180.f, 1.f, 1.f), 6, "hue bar cyan");
	expect_near(302, 466, ot::rgba(0, 0, 0), 8, "sv box bottom-left");
	expect_near(410, 374, ot::hsva(210.f, 1.f, 1.f), 10, "sv box top-right");
	expect_near(421, 420, RED3, 8, "alpha bar left");
	// 右端：RED3 按剩余 alpha 混在棋盘格 CHK1 上
	expect_near(452, 430, ot::lerp_color(ot::rgba(242, 242, 242), RED3,
		1.f - (452.5f - 420.f) / 36.f), 5, "alpha bar right");
	expect_not_pixel(478, 368, BG, "color wheel ring");
	expect_pixel(478, 398, BG, "color wheel hole");
	expect_pixel(314, 487, BLUE2, "swatch body");
	// 背景
	expect_pixel(508, 508, BG, "untouched background");
	// 填充规则：双子路径环（自动路由 tess2）
	expect_pixel(60, 570, BG, "subpath ring hole");
	expect_pixel(60, 537, GREEN, "subpath ring band");		// 上环带 r=33
	expect_pixel(60, 600, GREEN, "subpath ring band low");
	expect_pixel(60, 522, BG, "subpath ring outside");		// r=48 > 40
	// EVEN_ODD 交叠双圆：透镜透空
	expect_pixel(152, 548, YELLOW, "odd circle exclusive");
	expect_pixel(188, 570, BG, "odd lens empty");
	// NON_ZERO 半透明交叠：交叠区单次合成
	expect_near(262, 548, ot::lerp_color(BG, RED, 128.f / 255.f), 4, "nz overlap single");
	expect_near(298, 570, ot::lerp_color(BG, RED, 128.f / 255.f), 4, "nz lens single");
	// 蝴蝶结：两个三角瓣填充，交叉点右侧空
	expect_pixel(400, 545, MAGENTA, "bowtie top lobe");
	expect_pixel(400, 565, MAGENTA, "bowtie bottom lobe");
	expect_pixel(415, 552, BG, "bowtie outside lobes");
	// 互不相交双子路径
	expect_pixel(32, 635, WHITE, "disjoint rect 1");
	expect_pixel(80, 639, WHITE, "disjoint rect 2");
	expect_pixel(56, 635, BG, "disjoint gap");
	// 复合路径裁剪
	expect_pixel(170, 624, CYAN, "compound clip band");		// r=31 在环带内
	expect_pixel(170, 655, BG, "compound clip hole");
	expect_pixel(215, 690, BG, "compound clip outside");	// 矩形内、环外
	// 对照：单圆裁剪
	expect_pixel(350, 655, MAGENTA, "simple clip inside");
	expect_pixel(390, 690, BG, "simple clip outside");
	// 扩展区背景
	expect_pixel(508, 760, BG, "extended background");

	// ---- PNG 目检输出（RGBA 字 -> BGRA 字节） ----
	{
		std::vector<uint8_t> bgra((size_t)W * H * 4);
		for (size_t i = 0; i < (size_t)W * H; ++i)
		{
			// 读回 word 为 0xAABBGGRR：byte0=R、byte1=G、byte2=B、byte3=A
			uint32_t w = pixels[i];
			bgra[i * 4 + 0] = (uint8_t)(w >> 16);	// B
			bgra[i * 4 + 1] = (uint8_t)(w >> 8);	// G
			bgra[i * 4 + 2] = (uint8_t)w;			// R
			bgra[i * 4 + 3] = (uint8_t)(w >> 24);	// A
		}
		if (!write_png_bgra("ovg_tools_test.png", bgra.data(), W, H))
			std::printf("note: png dump failed (non-fatal)\n");
	}

	free_vgfbo_sdl3(&fbo);
	cb->destroy_rvg(vg);
	free_ctx_cb(cb);
	free_ovgctx_sdl3(ctx);
	free_sdl3gpu_device(ovg_device);
	SDL_DestroyGPUDevice(device);
	SDL_Quit();

	if (g_fail)
	{
		std::printf("ovg tools test: %d/%d checks failed\n", g_fail, g_checks);
		return 1;
	}
	std::printf("ovg tools test: ok (%d checks)\n", g_checks);
	return 0;
}
