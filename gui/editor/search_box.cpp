#include <vgui/editor/search_box.h>

#include <utility>

namespace vgui_editor_kit {

	void search_box::build(vgui_widget* parent, std::string placeholder)
	{
		if (panel_ || !parent)
			return;

		panel_ = vgui_widget_add(parent, VGUI_PANEL, nullptr);
		vgui_widget_set_id(panel_, "search_box");
		vgui_widget_set_size(panel_, 220, 26);
		vgui_widget_set_min_size(panel_, 140, 24);
		vgui_widget_set_layout(panel_, VGUI_LAYOUT_ROW);
		vgui_widget_set_padding(panel_, 0, 0, 0, 0);
		vgui_widget_set_gap(panel_, 2);
		vgui_widget_set_align_items(panel_, VGUI_ALIGN_CENTER);

		edit_ = vgui_widget_add(panel_, VGUI_EDIT, "");
		vgui_widget_set_a11y_name(edit_,
			placeholder.empty() ? "Search" : placeholder.c_str());
		vgui_widget_set_a11y_role(edit_, VGUI_A11Y_ROLE_EDIT);
		vgui_widget_set_id(edit_, "search_box_edit");
		vgui_widget_set_grow(edit_, 1.0f);
		vgui_widget_set_size(edit_, 0, 24);
		vgui_widget_set_font_size(edit_, 12);
		vgui_edit_set_placeholder(edit_, placeholder.c_str());
		vgui_widget_on_text(edit_, &search_box::on_text, this);
		vgui_widget_on_event(edit_, &search_box::on_edit_event, this);

		clear_button_.build(panel_, "x");
		vgui_widget* clear_widget = clear_button_.widget();
		vgui_widget_set_id(clear_widget, "search_box_clear");
		vgui_widget_set_size(clear_widget, 24, 24);
		vgui_widget_set_font_size(clear_widget, 11);
		clear_button_.set_tooltip("Clear search");
		clear_button_.set_on_click([this]() { clear(); });

		update_clear_visible();
	}

	void search_box::set_placeholder(std::string text)
	{
		if (edit_)
			vgui_edit_set_placeholder(edit_, text.c_str());
	}

	void search_box::set_text(std::string text)
	{
		update_text(std::move(text), false);
	}

	const std::string& search_box::text() const noexcept
	{
		return text_;
	}

	void search_box::clear()
	{
		if (text_.empty())
			return;
		update_text({}, true);
	}

	void search_box::submit()
	{
		if (submit_listener_)
			submit_listener_(text_);
	}

	void search_box::focus()
	{
		if (!edit_)
			return;
		vgui_context* ctx = vgui_widget_get_context(edit_);
		if (ctx)
			vgui_set_focus(ctx, edit_);
	}

	void search_box::set_on_change(change_listener listener)
	{
		change_listener_ = std::move(listener);
	}

	void search_box::set_on_submit(submit_listener listener)
	{
		submit_listener_ = std::move(listener);
	}

	vgui_widget* search_box::widget() const noexcept
	{
		return panel_;
	}

	vgui_widget* search_box::edit_widget() const noexcept
	{
		return edit_;
	}

	vgui_widget* search_box::clear_widget() const noexcept
	{
		return clear_button_.widget();
	}

	void search_box::on_text(vgui_widget* widget, const char* text, void* user)
	{
		(void)widget;
		auto* self = static_cast<search_box*>(user);
		if (self)
			self->update_text(text ? text : "", true);
	}

	void search_box::on_edit_event(vgui_widget* widget, const vgui_event* event,
		void* user)
	{
		auto* self = static_cast<search_box*>(user);
		if (!self || !widget || !event)
			return;

		if (event->type == VGUI_EVENT_SUBMIT)
		{
			const char* text = vgui_edit_get_text(widget);
			self->update_text(text ? text : "", true);
			self->submit();
		}
		else if (event->type == VGUI_EVENT_KEY_DOWN &&
			event->key == VGUI_KEY_ESCAPE)
		{
			self->clear();
		}
	}

	void search_box::update_clear_visible()
	{
		if (vgui_widget* clear = clear_button_.widget())
			vgui_widget_set_visible(clear, text_.empty() ? 0 : 1);
	}

	void search_box::update_text(std::string text, bool notify_change)
	{
		const bool changed = text != text_;
		text_ = std::move(text);
		if (edit_)
		{
			const char* current = vgui_edit_get_text(edit_);
			/* Text callbacks already contain the edit's new text.  Avoid
			 * re-setting it (which would move the caret and reset undo). */
			if (!current || text_ != current)
				vgui_edit_set_text(edit_, text_.c_str());
		}
		update_clear_visible();
		if (changed && notify_change && change_listener_)
			change_listener_(text_);
	}

} /* namespace vgui_editor_kit */
