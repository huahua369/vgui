#pragma once
/*
 * viewport_selection.h - picking and box selection for viewport_view (V03).
 *
 * The kit owns the interaction state machine, the selection rules and the
 * screen-space feedback; the host owns the actual hit testing.  This keeps
 * ray/AABB/box-intersection math out of the kit, as required by the roadmap.
 *
 *  * Left click: point_picker receives content-local coordinates and returns
 *    the hit object id (0 = nothing).
 *  * Left drag past the threshold: box_picker receives a normalised
 *    content-local rectangle and returns the hit object ids.
 *  * highlight_provider returns content-local screen boxes for the selected
 *    ids; the selection overlay draws them under the box-select marquee.
 *
 * Modifiers are read from vgui_context_get_mods() because vgui 0.3 mouse
 * events do not carry the mods field.  Ctrl toggles, Shift appends, no
 * modifier replaces (an empty hit clears).
 *
 * selection state lives in editor_context, so viewport, Outliner, property
 * grid and status bar all observe the same ordered set.
 */
#include <functional>
#include <vector>

#include "editor_context.h"
#include "editor_types.h"
#include "viewport_view.h"

namespace vgui_editor_kit {

	struct selection_point_query
	{
		float x = 0.0f;
		float y = 0.0f;
		int width = 0;
		int height = 0;
		int mods = 0;
	};

	struct selection_box_query
	{
		float x0 = 0.0f;
		float y0 = 0.0f;
		float x1 = 0.0f;
		float y1 = 0.0f;
		int width = 0;
		int height = 0;
		int mods = 0;
	};

	/* Screen box for one selected object, in viewport content coordinates. */
	struct selection_box
	{
		object_id id = invalid_object;
		float x = 0.0f;
		float y = 0.0f;
		float width = 0.0f;
		float height = 0.0f;
	};

	class viewport_selection
	{
	public:
		using point_picker = std::function<object_id(const selection_point_query&)>;
		using box_picker = std::function<std::vector<object_id>(
			const selection_box_query&)>;
		using highlight_provider = std::function<std::vector<selection_box>(
			const std::vector<object_id>& ids)>;

		viewport_selection() = default;
		~viewport_selection();

		viewport_selection(const viewport_selection&) = delete;
		viewport_selection& operator=(const viewport_selection&) = delete;

		void attach(viewport_view& viewport, editor_context& context);
		void detach();
		bool attached() const noexcept;

		void set_point_picker(point_picker picker);
		void set_box_picker(box_picker picker);
		void set_highlight_provider(highlight_provider provider);

		/* Disabling stops all picking gestures; an in-flight gesture is
		 * cancelled without changing the selection. */
		void set_enabled(bool enabled);
		bool enabled() const noexcept;

		/* false ignores Ctrl/Shift and always replaces the selection. */
		void set_multi_select(bool enabled);
		bool multi_select() const noexcept;

		/* Negative selects the vgui style dock.drag_threshold (default 4px). */
		void set_drag_threshold(float pixels);

		bool pending() const noexcept;
		bool selecting() const noexcept;
		/* Aborts the current gesture without changing the selection. */
		void cancel_selection();

		/* Mirrors editor_context::selection_ids(). */
		const std::vector<object_id>& selected_ids() const;

	private:
		enum class phase
		{
			idle,
			pending,
			box
		};

		int on_input(const vgui_event* event);
		float effective_threshold() const;
		void content_geometry(float* offset_x, float* offset_y,
			int* width, int* height) const;
		void apply_selection(const std::vector<object_id>& picked, int mods);
		void draw_selection(ovg_ctx_cb* cb, rvg_t* vg,
			float x, float y, float width, float height, float delta_time);
		void draw_interaction(ovg_ctx_cb* cb, rvg_t* vg,
			float x, float y, float width, float height, float delta_time);

		viewport_view* viewport_ = nullptr;
		editor_context* context_ = nullptr;
		int listener_token_ = 0;

		phase phase_ = phase::idle;
		float start_x_ = 0.0f;      /* content-local */
		float start_y_ = 0.0f;
		float current_x_ = 0.0f;
		float current_y_ = 0.0f;
		int width_ = 0;
		int height_ = 0;
		mutable float marquee_offset_ = 0.0f;

		bool enabled_ = true;
		bool multi_select_ = true;
		float drag_threshold_ = -1.0f;

		point_picker point_picker_;
		box_picker box_picker_;
		highlight_provider highlight_provider_;
	};

} /* namespace vgui_editor_kit */
