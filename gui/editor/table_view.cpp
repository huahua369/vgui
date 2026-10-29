#include <vgui/editor/table_view.h>

#include <vgui/editor/curve_editor.h>
#include <vgui/editor/gradient_editor.h>
#include <vgui/editor/spinbox.h>
#include "ui_scale.h"
#include <vgui/editor/vector_editor.h>

#include "tools/tools.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <utility>

#include <nlohmann/json.hpp>

namespace vgui_editor_kit {

	namespace {
		constexpr float kBorderInset = 1.0f;
		constexpr float kHeaderHeight = 26.0f;
		constexpr float kCellPaddingX = 7.0f;
		constexpr float kResizeGrab = 4.0f;
		constexpr float kScrollbarThickness = 10.0f;
		constexpr float kScrollbarMinThumb = 24.0f;
		constexpr float kScrollbarInset = 2.0f;
		constexpr std::size_t kNoRow = (std::size_t)-1;

		uint32_t to_ovg(vgui_color color)
		{
			return (color & 0xFF00FF00u) |
				((color & 0x000000FFu) << 16) |
				((color >> 16) & 0x000000FFu);
		}

		vgui_color with_alpha(vgui_color color, uint32_t alpha)
		{
			return (color & 0x00FFFFFFu) | ((alpha & 0xFFu) << 24);
		}

		std::string lower_copy(std::string text)
		{
			for (char& c : text)
				c = (char)std::tolower((unsigned char)c);
			return text;
		}

		std::string trim_copy(const std::string& text)
		{
			std::size_t begin = 0;
			while (begin < text.size() &&
				std::isspace((unsigned char)text[begin]))
				++begin;
			std::size_t end = text.size();
			while (end > begin &&
				std::isspace((unsigned char)text[end - 1]))
				--end;
			return text.substr(begin, end - begin);
		}

		bool parse_double_text(const std::string& text, double& out)
		{
			const std::string trimmed = trim_copy(text);
			if (trimmed.empty())
				return false;
			char* end = nullptr;
			const double value = std::strtod(trimmed.c_str(), &end);
			if (end == trimmed.c_str() || *end != '\0' || !std::isfinite(value))
				return false;
			out = value;
			return true;
		}

		bool parse_bool_text(const std::string& text, bool& out)
		{
			const std::string lowered = lower_copy(trim_copy(text));
			if (lowered == "true" || lowered == "1" || lowered == "yes" ||
				lowered == "on")
			{
				out = true;
				return true;
			}
			if (lowered == "false" || lowered == "0" || lowered == "no" ||
				lowered == "off")
			{
				out = false;
				return true;
			}
			return false;
		}

		int hex_digit(char c)
		{
			if (c >= '0' && c <= '9')
				return c - '0';
			if (c >= 'a' && c <= 'f')
				return c - 'a' + 10;
			if (c >= 'A' && c <= 'F')
				return c - 'A' + 10;
			return -1;
		}

		bool parse_hex_color(const std::string& text, vgui_color& out)
		{
			std::string value = trim_copy(text);
			if (value.size() > 2 && value[0] == '0' &&
				(value[1] == 'x' || value[1] == 'X'))
				value.erase(0, 2);
			if (!value.empty() && value[0] == '#')
				value.erase(0, 1);
			if (value.size() == 3 || value.size() == 4)
			{
				std::string expanded;
				expanded.reserve(value.size() * 2);
				for (char c : value)
				{
					expanded.push_back(c);
					expanded.push_back(c);
				}
				value = std::move(expanded);
			}
			if (value.size() != 6 && value.size() != 8)
				return false;
			uint32_t color = 0;
			for (char c : value)
			{
				const int digit = hex_digit(c);
				if (digit < 0)
					return false;
				color = (color << 4) | (uint32_t)digit;
			}
			if (value.size() == 6)
				color |= 0xFF000000u;
			out = color;
			return true;
		}

		int property_dimension(property_type type)
		{
			switch (type)
			{
			case property_type::vector2: return 2;
			case property_type::vector3: return 3;
			case property_type::vector4: return 4;
			default: return 0;
			}
		}

		/* Scissor rectangles do not participate in save/restore, so custom
		 * table painting intersects the current clip and restores it. */
		class clip_guard
		{
		public:
			clip_guard(ovg_ctx_cb* cb, rvg_t* vg, float x, float y, float w,
				float h) : cb_(cb), vg_(vg)
			{
				if (!cb_ || !vg_ || !cb_->clip_rect)
					return;
				if (cb_->get_clip_rect)
				{
					cb_->get_clip_rect(vg_, saved_);
					has_saved_ = true;
				}
				int x0 = (int)std::floor(x);
				int y0 = (int)std::floor(y);
				int x1 = (int)std::ceil(x + w);
				int y1 = (int)std::ceil(y + h);
				if (has_saved_ && saved_[2] > 0 && saved_[3] > 0)
				{
					x0 = std::max(x0, saved_[0]);
					y0 = std::max(y0, saved_[1]);
					x1 = std::min(x1, saved_[0] + saved_[2]);
					y1 = std::min(y1, saved_[1] + saved_[3]);
				}
				cb_->clip_rect(vg_, x0, y0, std::max(0, x1 - x0),
					std::max(0, y1 - y0));
				active_ = true;
			}

			~clip_guard()
			{
				if (!active_ || !cb_ || !vg_)
					return;
				if (has_saved_ && cb_->set_clip_rect)
					cb_->set_clip_rect(vg_, saved_);
				else if (cb_->clip_rect)
					cb_->clip_rect(vg_, 0, 0, -1, -1);
			}

			clip_guard(const clip_guard&) = delete;
			clip_guard& operator=(const clip_guard&) = delete;

		private:
			ovg_ctx_cb* cb_ = nullptr;
			rvg_t* vg_ = nullptr;
			int saved_[4] = { 0, 0, 0, 0 };
			bool has_saved_ = false;
			bool active_ = false;
		};

		void draw_cell_text(vgui_widget* widget, ovg_ctx_cb* cb, rvg_t* vg,
			const std::string& text, float x, float y, float width,
			float height, vgui_color color, table_alignment alignment)
		{
			if (text.empty() || width <= 0.0f || height <= 0.0f)
				return;
			float align = 0.0f;
			if (alignment == table_alignment::center)
				align = 0.5f;
			else if (alignment == table_alignment::right)
				align = 1.0f;
			const float padding = kCellPaddingX * ui_scale_of(widget);
			vgui_custom_draw_text(widget, cb, vg, text.c_str(),
				x + padding, y, std::max(1.0f, width - padding * 2.0f),
				height, color, align, 0.5f);
		}
	}

	/* --------------------------------------------------------------------- */
	/* State serialization                                                   */
	/* --------------------------------------------------------------------- */

	std::string table_state_to_json(const table_state& state)
	{
		nlohmann::json root;
		root["version"] = table_state::current_version;
		root["column_widths"] = state.column_widths;
		root["sort"] = {
			{ "column", state.sort_column },
			{ "ascending", state.sort_ascending }
		};
		return root.dump();
	}

	bool table_state_from_json(const std::string& text, table_state& out,
		std::string* error)
	{
		out = table_state{};
		auto fail = [error](const std::string& message) {
			if (error)
				*error = message;
			return false;
		};
		try
		{
			nlohmann::json root = nlohmann::json::parse(text);
			if (!root.is_object())
				return fail("table state root must be an object");

			auto version = root.find("version");
			if (version == root.end() || !version->is_number_integer())
				return fail("table state version is missing");
			const int file_version = version->get<int>();
			if (file_version < 1 || file_version > table_state::current_version)
				return fail("unsupported table state version");

			table_state parsed;
			parsed.version = file_version;

			auto widths = root.find("column_widths");
			if (widths != root.end())
			{
				if (!widths->is_array())
					return fail("column_widths must be an array");
				for (const auto& entry : *widths)
				{
					if (!entry.is_number())
						return fail("column width must be a number");
					const float width = entry.get<float>();
					if (!std::isfinite(width) || width <= 0.0f)
						return fail("column width must be positive");
					parsed.column_widths.push_back(width);
				}
			}

			auto sort = root.find("sort");
			if (sort != root.end())
			{
				if (!sort->is_object())
					return fail("sort must be an object");
				auto column = sort->find("column");
				if (column != sort->end())
				{
					if (!column->is_number_integer())
						return fail("sort.column must be an integer");
					parsed.sort_column = column->get<int>();
					if (parsed.sort_column < -1)
						return fail("sort.column is invalid");
				}
				auto ascending = sort->find("ascending");
				if (ascending != sort->end())
				{
					if (!ascending->is_boolean())
						return fail("sort.ascending must be a boolean");
					parsed.sort_ascending = ascending->get<bool>();
				}
			}

			out = std::move(parsed);
			if (error)
				error->clear();
			return true;
		}
		catch (const std::exception& e)
		{
			return fail(e.what());
		}
	}

	/* --------------------------------------------------------------------- */
	/* Construction / widget                                                 */
	/* --------------------------------------------------------------------- */

	table_view::table_view() = default;

	table_view::~table_view()
	{
		editing_ = false;
		edit_preview_active_ = false;
		if (context_)
		{
			if (popup_content_ && vgui_popup_content(context_) == popup_content_)
				vgui_popup_close(context_);
			if (edit_menu_ && vgui_menu_is_open(edit_menu_))
				vgui_menu_close(edit_menu_);
		}
		close_popup();
		popup_content_ = nullptr;

		if (widget_)
		{
			/* The widget belongs to the host tree and may outlive this
			 * component; never leave a callback pointing at freed state. */
			vgui_custom_set_draw(widget_, nullptr, nullptr);
			vgui_custom_set_event(widget_, nullptr, nullptr);
		}
		widget_ = nullptr;
		context_ = nullptr;

		edit_number_.reset();
		if (edit_number_panel_)
			vgui_widget_destroy(edit_number_panel_);
		edit_number_panel_ = nullptr;
		for (int i = 0; i < 3; ++i)
		{
			edit_vectors_[i].reset();
			if (edit_vector_panels_[i])
				vgui_widget_destroy(edit_vector_panels_[i]);
			edit_vector_panels_[i] = nullptr;
		}
		if (edit_color_)
		{
			if (edit_color_->widget())
				vgui_widget_destroy(edit_color_->widget());
			edit_color_.reset();
		}
		if (edit_curve_)
		{
			if (edit_curve_->widget())
				vgui_widget_destroy(edit_curve_->widget());
			edit_curve_.reset();
		}
		if (edit_gradient_)
		{
			if (edit_gradient_->widget())
				vgui_widget_destroy(edit_gradient_->widget());
			edit_gradient_.reset();
		}
		if (edit_text_)
			vgui_widget_destroy(edit_text_);
		edit_text_ = nullptr;
		if (edit_bool_)
			vgui_widget_destroy(edit_bool_);
		edit_bool_ = nullptr;
		if (edit_menu_)
			vgui_widget_destroy(edit_menu_);
		edit_menu_ = nullptr;
	}

	void table_view::build(vgui_widget* parent)
	{
		if (widget_ || !parent)
			return;
		context_ = vgui_widget_get_context(parent);
		widget_ = vgui_widget_add(parent, VGUI_CUSTOM, nullptr);
		if (!widget_)
			return;
		vgui_widget_set_id(widget_, "table_view");
		vgui_widget_set_a11y_name(widget_, "Table");
		vgui_widget_set_size(widget_, 420, 220);
		vgui_widget_set_min_size(widget_, 160, 80);
		vgui_widget_set_grow(widget_, 1.0f);
		vgui_widget_set_focusable(widget_, 1);
		vgui_widget_set_border(widget_, 0xFF2E3440u, 1.0f);
		vgui_custom_set_draw(widget_, &table_view::on_draw, this);
		vgui_custom_set_event(widget_, &table_view::on_event, this);
		vgui_custom_set_clip_content(widget_, 1);
	}

	vgui_widget* table_view::widget() const noexcept
	{
		return widget_;
	}

	bool table_view::built() const noexcept
	{
		return widget_ != nullptr;
	}

	void table_view::focus()
	{
		request_focus();
	}

	/* --------------------------------------------------------------------- */
	/* Data source / columns                                                 */
	/* --------------------------------------------------------------------- */

	void table_view::set_source(table_source source)
	{
		source_ = std::move(source);
		refresh();
	}

	const table_source& table_view::source() const noexcept
	{
		return source_;
	}

	bool table_view::add_column(table_column column)
	{
		if (column.id.empty())
			column.id = "column_" + std::to_string(columns_.size());
		for (const table_column& existing : columns_)
		{
			if (existing.id == column.id)
				return false;
		}
		if (column.width <= 0.0f || !std::isfinite(column.width))
			column.width = 120.0f;
		if (column.min_width < 1.0f || !std::isfinite(column.min_width))
			column.min_width = 1.0f;
		column.width = std::max(column.width, column.min_width);
		if (column.label.empty())
			column.label = column.id;
		columns_.push_back(std::move(column));
		clamp_scroll();
		return true;
	}

	void table_view::set_columns(std::vector<table_column> columns)
	{
		columns_.clear();
		columns_.reserve(columns.size());
		for (table_column& column : columns)
		{
			if (column.id.empty())
				column.id = "column_" + std::to_string(columns_.size());
			std::string base = column.id;
			int suffix = 1;
			while (std::any_of(columns_.begin(), columns_.end(),
				[&](const table_column& existing) {
					return existing.id == column.id;
				}))
			{
				column.id = base + "_" + std::to_string(suffix++);
			}
			if (column.width <= 0.0f || !std::isfinite(column.width))
				column.width = 120.0f;
			if (column.min_width < 1.0f || !std::isfinite(column.min_width))
				column.min_width = 1.0f;
			column.width = std::max(column.width, column.min_width);
			if (column.label.empty())
				column.label = column.id;
			columns_.push_back(std::move(column));
		}
		if (sort_column_ >= (int)columns_.size())
			clear_sort();
		else if (sort_column_ >= 0 && !columns_[(size_t)sort_column_].sortable)
			clear_sort();
		else if (sort_column_ >= 0)
			rebuild_display();
		clamp_scroll();
	}

	void table_view::clear_columns()
	{
		columns_.clear();
		clear_sort();
		clear_selection();
		scroll_x_ = 0.0f;
		scroll_y_ = 0.0f;
	}

	const std::vector<table_column>& table_view::columns() const noexcept
	{
		return columns_;
	}

	std::size_t table_view::column_count() const noexcept
	{
		return columns_.size();
	}

	bool table_view::set_column_width(std::size_t index, float width)
	{
		if (index >= columns_.size() || !std::isfinite(width))
			return false;
		columns_[index].width = std::max(width, columns_[index].min_width);
		clamp_scroll();
		return true;
	}

	float table_view::column_width(std::size_t index) const noexcept
	{
		return index < columns_.size() ? columns_[index].width : 0.0f;
	}

	/* --------------------------------------------------------------------- */
	/* Rows / sorting                                                        */
	/* --------------------------------------------------------------------- */

	void table_view::refresh()
	{
		row_count_ = source_.row_count ? source_.row_count() : 0;
		rebuild_display();
		if (selected_row_id_ != invalid_object)
		{
			std::size_t restored = 0;
			if (find_row_id(selected_row_id_, restored))
				selected_.row = restored;
			else if (selected_.valid())
			{
				clear_selection();
			}
		}
		else if (selected_.valid() && selected_.row >= row_count_)
		{
			clear_selection();
		}
		if (selected_.valid() && selected_.column >= (int)columns_.size())
			clear_selection();
		clamp_scroll();
	}

	void table_view::refresh_row(std::size_t source_row)
	{
		if (source_row >= row_count_)
			return;
		if (sort_column_ >= 0)
			rebuild_display();
		clamp_scroll();
	}

	std::size_t table_view::row_count() const noexcept
	{
		return row_count_;
	}

	std::size_t table_view::display_row_count() const noexcept
	{
		return row_count_;
	}

	std::size_t table_view::source_row_at(std::size_t display_row) const noexcept
	{
		if (display_row >= row_count_)
			return 0;
		if (display_.empty())
			return display_row;
		return display_row < display_.size() ? display_[display_row] : 0;
	}

	int table_view::display_index_of(std::size_t source_row) const noexcept
	{
		if (source_row >= row_count_)
			return -1;
		if (display_.empty())
			return (int)source_row;
		for (std::size_t i = 0; i < display_.size(); ++i)
		{
			if (display_[i] == source_row)
				return (int)i;
		}
		return -1;
	}

	table_row_range table_view::visible_row_range() const noexcept
	{
		layout_metrics m = metrics();
		const float row_height = row_height_px();
		if (m.body_height <= 0.0f || row_height <= 0.0f || row_count_ == 0)
			return {};
		const std::size_t first = (std::size_t)std::max(0.0f,
			std::floor(scroll_y_ / row_height));
		if (first >= row_count_)
			return {};
		const float visible_height = std::max(0.0f, m.body_height);
		std::size_t count = (std::size_t)std::max(1.0f,
			std::ceil((scroll_y_ + visible_height) / row_height)) - first;
		count = std::min(count, row_count_ - first);
		return { first, count };
	}

	int table_view::sort_column() const noexcept
	{
		return sort_column_;
	}

	bool table_view::sort_ascending() const noexcept
	{
		return sort_ascending_;
	}

	bool table_view::set_sort(int column, bool ascending)
	{
		if (column < 0)
		{
			clear_sort();
			return true;
		}
		if (column >= (int)columns_.size() ||
			!columns_[(size_t)column].sortable)
			return false;
		sort_column_ = column;
		sort_ascending_ = ascending;
		rebuild_display();
		if (selected_row_id_ != invalid_object)
		{
			std::size_t row = 0;
			if (find_row_id(selected_row_id_, row))
				selected_.row = row;
		}
		clamp_scroll();
		return true;
	}

	void table_view::clear_sort()
	{
		sort_column_ = -1;
		sort_ascending_ = true;
		display_.clear();
		clamp_scroll();
	}

	void table_view::rebuild_display()
	{
		display_.clear();
		if (sort_column_ < 0 || sort_column_ >= (int)columns_.size() ||
			!columns_[(size_t)sort_column_].sortable || row_count_ == 0)
			return;

		const table_column& column = columns_[(size_t)sort_column_];
		const property_type type = column.editor.type;
		const bool numeric = type == property_type::integer ||
			type == property_type::number;
		const bool boolean = type == property_type::boolean;

		struct entry
		{
			std::size_t row = 0;
			std::string text;
			std::string folded;
			double number = 0.0;
			bool has_number = false;
			bool flag = false;
		};
		std::vector<entry> entries;
		entries.reserve(row_count_);
		for (std::size_t row = 0; row < row_count_; ++row)
		{
			entry item;
			item.row = row;
			item.text = cell_text(row, sort_column_);
			item.folded = lower_copy(item.text);
			if (numeric)
				item.has_number = parse_double_text(item.text, item.number);
			else if (boolean)
				(void)parse_bool_text(item.text, item.flag);
			entries.push_back(std::move(item));
		}

		auto less = [&](const entry& a, const entry& b) {
			int result = 0;
			if (column.compare)
			{
				result = column.compare(a.text, b.text);
			}
			else if (numeric)
			{
				if (a.has_number && b.has_number)
					result = a.number < b.number ? -1 :
						(a.number > b.number ? 1 : 0);
				else if (a.has_number != b.has_number)
					result = a.has_number ? -1 : 1;
				else
					result = a.folded < b.folded ? -1 :
						(a.folded > b.folded ? 1 : 0);
			}
			else if (boolean)
			{
				result = a.flag == b.flag ? 0 : (a.flag ? 1 : -1);
			}
			else
			{
				result = a.folded < b.folded ? -1 :
					(a.folded > b.folded ? 1 : 0);
			}
			if (result == 0)
				result = a.row < b.row ? -1 : (a.row > b.row ? 1 : 0);
			return result;
		};

		std::sort(entries.begin(), entries.end(), [&](const entry& a,
			const entry& b) {
			const int result = less(a, b);
			return sort_ascending_ ? result < 0 : result > 0;
		});

		display_.reserve(entries.size());
		for (const entry& item : entries)
			display_.push_back(item.row);
	}

	/* --------------------------------------------------------------------- */
	/* Selection / navigation                                                */
	/* --------------------------------------------------------------------- */

	table_cell table_view::selected_cell() const noexcept
	{
		return selected_;
	}

	bool table_view::select_cell(std::size_t source_row, int column)
	{
		if (source_row >= row_count_ || column < 0 ||
			column >= (int)columns_.size())
			return false;
		selected_ = { source_row, column };
		selected_row_id_ = row_id_at(source_row);
		notify_selection();
		return true;
	}

	void table_view::clear_selection()
	{
		const bool was = selected_.valid();
		selected_ = {};
		selected_row_id_ = invalid_object;
		if (was)
			notify_selection();
	}

	table_cell table_view::hit_test(float content_x, float content_y) const
	{
		layout_metrics m = metrics();
		if (content_x < 0.0f || content_y < 0.0f ||
			content_x >= m.body_width || content_y < m.header_height ||
			content_y >= m.header_height + m.body_height)
			return {};
		const int column = column_at(content_x + scroll_x_);
		if (column < 0)
			return {};
		const std::size_t display = row_at(content_y - m.header_height +
			scroll_y_);
		if (display == kNoRow)
			return {};
		return { source_row_at(display), column };
	}

	void table_view::set_selection_listener(selection_listener listener)
	{
		selection_listener_ = std::move(listener);
	}

	void table_view::notify_selection()
	{
		if (selection_listener_)
			selection_listener_(selected_);
	}

	/* --------------------------------------------------------------------- */
	/* Geometry / scrolling                                                  */
	/* --------------------------------------------------------------------- */

	float table_view::ui_scale() const
	{
		return context_ ? vgui_editor_kit::ui_scale_of(context_)
			: vgui_editor_kit::ui_scale_of(widget_);
	}

	float table_view::column_width_px(std::size_t column) const
	{
		if (column >= columns_.size())
			return 0.0f;
		return std::max(0.0f, columns_[column].width * ui_scale());
	}

	float table_view::row_height_px() const
	{
		return std::max(1.0f, row_height_ * ui_scale());
	}

	table_view::layout_metrics table_view::metrics() const
	{
		layout_metrics m;
		if (!widget_)
			return m;

		const float scale = ui_scale();
		float wx = 0.0f, wy = 0.0f, ww = 0.0f, wh = 0.0f;
		vgui_widget_get_rect(widget_, &wx, &wy, &ww, &wh);
		m.x = wx + kBorderInset * scale;
		m.y = wy + kBorderInset * scale;
		m.width = std::max(0.0f, ww - kBorderInset * 2.0f * scale);
		m.height = std::max(0.0f, wh - kBorderInset * 2.0f * scale);
		if (m.width <= 0.0f || m.height <= 0.0f)
			return m;

		m.header_x = m.x;
		m.header_y = m.y;
		m.header_width = m.width;
		m.header_height = std::min(kHeaderHeight * scale, m.height);
		m.body_x = m.x;
		m.body_y = m.y + m.header_height;
		m.body_width = m.width;
		m.body_height = std::max(0.0f, m.height - m.header_height);

		m.columns_width = 0.0f;
		for (std::size_t i = 0; i < columns_.size(); ++i)
			m.columns_width += column_width_px(i);
		m.rows_height = (float)row_count_ * row_height_px();

		const float thickness = kScrollbarThickness * scale;
		bool vbar = m.rows_height > m.body_height + 0.5f;
		if (vbar)
			m.body_width = std::max(0.0f, m.body_width - thickness);
		bool hbar = m.columns_width > m.body_width + 0.5f;
		if (hbar)
			m.body_height = std::max(0.0f, m.body_height - thickness);
		if (!vbar && m.rows_height > m.body_height + 0.5f)
		{
			vbar = true;
			m.body_width = std::max(0.0f, m.body_width - thickness);
			if (m.columns_width > m.body_width + 0.5f)
				hbar = true;
		}

		m.vertical_scrollbar = vbar;
		m.horizontal_scrollbar = hbar;
		if (vbar)
		{
			m.vbar_x = m.x + m.width - thickness;
			m.vbar_y = m.body_y;
			m.vbar_width = thickness;
			m.vbar_height = std::max(0.0f, m.height - m.header_height -
				(hbar ? thickness : 0.0f));
		}
		if (hbar)
		{
			m.hbar_x = m.x;
			m.hbar_y = m.y + m.height - thickness;
			m.hbar_width = std::max(0.0f, m.width - (vbar ? thickness : 0.0f));
			m.hbar_height = thickness;
		}
		return m;
	}

	float table_view::column_offset(std::size_t column) const
	{
		float offset = 0.0f;
		const std::size_t count = std::min(column, columns_.size());
		for (std::size_t i = 0; i < count; ++i)
			offset += column_width_px(i);
		return offset;
	}

	int table_view::column_at(float content_x) const
	{
		float offset = 0.0f;
		for (std::size_t i = 0; i < columns_.size(); ++i)
		{
			const float width = column_width_px(i);
			if (content_x >= offset && content_x < offset + width)
				return (int)i;
			offset += width;
		}
		return -1;
	}

	std::size_t table_view::row_at(float content_y) const
	{
		const float row_height = row_height_px();
		if (row_height <= 0.0f || content_y < 0.0f)
			return kNoRow;
		const std::size_t row = (std::size_t)std::floor(content_y / row_height);
		return row < row_count_ ? row : kNoRow;
	}

	bool table_view::cell_rect(std::size_t source_row, int column, float* x,
		float* y, float* width, float* height) const
	{
		if (source_row >= row_count_ || column < 0 ||
			column >= (int)columns_.size())
			return false;
		const int display = display_index_of(source_row);
		if (display < 0)
			return false;
		const layout_metrics m = metrics();
		if (x)
			*x = m.body_x + column_offset((std::size_t)column) - scroll_x_;
		if (y)
			*y = m.body_y + (float)display * row_height_px() - scroll_y_;
		if (width)
			*width = column_width_px((std::size_t)column);
		if (height)
			*height = row_height_px();
		return true;
	}

	bool table_view::header_rect(int column, float* x, float* y, float* width,
		float* height) const
	{
		if (column < 0 || column >= (int)columns_.size())
			return false;
		const layout_metrics m = metrics();
		if (x)
			*x = m.header_x + column_offset((std::size_t)column) - scroll_x_;
		if (y)
			*y = m.header_y;
		if (width)
			*width = column_width_px((std::size_t)column);
		if (height)
			*height = m.header_height;
		return true;
	}

	float table_view::scroll_x() const noexcept
	{
		return scroll_x_;
	}

	float table_view::scroll_y() const noexcept
	{
		return scroll_y_;
	}

	void table_view::clamp_scroll()
	{
		scroll_x_ = std::clamp(scroll_x_, 0.0f, max_scroll_x());
		scroll_y_ = std::clamp(scroll_y_, 0.0f, max_scroll_y());
		if (!std::isfinite(scroll_x_))
			scroll_x_ = 0.0f;
		if (!std::isfinite(scroll_y_))
			scroll_y_ = 0.0f;
	}

	void table_view::set_scroll(float x, float y)
	{
		scroll_x_ = std::isfinite(x) ? x : 0.0f;
		scroll_y_ = std::isfinite(y) ? y : 0.0f;
		clamp_scroll();
	}

	float table_view::max_scroll_x() const noexcept
	{
		const layout_metrics m = metrics();
		return std::max(0.0f, m.columns_width - m.body_width);
	}

	float table_view::max_scroll_y() const noexcept
	{
		const layout_metrics m = metrics();
		return std::max(0.0f, m.rows_height - m.body_height);
	}

	void table_view::scroll_to_row(std::size_t source_row)
	{
		const int display = display_index_of(source_row);
		if (display < 0)
			return;
		const layout_metrics m = metrics();
		const float row_height = row_height_px();
		const float top = (float)display * row_height;
		const float bottom = top + row_height;
		if (top < scroll_y_)
			scroll_y_ = top;
		else if (bottom > scroll_y_ + m.body_height)
			scroll_y_ = bottom - m.body_height;
		clamp_scroll();
	}

	void table_view::set_row_height(float height)
	{
		if (height <= 0.0f || !std::isfinite(height))
			return;
		row_height_ = height;
		clamp_scroll();
	}

	float table_view::row_height() const noexcept
	{
		return row_height_;
	}

	float table_view::header_height() const noexcept
	{
		return kHeaderHeight;
	}

	/* --------------------------------------------------------------------- */
	/* Editing                                                               */
	/* --------------------------------------------------------------------- */

	bool table_view::begin_edit()
	{
		if (!selected_.valid())
			return false;
		return begin_edit(selected_.row, selected_.column);
	}

	bool table_view::begin_edit(std::size_t source_row, int column)
	{
		if (!widget_ || !context_ || source_row >= row_count_)
			return false;
		const table_column* item = column_at_index(column);
		if (!item || !item->editable || item->editor.read_only)
			return false;
		if (editing_)
			finish_edit_session(false);

		select_cell(source_row, column);
		scroll_to_row(source_row);

		float cx = 0.0f, cy = 0.0f, cw = 0.0f, ch = 0.0f;
		if (!cell_rect(source_row, column, &cx, &cy, &cw, &ch))
			return false;

		const property_desc* desc = editor_desc(column);
		const property_desc fallback;
		if (!desc)
			desc = &fallback;
		const property_type type = desc->type;

		edit_cell_ = { source_row, column };
		edit_original_ = cell_text(source_row, column);
		edit_preview_value_ = edit_original_;
		edit_preview_active_ = false;
		editing_ = true;
		edit_kind_ = edit_kind::none;

		switch (type)
		{
		case property_type::integer:
		case property_type::number:
		{
			ensure_number_editor();
			if (!edit_number_)
				break;
			configure_number_editor(*desc, edit_original_);
			edit_kind_ = edit_kind::number;
			popup_content_ = edit_number_panel_;
			if (!begin_popup_edit(popup_content_, edit_cell_, cx,
				cy + ch + 2.0f))
			{
				editing_ = false;
				edit_kind_ = edit_kind::none;
				return false;
			}
			edit_number_->focus();
			return true;
		}
		case property_type::boolean:
		{
			ensure_bool_editor();
			if (!edit_bool_)
				break;
			bool checked = false;
			(void)parse_bool_text(edit_original_, checked);
			vgui_widget_set_checked(edit_bool_, checked ? 1 : 0);
			vgui_widget_set_text(edit_bool_, checked ? "true" : "false");
			vgui_widget_set_size(edit_bool_,
				std::max(110.0f, to_logical(widget_, cw)), 30.0f);
			edit_kind_ = edit_kind::boolean;
			popup_content_ = edit_bool_;
			if (!begin_popup_edit(popup_content_, edit_cell_, cx,
				cy + ch + 2.0f))
			{
				editing_ = false;
				edit_kind_ = edit_kind::none;
				return false;
			}
			vgui_set_focus(context_, edit_bool_);
			return true;
		}
		case property_type::enumeration:
		{
			ensure_enum_menu();
			if (!edit_menu_)
				break;
			edit_kind_ = edit_kind::enumeration;
			edit_menu_values_.clear();
			vgui_menu_clear(edit_menu_);
			std::vector<std::string> labels = desc->choices;
			std::vector<std::string> values = desc->choice_values;
			if (values.empty())
				values = desc->choices;
			if (labels.empty())
				labels = values;
			if (desc->allow_empty)
			{
				const std::string label = desc->placeholder.empty()
					? "(None)" : desc->placeholder;
				vgui_item_id item = vgui_menu_add_item(edit_menu_, label.c_str());
				edit_menu_values_[item] = "";
			}
			const std::size_t count = std::min(labels.size(), values.size());
			for (std::size_t i = 0; i < count; ++i)
			{
				vgui_item_id item = vgui_menu_add_item(edit_menu_,
					labels[i].c_str());
				if (item)
					edit_menu_values_[item] = values[i];
			}
			vgui_menu_open(edit_menu_, cx, cy + ch);
			return true;
		}
		case property_type::color:
		{
			ensure_color_editor();
			if (!edit_color_)
				break;
			edit_color_->set_value(edit_original_);
			edit_color_->set_current(edit_original_);
			edit_kind_ = edit_kind::color;
			popup_content_ = edit_color_->widget();
			if (!begin_popup_edit(popup_content_, edit_cell_, cx,
				cy + ch + 2.0f))
			{
				editing_ = false;
				edit_kind_ = edit_kind::none;
				return false;
			}
			return true;
		}
		case property_type::curve:
		{
			ensure_curve_editor();
			if (!edit_curve_)
				break;
			(void)edit_curve_->set_value_text(edit_original_);
			edit_kind_ = edit_kind::curve;
			popup_content_ = edit_curve_->widget();
			if (!begin_popup_edit(popup_content_, edit_cell_, cx,
				cy + ch + 2.0f))
			{
				editing_ = false;
				edit_kind_ = edit_kind::none;
				return false;
			}
			return true;
		}
		case property_type::gradient:
		{
			ensure_gradient_editor();
			if (!edit_gradient_)
				break;
			(void)edit_gradient_->set_value_text(edit_original_);
			edit_kind_ = edit_kind::gradient;
			popup_content_ = edit_gradient_->widget();
			if (!begin_popup_edit(popup_content_, edit_cell_, cx,
				cy + ch + 2.0f))
			{
				editing_ = false;
				edit_kind_ = edit_kind::none;
				return false;
			}
			return true;
		}
		case property_type::vector2:
		case property_type::vector3:
		case property_type::vector4:
		{
			const int dimension = property_dimension(type);
			ensure_vector_editor(dimension);
			const int slot = std::clamp(dimension, 2, 4) - 2;
			if (!edit_vectors_[slot])
				break;
			configure_vector_editor(*desc, edit_original_);
			edit_kind_ = edit_kind::vector;
			popup_content_ = edit_vector_panels_[slot];
			if (!begin_popup_edit(popup_content_, edit_cell_, cx,
				cy + ch + 2.0f))
			{
				editing_ = false;
				edit_kind_ = edit_kind::none;
				return false;
			}
			edit_vectors_[slot]->focus_component(0);
			return true;
		}
		case property_type::text:
		case property_type::asset:
		case property_type::object_ref:
		default:
		{
			ensure_text_editor();
			if (!edit_text_)
				break;
			edit_kind_ = edit_kind::text;
			vgui_edit_set_text(edit_text_, edit_original_.c_str());
			vgui_edit_set_readonly(edit_text_, 0);
			vgui_widget_set_size(edit_text_,
				std::max(140.0f, to_logical(widget_, cw)), 28.0f);
			popup_content_ = edit_text_;
			if (!begin_popup_edit(popup_content_, edit_cell_, cx,
				cy + ch + 2.0f))
			{
				editing_ = false;
				edit_kind_ = edit_kind::none;
				return false;
			}
			vgui_set_focus(context_, edit_text_);
			vgui_edit_select_all(edit_text_);
			return true;
		}
		}

		editing_ = false;
		edit_kind_ = edit_kind::none;
		return false;
	}

	bool table_view::cancel_edit()
	{
		if (!editing_)
			return false;
		finish_edit_session(false);
		return true;
	}

	bool table_view::editing() const noexcept
	{
		return editing_;
	}

	table_cell table_view::editing_cell() const noexcept
	{
		return editing_ ? edit_cell_ : table_cell{};
	}

	bool table_view::commit_edit(std::string value)
	{
		if (!editing_ || !edit_cell_.valid())
			return false;
		const property_desc* desc = editor_desc(edit_cell_.column);
		property_value_result normalized;
		if (desc)
			normalized = property_normalize_value(*desc, std::move(value));
		else
		{
			normalized.ok = true;
			normalized.value = std::move(value);
		}
		if (!normalized.ok)
			return false;
		const std::string old_value = edit_original_;
		if (normalized.value != old_value)
		{
			if (!apply_cell_value(edit_cell_, normalized.value, true))
				return false;
			push_cell_command(edit_cell_, old_value, normalized.value);
			edit_original_ = normalized.value;
		}
		finish_edit_session(false);
		return true;
	}

	void table_view::set_edit_handler(edit_handler handler)
	{
		edit_handler_ = std::move(handler);
	}

	void table_view::set_edit_listener(edit_listener listener)
	{
		edit_listener_ = std::move(listener);
	}

	void table_view::set_command_stack(command_stack* stack)
	{
		commands_ = stack;
	}

	command_stack* table_view::command_stack_ptr() const noexcept
	{
		return commands_;
	}

	std::string table_view::editor_value_text() const
	{
		switch (edit_kind_)
		{
		case edit_kind::text:
			if (edit_text_)
			{
				const char* text = vgui_edit_get_text(edit_text_);
				return text ? text : "";
			}
			break;
		case edit_kind::number:
			if (edit_number_)
				return edit_number_->value_text();
			break;
		case edit_kind::vector:
		{
			const int dimension =
				property_dimension(editor_desc(edit_cell_.column)
					? editor_desc(edit_cell_.column)->type
					: property_type::vector3);
			const int slot = std::clamp(dimension, 2, 4) - 2;
			if (edit_vectors_[slot])
				return edit_vectors_[slot]->value_text();
			break;
		}
		case edit_kind::boolean:
			if (edit_bool_)
				return vgui_widget_get_checked(edit_bool_) ? "true" : "false";
			break;
		case edit_kind::color:
			if (edit_color_)
				return edit_preview_active_ ? edit_preview_value_
					: edit_color_->value();
			break;
		case edit_kind::curve:
			if (edit_curve_)
				return edit_preview_active_ ? edit_preview_value_
					: edit_curve_->value_text();
			break;
		case edit_kind::gradient:
			if (edit_gradient_)
				return edit_preview_active_ ? edit_preview_value_
					: edit_gradient_->value_text();
			break;
		case edit_kind::enumeration:
			break;
		case edit_kind::none:
			break;
		}
		return edit_preview_active_ ? edit_preview_value_ : edit_original_;
	}

	bool table_view::commit_current_edit()
	{
		if (!editing_ || !edit_cell_.valid())
			return false;
		const property_desc* desc = editor_desc(edit_cell_.column);
		const std::string text = editor_value_text();
		property_value_result normalized;
		if (desc)
			normalized = property_normalize_value(*desc, text);
		else
		{
			normalized.ok = true;
			normalized.value = text;
		}
		if (!normalized.ok)
		{
			if (edit_kind_ == edit_kind::number && edit_number_)
				edit_number_->set_error(normalized.error);
			return false;
		}
		const std::string old_value = edit_original_;
		if (normalized.value == old_value)
			return true;
		if (!apply_cell_value(edit_cell_, normalized.value, true))
			return false;
		push_cell_command(edit_cell_, old_value, normalized.value);
		edit_preview_value_ = normalized.value;
		edit_preview_active_ = false;
		return true;
	}

	void table_view::set_edit_preview(const std::string& value)
	{
		if (!editing_ || !edit_cell_.valid())
			return;
		const property_desc* desc = editor_desc(edit_cell_.column);
		property_value_result normalized;
		if (desc)
			normalized = property_normalize_value(*desc, value);
		else
		{
			normalized.ok = true;
			normalized.value = value;
		}
		if (!normalized.ok)
			return;
		edit_preview_value_ = normalized.value;
		edit_preview_active_ = edit_preview_value_ != edit_original_;
	}

	void table_view::finish_edit_session(bool commit)
	{
		if (!editing_ || finishing_edit_)
			return;
		finishing_edit_ = true;

		const edit_kind kind = edit_kind_;
		bool applied = false;
		if (commit)
		{
			if (kind == edit_kind::color || kind == edit_kind::curve ||
				kind == edit_kind::gradient)
			{
				if (edit_preview_active_)
				{
					const std::string old_value = edit_original_;
					if (apply_cell_value(edit_cell_, edit_preview_value_, true))
					{
						push_cell_command(edit_cell_, old_value,
							edit_preview_value_);
						applied = true;
					}
				}
			}
			else if (kind != edit_kind::boolean &&
				kind != edit_kind::enumeration &&
				kind != edit_kind::none)
			{
				applied = commit_current_edit();
			}
		}
		(void)applied;

		editing_ = false;
		edit_kind_ = edit_kind::none;
		edit_cell_ = {};
		edit_original_.clear();
		edit_preview_value_.clear();
		edit_preview_active_ = false;
		close_popup();
		close_menu();
		finishing_edit_ = false;
	}

	void table_view::close_popup()
	{
		if (context_ && popup_content_ &&
			vgui_popup_content(context_) == popup_content_)
			vgui_popup_close(context_);
	}

	void table_view::close_menu()
	{
		if (edit_menu_ && vgui_menu_is_open(edit_menu_))
			vgui_menu_close(edit_menu_);
	}

	bool table_view::begin_popup_edit(vgui_widget* content, const table_cell& cell,
		float x, float y)
	{
		(void)cell;
		if (!context_ || !content)
			return false;
		if (!vgui_popup_open(context_, content, x, y))
			return false;
		vgui_popup_on_close(context_, &table_view::on_popup_close, this);
		return true;
	}

	void table_view::configure_number_editor(const property_desc& desc,
		const std::string& value)
	{
		if (!edit_number_)
			return;
		edit_number_->set_value_type(desc.type == property_type::integer
			? spinbox::value_type::integer : spinbox::value_type::number);
		if (desc.has_range)
			edit_number_->set_range(desc.min_value, desc.max_value);
		else
			edit_number_->clear_range();
		edit_number_->set_step(desc.step);
		edit_number_->set_error("");
		double number = 0.0;
		if (parse_double_text(value, number))
			edit_number_->set_value(number);
		else
			edit_number_->set_value(0.0);
		if (edit_number_panel_)
			vgui_widget_set_size(edit_number_panel_, 180.0f, 30.0f);
	}

	void table_view::configure_vector_editor(const property_desc& desc,
		const std::string& value)
	{
		const int dimension = property_dimension(desc.type);
		const int slot = std::clamp(dimension, 2, 4) - 2;
		vector_editor* editor = edit_vectors_[slot].get();
		if (!editor)
			return;
		editor->set_semantic(desc.semantic);
		editor->set_value_type(desc.type == property_type::integer
			? spinbox::value_type::integer : spinbox::value_type::number);
		if (desc.has_range)
			editor->set_range(desc.min_value, desc.max_value);
		else
			editor->clear_range();
		editor->set_step(desc.step);
		if (!desc.component_labels.empty())
			editor->set_axis_labels(desc.component_labels);
		if (!desc.component_colors.empty())
			editor->set_axis_colors(desc.component_colors);
		editor->clear_component_errors();
		vector_editor::vector_value values{};
		if (vector_editor::parse_value(value, dimension, values))
			editor->set_value(values);
		if (edit_vector_panels_[slot])
			vgui_widget_set_size(edit_vector_panels_[slot], 330.0f, 30.0f);
	}

	/* --------------------------------------------------------------------- */
	/* Typed editors                                                         */
	/* --------------------------------------------------------------------- */

	void table_view::ensure_text_editor()
	{
		if (edit_text_ || !context_)
			return;
		edit_text_ = vgui_widget_create(context_, VGUI_EDIT, "");
		if (!edit_text_)
			return;
		vgui_widget_set_id(edit_text_, "table_view.cell_text");
		vgui_widget_set_size(edit_text_, 160, 28);
		vgui_widget_set_font_size(edit_text_, 12);
		vgui_widget_on_event(edit_text_, &table_view::on_text_event, this);
	}

	void table_view::ensure_number_editor()
	{
		if (edit_number_ || !context_)
			return;
		edit_number_panel_ = vgui_widget_create(context_, VGUI_PANEL, nullptr);
		if (!edit_number_panel_)
			return;
		vgui_widget_set_id(edit_number_panel_, "table_view.number_popup");
		vgui_widget_set_layout(edit_number_panel_, VGUI_LAYOUT_ROW);
		vgui_widget_set_padding(edit_number_panel_, 4, 2, 4, 2);
		vgui_widget_set_size(edit_number_panel_, 180, 30);
		edit_number_ = std::make_unique<spinbox>();
		edit_number_->set_id("table_view.cell_number");
		/* Cell popups keep the table's compact 24px chrome. */
		edit_number_->set_font_size(13.0f);
		edit_number_->build(edit_number_panel_, "");
		if (!edit_number_->widget())
		{
			edit_number_.reset();
			return;
		}
		vgui_widget_set_grow(edit_number_->widget(), 1.0f);
		vgui_widget_set_size(edit_number_->widget(), 0, 24);
		edit_number_->set_buttons_visible(false);
		edit_number_->set_on_commit([this](double) {
			if (commit_current_edit())
				finish_edit_session(false);
		});
	}

	void table_view::ensure_vector_editor(int dimension)
	{
		if (!context_)
			return;
		const int slot = std::clamp(dimension, 2, 4) - 2;
		if (edit_vectors_[slot])
			return;
		vgui_widget* panel = vgui_widget_create(context_, VGUI_PANEL, nullptr);
		if (!panel)
			return;
		vgui_widget_set_id(panel, "table_view.vector_popup");
		vgui_widget_set_layout(panel, VGUI_LAYOUT_ROW);
		vgui_widget_set_padding(panel, 4, 2, 4, 2);
		vgui_widget_set_size(panel, 330, 30);
		auto editor = std::make_unique<vector_editor>();
		editor->set_id("table_view.cell_vector");
		editor->build(panel, std::clamp(dimension, 2, 4));
		if (!editor->widget())
			return;
		vgui_widget_set_grow(editor->widget(), 1.0f);
		vgui_widget_set_align_self(editor->widget(), VGUI_ALIGN_STRETCH);
		editor->set_on_commit([this](const vector_editor::vector_value&) {
			if (commit_current_edit())
				finish_edit_session(false);
		});
		edit_vectors_[slot] = std::move(editor);
		edit_vector_panels_[slot] = panel;
	}

	void table_view::ensure_bool_editor()
	{
		if (edit_bool_ || !context_)
			return;
		edit_bool_ = vgui_widget_create(context_, VGUI_CHECKBOX, "false");
		if (!edit_bool_)
			return;
		vgui_widget_set_id(edit_bool_, "table_view.cell_bool");
		vgui_widget_set_size(edit_bool_, 130, 30);
		vgui_widget_on_value(edit_bool_, &table_view::on_bool_value, this);
	}

	void table_view::ensure_color_editor()
	{
		if (edit_color_ || !context_)
			return;
		edit_color_ = std::make_unique<color_picker>();
		edit_color_->set_id("table_view.color_picker");
		edit_color_->build(context_);
		if (!edit_color_->widget())
		{
			edit_color_.reset();
			return;
		}
		static const char* const kPresets[] = {
			"#FFFFFF", "#000000", "#FF0000", "#00FF00",
			"#0000FF", "#FFFF00", "#00FFFF", "#FF00FF"
		};
		std::vector<std::string> presets;
		for (const char* preset : kPresets)
			presets.push_back(preset);
		edit_color_->set_presets(presets);
		edit_color_->set_on_change([this](const std::string& hex) {
			set_edit_preview(hex);
		});
		edit_color_->set_on_commit([this](const std::string& hex) {
			set_edit_preview(hex);
		});
	}

	void table_view::ensure_curve_editor()
	{
		if (edit_curve_ || !context_)
			return;
		edit_curve_ = std::make_unique<curve_editor>();
		edit_curve_->set_id("table_view.curve_editor");
		edit_curve_->build(context_);
		if (!edit_curve_->widget())
		{
			edit_curve_.reset();
			return;
		}
		edit_curve_->set_on_change([this](const curve_value& value) {
			set_edit_preview(curve_editor::format_value(value));
		});
		edit_curve_->set_on_commit([this](const curve_value& value) {
			set_edit_preview(curve_editor::format_value(value));
		});
	}

	void table_view::ensure_gradient_editor()
	{
		if (edit_gradient_ || !context_)
			return;
		edit_gradient_ = std::make_unique<gradient_editor>();
		edit_gradient_->set_id("table_view.gradient_editor");
		edit_gradient_->build(context_);
		if (!edit_gradient_->widget())
		{
			edit_gradient_.reset();
			return;
		}
		edit_gradient_->set_on_change([this](const gradient_value& value) {
			set_edit_preview(gradient_editor::format_value(value));
		});
		edit_gradient_->set_on_commit([this](const gradient_value& value) {
			set_edit_preview(gradient_editor::format_value(value));
		});
	}

	void table_view::ensure_enum_menu()
	{
		if (edit_menu_ || !context_)
			return;
		edit_menu_ = vgui_widget_create(context_, VGUI_MENU, nullptr);
		if (!edit_menu_)
			return;
		vgui_widget_set_id(edit_menu_, "table_view.enum_menu");
		vgui_menu_on_activate(edit_menu_, &table_view::on_enum_activate, this);
		vgui_menu_on_close(edit_menu_, &table_view::on_enum_close, this);
	}

	void table_view::on_popup_close(void* user, int reason)
	{
		auto* self = static_cast<table_view*>(user);
		if (!self)
			return;
		if (self->editing_ && !self->finishing_edit_)
			self->finish_edit_session(reason != VGUI_POPUP_CLOSE_ESCAPE);
		if (self->context_)
			vgui_popup_on_close(self->context_, nullptr, nullptr);
	}

	void table_view::on_text_event(vgui_widget* widget, const vgui_event* event,
		void* user)
	{
		auto* self = static_cast<table_view*>(user);
		if (!self || !event || event->type != VGUI_EVENT_SUBMIT)
			return;
		if (widget != self->edit_text_ || !self->editing_)
			return;
		if (self->commit_current_edit())
			self->finish_edit_session(false);
	}

	void table_view::on_bool_value(vgui_widget* widget, float value, void* user)
	{
		auto* self = static_cast<table_view*>(user);
		if (!self || widget != self->edit_bool_ || !self->editing_)
			return;
		vgui_widget_set_checked(self->edit_bool_, value >= 0.5f ? 1 : 0);
		if (self->commit_current_edit())
			self->finish_edit_session(false);
	}

	void table_view::on_enum_activate(vgui_widget* menu, vgui_item_id item,
		void* user)
	{
		auto* self = static_cast<table_view*>(user);
		if (!self || menu != self->edit_menu_ || !self->editing_)
			return;
		auto found = self->edit_menu_values_.find(item);
		if (found == self->edit_menu_values_.end())
			return;
		const std::string value = found->second;
		const property_desc* desc = self->editor_desc(self->edit_cell_.column);
		property_value_result normalized;
		if (desc)
			normalized = property_normalize_value(*desc, value);
		else
		{
			normalized.ok = true;
			normalized.value = value;
		}
		if (!normalized.ok)
			return;
		if (self->apply_cell_value(self->edit_cell_, normalized.value, true))
		{
			self->push_cell_command(self->edit_cell_, self->edit_original_,
				normalized.value);
			self->edit_original_ = normalized.value;
		}
		self->finish_edit_session(false);
	}

	void table_view::on_enum_close(vgui_widget* menu, void* user)
	{
		auto* self = static_cast<table_view*>(user);
		if (!self || menu != self->edit_menu_ || !self->editing_)
			return;
		self->finish_edit_session(false);
	}

	/* --------------------------------------------------------------------- */
	/* Value application / commands                                          */
	/* --------------------------------------------------------------------- */

	const table_column* table_view::column_at_index(int index) const
	{
		if (index < 0 || index >= (int)columns_.size())
			return nullptr;
		return &columns_[(std::size_t)index];
	}

	const property_desc* table_view::editor_desc(int column) const
	{
		const table_column* item = column_at_index(column);
		return item ? &item->editor : nullptr;
	}

	std::string table_view::cell_text(std::size_t source_row, int column) const
	{
		const table_column* item = column_at_index(column);
		if (!item || source_row >= row_count_ || !source_.cell_text)
			return {};
		return source_.cell_text(source_row, item->id);
	}

	bool table_view::apply_cell_value(const table_cell& cell,
		const std::string& value, bool notify)
	{
		const table_column* column = column_at_index(cell.column);
		if (!column || cell.row >= row_count_)
			return false;
		bool ok = false;
		if (column->set_value)
			ok = column->set_value(cell.row, value);
		else if (edit_handler_)
			ok = edit_handler_(cell, value);
		else
			return false;
		if (!ok)
			return false;
		refresh_row(cell.row);
		if (notify)
			notify_edit(cell, value);
		return true;
	}

	void table_view::notify_edit(const table_cell& cell, const std::string& value)
	{
		if (edit_listener_)
			edit_listener_(cell, value);
	}

	void table_view::push_cell_command(const table_cell& cell,
		const std::string& old_value, const std::string& new_value)
	{
		if (!commands_ || old_value == new_value)
			return;
		const table_column* column = column_at_index(cell.column);
		const std::string label = column
			? (column->label.empty() ? column->id : column->label)
			: std::string("cell");
		const std::string column_id = column ? column->id : std::string();
		const object_id row_id = row_id_at(cell.row);
		auto applied = [this, cell, row_id, column_id](const std::string& value) {
			table_cell target = cell;
			if (row_id != invalid_object)
			{
				std::size_t row = 0;
				if (!find_row_id(row_id, row))
					return;
				target.row = row;
			}
			if (!column_id.empty())
			{
				target.column = -1;
				for (std::size_t i = 0; i < columns_.size(); ++i)
				{
					if (columns_[i].id == column_id)
					{
						target.column = (int)i;
						break;
					}
				}
				if (target.column < 0)
					return;
			}
			(void)apply_cell_value(target, value, true);
		};
		commands_->push(std::make_unique<function_command>("Set " + label,
			[applied, new_value]() { applied(new_value); },
			[applied, old_value]() { applied(old_value); }));
	}

	object_id table_view::row_id_at(std::size_t source_row) const
	{
		if (source_row >= row_count_ || !source_.row_id)
			return invalid_object;
		return source_.row_id(source_row);
	}

	bool table_view::find_row_id(object_id id, std::size_t& out) const
	{
		if (id == invalid_object || !source_.row_id)
			return false;
		for (std::size_t row = 0; row < row_count_; ++row)
		{
			if (source_.row_id(row) == id)
			{
				out = row;
				return true;
			}
		}
		return false;
	}

	/* --------------------------------------------------------------------- */
	/* Persistence                                                           */
	/* --------------------------------------------------------------------- */

	table_state table_view::capture_state() const
	{
		table_state state;
		state.column_widths.reserve(columns_.size());
		for (const table_column& column : columns_)
			state.column_widths.push_back(column.width);
		state.sort_column = sort_column_;
		state.sort_ascending = sort_ascending_;
		return state;
	}

	bool table_view::apply_state(const table_state& state, std::string* error)
	{
		if (state.version < 1 || state.version > table_state::current_version)
		{
			if (error)
				*error = "unsupported table state version";
			return false;
		}
		for (float width : state.column_widths)
		{
			if (!std::isfinite(width) || width <= 0.0f)
			{
				if (error)
					*error = "invalid column width";
				return false;
			}
		}
		const std::size_t count = std::min(state.column_widths.size(),
			columns_.size());
		for (std::size_t i = 0; i < count; ++i)
			set_column_width(i, state.column_widths[i]);

		if (state.sort_column < 0)
			clear_sort();
		else if (state.sort_column >= (int)columns_.size() ||
			!columns_[(std::size_t)state.sort_column].sortable)
			clear_sort();
		else
			set_sort(state.sort_column, state.sort_ascending);

		if (error)
			error->clear();
		return true;
	}

	std::size_t table_view::last_render_cell_queries() const noexcept
	{
		return render_cell_queries_;
	}

	/* --------------------------------------------------------------------- */
	/* Drawing                                                               */
	/* --------------------------------------------------------------------- */

	void table_view::on_draw(vgui_widget* widget, ovg_ctx_cb* cb, rvg_t* vg,
		float abs_x, float abs_y, float abs_w, float abs_h, float delta_time,
		void* user)
	{
		(void)delta_time;
		auto* self = static_cast<table_view*>(user);
		if (!self || !widget || !cb || !vg)
			return;
		const float scale = self->ui_scale();
		self->draw(cb, vg, widget, abs_x + kBorderInset * scale,
			abs_y + kBorderInset * scale, abs_w - kBorderInset * 2.0f * scale,
			abs_h - kBorderInset * 2.0f * scale);
	}

	void table_view::draw(ovg_ctx_cb* cb, rvg_t* vg, vgui_widget* widget,
		float x, float y, float width, float height)
	{
		if (width <= 0.0f || height <= 0.0f)
			return;
		(void)x;
		(void)y;
		const layout_metrics m = metrics();
		if (m.width <= 0.0f || m.height <= 0.0f)
			return;

		vgui_theme theme = {};
		if (context_)
			vgui_context_get_theme(context_, &theme);

		const vgui_color background = theme.list_bg ? theme.list_bg :
			(theme.panel_bg ? theme.panel_bg : 0xFF1B1E24u);
		const vgui_color border = theme.list_border ? theme.list_border :
			(theme.panel_border ? theme.panel_border : 0xFF2E3440u);
		ovg_tools::fill_rect(cb, vg, m.x, m.y, m.width, m.height,
			to_ovg(background));
		ovg_tools::stroke_rect(cb, vg, m.x, m.y, m.width, m.height,
			to_ovg(border), 1.0f);

		render_cell_queries_ = 0;
		draw_header(cb, vg, widget, m);
		draw_body(cb, vg, widget, m);
		draw_scrollbars(cb, vg, m);
	}

	void table_view::draw_header(ovg_ctx_cb* cb, rvg_t* vg,
		vgui_widget* widget, const layout_metrics& m)
	{
		vgui_theme theme = {};
		if (context_)
			vgui_context_get_theme(context_, &theme);
		const vgui_color header_bg = theme.button_bg ? theme.button_bg
			: 0xFF232831u;
		const vgui_color border = theme.list_border ? theme.list_border :
			(theme.panel_border ? theme.panel_border : 0xFF2E3440u);
		const vgui_color text_color = theme.text ? theme.text : 0xFFD8DEE9u;
		const vgui_color accent = theme.accent ? theme.accent : 0xFF409EFFu;

		ovg_tools::fill_rect(cb, vg, m.header_x, m.header_y,
			m.header_width, m.header_height, to_ovg(header_bg));
		ovg_tools::line(cb, vg, m.header_x, m.header_y + m.header_height - 0.5f,
			m.header_x + m.header_width, m.header_y + m.header_height - 0.5f,
			to_ovg(border), 1.0f, VG_LINE_CAP_BUTT);

		clip_guard clip(cb, vg, m.header_x, m.header_y, m.body_width,
			m.header_height);
		const float scale = ui_scale();
		float offset = -scroll_x_;
		for (std::size_t i = 0; i < columns_.size(); ++i)
		{
			const table_column& column = columns_[i];
			const float column_width = column_width_px(i);
			const float column_x = m.header_x + offset;
			if (column_x + column_width < m.header_x)
			{
				offset += column_width;
				continue;
			}
			if (column_x > m.header_x + m.body_width)
				break;

			const bool sorted = (int)i == sort_column_;
			const float arrow_space = column.sortable ? 16.0f * scale : 0.0f;
			draw_cell_text(widget, cb, vg, column.label, column_x,
				m.header_y, column_width - arrow_space - kCellPaddingX * scale,
				m.header_height, text_color, table_alignment::left);
			if (sorted)
			{
				const float cx = column_x + column_width - 11.0f * scale;
				const float cy = m.header_y + m.header_height * 0.5f;
				ovg_tools::tri_glyph(cb, vg, cx, cy, 4.0f * scale,
					sort_ascending_ ? 3 : 1, to_ovg(accent));
			}
			if (i + 1 < columns_.size())
			{
				ovg_tools::line(cb, vg, column_x + column_width,
					m.header_y + 3.0f * scale, column_x + column_width,
					m.header_y + m.header_height - 3.0f * scale, to_ovg(border),
					1.0f, VG_LINE_CAP_BUTT);
			}
			offset += column_width;
		}
	}

	void table_view::draw_body(ovg_ctx_cb* cb, rvg_t* vg,
		vgui_widget* widget, const layout_metrics& m)
	{
		vgui_theme theme = {};
		if (context_)
			vgui_context_get_theme(context_, &theme);
		const vgui_color background = theme.list_bg ? theme.list_bg :
			(theme.panel_bg ? theme.panel_bg : 0xFF1B1E24u);
		const vgui_color border = theme.list_border ? theme.list_border :
			(theme.panel_border ? theme.panel_border : 0xFF2E3440u);
		const vgui_color row_text = theme.row_text ? theme.row_text :
			(theme.text ? theme.text : 0xFFD8DEE9u);
		const vgui_color row_hover = theme.row_hover ? theme.row_hover :
			(theme.button_hover ? theme.button_hover : 0xFF2C3440u);
		const vgui_color selection = theme.row_selected ? theme.row_selected
			: with_alpha(theme.accent ? theme.accent : 0xFF409EFFu, 0x55);
		const vgui_color selection_text = theme.row_selected_text
			? theme.row_selected_text : row_text;
		const vgui_color accent = theme.accent ? theme.accent : 0xFF409EFFu;
		const uint32_t grid_color = to_ovg(with_alpha(border, 0x55));

		clip_guard clip(cb, vg, m.body_x, m.body_y, m.body_width,
			m.body_height);
		ovg_tools::fill_rect(cb, vg, m.body_x, m.body_y, m.body_width,
			m.body_height, to_ovg(background));

		const table_row_range visible = visible_row_range();
		const float row_height = row_height_px();
		const float scale = ui_scale();
		const float first_top = m.body_y + (float)visible.first * row_height -
			scroll_y_;
		for (std::size_t i = 0; i < visible.count; ++i)
		{
			const std::size_t display = visible.first + i;
			const std::size_t row = source_row_at(display);
			const float top = first_top + (float)i * row_height;
			const bool selected_row = selected_.valid() &&
				selected_.row == row;
			const bool hovered_row = hover_row_ == row && hover_column_ >= 0;

			if (selected_row)
				ovg_tools::fill_rect(cb, vg, m.body_x, top, m.body_width,
					row_height, to_ovg(selection));
			else if (hovered_row)
				ovg_tools::fill_rect(cb, vg, m.body_x, top, m.body_width,
					row_height, to_ovg(with_alpha(row_hover, 0x66)));
			else if (display % 2 == 1)
				ovg_tools::fill_rect(cb, vg, m.body_x, top, m.body_width,
					row_height, to_ovg(with_alpha(border, 0x12)));

			if (selected_row && selected_.column >= 0 &&
				selected_.column < (int)columns_.size())
			{
				const float sx = m.body_x +
					column_offset((std::size_t)selected_.column) - scroll_x_;
				const float sw = column_width_px((std::size_t)selected_.column);
				ovg_tools::stroke_rect(cb, vg, sx, top, sw, row_height,
					to_ovg(accent), 1.0f);
			}

			float offset = -scroll_x_;
			for (std::size_t column_index = 0;
				column_index < columns_.size(); ++column_index)
			{
				const table_column& column = columns_[column_index];
				const float column_width = column_width_px(column_index);
				const float cell_x = m.body_x + offset;
				offset += column_width;
				if (cell_x + column_width <= m.body_x ||
					cell_x >= m.body_x + m.body_width)
					continue;

				std::string text;
				const bool is_edit_cell = editing_ &&
					edit_cell_.row == row &&
					edit_cell_.column == (int)column_index;
				if (is_edit_cell && edit_preview_active_ &&
					(column.editor.type == property_type::color ||
						column.editor.type == property_type::curve ||
						column.editor.type == property_type::gradient))
					text = edit_preview_value_;
				else
					text = cell_text(row, (int)column_index);
				++render_cell_queries_;

				vgui_color cell_text_color = selected_row
					? selection_text : row_text;
				vgui_color cell_bg = 0;
				if (source_.cell_style)
				{
					const table_cell_style style = source_.cell_style(row,
						column.id);
					if (style.background)
						cell_bg = style.background;
					if (style.text_color)
						cell_text_color = style.text_color;
				}
				if (cell_bg)
					ovg_tools::fill_rect(cb, vg, cell_x, top, column_width,
						row_height, to_ovg(cell_bg));

				const property_type type = column.editor.type;
				if (type == property_type::color)
				{
					vgui_color color = 0;
					if (parse_hex_color(text, color))
					{
						const float swatch = std::max(8.0f * scale,
							std::min(14.0f * scale, row_height - 8.0f * scale));
						const float sy = top + (row_height - swatch) * 0.5f;
						ovg_tools::fill_round_rect(cb, vg,
							cell_x + kCellPaddingX * scale, sy, swatch, swatch,
							2.0f * scale, to_ovg(color));
						ovg_tools::stroke_round_rect(cb, vg,
							cell_x + kCellPaddingX * scale, sy, swatch, swatch,
							2.0f * scale, to_ovg(with_alpha(border, 0xCC)), 1.0f);
						draw_cell_text(widget, cb, vg, text,
							cell_x + kCellPaddingX * scale + swatch + 4.0f * scale,
							top,
							column_width - swatch - 4.0f * scale, row_height,
							cell_text_color, column.alignment);
						continue;
					}
				}
				else if (type == property_type::curve)
				{
					curve_value value;
					if (curve_editor::parse_value(text, value))
					{
						curve_editor::draw_preview(cb, vg,
							cell_x + 2.0f * scale, top + 3.0f * scale,
							std::max(1.0f, column_width - 4.0f * scale),
							std::max(1.0f, row_height - 6.0f * scale), value,
							false, scale);
						continue;
					}
				}
				else if (type == property_type::gradient)
				{
					gradient_value value;
					if (gradient_editor::parse_value(text, value))
					{
						gradient_editor::draw_preview(cb, vg,
							cell_x + 2.0f * scale, top + 3.0f * scale,
							std::max(1.0f, column_width - 4.0f * scale),
							std::max(1.0f, row_height - 6.0f * scale), value,
							false, scale);
						continue;
					}
				}

				draw_cell_text(widget, cb, vg, text, cell_x, top,
					column_width, row_height, cell_text_color,
					column.alignment);
			}

			ovg_tools::line(cb, vg, m.body_x, top + row_height - 0.5f,
				m.body_x + m.body_width, top + row_height - 0.5f, grid_color,
				1.0f, VG_LINE_CAP_BUTT);
		}

		/* Column separators are kept faint so alternating rows remain the
		 * dominant rhythm. */
		float offset = -scroll_x_;
		for (std::size_t i = 0; i + 1 < columns_.size(); ++i)
		{
			offset += column_width_px(i);
			const float sx = m.body_x + offset;
			if (sx <= m.body_x || sx >= m.body_x + m.body_width)
				continue;
			ovg_tools::line(cb, vg, sx, m.body_y, sx,
				m.body_y + m.body_height, grid_color, 1.0f,
				VG_LINE_CAP_BUTT);
		}
	}

	void table_view::draw_scrollbars(ovg_ctx_cb* cb, rvg_t* vg,
		const layout_metrics& m)
	{
		vgui_theme theme = {};
		if (context_)
			vgui_context_get_theme(context_, &theme);
		const vgui_color track = theme.track ? theme.track : 0xFF15181Eu;
		const vgui_color thumb = theme.scrollbar ? theme.scrollbar
			: 0xFF4A5568u;
		const float scale = ui_scale();
		const float inset = kScrollbarInset * scale;
		const float radius = 3.0f * scale;
		const float min_thumb = kScrollbarMinThumb * scale;

		if (m.vertical_scrollbar && m.vbar_height > 0.0f)
		{
			ovg_tools::fill_round_rect(cb, vg, m.vbar_x + inset,
				m.vbar_y + inset,
				std::max(1.0f, m.vbar_width - inset * 2.0f),
				std::max(1.0f, m.vbar_height - inset * 2.0f),
				radius, to_ovg(track));
			const float range = max_scroll_y();
			if (range > 0.0f)
			{
				const float track_h = m.vbar_height - inset * 2.0f;
				const float ratio = std::clamp(m.body_height /
					std::max(1.0f, m.rows_height), 0.0f, 1.0f);
				const float thumb_h = std::max(min_thumb,
					track_h * ratio);
				const float travel = std::max(0.0f, track_h - thumb_h);
				const float thumb_y = m.vbar_y + inset +
					travel * std::clamp(scroll_y_ / range, 0.0f, 1.0f);
				ovg_tools::fill_round_rect(cb, vg,
					m.vbar_x + inset, thumb_y,
					std::max(1.0f, m.vbar_width - inset * 2.0f),
					std::min(thumb_h, track_h), radius, to_ovg(thumb));
			}
		}

		if (m.horizontal_scrollbar && m.hbar_width > 0.0f)
		{
			ovg_tools::fill_round_rect(cb, vg, m.hbar_x + inset,
				m.hbar_y + inset,
				std::max(1.0f, m.hbar_width - inset * 2.0f),
				std::max(1.0f, m.hbar_height - inset * 2.0f),
				radius, to_ovg(track));
			const float range = max_scroll_x();
			if (range > 0.0f)
			{
				const float track_w = m.hbar_width - inset * 2.0f;
				const float ratio = std::clamp(m.body_width /
					std::max(1.0f, m.columns_width), 0.0f, 1.0f);
				const float thumb_w = std::max(min_thumb,
					track_w * ratio);
				const float travel = std::max(0.0f, track_w - thumb_w);
				const float thumb_x = m.hbar_x + inset +
					travel * std::clamp(scroll_x_ / range, 0.0f, 1.0f);
				ovg_tools::fill_round_rect(cb, vg, thumb_x,
					m.hbar_y + inset,
					std::min(thumb_w, track_w),
					std::max(1.0f, m.hbar_height - inset * 2.0f),
					radius, to_ovg(thumb));
			}
		}
	}

	/* --------------------------------------------------------------------- */
	/* Input                                                                 */
	/* --------------------------------------------------------------------- */

	void table_view::request_focus()
	{
		if (context_ && widget_)
			vgui_set_focus(context_, widget_);
	}

	int table_view::on_event(vgui_widget* widget, const vgui_event* event,
		void* user)
	{
		auto* self = static_cast<table_view*>(user);
		if (!self || !event || !widget)
			return 0;

		const float scale = self->ui_scale();
		const float inset = kScrollbarInset * scale;
		const float min_thumb = kScrollbarMinThumb * scale;
		const float resize_grab = kResizeGrab * scale;
		const float x = event->x - kBorderInset * scale;
		const float y = event->y - kBorderInset * scale;
		layout_metrics m = self->metrics();
		const bool inside = x >= 0.0f && y >= 0.0f &&
			x < m.width && y < m.height;
		const float vbar_x = m.vbar_x - m.x;
		const float vbar_y = m.vbar_y - m.y;
		const float hbar_x = m.hbar_x - m.x;
		const float hbar_y = m.hbar_y - m.y;

		auto vbar_thumb = [&](float* thumb_y, float* thumb_h) {
			*thumb_y = vbar_y;
			*thumb_h = m.vbar_height;
			if (!m.vertical_scrollbar || m.vbar_height <= 0.0f)
				return false;
			const float range = self->max_scroll_y();
			if (range <= 0.0f)
				return false;
			const float track_h = m.vbar_height - inset * 2.0f;
			const float ratio = std::clamp(m.body_height /
				std::max(1.0f, m.rows_height), 0.0f, 1.0f);
			const float h = std::min(track_h, std::max(min_thumb,
				track_h * ratio));
			const float travel = std::max(0.0f, track_h - h);
			*thumb_y = vbar_y + inset +
				travel * std::clamp(self->scroll_y_ / range, 0.0f, 1.0f);
			*thumb_h = h;
			return true;
		};
		auto hbar_thumb = [&](float* thumb_x, float* thumb_w) {
			*thumb_x = hbar_x;
			*thumb_w = m.hbar_width;
			if (!m.horizontal_scrollbar || m.hbar_width <= 0.0f)
				return false;
			const float range = self->max_scroll_x();
			if (range <= 0.0f)
				return false;
			const float track_w = m.hbar_width - inset * 2.0f;
			const float ratio = std::clamp(m.body_width /
				std::max(1.0f, m.columns_width), 0.0f, 1.0f);
			const float w = std::min(track_w, std::max(min_thumb,
				track_w * ratio));
			const float travel = std::max(0.0f, track_w - w);
			*thumb_x = hbar_x + inset +
				travel * std::clamp(self->scroll_x_ / range, 0.0f, 1.0f);
			*thumb_w = w;
			return true;
		};

		switch (event->type)
		{
		case VGUI_EVENT_MOUSE_DOWN:
			if (!inside || event->button != 1)
				return 0;
			self->request_focus();
			if (m.vertical_scrollbar && x >= vbar_x &&
				x < vbar_x + m.vbar_width && y >= vbar_y &&
				y < vbar_y + m.vbar_height)
			{
				float thumb_y = 0.0f, thumb_h = 0.0f;
				if (!vbar_thumb(&thumb_y, &thumb_h))
					return 1;
				if (y >= thumb_y && y < thumb_y + thumb_h)
					self->drag_thumb_offset_ = y - thumb_y;
				else
					self->drag_thumb_offset_ = thumb_h * 0.5f;
				self->dragging_vbar_ = true;
				self->dragging_hbar_ = false;
				const float track_h = m.vbar_height - inset * 2.0f;
				const float travel = std::max(0.0f, track_h - thumb_h);
				if (travel > 0.0f)
				{
					const float pos = std::clamp(y - vbar_y -
						inset - self->drag_thumb_offset_,
						0.0f, travel);
					self->set_scroll(self->scroll_x_,
						pos / travel * self->max_scroll_y());
				}
				return 1;
			}
			if (m.horizontal_scrollbar && x >= hbar_x &&
				x < hbar_x + m.hbar_width && y >= hbar_y &&
				y < hbar_y + m.hbar_height)
			{
				float thumb_x = 0.0f, thumb_w = 0.0f;
				if (!hbar_thumb(&thumb_x, &thumb_w))
					return 1;
				if (x >= thumb_x && x < thumb_x + thumb_w)
					self->drag_thumb_offset_ = x - thumb_x;
				else
					self->drag_thumb_offset_ = thumb_w * 0.5f;
				self->dragging_hbar_ = true;
				self->dragging_vbar_ = false;
				const float track_w = m.hbar_width - inset * 2.0f;
				const float travel = std::max(0.0f, track_w - thumb_w);
				if (travel > 0.0f)
				{
					const float pos = std::clamp(x - hbar_x -
						inset - self->drag_thumb_offset_,
						0.0f, travel);
					self->set_scroll(pos / travel * self->max_scroll_x(),
						self->scroll_y_);
				}
				return 1;
			}
			if (y < m.header_height)
			{
				const int column = self->column_at(x + self->scroll_x_);
				if (column < 0)
					return 1;
				const float right = self->column_offset((std::size_t)column) +
					self->column_width_px((std::size_t)column);
				if (std::fabs((x + self->scroll_x_) - right) <= resize_grab)
				{
					self->resizing_ = true;
					self->resize_column_ = column;
					self->resize_start_x_ = x;
					self->resize_start_width_ =
						self->columns_[(std::size_t)column].width;
					self->header_press_column_ = -1;
				}
				else
				{
					self->header_press_column_ = column;
					self->header_press_x_ = x;
				}
				return 1;
			}
			if (y < m.header_height + m.body_height)
			{
				const table_cell cell = self->hit_test(x, y);
				if (cell.valid())
					self->select_cell(cell.row, cell.column);
				return 1;
			}
			return 0;

		case VGUI_EVENT_DOUBLE_CLICK:
			if (!inside)
				return 0;
			{
				const table_cell cell = self->hit_test(x, y);
				if (cell.valid())
				{
					self->select_cell(cell.row, cell.column);
					return self->begin_edit(cell.row, cell.column) ? 1 : 0;
				}
			}
			return 0;

		case VGUI_EVENT_MOUSE_MOVE:
			if (self->resizing_)
			{
				if (self->resize_column_ >= 0)
				{
					self->set_column_width((std::size_t)self->resize_column_,
						self->resize_start_width_ +
						to_logical(widget, x - self->resize_start_x_));
				}
				return 1;
			}
			if (self->dragging_vbar_)
			{
				float thumb_y = 0.0f, thumb_h = 0.0f;
				if (vbar_thumb(&thumb_y, &thumb_h))
				{
					const float track_h = m.vbar_height -
						inset * 2.0f;
					const float travel = std::max(0.0f, track_h - thumb_h);
					if (travel > 0.0f)
					{
						const float pos = std::clamp(y - vbar_y -
							inset - self->drag_thumb_offset_,
							0.0f, travel);
						self->set_scroll(self->scroll_x_,
							pos / travel * self->max_scroll_y());
					}
				}
				return 1;
			}
			if (self->dragging_hbar_)
			{
				float thumb_x = 0.0f, thumb_w = 0.0f;
				if (hbar_thumb(&thumb_x, &thumb_w))
				{
					const float track_w = m.hbar_width -
						inset * 2.0f;
					const float travel = std::max(0.0f, track_w - thumb_w);
					if (travel > 0.0f)
					{
						const float pos = std::clamp(x - hbar_x -
							inset - self->drag_thumb_offset_,
							0.0f, travel);
						self->set_scroll(pos / travel * self->max_scroll_x(),
							self->scroll_y_);
					}
				}
				return 1;
			}
			if (!inside)
			{
				self->hover_row_ = kNoRow;
				self->hover_column_ = -1;
				return 0;
			}
			if (y < m.header_height)
			{
				self->hover_row_ = kNoRow;
				self->hover_column_ = -1;
				const int column = self->column_at(x + self->scroll_x_);
				bool near_edge = false;
				if (column >= 0)
				{
					const float right = self->column_offset(
						(std::size_t)column) +
						self->column_width_px((std::size_t)column);
					near_edge = std::fabs((x + self->scroll_x_) - right) <=
						resize_grab;
				}
				vgui_widget_set_cursor(widget, near_edge
					? VGUI_CURSOR_SIZE_H : VGUI_CURSOR_ARROW);
				return 1;
			}
			{
				const table_cell cell = self->hit_test(x, y);
				self->hover_row_ = cell.valid() ? cell.row : kNoRow;
				self->hover_column_ = cell.valid() ? cell.column : -1;
				vgui_widget_set_cursor(widget, VGUI_CURSOR_ARROW);
			}
			return 1;

		case VGUI_EVENT_MOUSE_UP:
			if (event->button != 1)
				return 0;
			if (self->resizing_)
			{
				self->resizing_ = false;
				self->resize_column_ = -1;
				return 1;
			}
			if (self->dragging_vbar_ || self->dragging_hbar_)
			{
				self->dragging_vbar_ = false;
				self->dragging_hbar_ = false;
				return 1;
			}
			if (self->header_press_column_ >= 0)
			{
				const int column = self->header_press_column_;
				self->header_press_column_ = -1;
				if (column >= 0 && column < (int)self->columns_.size() &&
					self->columns_[(std::size_t)column].sortable)
				{
					const bool ascending = self->sort_column_ == column
						? !self->sort_ascending_ : true;
					self->set_sort(column, ascending);
				}
				return 1;
			}
			return 0;

		case VGUI_EVENT_MOUSE_WHEEL:
			if (!inside || (event->wheel_x == 0.0f &&
				event->wheel_y == 0.0f))
				return 0;
			if ((event->mods & VGUI_MOD_SHIFT) != 0 ||
				std::fabs(event->wheel_x) > std::fabs(event->wheel_y))
			{
				const float delta = event->wheel_x != 0.0f
					? event->wheel_x : event->wheel_y;
				self->set_scroll(self->scroll_x_ - delta * 48.0f * scale,
					self->scroll_y_);
			}
			else
			{
				self->set_scroll(self->scroll_x_,
					self->scroll_y_ - event->wheel_y *
					self->row_height_px() * 3.0f);
			}
			return 1;

		case VGUI_EVENT_KEY_DOWN:
		{
			if (!self->editing_ && event->key == VGUI_KEY_ESCAPE)
			{
				self->clear_selection();
				return 1;
			}
			if (event->key == VGUI_KEY_ENTER || event->key == VGUI_KEY_F2)
			{
				if (self->begin_edit())
					return 1;
				return 0;
			}
			if (self->columns_.empty())
				return 0;
			table_cell current = self->selected_;
			int display = current.valid()
				? self->display_index_of(current.row) : 0;
			if (display < 0)
				display = 0;
			int column = current.valid() ? current.column : 0;
			if (column < 0 || column >= (int)self->columns_.size())
				column = 0;
			const std::size_t page = std::max<std::size_t>(1,
				(std::size_t)std::max(1.0f,
					(self->metrics().body_height / self->row_height_px())));
			switch (event->key)
			{
			case VGUI_KEY_UP:
				display = std::max(0, display - 1);
				break;
			case VGUI_KEY_DOWN:
				display = std::min((int)self->row_count_ - 1, display + 1);
				break;
			case VGUI_KEY_LEFT:
				column = std::max(0, column - 1);
				break;
			case VGUI_KEY_RIGHT:
				column = std::min((int)self->columns_.size() - 1,
					column + 1);
				break;
			case VGUI_KEY_HOME:
				display = (event->mods & VGUI_MOD_CTRL) != 0 ? 0 : display;
				column = 0;
				break;
			case VGUI_KEY_END:
				display = (event->mods & VGUI_MOD_CTRL) != 0
					? (int)self->row_count_ - 1 : display;
				column = (int)self->columns_.size() - 1;
				break;
			case VGUI_KEY_PAGE_UP:
				display = std::max(0, display - (int)page);
				break;
			case VGUI_KEY_PAGE_DOWN:
				display = std::min((int)self->row_count_ - 1,
					display + (int)page);
				break;
			default:
				return 0;
			}
			if (self->row_count_ == 0)
				return 1;
			const std::size_t row = self->source_row_at((std::size_t)display);
			if (self->select_cell(row, column))
				self->scroll_to_row(row);
			return 1;
		}

		case VGUI_EVENT_LEAVE:
			self->hover_row_ = kNoRow;
			self->hover_column_ = -1;
			vgui_widget_set_cursor(widget, VGUI_CURSOR_ARROW);
			return 0;

		case VGUI_EVENT_CANCEL:
			if (self->resizing_ || self->dragging_vbar_ ||
				self->dragging_hbar_ || self->header_press_column_ >= 0)
			{
				self->resizing_ = false;
				self->resize_column_ = -1;
				self->dragging_vbar_ = false;
				self->dragging_hbar_ = false;
				self->header_press_column_ = -1;
				return 1;
			}
			return 0;

		default:
			return 0;
		}
	}

} /* namespace vgui_editor_kit */
