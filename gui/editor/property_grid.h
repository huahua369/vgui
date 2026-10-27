#pragma once
/*
 * property_grid.h - typed property inspector for vgui_editor_kit.
 *
 * A grid is a scrollable list of label/editor rows grouped into collapsible
 * categories.  Text, integer, floating point, boolean, enumeration, color
 * (hex) and Vector2/3/4 properties share the same row model, so hosts can
 * build an inspector from property_desc records without knowing which core
 * widget backs each type.
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

#include "color_picker.h"
#include "command_stack.h"
#include "curve_editor.h"
#include "editor_types.h"
#include "gradient_editor.h"
#include "vector_editor.h"
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
		color,
		vector2,
		vector3,
		vector4,
		/* P06 reference rows.  The value is the referenced id (or empty for
		 * none); the host resolves names/thumbnails through the query
		 * handlers below. */
		asset,
		object_ref,
		/* P09 custom editors.  Values are canonical strings produced by
		 * curve_editor/gradient_editor; rows show a compact preview and open
		 * the full editor in a context popup. */
		curve,
		gradient
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
	 *   vector2/3/4 - "x, y" / "x, y, z" / "x, y, z, w"
	 *   curve       - "cubic;0,0,0,0;0.5,1,0,0;1,0,0,0"
	 *   gradient    - "0,#FF0000FF;0.5,#00FF00FF;1,#0000FFFF"
	 *
	 * choices are display labels.  When choice_values is empty, choices are
	 * used for both labels and values.  choice_enabled is optional; when set
	 * it must contain one entry per choice.  allow_empty lets an enumeration
	 * hold the empty value, shown as placeholder (or "(None)" when no
	 * placeholder is set) and offered as the first menu entry.  Vector rows
	 * use a compact component editor: has_range/step apply to every
	 * component, semantic selects the axis labels/colors, and the optional
	 * component_labels/component_colors override them.  get_value/set_value
	 * are optional per-property bindings; a grid-level value source/listener
	 * can be used instead. */
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
		bool allow_empty = false;
		vector_semantic semantic = vector_semantic::generic;
		std::vector<std::string> component_labels;
		std::vector<vgui_color> component_colors;
		/* asset/object_ref rows: host-interpreted type filter (e.g. "mesh"
		 * or ".png;.jpg"); passed through to the query/drop check handlers. */
		std::string asset_filter;
		std::function<std::string()> get_value;
		std::function<void(const std::string&)> set_value;
	};

	/* Result of property_normalize_value(): validates a candidate value for a
	 * property_desc and returns its canonical text form.  Cell editors in
	 * table_view reuse this so typed cell editing follows exactly the same
	 * range/choice/vector rules as PropertyGrid. */
	struct property_value_result
	{
		bool ok = false;
		std::string value;
		std::string error;
	};

	/* Validates and canonicalizes `text` for desc.  This is the same
	 * normalization PropertyGrid applies to programmatic and user edits. */
	property_value_result property_normalize_value(const property_desc& desc,
		std::string text);

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

		/* Number of property rows whose editor widgets are currently
		 * materialized.  With row virtualization this stays bounded by the
		 * visible window even when row_count() is much larger. */
		std::size_t materialized_row_count() const noexcept;

		bool set_category_collapsed(const std::string& category, bool collapsed);
		bool category_collapsed(const std::string& category) const;

		void set_command_stack(command_stack* stack);
		command_stack* command_stack_ptr() const noexcept;

		/* F03/A04: attaches a drop target to the grid that accepts asset
		 * payloads and hands the asset id (drag_payload::id) to the handler.
		 * Reference rows (asset/object_ref) install their own full-row drop
		 * targets and highlight the row while hovering.  The handler returns
		 * true when it assigned the asset. */
		using asset_drop_handler = std::function<bool(const std::string& asset)>;
		void set_asset_drop_handler(asset_drop_handler handler);

		/* P06 reference rows.  The grid never touches an asset database: the
		 * host resolves ids through these handlers. */
		struct asset_info
		{
			std::string name;			/* display name; empty -> raw id */
			std::string kind;			/* host-interpreted type tag */
			vgui_texture* thumbnail = nullptr;	/* optional */
			bool missing = false;		/* id not resolvable */
		};
		/* Resolve a reference for display.  Return false when the id is not
		 * resolvable (the grid reports it through the missing state). */
		using asset_query_fn = std::function<bool(const std::string& id,
			const std::string& filter, asset_info* out)>;
		/* Browse button pressed; the host opens its picker UI. */
		using asset_browse_fn = std::function<void(const std::string& id,
			const std::string& filter)>;
		/* An object_ref row's name was clicked; hosts usually select it. */
		using object_activate_fn = std::function<void(object_id id)>;
		/* Drop acceptance for a reference row; default accepts everything. */
		using asset_filter_check_fn = std::function<bool(const std::string& id,
			const std::string& filter)>;
		/* A row transitioned into the missing state (P07 hook: hosts may
		 * toast/alert; the row itself shows the error state). */
		using missing_asset_fn = std::function<void(const std::string& id,
			const std::string& filter)>;
		void set_asset_query(asset_query_fn handler);
		void set_asset_browse_handler(asset_browse_fn handler);
		void set_object_activated_handler(object_activate_fn handler);
		void set_asset_filter_check(asset_filter_check_fn handler);
		void set_missing_asset_handler(missing_asset_fn handler);

		/* P07: wraps several commits in a single undoable transaction.
		 * Nested batches collapse into the outer one; every commit inside
		 * still applies immediately.  Returns false when no command stack is
		 * attached or the batching is unbalanced. */
		bool begin_batch(const std::string& name);
		bool end_batch();
		std::size_t batch_depth() const noexcept { return batch_depth_; }

		/* P08: multi-target editing.  With a non-empty target list the grid
		 * pulls per-object values through the getter on every refresh: equal
		 * values display normally, differing values switch the row into the
		 * mixed state (placeholder text, neutral swatch, ...).  Commits then
		 * apply the value to every target inside one transaction (one undo)
		 * via the setter; desc.set_value is not called in multi mode.  An
		 * empty target list restores the regular single-object path. */
		using multi_getter_fn = std::function<std::string(const std::string& id,
			object_id target)>;
		using multi_setter_fn = std::function<void(const std::string& id,
			object_id target, const std::string& value)>;
		void set_multi_targets(std::vector<object_id> targets);
		const std::vector<object_id>& multi_targets() const noexcept
		{
			return multi_targets_;
		}
		void set_multi_getter(multi_getter_fn getter);
		void set_multi_setter(multi_setter_fn setter);
		/* True when the row's values across targets differ (display state). */
		bool property_mixed(const std::string& id) const;

		/* P08: property clipboard (in-process, shared across grids).  copy
		 * snapshots id/type/value of every row; paste commits matching rows
		 * (same id AND same type) inside one batch - references paste as
		 * raw ids.  Returns the copied/pasted entry count. */
		std::size_t copy_properties();
		std::size_t paste_properties();
		static std::size_t clipboard_property_count();

		/* P08: per-row favorites ("commonly used" filter).  The star button
		 * toggles a row; set_favorites_only() narrows the visible rows to
		 * favorites (combined with search and category collapse). */
		void set_property_favorite(const std::string& id, bool favorite);
		bool property_favorite(const std::string& id) const;
		std::vector<std::string> favorite_ids() const;
		void set_favorites_only(bool only);

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

		/* The editor widget backing a property (edit box, checkbox, enum
		 * button or vector panel).  Returns nullptr when the row is not
		 * built. */
		vgui_widget* property_editor(const std::string& id) const noexcept;

		/* Vector rows expose their composite editor for host bindings.  The
		 * pointer is owned by the grid and becomes invalid when the row is
		 * removed, filtered out of the materialized window, or the grid is
		 * destroyed. */
		vector_editor* property_vector_editor(const std::string& id) const noexcept;

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
			vector_editor::vector_value drag_start_components{};
			std::string drag_value;

			std::unique_ptr<vector_editor> vector_ui;
			/* asset/object_ref row widgets.  The name label sits in the
			 * editor slot; the thumb doubles as the drop target anchor. */
			vgui_widget* thumb = nullptr;
			vgui_widget* clear_btn = nullptr;
			vgui_widget* browse_btn = nullptr;
			std::string resolved_name;
			bool drop_installed = false;
			bool missing = false;
			/* P08: values differ across multi targets (display state). */
			bool mixed = false;
			bool favorite = false;
			vgui_widget* fav_btn = nullptr;
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
			vgui_widget* header = nullptr;
		};

		struct display_entry
		{
			enum class kind { category, property } type = kind::property;
			std::string key;		// category name or property id
			std::uint64_t token = 0;	// row token for property entries
			float height = 0.0f;
			float offset = 0.0f;		// top offset inside the content panel
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

		void rebuild_display();
		void recompute_display_layout();
		void update_window();
		void refresh_viewport();
		void sync_scroll_to_content();
		vgui_widget* entry_widget(const display_entry& entry);
		void release_row(row& item);
		void release_category(category_ui& ui);
		float row_height(const row& item) const;

		void set_row_error(row& item, std::string error);
		void clear_row_error(row& item);
		void reset_drag_state(row& item);
		void notify_change(const row& item);
		void apply_value(row& item, std::string value);
		void apply_recorded_value(std::uint64_t token, const std::string& value);
		void push_value_command(std::uint64_t token, std::string old_value,
			std::string new_value, std::string command_name);
		bool commit_value(row& item, std::string value, std::string command_name);
		std::string display_text(const row& item) const;

		void open_enum_menu(row& item);
		/* Opens the HSV color picker in a context popup anchored below the
		 * row swatch.  Change previews without a command; gestures commit
		 * one mergeable command; Escape reverts the session start value.
		 */
		class reference_row_drop_target;
		friend class reference_row_drop_target;
		enum class popup_kind
		{
			none = 0,
			color,
			curve,
			gradient
		};
		/* Opens the curve/gradient editor in a context popup anchored below
		 * the row preview. */
		void open_curve_editor(row& item);
		void open_gradient_editor(row& item);
		/* Shared popup session: previews use the row display only (no undo
		 * record), the completed gesture pushes exactly one mergeable
		 * command via commit_value(); Escape restores the session start. */
		void begin_popup_session(row& item, popup_kind kind);
		void reset_popup_session();
		void preview_popup_value(const std::string& id, const std::string& value);
		void commit_popup_value(const std::string& id, const std::string& value);
		void open_color_picker(row& item);
		static void on_popup_close(void* user, int reason);
		void add_recent_color(const std::string& color);
		/* P09 row previews (VGUI_CUSTOM). */
		static void draw_curve_preview(vgui_widget* widget, ovg_ctx_cb* cb,
			rvg_t* vg, float x, float y, float w, float h, float dt,
			void* user);
		static void draw_gradient_preview(vgui_widget* widget, ovg_ctx_cb* cb,
			rvg_t* vg, float x, float y, float w, float h, float dt,
			void* user);
		/* P06 reference rows: resolve via the query handler, refresh thumb/
		 * name/missing state and install the per-row drop target. */
		void refresh_reference_row(row& item);
		bool commit_reference(row& item, const std::string& id);
		bool reference_accepts(const row& item, drag_payload_kind kind) const;

		/* P08 multi-target: pull per-target values, flag the mixed state and
		 * refresh one target after a multi command applied (undo/redo). */
		void sync_row_from_targets(row& item);
		void apply_multi_value(const std::string& id, object_id target,
			const std::string& value);
		bool multi_active() const noexcept
		{
			return !multi_targets_.empty() && multi_getter_ && multi_setter_;
		}
		void update_row_mixed_ui(row& item);

		static void on_search_text(vgui_widget* widget, const char* text, void* user);
		static void on_fav_filter_click(vgui_widget* widget, const vgui_event* event, void* user);
		static void on_edit_event(vgui_widget* widget, const vgui_event* event, void* user);
		static void on_edit_text(vgui_widget* widget, const char* text, void* user);
		static void on_bool_value(vgui_widget* widget, float value, void* user);
		static void on_row_event(vgui_widget* widget, const vgui_event* event, void* user);
		static void on_reset_click(vgui_widget* widget, const vgui_event* event, void* user);
		static void on_category_click(vgui_widget* widget, const vgui_event* event, void* user);
		static void on_menu_activate(vgui_widget* menu, vgui_item_id item, void* user);
		static void on_scroll(vgui_widget* panel, float x, float y, void* user);

		vgui_widget* panel_ = nullptr;
		vgui_widget* search_ = nullptr;
		vgui_widget* fav_filter_ = nullptr;	// P08 收藏过滤开关
		vgui_widget* content_ = nullptr;
		vgui_widget* top_spacer_ = nullptr;
		vgui_widget* window_ = nullptr;
		vgui_widget* bottom_spacer_ = nullptr;
		std::vector<row> rows_;
		std::vector<display_entry> display_;
		std::vector<std::string> category_order_;
		std::map<std::string, category_ui> categories_;
		std::map<std::string, bool> category_collapsed_;
		std::string filter_;
		std::size_t visible_rows_ = 0;
		std::size_t window_begin_ = 0;
		std::size_t window_end_ = 0;
		float content_height_ = 0.0f;
		float viewport_h_ = 0.0f;
		float scroll_y_ = 0.0f;
		bool updating_window_ = false;
		std::uint64_t next_token_ = 1;
		std::uint64_t next_row_id_ = 1;
		std::vector<std::string> recent_colors_;
		std::unique_ptr<color_picker> color_picker_;
		std::unique_ptr<curve_editor> curve_editor_;
		std::unique_ptr<gradient_editor> gradient_editor_;
		popup_kind popup_kind_ = popup_kind::none;
		bool popup_session_active_ = false;
		bool popup_session_mixed_ = false;
		std::string popup_session_start_;
		bool popup_drag_active_ = false;
		std::string popup_drag_value_;
		std::string popup_session_id_;
		std::shared_ptr<int> alive_token_ = std::make_shared<int>(0);

		command_stack* command_stack_ = nullptr;
		change_listener change_listener_;
		value_source value_source_;
		bool read_only_ = false;

		editor_context* selection_context_ = nullptr;
		std::size_t selection_token_ = 0;
		selection_bind_fn selection_bind_;

		asset_drop_handler asset_drop_;
		std::unique_ptr<drop_target> drop_target_;	// F03 资产赋值 drop target

		/* P06 reference-row host bindings. */
		asset_query_fn asset_query_;
		asset_browse_fn asset_browse_;
		object_activate_fn object_activate_;
		asset_filter_check_fn asset_filter_check_;
		missing_asset_fn missing_asset_;
		std::size_t batch_depth_ = 0;

		/* P08 */
		std::vector<object_id> multi_targets_;
		multi_getter_fn multi_getter_;
		multi_setter_fn multi_setter_;
		bool favorites_only_ = false;
		/* 收藏按属性 id 存,clear()/重建后保持。 */
		std::map<std::string, bool> favorites_;
	};

} /* namespace vgui_editor_kit */
