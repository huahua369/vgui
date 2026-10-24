#pragma once
/*
 * command_registry.h - string-id editor commands with shortcuts and state.
 *
 * The registry is deliberately independent of widgets: menus, toolbars and
 * keyboard handlers all address the same command id.  Hosts can replace a
 * default command by re-adding the same id, and can supply enabled/checked
 * predicates so menus reflect the current editor state.
 */
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "vgui.h"

namespace vgui_editor_kit {

	/* Portable key combination.  Parsing accepts "Ctrl+S", "Ctrl+Shift+Z",
	 * "Delete" and similar spellings; formatting produces the same shape. */
	struct command_shortcut
	{
		int key = 0;
		int mods = 0;

		bool valid() const noexcept { return key != 0; }
		bool matches(int key, int mods) const noexcept;
		std::string to_string() const;
		static command_shortcut parse(const char* text);
	};

	struct command_entry
	{
		std::string id;
		std::string title;
		command_shortcut shortcut;
		std::function<void()> run;
		std::function<bool()> enabled;
		std::function<bool()> checked;
	};

	class command_registry
	{
	public:
		using action = std::function<void()>;
		using predicate = std::function<bool()>;

		/* Adds a command, or updates the title/action of an existing one while
		 * keeping its shortcut and state predicates.  The returned reference is
		 * invalidated by later add/remove calls. */
		command_entry& add(std::string id, std::string title, action run);
		bool remove(const char* id);
		void clear();

		bool has(const char* id) const;
		std::size_t size() const noexcept;
		const std::vector<command_entry>& entries() const noexcept;

		command_entry* find(const char* id);
		const command_entry* find(const char* id) const;

		bool set_shortcut(const char* id, command_shortcut shortcut);
		bool set_enabled(const char* id, predicate predicate);
		bool set_checked(const char* id, predicate predicate);

		/* Runs the command when it exists and its enabled predicate (if any)
		 * accepts.  Returns true when the action ran. */
		bool invoke(const char* id);

		bool is_enabled(const char* id) const;
		bool is_checked(const char* id) const;

		/* Keyboard dispatch.  Only key-down events trigger commands.  When a
		 * shortcut is registered but its command is disabled, the event is
		 * reported as consumed so the key does not leak into the editor. */
		bool handle_key(int key, int down, int mods);
		bool matches_shortcut(int key, int mods) const;

	private:
		std::vector<command_entry> entries_;
	};

} /* namespace vgui_editor_kit */
