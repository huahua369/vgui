#include "vector_editor.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <utility>

namespace vgui_editor_kit {

	namespace {
		constexpr float kAxisLabelWidth = 16.0f;
		constexpr float kComponentMinWidth = 36.0f;
		constexpr float kRowHeight = 24.0f;

		std::string trim_copy(std::string text)
		{
			auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
			while (!text.empty() && is_space((unsigned char)text.front()))
				text.erase(text.begin());
			while (!text.empty() && is_space((unsigned char)text.back()))
				text.pop_back();
			return text;
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
			char buffer[64];
			const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value,
				std::chars_format::general, 15);
			if (result.ec != std::errc())
				return std::to_string(value);
			return std::string(buffer, result.ptr);
		}

		void set_parse_error(std::string* error, int dimension)
		{
			if (error)
				*error = "expected " + std::to_string(dimension) + " numbers";
		}

		const std::string& empty_string()
		{
			static const std::string value;
			return value;
		}
	}

	void vector_editor::build(vgui_widget* parent, int dimension)
	{
		if (panel_ || !parent)
			return;
		if (dimension >= 2 && dimension <= kMaxComponents)
			dimension_ = dimension;

		apply_semantic_defaults();

		panel_ = vgui_widget_add(parent, VGUI_PANEL, nullptr);
		if (!panel_)
			return;
		vgui_widget_set_size(panel_, 220, kRowHeight);
		vgui_widget_set_min_size(panel_, 100, kRowHeight);
		vgui_widget_set_layout(panel_, VGUI_LAYOUT_ROW);
		vgui_widget_set_padding(panel_, 0, 0, 0, 0);
		vgui_widget_set_gap(panel_, 2);
		vgui_widget_set_align_items(panel_, VGUI_ALIGN_CENTER);

		components_.clear();
		for (int i = 0; i < dimension_; ++i)
		{
			auto box = std::make_unique<spinbox>();
			box->set_value_type(value_type_);
			if (has_range_)
				box->set_range(min_value_, max_value_);
			box->set_step(step_);
			box->set_label(labels_[(std::size_t)i]);
			box->set_label_width(kAxisLabelWidth);
			box->set_buttons_visible(false);
			box->set_on_change([this, i](double) { handle_change(i); });
			box->set_on_commit([this, i](double) { handle_commit(i); });
			box->build(panel_);
			if (!box->widget())
				continue;
			vgui_widget_set_grow(box->widget(), 1.0f);
			vgui_widget_set_size(box->widget(), 0, kRowHeight);
			vgui_widget_set_min_size(box->widget(), kComponentMinWidth,
				kRowHeight);
			components_.push_back(std::move(box));
		}

		apply_ids();
		sync_values();
		apply_axis_ui();
		apply_options();
		if (!enabled_)
			vgui_widget_set_enabled(panel_, 0);
		for (auto& box : components_)
		{
			box->set_enabled(enabled_);
			box->set_read_only(read_only_);
		}
	}

	bool vector_editor::set_dimension(int dimension)
	{
		if (dimension < 2 || dimension > kMaxComponents)
			return false;
		if (panel_)
			return dimension_ == dimension;
		dimension_ = dimension;
		apply_semantic_defaults();
		return true;
	}

	int vector_editor::dimension() const noexcept
	{
		return dimension_;
	}

	void vector_editor::set_id(std::string id)
	{
		if (id.empty())
			return;
		id_ = std::move(id);
		apply_ids();
	}

	const std::string& vector_editor::id() const noexcept
	{
		return id_;
	}

	void vector_editor::apply_ids()
	{
		if (panel_)
			vgui_widget_set_id(panel_, id_.c_str());
		for (std::size_t i = 0; i < components_.size(); ++i)
		{
			if (components_[i])
				components_[i]->set_id(id_ + "_" + std::to_string(i));
		}
	}

	void vector_editor::set_semantic(vector_semantic semantic)
	{
		semantic_ = semantic;
		custom_labels_ = false;
		custom_colors_ = false;
		apply_semantic_defaults();
		apply_axis_ui();
	}

	vector_semantic vector_editor::semantic() const noexcept
	{
		return semantic_;
	}

	void vector_editor::apply_semantic_defaults()
	{
		labels_.resize(kMaxComponents);
		colors_.resize(kMaxComponents);
		if (!custom_labels_)
		{
			for (int i = 0; i < kMaxComponents; ++i)
				labels_[(std::size_t)i] = default_axis_label(i, semantic_);
		}
		if (!custom_colors_)
		{
			for (int i = 0; i < kMaxComponents; ++i)
				colors_[(std::size_t)i] = default_axis_color(i, semantic_);
		}
	}

	void vector_editor::set_axis_labels(std::vector<std::string> labels)
	{
		custom_labels_ = true;
		labels_.resize(kMaxComponents);
		for (int i = 0; i < kMaxComponents; ++i)
		{
			labels_[(std::size_t)i] = (std::size_t)i < labels.size() &&
				!labels[(std::size_t)i].empty()
				? labels[(std::size_t)i]
				: default_axis_label(i, semantic_);
		}
		apply_axis_ui();
	}

	void vector_editor::set_axis_label(int index, std::string label)
	{
		if (index < 0 || index >= kMaxComponents)
			return;
		custom_labels_ = true;
		labels_.resize(kMaxComponents);
		labels_[(std::size_t)index] = label.empty()
			? default_axis_label(index, semantic_) : std::move(label);
		apply_axis_ui();
	}

	const std::vector<std::string>& vector_editor::axis_labels() const noexcept
	{
		return labels_;
	}

	void vector_editor::set_axis_colors(std::vector<vgui_color> colors)
	{
		custom_colors_ = true;
		colors_.resize(kMaxComponents);
		for (int i = 0; i < kMaxComponents; ++i)
		{
			colors_[(std::size_t)i] = (std::size_t)i < colors.size()
				? colors[(std::size_t)i]
				: default_axis_color(i, semantic_);
		}
		apply_axis_ui();
	}

	void vector_editor::set_axis_color(int index, vgui_color color)
	{
		if (index < 0 || index >= kMaxComponents)
			return;
		custom_colors_ = true;
		colors_.resize(kMaxComponents);
		colors_[(std::size_t)index] = color;
		apply_axis_ui();
	}

	const std::vector<vgui_color>& vector_editor::axis_colors() const noexcept
	{
		return colors_;
	}

	void vector_editor::apply_axis_ui()
	{
		for (std::size_t i = 0; i < components_.size(); ++i)
		{
			spinbox& box = *components_[i];
			if (i < labels_.size())
				box.set_label(labels_[i]);
			vgui_widget* label = box.label_widget();
			if (!label)
				continue;
			if (i < colors_.size())
				vgui_widget_set_text_color(label, colors_[i]);
			const std::string tip = "Drag to change " +
				(i < labels_.size() ? labels_[i] : std::string());
			vgui_widget_set_tooltip(label, tip.c_str());
		}
	}

	void vector_editor::set_value_type(spinbox::value_type type)
	{
		if (value_type_ == type)
			return;
		value_type_ = type;
		apply_options();
		for (std::size_t i = 0; i < components_.size(); ++i)
			values_[i] = components_[i]->value();
	}

	spinbox::value_type vector_editor::value_type() const noexcept
	{
		return value_type_;
	}

	void vector_editor::set_range(double min_value, double max_value)
	{
		if (!std::isfinite(min_value) || !std::isfinite(max_value))
			return;
		if (min_value > max_value)
			std::swap(min_value, max_value);
		has_range_ = true;
		min_value_ = min_value;
		max_value_ = max_value;
		for (std::size_t i = 0; i < components_.size(); ++i)
		{
			components_[i]->set_range(min_value_, max_value_);
			values_[i] = components_[i]->value();
		}
	}

	void vector_editor::clear_range()
	{
		has_range_ = false;
		for (auto& box : components_)
			box->clear_range();
	}

	bool vector_editor::has_range() const noexcept
	{
		return has_range_;
	}

	double vector_editor::min_value() const noexcept
	{
		return min_value_;
	}

	double vector_editor::max_value() const noexcept
	{
		return max_value_;
	}

	void vector_editor::set_step(double step)
	{
		step_ = std::isfinite(step) && step > 0.0 ? step : 0.0;
		for (auto& box : components_)
			box->set_step(step_);
	}

	double vector_editor::step() const noexcept
	{
		return effective_step();
	}

	double vector_editor::effective_step() const noexcept
	{
		if (step_ > 0.0)
			return step_;
		return value_type_ == spinbox::value_type::integer ? 1.0 : 0.1;
	}

	void vector_editor::apply_options()
	{
		for (auto& box : components_)
		{
			box->set_value_type(value_type_);
			if (has_range_)
				box->set_range(min_value_, max_value_);
			else
				box->clear_range();
			box->set_step(step_);
		}
	}

	void vector_editor::sync_values()
	{
		for (std::size_t i = 0; i < components_.size(); ++i)
		{
			components_[i]->set_value(values_[i]);
			values_[i] = components_[i]->value();
		}
	}

	void vector_editor::set_value(const vector_value& values)
	{
		for (int i = 0; i < dimension_; ++i)
			values_[(std::size_t)i] = values[(std::size_t)i];
		sync_values();
	}

	void vector_editor::set_component_value(int index, double value)
	{
		if (index < 0 || index >= dimension_)
			return;
		values_[(std::size_t)index] = value;
		if (index < (int)components_.size())
		{
			components_[(std::size_t)index]->set_value(value);
			values_[(std::size_t)index] =
				components_[(std::size_t)index]->value();
		}
	}

	vector_editor::vector_value vector_editor::value() const noexcept
	{
		return values_;
	}

	std::string vector_editor::value_text() const
	{
		return format_value(values_, dimension_);
	}

	bool vector_editor::commit_component_text(int index, std::string text)
	{
		if (index < 0 || index >= (int)components_.size())
			return false;
		return components_[(std::size_t)index]->commit_text(std::move(text));
	}

	bool vector_editor::commit_component_value(int index, double value)
	{
		if (index < 0 || index >= (int)components_.size())
			return false;
		return components_[(std::size_t)index]->commit_value(value);
	}

	void vector_editor::set_read_only(bool read_only)
	{
		if (read_only_ == read_only)
			return;
		read_only_ = read_only;
		for (auto& box : components_)
			box->set_read_only(read_only);
	}

	bool vector_editor::read_only() const noexcept
	{
		return read_only_;
	}

	void vector_editor::set_enabled(bool enabled)
	{
		if (enabled_ == enabled)
			return;
		enabled_ = enabled;
		if (panel_)
			vgui_widget_set_enabled(panel_, enabled ? 1 : 0);
		for (auto& box : components_)
			box->set_enabled(enabled);
	}

	bool vector_editor::enabled() const noexcept
	{
		return enabled_;
	}

	void vector_editor::set_component_error(int index, std::string error)
	{
		if (index < 0 || index >= (int)components_.size())
			return;
		components_[(std::size_t)index]->set_error(std::move(error));
	}

	void vector_editor::clear_component_errors()
	{
		for (auto& box : components_)
			box->set_error(std::string());
	}

	const std::string& vector_editor::component_error(int index) const
	{
		if (index < 0 || index >= (int)components_.size())
			return empty_string();
		return components_[(std::size_t)index]->error();
	}

	void vector_editor::focus_component(int index)
	{
		if (index < 0 || index >= (int)components_.size())
			return;
		components_[(std::size_t)index]->focus();
	}

	void vector_editor::set_on_change(value_listener listener)
	{
		change_listener_ = std::move(listener);
	}

	void vector_editor::set_on_commit(value_listener listener)
	{
		commit_listener_ = std::move(listener);
	}

	void vector_editor::set_value_source(value_source source)
	{
		value_source_ = std::move(source);
	}

	void vector_editor::refresh()
	{
		if (!value_source_)
			return;
		vector_value next = values_;
		if (value_source_(next))
			set_value(next);
	}

	void vector_editor::handle_change(int index)
	{
		if (index < 0 || index >= (int)components_.size())
			return;
		values_[(std::size_t)index] = components_[(std::size_t)index]->value();
		notify_change();
	}

	void vector_editor::handle_commit(int index)
	{
		if (index < 0 || index >= (int)components_.size())
			return;
		values_[(std::size_t)index] = components_[(std::size_t)index]->value();
		notify_commit();
	}

	void vector_editor::notify_change()
	{
		if (!change_listener_)
			return;
		vector_value snapshot = values_;
		change_listener_(snapshot);
	}

	void vector_editor::notify_commit()
	{
		if (!commit_listener_)
			return;
		vector_value snapshot = values_;
		commit_listener_(snapshot);
	}

	vgui_widget* vector_editor::widget() const noexcept
	{
		return panel_;
	}

	vgui_widget* vector_editor::component_widget(int index) const noexcept
	{
		if (index < 0 || index >= (int)components_.size())
			return nullptr;
		return components_[(std::size_t)index]->widget();
	}

	vgui_widget* vector_editor::component_edit_widget(int index) const noexcept
	{
		if (index < 0 || index >= (int)components_.size())
			return nullptr;
		return components_[(std::size_t)index]->edit_widget();
	}

	vgui_widget* vector_editor::component_label_widget(int index) const noexcept
	{
		if (index < 0 || index >= (int)components_.size())
			return nullptr;
		return components_[(std::size_t)index]->label_widget();
	}

	bool vector_editor::parse_value(const std::string& text, int dimension,
		vector_value& out, std::string* error)
	{
		if (dimension < 2 || dimension > kMaxComponents)
		{
			if (error)
				*error = "invalid vector dimension";
			return false;
		}

		std::string trimmed = trim_copy(text);
		if (trimmed.size() >= 2)
		{
			const char first = trimmed.front();
			const char last = trimmed.back();
			if ((first == '[' && last == ']') ||
				(first == '(' && last == ')') ||
				(first == '{' && last == '}'))
				trimmed = trim_copy(trimmed.substr(1, trimmed.size() - 2));
		}

		std::vector<std::string> tokens;
		std::string current;
		for (char c : trimmed)
		{
			if (c == ',' || c == ';' || std::isspace((unsigned char)c))
			{
				if (!current.empty())
				{
					tokens.push_back(current);
					current.clear();
				}
				continue;
			}
			current.push_back(c);
		}
		if (!current.empty())
			tokens.push_back(current);

		if ((int)tokens.size() != dimension)
		{
			set_parse_error(error, dimension);
			return false;
		}

		vector_value parsed{};
		for (int i = 0; i < dimension; ++i)
		{
			if (!parse_number_text(tokens[(std::size_t)i],
				parsed[(std::size_t)i]))
			{
				set_parse_error(error, dimension);
				return false;
			}
		}
		out = parsed;
		return true;
	}

	std::string vector_editor::format_value(const vector_value& values, int dimension)
	{
		dimension = std::clamp(dimension, 0, kMaxComponents);
		std::string text;
		for (int i = 0; i < dimension; ++i)
		{
			if (i > 0)
				text += ", ";
			text += format_number(values[(std::size_t)i]);
		}
		return text;
	}

	const char* vector_editor::default_axis_label(int index,
		vector_semantic semantic)
	{
		static const char* const generic[] = { "X", "Y", "Z", "W" };
		static const char* const color[] = { "R", "G", "B", "A" };
		if (index < 0 || index >= kMaxComponents)
			return "";
		return semantic == vector_semantic::color
			? color[index] : generic[index];
	}

	vgui_color vector_editor::default_axis_color(int index,
		vector_semantic semantic)
	{
		static const vgui_color position[] = {
			0xFFE06C75, 0xFF98C379, 0xFF61AFEF, 0xFFD19A66
		};
		static const vgui_color color[] = {
			0xFFE06C75, 0xFF98C379, 0xFF61AFEF, 0xFFABB2BF
		};
		if (index < 0 || index >= kMaxComponents)
			return 0;
		switch (semantic)
		{
		case vector_semantic::position:
			return position[index];
		case vector_semantic::color:
			return color[index];
		default:
			return 0;
		}
	}

} /* namespace vgui_editor_kit */
