#pragma once
/*
 * P09 打磨:弹出编辑器(curve/gradient/color picker)与 spinbox 的 chrome
 * 尺寸不再写死像素,而是由上下文字体派生,保证与 kit 其余部分一致,
 * 且宿主修改基础字号或 ui_scale 时自动跟随。
 *
 * 约定同 E01:本文件所有返回值都是逻辑单位,可直接传给 set_size /
 * set_font_size 等 setter;文本测量得到的有效像素先经 to_logical 换算。
 */
#include "ui_scale.h"

#include <vgui/vgui.h>

#include <algorithm>
#include <cstring>

namespace vgui_editor_kit {

	/* Context font size in logical units (effective size / ui_scale).
	 * Falls back to the 16 default when no font is configured. */
	inline float editor_font_size(vgui_context* ctx)
	{
		const float scale = ui_scale_of(ctx);
		const float effective = ctx ? vgui_context_get_font_size(ctx) : 16.0f;
		const float logical = effective / scale;
		return logical > 0.0f ? logical : 16.0f;
	}

	/* Row height that fits one line of text with breathing room.
	 * Keeps the historical 24 baseline for small fonts. */
	inline float editor_row_height(vgui_context* ctx)
	{
		return std::max(24.0f, editor_font_size(ctx) + 10.0f);
	}

	/* Smaller square chrome (color stops, swatches, hue bars): never thinner
	 * than the row minus padding, so hit targets stay usable. */
	inline float editor_row_inner(vgui_context* ctx)
	{
		return std::max(16.0f, editor_row_height(ctx) - 8.0f);
	}

	/* Minimum logical width for a text button: measured text width plus
	 * horizontal padding.  Measure with the built button so its font applies.
	 * Without a font (headless tests) fall back to a per-character estimate so
	 * buttons never collapse to bare padding. */
	inline float editor_button_min_w(vgui_widget* button, float padding = 24.0f)
	{
		if (!button || !vgui_widget_get_context(button)) return 48.0f;
		const char* text = vgui_widget_get_text(button);
		const float text_w = text ? vgui_widget_measure_text(button, text) : 0.0f;
		if (text_w > 0.0f)
			return to_logical(button, text_w) + padding;
		const float font = editor_font_size(vgui_widget_get_context(button));
		const std::size_t chars = text ? std::strlen(text) : 0;
		return font * 0.62f * (float)chars + padding;
	}

	/* Logical width for a short axis label ("T", "In", "Out") so two-character
	 * labels never clip at larger fonts. */
	inline float editor_label_width(vgui_widget* measure_with, const char* text)
	{
		if (!measure_with || !vgui_widget_get_context(measure_with)) return 24.0f;
		const float text_w = vgui_widget_measure_text(measure_with, text);
		return std::max(24.0f, to_logical(measure_with, text_w) + 10.0f);
	}

} // namespace vgui_editor_kit
