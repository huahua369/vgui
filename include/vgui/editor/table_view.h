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
#include <algorithm>
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
#include <vgui/vgui.h>

namespace vgui_editor_kit {

	enum class table_alignment
	{
		left = 0,
		center,
		right
	};

	/* How an in-cell image is placed inside its cell rectangle.  Matches the
	 * VGUI_IMAGE fit set so hosts can reuse the same convention. */
	enum class table_image_fit
	{
		contain = 0,	/* scale to fit, letterbox (Excel "Place in Cell") */
		cover,			/* scale to fill, crop overflow */
		stretch,		/* ignore aspect ratio */
		none			/* native size, centred, cropped */
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

	/* A rectangular selection expressed by two corner cells (anchor where the
	 * selection started, focus where it currently ends).  Rows use source row
	 * indices. */
	struct table_selection
	{
		table_cell anchor{};
		table_cell focus{};

		bool valid() const noexcept
		{
			return anchor.valid() && focus.valid();
		}
		/* Inclusive bounds in source-row / column index space. */
		std::size_t first_row() const noexcept
		{
			return std::min(anchor.row, focus.row);
		}
		std::size_t last_row() const noexcept
		{
			return std::max(anchor.row, focus.row);
		}
		int first_column() const noexcept
		{
			return std::min(anchor.column, focus.column);
		}
		int last_column() const noexcept
		{
			return std::max(anchor.column, focus.column);
		}
		std::size_t row_span() const noexcept
		{
			return valid() ? last_row() - first_row() + 1 : 0;
		}
		std::size_t column_span() const noexcept
		{
			return valid()
				? (std::size_t)(last_column() - first_column() + 1) : 0;
		}
		bool contains(std::size_t source_row, int column) const noexcept
		{
			return valid() && source_row >= first_row() &&
				source_row <= last_row() && column >= first_column() &&
				column <= last_column();
		}
	};

	inline bool operator==(const table_selection& a,
		const table_selection& b) noexcept
	{
		return a.anchor == b.anchor && a.focus == b.focus;
	}

	/* Optional per-cell presentation overrides.  A zero colour keeps the
	 * theme default. */
	struct table_cell_style
	{
		vgui_color text_color = 0;
		vgui_color background = 0;
	};

	/* Result of an in-cell image query.  `texture` is BORROWED: the host owns
	 * it and must keep it alive for the duration of the paint that consumes it.
	 * table_view never creates, updates or destroys the texture.  A null
	 * texture means "no image" and the cell falls back to its text. */
	struct table_cell_image
	{
		vgui_texture* texture = nullptr;
		table_image_fit fit = table_image_fit::contain;
		vgui_color tint = 0;	/* 0 keeps the theme/texture colour */
	};

	/* How a floating drawing follows its anchor cells when layout changes.
	 * Mirrors Excel's "move and size" / "move" / "don't move or size". */
	enum class table_drawing_sizing
	{
		move_and_size = 0,	/* follow anchor cells in position and size */
		move_only,			/* follow top-left anchor, keep fixed size */
		fixed				/* stay at fixed content coordinates */
	};

	/* A floating picture anchored to a cell range.  Geometry is recomputed
	 * from the anchors on every paint, so it follows scrolling, column
	 * resizing and re-sorting without storing absolute pixels.  `texture` is
	 * BORROWED: the host owns it and keeps it alive for the paint. */
	struct table_drawing
	{
		std::string id;						/* stable id for selection / undo */
		std::string key;					/* resource key resolved by the host */
		vgui_texture* texture = nullptr;	/* borrowed; null -> not painted */
		table_cell anchor_from{};			/* top-left anchor cell */
		table_cell anchor_to{};				/* bottom-right anchor cell */
		float from_dx = 0.0f, from_dy = 0.0f;	/* inset inside anchor_from */
		float to_dx = 0.0f, to_dy = 0.0f;		/* inset inside anchor_to */
		table_drawing_sizing sizing = table_drawing_sizing::move_and_size;
		/* Logical-pixel size used by move_only/fixed.  Zero means "derive
		 * from the anchors" (useful when the host does not track size). */
		float width = 0.0f;
		float height = 0.0f;
		table_image_fit fit = table_image_fit::stretch;
		float rotation = 0.0f;				/* degrees; reserved for S3 */
		float opacity = 1.0f;
		int z = 0;							/* draw order: ascending */
		std::string alt_text;
	};

	/* S4: a request from the table to the host to add/remove/change a drawing.
	 * The table never decodes files, assigns ids or mutates the host's drawing
	 * collection: it only describes the intent and the host answers by
	 * updating its model and recording one undo step.  `files` are BORROWED and
	 * valid only for the duration of the listener call. */
	enum class table_drawing_request_kind
	{
		insert = 0,	/* new drawing: `files` (drop/paste) or `drawing` (api) */
		remove,		/* delete `id` */
		update		/* replace `id` with `drawing` */
	};

	struct table_drawing_request
	{
		table_drawing_request_kind kind = table_drawing_request_kind::insert;
		/* OS drop / clipboard payload: borrowed paths, empty for api inserts. */
		std::vector<std::string> files;
		/* Cell under the drop/paste point; invalid for api calls. */
		table_cell anchor;
		/* Target of remove/update; empty on insert (host generates the id). */
		std::string id;
		/* Template for insert / full value for update. */
		table_drawing drawing;
		/* "drop" / "paste" / "api" - lets the host word its undo entry. */
		std::string source;
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
		/* Optional in-cell image query, using the same pull model and
		 * column-id convention as cell_text.  Only called for visible cells.
		 * Returning a null texture falls back to cell_text. */
		std::function<table_cell_image(std::size_t row,
			const std::string& column)> cell_image;
		/* Optional floating-drawing layer, pulled once per paint.  The host
		 * should return only drawings intersecting the visible range for very
		 * large sheets; geometry is recomputed from anchors each frame. */
		std::function<std::vector<table_drawing>()> drawings;
	};

	struct table_row_range
	{
		std::size_t first = 0;
		std::size_t count = 0;

		std::size_t end() const noexcept { return first + count; }
		bool empty() const noexcept { return count == 0; }
	};

	/* One column in a multi-column sort.  Order in the vector is priority. */
	struct table_sort_spec
	{
		int column = -1;
		bool ascending = true;

		bool valid() const noexcept { return column >= 0; }
	};

	inline bool operator==(const table_sort_spec& a,
		const table_sort_spec& b) noexcept
	{
		return a.column == b.column && a.ascending == b.ascending;
	}

	struct table_state
	{
		/* Version 2 adds the `sorts` array for multi-column sorting.  Version
		 * 1 files remain readable: they map to a single sort spec. */
		static constexpr int current_version = 2;

		int version = current_version;
		std::vector<float> column_widths;
		/* Primary sort, kept in sync with sorts[0] for v1 compatibility. */
		int sort_column = -1;
		bool sort_ascending = true;
		/* Full sort priority list (empty means unsorted). */
		std::vector<table_sort_spec> sorts;
	};

	std::string table_state_to_json(const table_state& state);
	bool table_state_from_json(const std::string& text, table_state& out,
		std::string* error = nullptr);

	/* S5: optional read-only serialization of the floating-drawing snapshot.
	 * This is a HOST-side helper for saving/restoring drawing geometry (e.g.
	 * in editor_layout); it is deliberately separate from table_state, which
	 * stays at v2.  The borrowed `texture` pointer is never serialized - a
	 * restored drawing has texture == nullptr and the host re-binds it by key.
	 * Fields absent in older JSON default to the same values as a fresh
	 * table_drawing, so forward-compatible additions stay readable. */
	std::string table_drawings_to_json(
		const std::vector<table_drawing>& drawings);
	bool table_drawings_from_json(const std::string& text,
		std::vector<table_drawing>& out, std::string* error = nullptr);

	class spinbox;

	class table_view
	{
	public:
		using selection_listener = std::function<void(const table_cell&)>;
		using selection_range_listener =
			std::function<void(const table_selection&)>;
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
		 * sortable column.  Sets a single-key sort (clears any secondary keys). */
		bool set_sort(int column, bool ascending);
		void clear_sort();

		/* ---- multi-column sorting ------------------------------------ */
		/* The full sort priority list; empty when unsorted. */
		const std::vector<table_sort_spec>& sort_specs() const noexcept;
		/* Replaces the whole sort.  Invalid entries (out of range, duplicate
		 * columns, unsortable) make the call fail with no change. */
		bool set_sort_specs(std::vector<table_sort_spec> specs);
		/* Adds a column as the last priority key, or flips its direction if it
		 * is already present.  Used by Shift+click on a header.  When there is
		 * no existing sort this behaves like set_sort(). */
		bool toggle_sort_spec(int column, bool ascending = true);
		/* Removes one column from the sort list.  Returns false when absent. */
		bool remove_sort_spec(int column);

		/* ---- selection / navigation ----------------------------------- */
		/* The active cell (selection focus).  Single-cell selection remains
		 * the default and keeps the original API unchanged. */
		table_cell selected_cell() const noexcept;
		bool select_cell(std::size_t source_row, int column);
		void clear_selection();
		table_cell hit_test(float content_x, float content_y) const;
		void set_selection_listener(selection_listener listener);

		/* ---- range selection ------------------------------------------ */
		/* The full rectangular selection.  For a single cell anchor == focus. */
		const table_selection& selection() const noexcept;
		/* Selects a rectangle; focus/anchor are normalised from the two given
		 * cells.  The active cell becomes focus. */
		bool select_range(const table_cell& anchor, const table_cell& focus);
		/* Extends the current selection's focus to a new cell (Shift+click /
		 * Shift+arrow).  When there is no selection, starts one. */
		bool extend_selection(const table_cell& focus);
		/* Selects all visible rows across all columns. */
		void select_all();
		/* Selects a whole row (Shift+space) or column (Ctrl+space). */
		bool select_row(std::size_t source_row);
		bool select_column(int column);
		void set_selection_range_listener(selection_range_listener listener);

		/* ---- floating drawings ----------------------------------------- */
		using drawing_selection_listener = std::function<void(const std::string&)>;
		/* Currently selected drawing id; empty when none.  Selecting a drawing
		 * clears the cell selection and vice versa (mutually exclusive). */
		std::string selected_drawing() const noexcept;
		bool select_drawing(const std::string& id);
		void clear_drawing_selection();
		/* Topmost drawing whose rectangle contains the point, or an empty id.
		 * content_x/y use the same space as hit_test(): scaled content pixels,
		 * origin at the body top-left, scroll not yet subtracted. */
		std::string hit_test_drawing(float content_x, float content_y) const;
		/* Drawing rectangle in ABSOLUTE window coordinates (like cell_rect),
		 * recomputed from anchors and current scroll.  False if id absent or
		 * the drawing cannot be resolved (e.g. hidden anchor row). */
		bool drawing_rect(const std::string& id, float* x, float* y,
			float* width, float* height) const;
		/* Number of drawings painted by the last paint (after z-sort / cull). */
		std::size_t last_render_drawing_count() const noexcept;
		void set_drawing_selection_listener(drawing_selection_listener listener);

		/* S5: tooltip text for a drawing - its alt_text, or its key when no
		 * alt text is set.  Returns empty for an unknown id.  The widget's
		 * tooltip provider uses this for the drawing currently under the
		 * pointer; hosts may also query it directly. */
		std::string drawing_tooltip_text(const std::string& id) const;

		/* ---- floating drawing interaction (S3) ------------------------ */
		/* Handles around a selected drawing.  Corners resize (aspect-locked
		 * unless Shift is held), edges resize one axis, rotate spins around
		 * the centre.  move is the drawing body itself. */
		enum class drawing_handle
		{
			none = 0,
			move,
			rotate,
			north_west, north, north_east, east,
			south_east, south, south_west, west
		};

		/* Fired once when a move/resize/rotate gesture is committed, or when a
		 * z-order command runs.  The table never mutates the host's drawing
		 * model: it hands `before` and the computed `after` back so the host
		 * can update its data and record one undo step.  Cancelling a gesture
		 * (Escape) produces no call. */
		using drawing_change_listener = std::function<void(
			const std::string& id,
			const table_drawing& before,
			const table_drawing& after)>;
		void set_drawing_change_listener(drawing_change_listener listener);

		/* Handle under an absolute window-space point for a given drawing.
		 * none when the point misses every handle/body.  Rotation is honoured:
		 * handle positions are rotated with the drawing. */
		drawing_handle hit_test_drawing_handle(const std::string& id,
			float abs_x, float abs_y) const;
		/* Whether a gesture is currently in progress (preview state). */
		bool drawing_gesture_active() const noexcept;

		/* Z-order commands.  Each calls the change listener once with the new
		 * z and returns false only when the id is not present or the value
		 * would not change.  The next paint pulls the re-sorted model. */
		bool bring_drawing_to_front(const std::string& id);
		bool send_drawing_to_back(const std::string& id);
		bool move_drawing_z(const std::string& id, int direction);

		/* ---- drawing insertion requests (S4) -------------------------- */
		/* The host answers insert/remove/update requests: it decodes any
		 * files, assigns stable ids/textures, edits its own drawing model and
		 * records one undo step.  Return false to refuse the request.  Without
		 * a listener, all request APIs are no-ops returning false. */
		using drawing_request_listener =
			std::function<bool(const table_drawing_request&)>;
		void set_drawing_request_listener(drawing_request_listener listener);
		drawing_request_listener drawing_request_listener_fn() const;

		/* Programmatic host API.  Each returns the host's answer. */
		bool insert_drawing(const table_drawing& drawing);
		bool remove_drawing(const std::string& id);
		bool update_drawing(const std::string& id,
			const table_drawing& drawing);

		/* OS file drop entry point (backends call this with absolute window
		 * coordinates).  Files are offered to the host as an insert request
		 * anchored at the cell under (abs_x, abs_y).  Returns true when the
		 * host accepted; false when there is no listener/at a bad location. */
		bool insert_drawings_from_files(
			const std::vector<std::string>& files, float abs_x, float abs_y);

		/* ---- clipboard ------------------------------------------------- */
		using clipboard_get_fn = std::function<std::string()>;
		using clipboard_set_fn = std::function<void(const std::string&)>;
		/* S4: host supplies image paths from the system clipboard; an empty
		 * vector means "no image".  The table turns them into a paste insert
		 * request anchored at the current selection.  Kept separate from the
		 * text handlers so existing callers are unaffected. */
		using clipboard_image_get_fn = std::function<std::vector<std::string>()>;
		/* Host supplies system-clipboard access; the table never depends on a
		 * windowing backend.  With no getter, Ctrl+V is a no-op; with no setter,
		 * copy still returns the TSV but does not write the system clipboard. */
		void set_clipboard_handlers(clipboard_get_fn get,
			clipboard_set_fn set);
		void set_clipboard_image_getter(clipboard_image_get_fn get);
		/* Pastes clipboard images (when a getter is installed and returns
		 * paths) as one insert request; returns false when there is no image
		 * or the host refused.  Text TSV is untouched. */
		bool paste_images_from_clipboard();
		/* Serializes the current selection to TSV (rows by '\\n', columns by
		 * '\\t') in display order.  Returns an empty string when no selection. */
		std::string copy_selection() const;
		/* Copies to the host clipboard (when installed) and returns the TSV. */
		std::string copy_selection_to_clipboard();
		/* Copies then clears every selected editable cell in one transaction. */
		bool cut_selection();
		/* Pastes TSV at the selection's top-left anchor in display order.
		 * Applies through normalization + column set_value as one undo record.
		 * Returns false if no selection, no writable column, or out of range. */
		bool paste_text(const std::string& tsv, std::string* error = nullptr);

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

		/* Number of cell_image queries performed by the last paint.  Mirrors
		 * last_render_cell_queries() for the in-cell image path. */
		std::size_t last_render_cell_image_queries() const noexcept;

	private:
		/* S4 OS-file drop target; defined out-of-line in table_view.cpp and
		 * kept alive for the widget's whole lifetime. */
		struct file_drop_target;
		std::unique_ptr<file_drop_target> file_drop_target_;

		/* Sends a request to the host; marks the widget dirty on acceptance so
		 * the next paint pulls the host-updated drawing model. */
		bool emit_drawing_request(const table_drawing_request& request) const;

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
		/* S5: dynamic tooltip for the hovered drawing's alt text. */
		static const char* on_tooltip(vgui_widget* widget, void* user);

		void draw(ovg_ctx_cb* cb, rvg_t* vg, vgui_widget* widget, float x,
			float y, float width, float height);
		void draw_header(ovg_ctx_cb* cb, rvg_t* vg, vgui_widget* widget,
			const layout_metrics& metrics);
		void draw_body(ovg_ctx_cb* cb, rvg_t* vg, vgui_widget* widget,
			const layout_metrics& metrics);
		/* Floating-drawing overlay: pulls source_.drawings once per paint,
		 * recomputes each rect from anchors, z-sorts and paints inside the
		 * body clip. */
		void draw_drawings(ovg_ctx_cb* cb, rvg_t* vg, vgui_widget* widget,
			const layout_metrics& metrics);
		void draw_scrollbars(ovg_ctx_cb* cb, rvg_t* vg,
			const layout_metrics& metrics);

		/* Content-space rect of a drawing; shared by paint and hit test.  The
		 * returned rect is in scaled content pixels (origin at body top-left,
		 * scroll not subtracted), matching hit_test's input space. */
		bool drawing_content_rect(const table_drawing& drawing, float* x,
			float* y, float* width, float* height) const;
		std::vector<table_drawing> pull_drawings() const;
		const table_drawing* find_drawing(const std::string& id) const;
		/* Absolute window rect of a drawing, optionally overridden by the
		 * live gesture preview (so hit testing follows what is on screen). */
		bool drawing_abs_rect(const table_drawing& drawing, float* x, float* y,
			float* width, float* height) const;
		/* Handle centres in absolute window coordinates for a drawing rect. */
		void drawing_handle_points(float ax, float ay, float aw, float ah,
			float rotation, float points[9][2]) const;
		/* Gesture state machine. */
		void begin_drawing_gesture(const std::string& id, drawing_handle handle,
			float abs_x, float abs_y, bool lock_aspect);
		void update_drawing_gesture(float abs_x, float abs_y, bool lock_aspect);
		void commit_drawing_gesture();
		void cancel_drawing_gesture();
		void emit_drawing_change(const std::string& id,
			const table_drawing& before, const table_drawing& after);

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
		/* TSV parsing: splits into rows/columns, handling quoted fields. */
		static std::vector<std::vector<std::string>> parse_tsv(
			const std::string& text);
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
		/* Full sort priority list (source of truth for sorting). */
		std::vector<table_sort_spec> sort_specs_;
		/* Primary-key mirror, kept for existing callers and getters. */
		int sort_column_ = -1;
		bool sort_ascending_ = true;

		table_cell selected_{};
		object_id selected_row_id_ = invalid_object;
		selection_listener selection_listener_;
		/* Rectangular selection; for single-cell selection anchor == focus. */
		table_selection selection_;
		selection_range_listener selection_range_listener_;

		/* Floating-drawing layer.  `drawings_` is refreshed once per paint and
		 * reused by hit testing so pointer queries do not pull host data. */
		std::vector<table_drawing> drawings_;
		std::string selected_drawing_id_;
		std::string hover_drawing_id_;
		std::string tooltip_text_;	/* scratch backing on_tooltip's c_str() */
		drawing_selection_listener drawing_selection_listener_;
		drawing_change_listener drawing_change_listener_;
		drawing_request_listener drawing_request_listener_;
		drawing_handle hover_drawing_handle_ = drawing_handle::none;

		/* In-flight transform gesture.  `before` is the snapshot at press;
		 * `preview` is the mutated copy painted until commit/cancel.  The
		 * host model is only updated on commit via drawing_change_listener. */
		bool drawing_gesture_active_ = false;
		drawing_handle drawing_gesture_handle_ = drawing_handle::none;
		std::string drawing_gesture_id_;
		table_drawing drawing_gesture_before_;
		/* Live preview in absolute window pixels (scale already applied), plus
		 * the preview rotation.  Overrides the anchor-derived rect while active. */
		float gesture_preview_x_ = 0.0f, gesture_preview_y_ = 0.0f;
		float gesture_preview_w_ = 0.0f, gesture_preview_h_ = 0.0f;
		float gesture_preview_rotation_ = 0.0f;
		/* Base absolute rect captured at press (preview overrides the helper). */
		float gesture_base_x_ = 0.0f, gesture_base_y_ = 0.0f;
		float gesture_base_w_ = 0.0f, gesture_base_h_ = 0.0f;
		float gesture_start_abs_x_ = 0.0f, gesture_start_abs_y_ = 0.0f;
		float gesture_rotation_start_ = 0.0f;	/* pointer angle at press */
		bool gesture_lock_aspect_ = true;

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
		clipboard_get_fn clipboard_get_;
		clipboard_set_fn clipboard_set_;
		clipboard_image_get_fn clipboard_image_get_;
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
		std::size_t render_cell_image_queries_ = 0;
		std::size_t render_drawing_count_ = 0;
	};

} /* namespace vgui_editor_kit */
