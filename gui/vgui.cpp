/*
gui实现

创建日期：2026-9-12
*/

#include "pch.h"
#include <SDL3/SDL.h>
#include "vgui_internal.h"
#include "vgui.h"

#include <vgui_sdl3.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_set>

#include <mapView.h>

#include <ovg_c.h>
#include <harfbuzz/hb.h>

#include <clipper2/clipper.h> 
using namespace Clipper2Lib;
//--------------------------------------------------------------------------------------------------------------------------------------------------
 
// todo font_get_char_extent
glm::ivec4 font_get_char_extent(char32_t ch, unsigned char font_size, font_familys_t* fallbacks, font_family_t** oft)
{
	glm::ivec4 ret = {};
	font_family_t* rfont = nullptr;
	//auto g = fallbacks->familys[0]->get_glyph_index0(ch, &rfont, fallbacks);
	//if (g && rfont)
	//{
	//	if (oft)*oft = rfont;
	//	double fns = font_size;
	//	double scale = rfont->get_scale(fns);
	//	int x0 = 0, y0 = 0, x1 = 0, y1 = 0, advance, lsb;
	//	stb_font::buildGlyphBitmap(rfont->font, g, scale, &advance, &lsb, &x0, &y0, &x1, &y1);
	//	double adv = scale * advance;
	//	auto bl = rfont->get_base_line(font_size);
	//	ret = { x1 - x0, y1 - y0, adv, bl };
	//}
	return ret;
}

int font_get_baseline(font_familys_t* family, int fontsize) {
	float baseline = 0;
	if (family)
	{
		int n = family->count;
		for (int i = 0; i < n; i++) {
			auto p = family->familys[i];
			if (p)
			{
				auto ex = get_font_extents(p->font, fontsize, false);
				baseline = std::max(baseline, ex.x);
			}
		}
	}
	return baseline;
}
int font_get_lineheight(font_familys_t* family, int fontsize, bool first) {
	float h = 0;
	if (family)
	{
		int n = first ? 1 : family->count;
		for (int i = 0; i < n; i++) {
			auto p = family->familys[i];
			if (p)
			{
				auto ex = get_font_extents(p->font, fontsize, false);
				h = std::max(h, ex.x - ex.y + ex.z);
			}
		}
	}
	return h;
}

void gui_viewport::set_viewport(const glm::ivec4& rc)
{
	//_root._pos = { rc.x,rc.y };
	//_root._size = { rc.z,rc.w };
}

void gui_viewport::clear()
{
	//_root.clear();
}

//void gui_viewport::add_div(div_cx0* c)
//{
//	_root.add(c);
//}

gui_viewport::~gui_viewport()
{
	if (ui)
	{
		vgui_destroy(ui);
		ui = nullptr;
	}
}

void gui_viewport::trigger(dev_event_t* e)
{
	if (!e) return;
	if (!ui)
	{
		e->ret = 0;
		return;
	}

	int consumed = vgui_dispatch_dev_event(ui, e);
	if (consumed) e->ret = 1;

	io.WantCaptureMouse = vgui_context_wants_mouse(ui) != 0;
	io.WantCaptureKey = vgui_context_wants_keyboard(ui) != 0;
	io.WantTextInput = vgui_context_wants_text_input(ui) != 0;
	vgui_context_get_text_input_rect(ui, &ime_rect.x, &ime_rect.y, &ime_rect.z, &ime_rect.w);

	if (e->type == dev_event_type_e::mouse_move_e && e->v.m)
		e->v.m->cursor = (cursor_st)vgui_context_get_cursor(ui);
}
//--------------------------------------------------------------------------------------------------------------------------------------------------
// C API implementation
//
// The public API (gui/vgui.h) is backed by an independent, retained-mode
// widget tree.  The legacy widget classes above are kept for C++ users and for
// the drawing helpers, but the C API has its own layout, event dispatch and
// renderer.  This keeps the public surface small and makes the behaviour
// predictable for C consumers.
//--------------------------------------------------------------------------------------------------------------------------------------------------

// Undo/redo entry: text plus the selection that was active at the time.
struct vgui_edit_snapshot
{
	std::string text;
	int cursor = 0;
	int anchor = 0;
};

struct vgui_widget
{
	vgui_context* ctx = nullptr;
	vgui_widget* parent = nullptr;
	std::vector<vgui_widget*> children;
	vgui_widget_kind kind = VGUI_PANEL;

	std::string text;
	std::string placeholder;
	std::string edit_text;
	std::string ime_text;
	std::string id;

	void* user = nullptr;
	vgui_event_fn event_fn = nullptr;
	vgui_value_fn value_fn = nullptr;
	vgui_text_fn text_fn = nullptr;

	// layout ---------------------------------------------------------------
	vgui_layout_kind layout = VGUI_LAYOUT_ABSOLUTE;
	vgui_align align_items = VGUI_ALIGN_START;
	vgui_align align_self = VGUI_ALIGN_AUTO;
	vgui_justify justify = VGUI_JUSTIFY_START;
	float x = 0, y = 0;
	float req_w = 0, req_h = 0;
	float min_w = 0, min_h = 0;
	float max_w = 0, max_h = 0;
	float margin[4] = { 0, 0, 0, 0 };	// left, top, right, bottom
	float padding[4] = { 0, 0, 0, 0 };
	float gap = 0;
	float grow = 0;

	int scrollable_x = 0, scrollable_y = 0;
	float scroll_x = 0, scroll_y = 0;
	float content_w = 0, content_h = 0;
	int scroll_drag_axis = 0;	// 面板滚动条拖动: 0 无 / 1 垂直 / 2 水平
	float scroll_drag_grab = 0;

	float abs_x = 0, abs_y = 0, abs_w = 0, abs_h = 0;
	float intrinsic_w = 0, intrinsic_h = 0;

	// visual ---------------------------------------------------------------
	vgui_color bg = 0;
	vgui_color border = 0;
	vgui_color text_color = 0;
	vgui_color accent = 0;
	vgui_color track = 0;
	float font_size = 0;
	float rounding = 0;
	float border_width = 0;
	float opacity = 1.0f;
	float focus_alpha = -1.0f;	// < 0 -> use the context value; 0 hides the ring
	float text_align_x = 0.5f;
	float text_align_y = 0.5f;
	int clip_children = 1;

	// state ----------------------------------------------------------------
	int visible = 1;
	int enabled = 1;
	int focusable = 0;
	int hovered = 0;
	int pressed = 0;
	int focused = 0;
	int pressed_button = 0;
	float hover_anim = 0;
	float press_anim = 0;
	float check_anim = 0;
	vgui_cursor cursor_type = VGUI_CURSOR_ARROW;

	// values ---------------------------------------------------------------
	float value = 0.0f;
	float range_min = 0.0f;
	float range_max = 1.0f;
	float step = 0.0f;
	int checked = 0;
	int vertical = 0;

	// edit -----------------------------------------------------------------
	int edit_cursor = 0;
	int edit_anchor = 0;
	int edit_drag = 0;			// 鼠标拖选模式: 0 无 / 1 字符 / 2 词 / 3 行
	int ime_cursor = 0;
	int ime_length = 0;
	int multiline = 0;
	int password = 0;
	int readonly = 0;
	char password_char = '*';
	float edit_scroll_x = 0;
	float edit_scroll_y = 0;
	int edit_scroll_drag = 0;	// 多行编辑框内容滚动条拖动中
	float edit_scroll_grab = 0;
	float caret_blink = 0;
	vgui_color sel_color = 0;	// 0 -> theme.edit_selection
	float sel_rounding = 2.0f;	// 选中高亮圆角半径
	std::vector<vgui_edit_snapshot> undo_stack;
	std::vector<vgui_edit_snapshot> redo_stack;
	double last_edit_push = -10.0;
	int last_edit_push_kind = 0;

	bool destroy_pending = false;
};

struct vgui_context
{
	font_familys_t* font = nullptr;
	float font_size = 16.0f;
	int width = 0, height = 0;
	vgui_theme theme = {};
	float focus_alpha = 1.0f;	// default focus-ring opacity
	vgui_widget* root = nullptr;

	std::unordered_set<vgui_widget*> all_widgets;
	std::unordered_set<vgui_widget*> alive;
	std::vector<vgui_widget*> pending_delete;

	// input state ----------------------------------------------------------
	float mouse_x = 0, mouse_y = 0;
	int mouse_down[8] = {};
	int click_count[8] = {};
	double click_time[8] = { -10, -10, -10, -10, -10, -10, -10, -10 };
	vgui_widget* hot = nullptr;
	vgui_widget* active = nullptr;
	vgui_widget* press_target = nullptr;
	int press_button = 0;
	vgui_widget* focus = nullptr;
	vgui_widget* slider_drag = nullptr;
	double time = 0;

	int want_mouse = 0;
	int want_key = 0;
	int want_text = 0;
	vgui_cursor cursor = VGUI_CURSOR_ARROW;

	bool layout_dirty = true;
	bool in_input = false;
};

namespace {

//----------------------------------------------------------------------------
// small helpers
//----------------------------------------------------------------------------

static float vgui_clampf(float v, float lo, float hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

static uint32_t vgui_mix_color(uint32_t a, uint32_t b, float t)
{
	t = vgui_clampf(t, 0.0f, 1.0f);
	float aa = (float)((a >> 24) & 0xFF), ar = (float)((a >> 16) & 0xFF);
	float ag = (float)((a >> 8) & 0xFF), ab = (float)(a & 0xFF);
	float ba = (float)((b >> 24) & 0xFF), br = (float)((b >> 16) & 0xFF);
	float bg = (float)((b >> 8) & 0xFF), bb = (float)(b & 0xFF);
	uint32_t r = (uint32_t)(ar + (br - ar) * t + 0.5f);
	uint32_t g = (uint32_t)(ag + (bg - ag) * t + 0.5f);
	uint32_t bl = (uint32_t)(ab + (bb - ab) * t + 0.5f);
	uint32_t al = (uint32_t)(aa + (ba - aa) * t + 0.5f);
	return (al << 24) | (r << 16) | (g << 8) | bl;
}

static uint32_t vgui_alpha_color(uint32_t c, float alpha)
{
	uint32_t a = (c >> 24) & 0xFF;
	a = (uint32_t)(a * vgui_clampf(alpha, 0.0f, 1.0f) + 0.5f);
	return (c & 0x00FFFFFF) | (a << 24);
}

static bool vgui_point_in(const vgui_widget* w, float x, float y)
{
	return x >= w->abs_x && y >= w->abs_y && x < w->abs_x + w->abs_w && y < w->abs_y + w->abs_h;
}

static bool vgui_is_cont(unsigned char c)
{
	return (c & 0xC0) == 0x80;
}

static int vgui_u8_next(const std::string& s, int i)
{
	if (i < 0) return 0;
	if (i >= (int)s.size()) return (int)s.size();
	unsigned char c = (unsigned char)s[(size_t)i];
	int n = 1;
	if (c >= 0xF0) n = 4;
	else if (c >= 0xE0) n = 3;
	else if (c >= 0xC0) n = 2;
	int r = i + n;
	return r > (int)s.size() ? (int)s.size() : r;
}

static int vgui_u8_prev(const std::string& s, int i)
{
	if (i <= 0) return 0;
	if (i > (int)s.size()) i = (int)s.size();
	i--;
	while (i > 0 && vgui_is_cont((unsigned char)s[(size_t)i])) i--;
	return i;
}

static int vgui_u8_count(const std::string& s)
{
	int n = 0;
	for (int i = 0; i < (int)s.size(); i = vgui_u8_next(s, i)) n++;
	return n;
}

static uint32_t vgui_u8_decode(const char* text)
{
	if (!text || !*text) return 0;
	const unsigned char* s = (const unsigned char*)text;
	if (s[0] < 0x80) return s[0];
	if ((s[0] & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80)
		return ((uint32_t)(s[0] & 0x1F) << 6) | (uint32_t)(s[1] & 0x3F);
	if ((s[0] & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80)
		return ((uint32_t)(s[0] & 0x0F) << 12) | ((uint32_t)(s[1] & 0x3F) << 6) | (uint32_t)(s[2] & 0x3F);
	if ((s[0] & 0xF8) == 0xF0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80)
		return ((uint32_t)(s[0] & 0x07) << 18) | ((uint32_t)(s[1] & 0x3F) << 12) |
			((uint32_t)(s[2] & 0x3F) << 6) | (uint32_t)(s[3] & 0x3F);
	return s[0];
}

static int vgui_u8_byte_at_col(const std::string& s, int col)
{
	int i = 0;
	for (int n = 0; n < col && i < (int)s.size(); n++) i = vgui_u8_next(s, i);
	return i;
}

static int vgui_u8_col_at_byte(const std::string& s, int byte_index)
{
	int i = 0, n = 0;
	while (i < byte_index && i < (int)s.size())
	{
		i = vgui_u8_next(s, i);
		n++;
	}
	return n;
}

static float vgui_font_size(vgui_context* ctx, vgui_widget* w)
{
	float fs = w && w->font_size > 0 ? w->font_size : (ctx ? ctx->font_size : 16.0f);
	if (fs < 1) fs = 16.0f;
	return fs;
}

static float vgui_line_height(vgui_context* ctx, vgui_widget* w)
{
	float fs = vgui_font_size(ctx, w);
	int lh = 0;
	if (ctx && ctx->font)
		lh = font_get_lineheight(ctx->font, (int)(fs + 0.5f), true);
	if (lh <= 0)
		lh = (int)(fs * 1.35f + 0.5f);
	if (lh < 1) lh = 1;
	return (float)lh;
}

// ovg anchors a text run on its baseline: vg_add_text_run leaves
// baseline_y = text_st_t::pos.y (the ascender is computed but unused).  The UI
// works in line boxes, so the renderer needs pos.y = box top + ascender.
// Mirrors ovg's own ascent source, the primary family at the pixel size.
static float vgui_text_ascent(vgui_context* ctx, vgui_widget* w)
{
	float fs = vgui_font_size(ctx, w);
	float a = 0;
	if (ctx && ctx->font && ctx->font->count > 0 && ctx->font->familys[0] && ctx->font->familys[0]->font)
		a = get_font_extents(ctx->font->familys[0]->font, (int)(fs + 0.5f), false).x;
	if (a <= 0) a = fs * 0.8f;
	return a;
}

// Real text measurement through HarfBuzz, mirroring ovg's shaping (font picked
// by coverage, advances taken at the shaping scale) so layout, alignment and
// the renderer agree.  Keeps the renderer text crisp and the auto-sized boxes
// exact.
static float vgui_measure_text_range(font_familys_t* ffs, int fontsize, const char* text, int len)
{
	if (!ffs || ffs->count < 1 || !text || len <= 0) return 0;
	if (fontsize < 1) fontsize = 16;
	float best = 0, cur = 0;
	for (int i = 0; i < len;)
	{
		unsigned char c = (unsigned char)text[i];
		if (c == '\n')
		{
			best = std::max(best, cur);
			cur = 0;
			i++;
			continue;
		}
		uint32_t cp = vgui_u8_decode(text + i);
		int n = 1;
		if (c >= 0xF0) n = 4;
		else if (c >= 0xE0) n = 3;
		else if (c >= 0xC0) n = 2;
		i += n;

		// Same family selection as ovg: first family whose coverage has the
		// codepoint, otherwise the primary family.
		font_family_t* fam = ffs->familys[0];
		for (int f = 0; f < ffs->count; f++)
		{
			font_family_t* cand = ffs->familys[f];
			if (cand && cand->font && cand->coverage && hb_set_has(cand->coverage, cp))
			{
				fam = cand;
				break;
			}
		}
		if (!fam || !fam->font)
		{
			cur += fontsize * 0.5f;
			continue;
		}
		hb_font_set_scale(fam->font, fontsize, fontsize);
		hb_codepoint_t gid = 0;
		hb_font_get_nominal_glyph(fam->font, cp, &gid);
		cur += (float)hb_font_get_glyph_h_advance(fam->font, gid);
	}
	return std::max(best, cur);
}

static float vgui_text_width(vgui_widget* w, const std::string& text)
{
	vgui_context* ctx = w ? w->ctx : nullptr;
	float fs = vgui_font_size(ctx, w);
	return vgui_measure_text_range(ctx ? ctx->font : nullptr, (int)fs, text.c_str(), (int)text.size());
}

static float vgui_text_width_n(vgui_widget* w, const char* text, int len)
{
	vgui_context* ctx = w ? w->ctx : nullptr;
	float fs = vgui_font_size(ctx, w);
	return vgui_measure_text_range(ctx ? ctx->font : nullptr, (int)fs, text, len);
}

static int vgui_text_lines(const std::string& text)
{
	int n = 1;
	for (char c : text) if (c == '\n') n++;
	return n;
}

static void vgui_mark_dirty(vgui_widget* w)
{
	if (w && w->ctx)
		w->ctx->layout_dirty = true;
}

//----------------------------------------------------------------------------
// event callbacks
//----------------------------------------------------------------------------

static void vgui_emit(vgui_widget* w, vgui_event_type type, float lx = 0, float ly = 0,
	int button = 0, int clicks = 0)
{
	if (!w || !w->event_fn || !w->ctx || !w->ctx->alive.count(w))
		return;
	vgui_event e = {};
	e.type = (int)type;
	e.x = lx;
	e.y = ly;
	e.button = button;
	e.clicks = clicks;
	w->event_fn(w, &e, w->user);
}

static void vgui_emit_full(vgui_widget* w, const vgui_event& e)
{
	if (!w || !w->event_fn || !w->ctx || !w->ctx->alive.count(w))
		return;
	w->event_fn(w, &e, w->user);
}

static float vgui_value_of(vgui_widget* w)
{
	if (!w) return 0;
	switch (w->kind)
	{
	case VGUI_CHECKBOX:
	case VGUI_RADIO:
	case VGUI_SWITCH:
		return w->checked ? 1.0f : 0.0f;
	case VGUI_SLIDER:
	case VGUI_PROGRESS:
		return w->range_min + w->value * (w->range_max - w->range_min);
	default:
		return w->value;
	}
}

static void vgui_notify_value(vgui_widget* w, vgui_event_type type = VGUI_EVENT_VALUE_CHANGED)
{
	if (!w || !w->ctx || !w->ctx->alive.count(w)) return;
	float v = vgui_value_of(w);
	if (w->value_fn)
		w->value_fn(w, v, w->user);
	if (w->event_fn)
	{
		vgui_event e = {};
		e.type = (int)type;
		vgui_emit_full(w, e);
	}
}

static void vgui_notify_text(vgui_widget* w)
{
	if (!w || !w->ctx || !w->ctx->alive.count(w)) return;
	if (w->text_fn)
		w->text_fn(w, w->text.c_str(), w->user);
}

//----------------------------------------------------------------------------
// widget construction / theme defaults
//----------------------------------------------------------------------------

static vgui_theme vgui_default_theme()
{
	vgui_theme t = {};
	t.text = 0xFFE6E6E6;
	t.panel_bg = 0xFF1E1E1E;
	t.panel_border = 0xFF3C3C3C;
	t.button_bg = 0xFF3A3A3A;
	t.button_hover = 0xFF4A4A4A;
	t.button_active = 0xFF2A2A2A;
	t.button_border = 0xFF555555;
	t.accent = 0xFF409EFF;
	t.accent_hover = 0xFF66B1FF;
	t.track = 0xFF303030;
	t.edit_bg = 0xFF252525;
	t.edit_selection = 0x80409EFF;
	t.scrollbar = 0xFF555555;
	t.disabled = 0xFF808080;
	return t;
}

static vgui_widget* vgui_new_widget(vgui_context* ctx, vgui_widget_kind kind, const char* text)
{
	vgui_widget* w = new vgui_widget();
	w->ctx = ctx;
	w->kind = kind;
	if (text) w->text = text;

	const vgui_theme& th = ctx->theme;
	w->text_color = th.text;
	w->accent = th.accent;
	w->track = th.track;

	switch (kind)
	{
	case VGUI_PANEL:
		w->bg = th.panel_bg;
		w->border = th.panel_border;
		w->border_width = 1;
		w->padding[0] = w->padding[1] = w->padding[2] = w->padding[3] = 8;
		w->clip_children = 1;
		break;
	case VGUI_LABEL:
		w->padding[0] = w->padding[2] = 2;
		w->text_align_x = 0.0f;
		break;
	case VGUI_BUTTON:
		w->bg = th.button_bg;
		w->border = th.button_border;
		w->border_width = 1;
		w->rounding = 4;
		w->padding[0] = w->padding[2] = 12;
		w->padding[1] = w->padding[3] = 6;
		w->text_align_x = 0.5f;
		w->focusable = 1;
		w->cursor_type = VGUI_CURSOR_HAND;
		break;
	case VGUI_CHECKBOX:
	case VGUI_RADIO:
	case VGUI_SWITCH:
		w->bg = 0;
		w->text_align_x = 0.0f;
		w->focusable = 1;
		w->cursor_type = VGUI_CURSOR_HAND;
		break;
	case VGUI_SLIDER:
		w->bg = 0;
		w->req_w = 180;
		w->req_h = 24;
		w->focusable = 1;
		w->cursor_type = VGUI_CURSOR_HAND;
		break;
	case VGUI_PROGRESS:
		w->bg = 0;
		w->req_w = 180;
		w->req_h = 20;
		w->text_align_x = 0.5f;
		break;
	case VGUI_EDIT:
		w->bg = th.edit_bg;
		w->border = th.button_border;
		w->border_width = 1;
		w->rounding = 3;
		w->req_w = 160;
		w->req_h = 30;
		w->padding[0] = w->padding[2] = 6;
		w->padding[1] = w->padding[3] = 4;
		w->text_align_x = 0.0f;
		w->focusable = 1;
		w->cursor_type = VGUI_CURSOR_IBEAM;
		break;
	case VGUI_SEPARATOR:
		w->bg = th.panel_border;
		w->req_h = 1;
		break;
	}

	ctx->all_widgets.insert(w);
	ctx->alive.insert(w);
	ctx->layout_dirty = true;
	return w;
}

static void vgui_measure_widget(vgui_widget* w, vgui_context* ctx);

static float vgui_child_req_w(vgui_widget* c)
{
	float v = c->req_w > 0 ? c->req_w : c->intrinsic_w;
	if (c->min_w > 0) v = std::max(v, c->min_w);
	if (c->max_w > 0) v = std::min(v, c->max_w);
	return std::max(v, 0.0f);
}

static float vgui_child_req_h(vgui_widget* c)
{
	float v = c->req_h > 0 ? c->req_h : c->intrinsic_h;
	if (c->min_h > 0) v = std::max(v, c->min_h);
	if (c->max_h > 0) v = std::min(v, c->max_h);
	return std::max(v, 0.0f);
}

static void vgui_measure_widget(vgui_widget* w, vgui_context* ctx)
{
	if (!w) return;
	float lh = vgui_line_height(ctx, w);
	float tw = vgui_text_width(w, w->text);
	float pl = w->padding[0], pt = w->padding[1], pr = w->padding[2], pb = w->padding[3];

	switch (w->kind)
	{
	case VGUI_PANEL:
	{
		float content_w = 0, content_h = 0;
		float row_w = 0, row_h = 0, col_w = 0, col_h = 0;
		int visible_count = 0;
		for (vgui_widget* c : w->children)
		{
			if (!c->visible) continue;
			vgui_measure_widget(c, ctx);
			float cw = vgui_child_req_w(c) + c->margin[0] + c->margin[2];
			float ch = vgui_child_req_h(c) + c->margin[1] + c->margin[3];
			if (w->layout == VGUI_LAYOUT_ROW)
			{
				row_w += cw;
				row_h = std::max(row_h, ch);
			}
			else if (w->layout == VGUI_LAYOUT_COLUMN)
			{
				col_w = std::max(col_w, cw);
				col_h += ch;
			}
			else
			{
				content_w = std::max(content_w, c->x + cw);
				content_h = std::max(content_h, c->y + ch);
			}
			visible_count++;
		}
		if (w->layout == VGUI_LAYOUT_ROW)
		{
			content_w = row_w + (visible_count > 1 ? w->gap * (visible_count - 1) : 0);
			content_h = row_h;
		}
		else if (w->layout == VGUI_LAYOUT_COLUMN)
		{
			content_w = col_w;
			content_h = col_h + (visible_count > 1 ? w->gap * (visible_count - 1) : 0);
		}
		w->intrinsic_w = content_w + pl + pr;
		w->intrinsic_h = content_h + pt + pb;
	}
	break;
	case VGUI_LABEL:
		w->intrinsic_w = tw + pl + pr;
		w->intrinsic_h = lh * vgui_text_lines(w->text) + pt + pb;
		break;
	case VGUI_BUTTON:
		w->intrinsic_w = tw + pl + pr + 4;
		w->intrinsic_h = std::max(lh + pt + pb, 28.0f);
		break;
	case VGUI_CHECKBOX:
	case VGUI_RADIO:
	{
		float box = std::max(14.0f, std::min(18.0f, lh - 2));
		float label = w->text.empty() ? 0 : (tw + 8);
		w->intrinsic_w = box + label + pl + pr;
		w->intrinsic_h = std::max(box, lh) + pt + pb;
	}
	break;
	case VGUI_SWITCH:
	{
		float track = 36;
		float label = w->text.empty() ? 0 : (tw + 8);
		w->intrinsic_w = track + label + pl + pr;
		w->intrinsic_h = std::max(20.0f, lh) + pt + pb;
	}
	break;
	case VGUI_SLIDER:
		w->intrinsic_w = w->req_w > 0 ? w->req_w : 180;
		w->intrinsic_h = std::max(w->req_h, 24.0f);
		break;
	case VGUI_PROGRESS:
		w->intrinsic_w = w->req_w > 0 ? w->req_w : 180;
		w->intrinsic_h = std::max(w->req_h, 20.0f);
		break;
	case VGUI_EDIT:
	{
		float minw = w->text.empty() ? 160.0f : std::max(160.0f, tw + pl + pr + 12);
		w->intrinsic_w = w->req_w > 0 ? w->req_w : minw;
		float lines = w->multiline ? (float)std::max(3, vgui_text_lines(w->edit_text)) : 1.0f;
		w->intrinsic_h = w->req_h > 0 ? w->req_h : (lh * lines + pt + pb);
	}
	break;
	case VGUI_SEPARATOR:
		w->intrinsic_w = w->req_w;
		w->intrinsic_h = w->req_h > 0 ? w->req_h : 1;
		break;
	}

	if (w->min_w > 0) w->intrinsic_w = std::max(w->intrinsic_w, w->min_w);
	if (w->min_h > 0) w->intrinsic_h = std::max(w->intrinsic_h, w->min_h);
	if (w->max_w > 0) w->intrinsic_w = std::min(w->intrinsic_w, w->max_w);
	if (w->max_h > 0) w->intrinsic_h = std::min(w->intrinsic_h, w->max_h);
}

static void vgui_layout_widget(vgui_widget* w, vgui_context* ctx, float x, float y, float forced_w, float forced_h);

struct vgui_placement
{
	vgui_widget* child;
	float x, y, w, h;
};

static void vgui_arrange_children(vgui_widget* w, vgui_context* ctx, float ix, float iy, float iw, float ih)
{
	std::vector<vgui_placement> placements;
	placements.reserve(w->children.size());

	float content_w = 0, content_h = 0;

	if (w->layout == VGUI_LAYOUT_ABSOLUTE)
	{
		for (vgui_widget* c : w->children)
		{
			if (!c->visible) continue;
			float cw = vgui_child_req_w(c);
			float ch = vgui_child_req_h(c);
			float px = ix + c->x + c->margin[0];
			float py = iy + c->y + c->margin[1];
			placements.push_back({ c, px, py, cw, ch });
			content_w = std::max(content_w, c->x + c->margin[0] + cw + c->margin[2]);
			content_h = std::max(content_h, c->y + c->margin[1] + ch + c->margin[3]);
		}
	}
	else
	{
		std::vector<vgui_widget*> visible;
		for (vgui_widget* c : w->children)
			if (c->visible) visible.push_back(c);
		int n = (int)visible.size();
		bool row = (w->layout == VGUI_LAYOUT_ROW);
		float inner_main = row ? iw : ih;
		float inner_cross = row ? ih : iw;

		float total_main = 0, max_cross = 0, grow_sum = 0;
		for (vgui_widget* c : visible)
		{
			float cw = vgui_child_req_w(c);
			float ch = vgui_child_req_h(c);
			float main = row ? cw : ch;
			float cross = row ? ch : cw;
			total_main += main + (row ? c->margin[0] + c->margin[2] : c->margin[1] + c->margin[3]);
			max_cross = std::max(max_cross, cross + (row ? c->margin[1] + c->margin[3] : c->margin[0] + c->margin[2]));
			if (c->grow > 0) grow_sum += c->grow;
		}
		if (n > 1) total_main += w->gap * (n - 1);

		float leftover = inner_main - total_main;
		float start = 0, extra_gap = 0;
		if (grow_sum <= 0 && n > 0)
		{
			switch (w->justify)
			{
			case VGUI_JUSTIFY_CENTER: start = leftover * 0.5f; break;
			case VGUI_JUSTIFY_END: start = leftover; break;
			case VGUI_JUSTIFY_SPACE_BETWEEN:
				if (n > 1) extra_gap = leftover / (n - 1);
				break;
			case VGUI_JUSTIFY_SPACE_AROUND:
				if (n > 0)
				{
					start = leftover / (n * 2.0f);
					extra_gap = leftover / n;
				}
				break;
			default: break;
			}
		}

		float cursor = start;
		for (vgui_widget* c : visible)
		{
			float cw = vgui_child_req_w(c);
			float ch = vgui_child_req_h(c);
			float main = row ? cw : ch;
			if (grow_sum > 0 && leftover > 0)
				main += leftover * (c->grow / grow_sum);

			vgui_align al = c->align_self != VGUI_ALIGN_AUTO ? c->align_self : w->align_items;
			float cross_avail = inner_cross - (row ? c->margin[1] + c->margin[3] : c->margin[0] + c->margin[2]);
			float cross = row ? ch : cw;
			float cross_pos = 0;
			if (al == VGUI_ALIGN_STRETCH && (row ? c->req_h <= 0 : c->req_w <= 0))
				cross = cross_avail;
			if (al == VGUI_ALIGN_CENTER) cross_pos = (cross_avail - cross) * 0.5f;
			else if (al == VGUI_ALIGN_END) cross_pos = cross_avail - cross;

			float px, py, pw, ph;
			if (row)
			{
				px = ix + cursor + c->margin[0];
				py = iy + (row ? c->margin[1] : c->margin[0]) + cross_pos;
				pw = main;
				ph = cross;
				cursor += main + c->margin[0] + c->margin[2] + w->gap + extra_gap;
			}
			else
			{
				px = ix + c->margin[0] + cross_pos;
				py = iy + cursor + c->margin[1];
				pw = cross;
				ph = main;
				cursor += main + c->margin[1] + c->margin[3] + w->gap + extra_gap;
			}
			placements.push_back({ c, px, py, pw, ph });
			content_w = std::max(content_w, px - ix + pw + c->margin[2]);
			content_h = std::max(content_h, py - iy + ph + c->margin[3]);
		}
		// Include the starting offset for centered / end aligned content.
		content_w = std::max(content_w, (row ? start + total_main : max_cross));
		content_h = std::max(content_h, (row ? max_cross : start + total_main));
	}

	w->content_w = std::max(0.0f, content_w);
	w->content_h = std::max(0.0f, content_h);

	float max_sx = std::max(0.0f, w->content_w - iw);
	float max_sy = std::max(0.0f, w->content_h - ih);
	if (w->scrollable_x) w->scroll_x = vgui_clampf(w->scroll_x, 0, max_sx);
	else w->scroll_x = 0;
	if (w->scrollable_y) w->scroll_y = vgui_clampf(w->scroll_y, 0, max_sy);
	else w->scroll_y = 0;

	for (const auto& p : placements)
	{
		vgui_layout_widget(p.child, ctx, p.x - w->scroll_x, p.y - w->scroll_y, p.w, p.h);
	}
}

static void vgui_layout_widget(vgui_widget* w, vgui_context* ctx, float x, float y, float forced_w, float forced_h)
{
	if (!w || !w->visible) return;
	float ww = forced_w;
	if (ww <= 0) ww = vgui_child_req_w(w);
	float hh = forced_h;
	if (hh <= 0) hh = vgui_child_req_h(w);
	if (w->min_w > 0) ww = std::max(ww, w->min_w);
	if (w->min_h > 0) hh = std::max(hh, w->min_h);
	if (w->max_w > 0) ww = std::min(ww, w->max_w);
	if (w->max_h > 0) hh = std::min(hh, w->max_h);
	ww = std::max(ww, 0.0f);
	hh = std::max(hh, 0.0f);

	w->abs_x = x;
	w->abs_y = y;
	w->abs_w = ww;
	w->abs_h = hh;

	if (w->kind != VGUI_PANEL) return;

	float ix = x + w->padding[0];
	float iy = y + w->padding[1];
	float iw = std::max(0.0f, ww - w->padding[0] - w->padding[2]);
	float ih = std::max(0.0f, hh - w->padding[1] - w->padding[3]);
	vgui_arrange_children(w, ctx, ix, iy, iw, ih);
}

static void vgui_ensure_layout(vgui_context* ctx)
{
	if (!ctx || !ctx->root) return;
	if (!ctx->layout_dirty) return;
	ctx->layout_dirty = false;
	vgui_measure_widget(ctx->root, ctx);
	float rw = ctx->width > 0 ? (float)ctx->width : std::max(ctx->root->intrinsic_w, 1.0f);
	float rh = ctx->height > 0 ? (float)ctx->height : std::max(ctx->root->intrinsic_h, 1.0f);
	vgui_layout_widget(ctx->root, ctx, 0, 0, rw, rh);
}

static void vgui_delete_rec(vgui_widget* w, vgui_context* ctx)
{
	if (!w) return;
	for (vgui_widget* c : w->children)
		vgui_delete_rec(c, ctx);
	ctx->alive.erase(w);
	ctx->all_widgets.erase(w);
	delete w;
}

static void vgui_flush_pending(vgui_context* ctx)
{
	if (!ctx || ctx->pending_delete.empty()) return;
	std::vector<vgui_widget*> list;
	list.swap(ctx->pending_delete);
	for (vgui_widget* w : list)
	{
		if (!w || !ctx->alive.count(w)) continue;
		if (w->parent)
		{
			auto& v = w->parent->children;
			v.erase(std::remove(v.begin(), v.end(), w), v.end());
		}
		if (ctx->root == w) ctx->root = nullptr;
		if (ctx->hot == w) ctx->hot = nullptr;
		if (ctx->active == w) ctx->active = nullptr;
		if (ctx->press_target == w) ctx->press_target = nullptr;
		if (ctx->focus == w) ctx->focus = nullptr;
		if (ctx->slider_drag == w) ctx->slider_drag = nullptr;
		vgui_delete_rec(w, ctx);
	}
	ctx->layout_dirty = true;
}

static void vgui_schedule_destroy(vgui_widget* w)
{
	if (!w || !w->ctx || w->destroy_pending) return;
	vgui_context* ctx = w->ctx;
	for (vgui_widget* p = w->parent; p; p = p->parent)
		if (p->destroy_pending) return; // parent will delete the subtree
	w->destroy_pending = true;
	w->visible = 0;
	if (ctx->hot == w) ctx->hot = nullptr;
	if (ctx->active == w) ctx->active = nullptr;
	if (ctx->press_target == w) ctx->press_target = nullptr;
	if (ctx->focus == w) ctx->focus = nullptr;
	if (ctx->slider_drag == w) ctx->slider_drag = nullptr;
	ctx->pending_delete.push_back(w);
	ctx->layout_dirty = true;
}

static vgui_widget* vgui_hit_rec(vgui_widget* w, float x, float y)
{
	if (!w || !w->visible || !w->enabled || w->destroy_pending) return nullptr;
	if (!vgui_point_in(w, x, y)) return nullptr;
	for (auto it = w->children.rbegin(); it != w->children.rend(); ++it)
	{
		vgui_widget* r = vgui_hit_rec(*it, x, y);
		if (r) return r;
	}
	return w;
}

static vgui_widget* vgui_hit_test_ctx(vgui_context* ctx, float x, float y)
{
	if (!ctx || !ctx->root) return nullptr;
	vgui_ensure_layout(ctx);
	return vgui_hit_rec(ctx->root, x, y);
}

static void vgui_set_focus_internal(vgui_context* ctx, vgui_widget* w)
{
	if (!ctx) return;
	if (ctx->focus == w) return;
	if (ctx->focus && ctx->focus->ctx == ctx)
	{
		ctx->focus->focused = 0;
		ctx->focus->caret_blink = 0;
		ctx->focus->edit_drag = 0;
	}
	ctx->focus = w;
	if (w)
	{
		w->focused = 1;
		w->caret_blink = 0;
	}
	ctx->want_key = (w != nullptr);
	ctx->want_text = (w && w->kind == VGUI_EDIT && !w->readonly);
}

//----------------------------------------------------------------------------
// text edit helpers
//----------------------------------------------------------------------------

static int vgui_edit_clamp_index(const std::string& s, int i)
{
	if (i < 0) return 0;
	if (i > (int)s.size()) return (int)s.size();
	while (i > 0 && i < (int)s.size() && vgui_is_cont((unsigned char)s[(size_t)i]))
		i--;
	return i;
}

static void vgui_edit_clamp(vgui_widget* w)
{
	w->edit_cursor = vgui_edit_clamp_index(w->edit_text, w->edit_cursor);
	w->edit_anchor = vgui_edit_clamp_index(w->edit_text, w->edit_anchor);
	if (w->edit_anchor > (int)w->edit_text.size()) w->edit_anchor = (int)w->edit_text.size();
}

static bool vgui_edit_has_sel(vgui_widget* w)
{
	return w && w->edit_cursor != w->edit_anchor;
}

static void vgui_edit_line_col(const std::string& s, int index, int& line, int& col)
{
	line = 0;
	col = 0;
	index = vgui_edit_clamp_index(s, index);
	for (int i = 0; i < index;)
	{
		if (s[(size_t)i] == '\n')
		{
			line++;
			col = 0;
			i++;
		}
		else
		{
			col++;
			i = vgui_u8_next(s, i);
		}
	}
}

static int vgui_edit_byte_from_line_col(const std::string& s, int line, int col)
{
	int cur_line = 0;
	int i = 0;
	while (i < (int)s.size() && cur_line < line)
	{
		if (s[(size_t)i] == '\n') cur_line++;
		i = vgui_u8_next(s, i);
	}
	if (cur_line < line) return (int)s.size();
	int n = 0;
	while (i < (int)s.size() && s[(size_t)i] != '\n' && n < col)
	{
		i = vgui_u8_next(s, i);
		n++;
	}
	return i;
}

// Byte range [begin, end) of a line (line 0 based, '\n' separated).
static void vgui_edit_line_bytes(const std::string& s, int line, int& begin, int& end)
{
	int i = 0, cur = 0;
	while (i < (int)s.size() && cur < line)
	{
		if (s[(size_t)i] == '\n') cur++;
		i++;
	}
	if (cur < line)
	{
		begin = end = (int)s.size();
		return;
	}
	begin = i;
	while (i < (int)s.size() && s[(size_t)i] != '\n') i++;
	end = i;
}

// Nearest byte boundary to a local x position inside [begin, end), using the
// real text metrics so clicks and carets agree with the rendered glyphs.
static int vgui_edit_byte_from_x(vgui_widget* w, int begin, int end, float lx)
{
	if (lx <= 0 || !w) return begin;
	const std::string& s = w->edit_text;
	int i = begin;
	float prev = 0;
	while (i < end)
	{
		int j = vgui_u8_next(s, i);
		if (j > end) j = end;
		float cur = prev + vgui_text_width_n(w, s.data() + i, j - i);
		if (lx < (prev + cur) * 0.5f) return i;
		prev = cur;
		i = j;
	}
	return end;
}

static void vgui_edit_ensure_visible(vgui_widget* w)
{
	if (!w) return;
	float lh = vgui_line_height(w->ctx, w);
	float inner_w = std::max(0.0f, w->abs_w - w->padding[0] - w->padding[2]);
	float inner_h = std::max(0.0f, w->abs_h - w->padding[1] - w->padding[3]);
	int line = 0, col = 0;
	vgui_edit_line_col(w->edit_text, w->edit_cursor, line, col);
	(void)col;
	int lb = 0, le = 0;
	vgui_edit_line_bytes(w->edit_text, line, lb, le);
	int upto = std::min(std::max(w->edit_cursor, lb), le);
	float cx = vgui_text_width_n(w, w->edit_text.data() + lb, upto - lb);
	float cy = line * lh;
	if (cx - w->edit_scroll_x > inner_w - 2) w->edit_scroll_x = cx - inner_w + 2;
	if (cx - w->edit_scroll_x < 0) w->edit_scroll_x = cx;
	if (w->multiline)
	{
		if (cy - w->edit_scroll_y > inner_h - lh) w->edit_scroll_y = cy - inner_h + lh;
		if (cy - w->edit_scroll_y < 0) w->edit_scroll_y = cy;
	}
	else
	{
		w->edit_scroll_y = 0;
	}
	// Never keep more scroll than the current text actually needs.  Without
	// this, undoing a large paste leaves the short restored text scrolled
	// completely out of view (empty-looking edit box).
	float line_w = vgui_text_width_n(w, w->edit_text.data() + lb, le - lb);
	float max_sx = std::max(0.0f, line_w - inner_w + 2.0f);
	float max_sy = w->multiline ? std::max(0.0f, (float)vgui_text_lines(w->edit_text) * lh - inner_h) : 0.0f;
	if (w->edit_scroll_x > max_sx) w->edit_scroll_x = max_sx;
	if (w->edit_scroll_y > max_sy) w->edit_scroll_y = max_sy;
	if (w->edit_scroll_x < 0) w->edit_scroll_x = 0;
	if (w->edit_scroll_y < 0) w->edit_scroll_y = 0;
}

static void vgui_edit_sync_text(vgui_widget* w)
{
	w->text = w->edit_text;
	vgui_mark_dirty(w);
	vgui_edit_ensure_visible(w);
}

//----------------------------------------------------------------------------
// edit history / clipboard
//----------------------------------------------------------------------------

static void vgui_edit_erase_selection(vgui_widget* w);

static void vgui_edit_history_reset(vgui_widget* w)
{
	if (!w) return;
	w->undo_stack.clear();
	w->redo_stack.clear();
	w->last_edit_push = -10.0;
	w->last_edit_push_kind = 0;
}

// Records the state before a modification.  kind 1 (typing / single character
// edits) merges with the previous typing step so a burst of keystrokes is
// undone in one go; anything else (kind 2) always starts a new undo step.
static void vgui_edit_history_push(vgui_widget* w, int kind)
{
	if (!w) return;
	double now = w->ctx ? w->ctx->time : 0.0;
	if (kind == 1 && w->last_edit_push_kind == 1 && now - w->last_edit_push < 0.8)
	{
		w->last_edit_push = now;
		return;
	}
	vgui_edit_snapshot s;
	s.text = w->edit_text;
	s.cursor = w->edit_cursor;
	s.anchor = w->edit_anchor;
	w->undo_stack.push_back(std::move(s));
	if (w->undo_stack.size() > 256)
		w->undo_stack.erase(w->undo_stack.begin());
	w->redo_stack.clear();
	w->last_edit_push = now;
	w->last_edit_push_kind = kind;
}

static void vgui_edit_apply_snapshot(vgui_widget* w, const vgui_edit_snapshot& s)
{
	w->edit_text = s.text;
	w->edit_cursor = vgui_edit_clamp_index(w->edit_text, s.cursor);
	w->edit_anchor = vgui_edit_clamp_index(w->edit_text, s.anchor);
	w->edit_drag = 0;
	w->ime_text.clear();
	w->ime_cursor = w->ime_length = 0;
	w->caret_blink = 0;
	vgui_edit_sync_text(w);
	vgui_notify_text(w);
}

static bool vgui_edit_can_undo_impl(const vgui_widget* w)
{
	return w && !w->readonly && !w->undo_stack.empty();
}

static bool vgui_edit_can_redo_impl(const vgui_widget* w)
{
	return w && !w->readonly && !w->redo_stack.empty();
}

static int vgui_edit_copy_impl(vgui_widget* w)
{
	if (!w || !vgui_edit_has_sel(w)) return 0;
	int a = std::min(w->edit_cursor, w->edit_anchor);
	int b = std::max(w->edit_cursor, w->edit_anchor);
	a = vgui_edit_clamp_index(w->edit_text, a);
	b = vgui_edit_clamp_index(w->edit_text, b);
	if (b <= a) return 0;
	set_clipboard(w->edit_text.substr((size_t)a, (size_t)(b - a)).c_str());
	return 1;
}

static int vgui_edit_cut_impl(vgui_widget* w)
{
	if (!w || w->readonly) return 0;
	if (!vgui_edit_copy_impl(w)) return 0;
	vgui_edit_history_push(w, 2);
	vgui_edit_erase_selection(w);
	vgui_edit_sync_text(w);
	vgui_notify_text(w);
	return 1;
}

// Single line editors keep the text on one line: line breaks pasted from the
// clipboard become spaces.  CR is always dropped so Windows clipboard text
// (CRLF) does not leave stray characters in the editor.
static std::string vgui_edit_sanitize_paste(vgui_widget* w, const std::string& s)
{
	std::string out;
	out.reserve(s.size());
	for (char c : s)
	{
		if (c == '\r') continue;
		if (!w->multiline && (c == '\n' || c == '\t'))
		{
			out.push_back(' ');
			continue;
		}
		out.push_back(c);
	}
	return out;
}

static int vgui_edit_paste_impl(vgui_widget* w)
{
	if (!w || w->readonly) return 0;
	std::string s = vgui_edit_sanitize_paste(w, get_clipboard());
	if (s.empty()) return 0;
	vgui_edit_history_push(w, 2);
	vgui_edit_erase_selection(w);
	int at = vgui_edit_clamp_index(w->edit_text, w->edit_cursor);
	w->edit_text.insert((size_t)at, s);
	w->edit_cursor = w->edit_anchor = at + (int)s.size();
	vgui_edit_sync_text(w);
	vgui_notify_text(w);
	return 1;
}

static int vgui_edit_undo_impl(vgui_widget* w)
{
	if (!vgui_edit_can_undo_impl(w)) return 0;
	vgui_edit_snapshot cur;
	cur.text = w->edit_text;
	cur.cursor = w->edit_cursor;
	cur.anchor = w->edit_anchor;

	vgui_edit_snapshot s = w->undo_stack.back();
	w->undo_stack.pop_back();
	w->redo_stack.push_back(std::move(cur));
	if (w->redo_stack.size() > 256)
		w->redo_stack.erase(w->redo_stack.begin());
	w->last_edit_push = -10.0;
	w->last_edit_push_kind = 0;
	vgui_edit_apply_snapshot(w, s);
	return 1;
}

static int vgui_edit_redo_impl(vgui_widget* w)
{
	if (!vgui_edit_can_redo_impl(w)) return 0;
	vgui_edit_snapshot cur;
	cur.text = w->edit_text;
	cur.cursor = w->edit_cursor;
	cur.anchor = w->edit_anchor;

	vgui_edit_snapshot s = w->redo_stack.back();
	w->redo_stack.pop_back();
	w->undo_stack.push_back(std::move(cur));
	if (w->undo_stack.size() > 256)
		w->undo_stack.erase(w->undo_stack.begin());
	w->last_edit_push = -10.0;
	w->last_edit_push_kind = 0;
	vgui_edit_apply_snapshot(w, s);
	return 1;
}

static void vgui_edit_erase_selection(vgui_widget* w)
{
	if (!vgui_edit_has_sel(w)) return;
	int a = std::min(w->edit_cursor, w->edit_anchor);
	int b = std::max(w->edit_cursor, w->edit_anchor);
	a = vgui_edit_clamp_index(w->edit_text, a);
	b = vgui_edit_clamp_index(w->edit_text, b);
	if (b > a)
		w->edit_text.erase((size_t)a, (size_t)(b - a));
	w->edit_cursor = w->edit_anchor = a;
}

static void vgui_edit_insert(vgui_widget* w, const char* text)
{
	if (!w || !text || w->readonly) return;
	vgui_edit_history_push(w, 1);
	vgui_edit_erase_selection(w);
	w->ime_text.clear();
	w->ime_cursor = w->ime_length = 0;
	std::string ins(text);
	int at = vgui_edit_clamp_index(w->edit_text, w->edit_cursor);
	w->edit_text.insert((size_t)at, ins);
	w->edit_cursor = w->edit_anchor = at + (int)ins.size();
	vgui_edit_sync_text(w);
	vgui_notify_text(w);
}

static int vgui_edit_word_prev(const std::string& s, int i)
{
	i = vgui_edit_clamp_index(s, i);
	while (i > 0)
	{
		int p = vgui_u8_prev(s, i);
		unsigned char c = (unsigned char)s[(size_t)p];
		if (!(c == ' ' || c == '\t' || c == '\n')) break;
		i = p;
	}
	while (i > 0)
	{
		int p = vgui_u8_prev(s, i);
		unsigned char c = (unsigned char)s[(size_t)p];
		if (c == ' ' || c == '\t' || c == '\n') break;
		i = p;
	}
	return i;
}

static int vgui_edit_word_next(const std::string& s, int i)
{
	i = vgui_edit_clamp_index(s, i);
	while (i < (int)s.size())
	{
		unsigned char c = (unsigned char)s[(size_t)i];
		if (c == ' ' || c == '\t' || c == '\n') break;
		i = vgui_u8_next(s, i);
	}
	while (i < (int)s.size())
	{
		unsigned char c = (unsigned char)s[(size_t)i];
		if (!(c == ' ' || c == '\t' || c == '\n')) break;
		i = vgui_u8_next(s, i);
	}
	return i;
}

static void vgui_edit_move(vgui_widget* w, int new_cursor, bool select)
{
	w->edit_cursor = vgui_edit_clamp_index(w->edit_text, new_cursor);
	if (!select) w->edit_anchor = w->edit_cursor;
	w->caret_blink = 0;
	vgui_edit_ensure_visible(w);
}

//----------------------------------------------------------------------------
// mouse selection helpers (drag / double click / triple click)
//----------------------------------------------------------------------------

static bool vgui_edit_is_space(unsigned char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

// CJK characters are treated as words of their own so double clicking Chinese
// text selects a single character instead of the whole sentence.
static bool vgui_edit_is_cjk(unsigned int cp)
{
	return (cp >= 0x2E80 && cp <= 0xA4CF) || (cp >= 0xAC00 && cp <= 0xD7AF) ||
		(cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFE30 && cp <= 0xFE4F) ||
		(cp >= 0xFF00 && cp <= 0xFFEF);
}

static bool vgui_edit_is_word_byte(unsigned char c)
{
	return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' ||
		c >= 0x80;	// 非 ASCII 由调用者另行判断
}

// Line under a local y coordinate (content space, already unscrolled by the
// caller through edit_scroll_y).
static int vgui_edit_line_at_y(vgui_widget* w, float y)
{
	float lh = std::max(1.0f, vgui_line_height(w->ctx, w));
	float ly = y - w->abs_y - w->padding[1] + w->edit_scroll_y;
	int line = w->multiline ? (int)std::floor(ly / lh) : 0;
	int max_line = std::max(0, vgui_text_lines(w->edit_text) - 1);
	if (line < 0) line = 0;
	if (line > max_line) line = max_line;
	return line;
}

// Byte index under a local (widget space) position.
static int vgui_edit_index_at(vgui_widget* w, float x, float y)
{
	float lx = x - w->abs_x - w->padding[0] + w->edit_scroll_x;
	int line = vgui_edit_line_at_y(w, y);
	int lb = 0, le = 0;
	vgui_edit_line_bytes(w->edit_text, line, lb, le);
	return vgui_edit_byte_from_x(w, lb, le, lx);
}

// Word (or single CJK character / whitespace run) around a byte index.
static void vgui_edit_word_range(vgui_widget* w, int idx, int& a, int& b)
{
	const std::string& s = w->edit_text;
	int size = (int)s.size();
	idx = vgui_edit_clamp_index(s, idx);
	a = b = idx;

	int ch_begin = idx;
	int ch_end = idx;
	if (ch_begin >= size)
	{
		if (ch_begin > 0) ch_begin = vgui_u8_prev(s, ch_begin);
		ch_end = size;
	}
	else
	{
		ch_end = vgui_u8_next(s, ch_begin);
	}
	if (ch_begin >= size)
	{
		a = b = size;
		return;
	}

	unsigned int cp = vgui_u8_decode(s.data() + ch_begin);
	unsigned char c = (unsigned char)s[(size_t)ch_begin];
	if (vgui_edit_is_cjk(cp))
	{
		a = ch_begin;
		b = ch_end;
		return;
	}
	if (!vgui_edit_is_word_byte(c))
	{
		// 空白或标点：连续同类字符算一段
		a = ch_begin;
		b = ch_end;
		while (a > 0)
		{
			int p = vgui_u8_prev(s, a);
			unsigned char pc = (unsigned char)s[(size_t)p];
			if (vgui_edit_is_cjk(vgui_u8_decode(s.data() + p))) break;
			if (vgui_edit_is_space(pc) != vgui_edit_is_space(c)) break;
			if (vgui_edit_is_word_byte(pc)) break;
			a = p;
		}
		while (b < size)
		{
			unsigned char nc = (unsigned char)s[(size_t)b];
			if (vgui_edit_is_cjk(vgui_u8_decode(s.data() + b))) break;
			if (vgui_edit_is_space(nc) != vgui_edit_is_space(c)) break;
			if (vgui_edit_is_word_byte(nc)) break;
			b = vgui_u8_next(s, b);
		}
		return;
	}

	a = ch_begin;
	b = ch_end;
	while (a > 0)
	{
		int p = vgui_u8_prev(s, a);
		unsigned char pc = (unsigned char)s[(size_t)p];
		if (vgui_edit_is_cjk(vgui_u8_decode(s.data() + p))) break;
		if (!vgui_edit_is_word_byte(pc)) break;
		a = p;
	}
	while (b < size)
	{
		unsigned char nc = (unsigned char)s[(size_t)b];
		if (vgui_edit_is_cjk(vgui_u8_decode(s.data() + b))) break;
		if (!vgui_edit_is_word_byte(nc)) break;
		b = vgui_u8_next(s, b);
	}
}

static void vgui_edit_select_word(vgui_widget* w, int idx)
{
	int a = 0, b = 0;
	vgui_edit_word_range(w, idx, a, b);
	w->edit_anchor = a;
	w->edit_cursor = b;
	w->caret_blink = 0;
}

static void vgui_edit_select_line(vgui_widget* w, int line)
{
	int lb = 0, le = 0;
	vgui_edit_line_bytes(w->edit_text, line, lb, le);
	w->edit_anchor = lb;
	w->edit_cursor = (le < (int)w->edit_text.size() && w->edit_text[(size_t)le] == '\n') ? le + 1 : le;
	w->caret_blink = 0;
}

// Extends the selection while the left button is held down.  The drag mode is
// decided by the click count that started the drag.
static void vgui_edit_drag_to(vgui_widget* w, float x, float y)
{
	int idx = vgui_edit_index_at(w, x, y);
	int anchor = vgui_edit_clamp_index(w->edit_text, w->edit_anchor);
	int cur = idx;
	if (w->edit_drag == 2)
	{
		int a = 0, b = 0;
		vgui_edit_word_range(w, idx, a, b);
		cur = (idx >= anchor) ? b : a;
	}
	else if (w->edit_drag == 3)
	{
		int line = vgui_edit_line_at_y(w, y);
		int lb = 0, le = 0;
		vgui_edit_line_bytes(w->edit_text, line, lb, le);
		cur = (idx >= anchor) ? le : lb;
	}
	w->edit_cursor = vgui_edit_clamp_index(w->edit_text, cur);
	w->caret_blink = 0;
	vgui_edit_ensure_visible(w);
}

static void vgui_activate_widget(vgui_widget* w, int clicks);

static bool vgui_edit_key(vgui_widget* w, int key, int down, int mods)
{
	if (!w || !down) return true;
	bool shift = (mods & VGUI_MOD_SHIFT) != 0;
	bool ctrl = (mods & VGUI_MOD_CTRL) != 0;
	int size = (int)w->edit_text.size();
	int line = 0, col = 0;
	vgui_edit_line_col(w->edit_text, w->edit_cursor, line, col);

	if (ctrl && key == 'A')
	{
		w->edit_anchor = 0;
		w->edit_cursor = size;
		w->caret_blink = 0;
		w->edit_drag = 0;
		return true;
	}
	// Windows 习惯：Ctrl+C/X/V 与 Ctrl+Insert / Shift+Delete / Shift+Insert 等价
	bool copy_key = (ctrl && key == 'C') || (ctrl && key == VGUI_KEY_INSERT);
	bool cut_key = (ctrl && key == 'X') || (shift && key == VGUI_KEY_DELETE);
	bool paste_key = (ctrl && key == 'V') || (shift && key == VGUI_KEY_INSERT);
	if (copy_key || cut_key || paste_key)
	{
		if (paste_key)
			vgui_edit_paste_impl(w);
		else if (cut_key)
			vgui_edit_cut_impl(w);
		else
			vgui_edit_copy_impl(w);
		return true;
	}
	if (ctrl && key == 'Z')
	{
		if (shift) vgui_edit_redo_impl(w);
		else vgui_edit_undo_impl(w);
		return true;
	}
	if (ctrl && key == 'Y')
	{
		vgui_edit_redo_impl(w);
		return true;
	}

	switch (key)
	{
	case VGUI_KEY_BACKSPACE:
		if (!w->readonly)
		{
			if (vgui_edit_has_sel(w))
			{
				vgui_edit_history_push(w, 2);
				vgui_edit_erase_selection(w);
			}
			else if (w->edit_cursor > 0)
			{
				vgui_edit_history_push(w, ctrl ? 2 : 1);
				int p = ctrl ? vgui_edit_word_prev(w->edit_text, w->edit_cursor) : vgui_u8_prev(w->edit_text, w->edit_cursor);
				w->edit_text.erase((size_t)p, (size_t)(w->edit_cursor - p));
				w->edit_cursor = w->edit_anchor = p;
			}
			vgui_edit_sync_text(w);
			vgui_notify_text(w);
		}
		return true;
	case VGUI_KEY_DELETE:
		if (!w->readonly)
		{
			if (vgui_edit_has_sel(w))
			{
				vgui_edit_history_push(w, 2);
				vgui_edit_erase_selection(w);
			}
			else if (w->edit_cursor < size)
			{
				vgui_edit_history_push(w, ctrl ? 2 : 1);
				int n = ctrl ? vgui_edit_word_next(w->edit_text, w->edit_cursor) : vgui_u8_next(w->edit_text, w->edit_cursor);
				w->edit_text.erase((size_t)w->edit_cursor, (size_t)(n - w->edit_cursor));
			}
			vgui_edit_sync_text(w);
			vgui_notify_text(w);
		}
		return true;
	case VGUI_KEY_LEFT:
		vgui_edit_move(w, ctrl ? vgui_edit_word_prev(w->edit_text, w->edit_cursor) : vgui_u8_prev(w->edit_text, w->edit_cursor), shift);
		return true;
	case VGUI_KEY_RIGHT:
		vgui_edit_move(w, ctrl ? vgui_edit_word_next(w->edit_text, w->edit_cursor) : vgui_u8_next(w->edit_text, w->edit_cursor), shift);
		return true;
	case VGUI_KEY_HOME:
		vgui_edit_move(w, ctrl ? 0 : vgui_edit_byte_from_line_col(w->edit_text, line, 0), shift);
		return true;
	case VGUI_KEY_END:
		if (ctrl)
		{
			vgui_edit_move(w, size, shift);
		}
		else
		{
			int e = vgui_edit_byte_from_line_col(w->edit_text, line, 1 << 30);
			while (e < size && w->edit_text[(size_t)e] != '\n') e++;
			vgui_edit_move(w, e, shift);
		}
		return true;
	case VGUI_KEY_UP:
	case VGUI_KEY_DOWN:
		if (w->multiline)
		{
			int target_line = line + (key == VGUI_KEY_UP ? -1 : 1);
			if (target_line < 0) target_line = 0;
			int max_line = vgui_text_lines(w->edit_text) - 1;
			if (target_line > max_line) target_line = max_line;
			vgui_edit_move(w, vgui_edit_byte_from_line_col(w->edit_text, target_line, col), shift);
		}
		else
		{
			vgui_edit_move(w, key == VGUI_KEY_UP ? 0 : size, shift);
		}
		return true;
	case VGUI_KEY_PAGE_UP:
		vgui_edit_move(w, 0, shift);
		return true;
	case VGUI_KEY_PAGE_DOWN:
		vgui_edit_move(w, size, shift);
		return true;
	case VGUI_KEY_ENTER:
		if (w->multiline && !w->readonly)
		{
			vgui_edit_insert(w, "\n");
		}
		else
		{
			vgui_event e = {};
			e.type = VGUI_EVENT_SUBMIT;
			e.key = VGUI_KEY_ENTER;
			vgui_emit_full(w, e);
		}
		return true;
	case VGUI_KEY_ESCAPE:
		vgui_set_focus_internal(w->ctx, nullptr);
		return true;
	default:
		break;
	}
	return true;
}

//----------------------------------------------------------------------------
// values / activation
//----------------------------------------------------------------------------

static void vgui_radio_select(vgui_widget* w)
{
	if (!w) return;
	if (w->parent)
	{
		for (vgui_widget* s : w->parent->children)
		{
			if (s != w && s->kind == VGUI_RADIO && s->checked)
			{
				s->checked = 0;
				vgui_notify_value(s);
				if (!w->ctx || !w->ctx->alive.count(w)) return;
			}
		}
	}
	bool changed = !w->checked;
	w->checked = 1;
	if (changed) vgui_notify_value(w);
}

static void vgui_set_value_internal(vgui_widget* w, float v, bool notify)
{
	if (!w) return;
	bool changed = false;
	switch (w->kind)
	{
	case VGUI_CHECKBOX:
	case VGUI_SWITCH:
	{
		int c = v >= 0.5f ? 1 : 0;
		changed = (w->checked != c);
		w->checked = c;
	}
	break;
	case VGUI_RADIO:
	{
		if (v >= 0.5f)
		{
			vgui_radio_select(w);
			return;
		}
		changed = (w->checked != 0);
		w->checked = 0;
	}
	break;
	case VGUI_SLIDER:
	case VGUI_PROGRESS:
	{
		float nv = vgui_clampf(v, 0.0f, 1.0f);
		float span = w->range_max - w->range_min;
		float nstep = span > 0 ? w->step / span : 0.0f;
		if (nstep > 0)
			nv = vgui_clampf(std::round(nv / nstep) * nstep, 0.0f, 1.0f);
		changed = (std::fabs(w->value - nv) > 1e-6f);
		w->value = nv;
	}
	break;
	default:
		changed = (std::fabs(w->value - v) > 1e-6f);
		w->value = v;
		break;
	}
	if (notify && changed)
		vgui_notify_value(w);
}

static void vgui_activate_widget(vgui_widget* w, int clicks)
{
	if (!w || !w->enabled) return;
	vgui_context* ctx = w->ctx;
	switch (w->kind)
	{
	case VGUI_CHECKBOX:
	case VGUI_SWITCH:
		if (clicks <= 1)
		{
			w->checked = !w->checked;
			vgui_notify_value(w);
		}
		break;
	case VGUI_RADIO:
		vgui_radio_select(w);
		break;
	default:
		break;
	}
	(void)ctx;
}

static void vgui_update_slider_from_mouse(vgui_widget* w, float lx, float ly)
{
	if (!w) return;
	float inner_w = std::max(1.0f, w->abs_w - w->padding[0] - w->padding[2]);
	float inner_h = std::max(1.0f, w->abs_h - w->padding[1] - w->padding[3]);
	float t;
	if (w->vertical)
		t = 1.0f - (ly - w->padding[1]) / inner_h;
	else
		t = (lx - w->padding[0]) / inner_w;
	vgui_set_value_internal(w, t, true);
}

static void vgui_focus_next_impl(vgui_context* ctx, bool reverse)
{
	if (!ctx || !ctx->root) return;
	std::vector<vgui_widget*> list;
	std::function<void(vgui_widget*)> collect = [&](vgui_widget* w)
	{
		if (!w || !w->visible || !w->enabled) return;
		if (w->focusable) list.push_back(w);
		for (vgui_widget* c : w->children) collect(c);
	};
	collect(ctx->root);
	if (list.empty())
	{
		vgui_set_focus_internal(ctx, nullptr);
		return;
	}
	auto it = std::find(list.begin(), list.end(), ctx->focus);
	int index = (it == list.end()) ? (reverse ? (int)list.size() - 1 : 0) : (int)(it - list.begin());
	index += reverse ? -1 : 1;
	if (index < 0) index = (int)list.size() - 1;
	if (index >= (int)list.size()) index = 0;
	vgui_set_focus_internal(ctx, list[(size_t)index]);
}

//----------------------------------------------------------------------------
// input
//----------------------------------------------------------------------------

static void vgui_begin_input(vgui_context* ctx)
{
	vgui_flush_pending(ctx);
	vgui_ensure_layout(ctx);
	ctx->in_input = true;
}

static void vgui_end_input(vgui_context* ctx)
{
	ctx->in_input = false;
	vgui_flush_pending(ctx);
}

//----------------------------------------------------------------------------
// scrollbar dragging (panel scrollbars and the multi-line edit content bar)
//----------------------------------------------------------------------------

struct vgui_scrollbar_info
{
	int axis = 0;			// 1 = panel vertical, 2 = panel horizontal, 3 = edit
	float track = 0;		// vertical: x of the bar, horizontal: y of the bar
	float start = 0;		// vertical: y, horizontal: x
	float span = 0;			// track length
	float thumb = 0;		// thumb start along the track
	float thumb_len = 0;
	float max_scroll = 0;
	float scroll = 0;
};

// Computes the geometry of a visible scrollbar.  The values mirror the drawing
// code so what the user grabs is exactly what is painted.
static bool vgui_scrollbar_geom(vgui_widget* w, int axis, vgui_scrollbar_info* g)
{
	if (!w || !g) return false;
	float ix = w->abs_x + w->padding[0];
	float iy = w->abs_y + w->padding[1];
	float iw = std::max(0.0f, w->abs_w - w->padding[0] - w->padding[2]);
	float ih = std::max(0.0f, w->abs_h - w->padding[1] - w->padding[3]);
	g->axis = axis;
	if (axis == 1)
	{
		if (!w->scrollable_y || w->content_h <= ih + 0.5f || ih <= 20.0f) return false;
		g->track = w->abs_x + w->abs_w - 7.0f;
		g->start = iy;
		g->span = ih;
		g->thumb_len = std::max(24.0f, ih * ih / w->content_h);
		g->max_scroll = w->content_h - ih;
		g->scroll = w->scroll_y;
	}
	else if (axis == 2)
	{
		if (!w->scrollable_x || w->content_w <= iw + 0.5f || iw <= 20.0f) return false;
		g->track = w->abs_y + w->abs_h - 7.0f;
		g->start = ix;
		g->span = iw;
		g->thumb_len = std::max(24.0f, iw * iw / w->content_w);
		g->max_scroll = w->content_w - iw;
		g->scroll = w->scroll_x;
	}
	else if (axis == 3)
	{
		if (!w->multiline || iw <= 1.0f || ih <= 1.0f) return false;
		float lh = std::max(1.0f, vgui_line_height(w->ctx, w));
		float ch = (float)vgui_text_lines(w->edit_text) * lh;
		if (ch <= ih + 0.5f || ih <= 20.0f) return false;
		g->track = w->abs_x + w->abs_w - 6.0f;
		g->start = iy;
		g->span = ih;
		g->thumb_len = std::max(20.0f, ih * ih / ch);
		g->max_scroll = std::max(0.0f, ch - ih);
		g->scroll = w->edit_scroll_y;
	}
	else
	{
		return false;
	}
	float range = g->span - g->thumb_len;
	float t = (g->max_scroll > 0.0f && range > 0.0f)
		? vgui_clampf(g->scroll / g->max_scroll, 0.0f, 1.0f) : 0.0f;
	g->thumb = g->start + t * range;
	return true;
}

static void vgui_scrollbar_apply(vgui_widget* w, const vgui_scrollbar_info& g, float thumb_start)
{
	float range = g.span - g.thumb_len;
	float t = range > 0.0f ? vgui_clampf((thumb_start - g.start) / range, 0.0f, 1.0f) : 0.0f;
	float scroll = t * g.max_scroll;
	if (g.axis == 3)
	{
		w->edit_scroll_y = scroll;
		return;
	}
	if (g.axis == 1) w->scroll_y = scroll;
	else w->scroll_x = scroll;
	if (w->ctx)
	{
		w->ctx->layout_dirty = true;
		vgui_ensure_layout(w->ctx);
	}
}

// Returns the scrollbar under the pointer.  Walking up from the hit widget is
// important: panel bars are painted on top of their children, so a click on
// the bar must not reach the child below it.
static bool vgui_scrollbar_hit(vgui_widget* from, float x, float y, vgui_widget** out, int* axis)
{
	if (!out || !axis) return false;
	vgui_widget* found = nullptr;
	int found_axis = 0;
	for (vgui_widget* w = from; w; w = w->parent)
	{
		if (!w->visible || !w->enabled) continue;
		vgui_scrollbar_info g;
		if (w->kind == VGUI_EDIT && vgui_scrollbar_geom(w, 3, &g) &&
			x >= w->abs_x + w->abs_w - 8.0f && x <= w->abs_x + w->abs_w &&
			y >= g.start && y <= g.start + g.span)
		{
			found = w;
			found_axis = 3;
		}
		if (w->kind == VGUI_PANEL)
		{
			// Keep walking after a hit: ancestor bars are painted on top of
			// descendant bars where the two overlap, and the horizontal bar is
			// painted after the vertical one.
			if (vgui_scrollbar_geom(w, 1, &g) &&
				x >= w->abs_x + w->abs_w - 8.0f && x <= w->abs_x + w->abs_w &&
				y >= g.start && y <= g.start + g.span)
			{
				found = w;
				found_axis = 1;
			}
			if (vgui_scrollbar_geom(w, 2, &g) &&
				y >= w->abs_y + w->abs_h - 8.0f && y <= w->abs_y + w->abs_h &&
				x >= g.start && x <= g.start + g.span)
			{
				found = w;
				found_axis = 2;
			}
		}
	}
	if (!found) return false;
	*out = found;
	*axis = found_axis;
	return true;
}

static void vgui_scrollbar_begin(vgui_context* ctx, vgui_widget* w, int axis, float x, float y)
{
	vgui_scrollbar_info g;
	if (!ctx || !w || !vgui_scrollbar_geom(w, axis, &g)) return;
	float pos = (axis == 2) ? x : y;
	float grab = g.thumb_len * 0.5f;
	if (pos >= g.thumb && pos <= g.thumb + g.thumb_len)
	{
		grab = pos - g.thumb;	// keep the grab point under the pointer
	}
	else
	{
		// Track click: jump so the thumb is centred on the pointer, then keep
		// dragging from there.
		vgui_scrollbar_apply(w, g, pos - grab);
	}
	if (axis == 3)
	{
		w->edit_scroll_drag = 1;
		w->edit_scroll_grab = grab;
		w->edit_drag = 0;
	}
	else
	{
		w->scroll_drag_axis = axis;
		w->scroll_drag_grab = grab;
	}
	ctx->active = w;
	ctx->want_mouse = 1;
}

static void vgui_scrollbar_drag_to(vgui_widget* w, float x, float y)
{
	if (!w) return;
	int axis = (w->kind == VGUI_EDIT) ? 3 : w->scroll_drag_axis;
	vgui_scrollbar_info g;
	if (!vgui_scrollbar_geom(w, axis, &g)) return;
	float pos = (axis == 2) ? x : y;
	float grab = (axis == 3) ? w->edit_scroll_grab : w->scroll_drag_grab;
	vgui_scrollbar_apply(w, g, pos - grab);
}

static int vgui_mouse_move_impl(vgui_context* ctx, float x, float y)
{
	if (!ctx) return 0;
	vgui_begin_input(ctx);
	ctx->mouse_x = x;
	ctx->mouse_y = y;

	vgui_widget* h = vgui_hit_rec(ctx->root, x, y);
	if (ctx->hot != h)
	{
		vgui_widget* old = ctx->hot;
		if (old && ctx->alive.count(old))
		{
			old->hovered = 0;
			vgui_emit(old, VGUI_EVENT_LEAVE, x - old->abs_x, y - old->abs_y);
		}
		ctx->hot = h;
		if (h && ctx->alive.count(h))
		{
			h->hovered = 1;
			vgui_emit(h, VGUI_EVENT_ENTER, x - h->abs_x, y - h->abs_y);
		}
	}
	if (h && ctx->alive.count(h))
		vgui_emit(h, VGUI_EVENT_MOUSE_MOVE, x - h->abs_x, y - h->abs_y);

	if (ctx->active && ctx->alive.count(ctx->active))
	{
		vgui_widget* a = ctx->active;
		if ((a->kind == VGUI_PANEL && a->scroll_drag_axis) ||
			(a->kind == VGUI_EDIT && a->edit_scroll_drag))
		{
			vgui_scrollbar_drag_to(a, x, y);
			if (!ctx->alive.count(a)) { vgui_end_input(ctx); return 1; }
		}
		else if (a->kind == VGUI_SLIDER)
		{
			vgui_update_slider_from_mouse(a, x - a->abs_x, y - a->abs_y);
			if (!ctx->alive.count(a)) { vgui_end_input(ctx); return 1; }
		}
		else if (a->kind == VGUI_EDIT && a->edit_drag && a->pressed_button == 1)
		{
			vgui_edit_drag_to(a, x, y);
			if (!ctx->alive.count(a)) { vgui_end_input(ctx); return 1; }
		}
		if (a != h)
			vgui_emit(a, VGUI_EVENT_MOUSE_MOVE, x - a->abs_x, y - a->abs_y);
		ctx->cursor = a->enabled ? a->cursor_type : VGUI_CURSOR_ARROW;
	}
	else
	{
		ctx->cursor = (h && h->enabled) ? h->cursor_type : VGUI_CURSOR_ARROW;
	}

	ctx->want_mouse = (h != nullptr) || (ctx->active != nullptr);
	int ret = (h != nullptr || ctx->active != nullptr) ? 1 : 0;
	vgui_end_input(ctx);
	return ret;
}

static int vgui_mouse_down_impl(vgui_context* ctx, int button, float x, float y, int clicks)
{
	if (!ctx) return 0;
	vgui_begin_input(ctx);
	ctx->mouse_x = x;
	ctx->mouse_y = y;
	if (button >= 0 && button < 8) ctx->mouse_down[button] = 1;

	// Track the click count here as well so callers that only forward raw
	// mouse events still get click/double-click information.
	double now = SDL_GetTicks() / 1000.0;
	if (button >= 0 && button < 8)
	{
		if (clicks > 0)
		{
			ctx->click_count[button] = clicks;
			ctx->click_time[button] = now;
		}
		else
		{
			if (now - ctx->click_time[button] > 0.45) ctx->click_count[button] = 1;
			else ctx->click_count[button]++;
			ctx->click_time[button] = now;
			clicks = ctx->click_count[button];
		}
	}
	if (clicks <= 0) clicks = 1;

	vgui_widget* h = vgui_hit_rec(ctx->root, x, y);
	ctx->want_mouse = h != nullptr;
	ctx->cursor = (h && h->enabled) ? h->cursor_type : VGUI_CURSOR_ARROW;

	// Scrollbars are painted over the content, so they get the first chance at
	// the click: they neither move focus nor forward the press to the widget
	// underneath.
	if (button == 1)
	{
		vgui_widget* sw = nullptr;
		int axis = 0;
		if (vgui_scrollbar_hit(h, x, y, &sw, &axis))
		{
			vgui_scrollbar_begin(ctx, sw, axis, x, y);
			vgui_end_input(ctx);
			return 1;
		}
	}

	if (h && h->enabled)
	{
		if (h->kind == VGUI_EDIT || h->focusable)
			vgui_set_focus_internal(ctx, h);
		else
			vgui_set_focus_internal(ctx, nullptr);

		h->pressed = 1;
		h->pressed_button = button;
		ctx->active = h;
		ctx->press_target = h;
		ctx->press_button = button;

		if (h->kind == VGUI_SLIDER)
		{
			ctx->slider_drag = h;
			vgui_update_slider_from_mouse(h, x - h->abs_x, y - h->abs_y);
			if (!ctx->alive.count(h)) { vgui_end_input(ctx); return 1; }
		}
		else if (h->kind == VGUI_EDIT)
		{
			int idx = vgui_edit_index_at(h, x, y);
			if (clicks >= 3)
			{
				vgui_edit_select_line(h, vgui_edit_line_at_y(h, y));
				h->edit_drag = 3;
			}
			else if (clicks == 2)
			{
				vgui_edit_select_word(h, idx);
				h->edit_drag = 2;
			}
			else
			{
				h->edit_cursor = h->edit_anchor = idx;
				h->caret_blink = 0;
				h->edit_drag = 1;
			}
			vgui_edit_ensure_visible(h);
		}

		vgui_event e = {};
		e.type = VGUI_EVENT_MOUSE_DOWN;
		e.x = x - h->abs_x;
		e.y = y - h->abs_y;
		e.button = button;
		e.clicks = clicks;
		vgui_emit_full(h, e);
	}

	int ret = (h != nullptr) ? 1 : 0;
	vgui_end_input(ctx);
	return ret;
}

static int vgui_mouse_up_impl(vgui_context* ctx, int button, float x, float y, int clicks)
{
	if (!ctx) return 0;
	vgui_begin_input(ctx);
	ctx->mouse_x = x;
	ctx->mouse_y = y;
	if (button >= 0 && button < 8) ctx->mouse_down[button] = 0;

	vgui_widget* target = ctx->press_target;
	if (target && !ctx->alive.count(target)) target = nullptr;
	if (button >= 0 && button < 8 && clicks <= 0) clicks = ctx->click_count[button];
	if (target && target->ctx != ctx) target = nullptr;

	// A scrollbar drag has no press target, so clear its state here: releasing
	// the button anywhere always ends the drag.
	if (ctx->active && ctx->alive.count(ctx->active))
	{
		ctx->active->scroll_drag_axis = 0;
		ctx->active->edit_scroll_drag = 0;
	}
	ctx->active = nullptr;

	int ret = target ? 1 : 0;
	if (target)
	{
		vgui_widget* h = vgui_hit_rec(ctx->root, x, y);
		bool inside = (h == target || (h && h->parent == target));
		float lx = x - target->abs_x;
		float ly = y - target->abs_y;

		// Clear press state before invoking user code so callbacks can
		// safely inspect the widget.
		target->pressed = 0;
		if (target->kind == VGUI_EDIT) target->edit_drag = 0;
		ctx->press_target = nullptr;
		ctx->slider_drag = nullptr;

		vgui_event up = {};
		up.type = VGUI_EVENT_MOUSE_UP;
		up.x = lx;
		up.y = ly;
		up.button = button;
		up.clicks = clicks;
		vgui_emit_full(target, up);
		if (!ctx->alive.count(target)) { vgui_end_input(ctx); return ret; }

		if (inside && target->enabled)
		{
			vgui_event_type ct = VGUI_EVENT_CLICK;
			if (clicks >= 3) ct = VGUI_EVENT_TRIPLE_CLICK;
			else if (clicks == 2) ct = VGUI_EVENT_DOUBLE_CLICK;

			vgui_event ce = {};
			ce.type = (int)ct;
			ce.x = lx;
			ce.y = ly;
			ce.button = button;
			ce.clicks = clicks;
			vgui_emit_full(target, ce);
			if (!ctx->alive.count(target)) { vgui_end_input(ctx); return ret; }

			vgui_activate_widget(target, clicks);
		}
	}

	ctx->want_mouse = ctx->hot != nullptr;
	vgui_end_input(ctx);
	return ret;
}

static vgui_widget* vgui_find_scroll_panel(vgui_widget* w, bool vertical)
{
	for (vgui_widget* p = w; p; p = p->parent)
	{
		if (p->kind == VGUI_PANEL)
		{
			if (vertical && p->scrollable_y && p->content_h > p->abs_h - p->padding[1] - p->padding[3])
				return p;
			if (!vertical && p->scrollable_x && p->content_w > p->abs_w - p->padding[0] - p->padding[2])
				return p;
		}
	}
	return nullptr;
}

static vgui_widget* vgui_find_edit(vgui_widget* w)
{
	for (vgui_widget* p = w; p; p = p->parent)
		if (p->kind == VGUI_EDIT)
			return p;
	return nullptr;
}

// Wheel over an edit box scrolls its own text instead of the surrounding
// panel.  Multi-line editors scroll vertically, single-line editors scroll
// horizontally when the text is wider than the box.
static bool vgui_edit_wheel(vgui_context* ctx, vgui_widget* e, float wx, float wy)
{
	if (!e || !e->enabled) return false;
	float lh = std::max(1.0f, vgui_line_height(ctx, e));
	float iw = std::max(1.0f, e->abs_w - e->padding[0] - e->padding[2]);
	float ih = std::max(1.0f, e->abs_h - e->padding[1] - e->padding[3]);
	bool scrolled = false;
	if (wy != 0)
	{
		if (e->multiline)
		{
			float ch = (float)vgui_text_lines(e->edit_text) * lh;
			if (ch > ih + 0.5f)
			{
				e->edit_scroll_y = vgui_clampf(e->edit_scroll_y - wy * lh * 3.0f, 0.0f, ch - ih);
				scrolled = true;
			}
		}
		else
		{
			int lb = 0, le = 0;
			vgui_edit_line_bytes(e->edit_text, 0, lb, le);
			float cw = vgui_text_width_n(e, e->edit_text.data() + lb, le - lb);
			if (cw > iw + 0.5f)
			{
				e->edit_scroll_x = vgui_clampf(e->edit_scroll_x - wy * 40.0f, 0.0f, cw - iw + 2.0f);
				scrolled = true;
			}
		}
	}
	if (wx != 0)
	{
		int line = 0, col = 0;
		vgui_edit_line_col(e->edit_text, e->edit_cursor, line, col);
		int lb = 0, le = 0;
		vgui_edit_line_bytes(e->edit_text, line, lb, le);
		float cw = vgui_text_width_n(e, e->edit_text.data() + lb, le - lb);
		float max_scroll = std::max(0.0f, cw - iw + 2.0f);
		if (max_scroll > 0.0f)
		{
			e->edit_scroll_x = vgui_clampf(e->edit_scroll_x - wx * 40.0f, 0.0f, max_scroll);
			scrolled = true;
		}
	}
	return scrolled;
}

static int vgui_mouse_wheel_impl(vgui_context* ctx, float wx, float wy)
{
	if (!ctx) return 0;
	vgui_begin_input(ctx);
	vgui_widget* h = ctx->hot ? ctx->hot : vgui_hit_rec(ctx->root, ctx->mouse_x, ctx->mouse_y);
	int consumed = 0;
	vgui_widget* edit = vgui_find_edit(h);
	if (edit && (wx != 0 || wy != 0))
		consumed = vgui_edit_wheel(ctx, edit, wx, wy) ? 1 : 0;
	if (wy != 0 && !consumed)
	{
		vgui_widget* p = vgui_find_scroll_panel(h, true);
		if (p)
		{
			p->scroll_y -= wy * 40.0f;
			ctx->layout_dirty = true;
			vgui_ensure_layout(ctx);
			consumed = 1;
		}
	}
	if (wx != 0 && !consumed)
	{
		vgui_widget* p = vgui_find_scroll_panel(h, false);
		if (p)
		{
			p->scroll_x -= wx * 40.0f;
			ctx->layout_dirty = true;
			vgui_ensure_layout(ctx);
			consumed = 1;
		}
	}
	if (h && ctx->alive.count(h))
	{
		vgui_event e = {};
		e.type = VGUI_EVENT_MOUSE_WHEEL;
		e.x = ctx->mouse_x - h->abs_x;
		e.y = ctx->mouse_y - h->abs_y;
		e.wheel_x = wx;
		e.wheel_y = wy;
		vgui_emit_full(h, e);
	}
	ctx->want_mouse = consumed || h != nullptr;
	vgui_end_input(ctx);
	return consumed;
}

static int vgui_key_impl(vgui_context* ctx, int key, int down, int mods)
{
	if (!ctx) return 0;
	vgui_begin_input(ctx);
	ctx->want_key = ctx->focus != nullptr;

	if (down && key == VGUI_KEY_TAB)
	{
		vgui_focus_next_impl(ctx, (mods & VGUI_MOD_SHIFT) != 0);
		vgui_end_input(ctx);
		return 1;
	}

	vgui_widget* f = ctx->focus;
	if (!f || !ctx->alive.count(f) || !f->enabled)
	{
		vgui_end_input(ctx);
		return 0;
	}

	int handled = 0;
	if (f->kind == VGUI_EDIT)
	{
		handled = vgui_edit_key(f, key, down, mods) ? 1 : 0;
	}
	else
	{
		if (down && (key == VGUI_KEY_ENTER || key == ' '))
		{
			vgui_activate_widget(f, 1);
			handled = 1;
		}
		if (down && f->kind == VGUI_SLIDER && (key == VGUI_KEY_LEFT || key == VGUI_KEY_RIGHT || key == VGUI_KEY_UP || key == VGUI_KEY_DOWN))
		{
			float span = f->range_max - f->range_min;
			if (span <= 0) span = 1.0f;
			float delta = f->step > 0 ? f->step / span : 0.01f;
			if (key == VGUI_KEY_LEFT || key == VGUI_KEY_DOWN) delta = -delta;
			vgui_set_value_internal(f, f->value + delta, true);
			handled = 1;
		}
	}

	if (ctx->alive.count(f))
	{
		vgui_event e = {};
		e.type = down ? VGUI_EVENT_KEY_DOWN : VGUI_EVENT_KEY_UP;
		e.key = key;
		e.mods = mods;
		e.x = ctx->mouse_x - f->abs_x;
		e.y = ctx->mouse_y - f->abs_y;
		vgui_emit_full(f, e);
	}

	vgui_end_input(ctx);
	return handled;
}

static int vgui_text_impl(vgui_context* ctx, const char* text)
{
	if (!ctx || !text) return 0;
	vgui_begin_input(ctx);
	vgui_widget* f = ctx->focus;
	int ret = 0;
	if (f && ctx->alive.count(f) && f->enabled && f->kind == VGUI_EDIT && !f->readonly)
	{
		vgui_edit_insert(f, text);
		if (ctx->alive.count(f))
		{
			vgui_event e = {};
			e.type = VGUI_EVENT_TEXT;
			e.text = text;
			e.codepoint = vgui_u8_decode(text);
			e.x = ctx->mouse_x - f->abs_x;
			e.y = ctx->mouse_y - f->abs_y;
			vgui_emit_full(f, e);
		}
		ret = 1;
	}
	vgui_end_input(ctx);
	return ret;
}

static int vgui_editing_impl(vgui_context* ctx, const char* text, int cursor, int length)
{
	if (!ctx) return 0;
	vgui_begin_input(ctx);
	vgui_widget* f = ctx->focus;
	int ret = 0;
	if (f && ctx->alive.count(f) && f->enabled && f->kind == VGUI_EDIT && !f->readonly)
	{
		// The ovg/SDL editing event describes the composition string and the
		// selection inside it.  Keep the committed text untouched until the
		// backend sends SDL_EVENT_TEXT_INPUT.
		f->ime_text = text ? text : "";
		f->ime_cursor = vgui_edit_clamp_index(f->ime_text, cursor);
		f->ime_length = std::max(0, length);
		if (f->ime_cursor + f->ime_length > (int)f->ime_text.size())
			f->ime_length = std::max(0, (int)f->ime_text.size() - f->ime_cursor);
		vgui_event e = {};
		e.type = VGUI_EVENT_EDITING;
		e.text = f->ime_text.c_str();
		e.codepoint = (uint32_t)f->ime_cursor;
		e.button = f->ime_length;
		vgui_emit_full(f, e);
		ret = 1;
	}
	vgui_end_input(ctx);
	return ret;
}

//----------------------------------------------------------------------------
// rendering
//----------------------------------------------------------------------------

static uint32_t vgui_widget_color(vgui_widget* w, uint32_t c)
{
	return vgui_alpha_color(c, w->opacity);
}

// The public vgui API packs colors as 0xAARRGGBB, while ovg stores/consumes
// them with the red channel in the low byte (i.e. 0xAABBGGRR for a literal).
// Swap the two outer channels when a color crosses into the renderer.
static uint32_t vgui_ovg_color(uint32_t c)
{
	return (c & 0xFF00FF00u) | ((c & 0x00FF0000u) >> 16) | ((c & 0x000000FFu) << 16);
}

// The ovg state stack does not include the clip rectangle, so clipping has to
// be tracked (and restored) here.  Clip rectangles are stored in the current
// user coordinate space; applying them again after a pop is therefore correct
// even when vgui is drawn under a transform.
struct vgui_clip_state
{
	std::vector<glm::ivec4> stack;
	glm::ivec4 external = {};
	bool has_external = false;
};

static void vgui_clip_push(vgui_clip_state* clips, ovg_ctx_cb* cb, rvg_t* vg,
	float x, float y, float w, float h)
{
	glm::ivec4 rc = {
		(int)std::floor(x), (int)std::floor(y),
		(int)std::ceil(std::max(0.0f, w)), (int)std::ceil(std::max(0.0f, h))
	};
	clips->stack.push_back(rc);
	cb->clip_rect(vg, rc.x, rc.y, rc.z, rc.w);
}

static void vgui_clip_pop(vgui_clip_state* clips, ovg_ctx_cb* cb, rvg_t* vg)
{
	if (clips->stack.empty()) return;
	clips->stack.pop_back();
	if (!clips->stack.empty())
	{
		const glm::ivec4& rc = clips->stack.back();
		cb->clip_rect(vg, rc.x, rc.y, rc.z, rc.w);
		return;
	}

	// A negative size is the renderer's "disable clipping" sentinel, which
	// also restores the scissor rectangle to the full render target.
	if (clips->has_external && cb->set_clip_rect)
		cb->set_clip_rect(vg, &clips->external);
	else
		cb->clip_rect(vg, 0, 0, -1, -1);
}

static void vgui_fill_rect(ovg_ctx_cb* cb, rvg_t* vg, float x, float y, float w, float h, float r, uint32_t color)
{
	if (!cb || !vg || color == 0 || w <= 0 || h <= 0) return;
	if (r > 0)
	{
		r = std::min(r, std::min(w, h) * 0.5f);
		cb->rounded_rectangle(vg, x, y, w, h, r);
	}
	else
	{
		cb->rectangle(vg, x, y, w, h);
	}
	cb->set_source_color(vg, vgui_ovg_color(color));
	cb->fill(vg);
}

static void vgui_stroke_rect(ovg_ctx_cb* cb, rvg_t* vg, float x, float y, float w, float h, float r, uint32_t color, float width)
{
	if (!cb || !vg || color == 0 || w <= 0 || h <= 0 || width <= 0) return;
	if (r > 0)
	{
		r = std::min(r, std::min(w, h) * 0.5f);
		cb->rounded_rectangle(vg, x, y, w, h, r);
	}
	else
	{
		cb->rectangle(vg, x, y, w, h);
	}
	cb->set_line_width(vg, width);
	cb->set_source_color(vg, vgui_ovg_color(color));
	cb->stroke(vg);
}

// One horizontal selection run (one text line).  Multi-line selections are
// filled as the union of these runs: adjacent runs that start or end at the
// same x merge into one straight edge, while every exposed corner of the
// resulting outline is rounded.  This keeps a shared left edge continuous and
// rounds both the outer ends and every step where the line width changes.
struct vgui_sel_band
{
	float x0;
	float x1;
	float y0;
	float y1;
};

struct vgui_sel_vertex
{
	float x;
	float y;
};

static bool vgui_sel_same_point(const vgui_sel_vertex& a, const vgui_sel_vertex& b)
{
	return std::fabs(a.x - b.x) < 0.0005f && std::fabs(a.y - b.y) < 0.0005f;
}

static void vgui_sel_add_vertex(std::vector<vgui_sel_vertex>& pts, float x, float y)
{
	vgui_sel_vertex p = { x, y };
	if (!pts.empty() && vgui_sel_same_point(pts.back(), p)) return;
	pts.push_back(p);
}

// Drop vertices that only continue a straight line; the fillet loop below must
// only see real corners.  Reflex corners are intentionally kept.  The stack
// keeps this linear even when Ctrl+A selects thousands of lines.
static void vgui_sel_remove_collinear(std::vector<vgui_sel_vertex>& pts)
{
	if (pts.size() < 3) return;
	std::vector<vgui_sel_vertex> out;
	out.reserve(pts.size());
	for (const vgui_sel_vertex& p : pts)
	{
		while (out.size() >= 2)
		{
			const vgui_sel_vertex& a = out[out.size() - 2];
			const vgui_sel_vertex& b = out.back();
			float abx = b.x - a.x, aby = b.y - a.y;
			float bcx = p.x - b.x, bcy = p.y - b.y;
			float cross = abx * bcy - aby * bcx;
			float dot = abx * bcx + aby * bcy;
			if (std::fabs(cross) < 0.0005f && dot >= 0.0f)
				out.pop_back();
			else
				break;
		}
		out.push_back(p);
	}
	// The outline is closed, so the last vertex may also be collinear with the
	// first two.  The first vertex itself is always a real corner here.
	while (out.size() >= 3)
	{
		const vgui_sel_vertex& a = out[out.size() - 2];
		const vgui_sel_vertex& b = out.back();
		const vgui_sel_vertex& c = out.front();
		float abx = b.x - a.x, aby = b.y - a.y;
		float bcx = c.x - b.x, bcy = c.y - b.y;
		float cross = abx * bcy - aby * bcx;
		float dot = abx * bcx + aby * bcy;
		if (std::fabs(cross) < 0.0005f && dot >= 0.0f)
			out.pop_back();
		else
			break;
	}
	pts.swap(out);
}

// Trace the outline of the union of the bands in clockwise order.  Bands are
// expected to be consecutive text lines, so y0 of one band equals y1 of the
// previous one; x0/x1 may move freely from line to line.
static void vgui_sel_build_outline(const std::vector<vgui_sel_band>& bands,
	std::vector<vgui_sel_vertex>& pts)
{
	pts.clear();
	size_t n = bands.size();
	if (n == 0) return;

	vgui_sel_add_vertex(pts, bands[0].x0, bands[0].y0);
	vgui_sel_add_vertex(pts, bands[0].x1, bands[0].y0);

	// Right-hand profile, top to bottom.  Use the current band's bottom y for
	// the step as well: the next band starts at that same boundary, and this
	// avoids hairline zigzags from independent floating-point y values.
	for (size_t i = 0; i < n; ++i)
	{
		vgui_sel_add_vertex(pts, bands[i].x1, bands[i].y1);
		if (i + 1 < n)
			vgui_sel_add_vertex(pts, bands[i + 1].x1, bands[i].y1);
	}

	vgui_sel_add_vertex(pts, bands[n - 1].x0, bands[n - 1].y1);

	// Left-hand profile, bottom to top, using the current band's top y as the
	// boundary with the previous band.
	for (size_t i = n; i-- > 0;)
	{
		vgui_sel_add_vertex(pts, bands[i].x0, bands[i].y0);
		if (i > 0)
			vgui_sel_add_vertex(pts, bands[i - 1].x0, bands[i].y0);
	}

	if (pts.size() > 1 && vgui_sel_same_point(pts.front(), pts.back()))
		pts.pop_back();
	vgui_sel_remove_collinear(pts);
}

// Fill the union outline with a quadratic fillet at every corner.  Trimming
// each corner by half of its shortest adjacent edge keeps neighbouring fillets
// from overlapping, including on very short width changes.
static void vgui_fill_selection(ovg_ctx_cb* cb, rvg_t* vg,
	const std::vector<vgui_sel_band>& bands, float radius, uint32_t color)
{
	if (!cb || !vg || color == 0 || bands.empty()) return;

	std::vector<vgui_sel_vertex> pts;
	vgui_sel_build_outline(bands, pts);
	if (pts.size() < 3) return;

	size_t n = pts.size();
	if (radius <= 0.0005f)
	{
		cb->move_to(vg, pts[0].x, pts[0].y);
		for (size_t i = 1; i < n; ++i)
			cb->line_to(vg, pts[i].x, pts[i].y);
		cb->close_path(vg);
		cb->set_source_color(vg, vgui_ovg_color(color));
		cb->fill(vg);
		return;
	}

	std::vector<vgui_sel_vertex> entry(n), exit(n);
	std::vector<float> trims(n, 0.0f);
	for (size_t i = 0; i < n; ++i)
	{
		const vgui_sel_vertex& prev = pts[(i + n - 1) % n];
		const vgui_sel_vertex& cur = pts[i];
		const vgui_sel_vertex& next = pts[(i + 1) % n];
		float inx = cur.x - prev.x, iny = cur.y - prev.y;
		float outx = next.x - cur.x, outy = next.y - cur.y;
		float in_len = std::sqrt(inx * inx + iny * iny);
		float out_len = std::sqrt(outx * outx + outy * outy);
		if (in_len < 0.0001f || out_len < 0.0001f)
		{
			entry[i] = cur;
			exit[i] = cur;
			continue;
		}
		float d = std::min(radius, std::min(in_len, out_len) * 0.5f);
		trims[i] = d;
		entry[i] = { cur.x - inx / in_len * d, cur.y - iny / in_len * d };
		exit[i] = { cur.x + outx / out_len * d, cur.y + outy / out_len * d };
	}

	cb->move_to(vg, entry[0].x, entry[0].y);
	for (size_t i = 0; i < n; ++i)
	{
		if (i > 0) cb->line_to(vg, entry[i].x, entry[i].y);
		if (trims[i] > 0.0001f)
			cb->quadratic_to(vg, pts[i].x, pts[i].y, exit[i].x, exit[i].y);
		else
			cb->line_to(vg, pts[i].x, pts[i].y);
	}
	cb->close_path(vg);
	cb->set_source_color(vg, vgui_ovg_color(color));
	cb->fill(vg);
}

static void vgui_draw_text(vgui_context* ctx, ovg_ctx_cb* cb, rvg_t* vg, vgui_widget* w,
	const std::string& text, float x, float y, float width, float height,
	uint32_t color, float ax = 0.0f, float ay = 0.5f)
{
	if (!ctx || !ctx->font || !cb || !vg || text.empty() || color == 0) return;
	float lh = vgui_line_height(ctx, w);
	float tw = vgui_text_width(w, text);
	float th = lh * (float)vgui_text_lines(text);

	// ovg only honours text_style_t::align when a text_box_rt is supplied, and
	// that box also resets the renderer clip.  Align here instead, then draw at
	// the resolved top-left corner, adding the ascender because text_st_t::pos.y
	// is the baseline of the first line (vg_add_text_run: baseline_y = pos.y).
	float px = x + (width - tw) * ax;
	float py = y + (height - th) * ay + vgui_text_ascent(ctx, w);

	text_style_t st = {};
	st.family = ctx->font;
	st.fontsize = vgui_font_size(ctx, w);
	st.color = vgui_ovg_color(color);
	st.align = { 0.0f, 0.0f };
	st.min_subpixel = 1;

	text_st_t t = {};
	t.pos = { px, py };
	t.size = { tw, th };
	t.text = text.c_str();
	t.text_len = (int)text.size();
	cb->add_text(vg, &t, &st, nullptr);
}

static uint32_t vgui_text_color(vgui_widget* w)
{
	vgui_context* ctx = w->ctx;
	uint32_t c = w->text_color ? w->text_color : ctx->theme.text;
	if (!w->enabled) c = vgui_mix_color(c, ctx->theme.disabled, 0.65f);
	return vgui_widget_color(w, c);
}

// Effective focus opacity: a widget override if set, otherwise the context
// default.  Used by the focus ring and by focus-tinted widget colours.
static float vgui_focus_alpha(const vgui_widget* w)
{
	if (!w) return 1.0f;
	if (w->focus_alpha >= 0.0f) return w->focus_alpha;
	return w->ctx ? w->ctx->focus_alpha : 1.0f;
}

static void vgui_draw_focus(vgui_widget* w, ovg_ctx_cb* cb, rvg_t* vg)
{
	if (!w->focused) return;
	vgui_context* ctx = w->ctx;
	if (w->abs_w < 4.0f || w->abs_h < 4.0f) return;
	float alpha = vgui_focus_alpha(w);
	if (alpha <= 0.001f) return;
	// 焦点环画在控件内部（与边框重合）：父容器的裁剪矩形再也不可能把
	// 焦点环的一部分裁掉，控件被部分滚动出可视区时仍能看到焦点。
	float r = w->rounding > 0 ? w->rounding : 4;
	uint32_t color = vgui_alpha_color(vgui_widget_color(w, ctx->theme.accent), alpha);
	vgui_stroke_rect(cb, vg, w->abs_x + 0.5f, w->abs_y + 0.5f, w->abs_w - 1, w->abs_h - 1, r,
		color, 1);
}

static void vgui_draw_button(vgui_widget* w, ovg_ctx_cb* cb, rvg_t* vg)
{
	vgui_context* ctx = w->ctx;
	uint32_t bg = w->bg ? w->bg : ctx->theme.button_bg;
	uint32_t border = w->border ? w->border : ctx->theme.button_border;
	if (!w->enabled)
	{
		bg = vgui_mix_color(bg, ctx->theme.disabled, 0.35f);
		border = vgui_mix_color(border, ctx->theme.disabled, 0.35f);
	}
	else if (w->pressed)
	{
		bg = vgui_mix_color(bg, ctx->theme.button_active, std::max(0.35f, w->press_anim));
	}
	else if (w->hovered || w->hover_anim > 0.01f)
	{
		bg = vgui_mix_color(bg, ctx->theme.button_hover, std::max(0.25f, w->hover_anim));
	}
	vgui_fill_rect(cb, vg, w->abs_x, w->abs_y, w->abs_w, w->abs_h, w->rounding, vgui_widget_color(w, bg));
	if (w->border_width > 0)
		vgui_stroke_rect(cb, vg, w->abs_x + 0.5f, w->abs_y + 0.5f, w->abs_w - 1, w->abs_h - 1, w->rounding,
			vgui_widget_color(w, border), w->border_width);

	float ty = w->abs_y + w->padding[1] + (w->pressed ? 1.0f : 0.0f);
	float tx = w->abs_x + w->padding[0];
	float tw = std::max(0.0f, w->abs_w - w->padding[0] - w->padding[2]);
	float th = std::max(0.0f, w->abs_h - w->padding[1] - w->padding[3]);
	vgui_draw_text(ctx, cb, vg, w, w->text, tx, ty, tw, th, vgui_text_color(w), w->text_align_x, w->text_align_y);
	vgui_draw_focus(w, cb, vg);
}

static void vgui_draw_label(vgui_widget* w, ovg_ctx_cb* cb, rvg_t* vg)
{
	vgui_context* ctx = w->ctx;
	if (w->bg)
		vgui_fill_rect(cb, vg, w->abs_x, w->abs_y, w->abs_w, w->abs_h, w->rounding, vgui_widget_color(w, w->bg));
	if (w->border_width > 0 && w->border)
		vgui_stroke_rect(cb, vg, w->abs_x + 0.5f, w->abs_y + 0.5f, w->abs_w - 1, w->abs_h - 1, w->rounding,
			vgui_widget_color(w, w->border), w->border_width);
	vgui_draw_text(ctx, cb, vg, w, w->text,
		w->abs_x + w->padding[0], w->abs_y + w->padding[1],
		std::max(0.0f, w->abs_w - w->padding[0] - w->padding[2]),
		std::max(0.0f, w->abs_h - w->padding[1] - w->padding[3]),
		vgui_text_color(w), w->text_align_x, w->text_align_y);
}

static void vgui_draw_checkbox(vgui_widget* w, ovg_ctx_cb* cb, rvg_t* vg)
{
	vgui_context* ctx = w->ctx;
	float lh = vgui_line_height(ctx, w);
	float box = std::max(14.0f, std::min(18.0f, lh - 2));
	float bx = w->abs_x + w->padding[0];
	float by = w->abs_y + (w->abs_h - box) * 0.5f;
	uint32_t border = w->enabled
		? (w->focused ? vgui_mix_color(ctx->theme.button_border, ctx->theme.accent, vgui_focus_alpha(w))
			: ctx->theme.button_border)
		: ctx->theme.disabled;
	uint32_t accent = w->enabled ? (w->accent ? w->accent : ctx->theme.accent) : ctx->theme.disabled;

	vgui_fill_rect(cb, vg, bx, by, box, box, 3, vgui_widget_color(w, ctx->theme.track));
	if (w->check_anim > 0.01f)
	{
		uint32_t c = vgui_mix_color(ctx->theme.track, accent, w->check_anim);
		vgui_fill_rect(cb, vg, bx, by, box, box, 3, vgui_widget_color(w, c));
	}
	vgui_stroke_rect(cb, vg, bx + 0.5f, by + 0.5f, box - 1, box - 1, 3, vgui_widget_color(w, border), 1);

	if (w->check_anim > 0.05f)
	{
		float t = w->check_anim;
		cb->set_line_width(vg, 2.0f);
		cb->set_line_cap(vg, 1);
		cb->set_line_join(vg, 1);
		cb->move_to(vg, bx + box * 0.22f, by + box * 0.52f);
		cb->line_to(vg, bx + box * 0.42f, by + box * 0.72f);
		cb->line_to(vg, bx + box * 0.80f, by + box * 0.28f);
		cb->set_source_color(vg, vgui_ovg_color(vgui_widget_color(w, vgui_alpha_color(0xFFFFFFFF, t))));
		cb->stroke(vg);
	}

	if (!w->text.empty())
	{
		float tx = bx + box + 8;
		vgui_draw_text(ctx, cb, vg, w, w->text, tx, w->abs_y, std::max(0.0f, w->abs_x + w->abs_w - tx),
			w->abs_h, vgui_text_color(w), 0.0f, 0.5f);
	}
	vgui_draw_focus(w, cb, vg);
}

static void vgui_draw_radio(vgui_widget* w, ovg_ctx_cb* cb, rvg_t* vg)
{
	vgui_context* ctx = w->ctx;
	float lh = vgui_line_height(ctx, w);
	float d = std::max(14.0f, std::min(18.0f, lh - 2));
	float cx = w->abs_x + w->padding[0] + d * 0.5f;
	float cy = w->abs_y + w->abs_h * 0.5f;
	uint32_t border = w->enabled
		? (w->focused ? vgui_mix_color(ctx->theme.button_border, ctx->theme.accent, vgui_focus_alpha(w))
			: ctx->theme.button_border)
		: ctx->theme.disabled;
	uint32_t accent = w->enabled ? (w->accent ? w->accent : ctx->theme.accent) : ctx->theme.disabled;

	cb->circle(vg, cx, cy, d * 0.5f);
	cb->set_source_color(vg, vgui_ovg_color(vgui_widget_color(w, ctx->theme.track)));
	cb->fill(vg);
	cb->circle(vg, cx, cy, d * 0.5f);
	cb->set_line_width(vg, 1);
	cb->set_source_color(vg, vgui_ovg_color(vgui_widget_color(w, border)));
	cb->stroke(vg);
	if (w->check_anim > 0.01f)
	{
		cb->circle(vg, cx, cy, d * 0.5f * 0.55f * w->check_anim);
		cb->set_source_color(vg, vgui_ovg_color(vgui_widget_color(w, accent)));
		cb->fill(vg);
	}

	if (!w->text.empty())
	{
		float tx = cx + d * 0.5f + 8;
		vgui_draw_text(ctx, cb, vg, w, w->text, tx, w->abs_y, std::max(0.0f, w->abs_x + w->abs_w - tx),
			w->abs_h, vgui_text_color(w), 0.0f, 0.5f);
	}
	vgui_draw_focus(w, cb, vg);
}

static void vgui_draw_switch(vgui_widget* w, ovg_ctx_cb* cb, rvg_t* vg)
{
	vgui_context* ctx = w->ctx;
	float lh = vgui_line_height(ctx, w);
	float track_w = 36;
	float track_h = std::max(18.0f, std::min(22.0f, lh - 2));
	float x = w->abs_x + w->padding[0];
	float y = w->abs_y + (w->abs_h - track_h) * 0.5f;
	uint32_t on = w->enabled ? (w->accent ? w->accent : ctx->theme.accent) : ctx->theme.disabled;
	uint32_t off = w->enabled ? ctx->theme.track : vgui_mix_color(ctx->theme.track, ctx->theme.disabled, 0.5f);
	float t = w->check_anim;
	uint32_t track = vgui_mix_color(off, on, t);
	vgui_fill_rect(cb, vg, x, y, track_w, track_h, track_h * 0.5f, vgui_widget_color(w, track));

	float radius = track_h * 0.5f - 2;
	float kx = x + radius + 2 + t * (track_w - radius * 2 - 4);
	float ky = y + track_h * 0.5f;
	cb->circle(vg, kx, ky, radius);
	cb->set_source_color(vg, vgui_ovg_color(vgui_widget_color(w, 0xFFFFFFFF)));
	cb->fill(vg);

	if (!w->text.empty())
	{
		float tx = x + track_w + 8;
		vgui_draw_text(ctx, cb, vg, w, w->text, tx, w->abs_y, std::max(0.0f, w->abs_x + w->abs_w - tx),
			w->abs_h, vgui_text_color(w), 0.0f, 0.5f);
	}
	vgui_draw_focus(w, cb, vg);
}

static void vgui_draw_slider(vgui_widget* w, ovg_ctx_cb* cb, rvg_t* vg)
{
	vgui_context* ctx = w->ctx;
	uint32_t accent = w->enabled ? (w->accent ? w->accent : ctx->theme.accent) : ctx->theme.disabled;
	uint32_t track = w->enabled ? (w->track ? w->track : ctx->theme.track) : vgui_mix_color(ctx->theme.track, ctx->theme.disabled, 0.5f);
	uint32_t knob_color = (w->focused && w->enabled)
		? vgui_mix_color(accent, ctx->theme.accent_hover, vgui_focus_alpha(w))
		: accent;
	float t = vgui_clampf(w->value, 0.0f, 1.0f);
	float knob = 7;
	float inner_w = std::max(1.0f, w->abs_w - w->padding[0] - w->padding[2]);
	float inner_h = std::max(1.0f, w->abs_h - w->padding[1] - w->padding[3]);

	if (w->bg)
		vgui_fill_rect(cb, vg, w->abs_x, w->abs_y, w->abs_w, w->abs_h, w->rounding,
			vgui_widget_color(w, vgui_alpha_color(w->bg, 0.25f)));

	if (!w->vertical)
	{
		float x = w->abs_x + w->padding[0];
		float y = w->abs_y + w->abs_h * 0.5f - 2;
		vgui_fill_rect(cb, vg, x, y, inner_w, 4, 2, vgui_widget_color(w, track));
		vgui_fill_rect(cb, vg, x, y, inner_w * t, 4, 2, vgui_widget_color(w, accent));
		float kx = x + inner_w * t;
		cb->circle(vg, kx, y + 2, knob);
		cb->set_source_color(vg, vgui_ovg_color(vgui_widget_color(w, knob_color)));
		cb->fill(vg);
	}
	else
	{
		float x = w->abs_x + w->abs_w * 0.5f - 2;
		float y = w->abs_y + w->padding[1];
		vgui_fill_rect(cb, vg, x, y, 4, inner_h, 2, vgui_widget_color(w, track));
		float fill_h = inner_h * t;
		vgui_fill_rect(cb, vg, x, y + inner_h - fill_h, 4, fill_h, 2, vgui_widget_color(w, accent));
		float ky = y + inner_h - fill_h;
		cb->circle(vg, x + 2, ky, knob);
		cb->set_source_color(vg, vgui_ovg_color(vgui_widget_color(w, knob_color)));
		cb->fill(vg);
	}
	vgui_draw_focus(w, cb, vg);
}

static void vgui_draw_progress(vgui_widget* w, ovg_ctx_cb* cb, rvg_t* vg)
{
	vgui_context* ctx = w->ctx;
	uint32_t accent = w->enabled ? (w->accent ? w->accent : ctx->theme.accent) : ctx->theme.disabled;
	uint32_t track = w->enabled ? (w->track ? w->track : ctx->theme.track) : vgui_mix_color(ctx->theme.track, ctx->theme.disabled, 0.5f);
	float t = vgui_clampf(w->value, 0.0f, 1.0f);
	float inner_w = std::max(0.0f, w->abs_w - w->padding[0] - w->padding[2]);
	float inner_h = std::max(0.0f, w->abs_h - w->padding[1] - w->padding[3]);
	float x = w->abs_x + w->padding[0];
	float y = w->abs_y + w->padding[1];

	if (!w->vertical)
	{
		vgui_fill_rect(cb, vg, x, y, inner_w, inner_h, w->rounding > 0 ? w->rounding : 3, vgui_widget_color(w, track));
		vgui_fill_rect(cb, vg, x, y, inner_w * t, inner_h, w->rounding > 0 ? w->rounding : 3, vgui_widget_color(w, accent));
	}
	else
	{
		vgui_fill_rect(cb, vg, x, y, inner_w, inner_h, w->rounding > 0 ? w->rounding : 3, vgui_widget_color(w, track));
		float fh = inner_h * t;
		vgui_fill_rect(cb, vg, x, y + inner_h - fh, inner_w, fh, w->rounding > 0 ? w->rounding : 3, vgui_widget_color(w, accent));
	}

	std::string label = w->text;
	if (label.empty())
	{
		char buf[32] = {};
		std::snprintf(buf, sizeof(buf), "%d%%", (int)std::lround(t * 100.0f));
		label = buf;
	}
	vgui_draw_text(ctx, cb, vg, w, label, x, y, inner_w, inner_h, vgui_text_color(w), w->text_align_x, w->text_align_y);
	vgui_draw_focus(w, cb, vg);
}

static void vgui_draw_edit(vgui_widget* w, ovg_ctx_cb* cb, rvg_t* vg, vgui_clip_state* clips)
{
	vgui_context* ctx = w->ctx;
	uint32_t bg = w->bg ? w->bg : ctx->theme.edit_bg;
	uint32_t base_border = w->border ? w->border : ctx->theme.button_border;
	uint32_t border = w->enabled
		? (w->focused ? vgui_mix_color(base_border, ctx->theme.accent, vgui_focus_alpha(w)) : base_border)
		: ctx->theme.disabled;
	if (!w->enabled) bg = vgui_mix_color(bg, ctx->theme.disabled, 0.25f);

	vgui_fill_rect(cb, vg, w->abs_x, w->abs_y, w->abs_w, w->abs_h, w->rounding, vgui_widget_color(w, bg));
	if (w->border_width > 0)
		vgui_stroke_rect(cb, vg, w->abs_x + 0.5f, w->abs_y + 0.5f, w->abs_w - 1, w->abs_h - 1, w->rounding,
			vgui_widget_color(w, border), w->border_width);

	float ix = w->abs_x + w->padding[0];
	float iy = w->abs_y + w->padding[1];
	float iw = std::max(0.0f, w->abs_w - w->padding[0] - w->padding[2]);
	float ih = std::max(0.0f, w->abs_h - w->padding[1] - w->padding[3]);
	if (iw <= 1 || ih <= 1) return;

	float lh = vgui_line_height(ctx, w);
	uint32_t color = vgui_text_color(w);

	if (w->edit_text.empty() && w->ime_text.empty() && !w->placeholder.empty() && !w->focused)
	{
		vgui_draw_text(ctx, cb, vg, w, w->placeholder, ix, iy, iw, lh,
			vgui_widget_color(w, ctx->theme.disabled), 0.0f, 0.5f);
		return;
	}

	cb->save(vg);
	vgui_clip_push(clips, cb, vg, ix, iy, iw, ih);

	int sel_a = std::min(w->edit_cursor, w->edit_anchor);
	int sel_b = std::max(w->edit_cursor, w->edit_anchor);
	float tx = ix - w->edit_scroll_x;

	struct vgui_edit_draw_line
	{
		std::string shown;
		int line_start = 0;
		int line_end = 0;
		int caret_byte = 0;
		float ty = 0;
	};
	std::vector<vgui_edit_draw_line> draw_lines;
	std::vector<vgui_sel_band> sel_bands;

	int line_start = 0;
	int line_index = 0;
	while (line_start <= (int)w->edit_text.size())
	{
		int line_end = line_start;
		while (line_end < (int)w->edit_text.size() && w->edit_text[(size_t)line_end] != '\n') line_end++;
		std::string line = w->edit_text.substr((size_t)line_start, (size_t)(line_end - line_start));
		std::string shown = line;
		if (w->password)
		{
			shown.clear();
			for (int i = 0; i < (int)line.size(); i = vgui_u8_next(line, i))
				shown.push_back(w->password_char);
		}
		float ty = iy - w->edit_scroll_y + line_index * lh;

		if (sel_b > sel_a && sel_a < line_end && sel_b > line_start)
		{
			int a = std::max(sel_a, line_start) - line_start;
			int b = std::min(sel_b, line_end) - line_start;
			int sa = vgui_u8_byte_at_col(shown, vgui_u8_col_at_byte(line, a));
			int sb = vgui_u8_byte_at_col(shown, vgui_u8_col_at_byte(line, b));
			float sx = tx + vgui_text_width_n(w, shown.data(), sa);
			float sw = vgui_text_width_n(w, shown.data() + sa, sb - sa);
			// 选中换行符时补一小段尾巴，让跨行选中看起来是连续的
			if (sel_b > line_end) sw += std::min(6.0f, lh * 0.3f);
			if (sw < 1.0f) sw = 1.0f;
			vgui_sel_band band;
			band.x0 = sx;
			band.x1 = sx + sw;
			band.y0 = ty;
			band.y1 = ty + lh;
			sel_bands.push_back(band);
		}

		vgui_edit_draw_line dl;
		dl.caret_byte = vgui_u8_byte_at_col(shown,
			vgui_u8_col_at_byte(line, w->edit_cursor - line_start));
		dl.shown = std::move(shown);
		dl.line_start = line_start;
		dl.line_end = line_end;
		dl.ty = ty;
		draw_lines.push_back(std::move(dl));

		if (line_end >= (int)w->edit_text.size()) break;
		line_start = line_end + 1;
		line_index++;
		if (!w->multiline) break;
	}

	// 先一次性填充整个选区的并集，再画文字，避免逐行重叠导致半透明色块叠深。
	if (!sel_bands.empty())
	{
		uint32_t selc = w->sel_color ? w->sel_color : ctx->theme.edit_selection;
		if (!w->focused) selc = vgui_alpha_color(selc, 0.6f);
		vgui_fill_selection(cb, vg, sel_bands, w->sel_rounding, vgui_widget_color(w, selc));
	}

	for (const vgui_edit_draw_line& dl : draw_lines)
	{
		vgui_draw_text(ctx, cb, vg, w, dl.shown, tx, dl.ty, std::max(iw, 1.0f), lh, color, 0.0f, 0.5f);

		if (w->focused && w->edit_cursor >= dl.line_start && w->edit_cursor <= dl.line_end)
		{
			// Draw the caret only for the line containing the cursor.
			float cx = tx + vgui_text_width_n(w, dl.shown.data(), dl.caret_byte);
			bool blink = (std::fmod(w->caret_blink, 1.0f) < 0.55f);
			if (blink)
				vgui_fill_rect(cb, vg, cx, dl.ty + 2, 1.5f, lh - 4, 0, color);
		}

		if (!w->ime_text.empty() && w->edit_cursor >= dl.line_start && w->edit_cursor <= dl.line_end)
		{
			float cx = tx + vgui_text_width_n(w, dl.shown.data(), dl.caret_byte);
			vgui_draw_text(ctx, cb, vg, w, w->ime_text, cx, dl.ty, iw, lh, ctx->theme.accent, 0.0f, 0.5f);
			int ia = vgui_edit_clamp_index(w->ime_text, w->ime_cursor);
			int ib = vgui_edit_clamp_index(w->ime_text, w->ime_cursor + w->ime_length);
			if (ib < ia) std::swap(ia, ib);
			float ux = cx + vgui_text_width_n(w, w->ime_text.data(), ia);
			float uw = vgui_text_width_n(w, w->ime_text.data() + ia, ib - ia);
			vgui_fill_rect(cb, vg, ux, dl.ty + lh - 1, std::max(2.0f, uw), 1, 0, ctx->theme.accent);
		}
	}

	vgui_clip_pop(clips, cb, vg);
	cb->restore(vg);

	// 多行编辑框的内容滚动条（覆盖在文本右侧，不占用布局宽度）
	if (w->multiline)
	{
		float ch = (float)vgui_text_lines(w->edit_text) * lh;
		float max_scroll = std::max(0.0f, ch - ih);
		if (w->edit_scroll_y > max_scroll) w->edit_scroll_y = max_scroll;
		if (ch > ih + 0.5f && ih > 20.0f)
		{
			float bar_w = 4.0f;
			float bx = w->abs_x + w->abs_w - bar_w - 2.0f;
			float th = std::max(20.0f, ih * ih / ch);
			float by = iy + (max_scroll > 0.0f ? (w->edit_scroll_y / max_scroll) * (ih - th) : 0.0f);
			// Keep the track opaque: compositing it with whatever happens to be
			// behind the widget can make it exactly the same color as the thumb
			// (e.g. the light theme's #DCDCDC track over a black root became
			// #9A9A9A, the same value as its thumb), which makes the whole
			// scrollbar look like one long thumb.
			vgui_fill_rect(cb, vg, bx, iy, bar_w, ih, bar_w * 0.5f,
				vgui_widget_color(w, ctx->theme.track));
			vgui_fill_rect(cb, vg, bx, by, bar_w, th, bar_w * 0.5f,
				vgui_widget_color(w, ctx->theme.scrollbar));
		}
	}

	vgui_draw_focus(w, cb, vg);
}

static void vgui_draw_scrollbars(vgui_widget* w, ovg_ctx_cb* cb, rvg_t* vg)
{
	vgui_context* ctx = w->ctx;
	float ix = w->abs_x + w->padding[0];
	float iy = w->abs_y + w->padding[1];
	float iw = std::max(0.0f, w->abs_w - w->padding[0] - w->padding[2]);
	float ih = std::max(0.0f, w->abs_h - w->padding[1] - w->padding[3]);
	if (w->scrollable_y && w->content_h > ih + 0.5f && ih > 20)
	{
		float track_x = w->abs_x + w->abs_w - 7;
		float th = std::max(24.0f, ih * ih / w->content_h);
		float max_scroll = w->content_h - ih;
		float ty = iy + (max_scroll > 0 ? (w->scroll_y / max_scroll) * (ih - th) : 0);
		vgui_fill_rect(cb, vg, track_x, iy, 5, ih, 2.5f, vgui_widget_color(w, ctx->theme.track));
		vgui_fill_rect(cb, vg, track_x, ty, 5, th, 2.5f, vgui_widget_color(w, ctx->theme.scrollbar));
	}
	if (w->scrollable_x && w->content_w > iw + 0.5f && iw > 20)
	{
		float track_y = w->abs_y + w->abs_h - 7;
		float tw = std::max(24.0f, iw * iw / w->content_w);
		float max_scroll = w->content_w - iw;
		float tx = ix + (max_scroll > 0 ? (w->scroll_x / max_scroll) * (iw - tw) : 0);
		vgui_fill_rect(cb, vg, ix, track_y, iw, 5, 2.5f, vgui_widget_color(w, ctx->theme.track));
		vgui_fill_rect(cb, vg, tx, track_y, tw, 5, 2.5f, vgui_widget_color(w, ctx->theme.scrollbar));
	}
}

static void vgui_draw_widget(vgui_widget* w, ovg_ctx_cb* cb, rvg_t* vg, vgui_clip_state* clips)
{
	if (!w || !w->visible || w->abs_w <= 0 || w->abs_h <= 0) return;
	vgui_context* ctx = w->ctx;

	if (w->kind == VGUI_PANEL)
	{
		if (w->bg)
			vgui_fill_rect(cb, vg, w->abs_x, w->abs_y, w->abs_w, w->abs_h, w->rounding, vgui_widget_color(w, w->bg));
		if (w->border_width > 0 && w->border)
			vgui_stroke_rect(cb, vg, w->abs_x + 0.5f, w->abs_y + 0.5f, w->abs_w - 1, w->abs_h - 1, w->rounding,
				vgui_widget_color(w, w->border), w->border_width);

		cb->save(vg);
		if (w->clip_children)
			vgui_clip_push(clips, cb, vg, w->abs_x, w->abs_y, w->abs_w, w->abs_h);
		for (vgui_widget* c : w->children)
			vgui_draw_widget(c, cb, vg, clips);
		if (w->clip_children)
			vgui_clip_pop(clips, cb, vg);
		cb->restore(vg);
		vgui_draw_scrollbars(w, cb, vg);
		vgui_draw_focus(w, cb, vg);
		return;
	}

	switch (w->kind)
	{
	case VGUI_LABEL: vgui_draw_label(w, cb, vg); break;
	case VGUI_BUTTON: vgui_draw_button(w, cb, vg); break;
	case VGUI_CHECKBOX: vgui_draw_checkbox(w, cb, vg); break;
	case VGUI_RADIO: vgui_draw_radio(w, cb, vg); break;
	case VGUI_SWITCH: vgui_draw_switch(w, cb, vg); break;
	case VGUI_SLIDER: vgui_draw_slider(w, cb, vg); break;
	case VGUI_PROGRESS: vgui_draw_progress(w, cb, vg); break;
	case VGUI_EDIT: vgui_draw_edit(w, cb, vg, clips); break;
	case VGUI_SEPARATOR:
		vgui_fill_rect(cb, vg, w->abs_x, w->abs_y, w->abs_w, std::max(1.0f, w->abs_h), 0,
			vgui_widget_color(w, w->bg ? w->bg : ctx->theme.panel_border));
		break;
	default: break;
	}
	(void)ctx;
}

static void vgui_update_impl(vgui_context* ctx, float dt)
{
	if (!ctx) return;
	if (dt < 0) dt = 0;
	ctx->time += dt;
	vgui_flush_pending(ctx);
	vgui_ensure_layout(ctx);

	float k = 1.0f - std::exp(-std::max(0.0f, dt) * 18.0f);
	for (vgui_widget* w : ctx->all_widgets)
	{
		if (!w || !ctx->alive.count(w) || w->destroy_pending) continue;
		float hover_target = w->hovered ? 1.0f : 0.0f;
		float press_target = w->pressed ? 1.0f : 0.0f;
		w->hover_anim += (hover_target - w->hover_anim) * k;
		w->press_anim += (press_target - w->press_anim) * k;
		if (w->kind == VGUI_CHECKBOX || w->kind == VGUI_RADIO || w->kind == VGUI_SWITCH)
		{
			float ct = w->checked ? 1.0f : 0.0f;
			w->check_anim += (ct - w->check_anim) * k;
		}
		if (w == ctx->focus && w->kind == VGUI_EDIT)
			w->caret_blink += dt;
		else
			w->caret_blink = 0;
	}
}

} // anonymous namespace

//----------------------------------------------------------------------------
// Public C API
//----------------------------------------------------------------------------

extern "C" {

const char* vgui_version(void)
{
	return "0.2.0";
}

//----------------------------------------------------------------------------
// context
//----------------------------------------------------------------------------

vgui_context* vgui_create(struct font_familys_t* font, float font_size)
{
	vgui_context* ctx = new vgui_context();
	ctx->font = font;
	ctx->font_size = (font_size > 0) ? font_size : 16.0f;
	ctx->theme = vgui_default_theme();

	ctx->root = vgui_new_widget(ctx, VGUI_PANEL, nullptr);
	ctx->root->bg = 0;
	ctx->root->border = 0;
	ctx->root->border_width = 0;
	ctx->root->padding[0] = ctx->root->padding[1] = 0;
	ctx->root->padding[2] = ctx->root->padding[3] = 0;
	ctx->root->layout = VGUI_LAYOUT_ABSOLUTE;
	ctx->root->clip_children = 1;
	ctx->layout_dirty = true;
	return ctx;
}

void vgui_destroy(vgui_context* ctx)
{
	if (!ctx) return;

	ctx->pending_delete.clear();
	if (ctx->root && ctx->alive.count(ctx->root))
		vgui_delete_rec(ctx->root, ctx);
	ctx->root = nullptr;

	// Delete widgets that were not attached to the root tree as well.
	for (vgui_widget* w : ctx->all_widgets)
	{
		if (w && ctx->alive.count(w))
		{
			ctx->alive.erase(w);
			delete w;
		}
	}
	ctx->all_widgets.clear();
	ctx->alive.clear();
	delete ctx;
}

void vgui_context_set_size(vgui_context* ctx, int width, int height)
{
	if (!ctx) return;
	ctx->width = std::max(0, width);
	ctx->height = std::max(0, height);
	ctx->layout_dirty = true;
}

void vgui_context_get_size(vgui_context* ctx, int* width, int* height)
{
	if (width) *width = ctx ? ctx->width : 0;
	if (height) *height = ctx ? ctx->height : 0;
}

void vgui_context_set_font(vgui_context* ctx, struct font_familys_t* font, float font_size)
{
	if (!ctx) return;
	ctx->font = font;
	if (font_size > 0) ctx->font_size = font_size;
	ctx->layout_dirty = true;
}

void vgui_context_set_theme(vgui_context* ctx, const vgui_theme* theme)
{
	if (!ctx || !theme) return;
	vgui_theme old = ctx->theme;
	ctx->theme = *theme;

	// Widgets copy their defaults when they are created.  Move the colors that
	// still match the old defaults over to the new theme so a theme switch is
	// visible without rebuilding the tree.
	for (vgui_widget* w : ctx->all_widgets)
	{
		if (!w || !ctx->alive.count(w)) continue;
		if (w->text_color == old.text) w->text_color = theme->text;
		if (w->accent == old.accent) w->accent = theme->accent;
		if (w->track == old.track) w->track = theme->track;
		if (w->bg == old.panel_bg) w->bg = theme->panel_bg;
		else if (w->bg == old.button_bg) w->bg = theme->button_bg;
		else if (w->bg == old.edit_bg) w->bg = theme->edit_bg;
		if (w->border == old.panel_border) w->border = theme->panel_border;
		else if (w->border == old.button_border) w->border = theme->button_border;
	}
}

void vgui_context_get_theme(vgui_context* ctx, vgui_theme* theme)
{
	if (theme) *theme = ctx ? ctx->theme : vgui_theme();
}

void vgui_context_set_focus_alpha(vgui_context* ctx, float alpha)
{
	if (!ctx) return;
	ctx->focus_alpha = vgui_clampf(alpha, 0.0f, 1.0f);
	vgui_mark_dirty(ctx->root);
}

float vgui_context_get_focus_alpha(vgui_context* ctx)
{
	return ctx ? ctx->focus_alpha : 1.0f;
}

vgui_widget* vgui_root(vgui_context* ctx)
{
	return ctx ? ctx->root : nullptr;
}

//----------------------------------------------------------------------------
// widget tree
//----------------------------------------------------------------------------

vgui_widget* vgui_widget_create(vgui_context* ctx, vgui_widget_kind kind, const char* text)
{
	if (!ctx) return nullptr;
	return vgui_new_widget(ctx, kind, text);
}

vgui_widget* vgui_widget_add(vgui_widget* parent, vgui_widget_kind kind, const char* text)
{
	if (!parent || !parent->ctx) return nullptr;
	vgui_widget* w = vgui_new_widget(parent->ctx, kind, text);
	vgui_widget_attach(parent, w);
	return w;
}

void vgui_widget_attach(vgui_widget* parent, vgui_widget* child)
{
	if (!parent || !child || parent == child || !parent->ctx || child->ctx != parent->ctx)
		return;
	if (child == parent->ctx->root) return;	// the root has no parent

	// Reject cycles.
	for (vgui_widget* p = parent; p; p = p->parent)
		if (p == child) return;

	if (child->parent == parent) return;
	if (child->destroy_pending) return;

	if (child->parent)
	{
		auto& old = child->parent->children;
		old.erase(std::remove(old.begin(), old.end(), child), old.end());
	}
	child->parent = parent;
	parent->children.push_back(child);
	parent->ctx->layout_dirty = true;
}

void vgui_widget_remove(vgui_widget* widget)
{
	if (!widget || !widget->parent) return;
	vgui_widget* parent = widget->parent;
	auto& v = parent->children;
	v.erase(std::remove(v.begin(), v.end(), widget), v.end());
	widget->parent = nullptr;
	if (widget->ctx) widget->ctx->layout_dirty = true;
}

void vgui_widget_destroy(vgui_widget* widget)
{
	if (!widget || !widget->ctx) return;
	vgui_context* ctx = widget->ctx;
	if (widget == ctx->root) return;	// owned by the context
	vgui_schedule_destroy(widget);
	if (!ctx->in_input)
		vgui_flush_pending(ctx);
}

void vgui_widget_clear(vgui_widget* panel)
{
	if (!panel || !panel->ctx) return;
	vgui_context* ctx = panel->ctx;
	std::vector<vgui_widget*> children = panel->children;
	for (vgui_widget* c : children)
		vgui_schedule_destroy(c);
	panel->children.clear();
	ctx->layout_dirty = true;
	if (!ctx->in_input)
		vgui_flush_pending(ctx);
}

vgui_widget_kind vgui_widget_get_kind(vgui_widget* widget)
{
	return widget ? widget->kind : VGUI_PANEL;
}

vgui_context* vgui_widget_get_context(vgui_widget* widget)
{
	return widget ? widget->ctx : nullptr;
}

vgui_widget* vgui_widget_get_parent(vgui_widget* widget)
{
	return widget ? widget->parent : nullptr;
}

size_t vgui_widget_get_child_count(vgui_widget* widget)
{
	return widget ? widget->children.size() : 0;
}

vgui_widget* vgui_widget_get_child(vgui_widget* widget, size_t index)
{
	if (!widget || index >= widget->children.size()) return nullptr;
	return widget->children[index];
}

//----------------------------------------------------------------------------
// properties
//----------------------------------------------------------------------------

void vgui_widget_set_id(vgui_widget* widget, const char* id)
{
	if (widget) widget->id = id ? id : "";
}

const char* vgui_widget_get_id(vgui_widget* widget)
{
	return widget ? widget->id.c_str() : "";
}

void vgui_widget_set_visible(vgui_widget* widget, int visible)
{
	if (!widget) return;
	visible = visible ? 1 : 0;
	if (widget->visible == visible) return;
	widget->visible = visible;
	vgui_context* ctx = widget->ctx;
	if (ctx)
	{
		if (!visible)
		{
			auto hidden = [widget](vgui_widget* node)
			{
				for (vgui_widget* p = node; p; p = p->parent)
					if (p == widget) return true;
				return false;
			};
			if (ctx->hot && hidden(ctx->hot)) { ctx->hot->hovered = 0; ctx->hot = nullptr; }
			if (ctx->active && hidden(ctx->active)) { ctx->active->pressed = 0; ctx->active = nullptr; }
			if (ctx->press_target && hidden(ctx->press_target)) ctx->press_target = nullptr;
			if (ctx->slider_drag && hidden(ctx->slider_drag)) ctx->slider_drag = nullptr;
			if (ctx->focus && hidden(ctx->focus)) vgui_set_focus_internal(ctx, nullptr);
		}
		ctx->layout_dirty = true;
	}
}

int vgui_widget_get_visible(vgui_widget* widget)
{
	return widget ? widget->visible : 0;
}

void vgui_widget_set_enabled(vgui_widget* widget, int enabled)
{
	if (!widget) return;
	enabled = enabled ? 1 : 0;
	widget->enabled = enabled;
	if (!enabled && widget->ctx)
	{
		vgui_context* ctx = widget->ctx;
		auto disabled = [widget](vgui_widget* node)
		{
			for (vgui_widget* p = node; p; p = p->parent)
				if (p == widget) return true;
			return false;
		};
		if (ctx->active && disabled(ctx->active)) { ctx->active->pressed = 0; ctx->active = nullptr; }
		if (ctx->press_target && disabled(ctx->press_target)) ctx->press_target = nullptr;
		if (ctx->slider_drag && disabled(ctx->slider_drag)) ctx->slider_drag = nullptr;
		if (ctx->focus && disabled(ctx->focus)) vgui_set_focus_internal(ctx, nullptr);
	}
}

int vgui_widget_get_enabled(vgui_widget* widget)
{
	return widget ? widget->enabled : 0;
}

void vgui_widget_set_focusable(vgui_widget* widget, int focusable)
{
	if (widget) widget->focusable = focusable ? 1 : 0;
}

int vgui_widget_get_focusable(vgui_widget* widget)
{
	return widget ? widget->focusable : 0;
}

void vgui_widget_set_text(vgui_widget* widget, const char* text)
{
	if (!widget) return;
	widget->text = text ? text : "";
	if (widget->kind == VGUI_EDIT)
	{
		widget->edit_text = widget->text;
		widget->edit_cursor = widget->edit_anchor = (int)widget->edit_text.size();
		widget->ime_text.clear();
		widget->ime_cursor = widget->ime_length = 0;
	}
	vgui_mark_dirty(widget);
}

const char* vgui_widget_get_text(vgui_widget* widget)
{
	if (!widget) return "";
	return widget->kind == VGUI_EDIT ? widget->edit_text.c_str() : widget->text.c_str();
}

void vgui_widget_set_font_size(vgui_widget* widget, float size)
{
	if (!widget) return;
	widget->font_size = size > 0 ? size : 0;
	vgui_mark_dirty(widget);
}

void vgui_widget_set_text_color(vgui_widget* widget, vgui_color color)
{
	if (widget) widget->text_color = color;
}

void vgui_widget_set_background(vgui_widget* widget, vgui_color color)
{
	if (widget) widget->bg = color;
}

void vgui_widget_set_border(vgui_widget* widget, vgui_color color, float width)
{
	if (!widget) return;
	widget->border = color;
	widget->border_width = width > 0 ? width : 0;
}

void vgui_widget_set_rounding(vgui_widget* widget, float radius)
{
	if (widget) widget->rounding = radius > 0 ? radius : 0;
}

void vgui_widget_set_text_align(vgui_widget* widget, float align_x, float align_y)
{
	if (!widget) return;
	widget->text_align_x = vgui_clampf(align_x, 0.0f, 1.0f);
	widget->text_align_y = vgui_clampf(align_y, 0.0f, 1.0f);
}

void vgui_widget_set_cursor(vgui_widget* widget, vgui_cursor cursor)
{
	if (widget) widget->cursor_type = cursor;
}

void vgui_widget_set_opacity(vgui_widget* widget, float opacity)
{
	if (widget) widget->opacity = vgui_clampf(opacity, 0.0f, 1.0f);
}

void vgui_widget_set_focus_alpha(vgui_widget* widget, float alpha)
{
	if (!widget) return;
	widget->focus_alpha = alpha < 0.0f ? -1.0f : vgui_clampf(alpha, 0.0f, 1.0f);
	vgui_mark_dirty(widget);
}

float vgui_widget_get_focus_alpha(vgui_widget* widget)
{
	if (!widget) return 0.0f;
	if (widget->focus_alpha >= 0.0f) return widget->focus_alpha;
	return widget->ctx ? widget->ctx->focus_alpha : 1.0f;
}

void vgui_widget_set_layout(vgui_widget* widget, vgui_layout_kind layout)
{
	if (!widget) return;
	widget->layout = layout;
	vgui_mark_dirty(widget);
}

void vgui_widget_set_pos(vgui_widget* widget, float x, float y)
{
	if (!widget) return;
	widget->x = x;
	widget->y = y;
	vgui_mark_dirty(widget);
}

void vgui_widget_set_size(vgui_widget* widget, float width, float height)
{
	if (!widget) return;
	widget->req_w = width > 0 ? width : 0;
	widget->req_h = height > 0 ? height : 0;
	vgui_mark_dirty(widget);
}

void vgui_widget_set_min_size(vgui_widget* widget, float width, float height)
{
	if (!widget) return;
	widget->min_w = width > 0 ? width : 0;
	widget->min_h = height > 0 ? height : 0;
	vgui_mark_dirty(widget);
}

void vgui_widget_set_max_size(vgui_widget* widget, float width, float height)
{
	if (!widget) return;
	widget->max_w = width > 0 ? width : 0;
	widget->max_h = height > 0 ? height : 0;
	vgui_mark_dirty(widget);
}

void vgui_widget_set_margin(vgui_widget* widget, float left, float top, float right, float bottom)
{
	if (!widget) return;
	widget->margin[0] = left;
	widget->margin[1] = top;
	widget->margin[2] = right;
	widget->margin[3] = bottom;
	vgui_mark_dirty(widget);
}

void vgui_widget_set_padding(vgui_widget* widget, float left, float top, float right, float bottom)
{
	if (!widget) return;
	widget->padding[0] = left;
	widget->padding[1] = top;
	widget->padding[2] = right;
	widget->padding[3] = bottom;
	vgui_mark_dirty(widget);
}

void vgui_widget_set_gap(vgui_widget* widget, float gap)
{
	if (!widget) return;
	widget->gap = gap > 0 ? gap : 0;
	vgui_mark_dirty(widget);
}

void vgui_widget_set_align_items(vgui_widget* widget, vgui_align align)
{
	if (!widget) return;
	widget->align_items = align;
	vgui_mark_dirty(widget);
}

void vgui_widget_set_align_self(vgui_widget* widget, vgui_align align)
{
	if (!widget) return;
	widget->align_self = align;
	vgui_mark_dirty(widget);
}

void vgui_widget_set_justify(vgui_widget* widget, vgui_justify justify)
{
	if (!widget) return;
	widget->justify = justify;
	vgui_mark_dirty(widget);
}

void vgui_widget_set_grow(vgui_widget* widget, float grow)
{
	if (!widget) return;
	widget->grow = grow > 0 ? grow : 0;
	vgui_mark_dirty(widget);
}

void vgui_widget_get_rect(vgui_widget* widget, float* x, float* y, float* width, float* height)
{
	if (!widget || !widget->ctx)
	{
		if (x) *x = 0;
		if (y) *y = 0;
		if (width) *width = 0;
		if (height) *height = 0;
		return;
	}
	vgui_ensure_layout(widget->ctx);
	if (!widget->parent && widget != widget->ctx->root)
	{
		vgui_measure_widget(widget, widget->ctx);
		vgui_layout_widget(widget, widget->ctx, 0, 0,
			std::max(widget->intrinsic_w, 1.0f), std::max(widget->intrinsic_h, 1.0f));
	}
	if (x) *x = widget->abs_x;
	if (y) *y = widget->abs_y;
	if (width) *width = widget->abs_w;
	if (height) *height = widget->abs_h;
}

void vgui_widget_set_value(vgui_widget* widget, float value)
{
	if (!widget) return;
	if (widget->kind == VGUI_RADIO)
	{
		if (value >= 0.5f)
		{
			// Selecting a radio unchecks its siblings without notifying them.
			if (widget->parent)
			{
				for (vgui_widget* s : widget->parent->children)
					if (s != widget && s->kind == VGUI_RADIO) s->checked = 0;
			}
			widget->checked = 1;
		}
		else
		{
			widget->checked = 0;
		}
		return;
	}
	if (widget->kind == VGUI_SLIDER || widget->kind == VGUI_PROGRESS)
	{
		float span = widget->range_max - widget->range_min;
		float normalized = span != 0 ? (value - widget->range_min) / span : 0.0f;
		vgui_set_value_internal(widget, normalized, false);
		return;
	}
	vgui_set_value_internal(widget, value, false);
}

float vgui_widget_get_value(vgui_widget* widget)
{
	return widget ? vgui_value_of(widget) : 0.0f;
}

void vgui_widget_set_range(vgui_widget* widget, float min_value, float max_value)
{
	if (!widget) return;
	if (min_value > max_value) std::swap(min_value, max_value);
	widget->range_min = min_value;
	widget->range_max = max_value;
}

void vgui_widget_set_step(vgui_widget* widget, float step)
{
	if (widget) widget->step = step > 0 ? step : 0;
}

void vgui_widget_set_checked(vgui_widget* widget, int checked)
{
	if (!widget) return;
	checked = checked ? 1 : 0;
	if (widget->kind == VGUI_RADIO)
	{
		vgui_widget_set_value(widget, checked ? 1.0f : 0.0f);
		return;
	}
	widget->checked = checked;
}

int vgui_widget_get_checked(vgui_widget* widget)
{
	return widget ? widget->checked : 0;
}

void vgui_widget_set_vertical(vgui_widget* widget, int vertical)
{
	if (!widget) return;
	widget->vertical = vertical ? 1 : 0;
	vgui_mark_dirty(widget);
}

void vgui_panel_set_scrollable(vgui_widget* panel, int horizontal, int vertical)
{
	if (!panel) return;
	panel->scrollable_x = horizontal ? 1 : 0;
	panel->scrollable_y = vertical ? 1 : 0;
	vgui_mark_dirty(panel);
}

void vgui_panel_set_scroll(vgui_widget* panel, float x, float y)
{
	if (!panel) return;
	panel->scroll_x = x;
	panel->scroll_y = y;
	vgui_mark_dirty(panel);
}

void vgui_panel_scroll_by(vgui_widget* panel, float dx, float dy)
{
	if (!panel) return;
	panel->scroll_x += dx;
	panel->scroll_y += dy;
	vgui_mark_dirty(panel);
}

void vgui_panel_get_scroll(vgui_widget* panel, float* x, float* y)
{
	if (panel && panel->ctx) vgui_ensure_layout(panel->ctx);
	if (x) *x = panel ? panel->scroll_x : 0;
	if (y) *y = panel ? panel->scroll_y : 0;
}

void vgui_panel_get_content_size(vgui_widget* panel, float* width, float* height)
{
	if (panel && panel->ctx) vgui_ensure_layout(panel->ctx);
	if (width) *width = panel ? panel->content_w : 0;
	if (height) *height = panel ? panel->content_h : 0;
}

void vgui_widget_on_event(vgui_widget* widget, vgui_event_fn fn, void* user)
{
	if (!widget) return;
	widget->event_fn = fn;
	widget->user = user;
}

void vgui_widget_on_value(vgui_widget* widget, vgui_value_fn fn, void* user)
{
	if (!widget) return;
	widget->value_fn = fn;
	widget->user = user;
}

void vgui_widget_on_text(vgui_widget* widget, vgui_text_fn fn, void* user)
{
	if (!widget) return;
	widget->text_fn = fn;
	widget->user = user;
}

//----------------------------------------------------------------------------
// text edit helpers
//----------------------------------------------------------------------------

void vgui_edit_set_text(vgui_widget* edit, const char* text)
{
	if (!edit || edit->kind != VGUI_EDIT) return;
	edit->edit_text = text ? text : "";
	edit->text = edit->edit_text;
	edit->edit_cursor = edit->edit_anchor = (int)edit->edit_text.size();
	edit->ime_text.clear();
	edit->ime_cursor = edit->ime_length = 0;
	edit->caret_blink = 0;
	edit->edit_drag = 0;
	vgui_edit_history_reset(edit);
	vgui_mark_dirty(edit);
	if (edit->focused)
	{
		// 保持光标可见（例如“追加文本”后继续编辑）
		vgui_edit_ensure_visible(edit);
	}
	else
	{
		// 程序化设置文本：未获得焦点时显示文本开头
		edit->edit_scroll_x = 0;
		edit->edit_scroll_y = 0;
	}
}

const char* vgui_edit_get_text(vgui_widget* edit)
{
	if (!edit) return "";
	return edit->kind == VGUI_EDIT ? edit->edit_text.c_str() : edit->text.c_str();
}

void vgui_edit_set_placeholder(vgui_widget* edit, const char* text)
{
	if (!edit) return;
	edit->placeholder = text ? text : "";
	vgui_mark_dirty(edit);
}

void vgui_edit_set_password(vgui_widget* edit, char mask)
{
	if (!edit) return;
	edit->password = mask != 0;
	edit->password_char = mask ? mask : '*';
}

void vgui_edit_set_multiline(vgui_widget* edit, int multiline)
{
	if (!edit) return;
	edit->multiline = multiline ? 1 : 0;
	vgui_mark_dirty(edit);
}

void vgui_edit_set_readonly(vgui_widget* edit, int readonly)
{
	if (!edit) return;
	edit->readonly = readonly ? 1 : 0;
	if (edit->readonly)
	{
		edit->ime_text.clear();
		edit->ime_cursor = edit->ime_length = 0;
	}
	if (edit->ctx && edit->ctx->focus == edit)
		edit->ctx->want_text = !edit->readonly;
}

void vgui_edit_set_cursor(vgui_widget* edit, int byte_index)
{
	if (!edit || edit->kind != VGUI_EDIT) return;
	edit->edit_cursor = edit->edit_anchor = vgui_edit_clamp_index(edit->edit_text, byte_index);
	edit->caret_blink = 0;
	vgui_edit_ensure_visible(edit);
}

int vgui_edit_get_cursor(vgui_widget* edit)
{
	return edit ? vgui_edit_clamp_index(edit->edit_text, edit->edit_cursor) : 0;
}

void vgui_edit_select_all(vgui_widget* edit)
{
	if (!edit || edit->kind != VGUI_EDIT) return;
	edit->edit_anchor = 0;
	edit->edit_cursor = (int)edit->edit_text.size();
	edit->caret_blink = 0;
}

void vgui_edit_select(vgui_widget* edit, int anchor, int cursor)
{
	if (!edit || edit->kind != VGUI_EDIT) return;
	edit->edit_anchor = vgui_edit_clamp_index(edit->edit_text, anchor);
	edit->edit_cursor = vgui_edit_clamp_index(edit->edit_text, cursor);
	edit->caret_blink = 0;
	vgui_edit_ensure_visible(edit);
}

void vgui_edit_clear_selection(vgui_widget* edit)
{
	if (!edit || edit->kind != VGUI_EDIT) return;
	edit->edit_anchor = vgui_edit_clamp_index(edit->edit_text, edit->edit_cursor);
}

int vgui_edit_has_selection(vgui_widget* edit)
{
	return (edit && edit->kind == VGUI_EDIT && vgui_edit_has_sel(edit)) ? 1 : 0;
}

void vgui_edit_set_ime_text(vgui_widget* edit, const char* text, int cursor, int length)
{
	if (!edit || edit->kind != VGUI_EDIT) return;
	edit->ime_text = text ? text : "";
	edit->ime_cursor = vgui_edit_clamp_index(edit->ime_text, cursor);
	edit->ime_length = std::max(0, length);
	if (edit->ime_cursor + edit->ime_length > (int)edit->ime_text.size())
		edit->ime_length = std::max(0, (int)edit->ime_text.size() - edit->ime_cursor);
}

void vgui_edit_set_selection_color(vgui_widget* edit, vgui_color color)
{
	if (!edit || edit->kind != VGUI_EDIT) return;
	edit->sel_color = color;
	vgui_mark_dirty(edit);
}

void vgui_edit_set_selection_rounding(vgui_widget* edit, float radius)
{
	if (!edit || edit->kind != VGUI_EDIT) return;
	edit->sel_rounding = std::max(0.0f, radius);
	vgui_mark_dirty(edit);
}

int vgui_edit_copy(vgui_widget* edit)
{
	return (edit && edit->kind == VGUI_EDIT) ? vgui_edit_copy_impl(edit) : 0;
}

int vgui_edit_cut(vgui_widget* edit)
{
	return (edit && edit->kind == VGUI_EDIT) ? vgui_edit_cut_impl(edit) : 0;
}

int vgui_edit_paste(vgui_widget* edit)
{
	return (edit && edit->kind == VGUI_EDIT) ? vgui_edit_paste_impl(edit) : 0;
}

int vgui_edit_undo(vgui_widget* edit)
{
	return (edit && edit->kind == VGUI_EDIT) ? vgui_edit_undo_impl(edit) : 0;
}

int vgui_edit_redo(vgui_widget* edit)
{
	return (edit && edit->kind == VGUI_EDIT) ? vgui_edit_redo_impl(edit) : 0;
}

int vgui_edit_can_undo(vgui_widget* edit)
{
	return (edit && edit->kind == VGUI_EDIT && vgui_edit_can_undo_impl(edit)) ? 1 : 0;
}

int vgui_edit_can_redo(vgui_widget* edit)
{
	return (edit && edit->kind == VGUI_EDIT && vgui_edit_can_redo_impl(edit)) ? 1 : 0;
}

//----------------------------------------------------------------------------
// focus and input
//----------------------------------------------------------------------------

void vgui_set_focus(vgui_context* ctx, vgui_widget* widget)
{
	if (!ctx) return;
	if (widget && (widget->ctx != ctx || !widget->visible || !widget->enabled))
		return;
	if (widget && widget->kind != VGUI_EDIT && !widget->focusable)
		return;
	vgui_set_focus_internal(ctx, widget);
}

vgui_widget* vgui_get_focus(vgui_context* ctx)
{
	return ctx ? ctx->focus : nullptr;
}

vgui_widget* vgui_hit_test(vgui_context* ctx, float x, float y)
{
	return vgui_hit_test_ctx(ctx, x, y);
}

vgui_cursor vgui_context_get_cursor(vgui_context* ctx)
{
	return ctx ? ctx->cursor : VGUI_CURSOR_ARROW;
}

int vgui_input_mouse_move(vgui_context* ctx, float x, float y)
{
	return vgui_mouse_move_impl(ctx, x, y);
}

int vgui_input_mouse_down(vgui_context* ctx, int button, float x, float y, int clicks)
{
	return vgui_mouse_down_impl(ctx, button, x, y, clicks);
}

int vgui_input_mouse_up(vgui_context* ctx, int button, float x, float y, int clicks)
{
	return vgui_mouse_up_impl(ctx, button, x, y, clicks);
}

int vgui_input_mouse_wheel(vgui_context* ctx, float wheel_x, float wheel_y)
{
	return vgui_mouse_wheel_impl(ctx, wheel_x, wheel_y);
}

int vgui_input_key(vgui_context* ctx, int key, int down, int mods)
{
	if (key >= 'a' && key <= 'z') key -= ('a' - 'A');
	return vgui_key_impl(ctx, key, down, mods);
}

int vgui_input_text(vgui_context* ctx, const char* utf8)
{
	return vgui_text_impl(ctx, utf8);
}

int vgui_input_editing(vgui_context* ctx, const char* utf8, int cursor, int length)
{
	return vgui_editing_impl(ctx, utf8, cursor, length);
}

void vgui_input_clear(vgui_context* ctx)
{
	if (!ctx) return;
	for (int i = 0; i < 8; i++)
	{
		ctx->mouse_down[i] = 0;
		ctx->click_count[i] = 0;
		ctx->click_time[i] = -10;
	}
	for (vgui_widget* w : ctx->all_widgets)
	{
		if (!w || !ctx->alive.count(w)) continue;
		w->hovered = 0;
		w->pressed = 0;
		w->edit_drag = 0;
		w->scroll_drag_axis = 0;
		w->edit_scroll_drag = 0;
	}
	ctx->hot = nullptr;
	ctx->active = nullptr;
	ctx->press_target = nullptr;
	ctx->slider_drag = nullptr;
	ctx->cursor = VGUI_CURSOR_ARROW;
	ctx->want_mouse = 0;
	ctx->want_key = ctx->focus != nullptr;
	ctx->want_text = (ctx->focus && ctx->focus->kind == VGUI_EDIT && !ctx->focus->readonly);
	vgui_flush_pending(ctx);
}

void vgui_input_focus_next(vgui_context* ctx, int reverse)
{
	vgui_focus_next_impl(ctx, reverse != 0);
}

int vgui_context_wants_mouse(vgui_context* ctx)
{
	if (!ctx) return 0;
	if (ctx->hot && ctx->alive.count(ctx->hot)) return 1;
	if (ctx->active && ctx->alive.count(ctx->active)) return 1;
	if (ctx->press_target && ctx->alive.count(ctx->press_target)) return 1;
	return ctx->want_mouse ? 1 : 0;
}

int vgui_context_wants_keyboard(vgui_context* ctx)
{
	if (!ctx || !ctx->focus) return 0;
	return ctx->alive.count(ctx->focus) && ctx->focus->enabled ? 1 : 0;
}

int vgui_context_wants_text_input(vgui_context* ctx)
{
	if (!ctx || !ctx->focus) return 0;
	vgui_widget* f = ctx->focus;
	return (ctx->alive.count(f) && f->enabled && f->kind == VGUI_EDIT && !f->readonly) ? 1 : 0;
}

int vgui_context_get_text_input_rect(vgui_context* ctx, int* x, int* y, int* width, int* height)
{
	if (x) *x = 0;
	if (y) *y = 0;
	if (width) *width = 0;
	if (height) *height = 0;
	if (!ctx || !ctx->focus || ctx->focus->kind != VGUI_EDIT) return 0;

	vgui_widget* w = ctx->focus;
	if (!ctx->alive.count(w) || !w->visible) return 0;
	vgui_ensure_layout(ctx);

	float lh = vgui_line_height(ctx, w);
	int line = 0, col = 0;
	vgui_edit_line_col(w->edit_text, w->edit_cursor, line, col);
	(void)col;
	int lb = 0, le = 0;
	vgui_edit_line_bytes(w->edit_text, line, lb, le);
	int upto = std::min(std::max(w->edit_cursor, lb), le);
	float caret_x = vgui_text_width_n(w, w->edit_text.data() + lb, upto - lb);
	float cell = 0;
	if (!w->ime_text.empty())
		cell = vgui_text_width_n(w, w->ime_text.data(), vgui_u8_next(w->ime_text, 0));
	if (cell < 1) cell = vgui_font_size(ctx, w);
	float rx = w->abs_x + w->padding[0] - w->edit_scroll_x + caret_x;
	float ry = w->abs_y + w->padding[1] - w->edit_scroll_y + line * lh;
	if (x) *x = (int)std::floor(rx);
	if (y) *y = (int)std::floor(ry);
	if (width) *width = (int)std::ceil(std::max(1.0f, cell));
	if (height) *height = (int)std::ceil(lh);
	return 1;
}

//----------------------------------------------------------------------------
// frame
//----------------------------------------------------------------------------

void vgui_layout(vgui_context* ctx)
{
	if (!ctx) return;
	vgui_flush_pending(ctx);
	vgui_ensure_layout(ctx);
}

void vgui_update(vgui_context* ctx, float delta_time)
{
	vgui_update_impl(ctx, delta_time);
}

void vgui_draw(vgui_context* ctx, struct ovg_ctx_cb* cb, struct rvg_t* vg)
{
	if (!ctx || !cb || !vg || !ctx->root) return;
	vgui_flush_pending(ctx);
	vgui_ensure_layout(ctx);

	vgui_clip_state clips;
	clips.external = {};
	if (cb->get_clip_rect)
		cb->get_clip_rect(vg, &clips.external);
	clips.has_external = clips.external.z > 0 && clips.external.w > 0;

	vgui_draw_widget(ctx->root, cb, vg, &clips);

	// The root may not clip its children; nested clipping still has to leave
	// the renderer's scissor in a clean state for subsequent draw calls.
	while (!clips.stack.empty())
		vgui_clip_pop(&clips, cb, vg);
}

} // extern "C"

//----------------------------------------------------------------------------
// SDL3 backend bridge (C++ linkage, declared in vgui_internal.h)
//----------------------------------------------------------------------------

namespace {

static int vgui_translate_sdl_key(int keycode)
{
	switch (keycode)
	{
	case SDLK_BACKSPACE: return VGUI_KEY_BACKSPACE;
	case SDLK_TAB: return VGUI_KEY_TAB;
	case SDLK_RETURN:
	case SDLK_KP_ENTER: return VGUI_KEY_ENTER;
	case SDLK_ESCAPE: return VGUI_KEY_ESCAPE;
	case SDLK_DELETE: return VGUI_KEY_DELETE;
	case SDLK_LEFT: return VGUI_KEY_LEFT;
	case SDLK_RIGHT: return VGUI_KEY_RIGHT;
	case SDLK_UP: return VGUI_KEY_UP;
	case SDLK_DOWN: return VGUI_KEY_DOWN;
	case SDLK_HOME: return VGUI_KEY_HOME;
	case SDLK_END: return VGUI_KEY_END;
	case SDLK_PAGEUP: return VGUI_KEY_PAGE_UP;
	case SDLK_PAGEDOWN: return VGUI_KEY_PAGE_DOWN;
	case SDLK_INSERT: return VGUI_KEY_INSERT;
	default: break;
	}

	if (keycode >= SDLK_A && keycode <= SDLK_Z)
		return 'A' + (keycode - SDLK_A);
	if (keycode >= SDLK_0 && keycode <= SDLK_9)
		return '0' + (keycode - SDLK_0);
	if (keycode > 0 && keycode < 128)
		return keycode;
	return 0;
}

static int vgui_translate_kmod(int kmod)
{
	int mods = VGUI_MOD_NONE;
	if (kmod & KM_SHIFT) mods |= VGUI_MOD_SHIFT;
	if (kmod & KM_CTRL) mods |= VGUI_MOD_CTRL;
	if (kmod & KM_ALT) mods |= VGUI_MOD_ALT;
	if (kmod & KM_GUI) mods |= VGUI_MOD_GUI;
	return mods;
}

static void vgui_ime_rect_to_event(vgui_context* ctx, int* x, int* y, int* w, int* h)
{
	vgui_context_get_text_input_rect(ctx, x, y, w, h);
}

} // anonymous namespace

int vgui_dispatch_dev_event(vgui_context* ctx, dev_event_t* e)
{
	if (!ctx || !e) return 0;

	int ret = 0;
	switch (e->type)
	{
	case dev_event_type_e::mouse_move_e:
	{
		mouse_move_et* m = e->v.m;
		if (!m) break;
		ret = vgui_mouse_move_impl(ctx, (float)m->x, (float)m->y);
		m->cursor = (cursor_st)vgui_context_get_cursor(ctx);
		break;
	}
	case dev_event_type_e::mouse_button_e:
	{
		mouse_button_et* b = e->v.b;
		if (!b) break;
		if (b->down)
			ret = vgui_mouse_down_impl(ctx, (int)b->button, (float)b->x, (float)b->y, (int)b->clicks);
		else
			ret = vgui_mouse_up_impl(ctx, (int)b->button, (float)b->x, (float)b->y, (int)b->clicks);
		break;
	}
	case dev_event_type_e::mouse_wheel_e:
	{
		mouse_wheel_et* w = e->v.w;
		if (!w) break;
		ret = vgui_mouse_wheel_impl(ctx, (float)w->x, (float)w->y);
		break;
	}
	case dev_event_type_e::keyboard_e:
	{
		keyboard_et* k = e->v.k;
		if (!k) break;
		int key = vgui_translate_sdl_key(k->keycode ? k->keycode : k->sym);
		int mods = vgui_translate_kmod(k->kmod);
		ret = vgui_key_impl(ctx, key, k->down ? 1 : 0, mods);
		break;
	}
	case dev_event_type_e::text_input_e:
	{
		text_input_et* t = e->v.t;
		if (!t) break;
		ret = vgui_text_impl(ctx, t->text ? t->text : "");
		vgui_ime_rect_to_event(ctx, &t->x, &t->y, &t->w, &t->h);
		break;
	}
	case dev_event_type_e::text_editing_e:
	{
		text_editing_et* t = e->v.e;
		if (!t) break;
		ret = vgui_editing_impl(ctx, t->text ? t->text : "", t->start, t->length);
		vgui_ime_rect_to_event(ctx, &t->x, &t->y, &t->w, &t->h);
		break;
	}
	case dev_event_type_e::finger_e:
	{
		finger_et* f = e->v.f;
		if (!f) break;
		float fx = f->x, fy = f->y;
		if (fx >= 0.0f && fx <= 1.0f && fy >= 0.0f && fy <= 1.0f)
		{
			fx *= (float)(ctx->width > 0 ? ctx->width : 1);
			fy *= (float)(ctx->height > 0 ? ctx->height : 1);
		}
		if (f->t == 1)
			ret = vgui_mouse_down_impl(ctx, 1, fx, fy, 1);
		else if (f->t == 2)
			ret = vgui_mouse_up_impl(ctx, 1, fx, fy, 1);
		else
			ret = vgui_mouse_move_impl(ctx, fx, fy);
		break;
	}
	default:
		break;
	}

	if (ret) e->ret = 1;
	return ret;
}

