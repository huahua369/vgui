#pragma once
/*
 * editor_context.h - shared state for one editor instance.
 *
 * The context does not own the vgui_context.  It connects editor panels to the
 * command stack and the current object selection without depending on a scene
 * or asset database implementation.
 */
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

		void add_selection_listener(selection_listener listener);

	private:
		void notify_selection();

		vgui_context* ui_ = nullptr;
		command_stack commands_;
		object_id selection_ = invalid_object;
		std::vector<selection_listener> selection_listeners_;
	};

} /* namespace vgui_editor_kit */
