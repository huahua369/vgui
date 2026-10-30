#include "chart_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace vgchart {

	namespace {

		constexpr size_t kFlushSize = 64 * 1024;
		constexpr size_t kMaxColumns = 4096;

		size_t export_lower_bound(const series_data& series, double x)
		{
			size_t lo = 0;
			size_t hi = series.size();
			while (lo < hi)
			{
				const size_t mid = lo + (hi - lo) / 2;
				if (series.at(mid).x < x) lo = mid + 1;
				else hi = mid;
			}
			return lo;
		}

		size_t export_upper_bound(const series_data& series, double x)
		{
			size_t lo = 0;
			size_t hi = series.size();
			while (lo < hi)
			{
				const size_t mid = lo + (hi - lo) / 2;
				if (series.at(mid).x <= x) lo = mid + 1;
				else hi = mid;
			}
			return lo;
		}

		struct csv_writer {
			vgui_chart_write_fn write = nullptr;
			void* user = nullptr;
			std::string buffer;
			bool ok = true;

			bool flush()
			{
				if (!ok || !write)
					return false;
				if (buffer.empty())
					return true;
				if (!write(user, buffer.data(), buffer.size()))
				{
					ok = false;
					buffer.clear();
					return false;
				}
				buffer.clear();
				return true;
			}

			void append(const char* text)
			{
				if (!ok || !text) return;
				buffer += text;
				if (buffer.size() >= kFlushSize)
					flush();
			}

			void append(const std::string& text)
			{
				if (!ok) return;
				buffer += text;
				if (buffer.size() >= kFlushSize)
					flush();
			}

			void field(const std::string& text)
			{
				if (!ok) return;
				buffer += '"';
				for (char c : text)
				{
					if (c == '"')
						buffer += '"';
					buffer += c;
				}
				buffer += '"';
				if (buffer.size() >= kFlushSize)
					flush();
			}

			void number(double value)
			{
				if (!ok) return;
				char text[48] = {};
				if (std::isfinite(value))
					std::snprintf(text, sizeof(text), "%.17g", value);
				else if (std::isnan(value))
					std::snprintf(text, sizeof(text), "nan");
				else
					std::snprintf(text, sizeof(text), "%sinf",
						value < 0.0 ? "-" : "");
				buffer += text;
				if (buffer.size() >= kFlushSize)
					flush();
			}
		};

		bool begin_row(csv_writer& out, const char* card_key,
			const std::string& series_key)
		{
			if (!out.ok) return false;
			if (card_key)
			{
				out.field(card_key);
				out.append(",");
			}
			out.field(series_key);
			out.append(",");
			return out.ok;
		}

		void end_row(csv_writer& out, double x, double y)
		{
			out.number(x);
			out.append(",");
			out.number(y);
			out.append("\n");
		}

		bool write_header(csv_writer& out, const char* card_key)
		{
			out.append(card_key ? "card,series,x,y\n" : "series,x,y\n");
			return out.ok;
		}

		bool resolve_view(const chart_impl* chart, double& x0, double& x1)
		{
			if (!chart)
				return false;
			if (chart->manual_view && std::isfinite(chart->view_x0)
				&& std::isfinite(chart->view_x1) && chart->view_x1 > chart->view_x0)
			{
				x0 = chart->view_x0;
				x1 = chart->view_x1;
				return true;
			}

			double lo = std::numeric_limits<double>::infinity();
			double hi = -std::numeric_limits<double>::infinity();
			bool any = false;
			for (const auto& series : chart->series)
			{
				if (!series || !series->visible || series->empty())
					continue;
				any = true;
				lo = std::min(lo, series->at(0).x);
				hi = std::max(hi, series->at(series->size() - 1).x);
			}
			if (!any || !(hi >= lo))
				return false;

			if (chart->follow && chart->options.window > 0.0)
			{
				x1 = hi;
				x0 = x1 - chart->options.window;
			}
			else
			{
				x0 = lo;
				x1 = hi;
				if (!(x1 > x0))
				{
					const double pad = std::max(1.0, std::fabs(x0) * 0.01);
					x0 -= pad;
					x1 += pad;
				}
			}
			return std::isfinite(x0) && std::isfinite(x1) && x1 > x0;
		}

		int export_series(csv_writer& out, const chart_impl* chart,
			const series_data& series, const char* card_key, uint32_t flags,
			bool have_view, double view_x0, double view_x1)
		{
			if (series.kind == VGUI_CHART_BAR)
			{
				/* Bars are exported as the raw data they represent; decimation
				 * for bars is a rendering detail and would hide samples. */
			}

			size_t begin = 0;
			size_t end = series.size();
			if (have_view && (flags & VGUI_CHART_EXPORT_VISIBLE_ONLY))
			{
				begin = export_lower_bound(series, view_x0);
				end = export_upper_bound(series, view_x1);
			}
			if (end <= begin)
				return 1;

			const size_t visible = end - begin;
			const bool decimated = (flags & VGUI_CHART_EXPORT_DECIMATED) != 0
				&& visible > 4;

			if (!decimated)
			{
				for (size_t i = begin; i < end; ++i)
				{
					const vgui_chart_point& point = series.at(i);
					if (!begin_row(out, card_key, series.key))
						return 0;
					end_row(out, point.x, point.y);
				}
				return out.ok ? 1 : 0;
			}

			double plot_w = 1024.0;
			if (chart->layout.valid && chart->layout.plot_w > 1.0f)
				plot_w = chart->layout.plot_w;
			else if (chart->widget)
			{
				float wx = 0.0f;
				float wy = 0.0f;
				float ww = 0.0f;
				float wh = 0.0f;
				vgui_widget_get_rect(chart->widget, &wx, &wy, &ww, &wh);
				if (ww > 1.0f)
					plot_w = ww;
			}
			size_t columns = (size_t)std::clamp(std::ceil(plot_w), 1.0, 4096.0);
			columns = std::min(columns, kMaxColumns);

			struct bucket {
				bool has = false;
				double min_x = 0.0;
				double min_y = 0.0;
				double max_x = 0.0;
				double max_y = 0.0;
			};

			const bool have_range = have_view && view_x1 > view_x0;
			const double x0 = have_range ? view_x0
				: (series.empty() ? 0.0 : series.at(begin).x);
			const double x1 = have_range ? view_x1
				: (series.empty() ? 1.0 : series.at(end - 1).x);
			const double span = (x1 > x0) ? x1 - x0 : 1.0;

			bucket current;
			long current_column = -1;
			auto flush = [&]() -> bool {
				if (!current.has)
					return true;
				if (!begin_row(out, card_key, series.key))
					return false;
				end_row(out, current.min_x, current.min_y);
				if (!(current.min_x == current.max_x && current.min_y == current.max_y))
				{
					if (!begin_row(out, card_key, series.key))
						return false;
					end_row(out, current.max_x, current.max_y);
				}
				current = bucket{};
				return out.ok;
			};

			for (size_t i = begin; i < end; ++i)
			{
				const vgui_chart_point& point = series.at(i);
				if (!std::isfinite(point.x) || !std::isfinite(point.y))
				{
					if (!flush())
						return 0;
					current_column = -1;
					continue;
				}
				long column = (long)std::floor((point.x - x0) / span
					* (double)columns);
				if (column < 0) column = 0;
				if ((size_t)column >= columns) column = (long)columns - 1;
				if (!current.has || column != current_column)
				{
					if (!flush())
						return 0;
					current_column = column;
					current.has = true;
					current.min_x = current.max_x = point.x;
					current.min_y = current.max_y = point.y;
				}
				else
				{
					if (point.y < current.min_y)
					{
						current.min_y = point.y;
						current.min_x = point.x;
					}
					if (point.y > current.max_y)
					{
						current.max_y = point.y;
						current.max_x = point.x;
					}
				}
			}
			if (!flush())
				return 0;
			return out.ok ? 1 : 0;
		}

	} /* namespace */

	int chart_export_csv_impl(vgui_chart* chart, const char* card_key,
		uint32_t flags, vgui_chart_write_fn write, void* user)
	{
		if (!chart || !write)
			return 0;

		const bool include_hidden = (flags & VGUI_CHART_EXPORT_INCLUDE_HIDDEN) != 0;
		bool has_series = false;
		for (const auto& series : chart->series)
		{
			if (series && (include_hidden || series->visible))
			{
				has_series = true;
				break;
			}
		}
		if (!has_series)
		{
			set_chart_error(chart, "no series to export");
			return 0;
		}

		double view_x0 = 0.0;
		double view_x1 = 1.0;
		const bool have_view = resolve_view(chart, view_x0, view_x1);

		csv_writer out;
		out.write = write;
		out.user = user;

		if ((flags & VGUI_CHART_EXPORT_HEADER) && !write_header(out, card_key))
		{
			set_chart_error(chart, "CSV write failed");
			return 0;
		}

		for (const auto& series_ptr : chart->series)
		{
			if (!series_ptr)
				continue;
			if (!include_hidden && !series_ptr->visible)
				continue;
			if (!export_series(out, chart, *series_ptr, card_key, flags,
				have_view, view_x0, view_x1))
			{
				set_chart_error(chart, "CSV write failed");
				return 0;
			}
			if (!out.ok)
			{
				set_chart_error(chart, "CSV write failed");
				return 0;
			}
		}

		if (!out.flush())
		{
			set_chart_error(chart, "CSV write failed");
			return 0;
		}
		return 1;
	}

} /* namespace vgchart */
