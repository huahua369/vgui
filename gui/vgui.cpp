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
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <mapView.h>

#include <ovg_c.h>
#include "tools/tools.hpp"
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

// Model records for list, tree and menu controls.  Keeping them out of the
// widget child list is deliberate: layout, tab order and hit testing must not
// see every row as an independent widget.
struct vgui_list_item
{
	vgui_item_id id = 0;
	std::string text;
	std::string detail;
	std::string tooltip;
	void* user = nullptr;
	int enabled = 1;
	// F04 行扩展：行图标、文字颜色覆盖（0 = 主题色）、行尾内联按钮。
	vgui_texture* icon = nullptr;
	uint32_t text_color = 0;
	int button_count = 0;
	std::vector<vgui_texture*> button_icons;
};

struct vgui_list_state
{
	std::vector<vgui_list_item> items;
	std::unordered_map<vgui_item_id, size_t> index;
	std::vector<vgui_item_id> selected;
	vgui_item_id cursor = 0;
	vgui_item_id hover = 0;
	vgui_item_id pressed = 0;
	vgui_item_id range_anchor = 0;
	vgui_item_id next_id = 1;
	vgui_selection_mode selection_mode = VGUI_SELECTION_SINGLE;
	std::string empty_text;
	vgui_item_select_fn select_fn = nullptr;
	void* select_user = nullptr;
	vgui_item_fn activate_fn = nullptr;
	void* activate_user = nullptr;
	vgui_item_button_fn button_fn = nullptr;
	void* button_user = nullptr;
	int hover_button = -1;	// hover 行上悬停的内联按钮序号

	// 最宽行宽度缓存：content_w 逐条测量在万行列表上是每次布局 O(N)，
	// 文本或字体不变时直接复用。模型变更点负责把 valid 置 false。
	bool content_w_valid = false;
	float content_w = 0.0f;
	const font_familys_t* content_w_font = nullptr;
	float content_w_font_size = 0.0f;

	// 可选虚拟数据源：设置后行数据由回调提供，items 不再被使用。
	vgui_row_source source = {};
	bool virtual_mode = false;
	vgui_list_item scratch_item;	// 虚拟模式下按需materialize的一行

	// F03 行拖拽：按下记录，移动超过阈值后发起通用拖拽。
	vgui_row_drag_fn row_drag_fn = nullptr;
	void* row_drag_user = nullptr;
	vgui_item_id drag_press_item = 0;
	int drag_press_index = -1;
	float drag_press_x = 0.0f, drag_press_y = 0.0f;
	int drag_pending = 0;
};

struct vgui_tree_node
{
	vgui_item_id id = 0;
	vgui_item_id parent = 0;
	std::vector<vgui_item_id> children;
	std::string text;
	std::string tooltip;
	void* user = nullptr;
	int enabled = 1;
	int expanded = 0;
	// F04 行扩展，与 vgui_list_item 一致。
	vgui_texture* icon = nullptr;
	uint32_t text_color = 0;
	int button_count = 0;
	std::vector<vgui_texture*> button_icons;
};

struct vgui_tree_row
{
	vgui_item_id id = 0;
	int depth = 0;
};

struct vgui_tree_state
{
	std::vector<vgui_tree_node> nodes;
	std::unordered_map<vgui_item_id, size_t> index;
	std::vector<vgui_item_id> roots;
	std::vector<vgui_tree_row> rows;
	// 反向可见行号缓存：与 rows 同步重建，避免每次查询都线性扫描 rows。
	std::unordered_map<vgui_item_id, int> row_index;
	bool rows_dirty = true;
	std::vector<vgui_item_id> selected;
	vgui_item_id cursor = 0;
	vgui_item_id hover = 0;
	vgui_item_id pressed = 0;
	vgui_item_id range_anchor = 0;
	vgui_item_id next_id = 1;
	vgui_selection_mode selection_mode = VGUI_SELECTION_SINGLE;
	std::string empty_text;
	vgui_item_select_fn select_fn = nullptr;
	void* select_user = nullptr;
	vgui_item_fn activate_fn = nullptr;
	void* activate_user = nullptr;
	vgui_item_button_fn button_fn = nullptr;
	void* button_user = nullptr;
	int hover_button = -1;

	// 最宽可见行宽度缓存。可见行集合变化（展开/折叠/增删）和文本变更都会失效。
	bool content_w_valid = false;
	float content_w = 0.0f;
	const font_familys_t* content_w_font = nullptr;
	float content_w_font_size = 0.0f;
	float content_w_indent = 0.0f;
	float content_w_arrow = 0.0f;

	// 可选虚拟数据源：行是"可见行投影"，展开状态由应用维护。
	vgui_row_source source = {};
	bool virtual_mode = false;
	vgui_tree_node scratch_node;	// 虚拟模式下按需materialize的一行

	// 行过滤（仅全量模型）：match 非 0 的行与其祖先链可见，忽略折叠状态。
	// match_text 命中的子串用 match_color 高亮（0 = 默认暖黄）。
	vgui_tree_filter_fn filter_fn = nullptr;
	void* filter_user = nullptr;
	std::string match_text;
	vgui_color match_color = 0;

	// F03 行拖拽，与 vgui_list_state 相同。
	vgui_row_drag_fn row_drag_fn = nullptr;
	void* row_drag_user = nullptr;
	vgui_item_id drag_press_item = 0;
	int drag_press_index = -1;
	float drag_press_x = 0.0f, drag_press_y = 0.0f;
	int drag_pending = 0;

	/* 按在已选行且有拖源时推迟到抬起再改选择，避免按下即把
	 * 多选拖拽折叠成单选（O03 多选拖动）。 */
	int defer_click = 0;
	int press_mods = 0;
};

struct vgui_menu_item
{
	vgui_item_id id = 0;
	vgui_menu_item_type type = VGUI_MENU_ITEM_NORMAL;
	std::string text;
	std::string shortcut;
	void* user = nullptr;
	int enabled = 1;
	int checkable = 0;
	int checked = 0;
	vgui_widget* submenu = nullptr;
};

struct vgui_menu_state
{
	std::vector<vgui_menu_item> items;
	std::unordered_map<vgui_item_id, size_t> index;
	vgui_item_id next_id = 1;
	vgui_item_id cursor = 0;
	vgui_item_id hover = 0;
	vgui_item_id pressed = 0;
	vgui_item_id submenu_item = 0;
	vgui_item_id pending_submenu_item = 0;
	double pending_submenu_time = 0.0;
	vgui_widget* submenu_open = nullptr;
	vgui_widget* parent_menu = nullptr;
	vgui_item_id parent_item = 0;
	int open = 0;
	float open_x = 0;
	float open_y = 0;
	int has_open_pos = 0;
	float width = 0;	// 0 -> automatic
	float max_height = 0;	// 0 -> context height minus edge margin
	int close_on_activate = 1;
	vgui_item_fn activate_fn = nullptr;
	void* activate_user = nullptr;
	vgui_widget_fn open_fn = nullptr;
	void* open_user = nullptr;
	vgui_widget_fn close_fn = nullptr;
	void* close_user = nullptr;
	/* Set when this popup belongs to a VGUI_MENUBAR item.  The bar uses these to
	 * clear its active title when the popup closes. */
	vgui_widget* owner_menubar = nullptr;
	vgui_item_id owner_item = 0;
};

struct vgui_menubar_item
{
	vgui_item_id id = 0;
	std::string text;
	vgui_widget* menu = nullptr;
	int enabled = 1;
};

struct vgui_menubar_state
{
	std::vector<vgui_menubar_item> items;
	std::unordered_map<vgui_item_id, size_t> index;
	vgui_item_id next_id = 1;
	vgui_item_id hover = 0;
	vgui_item_id pressed = 0;
	vgui_item_id active = 0;
};

struct vgui_splitter_state
{
	float sash_size = 4.0f;
	float default_ratio = 0.5f;
	float sash_x = 0.0f;
	float sash_y = 0.0f;
	float sash_w = 0.0f;
	float sash_h = 0.0f;
	int dragging = 0;
	int changed = 0;
};

struct vgui_tab_item
{
	vgui_item_id id = 0;
	std::string title;
	vgui_widget* page = nullptr;
	int enabled = 1;
	int closeable = 1;
	int unsaved = 0;
};

struct vgui_tab_state
{
	std::vector<vgui_tab_item> tabs;
	std::unordered_map<vgui_item_id, size_t> index;
	vgui_item_id next_id = 1;
	vgui_item_id active = 0;
	vgui_item_id hover = 0;
	vgui_item_id pressed = 0;
	vgui_item_id close_hover = 0;
	vgui_item_id close_pressed = 0;
	vgui_item_id drag_item = 0;
	float drag_start_x = 0.0f;
	int drag_active = 0;
	float tab_height = 30.0f;
	float tab_padding = 12.0f;
	float tab_gap = 2.0f;
	float close_size = 16.0f;
	vgui_item_fn select_fn = nullptr;
	void* select_user = nullptr;
	vgui_item_fn close_fn = nullptr;
	void* close_user = nullptr;
};

struct vgui_dock_panel
{
	vgui_item_id id = 0;
	std::string string_id;
	std::string title;
	vgui_widget* page = nullptr;
	vgui_dock_zone zone = VGUI_DOCK_CENTER;
	int open = 1;
	int closeable = 1;
	float fx = 0.0f, fy = 0.0f, fw = 0.0f, fh = 0.0f;
};

struct vgui_dock_tab_geom
{
	vgui_item_id id = 0;
	float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
	float close_x = 0.0f, close_y = 0.0f, close_w = 0.0f, close_h = 0.0f;
};

struct vgui_dock_state
{
	std::vector<vgui_dock_panel> panels;
	std::unordered_map<vgui_item_id, size_t> index;
	std::vector<vgui_item_id> order[4];	// LEFT, RIGHT, BOTTOM, CENTER
	std::vector<vgui_item_id> float_order;	// bottom-to-top stacking
	vgui_item_id active[4] = {};
	vgui_item_id next_id = 1;
	vgui_item_id hover = 0;
	vgui_item_id pressed = 0;
	vgui_item_id close_hover = 0;
	vgui_item_id close_pressed = 0;
	vgui_item_id press_panel = 0;
	int press_zone = -1;
	float press_x = 0.0f, press_y = 0.0f;
	int drag_mode = 0;	// 0 none, 1 tab pending, 2 tab dragging, 3 frame dragging
	vgui_item_id drag_panel = 0;
	float drag_x = 0.0f, drag_y = 0.0f;
	int drop_zone = -1;
	int drop_index = -1;
	int drop_floating = 0;
	float drag_offset_x = 0.0f, drag_offset_y = 0.0f;
	float zone_size[4] = { -1.0f, -1.0f, -1.0f, -1.0f };	// <0 -> style
	float zone_x[4] = {};
	float zone_y[4] = {};
	float zone_w[4] = {};
	float zone_h[4] = {};
	float tab_scroll[4] = {};
	int scroll_active[4] = {};	// auto-scroll the active tab once after activation
	std::vector<vgui_dock_tab_geom> tabs[4];
	vgui_item_fn close_fn = nullptr;
	void* close_user = nullptr;
	vgui_item_fn activate_fn = nullptr;
	void* activate_user = nullptr;
	vgui_item_fn change_fn = nullptr;
	void* change_user = nullptr;
};

struct vgui_image_state
{
	vgui_texture* texture = nullptr;
	int src_x = 0;
	int src_y = 0;
	int src_w = 0;
	int src_h = 0;
	vgui_image_fit fit = VGUI_IMAGE_FIT_STRETCH;
	vgui_color tint = 0xFFFFFFFFu;
	int flip_x = 0;
	int flip_y = 0;
	int pixel_snap = 0;

	~vgui_image_state()
	{
		if (texture)
			vgui_texture_release(texture);
	}
};

struct vgui_widget
{
	vgui_context* ctx = nullptr;
	vgui_widget* parent = nullptr;
	std::vector<vgui_widget*> children;
	vgui_widget_kind kind = VGUI_PANEL;

	std::string text;
	std::string text_key;
	std::string placeholder;
	std::string edit_text;
	std::string ime_text;
	std::string id;

	void* user = nullptr;
	vgui_event_fn event_fn = nullptr;
	vgui_value_fn value_fn = nullptr;
	vgui_text_fn text_fn = nullptr;
	vgui_scroll_fn scroll_fn = nullptr;
	void* scroll_user = nullptr;
	int scroll_notifying = 0;

	// layout ---------------------------------------------------------------
	vgui_layout_kind layout = VGUI_LAYOUT_ABSOLUTE;
	vgui_align align_items = VGUI_ALIGN_START;
	vgui_align align_self = VGUI_ALIGN_AUTO;
	vgui_justify justify = VGUI_JUSTIFY_START;
	vgui_align_content align_content = VGUI_ALIGN_CONTENT_START;
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
	float focus_alpha = 0.0f;	// 负数则共用ctx的值 < 0 -> use the context value; 0 hides the ring
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

	// tooltip --------------------------------------------------------------
	std::string tooltip;
	vgui_tooltip_text_fn tooltip_text_fn = nullptr;
	void* tooltip_user = nullptr;
	float tooltip_delay = -1.0f;	// <0 -> context style default
	int tooltip_enabled = 1;
	int tooltip_when_disabled = 1;
	vgui_tooltip_anchor tooltip_anchor = VGUI_TOOLTIP_ANCHOR_POINTER;

	// F03 drop target ------------------------------------------------------
	const vgui_drop_target* drop_target = nullptr;

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
	int edit_scroll_drag = 0;	// 多行编辑框内容滚动条拖动中: 0 / 3 垂直 / 4 水平
	float edit_scroll_grab = 0;
	float edit_content_w = 0;
	int edit_content_w_valid = 0;
	const font_familys_t* edit_content_font = nullptr;
	float edit_content_font_size = 0;
	float caret_blink = 0;
	float caret_width = 1.0f;
	vgui_color sel_color = 0;	// 0 -> theme.edit_selection
	float sel_rounding = 2.0f;	// 选中高亮圆角半径
	std::vector<vgui_edit_snapshot> undo_stack;
	std::vector<vgui_edit_snapshot> redo_stack;
	double last_edit_push = -10.0;
	int last_edit_push_kind = 0;

	// model controls -------------------------------------------------------
	std::unique_ptr<vgui_list_state> list;
	std::unique_ptr<vgui_tree_state> tree;
	std::unique_ptr<vgui_menu_state> menu;
	std::unique_ptr<vgui_menubar_state> menubar;

	// container controls ---------------------------------------------------
	std::unique_ptr<vgui_splitter_state> splitter;
	std::unique_ptr<vgui_tab_state> tabs;
	std::unique_ptr<vgui_dock_state> dock;

	// image ----------------------------------------------------------------
	std::unique_ptr<vgui_image_state> image;

	// button icon (VGUI_BUTTON) --------------------------------------------
	vgui_texture* icon_tex = nullptr;
	int icon_w = 0;
	int icon_h = 0;
	vgui_button_icon_pos icon_pos = VGUI_BUTTON_ICON_LEFT;
	float icon_gap = -1.0f;   // <0 表示使用 style.button.icon_gap
	uint32_t icon_tint = 0;    // 0 表示不染色（用纹理原色）

	// button gradient (VGUI_BUTTON, GRADIENT style) ------------------------
	vgui_button_style_kind btn_style_kind = VGUI_BUTTON_STYLE_SOLID;
	// 三组起止色（default / hover / active），0xAARRGGBB 存储
	vgui_color grad_top[3] = { 0xFF4a4a4a, 0x80404040, 0xFF292929 };
	vgui_color grad_bot[3] = { 0xFF3a3a3a, 0x80303030, 0xFF1d1d1d };
	vgui_color gradient_border_light = 0xFF5c5c5c;
	vgui_color gradient_border_dark = 0xFF1d1d1d;
	float gradient_opacity = 1.0f;

	// custom (VGUI_CUSTOM) -------------------------------------------------
	vgui_custom_draw_fn custom_draw = nullptr;
	void* custom_draw_user = nullptr;
	vgui_custom_size_fn custom_size = nullptr;
	void* custom_size_user = nullptr;
	vgui_custom_event_fn custom_event = nullptr;
	void* custom_event_user = nullptr;
	float custom_dt = 0;
	int custom_clip_content = 1;	// 1=裁剪到控件边界（默认），0=允许超出（发光/阴影/拖影等）

	bool destroy_pending = false;
};

// Text measurement caches.  Measuring used to call HarfBuzz for every
// codepoint on every draw, even when neither the text nor the font had
// changed.  The full-string cache handles the common static-label case; the
// per-codepoint cache also helps edit boxes and other prefixes that change
// one character at a time.
struct vgui_text_measure_key
{
	const font_familys_t* font = nullptr;
	int fontsize = 0;
	std::string text;

	bool operator<(const vgui_text_measure_key& o) const
	{
		if (font != o.font) return std::less<const font_familys_t*>()(font, o.font);
		if (fontsize != o.fontsize) return fontsize < o.fontsize;
		return text < o.text;
	}
};

struct vgui_glyph_advance_key
{
	const font_familys_t* font = nullptr;
	int fontsize = 0;
	uint32_t codepoint = 0;

	bool operator<(const vgui_glyph_advance_key& o) const
	{
		if (font != o.font) return std::less<const font_familys_t*>()(font, o.font);
		if (fontsize != o.fontsize) return fontsize < o.fontsize;
		return codepoint < o.codepoint;
	}
};

struct vgui_font_metrics
{
	float line_height = -1.0f;
	float ascent = -1.0f;
};

struct vgui_tooltip_state
{
	vgui_widget* candidate = nullptr;	// hover target being timed
	vgui_item_id candidate_item = 0;
	vgui_widget* owner = nullptr;		// visible tooltip owner
	vgui_item_id owner_item = 0;
	float hover_time = 0.0f;
	int visible = 0;
	int suppressed = 0;			// cleared on the next pointer move
	std::string text;
	std::vector<std::string> lines;
	float x = 0, y = 0, w = 0, h = 0;
};

struct vgui_toast_record
{
	vgui_toast_id id = 0;
	std::string text;
	std::vector<std::string> lines;
	vgui_toast_options options = {};
	float time_left = 0.0f;
	int sticky = 0;
	float x = 0, y = 0, w = 0, h = 0;
};

struct vgui_dialog_state
{
	vgui_widget* panel = nullptr;
	vgui_widget* input = nullptr;
	vgui_widget* confirm_button = nullptr;
	vgui_widget* cancel_button = nullptr;
	vgui_dialog_close_fn on_result = nullptr;
	void* user = nullptr;
	int kind = VGUI_DIALOG_ALERT;
};

struct vgui_modal_state
{
	int active = 0;
	int swallow_up = 0;
	int result = VGUI_MODAL_RESULT_NONE;
	vgui_widget* stage = nullptr;
	vgui_widget* panel = nullptr;
	vgui_widget* restore_focus = nullptr;
	vgui_modal_options options = {};
};

// context 级通用弹出面板(vgui_popup_open):无遮罩的轻量模态。
struct vgui_popup_state
{
	vgui_widget* content = nullptr;	// detached 子树,宿主拥有
	float x = 0.0f, y = 0.0f;		// 请求锚点(左上角)
	int swallow_up = 0;
	vgui_popup_close_fn close_fn = nullptr;
	void* close_user = nullptr;
};

struct vgui_context
{
	font_familys_t* font = nullptr;
	float font_size = 16.0f;
	int width = 0, height = 0;
	vgui_theme theme = {};
	vgui_style style = {};
	float focus_alpha = 1.0f;	// default focus-ring opacity
	vgui_widget* root = nullptr;

	// i18n / direction ------------------------------------------------------
	vgui_direction direction = VGUI_DIRECTION_LTR;
	std::string language;
	/* language -> key -> text.  Ordered maps keep pointers returned by
	 * vgui_context_translate() stable until the entry is overwritten. */
	std::map<std::string, std::map<std::string, std::string>> strings;

	std::map<vgui_text_measure_key, float> text_width_cache;
	std::map<vgui_glyph_advance_key, float> glyph_advance_cache;
	std::map<std::pair<const font_familys_t*, int>, vgui_font_metrics> font_metrics_cache;

	std::unordered_set<vgui_widget*> all_widgets;
	std::unordered_set<vgui_widget*> alive;
	std::vector<vgui_widget*> pending_delete;

	std::unordered_set<vgui_texture*> textures;
	std::vector<vgui_texture*> pending_texture_destroy;
	std::vector<vgui_texture*> retired_textures;
	uint64_t frame_index = 0;
	flex_run* flex = nullptr;

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
	vgui_widget* open_menu = nullptr;
	vgui_widget* menu_pressed = nullptr;
	int menu_swallow_up = 0;
	int mods = 0;
	double time = 0;

	int want_mouse = 0;
	int want_key = 0;
	int want_text = 0;
	vgui_cursor cursor = VGUI_CURSOR_ARROW;

	// F03 drag & drop -------------------------------------------------------
	vgui_drag_kind drag_kind = VGUI_DRAG_NONE;	// != NONE 时拖拽进行中
	std::string drag_text;						// 载荷字符串的持有副本
	std::vector<std::string> drag_files;
	std::vector<const char*> drag_file_ptrs;
	vgui_drag_payload drag_payload = {};
	vgui_widget* drag_source = nullptr;
	vgui_widget* drop_hover = nullptr;			// 当前悬停的 drop target 控件
	int drop_accepted = 0;
	vgui_drop_hint drop_hint = {};

	vgui_tooltip_state tooltip;
	int tooltip_enabled = 1;

	vgui_modal_state modal;
	vgui_popup_state popup;
	vgui_dialog_state dialog;
	std::vector<vgui_toast_record> toasts;
	vgui_toast_id next_toast_id = 1;

	// context-level shortcut filters --------------------------------------
	struct key_handler_entry
	{
		int token = 0;
		vgui_key_handler_fn fn = nullptr;
		void* user = nullptr;
	};
	std::vector<key_handler_entry> key_handlers;
	int next_key_handler = 1;

	// context 级焦点观察者（vgui_context_set_focus_listener）
	vgui_focus_fn focus_fn = nullptr;
	void* focus_user = nullptr;

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

	static bool vgui_is_rtl(const vgui_context* ctx)
	{
		return ctx && ctx->direction == VGUI_DIRECTION_RTL;
	}

	static bool vgui_widget_rtl(const vgui_widget* w)
	{
		return w && vgui_is_rtl(w->ctx);
	}

	// i18n lookup: active language -> "en" fallback -> the key itself.
	static const char* vgui_translate(vgui_context* ctx, const char* key)
	{
		if (!ctx || !key) return key ? key : "";
		if (ctx->language.empty())
			return key;
		auto lang = ctx->strings.find(ctx->language);
		if (lang != ctx->strings.end())
		{
			auto it = lang->second.find(key);
			if (it != lang->second.end()) return it->second.c_str();
		}
		auto en = ctx->strings.find("en");
		if (en != ctx->strings.end())
		{
			auto it = en->second.find(key);
			if (it != en->second.end()) return it->second.c_str();
		}
		return key;
	}

	// Re-applies every text-key binding after the language or table changes.
	static void vgui_i18n_refresh(vgui_context* ctx)
	{
		if (!ctx) return;
		for (vgui_widget* w : ctx->alive)
		{
			if (!w || w->text_key.empty()) continue;
			const char* tr = vgui_translate(ctx, w->text_key.c_str());
			w->text = tr ? tr : "";
			if (w->kind == VGUI_EDIT)
			{
				w->edit_text = w->text;
				w->edit_cursor = w->edit_anchor = (int)w->edit_text.size();
				w->ime_text.clear();
				w->ime_cursor = w->ime_length = 0;
				w->edit_content_w_valid = 0;
				w->edit_scroll_x = w->edit_scroll_y = 0;
			}
		}
		ctx->layout_dirty = true;
	}

	// Tooltip lifetime helpers.  The visible overlay lives on the context, so
	// every code path that destroys a widget or otherwise interrupts a hover
	// must be able to drop the references here.
	static void vgui_tooltip_hide(vgui_context* ctx)
	{
		if (!ctx) return;
		vgui_tooltip_state& t = ctx->tooltip;
		t.visible = 0;
		t.owner = nullptr;
		t.owner_item = 0;
		t.text.clear();
		t.lines.clear();
		t.x = t.y = t.w = t.h = 0.0f;
	}

	static void vgui_tooltip_clear_candidate(vgui_context* ctx)
	{
		if (!ctx) return;
		vgui_tooltip_hide(ctx);
		ctx->tooltip.candidate = nullptr;
		ctx->tooltip.candidate_item = 0;
		ctx->tooltip.hover_time = 0.0f;
		ctx->tooltip.suppressed = 0;
	}

	static void vgui_tooltip_suppress(vgui_context* ctx)
	{
		if (!ctx) return;
		vgui_tooltip_hide(ctx);
		ctx->tooltip.hover_time = 0.0f;
		ctx->tooltip.suppressed = 1;
	}

	static void vgui_tooltip_forget_widget(vgui_context* ctx, vgui_widget* w)
	{
		if (!ctx || !w) return;
		vgui_tooltip_state& t = ctx->tooltip;
		if (t.candidate == w)
		{
			t.candidate = nullptr;
			t.candidate_item = 0;
			t.hover_time = 0.0f;
		}
		if (t.owner == w)
			vgui_tooltip_hide(ctx);
	}

	// Input and focus run against the modal stage while a modal is open.  The
	// background tree stays intact but cannot be reached by hit testing or Tab.
	// A context-level popup (vgui_popup_open) does the same without a backdrop.
	static vgui_widget* vgui_active_root(vgui_context* ctx)
	{
		if (!ctx) return nullptr;
		if (ctx->modal.active && ctx->modal.stage)
			return ctx->modal.stage;
		if (ctx->popup.content)
			return ctx->popup.content;
		return ctx->root;
	}

	static bool vgui_is_within(vgui_widget* ancestor, vgui_widget* widget)
	{
		if (!ancestor || !widget) return false;
		for (vgui_widget* p = widget; p; p = p->parent)
			if (p == ancestor) return true;
		return false;
	}

	static vgui_widget* vgui_first_focusable(vgui_widget* root)
	{
		if (!root) return nullptr;
		if (root->visible && root->enabled && !root->destroy_pending && root->focusable)
			return root;
		for (vgui_widget* c : root->children)
		{
			vgui_widget* found = vgui_first_focusable(c);
			if (found) return found;
		}
		return nullptr;
	}

	static bool vgui_any_mouse_down(vgui_context* ctx)
	{
		if (!ctx) return false;
		for (int i = 0; i < 8; ++i)
			if (ctx->mouse_down[i]) return true;
		return false;
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

	static int vgui_texture_bpp(vgui_texture_format format)
	{
		switch (format)
		{
		case VGUI_TEXTURE_FORMAT_RGBA8:
		case VGUI_TEXTURE_FORMAT_BGRA8:
		case VGUI_TEXTURE_FORMAT_RGBA8_SRGB:
		case VGUI_TEXTURE_FORMAT_BGRA8_SRGB:
			return 4;
		default:
			return 0;
		}
	}

	static bool vgui_texture_desc_valid(const vgui_texture_desc* desc)
	{
		if (!desc || desc->width <= 0 || desc->height <= 0)
			return false;
		const int bpp = vgui_texture_bpp(desc->format);
		if (bpp <= 0)
			return false;
		const int stride = desc->stride > 0 ? desc->stride : desc->width * bpp;
		return stride >= desc->width * bpp;
	}

	static vg_format_t vgui_ovg_texture_format(vgui_texture_format format)
	{
		switch (format)
		{
		case VGUI_TEXTURE_FORMAT_BGRA8:      return VG_FORMAT_BGRA8;
		case VGUI_TEXTURE_FORMAT_RGBA8_SRGB: return VG_FORMAT_RGBA8_SRGB;
		case VGUI_TEXTURE_FORMAT_BGRA8_SRGB: return VG_FORMAT_BGRA8_SRGB;
		case VGUI_TEXTURE_FORMAT_RGBA8:
		default:                             return VG_FORMAT_RGBA8;
		}
	}

	// Pack the caller's pixels into a tightly packed RGBA/BGRA buffer.  Keeping
	// a canonical stride means the upload path does not have to remember the
	// caller's row layout.
	static bool vgui_texture_store_desc(vgui_texture* texture, const vgui_texture_desc* desc)
	{
		if (!texture || !vgui_texture_desc_valid(desc))
			return false;
		const int bpp = vgui_texture_bpp(desc->format);
		const size_t tight_stride = (size_t)desc->width * (size_t)bpp;
		const size_t total = tight_stride * (size_t)desc->height;
		if (total == 0)
			return false;

		std::vector<uint8_t> pixels;
		if (desc->pixels)
		{
			const size_t src_stride = desc->stride > 0 ? (size_t)desc->stride : tight_stride;
			pixels.resize(total);
			const uint8_t* src = static_cast<const uint8_t*>(desc->pixels);
			for (int y = 0; y < desc->height; ++y)
				std::memcpy(pixels.data() + (size_t)y * tight_stride,
					src + (size_t)y * src_stride, tight_stride);
		}

		texture->width = desc->width;
		texture->height = desc->height;
		texture->stride = (int)tight_stride;
		texture->format = desc->format;
		texture->pixels.swap(pixels);
		texture->image.width = desc->width;
		texture->image.height = desc->height;
		texture->image.valid = false;
		texture->dirty = !texture->pixels.empty();
		texture->upload_frame = ~0ull;
		return true;
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
		int fontsize = (int)(fs + 0.5f);
		if (fontsize < 1) fontsize = 1;

		vgui_font_metrics* metrics = nullptr;
		if (ctx && ctx->font)
		{
			metrics = &ctx->font_metrics_cache[std::make_pair(ctx->font, fontsize)];
			if (metrics->line_height >= 0)
				return metrics->line_height;
		}

		int lh = 0;
		if (ctx && ctx->font)
			lh = font_get_lineheight(ctx->font, fontsize, true);
		if (lh <= 0)
			lh = (int)(fs * 1.35f + 0.5f);
		if (lh < 1) lh = 1;
		if (metrics)
			metrics->line_height = (float)lh;
		return (float)lh;
	}

	// ovg anchors a text run on its baseline: vg_add_text_run leaves
	// baseline_y = text_st_t::pos.y (the ascender is computed but unused).  The UI
	// works in line boxes, so the renderer needs pos.y = box top + ascender.
	// Mirrors ovg's own ascent source, the primary family at the pixel size.
	static float vgui_text_ascent(vgui_context* ctx, vgui_widget* w)
	{
		float fs = vgui_font_size(ctx, w);
		int fontsize = (int)(fs + 0.5f);
		if (fontsize < 1) fontsize = 1;

		vgui_font_metrics* metrics = nullptr;
		if (ctx && ctx->font)
		{
			metrics = &ctx->font_metrics_cache[std::make_pair(ctx->font, fontsize)];
			if (metrics->ascent >= 0)
				return metrics->ascent;
		}

		float a = 0;
		if (ctx && ctx->font && ctx->font->count > 0 && ctx->font->familys[0] && ctx->font->familys[0]->font)
			a = get_font_extents(ctx->font->familys[0]->font, fontsize, false).x;
		if (a <= 0) a = fs * 0.8f;
		if (metrics)
			metrics->ascent = a;
		return a;
	}

	// Real text measurement through HarfBuzz, mirroring ovg's shaping (font picked
	// by coverage, advances taken at the shaping scale) so layout, alignment and
	// the renderer agree.  Keeps the renderer text crisp and the auto-sized boxes
	// exact.
	static float vgui_measure_text_range(vgui_context* ctx, font_familys_t* ffs, int fontsize, const char* text, int len)
	{
		if (!ffs || ffs->count < 1 || !text || len <= 0) return 0;
		if (fontsize < 1) fontsize = 16;

		// Fast path for complete strings: most labels are measured on every draw
		// with exactly the same font, size and contents.
		vgui_text_measure_key text_key;
		bool text_cached = ctx != nullptr;
		if (text_cached)
		{
			text_key.font = ffs;
			text_key.fontsize = fontsize;
			text_key.text.assign(text, (size_t)len);
			auto it = ctx->text_width_cache.find(text_key);
			if (it != ctx->text_width_cache.end())
				return it->second;
		}

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

			float advance = 0;
			bool advance_cached = false;
			vgui_glyph_advance_key glyph_key;
			if (ctx)
			{
				glyph_key.font = ffs;
				glyph_key.fontsize = fontsize;
				glyph_key.codepoint = cp;
				auto it = ctx->glyph_advance_cache.find(glyph_key);
				if (it != ctx->glyph_advance_cache.end())
				{
					advance = it->second;
					advance_cached = true;
				}
			}

			if (!advance_cached)
			{
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
					advance = fontsize * 0.5f;
				}
				else
				{
					hb_font_set_scale(fam->font, fontsize, fontsize);
					hb_codepoint_t gid = 0;
					hb_font_get_nominal_glyph(fam->font, cp, &gid);
					advance = (float)hb_font_get_glyph_h_advance(fam->font, gid);
				}
				if (ctx)
				{
					if (ctx->glyph_advance_cache.size() >= 8192)
						ctx->glyph_advance_cache.clear();
					ctx->glyph_advance_cache.emplace(glyph_key, advance);
				}
			}
			cur += advance;
		}

		float width = std::max(best, cur);
		if (text_cached)
		{
			if (ctx->text_width_cache.size() >= 4096)
				ctx->text_width_cache.clear();
			ctx->text_width_cache.emplace(std::move(text_key), width);
		}
		return width;
	}

	static float vgui_text_width(vgui_widget* w, const std::string& text)
	{
		vgui_context* ctx = w ? w->ctx : nullptr;
		float fs = vgui_font_size(ctx, w);
		return vgui_measure_text_range(ctx, ctx ? ctx->font : nullptr, (int)fs, text.c_str(), (int)text.size());
	}

	static float vgui_text_width_n(vgui_widget* w, const char* text, int len)
	{
		vgui_context* ctx = w ? w->ctx : nullptr;
		float fs = vgui_font_size(ctx, w);
		return vgui_measure_text_range(ctx, ctx ? ctx->font : nullptr, (int)fs, text, len);
	}

	// Greedy word wrapping used by dialogs and toasts.  Existing newlines are
	// preserved, runs of spaces at a break are dropped, and a single glyph
	// wider than max_width still occupies its own line instead of looping.
	static std::vector<std::string> vgui_wrap_text(vgui_context* ctx, vgui_widget* owner,
		const std::string& text, float max_width)
	{
		std::vector<std::string> lines;
		auto push_paragraph = [&](const std::string& para)
			{
				if (para.empty())
				{
					lines.emplace_back();
					return;
				}
				if (!ctx || max_width <= 0.0f)
				{
					lines.push_back(para);
					return;
				}
				auto measure = [&](const char* data, int len)
					{
						return vgui_measure_text_range(ctx, ctx->font,
							(int)vgui_font_size(ctx, owner), data, len);
					};
				size_t start = 0;
				while (start < para.size())
				{
					size_t i = start, fit = start, last_space = std::string::npos;
					while (i < para.size())
					{
						size_t next = (size_t)vgui_u8_next(para, (int)i);
						if (fit > start &&
							measure(para.data() + start, (int)(next - start)) > max_width)
							break;
						fit = next;
						if (para[i] == ' ' || para[i] == '\t')
							last_space = next;
						i = next;
					}
					if (fit >= para.size())
					{
						lines.push_back(para.substr(start));
						break;
					}
					size_t cut = fit;
					if (last_space != std::string::npos && last_space > start)
						cut = last_space;
					if (cut <= start)
						cut = fit;
					size_t end = cut;
					while (end > start && (para[end - 1] == ' ' || para[end - 1] == '\t'))
						end--;
					lines.push_back(para.substr(start, end - start));
					start = cut;
					while (start < para.size() && (para[start] == ' ' || para[start] == '\t'))
						start++;
				}
			};

		size_t start = 0;
		for (;;)
		{
			size_t nl = text.find('\n', start);
			if (nl == std::string::npos)
			{
				push_paragraph(text.substr(start));
				break;
			}
			push_paragraph(text.substr(start, nl - start));
			start = nl + 1;
		}
		if (lines.empty())
			lines.emplace_back();
		return lines;
	}

	// Widest rendered line of an edit box.  vgui_text_width already has a
	// full-string cache; this per-widget cache avoids rebuilding that key (and
	// the password mask) on every frame and on every scrollbar hit test.
	static float vgui_edit_content_width(vgui_widget* w)
	{
		if (!w) return 0.0f;
		vgui_context* ctx = w->ctx;
		font_familys_t* font = ctx ? ctx->font : nullptr;
		float fs = vgui_font_size(ctx, w);
		if (w->edit_content_w_valid &&
			w->edit_content_font == font &&
			w->edit_content_font_size == fs)
			return w->edit_content_w;

		float width = 0.0f;
		int len = (int)w->edit_text.size();
		if (!w->multiline)
		{
			size_t nl = w->edit_text.find('\n');
			if (nl != std::string::npos) len = (int)nl;
		}
		if (w->password)
		{
			std::string masked;
			masked.reserve((size_t)len);
			for (int i = 0; i < len;)
			{
				if (w->edit_text[(size_t)i] == '\n')
				{
					masked.push_back('\n');
					i++;
				}
				else
				{
					masked.push_back(w->password_char);
					i = vgui_u8_next(w->edit_text, i);
				}
			}
			width = vgui_text_width(w, masked);
		}
		else
		{
			width = vgui_text_width_n(w, w->edit_text.data(), len);
		}

		w->edit_content_w = width;
		w->edit_content_w_valid = 1;
		w->edit_content_font = font;
		w->edit_content_font_size = fs;
		return width;
	}

	static void vgui_edit_invalidate_content_width(vgui_widget* w)
	{
		if (w) w->edit_content_w_valid = 0;
	}

	static int vgui_text_lines(const std::string& text)
	{
		int n = 1;
		for (char c : text) if (c == '\n') n++;
		return n;
	}

	static void vgui_edit_clamp_scroll(vgui_widget* w)
	{
		if (!w) return;
		const vgui_style& st = w->ctx->style;
		float iw = std::max(0.0f, w->abs_w - w->padding[0] - w->padding[2]);
		float ih = std::max(0.0f, w->abs_h - w->padding[1] - w->padding[3]);
		float lh = std::max(1.0f, vgui_line_height(w->ctx, w));
		float max_sx = std::max(0.0f, vgui_edit_content_width(w) - iw + st.edit.scrollbar_extra_scroll);
		float max_sy = w->multiline
			? std::max(0.0f, (float)vgui_text_lines(w->edit_text) * lh - ih)
			: 0.0f;
		w->edit_scroll_x = vgui_clampf(w->edit_scroll_x, 0.0f, max_sx);
		w->edit_scroll_y = vgui_clampf(w->edit_scroll_y, 0.0f, max_sy);
	}

	static bool vgui_edit_hbar_visible(vgui_widget* w, float iw)
	{
		return w && w->multiline && iw > w->ctx->style.edit.scrollbar_min_track &&
			vgui_edit_content_width(w) > iw + 0.5f;
	}

	static bool vgui_edit_vbar_visible(vgui_widget* w, float ih)
	{
		if (!w || !w->multiline || ih <= w->ctx->style.edit.scrollbar_min_track) return false;
		float lh = std::max(1.0f, vgui_line_height(w->ctx, w));
		return (float)vgui_text_lines(w->edit_text) * lh > ih + 0.5f;
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
		if (!w || !w->ctx || !w->ctx->alive.count(w))
			return;
		vgui_event e = {};
		e.type = (int)type;
		e.x = lx;
		e.y = ly;
		e.button = button;
		e.clicks = clicks;
		// VGUI_CUSTOM 自定义事件回调：先调，返回 1 则不再转发到内建 event_fn
		if (w->kind == VGUI_CUSTOM && w->custom_event)
			if (w->custom_event(w, &e, w->custom_event_user))
				return;
		if (w->event_fn)
			w->event_fn(w, &e, w->user);
	}

	static void vgui_emit_full(vgui_widget* w, const vgui_event& e)
	{
		if (!w || !w->ctx || !w->ctx->alive.count(w))
			return;
		if (w->kind == VGUI_CUSTOM && w->custom_event)
			if (w->custom_event(w, &e, w->custom_event_user))
				return;
		if (w->event_fn)
			w->event_fn(w, &e, w->user);
	}

	/* Scroll listeners run after scroll_x/scroll_y change.  Re-entrant calls
	 * (a listener that adjusts the scroll offset) are coalesced: the outer
	 * invocation owns the notification and the listener can read the final
	 * offset directly from the panel. */
	static void vgui_notify_scroll(vgui_widget* w)
	{
		if (!w || !w->scroll_fn || w->scroll_notifying)
			return;
		vgui_scroll_fn fn = w->scroll_fn;
		void* user = w->scroll_user;
		vgui_context* ctx = w->ctx;
		w->scroll_notifying = 1;
		fn(w, w->scroll_x, w->scroll_y, user);
		/* The listener may rebuild and destroy this panel. */
		if (!ctx || ctx->alive.count(w))
			w->scroll_notifying = 0;
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
	// item-model helpers
	//----------------------------------------------------------------------------

	static vgui_list_state* vgui_list_of(vgui_widget* w)
	{
		return (w && w->kind == VGUI_LIST && w->list) ? w->list.get() : nullptr;
	}

	static vgui_tree_state* vgui_tree_of(vgui_widget* w)
	{
		return (w && w->kind == VGUI_TREE && w->tree) ? w->tree.get() : nullptr;
	}

	static vgui_menu_state* vgui_menu_of(vgui_widget* w)
	{
		return (w && w->kind == VGUI_MENU && w->menu) ? w->menu.get() : nullptr;
	}

	static vgui_splitter_state* vgui_splitter_of(vgui_widget* w)
	{
		return (w && w->kind == VGUI_SPLITTER && w->splitter) ? w->splitter.get() : nullptr;
	}

	static vgui_tab_state* vgui_tabs_of(vgui_widget* w)
	{
		return (w && w->kind == VGUI_TABVIEW && w->tabs) ? w->tabs.get() : nullptr;
	}

	static vgui_dock_state* vgui_dock_of(vgui_widget* w)
	{
		return (w && w->kind == VGUI_DOCK && w->dock) ? w->dock.get() : nullptr;
	}

	static vgui_menubar_state* vgui_menubar_of(vgui_widget* w)
	{
		return (w && w->kind == VGUI_MENUBAR && w->menubar) ? w->menubar.get() : nullptr;
	}

	static vgui_menubar_item* vgui_menubar_find(vgui_widget* w, vgui_item_id item)
	{
		vgui_menubar_state* st = vgui_menubar_of(w);
		if (!st || item == 0) return nullptr;
		auto it = st->index.find(item);
		if (it == st->index.end() || it->second >= st->items.size()) return nullptr;
		return &st->items[it->second];
	}

	static float vgui_menubar_item_width(vgui_widget* bar, const vgui_menubar_item& item)
	{
		const vgui_menubar_style& ms = bar->ctx->style.menubar;
		return vgui_text_width(bar, item.text) + ms.item_padding_x * 2.0f;
	}

	static void vgui_menubar_item_rect_at(vgui_widget* bar, size_t index,
		float* out_x, float* out_y, float* out_w, float* out_h)
	{
		vgui_menubar_state* st = vgui_menubar_of(bar);
		const vgui_menubar_style& ms = bar->ctx->style.menubar;
		float inner_y = bar->abs_y + ms.padding_y;
		float inner_h = std::max(0.0f, bar->abs_h - ms.padding_y * 2.0f);
		float item_h = ms.item_height > 0 ? std::min(ms.item_height, inner_h) : inner_h;
		if (item_h < inner_h) inner_y += (inner_h - item_h) * 0.5f;

		float px = bar->abs_x + ms.padding_x;
		float iw = 0.0f;
		if (st && index < st->items.size())
		{
			if (vgui_is_rtl(bar->ctx))
			{
				px = bar->abs_x + bar->abs_w - ms.padding_x;
				for (size_t i = 0; i <= index; ++i)
					px -= vgui_menubar_item_width(bar, st->items[i]) + (i > 0 ? ms.gap : 0.0f);
			}
			else
			{
				for (size_t i = 0; i < index; ++i)
					px += vgui_menubar_item_width(bar, st->items[i]) + ms.gap;
			}
			iw = vgui_menubar_item_width(bar, st->items[index]);
		}
		if (out_x) *out_x = px;
		if (out_y) *out_y = inner_y;
		if (out_w) *out_w = iw;
		if (out_h) *out_h = item_h;
	}

	static float vgui_menubar_content_width(vgui_widget* bar)
	{
		vgui_menubar_state* st = vgui_menubar_of(bar);
		if (!st) return 0.0f;
		const vgui_menubar_style& ms = bar->ctx->style.menubar;
		float w = 0.0f;
		for (size_t i = 0; i < st->items.size(); ++i)
		{
			if (i > 0) w += ms.gap;
			w += vgui_menubar_item_width(bar, st->items[i]);
		}
		return w;
	}

	static vgui_tab_item* vgui_tab_find(vgui_widget* w, vgui_item_id item)
	{
		vgui_tab_state* st = vgui_tabs_of(w);
		if (!st || item == 0) return nullptr;
		auto it = st->index.find(item);
		if (it == st->index.end() || it->second >= st->tabs.size()) return nullptr;
		return &st->tabs[it->second];
	}

	static int vgui_tab_find_index(vgui_widget* w, vgui_item_id item)
	{
		vgui_tab_state* st = vgui_tabs_of(w);
		if (!st || item == 0) return -1;
		auto it = st->index.find(item);
		if (it == st->index.end() || it->second >= st->tabs.size()) return -1;
		return (int)it->second;
	}

	static float vgui_tab_item_width(vgui_widget* w, const vgui_tab_item& tab)
	{
		vgui_tab_state* st = vgui_tabs_of(w);
		if (!st) return 0.0f;
		float width = vgui_text_width(w, tab.title) + st->tab_padding * 2.0f;
		if (tab.unsaved) width += 10.0f;
		if (tab.closeable) width += st->close_size + 8.0f;
		return std::max(64.0f, width);
	}

	static float vgui_tab_bar_width(vgui_widget* w)
	{
		vgui_tab_state* st = vgui_tabs_of(w);
		if (!st || st->tabs.empty()) return 0.0f;
		float width = st->tab_gap * (float)(st->tabs.size() - 1);
		for (const vgui_tab_item& tab : st->tabs)
			width += vgui_tab_item_width(w, tab);
		return width;
	}

	static bool vgui_tab_bar_hit(vgui_widget* w, float x, float y)
	{
		vgui_tab_state* st = vgui_tabs_of(w);
		if (!st) return false;
		float ix = w->abs_x + w->padding[0];
		float iy = w->abs_y + w->padding[1];
		float iw = std::max(0.0f, w->abs_w - w->padding[0] - w->padding[2]);
		float ih = std::max(0.0f, w->abs_h - w->padding[1] - w->padding[3]);
		float th = std::min(st->tab_height, ih);
		return x >= ix && x <= ix + iw && y >= iy && y <= iy + th;
	}

	static bool vgui_tab_rect(vgui_widget* w, int index, float* x, float* y, float* width, float* height)
	{
		vgui_tab_state* st = vgui_tabs_of(w);
		if (!st || index < 0 || index >= (int)st->tabs.size()) return false;
		float ix = w->abs_x + w->padding[0];
		float iy = w->abs_y + w->padding[1];
		float iw = std::max(0.0f, w->abs_w - w->padding[0] - w->padding[2]);
		float ih = std::max(0.0f, w->abs_h - w->padding[1] - w->padding[3]);
		float th = std::min(st->tab_height, ih);
		float tx = ix - w->scroll_x;
		if (vgui_is_rtl(w->ctx))
		{
			tx = ix + iw + w->scroll_x;
			for (int i = 0; i <= index; ++i)
				tx -= vgui_tab_item_width(w, st->tabs[(size_t)i]) + (i > 0 ? st->tab_gap : 0.0f);
		}
		else
		{
			for (int i = 0; i < index; ++i)
				tx += vgui_tab_item_width(w, st->tabs[(size_t)i]) + st->tab_gap;
		}
		if (x) *x = tx;
		if (y) *y = iy;
		if (width) *width = vgui_tab_item_width(w, st->tabs[(size_t)index]);
		if (height) *height = th;
		return true;
	}

	static bool vgui_tab_close_rect(vgui_widget* w, int index, float* x, float* y, float* width, float* height)
	{
		vgui_tab_state* st = vgui_tabs_of(w);
		if (!st || index < 0 || index >= (int)st->tabs.size()) return false;
		if (!st->tabs[(size_t)index].closeable) return false;
		float tx = 0, ty = 0, tw = 0, th = 0;
		if (!vgui_tab_rect(w, index, &tx, &ty, &tw, &th)) return false;
		float size = std::min(st->close_size, th);
		if (x) *x = vgui_is_rtl(w->ctx)
			? tx + st->tab_padding * 0.5f
			: tx + tw - size - st->tab_padding * 0.5f;
		if (y) *y = ty + (th - size) * 0.5f;
		if (width) *width = size;
		if (height) *height = size;
		return true;
	}

	static int vgui_tab_index_at(vgui_widget* w, float x, float y, bool* over_close)
	{
		if (over_close) *over_close = false;
		if (!vgui_tab_bar_hit(w, x, y)) return -1;
		vgui_tab_state* st = vgui_tabs_of(w);
		if (!st) return -1;
		for (int i = 0; i < (int)st->tabs.size(); ++i)
		{
			float tx = 0, ty = 0, tw = 0, th = 0;
			if (!vgui_tab_rect(w, i, &tx, &ty, &tw, &th)) continue;
			if (x < tx || x >= tx + tw || y < ty || y >= ty + th) continue;
			float cx = 0, cy = 0, cw = 0, ch = 0;
			if (vgui_tab_close_rect(w, i, &cx, &cy, &cw, &ch) &&
				x >= cx && x < cx + cw && y >= cy && y < cy + ch)
			{
				if (over_close) *over_close = true;
			}
			return i;
		}
		return -1;
	}

	static int vgui_tab_drop_index(vgui_widget* w, float x)
	{
		vgui_tab_state* st = vgui_tabs_of(w);
		if (!st) return 0;
		bool rtl = vgui_is_rtl(w->ctx);
		for (int i = 0; i < (int)st->tabs.size(); ++i)
		{
			float tx = 0, ty = 0, tw = 0, th = 0;
			if (!vgui_tab_rect(w, i, &tx, &ty, &tw, &th)) continue;
			if (rtl ? (x > tx + tw * 0.5f) : (x < tx + tw * 0.5f)) return i;
		}
		return (int)st->tabs.size();
	}

	//----------------------------------------------------------------------------
	// dock helpers
	//----------------------------------------------------------------------------

	static void vgui_set_focus_internal(vgui_context* ctx, vgui_widget* w);

	static void vgui_dock_forget_focus(vgui_widget* w, vgui_widget* page)
	{
		if (!w || !w->ctx || !page) return;
		for (vgui_widget* f = w->ctx->focus; f; f = f->parent)
		{
			if (f != page) continue;
			vgui_set_focus_internal(w->ctx, nullptr);
			return;
		}
	}

	static vgui_dock_panel* vgui_dock_find(vgui_widget* w, vgui_item_id item)
	{
		vgui_dock_state* st = vgui_dock_of(w);
		if (!st || item == 0) return nullptr;
		auto it = st->index.find(item);
		if (it == st->index.end() || it->second >= st->panels.size()) return nullptr;
		return &st->panels[it->second];
	}

	static int vgui_dock_find_index(vgui_widget* w, vgui_item_id item)
	{
		vgui_dock_state* st = vgui_dock_of(w);
		if (!st || item == 0) return -1;
		auto it = st->index.find(item);
		if (it == st->index.end() || it->second >= st->panels.size()) return -1;
		return (int)it->second;
	}

	static vgui_dock_panel* vgui_dock_floating_at_point(vgui_widget* w, float x, float y,
		bool* over_title, bool* over_close);

	static bool vgui_dock_panel_open(vgui_dock_state* st, vgui_item_id item)
	{
		if (!st || item == 0) return false;
		auto it = st->index.find(item);
		if (it == st->index.end() || it->second >= st->panels.size()) return false;
		return st->panels[it->second].open != 0;
	}

	static bool vgui_dock_zone_has_open(vgui_dock_state* st, int zone)
	{
		if (!st || zone < 0 || zone >= 4) return false;
		for (vgui_item_id id : st->order[zone])
			if (vgui_dock_panel_open(st, id)) return true;
		return false;
	}

	static vgui_item_id vgui_dock_pick_active(vgui_dock_state* st, int zone)
	{
		if (!st) return 0;
		if (zone == VGUI_DOCK_FLOATING)
		{
			for (auto it = st->float_order.rbegin(); it != st->float_order.rend(); ++it)
				if (vgui_dock_panel_open(st, *it)) return *it;
			return 0;
		}
		if (zone < 0 || zone >= 4) return 0;
		vgui_item_id current = st->active[zone];
		if (current && vgui_dock_panel_open(st, current))
		{
			auto it = st->index.find(current);
			if (it != st->index.end() && it->second < st->panels.size() &&
				st->panels[it->second].zone == (vgui_dock_zone)zone)
				return current;
		}
		for (vgui_item_id id : st->order[zone])
			if (vgui_dock_panel_open(st, id)) return id;
		return 0;
	}

	static void vgui_dock_erase_order(vgui_dock_state* st, vgui_item_id item)
	{
		if (!st) return;
		for (int z = 0; z < 4; ++z)
			st->order[z].erase(std::remove(st->order[z].begin(), st->order[z].end(), item),
				st->order[z].end());
		st->float_order.erase(std::remove(st->float_order.begin(), st->float_order.end(), item),
			st->float_order.end());
	}

	static void vgui_dock_insert_order(vgui_dock_state* st, int zone, vgui_item_id item, int index)
	{
		if (!st) return;
		if (zone == VGUI_DOCK_FLOATING)
		{
			if (index < 0 || index > (int)st->float_order.size())
				st->float_order.push_back(item);
			else
				st->float_order.insert(st->float_order.begin() + index, item);
			return;
		}
		if (zone < 0 || zone >= 4) return;
		std::vector<vgui_item_id>& order = st->order[zone];
		if (index < 0 || index > (int)order.size())
			order.push_back(item);
		else
			order.insert(order.begin() + index, item);
	}

	static float vgui_dock_resolved_zone_size(vgui_widget* w, int zone)
	{
		vgui_dock_state* st = vgui_dock_of(w);
		if (!st || !w || !w->ctx) return 0.0f;
		const vgui_dock_style& ds = w->ctx->style.dock;
		float value = (zone >= 0 && zone < 4) ? st->zone_size[zone] : -1.0f;
		if (value >= 0.0f) return value;
		if (zone == VGUI_DOCK_LEFT) return ds.left_width;
		if (zone == VGUI_DOCK_RIGHT) return ds.right_width;
		if (zone == VGUI_DOCK_BOTTOM) return ds.bottom_height;
		return 0.0f;
	}

	static void vgui_dock_content_rect(vgui_widget* w, float* x, float* y,
		float* width, float* height)
	{
		float rx = 0, ry = 0, rw = 0, rh = 0;
		if (w)
		{
			rx = w->abs_x + w->padding[0];
			ry = w->abs_y + w->padding[1];
			rw = std::max(0.0f, w->abs_w - w->padding[0] - w->padding[2]);
			rh = std::max(0.0f, w->abs_h - w->padding[1] - w->padding[3]);
			if (rw <= 0.0f && w->ctx && w->ctx->width > 0)
				rw = (float)w->ctx->width;
			if (rh <= 0.0f && w->ctx && w->ctx->height > 0)
				rh = (float)w->ctx->height;
		}
		if (x) *x = rx;
		if (y) *y = ry;
		if (width) *width = rw;
		if (height) *height = rh;
	}

	static float vgui_dock_tab_item_width(vgui_widget* w, const vgui_dock_panel& panel)
	{
		if (!w || !w->ctx) return 0.0f;
		const vgui_dock_style& ds = w->ctx->style.dock;
		float width = vgui_text_width(w, panel.title) + ds.tab_padding * 2.0f;
		if (panel.closeable) width += ds.close_size + 4.0f;
		return vgui_clampf(width, ds.tab_min_width, ds.tab_max_width);
	}

	static float vgui_dock_tab_bar_width(vgui_widget* w, int zone)
	{
		vgui_dock_state* st = vgui_dock_of(w);
		if (!st || zone < 0 || zone >= 4) return 0.0f;
		float width = 0.0f;
		int count = 0;
		for (vgui_item_id id : st->order[zone])
		{
			vgui_dock_panel* panel = vgui_dock_find(w, id);
			if (!panel || !panel->open) continue;
			width += vgui_dock_tab_item_width(w, *panel);
			count++;
		}
		if (count > 1) width += w->ctx->style.dock.tab_gap * (float)(count - 1);
		return width;
	}

	static void vgui_dock_float_rect(vgui_widget* w, const vgui_dock_panel& panel,
		float* x, float* y, float* width, float* height)
	{
		if (x) *x = panel.fx;
		if (y) *y = panel.fy;
		if (width) *width = panel.fw;
		if (height) *height = panel.fh;
	}

	static void vgui_dock_floating_content_rect(vgui_widget* w, const vgui_dock_panel& panel,
		float* x, float* y, float* width, float* height)
	{
		float title_h = 0.0f;
		if (w && w->ctx) title_h = w->ctx->style.dock.floating_title_height;
		title_h = std::min(title_h, panel.fh);
		if (x) *x = panel.fx;
		if (y) *y = panel.fy + title_h;
		if (width) *width = panel.fw;
		if (height) *height = std::max(0.0f, panel.fh - title_h);
	}

	static bool vgui_dock_floating_close_rect(vgui_widget* w, const vgui_dock_panel& panel,
		float* x, float* y, float* width, float* height)
	{
		if (!w || !w->ctx || !panel.closeable) return false;
		const vgui_dock_style& ds = w->ctx->style.dock;
		float title_h = std::min(ds.floating_title_height, panel.fh);
		float size = std::min(ds.close_size, title_h);
		if (size <= 0.0f) return false;
		if (x) *x = panel.fx + panel.fw - size - ds.tab_padding * 0.5f;
		if (y) *y = panel.fy + (title_h - size) * 0.5f;
		if (width) *width = size;
		if (height) *height = size;
		return true;
	}

	static bool vgui_dock_tab_at_point(vgui_widget* w, float x, float y,
		int* out_zone, int* out_index, bool* over_close)
	{
		if (out_zone) *out_zone = -1;
		if (out_index) *out_index = -1;
		if (over_close) *over_close = false;
		vgui_dock_state* st = vgui_dock_of(w);
		if (!st) return false;
		const vgui_dock_style& ds = w->ctx->style.dock;
		for (int zone = 0; zone < 4; ++zone)
		{
			if (st->zone_w[zone] <= 0.0f || st->zone_h[zone] <= 0.0f) continue;
			float bar_h = std::min(ds.tab_height, st->zone_h[zone]);
			if (x < st->zone_x[zone] || x >= st->zone_x[zone] + st->zone_w[zone] ||
				y < st->zone_y[zone] || y >= st->zone_y[zone] + bar_h)
				continue;
			for (size_t i = 0; i < st->tabs[zone].size(); ++i)
			{
				const vgui_dock_tab_geom& tab = st->tabs[zone][i];
				if (x < tab.x || x >= tab.x + tab.w || y < tab.y || y >= tab.y + tab.h)
					continue;
				if (out_zone) *out_zone = zone;
				if (out_index) *out_index = (int)i;
				if (over_close && tab.close_w > 0.0f &&
					x >= tab.close_x && x < tab.close_x + tab.close_w &&
					y >= tab.close_y && y < tab.close_y + tab.close_h)
					*over_close = true;
				return true;
			}
			return false;
		}
		return false;
	}

	static int vgui_dock_zone_tab_bar_at(vgui_widget* w, float x, float y)
	{
		vgui_dock_state* st = vgui_dock_of(w);
		if (!st || !w->ctx) return -1;
		// A frame under the pointer hides the tab bar.  The frame being dragged
		// follows the pointer, so ignore it while looking for a redock target.
		vgui_dock_panel* floating = vgui_dock_floating_at_point(w, x, y, nullptr, nullptr);
		if (floating && floating->id != st->drag_panel) return -1;
		const vgui_dock_style& ds = w->ctx->style.dock;
		for (int zone = 0; zone < 4; ++zone)
		{
			if (st->zone_w[zone] <= 0.0f || st->zone_h[zone] <= 0.0f) continue;
			float bar_h = std::min(ds.tab_height, st->zone_h[zone]);
			if (x >= st->zone_x[zone] && x < st->zone_x[zone] + st->zone_w[zone] &&
				y >= st->zone_y[zone] && y < st->zone_y[zone] + bar_h)
				return zone;
		}
		return -1;
	}

	static int vgui_dock_open_index(vgui_widget* w, int zone, vgui_item_id item)
	{
		vgui_dock_state* st = vgui_dock_of(w);
		if (!st || zone < 0 || zone >= 4) return -1;
		int index = 0;
		for (vgui_item_id id : st->order[zone])
		{
			vgui_dock_panel* panel = vgui_dock_find(w, id);
			if (!panel || !panel->open) continue;
			if (id == item) return index;
			index++;
		}
		return -1;
	}

	static vgui_dock_panel* vgui_dock_floating_at_point(vgui_widget* w, float x, float y,
		bool* over_title, bool* over_close)
	{
		if (over_title) *over_title = false;
		if (over_close) *over_close = false;
		vgui_dock_state* st = vgui_dock_of(w);
		if (!st || !w->ctx) return nullptr;
		const vgui_dock_style& ds = w->ctx->style.dock;
		for (auto it = st->float_order.rbegin(); it != st->float_order.rend(); ++it)
		{
			vgui_dock_panel* panel = vgui_dock_find(w, *it);
			if (!panel || !panel->open) continue;
			if (x < panel->fx || x >= panel->fx + panel->fw ||
				y < panel->fy || y >= panel->fy + panel->fh)
				continue;
			float title_h = std::min(ds.floating_title_height, panel->fh);
			if (over_title) *over_title = y < panel->fy + title_h;
			float cx = 0, cy = 0, cw = 0, ch = 0;
			if (over_close && vgui_dock_floating_close_rect(w, *panel, &cx, &cy, &cw, &ch) &&
				x >= cx && x < cx + cw && y >= cy && y < cy + ch)
				*over_close = true;
			return panel;
		}
		return nullptr;
	}

	static bool vgui_dock_chrome_hit(vgui_widget* w, float x, float y)
	{
		int zone = -1, index = -1;
		bool close = false;
		if (vgui_dock_tab_at_point(w, x, y, &zone, &index, &close))
			return true;
		vgui_dock_state* st = vgui_dock_of(w);
		if (!st || !w->ctx) return false;
		const vgui_dock_style& ds = w->ctx->style.dock;
		vgui_dock_panel* panel = vgui_dock_floating_at_point(w, x, y, nullptr, nullptr);
		if (!panel) return false;
		float title_h = std::min(ds.floating_title_height, panel->fh);
		if (y < panel->fy + title_h) return true;
		float cx = 0, cy = 0, cw = 0, ch = 0;
		vgui_dock_floating_content_rect(w, *panel, &cx, &cy, &cw, &ch);
		if (cw <= 0.0f || ch <= 0.0f) return true;
		return x < cx || x >= cx + cw || y < cy || y >= cy + ch;
	}

	static void vgui_dock_update_hover(vgui_widget* w, float x, float y)
	{
		vgui_dock_state* st = vgui_dock_of(w);
		if (!st) return;
		st->hover = 0;
		st->close_hover = 0;
		bool over_title = false;
		bool over_close = false;
		vgui_dock_panel* panel = vgui_dock_floating_at_point(w, x, y, &over_title, &over_close);
		if (panel && over_title)
		{
			st->hover = panel->id;
			if (over_close) st->close_hover = panel->id;
			return;
		}
		int zone = -1, index = -1;
		if (vgui_dock_tab_at_point(w, x, y, &zone, &index, &over_close) &&
			zone >= 0 && index >= 0 && index < (int)st->tabs[zone].size())
		{
			st->hover = st->tabs[zone][(size_t)index].id;
			if (over_close) st->close_hover = st->hover;
		}
	}

	static int vgui_dock_zone_at_point(vgui_widget* w, float x, float y)
	{
		vgui_dock_state* st = vgui_dock_of(w);
		if (!st) return -1;
		if (vgui_dock_floating_at_point(w, x, y, nullptr, nullptr))
			return VGUI_DOCK_FLOATING;
		for (int zone = 0; zone < 4; ++zone)
		{
			if (st->zone_w[zone] <= 0.0f || st->zone_h[zone] <= 0.0f) continue;
			if (x >= st->zone_x[zone] && x < st->zone_x[zone] + st->zone_w[zone] &&
				y >= st->zone_y[zone] && y < st->zone_y[zone] + st->zone_h[zone])
				return zone;
		}
		return -1;
	}

	// Dropping into an empty (collapsed) zone is still useful: infer the target
	// from the pointer's position near the centre area's edges.
	static int vgui_dock_fallback_zone(vgui_widget* w, float x, float y)
	{
		vgui_dock_state* st = vgui_dock_of(w);
		if (!st || !w->ctx) return -1;
		float cx = 0, cy = 0, cw = 0, ch = 0;
		vgui_dock_content_rect(w, &cx, &cy, &cw, &ch);
		if (cw <= 0.0f || ch <= 0.0f) return -1;
		if (x < cx || x >= cx + cw || y < cy || y >= cy + ch) return -1;
		const vgui_dock_style& ds = w->ctx->style.dock;
		float tab_h = std::min(ds.tab_height, ch);
		if (y < cy + tab_h) return VGUI_DOCK_CENTER;
		float left_band = std::min(vgui_dock_resolved_zone_size(w, VGUI_DOCK_LEFT), cw * 0.3f);
		float right_band = std::min(vgui_dock_resolved_zone_size(w, VGUI_DOCK_RIGHT), cw * 0.3f);
		float bottom_band = std::min(vgui_dock_resolved_zone_size(w, VGUI_DOCK_BOTTOM), ch * 0.3f);
		if (left_band > 0.0f && x < cx + left_band) return VGUI_DOCK_LEFT;
		if (right_band > 0.0f && x >= cx + cw - right_band) return VGUI_DOCK_RIGHT;
		if (bottom_band > 0.0f && y >= cy + ch - bottom_band) return VGUI_DOCK_BOTTOM;
		return VGUI_DOCK_CENTER;
	}

	static int vgui_dock_drop_index_at(vgui_widget* w, int zone, float x)
	{
		vgui_dock_state* st = vgui_dock_of(w);
		if (!st || zone < 0 || zone >= 4) return -1;
		if (st->zone_w[zone] <= 0.0f || st->zone_h[zone] <= 0.0f) return -1;
		if (x >= st->zone_x[zone] && x < st->zone_x[zone] + st->zone_w[zone] &&
			st->tabs[zone].empty())
			return 0;
		for (size_t i = 0; i < st->tabs[zone].size(); ++i)
		{
			const vgui_dock_tab_geom& tab = st->tabs[zone][i];
			if (x < tab.x + tab.w * 0.5f) return (int)i;
		}
		return (int)st->tabs[zone].size();
	}

	static int vgui_dock_drop_index_any(vgui_widget* w, int zone, float x)
	{
		vgui_dock_state* st = vgui_dock_of(w);
		if (!st || zone < 0 || zone >= 4) return -1;
		if (!vgui_dock_zone_has_open(st, zone)) return 0;
		return vgui_dock_drop_index_at(w, zone, x);
	}

	static void vgui_dock_notify_activate(vgui_widget* w, vgui_item_id item)
	{
		vgui_dock_state* st = vgui_dock_of(w);
		if (!st) return;
		vgui_item_fn fn = st->activate_fn;
		void* user = st->activate_user;
		if (fn) fn(w, item, user);
	}

	static void vgui_dock_notify_close(vgui_widget* w, vgui_item_id item)
	{
		vgui_dock_state* st = vgui_dock_of(w);
		if (!st) return;
		vgui_item_fn fn = st->close_fn;
		void* user = st->close_user;
		if (fn) fn(w, item, user);
	}

	static void vgui_dock_notify_change(vgui_widget* w)
	{
		vgui_dock_state* st = vgui_dock_of(w);
		if (!st) return;
		vgui_item_fn fn = st->change_fn;
		void* user = st->change_user;
		if (fn) fn(w, 0, user);
	}

	static bool vgui_dock_alive(vgui_widget* w)
	{
		return w && w->ctx && w->ctx->alive.count(w) != 0;
	}

	static bool vgui_dock_raise_float_internal(vgui_widget* w, vgui_item_id item)
	{
		vgui_dock_state* st = vgui_dock_of(w);
		if (!st || !vgui_dock_panel_open(st, item)) return false;
		bool was_top = !st->float_order.empty() && st->float_order.back() == item;
		st->float_order.erase(std::remove(st->float_order.begin(), st->float_order.end(), item),
			st->float_order.end());
		st->float_order.push_back(item);
		vgui_mark_dirty(w);
		return !was_top;
	}

	static bool vgui_dock_activate_internal(vgui_widget* w, vgui_item_id item, bool notify)
	{
		vgui_dock_state* st = vgui_dock_of(w);
		vgui_dock_panel* panel = vgui_dock_find(w, item);
		if (!st || !panel || !panel->open) return false;
		bool changed = false;
		if (panel->zone == VGUI_DOCK_FLOATING)
		{
			changed = vgui_dock_raise_float_internal(w, item);
		}
		else if (panel->zone >= 0 && panel->zone < 4)
		{
			changed = (st->active[panel->zone] != item);
			st->active[panel->zone] = item;
			if (changed) st->scroll_active[panel->zone] = 1;
		}
		vgui_mark_dirty(w);
		if (notify && changed && vgui_dock_alive(w))
			vgui_dock_notify_activate(w, item);
		return true;
	}

	static void vgui_dock_clamp_rect(vgui_widget* w, vgui_dock_panel* panel,
		float x, float y, float width, float height, bool center_if_origin)
	{
		if (!w || !panel || !w->ctx) return;
		const vgui_dock_style& ds = w->ctx->style.dock;
		float bx = 0, by = 0, bw = 0, bh = 0;
		vgui_dock_content_rect(w, &bx, &by, &bw, &bh);
		float fw = width > 0.0f ? width : ds.floating_width;
		float fh = height > 0.0f ? height : ds.floating_height;
		fw = std::max(fw, ds.floating_min_width);
		fh = std::max(fh, ds.floating_min_height);
		if (bw > 0.0f) fw = std::min(fw, bw);
		if (bh > 0.0f) fh = std::min(fh, bh);
		if (center_if_origin && x == 0.0f && y == 0.0f)
		{
			x = bx + (bw - fw) * 0.5f;
			y = by + (bh - fh) * 0.5f;
		}
		if (bw > 0.0f) x = vgui_clampf(x, bx, std::max(bx, bx + bw - fw));
		if (bh > 0.0f) y = vgui_clampf(y, by, std::max(by, by + bh - fh));
		panel->fx = x;
		panel->fy = y;
		panel->fw = fw;
		panel->fh = fh;
	}

	static bool vgui_dock_float_internal(vgui_widget* w, vgui_item_id item,
		float x, float y, float width, float height, bool notify_activate, bool notify_change)
	{
		vgui_dock_state* st = vgui_dock_of(w);
		vgui_dock_panel* panel = vgui_dock_find(w, item);
		if (!st || !panel) return false;
		vgui_dock_erase_order(st, item);
		panel->zone = VGUI_DOCK_FLOATING;
		panel->open = 1;
		vgui_dock_clamp_rect(w, panel, x, y, width, height, true);
		st->float_order.push_back(item);
		for (int z = 0; z < 4; ++z)
		{
			if (st->active[z] != item) continue;
			st->active[z] = vgui_dock_pick_active(st, z);
			st->scroll_active[z] = 1;
		}
		vgui_mark_dirty(w);
		if (notify_activate && vgui_dock_alive(w))
			vgui_dock_notify_activate(w, item);
		if (notify_change && vgui_dock_alive(w))
			vgui_dock_notify_change(w);
		return true;
	}

	static bool vgui_dock_move_internal(vgui_widget* w, vgui_item_id item,
		int zone, int index, bool notify_activate, bool notify_change)
	{
		vgui_dock_state* st = vgui_dock_of(w);
		vgui_dock_panel* panel = vgui_dock_find(w, item);
		if (!st || !panel) return false;
		if (zone == VGUI_DOCK_FLOATING)
			return vgui_dock_float_internal(w, item, 0.0f, 0.0f, 0.0f, 0.0f,
				notify_activate, notify_change);
		if (zone < 0 || zone >= 4) return false;
		int old_zone = (int)panel->zone;
		vgui_dock_erase_order(st, item);
		panel->zone = (vgui_dock_zone)zone;
		panel->open = 1;
		vgui_dock_insert_order(st, zone, item, index);
		if (old_zone >= 0 && old_zone < 4 && st->active[old_zone] == item)
		{
			st->active[old_zone] = vgui_dock_pick_active(st, old_zone);
			st->scroll_active[old_zone] = 1;
		}
		st->active[zone] = item;
		st->scroll_active[zone] = 1;
		vgui_mark_dirty(w);
		if (notify_activate && vgui_dock_alive(w))
			vgui_dock_notify_activate(w, item);
		if (notify_change && vgui_dock_alive(w))
			vgui_dock_notify_change(w);
		return true;
	}

	static bool vgui_dock_close_internal(vgui_widget* w, vgui_item_id item, bool notify)
	{
		vgui_dock_state* st = vgui_dock_of(w);
		vgui_dock_panel* panel = vgui_dock_find(w, item);
		if (!st || !panel || !panel->open) return false;
		panel->open = 0;
		if (panel->zone >= 0 && panel->zone < 4 && st->active[panel->zone] == item)
		{
			st->active[panel->zone] = vgui_dock_pick_active(st, panel->zone);
			st->scroll_active[panel->zone] = 1;
		}
		vgui_dock_forget_focus(w, panel->page);
		if (panel->page) panel->page->visible = 0;
		vgui_mark_dirty(w);
		if (notify && vgui_dock_alive(w))
		{
			vgui_dock_notify_close(w, item);
			if (!vgui_dock_alive(w)) return true;
			vgui_dock_notify_change(w);
		}
		return true;
	}

	static bool vgui_dock_restore_internal(vgui_widget* w, vgui_item_id item, bool notify)
	{
		vgui_dock_state* st = vgui_dock_of(w);
		vgui_dock_panel* panel = vgui_dock_find(w, item);
		if (!st || !panel || panel->open) return false;
		panel->open = 1;
		if (panel->zone == VGUI_DOCK_FLOATING)
			vgui_dock_raise_float_internal(w, item);
		else if (panel->zone >= 0 && panel->zone < 4)
		{
			st->active[panel->zone] = item;
			st->scroll_active[panel->zone] = 1;
		}
		vgui_mark_dirty(w);
		if (notify && vgui_dock_alive(w))
		{
			vgui_dock_notify_activate(w, item);
			if (!vgui_dock_alive(w)) return true;
			vgui_dock_notify_change(w);
		}
		return true;
	}

	static bool vgui_splitter_sash_hit(vgui_widget* w, float x, float y)
	{
		vgui_splitter_state* st = vgui_splitter_of(w);
		if (!st || st->sash_w <= 0.0f || st->sash_h <= 0.0f) return false;
		const float slop = 2.0f;
		return x >= st->sash_x - slop && x <= st->sash_x + st->sash_w + slop &&
			y >= st->sash_y - slop && y <= st->sash_y + st->sash_h + slop;
	}

	static vgui_cursor vgui_splitter_cursor(vgui_widget* w)
	{
		return (w && w->vertical) ? VGUI_CURSOR_SIZE_V : VGUI_CURSOR_SIZE_H;
	}

	static bool vgui_id_selected(const std::vector<vgui_item_id>& ids, vgui_item_id id)
	{
		return id != 0 && std::find(ids.begin(), ids.end(), id) != ids.end();
	}

	// ---- 虚拟数据源访问层 ----------------------------------------------------
	// 列表在虚拟模式下不再持有行数据：行数、行 id、id->行号都走回调。
	// 未设置数据源时这些函数等价于原来的 vector 访问。
	static int vgui_list_row_count(vgui_widget* w, vgui_list_state* st)
	{
		(void)w;
		if (!st) return 0;
		if (st->virtual_mode && st->source.row_count)
			return std::max(0, st->source.row_count(st->source.user));
		return (int)st->items.size();
	}

	static vgui_item_id vgui_list_row_id(vgui_widget* w, vgui_list_state* st, int index)
	{
		(void)w;
		if (!st || index < 0) return 0;
		if (st->virtual_mode && st->source.row_id)
			return st->source.row_id(st->source.user, index);
		return index < (int)st->items.size() ? st->items[(size_t)index].id : 0;
	}

	static int vgui_list_row_index(vgui_widget* w, vgui_list_state* st, vgui_item_id id)
	{
		if (!st || id == 0) return -1;
		if (st->virtual_mode)
		{
			if (st->source.row_id_index)
				return st->source.row_id_index(st->source.user, id);
			int n = vgui_list_row_count(w, st);
			for (int i = 0; i < n; ++i)
				if (vgui_list_row_id(w, st, i) == id) return i;
			return -1;
		}
		auto it = st->index.find(id);
		return it == st->index.end() ? -1 : (int)it->second;
	}

	// 虚拟模式下把第 index 行materialize到 scratch_item；全量模式直接返回内部引用。
	static vgui_list_item* vgui_list_row_materialize(vgui_widget* w, vgui_list_state* st, int index)
	{
		if (!st || index < 0) return nullptr;
		if (!st->virtual_mode)
			return index < (int)st->items.size() ? &st->items[(size_t)index] : nullptr;
		if (!st->source.row_info) return nullptr;
		const char* text = nullptr;
		const char* detail = nullptr;
		void* item_user = nullptr;
		if (!st->source.row_info(st->source.user, index, &text, &detail, &item_user))
			return nullptr;
		st->scratch_item.id = vgui_list_row_id(w, st, index);
		st->scratch_item.text = text ? text : "";
		st->scratch_item.detail = detail ? detail : "";
		const char* tip = st->source.row_tooltip
			? st->source.row_tooltip(st->source.user, index) : nullptr;
		st->scratch_item.tooltip = tip ? tip : "";
		st->scratch_item.user = item_user;
		st->scratch_item.enabled = st->source.row_enabled
			? (st->source.row_enabled(st->source.user, index) ? 1 : 0) : 1;
		st->scratch_item.icon = nullptr;
		if (st->source.row_icon)
			st->source.row_icon(st->source.user, index, &st->scratch_item.icon);
		st->scratch_item.text_color = st->source.row_text_color
			? st->source.row_text_color(st->source.user, index) : 0;
		st->scratch_item.button_count = 0;
		st->scratch_item.button_icons.clear();
		if (st->source.row_buttons)
		{
			const int count = std::min(VGUI_MAX_ROW_BUTTONS,
				std::max(0, st->source.row_buttons(st->source.user, index)));
			for (int b = 0; b < count; ++b)
			{
				vgui_texture* tex = nullptr;
				if (st->source.row_button_icon)
					st->source.row_button_icon(st->source.user, index, b, &tex);
				st->scratch_item.button_icons.push_back(tex);
			}
			st->scratch_item.button_count = count;
		}
		return &st->scratch_item;
	}

	static bool vgui_list_row_enabled(vgui_widget* w, vgui_list_state* st, int index)
	{
		if (st && st->virtual_mode)
			return st->source.row_enabled ? st->source.row_enabled(st->source.user, index) != 0 : true;
		if (!st || index < 0 || index >= (int)st->items.size()) return false;
		return st->items[(size_t)index].enabled != 0;
	}

	static vgui_list_item* vgui_list_item_find(vgui_widget* w, vgui_item_id id, size_t* out_index = nullptr)
	{
		vgui_list_state* st = vgui_list_of(w);
		if (!st || id == 0) return nullptr;
		if (st->virtual_mode)
		{
			int index = vgui_list_row_index(w, st, id);
			if (index < 0) return nullptr;
			if (out_index) *out_index = (size_t)index;
			return vgui_list_row_materialize(w, st, index);
		}
		auto it = st->index.find(id);
		if (it == st->index.end() || it->second >= st->items.size()) return nullptr;
		if (out_index) *out_index = it->second;
		return &st->items[it->second];
	}

	static vgui_list_item* vgui_list_item_at(vgui_widget* w, int index, vgui_item_id* out_id = nullptr)
	{
		vgui_list_state* st = vgui_list_of(w);
		if (!st || index < 0) return nullptr;
		if (st->virtual_mode)
		{
			if (index >= vgui_list_row_count(w, st)) return nullptr;
			if (out_id) *out_id = vgui_list_row_id(w, st, index);
			return vgui_list_row_materialize(w, st, index);
		}
		if (index >= (int)st->items.size()) return nullptr;
		if (out_id) *out_id = st->items[(size_t)index].id;
		return &st->items[(size_t)index];
	}

	// 只在 from 之后重新登记位置。追加是 O(1)，插入/删除只需修正被移动的尾部。
	static void vgui_list_update_index_from(vgui_list_state* st, size_t from)
	{
		if (!st) return;
		if (from > st->items.size()) from = st->items.size();
		for (size_t i = from; i < st->items.size(); ++i)
			st->index[st->items[i].id] = i;
	}

	static void vgui_list_notify_select(vgui_widget* w, vgui_item_id id, int selected)
	{
		vgui_list_state* st = vgui_list_of(w);
		if (!st || !w->ctx || !w->ctx->alive.count(w)) return;
		size_t index = (size_t)-1;
		vgui_list_item_find(w, id, &index);
		if (st->select_fn)
			st->select_fn(w, id, selected ? 1 : 0, st->select_user);
		if (!w->ctx || !w->ctx->alive.count(w)) return;
		vgui_event e = {};
		e.type = VGUI_EVENT_VALUE_CHANGED;
		e.item = id;
		e.index = (index == (size_t)-1) ? -1 : (int)index;
		vgui_emit_full(w, e);
	}

	static bool vgui_list_set_selected_internal(vgui_widget* w, vgui_item_id id, bool selected, bool notify)
	{
		vgui_list_state* st = vgui_list_of(w);
		if (!st || !vgui_list_item_find(w, id)) return false;
		bool has = vgui_id_selected(st->selected, id);
		if (selected == has) return false;
		if (selected)
		{
			if (st->selection_mode == VGUI_SELECTION_NONE) return false;
			if (st->selection_mode == VGUI_SELECTION_SINGLE)
			{
				std::vector<vgui_item_id> old = st->selected;
				st->selected.clear();
				if (notify)
					for (vgui_item_id old_id : old)
						vgui_list_notify_select(w, old_id, 0);
				if (!w->ctx || !w->ctx->alive.count(w)) return true;
				// A selection callback may have removed the target item or
				// changed the selection state.  Re-check before mutating again.
				st = vgui_list_of(w);
				if (!st || !vgui_list_item_find(w, id)) return true;
				if (st->selection_mode == VGUI_SELECTION_NONE) return true;
				if (vgui_id_selected(st->selected, id)) return true;
			}
			st->selected.push_back(id);
		}
		else
		{
			st->selected.erase(std::remove(st->selected.begin(), st->selected.end(), id), st->selected.end());
		}
		if (notify) vgui_list_notify_select(w, id, selected ? 1 : 0);
		return true;
	}

	static void vgui_list_clear_selection_internal(vgui_widget* w, bool notify)
	{
		vgui_list_state* st = vgui_list_of(w);
		if (!st) return;
		std::vector<vgui_item_id> old = st->selected;
		st->selected.clear();
		if (notify)
			for (vgui_item_id id : old)
			{
				vgui_list_notify_select(w, id, 0);
				if (!w->ctx || !w->ctx->alive.count(w)) return;
			}
	}

	static float vgui_list_row_height(vgui_widget* w)
	{
		vgui_list_state* st = vgui_list_of(w);
		if (!st || !w->ctx) return 1.0f;
		float h = w->ctx->style.list.row_height;
		if (h <= 0.0f) h = vgui_line_height(w->ctx, w) + 10.0f;
		return std::max(1.0f, h);
	}

	static float vgui_tree_row_height(vgui_widget* w)
	{
		vgui_tree_state* st = vgui_tree_of(w);
		if (!st || !w->ctx) return 1.0f;
		float h = w->ctx->style.tree.row_height;
		if (h <= 0.0f) h = vgui_line_height(w->ctx, w) + 10.0f;
		return std::max(1.0f, h);
	}

	// 树节点查找。虚拟模式下节点不存在于内存中，改为按行materialize到
	// scratch_node，供绘制和只读 getter 使用。
	static vgui_tree_node* vgui_tree_node_find(vgui_widget* w, vgui_item_id id, size_t* out_index = nullptr)
	{
		vgui_tree_state* st = vgui_tree_of(w);
		if (!st || id == 0) return nullptr;
		if (st->virtual_mode)
		{
			int index = -1;
			if (st->source.row_id_index)
				index = st->source.row_id_index(st->source.user, id);
			else
			{
				int n = st->source.row_count ? std::max(0, st->source.row_count(st->source.user)) : 0;
				for (int i = 0; i < n; ++i)
					if (st->source.row_id && st->source.row_id(st->source.user, i) == id)
					{
						index = i;
						break;
					}
			}
			if (index < 0 || !st->source.row_info) return nullptr;
			const char* text = nullptr;
			const char* detail = nullptr;
			void* item_user = nullptr;
			if (!st->source.row_info(st->source.user, index, &text, &detail, &item_user))
				return nullptr;
			st->scratch_node.id = id;
			st->scratch_node.parent = 0;
			st->scratch_node.children.clear();
			st->scratch_node.text = text ? text : "";
			const char* tip = st->source.row_tooltip
				? st->source.row_tooltip(st->source.user, index) : nullptr;
			st->scratch_node.tooltip = tip ? tip : "";
			st->scratch_node.user = item_user;
			st->scratch_node.enabled = st->source.row_enabled
				? (st->source.row_enabled(st->source.user, index) ? 1 : 0) : 1;
			st->scratch_node.expanded = st->source.row_expanded
				? (st->source.row_expanded(st->source.user, index) ? 1 : 0) : 0;
			st->scratch_node.icon = nullptr;
			if (st->source.row_icon)
				st->source.row_icon(st->source.user, index, &st->scratch_node.icon);
			st->scratch_node.text_color = st->source.row_text_color
				? st->source.row_text_color(st->source.user, index) : 0;
			st->scratch_node.button_count = 0;
			st->scratch_node.button_icons.clear();
			if (st->source.row_buttons)
			{
				const int count = std::min(VGUI_MAX_ROW_BUTTONS,
					std::max(0, st->source.row_buttons(st->source.user, index)));
				for (int b = 0; b < count; ++b)
				{
					vgui_texture* tex = nullptr;
					if (st->source.row_button_icon)
						st->source.row_button_icon(st->source.user, index, b, &tex);
					st->scratch_node.button_icons.push_back(tex);
				}
				st->scratch_node.button_count = count;
			}
			if (out_index) *out_index = (size_t)index;
			return &st->scratch_node;
		}
		auto it = st->index.find(id);
		if (it == st->index.end() || it->second >= st->nodes.size()) return nullptr;
		if (out_index) *out_index = it->second;
		return &st->nodes[it->second];
	}

	static vgui_tree_node* vgui_tree_node_at(vgui_widget* w, int index, vgui_item_id* out_id = nullptr)
	{
		vgui_tree_state* st = vgui_tree_of(w);
		if (!st || index < 0 || index >= (int)st->nodes.size()) return nullptr;
		if (out_id) *out_id = st->nodes[(size_t)index].id;
		return &st->nodes[(size_t)index];
	}

	static void vgui_tree_rebuild_index(vgui_tree_state* st)
	{
		if (!st) return;
		st->index.clear();
		for (size_t i = 0; i < st->nodes.size(); ++i)
			st->index[st->nodes[i].id] = i;
	}

	static void vgui_tree_rebuild_rows_rec(vgui_tree_state* st, vgui_item_id id, int depth)
	{
		vgui_tree_node* n = nullptr;
		auto it = st->index.find(id);
		if (it != st->index.end() && it->second < st->nodes.size()) n = &st->nodes[it->second];
		if (!n) return;
		st->row_index[id] = (int)st->rows.size();
		st->rows.push_back({ id, depth });
		if (!n->expanded) return;
		for (vgui_item_id c : n->children)
			vgui_tree_rebuild_rows_rec(st, c, depth + 1);
	}

	/* 过滤模式 pass 1（后序）：本行命中或任一后代命中的节点进 visible 集合。 */
	static bool vgui_tree_filter_collect(vgui_widget* w, vgui_tree_state* st,
		vgui_item_id id, std::unordered_set<vgui_item_id>& visible)
	{
		vgui_tree_node* n = vgui_tree_node_find(w, id);
		if (!n) return false;
		bool any = st->filter_fn && st->filter_fn(w, id, st->filter_user) != 0;
		for (vgui_item_id c : n->children)
			if (vgui_tree_filter_collect(w, st, c, visible)) any = true;
		if (any) visible.insert(id);
		return any;
	}

	/* 过滤模式 pass 2（先序）：只发射 visible 集合内的行，深度照常按层级。 */
	static void vgui_tree_rows_emit_filtered(vgui_widget* w, vgui_tree_state* st,
		vgui_item_id id, int depth, const std::unordered_set<vgui_item_id>& visible)
	{
		if (visible.find(id) == visible.end()) return;
		vgui_tree_node* n = vgui_tree_node_find(w, id);
		if (!n) return;
		st->row_index[id] = (int)st->rows.size();
		st->rows.push_back({ id, depth });
		for (vgui_item_id c : n->children)
			vgui_tree_rows_emit_filtered(w, st, c, depth + 1, visible);
	}

	static void vgui_tree_rebuild_rows(vgui_widget* w)
	{
		vgui_tree_state* st = vgui_tree_of(w);
		if (!st || !st->rows_dirty) return;
		// 虚拟模式下行就是索引空间本身，不做任何扁平化。
		if (st->virtual_mode)
		{
			st->rows_dirty = false;
			return;
		}
		st->rows.clear();
		st->row_index.clear();
		if (st->filter_fn)
		{
			std::unordered_set<vgui_item_id> visible;
			for (vgui_item_id id : st->roots)
				vgui_tree_filter_collect(w, st, id, visible);
			for (vgui_item_id id : st->roots)
				vgui_tree_rows_emit_filtered(w, st, id, 0, visible);
		}
		else
		{
			for (vgui_item_id id : st->roots)
				vgui_tree_rebuild_rows_rec(st, id, 0);
		}
		st->rows_dirty = false;
		// 可见行集合变了，最宽行宽度需要重算。
		st->content_w_valid = false;
	}

	// ---- 树行访问层 ----------------------------------------------------------
	// 非虚拟模式下行数据来自 rows（可见行投影）；虚拟模式下直接查回调。
	static int vgui_tree_row_count(vgui_widget* w, vgui_tree_state* st)
	{
		if (!st) return 0;
		if (st->virtual_mode)
			return st->source.row_count ? std::max(0, st->source.row_count(st->source.user)) : 0;
		vgui_tree_rebuild_rows(w);
		return (int)st->rows.size();
	}

	static vgui_item_id vgui_tree_row_id_at(vgui_widget* w, vgui_tree_state* st, int index)
	{
		if (!st || index < 0) return 0;
		if (st->virtual_mode)
			return st->source.row_id ? st->source.row_id(st->source.user, index) : 0;
		vgui_tree_rebuild_rows(w);
		return index < (int)st->rows.size() ? st->rows[(size_t)index].id : 0;
	}

	static int vgui_tree_row_depth_at(vgui_widget* w, vgui_tree_state* st, int index)
	{
		if (!st || index < 0) return 0;
		if (st->virtual_mode)
			return st->source.row_depth ? std::max(0, st->source.row_depth(st->source.user, index)) : 0;
		vgui_tree_rebuild_rows(w);
		return index < (int)st->rows.size() ? st->rows[(size_t)index].depth : 0;
	}

	static bool vgui_tree_row_has_children(vgui_widget* w, vgui_tree_state* st, int index)
	{
		if (!st || index < 0) return false;
		if (st->virtual_mode)
			return st->source.row_has_children
				? st->source.row_has_children(st->source.user, index) != 0 : false;
		vgui_tree_node* n = vgui_tree_node_find(w, vgui_tree_row_id_at(w, st, index));
		return n && !n->children.empty();
	}

	static bool vgui_tree_row_expanded(vgui_widget* w, vgui_tree_state* st, int index)
	{
		if (!st || index < 0) return false;
		if (st->virtual_mode)
			return st->source.row_expanded
				? st->source.row_expanded(st->source.user, index) != 0 : false;
		vgui_tree_node* n = vgui_tree_node_find(w, vgui_tree_row_id_at(w, st, index));
		return n && n->expanded != 0;
	}

	static bool vgui_tree_row_enabled_at(vgui_widget* w, vgui_tree_state* st, int index)
	{
		if (!st || index < 0) return false;
		if (st->virtual_mode)
			return st->source.row_enabled
				? st->source.row_enabled(st->source.user, index) != 0 : true;
		vgui_tree_node* n = vgui_tree_node_find(w, vgui_tree_row_id_at(w, st, index));
		return n && n->enabled != 0;
	}

	static int vgui_tree_visible_index(vgui_widget* w, vgui_item_id id)
	{
		vgui_tree_state* st = vgui_tree_of(w);
		if (!st || id == 0) return -1;
		if (st->virtual_mode)
		{
			if (st->source.row_id_index)
				return st->source.row_id_index(st->source.user, id);
			int n = vgui_tree_row_count(w, st);
			for (int i = 0; i < n; ++i)
				if (vgui_tree_row_id_at(w, st, i) == id) return i;
			return -1;
		}
		vgui_tree_rebuild_rows(w);
		auto it = st->row_index.find(id);
		return it == st->row_index.end() ? -1 : it->second;
	}

	static int vgui_tree_visible_count(vgui_widget* w)
	{
		vgui_tree_state* st = vgui_tree_of(w);
		if (!st) return 0;
		return vgui_tree_row_count(w, st);
	}

	static void vgui_tree_notify_select(vgui_widget* w, vgui_item_id id, int selected)
	{
		vgui_tree_state* st = vgui_tree_of(w);
		if (!st || !w->ctx || !w->ctx->alive.count(w)) return;
		int index = vgui_tree_visible_index(w, id);
		if (st->select_fn)
			st->select_fn(w, id, selected ? 1 : 0, st->select_user);
		if (!w->ctx || !w->ctx->alive.count(w)) return;
		vgui_event e = {};
		e.type = VGUI_EVENT_VALUE_CHANGED;
		e.item = id;
		e.index = index;
		vgui_emit_full(w, e);
	}

	static bool vgui_tree_set_selected_internal(vgui_widget* w, vgui_item_id id, bool selected, bool notify)
	{
		vgui_tree_state* st = vgui_tree_of(w);
		if (!st || !vgui_tree_node_find(w, id)) return false;
		bool has = vgui_id_selected(st->selected, id);
		if (selected == has) return false;
		if (selected)
		{
			if (st->selection_mode == VGUI_SELECTION_NONE) return false;
			if (st->selection_mode == VGUI_SELECTION_SINGLE)
			{
				std::vector<vgui_item_id> old = st->selected;
				st->selected.clear();
				if (notify)
					for (vgui_item_id old_id : old)
						vgui_tree_notify_select(w, old_id, 0);
				if (!w->ctx || !w->ctx->alive.count(w)) return true;
				// A selection callback may have removed the node or changed
				// the selection state.  Re-check before mutating again.
				st = vgui_tree_of(w);
				if (!st || !vgui_tree_node_find(w, id)) return true;
				if (st->selection_mode == VGUI_SELECTION_NONE) return true;
				if (vgui_id_selected(st->selected, id)) return true;
			}
			st->selected.push_back(id);
		}
		else
		{
			st->selected.erase(std::remove(st->selected.begin(), st->selected.end(), id), st->selected.end());
		}
		if (notify) vgui_tree_notify_select(w, id, selected ? 1 : 0);
		return true;
	}

	static void vgui_tree_clear_selection_internal(vgui_widget* w, bool notify)
	{
		vgui_tree_state* st = vgui_tree_of(w);
		if (!st) return;
		std::vector<vgui_item_id> old = st->selected;
		st->selected.clear();
		if (notify)
			for (vgui_item_id id : old)
			{
				vgui_tree_notify_select(w, id, 0);
				if (!w->ctx || !w->ctx->alive.count(w)) return;
			}
	}

	static void vgui_item_event(vgui_widget* w, vgui_event_type type, vgui_item_id item, int index,
		float lx, float ly, int button = 0, int clicks = 0)
	{
		if (!w || !w->ctx || !w->ctx->alive.count(w)) return;
		vgui_event e = {};
		e.type = (int)type;
		e.x = lx;
		e.y = ly;
		e.button = button;
		e.clicks = clicks;
		e.item = item;
		e.index = index;
		vgui_emit_full(w, e);
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
		t.scrollbar_hover = 0xFF727272;
		t.checkmark = 0xFFFFFFFF;
		t.switch_thumb = 0xFFFFFFFF;
		t.list_bg = t.panel_bg;
		t.list_border = t.panel_border;
		t.row_text = t.text;
		t.row_hover = 0x554A4A4A;
		t.row_selected = 0x55409EFF;
		t.row_selected_text = t.text;
		t.tree_guide = 0x553C3C3C;
		t.menu_bg = 0xFF282828;
		t.menu_border = 0xFF4A4A4A;
		t.menu_hover = 0xFF3F3F3F;
		t.menu_disabled_text = t.disabled;
		t.tooltip_bg = 0xFF303030;
		t.tooltip_border = 0xFF5A5A5A;
		t.tooltip_text = t.text;
		t.modal_backdrop = 0xFF000000;
		t.modal_bg = t.panel_bg;
		t.modal_border = t.button_border;
		t.modal_danger = 0xFFD9544D;
		t.toast_bg = t.menu_bg;
		t.toast_border = t.menu_border;
		t.toast_text = t.text;
		t.toast_info = t.accent;
		t.toast_success = 0xFF4CAF50;
		t.toast_warning = 0xFFE6A23C;
		t.toast_error = t.modal_danger;
		t.dock_bg = t.panel_bg;
		t.dock_border = t.panel_border;
		t.dock_tab_bg = t.button_bg;
		t.dock_tab_active = t.panel_bg;
		t.dock_tab_text = t.text;
		t.dock_floating_bg = t.menu_bg;
		t.dock_floating_border = t.menu_border;
		t.dock_floating_title = t.button_bg;
		return t;
	}

	static vgui_style vgui_default_style()
	{
		vgui_style s = {};
		s.animation_speed = 18.0f;
		s.text_min_subpixel = 1;
		s.disabled_text_mix = 0.65f;

		s.focus.min_size = 4.0f;
		s.focus.rounding = 4.0f;
		s.focus.inset = 0.5f;
		s.focus.width = 1.0f;

		s.panel.padding = 8.0f;
		s.panel.border_width = 1.0f;
		s.panel.rounding = 0.0f;
		s.panel.scrollbar_width = 5.0f;
		s.panel.scrollbar_inset = 2.0f;
		s.panel.scrollbar_min_thumb = 24.0f;
		s.panel.scrollbar_rounding = 2.5f;
		s.panel.scrollbar_hit_padding = 1.0f;
		s.panel.scrollbar_min_track = 20.0f;

		s.label.padding_x = 2.0f;

		s.button.padding_x = 12.0f;
		s.button.padding_y = 6.0f;
		s.button.rounding = 4.0f;
		s.button.border_width = 1.0f;
		s.button.min_height = 28.0f;
		s.button.text_extra_width = 4.0f;
		s.button.hover_mix_min = 0.25f;
		s.button.press_mix_min = 0.35f;
		s.button.press_offset = 1.0f;
		s.button.disabled_mix = 0.35f;
		s.button.icon_gap = 6.0f;

		s.choice.box_min = 14.0f;
		s.choice.box_max = 18.0f;
		s.choice.line_offset = 2.0f;
		s.choice.gap = 8.0f;

		s.checkbox.rounding = 3.0f;
		s.checkbox.border_width = 1.0f;
		s.checkbox.check_width = 2.0f;
		s.checkbox.check_start_x = 0.22f;
		s.checkbox.check_start_y = 0.52f;
		s.checkbox.check_mid_x = 0.42f;
		s.checkbox.check_mid_y = 0.72f;
		s.checkbox.check_end_x = 0.80f;
		s.checkbox.check_end_y = 0.28f;

		s.radio.border_width = 1.0f;
		s.radio.dot_scale = 0.55f;

		s.switch_.width = 36.0f;
		s.switch_.min_height = 18.0f;
		s.switch_.max_height = 22.0f;
		s.switch_.line_offset = 2.0f;
		s.switch_.min_control_height = 20.0f;
		s.switch_.thumb_inset = 2.0f;
		s.switch_.gap = 8.0f;
		s.switch_.disabled_mix = 0.5f;

		s.slider.req_width = 180.0f;
		s.slider.min_height = 24.0f;
		s.slider.track_thickness = 4.0f;
		s.slider.knob_radius = 7.0f;
		s.slider.background_alpha = 0.25f;
		s.slider.disabled_mix = 0.5f;

		s.progress.req_width = 180.0f;
		s.progress.min_height = 20.0f;
		s.progress.rounding = 3.0f;
		s.progress.disabled_mix = 0.5f;

		s.edit.padding_x = 6.0f;
		s.edit.padding_y = 4.0f;
		s.edit.rounding = 3.0f;
		s.edit.border_width = 1.0f;
		s.edit.req_width = 160.0f;
		s.edit.req_height = 30.0f;
		s.edit.min_width = 160.0f;
		s.edit.text_extra_width = 12.0f;
		s.edit.multiline_min_lines = 3;
		s.edit.selection_rounding = 2.0f;
		s.edit.selection_unfocused_alpha = 0.6f;
		s.edit.selection_tail_max_width = 6.0f;
		s.edit.selection_tail_line_ratio = 0.3f;
		s.edit.selection_min_width = 1.0f;
		s.edit.caret_width = 1.0f;
		s.edit.caret_blink_period = 1.0f;
		s.edit.caret_blink_duty = 0.55f;
		s.edit.disabled_bg_mix = 0.25f;
		s.edit.ime_underline_width = 1.0f;
		s.edit.ime_underline_min_width = 2.0f;
		s.edit.scrollbar_width = 4.0f;
		s.edit.scrollbar_inset = 2.0f;
		s.edit.scrollbar_min_thumb = 20.0f;
		s.edit.scrollbar_rounding = 2.0f;
		s.edit.scrollbar_hit_padding = 2.0f;
		s.edit.scrollbar_min_track = 20.0f;
		s.edit.scrollbar_min_cross = 8.0f;
		s.edit.scrollbar_extra_scroll = 2.0f;

		s.separator.thickness = 1.0f;

		s.list.row_height = 0.0f;
		s.list.padding_x = 4.0f;
		s.list.padding_y = 4.0f;
		s.list.rounding = 3.0f;
		s.list.req_width = 220.0f;
		s.list.req_height = 180.0f;
		s.list.disabled_mix = 0.6f;

		s.tree.row_height = 0.0f;
		s.tree.indent = 18.0f;
		s.tree.arrow_size = 9.0f;
		s.tree.arrow_gap = 4.0f;
		s.tree.padding_x = 6.0f;
		s.tree.padding_y = 4.0f;
		s.tree.rounding = 3.0f;
		s.tree.req_width = 220.0f;
		s.tree.req_height = 200.0f;
		s.tree.guide_alpha = 0.45f;
		s.tree.disabled_mix = 0.6f;

		s.menu.item_height = 28.0f;
		s.menu.padding_x = 4.0f;
		s.menu.padding_y = 4.0f;
		s.menu.item_padding_x = 8.0f;
		s.menu.check_width = 18.0f;
		s.menu.shortcut_gap = 24.0f;
		s.menu.submenu_arrow_width = 10.0f;
		s.menu.separator_height = 9.0f;
		s.menu.rounding = 4.0f;
		s.menu.min_width = 140.0f;
		s.menu.max_width = 360.0f;
		s.menu.max_height = 0.0f;
		s.menu.submenu_delay = 0.18f;
		s.menu.disabled_mix = 0.55f;

		s.menubar.item_height = 0.0f;
		s.menubar.padding_x = 4.0f;
		s.menubar.padding_y = 2.0f;
		s.menubar.item_padding_x = 10.0f;
		s.menubar.gap = 2.0f;
		s.menubar.rounding = 3.0f;
		s.menubar.req_height = 28.0f;
		s.menubar.disabled_mix = 0.55f;

		s.tooltip.delay = 0.5f;
		s.tooltip.pointer_offset_x = 14.0f;
		s.tooltip.pointer_offset_y = 20.0f;
		s.tooltip.widget_gap = 6.0f;
		s.tooltip.padding_x = 8.0f;
		s.tooltip.padding_y = 5.0f;
		s.tooltip.rounding = 4.0f;
		s.tooltip.border_width = 1.0f;
		s.tooltip.max_width = 320.0f;
		s.tooltip.margin = 4.0f;
		s.tooltip.line_gap = 1.0f;

		s.modal.margin = 48.0f;
		s.modal.dialog_width = 360.0f;
		s.modal.min_width = 220.0f;
		s.modal.max_height = 0.0f;
		s.modal.padding = 16.0f;
		s.modal.rounding = 6.0f;
		s.modal.border_width = 1.0f;
		s.modal.backdrop_alpha = 0.45f;
		s.modal.title_gap = 6.0f;
		s.modal.message_gap = 10.0f;
		s.modal.button_gap = 8.0f;
		s.modal.button_top_gap = 8.0f;

		s.toast.duration = 3.0f;
		s.toast.max_width = 360.0f;
		s.toast.padding_x = 12.0f;
		s.toast.padding_y = 8.0f;
		s.toast.rounding = 6.0f;
		s.toast.border_width = 1.0f;
		s.toast.margin = 12.0f;
		s.toast.gap = 8.0f;
		s.toast.line_gap = 1.0f;
		s.toast.max_visible = 3;

		s.dock.left_width = 200.0f;
		s.dock.right_width = 220.0f;
		s.dock.bottom_height = 180.0f;
		s.dock.gap = 4.0f;
		s.dock.tab_height = 30.0f;
		s.dock.tab_padding = 10.0f;
		s.dock.tab_gap = 2.0f;
		s.dock.tab_min_width = 64.0f;
		s.dock.tab_max_width = 180.0f;
		s.dock.close_size = 14.0f;
		s.dock.floating_title_height = 28.0f;
		s.dock.floating_width = 360.0f;
		s.dock.floating_height = 260.0f;
		s.dock.floating_min_width = 200.0f;
		s.dock.floating_min_height = 120.0f;
		s.dock.floating_rounding = 6.0f;
		s.dock.floating_border_width = 1.0f;
		s.dock.shadow = 6.0f;
		s.dock.drag_threshold = 4.0f;

		s.image.req_width = 240.0f;
		s.image.req_height = 160.0f;
		s.image.border_width = 1.0f;
		s.image.rounding = 0.0f;
		s.image.disabled_mix = 0.6f;
		return s;
	}

	static float vgui_nonneg(float v)
	{
		return v > 0.0f ? v : 0.0f;
	}

	static void vgui_sanitize_style(vgui_style* s)
	{
		if (!s) return;
		s->animation_speed = vgui_nonneg(s->animation_speed);
		if (s->text_min_subpixel < 0) s->text_min_subpixel = 0;
		s->disabled_text_mix = vgui_clampf(s->disabled_text_mix, 0.0f, 1.0f);

		s->focus.min_size = vgui_nonneg(s->focus.min_size);
		s->focus.rounding = vgui_nonneg(s->focus.rounding);
		s->focus.inset = vgui_nonneg(s->focus.inset);
		s->focus.width = vgui_nonneg(s->focus.width);

		s->panel.padding = vgui_nonneg(s->panel.padding);
		s->panel.border_width = vgui_nonneg(s->panel.border_width);
		s->panel.rounding = vgui_nonneg(s->panel.rounding);
		s->panel.scrollbar_width = vgui_nonneg(s->panel.scrollbar_width);
		s->panel.scrollbar_inset = vgui_nonneg(s->panel.scrollbar_inset);
		s->panel.scrollbar_min_thumb = vgui_nonneg(s->panel.scrollbar_min_thumb);
		s->panel.scrollbar_rounding = vgui_nonneg(s->panel.scrollbar_rounding);
		s->panel.scrollbar_hit_padding = vgui_nonneg(s->panel.scrollbar_hit_padding);
		s->panel.scrollbar_min_track = vgui_nonneg(s->panel.scrollbar_min_track);

		s->label.padding_x = vgui_nonneg(s->label.padding_x);

		s->button.padding_x = vgui_nonneg(s->button.padding_x);
		s->button.padding_y = vgui_nonneg(s->button.padding_y);
		s->button.rounding = vgui_nonneg(s->button.rounding);
		s->button.border_width = vgui_nonneg(s->button.border_width);
		s->button.min_height = vgui_nonneg(s->button.min_height);
		s->button.text_extra_width = vgui_nonneg(s->button.text_extra_width);
		s->button.hover_mix_min = vgui_clampf(s->button.hover_mix_min, 0.0f, 1.0f);
		s->button.press_mix_min = vgui_clampf(s->button.press_mix_min, 0.0f, 1.0f);
		s->button.press_offset = vgui_nonneg(s->button.press_offset);
		s->button.disabled_mix = vgui_clampf(s->button.disabled_mix, 0.0f, 1.0f);
		s->button.icon_gap = vgui_nonneg(s->button.icon_gap);

		s->choice.box_min = vgui_nonneg(s->choice.box_min);
		s->choice.box_max = vgui_nonneg(s->choice.box_max);
		if (s->choice.box_max < s->choice.box_min)
			std::swap(s->choice.box_max, s->choice.box_min);
		s->choice.line_offset = vgui_nonneg(s->choice.line_offset);
		s->choice.gap = vgui_nonneg(s->choice.gap);

		s->checkbox.rounding = vgui_nonneg(s->checkbox.rounding);
		s->checkbox.border_width = vgui_nonneg(s->checkbox.border_width);
		s->checkbox.check_width = vgui_nonneg(s->checkbox.check_width);
		s->checkbox.check_start_x = vgui_nonneg(s->checkbox.check_start_x);
		s->checkbox.check_start_y = vgui_nonneg(s->checkbox.check_start_y);
		s->checkbox.check_mid_x = vgui_nonneg(s->checkbox.check_mid_x);
		s->checkbox.check_mid_y = vgui_nonneg(s->checkbox.check_mid_y);
		s->checkbox.check_end_x = vgui_nonneg(s->checkbox.check_end_x);
		s->checkbox.check_end_y = vgui_nonneg(s->checkbox.check_end_y);

		s->radio.border_width = vgui_nonneg(s->radio.border_width);
		s->radio.dot_scale = vgui_clampf(s->radio.dot_scale, 0.0f, 1.0f);

		s->switch_.width = vgui_nonneg(s->switch_.width);
		s->switch_.min_height = vgui_nonneg(s->switch_.min_height);
		s->switch_.max_height = vgui_nonneg(s->switch_.max_height);
		if (s->switch_.max_height < s->switch_.min_height)
			std::swap(s->switch_.max_height, s->switch_.min_height);
		s->switch_.line_offset = vgui_nonneg(s->switch_.line_offset);
		s->switch_.min_control_height = vgui_nonneg(s->switch_.min_control_height);
		s->switch_.thumb_inset = vgui_nonneg(s->switch_.thumb_inset);
		s->switch_.gap = vgui_nonneg(s->switch_.gap);
		s->switch_.disabled_mix = vgui_clampf(s->switch_.disabled_mix, 0.0f, 1.0f);

		s->slider.req_width = vgui_nonneg(s->slider.req_width);
		s->slider.min_height = vgui_nonneg(s->slider.min_height);
		s->slider.track_thickness = vgui_nonneg(s->slider.track_thickness);
		s->slider.knob_radius = vgui_nonneg(s->slider.knob_radius);
		s->slider.background_alpha = vgui_clampf(s->slider.background_alpha, 0.0f, 1.0f);
		s->slider.disabled_mix = vgui_clampf(s->slider.disabled_mix, 0.0f, 1.0f);

		s->progress.req_width = vgui_nonneg(s->progress.req_width);
		s->progress.min_height = vgui_nonneg(s->progress.min_height);
		s->progress.rounding = vgui_nonneg(s->progress.rounding);
		s->progress.disabled_mix = vgui_clampf(s->progress.disabled_mix, 0.0f, 1.0f);

		s->edit.padding_x = vgui_nonneg(s->edit.padding_x);
		s->edit.padding_y = vgui_nonneg(s->edit.padding_y);
		s->edit.rounding = vgui_nonneg(s->edit.rounding);
		s->edit.border_width = vgui_nonneg(s->edit.border_width);
		s->edit.req_width = vgui_nonneg(s->edit.req_width);
		s->edit.req_height = vgui_nonneg(s->edit.req_height);
		s->edit.min_width = vgui_nonneg(s->edit.min_width);
		s->edit.text_extra_width = vgui_nonneg(s->edit.text_extra_width);
		s->edit.multiline_min_lines = std::max(1, s->edit.multiline_min_lines);
		s->edit.selection_rounding = vgui_nonneg(s->edit.selection_rounding);
		s->edit.selection_unfocused_alpha = vgui_clampf(s->edit.selection_unfocused_alpha, 0.0f, 1.0f);
		s->edit.selection_tail_max_width = vgui_nonneg(s->edit.selection_tail_max_width);
		s->edit.selection_tail_line_ratio = vgui_nonneg(s->edit.selection_tail_line_ratio);
		s->edit.selection_min_width = vgui_nonneg(s->edit.selection_min_width);
		s->edit.caret_width = vgui_nonneg(s->edit.caret_width);
		s->edit.caret_blink_period = std::max(0.001f, s->edit.caret_blink_period);
		s->edit.caret_blink_duty = vgui_clampf(s->edit.caret_blink_duty, 0.0f, 1.0f);
		s->edit.disabled_bg_mix = vgui_clampf(s->edit.disabled_bg_mix, 0.0f, 1.0f);
		s->edit.ime_underline_width = vgui_nonneg(s->edit.ime_underline_width);
		s->edit.ime_underline_min_width = vgui_nonneg(s->edit.ime_underline_min_width);
		s->edit.scrollbar_width = vgui_nonneg(s->edit.scrollbar_width);
		s->edit.scrollbar_inset = vgui_nonneg(s->edit.scrollbar_inset);
		s->edit.scrollbar_min_thumb = vgui_nonneg(s->edit.scrollbar_min_thumb);
		s->edit.scrollbar_rounding = vgui_nonneg(s->edit.scrollbar_rounding);
		s->edit.scrollbar_hit_padding = vgui_nonneg(s->edit.scrollbar_hit_padding);
		s->edit.scrollbar_min_track = vgui_nonneg(s->edit.scrollbar_min_track);
		s->edit.scrollbar_min_cross = vgui_nonneg(s->edit.scrollbar_min_cross);
		s->edit.scrollbar_extra_scroll = vgui_nonneg(s->edit.scrollbar_extra_scroll);

		s->separator.thickness = vgui_nonneg(s->separator.thickness);

		s->list.row_height = vgui_nonneg(s->list.row_height);
		s->list.padding_x = vgui_nonneg(s->list.padding_x);
		s->list.padding_y = vgui_nonneg(s->list.padding_y);
		s->list.rounding = vgui_nonneg(s->list.rounding);
		s->list.req_width = vgui_nonneg(s->list.req_width);
		s->list.req_height = vgui_nonneg(s->list.req_height);
		s->list.disabled_mix = vgui_clampf(s->list.disabled_mix, 0.0f, 1.0f);

		s->tree.row_height = vgui_nonneg(s->tree.row_height);
		s->tree.indent = vgui_nonneg(s->tree.indent);
		s->tree.arrow_size = vgui_nonneg(s->tree.arrow_size);
		s->tree.arrow_gap = vgui_nonneg(s->tree.arrow_gap);
		s->tree.padding_x = vgui_nonneg(s->tree.padding_x);
		s->tree.padding_y = vgui_nonneg(s->tree.padding_y);
		s->tree.rounding = vgui_nonneg(s->tree.rounding);
		s->tree.req_width = vgui_nonneg(s->tree.req_width);
		s->tree.req_height = vgui_nonneg(s->tree.req_height);
		s->tree.guide_alpha = vgui_clampf(s->tree.guide_alpha, 0.0f, 1.0f);
		s->tree.disabled_mix = vgui_clampf(s->tree.disabled_mix, 0.0f, 1.0f);

		s->menu.item_height = vgui_nonneg(s->menu.item_height);
		s->menu.padding_x = vgui_nonneg(s->menu.padding_x);
		s->menu.padding_y = vgui_nonneg(s->menu.padding_y);
		s->menu.item_padding_x = vgui_nonneg(s->menu.item_padding_x);
		s->menu.check_width = vgui_nonneg(s->menu.check_width);
		s->menu.shortcut_gap = vgui_nonneg(s->menu.shortcut_gap);
		s->menu.submenu_arrow_width = vgui_nonneg(s->menu.submenu_arrow_width);
		s->menu.separator_height = vgui_nonneg(s->menu.separator_height);
		s->menu.rounding = vgui_nonneg(s->menu.rounding);
		s->menu.min_width = vgui_nonneg(s->menu.min_width);
		s->menu.max_width = vgui_nonneg(s->menu.max_width);
		if (s->menu.max_width < s->menu.min_width)
			std::swap(s->menu.max_width, s->menu.min_width);
		s->menu.max_height = vgui_nonneg(s->menu.max_height);
		s->menu.submenu_delay = vgui_nonneg(s->menu.submenu_delay);
		s->menu.disabled_mix = vgui_clampf(s->menu.disabled_mix, 0.0f, 1.0f);

		s->menubar.item_height = vgui_nonneg(s->menubar.item_height);
		s->menubar.padding_x = vgui_nonneg(s->menubar.padding_x);
		s->menubar.padding_y = vgui_nonneg(s->menubar.padding_y);
		s->menubar.item_padding_x = vgui_nonneg(s->menubar.item_padding_x);
		s->menubar.gap = vgui_nonneg(s->menubar.gap);
		s->menubar.rounding = vgui_nonneg(s->menubar.rounding);
		s->menubar.req_height = vgui_nonneg(s->menubar.req_height);
		s->menubar.disabled_mix = vgui_clampf(s->menubar.disabled_mix, 0.0f, 1.0f);

		s->tooltip.delay = vgui_nonneg(s->tooltip.delay);
		s->tooltip.pointer_offset_x = vgui_nonneg(s->tooltip.pointer_offset_x);
		s->tooltip.pointer_offset_y = vgui_nonneg(s->tooltip.pointer_offset_y);
		s->tooltip.widget_gap = vgui_nonneg(s->tooltip.widget_gap);
		s->tooltip.padding_x = vgui_nonneg(s->tooltip.padding_x);
		s->tooltip.padding_y = vgui_nonneg(s->tooltip.padding_y);
		s->tooltip.rounding = vgui_nonneg(s->tooltip.rounding);
		s->tooltip.border_width = vgui_nonneg(s->tooltip.border_width);
		s->tooltip.max_width = vgui_nonneg(s->tooltip.max_width);
		s->tooltip.margin = vgui_nonneg(s->tooltip.margin);
		s->tooltip.line_gap = vgui_nonneg(s->tooltip.line_gap);

		s->modal.margin = vgui_nonneg(s->modal.margin);
		s->modal.dialog_width = vgui_nonneg(s->modal.dialog_width);
		s->modal.min_width = vgui_nonneg(s->modal.min_width);
		s->modal.max_height = vgui_nonneg(s->modal.max_height);
		s->modal.padding = vgui_nonneg(s->modal.padding);
		s->modal.rounding = vgui_nonneg(s->modal.rounding);
		s->modal.border_width = vgui_nonneg(s->modal.border_width);
		s->modal.backdrop_alpha = vgui_clampf(s->modal.backdrop_alpha, 0.0f, 1.0f);
		s->modal.title_gap = vgui_nonneg(s->modal.title_gap);
		s->modal.message_gap = vgui_nonneg(s->modal.message_gap);
		s->modal.button_gap = vgui_nonneg(s->modal.button_gap);
		s->modal.button_top_gap = vgui_nonneg(s->modal.button_top_gap);

		s->toast.duration = vgui_nonneg(s->toast.duration);
		s->toast.max_width = vgui_nonneg(s->toast.max_width);
		s->toast.padding_x = vgui_nonneg(s->toast.padding_x);
		s->toast.padding_y = vgui_nonneg(s->toast.padding_y);
		s->toast.rounding = vgui_nonneg(s->toast.rounding);
		s->toast.border_width = vgui_nonneg(s->toast.border_width);
		s->toast.margin = vgui_nonneg(s->toast.margin);
		s->toast.gap = vgui_nonneg(s->toast.gap);
		s->toast.line_gap = vgui_nonneg(s->toast.line_gap);
		if (s->toast.max_visible < 0) s->toast.max_visible = 0;

		s->image.req_width = vgui_nonneg(s->image.req_width);
		s->image.req_height = vgui_nonneg(s->image.req_height);
		s->image.border_width = vgui_nonneg(s->image.border_width);
		s->image.rounding = vgui_nonneg(s->image.rounding);
		s->image.disabled_mix = vgui_clampf(s->image.disabled_mix, 0.0f, 1.0f);

		// Styles written before these controls existed have zeroed additions.
		// Restore functional defaults while leaving explicitly supported zero
		// values (automatic row height, automatic menu height) alone.
		vgui_style def = vgui_default_style();
		if (s->list.padding_x == 0) s->list.padding_x = def.list.padding_x;
		if (s->list.padding_y == 0) s->list.padding_y = def.list.padding_y;
		if (s->list.req_width == 0) s->list.req_width = def.list.req_width;
		if (s->list.req_height == 0) s->list.req_height = def.list.req_height;
		if (s->tree.indent == 0) s->tree.indent = def.tree.indent;
		if (s->tree.arrow_size == 0) s->tree.arrow_size = def.tree.arrow_size;
		if (s->tree.arrow_gap == 0) s->tree.arrow_gap = def.tree.arrow_gap;
		if (s->tree.padding_x == 0) s->tree.padding_x = def.tree.padding_x;
		if (s->tree.padding_y == 0) s->tree.padding_y = def.tree.padding_y;
		if (s->tree.req_width == 0) s->tree.req_width = def.tree.req_width;
		if (s->tree.req_height == 0) s->tree.req_height = def.tree.req_height;
		if (s->menu.item_height == 0) s->menu.item_height = def.menu.item_height;
		if (s->menu.padding_x == 0) s->menu.padding_x = def.menu.padding_x;
		if (s->menu.padding_y == 0) s->menu.padding_y = def.menu.padding_y;
		if (s->menu.item_padding_x == 0) s->menu.item_padding_x = def.menu.item_padding_x;
		if (s->menu.check_width == 0) s->menu.check_width = def.menu.check_width;
		if (s->menu.shortcut_gap == 0) s->menu.shortcut_gap = def.menu.shortcut_gap;
		if (s->menu.submenu_arrow_width == 0) s->menu.submenu_arrow_width = def.menu.submenu_arrow_width;
		if (s->menu.separator_height == 0) s->menu.separator_height = def.menu.separator_height;
		if (s->menu.min_width == 0) s->menu.min_width = def.menu.min_width;
		if (s->menu.max_width == 0) s->menu.max_width = def.menu.max_width;
		if (s->menu.submenu_delay == 0) s->menu.submenu_delay = def.menu.submenu_delay;
		if (s->menubar.padding_x == 0) s->menubar.padding_x = def.menubar.padding_x;
		if (s->menubar.padding_y == 0) s->menubar.padding_y = def.menubar.padding_y;
		if (s->menubar.item_padding_x == 0) s->menubar.item_padding_x = def.menubar.item_padding_x;
		if (s->menubar.req_height == 0) s->menubar.req_height = def.menubar.req_height;
		// A style written before tooltips existed arrives with this sub-struct
		// completely zeroed.  Restore the defaults as a block so an intentional
		// zero delay (show immediately) is not mistaken for a missing field.
		if (s->tooltip.delay == 0 && s->tooltip.pointer_offset_x == 0 &&
			s->tooltip.pointer_offset_y == 0 && s->tooltip.widget_gap == 0 &&
			s->tooltip.padding_x == 0 && s->tooltip.padding_y == 0 &&
			s->tooltip.rounding == 0 && s->tooltip.border_width == 0 &&
			s->tooltip.max_width == 0 && s->tooltip.margin == 0 &&
			s->tooltip.line_gap == 0)
			s->tooltip = def.tooltip;
		// The modal and toast sub-structs were appended after tooltips; an
		// older style arrives with both completely zeroed.
		if (s->modal.margin == 0 && s->modal.dialog_width == 0 &&
			s->modal.min_width == 0 && s->modal.max_height == 0 &&
			s->modal.padding == 0 && s->modal.rounding == 0 &&
			s->modal.border_width == 0 && s->modal.backdrop_alpha == 0 &&
			s->modal.title_gap == 0 && s->modal.message_gap == 0 &&
			s->modal.button_gap == 0 && s->modal.button_top_gap == 0)
			s->modal = def.modal;
		if (s->toast.duration == 0 && s->toast.max_width == 0 &&
			s->toast.padding_x == 0 && s->toast.padding_y == 0 &&
			s->toast.rounding == 0 && s->toast.border_width == 0 &&
			s->toast.margin == 0 && s->toast.gap == 0 &&
			s->toast.line_gap == 0 && s->toast.max_visible == 0)
			s->toast = def.toast;
		if (s->toast.max_visible < 1) s->toast.max_visible = 1;
		// Dock style was appended last; a completely zeroed block predates it.
		if (s->dock.left_width == 0 && s->dock.right_width == 0 &&
			s->dock.bottom_height == 0 && s->dock.gap == 0 &&
			s->dock.tab_height == 0 && s->dock.tab_padding == 0 &&
			s->dock.tab_gap == 0 && s->dock.tab_min_width == 0 &&
			s->dock.tab_max_width == 0 && s->dock.close_size == 0 &&
			s->dock.floating_title_height == 0 && s->dock.floating_width == 0 &&
			s->dock.floating_height == 0 && s->dock.floating_min_width == 0 &&
			s->dock.floating_min_height == 0 && s->dock.floating_rounding == 0 &&
			s->dock.floating_border_width == 0 && s->dock.shadow == 0 &&
			s->dock.drag_threshold == 0)
			s->dock = def.dock;
		s->dock.left_width = vgui_nonneg(s->dock.left_width);
		s->dock.right_width = vgui_nonneg(s->dock.right_width);
		s->dock.bottom_height = vgui_nonneg(s->dock.bottom_height);
		s->dock.gap = vgui_nonneg(s->dock.gap);
		s->dock.tab_height = vgui_nonneg(s->dock.tab_height);
		s->dock.tab_padding = vgui_nonneg(s->dock.tab_padding);
		s->dock.tab_gap = vgui_nonneg(s->dock.tab_gap);
		s->dock.tab_min_width = vgui_nonneg(s->dock.tab_min_width);
		s->dock.tab_max_width = vgui_nonneg(s->dock.tab_max_width);
		if (s->dock.tab_max_width < s->dock.tab_min_width)
			s->dock.tab_max_width = s->dock.tab_min_width;
		s->dock.close_size = vgui_nonneg(s->dock.close_size);
		s->dock.floating_title_height = vgui_nonneg(s->dock.floating_title_height);
		s->dock.floating_width = vgui_nonneg(s->dock.floating_width);
		s->dock.floating_height = vgui_nonneg(s->dock.floating_height);
		s->dock.floating_min_width = vgui_nonneg(s->dock.floating_min_width);
		s->dock.floating_min_height = vgui_nonneg(s->dock.floating_min_height);
		s->dock.floating_rounding = vgui_nonneg(s->dock.floating_rounding);
		s->dock.floating_border_width = vgui_nonneg(s->dock.floating_border_width);
		s->dock.shadow = vgui_nonneg(s->dock.shadow);
		s->dock.drag_threshold = vgui_nonneg(s->dock.drag_threshold);
		if (s->dock.floating_width < s->dock.floating_min_width)
			s->dock.floating_width = s->dock.floating_min_width;
		if (s->dock.floating_height < s->dock.floating_min_height)
			s->dock.floating_height = s->dock.floating_min_height;
		if (s->image.req_width == 0) s->image.req_width = def.image.req_width;
		if (s->image.req_height == 0) s->image.req_height = def.image.req_height;
		if (s->image.border_width == 0) s->image.border_width = def.image.border_width;
		if (s->image.disabled_mix == 0) s->image.disabled_mix = def.image.disabled_mix;
	}

	static vgui_widget* vgui_new_widget(vgui_context* ctx, vgui_widget_kind kind, const char* text)
	{
		vgui_widget* w = new vgui_widget();
		w->ctx = ctx;
		w->kind = kind;
		if (text) w->text = text;

		const vgui_theme& th = ctx->theme;
		const vgui_style& st = ctx->style;
		w->text_color = th.text;
		w->accent = th.accent;
		w->track = th.track;

		switch (kind)
		{
		case VGUI_PANEL:
			w->bg = th.panel_bg;
			w->border = th.panel_border;
			w->border_width = st.panel.border_width;
			w->rounding = st.panel.rounding;
			w->padding[0] = w->padding[1] = w->padding[2] = w->padding[3] = st.panel.padding;
			w->clip_children = 1;
			break;
		case VGUI_LABEL:
			w->padding[0] = w->padding[2] = st.label.padding_x;
			w->text_align_x = 0.0f;
			break;
		case VGUI_BUTTON:
			w->bg = th.button_bg;
			w->border = th.button_border;
			w->border_width = st.button.border_width;
			w->rounding = st.button.rounding;
			w->padding[0] = w->padding[2] = st.button.padding_x;
			w->padding[1] = w->padding[3] = st.button.padding_y;
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
			w->req_w = st.slider.req_width;
			w->req_h = st.slider.min_height;
			w->focusable = 1;
			w->cursor_type = VGUI_CURSOR_HAND;
			break;
		case VGUI_PROGRESS:
			w->bg = 0;
			w->req_w = st.progress.req_width;
			w->req_h = st.progress.min_height;
			w->text_align_x = 0.5f;
			break;
		case VGUI_EDIT:
			w->bg = th.edit_bg;
			w->border = th.button_border;
			w->border_width = st.edit.border_width;
			w->rounding = st.edit.rounding;
			w->req_w = st.edit.req_width;
			w->req_h = st.edit.req_height;
			w->padding[0] = w->padding[2] = st.edit.padding_x;
			w->padding[1] = w->padding[3] = st.edit.padding_y;
			w->text_align_x = 0.0f;
			w->focusable = 1;
			w->cursor_type = VGUI_CURSOR_IBEAM;
			w->sel_rounding = st.edit.selection_rounding;
			w->caret_width = st.edit.caret_width;
			break;
		case VGUI_SEPARATOR:
			w->bg = th.panel_border;
			w->req_h = st.separator.thickness;
			break;
		case VGUI_LIST:
			w->list = std::make_unique<vgui_list_state>();
			w->bg = th.list_bg ? th.list_bg : th.panel_bg;
			w->border = th.list_border ? th.list_border : th.panel_border;
			w->border_width = 1.0f;
			w->rounding = st.list.rounding;
			w->req_w = st.list.req_width;
			w->req_h = st.list.req_height;
			w->padding[0] = w->padding[2] = st.list.padding_x;
			w->padding[1] = w->padding[3] = st.list.padding_y;
			w->text_align_x = 0.0f;
			w->focusable = 1;
			w->cursor_type = VGUI_CURSOR_HAND;
			w->scrollable_y = 1;
			break;
		case VGUI_TREE:
			w->tree = std::make_unique<vgui_tree_state>();
			w->bg = th.list_bg ? th.list_bg : th.panel_bg;
			w->border = th.list_border ? th.list_border : th.panel_border;
			w->border_width = 1.0f;
			w->rounding = st.tree.rounding;
			w->req_w = st.tree.req_width;
			w->req_h = st.tree.req_height;
			w->padding[0] = w->padding[2] = st.tree.padding_x;
			w->padding[1] = w->padding[3] = st.tree.padding_y;
			w->text_align_x = 0.0f;
			w->focusable = 1;
			w->cursor_type = VGUI_CURSOR_HAND;
			w->scrollable_y = 1;
			break;
		case VGUI_MENU:
			w->menu = std::make_unique<vgui_menu_state>();
			w->bg = th.menu_bg ? th.menu_bg : th.panel_bg;
			w->border = th.menu_border ? th.menu_border : th.button_border;
			w->border_width = 1.0f;
			w->rounding = st.menu.rounding;
			w->text_align_x = 0.0f;
			w->cursor_type = VGUI_CURSOR_ARROW;
			w->scrollable_y = 1;
			w->visible = 0;
			break;
		case VGUI_IMAGE:
			w->image = std::make_unique<vgui_image_state>();
			w->bg = 0;
			w->border = th.panel_border;
			w->border_width = st.image.border_width;
			w->rounding = st.image.rounding;
			// Auto-size: texture/source size when a texture is set, otherwise
			// the style's preferred size.
			w->req_w = 0.0f;
			w->req_h = 0.0f;
			w->focusable = 1;
			w->cursor_type = VGUI_CURSOR_ARROW;
			break;
		case VGUI_SPLITTER:
			w->splitter = std::make_unique<vgui_splitter_state>();
			w->bg = 0;
			w->border = th.panel_border;
			w->border_width = 0;
			w->padding[0] = w->padding[1] = w->padding[2] = w->padding[3] = 0.0f;
			w->value = 0.5f;
			w->range_min = 0.0f;
			w->range_max = 1.0f;
			w->focusable = 1;
			w->cursor_type = VGUI_CURSOR_SIZE_H;
			w->clip_children = 1;
			break;
		case VGUI_TABVIEW:
			w->tabs = std::make_unique<vgui_tab_state>();
			w->bg = th.panel_bg;
			w->border = th.panel_border;
			w->border_width = 1.0f;
			w->padding[0] = w->padding[1] = w->padding[2] = w->padding[3] = 0.0f;
			w->focusable = 1;
			w->cursor_type = VGUI_CURSOR_ARROW;
			w->clip_children = 1;
			break;
		case VGUI_MENUBAR:
			w->menubar = std::make_unique<vgui_menubar_state>();
			w->bg = 0;
			w->border = 0;
			w->border_width = 0.0f;
			w->rounding = st.menubar.rounding;
			w->padding[0] = w->padding[1] = w->padding[2] = w->padding[3] = 0.0f;
			w->req_h = st.menubar.req_height;
			w->focusable = 0;
			w->cursor_type = VGUI_CURSOR_HAND;
			w->clip_children = 1;
			break;
		case VGUI_DOCK:
			w->dock = std::make_unique<vgui_dock_state>();
			w->bg = th.dock_bg ? th.dock_bg : th.panel_bg;
			w->border = th.dock_border ? th.dock_border : th.panel_border;
			w->border_width = 1.0f;
			w->rounding = 0.0f;
			w->padding[0] = w->padding[1] = w->padding[2] = w->padding[3] = 0.0f;
			w->focusable = 1;
			w->cursor_type = VGUI_CURSOR_ARROW;
			w->clip_children = 1;
			break;
		case VGUI_CUSTOM:
			w->bg = th.panel_bg;
			w->border = th.panel_border;
			w->border_width = 1;
			w->rounding = 0;
			w->focusable = 1;
			w->cursor_type = VGUI_CURSOR_ARROW;
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

	static float vgui_menu_item_height(vgui_widget* w, const vgui_menu_item& item)
	{
		const vgui_menu_style& ms = w->ctx->style.menu;
		return item.type == VGUI_MENU_ITEM_SEPARATOR ? ms.separator_height : ms.item_height;
	}

	static bool vgui_menu_has_checkable(vgui_widget* w)
	{
		vgui_menu_state* st = vgui_menu_of(w);
		if (!st) return false;
		for (const vgui_menu_item& it : st->items)
			if (it.type != VGUI_MENU_ITEM_SEPARATOR && it.checkable) return true;
		return false;
	}

	static void vgui_menu_content_size(vgui_widget* w, float* out_w, float* out_h)
	{
		vgui_menu_state* st = vgui_menu_of(w);
		const vgui_menu_style& ms = w->ctx->style.menu;
		float width = ms.min_width;
		float height = ms.padding_y * 2.0f;
		bool any_check = vgui_menu_has_checkable(w);
		if (st)
		{
			for (const vgui_menu_item& it : st->items)
			{
				height += vgui_menu_item_height(w, it);
				if (it.type == VGUI_MENU_ITEM_SEPARATOR) continue;
				float cw = ms.padding_x * 2.0f + ms.item_padding_x * 2.0f;
				if (any_check) cw += ms.check_width;
				cw += vgui_text_width(w, it.text);
				if (!it.shortcut.empty())
					cw += ms.shortcut_gap + vgui_text_width(w, it.shortcut);
				if (it.type == VGUI_MENU_ITEM_SUBMENU)
					cw += ms.submenu_arrow_width + ms.item_padding_x;
				width = std::max(width, cw);
			}
		}
		float maxw = ms.max_width > 0 ? ms.max_width : width;
		if (st && st->width > 0) width = st->width;
		else width = std::min(width, maxw);
		width = std::max(width, ms.min_width);
		if (out_w) *out_w = width;
		if (out_h) *out_h = height;
	}

	static void vgui_image_source_size(vgui_widget* w, int* out_w, int* out_h)
	{
		int sw = 0, sh = 0;
		if (w && w->image)
		{
			vgui_texture* tex = w->image->texture;
			if (tex)
			{
				sw = w->image->src_w > 0 ? w->image->src_w : tex->width;
				sh = w->image->src_h > 0 ? w->image->src_h : tex->height;
			}
		}
		if (sw <= 0 || sh <= 0)
		{
			sw = w && w->ctx ? (int)w->ctx->style.image.req_width : 0;
			sh = w && w->ctx ? (int)w->ctx->style.image.req_height : 0;
		}
		if (out_w) *out_w = std::max(sw, 0);
		if (out_h) *out_h = std::max(sh, 0);
	}

	struct vgui_wrap_entry
	{
		vgui_widget* child;
		float x, y, w, h;
	};

	struct vgui_wrap_line
	{
		size_t begin;
		size_t end;
		float height;
	};

	// Compute a single-level wrapping layout.  OVG decides where lines break
	// (including grow), then vgui applies the final gap, per-line justify and
	// multi-line align-content.  Coordinates are relative to the content box.
	static void vgui_wrap_compute(vgui_widget* w, vgui_context* ctx, float avail_w,
		float avail_h, std::vector<vgui_wrap_entry>& out, float* out_w, float* out_h)
	{
		out.clear();
		if (out_w) *out_w = 0;
		if (out_h) *out_h = 0;
		if (!w || !ctx) return;

		std::vector<vgui_widget*> visible;
		visible.reserve(w->children.size());
		for (vgui_widget* c : w->children)
		{
			if (!c || !c->visible) continue;
			visible.push_back(c);
			out.push_back({ c, 0, 0, std::max(0.0f, vgui_child_req_w(c)),
				std::max(0.0f, vgui_child_req_h(c)) });
		}
		const size_t count = visible.size();
		if (count == 0)
			return;

		const float inner_w = std::max(avail_w, 1.0f);
		const float inner_h = std::max(avail_h, 0.0f);
		const float gap = std::max(0.0f, w->gap);
		if (!ctx->flex)
			ctx->flex = new_flex_run(nullptr);

		std::vector<node_dt> nodes(count + 1);
		std::vector<flex_data> styles(count + 1);
		nodes[0].size = { inner_w, inner_h };
		nodes[0].index = 0;
		nodes[0].child = nodes.data() + 1;
		nodes[0].child_count = count;
		styles[0].width = inner_w;
		styles[0].height = inner_h;
		styles[0].direction = flex_direction::ROW;
		styles[0].wrap = flex_wrap::WRAP;
		styles[0].justify_content = flex_align::ALIGN_START;
		styles[0].align_items = flex_align::ALIGN_START;
		styles[0].align_content = flex_align::ALIGN_START;

		for (size_t i = 0; i < count; ++i)
		{
			vgui_widget* c = visible[i];
			vgui_wrap_entry& e = out[i];
			nodes[i + 1].size = { e.w, e.h };
			nodes[i + 1].index = i + 1;
			nodes[i + 1].position = 0;
			styles[i + 1].width = e.w;
			styles[i + 1].height = e.h;
			styles[i + 1].margin_left = std::max(0.0f, c->margin[0]);
			styles[i + 1].margin_right = std::max(0.0f, c->margin[2]) + gap;
			styles[i + 1].basis = -1.0f;
			styles[i + 1].grow = std::max(0.0f, c->grow);
			styles[i + 1].shrink = 0.0f;
		}

		if (ctx->flex && nodes[0].child)
			flex_run_layout(ctx->flex, styles.data(), styles.size(), nodes.data(), nodes.size());

		// OVG positions every child of one line at the same cross-axis start
		// (align_items is START), so frame.y identifies the line.
		std::vector<vgui_wrap_line> lines;
		float current_line_y = 0.0f;
		for (size_t i = 0; i < count; ++i)
		{
			const float y = nodes[i + 1].frame.y;
			const float ww = std::max(0.0f, nodes[i + 1].frame.z);
			const float hh = std::max(0.0f, nodes[i + 1].frame.w);
			if (i == 0 || std::fabs(y - current_line_y) > 0.01f)
			{
				lines.push_back({ i, i, 0.0f });
				current_line_y = y;
			}
			vgui_wrap_entry& e = out[i];
			if (ww > 0.0f) e.w = ww;
			if (hh > 0.0f) e.h = hh;
			lines.back().end = i + 1;
			lines.back().height = std::max(lines.back().height,
				e.h + std::max(0.0f, visible[i]->margin[1]) +
				std::max(0.0f, visible[i]->margin[3]));
		}

		// Per-line grow: distribute the remaining main-axis space.
		for (const vgui_wrap_line& line : lines)
		{
			float used = 0.0f, grow_sum = 0.0f;
			for (size_t i = line.begin; i < line.end; ++i)
			{
				vgui_widget* c = visible[i];
				used += out[i].w + std::max(0.0f, c->margin[0]) + std::max(0.0f, c->margin[2]);
				grow_sum += std::max(0.0f, c->grow);
			}
			if (line.end > line.begin + 1)
				used += gap * (float)(line.end - line.begin - 1);
			float extra = inner_w - used;
			if (extra > 0.0f && grow_sum > 0.0f)
			{
				for (size_t i = line.begin; i < line.end; ++i)
				{
					float g = std::max(0.0f, visible[i]->grow);
					if (g > 0.0f) out[i].w += extra * (g / grow_sum);
				}
			}
		}

		float total_h = 0.0f;
		for (size_t i = 0; i < lines.size(); ++i)
		{
			total_h += lines[i].height;
			if (i + 1 < lines.size()) total_h += gap;
		}
		float free_h = std::max(0.0f, inner_h - total_h);
		float content_start = 0.0f;
		float line_spacing = gap;
		switch (w->align_content)
		{
		case VGUI_ALIGN_CONTENT_CENTER:
			content_start = free_h * 0.5f;
			break;
		case VGUI_ALIGN_CONTENT_END:
			content_start = free_h;
			break;
		case VGUI_ALIGN_CONTENT_SPACE_BETWEEN:
			if (lines.size() > 1) line_spacing += free_h / (float)(lines.size() - 1);
			break;
		case VGUI_ALIGN_CONTENT_SPACE_AROUND:
			content_start = free_h / (float)lines.size() * 0.5f;
			line_spacing += free_h / (float)lines.size();
			break;
		case VGUI_ALIGN_CONTENT_SPACE_EVENLY:
			content_start = free_h / (float)(lines.size() + 1);
			line_spacing += free_h / (float)(lines.size() + 1);
			break;
		case VGUI_ALIGN_CONTENT_STRETCH:
			if (free_h > 0.0f)
			{
				const float add = free_h / (float)lines.size();
				for (vgui_wrap_line& line : lines) line.height += add;
			}
			break;
		default:
			break;
		}

		float line_y = content_start;
		float max_right = 0.0f;
		for (const vgui_wrap_line& line : lines)
		{
			const size_t n = line.end - line.begin;
			float used = 0.0f;
			for (size_t i = line.begin; i < line.end; ++i)
			{
				vgui_widget* c = visible[i];
				used += out[i].w + std::max(0.0f, c->margin[0]) + std::max(0.0f, c->margin[2]);
			}
			if (n > 1) used += gap * (float)(n - 1);
			float free_x = std::max(0.0f, inner_w - used);
			float start = 0.0f, extra_gap = 0.0f;
			switch (w->justify)
			{
			case VGUI_JUSTIFY_CENTER: start = free_x * 0.5f; break;
			case VGUI_JUSTIFY_END: start = free_x; break;
			case VGUI_JUSTIFY_SPACE_BETWEEN:
				if (n > 1) extra_gap = free_x / (float)(n - 1);
				break;
			case VGUI_JUSTIFY_SPACE_AROUND:
				start = free_x / (float)n * 0.5f;
				extra_gap = free_x / (float)n;
				break;
			default: break;
			}

			float cursor = start;
			for (size_t i = line.begin; i < line.end; ++i)
			{
				vgui_widget* c = visible[i];
				vgui_wrap_entry& e = out[i];
				const float ml = std::max(0.0f, c->margin[0]);
				const float mt = std::max(0.0f, c->margin[1]);
				const float mr = std::max(0.0f, c->margin[2]);
				const float mb = std::max(0.0f, c->margin[3]);
				vgui_align al = c->align_self != VGUI_ALIGN_AUTO ? c->align_self : w->align_items;
				float cross_avail = std::max(0.0f, line.height - mt - mb);
				if (al == VGUI_ALIGN_STRETCH && c->req_h <= 0.0f)
					e.h = cross_avail;
				float cross_pos = 0.0f;
				if (al == VGUI_ALIGN_CENTER) cross_pos = (cross_avail - e.h) * 0.5f;
				else if (al == VGUI_ALIGN_END) cross_pos = cross_avail - e.h;

				e.x = cursor + ml;
				e.y = line_y + mt + std::max(0.0f, cross_pos);
				max_right = std::max(max_right, e.x + e.w + mr);
				cursor += ml + e.w + mr + gap + extra_gap;
			}
			line_y += line.height + line_spacing;
		}

		if (out_w) *out_w = std::max(max_right, inner_w);
		if (out_h) *out_h = std::max(0.0f, line_y - line_spacing);
	}

	// 列表最宽行宽度（带缓存）。content_w 只在自动宽度或横向滚动时才被
	// 消费，但逐条测量在万行列表上是每次布局 O(N)：文本和字体不变时复用
	// 上次结果，只有模型变更点负责把 content_w_valid 置 false。
	static float vgui_list_content_width(vgui_widget* w, vgui_list_state* st)
	{
		if (!w || !st) return 0.0f;
		// 虚拟模式无法全量扫描：由数据源提供最宽行，缺省回退到主题宽度。
		if (st->virtual_mode)
		{
			if (st->source.row_max_width)
				return std::max(0.0f, st->source.row_max_width(st->source.user));
			return w->ctx ? w->ctx->style.list.req_width : 0.0f;
		}
		const font_familys_t* font = w->ctx ? w->ctx->font : nullptr;
		float fs = vgui_font_size(w->ctx, w);
		if (st->content_w_valid && st->content_w_font == font && st->content_w_font_size == fs)
			return st->content_w;

		float cw = 0.0f;
		for (const vgui_list_item& it : st->items)
		{
			float iw = vgui_text_width(w, it.text);
			if (!it.detail.empty())
				iw += 16.0f + vgui_text_width(w, it.detail);
			cw = std::max(cw, iw);
		}
		st->content_w = cw;
		st->content_w_font = font;
		st->content_w_font_size = fs;
		st->content_w_valid = true;
		return cw;
	}

	// 树最宽可见行宽度（带缓存）。可见行集合由 rows_dirty 失效，缩进和
	// 箭头尺寸来自主题，一并作为缓存键。
	static float vgui_tree_content_width(vgui_widget* w, vgui_tree_state* st)
	{
		if (!w || !st) return 0.0f;
		// 虚拟模式无法全量扫描：由数据源提供最宽行，缺省回退到主题宽度。
		if (st->virtual_mode)
		{
			if (st->source.row_max_width)
				return std::max(0.0f, st->source.row_max_width(st->source.user));
			return w->ctx ? w->ctx->style.tree.req_width : 0.0f;
		}
		vgui_tree_rebuild_rows(w);
		const font_familys_t* font = w->ctx ? w->ctx->font : nullptr;
		float fs = vgui_font_size(w->ctx, w);
		float indent = w->ctx ? w->ctx->style.tree.indent : 0.0f;
		float arrow = w->ctx ? w->ctx->style.tree.arrow_size + w->ctx->style.tree.arrow_gap : 0.0f;
		if (st->content_w_valid && st->content_w_font == font &&
			st->content_w_font_size == fs && st->content_w_indent == indent &&
			st->content_w_arrow == arrow)
			return st->content_w;

		float cw = 0.0f;
		for (const vgui_tree_row& row : st->rows)
		{
			vgui_tree_node* n = vgui_tree_node_find(w, row.id);
			if (!n) continue;
			float iw = row.depth * indent + arrow + vgui_text_width(w, n->text);
			cw = std::max(cw, iw);
		}
		st->content_w = cw;
		st->content_w_font = font;
		st->content_w_font_size = fs;
		st->content_w_indent = indent;
		st->content_w_arrow = arrow;
		st->content_w_valid = true;
		return cw;
	}

	static void vgui_measure_widget(vgui_widget* w, vgui_context* ctx)
	{
		if (!w) return;
		float lh = vgui_line_height(ctx, w);
		float tw = vgui_text_width(w, w->text);
		float pl = w->padding[0], pt = w->padding[1], pr = w->padding[2], pb = w->padding[3];
		const vgui_style& st = ctx->style;

		switch (w->kind)
		{
		case VGUI_PANEL:
		{
			float content_w = 0, content_h = 0;
			float row_w = 0, row_h = 0, col_w = 0, col_h = 0;
			float wrap_w = 0;
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
				else if (w->layout == VGUI_LAYOUT_WRAP)
				{
					wrap_w = std::max(wrap_w, cw);
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
			else if (w->layout == VGUI_LAYOUT_WRAP)
			{
				float avail = w->req_w > 0 ? std::max(0.0f, w->req_w - pl - pr) : wrap_w;
				std::vector<vgui_wrap_entry> entries;
				float measured_w = 0.0f, measured_h = 0.0f;
				vgui_wrap_compute(w, ctx, avail, 0.0f, entries, &measured_w, &measured_h);
				content_w = w->req_w > 0 ? avail : measured_w;
				content_h = measured_h;
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
		{
			float iw = 0, ih = 0;
			float gap = w->icon_gap > 0 ? w->icon_gap : st.button.icon_gap;
			bool horiz = (w->icon_pos == VGUI_BUTTON_ICON_LEFT || w->icon_pos == VGUI_BUTTON_ICON_RIGHT);
			if (w->icon_tex && w->icon_tex->width > 0)
			{
				iw = w->icon_w > 0 ? w->icon_w : (float)w->icon_tex->width;
				ih = w->icon_h > 0 ? w->icon_h : (float)w->icon_tex->height;
			}
			bool has_icon = iw > 0 && ih > 0;
			bool has_text = !w->text.empty();
			float extra = st.button.text_extra_width;
			if (has_icon)
			{
				if (horiz)
					w->intrinsic_w = pl + pr + std::max(tw + extra + (has_text ? gap : 0), 0.0f) + iw;
				else
					w->intrinsic_w = pl + pr + std::max(tw + extra, iw);

				if (horiz)
					w->intrinsic_h = std::max(std::max(lh, ih) + pt + pb, st.button.min_height);
				else
					w->intrinsic_h = pt + pb + ih + (has_text ? gap : 0) + std::max(lh, 0.0f);
			}
			else
			{
				w->intrinsic_w = tw + pl + pr + extra;
				w->intrinsic_h = std::max(lh + pt + pb, st.button.min_height);
			}
		}
		break;
		case VGUI_CHECKBOX:
		case VGUI_RADIO:
		{
			float box = std::max(st.choice.box_min, std::min(st.choice.box_max, lh - st.choice.line_offset));
			float label = w->text.empty() ? 0 : (tw + st.choice.gap);
			w->intrinsic_w = box + label + pl + pr;
			w->intrinsic_h = std::max(box, lh) + pt + pb;
		}
		break;
		case VGUI_SWITCH:
		{
			float track = st.switch_.width;
			float label = w->text.empty() ? 0 : (tw + st.switch_.gap);
			w->intrinsic_w = track + label + pl + pr;
			w->intrinsic_h = std::max(st.switch_.min_control_height, lh) + pt + pb;
		}
		break;
		case VGUI_SLIDER:
			w->intrinsic_w = w->req_w > 0 ? w->req_w : st.slider.req_width;
			w->intrinsic_h = std::max(w->req_h, st.slider.min_height);
			break;
		case VGUI_PROGRESS:
			w->intrinsic_w = w->req_w > 0 ? w->req_w : st.progress.req_width;
			w->intrinsic_h = std::max(w->req_h, st.progress.min_height);
			break;
		case VGUI_EDIT:
		{
			float minw = w->text.empty() ? st.edit.min_width
				: std::max(st.edit.min_width, tw + pl + pr + st.edit.text_extra_width);
			w->intrinsic_w = w->req_w > 0 ? w->req_w : minw;
			float lines = w->multiline
				? (float)std::max(st.edit.multiline_min_lines, vgui_text_lines(w->edit_text))
				: 1.0f;
			w->intrinsic_h = w->req_h > 0 ? w->req_h : (lh * lines + pt + pb);
		}
		break;
		case VGUI_SEPARATOR:
			w->intrinsic_w = w->req_w;
			w->intrinsic_h = w->req_h > 0 ? w->req_h : st.separator.thickness;
			break;
		case VGUI_LIST:
		{
			vgui_list_state* ls = vgui_list_of(w);
			float row_h = vgui_list_row_height(w);
			/* content_w 只在自动宽度（req_w==0）或启用横向滚动时才会被消费：
			 * 逐条测量文本宽度在大列表上是每帧 O(N)。其余情况下 content_w
			 * 保持 0，vgui_panel_get_content_size 也随之返回 0。 */
			float content_w = 0;
			if (ls && (w->req_w <= 0.0f || w->scrollable_x))
				content_w = vgui_list_content_width(w, ls);
			w->content_w = content_w;
			w->content_h = (float)vgui_list_row_count(w, ls) * row_h;
			float default_w = std::max(st.list.req_width, content_w + pl + pr + 12.0f);
			w->intrinsic_w = w->req_w > 0 ? w->req_w : default_w;
			w->intrinsic_h = w->req_h > 0 ? w->req_h : st.list.req_height;
		}
		break;
		case VGUI_TREE:
		{
			vgui_tree_state* ts = vgui_tree_of(w);
			float row_h = vgui_tree_row_height(w);
			vgui_tree_rebuild_rows(w);
			/* 同 VGUI_LIST：没有自动宽度需求且未开横向滚动时跳过全行文本测量。 */
			float content_w = 0;
			if (ts && (w->req_w <= 0.0f || w->scrollable_x))
				content_w = vgui_tree_content_width(w, ts);
			w->content_w = content_w;
			w->content_h = (float)vgui_tree_row_count(w, ts) * row_h;
			float default_w = std::max(st.tree.req_width, content_w + pl + pr + 12.0f);
			w->intrinsic_w = w->req_w > 0 ? w->req_w : default_w;
			w->intrinsic_h = w->req_h > 0 ? w->req_h : st.tree.req_height;
		}
		break;
		case VGUI_MENU:
		{
			float cw = 0, ch = 0;
			vgui_menu_content_size(w, &cw, &ch);
			w->content_w = cw;
			w->content_h = ch;
			w->intrinsic_w = cw;
			w->intrinsic_h = ch;
		}
		break;
		case VGUI_MENUBAR:
		{
			const vgui_menubar_style& ms = ctx->style.menubar;
			float lh = vgui_line_height(ctx, w);
			float content_w = vgui_menubar_content_width(w);
			float content_h = ms.item_height > 0 ? ms.item_height : lh + ms.padding_y * 2.0f;
			w->content_w = content_w + ms.padding_x * 2.0f;
			w->content_h = content_h + ms.padding_y * 2.0f;
			w->intrinsic_w = w->req_w > 0 ? w->req_w : w->content_w;
			w->intrinsic_h = w->req_h > 0 ? w->req_h : w->content_h;
		}
		break;
		case VGUI_IMAGE:
		{
			int sw = 0, sh = 0;
			vgui_image_source_size(w, &sw, &sh);
			w->content_w = (float)sw;
			w->content_h = (float)sh;
			w->intrinsic_w = w->req_w > 0 ? w->req_w : (float)sw;
			w->intrinsic_h = w->req_h > 0 ? w->req_h : (float)sh;
		}
		break;
		case VGUI_SPLITTER:
		{
			vgui_splitter_state* st = vgui_splitter_of(w);
			float main_size = 0.0f, cross_size = 0.0f;
			int count = 0;
			for (vgui_widget* c : w->children)
			{
				if (!c->visible) continue;
				vgui_measure_widget(c, ctx);
				float cw = vgui_child_req_w(c);
				float ch = vgui_child_req_h(c);
				main_size += w->vertical ? ch : cw;
				cross_size = std::max(cross_size, w->vertical ? cw : ch);
				count++;
			}
			if (count > 1 && st) main_size += st->sash_size;
			w->intrinsic_w = (w->vertical ? cross_size : main_size) + pl + pr;
			w->intrinsic_h = (w->vertical ? main_size : cross_size) + pt + pb;
		}
		break;
		case VGUI_TABVIEW:
		{
			vgui_tab_state* st = vgui_tabs_of(w);
			float content_w = 0.0f, content_h = 0.0f;
			if (st)
			{
				for (const vgui_tab_item& tab : st->tabs)
				{
					if (!tab.page) continue;
					vgui_measure_widget(tab.page, ctx);
					content_w = std::max(content_w, vgui_child_req_w(tab.page));
					content_h = std::max(content_h, vgui_child_req_h(tab.page));
				}
			}
			float tab_w = vgui_tab_bar_width(w);
			float tab_h = st ? st->tab_height : 0.0f;
			w->content_w = std::max(content_w, tab_w);
			w->content_h = content_h + tab_h;
			w->intrinsic_w = w->req_w > 0 ? w->req_w : (std::max(content_w, tab_w) + pl + pr);
			w->intrinsic_h = w->req_h > 0 ? w->req_h : (content_h + tab_h + pt + pb);
		}
		break;
		case VGUI_DOCK:
		{
			vgui_dock_state* st = vgui_dock_of(w);
			const vgui_dock_style& ds = ctx->style.dock;
			float content_w = 0.0f, content_h = 0.0f;
			if (st)
			{
				for (const vgui_dock_panel& panel : st->panels)
				{
					if (!panel.page) continue;
					vgui_measure_widget(panel.page, ctx);
					content_w = std::max(content_w, vgui_child_req_w(panel.page));
					content_h = std::max(content_h, vgui_child_req_h(panel.page));
				}
			}
			w->content_w = std::max(content_w, ds.left_width + ds.right_width + 120.0f);
			w->content_h = std::max(content_h + ds.tab_height, ds.bottom_height + 120.0f);
			w->intrinsic_w = w->req_w > 0 ? w->req_w : w->content_w;
			w->intrinsic_h = w->req_h > 0 ? w->req_h : w->content_h;
		}
		break;
		case VGUI_CUSTOM:
			if (w->custom_size)
			{
				float cw = 0, ch = 0;
				w->custom_size(w, &cw, &ch, w->custom_size_user);
				w->intrinsic_w = w->req_w > 0 ? w->req_w : cw;
				w->intrinsic_h = w->req_h > 0 ? w->req_h : ch;
			}
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
		else if (w->layout == VGUI_LAYOUT_WRAP)
		{
			std::vector<vgui_wrap_entry> entries;
			float cw = 0.0f, ch = 0.0f;
			vgui_wrap_compute(w, ctx, iw, ih, entries, &cw, &ch);
			/* RTL wraps each line from the right.  The line breaking itself is
			 * direction-independent, so mirroring the finished x positions keeps
			 * every line, grow and justify result intact. */
			bool rtl = vgui_is_rtl(ctx);
			for (vgui_wrap_entry& e : entries)
			{
				float ex = rtl ? iw - e.x - e.w : e.x;
				placements.push_back({ e.child, ix + ex, iy + e.y, e.w, e.h });
			}
			content_w = std::max(content_w, cw);
			content_h = std::max(content_h, ch);
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
			std::vector<float> main_sizes;
			main_sizes.reserve(visible.size());
			for (vgui_widget* c : visible)
			{
				float cw = vgui_child_req_w(c);
				float ch = vgui_child_req_h(c);
				float main = row ? cw : ch;
				float cross = row ? ch : cw;
				main_sizes.push_back(main);
				total_main += main + (row ? c->margin[0] + c->margin[2] : c->margin[1] + c->margin[3]);
				max_cross = std::max(max_cross, cross + (row ? c->margin[1] + c->margin[3] : c->margin[0] + c->margin[2]));
				if (c->grow > 0) grow_sum += c->grow;
			}
			if (n > 1) total_main += w->gap * (n - 1);

			/* Row layouts mirror their main axis in RTL.  When the content is
			 * wider than the container it keeps LTR order so that horizontal
			 * scrolling, which is always left-anchored, cannot produce negative
			 * coordinates. */
			bool rtl_row = row && vgui_is_rtl(ctx) &&
				total_main <= inner_main + 0.01f;

			// A grow item is flexible: when its intrinsic main-axis size no longer
			// fits the container, shrink it back to the available space instead of
			// letting it overflow the parent.  Without this an edit with a long
			// line grows wider than its container, so it never shows a horizontal
			// scrollbar and paints outside the panel.
			float leftover = inner_main - total_main;
			if (leftover < 0 && grow_sum > 0)
			{
				float shrinkable_sum = 0;
				for (size_t i = 0; i < visible.size(); ++i)
				{
					vgui_widget* c = visible[i];
					if (c->grow <= 0) continue;
					float limit = row ? c->min_w : c->min_h;
					limit = std::max(0.0f, limit);
					if (main_sizes[i] > limit) shrinkable_sum += main_sizes[i] - limit;
				}
				float need = std::min(-leftover, shrinkable_sum);
				if (need > 0)
				{
					for (size_t i = 0; i < visible.size(); ++i)
					{
						vgui_widget* c = visible[i];
						if (c->grow <= 0) continue;
						float limit = row ? c->min_w : c->min_h;
						limit = std::max(0.0f, limit);
						float shrinkable = std::max(0.0f, main_sizes[i] - limit);
						if (shrinkable <= 0) continue;
						main_sizes[i] -= need * (shrinkable / shrinkable_sum);
					}
					total_main -= need;
				}
			}
			float start = 0, extra_gap = 0;
			if (grow_sum <= 0 && n > 0)
			{
				/* The mirrored cursor below already measures from the right, so
				 * START keeps starting at the start edge (the right one in RTL). */
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
			for (size_t ci = 0; ci < visible.size(); ++ci)
			{
				vgui_widget* c = visible[ci];
				float cw = vgui_child_req_w(c);
				float ch = vgui_child_req_h(c);
				float main = main_sizes[ci];
				if (grow_sum > 0 && leftover > 0)
					main += leftover * (c->grow / grow_sum);

				vgui_align al = c->align_self != VGUI_ALIGN_AUTO ? c->align_self : w->align_items;
				// The cross axis of a column layout is horizontal, so START/END
				// swap in RTL.  Row cross alignment is vertical and unaffected.
				if (vgui_is_rtl(ctx) && !row)
				{
					if (al == VGUI_ALIGN_START) al = VGUI_ALIGN_END;
					else if (al == VGUI_ALIGN_END) al = VGUI_ALIGN_START;
				}
				float cross_avail = inner_cross - (row ? c->margin[1] + c->margin[3] : c->margin[0] + c->margin[2]);
				float cross = row ? ch : cw;
				float cross_pos = 0;
				if (al == VGUI_ALIGN_STRETCH && (row ? c->req_h <= 0 : c->req_w <= 0))
					cross = cross_avail;
				if (al == VGUI_ALIGN_CENTER) cross_pos = (cross_avail - cross) * 0.5f;
				else if (al == VGUI_ALIGN_END) cross_pos = cross_avail - cross;

				float m_start = rtl_row ? c->margin[2] : c->margin[0];
				float m_end = rtl_row ? c->margin[0] : c->margin[2];

				float px, py, pw, ph;
				if (row)
				{
					if (rtl_row)
						px = ix + inner_main - cursor - m_start - main;
					else
						px = ix + cursor + m_start;
					py = iy + c->margin[1] + cross_pos;
					pw = main;
					ph = cross;
					cursor += main + m_start + m_end + w->gap + extra_gap;
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
				if (rtl_row)
					content_w = std::max(content_w, inner_main - (px - ix) + m_end);
				else
					content_w = std::max(content_w, px - ix + pw + m_end);
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

	static void vgui_layout_pane(vgui_widget* child, vgui_context* ctx,
		float x, float y, float ww, float hh)
	{
		// A forced zero size means "collapsed", not "use the intrinsic size"
		// (vgui_layout_widget() treats non-positive force values as auto).
		if (!child) return;
		if (ww > 0.0f && hh > 0.0f)
		{
			vgui_layout_widget(child, ctx, x, y, ww, hh);
			return;
		}
		child->abs_x = x;
		child->abs_y = y;
		child->abs_w = std::max(0.0f, ww);
		child->abs_h = std::max(0.0f, hh);
	}

	static void vgui_layout_splitter(vgui_widget* w, vgui_context* ctx, float x, float y, float ww, float hh)
	{
		vgui_splitter_state* st = vgui_splitter_of(w);
		if (!st) return;
		float ix = x + w->padding[0];
		float iy = y + w->padding[1];
		float iw = std::max(0.0f, ww - w->padding[0] - w->padding[2]);
		float ih = std::max(0.0f, hh - w->padding[1] - w->padding[3]);
		w->content_w = iw;
		w->content_h = ih;

		std::vector<vgui_widget*> visible;
		for (vgui_widget* c : w->children)
			if (c->visible) visible.push_back(c);
		st->sash_x = st->sash_y = st->sash_w = st->sash_h = 0.0f;
		if (visible.empty()) return;
		if (visible.size() == 1)
		{
			vgui_layout_pane(visible[0], ctx, ix, iy, iw, ih);
			return;
		}

		vgui_widget* first = visible[0];
		vgui_widget* second = visible[1];
		bool vertical = w->vertical != 0;
		float main = vertical ? ih : iw;
		float sash = std::max(0.0f, st->sash_size);
		if (sash > main) sash = main;
		float avail = std::max(0.0f, main - sash);

		float min0 = vertical ? first->min_h : first->min_w;
		float min1 = vertical ? second->min_h : second->min_w;
		min0 = vgui_clampf(min0, 0.0f, avail);
		min1 = vgui_clampf(min1, 0.0f, avail);
		if (min0 + min1 > avail && min0 + min1 > 0.0f)
		{
			float scale = avail / (min0 + min1);
			min0 *= scale;
			min1 *= scale;
		}

		float ratio = vgui_clampf(w->value, 0.0f, 1.0f);
		float first_size = avail * ratio;
		float lo = min0;
		float hi = std::max(lo, avail - min1);
		first_size = vgui_clampf(first_size, lo, hi);
		float second_size = std::max(0.0f, avail - first_size);

		if (vertical)
		{
			vgui_layout_pane(first, ctx, ix, iy, iw, first_size);
			vgui_layout_pane(second, ctx, ix, iy + first_size + sash, iw, second_size);
			st->sash_x = ix;
			st->sash_y = iy + first_size;
			st->sash_w = iw;
			st->sash_h = sash;
		}
		else
		{
			vgui_layout_pane(first, ctx, ix, iy, first_size, ih);
			vgui_layout_pane(second, ctx, ix + first_size + sash, iy, second_size, ih);
			st->sash_x = ix + first_size;
			st->sash_y = iy;
			st->sash_w = sash;
			st->sash_h = ih;
		}
	}

	static void vgui_tab_update_pages(vgui_widget* w)
	{
		vgui_tab_state* st = vgui_tabs_of(w);
		if (!st) return;
		vgui_tab_item* active = vgui_tab_find(w, st->active);
		vgui_widget* active_page = active ? active->page : nullptr;
		for (vgui_widget* c : w->children)
			c->visible = (c == active_page) ? 1 : 0;
		if (w->ctx) w->ctx->layout_dirty = true;
	}

	static void vgui_tab_ensure_visible(vgui_widget* w, vgui_item_id item)
	{
		vgui_tab_state* st = vgui_tabs_of(w);
		int index = vgui_tab_find_index(w, item);
		if (!st || index < 0 || w->abs_w <= 0.0f) return;
		float ix = w->abs_x + w->padding[0];
		float iw = std::max(0.0f, w->abs_w - w->padding[0] - w->padding[2]);
		float tx = 0, ty = 0, tw = 0, th = 0;
		if (!vgui_tab_rect(w, index, &tx, &ty, &tw, &th)) return;
		float max_scroll = std::max(0.0f, vgui_tab_bar_width(w) - iw);
		if (tx < ix)
			w->scroll_x = vgui_clampf(w->scroll_x - (ix - tx), 0.0f, max_scroll);
		else if (tx + tw > ix + iw)
			w->scroll_x = vgui_clampf(w->scroll_x + (tx + tw - (ix + iw)), 0.0f, max_scroll);
		if (w->ctx) w->ctx->layout_dirty = true;
	}

	static bool vgui_tab_set_active_internal(vgui_widget* w, vgui_item_id item, bool notify)
	{
		vgui_tab_state* st = vgui_tabs_of(w);
		if (!st) return false;
		vgui_tab_item* tab = vgui_tab_find(w, item);
		if (!tab) return false;
		bool changed = (st->active != item);
		st->active = item;
		vgui_tab_update_pages(w);
		vgui_tab_ensure_visible(w, item);
		if (w->ctx) w->ctx->layout_dirty = true;
		if (notify && changed && st->select_fn)
			st->select_fn(w, item, st->select_user);
		return true;
	}

	static bool vgui_tab_move_internal(vgui_widget* w, vgui_item_id item, int index)
	{
		vgui_tab_state* st = vgui_tabs_of(w);
		int current = vgui_tab_find_index(w, item);
		if (!st || current < 0) return false;
		int target = index;
		if (target < 0) target = 0;
		if (target >= (int)st->tabs.size()) target = (int)st->tabs.size() - 1;
		if (target == current) return true;
		vgui_tab_item moved = st->tabs[(size_t)current];
		st->tabs.erase(st->tabs.begin() + current);
		st->tabs.insert(st->tabs.begin() + target, moved);
		st->index.clear();
		for (size_t i = 0; i < st->tabs.size(); ++i)
			st->index[st->tabs[i].id] = i;
		if (w->ctx) w->ctx->layout_dirty = true;
		return true;
	}

	static void vgui_tab_request_close(vgui_widget* w, vgui_item_id item)
	{
		vgui_tab_state* st = vgui_tabs_of(w);
		vgui_tab_item* tab = vgui_tab_find(w, item);
		if (!st || !tab || !tab->closeable) return;
		if (st->close_fn)
		{
			st->close_fn(w, item, st->close_user);
			return;
		}
		vgui_tab_remove(w, item);
	}

	static void vgui_layout_tabview(vgui_widget* w, vgui_context* ctx, float x, float y, float ww, float hh)
	{
		vgui_tab_state* st = vgui_tabs_of(w);
		if (!st) return;
		float ix = x + w->padding[0];
		float iy = y + w->padding[1];
		float iw = std::max(0.0f, ww - w->padding[0] - w->padding[2]);
		float ih = std::max(0.0f, hh - w->padding[1] - w->padding[3]);
		float tab_h = std::min(st->tab_height, ih);
		float content_h = std::max(0.0f, ih - tab_h);
		float tab_w = vgui_tab_bar_width(w);
		float max_scroll = std::max(0.0f, tab_w - iw);
		w->scroll_x = vgui_clampf(w->scroll_x, 0.0f, max_scroll);
		w->content_w = std::max(tab_w, iw);
		w->content_h = content_h + tab_h;

		vgui_tab_item* active = vgui_tab_find(w, st->active);
		vgui_widget* active_page = active ? active->page : nullptr;
		for (vgui_widget* c : w->children)
		{
			c->visible = (c == active_page) ? 1 : 0;
			if (c->visible)
				vgui_layout_pane(c, ctx, ix, iy + tab_h, iw, content_h);
		}
	}

	static void vgui_layout_dock(vgui_widget* w, vgui_context* ctx, float x, float y, float ww, float hh)
	{
		vgui_dock_state* st = vgui_dock_of(w);
		if (!st) return;
		const vgui_dock_style& ds = ctx->style.dock;
		float ix = x + w->padding[0];
		float iy = y + w->padding[1];
		float iw = std::max(0.0f, ww - w->padding[0] - w->padding[2]);
		float ih = std::max(0.0f, hh - w->padding[1] - w->padding[3]);
		float gap = ds.gap;
		w->content_w = iw;
		w->content_h = ih;

		for (vgui_dock_panel& panel : st->panels)
			if (panel.page) panel.page->visible = 0;

		for (int z = 0; z < 4; ++z)
		{
			st->zone_x[z] = st->zone_y[z] = st->zone_w[z] = st->zone_h[z] = 0.0f;
			st->tabs[z].clear();
		}

		bool has_left = vgui_dock_zone_has_open(st, VGUI_DOCK_LEFT);
		bool has_right = vgui_dock_zone_has_open(st, VGUI_DOCK_RIGHT);
		bool has_bottom = vgui_dock_zone_has_open(st, VGUI_DOCK_BOTTOM);
		float left_w = has_left ? std::max(0.0f, vgui_dock_resolved_zone_size(w, VGUI_DOCK_LEFT)) : 0.0f;
		float right_w = has_right ? std::max(0.0f, vgui_dock_resolved_zone_size(w, VGUI_DOCK_RIGHT)) : 0.0f;
		float bottom_h = has_bottom ? std::max(0.0f, vgui_dock_resolved_zone_size(w, VGUI_DOCK_BOTTOM)) : 0.0f;

		if (left_w + right_w + gap * 2.0f > iw)
		{
			right_w = std::max(0.0f, iw - left_w - gap * 2.0f);
			if (left_w + right_w + gap * 2.0f > iw)
				left_w = std::max(0.0f, iw - right_w - gap * 2.0f);
		}
		if (bottom_h > ih) bottom_h = ih;

		if (left_w > 0.0f)
		{
			st->zone_x[VGUI_DOCK_LEFT] = ix;
			st->zone_y[VGUI_DOCK_LEFT] = iy;
			st->zone_w[VGUI_DOCK_LEFT] = left_w;
			st->zone_h[VGUI_DOCK_LEFT] = ih;
		}
		if (right_w > 0.0f)
		{
			st->zone_x[VGUI_DOCK_RIGHT] = ix + iw - right_w;
			st->zone_y[VGUI_DOCK_RIGHT] = iy;
			st->zone_w[VGUI_DOCK_RIGHT] = right_w;
			st->zone_h[VGUI_DOCK_RIGHT] = ih;
		}

		float mid_x = ix + (left_w > 0.0f ? left_w + gap : 0.0f);
		float mid_w = iw - (left_w > 0.0f ? left_w + gap : 0.0f)
			- (right_w > 0.0f ? right_w + gap : 0.0f);
		mid_w = std::max(0.0f, mid_w);
		float mid_h = ih - (bottom_h > 0.0f ? bottom_h + gap : 0.0f);
		mid_h = std::max(0.0f, mid_h);

		if (bottom_h > 0.0f)
		{
			st->zone_x[VGUI_DOCK_BOTTOM] = mid_x;
			st->zone_y[VGUI_DOCK_BOTTOM] = iy + ih - bottom_h;
			st->zone_w[VGUI_DOCK_BOTTOM] = mid_w;
			st->zone_h[VGUI_DOCK_BOTTOM] = bottom_h;
		}
		st->zone_x[VGUI_DOCK_CENTER] = mid_x;
		st->zone_y[VGUI_DOCK_CENTER] = iy;
		st->zone_w[VGUI_DOCK_CENTER] = mid_w;
		st->zone_h[VGUI_DOCK_CENTER] = mid_h;

		for (int zone = 0; zone < 4; ++zone)
		{
			bool has_open = vgui_dock_zone_has_open(st, zone);
			vgui_item_id active = has_open ? vgui_dock_pick_active(st, zone) : 0;
			st->active[zone] = active;
			if (!has_open || st->zone_w[zone] <= 0.0f || st->zone_h[zone] <= 0.0f)
				continue;

			float tab_h = std::min(ds.tab_height, st->zone_h[zone]);
			if (tab_h < 0.0f) tab_h = 0.0f;

			std::vector<vgui_item_id> ids;
			std::vector<float> widths;
			float total_w = 0.0f;
			for (vgui_item_id id : st->order[zone])
			{
				vgui_dock_panel* panel = vgui_dock_find(w, id);
				if (!panel || !panel->open) continue;
				float tw = vgui_dock_tab_item_width(w, *panel);
				ids.push_back(id);
				widths.push_back(tw);
				total_w += tw;
			}
			if (ids.size() > 1)
				total_w += ds.tab_gap * (float)(ids.size() - 1);

			float max_scroll = std::max(0.0f, total_w - st->zone_w[zone]);
			float active_x = 0.0f, active_w = 0.0f;
			for (size_t i = 0; i < ids.size(); ++i)
			{
				if (ids[i] == active)
				{
					active_x = (i > 0 ? ds.tab_gap * (float)i : 0.0f);
					for (size_t j = 0; j < i; ++j) active_x += widths[j];
					active_w = widths[i];
					break;
				}
			}
			if (st->scroll_active[zone] && active != 0 && active_w > 0.0f)
			{
				if (active_x - st->tab_scroll[zone] < 0.0f)
					st->tab_scroll[zone] = active_x;
				else if (active_x - st->tab_scroll[zone] + active_w > st->zone_w[zone])
					st->tab_scroll[zone] = active_x + active_w - st->zone_w[zone];
			}
			st->scroll_active[zone] = 0;
			st->tab_scroll[zone] = vgui_clampf(st->tab_scroll[zone], 0.0f, max_scroll);

			float tx = st->zone_x[zone] - st->tab_scroll[zone];
			for (size_t i = 0; i < ids.size(); ++i)
			{
				vgui_dock_panel* panel = vgui_dock_find(w, ids[i]);
				if (!panel) continue;
				vgui_dock_tab_geom geom;
				geom.id = ids[i];
				geom.x = tx;
				geom.y = st->zone_y[zone];
				geom.w = widths[i];
				geom.h = tab_h;
				if (panel->closeable && ds.close_size > 0.0f && tab_h > 0.0f)
				{
					float size = std::min(ds.close_size, tab_h);
					geom.close_w = size;
					geom.close_h = size;
					geom.close_x = tx + geom.w - size - ds.tab_padding * 0.5f;
					geom.close_y = geom.y + (tab_h - size) * 0.5f;
				}
				st->tabs[zone].push_back(geom);
				tx += widths[i] + ds.tab_gap;
			}

			vgui_dock_panel* active_panel = vgui_dock_find(w, active);
			float content_y = st->zone_y[zone] + tab_h;
			float content_h = std::max(0.0f, st->zone_h[zone] - tab_h);
			if (active_panel && active_panel->page)
			{
				active_panel->page->visible = 1;
				vgui_layout_pane(active_panel->page, ctx, st->zone_x[zone], content_y,
					st->zone_w[zone], content_h);
			}
		}

		for (vgui_item_id id : st->float_order)
		{
			vgui_dock_panel* panel = vgui_dock_find(w, id);
			if (!panel || !panel->open) continue;
			vgui_dock_clamp_rect(w, panel, panel->fx, panel->fy, panel->fw, panel->fh, false);
			float cx = 0, cy = 0, cw = 0, ch = 0;
			vgui_dock_floating_content_rect(w, *panel, &cx, &cy, &cw, &ch);
			if (panel->page)
			{
				panel->page->visible = 1;
				vgui_layout_pane(panel->page, ctx, cx, cy, cw, ch);
			}
		}

		// Floating pages must be last so hit testing and drawing prefer the
		// topmost frame over zone pages underneath it.
		std::vector<vgui_widget*> ordered;
		ordered.reserve(w->children.size());
		auto add_page = [&ordered](vgui_widget* page)
			{
				if (page && std::find(ordered.begin(), ordered.end(), page) == ordered.end())
					ordered.push_back(page);
			};
		for (int zone = 0; zone < 4; ++zone)
		{
			vgui_dock_panel* active_panel = vgui_dock_find(w, st->active[zone]);
			if (active_panel) add_page(active_panel->page);
		}
		for (vgui_item_id id : st->float_order)
		{
			vgui_dock_panel* panel = vgui_dock_find(w, id);
			if (panel && panel->open) add_page(panel->page);
		}
		for (vgui_widget* child : w->children)
			add_page(child);
		w->children.swap(ordered);
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

		if (w->kind == VGUI_LIST || w->kind == VGUI_TREE)
		{
			float ix = x + w->padding[0];
			float iy = y + w->padding[1];
			float iw = std::max(0.0f, ww - w->padding[0] - w->padding[2]);
			float ih = std::max(0.0f, hh - w->padding[1] - w->padding[3]);
			float max_sx = std::max(0.0f, w->content_w - iw);
			float max_sy = std::max(0.0f, w->content_h - ih);
			if (w->scrollable_x) w->scroll_x = vgui_clampf(w->scroll_x, 0.0f, max_sx);
			else w->scroll_x = 0;
			if (w->scrollable_y) w->scroll_y = vgui_clampf(w->scroll_y, 0.0f, max_sy);
			else w->scroll_y = 0;
			(void)ix; (void)iy;
			return;
		}

		if (w->kind == VGUI_SPLITTER)
		{
			vgui_layout_splitter(w, ctx, x, y, ww, hh);
			return;
		}
		if (w->kind == VGUI_TABVIEW)
		{
			vgui_layout_tabview(w, ctx, x, y, ww, hh);
			return;
		}
		if (w->kind == VGUI_DOCK)
		{
			vgui_layout_dock(w, ctx, x, y, ww, hh);
			return;
		}
		if (w->kind == VGUI_MENUBAR)
		{
			// The bar paints its titles itself; only the outer rect is needed.
			w->abs_x = x;
			w->abs_y = y;
			w->abs_w = ww;
			w->abs_h = hh;
			return;
		}

		if (w->kind != VGUI_PANEL) return;

		float ix = x + w->padding[0];
		float iy = y + w->padding[1];
		float iw = std::max(0.0f, ww - w->padding[0] - w->padding[2]);
		float ih = std::max(0.0f, hh - w->padding[1] - w->padding[3]);
		vgui_arrange_children(w, ctx, ix, iy, iw, ih);
	}

	static float vgui_menu_row_top(vgui_widget* menu, vgui_item_id item)
	{
		vgui_menu_state* st = vgui_menu_of(menu);
		if (!st) return menu->abs_y;
		const vgui_menu_style& ms = menu->ctx->style.menu;
		float y = menu->abs_y + ms.padding_y - menu->scroll_y;
		for (const vgui_menu_item& it : st->items)
		{
			if (it.id == item) return y;
			y += vgui_menu_item_height(menu, it);
		}
		return menu->abs_y;
	}

	static void vgui_menu_update_layout(vgui_widget* menu, vgui_context* ctx)
	{
		vgui_menu_state* st = vgui_menu_of(menu);
		if (!st || !st->open) return;
		vgui_measure_widget(menu, ctx);

		float cw = 0, ch = 0;
		vgui_menu_content_size(menu, &cw, &ch);
		menu->content_w = cw;
		menu->content_h = ch;
		menu->intrinsic_w = cw;
		menu->intrinsic_h = ch;

		float margin = 8.0f;
		float avail_h = ctx->height > 0 ? (float)ctx->height - margin * 2.0f : ch;
		avail_h = std::max(0.0f, avail_h);
		float max_h = st->max_height > 0 ? std::min(st->max_height, avail_h) : avail_h;
		if (ctx->height <= 0 && st->max_height <= 0) max_h = ch;
		float ah = std::max(0.0f, std::min(ch, max_h));
		float avail_w = ctx->width > 0 ? (float)ctx->width - margin * 2.0f : cw;
		avail_w = std::max(0.0f, avail_w);
		float aw = cw;
		if (ctx->width > 0) aw = std::max(1.0f, std::min(cw, avail_w));
		menu->abs_w = aw;
		menu->abs_h = ah;

		float x = st->open_x;
		float y = st->open_y;
		if (st->parent_menu)
		{
			bool rtl = vgui_is_rtl(ctx);
			x = rtl ? st->parent_menu->abs_x - aw + 2.0f
				: st->parent_menu->abs_x + st->parent_menu->abs_w - 2.0f;
			y = vgui_menu_row_top(st->parent_menu, st->parent_item);
			if (ctx->width > 0)
			{
				if (rtl)
				{
					if (x < margin)
						x = st->parent_menu->abs_x + st->parent_menu->abs_w - 2.0f;
				}
				else if (x + aw > (float)ctx->width - margin)
					x = st->parent_menu->abs_x - aw + 2.0f;
			}
		}

		if (ctx->width > 0)
		{
			x = std::min(x, (float)ctx->width - aw - margin);
			x = std::max(x, margin);
		}
		if (ctx->height > 0)
		{
			y = std::min(y, (float)ctx->height - ah - margin);
			y = std::max(y, margin);
		}
		menu->abs_x = x;
		menu->abs_y = y;

		float max_sy = std::max(0.0f, ch - ah);
		menu->scroll_y = vgui_clampf(menu->scroll_y, 0.0f, max_sy);
		menu->scrollable_y = 1;
		menu->scrollable_x = 0;
		menu->scroll_x = 0;

		if (st->submenu_open && st->submenu_open->menu && st->submenu_open->menu->open)
			vgui_menu_update_layout(st->submenu_open, ctx);
	}

	static float vgui_overlay_width(vgui_context* ctx)
	{
		if (!ctx) return 1.0f;
		if (ctx->width > 0) return (float)ctx->width;
		return std::max(ctx->root ? ctx->root->intrinsic_w : 1.0f, 1.0f);
	}

	static float vgui_overlay_height(vgui_context* ctx)
	{
		if (!ctx) return 1.0f;
		if (ctx->height > 0) return (float)ctx->height;
		return std::max(ctx->root ? ctx->root->intrinsic_h : 1.0f, 1.0f);
	}

	static void vgui_modal_layout(vgui_context* ctx)
	{
		if (!ctx || !ctx->modal.active || !ctx->modal.stage || !ctx->modal.panel)
			return;
		vgui_widget* stage = ctx->modal.stage;
		vgui_widget* panel = ctx->modal.panel;
		float cw = vgui_overlay_width(ctx);
		float ch = vgui_overlay_height(ctx);
		stage->abs_x = 0.0f;
		stage->abs_y = 0.0f;
		stage->abs_w = cw;
		stage->abs_h = ch;
		stage->content_w = cw;
		stage->content_h = ch;

		vgui_measure_widget(panel, ctx);
		const vgui_modal_style& ms = ctx->style.modal;
		float margin = ctx->modal.options.margin >= 0.0f
			? ctx->modal.options.margin : ms.margin;
		float avail_w = std::max(0.0f, cw - margin * 2.0f);
		float avail_h = std::max(0.0f, ch - margin * 2.0f);
		if (ms.max_height > 0.0f)
			avail_h = std::min(avail_h, ms.max_height);
		float pw = panel->req_w > 0.0f ? panel->req_w : panel->intrinsic_w;
		float ph = panel->req_h > 0.0f ? panel->req_h : panel->intrinsic_h;
		if (panel->min_w > 0.0f) pw = std::max(pw, panel->min_w);
		if (panel->min_h > 0.0f) ph = std::max(ph, panel->min_h);
		if (panel->max_w > 0.0f) pw = std::min(pw, panel->max_w);
		if (panel->max_h > 0.0f) ph = std::min(ph, panel->max_h);
		if (avail_w > 0.0f) pw = std::min(pw, avail_w);
		if (avail_h > 0.0f) ph = std::min(ph, avail_h);
		pw = std::max(0.0f, pw);
		ph = std::max(0.0f, ph);
		float px = std::floor((cw - pw) * 0.5f);
		float py = std::floor((ch - ph) * 0.5f);
		vgui_layout_widget(panel, ctx, px, py, pw, ph);
	}

	// ---- context 级通用弹出面板 ------------------------------------------------

	// 度量 content、按请求锚点定位并钳制到窗口内。每次布局都会重跑,
	// 内容尺寸变化(如 hex 编辑出错行变高)后位置自动修正。
	static void vgui_popup_update_layout(vgui_context* ctx)
	{
		if (!ctx || !ctx->popup.content) return;
		vgui_widget* content = ctx->popup.content;
		vgui_measure_widget(content, ctx);

		float pw = content->req_w > 0.0f ? content->req_w : content->intrinsic_w;
		float ph = content->req_h > 0.0f ? content->req_h : content->intrinsic_h;
		if (content->min_w > 0.0f) pw = std::max(pw, content->min_w);
		if (content->min_h > 0.0f) ph = std::max(ph, content->min_h);
		if (content->max_w > 0.0f) pw = std::min(pw, content->max_w);
		if (content->max_h > 0.0f) ph = std::min(ph, content->max_h);
		pw = std::max(1.0f, pw);
		ph = std::max(1.0f, ph);

		const float margin = 8.0f;
		if (ctx->width > 0)
		{
			pw = std::min(pw, std::max(1.0f, (float)ctx->width - margin * 2.0f));
			content->abs_x = vgui_clampf(ctx->popup.x, margin,
				std::max(margin, (float)ctx->width - pw - margin));
		}
		else
			content->abs_x = ctx->popup.x;
		if (ctx->height > 0)
		{
			ph = std::min(ph, std::max(1.0f, (float)ctx->height - margin * 2.0f));
			content->abs_y = vgui_clampf(ctx->popup.y, margin,
				std::max(margin, (float)ctx->height - ph - margin));
		}
		else
			content->abs_y = ctx->popup.y;
		content->abs_w = pw;
		content->abs_h = ph;

		vgui_layout_widget(content, ctx, content->abs_x, content->abs_y, pw, ph);
	}

	/* close_fn 通知 + 状态清理。close 监听器是 context 级持久回调,
	 * 跨会话保留;内容销毁路径不通知(宿主已在析构流程里)。 */
	static void vgui_popup_close_internal(vgui_context* ctx, int reason)
	{
		if (!ctx || !ctx->popup.content) return;
		vgui_widget* content = ctx->popup.content;
		vgui_popup_close_fn fn = ctx->popup.close_fn;
		void* user = ctx->popup.close_user;
		ctx->popup.content = nullptr;
		ctx->popup.swallow_up = 0;
		ctx->popup.x = ctx->popup.y = 0.0f;
		if (ctx->focus && vgui_is_within(content, ctx->focus))
			vgui_set_focus_internal(ctx, nullptr);
		if (fn)
			fn(user, reason);
	}

	static void vgui_toast_layout(vgui_context* ctx)
	{
		if (!ctx) return;
		const vgui_toast_style& ts = ctx->style.toast;
		float cw = vgui_overlay_width(ctx);
		float ch = vgui_overlay_height(ctx);
		float line_h = vgui_line_height(ctx, ctx->root);
		float line_gap = std::max(0.0f, ts.line_gap);
		float pad_x = std::max(0.0f, ts.padding_x);
		float pad_y = std::max(0.0f, ts.padding_y);
		float max_text_w = std::max(0.0f, ts.max_width - pad_x * 2.0f);
		float margin = std::max(0.0f, ts.margin);
		float gap = std::max(0.0f, ts.gap);
		int max_visible = std::max(1, ts.max_visible);

		float top_y[3] = { margin, margin, margin };
		float bottom_y[3] = { ch - margin, ch - margin, ch - margin };
		for (size_t i = 0; i < ctx->toasts.size(); ++i)
		{
			vgui_toast_record& t = ctx->toasts[i];
			if ((int)i >= max_visible)
			{
				t.x = t.y = t.w = t.h = 0.0f;
				continue;
			}
			t.lines = vgui_wrap_text(ctx, ctx->root, t.text, max_text_w);
			float text_w = 0.0f;
			for (const std::string& line : t.lines)
				text_w = std::max(text_w, vgui_text_width(ctx->root, line));
			float w = std::min(ts.max_width, text_w + pad_x * 2.0f);
			float h = (float)t.lines.size() * line_h
				+ (float)std::max(0, (int)t.lines.size() - 1) * line_gap
				+ pad_y * 2.0f;
			w = std::max(1.0f, w);
			h = std::max(1.0f, h);

			int pos = (int)t.options.position;
			if (pos < 0 || pos > (int)VGUI_TOAST_BOTTOM_CENTER)
				pos = VGUI_TOAST_TOP_RIGHT;
			int row = pos / 3;
			int col = pos % 3;
			float x = margin;
			if (col == 0) x = cw - margin - w;
			else if (col == 2) x = (cw - w) * 0.5f;
			float y;
			if (row == 0)
			{
				y = top_y[col];
				top_y[col] += h + gap;
			}
			else
			{
				y = bottom_y[col] - h;
				bottom_y[col] -= h + gap;
			}
			t.x = std::max(0.0f, x);
			t.y = y;
			t.w = w;
			t.h = h;
		}
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
		if (ctx->open_menu)
			vgui_menu_update_layout(ctx->open_menu, ctx);
		vgui_modal_layout(ctx);
		vgui_popup_update_layout(ctx);
		vgui_toast_layout(ctx);
	}

	static void vgui_menu_close_internal(vgui_widget* menu);
	static void vgui_modal_close_internal(vgui_context* ctx, int result);
	static void vgui_popup_close_internal(vgui_context* ctx, int reason);
	static void vgui_set_focus_internal(vgui_context* ctx, vgui_widget* w);

	static void vgui_dialog_forget_widget(vgui_context* ctx, vgui_widget* w)
	{
		if (!ctx || !w) return;
		vgui_dialog_state& d = ctx->dialog;
		if (d.panel == w)
		{
			d.panel = nullptr;
			d.input = nullptr;
			d.confirm_button = nullptr;
			d.cancel_button = nullptr;
			d.on_result = nullptr;
			d.user = nullptr;
			return;
		}
		if (d.input == w) d.input = nullptr;
		if (d.confirm_button == w) d.confirm_button = nullptr;
		if (d.cancel_button == w) d.cancel_button = nullptr;
	}

	static void vgui_delete_rec(vgui_widget* w, vgui_context* ctx)
	{
		if (!w) return;
		if (ctx)
		{
			vgui_tooltip_forget_widget(ctx, w);
			vgui_dialog_forget_widget(ctx, w);
			if (ctx->modal.panel == w)
			{
				ctx->modal.active = 0;
				ctx->modal.panel = nullptr;
			}
			if (ctx->modal.stage == w)
			{
				ctx->modal.active = 0;
				ctx->modal.stage = nullptr;
			}
			if (ctx->popup.content == w)
			{
				/* 内容被直接销毁:静默清状态,不通知 close_fn(宿主自己
				 * 销毁内容时通常已在析构流程里)。 */
				ctx->popup = vgui_popup_state();
			}
			/* Deletion can start at an ancestor, so clear every input
			 * reference as the recursion reaches the widget itself.  The
			 * flush path only sees the subtree root; leaving descendant
			 * pointers here would make later hit-test/focus cleanup walk
			 * freed widgets. */
			if (ctx->hot == w) ctx->hot = nullptr;
			if (ctx->active == w) ctx->active = nullptr;
			if (ctx->press_target == w) ctx->press_target = nullptr;
			if (ctx->focus == w) ctx->focus = nullptr;
			if (ctx->slider_drag == w) ctx->slider_drag = nullptr;
			if (ctx->open_menu == w) ctx->open_menu = nullptr;
			if (ctx->menu_pressed == w) ctx->menu_pressed = nullptr;
		}
		for (vgui_widget* c : w->children)
			vgui_delete_rec(c, ctx);

		// Menus own their submenu widgets, but submenus are not regular children:
		// they are context popups and must be deleted explicitly.
		if (w->kind == VGUI_MENU && w->menu)
		{
			vgui_menu_state* ms = w->menu.get();
			if (ms->parent_menu && ms->parent_menu->menu)
			{
				vgui_menu_state* parent = ms->parent_menu->menu.get();
				if (parent->submenu_open == w) parent->submenu_open = nullptr;
				for (vgui_menu_item& pi : parent->items)
					if (pi.submenu == w) pi.submenu = nullptr;
			}
			// Destroying a menu that is still attached to a menu bar detaches it
			// from the bar so the bar never keeps a dangling item.
			if (ms->owner_menubar && ctx->alive.count(ms->owner_menubar))
			{
				vgui_menubar_state* bs = vgui_menubar_of(ms->owner_menubar);
				if (bs)
				{
					auto bi = bs->index.find(ms->owner_item);
					if (bi != bs->index.end() && bi->second < bs->items.size() &&
						bs->items[bi->second].menu == w)
					{
						vgui_item_id removed = bs->items[bi->second].id;
						bs->items.erase(bs->items.begin() + (std::ptrdiff_t)bi->second);
						bs->index.clear();
						for (size_t i = 0; i < bs->items.size(); ++i)
							bs->index[bs->items[i].id] = i;
						if (bs->active == removed) bs->active = 0;
						if (bs->hover == removed) bs->hover = 0;
						if (bs->pressed == removed) bs->pressed = 0;
					}
				}
			}
			ms->owner_menubar = nullptr;
			ms->owner_item = 0;
			if (ctx->open_menu == w) ctx->open_menu = nullptr;
			std::vector<vgui_widget*> subs;
			for (const vgui_menu_item& item : ms->items)
				if (item.submenu) subs.push_back(item.submenu);
			for (vgui_widget* sub : subs)
				if (sub && ctx->alive.count(sub)) vgui_delete_rec(sub, ctx);
		}

		// A menu bar owns its popup menus (they are not layout children).
		if (w->kind == VGUI_MENUBAR && w->menubar)
		{
			vgui_menubar_state* bs = w->menubar.get();
			std::vector<vgui_widget*> menus;
			for (vgui_menubar_item& item : bs->items)
			{
				if (item.menu && ctx->alive.count(item.menu))
					menus.push_back(item.menu);
				if (item.menu && item.menu->menu)
				{
					item.menu->menu->owner_menubar = nullptr;
					item.menu->menu->owner_item = 0;
				}
				item.menu = nullptr;
			}
			bs->items.clear();
			bs->index.clear();
			bs->active = 0;
			bs->hover = 0;
			bs->pressed = 0;
			for (vgui_widget* menu : menus)
				if (ctx->alive.count(menu)) vgui_delete_rec(menu, ctx);
		}

		ctx->alive.erase(w);
		ctx->all_widgets.erase(w);

		// button icon texture — release ref before delete
		if (w->icon_tex) { vgui_texture_release(w->icon_tex); w->icon_tex = nullptr; }

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
			if (ctx->open_menu == w) ctx->open_menu = nullptr;
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
		if (ctx->modal.active && ctx->modal.panel == w)
			vgui_modal_close_internal(ctx, VGUI_MODAL_RESULT_CANCEL);
		if (w->kind == VGUI_MENU)
			vgui_menu_close_internal(w);
		w->destroy_pending = true;
		w->visible = 0;
		vgui_tooltip_forget_widget(ctx, w);
		vgui_dialog_forget_widget(ctx, w);
		if (ctx->hot == w) ctx->hot = nullptr;
		if (ctx->active == w) ctx->active = nullptr;
		if (ctx->press_target == w) ctx->press_target = nullptr;
		if (ctx->focus == w) ctx->focus = nullptr;
		if (ctx->slider_drag == w) ctx->slider_drag = nullptr;
		ctx->pending_delete.push_back(w);
		ctx->layout_dirty = true;
	}

	// Deactivates the modal, detaches its panel and reports the result.  The
	// callback still sees a valid panel; destroy_on_close is applied afterwards.
	static void vgui_modal_close_internal(vgui_context* ctx, int result)
	{
		if (!ctx || !ctx->modal.active) return;
		vgui_widget* panel = ctx->modal.panel;
		vgui_widget* stage = ctx->modal.stage;
		vgui_widget* restore = ctx->modal.restore_focus;
		vgui_modal_options options = ctx->modal.options;
		bool panel_alive = panel && ctx->alive.count(panel);
		bool stage_alive = stage && ctx->alive.count(stage);
		bool dialog_matched = panel_alive && ctx->dialog.panel == panel;
		bool destroy_panel = panel_alive && (options.destroy_on_close || dialog_matched);

		vgui_dialog_state dialog = {};
		std::string input_text;
		if (dialog_matched)
		{
			dialog = ctx->dialog;
			if (dialog.input && ctx->alive.count(dialog.input))
				input_text = dialog.input->edit_text;
			ctx->dialog = vgui_dialog_state();
		}

		ctx->modal.active = 0;
		ctx->modal.stage = nullptr;
		ctx->modal.panel = nullptr;
		ctx->modal.restore_focus = nullptr;
		ctx->modal.result = result;
		ctx->modal.options = vgui_modal_options();

		if (panel_alive)
			vgui_widget_remove(panel);
		ctx->hot = nullptr;
		ctx->active = nullptr;
		ctx->press_target = nullptr;
		ctx->slider_drag = nullptr;
		vgui_tooltip_clear_candidate(ctx);

		// Never restore focus into a panel that is about to be destroyed.
		if (restore && (!ctx->alive.count(restore) || !restore->visible ||
			!restore->enabled || restore->destroy_pending ||
			(destroy_panel && vgui_is_within(panel, restore))))
			restore = nullptr;
		vgui_set_focus_internal(ctx, restore);

		if (options.on_close)
			options.on_close(ctx, panel_alive ? panel : nullptr, result, options.user);
		if (dialog_matched && dialog.on_result)
			dialog.on_result(ctx, result, input_text.c_str(), dialog.user);

		// A callback may have opened another modal (possibly reusing the panel),
		// so only destroy the objects that still belong to this closing modal.
		if (stage_alive && ctx->alive.count(stage))
			vgui_schedule_destroy(stage);
		if (destroy_panel && panel_alive && ctx->alive.count(panel) &&
			ctx->modal.panel != panel)
			vgui_schedule_destroy(panel);

		// Focus may still sit on one of the widgets that were just scheduled for
		// deletion (panel controls, or a background element of the stage).
		if (ctx->focus && ctx->focus->ctx == ctx &&
			((destroy_panel && vgui_is_within(panel, ctx->focus)) ||
				vgui_is_within(stage, ctx->focus)))
			vgui_set_focus_internal(ctx, nullptr);
		ctx->layout_dirty = true;
	}

	static void vgui_dialog_button_event(vgui_widget* widget, const vgui_event* event,
		void* user)
	{
		(void)user;
		if (!widget || !event || event->type != VGUI_EVENT_CLICK) return;
		vgui_context* ctx = widget->ctx;
		if (!ctx || !ctx->modal.active) return;
		vgui_modal_close_internal(ctx, (int)widget->value);
	}

	static vgui_widget* vgui_hit_rec(vgui_widget* w, float x, float y)
	{
		if (!w || !w->visible || !w->enabled || w->destroy_pending) return nullptr;
		if (!vgui_point_in(w, x, y)) return nullptr;
		if (w->kind == VGUI_SPLITTER && vgui_splitter_sash_hit(w, x, y))
			return w;
		if (w->kind == VGUI_TABVIEW && vgui_tab_bar_hit(w, x, y))
			return w;
		if (w->kind == VGUI_DOCK && vgui_dock_chrome_hit(w, x, y))
			return w;
		for (auto it = w->children.rbegin(); it != w->children.rend(); ++it)
		{
			vgui_widget* r = vgui_hit_rec(*it, x, y);
			if (r) return r;
		}
		return w;
	}

	static vgui_widget* vgui_hit_test_ctx(vgui_context* ctx, float x, float y)
	{
		vgui_widget* root = vgui_active_root(ctx);
		if (!ctx || !root) return nullptr;
		vgui_ensure_layout(ctx);
		return vgui_hit_rec(root, x, y);
	}

	//----------------------------------------------------------------------------
	// tooltip
	//----------------------------------------------------------------------------

	// Defined with the list/tree interaction helpers below; declared here so the
	// tooltip resolver can reuse the same row hit-testing rules.
	static int vgui_list_index_at(vgui_widget* w, float x, float y);
	static int vgui_tree_row_at(vgui_widget* w, float x, float y);

	static std::string vgui_tooltip_widget_text(vgui_widget* w)
	{
		if (!w || !w->tooltip_enabled) return {};
		if (w->tooltip_text_fn)
		{
			const char* text = w->tooltip_text_fn(w, w->tooltip_user);
			if (text && *text) return std::string(text);
		}
		return w->tooltip;
	}

	static std::string vgui_tooltip_item_text(vgui_widget* w, vgui_item_id item)
	{
		if (!w || !item || !w->tooltip_enabled) return {};
		if (w->kind == VGUI_LIST)
		{
			vgui_list_item* it = vgui_list_item_find(w, item);
			if (it) return it->tooltip;
		}
		else if (w->kind == VGUI_TREE)
		{
			vgui_tree_node* node = vgui_tree_node_find(w, item);
			if (node) return node->tooltip;
		}
		return {};
	}

	static std::string vgui_tooltip_owner_text(vgui_widget* owner, vgui_item_id item)
	{
		std::string text;
		if (item) text = vgui_tooltip_item_text(owner, item);
		if (text.empty()) text = vgui_tooltip_widget_text(owner);
		return text;
	}

	static bool vgui_tooltip_effective_enabled(vgui_widget* w)
	{
		for (vgui_widget* p = w; p; p = p->parent)
			if (!p->enabled) return false;
		return true;
	}

	static bool vgui_tooltip_visible_chain(vgui_widget* w)
	{
		for (vgui_widget* p = w; p; p = p->parent)
			if (!p->visible || p->destroy_pending) return false;
		return w && w->ctx && w->ctx->alive.count(w);
	}

	// Unlike the normal hit test this ignores the enabled flag: disabled widgets
	// still receive tooltips unless their own when_disabled policy says no.
	static vgui_widget* vgui_tooltip_hit_rec(vgui_widget* w, float x, float y)
	{
		if (!w || !w->visible || w->destroy_pending) return nullptr;
		if (!vgui_point_in(w, x, y)) return nullptr;
		for (auto it = w->children.rbegin(); it != w->children.rend(); ++it)
		{
			vgui_widget* r = vgui_tooltip_hit_rec(*it, x, y);
			if (r) return r;
		}
		return w;
	}

	// Deepest widget with usable tooltip text wins.  When a child has no text
	// the search continues with its ancestors, so a tooltip attached to a row or
	// toolbar button also covers its labels and icons.
	static vgui_widget* vgui_tooltip_resolve_at(vgui_context* ctx, vgui_widget* hit,
		float x, float y, vgui_item_id* out_item, std::string* out_text)
	{
		if (!ctx || !hit) return nullptr;
		for (vgui_widget* node = hit; node && node->ctx == ctx; node = node->parent)
		{
			if (!node->visible || node->destroy_pending || !node->tooltip_enabled)
				continue;

			vgui_item_id item = 0;
			std::string text;
			if (node == hit && (node->kind == VGUI_LIST || node->kind == VGUI_TREE))
			{
				int index = node->kind == VGUI_LIST
					? vgui_list_index_at(node, x, y)
					: vgui_tree_row_at(node, x, y);
				if (index >= 0)
				{
					vgui_item_id id = node->kind == VGUI_LIST
						? vgui_list_row_id(node, node->list.get(), index)
						: vgui_tree_row_id_at(node, node->tree.get(), index);
					if (id)
					{
						text = vgui_tooltip_item_text(node, id);
						if (!text.empty()) item = id;
					}
				}
			}
			if (text.empty())
				text = vgui_tooltip_widget_text(node);
			if (text.empty())
				continue;
			if (!vgui_tooltip_effective_enabled(node) && !node->tooltip_when_disabled)
				continue;

			if (out_item) *out_item = item;
			if (out_text) *out_text = std::move(text);
			return node;
		}
		return nullptr;
	}

	static void vgui_tooltip_wrap_paragraph(vgui_widget* owner, const std::string& para,
		float max_w, std::vector<std::string>& out)
	{
		if (max_w <= 0.0f || vgui_text_width(owner, para) <= max_w)
		{
			out.push_back(para);
			return;
		}

		const int len = (int)para.size();
		int line_start = 0;
		int break_pos = -1;
		int i = 0;
		bool pushed = false;
		while (i < len)
		{
			int j = vgui_u8_next(para, i);
			if (j <= i) j = i + 1;
			float width = vgui_text_width_n(owner, para.data() + line_start, j - line_start);
			if (width > max_w && j > line_start)
			{
				int cut = break_pos > line_start ? break_pos : i;
				if (cut <= line_start) cut = j;	// a single glyph wider than the box
				size_t end = (size_t)cut;
				while (end > (size_t)line_start &&
					(para[end - 1] == ' ' || para[end - 1] == '\t' || para[end - 1] == '\r'))
					--end;
				if (end > (size_t)line_start || cut == j)
				{
					out.push_back(para.substr((size_t)line_start, end - (size_t)line_start));
					pushed = true;
				}
				if (break_pos > line_start)
				{
					line_start = break_pos;
					while (line_start < len &&
						(para[(size_t)line_start] == ' ' || para[(size_t)line_start] == '\t'))
						++line_start;
				}
				else
				{
					line_start = cut;
				}
				break_pos = -1;
				i = line_start;
				continue;
			}
			if (para[(size_t)i] == ' ' || para[(size_t)i] == '\t')
				break_pos = j;
			i = j;
		}

		if (line_start < len)
		{
			size_t end = (size_t)len;
			while (end > (size_t)line_start &&
				(para[end - 1] == ' ' || para[end - 1] == '\t' || para[end - 1] == '\r'))
				--end;
			out.push_back(para.substr((size_t)line_start, end - (size_t)line_start));
		}
		else if (!pushed)
		{
			out.push_back(std::string());
		}
	}

	static void vgui_tooltip_wrap_text(vgui_widget* owner, const std::string& text,
		float max_w, std::vector<std::string>& out)
	{
		out.clear();
		size_t start = 0;
		for (;;)
		{
			size_t end = text.find('\n', start);
			std::string para = text.substr(start,
				end == std::string::npos ? std::string::npos : end - start);
			if (!para.empty() && para.back() == '\r')
				para.pop_back();
			if (para.empty())
				out.push_back(std::string());
			else
				vgui_tooltip_wrap_paragraph(owner, para, max_w, out);
			if (end == std::string::npos) break;
			start = end + 1;
		}
	}

	static bool vgui_tooltip_item_rect(vgui_widget* w, vgui_item_id item,
		float* x, float* y, float* width, float* height)
	{
		if (!w || !item || !x || !y || !width || !height) return false;
		if (w->kind == VGUI_LIST && w->list)
		{
			int index = vgui_list_row_index(w, w->list.get(), item);
			if (index < 0) return false;
			float row_h = vgui_list_row_height(w);
			*x = w->abs_x + w->padding[0];
			*y = w->abs_y + w->padding[1] + (float)index * row_h - w->scroll_y;
			*width = std::max(0.0f, w->abs_w - w->padding[0] - w->padding[2]);
			*height = row_h;
			return true;
		}
		if (w->kind == VGUI_TREE && w->tree)
		{
			int index = vgui_tree_visible_index(w, item);
			if (index < 0) return false;
			float row_h = vgui_tree_row_height(w);
			*x = w->abs_x + w->padding[0];
			*y = w->abs_y + w->padding[1] + (float)index * row_h - w->scroll_y;
			*width = std::max(0.0f, w->abs_w - w->padding[0] - w->padding[2]);
			*height = row_h;
			return true;
		}
		return false;
	}

	static void vgui_tooltip_measure(vgui_context* ctx)
	{
		if (!ctx) return;
		vgui_tooltip_state& t = ctx->tooltip;
		if (!t.visible || !t.owner || t.text.empty() ||
			!ctx->alive.count(t.owner) || !t.owner->visible)
		{
			t.w = t.h = 0.0f;
			return;
		}

		const vgui_tooltip_style& ts = ctx->style.tooltip;
		float pad_x = std::max(0.0f, ts.padding_x);
		float pad_y = std::max(0.0f, ts.padding_y);
		float margin = std::max(0.0f, ts.margin);
		float max_w = ts.max_width > 0.0f ? ts.max_width : 320.0f;
		if (ctx->width > 0)
			max_w = std::min(max_w, std::max(16.0f, (float)ctx->width - margin * 2.0f));
		float inner_max = std::max(1.0f, max_w - pad_x * 2.0f);

		vgui_tooltip_wrap_text(t.owner, t.text, inner_max, t.lines);
		if (t.lines.empty())
			t.lines.push_back(std::string());

		float line_h = vgui_line_height(ctx, t.owner);
		float gap = std::max(0.0f, ts.line_gap);
		float text_w = 0.0f;
		for (const std::string& line : t.lines)
			text_w = std::max(text_w, vgui_text_width(t.owner, line));

		float w = std::min(inner_max, text_w) + pad_x * 2.0f;
		float h = (float)t.lines.size() * line_h +
			(float)(t.lines.size() > 1 ? t.lines.size() - 1 : 0) * gap + pad_y * 2.0f;
		w = std::max(1.0f, w);
		h = std::max(1.0f, h);

		float x = 0.0f, y = 0.0f;
		if (t.owner->tooltip_anchor == VGUI_TOOLTIP_ANCHOR_WIDGET)
		{
			float ax = t.owner->abs_x, ay = t.owner->abs_y;
			float aw = t.owner->abs_w, ah = t.owner->abs_h;
			vgui_tooltip_item_rect(t.owner, t.owner_item, &ax, &ay, &aw, &ah);
			float anchor_gap = std::max(0.0f, ts.widget_gap);
			x = ax + (aw - w) * 0.5f;
			y = ay + ah + anchor_gap;
			if (ctx->height > 0 && y + h > (float)ctx->height - margin)
			{
				float above = ay - anchor_gap - h;
				if (above >= margin)
					y = above;
			}
		}
		else
		{
			x = ctx->mouse_x + ts.pointer_offset_x;
			y = ctx->mouse_y + ts.pointer_offset_y;
			if (ctx->width > 0 && x + w > (float)ctx->width - margin)
				x = ctx->mouse_x - ts.pointer_offset_x - w;
			if (ctx->height > 0 && y + h > (float)ctx->height - margin)
				y = ctx->mouse_y - ts.pointer_offset_y - h;
		}

		if (ctx->width > 0)
		{
			float hi = std::max(margin, (float)ctx->width - w - margin);
			x = vgui_clampf(x, margin, hi);
		}
		if (ctx->height > 0)
		{
			float hi = std::max(margin, (float)ctx->height - h - margin);
			y = vgui_clampf(y, margin, hi);
		}

		t.x = x;
		t.y = y;
		t.w = w;
		t.h = h;
	}

	static void vgui_tooltip_on_mouse_move(vgui_context* ctx, float x, float y)
	{
		if (!ctx) return;
		vgui_tooltip_state& t = ctx->tooltip;
		t.suppressed = 0;
		if (!ctx->tooltip_enabled || ctx->open_menu || ctx->popup.content || vgui_any_mouse_down(ctx))
		{
			vgui_tooltip_clear_candidate(ctx);
			return;
		}

		vgui_ensure_layout(ctx);
		vgui_widget* hit = vgui_tooltip_hit_rec(vgui_active_root(ctx), x, y);
		vgui_item_id item = 0;
		std::string text;
		vgui_widget* owner = vgui_tooltip_resolve_at(ctx, hit, x, y, &item, &text);
		if (!owner)
		{
			vgui_tooltip_clear_candidate(ctx);
			return;
		}

		if (t.candidate != owner || t.candidate_item != item)
		{
			vgui_tooltip_hide(ctx);
			t.candidate = owner;
			t.candidate_item = item;
			t.hover_time = 0.0f;
			return;
		}

		if (t.visible)
		{
			if (text != t.text)
			{
				t.text = std::move(text);
				vgui_tooltip_measure(ctx);
			}
			else
			{
				// Pointer-anchored tooltips follow the cursor between updates.
				vgui_tooltip_measure(ctx);
			}
		}
	}

	static void vgui_tooltip_update(vgui_context* ctx, float dt)
	{
		if (!ctx) return;
		vgui_tooltip_state& t = ctx->tooltip;
		if (!ctx->tooltip_enabled || ctx->open_menu || ctx->popup.content)
		{
			vgui_tooltip_clear_candidate(ctx);
			return;
		}
		if (!t.candidate || !ctx->alive.count(t.candidate) || !vgui_tooltip_visible_chain(t.candidate))
		{
			vgui_tooltip_clear_candidate(ctx);
			return;
		}
		if (t.suppressed)
		{
			vgui_tooltip_hide(ctx);
			return;
		}
		if (vgui_any_mouse_down(ctx))
		{
			vgui_tooltip_suppress(ctx);
			return;
		}
		if (!t.candidate->tooltip_enabled)
		{
			vgui_tooltip_clear_candidate(ctx);
			return;
		}
		if (!vgui_tooltip_effective_enabled(t.candidate) && !t.candidate->tooltip_when_disabled)
		{
			vgui_tooltip_clear_candidate(ctx);
			return;
		}

		if (t.visible)
		{
			if (t.owner != t.candidate || t.owner_item != t.candidate_item ||
				t.text.empty() || !vgui_tooltip_visible_chain(t.owner))
				vgui_tooltip_hide(ctx);
			return;
		}

		if (t.hover_time == 0.0f)
		{
			std::string text = vgui_tooltip_owner_text(t.candidate, t.candidate_item);
			if (text.empty())
				return;
			t.text = std::move(text);
		}

		float delay = t.candidate->tooltip_delay >= 0.0f
			? t.candidate->tooltip_delay : ctx->style.tooltip.delay;
		t.hover_time += std::max(0.0f, dt);
		if (t.hover_time < delay)
			return;

		t.owner = t.candidate;
		t.owner_item = t.candidate_item;
		t.visible = 1;
		vgui_tooltip_measure(ctx);
	}

	// Re-reads the text of the visible tooltip after a model / property change.
	static void vgui_tooltip_refresh(vgui_context* ctx, vgui_widget* owner, vgui_item_id item)
	{
		if (!ctx || ctx->tooltip.owner != owner || ctx->tooltip.owner_item != item)
			return;
		ctx->tooltip.text = vgui_tooltip_owner_text(owner, item);
		if (ctx->tooltip.text.empty())
			vgui_tooltip_hide(ctx);
		else
			vgui_tooltip_measure(ctx);
	}

	static bool vgui_tooltip_is_visible(vgui_context* ctx)
	{
		return ctx && ctx->tooltip.visible && ctx->tooltip.owner &&
			vgui_tooltip_visible_chain(ctx->tooltip.owner);
	}

	static void vgui_set_focus_internal(vgui_context* ctx, vgui_widget* w)
	{
		if (!ctx) return;
		if (ctx->focus == w) return;
		vgui_widget* previous = ctx->focus;
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
		if (ctx->focus_fn)
			ctx->focus_fn(ctx, previous, w, ctx->focus_user);
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
		// Never keep more scroll than the whole text actually needs.  Without
		// this, undoing a large paste leaves the short restored text scrolled
		// completely out of view (empty-looking edit box).  Horizontal scrolling
		// spans the widest line, so the bar can reach the end of long content.
		vgui_edit_clamp_scroll(w);
	}

	static void vgui_edit_sync_text(vgui_widget* w)
	{
		vgui_edit_invalidate_content_width(w);
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
		float ix = w->abs_x + w->padding[0];
		float iw = std::max(0.0f, w->abs_w - w->padding[0] - w->padding[2]);
		float lx = vgui_is_rtl(w->ctx)
			? ix + iw + w->edit_scroll_x - x
			: x - ix + w->edit_scroll_x;
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
		{
			/* 先广播取消再失焦：行内编辑等宿主可借 VGUI_EVENT_CANCEL
			 * 丢弃未提交内容，而不必区分"失焦提交"路径。 */
			vgui_event cancel = {};
			cancel.type = VGUI_EVENT_CANCEL;
			cancel.key = VGUI_KEY_ESCAPE;
			vgui_emit_full(w, cancel);
			vgui_set_focus_internal(w->ctx, nullptr);
			return true;
		}
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
		{
			t = (lx - w->padding[0]) / inner_w;
			if (vgui_is_rtl(w->ctx)) t = 1.0f - t;
		}
		vgui_set_value_internal(w, t, true);
	}

	static void vgui_focus_next_impl(vgui_context* ctx, bool reverse)
	{
		vgui_widget* root = vgui_active_root(ctx);
		if (!ctx || !root) return;
		std::vector<vgui_widget*> list;
		std::function<void(vgui_widget*)> collect = [&](vgui_widget* w)
			{
				if (!w || !w->visible || !w->enabled) return;
				if (w->focusable) list.push_back(w);
				for (vgui_widget* c : w->children) collect(c);
			};
		collect(root);
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
		int axis = 0;			// 1 = panel vertical, 2 = panel horizontal,
		// 3 = edit vertical, 4 = edit horizontal
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
		const vgui_style& st = w->ctx->style;
		float ix = w->abs_x + w->padding[0];
		float iy = w->abs_y + w->padding[1];
		float iw = std::max(0.0f, w->abs_w - w->padding[0] - w->padding[2]);
		float ih = std::max(0.0f, w->abs_h - w->padding[1] - w->padding[3]);
		g->axis = axis;
		if (axis == 1)
		{
			if (!w->scrollable_y || w->content_h <= ih + 0.5f || ih <= st.panel.scrollbar_min_track) return false;
			g->track = vgui_is_rtl(w->ctx)
				? w->abs_x
				: w->abs_x + w->abs_w - (st.panel.scrollbar_width + st.panel.scrollbar_inset);
			g->start = iy;
			g->span = ih;
			g->thumb_len = std::min(ih, std::max(st.panel.scrollbar_min_thumb, ih * ih / w->content_h));
			g->max_scroll = w->content_h - ih;
			g->scroll = w->scroll_y;
		}
		else if (axis == 2)
		{
			if (!w->scrollable_x || w->content_w <= iw + 0.5f || iw <= st.panel.scrollbar_min_track) return false;
			g->track = w->abs_y + w->abs_h - (st.panel.scrollbar_width + st.panel.scrollbar_inset);
			g->start = ix;
			g->span = iw;
			g->thumb_len = std::min(iw, std::max(st.panel.scrollbar_min_thumb, iw * iw / w->content_w));
			g->max_scroll = w->content_w - iw;
			g->scroll = w->scroll_x;
		}
		else if (axis == 3)
		{
			if (!w->multiline || iw <= 1.0f || ih <= 1.0f) return false;
			if (!vgui_edit_vbar_visible(w, ih)) return false;
			float lh = std::max(1.0f, vgui_line_height(w->ctx, w));
			float ch = (float)vgui_text_lines(w->edit_text) * lh;
			float corner = st.edit.scrollbar_width + st.edit.scrollbar_inset;
			float track_end = vgui_edit_hbar_visible(w, iw)
				? w->abs_y + w->abs_h - corner : iy + ih;
			float track_h = track_end - iy;
			if (track_h <= 0.0f) return false;
			g->track = vgui_is_rtl(w->ctx) ? w->abs_x : w->abs_x + w->abs_w - corner;
			g->start = iy;
			g->span = track_h;
			g->thumb_len = std::min(track_h, std::max(st.edit.scrollbar_min_thumb, track_h * ih / ch));
			g->max_scroll = std::max(0.0f, ch - ih);
			g->scroll = w->edit_scroll_y;
		}
		else if (axis == 4)
		{
			if (!w->multiline || iw <= 1.0f || ih <= st.edit.scrollbar_min_cross) return false;
			if (!vgui_edit_hbar_visible(w, iw)) return false;
			float cw = vgui_edit_content_width(w);
			float corner = st.edit.scrollbar_width + st.edit.scrollbar_inset;
			bool vbar = vgui_edit_vbar_visible(w, ih);
			float track_start = vgui_is_rtl(w->ctx) && vbar ? w->abs_x + corner : ix;
			float track_end = vgui_is_rtl(w->ctx)
				? ix + iw
				: (vbar ? w->abs_x + w->abs_w - corner : ix + iw);
			float track_w = track_end - track_start;
			if (track_w <= 0.0f) return false;
			g->track = w->abs_y + w->abs_h - corner;
			g->start = track_start;
			g->span = track_w;
			g->thumb_len = std::min(track_w, std::max(st.edit.scrollbar_min_thumb, track_w * iw / cw));
			g->max_scroll = std::max(0.0f, cw - iw + st.edit.scrollbar_extra_scroll);
			g->scroll = w->edit_scroll_x;
		}
		else
		{
			return false;
		}
		float range = g->span - g->thumb_len;
		float t = (g->max_scroll > 0.0f && range > 0.0f)
			? vgui_clampf(g->scroll / g->max_scroll, 0.0f, 1.0f) : 0.0f;
		// Horizontal bars grow from the right edge in RTL.
		if (vgui_is_rtl(w->ctx) && (axis == 2 || axis == 4))
			t = 1.0f - t;
		g->thumb = g->start + t * range;
		return true;
	}

	static void vgui_scrollbar_apply(vgui_widget* w, const vgui_scrollbar_info& g, float thumb_start)
	{
		float range = g.span - g.thumb_len;
		float t = range > 0.0f ? vgui_clampf((thumb_start - g.start) / range, 0.0f, 1.0f) : 0.0f;
		if (vgui_is_rtl(w->ctx) && (g.axis == 2 || g.axis == 4))
			t = 1.0f - t;
		float scroll = t * g.max_scroll;
		if (g.axis == 3)
		{
			w->edit_scroll_y = scroll;
			return;
		}
		if (g.axis == 4)
		{
			w->edit_scroll_x = scroll;
			return;
		}
		if (g.axis == 1) w->scroll_y = scroll;
		else w->scroll_x = scroll;
		if (w->ctx)
		{
			w->ctx->layout_dirty = true;
			vgui_ensure_layout(w->ctx);
		}
		vgui_notify_scroll(w);
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
			const vgui_style& st = w->ctx->style;
			float edit_hit = st.edit.scrollbar_width + st.edit.scrollbar_inset + st.edit.scrollbar_hit_padding;
			float panel_hit = st.panel.scrollbar_width + st.panel.scrollbar_inset + st.panel.scrollbar_hit_padding;
			if (w->kind == VGUI_EDIT &&
				x >= (vgui_is_rtl(w->ctx) ? w->abs_x : w->abs_x + w->abs_w - edit_hit) &&
				x <= (vgui_is_rtl(w->ctx) ? w->abs_x + edit_hit : w->abs_x + w->abs_w) &&
				y >= w->abs_y && y <= w->abs_y + w->abs_h &&
				vgui_scrollbar_geom(w, 3, &g) &&
				y >= g.start && y <= g.start + g.span)
			{
				found = w;
				found_axis = 3;
			}
			if (w->kind == VGUI_EDIT &&
				y >= w->abs_y + w->abs_h - edit_hit && y <= w->abs_y + w->abs_h &&
				x >= w->abs_x && x <= w->abs_x + w->abs_w &&
				vgui_scrollbar_geom(w, 4, &g) &&
				x >= g.start && x <= g.start + g.span)
			{
				found = w;
				found_axis = 4;
			}
			if (w->kind == VGUI_PANEL || w->kind == VGUI_LIST ||
				w->kind == VGUI_TREE || w->kind == VGUI_MENU)
			{
				// Keep walking after a hit: ancestor bars are painted on top of
				// descendant bars where the two overlap, and the horizontal bar is
				// painted after the vertical one.
				if (x >= (vgui_is_rtl(w->ctx) ? w->abs_x : w->abs_x + w->abs_w - panel_hit) &&
					x <= (vgui_is_rtl(w->ctx) ? w->abs_x + panel_hit : w->abs_x + w->abs_w) &&
					y >= w->abs_y && y <= w->abs_y + w->abs_h &&
					vgui_scrollbar_geom(w, 1, &g) &&
					y >= g.start && y <= g.start + g.span)
				{
					found = w;
					found_axis = 1;
				}
				if (y >= w->abs_y + w->abs_h - panel_hit && y <= w->abs_y + w->abs_h &&
					x >= w->abs_x && x <= w->abs_x + w->abs_w &&
					vgui_scrollbar_geom(w, 2, &g) &&
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

	// True while the pointer is over one specific bar (or that bar is being
	// dragged).  Reuse vgui_scrollbar_hit so painting and hover feedback always
	// agree about the covered area, including the corner where two edit bars meet.
	static bool vgui_scrollbar_hovered(vgui_context* ctx, vgui_widget* w, int axis)
	{
		if (!ctx || !w || !w->enabled) return false;
		if (axis == 3 || axis == 4)
		{
			if (w->edit_scroll_drag == axis) return true;
		}
		else if (w->scroll_drag_axis == axis)
		{
			return true;
		}
		vgui_widget* from = ctx->hot;
		if (from && !ctx->alive.count(from)) from = nullptr;
		vgui_widget* sw = nullptr;
		int a = 0;
		return vgui_scrollbar_hit(from, ctx->mouse_x, ctx->mouse_y, &sw, &a) &&
			sw == w && a == axis;
	}

	static void vgui_scrollbar_begin(vgui_context* ctx, vgui_widget* w, int axis, float x, float y)
	{
		vgui_scrollbar_info g;
		if (!ctx || !w || !vgui_scrollbar_geom(w, axis, &g)) return;
		float pos = (axis == 2 || axis == 4) ? x : y;
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
		if (axis == 3 || axis == 4)
		{
			w->edit_scroll_drag = axis;
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
		int axis = (w->kind == VGUI_EDIT) ? w->edit_scroll_drag : w->scroll_drag_axis;
		vgui_scrollbar_info g;
		if (!vgui_scrollbar_geom(w, axis, &g)) return;
		float pos = (axis == 2 || axis == 4) ? x : y;
		float grab = (axis == 3 || axis == 4) ? w->edit_scroll_grab : w->scroll_drag_grab;
		vgui_scrollbar_apply(w, g, pos - grab);
	}

	//----------------------------------------------------------------------------
	// list / tree interaction
	//----------------------------------------------------------------------------

	static int vgui_list_index_at(vgui_widget* w, float x, float y)
	{
		vgui_list_state* st = vgui_list_of(w);
		if (!st) return -1;
		float ix = w->abs_x + w->padding[0];
		float iy = w->abs_y + w->padding[1];
		float iw = w->abs_w - w->padding[0] - w->padding[2];
		float ih = w->abs_h - w->padding[1] - w->padding[3];
		if (x < ix || x >= ix + iw || y < iy || y >= iy + ih) return -1;
		float local = y - iy + w->scroll_y;
		if (local < 0) return -1;
		int idx = (int)(local / vgui_list_row_height(w));
		return (idx >= 0 && idx < vgui_list_row_count(w, st)) ? idx : -1;
	}

	static int vgui_tree_row_at(vgui_widget* w, float x, float y)
	{
		vgui_tree_state* st = vgui_tree_of(w);
		if (!st) return -1;
		float ix = w->abs_x + w->padding[0];
		float iy = w->abs_y + w->padding[1];
		float iw = w->abs_w - w->padding[0] - w->padding[2];
		float ih = w->abs_h - w->padding[1] - w->padding[3];
		if (x < ix || x >= ix + iw || y < iy || y >= iy + ih) return -1;
		float local = y - iy + w->scroll_y;
		if (local < 0) return -1;
		int idx = (int)(local / vgui_tree_row_height(w));
		return (idx >= 0 && idx < vgui_tree_row_count(w, st)) ? idx : -1;
	}

	static bool vgui_tree_arrow_hit(vgui_widget* w, int row, float x)
	{
		vgui_tree_state* st = vgui_tree_of(w);
		if (!st || row < 0 || row >= vgui_tree_row_count(w, st)) return false;
		if (!vgui_tree_row_has_children(w, st, row)) return false;
		const vgui_tree_style& ts = w->ctx->style.tree;
		float depth = (float)vgui_tree_row_depth_at(w, st, row) * ts.indent;
		if (vgui_is_rtl(w->ctx))
		{
			float right = w->abs_x + w->abs_w - w->padding[2] - depth;
			return x <= right && x >= right - ts.arrow_size - ts.arrow_gap;
		}
		float tx = w->abs_x + w->padding[0] + depth;
		return x >= tx && x <= tx + ts.arrow_size + ts.arrow_gap;
	}

	// F04 行内按钮几何：按钮贴行尾（RTL 时贴行首），纵向居中，固定边距。
	// 可见垂直滚动条会占用行尾的命中区（scrollbar_hit 优先于行点击），
	// 按钮整体向内避让，避免永远点不到。
	static float vgui_row_button_size(vgui_widget* w, float row_h)
	{
		(void)w;
		return std::max(10.0f, std::min(18.0f, row_h - 8.0f));
	}

	static float vgui_row_scrollbar_reserve(vgui_widget* w)
	{
		const vgui_style& st = w->ctx->style;
		const float ih = std::max(0.0f, w->abs_h - w->padding[1] - w->padding[3]);
		if (w->scrollable_y && w->content_h > ih + 0.5f &&
			ih > st.panel.scrollbar_min_track)
			return st.panel.scrollbar_width + st.panel.scrollbar_inset +
				st.panel.scrollbar_hit_padding + 2.0f;
		return 0.0f;
	}

	static void vgui_row_button_rect(vgui_widget* w, float ix, float iw,
		float row_y, float row_h, int button, int total,
		float* out_x, float* out_size)
	{
		const float size = vgui_row_button_size(w, row_h);
		const float gap = 4.0f;
		const float margin = 4.0f;
		const float bar = vgui_row_scrollbar_reserve(w);
		float bx;
		if (vgui_is_rtl(w->ctx))
			bx = ix + bar + margin + (float)button * (size + gap);
		else
			bx = ix + iw - bar - margin - size - (float)(total - 1 - button) * (size + gap);
		*out_x = bx;
		*out_size = size;
	}

	static int vgui_row_buttons_hit_at(vgui_widget* w, float ix, float iw,
		float row_y, float row_h, int total, float x, float y)
	{
		if (total <= 0) return -1;
		const float size = vgui_row_button_size(w, row_h);
		const float cy = row_y + (row_h - size) * 0.5f;
		if (y < cy || y >= cy + size) return -1;
		for (int b = 0; b < total; ++b)
		{
			float bx = 0, bs = 0;
			vgui_row_button_rect(w, ix, iw, row_y, row_h, b, total, &bx, &bs);
			if (x >= bx && x < bx + bs) return b;
		}
		return -1;
	}

	static int vgui_list_button_hit(vgui_widget* w, int index, float x, float y)
	{
		vgui_list_state* st = vgui_list_of(w);
		if (!st || index < 0) return -1;
		vgui_list_item* item = vgui_list_item_at(w, index, nullptr);
		if (!item || item->button_count <= 0) return -1;
		const float ix = w->abs_x + w->padding[0];
		const float iy = w->abs_y + w->padding[1];
		const float iw = std::max(0.0f, w->abs_w - w->padding[0] - w->padding[2]);
		const float row_h = vgui_list_row_height(w);
		const float row_y = iy - w->scroll_y + (float)index * row_h;
		return vgui_row_buttons_hit_at(w, ix, iw, row_y, row_h,
			item->button_count, x, y);
	}

	static int vgui_tree_button_hit(vgui_widget* w, int row, float x, float y)
	{
		vgui_tree_state* st = vgui_tree_of(w);
		if (!st || row < 0) return -1;
		vgui_tree_node* node = vgui_tree_node_find(w, vgui_tree_row_id_at(w, st, row));
		if (!node || node->button_count <= 0) return -1;
		const float ix = w->abs_x + w->padding[0];
		const float iy = w->abs_y + w->padding[1];
		const float iw = std::max(0.0f, w->abs_w - w->padding[0] - w->padding[2]);
		const float row_h = vgui_tree_row_height(w);
		const float row_y = iy - w->scroll_y + (float)row * row_h;
		return vgui_row_buttons_hit_at(w, ix, iw, row_y, row_h,
			node->button_count, x, y);
	}

	static void vgui_list_ensure_visible(vgui_widget* w, vgui_item_id id, vgui_scroll_align align)
	{
		vgui_list_state* st = vgui_list_of(w);
		if (!st) return;
		int row_index = vgui_list_row_index(w, st, id);
		if (row_index < 0) return;
		float row_h = vgui_list_row_height(w);
		float ih = std::max(0.0f, w->abs_h - w->padding[1] - w->padding[3]);
		float top = (float)row_index * row_h;
		float bottom = top + row_h;
		float scroll = w->scroll_y;
		switch (align)
		{
		case VGUI_SCROLL_ALIGN_TOP: scroll = top; break;
		case VGUI_SCROLL_ALIGN_CENTER: scroll = top - (ih - row_h) * 0.5f; break;
		case VGUI_SCROLL_ALIGN_BOTTOM: scroll = bottom - ih; break;
		default:
			if (top < scroll) scroll = top;
			else if (bottom > scroll + ih) scroll = bottom - ih;
			break;
		}
		scroll = vgui_clampf(scroll, 0.0f, std::max(0.0f, w->content_h - ih));
		if (scroll != w->scroll_y)
		{
			w->scroll_y = scroll;
			vgui_mark_dirty(w);
			vgui_ensure_layout(w->ctx);
		}
	}

	static void vgui_tree_ensure_visible(vgui_widget* w, vgui_item_id id, vgui_scroll_align align)
	{
		vgui_tree_state* st = vgui_tree_of(w);
		int index = vgui_tree_visible_index(w, id);
		if (!st || index < 0) return;
		float row_h = vgui_tree_row_height(w);
		float ih = std::max(0.0f, w->abs_h - w->padding[1] - w->padding[3]);
		float top = (float)index * row_h;
		float bottom = top + row_h;
		float scroll = w->scroll_y;
		switch (align)
		{
		case VGUI_SCROLL_ALIGN_TOP: scroll = top; break;
		case VGUI_SCROLL_ALIGN_CENTER: scroll = top - (ih - row_h) * 0.5f; break;
		case VGUI_SCROLL_ALIGN_BOTTOM: scroll = bottom - ih; break;
		default:
			if (top < scroll) scroll = top;
			else if (bottom > scroll + ih) scroll = bottom - ih;
			break;
		}
		scroll = vgui_clampf(scroll, 0.0f, std::max(0.0f, w->content_h - ih));
		if (scroll != w->scroll_y)
		{
			w->scroll_y = scroll;
			vgui_mark_dirty(w);
			vgui_ensure_layout(w->ctx);
		}
	}

	static void vgui_list_apply_click(vgui_widget* w, vgui_item_id id, int mods)
	{
		vgui_list_state* st = vgui_list_of(w);
		if (!st || !vgui_list_item_find(w, id)) return;
		bool shift = (mods & VGUI_MOD_SHIFT) != 0;
		bool ctrl = (mods & VGUI_MOD_CTRL) != 0;
		st->cursor = id;
		if (st->selection_mode == VGUI_SELECTION_NONE)
		{
			st->range_anchor = id;
			return;
		}
		if (st->selection_mode == VGUI_SELECTION_SINGLE)
		{
			if (!vgui_id_selected(st->selected, id))
			{
				vgui_list_clear_selection_internal(w, true);
				if (!w->ctx || !w->ctx->alive.count(w)) return;
				vgui_list_set_selected_internal(w, id, true, true);
			}
			st->range_anchor = id;
			vgui_list_ensure_visible(w, id, VGUI_SCROLL_ALIGN_NEAREST);
			return;
		}

		if (shift && st->range_anchor != 0)
		{
			if (!ctrl) vgui_list_clear_selection_internal(w, true);
			if (!w->ctx || !w->ctx->alive.count(w)) return;
			size_t ai = (size_t)-1, ti = (size_t)-1;
			vgui_list_item_find(w, st->range_anchor, &ai);
			vgui_list_item_find(w, id, &ti);
			if (ai == (size_t)-1) ai = ti;
			if (ti == (size_t)-1) return;
			size_t lo = std::min(ai, ti), hi = std::max(ai, ti);
			int count = vgui_list_row_count(w, st);
			if (count <= 0) return;
			if (hi >= (size_t)count) hi = (size_t)count - 1;
			std::vector<vgui_item_id> range;
			for (size_t i = lo; i <= hi; ++i) range.push_back(vgui_list_row_id(w, st, (int)i));
			for (vgui_item_id rid : range)
			{
				st = vgui_list_of(w);
				if (!st) return;
				vgui_list_item* it = vgui_list_item_find(w, rid);
				if (!it || !it->enabled) continue;
				vgui_list_set_selected_internal(w, rid, true, true);
				if (!w->ctx || !w->ctx->alive.count(w)) return;
			}
		}
		else if (ctrl)
		{
			bool sel = vgui_id_selected(st->selected, id);
			vgui_list_set_selected_internal(w, id, !sel, true);
			st->range_anchor = id;
		}
		else
		{
			vgui_list_clear_selection_internal(w, true);
			if (!w->ctx || !w->ctx->alive.count(w)) return;
			vgui_list_set_selected_internal(w, id, true, true);
			st->range_anchor = id;
		}
		if (w->ctx && w->ctx->alive.count(w))
			vgui_list_ensure_visible(w, id, VGUI_SCROLL_ALIGN_NEAREST);
	}

	static void vgui_tree_apply_click(vgui_widget* w, vgui_item_id id, int mods)
	{
		vgui_tree_state* st = vgui_tree_of(w);
		if (!st || !vgui_tree_node_find(w, id)) return;
		bool shift = (mods & VGUI_MOD_SHIFT) != 0;
		bool ctrl = (mods & VGUI_MOD_CTRL) != 0;
		st->cursor = id;
		if (st->selection_mode == VGUI_SELECTION_NONE)
		{
			st->range_anchor = id;
			return;
		}
		if (st->selection_mode == VGUI_SELECTION_SINGLE)
		{
			if (!vgui_id_selected(st->selected, id))
			{
				vgui_tree_clear_selection_internal(w, true);
				if (!w->ctx || !w->ctx->alive.count(w)) return;
				vgui_tree_set_selected_internal(w, id, true, true);
			}
			st->range_anchor = id;
			return;
		}

		if (shift && st->range_anchor != 0)
		{
			if (!ctrl) vgui_tree_clear_selection_internal(w, true);
			if (!w->ctx || !w->ctx->alive.count(w)) return;
			int ai = vgui_tree_visible_index(w, st->range_anchor);
			int ti = vgui_tree_visible_index(w, id);
			if (ai < 0) ai = ti;
			if (ti < 0) return;
			vgui_tree_rebuild_rows(w);
			int lo = std::min(ai, ti), hi = std::max(ai, ti);
			int count = vgui_tree_row_count(w, st);
			if (count <= 0) return;
			if (hi >= count) hi = count - 1;
			std::vector<vgui_item_id> range;
			for (int i = lo; i <= hi; ++i) range.push_back(vgui_tree_row_id_at(w, st, i));
			for (vgui_item_id rid : range)
			{
				st = vgui_tree_of(w);
				if (!st) return;
				vgui_tree_node* n = vgui_tree_node_find(w, rid);
				if (!n || !n->enabled) continue;
				vgui_tree_set_selected_internal(w, rid, true, true);
				if (!w->ctx || !w->ctx->alive.count(w)) return;
			}
		}
		else if (ctrl)
		{
			bool sel = vgui_id_selected(st->selected, id);
			vgui_tree_set_selected_internal(w, id, !sel, true);
			st->range_anchor = id;
		}
		else
		{
			vgui_tree_clear_selection_internal(w, true);
			if (!w->ctx || !w->ctx->alive.count(w)) return;
			vgui_tree_set_selected_internal(w, id, true, true);
			st->range_anchor = id;
		}
	}

	static void vgui_list_activate(vgui_widget* w, vgui_item_id id)
	{
		vgui_list_state* st = vgui_list_of(w);
		if (!st || !vgui_list_item_find(w, id)) return;
		size_t index = (size_t)-1;
		vgui_list_item_find(w, id, &index);
		if (st->activate_fn)
			st->activate_fn(w, id, st->activate_user);
		if (w->ctx && w->ctx->alive.count(w))
			vgui_item_event(w, VGUI_EVENT_SUBMIT, id, (int)index, 0, 0);
	}

	static void vgui_tree_activate(vgui_widget* w, vgui_item_id id)
	{
		vgui_tree_state* st = vgui_tree_of(w);
		if (!st || !vgui_tree_node_find(w, id)) return;
		int index = vgui_tree_visible_index(w, id);
		if (st->activate_fn)
			st->activate_fn(w, id, st->activate_user);
		if (w->ctx && w->ctx->alive.count(w))
			vgui_item_event(w, VGUI_EVENT_SUBMIT, id, index, 0, 0);
	}

	static int vgui_list_next_index(vgui_widget* w, int from, int delta)
	{
		vgui_list_state* st = vgui_list_of(w);
		if (!st) return -1;
		int n = vgui_list_row_count(w, st);
		if (n <= 0) return -1;
		if (from < 0) from = delta > 0 ? -1 : n;
		for (int step = 0; step < n; ++step)
		{
			from += delta;
			if (from < 0) from = 0;
			if (from >= n) from = n - 1;
			if (vgui_list_row_enabled(w, st, from)) return from;
			if ((delta > 0 && from >= n - 1) || (delta < 0 && from <= 0)) break;
		}
		return -1;
	}

	// 切换某可见行的展开状态。虚拟模式交给宿主的 row_toggle，实体模式直接改节点。
	static bool vgui_tree_toggle_row(vgui_widget* w, int row)
	{
		vgui_tree_state* st = vgui_tree_of(w);
		if (!st || row < 0 || row >= vgui_tree_row_count(w, st)) return false;
		if (st->virtual_mode)
		{
			if (!st->source.row_toggle) return false;
			st->source.row_toggle(st->source.user, row);
			// 宿主可能已在回调里刷新过；这里兜底一次，保证行数和最宽行失效。
			st = vgui_tree_of(w);
			if (st)
			{
				st->rows_dirty = true;
				st->content_w_valid = false;
			}
			vgui_mark_dirty(w);
			return true;
		}
		vgui_tree_node* n = vgui_tree_node_find(w, vgui_tree_row_id_at(w, st, row));
		if (!n || n->children.empty()) return false;
		n->expanded = n->expanded ? 0 : 1;
		st->rows_dirty = true;
		st->content_w_valid = false;
		vgui_mark_dirty(w);
		return true;
	}

	static int vgui_tree_next_index(vgui_widget* w, int from, int delta)
	{
		vgui_tree_state* st = vgui_tree_of(w);
		if (!st) return -1;
		int n = vgui_tree_row_count(w, st);
		if (n == 0) return -1;
		if (from < 0) from = delta > 0 ? -1 : n;
		for (int step = 0; step < n; ++step)
		{
			from += delta;
			if (from < 0) from = 0;
			if (from >= n) from = n - 1;
			if (vgui_tree_row_enabled_at(w, st, from)) return from;
			if ((delta > 0 && from >= n - 1) || (delta < 0 && from <= 0)) break;
		}
		return -1;
	}

	static bool vgui_list_key(vgui_widget* w, int key, int down, int mods)
	{
		vgui_list_state* st = vgui_list_of(w);
		if (!st) return false;
		if (!down) return true;
		bool shift = (mods & VGUI_MOD_SHIFT) != 0;
		bool ctrl = (mods & VGUI_MOD_CTRL) != 0;
		int n = vgui_list_row_count(w, st);
		int idx = -1;
		vgui_item_id cid = st->cursor;
		size_t ci = (size_t)-1;
		if (vgui_list_item_find(w, cid, &ci)) idx = (int)ci;

		if (ctrl && key == 'A')
		{
			if (st->selection_mode == VGUI_SELECTION_MULTIPLE)
			{
				/* 批量全选：先把候选 id 收集好，再用哈希集合去重，最后逐条
				 * 通知。原来的逐条 set_selected 每次都线性扫描 selected，
				 * 大列表上退化成 O(N²)。 */
				std::unordered_set<vgui_item_id> have(st->selected.begin(), st->selected.end());
				std::vector<vgui_item_id> ids;
				ids.reserve((size_t)n);
				for (int i = 0; i < n; ++i)
				{
					if (!vgui_list_row_enabled(w, st, i)) continue;
					vgui_item_id rid = vgui_list_row_id(w, st, i);
					if (rid != 0 && !have.count(rid)) ids.push_back(rid);
				}
				for (vgui_item_id id : ids)
				{
					st = vgui_list_of(w);
					if (!st) return true;
					if (!vgui_list_item_find(w, id)) continue;
					st->selected.push_back(id);
					vgui_list_notify_select(w, id, 1);
					if (!w->ctx || !w->ctx->alive.count(w)) return true;
				}
			}
			return true;
		}

		if (key == VGUI_KEY_UP || key == VGUI_KEY_DOWN || key == VGUI_KEY_HOME ||
			key == VGUI_KEY_END || key == VGUI_KEY_PAGE_UP || key == VGUI_KEY_PAGE_DOWN)
		{
			int ni = idx;
			if (key == VGUI_KEY_HOME) ni = vgui_list_next_index(w, -1, 1);
			else if (key == VGUI_KEY_END) ni = vgui_list_next_index(w, n, -1);
			else
			{
				int delta = (key == VGUI_KEY_UP || key == VGUI_KEY_PAGE_UP) ? -1 : 1;
				int steps = 1;
				if (key == VGUI_KEY_PAGE_UP || key == VGUI_KEY_PAGE_DOWN)
				{
					float ih = std::max(1.0f, w->abs_h - w->padding[1] - w->padding[3]);
					steps = std::max(1, (int)(ih / vgui_list_row_height(w)));
				}
				for (int i = 0; i < steps && ni >= 0; ++i)
					ni = vgui_list_next_index(w, ni, delta);
				if (ni < 0 && idx >= 0) ni = idx;
			}
			if (ni >= 0 && ni < n)
			{
				vgui_item_id nid = vgui_list_row_id(w, st, ni);
				st->cursor = nid;
				if (st->selection_mode == VGUI_SELECTION_SINGLE)
					vgui_list_apply_click(w, nid, mods);
				else if (st->selection_mode == VGUI_SELECTION_MULTIPLE && shift)
				{
					if (st->range_anchor == 0) st->range_anchor = nid;
					vgui_list_apply_click(w, nid, VGUI_MOD_SHIFT | (ctrl ? VGUI_MOD_CTRL : 0));
				}
				if (w->ctx && w->ctx->alive.count(w))
					vgui_list_ensure_visible(w, nid, VGUI_SCROLL_ALIGN_NEAREST);
			}
			return true;
		}
		if (key == ' ')
		{
			if (idx >= 0)
			{
				vgui_item_id id = vgui_list_row_id(w, st, idx);
				if (st->selection_mode == VGUI_SELECTION_MULTIPLE)
					vgui_list_set_selected_internal(w, id, !vgui_id_selected(st->selected, id), true);
				else
					vgui_list_apply_click(w, id, mods);
				if (w->ctx && w->ctx->alive.count(w))
					vgui_list_ensure_visible(w, id, VGUI_SCROLL_ALIGN_NEAREST);
			}
			return true;
		}
		if (key == VGUI_KEY_ENTER)
		{
			if (idx >= 0) vgui_list_activate(w, vgui_list_row_id(w, st, idx));
			return true;
		}
		return false;
	}

	static bool vgui_tree_key(vgui_widget* w, int key, int down, int mods)
	{
		vgui_tree_state* st = vgui_tree_of(w);
		if (!st) return false;
		if (!down) return true;
		bool shift = (mods & VGUI_MOD_SHIFT) != 0;
		bool ctrl = (mods & VGUI_MOD_CTRL) != 0;
		int n = vgui_tree_row_count(w, st);
		int idx = vgui_tree_visible_index(w, st->cursor);

		if (ctrl && key == 'A')
		{
			if (st->selection_mode == VGUI_SELECTION_MULTIPLE)
			{
				/* 批量全选：候选行只收集一次并用哈希集合去重，避免逐条
				 * set_selected 里对 selected / rows 的线性扫描。 */
				std::unordered_set<vgui_item_id> have(st->selected.begin(), st->selected.end());
				std::vector<vgui_item_id> ids;
				ids.reserve((size_t)n);
				for (int i = 0; i < n; ++i)
				{
					vgui_item_id rid = vgui_tree_row_id_at(w, st, i);
					if (rid == 0 || have.count(rid)) continue;
					if (!vgui_tree_row_enabled_at(w, st, i)) continue;
					ids.push_back(rid);
				}
				for (vgui_item_id id : ids)
				{
					st = vgui_tree_of(w);
					if (!st) return true;
					if (!vgui_tree_node_find(w, id)) continue;
					st->selected.push_back(id);
					vgui_tree_notify_select(w, id, 1);
					if (!w->ctx || !w->ctx->alive.count(w)) return true;
				}
			}
			return true;
		}

		if (key == VGUI_KEY_UP || key == VGUI_KEY_DOWN || key == VGUI_KEY_HOME ||
			key == VGUI_KEY_END || key == VGUI_KEY_PAGE_UP || key == VGUI_KEY_PAGE_DOWN)
		{
			int ni = idx;
			if (key == VGUI_KEY_HOME) ni = vgui_tree_next_index(w, -1, 1);
			else if (key == VGUI_KEY_END) ni = vgui_tree_next_index(w, n, -1);
			else
			{
				int delta = (key == VGUI_KEY_UP || key == VGUI_KEY_PAGE_UP) ? -1 : 1;
				int steps = 1;
				if (key == VGUI_KEY_PAGE_UP || key == VGUI_KEY_PAGE_DOWN)
				{
					float ih = std::max(1.0f, w->abs_h - w->padding[1] - w->padding[3]);
					steps = std::max(1, (int)(ih / vgui_tree_row_height(w)));
				}
				for (int i = 0; i < steps && ni >= 0; ++i)
					ni = vgui_tree_next_index(w, ni, delta);
				if (ni < 0 && idx >= 0) ni = idx;
			}
			if (ni >= 0 && ni < n)
			{
				vgui_item_id nid = vgui_tree_row_id_at(w, st, ni);
				st->cursor = nid;
				if (st->selection_mode == VGUI_SELECTION_SINGLE)
					vgui_tree_apply_click(w, nid, mods);
				else if (st->selection_mode == VGUI_SELECTION_MULTIPLE && shift)
				{
					if (st->range_anchor == 0) st->range_anchor = nid;
					vgui_tree_apply_click(w, nid, VGUI_MOD_SHIFT | (ctrl ? VGUI_MOD_CTRL : 0));
				}
				if (w->ctx && w->ctx->alive.count(w))
					vgui_tree_ensure_visible(w, nid, VGUI_SCROLL_ALIGN_NEAREST);
			}
			return true;
		}

		if (idx >= 0 && idx < n)
		{
			vgui_item_id id = vgui_tree_row_id_at(w, st, idx);
			vgui_tree_node* node = vgui_tree_node_find(w, id);
			if (key == VGUI_KEY_LEFT)
			{
				if (vgui_tree_row_expanded(w, st, idx))
				{
					vgui_tree_toggle_row(w, idx);
				}
				else if (!st->virtual_mode && node && node->parent)
				{
					st->cursor = node->parent;
					vgui_tree_expand_to(w, node->parent);
					if (st->selection_mode == VGUI_SELECTION_SINGLE)
						vgui_tree_apply_click(w, node->parent, mods);
					vgui_tree_ensure_visible(w, node->parent, VGUI_SCROLL_ALIGN_NEAREST);
				}
				return true;
			}
			if (key == VGUI_KEY_RIGHT)
			{
				if (vgui_tree_row_has_children(w, st, idx))
				{
					if (!vgui_tree_row_expanded(w, st, idx))
					{
						vgui_tree_toggle_row(w, idx);
					}
					else
					{
						int ni = vgui_tree_next_index(w, idx, 1);
						if (ni >= 0 && ni < n)
						{
							st->cursor = vgui_tree_row_id_at(w, st, ni);
							if (st->selection_mode == VGUI_SELECTION_SINGLE)
								vgui_tree_apply_click(w, st->cursor, mods);
							vgui_tree_ensure_visible(w, st->cursor, VGUI_SCROLL_ALIGN_NEAREST);
						}
					}
				}
				return true;
			}
			if (key == ' ')
			{
				if (vgui_tree_row_has_children(w, st, idx))
				{
					vgui_tree_toggle_row(w, idx);
				}
				else if (st->selection_mode == VGUI_SELECTION_MULTIPLE)
					vgui_tree_set_selected_internal(w, id, !vgui_id_selected(st->selected, id), true);
				else
					vgui_tree_apply_click(w, id, mods);
				return true;
			}
			if (key == VGUI_KEY_ENTER)
			{
				vgui_tree_activate(w, id);
				return true;
			}
		}
		return false;
	}

	//----------------------------------------------------------------------------
	// menu popup helpers
	//----------------------------------------------------------------------------

	static void vgui_menu_close_internal(vgui_widget* menu)
	{
		if (!menu || menu->kind != VGUI_MENU || !menu->menu) return;
		vgui_context* ctx = menu->ctx;
		vgui_menu_state* st = menu->menu.get();
		vgui_widget* sub = st->submenu_open;
		bool was_open = st->open != 0;
		st->submenu_open = nullptr;
		st->submenu_item = 0;
		st->pending_submenu_item = 0;
		if (sub && ctx && ctx->alive.count(sub))
			vgui_menu_close_internal(sub);
		if (!ctx || !ctx->alive.count(menu)) return;
		st = menu->menu.get();
		if (!st) return;
		if (st->parent_menu && st->parent_menu->menu)
		{
			vgui_menu_state* parent = st->parent_menu->menu.get();
			if (parent->submenu_open == menu) parent->submenu_open = nullptr;
			if (parent->submenu_item == st->parent_item) parent->submenu_item = 0;
		}
		if (ctx->open_menu == menu) ctx->open_menu = nullptr;
		if (ctx->menu_pressed == menu) ctx->menu_pressed = nullptr;
		menu->visible = 0;
		menu->pressed = 0;
		st->open = 0;
		st->cursor = 0;
		st->hover = 0;
		st->pressed = 0;
		if (st->owner_menubar && st->owner_item && ctx->alive.count(st->owner_menubar) &&
			st->owner_menubar->kind == VGUI_MENUBAR && st->owner_menubar->menubar)
		{
			vgui_menubar_state* bs = st->owner_menubar->menubar.get();
			if (bs->active == st->owner_item) bs->active = 0;
		}
		if (was_open && st->close_fn)
			st->close_fn(menu, st->close_user);
	}

	static void vgui_menu_close_all(vgui_context* ctx)
	{
		if (!ctx || !ctx->open_menu) return;
		vgui_widget* menu = ctx->open_menu;
		ctx->open_menu = nullptr;
		if (ctx->alive.count(menu))
			vgui_menu_close_internal(menu);
	}

	static vgui_item_id vgui_menu_first_selectable(vgui_widget* menu, bool reverse)
	{
		vgui_menu_state* st = vgui_menu_of(menu);
		if (!st) return 0;
		if (reverse)
		{
			for (int i = (int)st->items.size() - 1; i >= 0; --i)
				if (st->items[(size_t)i].enabled && st->items[(size_t)i].type != VGUI_MENU_ITEM_SEPARATOR)
					return st->items[(size_t)i].id;
		}
		else
		{
			for (const vgui_menu_item& it : st->items)
				if (it.enabled && it.type != VGUI_MENU_ITEM_SEPARATOR)
					return it.id;
		}
		return 0;
	}

	static void vgui_menu_open_submenu(vgui_widget* parent, vgui_item_id item_id)
	{
		vgui_menu_state* ps = vgui_menu_of(parent);
		if (!ps || !parent->ctx) return;
		auto it = ps->index.find(item_id);
		if (it == ps->index.end() || it->second >= ps->items.size()) return;
		vgui_menu_item& item = ps->items[it->second];
		if (item.type != VGUI_MENU_ITEM_SUBMENU || !item.submenu || !item.enabled) return;
		if (ps->submenu_open == item.submenu && item.submenu->menu && item.submenu->menu->open) return;
		vgui_context* ctx = parent->ctx;
		if (ps->submenu_open && ps->submenu_open != item.submenu)
			vgui_menu_close_internal(ps->submenu_open);
		if (!ctx || !ctx->alive.count(parent) || parent->destroy_pending) return;
		ps = vgui_menu_of(parent);
		if (!ps) return;
		it = ps->index.find(item_id);
		if (it == ps->index.end()) return;
		vgui_widget* sub = ps->items[it->second].submenu;
		if (!sub || sub->kind != VGUI_MENU || !sub->menu || sub->destroy_pending) return;
		ps->submenu_item = item_id;
		ps->submenu_open = sub;
		ps->pending_submenu_item = 0;
		vgui_menu_state* ss = sub->menu.get();
		if (ss->parent_menu && ss->parent_menu != parent && ss->parent_menu->menu)
		{
			vgui_menu_state* old = ss->parent_menu->menu.get();
			if (old->submenu_open == sub) old->submenu_open = nullptr;
		}
		ss->parent_menu = parent;
		ss->parent_item = item_id;
		ss->open = 1;
		ss->has_open_pos = 0;
		ss->open_x = 0;
		ss->open_y = 0;
		sub->scroll_y = 0;
		ss->cursor = vgui_menu_first_selectable(sub, false);
		ss->hover = 0;
		ss->pressed = 0;
		sub->visible = 1;
		if (ss->open_fn) ss->open_fn(sub, ss->open_user);
		if (!ctx->alive.count(sub)) return;
		ctx->layout_dirty = true;
		if (ctx->open_menu && ctx->alive.count(parent))
			vgui_menu_update_layout(ctx->open_menu, ctx);
	}

	static void vgui_menu_close_submenu(vgui_widget* parent)
	{
		vgui_menu_state* ps = vgui_menu_of(parent);
		if (!ps || !ps->submenu_open) return;
		vgui_widget* sub = ps->submenu_open;
		ps->submenu_open = nullptr;
		ps->submenu_item = 0;
		ps->pending_submenu_item = 0;
		if (parent->ctx && parent->ctx->alive.count(sub))
			vgui_menu_close_internal(sub);
	}

	static void vgui_menu_show(vgui_widget* menu, float x, float y)
	{
		vgui_menu_state* st = vgui_menu_of(menu);
		if (!st || !menu->ctx || menu->destroy_pending) return;
		vgui_context* ctx = menu->ctx;
		vgui_tooltip_clear_candidate(ctx);
		if (ctx->open_menu && ctx->open_menu != menu)
			vgui_menu_close_all(ctx);
		if (ctx->popup.content)
			vgui_popup_close_internal(ctx, VGUI_POPUP_CLOSE_EXPLICIT);
		if (!ctx->alive.count(menu)) return;
		st = vgui_menu_of(menu);
		if (!st) return;
		if (st->submenu_open) vgui_menu_close_submenu(menu);
		if (!ctx->alive.count(menu)) return;
		st = vgui_menu_of(menu);
		if (!st) return;
		if (st->parent_menu && st->parent_menu->menu)
		{
			vgui_menu_state* parent = st->parent_menu->menu.get();
			if (parent->submenu_open == menu) parent->submenu_open = nullptr;
		}
		st->parent_menu = nullptr;
		st->parent_item = 0;
		st->open = 1;
		st->has_open_pos = 1;
		st->open_x = x;
		st->open_y = y;
		menu->scroll_y = 0;
		st->cursor = vgui_menu_first_selectable(menu, false);
		st->hover = 0;
		st->pressed = 0;
		st->pending_submenu_item = 0;
		menu->visible = 1;
		menu->pressed = 0;
		ctx->open_menu = menu;
		if (st->open_fn) st->open_fn(menu, st->open_user);
		if (!ctx->alive.count(menu)) return;
		ctx->layout_dirty = true;
		vgui_ensure_layout(ctx);
	}

	static void vgui_menu_show_below(vgui_widget* menu, vgui_widget* anchor)
	{
		if (!menu || !anchor || menu->ctx != anchor->ctx) return;
		vgui_context* ctx = menu->ctx;
		vgui_ensure_layout(ctx);
		float cw = 0, ch = 0;
		vgui_measure_widget(menu, ctx);
		vgui_menu_content_size(menu, &cw, &ch);
		float x = vgui_is_rtl(ctx) ? anchor->abs_x + anchor->abs_w - cw : anchor->abs_x;
		float y = anchor->abs_y + anchor->abs_h;
		if (ctx->height > 0 && y + ch > (float)ctx->height - 8.0f &&
			anchor->abs_y - ch >= 8.0f)
			y = anchor->abs_y - ch;
		vgui_menu_show(menu, x, y);
	}

	static void vgui_menu_show_for(vgui_widget* menu, vgui_widget* anchor, float local_x, float local_y)
	{
		if (!menu || !anchor || menu->ctx != anchor->ctx) return;
		vgui_context* ctx = menu->ctx;
		vgui_ensure_layout(ctx);
		float cw = 0, ch = 0;
		vgui_measure_widget(menu, ctx);
		vgui_menu_content_size(menu, &cw, &ch);
		float x = anchor->abs_x + local_x;
		float y = anchor->abs_y + local_y;
		if (ctx->width > 0 && x + cw > (float)ctx->width - 8.0f)
			x = (float)ctx->width - cw - 8.0f;
		if (ctx->height > 0 && y + ch > (float)ctx->height - 8.0f)
			y = (float)ctx->height - ch - 8.0f;
		vgui_menu_show(menu, x, y);
	}

	static vgui_widget* vgui_menu_owner_menubar(vgui_widget* menu)
	{
		vgui_menu_state* st = vgui_menu_of(menu);
		if (!st || !st->owner_menubar) return nullptr;
		vgui_widget* bar = st->owner_menubar;
		if (!bar->ctx || !bar->ctx->alive.count(bar) || bar->kind != VGUI_MENUBAR) return nullptr;
		return bar;
	}

	static vgui_item_id vgui_menubar_item_at_point(vgui_widget* bar, float x, float y)
	{
		vgui_menubar_state* st = vgui_menubar_of(bar);
		if (!st || !bar) return 0;
		if (x < bar->abs_x || y < bar->abs_y ||
			x >= bar->abs_x + bar->abs_w || y >= bar->abs_y + bar->abs_h)
			return 0;
		for (size_t i = 0; i < st->items.size(); ++i)
		{
			float ix = 0, iy = 0, iw = 0, ih = 0;
			vgui_menubar_item_rect_at(bar, i, &ix, &iy, &iw, &ih);
			if (x >= ix && x < ix + iw && y >= iy && y < iy + ih)
				return st->items[i].id;
		}
		return 0;
	}

	static vgui_item_id vgui_menubar_step(vgui_widget* bar, int delta)
	{
		vgui_menubar_state* st = vgui_menubar_of(bar);
		if (!st || st->items.empty()) return 0;
		int count = (int)st->items.size();
		int start = -1;
		for (int i = 0; i < count; ++i)
			if (st->items[(size_t)i].id == st->active) { start = i; break; }
		if (start < 0)
			start = delta >= 0 ? -1 : 0;
		for (int step = 1; step <= count; ++step)
		{
			int next = start + delta * step;
			next %= count;
			if (next < 0) next += count;
			if (st->items[(size_t)next].enabled && st->items[(size_t)next].menu)
				return st->items[(size_t)next].id;
		}
		return 0;
	}

	static void vgui_menubar_show(vgui_widget* bar, vgui_item_id item)
	{
		vgui_menubar_state* st = vgui_menubar_of(bar);
		if (!st || !bar->ctx) return;
		auto it = st->index.find(item);
		if (it == st->index.end() || it->second >= st->items.size()) return;
		vgui_menubar_item& entry = st->items[it->second];
		if (!entry.enabled || !entry.menu) return;
		vgui_widget* menu = entry.menu;
		size_t index = it->second;
		float x = 0, y = 0, w = 0, h = 0;
		vgui_menubar_item_rect_at(bar, index, &x, &y, &w, &h);
		vgui_menu_show(menu, x, y + h);
		vgui_context* ctx = bar->ctx;
		if (!ctx || !ctx->alive.count(bar) || !ctx->alive.count(menu)) return;
		st = vgui_menubar_of(bar);
		vgui_menu_state* ms = vgui_menu_of(menu);
		if (!st || !ms || !ms->open) return;
		st->active = item;
		st->hover = item;
	}

	static vgui_widget* vgui_menu_hit_rec(vgui_widget* menu, float x, float y)
	{
		vgui_menu_state* st = vgui_menu_of(menu);
		if (!st || !st->open || !menu->visible) return nullptr;
		if (st->submenu_open && st->submenu_open->menu && st->submenu_open->menu->open)
		{
			vgui_widget* r = vgui_menu_hit_rec(st->submenu_open, x, y);
			if (r) return r;
		}
		if (vgui_point_in(menu, x, y)) return menu;
		return nullptr;
	}

	static vgui_widget* vgui_menu_deepest(vgui_widget* menu)
	{
		vgui_widget* cur = menu;
		while (cur && cur->menu && cur->menu->submenu_open &&
			cur->menu->submenu_open->menu && cur->menu->submenu_open->menu->open)
			cur = cur->menu->submenu_open;
		return cur;
	}

	static int vgui_menu_index_at(vgui_widget* menu, float y)
	{
		vgui_menu_state* st = vgui_menu_of(menu);
		if (!st) return -1;
		const vgui_menu_style& ms = menu->ctx->style.menu;
		float top = menu->abs_y + ms.padding_y - menu->scroll_y;
		for (size_t i = 0; i < st->items.size(); ++i)
		{
			float h = vgui_menu_item_height(menu, st->items[i]);
			if (y >= top && y < top + h) return (int)i;
			top += h;
		}
		return -1;
	}

	static vgui_item_id vgui_menu_item_at(vgui_widget* menu, float x, float y, int* out_index = nullptr)
	{
		if (!menu || !vgui_point_in(menu, x, y)) return 0;
		int idx = vgui_menu_index_at(menu, y);
		vgui_menu_state* st = vgui_menu_of(menu);
		if (!st || idx < 0 || idx >= (int)st->items.size()) return 0;
		if (out_index) *out_index = idx;
		return st->items[(size_t)idx].id;
	}

	static void vgui_menu_set_hover(vgui_widget* menu, vgui_item_id id)
	{
		vgui_menu_state* st = vgui_menu_of(menu);
		if (!st) return;
		if (st->hover == id) return;
		st->hover = id;
		vgui_item_id pending = 0;
		if (id)
		{
			vgui_menu_item* it = nullptr;
			auto p = st->index.find(id);
			if (p != st->index.end() && p->second < st->items.size()) it = &st->items[p->second];
			if (it && it->type == VGUI_MENU_ITEM_SUBMENU && it->enabled && it->submenu)
			{
				pending = id;
			}
			else if (st->submenu_open)
			{
				vgui_menu_close_submenu(menu);
			}
		}
		if (pending != st->pending_submenu_item)
		{
			st->pending_submenu_item = pending;
			st->pending_submenu_time = menu->ctx ? menu->ctx->time : 0.0;
		}
	}

	static void vgui_menu_mouse_move(vgui_context* ctx, float x, float y)
	{
		if (!ctx || !ctx->open_menu) return;
		vgui_widget* hit = vgui_menu_hit_rec(ctx->open_menu, x, y);
		if (!hit) return;
		int idx = -1;
		vgui_item_id id = vgui_menu_item_at(hit, x, y, &idx);
		if (id)
		{
			vgui_menu_state* st = vgui_menu_of(hit);
			if (st && st->items[(size_t)idx].type == VGUI_MENU_ITEM_SEPARATOR)
				id = 0;
		}
		vgui_menu_set_hover(hit, id);
	}

	static void vgui_menu_mouse_down(vgui_context* ctx, int button, float x, float y)
	{
		if (!ctx || !ctx->open_menu || button != 1) return;
		vgui_widget* hit = vgui_menu_hit_rec(ctx->open_menu, x, y);
		if (!hit)
		{
			vgui_menu_close_all(ctx);
			ctx->menu_swallow_up = 1;
			return;
		}
		ctx->menu_swallow_up = 0;
		vgui_widget* sw = nullptr;
		int axis = 0;
		if (vgui_scrollbar_hit(hit, x, y, &sw, &axis) && sw == hit)
		{
			vgui_scrollbar_begin(ctx, sw, axis, x, y);
			return;
		}
		int idx = -1;
		vgui_item_id id = vgui_menu_item_at(hit, x, y, &idx);
		vgui_menu_state* st = vgui_menu_of(hit);
		if (!st || !id) return;
		const vgui_menu_item& item = st->items[(size_t)idx];
		if (!item.enabled || item.type == VGUI_MENU_ITEM_SEPARATOR) return;
		st->cursor = id;
		st->hover = id;
		if (item.type == VGUI_MENU_ITEM_SUBMENU)
		{
			vgui_menu_open_submenu(hit, id);
			return;
		}
		st->pressed = id;
		hit->pressed = 1;
		hit->pressed_button = button;
		ctx->menu_pressed = hit;
	}

	static void vgui_menu_activate_item(vgui_widget* menu, vgui_item_id id)
	{
		vgui_menu_state* st = vgui_menu_of(menu);
		if (!st || !menu->ctx) return;
		auto p = st->index.find(id);
		if (p == st->index.end() || p->second >= st->items.size()) return;
		vgui_menu_item& item = st->items[p->second];
		if (!item.enabled || item.type == VGUI_MENU_ITEM_SEPARATOR) return;
		if (item.type == VGUI_MENU_ITEM_SUBMENU)
		{
			vgui_menu_open_submenu(menu, id);
			return;
		}
		if (item.checkable) item.checked = !item.checked;
		vgui_context* ctx = menu->ctx;
		int index = (int)p->second;
		if (st->activate_fn)
			st->activate_fn(menu, id, st->activate_user);
		if (ctx->alive.count(menu))
			vgui_item_event(menu, VGUI_EVENT_CLICK, id, index, 0, 0);
		if (!ctx->alive.count(menu)) return;
		if (st->close_on_activate)
			vgui_menu_close_all(ctx);
	}

	static void vgui_menu_mouse_up(vgui_context* ctx, int button, float x, float y)
	{
		if (!ctx || button != 1) return;
		if (!ctx->open_menu)
		{
			ctx->menu_pressed = nullptr;
			return;
		}
		vgui_widget* pressed_menu = ctx->menu_pressed;
		vgui_item_id pressed_id = 0;
		if (pressed_menu && ctx->alive.count(pressed_menu) && pressed_menu->menu)
		{
			pressed_id = pressed_menu->menu->pressed;
			pressed_menu->menu->pressed = 0;
			pressed_menu->pressed = 0;
		}
		ctx->menu_pressed = nullptr;
		if (!pressed_menu || !pressed_id || !ctx->alive.count(pressed_menu)) return;
		vgui_widget* hit = vgui_menu_hit_rec(ctx->open_menu, x, y);
		int idx = -1;
		vgui_item_id id = hit ? vgui_menu_item_at(hit, x, y, &idx) : 0;
		if (hit == pressed_menu && id == pressed_id)
			vgui_menu_activate_item(pressed_menu, id);
	}

	static bool vgui_menu_move_cursor(vgui_widget* menu, int delta)
	{
		vgui_menu_state* st = vgui_menu_of(menu);
		if (!st || st->items.empty()) return false;
		int n = (int)st->items.size();
		int cur = -1;
		auto p = st->index.find(st->cursor);
		if (p != st->index.end()) cur = (int)p->second;
		for (int step = 0; step < n; ++step)
		{
			cur += delta;
			if (cur < 0) cur = n - 1;
			if (cur >= n) cur = 0;
			const vgui_menu_item& it = st->items[(size_t)cur];
			if (it.enabled && it.type != VGUI_MENU_ITEM_SEPARATOR)
			{
				st->cursor = it.id;
				st->hover = it.id;
				if (st->submenu_open && st->submenu_item != it.id)
					vgui_menu_close_submenu(menu);
				return true;
			}
		}
		return false;
	}

	static bool vgui_menu_key(vgui_context* ctx, int key, int down)
	{
		if (!ctx || !ctx->open_menu) return false;
		if (!down) return true;
		vgui_widget* menu = vgui_menu_deepest(ctx->open_menu);
		if (!menu) return true;
		vgui_menu_state* st = vgui_menu_of(menu);
		if (!st) return true;
		vgui_menu_item* cur = nullptr;
		auto p = st->index.find(st->cursor);
		if (p != st->index.end() && p->second < st->items.size()) cur = &st->items[p->second];
		if (!cur)
		{
			st->cursor = vgui_menu_first_selectable(menu, key == VGUI_KEY_UP || key == VGUI_KEY_END);
			p = st->index.find(st->cursor);
			if (p != st->index.end() && p->second < st->items.size()) cur = &st->items[p->second];
		}

		switch (key)
		{
		case VGUI_KEY_UP:
			vgui_menu_move_cursor(menu, -1);
			return true;
		case VGUI_KEY_DOWN:
			vgui_menu_move_cursor(menu, 1);
			return true;
		case VGUI_KEY_HOME:
		{
			for (const vgui_menu_item& it : st->items)
				if (it.enabled && it.type != VGUI_MENU_ITEM_SEPARATOR)
				{
					st->cursor = it.id;
					st->hover = it.id;
					break;
				}
			return true;
		}
		case VGUI_KEY_END:
		{
			for (int i = (int)st->items.size() - 1; i >= 0; --i)
				if (st->items[(size_t)i].enabled && st->items[(size_t)i].type != VGUI_MENU_ITEM_SEPARATOR)
				{
					st->cursor = st->items[(size_t)i].id;
					st->hover = st->cursor;
					break;
				}
			return true;
		}
		case VGUI_KEY_RIGHT:
			if (cur && cur->type == VGUI_MENU_ITEM_SUBMENU)
				vgui_menu_open_submenu(menu, cur->id);
			return true;
		case VGUI_KEY_LEFT:
			if (st->submenu_open)
			{
				vgui_menu_close_submenu(menu);
			}
			else if (st->parent_menu)
			{
				vgui_widget* parent = st->parent_menu;
				vgui_item_id parent_item = st->parent_item;
				vgui_menu_close_internal(menu);
				if (ctx->alive.count(parent) && parent->menu)
				{
					parent->menu->cursor = parent_item;
					parent->menu->hover = parent_item;
				}
			}
			return true;
		case VGUI_KEY_ESCAPE:
			if (st->submenu_open)
				vgui_menu_close_submenu(menu);
			else if (st->parent_menu)
				vgui_menu_close_internal(menu);
			else
				vgui_menu_close_all(ctx);
			return true;
		case VGUI_KEY_ENTER:
		case ' ':
			if (cur) vgui_menu_activate_item(menu, cur->id);
			return true;
		case VGUI_KEY_TAB:
			vgui_menu_close_all(ctx);
			return true;
		default:
			return true;
		}
	}

	static void vgui_splitter_drag_to(vgui_widget* w, float x, float y)
	{
		vgui_splitter_state* st = vgui_splitter_of(w);
		if (!st || !st->dragging) return;
		bool vertical = w->vertical != 0;
		float main = vertical ? (w->abs_h - w->padding[1] - w->padding[3])
			: (w->abs_w - w->padding[0] - w->padding[2]);
		main = std::max(0.0f, main);
		float sash = vgui_clampf(st->sash_size, 0.0f, main);
		float avail = std::max(0.0f, main - sash);
		vgui_widget* first_child = nullptr;
		vgui_widget* second_child = nullptr;
		for (vgui_widget* c : w->children)
		{
			if (!c->visible) continue;
			if (!first_child) first_child = c;
			else { second_child = c; break; }
		}
		if (!first_child || !second_child) return;
		float min0 = vertical ? first_child->min_h : first_child->min_w;
		float min1 = vertical ? second_child->min_h : second_child->min_w;
		min0 = vgui_clampf(min0, 0.0f, avail);
		min1 = vgui_clampf(min1, 0.0f, avail);
		if (min0 + min1 > avail && min0 + min1 > 0.0f)
		{
			float scale = avail / (min0 + min1);
			min0 *= scale;
			min1 *= scale;
		}
		float pos = vertical ? (y - w->abs_y - w->padding[1])
			: (x - w->abs_x - w->padding[0]);
		float first = pos - sash * 0.5f;
		float lo = min0;
		float hi = std::max(lo, avail - min1);
		first = vgui_clampf(first, lo, hi);
		float ratio = avail > 0.0f ? first / avail : 0.5f;
		if (std::fabs(ratio - w->value) > 0.0001f)
		{
			w->value = ratio;
			st->changed = 1;
			if (w->ctx)
			{
				w->ctx->layout_dirty = true;
				vgui_ensure_layout(w->ctx);
			}
		}
	}

	static bool vgui_tab_key(vgui_widget* w, int key, int down, int mods)
	{
		vgui_tab_state* st = vgui_tabs_of(w);
		if (!st) return false;
		if (!down) return false;
		int current = vgui_tab_find_index(w, st->active);
		int count = (int)st->tabs.size();

		if ((mods & VGUI_MOD_CTRL) && (key == 'W' || key == VGUI_KEY_DELETE))
		{
			if (current >= 0 && st->tabs[(size_t)current].closeable)
				vgui_tab_request_close(w, st->active);
			return true;
		}
		if ((mods & VGUI_MOD_CTRL) && key == VGUI_KEY_TAB)
		{
			if (count > 0)
			{
				int step = (mods & VGUI_MOD_SHIFT) ? -1 : 1;
				int start = current < 0 ? 0 : current;
				for (int i = 1; i <= count; ++i)
				{
					int next = (start + step * i) % count;
					if (next < 0) next += count;
					if (st->tabs[(size_t)next].enabled)
					{
						vgui_tab_set_active_internal(w, st->tabs[(size_t)next].id, true);
						return true;
					}
				}
			}
			return true;
		}
		if (key == VGUI_KEY_HOME || key == VGUI_KEY_END ||
			key == VGUI_KEY_LEFT || key == VGUI_KEY_RIGHT ||
			key == VGUI_KEY_UP || key == VGUI_KEY_DOWN)
		{
			if (count <= 0) return true;
			int target = -1;
			if (key == VGUI_KEY_HOME)
			{
				for (int i = 0; i < count; ++i)
					if (st->tabs[(size_t)i].enabled) { target = i; break; }
			}
			else if (key == VGUI_KEY_END)
			{
				for (int i = count - 1; i >= 0; --i)
					if (st->tabs[(size_t)i].enabled) { target = i; break; }
			}
			else
			{
				int step = (key == VGUI_KEY_LEFT || key == VGUI_KEY_UP) ? -1 : 1;
				int start = current < 0 ? 0 : current;
				for (int i = 1; i <= count; ++i)
				{
					int next = (start + step * i) % count;
					if (next < 0) next += count;
					if (st->tabs[(size_t)next].enabled) { target = next; break; }
				}
			}
			if (target >= 0)
				vgui_tab_set_active_internal(w, st->tabs[(size_t)target].id, true);
			return true;
		}
		return false;
	}

	//----------------------------------------------------------------------------
	// F03 generic drag & drop
	//----------------------------------------------------------------------------

	// 开始拖拽：拷贝载荷字符串（files 逐个深拷贝），挂到 context 上。
	static void vgui_drag_setup_payload(vgui_context* ctx, const vgui_drag_payload* payload)
	{
		ctx->drag_text = payload->text ? payload->text : "";
		ctx->drag_files.clear();
		ctx->drag_file_ptrs.clear();
		if (payload->kind == VGUI_DRAG_FILES && payload->files && payload->file_count > 0)
		{
			for (int i = 0; i < payload->file_count; ++i)
			{
				if (payload->files[i])
					ctx->drag_files.push_back(payload->files[i]);
			}
			ctx->drag_file_ptrs.reserve(ctx->drag_files.size());
			for (const std::string& f : ctx->drag_files)
				ctx->drag_file_ptrs.push_back(f.c_str());
		}
		ctx->drag_payload = *payload;
		ctx->drag_payload.text = ctx->drag_text.c_str();
		ctx->drag_payload.files = ctx->drag_file_ptrs.empty() ? nullptr : ctx->drag_file_ptrs.data();
		ctx->drag_payload.file_count = (int)ctx->drag_file_ptrs.size();
	}

	// 结束拖拽并清理全部状态。
	static void vgui_drag_end(vgui_context* ctx)
	{
		ctx->drag_kind = VGUI_DRAG_NONE;
		ctx->drag_payload = {};
		ctx->drag_text.clear();
		ctx->drag_files.clear();
		ctx->drag_file_ptrs.clear();
		ctx->drag_source = nullptr;
		ctx->drop_hover = nullptr;
		ctx->drop_accepted = 0;
		ctx->drop_hint = {};
	}

	// 命中点向上找第一个注册了 drop target 的控件（target 可以是祖先容器）。
	static vgui_widget* vgui_drop_target_at(vgui_context* ctx, float x, float y)
	{
		vgui_widget* hit = vgui_hit_rec(vgui_active_root(ctx), x, y);
		for (vgui_widget* w = hit; w; w = w->parent)
		{
			if (!ctx->alive.count(w)) return nullptr;
			if (w->drop_target)
				return w;
		}
		return nullptr;
	}

	// 拖拽中的 mouse move 路由：更新悬停 target、接受状态与提示矩形。
	static void vgui_drag_route_move(vgui_context* ctx, float x, float y)
	{
		vgui_widget* target = vgui_drop_target_at(ctx, x, y);
		if (target != ctx->drop_hover)
		{
			ctx->drop_hover = target;
			ctx->drop_accepted = 0;
			ctx->drop_hint = {};
			if (target && target->drop_target && target->drop_target->can_accept)
				ctx->drop_accepted = target->drop_target->can_accept(
					target->drop_target->user, &ctx->drag_payload) ? 1 : 0;
		}
		if (ctx->drop_accepted && target && target->drop_target && target->drop_target->hover)
		{
			vgui_drop_hint hint = {};
			target->drop_target->hover(target->drop_target->user, &ctx->drag_payload,
				x, y, &hint);
			ctx->drop_hint = hint;
		}
		else
		{
			ctx->drop_hint = {};
		}
		ctx->cursor = ctx->drop_accepted ? VGUI_CURSOR_HAND : VGUI_CURSOR_NO;
		vgui_tooltip_suppress(ctx);
	}

	// 释放：把载荷交给当前悬停且接受的 target，然后结束拖拽。
	static void vgui_drag_route_drop(vgui_context* ctx, float x, float y)
	{
		vgui_widget* target = ctx->drop_hover;
		if (target && !ctx->alive.count(target)) target = nullptr;
		bool handled = false;
		if (target && ctx->drop_accepted && target->drop_target && target->drop_target->drop)
			handled = target->drop_target->drop(target->drop_target->user,
				&ctx->drag_payload, x, y) != 0;
		(void)handled;
		vgui_drag_end(ctx);
	}

	// 行拖拽发起：按下记录的行在移动超过阈值后开始通用拖拽。
	static void vgui_row_drag_try_begin(vgui_widget* w, float x, float y)
	{
		vgui_context* ctx = w->ctx;
		if (!ctx || ctx->drag_kind != VGUI_DRAG_NONE) return;
		vgui_drag_payload payload = {};
		vgui_item_id item = 0;
		int index = -1;
		if (w->kind == VGUI_LIST && w->list && w->list->drag_pending)
		{
			item = w->list->drag_press_item;
			index = w->list->drag_press_index;
			if (!w->list->row_drag_fn ||
				!w->list->row_drag_fn(w->list->row_drag_user, item, index, &payload))
				return;
		}
		else if (w->kind == VGUI_TREE && w->tree && w->tree->drag_pending)
		{
			item = w->tree->drag_press_item;
			index = w->tree->drag_press_index;
			if (!w->tree->row_drag_fn ||
				!w->tree->row_drag_fn(w->tree->row_drag_user, item, index, &payload))
				return;
		}
		else
		{
			return;
		}
		if (vgui_begin_drag(ctx, w, &payload))
		{
			// 拖拽发起后取消按流行程，释放时不再触发行点击。
			w->pressed = 0;
			if (w->list) w->list->pressed = 0;
			if (w->tree)
			{
				w->tree->pressed = 0;
				w->tree->defer_click = 0;	// 拖走的按下不再补选
			}
			ctx->active = nullptr;
			ctx->press_target = nullptr;
			vgui_drag_route_move(ctx, x, y);
		}
	}

	static int vgui_mouse_move_impl(vgui_context* ctx, float x, float y)
	{
		if (!ctx) return 0;
		vgui_begin_input(ctx);
		ctx->mouse_x = x;
		ctx->mouse_y = y;

		// F03: 拖拽进行中时接管移动事件，悬停/tooltip/菜单语义全部抑制。
		if (ctx->drag_kind != VGUI_DRAG_NONE)
		{
			vgui_drag_route_move(ctx, x, y);
			ctx->want_mouse = 1;
			vgui_end_input(ctx);
			return 1;
		}

		if (ctx->open_menu)
		{
			if (ctx->hot && ctx->alive.count(ctx->hot))
			{
				ctx->hot->hovered = 0;
				vgui_emit(ctx->hot, VGUI_EVENT_LEAVE, x - ctx->hot->abs_x, y - ctx->hot->abs_y);
				ctx->hot = nullptr;
			}
			if (ctx->active && ctx->alive.count(ctx->active) && ctx->active->scroll_drag_axis)
			{
				vgui_widget* drag = ctx->active;
				vgui_scrollbar_drag_to(drag, x, y);
				if (ctx->alive.count(drag) && drag->ctx)
					vgui_menu_update_layout(drag, drag->ctx);
			}
			// A menubar keeps its menu chain open while the pointer travels over
			// its titles: hover another title to switch menus.
			vgui_widget* owner_bar = vgui_menu_owner_menubar(ctx->open_menu);
			if (owner_bar)
			{
				vgui_menubar_state* bs = vgui_menubar_of(owner_bar);
				vgui_item_id over = vgui_menubar_item_at_point(owner_bar, x, y);
				if (bs)
				{
					bs->hover = over;
					if (over && over != bs->active)
					{
						vgui_menubar_item* bi = vgui_menubar_find(owner_bar, over);
						if (bi && bi->enabled && bi->menu)
							vgui_menubar_show(owner_bar, over);
					}
				}
			}
			vgui_menu_mouse_move(ctx, x, y);
			vgui_widget* mh = vgui_menu_hit_rec(ctx->open_menu, x, y);
			vgui_widget* sw = nullptr;
			int sa = 0;
			bool over_bar_scroll = mh && vgui_scrollbar_hit(mh, x, y, &sw, &sa);
			int idx = -1;
			vgui_item_id item = mh ? vgui_menu_item_at(mh, x, y, &idx) : 0;
			ctx->cursor = (!over_bar_scroll && item && mh->menu && idx >= 0 &&
				mh->menu->items[(size_t)idx].enabled &&
				mh->menu->items[(size_t)idx].type != VGUI_MENU_ITEM_SEPARATOR)
				? VGUI_CURSOR_HAND : VGUI_CURSOR_ARROW;
			if (owner_bar && vgui_menubar_item_at_point(owner_bar, x, y))
				ctx->cursor = VGUI_CURSOR_HAND;
			ctx->want_mouse = 1;
			vgui_end_input(ctx);
			return 1;
		}

		vgui_widget* h = vgui_hit_rec(vgui_active_root(ctx), x, y);
		vgui_widget* scrollbar_widget = nullptr;
		int scrollbar_axis = 0;
		bool over_scrollbar = vgui_scrollbar_hit(h, x, y, &scrollbar_widget, &scrollbar_axis);
		if (ctx->hot != h)
		{
			vgui_widget* old = ctx->hot;
			if (old && ctx->alive.count(old))
			{
				old->hovered = 0;
				if (old->kind == VGUI_TABVIEW && old->tabs)
				{
					old->tabs->hover = 0;
					old->tabs->close_hover = 0;
				}
				if (old->kind == VGUI_DOCK && old->dock)
				{
					old->dock->hover = 0;
					old->dock->close_hover = 0;
				}
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
		{
			if (h->kind == VGUI_LIST)
			{
				int idx = vgui_list_index_at(h, x, y);
				vgui_item_id item = 0;
				vgui_list_item* it = vgui_list_item_at(h, idx, &item);
				if (h->list)
				{
					h->list->hover = it ? item : 0;
					h->list->hover_button = (it && idx >= 0)
						? vgui_list_button_hit(h, idx, x, y) : -1;
				}
				vgui_item_event(h, VGUI_EVENT_MOUSE_MOVE, item, idx, x - h->abs_x, y - h->abs_y);
			}
			else if (h->kind == VGUI_TREE)
			{
				int row = vgui_tree_row_at(h, x, y);
				vgui_item_id item = 0;
				if (h->tree && row >= 0)
					item = vgui_tree_row_id_at(h, h->tree.get(), row);
				if (h->tree)
				{
					h->tree->hover = item;
					h->tree->hover_button = (item && row >= 0)
						? vgui_tree_button_hit(h, row, x, y) : -1;
				}
				vgui_item_event(h, VGUI_EVENT_MOUSE_MOVE, item, row, x - h->abs_x, y - h->abs_y);
			}
			else if (h->kind == VGUI_TABVIEW)
			{
				bool over_close = false;
				int index = vgui_tab_index_at(h, x, y, &over_close);
				vgui_tab_state* ts = vgui_tabs_of(h);
				vgui_item_id item = (ts && index >= 0) ? ts->tabs[(size_t)index].id : 0;
				if (ts)
				{
					ts->hover = item;
					ts->close_hover = over_close ? item : 0;
				}
				vgui_item_event(h, VGUI_EVENT_MOUSE_MOVE, item, index, x - h->abs_x, y - h->abs_y);
			}
			else if (h->kind == VGUI_MENUBAR)
			{
				vgui_menubar_state* bs = vgui_menubar_of(h);
				if (bs) bs->hover = vgui_menubar_item_at_point(h, x, y);
			}
			else if (h->kind == VGUI_DOCK)
			{
				vgui_dock_update_hover(h, x, y);
			}
			else
			{
				vgui_emit(h, VGUI_EVENT_MOUSE_MOVE, x - h->abs_x, y - h->abs_y);
			}
		}

		if (ctx->active && ctx->alive.count(ctx->active))
		{
			vgui_widget* a = ctx->active;
			bool splitter_dragging = (a->kind == VGUI_SPLITTER && a->splitter && a->splitter->dragging);
			if (splitter_dragging)
			{
				vgui_splitter_drag_to(a, x, y);
				if (!ctx->alive.count(a)) { vgui_end_input(ctx); return 1; }
			}
			else if (a->scroll_drag_axis || (a->kind == VGUI_EDIT && a->edit_scroll_drag))
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
			else if (a->kind == VGUI_TABVIEW && a->tabs && a->tabs->drag_item)
			{
				vgui_tab_state* ts = a->tabs.get();
				if (std::fabs(x - ts->drag_start_x) > ctx->style.dock.drag_threshold)
					ts->drag_active = 1;
				if (ts->drag_active)
				{
					int current = vgui_tab_find_index(a, ts->drag_item);
					int drop = vgui_tab_drop_index(a, x);
					if (drop > current) drop--;
					if (current >= 0 && drop >= 0 && drop != current)
						vgui_tab_move_internal(a, ts->drag_item, drop);
				}
			}
			else if (a->kind == VGUI_DOCK && a->dock && a->dock->drag_mode)
			{
				vgui_dock_state* ds = a->dock.get();
				if (ds->drag_mode == 1)
				{
					float threshold = ctx->style.dock.drag_threshold;
					if (std::fabs(x - ds->press_x) > threshold ||
						std::fabs(y - ds->press_y) > threshold)
						ds->drag_mode = 2;
				}
				if (ds->drag_mode == 2)
				{
					ds->drag_x = x;
					ds->drag_y = y;
					ds->drop_zone = -1;
					ds->drop_index = -1;
					ds->drop_floating = 0;
					float bx = 0, by = 0, bw = 0, bh = 0;
					vgui_dock_content_rect(a, &bx, &by, &bw, &bh);
					if (x < bx || x >= bx + bw || y < by || y >= by + bh)
					{
						ds->drop_floating = 1;
					}
					else
					{
						int zone = vgui_dock_zone_at_point(a, x, y);
						if (zone == VGUI_DOCK_CENTER)
						{
							int fallback = vgui_dock_fallback_zone(a, x, y);
							if (fallback >= 0 && fallback != VGUI_DOCK_CENTER &&
								!vgui_dock_zone_has_open(ds, fallback))
								zone = fallback;
						}
						if (zone >= 0 && zone < 4)
						{
							ds->drop_zone = zone;
							ds->drop_index = vgui_dock_drop_index_any(a, zone, x);
						}
						else if (zone == VGUI_DOCK_FLOATING)
						{
							ds->drop_floating = 1;
						}
						else
						{
							int fallback = vgui_dock_fallback_zone(a, x, y);
							if (fallback >= 0)
							{
								ds->drop_zone = fallback;
								ds->drop_index = vgui_dock_drop_index_any(a, fallback, x);
							}
						}
					}
					vgui_mark_dirty(a);
				}
				else if (ds->drag_mode == 3)
				{
					vgui_dock_panel* panel = vgui_dock_find(a, ds->drag_panel);
					if (panel)
					{
						vgui_dock_clamp_rect(a, panel, x - ds->drag_offset_x,
							y - ds->drag_offset_y, panel->fw, panel->fh, false);
						vgui_mark_dirty(a);
					}
				}
			}
			else if ((a->kind == VGUI_LIST || a->kind == VGUI_TREE) && a->pressed_button == 1)
			{
				// F03 行拖拽：移动超过阈值且行有 drag 源时发起通用拖拽。
				const bool pending =
					(a->kind == VGUI_LIST && a->list && a->list->drag_pending) ||
					(a->kind == VGUI_TREE && a->tree && a->tree->drag_pending);
				if (pending)
				{
					const float px = a->kind == VGUI_LIST
						? a->list->drag_press_x : a->tree->drag_press_x;
					const float py = a->kind == VGUI_LIST
						? a->list->drag_press_y : a->tree->drag_press_y;
					const float threshold = ctx->style.dock.drag_threshold;
					if (std::fabs(x - px) > threshold || std::fabs(y - py) > threshold)
					{
						vgui_row_drag_try_begin(a, x, y);
						if (!ctx->alive.count(a)) { vgui_end_input(ctx); return 1; }
					}
				}
			}
			if (a != h && ctx->alive.count(a))
				vgui_emit(a, VGUI_EVENT_MOUSE_MOVE, x - a->abs_x, y - a->abs_y);
			bool dragging_bar = a->scroll_drag_axis || (a->kind == VGUI_EDIT && a->edit_scroll_drag);
			if (dragging_bar || over_scrollbar)
				ctx->cursor = VGUI_CURSOR_ARROW;
			else if (splitter_dragging || a->kind == VGUI_SPLITTER)
				ctx->cursor = vgui_splitter_cursor(a);
			else if (a->kind == VGUI_TABVIEW && vgui_tab_bar_hit(a, x, y))
				ctx->cursor = VGUI_CURSOR_HAND;
			else if (a->kind == VGUI_DOCK)
				ctx->cursor = (a->dock && (a->dock->hover || a->dock->drag_mode))
					? VGUI_CURSOR_HAND : VGUI_CURSOR_ARROW;
			else
				ctx->cursor = a->enabled ? a->cursor_type : VGUI_CURSOR_ARROW;
		}
		else
		{
			if (over_scrollbar || !h || !h->enabled)
				ctx->cursor = VGUI_CURSOR_ARROW;
			else if (h->kind == VGUI_SPLITTER)
				ctx->cursor = vgui_splitter_cursor(h);
			else if (h->kind == VGUI_TABVIEW && vgui_tab_bar_hit(h, x, y))
				ctx->cursor = VGUI_CURSOR_HAND;
			else if (h->kind == VGUI_DOCK)
				ctx->cursor = (h->dock && (h->dock->hover || h->dock->close_hover))
					? VGUI_CURSOR_HAND : VGUI_CURSOR_ARROW;
			else if (h->kind == VGUI_MENUBAR)
				ctx->cursor = vgui_menubar_item_at_point(h, x, y)
					? VGUI_CURSOR_HAND : VGUI_CURSOR_ARROW;
			else
				ctx->cursor = h->cursor_type;
		}

		ctx->want_mouse = (h != nullptr) || (ctx->active != nullptr);
		int ret = (h != nullptr || ctx->active != nullptr) ? 1 : 0;
		vgui_tooltip_on_mouse_move(ctx, x, y);
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
		vgui_tooltip_suppress(ctx);

		if (ctx->modal.active && ctx->modal.stage)
		{
			vgui_widget* mh = vgui_hit_rec(ctx->modal.stage, x, y);
			if (!mh || mh == ctx->modal.stage)
			{
				ctx->want_mouse = 1;
				ctx->cursor = VGUI_CURSOR_ARROW;
				ctx->modal.swallow_up = 1;
				if (button == 1 && ctx->modal.options.close_on_backdrop)
				{
					vgui_modal_close_internal(ctx, ctx->modal.options.backdrop_result);
					ctx->modal.swallow_up = 1;
				}
				vgui_end_input(ctx);
				return 1;
			}
		}

		// popup:内容矩形之外的按下关闭并吞掉;内容内部(含面板自身空白)
		// 走正常路由(active_root 已指向 content)。
		if (ctx->popup.content)
		{
			vgui_widget* mh = vgui_hit_rec(ctx->popup.content, x, y);
			if (!mh)
			{
				ctx->want_mouse = 1;
				ctx->cursor = VGUI_CURSOR_ARROW;
				if (button == 1)
				{
					ctx->popup.swallow_up = 1;
					vgui_popup_close_internal(ctx, VGUI_POPUP_CLOSE_OUTSIDE);
				}
				vgui_end_input(ctx);
				return 1;
			}
		}

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

		if (ctx->open_menu)
		{
			vgui_widget* owner_bar = vgui_menu_owner_menubar(ctx->open_menu);
			if (owner_bar && button == 1)
			{
				vgui_menubar_state* bs = vgui_menubar_of(owner_bar);
				vgui_item_id over = vgui_menubar_item_at_point(owner_bar, x, y);
				if (bs && over)
				{
					if (bs->active == over)
						vgui_menu_close_all(ctx);
					else
						vgui_menubar_show(owner_bar, over);
					ctx->menu_swallow_up = 1;
					ctx->want_mouse = 1;
					ctx->cursor = VGUI_CURSOR_HAND;
					vgui_end_input(ctx);
					return 1;
				}
			}
			vgui_menu_mouse_down(ctx, button, x, y);
			ctx->want_mouse = 1;
			ctx->cursor = VGUI_CURSOR_ARROW;
			vgui_end_input(ctx);
			return 1;
		}

		vgui_widget* h = vgui_hit_rec(vgui_active_root(ctx), x, y);
		vgui_widget* sw = nullptr;
		int axis = 0;
		bool over_scrollbar = vgui_scrollbar_hit(h, x, y, &sw, &axis);
		ctx->want_mouse = h != nullptr;
		ctx->cursor = (over_scrollbar || !h || !h->enabled)
			? VGUI_CURSOR_ARROW : h->cursor_type;

		if (h && h->enabled && h->kind == VGUI_MENUBAR && button == 1)
		{
			vgui_menubar_state* bs = vgui_menubar_of(h);
			vgui_item_id item = vgui_menubar_item_at_point(h, x, y);
			if (bs)
			{
				bs->pressed = item;
				if (item && bs->active == item)
				{
					vgui_menu_close_all(ctx);
					ctx->menu_swallow_up = 1;
				}
				else if (item)
				{
					vgui_menubar_show(h, item);
				}
			}
			ctx->want_mouse = 1;
			ctx->cursor = item ? VGUI_CURSOR_HAND : VGUI_CURSOR_ARROW;
			vgui_end_input(ctx);
			return 1;
		}

		// Scrollbars are painted over the content, so they get the first chance at
		// the click: they neither move focus nor forward the press to the widget
		// underneath.
		if (button == 1 && over_scrollbar)
		{
			vgui_scrollbar_begin(ctx, sw, axis, x, y);
			vgui_end_input(ctx);
			return 1;
		}

		if (h && h->enabled && h->kind == VGUI_SPLITTER)
		{
			vgui_splitter_state* st = vgui_splitter_of(h);
			vgui_set_focus_internal(ctx, h);
			h->pressed = 1;
			h->pressed_button = button;
			ctx->active = h;
			ctx->press_target = h;
			ctx->press_button = button;
			if (st)
			{
				if (button == 1 && clicks >= 2)
				{
					h->value = st->default_ratio;
					st->dragging = 0;
					st->changed = 1;
					ctx->layout_dirty = true;
					vgui_ensure_layout(ctx);
				}
				else
				{
					st->dragging = (button == 1) ? 1 : 0;
					st->changed = 0;
				}
			}
			vgui_emit(h, VGUI_EVENT_MOUSE_DOWN, x - h->abs_x, y - h->abs_y, button, clicks);
			ctx->want_mouse = 1;
			ctx->cursor = vgui_splitter_cursor(h);
			vgui_end_input(ctx);
			return 1;
		}

		if (h && h->enabled && h->kind == VGUI_DOCK)
		{
			vgui_dock_state* st = vgui_dock_of(h);
			vgui_set_focus_internal(ctx, h);
			h->pressed = 1;
			h->pressed_button = button;
			ctx->active = h;
			ctx->press_target = h;
			ctx->press_button = button;
			if (st)
			{
				st->pressed = 0;
				st->close_pressed = 0;
				st->drag_mode = 0;
				st->drag_panel = 0;
				int zone = -1, index = -1;
				bool over_close = false;
				bool on_tab = vgui_dock_tab_at_point(h, x, y, &zone, &index, &over_close);
				bool over_title = false;
				bool float_close = false;
				vgui_dock_panel* fp = vgui_dock_floating_at_point(h, x, y, &over_title, &float_close);
				if (fp)
				{
					if (over_title && button == 1)
					{
						st->pressed = fp->id;
						st->press_panel = fp->id;
						st->press_zone = VGUI_DOCK_FLOATING;
						st->press_x = x;
						st->press_y = y;
						if (float_close)
						{
							st->close_pressed = fp->id;
						}
						else
						{
							st->drag_mode = 3;
							st->drag_panel = fp->id;
							st->drag_offset_x = x - fp->fx;
							st->drag_offset_y = y - fp->fy;
							vgui_dock_activate_internal(h, fp->id, true);
							if (!ctx->alive.count(h)) { vgui_end_input(ctx); return 1; }
						}
					}
				}
				else if (on_tab && zone >= 0 && index >= 0 && index < (int)st->tabs[zone].size())
				{
					vgui_item_id item = st->tabs[zone][(size_t)index].id;
					if (button == 1 && over_close)
					{
						st->close_pressed = item;
					}
					else if (button == 1)
					{
						st->pressed = item;
						st->press_panel = item;
						st->press_zone = zone;
						st->press_x = x;
						st->press_y = y;
						st->drag_panel = item;
						st->drag_x = x;
						st->drag_y = y;
						st->drag_mode = 1;
						vgui_dock_activate_internal(h, item, true);
						if (!ctx->alive.count(h)) { vgui_end_input(ctx); return 1; }
					}
				}
				vgui_dock_update_hover(h, x, y);
			}
			vgui_emit(h, VGUI_EVENT_MOUSE_DOWN, x - h->abs_x, y - h->abs_y, button, clicks);
			if (!ctx->alive.count(h)) { vgui_end_input(ctx); return 1; }
			ctx->want_mouse = 1;
			ctx->cursor = (st && (st->hover || st->close_hover || st->drag_mode))
				? VGUI_CURSOR_HAND : VGUI_CURSOR_ARROW;
			vgui_end_input(ctx);
			return 1;
		}

		if (h && h->enabled && h->kind == VGUI_TABVIEW)
		{
			vgui_tab_state* st = vgui_tabs_of(h);
			vgui_set_focus_internal(ctx, h);
			h->pressed = 1;
			h->pressed_button = button;
			ctx->active = h;
			ctx->press_target = h;
			ctx->press_button = button;
			bool over_close = false;
			int index = vgui_tab_index_at(h, x, y, &over_close);
			vgui_item_id item = (st && index >= 0) ? st->tabs[(size_t)index].id : 0;
			if (st && item && item != 0)
			{
				vgui_tab_item* tab = &st->tabs[(size_t)index];
				if (button == 1 && tab->enabled)
				{
					if (over_close)
					{
						st->close_pressed = item;
					}
					else
					{
						st->pressed = item;
						st->drag_item = item;
						st->drag_start_x = x;
						st->drag_active = 0;
						vgui_tab_set_active_internal(h, item, true);
						if (!ctx->alive.count(h)) { vgui_end_input(ctx); return 1; }
					}
				}
				vgui_item_event(h, VGUI_EVENT_MOUSE_DOWN, item, index,
					x - h->abs_x, y - h->abs_y, button, clicks);
			}
			ctx->want_mouse = 1;
			ctx->cursor = vgui_tab_bar_hit(h, x, y) ? VGUI_CURSOR_HAND : VGUI_CURSOR_ARROW;
			vgui_end_input(ctx);
			return 1;
		}

		if (h && h->enabled && (h->kind == VGUI_LIST || h->kind == VGUI_TREE))
		{
			vgui_set_focus_internal(ctx, h);
			h->pressed = 1;
			h->pressed_button = button;
			ctx->active = h;
			ctx->press_target = h;
			ctx->press_button = button;

			int index = -1;
			vgui_item_id item = 0;
			bool arrow = false;
			if (h->kind == VGUI_LIST)
			{
				index = vgui_list_index_at(h, x, y);
				vgui_list_item_at(h, index, &item);
				const int button_hit = (button == 1 && item)
					? vgui_list_button_hit(h, index, x, y) : -1;
				if (h->list) h->list->pressed = (button == 1 && button_hit < 0) ? item : 0;
				if (button == 1 && item && button_hit < 0)
					vgui_list_apply_click(h, item, ctx->mods);
				if (button == 1 && item && button_hit >= 0)
				{
					if (h->list && h->list->button_fn)
					{
						h->list->button_fn(h, item, button_hit, h->list->button_user);
						if (!ctx->alive.count(h)) { vgui_end_input(ctx); return 1; }
					}
					if (h->list && h->list->virtual_mode && h->list->source.row_button_click)
					{
						h->list->source.row_button_click(h->list->source.user, index, button_hit);
						if (!ctx->alive.count(h)) { vgui_end_input(ctx); return 1; }
					}
					if (h->list) h->list->hover_button = button_hit;
				}
					if (h->list) h->list->hover = item;
					// F03 行拖拽：记录按下位置，移动超过阈值时发起。
					if (h->list)
					{
						h->list->drag_pending = (button == 1 && item && h->list->row_drag_fn) ? 1 : 0;
						h->list->drag_press_item = item;
						h->list->drag_press_index = index;
						h->list->drag_press_x = x;
						h->list->drag_press_y = y;
					}
				}
			else
			{
				index = vgui_tree_row_at(h, x, y);
				if (h->tree && index >= 0)
					item = vgui_tree_row_id_at(h, h->tree.get(), index);
				arrow = item && vgui_tree_arrow_hit(h, index, x);
				const int button_hit = (button == 1 && item && !arrow)
					? vgui_tree_button_hit(h, index, x, y) : -1;
				if (arrow)
				{
					vgui_tree_toggle_row(h, index);
					if (h->ctx) vgui_ensure_layout(h->ctx);
					h->tree->pressed = 0;
				}
				else if (button_hit >= 0)
				{
					h->tree->pressed = 0;
					if (h->tree->button_fn)
					{
						h->tree->button_fn(h, item, button_hit, h->tree->button_user);
						if (!ctx->alive.count(h)) { vgui_end_input(ctx); return 1; }
					}
					if (h->tree->virtual_mode && h->tree->source.row_button_click)
					{
						h->tree->source.row_button_click(h->tree->source.user, index, button_hit);
						if (!ctx->alive.count(h)) { vgui_end_input(ctx); return 1; }
					}
					h->tree->hover_button = button_hit;
				}
				else
				{
					if (h->tree) h->tree->pressed = (button == 1) ? item : 0;
					if (button == 1 && item)
					{
						/* 按在已选行且挂了拖源:推迟到抬起再改选择,
						 * 否则按下即把多选拖拽折叠成单选。 */
						const bool defer = h->tree && h->tree->row_drag_fn &&
							vgui_id_selected(h->tree->selected, item);
						if (defer)
						{
							h->tree->defer_click = 1;
							h->tree->press_mods = ctx->mods;
						}
						else
						{
							h->tree->defer_click = 0;
							vgui_tree_apply_click(h, item, ctx->mods);
						}
					}
					else if (h->tree)
						h->tree->defer_click = 0;
				}
					if (h->tree) h->tree->hover = item;
					// F03 行拖拽：箭头/行内按钮上的按下不发起拖拽。
					if (h->tree)
					{
						h->tree->drag_pending = (button == 1 && item && !arrow && button_hit < 0 &&
							h->tree->row_drag_fn) ? 1 : 0;
						h->tree->drag_press_item = item;
						h->tree->drag_press_index = index;
						h->tree->drag_press_x = x;
						h->tree->drag_press_y = y;
					}
				}
			vgui_item_event(h, VGUI_EVENT_MOUSE_DOWN, item, index,
				x - h->abs_x, y - h->abs_y, button, clicks);
			(void)arrow;
			ctx->want_mouse = 1;
			vgui_end_input(ctx);
			return 1;
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
		vgui_tooltip_suppress(ctx);

		// F03: 拖拽中的释放不再走任何控件点击路径，直接交付 drop。
		if (ctx->drag_kind != VGUI_DRAG_NONE)
		{
			ctx->press_target = nullptr;
			ctx->active = nullptr;
			if (button == 1)
				vgui_drag_route_drop(ctx, x, y);
			ctx->want_mouse = 1;
			vgui_end_input(ctx);
			return 1;
		}

		if (ctx->modal.swallow_up)
		{
			ctx->modal.swallow_up = 0;
			ctx->want_mouse = 1;
			vgui_end_input(ctx);
			return 1;
		}
		if (ctx->popup.swallow_up)
		{
			ctx->popup.swallow_up = 0;
			ctx->want_mouse = 1;
			vgui_end_input(ctx);
			return 1;
		}
		if (ctx->modal.active && !ctx->press_target)
		{
			ctx->want_mouse = 1;
			vgui_end_input(ctx);
			return 1;
		}

		if (ctx->menu_swallow_up)
		{
			ctx->menu_swallow_up = 0;
			ctx->want_mouse = 1;
			vgui_end_input(ctx);
			return 1;
		}
		if (ctx->open_menu)
		{
			if (ctx->active && ctx->alive.count(ctx->active) && ctx->active->scroll_drag_axis)
			{
				ctx->active->scroll_drag_axis = 0;
				ctx->active = nullptr;
			}
			vgui_menu_mouse_up(ctx, button, x, y);
			ctx->want_mouse = 1;
			vgui_end_input(ctx);
			return 1;
		}

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

		if (target && target->kind == VGUI_DOCK)
		{
			vgui_dock_state* st = vgui_dock_of(target);
			int drag_mode = st ? st->drag_mode : 0;
			vgui_item_id drag_panel = st ? st->drag_panel : 0;
			int drop_zone = st ? st->drop_zone : -1;
			int drop_index = st ? st->drop_index : -1;
			int drop_floating = st ? st->drop_floating : 0;
			vgui_item_id close_pressed = st ? st->close_pressed : 0;
			float drag_x = st ? st->drag_x : x;
			float drag_y = st ? st->drag_y : y;
			float lx = x - target->abs_x;
			float ly = y - target->abs_y;
			target->pressed = 0;
			ctx->press_target = nullptr;
			ctx->slider_drag = nullptr;
			if (st)
			{
				st->pressed = 0;
				st->close_pressed = 0;
				st->drag_mode = 0;
				st->drop_zone = -1;
				st->drop_index = -1;
				st->drop_floating = 0;
			}
			vgui_emit(target, VGUI_EVENT_MOUSE_UP, lx, ly, button, clicks);
			if (!ctx->alive.count(target)) { vgui_end_input(ctx); return 1; }

			if (close_pressed)
			{
				bool still_close = false;
				int zone = -1, index = -1;
				bool over_close = false;
				vgui_dock_state* st2 = vgui_dock_of(target);
				if (st2 && vgui_dock_tab_at_point(target, x, y, &zone, &index, &over_close) &&
					over_close && zone >= 0 && index >= 0 &&
					index < (int)st2->tabs[zone].size() &&
					st2->tabs[zone][(size_t)index].id == close_pressed)
					still_close = true;
				if (!still_close)
				{
					bool over_title = false, float_close = false;
					vgui_dock_panel* fp = vgui_dock_floating_at_point(target, x, y,
						&over_title, &float_close);
					if (fp && float_close && fp->id == close_pressed) still_close = true;
				}
				if (still_close)
				{
					vgui_dock_close_internal(target, close_pressed, true);
					if (!ctx->alive.count(target)) { vgui_end_input(ctx); return 1; }
				}
			}
			else if (drag_mode == 2 && drag_panel)
			{
				if (drop_floating)
				{
					vgui_dock_float_internal(target, drag_panel, drag_x, drag_y, 0.0f, 0.0f,
						true, true);
				}
				else if (drop_zone >= 0 && drop_zone < 4)
				{
					int index = drop_index;
					int current = vgui_dock_open_index(target, drop_zone, drag_panel);
					if (current >= 0 && index > current) index--;
					vgui_dock_move_internal(target, drag_panel, drop_zone, index, true, true);
				}
				if (!ctx->alive.count(target)) { vgui_end_input(ctx); return 1; }
			}
			else if (drag_mode == 3 && drag_panel)
			{
				int zone = vgui_dock_zone_tab_bar_at(target, x, y);
				if (zone >= 0 && zone < 4)
				{
					int index = vgui_dock_drop_index_at(target, zone, x);
					int current = vgui_dock_open_index(target, zone, drag_panel);
					if (current >= 0 && index > current) index--;
					vgui_dock_move_internal(target, drag_panel, zone, index, true, true);
					if (!ctx->alive.count(target)) { vgui_end_input(ctx); return 1; }
				}
			}
			if (st) st->drag_panel = 0;
			vgui_mark_dirty(target);
			ctx->want_mouse = ctx->hot != nullptr;
			vgui_end_input(ctx);
			return 1;
		}

		if (target && target->kind == VGUI_TABVIEW)
		{
			vgui_tab_state* st = vgui_tabs_of(target);
			bool over_close = false;
			int index = vgui_tab_index_at(target, x, y, &over_close);
			vgui_item_id item = (st && index >= 0) ? st->tabs[(size_t)index].id : 0;
			vgui_item_id pressed_item = st ? st->pressed : 0;
			vgui_item_id close_item = st ? st->close_pressed : 0;
			int dragged = st ? st->drag_active : 0;
			float lx = x - target->abs_x;
			float ly = y - target->abs_y;
			target->pressed = 0;
			ctx->press_target = nullptr;
			ctx->slider_drag = nullptr;
			if (st)
			{
				st->pressed = 0;
				st->drag_item = 0;
				st->drag_active = 0;
				st->close_pressed = 0;
			}
			vgui_item_event(target, VGUI_EVENT_MOUSE_UP, item, index, lx, ly, button, clicks);
			if (!ctx->alive.count(target)) { vgui_end_input(ctx); return 1; }

			if (close_item && item == close_item && over_close)
			{
				vgui_tab_request_close(target, close_item);
				if (!ctx->alive.count(target)) { vgui_end_input(ctx); return 1; }
			}
			else if (!dragged && item && item == pressed_item && target->enabled)
			{
				vgui_event_type ct = VGUI_EVENT_CLICK;
				if (clicks >= 3) ct = VGUI_EVENT_TRIPLE_CLICK;
				else if (clicks == 2) ct = VGUI_EVENT_DOUBLE_CLICK;
				vgui_item_event(target, ct, item, index, lx, ly, button, clicks);
			}
			ctx->want_mouse = ctx->hot != nullptr;
			vgui_end_input(ctx);
			return 1;
		}

		if (target && (target->kind == VGUI_LIST || target->kind == VGUI_TREE))
		{
			float lx = x - target->abs_x;
			float ly = y - target->abs_y;
			int index = -1;
			vgui_item_id item = 0;
			vgui_item_id pressed_id = 0;
			if (target->kind == VGUI_LIST)
			{
				index = vgui_list_index_at(target, x, y);
				vgui_list_item_at(target, index, &item);
				if (target->list)
				{
					pressed_id = target->list->pressed;
					target->list->pressed = 0;
				}
			}
			else
			{
				index = vgui_tree_row_at(target, x, y);
				if (target->tree && index >= 0)
					item = vgui_tree_row_id_at(target, target->tree.get(), index);
				if (target->tree)
				{
					pressed_id = target->tree->pressed;
					target->tree->pressed = 0;
				}
			}
			target->pressed = 0;
			ctx->press_target = nullptr;
			ctx->slider_drag = nullptr;
			vgui_item_event(target, VGUI_EVENT_MOUSE_UP, item, index, lx, ly, button, clicks);
			if (ctx->alive.count(target) && pressed_id != 0 && item == pressed_id && target->enabled)
			{
				/* O03:按下时被推迟的选择在这里补上(未发起拖拽的普通点击)。 */
				if (target->kind == VGUI_TREE && target->tree->defer_click)
				{
					target->tree->defer_click = 0;
					vgui_tree_apply_click(target, item, target->tree->press_mods);
					if (!ctx->alive.count(target)) { vgui_end_input(ctx); return 1; }
				}
				vgui_event_type ct = VGUI_EVENT_CLICK;
				if (clicks >= 3) ct = VGUI_EVENT_TRIPLE_CLICK;
				else if (clicks == 2) ct = VGUI_EVENT_DOUBLE_CLICK;
				vgui_item_event(target, ct, item, index, lx, ly, button, clicks);
				if (ctx->alive.count(target) && clicks >= 2)
				{
					if (target->kind == VGUI_LIST) vgui_list_activate(target, item);
					else vgui_tree_activate(target, item);
				}
			}
			ctx->want_mouse = ctx->hot != nullptr;
			vgui_end_input(ctx);
			return 1;
		}

		int ret = target ? 1 : 0;
		if (target)
		{
			vgui_widget* h = vgui_hit_rec(vgui_active_root(ctx), x, y);
			bool inside = (h == target || (h && h->parent == target));
			float lx = x - target->abs_x;
			float ly = y - target->abs_y;
			bool splitter_changed = false;

			// Clear press state before invoking user code so callbacks can
			// safely inspect the widget.
			target->pressed = 0;
			if (target->kind == VGUI_EDIT) target->edit_drag = 0;
			if (target->kind == VGUI_SPLITTER && target->splitter)
			{
				splitter_changed = target->splitter->changed != 0;
				target->splitter->changed = 0;
				target->splitter->dragging = 0;
			}
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
				if (splitter_changed && ctx->alive.count(target))
					vgui_notify_value(target);
			}
			else if (splitter_changed && ctx->alive.count(target))
			{
				vgui_notify_value(target);
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
			if (p->kind == VGUI_PANEL || p->kind == VGUI_LIST || p->kind == VGUI_TREE)
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
	// panel.  Multi-line editors scroll vertically, and any editor scrolls
	// horizontally when its widest line is wider than the box.
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
				float cw = vgui_edit_content_width(e);
				if (cw > iw + 0.5f)
				{
					e->edit_scroll_x = vgui_clampf(e->edit_scroll_x - wy * 40.0f, 0.0f,
						cw - iw + ctx->style.edit.scrollbar_extra_scroll);
					scrolled = true;
				}
			}
		}
		if (wx != 0)
		{
			float cw = vgui_edit_content_width(e);
			float max_scroll = std::max(0.0f, cw - iw + ctx->style.edit.scrollbar_extra_scroll);
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
		vgui_tooltip_suppress(ctx);
		// popup:内容外的滚轮关闭弹出层;内容内走正常路由。
		if (ctx->popup.content &&
			!vgui_hit_rec(ctx->popup.content, ctx->mouse_x, ctx->mouse_y))
		{
			vgui_popup_close_internal(ctx, VGUI_POPUP_CLOSE_WHEEL);
			ctx->want_mouse = 1;
			vgui_end_input(ctx);
			return 1;
		}
		if (ctx->open_menu)
		{
			vgui_widget* m = vgui_menu_hit_rec(ctx->open_menu, ctx->mouse_x, ctx->mouse_y);
			if (!m) m = vgui_menu_deepest(ctx->open_menu);
			if (m && m->menu)
			{
				float ih = std::max(0.0f, m->abs_h);
				float max_sy = std::max(0.0f, m->content_h - ih);
				if (wy != 0 && max_sy > 0.0f)
				{
					m->scroll_y = vgui_clampf(m->scroll_y - wy * 40.0f, 0.0f, max_sy);
					ctx->layout_dirty = true;
					vgui_ensure_layout(ctx);
				}
			}
			ctx->want_mouse = 1;
			vgui_end_input(ctx);
			return 1;
		}
		vgui_widget* h = ctx->hot ? ctx->hot : vgui_hit_rec(vgui_active_root(ctx), ctx->mouse_x, ctx->mouse_y);
		int consumed = 0;
		vgui_widget* edit = vgui_find_edit(h);
		if (edit && (wx != 0 || wy != 0))
			consumed = vgui_edit_wheel(ctx, edit, wx, wy) ? 1 : 0;
		if ((wx != 0 || wy != 0) && !consumed)
		{
			for (vgui_widget* p = h; p; p = p->parent)
			{
				if (p->kind == VGUI_DOCK && p->dock)
				{
					int zone = vgui_dock_zone_tab_bar_at(p, ctx->mouse_x, ctx->mouse_y);
					if (zone < 0) continue;
					float max_scroll = std::max(0.0f,
						vgui_dock_tab_bar_width(p, zone) - p->dock->zone_w[zone]);
					float delta = wx != 0.0f ? wx : wy;
					p->dock->tab_scroll[zone] = vgui_clampf(
						p->dock->tab_scroll[zone] - delta * 40.0f, 0.0f, max_scroll);
					ctx->layout_dirty = true;
					vgui_ensure_layout(ctx);
					consumed = 1;
					break;
				}
				if (p->kind != VGUI_TABVIEW || !vgui_tab_bar_hit(p, ctx->mouse_x, ctx->mouse_y))
					continue;
				float iw = std::max(0.0f, p->abs_w - p->padding[0] - p->padding[2]);
				float max_scroll = std::max(0.0f, vgui_tab_bar_width(p) - iw);
				float delta = wx != 0.0f ? wx : wy;
				p->scroll_x = vgui_clampf(p->scroll_x - delta * 40.0f, 0.0f, max_scroll);
				ctx->layout_dirty = true;
				vgui_ensure_layout(ctx);
				vgui_notify_scroll(p);
				consumed = 1;
				break;
			}
		}
		if (wy != 0 && !consumed)
		{
			vgui_widget* p = vgui_find_scroll_panel(h, true);
			if (p)
			{
				p->scroll_y -= wy * 40.0f;
				ctx->layout_dirty = true;
				vgui_ensure_layout(ctx);
				vgui_notify_scroll(p);
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
				vgui_notify_scroll(p);
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

	static int vgui_dispatch_key_handlers(vgui_context* ctx, int key, int down, int mods)
	{
		if (!ctx || ctx->key_handlers.empty()) return 0;
		std::vector<vgui_context::key_handler_entry> handlers = ctx->key_handlers;
		for (auto it = handlers.rbegin(); it != handlers.rend(); ++it)
		{
			if (!it->fn) continue;
			bool present = false;
			for (const auto& entry : ctx->key_handlers)
				if (entry.token == it->token) { present = true; break; }
			if (!present) continue;
			if (it->fn(ctx, key, down, mods, it->user)) return 1;
		}
		return 0;
	}

	static int vgui_key_impl(vgui_context* ctx, int key, int down, int mods)
	{
		if (!ctx) return 0;
		vgui_begin_input(ctx);
		vgui_tooltip_suppress(ctx);
		ctx->mods = mods;
		ctx->want_key = ctx->focus != nullptr || ctx->open_menu != nullptr ||
			!ctx->key_handlers.empty() || ctx->modal.active;

		if (ctx->modal.active && down && key == VGUI_KEY_ESCAPE)
		{
			if (ctx->modal.options.close_on_escape)
				vgui_modal_close_internal(ctx, ctx->modal.options.escape_result);
			vgui_end_input(ctx);
			return 1;
		}
		// popup:焦点在内容内的 edit 上时 Esc 先走 edit 取消(blur),
		// 失焦后的下一个 Esc 才关闭 popup;其他焦点控件不拦截。
		if (ctx->popup.content && down && key == VGUI_KEY_ESCAPE)
		{
			vgui_widget* pf = ctx->focus;
			const bool edit_holds = pf && pf->kind == VGUI_EDIT &&
				vgui_is_within(ctx->popup.content, pf);
			if (!edit_holds)
			{
				vgui_popup_close_internal(ctx, VGUI_POPUP_CLOSE_ESCAPE);
				vgui_end_input(ctx);
				return 1;
			}
		}
		if (ctx->modal.active && down && key == VGUI_KEY_ENTER &&
			ctx->dialog.panel && ctx->dialog.panel == ctx->modal.panel)
		{
			vgui_modal_close_internal(ctx, VGUI_MODAL_RESULT_CONFIRM);
			vgui_end_input(ctx);
			return 1;
		}

		if (ctx->open_menu)
		{
			// Left/right at the top level of a menubar switches between titles.
			// A right press on a submenu entry still opens that submenu.
			vgui_menu_state* mst = ctx->open_menu->menu.get();
			if (down && mst && mst->owner_menubar && !mst->submenu_open &&
				(key == VGUI_KEY_LEFT || key == VGUI_KEY_RIGHT))
			{
				bool submenu_cursor = false;
				auto p = mst->index.find(mst->cursor);
				if (p != mst->index.end() && p->second < mst->items.size())
				{
					const vgui_menu_item& cur = mst->items[p->second];
					submenu_cursor = cur.enabled && cur.type == VGUI_MENU_ITEM_SUBMENU;
				}
				if (key == VGUI_KEY_LEFT || !submenu_cursor)
				{
					vgui_widget* bar = mst->owner_menubar;
					vgui_item_id next = vgui_menubar_step(bar, key == VGUI_KEY_RIGHT ? 1 : -1);
					if (next) vgui_menubar_show(bar, next);
					ctx->want_key = 1;
					ctx->want_text = 0;
					vgui_end_input(ctx);
					return 1;
				}
			}
			vgui_menu_key(ctx, key, down);
			ctx->want_key = 1;
			ctx->want_text = 0;
			vgui_end_input(ctx);
			return 1;
		}

		if (down && key == VGUI_KEY_TAB && (mods & VGUI_MOD_CTRL) == 0)
		{
			vgui_focus_next_impl(ctx, (mods & VGUI_MOD_SHIFT) != 0);
			vgui_end_input(ctx);
			return 1;
		}

		vgui_widget* f = ctx->focus;
		if (!f || !ctx->alive.count(f) || !f->enabled)
		{
			int handled = ctx->modal.active ? 1 : vgui_dispatch_key_handlers(ctx, key, down, mods);
			vgui_end_input(ctx);
			return handled;
		}

		int handled = 0;
		if (f->kind == VGUI_EDIT)
		{
			handled = vgui_edit_key(f, key, down, mods) ? 1 : 0;
		}
		else if (f->kind == VGUI_LIST)
		{
			handled = vgui_list_key(f, key, down, mods) ? 1 : 0;
			if (!ctx->alive.count(f)) { vgui_end_input(ctx); return handled; }
		}
		else if (f->kind == VGUI_TREE)
		{
			handled = vgui_tree_key(f, key, down, mods) ? 1 : 0;
			if (!ctx->alive.count(f)) { vgui_end_input(ctx); return handled; }
		}
		else if (f->kind == VGUI_TABVIEW)
		{
			handled = vgui_tab_key(f, key, down, mods) ? 1 : 0;
			if (!ctx->alive.count(f)) { vgui_end_input(ctx); return handled; }
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
				// A horizontal RTL slider grows towards the left.
				if (vgui_is_rtl(ctx) && !f->vertical &&
					(key == VGUI_KEY_LEFT || key == VGUI_KEY_RIGHT))
					delta = -delta;
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

		if (!handled)
		{
			if (ctx->modal.active)
				handled = 1;
			else
				handled = vgui_dispatch_key_handlers(ctx, key, down, mods);
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

	// Scrollbar feedback: only the thumb changes, and only slightly, so the
	// track stays quiet and the bar remains readable on both dark and light
	// themes.  The hover colour is part of the theme.
	static uint32_t vgui_scrollbar_thumb_color(vgui_widget* w, int axis)
	{
		vgui_context* ctx = w->ctx;
		uint32_t c = ctx->theme.scrollbar;
		if (vgui_scrollbar_hovered(ctx, w, axis) && ctx->theme.scrollbar_hover)
			c = ctx->theme.scrollbar_hover;
		return vgui_widget_color(w, c);
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

	static glm::ivec4 vgui_clip_intersect(const glm::ivec4& a, const glm::ivec4& b)
	{
		int x0 = std::max(a.x, b.x);
		int y0 = std::max(a.y, b.y);
		int x1 = std::min(a.x + a.z, b.x + b.z);
		int y1 = std::min(a.y + a.w, b.y + b.w);
		return { x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0) };
	}

	static void vgui_clip_push(vgui_clip_state* clips, ovg_ctx_cb* cb, rvg_t* vg,
		float x, float y, float w, float h)
	{
		glm::ivec4 rc = {
			(int)std::floor(x), (int)std::floor(y),
			(int)std::ceil(std::max(0.0f, w)), (int)std::ceil(std::max(0.0f, h))
		};
		// Nested clips must be intersected with the parent.  Some callers (list
		// rows, tree rows, edit text) push a clip that can be scrolled partially
		// outside the widget; replacing the parent would let those pixels escape
		// the widget viewport.
		if (!clips->stack.empty())
			rc = vgui_clip_intersect(clips->stack.back(), rc);
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
		// color==0 早退：工具层收到全透明色仍会录制绘制命令
		if (!cb || !vg || color == 0 || w <= 0 || h <= 0) return;
		ovg_tools::fill_round_rect(cb, vg, x, y, w, h, r, vgui_ovg_color(color));
	}

	static void vgui_stroke_rect(ovg_ctx_cb* cb, rvg_t* vg, float x, float y, float w, float h, float r, uint32_t color, float width)
	{
		if (!cb || !vg || color == 0 || w <= 0 || h <= 0 || width <= 0) return;
		ovg_tools::stroke_round_rect(cb, vg, x, y, w, h, r, vgui_ovg_color(color), width);
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

	/* Physical text drawing.  ax/ay are used exactly as supplied, so compound
	 * controls that already placed every sub-region themselves can avoid the
	 * automatic RTL mirroring done by vgui_draw_text(). */
	static void vgui_draw_text_raw(vgui_context* ctx, ovg_ctx_cb* cb, rvg_t* vg, vgui_widget* w,
		const std::string& text, float x, float y, float width, float height,
		uint32_t color, float ax, float ay)
	{
		if (!ctx || !ctx->font || !cb || !vg || text.empty() || color == 0) return;
		float lh = vgui_line_height(ctx, w);
		float tw = std::max(width, vgui_text_width(w, text));
		int count = vgui_text_lines(text);
		float th = std::max(height, lh);
		if (count > 1)
			th = lh * (float)count;
		text_style_t st = {};
		st.family = ctx->font;
		st.fontsize = vgui_font_size(ctx, w);
		st.color = vgui_ovg_color(color);
		st.min_subpixel = ctx->style.text_min_subpixel;
		text_box_rt box = {};
		box.text_align = { ax, ay };
		box.rc = { x,y,tw, th };
		box.baseline = vgui_text_ascent(ctx, w);
		text_st_t t = {};
		t.pos = { x, y };
		t.size = { tw, height };
		t.text = text.c_str();
		t.text_len = (int)text.size();
		cb->add_text(vg, &t, &st, &box);
	}

	/* Direction-aware text drawing: 0/1 alignment means start/end, so it swaps
	 * in RTL while centre alignment is unchanged. */
	static void vgui_draw_text(vgui_context* ctx, ovg_ctx_cb* cb, rvg_t* vg, vgui_widget* w,
		const std::string& text, float x, float y, float width, float height,
		uint32_t color, float ax = 0.0f, float ay = 0.5f)
	{
		if (vgui_is_rtl(ctx))
			ax = 1.0f - ax;
		vgui_draw_text_raw(ctx, cb, vg, w, text, x, y, width, height, color, ax, ay);
	}

	static uint32_t vgui_text_color(vgui_widget* w)
	{
		vgui_context* ctx = w->ctx;
		uint32_t c = w->text_color ? w->text_color : ctx->theme.text;
		if (!w->enabled) c = vgui_mix_color(c, ctx->theme.disabled, ctx->style.disabled_text_mix);
		return vgui_widget_color(w, c);
	}

	// Effective focus opacity: a widget override if set, otherwise the context
	// default.  Used by the focus ring and by focus-tinted widget colours.
	static float vgui_focus_alpha(const vgui_widget* w)
	{
		if (!w) return 0.0f;
		if (w->focus_alpha >= 0.0f) return w->focus_alpha;
		return w->ctx ? w->ctx->focus_alpha : 1.0f;
	}

	static void vgui_draw_focus(vgui_widget* w, ovg_ctx_cb* cb, rvg_t* vg)
	{
		if (!w->focused) return;
		vgui_context* ctx = w->ctx;
		const vgui_focus_style& fs = ctx->style.focus;
		if (w->abs_w < fs.min_size || w->abs_h < fs.min_size) return;
		float alpha = vgui_focus_alpha(w);
		if (alpha <= 0.001f) return;
		// 焦点环画在控件内部（与边框重合）：父容器的裁剪矩形再也不可能把
		// 焦点环的一部分裁掉，控件被部分滚动出可视区时仍能看到焦点。
		float r = w->rounding > 0 ? w->rounding : fs.rounding;
		uint32_t color = vgui_alpha_color(vgui_widget_color(w, ctx->theme.accent), alpha);
		float d[] = { fs.width * 2.0f,fs.width * 2.0f };
		cb->set_dash(vg, d, 2, 0);
		vgui_stroke_rect(cb, vg, w->abs_x + fs.inset, w->abs_y + fs.inset,
			w->abs_w - fs.inset * 2.0f, w->abs_h - fs.inset * 2.0f, r, color, fs.width);
		// dash 用完即清（与 ovg_tools 约定一致）：焦点环泄漏的 dash 会与后续
		// save/restore 的 dash 数组所有权交互，留下悬空引用后按垃圾值细分描边。
		cb->set_dash(vg, nullptr, 0, 0);
	}

	// Forward — declared later at vgui_draw_image. Used by VGUI_BUTTON icon draw.
	static void vgui_texture_ensure_upload(vgui_context* ctx, vgui_texture* texture, ovg_ctx_cb* cb, rvg_t* vg);

	// -------------------------------------------------------------------
	// Gradient button helpers (ported from vgui_legacy.cpp)
	// -------------------------------------------------------------------

	// 把 0xAARRGGBB 的 alpha 按比例乘一遍，返回新颜色（保留 RGB 分量的乘性缩放语义）。
	static uint32_t vgui_set_alpha_xf(uint32_t c, double af)
	{
		if (af < 0) af = 0;
		uint8_t a = (c >> 24) & 0xFF;
		double na = a * af + 0.5;
		if (na > 255) na = 255;
		a = (uint8_t)na;
		return (c & 0x00FFFFFFu) | ((uint32_t)a << 24);
	}

	// 直接写 alpha 通道（0..1）。
	static uint32_t vgui_set_alpha_f(uint32_t c, double af)
	{
		if (af > 1) af = 1;
		if (af < 0) af = 0;
		uint8_t a = (uint8_t)(af * 255.0 + 0.5);
		return (c & 0x00FFFFFFu) | ((uint32_t)a << 24);
	}

	static double vgui_get_alpha_f(vgui_color c)
	{
		return ((c >> 24) & 0xFF) / 255.0;
	}

	// 构造垂直线性渐变 pattern 并对给定矩形填充。
	static void vgui_paint_linear_rect(ovg_ctx_cb* cb, rvg_t* vg,
		float x, float y, float w, float h, float rounding,
		vgui_color top, vgui_color bot)
	{
		ovg_tools::fill_round_rect_gradient(cb, vg, x, y, w, h, rounding,
			vgui_ovg_color(top), vgui_ovg_color(bot));
	}

	static void vgui_draw_button_gradient(vgui_widget* w, ovg_ctx_cb* cb, rvg_t* vg,
		float thickness, float rounding)
	{
		vgui_context* ctx = w->ctx;

		// 选状态对应的 gradTop/gradBot
		int state_idx; // 0=default, 1=hover, 2=active
		if (w->pressed) state_idx = 2;
		else if (w->hovered || w->hover_anim > 0.01f) state_idx = 1;
		else state_idx = 0;

		uint32_t gradTop = w->grad_top[state_idx];
		uint32_t gradBot = w->grad_bot[state_idx];
		uint32_t borderLight = w->gradient_border_light;
		uint32_t borderDark = w->gradient_border_dark;
		double oa = w->gradient_opacity;

		// 禁用态：整体向 disabled 色混合，相当于降低 opacity
		if (!w->enabled)
		{
			float disabled_mix = ctx->style.button.disabled_mix;
			gradTop = vgui_mix_color(gradTop, ctx->theme.disabled, disabled_mix);
			gradBot = vgui_mix_color(gradBot, ctx->theme.disabled, disabled_mix);
			borderLight = vgui_mix_color(borderLight, ctx->theme.disabled, disabled_mix);
			borderDark = vgui_mix_color(borderDark, ctx->theme.disabled, disabled_mix);
		}

		// 按下态：整体内移 thickness + 渐变透明度略微降低
		bool pushed = w->pressed;
		if (pushed)
		{
			gradTop = vgui_set_alpha_f(gradTop, 0.8);
			gradBot = vgui_set_alpha_f(gradBot, 0.8);
		}

		// 应用全局 opacity
		gradTop = vgui_set_alpha_xf(gradTop, oa);
		gradBot = vgui_set_alpha_xf(gradBot, oa);
		borderLight = vgui_set_alpha_xf(borderLight, oa);
		borderDark = vgui_set_alpha_xf(borderDark, oa);

		float x = w->abs_x, y = w->abs_y, ww = w->abs_w, hh = w->abs_h;

		// 按下态视觉凹陷：背景整体下移 press_offset，与 text/icon 的按下偏移量对齐
		float translate_y = pushed ? ctx->style.button.press_offset : 0.0f;

		cb->save(vg);
		cb->translate(vg, 0.0f, translate_y);

		// 1) 可选的底色（legacy light 风格用）：沿用 bg 字段
		if (w->bg && vgui_get_alpha_f(w->bg) > 0.001)
		{
			float t2 = thickness > 1 ? thickness : 0.0f;
			cb->set_source_color(vg, vgui_ovg_color(w->bg));
			cb->rounded_rectangle(vg, x + t2, y + t2, ww - t2 * 2, hh - t2 * 2, rounding);
			cb->fill(vg);
		}

		// 2) 渐变填充（内缩 thickness）
		float pad = thickness > 1 ? thickness : 0.0f;
		if (pad > 0)
			cb->translate(vg, pad, pad);
		vgui_paint_linear_rect(cb, vg,
			x, y, ww - pad * 2, hh - pad * 2, rounding,
			gradTop, gradBot);
		if (pad > 0)
			cb->translate(vg, -pad, -pad);

		// 3) 双层边框（高光在上、阴影在下）
		float line_w = thickness > 0 ? thickness : 1.0f;
		float half = line_w * 0.5f;
		cb->set_line_width(vg, line_w);

		// 上沿高光（不画底部）
		cb->set_source_color(vg, vgui_ovg_color(borderLight));
		if (pushed)
			cb->rounded_rectangle(vg, x + half, y + half, ww - line_w, hh - line_w, rounding);
		else
			cb->rounded_rectangle(vg, x + half, y + half + 1.0f, ww - line_w, hh - line_w - 1.0f, rounding);
		cb->stroke(vg);

		// 下沿阴影（完整轮廓）
		cb->set_source_color(vg, vgui_ovg_color(borderDark));
		cb->rounded_rectangle(vg, x + half, y + half, ww - line_w, hh - line_w, rounding);
		cb->stroke(vg);

		cb->restore(vg);
	}

	static void vgui_draw_button(vgui_widget* w, ovg_ctx_cb* cb, rvg_t* vg)
	{
		vgui_context* ctx = w->ctx;
		const vgui_button_style& bs = ctx->style.button;

		if (w->btn_style_kind == VGUI_BUTTON_STYLE_GRADIENT)
		{
			float thickness = w->border_width > 0 ? w->border_width : 1.0f;
			float rounding = w->rounding > 0 ? w->rounding : bs.rounding;
			vgui_draw_button_gradient(w, cb, vg, thickness, rounding);
			// 渐变按钮：不叠加纯色 fill + stroke，直接跳过进入 icon/text 绘制
		}
		else
		{
			uint32_t bg = w->bg ? w->bg : ctx->theme.button_bg;
			uint32_t border = w->border ? w->border : ctx->theme.button_border;
			if (!w->enabled)
			{
				bg = vgui_mix_color(bg, ctx->theme.disabled, bs.disabled_mix);
				border = vgui_mix_color(border, ctx->theme.disabled, bs.disabled_mix);
			}
			else
			{
				/* Toggle buttons and command buttons use the checked flag as
				 * their active state.  Keep the same press/hover feedback on
				 * top of the active tint. */
				if (w->checked)
					bg = vgui_mix_color(bg, ctx->theme.button_active,
						bs.press_mix_min);
				if (w->pressed)
					bg = vgui_mix_color(bg, ctx->theme.button_active,
						std::max(bs.press_mix_min, w->press_anim));
				else if (w->hovered || w->hover_anim > 0.01f)
					bg = vgui_mix_color(bg, ctx->theme.button_hover,
						std::max(bs.hover_mix_min, w->hover_anim));
			}
			vgui_fill_rect(cb, vg, w->abs_x, w->abs_y, w->abs_w, w->abs_h, w->rounding, vgui_widget_color(w, bg));
			if (w->border_width > 0)
			{
				float bi = w->border_width * 0.5f;
				vgui_stroke_rect(cb, vg, w->abs_x + bi, w->abs_y + bi,
					w->abs_w - w->border_width, w->abs_h - w->border_width, w->rounding,
					vgui_widget_color(w, border), w->border_width);
			}
		}

		// content area (inside padding)
		float ty0 = w->abs_y + w->padding[1] + (w->pressed ? bs.press_offset : 0.0f);
		float tx0 = w->abs_x + w->padding[0];
		float cw = std::max(0.0f, w->abs_w - w->padding[0] - w->padding[2]);
		float ch = std::max(0.0f, w->abs_h - w->padding[1] - w->padding[3]);

		// --- icon + text layout ---
		vgui_texture* tex = w->icon_tex;
		int icon_w = w->icon_w > 0 ? w->icon_w : (tex ? tex->width : 0);
		int icon_h = w->icon_h > 0 ? w->icon_h : (tex ? tex->height : 0);
		const bool has_icon = tex && tex->width > 0 && tex->height > 0 && icon_w > 0 && icon_h > 0;
		const bool has_text = !w->text.empty();
		const float gap = w->icon_gap > 0 ? w->icon_gap : bs.icon_gap;
		bool rtl = vgui_is_rtl(ctx);
		/* LEFT/RIGHT describe the start/end side, so they swap in RTL. */
		vgui_button_icon_pos pos = w->icon_pos;
		if (rtl)
		{
			if (pos == VGUI_BUTTON_ICON_LEFT) pos = VGUI_BUTTON_ICON_RIGHT;
			else if (pos == VGUI_BUTTON_ICON_RIGHT) pos = VGUI_BUTTON_ICON_LEFT;
		}

		if (has_icon || has_text)
		{
			float iw = has_icon ? (float)icon_w : 0;
			float ih = has_icon ? (float)icon_h : 0;
			float tw = has_text ? vgui_text_width(w, w->text) : 0;
			float th = has_text ? vgui_line_height(ctx, w) : 0;

			bool horiz = (pos == VGUI_BUTTON_ICON_LEFT || pos == VGUI_BUTTON_ICON_RIGHT);
			float total_w = horiz ? (iw + (has_icon && has_text ? gap : 0) + tw) : std::max(iw, tw);
			float total_h = horiz ? std::max(ih, th) : (ih + (has_icon && has_text ? gap : 0) + th);

			// 整体在 content area 内按 text_align 对齐
			float align_x = rtl ? 1.0f - w->text_align_x : w->text_align_x;
			float x_start = tx0 + (cw - total_w) * align_x;
			float y_start = ty0 + (ch - total_h) * w->text_align_y;

			float ix = 0, iy = 0, tx = 0, ty = 0;
			bool draw_icon = false, draw_text = false;

			if (horiz)
			{
				if (has_icon)
				{
					ix = x_start;
					iy = y_start + (total_h - ih) * 0.5f;
					draw_icon = true;
				}
				if (has_text)
				{
					if (pos == VGUI_BUTTON_ICON_RIGHT)
						tx = x_start;   // text 先
					else
						tx = x_start + iw + (has_icon ? gap : 0);
					ty = y_start + (total_h - th) * 0.5f;
					draw_text = true;
				}
				if (has_icon && has_text && pos == VGUI_BUTTON_ICON_RIGHT)
				{
					ix = tx + tw + gap;
					draw_icon = true;
				}
			}
			else  // VGUI_BUTTON_ICON_TOP / BOTTOM
			{
				if (has_icon)
				{
					ix = x_start + (total_w - iw) * 0.5f;
					iy = y_start;
					draw_icon = true;
				}
				if (has_text)
				{
					if (pos == VGUI_BUTTON_ICON_BOTTOM)
						ty = y_start;
					else
						ty = y_start + ih + (has_icon ? gap : 0);
					tx = x_start + (total_w - tw) * 0.5f;
					draw_text = true;
				}
				if (has_icon && has_text && pos == VGUI_BUTTON_ICON_BOTTOM)
				{
					iy = ty + th + gap;
					draw_icon = true;
				}
			}

			// 画 icon
			if (draw_icon)
			{
				vgui_texture_ensure_upload(ctx, tex, cb, vg);

				uint32_t tint = w->icon_tint;
				if (!w->enabled)
				{
					uint32_t base = tint ? tint : 0xFFFFFFFF;
					tint = vgui_mix_color(base, ctx->theme.disabled, bs.disabled_mix);
				}
				tint = vgui_ovg_color(tint ? tint : 0xFFFFFFFF);

				if (tex->native.image.id != 0 && cb->add_texture)
				{
					ovg_texture_r r = {};
					r.tex = &tex->native;
					r.rc = { 0, 0, tex->width, tex->height };
					r.dst = { (int)std::lround(ix), (int)std::lround(iy),
						(int)std::lround(iw), (int)std::lround(ih) };
					r.color = tint;
					cb->add_texture(vg, &r);
				}
				else if (tex->cpu)
				{
					// CPU 纹理暂时不支持直接在按钮上画 — 用户需先上传到 GPU 或使用 native 纹理。
					// 这里留空避免 crash；下次可以像 vgui_draw_image 那样走 geometry 路径。
				}
			}

			// 画 text — 用 tx/ty 作为 text 起点坐标，atx/aty 传给 draw_text
			// 作为 text 可用区域左上角（draw_text 内部会按 text_align 在这个矩形内对齐）。
			if (draw_text)
			{
				float atx, aty, atw, ath;
				if (horiz)
				{
					atw = std::max(0.0f, has_icon ? cw - iw - gap : cw);
					ath = ch;
					if (pos == VGUI_BUTTON_ICON_LEFT && has_icon)
						atx = tx0 + iw + gap;
					else
						atx = tx0;
					aty = ty;
				}
				else  // VGUI_BUTTON_ICON_TOP / BOTTOM
				{
					atw = cw;
					ath = std::max(0.0f, has_icon ? ch - ih - gap : ch);
					atx = tx0;
					if (pos == VGUI_BUTTON_ICON_TOP && has_icon)
						aty = ty + ih + gap;
					else
						aty = ty;
				}
				vgui_draw_text(ctx, cb, vg, w, w->text, atx, aty, atw, ath,
					vgui_text_color(w), w->text_align_x, w->text_align_y);
			}
		}
		else
		{
			// 纯文字按钮 — 保持原行为（padding 内 + text_align）
			vgui_draw_text(ctx, cb, vg, w, w->text, tx0, ty0, cw, ch,
				vgui_text_color(w), w->text_align_x, w->text_align_y);
		}

		vgui_draw_focus(w, cb, vg);
	}

	static void vgui_draw_label(vgui_widget* w, ovg_ctx_cb* cb, rvg_t* vg)
	{
		vgui_context* ctx = w->ctx;
		if (w->bg)
			vgui_fill_rect(cb, vg, w->abs_x, w->abs_y, w->abs_w, w->abs_h, w->rounding, vgui_widget_color(w, w->bg));
		if (w->border_width > 0 && w->border)
		{
			float bi = w->border_width * 0.5f;
			vgui_stroke_rect(cb, vg, w->abs_x + bi, w->abs_y + bi,
				w->abs_w - w->border_width, w->abs_h - w->border_width, w->rounding,
				vgui_widget_color(w, w->border), w->border_width);
		}
		vgui_draw_text(ctx, cb, vg, w, w->text,
			w->abs_x + w->padding[0], w->abs_y + w->padding[1],
			std::max(0.0f, w->abs_w - w->padding[0] - w->padding[2]),
			std::max(0.0f, w->abs_h - w->padding[1] - w->padding[3]),
			vgui_text_color(w), w->text_align_x, w->text_align_y);
	}

	static void vgui_draw_checkbox(vgui_widget* w, ovg_ctx_cb* cb, rvg_t* vg)
	{
		vgui_context* ctx = w->ctx;
		const vgui_style& st = ctx->style;
		bool rtl = vgui_is_rtl(ctx);
		float lh = vgui_line_height(ctx, w);
		float box = std::max(st.choice.box_min, std::min(st.choice.box_max, lh - st.choice.line_offset));
		float bx = rtl ? w->abs_x + w->abs_w - w->padding[2] - box : w->abs_x + w->padding[0];
		float by = w->abs_y + (w->abs_h - box) * 0.5f;
		uint32_t border = w->enabled
			? (w->focused ? vgui_mix_color(ctx->theme.button_border, ctx->theme.accent, vgui_focus_alpha(w))
				: ctx->theme.button_border)
			: ctx->theme.disabled;
		uint32_t accent = w->enabled ? (w->accent ? w->accent : ctx->theme.accent) : ctx->theme.disabled;

		vgui_fill_rect(cb, vg, bx, by, box, box, st.checkbox.rounding, vgui_widget_color(w, ctx->theme.track));
		if (w->check_anim > 0.01f)
		{
			uint32_t c = vgui_mix_color(ctx->theme.track, accent, w->check_anim);
			vgui_fill_rect(cb, vg, bx, by, box, box, st.checkbox.rounding, vgui_widget_color(w, c));
		}
		if (st.checkbox.border_width > 0)
		{
			float bi = st.checkbox.border_width * 0.5f;
			vgui_stroke_rect(cb, vg, bx + bi, by + bi, box - st.checkbox.border_width,
				box - st.checkbox.border_width, st.checkbox.rounding, vgui_widget_color(w, border),
				st.checkbox.border_width);
		}

		if (w->check_anim > 0.05f && st.checkbox.check_width > 0)
		{
			float t = w->check_anim;
			cb->set_line_width(vg, st.checkbox.check_width);
			cb->set_line_cap(vg, 1);
			cb->set_line_join(vg, 1);
			cb->move_to(vg, bx + box * st.checkbox.check_start_x, by + box * st.checkbox.check_start_y);
			cb->line_to(vg, bx + box * st.checkbox.check_mid_x, by + box * st.checkbox.check_mid_y);
			cb->line_to(vg, bx + box * st.checkbox.check_end_x, by + box * st.checkbox.check_end_y);
			cb->set_source_color(vg, vgui_ovg_color(vgui_widget_color(w,
				vgui_alpha_color(ctx->theme.checkmark, t))));
			cb->stroke(vg);
		}

		if (!w->text.empty())
		{
			if (rtl)
			{
				float tx = w->abs_x + w->padding[0];
				vgui_draw_text(ctx, cb, vg, w, w->text, tx, w->abs_y,
					std::max(0.0f, bx - st.choice.gap - tx), w->abs_h,
					vgui_text_color(w), w->text_align_x, w->text_align_y);
			}
			else
			{
				float tx = bx + box + st.choice.gap;
				vgui_draw_text(ctx, cb, vg, w, w->text, tx, w->abs_y, std::max(0.0f, w->abs_x + w->abs_w - tx),
					w->abs_h, vgui_text_color(w), w->text_align_x, w->text_align_y);
			}
		}
		vgui_draw_focus(w, cb, vg);
	}

	static void vgui_draw_radio(vgui_widget* w, ovg_ctx_cb* cb, rvg_t* vg)
	{
		vgui_context* ctx = w->ctx;
		const vgui_style& st = ctx->style;
		bool rtl = vgui_is_rtl(ctx);
		float lh = vgui_line_height(ctx, w);
		float d = std::max(st.choice.box_min, std::min(st.choice.box_max, lh - st.choice.line_offset));
		float cx = rtl ? w->abs_x + w->abs_w - w->padding[2] - d * 0.5f : w->abs_x + w->padding[0] + d * 0.5f;
		float cy = w->abs_y + w->abs_h * 0.5f;
		uint32_t border = w->enabled
			? (w->focused ? vgui_mix_color(ctx->theme.button_border, ctx->theme.accent, vgui_focus_alpha(w))
				: ctx->theme.button_border)
			: ctx->theme.disabled;
		uint32_t accent = w->enabled ? (w->accent ? w->accent : ctx->theme.accent) : ctx->theme.disabled;

		cb->circle(vg, cx, cy, d * 0.5f);
		cb->set_source_color(vg, vgui_ovg_color(vgui_widget_color(w, ctx->theme.track)));
		cb->fill(vg);
		if (st.radio.border_width > 0)
		{
			cb->circle(vg, cx, cy, d * 0.5f);
			cb->set_line_width(vg, st.radio.border_width);
			cb->set_source_color(vg, vgui_ovg_color(vgui_widget_color(w, border)));
			cb->stroke(vg);
		}
		if (w->check_anim > 0.01f)
		{
			cb->circle(vg, cx, cy, d * 0.5f * st.radio.dot_scale * w->check_anim);
			cb->set_source_color(vg, vgui_ovg_color(vgui_widget_color(w, accent)));
			cb->fill(vg);
		}

		if (!w->text.empty())
		{
			if (rtl)
			{
				float tx = w->abs_x + w->padding[0];
				vgui_draw_text(ctx, cb, vg, w, w->text, tx, w->abs_y,
					std::max(0.0f, cx - d * 0.5f - st.choice.gap - tx), w->abs_h,
					vgui_text_color(w), w->text_align_x, w->text_align_y);
			}
			else
			{
				float tx = cx + d * 0.5f + st.choice.gap;
				vgui_draw_text(ctx, cb, vg, w, w->text, tx, w->abs_y, std::max(0.0f, w->abs_x + w->abs_w - tx),
					w->abs_h, vgui_text_color(w), w->text_align_x, w->text_align_y);
			}
		}
		vgui_draw_focus(w, cb, vg);
	}

	static void vgui_draw_switch(vgui_widget* w, ovg_ctx_cb* cb, rvg_t* vg)
	{
		vgui_context* ctx = w->ctx;
		const vgui_switch_style& ss = ctx->style.switch_;
		bool rtl = vgui_is_rtl(ctx);
		float lh = vgui_line_height(ctx, w);
		float track_w = ss.width;
		float track_h = std::max(ss.min_height, std::min(ss.max_height, lh - ss.line_offset));
		float x = rtl ? w->abs_x + w->abs_w - w->padding[2] - track_w : w->abs_x + w->padding[0];
		float y = w->abs_y + (w->abs_h - track_h) * 0.5f;
		uint32_t on = w->enabled ? (w->accent ? w->accent : ctx->theme.accent) : ctx->theme.disabled;
		uint32_t off = w->enabled ? ctx->theme.track
			: vgui_mix_color(ctx->theme.track, ctx->theme.disabled, ss.disabled_mix);
		float t = w->check_anim;
		uint32_t track = vgui_mix_color(off, on, t);
		vgui_fill_rect(cb, vg, x, y, track_w, track_h, track_h * 0.5f, vgui_widget_color(w, track));

		float radius = std::max(0.0f, track_h * 0.5f - ss.thumb_inset);
		float travel = std::max(0.0f, track_w - radius * 2.0f - ss.thumb_inset * 2.0f);
		float kx = rtl
			? x + track_w - radius - ss.thumb_inset - t * travel
			: x + radius + ss.thumb_inset + t * travel;
		float ky = y + track_h * 0.5f;
		cb->circle(vg, kx, ky, radius);
		cb->set_source_color(vg, vgui_ovg_color(vgui_widget_color(w, ctx->theme.switch_thumb)));
		cb->fill(vg);

		if (!w->text.empty())
		{
			if (rtl)
			{
				float tx = w->abs_x + w->padding[0];
				vgui_draw_text(ctx, cb, vg, w, w->text, tx, w->abs_y,
					std::max(0.0f, x - ss.gap - tx), w->abs_h,
					vgui_text_color(w), w->text_align_x, w->text_align_y);
			}
			else
			{
				float tx = x + track_w + ss.gap;
				vgui_draw_text(ctx, cb, vg, w, w->text, tx, w->abs_y, std::max(0.0f, w->abs_x + w->abs_w - tx),
					w->abs_h, vgui_text_color(w), w->text_align_x, w->text_align_y);
			}
		}
		vgui_draw_focus(w, cb, vg);
	}

	static void vgui_draw_slider(vgui_widget* w, ovg_ctx_cb* cb, rvg_t* vg)
	{
		vgui_context* ctx = w->ctx;
		const vgui_slider_style& ss = ctx->style.slider;
		uint32_t accent = w->enabled ? (w->accent ? w->accent : ctx->theme.accent) : ctx->theme.disabled;
		uint32_t track = w->enabled ? (w->track ? w->track : ctx->theme.track)
			: vgui_mix_color(ctx->theme.track, ctx->theme.disabled, ss.disabled_mix);
		uint32_t knob_color = (w->focused && w->enabled)
			? vgui_mix_color(accent, ctx->theme.accent_hover, vgui_focus_alpha(w))
			: accent;
		float t = vgui_clampf(w->value, 0.0f, 1.0f);
		float knob = ss.knob_radius;
		float thickness = ss.track_thickness;
		float track_rounding = thickness * 0.5f;
		float inner_w = std::max(1.0f, w->abs_w - w->padding[0] - w->padding[2]);
		float inner_h = std::max(1.0f, w->abs_h - w->padding[1] - w->padding[3]);

		if (w->bg)
			vgui_fill_rect(cb, vg, w->abs_x, w->abs_y, w->abs_w, w->abs_h, w->rounding,
				vgui_widget_color(w, vgui_alpha_color(w->bg, ss.background_alpha)));

		if (!w->vertical)
		{
			float x = w->abs_x + w->padding[0];
			float y = w->abs_y + w->abs_h * 0.5f - thickness * 0.5f;
			vgui_fill_rect(cb, vg, x, y, inner_w, thickness, track_rounding, vgui_widget_color(w, track));
			if (vgui_is_rtl(ctx))
			{
				float fill_w = inner_w * t;
				vgui_fill_rect(cb, vg, x + inner_w - fill_w, y, fill_w, thickness, track_rounding,
					vgui_widget_color(w, accent));
				cb->circle(vg, x + inner_w - fill_w, y + thickness * 0.5f, knob);
			}
			else
			{
				vgui_fill_rect(cb, vg, x, y, inner_w * t, thickness, track_rounding, vgui_widget_color(w, accent));
				cb->circle(vg, x + inner_w * t, y + thickness * 0.5f, knob);
			}
			cb->set_source_color(vg, vgui_ovg_color(vgui_widget_color(w, knob_color)));
			cb->fill(vg);
		}
		else
		{
			float x = w->abs_x + w->abs_w * 0.5f - thickness * 0.5f;
			float y = w->abs_y + w->padding[1];
			vgui_fill_rect(cb, vg, x, y, thickness, inner_h, track_rounding, vgui_widget_color(w, track));
			float fill_h = inner_h * t;
			vgui_fill_rect(cb, vg, x, y + inner_h - fill_h, thickness, fill_h, track_rounding,
				vgui_widget_color(w, accent));
			float ky = y + inner_h - fill_h;
			cb->circle(vg, x + thickness * 0.5f, ky, knob);
			cb->set_source_color(vg, vgui_ovg_color(vgui_widget_color(w, knob_color)));
			cb->fill(vg);
		}
		vgui_draw_focus(w, cb, vg);
	}

	static void vgui_draw_progress(vgui_widget* w, ovg_ctx_cb* cb, rvg_t* vg)
	{
		vgui_context* ctx = w->ctx;
		const vgui_progress_style& ps = ctx->style.progress;
		uint32_t accent = w->enabled ? (w->accent ? w->accent : ctx->theme.accent) : ctx->theme.disabled;
		uint32_t track = w->enabled ? (w->track ? w->track : ctx->theme.track)
			: vgui_mix_color(ctx->theme.track, ctx->theme.disabled, ps.disabled_mix);
		float t = vgui_clampf(w->value, 0.0f, 1.0f);
		float inner_w = std::max(0.0f, w->abs_w - w->padding[0] - w->padding[2]);
		float inner_h = std::max(0.0f, w->abs_h - w->padding[1] - w->padding[3]);
		float x = w->abs_x + w->padding[0];
		float y = w->abs_y + w->padding[1];
		float rounding = w->rounding > 0 ? w->rounding : ps.rounding;

		if (!w->vertical)
		{
			vgui_fill_rect(cb, vg, x, y, inner_w, inner_h, rounding, vgui_widget_color(w, track));
			if (vgui_is_rtl(ctx))
			{
				float fill_w = inner_w * t;
				vgui_fill_rect(cb, vg, x + inner_w - fill_w, y, fill_w, inner_h, rounding,
					vgui_widget_color(w, accent));
			}
			else
			{
				vgui_fill_rect(cb, vg, x, y, inner_w * t, inner_h, rounding, vgui_widget_color(w, accent));
			}
		}
		else
		{
			vgui_fill_rect(cb, vg, x, y, inner_w, inner_h, rounding, vgui_widget_color(w, track));
			float fh = inner_h * t;
			vgui_fill_rect(cb, vg, x, y + inner_h - fh, inner_w, fh, rounding, vgui_widget_color(w, accent));
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
		const vgui_style& st = ctx->style;
		uint32_t bg = w->bg ? w->bg : ctx->theme.edit_bg;
		uint32_t base_border = w->border ? w->border : ctx->theme.button_border;
		uint32_t border = w->enabled
			? (w->focused ? vgui_mix_color(base_border, ctx->theme.accent, vgui_focus_alpha(w)) : base_border)
			: ctx->theme.disabled;
		if (!w->enabled) bg = vgui_mix_color(bg, ctx->theme.disabled, st.edit.disabled_bg_mix);

		vgui_fill_rect(cb, vg, w->abs_x, w->abs_y, w->abs_w, w->abs_h, w->rounding, vgui_widget_color(w, bg));
		if (w->border_width > 0)
		{
			float bi = w->border_width * 0.5f;
			vgui_stroke_rect(cb, vg, w->abs_x + bi, w->abs_y + bi,
				w->abs_w - w->border_width, w->abs_h - w->border_width, w->rounding,
				vgui_widget_color(w, border), w->border_width);
		}

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
		vgui_edit_clamp_scroll(w);
		vgui_clip_push(clips, cb, vg, ix, iy, iw, ih);

		int sel_a = std::min(w->edit_cursor, w->edit_anchor);
		int sel_b = std::max(w->edit_cursor, w->edit_anchor);
		bool rtl = vgui_is_rtl(ctx);
		/* LTR anchors the text at the left edge and shifts it left as the user
		 * scrolls; RTL anchors it at the right edge and shifts it right. */
		float tx = rtl ? ix + w->edit_scroll_x : ix - w->edit_scroll_x;

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
				float wa = vgui_text_width_n(w, shown.data(), sa);
				float wb = vgui_text_width_n(w, shown.data(), sb);
				float sx = rtl ? tx + iw - wb : tx + wa;
				float sw = wb - wa;
				// 选中换行符时补一小段尾巴，让跨行选中看起来是连续的
				if (sel_b > line_end)
				{
					float tail = std::min(st.edit.selection_tail_max_width, lh * st.edit.selection_tail_line_ratio);
					if (rtl)
						sx -= tail;	// RTL 下选区末端在左侧
					sw += tail;
				}
				if (sw < st.edit.selection_min_width) sw = st.edit.selection_min_width;
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
			if (!w->focused)
				selc = vgui_alpha_color(selc, st.edit.selection_unfocused_alpha);
			vgui_fill_selection(cb, vg, sel_bands, w->sel_rounding, vgui_widget_color(w, selc));
		}

		for (const vgui_edit_draw_line& dl : draw_lines)
		{
			vgui_draw_text(ctx, cb, vg, w, dl.shown, tx, dl.ty, std::max(iw, 1.0f), lh, color, 0.0f, 0.5f);

			if (w->focused && w->edit_cursor >= dl.line_start && w->edit_cursor <= dl.line_end)
			{
				// Draw the caret only for the line containing the cursor.
				float prefix = vgui_text_width_n(w, dl.shown.data(), dl.caret_byte);
				float cx = rtl ? tx + iw - prefix : tx + prefix;
				bool blink = (std::fmod(w->caret_blink, st.edit.caret_blink_period) <
					st.edit.caret_blink_period * st.edit.caret_blink_duty);
				if (blink)
					vgui_fill_rect(cb, vg, cx, dl.ty, w->caret_width, lh, 0, color);
			}

			if (!w->ime_text.empty() && w->edit_cursor >= dl.line_start && w->edit_cursor <= dl.line_end)
			{
				float prefix = vgui_text_width_n(w, dl.shown.data(), dl.caret_byte);
				float cx = rtl ? tx + iw - prefix : tx + prefix;
				if (rtl)
					vgui_draw_text_raw(ctx, cb, vg, w, w->ime_text, cx - std::max(iw, 1.0f), dl.ty,
						std::max(iw, 1.0f), lh, ctx->theme.accent, 1.0f, 0.5f);
				else
					vgui_draw_text_raw(ctx, cb, vg, w, w->ime_text, cx, dl.ty, iw, lh,
						ctx->theme.accent, 0.0f, 0.5f);
				int ia = vgui_edit_clamp_index(w->ime_text, w->ime_cursor);
				int ib = vgui_edit_clamp_index(w->ime_text, w->ime_cursor + w->ime_length);
				if (ib < ia) std::swap(ia, ib);
				float ua = vgui_text_width_n(w, w->ime_text.data(), ia);
				float ub = vgui_text_width_n(w, w->ime_text.data(), ib);
				float ux = rtl ? cx - ub : cx + ua;
				float uw = ub - ua;
				vgui_fill_rect(cb, vg, ux, dl.ty + lh - st.edit.ime_underline_width,
					std::max(st.edit.ime_underline_min_width, uw), st.edit.ime_underline_width, 0,
					ctx->theme.accent);
			}
		}

		vgui_clip_pop(clips, cb, vg);
		cb->restore(vg);

		// 多行编辑框的内容滚动条（覆盖在文本右侧/底部，不占用布局宽度）
		if (w->multiline)
		{
			vgui_scrollbar_info g;
			uint32_t track_color = vgui_widget_color(w, ctx->theme.track);
			float bar_w = st.edit.scrollbar_width;
			float bar_r = st.edit.scrollbar_rounding;
			// Keep the track opaque: compositing it with whatever happens to be
			// behind the widget can make it exactly the same color as the thumb
			// (e.g. the light theme's #DCDCDC track over a black root became
			// #9A9A9A, the same value as its thumb), which makes the whole
			// scrollbar look like one long thumb.
			if (vgui_scrollbar_geom(w, 3, &g))
			{
				vgui_fill_rect(cb, vg, g.track, g.start, bar_w, g.span, bar_r, track_color);
				vgui_fill_rect(cb, vg, g.track, g.thumb, bar_w, g.thumb_len, bar_r, vgui_scrollbar_thumb_color(w, 3));
			}
			if (vgui_scrollbar_geom(w, 4, &g))
			{
				vgui_fill_rect(cb, vg, g.start, g.track, g.span, bar_w, bar_r, track_color);
				vgui_fill_rect(cb, vg, g.thumb, g.track, g.thumb_len, bar_w, bar_r, vgui_scrollbar_thumb_color(w, 4));
			}
		}

		vgui_draw_focus(w, cb, vg);
	}

	static void vgui_draw_scrollbars(vgui_widget* w, ovg_ctx_cb* cb, rvg_t* vg)
	{
		vgui_context* ctx = w->ctx;
		const vgui_panel_style& ps = ctx->style.panel;
		float ix = w->abs_x + w->padding[0];
		float iy = w->abs_y + w->padding[1];
		float iw = std::max(0.0f, w->abs_w - w->padding[0] - w->padding[2]);
		float ih = std::max(0.0f, w->abs_h - w->padding[1] - w->padding[3]);
		float bar_w = ps.scrollbar_width;
		float bar_r = ps.scrollbar_rounding;
		float corner = bar_w + ps.scrollbar_inset;
		if (w->scrollable_y && w->content_h > ih + 0.5f && ih > ps.scrollbar_min_track)
		{
			float track_x = vgui_is_rtl(ctx) ? w->abs_x : w->abs_x + w->abs_w - corner;
			float th = std::min(ih, std::max(ps.scrollbar_min_thumb, ih * ih / w->content_h));
			float max_scroll = w->content_h - ih;
			float ty = iy + (max_scroll > 0 ? (w->scroll_y / max_scroll) * (ih - th) : 0);
			vgui_fill_rect(cb, vg, track_x, iy, bar_w, ih, bar_r, vgui_widget_color(w, ctx->theme.track));
			vgui_fill_rect(cb, vg, track_x, ty, bar_w, th, bar_r, vgui_scrollbar_thumb_color(w, 1));
		}
		if (w->scrollable_x && w->content_w > iw + 0.5f && iw > ps.scrollbar_min_track)
		{
			float track_y = w->abs_y + w->abs_h - corner;
			float tw = std::min(iw, std::max(ps.scrollbar_min_thumb, iw * iw / w->content_w));
			float max_scroll = w->content_w - iw;
			float ratio = max_scroll > 0 ? (w->scroll_x / max_scroll) : 0.0f;
			if (vgui_is_rtl(ctx)) ratio = 1.0f - ratio;
			float tx = ix + ratio * (iw - tw);
			vgui_fill_rect(cb, vg, ix, track_y, iw, bar_w, bar_r, vgui_widget_color(w, ctx->theme.track));
			vgui_fill_rect(cb, vg, tx, track_y, tw, bar_w, bar_r, vgui_scrollbar_thumb_color(w, 2));
		}
	}

	static void vgui_draw_text_clipped(vgui_context* ctx, ovg_ctx_cb* cb, rvg_t* vg, vgui_widget* w,
		const std::string& text, float x, float y, float width, float height,
		uint32_t color, float ax, float ay, vgui_clip_state* clips)
	{
		if (width <= 0.0f || color == 0) return;
		cb->save(vg);
		vgui_clip_push(clips, cb, vg, x, y, width, height);
		vgui_draw_text(ctx, cb, vg, w, text, x, y, width, height, color, ax, ay);
		vgui_clip_pop(clips, cb, vg);
		cb->restore(vg);
	}

	static void vgui_texture_ensure_upload(vgui_context* ctx, vgui_texture* texture,
		ovg_ctx_cb* cb, rvg_t* vg)
	{
		if (!ctx || !texture || !texture->cpu || !texture->dirty ||
			texture->width <= 0 || texture->height <= 0 ||
			texture->pixels.empty() || !cb || !vg || !cb->image_update)
			return;
		if (texture->upload_frame == ctx->frame_index)
			return;

		vg_image_desc_t desc = {};
		desc.img = &texture->image;
		desc.width = (uint32_t)texture->width;
		desc.height = (uint32_t)texture->height;
		desc.format = (uint32_t)vgui_ovg_texture_format(texture->format);
		desc.stride = (uint32_t)texture->stride;
		desc.pixels = texture->pixels.data();
		desc.x = 0;
		desc.y = 0;
		desc.w = desc.width;
		desc.h = desc.height;
		desc.is_copy = true;
		cb->image_update(vg, &texture->image, &desc);

		texture->upload_frame = ctx->frame_index;
		texture->dirty = false;
		texture->image.valid = false;
	}

	static void vgui_draw_image(vgui_widget* w, ovg_ctx_cb* cb, rvg_t* vg, vgui_clip_state* clips)
	{
		vgui_context* ctx = w->ctx;
		vgui_image_state* image = w->image.get();
		if (!image) return;
		const vgui_image_style& is = ctx->style.image;

		if (w->bg)
			vgui_fill_rect(cb, vg, w->abs_x, w->abs_y, w->abs_w, w->abs_h,
				w->rounding, vgui_widget_color(w, w->bg));

		uint32_t tint = image->tint;
		if (!w->enabled)
			tint = vgui_mix_color(tint, ctx->theme.disabled, is.disabled_mix);
		// image->tint uses the public 0xAARRGGBB order; the image draw paths
		// (add_image, add_texture, add_geometry) all expect OVG's channel order.
		tint = vgui_ovg_color(vgui_widget_color(w, tint));

		vgui_texture* texture = image->texture;
		if (texture)
			vgui_texture_ensure_upload(ctx, texture, cb, vg);

		float dst_x = w->abs_x;
		float dst_y = w->abs_y;
		float dst_w = w->abs_w;
		float dst_h = w->abs_h;

		if (texture && texture->image.id != 0 && texture->width > 0 && texture->height > 0 &&
			dst_w > 0.0f && dst_h > 0.0f)
		{
			const int tex_w = texture->width;
			const int tex_h = texture->height;
			int sx = std::min(std::max(image->src_x, 0), tex_w - 1);
			int sy = std::min(std::max(image->src_y, 0), tex_h - 1);
			int sw = image->src_w > 0 ? std::min(image->src_w, tex_w - sx) : tex_w - sx;
			int sh = image->src_h > 0 ? std::min(image->src_h, tex_h - sy) : tex_h - sy;
			sw = std::max(sw, 1);
			sh = std::max(sh, 1);

			if (image->fit == VGUI_IMAGE_FIT_COVER && sw > 0 && sh > 0)
			{
				// Crop the source to the destination aspect ratio so the image
				// fills the viewport without being stretched.
				const float dst_aspect = dst_w / dst_h;
				const float src_aspect = (float)sw / (float)sh;
				if (src_aspect > dst_aspect)
				{
					int nw = std::max(1, (int)std::lround((float)sh * dst_aspect));
					sx += (sw - nw) / 2;
					sw = nw;
				}
				else if (src_aspect < dst_aspect)
				{
					int nh = std::max(1, (int)std::lround((float)sw / dst_aspect));
					sy += (sh - nh) / 2;
					sh = nh;
				}
			}

			if (image->fit == VGUI_IMAGE_FIT_CONTAIN || image->fit == VGUI_IMAGE_FIT_NONE)
			{
				const float scale = image->fit == VGUI_IMAGE_FIT_NONE
					? 1.0f
					: std::min(dst_w / (float)sw, dst_h / (float)sh);
				const float fit_w = (float)sw * scale;
				const float fit_h = (float)sh * scale;
				dst_x += (dst_w - fit_w) * 0.5f;
				dst_y += (dst_h - fit_h) * 0.5f;
				dst_w = fit_w;
				dst_h = fit_h;
			}

			if (image->pixel_snap)
			{
				dst_x = std::round(dst_x);
				dst_y = std::round(dst_y);
				dst_w = std::round(dst_w);
				dst_h = std::round(dst_h);
			}
			if (dst_w >= 1.0f && dst_h >= 1.0f)
			{
				cb->save(vg);
				vgui_clip_push(clips, cb, vg, w->abs_x, w->abs_y, w->abs_w, w->abs_h);
				const bool native = !texture->cpu && texture->native.image.id != 0;
				if (native && cb->add_texture)
				{
					ovg_texture_r r = {};
					r.tex = &texture->native;
					r.rc = { sx, sy, sw, sh };
					r.dst = { (int)std::lround(dst_x), (int)std::lround(dst_y),
						(int)std::lround(dst_w), (int)std::lround(dst_h) };
					r.color = tint;
					r.flip = (int8_t)((image->flip_x ? FLIP_HORIZONTAL : 0) |
						(image->flip_y ? FLIP_VERTICAL : 0));
					cb->add_texture(vg, &r);
				}
				else if (image->flip_x || image->flip_y)
				{
					// CPU textures keep the geometry path because add_texture only
					// accepts backend-owned ovg_texture_t handles.
					gem_info_t info = {};
					info.blendMode = (int8_t)blendMode_e::normal;
					info.flags = (uint8_t)depth_stencil_State::d_stenciltest_enable;
					info.frontFace = 0;
					info.cullMode = 0;
					info.shader = ST_NONE;
					const glm::mat4 mat = glm::ortho(0.0f, (float)vg->width,
						0.0f, (float)vg->height, -1.0f, 1.0f);
					cb->set_geom_state(vg, &info, &mat);

					float u0 = (float)sx / (float)tex_w;
					float v0 = (float)sy / (float)tex_h;
					float u1 = (float)(sx + sw) / (float)tex_w;
					float v1 = (float)(sy + sh) / (float)tex_h;
					if (image->flip_x) std::swap(u0, u1);
					if (image->flip_y) std::swap(v0, v1);

					const float x0 = dst_x, y0 = dst_y;
					const float x1 = dst_x + dst_w, y1 = dst_y + dst_h;
					const float xy[8] = { x0, y0, x1, y0, x1, y1, x0, y1 };
					const float uv[8] = { u0, v0, u1, v0, u1, v1, u0, v1 };
					const uint32_t colors[4] = { tint, tint, tint, tint };
					const uint32_t indices[6] = { 0, 1, 2, 0, 2, 3 };
					cb->add_geometry(vg, &texture->image,
						xy, 2 * sizeof(float), colors, sizeof(uint32_t),
						uv, 2 * sizeof(float), 4, indices, 6, sizeof(uint32_t), 1);
				}
				else
				{
					ovg_image_r r = {};
					r.img = native ? &texture->native.image : &texture->image;
					r.rc = { sx, sy, sw, sh };
					r.dst = { (int)std::lround(dst_x), (int)std::lround(dst_y),
						(int)std::lround(dst_w), (int)std::lround(dst_h) };
					r.color = tint;
					cb->add_image(vg, &r);
				}
				vgui_clip_pop(clips, cb, vg);
				cb->restore(vg);
			}
		}

		if (w->border_width > 0 && w->border)
		{
			float bi = w->border_width * 0.5f;
			vgui_stroke_rect(cb, vg, w->abs_x + bi, w->abs_y + bi,
				w->abs_w - w->border_width, w->abs_h - w->border_width,
				w->rounding, vgui_widget_color(w, w->border), w->border_width);
		}
		vgui_draw_focus(w, cb, vg);
	}

	static uint32_t vgui_row_text_color(vgui_widget* w, bool selected, bool enabled)
	{
		vgui_context* ctx = w->ctx;
		uint32_t c = selected
			? (ctx->theme.row_selected_text ? ctx->theme.row_selected_text : ctx->theme.text)
			: (ctx->theme.row_text ? ctx->theme.row_text : ctx->theme.text);
		if (!enabled)
			c = vgui_mix_color(c, ctx->theme.disabled,
				w->kind == VGUI_TREE ? ctx->style.tree.disabled_mix : ctx->style.list.disabled_mix);
		return vgui_widget_color(w, c);
	}

	// F04 行文字颜色覆盖：0 = 用主题色；禁用行保持 disabled 混合。
	static uint32_t vgui_row_text_color_of(vgui_widget* w, uint32_t override_color,
		bool selected, bool enabled)
	{
		if (!override_color)
			return vgui_row_text_color(w, selected, enabled);
		vgui_context* ctx = w->ctx;
		if (!enabled)
			override_color = vgui_mix_color(override_color, ctx->theme.disabled,
				w->kind == VGUI_TREE ? ctx->style.tree.disabled_mix : ctx->style.list.disabled_mix);
		return vgui_widget_color(w, override_color);
	}

	static void vgui_draw_row_texture(vgui_widget* w, ovg_ctx_cb* cb, rvg_t* vg,
		vgui_texture* tex, float x, float y, float size, uint32_t tint, bool enabled)
	{
		vgui_context* ctx = w->ctx;
		if (!ctx || !tex || size <= 0.0f) return;
		vgui_texture_ensure_upload(ctx, tex, cb, vg);
		uint32_t color = tint ? tint : 0xFFFFFFFF;
		if (!enabled)
			color = vgui_mix_color(color, ctx->theme.disabled, 0.5f);
		// Native textures use the backend texture path; CPU textures keep the
		// image handle filled by image_update and must go through add_image.
		const bool native = !tex->cpu && tex->native.image.id != 0;
		if (native && cb->add_texture)
		{
			ovg_texture_r r = {};
			r.tex = &tex->native;
			r.rc = { 0, 0, tex->width, tex->height };
			r.dst = { (int)std::lround(x), (int)std::lround(y), (int)std::lround(size), (int)std::lround(size) };
			r.color = vgui_ovg_color(color);
			cb->add_texture(vg, &r);
			return;
		}
		if (!native && tex->image.id != 0 && cb->add_image)
		{
			ovg_image_r r = {};
			r.img = &tex->image;
			r.rc = { 0, 0, tex->width, tex->height };
			r.dst = { (int)std::lround(x), (int)std::lround(y), (int)std::lround(size), (int)std::lround(size) };
			r.color = vgui_ovg_color(color);
			cb->add_image(vg, &r);
		}
	}

	// F04 行尾内联按钮绘制。hover 行 id 与按钮序号由调用方从控件状态取出。
	static void vgui_draw_row_buttons(vgui_widget* w, ovg_ctx_cb* cb, rvg_t* vg,
		float ix, float iw, float row_y, float row_h, vgui_item_id row_id,
		vgui_item_id hover_row_id, int hover_button, int button_count,
		const std::vector<vgui_texture*>& icons, uint32_t text_color)
	{
		vgui_context* ctx = w->ctx;
		if (!ctx || button_count <= 0) return;
		for (int b = 0; b < button_count; ++b)
		{
			float bx = 0, bs = 0;
			vgui_row_button_rect(w, ix, iw, row_y, row_h, b, button_count, &bx, &bs);
			const float by = row_y + (row_h - bs) * 0.5f;
			const bool hot = row_id != 0 && row_id == hover_row_id && b == hover_button;
			uint32_t bg = hot
				? (ctx->theme.row_hover ? ctx->theme.row_hover : 0x334A4A4A)
				: (ctx->theme.panel_border ? ctx->theme.panel_border : 0xFF2E3440);
			vgui_fill_rect(cb, vg, bx, by, bs, bs, 3.0f, vgui_widget_color(w, bg));
			vgui_texture* tex = b < (int)icons.size() ? icons[(size_t)b] : nullptr;
			if (tex)
				vgui_draw_row_texture(w, cb, vg, tex, bx + bs * 0.2f, by + bs * 0.2f,
					bs * 0.6f, text_color, true);
		}
	}

	static void vgui_draw_model_frame(vgui_widget* w, ovg_ctx_cb* cb, rvg_t* vg)
	{
		vgui_context* ctx = w->ctx;
		uint32_t bg = w->bg ? w->bg : (ctx->theme.list_bg ? ctx->theme.list_bg : ctx->theme.panel_bg);
		uint32_t border = w->border ? w->border
			: (ctx->theme.list_border ? ctx->theme.list_border : ctx->theme.panel_border);
		vgui_fill_rect(cb, vg, w->abs_x, w->abs_y, w->abs_w, w->abs_h, w->rounding,
			vgui_widget_color(w, bg));
		if (w->border_width > 0)
		{
			float bi = w->border_width * 0.5f;
			vgui_stroke_rect(cb, vg, w->abs_x + bi, w->abs_y + bi,
				w->abs_w - w->border_width, w->abs_h - w->border_width, w->rounding,
				vgui_widget_color(w, border), w->border_width);
		}
	}

	// 由滚动位置与固定行高直接求出需要绘制的行区间，避免每帧遍历全部
	// 列表/树数据。列表和树都是等高行，所以首行与末行可以直接算出来。
	struct vgui_row_window
	{
		int first = 0;
		int last = 0;
	};

	static vgui_row_window vgui_row_window_of(vgui_widget* w, int count, float ih, float row_h)
	{
		vgui_row_window win;
		if (count <= 0 || ih <= 0.0f || row_h <= 0.0f) return win;
		float scroll = std::max(0.0f, w->scroll_y);
		/* 上下各多算一行做安全边界，浮点误差不会让首尾行被裁掉。 */
		int first = (int)std::floor(scroll / row_h) - 1;
		if (first < 0) first = 0;
		int last = (int)std::ceil((scroll + ih) / row_h) + 1;
		if (last > count) last = count;
		if (last < first) last = first;
		win.first = first;
		win.last = last;
		return win;
	}

	static void vgui_draw_list(vgui_widget* w, ovg_ctx_cb* cb, rvg_t* vg, vgui_clip_state* clips)
	{
		vgui_context* ctx = w->ctx;
		vgui_list_state* st = vgui_list_of(w);
		if (!st) return;
		const vgui_list_style& ls = ctx->style.list;
		float ix = w->abs_x + w->padding[0];
		float iy = w->abs_y + w->padding[1];
		float iw = std::max(0.0f, w->abs_w - w->padding[0] - w->padding[2]);
		float ih = std::max(0.0f, w->abs_h - w->padding[1] - w->padding[3]);
		float row_h = vgui_list_row_height(w);

		vgui_draw_model_frame(w, cb, vg);
		cb->save(vg);
		vgui_clip_push(clips, cb, vg, ix, iy, iw, ih);
		const int row_count = vgui_list_row_count(w, st);
		vgui_row_window win = vgui_row_window_of(w, row_count, ih, row_h);
		for (int i = win.first; i < win.last; ++i)
		{
			vgui_list_item* row = vgui_list_row_materialize(w, st, i);
			if (!row) continue;
			const vgui_list_item& item = *row;
			float y = iy - w->scroll_y + (float)i * row_h;
			if (y + row_h <= iy || y >= iy + ih) continue;
			bool selected = vgui_id_selected(st->selected, item.id);
			if (selected)
				vgui_fill_rect(cb, vg, ix, y, iw, row_h, ls.rounding,
					vgui_widget_color(w, ctx->theme.row_selected ? ctx->theme.row_selected : 0x55409EFF));
			else if (item.enabled && st->hover == item.id)
				vgui_fill_rect(cb, vg, ix, y, iw, row_h, ls.rounding,
					vgui_widget_color(w, ctx->theme.row_hover ? ctx->theme.row_hover : 0x334A4A4A));

			const bool rtl = vgui_is_rtl(ctx);
			uint32_t color = vgui_row_text_color_of(w, item.text_color, selected, item.enabled != 0);

			// F04 自定义行绘制：替换标准内容绘制；选中/悬停底色与按钮仍由控件管理。
			if (st->virtual_mode && st->source.row_draw)
			{
				st->source.row_draw(st->source.user, i, cb, vg, ix, y, iw, row_h);
				vgui_draw_row_buttons(w, cb, vg, ix, iw, y, row_h, item.id,
					st->hover, st->hover_button, item.button_count,
					item.button_icons, color);
				continue;
			}

			// 行尾按钮占用的宽度，文本与 detail 需避让。
			float buttons_w = 0.0f;
			if (item.button_count > 0)
			{
				const float bsize = vgui_row_button_size(w, row_h);
				buttons_w = 4.0f + (float)item.button_count * bsize
					+ (float)(item.button_count - 1) * 4.0f;
			}

			float detail_w = item.detail.empty() ? 0.0f : vgui_text_width(w, item.detail);
			float text_w = iw - 12.0f - buttons_w - (detail_w > 0 ? detail_w + 16.0f : 0.0f);
			float text_x = rtl ? ix + 6.0f + buttons_w + (detail_w > 0 ? detail_w + 16.0f : 0.0f)
				: ix + 6.0f;
			if (item.icon)
			{
				const float isize = std::max(8.0f, std::min(18.0f, row_h - 10.0f));
				const float icy = y + (row_h - isize) * 0.5f;
				if (rtl)
				{
					vgui_draw_row_texture(w, cb, vg, item.icon,
						text_x + std::max(0.0f, text_w) - isize, icy, isize, 0, item.enabled != 0);
					text_w -= isize + 4.0f;
				}
				else
				{
					vgui_draw_row_texture(w, cb, vg, item.icon, text_x, icy, isize, 0, item.enabled != 0);
					text_x += isize + 4.0f;
					text_w -= isize + 4.0f;
				}
			}
			vgui_draw_text_clipped(ctx, cb, vg, w, item.text, text_x, y,
				std::max(0.0f, text_w), row_h, color, 0.0f, 0.5f, clips);
			if (!item.detail.empty())
			{
				uint32_t dc = color;
				if (selected) dc = vgui_alpha_color(dc, 0.75f);
				if (rtl)
					vgui_draw_text_raw(ctx, cb, vg, w, item.detail, ix + 6.0f + buttons_w, y,
						detail_w, row_h, dc, 0.0f, 0.5f);
				else
					vgui_draw_text(ctx, cb, vg, w, item.detail,
						ix + iw - buttons_w - detail_w - 6.0f, y, detail_w, row_h, dc, 0.0f, 0.5f);
			}
			vgui_draw_row_buttons(w, cb, vg, ix, iw, y, row_h, item.id,
				st->hover, st->hover_button, item.button_count, item.button_icons, color);
		}
		if (row_count == 0 && !st->empty_text.empty())
			vgui_draw_text(ctx, cb, vg, w, st->empty_text, ix, iy, iw, ih,
				vgui_widget_color(w, ctx->theme.disabled ? ctx->theme.disabled : ctx->theme.text),
				0.5f, 0.5f);
		vgui_clip_pop(clips, cb, vg);
		cb->restore(vg);
		vgui_draw_scrollbars(w, cb, vg);
		vgui_draw_focus(w, cb, vg);
	}

	static char vgui_ascii_lower(char c)
	{
		return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
	}

	/* 大小写不敏感的首个子串位置（仅折叠 ASCII；UTF-8 多字节安全）。 */
	static size_t vgui_find_ci(const std::string& hay, const std::string& needle)
	{
		if (needle.empty() || needle.size() > hay.size())
			return std::string::npos;
		for (size_t i = 0; i + needle.size() <= hay.size(); ++i)
		{
			size_t j = 0;
			while (j < needle.size() &&
				vgui_ascii_lower(hay[i + j]) == vgui_ascii_lower(needle[j]))
				++j;
			if (j == needle.size())
				return i;
		}
		return std::string::npos;
	}

	/* 树行文本：过滤激活且配置了 match_text 时，把首个命中子串画成
	 * match_color，其余部分保持原色；未命中退回普通绘制。 */
	static void vgui_draw_tree_text(vgui_context* ctx, ovg_ctx_cb* cb, rvg_t* vg,
		vgui_widget* w, vgui_tree_state* st, const std::string& text,
		float x, float y, float width, float height, uint32_t color,
		vgui_clip_state* clips)
	{
		const bool highlight = st->filter_fn && !st->match_text.empty();
		const size_t pos = highlight ? vgui_find_ci(text, st->match_text)
			: std::string::npos;
		if (!highlight || pos == std::string::npos || width <= 0.0f)
		{
			vgui_draw_text_clipped(ctx, cb, vg, w, text, x, y, width, height,
				color, 0.0f, 0.5f, clips);
			return;
		}
		const uint32_t match_color = st->match_color != 0 ? st->match_color
			: 0xFFE6A23C;
		const std::string prefix = text.substr(0, pos);
		const std::string match = text.substr(pos, st->match_text.size());
		const std::string suffix = text.substr(pos + st->match_text.size());
		vgui_draw_text_clipped(ctx, cb, vg, w, prefix, x, y, width, height,
			color, 0.0f, 0.5f, clips);
		const float pre_w = prefix.empty() ? 0.0f : vgui_text_width(w, prefix);
		vgui_draw_text_clipped(ctx, cb, vg, w, match, x + pre_w, y,
			std::max(0.0f, width - pre_w), height, match_color, 0.0f, 0.5f, clips);
		const float match_w = vgui_text_width(w, match);
		if (!suffix.empty())
			vgui_draw_text_clipped(ctx, cb, vg, w, suffix, x + pre_w + match_w, y,
				std::max(0.0f, width - pre_w - match_w), height, color, 0.0f,
				0.5f, clips);
	}

	static void vgui_draw_tree_arrow(vgui_widget* w, ovg_ctx_cb* cb, rvg_t* vg,
		float cx, float cy, float size, bool expanded, uint32_t color, bool flip = false)
	{
		float h = size * 0.5f;
		float s = flip ? -1.0f : 1.0f;
		if (expanded)
		{
			cb->move_to(vg, cx - h * s, cy - h * 0.45f);
			cb->line_to(vg, cx + h * s, cy - h * 0.45f);
			cb->line_to(vg, cx, cy + h * 0.55f);
		}
		else
		{
			cb->move_to(vg, cx - h * 0.55f * s, cy - h);
			cb->line_to(vg, cx + h * 0.55f * s, cy);
			cb->line_to(vg, cx - h * 0.55f * s, cy + h);
		}
		cb->close_path(vg);
		cb->set_source_color(vg, vgui_ovg_color(vgui_widget_color(w, color)));
		cb->fill(vg);
	}

	static void vgui_draw_tree(vgui_widget* w, ovg_ctx_cb* cb, rvg_t* vg, vgui_clip_state* clips)
	{
		vgui_context* ctx = w->ctx;
		vgui_tree_state* st = vgui_tree_of(w);
		if (!st) return;
		vgui_tree_rebuild_rows(w);
		const vgui_tree_style& ts = ctx->style.tree;
		float ix = w->abs_x + w->padding[0];
		float iy = w->abs_y + w->padding[1];
		float iw = std::max(0.0f, w->abs_w - w->padding[0] - w->padding[2]);
		float ih = std::max(0.0f, w->abs_h - w->padding[1] - w->padding[3]);

		float row_h = vgui_tree_row_height(w);
		bool rtl = vgui_is_rtl(ctx);
		uint32_t guide = vgui_alpha_color(
			ctx->theme.tree_guide ? ctx->theme.tree_guide : ctx->theme.panel_border,
			ts.guide_alpha);

		vgui_draw_model_frame(w, cb, vg);
		cb->save(vg);
		vgui_clip_push(clips, cb, vg, ix, iy, iw, ih);
		const int row_count = vgui_tree_row_count(w, st);
		vgui_row_window win = vgui_row_window_of(w, row_count, ih, row_h);
		for (int i = win.first; i < win.last; ++i)
		{
			vgui_item_id row_id = vgui_tree_row_id_at(w, st, i);
			vgui_tree_node* node = vgui_tree_node_find(w, row_id);
			if (!node) continue;
			int row_depth = vgui_tree_row_depth_at(w, st, i);
			float y = iy - w->scroll_y + (float)i * row_h;
			if (y + row_h <= iy || y >= iy + ih)
				continue;
			bool selected = vgui_id_selected(st->selected, node->id);
			if (selected)
				vgui_fill_rect(cb, vg, ix, y, iw, row_h, ts.rounding,
					vgui_widget_color(w, ctx->theme.row_selected ? ctx->theme.row_selected : 0x55409EFF));
			else if (node->enabled && st->hover == node->id)
				vgui_fill_rect(cb, vg, ix, y, iw, row_h, ts.rounding,
					vgui_widget_color(w, ctx->theme.row_hover ? ctx->theme.row_hover : 0x334A4A4A));

			for (int depth = 0; depth < row_depth; ++depth)
			{
				float gx = rtl
					? ix + iw - (float)depth * ts.indent - ts.arrow_size * 0.5f
					: ix + (float)depth * ts.indent + ts.arrow_size * 0.5f;
				vgui_fill_rect(cb, vg, gx, y + 4.0f, 1.0f,
					std::max(0.0f, row_h - 8.0f), 0, vgui_widget_color(w, guide));
			}

			float tx = rtl
				? ix + iw - ts.arrow_gap - (float)row_depth * ts.indent - ts.arrow_size
				: ix + ts.arrow_gap + (float)row_depth * ts.indent;
			float text_x = rtl ? ix + 4.0f : tx + ts.arrow_size + ts.arrow_gap;
			uint32_t color = vgui_row_text_color_of(w, node->text_color, selected, node->enabled != 0);
			if (st->virtual_mode ? vgui_tree_row_has_children(w, st, i) : !node->children.empty())
				vgui_draw_tree_arrow(w, cb, vg, tx + ts.arrow_size * 0.5f,
					y + row_h * 0.5f, ts.arrow_size,
					st->virtual_mode ? vgui_tree_row_expanded(w, st, i) : node->expanded != 0, color, rtl);

			// F04 行尾按钮占用的宽度，文本区避让（RTL 时按钮贴行首）。
			float buttons_w = 0.0f;
			if (node->button_count > 0)
			{
				const float bsize = vgui_row_button_size(w, row_h);
				buttons_w = 4.0f + (float)node->button_count * bsize
					+ (float)(node->button_count - 1) * 4.0f;
			}
			if (rtl)
				text_x += buttons_w;

			// F04 自定义行绘制：替换文本绘制；引导线/箭头与按钮仍由控件管理。
			if (st->virtual_mode && st->source.row_draw)
			{
				st->source.row_draw(st->source.user, i, cb, vg, ix, y, iw, row_h);
			}
			else
			{
				float text_w = rtl
					? std::max(0.0f, tx - ts.arrow_gap - text_x)
					: std::max(0.0f, iw - (text_x - ix) - 4.0f - buttons_w);
				if (node->icon)
				{
					const float isize = std::max(8.0f, std::min(18.0f, row_h - 10.0f));
					const float icy = y + (row_h - isize) * 0.5f;
					if (rtl)
					{
						vgui_draw_row_texture(w, cb, vg, node->icon,
							text_x + text_w - isize, icy, isize, 0, node->enabled != 0);
						text_w -= isize + 4.0f;
					}
					else
					{
						vgui_draw_row_texture(w, cb, vg, node->icon, text_x, icy, isize, 0, node->enabled != 0);
						text_x += isize + 4.0f;
						text_w -= isize + 4.0f;
					}
				}
				vgui_draw_tree_text(ctx, cb, vg, w, st, node->text, text_x, y,
					std::max(0.0f, text_w), row_h, color, clips);
			}
			vgui_draw_row_buttons(w, cb, vg, ix, iw, y, row_h, node->id,
				st->hover, st->hover_button, node->button_count, node->button_icons, color);
		}
		if (row_count == 0 && !st->empty_text.empty())
			vgui_draw_text(ctx, cb, vg, w, st->empty_text, ix, iy, iw, ih,
				vgui_widget_color(w, ctx->theme.disabled ? ctx->theme.disabled : ctx->theme.text),
				0.5f, 0.5f);
		vgui_clip_pop(clips, cb, vg);
		cb->restore(vg);
		vgui_draw_scrollbars(w, cb, vg);
		vgui_draw_focus(w, cb, vg);
	}

	static void vgui_draw_menubar(vgui_widget* w, ovg_ctx_cb* cb, rvg_t* vg, vgui_clip_state* clips)
	{
		vgui_context* ctx = w->ctx;
		vgui_menubar_state* st = vgui_menubar_of(w);
		if (!st || !ctx) return;
		const vgui_menubar_style& ms = ctx->style.menubar;
		uint32_t bg = w->bg;
		uint32_t border = w->border ? w->border : ctx->theme.panel_border;
		uint32_t hover = ctx->theme.menu_hover ? ctx->theme.menu_hover : ctx->theme.button_hover;
		uint32_t text = ctx->theme.row_text ? ctx->theme.row_text : ctx->theme.text;
		if (w->text_color) text = w->text_color;

		if (bg)
			vgui_fill_rect(cb, vg, w->abs_x, w->abs_y, w->abs_w, w->abs_h, w->rounding,
				vgui_widget_color(w, bg));
		if (w->border_width > 0 && border)
		{
			float bi = w->border_width * 0.5f;
			vgui_stroke_rect(cb, vg, w->abs_x + bi, w->abs_y + bi,
				w->abs_w - w->border_width, w->abs_h - w->border_width, w->rounding,
				vgui_widget_color(w, border), w->border_width);
		}

		cb->save(vg);
		vgui_clip_push(clips, cb, vg, w->abs_x, w->abs_y, w->abs_w, w->abs_h);
		for (size_t i = 0; i < st->items.size(); ++i)
		{
			const vgui_menubar_item& item = st->items[i];
			float ix = 0, iy = 0, iw = 0, ih = 0;
			vgui_menubar_item_rect_at(w, i, &ix, &iy, &iw, &ih);
			if (iw <= 0.0f || ih <= 0.0f) continue;
			bool active = st->active == item.id;
			bool hot = item.enabled && st->hover == item.id;
			if (active || hot)
			{
				uint32_t fill = hover;
				if (active)
					fill = vgui_mix_color(hover, ctx->theme.accent, 0.35f);
				vgui_fill_rect(cb, vg, ix, iy, iw, ih, ms.rounding,
					vgui_widget_color(w, fill));
			}
			uint32_t color = item.enabled ? text
				: vgui_mix_color(text, ctx->theme.disabled, ms.disabled_mix);
			vgui_draw_text(ctx, cb, vg, w, item.text,
				ix + ms.item_padding_x, iy,
				std::max(0.0f, iw - ms.item_padding_x * 2.0f), ih,
				vgui_widget_color(w, color), 0.0f, 0.5f);
		}
		vgui_clip_pop(clips, cb, vg);
		cb->restore(vg);
	}

	static void vgui_draw_menu(vgui_widget* w, ovg_ctx_cb* cb, rvg_t* vg, vgui_clip_state* clips)
	{
		vgui_context* ctx = w->ctx;
		vgui_menu_state* st = vgui_menu_of(w);
		if (!st || !st->open) return;
		const vgui_menu_style& ms = ctx->style.menu;
		bool any_check = vgui_menu_has_checkable(w);
		uint32_t bg = w->bg ? w->bg : (ctx->theme.menu_bg ? ctx->theme.menu_bg : ctx->theme.panel_bg);
		uint32_t border = w->border ? w->border
			: (ctx->theme.menu_border ? ctx->theme.menu_border : ctx->theme.button_border);
		uint32_t text = ctx->theme.row_text ? ctx->theme.row_text : ctx->theme.text;
		uint32_t disabled = ctx->theme.menu_disabled_text ? ctx->theme.menu_disabled_text : ctx->theme.disabled;
		uint32_t hover = ctx->theme.menu_hover ? ctx->theme.menu_hover : ctx->theme.button_hover;

		vgui_fill_rect(cb, vg, w->abs_x, w->abs_y, w->abs_w, w->abs_h, w->rounding,
			vgui_widget_color(w, bg));
		if (w->border_width > 0)
		{
			float bi = w->border_width * 0.5f;
			vgui_stroke_rect(cb, vg, w->abs_x + bi, w->abs_y + bi,
				w->abs_w - w->border_width, w->abs_h - w->border_width, w->rounding,
				vgui_widget_color(w, border), w->border_width);
		}

		cb->save(vg);
		vgui_clip_push(clips, cb, vg, w->abs_x, w->abs_y, w->abs_w, w->abs_h);
		float y = w->abs_y + ms.padding_y - w->scroll_y;
		float row_x = w->abs_x + ms.padding_x;
		float row_w = std::max(0.0f, w->abs_w - ms.padding_x * 2.0f);
		for (size_t i = 0; i < st->items.size(); ++i)
		{
			vgui_menu_item& item = st->items[i];
			float h = vgui_menu_item_height(w, item);
			if (y + h <= w->abs_y || y >= w->abs_y + w->abs_h)
			{
				y += h;
				continue;
			}
			if (item.type == VGUI_MENU_ITEM_SEPARATOR)
			{
				vgui_fill_rect(cb, vg, row_x + ms.item_padding_x, y + h * 0.5f,
					std::max(0.0f, row_w - ms.item_padding_x * 2.0f), 1.0f, 0,
					vgui_widget_color(w, border));
				y += h;
				continue;
			}
			bool active = item.enabled && (st->hover == item.id || st->cursor == item.id);
			if (active)
				vgui_fill_rect(cb, vg, row_x, y, row_w, h, std::max(0.0f, w->rounding - 1.0f),
					vgui_widget_color(w, hover));

			uint32_t color = item.enabled ? text : disabled;
			if (!item.enabled && item.enabled == 0)
				color = vgui_mix_color(text, disabled, ms.disabled_mix);
			bool rtl = vgui_is_rtl(ctx);
			float right = w->abs_x + w->abs_w - ms.padding_x - ms.item_padding_x;
			float left = w->abs_x + ms.padding_x + ms.item_padding_x;
			float text_x, text_end;
			float shortcut_x = right;
			float check_x = row_x + ms.item_padding_x + ms.check_width * 0.5f;
			float arrow_cx = right - ms.submenu_arrow_width * 0.35f;
			if (rtl)
			{
				/* Mirrored: check column on the right, text right-aligned before
				 * it, shortcut and submenu arrow on the left. */
				float arrow_span = item.type == VGUI_MENU_ITEM_SUBMENU
					? ms.submenu_arrow_width + ms.item_padding_x : 0.0f;
				float shortcut_span = item.shortcut.empty()
					? 0.0f : (vgui_text_width(w, item.shortcut) + ms.shortcut_gap);
				text_x = left + arrow_span + shortcut_span;
				text_end = right - (any_check ? ms.check_width : 0.0f);
				shortcut_x = left + arrow_span;
				check_x = right - ms.check_width * 0.5f;
				arrow_cx = left + ms.submenu_arrow_width * 0.35f;
			}
			else
			{
				text_x = row_x + ms.item_padding_x + (any_check ? ms.check_width : 0.0f);
				if (item.type == VGUI_MENU_ITEM_SUBMENU)
					shortcut_x -= ms.submenu_arrow_width + ms.item_padding_x;
				text_end = shortcut_x;
				if (!item.shortcut.empty()) text_end -= ms.shortcut_gap;
			}

			if (any_check && item.checkable && item.checked)
			{
				float check = std::max(0.0f, std::min(10.0f, ms.check_width - 4.0f));
				float cx = check_x;
				float cy = y + h * 0.5f;
				float d = check * 0.5f;
				cb->set_line_width(vg, 2.0f);
				cb->set_line_cap(vg, 1);
				cb->move_to(vg, cx - d * 0.8f, cy);
				cb->line_to(vg, cx - d * 0.15f, cy + d * 0.65f);
				cb->line_to(vg, cx + d * 0.8f, cy - d * 0.7f);
				cb->set_source_color(vg, vgui_ovg_color(vgui_widget_color(w,
					item.enabled ? (ctx->theme.checkmark ? ctx->theme.checkmark : 0xFFFFFFFF) : disabled)));
				cb->stroke(vg);
			}

			vgui_draw_text_clipped(ctx, cb, vg, w, item.text, text_x, y,
				std::max(0.0f, text_end - text_x), h, color, 0.0f, 0.5f, clips);
			if (!item.shortcut.empty())
			{
				float sw = vgui_text_width(w, item.shortcut);
				uint32_t sc = item.enabled ? vgui_alpha_color(text, 0.65f)
					: vgui_mix_color(text, disabled, ms.disabled_mix);
				if (rtl)
					vgui_draw_text_raw(ctx, cb, vg, w, item.shortcut, shortcut_x, y,
						sw, h, vgui_widget_color(w, sc), 0.0f, 0.5f);
				else
					vgui_draw_text_raw(ctx, cb, vg, w, item.shortcut, shortcut_x - sw, y,
						sw, h, vgui_widget_color(w, sc), 0.0f, 0.5f);
			}
			if (item.type == VGUI_MENU_ITEM_SUBMENU)
			{
				uint32_t ac = item.enabled ? color : disabled;
				vgui_draw_tree_arrow(w, cb, vg, arrow_cx, y + h * 0.5f,
					ms.submenu_arrow_width * 0.7f, false, ac, rtl);
			}
			y += h;
		}
		vgui_clip_pop(clips, cb, vg);
		cb->restore(vg);
		vgui_draw_scrollbars(w, cb, vg);
	}

	static void vgui_draw_menu_chain(vgui_widget* menu, ovg_ctx_cb* cb, rvg_t* vg, vgui_clip_state* clips)
	{
		vgui_menu_state* st = vgui_menu_of(menu);
		if (!st || !st->open) return;
		vgui_draw_menu(menu, cb, vg, clips);
		if (st->submenu_open && st->submenu_open->menu && st->submenu_open->menu->open)
			vgui_draw_menu_chain(st->submenu_open, cb, vg, clips);
	}

	// F03 拖放覆盖层：插入指示与拖影，画在控件树之上、tooltip 之下。
	static void vgui_draw_drag(vgui_context* ctx, ovg_ctx_cb* cb, rvg_t* vg)
	{
		if (!ctx || ctx->drag_kind == VGUI_DRAG_NONE) return;

		// 提示矩形（接受方通过 hover 回调给出）。
		if (ctx->drop_accepted && ctx->drop_hint.kind != VGUI_DROP_HINT_NONE)
		{
			const vgui_drop_hint& hint = ctx->drop_hint;
			uint32_t accent = ctx->theme.accent;
			if (hint.kind == VGUI_DROP_HINT_RECT)
			{
				vgui_fill_rect(cb, vg, hint.x, hint.y, hint.w, hint.h, 0.0f,
					vgui_alpha_color(accent, 0.12f));
				vgui_stroke_rect(cb, vg, hint.x + 0.5f, hint.y + 0.5f,
					std::max(0.0f, hint.w - 1.0f), std::max(0.0f, hint.h - 1.0f), 0.0f,
					vgui_alpha_color(accent, 0.85f), 1.0f);
			}
			else if (hint.kind == VGUI_DROP_HINT_LINE)
			{
				vgui_fill_rect(cb, vg, hint.x, hint.y,
					std::max(2.0f, hint.w), std::max(2.0f, hint.h), 0.0f, accent);
			}
		}

		// 拖影：指针旁的小标签，样式与 Dock 的浮动标题条一致。
		vgui_widget* anchor = ctx->drop_hover ? ctx->drop_hover : ctx->root;
		std::string label = ctx->drag_text;
		if (label.empty() && ctx->drag_payload.kind == VGUI_DRAG_FILES &&
			!ctx->drag_files.empty())
		{
			label = ctx->drag_files.front();
			const size_t slash = label.find_last_of("/\\");
			if (slash != std::string::npos)
				label = label.substr(slash + 1);
			if (ctx->drag_files.size() > 1)
				label += " (+" + std::to_string(ctx->drag_files.size() - 1) + ")";
		}
		if (label.empty())
			label = ctx->drag_payload.kind == VGUI_DRAG_FILES ? "files" : "item";
		float gw = vgui_clampf(vgui_text_width(anchor, label) + 16.0f, 40.0f, 320.0f);
		float gh = 20.0f;
		float gx = ctx->mouse_x + 12.0f;
		float gy = ctx->mouse_y + 12.0f;
		if (gx + gw > (float)ctx->width) gx = ctx->mouse_x - gw - 6.0f;
		if (gy + gh > (float)ctx->height) gy = ctx->mouse_y - gh - 6.0f;
		vgui_fill_rect(cb, vg, gx, gy, gw, gh, 3.0f, 0xC0202020);
		vgui_stroke_rect(cb, vg, gx + 0.5f, gy + 0.5f, gw - 1.0f, gh - 1.0f, 3.0f,
			ctx->theme.accent, 1.0f);
		vgui_draw_text(ctx, cb, vg, anchor, label, gx + 8.0f, gy, gw - 16.0f, gh,
			ctx->theme.text, 0.0f, 0.5f);
	}

	static void vgui_draw_tooltip(vgui_context* ctx, ovg_ctx_cb* cb, rvg_t* vg)
	{
		if (!vgui_tooltip_is_visible(ctx)) return;
		vgui_tooltip_measure(ctx);
		if (!vgui_tooltip_is_visible(ctx) || ctx->tooltip.w <= 0.0f)
			return;

		const vgui_tooltip_state& t = ctx->tooltip;
		const vgui_tooltip_style& ts = ctx->style.tooltip;
		vgui_color bg = ctx->theme.tooltip_bg
			? ctx->theme.tooltip_bg : ctx->theme.panel_bg;
		vgui_color border = ctx->theme.tooltip_border
			? ctx->theme.tooltip_border : ctx->theme.button_border;
		vgui_color text = ctx->theme.tooltip_text
			? ctx->theme.tooltip_text : ctx->theme.text;
		if (!bg) bg = 0xFF303030;
		if (!text) text = 0xFFFFFFFF;

		cb->save(vg);
		vgui_fill_rect(cb, vg, t.x, t.y, t.w, t.h, ts.rounding, bg);
		if (ts.border_width > 0.0f && border)
		{
			float bi = ts.border_width * 0.5f;
			vgui_stroke_rect(cb, vg, t.x + bi, t.y + bi,
				t.w - ts.border_width, t.h - ts.border_width,
				ts.rounding, border, ts.border_width);
		}

		float pad_x = std::max(0.0f, ts.padding_x);
		float pad_y = std::max(0.0f, ts.padding_y);
		float line_h = vgui_line_height(ctx, t.owner);
		float gap = std::max(0.0f, ts.line_gap);
		float tx = t.x + pad_x;
		float ty = t.y + pad_y;
		float tw = std::max(1.0f, t.w - pad_x * 2.0f);
		for (const std::string& line : t.lines)
		{
			vgui_draw_text(ctx, cb, vg, t.owner, line, tx, ty, tw, line_h,
				text, 0.0f, 0.5f);
			ty += line_h + gap;
		}
		cb->restore(vg);
	}

	static void vgui_draw_widget(vgui_widget* w, ovg_ctx_cb* cb, rvg_t* vg, vgui_clip_state* clips);

	static void vgui_draw_modal(vgui_context* ctx, ovg_ctx_cb* cb, rvg_t* vg,
		vgui_clip_state* clips)
	{
		if (!ctx || !ctx->modal.active || !ctx->modal.stage || !ctx->modal.panel)
			return;
		const vgui_modal_style& ms = ctx->style.modal;
		float alpha = ctx->modal.options.backdrop_alpha >= 0.0f
			? ctx->modal.options.backdrop_alpha : ms.backdrop_alpha;
		if (ctx->modal.options.dim_background && alpha > 0.0f)
		{
			uint32_t color = vgui_alpha_color(ctx->theme.modal_backdrop, alpha);
			vgui_fill_rect(cb, vg, 0.0f, 0.0f,
				vgui_overlay_width(ctx), vgui_overlay_height(ctx), 0.0f, color);
		}
		vgui_draw_widget(ctx->modal.stage, cb, vg, clips);
		while (!clips->stack.empty())
			vgui_clip_pop(clips, cb, vg);
	}

	static uint32_t vgui_toast_level_color(vgui_context* ctx, int level)
	{
		if (!ctx) return 0xFFFFFFFF;
		switch (level)
		{
		case VGUI_TOAST_SUCCESS:
			return ctx->theme.toast_success ? ctx->theme.toast_success : 0xFF4CAF50;
		case VGUI_TOAST_WARNING:
			return ctx->theme.toast_warning ? ctx->theme.toast_warning : 0xFFE6A23C;
		case VGUI_TOAST_ERROR:
			return ctx->theme.toast_error ? ctx->theme.toast_error
				: (ctx->theme.modal_danger ? ctx->theme.modal_danger : 0xFFD9544D);
		case VGUI_TOAST_INFO:
		default:
			return ctx->theme.toast_info ? ctx->theme.toast_info
				: (ctx->theme.accent ? ctx->theme.accent : 0xFF409EFF);
		}
	}

	static void vgui_draw_toasts(vgui_context* ctx, ovg_ctx_cb* cb, rvg_t* vg)
	{
		if (!ctx || ctx->toasts.empty()) return;
		const vgui_toast_style& ts = ctx->style.toast;
		uint32_t bg = ctx->theme.toast_bg ? ctx->theme.toast_bg : ctx->theme.panel_bg;
		uint32_t border = ctx->theme.toast_border
			? ctx->theme.toast_border : ctx->theme.panel_border;
		uint32_t text = ctx->theme.toast_text ? ctx->theme.toast_text : ctx->theme.text;
		float pad_x = std::max(0.0f, ts.padding_x);
		float pad_y = std::max(0.0f, ts.padding_y);
		float line_h = vgui_line_height(ctx, ctx->root);
		float line_gap = std::max(0.0f, ts.line_gap);
		float rounding = std::max(0.0f, ts.rounding);
		int max_visible = std::max(1, ts.max_visible);

		for (size_t i = 0; i < ctx->toasts.size() && (int)i < max_visible; ++i)
		{
			const vgui_toast_record& t = ctx->toasts[i];
			if (t.w <= 0.0f || t.h <= 0.0f) continue;
			vgui_fill_rect(cb, vg, t.x, t.y, t.w, t.h, rounding, bg);
			if (ts.border_width > 0.0f && border)
			{
				float bi = ts.border_width * 0.5f;
				vgui_stroke_rect(cb, vg, t.x + bi, t.y + bi,
					t.w - ts.border_width, t.h - ts.border_width, rounding,
					border, ts.border_width);
			}

			float accent_w = 3.0f;
			uint32_t accent = vgui_toast_level_color(ctx, (int)t.options.level);
			vgui_fill_rect(cb, vg, t.x + 2.0f, t.y + 2.0f, accent_w,
				std::max(0.0f, t.h - 4.0f), std::min(1.5f, accent_w * 0.5f),
				accent);

			float tx = t.x + pad_x + accent_w + 6.0f;
			float tw = std::max(1.0f, t.w - (tx - t.x) - pad_x);
			float ty = t.y + pad_y;
			for (const std::string& line : t.lines)
			{
				vgui_draw_text(ctx, cb, vg, ctx->root, line, tx, ty, tw, line_h,
					text, 0.0f, 0.5f);
				ty += line_h + line_gap;
			}
		}
	}

	static void vgui_draw_splitter(vgui_widget* w, ovg_ctx_cb* cb, rvg_t* vg, vgui_clip_state* clips)
	{
		vgui_context* ctx = w->ctx;
		if (!ctx) return;
		if (w->bg)
			vgui_fill_rect(cb, vg, w->abs_x, w->abs_y, w->abs_w, w->abs_h, w->rounding,
				vgui_widget_color(w, w->bg));
		if (w->border_width > 0 && w->border)
		{
			float bi = w->border_width * 0.5f;
			vgui_stroke_rect(cb, vg, w->abs_x + bi, w->abs_y + bi,
				w->abs_w - w->border_width, w->abs_h - w->border_width, w->rounding,
				vgui_widget_color(w, w->border), w->border_width);
		}

		cb->save(vg);
		if (w->clip_children)
			vgui_clip_push(clips, cb, vg, w->abs_x, w->abs_y, w->abs_w, w->abs_h);
		for (vgui_widget* c : w->children)
			vgui_draw_widget(c, cb, vg, clips);
		if (w->clip_children)
			vgui_clip_pop(clips, cb, vg);
		cb->restore(vg);

		vgui_splitter_state* st = vgui_splitter_of(w);
		if (st && st->sash_w > 0.0f && st->sash_h > 0.0f)
		{
			bool hot = w->hovered || w->pressed;
			uint32_t color = hot ? ctx->theme.accent
				: (w->border ? w->border : ctx->theme.panel_border);
			vgui_fill_rect(cb, vg, st->sash_x, st->sash_y, st->sash_w, st->sash_h, 0.0f,
				vgui_widget_color(w, color));

			uint32_t grip = ctx->theme.checkmark ? ctx->theme.checkmark : ctx->theme.text;
			grip = vgui_widget_color(w, vgui_alpha_color(grip, hot ? 0.85f : 0.45f));
			if (st->sash_w >= 4.0f && st->sash_h >= 12.0f)
			{
				float cx = st->sash_x + st->sash_w * 0.5f;
				float cy = st->sash_y + st->sash_h * 0.5f;
				for (int i = -1; i <= 1; ++i)
					vgui_fill_rect(cb, vg, cx - 1.0f, cy + i * 6.0f - 1.0f, 2.0f, 2.0f, 1.0f, grip);
			}
			else if (st->sash_w >= 12.0f && st->sash_h >= 4.0f)
			{
				float cx = st->sash_x + st->sash_w * 0.5f;
				float cy = st->sash_y + st->sash_h * 0.5f;
				for (int i = -1; i <= 1; ++i)
					vgui_fill_rect(cb, vg, cx + i * 6.0f - 1.0f, cy - 1.0f, 2.0f, 2.0f, 1.0f, grip);
			}
		}
		vgui_draw_focus(w, cb, vg);
	}

	static void vgui_draw_tabview(vgui_widget* w, ovg_ctx_cb* cb, rvg_t* vg, vgui_clip_state* clips)
	{
		vgui_context* ctx = w->ctx;
		vgui_tab_state* st = vgui_tabs_of(w);
		if (!ctx || !st) return;

		if (w->bg)
			vgui_fill_rect(cb, vg, w->abs_x, w->abs_y, w->abs_w, w->abs_h, w->rounding,
				vgui_widget_color(w, w->bg));
		if (w->border_width > 0 && w->border)
		{
			float bi = w->border_width * 0.5f;
			vgui_stroke_rect(cb, vg, w->abs_x + bi, w->abs_y + bi,
				w->abs_w - w->border_width, w->abs_h - w->border_width, w->rounding,
				vgui_widget_color(w, w->border), w->border_width);
		}

		float ix = w->abs_x + w->padding[0];
		float iy = w->abs_y + w->padding[1];
		float iw = std::max(0.0f, w->abs_w - w->padding[0] - w->padding[2]);
		float ih = std::max(0.0f, w->abs_h - w->padding[1] - w->padding[3]);
		float tab_h = std::min(st->tab_height, ih);
		float content_h = std::max(0.0f, ih - tab_h);

		cb->save(vg);
		if (content_h > 0.0f)
		{
			vgui_clip_push(clips, cb, vg, ix, iy + tab_h, iw, content_h);
			for (vgui_widget* c : w->children)
				vgui_draw_widget(c, cb, vg, clips);
			vgui_clip_pop(clips, cb, vg);
		}
		cb->restore(vg);

		uint32_t bar_bg = ctx->theme.button_bg ? ctx->theme.button_bg : ctx->theme.panel_bg;
		uint32_t border = w->border ? w->border : ctx->theme.panel_border;
		vgui_fill_rect(cb, vg, ix, iy, iw, tab_h, 0.0f, vgui_widget_color(w, bar_bg));
		vgui_fill_rect(cb, vg, ix, iy + std::max(0.0f, tab_h - 1.0f), iw, 1.0f, 0.0f,
			vgui_widget_color(w, border));

		cb->save(vg);
		vgui_clip_push(clips, cb, vg, ix, iy, iw, tab_h);
		for (int i = 0; i < (int)st->tabs.size(); ++i)
		{
			const vgui_tab_item& tab = st->tabs[(size_t)i];
			float tx = 0, ty = 0, tw = 0, th = 0;
			if (!vgui_tab_rect(w, i, &tx, &ty, &tw, &th)) continue;
			if (tx + tw < ix || tx > ix + iw) continue;

			bool active = tab.id == st->active;
			bool hover = tab.id == st->hover;
			bool pressed = tab.id == st->pressed;
			bool disabled = !tab.enabled || !w->enabled;
			uint32_t bg = active ? (w->bg ? w->bg : ctx->theme.panel_bg)
				: pressed ? ctx->theme.button_active
				: hover ? ctx->theme.button_hover : ctx->theme.button_bg;
			if (disabled) bg = vgui_mix_color(bg, ctx->theme.disabled, 0.35f);
			vgui_fill_rect(cb, vg, tx + 1.0f, ty + 2.0f, std::max(0.0f, tw - 2.0f),
				std::max(0.0f, th - 3.0f), 3.0f, vgui_widget_color(w, bg));

			if (active)
				vgui_fill_rect(cb, vg, tx + 1.0f, ty + th - 2.0f, std::max(0.0f, tw - 2.0f), 2.0f,
					0.0f, vgui_widget_color(w, ctx->theme.accent));

			bool rtl = vgui_is_rtl(ctx);
			if (tab.unsaved)
			{
				float dot_x = rtl ? tx + tw - st->tab_padding * 0.5f - 4.0f
					: tx + st->tab_padding * 0.5f;
				vgui_fill_rect(cb, vg, dot_x, ty + th * 0.5f - 2.0f,
					4.0f, 4.0f, 2.0f,
					vgui_widget_color(w, disabled ? ctx->theme.disabled : ctx->theme.accent));
			}

			float close_space = tab.closeable ? (st->close_size + 8.0f) : 0.0f;
			float text_x, text_w;
			if (rtl)
			{
				text_x = tx + close_space + st->tab_padding * 0.5f;
				float text_end = tx + tw - st->tab_padding - (tab.unsaved ? 10.0f : 0.0f);
				text_w = std::max(0.0f, text_end - text_x);
			}
			else
			{
				text_x = tx + st->tab_padding;
				if (tab.unsaved) text_x += 10.0f;
				text_w = std::max(0.0f, tx + tw - st->tab_padding * 0.5f - close_space - text_x);
			}
			uint32_t text_color = disabled ? ctx->theme.disabled
				: (active ? ctx->theme.text : (ctx->theme.row_text ? ctx->theme.row_text : ctx->theme.text));
			vgui_draw_text_clipped(ctx, cb, vg, w, tab.title, text_x, ty,
				text_w, th, vgui_widget_color(w, text_color), 0.0f, 0.5f, clips);

			float cx = 0, cy = 0, cw = 0, ch = 0;
			if (vgui_tab_close_rect(w, i, &cx, &cy, &cw, &ch))
			{
				bool close_hot = st->close_hover == tab.id || st->close_pressed == tab.id;
				uint32_t close_color = disabled ? ctx->theme.disabled
					: close_hot ? ctx->theme.accent_hover : ctx->theme.row_text;
				if (close_color == 0) close_color = ctx->theme.text;
				cb->set_line_width(vg, 1.4f);
				cb->set_line_cap(vg, 1);
				float inset = std::max(2.0f, cw * 0.28f);
				cb->move_to(vg, cx + inset, cy + inset);
				cb->line_to(vg, cx + cw - inset, cy + ch - inset);
				cb->move_to(vg, cx + cw - inset, cy + inset);
				cb->line_to(vg, cx + inset, cy + ch - inset);
				cb->set_source_color(vg, vgui_ovg_color(vgui_widget_color(w, close_color)));
				cb->stroke(vg);
			}
		}
		vgui_clip_pop(clips, cb, vg);
		cb->restore(vg);
		vgui_draw_focus(w, cb, vg);
	}

	static void vgui_draw_dock(vgui_widget* w, ovg_ctx_cb* cb, rvg_t* vg, vgui_clip_state* clips)
	{
		vgui_context* ctx = w->ctx;
		vgui_dock_state* st = vgui_dock_of(w);
		if (!ctx || !st) return;
		const vgui_dock_style& ds = ctx->style.dock;

		uint32_t bg = w->bg ? w->bg : (ctx->theme.dock_bg ? ctx->theme.dock_bg : ctx->theme.panel_bg);
		uint32_t border = w->border ? w->border
			: (ctx->theme.dock_border ? ctx->theme.dock_border : ctx->theme.panel_border);
		uint32_t tab_bg = ctx->theme.dock_tab_bg ? ctx->theme.dock_tab_bg : ctx->theme.button_bg;
		uint32_t tab_active = ctx->theme.dock_tab_active
			? ctx->theme.dock_tab_active : ctx->theme.panel_bg;
		uint32_t tab_text = ctx->theme.dock_tab_text ? ctx->theme.dock_tab_text : ctx->theme.text;
		uint32_t float_bg = ctx->theme.dock_floating_bg
			? ctx->theme.dock_floating_bg : ctx->theme.menu_bg;
		uint32_t float_border = ctx->theme.dock_floating_border
			? ctx->theme.dock_floating_border : ctx->theme.menu_border;
		uint32_t float_title = ctx->theme.dock_floating_title
			? ctx->theme.dock_floating_title : ctx->theme.button_bg;

		vgui_fill_rect(cb, vg, w->abs_x, w->abs_y, w->abs_w, w->abs_h, w->rounding,
			vgui_widget_color(w, bg));
		if (w->border_width > 0.0f && border)
		{
			float bi = w->border_width * 0.5f;
			vgui_stroke_rect(cb, vg, w->abs_x + bi, w->abs_y + bi,
				w->abs_w - w->border_width, w->abs_h - w->border_width, w->rounding,
				vgui_widget_color(w, border), w->border_width);
		}

		for (int zone = 0; zone < 4; ++zone)
		{
			if (st->zone_w[zone] <= 0.0f || st->zone_h[zone] <= 0.0f) continue;
			if (!vgui_dock_zone_has_open(st, zone)) continue;
			float zx = st->zone_x[zone], zy = st->zone_y[zone];
			float zw = st->zone_w[zone], zh = st->zone_h[zone];
			float tab_h = std::min(ds.tab_height, zh);
			float content_h = std::max(0.0f, zh - tab_h);

			vgui_fill_rect(cb, vg, zx, zy, zw, zh, 0.0f, vgui_widget_color(w, tab_active));
			vgui_fill_rect(cb, vg, zx, zy, zw, tab_h, 0.0f, vgui_widget_color(w, tab_bg));
			if (tab_h > 0.0f && border)
				vgui_fill_rect(cb, vg, zx, zy + tab_h - 1.0f, zw, 1.0f, 0.0f,
					vgui_widget_color(w, border));

			cb->save(vg);
			vgui_clip_push(clips, cb, vg, zx, zy, zw, tab_h);
			for (const vgui_dock_tab_geom& tab : st->tabs[zone])
			{
				if (tab.x + tab.w < zx || tab.x > zx + zw) continue;
				bool active = tab.id == st->active[zone];
				bool hover = tab.id == st->hover;
				bool pressed = tab.id == st->pressed;
				uint32_t fill = active ? tab_active
					: pressed ? ctx->theme.button_active
					: hover ? ctx->theme.button_hover : tab_bg;
				if (!fill) fill = tab_bg;
				vgui_fill_rect(cb, vg, tab.x + 1.0f, tab.y + 2.0f,
					std::max(0.0f, tab.w - 2.0f), std::max(0.0f, tab.h - 3.0f),
					3.0f, vgui_widget_color(w, fill));
				if (active)
					vgui_fill_rect(cb, vg, tab.x + 1.0f, tab.y + tab.h - 2.0f,
						std::max(0.0f, tab.w - 2.0f), 2.0f, 0.0f,
						vgui_widget_color(w, ctx->theme.accent));

				vgui_dock_panel* panel = vgui_dock_find(w, tab.id);
				if (panel)
				{
					float text_x = tab.x + ds.tab_padding;
					float close_space = tab.close_w > 0.0f ? tab.close_w + ds.tab_padding * 0.5f : 0.0f;
					float text_w = std::max(0.0f, tab.w - ds.tab_padding - close_space);
					uint32_t text_color = active ? tab_text
						: (ctx->theme.row_text ? ctx->theme.row_text : tab_text);
					vgui_draw_text_clipped(ctx, cb, vg, w, panel->title, text_x, tab.y,
						text_w, tab.h, vgui_widget_color(w, text_color), 0.0f, 0.5f, clips);
				}
				if (tab.close_w > 0.0f && tab.close_h > 0.0f)
				{
					bool close_hot = st->close_hover == tab.id || st->close_pressed == tab.id;
					uint32_t close_color = close_hot ? ctx->theme.accent_hover
						: (ctx->theme.row_text ? ctx->theme.row_text : tab_text);
					if (!close_color) close_color = tab_text;
					cb->set_line_width(vg, 1.4f);
					cb->set_line_cap(vg, 1);
					float inset = std::max(2.0f, tab.close_w * 0.28f);
					cb->move_to(vg, tab.close_x + inset, tab.close_y + inset);
					cb->line_to(vg, tab.close_x + tab.close_w - inset,
						tab.close_y + tab.close_h - inset);
					cb->move_to(vg, tab.close_x + tab.close_w - inset, tab.close_y + inset);
					cb->line_to(vg, tab.close_x + inset, tab.close_y + tab.close_h - inset);
					cb->set_source_color(vg, vgui_ovg_color(vgui_widget_color(w, close_color)));
					cb->stroke(vg);
				}
			}
			vgui_clip_pop(clips, cb, vg);
			cb->restore(vg);

			vgui_dock_panel* active = vgui_dock_find(w, st->active[zone]);
			if (active && active->page && content_h > 0.0f)
			{
				cb->save(vg);
				vgui_clip_push(clips, cb, vg, zx, zy + tab_h, zw, content_h);
				vgui_draw_widget(active->page, cb, vg, clips);
				vgui_clip_pop(clips, cb, vg);
				cb->restore(vg);
			}
			if (border)
				vgui_stroke_rect(cb, vg, zx + 0.5f, zy + 0.5f,
					std::max(0.0f, zw - 1.0f), std::max(0.0f, zh - 1.0f),
					0.0f, vgui_widget_color(w, border), 1.0f);
		}

		for (vgui_item_id id : st->float_order)
		{
			vgui_dock_panel* panel = vgui_dock_find(w, id);
			if (!panel || !panel->open || panel->fw <= 0.0f || panel->fh <= 0.0f)
				continue;
			float fx = panel->fx, fy = panel->fy, fw = panel->fw, fh = panel->fh;
			float title_h = std::min(ds.floating_title_height, fh);
			float rounding = ds.floating_rounding;
			if (ds.shadow > 0.0f)
				vgui_fill_rect(cb, vg, fx + ds.shadow, fy + ds.shadow, fw, fh, rounding,
					0x50000000);
			vgui_fill_rect(cb, vg, fx, fy, fw, fh, rounding,
				vgui_widget_color(w, float_bg));
			vgui_fill_rect(cb, vg, fx, fy, fw, title_h, rounding,
				vgui_widget_color(w, float_title));
			if (ds.floating_border_width > 0.0f && float_border)
			{
				float bi = ds.floating_border_width * 0.5f;
				vgui_stroke_rect(cb, vg, fx + bi, fy + bi, fw - ds.floating_border_width,
					fh - ds.floating_border_width, rounding,
					vgui_widget_color(w, float_border), ds.floating_border_width);
			}

			float text_x = fx + ds.tab_padding;
			float close_space = panel->closeable ? ds.close_size + ds.tab_padding : 0.0f;
			float text_w = std::max(0.0f, fw - ds.tab_padding * 2.0f - close_space);
			vgui_draw_text_clipped(ctx, cb, vg, w, panel->title, text_x, fy, text_w,
				title_h, vgui_widget_color(w, tab_text), 0.0f, 0.5f, clips);

			float cx = 0, cy = 0, cw = 0, ch = 0;
			if (vgui_dock_floating_close_rect(w, *panel, &cx, &cy, &cw, &ch))
			{
				bool close_hot = st->close_hover == id || st->close_pressed == id;
				uint32_t close_color = close_hot ? ctx->theme.accent_hover
					: (ctx->theme.row_text ? ctx->theme.row_text : tab_text);
				if (!close_color) close_color = tab_text;
				cb->set_line_width(vg, 1.4f);
				cb->set_line_cap(vg, 1);
				float inset = std::max(2.0f, cw * 0.28f);
				cb->move_to(vg, cx + inset, cy + inset);
				cb->line_to(vg, cx + cw - inset, cy + ch - inset);
				cb->move_to(vg, cx + cw - inset, cy + inset);
				cb->line_to(vg, cx + inset, cy + ch - inset);
				cb->set_source_color(vg, vgui_ovg_color(vgui_widget_color(w, close_color)));
				cb->stroke(vg);
			}

			float content_y = fy + title_h;
			float content_h = std::max(0.0f, fh - title_h);
			if (panel->page && content_h > 0.0f)
			{
				cb->save(vg);
				vgui_clip_push(clips, cb, vg, fx, content_y, fw, content_h);
				vgui_draw_widget(panel->page, cb, vg, clips);
				vgui_clip_pop(clips, cb, vg);
				cb->restore(vg);
			}
		}

		if (st->drag_mode == 2)
		{
			if (st->drop_zone >= 0 && st->drop_zone < 4 &&
				st->zone_w[st->drop_zone] > 0.0f && st->zone_h[st->drop_zone] > 0.0f)
			{
				int zone = st->drop_zone;
				float zx = st->zone_x[zone], zy = st->zone_y[zone];
				float zw = st->zone_w[zone], zh = st->zone_h[zone];
				vgui_fill_rect(cb, vg, zx, zy, zw, zh, 0.0f,
					vgui_alpha_color(ctx->theme.accent, 0.12f));
				vgui_stroke_rect(cb, vg, zx + 0.5f, zy + 0.5f,
					std::max(0.0f, zw - 1.0f), std::max(0.0f, zh - 1.0f), 0.0f,
					vgui_alpha_color(ctx->theme.accent, 0.85f), 1.0f);
				float bar_h = std::min(ds.tab_height, zh);
				float line_x = zx;
				if (st->drop_index >= 0 && st->drop_index < (int)st->tabs[zone].size())
					line_x = st->tabs[zone][(size_t)st->drop_index].x;
				else if (!st->tabs[zone].empty())
					line_x = st->tabs[zone].back().x + st->tabs[zone].back().w;
				line_x = vgui_clampf(line_x, zx, zx + zw - 2.0f);
				vgui_fill_rect(cb, vg, line_x, zy, 2.0f, bar_h, 0.0f, ctx->theme.accent);
			}

			vgui_dock_panel* drag = vgui_dock_find(w, st->drag_panel);
			if (drag)
			{
				float gx = st->drag_x + 10.0f;
				float gy = st->drag_y + 10.0f;
				float gw = vgui_clampf(vgui_text_width(w, drag->title) + ds.tab_padding * 2.0f,
					ds.tab_min_width, ds.tab_max_width);
				float gh = std::max(18.0f, ds.tab_height);
				vgui_fill_rect(cb, vg, gx, gy, gw, gh, 3.0f, 0xC0202020);
				vgui_stroke_rect(cb, vg, gx + 0.5f, gy + 0.5f, gw - 1.0f, gh - 1.0f, 3.0f,
					ctx->theme.accent, 1.0f);
				vgui_draw_text(ctx, cb, vg, w, drag->title, gx + ds.tab_padding, gy,
					gw - ds.tab_padding * 2.0f, gh, ctx->theme.text, 0.0f, 0.5f);
			}
		}

		vgui_draw_focus(w, cb, vg);
	}

	static void vgui_draw_widget(vgui_widget* w, ovg_ctx_cb* cb, rvg_t* vg, vgui_clip_state* clips)
	{
		if (!w || !w->visible || w->abs_w <= 0 || w->abs_h <= 0) return;
		// 区域外直接不渲染
		if (w->abs_x + w->abs_w < 0 || w->abs_y + w->abs_h < 0 || (vg->width > 0 && w->abs_x > vg->width) || (vg->height > 0 && w->abs_y > vg->height))
			return;
		vgui_context* ctx = w->ctx;

		if (w->kind == VGUI_SPLITTER)
		{
			vgui_draw_splitter(w, cb, vg, clips);
			return;
		}
		if (w->kind == VGUI_TABVIEW)
		{
			vgui_draw_tabview(w, cb, vg, clips);
			return;
		}
		if (w->kind == VGUI_DOCK)
		{
			vgui_draw_dock(w, cb, vg, clips);
			return;
		}

		if (w->kind == VGUI_PANEL)
		{
			if (w->bg)
				vgui_fill_rect(cb, vg, w->abs_x, w->abs_y, w->abs_w, w->abs_h, w->rounding, vgui_widget_color(w, w->bg));
			if (w->border_width > 0 && w->border)
			{
				float bi = w->border_width * 0.5f;
				vgui_stroke_rect(cb, vg, w->abs_x + bi, w->abs_y + bi,
					w->abs_w - w->border_width, w->abs_h - w->border_width, w->rounding,
					vgui_widget_color(w, w->border), w->border_width);
			}

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
			vgui_fill_rect(cb, vg, w->abs_x, w->abs_y, w->abs_w,
				std::max(ctx->style.separator.thickness, w->abs_h), 0,
				vgui_widget_color(w, w->bg ? w->bg : ctx->theme.panel_border));
			break;
		case VGUI_LIST: vgui_draw_list(w, cb, vg, clips); break;
		case VGUI_TREE: vgui_draw_tree(w, cb, vg, clips); break;
		case VGUI_MENU: vgui_draw_menu(w, cb, vg, clips); break;
		case VGUI_MENUBAR: vgui_draw_menubar(w, cb, vg, clips); break;
		case VGUI_IMAGE: vgui_draw_image(w, cb, vg, clips); break;
		case VGUI_CUSTOM:
		{
			// 1) 复用内建背景/边框/圆角 —— 用户省掉重复写
			if (w->bg)
				vgui_fill_rect(cb, vg, w->abs_x, w->abs_y, w->abs_w, w->abs_h, w->rounding,
					vgui_widget_color(w, w->bg));
			if (w->border_width > 0 && w->border)
			{
				float bi = w->border_width * 0.5f;
				vgui_stroke_rect(cb, vg, w->abs_x + bi, w->abs_y + bi,
					w->abs_w - w->border_width, w->abs_h - w->border_width, w->rounding,
					vgui_widget_color(w, w->border), w->border_width);
			}
			// 焦点环画在自定义内容之下（先画焦点环，再画用户内容）
			vgui_draw_focus(w, cb, vg);

			// 2) save + 可选 clip —— custom_clip_content=0 时允许超出控件边界
			cb->save(vg);
			if (w->custom_clip_content)
				vgui_clip_push(clips, cb, vg, w->abs_x, w->abs_y, w->abs_w, w->abs_h);

			// 3) 用户回调：直接拿到 ovg_ctx_cb* 和 rvg_t*
			if (w->custom_draw)
				w->custom_draw(w, cb, vg,
					w->abs_x, w->abs_y, w->abs_w, w->abs_h,
					w->custom_dt, w->custom_draw_user);

			// 4) 还原（clip 和 save 必须成对）
			if (w->custom_clip_content)
				vgui_clip_pop(clips, cb, vg);
			cb->restore(vg);
			break;
		}
		default: break;
		}
		(void)ctx;
	}

	static void vgui_menu_tick(vgui_widget* menu, vgui_context* ctx)
	{
		vgui_menu_state* st = vgui_menu_of(menu);
		if (!st || !st->open) return;
		if (st->pending_submenu_item && ctx->time - st->pending_submenu_time >= ctx->style.menu.submenu_delay)
		{
			vgui_item_id item = st->pending_submenu_item;
			st->pending_submenu_item = 0;
			vgui_menu_open_submenu(menu, item);
			if (!ctx->alive.count(menu)) return;
			st = vgui_menu_of(menu);
			if (!st) return;
		}
		if (st->submenu_open && st->submenu_open->menu && st->submenu_open->menu->open)
			vgui_menu_tick(st->submenu_open, ctx);
	}

	static void vgui_update_impl(vgui_context* ctx, float dt)
	{
		if (!ctx) return;
		if (dt < 0) dt = 0;
		ctx->time += dt;
		vgui_flush_pending(ctx);
		vgui_ensure_layout(ctx);
		if (ctx->open_menu) vgui_menu_tick(ctx->open_menu, ctx);
		vgui_tooltip_update(ctx, dt);

		for (size_t i = 0; i < ctx->toasts.size();)
		{
			vgui_toast_record& toast = ctx->toasts[i];
			if (!toast.sticky)
			{
				toast.time_left -= dt;
				if (toast.time_left <= 0.0f)
				{
					ctx->toasts.erase(ctx->toasts.begin() + (std::ptrdiff_t)i);
					ctx->layout_dirty = true;
					continue;
				}
			}
			++i;
		}

		// speed 0 means "no tween": snap to the target state instead of freezing
		// any animation that happened to be in flight when the style changed.
		float speed = ctx->style.animation_speed;
		float k = speed > 0.0f
			? 1.0f - std::exp(-std::max(0.0f, dt) * speed)
			: 1.0f;
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
			if (w->kind == VGUI_CUSTOM)
				w->custom_dt = dt;
		}
	}

	static void vgui_process_texture_retire(vgui_context* ctx, ovg_ctx_cb* cb, rvg_t* vg)
	{
		if (!ctx) return;

		// These textures were handed to image_destroy on the previous draw.
		// The renderer has consumed their descriptors by now.
		for (vgui_texture* texture : ctx->retired_textures)
			delete texture;
		ctx->retired_textures.clear();

		if (ctx->pending_texture_destroy.empty())
			return;

		std::vector<vgui_texture*> pending;
		pending.swap(ctx->pending_texture_destroy);
		for (vgui_texture* texture : pending)
		{
			if (!texture) continue;
			// Native textures keep their handle alive until this point, so the
			// previous frame's draw data can still read vg_image_t::id.  Release
			// the backend wrapper now and delete the vgui_texture; the backend
			// defers sampler/texture destruction until the GPU is idle.
			if (texture->release_native)
			{
				texture->release_native(texture);
				texture->release_native = nullptr;
				delete texture;
				continue;
			}
			if (texture->image.id != 0 && cb && vg && cb->image_destroy)
			{
				cb->image_destroy(vg, &texture->image);
				ctx->retired_textures.push_back(texture);
			}
			else
			{
				delete texture;
			}
		}
	}

} // anonymous namespace

// Clip helpers for the public VGUI_CUSTOM drawing helpers.  save/restore does
// not cover the scissor rectangle, so callers capture and restore it explicitly
// and intersect the requested rect with the active clip instead of replacing it.
static void vgui_custom_clip_begin(ovg_ctx_cb* cb, rvg_t* vg, float x, float y, float w, float h, glm::ivec4* saved, int* has_saved)
{
	*has_saved = 0;
	saved->x = saved->y = saved->z = saved->w = 0;
	if (!cb || !vg || !cb->clip_rect) return;
	if (cb->get_clip_rect)
	{
		cb->get_clip_rect(vg, saved);
		*has_saved = 1;
	}
	int x0 = (int)std::floor(x);
	int y0 = (int)std::floor(y);
	int x1 = (int)std::ceil(x + w);
	int y1 = (int)std::ceil(y + h);
	if (*has_saved && saved->z > 0 && saved->w > 0)
	{
		x0 = std::max(x0, saved->x);
		y0 = std::max(y0, saved->y);
		x1 = std::min(x1, saved->x + saved->z);
		y1 = std::min(y1, saved->y + saved->w);
	}
	cb->clip_rect(vg, x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0));
}

static void vgui_custom_clip_end(ovg_ctx_cb* cb, rvg_t* vg, const glm::ivec4& saved, int has_saved)
{
	if (!cb || !vg) return;
	if (has_saved && cb->set_clip_rect)
		cb->set_clip_rect(vg, const_cast<glm::ivec4*>(&saved));
	else if (cb->clip_rect)
		cb->clip_rect(vg, 0, 0, -1, -1);
}

//----------------------------------------------------------------------------
// Public C API
//----------------------------------------------------------------------------

extern "C" {

	const char* vgui_version(void)
	{
		return "0.3.0";
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
		ctx->style = vgui_default_style();

		ctx->root = vgui_new_widget(ctx, VGUI_PANEL, nullptr);
		ctx->root->bg = 0;
		ctx->root->border = 0;
		ctx->root->border_width = 0;
		ctx->root->rounding = 0;
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
		ctx->modal.active = 0;
		ctx->modal.stage = nullptr;
		ctx->modal.panel = nullptr;
		ctx->modal.restore_focus = nullptr;
		ctx->dialog = vgui_dialog_state();
		ctx->toasts.clear();
		if (ctx->root && ctx->alive.count(ctx->root))
			vgui_delete_rec(ctx->root, ctx);
		ctx->root = nullptr;

		// Delete widgets that were not attached to the root tree as well.  Take a
		// snapshot: deleting a popup menu can also delete its owned submenus.
		std::vector<vgui_widget*> remaining(ctx->all_widgets.begin(), ctx->all_widgets.end());
		for (vgui_widget* w : remaining)
		{
			if (w && ctx->alive.count(w))
			{
				ctx->alive.erase(w);
				delete w;
			}
		}
		ctx->all_widgets.clear();
		ctx->alive.clear();

		// Release textures that were never attached to an image or whose
		// GPU-destroy descriptor was still queued.  Widget deletion above has
		// already released the image references.
		std::unordered_set<vgui_texture*> textures = ctx->textures;
		for (vgui_texture* texture : ctx->pending_texture_destroy)
			if (texture) textures.insert(texture);
		for (vgui_texture* texture : ctx->retired_textures)
			if (texture) textures.insert(texture);
		for (vgui_texture* texture : textures)
		{
			if (!texture) continue;
			if (texture->release_native)
			{
				texture->release_native(texture);
				texture->release_native = nullptr;
			}
			delete texture;
		}
		ctx->textures.clear();
		ctx->pending_texture_destroy.clear();
		ctx->retired_textures.clear();
		if (ctx->flex)
		{
			free_flex_run(ctx->flex);
			ctx->flex = nullptr;
		}
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
		ctx->text_width_cache.clear();
		ctx->glyph_advance_cache.clear();
		ctx->font_metrics_cache.clear();
		ctx->layout_dirty = true;
	}

	void vgui_context_set_theme(vgui_context* ctx, const vgui_theme* theme)
	{
		if (!ctx || !theme) return;
		vgui_theme old = ctx->theme;
		vgui_theme next = *theme;
		vgui_theme def = vgui_default_theme();
		// Backwards compatibility for themes written before the three new
		// colours existed.  An unset hover colour keeps the thumb unchanged;
		// unset foreground colours fall back to the default (white).
		if (!next.scrollbar_hover)
			next.scrollbar_hover = next.scrollbar ? next.scrollbar : def.scrollbar_hover;
		if (!next.checkmark) next.checkmark = def.checkmark;
		if (!next.switch_thumb) next.switch_thumb = def.switch_thumb;

		// Model-control colours were appended to vgui_theme.  Old theme structs
		// therefore arrive with these fields zeroed; derive sensible values from
		// the core palette instead of rendering transparent text.
		if (!next.list_bg) next.list_bg = next.panel_bg ? next.panel_bg : def.list_bg;
		if (!next.list_border) next.list_border = next.panel_border ? next.panel_border : def.list_border;
		if (!next.row_text) next.row_text = next.text ? next.text : def.row_text;
		if (!next.row_hover) next.row_hover = next.button_hover ? next.button_hover : def.row_hover;
		if (!next.row_selected) next.row_selected = def.row_selected;
		if (!next.row_selected_text) next.row_selected_text = next.text ? next.text : def.row_selected_text;
		if (!next.tree_guide)
			next.tree_guide = vgui_alpha_color(next.panel_border ? next.panel_border : def.tree_guide, 0.45f);
		if (!next.menu_bg) next.menu_bg = next.panel_bg ? next.panel_bg : def.menu_bg;
		if (!next.menu_border) next.menu_border = next.button_border ? next.button_border : def.menu_border;
		if (!next.menu_hover) next.menu_hover = next.button_hover ? next.button_hover : def.menu_hover;
		if (!next.menu_disabled_text) next.menu_disabled_text = next.disabled ? next.disabled : def.menu_disabled_text;
		if (!next.tooltip_bg) next.tooltip_bg = next.menu_bg ? next.menu_bg : def.tooltip_bg;
		if (!next.tooltip_border) next.tooltip_border = next.menu_border ? next.menu_border : def.tooltip_border;
		if (!next.tooltip_text) next.tooltip_text = next.text ? next.text : def.tooltip_text;
		// Modal and toast colours were appended after tooltips.  Derive their
		// fallbacks from the existing palette so old themes keep working.
		if (!next.modal_backdrop) next.modal_backdrop = def.modal_backdrop;
		if (!next.modal_bg) next.modal_bg = next.panel_bg ? next.panel_bg : def.modal_bg;
		if (!next.modal_border) next.modal_border = next.button_border ? next.button_border : def.modal_border;
		if (!next.modal_danger) next.modal_danger = def.modal_danger;
		if (!next.toast_bg) next.toast_bg = next.menu_bg ? next.menu_bg : def.toast_bg;
		if (!next.toast_border) next.toast_border = next.menu_border ? next.menu_border : def.toast_border;
		if (!next.toast_text) next.toast_text = next.text ? next.text : def.toast_text;
		if (!next.toast_info) next.toast_info = next.accent ? next.accent : def.toast_info;
		if (!next.toast_success) next.toast_success = def.toast_success;
		if (!next.toast_warning) next.toast_warning = def.toast_warning;
		if (!next.toast_error) next.toast_error = next.modal_danger ? next.modal_danger : def.toast_error;
		// Dock colours were appended after the overlay controls.  Derive them
		// from the core palette so a pre-dock theme still renders correctly.
		if (!next.dock_bg) next.dock_bg = next.panel_bg ? next.panel_bg : def.dock_bg;
		if (!next.dock_border) next.dock_border = next.panel_border ? next.panel_border : def.dock_border;
		if (!next.dock_tab_bg) next.dock_tab_bg = next.button_bg ? next.button_bg : def.dock_tab_bg;
		if (!next.dock_tab_active) next.dock_tab_active = next.panel_bg ? next.panel_bg : def.dock_tab_active;
		if (!next.dock_tab_text) next.dock_tab_text = next.text ? next.text : def.dock_tab_text;
		if (!next.dock_floating_bg) next.dock_floating_bg = next.menu_bg ? next.menu_bg : def.dock_floating_bg;
		if (!next.dock_floating_border)
			next.dock_floating_border = next.menu_border ? next.menu_border : def.dock_floating_border;
		if (!next.dock_floating_title)
			next.dock_floating_title = next.button_bg ? next.button_bg : def.dock_floating_title;
		ctx->theme = next;

		// Widgets copy their defaults when they are created.  Move the colors that
		// still match the old defaults over to the new theme so a theme switch is
		// visible without rebuilding the tree.
		for (vgui_widget* w : ctx->all_widgets)
		{
			if (!w || !ctx->alive.count(w)) continue;
			uint32_t old_bg = w->bg;
			uint32_t old_border = w->border;
			if (w->text_color == old.text) w->text_color = next.text;
			if (w->accent == old.accent) w->accent = next.accent;
			if (w->track == old.track) w->track = next.track;
			if (w->kind == VGUI_LIST || w->kind == VGUI_TREE)
			{
				if (old_bg == old.list_bg || old_bg == old.panel_bg) w->bg = next.list_bg;
				if (old_border == old.list_border || old_border == old.panel_border) w->border = next.list_border;
				continue;
			}
			if (w->kind == VGUI_DOCK)
			{
				if (old_bg == old.dock_bg || old_bg == old.panel_bg) w->bg = next.dock_bg;
				if (old_border == old.dock_border || old_border == old.panel_border)
					w->border = next.dock_border;
				continue;
			}
			if (w->kind == VGUI_MENU)
			{
				if (old_bg == old.menu_bg || old_bg == old.panel_bg) w->bg = next.menu_bg;
				if (old_border == old.menu_border || old_border == old.button_border) w->border = next.menu_border;
				continue;
			}
			if (old_bg == old.panel_bg) w->bg = next.panel_bg;
			else if (old_bg == old.button_bg) w->bg = next.button_bg;
			else if (old_bg == old.edit_bg) w->bg = next.edit_bg;
			if (old_border == old.panel_border) w->border = next.panel_border;
			else if (old_border == old.button_border) w->border = next.button_border;
		}
	}

	void vgui_context_get_theme(vgui_context* ctx, vgui_theme* theme)
	{
		if (theme) *theme = ctx ? ctx->theme : vgui_theme();
	}

	void vgui_style_default(vgui_style* style)
	{
		if (style) *style = vgui_default_style();
	}

	void vgui_context_set_style(vgui_context* ctx, const vgui_style* style)
	{
		if (!ctx || !style) return;
		vgui_style old = ctx->style;
		vgui_style next = *style;
		vgui_sanitize_style(&next);
		ctx->style = next;

		auto move = [](float& value, float old_value, float new_value)
			{
				if (value == old_value) value = new_value;
			};
		for (vgui_widget* w : ctx->all_widgets)
		{
			if (!w || !ctx->alive.count(w) || w == ctx->root) continue;
			switch (w->kind)
			{
			case VGUI_PANEL:
				if (w->padding[0] == old.panel.padding && w->padding[1] == old.panel.padding &&
					w->padding[2] == old.panel.padding && w->padding[3] == old.panel.padding)
					w->padding[0] = w->padding[1] = w->padding[2] = w->padding[3] = next.panel.padding;
				move(w->border_width, old.panel.border_width, next.panel.border_width);
				move(w->rounding, old.panel.rounding, next.panel.rounding);
				break;
			case VGUI_LABEL:
				move(w->padding[0], old.label.padding_x, next.label.padding_x);
				move(w->padding[2], old.label.padding_x, next.label.padding_x);
				break;
			case VGUI_BUTTON:
				move(w->padding[0], old.button.padding_x, next.button.padding_x);
				move(w->padding[2], old.button.padding_x, next.button.padding_x);
				move(w->padding[1], old.button.padding_y, next.button.padding_y);
				move(w->padding[3], old.button.padding_y, next.button.padding_y);
				move(w->border_width, old.button.border_width, next.button.border_width);
				move(w->rounding, old.button.rounding, next.button.rounding);
				break;
			case VGUI_SLIDER:
				move(w->req_w, old.slider.req_width, next.slider.req_width);
				move(w->req_h, old.slider.min_height, next.slider.min_height);
				break;
			case VGUI_PROGRESS:
				move(w->req_w, old.progress.req_width, next.progress.req_width);
				move(w->req_h, old.progress.min_height, next.progress.min_height);
				break;
			case VGUI_EDIT:
				move(w->padding[0], old.edit.padding_x, next.edit.padding_x);
				move(w->padding[2], old.edit.padding_x, next.edit.padding_x);
				move(w->padding[1], old.edit.padding_y, next.edit.padding_y);
				move(w->padding[3], old.edit.padding_y, next.edit.padding_y);
				move(w->border_width, old.edit.border_width, next.edit.border_width);
				move(w->rounding, old.edit.rounding, next.edit.rounding);
				move(w->req_w, old.edit.req_width, next.edit.req_width);
				move(w->req_h, old.edit.req_height, next.edit.req_height);
				move(w->sel_rounding, old.edit.selection_rounding, next.edit.selection_rounding);
				move(w->caret_width, old.edit.caret_width, next.edit.caret_width);
				break;
			case VGUI_SEPARATOR:
				move(w->req_h, old.separator.thickness, next.separator.thickness);
				break;
			case VGUI_LIST:
				move(w->padding[0], old.list.padding_x, next.list.padding_x);
				move(w->padding[2], old.list.padding_x, next.list.padding_x);
				move(w->padding[1], old.list.padding_y, next.list.padding_y);
				move(w->padding[3], old.list.padding_y, next.list.padding_y);
				move(w->rounding, old.list.rounding, next.list.rounding);
				move(w->req_w, old.list.req_width, next.list.req_width);
				move(w->req_h, old.list.req_height, next.list.req_height);
				break;
			case VGUI_TREE:
				move(w->padding[0], old.tree.padding_x, next.tree.padding_x);
				move(w->padding[2], old.tree.padding_x, next.tree.padding_x);
				move(w->padding[1], old.tree.padding_y, next.tree.padding_y);
				move(w->padding[3], old.tree.padding_y, next.tree.padding_y);
				move(w->rounding, old.tree.rounding, next.tree.rounding);
				move(w->req_w, old.tree.req_width, next.tree.req_width);
				move(w->req_h, old.tree.req_height, next.tree.req_height);
				break;
			case VGUI_MENU:
				move(w->rounding, old.menu.rounding, next.menu.rounding);
				break;
			case VGUI_MENUBAR:
				move(w->rounding, old.menubar.rounding, next.menubar.rounding);
				break;
			case VGUI_IMAGE:
				move(w->border_width, old.image.border_width, next.image.border_width);
				move(w->rounding, old.image.rounding, next.image.rounding);
				move(w->req_w, old.image.req_width, next.image.req_width);
				move(w->req_h, old.image.req_height, next.image.req_height);
				break;
			default:
				break;
			}
		}
		ctx->layout_dirty = true;
	}

	void vgui_context_get_style(vgui_context* ctx, vgui_style* style)
	{
		if (style) *style = ctx ? ctx->style : vgui_default_style();
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

	void vgui_context_set_direction(vgui_context* ctx, vgui_direction direction)
	{
		if (!ctx) return;
		vgui_direction dir = direction == VGUI_DIRECTION_RTL ? VGUI_DIRECTION_RTL : VGUI_DIRECTION_LTR;
		if (ctx->direction == dir) return;
		ctx->direction = dir;
		ctx->layout_dirty = true;
	}

	vgui_direction vgui_context_get_direction(vgui_context* ctx)
	{
		return ctx ? ctx->direction : VGUI_DIRECTION_LTR;
	}

	void vgui_context_set_string(vgui_context* ctx, const char* language, const char* key, const char* text)
	{
		if (!ctx || !language || !*language || !key || !*key) return;
		ctx->strings[language][key] = text ? text : "";
		if (ctx->language == language)
			vgui_i18n_refresh(ctx);
	}

	void vgui_context_set_language(vgui_context* ctx, const char* language)
	{
		if (!ctx) return;
		std::string lang = language ? language : "";
		if (ctx->language == lang) return;
		ctx->language = std::move(lang);
		vgui_i18n_refresh(ctx);
	}

	const char* vgui_context_get_language(vgui_context* ctx)
	{
		return ctx ? ctx->language.c_str() : "";
	}

	const char* vgui_context_translate(vgui_context* ctx, const char* key)
	{
		if (!ctx || !key) return key ? key : "";
		return vgui_translate(ctx, key);
	}

	//----------------------------------------------------------------------------
	// textures
	//----------------------------------------------------------------------------

	vgui_texture* vgui_texture_create(vgui_context* ctx, const vgui_texture_desc* desc)
	{
		if (!ctx || !vgui_texture_desc_valid(desc))
			return nullptr;
		vgui_texture* texture = new vgui_texture();
		texture->ctx = ctx;
		if (!vgui_texture_store_desc(texture, desc))
		{
			delete texture;
			return nullptr;
		}
		ctx->textures.insert(texture);
		return texture;
	}

	int vgui_texture_update(vgui_texture* texture, const vgui_texture_desc* desc)
	{
		if (!texture || !texture->cpu || !vgui_texture_store_desc(texture, desc))
			return 0;
		vgui_texture_mark_layout_dirty(texture);
		return 1;
	}

	void vgui_texture_destroy(vgui_texture* texture)
	{
		vgui_texture_release(texture);
	}

	int vgui_texture_get_width(const vgui_texture* texture)
	{
		return texture ? texture->width : 0;
	}

	int vgui_texture_get_height(const vgui_texture* texture)
	{
		return texture ? texture->height : 0;
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
		if (child->kind == VGUI_MENU) return;	// popups are context-level, not layout children

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
		if (panel->kind == VGUI_LIST)
		{
			vgui_list_clear(panel);
			ctx->layout_dirty = true;
			if (!ctx->in_input) vgui_flush_pending(ctx);
			return;
		}
		if (panel->kind == VGUI_TREE)
		{
			vgui_tree_clear(panel);
			ctx->layout_dirty = true;
			if (!ctx->in_input) vgui_flush_pending(ctx);
			return;
		}
		if (panel->kind == VGUI_MENU)
		{
			vgui_menu_clear(panel);
			ctx->layout_dirty = true;
			if (!ctx->in_input) vgui_flush_pending(ctx);
			return;
		}
		if (panel->kind == VGUI_TABVIEW)
		{
			vgui_tab_clear(panel);
			ctx->layout_dirty = true;
			if (!ctx->in_input) vgui_flush_pending(ctx);
			return;
		}
		if (panel->kind == VGUI_DOCK)
		{
			vgui_dock_clear(panel);
			ctx->layout_dirty = true;
			if (!ctx->in_input) vgui_flush_pending(ctx);
			return;
		}
		if (panel->kind == VGUI_MENUBAR)
		{
			vgui_menubar_clear(panel);
			ctx->layout_dirty = true;
			if (!ctx->in_input) vgui_flush_pending(ctx);
			return;
		}
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
		if (!visible && widget->kind == VGUI_MENU)
		{
			vgui_menu_close_internal(widget);
			if (!widget->ctx || !widget->ctx->alive.count(widget)) return;
		}
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
		widget->text_key.clear();
		widget->text = text ? text : "";
		if (widget->kind == VGUI_EDIT)
		{
			widget->edit_text = widget->text;
			widget->edit_cursor = widget->edit_anchor = (int)widget->edit_text.size();
			widget->ime_text.clear();
			widget->ime_cursor = widget->ime_length = 0;
			vgui_edit_invalidate_content_width(widget);
		}
		vgui_mark_dirty(widget);
	}

	const char* vgui_widget_get_text(vgui_widget* widget)
	{
		if (!widget) return "";
		return widget->kind == VGUI_EDIT ? widget->edit_text.c_str() : widget->text.c_str();
	}

	void vgui_widget_set_text_key(vgui_widget* widget, const char* key)
	{
		if (!widget) return;
		if (!key || !*key)
		{
			widget->text_key.clear();
			return;
		}
		widget->text_key = key;
		widget->text = vgui_translate(widget->ctx, key);
		if (widget->kind == VGUI_EDIT)
		{
			widget->edit_text = widget->text;
			widget->edit_cursor = widget->edit_anchor = (int)widget->edit_text.size();
			widget->ime_text.clear();
			widget->ime_cursor = widget->ime_length = 0;
			vgui_edit_invalidate_content_width(widget);
		}
		vgui_mark_dirty(widget);
	}

	const char* vgui_widget_get_text_key(vgui_widget* widget)
	{
		return widget ? widget->text_key.c_str() : "";
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

	void vgui_widget_set_tooltip(vgui_widget* widget, const char* text)
	{
		if (!widget) return;
		widget->tooltip = text ? text : "";
		vgui_context* ctx = widget->ctx;
		if (ctx && ctx->tooltip.owner == widget)
		{
			ctx->tooltip.text = vgui_tooltip_owner_text(widget, ctx->tooltip.owner_item);
			if (ctx->tooltip.text.empty())
				vgui_tooltip_hide(ctx);
			else
				vgui_tooltip_measure(ctx);
		}
	}

	const char* vgui_widget_get_tooltip(vgui_widget* widget)
	{
		return widget ? widget->tooltip.c_str() : "";
	}

	void vgui_widget_set_tooltip_provider(vgui_widget* widget,
		vgui_tooltip_text_fn fn, void* user)
	{
		if (!widget) return;
		widget->tooltip_text_fn = fn;
		widget->tooltip_user = user;
		vgui_context* ctx = widget->ctx;
		if (ctx && ctx->tooltip.owner == widget)
		{
			ctx->tooltip.text = vgui_tooltip_owner_text(widget, ctx->tooltip.owner_item);
			if (ctx->tooltip.text.empty())
				vgui_tooltip_hide(ctx);
			else
				vgui_tooltip_measure(ctx);
		}
	}

	void vgui_widget_set_tooltip_delay(vgui_widget* widget, float seconds)
	{
		if (widget) widget->tooltip_delay = seconds < 0.0f ? -1.0f : seconds;
	}

	float vgui_widget_get_tooltip_delay(vgui_widget* widget)
	{
		return widget ? widget->tooltip_delay : -1.0f;
	}

	void vgui_widget_set_tooltip_enabled(vgui_widget* widget, int enabled)
	{
		if (!widget) return;
		widget->tooltip_enabled = enabled ? 1 : 0;
		vgui_context* ctx = widget->ctx;
		if (ctx && !enabled &&
			(ctx->tooltip.candidate == widget || ctx->tooltip.owner == widget))
			vgui_tooltip_clear_candidate(ctx);
	}

	int vgui_widget_get_tooltip_enabled(vgui_widget* widget)
	{
		return widget ? widget->tooltip_enabled : 0;
	}

	void vgui_widget_set_tooltip_when_disabled(vgui_widget* widget, int enabled)
	{
		if (widget) widget->tooltip_when_disabled = enabled ? 1 : 0;
	}

	int vgui_widget_get_tooltip_when_disabled(vgui_widget* widget)
	{
		return widget ? widget->tooltip_when_disabled : 0;
	}

	void vgui_widget_set_tooltip_anchor(vgui_widget* widget, vgui_tooltip_anchor anchor)
	{
		if (!widget) return;
		widget->tooltip_anchor = anchor == VGUI_TOOLTIP_ANCHOR_WIDGET
			? VGUI_TOOLTIP_ANCHOR_WIDGET : VGUI_TOOLTIP_ANCHOR_POINTER;
		vgui_context* ctx = widget->ctx;
		if (ctx && ctx->tooltip.owner == widget)
			vgui_tooltip_measure(ctx);
	}

	vgui_tooltip_anchor vgui_widget_get_tooltip_anchor(vgui_widget* widget)
	{
		return widget ? widget->tooltip_anchor : VGUI_TOOLTIP_ANCHOR_POINTER;
	}

	void vgui_context_set_tooltip_enabled(vgui_context* ctx, int enabled)
	{
		if (!ctx) return;
		ctx->tooltip_enabled = enabled ? 1 : 0;
		if (!enabled) vgui_tooltip_clear_candidate(ctx);
	}

	int vgui_context_get_tooltip_enabled(vgui_context* ctx)
	{
		return ctx ? ctx->tooltip_enabled : 0;
	}

	void vgui_context_hide_tooltip(vgui_context* ctx)
	{
		if (ctx) vgui_tooltip_clear_candidate(ctx);
	}

	int vgui_context_tooltip_visible(vgui_context* ctx)
	{
		if (!ctx || !ctx->tooltip.visible || !ctx->tooltip.owner) return 0;
		if (!vgui_tooltip_visible_chain(ctx->tooltip.owner)) return 0;
		return 1;
	}

	const char* vgui_context_tooltip_text(vgui_context* ctx)
	{
		return vgui_context_tooltip_visible(ctx) ? ctx->tooltip.text.c_str() : "";
	}

	vgui_widget* vgui_context_tooltip_owner(vgui_context* ctx)
	{
		return vgui_context_tooltip_visible(ctx) ? ctx->tooltip.owner : nullptr;
	}

	int vgui_context_tooltip_rect(vgui_context* ctx,
		float* x, float* y, float* width, float* height)
	{
		if (!vgui_context_tooltip_visible(ctx)) return 0;
		vgui_tooltip_measure(ctx);
		if (!ctx->tooltip.visible) return 0;
		if (x) *x = ctx->tooltip.x;
		if (y) *y = ctx->tooltip.y;
		if (width) *width = ctx->tooltip.w;
		if (height) *height = ctx->tooltip.h;
		return 1;
	}

	//----------------------------------------------------------------------------
	// modal, dialog and toast
	//----------------------------------------------------------------------------

	void vgui_modal_options_default(vgui_modal_options* options)
	{
		if (!options) return;
		vgui_modal_options o = {};
		o.close_on_escape = 1;
		o.close_on_backdrop = 0;
		o.dim_background = 1;
		o.destroy_on_close = 0;
		o.escape_result = VGUI_MODAL_RESULT_CANCEL;
		o.backdrop_result = VGUI_MODAL_RESULT_CANCEL;
		o.margin = -1.0f;
		o.backdrop_alpha = -1.0f;
		o.on_close = nullptr;
		o.user = nullptr;
		*options = o;
	}

	int vgui_modal_show(vgui_context* ctx, vgui_widget* panel,
		const vgui_modal_options* options)
	{
		if (!ctx || !panel || panel->ctx != ctx || panel == ctx->root ||
			panel->kind == VGUI_MENU || panel->destroy_pending ||
			!ctx->alive.count(panel) || ctx->modal.active)
			return 0;

		vgui_modal_options opts;
		vgui_modal_options_default(&opts);
		if (options) opts = *options;
		if (opts.margin < 0.0f) opts.margin = -1.0f;
		if (opts.backdrop_alpha < 0.0f) opts.backdrop_alpha = -1.0f;
		else opts.backdrop_alpha = vgui_clampf(opts.backdrop_alpha, 0.0f, 1.0f);

		if (ctx->open_menu) vgui_menu_close_all(ctx);
		if (ctx->popup.content)
			vgui_popup_close_internal(ctx, VGUI_POPUP_CLOSE_EXPLICIT);
		if (!ctx->alive.count(panel) || panel->destroy_pending) return 0;

		vgui_widget* restore = ctx->focus;
		if (restore && (!ctx->alive.count(restore) || restore->destroy_pending))
			restore = nullptr;
		vgui_widget* keep_focus = (restore && vgui_is_within(panel, restore))
			? restore : nullptr;

		// Input state belongs to the background tree; drop it before the modal
		// takes over so a pending press cannot activate a hidden control.
		if (ctx->hot && ctx->alive.count(ctx->hot)) ctx->hot->hovered = 0;
		if (ctx->active && ctx->alive.count(ctx->active)) ctx->active->pressed = 0;
		if (ctx->press_target && ctx->alive.count(ctx->press_target))
			ctx->press_target->pressed = 0;
		ctx->hot = nullptr;
		ctx->active = nullptr;
		ctx->press_target = nullptr;
		ctx->slider_drag = nullptr;
		vgui_tooltip_clear_candidate(ctx);

		vgui_widget* stage = vgui_new_widget(ctx, VGUI_PANEL, nullptr);
		if (!stage) return 0;
		stage->bg = 0;
		stage->border = 0;
		stage->border_width = 0;
		stage->rounding = 0;
		stage->padding[0] = stage->padding[1] = 0.0f;
		stage->padding[2] = stage->padding[3] = 0.0f;
		stage->layout = VGUI_LAYOUT_ABSOLUTE;
		stage->clip_children = 1;
		stage->visible = 1;
		stage->focusable = 0;

		vgui_widget_remove(panel);
		vgui_widget_attach(stage, panel);
		if (panel->parent != stage)
		{
			vgui_schedule_destroy(stage);
			if (!ctx->in_input) vgui_flush_pending(ctx);
			return 0;
		}

		ctx->modal.active = 1;
		ctx->modal.swallow_up = 0;
		ctx->modal.result = VGUI_MODAL_RESULT_NONE;
		ctx->modal.stage = stage;
		ctx->modal.panel = panel;
		ctx->modal.restore_focus = restore;
		ctx->modal.options = opts;
		ctx->layout_dirty = true;

		vgui_widget* first = keep_focus ? keep_focus : vgui_first_focusable(panel);
		vgui_set_focus_internal(ctx, first);
		return 1;
	}

	void vgui_modal_close(vgui_context* ctx, int result)
	{
		vgui_modal_close_internal(ctx, result);
	}

	int vgui_modal_active(vgui_context* ctx)
	{
		return ctx && ctx->modal.active ? 1 : 0;
	}

	vgui_widget* vgui_modal_panel(vgui_context* ctx)
	{
		return ctx && ctx->modal.active ? ctx->modal.panel : nullptr;
	}

	int vgui_modal_result(vgui_context* ctx)
	{
		return ctx ? ctx->modal.result : VGUI_MODAL_RESULT_NONE;
	}

	void vgui_dialog_desc_default(vgui_dialog_desc* desc)
	{
		if (!desc) return;
		vgui_dialog_desc d = {};
		d.kind = VGUI_DIALOG_ALERT;
		d.danger = 0;
		d.width = 0.0f;
		*desc = d;
	}

	vgui_widget* vgui_dialog_show(vgui_context* ctx, const vgui_dialog_desc* desc)
	{
		if (!ctx || !desc) return nullptr;
		if (ctx->modal.active) return nullptr;

		const vgui_modal_style& ms = ctx->style.modal;
		float width = desc->width > 0.0f ? desc->width : ms.dialog_width;
		width = std::max(width, ms.min_width);
		float margin = std::max(0.0f, ms.margin);
		float avail_w = vgui_overlay_width(ctx) - margin * 2.0f;
		if (avail_w > 0.0f) width = std::min(width, avail_w);
		width = std::max(1.0f, width);
		float pad = std::max(0.0f, ms.padding);
		float content_w = std::max(1.0f, width - pad * 2.0f);

		vgui_widget* panel = vgui_widget_create(ctx, VGUI_PANEL, nullptr);
		if (!panel) return nullptr;
		panel->bg = ctx->theme.modal_bg;
		panel->border = ctx->theme.modal_border;
		panel->border_width = std::max(0.0f, ms.border_width);
		panel->rounding = std::max(0.0f, ms.rounding);
		panel->padding[0] = panel->padding[1] = pad;
		panel->padding[2] = panel->padding[3] = pad;
		panel->req_w = width;
		panel->req_h = 0.0f;
		panel->layout = VGUI_LAYOUT_COLUMN;
		panel->align_items = VGUI_ALIGN_STRETCH;
		panel->gap = 0.0f;

		auto add_label = [&](const char* text) -> vgui_widget*
			{
				if (!text || !*text) return nullptr;
				std::vector<std::string> lines = vgui_wrap_text(ctx, panel, text, content_w);
				std::string joined;
				for (size_t i = 0; i < lines.size(); ++i)
				{
					if (i) joined += '\n';
					joined += lines[i];
				}
				vgui_widget* label = vgui_widget_add(panel, VGUI_LABEL, joined.c_str());
				if (!label) return nullptr;
				label->padding[0] = label->padding[1] = 0.0f;
				label->padding[2] = label->padding[3] = 0.0f;
				label->text_align_x = 0.0f;
				label->text_align_y = 0.5f;
				return label;
			};

		vgui_widget* title = add_label(desc->title);
		if (title)
			title->margin[3] = std::max(0.0f, ms.title_gap);
		vgui_widget* message = add_label(desc->message);
		bool has_body = desc->kind == VGUI_DIALOG_PROMPT;
		if (message)
			message->margin[3] = (has_body || desc->kind != VGUI_DIALOG_ALERT)
				? std::max(0.0f, ms.message_gap) : 0.0f;

		vgui_widget* input = nullptr;
		if (desc->kind == VGUI_DIALOG_PROMPT)
		{
			input = vgui_widget_add(panel, VGUI_EDIT, nullptr);
			if (input)
			{
				input->req_w = 0.0f;
				vgui_edit_set_placeholder(input, desc->placeholder ? desc->placeholder : "");
				vgui_edit_set_text(input, desc->initial_text ? desc->initial_text : "");
				input->margin[3] = 0.0f;
			}
		}

		vgui_widget* row = vgui_widget_add(panel, VGUI_PANEL, nullptr);
		if (row)
		{
			row->bg = 0;
			row->border = 0;
			row->border_width = 0;
			row->rounding = 0;
			row->padding[0] = row->padding[1] = 0.0f;
			row->padding[2] = row->padding[3] = 0.0f;
			row->layout = VGUI_LAYOUT_ROW;
			row->justify = VGUI_JUSTIFY_END;
			row->gap = std::max(0.0f, ms.button_gap);
			row->margin[1] = std::max(0.0f, ms.button_top_gap);
			row->req_w = 0.0f;
		}

		vgui_widget* cancel_button = nullptr;
		vgui_widget* confirm_button = nullptr;
		if (row)
		{
			const char* confirm_text = (desc->confirm_text && *desc->confirm_text)
				? desc->confirm_text : "OK";
			if (desc->kind != VGUI_DIALOG_ALERT)
			{
				const char* cancel_text = (desc->cancel_text && *desc->cancel_text)
					? desc->cancel_text : "Cancel";
				cancel_button = vgui_widget_add(row, VGUI_BUTTON, cancel_text);
				if (cancel_button)
				{
					cancel_button->min_w = 72.0f;
					cancel_button->value = (float)VGUI_MODAL_RESULT_CANCEL;
					vgui_widget_on_event(cancel_button, vgui_dialog_button_event, nullptr);
				}
			}
			confirm_button = vgui_widget_add(row, VGUI_BUTTON, confirm_text);
			if (confirm_button)
			{
				confirm_button->min_w = 72.0f;
				confirm_button->value = (float)VGUI_MODAL_RESULT_CONFIRM;
				vgui_widget_on_event(confirm_button, vgui_dialog_button_event, nullptr);
				if (desc->danger)
				{
					confirm_button->bg = ctx->theme.modal_danger;
					confirm_button->border = vgui_mix_color(
						ctx->theme.modal_danger, 0xFF000000, 0.35f);
					confirm_button->text_color = 0xFFFFFFFF;
				}
			}
		}

		vgui_modal_options mo;
		vgui_modal_options_default(&mo);
		mo.destroy_on_close = 1;
		mo.close_on_backdrop = 0;
		mo.escape_result = VGUI_MODAL_RESULT_CANCEL;
		mo.backdrop_result = VGUI_MODAL_RESULT_CANCEL;
		if (!vgui_modal_show(ctx, panel, &mo))
		{
			vgui_widget_destroy(panel);
			return nullptr;
		}

		ctx->dialog.panel = panel;
		ctx->dialog.input = input;
		ctx->dialog.confirm_button = confirm_button;
		ctx->dialog.cancel_button = cancel_button;
		ctx->dialog.on_result = desc->on_result;
		ctx->dialog.user = desc->user;
		ctx->dialog.kind = (int)desc->kind;

		if (input)
		{
			vgui_set_focus_internal(ctx, input);
			input->edit_anchor = 0;
			input->edit_cursor = (int)input->edit_text.size();
			input->caret_blink = 0.0f;
		}
		else if (confirm_button)
		{
			vgui_set_focus_internal(ctx, confirm_button);
		}
		return panel;
	}

	vgui_widget* vgui_dialog_input(vgui_context* ctx)
	{
		if (!ctx || !ctx->modal.active || ctx->dialog.panel != ctx->modal.panel)
			return nullptr;
		return ctx->dialog.input;
	}

	vgui_widget* vgui_dialog_alert(vgui_context* ctx, const char* title,
		const char* message, vgui_dialog_close_fn on_result, void* user)
	{
		vgui_dialog_desc desc;
		vgui_dialog_desc_default(&desc);
		desc.kind = VGUI_DIALOG_ALERT;
		desc.title = title;
		desc.message = message;
		desc.on_result = on_result;
		desc.user = user;
		return vgui_dialog_show(ctx, &desc);
	}

	vgui_widget* vgui_dialog_confirm(vgui_context* ctx, const char* title,
		const char* message, int danger, vgui_dialog_close_fn on_result, void* user)
	{
		vgui_dialog_desc desc;
		vgui_dialog_desc_default(&desc);
		desc.kind = VGUI_DIALOG_CONFIRM;
		desc.title = title;
		desc.message = message;
		desc.danger = danger;
		desc.on_result = on_result;
		desc.user = user;
		return vgui_dialog_show(ctx, &desc);
	}

	vgui_widget* vgui_dialog_prompt(vgui_context* ctx, const char* title,
		const char* message, const char* initial_text,
		vgui_dialog_close_fn on_result, void* user)
	{
		vgui_dialog_desc desc;
		vgui_dialog_desc_default(&desc);
		desc.kind = VGUI_DIALOG_PROMPT;
		desc.title = title;
		desc.message = message;
		desc.initial_text = initial_text;
		desc.on_result = on_result;
		desc.user = user;
		return vgui_dialog_show(ctx, &desc);
	}

	void vgui_toast_options_default(vgui_toast_options* options)
	{
		if (!options) return;
		vgui_toast_options o = {};
		o.position = VGUI_TOAST_TOP_RIGHT;
		o.level = VGUI_TOAST_INFO;
		o.duration = -1.0f;
		*options = o;
	}

	vgui_toast_id vgui_toast_show(vgui_context* ctx, const char* text,
		const vgui_toast_options* options)
	{
		if (!ctx || !text || !*text) return 0;
		vgui_toast_options opts;
		vgui_toast_options_default(&opts);
		if (options) opts = *options;
		if (opts.position < VGUI_TOAST_TOP_RIGHT ||
			opts.position > VGUI_TOAST_BOTTOM_CENTER)
			opts.position = VGUI_TOAST_TOP_RIGHT;
		if (opts.level < VGUI_TOAST_INFO || opts.level > VGUI_TOAST_ERROR)
			opts.level = VGUI_TOAST_INFO;

		float duration = opts.duration;
		if (duration < 0.0f) duration = ctx->style.toast.duration;
		duration = std::max(0.0f, duration);

		vgui_toast_record rec;
		rec.id = ctx->next_toast_id++;
		if (rec.id == 0) rec.id = ctx->next_toast_id++;
		rec.text = text;
		rec.options = opts;
		rec.options.duration = duration;
		rec.sticky = duration <= 0.0f ? 1 : 0;
		rec.time_left = duration;
		vgui_toast_id id = rec.id;
		ctx->toasts.push_back(std::move(rec));
		ctx->layout_dirty = true;
		return id;
	}

	int vgui_toast_dismiss(vgui_context* ctx, vgui_toast_id toast)
	{
		if (!ctx || toast == 0) return 0;
		for (size_t i = 0; i < ctx->toasts.size(); ++i)
		{
			if (ctx->toasts[i].id != toast) continue;
			ctx->toasts.erase(ctx->toasts.begin() + (std::ptrdiff_t)i);
			ctx->layout_dirty = true;
			return 1;
		}
		return 0;
	}

	void vgui_toast_clear(vgui_context* ctx)
	{
		if (!ctx) return;
		if (!ctx->toasts.empty())
		{
			ctx->toasts.clear();
			ctx->layout_dirty = true;
		}
	}

	int vgui_toast_count(vgui_context* ctx)
	{
		return ctx ? (int)ctx->toasts.size() : 0;
	}

	int vgui_toast_get_rect(vgui_context* ctx, vgui_toast_id toast,
		float* x, float* y, float* width, float* height)
	{
		if (!ctx || toast == 0) return 0;
		vgui_toast_layout(ctx);
		int max_visible = std::max(1, ctx->style.toast.max_visible);
		for (size_t i = 0; i < ctx->toasts.size(); ++i)
		{
			if (ctx->toasts[i].id != toast) continue;
			if ((int)i >= max_visible) return 0;
			const vgui_toast_record& t = ctx->toasts[i];
			if (t.w <= 0.0f || t.h <= 0.0f) return 0;
			if (x) *x = t.x;
			if (y) *y = t.y;
			if (width) *width = t.w;
			if (height) *height = t.h;
			return 1;
		}
		return 0;
	}

	// ---- context 级通用弹出面板 -----------------------------------------------

	int vgui_popup_open(vgui_context* ctx, vgui_widget* content, float x, float y)
	{
		if (!ctx || !content || content->ctx != ctx) return 0;
		if (content->parent) return 0;	/* 只接受 detached 子树 */
		if (ctx->modal.active) return 0;
		if (ctx->popup.content == content)
		{
			/* 重复打开同一个内容:仅更新锚点。 */
			ctx->popup.x = x;
			ctx->popup.y = y;
			vgui_popup_update_layout(ctx);
			return 1;
		}
		if (ctx->popup.content)
			vgui_popup_close_internal(ctx, VGUI_POPUP_CLOSE_EXPLICIT);
		if (ctx->open_menu)
			vgui_menu_close_all(ctx);
		if (!ctx->alive.count(content) || content->destroy_pending)
			return 0;

		/* 保留 close 监听器:on_close 是 context 级持久回调,跨会话有效。 */
		vgui_popup_close_fn fn = ctx->popup.close_fn;
		void* user = ctx->popup.close_user;
		ctx->popup = vgui_popup_state();
		ctx->popup.close_fn = fn;
		ctx->popup.close_user = user;
		ctx->popup.content = content;
		ctx->popup.x = x;
		ctx->popup.y = y;
		content->visible = 1;
		ctx->layout_dirty = true;
		vgui_ensure_layout(ctx);
		return 1;
	}

	void vgui_popup_close(vgui_context* ctx)
	{
		vgui_popup_close_internal(ctx, VGUI_POPUP_CLOSE_EXPLICIT);
	}

	int vgui_popup_active(vgui_context* ctx)
	{
		return ctx && ctx->popup.content ? 1 : 0;
	}

	vgui_widget* vgui_popup_content(vgui_context* ctx)
	{
		return ctx ? ctx->popup.content : nullptr;
	}

	void vgui_popup_on_close(vgui_context* ctx, vgui_popup_close_fn fn, void* user)
	{
		if (!ctx) return;
		ctx->popup.close_fn = fn;
		ctx->popup.close_user = user;
	}

	void vgui_image_set_texture(vgui_widget* widget, vgui_texture* texture)
	{
		if (!widget || widget->kind != VGUI_IMAGE || !widget->image)
			return;
		if (widget->image->texture == texture)
			return;
		if (texture)
			vgui_texture_retain(texture);
		vgui_texture* old = widget->image->texture;
		widget->image->texture = texture;
		if (old)
			vgui_texture_release(old);
		vgui_mark_dirty(widget);
	}

	void vgui_image_set_source(vgui_widget* widget, int x, int y, int width, int height)
	{
		if (!widget || widget->kind != VGUI_IMAGE || !widget->image) return;
		widget->image->src_x = std::max(0, x);
		widget->image->src_y = std::max(0, y);
		widget->image->src_w = std::max(0, width);
		widget->image->src_h = std::max(0, height);
		vgui_mark_dirty(widget);
	}

	void vgui_image_set_fit(vgui_widget* widget, vgui_image_fit fit)
	{
		if (!widget || widget->kind != VGUI_IMAGE || !widget->image) return;
		widget->image->fit = fit;
		vgui_mark_dirty(widget);
	}

	void vgui_image_set_tint(vgui_widget* widget, vgui_color color)
	{
		if (!widget || widget->kind != VGUI_IMAGE || !widget->image) return;
		widget->image->tint = color;
		vgui_mark_dirty(widget);
	}

	void vgui_image_set_flip(vgui_widget* widget, int flip_x, int flip_y)
	{
		if (!widget || widget->kind != VGUI_IMAGE || !widget->image) return;
		widget->image->flip_x = flip_x != 0;
		widget->image->flip_y = flip_y != 0;
		vgui_mark_dirty(widget);
	}

	void vgui_image_set_pixel_snap(vgui_widget* widget, int enabled)
	{
		if (!widget || widget->kind != VGUI_IMAGE || !widget->image) return;
		widget->image->pixel_snap = enabled != 0;
		vgui_mark_dirty(widget);
	}

	// -------------------------------------------------------------------------
	// Button icon API
	// -------------------------------------------------------------------------

	void vgui_button_set_icon(vgui_widget* widget, vgui_texture* tex,
		int icon_w, int icon_h, vgui_button_icon_pos pos)
	{
		if (!widget || widget->kind != VGUI_BUTTON) return;
		if (widget->icon_tex == tex && widget->icon_w == icon_w &&
			widget->icon_h == icon_h && widget->icon_pos == pos)
			return;
		if (tex) vgui_texture_retain(tex);
		vgui_texture* old = widget->icon_tex;
		widget->icon_tex = tex;
		if (old) vgui_texture_release(old);
		widget->icon_w = icon_w;
		widget->icon_h = icon_h;
		widget->icon_pos = pos;
		if (widget->ctx) widget->ctx->layout_dirty = true;
		vgui_mark_dirty(widget);
	}

	void vgui_button_set_icon_gap(vgui_widget* widget, float gap)
	{
		if (!widget || widget->kind != VGUI_BUTTON) return;
		if (widget->icon_gap == gap) return;
		widget->icon_gap = gap;
		if (widget->ctx) widget->ctx->layout_dirty = true;
		vgui_mark_dirty(widget);
	}

	void vgui_button_set_icon_tint(vgui_widget* widget, vgui_color tint)
	{
		if (!widget || widget->kind != VGUI_BUTTON) return;
		widget->icon_tint = tint;   // 0 = no tint (using raw texture)
		vgui_mark_dirty(widget);
	}

	void vgui_button_set_style(vgui_widget* widget, vgui_button_style_kind kind)
	{
		if (!widget || widget->kind != VGUI_BUTTON) return;
		widget->btn_style_kind = kind;
		vgui_mark_dirty(widget);
	}

	vgui_button_style_kind vgui_button_get_style(vgui_widget* widget)
	{
		if (!widget || widget->kind != VGUI_BUTTON) return VGUI_BUTTON_STYLE_SOLID;
		return widget->btn_style_kind;
	}

	void vgui_button_set_gradient(vgui_widget* widget,
		vgui_color grad_top_default, vgui_color grad_bot_default,
		vgui_color grad_top_hover, vgui_color grad_bot_hover,
		vgui_color grad_top_active, vgui_color grad_bot_active,
		vgui_color border_light, vgui_color border_dark)
	{
		if (!widget || widget->kind != VGUI_BUTTON) return;
		widget->grad_top[0] = grad_top_default;
		widget->grad_top[1] = grad_top_hover;
		widget->grad_top[2] = grad_top_active;
		widget->grad_bot[0] = grad_bot_default;
		widget->grad_bot[1] = grad_bot_hover;
		widget->grad_bot[2] = grad_bot_active;
		widget->gradient_border_light = border_light;
		widget->gradient_border_dark = border_dark;
		vgui_mark_dirty(widget);
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

	void vgui_widget_set_align_content(vgui_widget* widget, vgui_align_content align)
	{
		if (!widget) return;
		widget->align_content = align;
		vgui_mark_dirty(widget);
	}

	vgui_align_content vgui_widget_get_align_content(vgui_widget* widget)
	{
		return widget ? widget->align_content : VGUI_ALIGN_CONTENT_START;
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
			// Open menus are context popups: their geometry is maintained by
			// the menu layout pass, not by the regular widget-tree layout.
			// The context-level popup (vgui_popup_open) is the same.
			const bool menu_open = widget->kind == VGUI_MENU && widget->menu &&
				widget->menu->open;
			if (!menu_open && widget != widget->ctx->popup.content)
			{
				vgui_measure_widget(widget, widget->ctx);
				vgui_layout_widget(widget, widget->ctx, 0, 0,
					std::max(widget->intrinsic_w, 1.0f), std::max(widget->intrinsic_h, 1.0f));
			}
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
		vgui_mark_dirty(widget);
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
		vgui_notify_scroll(panel);
	}

	void vgui_panel_scroll_by(vgui_widget* panel, float dx, float dy)
	{
		if (!panel) return;
		panel->scroll_x += dx;
		panel->scroll_y += dy;
		vgui_mark_dirty(panel);
		vgui_notify_scroll(panel);
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

	void vgui_panel_set_scroll_listener(vgui_widget* panel, vgui_scroll_fn fn,
		void* user)
	{
		if (!panel) return;
		panel->scroll_fn = fn;
		panel->scroll_user = fn ? user : nullptr;
	}

	//----------------------------------------------------------------------------
	// splitter
	//----------------------------------------------------------------------------

	void vgui_splitter_set_vertical(vgui_widget* splitter, int vertical)
	{
		if (!splitter || splitter->kind != VGUI_SPLITTER) return;
		splitter->vertical = vertical ? 1 : 0;
		vgui_mark_dirty(splitter);
	}

	int vgui_splitter_get_vertical(vgui_widget* splitter)
	{
		return (splitter && splitter->kind == VGUI_SPLITTER) ? splitter->vertical : 0;
	}

	void vgui_splitter_set_ratio(vgui_widget* splitter, float ratio)
	{
		if (!splitter || splitter->kind != VGUI_SPLITTER) return;
		splitter->value = vgui_clampf(ratio, 0.0f, 1.0f);
		vgui_mark_dirty(splitter);
	}

	float vgui_splitter_get_ratio(vgui_widget* splitter)
	{
		if (!splitter || splitter->kind != VGUI_SPLITTER) return 0.0f;
		return vgui_clampf(splitter->value, 0.0f, 1.0f);
	}

	void vgui_splitter_set_sash_size(vgui_widget* splitter, float size)
	{
		vgui_splitter_state* st = vgui_splitter_of(splitter);
		if (!st) return;
		st->sash_size = std::max(0.0f, size);
		vgui_mark_dirty(splitter);
	}

	float vgui_splitter_get_sash_size(vgui_widget* splitter)
	{
		vgui_splitter_state* st = vgui_splitter_of(splitter);
		return st ? st->sash_size : 0.0f;
	}

	void vgui_splitter_reset(vgui_widget* splitter)
	{
		vgui_splitter_state* st = vgui_splitter_of(splitter);
		if (!st) return;
		splitter->value = st->default_ratio;
		st->dragging = 0;
		st->changed = 0;
		vgui_mark_dirty(splitter);
	}

	//----------------------------------------------------------------------------
	// tab view
	//----------------------------------------------------------------------------

	vgui_item_id vgui_tab_add(vgui_widget* tabs, const char* title, vgui_widget* page)
	{
		vgui_tab_state* st = vgui_tabs_of(tabs);
		if (!st) return 0;
		vgui_item_id id = 0;
		do
		{
			id = st->next_id++;
			if (st->next_id == 0) st->next_id = 1;
		} while (id == 0 || st->index.find(id) != st->index.end());

		if (page)
		{
			if (page->ctx != tabs->ctx || page == tabs || page == tabs->ctx->root ||
				page->kind == VGUI_MENU || page->destroy_pending)
				page = nullptr;
			else
			{
				for (vgui_widget* p = tabs; p; p = p->parent)
				{
					if (p != page) continue;
					page = nullptr;
					break;
				}
			}
		}
		vgui_tab_item item;
		item.id = id;
		item.title = title ? title : "";
		item.page = page;
		st->index[id] = st->tabs.size();
		st->tabs.push_back(item);

		if (page)
		{
			vgui_widget_attach(tabs, page);
			page->visible = 0;
		}
		if (st->active == 0)
			vgui_tab_set_active_internal(tabs, id, false);
		else
			vgui_tab_update_pages(tabs);
		if (tabs->ctx) tabs->ctx->layout_dirty = true;
		return id;
	}

	int vgui_tab_remove(vgui_widget* tabs, vgui_item_id item)
	{
		vgui_tab_state* st = vgui_tabs_of(tabs);
		int index = vgui_tab_find_index(tabs, item);
		if (!st || index < 0) return 0;
		vgui_widget* page = st->tabs[(size_t)index].page;
		bool was_active = (st->active == item);
		vgui_item_id old_active = st->active;
		st->tabs.erase(st->tabs.begin() + index);
		st->index.clear();
		for (size_t i = 0; i < st->tabs.size(); ++i)
			st->index[st->tabs[i].id] = i;
		if (was_active)
		{
			if (index < (int)st->tabs.size())
				st->active = st->tabs[(size_t)index].id;
			else if (!st->tabs.empty())
				st->active = st->tabs.back().id;
			else
				st->active = 0;
		}
		vgui_tab_update_pages(tabs);
		if (page && page->ctx == tabs->ctx)
			vgui_widget_destroy(page);
		if (tabs->ctx)
		{
			tabs->ctx->layout_dirty = true;
			if (st->active != old_active && st->active != 0 && st->select_fn &&
				tabs->ctx->alive.count(tabs))
				st->select_fn(tabs, st->active, st->select_user);
		}
		return 1;
	}

	void vgui_tab_clear(vgui_widget* tabs)
	{
		vgui_tab_state* st = vgui_tabs_of(tabs);
		if (!st) return;
		std::vector<vgui_widget*> pages;
		for (const vgui_tab_item& tab : st->tabs)
			if (tab.page) pages.push_back(tab.page);
		st->tabs.clear();
		st->index.clear();
		st->active = 0;
		st->hover = st->pressed = st->close_hover = st->close_pressed = 0;
		st->drag_item = 0;
		st->drag_active = 0;
		vgui_tab_update_pages(tabs);
		for (vgui_widget* page : pages)
			if (page && page->ctx == tabs->ctx) vgui_widget_destroy(page);
		if (tabs->ctx) tabs->ctx->layout_dirty = true;
	}

	int vgui_tab_get_count(vgui_widget* tabs)
	{
		vgui_tab_state* st = vgui_tabs_of(tabs);
		return st ? (int)st->tabs.size() : 0;
	}

	vgui_item_id vgui_tab_get_at(vgui_widget* tabs, int index)
	{
		vgui_tab_state* st = vgui_tabs_of(tabs);
		if (!st || index < 0 || index >= (int)st->tabs.size()) return 0;
		return st->tabs[(size_t)index].id;
	}

	int vgui_tab_get_index(vgui_widget* tabs, vgui_item_id item)
	{
		return vgui_tab_find_index(tabs, item);
	}

	vgui_item_id vgui_tab_get_active(vgui_widget* tabs)
	{
		vgui_tab_state* st = vgui_tabs_of(tabs);
		return st ? st->active : 0;
	}

	int vgui_tab_set_active(vgui_widget* tabs, vgui_item_id item)
	{
		return vgui_tab_set_active_internal(tabs, item, true) ? 1 : 0;
	}

	vgui_widget* vgui_tab_get_page(vgui_widget* tabs, vgui_item_id item)
	{
		vgui_tab_item* tab = vgui_tab_find(tabs, item);
		return tab ? tab->page : nullptr;
	}

	const char* vgui_tab_get_title(vgui_widget* tabs, vgui_item_id item)
	{
		vgui_tab_item* tab = vgui_tab_find(tabs, item);
		return tab ? tab->title.c_str() : "";
	}

	void vgui_tab_set_title(vgui_widget* tabs, vgui_item_id item, const char* title)
	{
		vgui_tab_item* tab = vgui_tab_find(tabs, item);
		if (!tab) return;
		tab->title = title ? title : "";
		vgui_mark_dirty(tabs);
	}

	int vgui_tab_get_enabled(vgui_widget* tabs, vgui_item_id item)
	{
		vgui_tab_item* tab = vgui_tab_find(tabs, item);
		return tab ? tab->enabled : 0;
	}

	void vgui_tab_set_enabled(vgui_widget* tabs, vgui_item_id item, int enabled)
	{
		vgui_tab_item* tab = vgui_tab_find(tabs, item);
		if (!tab) return;
		tab->enabled = enabled ? 1 : 0;
		vgui_mark_dirty(tabs);
	}

	int vgui_tab_get_closeable(vgui_widget* tabs, vgui_item_id item)
	{
		vgui_tab_item* tab = vgui_tab_find(tabs, item);
		return tab ? tab->closeable : 0;
	}

	void vgui_tab_set_closeable(vgui_widget* tabs, vgui_item_id item, int closeable)
	{
		vgui_tab_item* tab = vgui_tab_find(tabs, item);
		if (!tab) return;
		tab->closeable = closeable ? 1 : 0;
		vgui_mark_dirty(tabs);
	}

	int vgui_tab_get_unsaved(vgui_widget* tabs, vgui_item_id item)
	{
		vgui_tab_item* tab = vgui_tab_find(tabs, item);
		return tab ? tab->unsaved : 0;
	}

	void vgui_tab_set_unsaved(vgui_widget* tabs, vgui_item_id item, int unsaved)
	{
		vgui_tab_item* tab = vgui_tab_find(tabs, item);
		if (!tab) return;
		tab->unsaved = unsaved ? 1 : 0;
		vgui_mark_dirty(tabs);
	}

	int vgui_tab_move(vgui_widget* tabs, vgui_item_id item, int index)
	{
		if (!vgui_tab_move_internal(tabs, item, index)) return 0;
		vgui_tab_state* st = vgui_tabs_of(tabs);
		if (st && st->active != 0)
			vgui_tab_ensure_visible(tabs, st->active);
		if (tabs->ctx) tabs->ctx->layout_dirty = true;
		return 1;
	}

	int vgui_tab_get_rect(vgui_widget* tabs, vgui_item_id item,
		float* x, float* y, float* width, float* height)
	{
		if (tabs && tabs->ctx) vgui_ensure_layout(tabs->ctx);
		int index = vgui_tab_find_index(tabs, item);
		if (index < 0) return 0;
		return vgui_tab_rect(tabs, index, x, y, width, height) ? 1 : 0;
	}

	void vgui_tab_set_tab_height(vgui_widget* tabs, float height)
	{
		vgui_tab_state* st = vgui_tabs_of(tabs);
		if (!st) return;
		st->tab_height = std::max(1.0f, height);
		vgui_mark_dirty(tabs);
	}

	float vgui_tab_get_tab_height(vgui_widget* tabs)
	{
		vgui_tab_state* st = vgui_tabs_of(tabs);
		return st ? st->tab_height : 0.0f;
	}

	void vgui_tab_on_select(vgui_widget* tabs, vgui_item_fn fn, void* user)
	{
		vgui_tab_state* st = vgui_tabs_of(tabs);
		if (!st) return;
		st->select_fn = fn;
		st->select_user = user;
	}

	void vgui_tab_on_close(vgui_widget* tabs, vgui_item_fn fn, void* user)
	{
		vgui_tab_state* st = vgui_tabs_of(tabs);
		if (!st) return;
		st->close_fn = fn;
		st->close_user = user;
	}

	//----------------------------------------------------------------------------
	// dock API
	//----------------------------------------------------------------------------

	void vgui_dock_panel_desc_default(vgui_dock_panel_desc* desc)
	{
		if (!desc) return;
		*desc = vgui_dock_panel_desc();
		desc->zone = VGUI_DOCK_CENTER;
		desc->index = -1;
		desc->closeable = 1;
		desc->open = 1;
	}

	vgui_item_id vgui_dock_add(vgui_widget* dock, const vgui_dock_panel_desc* desc)
	{
		vgui_dock_state* st = vgui_dock_of(dock);
		if (!st || !desc || !vgui_dock_alive(dock)) return 0;
		vgui_widget* page = desc->panel;
		if (page)
		{
			if (page->ctx != dock->ctx || page == dock || page == dock->ctx->root ||
				page->destroy_pending || page->kind == VGUI_MENU)
				return 0;
			for (vgui_widget* p = dock; p; p = p->parent)
				if (p == page) return 0;
			for (const vgui_dock_panel& existing : st->panels)
				if (existing.page == page) return 0;
		}
		std::string string_id = desc->id ? desc->id : "";
		if (!string_id.empty())
		{
			for (const vgui_dock_panel& existing : st->panels)
				if (existing.string_id == string_id) return 0;
		}

		vgui_item_id id = 0;
		do
		{
			id = st->next_id++;
			if (st->next_id == 0) st->next_id = 1;
		} while (id == 0 || st->index.find(id) != st->index.end());

		vgui_dock_panel panel;
		panel.id = id;
		panel.string_id = string_id;
		panel.title = desc->title ? desc->title : "";
		panel.page = page;
		panel.closeable = desc->closeable ? 1 : 0;
		int zone = (int)desc->zone;
		if (zone < 0 || zone > (int)VGUI_DOCK_FLOATING) zone = VGUI_DOCK_CENTER;
		panel.zone = (vgui_dock_zone)zone;
		panel.open = desc->open ? 1 : 0;
		if (panel.zone == VGUI_DOCK_FLOATING) panel.open = 1;

		st->index[id] = st->panels.size();
		st->panels.push_back(panel);
		vgui_dock_panel* stored = &st->panels.back();
		if (stored->zone == VGUI_DOCK_FLOATING)
		{
			vgui_dock_clamp_rect(dock, stored, desc->x, desc->y, desc->width, desc->height, true);
			st->float_order.push_back(id);
		}
		else
		{
			vgui_dock_insert_order(st, zone, id, desc->index);
			if (stored->open)
			{
				st->active[zone] = id;
				st->scroll_active[zone] = 1;
			}
		}
		if (page)
		{
			vgui_widget_attach(dock, page);
			page->visible = 0;
		}
		vgui_mark_dirty(dock);
		if (stored->open && vgui_dock_alive(dock))
			vgui_dock_notify_activate(dock, id);
		if (vgui_dock_alive(dock))
			vgui_dock_notify_change(dock);
		return id;
	}

	int vgui_dock_remove(vgui_widget* dock, vgui_item_id panel_id)
	{
		vgui_dock_state* st = vgui_dock_of(dock);
		int index = vgui_dock_find_index(dock, panel_id);
		if (!st || index < 0) return 0;
		vgui_widget* page = st->panels[(size_t)index].page;
		st->panels.erase(st->panels.begin() + index);
		st->index.clear();
		for (size_t i = 0; i < st->panels.size(); ++i)
			st->index[st->panels[i].id] = i;
		vgui_dock_erase_order(st, panel_id);
		for (int z = 0; z < 4; ++z)
		{
			vgui_item_id next = vgui_dock_pick_active(st, z);
			if (st->active[z] != next) st->scroll_active[z] = 1;
			st->active[z] = next;
		}
		if (page)
		{
			vgui_dock_forget_focus(dock, page);
			if (page->parent == dock) vgui_widget_remove(page);
			if (page->ctx == dock->ctx) vgui_widget_destroy(page);
		}
		vgui_mark_dirty(dock);
		if (vgui_dock_alive(dock)) vgui_dock_notify_change(dock);
		return 1;
	}

	vgui_widget* vgui_dock_detach(vgui_widget* dock, vgui_item_id panel_id)
	{
		vgui_dock_state* st = vgui_dock_of(dock);
		int index = vgui_dock_find_index(dock, panel_id);
		if (!st || index < 0) return nullptr;
		vgui_widget* page = st->panels[(size_t)index].page;
		st->panels.erase(st->panels.begin() + index);
		st->index.clear();
		for (size_t i = 0; i < st->panels.size(); ++i)
			st->index[st->panels[i].id] = i;
		vgui_dock_erase_order(st, panel_id);
		for (int z = 0; z < 4; ++z)
		{
			vgui_item_id next = vgui_dock_pick_active(st, z);
			if (st->active[z] != next) st->scroll_active[z] = 1;
			st->active[z] = next;
		}
		if (page)
		{
			vgui_dock_forget_focus(dock, page);
			if (page->parent == dock) vgui_widget_remove(page);
			page->visible = 1;
		}
		vgui_mark_dirty(dock);
		if (vgui_dock_alive(dock)) vgui_dock_notify_change(dock);
		return page;
	}

	void vgui_dock_clear(vgui_widget* dock)
	{
		vgui_dock_state* st = vgui_dock_of(dock);
		if (!st) return;
		std::vector<vgui_widget*> pages;
		for (const vgui_dock_panel& panel : st->panels)
			if (panel.page) pages.push_back(panel.page);
		st->panels.clear();
		st->index.clear();
		for (int z = 0; z < 4; ++z)
		{
			st->order[z].clear();
			st->active[z] = 0;
			st->scroll_active[z] = 0;
			st->tabs[z].clear();
			st->tab_scroll[z] = 0.0f;
		}
		st->float_order.clear();
		st->hover = 0;
		st->pressed = 0;
		st->close_hover = 0;
		st->close_pressed = 0;
		st->drag_mode = 0;
		st->drag_panel = 0;
		st->drop_zone = -1;
		st->drop_index = -1;
		st->drop_floating = 0;
		for (vgui_widget* page : pages)
		{
			vgui_dock_forget_focus(dock, page);
			if (page->parent == dock) vgui_widget_remove(page);
			if (page->ctx == dock->ctx) vgui_widget_destroy(page);
		}
		vgui_mark_dirty(dock);
		if (vgui_dock_alive(dock)) vgui_dock_notify_change(dock);
	}

	int vgui_dock_get_count(vgui_widget* dock)
	{
		vgui_dock_state* st = vgui_dock_of(dock);
		return st ? (int)st->panels.size() : 0;
	}

	vgui_item_id vgui_dock_get_at(vgui_widget* dock, int index)
	{
		vgui_dock_state* st = vgui_dock_of(dock);
		if (!st || index < 0 || index >= (int)st->panels.size()) return 0;
		return st->panels[(size_t)index].id;
	}

	vgui_item_id vgui_dock_find(vgui_widget* dock, const char* id)
	{
		vgui_dock_state* st = vgui_dock_of(dock);
		if (!st || !id) return 0;
		for (const vgui_dock_panel& panel : st->panels)
			if (panel.string_id == id) return panel.id;
		return 0;
	}

	vgui_widget* vgui_dock_get_panel(vgui_widget* dock, vgui_item_id panel_id)
	{
		vgui_dock_panel* panel = vgui_dock_find(dock, panel_id);
		return panel ? panel->page : nullptr;
	}

	const char* vgui_dock_get_id(vgui_widget* dock, vgui_item_id panel_id)
	{
		vgui_dock_panel* panel = vgui_dock_find(dock, panel_id);
		return panel ? panel->string_id.c_str() : nullptr;
	}

	const char* vgui_dock_get_title(vgui_widget* dock, vgui_item_id panel_id)
	{
		vgui_dock_panel* panel = vgui_dock_find(dock, panel_id);
		return panel ? panel->title.c_str() : nullptr;
	}

	void vgui_dock_set_title(vgui_widget* dock, vgui_item_id panel_id, const char* title)
	{
		vgui_dock_panel* panel = vgui_dock_find(dock, panel_id);
		if (!panel) return;
		panel->title = title ? title : "";
		vgui_dock_state* st = vgui_dock_of(dock);
		if (st && panel->zone >= 0 && panel->zone < 4)
			st->scroll_active[panel->zone] = 1;
		vgui_mark_dirty(dock);
		if (vgui_dock_alive(dock)) vgui_dock_notify_change(dock);
	}

	int vgui_dock_get_closeable(vgui_widget* dock, vgui_item_id panel_id)
	{
		vgui_dock_panel* panel = vgui_dock_find(dock, panel_id);
		return panel ? panel->closeable : 0;
	}

	void vgui_dock_set_closeable(vgui_widget* dock, vgui_item_id panel_id, int closeable)
	{
		vgui_dock_panel* panel = vgui_dock_find(dock, panel_id);
		if (!panel) return;
		panel->closeable = closeable ? 1 : 0;
		vgui_dock_state* st = vgui_dock_of(dock);
		if (st && panel->zone >= 0 && panel->zone < 4)
			st->scroll_active[panel->zone] = 1;
		vgui_mark_dirty(dock);
		if (vgui_dock_alive(dock)) vgui_dock_notify_change(dock);
	}

	int vgui_dock_get_zone_count(vgui_widget* dock, vgui_dock_zone zone)
	{
		vgui_dock_state* st = vgui_dock_of(dock);
		if (!st) return 0;
		int z = (int)zone;
		if (z == VGUI_DOCK_FLOATING) return (int)st->float_order.size();
		if (z < 0 || z >= 4) return 0;
		return (int)st->order[z].size();
	}

	vgui_item_id vgui_dock_get_zone_at(vgui_widget* dock, vgui_dock_zone zone, int index)
	{
		vgui_dock_state* st = vgui_dock_of(dock);
		if (!st) return 0;
		int z = (int)zone;
		if (z == VGUI_DOCK_FLOATING)
		{
			if (index < 0 || index >= (int)st->float_order.size()) return 0;
			return st->float_order[(size_t)index];
		}
		if (z < 0 || z >= 4 || index < 0 || index >= (int)st->order[z].size()) return 0;
		return st->order[z][(size_t)index];
	}

	vgui_dock_zone vgui_dock_get_zone(vgui_widget* dock, vgui_item_id panel_id)
	{
		vgui_dock_panel* panel = vgui_dock_find(dock, panel_id);
		return panel ? panel->zone : VGUI_DOCK_CENTER;
	}

	int vgui_dock_get_index(vgui_widget* dock, vgui_item_id panel_id)
	{
		vgui_dock_state* st = vgui_dock_of(dock);
		vgui_dock_panel* panel = vgui_dock_find(dock, panel_id);
		if (!st || !panel) return -1;
		if (panel->zone == VGUI_DOCK_FLOATING)
		{
			for (size_t i = 0; i < st->float_order.size(); ++i)
				if (st->float_order[i] == panel_id) return (int)i;
			return -1;
		}
		int z = (int)panel->zone;
		if (z < 0 || z >= 4) return -1;
		for (size_t i = 0; i < st->order[z].size(); ++i)
			if (st->order[z][i] == panel_id) return (int)i;
		return -1;
	}

	int vgui_dock_set_zone(vgui_widget* dock, vgui_item_id panel_id, vgui_dock_zone zone, int index)
	{
		if (!vgui_dock_find(dock, panel_id)) return 0;
		int z = (int)zone;
		if (z < 0 || z > (int)VGUI_DOCK_FLOATING) return 0;
		return vgui_dock_move_internal(dock, panel_id, z, index, true, true) ? 1 : 0;
	}

	vgui_item_id vgui_dock_get_active(vgui_widget* dock, vgui_dock_zone zone)
	{
		vgui_dock_state* st = vgui_dock_of(dock);
		if (!st) return 0;
		return vgui_dock_pick_active(st, (int)zone);
	}

	int vgui_dock_activate(vgui_widget* dock, vgui_item_id panel_id)
	{
		return vgui_dock_activate_internal(dock, panel_id, true) ? 1 : 0;
	}

	int vgui_dock_float(vgui_widget* dock, vgui_item_id panel_id, float x, float y,
		float width, float height)
	{
		return vgui_dock_float_internal(dock, panel_id, x, y, width, height, true, true) ? 1 : 0;
	}

	int vgui_dock_is_floating(vgui_widget* dock, vgui_item_id panel_id)
	{
		vgui_dock_panel* panel = vgui_dock_find(dock, panel_id);
		return (panel && panel->zone == VGUI_DOCK_FLOATING) ? 1 : 0;
	}

	int vgui_dock_get_floating_rect(vgui_widget* dock, vgui_item_id panel_id,
		float* x, float* y, float* width, float* height)
	{
		vgui_dock_panel* panel = vgui_dock_find(dock, panel_id);
		if (!panel || panel->zone != VGUI_DOCK_FLOATING) return 0;
		vgui_dock_float_rect(dock, *panel, x, y, width, height);
		return 1;
	}

	int vgui_dock_set_floating_rect(vgui_widget* dock, vgui_item_id panel_id,
		float x, float y, float width, float height)
	{
		vgui_dock_panel* panel = vgui_dock_find(dock, panel_id);
		if (!panel || panel->zone != VGUI_DOCK_FLOATING) return 0;
		vgui_dock_clamp_rect(dock, panel, x, y, width, height, true);
		bool raised = vgui_dock_raise_float_internal(dock, panel_id);
		vgui_mark_dirty(dock);
		if (raised && vgui_dock_alive(dock)) vgui_dock_notify_activate(dock, panel_id);
		if (vgui_dock_alive(dock)) vgui_dock_notify_change(dock);
		return 1;
	}

	int vgui_dock_close(vgui_widget* dock, vgui_item_id panel_id)
	{
		return vgui_dock_close_internal(dock, panel_id, true) ? 1 : 0;
	}

	int vgui_dock_restore(vgui_widget* dock, vgui_item_id panel_id)
	{
		return vgui_dock_restore_internal(dock, panel_id, true) ? 1 : 0;
	}

	int vgui_dock_is_open(vgui_widget* dock, vgui_item_id panel_id)
	{
		vgui_dock_panel* panel = vgui_dock_find(dock, panel_id);
		return panel ? panel->open : 0;
	}

	void vgui_dock_set_zone_size(vgui_widget* dock, vgui_dock_zone zone, float size)
	{
		vgui_dock_state* st = vgui_dock_of(dock);
		int z = (int)zone;
		if (!st || z < 0 || z > (int)VGUI_DOCK_BOTTOM) return;
		st->zone_size[z] = size < 0.0f ? -1.0f : size;
		for (int i = 0; i < 4; ++i) st->scroll_active[i] = 1;
		vgui_mark_dirty(dock);
		if (vgui_dock_alive(dock)) vgui_dock_notify_change(dock);
	}

	float vgui_dock_get_zone_size(vgui_widget* dock, vgui_dock_zone zone)
	{
		vgui_dock_state* st = vgui_dock_of(dock);
		int z = (int)zone;
		if (!st || z < 0 || z > (int)VGUI_DOCK_BOTTOM) return 0.0f;
		return vgui_dock_resolved_zone_size(dock, z);
	}

	int vgui_dock_get_zone_rect(vgui_widget* dock, vgui_dock_zone zone,
		float* x, float* y, float* width, float* height)
	{
		vgui_dock_state* st = vgui_dock_of(dock);
		if (!st) return 0;
		if (dock && dock->ctx) vgui_ensure_layout(dock->ctx);
		int z = (int)zone;
		if (z < 0 || z >= 4) return 0;
		if (st->zone_w[z] <= 0.0f || st->zone_h[z] <= 0.0f) return 0;
		if (x) *x = st->zone_x[z];
		if (y) *y = st->zone_y[z];
		if (width) *width = st->zone_w[z];
		if (height) *height = st->zone_h[z];
		return 1;
	}

	int vgui_dock_get_tab_rect(vgui_widget* dock, vgui_item_id panel_id,
		float* x, float* y, float* width, float* height)
	{
		vgui_dock_state* st = vgui_dock_of(dock);
		vgui_dock_panel* panel = vgui_dock_find(dock, panel_id);
		if (!st || !panel || !panel->open) return 0;
		if (dock && dock->ctx) vgui_ensure_layout(dock->ctx);
		if (panel->zone == VGUI_DOCK_FLOATING)
		{
			float title_h = 0.0f;
			if (dock && dock->ctx) title_h = dock->ctx->style.dock.floating_title_height;
			title_h = std::min(title_h, panel->fh);
			if (x) *x = panel->fx;
			if (y) *y = panel->fy;
			if (width) *width = panel->fw;
			if (height) *height = title_h;
			return 1;
		}
		int z = (int)panel->zone;
		if (z < 0 || z >= 4) return 0;
		for (const vgui_dock_tab_geom& tab : st->tabs[z])
		{
			if (tab.id != panel_id) continue;
			if (x) *x = tab.x;
			if (y) *y = tab.y;
			if (width) *width = tab.w;
			if (height) *height = tab.h;
			return 1;
		}
		return 0;
	}

	void vgui_dock_on_close(vgui_widget* dock, vgui_item_fn fn, void* user)
	{
		vgui_dock_state* st = vgui_dock_of(dock);
		if (!st) return;
		st->close_fn = fn;
		st->close_user = user;
	}

	void vgui_dock_on_activate(vgui_widget* dock, vgui_item_fn fn, void* user)
	{
		vgui_dock_state* st = vgui_dock_of(dock);
		if (!st) return;
		st->activate_fn = fn;
		st->activate_user = user;
	}

	void vgui_dock_on_change(vgui_widget* dock, vgui_item_fn fn, void* user)
	{
		vgui_dock_state* st = vgui_dock_of(dock);
		if (!st) return;
		st->change_fn = fn;
		st->change_user = user;
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
		vgui_edit_invalidate_content_width(edit);
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
		vgui_edit_invalidate_content_width(edit);
		vgui_edit_clamp_scroll(edit);
		vgui_mark_dirty(edit);
	}

	void vgui_edit_set_multiline(vgui_widget* edit, int multiline)
	{
		if (!edit) return;
		edit->multiline = multiline ? 1 : 0;
		vgui_edit_invalidate_content_width(edit);
		vgui_edit_clamp_scroll(edit);
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

	void vgui_edit_set_caret_width(vgui_widget* edit, float width)
	{
		if (!edit || edit->kind != VGUI_EDIT) return;
		edit->caret_width = std::max(0.0f, width);
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
	// list model
	//----------------------------------------------------------------------------

	// 虚拟数据源：可选。设置后行数据由应用提供，列表只查询可见范围。
	void vgui_list_set_row_source(vgui_widget* list, const vgui_row_source* source)
	{
		vgui_list_state* st = vgui_list_of(list);
		if (!st) return;
		const bool was_virtual = st->virtual_mode;
		if (source && source->row_count && source->row_info && source->row_id)
		{
			st->source = *source;
			st->virtual_mode = true;
			st->items.clear();
			st->index.clear();
		}
		else
		{
			st->source = {};
			st->virtual_mode = false;
		}
		// 数据源变化后行数、最宽行和选择都可能失效。
		st->content_w_valid = false;
		if (was_virtual != st->virtual_mode)
		{
			st->selected.clear();
			st->cursor = 0;
			st->hover = 0;
			st->pressed = 0;
			st->range_anchor = 0;
			st->scratch_item = vgui_list_item();
		}
		vgui_mark_dirty(list);
	}

	int vgui_list_is_virtual(vgui_widget* list)
	{
		vgui_list_state* st = vgui_list_of(list);
		return st && st->virtual_mode ? 1 : 0;
	}

	void vgui_list_source_changed(vgui_widget* list)
	{
		vgui_list_state* st = vgui_list_of(list);
		if (!st) return;
		st->content_w_valid = false;
		vgui_mark_dirty(list);
	}

	// F04 细粒度失效：可见行数与顺序未变（只是内容变了），不重建行投影。
	void vgui_list_source_rows_changed(vgui_widget* list, int first, int count)
	{
		(void)first;
		(void)count;
		vgui_list_state* st = vgui_list_of(list);
		if (!st || !st->virtual_mode) return;
		st->content_w_valid = false;
		vgui_mark_dirty(list);
	}

	static vgui_item_id vgui_next_item_id(vgui_item_id& next)
	{
		if (next == 0) next = 1;
		vgui_item_id id = next++;
		if (next == 0) next = 1;
		return id;
	}

	vgui_item_id vgui_list_add_item(vgui_widget* list, const char* text, void* user)
	{
		vgui_list_state* st = vgui_list_of(list);
		if (!st || st->virtual_mode) return 0;
		vgui_list_item item;
		item.id = vgui_next_item_id(st->next_id);
		if (text) item.text = text;
		item.user = user;
		st->items.push_back(std::move(item));
		vgui_item_id id = st->items.back().id;
		st->index[id] = st->items.size() - 1;
		st->content_w_valid = false;
		vgui_mark_dirty(list);
		return id;
	}

	vgui_item_id vgui_list_insert_item(vgui_widget* list, int index, const char* text, void* user)
	{
		vgui_list_state* st = vgui_list_of(list);
		if (!st || st->virtual_mode) return 0;
		if (index < 0) index = 0;
		if (index > (int)st->items.size()) index = (int)st->items.size();
		vgui_list_item item;
		item.id = vgui_next_item_id(st->next_id);
		if (text) item.text = text;
		item.user = user;
		st->items.insert(st->items.begin() + index, std::move(item));
		vgui_list_update_index_from(st, (size_t)index);
		st->content_w_valid = false;
		vgui_mark_dirty(list);
		return st->items[(size_t)index].id;
	}

	int vgui_list_remove_item(vgui_widget* list, vgui_item_id item)
	{
		vgui_list_state* st = vgui_list_of(list);
		size_t index = (size_t)-1;
		if (!st || st->virtual_mode || !vgui_list_item_find(list, item, &index)) return 0;
		if (vgui_id_selected(st->selected, item))
		{
			st->selected.erase(std::remove(st->selected.begin(), st->selected.end(), item), st->selected.end());
			vgui_list_notify_select(list, item, 0);
			if (!list->ctx || !list->ctx->alive.count(list)) return 1;
			// The callback may have removed this item (or cleared the model);
			// refresh the index instead of using the stale pre-callback one.
			st = vgui_list_of(list);
			if (!st || !vgui_list_item_find(list, item, &index)) return 1;
		}
		st->items.erase(st->items.begin() + (ptrdiff_t)index);
		if (st->cursor == item) st->cursor = 0;
		if (st->hover == item) st->hover = 0;
		if (st->pressed == item) st->pressed = 0;
		if (st->range_anchor == item) st->range_anchor = 0;
		st->index.erase(item);
		vgui_list_update_index_from(st, index);
		st->content_w_valid = false;
		vgui_mark_dirty(list);
		return 1;
	}

	void vgui_list_clear(vgui_widget* list)
	{
		vgui_list_state* st = vgui_list_of(list);
		if (!st || st->virtual_mode) return;
		vgui_list_clear_selection_internal(list, true);
		if (!list->ctx || !list->ctx->alive.count(list)) return;
		st = vgui_list_of(list);
		if (!st) return;
		st->items.clear();
		st->index.clear();
		st->selected.clear();
		st->cursor = 0;
		st->hover = 0;
		st->pressed = 0;
		st->range_anchor = 0;
		st->content_w_valid = false;
		vgui_mark_dirty(list);
	}

	int vgui_list_get_count(vgui_widget* list)
	{
		vgui_list_state* st = vgui_list_of(list);
		return vgui_list_row_count(list, st);
	}

	vgui_item_id vgui_list_get_item_at(vgui_widget* list, int index)
	{
		vgui_item_id id = 0;
		vgui_list_item_at(list, index, &id);
		return id;
	}

	vgui_item_id vgui_list_item_at_point(vgui_widget* list, float local_x, float local_y)
	{
		vgui_list_state* st = vgui_list_of(list);
		if (!st) return 0;
		vgui_ensure_layout(list->ctx);
		int index = vgui_list_index_at(list, list->abs_x + local_x, list->abs_y + local_y);
		if (index < 0) return 0;
		return vgui_list_row_id(list, st, index);
	}

	void vgui_list_set_item_text(vgui_widget* list, vgui_item_id item, const char* text)
	{
		vgui_list_state* st = vgui_list_of(list);
		if (!st || st->virtual_mode) return;
		vgui_list_item* it = vgui_list_item_find(list, item);
		if (!it) return;
		it->text = text ? text : "";
		st->content_w_valid = false;
		vgui_mark_dirty(list);
	}

	const char* vgui_list_get_item_text(vgui_widget* list, vgui_item_id item)
	{
		vgui_list_item* it = vgui_list_item_find(list, item);
		return it ? it->text.c_str() : "";
	}

	void vgui_list_set_item_detail(vgui_widget* list, vgui_item_id item, const char* detail)
	{
		vgui_list_state* st = vgui_list_of(list);
		if (!st || st->virtual_mode) return;
		vgui_list_item* it = vgui_list_item_find(list, item);
		if (!it) return;
		it->detail = detail ? detail : "";
		st->content_w_valid = false;
		vgui_mark_dirty(list);
	}

	const char* vgui_list_get_item_detail(vgui_widget* list, vgui_item_id item)
	{
		vgui_list_item* it = vgui_list_item_find(list, item);
		return it ? it->detail.c_str() : "";
	}

	void vgui_list_set_item_tooltip(vgui_widget* list, vgui_item_id item, const char* tooltip)
	{
		vgui_list_state* st = vgui_list_of(list);
		if (!st || st->virtual_mode) return;
		vgui_list_item* it = vgui_list_item_find(list, item);
		if (!it) return;
		it->tooltip = tooltip ? tooltip : "";
		if (list->ctx) vgui_tooltip_refresh(list->ctx, list, item);
	}

	const char* vgui_list_get_item_tooltip(vgui_widget* list, vgui_item_id item)
	{
		vgui_list_item* it = vgui_list_item_find(list, item);
		return it ? it->tooltip.c_str() : "";
	}

	void vgui_list_set_item_enabled(vgui_widget* list, vgui_item_id item, int enabled)
	{
		vgui_list_state* st = vgui_list_of(list);
		if (!st || st->virtual_mode) return;
		vgui_list_item* it = vgui_list_item_find(list, item);
		if (!it) return;
		it->enabled = enabled ? 1 : 0;
		vgui_mark_dirty(list);
	}

	int vgui_list_get_item_enabled(vgui_widget* list, vgui_item_id item)
	{
		vgui_list_item* it = vgui_list_item_find(list, item);
		return it ? it->enabled : 0;
	}

	void vgui_list_set_item_user(vgui_widget* list, vgui_item_id item, void* user)
	{
		vgui_list_state* st = vgui_list_of(list);
		if (!st || st->virtual_mode) return;
		vgui_list_item* it = vgui_list_item_find(list, item);
		if (it) it->user = user;
	}

	void* vgui_list_get_item_user(vgui_widget* list, vgui_item_id item)
	{
		vgui_list_item* it = vgui_list_item_find(list, item);
		return it ? it->user : nullptr;
	}

	// ---- F04 行扩展（icon / text color / inline buttons） ---------------------
	// 全量模式写入 item 字段；虚拟模式下 setter 是 no-op（数据由行源提供），
	// getter 经 materialize 读取回调结果。

	void vgui_list_set_item_icon(vgui_widget* list, vgui_item_id item, vgui_texture* icon)
	{
		vgui_list_state* st = vgui_list_of(list);
		if (!st || st->virtual_mode) return;
		vgui_list_item* it = vgui_list_item_find(list, item);
		if (!it || it->icon == icon) return;
		it->icon = icon;
		vgui_mark_dirty(list);
	}

	vgui_texture* vgui_list_get_item_icon(vgui_widget* list, vgui_item_id item)
	{
		vgui_list_item* it = vgui_list_item_find(list, item);
		return it ? it->icon : nullptr;
	}

	void vgui_list_set_item_text_color(vgui_widget* list, vgui_item_id item, vgui_color color)
	{
		vgui_list_state* st = vgui_list_of(list);
		if (!st || st->virtual_mode) return;
		vgui_list_item* it = vgui_list_item_find(list, item);
		if (!it || it->text_color == color) return;
		it->text_color = color;
		vgui_mark_dirty(list);
	}

	vgui_color vgui_list_get_item_text_color(vgui_widget* list, vgui_item_id item)
	{
		vgui_list_item* it = vgui_list_item_find(list, item);
		return it ? it->text_color : 0;
	}

	void vgui_list_set_item_buttons(vgui_widget* list, vgui_item_id item, int count)
	{
		vgui_list_state* st = vgui_list_of(list);
		if (!st || st->virtual_mode) return;
		vgui_list_item* it = vgui_list_item_find(list, item);
		if (!it) return;
		count = std::min(VGUI_MAX_ROW_BUTTONS, std::max(0, count));
		if (it->button_count == count) return;
		it->button_count = count;
		it->button_icons.assign((size_t)count, nullptr);
		vgui_mark_dirty(list);
	}

	int vgui_list_get_item_buttons(vgui_widget* list, vgui_item_id item)
	{
		vgui_list_item* it = vgui_list_item_find(list, item);
		return it ? it->button_count : 0;
	}

	void vgui_list_set_item_button_icon(vgui_widget* list, vgui_item_id item, int button, vgui_texture* icon)
	{
		vgui_list_state* st = vgui_list_of(list);
		if (!st || st->virtual_mode) return;
		vgui_list_item* it = vgui_list_item_find(list, item);
		if (!it || button < 0 || button >= it->button_count) return;
		if ((int)it->button_icons.size() <= button)
			it->button_icons.resize((size_t)button + 1, nullptr);
		it->button_icons[(size_t)button] = icon;
		vgui_mark_dirty(list);
	}

	vgui_texture* vgui_list_get_item_button_icon(vgui_widget* list, vgui_item_id item, int button)
	{
		vgui_list_item* it = vgui_list_item_find(list, item);
		if (!it || button < 0) return nullptr;
		return button < (int)it->button_icons.size() ? it->button_icons[(size_t)button] : nullptr;
	}

	void vgui_list_on_item_button(vgui_widget* list, vgui_item_button_fn fn, void* user)
	{
		vgui_list_state* st = vgui_list_of(list);
		if (!st) return;
		st->button_fn = fn;
		st->button_user = user;
	}

	void vgui_list_set_selection_mode(vgui_widget* list, vgui_selection_mode mode)
	{
		vgui_list_state* st = vgui_list_of(list);
		if (!st) return;
		if (mode != VGUI_SELECTION_NONE && mode != VGUI_SELECTION_SINGLE && mode != VGUI_SELECTION_MULTIPLE)
			mode = VGUI_SELECTION_SINGLE;
		st->selection_mode = mode;
		if (mode == VGUI_SELECTION_NONE)
			vgui_list_clear_selection_internal(list, true);
		else if (mode == VGUI_SELECTION_SINGLE && st->selected.size() > 1)
		{
			std::vector<vgui_item_id> old = st->selected;
			st->selected.clear();
			vgui_item_id keep = old.front();
			st->selected.push_back(keep);
			for (size_t i = 1; i < old.size(); ++i)
			{
				vgui_list_notify_select(list, old[i], 0);
				if (!list->ctx || !list->ctx->alive.count(list)) return;
			}
		}
	}

	vgui_selection_mode vgui_list_get_selection_mode(vgui_widget* list)
	{
		vgui_list_state* st = vgui_list_of(list);
		return st ? st->selection_mode : VGUI_SELECTION_NONE;
	}

	int vgui_list_select(vgui_widget* list, vgui_item_id item, int selected)
	{
		return vgui_list_set_selected_internal(list, item, selected != 0, true) ? 1 : 0;
	}

	void vgui_list_clear_selection(vgui_widget* list)
	{
		vgui_list_clear_selection_internal(list, true);
	}

	int vgui_list_is_selected(vgui_widget* list, vgui_item_id item)
	{
		vgui_list_state* st = vgui_list_of(list);
		return st && vgui_id_selected(st->selected, item) ? 1 : 0;
	}

	int vgui_list_get_selected_count(vgui_widget* list)
	{
		vgui_list_state* st = vgui_list_of(list);
		return st ? (int)st->selected.size() : 0;
	}

	vgui_item_id vgui_list_get_selected(vgui_widget* list, int index)
	{
		vgui_list_state* st = vgui_list_of(list);
		if (!st || index < 0 || index >= (int)st->selected.size()) return 0;
		return st->selected[(size_t)index];
	}

	void vgui_list_set_cursor(vgui_widget* list, vgui_item_id item)
	{
		vgui_list_state* st = vgui_list_of(list);
		if (!st) return;
		st->cursor = vgui_list_item_find(list, item) ? item : 0;
		vgui_mark_dirty(list);
	}

	vgui_item_id vgui_list_get_cursor(vgui_widget* list)
	{
		vgui_list_state* st = vgui_list_of(list);
		return st ? st->cursor : 0;
	}

	void vgui_list_scroll_to_item(vgui_widget* list, vgui_item_id item, vgui_scroll_align align)
	{
		if (list) vgui_ensure_layout(list->ctx);
		vgui_list_ensure_visible(list, item, align);
	}

	void vgui_list_set_empty_text(vgui_widget* list, const char* text)
	{
		vgui_list_state* st = vgui_list_of(list);
		if (!st) return;
		st->empty_text = text ? text : "";
		vgui_mark_dirty(list);
	}

	void vgui_list_on_select(vgui_widget* list, vgui_item_select_fn fn, void* user)
	{
		vgui_list_state* st = vgui_list_of(list);
		if (!st) return;
		st->select_fn = fn;
		st->select_user = user;
	}

	void vgui_list_on_activate(vgui_widget* list, vgui_item_fn fn, void* user)
	{
		vgui_list_state* st = vgui_list_of(list);
		if (!st) return;
		st->activate_fn = fn;
		st->activate_user = user;
	}

	//----------------------------------------------------------------------------
	// tree model
	//----------------------------------------------------------------------------

	// 虚拟数据源：树的行是"可见行投影"，展开状态由应用维护。
	void vgui_tree_set_row_source(vgui_widget* tree, const vgui_row_source* source)
	{
		vgui_tree_state* st = vgui_tree_of(tree);
		if (!st) return;
		const bool was_virtual = st->virtual_mode;
		if (source && source->row_count && source->row_info && source->row_id)
		{
			st->source = *source;
			st->virtual_mode = true;
			st->nodes.clear();
			st->roots.clear();
			st->index.clear();
			st->rows.clear();
			st->row_index.clear();
		}
		else
		{
			st->source = {};
			st->virtual_mode = false;
			st->rows.clear();
			st->row_index.clear();
		}
		st->rows_dirty = true;
		st->content_w_valid = false;
		if (was_virtual != st->virtual_mode)
		{
			st->selected.clear();
			st->cursor = 0;
			st->hover = 0;
			st->pressed = 0;
			st->range_anchor = 0;
			st->scratch_node = vgui_tree_node();
		}
		vgui_mark_dirty(tree);
	}

	int vgui_tree_is_virtual(vgui_widget* tree)
	{
		vgui_tree_state* st = vgui_tree_of(tree);
		return st && st->virtual_mode ? 1 : 0;
	}

	void vgui_tree_source_changed(vgui_widget* tree)
	{
		vgui_tree_state* st = vgui_tree_of(tree);
		if (!st) return;
		st->rows_dirty = true;
		st->content_w_valid = false;
		vgui_mark_dirty(tree);
	}

	// F04 细粒度失效：不置 rows_dirty，避免整棵可见行投影重建。
	void vgui_tree_source_rows_changed(vgui_widget* tree, int first, int count)
	{
		(void)first;
		(void)count;
		vgui_tree_state* st = vgui_tree_of(tree);
		if (!st || !st->virtual_mode) return;
		st->content_w_valid = false;
		vgui_mark_dirty(tree);
	}

	// ---- 行过滤 / 行矩形（O04/O02）-------------------------------------------

	void vgui_tree_set_filter(vgui_widget* tree, vgui_tree_filter_fn fn, void* user)
	{
		vgui_tree_state* st = vgui_tree_of(tree);
		if (!st) return;
		st->filter_fn = fn;
		st->filter_user = user;
		st->rows_dirty = true;
		st->content_w_valid = false;
		vgui_mark_dirty(tree);
	}

	void vgui_tree_set_match_text(vgui_widget* tree, const char* text)
	{
		vgui_tree_state* st = vgui_tree_of(tree);
		if (!st) return;
		st->match_text = text ? text : "";
		vgui_mark_dirty(tree);
	}

	void vgui_tree_set_match_color(vgui_widget* tree, vgui_color color)
	{
		vgui_tree_state* st = vgui_tree_of(tree);
		if (!st) return;
		st->match_color = color;
		vgui_mark_dirty(tree);
	}

	int vgui_tree_get_item_rect(vgui_widget* tree, vgui_item_id node,
		float* x, float* y, float* w, float* h)
	{
		vgui_tree_state* st = vgui_tree_of(tree);
		if (!st || !tree || node == 0 || !x || !y || !w || !h) return 0;
		const int index = vgui_tree_visible_index(tree, node);
		if (index < 0) return 0;
		const float row_h = vgui_tree_row_height(tree);
		*x = tree->abs_x + tree->padding[0];
		*w = std::max(0.0f, tree->abs_w - tree->padding[0] - tree->padding[2]);
		*y = tree->abs_y + tree->padding[1] - tree->scroll_y + (float)index * row_h;
		*h = row_h;
		return 1;
	}

	// ---- F03 通用拖放 ---------------------------------------------------------

	int vgui_begin_drag(vgui_context* ctx, vgui_widget* source, const vgui_drag_payload* payload)
	{
		if (!ctx || !payload || payload->kind == VGUI_DRAG_NONE)
			return 0;
		if (ctx->drag_kind != VGUI_DRAG_NONE)
			return 0;
		ctx->drag_kind = payload->kind;
		vgui_drag_setup_payload(ctx, payload);
		ctx->drag_source = source;
		ctx->drop_hover = nullptr;
		ctx->drop_accepted = 0;
		ctx->drop_hint = {};
		ctx->cursor = VGUI_CURSOR_NO;
		vgui_tooltip_suppress(ctx);
		return 1;
	}

	void vgui_cancel_drag(vgui_context* ctx)
	{
		if (!ctx || ctx->drag_kind == VGUI_DRAG_NONE) return;
		vgui_drag_end(ctx);
		ctx->cursor = VGUI_CURSOR_ARROW;
	}

	int vgui_drag_active(vgui_context* ctx)
	{
		return ctx && ctx->drag_kind != VGUI_DRAG_NONE ? 1 : 0;
	}

	vgui_widget* vgui_drag_hover_target(vgui_context* ctx)
	{
		return ctx && ctx->drop_accepted ? ctx->drop_hover : nullptr;
	}

	void vgui_widget_set_drop_target(vgui_widget* widget, const vgui_drop_target* target)
	{
		if (widget)
			widget->drop_target = target;
	}

	void vgui_list_set_row_drag(vgui_widget* list, vgui_row_drag_fn fn, void* user)
	{
		vgui_list_state* st = vgui_list_of(list);
		if (!st) return;
		st->row_drag_fn = fn;
		st->row_drag_user = user;
		if (!fn)
			st->drag_pending = 0;
	}

	void vgui_tree_set_row_drag(vgui_widget* tree, vgui_row_drag_fn fn, void* user)
	{
		vgui_tree_state* st = vgui_tree_of(tree);
		if (!st) return;
		st->row_drag_fn = fn;
		st->row_drag_user = user;
		if (!fn)
			st->drag_pending = 0;
	}

	int vgui_input_drop_files(vgui_context* ctx, const char* const* files,
		int count, float x, float y)
	{
		if (!ctx || !files || count <= 0) return 0;
		if (ctx->drag_kind != VGUI_DRAG_NONE) return 0;

		vgui_drag_payload payload = {};
		payload.kind = VGUI_DRAG_FILES;
		payload.files = files;
		payload.file_count = count;
		ctx->drag_kind = VGUI_DRAG_FILES;
		vgui_drag_setup_payload(ctx, &payload);

		// 一次性 hover + drop：命中点向上找 target，接受则直接交付。
		vgui_widget* target = vgui_drop_target_at(ctx, x, y);
		bool accepted = false;
		if (target && target->drop_target && target->drop_target->can_accept)
			accepted = target->drop_target->can_accept(target->drop_target->user,
				&ctx->drag_payload) != 0;
		if (accepted && target->drop_target->hover)
		{
			vgui_drop_hint hint = {};
			target->drop_target->hover(target->drop_target->user, &ctx->drag_payload,
				x, y, &hint);
		}
		bool handled = false;
		if (accepted && target->drop_target->drop)
			handled = target->drop_target->drop(target->drop_target->user,
				&ctx->drag_payload, x, y) != 0;
		vgui_drag_end(ctx);
		return handled ? 1 : 0;
	}

	vgui_item_id vgui_tree_add_node(vgui_widget* tree, vgui_item_id parent, const char* text, void* user)
	{
		vgui_tree_state* st = vgui_tree_of(tree);
		if (!st || st->virtual_mode) return 0;
		if (parent != 0 && !vgui_tree_node_find(tree, parent)) return 0;
		vgui_tree_node node;
		vgui_item_id id = vgui_next_item_id(st->next_id);
		node.id = id;
		node.parent = parent;
		if (text) node.text = text;
		node.user = user;
		st->nodes.push_back(std::move(node));
		// 追加只影响新节点自身的位置，无需重建整张 id->index 表。
		st->index[id] = st->nodes.size() - 1;
		if (parent == 0)
			st->roots.push_back(id);
		else
		{
			vgui_tree_node* p = vgui_tree_node_find(tree, parent);
			if (p) p->children.push_back(id);
		}
		st->rows_dirty = true;
		vgui_mark_dirty(tree);
		return id;
	}

	int vgui_tree_remove_node(vgui_widget* tree, vgui_item_id node)
	{
		vgui_tree_state* st = vgui_tree_of(tree);
		if (!st || st->virtual_mode || !vgui_tree_node_find(tree, node)) return 0;
		std::unordered_set<vgui_item_id> doomed;
		std::vector<vgui_item_id> stack;
		stack.push_back(node);
		while (!stack.empty())
		{
			vgui_item_id id = stack.back();
			stack.pop_back();
			if (id == 0 || doomed.count(id)) continue;
			vgui_tree_node* n = vgui_tree_node_find(tree, id);
			if (!n) continue;
			doomed.insert(id);
			for (vgui_item_id c : n->children) stack.push_back(c);
		}
		for (vgui_item_id id : doomed)
			if (vgui_id_selected(st->selected, id))
			{
				st->selected.erase(std::remove(st->selected.begin(), st->selected.end(), id), st->selected.end());
				vgui_tree_notify_select(tree, id, 0);
				if (!tree->ctx || !tree->ctx->alive.count(tree)) return 1;
				st = vgui_tree_of(tree);
				if (!st) return 1;
			}
		for (vgui_tree_node& n : st->nodes)
			n.children.erase(std::remove_if(n.children.begin(), n.children.end(),
				[&](vgui_item_id id) { return doomed.count(id) != 0; }), n.children.end());
		st->nodes.erase(std::remove_if(st->nodes.begin(), st->nodes.end(),
			[&](const vgui_tree_node& n) { return doomed.count(n.id) != 0; }), st->nodes.end());
		vgui_tree_rebuild_index(st);
		st->roots.clear();
		for (const vgui_tree_node& n : st->nodes)
			if (n.parent == 0) st->roots.push_back(n.id);
		if (doomed.count(st->cursor)) st->cursor = 0;
		if (doomed.count(st->hover)) st->hover = 0;
		if (doomed.count(st->pressed)) st->pressed = 0;
		if (doomed.count(st->range_anchor)) st->range_anchor = 0;
		st->rows_dirty = true;
		vgui_mark_dirty(tree);
		return 1;
	}

	void vgui_tree_clear(vgui_widget* tree)
	{
		vgui_tree_state* st = vgui_tree_of(tree);
		if (!st || st->virtual_mode) return;
		vgui_tree_clear_selection_internal(tree, true);
		if (!tree->ctx || !tree->ctx->alive.count(tree)) return;
		st = vgui_tree_of(tree);
		if (!st) return;
		st->nodes.clear();
		st->roots.clear();
		st->index.clear();
		st->rows.clear();
		st->row_index.clear();
		st->rows_dirty = true;
		st->selected.clear();
		st->cursor = 0;
		st->hover = 0;
		st->pressed = 0;
		st->range_anchor = 0;
		vgui_mark_dirty(tree);
	}

	int vgui_tree_get_node_count(vgui_widget* tree)
	{
		vgui_tree_state* st = vgui_tree_of(tree);
		if (!st) return 0;
		// 虚拟模式下行数即可见行数，节点计数没有独立含义。
		if (st->virtual_mode) return vgui_tree_row_count(tree, st);
		return (int)st->nodes.size();
	}

	int vgui_tree_get_root_count(vgui_widget* tree)
	{
		vgui_tree_state* st = vgui_tree_of(tree);
		if (!st || st->virtual_mode) return 0;	// 虚拟模式无层级信息
		return (int)st->roots.size();
	}

	vgui_item_id vgui_tree_get_root(vgui_widget* tree, int index)
	{
		vgui_tree_state* st = vgui_tree_of(tree);
		if (!st || st->virtual_mode || index < 0 || index >= (int)st->roots.size()) return 0;
		return st->roots[(size_t)index];
	}

	int vgui_tree_get_child_count(vgui_widget* tree, vgui_item_id node)
	{
		vgui_tree_state* st = vgui_tree_of(tree);
		if (st && st->virtual_mode) return 0;
		vgui_tree_node* n = vgui_tree_node_find(tree, node);
		return n ? (int)n->children.size() : 0;
	}

	vgui_item_id vgui_tree_get_child(vgui_widget* tree, vgui_item_id node, int index)
	{
		vgui_tree_state* st = vgui_tree_of(tree);
		if (st && st->virtual_mode) return 0;
		vgui_tree_node* n = vgui_tree_node_find(tree, node);
		if (!n || index < 0 || index >= (int)n->children.size()) return 0;
		return n->children[(size_t)index];
	}

	vgui_item_id vgui_tree_get_parent(vgui_widget* tree, vgui_item_id node)
	{
		vgui_tree_state* st = vgui_tree_of(tree);
		if (st && st->virtual_mode) return 0;
		vgui_tree_node* n = vgui_tree_node_find(tree, node);
		return n ? n->parent : 0;
	}

	int vgui_tree_get_depth(vgui_widget* tree, vgui_item_id node)
	{
		vgui_tree_state* st = vgui_tree_of(tree);
		if (st && st->virtual_mode)
		{
			int index = vgui_tree_visible_index(tree, node);
			return index < 0 ? 0 : vgui_tree_row_depth_at(tree, st, index);
		}
		vgui_tree_node* n = vgui_tree_node_find(tree, node);
		int depth = 0;
		while (n && n->parent)
		{
			depth++;
			n = vgui_tree_node_find(tree, n->parent);
		}
		return depth;
	}

	void vgui_tree_set_item_text(vgui_widget* tree, vgui_item_id node, const char* text)
	{
		vgui_tree_state* st = vgui_tree_of(tree);
		if (!st || st->virtual_mode) return;
		vgui_tree_node* n = vgui_tree_node_find(tree, node);
		if (!n) return;
		n->text = text ? text : "";
		if (st) st->content_w_valid = false;
		vgui_mark_dirty(tree);
	}

	const char* vgui_tree_get_item_text(vgui_widget* tree, vgui_item_id node)
	{
		vgui_tree_node* n = vgui_tree_node_find(tree, node);
		return n ? n->text.c_str() : "";
	}

	void vgui_tree_set_item_tooltip(vgui_widget* tree, vgui_item_id node, const char* tooltip)
	{
		vgui_tree_state* st = vgui_tree_of(tree);
		if (!st || st->virtual_mode) return;
		vgui_tree_node* n = vgui_tree_node_find(tree, node);
		if (!n) return;
		n->tooltip = tooltip ? tooltip : "";
		if (tree->ctx) vgui_tooltip_refresh(tree->ctx, tree, node);
	}

	const char* vgui_tree_get_item_tooltip(vgui_widget* tree, vgui_item_id node)
	{
		vgui_tree_node* n = vgui_tree_node_find(tree, node);
		return n ? n->tooltip.c_str() : "";
	}

	vgui_item_id vgui_tree_item_at_point(vgui_widget* tree, float local_x, float local_y)
	{
		vgui_tree_state* st = vgui_tree_of(tree);
		if (!st) return 0;
		vgui_ensure_layout(tree->ctx);
		int index = vgui_tree_row_at(tree, tree->abs_x + local_x, tree->abs_y + local_y);
		if (index < 0) return 0;
		return vgui_tree_row_id_at(tree, st, index);
	}

	void vgui_tree_set_item_enabled(vgui_widget* tree, vgui_item_id node, int enabled)
	{
		vgui_tree_state* st = vgui_tree_of(tree);
		if (!st || st->virtual_mode) return;
		vgui_tree_node* n = vgui_tree_node_find(tree, node);
		if (!n) return;
		n->enabled = enabled ? 1 : 0;
		vgui_mark_dirty(tree);
	}

	int vgui_tree_get_item_enabled(vgui_widget* tree, vgui_item_id node)
	{
		vgui_tree_node* n = vgui_tree_node_find(tree, node);
		return n ? n->enabled : 0;
	}

	void vgui_tree_set_item_user(vgui_widget* tree, vgui_item_id node, void* user)
	{
		vgui_tree_state* st = vgui_tree_of(tree);
		if (!st || st->virtual_mode) return;
		vgui_tree_node* n = vgui_tree_node_find(tree, node);
		if (n) n->user = user;
	}

	void* vgui_tree_get_item_user(vgui_widget* tree, vgui_item_id node)
	{
		vgui_tree_node* n = vgui_tree_node_find(tree, node);
		return n ? n->user : nullptr;
	}

	// ---- F04 行扩展（icon / text color / inline buttons），与 list 同约定 ----

	void vgui_tree_set_item_icon(vgui_widget* tree, vgui_item_id node, vgui_texture* icon)
	{
		vgui_tree_state* st = vgui_tree_of(tree);
		if (!st || st->virtual_mode) return;
		vgui_tree_node* n = vgui_tree_node_find(tree, node);
		if (!n || n->icon == icon) return;
		n->icon = icon;
		vgui_mark_dirty(tree);
	}

	vgui_texture* vgui_tree_get_item_icon(vgui_widget* tree, vgui_item_id node)
	{
		vgui_tree_node* n = vgui_tree_node_find(tree, node);
		return n ? n->icon : nullptr;
	}

	void vgui_tree_set_item_text_color(vgui_widget* tree, vgui_item_id node, vgui_color color)
	{
		vgui_tree_state* st = vgui_tree_of(tree);
		if (!st || st->virtual_mode) return;
		vgui_tree_node* n = vgui_tree_node_find(tree, node);
		if (!n || n->text_color == color) return;
		n->text_color = color;
		vgui_mark_dirty(tree);
	}

	vgui_color vgui_tree_get_item_text_color(vgui_widget* tree, vgui_item_id node)
	{
		vgui_tree_node* n = vgui_tree_node_find(tree, node);
		return n ? n->text_color : 0;
	}

	void vgui_tree_set_item_buttons(vgui_widget* tree, vgui_item_id node, int count)
	{
		vgui_tree_state* st = vgui_tree_of(tree);
		if (!st || st->virtual_mode) return;
		vgui_tree_node* n = vgui_tree_node_find(tree, node);
		if (!n) return;
		count = std::min(VGUI_MAX_ROW_BUTTONS, std::max(0, count));
		if (n->button_count == count) return;
		n->button_count = count;
		n->button_icons.assign((size_t)count, nullptr);
		vgui_mark_dirty(tree);
	}

	int vgui_tree_get_item_buttons(vgui_widget* tree, vgui_item_id node)
	{
		vgui_tree_node* n = vgui_tree_node_find(tree, node);
		return n ? n->button_count : 0;
	}

	void vgui_tree_set_item_button_icon(vgui_widget* tree, vgui_item_id node, int button, vgui_texture* icon)
	{
		vgui_tree_state* st = vgui_tree_of(tree);
		if (!st || st->virtual_mode) return;
		vgui_tree_node* n = vgui_tree_node_find(tree, node);
		if (!n || button < 0 || button >= n->button_count) return;
		if ((int)n->button_icons.size() <= button)
			n->button_icons.resize((size_t)button + 1, nullptr);
		n->button_icons[(size_t)button] = icon;
		vgui_mark_dirty(tree);
	}

	vgui_texture* vgui_tree_get_item_button_icon(vgui_widget* tree, vgui_item_id node, int button)
	{
		vgui_tree_node* n = vgui_tree_node_find(tree, node);
		if (!n || button < 0) return nullptr;
		return button < (int)n->button_icons.size() ? n->button_icons[(size_t)button] : nullptr;
	}

	void vgui_tree_on_item_button(vgui_widget* tree, vgui_item_button_fn fn, void* user)
	{
		vgui_tree_state* st = vgui_tree_of(tree);
		if (!st) return;
		st->button_fn = fn;
		st->button_user = user;
	}

	void vgui_tree_set_item_expanded(vgui_widget* tree, vgui_item_id node, int expanded)
	{
		vgui_tree_state* st = vgui_tree_of(tree);
		if (!st || st->virtual_mode) return;	// 虚拟模式下展开状态归宿主所有
		vgui_tree_node* n = vgui_tree_node_find(tree, node);
		if (!n) return;
		int e = expanded ? 1 : 0;
		if (n->expanded == e) return;
		n->expanded = e;
		st->rows_dirty = true;
		vgui_mark_dirty(tree);
	}

	int vgui_tree_get_item_expanded(vgui_widget* tree, vgui_item_id node)
	{
		vgui_tree_node* n = vgui_tree_node_find(tree, node);
		return n ? n->expanded : 0;
	}

	int vgui_tree_expand(vgui_widget* tree, vgui_item_id node)
	{
		vgui_tree_state* st = vgui_tree_of(tree);
		if (!st || st->virtual_mode) return 0;
		vgui_tree_node* n = vgui_tree_node_find(tree, node);
		if (!n || n->expanded || n->children.empty()) return 0;
		vgui_tree_set_item_expanded(tree, node, 1);
		return 1;
	}

	int vgui_tree_collapse(vgui_widget* tree, vgui_item_id node)
	{
		vgui_tree_state* st = vgui_tree_of(tree);
		if (!st || st->virtual_mode) return 0;
		vgui_tree_node* n = vgui_tree_node_find(tree, node);
		if (!n || !n->expanded || n->children.empty()) return 0;
		vgui_tree_set_item_expanded(tree, node, 0);
		return 1;
	}

	int vgui_tree_toggle(vgui_widget* tree, vgui_item_id node)
	{
		vgui_tree_state* st = vgui_tree_of(tree);
		if (!st || st->virtual_mode) return 0;
		vgui_tree_node* n = vgui_tree_node_find(tree, node);
		if (!n || n->children.empty()) return 0;
		vgui_tree_set_item_expanded(tree, node, !n->expanded);
		return 1;
	}

	void vgui_tree_expand_all(vgui_widget* tree)
	{
		vgui_tree_state* st = vgui_tree_of(tree);
		if (!st || st->virtual_mode) return;
		for (vgui_tree_node& n : st->nodes)
			if (!n.children.empty()) n.expanded = 1;
		st->rows_dirty = true;
		vgui_mark_dirty(tree);
	}

	void vgui_tree_collapse_all(vgui_widget* tree)
	{
		vgui_tree_state* st = vgui_tree_of(tree);
		if (!st || st->virtual_mode) return;
		for (vgui_tree_node& n : st->nodes) n.expanded = 0;
		st->rows_dirty = true;
		vgui_mark_dirty(tree);
	}

	void vgui_tree_expand_to(vgui_widget* tree, vgui_item_id node)
	{
		vgui_tree_state* st = vgui_tree_of(tree);
		if (!st || st->virtual_mode || !vgui_tree_node_find(tree, node)) return;
		vgui_item_id p = vgui_tree_get_parent(tree, node);
		while (p)
		{
			vgui_tree_node* n = vgui_tree_node_find(tree, p);
			if (!n) break;
			n->expanded = 1;
			p = n->parent;
		}
		st->rows_dirty = true;
		vgui_mark_dirty(tree);
	}

	void vgui_tree_set_selection_mode(vgui_widget* tree, vgui_selection_mode mode)
	{
		vgui_tree_state* st = vgui_tree_of(tree);
		if (!st) return;
		if (mode != VGUI_SELECTION_NONE && mode != VGUI_SELECTION_SINGLE && mode != VGUI_SELECTION_MULTIPLE)
			mode = VGUI_SELECTION_SINGLE;
		st->selection_mode = mode;
		if (mode == VGUI_SELECTION_NONE)
			vgui_tree_clear_selection_internal(tree, true);
		else if (mode == VGUI_SELECTION_SINGLE && st->selected.size() > 1)
		{
			std::vector<vgui_item_id> old = st->selected;
			st->selected.clear();
			st->selected.push_back(old.front());
			for (size_t i = 1; i < old.size(); ++i)
			{
				vgui_tree_notify_select(tree, old[i], 0);
				if (!tree->ctx || !tree->ctx->alive.count(tree)) return;
			}
		}
	}

	vgui_selection_mode vgui_tree_get_selection_mode(vgui_widget* tree)
	{
		vgui_tree_state* st = vgui_tree_of(tree);
		return st ? st->selection_mode : VGUI_SELECTION_NONE;
	}

	int vgui_tree_select(vgui_widget* tree, vgui_item_id node, int selected)
	{
		return vgui_tree_set_selected_internal(tree, node, selected != 0, true) ? 1 : 0;
	}

	void vgui_tree_clear_selection(vgui_widget* tree)
	{
		vgui_tree_clear_selection_internal(tree, true);
	}

	int vgui_tree_is_selected(vgui_widget* tree, vgui_item_id node)
	{
		vgui_tree_state* st = vgui_tree_of(tree);
		return st && vgui_id_selected(st->selected, node) ? 1 : 0;
	}

	int vgui_tree_get_selected_count(vgui_widget* tree)
	{
		vgui_tree_state* st = vgui_tree_of(tree);
		return st ? (int)st->selected.size() : 0;
	}

	vgui_item_id vgui_tree_get_selected(vgui_widget* tree, int index)
	{
		vgui_tree_state* st = vgui_tree_of(tree);
		if (!st || index < 0 || index >= (int)st->selected.size()) return 0;
		return st->selected[(size_t)index];
	}

	void vgui_tree_set_cursor(vgui_widget* tree, vgui_item_id node)
	{
		vgui_tree_state* st = vgui_tree_of(tree);
		if (!st) return;
		st->cursor = vgui_tree_node_find(tree, node) ? node : 0;
		vgui_mark_dirty(tree);
	}

	vgui_item_id vgui_tree_get_cursor(vgui_widget* tree)
	{
		vgui_tree_state* st = vgui_tree_of(tree);
		return st ? st->cursor : 0;
	}

	void vgui_tree_scroll_to_node(vgui_widget* tree, vgui_item_id node, vgui_scroll_align align)
	{
		if (tree) vgui_ensure_layout(tree->ctx);
		vgui_tree_expand_to(tree, node);
		vgui_tree_ensure_visible(tree, node, align);
	}

	void vgui_tree_set_empty_text(vgui_widget* tree, const char* text)
	{
		vgui_tree_state* st = vgui_tree_of(tree);
		if (!st) return;
		st->empty_text = text ? text : "";
		vgui_mark_dirty(tree);
	}

	void vgui_tree_on_select(vgui_widget* tree, vgui_item_select_fn fn, void* user)
	{
		vgui_tree_state* st = vgui_tree_of(tree);
		if (!st) return;
		st->select_fn = fn;
		st->select_user = user;
	}

	void vgui_tree_on_activate(vgui_widget* tree, vgui_item_fn fn, void* user)
	{
		vgui_tree_state* st = vgui_tree_of(tree);
		if (!st) return;
		st->activate_fn = fn;
		st->activate_user = user;
	}

	//----------------------------------------------------------------------------
	// menu model and popup lifecycle
	//----------------------------------------------------------------------------

	vgui_item_id vgui_menu_add_item(vgui_widget* menu, const char* text)
	{
		vgui_menu_state* st = vgui_menu_of(menu);
		if (!st) return 0;
		vgui_menu_item item;
		item.id = vgui_next_item_id(st->next_id);
		if (text) item.text = text;
		st->items.push_back(std::move(item));
		vgui_menu_state* s = vgui_menu_of(menu);
		s->index[s->items.back().id] = s->items.size() - 1;
		vgui_mark_dirty(menu);
		return s->items.back().id;
	}

	vgui_item_id vgui_menu_add_check_item(vgui_widget* menu, const char* text, int checked)
	{
		vgui_item_id id = vgui_menu_add_item(menu, text);
		vgui_menu_state* st = vgui_menu_of(menu);
		if (st && id)
		{
			auto it = st->index.find(id);
			if (it != st->index.end())
			{
				st->items[it->second].checkable = 1;
				st->items[it->second].checked = checked ? 1 : 0;
			}
		}
		return id;
	}

	vgui_item_id vgui_menu_add_separator(vgui_widget* menu)
	{
		vgui_menu_state* st = vgui_menu_of(menu);
		if (!st) return 0;
		vgui_menu_item item;
		item.id = vgui_next_item_id(st->next_id);
		item.type = VGUI_MENU_ITEM_SEPARATOR;
		st->items.push_back(std::move(item));
		vgui_menu_state* s = vgui_menu_of(menu);
		s->index[s->items.back().id] = s->items.size() - 1;
		vgui_mark_dirty(menu);
		return s->items.back().id;
	}

	vgui_item_id vgui_menu_add_submenu(vgui_widget* menu, const char* text, vgui_widget** out_submenu)
	{
		vgui_menu_state* st = vgui_menu_of(menu);
		if (!st || !menu->ctx) return 0;
		vgui_widget* sub = vgui_widget_create(menu->ctx, VGUI_MENU, nullptr);
		if (!sub) return 0;
		vgui_menu_item item;
		item.id = vgui_next_item_id(st->next_id);
		item.type = VGUI_MENU_ITEM_SUBMENU;
		if (text) item.text = text;
		item.submenu = sub;
		st->items.push_back(std::move(item));
		vgui_menu_state* s = vgui_menu_of(menu);
		vgui_item_id id = s->items.back().id;
		s->index[id] = s->items.size() - 1;
		if (sub->menu)
		{
			sub->menu->parent_menu = menu;
			sub->menu->parent_item = id;
		}
		if (out_submenu) *out_submenu = sub;
		vgui_mark_dirty(menu);
		return id;
	}

	int vgui_menu_remove_item(vgui_widget* menu, vgui_item_id item)
	{
		vgui_menu_state* st = vgui_menu_of(menu);
		if (!st) return 0;
		auto it = st->index.find(item);
		if (it == st->index.end() || it->second >= st->items.size()) return 0;
		vgui_context* ctx = menu->ctx;
		vgui_widget* sub = st->items[it->second].submenu;
		if (sub)
		{
			if (st->submenu_open == sub) vgui_menu_close_submenu(menu);
			if (ctx && ctx->alive.count(sub)) vgui_schedule_destroy(sub);
			if (!ctx || !ctx->alive.count(menu) || menu->destroy_pending) return 1;
			st = vgui_menu_of(menu);
			if (!st) return 1;
			it = st->index.find(item);
			if (it == st->index.end()) return 1;
		}
		st->items.erase(st->items.begin() + (ptrdiff_t)it->second);
		st->index.clear();
		for (size_t i = 0; i < st->items.size(); ++i) st->index[st->items[i].id] = i;
		if (st->cursor == item) st->cursor = 0;
		if (st->hover == item) st->hover = 0;
		if (st->pressed == item) st->pressed = 0;
		vgui_mark_dirty(menu);
		if (ctx && !ctx->in_input) vgui_flush_pending(ctx);
		return 1;
	}

	void vgui_menu_clear(vgui_widget* menu)
	{
		vgui_menu_state* st = vgui_menu_of(menu);
		if (!st) return;
		vgui_context* ctx = menu->ctx;
		vgui_menu_close_submenu(menu);
		if (!ctx || !ctx->alive.count(menu) || menu->destroy_pending) return;
		st = vgui_menu_of(menu);
		if (!st) return;
		for (vgui_menu_item& it : st->items)
			if (it.submenu && ctx->alive.count(it.submenu))
				vgui_schedule_destroy(it.submenu);
		st->items.clear();
		st->index.clear();
		st->cursor = 0;
		st->hover = 0;
		st->pressed = 0;
		st->submenu_open = nullptr;
		st->submenu_item = 0;
		vgui_mark_dirty(menu);
		if (ctx && !ctx->in_input) vgui_flush_pending(ctx);
	}

	int vgui_menu_get_count(vgui_widget* menu)
	{
		vgui_menu_state* st = vgui_menu_of(menu);
		return st ? (int)st->items.size() : 0;
	}

	vgui_item_id vgui_menu_get_item_at(vgui_widget* menu, int index)
	{
		vgui_menu_state* st = vgui_menu_of(menu);
		if (!st || index < 0 || index >= (int)st->items.size()) return 0;
		return st->items[(size_t)index].id;
	}

	vgui_menu_item_type vgui_menu_get_item_type(vgui_widget* menu, vgui_item_id item)
	{
		vgui_menu_state* st = vgui_menu_of(menu);
		if (!st) return VGUI_MENU_ITEM_NORMAL;
		auto it = st->index.find(item);
		return it != st->index.end() ? st->items[it->second].type : VGUI_MENU_ITEM_NORMAL;
	}

	vgui_widget* vgui_menu_get_submenu(vgui_widget* menu, vgui_item_id item)
	{
		vgui_menu_state* st = vgui_menu_of(menu);
		if (!st) return nullptr;
		auto it = st->index.find(item);
		return it != st->index.end() ? st->items[it->second].submenu : nullptr;
	}

	void vgui_menu_set_item_text(vgui_widget* menu, vgui_item_id item, const char* text)
	{
		vgui_menu_state* st = vgui_menu_of(menu);
		if (!st) return;
		auto it = st->index.find(item);
		if (it == st->index.end()) return;
		st->items[it->second].text = text ? text : "";
		vgui_mark_dirty(menu);
	}

	const char* vgui_menu_get_item_text(vgui_widget* menu, vgui_item_id item)
	{
		vgui_menu_state* st = vgui_menu_of(menu);
		if (!st) return "";
		auto it = st->index.find(item);
		return it != st->index.end() ? st->items[it->second].text.c_str() : "";
	}

	void vgui_menu_set_item_shortcut(vgui_widget* menu, vgui_item_id item, const char* shortcut)
	{
		vgui_menu_state* st = vgui_menu_of(menu);
		if (!st) return;
		auto it = st->index.find(item);
		if (it == st->index.end()) return;
		st->items[it->second].shortcut = shortcut ? shortcut : "";
		vgui_mark_dirty(menu);
	}

	const char* vgui_menu_get_item_shortcut(vgui_widget* menu, vgui_item_id item)
	{
		vgui_menu_state* st = vgui_menu_of(menu);
		if (!st) return "";
		auto it = st->index.find(item);
		return it != st->index.end() ? st->items[it->second].shortcut.c_str() : "";
	}

	void vgui_menu_set_item_enabled(vgui_widget* menu, vgui_item_id item, int enabled)
	{
		vgui_menu_state* st = vgui_menu_of(menu);
		if (!st) return;
		auto it = st->index.find(item);
		if (it == st->index.end()) return;
		st->items[it->second].enabled = enabled ? 1 : 0;
		vgui_mark_dirty(menu);
	}

	int vgui_menu_get_item_enabled(vgui_widget* menu, vgui_item_id item)
	{
		vgui_menu_state* st = vgui_menu_of(menu);
		if (!st) return 0;
		auto it = st->index.find(item);
		return it != st->index.end() ? st->items[it->second].enabled : 0;
	}

	void vgui_menu_set_item_checked(vgui_widget* menu, vgui_item_id item, int checked)
	{
		vgui_menu_state* st = vgui_menu_of(menu);
		if (!st) return;
		auto it = st->index.find(item);
		if (it == st->index.end()) return;
		st->items[it->second].checked = checked ? 1 : 0;
		vgui_mark_dirty(menu);
	}

	int vgui_menu_get_item_checked(vgui_widget* menu, vgui_item_id item)
	{
		vgui_menu_state* st = vgui_menu_of(menu);
		if (!st) return 0;
		auto it = st->index.find(item);
		return it != st->index.end() ? st->items[it->second].checked : 0;
	}

	int vgui_menu_set_item_checkable(vgui_widget* menu, vgui_item_id item, int checkable)
	{
		vgui_menu_state* st = vgui_menu_of(menu);
		if (!st) return 0;
		auto it = st->index.find(item);
		if (it == st->index.end()) return 0;
		st->items[it->second].checkable = checkable ? 1 : 0;
		if (!checkable) st->items[it->second].checked = 0;
		vgui_mark_dirty(menu);
		return 1;
	}

	int vgui_menu_get_item_checkable(vgui_widget* menu, vgui_item_id item)
	{
		vgui_menu_state* st = vgui_menu_of(menu);
		if (!st) return 0;
		auto it = st->index.find(item);
		return it != st->index.end() ? st->items[it->second].checkable : 0;
	}

	void vgui_menu_set_item_user(vgui_widget* menu, vgui_item_id item, void* user)
	{
		vgui_menu_state* st = vgui_menu_of(menu);
		if (!st) return;
		auto it = st->index.find(item);
		if (it != st->index.end()) st->items[it->second].user = user;
	}

	void* vgui_menu_get_item_user(vgui_widget* menu, vgui_item_id item)
	{
		vgui_menu_state* st = vgui_menu_of(menu);
		if (!st) return nullptr;
		auto it = st->index.find(item);
		return it != st->index.end() ? st->items[it->second].user : nullptr;
	}

	void vgui_menu_open(vgui_widget* menu, float x, float y)
	{
		vgui_menu_show(menu, x, y);
	}

	void vgui_menu_open_below(vgui_widget* menu, vgui_widget* anchor)
	{
		vgui_menu_show_below(menu, anchor);
	}

	void vgui_menu_open_for(vgui_widget* menu, vgui_widget* anchor, float local_x, float local_y)
	{
		vgui_menu_show_for(menu, anchor, local_x, local_y);
	}

	void vgui_menu_close(vgui_widget* menu)
	{
		vgui_menu_state* st = vgui_menu_of(menu);
		if (!st) return;
		if (st->parent_menu)
		{
			vgui_menu_close_internal(menu);
			return;
		}
		vgui_menu_close_all(menu->ctx);
	}

	int vgui_menu_is_open(vgui_widget* menu)
	{
		vgui_menu_state* st = vgui_menu_of(menu);
		return st && st->open ? 1 : 0;
	}

	void vgui_menu_set_cursor(vgui_widget* menu, vgui_item_id item)
	{
		vgui_menu_state* st = vgui_menu_of(menu);
		if (!st) return;
		auto it = st->index.find(item);
		if (it == st->index.end() || it->second >= st->items.size()) return;
		const vgui_menu_item& entry = st->items[it->second];
		if (!entry.enabled || entry.type == VGUI_MENU_ITEM_SEPARATOR) return;
		if (st->submenu_open && st->submenu_item != item)
			vgui_menu_close_submenu(menu);
		st->cursor = item;
		st->hover = item;
		vgui_mark_dirty(menu);
	}

	vgui_item_id vgui_menu_get_cursor(vgui_widget* menu)
	{
		vgui_menu_state* st = vgui_menu_of(menu);
		return st ? st->cursor : 0;
	}

	vgui_widget* vgui_context_get_open_menu(vgui_context* ctx)
	{
		return ctx ? ctx->open_menu : nullptr;
	}

	void vgui_menu_set_width(vgui_widget* menu, float width)
	{
		vgui_menu_state* st = vgui_menu_of(menu);
		if (!st) return;
		st->width = std::max(0.0f, width);
		vgui_mark_dirty(menu);
		if (menu->ctx && menu->ctx->open_menu) vgui_ensure_layout(menu->ctx);
	}

	float vgui_menu_get_width(vgui_widget* menu)
	{
		vgui_menu_state* st = vgui_menu_of(menu);
		return st ? st->width : 0.0f;
	}

	void vgui_menu_set_max_height(vgui_widget* menu, float height)
	{
		vgui_menu_state* st = vgui_menu_of(menu);
		if (!st) return;
		st->max_height = std::max(0.0f, height);
		vgui_mark_dirty(menu);
		if (menu->ctx && menu->ctx->open_menu) vgui_ensure_layout(menu->ctx);
	}

	float vgui_menu_get_max_height(vgui_widget* menu)
	{
		vgui_menu_state* st = vgui_menu_of(menu);
		return st ? st->max_height : 0.0f;
	}

	void vgui_menu_set_close_on_activate(vgui_widget* menu, int close)
	{
		vgui_menu_state* st = vgui_menu_of(menu);
		if (st) st->close_on_activate = close ? 1 : 0;
	}

	int vgui_menu_get_close_on_activate(vgui_widget* menu)
	{
		vgui_menu_state* st = vgui_menu_of(menu);
		return st ? st->close_on_activate : 0;
	}

	void vgui_menu_on_activate(vgui_widget* menu, vgui_item_fn fn, void* user)
	{
		vgui_menu_state* st = vgui_menu_of(menu);
		if (!st) return;
		st->activate_fn = fn;
		st->activate_user = user;
	}

	void vgui_menu_on_open(vgui_widget* menu, vgui_widget_fn fn, void* user)
	{
		vgui_menu_state* st = vgui_menu_of(menu);
		if (!st) return;
		st->open_fn = fn;
		st->open_user = user;
	}

	void vgui_menu_on_close(vgui_widget* menu, vgui_widget_fn fn, void* user)
	{
		vgui_menu_state* st = vgui_menu_of(menu);
		if (!st) return;
		st->close_fn = fn;
		st->close_user = user;
	}

	//----------------------------------------------------------------------------
	// menu bar
	//----------------------------------------------------------------------------

	vgui_item_id vgui_menubar_add_item(vgui_widget* menubar, const char* title)
	{
		vgui_menubar_state* st = vgui_menubar_of(menubar);
		if (!st || !menubar->ctx) return 0;
		vgui_widget* menu = vgui_new_widget(menubar->ctx, VGUI_MENU, nullptr);
		if (!menu) return 0;
		vgui_item_id id = vgui_menubar_add_menu(menubar, title, menu);
		if (!id)
			vgui_widget_destroy(menu);
		return id;
	}

	vgui_item_id vgui_menubar_add_menu(vgui_widget* menubar, const char* title, vgui_widget* menu)
	{
		vgui_menubar_state* st = vgui_menubar_of(menubar);
		if (!st || !menu || menu->kind != VGUI_MENU || !menu->menu ||
			menu->ctx != menubar->ctx || menu->destroy_pending)
			return 0;
		vgui_menu_state* ms = menu->menu.get();
		if (ms->owner_menubar || ms->parent_menu) return 0;
		if (ms->open && menubar->ctx)
			vgui_menu_close_all(menubar->ctx);

		vgui_menubar_item item;
		item.id = vgui_next_item_id(st->next_id);
		if (title) item.text = title;
		item.menu = menu;
		st->items.push_back(std::move(item));
		st->index[st->items.back().id] = st->items.size() - 1;
		ms->owner_menubar = menubar;
		ms->owner_item = st->items.back().id;
		vgui_mark_dirty(menubar);
		return st->items.back().id;
	}

	int vgui_menubar_remove_item(vgui_widget* menubar, vgui_item_id item)
	{
		vgui_menubar_state* st = vgui_menubar_of(menubar);
		if (!st) return 0;
		auto it = st->index.find(item);
		if (it == st->index.end() || it->second >= st->items.size()) return 0;

		vgui_context* ctx = menubar->ctx;
		vgui_widget* menu = st->items[it->second].menu;
		if (menu)
		{
			if (menu->menu)
			{
				menu->menu->owner_menubar = nullptr;
				menu->menu->owner_item = 0;
			}
			if (ctx && ctx->open_menu == menu)
				vgui_menu_close_all(ctx);
			if (ctx && ctx->alive.count(menu))
				vgui_schedule_destroy(menu);
		}
		st->items.erase(st->items.begin() + (std::ptrdiff_t)it->second);
		st->index.clear();
		for (size_t i = 0; i < st->items.size(); ++i)
			st->index[st->items[i].id] = i;
		if (st->active == item) st->active = 0;
		if (st->hover == item) st->hover = 0;
		if (st->pressed == item) st->pressed = 0;
		vgui_mark_dirty(menubar);
		if (ctx && !ctx->in_input) vgui_flush_pending(ctx);
		return 1;
	}

	void vgui_menubar_clear(vgui_widget* menubar)
	{
		vgui_menubar_state* st = vgui_menubar_of(menubar);
		if (!st) return;
		while (!st->items.empty())
		{
			if (!vgui_menubar_remove_item(menubar, st->items.back().id))
				break;
			st = vgui_menubar_of(menubar);
			if (!st) return;
		}
	}

	int vgui_menubar_get_count(vgui_widget* menubar)
	{
		vgui_menubar_state* st = vgui_menubar_of(menubar);
		return st ? (int)st->items.size() : 0;
	}

	vgui_item_id vgui_menubar_get_item_at(vgui_widget* menubar, int index)
	{
		vgui_menubar_state* st = vgui_menubar_of(menubar);
		if (!st || index < 0 || index >= (int)st->items.size()) return 0;
		return st->items[(size_t)index].id;
	}

	const char* vgui_menubar_get_item_text(vgui_widget* menubar, vgui_item_id item)
	{
		vgui_menubar_item* it = vgui_menubar_find(menubar, item);
		return it ? it->text.c_str() : "";
	}

	void vgui_menubar_set_item_text(vgui_widget* menubar, vgui_item_id item, const char* text)
	{
		vgui_menubar_item* it = vgui_menubar_find(menubar, item);
		if (!it) return;
		it->text = text ? text : "";
		vgui_mark_dirty(menubar);
	}

	int vgui_menubar_get_item_enabled(vgui_widget* menubar, vgui_item_id item)
	{
		vgui_menubar_item* it = vgui_menubar_find(menubar, item);
		return it ? it->enabled : 0;
	}

	void vgui_menubar_set_item_enabled(vgui_widget* menubar, vgui_item_id item, int enabled)
	{
		vgui_menubar_item* it = vgui_menubar_find(menubar, item);
		if (!it) return;
		it->enabled = enabled ? 1 : 0;
		if (!it->enabled && it->menu && it->menu->menu && it->menu->menu->open)
			vgui_menu_close_all(menubar->ctx);
		vgui_mark_dirty(menubar);
	}

	vgui_widget* vgui_menubar_get_menu(vgui_widget* menubar, vgui_item_id item)
	{
		vgui_menubar_item* it = vgui_menubar_find(menubar, item);
		return it ? it->menu : nullptr;
	}

	int vgui_menubar_get_item_rect(vgui_widget* menubar, vgui_item_id item,
		float* x, float* y, float* width, float* height)
	{
		if (x) *x = 0.0f;
		if (y) *y = 0.0f;
		if (width) *width = 0.0f;
		if (height) *height = 0.0f;
		vgui_menubar_state* st = vgui_menubar_of(menubar);
		if (!st || !menubar->ctx) return 0;
		auto it = st->index.find(item);
		if (it == st->index.end() || it->second >= st->items.size()) return 0;
		vgui_ensure_layout(menubar->ctx);
		vgui_menubar_item_rect_at(menubar, it->second, x, y, width, height);
		return 1;
	}

	vgui_item_id vgui_menubar_item_at(vgui_widget* menubar, float local_x, float local_y)
	{
		vgui_menubar_state* st = vgui_menubar_of(menubar);
		if (!st || !menubar->ctx) return 0;
		vgui_ensure_layout(menubar->ctx);
		return vgui_menubar_item_at_point(menubar,
			menubar->abs_x + local_x, menubar->abs_y + local_y);
	}

	vgui_item_id vgui_menubar_get_active(vgui_widget* menubar)
	{
		vgui_menubar_state* st = vgui_menubar_of(menubar);
		return st ? st->active : 0;
	}

	int vgui_menubar_open_item(vgui_widget* menubar, vgui_item_id item)
	{
		vgui_menubar_item* it = vgui_menubar_find(menubar, item);
		if (!it || !it->enabled || !it->menu) return 0;
		vgui_menubar_show(menubar, item);
		vgui_menu_state* ms = vgui_menu_of(it->menu);
		return ms && ms->open ? 1 : 0;
	}

	void vgui_menubar_close(vgui_widget* menubar)
	{
		vgui_menubar_state* st = vgui_menubar_of(menubar);
		if (!st) return;
		if (menubar->ctx && menubar->ctx->open_menu)
		{
			vgui_widget* owner = vgui_menu_owner_menubar(menubar->ctx->open_menu);
			if (owner == menubar)
				vgui_menu_close_all(menubar->ctx);
		}
		st = vgui_menubar_of(menubar);
		if (!st) return;
		st->active = 0;
		st->hover = 0;
		st->pressed = 0;
		vgui_mark_dirty(menubar);
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
		if (widget && ctx->modal.active &&
			!vgui_is_within(ctx->modal.stage, widget))
			return;
		if (ctx->open_menu) vgui_menu_close_all(ctx);
		if (ctx->popup.content && !vgui_is_within(ctx->popup.content, widget))
			vgui_popup_close_internal(ctx, VGUI_POPUP_CLOSE_EXPLICIT);
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

	int vgui_context_get_mods(vgui_context* ctx)
	{
		return ctx ? ctx->mods : 0;
	}

	void vgui_context_set_focus_listener(vgui_context* ctx, vgui_focus_fn fn, void* user)
	{
		if (!ctx) return;
		ctx->focus_fn = fn;
		ctx->focus_user = user;
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
		vgui_menu_close_all(ctx);
		vgui_tooltip_clear_candidate(ctx);
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
			if (w->list)
			{
				w->list->hover = 0;
				w->list->pressed = 0;
			}
			if (w->tree)
			{
				w->tree->hover = 0;
				w->tree->pressed = 0;
			}
			if (w->menu)
			{
				w->menu->hover = 0;
				w->menu->pressed = 0;
				w->menu->pending_submenu_item = 0;
			}
			if (w->splitter)
			{
				w->splitter->dragging = 0;
				w->splitter->changed = 0;
			}
			if (w->tabs)
			{
				w->tabs->hover = 0;
				w->tabs->pressed = 0;
				w->tabs->close_hover = 0;
				w->tabs->close_pressed = 0;
				w->tabs->drag_item = 0;
				w->tabs->drag_active = 0;
			}
			if (w->dock)
			{
				w->dock->hover = 0;
				w->dock->pressed = 0;
				w->dock->close_hover = 0;
				w->dock->close_pressed = 0;
				w->dock->press_panel = 0;
				w->dock->press_zone = -1;
				w->dock->drag_mode = 0;
				w->dock->drag_panel = 0;
				w->dock->drop_zone = -1;
				w->dock->drop_index = -1;
				w->dock->drop_floating = 0;
			}
		}
		ctx->hot = nullptr;
		ctx->active = nullptr;
		ctx->press_target = nullptr;
		ctx->slider_drag = nullptr;
		ctx->menu_pressed = nullptr;
		ctx->menu_swallow_up = 0;
		ctx->mods = 0;
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

	int vgui_context_add_key_handler(vgui_context* ctx, vgui_key_handler_fn fn, void* user)
	{
		if (!ctx || !fn) return 0;
		vgui_context::key_handler_entry entry;
		entry.token = ctx->next_key_handler++;
		entry.fn = fn;
		entry.user = user;
		ctx->key_handlers.push_back(entry);
		return entry.token;
	}

	void vgui_context_remove_key_handler(vgui_context* ctx, int token)
	{
		if (!ctx || token <= 0) return;
		ctx->key_handlers.erase(
			std::remove_if(ctx->key_handlers.begin(), ctx->key_handlers.end(),
				[token](const vgui_context::key_handler_entry& entry) {
					return entry.token == token;
				}),
			ctx->key_handlers.end());
	}

	int vgui_context_wants_mouse(vgui_context* ctx)
	{
		if (!ctx) return 0;
		if (ctx->open_menu || ctx->popup.content) return 1;
		if (ctx->menu_swallow_up) return 1;
		if (ctx->hot && ctx->alive.count(ctx->hot)) return 1;
		if (ctx->active && ctx->alive.count(ctx->active)) return 1;
		if (ctx->press_target && ctx->alive.count(ctx->press_target)) return 1;
		return ctx->want_mouse ? 1 : 0;
	}

	int vgui_context_wants_keyboard(vgui_context* ctx)
	{
		if (!ctx) return 0;
		if (ctx->open_menu || ctx->popup.content) return 1;
		if (!ctx->key_handlers.empty()) return 1;
		if (!ctx->focus) return 0;
		return ctx->alive.count(ctx->focus) && ctx->focus->enabled ? 1 : 0;
	}

	int vgui_context_wants_text_input(vgui_context* ctx)
	{
		if (!ctx || ctx->open_menu || !ctx->focus) return 0;
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
		vgui_process_texture_retire(ctx, cb, vg);

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

		// Menus float above the whole tree.  They are not layout children, so
		// they are drawn after the root has finished (and after restoring its
		// clipping state).
		if (ctx->open_menu)
		{
			vgui_draw_menu_chain(ctx->open_menu, cb, vg, &clips);
			while (!clips.stack.empty())
				vgui_clip_pop(&clips, cb, vg);
		}

		// context 级 popup 画在菜单之上;modal 层再压过 popup。
		if (ctx->popup.content)
		{
			vgui_draw_widget(ctx->popup.content, cb, vg, &clips);
			while (!clips.stack.empty())
				vgui_clip_pop(&clips, cb, vg);
		}

		// The modal stage sits above the background tree and menus but below
		// tooltips and notifications.
		vgui_draw_modal(ctx, cb, vg, &clips);

		// F03 drag ghost and drop hints float above the modal stage.
		vgui_draw_drag(ctx, cb, vg);

		// Tooltips sit above menus as well; they never overlap an open menu
		// because opening one clears the hover state.
		vgui_draw_tooltip(ctx, cb, vg);

		// Toasts are the last overlay so a notification stays readable even
		// while a modal is open.
		vgui_draw_toasts(ctx, cb, vg);

		++ctx->frame_index;
	}

	// -------------------------------------------------------------------------
	// VGUI_CUSTOM widget public API
	// -------------------------------------------------------------------------

	void vgui_custom_set_draw(vgui_widget* widget, vgui_custom_draw_fn draw, void* user)
	{
		if (!widget) return;
		widget->custom_draw = draw;
		widget->custom_draw_user = user;
	}

	void vgui_custom_set_size(vgui_widget* widget, vgui_custom_size_fn size, void* user)
	{
		if (!widget) return;
		widget->custom_size = size;
		widget->custom_size_user = user;
		if (widget->ctx)
			widget->ctx->layout_dirty = true;
	}

	void vgui_custom_set_event(vgui_widget* widget, vgui_custom_event_fn event, void* user)
	{
		if (!widget) return;
		widget->custom_event = event;
		widget->custom_event_user = user;
	}

	void vgui_custom_set_clip_content(vgui_widget* widget, int clip)
	{
		if (!widget) return;
		widget->custom_clip_content = clip ? 1 : 0;
	}

	void vgui_custom_draw_text(vgui_widget* widget, struct ovg_ctx_cb* cb, struct rvg_t* vg, const char* text, float x, float y, float width, float height, vgui_color color, float align_x, float align_y)
	{
		if (!widget || !widget->ctx || !cb || !vg || !text || !*text || width <= 0.0f || height <= 0.0f || color == 0) return;
		glm::ivec4 saved = {};
		int has_saved = 0;
		vgui_custom_clip_begin(cb, vg, x, y, width, height, &saved, &has_saved);
		vgui_draw_text(widget->ctx, cb, vg, widget, text, x, y, width, height, vgui_widget_color(widget, color), align_x, align_y);
		vgui_custom_clip_end(cb, vg, saved, has_saved);
	}

	void vgui_custom_draw_texture(vgui_widget* widget, struct ovg_ctx_cb* cb, struct rvg_t* vg, vgui_texture* texture, float x, float y, float width, float height, vgui_color tint)
	{
		if (!widget || !widget->ctx || !cb || !vg || !texture || width <= 0.0f || height <= 0.0f) return;
		vgui_texture_ensure_upload(widget->ctx, texture, cb, vg);
		const int tex_w = texture->width;
		const int tex_h = texture->height;
		if (tex_w <= 0 || tex_h <= 0) return;
		const float scale = std::min(width / (float)tex_w, height / (float)tex_h);
		const float draw_w = std::max(1.0f, (float)tex_w * scale);
		const float draw_h = std::max(1.0f, (float)tex_h * scale);
		const float draw_x = x + (width - draw_w) * 0.5f;
		const float draw_y = y + (height - draw_h) * 0.5f;
		glm::ivec4 saved = {};
		int has_saved = 0;
		vgui_custom_clip_begin(cb, vg, draw_x, draw_y, draw_w, draw_h, &saved, &has_saved);
		const uint32_t color = vgui_ovg_color(vgui_widget_color(widget, tint ? tint : 0xFFFFFFFF));
		const bool native = !texture->cpu && texture->native.image.id != 0;
		if (native && cb->add_texture)
		{
			ovg_texture_r r = {};
			r.tex = &texture->native;
			r.rc = { 0, 0, tex_w, tex_h };
			r.dst = { (int)std::lround(draw_x), (int)std::lround(draw_y), (int)std::lround(draw_w), (int)std::lround(draw_h) };
			r.color = color;
			cb->add_texture(vg, &r);
		}
		else if (!native && texture->image.id != 0 && cb->add_image)
		{
			ovg_image_r r = {};
			r.img = &texture->image;
			r.rc = { 0, 0, tex_w, tex_h };
			r.dst = { (int)std::lround(draw_x), (int)std::lround(draw_y), (int)std::lround(draw_w), (int)std::lround(draw_h) };
			r.color = color;
			cb->add_image(vg, &r);
		}
		vgui_custom_clip_end(cb, vg, saved, has_saved);
	}

} // extern "C"

//----------------------------------------------------------------------------
// Texture lifetime (C++ linkage, declared in vgui_internal.h)
//----------------------------------------------------------------------------

void vgui_texture_retain(vgui_texture* texture)
{
	if (texture) ++texture->refs;
}

void vgui_texture_mark_layout_dirty(vgui_texture* texture)
{
	if (texture && texture->ctx)
		texture->ctx->layout_dirty = true;
}

void vgui_texture_release(vgui_texture* texture)
{
	if (!texture) return;
	if (--texture->refs > 0) return;

	vgui_context* ctx = texture->ctx;
	if (ctx) ctx->textures.erase(texture);

	if (texture->release_native)
	{
		// The current frame may still reference native.image in recorded draw
		// data.  Defer the backend release and deletion to the next vgui_draw(),
		// after the renderer has consumed that data.
		if (ctx)
		{
			ctx->pending_texture_destroy.push_back(texture);
		}
		else
		{
			texture->release_native(texture);
			texture->release_native = nullptr;
			delete texture;
		}
		return;
	}

	// CPU textures may already have a GPU handle.  Keep the object and its
	// vg_image_t alive until the next draw can enqueue image_destroy; the
	// renderer consumes that descriptor after vgui_draw() returns.
	if (ctx && texture->image.id != 0)
	{
		ctx->pending_texture_destroy.push_back(texture);
		return;
	}
	delete texture;
}

vgui_texture* vgui_texture_create_native_internal(
	vgui_context* ctx, int width, int height, vgui_texture_format format)
{
	if (!ctx || width <= 0 || height <= 0 || vgui_texture_bpp(format) <= 0)
		return nullptr;
	vgui_texture* texture = new vgui_texture();
	texture->ctx = ctx;
	texture->cpu = false;
	texture->dirty = false;
	texture->width = width;
	texture->height = height;
	texture->stride = width * vgui_texture_bpp(format);
	texture->format = format;
	texture->image.width = width;
	texture->image.height = height;
	texture->image.valid = false;
	ctx->textures.insert(texture);
	return texture;
}

void vgui_texture_bind_native(vgui_texture* texture, const ovg_texture_t* native,
	void* native_ctx, void* native_texture, void (*release_native)(vgui_texture*))
{
	if (!texture || texture->cpu || !native) return;
	texture->native = *native;
	// image 仅用于通用检查/宽度读取；原生绘制使用 &texture->native。
	texture->image = native->image;
	texture->image.valid = false;
	texture->native_ctx = native_ctx;
	texture->native_texture = native_texture;
	texture->release_native = release_native;
}

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
		case SDLK_F1: return VGUI_KEY_F1;
		case SDLK_F2: return VGUI_KEY_F2;
		case SDLK_F3: return VGUI_KEY_F3;
		case SDLK_F4: return VGUI_KEY_F4;
		case SDLK_F5: return VGUI_KEY_F5;
		case SDLK_F6: return VGUI_KEY_F6;
		case SDLK_F7: return VGUI_KEY_F7;
		case SDLK_F8: return VGUI_KEY_F8;
		case SDLK_F9: return VGUI_KEY_F9;
		case SDLK_F10: return VGUI_KEY_F10;
		case SDLK_F11: return VGUI_KEY_F11;
		case SDLK_F12: return VGUI_KEY_F12;
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
	case dev_event_type_e::ole_drop_e:
	{
		// F03: OS 文件拖入桥接到通用拖放路由（fmt==1 文件列表）。
		ole_drop_et* d = e->v.d;
		if (!d || d->fmt != 1 || !d->str || d->count <= 0) break;
		ret = vgui_input_drop_files(ctx, d->str, d->count, d->x, d->y);
		break;
	}
	default:
		break;
	}

	if (ret) e->ret = 1;
	return ret;
}

