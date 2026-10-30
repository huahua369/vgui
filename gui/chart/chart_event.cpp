#include "chart_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace vgchart {

	namespace {

		void ensure_layout(chart_impl* chart)
		{
			if (!chart || !chart->widget || chart->layout.valid)
				return;
			float x = 0.0f;
			float y = 0.0f;
			float w = 0.0f;
			float h = 0.0f;
			vgui_widget_get_rect(chart->widget, &x, &y, &w, &h);
			if (w > 1.0f && h > 1.0f)
				chart_prepare(chart, x, y, w, h);
		}

		size_t nearest_index(const series_data& series, double x)
		{
			if (series.empty()) return 0;
			size_t lo = 0;
			size_t hi = series.size();
			while (lo < hi)
			{
				const size_t mid = lo + (hi - lo) / 2;
				if (series.at(mid).x < x) lo = mid + 1;
				else hi = mid;
			}
			if (lo >= series.size()) return series.size() - 1;
			if (lo > 0)
			{
				const double left = std::fabs(series.at(lo - 1).x - x);
				const double right = std::fabs(series.at(lo).x - x);
				if (left < right) --lo;
			}
			return lo;
		}

		std::vector<size_t> visible_series(const chart_impl* chart)
		{
			std::vector<size_t> result;
			for (size_t i = 0; i < chart->series.size(); ++i)
			{
				if (chart->series[i] && chart->series[i]->visible
					&& !chart->series[i]->empty())
					result.push_back(i);
			}
			return result;
		}

		void clear_hover(chart_impl* chart)
		{
			chart->hover_valid = false;
			chart->hover_series = -1;
			chart->hover_index = 0;
		}

		void update_hover(chart_impl* chart, float local_x, float local_y,
			float widget_x, float widget_y)
		{
			ensure_layout(chart);
			const chart_layout& layout = chart->layout;
			if (!layout.valid || layout.empty)
			{
				clear_hover(chart);
				return;
			}
			const float wx = local_x + widget_x;
			const float wy = local_y + widget_y;
			if (wx < layout.plot_x || wx > layout.plot_x + layout.plot_w
				|| wy < layout.plot_y - 4.0f
				|| wy > layout.plot_y + layout.plot_h + 4.0f)
			{
				clear_hover(chart);
				return;
			}
			const double data_x = layout.x0
				+ (double)((wx - layout.plot_x) / layout.plot_w)
				* (layout.x1 - layout.x0);

			int best_series = -1;
			size_t best_index = 0;
			float best_distance = 1e30f;
			for (size_t i = 0; i < chart->series.size(); ++i)
			{
				const series_data& series = *chart->series[i];
				if (!series.visible || series.empty()) continue;
				const size_t index = nearest_index(series, data_x);
				const vgui_chart_point& point = series.at(index);
				if (point.x < layout.x0 || point.x > layout.x1) continue;
				const float px = layout.plot_x
					+ (float)((point.x - layout.x0) / (layout.x1 - layout.x0))
					* layout.plot_w;
				const float distance = std::fabs(px - wx);
				if (distance < best_distance)
				{
					best_distance = distance;
					best_series = (int)i;
					best_index = index;
				}
			}
			if (best_series < 0)
			{
				clear_hover(chart);
				return;
			}
			chart->hover_valid = true;
			chart->hover_series = best_series;
			chart->hover_index = best_index;
			chart->hover_x = chart->series[(size_t)best_series]->at(best_index).x;
			chart->hover_px = wx;
			chart->hover_py = wy;
		}

		void set_hover_to(chart_impl* chart, int series_index, size_t index)
		{
			if (series_index < 0 || (size_t)series_index >= chart->series.size())
				return;
			const series_data& series = *chart->series[(size_t)series_index];
			if (series.empty()) return;
			index = std::min(index, series.size() - 1);
			const vgui_chart_point& point = series.at(index);
			const chart_layout& layout = chart->layout;
			if (!layout.valid) return;
			chart->hover_valid = true;
			chart->hover_series = series_index;
			chart->hover_index = index;
			chart->hover_x = point.x;
			chart->hover_px = layout.plot_x
				+ (float)((point.x - layout.x0) / (layout.x1 - layout.x0))
				* layout.plot_w;
			chart->hover_py = std::isfinite(point.y)
				? layout.plot_y + (float)((layout.y1 - point.y)
					/ (layout.y1 - layout.y0)) * layout.plot_h
				: layout.plot_y + layout.plot_h * 0.5f;
		}

		void invalidate_and_follow(chart_impl* chart, bool follow)
		{
			chart->follow = follow;
			chart->manual_view = false;
			chart_invalidate(chart);
		}

		void zoom_at(chart_impl* chart, double anchor, double factor)
		{
			if (!chart->layout.valid) return;
			const double x0 = chart->layout.x0;
			const double x1 = chart->layout.x1;
			double a = std::clamp(anchor, x0, x1);
			factor = std::clamp(factor, 0.05, 20.0);
			double nx0 = a + (x0 - a) * factor;
			double nx1 = a + (x1 - a) * factor;
			if (!(nx1 > nx0))
			{
				const double c = (x0 + x1) * 0.5;
				nx0 = c - 0.5;
				nx1 = c + 0.5;
			}
			chart->manual_view = true;
			chart->follow = false;
			chart->view_x0 = nx0;
			chart->view_x1 = nx1;
			chart_invalidate(chart);
		}

		bool legend_click(chart_impl* chart, float wx, float wy)
		{
			for (const legend_hit& hit : chart->layout.legend)
			{
				if (wx >= hit.x && wx <= hit.x + hit.w
					&& wy >= hit.y && wy <= hit.y + hit.h)
				{
					if (hit.series_index < chart->series.size())
					{
						chart->series[hit.series_index]->visible =
							!chart->series[hit.series_index]->visible;
						chart_invalidate(chart);
						return true;
					}
				}
			}
			return false;
		}

		void announce(chart_impl* chart)
		{
			if (!chart->ctx) return;
			std::string text = chart->title.empty() ? "Chart" : chart->title;
			if (chart->hover_valid && chart->hover_series >= 0
				&& (size_t)chart->hover_series < chart->series.size())
			{
				const series_data& series = *chart->series[(size_t)chart->hover_series];
				if (chart->hover_index < series.size())
				{
					const vgui_chart_point& point = series.at(chart->hover_index);
					std::string value;
					format_value(chart->y_axis, point.y, value);
					text += ": ";
					text += series.name;
					text += " ";
					text += value;
					if (!series.unit.empty())
					{
						text += " ";
						text += series.unit;
					}
				}
			}
			vgui_context_announce(chart->ctx, text.c_str());
		}

	} /* namespace */

	int chart_event(vgui_widget* widget, const vgui_event* event, void* user)
	{
		chart_impl* chart = static_cast<chart_impl*>(user);
		if (!chart || !widget || !event)
			return 0;

		float wx = 0.0f;
		float wy = 0.0f;
		{
			float width = 0.0f;
			float height = 0.0f;
			vgui_widget_get_rect(widget, &wx, &wy, &width, &height);
		}
		const float ax = wx + event->x;
		const float ay = wy + event->y;

		switch (event->type)
		{
		case VGUI_EVENT_MOUSE_MOVE:
		{
			ensure_layout(chart);
			if (chart->panning && chart->layout.valid)
			{
				const float span = chart->layout.plot_w;
				if (span > 1.0f)
				{
					const double x_span = chart->pan_x1 - chart->pan_x0;
					const double shift = -(double)(event->x - chart->pan_last_x)
						/ (double)span * x_span;
					chart->manual_view = true;
					chart->follow = false;
					chart->view_x0 = chart->pan_x0 + shift;
					chart->view_x1 = chart->pan_x1 + shift;
					chart_invalidate(chart);
				}
				return 1;
			}
			update_hover(chart, event->x, event->y, wx, wy);
			return chart->hover_valid ? 1 : 0;
		}
		case VGUI_EVENT_MOUSE_DOWN:
		{
			ensure_layout(chart);
			vgui_set_focus(chart->ctx, widget);
			if (event->button == 2 && chart->layout.valid)
			{
				chart->panning = true;
				chart->pan_last_x = event->x;
				chart->pan_x0 = chart->layout.x0;
				chart->pan_x1 = chart->layout.x1;
				return 1;
			}
			update_hover(chart, event->x, event->y, wx, wy);
			return 1;
		}
		case VGUI_EVENT_MOUSE_UP:
			if (chart->panning)
			{
				chart->panning = false;
				return 1;
			}
			return 0;
		case VGUI_EVENT_CLICK:
		{
			ensure_layout(chart);
			if (legend_click(chart, ax, ay))
				return 1;
			update_hover(chart, event->x, event->y, wx, wy);
			return chart->hover_valid ? 1 : 0;
		}
		case VGUI_EVENT_DOUBLE_CLICK:
			chart->manual_view = false;
			chart->follow = chart->options.follow != 0;
			chart_invalidate(chart);
			return 1;
		case VGUI_EVENT_MOUSE_WHEEL:
		{
			if ((event->mods & VGUI_MOD_CTRL) == 0)
				return 0;
			ensure_layout(chart);
			if (!chart->layout.valid) return 0;
			float wheel = event->wheel_y != 0.0f ? event->wheel_y : event->wheel_x;
			if (wheel == 0.0f) wheel = 1.0f;
			const float px = std::clamp(ax, chart->layout.plot_x,
				chart->layout.plot_x + chart->layout.plot_w);
			const double anchor = chart->layout.x0
				+ (double)((px - chart->layout.plot_x) / chart->layout.plot_w)
				* (chart->layout.x1 - chart->layout.x0);
			zoom_at(chart, anchor, std::pow(1.25, -(double)wheel));
			update_hover(chart, event->x, event->y, wx, wy);
			return 1;
		}
		case VGUI_EVENT_ENTER:
			return 0;
		case VGUI_EVENT_LEAVE:
			clear_hover(chart);
			return 0;
		case VGUI_EVENT_KEY_DOWN:
			break;
		case VGUI_EVENT_KEY_UP:
			return 0;
		default:
			return 0;
		}

		ensure_layout(chart);
		if (!chart->layout.valid)
			return 0;

		const int key = event->key;
		std::vector<size_t> visible = visible_series(chart);

		if (key == VGUI_KEY_ESCAPE)
		{
			if (chart->panning)
			{
				chart->panning = false;
				chart->manual_view = true;
				chart->view_x0 = chart->pan_x0;
				chart->view_x1 = chart->pan_x1;
				chart_invalidate(chart);
			}
			clear_hover(chart);
			return 1;
		}
		if (key == VGUI_KEY_ENTER)
		{
			announce(chart);
			return 1;
		}
		if (key == '0' || key == VGUI_KEY_HOME || key == VGUI_KEY_END)
		{
			if (key == '0')
			{
				invalidate_and_follow(chart, chart->options.follow != 0);
				return 1;
			}
			if (visible.empty()) return 1;
			const size_t series_index = visible.front();
			const series_data& series = *chart->series[series_index];
			const size_t begin = series.empty() ? 0 : (key == VGUI_KEY_HOME
				? 0 : series.size() - 1);
			set_hover_to(chart, (int)series_index, begin);
			return 1;
		}
		if (key == '+' || key == '=' || key == '-' || key == '_')
		{
			const bool zoom_in = key == '+' || key == '=';
			const double center = (chart->layout.x0 + chart->layout.x1) * 0.5;
			zoom_at(chart, center, zoom_in ? 1.0 / 1.25 : 1.25);
			return 1;
		}
		if (key == VGUI_KEY_UP || key == VGUI_KEY_DOWN)
		{
			if (visible.empty()) return 1;
			int current = chart->hover_series;
			int position = -1;
			for (size_t i = 0; i < visible.size(); ++i)
			{
				if ((int)visible[i] == current) { position = (int)i; break; }
			}
			if (position < 0) position = 0;
			else
			{
				if (key == VGUI_KEY_UP)
					position = (position - 1 + (int)visible.size()) % (int)visible.size();
				else
					position = (position + 1) % (int)visible.size();
			}
			const size_t series_index = visible[(size_t)position];
			const size_t index = nearest_index(*chart->series[series_index],
				chart->hover_valid ? chart->hover_x : chart->layout.x0);
			set_hover_to(chart, (int)series_index, index);
			return 1;
		}
		if (key == VGUI_KEY_LEFT || key == VGUI_KEY_RIGHT)
		{
			if (visible.empty()) return 1;
			int series_index = chart->hover_series;
			bool found = false;
			for (size_t index : visible)
			{
				if ((int)index == series_index) { found = true; break; }
			}
			if (!found) series_index = (int)visible.front();
			const series_data& series = *chart->series[(size_t)series_index];
			size_t index = nearest_index(series,
				chart->hover_valid ? chart->hover_x : chart->layout.x0);
			if (key == VGUI_KEY_LEFT && index > 0) --index;
			else if (key == VGUI_KEY_RIGHT && index + 1 < series.size()) ++index;
			set_hover_to(chart, series_index, index);
			return 1;
		}
		return 0;
	}

} /* namespace vgchart */
