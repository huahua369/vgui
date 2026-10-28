#include "property_grid.h"

#include "drag_bridge.h"
#include "editor_context.h"
#include "ui_scale.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace vgui_editor_kit {

	namespace {
		constexpr vgui_color kPanelBg = 0xFF1B1E24;
		constexpr vgui_color kBorder = 0xFF2E3440;
		constexpr vgui_color kLabelText = 0xFF9FB0C4;
		constexpr vgui_color kErrorText = 0xFFE06C75;
		constexpr vgui_color kCategoryBg = 0xFF232831;
		constexpr float kRowHeight = 28.0f;
		constexpr float kErrorHeight = 16.0f;
		constexpr float kLabelWidth = 112.0f;
		constexpr float kResetWidth = 24.0f;
		constexpr float kCategoryHeight = 24.0f;
		constexpr float kItemGap = 4.0f;
		constexpr float kPanelPadding = 6.0f;
		constexpr float kScrollbarThickness = 10.0f;	// horizontal bar height
		constexpr float kScrollbarThicknessV = 5.0f;	// vertical bar width
		constexpr float kScrollbarInset = 2.0f;
		constexpr float kScrollbarGap = 4.0f;
		/* Bars are painted over the panel's padding box, so the right and bottom
		 * padding reserve their bands; otherwise a bar covers the row edge. */
		constexpr float kPanelPaddingRight =
			kScrollbarGap + kScrollbarThicknessV + kScrollbarInset;
		constexpr float kPanelPaddingBottom =
			kScrollbarGap + kScrollbarThickness + kScrollbarInset;
		constexpr float kPanelPaddingV = kPanelPadding + kPanelPaddingBottom;
		constexpr float kMinContentWidth = 280.0f;	// horizontal scroll floor
		constexpr float kSearchLead = 28.0f;	// search 24 + gap 4
		constexpr float kOverscan = 96.0f;
		constexpr std::size_t kMinWindowEntries = 4;

		using normalized_value = property_value_result;

		/* Undo record for one property row.  Consecutive commits on the same row
		 * merge into a single record so drags and repeated edits undo at once. */
		class property_command final : public command
		{
		public:
			using apply_fn = std::function<void(std::uint64_t, const std::string&)>;

			property_command(std::string name, std::uint64_t token,
				std::string old_value, std::string new_value, apply_fn apply)
				: name_(std::move(name)), token_(token),
				old_value_(std::move(old_value)), new_value_(std::move(new_value)),
				apply_(std::move(apply))
			{
			}

			const char* name() const noexcept override
			{
				return name_.c_str();
			}

			bool execute() override
			{
				if (apply_)
					apply_(token_, new_value_);
				return true;
			}

			void undo() override
			{
				if (apply_)
					apply_(token_, old_value_);
			}

			bool merge_with(command& next) override
			{
				auto* other = dynamic_cast<property_command*>(&next);
				if (!other || other->token_ != token_)
					return false;
				/* Only merge an unbroken chain: next must replace exactly the
				 * value this record last applied.  A display-only update in
				 * between leaves the histories disconnected. */
				if (other->old_value_ != new_value_)
					return false;
				new_value_ = std::move(other->new_value_);
				return true;
			}

		private:
			std::string name_;
			std::uint64_t token_;
			std::string old_value_;
			std::string new_value_;
			apply_fn apply_;
		};

		/* P08:多目标批量的单对象 undo 记录。push 前效果已应用,undo/redo
		 * 经 apply 回调写回该对象的旧/新值并刷新行显示。 */
		class multi_command final : public command
		{
		public:
			using apply_fn = std::function<void(object_id, const std::string&)>;
			multi_command(std::string name, std::string id, object_id target,
				std::string old_value, std::string new_value, apply_fn apply)
				: name_(std::move(name)), id_(std::move(id)), target_(target),
				old_value_(std::move(old_value)), new_value_(std::move(new_value)),
				apply_(std::move(apply))
			{
			}

			const char* name() const noexcept override
			{
				return name_.c_str();
			}

			bool execute() override
			{
				if (apply_)
					apply_(target_, new_value_);
				return true;
			}

			void undo() override
			{
				if (apply_)
					apply_(target_, old_value_);
			}

		private:
			std::string name_;
			std::string id_;
			object_id target_;
			std::string old_value_;
			std::string new_value_;
			apply_fn apply_;
		};

		/* P08:进程内属性剪贴板(kit 级,跨 grid 实例共享)。 */
		struct clipboard_entry
		{
			property_type type = property_type::text;
			std::string value;
		};

		std::map<std::string, clipboard_entry>& property_clipboard()
		{
			static std::map<std::string, clipboard_entry> clipboard;
			return clipboard;
		}

		std::string trim_copy(std::string text)
		{
			auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
			while (!text.empty() && is_space((unsigned char)text.front()))
				text.erase(text.begin());
			while (!text.empty() && is_space((unsigned char)text.back()))
				text.pop_back();
			return text;
		}

		std::string lower_copy(std::string text)
		{
			for (char& c : text)
			{
				if (c >= 'A' && c <= 'Z')
					c = (char)(c - 'A' + 'a');
			}
			return text;
		}

		bool parse_integer_text(const std::string& text, long long& out)
		{
			const std::string trimmed = trim_copy(text);
			if (trimmed.empty())
				return false;
			errno = 0;
			char* end = nullptr;
			const long long value = std::strtoll(trimmed.c_str(), &end, 10);
			if (errno == ERANGE || end == trimmed.c_str() || *end != '\0')
				return false;
			out = value;
			return true;
		}

		bool parse_number_text(const std::string& text, double& out)
		{
			const std::string trimmed = trim_copy(text);
			if (trimmed.empty())
				return false;
			errno = 0;
			char* end = nullptr;
			const double value = std::strtod(trimmed.c_str(), &end);
			if (errno == ERANGE || end == trimmed.c_str() || *end != '\0' ||
				!std::isfinite(value))
				return false;
			out = value;
			return true;
		}

		std::string format_number(double value)
		{
			if (value == 0.0)
				value = 0.0;
			char buffer[64];
			const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value,
				std::chars_format::general, 6);
			if (result.ec != std::errc())
				return std::to_string(value);
			return std::string(buffer, result.ptr);
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

		bool parse_color(std::string text, vgui_color& out, std::string& canonical)
		{
			std::string digits = trim_copy(std::move(text));
			bool argb_prefix = false;
			if (digits.size() >= 2 && digits[0] == '0' &&
				(digits[1] == 'x' || digits[1] == 'X'))
			{
				argb_prefix = true;
				digits.erase(0, 2);
			}
			else if (!digits.empty() && digits[0] == '#')
			{
				digits.erase(0, 1);
			}

			for (char c : digits)
			{
				if (hex_digit(c) < 0)
					return false;
			}

			auto nibble = [&digits](std::size_t index) {
				return (unsigned int)hex_digit(digits[index]);
			};
			auto byte = [&digits](std::size_t index) {
				return (unsigned int)(hex_digit(digits[index]) * 16 +
					hex_digit(digits[index + 1]));
			};

			unsigned int r = 0, g = 0, b = 0, a = 255;
			switch (digits.size())
			{
			case 3:
				r = nibble(0) * 17;
				g = nibble(1) * 17;
				b = nibble(2) * 17;
				break;
			case 4:
				r = nibble(0) * 17;
				g = nibble(1) * 17;
				b = nibble(2) * 17;
				a = nibble(3) * 17;
				break;
			case 6:
				r = byte(0);
				g = byte(2);
				b = byte(4);
				break;
			case 8:
				if (argb_prefix)
				{
					a = byte(0);
					r = byte(2);
					g = byte(4);
					b = byte(6);
				}
				else
				{
					r = byte(0);
					g = byte(2);
					b = byte(4);
					a = byte(6);
				}
				break;
			default:
				return false;
			}

			out = ((vgui_color)a << 24) | ((vgui_color)r << 16) |
				((vgui_color)g << 8) | (vgui_color)b;
			char buffer[16];
			if (a == 255)
				std::snprintf(buffer, sizeof(buffer), "#%02X%02X%02X", r, g, b);
			else
				std::snprintf(buffer, sizeof(buffer), "#%02X%02X%02X%02X", r, g, b, a);
			canonical = buffer;
			return true;
		}

		std::string choice_value_at(const property_desc& desc, std::size_t index)
		{
			if (index >= desc.choices.size())
				return std::string();
			return desc.choice_values.empty() ? desc.choices[index] : desc.choice_values[index];
		}

		std::string choice_label_at(const property_desc& desc, std::size_t index)
		{
			return index < desc.choices.size() ? desc.choices[index] : std::string();
		}

		std::string empty_choice_label(const property_desc& desc)
		{
			return desc.placeholder.empty() ? std::string("(None)")
				: desc.placeholder;
		}

		int vector_dimension(property_type type)
		{
			switch (type)
			{
			case property_type::vector2: return 2;
			case property_type::vector3: return 3;
			case property_type::vector4: return 4;
			default: return 0;
			}
		}

		bool is_vector_type(property_type type)
		{
			return vector_dimension(type) != 0;
		}

		int choice_index_for_text(const property_desc& desc, const std::string& text)
		{
			const std::string trimmed = trim_copy(text);
			for (std::size_t i = 0; i < desc.choices.size(); ++i)
			{
				if (choice_value_at(desc, i) == trimmed || choice_label_at(desc, i) == trimmed)
					return (int)i;
			}
			const std::string lowered = lower_copy(trimmed);
			for (std::size_t i = 0; i < desc.choices.size(); ++i)
			{
				if (lower_copy(choice_value_at(desc, i)) == lowered ||
					lower_copy(choice_label_at(desc, i)) == lowered)
					return (int)i;
			}
			return -1;
		}

		normalized_value normalize_value(const property_desc& desc, std::string text)
		{
			normalized_value result;
			switch (desc.type)
			{
			case property_type::text:
				result.value = std::move(text);
				result.ok = true;
				break;
			case property_type::integer:
			{
				long long value = 0;
				if (!parse_integer_text(text, value))
				{
					result.error = "expected an integer";
					break;
				}
				double number = (double)value;
				if (desc.has_range)
				{
					const double low = std::min(desc.min_value, desc.max_value);
					const double high = std::max(desc.min_value, desc.max_value);
					number = std::clamp(number, low, high);
				}
				result.value = std::to_string((long long)std::llround(number));
				result.ok = true;
				break;
			}
			case property_type::number:
			{
				double value = 0.0;
				if (!parse_number_text(text, value))
				{
					result.error = "expected a number";
					break;
				}
				if (desc.has_range)
				{
					const double low = std::min(desc.min_value, desc.max_value);
					const double high = std::max(desc.min_value, desc.max_value);
					value = std::clamp(value, low, high);
				}
				result.value = format_number(value);
				result.ok = true;
				break;
			}
			case property_type::boolean:
			{
				const std::string lowered = lower_copy(trim_copy(text));
				if (lowered == "true" || lowered == "1" || lowered == "yes" ||
					lowered == "on")
				{
					result.value = "true";
					result.ok = true;
				}
				else if (lowered == "false" || lowered == "0" || lowered == "no" ||
					lowered == "off")
				{
					result.value = "false";
					result.ok = true;
				}
				else
				{
					result.error = "expected true or false";
				}
				break;
			}
			case property_type::enumeration:
			{
				const std::string trimmed = trim_copy(text);
				if (desc.allow_empty && trimmed.empty())
				{
					result.value.clear();
					result.ok = true;
					break;
				}
				const int index = choice_index_for_text(desc, text);
				if (index < 0)
				{
					if (desc.allow_empty &&
						trimmed == empty_choice_label(desc))
					{
						result.value.clear();
						result.ok = true;
						break;
					}
					result.error = "unknown choice";
					break;
				}
				result.value = choice_value_at(desc, (std::size_t)index);
				result.ok = true;
				break;
			}
			case property_type::color:
			{
				vgui_color color = 0;
				std::string canonical;
				if (!parse_color(std::move(text), color, canonical))
				{
					result.error = "expected #RRGGBB or #RRGGBBAA";
					break;
				}
				result.value = std::move(canonical);
				result.ok = true;
				break;
			}
			case property_type::asset:
			case property_type::object_ref:
				/* 引用值是宿主 id 文本;空 = 未设置。 */
				result.value = trim_copy(std::move(text));
				result.ok = true;
				break;
			case property_type::curve:
			{
				curve_value curve;
				std::string error;
				if (!curve_editor::parse_value(text, curve, &error))
				{
					result.error = error.empty() ? "expected a curve" : error;
					break;
				}
				result.value = curve_editor::format_value(curve);
				result.ok = true;
				break;
			}
			case property_type::gradient:
			{
				gradient_value gradient;
				std::string error;
				if (!gradient_editor::parse_value(text, gradient, &error))
				{
					result.error = error.empty() ? "expected a gradient"
						: error;
					break;
				}
				result.value = gradient_editor::format_value(gradient);
				result.ok = true;
				break;
			}
			case property_type::vector2:
			case property_type::vector3:
			case property_type::vector4:
			{
				const int dimension = vector_dimension(desc.type);
				vector_editor::vector_value values{};
				std::string error;
				if (!vector_editor::parse_value(text, dimension, values,
					&error))
				{
					result.error = error.empty()
						? "expected a vector" : error;
					break;
				}
				if (desc.has_range)
				{
					const double low = std::min(desc.min_value, desc.max_value);
					const double high = std::max(desc.min_value, desc.max_value);
					for (int i = 0; i < dimension; ++i)
						values[(std::size_t)i] = std::clamp(
							values[(std::size_t)i], low, high);
				}
				result.value = vector_editor::format_value(values, dimension);
				result.ok = true;
				break;
			}
			}
			return result;
		}

		std::string property_widget_id(const std::string& id)
		{
			return "property:" + id;
		}

		std::string category_widget_id(const std::string& name)
		{
			return "category:" + name;
		}

		std::string category_header_text(const std::string& name, bool collapsed)
		{
			return (collapsed ? ">  " : "v  ") + name;
		}
	}

	property_value_result property_normalize_value(const property_desc& desc,
		std::string text)
	{
		return normalize_value(desc, std::move(text));
	}

	property_grid::~property_grid()
	{
		if (popup_session_active_ && panel_)
		{
			popup_session_active_ = false;
			vgui_popup_close(vgui_widget_get_context(panel_));
		}
		unbind_selection();
		for (row& item : rows_)
			release_row(item);
		for (auto& entry : categories_)
			release_category(entry.second);
		categories_.clear();
		if (panel_)
		{
			vgui_panel_set_scroll_listener(panel_, nullptr, nullptr);
			vgui_widget_destroy(panel_);
			panel_ = nullptr;
			search_ = nullptr;
			content_ = nullptr;
			top_spacer_ = nullptr;
			window_ = nullptr;
			bottom_spacer_ = nullptr;
		}
		rows_.clear();
		display_.clear();
		category_order_.clear();
	}

	void property_grid::build(vgui_widget* parent)
	{
		if (panel_ || !parent)
			return;

		panel_ = vgui_widget_add(parent, VGUI_PANEL, nullptr);
		vgui_widget_set_a11y_name(panel_, "Properties");
		vgui_widget_set_grow(panel_, 1.0f);
		/* GROW only takes the main axis in a column parent.  The grid used to
		 * size itself from its widest row (vector rows) and overflow the dock
		 * page, clipping the Y/Z fields.  Stretch across the cross axis so rows
		 * lay out against the real panel width. */
		vgui_widget_set_align_self(panel_, VGUI_ALIGN_STRETCH);
		vgui_widget_set_layout(panel_, VGUI_LAYOUT_COLUMN);
		vgui_widget_set_padding(panel_, kPanelPadding, kPanelPadding,
			kPanelPaddingRight, kPanelPaddingBottom);
		vgui_widget_set_gap(panel_, kItemGap);
		vgui_widget_set_background(panel_, kPanelBg);
		vgui_widget_set_border(panel_, kBorder, 1);
		/* Narrow hosts keep rows usable: below this width the panel scrolls
		 * horizontally instead of squeezing the editor fields to nothing. */
		vgui_panel_set_scrollable(panel_, 1, 1);
		/* The stock horizontal bar is hard to grab with a pointer, so the grid
		 * uses a thicker one; both bar bands are reserved by the padding above. */
		vgui_panel_set_scrollbar_size(panel_, kScrollbarThickness,
			kScrollbarThicknessV, kScrollbarInset);

		/* 搜索行 = 搜索框 + P08 收藏过滤开关(高度保持 24,kSearchLead
		 * 不变)。 */
		vgui_widget* search_row = vgui_widget_add(panel_, VGUI_PANEL, nullptr);
		vgui_widget_set_size(search_row, 0, 24);
		vgui_widget_set_align_self(search_row, VGUI_ALIGN_STRETCH);
		vgui_widget_set_layout(search_row, VGUI_LAYOUT_ROW);
		vgui_widget_set_gap(search_row, 4);
		vgui_widget_set_padding(search_row, 0, 0, 0, 0);

		search_ = vgui_widget_add(search_row, VGUI_EDIT, nullptr);
		vgui_widget_set_grow(search_, 1.0f);
		vgui_widget_set_size(search_, 0, 24);
		vgui_widget_set_font_size(search_, 12);
		vgui_widget_set_id(search_, "__property_search");
		vgui_widget_set_a11y_name(search_, "Search properties");
		vgui_edit_set_placeholder(search_, "Search properties");
		vgui_edit_set_background_transparent(search_, 1);
		vgui_widget_on_text(search_, &property_grid::on_search_text, this);

		fav_filter_ = vgui_widget_add(search_row, VGUI_BUTTON, "*");
		vgui_widget_set_size(fav_filter_, 24, 24);
		vgui_widget_set_font_size(fav_filter_, 12);
		vgui_widget_set_id(fav_filter_, "__property_fav_filter");
		vgui_widget_set_a11y_name(fav_filter_, "Favorites only");
		vgui_widget_set_a11y_role(fav_filter_, VGUI_A11Y_ROLE_TOGGLE);
		vgui_widget_set_tooltip(fav_filter_, "Favorites only");
		vgui_widget_on_event(fav_filter_,
			&property_grid::on_fav_filter_click, this);

		/* content_ is a fixed-height column of three children:
		 *   top spacer -> materialized window -> bottom spacer
		 * The spacers stand in for rows that are outside the viewport, so the
		 * scroll range stays exact without building every row. */
		content_ = vgui_widget_add(panel_, VGUI_PANEL, nullptr);
		vgui_widget_set_align_self(content_, VGUI_ALIGN_STRETCH);
		vgui_widget_set_min_size(content_, kMinContentWidth, 0);
		vgui_widget_set_layout(content_, VGUI_LAYOUT_COLUMN);
		vgui_widget_set_gap(content_, 0);
		vgui_widget_set_padding(content_, 0, 0, 0, 0);

		top_spacer_ = vgui_widget_add(content_, VGUI_PANEL, nullptr);
		vgui_widget_set_align_self(top_spacer_, VGUI_ALIGN_STRETCH);
		vgui_widget_set_size(top_spacer_, 0, 0);
		vgui_widget_set_visible(top_spacer_, 0);

		window_ = vgui_widget_add(content_, VGUI_PANEL, nullptr);
		vgui_widget_set_align_self(window_, VGUI_ALIGN_STRETCH);
		vgui_widget_set_layout(window_, VGUI_LAYOUT_COLUMN);
		vgui_widget_set_gap(window_, kItemGap);
		vgui_widget_set_padding(window_, 0, 0, 0, 0);
		vgui_widget_set_size(window_, 0, 0);
		vgui_widget_set_visible(window_, 0);

		bottom_spacer_ = vgui_widget_add(content_, VGUI_PANEL, nullptr);
		vgui_widget_set_align_self(bottom_spacer_, VGUI_ALIGN_STRETCH);
		vgui_widget_set_size(bottom_spacer_, 0, 0);
		vgui_widget_set_visible(bottom_spacer_, 0);

		vgui_panel_set_scroll_listener(panel_, &property_grid::on_scroll, this);

		/* Before the first layout we only know the context size.  Use it as a
		 * generous viewport estimate so a freshly built grid is populated even
		 * when the host never scrolls it. */
		vgui_context* ctx = vgui_widget_get_context(panel_);
		int cw = 0, ch = 0;
		if (ctx)
			vgui_context_get_size(ctx, &cw, &ch);
		(void)cw;
		viewport_h_ = ch > 0
			? to_logical(panel_, (float)ch) - kPanelPaddingV : 0.0f;
		if (viewport_h_ < 200.0f)
			viewport_h_ = 200.0f;

		update_visibility();
	}

	void property_grid::clear()
	{
		if (popup_session_active_)
		{
			popup_session_active_ = false;
			if (panel_)
				vgui_popup_close(vgui_widget_get_context(panel_));
			reset_popup_session();
		}

		for (row& item : rows_)
			release_row(item);
		for (auto& entry : categories_)
			release_category(entry.second);
		categories_.clear();
		rows_.clear();
		display_.clear();
		category_order_.clear();
		visible_rows_ = 0;
		window_begin_ = 0;
		window_end_ = 0;
		content_height_ = 0.0f;
		scroll_y_ = 0.0f;

		/* Released rows and headers detach themselves, but keep this guard so
		 * clear() also recovers from any widget that was left attached. */
		if (window_)
		{
			std::vector<vgui_widget*> current;
			const std::size_t count = vgui_widget_get_child_count(window_);
			current.reserve(count);
			for (std::size_t i = 0; i < count; ++i)
				current.push_back(vgui_widget_get_child(window_, i));
			for (vgui_widget* child : current)
				vgui_widget_remove(child);
			vgui_widget_set_size(window_, 0, 0);
			vgui_widget_set_visible(window_, 0);
		}
		if (top_spacer_)
		{
			vgui_widget_set_size(top_spacer_, 0, 0);
			vgui_widget_set_visible(top_spacer_, 0);
		}
		if (bottom_spacer_)
		{
			vgui_widget_set_size(bottom_spacer_, 0, 0);
			vgui_widget_set_visible(bottom_spacer_, 0);
		}
		if (content_)
		{
			vgui_widget_set_size(content_, 0, 0);
			vgui_widget_set_min_size(content_, kMinContentWidth, 0);
		}
		if (panel_)
			vgui_panel_set_scroll(panel_, 0.0f, 0.0f);
	}

	void property_grid::add_row(std::string label, std::string value)
	{
		property_desc desc;
		desc.id = "row." + std::to_string(next_row_id_++);
		while (find_row(desc.id))
			desc.id = "row." + std::to_string(next_row_id_++);
		desc.label = std::move(label);
		desc.type = property_type::text;
		desc.value = std::move(value);
		add_property(std::move(desc));
	}

	bool property_grid::prepare_desc(property_desc& desc)
	{
		if (desc.id.empty())
		{
			desc.id = "property." + std::to_string(next_row_id_++);
			while (find_row(desc.id))
				desc.id = "property." + std::to_string(next_row_id_++);
		}
		if (desc.label.empty())
			desc.label = desc.id;
		if (desc.category.empty())
			desc.category = "General";
		if (desc.type == property_type::enumeration)
		{
			if (desc.choices.empty())
				return false;
			if (!desc.choice_values.empty() &&
				desc.choice_values.size() != desc.choices.size())
				return false;
			if (!desc.choice_enabled.empty() &&
				desc.choice_enabled.size() != desc.choices.size())
				return false;
		}
		if (desc.has_range && desc.min_value > desc.max_value)
			std::swap(desc.min_value, desc.max_value);
		if (desc.step < 0.0)
			desc.step = 0.0;

		normalized_value value = normalize_value(desc, desc.value);
		if (!value.ok)
			return false;
		desc.value = value.value;

		if (desc.has_default)
		{
			normalized_value default_value = normalize_value(desc, desc.default_value);
			if (!default_value.ok)
				return false;
			desc.default_value = default_value.value;
		}
		return true;
	}

	bool property_grid::add_property(property_desc desc)
	{
		if (!prepare_desc(desc) || find_row(desc.id))
			return false;

		row item;
		item.token = next_token_++;
		item.desc = std::move(desc);
		item.value = item.desc.value;
		const std::string category = item.desc.category;
		rows_.push_back(std::move(item));
		if (std::find(category_order_.begin(), category_order_.end(), category) ==
			category_order_.end())
			category_order_.push_back(category);
		update_visibility();
		return true;
	}

	void property_grid::refresh_row_desc(row& item)
	{
		if (item.label)
			vgui_widget_set_text(item.label, item.desc.label.c_str());

		if (item.vector_ui)
		{
			/* E02: vector metadata is normally fixed per property id, but a
			 * host may switch between objects with different presets.  Apply
			 * it without rebuilding the row. */
			item.vector_ui->set_semantic(item.desc.semantic);
			if (!item.desc.component_labels.empty())
				item.vector_ui->set_axis_labels(item.desc.component_labels);
			if (!item.desc.component_colors.empty())
				item.vector_ui->set_axis_colors(item.desc.component_colors);
			if (item.desc.has_range)
				item.vector_ui->set_range(item.desc.min_value,
					item.desc.max_value);
			else
				item.vector_ui->clear_range();
			item.vector_ui->set_step(item.desc.step);
			item.vector_ui->set_read_only(read_only_ || item.desc.read_only);
			item.vector_ui->set_enabled(!(read_only_ || item.desc.read_only));
		}
	}

	std::size_t property_grid::set_properties(
		std::vector<property_desc> properties)
	{
		if (popup_session_active_)
		{
			popup_session_active_ = false;
			if (panel_)
				vgui_popup_close(vgui_widget_get_context(panel_));
			reset_popup_session();
		}

		std::vector<property_desc> incoming;
		std::unordered_set<std::string> incoming_ids;
		incoming.reserve(properties.size());
		incoming_ids.reserve(properties.size());
		for (property_desc& desc : properties)
		{
			if (!prepare_desc(desc))
				continue;
			if (incoming_ids.insert(desc.id).second)
				incoming.push_back(std::move(desc));
		}

		/* E02: id lookup is expected O(1) so switching a large inspected
		 * object does not become quadratic in the property count. */
		std::unordered_map<std::string, std::size_t> row_index;
		row_index.reserve(rows_.size());
		for (std::size_t i = 0; i < rows_.size(); ++i)
			row_index.emplace(rows_[i].desc.id, i);

		std::vector<char> reused(rows_.size(), 0);
		std::vector<row> next;
		next.reserve(incoming.size());
		std::size_t changed = 0;

		for (property_desc& desc : incoming)
		{
			std::size_t match = rows_.size();
			const auto found = row_index.find(desc.id);
			if (found != row_index.end())
				match = found->second;

			if (match < rows_.size() && rows_[match].desc.type == desc.type)
			{
				reused[match] = 1;
				row item = std::move(rows_[match]);
				/* Enumeration menus are built lazily and cached with their
				 * choice ids.  Rebuild the cache when the incoming descriptor
				 * changes the menu's contents. */
				const property_desc& old = item.desc;
				const bool menu_stale = item.menu &&
					old.type == property_type::enumeration &&
					(old.choices != desc.choices ||
						old.choice_values != desc.choice_values ||
						old.choice_enabled != desc.choice_enabled ||
						old.allow_empty != desc.allow_empty ||
						old.placeholder != desc.placeholder);
				if (menu_stale)
				{
					vgui_widget_destroy(item.menu);
					item.menu = nullptr;
					item.menu_values.clear();
				}
				item.desc = std::move(desc);
				item.value = item.desc.value;
				item.mixed = false;
				refresh_row_desc(item);
				sync_row_from_targets(item);
				update_row_ui(item, true);
				next.push_back(std::move(item));
				continue;
			}

			if (match < rows_.size())
			{
				/* Same id, different widget kind: only this row is rebuilt.
				 * The replacement below accounts for exactly one changed row. */
				reused[match] = 1;
				release_row(rows_[match]);
			}

			row item;
			item.token = next_token_++;
			item.desc = std::move(desc);
			item.value = item.desc.value;
			sync_row_from_targets(item);
			next.push_back(std::move(item));
			++changed;
		}

		for (std::size_t i = 0; i < rows_.size(); ++i)
		{
			if (reused[i])
				continue;
			release_row(rows_[i]);
			++changed;
		}

		rows_ = std::move(next);
		category_order_.clear();
		for (const row& item : rows_)
		{
			if (std::find(category_order_.begin(), category_order_.end(),
				item.desc.category) == category_order_.end())
				category_order_.push_back(item.desc.category);
		}
		update_visibility();
		return changed;
	}

	bool property_grid::remove_property(const std::string& id)
	{
		for (auto it = rows_.begin(); it != rows_.end(); ++it)
		{
			if (it->desc.id != id)
				continue;
			release_row(*it);
			rows_.erase(it);
			update_visibility();
			return true;
		}
		return false;
	}

	bool property_grid::has_property(const std::string& id) const
	{
		return find_row(id) != nullptr;
	}

	bool property_grid::set_row_value(std::size_t index, std::string value)
	{
		if (index >= rows_.size())
			return false;
		row& item = rows_[index];
		reset_drag_state(item);
		item.value = std::move(value);
		update_row_ui(item, true);
		update_visibility();
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

	std::string property_grid::property_value(const std::string& id) const
	{
		const row* item = find_row(id);
		return item ? item->value : std::string();
	}

	bool property_grid::set_property_value(const std::string& id, std::string value)
	{
		row* item = find_row(id);
		if (!item)
			return false;
		normalized_value normalized = normalize_value(item->desc, std::move(value));
		if (!normalized.ok)
		{
			set_row_error(*item, normalized.error);
			return false;
		}
		clear_row_error(*item);
		reset_drag_state(*item);
		item->value = normalized.value;
		update_row_ui(*item, true);
		update_visibility();
		return true;
	}

	bool property_grid::commit_property_value(const std::string& id, std::string value)
	{
		row* item = find_row(id);
		if (!item)
			return false;
		if (read_only_ || item->desc.read_only)
		{
			set_row_error(*item, "property is read-only");
			return false;
		}
		const std::string label = item->desc.label.empty() ? item->desc.id : item->desc.label;
		return commit_value(*item, std::move(value), "Set " + label);
	}

	bool property_grid::reset_property(const std::string& id)
	{
		row* item = find_row(id);
		if (!item)
			return false;
		if (!item->desc.has_default)
		{
			set_row_error(*item, "no default value");
			return false;
		}
		if (read_only_ || item->desc.read_only)
		{
			set_row_error(*item, "property is read-only");
			return false;
		}
		const std::string label = item->desc.label.empty() ? item->desc.id : item->desc.label;
		const std::string value = item->desc.default_value;
		return commit_value(*item, value, "Reset " + label);
	}

	void property_grid::reset_all()
	{
		std::vector<std::string> ids;
		ids.reserve(rows_.size());
		for (const row& item : rows_)
			ids.push_back(item.desc.id);
		begin_batch("Reset all properties");
		for (const std::string& id : ids)
			reset_property(id);
		end_batch();
	}

	std::string property_grid::property_error(const std::string& id) const
	{
		const row* item = find_row(id);
		return item ? item->error : std::string();
	}

	bool property_grid::set_property_error(const std::string& id, std::string error)
	{
		row* item = find_row(id);
		if (!item)
			return false;
		set_row_error(*item, std::move(error));
		return true;
	}

	bool property_grid::property_read_only(const std::string& id) const
	{
		const row* item = find_row(id);
		return item && (read_only_ || item->desc.read_only);
	}

	bool property_grid::set_property_read_only(const std::string& id, bool read_only)
	{
		row* item = find_row(id);
		if (!item)
			return false;
		item->desc.read_only = read_only;
		update_row_ui(*item, false);
		return true;
	}

	void property_grid::set_read_only(bool read_only)
	{
		if (read_only_ == read_only)
			return;
		read_only_ = read_only;
		for (row& item : rows_)
			update_row_ui(item, false);
	}

	bool property_grid::read_only() const noexcept
	{
		return read_only_;
	}

	void property_grid::set_filter(std::string text)
	{
		filter_ = std::move(text);
		if (search_)
		{
			const char* current = vgui_edit_get_text(search_);
			if (!current || filter_ != current)
				vgui_edit_set_text(search_, filter_.c_str());
		}
		update_visibility();
	}

	const std::string& property_grid::filter() const noexcept
	{
		return filter_;
	}

	std::size_t property_grid::visible_row_count() const noexcept
	{
		return visible_rows_;
	}

	std::size_t property_grid::materialized_row_count() const noexcept
	{
		std::size_t count = 0;
		for (const row& item : rows_)
		{
			if (item.panel)
				++count;
		}
		return count;
	}

	bool property_grid::property_visible(const std::string& id) const
	{
		const row* item = find_row(id);
		return item && item->visible;
	}

	bool property_grid::set_category_collapsed(const std::string& category, bool collapsed)
	{
		category_collapsed_[category] = collapsed;
		update_visibility();
		return true;
	}

	bool property_grid::category_collapsed(const std::string& category) const
	{
		const auto it = category_collapsed_.find(category);
		return it != category_collapsed_.end() && it->second;
	}

	void property_grid::set_command_stack(command_stack* stack)
	{
		command_stack_ = stack;
	}

	command_stack* property_grid::command_stack_ptr() const noexcept
	{
		return command_stack_;
	}

	void property_grid::set_asset_drop_handler(asset_drop_handler handler)
	{
		asset_drop_ = std::move(handler);
		if (!panel_)
			return;
		if (!asset_drop_)
		{
			set_editor_drop_target(panel_, nullptr);
			drop_target_.reset();
			return;
		}
		if (!drop_target_)
		{
			class asset_drop_target final : public drop_target
			{
			public:
				explicit asset_drop_target(property_grid* owner) : owner_(owner) {}

				bool can_accept(const drag_payload& payload) const override
				{
					return !owner_->read_only_ &&
						payload.kind == drag_payload_kind::asset;
				}

				bool drop(const drag_payload& payload, float x, float y) override
				{
					(void)x;
					(void)y;
					if (!owner_ || payload.id.empty())
						return false;
					return owner_->asset_drop_(payload.id);
				}

			private:
				property_grid* owner_;
			};
			drop_target_ = std::make_unique<asset_drop_target>(this);
		}
		set_editor_drop_target(panel_, drop_target_.get());
	}

	/* P06 reference rows ------------------------------------------------------ */

	void property_grid::set_asset_query(asset_query_fn handler)
	{
		asset_query_ = std::move(handler);
	}

	void property_grid::set_asset_browse_handler(asset_browse_fn handler)
	{
		asset_browse_ = std::move(handler);
	}

	void property_grid::set_object_activated_handler(object_activate_fn handler)
	{
		object_activate_ = std::move(handler);
	}

	void property_grid::set_asset_filter_check(asset_filter_check_fn handler)
	{
		asset_filter_check_ = std::move(handler);
	}

	void property_grid::set_missing_asset_handler(missing_asset_fn handler)
	{
		missing_asset_ = std::move(handler);
	}

	bool property_grid::begin_batch(const std::string& name)
	{
		if (!command_stack_)
			return false;
		command_stack_->begin_transaction(name);
		++batch_depth_;
		return true;
	}

	bool property_grid::end_batch()
	{
		if (!command_stack_ || batch_depth_ == 0)
			return false;
		--batch_depth_;
		command_stack_->commit_transaction();
		return true;
	}

	/* ---- P08:多目标编辑 ------------------------------------------------------ */

	void property_grid::set_multi_targets(std::vector<object_id> targets)
	{
		multi_targets_ = std::move(targets);
		for (row& item : rows_)
		{
			item.mixed = false;
			sync_row_from_targets(item);
			update_row_ui(item, true);
		}
		update_visibility();
	}

	void property_grid::set_multi_getter(multi_getter_fn getter)
	{
		multi_getter_ = std::move(getter);
	}

	void property_grid::set_multi_setter(multi_setter_fn setter)
	{
		multi_setter_ = std::move(setter);
	}

	bool property_grid::property_mixed(const std::string& id) const
	{
		const row* item = find_row(id);
		return item && item->mixed;
	}

	void property_grid::sync_row_from_targets(row& item)
	{
		item.mixed = false;
		if (!multi_active())
			return;
		std::string first;
		bool have = false;
		for (object_id target : multi_targets_)
		{
			const std::string value = multi_getter_(item.desc.id, target);
			if (!have)
			{
				first = value;
				have = true;
			}
			else if (value != first)
			{
				item.mixed = true;
				return;
			}
		}
		if (have)
		{
			/* 全同:显示该值(过一遍规范化,宿主值不规范时兜底)。 */
			normalized_value normalized = normalize_value(item.desc, first);
			item.value = normalized.ok ? normalized.value : first;
		}
	}

	void property_grid::apply_multi_value(const std::string& id, object_id target,
		const std::string& value)
	{
		if (multi_setter_)
			multi_setter_(id, target, value);
		row* item = find_row(id);
		if (!item)
			return;
		sync_row_from_targets(*item);
		update_row_ui(*item, true);
		if (change_listener_)
			change_listener_(id, item->value);
	}

	void property_grid::update_row_mixed_ui(row& item)
	{
		if (!item.panel)
			return;
		switch (item.desc.type)
		{
		case property_type::enumeration:
			if (item.editor)
				vgui_widget_set_text(item.editor, "(mixed)");
			break;
		case property_type::boolean:
			if (item.editor)
				vgui_widget_set_checked(item.editor, 0);
			break;
		case property_type::color:
			if (item.swatch)
				vgui_widget_set_background(item.swatch, 0xFF6C7A89);
			/* color 行编辑框是 edit,落到下一组。 */
			[[fallthrough]];
		case property_type::text:
		case property_type::integer:
		case property_type::number:
			if (item.editor)
			{
				vgui_edit_set_text(item.editor, "");
				vgui_edit_set_placeholder(item.editor, "(mixed)");
			}
			break;
		case property_type::asset:
		case property_type::object_ref:
			if (item.editor)
			{
				vgui_widget_set_text(item.editor, "(mixed)");
				vgui_widget_set_text_color(item.editor, kLabelText);
			}
			if (item.thumb)
				vgui_widget_set_background(item.thumb, 0xFF6C7A89);
			break;
		default:
			/* vector:保留主选值分量显示,靠 tooltip 提示。 */
			break;
		}
		vgui_widget_set_tooltip(item.panel, "(mixed values)");
		if (item.label)
			vgui_widget_set_tooltip(item.label, "(mixed values)");
	}

	/* ---- P08:属性剪贴板 ------------------------------------------------------ */

	std::size_t property_grid::copy_properties()
	{
		auto& clipboard = property_clipboard();
		clipboard.clear();
		for (const row& item : rows_)
			clipboard[item.desc.id] = { item.desc.type, item.value };
		return clipboard.size();
	}

	std::size_t property_grid::clipboard_property_count()
	{
		return property_clipboard().size();
	}

	std::size_t property_grid::paste_properties()
	{
		const auto& clipboard = property_clipboard();
		/* 先数可粘贴行(id 相同且类型一致),空批不产生 undo 记录。 */
		std::vector<std::string> paste_ids;
		for (const row& item : rows_)
		{
			auto it = clipboard.find(item.desc.id);
			if (it != clipboard.end() && it->second.type == item.desc.type)
				paste_ids.push_back(item.desc.id);
		}
		if (paste_ids.empty())
			return 0;

		std::size_t pasted = 0;
		begin_batch("Paste properties");
		for (const std::string& id : paste_ids)
		{
			row* item = find_row(id);
			auto it = clipboard.find(id);
			if (item && it != clipboard.end() &&
				commit_value(*item, it->second.value, "Paste " + id))
			{
				++pasted;
			}
		}
		end_batch();
		return pasted;
	}

	/* ---- P08:收藏 ------------------------------------------------------------ */

	void property_grid::set_property_favorite(const std::string& id, bool favorite)
	{
		favorites_[id] = favorite;
		row* item = find_row(id);
		if (item)
		{
			item->favorite = favorite;
			if (item->fav_btn)
				vgui_widget_set_checked(item->fav_btn, favorite ? 1 : 0);
			update_visibility();
		}
	}

	bool property_grid::property_favorite(const std::string& id) const
	{
		auto it = favorites_.find(id);
		if (it != favorites_.end())
			return it->second;
		const row* item = find_row(id);
		return item && item->favorite;
	}

	std::vector<std::string> property_grid::favorite_ids() const
	{
		std::vector<std::string> out;
		for (const row& item : rows_)
			if (item.favorite)
				out.push_back(item.desc.id);
		return out;
	}

	void property_grid::set_favorites_only(bool only)
	{
		favorites_only_ = only;
		update_visibility();
	}

	bool property_grid::reference_accepts(const row& item,
		drag_payload_kind kind) const
	{
		const drag_payload_kind wanted = item.desc.type == property_type::asset
			? drag_payload_kind::asset
			: drag_payload_kind::scene_object;
		if (kind != wanted)
			return false;
		if (asset_filter_check_)
			return asset_filter_check_(item.value, item.desc.asset_filter);
		return true;
	}

	bool property_grid::commit_reference(row& item, const std::string& id)
	{
		const std::string label = item.desc.label.empty()
			? item.desc.id : item.desc.label;
		return commit_value(item, id, "Set " + label);
	}

class property_grid::reference_row_drop_target final : public drop_target
{
public:
	reference_row_drop_target(property_grid* owner, std::string id)
		: owner_(owner), id_(std::move(id))
	{
	}

	bool can_accept(const drag_payload& payload) const override
	{
		if (!owner_)
			return false;
		const row* item = owner_->find_row(id_);
		if (!item || !item->panel || owner_->read_only_ || item->desc.read_only)
			return false;
		return owner_->reference_accepts(*item, payload.kind);
	}

	void hover(const drag_payload& payload, vgui_widget* target, float x,
		float y, vgui_drop_hint* hint) override
	{
		(void)target;
		(void)x;
		(void)y;
		if (!hint || !can_accept(payload))
			return;
		row* item = owner_->find_row(id_);
		if (!item || !item->panel)
			return;
		float rx = 0, ry = 0, rw = 0, rh = 0;
		vgui_widget_get_rect(item->panel, &rx, &ry, &rw, &rh);
		if (rw <= 0.0f || rh <= 0.0f)
			return;
		hint->kind = VGUI_DROP_HINT_RECT;
		hint->x = rx;
		hint->y = ry;
		hint->w = rw;
		hint->h = rh;
	}

	bool drop(const drag_payload& payload, float x, float y) override
	{
		(void)x;
		(void)y;
		if (!owner_ || payload.id.empty())
			return false;
		row* item = owner_->find_row(id_);
		if (!item)
			return false;
		return owner_->commit_reference(*item, payload.id);
	}

private:
	property_grid* owner_;
	std::string id_;
};

	void property_grid::refresh_reference_row(row& item)
	{
		if (!item.editor)
			return;

		/* P08:混合态引用行只显示提示,不做解析。 */
		if (item.mixed)
		{
			vgui_widget_set_text(item.editor, "(mixed)");
			vgui_widget_set_text_color(item.editor, kLabelText);
			if (item.thumb)
				vgui_widget_set_background(item.thumb, 0xFF6C7A89);
			return;
		}

		/* 解析:查询宿主;失败/空值 → 名称占位与 missing 态。 */
		std::string previous_missing = item.missing ? "1" : "0";
		item.missing = false;
		item.resolved_name.clear();
		vgui_texture* thumbnail = nullptr;
		if (!item.value.empty() && asset_query_)
		{
			asset_info info;
			if (asset_query_(item.value, item.desc.asset_filter, &info))
			{
				item.resolved_name = info.name;
				thumbnail = info.thumbnail;
			}
			else
			{
				item.missing = true;
			}
		}
		else if (!item.value.empty())
		{
			item.missing = true;	/* 无 query handler:视为未解析 */
		}

		vgui_widget_set_text(item.editor, display_text(item).c_str());
		vgui_widget_set_text_color(item.editor,
			item.missing ? kErrorText : kLabelText);
		if (item.missing)
		{
			const std::string tip = "missing " +
				(item.desc.asset_filter.empty() ? std::string("asset")
					: item.desc.asset_filter) + ": " + item.value;
			vgui_widget_set_tooltip(item.editor, tip.c_str());
			vgui_widget_set_tooltip(item.thumb, tip.c_str());
		}
		else if (!item.desc.tooltip.empty())
		{
			vgui_widget_set_tooltip(item.editor, item.desc.tooltip.c_str());
			vgui_widget_set_tooltip(item.thumb, item.desc.tooltip.c_str());
		}
		else
		{
			vgui_widget_set_tooltip(item.editor, nullptr);
			vgui_widget_set_tooltip(item.thumb, nullptr);
		}

		/* 缩略图:有纹理贴纹理,否则用类型色块。 */
		if (item.thumb)
		{
			if (thumbnail)
				vgui_widget_set_background(item.thumb, 0x00000000);
			else if (item.desc.type == property_type::object_ref)
				vgui_widget_set_background(item.thumb, 0xFF7090C0);
			else if (item.missing)
				vgui_widget_set_background(item.thumb, kErrorText);
			else
				vgui_widget_set_background(item.thumb, item.value.empty()
					? kPanelBg : 0xFF40E0D0);
		}
		vgui_widget_set_enabled(item.clear_btn,
			(read_only_ || item.desc.read_only) ? 0 : 1);

		/* missing 转换时通知宿主(P07 hook)。 */
		if (item.missing && previous_missing != "1" && missing_asset_)
			missing_asset_(item.desc.id, item.desc.asset_filter);

		/* A04 行级 drop target:整行面板作锚点(缩略图/名称/按钮都可落),
		 * hover 用 RECT 高亮整行;detached adapter 泄漏式,每行只挂一次。 */
		if (item.panel && !item.drop_installed)
		{
			set_editor_drop_target(item.panel,
				new reference_row_drop_target(this, item.desc.id));
			item.drop_installed = true;
		}
	}

	void property_grid::set_change_listener(change_listener listener)
	{
		change_listener_ = std::move(listener);
	}

	void property_grid::set_value_source(value_source source)
	{
		value_source_ = std::move(source);
	}

	void property_grid::refresh()
	{
		/* P08:多目标模式优先从 getter 拉值(混合态检测)。 */
		if (multi_active())
		{
			for (row& item : rows_)
			{
				sync_row_from_targets(item);
				update_row_ui(item, true);
			}
			update_visibility();
			return;
		}
		for (row& item : rows_)
		{
			std::string value;
			bool has_value = false;
			if (value_source_)
			{
				has_value = value_source_(item.desc.id, value);
			}
			else if (item.desc.get_value)
			{
				value = item.desc.get_value();
				has_value = true;
			}
			if (!has_value)
				continue;

			normalized_value normalized = normalize_value(item.desc, std::move(value));
			if (!normalized.ok)
			{
				set_row_error(item, normalized.error);
				continue;
			}
			clear_row_error(item);
			item.value = normalized.value;
			update_row_ui(item, true);
		}
		update_visibility();
	}

	void property_grid::bind_selection(editor_context& context, selection_bind_fn binder)
	{
		unbind_selection();
		if (!binder)
			return;
		selection_context_ = &context;
		selection_bind_ = std::move(binder);
		selection_token_ = context.add_selection_listener([this](object_id id) {
			if (selection_bind_)
				selection_bind_(*this, id);
		});
		if (!selection_token_)
		{
			selection_context_ = nullptr;
			selection_bind_ = nullptr;
			return;
		}
		if (selection_bind_)
			selection_bind_(*this, context.selection());
	}

	void property_grid::unbind_selection()
	{
		if (selection_context_ && selection_token_)
			selection_context_->remove_selection_listener(selection_token_);
		selection_context_ = nullptr;
		selection_token_ = 0;
		selection_bind_ = nullptr;
	}

	vgui_widget* property_grid::property_editor(const std::string& id) const noexcept
	{
		const row* item = find_row(id);
		return item ? item->editor : nullptr;
	}

	vector_editor* property_grid::property_vector_editor(
		const std::string& id) const noexcept
	{
		const row* item = find_row(id);
		return item ? item->vector_ui.get() : nullptr;
	}

	vgui_widget* property_grid::widget() const noexcept
	{
		return panel_;
	}

	property_grid::row* property_grid::find_row(const std::string& id)
	{
		for (row& item : rows_)
		{
			if (item.desc.id == id)
				return &item;
		}
		return nullptr;
	}

	const property_grid::row* property_grid::find_row(const std::string& id) const
	{
		for (const row& item : rows_)
		{
			if (item.desc.id == id)
				return &item;
		}
		return nullptr;
	}

	property_grid::row* property_grid::find_row_by_token(std::uint64_t token)
	{
		for (row& item : rows_)
		{
			if (item.token == token)
				return &item;
		}
		return nullptr;
	}

	property_grid::row* property_grid::row_for_widget(vgui_widget* widget)
	{
		if (!widget)
			return nullptr;
		const char* raw = vgui_widget_get_id(widget);
		if (!raw || !*raw)
			return nullptr;
		static constexpr const char* prefix = "property:";
		const std::string id = raw;
		if (id.compare(0, 9, prefix) != 0)
			return nullptr;
		return find_row(id.substr(9));
	}

	property_grid::category_ui* property_grid::ensure_category(const std::string& name)
	{
		auto found = categories_.find(name);
		if (found == categories_.end())
			found = categories_.emplace(name, category_ui{}).first;
		category_ui& ui = found->second;
		if (!ui.header)
		{
			vgui_context* ctx = content_ ? vgui_widget_get_context(content_) : nullptr;
			if (!ctx)
				return nullptr;
			ui.header = vgui_widget_create(ctx, VGUI_BUTTON,
				category_header_text(name, category_collapsed(name)).c_str());
			if (!ui.header)
				return nullptr;
			vgui_widget_set_size(ui.header, 0, kCategoryHeight);
			vgui_widget_set_align_self(ui.header, VGUI_ALIGN_STRETCH);
			vgui_widget_set_font_size(ui.header, 12);
			/* Collapse arrows and names read as a column label, not a button
			 * caption, so the header text hugs the left edge like the row labels. */
			vgui_widget_set_text_align(ui.header, 0.0f, 0.5f);
			vgui_widget_set_background(ui.header, kCategoryBg);
			vgui_widget_set_id(ui.header, category_widget_id(name).c_str());
			vgui_widget_on_event(ui.header, &property_grid::on_category_click, this);
		}
		vgui_widget_set_text(ui.header,
			category_header_text(name, category_collapsed(name)).c_str());
		return &ui;
	}

	void property_grid::ensure_row_ui(row& item)
	{
		if (!content_ || item.panel)
			return;
		vgui_context* ctx = vgui_widget_get_context(content_);
		if (!ctx)
			return;

		/* Rows are created detached; update_window() attaches only the ones in
		 * the visible window to window_. */
		item.panel = vgui_widget_create(ctx, VGUI_PANEL, nullptr);
		if (!item.panel)
			return;

		// Rows use a fixed height; the style panel padding would make the
		// 24px controls overflow the row and break hit testing.
		vgui_widget_set_align_self(item.panel, VGUI_ALIGN_STRETCH);
		vgui_widget_set_layout(item.panel, VGUI_LAYOUT_COLUMN);
		vgui_widget_set_gap(item.panel, 0);
		vgui_widget_set_padding(item.panel, 0, 0, 0, 0);
		vgui_widget_set_size(item.panel, 0, kRowHeight);

		item.line = vgui_widget_add(item.panel, VGUI_PANEL, nullptr);
		vgui_widget_set_size(item.line, 0, kRowHeight);
		vgui_widget_set_align_self(item.line, VGUI_ALIGN_STRETCH);
		vgui_widget_set_layout(item.line, VGUI_LAYOUT_ROW);
		/* 24px controls sit in a 28px row; centering keeps the 2px slack on
		 * both edges so labels and values share one baseline. */
		vgui_widget_set_align_items(item.line, VGUI_ALIGN_CENTER);
		vgui_widget_set_gap(item.line, 6);
		vgui_widget_set_padding(item.line, 0, 0, 0, 0);

		item.label = vgui_widget_add(item.line, VGUI_LABEL, item.desc.label.c_str());
		vgui_widget_set_size(item.label, kLabelWidth, 24);
		vgui_widget_set_font_size(item.label, 13);
		vgui_widget_set_text_color(item.label, kLabelText);
		vgui_widget_set_text_align(item.label, 0.0f, 0.5f);
		vgui_widget_set_id(item.label, property_widget_id(item.desc.id).c_str());
		vgui_widget_on_event(item.label, &property_grid::on_row_event, this);

		if (item.desc.type == property_type::color)
		{
			item.swatch = vgui_widget_add(item.line, VGUI_PANEL, nullptr);
			vgui_widget_set_size(item.swatch, 20, 20);
			vgui_widget_set_border(item.swatch, kBorder, 1);
			vgui_widget_set_id(item.swatch, property_widget_id(item.desc.id).c_str());
			vgui_widget_on_event(item.swatch, &property_grid::on_row_event, this);
		}

		switch (item.desc.type)
		{
		case property_type::curve:
		case property_type::gradient:
		{
			/* P09:紧凑预览占用 editor 槽;点击在 context popup 中打开
			 * 完整编辑器(与颜色行一致)。 */
			item.editor = vgui_widget_add(item.line, VGUI_CUSTOM, nullptr);
			vgui_widget_set_grow(item.editor, 1.0f);
			vgui_widget_set_size(item.editor, 0, 24);
			vgui_widget_set_min_size(item.editor, 80, 24);
			vgui_widget_set_id(item.editor,
				property_widget_id(item.desc.id).c_str());
			vgui_widget_on_event(item.editor, &property_grid::on_row_event,
				this);
			if (item.desc.type == property_type::curve)
				vgui_custom_set_draw(item.editor,
					&property_grid::draw_curve_preview, this);
			else
				vgui_custom_set_draw(item.editor,
					&property_grid::draw_gradient_preview, this);
			break;
		}
		case property_type::boolean:
			item.editor = vgui_widget_add(item.line, VGUI_CHECKBOX, nullptr);
			vgui_widget_set_size(item.editor, 24, 24);
			vgui_widget_on_value(item.editor, &property_grid::on_bool_value, this);
			break;
		case property_type::enumeration:
			item.editor = vgui_widget_add(item.line, VGUI_BUTTON,
				display_text(item).c_str());
			vgui_widget_set_grow(item.editor, 1.0f);
			vgui_widget_set_size(item.editor, 0, 24);
			vgui_widget_set_font_size(item.editor, 13);
			vgui_widget_on_event(item.editor, &property_grid::on_row_event, this);
			break;
		case property_type::asset:
		case property_type::object_ref:
		{
			/* 缩略图 + 名称(占 editor 槽) + 清空 + 浏览。 */
			item.thumb = vgui_widget_add(item.line, VGUI_PANEL, nullptr);
			vgui_widget_set_size(item.thumb, 24, 24);
			vgui_widget_set_border(item.thumb, kBorder, 1);
			vgui_widget_set_id(item.thumb,
				property_widget_id(item.desc.id).c_str());
			vgui_widget_on_event(item.thumb,
				&property_grid::on_row_event, this);

			item.editor = vgui_widget_add(item.line, VGUI_LABEL, nullptr);
			vgui_widget_set_grow(item.editor, 1.0f);
			vgui_widget_set_size(item.editor, 0, 24);
			vgui_widget_set_font_size(item.editor, 13);
			vgui_widget_set_text_align(item.editor, 0.0f, 0.5f);
			vgui_widget_set_text_color(item.editor, kLabelText);
			vgui_widget_on_event(item.editor,
				&property_grid::on_row_event, this);

			item.clear_btn = vgui_widget_add(item.line, VGUI_BUTTON, "x");
			vgui_widget_set_size(item.clear_btn, 24, 24);
			vgui_widget_set_font_size(item.clear_btn, 11);
			vgui_widget_set_id(item.clear_btn,
				property_widget_id(item.desc.id).c_str());
			vgui_widget_set_tooltip(item.clear_btn, "Clear reference");
			vgui_widget_set_a11y_name(item.clear_btn, "Clear reference");
			vgui_widget_on_event(item.clear_btn,
				&property_grid::on_row_event, this);

			item.browse_btn = vgui_widget_add(item.line, VGUI_BUTTON, "...");
			vgui_widget_set_size(item.browse_btn, 30, 24);
			vgui_widget_set_font_size(item.browse_btn, 11);
			vgui_widget_set_id(item.browse_btn,
				property_widget_id(item.desc.id).c_str());
			vgui_widget_set_tooltip(item.browse_btn, "Browse");
			vgui_widget_set_a11y_name(item.browse_btn, "Browse reference");
			vgui_widget_on_event(item.browse_btn,
				&property_grid::on_row_event, this);
			break;
		}
		case property_type::vector2:
		case property_type::vector3:
		case property_type::vector4:
		{
			const int dimension = vector_dimension(item.desc.type);
			item.vector_ui = std::make_unique<vector_editor>();
			item.vector_ui->set_id(property_widget_id(item.desc.id));
			item.vector_ui->set_dimension(dimension);
			item.vector_ui->set_semantic(item.desc.semantic);
			if (!item.desc.component_labels.empty())
				item.vector_ui->set_axis_labels(item.desc.component_labels);
			if (!item.desc.component_colors.empty())
				item.vector_ui->set_axis_colors(item.desc.component_colors);
			item.vector_ui->set_value_type(spinbox::value_type::number);
			if (item.desc.has_range)
				item.vector_ui->set_range(item.desc.min_value,
					item.desc.max_value);
			if (item.desc.step > 0.0)
				item.vector_ui->set_step(item.desc.step);
			item.vector_ui->build(item.line, dimension);
			if (item.vector_ui->widget())
			{
				vgui_widget_set_grow(item.vector_ui->widget(), 1.0f);
				/* Fill the row so the component borders line up with the row
				 * border instead of leaving a gap at the bottom. */
				vgui_widget_set_size(item.vector_ui->widget(), 0, kRowHeight);
				vgui_widget_set_min_size(item.vector_ui->widget(), 80,
					kRowHeight);
				/* Let the row background show through the value fields. */
				for (int i = 0; i < item.vector_ui->dimension(); ++i)
				{
					vgui_widget* edit =
						item.vector_ui->component_edit_widget(i);
					if (edit)
						vgui_edit_set_background_transparent(edit, 1);
				}
				vector_editor::vector_value values{};
				std::string error;
				if (vector_editor::parse_value(item.value, dimension,
					values, &error))
					item.vector_ui->set_value(values);
			}
			const std::string property_id = item.desc.id;
			item.vector_ui->set_on_change(
				[this, property_id](const vector_editor::vector_value& values) {
					row* target = find_row(property_id);
					if (!target || !target->vector_ui)
						return;
					const int dimension = vector_dimension(target->desc.type);
					const std::string text =
						vector_editor::format_value(values, dimension);
					if (text == target->value)
						return;
					if (!target->drag_active)
					{
						target->drag_active = true;
						target->drag_value = target->value;
					}
					target->value = text;
					notify_change(*target);
					refresh_content_width();
				});
			item.vector_ui->set_on_commit(
				[this, property_id](const vector_editor::vector_value& values) {
					row* target = find_row(property_id);
					if (!target || !target->vector_ui)
						return;
					const int dimension = vector_dimension(target->desc.type);
					const std::string text =
						vector_editor::format_value(values, dimension);
					const std::string old_value = target->drag_active
						? target->drag_value : target->value;
					const std::uint64_t token = target->token;
					target->drag_active = false;
					target->drag_value.clear();
					target->value = text;
					update_row_ui(*target, true);
					refresh_content_width();
					const std::string label = target->desc.label.empty()
						? target->desc.id : target->desc.label;
					push_value_command(token, old_value, text,
						"Set " + label);
				});
			item.editor = item.vector_ui->widget();
			break;
		}
		default:
			item.editor = vgui_widget_add(item.line, VGUI_EDIT, item.value.c_str());
			vgui_widget_set_grow(item.editor, 1.0f);
			vgui_widget_set_size(item.editor, 0, 24);
			vgui_widget_set_font_size(item.editor, 13);
			vgui_edit_set_background_transparent(item.editor, 1);
			if (!item.desc.placeholder.empty())
				vgui_edit_set_placeholder(item.editor, item.desc.placeholder.c_str());
			vgui_widget_on_event(item.editor, &property_grid::on_edit_event, this);
			vgui_widget_on_text(item.editor, &property_grid::on_edit_text, this);
			break;
		}
		vgui_widget_set_id(item.editor, property_widget_id(item.desc.id).c_str());
		const std::string a11y_label = item.desc.label.empty()
			? item.desc.id : item.desc.label;
		vgui_widget_set_a11y_name(item.editor, a11y_label.c_str());
		if (item.vector_ui)
		{
			for (int i = 0; i < item.vector_ui->dimension(); ++i)
			{
				vgui_widget* component = item.vector_ui->component_edit_widget(i);
				if (!component)
					component = item.vector_ui->component_widget(i);
				const std::vector<std::string>& axis_labels =
					item.vector_ui->axis_labels();
				const std::string axis = i < (int)axis_labels.size()
					? axis_labels[(size_t)i] : std::to_string(i);
				const std::string component_name = a11y_label + " " + axis;
				vgui_widget_set_a11y_name(component, component_name.c_str());
			}
		}

		/* P08:收藏星标(reset 左侧),点击切换 favorites_only 过滤范围。 */
		item.favorite = property_favorite(item.desc.id);
		item.fav_btn = vgui_widget_add(item.line, VGUI_BUTTON, "*");
		vgui_widget_set_size(item.fav_btn, 20, 24);
		vgui_widget_set_font_size(item.fav_btn, 12);
		vgui_widget_set_id(item.fav_btn, property_widget_id(item.desc.id).c_str());
		vgui_widget_on_event(item.fav_btn, &property_grid::on_row_event, this);
		vgui_widget_set_checked(item.fav_btn, item.favorite ? 1 : 0);
		vgui_widget_set_tooltip(item.fav_btn, "Toggle favorite (common filter)");
		vgui_widget_set_a11y_name(item.fav_btn, "Toggle favorite");
		vgui_widget_set_a11y_role(item.fav_btn, VGUI_A11Y_ROLE_TOGGLE);

		item.reset = vgui_widget_add(item.line, VGUI_BUTTON, "R");
		vgui_widget_set_size(item.reset, kResetWidth, 24);
		vgui_widget_set_font_size(item.reset, 11);
		vgui_widget_set_id(item.reset, property_widget_id(item.desc.id).c_str());
		vgui_widget_on_event(item.reset, &property_grid::on_reset_click, this);
		vgui_widget_set_visible(item.reset, item.desc.has_default ? 1 : 0);
		vgui_widget_set_tooltip(item.reset, "Reset to default");
		vgui_widget_set_a11y_name(item.reset, "Reset to default");

		item.error_label = vgui_widget_add(item.panel, VGUI_LABEL, nullptr);
		vgui_widget_set_size(item.error_label, 0, kErrorHeight);
		vgui_widget_set_align_self(item.error_label, VGUI_ALIGN_STRETCH);
		vgui_widget_set_font_size(item.error_label, 11);
		vgui_widget_set_text_color(item.error_label, kErrorText);
		vgui_widget_set_visible(item.error_label, 0);

		if (!item.desc.tooltip.empty())
		{
			vgui_widget_set_tooltip(item.panel, item.desc.tooltip.c_str());
			vgui_widget_set_tooltip(item.label, item.desc.tooltip.c_str());
			vgui_widget_set_tooltip(item.editor, item.desc.tooltip.c_str());
			if (item.swatch)
				vgui_widget_set_tooltip(item.swatch, item.desc.tooltip.c_str());
			if (item.vector_ui)
			{
				for (int i = 0; i < vector_dimension(item.desc.type); ++i)
				{
					if (vgui_widget* edit =
						item.vector_ui->component_edit_widget(i))
						vgui_widget_set_tooltip(edit,
							item.desc.tooltip.c_str());
				}
			}
		}

		update_row_ui(item, true);
	}

	void property_grid::update_row_ui(row& item, bool sync_editor)
	{
		if (!item.panel)
			return;
		const bool readonly = read_only_ || item.desc.read_only;
		if (item.editor)
		{
			vgui_widget_set_enabled(item.editor, readonly ? 0 : 1);
			if (item.desc.type != property_type::boolean &&
				item.desc.type != property_type::enumeration &&
				item.desc.type != property_type::curve &&
				item.desc.type != property_type::gradient &&
				!is_vector_type(item.desc.type))
				vgui_edit_set_readonly(item.editor, readonly ? 1 : 0);
		}
		if (item.vector_ui)
		{
			item.vector_ui->set_read_only(readonly);
			item.vector_ui->set_enabled(!readonly);
		}
		if (item.reset)
		{
			vgui_widget_set_enabled(item.reset, readonly ? 0 : 1);
			vgui_widget_set_visible(item.reset, item.desc.has_default ? 1 : 0);
		}
		if (item.fav_btn)
			vgui_widget_set_checked(item.fav_btn, item.favorite ? 1 : 0);

		if (item.mixed)
		{
			update_row_mixed_ui(item);
			return;
		}

		if (sync_editor && item.editor)
		{
			switch (item.desc.type)
			{
			case property_type::boolean:
				vgui_widget_set_checked(item.editor, item.value == "true" ? 1 : 0);
				break;
			case property_type::enumeration:
				vgui_widget_set_text(item.editor, display_text(item).c_str());
				break;
			case property_type::asset:
			case property_type::object_ref:
				refresh_reference_row(item);
				break;
			case property_type::vector2:
			case property_type::vector3:
			case property_type::vector4:
			{
				if (!item.vector_ui)
					break;
				const int dimension = vector_dimension(item.desc.type);
				vector_editor::vector_value values{};
				std::string error;
				if (vector_editor::parse_value(item.value, dimension, values,
					&error))
					item.vector_ui->set_value(values);
				break;
			}
			case property_type::curve:
			case property_type::gradient:
				/* 自绘预览每帧从 item.value 读取,无需同步控件。 */
				break;
			default:
				/* 混合态可能残留 "(mixed)" 占位,恢复正常时还原 desc 占位。 */
				vgui_edit_set_text(item.editor, item.value.c_str());
				vgui_edit_set_placeholder(item.editor,
					item.desc.placeholder.empty() ? nullptr
						: item.desc.placeholder.c_str());
				break;
			}
		}

		if (item.swatch)
		{
			vgui_color color = 0;
			std::string canonical;
			if (parse_color(item.value, color, canonical))
				vgui_widget_set_background(item.swatch, color);
			else
				vgui_widget_set_background(item.swatch, kPanelBg);
		}

		update_row_error_ui(item);
	}

	void property_grid::update_row_error_ui(row& item)
	{
		if (!item.panel)
			return;
		const bool has_error = !item.error.empty();
		if (item.error_label)
		{
			vgui_widget_set_text(item.error_label, item.error.c_str());
			vgui_widget_set_visible(item.error_label, has_error ? 1 : 0);
		}
		vgui_widget_set_size(item.panel, 0,
			has_error ? kRowHeight + kErrorHeight + 2.0f : kRowHeight);

		const std::string& tip = has_error ? item.error : item.desc.tooltip;
		vgui_widget_set_tooltip(item.panel, tip.c_str());
		if (item.label)
			vgui_widget_set_tooltip(item.label, tip.c_str());
		if (item.editor)
			vgui_widget_set_tooltip(item.editor, tip.c_str());
		if (item.swatch)
			vgui_widget_set_tooltip(item.swatch, tip.c_str());
		if (item.vector_ui)
		{
			for (int i = 0; i < vector_dimension(item.desc.type); ++i)
			{
				if (vgui_widget* edit = item.vector_ui->component_edit_widget(i))
					vgui_widget_set_tooltip(edit, tip.c_str());
			}
		}
		if (item.editor)
		{
			if (has_error)
				vgui_widget_set_border(item.editor, kErrorText, 1.0f);
			else
				vgui_widget_set_border(item.editor, 0, 0.0f);
		}
	}

	float property_grid::row_height(const row& item) const
	{
		return item.error.empty() ? kRowHeight : kRowHeight + kErrorHeight + 2.0f;
	}

	void property_grid::rebuild_display()
	{
		display_.clear();
		display_.reserve(rows_.size() + category_order_.size());
		for (const std::string& name : category_order_)
		{
			bool any_match = false;
			for (const row& item : rows_)
			{
				if (item.desc.category == name && row_matches(item))
				{
					any_match = true;
					break;
				}
			}
			if (!any_match)
				continue;

			display_entry header;
			header.type = display_entry::kind::category;
			header.key = name;
			header.height = kCategoryHeight;
			display_.push_back(std::move(header));

			if (category_collapsed(name))
				continue;

			for (const row& item : rows_)
			{
				if (item.desc.category != name || !row_matches(item))
					continue;
				display_entry entry;
				entry.type = display_entry::kind::property;
				entry.key = item.desc.id;
				entry.token = item.token;
				entry.height = row_height(item);
				display_.push_back(std::move(entry));
			}
		}
	}

	void property_grid::recompute_display_layout()
	{
		float offset = 0.0f;
		for (std::size_t i = 0; i < display_.size(); ++i)
		{
			display_[i].offset = offset;
			offset += display_[i].height;
			if (i + 1 < display_.size())
				offset += kItemGap;
		}
		content_height_ = offset;
		if (content_)
			vgui_widget_set_size(content_, 0, content_height_);
	}

	/* Vector rows size their value fields from the value text, so a long
	 * number can need more room than the host gives the grid.  Feed the widest
	 * visible row back as content_ min width: the stretch floor then widens the
	 * scrollable content and the horizontal scrollbar appears instead of the
	 * row being clipped.  Rows scrolled out of the window keep their last
	 * measurement until they materialize again. */
	void property_grid::refresh_content_width()
	{
		if (!content_)
			return;
		float required = kMinContentWidth;
		for (row& item : rows_)
		{
			if (!item.visible || !is_vector_type(item.desc.type))
				continue;
			/* Measure the row line, not the whole row: the error label under the
			 * line must not widen the scroll range with its message. */
			vgui_widget* content = item.line ? item.line : item.panel;
			if (content)
			{
				float width = 0.0f, height = 0.0f;
				vgui_widget_get_intrinsic_size(content, &width, &height);
				item.required_width = to_logical(content, width);
			}
			required = std::max(required, item.required_width);
		}
		vgui_widget_set_min_size(content_, required, 0);
	}

	void property_grid::refresh_viewport()
	{
		if (!panel_)
			return;
		float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
		vgui_widget_get_rect(panel_, &x, &y, &w, &h);
		(void)x; (void)y; (void)w;
		if (h > 0.0f)
			viewport_h_ = std::max(0.0f,
				to_logical(panel_, h) - kPanelPaddingV);
	}

	/* Filtering, collapsing and row errors change the content height without a
	 * scroll event.  Clamp the stored offset to the new range (and move the
	 * panel with it) so the materialized window matches what the next layout
	 * pass will actually show. */
	void property_grid::sync_scroll_to_content()
	{
		if (!panel_)
			return;
		refresh_viewport();
		float sx = 0.0f, sy = 0.0f;
		vgui_panel_get_scroll(panel_, &sx, &sy);
		const float sy_logical = to_logical(panel_, std::max(0.0f, sy));
		float clamped = sy_logical;
		if (viewport_h_ > 0.0f)
		{
			const float max_scroll = std::max(0.0f,
				content_height_ + kSearchLead - viewport_h_);
			clamped = std::clamp(clamped, 0.0f, max_scroll);
		}
		scroll_y_ = clamped;
		if (std::fabs(clamped - sy_logical) > 0.01f)
			vgui_panel_set_scroll(panel_, sx, to_physical(panel_, clamped));
	}

	vgui_widget* property_grid::entry_widget(const display_entry& entry)
	{
		if (entry.type == display_entry::kind::category)
		{
			category_ui* ui = ensure_category(entry.key);
			return ui ? ui->header : nullptr;
		}
		row* item = find_row_by_token(entry.token);
		if (!item)
			item = find_row(entry.key);
		if (!item)
			return nullptr;
		ensure_row_ui(*item);
		return item->panel;
	}

	void property_grid::release_row(row& item)
	{
		if (item.menu)
		{
			vgui_widget_destroy(item.menu);
			item.menu = nullptr;
			item.menu_values.clear();
		}
		/* Destroy the composite editor before its widget tree so no spinbox
		 * callback can observe a partially destroyed row. */
		item.vector_ui.reset();
		reset_drag_state(item);
		if (!item.panel)
			return;
		vgui_context* ctx = vgui_widget_get_context(item.panel);
		if (ctx && item.editor && vgui_get_focus(ctx) == item.editor)
			vgui_set_focus(ctx, nullptr);
		vgui_widget_remove(item.panel);
		vgui_widget_destroy(item.panel);
		item.panel = nullptr;
		item.line = nullptr;
		item.label = nullptr;
		item.swatch = nullptr;
		item.thumb = nullptr;
		item.clear_btn = nullptr;
		item.browse_btn = nullptr;
		item.editor = nullptr;
		item.fav_btn = nullptr;
		item.reset = nullptr;
		item.error_label = nullptr;
		/* The widgets are gone, so the next materialization must reinstall the
		 * row drop target on the new panel. */
		item.drop_installed = false;
		item.drag_active = false;
	}

	void property_grid::release_category(category_ui& ui)
	{
		if (!ui.header)
			return;
		vgui_widget_remove(ui.header);
		vgui_widget_destroy(ui.header);
		ui.header = nullptr;
	}

	void property_grid::update_window()
	{
		if (!content_ || !window_ || !top_spacer_ || !bottom_spacer_ ||
			updating_window_)
			return;
		updating_window_ = true;

		const std::size_t count = display_.size();
		std::size_t begin = 0;
		std::size_t end = 0;
		if (count > 0)
		{
			const float scroll = std::max(0.0f, scroll_y_ - kSearchLead);
			const float viewport = viewport_h_ > 0.0f ? viewport_h_ : 600.0f;
			const float top = std::max(0.0f, scroll - kOverscan);
			const float bottom = scroll + viewport + kOverscan;
			auto first = std::lower_bound(display_.begin(), display_.end(), top,
				[](const display_entry& entry, float value) {
					return entry.offset + entry.height < value;
				});
			begin = (std::size_t)(first - display_.begin());
			if (begin >= count)
				begin = count - 1;
			auto last = std::upper_bound(
				display_.begin() + (std::ptrdiff_t)begin, display_.end(), bottom,
				[](float value, const display_entry& entry) {
					return value < entry.offset;
				});
			end = (std::size_t)(last - display_.begin());
			if (end <= begin)
				end = std::min(count, begin + 1);
			if (end - begin < kMinWindowEntries)
				end = std::min(count, begin + kMinWindowEntries);
			if (end - begin < kMinWindowEntries && begin > 0)
			{
				begin = end > kMinWindowEntries ? end - kMinWindowEntries : 0;
				if (end <= begin)
					end = std::min(count, begin + 1);
			}
		}
		window_begin_ = begin;
		window_end_ = end;

		/* Materialization is tracked per row/header rather than per display
		 * index, so rebuilding the display list (filter, collapse, add/remove)
		 * can safely reorder entries without releasing the wrong widgets. */
		std::vector<std::string> keep_rows;
		std::vector<std::string> keep_categories;
		keep_rows.reserve(end > begin ? end - begin : 0);
		keep_categories.reserve(end > begin ? end - begin : 0);
		for (std::size_t i = begin; i < end; ++i)
		{
			if (display_[i].type == display_entry::kind::category)
				keep_categories.push_back(display_[i].key);
			else
				keep_rows.push_back(display_[i].key);
		}
		auto keep = [](const std::vector<std::string>& list, const std::string& key) {
			return std::find(list.begin(), list.end(), key) != list.end();
		};
		for (row& item : rows_)
		{
			if (item.panel && !keep(keep_rows, item.desc.id))
				release_row(item);
		}
		for (auto& entry : categories_)
		{
			if (entry.second.header && !keep(keep_categories, entry.first))
				release_category(entry.second);
		}

		std::vector<vgui_widget*> ordered;
		ordered.reserve(end > begin ? end - begin : 0);
		for (std::size_t i = begin; i < end; ++i)
		{
			vgui_widget* widget = entry_widget(display_[i]);
			if (widget)
				ordered.push_back(widget);
		}

		/* Detach the current window children, then reattach them in display
		 * order.  Detach/attach preserves widget identity and focus for rows
		 * that stay inside the window. */
		std::vector<vgui_widget*> current;
		const std::size_t current_count = vgui_widget_get_child_count(window_);
		current.reserve(current_count);
		for (std::size_t i = 0; i < current_count; ++i)
			current.push_back(vgui_widget_get_child(window_, i));
		for (vgui_widget* child : current)
			vgui_widget_remove(child);
		for (vgui_widget* widget : ordered)
			vgui_widget_attach(window_, widget);

		float window_height = 0.0f;
		for (std::size_t i = begin; i < end; ++i)
			window_height += display_[i].height;
		if (end > begin)
			window_height += kItemGap * (float)(end - begin - 1);
		vgui_widget_set_size(window_, 0, window_height);
		vgui_widget_set_visible(window_, window_height > 0.0f ? 1 : 0);

		float top_height = 0.0f;
		float bottom_height = 0.0f;
		if (end > begin)
		{
			top_height = display_[begin].offset;
			const display_entry& last = display_[end - 1];
			bottom_height = content_height_ - (last.offset + last.height);
		}
		else
		{
			bottom_height = content_height_;
		}
		top_height = std::max(0.0f, top_height);
		bottom_height = std::max(0.0f, bottom_height);
		vgui_widget_set_size(top_spacer_, 0, top_height);
		vgui_widget_set_visible(top_spacer_, top_height > 0.5f ? 1 : 0);
		vgui_widget_set_size(bottom_spacer_, 0, bottom_height);
		vgui_widget_set_visible(bottom_spacer_, bottom_height > 0.5f ? 1 : 0);
		vgui_widget_set_size(content_, 0, content_height_);

		updating_window_ = false;
		refresh_content_width();
	}

	void property_grid::update_visibility()
	{
		visible_rows_ = 0;
		for (row& item : rows_)
		{
			item.visible = row_matches(item) &&
				!category_collapsed(item.desc.category);
			if (item.visible)
				++visible_rows_;
		}
		rebuild_display();
		recompute_display_layout();
		sync_scroll_to_content();
		update_window();
	}

	bool property_grid::row_matches(const row& item) const
	{
		/* P08:收藏过滤与搜索叠加。 */
		if (favorites_only_ && !item.favorite)
			return false;
		if (filter_.empty())
			return true;
		const std::string needle = lower_copy(filter_);
		auto contains = [&needle](const std::string& text) {
			return lower_copy(text).find(needle) != std::string::npos;
		};
		return contains(item.desc.id) || contains(item.desc.label) ||
			contains(item.desc.category) || contains(item.value);
	}

	void property_grid::set_row_error(row& item, std::string error)
	{
		const bool height_changed = item.error.empty() != error.empty();
		item.error = std::move(error);
		update_row_error_ui(item);
		if (height_changed && content_)
		{
			recompute_display_layout();
			sync_scroll_to_content();
			update_window();
		}
	}

	void property_grid::clear_row_error(row& item)
	{
		if (item.error.empty())
			return;
		item.error.clear();
		update_row_error_ui(item);
		if (content_)
		{
			recompute_display_layout();
			sync_scroll_to_content();
			update_window();
		}
	}

	void property_grid::reset_drag_state(row& item)
	{
		item.drag_active = false;
		item.drag_value.clear();
	}

	void property_grid::notify_change(const row& item)
	{
		const std::string id = item.desc.id;
		const std::string value = item.value;
		/* 多目标模式由 multi_setter 写模型;desc.set_value 只服务单选。 */
		if (!multi_active() && item.desc.set_value)
			item.desc.set_value(value);
		if (change_listener_)
			change_listener_(id, value);
	}

	void property_grid::apply_value(row& item, std::string value)
	{
		reset_drag_state(item);
		item.value = std::move(value);
		update_row_ui(item, true);
		notify_change(item);
		update_visibility();
	}

	void property_grid::apply_recorded_value(std::uint64_t token, const std::string& value)
	{
		row* item = find_row_by_token(token);
		if (!item)
			return;
		normalized_value normalized = normalize_value(item->desc, value);
		if (!normalized.ok)
		{
			set_row_error(*item, normalized.error);
			return;
		}
		clear_row_error(*item);
		reset_drag_state(*item);
		item->value = normalized.value;
		update_row_ui(*item, true);
		notify_change(*item);
		update_visibility();
	}

	void property_grid::push_value_command(std::uint64_t token,
		std::string old_value, std::string new_value,
		std::string command_name)
	{
		if (!command_stack_ || old_value == new_value)
			return;
		std::weak_ptr<int> alive = alive_token_;
		property_command::apply_fn apply =
			[this, alive](std::uint64_t token, const std::string& value) {
				if (!alive.expired())
					apply_recorded_value(token, value);
			};
		command_stack_->push(std::make_unique<property_command>(
			std::move(command_name), token, std::move(old_value),
			std::move(new_value), std::move(apply)));
	}

	bool property_grid::commit_value(row& item, std::string value,
		std::string command_name)
	{
		if (read_only_ || item.desc.read_only)
		{
			set_row_error(item, "property is read-only");
			return false;
		}
		normalized_value normalized = normalize_value(item.desc, std::move(value));
		if (!normalized.ok)
		{
			set_row_error(item, normalized.error);
			return false;
		}
		if (item.desc.type == property_type::enumeration &&
			!item.desc.choice_enabled.empty())
		{
			const int index = choice_index_for_text(item.desc, normalized.value);
			if (index >= 0 && !item.desc.choice_enabled[(std::size_t)index])
			{
				set_row_error(item, "choice is disabled");
				return false;
			}
		}
		clear_row_error(item);
		if (multi_active())
		{
			/* P08:混合/多目标提交——值应用到每个 target,batch 内
			 * per-target 命令(先应用后 push),一条 undo;undo/redo 经
			 * getter 重刷行(各对象回旧值时自动回到混合态)。 */
			const std::string new_value = normalized.value;
			const std::string label = item.desc.label.empty()
				? item.desc.id : item.desc.label;
			const std::string name = command_name.empty()
				? "Set " + label : command_name;
			item.mixed = false;
			apply_value(item, new_value);
			if (item.desc.type == property_type::color)
				add_recent_color(new_value);

			std::vector<std::pair<object_id, std::string>> records;
			records.reserve(multi_targets_.size());
			for (object_id target : multi_targets_)
			{
				std::string old_value = multi_getter_(item.desc.id, target);
				if (old_value != new_value)
					records.emplace_back(target, std::move(old_value));
			}
			if (command_stack_ && !records.empty())
			{
				command_stack_->begin_transaction(name);
				const std::string id = item.desc.id;
				std::weak_ptr<int> alive = alive_token_;
				for (auto& record : records)
				{
					multi_command::apply_fn apply =
						[this, alive, id](object_id target,
							const std::string& value)
					{
						if (!alive.expired())
							apply_multi_value(id, target, value);
					};
					command_stack_->push(std::make_unique<multi_command>(
						name, id, record.first, std::move(record.second),
						new_value, std::move(apply)));
					multi_setter_(id, record.first, new_value);
				}
				command_stack_->commit_transaction();
			}
			else
			{
				for (const auto& record : records)
					multi_setter_(item.desc.id, record.first, new_value);
			}
			return true;
		}
		if (normalized.value == item.value)
		{
			update_row_ui(item, true);
			return true;
		}

		const std::string old_value = item.value;
		const std::string new_value = normalized.value;
		const std::uint64_t token = item.token;
		const bool is_color = item.desc.type == property_type::color;
		apply_value(item, new_value);
		if (is_color)
			add_recent_color(new_value);
		push_value_command(token, old_value, new_value,
			std::move(command_name));
		return true;
	}

	std::string property_grid::display_text(const row& item) const
	{
		if (item.desc.type == property_type::asset ||
			item.desc.type == property_type::object_ref)
		{
			if (item.value.empty())
				return "(None)";
			return item.missing ? item.value + " (missing)"
				: (item.resolved_name.empty() ? item.value
					: item.resolved_name);
		}
		if (item.desc.type == property_type::enumeration)
		{
			if (item.value.empty() && item.desc.allow_empty)
				return empty_choice_label(item.desc);
			const int index = choice_index_for_text(item.desc, item.value);
			if (index >= 0)
				return choice_label_at(item.desc, (std::size_t)index);
		}
		return item.value;
	}

	void property_grid::open_enum_menu(row& item)
	{
		if (read_only_ || item.desc.read_only)
			return;
		if (!item.editor || item.desc.choices.empty())
			return;
		if (!item.menu)
		{
			vgui_context* ctx = vgui_widget_get_context(item.editor);
			if (!ctx)
				return;
			item.menu = vgui_widget_create(ctx, VGUI_MENU, nullptr);
			if (!item.menu)
				return;
			vgui_menu_on_activate(item.menu, &property_grid::on_menu_activate, this);
			if (item.desc.allow_empty)
			{
				vgui_item_id menu_item = vgui_menu_add_item(item.menu,
					empty_choice_label(item.desc).c_str());
				if (menu_item)
					item.menu_values[menu_item] = std::string();
			}
			for (std::size_t i = 0; i < item.desc.choices.size(); ++i)
			{
				vgui_item_id menu_item = vgui_menu_add_item(item.menu,
					choice_label_at(item.desc, i).c_str());
				if (!menu_item)
					continue;
				item.menu_values[menu_item] = choice_value_at(item.desc, i);
				if (!item.desc.choice_enabled.empty() &&
					!item.desc.choice_enabled[i])
					vgui_menu_set_item_enabled(item.menu, menu_item, 0);
			}
		}
		for (const auto& entry : item.menu_values)
			vgui_menu_set_item_checked(item.menu, entry.first,
				entry.second == item.value ? 1 : 0);
		vgui_menu_open_below(item.menu, item.editor);
		if (!vgui_menu_is_open(item.menu))
			return;
		/* Start keyboard navigation on the current selection so opening and
		 * closing the popup never changes the value by itself. */
		for (const auto& entry : item.menu_values)
		{
			if (entry.second != item.value)
				continue;
			vgui_menu_set_cursor(item.menu, entry.first);
			break;
		}
	}

	void property_grid::begin_popup_session(row& item, popup_kind kind)
	{
		popup_kind_ = kind;
		popup_session_active_ = true;
		popup_session_mixed_ = item.mixed;
		popup_session_start_ = item.value;
		popup_session_id_ = item.desc.id;
		popup_drag_active_ = false;
		popup_drag_value_.clear();
	}

	void property_grid::reset_popup_session()
	{
		popup_kind_ = popup_kind::none;
		popup_session_active_ = false;
		popup_session_mixed_ = false;
		popup_session_start_.clear();
		popup_session_id_.clear();
		popup_drag_active_ = false;
		popup_drag_value_.clear();
	}

	void property_grid::preview_popup_value(const std::string& id,
		const std::string& value)
	{
		row* target = find_row(id);
		if (!target)
			return;
		normalized_value normalized = normalize_value(target->desc, value);
		if (!normalized.ok)
			return;
		if (!popup_drag_active_)
		{
			/* 第一次预览:快照手势起点,提交时作为 undo 记录的 old。 */
			popup_drag_active_ = true;
			popup_drag_value_ = target->value;
		}
		target->mixed = false;
		target->value = normalized.value;
		update_row_ui(*target, true);
		update_visibility();
	}

	void property_grid::commit_popup_value(const std::string& id,
		const std::string& value)
	{
		row* target = find_row(id);
		if (!target)
			return;
		normalized_value normalized = normalize_value(target->desc, value);
		if (!normalized.ok)
			return;
		const std::string old_value = popup_drag_active_
			? popup_drag_value_ : target->value;
		popup_drag_active_ = false;
		popup_drag_value_.clear();
		target->mixed = false;
		if (old_value == normalized.value)
		{
			target->value = normalized.value;
			update_row_ui(*target, true);
			return;
		}
		/* commit_value() 用行当前值作为 old;先回填手势起点,这样单目标
		 * 直接入栈,多目标仍按每 target getter 生成 per-target 记录。 */
		target->value = old_value;
		const std::string label = target->desc.label.empty()
			? target->desc.id : target->desc.label;
		(void)commit_value(*target, normalized.value, "Set " + label);
	}

	void property_grid::open_color_picker(row& item)
	{
		if (read_only_ || item.desc.read_only)
			return;
		if (!item.editor)
			return;
		vgui_context* ctx = vgui_widget_get_context(item.editor);
		if (!ctx)
			return;
		if (!color_picker_)
		{
			color_picker_ = std::make_unique<color_picker>();
			color_picker_->set_id("property-grid.color-picker");
			color_picker_->build(ctx);
		}

		static const char* const kPresets[] = {
			"#FFFFFF", "#000000", "#FF0000", "#00FF00",
			"#0000FF", "#FFFF00", "#00FFFF", "#FF00FF",
			"#808080", "#FFA500"
		};
		std::vector<std::string> presets;
		for (const char* preset : kPresets)
			presets.push_back(preset);
		color_picker_->set_presets(presets);
		color_picker_->set_recent(recent_colors_);

		const std::string id = item.desc.id;
		color_picker_->set_on_change([this, id](const std::string& hex) {
			preview_popup_value(id, hex);
		});
		color_picker_->set_on_commit([this, id](const std::string& hex) {
			commit_popup_value(id, hex);
		});
		color_picker_->set_value(item.value);
		color_picker_->set_current(item.value);

		float ax = 0, ay = 0, aw = 0, ah = 0;
		if (item.swatch)
			vgui_widget_get_rect(item.swatch, &ax, &ay, &aw, &ah);
		else
			vgui_widget_get_rect(item.editor, &ax, &ay, &aw, &ah);
		(void)aw;
		if (!vgui_popup_open(ctx, color_picker_->widget(), ax, ay + ah + 4.0f))
			return;
		vgui_popup_on_close(ctx, &property_grid::on_popup_close, this);
		begin_popup_session(item, popup_kind::color);
	}

	void property_grid::open_curve_editor(row& item)
	{
		if (read_only_ || item.desc.read_only || !item.editor)
			return;
		vgui_context* ctx = vgui_widget_get_context(item.editor);
		if (!ctx)
			return;
		if (!curve_editor_)
		{
			curve_editor_ = std::make_unique<curve_editor>();
			curve_editor_->set_id("property-grid.curve-editor");
			curve_editor_->build(ctx);
		}
		const std::string id = item.desc.id;
		curve_editor_->set_on_change([this, id](const curve_value& v) {
			preview_popup_value(id, curve_editor::format_value(v));
		});
		curve_editor_->set_on_commit([this, id](const curve_value& v) {
			commit_popup_value(id, curve_editor::format_value(v));
		});
		(void)curve_editor_->set_value_text(item.value);

		float ax = 0, ay = 0, aw = 0, ah = 0;
		vgui_widget_get_rect(item.editor, &ax, &ay, &aw, &ah);
		(void)aw;
		if (!vgui_popup_open(ctx, curve_editor_->widget(), ax, ay + ah + 4.0f))
			return;
		vgui_popup_on_close(ctx, &property_grid::on_popup_close, this);
		begin_popup_session(item, popup_kind::curve);
	}

	void property_grid::open_gradient_editor(row& item)
	{
		if (read_only_ || item.desc.read_only || !item.editor)
			return;
		vgui_context* ctx = vgui_widget_get_context(item.editor);
		if (!ctx)
			return;
		if (!gradient_editor_)
		{
			gradient_editor_ = std::make_unique<gradient_editor>();
			gradient_editor_->set_id("property-grid.gradient-editor");
			gradient_editor_->build(ctx);
		}
		const std::string id = item.desc.id;
		gradient_editor_->set_on_change(
			[this, id](const gradient_value& v) {
				preview_popup_value(id, gradient_editor::format_value(v));
			});
		gradient_editor_->set_on_commit(
			[this, id](const gradient_value& v) {
				commit_popup_value(id, gradient_editor::format_value(v));
			});
		(void)gradient_editor_->set_value_text(item.value);

		float ax = 0, ay = 0, aw = 0, ah = 0;
		vgui_widget_get_rect(item.editor, &ax, &ay, &aw, &ah);
		(void)aw;
		if (!vgui_popup_open(ctx, gradient_editor_->widget(),
			ax, ay + ah + 4.0f))
			return;
		vgui_popup_on_close(ctx, &property_grid::on_popup_close, this);
		begin_popup_session(item, popup_kind::gradient);
	}

	void property_grid::on_popup_close(void* user, int reason)
	{
		auto* self = static_cast<property_grid*>(user);
		if (!self || !self->popup_session_active_)
			return;
		self->popup_session_active_ = false;
		const std::string id = self->popup_session_id_;
		/* 未提交的预览(Esc/外点/替换)回到会话起点;已提交的手势保留
		 * 新值,但 Esc 仍回滚显示(与合并链顶相接,undo 一次恢复)。 */
		const bool rollback = reason == VGUI_POPUP_CLOSE_ESCAPE ||
			self->popup_drag_active_;
		row* target = self->find_row(id);
		if (target && rollback)
		{
			if (self->popup_session_mixed_ && self->multi_active())
			{
				self->sync_row_from_targets(*target);
				self->update_row_ui(*target, true);
			}
			else
			{
				(void)self->set_property_value(id,
					self->popup_session_start_);
			}
		}
		self->reset_popup_session();
	}

	void property_grid::add_recent_color(const std::string& color)
	{
		auto found = std::find(recent_colors_.begin(), recent_colors_.end(), color);
		if (found != recent_colors_.end())
			recent_colors_.erase(found);
		recent_colors_.insert(recent_colors_.begin(), color);
		if (recent_colors_.size() > 8)
			recent_colors_.resize(8);
	}

	void property_grid::draw_curve_preview(vgui_widget* widget, ovg_ctx_cb* cb,
		rvg_t* vg, float x, float y, float w, float h, float dt, void* user)
	{
		(void)dt;
		auto* self = static_cast<property_grid*>(user);
		if (!self || !cb || !vg)
			return;
		row* item = self->row_for_widget(widget);
		if (!item)
			return;
		curve_value value;
		const bool parsed = !item->mixed &&
			curve_editor::parse_value(item->value, value);
		curve_editor::draw_preview(cb, vg, x, y, w, h, value,
			item->mixed || !parsed, ui_scale_of(widget));
	}

	void property_grid::draw_gradient_preview(vgui_widget* widget,
		ovg_ctx_cb* cb, rvg_t* vg, float x, float y, float w, float h,
		float dt, void* user)
	{
		(void)dt;
		auto* self = static_cast<property_grid*>(user);
		if (!self || !cb || !vg)
			return;
		row* item = self->row_for_widget(widget);
		if (!item)
			return;
		gradient_value value;
		const bool parsed = !item->mixed &&
			gradient_editor::parse_value(item->value, value);
		gradient_editor::draw_preview(cb, vg, x, y, w, h, value,
			item->mixed || !parsed, ui_scale_of(widget));
	}

	void property_grid::on_scroll(vgui_widget* widget, float x, float y, void* user)
	{
		auto* self = static_cast<property_grid*>(user);
		if (!self || widget != self->panel_)
			return;
		self->refresh_viewport();
		const float y_logical = to_logical(widget, y);
		float clamped = y_logical;
		if (self->viewport_h_ > 0.0f)
		{
			const float max_scroll = std::max(0.0f,
				self->content_height_ + kSearchLead - self->viewport_h_);
			clamped = std::clamp(y_logical, 0.0f, max_scroll);
		}
		self->scroll_y_ = clamped;
		if (std::fabs(clamped - y_logical) > 0.01f)
			vgui_panel_set_scroll(widget, x, to_physical(widget, clamped));
		self->update_window();
	}

	void property_grid::on_search_text(vgui_widget* widget, const char* text, void* user)
	{
		(void)widget;
		auto* self = static_cast<property_grid*>(user);
		if (self)
			self->set_filter(text ? text : "");
	}

	void property_grid::on_fav_filter_click(vgui_widget* widget,
		const vgui_event* event, void* user)
	{
		(void)widget;
		auto* self = static_cast<property_grid*>(user);
		if (!self || !event || event->type != VGUI_EVENT_CLICK)
			return;
		self->set_favorites_only(!self->favorites_only_);
		if (self->fav_filter_)
			vgui_widget_set_checked(self->fav_filter_,
				self->favorites_only_ ? 1 : 0);
	}

	void property_grid::on_edit_event(vgui_widget* widget, const vgui_event* event,
		void* user)
	{
		if (!event || event->type != VGUI_EVENT_SUBMIT)
			return;
		auto* self = static_cast<property_grid*>(user);
		if (!self)
			return;
		row* item = self->row_for_widget(widget);
		if (!item)
			return;
		const std::string label = item->desc.label.empty() ? item->desc.id : item->desc.label;
		const char* text = vgui_edit_get_text(widget);
		/* Keep invalid text in the editor so the user can correct it; the row
		 * error border/message is set by commit_value(). */
		(void)self->commit_value(*item, text ? text : "", "Set " + label);
	}

	void property_grid::on_edit_text(vgui_widget* widget, const char* text, void* user)
	{
		(void)text;
		auto* self = static_cast<property_grid*>(user);
		if (!self)
			return;
		row* item = self->row_for_widget(widget);
		if (item)
			self->clear_row_error(*item);
	}

	void property_grid::on_bool_value(vgui_widget* widget, float value, void* user)
	{
		auto* self = static_cast<property_grid*>(user);
		if (!self)
			return;
		row* item = self->row_for_widget(widget);
		if (!item)
			return;
		const std::string label = item->desc.label.empty() ? item->desc.id : item->desc.label;
		if (!self->commit_value(*item, value >= 0.5f ? "true" : "false", "Set " + label))
			self->update_row_ui(*item, true);
	}

	void property_grid::on_row_event(vgui_widget* widget, const vgui_event* event,
		void* user)
	{
		if (!event)
			return;
		auto* self = static_cast<property_grid*>(user);
		if (!self)
			return;
		row* item = self->row_for_widget(widget);
		if (!item)
			return;

		const bool editable = !self->read_only_ && !item->desc.read_only;
		if (!editable)
		{
			if (item->drag_active)
			{
				self->reset_drag_state(*item);
				self->update_row_ui(*item, true);
			}
			return;
		}

		if (event->type == VGUI_EVENT_CLICK)
		{
			if (widget == item->fav_btn)
			{
				self->set_property_favorite(item->desc.id, !item->favorite);
				return;
			}
			if (widget == item->swatch)
			{
				self->open_color_picker(*item);
				return;
			}
			if (widget == item->editor &&
				item->desc.type == property_type::curve)
			{
				self->open_curve_editor(*item);
				return;
			}
			if (widget == item->editor &&
				item->desc.type == property_type::gradient)
			{
				self->open_gradient_editor(*item);
				return;
			}
			if (widget == item->clear_btn &&
				(item->desc.type == property_type::asset ||
					item->desc.type == property_type::object_ref))
			{
				self->commit_reference(*item, "");
				return;
			}
			if (widget == item->browse_btn &&
				(item->desc.type == property_type::asset ||
					item->desc.type == property_type::object_ref))
			{
				if (self->asset_browse_)
					self->asset_browse_(item->value, item->desc.asset_filter);
				return;
			}
			if (widget == item->editor &&
				item->desc.type == property_type::object_ref &&
				!item->value.empty())
			{
				if (self->object_activate_)
				{
					try
					{
						self->object_activate_(static_cast<object_id>(
							std::stoull(item->value)));
					}
					catch (...)
					{
					}
				}
				return;
			}
			if (widget == item->editor &&
				item->desc.type == property_type::enumeration)
			{
				self->open_enum_menu(*item);
				return;
			}
			if (widget == item->label)
			{
				if (item->desc.type == property_type::boolean)
				{
					const std::string label = item->desc.label.empty()
						? item->desc.id : item->desc.label;
					self->commit_value(*item,
						item->value == "true" ? "false" : "true", "Set " + label);
					return;
				}
				if (item->desc.type == property_type::enumeration)
				{
					/* Labels are not focusable.  Move focus to the enum
					 * button first so closing the popup leaves focus in the
					 * editor instead of clearing it. */
					if (item->editor && !self->read_only_ &&
						!item->desc.read_only)
						vgui_set_focus(vgui_widget_get_context(widget),
							item->editor);
					self->open_enum_menu(*item);
					return;
				}
				if (item->desc.type == property_type::curve)
				{
					self->open_curve_editor(*item);
					return;
				}
				if (item->desc.type == property_type::gradient)
				{
					self->open_gradient_editor(*item);
					return;
				}
				if (item->editor && item->desc.type != property_type::color)
				{
					if (is_vector_type(item->desc.type))
					{
						if (item->vector_ui)
							item->vector_ui->focus_component(0);
					}
					else
						vgui_set_focus(vgui_widget_get_context(widget),
							item->editor);
				}
			}
		}
	}

	void property_grid::on_reset_click(vgui_widget* widget, const vgui_event* event,
		void* user)
	{
		if (!event || event->type != VGUI_EVENT_CLICK)
			return;
		auto* self = static_cast<property_grid*>(user);
		if (!self)
			return;
		row* item = self->row_for_widget(widget);
		if (item)
			self->reset_property(item->desc.id);
	}

	void property_grid::on_category_click(vgui_widget* widget, const vgui_event* event,
		void* user)
	{
		if (!event || event->type != VGUI_EVENT_CLICK)
			return;
		auto* self = static_cast<property_grid*>(user);
		if (!self)
			return;
		const char* raw = vgui_widget_get_id(widget);
		if (!raw)
			return;
		static constexpr const char* prefix = "category:";
		const std::string id = raw;
		if (id.compare(0, 9, prefix) != 0)
			return;
		const std::string name = id.substr(9);
		self->set_category_collapsed(name, !self->category_collapsed(name));
	}

	void property_grid::on_menu_activate(vgui_widget* menu, vgui_item_id item_id,
		void* user)
	{
		auto* self = static_cast<property_grid*>(user);
		if (!self || !menu || !item_id)
			return;
		for (row& item : self->rows_)
		{
			if (item.menu != menu)
				continue;
			const auto found = item.menu_values.find(item_id);
			if (found == item.menu_values.end())
				return;
			const std::string label = item.desc.label.empty()
				? item.desc.id : item.desc.label;
			self->commit_value(item, found->second, "Set " + label);
			return;
		}
	}

} /* namespace vgui_editor_kit */
