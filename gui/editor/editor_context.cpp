#include "editor_context.h"

#include <algorithm>
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

	const std::vector<object_id>& editor_context::selection_ids() const noexcept
	{
		return selection_set_;
	}

	std::size_t editor_context::selection_count() const noexcept
	{
		return selection_set_.size();
	}

	bool editor_context::is_selected(object_id id) const noexcept
	{
		return id != invalid_object &&
			std::find(selection_set_.begin(), selection_set_.end(), id) !=
				selection_set_.end();
	}

	void editor_context::set_selection(object_id id)
	{
		std::vector<object_id> ids;
		if (id != invalid_object)
			ids.push_back(id);
		set_selection(ids, id);
	}

	void editor_context::set_selection(const std::vector<object_id>& ids,
		object_id primary)
	{
		std::vector<object_id> next;
		next.reserve(ids.size());
		for (object_id id : ids)
		{
			if (id == invalid_object)
				continue;
			if (std::find(next.begin(), next.end(), id) != next.end())
				continue;
			next.push_back(id);
		}

		object_id next_primary = primary;
		if (next_primary == invalid_object ||
			std::find(next.begin(), next.end(), next_primary) == next.end())
			next_primary = next.empty() ? invalid_object : next.front();

		const bool set_changed = next != selection_set_;
		const bool primary_changed = next_primary != selection_;
		if (!set_changed && !primary_changed)
			return;

		selection_set_ = std::move(next);
		selection_ = next_primary;
		notify_selection(primary_changed, set_changed);
	}

	void editor_context::add_to_selection(object_id id)
	{
		if (id == invalid_object || is_selected(id))
			return;
		const bool primary_changed = selection_ == invalid_object;
		selection_set_.push_back(id);
		if (primary_changed)
			selection_ = id;
		notify_selection(primary_changed, true);
	}

	void editor_context::remove_from_selection(object_id id)
	{
		auto it = std::find(selection_set_.begin(), selection_set_.end(), id);
		if (it == selection_set_.end())
			return;
		selection_set_.erase(it);
		const bool primary_changed = selection_ == id;
		if (primary_changed)
			selection_ = selection_set_.empty()
				? invalid_object : selection_set_.front();
		notify_selection(primary_changed, true);
	}

	void editor_context::toggle_selection(object_id id)
	{
		if (is_selected(id))
			remove_from_selection(id);
		else
			add_to_selection(id);
	}

	void editor_context::clear_selection()
	{
		if (selection_set_.empty() && selection_ == invalid_object)
			return;
		const bool primary_changed = selection_ != invalid_object;
		const bool set_changed = !selection_set_.empty();
		selection_set_.clear();
		selection_ = invalid_object;
		notify_selection(primary_changed, set_changed);
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

	editor_context::selection_set_listener_id editor_context::add_selection_set_listener(
		selection_set_listener listener)
	{
		if (!listener)
			return 0;
		const selection_set_listener_id id = next_selection_set_listener_++;
		selection_set_listeners_.push_back({ id, std::move(listener) });
		return id;
	}

	bool editor_context::remove_selection_set_listener(selection_set_listener_id id)
	{
		if (!id)
			return false;
		for (auto it = selection_set_listeners_.begin();
			it != selection_set_listeners_.end(); ++it)
		{
			if (it->id != id)
				continue;
			selection_set_listeners_.erase(it);
			return true;
		}
		return false;
	}

	void editor_context::notify_selection(bool primary_changed, bool set_changed)
	{
		/* Copy the callbacks so a listener may add or remove listeners while
		 * being notified without invalidating this iteration. */
		if (primary_changed)
		{
			std::vector<selection_listener> listeners;
			listeners.reserve(selection_listeners_.size());
			for (const listener_entry& entry : selection_listeners_)
				listeners.push_back(entry.fn);
			for (const auto& listener : listeners)
				listener(selection_);
		}
		if (set_changed)
		{
			std::vector<selection_set_listener> listeners;
			listeners.reserve(selection_set_listeners_.size());
			for (const set_listener_entry& entry : selection_set_listeners_)
				listeners.push_back(entry.fn);
			/* Snapshot the ids so a listener that mutates the selection cannot
			 * invalidate the argument seen by later listeners. */
			const std::vector<object_id> ids = selection_set_;
			for (const auto& listener : listeners)
				listener(ids);
		}
	}

} /* namespace vgui_editor_kit */
