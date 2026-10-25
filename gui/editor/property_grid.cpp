#include "property_grid.h"

#include "drag_bridge.h"
#include "editor_context.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <system_error>
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
		constexpr float kPanelPaddingV = 12.0f;	// build(): 6 top + 6 bottom
		constexpr float kSearchLead = 28.0f;	// search 24 + gap 4
		constexpr float kOverscan = 96.0f;
		constexpr std::size_t kMinWindowEntries = 4;

		struct normalized_value
		{
			bool ok = false;
			std::string value;
			std::string error;
		};

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
				const int index = choice_index_for_text(desc, text);
				if (index < 0)
				{
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

	property_grid::~property_grid()
	{
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
		vgui_widget_set_grow(panel_, 1.0f);
		vgui_widget_set_layout(panel_, VGUI_LAYOUT_COLUMN);
		vgui_widget_set_padding(panel_, 6, 6, 6, 6);
		vgui_widget_set_gap(panel_, kItemGap);
		vgui_widget_set_background(panel_, kPanelBg);
		vgui_widget_set_border(panel_, kBorder, 1);
		vgui_panel_set_scrollable(panel_, 0, 1);

		search_ = vgui_widget_add(panel_, VGUI_EDIT, nullptr);
		vgui_widget_set_size(search_, 0, 24);
		vgui_widget_set_align_self(search_, VGUI_ALIGN_STRETCH);
		vgui_widget_set_font_size(search_, 12);
		vgui_widget_set_id(search_, "__property_search");
		vgui_edit_set_placeholder(search_, "Search properties");
		vgui_widget_on_text(search_, &property_grid::on_search_text, this);

		/* content_ is a fixed-height column of three children:
		 *   top spacer -> materialized window -> bottom spacer
		 * The spacers stand in for rows that are outside the viewport, so the
		 * scroll range stays exact without building every row. */
		content_ = vgui_widget_add(panel_, VGUI_PANEL, nullptr);
		vgui_widget_set_align_self(content_, VGUI_ALIGN_STRETCH);
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
		viewport_h_ = ch > 0 ? (float)ch - kPanelPaddingV : 0.0f;
		if (viewport_h_ < 200.0f)
			viewport_h_ = 200.0f;

		update_visibility();
	}

	void property_grid::clear()
	{
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
			vgui_widget_set_size(content_, 0, 0);
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

	bool property_grid::add_property(property_desc desc)
	{
		if (desc.id.empty())
		{
			desc.id = "property." + std::to_string(next_row_id_++);
			while (find_row(desc.id))
				desc.id = "property." + std::to_string(next_row_id_++);
		}
		if (find_row(desc.id))
			return false;
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
		if (command_stack_)
			command_stack_->begin_transaction("Reset all properties");
		for (const std::string& id : ids)
			reset_property(id);
		if (command_stack_)
			command_stack_->commit_transaction();
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
					return payload.kind == drag_payload_kind::asset;
				}

				bool drop(const drag_payload& payload, float x, float y) override
				{
					(void)x;
					(void)y;
					if (!owner_ || payload.id.empty())
						return false;
					owner_->asset_drop_(payload.id);
					return true;
				}

			private:
				property_grid* owner_;
			};
			drop_target_ = std::make_unique<asset_drop_target>(this);
		}
		set_editor_drop_target(panel_, drop_target_.get());
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
		vgui_widget_set_gap(item.line, 6);
		vgui_widget_set_padding(item.line, 0, 0, 0, 0);

		item.label = vgui_widget_add(item.line, VGUI_LABEL, item.desc.label.c_str());
		vgui_widget_set_size(item.label, kLabelWidth, 24);
		vgui_widget_set_font_size(item.label, 13);
		vgui_widget_set_text_color(item.label, kLabelText);
		vgui_widget_set_text_align(item.label, 0.0f, 0.5f);
		vgui_widget_set_id(item.label, property_widget_id(item.desc.id).c_str());
		vgui_widget_on_event(item.label, &property_grid::on_row_event, this);
		if (item.desc.type == property_type::integer ||
			item.desc.type == property_type::number)
			vgui_widget_set_cursor(item.label, VGUI_CURSOR_SIZE_H);

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
		default:
			item.editor = vgui_widget_add(item.line, VGUI_EDIT, item.value.c_str());
			vgui_widget_set_grow(item.editor, 1.0f);
			vgui_widget_set_size(item.editor, 0, 24);
			vgui_widget_set_font_size(item.editor, 13);
			if (!item.desc.placeholder.empty())
				vgui_edit_set_placeholder(item.editor, item.desc.placeholder.c_str());
			vgui_widget_on_event(item.editor, &property_grid::on_edit_event, this);
			vgui_widget_on_text(item.editor, &property_grid::on_edit_text, this);
			break;
		}
		vgui_widget_set_id(item.editor, property_widget_id(item.desc.id).c_str());

		item.reset = vgui_widget_add(item.line, VGUI_BUTTON, "R");
		vgui_widget_set_size(item.reset, kResetWidth, 24);
		vgui_widget_set_font_size(item.reset, 11);
		vgui_widget_set_id(item.reset, property_widget_id(item.desc.id).c_str());
		vgui_widget_on_event(item.reset, &property_grid::on_reset_click, this);
		vgui_widget_set_visible(item.reset, item.desc.has_default ? 1 : 0);
		vgui_widget_set_tooltip(item.reset, "Reset to default");

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
		}

		update_row_ui(item, true);
	}

	void property_grid::update_row_ui(row& item, bool sync_editor)
	{
		if (!item.panel)
			return;
		const bool readonly = read_only_ || item.desc.read_only;
		if (item.label &&
			(item.desc.type == property_type::integer ||
				item.desc.type == property_type::number))
			vgui_widget_set_cursor(item.label,
				readonly ? VGUI_CURSOR_ARROW : VGUI_CURSOR_SIZE_H);
		if (item.editor)
		{
			vgui_widget_set_enabled(item.editor, readonly ? 0 : 1);
			if (item.desc.type != property_type::boolean &&
				item.desc.type != property_type::enumeration)
				vgui_edit_set_readonly(item.editor, readonly ? 1 : 0);
		}
		if (item.reset)
		{
			vgui_widget_set_enabled(item.reset, readonly ? 0 : 1);
			vgui_widget_set_visible(item.reset, item.desc.has_default ? 1 : 0);
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
			default:
				vgui_edit_set_text(item.editor, item.value.c_str());
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

	void property_grid::refresh_viewport()
	{
		if (!panel_)
			return;
		float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
		vgui_widget_get_rect(panel_, &x, &y, &w, &h);
		if (h > 0.0f)
			viewport_h_ = std::max(0.0f, h - kPanelPaddingV);
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
		float clamped = std::max(0.0f, sy);
		if (viewport_h_ > 0.0f)
		{
			const float max_scroll = std::max(0.0f,
				content_height_ + kSearchLead - viewport_h_);
			clamped = std::clamp(clamped, 0.0f, max_scroll);
		}
		scroll_y_ = clamped;
		if (clamped != sy)
			vgui_panel_set_scroll(panel_, sx, clamped);
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
		item.editor = nullptr;
		item.reset = nullptr;
		item.error_label = nullptr;
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

	void property_grid::notify_change(const row& item)
	{
		const std::string id = item.desc.id;
		const std::string value = item.value;
		if (item.desc.set_value)
			item.desc.set_value(value);
		if (change_listener_)
			change_listener_(id, value);
	}

	void property_grid::apply_value(row& item, std::string value)
	{
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
		item->value = normalized.value;
		update_row_ui(*item, true);
		notify_change(*item);
		update_visibility();
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

		if (command_stack_)
		{
			std::weak_ptr<int> alive = alive_token_;
			property_command::apply_fn apply =
				[this, alive](std::uint64_t token, const std::string& value) {
					if (!alive.expired())
						apply_recorded_value(token, value);
				};
			command_stack_->push(std::make_unique<property_command>(
				std::move(command_name), token, old_value, new_value,
				std::move(apply)));
		}
		return true;
	}

	std::string property_grid::display_text(const row& item) const
	{
		if (item.desc.type == property_type::enumeration)
		{
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
	}

	void property_grid::open_color_menu(row& item)
	{
		if (read_only_ || item.desc.read_only)
			return;
		if (!item.editor)
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
		}

		vgui_menu_clear(item.menu);
		item.menu_values.clear();
		static const char* const presets[] = {
			"#FFFFFF", "#000000", "#FF0000", "#00FF00",
			"#0000FF", "#FFFF00", "#00FFFF", "#FF00FF",
			"#808080", "#FFA500"
		};
		auto add_color = [&item](const char* color) {
			vgui_item_id menu_item = vgui_menu_add_item(item.menu, color);
			if (!menu_item)
				return;
			item.menu_values[menu_item] = color;
		};
		for (const char* preset : presets)
			add_color(preset);
		if (!recent_colors_.empty())
		{
			vgui_menu_add_separator(item.menu);
			for (const std::string& recent : recent_colors_)
				add_color(recent.c_str());
		}
		for (const auto& entry : item.menu_values)
			vgui_menu_set_item_checked(item.menu, entry.first,
				entry.second == item.value ? 1 : 0);
		vgui_menu_open_below(item.menu, item.editor);
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

	void property_grid::on_scroll(vgui_widget* widget, float x, float y, void* user)
	{
		auto* self = static_cast<property_grid*>(user);
		if (!self || widget != self->panel_)
			return;
		self->refresh_viewport();
		float clamped = y;
		if (self->viewport_h_ > 0.0f)
		{
			const float max_scroll = std::max(0.0f,
				self->content_height_ + kSearchLead - self->viewport_h_);
			clamped = std::clamp(y, 0.0f, max_scroll);
		}
		self->scroll_y_ = clamped;
		if (clamped != y)
			vgui_panel_set_scroll(widget, x, clamped);
		self->update_window();
	}

	void property_grid::on_search_text(vgui_widget* widget, const char* text, void* user)
	{
		(void)widget;
		auto* self = static_cast<property_grid*>(user);
		if (self)
			self->set_filter(text ? text : "");
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
				item->drag_active = false;
				item->drag_value.clear();
				self->update_row_ui(*item, true);
			}
			return;
		}

		if (widget == item->label &&
			(item->desc.type == property_type::integer ||
				item->desc.type == property_type::number))
		{
			if (event->type == VGUI_EVENT_MOUSE_DOWN && event->button == 1)
			{
				double value = 0.0;
				if (!parse_number_text(item->value, value))
					return;
				item->drag_active = true;
				item->drag_start_x = event->x;
				item->drag_start_value = value;
				item->drag_value = item->value;
				return;
			}
			if (event->type == VGUI_EVENT_MOUSE_MOVE && item->drag_active)
			{
				double step = item->desc.step > 0.0
					? item->desc.step
					: (item->desc.type == property_type::integer ? 1.0 : 0.1);
				vgui_context* ctx = vgui_widget_get_context(widget);
				const int mods = vgui_context_get_mods(ctx);
				if (mods & VGUI_MOD_SHIFT)
					step *= 0.1;
				if (mods & VGUI_MOD_ALT)
					step *= 10.0;

				double value = item->drag_start_value +
					(double)(event->x - item->drag_start_x) * step;
				if (item->desc.type == property_type::integer)
					value = std::round(value);
				else if (item->desc.step > 0.0)
				{
					const double base = item->desc.has_range
						? item->desc.min_value : 0.0;
					value = base + std::round((value - base) / item->desc.step) *
						item->desc.step;
				}
				if (item->desc.has_range)
				{
					const double low = std::min(item->desc.min_value,
						item->desc.max_value);
					const double high = std::max(item->desc.min_value,
						item->desc.max_value);
					value = std::clamp(value, low, high);
				}
				item->drag_value = item->desc.type == property_type::integer
					? std::to_string((long long)std::llround(value))
					: format_number(value);
				if (item->editor)
					vgui_edit_set_text(item->editor, item->drag_value.c_str());
				return;
			}
			if (event->type == VGUI_EVENT_MOUSE_UP && item->drag_active)
			{
				item->drag_active = false;
				const std::string label = item->desc.label.empty()
					? item->desc.id : item->desc.label;
				const std::string value = item->drag_value;
				item->drag_value.clear();
				if (!self->commit_value(*item, value, "Set " + label))
					self->update_row_ui(*item, true);
				return;
			}
		}

		if (event->type == VGUI_EVENT_CLICK)
		{
			if (widget == item->swatch)
			{
				self->open_color_menu(*item);
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
					self->open_enum_menu(*item);
					return;
				}
				if (item->editor && item->desc.type != property_type::color)
					vgui_set_focus(vgui_widget_get_context(widget), item->editor);
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
