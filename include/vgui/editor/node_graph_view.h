#pragma once
/*
 * node_graph_view.h - node-based graph editor for vgui_editor_kit (T03).
 *
 * The graph owns a small, renderer-independent model: nodes with stable ids,
 * typed input/output ports and cubic bezier connections.  All graph
 * coordinates and view transforms use double, so hit testing, dragging and
 * connection picking keep their precision when the view is panned far from the
 * origin or zoomed far out over a large graph.
 *
 * Interaction:
 *  * LMB on a node selects it.  Ctrl toggles, Shift appends; dragging a
 *    selected node moves the whole selection.  Locked nodes can be selected
 *    but never move.  LMB on a connection selects it; LMB on empty space
 *    starts a marquee that selects intersecting nodes and connections.
 *  * Dragging from an output port to an input port (or the reverse) creates a
 *    connection.  Inputs with multi = false replace their previous connection.
 *    Releasing anywhere else cancels the pending link.
 *  * MMB pans, the wheel zooms around the cursor, Ctrl+A selects everything,
 *    Ctrl+D duplicates selected nodes and their internal connections, Delete
 *    removes the selection, F frames the selection and Home fits the graph.
 *  * Esc, VGUI_EVENT_CANCEL or focus loss restore the gesture snapshot.  A
 *    completed move/connect/disconnect/delete/duplicate pushes one command.
 *  * Ctrl+F focuses the embedded search edit.  Matching nodes stay bright while
 *    the rest are dimmed; Enter cycles through matches and centres the view.
 *  * The bottom-right minimap shows node bounds and the viewport rectangle;
 *    clicking or dragging inside it navigates the graph.
 *  * A context menu provides Add Node, Duplicate, Delete, Disconnect, Frame,
 *    Fit All, Select All and the grid/minimap/snap toggles.  Add Node calls the
 *    host's create_node_handler, so the application decides which node types
 *    exist without adding document concepts to the editor kit.
 *
 * Command integration:
 *  * set_command_stack() makes committed gestures and editing actions push one
 *    command each ("Move nodes", "Connect nodes", "Disconnect nodes",
 *    "Delete nodes", "Duplicate nodes", "Add node").  Direct model edits
 *    through add_node()/add_connection() never record commands; hosts use those
 *    while loading a document.
 *
 * View settings, grid/minimap/snap options and selection persist through
 * node_graph_state_to_json()/node_graph_state_from_json(); editor_shell stores
 * that opaque payload in editor_layout (version 5).
 */
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "command_stack.h"
#include "editor_types.h"
#include <vgui/vgui.h>

namespace vgui_editor_kit {

	struct node_graph_point
	{
		double x = 0.0;
		double y = 0.0;
	};

	struct node_graph_rect
	{
		double x = 0.0;
		double y = 0.0;
		double width = 0.0;
		double height = 0.0;

		double right() const noexcept { return x + width; }
		double bottom() const noexcept { return y + height; }
		bool empty() const noexcept
		{
			return width <= 0.0 || height <= 0.0;
		}
		bool contains(double px, double py) const noexcept
		{
			return px >= x && py >= y && px <= right() && py <= bottom();
		}
	};

	enum class node_port_direction
	{
		input = 0,
		output
	};

	enum class node_port_type
	{
		flow = 0,
		number,
		string,
		boolean,
		color,
		vector,
		object,
		custom
	};

	struct node_port
	{
		/* Stable per-graph id.  add_node()/add_port() allocate one when 0. */
		object_id id = invalid_object;
		std::string name;
		node_port_direction direction = node_port_direction::input;
		node_port_type type = node_port_type::flow;
		/* False inputs accept one connection; a new link replaces the old. */
		bool multi = false;
	};

	struct node_node
	{
		/* Stable per-graph id.  add_node() allocates one when 0. */
		object_id id = invalid_object;
		std::string title;
		std::string category;
		/* Top-left corner in graph coordinates. */
		double x = 0.0;
		double y = 0.0;
		/* Node width in graph units; clamped to a readable range. */
		double width = 180.0;
		bool collapsed = false;
		bool visible = true;
		/* Locked nodes stay selectable but cannot be moved. */
		bool locked = false;
		/* Header accent.  0 uses the theme accent. */
		vgui_color accent = 0;
		std::vector<node_port> inputs;
		std::vector<node_port> outputs;
	};

	struct node_connection
	{
		/* Stable per-graph id.  add_connection() allocates one when 0. */
		object_id id = invalid_object;
		object_id from_node = invalid_object;
		object_id from_port = invalid_object;
		object_id to_node = invalid_object;
		object_id to_port = invalid_object;
		/* 0 uses the source port type colour. */
		vgui_color color = 0;
		bool visible = true;
	};

	struct node_graph_view_state
	{
		/* Graph coordinate displayed at the content top-left. */
		double offset_x = 0.0;
		double offset_y = 0.0;
		/* Positive scale from graph units to content pixels. */
		double zoom = 1.0;
	};

	struct node_graph_state
	{
		static constexpr int current_version = 1;

		int version = current_version;
		node_graph_view_state view{};
		bool grid_visible = true;
		bool minimap_visible = true;
		bool snap_to_grid = true;
		double grid_step = 24.0;
		object_id primary = invalid_object;
		std::vector<object_id> selection;
	};

	std::string node_graph_state_to_json(const node_graph_state& state);
	bool node_graph_state_from_json(const std::string& text,
		node_graph_state& out, std::string* error = nullptr);

	enum class node_graph_gesture
	{
		none = 0,
		pending_move,
		move,
		pending_marquee,
		marquee,
		pan,
		connect,
		minimap
	};

	enum class node_graph_action
	{
		add_node = 0,
		duplicate,
		delete_selection,
		disconnect,
		frame_selection,
		fit_all,
		select_all,
		toggle_grid,
		toggle_minimap,
		toggle_snap
	};

	class node_graph_view
	{
	public:
		using selection_listener =
			std::function<void(const std::vector<object_id>&)>;
		using change_listener = std::function<void()>;
		/* Fill a node descriptor (position is pre-seeded with the context
		 * location) and return false to cancel. */
		using create_node_handler = std::function<bool(node_node&)>;

		node_graph_view() = default;
		~node_graph_view();

		node_graph_view(const node_graph_view&) = delete;
		node_graph_view& operator=(const node_graph_view&) = delete;

		void build(vgui_widget* parent);
		vgui_widget* widget() const noexcept;
		bool built() const noexcept;

		/* ---- model ---------------------------------------------------- */
		/* Returns the node id, or invalid_object for a duplicate explicit id.
		 * Node and port ids share one global space with connections. */
		object_id add_node(node_node node);
		/* Replaces the record and its port lists.  Connections referencing
		 * removed ports are dropped. */
		bool update_node(const node_node& node);
		bool remove_node(object_id id);
		void clear_nodes();
		const std::vector<node_node>& nodes() const noexcept;
		const node_node* find_node(object_id id) const noexcept;
		node_node* find_node(object_id id) noexcept;
		std::size_t node_count() const noexcept;

		/* Adds one port to a node.  Returns the port id or invalid_object. */
		object_id add_port(object_id node, node_port port,
			node_port_direction direction);
		/* Keeps the existing port id; direction changes move the port between
		 * the input/output lists and drop now-invalid connections. */
		bool update_port(object_id port, const node_port& port_record);
		bool remove_port(object_id port);
		const node_port* find_port(object_id port,
			object_id* node = nullptr,
			node_port_direction* direction = nullptr) const noexcept;
		node_port* find_port(object_id port, object_id* node = nullptr,
			node_port_direction* direction = nullptr) noexcept;

		object_id add_connection(node_connection connection);
		bool update_connection(const node_connection& connection);
		bool remove_connection(object_id id);
		void clear_connections();
		const std::vector<node_connection>& connections() const noexcept;
		const node_connection* find_connection(object_id id) const noexcept;
		node_connection* find_connection(object_id id) noexcept;
		std::size_t connection_count() const noexcept;

		/* ---- selection ------------------------------------------------ */
		void set_selection(const std::vector<object_id>& ids,
			object_id primary = invalid_object);
		void clear_selection();
		void select_all();
		const std::vector<object_id>& selection_ids() const noexcept;
		object_id primary_selection() const noexcept;
		bool is_selected(object_id id) const noexcept;
		void set_multi_select(bool enabled);
		bool multi_select() const noexcept;
		void set_selection_listener(selection_listener listener);

		/* Fired for model, view, search and gesture changes (not for selection;
		 * use the selection listener for that). */
		void set_change_listener(change_listener listener);

		/* ---- view / grid ---------------------------------------------- */
		node_graph_view_state view_state() const noexcept;
		/* Cancels an in-flight gesture so the view never changes underneath a
		 * drag snapshot. */
		void set_view_state(const node_graph_view_state& state);
		double zoom() const noexcept;
		void set_zoom_limits(double minimum, double maximum);
		/* Zooms by `factor`, keeping the content point fixed. */
		void zoom_at(float content_x, float content_y, double factor);
		void pan_by_pixels(float delta_x, float delta_y);
		void reset_view();
		void frame_selection();
		void fit_all();

		void set_grid_visible(bool visible);
		bool grid_visible() const noexcept;
		/* Base grid spacing in graph units before adaptive density. */
		void set_grid_step(double step);
		double grid_step() const noexcept;
		double effective_grid_step() const noexcept;
		void set_snap_to_grid(bool enabled);
		bool snap_to_grid() const noexcept;
		node_graph_point snap_point(node_graph_point point) const noexcept;

		void set_minimap_visible(bool visible);
		bool minimap_visible() const noexcept;
		/* Content-local rectangle (inside the 1px border); false when hidden
		 * or the view is too small for a usable minimap. */
		bool minimap_rect(float* x, float* y, float* width,
			float* height) const;

		/* ---- coordinates / geometry ----------------------------------- */
		/* Content-local screen (inside the 1px border) <-> graph. */
		node_graph_point screen_to_graph(float content_x,
			float content_y) const noexcept;
		node_graph_point graph_to_screen(double graph_x,
			double graph_y) const noexcept;
		node_graph_rect node_rect(object_id node) const;
		/* Small square around the port centre in graph coordinates. */
		node_graph_rect port_rect(object_id port) const;
		bool port_position(object_id port, node_graph_point* point) const;
		node_graph_rect selection_bounds() const;
		node_graph_rect content_bounds() const;
		/* 6 screen pixels expressed in graph units at the current zoom. */
		double connection_hit_tolerance() const noexcept;

		/* Content-local coordinates; 0 when nothing is hit.  node_at ignores
		 * ports, port_at ignores node bodies and hit_test prefers ports, then
		 * nodes, then connections. */
		object_id node_at(float content_x, float content_y) const;
		object_id port_at(float content_x, float content_y) const;
		object_id connection_at(float content_x, float content_y) const;
		object_id hit_test(float content_x, float content_y) const;

		/* ---- interaction ---------------------------------------------- */
		bool dragging() const noexcept;
		node_graph_gesture gesture() const noexcept;
		object_id pending_connection_port() const noexcept;
		/* Restores the live gesture snapshot; safe without a gesture. */
		void cancel_gesture();
		void set_drag_threshold(float pixels);
		float drag_threshold() const noexcept;

		/* Creates an output->input link, or the reverse when the arguments are
		 * supplied input-first.  Types must match or be custom. */
		bool connect_ports(object_id port_a, object_id port_b);
		bool disconnect_selection();
		bool delete_selection();
		bool duplicate_selection();

		void set_create_node_handler(create_node_handler handler);
		/* Dispatches a menu action; returns true when something changed. */
		bool perform_action(node_graph_action action);
		vgui_widget* menu() const noexcept;
		/* Opens the context menu at a content-local point and remembers it for
		 * perform_action(add_node). */
		void open_context_menu(float content_x, float content_y);

		/* ---- search ---------------------------------------------------- */
		void set_search_text(std::string text);
		const std::string& search_text() const noexcept;
		void clear_search();
		void focus_search();
		vgui_widget* search_widget() const noexcept;
		std::size_t search_match_count() const noexcept;
		const std::vector<object_id>& search_matches() const noexcept;
		bool node_matches_search(object_id node) const noexcept;
		/* Selects and centres the next matching node. */
		object_id next_search_match();

		void set_command_stack(command_stack* stack);
		command_stack* command_stack_ptr() const noexcept;

		void set_enabled(bool enabled);
		bool enabled() const noexcept;
		void request_focus();
		bool focused() const;

		/* ---- persistence ----------------------------------------------- */
		node_graph_state capture_state() const;
		/* Validates the whole state before mutating anything. */
		bool apply_state(const node_graph_state& state,
			std::string* error = nullptr);

	private:
		struct snapshot
		{
			std::vector<node_node> nodes;
			std::vector<node_connection> connections;
			std::vector<object_id> selection;
			object_id primary = invalid_object;
			object_id next_id = 1;
		};

		static void on_draw(vgui_widget* widget, ovg_ctx_cb* cb, rvg_t* vg,
			float abs_x, float abs_y, float abs_w, float abs_h,
			float delta_time, void* user);
		static int on_event(vgui_widget* widget, const vgui_event* event,
			void* user);
		static void on_focus_change(vgui_context* ctx, vgui_widget* old_focus,
			vgui_widget* new_focus, void* user);
		static void on_search_text(vgui_widget* widget, const char* text,
			void* user);
		static void on_search_event(vgui_widget* widget,
			const vgui_event* event, void* user);
		static void on_menu_activate(vgui_widget* menu, vgui_item_id item,
			void* user);

		bool content_geometry(float* offset_x, float* offset_y, float* width,
			float* height) const;
		float effective_drag_threshold() const;
		bool id_used(object_id id) const noexcept;
		bool port_id_used_elsewhere(object_id id, object_id node) const noexcept;
		object_id allocate_id();
		bool connection_exists(object_id from_port, object_id to_port) const
			noexcept;
		bool ports_compatible(const node_port& a,
			const node_port& b) const noexcept;
		bool resolve_connection_ports(object_id port_a, object_id port_b,
			object_id* from_node, object_id* from_port,
			object_id* to_node, object_id* to_port,
			const node_port** from, const node_port** to) const;
		bool drop_invalid_connections();
		void drop_connections_for_port(object_id port);
		void drop_connections_for_node(object_id node);

		void notify_change();
		void notify_selection();
		void prune_selection();
		void refresh_search_matches();
		void apply_selection(const std::vector<object_id>& picked, int mods);
		std::vector<object_id> entities_in_rect(double x0, double y0, double x1,
			double y1) const;
		void ensure_node_visible(object_id node);
		void apply_view_to_bounds(const node_graph_rect& bounds,
			float margin_pixels);

		/* E01: graph coordinates are logical; screen mapping applies the
		 * context UI scale on top of the user zoom. */
		float ui_scale() const noexcept;
		double effective_zoom() const noexcept;
		double node_header_height() const noexcept;
		double node_row_height() const noexcept;
		double node_height(const node_node& node) const noexcept;
		double port_radius() const noexcept;
		bool port_screen_position(const node_node& node, const node_port& port,
			node_port_direction direction, node_graph_point* graph,
			node_graph_point* screen) const;
		void connection_points(const node_connection& connection,
			node_graph_point* from, node_graph_point* to) const;
		node_graph_point sample_connection(const node_connection& connection,
			double t) const;
		bool curve_intersects_rect(const node_connection& connection,
			const node_graph_rect& rect) const;

		snapshot capture_snapshot() const;
		void restore_snapshot(const snapshot& state);
		void push_command(const char* name, const snapshot& before);
		void update_move(float content_x, float content_y);
		void finish_move();
		bool move_changed() const;
		void cancel_gesture_impl();
		void reset_gesture();
		void navigate_minimap(float content_x, float content_y);
		node_graph_rect minimap_content_rect() const;
		node_graph_rect minimap_graph_bounds() const;
		bool default_node_at(node_node* node) const;

		void draw(ovg_ctx_cb* cb, rvg_t* vg, vgui_widget* widget, float x,
			float y, float width, float height, float delta_time);
		void draw_grid(ovg_ctx_cb* cb, rvg_t* vg, float x, float y,
			float width, float height) const;
		void draw_connections(ovg_ctx_cb* cb, rvg_t* vg,
			vgui_widget* widget, float x, float y, float width,
			float height) const;
		void draw_nodes(ovg_ctx_cb* cb, rvg_t* vg, vgui_widget* widget,
			float x, float y, float width, float height) const;
		void draw_pending_connection(ovg_ctx_cb* cb, rvg_t* vg, float x,
			float y) const;
		void draw_minimap(ovg_ctx_cb* cb, rvg_t* vg, float x, float y,
			float width, float height) const;

		vgui_widget* widget_ = nullptr;
		vgui_widget* search_edit_ = nullptr;
		vgui_widget* menu_ = nullptr;
		vgui_context* context_ = nullptr;
		int focus_token_ = 0;

		std::vector<node_node> nodes_;
		std::vector<node_connection> connections_;
		object_id next_id_ = 1;

		std::vector<object_id> selection_;
		object_id primary_ = invalid_object;
		selection_listener selection_listener_;
		change_listener change_listener_;
		create_node_handler create_node_handler_;

		node_graph_view_state view_{};
		double min_zoom_ = 0.001;
		double max_zoom_ = 32.0;
		bool grid_visible_ = true;
		double grid_step_ = 24.0;
		bool snap_to_grid_ = true;
		bool minimap_visible_ = true;
		float marquee_offset_ = 0.0f;

		std::string search_text_;
		std::vector<object_id> search_matches_;
		std::size_t search_match_index_ = 0;

		command_stack* commands_ = nullptr;
		bool enabled_ = true;
		bool multi_select_ = true;
		float drag_threshold_ = -1.0f;

		node_graph_gesture gesture_ = node_graph_gesture::none;
		float press_x_ = 0.0f;
		float press_y_ = 0.0f;
		float marquee_x_ = 0.0f;
		float marquee_y_ = 0.0f;
		node_graph_point press_graph_{};
		node_graph_view_state gesture_view_{};
		std::vector<node_node> gesture_start_nodes_;
		snapshot gesture_snapshot_{};
		object_id connect_from_port_ = invalid_object;
		object_id connect_to_port_ = invalid_object;
		node_graph_point context_graph_{};

		mutable std::shared_ptr<int> alive_token_ = std::make_shared<int>(0);
	};

} /* namespace vgui_editor_kit */
