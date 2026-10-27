#include "breadcrumb.h"

#include <utility>

namespace vgui_editor_kit {

	namespace {
		constexpr vgui_color kFallbackText = 0xFFB8C4D6;
		constexpr vgui_color kFallbackPanel = 0xFF1E232B;
		constexpr vgui_color kFallbackMuted = 0xFF7F8B9C;

		vgui_theme theme_for(vgui_widget* widget)
		{
			vgui_theme theme = {};
			vgui_context* ctx = widget ? vgui_widget_get_context(widget) : nullptr;
			if (ctx)
				vgui_context_get_theme(ctx, &theme);
			return theme;
		}

		vgui_color mix_color(vgui_color a, vgui_color b, float t)
		{
			if (t < 0.0f) t = 0.0f;
			if (t > 1.0f) t = 1.0f;
			auto mix = [t](unsigned ca, unsigned cb) {
				return (unsigned)(ca + (cb - ca) * t + 0.5f);
			};
			return (mix((a >> 24) & 0xFF, (b >> 24) & 0xFF) << 24)
				| (mix((a >> 16) & 0xFF, (b >> 16) & 0xFF) << 16)
				| (mix((a >> 8) & 0xFF, (b >> 8) & 0xFF) << 8)
				| mix(a & 0xFF, b & 0xFF);
		}

		vgui_color muted_color(const vgui_theme& theme)
		{
			const vgui_color text = theme.text ? theme.text : kFallbackText;
			const vgui_color panel = theme.panel_bg ? theme.panel_bg : kFallbackPanel;
			return theme.text ? mix_color(text, panel, 0.45f) : kFallbackMuted;
		}
	}

	void breadcrumb::build(vgui_widget* parent)
	{
		if (panel_ || !parent)
			return;
		panel_ = vgui_widget_add(parent, VGUI_PANEL, nullptr);
		vgui_widget_set_a11y_name(panel_, "Breadcrumb");
		vgui_widget_set_id(panel_, "breadcrumb");
		vgui_widget_set_size(panel_, 0, 26);
		vgui_widget_set_min_size(panel_, 0, 22);
		vgui_widget_set_align_self(panel_, VGUI_ALIGN_STRETCH);
		vgui_widget_set_layout(panel_, VGUI_LAYOUT_ROW);
		vgui_widget_set_padding(panel_, 0, 0, 0, 0);
		vgui_widget_set_gap(panel_, 2);
		vgui_widget_set_align_items(panel_, VGUI_ALIGN_CENTER);
	}

	void breadcrumb::add_item(std::string label, std::string id)
	{
		if (!panel_)
			return;
		if (!items_.empty())
			add_separator();

		const std::size_t index = items_.size();
		auto button = std::make_unique<icon_button>();
		button->build(panel_, label);
		vgui_widget* widget = button->widget();
		vgui_widget_set_size(widget, 0, 24);
		vgui_widget_set_font_size(widget, 12);
		button->set_on_click([this, index]() { (void)navigate(index); });

		items_.push_back(std::move(button));
		path_.push_back({ std::move(label), std::move(id) });
		active_ = path_.size() - 1;
		has_active_ = true;
		update_active();
	}

	void breadcrumb::set_items(std::vector<breadcrumb_item> items)
	{
		clear();
		for (breadcrumb_item& item : items)
			add_item(std::move(item.label), std::move(item.id));
	}

	void breadcrumb::clear()
	{
		if (panel_)
			vgui_widget_clear(panel_);
		items_.clear();
		separators_.clear();
		path_.clear();
		active_ = 0;
		has_active_ = false;
	}

	std::size_t breadcrumb::count() const noexcept
	{
		return path_.size();
	}

	const breadcrumb_item& breadcrumb::item(std::size_t index) const
	{
		static const breadcrumb_item empty;
		return index < path_.size() ? path_[index] : empty;
	}

	bool breadcrumb::navigate(std::size_t index)
	{
		if (index >= path_.size())
			return false;
		if (listener_)
			listener_(index, path_[index].id);
		return true;
	}

	void breadcrumb::set_active(std::size_t index)
	{
		if (index >= path_.size())
			return;
		active_ = index;
		has_active_ = true;
		update_active();
	}

	std::size_t breadcrumb::active() const noexcept
	{
		return has_active_ ? active_ : 0;
	}

	void breadcrumb::set_on_navigate(navigate_listener listener)
	{
		listener_ = std::move(listener);
	}

	vgui_widget* breadcrumb::item_widget(std::size_t index) const noexcept
	{
		return index < items_.size() ? items_[index]->widget() : nullptr;
	}

	vgui_widget* breadcrumb::widget() const noexcept
	{
		return panel_;
	}

	void breadcrumb::add_separator()
	{
		if (!panel_)
			return;
		vgui_widget* separator = vgui_widget_add(panel_, VGUI_LABEL, "/");
		vgui_widget_set_size(separator, 0, 24);
		vgui_widget_set_font_size(separator, 12);
		vgui_widget_set_text_color(separator, muted_color(theme_for(panel_)));
		vgui_widget_set_text_align(separator, 0.5f, 0.5f);
		separators_.push_back(separator);
	}

	void breadcrumb::update_active()
	{
		for (std::size_t i = 0; i < items_.size(); ++i)
			items_[i]->set_checked(has_active_ && i == active_);
	}

} /* namespace vgui_editor_kit */
