#include "editor_context.h"

#include <utility>

namespace vgui_editor_kit {

	editor_context::editor_context(vgui_context* ui)
		: ui_(ui)
	{
	}

	vgui_context* editor_context::ui() const noexcept
	{
		return ui_;
	}

	vgui_widget* editor_context::root() const noexcept
	{
		return ui_ ? vgui_root(ui_) : nullptr;
	}

	command_stack& editor_context::commands() noexcept
	{
		return commands_;
	}

	const command_stack& editor_context::commands() const noexcept
	{
		return commands_;
	}

	object_id editor_context::selection() const noexcept
	{
		return selection_;
	}

	void editor_context::set_selection(object_id id)
	{
		if (selection_ == id)
			return;

		selection_ = id;
		notify_selection();
	}

	void editor_context::clear_selection()
	{
		set_selection(invalid_object);
	}

	editor_context::selection_listener_id editor_context::add_selection_listener(
		selection_listener listener)
	{
		if (!listener)
			return 0;
		const selection_listener_id id = next_selection_listener_++;
		selection_listeners_.push_back({ id, std::move(listener) });
		return id;
	}

	bool editor_context::remove_selection_listener(selection_listener_id id)
	{
		if (!id)
			return false;
		for (auto it = selection_listeners_.begin(); it != selection_listeners_.end(); ++it)
		{
			if (it->id != id)
				continue;
			selection_listeners_.erase(it);
			return true;
		}
		return false;
	}

	void editor_context::notify_selection()
	{
		/* Copy the callbacks so a listener may add or remove listeners while
		 * being notified without invalidating this iteration. */
		std::vector<selection_listener> listeners;
		listeners.reserve(selection_listeners_.size());
		for (const listener_entry& entry : selection_listeners_)
			listeners.push_back(entry.fn);
		for (const auto& listener : listeners)
			listener(selection_);
	}

} /* namespace vgui_editor_kit */
