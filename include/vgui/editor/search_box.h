#pragma once
/*
 * search_box.h - single-line search field with a clear button.
 *
 * The box wraps VGUI_EDIT and reports text changes while typing plus a submit
 * event on Enter.  Programmatic set_text() is silent; clear() behaves like the
 * user pressing the clear button and notifies the change listener.
 */
#include <functional>
#include <string>

#include "toolbar.h"
#include <vgui/vgui.h>

namespace vgui_editor_kit {

	class search_box
	{
	public:
		using change_listener = std::function<void(const std::string&)>;
		using submit_listener = std::function<void(const std::string&)>;

		search_box() = default;
		search_box(const search_box&) = delete;
		search_box& operator=(const search_box&) = delete;

		void build(vgui_widget* parent, std::string placeholder = "Search");
		void set_placeholder(std::string text);

		/* Silent programmatic update. */
		void set_text(std::string text);
		const std::string& text() const noexcept;

		/* User-style clear: updates the text and notifies on change. */
		void clear();
		/* User-style submit: notifies the submit listener with the current text. */
		void submit();

		void focus();
		void set_on_change(change_listener listener);
		void set_on_submit(submit_listener listener);

		vgui_widget* widget() const noexcept;
		vgui_widget* edit_widget() const noexcept;
		vgui_widget* clear_widget() const noexcept;

	private:
		static void on_text(vgui_widget* widget, const char* text, void* user);
		static void on_edit_event(vgui_widget* widget, const vgui_event* event,
			void* user);
		void update_clear_visible();
		void update_text(std::string text, bool notify_change);

		vgui_widget* panel_ = nullptr;
		vgui_widget* edit_ = nullptr;
		icon_button clear_button_;
		std::string text_;
		change_listener change_listener_;
		submit_listener submit_listener_;
	};

} /* namespace vgui_editor_kit */
