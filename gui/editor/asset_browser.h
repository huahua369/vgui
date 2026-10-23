#pragma once
/*
 * asset_browser.h - list-based asset browser skeleton.
 *
 * Thumbnail grids, breadcrumbs, async thumbnails and cross-panel drag/drop are
 * later MVP steps.  This class establishes the component boundary and selection
 * callback used by the shell.
 */
#include <functional>
#include <string>

#include "vgui.h"

namespace vgui_editor_kit {

	class asset_browser
	{
	public:
		using selection_listener = std::function<void(vgui_item_id)>;

		void build(vgui_widget* parent);

		void clear();
		vgui_item_id add_item(std::string text, std::string detail = {});

		vgui_item_id selected() const noexcept;
		vgui_widget* widget() const noexcept;

		void set_selection_listener(selection_listener listener);

	private:
		static void on_select(vgui_widget* widget, vgui_item_id item, int selected,
			void* user);

		vgui_widget* list_ = nullptr;
		vgui_item_id selected_ = 0;
		selection_listener listener_;
	};

} /* namespace vgui_editor_kit */
