// Native SDL3 example: application-owned UTF-8 + spans in VGUI_CUSTOM.
#include "pch.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>

#include <ovg_c.h>
#include <vgui/backend/vgui_multiwindow.h>
#include <vgui/richtext.h>

namespace {

constexpr vgui_color paper = 0xFF0D1523u;
constexpr vgui_color card_bg = 0xFF172235u;
constexpr vgui_color edge = 0xFF2B3D55u;
constexpr vgui_color ink = 0xFFF0F5FCu;
constexpr vgui_color muted = 0xFFA8BCD7u;
constexpr vgui_color cyan = 0xFF7DE2F3u;
constexpr vgui_color gold = 0xFFFFC890u;
constexpr vgui_color lilac = 0xFFD4B9FFu;

struct demo_state {
	vgui_widget* align[3] = {};
	vgui_widget* glow[3] = {};
	vgui_widget* sigma = nullptr;
	vgui_widget* sigma_label = nullptr;
	vgui_widget* skip_ink = nullptr;
	vgui_widget* previews[3] = {};
	vgui_widget* wrap_width = nullptr;
	vgui_widget* wrap_preview = nullptr;
};

float preview_alignment(const demo_state& state)
{
	return vgui_widget_get_checked(state.align[2]) ? 1.0f :
		(vgui_widget_get_checked(state.align[1]) ? 0.5f : 0.0f);
}

// The C API takes byte offsets, not Unicode character counts.
vgui_text_span span_for(const char* text, const char* part, vgui_color color = 0, float size = 0)
{
	vgui_text_span span{};
	if (const char* start = std::strstr(text, part)) {
		span.start = static_cast<int>(start - text);
		span.length = static_cast<int>(std::strlen(part));
	}
	span.color = color;
	span.font_size = size;
	return span;
}

struct draw_area {
	vgui_widget* widget;
	ovg_ctx_cb* cb;
	rvg_t* vg;
	float x, y, w, h;
	float align;

	template <size_t N>
	void draw(const char* text, const vgui_text_span (&spans)[N], float base_size) const
	{
		vgui_custom_draw_text_spans(widget, cb, vg, text, spans, static_cast<int>(N),
			x, y, w, h, ink, align, 0.5f, base_size);
	}
};

void draw_type(const draw_area& area)
{
	// "é" is two UTF-8 bytes. The last line also mixes sizes on one baseline.
	const char* text = "Regular / Bold / Italic\nWarm + Cool + caf\xC3\xA9\nSmall / Signal / Bold italic";
	vgui_text_span spans[] = {
		span_for(text, "Bold"), span_for(text, "Italic"),
		span_for(text, "Warm", gold), span_for(text, "Cool", cyan),
		span_for(text, "caf\xC3\xA9", lilac), span_for(text, "Small", muted, 16),
		span_for(text, "Signal", cyan, 34), span_for(text, "Bold italic", lilac)
	};
	spans[0].font_weight = spans[7].font_weight = VGUI_FONT_WEIGHT_BOLD;
	spans[1].font_slant = spans[7].font_slant = VGUI_FONT_SLANT_ITALIC;
	area.draw(text, spans, 23);
}

void draw_decor(const draw_area& area, bool skip_ink)
{
	const char* text = "Solid gypq / Double fj\nDotted / Dashed / Wavy\n\nStrike / Overline / Both";
	vgui_text_span spans[] = {
		span_for(text, "Solid gypq"), span_for(text, "Double fj"), span_for(text, "Dotted"),
		span_for(text, "Dashed"), span_for(text, "Wavy"), span_for(text, "Strike"),
		span_for(text, "Overline"), span_for(text, "Both")
	};
	constexpr unsigned styles[] = { VGUI_TEXT_DECOR_STYLE_SOLID, VGUI_TEXT_DECOR_STYLE_DOUBLE,
		VGUI_TEXT_DECOR_STYLE_DOTTED, VGUI_TEXT_DECOR_STYLE_DASHED, VGUI_TEXT_DECOR_STYLE_WAVY };
	for (int i = 0; i < 5; ++i) {
		spans[i].decor = VGUI_TEXT_DECOR_UNDERLINE;
		spans[i].decor_color = cyan;
		spans[i].decor_style = styles[i];
		spans[i].decor_thickness = 2.0f;
		// Limit the toggle to the descender examples so the other line styles stay visible.
		if (skip_ink && i < 2) spans[i].decor_flags = VGUI_TEXT_DECOR_FLAG_SKIP_INK;
	}
	spans[4].decor_offset = 3.0f; // Leave room for the wave's crest above its center.
	spans[5].decor = VGUI_TEXT_DECOR_STRIKETHROUGH;
	spans[5].decor_color = gold;
	spans[6].decor = VGUI_TEXT_DECOR_OVERLINE;
	spans[6].decor_color = lilac;
	spans[6].decor_thickness = 2.0f;
	spans[7].decor = VGUI_TEXT_DECOR_UNDERLINE | VGUI_TEXT_DECOR_OVERLINE;
	spans[7].decor_color = cyan;
	spans[7].decor_thickness = 2.0f;
	spans[7].overline.present = VGUI_TEXT_DECOR_PRESENT_COLOR | VGUI_TEXT_DECOR_PRESENT_STYLE;
	spans[7].overline.color = gold;
	spans[7].overline.style = VGUI_TEXT_DECOR_STYLE_DASHED;
	area.draw(text, spans, 23);
}

void draw_glow(const draw_area& area, const demo_state& state)
{
	const char* text = "Halo signal   /   This run has no glow";
	vgui_text_span spans[] = { span_for(text, "Halo signal", lilac, 48) };
	spans[0].font_weight = VGUI_FONT_WEIGHT_BOLD;
	if (vgui_widget_get_checked(state.glow[0])) {
		spans[0].glow.flags = VGUI_TEXT_GLOW_OFF;
	} else {
		spans[0].glow.color = 0xF063D8FFu;
		spans[0].glow.sigma = vgui_widget_get_value(state.sigma);
		spans[0].glow.mode = vgui_widget_get_checked(state.glow[2]) ? VGUI_TEXT_GLOW_INNER : VGUI_TEXT_GLOW_OUTER;
	}
	area.draw(text, spans, 19);
}

void draw_preview(vgui_widget* widget, ovg_ctx_cb* cb, rvg_t* vg,
	float x, float y, float w, float h, float, void* user)
{
	const auto& state = *static_cast<demo_state*>(user);
	const bool halo = widget == state.previews[2];
	const float inset = (halo ? 36.0f : 18.0f) * vgui_context_get_ui_scale(vgui_widget_get_context(widget));
	draw_area area{ widget, cb, vg, x + inset, y, w - 2 * inset, h, preview_alignment(state) };
	if (halo) draw_glow(area, state);
	else if (widget == state.previews[0]) draw_type(area);
	else draw_decor(area, vgui_widget_get_checked(state.skip_ink) != 0);
}

void draw_wrap_preview(vgui_widget* widget, ovg_ctx_cb* cb, rvg_t* vg,
	float x, float y, float w, float h, float, void* user)
{
	const auto& state = *static_cast<demo_state*>(user);
	const float scale = vgui_context_get_ui_scale(vgui_widget_get_context(widget));
	const float pad = 12 * scale;
	const float max_width = std::min(820 * scale, w - 2 * pad);
	const float body_height = h - 2 * pad - 20 * scale;
	if (max_width <= 0 || body_height <= 0) return;
	const float min_width = std::min(185 * scale, max_width);
	const float width = min_width + (max_width - min_width) * (vgui_widget_get_value(state.wrap_width) / 100);

	// The highlighted area and its right edge show the actual wrapping bounds.
	const float left = x + pad;
	auto fill_rect = [&](float rect_x, float rect_width, uint32_t ovg_color) {
		cb->rectangle(vg, rect_x, y + pad, rect_width, body_height);
		cb->set_source_color(vg, ovg_color); // OVG uses AABBGGRR.
		cb->fill(vg);
	};
	fill_rect(left, width, 0xFF35251Au);
	fill_rect(left + width, scale, 0xFF847046u);

	const char* text = "Drag the slider to reflow a caf\xC3\xA9 note. Bold text and a wavy underline keep their styles.\n"
		"After the hard break.";
	constexpr float font_size = 17;
	vgui_text_span spans[] = {
		span_for(text, "caf\xC3\xA9", lilac), span_for(text, "Bold text", gold, 19),
		span_for(text, "wavy underline", cyan)
	};
	spans[1].font_weight = VGUI_FONT_WEIGHT_BOLD;
	spans[2].decor = VGUI_TEXT_DECOR_UNDERLINE;
	spans[2].decor_style = VGUI_TEXT_DECOR_STYLE_WAVY;
	spans[2].decor_offset = 2;
	constexpr int span_count = static_cast<int>(sizeof(spans) / sizeof(spans[0]));
	float text_width = 0, text_height = 0;
	vgui_widget_measure_text_spans_wrapped(widget, text, spans, span_count, width, font_size, &text_width, &text_height);
	vgui_custom_draw_text_spans_wrapped(widget, cb, vg, text, spans, span_count,
		left, y + pad, width, body_height, ink, preview_alignment(state), 0, font_size);

	char metrics[88];
	std::snprintf(metrics, sizeof(metrics), "Box %.0f px   /   Text %.0f x %.0f px", width, text_width, text_height);
	vgui_custom_draw_text_sized(widget, cb, vg, metrics,
		left, y + pad + body_height, w - 2 * pad, 20 * scale, muted, 0, 0.5f, 13);
}

void update_previews(vgui_widget*, float, void* user)
{
	const auto& state = *static_cast<demo_state*>(user);
	char label[24];
	std::snprintf(label, sizeof(label), "%.0f px", vgui_widget_get_value(state.sigma));
	vgui_widget_set_text(state.sigma_label, label);
	for (auto* preview : state.previews) vgui_widget_mark_dirty(preview);
	if (state.wrap_preview) vgui_widget_mark_dirty(state.wrap_preview);
}

vgui_widget* panel(vgui_widget* parent, vgui_layout_kind layout, float gap, vgui_color color = 0)
{
	auto* w = vgui_widget_add(parent, VGUI_PANEL, nullptr);
	vgui_widget_set_layout(w, layout);
	vgui_widget_set_gap(w, gap);
	vgui_widget_set_padding(w, 0, 0, 0, 0);
	vgui_widget_set_background(w, color);
	vgui_widget_set_border(w, color ? edge : 0, color ? 1 : 0);
	vgui_widget_set_align_items(w, VGUI_ALIGN_STRETCH);
	if (color) vgui_widget_set_rounding(w, 12);
	return w;
}

vgui_widget* label(vgui_widget* parent, const char* text, float size, vgui_color color)
{
	auto* w = vgui_widget_add(parent, VGUI_LABEL, text);
	vgui_widget_set_font_size(w, size);
	vgui_widget_set_text_color(w, color);
	return w;
}

void add_radios(vgui_widget* parent, const char* title, const char* const (&names)[3],
	vgui_widget* (&buttons)[3], int selected, demo_state& state)
{
	auto* group = panel(parent, VGUI_LAYOUT_ROW, 10);
	vgui_widget_set_align_items(group, VGUI_ALIGN_CENTER);
	label(group, title, 13, cyan);
	// VGUI_RADIO exclusivity is scoped to a shared parent.
	for (int i = 0; i < 3; ++i) {
		buttons[i] = vgui_widget_add(group, VGUI_RADIO, names[i]);
		vgui_widget_set_checked(buttons[i], i == selected);
		vgui_widget_on_value(buttons[i], update_previews, &state);
	}
}

void add_specimen(vgui_widget* parent, demo_state& state, int index,
	const char* title, const char* hint, float height)
{
	auto* card = panel(parent, VGUI_LAYOUT_COLUMN, 6, card_bg);
	if (index < 2) {
		vgui_widget_set_size(card, 418, 0);
		vgui_widget_set_grow(card, 1);
	}
	vgui_widget_set_padding(card, 16, 12, 16, 14);
	label(card, title, 15, cyan);
	auto* preview = state.previews[index] = vgui_widget_add(card, VGUI_CUSTOM, nullptr);
	vgui_widget_set_size(preview, 0, height);
	vgui_widget_set_background(preview, paper);
	vgui_widget_set_border(preview, 0, 0);
	vgui_widget_set_rounding(preview, 7);
	vgui_custom_set_draw(preview, draw_preview, &state);
	label(card, hint, 13, muted);
}

void add_wrap_specimen(vgui_widget* root, demo_state& state)
{
	auto* card = panel(root, VGUI_LAYOUT_COLUMN, 6, card_bg);
	vgui_widget_set_padding(card, 16, 12, 16, 14);
	auto* bar = panel(card, VGUI_LAYOUT_ROW, 14);
	vgui_widget_set_align_items(bar, VGUI_ALIGN_CENTER);
	vgui_widget_set_grow(label(bar, "04 / AUTO WRAP", 15, cyan), 1);
	label(bar, "WIDTH", 13, cyan);
	state.wrap_width = vgui_widget_add(bar, VGUI_SLIDER, nullptr);
	vgui_widget_set_size(state.wrap_width, 180, 22);
	vgui_widget_set_range(state.wrap_width, 0, 100);
	vgui_widget_set_value(state.wrap_width, 54);

	auto* preview = state.wrap_preview = vgui_widget_add(card, VGUI_CUSTOM, nullptr);
	vgui_widget_set_size(preview, 0, 174);
	vgui_widget_set_background(preview, paper);
	vgui_widget_set_border(preview, 0, 0);
	vgui_widget_set_rounding(preview, 7);
	vgui_custom_set_draw(preview, draw_wrap_preview, &state);
	vgui_widget_on_value(state.wrap_width, [](vgui_widget*, float, void* user) {
		vgui_widget_mark_dirty(static_cast<demo_state*>(user)->wrap_preview);
	}, &state);
}

void build_ui(vgui_context* ui, demo_state& state)
{
	vgui_theme theme{};
	vgui_context_get_theme(ui, &theme);
	theme.text = ink;
	theme.accent = 0xFF57B5C9u;
	theme.accent_hover = cyan;
	theme.button_bg = paper;
	theme.button_border = edge;
	theme.track = edge;
	vgui_context_set_theme(ui, &theme);

	auto* root = vgui_root(ui);
	vgui_widget_set_layout(root, VGUI_LAYOUT_COLUMN);
	vgui_widget_set_align_items(root, VGUI_ALIGN_STRETCH);
	vgui_widget_set_background(root, paper);
	vgui_widget_set_border(root, 0, 0);
	vgui_widget_set_padding(root, 26, 24, 26, 24);
	vgui_widget_set_gap(root, 16);
	vgui_panel_set_scrollable(root, 0, 1);

	auto* header = panel(root, VGUI_LAYOUT_COLUMN, 3);
	label(header, "OVG / TEXT LAB", 14, cyan);
	vgui_widget_set_font_weight(label(header, "Rich text", 42, ink), VGUI_FONT_WEIGHT_BOLD);
	label(header, "UTF-8 byte ranges, mixed typography and glyph effects in a single draw call.", 16, muted);

	auto* controls = panel(root, VGUI_LAYOUT_ROW, 14, card_bg);
	vgui_widget_set_align_items(controls, VGUI_ALIGN_CENTER);
	vgui_widget_set_justify(controls, VGUI_JUSTIFY_SPACE_BETWEEN);
	vgui_widget_set_padding(controls, 16, 10, 16, 10);
	add_radios(controls, "GLOW", { "Off", "Outer", "Inner" }, state.glow, 1, state);
	add_radios(controls, "ALIGN", { "Left", "Center", "Right" }, state.align, 0, state);
	auto* blur = panel(controls, VGUI_LAYOUT_ROW, 8);
	vgui_widget_set_align_items(blur, VGUI_ALIGN_CENTER);
	label(blur, "SIGMA", 13, cyan);
	state.sigma = vgui_widget_add(blur, VGUI_SLIDER, nullptr);
	vgui_widget_set_size(state.sigma, 76, 22);
	vgui_widget_set_range(state.sigma, 1, 9);
	vgui_widget_set_step(state.sigma, 1);
	vgui_widget_set_value(state.sigma, 4);
	vgui_widget_on_value(state.sigma, update_previews, &state);
	state.sigma_label = label(blur, "4 px", 13, muted);
	state.skip_ink = vgui_widget_add(controls, VGUI_CHECKBOX, "Skip ink");
	vgui_widget_set_checked(state.skip_ink, 1);
	vgui_widget_on_value(state.skip_ink, update_previews, &state);

	auto* grid = panel(root, VGUI_LAYOUT_ROW, 16);
	add_specimen(grid, state, 0, "01 / TYPOGRAPHY", "Weight, slant, size and color on shared baselines.", 134);
	add_specimen(grid, state, 1, "02 / DECORATIONS", "Toggle Skip ink to see gaps around gypq and fj.", 134);
	add_specimen(root, state, 2, "03 / GLOW", "Only the first span glows. Change mode or adjust the blur sigma.", 132);
	add_wrap_specimen(root, state);
	label(root, "VGUI_CUSTOM  /  Explicit and automatic line breaks", 13, muted);
}

} // namespace

int main()
{
	app_mgr app;
	app.set_msaa_samples(4);
	const char* path = VGUI_SOURCE_DIR "/sample/res/fonts/DejaVuSans.ttf";
	std::unique_ptr<font_familys_t, decltype(&delete_font_family)> font(
		load_font_from_path(app.font_ctx, path, "RichtextDemo") ?
			new_font_family(app.font_ctx, "RichtextDemo", 0) : nullptr, delete_font_family);
	if (!font) {
		std::fprintf(stderr, "Cannot load demo font: %s\n", path);
		return 1;
	}
	if (!app.init_gpu(true)) {
		std::fprintf(stderr, "Cannot initialize the SDL3 Vulkan renderer: %s\n", SDL_GetError());
		return 1;
	}
	auto* window = app.create("VGUI / Rich Text", 1060, 926, 0, true);
	if (!window) {
		std::fprintf(stderr, "Cannot open the rich text demo: %s\n", SDL_GetError());
		return 1;
	}
	SDL_SetWindowMinimumSize(window->window, 920, 480);

	demo_state state;
	std::unique_ptr<vgui_context, decltype(&vgui_destroy)> ui(vgui_create(font.get(), 15), vgui_destroy);
	std::unique_ptr<ovg_ctx_cb, decltype(&free_ctx_cb)> cb(new_ctx_cb(), free_ctx_cb);
	multiwindow_host host(&app, cb.get());
	if (!host.attach(window, ui.get())) {
		std::fprintf(stderr, "Cannot create the rich text demo canvas\n");
		return 1;
	}
	build_ui(ui.get(), state);
	host.set_on_demand_render(true);
	Uint64 last = SDL_GetTicks();
	while (app.get_event() >= 0 && !window->pending_close) {
		const Uint64 now = SDL_GetTicks();
		host.begin_frame(static_cast<float>(now - last) / 1000.0f);
		last = now;
		host.draw_all();
		host.end_frame();
	}
	app.wait_gpu();
	window->viewport->ui = nullptr; // The unique_ptr owns the UI, not the viewport.
	return 0;
}
