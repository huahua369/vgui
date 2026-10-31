#include "chart_internal.h"

#include "tools/tools.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace vgchart {

	namespace {

		constexpr float kPi = 3.14159265358979323846f;

		float logical_font(vgui_context* ctx)
		{
			if (!ctx) return 13.0f;
			const float scale = vgui_context_get_ui_scale(ctx);
			const float effective = vgui_context_get_font_size(ctx);
			if (!(scale > 0.0f)) return 13.0f;
			const float value = effective / scale;
			return value > 4.0f ? value : 13.0f;
		}

		float line_height(vgui_widget* widget, float font)
		{
			if (widget)
			{
				const float h = vgui_widget_get_line_height_sized(widget, font);
				if (h > 0.0f) return h;
			}
			return font * 1.35f;
		}

		void draw_text(vgui_widget* widget, ovg_ctx_cb* cb, rvg_t* vg,
			const std::string& text, float x, float y, float w, float h,
			vgui_color color, float align_x, float align_y, float font)
		{
			if (text.empty() || w <= 0.0f || h <= 0.0f) return;
			vgui_custom_draw_text_sized(widget, cb, vg, text.c_str(),
				x, y, w, h, color, align_x, align_y, font);
		}

		void stroke_arc(ovg_ctx_cb* cb, rvg_t* vg, float cx, float cy,
			float radius, float a0, float a1, float width, vgui_color color)
		{
			if (!(a1 > a0)) return;
			cb->new_path(vg);
			cb->arc(vg, cx, cy, radius, a0 * kPi / 180.0f,
				a1 * kPi / 180.0f);
			cb->set_line_width(vg, std::max(1.0f, width));
			cb->set_line_cap(vg, VG_LINE_CAP_ROUND);
			cb->set_line_join(vg, VG_LINE_JOIN_ROUND);
			cb->set_source_color(vg, to_ovg(color));
			cb->stroke(vg);
		}

		vgui_color zone_color(const vgui_chart_theme& theme,
			const gauge_zone_data& zone)
		{
			if (zone.color) return zone.color;
			const double span = zone.to - zone.from;
			if (span < 0.0) return theme.negative;
			return theme.series[0];
		}

	} /* namespace */

	void gauge_destroy_impl(gauge_impl* gauge)
	{
		if (!gauge) return;
		if (gauge->widget)
		{
			vgui_custom_set_draw(gauge->widget, nullptr, nullptr);
			vgui_custom_set_size(gauge->widget, nullptr, nullptr);
			vgui_widget_destroy(gauge->widget);
			gauge->widget = nullptr;
		}
		delete gauge;
	}

	void gauge_draw(vgui_widget* widget, ovg_ctx_cb* cb, rvg_t* vg,
		float x, float y, float w, float h, float dt, void* user)
	{
		(void)dt;
		gauge_impl* gauge = static_cast<gauge_impl*>(user);
		if (!gauge || !widget || !cb || !vg) return;
		if (w < 24.0f || h < 24.0f) return;

		if (!gauge->theme_custom)
			derive_theme(gauge->ctx, &gauge->theme);
		const vgui_chart_theme& theme = gauge->theme;
		const float scale = ui_scale(widget);
		const float font = logical_font(gauge->ctx);

		const float pad = 8.0f * scale;
		const float avail_w = w - pad * 2.0f;
		const float avail_h = h - pad * 2.0f;
		if (avail_w < 20.0f || avail_h < 20.0f) return;

		const float thickness = std::max(5.0f * scale,
			std::min(avail_w, avail_h) * 0.085f);
		const float radius = std::max(8.0f, std::min(avail_w, avail_h) * 0.5f
			- thickness * 0.9f - pad);
		const float cx = x + w * 0.5f;
		const float cy = y + h * 0.52f;
		const float start = gauge->start_angle;
		const float sweep = gauge->sweep;

		ovg_tools::fill_round_rect(cb, vg, x + pad * 0.5f, y + pad * 0.5f,
			w - pad, h - pad, 4.0f * scale,
			to_ovg(with_alpha(theme.plot_bg, 0x90)));

		const vgui_color track = mix_color(theme.axis, theme.plot_bg, 0.55f);
		stroke_arc(cb, vg, cx, cy, radius, start, start + sweep,
			thickness, with_alpha(track, 0xA0));

		if (gauge->max > gauge->min)
		{
			for (const gauge_zone_data& zone : gauge->zones)
			{
				double from = std::clamp(std::min(zone.from, zone.to),
					gauge->min, gauge->max);
				double to = std::clamp(std::max(zone.from, zone.to),
					gauge->min, gauge->max);
				if (!(to > from)) continue;
				const float a0 = start + (float)((from - gauge->min)
					/ (gauge->max - gauge->min)) * sweep;
				const float a1 = start + (float)((to - gauge->min)
					/ (gauge->max - gauge->min)) * sweep;
				stroke_arc(cb, vg, cx, cy, radius + thickness * 0.27f,
					a0, a1, thickness * 0.34f,
					zone_color(theme, zone));
			}
		}

		double shown = gauge->value;
		bool has_value = gauge->has_value && std::isfinite(shown);
		if (has_value)
			shown = std::clamp(shown, gauge->min, gauge->max);
		if (has_value && gauge->max > gauge->min)
		{
			const float angle = start + (float)((shown - gauge->min)
				/ (gauge->max - gauge->min)) * sweep;
			if (angle > start)
				stroke_arc(cb, vg, cx, cy, radius - thickness * 0.16f,
					start, angle, thickness * 0.46f, theme.series[0]);
		}

		if (gauge->show_ticks && gauge->max > gauge->min)
		{
			const int tick_count = 5;
			for (int i = 0; i < tick_count; ++i)
			{
				const double value = gauge->min
					+ (gauge->max - gauge->min) * (double)i / (double)(tick_count - 1);
				const float angle = (start + (float)((value - gauge->min)
					/ (gauge->max - gauge->min)) * sweep) * kPi / 180.0f;
				const float r0 = radius - thickness * 0.72f;
				const float r1 = radius - thickness * 0.42f;
				const float px0 = cx + std::cos(angle) * r0;
				const float py0 = cy + std::sin(angle) * r0;
				const float px1 = cx + std::cos(angle) * r1;
				const float py1 = cy + std::sin(angle) * r1;
				cb->new_path(vg);
				cb->move_to(vg, px0, py0);
				cb->line_to(vg, px1, py1);
				cb->set_line_width(vg, std::max(1.0f, scale));
				cb->set_source_color(vg, to_ovg(with_alpha(theme.axis, 0xD0)));
				cb->stroke(vg);
			}
		}

		std::string value_text;
		if (has_value)
			format_value(axis_config{}, shown, value_text);
		else
			value_text = "--";
		if (has_value && try_format_pattern(gauge->pattern, shown, value_text))
		{
			/* formatted */
		}
		else if (has_value && gauge->format)
		{
			char buffer[96] = {};
			if (gauge->format(gauge->format_user, shown, buffer, sizeof(buffer))
				&& buffer[0])
				value_text = buffer;
		}

		if (gauge->show_value)
		{
			float value_font = std::min(48.0f,
				std::max(16.0f, std::min(avail_w / scale, avail_h / scale) * 0.20f));
			const float value_h = line_height(widget, value_font);
			const vgui_color value_color = has_value ? theme.text : theme.muted_text;
			draw_text(widget, cb, vg, value_text, x + pad,
				cy - value_h * 0.75f, w - pad * 2.0f, value_h,
				value_color, 0.5f, 0.5f, value_font);
			if (!gauge->unit.empty())
			{
				draw_text(widget, cb, vg, gauge->unit, x + pad,
					cy + value_h * 0.28f, w - pad * 2.0f, value_h * 0.7f,
					theme.muted_text, 0.5f, 0.5f, font * 0.9f);
			}
		}

		if (!gauge->label.empty())
		{
			const float label_h = line_height(widget, font);
			draw_text(widget, cb, vg, gauge->label, x + pad,
				y + h - label_h - 4.0f * scale, w - pad * 2.0f, label_h,
				theme.muted_text, 0.5f, 0.5f, font * 0.9f);
		}

		if (gauge->show_ticks && gauge->max > gauge->min)
		{
			std::string min_text;
			std::string max_text;
			format_value(axis_config{}, gauge->min, min_text);
			format_value(axis_config{}, gauge->max, max_text);
			const float tick_font = std::max(9.0f, font * 0.7f);
			const float label_h = line_height(widget, tick_font);
			const float angle0 = (start) * kPi / 180.0f;
			const float angle1 = (start + sweep) * kPi / 180.0f;
			const float r = radius + thickness * 0.8f;
			draw_text(widget, cb, vg, min_text,
				cx + std::cos(angle0) * r - 28.0f * scale,
				cy + std::sin(angle0) * r - label_h * 0.5f,
				56.0f * scale, label_h, theme.muted_text, 0.5f, 0.5f,
				tick_font);
			draw_text(widget, cb, vg, max_text,
				cx + std::cos(angle1) * r - 28.0f * scale,
				cy + std::sin(angle1) * r - label_h * 0.5f,
				56.0f * scale, label_h, theme.muted_text, 0.5f, 0.5f,
				tick_font);
		}
	}

} /* namespace vgchart */

extern "C" {

	void vgui_gauge_options_default(vgui_gauge_options* options)
	{
		if (!options) return;
		std::memset(options, 0, sizeof(*options));
		options->struct_size = (uint32_t)sizeof(*options);
		options->min = 0.0;
		options->max = 100.0;
		options->start_angle = 135.0;
		options->sweep = 270.0;
		options->show_ticks = 1;
		options->show_value = 1;
	}

	vgui_gauge* vgui_gauge_create(vgui_widget* parent)
	{
		if (!parent) return nullptr;
		vgui_gauge* gauge = new (std::nothrow) vgui_gauge();
		if (!gauge) return nullptr;
		gauge->ctx = vgui_widget_get_context(parent);
		gauge->min = 0.0;
		gauge->max = 100.0;
		gauge->start_angle = 135.0;
		gauge->sweep = 270.0;
		gauge->show_ticks = true;
		gauge->show_value = true;
		vgchart::derive_theme(gauge->ctx, &gauge->theme);

		gauge->widget = vgui_widget_add(parent, VGUI_CUSTOM, nullptr);
		if (!gauge->widget)
		{
			delete gauge;
			return nullptr;
		}
		vgui_widget_set_id(gauge->widget, "vgui_gauge");
		vgui_widget_set_a11y_name(gauge->widget, "Gauge");
		vgui_widget_set_a11y_role(gauge->widget, VGUI_A11Y_ROLE_PROGRESS);
		vgui_widget_set_min_size(gauge->widget, 160, 130);
		vgui_widget_set_grow(gauge->widget, 1.0f);
		vgui_custom_set_draw(gauge->widget, &vgchart::gauge_draw, gauge);
		vgui_custom_set_size(gauge->widget, &vgchart::gauge_size, gauge);
		return gauge;
	}

	void vgui_gauge_destroy(vgui_gauge* gauge)
	{
		vgchart::gauge_destroy_impl(gauge);
	}

	vgui_widget* vgui_gauge_widget(vgui_gauge* gauge)
	{
		return gauge ? gauge->widget : nullptr;
	}

	int vgui_gauge_set_options(vgui_gauge* gauge,
		const vgui_gauge_options* options)
	{
		if (!gauge || !options)
		{
			if (gauge) vgchart::set_gauge_error(gauge, "invalid gauge options");
			return 0;
		}
		vgui_gauge_options d = {};
		size_t bytes = sizeof(d);
		if (options->struct_size && options->struct_size < bytes)
			bytes = options->struct_size;
		std::memcpy(&d, options, bytes);

		if (!std::isfinite(d.min) || !std::isfinite(d.max) || !(d.max > d.min))
		{
			vgchart::set_gauge_error(gauge, "gauge max must be greater than min");
			return 0;
		}
		if (!std::isfinite(d.start_angle) || !std::isfinite(d.sweep))
		{
			vgchart::set_gauge_error(gauge, "invalid gauge angles");
			return 0;
		}
		double sweep = std::fabs(d.sweep);
		if (sweep < 10.0) sweep = 10.0;
		if (sweep > 360.0) sweep = 360.0;

		gauge->min = d.min;
		gauge->max = d.max;
		gauge->start_angle = std::fmod(d.start_angle, 360.0);
		if (gauge->start_angle < 0.0) gauge->start_angle += 360.0;
		gauge->sweep = sweep;
		gauge->show_ticks = d.show_ticks != 0;
		gauge->show_value = d.show_value != 0;
		gauge->label = d.label ? d.label : "";
		gauge->unit = d.unit ? d.unit : "";
		gauge->pattern = d.format_pattern ? d.format_pattern : "";
		gauge->format = d.format;
		gauge->format_user = d.format_user;
		gauge->zones.clear();
		if (d.zones && d.zone_count > 0)
		{
			const size_t count = std::min<size_t>(d.zone_count, 32);
			gauge->zones.reserve(count);
			for (size_t i = 0; i < count; ++i)
			{
				const vgui_gauge_zone& zone = d.zones[i];
				if (!std::isfinite(zone.from) || !std::isfinite(zone.to))
					continue;
				vgchart::gauge_zone_data copy;
				copy.from = zone.from;
				copy.to = zone.to;
				copy.color = zone.color;
				copy.label = zone.label ? zone.label : "";
				gauge->zones.push_back(std::move(copy));
			}
		}
		if (gauge->widget)
		{
			vgui_widget_set_a11y_name(gauge->widget,
				gauge->label.empty() ? "Gauge" : gauge->label.c_str());
		}
		return 1;
	}

	int vgui_gauge_set_value(vgui_gauge* gauge, double value)
	{
		if (!gauge)
			return 0;
		if (std::isnan(value))
		{
			gauge->has_value = false;
			gauge->value = value;
			return 1;
		}
		gauge->value = value;
		gauge->has_value = true;
		return 1;
	}

	double vgui_gauge_get_value(vgui_gauge* gauge)
	{
		if (!gauge) return 0.0;
		return gauge->has_value ? gauge->value
			: std::numeric_limits<double>::quiet_NaN();
	}

	int vgui_gauge_set_theme(vgui_gauge* gauge, const vgui_chart_theme* theme)
	{
		if (!gauge)
			return 0;
		if (theme)
		{
			size_t bytes = sizeof(gauge->theme);
			gauge->theme = {};
			if (theme->struct_size && theme->struct_size < bytes)
				bytes = theme->struct_size;
			std::memcpy(&gauge->theme, theme, bytes);
			gauge->theme_custom = true;
		}
		else
		{
			gauge->theme_custom = false;
			vgchart::derive_theme(gauge->ctx, &gauge->theme);
		}
		return 1;
	}

	const char* vgui_gauge_last_error(vgui_gauge* gauge)
	{
		return gauge ? gauge->last_error.c_str() : "";
	}

} /* extern "C" */
