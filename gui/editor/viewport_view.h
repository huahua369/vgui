#pragma once
/*
 * viewport_view.h - editor viewport skeleton.
 *
 * The host renders the scene into a native texture and binds it with
 * set_texture().  Camera navigation, picking and gizmos are separate MVP
 * steps built on top of this widget boundary.
 */
#include "vgui.h"

namespace vgui_editor_kit {

	class viewport_view
	{
	public:
		void build(vgui_widget* parent);

		void set_texture(vgui_texture* texture);
		vgui_texture* texture() const noexcept;

		vgui_widget* widget() const noexcept;

	private:
		vgui_widget* image_ = nullptr;
		vgui_texture* texture_ = nullptr;
	};

} /* namespace vgui_editor_kit */
