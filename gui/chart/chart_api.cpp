#include "chart_internal.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>

namespace vgchart {

	namespace {

		int find_series_index(const chart_impl* chart, vgui_series_id id)
		{
			if (!chart) return -1;
			for (size_t i = 0; i < chart->series.size(); ++i)
			{
				if (chart->series[i] && chart->series[i]->id == id)
					return (int)i;
			}
			return -1;
		}

		bool key_exists(const chart_impl* chart, const std::string& key,
			const series_data* except)
		{
			for (const auto& item : chart->series)
			{
				if (item && item.get() != except && item->key == key)
					return true;
			}
			return false;
		}

		vgui_chart_series_desc copy_series_desc(const vgui_chart_series_desc* desc)
		{
			vgui_chart_series_desc copy = {};
			if (!desc) return copy;
			size_t bytes = sizeof(copy);
			if (desc->struct_size && desc->struct_size < bytes)
				bytes = desc->struct_size;
			std::memcpy(&copy, desc, bytes);
			return copy;
		}

		vgui_chart_options copy_options(const vgui_chart_options* options)
		{
			vgui_chart_options copy = {};
			if (!options) return copy;
			size_t bytes = sizeof(copy);
			if (options->struct_size && options->struct_size < bytes)
				bytes = options->struct_size;
			std::memcpy(&copy, options, bytes);
			return copy;
		}

		bool valid_series_desc(const chart_impl* chart,
			const vgui_chart_series_desc& desc, std::string& error)
		{
			if (!desc.key || !*desc.key)
			{
				error = "series key is required";
				return false;
			}
			if (desc.kind != VGUI_CHART_LINE && desc.kind != VGUI_CHART_BAR
				&& desc.kind != VGUI_CHART_AREA)
			{
				error = "invalid series kind";
				return false;
			}
			if (desc.axis != 0)
			{
				error = "series axis must be 0 in P0";
				return false;
			}
			if (key_exists(chart, desc.key, nullptr))
			{
				error = "duplicate series key";
				return false;
			}
			return true;
		}

	} /* namespace */

	void chart_invalidate(chart_impl* chart)
	{
		if (!chart) return;
		chart->layout.valid = false;
		for (series_cache& cache : chart->cache)
			cache.valid = false;
	}

} /* namespace vgchart */

extern "C" {

	void vgui_chart_options_default(vgui_chart_options* options)
	{
		if (!options) return;
		std::memset(options, 0, sizeof(*options));
		options->struct_size = (uint32_t)sizeof(*options);
		options->legend = 1;
		options->tooltip = 1;
		options->crosshair = 1;
		options->follow = 1;
		options->window = 0.0;
		options->decimation = VGUI_CHART_DECIMATION_MIN_MAX;
	}

	void vgui_chart_axis_desc_default(vgui_chart_axis_desc* desc)
	{
		if (!desc) return;
		std::memset(desc, 0, sizeof(*desc));
		desc->struct_size = (uint32_t)sizeof(*desc);
		desc->scale = VGUI_CHART_SCALE_LINEAR;
		desc->tick_count = 5;
		desc->grid = 1;
	}

	void vgui_chart_series_desc_default(vgui_chart_series_desc* desc)
	{
		if (!desc) return;
		std::memset(desc, 0, sizeof(*desc));
		desc->struct_size = (uint32_t)sizeof(*desc);
		desc->kind = VGUI_CHART_LINE;
		desc->line_width = 2.0f;
		desc->fill_alpha = 0.15f;
		desc->visible = 1;
	}

	vgui_chart* vgui_chart_create(vgui_widget* parent)
	{
		if (!parent) return nullptr;
		vgui_chart* chart = new (std::nothrow) vgui_chart();
		if (!chart) return nullptr;

		chart->ctx = vgui_widget_get_context(parent);
		vgui_chart_options_default(&chart->options);
		chart->x_axis.tick_count = 5;
		chart->x_axis.grid = 1;
		chart->y_axis.tick_count = 5;
		chart->y_axis.grid = 1;
		chart->follow = chart->options.follow != 0;
		vgchart::derive_theme(chart->ctx, &chart->theme);

		chart->widget = vgui_widget_add(parent, VGUI_CUSTOM, nullptr);
		if (!chart->widget)
		{
			delete chart;
			return nullptr;
		}
		vgui_widget_set_id(chart->widget, "vgui_chart");
		vgui_widget_set_a11y_name(chart->widget, "Chart");
		vgui_widget_set_min_size(chart->widget, 240, 160);
		vgui_widget_set_grow(chart->widget, 1.0f);
		vgui_widget_set_focusable(chart->widget, 1);
		vgui_custom_set_draw(chart->widget, &vgchart::chart_draw, chart);
		vgui_custom_set_event(chart->widget, &vgchart::chart_event, chart);
		vgui_custom_set_size(chart->widget, &vgchart::chart_size, chart);
		return chart;
	}

	void vgui_chart_destroy(vgui_chart* chart)
	{
		if (!chart) return;
		if (chart->widget)
		{
			vgui_custom_set_draw(chart->widget, nullptr, nullptr);
			vgui_custom_set_event(chart->widget, nullptr, nullptr);
			vgui_custom_set_size(chart->widget, nullptr, nullptr);
			vgui_widget_destroy(chart->widget);
			chart->widget = nullptr;
		}
		delete chart;
	}

	vgui_widget* vgui_chart_widget(vgui_chart* chart)
	{
		return chart ? chart->widget : nullptr;
	}

	void vgui_chart_set_title(vgui_chart* chart, const char* title)
	{
		if (!chart) return;
		chart->title = vgchart::sanitize_utf8(title);
		if (chart->widget)
			vgui_widget_set_a11y_name(chart->widget,
				chart->title.empty() ? "Chart" : chart->title.c_str());
		vgchart::chart_invalidate(chart);
	}

	vgui_series_id vgui_chart_add_series(vgui_chart* chart,
		const vgui_chart_series_desc* desc)
	{
		if (!chart || !desc)
		{
			if (chart) vgchart::set_chart_error(chart, "invalid series description");
			return 0;
		}
		const vgui_chart_series_desc d = vgchart::copy_series_desc(desc);
		std::string error;
		if (!vgchart::valid_series_desc(chart, d, error))
		{
			vgchart::set_chart_error(chart, error.c_str());
			return 0;
		}

		auto series = std::make_unique<vgchart::series_data>();
		series->id = chart->next_id++;
		if (chart->next_id == 0) chart->next_id = 1;
		series->key = d.key;
		series->name = (d.name && *d.name) ? d.name : d.key;
		series->unit = d.unit ? d.unit : "";
		series->stack_group = d.stack_group ? d.stack_group : "";
		series->kind = d.kind;
		series->axis = 0;
		series->color = d.color;
		series->line_width = d.line_width > 0.0f ? d.line_width : 2.0f;
		series->fill_alpha = d.fill_alpha < 0.0f ? 0.15f
			: (d.fill_alpha > 1.0f ? 1.0f : d.fill_alpha);
		series->visible = d.visible != 0;
		series->stacked = d.stacked != 0
			&& (d.kind == VGUI_CHART_BAR || d.kind == VGUI_CHART_AREA);

		const vgui_series_id id = series->id;
		chart->series.push_back(std::move(series));
		chart->cache.emplace_back();
		vgchart::chart_invalidate(chart);
		return id;
	}

	vgui_series_id vgui_chart_find_series(vgui_chart* chart, const char* key)
	{
		if (!chart || !key) return 0;
		for (const auto& series : chart->series)
		{
			if (series && series->key == key)
				return series->id;
		}
		return 0;
	}

	int vgui_chart_remove_series(vgui_chart* chart, vgui_series_id series)
	{
		if (!chart)
			return 0;
		const int index = vgchart::find_series_index(chart, series);
		if (index < 0)
		{
			vgchart::set_chart_error(chart, "series not found");
			return 0;
		}
		chart->series.erase(chart->series.begin() + index);
		if ((size_t)index < chart->cache.size())
			chart->cache.erase(chart->cache.begin() + index);
		vgchart::chart_invalidate(chart);
		return 1;
	}

	int vgui_chart_update_series(vgui_chart* chart, vgui_series_id series,
		const vgui_chart_series_desc* desc)
	{
		if (!chart || !desc)
		{
			if (chart) vgchart::set_chart_error(chart, "invalid series description");
			return 0;
		}
		const int index = vgchart::find_series_index(chart, series);
		if (index < 0)
		{
			vgchart::set_chart_error(chart, "series not found");
			return 0;
		}
		vgui_chart_series_desc d = vgchart::copy_series_desc(desc);
		vgchart::series_data& target = *chart->series[(size_t)index];

		if (d.key && *d.key && target.key != d.key)
		{
			if (vgchart::key_exists(chart, d.key, &target))
			{
				vgchart::set_chart_error(chart, "duplicate series key");
				return 0;
			}
			target.key = d.key;
		}
		if (d.kind != VGUI_CHART_LINE && d.kind != VGUI_CHART_BAR
			&& d.kind != VGUI_CHART_AREA)
		{
			vgchart::set_chart_error(chart, "invalid series kind");
			return 0;
		}
		if (d.kind != target.kind && !target.storage.empty())
		{
			vgchart::set_chart_error(chart, "cannot change series kind after points exist");
			return 0;
		}
		target.kind = d.kind;
		if (d.name) target.name = *d.name ? d.name : target.key;
		if (d.unit) target.unit = d.unit;
		if (d.stack_group) target.stack_group = d.stack_group;
		target.color = d.color;
		target.line_width = d.line_width > 0.0f ? d.line_width : 2.0f;
		target.fill_alpha = d.fill_alpha < 0.0f ? 0.15f
			: (d.fill_alpha > 1.0f ? 1.0f : d.fill_alpha);
		target.visible = d.visible != 0;
		target.stacked = d.stacked != 0
			&& (target.kind == VGUI_CHART_BAR || target.kind == VGUI_CHART_AREA);
		vgchart::chart_invalidate(chart);
		return 1;
	}

	int vgui_chart_reserve(vgui_chart* chart, vgui_series_id series, size_t capacity)
	{
		if (!chart)
			return 0;
		const int index = vgchart::find_series_index(chart, series);
		if (index < 0)
		{
			vgchart::set_chart_error(chart, "series not found");
			return 0;
		}
		try
		{
			chart->series[(size_t)index]->storage.reserve(capacity);
		}
		catch (const std::bad_alloc&)
		{
			vgchart::set_chart_error(chart, "out of memory");
			return 0;
		}
		return 1;
	}

	int vgui_chart_set_ring_capacity(vgui_chart* chart, vgui_series_id series,
		size_t capacity)
	{
		if (!chart)
			return 0;
		const int index = vgchart::find_series_index(chart, series);
		if (index < 0)
		{
			vgchart::set_chart_error(chart, "series not found");
			return 0;
		}
		vgchart::series_data& target = *chart->series[(size_t)index];
		try
		{
			target.normalize();
			if (capacity == 0)
			{
				target.ring_capacity = 0;
			}
			else
			{
				if (target.storage.size() > capacity)
					target.storage.erase(target.storage.begin(),
						target.storage.begin()
						+ (std::ptrdiff_t)(target.storage.size() - capacity));
				target.storage.reserve(capacity);
				target.ring_capacity = capacity;
			}
			target.head = 0;
		}
		catch (const std::bad_alloc&)
		{
			vgchart::set_chart_error(chart, "out of memory");
			return 0;
		}
		++target.revision;
		vgchart::chart_invalidate(chart);
		return 1;
	}

	int vgui_chart_set_points(vgui_chart* chart, vgui_series_id series,
		const vgui_chart_point* points, size_t count)
	{
		if (!chart)
			return 0;
		if (count > 0 && !points)
		{
			vgchart::set_chart_error(chart, "points is NULL");
			return 0;
		}
		const int index = vgchart::find_series_index(chart, series);
		if (index < 0)
		{
			vgchart::set_chart_error(chart, "series not found");
			return 0;
		}
		vgchart::series_data& target = *chart->series[(size_t)index];
		if (count == 0)
		{
			target.storage.clear();
			target.head = 0;
			++target.revision;
			vgchart::chart_invalidate(chart);
			return 1;
		}
		try
		{
			std::vector<vgui_chart_point> copy(points, points + count);
			for (const vgui_chart_point& p : copy)
			{
				if (!std::isfinite(p.x))
				{
					vgchart::set_chart_error(chart, "point x must be finite");
					return 0;
				}
			}
			std::stable_sort(copy.begin(), copy.end(),
				[](const vgui_chart_point& a, const vgui_chart_point& b) {
					return a.x < b.x;
				});
			if (target.ring_capacity && copy.size() > target.ring_capacity)
				copy.erase(copy.begin(),
					copy.begin() + (std::ptrdiff_t)(copy.size() - target.ring_capacity));
			target.storage.swap(copy);
			target.head = 0;
		}
		catch (const std::bad_alloc&)
		{
			vgchart::set_chart_error(chart, "out of memory");
			return 0;
		}
		++target.revision;
		vgchart::chart_invalidate(chart);
		return 1;
	}

	int vgui_chart_append_points(vgui_chart* chart, vgui_series_id series,
		const vgui_chart_point* points, size_t count)
	{
		if (!chart)
			return 0;
		if (count == 0) return 1;
		if (!points)
		{
			vgchart::set_chart_error(chart, "points is NULL");
			return 0;
		}
		const int index = vgchart::find_series_index(chart, series);
		if (index < 0)
		{
			vgchart::set_chart_error(chart, "series not found");
			return 0;
		}
		vgchart::series_data& target = *chart->series[(size_t)index];

		double previous = target.empty() ? -std::numeric_limits<double>::infinity()
			: target.at(target.size() - 1).x;
		for (size_t i = 0; i < count; ++i)
		{
			if (!std::isfinite(points[i].x))
			{
				vgchart::set_chart_error(chart, "point x must be finite");
				return 0;
			}
			if (points[i].x < previous)
			{
				vgchart::set_chart_error(chart, "point x must be non-decreasing");
				return 0;
			}
			previous = points[i].x;
		}

		try
		{
			if (target.ring_capacity == 0)
			{
				target.storage.insert(target.storage.end(), points, points + count);
			}
			else
			{
				const size_t capacity = target.ring_capacity;
				target.storage.reserve(capacity);
				size_t i = 0;
				if (target.storage.size() < capacity)
				{
					const size_t room = capacity - target.storage.size();
					const size_t take = std::min(room, count);
					target.storage.insert(target.storage.end(), points, points + take);
					i = take;
				}
				for (; i < count; ++i)
				{
					if (target.storage.size() < capacity)
						target.storage.push_back(points[i]);
					else
					{
						target.storage[target.head] = points[i];
						target.head = (target.head + 1) % capacity;
					}
				}
			}
		}
		catch (const std::bad_alloc&)
		{
			vgchart::set_chart_error(chart, "out of memory");
			return 0;
		}
		++target.revision;
		vgchart::chart_invalidate(chart);
		return 1;
	}

	int vgui_chart_clear_points(vgui_chart* chart, vgui_series_id series)
	{
		if (!chart)
			return 0;
		const int index = vgchart::find_series_index(chart, series);
		if (index < 0)
		{
			vgchart::set_chart_error(chart, "series not found");
			return 0;
		}
		vgchart::series_data& target = *chart->series[(size_t)index];
		target.storage.clear();
		target.head = 0;
		++target.revision;
		vgchart::chart_invalidate(chart);
		return 1;
	}

	size_t vgui_chart_point_count(vgui_chart* chart, vgui_series_id series)
	{
		if (!chart)
			return 0;
		const int index = vgchart::find_series_index(chart, series);
		return index < 0 ? 0 : chart->series[(size_t)index]->size();
	}

	int vgui_chart_set_view(vgui_chart* chart, double x0, double x1)
	{
		if (!chart)
			return 0;
		if (!std::isfinite(x0) || !std::isfinite(x1) || !(x1 > x0))
		{
			vgchart::set_chart_error(chart, "invalid x view");
			return 0;
		}
		chart->manual_view = true;
		chart->follow = false;
		chart->view_x0 = x0;
		chart->view_x1 = x1;
		vgchart::chart_invalidate(chart);
		return 1;
	}

	int vgui_chart_get_view(vgui_chart* chart, double* x0, double* x1)
	{
		if (!chart || !chart->manual_view)
			return 0;
		if (x0) *x0 = chart->view_x0;
		if (x1) *x1 = chart->view_x1;
		return 1;
	}

	int vgui_chart_reset_view(vgui_chart* chart)
	{
		if (!chart)
			return 0;
		chart->manual_view = false;
		chart->follow = chart->options.follow != 0;
		vgchart::chart_invalidate(chart);
		return 1;
	}

	int vgui_chart_set_follow(vgui_chart* chart, int follow)
	{
		if (!chart)
			return 0;
		chart->follow = follow != 0;
		if (chart->follow)
			chart->manual_view = false;
		vgchart::chart_invalidate(chart);
		return 1;
	}

	int vgui_chart_get_follow(vgui_chart* chart)
	{
		return chart ? (chart->follow ? 1 : 0) : 0;
	}

	int vgui_chart_set_options(vgui_chart* chart,
		const vgui_chart_options* options)
	{
		if (!chart || !options)
		{
			if (chart) vgchart::set_chart_error(chart, "invalid options");
			return 0;
		}
		vgui_chart_options d = vgchart::copy_options(options);
		if (!std::isfinite(d.window) || d.window < 0.0)
			d.window = 0.0;
		if (d.decimation != VGUI_CHART_DECIMATION_NONE)
			d.decimation = VGUI_CHART_DECIMATION_MIN_MAX;
		chart->options = d;
		chart->follow = d.follow != 0 && !chart->manual_view;
		vgchart::chart_invalidate(chart);
		return 1;
	}

	int vgui_chart_set_axis(vgui_chart* chart, int axis,
		const vgui_chart_axis_desc* desc)
	{
		if (!chart || !desc)
		{
			if (chart) vgchart::set_chart_error(chart, "invalid axis description");
			return 0;
		}
		if (axis != 0 && axis != 1)
		{
			vgchart::set_chart_error(chart, "axis must be 0 (x) or 1 (y)");
			return 0;
		}
		vgchart::axis_config config;
		vgchart::axis_from_desc(config, desc);
		if (axis == 1)
			config.scale = VGUI_CHART_SCALE_LINEAR;
		if (!std::isfinite(config.min) || !std::isfinite(config.max))
		{
			vgchart::set_chart_error(chart, "axis range must be finite");
			return 0;
		}
		if (axis == 0) chart->x_axis = config;
		else chart->y_axis = config;
		vgchart::chart_invalidate(chart);
		return 1;
	}

	int vgui_chart_set_axis_format(vgui_chart* chart, int axis,
		vgui_chart_format_fn format, void* user)
	{
		if (!chart)
			return 0;
		if (axis != 0 && axis != 1)
		{
			vgchart::set_chart_error(chart, "axis must be 0 (x) or 1 (y)");
			return 0;
		}
		vgchart::axis_config& config = axis == 0 ? chart->x_axis : chart->y_axis;
		config.format = format;
		config.format_user = user;
		vgchart::chart_invalidate(chart);
		return 1;
	}

	int vgui_chart_set_palette(vgui_chart* chart, const vgui_color* colors,
		size_t count)
	{
		if (!chart)
			return 0;
		chart->palette.clear();
		if (colors && count > 0)
		{
			count = std::min<size_t>(count, 10);
			chart->palette.assign(colors, colors + count);
		}
		vgchart::chart_invalidate(chart);
		return 1;
	}

	int vgui_chart_set_theme(vgui_chart* chart, const vgui_chart_theme* theme)
	{
		if (!chart)
			return 0;
		if (theme)
		{
			size_t bytes = sizeof(chart->theme);
			chart->theme = {};
			if (theme->struct_size && theme->struct_size < bytes)
				bytes = theme->struct_size;
			std::memcpy(&chart->theme, theme, bytes);
			chart->theme_custom = true;
		}
		else
		{
			chart->theme_custom = false;
			vgchart::derive_theme(chart->ctx, &chart->theme);
		}
		vgchart::chart_invalidate(chart);
		return 1;
	}

	int vgui_chart_get_stats(vgui_chart* chart, vgui_chart_stats* stats)
	{
		if (!chart || !stats)
			return 0;
		vgui_chart_stats copy = chart->stats;
		size_t bytes = sizeof(copy);
		if (stats->struct_size && stats->struct_size < bytes)
			bytes = stats->struct_size;
		std::memcpy(stats, &copy, bytes);
		return 1;
	}

	int vgui_chart_get_axis_ticks(vgui_chart* chart, int axis,
		vgui_chart_tick* out, size_t cap)
	{
		if (!chart || axis < 0 || axis > 1)
			return 0;
		if (!chart->layout.valid)
			return 0;
		const std::vector<vgchart::tick_value>& source = axis == 0
			? chart->layout.xticks : chart->layout.yticks;
		if (out)
		{
			const size_t count = std::min(cap, source.size());
			for (size_t i = 0; i < count; ++i)
			{
				vgui_chart_tick copy = {};
				copy.struct_size = (uint32_t)sizeof(copy);
				copy.value = source[i].value;
				copy.text = source[i].text.c_str();
				size_t bytes = sizeof(copy);
				if (out[i].struct_size && out[i].struct_size < bytes)
					bytes = out[i].struct_size;
				std::memcpy(&out[i], &copy, bytes);
			}
		}
		return (int)source.size();
	}

	int vgui_chart_export_csv(vgui_chart* chart, uint32_t flags,
		vgui_chart_write_fn write, void* user)
	{
		return vgchart::chart_export_csv_impl(chart, nullptr, flags, write, user);
	}

	const char* vgui_chart_last_error(vgui_chart* chart)
	{
		return chart ? chart->last_error.c_str() : "";
	}

} /* extern "C" */
