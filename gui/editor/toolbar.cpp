#include "toolbar.h"

#include "command_registry.h"

#include <utility>

namespace vgui_editor_kit {

	namespace {
		constexpr vgui_color kFallbackPanel = 0xFF1E232B;
		constexpr vgui_color kFallbackBorder = 0xFF2E3440;
		constexpr vgui_color kFallbackText = 0xFFD8DEE9;

		vgui_theme theme_for(vgui_widget* widget)
		{
			vgui_theme theme = {};
			vgui_context* ctx = widget ? vgui_widget_get_context(widget) : nullptr;
			if (ctx)
				vgui_context_get_theme(ctx, &theme);
			return theme;
		}

		vgui_color panel_color(const vgui_theme& theme)
		{
			return theme.panel_bg ? theme.panel_bg : kFallbackPanel;
		}

		vgui_color border_color(const vgui_theme& theme)
		{
			return theme.panel_border ? theme.panel_border : kFallbackBorder;
		}

		vgui_color text_color(const vgui_theme& theme)
		{
			return theme.text ? theme.text : kFallbackText;
		}
	}

	void icon_button::build(vgui_widget* parent, std::string text)
	{
		if (button_ || !parent)
			return;
		button_ = vgui_widget_add(parent, VGUI_BUTTON, text.c_str());
		vgui_widget_set_size(button_, 0, 24);
		vgui_widget_set_min_size(button_, 24, 24);
		vgui_widget_set_font_size(button_, 12);
		vgui_widget_on_event(button_, &icon_button::on_event, this);
	}

	void icon_button::set_icon(vgui_texture* texture, int width, int height)
	{
		if (button_)
			vgui_button_set_icon(button_, texture, width, height,
				VGUI_BUTTON_ICON_LEFT);
	}

	void icon_button::set_text(std::string text)
	{
		if (button_)
			vgui_widget_set_text(button_, text.c_str());
	}

	void icon_button::set_tooltip(std::string text)
	{
		if (button_)
			vgui_widget_set_tooltip(button_, text.c_str());
	}

	void icon_button::set_enabled(bool enabled)
	{
		if (button_)
			vgui_widget_set_enabled(button_, enabled ? 1 : 0);
	}

	bool icon_button::enabled() const
	{
		return button_ && vgui_widget_get_enabled(button_) != 0;
	}

	void icon_button::set_checked(bool checked)
	{
		if (button_)
			vgui_widget_set_checked(button_, checked ? 1 : 0);
	}

	bool icon_button::checked() const
	{
		return button_ && vgui_widget_get_checked(button_) != 0;
	}

	void icon_button::set_on_click(click_listener listener)
	{
		listener_ = std::move(listener);
	}

	void icon_button::click()
	{
		if (button_ && enabled() && listener_)
			listener_();
	}

	vgui_widget* icon_button::widget() const noexcept
	{
		return button_;
	}

	void icon_button::on_event(vgui_widget* widget, const vgui_event* event,
		void* user)
	{
		(void)widget;
		if (!event || event->type != VGUI_EVENT_CLICK || !user)
			return;
		static_cast<icon_button*>(user)->click();
	}

	void toggle_button::build(vgui_widget* parent, std::string text)
	{
		button_.build(parent, std::move(text));
		button_.set_on_click([this]() { toggle(); });
	}

	void toggle_button::set_icon(vgui_texture* texture, int width, int height)
	{
		button_.set_icon(texture, width, height);
	}

	void toggle_button::set_text(std::string text)
	{
		button_.set_text(std::move(text));
	}

	void toggle_button::set_tooltip(std::string text)
	{
		button_.set_tooltip(std::move(text));
	}

	void toggle_button::set_enabled(bool enabled)
	{
		button_.set_enabled(enabled);
	}

	bool toggle_button::enabled() const
	{
		return button_.enabled();
	}

	void toggle_button::set_checked(bool checked)
	{
		checked_ = checked;
		button_.set_checked(checked);
	}

	bool toggle_button::checked() const
	{
		return checked_;
	}

	void toggle_button::toggle()
	{
		if (!button_.enabled())
			return;
		set_checked(!checked_);
		if (listener_)
			listener_(checked_);
	}

	void toggle_button::set_on_change(change_listener listener)
	{
		listener_ = std::move(listener);
	}

	vgui_widget* toggle_button::widget() const noexcept
	{
		return button_.widget();
	}

	void segmented_control::build(vgui_widget* parent)
	{
		if (panel_ || !parent)
			return;
		panel_ = vgui_widget_add(parent, VGUI_PANEL, nullptr);
		vgui_widget_set_size(panel_, 0, 24);
		vgui_widget_set_min_size(panel_, 0, 24);
		vgui_widget_set_layout(panel_, VGUI_LAYOUT_ROW);
		vgui_widget_set_padding(panel_, 0, 0, 0, 0);
		vgui_widget_set_gap(panel_, 1);
		vgui_widget_set_align_items(panel_, VGUI_ALIGN_CENTER);
	}

	std::size_t segmented_control::add_segment(std::string text)
	{
		if (!panel_)
			return (std::size_t)-1;
		const std::size_t index = segments_.size();
		auto segment = std::make_unique<icon_button>();
		segment->build(panel_, std::move(text));
		segment->set_on_click([this, index]() { (void)select(index); });
		segments_.push_back(std::move(segment));
		if (!has_selection_)
			apply_selection(0);
		return index;
	}

	void segmented_control::clear()
	{
		if (panel_)
			vgui_widget_clear(panel_);
		segments_.clear();
		selected_ = 0;
		has_selection_ = false;
	}

	std::size_t segmented_control::count() const noexcept
	{
		return segments_.size();
	}

	bool segmented_control::select(std::size_t index)
	{
		if (index >= segments_.size() || !segments_[index]->enabled())
			return false;
		if (has_selection_ && selected_ == index)
			return true;
		apply_selection(index);
		if (listener_)
			listener_(index);
		return true;
	}

	void segmented_control::set_selected(std::size_t index)
	{
		if (index < segments_.size())
			apply_selection(index);
	}

	std::size_t segmented_control::selected() const noexcept
	{
		return selected_;
	}

	bool segmented_control::has_selection() const noexcept
	{
		return has_selection_;
	}

	void segmented_control::set_segment_enabled(std::size_t index, bool enabled)
	{
		if (index < segments_.size())
			segments_[index]->set_enabled(enabled);
	}

	bool segmented_control::segment_enabled(std::size_t index) const
	{
		return index < segments_.size() && segments_[index]->enabled();
	}

	void segmented_control::set_on_change(change_listener listener)
	{
		listener_ = std::move(listener);
	}

	vgui_widget* segmented_control::segment_widget(std::size_t index) const noexcept
	{
		return index < segments_.size() ? segments_[index]->widget() : nullptr;
	}

	vgui_widget* segmented_control::widget() const noexcept
	{
		return panel_;
	}

	void segmented_control::apply_selection(std::size_t index)
	{
		if (index >= segments_.size())
			return;
		selected_ = index;
		has_selection_ = true;
		for (std::size_t i = 0; i < segments_.size(); ++i)
			segments_[i]->set_checked(i == index);
	}

	void toolbar_view::build(vgui_widget* parent)
	{
		if (panel_ || !parent)
			return;
		panel_ = vgui_widget_add(parent, VGUI_PANEL, nullptr);
		vgui_widget_set_size(panel_, 0, 34);
		vgui_widget_set_min_size(panel_, 0, 28);
		vgui_widget_set_align_self(panel_, VGUI_ALIGN_STRETCH);
		vgui_widget_set_layout(panel_, VGUI_LAYOUT_ROW);
		vgui_widget_set_padding(panel_, 8, 0, 8, 0);
		vgui_widget_set_gap(panel_, 6);
		vgui_widget_set_align_items(panel_, VGUI_ALIGN_CENTER);
		const vgui_theme theme = theme_for(panel_);
		vgui_widget_set_background(panel_, panel_color(theme));
		vgui_widget_set_border(panel_, border_color(theme), 1);
	}

	icon_button& toolbar_view::add_icon_button(std::string text,
		vgui_texture* icon, int icon_width, int icon_height)
	{
		auto button = std::make_unique<icon_button>();
		button->build(panel_, std::move(text));
		button->set_icon(icon, icon_width, icon_height);
		icon_button& result = *button;
		icon_buttons_.push_back(std::move(button));
		return result;
	}

	toggle_button& toolbar_view::add_toggle_button(std::string text, bool checked)
	{
		auto button = std::make_unique<toggle_button>();
		button->build(panel_, std::move(text));
		button->set_checked(checked);
		toggle_button& result = *button;
		toggle_buttons_.push_back(std::move(button));
		return result;
	}

	segmented_control& toolbar_view::add_segmented_control()
	{
		auto control = std::make_unique<segmented_control>();
		control->build(panel_);
		segmented_control& result = *control;
		segmented_controls_.push_back(std::move(control));
		return result;
	}

	icon_button* toolbar_view::add_command_button(command_registry& commands,
		const char* id, vgui_texture* icon, int icon_width, int icon_height)
	{
		if (!panel_ || !id || !*id)
			return nullptr;
		const command_entry* entry = commands.find(id);
		if (!entry)
			return nullptr;

		auto button = std::make_unique<icon_button>();
		button->build(panel_, entry->title);
		button->set_icon(icon, icon_width, icon_height);
		std::string tooltip = entry->title;
		const std::string shortcut = entry->shortcut.to_string();
		if (!shortcut.empty())
			tooltip += " (" + shortcut + ")";
		button->set_tooltip(tooltip);
		button->set_on_click([this, &commands, command = std::string(id)]() {
			(void)commands.invoke(command.c_str());
			refresh_commands();
		});

		icon_button* result = button.get();
		icon_buttons_.push_back(std::move(button));
		command_bindings_.push_back({ &commands, result, id });
		refresh_commands();
		return result;
	}

	vgui_widget* toolbar_view::add_label(std::string text, bool grow)
	{
		if (!panel_)
			return nullptr;
		vgui_widget* label = vgui_widget_add(panel_, VGUI_LABEL, text.c_str());
		vgui_widget_set_size(label, 0, 24);
		vgui_widget_set_font_size(label, 12);
		vgui_widget_set_text_color(label, text_color(theme_for(panel_)));
		vgui_widget_set_text_align(label, 0.0f, 0.5f);
		if (grow)
			vgui_widget_set_grow(label, 1.0f);
		return label;
	}

	vgui_widget* toolbar_view::add_spacer(float grow)
	{
		if (!panel_)
			return nullptr;
		vgui_widget* spacer = vgui_widget_add(panel_, VGUI_PANEL, nullptr);
		vgui_widget_set_size(spacer, 0, 1);
		vgui_widget_set_grow(spacer, grow > 0.0f ? grow : 1.0f);
		return spacer;
	}

	vgui_widget* toolbar_view::add_separator()
	{
		if (!panel_)
			return nullptr;
		vgui_widget* separator = vgui_widget_add(panel_, VGUI_PANEL, nullptr);
		vgui_widget_set_size(separator, 1, 20);
		vgui_widget_set_margin(separator, 2, 0, 2, 0);
		vgui_widget_set_background(separator,
			border_color(theme_for(panel_)));
		return separator;
	}

	void toolbar_view::refresh_commands()
	{
		for (const command_binding& binding : command_bindings_)
		{
			if (!binding.commands || !binding.button || binding.id.empty())
				continue;
			const command_entry* entry = binding.commands->find(binding.id.c_str());
			if (!entry)
			{
				binding.button->set_enabled(false);
				binding.button->set_checked(false);
				continue;
			}
			binding.button->set_text(entry->title);
			std::string tooltip = entry->title;
			const std::string shortcut = entry->shortcut.to_string();
			if (!shortcut.empty())
				tooltip += " (" + shortcut + ")";
			binding.button->set_tooltip(tooltip);
			binding.button->set_enabled(
				binding.commands->is_enabled(binding.id.c_str()));
			binding.button->set_checked(entry->checked &&
				binding.commands->is_checked(binding.id.c_str()));
		}
	}

	vgui_widget* toolbar_view::widget() const noexcept
	{
		return panel_;
	}

} /* namespace vgui_editor_kit */
