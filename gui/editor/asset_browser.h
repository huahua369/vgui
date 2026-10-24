#pragma once
/*
 * asset_browser.h - list-based asset browser skeleton.
 *
 * Thumbnail grids, breadcrumbs, async thumbnails and cross-panel drag/drop are
 * later MVP steps.  This class establishes the component boundary, the
 * selection callback used by the shell and the command-integrated rename path.
 *
 * rename_item() routes through the rename handler and records an undoable
 * command when a command stack is attached.
 */
#include <functional>
#include <memory>
#include <string>

#include "command_stack.h"
#include "vgui.h"

namespace vgui_editor_kit {

	class asset_browser
	{
	public:
		using selection_listener = std::function<void(vgui_item_id)>;
		/* Applies the new name to the host asset model.  Return false to reject
		 * the rename; rejected renames are not recorded. */
		using rename_handler = std::function<bool(vgui_item_id item,
			const std::string& name)>;

		void build(vgui_widget* parent);

		void clear();
		vgui_item_id add_item(std::string text, std::string detail = {});

		vgui_item_id selected() const noexcept;
		vgui_widget* widget() const noexcept;

		void set_selection_listener(selection_listener listener);

		void set_command_stack(command_stack* stack);
		void set_rename_handler(rename_handler handler);
		/* Renames the asset through the rename handler and records an undoable
		 * "Rename asset" command.  Returns false when the rename is rejected. */
		bool rename_item(vgui_item_id item, const std::string& name);

		/* F03: rows drag as asset payloads carrying the asset name in
		 * drag_payload::id. */
		void enable_row_drag();

	private:
		class rename_command;

		bool apply_rename(vgui_item_id item, const std::string& name);

		static void on_select(vgui_widget* widget, vgui_item_id item, int selected,
			void* user);

		vgui_widget* list_ = nullptr;
		vgui_item_id selected_ = 0;
		selection_listener listener_;
		command_stack* command_stack_ = nullptr;
		rename_handler rename_handler_;
		std::shared_ptr<int> alive_token_ = std::make_shared<int>(0);
	};

} /* namespace vgui_editor_kit */
