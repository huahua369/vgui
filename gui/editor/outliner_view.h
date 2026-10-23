#pragma once
/*
 * outliner_view.h - scene hierarchy skeleton backed by VGUI_TREE.
 *
 * The scene model stays in the host application.  This class owns only the
 * tree widget and maps tree item IDs to stable editor object IDs.
 */
#include <cstdint>
#include <functional>

#include "editor_context.h"
#include "vgui.h"

namespace vgui_editor_kit {

	class outliner_view
	{
	public:
		using selection_listener = std::function<void(object_id)>;

		void build(editor_context& context, vgui_widget* parent);

		void clear();
		vgui_item_id add_node(const char* text, vgui_item_id parent = 0,
			object_id object = invalid_object);
		void expand(vgui_item_id node);

		vgui_item_id selected() const noexcept;
		vgui_widget* widget() const noexcept;

		void set_selection_listener(selection_listener listener);

	private:
		static void on_select(vgui_widget* widget, vgui_item_id item, int selected,
			void* user);

		editor_context* context_ = nullptr;
		vgui_widget* tree_ = nullptr;
		vgui_item_id selected_ = 0;
		selection_listener listener_;
	};

} /* namespace vgui_editor_kit */
