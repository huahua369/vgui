#pragma once
/*
 * viewport_view.h - editor viewport skeleton.
 *
 * The host renders the scene into a native texture and binds it with
 * set_texture().  Camera navigation, picking and gizmos are separate MVP
 * steps built on top of this widget boundary.
 *
 * F03: the viewport accepts asset / scene_object drops and reports them
 * through the drop listener.  A04: the listener returns whether the host
 * handled the drop (creating an object, V06) so rejected drops show no
 * feedback; hovering paints a compact rect marker at the cursor.
 */
#include <functional>
#include <memory>

#include "editor_types.h"
#include "vgui.h"

namespace vgui_editor_kit {

	class viewport_view
	{
	public:
		/* Return true when the drop was handled; false rejects it. */
		using drop_listener = std::function<bool(const drag_payload& payload,
			float x, float y)>;

		void build(vgui_widget* parent);

		void set_texture(vgui_texture* texture);
		vgui_texture* texture() const noexcept;

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
		vgui_widget* image_ = nullptr;
		vgui_texture* texture_ = nullptr;
		drop_listener listener_;
		drag_payload last_drop_;
		float last_drop_x_ = 0.0f;
		float last_drop_y_ = 0.0f;
		std::unique_ptr<drop_target> drop_target_;
	};

} /* namespace vgui_editor_kit */
