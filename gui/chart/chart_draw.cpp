#include "chart_internal.h"

#include "tools/tools.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace vgchart {

	namespace {

		const char* tr(vgui_context* ctx, const char* key, const char* fallback)
		{
			if (!ctx) return fallback;
			const char* value = vgui_context_translate(ctx, key);
			if (!value || !*value || std::strcmp(value, key) == 0)
				return fallback;
			return value;
		}

		float text_width(vgui_widget* widget, const std::string& text, float font)
		{
			if (text.empty()) return 0.0f;
			if (widget)
			{
				const float w = vgui_widget_measure_text_sized(widget,
					text.c_str(), font);
				if (w > 0.0f) return w;
			}
			return (float)text.size() * font * 0.5f;
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

		void draw_line(ovg_ctx_cb* cb, rvg_t* vg, float x0, float y0,
			float x1, float y1, vgui_color color, float width)
		{
			cb->new_path(vg);
			cb->move_to(vg, x0, y0);
			cb->line_to(vg, x1, y1);
			cb->set_line_width(vg, std::max(1.0f, width));
			cb->set_line_cap(vg, VG_LINE_CAP_BUTT);
			cb->set_line_join(vg, VG_LINE_JOIN_MITER);
			cb->set_source_color(vg, to_ovg(color));
			cb->stroke(vg);
		}

		struct clip_guard {
			ovg_ctx_cb* cb;
			rvg_t* vg;
			int previous[4] = {};
			bool saved = false;

			clip_guard(ovg_ctx_cb* c, rvg_t* v, float x, float y,
				float w, float h) : cb(c), vg(v)
			{
				if (!cb) return;
				if (cb->get_clip_rect)
				{
					cb->get_clip_rect(vg, previous);
					saved = true;
				}
				cb->clip_rect(vg, (int)std::floor(x), (int)std::floor(y),
					(int)std::ceil(w), (int)std::ceil(h));
			}

			~clip_guard()
			{
				if (!cb) return;
				if (saved && cb->set_clip_rect)
					cb->set_clip_rect(vg, previous);
				else
					cb->clip_rect(vg, 0, 0, -1, -1);
			}
		};

		float map_x(const chart_layout& layout, double x)
		{
			return layout.plot_x + (float)((x - layout.x0)
				/ (layout.x1 - layout.x0)) * layout.plot_w;
		}

		float map_y(const chart_layout& layout, double y)
		{
			return layout.plot_y + (float)((layout.y1 - y)
				/ (layout.y1 - layout.y0)) * layout.plot_h;
		}

		vgui_color series_color(const chart_impl* chart, size_t index)
		{
			if (index < chart->series.size())
			{
				const vgui_color custom = chart->series[index]->color;
				if (custom)
					return custom;
			}
			return palette_color(chart, index);
		}

		void paint_segment_fill(ovg_ctx_cb* cb, rvg_t* vg,
			const series_cache& cache, size_t begin, size_t end,
			float baseline, vgui_color color)
		{
			if (end - begin < 2) return;
			cb->new_path(vg);
			cb->move_to(vg, cache.line[begin].x, cache.line[begin].y);
			for (size_t i = begin + 1; i < end; ++i)
				cb->line_to(vg, cache.line[i].x, cache.line[i].y);
			cb->line_to(vg, cache.line[end - 1].x, baseline);
			cb->line_to(vg, cache.line[begin].x, baseline);
			cb->close_path(vg);
			cb->set_source_color(vg, to_ovg(color));
			cb->fill(vg);
		}

		void paint_segment_stroke(ovg_ctx_cb* cb, rvg_t* vg,
			const series_cache& cache, size_t begin, size_t end,
			vgui_color color, float width)
		{
			if (end <= begin) return;
			if (end - begin == 1)
			{
				ovg_tools::fill_circle(cb, vg, cache.line[begin].x,
					cache.line[begin].y, std::max(1.5f, width),
					to_ovg(color));
				return;
			}
			cb->new_path(vg);
			cb->move_to(vg, cache.line[begin].x, cache.line[begin].y);
			for (size_t i = begin + 1; i < end; ++i)
				cb->line_to(vg, cache.line[i].x, cache.line[i].y);
			cb->set_line_width(vg, std::max(1.0f, width));
			cb->set_line_cap(vg, VG_LINE_CAP_ROUND);
			cb->set_line_join(vg, VG_LINE_JOIN_ROUND);
			cb->set_source_color(vg, to_ovg(color));
			cb->stroke(vg);
		}

		void paint_area(chart_impl* chart, ovg_ctx_cb* cb, rvg_t* vg,
			size_t index, const chart_layout& layout)
		{
			const series_data& series = *chart->series[index];
			const series_cache& cache = chart->cache[index];
			if (cache.line.size() < 2) return;
			const float scale = ui_scale(chart->widget);
			const vgui_color color = with_alpha(series_color(chart, index),
				(unsigned)std::clamp((int)std::lround(series.fill_alpha * 255.0f),
					0, 255));
			float baseline = layout.plot_y + layout.plot_h;
			if (layout.y0 <= 0.0 && 0.0 <= layout.y1)
				baseline = map_y(layout, 0.0);
			baseline = std::clamp(baseline, layout.plot_y, layout.plot_y + layout.plot_h);
			size_t i = 0;
			while (i < cache.line.size())
			{
				const size_t begin = i;
				++i;
				while (i < cache.line.size() && (cache.line[i].flags & 1) == 0)
					++i;
				paint_segment_fill(cb, vg, cache, begin, i, baseline, color);
			}
			(void)scale;
		}

		void paint_line(chart_impl* chart, ovg_ctx_cb* cb, rvg_t* vg,
			size_t index)
		{
			const series_data& series = *chart->series[index];
			const series_cache& cache = chart->cache[index];
			if (cache.line.empty()) return;
			const float scale = ui_scale(chart->widget);
			const float width = std::max(1.0f, series.line_width * scale);
			const vgui_color color = series_color(chart, index);
			size_t i = 0;
			while (i < cache.line.size())
			{
				const size_t begin = i;
				++i;
				while (i < cache.line.size() && (cache.line[i].flags & 1) == 0)
					++i;
				paint_segment_stroke(cb, vg, cache, begin, i, color, width);
			}
		}

		void paint_bars(chart_impl* chart, ovg_ctx_cb* cb, rvg_t* vg,
			const chart_layout& layout)
		{
			const float scale = ui_scale(chart->widget);
			const size_t count = chart->series.size();
			std::vector<int> slot(count, -1);
			std::map<std::string, int> stack_slots;
			int slots = 0;
			size_t max_columns = 0;
			for (size_t i = 0; i < count; ++i)
			{
				const series_data& series = *chart->series[i];
				if (!series.visible || series.kind != VGUI_CHART_BAR
					|| i >= chart->cache.size())
					continue;
				max_columns = std::max(max_columns, chart->cache[i].bars.size());
				if (series.stacked)
				{
					const std::string group = series.stack_group.empty()
						? std::string("__default") : series.stack_group;
					auto found = stack_slots.find(group);
					if (found == stack_slots.end())
					{
						stack_slots[group] = slots;
						slot[i] = slots;
						++slots;
					}
					else
						slot[i] = found->second;
				}
				else
					slot[i] = slots++;
			}
			if (max_columns == 0 || slots == 0) return;
			const float span = layout.plot_w / (float)max_columns;
			const float slot_width = span / (float)slots;
			const float bar_width = std::max(1.0f, slot_width * 0.72f);
			const int columns = (int)max_columns + 2;

			std::map<std::string, std::pair<std::vector<double>, std::vector<double>>> stacks;
			for (size_t i = 0; i < count; ++i)
			{
				const series_data& series = *chart->series[i];
				if (!series.visible || series.kind != VGUI_CHART_BAR
					|| i >= chart->cache.size())
					continue;
				const series_cache& cache = chart->cache[i];
				const vgui_color color = series_color(chart, i);
				const float offset = ((float)slot[i] - (float)(slots - 1) * 0.5f)
					* slot_width;
				std::vector<double>* pos = nullptr;
				std::vector<double>* neg = nullptr;
				if (series.stacked)
				{
					const std::string group = series.stack_group.empty()
						? std::string("__default") : series.stack_group;
					auto& entry = stacks[group];
					if ((int)entry.first.size() < columns)
					{
						entry.first.resize((size_t)columns, 0.0);
						entry.second.resize((size_t)columns, 0.0);
					}
					pos = &entry.first;
					neg = &entry.second;
				}
				for (const bar_bucket& bucket : cache.bars)
				{
					double base = 0.0;
					if (pos && neg)
					{
						const int col = std::clamp(bucket.column, 0, columns - 1);
						base = bucket.value >= 0.0 ? (*pos)[(size_t)col]
							: (*neg)[(size_t)col];
						if (bucket.value >= 0.0) (*pos)[(size_t)col] += bucket.value;
						else (*neg)[(size_t)col] += bucket.value;
					}
					const double top_value = base + bucket.value;
					float top = map_y(layout, top_value);
					float bottom = map_y(layout, base);
					if (top > bottom) std::swap(top, bottom);
					if (bottom - top < 1.0f) bottom = top + 1.0f;
					const float x = bucket.x + offset - bar_width * 0.5f;
					ovg_tools::fill_rect(cb, vg, x, top, bar_width,
						bottom - top, to_ovg(with_alpha(color, 0xD8)));
				}
			}
		}

		void paint_axes(chart_impl* chart, ovg_ctx_cb* cb, rvg_t* vg,
			const chart_layout& layout)
		{
			const float scale = ui_scale(chart->widget);
			const float tick = 4.0f * scale;
			const vgui_color axis = chart->theme.axis;
			draw_line(cb, vg, layout.plot_x, layout.plot_y + layout.plot_h,
				layout.plot_x + layout.plot_w, layout.plot_y + layout.plot_h,
				axis, 1.0f * scale);
			draw_line(cb, vg, layout.plot_x, layout.plot_y,
				layout.plot_x, layout.plot_y + layout.plot_h, axis, 1.0f * scale);

			for (const tick_value& t : layout.xticks)
			{
				if (t.pos < layout.plot_x - 1.0f
					|| t.pos > layout.plot_x + layout.plot_w + 1.0f)
					continue;
				draw_line(cb, vg, t.pos, layout.plot_y + layout.plot_h,
					t.pos, layout.plot_y + layout.plot_h + tick, axis,
					1.0f * scale);
				if (!t.label) continue;
				const float width = std::max(40.0f * scale,
					text_width(chart->widget, t.text, layout.axis_font) + 8.0f * scale);
				draw_text(chart->widget, cb, vg, t.text,
					t.pos - width * 0.5f, layout.plot_y + layout.plot_h + tick + 1.0f * scale,
					width, line_height(chart->widget, layout.axis_font),
					chart->theme.muted_text, 0.5f, 0.0f, layout.axis_font);
			}

			for (const tick_value& t : layout.yticks)
			{
				if (t.pos < layout.plot_y - 1.0f
					|| t.pos > layout.plot_y + layout.plot_h + 1.0f)
					continue;
				draw_line(cb, vg, layout.plot_x - tick, t.pos,
					layout.plot_x, t.pos, axis, 1.0f * scale);
				if (!t.label) continue;
				const float width = layout.plot_x - layout.content_x - 6.0f * scale;
				draw_text(chart->widget, cb, vg, t.text,
					layout.content_x, t.pos - line_height(chart->widget, layout.axis_font) * 0.5f,
					width, line_height(chart->widget, layout.axis_font),
					chart->theme.muted_text, 1.0f, 0.0f, layout.axis_font);
			}

			if (!chart->x_axis.label.empty())
			{
				const float h = line_height(chart->widget, layout.axis_font);
				draw_text(chart->widget, cb, vg, chart->x_axis.label,
					layout.plot_x, layout.y + layout.h - h - 2.0f * scale,
					layout.plot_w, h, chart->theme.muted_text, 0.5f, 0.5f,
					layout.axis_font);
			}
			if (!chart->y_axis.label.empty())
			{
				const float h = line_height(chart->widget, layout.axis_font);
				draw_text(chart->widget, cb, vg, chart->y_axis.label,
					layout.plot_x + 4.0f * scale, layout.plot_y + 2.0f * scale,
					layout.plot_w - 8.0f * scale, h,
					chart->theme.muted_text, 0.0f, 0.0f, layout.axis_font);
			}
		}

		void paint_title(chart_impl* chart, ovg_ctx_cb* cb, rvg_t* vg,
			const chart_layout& layout)
		{
			if (chart->title.empty() || layout.title_h <= 0.0f) return;
			draw_text(chart->widget, cb, vg, chart->title,
				layout.title_x, layout.title_y, layout.content_w,
				layout.title_h, chart->theme.text, 0.0f, 0.5f,
				layout.title_font);
		}

		void paint_legend(chart_impl* chart, ovg_ctx_cb* cb, rvg_t* vg,
			const chart_layout& layout)
		{
			if (!chart->options.legend || layout.legend_h <= 0.0f)
				return;
			const float scale = ui_scale(chart->widget);
			const float swatch = 14.0f * scale;
			const float row_h = line_height(chart->widget, layout.legend_font);
			for (const legend_hit& hit : layout.legend)
			{
				if (hit.series_index >= chart->series.size()) continue;
				const series_data& series = *chart->series[hit.series_index];
				const vgui_color color = series_color(chart, hit.series_index);
				const float cy = hit.y + hit.h * 0.5f;
				if (series.kind == VGUI_CHART_BAR)
				{
					ovg_tools::fill_round_rect(cb, vg, hit.x, cy - 3.0f * scale,
						swatch, 6.0f * scale, 1.0f * scale, to_ovg(color));
				}
				else
				{
					draw_line(cb, vg, hit.x, cy, hit.x + swatch, cy, color,
						std::max(1.5f, 2.0f * scale));
					if (series.kind == VGUI_CHART_AREA)
						ovg_tools::fill_rect(cb, vg, hit.x, cy - 2.0f * scale,
							swatch, 4.0f * scale,
							to_ovg(with_alpha(color, 0x50)));
				}
				draw_text(chart->widget, cb, vg, series.name,
					hit.x + swatch + 4.0f * scale, hit.y,
					hit.w - swatch - 4.0f * scale, row_h,
					chart->theme.muted_text, 0.0f, 0.5f,
					layout.legend_font);
			}
		}

		void paint_grid(chart_impl* chart, ovg_ctx_cb* cb, rvg_t* vg,
			const chart_layout& layout)
		{
			const float scale = ui_scale(chart->widget);
			const vgui_color grid = with_alpha(chart->theme.grid, 0xB0);
			if (chart->x_axis.grid)
			{
				for (const tick_value& t : layout.xticks)
				{
					if (t.pos < layout.plot_x || t.pos > layout.plot_x + layout.plot_w)
						continue;
					draw_line(cb, vg, t.pos, layout.plot_y, t.pos,
						layout.plot_y + layout.plot_h, grid, 1.0f * scale);
				}
			}
			if (chart->y_axis.grid)
			{
				for (const tick_value& t : layout.yticks)
				{
					if (t.pos < layout.plot_y || t.pos > layout.plot_y + layout.plot_h)
						continue;
					draw_line(cb, vg, layout.plot_x, t.pos,
						layout.plot_x + layout.plot_w, t.pos, grid,
						1.0f * scale);
				}
			}
			if (layout.y0 < 0.0 && 0.0 < layout.y1)
			{
				const float zero = map_y(layout, 0.0);
				draw_line(cb, vg, layout.plot_x, zero,
					layout.plot_x + layout.plot_w, zero,
					with_alpha(chart->theme.axis, 0xE0), 1.0f * scale);
			}
		}

		void paint_tooltip(chart_impl* chart, ovg_ctx_cb* cb, rvg_t* vg,
			const chart_layout& layout)
		{
			if (!chart->options.tooltip || !chart->hover_valid)
				return;
			if (layout.empty || !(layout.x1 > layout.x0))
				return;
			const float scale = ui_scale(chart->widget);
			const float px = map_x(layout, chart->hover_x);
			if (px < layout.plot_x - 2.0f || px > layout.plot_x + layout.plot_w + 2.0f)
				return;

			if (chart->options.crosshair)
			{
				draw_line(cb, vg, px, layout.plot_y, px,
					layout.plot_y + layout.plot_h,
					with_alpha(chart->theme.crosshair, 0xD0), 1.0f * scale);
			}

			struct row {
				vgui_color color;
				std::string text;
			};
			std::vector<row> rows;
			const float font = layout.axis_font;
			const float row_h = line_height(chart->widget, font) + 2.0f * scale;
			const float swatch = 8.0f * scale;
			const float pad = 6.0f * scale;
			float text_w = 0.0f;
			for (size_t i = 0; i < chart->series.size() && rows.size() < 8; ++i)
			{
				const series_data& series = *chart->series[i];
				if (!series.visible || series.empty()) continue;
				size_t lo = 0;
				size_t hi = series.size();
				while (lo < hi)
				{
					const size_t mid = lo + (hi - lo) / 2;
					if (series.at(mid).x < chart->hover_x) lo = mid + 1;
					else hi = mid;
				}
				size_t index = lo;
				if (index >= series.size()) index = series.size() - 1;
				if (index > 0)
				{
					const double left = std::fabs(series.at(index - 1).x - chart->hover_x);
					const double right = std::fabs(series.at(index).x - chart->hover_x);
					if (left < right) --index;
				}
				const vgui_chart_point& point = series.at(index);
				if (!std::isfinite(point.y)) continue;
				std::string value;
				format_value(chart->y_axis, point.y, value);
				const std::string unit = !series.unit.empty()
					? series.unit : chart->y_axis.unit;
				std::string text = series.name;
				text += "  ";
				text += value;
				if (!unit.empty())
				{
					text += " ";
					text += unit;
				}
				text_w = std::max(text_w, text_width(chart->widget, text, font));
				rows.push_back({ series_color(chart, i), std::move(text) });

				const float my = map_y(layout, point.y);
				if (my >= layout.plot_y && my <= layout.plot_y + layout.plot_h)
					ovg_tools::fill_circle(cb, vg, px, my, 3.0f * scale,
						to_ovg(series_color(chart, i)));
			}
			if (rows.empty()) return;

			const float box_w = std::max(130.0f * scale,
				text_w + swatch + pad * 3.0f);
			const float box_h = (float)rows.size() * row_h + pad * 2.0f;
			float bx = chart->hover_px + 12.0f * scale;
			if (bx + box_w > layout.content_x + layout.content_w)
				bx = chart->hover_px - box_w - 12.0f * scale;
			bx = std::clamp(bx, layout.content_x,
				layout.content_x + layout.content_w - box_w);
			float by = chart->hover_py + 12.0f * scale;
			if (by + box_h > layout.content_y + layout.content_h)
				by = chart->hover_py - box_h - 12.0f * scale;
			by = std::clamp(by, layout.content_y,
				layout.content_y + layout.content_h - box_h);
			if (bx < layout.content_x || by < layout.content_y) return;

			ovg_tools::fill_round_rect(cb, vg, bx, by, box_w, box_h,
				4.0f * scale, to_ovg(with_alpha(chart->theme.tooltip_bg, 0xF2)));
			ovg_tools::stroke_round_rect(cb, vg, bx, by, box_w, box_h,
				4.0f * scale, to_ovg(chart->theme.tooltip_border), 1.0f * scale);
			for (size_t i = 0; i < rows.size(); ++i)
			{
				const float y = by + pad + (float)i * row_h;
				ovg_tools::fill_round_rect(cb, vg, bx + pad, y + row_h * 0.5f - 3.0f * scale,
					swatch, 6.0f * scale, 1.5f * scale,
					to_ovg(rows[i].color));
				draw_text(chart->widget, cb, vg, rows[i].text,
					bx + pad + swatch + pad, y,
					box_w - pad * 3.0f - swatch, row_h,
					chart->theme.tooltip_text, 0.0f, 0.5f, font);
			}
		}

		void paint_empty(chart_impl* chart, ovg_ctx_cb* cb, rvg_t* vg,
			const chart_layout& layout)
		{
			bool empty = layout.empty;
			if (!empty)
			{
				empty = true;
				for (const auto& series : chart->series)
				{
					if (series && series->visible && !series->empty())
					{
						empty = false;
						break;
					}
				}
			}
			if (!empty) return;
			std::string text = chart->last_error.empty()
				? tr(chart->ctx, "chart.empty", "No data")
				: chart->last_error;
			if (text.size() > 160)
				text = text.substr(0, 160);
			const float font = std::max(10.0f,
				layout.axis_font > 0.0f ? layout.axis_font : 11.0f);
			const float px = layout.plot_w > 2.0f ? layout.plot_x : layout.content_x;
			const float py = layout.plot_h > 2.0f ? layout.plot_y : layout.content_y;
			const float pw = layout.plot_w > 2.0f ? layout.plot_w : layout.content_w;
			const float ph = layout.plot_h > 2.0f ? layout.plot_h : layout.content_h;
			draw_text(chart->widget, cb, vg, text, px, py, pw, ph,
				chart->theme.muted_text, 0.5f, 0.5f, font);
		}

	} /* namespace */

	void chart_draw(vgui_widget* widget, ovg_ctx_cb* cb, rvg_t* vg,
		float x, float y, float w, float h, float dt, void* user)
	{
		(void)dt;
		chart_impl* chart = static_cast<chart_impl*>(user);
		if (!chart || !widget || !cb || !vg)
			return;

		chart_prepare(chart, x, y, w >= 1.0f ? w : 1.0f,
			h >= 1.0f ? h : 1.0f);
		const chart_layout& layout = chart->layout;
		if (!layout.valid)
		{
			paint_empty(chart, cb, vg, layout);
			return;
		}

		const float scale = ui_scale(widget);
		ovg_tools::fill_round_rect(cb, vg, layout.plot_x, layout.plot_y,
			layout.plot_w, layout.plot_h, 2.0f * scale,
			to_ovg(chart->theme.plot_bg));

		{
			clip_guard clip(cb, vg, layout.plot_x, layout.plot_y,
				layout.plot_w, layout.plot_h);
			paint_grid(chart, cb, vg, layout);
			for (size_t i = 0; i < chart->series.size(); ++i)
			{
				if (!chart->series[i]->visible
					|| chart->series[i]->kind != VGUI_CHART_AREA)
					continue;
				paint_area(chart, cb, vg, i, layout);
			}
			paint_bars(chart, cb, vg, layout);
			for (size_t i = 0; i < chart->series.size(); ++i)
			{
				if (!chart->series[i]->visible
					|| chart->series[i]->kind == VGUI_CHART_BAR)
					continue;
				paint_line(chart, cb, vg, i);
			}
			paint_tooltip(chart, cb, vg, layout);
		}

		paint_axes(chart, cb, vg, layout);
		paint_title(chart, cb, vg, layout);
		paint_legend(chart, cb, vg, layout);
		paint_empty(chart, cb, vg, layout);
	}

	void chart_size(vgui_widget* widget, float* out_w, float* out_h, void* user)
	{
		(void)widget;
		(void)user;
		if (out_w) *out_w = 360.0f;
		if (out_h) *out_h = 220.0f;
	}

	void gauge_size(vgui_widget* widget, float* out_w, float* out_h, void* user)
	{
		(void)widget;
		(void)user;
		if (out_w) *out_w = 220.0f;
		if (out_h) *out_h = 180.0f;
	}

} /* namespace vgchart */
