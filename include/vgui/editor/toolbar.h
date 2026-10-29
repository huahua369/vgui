#pragma once
/*
 * toolbar.h - toolbar helpers for the editor shell.
 *
 * icon_button wraps a core button and exposes its icon, tooltip, checked
 * (active) and enabled state.  toggle_button adds click-to-toggle behavior;
 * segmented_control groups a row of mutually exclusive toggle buttons.
 * toolbar_view lays the controls out in a horizontal strip and can bind
 * command buttons to a command_registry so enabled/checked state stays in
 * sync with menus and shortcuts.
 *
 * Components are non-copyable because their widget callbacks point back at
 * the owning C++ object.  Keep the component alive for as long as its widget
 * tree is alive (the editor_shell owns its toolbar for that reason).
 */
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <vgui/vgui.h>

namespace vgui_editor_kit {

	class command_registry;

	class icon_button
	{
	public:
		using click_listener = std::function<void()>;

		icon_button() = default;
		icon_button(const icon_button&) = delete;
		icon_button& operator=(const icon_button&) = delete;

		void build(vgui_widget* parent, std::string text = {});
		void set_icon(vgui_texture* texture, int width = 0, int height = 0);
		void set_text(std::string text);
		void set_tooltip(std::string text);
		void set_enabled(bool enabled);
		bool enabled() const;
		void set_checked(bool checked);
		bool checked() const;
		void set_on_click(click_listener listener);
		void click();
		vgui_widget* widget() const noexcept;

	private:
		static void on_event(vgui_widget* widget, const vgui_event* event,
			void* user);

		vgui_widget* button_ = nullptr;
		click_listener listener_;
	};

	class toggle_button
	{
	public:
		using change_listener = std::function<void(bool)>;

		toggle_button() = default;
		toggle_button(const toggle_button&) = delete;
		toggle_button& operator=(const toggle_button&) = delete;

		void build(vgui_widget* parent, std::string text = {});
		void set_icon(vgui_texture* texture, int width = 0, int height = 0);
		void set_text(std::string text);
		void set_tooltip(std::string text);
		void set_enabled(bool enabled);
		bool enabled() const;
		void set_checked(bool checked);
		bool checked() const;
		void toggle();
		void set_on_change(change_listener listener);
		vgui_widget* widget() const noexcept;

	private:
		icon_button button_;
		bool checked_ = false;
		change_listener listener_;
	};

	class segmented_control
	{
	public:
		using change_listener = std::function<void(std::size_t)>;

		segmented_control() = default;
		segmented_control(const segmented_control&) = delete;
		segmented_control& operator=(const segmented_control&) = delete;

		void build(vgui_widget* parent);
		std::size_t add_segment(std::string text);
		void clear();
		std::size_t count() const noexcept;

		/* User-style selection: rejects disabled/out-of-range segments and
		 * notifies the change listener.  set_selected() is the programmatic
		 * form and never notifies. */
		bool select(std::size_t index);
		void set_selected(std::size_t index);
		std::size_t selected() const noexcept;
		bool has_selection() const noexcept;

		void set_segment_enabled(std::size_t index, bool enabled);
		bool segment_enabled(std::size_t index) const;
		void set_on_change(change_listener listener);

		vgui_widget* segment_widget(std::size_t index) const noexcept;
		vgui_widget* widget() const noexcept;

	private:
		void apply_selection(std::size_t index);

		vgui_widget* panel_ = nullptr;
		std::vector<std::unique_ptr<icon_button>> segments_;
		std::size_t selected_ = 0;
		bool has_selection_ = false;
		change_listener listener_;
	};

	class toolbar_view
	{
	public:
		toolbar_view() = default;
		toolbar_view(const toolbar_view&) = delete;
		toolbar_view& operator=(const toolbar_view&) = delete;

		void build(vgui_widget* parent);

		icon_button& add_icon_button(std::string text,
			vgui_texture* icon = nullptr, int icon_width = 0,
			int icon_height = 0);
		toggle_button& add_toggle_button(std::string text,
			bool checked = false);
		segmented_control& add_segmented_control();

		/* Adds a button bound to a command.  The returned pointer is null when
		 * the command id is unknown.  The toolbar only observes the registry;
		 * the registry must outlive the toolbar. */
		icon_button* add_command_button(command_registry& commands,
			const char* id, vgui_texture* icon = nullptr, int icon_width = 0,
			int icon_height = 0);

		vgui_widget* add_label(std::string text, bool grow = false);
		vgui_widget* add_spacer(float grow = 1.0f);
		vgui_widget* add_separator();

		void refresh_commands();
		vgui_widget* widget() const noexcept;

	private:
		struct command_binding
		{
			command_registry* commands = nullptr;
			icon_button* button = nullptr;
			std::string id;
		};

		vgui_widget* panel_ = nullptr;
		std::vector<std::unique_ptr<icon_button>> icon_buttons_;
		std::vector<std::unique_ptr<toggle_button>> toggle_buttons_;
		std::vector<std::unique_ptr<segmented_control>> segmented_controls_;
		std::vector<command_binding> command_bindings_;
	};

} /* namespace vgui_editor_kit */
