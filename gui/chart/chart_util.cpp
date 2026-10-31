#include "chart_internal.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#include <time.h>
#else
#include <ctime>
#endif

namespace vgchart {

	namespace {

		constexpr vgui_color kFallbackPanel = 0xFF1E232B;
		constexpr vgui_color kFallbackBorder = 0xFF2E3440;
		constexpr vgui_color kFallbackText = 0xFFE8EEF8;
		constexpr vgui_color kFallbackAccent = 0xFF409EFF;
		constexpr vgui_color kFallbackSuccess = 0xFF3FB950;
		constexpr vgui_color kFallbackWarning = 0xFFD29922;
		constexpr vgui_color kFallbackError = 0xFFF85149;

		bool is_light(vgui_color c)
		{
			const float r = (float)((c >> 16) & 0xFF) / 255.0f;
			const float g = (float)((c >> 8) & 0xFF) / 255.0f;
			const float b = (float)(c & 0xFF) / 255.0f;
			return (r * 0.2126f + g * 0.7152f + b * 0.0722f) > 0.58f;
		}

		bool pattern_is_safe(const char* pattern)
		{
			if (!pattern || !*pattern) return false;
			int conversions = 0;
			for (const char* p = pattern; *p; ++p)
			{
				if (*p != '%') continue;
				++p;
				if (*p == '%') continue;
				if (!*p) return false;
				while (*p && std::strchr("-+ #0'", *p)) ++p;
				while (*p >= '0' && *p <= '9') ++p;
				if (*p == '.')
				{
					++p;
					while (*p >= '0' && *p <= '9') ++p;
				}
				if (*p && std::strchr("hlLzjt", *p)) ++p;
				if (!*p || !std::strchr("fFeEgGaA", *p)) return false;
				++conversions;
			}
			return conversions == 1;
		}

		void fill_theme(const vgui_theme& source, vgui_chart_theme* out)
		{
			if (!out) return;
			const vgui_color panel = ensure_color(source.panel_bg, kFallbackPanel);
			const vgui_color border = ensure_color(source.panel_border, kFallbackBorder);
			const vgui_color text = ensure_color(source.text, kFallbackText);
			const vgui_color accent = ensure_color(source.accent, kFallbackAccent);
			const bool light = is_light(panel);
			const vgui_color edge = light ? 0xFFFFFFFFu : 0xFF000000u;

			out->struct_size = (uint32_t)sizeof(vgui_chart_theme);
			out->plot_bg = mix_color(panel, edge, light ? 0.16f : 0.28f);
			out->grid = mix_color(border, out->plot_bg, 0.32f);
			out->axis = mix_color(text, out->plot_bg, 0.55f);
			out->text = text;
			out->muted_text = mix_color(text, out->plot_bg, 0.45f);
			out->tooltip_bg = ensure_color(source.tooltip_bg,
				mix_color(panel, edge, light ? 0.05f : 0.18f));
			out->tooltip_border = ensure_color(source.tooltip_border, border);
			out->tooltip_text = ensure_color(source.tooltip_text, text);
			out->crosshair = mix_color(accent, text, 0.25f);
			out->selection = with_alpha(accent, 0x66);

			out->series[0] = accent;
			out->series[1] = ensure_color(source.toast_success, kFallbackSuccess);
			out->series[2] = ensure_color(source.toast_warning, kFallbackWarning);
			out->series[3] = ensure_color(source.toast_error, kFallbackError);
			out->series[4] = 0xFFA371F7u;
			out->series[5] = 0xFF39C5CFu;
			out->series[6] = 0xFFFF7B72u;
			out->series[7] = 0xFFE3B341u;
			out->series[8] = 0xFF7EE787u;
			out->series[9] = 0xFF79C0FFu;
			if (light)
			{
				/* Slightly darken the palette so it stays readable on white. */
				for (int i = 0; i < 10; ++i)
					out->series[i] = mix_color(out->series[i], 0xFF101418u, 0.12f);
			}

			out->positive = ensure_color(source.toast_success, kFallbackSuccess);
			out->negative = ensure_color(source.toast_error, kFallbackError);
			out->warning = ensure_color(source.toast_warning, kFallbackWarning);
			out->critical = ensure_color(source.toast_error, kFallbackError);
		}

	} /* namespace */

	void derive_theme(vgui_context* ctx, vgui_chart_theme* out)
	{
		vgui_theme theme = {};
		if (ctx)
			vgui_context_get_theme(ctx, &theme);
		fill_theme(theme, out);
	}

	void fill_theme_from(const vgui_theme* theme, vgui_chart_theme* out)
	{
		vgui_theme fallback = {};
		fill_theme(theme ? *theme : fallback, out);
	}

	bool format_pattern_safe(const char* pattern)
	{
		return pattern_is_safe(pattern);
	}

	bool try_format_pattern(const std::string& pattern, double value,
		std::string& out)
	{
		if (pattern.empty() || !pattern_is_safe(pattern.c_str()))
			return false;
		char buffer[128] = {};
		std::snprintf(buffer, sizeof(buffer), pattern.c_str(), value);
		out = buffer;
		return true;
	}

	void axis_from_desc(axis_config& out, const vgui_chart_axis_desc* desc)
	{
		if (!desc)
		{
			out = axis_config{};
			return;
		}

		vgui_chart_axis_desc d = {};
		size_t bytes = sizeof(d);
		if (desc->struct_size && desc->struct_size < bytes)
			bytes = desc->struct_size;
		std::memcpy(&d, desc, bytes);

		out.scale = (d.scale == VGUI_CHART_SCALE_TIME
			|| d.scale == VGUI_CHART_SCALE_DATETIME)
			? d.scale : VGUI_CHART_SCALE_LINEAR;
		out.min = d.min;
		out.max = d.max;
		out.tick_count = d.tick_count > 0 ? d.tick_count : 5;
		if (out.tick_count > 32) out.tick_count = 32;
		out.include_zero = d.include_zero ? 1 : 0;
		out.grid = d.grid ? 1 : 0;
		out.label = d.label ? d.label : "";
		out.unit = d.unit ? d.unit : "";
		out.pattern = d.format_pattern ? d.format_pattern : "";
		out.format = d.format;
		out.format_user = d.format_user;
		/* time_zone sits at the tail of the struct; hosts compiled against the
		 * P0 layout pass a smaller struct_size and keep the default UTC. */
		out.time_zone = d.time_zone ? 1 : 0;
	}

	void format_value(const axis_config& axis, double value, std::string& out)
	{
		if (!std::isfinite(value))
		{
			out = "--";
			return;
		}
		if (axis.scale == VGUI_CHART_SCALE_TIME)
		{
			format_duration(value, out);
			return;
		}
		if (axis.scale == VGUI_CHART_SCALE_DATETIME)
		{
			format_datetime(value, axis.time_zone, true, out);
			return;
		}

		char buffer[128] = {};
		if (axis.format)
		{
			if (axis.format(axis.format_user, value, buffer, sizeof(buffer))
				&& buffer[0])
			{
				out = buffer;
				return;
			}
		}
		if (try_format_pattern(axis.pattern, value, out))
			return;
		std::snprintf(buffer, sizeof(buffer), "%.6g", value);
		out = buffer;
	}

	void format_duration(double seconds, std::string& out)
	{
		char buffer[64] = {};
		if (!std::isfinite(seconds))
		{
			out = "--";
			return;
		}
		const double sign = seconds < 0.0 ? -1.0 : 1.0;
		double s = std::fabs(seconds);
		if (s < 1.0)
		{
			std::snprintf(buffer, sizeof(buffer), "%s%.3gs", sign < 0 ? "-" : "", s);
		}
		else if (s < 60.0)
		{
			const double rounded = std::round(s * 10.0) / 10.0;
			std::snprintf(buffer, sizeof(buffer),
				std::fabs(rounded - std::round(rounded)) < 1e-9 ? "%s%.0fs" : "%s%.1fs",
				sign < 0 ? "-" : "", rounded);
		}
		else if (s < 3600.0)
		{
			const int total = (int)std::round(s);
			const int m = total / 60;
			const int r = total % 60;
			if (r == 0)
				std::snprintf(buffer, sizeof(buffer), "%s%dm", sign < 0 ? "-" : "", m);
			else
				std::snprintf(buffer, sizeof(buffer), "%s%dm%02ds",
					sign < 0 ? "-" : "", m, r);
		}
		else if (s < 86400.0)
		{
			const int total = (int)std::round(s);
			const int hr = total / 3600;
			const int m = (total % 3600) / 60;
			if (m == 0)
				std::snprintf(buffer, sizeof(buffer), "%s%dh", sign < 0 ? "-" : "", hr);
			else
				std::snprintf(buffer, sizeof(buffer), "%s%dh%02dm",
					sign < 0 ? "-" : "", hr, m);
		}
		else
		{
			const int total = (int)std::round(s / 86400.0);
			std::snprintf(buffer, sizeof(buffer), "%s%dd", sign < 0 ? "-" : "", total);
		}
		out = buffer;
	}

	/* ---------------------------------------------------------- civil time */
	/* Days since 1970-01-01 <-> proleptic Gregorian civil date
	 * (Howard Hinnant's algorithms; no OS dependency, handles year < 1970). */

	long long days_from_civil(long long y, unsigned m, unsigned d)
	{
		y -= m <= 2;
		const long long era = (y >= 0 ? y : y - 399) / 400;
		const unsigned yoe = (unsigned)(y - era * 400);
		const unsigned doy = (153u * (m + (m > 2 ? -3u : 9u)) + 2u) / 5u + d - 1u;
		const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
		return era * 146097LL + (long long)doe - 719468LL;
	}

	void civil_from_days(long long z, long long& y, unsigned& m, unsigned& d)
	{
		z += 719468;
		const long long era = (z >= 0 ? z : z - 146096) / 146097;
		const unsigned doe = (unsigned)(z - era * 146097);
		const unsigned yoe = (doe - doe / 1460u + doe / 36524u
			- doe / 146096u) / 365u;
		y = (long long)yoe + era * 400;
		const unsigned doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
		const unsigned mp = (5u * doy + 2u) / 153u;
		d = doy - (153u * mp + 2u) / 5u + 1u;
		m = mp < 10u ? mp + 3u : mp - 9u;
		y += (m <= 2);
	}

	double local_utc_offset(double epoch)
	{
		const time_t tt = (time_t)std::floor(epoch);
		std::tm tm_value{};
#ifdef _WIN32
		if (localtime_s(&tm_value, &tt) != 0)
			return 0.0;
#else
		if (!localtime_r(&tt, &tm_value))
			return 0.0;
#endif
		const long long days = days_from_civil(
			tm_value.tm_year + 1900, (unsigned)(tm_value.tm_mon + 1),
			(unsigned)tm_value.tm_mday);
		const double as_utc = (double)days * 86400.0
			+ (double)tm_value.tm_hour * 3600.0
			+ (double)tm_value.tm_min * 60.0
			+ (double)tm_value.tm_sec;
		return as_utc - std::floor(epoch);
	}

	void format_datetime(double value, int time_zone, bool full, std::string& out)
	{
		if (!std::isfinite(value))
		{
			out = "--";
			return;
		}
		const double shifted = value
			+ (time_zone ? local_utc_offset(value) : 0.0);
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
		unsigned m = 0;
		unsigned d = 0;
		civil_from_days(days, y, m, d);
		const int hh = sec_of_day / 3600;
		const int mm = (sec_of_day % 3600) / 60;
		const int ss = sec_of_day % 60;
		char buffer[64] = {};
		if (full)
			std::snprintf(buffer, sizeof(buffer),
				"%04lld-%02u-%02u %02d:%02d:%02d", y, m, d, hh, mm, ss);
		else
			std::snprintf(buffer, sizeof(buffer), "%02u-%02u %02d:%02d",
				m, d, hh, mm);
		out = buffer;
	}

	vgui_color palette_color(const chart_impl* chart, size_t index)
	{
		if (!chart) return 0xFFFFFFFFu;
		if (!chart->palette.empty())
			return chart->palette[index % chart->palette.size()];
		const vgui_color c = chart->theme.series[index % 10];
		return c ? c : 0xFFFFFFFFu;
	}

	std::string sanitize_utf8(const char* text)
	{
		return text ? std::string(text) : std::string();
	}

	void set_chart_error(chart_impl* chart, const char* text)
	{
		if (chart)
			chart->last_error = text ? text : "";
	}

	void set_gauge_error(gauge_impl* gauge, const char* text)
	{
		if (gauge)
			gauge->last_error = text ? text : "";
	}

} /* namespace vgchart */

extern "C" {

	void vgui_chart_theme_from_theme(const vgui_theme* theme, vgui_chart_theme* out)
	{
		vgchart::fill_theme_from(theme, out);
	}

	void vgui_chart_theme_high_contrast(vgui_chart_theme* out)
	{
		if (!out) return;
		out->struct_size = (uint32_t)sizeof(vgui_chart_theme);
		out->plot_bg = 0xFF000000u;
		out->grid = 0xFF3A3A3Au;
		out->axis = 0xFFD0D0D0u;
		out->text = 0xFFFFFFFFu;
		out->muted_text = 0xFFB8B8B8u;
		out->tooltip_bg = 0xFF101010u;
		out->tooltip_border = 0xFFFFFFFFu;
		out->tooltip_text = 0xFFFFFFFFu;
		out->crosshair = 0xFFFFD60Au;
		out->selection = 0x66FFD60Au;
		out->series[0] = 0xFFFFD60Au;
		out->series[1] = 0xFF64D2FFu;
		out->series[2] = 0xFF30D158u;
		out->series[3] = 0xFFFF9F0Au;
		out->series[4] = 0xFFBF5AF2u;
		out->series[5] = 0xFFFF453Au;
		out->series[6] = 0xFF5E5CE6u;
		out->series[7] = 0xFF66D4CFu;
		out->series[8] = 0xFFFFB340u;
		out->series[9] = 0xFFFFFFFFu;
		out->positive = 0xFF30D158u;
		out->negative = 0xFFFF453Au;
		out->warning = 0xFFFFD60Au;
		out->critical = 0xFFFF453Au;
	}

} /* extern "C" */
