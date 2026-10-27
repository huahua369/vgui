#pragma once
/*
 * table_view.h - virtualized, editable data grid for vgui_editor_kit (T01).
 *
 * The table is a pull-based view over a host-owned data source: row_count is
 * queried once per refresh, cell text only for rows inside the visible window,
 * and sorting keys only for the active sort column.  This keeps large tables
 * independent of the number of rows in both layout and painting work.
 *
 * Columns carry a property_desc so their cell editor is the same typed editor
 * PropertyGrid uses (text, integer/number spinbox, checkbox, enumeration menu,
 * color picker, vector editor, curve/gradient editor).  Values are validated
 * with property_normalize_value() before they are handed to the host.
 *
 * The host owns the data; table_view owns selection, sorting, column widths,
 * scrolling and the ephemeral edit UI.  Widths and sort state round-trip
 * through table_state_to_json()/table_state_from_json() and editor_shell stores
 * the resulting JSON in editor_layout.
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
#include "property_grid.h"
#include "vgui.h"

namespace vgui_editor_kit {

	enum class table_alignment
	{
		left = 0,
		center,
		right
	};

	struct table_cell
	{
		std::size_t row = 0;	/* source row index */
		int column = -1;	/* column index; negative means no cell */

		bool valid() const noexcept { return column >= 0; }
	};

	inline bool operator==(const table_cell& a, const table_cell& b) noexcept
	{
		return a.row == b.row && a.column == b.column;
	}

	inline bool operator!=(const table_cell& a, const table_cell& b) noexcept
	{
		return !(a == b);
	}

	/* Optional per-cell presentation overrides.  A zero colour keeps the
	 * theme default. */
	struct table_cell_style
	{
		vgui_color text_color = 0;
		vgui_color background = 0;
	};

	struct table_column
	{
		std::string id;
		std::string label;
		float width = 120.0f;
		float min_width = 40.0f;
		table_alignment alignment = table_alignment::left;
		bool sortable = true;
		bool editable = true;
		/* Type, choices, range, semantic and validation rules for the cell
		 * editor.  desc.id/label are ignored in favour of the column's. */
		property_desc editor;
		/* Optional column sort comparator.  Returns <0, 0 or >0.  When empty
		 * the table compares numerically for integer/number columns and
		 * case-insensitively for text. */
		std::function<int(const std::string&, const std::string&)> compare;
		/* Applies an edited value to the host model.  Return false to reject.
		 * When empty, table_view calls its edit_handler. */
		std::function<bool(std::size_t row, const std::string& value)> set_value;
	};

	/* Pull-based table model.  cell_text receives the source row index and the
	 * stable column id.  row_id is optional; when set, selection and undo
	 * records follow the row across re-sorts and inserts. */
	struct table_source
	{
		std::function<std::size_t()> row_count;
		std::function<std::string(std::size_t row, const std::string& column)>
			cell_text;
		std::function<object_id(std::size_t row)> row_id;
		std::function<table_cell_style(std::size_t row,
			const std::string& column)> cell_style;
	};

	struct table_row_range
	{
		std::size_t first = 0;
		std::size_t count = 0;

		std::size_t end() const noexcept { return first + count; }
		bool empty() const noexcept { return count == 0; }
	};

	struct table_state
	{
		static constexpr int current_version = 1;

		int version = current_version;
		std::vector<float> column_widths;
		int sort_column = -1;
		bool sort_ascending = true;
	};

	std::string table_state_to_json(const table_state& state);
	bool table_state_from_json(const std::string& text, table_state& out,
		std::string* error = nullptr);

	class spinbox;

	class table_view
	{
	public:
		using selection_listener = std::function<void(const table_cell&)>;
		/* Applies an edit when the column has no set_value callback.  Return
		 * false to reject the value. */
		using edit_handler = std::function<bool(const table_cell&,
			const std::string&)>;
		/* Notification after a value was accepted and handed to the host. */
		using edit_listener = std::function<void(const table_cell&,
			const std::string&)>;

		table_view();
		~table_view();

		table_view(const table_view&) = delete;
		table_view& operator=(const table_view&) = delete;

		void build(vgui_widget* parent);
		vgui_widget* widget() const noexcept;
		bool built() const noexcept;
		void focus();

		/* ---- data source ---------------------------------------------- */
		void set_source(table_source source);
		const table_source& source() const noexcept;

		/* ---- columns -------------------------------------------------- */
		/* Returns false for an empty/duplicate column id.  An empty id is
		 * generated from the column index. */
		bool add_column(table_column column);
		void set_columns(std::vector<table_column> columns);
		void clear_columns();
		const std::vector<table_column>& columns() const noexcept;
		std::size_t column_count() const noexcept;
		bool set_column_width(std::size_t index, float width);
		float column_width(std::size_t index) const noexcept;

		/* ---- rows / sorting ------------------------------------------- */
		/* Re-reads row_count, rebuilds sort keys and clamps scrolling and
		 * selection.  Does not query cell text unless a sort is active. */
		void refresh();
		/* Re-reads one row's content; rebuilds the order when the sorted
		 * column changed. */
		void refresh_row(std::size_t source_row);

		std::size_t row_count() const noexcept;
		std::size_t display_row_count() const noexcept;
		/* Maps a displayed row back to the source row; out of range -> 0. */
		std::size_t source_row_at(std::size_t display_row) const noexcept;
		/* Source row -> displayed row, or -1 when the row is hidden/invalid. */
		int display_index_of(std::size_t source_row) const noexcept;
		table_row_range visible_row_range() const noexcept;

		int sort_column() const noexcept;
		bool sort_ascending() const noexcept;
		/* column < 0 clears the sort.  Returns false when the index is not a
		 * sortable column. */
		bool set_sort(int column, bool ascending);
		void clear_sort();

		/* ---- selection / navigation ----------------------------------- */
		table_cell selected_cell() const noexcept;
		bool select_cell(std::size_t source_row, int column);
		void clear_selection();
		table_cell hit_test(float content_x, float content_y) const;
		void set_selection_listener(selection_listener listener);

		/* ---- geometry -------------------------------------------------- */
		/* Absolute rectangles.  cell_rect works for off-screen rows too; it
		 * simply returns coordinates outside the body. */
		bool cell_rect(std::size_t source_row, int column, float* x, float* y,
			float* width, float* height) const;
		bool header_rect(int column, float* x, float* y, float* width,
			float* height) const;

		float scroll_x() const noexcept;
		float scroll_y() const noexcept;
		void set_scroll(float x, float y);
		float max_scroll_x() const noexcept;
		float max_scroll_y() const noexcept;
		void scroll_to_row(std::size_t source_row);

		void set_row_height(float height);
		float row_height() const noexcept;
		float header_height() const noexcept;

		/* ---- editing --------------------------------------------------- */
		bool begin_edit();
		bool begin_edit(std::size_t source_row, int column);
		/* Closes the popup without applying the pending value. */
		bool cancel_edit();
		bool editing() const noexcept;
		table_cell editing_cell() const noexcept;
		/* Programmatic commit for the active cell.  Returns false when the
		 * value was rejected by validation or by the host setter. */
		bool commit_edit(std::string value);

		void set_edit_handler(edit_handler handler);
		void set_edit_listener(edit_listener listener);
		void set_command_stack(command_stack* stack);
		command_stack* command_stack_ptr() const noexcept;

		/* ---- persistence ----------------------------------------------- */
		table_state capture_state() const;
		/* Applies widths and sort; invalid sort columns are cleared.  Widths
		 * beyond the current column count are ignored. */
		bool apply_state(const table_state& state, std::string* error = nullptr);

		/* Number of cell_text queries performed by the last paint.  Tests and
		 * hosts can use it to verify that only visible rows were queried. */
		std::size_t last_render_cell_queries() const noexcept;

	private:
		struct layout_metrics
		{
			float x = 0.0f, y = 0.0f, width = 0.0f, height = 0.0f;
			float header_x = 0.0f, header_y = 0.0f;
			float header_width = 0.0f, header_height = 0.0f;
			float body_x = 0.0f, body_y = 0.0f;
			float body_width = 0.0f, body_height = 0.0f;
			bool vertical_scrollbar = false;
			bool horizontal_scrollbar = false;
			float vbar_x = 0.0f, vbar_y = 0.0f;
			float vbar_width = 0.0f, vbar_height = 0.0f;
			float hbar_x = 0.0f, hbar_y = 0.0f;
			float hbar_width = 0.0f, hbar_height = 0.0f;
			float columns_width = 0.0f;
			float rows_height = 0.0f;
		};

		static void on_draw(vgui_widget* widget, ovg_ctx_cb* cb, rvg_t* vg,
			float abs_x, float abs_y, float abs_w, float abs_h,
			float delta_time, void* user);
		static int on_event(vgui_widget* widget, const vgui_event* event,
			void* user);

		void draw(ovg_ctx_cb* cb, rvg_t* vg, vgui_widget* widget, float x,
			float y, float width, float height);
		void draw_header(ovg_ctx_cb* cb, rvg_t* vg, vgui_widget* widget,
			const layout_metrics& metrics);
		void draw_body(ovg_ctx_cb* cb, rvg_t* vg, vgui_widget* widget,
			const layout_metrics& metrics);
		void draw_scrollbars(ovg_ctx_cb* cb, rvg_t* vg,
			const layout_metrics& metrics);

		layout_metrics metrics() const;
		float ui_scale() const;
		float column_width_px(std::size_t column) const;
		float row_height_px() const;
		float column_offset(std::size_t column) const;
		int column_at(float content_x) const;
		std::size_t row_at(float content_y) const;
		void clamp_scroll();

		void rebuild_display();
		void notify_selection();
		void notify_edit(const table_cell& cell, const std::string& value);
		const table_column* column_at_index(int index) const;
		const property_desc* editor_desc(int column) const;

		std::string cell_text(std::size_t source_row, int column) const;

		bool apply_cell_value(const table_cell& cell, const std::string& value,
			bool notify);
		void push_cell_command(const table_cell& cell,
			const std::string& old_value, const std::string& new_value);
		object_id row_id_at(std::size_t source_row) const;
		bool find_row_id(object_id id, std::size_t& out) const;

		void ensure_text_editor();
		void ensure_number_editor();
		void ensure_vector_editor(int dimension);
		void ensure_bool_editor();
		void ensure_color_editor();
		void ensure_curve_editor();
		void ensure_gradient_editor();
		void ensure_enum_menu();

		bool begin_popup_edit(vgui_widget* content, const table_cell& cell,
			float x, float y);
		void configure_number_editor(const property_desc& desc,
			const std::string& value);
		void configure_vector_editor(const property_desc& desc,
			const std::string& value);
		void finish_edit_session(bool commit);
		void close_popup();
		void close_menu();
		std::string editor_value_text() const;
		bool commit_current_edit();
		void set_edit_preview(const std::string& value);
		void request_focus();

		static void on_popup_close(void* user, int reason);
		static void on_text_event(vgui_widget* widget, const vgui_event* event,
			void* user);
		static void on_bool_value(vgui_widget* widget, float value, void* user);
		static void on_enum_activate(vgui_widget* menu, vgui_item_id item,
			void* user);
		static void on_enum_close(vgui_widget* menu, void* user);

		vgui_widget* widget_ = nullptr;
		vgui_context* context_ = nullptr;
		table_source source_;
		std::vector<table_column> columns_;
		std::size_t row_count_ = 0;

		/* Sorted display order.  Empty means the natural source order. */
		std::vector<std::size_t> display_;
		int sort_column_ = -1;
		bool sort_ascending_ = true;

		table_cell selected_{};
		object_id selected_row_id_ = invalid_object;
		selection_listener selection_listener_;

		float row_height_ = 24.0f;
		float scroll_x_ = 0.0f;
		float scroll_y_ = 0.0f;
		std::size_t hover_row_ = 0;
		int hover_column_ = -1;

		bool resizing_ = false;
		int resize_column_ = -1;
		float resize_start_x_ = 0.0f;
		float resize_start_width_ = 0.0f;
		int header_press_column_ = -1;
		float header_press_x_ = 0.0f;

		bool dragging_vbar_ = false;
		bool dragging_hbar_ = false;
		float drag_thumb_offset_ = 0.0f;

		enum class edit_kind
		{
			none = 0,
			text,
			number,
			vector,
			boolean,
			enumeration,
			color,
			curve,
			gradient
		};

		bool editing_ = false;
		edit_kind edit_kind_ = edit_kind::none;
		table_cell edit_cell_{};
		std::string edit_original_;
		std::string edit_preview_value_;
		bool edit_preview_active_ = false;
		bool finishing_edit_ = false;

		edit_handler edit_handler_;
		edit_listener edit_listener_;
		command_stack* commands_ = nullptr;

		vgui_widget* popup_content_ = nullptr;
		vgui_widget* edit_text_ = nullptr;
		std::unique_ptr<spinbox> edit_number_;
		vgui_widget* edit_number_panel_ = nullptr;
		std::unique_ptr<vector_editor> edit_vectors_[3];
		vgui_widget* edit_vector_panels_[3] = { nullptr, nullptr, nullptr };
		std::unique_ptr<color_picker> edit_color_;
		std::unique_ptr<curve_editor> edit_curve_;
		std::unique_ptr<gradient_editor> edit_gradient_;
		vgui_widget* edit_bool_ = nullptr;
		vgui_widget* edit_menu_ = nullptr;
		std::map<vgui_item_id, std::string> edit_menu_values_;

		std::size_t render_cell_queries_ = 0;
	};

} /* namespace vgui_editor_kit */
