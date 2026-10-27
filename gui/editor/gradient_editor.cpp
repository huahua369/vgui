#include "gradient_editor.h"

#include "color_picker.h"
#include "spinbox.h"

#include "pch.h"
#include "tools/tools.hpp"
#include "ui_scale.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace vgui_editor_kit {

	namespace {
		constexpr vgui_color kPanelBg = 0xFF22262E;
		constexpr vgui_color kPanelBorder = 0xFF3A424E;
		constexpr vgui_color kHandleOutline = 0xFFE0B050;
		constexpr float kBarPadX = 6.0f;
		constexpr float kBarPadY = 4.0f;
		constexpr float kBarGradientH = 28.0f;
		constexpr float kStopRadius = 5.0f;

		struct preset_def
		{
			const char* name;
			const char* text;
		};

		const preset_def kPresets[] = {
			{ "Mono", "0,#000000FF;1,#FFFFFFFF" },
			{ "Fire", "0,#000000FF;0.4,#B22200FF;0.75,#FF9900FF;1,#FFFF66FF" },
			{ "Ocean", "0,#001B3AFF;0.5,#006699FF;1,#66E0FFFF" },
			{ "Rainbow", "0,#FF0000FF;0.25,#FFFF00FF;0.5,#00FF00FF;0.75,#00FFFFFF;1,#0000FFFF" }
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

		bool parse_float(const std::string& text, float* out)
		{
			const std::string trimmed = trim_copy(text);
			if (trimmed.empty())
				return false;
			errno = 0;
			char* end = nullptr;
			const float value = std::strtof(trimmed.c_str(), &end);
			if (errno == ERANGE || end == trimmed.c_str() || *end != '\0' ||
				!std::isfinite(value))
				return false;
			if (out)
				*out = value;
			return true;
		}

		std::string format_float(float value)
		{
			if (std::fabs(value) < 0.00005f)
				value = 0.0f;
			char buffer[32];
			std::snprintf(buffer, sizeof(buffer), "%.4f", (double)value);
			std::string text = buffer;
			while (!text.empty() && text.back() == '0')
				text.pop_back();
			if (!text.empty() && text.back() == '.')
				text.pop_back();
			return text.empty() ? std::string("0") : text;
		}

		std::vector<std::string> split(const std::string& text, char separator)
		{
			std::vector<std::string> parts;
			std::size_t begin = 0;
			while (begin <= text.size())
			{
				const std::size_t end = text.find(separator, begin);
				parts.push_back(text.substr(begin,
					end == std::string::npos ? std::string::npos : end - begin));
				if (end == std::string::npos)
					break;
				begin = end + 1;
			}
			return parts;
		}

		int hex_digit(char c)
		{
			if (c >= '0' && c <= '9') return c - '0';
			if (c >= 'a' && c <= 'f') return c - 'a' + 10;
			if (c >= 'A' && c <= 'F') return c - 'A' + 10;
			return -1;
		}

		/* #RRGGBB / #RRGGBBAA(可带 # 或 0x 前缀、3/4 位短写) -> vgui ARGB。 */
		bool parse_color(const std::string& text, uint32_t* out,
			std::string* canonical)
		{
			std::string digits = trim_copy(text);
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
			if (digits.size() != 3 && digits.size() != 4 &&
				digits.size() != 6 && digits.size() != 8)
				return false;
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
			const uint32_t argb = ((uint32_t)a << 24) | ((uint32_t)r << 16) |
				((uint32_t)g << 8) | (uint32_t)b;
			if (out)
				*out = argb;
			if (canonical)
			{
				char buffer[16];
				if (a == 255)
					std::snprintf(buffer, sizeof(buffer), "#%02X%02X%02X",
						r, g, b);
				else
					std::snprintf(buffer, sizeof(buffer), "#%02X%02X%02X%02X",
						r, g, b, a);
				*canonical = buffer;
			}
			return true;
		}

		uint32_t to_ovg(uint32_t argb)
		{
			const uint32_t a = argb & 0xFF000000u;
			const uint32_t r = (argb >> 16) & 0xFFu;
			const uint32_t g = (argb >> 8) & 0xFFu;
			const uint32_t b = argb & 0xFFu;
			return a | (b << 16) | (g << 8) | r;
		}

		bool normalize_gradient(gradient_value& value, std::string* error)
		{
			if (value.stops.size() < 2)
			{
				if (error)
					*error = "gradient needs at least two stops";
				return false;
			}
			if (value.stops.size() > 64)
			{
				if (error)
					*error = "gradient has more than 64 stops";
				return false;
			}
			for (gradient_stop& stop : value.stops)
			{
				if (!std::isfinite(stop.t))
				{
					if (error)
						*error = "gradient stop is not finite";
					return false;
				}
				stop.t = std::clamp(stop.t, 0.0f, 1.0f);
				std::string canonical;
				if (!parse_color(stop.color, nullptr, &canonical))
				{
					if (error)
						*error = "invalid gradient color";
					return false;
				}
				stop.color = std::move(canonical);
			}
			std::stable_sort(value.stops.begin(), value.stops.end(),
				[](const gradient_stop& a, const gradient_stop& b) {
					return a.t < b.t;
				});
			for (std::size_t i = 1; i < value.stops.size(); ++i)
			{
				if (value.stops[i].t - value.stops[i - 1].t < 1e-4f)
				{
					if (error)
						*error = "gradient stops need distinct times";
					return false;
				}
			}
			return true;
		}

		/* ARGB lerp for new stops. */
		bool sample_color(const gradient_value& value, float t, std::string* out)
		{
			if (value.stops.empty() || !out)
				return false;
			if (t <= value.stops.front().t)
			{
				*out = value.stops.front().color;
				return true;
			}
			if (t >= value.stops.back().t)
			{
				*out = value.stops.back().color;
				return true;
			}
			for (std::size_t i = 0; i + 1 < value.stops.size(); ++i)
			{
				const gradient_stop& a = value.stops[i];
				const gradient_stop& b = value.stops[i + 1];
				if (t < a.t || t > b.t)
					continue;
				uint32_t ca = 0, cb = 0;
				if (!parse_color(a.color, &ca, nullptr) ||
					!parse_color(b.color, &cb, nullptr))
					return false;
				const float span = std::max(1e-5f, b.t - a.t);
				const float k = std::clamp((t - a.t) / span, 0.0f, 1.0f);
				auto channel = [k](uint32_t x, uint32_t y, int shift) {
					const float v = (float)((x >> shift) & 0xFFu) +
						((float)((y >> shift) & 0xFFu) -
							(float)((x >> shift) & 0xFFu)) * k;
					return (uint32_t)std::lround(std::clamp(v, 0.0f, 255.0f));
				};
				const uint32_t mixed =
					(channel(ca, cb, 24) << 24) | (channel(ca, cb, 16) << 16) |
					(channel(ca, cb, 8) << 8) | channel(ca, cb, 0);
				char buffer[16];
				const uint32_t alpha = (mixed >> 24) & 0xFFu;
				if (alpha == 255)
					std::snprintf(buffer, sizeof(buffer), "#%02X%02X%02X",
						(unsigned)((mixed >> 16) & 0xFFu),
						(unsigned)((mixed >> 8) & 0xFFu),
						(unsigned)(mixed & 0xFFu));
				else
					std::snprintf(buffer, sizeof(buffer), "#%02X%02X%02X%02X",
						(unsigned)((mixed >> 16) & 0xFFu),
						(unsigned)((mixed >> 8) & 0xFFu),
						(unsigned)(mixed & 0xFFu), (unsigned)alpha);
				*out = buffer;
				return true;
			}
			*out = value.stops.back().color;
			return true;
		}

		void gradient_stops_ovg(const gradient_value& value,
			std::vector<ovg_tools::gradient_stop>& out)
		{
			out.clear();
			out.reserve(value.stops.size());
			for (const gradient_stop& stop : value.stops)
			{
				uint32_t argb = 0;
				if (!parse_color(stop.color, &argb, nullptr))
					continue;
				out.push_back({ stop.t, to_ovg(argb) });
			}
		}
	}

	gradient_editor::gradient_editor() = default;
	gradient_editor::~gradient_editor() = default;

	void gradient_editor::build(vgui_widget* parent)
	{
		if (root_ || !parent)
			return;
		vgui_context* ctx = vgui_widget_get_context(parent);
		if (!ctx)
			return;
		build(ctx);
		if (root_)
			vgui_widget_attach(parent, root_);
	}

	void gradient_editor::build(vgui_context* ctx)
	{
		if (root_ || !ctx)
			return;
		root_ = vgui_widget_create(ctx, VGUI_PANEL, nullptr);
		vgui_widget_set_layout(root_, VGUI_LAYOUT_COLUMN);
		vgui_widget_set_padding(root_, 8, 8, 8, 8);
		vgui_widget_set_gap(root_, 6.0f);
		vgui_widget_set_background(root_, kPanelBg);
		vgui_widget_set_border(root_, kPanelBorder, 1.0f);
		vgui_widget_set_min_size(root_, 300, 0);

		bar_ = vgui_widget_add(root_, VGUI_CUSTOM, nullptr);
		vgui_widget_set_size(bar_, 0, 50);
		vgui_widget_set_align_self(bar_, VGUI_ALIGN_STRETCH);
		vgui_widget_set_min_size(bar_, 220, 44);
		vgui_custom_set_draw(bar_, &gradient_editor::draw_bar, this);
		vgui_custom_set_event(bar_, &gradient_editor::on_bar_event, this);
		vgui_widget_set_tooltip(bar_,
			"Drag stops; double-click the bar to add a stop");

		stop_row_ = vgui_widget_add(root_, VGUI_PANEL, nullptr);
		vgui_widget_set_layout(stop_row_, VGUI_LAYOUT_ROW);
		vgui_widget_set_gap(stop_row_, 4.0f);
		vgui_widget_set_size(stop_row_, 0, 24);
		vgui_widget_set_align_self(stop_row_, VGUI_ALIGN_STRETCH);

		time_spin_ = std::make_unique<spinbox>();
		time_spin_->build(stop_row_, "T");
		time_spin_->set_value_type(spinbox::value_type::number);
		time_spin_->set_range(0.0, 1.0);
		time_spin_->set_step(0.01);
		time_spin_->set_label_width(24.0f);
		time_spin_->set_buttons_visible(false);
		time_spin_->set_on_change([this](double value) {
			on_time_change(value);
		});
		time_spin_->set_on_commit([this](double) { on_time_commit(); });
		vgui_widget_set_grow(time_spin_->widget(), 1.0f);
		vgui_widget_set_size(time_spin_->widget(), 0, 24);
		vgui_widget_set_tooltip(time_spin_->edit_widget(), "Selected stop time");

		add_ = vgui_widget_add(stop_row_, VGUI_BUTTON, "Add");
		vgui_widget_set_size(add_, 48, 24);
		vgui_widget_set_font_size(add_, 12);
		vgui_widget_on_event(add_, &gradient_editor::on_add_click, this);
		vgui_widget_set_tooltip(add_, "Add a stop with the sampled color");

		remove_ = vgui_widget_add(stop_row_, VGUI_BUTTON, "Del");
		vgui_widget_set_size(remove_, 48, 24);
		vgui_widget_set_font_size(remove_, 12);
		vgui_widget_on_event(remove_, &gradient_editor::on_remove_click, this);
		vgui_widget_set_tooltip(remove_, "Delete the selected stop");

		preset_row_ = vgui_widget_add(root_, VGUI_PANEL, nullptr);
		vgui_widget_set_layout(preset_row_, VGUI_LAYOUT_WRAP);
		vgui_widget_set_gap(preset_row_, 4.0f);
		vgui_widget_set_align_self(preset_row_, VGUI_ALIGN_STRETCH);
		for (std::size_t i = 0; i < preset_count(); ++i)
		{
			presets_[i] = vgui_widget_add(preset_row_, VGUI_BUTTON,
				preset_name(i));
			vgui_widget_set_size(presets_[i], 68, 22);
			vgui_widget_set_font_size(presets_[i], 11);
			vgui_widget_on_event(presets_[i], &gradient_editor::on_preset_click,
				this);
		}

		picker_ = std::make_unique<color_picker>();
		picker_->set_id(id_ + ".picker");
		picker_->build(ctx);
		picker_->set_on_press([this](const std::string& color) {
			on_picker_press(color);
		});
		picker_->set_on_change([this](const std::string& color) {
			on_picker_change(color);
		});
		picker_->set_on_commit([this](const std::string& color) {
			on_picker_commit(color);
		});
		if (picker_->widget())
		{
			vgui_widget_set_align_self(picker_->widget(), VGUI_ALIGN_STRETCH);
			vgui_widget_attach(root_, picker_->widget());
		}

		value_.stops = {
			{ 0.0f, "#000000" },
			{ 1.0f, "#FFFFFF" }
		};
		selected_ = 0;
		apply_ids();
		sync_controls();
	}

	void gradient_editor::set_id(std::string id)
	{
		id_ = std::move(id);
		apply_ids();
	}

	const std::string& gradient_editor::id() const noexcept
	{
		return id_;
	}

	vgui_widget* gradient_editor::widget() const noexcept
	{
		return root_;
	}

	void gradient_editor::apply_ids()
	{
		if (!root_)
			return;
		vgui_widget_set_id(root_, id_.c_str());
		if (bar_)
			vgui_widget_set_id(bar_, (id_ + ".bar").c_str());
		if (add_)
			vgui_widget_set_id(add_, (id_ + ".add").c_str());
		if (remove_)
			vgui_widget_set_id(remove_, (id_ + ".remove").c_str());
		if (time_spin_)
			time_spin_->set_id(id_ + ".time");
		for (std::size_t i = 0; i < preset_count(); ++i)
		{
			if (presets_[i])
				vgui_widget_set_id(presets_[i],
					(id_ + ".preset." + std::to_string(i)).c_str());
		}
		if (picker_ && picker_->widget())
			vgui_widget_set_id(picker_->widget(), (id_ + ".picker").c_str());
	}

	bool gradient_editor::set_value(const gradient_value& value)
	{
		gradient_value normalized = value;
		std::string error;
		if (!normalize_gradient(normalized, &error))
			return false;
		value_ = std::move(normalized);
		select_clamped(selected_);
		sync_controls();
		return true;
	}

	bool gradient_editor::set_value_text(const std::string& text,
		std::string* error)
	{
		gradient_value parsed;
		if (!parse_value(text, parsed, error))
			return false;
		return set_value(parsed);
	}

	const gradient_value& gradient_editor::value() const noexcept
	{
		return value_;
	}

	std::string gradient_editor::value_text() const
	{
		return format_value(value_);
	}

	void gradient_editor::set_read_only(bool read_only)
	{
		read_only_ = read_only;
		sync_controls();
	}

	bool gradient_editor::read_only() const noexcept
	{
		return read_only_;
	}

	void gradient_editor::set_enabled(bool enabled)
	{
		enabled_ = enabled;
		if (root_)
			vgui_widget_set_enabled(root_, enabled ? 1 : 0);
		sync_controls();
	}

	bool gradient_editor::enabled() const noexcept
	{
		return enabled_;
	}

	void gradient_editor::select_clamped(int index)
	{
		if (value_.stops.empty())
			selected_ = 0;
		else
			selected_ = std::clamp(index, 0, (int)value_.stops.size() - 1);
	}

	bool gradient_editor::select_stop(int index)
	{
		if (index < 0 || (std::size_t)index >= value_.stops.size())
			return false;
		if (selected_ == index)
			return true;
		selected_ = index;
		sync_controls();
		return true;
	}

	int gradient_editor::selected_stop() const noexcept
	{
		return selected_;
	}

	void gradient_editor::sync_controls()
	{
		if (!root_)
			return;
		syncing_ = true;
		const bool editable = enabled_ && !read_only_;
		const bool has_stop = !value_.stops.empty() && selected_ >= 0 &&
			(std::size_t)selected_ < value_.stops.size();
		if (bar_)
			vgui_widget_set_enabled(bar_, editable ? 1 : 0);
		if (time_spin_)
		{
			time_spin_->set_enabled(editable && has_stop);
			time_spin_->set_read_only(read_only_);
			if (has_stop)
				time_spin_->set_value(value_.stops[(std::size_t)selected_].t);
		}
		if (add_)
			vgui_widget_set_enabled(add_, editable ? 1 : 0);
		if (remove_)
			vgui_widget_set_enabled(remove_,
				editable && value_.stops.size() > 2 ? 1 : 0);
		for (std::size_t i = 0; i < preset_count(); ++i)
		{
			if (presets_[i])
				vgui_widget_set_enabled(presets_[i], editable ? 1 : 0);
		}
		if (picker_ && picker_->widget())
		{
			vgui_widget_set_enabled(picker_->widget(), editable ? 1 : 0);
			if (has_stop)
			{
				picker_->set_value(value_.stops[(std::size_t)selected_].color);
				picker_->set_current(value_.stops[(std::size_t)selected_].color);
			}
		}
		syncing_ = false;
	}

	void gradient_editor::begin_gesture()
	{
		if (gesture_active_)
			return;
		gesture_active_ = true;
		if (press_listener_)
			press_listener_(value_);
	}

	void gradient_editor::end_gesture()
	{
		if (!gesture_active_)
			return;
		gesture_active_ = false;
		if (commit_listener_)
			commit_listener_(value_);
	}

	void gradient_editor::notify_change()
	{
		if (change_listener_)
			change_listener_(value_);
	}

	void gradient_editor::set_on_press(value_listener listener)
	{
		press_listener_ = std::move(listener);
	}

	void gradient_editor::set_on_change(value_listener listener)
	{
		change_listener_ = std::move(listener);
	}

	void gradient_editor::set_on_commit(value_listener listener)
	{
		commit_listener_ = std::move(listener);
	}

	vgui_widget* gradient_editor::bar_widget() const noexcept
	{
		return bar_;
	}

	vgui_widget* gradient_editor::add_widget() const noexcept
	{
		return add_;
	}

	vgui_widget* gradient_editor::remove_widget() const noexcept
	{
		return remove_;
	}

	vgui_widget* gradient_editor::time_widget() const noexcept
	{
		return time_spin_ ? time_spin_->widget() : nullptr;
	}

	vgui_widget* gradient_editor::preset_widget(std::size_t index) const noexcept
	{
		return index < preset_count() ? presets_[index] : nullptr;
	}

	color_picker* gradient_editor::picker() const noexcept
	{
		return picker_.get();
	}

	std::size_t gradient_editor::preset_count() const noexcept
	{
		return sizeof(kPresets) / sizeof(kPresets[0]);
	}

	const char* gradient_editor::preset_name(std::size_t index) const noexcept
	{
		return index < preset_count() ? kPresets[index].name : "";
	}

	bool gradient_editor::commit_preset(std::size_t index)
	{
		if (read_only_ || !enabled_ || index >= preset_count())
			return false;
		gradient_value next;
		if (!parse_value(kPresets[index].text, next))
			return false;
		if (format_value(next) == format_value(value_))
			return false;
		return commit_value(std::move(next));
	}

	bool gradient_editor::commit_value(gradient_value next)
	{
		if (read_only_ || !enabled_)
			return false;
		if (!normalize_gradient(next, nullptr))
			return false;
		begin_gesture();
		value_ = std::move(next);
		select_clamped(selected_);
		sync_controls();
		notify_change();
		end_gesture();
		return true;
	}

	bool gradient_editor::add_stop(float t)
	{
		if (read_only_ || !enabled_ || value_.stops.size() >= 64)
			return false;
		t = std::clamp(t, 0.0f, 1.0f);
		for (const gradient_stop& stop : value_.stops)
		{
			if (std::fabs(stop.t - t) < 1e-4f)
				return false;
		}
		std::string color;
		if (!sample_color(value_, t, &color))
			return false;
		begin_gesture();
		gradient_stop stop;
		stop.t = t;
		stop.color = std::move(color);
		value_.stops.push_back(stop);
		std::stable_sort(value_.stops.begin(), value_.stops.end(),
			[](const gradient_stop& a, const gradient_stop& b) {
				return a.t < b.t;
			});
		for (std::size_t i = 0; i < value_.stops.size(); ++i)
		{
			if (std::fabs(value_.stops[i].t - t) < 1e-5f)
			{
				selected_ = (int)i;
				break;
			}
		}
		sync_controls();
		notify_change();
		end_gesture();
		return true;
	}

	bool gradient_editor::remove_selected_stop()
	{
		if (read_only_ || !enabled_ || value_.stops.size() <= 2 ||
			selected_ < 0 || (std::size_t)selected_ >= value_.stops.size())
			return false;
		begin_gesture();
		value_.stops.erase(value_.stops.begin() + selected_);
		select_clamped(selected_);
		sync_controls();
		notify_change();
		end_gesture();
		return true;
	}

	bool gradient_editor::set_selected_color_internal(const std::string& color)
	{
		if (selected_ < 0 || (std::size_t)selected_ >= value_.stops.size())
			return false;
		std::string canonical;
		if (!parse_color(color, nullptr, &canonical))
			return false;
		if (value_.stops[(std::size_t)selected_].color == canonical)
			return false;
		value_.stops[(std::size_t)selected_].color = std::move(canonical);
		return true;
	}

	bool gradient_editor::set_selected_color(const std::string& color)
	{
		if (read_only_ || !enabled_)
			return false;
		std::string canonical;
		if (!parse_color(color, nullptr, &canonical))
			return false;
		if (selected_ < 0 || (std::size_t)selected_ >= value_.stops.size() ||
			value_.stops[(std::size_t)selected_].color == canonical)
			return false;
		begin_gesture();
		value_.stops[(std::size_t)selected_].color = std::move(canonical);
		sync_controls();
		notify_change();
		end_gesture();
		return true;
	}

	float gradient_editor::bar_t(float x) const
	{
		if (!bar_)
			return 0.0f;
		float rx = 0, ry = 0, rw = 0, rh = 0;
		vgui_widget_get_rect(bar_, &rx, &ry, &rw, &rh);
		const float pad = kBarPadX * ui_scale_of(bar_);
		const float left = pad;
		const float width = std::max(1.0f, rw - pad * 2.0f);
		return std::clamp((x - left) / width, 0.0f, 1.0f);
	}

	int gradient_editor::stop_at(float x) const
	{
		if (!bar_ || value_.stops.empty())
			return -1;
		float rx = 0, ry = 0, rw = 0, rh = 0;
		vgui_widget_get_rect(bar_, &rx, &ry, &rw, &rh);
		const float scale = ui_scale_of(bar_);
		const float pad = kBarPadX * scale;
		const float width = std::max(1.0f, rw - pad * 2.0f);
		int best = -1;
		float best_distance = 9.0f * scale;
		for (std::size_t i = 0; i < value_.stops.size(); ++i)
		{
			const float sx = pad + value_.stops[i].t * width;
			const float distance = std::fabs(x - sx);
			if (distance < best_distance)
			{
				best_distance = distance;
				best = (int)i;
			}
		}
		return best;
	}

	void gradient_editor::on_time_change(double value)
	{
		if (syncing_ || read_only_ || !enabled_ || selected_ < 0 ||
			(std::size_t)selected_ >= value_.stops.size())
			return;
		begin_gesture();
		float t = (float)value;
		const int index = selected_;
		if (index > 0)
			t = std::max(t, value_.stops[(std::size_t)index - 1].t + 1e-3f);
		if ((std::size_t)index + 1 < value_.stops.size())
			t = std::min(t, value_.stops[(std::size_t)index + 1].t - 1e-3f);
		t = std::clamp(t, 0.0f, 1.0f);
		value_.stops[(std::size_t)index].t = t;
		if (time_spin_)
			time_spin_->set_value(t);
		notify_change();
	}

	void gradient_editor::on_time_commit()
	{
		if (read_only_ || !enabled_)
			return;
		end_gesture();
	}

	void gradient_editor::on_picker_press(const std::string& color)
	{
		(void)color;
		if (syncing_ || read_only_ || !enabled_)
			return;
		begin_gesture();
	}

	void gradient_editor::on_picker_change(const std::string& color)
	{
		if (syncing_ || read_only_ || !enabled_)
			return;
		begin_gesture();
		if (!set_selected_color_internal(color))
			return;
		notify_change();
	}

	void gradient_editor::on_picker_commit(const std::string& color)
	{
		if (syncing_ || read_only_ || !enabled_)
			return;
		(void)set_selected_color_internal(color);
		end_gesture();
	}

	int gradient_editor::on_bar_event(vgui_widget* widget, const vgui_event* event,
		void* user)
	{
		auto* self = static_cast<gradient_editor*>(user);
		if (!self || !event || !widget)
			return 0;
		if (self->read_only_ || !self->enabled_)
			return 0;
		if (event->type == VGUI_EVENT_DOUBLE_CLICK && event->button == 1)
		{
			(void)self->add_stop(self->bar_t(event->x));
			return 1;
		}
		if (event->type == VGUI_EVENT_MOUSE_DOWN && event->button == 1)
		{
			const int index = self->stop_at(event->x);
			if (index < 0)
				return 0;
			self->selected_ = index;
			self->drag_ = drag_kind::stop;
			self->begin_gesture();
			self->sync_controls();
			return 1;
		}
		if (event->type == VGUI_EVENT_MOUSE_MOVE &&
			self->drag_ == drag_kind::stop)
		{
			const int index = self->selected_;
			if (index < 0 || (std::size_t)index >= self->value_.stops.size())
				return 0;
			float t = self->bar_t(event->x);
			if (index > 0)
				t = std::max(t,
					self->value_.stops[(std::size_t)index - 1].t + 1e-3f);
			if ((std::size_t)index + 1 < self->value_.stops.size())
				t = std::min(t,
					self->value_.stops[(std::size_t)index + 1].t - 1e-3f);
			t = std::clamp(t, 0.0f, 1.0f);
			self->value_.stops[(std::size_t)index].t = t;
			if (self->time_spin_)
				self->time_spin_->set_value(t);
			self->notify_change();
			return 1;
		}
		if (event->type == VGUI_EVENT_MOUSE_UP &&
			self->drag_ == drag_kind::stop)
		{
			self->drag_ = drag_kind::none;
			self->end_gesture();
			return 1;
		}
		return 0;
	}

	void gradient_editor::on_add_click(vgui_widget* widget,
		const vgui_event* event, void* user)
	{
		(void)widget;
		if (!event || event->type != VGUI_EVENT_CLICK)
			return;
		auto* self = static_cast<gradient_editor*>(user);
		if (!self || self->value_.stops.size() < 2)
			return;
		std::size_t best = 0;
		float best_gap = -1.0f;
		for (std::size_t i = 0; i + 1 < self->value_.stops.size(); ++i)
		{
			const float gap = self->value_.stops[i + 1].t -
				self->value_.stops[i].t;
			if (gap > best_gap)
			{
				best_gap = gap;
				best = i;
			}
		}
		const float t = (self->value_.stops[best].t +
			self->value_.stops[best + 1].t) * 0.5f;
		(void)self->add_stop(t);
	}

	void gradient_editor::on_remove_click(vgui_widget* widget,
		const vgui_event* event, void* user)
	{
		(void)widget;
		if (!event || event->type != VGUI_EVENT_CLICK)
			return;
		auto* self = static_cast<gradient_editor*>(user);
		if (self)
			(void)self->remove_selected_stop();
	}

	void gradient_editor::on_preset_click(vgui_widget* widget,
		const vgui_event* event, void* user)
	{
		if (!event || event->type != VGUI_EVENT_CLICK)
			return;
		auto* self = static_cast<gradient_editor*>(user);
		if (!self)
			return;
		for (std::size_t i = 0; i < self->preset_count(); ++i)
		{
			if (widget == self->presets_[i])
			{
				(void)self->commit_preset(i);
				return;
			}
		}
	}

	void gradient_editor::draw_bar(vgui_widget* widget, ovg_ctx_cb* cb,
		rvg_t* vg, float x, float y, float w, float h, float dt, void* user)
	{
		(void)widget;
		(void)dt;
		auto* self = static_cast<gradient_editor*>(user);
		if (!self || !cb || !vg || w <= 0.0f || h <= 0.0f)
			return;
		const float scale = ui_scale_of(widget);
		const float pad_x = kBarPadX * scale;
		const float pad_y = kBarPadY * scale;
		const float bar_x = x + pad_x;
		const float bar_y = y + pad_y;
		const float bar_w = std::max(1.0f, w - pad_x * 2.0f);
		const float bar_h = kBarGradientH * scale;
		ovg_tools::checkerboard(cb, vg, bar_x, bar_y, bar_w, bar_h,
			6.0f * scale, 0xFFCCCCCCu, 0xFFF2F2F2u);
		std::vector<ovg_tools::gradient_stop> stops;
		gradient_stops_ovg(self->value_, stops);
		if (!stops.empty())
			ovg_tools::gradient_bar(cb, vg, bar_x, bar_y, bar_w, bar_h,
				stops.data(), stops.size());
		ovg_tools::stroke_rect(cb, vg, bar_x, bar_y, bar_w, bar_h,
			to_ovg(kPanelBorder), 1.0f * scale);

		const float handle_y = y + h - 8.0f * scale;
		const float stop_radius = kStopRadius * scale;
		for (std::size_t i = 0; i < self->value_.stops.size(); ++i)
		{
			const gradient_stop& stop = self->value_.stops[i];
			const float sx = bar_x + stop.t * bar_w;
			ovg_tools::line(cb, vg, sx, bar_y + bar_h, sx,
				handle_y - 5.0f * scale, to_ovg(kPanelBorder), 1.0f * scale);
			uint32_t argb = 0;
			if (!parse_color(stop.color, &argb, nullptr))
				argb = 0xFF808080u;
			ovg_tools::fill_circle(cb, vg, sx, handle_y, stop_radius,
				to_ovg(argb));
			const bool selected = (int)i == self->selected_;
			ovg_tools::stroke_circle(cb, vg, sx, handle_y, stop_radius,
				selected ? to_ovg(kHandleOutline) : to_ovg(0xFFFFFFFFu),
				(selected ? 2.0f : 1.0f) * scale);
		}
	}

	void gradient_editor::draw_preview(ovg_ctx_cb* cb, rvg_t* vg, float x,
		float y, float w, float h, const gradient_value& value, bool mixed,
		float ui_scale)
	{
		if (!cb || !vg || w <= 0.0f || h <= 0.0f)
			return;
		const float scale = std::isfinite(ui_scale) && ui_scale > 0.0f
			? ui_scale : 1.0f;
		if (mixed)
		{
			ovg_tools::fill_round_rect(cb, vg, x, y, w, h, 3.0f * scale,
				ovg_tools::rgba(80, 86, 92));
			return;
		}
		ovg_tools::checkerboard(cb, vg, x, y, w, h, 5.0f * scale,
			0xFFCCCCCCu, 0xFFF2F2F2u);
		std::vector<ovg_tools::gradient_stop> stops;
		gradient_stops_ovg(value, stops);
		if (!stops.empty())
			ovg_tools::gradient_bar(cb, vg, x, y, w, h, stops.data(),
				stops.size());
		ovg_tools::stroke_rect(cb, vg, x, y, w, h, to_ovg(kPanelBorder),
			1.0f * scale);
	}

	bool gradient_editor::parse_value(const std::string& text,
		gradient_value& out, std::string* error)
	{
		const std::string trimmed = trim_copy(text);
		if (trimmed.empty())
		{
			if (error)
				*error = "empty gradient";
			return false;
		}
		gradient_value parsed;
		for (const std::string& raw : split(trimmed, ';'))
		{
			const std::string token = trim_copy(raw);
			if (token.empty())
				continue;
			const std::vector<std::string> fields = split(token, ',');
			if (fields.size() != 2)
			{
				if (error)
					*error = "gradient stop needs t,color";
				return false;
			}
			gradient_stop stop;
			if (!parse_float(fields[0], &stop.t))
			{
				if (error)
					*error = "invalid gradient time";
				return false;
			}
			std::string canonical;
			if (!parse_color(fields[1], nullptr, &canonical))
			{
				if (error)
					*error = "invalid gradient color";
				return false;
			}
			stop.color = std::move(canonical);
			parsed.stops.push_back(std::move(stop));
		}
		if (!normalize_gradient(parsed, error))
			return false;
		out = std::move(parsed);
		return true;
	}

	std::string gradient_editor::format_value(const gradient_value& value)
	{
		std::string text;
		for (const gradient_stop& stop : value.stops)
		{
			if (!text.empty())
				text += ';';
			text += format_float(stop.t);
			text += ',';
			text += stop.color;
		}
		return text;
	}

} /* namespace vgui_editor_kit */
