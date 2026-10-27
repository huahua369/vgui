#include "curve_editor.h"

#include "spinbox.h"

#include "pch.h"
#include "tools/tools.hpp"
#include "ui_scale.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <system_error>

namespace vgui_editor_kit {

	namespace {
		constexpr vgui_color kPanelBg = 0xFF22262E;
		constexpr vgui_color kPanelBorder = 0xFF3A424E;
		constexpr vgui_color kGraphBg = 0xFF161A20;
		constexpr vgui_color kGrid = 0xFF2C333D;
		constexpr float kGraphMargin = 10.0f;

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

		bool parse_mode(const std::string& text, curve_interpolation* out)
		{
			const std::string lowered = lower_copy(trim_copy(text));
			if (lowered == "linear")
				*out = curve_interpolation::linear;
			else if (lowered == "step")
				*out = curve_interpolation::step;
			else if (lowered == "cubic" || lowered == "bezier" ||
				lowered == "spline")
				*out = curve_interpolation::cubic;
			else
				return false;
			return true;
		}

		bool normalize_curve(curve_value& value, std::string* error)
		{
			if (value.keys.size() < 2)
			{
				if (error)
					*error = "curve needs at least two keys";
				return false;
			}
			if (value.keys.size() > 64)
			{
				if (error)
					*error = "curve has more than 64 keys";
				return false;
			}
			for (curve_key& key : value.keys)
			{
				if (!std::isfinite(key.t) || !std::isfinite(key.value) ||
					!std::isfinite(key.in_tangent) ||
					!std::isfinite(key.out_tangent))
				{
					if (error)
						*error = "curve key is not finite";
					return false;
				}
				key.t = std::clamp(key.t, 0.0f, 1.0f);
				key.value = std::clamp(key.value, 0.0f, 1.0f);
				key.in_tangent = std::clamp(key.in_tangent, -20.0f, 20.0f);
				key.out_tangent = std::clamp(key.out_tangent, -20.0f, 20.0f);
			}
			std::stable_sort(value.keys.begin(), value.keys.end(),
				[](const curve_key& a, const curve_key& b) {
					return a.t < b.t;
				});
			for (std::size_t i = 1; i < value.keys.size(); ++i)
			{
				if (value.keys[i].t - value.keys[i - 1].t < 1e-4f)
				{
					if (error)
						*error = "curve keys need distinct times";
					return false;
				}
			}
			return true;
		}

		uint32_t to_ovg(uint32_t vgui_color_value)
		{
			const uint32_t a = vgui_color_value & 0xFF000000u;
			const uint32_t r = (vgui_color_value >> 16) & 0xFFu;
			const uint32_t g = (vgui_color_value >> 8) & 0xFFu;
			const uint32_t b = vgui_color_value & 0xFFu;
			return a | (b << 16) | (g << 8) | r;
		}

		/* t/value graph -> pixel.  x/y are the top-left of the graph. */
		void map_key(const curve_value& value, const curve_key& key,
			float x, float y, float w, float h, float* out_x, float* out_y)
		{
			*out_x = x + key.t * w;
			*out_y = y + (1.0f - key.value) * h;
		}

		void draw_curve_path(ovg_ctx_cb* cb, rvg_t* vg,
			const curve_value& value, float x, float y, float w, float h,
			uint32_t color, float line_width)
		{
			if (!cb || !vg || value.keys.size() < 2 || w <= 0.0f || h <= 0.0f)
				return;
			(void)cb;
			for (std::size_t i = 0; i + 1 < value.keys.size(); ++i)
			{
				const curve_key& a = value.keys[i];
				const curve_key& b = value.keys[i + 1];
				float ax = 0, ay = 0, bx = 0, by = 0;
				map_key(value, a, x, y, w, h, &ax, &ay);
				map_key(value, b, x, y, w, h, &bx, &by);
				cb->new_path(vg);
				cb->move_to(vg, ax, ay);
				const float dt = b.t - a.t;
				switch (value.interpolation)
				{
				case curve_interpolation::step:
					cb->line_to(vg, bx, ay);
					cb->line_to(vg, bx, by);
					break;
				case curve_interpolation::cubic:
				{
					const float dx = dt * w / 3.0f;
					const float y1 = y + (1.0f - (a.value + a.out_tangent * dt / 3.0f)) * h;
					const float y2 = y + (1.0f - (b.value - b.in_tangent * dt / 3.0f)) * h;
					cb->curve_to(vg, ax + dx, y1, bx - dx, y2, bx, by);
					break;
				}
				default:
					cb->line_to(vg, bx, by);
					break;
				}
				cb->set_line_width(vg, line_width);
				cb->set_line_cap(vg, VG_LINE_CAP_ROUND);
				cb->set_line_join(vg, VG_LINE_JOIN_ROUND);
				cb->set_source_color(vg, color);
				cb->stroke(vg);
			}
		}
	}

	curve_editor::~curve_editor() = default;

	void curve_editor::build(vgui_widget* parent)
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

	void curve_editor::build(vgui_context* ctx)
	{
		if (root_ || !ctx)
			return;
		root_ = vgui_widget_create(ctx, VGUI_PANEL, nullptr);
		vgui_widget_set_layout(root_, VGUI_LAYOUT_COLUMN);
		vgui_widget_set_padding(root_, 8, 8, 8, 8);
		vgui_widget_set_gap(root_, 6.0f);
		vgui_widget_set_background(root_, kPanelBg);
		vgui_widget_set_border(root_, kPanelBorder, 1.0f);
		vgui_widget_set_min_size(root_, 320, 0);

		mode_row_ = vgui_widget_add(root_, VGUI_PANEL, nullptr);
		vgui_widget_set_layout(mode_row_, VGUI_LAYOUT_ROW);
		vgui_widget_set_gap(mode_row_, 4.0f);
		vgui_widget_set_size(mode_row_, 0, 24);
		vgui_widget_set_align_self(mode_row_, VGUI_ALIGN_STRETCH);

		for (int i = 0; i < 3; ++i)
		{
			const curve_interpolation mode = (curve_interpolation)i;
			mode_buttons_[i] = vgui_widget_add(mode_row_, VGUI_BUTTON,
				interpolation_label(mode));
			vgui_widget_set_size(mode_buttons_[i], 76, 24);
			vgui_widget_set_font_size(mode_buttons_[i], 12);
			vgui_widget_on_event(mode_buttons_[i], &curve_editor::on_mode_click,
				this);
			vgui_widget_set_tooltip(mode_buttons_[i],
				"Curve interpolation mode");
		}

		graph_ = vgui_widget_add(root_, VGUI_CUSTOM, nullptr);
		vgui_widget_set_size(graph_, 0, 168);
		vgui_widget_set_align_self(graph_, VGUI_ALIGN_STRETCH);
		vgui_widget_set_min_size(graph_, 240, 120);
		vgui_custom_set_draw(graph_, &curve_editor::draw_graph, this);
		vgui_custom_set_event(graph_, &curve_editor::on_graph_event, this);
		vgui_widget_set_tooltip(graph_,
			"Double-click to add a key; drag keys and tangent handles");

		key_row_ = vgui_widget_add(root_, VGUI_PANEL, nullptr);
		vgui_widget_set_layout(key_row_, VGUI_LAYOUT_ROW);
		vgui_widget_set_gap(key_row_, 4.0f);
		vgui_widget_set_size(key_row_, 0, 24);
		vgui_widget_set_align_self(key_row_, VGUI_ALIGN_STRETCH);

		time_spin_ = std::make_unique<spinbox>();
		time_spin_->build(key_row_, "T");
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

		value_spin_ = std::make_unique<spinbox>();
		value_spin_->build(key_row_, "V");
		value_spin_->set_value_type(spinbox::value_type::number);
		value_spin_->set_range(0.0, 1.0);
		value_spin_->set_step(0.01);
		value_spin_->set_label_width(24.0f);
		value_spin_->set_buttons_visible(false);
		value_spin_->set_on_change([this](double value) {
			on_value_change(value);
		});
		value_spin_->set_on_commit([this](double) { on_value_commit(); });
		vgui_widget_set_grow(value_spin_->widget(), 1.0f);
		vgui_widget_set_size(value_spin_->widget(), 0, 24);

		add_ = vgui_widget_add(key_row_, VGUI_BUTTON, "Add");
		vgui_widget_set_size(add_, 48, 24);
		vgui_widget_set_font_size(add_, 12);
		vgui_widget_on_event(add_, &curve_editor::on_add_click, this);
		vgui_widget_set_tooltip(add_, "Add a key in the largest gap");

		remove_ = vgui_widget_add(key_row_, VGUI_BUTTON, "Del");
		vgui_widget_set_size(remove_, 48, 24);
		vgui_widget_set_font_size(remove_, 12);
		vgui_widget_on_event(remove_, &curve_editor::on_remove_click, this);
		vgui_widget_set_tooltip(remove_, "Delete the selected key");

		tangent_row_ = vgui_widget_add(root_, VGUI_PANEL, nullptr);
		vgui_widget_set_layout(tangent_row_, VGUI_LAYOUT_ROW);
		vgui_widget_set_gap(tangent_row_, 4.0f);
		vgui_widget_set_size(tangent_row_, 0, 24);
		vgui_widget_set_align_self(tangent_row_, VGUI_ALIGN_STRETCH);

		in_spin_ = std::make_unique<spinbox>();
		in_spin_->build(tangent_row_, "In");
		in_spin_->set_value_type(spinbox::value_type::number);
		in_spin_->set_range(-20.0, 20.0);
		in_spin_->set_step(0.05);
		in_spin_->set_label_width(24.0f);
		in_spin_->set_buttons_visible(false);
		in_spin_->set_on_change([this](double value) {
			on_in_change(value);
		});
		in_spin_->set_on_commit([this](double) { on_in_commit(); });
		vgui_widget_set_grow(in_spin_->widget(), 1.0f);
		vgui_widget_set_size(in_spin_->widget(), 0, 24);
		vgui_widget_set_tooltip(in_spin_->edit_widget(),
			"Incoming tangent slope");

		out_spin_ = std::make_unique<spinbox>();
		out_spin_->build(tangent_row_, "Out");
		out_spin_->set_value_type(spinbox::value_type::number);
		out_spin_->set_range(-20.0, 20.0);
		out_spin_->set_step(0.05);
		out_spin_->set_label_width(24.0f);
		out_spin_->set_buttons_visible(false);
		out_spin_->set_on_change([this](double value) {
			on_out_change(value);
		});
		out_spin_->set_on_commit([this](double) { on_out_commit(); });
		vgui_widget_set_grow(out_spin_->widget(), 1.0f);
		vgui_widget_set_size(out_spin_->widget(), 0, 24);
		vgui_widget_set_tooltip(out_spin_->edit_widget(),
			"Outgoing tangent slope");

		value_.interpolation = curve_interpolation::cubic;
		value_.keys = {
			{ 0.0f, 0.0f, 0.0f, 0.0f },
			{ 1.0f, 1.0f, 0.0f, 0.0f }
		};
		selected_ = 0;
		apply_ids();
		sync_controls();
	}

	void curve_editor::set_id(std::string id)
	{
		id_ = std::move(id);
		apply_ids();
	}

	const std::string& curve_editor::id() const noexcept
	{
		return id_;
	}

	vgui_widget* curve_editor::widget() const noexcept
	{
		return root_;
	}

	void curve_editor::apply_ids()
	{
		if (!root_)
			return;
		vgui_widget_set_id(root_, id_.c_str());
		for (int i = 0; i < 3; ++i)
		{
			if (!mode_buttons_[i])
				continue;
			const std::string widget_id = id_ + ".mode." + interpolation_name(
				(curve_interpolation)i);
			vgui_widget_set_id(mode_buttons_[i], widget_id.c_str());
		}
		if (graph_)
			vgui_widget_set_id(graph_, (id_ + ".graph").c_str());
		if (add_)
			vgui_widget_set_id(add_, (id_ + ".add").c_str());
		if (remove_)
			vgui_widget_set_id(remove_, (id_ + ".remove").c_str());
		if (time_spin_)
			time_spin_->set_id(id_ + ".time");
		if (value_spin_)
			value_spin_->set_id(id_ + ".value");
		if (in_spin_)
			in_spin_->set_id(id_ + ".in");
		if (out_spin_)
			out_spin_->set_id(id_ + ".out");
	}

	bool curve_editor::set_value(const curve_value& value)
	{
		curve_value normalized = value;
		std::string error;
		if (!normalize_curve(normalized, &error))
			return false;
		value_ = std::move(normalized);
		select_clamped(selected_);
		sync_controls();
		return true;
	}

	bool curve_editor::set_value_text(const std::string& text, std::string* error)
	{
		curve_value parsed;
		if (!parse_value(text, parsed, error))
			return false;
		return set_value(parsed);
	}

	const curve_value& curve_editor::value() const noexcept
	{
		return value_;
	}

	std::string curve_editor::value_text() const
	{
		return format_value(value_);
	}

	void curve_editor::set_read_only(bool read_only)
	{
		read_only_ = read_only;
		sync_controls();
	}

	bool curve_editor::read_only() const noexcept
	{
		return read_only_;
	}

	void curve_editor::set_enabled(bool enabled)
	{
		enabled_ = enabled;
		if (root_)
			vgui_widget_set_enabled(root_, enabled ? 1 : 0);
		sync_controls();
	}

	bool curve_editor::enabled() const noexcept
	{
		return enabled_;
	}

	void curve_editor::select_clamped(int index)
	{
		if (value_.keys.empty())
			selected_ = 0;
		else
			selected_ = std::clamp(index, 0, (int)value_.keys.size() - 1);
	}

	bool curve_editor::select_key(int index)
	{
		if (index < 0 || (std::size_t)index >= value_.keys.size())
			return false;
		selected_ = index;
		sync_controls();
		return true;
	}

	int curve_editor::selected_key() const noexcept
	{
		return selected_;
	}

	bool curve_editor::add_key(float t, float value)
	{
		if (read_only_ || !enabled_ || value_.keys.size() >= 64)
			return false;
		t = std::clamp(t, 0.0f, 1.0f);
		value = std::clamp(value, 0.0f, 1.0f);
		for (const curve_key& key : value_.keys)
		{
			if (std::fabs(key.t - t) < 1e-4f)
				return false;
		}
		begin_gesture();
		curve_key key;
		key.t = t;
		key.value = value;
		/* Flat tangents produce a smooth default that users can drag. */
		value_.keys.push_back(key);
		std::stable_sort(value_.keys.begin(), value_.keys.end(),
			[](const curve_key& a, const curve_key& b) {
				return a.t < b.t;
			});
		for (std::size_t i = 0; i < value_.keys.size(); ++i)
		{
			if (std::fabs(value_.keys[i].t - t) < 1e-5f)
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

	bool curve_editor::remove_selected_key()
	{
		if (read_only_ || !enabled_ || value_.keys.size() <= 2 ||
			selected_ < 0 || (std::size_t)selected_ >= value_.keys.size())
			return false;
		begin_gesture();
		value_.keys.erase(value_.keys.begin() + selected_);
		select_clamped(selected_);
		sync_controls();
		notify_change();
		end_gesture();
		return true;
	}

	bool curve_editor::commit_interpolation(curve_interpolation mode)
	{
		if (read_only_ || !enabled_ || mode == value_.interpolation)
			return mode == value_.interpolation;
		begin_gesture();
		value_.interpolation = mode;
		sync_controls();
		notify_change();
		end_gesture();
		return true;
	}

	curve_interpolation curve_editor::interpolation() const noexcept
	{
		return value_.interpolation;
	}

	void curve_editor::set_on_press(value_listener listener)
	{
		press_listener_ = std::move(listener);
	}

	void curve_editor::set_on_change(value_listener listener)
	{
		change_listener_ = std::move(listener);
	}

	void curve_editor::set_on_commit(value_listener listener)
	{
		commit_listener_ = std::move(listener);
	}

	vgui_widget* curve_editor::graph_widget() const noexcept
	{
		return graph_;
	}

	vgui_widget* curve_editor::mode_widget(curve_interpolation mode) const noexcept
	{
		const int index = (int)mode;
		return index >= 0 && index < 3 ? mode_buttons_[index] : nullptr;
	}

	vgui_widget* curve_editor::add_widget() const noexcept
	{
		return add_;
	}

	vgui_widget* curve_editor::remove_widget() const noexcept
	{
		return remove_;
	}

	vgui_widget* curve_editor::time_widget() const noexcept
	{
		return time_spin_ ? time_spin_->widget() : nullptr;
	}

	vgui_widget* curve_editor::value_widget() const noexcept
	{
		return value_spin_ ? value_spin_->widget() : nullptr;
	}

	vgui_widget* curve_editor::in_tangent_widget() const noexcept
	{
		return in_spin_ ? in_spin_->widget() : nullptr;
	}

	vgui_widget* curve_editor::out_tangent_widget() const noexcept
	{
		return out_spin_ ? out_spin_->widget() : nullptr;
	}

	void curve_editor::sync_controls()
	{
		if (!root_)
			return;
		syncing_ = true;
		sync_mode_buttons();
		const bool has_key = !value_.keys.empty() &&
			selected_ >= 0 && (std::size_t)selected_ < value_.keys.size();
		if (time_spin_)
		{
			time_spin_->set_enabled(enabled_ && !read_only_ && has_key);
			time_spin_->set_read_only(read_only_);
		}
		if (value_spin_)
		{
			value_spin_->set_enabled(enabled_ && !read_only_ && has_key);
			value_spin_->set_read_only(read_only_);
		}
		const bool tangents = has_key &&
			value_.interpolation == curve_interpolation::cubic;
		if (in_spin_)
		{
			in_spin_->set_enabled(enabled_ && !read_only_ && tangents);
			in_spin_->set_read_only(read_only_);
		}
		if (out_spin_)
		{
			out_spin_->set_enabled(enabled_ && !read_only_ && tangents);
			out_spin_->set_read_only(read_only_);
		}
		if (add_)
			vgui_widget_set_enabled(add_,
				enabled_ && !read_only_ ? 1 : 0);
		if (remove_)
			vgui_widget_set_enabled(remove_,
				enabled_ && !read_only_ && value_.keys.size() > 2 ? 1 : 0);
		for (int i = 0; i < 3; ++i)
		{
			if (mode_buttons_[i])
				vgui_widget_set_enabled(mode_buttons_[i],
					enabled_ && !read_only_ ? 1 : 0);
		}
		if (has_key)
		{
			const curve_key& key = value_.keys[(std::size_t)selected_];
			if (time_spin_)
				time_spin_->set_value(key.t);
			if (value_spin_)
				value_spin_->set_value(key.value);
			if (in_spin_)
				in_spin_->set_value(key.in_tangent);
			if (out_spin_)
				out_spin_->set_value(key.out_tangent);
		}
		syncing_ = false;
	}

	void curve_editor::sync_mode_buttons()
	{
		for (int i = 0; i < 3; ++i)
		{
			if (mode_buttons_[i])
				vgui_widget_set_checked(mode_buttons_[i],
					(int)value_.interpolation == i ? 1 : 0);
		}
	}

	curve_editor::graph_rect curve_editor::graph_area(vgui_widget* widget) const
	{
		float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
		if (widget)
			vgui_widget_get_rect(widget, &x, &y, &w, &h);
		return graph_area_from_size(w, h);
	}

	curve_editor::graph_rect curve_editor::graph_area_from_size(float w, float h) const
	{
		graph_rect area;
		const float margin = kGraphMargin * ui_scale_of(graph_);
		area.x = margin;
		area.y = margin;
		area.w = std::max(1.0f, w - margin * 2.0f);
		area.h = std::max(1.0f, h - margin * 2.0f);
		return area;
	}

	void curve_editor::key_to_local(const graph_rect& area, const curve_key& key,
		float* out_x, float* out_y) const
	{
		if (out_x)
			*out_x = area.x + key.t * area.w;
		if (out_y)
			*out_y = area.y + (1.0f - key.value) * area.h;
	}

	void curve_editor::local_to_key(const graph_rect& area, float x, float y,
		float* t, float* value) const
	{
		if (t)
			*t = std::clamp((x - area.x) / area.w, 0.0f, 1.0f);
		if (value)
			*value = std::clamp(1.0f - (y - area.y) / area.h, 0.0f, 1.0f);
	}

	int curve_editor::key_at(const graph_rect& area, float x, float y) const
	{
		const float radius = 7.0f * ui_scale_of(graph_);
		for (std::size_t i = 0; i < value_.keys.size(); ++i)
		{
			float kx = 0.0f, ky = 0.0f;
			key_to_local(area, value_.keys[i], &kx, &ky);
			const float dx = x - kx;
			const float dy = y - ky;
			if (dx * dx + dy * dy <= radius * radius)
				return (int)i;
		}
		return -1;
	}

	void curve_editor::tangent_points(const graph_rect& area, int index,
		float* in_x, float* in_y, float* out_x, float* out_y) const
	{
		if (in_x) *in_x = -1.0f;
		if (in_y) *in_y = -1.0f;
		if (out_x) *out_x = -1.0f;
		if (out_y) *out_y = -1.0f;
		if (index < 0 || (std::size_t)index >= value_.keys.size())
			return;
		const curve_key& key = value_.keys[(std::size_t)index];
		float kx = 0.0f, ky = 0.0f;
		key_to_local(area, key, &kx, &ky);
		if (index > 0)
		{
			const float dt = key.t - value_.keys[(std::size_t)index - 1].t;
			const float dx = dt * area.w / 3.0f;
			if (in_x) *in_x = kx - dx;
			if (in_y)
				*in_y = area.y + (1.0f - (key.value - key.in_tangent * dt / 3.0f)) * area.h;
		}
		if ((std::size_t)index + 1 < value_.keys.size())
		{
			const float dt = value_.keys[(std::size_t)index + 1].t - key.t;
			const float dx = dt * area.w / 3.0f;
			if (out_x) *out_x = kx + dx;
			if (out_y)
				*out_y = area.y + (1.0f - (key.value + key.out_tangent * dt / 3.0f)) * area.h;
		}
	}

	int curve_editor::tangent_at(const graph_rect& area, float x, float y,
		drag_kind* out_kind) const
	{
		if (value_.interpolation != curve_interpolation::cubic)
			return -1;
		float ix = 0, iy = 0, ox = 0, oy = 0;
		tangent_points(area, selected_, &ix, &iy, &ox, &oy);
		const float radius = 7.0f * ui_scale_of(graph_);
		if (ox >= 0.0f)
		{
			const float dx = x - ox, dy = y - oy;
			if (dx * dx + dy * dy <= radius * radius)
			{
				if (out_kind)
					*out_kind = drag_kind::out_tangent;
				return selected_;
			}
		}
		if (ix >= 0.0f)
		{
			const float dx = x - ix, dy = y - iy;
			if (dx * dx + dy * dy <= radius * radius)
			{
				if (out_kind)
					*out_kind = drag_kind::in_tangent;
				return selected_;
			}
		}
		return -1;
	}

	void curve_editor::begin_gesture()
	{
		if (gesture_active_)
			return;
		gesture_active_ = true;
		if (press_listener_)
			press_listener_(value_);
	}

	void curve_editor::end_gesture()
	{
		if (!gesture_active_)
			return;
		gesture_active_ = false;
		if (commit_listener_)
			commit_listener_(value_);
	}

	void curve_editor::notify_change()
	{
		if (change_listener_)
			change_listener_(value_);
	}

	void curve_editor::on_time_change(double value)
	{
		if (syncing_ || read_only_ || !enabled_ || selected_ < 0 ||
			(std::size_t)selected_ >= value_.keys.size())
			return;
		begin_gesture();
		const int index = selected_;
		float t = (float)value;
		if (index > 0)
			t = std::max(t, value_.keys[(std::size_t)index - 1].t + 1e-3f);
		if ((std::size_t)index + 1 < value_.keys.size())
			t = std::min(t, value_.keys[(std::size_t)index + 1].t - 1e-3f);
		t = std::clamp(t, 0.0f, 1.0f);
		value_.keys[(std::size_t)index].t = t;
		if (time_spin_)
			time_spin_->set_value(t);
		notify_change();
	}

	void curve_editor::on_time_commit()
	{
		if (read_only_ || !enabled_)
			return;
		end_gesture();
	}

	void curve_editor::on_value_change(double value)
	{
		if (syncing_ || read_only_ || !enabled_ || selected_ < 0 ||
			(std::size_t)selected_ >= value_.keys.size())
			return;
		begin_gesture();
		value_.keys[(std::size_t)selected_].value =
			std::clamp((float)value, 0.0f, 1.0f);
		notify_change();
	}

	void curve_editor::on_value_commit()
	{
		if (read_only_ || !enabled_)
			return;
		end_gesture();
	}

	void curve_editor::on_in_change(double value)
	{
		if (syncing_ || read_only_ || !enabled_ || selected_ < 0 ||
			(std::size_t)selected_ >= value_.keys.size())
			return;
		begin_gesture();
		value_.keys[(std::size_t)selected_].in_tangent =
			std::clamp((float)value, -20.0f, 20.0f);
		notify_change();
	}

	void curve_editor::on_in_commit()
	{
		if (read_only_ || !enabled_)
			return;
		end_gesture();
	}

	void curve_editor::on_out_change(double value)
	{
		if (syncing_ || read_only_ || !enabled_ || selected_ < 0 ||
			(std::size_t)selected_ >= value_.keys.size())
			return;
		begin_gesture();
		value_.keys[(std::size_t)selected_].out_tangent =
			std::clamp((float)value, -20.0f, 20.0f);
		notify_change();
	}

	void curve_editor::on_out_commit()
	{
		if (read_only_ || !enabled_)
			return;
		end_gesture();
	}

	int curve_editor::on_graph_event(vgui_widget* widget, const vgui_event* event,
		void* user)
	{
		auto* self = static_cast<curve_editor*>(user);
		if (!self || !event || !widget)
			return 0;
		if (self->read_only_ || !self->enabled_)
			return 0;
		const graph_rect area = self->graph_area(widget);
		if (event->type == VGUI_EVENT_DOUBLE_CLICK && event->button == 1)
		{
			if (self->key_at(area, event->x, event->y) < 0)
			{
				float t = 0.0f, value = 0.0f;
				self->local_to_key(area, event->x, event->y, &t, &value);
				(void)self->add_key(t, value);
			}
			return 1;
		}
		if (event->type == VGUI_EVENT_MOUSE_DOWN && event->button == 1)
		{
			drag_kind kind = drag_kind::none;
			const int tangent = self->tangent_at(area, event->x, event->y,
				&kind);
			if (tangent >= 0)
			{
				self->drag_ = kind;
				self->begin_gesture();
				return 1;
			}
			const int key = self->key_at(area, event->x, event->y);
			if (key >= 0)
			{
				self->selected_ = key;
				self->drag_ = drag_kind::key;
				self->sync_controls();
				self->begin_gesture();
				return 1;
			}
			return 0;
		}
		if (event->type == VGUI_EVENT_MOUSE_MOVE && self->drag_ != drag_kind::none)
		{
			if (self->drag_ == drag_kind::key)
			{
				float t = 0.0f, value = 0.0f;
				self->local_to_key(area, event->x, event->y, &t, &value);
				const int index = self->selected_;
				if (index < 0 || (std::size_t)index >= self->value_.keys.size())
					return 0;
				if (index > 0)
					t = std::max(t,
						self->value_.keys[(std::size_t)index - 1].t + 1e-3f);
				if ((std::size_t)index + 1 < self->value_.keys.size())
					t = std::min(t,
						self->value_.keys[(std::size_t)index + 1].t - 1e-3f);
				t = std::clamp(t, 0.0f, 1.0f);
				curve_key& key = self->value_.keys[(std::size_t)index];
				key.t = t;
				key.value = std::clamp(value, 0.0f, 1.0f);
				self->sync_controls();
				self->notify_change();
				return 1;
			}
			const int index = self->selected_;
			if (index < 0 || (std::size_t)index >= self->value_.keys.size())
				return 0;
			curve_key& key = self->value_.keys[(std::size_t)index];
			const float handle_value = std::clamp(
				1.0f - (event->y - area.y) / area.h, -19.0f, 20.0f);
			if (self->drag_ == drag_kind::out_tangent &&
				(std::size_t)index + 1 < self->value_.keys.size())
			{
				const float dt = self->value_.keys[(std::size_t)index + 1].t -
					key.t;
				if (dt > 1e-4f)
					key.out_tangent = std::clamp(
						(handle_value - key.value) / (dt / 3.0f),
						-20.0f, 20.0f);
			}
			else if (self->drag_ == drag_kind::in_tangent && index > 0)
			{
				const float dt = key.t -
					self->value_.keys[(std::size_t)index - 1].t;
				if (dt > 1e-4f)
					key.in_tangent = std::clamp(
						(key.value - handle_value) / (dt / 3.0f),
						-20.0f, 20.0f);
			}
			self->sync_controls();
			self->notify_change();
			return 1;
		}
		if (event->type == VGUI_EVENT_MOUSE_UP && self->drag_ != drag_kind::none)
		{
			self->drag_ = drag_kind::none;
			self->end_gesture();
			return 1;
		}
		return 0;
	}

	void curve_editor::on_mode_click(vgui_widget* widget, const vgui_event* event,
		void* user)
	{
		(void)widget;
		if (!event || event->type != VGUI_EVENT_CLICK)
			return;
		auto* self = static_cast<curve_editor*>(user);
		if (!self)
			return;
		for (int i = 0; i < 3; ++i)
		{
			if (widget == self->mode_buttons_[i])
			{
				(void)self->commit_interpolation((curve_interpolation)i);
				return;
			}
		}
	}

	void curve_editor::on_add_click(vgui_widget* widget, const vgui_event* event,
		void* user)
	{
		(void)widget;
		if (!event || event->type != VGUI_EVENT_CLICK)
			return;
		auto* self = static_cast<curve_editor*>(user);
		if (!self || self->value_.keys.size() < 2)
			return;
		/* Insert in the widest gap so repeated adds spread over the graph. */
		std::size_t best = 0;
		float best_gap = -1.0f;
		for (std::size_t i = 0; i + 1 < self->value_.keys.size(); ++i)
		{
			const float gap = self->value_.keys[i + 1].t -
				self->value_.keys[i].t;
			if (gap > best_gap)
			{
				best_gap = gap;
				best = i;
			}
		}
		const curve_key& a = self->value_.keys[best];
		const curve_key& b = self->value_.keys[best + 1];
		const float t = (a.t + b.t) * 0.5f;
		/* Linear sample for the default value. */
		const float value = a.value + (b.value - a.value) *
			(t - a.t) / std::max(1e-5f, b.t - a.t);
		(void)self->add_key(t, value);
	}

	void curve_editor::on_remove_click(vgui_widget* widget,
		const vgui_event* event, void* user)
	{
		(void)widget;
		if (!event || event->type != VGUI_EVENT_CLICK)
			return;
		auto* self = static_cast<curve_editor*>(user);
		if (self)
			(void)self->remove_selected_key();
	}

	void curve_editor::draw_graph(vgui_widget* widget, ovg_ctx_cb* cb, rvg_t* vg,
		float x, float y, float w, float h, float dt, void* user)
	{
		(void)dt;
		auto* self = static_cast<curve_editor*>(user);
		if (!self || !cb || !vg || w <= 0.0f || h <= 0.0f)
			return;
		ovg_tools::fill_rect(cb, vg, x, y, w, h, to_ovg(kGraphBg));
		const float scale = ui_scale_of(widget);
		const graph_rect area = self->graph_area_from_size(w, h);
		const float gx = x;
		const float gy = y;
		for (int i = 1; i < 4; ++i)
		{
			const float t = (float)i / 4.0f;
			ovg_tools::line(cb, vg, gx + area.x + area.w * t, gy + area.y,
				gx + area.x + area.w * t, gy + area.y + area.h,
				to_ovg(kGrid), 1.0f * scale);
			ovg_tools::line(cb, vg, gx + area.x, gy + area.y + area.h * t,
				gx + area.x + area.w, gy + area.y + area.h * t,
				to_ovg(kGrid), 1.0f * scale);
		}
		ovg_tools::stroke_rect(cb, vg, gx + area.x, gy + area.y, area.w,
			area.h, to_ovg(kPanelBorder), 1.0f * scale);
		draw_curve_path(cb, vg, self->value_, gx + area.x, gy + area.y,
			area.w, area.h, ovg_tools::rgba(230, 180, 90), 1.75f * scale);

		for (std::size_t i = 0; i < self->value_.keys.size(); ++i)
		{
			float kx = 0.0f, ky = 0.0f;
			self->key_to_local(area, self->value_.keys[i], &kx, &ky);
			const float size = ((int)i == self->selected_ ? 9.0f : 7.0f) *
				scale;
			ovg_tools::fill_rect(cb, vg, gx + kx - size * 0.5f,
				gy + ky - size * 0.5f, size, size,
				ovg_tools::rgba(235, 235, 235));
			if ((int)i == self->selected_)
				ovg_tools::stroke_rect(cb, vg, gx + kx - size * 0.5f,
					gy + ky - size * 0.5f, size, size,
					ovg_tools::rgba(245, 190, 80), 1.0f * scale);
		}

		if (self->value_.interpolation == curve_interpolation::cubic &&
			self->selected_ >= 0 &&
			(std::size_t)self->selected_ < self->value_.keys.size())
		{
			float ix = 0, iy = 0, ox = 0, oy = 0;
			self->tangent_points(area, self->selected_, &ix, &iy, &ox, &oy);
			float kx = 0.0f, ky = 0.0f;
			self->key_to_local(area,
				self->value_.keys[(std::size_t)self->selected_], &kx, &ky);
			const uint32_t handle_color = to_ovg(0xFFE0B050);
			const uint32_t line_color = to_ovg(0xFF6C7A89);
			if (ix >= 0.0f)
			{
				ovg_tools::dashed_line(cb, vg, gx + kx, gy + ky,
					gx + ix, gy + iy, line_color, 1.0f * scale,
					3.0f * scale, 2.0f * scale);
				ovg_tools::fill_circle(cb, vg, gx + ix, gy + iy,
					4.0f * scale, handle_color);
			}
			if (ox >= 0.0f)
			{
				ovg_tools::dashed_line(cb, vg, gx + kx, gy + ky,
					gx + ox, gy + oy, line_color, 1.0f * scale,
					3.0f * scale, 2.0f * scale);
				ovg_tools::fill_circle(cb, vg, gx + ox, gy + oy,
					4.0f * scale, handle_color);
			}
		}
	}

	void curve_editor::draw_preview(ovg_ctx_cb* cb, rvg_t* vg, float x, float y,
		float w, float h, const curve_value& value, bool mixed,
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
		ovg_tools::fill_round_rect(cb, vg, x, y, w, h, 3.0f * scale,
			to_ovg(kGraphBg));
		const float inset = std::min(4.0f * scale, h * 0.2f);
		draw_curve_path(cb, vg, value, x + inset, y + inset,
			std::max(1.0f, w - inset * 2.0f),
			std::max(1.0f, h - inset * 2.0f),
			ovg_tools::rgba(230, 180, 90), 1.5f * scale);
		for (const curve_key& key : value.keys)
		{
			const float kx = x + inset + key.t *
				std::max(1.0f, w - inset * 2.0f);
			const float ky = y + inset + (1.0f - key.value) *
				std::max(1.0f, h - inset * 2.0f);
			ovg_tools::fill_circle(cb, vg, kx, ky, 1.8f * scale,
				ovg_tools::rgba(235, 235, 235));
		}
	}

	bool curve_editor::parse_value(const std::string& text, curve_value& out,
		std::string* error)
	{
		const std::string trimmed = trim_copy(text);
		if (trimmed.empty())
		{
			if (error)
				*error = "empty curve";
			return false;
		}
		const std::vector<std::string> parts = split(trimmed, ';');
		std::size_t begin = 0;
		curve_value parsed;
		parsed.interpolation = curve_interpolation::cubic;
		if (!parts.empty())
		{
			curve_interpolation mode = curve_interpolation::cubic;
			if (parse_mode(parts[0], &mode))
			{
				parsed.interpolation = mode;
				begin = 1;
			}
		}
		for (std::size_t i = begin; i < parts.size(); ++i)
		{
			const std::string token = trim_copy(parts[i]);
			if (token.empty())
				continue;
			const std::vector<std::string> fields = split(token, ',');
			if (fields.size() != 2 && fields.size() != 4)
			{
				if (error)
					*error = "curve key needs t,value or t,value,in,out";
				return false;
			}
			curve_key key;
			if (!parse_float(fields[0], &key.t) ||
				!parse_float(fields[1], &key.value))
			{
				if (error)
					*error = "invalid curve number";
				return false;
			}
			if (fields.size() == 4)
			{
				if (!parse_float(fields[2], &key.in_tangent) ||
					!parse_float(fields[3], &key.out_tangent))
				{
					if (error)
						*error = "invalid curve tangent";
					return false;
				}
			}
			parsed.keys.push_back(key);
		}
		if (!normalize_curve(parsed, error))
			return false;
		out = std::move(parsed);
		return true;
	}

	std::string curve_editor::format_value(const curve_value& value)
	{
		std::string text = interpolation_name(value.interpolation);
		for (const curve_key& key : value.keys)
		{
			text += ';';
			text += format_float(key.t);
			text += ',';
			text += format_float(key.value);
			text += ',';
			text += format_float(key.in_tangent);
			text += ',';
			text += format_float(key.out_tangent);
		}
		return text;
	}

	const char* curve_editor::interpolation_name(curve_interpolation mode)
	{
		switch (mode)
		{
		case curve_interpolation::linear: return "linear";
		case curve_interpolation::step: return "step";
		default: return "cubic";
		}
	}

	const char* curve_editor::interpolation_label(curve_interpolation mode)
	{
		switch (mode)
		{
		case curve_interpolation::linear: return "Linear";
		case curve_interpolation::step: return "Step";
		default: return "Cubic";
		}
	}

} /* namespace vgui_editor_kit */
