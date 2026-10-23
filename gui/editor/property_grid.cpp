#include "property_grid.h"

#include <utility>

namespace vgui_editor_kit {

	namespace {
		constexpr vgui_color kPanelBg = 0xFF1B1E24;
		constexpr vgui_color kBorder = 0xFF2E3440;
		constexpr vgui_color kLabelText = 0xFF9FB0C4;
	}

	void property_grid::build(vgui_widget* parent)
	{
		if (panel_ || !parent)
			return;

		panel_ = vgui_widget_add(parent, VGUI_PANEL, nullptr);
		vgui_widget_set_grow(panel_, 1.0f);
		vgui_widget_set_layout(panel_, VGUI_LAYOUT_COLUMN);
		vgui_widget_set_padding(panel_, 6, 6, 6, 6);
		vgui_widget_set_gap(panel_, 4);
		vgui_widget_set_background(panel_, kPanelBg);
		vgui_widget_set_border(panel_, kBorder, 1);
		vgui_panel_set_scrollable(panel_, 0, 1);
	}

	void property_grid::clear()
	{
		if (panel_)
			vgui_widget_clear(panel_);
		rows_.clear();
	}

	void property_grid::add_row(std::string label, std::string value)
	{
		if (!panel_)
			return;

		vgui_widget* row_panel = vgui_widget_add(panel_, VGUI_PANEL, nullptr);
		vgui_widget_set_size(row_panel, 0, 28);
		vgui_widget_set_align_self(row_panel, VGUI_ALIGN_STRETCH);
		vgui_widget_set_layout(row_panel, VGUI_LAYOUT_ROW);
		vgui_widget_set_gap(row_panel, 6);

		vgui_widget* label_widget = vgui_widget_add(row_panel, VGUI_LABEL, label.c_str());
		vgui_widget_set_size(label_widget, 112, 24);
		vgui_widget_set_font_size(label_widget, 13);
		vgui_widget_set_text_color(label_widget, kLabelText);
		vgui_widget_set_text_align(label_widget, 0.0f, 0.5f);

		vgui_widget* editor = vgui_widget_add(row_panel, VGUI_EDIT, value.c_str());
		vgui_widget_set_grow(editor, 1.0f);
		vgui_widget_set_size(editor, 0, 24);
		vgui_widget_set_font_size(editor, 13);

		rows_.push_back({ row_panel, label_widget, editor, std::move(value) });
	}

	bool property_grid::set_row_value(std::size_t index, std::string value)
	{
		if (index >= rows_.size())
			return false;

		row& item = rows_[index];
		item.value = std::move(value);
		vgui_widget_set_text(item.editor, item.value.c_str());
		return true;
	}

	std::string property_grid::row_value(std::size_t index) const
	{
		return index < rows_.size() ? rows_[index].value : std::string();
	}

	std::size_t property_grid::row_count() const noexcept
	{
		return rows_.size();
	}

	vgui_widget* property_grid::widget() const noexcept
	{
		return panel_;
	}

} /* namespace vgui_editor_kit */
