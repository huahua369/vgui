#pragma once
/*
 * E01 internal helper: editor-kit components mix two coordinate spaces.
 *
 * vgui widget geometry setters take logical units and multiply by ui_scale,
 * while vgui_widget_get_rect() / vgui_panel_get_scroll() report effective
 * render pixels.  Use these helpers whenever a measured pixel value is fed
 * back into a logical setter (or vice versa).  Drawing and hit testing can
 * keep using pixels directly.
 */
#include <vgui/vgui.h>

#include <algorithm>
#include <cmath>

namespace vgui_editor_kit {

	inline float ui_scale_of(vgui_context* ctx)
	{
		const float scale = ctx ? vgui_context_get_ui_scale(ctx) : 1.0f;
		return std::isfinite(scale) && scale > 0.0f ? scale : 1.0f;
	}

	inline float ui_scale_of(vgui_widget* widget)
	{
		return ui_scale_of(widget ? vgui_widget_get_context(widget) : nullptr);
	}

	/* Convert a measured effective-pixel value into logical units. */
	inline float to_logical(vgui_widget* widget, float pixels)
	{
		return pixels / ui_scale_of(widget);
	}

	/* Convert a logical value into effective pixels. */
	inline float to_physical(vgui_widget* widget, float logical)
	{
		return logical * ui_scale_of(widget);
	}

} // namespace vgui_editor_kit
