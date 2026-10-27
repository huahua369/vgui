// sample/vgui_screenshot_test.cpp
//
// E03 截图回归测试：离屏渲染 Dock / PropertyGrid / Outliner / AssetBrowser /
// Viewport 的关键状态，与 sample/screenshots 下的基线 PNG 做容差对比。
//
// 流程与 vgui_texture_test 相同：SDL3 + SDL_GPU(Vulkan) 创建离屏 FBO，OVG 录制
// vgui_draw 的绘制列表，提交后读回像素。测试不创建窗口，可在无交互环境（CI）
// 中运行；没有 Vulkan 设备时返回 77，由 CTest 标记为 SKIP。
//
// 用法：
//   vgui_screenshot_test                          # 对比当前渲染与基线
//   vgui_screenshot_test --update                 # 生成/更新基线
//   vgui_screenshot_test --filter=outliner        # 只跑名称包含过滤串的场景
//   vgui_screenshot_test --list                   # 列出场景
//   --baseline-dir=<dir> --out-dir=<dir>
//   --tolerance=<0..255> --max-diff-ratio=<0..1>
//
// 失败时在 --out-dir 写 <scene>_actual.png 和 <scene>_diff.png，差异像素标红。

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "gpu_test_util.h"
#include "ovg.h"
#include "ovg_fonts.h"
#include "tools/tools.hpp"
#include "vgui.h"
#include "vgui_renderer_sdl3.h"

#include "asset_browser.h"
#include "editor_context.h"
#include "editor_shell.h"
#include "outliner_view.h"
#include "property_grid.h"
#include "viewport_camera.h"
#include "viewport_gizmo.h"
#include "viewport_grid.h"
#include "viewport_view.h"

bool write_png_bgra(const char* path, const uint8_t* bgra, int w, int h);

using namespace vgui_editor_kit;

namespace ot = ovg_tools;

namespace {

	constexpr int kCanvasWidth = 1440;
	constexpr int kCanvasHeight = 900;
	constexpr int kSkipReturnCode = 77;

	// ---------------------------------------------------------------- 选项

	struct options
	{
		bool update = false;
		bool list = false;
		std::string filter;
		std::string baseline_dir;
		std::string out_dir = "screenshots";
		int tolerance = 2;
		double max_diff_ratio = 0.0005;
	};

	struct scene_spec
	{
		const char* name = "";
		int width = 0;
		int height = 0;
		bool light = false;
	};

	const scene_spec kScenes[] = {
		{ "dock_default_dark", 1440, 900, false },
		{ "dock_floating_dark", 1440, 900, false },
		{ "property_grid_dark", 560, 720, false },
		{ "property_grid_states_dark", 560, 720, false },
		{ "outliner_selected_dark", 400, 560, false },
		{ "outliner_filtered_light", 400, 560, true },
		{ "asset_browser_grid_light", 700, 520, true },
		{ "asset_browser_list_dark", 700, 520, false },
		{ "viewport_gizmo_dark", 760, 520, false },
	};

	// ---------------------------------------------------------------- 主题

	vgui_theme make_theme(bool light)
	{
		vgui_theme t = {};
		if (light)
		{
			t.text = 0xFF303030;
			t.panel_bg = 0xFFF3F3F3;
			t.panel_border = 0xFFD2D2D2;
			t.button_bg = 0xFFE4E4E4;
			t.button_hover = 0xFFF2F2F2;
			t.button_active = 0xFFCFCFCF;
			t.button_border = 0xFFBFBFBF;
			t.accent = 0xFF2D7FF9;
			t.accent_hover = 0xFF5A9DF9;
			t.track = 0xFFDCDCDC;
			t.edit_bg = 0xFFFFFFFF;
			t.edit_selection = 0x802D7FF9;
			t.scrollbar = 0xFF9A9A9A;
			t.disabled = 0xFFAAAAAA;
			t.scrollbar_hover = 0xFF858585;
			t.checkmark = 0xFFFFFFFF;
			t.switch_thumb = 0xFFFFFFFF;
			t.list_bg = 0xFFFFFFFF;
			t.list_border = 0xFFD0D0D0;
			t.row_text = 0xFF303030;
			t.row_hover = 0xFFEAF2FF;
			t.row_selected = 0x602D7FF9;
			t.row_selected_text = 0xFF1A1A1A;
			t.tree_guide = 0x30000000;
			t.menu_bg = 0xFFFFFFFF;
			t.menu_border = 0xFFC8C8C8;
			t.menu_hover = 0xFFEAF2FF;
			t.menu_disabled_text = 0xFFAAAAAA;
			t.tooltip_bg = 0xFFFFFFFF;
			t.tooltip_border = 0xFFC8C8C8;
			t.tooltip_text = 0xFF303030;
			t.modal_backdrop = 0xC0000000;
			t.modal_bg = 0xFFFFFFFF;
			t.modal_border = 0xFFC8C8C8;
			t.modal_danger = 0xFFD03030;
			t.toast_bg = 0xFFFFFFFF;
			t.toast_border = 0xFFC8C8C8;
			t.toast_text = 0xFF303030;
			t.toast_info = 0xFF2D7FF9;
			t.toast_success = 0xFF2E8B57;
			t.toast_warning = 0xFFD08A20;
			t.toast_error = 0xFFD03030;
			t.dock_bg = 0xFFF3F3F3;
			t.dock_border = 0xFFD2D2D2;
			t.dock_tab_bg = 0xFFE4E4E4;
			t.dock_tab_active = 0xFFF3F3F3;
			t.dock_tab_text = 0xFF303030;
			t.dock_floating_bg = 0xFFFFFFFF;
			t.dock_floating_border = 0xFFC8C8C8;
			t.dock_floating_title = 0xFFE4E4E4;
			return t;
		}

		t.text = 0xFFE6E6E6;
		t.panel_bg = 0xFF1E1E1E;
		t.panel_border = 0xFF3C3C3C;
		t.button_bg = 0xFF3A3A3A;
		t.button_hover = 0xFF4A4A4A;
		t.button_active = 0xFF2A2A2A;
		t.button_border = 0xFF555555;
		t.accent = 0xFF409EFF;
		t.accent_hover = 0xFF66B1FF;
		t.track = 0xFF303030;
		t.edit_bg = 0xFF252525;
		t.edit_selection = 0x80409EFF;
		t.scrollbar = 0xFF555555;
		t.disabled = 0xFF808080;
		t.scrollbar_hover = 0xFF727272;
		t.checkmark = 0xFFFFFFFF;
		t.switch_thumb = 0xFFFFFFFF;
		t.list_bg = 0xFF191C21;
		t.list_border = 0xFF3C3C3C;
		t.row_text = 0xFFE6E6E6;
		t.row_hover = 0xFF2A2F36;
		t.row_selected = 0x70409EFF;
		t.row_selected_text = 0xFFFFFFFF;
		t.tree_guide = 0x30FFFFFF;
		t.menu_bg = 0xFF252525;
		t.menu_border = 0xFF555555;
		t.menu_hover = 0xFF3A3A3A;
		t.menu_disabled_text = 0xFF808080;
		t.tooltip_bg = 0xFF252525;
		t.tooltip_border = 0xFF555555;
		t.tooltip_text = 0xFFE6E6E6;
		t.modal_backdrop = 0xC0000000;
		t.modal_bg = 0xFF252525;
		t.modal_border = 0xFF555555;
		t.modal_danger = 0xFFF05050;
		t.toast_bg = 0xFF252525;
		t.toast_border = 0xFF555555;
		t.toast_text = 0xFFE6E6E6;
		t.toast_info = 0xFF409EFF;
		t.toast_success = 0xFF50C878;
		t.toast_warning = 0xFFF0B040;
		t.toast_error = 0xFFF05050;
		t.dock_bg = 0xFF1E232B;
		t.dock_border = 0xFF2E3440;
		t.dock_tab_bg = 0xFF1A1D23;
		t.dock_tab_active = 0xFF1E232B;
		t.dock_tab_text = 0xFFB8C4D6;
		t.dock_floating_bg = 0xFF252A32;
		t.dock_floating_border = 0xFF3C4452;
		t.dock_floating_title = 0xFF1E232B;
		return t;
	}

	uint32_t to_ovg_color(vgui_color color)
	{
		const uint32_t a = (color >> 24) & 0xFFu;
		const uint32_t r = (color >> 16) & 0xFFu;
		const uint32_t g = (color >> 8) & 0xFFu;
		const uint32_t b = color & 0xFFu;
		return (a << 24) | (b << 16) | (g << 8) | r;
	}

	// ---------------------------------------------------------------- 像素画布

	struct pixel_canvas
	{
		int w = 0;
		int h = 0;
		std::vector<uint8_t> data;

		pixel_canvas(int width, int height)
			: w(width), h(height), data((size_t)width * height * 4, 0)
		{
		}

		void blend(int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
		{
			if (x < 0 || y < 0 || x >= w || y >= h || a == 0)
				return;
			uint8_t* p = &data[((size_t)y * w + x) * 4];
			if (a == 255)
			{
				p[0] = r;
				p[1] = g;
				p[2] = b;
				p[3] = 255;
				return;
			}
			const uint32_t ia = 255u - a;
			p[0] = (uint8_t)((r * a + p[0] * ia) / 255u);
			p[1] = (uint8_t)((g * a + p[1] * ia) / 255u);
			p[2] = (uint8_t)((b * a + p[2] * ia) / 255u);
			p[3] = (uint8_t)std::min<uint32_t>(255u, p[3] + a);
		}

		void rect(int x0, int y0, int x1, int y1,
			uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255)
		{
			for (int y = std::max(0, y0); y <= std::min(h - 1, y1); ++y)
				for (int x = std::max(0, x0); x <= std::min(w - 1, x1); ++x)
					blend(x, y, r, g, b, a);
		}

		void circle(float cx, float cy, float radius,
			uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255)
		{
			const int x0 = (int)std::floor(cx - radius);
			const int x1 = (int)std::ceil(cx + radius);
			const int y0 = (int)std::floor(cy - radius);
			const int y1 = (int)std::ceil(cy + radius);
			const float rr = radius * radius;
			for (int y = y0; y <= y1; ++y)
				for (int x = x0; x <= x1; ++x)
				{
					const float dx = (float)x + 0.5f - cx;
					const float dy = (float)y + 0.5f - cy;
					if (dx * dx + dy * dy <= rr)
						blend(x, y, r, g, b, a);
				}
		}

		// 软椭圆：中心最实、边缘渐隐，用于接触阴影。
		void soft_ellipse(float cx, float cy, float rx, float ry,
			uint8_t r, uint8_t g, uint8_t b, uint8_t max_alpha)
		{
			const int x0 = (int)std::floor(cx - rx);
			const int x1 = (int)std::ceil(cx + rx);
			const int y0 = (int)std::floor(cy - ry);
			const int y1 = (int)std::ceil(cy + ry);
			for (int y = y0; y <= y1; ++y)
				for (int x = x0; x <= x1; ++x)
				{
					const float dx = ((float)x + 0.5f - cx) / std::max(1.0f, rx);
					const float dy = ((float)y + 0.5f - cy) / std::max(1.0f, ry);
					const float d2 = dx * dx + dy * dy;
					if (d2 > 1.0f)
						continue;
					const uint8_t a = (uint8_t)(max_alpha * (1.0f - d2));
					blend(x, y, r, g, b, a);
				}
		}

		void ring(float cx, float cy, float outer, float inner,
			uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255)
		{
			const int x0 = (int)std::floor(cx - outer);
			const int x1 = (int)std::ceil(cx + outer);
			const int y0 = (int)std::floor(cy - outer);
			const int y1 = (int)std::ceil(cy + outer);
			const float ro = outer * outer;
			const float ri = inner * inner;
			for (int y = y0; y <= y1; ++y)
				for (int x = x0; x <= x1; ++x)
				{
					const float dx = (float)x + 0.5f - cx;
					const float dy = (float)y + 0.5f - cy;
					const float d = dx * dx + dy * dy;
					if (d <= ro && d >= ri)
						blend(x, y, r, g, b, a);
				}
		}

		void line(float x0, float y0, float x1, float y1, float thickness,
			uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255)
		{
			const float dx = x1 - x0;
			const float dy = y1 - y0;
			const int steps = std::max(1, (int)std::ceil(std::sqrt(dx * dx + dy * dy) * 2.0f));
			const float radius = std::max(0.5f, thickness * 0.5f);
			for (int i = 0; i <= steps; ++i)
			{
				const float t = (float)i / (float)steps;
				circle(x0 + dx * t, y0 + dy * t, radius, r, g, b, a);
			}
		}

		void polygon(const std::vector<std::pair<float, float>>& points,
			uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255)
		{
			if (points.size() < 3)
				return;
			float min_y = points[0].second;
			float max_y = points[0].second;
			for (const auto& p : points)
			{
				min_y = std::min(min_y, p.second);
				max_y = std::max(max_y, p.second);
			}
			const int y0 = std::max(0, (int)std::floor(min_y));
			const int y1 = std::min(h - 1, (int)std::ceil(max_y));
			std::vector<float> xs;
			for (int y = y0; y <= y1; ++y)
			{
				const float sy = (float)y + 0.5f;
				xs.clear();
				for (size_t i = 0; i < points.size(); ++i)
				{
					const auto& a_pt = points[i];
					const auto& b_pt = points[(i + 1) % points.size()];
					const float ay = a_pt.second;
					const float by = b_pt.second;
					if ((ay <= sy && by > sy) || (by <= sy && ay > sy))
					{
						const float t = (sy - ay) / (by - ay);
						xs.push_back(a_pt.first + (b_pt.first - a_pt.first) * t);
					}
				}
				if (xs.size() < 2)
					continue;
				std::sort(xs.begin(), xs.end());
				for (size_t i = 0; i + 1 < xs.size(); i += 2)
				{
					const int xa = std::max(0, (int)std::ceil(xs[i]));
					const int xb = std::min(w - 1, (int)std::floor(xs[i + 1]));
					for (int x = xa; x <= xb; ++x)
						blend(x, y, r, g, b, a);
				}
			}
		}
	};

	vgui_texture* texture_from_canvas(vgui_context* ui, const pixel_canvas& canvas)
	{
		if (!ui)
			return nullptr;
		vgui_texture_desc desc = {};
		desc.width = canvas.w;
		desc.height = canvas.h;
		desc.stride = canvas.w * 4;
		desc.format = VGUI_TEXTURE_FORMAT_RGBA8;
		desc.pixels = canvas.data.data();
		return vgui_texture_create(ui, &desc);
	}

	// ---------------------------------------------------------------- 图像素材

	enum class icon_kind
	{
		folder,
		mesh,
		light,
		camera,
		eye_on,
		eye_off,
		lock_on,
		lock_off
	};

	vgui_texture* make_icon(vgui_context* ui, icon_kind kind, bool light_theme)
	{
		pixel_canvas c(16, 16);
		const uint8_t i_r = light_theme ? 48 : 214;
		const uint8_t i_g = light_theme ? 54 : 220;
		const uint8_t i_b = light_theme ? 64 : 228;
		if (kind == icon_kind::folder)
		{
			c.rect(2, 4, 7, 5, 64, 120, 210);
			c.rect(2, 6, 13, 13, 78, 138, 232);
			c.rect(3, 7, 12, 12, 108, 166, 248);
		}
		else if (kind == icon_kind::mesh)
		{
			c.polygon({ { 8, 1 }, { 14, 5 }, { 14, 10 }, { 8, 14 }, { 2, 10 }, { 2, 5 } },
				214, 132, 58);
			c.polygon({ { 8, 3 }, { 12, 5.5f }, { 8, 8 }, { 4, 5.5f } },
				238, 172, 94);
			c.polygon({ { 4, 6.5f }, { 8, 9 }, { 8, 12.5f }, { 4, 10 } },
				170, 100, 42);
			c.polygon({ { 12, 6.5f }, { 8, 9 }, { 8, 12.5f }, { 12, 10 } },
				190, 116, 50);
		}
		else if (kind == icon_kind::light)
		{
			c.circle(8, 8, 4.0f, 244, 206, 84);
			c.circle(8, 8, 2.2f, 255, 236, 156);
			c.line(8, 0.5f, 8, 2.5f, 1.2f, 244, 206, 84);
			c.line(8, 13.5f, 8, 15.5f, 1.2f, 244, 206, 84);
			c.line(0.5f, 8, 2.5f, 8, 1.2f, 244, 206, 84);
			c.line(13.5f, 8, 15.5f, 8, 1.2f, 244, 206, 84);
		}
		else if (kind == icon_kind::camera)
		{
			c.rect(1, 4, 10, 10, 92, 206, 226);
			c.polygon({ { 10, 5 }, { 15, 3 }, { 15, 11 }, { 10, 9 } }, 70, 176, 200);
			c.ring(7, 7.5f, 3.4f, 2.1f, 24, 54, 66);
		}
		else if (kind == icon_kind::eye_on || kind == icon_kind::eye_off)
		{
			// 杏仁形眼眶 = 外椭圆减内椭圆。
			for (int y = 0; y < 16; ++y)
				for (int x = 0; x < 16; ++x)
				{
					const float dx = ((float)x + 0.5f - 8.0f) / 7.0f;
					const float dy = ((float)y + 0.5f - 8.0f) / 4.0f;
					const float d = dx * dx + dy * dy;
					if (d <= 1.0f && d >= 0.42f)
						c.blend(x, y, i_r, i_g, i_b, 235);
				}
			c.circle(8, 8, 2.3f, i_r, i_g, i_b, 255);
			if (kind == icon_kind::eye_off)
				c.line(2.5f, 13.5f, 13.5f, 2.5f, 1.6f, i_r, i_g, i_b, 255);
		}
		else
		{
			// 锁：锁体 + 上方锁梁；off 状态锁梁向右开口。
			c.rect(3, 7, 12, 14, i_r, i_g, i_b, 230);
			c.rect(4, 8, 11, 13, 30, 34, 40, 200);
			const float cx = kind == icon_kind::lock_on ? 7.5f : 9.5f;
			c.ring(cx, 6.0f, 3.4f, 2.1f, i_r, i_g, i_b, 230);
			c.rect((int)cx - 1, 5, (int)cx + 1, 11, i_r, i_g, i_b, 230);
		}
		return texture_from_canvas(ui, c);
	}

	vgui_texture* make_thumbnail(vgui_context* ui, int type, int index)
	{
		pixel_canvas c(64, 64);
		c.rect(0, 0, 63, 63, 30, 34, 42);
		static const uint8_t hue[6][3] = {
			{ 84, 150, 240 }, { 240, 148, 84 }, { 96, 204, 132 },
			{ 200, 120, 236 }, { 236, 206, 90 }, { 120, 200, 226 }
		};
		const uint8_t* col = hue[index % 6];
		if (type == 0)
		{
			// 纹理：棋盘格 + 边框
			for (int y = 6; y < 58; ++y)
				for (int x = 6; x < 58; ++x)
				{
					const bool on = (((x - 6) / 8) + ((y - 6) / 8)) % 2 == 0;
					const uint8_t k = on ? 255 : 130;
					c.blend(x, y, (uint8_t)(col[0] * k / 255), (uint8_t)(col[1] * k / 255),
						(uint8_t)(col[2] * k / 255), 255);
				}
			c.rect(5, 5, 58, 5, col[0], col[1], col[2]);
			c.rect(5, 58, 58, 58, col[0], col[1], col[2]);
			c.rect(5, 5, 5, 58, col[0], col[1], col[2]);
			c.rect(58, 5, 58, 58, col[0], col[1], col[2]);
		}
		else if (type == 1)
		{
			// 网格：等轴立方体
			c.polygon({ { 32, 9 }, { 55, 24 }, { 55, 44 }, { 32, 59 }, { 9, 44 }, { 9, 24 } },
				col[0], col[1], col[2]);
			c.polygon({ { 32, 13 }, { 50, 25 }, { 32, 36 }, { 14, 25 } },
				(uint8_t)std::min(255, col[0] + 40),
				(uint8_t)std::min(255, col[1] + 40),
				(uint8_t)std::min(255, col[2] + 40));
			c.polygon({ { 14, 27 }, { 32, 38 }, { 32, 56 }, { 14, 45 } },
				(uint8_t)(col[0] * 0.65f), (uint8_t)(col[1] * 0.65f), (uint8_t)(col[2] * 0.65f));
			c.polygon({ { 50, 27 }, { 32, 38 }, { 32, 56 }, { 50, 45 } },
				(uint8_t)(col[0] * 0.8f), (uint8_t)(col[1] * 0.8f), (uint8_t)(col[2] * 0.8f));
		}
		else
		{
			// 材质：径向球
			for (int y = 0; y < 64; ++y)
				for (int x = 0; x < 64; ++x)
				{
					const float dx = (float)x + 0.5f - 32.0f;
					const float dy = (float)y + 0.5f - 32.0f;
					const float d = std::sqrt(dx * dx + dy * dy);
					if (d > 26.0f)
						continue;
					const float shade = 0.5f + 0.5f * (1.0f - d / 26.0f);
					const float nx = dx / 26.0f;
					const float ny = dy / 26.0f;
					const float light = std::max(0.0f, 0.35f + 0.65f * (-nx * 0.4f - ny * 0.6f + 0.5f));
					c.blend(x, y, (uint8_t)std::min(255.0f, col[0] * shade * light * 1.8f),
						(uint8_t)std::min(255.0f, col[1] * shade * light * 1.8f),
						(uint8_t)std::min(255.0f, col[2] * shade * light * 1.8f), 255);
				}
			c.circle(32, 32, 26.0f, col[0], col[1], col[2], 30);
		}
		return texture_from_canvas(ui, c);
	}

	// 视口用的合成 3D 画面：天空/地面渐变 + 等轴立方体 + 接触阴影。
	pixel_canvas build_viewport_canvas(int w, int h)
	{
		pixel_canvas c(w, h);
		const float horizon = h * 0.30f;
		for (int y = 0; y < h; ++y)
		{
			if (y < horizon)
			{
				const float t = (float)y / horizon;
				c.rect(0, y, w - 1, y,
					(uint8_t)(26 + 44 * t), (uint8_t)(32 + 44 * t), (uint8_t)(44 + 48 * t));
			}
			else
			{
				const float t = (y - horizon) / std::max(1.0f, h - horizon);
				c.rect(0, y, w - 1, y,
					(uint8_t)(48 - 20 * t), (uint8_t)(56 - 22 * t), (uint8_t)(66 - 24 * t));
			}
		}
		// 地平线上的淡光带
		c.rect(0, (int)horizon - 2, w - 1, (int)horizon + 1, 96, 116, 140, 80);

		const float cx = w * 0.5f;
		const float cy = h * 0.62f;
		const float sx = w * 0.12f;
		const float sy = h * 0.09f;
		const float body = h * 0.22f;
		// 接触阴影
		c.soft_ellipse(cx, cy + body + sy * 0.65f, sx * 1.15f, sy * 0.75f,
			8, 10, 14, 78);
		// 顶面 / 左面 / 右面
		c.polygon({ { cx, cy - body - sy }, { cx + sx, cy - body },
			{ cx, cy - body + sy }, { cx - sx, cy - body } },
			100, 160, 248);
		c.polygon({ { cx - sx, cy - body }, { cx, cy - body + sy },
			{ cx, cy + sy }, { cx - sx, cy } },
			52, 92, 158);
		c.polygon({ { cx + sx, cy - body }, { cx, cy - body + sy },
			{ cx, cy + sy }, { cx + sx, cy } },
			74, 124, 204);
		// 棱线
		c.line(cx - sx, cy - body, cx, cy - body + sy, 1.2f, 20, 26, 36, 200);
		c.line(cx + sx, cy - body, cx, cy - body + sy, 1.2f, 20, 26, 36, 200);
		c.line(cx - sx, cy - body, cx - sx, cy, 1.2f, 20, 26, 36, 160);
		c.line(cx + sx, cy - body, cx + sx, cy, 1.2f, 20, 26, 36, 160);
		c.line(cx, cy - body + sy, cx, cy + sy, 1.2f, 20, 26, 36, 160);
		return c;
	}

	vgui_texture* make_viewport_texture(vgui_context* ui)
	{
		return texture_from_canvas(ui, build_viewport_canvas(640, 400));
	}

	vgui_texture* make_depth_texture(vgui_context* ui)
	{
		pixel_canvas src = build_viewport_canvas(320, 200);
		for (int y = 0; y < src.h; ++y)
			for (int x = 0; x < src.w; ++x)
			{
				uint8_t* p = &src.data[((size_t)y * src.w + x) * 4];
				const uint8_t lum = (uint8_t)((p[0] * 30 + p[1] * 59 + p[2] * 11) / 100);
				p[0] = p[1] = p[2] = (uint8_t)(40 + lum / 2);
				p[3] = 255;
			}
		return texture_from_canvas(ui, src);
	}

	// ---------------------------------------------------------------- 属性模型

	std::vector<property_desc> make_property_set()
	{
		std::vector<property_desc> set;
		auto add = [&](property_desc desc) { set.push_back(std::move(desc)); };

		{
			property_desc d;
			d.id = "name";
			d.label = "Name";
			d.type = property_type::text;
			d.value = "Cube_01";
			d.category = "Object";
			d.has_default = true;
			d.default_value = "Cube";
			add(d);
		}
		{
			property_desc d;
			d.id = "visible";
			d.label = "Visible";
			d.type = property_type::boolean;
			d.value = "true";
			d.category = "Object";
			add(d);
		}
		{
			property_desc d;
			d.id = "layer";
			d.label = "Layer";
			d.type = property_type::enumeration;
			d.value = "Default";
			d.category = "Object";
			d.choices = { "Default", "FX", "UI" };
			add(d);
		}
		{
			property_desc d;
			d.id = "position";
			d.label = "Position";
			d.type = property_type::vector3;
			d.value = "0, 1.5, 0";
			d.category = "Transform";
			d.semantic = vector_semantic::position;
			d.has_range = true;
			d.min_value = -100.0;
			d.max_value = 100.0;
			d.step = 0.1;
			d.has_default = true;
			d.default_value = "0, 0, 0";
			add(d);
		}
		{
			property_desc d;
			d.id = "rotation";
			d.label = "Rotation";
			d.type = property_type::vector3;
			d.value = "0, 45, 0";
			d.category = "Transform";
			d.step = 0.5;
			add(d);
		}
		{
			property_desc d;
			d.id = "scale";
			d.label = "Scale";
			d.type = property_type::vector3;
			d.value = "1, 1, 1";
			d.category = "Transform";
			d.step = 0.01;
			add(d);
		}
		{
			property_desc d;
			d.id = "albedo";
			d.label = "Albedo";
			d.type = property_type::color;
			d.value = "#4C9AFF";
			d.category = "Material";
			add(d);
		}
		{
			property_desc d;
			d.id = "metallic";
			d.label = "Metallic";
			d.type = property_type::number;
			d.value = "0.35";
			d.category = "Material";
			d.has_range = true;
			d.min_value = 0.0;
			d.max_value = 1.0;
			d.step = 0.01;
			add(d);
		}
		{
			property_desc d;
			d.id = "smoothness";
			d.label = "Smoothness";
			d.type = property_type::number;
			d.value = "0.72";
			d.category = "Material";
			d.has_range = true;
			d.min_value = 0.0;
			d.max_value = 1.0;
			d.step = 0.01;
			add(d);
		}
		{
			property_desc d;
			d.id = "texture";
			d.label = "Base Map";
			d.type = property_type::asset;
			d.value = "textures/checker.png";
			d.category = "Material";
			d.asset_filter = "texture";
			add(d);
		}
		{
			property_desc d;
			d.id = "curve";
			d.label = "Idle Curve";
			d.type = property_type::curve;
			d.value = "cubic;0,0,0,0;0.5,1,0,0;1,0,0,0";
			d.category = "Material";
			add(d);
		}
		{
			property_desc d;
			d.id = "gradient";
			d.label = "Color Ramp";
			d.type = property_type::gradient;
			d.value = "0,#FF4C4CFF;0.5,#FFD166FF;1,#4C9AFFFF";
			d.category = "Material";
			add(d);
		}
		{
			property_desc d;
			d.id = "notes";
			d.label = "Notes";
			d.type = property_type::text;
			d.value = "Rendered by the E03 screenshot regression harness.";
			d.category = "Diagnostics";
			add(d);
		}
		{
			property_desc d;
			d.id = "asset_id";
			d.label = "Asset ID";
			d.type = property_type::integer;
			d.value = "1024";
			d.category = "Diagnostics";
			d.read_only = true;
			add(d);
		}
		return set;
	}

	std::vector<property_desc> make_shell_property_set()
	{
		std::vector<property_desc> all = make_property_set();
		std::vector<property_desc> set;
		static const char* keep[] = {
			"name", "visible", "layer", "albedo", "metallic", "smoothness",
			"texture", "notes"
		};
		for (const property_desc& desc : all)
			for (const char* id : keep)
				if (desc.id == id)
				{
					set.push_back(desc);
					break;
				}
		return set;
	}

	void install_property_handlers(property_grid& grid)
	{
		grid.set_asset_query([](const std::string& id, const std::string&,
			property_grid::asset_info* out)
		{
			if (!out)
				return false;
			if (id == "textures/checker.png")
			{
				out->name = "checker.png";
				out->kind = "texture";
				return true;
			}
			return false;
		});
	}

	// ---------------------------------------------------------------- 组件构建

	vgui_widget* make_page(vgui_context* ui, const scene_spec& spec, int padding,
		const vgui_theme& theme)
	{
		vgui_widget* page = vgui_widget_add(vgui_root(ui), VGUI_PANEL, nullptr);
		vgui_widget_set_pos(page, (float)padding, (float)padding);
		vgui_widget_set_size(page, (float)(spec.width - padding * 2),
			(float)(spec.height - padding * 2));
		vgui_widget_set_layout(page, VGUI_LAYOUT_COLUMN);
		vgui_widget_set_align_items(page, VGUI_ALIGN_STRETCH);
		vgui_widget_set_padding(page, 0, 0, 0, 0);
		vgui_widget_set_gap(page, 0);
		vgui_widget_set_background(page, theme.panel_bg);
		vgui_widget_set_border(page, theme.panel_border, 1);
		return page;
	}

	void populate_outliner(outliner_view& outliner, editor_context& context,
		vgui_texture* folder_icon, vgui_texture* mesh_icon,
		vgui_texture* light_icon, vgui_texture* camera_icon,
		vgui_texture* eye_on, vgui_texture* eye_off,
		vgui_texture* lock_on, vgui_texture* lock_off)
	{
		outliner.clear();
		// 树默认按内容宽度测量（style.tree.req_width=220）；清掉固定尺寸后
		// 由面板拉伸填满，选中行才能横跨整行。
		vgui_widget_set_size(outliner.widget(), 0, 0);
		vgui_widget_set_align_self(outliner.widget(), VGUI_ALIGN_STRETCH);
		outliner.enable_row_buttons();
		outliner.set_row_button_icons(eye_on, eye_off, lock_on, lock_off);
		outliner.set_multi_select(true);
		outliner.enable_selection_sync();

		const vgui_item_id world = outliner.add_node("World", 0, 100);
		const vgui_item_id env = outliner.add_node("Environment", world, 101);
		const vgui_item_id props = outliner.add_node("Props", world, 102);
		const vgui_item_id lights = outliner.add_node("Lights", world, 103);
		outliner.add_node("Ground Plane", env, 104);
		outliner.add_node("Sky Dome", env, 105);
		outliner.add_node("Crate_A", props, 106);
		outliner.add_node("Crate_B", props, 107);
		outliner.add_node("Player Start", props, 108);
		outliner.add_node("Key Light", lights, 109);
		outliner.add_node("Fill Light", lights, 110);
		outliner.add_node("Main Camera", world, 111);

		outliner.expand(world);
		outliner.expand(env);
		outliner.expand(props);
		outliner.expand(lights);

		outliner.set_node_icon(world, folder_icon);
		outliner.set_node_icon(env, folder_icon);
		outliner.set_node_icon(props, folder_icon);
		outliner.set_node_icon(lights, folder_icon);
		outliner.set_node_icon(104, mesh_icon);
		outliner.set_node_icon(106, mesh_icon);
		outliner.set_node_icon(107, mesh_icon);
		outliner.set_node_icon(109, light_icon);
		outliner.set_node_icon(110, light_icon);
		outliner.set_node_icon(111, camera_icon);

		outliner.set_object_visibility(105, false);
		outliner.set_object_locked(107, true);

		context.set_selection({ 106, 109 }, 106);
	}

	std::vector<vgui_item_id> populate_assets(asset_browser& browser, vgui_context* ui,
		bool grid, bool filtered)
	{
		browser.clear();
		browser.set_view(grid ? asset_browser::view_mode::grid
			: asset_browser::view_mode::list);
		browser.set_type_options({ "Texture", "Mesh", "Material" });
		if (filtered)
		{
			browser.set_search_text("rock", true);
			browser.set_type_filter("Texture");
		}
		else
		{
			browser.set_search_text("", true);
			browser.set_type_filter("");
		}

		static const struct asset_seed
		{
			const char* name;
			const char* type;
			int kind;
		} seeds[] = {
			{ "rock_albedo.png", "Texture", 0 },
			{ "rock_normal.png", "Texture", 0 },
			{ "rock_roughness.png", "Texture", 0 },
			{ "crate_albedo.png", "Texture", 0 },
			{ "boulder.mesh", "Mesh", 1 },
			{ "crate.mesh", "Mesh", 1 },
			{ "rock_material.mat", "Material", 2 },
			{ "lamp.material.mat", "Material", 2 },
			{ "terrain_albedo.png", "Texture", 0 },
			{ "props.mesh", "Mesh", 1 },
		};

		std::vector<vgui_item_id> ids;
		for (size_t i = 0; i < sizeof(seeds) / sizeof(seeds[0]); ++i)
		{
			const vgui_item_id id = browser.add_item(seeds[i].name, seeds[i].type);
			browser.set_item_thumbnail(id, make_thumbnail(ui, seeds[i].kind, (int)i));
			ids.push_back(id);
		}
		if (!ids.empty())
		{
			if (grid)
				browser.set_selection({ ids[0], ids[2] });
			else
				browser.set_selection({ ids[0] });
		}
		return ids;
	}

	void populate_shell_assets(asset_browser& browser, vgui_context* ui)
	{
		browser.clear();
		browser.set_view(asset_browser::view_mode::list);
		static const struct asset_seed
		{
			const char* name;
			const char* type;
			int kind;
		} seeds[] = {
			{ "rock_albedo.png", "Texture", 0 },
			{ "crate_albedo.png", "Texture", 0 },
			{ "boulder.mesh", "Mesh", 1 },
			{ "crate.mesh", "Mesh", 1 },
			{ "rock_material.mat", "Material", 2 },
			{ "lamp.material.mat", "Material", 2 },
		};
		std::vector<vgui_item_id> ids;
		for (size_t i = 0; i < sizeof(seeds) / sizeof(seeds[0]); ++i)
		{
			const vgui_item_id id = browser.add_item(seeds[i].name, seeds[i].type);
			browser.set_item_thumbnail(id, make_thumbnail(ui, seeds[i].kind, (int)i));
			ids.push_back(id);
		}
		if (!ids.empty())
			browser.set_selection({ ids[0] });
	}

	// ---------------------------------------------------------------- 投影

	struct projector
	{
		camera_vec3 eye{};
		camera_vec3 forward{ 0, 0, -1 };
		camera_vec3 right{ 1, 0, 0 };
		camera_vec3 up{ 0, 1, 0 };
		float tan_half_fov = 0.4663f;
		int width = 1;
		int height = 1;

		bool project(const camera_vec3& world, float* x, float* y) const
		{
			const float dx = world.x - eye.x;
			const float dy = world.y - eye.y;
			const float dz = world.z - eye.z;
			const float z = dx * forward.x + dy * forward.y + dz * forward.z;
			if (z <= 0.001f)
				return false;
			const float xc = dx * right.x + dy * right.y + dz * right.z;
			const float yc = dx * up.x + dy * up.y + dz * up.z;
			const float aspect = (float)width / (float)std::max(1, height);
			const float ndc_x = (xc / z) / (tan_half_fov * aspect);
			const float ndc_y = (yc / z) / tan_half_fov;
			*x = (ndc_x * 0.5f + 0.5f) * (float)width;
			*y = (0.5f - ndc_y * 0.5f) * (float)height;
			return true;
		}

		float depth(const camera_vec3& world) const
		{
			return (world.x - eye.x) * forward.x + (world.y - eye.y) * forward.y +
				(world.z - eye.z) * forward.z;
		}

		// 世界线段投影：先裁掉相机近平面之后的部分，否则横跨相机的网格线会
		// 因为一端不可见而整条被丢弃，地面只剩地平线附近的远端线段。
		bool project_segment(const camera_vec3& a, const camera_vec3& b,
			float* ax, float* ay, float* bx, float* by) const
		{
			constexpr float kNear = 0.05f;
			float za = depth(a);
			float zb = depth(b);
			if (za <= kNear && zb <= kNear)
				return false;
			camera_vec3 aa = a;
			camera_vec3 bb = b;
			if (za <= kNear)
			{
				const float t = (kNear - za) / (zb - za);
				aa = { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
					a.z + (b.z - a.z) * t };
			}
			else if (zb <= kNear)
			{
				const float t = (kNear - zb) / (za - zb);
				bb = { b.x + (a.x - b.x) * t, b.y + (a.y - b.y) * t,
					b.z + (a.z - b.z) * t };
			}
			return project(aa, ax, ay) && project(bb, bx, by);
		}
	};

	projector make_projector(const viewport_camera& camera, const camera_state& state,
		int width, int height)
	{
		projector p;
		p.eye = camera.position();
		p.forward = camera.forward();
		p.right = camera.right();
		p.up = camera.up();
		p.tan_half_fov = std::tan(state.fov_deg * 3.14159265358979323846f / 360.0f);
		p.width = std::max(1, width);
		p.height = std::max(1, height);
		return p;
	}

	// ---------------------------------------------------------------- 场景状态

	struct scene_state
	{
		vgui_context* ui = nullptr;
		vgui_theme theme = {};
		std::unique_ptr<editor_context> context;
		std::unique_ptr<editor_shell> shell;
		std::unique_ptr<property_grid> properties;
		std::unique_ptr<outliner_view> outliner;
		std::unique_ptr<asset_browser> assets;
		std::unique_ptr<viewport_view> viewport;
		std::unique_ptr<viewport_camera> camera;
		std::unique_ptr<viewport_gizmo> gizmo;
		std::vector<viewport_grid_line> grid_lines;
		projector proj;
	};

	void build_dock(scene_state& st, const scene_spec& spec, bool floating)
	{
		st.context = std::make_unique<editor_context>(st.ui);
		st.shell = std::make_unique<editor_shell>(*st.context);
		st.shell->build();
		editor_shell& shell = *st.shell;
		vgui_context* ui = st.ui;

		vgui_texture* folder = make_icon(ui, icon_kind::folder, false);
		vgui_texture* mesh = make_icon(ui, icon_kind::mesh, false);
		vgui_texture* light = make_icon(ui, icon_kind::light, false);
		vgui_texture* camera = make_icon(ui, icon_kind::camera, false);
		vgui_texture* eye_on = make_icon(ui, icon_kind::eye_on, false);
		vgui_texture* eye_off = make_icon(ui, icon_kind::eye_off, false);
		vgui_texture* lock_on = make_icon(ui, icon_kind::lock_on, false);
		vgui_texture* lock_off = make_icon(ui, icon_kind::lock_off, false);
		populate_outliner(shell.scene(), *st.context, folder, mesh, light, camera,
			eye_on, eye_off, lock_on, lock_off);

		install_property_handlers(shell.properties());
		shell.properties().set_properties(make_shell_property_set());

		populate_shell_assets(shell.assets(), ui);

		shell.viewport().set_texture(make_viewport_texture(ui));
		shell.preview_viewport().set_texture(make_viewport_texture(ui));
		shell.depth_viewport().set_texture(make_depth_texture(ui));

		shell.set_status(floating ? "Properties floated - layout state"
			: "Scene ready - 12 objects");

		if (!floating)
		{
			vgui_widget* dock = shell.dock();
			vgui_dock_set_zone_size(dock, VGUI_DOCK_RIGHT, 380.0f);
			vgui_dock_set_zone_size(dock, VGUI_DOCK_BOTTOM, 220.0f);
		}
		else
		{
			vgui_widget* dock = shell.dock();
			vgui_dock_set_zone_size(dock, VGUI_DOCK_LEFT, 300.0f);
			vgui_dock_set_zone_size(dock, VGUI_DOCK_BOTTOM, 240.0f);
			const vgui_item_id properties_panel = vgui_dock_find(dock, "properties");
			if (properties_panel)
				vgui_dock_float(dock, properties_panel, 820.0f, 130.0f, 500.0f, 620.0f);

			console_view& console = shell.console();
			console.append(console_view::level::info, "scene loaded: demo.ovgs", "loader");
			console.append(console_view::level::debug, "built 12 objects / 1 camera", "scene");
			console.append(console_view::level::warning, "texture checker.png uses fallback sampler", "renderer");
			console.append(console_view::level::error, "asset rock_normal.png failed validation", "importer");
			console.poll();
			vgui_widget* tabs = shell.bottom_tabs();
			const vgui_item_id console_tab = vgui_tab_get_at(tabs, 1);
			if (console_tab)
				vgui_tab_set_active(tabs, console_tab);
		}
		(void)spec;
	}

	void build_property_grid(scene_state& st, const scene_spec& spec, bool states)
	{
		vgui_widget* page = make_page(st.ui, spec, 8, st.theme);
		st.properties = std::make_unique<property_grid>();
		st.properties->build(page);
		property_grid& grid = *st.properties;

		std::vector<property_desc> set = make_property_set();
		install_property_handlers(grid);
		grid.set_properties(set);

		if (states)
		{
			grid.set_category_collapsed("Material", true);
			grid.set_property_error("notes", "Value is too long (max 32 chars)");
			grid.set_property_favorite("position", true);
			grid.set_property_favorite("albedo", true);
			grid.set_property_read_only("asset_id", true);
		}
		else
		{
			// P08 混合值：多个目标上取值不同时行进入 mixed 显示。
			std::map<std::string, std::string> values;
			for (const property_desc& desc : set)
				values[desc.id] = desc.value;
			values["name"] = "Cube";
			grid.set_multi_getter([values](const std::string& id, object_id target)
			{
				if (id == "position")
					return target == 100 ? std::string("0, 1.5, 0") : std::string("2, 0, -3");
				if (id == "visible")
					return target == 100 ? std::string("true") : std::string("false");
				if (id == "albedo")
					return target == 100 ? std::string("#4C9AFF") : std::string("#FFB84C");
				auto it = values.find(id);
				return it == values.end() ? std::string() : it->second;
			});
			grid.set_multi_setter([](const std::string&, object_id, const std::string&) {});
			grid.set_multi_targets({ 100, 101 });
		}
	}

	void build_outliner(scene_state& st, const scene_spec& spec, bool filtered)
	{
		vgui_widget* page = make_page(st.ui, spec, 8, st.theme);
		vgui_widget_set_gap(page, 6);

		if (filtered)
		{
			vgui_widget* filter = vgui_widget_add(page, VGUI_EDIT, nullptr);
			vgui_widget_set_size(filter, 0, 28);
			vgui_widget_set_align_self(filter, VGUI_ALIGN_STRETCH);
			vgui_edit_set_placeholder(filter, "Filter scene");
			vgui_edit_set_text(filter, "light");
		}

		st.context = std::make_unique<editor_context>(st.ui);
		st.outliner = std::make_unique<outliner_view>();
		st.outliner->build(*st.context, page);
		outliner_view& out = *st.outliner;

		const bool light_theme = spec.light;
		vgui_texture* folder = make_icon(st.ui, icon_kind::folder, light_theme);
		vgui_texture* mesh = make_icon(st.ui, icon_kind::mesh, light_theme);
		vgui_texture* light = make_icon(st.ui, icon_kind::light, light_theme);
		vgui_texture* camera = make_icon(st.ui, icon_kind::camera, light_theme);
		vgui_texture* eye_on = make_icon(st.ui, icon_kind::eye_on, light_theme);
		vgui_texture* eye_off = make_icon(st.ui, icon_kind::eye_off, light_theme);
		vgui_texture* lock_on = make_icon(st.ui, icon_kind::lock_on, light_theme);
		vgui_texture* lock_off = make_icon(st.ui, icon_kind::lock_off, light_theme);

		populate_outliner(out, *st.context, folder, mesh, light, camera,
			eye_on, eye_off, lock_on, lock_off);
		if (filtered)
		{
			out.set_filter("light");
			st.context->set_selection(109);
		}
	}

	void build_asset_browser(scene_state& st, const scene_spec& spec, bool grid)
	{
		vgui_widget* page = make_page(st.ui, spec, 8, st.theme);
		st.assets = std::make_unique<asset_browser>();
		st.assets->build(page);
		populate_assets(*st.assets, st.ui, grid, !grid);
	}

	void build_viewport(scene_state& st, const scene_spec& spec)
	{
		vgui_widget* page = make_page(st.ui, spec, 8, st.theme);
		vgui_widget_set_padding(page, 0, 0, 0, 0);
		vgui_widget_set_background(page, 0xFF0E1116);

		st.viewport = std::make_unique<viewport_view>();
		st.viewport->build(page);
		st.viewport->set_texture(make_viewport_texture(st.ui));

		camera_state camera_state_value;
		camera_state_value.target = { 0, 1.0f, 0 };
		camera_state_value.yaw = 0.72f;
		camera_state_value.pitch = 0.18f;
		camera_state_value.distance = 8.0f;
		camera_state_value.fov_deg = 50.0f;

		st.camera = std::make_unique<viewport_camera>();
		st.camera->set_state(camera_state_value);

		const int content_w = spec.width - 16;
		const int content_h = spec.height - 16;
		st.proj = make_projector(*st.camera, st.camera->get_state(), content_w, content_h);

		viewport_grid grid;
		grid.set_auto_density(true);
		grid.set_half_cell_count(64);
		grid.set_axis_length_cells(6.0f);
		grid.set_colors(0x66C8D2E0, 0xAAC8D2E0,
			0xFFE05050, 0xFF50E070, 0xFF5080E0);
		grid.build(st.camera->get_state(), content_w, content_h, &st.grid_lines);

		viewport_view& viewport = *st.viewport;
		viewport.set_selection_overlay([&st](ovg_ctx_cb* cb, rvg_t* vg,
			float x, float y, float, float, float)
		{
			// 世界网格
			for (const viewport_grid_line& line : st.grid_lines)
			{
				float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
				if (!st.proj.project_segment(line.a, line.b, &x0, &y0, &x1, &y1))
					continue;
				vec2 pts[2] = { vec2(x + x0, y + y0), vec2(x + x1, y + y1) };
				ot::stroke_polyline(cb, vg, pts, 2, to_ovg_color(line.color), 1.0f);
			}

			// 选中立方体的屏幕包围框
			float min_x = 1e9f, min_y = 1e9f, max_x = -1e9f, max_y = -1e9f;
			bool valid = true;
			for (int i = 0; i < 8; ++i)
			{
				const camera_vec3 corner = {
					(i & 1) ? 1.0f : -1.0f,
					(i & 2) ? 2.0f : 0.0f,
					(i & 4) ? 1.0f : -1.0f
				};
				float px = 0, py = 0;
				if (!st.proj.project(corner, &px, &py))
				{
					valid = false;
					break;
				}
				min_x = std::min(min_x, px);
				min_y = std::min(min_y, py);
				max_x = std::max(max_x, px);
				max_y = std::max(max_y, py);
			}
			if (valid)
			{
				const float bx = x + min_x;
				const float by = y + min_y;
				const float bw = std::max(1.0f, max_x - min_x);
				const float bh = std::max(1.0f, max_y - min_y);
				ot::fill_round_rect(cb, vg, bx, by, bw, bh, 2.0f, ot::rgba(64, 158, 255, 34));
				ot::stroke_rect(cb, vg, bx, by, bw, bh, ot::rgba(96, 180, 255, 220), 1.5f);
				const float hs = 5.0f;
				ot::fill_rect(cb, vg, bx - hs * 0.5f, by - hs * 0.5f, hs, hs, ot::rgba(220, 236, 255, 240));
				ot::fill_rect(cb, vg, bx + bw - hs * 0.5f, by - hs * 0.5f, hs, hs, ot::rgba(220, 236, 255, 240));
				ot::fill_rect(cb, vg, bx - hs * 0.5f, by + bh - hs * 0.5f, hs, hs, ot::rgba(220, 236, 255, 240));
				ot::fill_rect(cb, vg, bx + bw - hs * 0.5f, by + bh - hs * 0.5f, hs, hs, ot::rgba(220, 236, 255, 240));
			}
		});

		st.gizmo = std::make_unique<viewport_gizmo>();
		st.gizmo->attach(viewport);
		st.gizmo->set_frame_provider([]()
		{
			gizmo_frame frame;
			frame.valid = true;
			frame.origin = { 0, 1, 0 };
			return frame;
		});
		st.gizmo->set_projection([&st](const camera_vec3& world, float* out_x, float* out_y)
		{
			return st.proj.project(world, out_x, out_y);
		});
		st.gizmo->set_screen_length(92.0f);
		st.gizmo->set_mode(gizmo_mode::translate);
		st.gizmo->set_visible(true);
	}

	// ---------------------------------------------------------------- 渲染

	struct renderer
	{
		SDL_GPUDevice* device = nullptr;
		vgpu_device_t* vgpu_device = nullptr;
		vgpu_ctx_t* vgpu_ctx = nullptr;
		vg_fbo_t fbo = {};
		ovg_ctx_cb* cb = nullptr;
		rvg_t* vg = nullptr;
	};

	font_cache_cx* g_font_cache = nullptr;
	font_familys_t* g_font = nullptr;

	bool init_font()
	{
		g_font_cache = new_font_cache();
		if (!g_font_cache)
			return false;
		const std::string font_path =
			std::string(VGUI_SOURCE_DIR) + "/sample/res/fonts/DejaVuSans.ttf";
		if (!g_font_cache->load_font_from_path(font_path.c_str(), nullptr))
			std::fprintf(stderr, "warning: could not add %s to fontconfig\n",
				font_path.c_str());
		g_font_cache->get_sys_family();
		g_font = new_font_family(g_font_cache, "DejaVu Sans", nullptr);
		if (!g_font || g_font->count <= 0)
		{
			std::fprintf(stderr, "字体族 DejaVu Sans 没有可用字形（%s）\n",
				font_path.c_str());
			return false;
		}
		return true;
	}

	bool init_renderer(renderer& r)
	{
		if (!SDL_Init(SDL_INIT_VIDEO))
		{
			std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
			return false;
		}
		r.device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, true, "vulkan");
		if (!r.device)
		{
			std::fprintf(stderr, "SKIP: SDL_CreateGPUDevice(Vulkan) failed: %s\n",
				SDL_GetError());
			return false;
		}
		r.vgpu_device = vgpu_create_device(r.device);
		r.vgpu_ctx = vgpu_create_context(r.vgpu_device,
			SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
			SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT,
			SDL_GPU_SAMPLECOUNT_1);
		if (!r.vgpu_ctx)
		{
			std::fprintf(stderr, "vgpu_create_context failed\n");
			return false;
		}
		r.fbo = vgpu_create_fbo(r.vgpu_ctx, kCanvasWidth, kCanvasHeight, nullptr);
		r.cb = new_ctx_cb();
		r.vg = r.cb->new_rvg(r.cb->ac);
		r.vg->width = kCanvasWidth;
		r.vg->height = kCanvasHeight;
		return true;
	}

	void shutdown_renderer(renderer& r)
	{
		if (r.vg && r.cb)
			r.cb->destroy_rvg(r.vg);
		if (r.cb)
			free_ctx_cb(r.cb);
		r.vg = nullptr;
		r.cb = nullptr;
		if (r.vgpu_ctx)
		{
			vgpu_destroy_fbo(&r.fbo);
			vgpu_destroy_context(r.vgpu_ctx);
		}
		if (r.vgpu_device)
			vgpu_destroy_device(r.vgpu_device);
		if (r.device)
			SDL_DestroyGPUDevice(r.device);
		r.device = nullptr;
		r.vgpu_device = nullptr;
		r.vgpu_ctx = nullptr;
		SDL_Quit();
	}

	// 返回 0 成功；1 渲染/读回失败。
	int render_scene(renderer& r, const scene_spec& spec,
		std::vector<uint8_t>& out_rgba)
	{
		vgui_context* ui = vgui_create(g_font, 16.0f);
		if (!ui)
			return 1;
		vgui_context_set_size(ui, spec.width, spec.height);
		vgui_context_set_ui_scale(ui, 1.0f);
		vgui_theme theme = make_theme(spec.light);
		vgui_context_set_theme(ui, &theme);
		vgui_widget_set_background(vgui_root(ui), theme.panel_bg);

		{
			scene_state st;
			st.ui = ui;
			st.theme = theme;

			if (std::strcmp(spec.name, "dock_default_dark") == 0)
				build_dock(st, spec, false);
			else if (std::strcmp(spec.name, "dock_floating_dark") == 0)
				build_dock(st, spec, true);
			else if (std::strcmp(spec.name, "property_grid_dark") == 0)
				build_property_grid(st, spec, false);
			else if (std::strcmp(spec.name, "property_grid_states_dark") == 0)
				build_property_grid(st, spec, true);
			else if (std::strcmp(spec.name, "outliner_selected_dark") == 0)
				build_outliner(st, spec, false);
			else if (std::strcmp(spec.name, "outliner_filtered_light") == 0)
				build_outliner(st, spec, true);
			else if (std::strcmp(spec.name, "asset_browser_grid_light") == 0)
				build_asset_browser(st, spec, true);
			else if (std::strcmp(spec.name, "asset_browser_list_dark") == 0)
				build_asset_browser(st, spec, false);
			else if (std::strcmp(spec.name, "viewport_gizmo_dark") == 0)
				build_viewport(st, spec);
			else
			{
				vgui_destroy(ui);
				return 1;
			}

			// 固定时间步进后布局，保证动画/滚动条状态可复现。
			vgui_update(ui, 0.5f);
			vgui_layout(ui);

			r.cb->clear(r.vg);
			r.cb->reset_clip(r.vg, 1);
			vgui_draw(ui, r.cb, r.vg);

			ovg_draw_data_t data = get_draw_list(r.vg);
			data.fbo = &r.fbo;
			vgpu_render_frame(r.vgpu_ctx, &data, 1);
			vgpu_wait_idle(r.vgpu_ctx);

			// CPU 纹理（图标/缩略图）在首帧才由 image_update 分配 OVG 句柄；
			// 第二帧重绘让列表/树/资产网格的行级纹理真正可见，然后再读回。
			r.cb->clear(r.vg);
			r.cb->reset_clip(r.vg, 1);
			vgui_draw(ui, r.cb, r.vg);
			ovg_draw_data_t data2 = get_draw_list(r.vg);
			data2.fbo = &r.fbo;
			vgpu_render_frame(r.vgpu_ctx, &data2, 1);
			vgpu_wait_idle(r.vgpu_ctx);

			std::vector<uint32_t> pixels;
			if (!download_texture(r.device, vgpu_fbo_color_texture(&r.fbo),
				kCanvasWidth, kCanvasHeight, pixels))
			{
				vgui_destroy(ui);
				return 1;
			}
			out_rgba.assign((size_t)spec.width * spec.height * 4, 0);
			for (int y = 0; y < spec.height; ++y)
				for (int x = 0; x < spec.width; ++x)
				{
					const uint32_t word = pixels[(size_t)y * kCanvasWidth + x];
					uint8_t* dst = &out_rgba[((size_t)y * spec.width + x) * 4];
					dst[0] = (uint8_t)(word & 0xFFu);
					dst[1] = (uint8_t)((word >> 8) & 0xFFu);
					dst[2] = (uint8_t)((word >> 16) & 0xFFu);
					dst[3] = (uint8_t)((word >> 24) & 0xFFu);
				}
		}

		vgui_destroy(ui);
		return 0;
	}

	// ---------------------------------------------------------------- PNG / 对比

	bool write_rgba_png(const std::filesystem::path& path, const uint8_t* rgba,
		int width, int height)
	{
		std::vector<uint8_t> bgra((size_t)width * height * 4);
		for (size_t i = 0; i < (size_t)width * height; ++i)
		{
			bgra[i * 4 + 0] = rgba[i * 4 + 2];
			bgra[i * 4 + 1] = rgba[i * 4 + 1];
			bgra[i * 4 + 2] = rgba[i * 4 + 0];
			bgra[i * 4 + 3] = rgba[i * 4 + 3];
		}
		return write_png_bgra(path.string().c_str(), bgra.data(), width, height);
	}

	struct comparison
	{
		long long diff_pixels = 0;
		int max_delta = 0;
	};

	comparison compare_rgba(const std::vector<uint8_t>& actual,
		const std::vector<uint8_t>& baseline, int tolerance)
	{
		comparison result;
		if (actual.size() != baseline.size())
			return result;
		const size_t count = actual.size() / 4;
		for (size_t i = 0; i < count; ++i)
		{
			int delta = 0;
			for (int c = 0; c < 4; ++c)
				delta = std::max(delta,
					std::abs((int)actual[i * 4 + c] - (int)baseline[i * 4 + c]));
			result.max_delta = std::max(result.max_delta, delta);
			if (delta > tolerance)
				++result.diff_pixels;
		}
		return result;
	}

	void write_diff_png(const std::filesystem::path& path,
		const std::vector<uint8_t>& actual, const std::vector<uint8_t>& baseline,
		int width, int height, int tolerance)
	{
		std::vector<uint8_t> diff((size_t)width * height * 4, 0);
		for (size_t i = 0; i < (size_t)width * height; ++i)
		{
			int delta = 0;
			for (int c = 0; c < 4; ++c)
				delta = std::max(delta,
					std::abs((int)actual[i * 4 + c] - (int)baseline[i * 4 + c]));
			uint8_t* dst = &diff[i * 4];
			if (delta > tolerance)
			{
				dst[0] = 255;
				dst[1] = 40;
				dst[2] = 72;
				dst[3] = 255;
			}
			else
			{
				const uint8_t gray = (uint8_t)((baseline[i * 4] * 30 +
					baseline[i * 4 + 1] * 59 + baseline[i * 4 + 2] * 11) / 100 / 3);
				dst[0] = dst[1] = dst[2] = gray;
				dst[3] = 255;
			}
		}
		(void)write_rgba_png(path, diff.data(), width, height);
	}

	bool load_png(const std::filesystem::path& path, std::vector<uint8_t>& rgba,
		int* width, int* height)
	{
		int w = 0, h = 0, channels = 0;
		unsigned char* data = stbi_load(path.string().c_str(), &w, &h, &channels, 4);
		if (!data)
			return false;
		rgba.assign((size_t)w * h * 4, 0);
		std::memcpy(rgba.data(), data, rgba.size());
		stbi_image_free(data);
		if (width) *width = w;
		if (height) *height = h;
		return true;
	}

} // namespace

// ---------------------------------------------------------------- main

static options parse_options(int argc, char** argv)
{
	options opt;
	bool baseline_overridden = false;
	for (int i = 1; i < argc; ++i)
	{
		std::string arg = argv[i] ? argv[i] : "";
		if (arg == "--update") { opt.update = true; continue; }
		if (arg == "--list") { opt.list = true; continue; }
		if (arg.rfind("--", 0) != 0)
			continue;

		std::string key = arg;
		std::string value;
		const size_t eq = arg.find('=');
		if (eq != std::string::npos)
		{
			key = arg.substr(0, eq);
			value = arg.substr(eq + 1);
		}
		else if (i + 1 < argc &&
			(key == "--filter" || key == "--baseline-dir" || key == "--out-dir" ||
				key == "--tolerance" || key == "--max-diff-ratio"))
		{
			const char* next = argv[++i];
			value = next ? next : "";
		}

		if (key == "--filter") opt.filter = value;
		else if (key == "--baseline-dir")
		{
			opt.baseline_dir = value;
			baseline_overridden = true;
		}
		else if (key == "--out-dir") opt.out_dir = value;
		else if (key == "--tolerance")
		{
			const int v = std::atoi(value.c_str());
			opt.tolerance = std::clamp(v, 0, 255);
		}
		else if (key == "--max-diff-ratio")
		{
			const double v = std::atof(value.c_str());
			opt.max_diff_ratio = std::clamp(v, 0.0, 1.0);
		}
	}
	if (!baseline_overridden)
		opt.baseline_dir = std::string(VGUI_SOURCE_DIR) + "/sample/screenshots";
	return opt;
}

int main(int argc, char** argv)
{
	const options opt = parse_options(argc, argv);

	if (opt.list)
	{
		for (const scene_spec& spec : kScenes)
			std::printf("%-30s %4dx%-4d %s\n", spec.name, spec.width, spec.height,
				spec.light ? "light" : "dark");
		return 0;
	}

	std::error_code ec;
	std::filesystem::create_directories(opt.out_dir, ec);
	if (opt.update)
		std::filesystem::create_directories(opt.baseline_dir, ec);

	renderer r;
	if (!init_renderer(r))
	{
		shutdown_renderer(r);
		return kSkipReturnCode;
	}
	if (!init_font())
	{
		std::fprintf(stderr, "无法初始化字体（需要 sample/res/fonts/DejaVuSans.ttf）\n");
		shutdown_renderer(r);
		return 2;
	}

	int ran = 0;
	int failures = 0;
	for (const scene_spec& spec : kScenes)
	{
		if (!opt.filter.empty() &&
			std::string(spec.name).find(opt.filter) == std::string::npos)
			continue;
		++ran;

		std::vector<uint8_t> actual;
		if (render_scene(r, spec, actual) != 0)
		{
			std::printf("FAIL %-30s render/readback failed\n", spec.name);
			++failures;
			continue;
		}

		const std::filesystem::path baseline_path =
			std::filesystem::path(opt.baseline_dir) / (std::string(spec.name) + ".png");

		if (opt.update)
		{
			if (!write_rgba_png(baseline_path, actual.data(), spec.width, spec.height))
			{
				std::printf("FAIL %-30s could not write baseline %s\n", spec.name,
					baseline_path.string().c_str());
				++failures;
			}
			else
			{
				std::printf("BASELINE %-26s %s\n", spec.name,
					baseline_path.string().c_str());
			}
			continue;
		}

		std::vector<uint8_t> baseline;
		int baseline_w = 0, baseline_h = 0;
		if (!load_png(baseline_path, baseline, &baseline_w, &baseline_h))
		{
			std::printf("FAIL %-30s missing baseline %s (run --update)\n", spec.name,
				baseline_path.string().c_str());
			++failures;
			continue;
		}
		if (baseline_w != spec.width || baseline_h != spec.height)
		{
			std::printf("FAIL %-30s baseline size %dx%d != %dx%d\n", spec.name,
				baseline_w, baseline_h, spec.width, spec.height);
			write_rgba_png(std::filesystem::path(opt.out_dir) /
				(std::string(spec.name) + "_actual.png"), actual.data(),
				spec.width, spec.height);
			++failures;
			continue;
		}

		const comparison cmp = compare_rgba(actual, baseline, opt.tolerance);
		const double total_pixels = (double)spec.width * spec.height;
		const double ratio = total_pixels > 0
			? (double)cmp.diff_pixels / total_pixels : 0.0;
		if (cmp.diff_pixels > 0 && ratio > opt.max_diff_ratio)
		{
			const std::filesystem::path actual_path =
				std::filesystem::path(opt.out_dir) / (std::string(spec.name) + "_actual.png");
			const std::filesystem::path diff_path =
				std::filesystem::path(opt.out_dir) / (std::string(spec.name) + "_diff.png");
			write_rgba_png(actual_path, actual.data(), spec.width, spec.height);
			write_diff_png(diff_path, actual, baseline, spec.width, spec.height,
				opt.tolerance);
			std::printf("FAIL %-30s diff=%lld/%lld (%.3f%%) max_delta=%d -> %s\n",
				spec.name, cmp.diff_pixels, (long long)total_pixels, ratio * 100.0,
				cmp.max_delta, diff_path.string().c_str());
			++failures;
		}
		else
		{
			std::printf("ok   %-30s diff=%lld max_delta=%d\n",
				spec.name, cmp.diff_pixels, cmp.max_delta);
		}
	}

	if (g_font)
		delete_font_family(g_font);
	g_font = nullptr;
	if (g_font_cache)
		free_font_cache(g_font_cache);
	g_font_cache = nullptr;
	shutdown_renderer(r);

	if (ran == 0)
	{
		std::printf("没有匹配的场景（filter=%s）\n", opt.filter.c_str());
		return 2;
	}
	if (failures)
	{
		std::printf("E03 screenshots: %d/%d scenes failed\n", failures, ran);
		return 1;
	}
	std::printf("E03 screenshots: ok (%d scenes)%s\n", ran,
		opt.update ? " [baselines updated]" : "");
	return 0;
}
