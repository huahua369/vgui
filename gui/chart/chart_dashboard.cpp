#include "chart_internal.h"

#include "tools/tools.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <new>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace vgchart {

		using json = nlohmann::json;

		struct config_error : std::runtime_error {
			explicit config_error(const std::string& text)
				: std::runtime_error(text) {}
		};

		struct pending_series {
			std::string key;
			std::string name;
			std::string unit;
			std::string stack_group;
			int kind = VGUI_CHART_LINE;
			vgui_color color = 0;
			float line_width = 2.0f;
			float fill_alpha = 0.15f;
			bool visible = true;
			bool stacked = false;
		};

		struct pending_zone {
			double from = 0.0;
			double to = 0.0;
			vgui_color color = 0;
			std::string label;
		};

		struct pending_card {
			dashboard_card_kind kind = dashboard_card_kind::kpi;
			std::string id;
			std::string title;
			std::string value_key;
			std::string delta_key;
			std::string text_key;
			std::string status_key;
			std::string format;
			std::string unit;
			int span = 1;
			bool has_value = false;
			double value = 0.0;
			int status = VGUI_CHART_STATUS_NORMAL;
			std::string status_text;
			/* Snapshot data section, validated while parsing. */
			std::map<std::string, std::vector<vgui_chart_point>> data;

			vgui_chart_options options = {};
			axis_config x_axis;
			axis_config y_axis;
			std::vector<pending_series> series;
			bool view_manual = false;
			double view_x0 = 0.0;
			double view_x1 = 0.0;
			int view_follow = -1;

			double gauge_min = 0.0;
			double gauge_max = 100.0;
			double gauge_start_angle = 135.0;
			double gauge_sweep = 270.0;
			bool gauge_show_ticks = true;
			bool gauge_show_value = true;
			std::string gauge_label;
			std::string gauge_unit;
			std::string gauge_format;
			std::vector<pending_zone> zones;
		};

		struct pending_dashboard {
			std::string title;
			std::string mode = "stack";	/* stack | wrap | grid | columns */
			int columns = 2;
			float gap = 10.0f;
			float padding = 12.0f;
			std::vector<pending_card> cards;
			std::map<std::string, json> runtime;
		};

		struct build_result {
			vgui_widget* root = nullptr;
			vgui_widget* title_label = nullptr;
			std::vector<dashboard_card> cards;
			std::vector<vgui_chart*> charts;
			std::vector<vgui_gauge*> gauges;
		};

		std::string path_join(const std::string& base, const char* key)
		{
			if (base.empty()) return key;
			return base + "." + key;
		}

		std::string path_index(const std::string& base, size_t index)
		{
			return base + "[" + std::to_string(index) + "]";
		}

		const json& require_object(const json& parent, const char* key,
			const std::string& path)
		{
			auto it = parent.find(key);
			if (it == parent.end() || !it->is_object())
				throw config_error(path_join(path, key) + ": expected object");
			return *it;
		}

		const json& require_array(const json& parent, const char* key,
			const std::string& path)
		{
			auto it = parent.find(key);
			if (it == parent.end() || !it->is_array())
				throw config_error(path_join(path, key) + ": expected array");
			return *it;
		}

		std::string require_string(const json& parent, const char* key,
			const std::string& path)
		{
			auto it = parent.find(key);
			if (it == parent.end() || !it->is_string())
				throw config_error(path_join(path, key) + ": expected string");
			return it->get<std::string>();
		}

		std::string optional_string(const json& parent, const char* key,
			const std::string& fallback)
		{
			auto it = parent.find(key);
			if (it == parent.end() || it->is_null())
				return fallback;
			if (!it->is_string())
				throw config_error(std::string(key) + ": expected string");
			return it->get<std::string>();
		}

		double optional_number(const json& parent, const char* key,
			double fallback, const std::string& path)
		{
			auto it = parent.find(key);
			if (it == parent.end() || it->is_null())
				return fallback;
			if (!it->is_number())
				throw config_error(path_join(path, key) + ": expected number");
			const double value = it->get<double>();
			if (!std::isfinite(value))
				throw config_error(path_join(path, key) + ": must be finite");
			return value;
		}

		int optional_int(const json& parent, const char* key, int fallback,
			const std::string& path)
		{
			auto it = parent.find(key);
			if (it == parent.end() || it->is_null())
				return fallback;
			if (it->is_boolean())
				return it->get<bool>() ? 1 : 0;
			if (!it->is_number_integer())
				throw config_error(path_join(path, key) + ": expected integer");
			return it->get<int>();
		}

		bool optional_bool(const json& parent, const char* key, bool fallback,
			const std::string& path)
		{
			auto it = parent.find(key);
			if (it == parent.end() || it->is_null())
				return fallback;
			if (it->is_boolean())
				return it->get<bool>();
			if (it->is_number_integer())
				return it->get<long long>() != 0;
			throw config_error(path_join(path, key) + ": expected boolean");
		}

		bool hex_digit(char c, unsigned& out)
		{
			if (c >= '0' && c <= '9') { out = (unsigned)(c - '0'); return true; }
			if (c >= 'a' && c <= 'f') { out = (unsigned)(c - 'a' + 10); return true; }
			if (c >= 'A' && c <= 'F') { out = (unsigned)(c - 'A' + 10); return true; }
			return false;
		}

		vgui_color parse_color(const json& value, const std::string& path)
		{
			if (value.is_null())
				return 0;
			if (value.is_number_unsigned())
				return (vgui_color)value.get<uint32_t>();
			if (value.is_number_integer())
				return (vgui_color)(uint32_t)value.get<long long>();
			if (!value.is_string())
				throw config_error(path + ": expected color string or integer");

			std::string text = value.get<std::string>();
			if (!text.empty() && text[0] == '#')
				text.erase(text.begin());
			if (text.size() != 3 && text.size() != 6 && text.size() != 8)
				throw config_error(path + ": color must be #RGB, #RRGGBB or #AARRGGBB");
			unsigned values[8] = {};
			for (size_t i = 0; i < text.size(); ++i)
			{
				unsigned digit = 0;
				if (!hex_digit(text[i], digit))
					throw config_error(path + ": invalid color digit");
				values[i] = digit;
			}
			if (text.size() == 3)
			{
				const unsigned r = values[0] * 17;
				const unsigned g = values[1] * 17;
				const unsigned b = values[2] * 17;
				return 0xFF000000u | (r << 16) | (g << 8) | b;
			}
			if (text.size() == 6)
			{
				const unsigned r = (values[0] << 4) | values[1];
				const unsigned g = (values[2] << 4) | values[3];
				const unsigned b = (values[4] << 4) | values[5];
				return 0xFF000000u | (r << 16) | (g << 8) | b;
			}
			return (vgui_color)((values[0] << 28) | (values[1] << 24)
				| (values[2] << 20) | (values[3] << 16)
				| (values[4] << 12) | (values[5] << 8)
				| (values[6] << 4) | values[7]);
		}

		std::string color_text(vgui_color color)
		{
			char text[16] = {};
			std::snprintf(text, sizeof(text), "#%08X", color);
			return text;
		}

		int parse_series_kind(const json& value, const std::string& path)
		{
			if (value.is_number_integer())
			{
				const int kind = value.get<int>();
				if (kind == VGUI_CHART_LINE || kind == VGUI_CHART_BAR
					|| kind == VGUI_CHART_AREA)
					return kind;
				throw config_error(path + ": unknown series kind");
			}
			if (!value.is_string())
				throw config_error(path + ": expected series kind string");
			const std::string text = value.get<std::string>();
			if (text == "line") return VGUI_CHART_LINE;
			if (text == "bar") return VGUI_CHART_BAR;
			if (text == "area") return VGUI_CHART_AREA;
			throw config_error(path + ": unknown series kind '" + text + "'");
		}

		int parse_scale(const json& value, const std::string& path)
		{
			if (value.is_number_integer())
			{
				const int scale = value.get<int>();
				if (scale == VGUI_CHART_SCALE_LINEAR
					|| scale == VGUI_CHART_SCALE_TIME
					|| scale == VGUI_CHART_SCALE_DATETIME)
					return scale;
			}
			if (value.is_string())
			{
				const std::string text = value.get<std::string>();
				if (text == "linear") return VGUI_CHART_SCALE_LINEAR;
				if (text == "time" || text == "duration")
					return VGUI_CHART_SCALE_TIME;
				if (text == "datetime" || text == "date_time"
					|| text == "epoch")
					return VGUI_CHART_SCALE_DATETIME;
			}
			throw config_error(path + ": unknown axis scale");
		}

		int parse_decimation(const json& value, const std::string& path)
		{
			if (value.is_number_integer())
			{
				const int mode = value.get<int>();
				if (mode == VGUI_CHART_DECIMATION_NONE
					|| mode == VGUI_CHART_DECIMATION_MIN_MAX)
					return mode;
			}
			if (value.is_string())
			{
				const std::string text = value.get<std::string>();
				if (text == "none") return VGUI_CHART_DECIMATION_NONE;
				if (text == "min_max" || text == "minmax")
					return VGUI_CHART_DECIMATION_MIN_MAX;
			}
			throw config_error(path + ": unknown decimation mode");
		}

		axis_config parse_axis(const json& parent, const char* key,
			const std::string& path, const axis_config& fallback)
		{
			auto it = parent.find(key);
			if (it == parent.end() || it->is_null())
				return fallback;
			if (!it->is_object())
				throw config_error(path_join(path, key) + ": expected object");
			const json& source = *it;
			const std::string base = path_join(path, key);
			axis_config axis = fallback;
			if (source.contains("scale"))
				axis.scale = parse_scale(source["scale"], base + ".scale");
			axis.min = optional_number(source, "min", fallback.min, base);
			axis.max = optional_number(source, "max", fallback.max, base);
			axis.tick_count = std::clamp(optional_int(source, "ticks",
				optional_int(source, "tick_count", fallback.tick_count, base),
				base), 2, 32);
			axis.include_zero = optional_bool(source, "include_zero",
				fallback.include_zero != 0, base) ? 1 : 0;
			axis.grid = optional_bool(source, "grid", fallback.grid != 0, base) ? 1 : 0;
			axis.label = optional_string(source, "label", fallback.label);
			axis.unit = optional_string(source, "unit", fallback.unit);
			{
				auto tz = source.find("timezone");
				if (tz == source.end() || tz->is_null())
					axis.time_zone = fallback.time_zone;
				else if (tz->is_string())
				{
					const std::string zone = tz->get<std::string>();
					if (zone == "local") axis.time_zone = 1;
					else if (zone == "utc" || zone.empty()) axis.time_zone = 0;
					else throw config_error(base + ".timezone: expected \"utc\" or \"local\"");
				}
				else
					throw config_error(base + ".timezone: expected string");
			}
			if (source.contains("format"))
			{
				if (!source["format"].is_string())
					throw config_error(base + ".format: expected string");
				axis.pattern = source["format"].get<std::string>();
			}
			else if (source.contains("format_pattern"))
			{
				if (!source["format_pattern"].is_string())
					throw config_error(base + ".format_pattern: expected string");
				axis.pattern = source["format_pattern"].get<std::string>();
			}
			if (axis.min < axis.max
				&& (!std::isfinite(axis.min) || !std::isfinite(axis.max)))
				throw config_error(base + ": axis range must be finite");
			return axis;
		}

		void parse_options(const json& parent, pending_card& card,
			const std::string& path)
		{
			if (!parent.contains("options") || parent["options"].is_null())
				return;
			if (!parent["options"].is_object())
				throw config_error(path + ".options: expected object");
			const json& source = parent["options"];
			const std::string base = path + ".options";
			vgui_chart_options defaults = {};
			vgui_chart_options_default(&defaults);
			card.options.legend = optional_bool(source, "legend",
				defaults.legend != 0, base) ? 1 : 0;
			card.options.tooltip = optional_bool(source, "tooltip",
				defaults.tooltip != 0, base) ? 1 : 0;
			card.options.crosshair = optional_bool(source, "crosshair",
				defaults.crosshair != 0, base) ? 1 : 0;
			card.options.follow = optional_bool(source, "follow",
				defaults.follow != 0, base) ? 1 : 0;
			card.options.window = std::max(0.0,
				optional_number(source, "window", 0.0, base));
			card.options.decimation = defaults.decimation;
			if (source.contains("decimation"))
				card.options.decimation = parse_decimation(
					source["decimation"], base + ".decimation");
		}

		void parse_series(const json& parent, pending_card& card,
			const std::string& path)
		{
			if (!parent.contains("series") || parent["series"].is_null())
				return;
			const json& list = require_array(parent, "series", path);
			const std::string base = path + ".series";
			std::unordered_set<std::string> keys;
			for (size_t i = 0; i < list.size(); ++i)
			{
				const json& item = list[i];
				const std::string item_path = path_index(base, i);
				if (!item.is_object())
					throw config_error(item_path + ": expected object");
				pending_series series;
				series.key = require_string(item, "key", item_path);
				if (series.key.empty())
					throw config_error(item_path + ".key: must not be empty");
				if (!keys.insert(series.key).second)
					throw config_error(item_path + ".key: duplicate series key");
				series.name = optional_string(item, "name", series.key);
				series.unit = optional_string(item, "unit", "");
				series.stack_group = optional_string(item, "stack_group", "");
				if (item.contains("kind"))
					series.kind = parse_series_kind(item["kind"], item_path + ".kind");
				if (item.contains("axis"))
				{
					const int axis = optional_int(item, "axis", 0, item_path);
					if (axis != 0)
						throw config_error(item_path + ".axis: only axis 0 is supported in schema version 1");
				}
				if (item.contains("color") && !item["color"].is_null())
					series.color = parse_color(item["color"], item_path + ".color");
				series.line_width = (float)std::clamp(
					optional_number(item, "line_width", 2.0, item_path),
					0.5, 16.0);
				series.fill_alpha = (float)std::clamp(
					optional_number(item, "fill_alpha", 0.15, item_path),
					0.0, 1.0);
				series.visible = optional_bool(item, "visible", true, item_path);
				series.stacked = optional_bool(item, "stacked", false, item_path);
				card.series.push_back(std::move(series));
			}
		}

		void parse_view(const json& parent, pending_card& card,
			const std::string& path)
		{
			if (!parent.contains("view") || parent["view"].is_null())
				return;
			if (!parent["view"].is_object())
				throw config_error(path + ".view: expected object");
			const json& source = parent["view"];
			const std::string base = path + ".view";
			if (source.contains("x0") || source.contains("x1"))
			{
				const double x0 = optional_number(source, "x0", 0.0, base);
				const double x1 = optional_number(source, "x1", 0.0, base);
				if (!(x1 > x0))
					throw config_error(base + ": x1 must be greater than x0");
				card.view_manual = true;
				card.view_x0 = x0;
				card.view_x1 = x1;
			}
			if (source.contains("follow"))
				card.view_follow = optional_bool(source, "follow", true, base) ? 1 : 0;
		}

		vgui_color status_color(const vgui_chart_theme& theme, int status)
		{
			switch (status)
			{
			case VGUI_CHART_STATUS_INFO: return theme.series[0];
			case VGUI_CHART_STATUS_WARNING: return theme.warning;
			case VGUI_CHART_STATUS_CRITICAL: return theme.critical;
			case VGUI_CHART_STATUS_STALE: return theme.muted_text;
			default: return theme.muted_text;
			}
		}

		std::string format_kpi(const dashboard_card& card, double value)
		{
			if (!std::isfinite(value))
				return "--";
			std::string text;
			if (!try_format_pattern(card.format, value, text))
			{
				char buffer[64] = {};
				std::snprintf(buffer, sizeof(buffer), "%.6g", value);
				text = buffer;
			}
			if (!card.unit.empty())
			{
				text += " ";
				text += card.unit;
			}
			return text;
		}

		vgui_color theme_panel_bg(vgui_context* ctx)
		{
			vgui_theme theme = {};
			if (ctx)
				vgui_context_get_theme(ctx, &theme);
			return theme.panel_bg ? theme.panel_bg : 0xFF1E232Bu;
		}

		vgui_color theme_panel_border(vgui_context* ctx)
		{
			vgui_theme theme = {};
			if (ctx)
				vgui_context_get_theme(ctx, &theme);
			return theme.panel_border ? theme.panel_border : 0xFF2E3440u;
		}

		vgui_color theme_text(vgui_context* ctx)
		{
			vgui_theme theme = {};
			if (ctx)
				vgui_context_get_theme(ctx, &theme);
			return theme.text ? theme.text : 0xFFE8EEF8u;
		}

		vgui_color theme_muted(vgui_context* ctx)
		{
			vgui_theme theme = {};
			if (ctx)
				vgui_context_get_theme(ctx, &theme);
			const vgui_color text = theme.text ? theme.text : 0xFFE8EEF8u;
			return mix_color(text, theme_panel_bg(ctx), 0.45f);
		}

		vgui_widget* add_label(vgui_widget* parent, const std::string& text,
			float font, vgui_color color)
		{
			vgui_widget* label = vgui_widget_add(parent, VGUI_LABEL, text.c_str());
			if (!label)
				return nullptr;
			vgui_widget_set_font_size(label, font);
			vgui_widget_set_text_color(label, color);
			vgui_widget_set_align_self(label, VGUI_ALIGN_STRETCH);
			return label;
		}

		/* Preferred logical card sizes; shared by the kind builders and the
		 * grid span widening. */
		constexpr float kChartCardMinW = 220.0f;
		constexpr float kChartCardMinH = 200.0f;
		constexpr float kGaugeCardMinW = 180.0f;
		constexpr float kGaugeCardMinH = 220.0f;
		constexpr float kKpiCardMinW = 160.0f;
		constexpr float kKpiCardMinH = 110.0f;

		void set_axis(vgui_chart* chart, int index, const axis_config& axis)
		{
			vgui_chart_axis_desc desc = {};
			vgui_chart_axis_desc_default(&desc);
			desc.scale = axis.scale;
			desc.min = axis.min;
			desc.max = axis.max;
			desc.tick_count = axis.tick_count;
			desc.include_zero = axis.include_zero;
			desc.grid = axis.grid;
			desc.label = axis.label.c_str();
			desc.unit = axis.unit.c_str();
			desc.format_pattern = axis.pattern.empty() ? nullptr : axis.pattern.c_str();
			desc.time_zone = axis.time_zone;
			vgui_chart_set_axis(chart, index, &desc);
		}

		void destroy_build(build_result& built)
		{
			for (vgui_chart* chart : built.charts)
				vgui_chart_destroy(chart);
			built.charts.clear();
			for (vgui_gauge* gauge : built.gauges)
				vgui_gauge_destroy(gauge);
			built.gauges.clear();
			if (built.root)
			{
				vgui_widget_destroy(built.root);
				built.root = nullptr;
			}
			built.title_label = nullptr;
			built.cards.clear();
		}

		bool build_chart_card(vgui_dashboard* dashboard, const pending_card& pending,
			dashboard_card& card, build_result& built, std::string& error)
		{
			card.chart = vgui_chart_create(card.panel);
			if (!card.chart)
			{
				error = "card '" + pending.id + "': cannot create chart";
				return false;
			}
			built.charts.push_back(card.chart);
			if (!pending.title.empty())
				vgui_chart_set_title(card.chart, pending.title.c_str());
			vgui_chart_set_options(card.chart, &pending.options);
			set_axis(card.chart, 0, pending.x_axis);
			set_axis(card.chart, 1, pending.y_axis);

			for (const pending_series& series : pending.series)
			{
				vgui_chart_series_desc desc = {};
				vgui_chart_series_desc_default(&desc);
				desc.key = series.key.c_str();
				desc.name = series.name.c_str();
				desc.kind = series.kind;
				desc.color = series.color;
				desc.line_width = series.line_width;
				desc.fill_alpha = series.fill_alpha;
				desc.visible = series.visible ? 1 : 0;
				desc.stacked = series.stacked ? 1 : 0;
				desc.stack_group = series.stack_group.empty()
					? nullptr : series.stack_group.c_str();
				desc.unit = series.unit.empty() ? nullptr : series.unit.c_str();
				if (!vgui_chart_add_series(card.chart, &desc))
				{
					error = "card '" + pending.id + "': "
						+ vgui_chart_last_error(card.chart);
					return false;
				}
			}

			if (pending.view_follow >= 0)
				vgui_chart_set_follow(card.chart, pending.view_follow);
			if (pending.view_manual)
				vgui_chart_set_view(card.chart, pending.view_x0, pending.view_x1);

			vgui_widget_set_min_size(card.panel, kChartCardMinW, kChartCardMinH);
			vgui_widget_set_grow(card.panel, 1.0f);
			(void)dashboard;
			return true;
		}

		bool build_gauge_card(vgui_dashboard* dashboard, const pending_card& pending,
			dashboard_card& card, build_result& built, std::string& error)
		{
			const vgui_color muted = theme_muted(dashboard->ctx);
			card.title_label = add_label(card.panel,
				pending.title.empty() ? pending.id : pending.title, 12.0f, muted);

			card.gauge = vgui_gauge_create(card.panel);
			if (!card.gauge)
			{
				error = "card '" + pending.id + "': cannot create gauge";
				return false;
			}
			built.gauges.push_back(card.gauge);

			vgui_gauge_options options = {};
			vgui_gauge_options_default(&options);
			options.min = pending.gauge_min;
			options.max = pending.gauge_max;
			options.start_angle = pending.gauge_start_angle;
			options.sweep = pending.gauge_sweep;
			options.show_ticks = pending.gauge_show_ticks ? 1 : 0;
			options.show_value = pending.gauge_show_value ? 1 : 0;
			options.label = pending.gauge_label.empty()
				? nullptr : pending.gauge_label.c_str();
			options.unit = pending.gauge_unit.empty()
				? nullptr : pending.gauge_unit.c_str();
			options.format_pattern = pending.gauge_format.empty()
				? nullptr : pending.gauge_format.c_str();

			std::vector<vgui_gauge_zone> zones;
			zones.reserve(pending.zones.size());
			for (const pending_zone& zone : pending.zones)
			{
				vgui_gauge_zone desc = {};
				desc.from = zone.from;
				desc.to = zone.to;
				desc.color = zone.color;
				desc.label = zone.label.empty() ? nullptr : zone.label.c_str();
				zones.push_back(desc);
			}
			options.zones = zones.empty() ? nullptr : zones.data();
			options.zone_count = zones.size();
			if (!vgui_gauge_set_options(card.gauge, &options))
			{
				error = "card '" + pending.id + "': "
					+ vgui_gauge_last_error(card.gauge);
				return false;
			}
			if (pending.has_value)
				vgui_gauge_set_value(card.gauge, pending.value);
			else
				vgui_gauge_set_value(card.gauge,
					std::numeric_limits<double>::quiet_NaN());

			vgui_widget* widget = vgui_gauge_widget(card.gauge);
			if (widget)
				vgui_widget_set_a11y_name(widget,
					pending.title.empty() ? pending.id.c_str() : pending.title.c_str());
			vgui_widget_set_min_size(card.panel, kGaugeCardMinW, kGaugeCardMinH);
			vgui_widget_set_grow(card.panel, 0.75f);
			return true;
		}

		bool build_kpi_card(vgui_dashboard* dashboard, const pending_card& pending,
			dashboard_card& card, build_result& built, std::string& error)
		{
			(void)built;
			const vgui_color text = theme_text(dashboard->ctx);
			const vgui_color muted = theme_muted(dashboard->ctx);
			const vgui_color warning = [&]() {
				vgui_theme theme = {};
				if (dashboard->ctx)
					vgui_context_get_theme(dashboard->ctx, &theme);
				return theme.toast_warning ? theme.toast_warning : 0xFFD29922u;
			}();
			(void)warning;

			card.title_label = add_label(card.panel,
				pending.title.empty() ? pending.id : pending.title, 12.0f, muted);
			card.value_label = add_label(card.panel,
				pending.has_value ? format_kpi(card, pending.value) : "--",
				26.0f, text);
			if (card.value_label)
			{
				vgui_widget_set_a11y_name(card.value_label,
					pending.title.empty() ? pending.id.c_str() : pending.title.c_str());
			}
			if (!pending.delta_key.empty())
			{
				card.delta_label = add_label(card.panel, "", 11.0f, muted);
			}
			if (!pending.text_key.empty())
			{
				card.subtitle_label = add_label(card.panel, "", 11.0f, muted);
			}
			if (!pending.status_key.empty())
			{
				card.status_label = add_label(card.panel, "", 11.0f, muted);
			}
			if (pending.has_value)
			{
				card.has_value = true;
				card.last_value = pending.value;
			}
			if (pending.status != VGUI_CHART_STATUS_NORMAL)
			{
				card.status = std::clamp(pending.status,
					(int)VGUI_CHART_STATUS_NORMAL, (int)VGUI_CHART_STATUS_STALE);
				if (card.status_label && !pending.status_text.empty())
				{
					vgui_widget_set_text(card.status_label,
						pending.status_text.c_str());
					vgui_widget_set_visible(card.status_label, 1);
					vgui_widget_set_text_color(card.status_label,
						status_color([&]() {
							vgui_chart_theme chart_theme = {};
							derive_theme(dashboard->ctx, &chart_theme);
							return chart_theme;
						}(), card.status));
				}
			}
			vgui_widget_set_min_size(card.panel, kKpiCardMinW, kKpiCardMinH);
			vgui_widget_set_grow(card.panel, 0.0f);
			(void)error;
			return true;
		}

		bool build_card(vgui_dashboard* dashboard, const pending_card& pending,
			vgui_widget* container, const pending_dashboard& config,
			build_result& built, std::string& error)
		{
			vgui_widget* panel = vgui_widget_add(container, VGUI_PANEL, nullptr);
			if (!panel)
			{
				error = "cannot create card panel";
				return false;
			}

			const vgui_color bg = mix_color(theme_panel_bg(dashboard->ctx),
				theme_text(dashboard->ctx), 0.035f);
			const vgui_color border = theme_panel_border(dashboard->ctx);
			vgui_widget_set_id(panel,
				(std::string("vgui_card_") + pending.id).c_str());
			vgui_widget_set_layout(panel, VGUI_LAYOUT_COLUMN);
			vgui_widget_set_padding(panel, 10, 10, 10, 10);
			vgui_widget_set_gap(panel, 4);
			vgui_widget_set_align_items(panel, VGUI_ALIGN_STRETCH);
			vgui_widget_set_align_self(panel, VGUI_ALIGN_STRETCH);
			vgui_widget_set_background(panel, bg);
			vgui_widget_set_border(panel, border, 1.0f);
			vgui_widget_set_rounding(panel, 6.0f);
			vgui_widget_set_a11y_name(panel,
				pending.title.empty() ? pending.id.c_str() : pending.title.c_str());

			dashboard_card card;
			card.kind = pending.kind;
			card.id = pending.id;
			card.title = pending.title;
			card.value_key = pending.value_key;
			card.delta_key = pending.delta_key;
			card.text_key = pending.text_key;
			card.status_key = pending.status_key;
			card.format = pending.format;
			card.unit = pending.unit;
			card.status_text = pending.status_text;
			card.span = pending.span;
			card.panel = panel;

			bool ok = false;
			switch (pending.kind)
			{
			case dashboard_card_kind::kpi:
				ok = build_kpi_card(dashboard, pending, card, built, error);
				break;
			case dashboard_card_kind::chart:
				ok = build_chart_card(dashboard, pending, card, built, error);
				break;
			case dashboard_card_kind::gauge:
				ok = build_gauge_card(dashboard, pending, card, built, error);
				break;
			}
			if (!ok)
				return false;

			/* Grid cells: a card spanning k cells must be able to grow across
			 * them, and its minimum width must cover the cells plus the gaps
			 * between them so the row never under-fills. */
			if (config.mode == "grid" && card.span > 1)
			{
				const float base_w = pending.kind == dashboard_card_kind::chart
					? kChartCardMinW
					: pending.kind == dashboard_card_kind::gauge
					? kGaugeCardMinW : kKpiCardMinW;
				const float base_h = pending.kind == dashboard_card_kind::chart
					? kChartCardMinH
					: pending.kind == dashboard_card_kind::gauge
					? kGaugeCardMinH : kKpiCardMinH;
				const float spanned = base_w * (float)card.span
					+ config.gap * (float)(card.span - 1);
				vgui_widget_set_min_size(panel, spanned, base_h);
				vgui_widget_set_grow(panel, (float)card.span);
			}
			built.cards.push_back(std::move(card));
			return true;
		}

		vgui_widget* add_row_panel(vgui_widget* parent, float gap)
		{
			vgui_widget* row = vgui_widget_add(parent, VGUI_PANEL, nullptr);
			if (!row)
				return nullptr;
			vgui_widget_set_id(row, "vgui_dashboard_row");
			vgui_widget_set_layout(row, VGUI_LAYOUT_ROW);
			vgui_widget_set_gap(row, gap);
			vgui_widget_set_align_items(row, VGUI_ALIGN_STRETCH);
			vgui_widget_set_align_self(row, VGUI_ALIGN_STRETCH);
			vgui_widget_set_background(row, 0);
			vgui_widget_set_border(row, 0, 0);
			vgui_widget_set_padding(row, 0, 0, 0, 0);
			return row;
		}

		bool build_dashboard(vgui_dashboard* dashboard,
			const pending_dashboard& config, build_result& built,
			std::string& error)
		{
			if (!dashboard || !dashboard->parent)
			{
				error = "dashboard has no parent widget";
				return false;
			}

			built.root = vgui_widget_add(dashboard->parent, VGUI_PANEL, nullptr);
			if (!built.root)
			{
				error = "cannot create dashboard panel";
				return false;
			}
			vgui_widget_set_id(built.root, "vgui_dashboard");
			vgui_widget_set_layout(built.root,
				config.mode == "wrap" ? VGUI_LAYOUT_WRAP
				: config.mode == "columns" ? VGUI_LAYOUT_ROW
				: VGUI_LAYOUT_COLUMN);
			vgui_widget_set_padding(built.root, config.padding, config.padding,
				config.padding, config.padding);
			vgui_widget_set_gap(built.root, config.gap);
			vgui_widget_set_align_items(built.root, VGUI_ALIGN_STRETCH);
			vgui_widget_set_background(built.root, 0);
			vgui_widget_set_border(built.root, 0, 0);
			vgui_widget_set_grow(built.root, 1.0f);
			if (!config.title.empty())
			{
				built.title_label = add_label(built.root, config.title, 16.0f,
					theme_text(dashboard->ctx));
				vgui_widget_set_a11y_name(built.root, config.title.c_str());
			}

			if (config.mode == "grid")
			{
				/* Row-major cells; a card that does not fit in the remaining
				 * columns of the current row starts a new row. */
				vgui_widget* row = nullptr;
				int cursor = 0;
				for (const pending_card& pending : config.cards)
				{
					const int span = std::max(1, std::min(pending.span,
						config.columns));
					if (!row || cursor + span > config.columns)
					{
						row = add_row_panel(built.root, config.gap);
						if (!row)
						{
							error = "cannot create grid row";
							return false;
						}
						cursor = 0;
					}
					pending_card placed = pending;
					placed.span = span;
					if (!build_card(dashboard, placed, row, config, built,
						error))
						return false;
					cursor += span;
				}
			}
			else if (config.mode == "columns")
			{
				/* Round-robin vertical stacks; reading order runs down each
				 * column before moving to the next one. */
				std::vector<vgui_widget*> columns;
				for (int i = 0; i < config.columns; ++i)
				{
					vgui_widget* column = vgui_widget_add(built.root,
						VGUI_PANEL, nullptr);
					if (!column)
					{
						error = "cannot create dashboard column";
						return false;
					}
					vgui_widget_set_id(column, "vgui_dashboard_column");
					vgui_widget_set_layout(column, VGUI_LAYOUT_COLUMN);
					vgui_widget_set_gap(column, config.gap);
					vgui_widget_set_align_items(column, VGUI_ALIGN_STRETCH);
					vgui_widget_set_align_self(column, VGUI_ALIGN_STRETCH);
					vgui_widget_set_grow(column, 1.0f);
					vgui_widget_set_background(column, 0);
					vgui_widget_set_border(column, 0, 0);
					vgui_widget_set_padding(column, 0, 0, 0, 0);
					columns.push_back(column);
				}
				size_t index = 0;
				for (const pending_card& pending : config.cards)
				{
					vgui_widget* column = columns[index % columns.size()];
					++index;
					if (!build_card(dashboard, pending, column, config, built,
						error))
						return false;
				}
			}
			else
			{
				/* stack and wrap place every card directly in the root. */
				for (const pending_card& pending : config.cards)
				{
					if (!build_card(dashboard, pending, built.root, config,
						built, error))
						return false;
				}
			}
			return true;
		}

		void clear_dashboard(vgui_dashboard* dashboard)
		{
			if (!dashboard) return;
			for (vgui_chart* chart : dashboard->charts)
				vgui_chart_destroy(chart);
			dashboard->charts.clear();
			for (vgui_gauge* gauge : dashboard->gauges)
				vgui_gauge_destroy(gauge);
			dashboard->gauges.clear();
			if (dashboard->root)
			{
				vgui_widget_destroy(dashboard->root);
				dashboard->root = nullptr;
			}
			dashboard->title_label = nullptr;
			dashboard->cards.clear();
		}

		void parse_card_keys(const json& source, pending_card& card,
			const std::string& path)
		{
			card.value_key = optional_string(source, "value_key", "");
			card.delta_key = optional_string(source, "delta_key", "");
			card.text_key = optional_string(source, "text_key", "");
			card.status_key = optional_string(source, "status_key", "");
			card.format = optional_string(source, "format", "");
			card.unit = optional_string(source, "unit", "");
			if (source.contains("value"))
			{
				card.has_value = true;
				card.value = optional_number(source, "value", 0.0, path);
			}
			card.status = std::clamp(optional_int(source, "status",
				(int)VGUI_CHART_STATUS_NORMAL, path),
				(int)VGUI_CHART_STATUS_NORMAL, (int)VGUI_CHART_STATUS_STALE);
			card.status_text = optional_string(source, "status_text", "");
		}

		json point_y_json(double y)
		{
			/* JSON has no NaN/Inf; the loader accepts null, "nan", "inf" and
			 * "-inf" back. */
			if (std::isnan(y)) return json();
			if (std::isinf(y)) return y > 0.0 ? json("inf") : json("-inf");
			return json(y);
		}

		double parse_point_y(const json& value, const std::string& path)
		{
			const double nan_value = std::numeric_limits<double>::quiet_NaN();
			if (value.is_number())
				return value.get<double>();
			if (value.is_null())
				return nan_value;
			if (value.is_string())
			{
				const std::string text = value.get<std::string>();
				if (text == "nan") return nan_value;
				if (text == "inf")
					return std::numeric_limits<double>::infinity();
				if (text == "-inf")
					return -std::numeric_limits<double>::infinity();
			}
			throw config_error(path + ": expected number, null, \"nan\", \"inf\" or \"-inf\"");
		}

		void parse_data_section(const json& root, pending_dashboard& config)
		{
			auto it = root.find("data");
			if (it == root.end() || it->is_null())
				return;
			if (!it->is_object())
				throw config_error("data: expected object");

			std::map<std::string, std::unordered_set<std::string>> series_by_card;
			for (const pending_card& card : config.cards)
			{
				if (card.kind != dashboard_card_kind::chart) continue;
				for (const pending_series& series : card.series)
					series_by_card[card.id].insert(series.key);
			}

			for (auto card_it = it->begin(); card_it != it->end(); ++card_it)
			{
				const std::string& card_id = card_it.key();
				const std::string card_path = "data." + card_id;
				auto found = series_by_card.find(card_id);
				if (found == series_by_card.end())
					throw config_error(card_path + ": unknown chart card");
				if (!card_it->is_object())
					throw config_error(card_path + ": expected object");

				pending_card* card = nullptr;
				for (pending_card& candidate : config.cards)
				{
					if (candidate.id == card_id)
					{
						card = &candidate;
						break;
					}
				}
				for (auto series_it = card_it->begin();
					series_it != card_it->end(); ++series_it)
				{
					const std::string series_path =
						card_path + "." + series_it.key();
					if (!found->second.count(series_it.key()))
						throw config_error(series_path + ": unknown series");
					if (!series_it->is_array())
						throw config_error(series_path + ": expected array");

					std::vector<vgui_chart_point> points;
					points.reserve(series_it->size());
					for (size_t i = 0; i < series_it->size(); ++i)
					{
						const json& pair = (*series_it)[i];
						const std::string item_path = series_path
							+ "[" + std::to_string(i) + "]";
						if (!pair.is_array() || pair.size() != 2)
							throw config_error(item_path + ": expected [x, y] pair");
						if (!pair[0].is_number())
							throw config_error(item_path + ".x: expected number");
						const double x = pair[0].get<double>();
						if (!std::isfinite(x))
							throw config_error(item_path + ".x: must be finite");
						const double y = parse_point_y(pair[1], item_path + ".y");
						if (!points.empty() && x < points.back().x)
							throw config_error(item_path
								+ ".x: must be non-decreasing");
						points.push_back({x, y});
					}
					card->data[series_it.key()] = std::move(points);
				}
			}
		}

		void parse_runtime_section(const json& root, pending_dashboard& config)
		{
			auto it = root.find("runtime");
			if (it == root.end() || it->is_null())
				return;
			if (!it->is_object())
				throw config_error("runtime: expected object");
			for (auto entry = it->begin(); entry != it->end(); ++entry)
			{
				const json& value = entry.value();
				const bool ok = value.is_number() || value.is_string()
					|| (value.is_array() && value.size() == 2
						&& value[0].is_number_integer() && value[1].is_string());
				if (!ok)
					throw config_error("runtime." + entry.key()
						+ ": expected number, string or [status, text]");
				config.runtime[entry.key()] = value;
			}
		}

		void parse_gauge(const json& item, pending_card& card,
			const std::string& path)
		{
			json empty = json::object();
			const json* source = &empty;
			std::string base = path;
			auto it = item.find("gauge");
			if (it != item.end())
			{
				if (!it->is_object())
					throw config_error(path + ".gauge: expected object");
				source = &(*it);
				base = path + ".gauge";
			}
			card.gauge_min = optional_number(*source, "min", 0.0, base);
			card.gauge_max = optional_number(*source, "max", 100.0, base);
			if (!(card.gauge_max > card.gauge_min))
				throw config_error(base + ": max must be greater than min");
			card.gauge_start_angle = optional_number(*source, "start_angle",
				135.0, base);
			card.gauge_sweep = std::clamp(optional_number(*source, "sweep",
				270.0, base), 10.0, 360.0);
			card.gauge_show_ticks = optional_bool(*source, "show_ticks", true, base);
			card.gauge_show_value = optional_bool(*source, "show_value", true, base);
			card.gauge_label = optional_string(*source, "label", "");
			card.gauge_unit = optional_string(*source, "unit", "");
			card.gauge_format = optional_string(*source, "format", "");
			if (source->contains("zones"))
			{
				const json& zones = require_array(*source, "zones", base);
				const std::string zones_path = base + ".zones";
				for (size_t i = 0; i < zones.size(); ++i)
				{
					const json& zone = zones[i];
					const std::string zone_path = path_index(zones_path, i);
					if (!zone.is_object())
						throw config_error(zone_path + ": expected object");
					pending_zone parsed;
					parsed.from = optional_number(zone, "from", 0.0, zone_path);
					parsed.to = optional_number(zone, "to", 0.0, zone_path);
					if (zone.contains("color") && !zone["color"].is_null())
						parsed.color = parse_color(zone["color"], zone_path + ".color");
					parsed.label = optional_string(zone, "label", "");
					card.zones.push_back(std::move(parsed));
				}
			}
			if (card.value_key.empty() && source->contains("value_key"))
				card.value_key = optional_string(*source, "value_key", "");
		}

		pending_dashboard parse_config(const json& root)
		{
			if (!root.is_object())
				throw config_error("root: expected object");
			if (!root.contains("version"))
				throw config_error("version: required");
			if (!root["version"].is_number_integer()
				|| root["version"].get<int>() != 1)
				throw config_error("version: only schema version 1 is supported");

			pending_dashboard config;
			config.title = optional_string(root, "title", "");
			if (root.contains("layout") && !root["layout"].is_null())
			{
				const json& layout = require_object(root, "layout", "");
				config.padding = (float)std::clamp(
					optional_number(layout, "padding", 12.0, "layout"),
					0.0, 128.0);
				config.gap = (float)std::clamp(
					optional_number(layout, "gap", 10.0, "layout"), 0.0, 128.0);
				const std::string mode = optional_string(layout, "mode", "stack");
				if (mode != "stack" && mode != "wrap" && mode != "grid"
					&& mode != "columns")
					throw config_error(
						"layout.mode: unknown layout mode '" + mode + "'");
				config.mode = mode;
				config.columns = std::clamp(
					optional_int(layout, "columns", 2, "layout"), 1, 12);
			}

			const json& cards = require_array(root, "cards", "");
			std::unordered_set<std::string> ids;
			for (size_t i = 0; i < cards.size(); ++i)
			{
				const json& item = cards[i];
				const std::string base = path_index("cards", i);
				if (!item.is_object())
					throw config_error(base + ": expected object");
				pending_card card;
				const std::string type = require_string(item, "type", base);
				if (type == "kpi") card.kind = dashboard_card_kind::kpi;
				else if (type == "chart") card.kind = dashboard_card_kind::chart;
				else if (type == "gauge") card.kind = dashboard_card_kind::gauge;
				else throw config_error(base + ".type: unknown card type '" + type + "'");
				card.id = require_string(item, "id", base);
				if (card.id.empty())
					throw config_error(base + ".id: must not be empty");
				if (!ids.insert(card.id).second)
					throw config_error(base + ".id: duplicate card id");
				card.title = optional_string(item, "title", "");
				card.span = std::clamp(optional_int(item, "span", 1, base),
					1, config.columns);
				parse_card_keys(item, card, base);

				if (card.kind == dashboard_card_kind::chart)
				{
					vgui_chart_options_default(&card.options);
					card.x_axis.tick_count = 5;
					card.x_axis.grid = 1;
					card.y_axis.tick_count = 5;
					card.y_axis.grid = 1;
					parse_options(item, card, base);
					if (item.contains("axes") && !item["axes"].is_null())
					{
						const json& axes = require_object(item, "axes", base);
						card.x_axis = parse_axis(axes, "x", base + ".axes", card.x_axis);
						card.y_axis = parse_axis(axes, "y", base + ".axes", card.y_axis);
						card.y_axis.scale = VGUI_CHART_SCALE_LINEAR;
					}
					parse_series(item, card, base);
					parse_view(item, card, base);
				}
				else if (card.kind == dashboard_card_kind::gauge)
				{
					parse_gauge(item, card, base);
				}
				config.cards.push_back(std::move(card));
			}
			parse_data_section(root, config);
			parse_runtime_section(root, config);
			return config;
		}

		json axis_json(const axis_config& axis)
		{
			json out = json::object();
			out["scale"] = axis.scale == VGUI_CHART_SCALE_DATETIME ? "datetime"
				: axis.scale == VGUI_CHART_SCALE_TIME ? "time" : "linear";
			if (axis.scale == VGUI_CHART_SCALE_DATETIME && axis.time_zone)
				out["timezone"] = "local";
			if (axis.min < axis.max)
			{
				out["min"] = axis.min;
				out["max"] = axis.max;
			}
			out["ticks"] = axis.tick_count;
			out["include_zero"] = axis.include_zero != 0;
			out["grid"] = axis.grid != 0;
			if (!axis.label.empty()) out["label"] = axis.label;
			if (!axis.unit.empty()) out["unit"] = axis.unit;
			if (!axis.pattern.empty()) out["format"] = axis.pattern;
			return out;
		}

		json options_json(const vgui_chart_options& options)
		{
			json out = json::object();
			out["legend"] = options.legend != 0;
			out["tooltip"] = options.tooltip != 0;
			out["crosshair"] = options.crosshair != 0;
			out["follow"] = options.follow != 0;
			if (options.window > 0.0) out["window"] = options.window;
			out["decimation"] = options.decimation == VGUI_CHART_DECIMATION_NONE
				? "none" : "min_max";
			return out;
		}

		json chart_json(const dashboard_card& card)
		{
			json out = json::object();
			out["options"] = options_json(card.chart->options);
			json axes = json::object();
			axes["x"] = axis_json(card.chart->x_axis);
			axes["y"] = axis_json(card.chart->y_axis);
			out["axes"] = std::move(axes);

			json series = json::array();
			for (const auto& item : card.chart->series)
			{
				if (!item) continue;
				json s = json::object();
				s["key"] = item->key;
				if (item->name != item->key) s["name"] = item->name;
				s["kind"] = item->kind == VGUI_CHART_BAR ? "bar"
					: item->kind == VGUI_CHART_AREA ? "area" : "line";
				if (item->color) s["color"] = color_text(item->color);
				s["line_width"] = item->line_width;
				s["fill_alpha"] = item->fill_alpha;
				s["visible"] = item->visible;
				s["stacked"] = item->stacked;
				if (!item->stack_group.empty()) s["stack_group"] = item->stack_group;
				if (!item->unit.empty()) s["unit"] = item->unit;
				series.push_back(std::move(s));
			}
			out["series"] = std::move(series);

			json view = json::object();
			if (card.chart->manual_view)
			{
				view["x0"] = card.chart->view_x0;
				view["x1"] = card.chart->view_x1;
			}
			else
			{
				view["follow"] = card.chart->follow;
			}
			out["view"] = std::move(view);
			return out;
		}

		json gauge_json(const dashboard_card& card)
		{
			json out = json::object();
			const vgui_gauge* gauge = card.gauge;
			if (!gauge) return out;
			out["min"] = gauge->min;
			out["max"] = gauge->max;
			out["start_angle"] = gauge->start_angle;
			out["sweep"] = gauge->sweep;
			out["show_ticks"] = gauge->show_ticks;
			out["show_value"] = gauge->show_value;
			if (!gauge->label.empty()) out["label"] = gauge->label;
			if (!gauge->unit.empty()) out["unit"] = gauge->unit;
			if (!gauge->pattern.empty()) out["format"] = gauge->pattern;
			if (!gauge->zones.empty())
			{
				json zones = json::array();
				for (const gauge_zone_data& zone : gauge->zones)
				{
					json z = json::object();
					z["from"] = zone.from;
					z["to"] = zone.to;
					if (zone.color) z["color"] = color_text(zone.color);
					if (!zone.label.empty()) z["label"] = zone.label;
					zones.push_back(std::move(z));
				}
				out["zones"] = std::move(zones);
			}
			return out;
		}

		json data_json(const vgui_dashboard* dashboard)
		{
			json data = json::object();
			for (const dashboard_card& card : dashboard->cards)
			{
				if (card.kind != dashboard_card_kind::chart || !card.chart)
					continue;
				json series_obj = json::object();
				for (const auto& item : card.chart->series)
				{
					if (!item || item->empty()) continue;
					json points = json::array();
					points.get_ref<json::array_t&>().reserve(item->size());
					for (size_t i = 0; i < item->size(); ++i)
					{
						const vgui_chart_point& point = item->at(i);
						json pair = json::array();
						pair.push_back(point.x);
						pair.push_back(point_y_json(point.y));
						points.push_back(std::move(pair));
					}
					series_obj[item->key] = std::move(points);
				}
				if (!series_obj.empty())
					data[card.id] = std::move(series_obj);
			}
			return data;
		}

		json runtime_json(const vgui_dashboard* dashboard)
		{
			json runtime = json::object();
			for (const dashboard_card& card : dashboard->cards)
			{
				if (card.kind == dashboard_card_kind::kpi)
				{
					if (card.has_value && !card.value_key.empty())
						runtime[card.value_key] = card.last_value;
					if (!card.delta_key.empty() && !card.delta_text.empty())
						runtime[card.delta_key] = card.delta_text;
					if (!card.text_key.empty() && !card.subtitle_text.empty())
						runtime[card.text_key] = card.subtitle_text;
					if (!card.status_key.empty()
						&& card.status != VGUI_CHART_STATUS_NORMAL)
					{
						json status = json::array();
						status.push_back(card.status);
						status.push_back(card.status_text);
						runtime[card.status_key] = std::move(status);
					}
				}
				else if (card.kind == dashboard_card_kind::gauge
					&& card.gauge && card.gauge->has_value
					&& !card.value_key.empty())
				{
					runtime[card.value_key] = card.gauge->value;
				}
			}
			return runtime;
		}

		json dashboard_json(const vgui_dashboard* dashboard, uint32_t flags)
		{
			json root = json::object();
			root["version"] = 1;
			if (!dashboard->title.empty()) root["title"] = dashboard->title;
			json layout = json::object();
			layout["mode"] = dashboard->layout_mode.empty()
				? "stack" : dashboard->layout_mode;
			if (layout["mode"] == "grid" || layout["mode"] == "columns")
				layout["columns"] = dashboard->layout_columns;
			layout["gap"] = dashboard->layout_gap;
			layout["padding"] = dashboard->layout_padding;
			root["layout"] = std::move(layout);

			json cards = json::array();
			for (const dashboard_card& card : dashboard->cards)
			{
				json item = json::object();
				switch (card.kind)
				{
				case dashboard_card_kind::kpi: item["type"] = "kpi"; break;
				case dashboard_card_kind::chart: item["type"] = "chart"; break;
				case dashboard_card_kind::gauge: item["type"] = "gauge"; break;
				}
				item["id"] = card.id;
				if (!card.title.empty()) item["title"] = card.title;
				if (dashboard->layout_mode == "grid" && card.span > 1)
					item["span"] = card.span;
				if (!card.value_key.empty()) item["value_key"] = card.value_key;
				if (!card.delta_key.empty()) item["delta_key"] = card.delta_key;
				if (!card.text_key.empty()) item["text_key"] = card.text_key;
				if (!card.status_key.empty()) item["status_key"] = card.status_key;
				if (!card.format.empty() && card.kind == dashboard_card_kind::kpi)
					item["format"] = card.format;
				if (!card.unit.empty() && card.kind == dashboard_card_kind::kpi)
					item["unit"] = card.unit;
				if (card.kind == dashboard_card_kind::chart && card.chart)
				{
					json chart = chart_json(card);
					for (auto it = chart.begin(); it != chart.end(); ++it)
						item[it.key()] = it.value();
				}
				else if (card.kind == dashboard_card_kind::gauge && card.gauge)
				{
					item["gauge"] = gauge_json(card);
				}
				cards.push_back(std::move(item));
			}
			root["cards"] = std::move(cards);
			if ((flags & (uint32_t)vgui_chart_save_flags::VGUI_CHART_SAVE_DATA) != 0)
			{
				json data = data_json(dashboard);
				if (!data.empty()) root["data"] = std::move(data);
			}
			if ((flags & (uint32_t)vgui_chart_save_flags::VGUI_CHART_SAVE_RUNTIME) != 0)
			{
				json runtime = runtime_json(dashboard);
				if (!runtime.empty()) root["runtime"] = std::move(runtime);
			}
			return root;
		}

		dashboard_card* find_card(vgui_dashboard* dashboard, const char* id)
		{
			if (!dashboard || !id || !*id) return nullptr;
			for (dashboard_card& card : dashboard->cards)
			{
				if (card.id == id)
					return &card;
			}
			return nullptr;
		}

		bool write_text(vgui_chart_write_fn write, void* user,
			const std::string& text)
		{
			if (!write) return false;
			size_t offset = 0;
			while (offset < text.size())
			{
				const size_t chunk = std::min<size_t>(64 * 1024, text.size() - offset);
				if (!write(user, text.data() + offset, chunk))
					return false;
				offset += chunk;
			}
			return true;
		}

		void refresh_kpi(dashboard_card& card)
		{
			if (!card.value_label) return;
			const std::string text = card.has_value
				? format_kpi(card, card.last_value) : "--";
			vgui_widget_set_text(card.value_label, text.c_str());
		}

	void set_dashboard_error(vgui_dashboard* dashboard, const char* text)
	{
		if (dashboard)
			dashboard->last_error = text ? text : "";
	}

} /* namespace vgchart */

extern "C" {

	vgui_dashboard* vgui_dashboard_create(vgui_widget* parent)
	{
		if (!parent) return nullptr;
		vgui_dashboard* dashboard = new (std::nothrow) vgui_dashboard();
		if (!dashboard) return nullptr;
		dashboard->ctx = vgui_widget_get_context(parent);
		dashboard->parent = parent;
		dashboard->root = vgui_widget_add(parent, VGUI_PANEL, nullptr);
		if (!dashboard->root)
		{
			delete dashboard;
			return nullptr;
		}
		vgui_widget_set_id(dashboard->root, "vgui_dashboard");
		vgui_widget_set_layout(dashboard->root, VGUI_LAYOUT_COLUMN);
		vgui_widget_set_padding(dashboard->root, 12, 12, 12, 12);
		vgui_widget_set_gap(dashboard->root, 10);
		vgui_widget_set_align_items(dashboard->root, VGUI_ALIGN_STRETCH);
		vgui_widget_set_background(dashboard->root, 0);
		vgui_widget_set_border(dashboard->root, 0, 0);
		vgui_widget_set_grow(dashboard->root, 1.0f);
		vgui_widget_set_min_size(dashboard->root, 200, 120);
		return dashboard;
	}

	void vgui_dashboard_destroy(vgui_dashboard* dashboard)
	{
		if (!dashboard) return;
		vgchart::clear_dashboard(dashboard);
		delete dashboard;
	}

	vgui_widget* vgui_dashboard_widget(vgui_dashboard* dashboard)
	{
		return dashboard ? dashboard->root : nullptr;
	}

	int vgui_dashboard_load_json(vgui_dashboard* dashboard, const char* text,
		size_t len, char* error, size_t error_cap)
	{
		if (error && error_cap) error[0] = '\0';
		if (!dashboard || !text)
		{
			vgchart::set_dashboard_error(dashboard, "invalid dashboard JSON");
			return 0;
		}

		std::string failure;
		try
		{
			nlohmann::json root = nlohmann::json::parse(text, text + len);
			vgchart::pending_dashboard config = vgchart::parse_config(root);

			vgchart::build_result built;
			if (!vgchart::build_dashboard(dashboard, config, built, failure))
			{
				vgchart::destroy_build(built);
				if (failure.empty())
					failure = "cannot build dashboard";
				vgchart::set_dashboard_error(dashboard, failure.c_str());
				if (error && error_cap)
				{
					std::snprintf(error, error_cap, "%s", failure.c_str());
				}
				return 0;
			}

			/* Snapshot data: validated while parsing, so these appends can
			 * only fail on allocation problems.  They run before the swap so
			 * a failure still leaves the previous dashboard untouched. */
			for (size_t i = 0;
				i < config.cards.size() && i < built.cards.size(); ++i)
			{
				const vgchart::pending_card& pending = config.cards[i];
				vgchart::dashboard_card& card = built.cards[i];
				if (pending.data.empty() || !card.chart)
					continue;
				for (const auto& entry : pending.data)
				{
					const vgui_series_id id = vgui_chart_find_series(
						card.chart, entry.first.c_str());
					if (!id || (entry.second.size()
						&& !vgui_chart_append_points(card.chart, id,
							entry.second.data(), entry.second.size())))
					{
						failure = "cannot restore data for card '"
							+ pending.id + "'";
						vgchart::destroy_build(built);
						vgchart::set_dashboard_error(dashboard, failure.c_str());
						if (error && error_cap)
							std::snprintf(error, error_cap, "%s",
								failure.c_str());
						return 0;
					}
				}
			}

			/* Loading replaces the root panel.  Preserve the geometry the
			 * caller assigned to the previous root so live dashboard reloads
			 * do not shrink back to their preferred size. */
			float old_x = 0.0f;
			float old_y = 0.0f;
			float old_w = 0.0f;
			float old_h = 0.0f;
			const bool had_root = dashboard->root != nullptr;
			if (had_root)
			{
				vgui_widget_get_rect(dashboard->root, &old_x, &old_y,
					&old_w, &old_h);
				if (dashboard->parent)
				{
					float parent_x = 0.0f;
					float parent_y = 0.0f;
					float parent_w = 0.0f;
					float parent_h = 0.0f;
					vgui_widget_get_rect(dashboard->parent, &parent_x,
						&parent_y, &parent_w, &parent_h);
					old_x -= parent_x;
					old_y -= parent_y;
				}
				float scale = vgui_context_get_ui_scale(dashboard->ctx);
				if (!(scale > 0.0f)) scale = 1.0f;
				vgui_widget_set_pos(built.root, old_x / scale,
					old_y / scale);
				vgui_widget_set_size(built.root, old_w / scale,
					old_h / scale);
			}

			vgchart::clear_dashboard(dashboard);
			dashboard->root = built.root;
			dashboard->title_label = built.title_label;
			dashboard->cards = std::move(built.cards);
			dashboard->charts = std::move(built.charts);
			dashboard->gauges = std::move(built.gauges);
			dashboard->title = config.title;
			dashboard->layout_mode = config.mode;
			dashboard->layout_columns = config.columns;
			dashboard->layout_gap = config.gap;
			dashboard->layout_padding = config.padding;
			dashboard->config_json.assign(text, len);

			/* Runtime values go through the public setters after the swap;
			 * unmatched keys only leave a note in last_error. */
			for (const auto& entry : config.runtime)
			{
				const nlohmann::json& value = entry.second;
				if (value.is_number())
				{
					vgui_dashboard_set_number(dashboard, entry.first.c_str(),
						value.get<double>());
				}
				else if (value.is_string())
				{
					vgui_dashboard_set_text(dashboard, entry.first.c_str(),
						value.get<std::string>().c_str());
				}
				else
				{
					vgui_dashboard_set_status(dashboard, entry.first.c_str(),
						value[0].get<int>(), value[1].get<std::string>().c_str());
				}
			}
			dashboard->last_error.clear();
			vgui_layout(dashboard->ctx);
			return 1;
		}
		catch (const vgchart::config_error& e)
		{
			failure = e.what();
		}
		catch (const nlohmann::json::exception& e)
		{
			failure = std::string("invalid JSON: ") + e.what();
		}
		catch (const std::bad_alloc&)
		{
			failure = "out of memory";
		}
		catch (const std::exception& e)
		{
			failure = e.what();
		}

		vgchart::set_dashboard_error(dashboard, failure.c_str());
		if (error && error_cap)
			std::snprintf(error, error_cap, "%s", failure.c_str());
		return 0;
	}

	int vgui_dashboard_save_json(vgui_dashboard* dashboard,
		vgui_chart_write_fn write, void* user)
	{
		return vgui_dashboard_save_json_ex(dashboard,
			(uint32_t)vgui_chart_save_flags::VGUI_CHART_SAVE_DEFAULT,
			write, user);
	}

	int vgui_dashboard_save_json_ex(vgui_dashboard* dashboard, uint32_t flags,
		vgui_chart_write_fn write, void* user)
	{
		if (!dashboard || !write)
			return 0;
		try
		{
			const std::string text = vgchart::dashboard_json(dashboard,
				flags).dump(2);
			if (!vgchart::write_text(write, user, text))
			{
				vgchart::set_dashboard_error(dashboard, "JSON write failed");
				return 0;
			}
			dashboard->config_json = text;
			dashboard->last_error.clear();
			return 1;
		}
		catch (const std::bad_alloc&)
		{
			vgchart::set_dashboard_error(dashboard, "out of memory");
			return 0;
		}
		catch (const std::exception& e)
		{
			vgchart::set_dashboard_error(dashboard, e.what());
			return 0;
		}
	}

	int vgui_dashboard_set_number(vgui_dashboard* dashboard, const char* key,
		double value)
	{
		if (!dashboard || !key || !*key)
			return 0;
		bool matched = false;
		for (vgchart::dashboard_card& card : dashboard->cards)
		{
			if (card.kind == vgchart::dashboard_card_kind::kpi
				&& card.value_key == key)
			{
				card.has_value = true;
				card.last_value = value;
				vgchart::refresh_kpi(card);
				matched = true;
			}
			else if (card.kind == vgchart::dashboard_card_kind::gauge
				&& card.value_key == key && card.gauge)
			{
				vgui_gauge_set_value(card.gauge, value);
				matched = true;
			}
		}
		if (!matched)
			vgchart::set_dashboard_error(dashboard, "numeric key not found");
		return matched ? 1 : 0;
	}

	int vgui_dashboard_set_text(vgui_dashboard* dashboard, const char* key,
		const char* text)
	{
		if (!dashboard || !key || !*key)
			return 0;
		const char* value = text ? text : "";
		bool matched = false;
		for (vgchart::dashboard_card& card : dashboard->cards)
		{
			if (!card.delta_key.empty() && card.delta_key == key)
			{
				card.delta_text = value;
				if (card.delta_label)
				{
					vgui_widget_set_text(card.delta_label, value);
					vgui_widget_set_visible(card.delta_label, *value ? 1 : 0);
				}
				matched = true;
			}
			if (!card.text_key.empty() && card.text_key == key)
			{
				card.subtitle_text = value;
				if (card.subtitle_label)
				{
					vgui_widget_set_text(card.subtitle_label, value);
					vgui_widget_set_visible(card.subtitle_label, *value ? 1 : 0);
				}
				matched = true;
			}
		}
		if (!matched)
			vgchart::set_dashboard_error(dashboard, "text key not found");
		return matched ? 1 : 0;
	}

	int vgui_dashboard_set_status(vgui_dashboard* dashboard, const char* key,
		int status, const char* text)
	{
		if (!dashboard || !key || !*key)
			return 0;
		const int clamped = std::clamp(status,
			(int)VGUI_CHART_STATUS_NORMAL, (int)VGUI_CHART_STATUS_STALE);
		bool matched = false;
		for (vgchart::dashboard_card& card : dashboard->cards)
		{
			if (!(card.status_key == key || card.id == key))
				continue;
			card.status = clamped;
			card.status_text = text ? text : "";
			if (card.status_label)
			{
				const char* value = text ? text : "";
				vgui_widget_set_text(card.status_label, value);
				vgui_widget_set_visible(card.status_label, *value ? 1 : 0);
				vgui_chart_theme theme = {};
				vgchart::derive_theme(dashboard->ctx, &theme);
				vgui_widget_set_text_color(card.status_label,
					vgchart::status_color(theme, clamped));
			}
			matched = true;
		}
		if (!matched)
			vgchart::set_dashboard_error(dashboard, "status key not found");
		return matched ? 1 : 0;
	}

	int vgui_dashboard_append_points(vgui_dashboard* dashboard,
		const char* card_id, const char* series, const vgui_chart_point* points,
		size_t count)
	{
		if (!dashboard || !card_id || !series)
			return 0;
		vgchart::dashboard_card* card = vgchart::find_card(dashboard, card_id);
		if (!card || card->kind != vgchart::dashboard_card_kind::chart
			|| !card->chart)
		{
			vgchart::set_dashboard_error(dashboard, "chart card not found");
			return 0;
		}
		const vgui_series_id id = vgui_chart_find_series(card->chart, series);
		if (!id)
		{
			vgchart::set_dashboard_error(dashboard, "series not found");
			return 0;
		}
		if (!vgui_chart_append_points(card->chart, id, points, count))
		{
			vgchart::set_dashboard_error(dashboard,
				vgui_chart_last_error(card->chart));
			return 0;
		}
		return 1;
	}

	vgui_chart* vgui_dashboard_chart(vgui_dashboard* dashboard, const char* card_id)
	{
		vgchart::dashboard_card* card = vgchart::find_card(dashboard, card_id);
		if (!card || card->kind != vgchart::dashboard_card_kind::chart)
			return nullptr;
		return card->chart;
	}

	vgui_gauge* vgui_dashboard_gauge(vgui_dashboard* dashboard, const char* card_id)
	{
		vgchart::dashboard_card* card = vgchart::find_card(dashboard, card_id);
		if (!card || card->kind != vgchart::dashboard_card_kind::gauge)
			return nullptr;
		return card->gauge;
	}

	void vgui_dashboard_layout(vgui_dashboard* dashboard)
	{
		if (dashboard && dashboard->ctx)
			vgui_layout(dashboard->ctx);
	}

	int vgui_dashboard_export_csv(vgui_dashboard* dashboard, uint32_t flags,
		vgui_chart_write_fn write, void* user)
	{
		if (!dashboard || !write)
			return 0;
		if ((flags & VGUI_CHART_EXPORT_HEADER) != 0)
		{
			if (!write(user, "card,series,x,y\n", 16))
			{
				vgchart::set_dashboard_error(dashboard, "CSV write failed");
				return 0;
			}
		}
		const uint32_t child_flags = flags & ~(uint32_t)VGUI_CHART_EXPORT_HEADER;
		for (vgchart::dashboard_card& card : dashboard->cards)
		{
			if (card.kind != vgchart::dashboard_card_kind::chart || !card.chart)
				continue;
			if (!vgchart::chart_export_csv_impl(card.chart, card.id.c_str(),
				child_flags, write, user))
			{
				vgchart::set_dashboard_error(dashboard,
					vgui_chart_last_error(card.chart));
				return 0;
			}
		}
		return 1;
	}

	const char* vgui_dashboard_last_error(vgui_dashboard* dashboard)
	{
		return dashboard ? dashboard->last_error.c_str() : "";
	}

} /* extern "C" */
