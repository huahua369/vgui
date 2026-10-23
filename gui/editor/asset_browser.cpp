#include "asset_browser.h"

#include <utility>

namespace vgui_editor_kit {

	void asset_browser::build(vgui_widget* parent)
	{
		if (list_ || !parent)
			return;

		list_ = vgui_widget_add(parent, VGUI_LIST, nullptr);
		vgui_widget_set_grow(list_, 1.0f);
		vgui_widget_set_min_size(list_, 160, 80);
		vgui_list_set_selection_mode(list_, VGUI_SELECTION_SINGLE);
		vgui_list_on_select(list_, &asset_browser::on_select, this);
	}

	void asset_browser::clear()
	{
		if (list_)
			vgui_list_clear(list_);
		selected_ = 0;
	}

	vgui_item_id asset_browser::add_item(std::string text, std::string detail)
	{
		if (!list_)
			return 0;

		vgui_item_id item = vgui_list_add_item(list_, text.c_str(), nullptr);
		if (item && !detail.empty())
			vgui_list_set_item_detail(list_, item, detail.c_str());
		return item;
	}

	vgui_item_id asset_browser::selected() const noexcept
	{
		return selected_;
	}

	vgui_widget* asset_browser::widget() const noexcept
	{
		return list_;
	}

	void asset_browser::set_selection_listener(selection_listener listener)
	{
		listener_ = std::move(listener);
	}

	void asset_browser::on_select(vgui_widget* widget, vgui_item_id item,
		int selected, void* user)
	{
		(void)widget;
		if (!selected || !user)
			return;

		auto* self = static_cast<asset_browser*>(user);
		self->selected_ = item;
		if (self->listener_)
			self->listener_(item);
	}

} /* namespace vgui_editor_kit */
