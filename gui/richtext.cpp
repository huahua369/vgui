/*
gui rich-text primitives
Core rich-text primitives: turn UTF-8 text + byte spans into styled runs.
*/

#include "pch.h"
#include <vgui/richtext.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <ovg_c.h>
#include "tools/tools.hpp"

namespace
{
	struct text_run
	{
		std::string text;
		float width = 0.0f;
		float font_size = 0.0f;
		vgui_color color = 0;
		unsigned decor = 0;
	};

	/* vgui.h packs colours as 0xAARRGGBB; ovg consumes 0xAABBGGRR. */
	uint32_t to_ovg_color(vgui_color c)
	{
		return (c & 0xFF00FF00u) | ((c & 0x00FF0000u) >> 16) | ((c & 0x000000FFu) << 16);
	}

	bool is_utf8_continuation(unsigned char c)
	{
		return (c & 0xC0) == 0x80;
	}

	/* Keep a byte cut from landing in the middle of a multi-byte character. */
	void snap_utf8_boundaries(const char* text, int len, int& start, int& end)
	{
		while (start > 0 && is_utf8_continuation((unsigned char)text[start])) --start;
		while (end < len && is_utf8_continuation((unsigned char)text[end])) ++end;
		if (end < start) end = start;
	}

	std::vector<text_run> build_runs(vgui_widget* widget, const char* text,
		const vgui_text_span* spans, int span_count,
		vgui_color base_color, float base_font_size)
	{
		std::vector<text_run> runs;
		if (!widget || !text) return runs;
		const int len = (int)std::strlen(text);
		if (len <= 0) return runs;

		struct item
		{
			int start;
			int end;
			vgui_color color;
			float font_size;
			unsigned decor;
			int order;
		};
		std::vector<item> items;
		if (spans && span_count > 0)
		{
			items.reserve((std::size_t)span_count);
			for (int i = 0; i < span_count; ++i)
			{
				int start = spans[i].start;
				int length = spans[i].length;
				if (length <= 0) continue;
				if (start < 0) { length += start; start = 0; }
				if (length <= 0 || start >= len) continue;
				int end = start + length;
				if (end > len) end = len;
				snap_utf8_boundaries(text, len, start, end);
				if (end <= start) continue;
				items.push_back({ start, end, spans[i].color, spans[i].font_size,
					spans[i].decor, i });
			}
		}
		std::stable_sort(items.begin(), items.end(), [](const item& a, const item& b)
			{
				if (a.start != b.start) return a.start < b.start;
				return a.order < b.order;
			});

		int cursor = 0;
		auto emit = [&](int start, int end, vgui_color color, float font_size, unsigned decor)
			{
				if (end <= start) return;
				text_run run;
				run.text.assign(text + start, (std::size_t)(end - start));
				run.font_size = font_size > 0.0f ? font_size : base_font_size;
				run.color = color ? color : base_color;
				run.decor = decor;
				run.width = vgui_widget_measure_text_sized(widget, run.text.c_str(), run.font_size);
				runs.push_back(std::move(run));
			};

		for (const item& it : items)
		{
			if (it.end <= cursor) continue;  /* fully covered by an earlier span */
			if (it.start > cursor) emit(cursor, it.start, 0, 0.0f, 0);
			const int run_start = std::max(it.start, cursor);
			emit(run_start, it.end, it.color, it.font_size, it.decor);
			cursor = it.end;
		}
		if (cursor < len) emit(cursor, len, 0, 0.0f, 0);
		return runs;
	}

	void draw_decoration(ovg_ctx_cb* cb, rvg_t* vg, float x, float y,
		float width, float thickness, vgui_color color)
	{
		if (width <= 0.0f || thickness <= 0.0f || color == 0) return;
		ovg_tools::fill_round_rect(cb, vg, x, y, width, thickness, 0.0f,
			to_ovg_color(color));
	}
}

void vgui_custom_draw_text_spans(vgui_widget* widget,
	ovg_ctx_cb* cb, rvg_t* vg, const char* text,
	const vgui_text_span* spans, int span_count,
	float x, float y, float width, float height,
	vgui_color base_color, float align_x, float align_y, float base_font_size)
{
	if (!widget || !cb || !vg || !text || !*text ||
		width <= 0.0f || height <= 0.0f || base_color == 0) return;

	std::vector<text_run> runs = build_runs(widget, text, spans, span_count,
		base_color, base_font_size);
	if (runs.empty()) return;

	float box_height = 0.0f;
	float total_width = 0.0f;
	for (const text_run& run : runs)
	{
		const float line_height = vgui_widget_get_line_height_sized(widget, run.font_size);
		if (line_height > box_height) box_height = line_height;
		total_width += run.width;
	}
	if (box_height <= 0.0f) box_height = vgui_widget_get_line_height(widget);
	if (box_height <= 0.0f) return;

	const float ax = std::clamp(align_x, 0.0f, 1.0f);
	const float ay = std::clamp(align_y, 0.0f, 1.0f);
	float pen_x = x + std::max(0.0f, (width - total_width) * ax);
	const float pen_y = y + std::max(0.0f, (height - box_height) * ay);
	const float clip_right = x + width;

	for (const text_run& run : runs)
	{
		if (pen_x >= clip_right) break;
		const float line_height = vgui_widget_get_line_height_sized(widget, run.font_size);
		const float run_y = pen_y + (box_height - line_height) * 0.5f;
		const float available = clip_right - pen_x;
		vgui_custom_draw_text_sized(widget, cb, vg, run.text.c_str(),
			pen_x, run_y, available, line_height, run.color, 0.0f, 0.0f, run.font_size);

		if (run.decor && line_height > 0.0f)
		{
			const float draw_w = std::min(run.width, available);
			const float thickness = std::max(1.0f, run.font_size * 0.06f);
			if (run.decor & VGUI_TEXT_DECOR_UNDERLINE)
				draw_decoration(cb, vg, pen_x, run_y + line_height - thickness,
					draw_w, thickness, run.color);
			if (run.decor & VGUI_TEXT_DECOR_STRIKETHROUGH)
				draw_decoration(cb, vg, pen_x, run_y + line_height * 0.55f,
					draw_w, thickness, run.color);
		}
		pen_x += run.width;
	}
}

float vgui_widget_measure_text_spans(vgui_widget* widget,
	const char* text, const vgui_text_span* spans, int span_count,
	float base_font_size)
{
	std::vector<text_run> runs = build_runs(widget, text, spans, span_count,
		0xFFFFFFFFu, base_font_size);
	float total = 0.0f;
	for (const text_run& run : runs) total += run.width;
	return total;
}

float vgui_widget_get_line_height_spans(vgui_widget* widget,
	const vgui_text_span* spans, int span_count, float base_font_size)
{
	if (!widget) return 0.0f;
	float height = vgui_widget_get_line_height_sized(widget, base_font_size);
	if (spans && span_count > 0)
	{
		for (int i = 0; i < span_count; ++i)
		{
			if (spans[i].length <= 0) continue;
			const float line_height = vgui_widget_get_line_height_sized(widget, spans[i].font_size);
			if (line_height > height) height = line_height;
		}
	}
	return height;
}
