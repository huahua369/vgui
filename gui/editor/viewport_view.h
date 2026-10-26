#pragma once
/*
 * viewport_view.h - editor viewport surface (V01).
 *
 * The host renders the scene into a texture (CPU or native) and binds it with
 * set_texture().  The surface is a focusable VGUI_CUSTOM widget, so mouse,
 * keyboard and wheel events reach the host directly:
 *
 *  * resize_listener fires from poll_resize() after the layout produced a new
 *    integer render size.  The host rebuilds its FBO and updates the bound
 *    native texture in place; the previous texture stays bound, so resizing
 *    never leaves a blank frame.
 *  * event_listener receives every raw event.  After a press inside the
 *    surface, moves keep arriving with out-of-bounds local coordinates until
 *    the matching release (mouse capture).
 *  * focus_listener reports focus changes.  Losing focus while a gesture is
 *    running cancels it and forwards a synthetic VGUI_EVENT_CANCEL.
 *
 * V01 deliberately contains no camera, picking or gizmo math; V02/V03/V05
 * build those on top of this boundary.  The host owns the texture and the
 * framebuffer; the viewport only borrows the texture pointer.
 *
 * F03/A04: the viewport accepts asset / scene_object drops and reports them
 * through the drop listener.  A04: the listener returns whether the host
 * handled the drop (creating an object, V06) so rejected drops show no
 * feedback; hovering paints a compact rect marker at the cursor.
 */
#include <functional>
#include <memory>
#include <vector>

#include "editor_types.h"
#include "vgui.h"

namespace vgui_editor_kit {

	class viewport_view
	{
	public:
		/* Return true when the drop was handled; false rejects it. */
		using drop_listener = std::function<bool(const drag_payload& payload,
			float x, float y)>;

		/* New integer render size after layout.  Non-positive layout sizes are
		 * ignored, so a collapsed dock page never rebuilds a 1x1 target. */
		using resize_listener = std::function<void(int width, int height)>;

		/* Raw input.  x/y are local to the surface and may be negative or past
		 * width/height while a press is captured. */
		using event_listener = std::function<bool(const vgui_event* event)>;

		/* Focus entered (true) or left (false) the surface. */
		using focus_listener = std::function<void(bool focused)>;

		/* Overlay drawing on top of the bound texture.  x/y/width/height is the
		 * absolute content rect (the area inside the 1px border).  Three slots
		 * are drawn in a fixed order: selection highlights, interaction
		 * feedback (box-select marquee), then foreground (gizmos). */
		using overlay_draw_fn = std::function<void(ovg_ctx_cb* cb, rvg_t* vg,
			float x, float y, float width, float height, float delta_time)>;

		viewport_view() = default;
		~viewport_view();

		viewport_view(const viewport_view&) = delete;
		viewport_view& operator=(const viewport_view&) = delete;

		void build(vgui_widget* parent);

		/* The viewport borrows the pointer.  Native textures stay valid across
		 * FBO rebuilds through vgui_sdl3_texture_update_native(); CPU textures
		 * must outlive the binding. */
		void set_texture(vgui_texture* texture);
		vgui_texture* texture() const noexcept;

		/* Layout -> render size.  Call once per frame after vgui_layout() (or
		 * vgui_update()) and before rendering the scene.  Returns true when the
		 * size changed and the resize listener ran. */
		bool poll_resize();
		int render_width() const noexcept;
		int render_height() const noexcept;
		void get_render_size(int* width, int* height) const noexcept;
		/* Content rect in surface-local coordinates (excludes the 1px border). */
		void get_content_rect(float* x, float* y, float* width, float* height) const noexcept;
		void set_resize_listener(resize_listener listener);

		void set_selection_overlay(overlay_draw_fn draw);
		void set_interaction_overlay(overlay_draw_fn draw);
		void set_foreground_overlay(overlay_draw_fn draw);

		void set_event_listener(event_listener listener);
		/* Additional event observers used by components layered on the surface
		 * (viewport_camera, gizmos).  They run after the listener installed by
		 * set_event_listener(), in registration order; any observer may consume
		 * the event.  Returns a token for remove_event_listener(). */
		int add_event_listener(event_listener listener);
		void remove_event_listener(int token);
		void set_focus_listener(focus_listener listener);

		/* True between a mouse-down inside the surface and its matching
		 * release.  Moves outside the surface keep arriving in that span. */
		bool dragging() const noexcept;
		int drag_button() const noexcept;

		/* Ends the current gesture and forwards a synthetic VGUI_EVENT_CANCEL.
		 * Safe to call when no gesture is running. */
		void cancel_interaction();

		bool focused() const;
		void request_focus();

		/* F03/A04: accepts asset / scene_object drops. */
		void set_drop_listener(drop_listener listener);
		/* The last accepted drop, or an empty payload. */
		const drag_payload& last_drop() const noexcept;
		/* Absolute coordinates of the last accepted drop. */
		float last_drop_x() const noexcept;
		float last_drop_y() const noexcept;
		/* Records a drop (called by the internal drop target).  Returns false
		 * when no listener is installed or the host rejects the payload. */
		bool record_drop(const drag_payload& payload, float x, float y);

		vgui_widget* widget() const noexcept;

	private:
		static void on_draw(vgui_widget* widget, ovg_ctx_cb* cb, rvg_t* vg,
			float abs_x, float abs_y, float abs_w, float abs_h,
			float delta_time, void* user);
		static int on_event(vgui_widget* widget, const vgui_event* event,
			void* user);
		static void on_focus_change(vgui_context* ctx, vgui_widget* old_focus,
			vgui_widget* new_focus, void* user);

		void begin_gesture(const vgui_event& event);
		void end_gesture(bool cancelled);
		bool forward_event(const vgui_event* event);

		vgui_widget* surface_ = nullptr;
		vgui_context* context_ = nullptr;
		vgui_texture* texture_ = nullptr;

		struct event_listener_entry
		{
			int token = 0;
			event_listener fn;
		};

		resize_listener resize_listener_;
		overlay_draw_fn selection_overlay_;
		overlay_draw_fn interaction_overlay_;
		overlay_draw_fn foreground_overlay_;
		event_listener event_listener_;
		std::vector<event_listener_entry> event_listeners_;
		int next_event_listener_ = 1;
		focus_listener focus_listener_;

		int render_w_ = 0;
		int render_h_ = 0;
		int focus_token_ = 0;
		bool dragging_ = false;
		int drag_button_ = 0;
		float last_x_ = 0.0f;
		float last_y_ = 0.0f;

		drop_listener listener_;
		drag_payload last_drop_;
		float last_drop_x_ = 0.0f;
		float last_drop_y_ = 0.0f;
		std::unique_ptr<drop_target> drop_target_;
	};

} /* namespace vgui_editor_kit */
