#pragma once
/*
 * property_grid.h - typed property inspector for vgui_editor_kit.
 *
 * A grid is a scrollable list of label/editor rows grouped into collapsible
 * categories.  Text, integer, floating point, boolean, enumeration and color
 * (hex) properties share the same row model, so hosts can build an inspector
 * from property_desc records without knowing which core widget backs each
 * type.
 *
 * The grid must be destroyed before the vgui context and, when a command stack
 * or editor_context is attached, before those objects.  Committed edits are
 * recorded through command_stack::push(); undo/redo calls the property setter
 * or the grid change listener again so the host model stays in sync.
 */
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "command_stack.h"
#include "editor_types.h"
#include "vgui.h"

namespace vgui_editor_kit {

	class editor_context;

	enum class property_type
	{
		text = 0,
		integer,
		number,
		boolean,
		enumeration,
		color
	};

	/* Describes one editable property.  `value` and `default_value` use the
	 * canonical string form of the type:
	 *   text        - raw UTF-8 text
	 *   integer     - decimal integer, e.g. "12"
	 *   number      - decimal floating point, e.g. "0.5"
	 *   boolean     - "true" / "false"
	 *   enumeration - one of choice_values (or choices when choice_values is
	 *                 empty)
	 *   color       - "#RRGGBB" or "#RRGGBBAA"
	 *
	 * choices are display labels.  When choice_values is empty, choices are
	 * used for both labels and values.  choice_enabled is optional; when set
	 * it must contain one entry per choice.  get_value/set_value are optional
	 * per-property bindings; a grid-level value source/listener can be used
	 * instead. */
	struct property_desc
	{
		std::string id;
		std::string label;
		property_type type = property_type::text;
		std::string value;
		std::string category = "General";
		std::string tooltip;
		std::string placeholder;
		std::string default_value;
		bool has_default = false;
		bool read_only = false;
		bool has_range = false;
		double min_value = 0.0;
		double max_value = 0.0;
		double step = 0.0;
		std::vector<std::string> choices;
		std::vector<std::string> choice_values;
		std::vector<char> choice_enabled;
		std::function<std::string()> get_value;
		std::function<void(const std::string&)> set_value;
	};

	class property_grid
	{
	public:
		using change_listener = std::function<void(const std::string& id,
			const std::string& value)>;
		using value_source = std::function<bool(const std::string& id,
			std::string& out_value)>;
		using selection_bind_fn = std::function<void(property_grid&, object_id)>;

		property_grid() = default;
		~property_grid();

		property_grid(const property_grid&) = delete;
		property_grid& operator=(const property_grid&) = delete;

		void build(vgui_widget* parent);

		/* Removes all rows.  Category collapse state and the search filter are
		 * kept so rebuilding for another object does not lose the user's view
		 * state. */
		void clear();

		/* Compatibility helpers: add_row() creates a text property with an
		 * auto-generated id in the "General" category.  set_row_value() only
		 * updates the displayed/stored value; it never records a command or
		 * notifies the change listener. */
		void add_row(std::string label, std::string value);
		bool set_row_value(std::size_t index, std::string value);

		bool add_property(property_desc desc);
		bool remove_property(const std::string& id);
		bool has_property(const std::string& id) const;

		std::string row_value(std::size_t index) const;
		std::size_t row_count() const noexcept;

		std::string property_value(const std::string& id) const;
		/* Programmatic display/model sync.  Validates the value and updates the
		 * row, but does not notify the listener or push a command. */
		bool set_property_value(const std::string& id, std::string value);
		/* Applies a user-style edit: validates, updates the row, notifies the
		 * change listener and records an undo command when a stack is attached. */
		bool commit_property_value(const std::string& id, std::string value);

		bool reset_property(const std::string& id);
		void reset_all();

		std::string property_error(const std::string& id) const;
		bool set_property_error(const std::string& id, std::string error);

		bool property_read_only(const std::string& id) const;
		bool set_property_read_only(const std::string& id, bool read_only);
		void set_read_only(bool read_only);
		bool read_only() const noexcept;

		void set_filter(std::string text);
		const std::string& filter() const noexcept;
		std::size_t visible_row_count() const noexcept;
		bool property_visible(const std::string& id) const;

		bool set_category_collapsed(const std::string& category, bool collapsed);
		bool category_collapsed(const std::string& category) const;

		void set_command_stack(command_stack* stack);
		command_stack* command_stack_ptr() const noexcept;

		void set_change_listener(change_listener listener);
		void set_value_source(value_source source);
		void refresh();

		/* Rebinds the grid whenever the editor selection changes.  The binder
		 * receives the grid and the new object id and is expected to rebuild
		 * rows with clear()/add_property().  The initial selection is delivered
		 * immediately.  Call unbind_selection() before the editor_context is
		 * destroyed if the grid outlives it. */
		void bind_selection(editor_context& context, selection_bind_fn binder);
		void unbind_selection();

		/* The editor widget backing a property (edit box, checkbox or enum
		 * button).  Returns nullptr when the row is not built. */
		vgui_widget* property_editor(const std::string& id) const noexcept;

		vgui_widget* widget() const noexcept;

	private:
		struct row
		{
			std::uint64_t token = 0;
			property_desc desc;
			std::string value;
			std::string error;
			bool visible = true;

			bool drag_active = false;
			float drag_start_x = 0.0f;
			double drag_start_value = 0.0;
			std::string drag_value;

			vgui_widget* panel = nullptr;
			vgui_widget* line = nullptr;
			vgui_widget* label = nullptr;
			vgui_widget* swatch = nullptr;
			vgui_widget* editor = nullptr;
			vgui_widget* reset = nullptr;
			vgui_widget* error_label = nullptr;
			vgui_widget* menu = nullptr;
			std::map<vgui_item_id, std::string> menu_values;
		};

		struct category_ui
		{
			vgui_widget* panel = nullptr;
			vgui_widget* header = nullptr;
			vgui_widget* rows = nullptr;
		};

		row* find_row(const std::string& id);
		const row* find_row(const std::string& id) const;
		row* find_row_by_token(std::uint64_t token);
		row* row_for_widget(vgui_widget* widget);

		category_ui* ensure_category(const std::string& name);
		void ensure_row_ui(row& item);
		void update_row_ui(row& item, bool sync_editor = true);
		void update_row_error_ui(row& item);
		void update_visibility();
		bool row_matches(const row& item) const;

		void set_row_error(row& item, std::string error);
		void clear_row_error(row& item);
		void notify_change(const row& item);
		void apply_value(row& item, std::string value);
		void apply_recorded_value(std::uint64_t token, const std::string& value);
		bool commit_value(row& item, std::string value, std::string command_name);
		std::string display_text(const row& item) const;

		void open_enum_menu(row& item);
		void open_color_menu(row& item);
		void add_recent_color(const std::string& color);

		static void on_search_text(vgui_widget* widget, const char* text, void* user);
		static void on_edit_event(vgui_widget* widget, const vgui_event* event, void* user);
		static void on_edit_text(vgui_widget* widget, const char* text, void* user);
		static void on_bool_value(vgui_widget* widget, float value, void* user);
		static void on_row_event(vgui_widget* widget, const vgui_event* event, void* user);
		static void on_reset_click(vgui_widget* widget, const vgui_event* event, void* user);
		static void on_category_click(vgui_widget* widget, const vgui_event* event, void* user);
		static void on_menu_activate(vgui_widget* menu, vgui_item_id item, void* user);

		vgui_widget* panel_ = nullptr;
		vgui_widget* search_ = nullptr;
		vgui_widget* content_ = nullptr;
		std::vector<row> rows_;
		std::map<std::string, category_ui> categories_;
		std::map<std::string, bool> category_collapsed_;
		std::string filter_;
		std::size_t visible_rows_ = 0;
		std::uint64_t next_token_ = 1;
		std::uint64_t next_row_id_ = 1;
		std::vector<std::string> recent_colors_;
		std::shared_ptr<int> alive_token_ = std::make_shared<int>(0);

		command_stack* command_stack_ = nullptr;
		change_listener change_listener_;
		value_source value_source_;
		bool read_only_ = false;

		editor_context* selection_context_ = nullptr;
		std::size_t selection_token_ = 0;
		selection_bind_fn selection_bind_;
	};

} /* namespace vgui_editor_kit */
