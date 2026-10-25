#include "status_bar.h"

#include <cstdio>
#include <utility>

namespace vgui_editor_kit {

	namespace {
		constexpr vgui_color kFallbackPanel = 0xFF1E232B;
		constexpr vgui_color kFallbackBorder = 0xFF2E3440;
		constexpr vgui_color kFallbackText = 0xFFB8C4D6;
		constexpr vgui_color kFallbackMuted = 0xFF7F8B9C;
		constexpr vgui_color kFallbackError = 0xFFE06C75;

		vgui_theme theme_for(vgui_widget* widget)
		{
			vgui_theme theme = {};
			vgui_context* ctx = widget ? vgui_widget_get_context(widget) : nullptr;
			if (ctx)
				vgui_context_get_theme(ctx, &theme);
			return theme;
		}

		vgui_color panel_color(const vgui_theme& theme)
		{
			return theme.panel_bg ? theme.panel_bg : kFallbackPanel;
		}

		vgui_color border_color(const vgui_theme& theme)
		{
			return theme.panel_border ? theme.panel_border : kFallbackBorder;
		}

		vgui_color text_color(const vgui_theme& theme)
		{
			return theme.text ? theme.text : kFallbackText;
		}

		vgui_color mix_color(vgui_color a, vgui_color b, float t)
		{
			if (t < 0.0f) t = 0.0f;
			if (t > 1.0f) t = 1.0f;
			auto mix = [t](unsigned ca, unsigned cb) {
				return (unsigned)(ca + (cb - ca) * t + 0.5f);
			};
			return (mix((a >> 24) & 0xFF, (b >> 24) & 0xFF) << 24)
				| (mix((a >> 16) & 0xFF, (b >> 16) & 0xFF) << 16)
				| (mix((a >> 8) & 0xFF, (b >> 8) & 0xFF) << 8)
				| mix(a & 0xFF, b & 0xFF);
		}

		vgui_color muted_color(const vgui_theme& theme)
		{
			return theme.text
				? mix_color(theme.text, panel_color(theme), 0.4f)
				: kFallbackMuted;
		}

		vgui_color error_color(const vgui_theme& theme)
		{
			if (theme.toast_error)
				return theme.toast_error;
			return theme.modal_danger ? theme.modal_danger : kFallbackError;
		}

		vgui_widget* make_label(vgui_widget* parent, vgui_color color)
		{
			vgui_widget* label = vgui_widget_add(parent, VGUI_LABEL, "");
			vgui_widget_set_size(label, 0, 20);
			vgui_widget_set_font_size(label, 12);
			vgui_widget_set_text_color(label, color);
			vgui_widget_set_text_align(label, 1.0f, 0.5f);
			return label;
		}
	}

	void status_bar::build(vgui_widget* parent)
	{
		if (panel_ || !parent)
			return;

		panel_ = vgui_widget_add(parent, VGUI_PANEL, nullptr);
		vgui_widget_set_size(panel_, 0, 26);
		vgui_widget_set_min_size(panel_, 0, 22);
		vgui_widget_set_align_self(panel_, VGUI_ALIGN_STRETCH);
		vgui_widget_set_layout(panel_, VGUI_LAYOUT_ROW);
		vgui_widget_set_padding(panel_, 8, 0, 8, 0);
		vgui_widget_set_gap(panel_, 14);
		vgui_widget_set_align_items(panel_, VGUI_ALIGN_CENTER);

		const vgui_theme theme = theme_for(panel_);
		vgui_widget_set_background(panel_, panel_color(theme));
		vgui_widget_set_border(panel_, border_color(theme), 1);

		message_label_ = vgui_widget_add(panel_, VGUI_LABEL, "");
		vgui_widget_set_grow(message_label_, 1.0f);
		vgui_widget_set_size(message_label_, 0, 20);
		vgui_widget_set_font_size(message_label_, 12);
		vgui_widget_set_text_color(message_label_, text_color(theme));
		vgui_widget_set_text_align(message_label_, 0.0f, 0.5f);

		selection_label_ = make_label(panel_, muted_color(theme));
		vgui_widget_set_size(selection_label_, 170, 20);

		coordinate_label_ = make_label(panel_, muted_color(theme));
		vgui_widget_set_size(coordinate_label_, 150, 20);

		error_label_ = make_label(panel_, error_color(theme));
		vgui_widget_set_size(error_label_, 240, 20);

		apply_message();
		apply_selection();
		apply_coordinates();
		apply_error();
	}

	void status_bar::set_message(std::string text)
	{
		message_ = std::move(text);
		apply_message();
	}

	const std::string& status_bar::message() const noexcept
	{
		return message_;
	}

	void status_bar::set_selected_object(object_id id)
	{
		set_selection_text(id == invalid_object ? std::string()
			: "Selected: " + std::to_string(id));
	}

	void status_bar::set_selection_text(std::string text)
	{
		selection_ = std::move(text);
		apply_selection();
	}

	const std::string& status_bar::selection_text() const noexcept
	{
		return selection_;
	}

	void status_bar::set_coordinates(float x, float y)
	{
		char buffer[64];
		std::snprintf(buffer, sizeof(buffer), "x %.2f  y %.2f", x, y);
		set_coordinate_text(buffer);
	}

	void status_bar::set_coordinate_text(std::string text)
	{
		coordinates_ = std::move(text);
		apply_coordinates();
	}

	const std::string& status_bar::coordinate_text() const noexcept
	{
		return coordinates_;
	}

	void status_bar::set_error(std::string text)
	{
		error_ = std::move(text);
		apply_error();
	}

	void status_bar::clear_error()
	{
		set_error({});
	}

	const std::string& status_bar::error() const noexcept
	{
		return error_;
	}

	vgui_widget* status_bar::widget() const noexcept
	{
		return panel_;
	}

	vgui_widget* status_bar::message_widget() const noexcept
	{
		return message_label_;
	}

	vgui_widget* status_bar::selection_widget() const noexcept
	{
		return selection_label_;
	}

	vgui_widget* status_bar::coordinate_widget() const noexcept
	{
		return coordinate_label_;
	}

	vgui_widget* status_bar::error_widget() const noexcept
	{
		return error_label_;
	}

	void status_bar::apply_message()
	{
		if (message_label_)
			vgui_widget_set_text(message_label_, message_.c_str());
	}

	void status_bar::apply_selection()
	{
		if (selection_label_)
			vgui_widget_set_text(selection_label_, selection_.c_str());
	}

	void status_bar::apply_coordinates()
	{
		if (coordinate_label_)
			vgui_widget_set_text(coordinate_label_, coordinates_.c_str());
	}

	void status_bar::apply_error()
	{
		if (error_label_)
			vgui_widget_set_text(error_label_, error_.c_str());
	}

} /* namespace vgui_editor_kit */
