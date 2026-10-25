#pragma once
/*
 * status_bar.h - status strip for the editor shell.
 *
 * The status bar shows four independent fields: a free-form message, the
 * current selection, pointer/world coordinates and the latest error.  Fields
 * can be updated before build(); the text is kept and applied once the widget
 * tree exists.  This makes it safe for the shell to register a selection
 * listener in its constructor.
 */
#include <string>

#include "editor_types.h"
#include "vgui.h"

namespace vgui_editor_kit {

	class status_bar
	{
	public:
		status_bar() = default;
		status_bar(const status_bar&) = delete;
		status_bar& operator=(const status_bar&) = delete;

		void build(vgui_widget* parent);

		void set_message(std::string text);
		const std::string& message() const noexcept;

		/* Formats "Selected: <id>"; invalid_object clears the field. */
		void set_selected_object(object_id id);
		void set_selection_text(std::string text);
		const std::string& selection_text() const noexcept;

		/* Formats "x 1.25  y -2.00". */
		void set_coordinates(float x, float y);
		void set_coordinate_text(std::string text);
		const std::string& coordinate_text() const noexcept;

		/* The error field is empty when there is no error. */
		void set_error(std::string text);
		void clear_error();
		const std::string& error() const noexcept;

		vgui_widget* widget() const noexcept;
		vgui_widget* message_widget() const noexcept;
		vgui_widget* selection_widget() const noexcept;
		vgui_widget* coordinate_widget() const noexcept;
		vgui_widget* error_widget() const noexcept;

	private:
		void apply_message();
		void apply_selection();
		void apply_coordinates();
		void apply_error();

		vgui_widget* panel_ = nullptr;
		vgui_widget* message_label_ = nullptr;
		vgui_widget* selection_label_ = nullptr;
		vgui_widget* coordinate_label_ = nullptr;
		vgui_widget* error_label_ = nullptr;

		std::string message_ = "ready";
		std::string selection_;
		std::string coordinates_;
		std::string error_;
	};

} /* namespace vgui_editor_kit */
