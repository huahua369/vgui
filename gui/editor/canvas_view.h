#pragma once
/*
 * canvas_view.h - standalone 2D canvas for scene and UI editors (V08).
 *
 * The canvas owns a small, renderer-independent item model (stable object_id,
 * unrotated bounds, rotation around the item centre), a selection set and a
 * pan/zoom view.  Unlike viewport_view, no host scene or projection is needed:
 * a canvas is useful on its own as a reusable 2D editing surface.
 *
 * Interaction:
 *  * LMB on an item selects it.  Ctrl toggles, Shift appends; dragging a
 *    selected item moves the whole selection.  LMB on empty space starts a
 *    marquee; the selection is applied on release.
 *  * A single selected item shows eight resize handles and a rotation handle.
 *    Handles are hit tested in content-local screen coordinates, so they stay
 *    constant-size while the canvas zooms.
 *  * MMB pans, the wheel zooms around the cursor.  The view is expressed as
 *    the canvas coordinate at the content origin plus a positive zoom factor.
 *  * Move/resize/rotate read snap_settings (borrowed) on every update.
 *  * Esc, VGUI_EVENT_CANCEL or focus loss restores the gesture snapshot.
 *
 * Command integration:
 *  * set_command_stack() makes every committed gesture push one command
 *    ("Move", "Resize" or "Rotate").  Previews are applied live; cancelling
 *    restores the snapshot without touching the stack.  Clicks and drags that
 *    do not change an item produce no command.
 *
 * The item model deliberately stores only presentation data.  Hosts keep the
 * document, synchronise it through change_listener and can rebuild the canvas
 * from their own model at any time.
 */
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "command_stack.h"
#include "editor_types.h"
#include "viewport_grid.h"
#include "vgui.h"

namespace vgui_editor_kit {

	struct canvas_point
	{
		float x = 0.0f;
		float y = 0.0f;
	};

	struct canvas_rect
	{
		float x = 0.0f;
		float y = 0.0f;
		float width = 0.0f;
		float height = 0.0f;

		float right() const noexcept { return x + width; }
		float bottom() const noexcept { return y + height; }
		bool empty() const noexcept { return width <= 0.0f || height <= 0.0f; }
	};

	enum class canvas_shape
	{
		rectangle,
		ellipse,
		diamond
	};

	/* Handle order matches the eight resize directions around the frame. */
	enum class canvas_handle
	{
		none,
		north_west,
		north,
		north_east,
		east,
		south_east,
		south,
		south_west,
		west,
		rotate
	};

	enum class canvas_gesture
	{
		none,
		pending_move,
		move,
		pending_marquee,
		marquee,
		resize,
		rotate,
		pan
	};

	struct canvas_item
	{
		/* Stable per-canvas id.  add_item() allocates one when this is 0. */
		object_id id = invalid_object;
		/* Unrotated rectangle in canvas coordinates; rotation is around its
		 * centre. */
		canvas_rect bounds{ 0.0f, 0.0f, 100.0f, 60.0f };
		float rotation = 0.0f;	/* radians, clockwise in screen space */
		canvas_shape shape = canvas_shape::rectangle;
		vgui_color fill = 0xFF2E4A6A;		/* 0xAARRGGBB */
		vgui_color border = 0xFF7FB6FF;
		vgui_color text_color = 0xFFE8EEF8;
		float border_width = 1.0f;
		float corner_radius = 0.0f;
		std::string label;
		bool visible = true;
		/* Locked items are drawn but ignored by hit testing and gestures. */
		bool locked = false;
	};

	struct canvas_view_state
	{
		/* Canvas coordinate displayed at the content top-left. */
		float offset_x = 0.0f;
		float offset_y = 0.0f;
		/* Positive scale from canvas units to content pixels. */
		float zoom = 1.0f;
	};

	class canvas_view
	{
	public:
		using selection_listener =
			std::function<void(const std::vector<object_id>&)>;
		using change_listener = std::function<void()>;

		canvas_view() = default;
		~canvas_view();

		canvas_view(const canvas_view&) = delete;
		canvas_view& operator=(const canvas_view&) = delete;

		void build(vgui_widget* parent);
		vgui_widget* widget() const noexcept;
		bool built() const noexcept;

		/* ---- model ---------------------------------------------------- */
		/* Returns the item id, or invalid_object for a duplicate explicit id. */
		object_id add_item(canvas_item item);
		bool update_item(const canvas_item& item);
		bool remove_item(object_id id);
		void clear_items();
		const std::vector<canvas_item>& items() const noexcept;
		const canvas_item* find_item(object_id id) const noexcept;
		canvas_item* find_item(object_id id) noexcept;

		/* ---- selection ------------------------------------------------ */
		void set_selection(const std::vector<object_id>& ids,
			object_id primary = invalid_object);
		void clear_selection();
		const std::vector<object_id>& selection_ids() const noexcept;
		object_id primary_selection() const noexcept;
		bool is_selected(object_id id) const noexcept;
		void set_multi_select(bool enabled);
		bool multi_select() const noexcept;
		void set_selection_listener(selection_listener listener);

		/* Fired for model, view and gesture changes (not for selection; use the
		 * selection listener for that). */
		void set_change_listener(change_listener listener);

		/* ---- view / grid ---------------------------------------------- */
		canvas_view_state view_state() const noexcept;
		/* Cancels an in-flight gesture so the view never changes underneath a
		 * transform snapshot. */
		void set_view_state(const canvas_view_state& state);
		float zoom() const noexcept;
		/* Zooms by `factor` while keeping the given content point fixed. */
		void zoom_at(float content_x, float content_y, float factor);
		void set_zoom_limits(float minimum, float maximum);
		void reset_view();

		void set_grid_visible(bool visible);
		bool grid_visible() const noexcept;
		/* Base grid spacing in canvas units before adaptive density. */
		void set_grid_step(float step);
		float grid_step() const noexcept;
		void set_grid_pixel_range(float minimum, float maximum);
		/* Grid spacing actually drawn at the current zoom. */
		float effective_grid_step() const noexcept;

		/* Borrowed; changes take effect immediately. */
		void set_snap_settings(const snap_settings* snap);
		void set_command_stack(command_stack* stack);
		command_stack* command_stack_ptr() const noexcept;

		/* ---- interaction ---------------------------------------------- */
		/* Content-local screen (inside the 1px border) <-> canvas. */
		canvas_point screen_to_canvas(float content_x, float content_y) const;
		canvas_point canvas_to_screen(float canvas_x, float canvas_y) const;
		/* Rotated item bounds (axis-aligned canvas rectangle). */
		canvas_rect item_bounds(object_id id) const;
		canvas_rect selection_bounds() const;
		/* Content-local coordinates; 0 when nothing selectable is hit. */
		object_id hit_test(float content_x, float content_y) const;
		canvas_handle handle_at(float content_x, float content_y) const;

		bool dragging() const noexcept;
		canvas_gesture gesture() const noexcept;
		canvas_handle active_handle() const noexcept;
		canvas_handle hovered_handle() const noexcept;
		/* Restores the live gesture snapshot; safe without a gesture. */
		void cancel_gesture();
		void set_drag_threshold(float pixels);
		float drag_threshold() const noexcept;
		void set_min_item_size(float size);

		void set_enabled(bool enabled);
		bool enabled() const noexcept;
		void request_focus();
		bool focused() const;

	private:
		static void on_draw(vgui_widget* widget, ovg_ctx_cb* cb, rvg_t* vg,
			float abs_x, float abs_y, float abs_w, float abs_h,
			float delta_time, void* user);
		static int on_event(vgui_widget* widget, const vgui_event* event,
			void* user);
		static void on_focus_change(vgui_context* ctx, vgui_widget* old_focus,
			vgui_widget* new_focus, void* user);

		bool content_geometry(float* offset_x, float* offset_y, float* width,
			float* height) const;
		float ui_scale() const;
		float effective_threshold() const;
		void notify_change();
		void notify_selection();
		void prune_selection();

		canvas_point item_point(const canvas_item& item, float local_x,
			float local_y) const;
		canvas_point item_handle_point(const canvas_item& item,
			canvas_handle handle) const;
		canvas_point rotate_handle_point(const canvas_item& item) const;
		bool item_contains(const canvas_item& item, canvas_point point) const;

		void apply_selection(const std::vector<object_id>& picked, int mods);
		std::vector<object_id> items_in_rect(canvas_rect rect) const;

		void capture_gesture_items();
		void begin_transform(canvas_handle handle, canvas_point point);
		void update_move(canvas_point point);
		void update_resize(canvas_point point);
		void update_rotate(canvas_point point);
		void finish_transform(canvas_gesture gesture, canvas_point point);
		void cancel_transform();
		void reset_gesture();
		bool gesture_changed() const;
		void apply_item_states(const std::vector<canvas_item>& states);
		void push_gesture_command(const char* name,
			const std::vector<canvas_item>& before,
			const std::vector<canvas_item>& after);
		float snap_translation(float value) const;

		void draw(ovg_ctx_cb* cb, rvg_t* vg, vgui_widget* widget, float x,
			float y, float width, float height, float delta_time);
		void draw_grid(ovg_ctx_cb* cb, rvg_t* vg, float x, float y,
			float width, float height, uint32_t minor, uint32_t major) const;
		void draw_item(ovg_ctx_cb* cb, rvg_t* vg, vgui_widget* widget,
			const canvas_item& item, float x, float y) const;
		void draw_selection(ovg_ctx_cb* cb, rvg_t* vg, float x, float y,
			uint32_t accent, uint32_t accent_hover) const;

		vgui_widget* widget_ = nullptr;
		vgui_context* context_ = nullptr;
		int focus_token_ = 0;

		std::vector<canvas_item> items_;
		object_id next_item_id_ = 1;
		std::vector<object_id> selection_;
		object_id primary_ = invalid_object;
		selection_listener selection_listener_;
		change_listener change_listener_;

		canvas_view_state view_{};
		float min_zoom_ = 0.05f;
		float max_zoom_ = 20.0f;
		bool grid_visible_ = true;
		float grid_step_ = 20.0f;
		float min_grid_pixels_ = 12.0f;
		float max_grid_pixels_ = 96.0f;
		float marquee_offset_ = 0.0f;

		const snap_settings* snap_ = nullptr;
		command_stack* commands_ = nullptr;

		bool enabled_ = true;
		bool multi_select_ = true;
		float drag_threshold_ = -1.0f;
		float min_item_size_ = 4.0f;
		float handle_size_ = 7.0f;
		float handle_tolerance_ = 8.0f;
		float rotate_handle_distance_ = 26.0f;

		canvas_gesture gesture_ = canvas_gesture::none;
		canvas_handle active_handle_ = canvas_handle::none;
		canvas_handle hovered_handle_ = canvas_handle::none;
		float press_x_ = 0.0f;
		float press_y_ = 0.0f;
		float marquee_x_ = 0.0f;
		float marquee_y_ = 0.0f;
		canvas_view_state gesture_view_{};
		std::vector<canvas_item> gesture_start_;
		float rotate_start_angle_ = 0.0f;

		mutable std::shared_ptr<int> alive_token_ = std::make_shared<int>(0);
	};

} /* namespace vgui_editor_kit */
