#pragma once
/*
 * combo_box.h - button plus popup-list selector.
 *
 * The control shows the selected item's label (or a placeholder when nothing
 * is selected) and opens a detached VGUI_MENU below its button.  Opening,
 * closing and choosing an item never move keyboard focus: the menu consumes
 * navigation keys while it is open, and the button keeps the focus it had.
 *
 * Items carry a display label, a value and an enabled flag.  Items may be
 * disabled, and an item may have an empty value or label.  A selected item
 * with an empty label displays the placeholder.  The box also supports no
 * selection at all, which displays the placeholder and reports an empty value.
 *
 * set_items(), set_selected_index(), set_value(), clear_selection() and
 * set_placeholder() are silent.  User gestures (menu activation, closed
 * Up/Down/Home/End, select_index(), select_value(), select_none()) validate
 * disabled items and notify the change and commit listeners exactly once for a
 * completed selection that changes the value.
 *
 * Like the other editor-kit components the combo box is non-copyable because
 * its widget callbacks point back at the C++ object.  Keep it alive for at
 * least as long as its button and menu, and destroy it before the vgui
 * context.
 */
#include <cstddef>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "vgui.h"

namespace vgui_editor_kit {

	class combo_box
	{
	public:
		struct item
		{
			std::string label;
			std::string value;
			bool enabled = true;
		};

		using selection_listener =
			std::function<void(int index, const std::string& value)>;

		combo_box() = default;
		~combo_box();
		combo_box(const combo_box&) = delete;
		combo_box& operator=(const combo_box&) = delete;

		void build(vgui_widget* parent);

		/* Widget id prefix.  Call before build() to name the button and menu;
		 * applying it after build() renames the existing widgets. */
		void set_id(std::string id);
		const std::string& id() const noexcept;

		/* Replaces the item list.  The current value is preserved when an item
		 * with that value still exists; otherwise the selection is cleared.
		 * Closes the popup before rebuilding it. */
		void set_items(std::vector<item> items);
		const std::vector<item>& items() const noexcept;
		std::size_t item_count() const noexcept;
		const item* item_at(int index) const noexcept;
		int index_of_value(const std::string& value) const noexcept;
		bool set_item_enabled(int index, bool enabled);

		void set_placeholder(std::string text);
		const std::string& placeholder() const noexcept;
		/* Selected label, or the placeholder when nothing is selected or the
		 * selected label is empty. */
		std::string display_text() const;

		/* Silent selection changes.  set_selected_index(-1) clears the
		 * selection.  set_value() selects the first item with that value and
		 * returns false when no item matches.  Silent setters may select a
		 * disabled item because the host model can hold a value that is not
		 * currently available; the user-style selectors below reject it. */
		bool set_selected_index(int index);
		bool set_value(const std::string& value);
		void clear_selection();

		/* User-style selection.  Disabled and out-of-range items are rejected.
		 * Selecting the current item succeeds without notifying. */
		bool select_index(int index);
		bool select_value(const std::string& value);
		bool select_none();

		int selected_index() const noexcept;
		const std::string& value() const noexcept;
		const item* selected_item() const noexcept;

		bool open();
		void close();
		bool is_open() const;
		void toggle();

		void set_enabled(bool enabled);
		bool enabled() const noexcept;
		void set_read_only(bool read_only);
		bool read_only() const noexcept;

		void focus();

		void set_on_change(selection_listener listener);
		void set_on_commit(selection_listener listener);

		vgui_widget* widget() const noexcept;

	private:
		static void on_button_event(vgui_widget* widget, const vgui_event* event,
			void* user);
		static void on_menu_activate(vgui_widget* menu, vgui_item_id item,
			void* user);

		void apply_ids();
		void sync_display();
		void sync_enabled();
		void ensure_menu();
		void rebuild_menu();
		void sync_menu_checks();
		bool apply_selection(int index, bool notify);
		vgui_item_id menu_item_for_index(int index) const;
		int next_enabled_index(int from, int direction) const;
		int first_enabled_index() const;
		int last_enabled_index() const;
		void notify_change();
		void notify_commit();

		vgui_widget* button_ = nullptr;
		vgui_widget* menu_ = nullptr;
		std::vector<item> items_;
		std::vector<std::pair<vgui_item_id, int>> menu_items_;
		std::string id_ = "combo_box";
		std::string placeholder_;
		int selected_ = -1;
		bool enabled_ = true;
		bool read_only_ = false;

		selection_listener change_listener_;
		selection_listener commit_listener_;
	};

} /* namespace vgui_editor_kit */
