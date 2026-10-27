#include "command_registry.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

namespace vgui_editor_kit {

	namespace {
		constexpr int kModMask = VGUI_MOD_SHIFT | VGUI_MOD_CTRL | VGUI_MOD_ALT | VGUI_MOD_GUI;

		int lower_key(int key)
		{
			if (key >= 'A' && key <= 'Z')
				return key - 'A' + 'a';
			return key;
		}

		std::string trim(const std::string& text)
		{
			std::size_t begin = 0;
			while (begin < text.size() && std::isspace((unsigned char)text[begin]))
				++begin;
			std::size_t end = text.size();
			while (end > begin && std::isspace((unsigned char)text[end - 1]))
				--end;
			return text.substr(begin, end - begin);
		}

		std::string lower(std::string text)
		{
			std::transform(text.begin(), text.end(), text.begin(),
				[](unsigned char c) { return (char)std::tolower(c); });
			return text;
		}

		int parse_key_name(const std::string& raw)
		{
			std::string name = lower(raw);
			if (name.empty())
				return 0;
			if (name.size() == 1)
			{
				unsigned char c = (unsigned char)name[0];
				if (c >= 'a' && c <= 'z')
					return (int)(c - 'a' + 'A');
				return (int)c;
			}
			if (name == "space") return ' ';
			if (name == "backspace" || name == "bksp") return VGUI_KEY_BACKSPACE;
			if (name == "tab") return VGUI_KEY_TAB;
			if (name == "enter" || name == "return") return VGUI_KEY_ENTER;
			if (name == "escape" || name == "esc") return VGUI_KEY_ESCAPE;
			if (name == "delete" || name == "del") return VGUI_KEY_DELETE;
			if (name == "left") return VGUI_KEY_LEFT;
			if (name == "right") return VGUI_KEY_RIGHT;
			if (name == "up") return VGUI_KEY_UP;
			if (name == "down") return VGUI_KEY_DOWN;
			if (name == "home") return VGUI_KEY_HOME;
			if (name == "end") return VGUI_KEY_END;
			if (name == "pageup" || name == "pgup" || name == "page up") return VGUI_KEY_PAGE_UP;
			if (name == "pagedown" || name == "pgdn" || name == "page down") return VGUI_KEY_PAGE_DOWN;
			if (name == "insert" || name == "ins") return VGUI_KEY_INSERT;
			if (name.size() >= 2 && name[0] == 'f')
			{
				const int index = std::atoi(name.c_str() + 1);
				if (index >= 1 && index <= 12)
					return VGUI_KEY_F1 + (index - 1);
			}
			return 0;
		}

		const char* key_name(int key)
		{
			switch (key)
			{
			case VGUI_KEY_BACKSPACE: return "Backspace";
			case VGUI_KEY_TAB: return "Tab";
			case VGUI_KEY_ENTER: return "Enter";
			case VGUI_KEY_ESCAPE: return "Esc";
			case VGUI_KEY_DELETE: return "Delete";
			case VGUI_KEY_LEFT: return "Left";
			case VGUI_KEY_RIGHT: return "Right";
			case VGUI_KEY_UP: return "Up";
			case VGUI_KEY_DOWN: return "Down";
			case VGUI_KEY_HOME: return "Home";
			case VGUI_KEY_END: return "End";
			case VGUI_KEY_PAGE_UP: return "PageUp";
			case VGUI_KEY_PAGE_DOWN: return "PageDown";
			case VGUI_KEY_INSERT: return "Insert";
			case ' ': return "Space";
			default: break;
			}
			if (key >= VGUI_KEY_F1 && key <= VGUI_KEY_F12)
			{
				static const char* function_keys[] = {
					"F1", "F2", "F3", "F4", "F5", "F6",
					"F7", "F8", "F9", "F10", "F11", "F12"
				};
				return function_keys[key - VGUI_KEY_F1];
			}
			return nullptr;
		}
	}

	bool command_shortcut::matches(int other_key, int other_mods) const noexcept
	{
		if (!valid() || lower_key(key) != lower_key(other_key))
			return false;
		return (mods & kModMask) == (other_mods & kModMask);
	}

	std::string command_shortcut::to_string() const
	{
		if (!valid())
			return std::string();
		std::string text;
		if (mods & VGUI_MOD_CTRL) text += "Ctrl+";
		if (mods & VGUI_MOD_SHIFT) text += "Shift+";
		if (mods & VGUI_MOD_ALT) text += "Alt+";
		if (mods & VGUI_MOD_GUI) text += "Super+";

		if (const char* name = key_name(key))
		{
			text += name;
			return text;
		}
		if (key >= 'A' && key <= 'Z')
		{
			text += (char)key;
			return text;
		}
		if (key >= 0x20 && key < 0x7F)
		{
			text += (char)key;
			return text;
		}
		return std::string();
	}

	command_shortcut command_shortcut::parse(const char* text)
	{
		command_shortcut result;
		if (!text || !*text)
			return result;

		std::string remaining = text;
		bool have_key = false;
		std::size_t start = 0;
		while (start <= remaining.size())
		{
			std::size_t plus = remaining.find('+', start);
			std::string token = trim(remaining.substr(start,
				plus == std::string::npos ? std::string::npos : plus - start));
			if (token.empty())
				return command_shortcut();

			std::string name = lower(token);
			if (name == "ctrl" || name == "control")
				result.mods |= VGUI_MOD_CTRL;
			else if (name == "shift")
				result.mods |= VGUI_MOD_SHIFT;
			else if (name == "alt")
				result.mods |= VGUI_MOD_ALT;
			else if (name == "gui" || name == "super" || name == "win" || name == "cmd")
				result.mods |= VGUI_MOD_GUI;
			else
			{
				int key = parse_key_name(token);
				if (!key || have_key)
					return command_shortcut();
				result.key = key;
				have_key = true;
			}

			if (plus == std::string::npos)
				break;
			start = plus + 1;
		}
		return have_key ? result : command_shortcut();
	}

	command_entry& command_registry::add(std::string id, std::string title, action run)
	{
		for (command_entry& entry : entries_)
		{
			if (entry.id != id)
				continue;
			entry.title = std::move(title);
			entry.run = std::move(run);
			return entry;
		}

		command_entry entry;
		entry.id = std::move(id);
		entry.title = std::move(title);
		entry.run = std::move(run);
		entries_.push_back(std::move(entry));
		return entries_.back();
	}

	bool command_registry::remove(const char* id)
	{
		if (!id || !*id)
			return false;
		for (auto it = entries_.begin(); it != entries_.end(); ++it)
		{
			if (it->id == id)
			{
				entries_.erase(it);
				return true;
			}
		}
		return false;
	}

	void command_registry::clear()
	{
		entries_.clear();
	}

	bool command_registry::has(const char* id) const
	{
		return find(id) != nullptr;
	}

	std::size_t command_registry::size() const noexcept
	{
		return entries_.size();
	}

	const std::vector<command_entry>& command_registry::entries() const noexcept
	{
		return entries_;
	}

	command_entry* command_registry::find(const char* id)
	{
		if (!id || !*id)
			return nullptr;
		for (command_entry& entry : entries_)
			if (entry.id == id)
				return &entry;
		return nullptr;
	}

	const command_entry* command_registry::find(const char* id) const
	{
		if (!id || !*id)
			return nullptr;
		for (const command_entry& entry : entries_)
			if (entry.id == id)
				return &entry;
		return nullptr;
	}

	bool command_registry::set_shortcut(const char* id, command_shortcut shortcut)
	{
		command_entry* entry = find(id);
		if (!entry)
			return false;
		entry->shortcut = shortcut;
		return true;
	}

	bool command_registry::set_enabled(const char* id, predicate predicate)
	{
		command_entry* entry = find(id);
		if (!entry)
			return false;
		entry->enabled = std::move(predicate);
		return true;
	}

	bool command_registry::set_checked(const char* id, predicate predicate)
	{
		command_entry* entry = find(id);
		if (!entry)
			return false;
		entry->checked = std::move(predicate);
		return true;
	}

	bool command_registry::invoke(const char* id)
	{
		command_entry* entry = find(id);
		if (!entry)
			return false;
		if (entry->enabled && !entry->enabled())
			return false;
		if (!entry->run)
			return false;
		entry->run();
		return true;
	}

	bool command_registry::is_enabled(const char* id) const
	{
		const command_entry* entry = find(id);
		if (!entry)
			return false;
		return !entry->enabled || entry->enabled();
	}

	bool command_registry::is_checked(const char* id) const
	{
		const command_entry* entry = find(id);
		if (!entry || !entry->checked)
			return false;
		return entry->checked();
	}

	bool command_registry::handle_key(int key, int down, int mods)
	{
		if (!down || !key)
			return false;
		bool matched = false;
		for (command_entry& entry : entries_)
		{
			if (!entry.shortcut.matches(key, mods))
				continue;
			matched = true;
			if (entry.enabled && !entry.enabled())
				continue;
			if (entry.run)
			{
				entry.run();
				return true;
			}
		}
		return matched;
	}

	bool command_registry::matches_shortcut(int key, int mods) const
	{
		for (const command_entry& entry : entries_)
			if (entry.shortcut.matches(key, mods))
				return true;
		return false;
	}

} /* namespace vgui_editor_kit */
