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
#include <vgui/vgui.h>

namespace vgui_editor_kit {

	class editor_context
	{
	public:
		using selection_listener = std::function<void(object_id)>;
		using selection_listener_id = std::size_t;
		/* V03: full ordered selection set, including the primary id. */
		using selection_set_listener = std::function<void(const std::vector<object_id>&)>;
		using selection_set_listener_id = std::size_t;

		explicit editor_context(vgui_context* ui);

		editor_context(const editor_context&) = delete;
		editor_context& operator=(const editor_context&) = delete;

		vgui_context* ui() const noexcept;
		vgui_widget* root() const noexcept;

		command_stack& commands() noexcept;
		const command_stack& commands() const noexcept;

		object_id selection() const noexcept;
		/* Ordered selection set; the primary id is selection(). */
		const std::vector<object_id>& selection_ids() const noexcept;
		std::size_t selection_count() const noexcept;
		bool is_selected(object_id id) const noexcept;

		void set_selection(object_id id);
		/* Replaces the set.  primary defaults to the first id when omitted or
		 * when it is not part of ids.  Duplicates and invalid ids are removed. */
		void set_selection(const std::vector<object_id>& ids,
			object_id primary = invalid_object);
		void add_to_selection(object_id id);
		void remove_from_selection(object_id id);
		void toggle_selection(object_id id);
		void clear_selection();

		/* Returns a non-zero token.  Pass it to remove_selection_listener()
		 * when the listening component is destroyed or rebound. */
		selection_listener_id add_selection_listener(selection_listener listener);
		bool remove_selection_listener(selection_listener_id id);

		/* Set observers run after the single-id observer, in registration
		 * order, and only when the set itself changed. */
		selection_set_listener_id add_selection_set_listener(
			selection_set_listener listener);
		bool remove_selection_set_listener(selection_set_listener_id id);

	private:
		struct listener_entry
		{
			selection_listener_id id = 0;
			selection_listener fn;
		};

		struct set_listener_entry
		{
			selection_set_listener_id id = 0;
			selection_set_listener fn;
		};

		void notify_selection(bool primary_changed, bool set_changed);

		vgui_context* ui_ = nullptr;
		command_stack commands_;
		object_id selection_ = invalid_object;
		std::vector<object_id> selection_set_;
		std::vector<listener_entry> selection_listeners_;
		std::vector<set_listener_entry> selection_set_listeners_;
		selection_listener_id next_selection_listener_ = 1;
		selection_set_listener_id next_selection_set_listener_ = 1;
	};

} /* namespace vgui_editor_kit */
