#include "spinbox.h"

#include "editor_metrics.h"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <utility>

namespace vgui_editor_kit {

	namespace {
		constexpr vgui_color kErrorColor = 0xFFE06C75;
		constexpr float kLabelWidth = 72.0f;
		constexpr float kEmptyLabelWidth = 16.0f;

		std::string trim_copy(std::string text)
		{
			auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
			while (!text.empty() && is_space((unsigned char)text.front()))
				text.erase(text.begin());
			while (!text.empty() && is_space((unsigned char)text.back()))
				text.pop_back();
			return text;
		}

		bool parse_integer_text(const std::string& text, long long& out)
		{
			if (text.empty())
				return false;
			errno = 0;
			char* end = nullptr;
			const long long value = std::strtoll(text.c_str(), &end, 10);
			if (errno == ERANGE || end == text.c_str() || *end != '\0')
				return false;
			out = value;
			return true;
		}

		bool parse_number_text(const std::string& text, double& out)
		{
			if (text.empty())
				return false;
			errno = 0;
			char* end = nullptr;
			const double value = std::strtod(text.c_str(), &end);
			if (errno == ERANGE || end == text.c_str() || *end != '\0' ||
				!std::isfinite(value))
				return false;
			out = value;
			return true;
		}

		std::string format_number(double value)
		{
			if (value == 0.0)
				value = 0.0;
			/* Enough significant digits to keep typical editor values intact
			 * while still trimming the binary noise from step arithmetic. */
			char buffer[64];
			const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value,
				std::chars_format::general, 15);
			if (result.ec != std::errc())
				return std::to_string(value);
			return std::string(buffer, result.ptr);
		}

		int context_mods(vgui_widget* widget)
		{
			vgui_context* ctx = widget ? vgui_widget_get_context(widget) : nullptr;
			return ctx ? vgui_context_get_mods(ctx) : 0;
		}
	}

	void spinbox::build(vgui_widget* parent, std::string label)
	{
		if (panel_ || !parent)
			return;

		label_ = std::move(label);

		panel_ = vgui_widget_add(parent, VGUI_PANEL, nullptr);
		const float row_h = chrome_row_height();
		vgui_widget_set_size(panel_, 220, row_h);
		vgui_widget_set_min_size(panel_, 120, row_h);
		vgui_widget_set_layout(panel_, VGUI_LAYOUT_ROW);
		vgui_widget_set_padding(panel_, 0, 0, 0, 0);
		vgui_widget_set_gap(panel_, 2);
		vgui_widget_set_align_items(panel_, VGUI_ALIGN_CENTER);

		label_widget_ = vgui_widget_add(panel_, VGUI_LABEL, label_.c_str());
		vgui_widget_set_size(label_widget_,
			label_.empty() ? kEmptyLabelWidth : label_width_, row_h);
		vgui_widget_set_text_align(label_widget_, 0.0f, 0.5f);
		vgui_widget_set_tooltip(label_widget_, "Drag to change");
		vgui_widget_on_event(label_widget_, &spinbox::on_label_event, this);

		edit_ = vgui_widget_add(panel_, VGUI_EDIT, "");
		vgui_widget_set_a11y_name(edit_, label_.empty() ? "Value" : label_.c_str());
		vgui_widget_set_a11y_role(edit_, VGUI_A11Y_ROLE_EDIT);
		vgui_widget_set_grow(edit_, 1.0f);
		vgui_widget_set_size(edit_, 0, row_h);
		apply_font();
		vgui_widget_on_event(edit_, &spinbox::on_edit_event, this);
		vgui_widget_on_text(edit_, &spinbox::on_edit_text, this);

		decrement_.build(panel_, "-");
		decrement_.set_tooltip("Decrease");
		decrement_.set_on_click([this]() { nudge(-1, context_mods(edit_)); });
		vgui_widget_set_size(decrement_.widget(), row_h, row_h);

		increment_.build(panel_, "+");
		increment_.set_tooltip("Increase");
		increment_.set_on_click([this]() { nudge(1, context_mods(edit_)); });
		vgui_widget_set_size(increment_.widget(), row_h, row_h);

		apply_ids();
		set_buttons_visible(buttons_visible_);
		update_control_state();
		sync_text();
		update_error_ui();
	}

	void spinbox::set_id(std::string id)
	{
		if (id.empty())
			return;
		id_ = std::move(id);
		apply_ids();
	}

	const std::string& spinbox::id() const noexcept
	{
		return id_;
	}

	void spinbox::apply_ids()
	{
		if (panel_)
			vgui_widget_set_id(panel_, id_.c_str());
		if (label_widget_)
			vgui_widget_set_id(label_widget_, (id_ + "_label").c_str());
		if (edit_)
			vgui_widget_set_id(edit_, (id_ + "_edit").c_str());
		if (decrement_.widget())
			vgui_widget_set_id(decrement_.widget(),
				(id_ + "_decrement").c_str());
		if (increment_.widget())
			vgui_widget_set_id(increment_.widget(),
				(id_ + "_increment").c_str());
	}

	void spinbox::set_label(std::string label)
	{
		label_ = std::move(label);
		if (!label_widget_)
			return;
		vgui_widget_set_text(label_widget_, label_.c_str());
		vgui_widget_set_size(label_widget_,
			label_.empty() ? kEmptyLabelWidth : label_width_, chrome_row_height());
	}

	const std::string& spinbox::label() const noexcept
	{
		return label_;
	}

	void spinbox::set_label_width(float width)
	{
		if (!std::isfinite(width))
			return;
		label_width_ = std::clamp(width, 0.0f, 512.0f);
		if (label_widget_)
		{
			vgui_widget_set_size(label_widget_,
				label_.empty() ? kEmptyLabelWidth : label_width_,
				chrome_row_height());
		}
	}

	float spinbox::label_width() const noexcept
	{
		return label_width_;
	}

	void spinbox::set_font_size(float size)
	{
		font_size_ = std::isfinite(size) && size > 0.0f ? size : 0.0f;
		if (panel_)
		{
			apply_font();
			apply_chrome();
		}
	}

	float spinbox::font_size() const noexcept
	{
		return font_size_;
	}

	void spinbox::set_edit_min_width(float width)
	{
		edit_min_width_ = std::isfinite(width) && width > 0.0f ? width : 0.0f;
		if (edit_)
			apply_chrome();
	}

	float spinbox::edit_min_width() const noexcept
	{
		return edit_min_width_;
	}

	void spinbox::set_auto_width(bool enabled)
	{
		auto_width_ = enabled;
		auto_width_applied_ = 0.0f;
		apply_auto_width();
	}

	bool spinbox::auto_width() const noexcept
	{
		return auto_width_;
	}

	void spinbox::apply_font()
	{
		/* 0 resets to "inherit the context font". */
		if (label_widget_)
			vgui_widget_set_font_size(label_widget_, font_size_);
		if (edit_)
			vgui_widget_set_font_size(edit_, font_size_);
	}

	void spinbox::apply_chrome()
	{
		if (!panel_)
			return;
		const float row_h = chrome_row_height();
		vgui_widget_set_size(panel_, 220, row_h);
		vgui_widget_set_min_size(panel_, buttons_visible_ ? 120 : 60, row_h);
		if (label_widget_)
			vgui_widget_set_size(label_widget_,
				label_.empty() ? kEmptyLabelWidth : label_width_, row_h);
		auto_width_applied_ = 0.0f;
		if (edit_)
		{
			const float width = edit_min_width_ > 0.0f ? edit_min_width_
				: (auto_width_ ? auto_edit_width() : 0.0f);
			if (auto_width_ && edit_min_width_ <= 0.0f)
				auto_width_applied_ = width;
			vgui_widget_set_size(edit_, width, row_h);
		}
		if (decrement_.widget())
			vgui_widget_set_size(decrement_.widget(), row_h, row_h);
		if (increment_.widget())
			vgui_widget_set_size(increment_.widget(), row_h, row_h);
	}

	/* Width that fits the text currently in the edit.  Falls back to a
	 * per-character estimate when the font is unavailable (headless tests) so
	 * the field never collapses to bare padding. */
	float spinbox::auto_edit_width() const
	{
		if (!edit_)
			return 0.0f;
		const char* current = vgui_edit_get_text(edit_);
		const std::string text = current ? current : value_text();
		vgui_context* ctx = vgui_widget_get_context(edit_);
		float text_w = text.empty() ? 0.0f
			: to_logical(edit_, vgui_widget_measure_text(edit_, text.c_str()));
		if (text_w <= 0.0f)
		{
			const float font = font_size_ > 0.0f
				? font_size_ : editor_font_size(ctx);
			text_w = font * 0.62f * (float)text.size();
		}
		float padding = 16.0f;
		if (ctx)
		{
			vgui_style style;
			vgui_context_get_style(ctx, &style);
			padding = style.edit.padding_x * 2.0f + 4.0f;
		}
		return text_w + std::max(padding, 16.0f);
	}

	void spinbox::apply_auto_width()
	{
		if (!edit_ || !auto_width_ || edit_min_width_ > 0.0f)
			return;
		const float width = std::max(auto_edit_width(), 24.0f);
		if (std::fabs(width - auto_width_applied_) < 0.5f)
			return;
		auto_width_applied_ = width;
		vgui_widget_set_size(edit_, width, chrome_row_height());
	}

	float spinbox::chrome_row_height() const
	{
		vgui_context* ctx = panel_ ? vgui_widget_get_context(panel_) : nullptr;
		const float font = font_size_ > 0.0f ? font_size_ : editor_font_size(ctx);
		return std::max(24.0f, font + 10.0f);
	}

	void spinbox::set_buttons_visible(bool visible)
	{
		buttons_visible_ = visible;
		if (decrement_.widget())
			vgui_widget_set_visible(decrement_.widget(), visible ? 1 : 0);
		if (increment_.widget())
			vgui_widget_set_visible(increment_.widget(), visible ? 1 : 0);
		if (panel_)
			apply_chrome();
	}

	bool spinbox::buttons_visible() const noexcept
	{
		return buttons_visible_;
	}

	void spinbox::set_value_type(value_type type)
	{
		if (type_ == type)
			return;
		type_ = type;
		set_value(value_);
	}

	spinbox::value_type spinbox::type() const noexcept
	{
		return type_;
	}

	void spinbox::set_range(double min_value, double max_value)
	{
		if (!std::isfinite(min_value) || !std::isfinite(max_value))
			return;
		if (min_value > max_value)
			std::swap(min_value, max_value);
		has_range_ = true;
		min_value_ = min_value;
		max_value_ = max_value;
		set_value(value_);
	}

	void spinbox::clear_range()
	{
		has_range_ = false;
	}

	bool spinbox::has_range() const noexcept
	{
		return has_range_;
	}

	double spinbox::min_value() const noexcept
	{
		return min_value_;
	}

	double spinbox::max_value() const noexcept
	{
		return max_value_;
	}

	void spinbox::set_step(double step)
	{
		step_ = std::isfinite(step) && step > 0.0 ? step : 0.0;
	}

	double spinbox::step() const noexcept
	{
		return effective_step();
	}

	double spinbox::effective_step() const noexcept
	{
		if (step_ > 0.0)
			return step_;
		return type_ == value_type::integer ? 1.0 : 0.1;
	}

	double spinbox::normalize_value(double value, bool snap_to_step) const noexcept
	{
		if (!std::isfinite(value))
			return value_;
		if (type_ == value_type::integer)
		{
			value = std::round(value);
			if (has_range_)
			{
				/* Keep integer values inside an integer-friendly range even
				 * when the host supplied fractional bounds. */
				const double low = std::ceil(min_value_);
				const double high = std::floor(max_value_);
				if (low <= high)
					value = std::clamp(value, low, high);
				else
					value = std::round(std::clamp(value, min_value_, max_value_));
			}
		}
		else
		{
			if (snap_to_step)
			{
				const double step = effective_step();
				if (step > 0.0)
				{
					const double base = has_range_ ? min_value_ : 0.0;
					value = base + std::round((value - base) / step) * step;
				}
			}
			if (has_range_)
				value = std::clamp(value, min_value_, max_value_);
		}
		return value == 0.0 ? 0.0 : value;
	}

	double spinbox::canonicalize(double value) const
	{
		if (type_ == value_type::integer)
		{
			const double rounded = std::round(value);
			if (rounded >= 9.0e18)
				return 9.0e18;
			if (rounded <= -9.0e18)
				return -9.0e18;
			return rounded == 0.0 ? 0.0 : rounded;
		}

		const std::string text = format_number(value);
		double parsed = value;
		if (parse_number_text(text, parsed))
			return parsed;
		return value;
	}

	void spinbox::set_value(double value)
	{
		if (!std::isfinite(value))
			return;
		error_.clear();
		value_ = canonicalize(normalize_value(value, false));
		sync_text();
		update_error_ui();
	}

	double spinbox::value() const noexcept
	{
		return value_;
	}

	std::string spinbox::value_text() const
	{
		if (type_ == value_type::integer)
			return std::to_string((long long)std::llround(value_));
		return format_number(value_);
	}

	void spinbox::sync_text()
	{
		if (!edit_)
			return;
		const std::string text = value_text();
		const char* current = vgui_edit_get_text(edit_);
		if (!current || text != current)
			vgui_edit_set_text(edit_, text.c_str());
		apply_auto_width();
	}

	bool spinbox::commit_text(std::string text)
	{
		if (!enabled_)
		{
			set_error("value is disabled");
			return false;
		}
		if (read_only_)
		{
			set_error("value is read-only");
			return false;
		}

		const std::string trimmed = trim_copy(std::move(text));
		double parsed = 0.0;
		if (type_ == value_type::integer)
		{
			long long integer = 0;
			if (!parse_integer_text(trimmed, integer))
			{
				set_error("expected an integer");
				return false;
			}
			parsed = (double)integer;
		}
		else if (!parse_number_text(trimmed, parsed))
		{
			set_error("expected a number");
			return false;
		}

		const double next = canonicalize(normalize_value(parsed, false));
		error_.clear();
		update_error_ui();
		if (next == value_)
		{
			sync_text();
			return true;
		}

		value_ = next;
		sync_text();
		notify_change();
		notify_commit();
		return true;
	}

	bool spinbox::commit_value(double value)
	{
		if (!enabled_)
		{
			set_error("value is disabled");
			return false;
		}
		if (read_only_)
		{
			set_error("value is read-only");
			return false;
		}
		if (!std::isfinite(value))
		{
			set_error("expected a number");
			return false;
		}

		const double next = canonicalize(normalize_value(value, false));
		error_.clear();
		update_error_ui();
		if (next == value_)
		{
			sync_text();
			return true;
		}

		value_ = next;
		sync_text();
		notify_change();
		notify_commit();
		return true;
	}

	void spinbox::step_by(int steps)
	{
		if (steps == 0)
			return;
		if (!enabled_ || read_only_)
			return;

		double delta = (double)steps * effective_step();
		if (type_ == value_type::integer)
		{
			delta = std::round(delta);
			if (delta == 0.0)
				delta = steps > 0 ? 1.0 : -1.0;
		}
		commit_value(value_ + delta);
	}

	void spinbox::nudge(int direction, int mods)
	{
		if (direction == 0 || !enabled_ || read_only_)
			return;

		double scale = 1.0;
		if (mods & VGUI_MOD_SHIFT)
			scale *= 0.1;
		if (mods & VGUI_MOD_ALT)
			scale *= 10.0;

		double delta = (double)direction * effective_step() * scale;
		if (type_ == value_type::integer)
		{
			delta = std::round(delta);
			if (delta == 0.0)
				delta = direction > 0 ? 1.0 : -1.0;
		}
		commit_value(value_ + delta);
	}

	void spinbox::set_read_only(bool read_only)
	{
		if (read_only_ == read_only)
			return;
		read_only_ = read_only;
		if (read_only_)
			cancel_drag();
		update_control_state();
	}

	bool spinbox::read_only() const noexcept
	{
		return read_only_;
	}

	void spinbox::set_enabled(bool enabled)
	{
		if (enabled_ == enabled)
			return;
		enabled_ = enabled;
		if (!enabled_)
			cancel_drag();
		update_control_state();
	}

	bool spinbox::enabled() const noexcept
	{
		return enabled_;
	}

	void spinbox::update_control_state()
	{
		if (panel_)
			vgui_widget_set_enabled(panel_, enabled_ ? 1 : 0);
		if (edit_)
		{
			vgui_widget_set_enabled(edit_, enabled_ ? 1 : 0);
			vgui_edit_set_readonly(edit_, read_only_ ? 1 : 0);
		}

		const bool editable = enabled_ && !read_only_;
		decrement_.set_enabled(editable);
		increment_.set_enabled(editable);
		if (label_widget_)
			vgui_widget_set_cursor(label_widget_,
				editable ? VGUI_CURSOR_SIZE_H : VGUI_CURSOR_ARROW);
	}

	void spinbox::set_error(std::string error)
	{
		error_ = std::move(error);
		update_error_ui();
	}

	const std::string& spinbox::error() const noexcept
	{
		return error_;
	}

	void spinbox::update_error_ui()
	{
		if (!edit_)
			return;
		const bool has_error = !error_.empty();
		vgui_widget_set_border(edit_, has_error ? kErrorColor : 0,
			has_error ? 1.0f : 0.0f);
		vgui_widget_set_tooltip(edit_, has_error ? error_.c_str() : "");
	}

	void spinbox::focus()
	{
		if (!edit_ || !enabled_)
			return;
		vgui_context* ctx = vgui_widget_get_context(edit_);
		if (ctx)
			vgui_set_focus(ctx, edit_);
	}

	void spinbox::set_on_change(value_listener listener)
	{
		change_listener_ = std::move(listener);
	}

	void spinbox::set_on_commit(value_listener listener)
	{
		commit_listener_ = std::move(listener);
	}

	void spinbox::notify_change()
	{
		if (change_listener_)
			change_listener_(value_);
	}

	void spinbox::notify_commit()
	{
		if (commit_listener_)
			commit_listener_(value_);
	}

	vgui_widget* spinbox::widget() const noexcept
	{
		return panel_;
	}

	vgui_widget* spinbox::edit_widget() const noexcept
	{
		return edit_;
	}

	vgui_widget* spinbox::label_widget() const noexcept
	{
		return label_widget_;
	}

	vgui_widget* spinbox::decrement_widget() const noexcept
	{
		return decrement_.widget();
	}

	vgui_widget* spinbox::increment_widget() const noexcept
	{
		return increment_.widget();
	}

	void spinbox::begin_drag(const vgui_event& event)
	{
		if (!enabled_ || read_only_)
			return;
		dragging_ = true;
		drag_start_x_ = event.x;
		drag_start_value_ = value_;
		drag_changed_ = false;
		error_.clear();
		update_error_ui();
	}

	void spinbox::update_drag(const vgui_event& event)
	{
		if (!dragging_)
			return;
		if (!enabled_ || read_only_)
		{
			cancel_drag();
			return;
		}

		double step = effective_step();
		const int mods = context_mods(edit_);
		if (mods & VGUI_MOD_SHIFT)
			step *= 0.1;
		if (mods & VGUI_MOD_ALT)
			step *= 10.0;

		double next = drag_start_value_ +
			(double)(event.x - drag_start_x_) * step;
		next = canonicalize(normalize_value(next, true));
		if (next == value_)
			return;

		value_ = next;
		drag_changed_ = true;
		sync_text();
		notify_change();
	}

	void spinbox::end_drag()
	{
		if (!dragging_)
			return;
		dragging_ = false;
		const bool changed = drag_changed_ && value_ != drag_start_value_;
		drag_changed_ = false;
		if (changed)
			notify_commit();
	}

	void spinbox::cancel_drag()
	{
		if (!dragging_)
			return;
		dragging_ = false;
		drag_changed_ = false;
		if (value_ != drag_start_value_)
		{
			value_ = drag_start_value_;
			sync_text();
			notify_change();
		}
	}

	void spinbox::on_label_event(vgui_widget* widget, const vgui_event* event,
		void* user)
	{
		auto* self = static_cast<spinbox*>(user);
		if (!self || !event || widget != self->label_widget_)
			return;

		if (event->type == VGUI_EVENT_MOUSE_DOWN && event->button == 1)
		{
			self->begin_drag(*event);
			return;
		}
		if (event->type == VGUI_EVENT_MOUSE_MOVE)
		{
			self->update_drag(*event);
			return;
		}
		if (event->type == VGUI_EVENT_MOUSE_UP && event->button == 1)
		{
			self->end_drag();
			return;
		}
		if (event->type == VGUI_EVENT_CLICK && event->button == 1)
			self->focus();
	}

	void spinbox::on_edit_event(vgui_widget* widget, const vgui_event* event,
		void* user)
	{
		auto* self = static_cast<spinbox*>(user);
		if (!self || !event || widget != self->edit_)
			return;

		if (event->type == VGUI_EVENT_SUBMIT)
		{
			const char* text = vgui_edit_get_text(widget);
			self->commit_text(text ? text : "");
		}
		else if (event->type == VGUI_EVENT_KEY_DOWN)
		{
			if (event->key == VGUI_KEY_ESCAPE)
			{
				self->error_.clear();
				self->update_error_ui();
				self->sync_text();
			}
			else if (event->key == VGUI_KEY_UP)
			{
				self->nudge(1, event->mods);
			}
			else if (event->key == VGUI_KEY_DOWN)
			{
				self->nudge(-1, event->mods);
			}
		}
	}

	void spinbox::on_edit_text(vgui_widget* widget, const char* text, void* user)
	{
		(void)text;
		auto* self = static_cast<spinbox*>(user);
		if (!self || !widget || widget != self->edit_)
			return;
		if (!self->error_.empty())
		{
			self->error_.clear();
			self->update_error_ui();
		}
	}

} /* namespace vgui_editor_kit */
