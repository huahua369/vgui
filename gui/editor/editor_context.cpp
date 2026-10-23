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

	void editor_context::add_selection_listener(selection_listener listener)
	{
		if (listener)
			selection_listeners_.push_back(std::move(listener));
	}

	void editor_context::notify_selection()
	{
		for (const auto& listener : selection_listeners_)
			listener(selection_);
	}

} /* namespace vgui_editor_kit */
