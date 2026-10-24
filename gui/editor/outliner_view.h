#pragma once
/*
 * outliner_view.h - scene hierarchy skeleton backed by VGUI_TREE.
 *
 * The scene model stays in the host application.  This class owns only the
 * tree widget and maps tree item IDs to stable editor object IDs.
 *
 * move_node() routes through the move handler and records an undoable command
 * when a command stack is attached.  The handler applies the move to the host
 * model and mirrors it in this tree (e.g. rebuild via clear()/add_node()).
 */
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "command_stack.h"
#include "editor_context.h"
#include "editor_types.h"
#include "vgui.h"

namespace vgui_editor_kit {

	class outliner_view
	{
	public:
		using selection_listener = std::function<void(object_id)>;
		/* Applies an absolute move of node under parent at index.  Return false
		 * to reject the move; rejected moves are not recorded. */
		using move_handler = std::function<bool(object_id node, object_id parent,
			std::size_t index)>;

		void build(editor_context& context, vgui_widget* parent);

		void clear();
		vgui_item_id add_node(const char* text, vgui_item_id parent = 0,
			object_id object = invalid_object);
		void expand(vgui_item_id node);

		vgui_item_id selected() const noexcept;
		vgui_widget* widget() const noexcept;

		void set_selection_listener(selection_listener listener);

		void set_command_stack(command_stack* stack);
		void set_move_handler(move_handler handler);
		/* Moves node under new_parent at new_index through the move handler and
		 * records an undoable "Move node" command.  Returns false when the move
		 * is rejected or no handler is set. */
		bool move_node(object_id node, object_id new_parent, std::size_t new_index);

		/* F03 drag & drop: rows drag as scene_object payloads carrying the
		 * stable object id (decimal text in drag_payload::id); dropping on the
		 * tree routes through move_node(). */
		void enable_drag_drop();
		/* Resolves a drop point to the parent object and sibling index of the
		 * row under the pointer (empty area appends to the roots). */
		bool row_drop_location(float x, float y, object_id* out_parent,
			std::size_t* out_index) const;

	private:
		class move_command;

		struct node_location
		{
			object_id parent = invalid_object;
			std::size_t index = 0;
			bool valid = false;
		};

		object_id object_for_item(vgui_item_id item) const;
		vgui_item_id item_for_object(object_id object) const;
		node_location location_of(vgui_item_id item) const;
		bool apply_move(object_id node, object_id parent, std::size_t index);

		static void on_select(vgui_widget* widget, vgui_item_id item, int selected,
			void* user);

		editor_context* context_ = nullptr;
		vgui_widget* tree_ = nullptr;
		vgui_item_id selected_ = 0;
		selection_listener listener_;
		command_stack* command_stack_ = nullptr;
		move_handler move_handler_;
		std::shared_ptr<int> alive_token_ = std::make_shared<int>(0);
		std::unique_ptr<drop_target> drop_target_;	// F03 drop target（树重排）
	};

} /* namespace vgui_editor_kit */
