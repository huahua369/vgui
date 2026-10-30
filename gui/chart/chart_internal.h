#pragma once
/*
 * chart_internal.h - C++17 implementation details shared by vgui_chart.
 *
 * Nothing in this header is public.  Public consumers only see
 * include/vgui/chart/vgui_chart.h.
 */
#include <vgui/chart/vgui_chart.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace vgchart {

	inline uint32_t to_ovg(vgui_color c)
	{
		return (c & 0xFF00FF00u) | ((c & 0x00FF0000u) >> 16)
			| ((c & 0x000000FFu) << 16);
	}

	inline vgui_color mix_color(vgui_color a, vgui_color b, float t)
	{
		if (t <= 0.0f) return a;
		if (t >= 1.0f) return b;
		auto mix = [t](unsigned ca, unsigned cb) {
			return (unsigned)(ca + ((int)cb - (int)ca) * t + 0.5f);
		};
		return (mix((a >> 24) & 0xFF, (b >> 24) & 0xFF) << 24)
			| (mix((a >> 16) & 0xFF, (b >> 16) & 0xFF) << 16)
			| (mix((a >> 8) & 0xFF, (b >> 8) & 0xFF) << 8)
			| mix(a & 0xFF, b & 0xFF);
	}

	inline vgui_color with_alpha(vgui_color c, unsigned alpha)
	{
		return (c & 0x00FFFFFFu) | ((vgui_color)(alpha & 0xFF) << 24);
	}

	inline vgui_color ensure_color(vgui_color c, vgui_color fallback)
	{
		return c ? c : fallback;
	}

	inline float ui_scale(vgui_widget* widget)
	{
		vgui_context* ctx = widget ? vgui_widget_get_context(widget) : nullptr;
		if (!ctx) return 1.0f;
		const float s = vgui_context_get_ui_scale(ctx);
		return (std::isfinite(s) && s > 0.0f) ? s : 1.0f;
	}

	inline bool finite(double v)
	{
		return std::isfinite(v);
	}

	inline bool finite(float v)
	{
		return std::isfinite(v);
	}

	/* ---------------------------------------------------------------- axis */

	struct axis_config {
		int scale = VGUI_CHART_SCALE_LINEAR;
		double min = 0.0;
		double max = 0.0;
		int tick_count = 5;
		int include_zero = 0;
		int grid = 1;
		std::string label;
		std::string unit;
		std::string pattern;
		vgui_chart_format_fn format = nullptr;
		void* format_user = nullptr;
	};

	void axis_from_desc(axis_config& out, const vgui_chart_axis_desc* desc);
	void format_value(const axis_config& axis, double value, std::string& out);
	void format_duration(double seconds, std::string& out);
	/* Returns true when pattern contains exactly one floating point conversion. */
	bool format_pattern_safe(const char* pattern);
	/* Applies a validated printf-style pattern to value. */
	bool try_format_pattern(const std::string& pattern, double value,
		std::string& out);

	struct tick_value {
		double value = 0.0;
		float pos = 0.0f;
		std::string text;
		bool label = true;
	};

	/* ---------------------------------------------------------------- series */

	struct series_data {
		vgui_series_id id = 0;
		std::string key;
		std::string name;
		std::string unit;
		std::string stack_group;
		int kind = VGUI_CHART_LINE;
		int axis = 0;
		vgui_color color = 0;
		float line_width = 2.0f;
		float fill_alpha = 0.15f;
		bool visible = true;
		bool stacked = false;
		std::vector<vgui_chart_point> storage;
		size_t head = 0;
		size_t ring_capacity = 0;
		uint64_t revision = 1;

		size_t size() const noexcept { return storage.size(); }
		bool empty() const noexcept { return storage.empty(); }

		const vgui_chart_point& at(size_t i) const noexcept
		{
			if (ring_capacity && !storage.empty())
				return storage[(head + i) % storage.size()];
			return storage[i];
		}

		void normalize()
		{
			if (!ring_capacity || head == 0 || storage.empty())
			{
				head = 0;
				return;
			}
			std::vector<vgui_chart_point> ordered(storage.size());
			for (size_t i = 0; i < storage.size(); ++i)
				ordered[i] = storage[(head + i) % storage.size()];
			storage.swap(ordered);
			head = 0;
		}

		double x_at(size_t i) const noexcept { return at(i).x; }
		double y_at(size_t i) const noexcept { return at(i).y; }
	};

	struct chart_vertex {
		float x = 0.0f;
		float y = 0.0f;
		/* bit 0: start a new sub-path before this vertex. */
		unsigned char flags = 0;
	};

	struct bar_bucket {
		float x = 0.0f;
		double value = 0.0;
		int column = 0;
	};

	struct series_cache {
		uint64_t revision = 0;
		double x0 = 0.0;
		double x1 = 0.0;
		float plot_w = 0.0f;
		int decimation = 0;
		bool valid = false;
		size_t visible_points = 0;
		size_t drawn_points = 0;
		std::vector<chart_vertex> line;
		std::vector<bar_bucket> bars;
	};

	/* --------------------------------------------------------------- layout */

	struct legend_hit {
		float x = 0.0f;
		float y = 0.0f;
		float w = 0.0f;
		float h = 0.0f;
		size_t series_index = 0;
	};

	struct chart_layout {
		bool valid = false;
		bool empty = true;
		float x = 0.0f;
		float y = 0.0f;
		float w = 0.0f;
		float h = 0.0f;
		float plot_x = 0.0f;
		float plot_y = 0.0f;
		float plot_w = 0.0f;
		float plot_h = 0.0f;
		float content_x = 0.0f;
		float content_y = 0.0f;
		float content_w = 0.0f;
		float content_h = 0.0f;
		float title_x = 0.0f;
		float title_y = 0.0f;
		float title_h = 0.0f;
		float legend_x = 0.0f;
		float legend_y = 0.0f;
		float legend_h = 0.0f;
		float title_font = 13.0f;
		float axis_font = 10.0f;
		float legend_font = 10.0f;
		double x0 = 0.0;
		double x1 = 1.0;
		double y0 = 0.0;
		double y1 = 1.0;
		std::vector<tick_value> xticks;
		std::vector<tick_value> yticks;
		std::vector<legend_hit> legend;
	};

	/* ---------------------------------------------------------------- chart */

	struct chart_impl {
		vgui_widget* widget = nullptr;
		vgui_context* ctx = nullptr;
		std::string title;
		std::string last_error;
		std::vector<std::unique_ptr<series_data>> series;
		vgui_series_id next_id = 1;
		axis_config x_axis;
		axis_config y_axis;
		vgui_chart_options options = {};
		vgui_chart_theme theme = {};
		bool theme_custom = false;
		std::vector<vgui_color> palette;

		bool manual_view = false;
		double view_x0 = 0.0;
		double view_x1 = 0.0;
		bool follow = false;

		bool hover_valid = false;
		double hover_x = 0.0;
		float hover_px = 0.0f;
		float hover_py = 0.0f;
		int hover_series = -1;
		size_t hover_index = 0;

		bool panning = false;
		float pan_last_x = 0.0f;
		double pan_x0 = 0.0;
		double pan_x1 = 0.0;

		chart_layout layout;
		std::vector<series_cache> cache;
		vgui_chart_stats stats = {};
	};

	/* ---------------------------------------------------------------- gauge */

	struct gauge_zone_data {
		double from = 0.0;
		double to = 0.0;
		vgui_color color = 0;
		std::string label;
	};

	struct gauge_impl {
		vgui_widget* widget = nullptr;
		vgui_context* ctx = nullptr;
		double min = 0.0;
		double max = 100.0;
		double start_angle = 135.0;
		double sweep = 270.0;
		bool show_ticks = true;
		bool show_value = true;
		std::string label;
		std::string unit;
		std::string pattern;
		std::string last_error;
		vgui_chart_format_fn format = nullptr;
		void* format_user = nullptr;
		std::vector<gauge_zone_data> zones;
		double value = 0.0;
		bool has_value = true;
		vgui_chart_theme theme = {};
		bool theme_custom = false;
	};

	/* ------------------------------------------------------------ dashboard */

	enum class dashboard_card_kind {
		kpi,
		chart,
		gauge
	};

	struct dashboard_card {
		dashboard_card_kind kind = dashboard_card_kind::kpi;
		std::string id;
		std::string title;
		std::string value_key;
		std::string delta_key;
		std::string status_key;
		std::string text_key;
		std::string format;
		std::string unit;
		size_t json_index = 0;
		vgui_widget* panel = nullptr;
		vgui_widget* title_label = nullptr;
		vgui_widget* value_label = nullptr;
		vgui_widget* unit_label = nullptr;
		vgui_widget* delta_label = nullptr;
		vgui_widget* subtitle_label = nullptr;
		vgui_widget* status_label = nullptr;
		vgui_chart* chart = nullptr;
		vgui_gauge* gauge = nullptr;
		int status = VGUI_CHART_STATUS_NORMAL;
		bool has_value = false;
		double last_value = 0.0;
	};

	/* --------------------------------------------------------------- shared */

	void derive_theme(vgui_context* ctx, vgui_chart_theme* out);
	void fill_theme_from(const vgui_theme* theme, vgui_chart_theme* out);
	vgui_color palette_color(const chart_impl* chart, size_t index);
	std::string sanitize_utf8(const char* text);
	void set_chart_error(chart_impl* chart, const char* text);
	void set_gauge_error(gauge_impl* gauge, const char* text);

	void chart_draw(vgui_widget* widget, ovg_ctx_cb* cb, rvg_t* vg,
		float x, float y, float w, float h, float dt, void* user);
	int chart_event(vgui_widget* widget, const vgui_event* event, void* user);
	void chart_size(vgui_widget* widget, float* out_w, float* out_h, void* user);
	void chart_invalidate(chart_impl* chart);
	/* Computes the layout, decimation caches and render statistics. */
	void chart_prepare(chart_impl* chart, float x, float y, float w, float h);

	void gauge_draw(vgui_widget* widget, ovg_ctx_cb* cb, rvg_t* vg,
		float x, float y, float w, float h, float dt, void* user);
	void gauge_size(vgui_widget* widget, float* out_w, float* out_h, void* user);
	void gauge_destroy_impl(gauge_impl* gauge);

	/* Implementation used by both standalone charts and dashboard export. */
	int chart_export_csv_impl(vgui_chart* chart, const char* card_key,
		uint32_t flags, vgui_chart_write_fn write, void* user);

} /* namespace vgchart */

/* Opaque public handles are concrete only inside the module. */
struct vgui_chart : vgchart::chart_impl {
};

struct vgui_gauge : vgchart::gauge_impl {
};

struct vgui_dashboard {
	vgui_widget* parent = nullptr;
	vgui_widget* root = nullptr;
	vgui_widget* title_label = nullptr;
	vgui_context* ctx = nullptr;
	std::vector<vgchart::dashboard_card> cards;
	std::vector<vgui_chart*> charts;
	std::vector<vgui_gauge*> gauges;
	std::string title;
	std::string config_json;
	std::string last_error;
	float layout_gap = 10.0f;
	float layout_padding = 12.0f;
};
