// sample/vgui_chart_render_test.cpp
//
// Offscreen render test for the optional vgui_chart module.
//
// The pure C test (vgui_chart_test.c) covers data management, JSON
// configuration and export.  This test complements it by drawing a real
// chart, gauge and JSON dashboard through vgui + OVG into an offscreen FBO,
// then sampling the readback to make sure the module paints what it reports:
//
//   - chart plot background, decimated line series and bar series
//   - gauge track, zone arcs, value arc and value text
//   - dashboard KPI / chart / gauge cards built from schema v1 JSON
//
// A PNG of the last dashboard frame is written for visual inspection.  When
// no Vulkan device is available the test returns 77 so CTest reports SKIP,
// matching the other GPU tests in this repository.

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#include <glm/glm.hpp>

#include "gpu_test_util.h"
#include "ovg.h"
#include "ovg_fonts.h"
#include "tools/tools.hpp"

#include <vgui/vgui.h>
#include <vgui/backend/vgui_renderer_sdl3.h>
#include <vgui/chart/vgui_chart.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace {

	constexpr int kWidth = 900;
	constexpr int kHeight = 720;
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

	struct rgb {
		int r = 0;
		int g = 0;
		int b = 0;
		int a = 255;
	};

	rgb from_vgui(vgui_color color)
	{
		rgb out;
		out.r = (int)((color >> 16) & 0xFFu);
		out.g = (int)((color >> 8) & 0xFFu);
		out.b = (int)(color & 0xFFu);
		out.a = (int)((color >> 24) & 0xFFu);
		return out;
	}

	rgb blend(rgb src, rgb dst, int alpha)
	{
		const int a = std::clamp(alpha, 0, 255);
		rgb out;
		out.r = (src.r * a + dst.r * (255 - a) + 127) / 255;
		out.g = (src.g * a + dst.g * (255 - a) + 127) / 255;
		out.b = (src.b * a + dst.b * (255 - a) + 127) / 255;
		out.a = 255;
		return out;
	}

	rgb mix(rgb a, rgb b, float t)
	{
		t = std::clamp(t, 0.0f, 1.0f);
		auto channel = [t](int ca, int cb) {
			return std::clamp((int)((float)ca + ((float)cb - (float)ca) * t
				+ 0.5f), 0, 255);
		};
		rgb out;
		out.r = channel(a.r, b.r);
		out.g = channel(a.g, b.g);
		out.b = channel(a.b, b.b);
		out.a = channel(a.a, b.a);
		return out;
	}

	bool near_color(uint32_t word, rgb expected, int tolerance)
	{
		// SDL_GPU readback is R8G8B8A8 in memory order, packed by the
		// gpu_test_util rgba() helper as AABBGGRR when read as uint32_t.
		const int r = (int)(word & 0xFFu);
		const int g = (int)((word >> 8) & 0xFFu);
		const int b = (int)((word >> 16) & 0xFFu);
		const int a = (int)((word >> 24) & 0xFFu);
		if (std::abs(r - expected.r) > tolerance) return false;
		if (std::abs(g - expected.g) > tolerance) return false;
		if (std::abs(b - expected.b) > tolerance) return false;
		if (std::abs(a - expected.a) > tolerance) return false;
		return true;
	}

	int count_color(const test_context& t, rgb expected, int tolerance,
		int x0 = 0, int y0 = 0, int x1 = kWidth, int y1 = kHeight)
	{
		x0 = std::clamp(x0, 0, kWidth);
		x1 = std::clamp(x1, 0, kWidth);
		y0 = std::clamp(y0, 0, kHeight);
		y1 = std::clamp(y1, 0, kHeight);
		int count = 0;
		for (int y = y0; y < y1; ++y)
			for (int x = x0; x < x1; ++x)
				if (near_color(t.pixels[(size_t)y * kWidth + x],
					expected, tolerance))
					++count;
		return count;
	}

	void check(test_context& t, bool ok, const char* what)
	{
		++t.checks;
		if (ok)
		{
			std::printf("PASS %s\n", what);
		}
		else
		{
			std::printf("FAIL %s\n", what);
			++t.failures;
		}
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
		t.vg = t.cb->new_rvg(t.cb->ac);
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

	bool write_scene_png(const test_context& t, const char* path)
	{
		std::vector<uint8_t> bgra((size_t)kWidth * kHeight * 4);
		for (size_t i = 0; i < t.pixels.size(); ++i)
		{
			const uint32_t word = t.pixels[i];
			// write_png_bgra takes B,G,R,A bytes.
			bgra[i * 4 + 0] = (uint8_t)((word >> 16) & 0xFFu);
			bgra[i * 4 + 1] = (uint8_t)((word >> 8) & 0xFFu);
			bgra[i * 4 + 2] = (uint8_t)(word & 0xFFu);
			bgra[i * 4 + 3] = (uint8_t)((word >> 24) & 0xFFu);
		}
		return write_png_bgra(path, bgra.data(), kWidth, kHeight);
	}

	/* Eyeball PNGs land in <repo>/screenshots regardless of the process CWD
	 * so they never pollute the repository root. */
	std::string screenshot_path(const char* name)
	{
		std::filesystem::create_directories(
			std::string(VGUI_SOURCE_DIR) + "/screenshots");
		return std::string(VGUI_SOURCE_DIR) + "/screenshots/" + name;
	}

	bool render_frame(test_context& t, vgui_context* ui)
	{
		// Two passes: the first one may fill CPU glyph/image atlases, the
		// second pass samples the finished atlases.
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

	vgui_theme make_theme()
	{
		vgui_theme theme = {};
		theme.text = 0xFFF2F5F9u;
		theme.panel_bg = 0xFF0B0D10u;
		theme.panel_border = 0xFF2B3038u;
		theme.button_bg = 0xFF171B21u;
		theme.button_hover = 0xFF20252D;
		theme.button_active = 0xFF2C333D;
		theme.button_border = 0xFF3A424E;
		theme.accent = 0xFF4C9AFFu;
		theme.accent_hover = 0xFF78B4FFu;
		theme.track = 0xFF262C34u;
		theme.edit_bg = 0xFF11151Au;
		theme.edit_selection = 0x804C9AFFu;
		theme.scrollbar = 0xFF5A6472u;
		theme.disabled = 0xFF6A7380u;
		theme.scrollbar_hover = 0xFF7C8797u;
		theme.checkmark = 0xFFFFFFFFu;
		theme.switch_thumb = 0xFFFFFFFFu;
		theme.list_bg = 0xFF11151Au;
		theme.list_border = 0xFF2B3038u;
		theme.row_text = 0xFFE7ECF3u;
		theme.row_hover = 0xFF1A2028u;
		theme.row_selected = 0x604C9AFFu;
		theme.row_selected_text = 0xFFFFFFFFu;
		theme.tree_guide = 0x302B3038u;
		theme.menu_bg = 0xFF171B21u;
		theme.menu_border = 0xFF2B3038u;
		theme.menu_hover = 0xFF20252D;
		theme.menu_disabled_text = 0xFF6A7380u;
		theme.tooltip_bg = 0xFF1B2028u;
		theme.tooltip_border = 0xFF3A424Eu;
		theme.tooltip_text = 0xFFF2F5F9u;
		theme.modal_backdrop = 0xC0000000u;
		theme.modal_bg = 0xFF171B21u;
		theme.modal_border = 0xFF2B3038u;
		theme.modal_danger = 0xFFE5534Bu;
		theme.toast_bg = 0xFF171B21u;
		theme.toast_border = 0xFF2B3038u;
		theme.toast_text = 0xFFF2F5F9u;
		theme.toast_info = 0xFF4C9AFFu;
		theme.toast_success = 0xFF3FB950u;
		theme.toast_warning = 0xFFD29922u;
		theme.toast_error = 0xFFE5534Bu;
		theme.dock_bg = 0xFF101419u;
		theme.dock_border = 0xFF2B3038u;
		theme.dock_tab_bg = 0xFF171B21u;
		theme.dock_tab_active = 0xFF101419u;
		theme.dock_tab_text = 0xFFE7ECF3u;
		theme.dock_floating_bg = 0xFF171B21u;
		theme.dock_floating_border = 0xFF3A424Eu;
		return theme;
	}

	void make_chart_theme(vgui_chart_theme& theme, vgui_color series0,
		vgui_color series1)
	{
		std::memset(&theme, 0, sizeof(theme));
		theme.struct_size = (uint32_t)sizeof(theme);
		theme.plot_bg = 0xFF202A36u;
		theme.grid = 0xFF344052u;
		theme.axis = 0xFF6E7B8Du;
		theme.text = 0xFFFFFFFFu;
		theme.muted_text = 0xFFAAB6C6u;
		theme.tooltip_bg = 0xFF10151Au;
		theme.tooltip_border = 0xFF4C9AFFu;
		theme.tooltip_text = 0xFFFFFFFFu;
		theme.crosshair = 0xFFFFD166u;
		theme.selection = 0x404C9AFFu;
		theme.series[0] = series0;
		theme.series[1] = series1;
		theme.positive = 0xFF3FB950u;
		theme.negative = 0xFFE5534Bu;
		theme.warning = 0xFFD29922u;
		theme.critical = 0xFFE5534Bu;
	}

	void set_rect(vgui_widget* widget, float x, float y, float w, float h)
	{
		vgui_widget_set_pos(widget, x, y);
		vgui_widget_set_size(widget, w, h);
	}

	void test_chart_scene(test_context& t, font_familys_t* font)
	{
		vgui_context* ui = vgui_create(font, 15.0f);
		check(t, ui != nullptr, "chart scene: context");
		if (!ui)
			return;
		vgui_context_set_size(ui, kWidth, kHeight);
		vgui_context_set_ui_scale(ui, 1.0f);
		const vgui_theme theme = make_theme();
		vgui_context_set_theme(ui, &theme);
		vgui_widget_set_background(vgui_root(ui), theme.panel_bg);

		vgui_chart* chart = vgui_chart_create(vgui_root(ui));
		check(t, chart != nullptr, "chart scene: create");
		if (!chart)
		{
			vgui_destroy(ui);
			return;
		}
		vgui_widget* widget = vgui_chart_widget(chart);
		set_rect(widget, 20, 20, 860, 560);
		vgui_chart_set_title(chart, "Request latency");

		vgui_chart_theme chart_theme;
		make_chart_theme(chart_theme, 0xFFFF3B30u, 0xFF34C759u);
		vgui_chart_set_theme(chart, &chart_theme);

		vgui_chart_options options;
		vgui_chart_options_default(&options);
		options.follow = 0;
		options.legend = 1;
		vgui_chart_set_options(chart, &options);

		vgui_chart_series_desc desc;
		vgui_chart_series_desc_default(&desc);
		desc.key = "latency";
		desc.name = "Latency";
		desc.kind = VGUI_CHART_LINE;
		desc.color = 0xFFFF3B30u;
		desc.unit = "ms";
		vgui_series_id line = vgui_chart_add_series(chart, &desc);
		check(t, line != 0, "chart scene: line series");
		check(t, vgui_chart_reserve(chart, line, 100000) == 1,
			"chart scene: reserve 100k");

		std::vector<vgui_chart_point> points(100000);
		for (size_t i = 0; i < points.size(); ++i)
		{
			points[i].x = (double)i * 0.05;
			points[i].y = 48.0 + std::sin((double)i * 0.0021) * 18.0
				+ std::sin((double)i * 0.017) * 2.5;
		}
		check(t, vgui_chart_append_points(chart, line, points.data(),
			points.size()) == 1, "chart scene: append 100k");

		desc.key = "errors";
		desc.name = "Errors";
		desc.kind = VGUI_CHART_BAR;
		desc.color = 0xFF00FF00u;
		desc.unit = "n";
		vgui_series_id bars = vgui_chart_add_series(chart, &desc);
		check(t, bars != 0, "chart scene: bar series");
		std::vector<vgui_chart_point> bar_points(24);
		for (size_t i = 0; i < bar_points.size(); ++i)
		{
			bar_points[i].x = (double)i * 200.0;
			bar_points[i].y = 6.0 + (double)((i * 37) % 41);
		}
		check(t, vgui_chart_append_points(chart, bars, bar_points.data(),
			bar_points.size()) == 1, "chart scene: append bars");

		vgui_chart_axis_desc axis;
		vgui_chart_axis_desc_default(&axis);
		axis.min = 0.0;
		axis.max = 90.0;
		axis.tick_count = 6;
		axis.grid = 1;
		axis.unit = "ms";
		vgui_chart_set_axis(chart, 1, &axis);

		vgui_update(ui, 0.016f);
		vgui_layout(ui);
		check(t, render_frame(t, ui), "chart scene: render/readback");

		const rgb bg = from_vgui(theme.panel_bg);
		const rgb plot = from_vgui(chart_theme.plot_bg);
		const rgb line_color = from_vgui(0xFFFF3B30u);
		const rgb bar_color = blend(from_vgui(0xFF00FF00u), plot, 0xD8);
		const rgb text = from_vgui(0xFFFFFFFFu);

		check(t, near_color(t.pixels[0], bg, 1),
			"chart scene: root background");
		check(t, count_color(t, plot, 2) > 10000,
			"chart scene: plot background");
		check(t, count_color(t, line_color, 8) > 300,
			"chart scene: line series pixels");
		check(t, count_color(t, bar_color, 10) > 100,
			"chart scene: bar series pixels");
		check(t, write_scene_png(t, screenshot_path("vgui_chart_render_chart.png").c_str()),
			"chart scene: write PNG");
		check(t, count_color(t, text, 24) > 50,
			"chart scene: text pixels");

		vgui_chart_stats stats;
		std::memset(&stats, 0, sizeof(stats));
		stats.struct_size = (uint32_t)sizeof(stats);
		check(t, vgui_chart_get_stats(chart, &stats) == 1,
			"chart scene: stats");
		check(t, stats.raw_points >= 100024, "chart scene: raw point count");
		check(t, stats.drawn_points > 0 && stats.drawn_points < 20000,
			"chart scene: decimated draw count");
		check(t, stats.series_drawn == 2, "chart scene: series count");

		vgui_chart_destroy(chart);
		vgui_destroy(ui);
	}

	void test_gauge_scene(test_context& t, font_familys_t* font)
	{
		vgui_context* ui = vgui_create(font, 15.0f);
		check(t, ui != nullptr, "gauge scene: context");
		if (!ui)
			return;
		vgui_context_set_size(ui, kWidth, kHeight);
		vgui_context_set_ui_scale(ui, 1.0f);
		const vgui_theme theme = make_theme();
		vgui_context_set_theme(ui, &theme);
		vgui_widget_set_background(vgui_root(ui), theme.panel_bg);

		vgui_gauge* gauge = vgui_gauge_create(vgui_root(ui));
		check(t, gauge != nullptr, "gauge scene: create");
		if (!gauge)
		{
			vgui_destroy(ui);
			return;
		}
		set_rect(vgui_gauge_widget(gauge), 220, 60, 460, 460);

		vgui_chart_theme chart_theme;
		make_chart_theme(chart_theme, 0xFF00A0FFu, 0xFF34C759u);
		vgui_gauge_set_theme(gauge, &chart_theme);

		vgui_gauge_zone zones[2] = {};
		zones[0].from = 0.0;
		zones[0].to = 70.0;
		zones[0].color = 0xFF00C853u;
		zones[1].from = 70.0;
		zones[1].to = 100.0;
		zones[1].color = 0xFFD50000u;

		vgui_gauge_options options;
		vgui_gauge_options_default(&options);
		options.min = 0.0;
		options.max = 100.0;
		options.start_angle = 135.0;
		options.sweep = 270.0;
		options.show_ticks = 1;
		options.show_value = 1;
		options.label = "Disk usage";
		options.unit = "%";
		options.format_pattern = "%.0f";
		options.zones = zones;
		options.zone_count = 2;
		check(t, vgui_gauge_set_options(gauge, &options) == 1,
			"gauge scene: options");
		check(t, vgui_gauge_set_value(gauge, 78.0) == 1,
			"gauge scene: value");

		vgui_update(ui, 0.016f);
		vgui_layout(ui);
		check(t, render_frame(t, ui), "gauge scene: render/readback");

		const rgb value_arc = from_vgui(0xFF00A0FFu);
		const rgb zone_ok = from_vgui(0xFF00C853u);
		const rgb zone_bad = from_vgui(0xFFD50000u);
		const rgb text = from_vgui(0xFFFFFFFFu);

		check(t, count_color(t, value_arc, 8) > 100,
			"gauge scene: value arc pixels");
		check(t, count_color(t, zone_ok, 8) > 50,
			"gauge scene: low zone pixels");
		check(t, count_color(t, zone_bad, 8) > 50,
			"gauge scene: high zone pixels");
		check(t, write_scene_png(t, screenshot_path("vgui_chart_render_gauge.png").c_str()),
			"gauge scene: write PNG");
		check(t, count_color(t, text, 24) > 200,
			"gauge scene: value text pixels");

		vgui_gauge_destroy(gauge);
		vgui_destroy(ui);
	}

	const char* k_dashboard_json =
		"{"
		"\"version\":1,"
		"\"title\":\"Operations\","
		"\"layout\":{\"mode\":\"stack\",\"gap\":10,\"padding\":12},"
		"\"cards\":["
		"{\"type\":\"kpi\",\"id\":\"cpu\",\"title\":\"CPU\","
		"\"value_key\":\"cpu.value\",\"delta_key\":\"cpu.delta\","
		"\"text_key\":\"cpu.text\",\"status_key\":\"cpu.status\","
		"\"format\":\"%.1f\",\"unit\":\"%\"},"
		"{\"type\":\"chart\",\"id\":\"load\",\"title\":\"Load\","
		"\"options\":{\"legend\":true,\"follow\":false,\"tooltip\":true},"
		"\"axes\":{\"x\":{\"scale\":\"linear\",\"tick_count\":6},"
		"\"y\":{\"min\":0,\"max\":100,\"tick_count\":6,\"unit\":\"req/s\"}},"
		"\"series\":["
		"{\"key\":\"rps\",\"name\":\"Requests\",\"kind\":\"line\","
		"\"color\":\"#FF2D95\",\"unit\":\"req/s\"},"
		"{\"key\":\"errors\",\"name\":\"Errors\",\"kind\":\"bar\","
		"\"color\":\"#00E5FF\"}]},"
		"{\"type\":\"gauge\",\"id\":\"disk\",\"title\":\"Disk\","
		"\"value_key\":\"disk.value\","
		"\"gauge\":{\"min\":0,\"max\":100,\"show_ticks\":true,"
		"\"show_value\":true,\"label\":\"Disk usage\",\"unit\":\"%\","
		"\"format\":\"%.0f\","
		"\"zones\":[{\"from\":0,\"to\":70,\"color\":\"#00C853\"},"
		"{\"from\":70,\"to\":100,\"color\":\"#D50000\"}]}}"
		"]"
		"}";

	void test_dashboard_scene(test_context& t, font_familys_t* font)
	{
		vgui_context* ui = vgui_create(font, 15.0f);
		check(t, ui != nullptr, "dashboard scene: context");
		if (!ui)
			return;
		vgui_context_set_size(ui, kWidth, kHeight);
		vgui_context_set_ui_scale(ui, 1.0f);
		const vgui_theme theme = make_theme();
		vgui_context_set_theme(ui, &theme);
		vgui_widget_set_background(vgui_root(ui), theme.panel_bg);

		vgui_dashboard* dashboard = vgui_dashboard_create(vgui_root(ui));
		check(t, dashboard != nullptr, "dashboard scene: create");
		if (!dashboard)
		{
			vgui_destroy(ui);
			return;
		}
		set_rect(vgui_dashboard_widget(dashboard), 0, 0, kWidth, kHeight);

		char error[256] = {};
		check(t, vgui_dashboard_load_json(dashboard, k_dashboard_json,
			std::strlen(k_dashboard_json), error, sizeof(error)) == 1,
			"dashboard scene: load JSON");
		if (error[0])
			std::printf("      load error: %s\n", error);

		vgui_chart* chart = vgui_dashboard_chart(dashboard, "load");
		vgui_gauge* gauge = vgui_dashboard_gauge(dashboard, "disk");
		check(t, chart != nullptr, "dashboard scene: chart lookup");
		check(t, gauge != nullptr, "dashboard scene: gauge lookup");
		if (!chart || !gauge)
		{
			vgui_dashboard_destroy(dashboard);
			vgui_destroy(ui);
			return;
		}

		vgui_chart_theme chart_theme;
		make_chart_theme(chart_theme, 0xFFFF2D95u, 0xFF00E5FFu);
		vgui_chart_set_theme(chart, &chart_theme);
		vgui_gauge_set_theme(gauge, &chart_theme);

		check(t, vgui_dashboard_set_number(dashboard, "cpu.value", 42.5) == 1,
			"dashboard scene: KPI value binding");
		check(t, vgui_dashboard_set_text(dashboard, "cpu.delta", "+1.2%") == 1,
			"dashboard scene: KPI delta binding");
		check(t, vgui_dashboard_set_text(dashboard, "cpu.text", "8 cores") == 1,
			"dashboard scene: KPI subtitle binding");
		check(t, vgui_dashboard_set_status(dashboard, "cpu.status",
			VGUI_CHART_STATUS_WARNING, "warm") == 1,
			"dashboard scene: KPI status binding");
		check(t, vgui_dashboard_set_number(dashboard, "disk.value", 78.0) == 1,
			"dashboard scene: gauge binding");

		std::vector<vgui_chart_point> points(120);
		for (size_t i = 0; i < points.size(); ++i)
		{
			points[i].x = (double)i;
			points[i].y = 40.0 + std::sin((double)i * 0.22) * 24.0;
		}
		check(t, vgui_dashboard_append_points(dashboard, "load", "rps",
			points.data(), points.size()) == 1,
			"dashboard scene: append rps");
		for (size_t i = 0; i < points.size(); ++i)
			points[i].y = 3.0 + (double)((i * 13) % 17);
		check(t, vgui_dashboard_append_points(dashboard, "load", "errors",
			points.data(), points.size()) == 1,
			"dashboard scene: append errors");

		vgui_update(ui, 0.016f);
		vgui_layout(ui);
		vgui_dashboard_layout(dashboard);
		check(t, render_frame(t, ui), "dashboard scene: render/readback");

		const rgb line_color = from_vgui(0xFFFF2D95u);
		const rgb bar_color = blend(from_vgui(0xFF00E5FFu),
			from_vgui(chart_theme.plot_bg), 0xD8);
		const rgb gauge_arc = from_vgui(0xFFFF2D95u);
		const rgb card_bg = mix(from_vgui(theme.panel_bg),
			from_vgui(theme.text), 0.035f);

		check(t, count_color(t, line_color, 8) > 100,
			"dashboard scene: chart line pixels");
		check(t, count_color(t, bar_color, 12) > 50,
			"dashboard scene: chart bar pixels");
		check(t, count_color(t, gauge_arc, 8) > 100,
			"dashboard scene: gauge pixels");
		check(t, count_color(t, card_bg, 3) > 2000,
			"dashboard scene: card background");
		check(t, count_color(t, from_vgui(theme.text), 24) > 300,
			"dashboard scene: dashboard text");

		check(t, write_scene_png(t, screenshot_path("vgui_chart_render.png").c_str()),
			"dashboard scene: write PNG");

		vgui_dashboard_destroy(dashboard);
		vgui_destroy(ui);
	}

} /* namespace */

namespace {

	/* ------------------------------------------------------------- P1 ----- */

	/* 2026-03-17 00:00:00 UTC. */
	constexpr double kEpochMarch17 = 1773705600.0;

	struct export_state {
		test_context* t = nullptr;
		std::vector<uint8_t> rgba;
	};

	export_state g_export;

	/* Host-side PNG renderer: repaints the widget's own context into a fresh
	 * FBO at the requested size, painting desc.background underneath. */
	int test_png_render(void* user, vgui_widget* widget, int width, int height,
		vgui_color background, const uint8_t** out_rgba, int* out_stride)
	{
		export_state* state = (export_state*)user;
		test_context* t = state->t;
		vgui_context* ui = vgui_widget_get_context(widget);
		if (!t || !ui || width <= 0 || height <= 0)
			return 0;

		vgui_context_set_size(ui, (float)width, (float)height);
		vgui_widget_set_pos(widget, 0.0f, 0.0f);
		vgui_widget_set_size(widget, (float)width, (float)height);
		vgui_update(ui, 0.016f);
		vgui_layout(ui);

		vg_fbo_t fbo = vgpu_create_fbo(t->vgpu_ctx, width, height, nullptr);
		if (!fbo.ctx)
			return 0;
		t->vg->width = (float)width;
		t->vg->height = (float)height;
		for (int pass = 0; pass < 2; ++pass)
		{
			t->cb->clear(t->vg);
			t->cb->reset_clip(t->vg, 1);
			/* Backdrop: charts leave padding transparent. */
			t->cb->set_source_color(t->vg, ((background & 0xFF00FF00u)
				| ((background & 0x00FF0000u) >> 16)
				| ((background & 0x000000FFu) << 16)));
			t->cb->rectangle(t->vg, 0.0f, 0.0f, (float)width, (float)height);
			t->cb->fill(t->vg);
			vgui_draw(ui, t->cb, t->vg);

			ovg_draw_data_t data = get_draw_list(t->vg);
			data.fbo = &fbo;
			vgpu_render_frame(t->vgpu_ctx, &data, 1);
			vgpu_wait_idle(t->vgpu_ctx);
		}
		std::vector<uint32_t> pixels;
		const bool ok = download_texture(t->device,
			vgpu_fbo_color_texture(&fbo), width, height, pixels);
		vgpu_destroy_fbo(&fbo);
		t->vg->width = (float)kWidth;
		t->vg->height = (float)kHeight;
		if (!ok || pixels.size() != (size_t)width * (size_t)height)
			return 0;

		/* SDL_GPU readback words are AABBGGRR; stb PNG wants RGBA bytes. */
		state->rgba.resize(pixels.size() * 4);
		for (size_t i = 0; i < pixels.size(); ++i)
		{
			const uint32_t word = pixels[i];
			state->rgba[i * 4 + 0] = (uint8_t)(word & 0xFFu);
			state->rgba[i * 4 + 1] = (uint8_t)((word >> 8) & 0xFFu);
			state->rgba[i * 4 + 2] = (uint8_t)((word >> 16) & 0xFFu);
			state->rgba[i * 4 + 3] = (uint8_t)((word >> 24) & 0xFFu);
		}
		*out_rgba = state->rgba.data();
		*out_stride = width * 4;
		return 1;
	}

	void test_datetime_scene(test_context& t, font_familys_t* font)
	{
		vgui_context* ui = vgui_create(font, 15.0f);
		check(t, ui != nullptr, "datetime scene: context");
		if (!ui)
			return;
		vgui_context_set_size(ui, kWidth, kHeight);
		vgui_context_set_ui_scale(ui, 1.0f);
		const vgui_theme theme = make_theme();
		vgui_context_set_theme(ui, &theme);
		vgui_widget_set_background(vgui_root(ui), theme.panel_bg);

		vgui_chart* chart = vgui_chart_create(vgui_root(ui));
		check(t, chart != nullptr, "datetime scene: create");
		if (!chart)
		{
			vgui_destroy(ui);
			return;
		}
		set_rect(vgui_chart_widget(chart), 20, 20, 860, 560);
		vgui_chart_set_title(chart, "Timeline");

		vgui_chart_theme chart_theme;
		make_chart_theme(chart_theme, 0xFFFF3B30u, 0xFF34C759u);
		vgui_chart_set_theme(chart, &chart_theme);

		vgui_chart_axis_desc x_axis;
		vgui_chart_axis_desc_default(&x_axis);
		x_axis.scale = VGUI_CHART_SCALE_DATETIME;
		x_axis.time_zone = 0;	/* UTC: deterministic across machines */
		x_axis.tick_count = 3;
		x_axis.grid = 1;
		vgui_chart_set_axis(chart, 0, &x_axis);

		vgui_chart_axis_desc y_axis;
		vgui_chart_axis_desc_default(&y_axis);
		y_axis.min = 0.0;
		y_axis.max = 10.0;
		y_axis.tick_count = 5;
		y_axis.grid = 1;
		vgui_chart_set_axis(chart, 1, &y_axis);

		vgui_chart_series_desc desc;
		vgui_chart_series_desc_default(&desc);
		desc.key = "s";
		desc.name = "Samples";
		desc.kind = VGUI_CHART_LINE;
		vgui_series_id series = vgui_chart_add_series(chart, &desc);
		check(t, series != 0, "datetime scene: series");

		/* One point every 6 hours across exactly three days. */
		std::vector<vgui_chart_point> points(13);
		for (size_t i = 0; i < points.size(); ++i)
		{
			points[i].x = kEpochMarch17 + (double)i * 21600.0;
			points[i].y = 3.0 + std::sin((double)i * 0.9) * 2.5;
		}
		check(t, vgui_chart_append_points(chart, series, points.data(),
			points.size()) == 1, "datetime scene: append points");

		vgui_update(ui, 0.016f);
		vgui_layout(ui);
		check(t, render_frame(t, ui), "datetime scene: render/readback");

		vgui_chart_tick ticks[8] = {};
		const int total = vgui_chart_get_axis_ticks(chart, 0, ticks, 8);
		check(t, total == 4, "datetime scene: midnight tick count");
		if (total == 4)
		{
			bool values_ok = true;
			bool labels_ok = true;
			for (int i = 0; i < 4; ++i)
			{
				char label[16] = {};
				std::snprintf(label, sizeof(label), "03-%02d", 17 + i);
				if (std::fabs(ticks[i].value
					- (kEpochMarch17 + 86400.0 * (double)i)) > 1e-6)
					values_ok = false;
				if (!ticks[i].text
					|| std::strcmp(ticks[i].text, label) != 0)
					labels_ok = false;
			}
			check(t, values_ok, "datetime scene: ticks align to UTC midnight");
			check(t, labels_ok, "datetime scene: tick labels are MM-DD");
		}

		vgui_chart_tick yticks[8] = {};
		const int ytotal = vgui_chart_get_axis_ticks(chart, 1, yticks, 8);
		check(t, ytotal > 0, "datetime scene: y ticks available");
		check(t, yticks[0].text != nullptr, "datetime scene: y tick text set");

		vgui_chart_stats stats;
		std::memset(&stats, 0, sizeof(stats));
		stats.struct_size = (uint32_t)sizeof(stats);
		check(t, vgui_chart_get_stats(chart, &stats) == 1
			&& stats.visible_points == points.size(),
			"datetime scene: all points visible");

		vgui_chart_destroy(chart);
		vgui_destroy(ui);
	}

	const char* k_grid_dashboard_json =
		"{"
		"\"version\":1,"
		"\"title\":\"Grid\","
		"\"layout\":{\"mode\":\"grid\",\"columns\":3,\"gap\":10,\"padding\":10},"
		"\"cards\":["
		"{\"type\":\"kpi\",\"id\":\"a\",\"title\":\"Alpha\","
		"\"value_key\":\"a.value\",\"format\":\"%.0f\",\"unit\":\"%\"},"
		"{\"type\":\"kpi\",\"id\":\"b\",\"title\":\"Beta\","
		"\"value_key\":\"b.value\",\"format\":\"%.0f\",\"unit\":\"ms\"},"
		"{\"type\":\"kpi\",\"id\":\"c\",\"title\":\"Gamma\","
		"\"value_key\":\"c.value\",\"format\":\"%.0f\",\"unit\":\"n\"},"
		"{\"type\":\"chart\",\"id\":\"load\",\"title\":\"Load\","
		"\"series\":[{\"key\":\"rps\",\"kind\":\"line\"}],\"span\":2},"
		"{\"type\":\"gauge\",\"id\":\"disk\",\"title\":\"Disk\","
		"\"value_key\":\"disk.value\"}"
		"]"
		"}";

	void test_grid_scene(test_context& t, font_familys_t* font)
	{
		vgui_context* ui = vgui_create(font, 15.0f);
		check(t, ui != nullptr, "grid scene: context");
		if (!ui)
			return;
		vgui_context_set_size(ui, kWidth, kHeight);
		vgui_context_set_ui_scale(ui, 1.0f);
		const vgui_theme theme = make_theme();
		vgui_context_set_theme(ui, &theme);
		vgui_widget_set_background(vgui_root(ui), theme.panel_bg);

		vgui_dashboard* dashboard = vgui_dashboard_create(vgui_root(ui));
		check(t, dashboard != nullptr, "grid scene: create");
		if (!dashboard)
		{
			vgui_destroy(ui);
			return;
		}
		set_rect(vgui_dashboard_widget(dashboard), 0, 0, kWidth, kHeight);

		char error[256] = {};
		check(t, vgui_dashboard_load_json(dashboard, k_grid_dashboard_json,
			std::strlen(k_grid_dashboard_json), error, sizeof(error)) == 1,
			"grid scene: load JSON");
		if (error[0])
			std::printf("      load error: %s\n", error);

		vgui_chart* chart = vgui_dashboard_chart(dashboard, "load");
		vgui_gauge* gauge = vgui_dashboard_gauge(dashboard, "disk");
		check(t, chart != nullptr, "grid scene: chart lookup");
		check(t, gauge != nullptr, "grid scene: gauge lookup");
		if (!chart || !gauge)
		{
			vgui_dashboard_destroy(dashboard);
			vgui_destroy(ui);
			return;
		}

		vgui_dashboard_set_number(dashboard, "a.value", 11.0);
		vgui_dashboard_set_number(dashboard, "b.value", 22.0);
		vgui_dashboard_set_number(dashboard, "c.value", 33.0);
		vgui_dashboard_set_number(dashboard, "disk.value", 64.0);

		std::vector<vgui_chart_point> points(80);
		for (size_t i = 0; i < points.size(); ++i)
			points[i] = {(double)i, 40.0 + std::sin((double)i * 0.3) * 20.0};
		check(t, vgui_dashboard_append_points(dashboard, "load", "rps",
			points.data(), points.size()) == 1, "grid scene: append points");

		vgui_update(ui, 0.016f);
		vgui_layout(ui);
		vgui_dashboard_layout(dashboard);
		check(t, render_frame(t, ui), "grid scene: render/readback");

		/* Derived series[0] colour is the theme accent. */
		const rgb line_color = from_vgui(theme.accent);
		const rgb card_bg = mix(from_vgui(theme.panel_bg),
			from_vgui(theme.text), 0.035f);

		check(t, count_color(t, line_color, 8) > 80,
			"grid scene: chart line pixels");
		check(t, count_color(t, card_bg, 3) > 2000,
			"grid scene: card backgrounds");
		check(t, count_color(t, from_vgui(theme.text), 24) > 200,
			"grid scene: text pixels");

		check(t, write_scene_png(t, screenshot_path("vgui_chart_render_grid.png").c_str()),
			"grid scene: write PNG");

		vgui_dashboard_destroy(dashboard);
		vgui_destroy(ui);
	}

	void test_png_export_scene(test_context& t, font_familys_t* font)
	{
		vgui_context* ui = vgui_create(font, 15.0f);
		check(t, ui != nullptr, "png scene: context");
		if (!ui)
			return;
		vgui_context_set_size(ui, kWidth, kHeight);
		vgui_context_set_ui_scale(ui, 1.0f);
		const vgui_theme theme = make_theme();
		vgui_context_set_theme(ui, &theme);
		/* Keep the root transparent so desc.background shows through. */
		vgui_widget_set_background(vgui_root(ui), 0);

		vgui_chart* chart = vgui_chart_create(vgui_root(ui));
		check(t, chart != nullptr, "png scene: create");
		if (!chart)
		{
			vgui_destroy(ui);
			return;
		}
		set_rect(vgui_chart_widget(chart), 0, 0, (float)kWidth,
			(float)kHeight);
		vgui_chart_set_title(chart, "Export target");
		/* CUSTOM widgets get an opaque theme panel background by default;
		 * clear it so desc.background shows through the padding. */
		vgui_widget* chart_widget = vgui_chart_widget(chart);
		vgui_widget_set_background(chart_widget, 0);
		vgui_widget_set_border(chart_widget, 0, 0);

		vgui_chart_theme chart_theme;
		make_chart_theme(chart_theme, 0xFFFF3B30u, 0xFF34C759u);
		vgui_chart_set_theme(chart, &chart_theme);

		vgui_chart_series_desc desc;
		vgui_chart_series_desc_default(&desc);
		desc.key = "s";
		desc.name = "Samples";
		desc.kind = VGUI_CHART_LINE;
		vgui_series_id series = vgui_chart_add_series(chart, &desc);
		std::vector<vgui_chart_point> points(200);
		for (size_t i = 0; i < points.size(); ++i)
			points[i] = {(double)i * 0.5,
				50.0 + std::sin((double)i * 0.11) * 30.0};
		check(t, vgui_chart_append_points(chart, series, points.data(),
			points.size()) == 1, "png scene: append points");

		g_export.t = &t;
		check(t, vgui_chart_set_png_renderer(test_png_render, &g_export) == 1,
			"png scene: install renderer");

		{
			vgui_chart_png_desc png_desc;
			vgui_chart_png_desc_default(&png_desc);
			png_desc.width = 640;
			png_desc.height = 400;
			png_desc.background = 0xFF3366AAu;

			std::vector<uint8_t> file_bytes;
			auto file_write = [](void* user, const char* data,
				size_t size) -> int {
				auto* out = (std::vector<uint8_t>*)user;
				out->insert(out->end(), data, data + size);
				return 1;
			};
			check(t, vgui_chart_export_png(chart, &png_desc, file_write,
				&file_bytes) == 1, "png scene: export PNG");

			check(t, file_bytes.size() > 1000, "png scene: PNG has content");
			const bool signature = file_bytes.size() >= 8
				&& std::memcmp(file_bytes.data(), "\x89PNG\r\n\x1a\n", 8) == 0;
			check(t, signature, "png scene: PNG signature");
			if (signature)
			{
				const unsigned char expect_header[16] = {
					0, 0, 0, 13, 'I', 'H', 'D', 'R',
					0, 0, 2, 128,	/* 640 */
					0, 0, 1, 144	/* 400 */
				};
				check(t, std::memcmp(file_bytes.data() + 8, expect_header, 16)
					== 0, "png scene: IHDR dimensions");
			}
			check(t, file_bytes.size() >= 12
				&& std::memcmp(file_bytes.data() + file_bytes.size() - 8,
					"IEND", 4) == 0, "png scene: IEND trailer");

			const std::string export_path =
				screenshot_path("vgui_chart_export.png");
			FILE* out = std::fopen(export_path.c_str(), "wb");
			if (out)
			{
				std::fwrite(file_bytes.data(), 1, file_bytes.size(), out);
				std::fclose(out);
			}
		}

		{
			/* The backdrop colour must show in the transparent padding of
			 * the exported RGBA buffer (top-left corner). */
			const bool backdrop_ok = g_export.rgba.size() >= 4
				&& g_export.rgba[0] == 0x33u
				&& g_export.rgba[1] == 0x66u
				&& g_export.rgba[2] == 0xAAu
				&& g_export.rgba[3] == 0xFFu;
			check(t, backdrop_ok, "png scene: backdrop colour under padding");
			const bool varied = [&]() {
				if (g_export.rgba.empty())
					return false;
				for (size_t i = 4; i + 4 <= g_export.rgba.size(); i += 4)
				{
					if (std::memcmp(&g_export.rgba[i], &g_export.rgba[0], 4)
						!= 0)
						return true;
				}
				return false;
			}();
			check(t, varied, "png scene: exported pixels are not uniform");
		}

		vgui_chart_set_png_renderer(nullptr, nullptr);
		vgui_chart_destroy(chart);
		vgui_destroy(ui);
	}

} /* namespace */

int main()
{
	font_cache_cx* font_cache = nullptr;
	font_familys_t* font = nullptr;
	if (!init_font(font_cache, font))
	{
		std::fprintf(stderr, "cannot initialise test font\n");
		return 2;
	}

	test_context t;
	if (!init_renderer(t))
	{
		shutdown_renderer(t);
		return kSkipReturnCode;
	}

	test_chart_scene(t, font);
	test_gauge_scene(t, font);
	test_dashboard_scene(t, font);
	test_datetime_scene(t, font);
	test_grid_scene(t, font);
	test_png_export_scene(t, font);

	shutdown_renderer(t);

	std::printf("%d passed, %d failed\n", t.checks - t.failures, t.failures);
	return t.failures == 0 ? 0 : 1;
}
