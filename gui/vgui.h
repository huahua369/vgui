#pragma once
/*
 * vgui public C API.
 *
 * This header is the only public interface of libvgui.  It is plain C99/C11:
 * no C++ classes, templates, std:: types or glm types are used here, so the
 * library can be consumed from C, C++ and other languages with a C FFI.
 *
 * Coordinates are expressed in window pixels.  Colors use the packed
 * 0xAARRGGBB layout used by the rest of the project.
 *
 * Typical usage:
 *
 *     vgui_context* ui = vgui_create(font_family, 16.0f);
 *     vgui_context_set_size(ui, 800, 600);
 *
 *     vgui_widget* root = vgui_root(ui);
 *     vgui_widget_set_layout(root, VGUI_LAYOUT_COLUMN);
 *     vgui_widget_set_padding(root, 16, 16, 16, 16);
 *     vgui_widget_set_gap(root, 8);
 *
 *     vgui_widget* title = vgui_widget_add(root, VGUI_LABEL, "Hello");
 *     vgui_widget* button = vgui_widget_add(root, VGUI_BUTTON, "Click me");
 *     vgui_widget_on_event(button, on_button_event, user);
 *
 *     // per frame
 *     vgui_update(ui, delta);
 *     vgui_draw(ui, ovg, rvg);
 *
 * Input can either be forwarded with the specific vgui_input_* functions or
 * from the SDL3 backend through gui_viewport::trigger().
 */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef VGUI_API
#define VGUI_API
#endif

typedef struct vgui_context vgui_context;
typedef struct vgui_widget vgui_widget;

/* Forward declarations of ovg objects.  Keeping them as opaque tags avoids
 * forcing users of this header to include the ovg headers. */
struct font_familys_t;
struct ovg_ctx_cb;
struct rvg_t;

/* Packed 0xAARRGGBB color. */
typedef uint32_t vgui_color;

/* ------------------------------------------------------------------------- */
/* Widgets                                                                   */
/* ------------------------------------------------------------------------- */

typedef enum vgui_widget_kind {
	VGUI_PANEL = 0,
	VGUI_LABEL,
	VGUI_BUTTON,
	VGUI_CHECKBOX,
	VGUI_RADIO,
	VGUI_SWITCH,
	VGUI_SLIDER,
	VGUI_PROGRESS,
	VGUI_EDIT,
	VGUI_SEPARATOR
} vgui_widget_kind;

typedef enum vgui_layout_kind {
	VGUI_LAYOUT_ABSOLUTE = 0,
	VGUI_LAYOUT_ROW,
	VGUI_LAYOUT_COLUMN
} vgui_layout_kind;

typedef enum vgui_align {
	VGUI_ALIGN_AUTO = 0,
	VGUI_ALIGN_START,
	VGUI_ALIGN_CENTER,
	VGUI_ALIGN_END,
	VGUI_ALIGN_STRETCH
} vgui_align;

typedef enum vgui_justify {
	VGUI_JUSTIFY_START = 0,
	VGUI_JUSTIFY_CENTER,
	VGUI_JUSTIFY_END,
	VGUI_JUSTIFY_SPACE_BETWEEN,
	VGUI_JUSTIFY_SPACE_AROUND
} vgui_justify;

/* Mouse cursor shapes.  The numeric values intentionally match the legacy
 * cursor_st enum in the SDL3 backend. */
typedef enum vgui_cursor {
	VGUI_CURSOR_NULL = 0,
	VGUI_CURSOR_ARROW,
	VGUI_CURSOR_IBEAM,
	VGUI_CURSOR_WAIT,
	VGUI_CURSOR_NO,
	VGUI_CURSOR_HAND
} vgui_cursor;

/* Portable key codes passed to vgui_input_key()/vgui_widget_on_event().
 * Printable characters are delivered through vgui_input_text(); these are the
 * keys that have an editing or navigation meaning. */
typedef enum vgui_key {
	VGUI_KEY_NONE = 0,
	VGUI_KEY_BACKSPACE = 8,
	VGUI_KEY_TAB = 9,
	VGUI_KEY_ENTER = 13,
	VGUI_KEY_ESCAPE = 27,
	VGUI_KEY_DELETE = 127,
	VGUI_KEY_LEFT = 0x101,
	VGUI_KEY_RIGHT,
	VGUI_KEY_UP,
	VGUI_KEY_DOWN,
	VGUI_KEY_HOME,
	VGUI_KEY_END,
	VGUI_KEY_PAGE_UP,
	VGUI_KEY_PAGE_DOWN,
	VGUI_KEY_INSERT
} vgui_key;

typedef enum vgui_mod {
	VGUI_MOD_NONE = 0,
	VGUI_MOD_SHIFT = 1,
	VGUI_MOD_CTRL = 2,
	VGUI_MOD_ALT = 4,
	VGUI_MOD_GUI = 8
} vgui_mod;

/* ------------------------------------------------------------------------- */
/* Events                                                                    */
/* ------------------------------------------------------------------------- */

typedef enum vgui_event_type {
	VGUI_EVENT_NONE = 0,
	VGUI_EVENT_MOUSE_MOVE,
	VGUI_EVENT_MOUSE_DOWN,
	VGUI_EVENT_MOUSE_UP,
	VGUI_EVENT_MOUSE_WHEEL,
	VGUI_EVENT_CLICK,
	VGUI_EVENT_DOUBLE_CLICK,
	VGUI_EVENT_TRIPLE_CLICK,
	VGUI_EVENT_ENTER,
	VGUI_EVENT_LEAVE,
	VGUI_EVENT_KEY_DOWN,
	VGUI_EVENT_KEY_UP,
	VGUI_EVENT_TEXT,
	VGUI_EVENT_EDITING,
	VGUI_EVENT_VALUE_CHANGED,
	VGUI_EVENT_SUBMIT
} vgui_event_type;

typedef struct vgui_event {
	int type;                 /* vgui_event_type */
	float x, y;               /* local widget coordinates */
	float wheel_x, wheel_y;   /* wheel delta for VGUI_EVENT_MOUSE_WHEEL */
	int button;               /* 1=left, 2=middle, 3=right */
	int clicks;               /* click count */
	int key;                  /* vgui_key or ASCII letter */
	int mods;                 /* vgui_mod bit mask */
	uint32_t codepoint;       /* unicode scalar for text events, else 0 */
	const char* text;         /* utf-8 text for text/editing events */
} vgui_event;

typedef void (*vgui_event_fn)(vgui_widget* widget, const vgui_event* event, void* user);
typedef void (*vgui_value_fn)(vgui_widget* widget, float value, void* user);
typedef void (*vgui_text_fn)(vgui_widget* widget, const char* text, void* user);

/* ------------------------------------------------------------------------- */
/* Theme                                                                     */
/* ------------------------------------------------------------------------- */

typedef struct vgui_theme {
	vgui_color text;
	vgui_color panel_bg;
	vgui_color panel_border;
	vgui_color button_bg;
	vgui_color button_hover;
	vgui_color button_active;
	vgui_color button_border;
	vgui_color accent;
	vgui_color accent_hover;
	vgui_color track;
	vgui_color edit_bg;
	vgui_color edit_selection;
	vgui_color scrollbar;
	vgui_color disabled;
	/* New fields at the end keep positional initialisers source-compatible.
	 * Passing zero to vgui_context_set_theme() selects a backwards-compatible
	 * fallback: scrollbar_hover uses the scrollbar colour, while checkmark and
	 * switch_thumb use white. */
	vgui_color scrollbar_hover;	/* scrollbar thumb while hovered */
	vgui_color checkmark;		/* checkbox check mark */
	vgui_color switch_thumb;	/* switch knob */
} vgui_theme;

/* ------------------------------------------------------------------------- */
/* Style                                                                     */
/* ------------------------------------------------------------------------- */

/* Geometry, opacity and animation parameters.  Colours live in vgui_theme.
 * Every field has a visible default; call vgui_style_default() before changing
 * individual fields, then pass the result to vgui_context_set_style().
 * Values are clamped to sensible ranges, but a zero-sized control remains
 * possible when explicitly requested. */
typedef struct vgui_focus_style {
	float min_size;		/* 4: smaller widgets do not draw a focus ring */
	float rounding;		/* 4: fallback when the widget has no rounding */
	float inset;		/* 0.5: ring inset from the widget bounds */
	float width;		/* 1: ring stroke width */
} vgui_focus_style;

typedef struct vgui_panel_style {
	float padding;			/* 8: default inner padding */
	float border_width;		/* 1 */
	float rounding;			/* 0 */
	float scrollbar_width;		/* 5 */
	float scrollbar_inset;		/* 2: gap to the panel edge */
	float scrollbar_min_thumb;	/* 24 */
	float scrollbar_rounding;	/* 2.5 */
	float scrollbar_hit_padding;	/* 1: extra pointer slop beside the bar */
	float scrollbar_min_track;	/* 20: hide shorter tracks */
} vgui_panel_style;

typedef struct vgui_label_style {
	float padding_x;		/* 2 */
} vgui_label_style;

typedef struct vgui_button_style {
	float padding_x;		/* 12 */
	float padding_y;		/* 6 */
	float rounding;			/* 4 */
	float border_width;		/* 1 */
	float min_height;		/* 28 */
	float text_extra_width;		/* 4 */
	float hover_mix_min;		/* 0.25: minimum hover mix */
	float press_mix_min;		/* 0.35: minimum pressed mix */
	float press_offset;		/* 1: text shift while pressed */
	float disabled_mix;		/* 0.35: toward theme.disabled */
} vgui_button_style;

typedef struct vgui_choice_style {
	float box_min;			/* 14: checkbox/radio square/circle */
	float box_max;			/* 18 */
	float line_offset;		/* 2: box = line_height - offset */
	float gap;			/* 8: box-to-label gap */
} vgui_choice_style;

typedef struct vgui_checkbox_style {
	float rounding;			/* 3 */
	float border_width;		/* 1 */
	float check_width;		/* 2 */
	float check_start_x;		/* 0.22: check path, fraction of box size */
	float check_start_y;		/* 0.52 */
	float check_mid_x;		/* 0.42 */
	float check_mid_y;		/* 0.72 */
	float check_end_x;		/* 0.80 */
	float check_end_y;		/* 0.28 */
} vgui_checkbox_style;

typedef struct vgui_radio_style {
	float border_width;		/* 1 */
	float dot_scale;		/* 0.55 of the outer radius */
} vgui_radio_style;

typedef struct vgui_switch_style {
	float width;			/* 36 */
	float min_height;		/* 18 */
	float max_height;		/* 22 */
	float line_offset;		/* 2: track = line_height - offset */
	float min_control_height;	/* 20: intrinsic height floor */
	float thumb_inset;		/* 2 */
	float gap;			/* 8: track-to-label gap */
	float disabled_mix;		/* 0.5: toward theme.disabled */
} vgui_switch_style;

typedef struct vgui_slider_style {
	float req_width;		/* 180 */
	float min_height;		/* 24 (also the default requested height) */
	float track_thickness;		/* 4 */
	float knob_radius;		/* 7 */
	float background_alpha;		/* 0.25 when a background is set */
	float disabled_mix;		/* 0.5: toward theme.disabled */
} vgui_slider_style;

typedef struct vgui_progress_style {
	float req_width;		/* 180 */
	float min_height;		/* 20 (also the default requested height) */
	float rounding;			/* 3: fallback when the widget has no rounding */
	float disabled_mix;		/* 0.5: toward theme.disabled */
} vgui_progress_style;

typedef struct vgui_edit_style {
	float padding_x;		/* 6 */
	float padding_y;		/* 4 */
	float rounding;			/* 3 */
	float border_width;		/* 1 */
	float req_width;		/* 160 */
	float req_height;		/* 30 */
	float min_width;		/* 160 */
	float text_extra_width;		/* 12 */
	int multiline_min_lines;	/* 3 */
	float selection_rounding;	/* 2 */
	float selection_unfocused_alpha;/* 0.6 */
	float selection_tail_max_width;	/* 6: tail after a selected newline */
	float selection_tail_line_ratio;/* 0.3: tail width relative to line height */
	float selection_min_width;	/* 1 */
	float caret_width;		/* 1 */
	float caret_blink_period;	/* 1 second */
	float caret_blink_duty;		/* 0.55 */
	float disabled_bg_mix;		/* 0.25: toward theme.disabled */
	float ime_underline_width;	/* 1 */
	float ime_underline_min_width;	/* 2 */
	float scrollbar_width;		/* 4 */
	float scrollbar_inset;		/* 2 */
	float scrollbar_min_thumb;	/* 20 */
	float scrollbar_rounding;	/* 2 */
	float scrollbar_hit_padding;	/* 2 */
	float scrollbar_min_track;	/* 20 */
	float scrollbar_min_cross;	/* 8: hide the horizontal bar below this height */
	float scrollbar_extra_scroll;	/* 2: lets the last glyph edge be reached */
} vgui_edit_style;

typedef struct vgui_separator_style {
	float thickness;		/* 1 */
} vgui_separator_style;

typedef struct vgui_style {
	float animation_speed;		/* 18: exponential hover/check animation rate */
	int text_min_subpixel;		/* 1: text renderer subpixel mode */
	float disabled_text_mix;	/* 0.65: toward theme.disabled */
	vgui_focus_style focus;
	vgui_panel_style panel;
	vgui_label_style label;
	vgui_button_style button;
	vgui_choice_style choice;
	vgui_checkbox_style checkbox;
	vgui_radio_style radio;
	vgui_switch_style switch_;
	vgui_slider_style slider;
	vgui_progress_style progress;
	vgui_edit_style edit;
	vgui_separator_style separator;
} vgui_style;

/* ------------------------------------------------------------------------- */
/* Context                                                                   */
/* ------------------------------------------------------------------------- */

VGUI_API const char* vgui_version(void);

/* Creates a context.  font may be NULL; in that case text is not rendered but
 * layout and events still work. */
VGUI_API vgui_context* vgui_create(struct font_familys_t* font, float font_size);
VGUI_API void vgui_destroy(vgui_context* ctx);

VGUI_API void vgui_context_set_size(vgui_context* ctx, int width, int height);
VGUI_API void vgui_context_get_size(vgui_context* ctx, int* width, int* height);
VGUI_API void vgui_context_set_font(vgui_context* ctx, struct font_familys_t* font, float font_size);
VGUI_API void vgui_context_set_theme(vgui_context* ctx, const vgui_theme* theme);
VGUI_API void vgui_context_get_theme(vgui_context* ctx, vgui_theme* theme);

/* Fills style with the built-in defaults.  Style changes are applied to the
 * context immediately; widgets whose padding/rounding/requested sizes still
 * equal the previous defaults are migrated as well, so an existing tree can be
 * restyled without rebuilding it. */
VGUI_API void vgui_style_default(vgui_style* style);
VGUI_API void vgui_context_set_style(vgui_context* ctx, const vgui_style* style);
VGUI_API void vgui_context_get_style(vgui_context* ctx, vgui_style* style);

/* Focus-ring opacity (0..1) for widgets that do not override it.  0 disables
 * focus rings entirely, which is convenient for screenshots or touch UIs. */
VGUI_API void vgui_context_set_focus_alpha(vgui_context* ctx, float alpha);
VGUI_API float vgui_context_get_focus_alpha(vgui_context* ctx);

/* Root panel.  It is owned by the context; do not destroy it. */
VGUI_API vgui_widget* vgui_root(vgui_context* ctx);

/* ------------------------------------------------------------------------- */
/* Widget tree                                                               */
/* ------------------------------------------------------------------------- */

VGUI_API vgui_widget* vgui_widget_create(vgui_context* ctx, vgui_widget_kind kind, const char* text);
VGUI_API vgui_widget* vgui_widget_add(vgui_widget* parent, vgui_widget_kind kind, const char* text);
VGUI_API void vgui_widget_attach(vgui_widget* parent, vgui_widget* child);
VGUI_API void vgui_widget_remove(vgui_widget* widget);
VGUI_API void vgui_widget_destroy(vgui_widget* widget);
VGUI_API void vgui_widget_clear(vgui_widget* panel);

VGUI_API vgui_widget_kind vgui_widget_get_kind(vgui_widget* widget);
VGUI_API vgui_context* vgui_widget_get_context(vgui_widget* widget);
VGUI_API vgui_widget* vgui_widget_get_parent(vgui_widget* widget);
VGUI_API size_t vgui_widget_get_child_count(vgui_widget* widget);
VGUI_API vgui_widget* vgui_widget_get_child(vgui_widget* widget, size_t index);

/* ------------------------------------------------------------------------- */
/* Properties                                                                */
/* ------------------------------------------------------------------------- */

VGUI_API void vgui_widget_set_id(vgui_widget* widget, const char* id);
VGUI_API const char* vgui_widget_get_id(vgui_widget* widget);

VGUI_API void vgui_widget_set_visible(vgui_widget* widget, int visible);
VGUI_API int vgui_widget_get_visible(vgui_widget* widget);
VGUI_API void vgui_widget_set_enabled(vgui_widget* widget, int enabled);
VGUI_API int vgui_widget_get_enabled(vgui_widget* widget);
VGUI_API void vgui_widget_set_focusable(vgui_widget* widget, int focusable);
VGUI_API int vgui_widget_get_focusable(vgui_widget* widget);

VGUI_API void vgui_widget_set_text(vgui_widget* widget, const char* text);
VGUI_API const char* vgui_widget_get_text(vgui_widget* widget);

VGUI_API void vgui_widget_set_font_size(vgui_widget* widget, float size);
VGUI_API void vgui_widget_set_text_color(vgui_widget* widget, vgui_color color);
VGUI_API void vgui_widget_set_background(vgui_widget* widget, vgui_color color);
VGUI_API void vgui_widget_set_border(vgui_widget* widget, vgui_color color, float width);
VGUI_API void vgui_widget_set_rounding(vgui_widget* widget, float radius);
VGUI_API void vgui_widget_set_text_align(vgui_widget* widget, float align_x, float align_y);
VGUI_API void vgui_widget_set_cursor(vgui_widget* widget, vgui_cursor cursor);
VGUI_API void vgui_widget_set_opacity(vgui_widget* widget, float opacity);

/* Focus-ring opacity for this widget (0..1, 0 hides the ring).  A negative
 * value restores inheritance from the context default.  The getter returns
 * the effective opacity, including the inherited value. */
VGUI_API void vgui_widget_set_focus_alpha(vgui_widget* widget, float alpha);
VGUI_API float vgui_widget_get_focus_alpha(vgui_widget* widget);

/* Layout properties. */
VGUI_API void vgui_widget_set_layout(vgui_widget* widget, vgui_layout_kind layout);
VGUI_API void vgui_widget_set_pos(vgui_widget* widget, float x, float y);
VGUI_API void vgui_widget_set_size(vgui_widget* widget, float width, float height);
VGUI_API void vgui_widget_set_min_size(vgui_widget* widget, float width, float height);
VGUI_API void vgui_widget_set_max_size(vgui_widget* widget, float width, float height);
VGUI_API void vgui_widget_set_margin(vgui_widget* widget, float left, float top, float right, float bottom);
VGUI_API void vgui_widget_set_padding(vgui_widget* widget, float left, float top, float right, float bottom);
VGUI_API void vgui_widget_set_gap(vgui_widget* widget, float gap);
VGUI_API void vgui_widget_set_align_items(vgui_widget* widget, vgui_align align);
VGUI_API void vgui_widget_set_align_self(vgui_widget* widget, vgui_align align);
VGUI_API void vgui_widget_set_justify(vgui_widget* widget, vgui_justify justify);
VGUI_API void vgui_widget_set_grow(vgui_widget* widget, float grow);

VGUI_API void vgui_widget_get_rect(vgui_widget* widget, float* x, float* y, float* width, float* height);

/* Value controls (checkbox/radio/switch/slider/progress). */
VGUI_API void vgui_widget_set_value(vgui_widget* widget, float value);
VGUI_API float vgui_widget_get_value(vgui_widget* widget);
VGUI_API void vgui_widget_set_range(vgui_widget* widget, float min_value, float max_value);
VGUI_API void vgui_widget_set_step(vgui_widget* widget, float step);
VGUI_API void vgui_widget_set_checked(vgui_widget* widget, int checked);
VGUI_API int vgui_widget_get_checked(vgui_widget* widget);
VGUI_API void vgui_widget_set_vertical(vgui_widget* widget, int vertical);

/* Panels: scrolling.  Scroll offsets are clamped to the content extent. */
VGUI_API void vgui_panel_set_scrollable(vgui_widget* panel, int horizontal, int vertical);
VGUI_API void vgui_panel_set_scroll(vgui_widget* panel, float x, float y);
VGUI_API void vgui_panel_scroll_by(vgui_widget* panel, float dx, float dy);
VGUI_API void vgui_panel_get_scroll(vgui_widget* panel, float* x, float* y);
VGUI_API void vgui_panel_get_content_size(vgui_widget* panel, float* width, float* height);

/* Callbacks. */
VGUI_API void vgui_widget_on_event(vgui_widget* widget, vgui_event_fn fn, void* user);
VGUI_API void vgui_widget_on_value(vgui_widget* widget, vgui_value_fn fn, void* user);
VGUI_API void vgui_widget_on_text(vgui_widget* widget, vgui_text_fn fn, void* user);

/* ------------------------------------------------------------------------- */
/* Text edit helpers                                                         */
/* ------------------------------------------------------------------------- */

VGUI_API void vgui_edit_set_text(vgui_widget* edit, const char* text);
VGUI_API const char* vgui_edit_get_text(vgui_widget* edit);
VGUI_API void vgui_edit_set_placeholder(vgui_widget* edit, const char* text);
VGUI_API void vgui_edit_set_password(vgui_widget* edit, char mask);
VGUI_API void vgui_edit_set_multiline(vgui_widget* edit, int multiline);
VGUI_API void vgui_edit_set_readonly(vgui_widget* edit, int readonly);
VGUI_API void vgui_edit_set_cursor(vgui_widget* edit, int byte_index);
VGUI_API int vgui_edit_get_cursor(vgui_widget* edit);
VGUI_API void vgui_edit_select_all(vgui_widget* edit);
VGUI_API void vgui_edit_select(vgui_widget* edit, int anchor, int cursor);
VGUI_API void vgui_edit_clear_selection(vgui_widget* edit);
VGUI_API int vgui_edit_has_selection(vgui_widget* edit);
VGUI_API void vgui_edit_set_ime_text(vgui_widget* edit, const char* text, int cursor, int length);

/* Selection highlight style.  color 0 restores theme.edit_selection.
 * rounding is the corner radius in pixels; 0 removes the rounding entirely.
 * It applies to every exposed corner of a single- or multi-line selection. */
VGUI_API void vgui_edit_set_selection_color(vgui_widget* edit, vgui_color color);
VGUI_API void vgui_edit_set_selection_rounding(vgui_widget* edit, float radius);

/* Caret width in pixels.  Defaults to 1; 0 hides the caret. */
VGUI_API void vgui_edit_set_caret_width(vgui_widget* edit, float width);

/* Clipboard / history helpers; these are the operations behind the Ctrl+C,
 * Ctrl+X, Ctrl+V, Ctrl+Z and Ctrl+Y shortcuts.  They return non-zero when the
 * operation changed (or copied) something. */
VGUI_API int vgui_edit_copy(vgui_widget* edit);
VGUI_API int vgui_edit_cut(vgui_widget* edit);
VGUI_API int vgui_edit_paste(vgui_widget* edit);
VGUI_API int vgui_edit_undo(vgui_widget* edit);
VGUI_API int vgui_edit_redo(vgui_widget* edit);
VGUI_API int vgui_edit_can_undo(vgui_widget* edit);
VGUI_API int vgui_edit_can_redo(vgui_widget* edit);

/* ------------------------------------------------------------------------- */
/* Focus and input                                                           */
/* ------------------------------------------------------------------------- */

VGUI_API void vgui_set_focus(vgui_context* ctx, vgui_widget* widget);
VGUI_API vgui_widget* vgui_get_focus(vgui_context* ctx);
VGUI_API vgui_widget* vgui_hit_test(vgui_context* ctx, float x, float y);
VGUI_API vgui_cursor vgui_context_get_cursor(vgui_context* ctx);

VGUI_API int vgui_input_mouse_move(vgui_context* ctx, float x, float y);
VGUI_API int vgui_input_mouse_down(vgui_context* ctx, int button, float x, float y, int clicks);
VGUI_API int vgui_input_mouse_up(vgui_context* ctx, int button, float x, float y, int clicks);
VGUI_API int vgui_input_mouse_wheel(vgui_context* ctx, float wheel_x, float wheel_y);
VGUI_API int vgui_input_key(vgui_context* ctx, int key, int down, int mods);
VGUI_API int vgui_input_text(vgui_context* ctx, const char* utf8);
VGUI_API int vgui_input_editing(vgui_context* ctx, const char* utf8, int cursor, int length);
VGUI_API void vgui_input_clear(vgui_context* ctx);
VGUI_API void vgui_input_focus_next(vgui_context* ctx, int reverse);

VGUI_API int vgui_context_wants_mouse(vgui_context* ctx);
VGUI_API int vgui_context_wants_keyboard(vgui_context* ctx);
VGUI_API int vgui_context_wants_text_input(vgui_context* ctx);
VGUI_API int vgui_context_get_text_input_rect(vgui_context* ctx, int* x, int* y, int* width, int* height);

/* ------------------------------------------------------------------------- */
/* Frame                                                                     */
/* ------------------------------------------------------------------------- */

VGUI_API void vgui_layout(vgui_context* ctx);
VGUI_API void vgui_update(vgui_context* ctx, float delta_time);
VGUI_API void vgui_draw(vgui_context* ctx, struct ovg_ctx_cb* cb, struct rvg_t* vg);

#ifdef __cplusplus
} /* extern "C" */
#endif
