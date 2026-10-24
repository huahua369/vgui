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
		VGUI_SEPARATOR,
		VGUI_LIST,
		VGUI_TREE,
		VGUI_MENU,
		VGUI_IMAGE,
		VGUI_CUSTOM,
		VGUI_SPLITTER,
		VGUI_TABVIEW,
		VGUI_MENUBAR
	} vgui_widget_kind;

	typedef enum vgui_layout_kind {
		VGUI_LAYOUT_ABSOLUTE = 0,
		VGUI_LAYOUT_ROW,
		VGUI_LAYOUT_COLUMN,
		VGUI_LAYOUT_WRAP
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

	/* Multi-line distribution for VGUI_LAYOUT_WRAP.  It is ignored by the other
	 * layout kinds. */
	typedef enum vgui_align_content {
		VGUI_ALIGN_CONTENT_START = 0,
		VGUI_ALIGN_CONTENT_CENTER,
		VGUI_ALIGN_CONTENT_END,
		VGUI_ALIGN_CONTENT_SPACE_BETWEEN,
		VGUI_ALIGN_CONTENT_SPACE_AROUND,
		VGUI_ALIGN_CONTENT_SPACE_EVENLY,
		VGUI_ALIGN_CONTENT_STRETCH
	} vgui_align_content;

	typedef enum vgui_image_fit {
		VGUI_IMAGE_FIT_STRETCH = 0,
		VGUI_IMAGE_FIT_CONTAIN,
		VGUI_IMAGE_FIT_COVER,
		VGUI_IMAGE_FIT_NONE
	} vgui_image_fit;

	typedef enum vgui_texture_format {
		VGUI_TEXTURE_FORMAT_RGBA8 = 0,
		VGUI_TEXTURE_FORMAT_BGRA8,
		VGUI_TEXTURE_FORMAT_RGBA8_SRGB,
		VGUI_TEXTURE_FORMAT_BGRA8_SRGB
	} vgui_texture_format;

	typedef enum vgui_texture_filter {
		VGUI_TEXTURE_FILTER_NEAREST = 0,
		VGUI_TEXTURE_FILTER_LINEAR
	} vgui_texture_filter;

	typedef enum vgui_texture_wrap {
		VGUI_TEXTURE_WRAP_CLAMP = 0,
		VGUI_TEXTURE_WRAP_REPEAT,
		VGUI_TEXTURE_WRAP_MIRROR
	} vgui_texture_wrap;

	/* Opaque GPU/CPU image resource owned by a vgui context.  CPU pixels are
	 * copied by vgui_texture_create/update; the caller may release its buffer
	 * immediately.  The first draw uploads the pixels lazily. */
	typedef struct vgui_texture vgui_texture;

	typedef struct vgui_texture_desc {
		int width;
		int height;
		int stride;			/* bytes per row; 0 = width * bytes-per-pixel */
		vgui_texture_format format;
		const void* pixels;		/* CPU pixels; may be NULL for an empty texture */
	} vgui_texture_desc;

	/* Mouse cursor shapes.  The numeric values intentionally match the legacy
	 * cursor_st enum in the SDL3 backend. */
	typedef enum vgui_cursor {
		VGUI_CURSOR_NULL = 0,
		VGUI_CURSOR_ARROW,
		VGUI_CURSOR_IBEAM,
		VGUI_CURSOR_WAIT,
		VGUI_CURSOR_NO,
		VGUI_CURSOR_HAND,
		VGUI_CURSOR_SIZE_H,	/* horizontal resize (left/right) */
		VGUI_CURSOR_SIZE_V	/* vertical resize (up/down) */
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

	/* Stable identifier for a list/tree/menu model item.  Zero is never a valid
	 * item id.  Ids are unique within one widget and remain stable until the item
	 * is removed; they are not reused while the widget is alive. */
	typedef uint64_t vgui_item_id;

	typedef enum vgui_selection_mode {
		VGUI_SELECTION_NONE = 0,
		VGUI_SELECTION_SINGLE,
		VGUI_SELECTION_MULTIPLE
	} vgui_selection_mode;

	typedef enum vgui_scroll_align {
		VGUI_SCROLL_ALIGN_NEAREST = 0,	/* scroll only when the item is outside */
		VGUI_SCROLL_ALIGN_TOP,		/* put the item at the top/left edge */
		VGUI_SCROLL_ALIGN_CENTER,	/* centre the item */
		VGUI_SCROLL_ALIGN_BOTTOM	/* put the item at the bottom/right edge */
	} vgui_scroll_align;

	typedef enum vgui_menu_item_type {
		VGUI_MENU_ITEM_NORMAL = 0,
		VGUI_MENU_ITEM_SEPARATOR,
		VGUI_MENU_ITEM_SUBMENU
	} vgui_menu_item_type;

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
		/* Model-item context for lists, trees and menus.  item is 0 when the event
		 * does not refer to an item.  index is the item's index in the model (or
		 * the visible row index for a tree), and -1 when not applicable. */
		vgui_item_id item;
		int index;
	} vgui_event;

	typedef void (*vgui_event_fn)(vgui_widget* widget, const vgui_event* event, void* user);
	typedef void (*vgui_value_fn)(vgui_widget* widget, float value, void* user);
	typedef void (*vgui_text_fn)(vgui_widget* widget, const char* text, void* user);
	typedef void (*vgui_item_fn)(vgui_widget* widget, vgui_item_id item, void* user);
	typedef void (*vgui_item_select_fn)(vgui_widget* widget, vgui_item_id item, int selected, void* user);
	typedef void (*vgui_widget_fn)(vgui_widget* widget, void* user);

	/* Context-level key filter, used for application shortcuts.  Handlers run
	 * after the focused widget and the open menu have had a chance to consume
	 * the key, in reverse registration order.  Returning non-zero marks the key
	 * as handled.  Handlers are called for both key-down and key-up events. */
	typedef int (*vgui_key_handler_fn)(vgui_context* ctx, int key, int down,
		int mods, void* user);

	/* ------------------------------------------------------------------------- */
	/* Custom-draw widget (VGUI_CUSTOM)                                          */
	/* ------------------------------------------------------------------------- */

	/*
	 * VGUI_CUSTOM 控件直接暴露 ovg_ctx_cb* / rvg_t* 给用户回调，允许任意
	 * OVG 矢量、图案、几何命令。draw 回调每帧在 vgui_draw() 里被调用；
	 * abs_x/y/w/h 是布局后算好的绝对坐标，delta_time 是自上一帧以来的秒数。
	 * 用户可以在回调里调用 ovg 的 save/restore/fill/stroke/clip/new_pattern
	 * 等全部接口。pattern 需要自己 destroy。
	 */
	typedef void (*vgui_custom_draw_fn)(vgui_widget* widget,
		struct ovg_ctx_cb* cb, struct rvg_t* vg,
		float abs_x, float abs_y,
		float abs_w, float abs_h,
		float delta_time,
		void* user);

	/*
	 * 尺寸查询：填充 intrinsic_w/h（0=走常规 flex 规则）。
	 */
	typedef void (*vgui_custom_size_fn)(vgui_widget* widget,
		float* out_intrinsic_w,
		float* out_intrinsic_h,
		void* user);

	/*
	 * 事件回调：返回 1 表示事件已消费（不再向父/兄弟分发）。
	 * 如果只需要命中测试过滤，在回调里判断坐标即可，hit test 本身
	 * 仍按 abs_x/abs_y/abs_w/abs_h 的矩形进行。
	 */
	typedef int (*vgui_custom_event_fn)(vgui_widget* widget,
		const vgui_event* event,
		void* user);

	/* ------------------------------------------------------------------------- */
	/* Tooltip                                                                   */
	/* ------------------------------------------------------------------------- */

	/* Tooltips are context-level overlays: they are not child widgets and do not
	 * take part in layout or hit testing.  A widget supplies static text, or a
	 * provider callback for text that depends on runtime state.  Lists and trees
	 * can additionally assign a tooltip to each row/node. */
	typedef enum vgui_tooltip_anchor {
		VGUI_TOOLTIP_ANCHOR_POINTER = 0,	/* follow the mouse with an offset */
		VGUI_TOOLTIP_ANCHOR_WIDGET		/* below the widget, centred; flips above */
	} vgui_tooltip_anchor;

	/* Returns the tooltip text for a widget, or NULL/"" when it has none.  The
	 * returned pointer only has to stay valid until the next callback. */
	typedef const char* (*vgui_tooltip_text_fn)(vgui_widget* widget, void* user);

	/* ------------------------------------------------------------------------- */
	/* Modal, dialog and toast                                                   */
	/* ------------------------------------------------------------------------- */

	/* A modal owns a context-level stage that covers the whole window.  The
	 * supplied panel is detached from its old parent and centred inside that
	 * stage.  While the modal is open hit testing, keyboard focus, Tab cycling
	 * and wheel input are restricted to the stage, so the background cannot be
	 * interacted with.  One modal can be open per context.
	 *
	 * close_on_escape defaults to 1, close_on_backdrop to 0, dim_background to
	 * 1 and destroy_on_close to 0 (the caller keeps ownership of the panel).
	 * A negative margin/backdrop_alpha selects the value from
	 * style.modal.  The optional close callback runs after the modal has been
	 * deactivated and the panel detached, but before destroy_on_close destroys
	 * it, so the panel is still valid for inspection. */
	typedef enum vgui_modal_result {
		VGUI_MODAL_RESULT_NONE = 0,
		VGUI_MODAL_RESULT_CANCEL = 1,
		VGUI_MODAL_RESULT_CONFIRM = 2
	} vgui_modal_result;

	typedef void (*vgui_modal_close_fn)(vgui_context* ctx, vgui_widget* panel,
		int result, void* user);

	typedef struct vgui_modal_options {
		int close_on_escape;
		int close_on_backdrop;
		int dim_background;
		int destroy_on_close;
		int escape_result;
		int backdrop_result;
		float margin;			/* <0 -> style.modal.margin */
		float backdrop_alpha;		/* <0 -> style.modal.backdrop_alpha */
		vgui_modal_close_fn on_close;
		void* user;
	} vgui_modal_options;

	VGUI_API void vgui_modal_options_default(vgui_modal_options* options);

	/* Shows panel as the context modal.  Returns 0 when the context already has
	 * an open modal or panel is invalid/owned by another context. */
	VGUI_API int vgui_modal_show(vgui_context* ctx, vgui_widget* panel, const vgui_modal_options* options);
	/* result is passed to the close callback; use vgui_modal_result values or
	 * an application-defined integer.  Esc/backdrop use the configured results.
	 * Calling close while no modal is active is a no-op. */
	VGUI_API void vgui_modal_close(vgui_context* ctx, int result);
	VGUI_API int vgui_modal_active(vgui_context* ctx);
	/* Active panel, or NULL when no modal is open. */
	VGUI_API vgui_widget* vgui_modal_panel(vgui_context* ctx);
	/* Result of the most recently closed modal; reset when a modal opens. */
	VGUI_API int vgui_modal_result(vgui_context* ctx);

	/* Convenience dialogs.  They build a modal panel, manage its lifetime and
	 * report the result through on_result.  Confirm/alert focus the default
	 * button, prompt focuses the edit box.  Enter confirms (including from the
	 * prompt edit box), Esc cancels, and clicking the backdrop does not close
	 * the dialog. */
	typedef enum vgui_dialog_kind {
		VGUI_DIALOG_ALERT = 0,		/* message + OK */
		VGUI_DIALOG_CONFIRM,		/* message + Cancel/OK */
		VGUI_DIALOG_PROMPT		/* message + edit + Cancel/OK */
	} vgui_dialog_kind;

	/* text is the current prompt value ("" for alert/confirm) and is only valid
	 * for the duration of the callback. */
	typedef void (*vgui_dialog_close_fn)(vgui_context* ctx, int result,
		const char* text, void* user);

	typedef struct vgui_dialog_desc {
		vgui_dialog_kind kind;
		const char* title;
		const char* message;
		const char* confirm_text;	/* NULL/"" -> "OK" */
		const char* cancel_text;	/* NULL/"" -> "Cancel" */
		const char* placeholder;	/* prompt only */
		const char* initial_text;	/* prompt only */
		int danger;			/* confirm button uses the danger colour */
		float width;			/* 0 -> style.modal.dialog_width */
		vgui_dialog_close_fn on_result;
		void* user;
	} vgui_dialog_desc;

	VGUI_API void vgui_dialog_desc_default(vgui_dialog_desc* desc);
	/* Returns the dialog panel, or NULL when another modal is already open and
	 * the dialog could not be shown. */
	VGUI_API vgui_widget* vgui_dialog_show(vgui_context* ctx, const vgui_dialog_desc* desc);
	/* Prompt edit box while a prompt dialog is open, otherwise NULL. */
	VGUI_API vgui_widget* vgui_dialog_input(vgui_context* ctx);

	VGUI_API vgui_widget* vgui_dialog_alert(vgui_context* ctx, const char* title, const char* message, vgui_dialog_close_fn on_result, void* user);
	VGUI_API vgui_widget* vgui_dialog_confirm(vgui_context* ctx, const char* title, const char* message, int danger, vgui_dialog_close_fn on_result, void* user);
	VGUI_API vgui_widget* vgui_dialog_prompt(vgui_context* ctx, const char* title, const char* message, const char* initial_text, vgui_dialog_close_fn on_result, void* user);

	/* Lightweight notifications.  Toasts are context overlays: they do not take
	 * part in layout or hit testing and expire automatically in vgui_update().
	 * duration < 0 uses style.toast.duration; 0 keeps the toast until dismissed.
	 * The visible count is capped by style.toast.max_visible. */
	typedef uint64_t vgui_toast_id;

	typedef enum vgui_toast_position {
		VGUI_TOAST_TOP_RIGHT = 0,
		VGUI_TOAST_TOP_LEFT,
		VGUI_TOAST_TOP_CENTER,
		VGUI_TOAST_BOTTOM_RIGHT,
		VGUI_TOAST_BOTTOM_LEFT,
		VGUI_TOAST_BOTTOM_CENTER
	} vgui_toast_position;

	typedef enum vgui_toast_level {
		VGUI_TOAST_INFO = 0,
		VGUI_TOAST_SUCCESS,
		VGUI_TOAST_WARNING,
		VGUI_TOAST_ERROR
	} vgui_toast_level;

	typedef struct vgui_toast_options {
		vgui_toast_position position;
		vgui_toast_level level;
		float duration;			/* <0 -> style.toast.duration, 0 = sticky */
	} vgui_toast_options;

	VGUI_API void vgui_toast_options_default(vgui_toast_options* options);
	VGUI_API vgui_toast_id vgui_toast_show(vgui_context* ctx, const char* text, const vgui_toast_options* options);
	VGUI_API int vgui_toast_dismiss(vgui_context* ctx, vgui_toast_id toast);
	VGUI_API void vgui_toast_clear(vgui_context* ctx);
	VGUI_API int vgui_toast_count(vgui_context* ctx);
	/* Current screen rectangle of a toast.  Returns 0 for unknown ids or when
	 * the toast is beyond style.toast.max_visible.  Any output may be NULL. */
	VGUI_API int vgui_toast_get_rect(vgui_context* ctx, vgui_toast_id toast, float* x, float* y, float* width, float* height);

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
		/* Model controls.  Zero selects a backwards-compatible fallback as
		 * documented in vgui_context_set_theme(). */
		vgui_color list_bg;		/* fallback: panel_bg */
		vgui_color list_border;		/* fallback: panel_border */
		vgui_color row_text;		/* fallback: text */
		vgui_color row_hover;		/* fallback: button_hover */
		vgui_color row_selected;		/* fallback: semi-transparent accent */
		vgui_color row_selected_text;	/* fallback: text */
		vgui_color tree_guide;		/* fallback: semi-transparent panel_border */
		vgui_color menu_bg;		/* fallback: panel_bg */
		vgui_color menu_border;		/* fallback: button_border */
		vgui_color menu_hover;		/* fallback: button_hover */
		vgui_color menu_disabled_text;	/* fallback: disabled */
		vgui_color tooltip_bg;		/* fallback: menu_bg */
		vgui_color tooltip_border;	/* fallback: menu_border */
		vgui_color tooltip_text;	/* fallback: text */
		/* Modal, dialog and toast colours.  A zero value selects a
		 * backwards-compatible fallback as documented in
		 * vgui_context_set_theme(). */
		vgui_color modal_backdrop;	/* fallback: opaque black (alpha is scaled by style) */
		vgui_color modal_bg;		/* fallback: panel_bg */
		vgui_color modal_border;	/* fallback: button_border */
		vgui_color modal_danger;	/* fallback: red danger tone */
		vgui_color toast_bg;		/* fallback: menu_bg */
		vgui_color toast_border;	/* fallback: menu_border */
		vgui_color toast_text;		/* fallback: text */
		vgui_color toast_info;		/* fallback: accent */
		vgui_color toast_success;	/* fallback: green success tone */
		vgui_color toast_warning;	/* fallback: amber warning tone */
		vgui_color toast_error;		/* fallback: modal_danger */
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
		float icon_gap;			/* 6: icon-to-text gap when both are present */
	} vgui_button_style;

	/* 按钮 icon 位置。VERTICAL 只对 VGUI_BUTTON，其他控件忽略。
	 * LEFT / RIGHT 横排 icon+text，TOP / BOTTOM 竖排 icon+text。 */
	typedef enum vgui_button_icon_pos {
		VGUI_BUTTON_ICON_LEFT = 0,
		VGUI_BUTTON_ICON_RIGHT,
		VGUI_BUTTON_ICON_TOP,
		VGUI_BUTTON_ICON_BOTTOM
	} vgui_button_icon_pos;

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

	typedef struct vgui_list_style {
		float row_height;	/* 0: line height + 10 */
		float padding_x;		/* 4 */
		float padding_y;		/* 4 */
		float rounding;		/* 3: row highlight rounding */
		float req_width;		/* 220 */
		float req_height;		/* 180 */
		float disabled_mix;		/* 0.6: toward theme.disabled */
	} vgui_list_style;

	typedef struct vgui_tree_style {
		float row_height;	/* 0: line height + 10 */
		float indent;		/* 18 */
		float arrow_size;	/* 9 */
		float arrow_gap;	/* 4 */
		float padding_x;		/* 6 */
		float padding_y;		/* 4 */
		float rounding;		/* 3: row highlight rounding */
		float req_width;		/* 220 */
		float req_height;		/* 200 */
		float guide_alpha;	/* 0.45 */
		float disabled_mix;		/* 0.6: toward theme.disabled */
	} vgui_tree_style;

	typedef struct vgui_menu_style {
		float item_height;	/* 28 */
		float padding_x;		/* 4 */
		float padding_y;		/* 4 */
		float item_padding_x;	/* 8 */
		float check_width;	/* 18: check column when any item is checkable */
		float shortcut_gap;	/* 24 */
		float submenu_arrow_width;/* 10 */
		float separator_height;	/* 9 */
		float rounding;		/* 4 */
		float min_width;		/* 140 */
		float max_width;		/* 360 */
		float max_height;	/* 0: context height minus edge margin */
		float submenu_delay;	/* 0.18 seconds */
		float disabled_mix;		/* 0.55: toward theme.disabled */
	} vgui_menu_style;

	typedef struct vgui_menubar_style {
		float item_height;	/* 0: fill the bar's inner height */
		float padding_x;		/* 4 */
		float padding_y;		/* 2 */
		float item_padding_x;	/* 10 */
		float gap;			/* 2 */
		float rounding;		/* 3 */
		float req_height;	/* 28: preferred bar height */
		float disabled_mix;	/* 0.55: toward theme.disabled */
	} vgui_menubar_style;

	typedef struct vgui_tooltip_style {
		float delay;			/* 0.5: seconds of hover before showing */
		float pointer_offset_x;	/* 14: pointer-anchored tooltip offset */
		float pointer_offset_y;	/* 20 */
		float widget_gap;		/* 6: gap to a widget-anchored tooltip */
		float padding_x;		/* 8 */
		float padding_y;		/* 5 */
		float rounding;			/* 4 */
		float border_width;		/* 1 */
		float max_width;		/* 320: text wraps beyond this */
		float margin;			/* 4: minimum distance from context edges */
		float line_gap;			/* 1: extra space between wrapped lines */
	} vgui_tooltip_style;

	typedef struct vgui_modal_style {
		float margin;			/* 48: minimum distance between the panel and context edges */
		float dialog_width;		/* 360: default width of convenience dialogs */
		float min_width;		/* 220: dialogs shrink below dialog_width only to this */
		float max_height;		/* 0: context height minus margin * 2 */
		float padding;			/* 16 */
		float rounding;			/* 6 */
		float border_width;		/* 1 */
		float backdrop_alpha;		/* 0.45: dim strength; 0 hides the backdrop */
		float title_gap;		/* 6: title-to-message gap */
		float message_gap;		/* 10: message-to-content gap */
		float button_gap;		/* 8: gap between dialog buttons */
		float button_top_gap;		/* 8: extra gap above the button row */
	} vgui_modal_style;

	typedef struct vgui_toast_style {
		float duration;			/* 3: seconds; 0 keeps a toast until dismissed */
		float max_width;		/* 360: text wraps beyond this */
		float padding_x;		/* 12 */
		float padding_y;		/* 8 */
		float rounding;			/* 6 */
		float border_width;		/* 1 */
		float margin;			/* 12: distance from context edges */
		float gap;			/* 8: gap between stacked toasts */
		float line_gap;			/* 1: extra space between wrapped lines */
		int max_visible;		/* 3: older toasts wait until a slot frees up */
	} vgui_toast_style;

	typedef struct vgui_image_style {
		float req_width;		/* 240: preferred size when no texture is set */
		float req_height;		/* 160 */
		float border_width;		/* 1 */
		float rounding;		/* 0 */
		float disabled_mix;		/* 0.6: toward theme.disabled */
	} vgui_image_style;

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
		vgui_list_style list;
		vgui_tree_style tree;
		vgui_menu_style menu;
		vgui_menubar_style menubar;
		vgui_image_style image;
		/* Appended at the end to keep positional initialisers working. */
		vgui_tooltip_style tooltip;
		vgui_modal_style modal;
		vgui_toast_style toast;
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

	/* ------------------------------------------------------------------------- */
	/* Textures                                                                  */
	/* ------------------------------------------------------------------------- */

	/* CPU texture.  pixels are copied; pass NULL to create an empty texture that
	 * can be filled later with vgui_texture_update().  The GPU upload is lazy: the
	 * first image draw uploads the pixels, and static textures are uploaded once. */
	VGUI_API vgui_texture* vgui_texture_create(vgui_context* ctx, const vgui_texture_desc* desc);
	VGUI_API int vgui_texture_update(vgui_texture* texture, const vgui_texture_desc* desc);
	VGUI_API void vgui_texture_destroy(vgui_texture* texture);
	VGUI_API int vgui_texture_get_width(const vgui_texture* texture);
	VGUI_API int vgui_texture_get_height(const vgui_texture* texture);

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

	/* Tooltip properties.  set_tooltip() stores static text; a provider, when
	 * set, takes precedence and may return different text each time.  A negative
	 * delay inherits style.tooltip.delay; zero shows immediately.  Disabled
	 * widgets keep their tooltip by default; clear when_disabled to suppress it. */
	VGUI_API void vgui_widget_set_tooltip(vgui_widget* widget, const char* text);
	VGUI_API const char* vgui_widget_get_tooltip(vgui_widget* widget);
	VGUI_API void vgui_widget_set_tooltip_provider(vgui_widget* widget, vgui_tooltip_text_fn fn, void* user);
	VGUI_API void vgui_widget_set_tooltip_delay(vgui_widget* widget, float seconds);
	VGUI_API float vgui_widget_get_tooltip_delay(vgui_widget* widget);
	VGUI_API void vgui_widget_set_tooltip_enabled(vgui_widget* widget, int enabled);
	VGUI_API int vgui_widget_get_tooltip_enabled(vgui_widget* widget);
	VGUI_API void vgui_widget_set_tooltip_when_disabled(vgui_widget* widget, int enabled);
	VGUI_API int vgui_widget_get_tooltip_when_disabled(vgui_widget* widget);
	VGUI_API void vgui_widget_set_tooltip_anchor(vgui_widget* widget, vgui_tooltip_anchor anchor);
	VGUI_API vgui_tooltip_anchor vgui_widget_get_tooltip_anchor(vgui_widget* widget);

	/* The running tooltip is owned by the context because it floats above the
	 * tree.  hide_tooltip() also cancels the pending hover, so it will not
	 * reappear until the pointer moves to another target. */
	VGUI_API void vgui_context_set_tooltip_enabled(vgui_context* ctx, int enabled);
	VGUI_API int vgui_context_get_tooltip_enabled(vgui_context* ctx);
	VGUI_API void vgui_context_hide_tooltip(vgui_context* ctx);
	VGUI_API int vgui_context_tooltip_visible(vgui_context* ctx);
	VGUI_API const char* vgui_context_tooltip_text(vgui_context* ctx);
	VGUI_API vgui_widget* vgui_context_tooltip_owner(vgui_context* ctx);
	/* Returns non-zero while a tooltip is visible and writes its current screen
	 * rectangle.  Any output pointer may be NULL. */
	VGUI_API int vgui_context_tooltip_rect(vgui_context* ctx, float* x, float* y, float* width, float* height);

	/* ------------------------------------------------------------------------- */
	/* Image control                                                             */
	/* ------------------------------------------------------------------------- */
	/* set_texture(NULL) clears the binding.  source width/height <= 0 select the
	 * full texture.  flip is applied by the draw path. */
	VGUI_API void vgui_image_set_texture(vgui_widget* widget, vgui_texture* texture);
	VGUI_API void vgui_image_set_source(vgui_widget* widget, int x, int y, int width, int height);
	VGUI_API void vgui_image_set_fit(vgui_widget* widget, vgui_image_fit fit);
	VGUI_API void vgui_image_set_tint(vgui_widget* widget, vgui_color color);
	VGUI_API void vgui_image_set_flip(vgui_widget* widget, int flip_x, int flip_y);
	VGUI_API void vgui_image_set_pixel_snap(vgui_widget* widget, int enabled);

	/* ------------------------------------------------------------------------- */
	/* Button icon                                                               */
	/* ------------------------------------------------------------------------- */

	/*
	 * 给 VGUI_BUTTON 设置一个 icon 纹理。tex 为 NULL 时清除。
	 * icon_w/icon_h 指定在按钮内绘制时的显示尺寸（<= 0 时使用纹理本身尺寸）。
	 * pos = LEFT(默认) / RIGHT / TOP / BOTTOM。
	 * 设置后按钮 intrinsic size 会包含 icon 尺寸（与 text 之间自动留 icon_gap）。
	 */
	VGUI_API void vgui_button_set_icon(vgui_widget* widget, vgui_texture* tex, int icon_w, int icon_h, vgui_button_icon_pos pos);
	/* 覆盖默认 icon-to-text gap（默认取 style.button.icon_gap）。<=0 则回退到 style。 */
	VGUI_API void vgui_button_set_icon_gap(vgui_widget* widget, float gap);
	/* tint 用 0 恢复默认（不染色）；disabled 态会自动混入 disabled_mix。 */
	VGUI_API void vgui_button_set_icon_tint(vgui_widget* widget, vgui_color tint);

	/* ------------------------------------------------------------------------- */
	/* Button style                                                              */
	/* ------------------------------------------------------------------------- */

	typedef enum vgui_button_style_kind {
		VGUI_BUTTON_STYLE_SOLID = 0,	/* 默认：纯色填充 + 纯色边框，主题色驱动 */
		VGUI_BUTTON_STYLE_GRADIENT	/* 渐变填充 + 双层高光/阴影边框（legacy 风格） */
	} vgui_button_style_kind;

	/* 设置按钮风格。kind 默认 SOLID，与现有行为一致。*/
	VGUI_API void vgui_button_set_style(vgui_widget* widget, vgui_button_style_kind kind);
	VGUI_API vgui_button_style_kind vgui_button_get_style(vgui_widget* widget);

	/*
	 * 为 GRADIENT 风格设置三组独立的渐变起止色（均为 0xAARRGGBB 格式）：
	 *   grad_top_default / grad_bot_default — 默认态
	 *   grad_top_hover   / grad_bot_hover   — 鼠标悬停态
	 *   grad_top_active  / grad_bot_active  — 按下态
	 *   border_light / border_dark          — 双层高光/阴影边框颜色
	 * 不调用则使用内置默认渐变（深灰系）。
	 */
	VGUI_API void vgui_button_set_gradient(vgui_widget* widget, vgui_color grad_top_default, vgui_color grad_bot_default, vgui_color grad_top_hover, vgui_color grad_bot_hover, vgui_color grad_top_active, vgui_color grad_bot_active, vgui_color border_light, vgui_color border_dark);

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
	VGUI_API void vgui_widget_set_align_content(vgui_widget* widget, vgui_align_content align);
	VGUI_API vgui_align_content vgui_widget_get_align_content(vgui_widget* widget);
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

	/* Splitter: a two-pane container.  vertical=0 lays the first visible child
	 * on the left and the second on the right; vertical=1 lays them out top and
	 * bottom.  ratio is the fraction of the main axis assigned to the first
	 * visible child (0..1).  Dragging the sash changes the ratio; double-clicking
	 * it resets to the default.  Child min_w/min_h are respected. */
	VGUI_API void vgui_splitter_set_vertical(vgui_widget* splitter, int vertical);
	VGUI_API int vgui_splitter_get_vertical(vgui_widget* splitter);
	VGUI_API void vgui_splitter_set_ratio(vgui_widget* splitter, float ratio);
	VGUI_API float vgui_splitter_get_ratio(vgui_widget* splitter);
	VGUI_API void vgui_splitter_set_sash_size(vgui_widget* splitter, float size);
	VGUI_API float vgui_splitter_get_sash_size(vgui_widget* splitter);
	VGUI_API void vgui_splitter_reset(vgui_widget* splitter);

	/* TabView: a page container with a tab strip.  vgui_tab_add() attaches and
	 * transfers ownership of page to the tab view.  The tab view controls the
	 * page's visibility; removing a tab destroys its page.  A close callback can
	 * veto or defer removal; without a callback closeable tabs close immediately.
	 * vgui_tab_get_rect() returns absolute window coordinates. */
	VGUI_API vgui_item_id vgui_tab_add(vgui_widget* tabs, const char* title, vgui_widget* page);
	VGUI_API int vgui_tab_remove(vgui_widget* tabs, vgui_item_id item);
	VGUI_API void vgui_tab_clear(vgui_widget* tabs);
	VGUI_API int vgui_tab_get_count(vgui_widget* tabs);
	VGUI_API vgui_item_id vgui_tab_get_at(vgui_widget* tabs, int index);
	VGUI_API int vgui_tab_get_index(vgui_widget* tabs, vgui_item_id item);
	VGUI_API vgui_item_id vgui_tab_get_active(vgui_widget* tabs);
	VGUI_API int vgui_tab_set_active(vgui_widget* tabs, vgui_item_id item);
	VGUI_API vgui_widget* vgui_tab_get_page(vgui_widget* tabs, vgui_item_id item);
	VGUI_API const char* vgui_tab_get_title(vgui_widget* tabs, vgui_item_id item);
	VGUI_API void vgui_tab_set_title(vgui_widget* tabs, vgui_item_id item, const char* title);
	VGUI_API int vgui_tab_get_enabled(vgui_widget* tabs, vgui_item_id item);
	VGUI_API void vgui_tab_set_enabled(vgui_widget* tabs, vgui_item_id item, int enabled);
	VGUI_API int vgui_tab_get_closeable(vgui_widget* tabs, vgui_item_id item);
	VGUI_API void vgui_tab_set_closeable(vgui_widget* tabs, vgui_item_id item, int closeable);
	VGUI_API int vgui_tab_get_unsaved(vgui_widget* tabs, vgui_item_id item);
	VGUI_API void vgui_tab_set_unsaved(vgui_widget* tabs, vgui_item_id item, int unsaved);
	VGUI_API int vgui_tab_move(vgui_widget* tabs, vgui_item_id item, int index);
	VGUI_API int vgui_tab_get_rect(vgui_widget* tabs, vgui_item_id item, float* x, float* y, float* width, float* height);
	VGUI_API void vgui_tab_set_tab_height(vgui_widget* tabs, float height);
	VGUI_API float vgui_tab_get_tab_height(vgui_widget* tabs);
	VGUI_API void vgui_tab_on_select(vgui_widget* tabs, vgui_item_fn fn, void* user);
	VGUI_API void vgui_tab_on_close(vgui_widget* tabs, vgui_item_fn fn, void* user);

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
	/* List / tree model items                                                   */
	/* ------------------------------------------------------------------------- */

	/* Lists and trees are data-model controls: their items are records with stable
	 * vgui_item_id values, not child widgets.  The first version supports selection
	 * only (no per-row checkboxes).  Returned ids are zero on failure. */

	VGUI_API vgui_item_id vgui_list_add_item(vgui_widget* list, const char* text, void* user);
	VGUI_API vgui_item_id vgui_list_insert_item(vgui_widget* list, int index, const char* text, void* user);
	VGUI_API int vgui_list_remove_item(vgui_widget* list, vgui_item_id item);
	VGUI_API void vgui_list_clear(vgui_widget* list);
	VGUI_API int vgui_list_get_count(vgui_widget* list);
	VGUI_API vgui_item_id vgui_list_get_item_at(vgui_widget* list, int index);

	VGUI_API void vgui_list_set_item_text(vgui_widget* list, vgui_item_id item, const char* text);
	VGUI_API const char* vgui_list_get_item_text(vgui_widget* list, vgui_item_id item);
	VGUI_API void vgui_list_set_item_detail(vgui_widget* list, vgui_item_id item, const char* detail);
	VGUI_API const char* vgui_list_get_item_detail(vgui_widget* list, vgui_item_id item);
	VGUI_API void vgui_list_set_item_enabled(vgui_widget* list, vgui_item_id item, int enabled);
	VGUI_API int vgui_list_get_item_enabled(vgui_widget* list, vgui_item_id item);
	VGUI_API void vgui_list_set_item_user(vgui_widget* list, vgui_item_id item, void* user);
	VGUI_API void* vgui_list_get_item_user(vgui_widget* list, vgui_item_id item);

	VGUI_API void vgui_list_set_selection_mode(vgui_widget* list, vgui_selection_mode mode);
	VGUI_API vgui_selection_mode vgui_list_get_selection_mode(vgui_widget* list);
	VGUI_API int vgui_list_select(vgui_widget* list, vgui_item_id item, int selected);
	VGUI_API void vgui_list_clear_selection(vgui_widget* list);
	VGUI_API int vgui_list_is_selected(vgui_widget* list, vgui_item_id item);
	VGUI_API int vgui_list_get_selected_count(vgui_widget* list);
	VGUI_API vgui_item_id vgui_list_get_selected(vgui_widget* list, int index);
	VGUI_API void vgui_list_set_cursor(vgui_widget* list, vgui_item_id item);
	VGUI_API vgui_item_id vgui_list_get_cursor(vgui_widget* list);
	VGUI_API void vgui_list_scroll_to_item(vgui_widget* list, vgui_item_id item, vgui_scroll_align align);
	VGUI_API void vgui_list_set_empty_text(vgui_widget* list, const char* text);
	VGUI_API void vgui_list_on_select(vgui_widget* list, vgui_item_select_fn fn, void* user);
	VGUI_API void vgui_list_on_activate(vgui_widget* list, vgui_item_fn fn, void* user);

	VGUI_API vgui_item_id vgui_tree_add_node(vgui_widget* tree, vgui_item_id parent, const char* text, void* user);
	VGUI_API int vgui_tree_remove_node(vgui_widget* tree, vgui_item_id node);
	VGUI_API void vgui_tree_clear(vgui_widget* tree);
	VGUI_API int vgui_tree_get_node_count(vgui_widget* tree);
	VGUI_API int vgui_tree_get_root_count(vgui_widget* tree);
	VGUI_API vgui_item_id vgui_tree_get_root(vgui_widget* tree, int index);
	VGUI_API int vgui_tree_get_child_count(vgui_widget* tree, vgui_item_id node);
	VGUI_API vgui_item_id vgui_tree_get_child(vgui_widget* tree, vgui_item_id node, int index);
	VGUI_API vgui_item_id vgui_tree_get_parent(vgui_widget* tree, vgui_item_id node);
	VGUI_API int vgui_tree_get_depth(vgui_widget* tree, vgui_item_id node);

	VGUI_API void vgui_tree_set_item_text(vgui_widget* tree, vgui_item_id node, const char* text);
	VGUI_API const char* vgui_tree_get_item_text(vgui_widget* tree, vgui_item_id node);
	VGUI_API void vgui_tree_set_item_enabled(vgui_widget* tree, vgui_item_id node, int enabled);
	VGUI_API int vgui_tree_get_item_enabled(vgui_widget* tree, vgui_item_id node);
	VGUI_API void vgui_tree_set_item_user(vgui_widget* tree, vgui_item_id node, void* user);
	VGUI_API void* vgui_tree_get_item_user(vgui_widget* tree, vgui_item_id node);

	VGUI_API void vgui_tree_set_item_expanded(vgui_widget* tree, vgui_item_id node, int expanded);
	VGUI_API int vgui_tree_get_item_expanded(vgui_widget* tree, vgui_item_id node);
	VGUI_API int vgui_tree_expand(vgui_widget* tree, vgui_item_id node);
	VGUI_API int vgui_tree_collapse(vgui_widget* tree, vgui_item_id node);
	VGUI_API int vgui_tree_toggle(vgui_widget* tree, vgui_item_id node);
	VGUI_API void vgui_tree_expand_all(vgui_widget* tree);
	VGUI_API void vgui_tree_collapse_all(vgui_widget* tree);
	VGUI_API void vgui_tree_expand_to(vgui_widget* tree, vgui_item_id node);

	VGUI_API void vgui_tree_set_selection_mode(vgui_widget* tree, vgui_selection_mode mode);
	VGUI_API vgui_selection_mode vgui_tree_get_selection_mode(vgui_widget* tree);
	VGUI_API int vgui_tree_select(vgui_widget* tree, vgui_item_id node, int selected);
	VGUI_API void vgui_tree_clear_selection(vgui_widget* tree);
	VGUI_API int vgui_tree_is_selected(vgui_widget* tree, vgui_item_id node);
	VGUI_API int vgui_tree_get_selected_count(vgui_widget* tree);
	VGUI_API vgui_item_id vgui_tree_get_selected(vgui_widget* tree, int index);
	VGUI_API void vgui_tree_set_cursor(vgui_widget* tree, vgui_item_id node);
	VGUI_API vgui_item_id vgui_tree_get_cursor(vgui_widget* tree);
	VGUI_API void vgui_tree_scroll_to_node(vgui_widget* tree, vgui_item_id node, vgui_scroll_align align);
	VGUI_API void vgui_tree_set_empty_text(vgui_widget* tree, const char* text);
	VGUI_API void vgui_tree_on_select(vgui_widget* tree, vgui_item_select_fn fn, void* user);
	VGUI_API void vgui_tree_on_activate(vgui_widget* tree, vgui_item_fn fn, void* user);

	/* ------------------------------------------------------------------------- */
	/* Optional virtual row source                                               */
	/* ------------------------------------------------------------------------- */

	/* A row source turns a list into a read-only virtual view over rows owned by
	 * the application.  While a source is attached the control stores no rows:
	 * layout, drawing, hit testing and keyboard navigation only query the
	 * visible index range through these callbacks, so row count is unbounded.
	 *
	 * Contract:
	 *   - All callbacks run on the UI thread, inside layout / draw / input
	 *     handling.  They must be fast and must not mutate the widget.
	 *   - row_info() fills text / detail / item_user and returns non-zero for a
	 *     valid row.  The returned pointers only have to stay valid until the
	 *     next callback.
	 *   - row_id_index() may return -1 when the id is unknown; when the callback
	 *     is NULL the core falls back to a linear scan over row_id().
	 *   - row_enabled / row_max_width are optional.  Without row_max_width the
	 *     control skips the widest-row measurement and falls back to the style
	 *     width, which avoids a full scan of the data source.
	 *
	 * While a source is attached the model mutation APIs (add / insert / remove /
	 * clear / set_item_text / set_item_detail / set_item_enabled / set_item_user)
	 * are no-ops, and get_count / get_item_* are answered from the callbacks.
	 * Call vgui_list_source_changed() after the backing data changes.
	 *
	 * Trees use the same struct with a different row meaning: a row is one
	 * *visible* row of the flattened tree, in display order, and the application
	 * owns the expansion state.
	 *   - row_depth(index)        indentation level; required for trees, 0 for lists.
	 *   - row_has_children(index) draws the expand arrow; optional, default false.
	 *   - row_expanded(index)     arrow direction; optional, default collapsed.
	 *   - row_toggle(index)       called when the user clicks the arrow (or uses
	 *                             Space / Left / Right); optional.  The callback
	 *                             must update its own expansion state and call
	 *                             vgui_tree_source_changed().
	 *   - row_info's detail is ignored by trees.
	 * Tree mutation APIs (add_node / remove_node / clear / set_item_expanded /
	 * expand* / collapse*) are no-ops while a source is attached. */
	typedef struct vgui_row_source
	{
		int (*row_count)(void* user);
		int (*row_info)(void* user, int index, const char** text, const char** detail,
			void** item_user);
		vgui_item_id (*row_id)(void* user, int index);
		int (*row_id_index)(void* user, vgui_item_id item);
		int (*row_enabled)(void* user, int index);
		float (*row_max_width)(void* user);
		int (*row_depth)(void* user, int index);
		int (*row_has_children)(void* user, int index);
		int (*row_expanded)(void* user, int index);
		void (*row_toggle)(void* user, int index);
		void* user;
		/* Optional row tooltip.  Return NULL/"" for no tooltip.  The returned
		 * pointer only has to stay valid until the next callback. */
		const char* (*row_tooltip)(void* user, int index);
	} vgui_row_source;

	VGUI_API void vgui_list_set_row_source(vgui_widget* list, const vgui_row_source* source);
	VGUI_API int vgui_list_is_virtual(vgui_widget* list);
	VGUI_API void vgui_list_source_changed(vgui_widget* list);

	VGUI_API void vgui_tree_set_row_source(vgui_widget* tree, const vgui_row_source* source);
	VGUI_API int vgui_tree_is_virtual(vgui_widget* tree);
	VGUI_API void vgui_tree_source_changed(vgui_widget* tree);

	/* ------------------------------------------------------------------------- */
	/* Menu                                                                      */
	/* ------------------------------------------------------------------------- */

	/* Menus are context-level popups.  They are not attached as layout children;
	 * one top-level menu chain can be open per context.  The context draws the menu
	 * above the widget tree and consumes outside presses while it is open. */

	VGUI_API vgui_item_id vgui_menu_add_item(vgui_widget* menu, const char* text);
	VGUI_API vgui_item_id vgui_menu_add_check_item(vgui_widget* menu, const char* text, int checked);
	VGUI_API vgui_item_id vgui_menu_add_separator(vgui_widget* menu);
	VGUI_API vgui_item_id vgui_menu_add_submenu(vgui_widget* menu, const char* text, vgui_widget** out_submenu);
	VGUI_API int vgui_menu_remove_item(vgui_widget* menu, vgui_item_id item);
	VGUI_API void vgui_menu_clear(vgui_widget* menu);
	VGUI_API int vgui_menu_get_count(vgui_widget* menu);
	VGUI_API vgui_item_id vgui_menu_get_item_at(vgui_widget* menu, int index);
	VGUI_API vgui_menu_item_type vgui_menu_get_item_type(vgui_widget* menu, vgui_item_id item);
	VGUI_API vgui_widget* vgui_menu_get_submenu(vgui_widget* menu, vgui_item_id item);

	VGUI_API void vgui_menu_set_item_text(vgui_widget* menu, vgui_item_id item, const char* text);
	VGUI_API const char* vgui_menu_get_item_text(vgui_widget* menu, vgui_item_id item);
	VGUI_API void vgui_menu_set_item_shortcut(vgui_widget* menu, vgui_item_id item, const char* shortcut);
	VGUI_API const char* vgui_menu_get_item_shortcut(vgui_widget* menu, vgui_item_id item);
	VGUI_API void vgui_menu_set_item_enabled(vgui_widget* menu, vgui_item_id item, int enabled);
	VGUI_API int vgui_menu_get_item_enabled(vgui_widget* menu, vgui_item_id item);
	VGUI_API void vgui_menu_set_item_checked(vgui_widget* menu, vgui_item_id item, int checked);
	VGUI_API int vgui_menu_get_item_checked(vgui_widget* menu, vgui_item_id item);
	VGUI_API int vgui_menu_set_item_checkable(vgui_widget* menu, vgui_item_id item, int checkable);
	VGUI_API int vgui_menu_get_item_checkable(vgui_widget* menu, vgui_item_id item);
	VGUI_API void vgui_menu_set_item_user(vgui_widget* menu, vgui_item_id item, void* user);
	VGUI_API void* vgui_menu_get_item_user(vgui_widget* menu, vgui_item_id item);

	VGUI_API void vgui_menu_open(vgui_widget* menu, float x, float y);
	VGUI_API void vgui_menu_open_below(vgui_widget* menu, vgui_widget* anchor);
	VGUI_API void vgui_menu_open_for(vgui_widget* menu, vgui_widget* anchor, float local_x, float local_y);
	VGUI_API void vgui_menu_close(vgui_widget* menu);
	VGUI_API int vgui_menu_is_open(vgui_widget* menu);
	VGUI_API vgui_widget* vgui_context_get_open_menu(vgui_context* ctx);
	VGUI_API void vgui_menu_set_width(vgui_widget* menu, float width);
	VGUI_API float vgui_menu_get_width(vgui_widget* menu);
	VGUI_API void vgui_menu_set_max_height(vgui_widget* menu, float height);
	VGUI_API float vgui_menu_get_max_height(vgui_widget* menu);
	VGUI_API void vgui_menu_set_close_on_activate(vgui_widget* menu, int close);
	VGUI_API int vgui_menu_get_close_on_activate(vgui_widget* menu);
	VGUI_API void vgui_menu_on_activate(vgui_widget* menu, vgui_item_fn fn, void* user);
	VGUI_API void vgui_menu_on_open(vgui_widget* menu, vgui_widget_fn fn, void* user);
	VGUI_API void vgui_menu_on_close(vgui_widget* menu, vgui_widget_fn fn, void* user);

	/* ------------------------------------------------------------------------- */
	/* MenuBar                                                                   */
	/* ------------------------------------------------------------------------- */

	/* MenuBar is a regular layout child: a horizontal strip of clickable titles.
	 * Each title owns one popup menu (VGUI_MENU).  Adding a menu transfers its
	 * ownership to the bar, so removing an item or destroying the bar destroys
	 * the associated popup.  Clicking a title opens its menu below the title;
	 * hovering another title while a menu is open switches menus (as do the left
	 * and right arrow keys).  The bar keeps no focus; the popup menu owns input
	 * while it is open. */

	VGUI_API vgui_item_id vgui_menubar_add_item(vgui_widget* menubar, const char* title);
	VGUI_API vgui_item_id vgui_menubar_add_menu(vgui_widget* menubar, const char* title, vgui_widget* menu);
	VGUI_API int vgui_menubar_remove_item(vgui_widget* menubar, vgui_item_id item);
	VGUI_API void vgui_menubar_clear(vgui_widget* menubar);
	VGUI_API int vgui_menubar_get_count(vgui_widget* menubar);
	VGUI_API vgui_item_id vgui_menubar_get_item_at(vgui_widget* menubar, int index);

	VGUI_API const char* vgui_menubar_get_item_text(vgui_widget* menubar, vgui_item_id item);
	VGUI_API void vgui_menubar_set_item_text(vgui_widget* menubar, vgui_item_id item, const char* text);
	VGUI_API int vgui_menubar_get_item_enabled(vgui_widget* menubar, vgui_item_id item);
	VGUI_API void vgui_menubar_set_item_enabled(vgui_widget* menubar, vgui_item_id item, int enabled);
	VGUI_API vgui_widget* vgui_menubar_get_menu(vgui_widget* menubar, vgui_item_id item);

	/* Item rect in absolute window coordinates.  The bar must have been laid
	 * out at least once (call vgui_layout() or vgui_update() first). */
	VGUI_API int vgui_menubar_get_item_rect(vgui_widget* menubar, vgui_item_id item, float* x, float* y, float* width, float* height);
	/* Title hit test using local coordinates; returns 0 when the point is not
	 * over an item. */
	VGUI_API vgui_item_id vgui_menubar_item_at(vgui_widget* menubar, float local_x, float local_y);

	/* Per-row tooltip.  Like other model setters these are no-ops while a
	 * virtual row source is attached; source rows use row_tooltip instead. */
	VGUI_API void vgui_list_set_item_tooltip(vgui_widget* list, vgui_item_id item, const char* tooltip);
	VGUI_API const char* vgui_list_get_item_tooltip(vgui_widget* list, vgui_item_id item);
	VGUI_API void vgui_tree_set_item_tooltip(vgui_widget* tree, vgui_item_id node, const char* tooltip);
	VGUI_API const char* vgui_tree_get_item_tooltip(vgui_widget* tree, vgui_item_id node);

	/* Row hit testing in widget-local coordinates.  Returns 0 when the point is
	 * outside the content area, over padding, or past the last row. */
	VGUI_API vgui_item_id vgui_list_item_at_point(vgui_widget* list, float local_x, float local_y);
	VGUI_API vgui_item_id vgui_tree_item_at_point(vgui_widget* tree, float local_x, float local_y);

	VGUI_API vgui_item_id vgui_menubar_get_active(vgui_widget* menubar);
	VGUI_API int vgui_menubar_open_item(vgui_widget* menubar, vgui_item_id item);
	VGUI_API void vgui_menubar_close(vgui_widget* menubar);

	/* ------------------------------------------------------------------------- */
	/* Focus and input                                                           */
	/* ------------------------------------------------------------------------- */

	VGUI_API void vgui_set_focus(vgui_context* ctx, vgui_widget* widget);
	VGUI_API vgui_widget* vgui_get_focus(vgui_context* ctx);
	VGUI_API vgui_widget* vgui_hit_test(vgui_context* ctx, float x, float y);
	VGUI_API vgui_cursor vgui_context_get_cursor(vgui_context* ctx);

	/* Register a context-level shortcut filter.  Returns a non-zero token that
	 * can be passed to vgui_context_remove_key_handler().  Handlers run in
	 * reverse registration order (the most recently added handler wins). */
	VGUI_API int vgui_context_add_key_handler(vgui_context* ctx, vgui_key_handler_fn fn, void* user);
	VGUI_API void vgui_context_remove_key_handler(vgui_context* ctx, int token);

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
	/* Custom widget                                                             */
	/* ------------------------------------------------------------------------- */

	VGUI_API void vgui_custom_set_draw(vgui_widget* widget, vgui_custom_draw_fn draw, void* user);
	VGUI_API void vgui_custom_set_size(vgui_widget* widget, vgui_custom_size_fn size, void* user);
	VGUI_API void vgui_custom_set_event(vgui_widget* widget, vgui_custom_event_fn event, void* user);
	/* 1=裁剪到控件边界（默认），0=允许超出（发光/阴影等效果）。 */
	VGUI_API void vgui_custom_set_clip_content(vgui_widget* widget, int clip);

	/* ------------------------------------------------------------------------- */
	/* Frame                                                                     */
	/* ------------------------------------------------------------------------- */

	VGUI_API void vgui_layout(vgui_context* ctx);
	VGUI_API void vgui_update(vgui_context* ctx, float delta_time);
	VGUI_API void vgui_draw(vgui_context* ctx, struct ovg_ctx_cb* cb, struct rvg_t* vg);

#ifdef __cplusplus
} /* extern "C" */
#endif
