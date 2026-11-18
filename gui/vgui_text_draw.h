#pragma once

#include <vgui/vgui.h>
#include <cstddef>
#include <utility>
#include <vector>

enum
{
	VGUI_TEXT_PASS_GLOW = 1u << 0,
	VGUI_TEXT_PASS_TEXT = 1u << 1,
	VGUI_TEXT_PASS_CLIP_TEXT = 1u << 2,
	VGUI_TEXT_PASS_INNER_GLOW = 1u << 3,
	VGUI_TEXT_PASS_ALL = VGUI_TEXT_PASS_GLOW | VGUI_TEXT_PASS_TEXT | VGUI_TEXT_PASS_INNER_GLOW
};

// Layout in device pixels, except font_size and glow sigma (logical pixels).
// color already includes widget opacity; a null glow uses the widget glow.
void vgui_draw_text_passes(vgui_widget* widget, ovg_ctx_cb* cb, rvg_t* vg,
	const char* text, float x, float y, float width, float height,
	vgui_color color, float align_x, float align_y, float font_size,
	const vgui_text_glow* glow, unsigned passes, int font_weight = 0, int font_slant = 0);
vgui_color vgui_text_widget_color(vgui_widget* widget, vgui_color color);
void vgui_text_ink_intervals(vgui_widget* widget, const char* text, float x, float y,
	float font_size, int weight, int slant, float top, float bottom,
	std::vector<std::pair<float, float>>& out);

struct vgui_glow_run {
	const char* text;
	float x, y, width, height;
	float font_size;
	int weight, slant;
};
struct vgui_glow_rect { float x, y, width, height; };
// Caller supplies the already intersected scissor. Returns false when the
// callback cannot batch, so text runs may fall back to individual calls.
bool vgui_text_glow_batch(vgui_widget*, ovg_ctx_cb*, rvg_t*, const vgui_text_glow&,
	const vgui_glow_run*, std::size_t, const vgui_glow_rect* = nullptr, std::size_t = 0);
// Incremented when the context font is replaced, even at the same size.
uint64_t vgui_text_font_revision(const vgui_context* ctx);
