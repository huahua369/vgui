#pragma once
/*
 * editor_context.h - shared state for one editor instance.
 *
 * The context does not own the vgui_context.  It connects editor panels to the
 * command stack and the current object selection without depending on a scene
 * or asset database implementation.
 */
#include <cstddef>
#include <functional>
#include <vector>

#include "command_stack.h"
#include "editor_types.h"
#include "vgui.h"

namespace vgui_editor_kit {

	class editor_context
	{
	public:
		using selection_listener = std::function<void(object_id)>;
		using selection_listener_id = std::size_t;

		explicit editor_context(vgui_context* ui);

		editor_context(const editor_context&) = delete;
		editor_context& operator=(const editor_context&) = delete;

		vgui_context* ui() const noexcept;
		vgui_widget* root() const noexcept;

		command_stack& commands() noexcept;
		const command_stack& commands() const noexcept;

		object_id selection() const noexcept;
		void set_selection(object_id id);
		void clear_selection();

		/* Returns a non-zero token.  Pass it to remove_selection_listener()
		 * when the listening component is destroyed or rebound. */
		selection_listener_id add_selection_listener(selection_listener listener);
		bool remove_selection_listener(selection_listener_id id);

	private:
		struct listener_entry
		{
			selection_listener_id id = 0;
			selection_listener fn;
		};

		void notify_selection();

		vgui_context* ui_ = nullptr;
		command_stack commands_;
		object_id selection_ = invalid_object;
		std::vector<listener_entry> selection_listeners_;
		selection_listener_id next_selection_listener_ = 1;
	};

} /* namespace vgui_editor_kit */
