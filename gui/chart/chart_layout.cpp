#include "chart_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace vgchart {

	namespace {

		constexpr double kEpsilon = 1e-12;

		size_t lower_bound_x(const series_data& series, double x)
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

		size_t upper_bound_x(const series_data& series, double x)
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

		float logical_font(vgui_context* ctx)
		{
			if (!ctx) return 13.0f;
			const float scale = vgui_context_get_ui_scale(ctx);
			const float effective = vgui_context_get_font_size(ctx);
			if (!(scale > 0.0f)) return 13.0f;
			const float value = effective / scale;
			return value > 4.0f ? value : 13.0f;
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

		double nice_step(double raw)
		{
			if (!(raw > 0.0) || !std::isfinite(raw)) return 1.0;
			const double exponent = std::floor(std::log10(raw));
			const double power = std::pow(10.0, exponent);
			const double fraction = raw / power;
			double nice = 1.0;
			if (fraction <= 1.0) nice = 1.0;
			else if (fraction <= 2.0) nice = 2.0;
			else if (fraction <= 2.5) nice = 2.5;
			else if (fraction <= 5.0) nice = 5.0;
			else nice = 10.0;
			return nice * power;
		}

		double time_step(double raw)
		{
			static const double steps[] = {
				0.001, 0.002, 0.005, 0.01, 0.02, 0.05, 0.1, 0.2, 0.5,
				1.0, 2.0, 5.0, 10.0, 15.0, 30.0,
				60.0, 120.0, 300.0, 600.0, 900.0, 1800.0,
				3600.0, 7200.0, 14400.0, 21600.0, 43200.0, 86400.0,
				172800.0, 604800.0, 2592000.0, 31536000.0
			};
			for (double step : steps)
			{
				if (step >= raw)
					return step;
			}
			return steps[sizeof(steps) / sizeof(steps[0]) - 1];
		}

		/* ---------------------------------------------------- datetime axis */

		enum class datetime_unit {
			second,
			minute,
			hour,
			day,
			month,
			year
		};

		struct datetime_step {
			datetime_unit unit = datetime_unit::second;
			int count = 1;
			double seconds = 1.0;	/* nominal length, used for the choice */
		};

		const datetime_step* pick_datetime_step(double raw)
		{
			static const datetime_step steps[] = {
				{ datetime_unit::second, 1, 1.0 },
				{ datetime_unit::second, 2, 2.0 },
				{ datetime_unit::second, 5, 5.0 },
				{ datetime_unit::second, 10, 10.0 },
				{ datetime_unit::second, 15, 15.0 },
				{ datetime_unit::second, 30, 30.0 },
				{ datetime_unit::minute, 1, 60.0 },
				{ datetime_unit::minute, 2, 120.0 },
				{ datetime_unit::minute, 5, 300.0 },
				{ datetime_unit::minute, 10, 600.0 },
				{ datetime_unit::minute, 15, 900.0 },
				{ datetime_unit::minute, 30, 1800.0 },
				{ datetime_unit::hour, 1, 3600.0 },
				{ datetime_unit::hour, 2, 7200.0 },
				{ datetime_unit::hour, 3, 10800.0 },
				{ datetime_unit::hour, 6, 21600.0 },
				{ datetime_unit::hour, 12, 43200.0 },
				{ datetime_unit::day, 1, 86400.0 },
				{ datetime_unit::day, 2, 172800.0 },
				{ datetime_unit::day, 7, 604800.0 },
				{ datetime_unit::day, 14, 1209600.0 },
				{ datetime_unit::month, 1, 2678400.0 },
				{ datetime_unit::month, 3, 8035200.0 },
				{ datetime_unit::month, 6, 16070400.0 },
				{ datetime_unit::year, 1, 31536000.0 },
				{ datetime_unit::year, 2, 63072000.0 },
				{ datetime_unit::year, 5, 157680000.0 },
				{ datetime_unit::year, 10, 315360000.0 },
				{ datetime_unit::year, 20, 630720000.0 },
				{ datetime_unit::year, 25, 788400000.0 },
				{ datetime_unit::year, 50, 1576800000.0 },
				{ datetime_unit::year, 100, 3153600000.0 },
			};
			for (const datetime_step& step : steps)
			{
				if (step.seconds >= raw)
					return &step;
			}
			return &steps[sizeof(steps) / sizeof(steps[0]) - 1];
		}

		/* Epoch seconds of the first calendar instant of (y, m) in UTC,
		 * corrected to the axis timezone. */
		double month_start_epoch(long long y, unsigned m, int time_zone)
		{
			const double utc = (double)days_from_civil(y, m, 1) * 86400.0;
			if (!time_zone) return utc;
			return utc - local_utc_offset(utc);
		}

		void push_datetime_tick(const axis_config& axis, double value,
			datetime_unit unit, std::vector<tick_value>& out)
		{
			tick_value tick;
			tick.value = value;
			if (axis.format || !axis.pattern.empty())
			{
				format_value(axis, value, tick.text);
			}
			else
			{
				const double shifted = value
					+ (axis.time_zone ? local_utc_offset(value) : 0.0);
				const double floored = std::floor(shifted);
				long long days = (long long)std::floor(floored / 86400.0);
				double rem = floored - (double)days * 86400.0;
				int sec_of_day = (int)(rem + 0.5);
				if (sec_of_day >= 86400)
				{
					sec_of_day -= 86400;
					days += 1;
				}
				long long y = 0;
				unsigned mo = 0;
				unsigned d = 0;
				civil_from_days(days, y, mo, d);
				const int hh = sec_of_day / 3600;
				const int mm = (sec_of_day % 3600) / 60;
				const int ss = sec_of_day % 60;
				char buffer[64] = {};
				switch (unit)
				{
				case datetime_unit::second:
					std::snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d",
						hh, mm, ss);
					break;
				case datetime_unit::minute:
					std::snprintf(buffer, sizeof(buffer), "%02d:%02d", hh, mm);
					break;
				case datetime_unit::hour:
					std::snprintf(buffer, sizeof(buffer), "%02u-%02u %02d:%02d",
						mo, d, hh, mm);
					break;
				case datetime_unit::day:
					std::snprintf(buffer, sizeof(buffer), "%02u-%02u", mo, d);
					break;
				case datetime_unit::month:
					std::snprintf(buffer, sizeof(buffer), "%04lld-%02u", y, mo);
					break;
				default:
					std::snprintf(buffer, sizeof(buffer), "%04lld", y);
					break;
				}
				tick.text = buffer;
			}
			out.push_back(std::move(tick));
		}

		void build_datetime_ticks(const axis_config& axis, double lo, double hi,
			int requested, std::vector<tick_value>& out)
		{
			const int want = std::clamp(requested > 0 ? requested : 5, 2, 16);
			const double raw = (hi - lo) / (double)want;
			const datetime_step& step = *pick_datetime_step(raw);
			/* Local ticks are anchored at the range start; per-tick DST is
			 * recomputed so local midnights stay exact. */
			const double shift = axis.time_zone ? local_utc_offset(lo) : 0.0;

			switch (step.unit)
			{
			case datetime_unit::second:
			case datetime_unit::minute:
			case datetime_unit::hour:
			{
				const double span = step.seconds;
				double value = std::ceil((lo + shift - 1e-9) / span) * span
					- shift;
				for (int i = 0; i < 64; ++i)
				{
					if (value > hi + 1e-9) break;
					if (value >= lo - 1e-9)
						push_datetime_tick(axis, value, step.unit, out);
					value += span;
				}
				break;
			}
			case datetime_unit::day:
			{
				long long day = (long long)std::floor((lo + shift) / 86400.0);
				day = ((day % step.count) + step.count) % step.count == 0
					? day
					: day + (step.count - day % step.count);
				for (int i = 0; i < 64; ++i)
				{
					const double utc = (double)day * 86400.0;
					const double value = utc - (axis.time_zone
						? local_utc_offset(utc) : 0.0);
					if (value > hi + 1e-9) break;
					if (value >= lo - 1e-9)
						push_datetime_tick(axis, value, step.unit, out);
					day += step.count;
				}
				break;
			}
			case datetime_unit::month:
			case datetime_unit::year:
			{
				const bool years = step.unit == datetime_unit::year;
				const long long base_days = (long long)std::floor(
					(lo + shift) / 86400.0);
				long long y = 0;
				unsigned m = 0;
				unsigned d = 0;
				civil_from_days(base_days, y, m, d);
				if (years)
				{
					const long long r = y % step.count;
					if (r != 0) y += step.count - r;
				}
				else
				{
					const long long total = y * 12 + (m - 1);
					const long long r = total % step.count;
					if (r != 0)
					{
						long long next = total + (step.count - r);
						y = next / 12;
						m = (unsigned)(next % 12 + 1);
					}
				}
				for (int i = 0; i < 64; ++i)
				{
					const double value = month_start_epoch(y, m, axis.time_zone);
					if (value > hi + 1e-9) break;
					if (value >= lo - 1e-9)
						push_datetime_tick(axis, value, step.unit, out);
					if (years)
					{
						y += step.count;
					}
					else
					{
						long long total = y * 12 + (m - 1) + step.count;
						y = total / 12;
						m = (unsigned)(total % 12 + 1);
					}
				}
				break;
			}
			}
		}

		void build_ticks(const axis_config& axis, double lo, double hi,
			int requested, std::vector<tick_value>& out)
		{
			out.clear();
			if (!std::isfinite(lo) || !std::isfinite(hi) || !(hi > lo))
				return;
			if (axis.scale == VGUI_CHART_SCALE_DATETIME)
			{
				build_datetime_ticks(axis, lo, hi, requested, out);
				return;
			}
			const int want = std::clamp(requested > 0 ? requested : 5, 2, 16);
			const double raw = (hi - lo) / (double)want;
			const bool time_axis = axis.scale == VGUI_CHART_SCALE_TIME;
			double step = time_axis ? time_step(raw) : nice_step(raw);
			if (!(step > 0.0) || !std::isfinite(step))
				step = raw > 0.0 ? raw : 1.0;

			const double start = std::ceil((lo - step * 1e-9) / step) * step;
			const double limit = hi + step * 1e-9;
			for (int i = 0; i < 64; ++i)
			{
				const double value = start + step * (double)i;
				if (value > limit) break;
				if (value < lo - step * 1e-9) continue;
				tick_value tick;
				tick.value = std::fabs(value) < step * 1e-9 ? 0.0 : value;
				format_value(axis, tick.value, tick.text);
				out.push_back(std::move(tick));
			}
		}

		bool compute_x_view(chart_impl* chart, double& x0, double& x1,
			bool& empty)
		{
			double data_min = std::numeric_limits<double>::infinity();
			double data_max = -std::numeric_limits<double>::infinity();
			bool any = false;
			for (const auto& series : chart->series)
			{
				if (!series || !series->visible || series->empty())
					continue;
				any = true;
				data_min = std::min(data_min, series->at(0).x);
				data_max = std::max(data_max, series->at(series->size() - 1).x);
			}
			empty = !any;

			if (chart->manual_view)
			{
				x0 = chart->view_x0;
				x1 = chart->view_x1;
			}
			else if (any && chart->follow)
			{
				if (chart->options.window > 0.0)
				{
					x1 = data_max;
					x0 = x1 - chart->options.window;
				}
				else
				{
					x0 = data_min;
					x1 = data_max;
				}
			}
			else if (any)
			{
				x0 = data_min;
				x1 = data_max;
			}
			else
			{
				x0 = 0.0;
				x1 = 1.0;
			}

			if (!std::isfinite(x0) || !std::isfinite(x1) || !(x1 > x0))
			{
				const double center = std::isfinite(x0) ? x0 : 0.0;
				const double half = std::max(1.0, std::fabs(center) * 0.02);
				x0 = center - half;
				x1 = center + half;
			}
			if (!(x1 > x0))
			{
				x0 = 0.0;
				x1 = 1.0;
			}
			return any;
		}

		void compute_y_range(chart_impl* chart, double x0, double x1,
			double& y0, double& y1, bool& empty)
		{
			double lo = std::numeric_limits<double>::infinity();
			double hi = -std::numeric_limits<double>::infinity();
			bool any = false;
			bool has_bars = false;

			std::map<std::string, std::vector<double>> stacked_pos;
			std::map<std::string, std::vector<double>> stacked_neg;

			for (const auto& series_ptr : chart->series)
			{
				if (!series_ptr || !series_ptr->visible || series_ptr->empty())
					continue;
				const series_data& series = *series_ptr;
				const size_t begin = lower_bound_x(series, x0);
				const size_t end = upper_bound_x(series, x1);
				if (end <= begin) continue;
				const bool stacked_bars = series.stacked
					&& series.kind == VGUI_CHART_BAR;
				if (series.kind == VGUI_CHART_BAR) has_bars = true;
				if (stacked_bars)
				{
					const std::string group = series.stack_group.empty()
						? std::string("__default") : series.stack_group;
					auto& pos = stacked_pos[group];
					auto& neg = stacked_neg[group];
					const size_t count = end - begin;
					if (pos.size() < count) pos.resize(count, 0.0);
					if (neg.size() < count) neg.resize(count, 0.0);
					for (size_t j = 0; j < count; ++j)
					{
						const double y = series.at(begin + j).y;
						if (!std::isfinite(y)) continue;
						any = true;
						if (y >= 0.0) pos[j] += y;
						else neg[j] += y;
					}
					continue;
				}
				for (size_t i = begin; i < end; ++i)
				{
					const double y = series.at(i).y;
					if (!std::isfinite(y)) continue;
					any = true;
					if (y < lo) lo = y;
					if (y > hi) hi = y;
				}
			}

			for (const auto& entry : stacked_pos)
			{
				for (double value : entry.second)
				{
					if (value > hi) hi = value;
				}
			}
			for (const auto& entry : stacked_neg)
			{
				for (double value : entry.second)
				{
					if (value < lo) lo = value;
				}
			}

			empty = !any;
			if (chart->y_axis.min < chart->y_axis.max)
			{
				y0 = chart->y_axis.min;
				y1 = chart->y_axis.max;
				return;
			}
			if (!any)
			{
				y0 = 0.0;
				y1 = 1.0;
				return;
			}

			const bool include_zero = has_bars || chart->y_axis.include_zero;
			if (include_zero)
			{
				lo = std::min(lo, 0.0);
				hi = std::max(hi, 0.0);
			}
			if (!(hi > lo))
			{
				const double pad = std::max(1.0, std::fabs(lo) * 0.1);
				y0 = lo - pad;
				y1 = hi + pad;
			}
			else
			{
				const double pad = (hi - lo) * 0.06;
				y0 = lo - pad;
				y1 = hi + pad;
				if (include_zero)
				{
					y0 = std::min(y0, 0.0);
					y1 = std::max(y1, 0.0);
				}
			}
			if (!std::isfinite(y0) || !std::isfinite(y1) || !(y1 > y0))
			{
				y0 = 0.0;
				y1 = 1.0;
			}
		}

		void update_cache(chart_impl* chart, size_t index,
			const chart_layout& layout, bool& cache_hit)
		{
			series_data& series = *chart->series[index];
			series_cache& cache = chart->cache[index];
			const double x0 = layout.x0;
			const double x1 = layout.x1;
			const float plot_w = layout.plot_w;
			const int mode = chart->options.decimation;

			if (cache.valid && cache.revision == series.revision
				&& cache.x0 == x0 && cache.x1 == x1
				&& cache.plot_w == plot_w && cache.decimation == mode)
			{
				cache_hit = cache_hit && true;
				return;
			}
			cache_hit = false;
			cache.valid = false;
			cache.revision = series.revision;
			cache.x0 = x0;
			cache.x1 = x1;
			cache.plot_w = plot_w;
			cache.decimation = mode;
			cache.line.clear();
			cache.bars.clear();
			cache.visible_points = 0;
			cache.drawn_points = 0;

			const size_t begin = lower_bound_x(series, x0);
			const size_t end = upper_bound_x(series, x1);
			cache.visible_points = end > begin ? end - begin : 0;
			if (end <= begin || plot_w < 2.0f || !(x1 > x0))
			{
				cache.valid = true;
				return;
			}

			const double x_span = x1 - x0;
			const double y_span = layout.y1 - layout.y0;
			const float plot_x = layout.plot_x;
			const float plot_y = layout.plot_y;
			const float plot_h = layout.plot_h;
			auto sx = [&](double x) {
				return plot_x + (float)((x - x0) / x_span) * plot_w;
			};
			auto sy = [&](double y) {
				return plot_y + (float)((layout.y1 - y) / y_span) * plot_h;
			};

			if (series.kind == VGUI_CHART_BAR)
			{
				const int columns = std::max(1, (int)std::ceil(plot_w));
				std::vector<double> values((size_t)columns,
					std::numeric_limits<double>::quiet_NaN());
				std::vector<char> seen((size_t)columns, 0);
				for (size_t i = begin; i < end; ++i)
				{
					const vgui_chart_point& point = series.at(i);
					if (!std::isfinite(point.x) || !std::isfinite(point.y))
						continue;
					int column = (int)std::floor(
						(point.x - x0) / x_span * (double)columns);
					column = std::clamp(column, 0, columns - 1);
					if (!seen[(size_t)column]
						|| std::fabs(point.y) > std::fabs(values[(size_t)column]))
					{
						values[(size_t)column] = point.y;
						seen[(size_t)column] = 1;
					}
				}
				cache.bars.reserve((size_t)columns);
				for (int col = 0; col < columns; ++col)
				{
					if (!seen[(size_t)col]) continue;
					bar_bucket bucket;
					bucket.column = col;
					bucket.x = plot_x + ((float)col + 0.5f) / (float)columns * plot_w;
					bucket.value = values[(size_t)col];
					cache.bars.push_back(bucket);
				}
				cache.drawn_points = cache.bars.size();
				cache.valid = true;
				return;
			}

			const bool use_minmax = mode == VGUI_CHART_DECIMATION_MIN_MAX
				&& cache.visible_points > (size_t)std::ceil(plot_w * 2.0f)
				&& plot_w >= 2.0f;
			if (!use_minmax)
			{
				cache.line.reserve(cache.visible_points);
				bool pending_break = false;
				for (size_t i = begin; i < end; ++i)
				{
					const vgui_chart_point& point = series.at(i);
					if (!std::isfinite(point.y))
					{
						pending_break = true;
						continue;
					}
					chart_vertex vertex;
					vertex.x = sx(point.x);
					vertex.y = sy(point.y);
					vertex.flags = pending_break ? 1 : 0;
					pending_break = false;
					cache.line.push_back(vertex);
				}
			}
			else
			{
				const int columns = std::max(1, (int)std::ceil(plot_w));
				cache.line.reserve((size_t)columns * 2 + 8);
				int current_col = -1;
				bool has_bucket = false;
				bool gap = false;
				double min_y = 0.0;
				double max_y = 0.0;
				double min_x = 0.0;
				double max_x = 0.0;

				auto flush = [&]() {
					if (!has_bucket) return;
					const bool min_first = min_x <= max_x;
					chart_vertex first;
					first.x = sx(min_first ? min_x : max_x);
					first.y = sy(min_first ? min_y : max_y);
					first.flags = gap ? 1 : 0;
					cache.line.push_back(first);
					chart_vertex second;
					second.x = sx(min_first ? max_x : min_x);
					second.y = sy(min_first ? max_y : min_y);
					second.flags = 0;
					cache.line.push_back(second);
					has_bucket = false;
					gap = false;
				};

				for (size_t i = begin; i < end; ++i)
				{
					const vgui_chart_point& point = series.at(i);
					if (!std::isfinite(point.y))
					{
						flush();
						gap = true;
						current_col = -1;
						continue;
					}
					int column = (int)std::floor(
						(point.x - x0) / x_span * (double)columns);
					column = std::clamp(column, 0, columns - 1);
					if (!has_bucket || column != current_col)
					{
						flush();
						current_col = column;
						has_bucket = true;
						min_y = max_y = point.y;
						min_x = max_x = point.x;
					}
					else
					{
						if (point.y < min_y)
						{
							min_y = point.y;
							min_x = point.x;
						}
						if (point.y > max_y)
						{
							max_y = point.y;
							max_x = point.x;
						}
					}
				}
				flush();
			}
			cache.drawn_points = cache.line.size();
			cache.valid = true;
		}

		void filter_tick_labels(chart_impl* chart, chart_layout& layout,
			const std::vector<tick_value>& source, bool horizontal)
		{
			if (source.empty()) return;
			const float font = layout.axis_font;
			float last_end = -1e30f;
			bool first = true;
			for (const tick_value& tick : source)
			{
				tick_value copy = tick;
				const float width = text_width(chart->widget, tick.text, font);
				const float gap = horizontal ? width + 8.0f * ui_scale(chart->widget)
					: line_height(chart->widget, font) + 2.0f;
				/* X ticks are ascending left-to-right, Y ticks descend from
				 * the bottom to the top of the plot.  Measure the distance in
				 * the direction the labels advance so the second Y tick is not
				 * rejected by a one-sided overlap test. */
				const float distance = horizontal
					? (tick.pos - width * 0.5f) - last_end
					: last_end - tick.pos;
				if (!first && distance < gap)
					copy.label = false;
				else
				{
					copy.label = true;
					first = false;
					last_end = horizontal ? tick.pos + width * 0.5f : tick.pos;
				}
				if (horizontal) layout.xticks.push_back(std::move(copy));
				else layout.yticks.push_back(std::move(copy));
			}
		}

	} /* namespace */

	void chart_prepare(chart_impl* chart, float x, float y, float w, float h)
	{
		chart->layout = {};
		chart->stats = {};

		if (!chart->theme_custom)
			derive_theme(chart->ctx, &chart->theme);

		for (const auto& series : chart->series)
		{
			if (series)
				chart->stats.raw_points += series->size();
		}

		if (w < 40.0f || h < 36.0f)
			return;

		const float scale = ui_scale(chart->widget);
		const float font = logical_font(chart->ctx);
		const float pad = 8.0f * scale;
		const float inner_x = x + pad;
		const float inner_y = y + pad;
		const float inner_w = w - pad * 2.0f;
		const float inner_h = h - pad * 2.0f;
		if (inner_w < 40.0f || inner_h < 32.0f)
			return;

		double x0 = 0.0;
		double x1 = 1.0;
		bool empty = false;
		compute_x_view(chart, x0, x1, empty);
		double y0 = 0.0;
		double y1 = 1.0;
		compute_y_range(chart, x0, x1, y0, y1, empty);

		chart_layout layout;
		layout.x = x;
		layout.y = y;
		layout.w = w;
		layout.h = h;
		layout.content_x = inner_x;
		layout.content_y = inner_y;
		layout.content_w = inner_w;
		layout.content_h = inner_h;
		layout.x0 = x0;
		layout.x1 = x1;
		layout.y0 = y0;
		layout.y1 = y1;
		layout.empty = empty;
		layout.axis_font = std::max(9.0f, font * 0.72f);
		layout.title_font = std::max(10.0f, font * 0.95f);
		layout.legend_font = layout.axis_font;

		if (!chart->title.empty())
			layout.title_h = line_height(chart->widget, layout.title_font) + 4.0f * scale;
		layout.title_x = inner_x;
		layout.title_y = inner_y;

		if (chart->options.legend)
		{
			const float item_h = line_height(chart->widget, layout.legend_font);
			layout.legend_h = item_h + 6.0f * scale;
			layout.legend_x = inner_x;
			layout.legend_y = inner_y + layout.title_h;
		}

		std::vector<tick_value> x_ticks;
		std::vector<tick_value> y_ticks;
		build_ticks(chart->x_axis, x0, x1, chart->x_axis.tick_count, x_ticks);
		build_ticks(chart->y_axis, y0, y1, chart->y_axis.tick_count, y_ticks);

		float y_label_width = 0.0f;
		for (const tick_value& tick : y_ticks)
			y_label_width = std::max(y_label_width,
				text_width(chart->widget, tick.text, layout.axis_font));
		const float y_axis_width = y_label_width + 12.0f * scale;
		float x_axis_height = line_height(chart->widget, layout.axis_font)
			+ 6.0f * scale;
		if (!chart->x_axis.label.empty())
			x_axis_height += line_height(chart->widget, layout.axis_font) + 2.0f * scale;

		layout.plot_x = inner_x + y_axis_width;
		layout.plot_y = inner_y + layout.title_h + layout.legend_h + 2.0f * scale;
		layout.plot_w = inner_w - y_axis_width - 2.0f * scale;
		layout.plot_h = inner_h - layout.title_h - layout.legend_h
			- x_axis_height - 4.0f * scale;
		if (layout.plot_w < 24.0f || layout.plot_h < 20.0f)
		{
			chart->layout = layout;
			chart->layout.valid = false;
			return;
		}

		for (tick_value& tick : x_ticks)
			tick.pos = layout.plot_x
				+ (float)((tick.value - x0) / (x1 - x0)) * layout.plot_w;
		for (tick_value& tick : y_ticks)
			tick.pos = layout.plot_y
				+ (float)((y1 - tick.value) / (y1 - y0)) * layout.plot_h;
		filter_tick_labels(chart, layout, x_ticks, true);
		filter_tick_labels(chart, layout, y_ticks, false);

		if (chart->options.legend)
		{
			float cursor = inner_x;
			const float swatch = 14.0f * scale;
			const float item_gap = 14.0f * scale;
			const float item_h = line_height(chart->widget, layout.legend_font);
			for (size_t i = 0; i < chart->series.size(); ++i)
			{
				const series_data& series = *chart->series[i];
				if (!series.visible) continue;
				const float text_w = text_width(chart->widget, series.name,
					layout.legend_font);
				const float item_w = swatch + 4.0f * scale + text_w;
				if (cursor + item_w > inner_x + inner_w)
				{
					layout.legend_h = 0.0f;
					layout.plot_y = inner_y + layout.title_h + 2.0f * scale;
					layout.plot_h = inner_h - layout.title_h - x_axis_height - 4.0f * scale;
					if (layout.plot_h < 20.0f)
					{
						chart->layout = layout;
						chart->layout.valid = false;
						return;
					}
					break;
				}
				legend_hit hit;
				hit.x = cursor;
				hit.y = layout.legend_y;
				hit.w = item_w;
				hit.h = item_h;
				hit.series_index = i;
				layout.legend.push_back(hit);
				cursor += item_w + item_gap;
			}
		}

		for (const auto& series : chart->series)
		{
			if (series && series->visible)
				++chart->stats.series_drawn;
		}

		bool cache_hit = true;
		for (size_t i = 0; i < chart->series.size(); ++i)
		{
			series_data& series = *chart->series[i];
			if (!series.visible) continue;
			update_cache(chart, i, layout, cache_hit);
			if (i < chart->cache.size())
			{
				chart->stats.visible_points += chart->cache[i].visible_points;
				chart->stats.drawn_points += chart->cache[i].drawn_points;
			}
		}
		chart->stats.cache_hit = cache_hit ? 1 : 0;
		layout.valid = true;
		chart->layout = std::move(layout);
	}

} /* namespace vgchart */
