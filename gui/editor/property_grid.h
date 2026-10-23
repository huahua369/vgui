#pragma once
/*
 * property_grid.h - first editor-kit inspector skeleton.
 *
 * The public surface intentionally stays small for the MVP: rows are plain
 * label/editor pairs until the typed property editors (number, combo, vector,
 * color, asset reference) land.
 */
#include <cstddef>
#include <string>
#include <vector>

#include "vgui.h"

namespace vgui_editor_kit {

	class property_grid
	{
	public:
		void build(vgui_widget* parent);

		void clear();
		void add_row(std::string label, std::string value);
		bool set_row_value(std::size_t index, std::string value);

		std::string row_value(std::size_t index) const;
		std::size_t row_count() const noexcept;

		vgui_widget* widget() const noexcept;

	private:
		struct row
		{
			vgui_widget* panel = nullptr;
			vgui_widget* label = nullptr;
			vgui_widget* editor = nullptr;
			std::string value;
		};

		vgui_widget* panel_ = nullptr;
		std::vector<row> rows_;
	};

} /* namespace vgui_editor_kit */
