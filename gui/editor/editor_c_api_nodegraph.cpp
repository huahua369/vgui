/* gui/editor/editor_c_api_nodegraph.cpp - C facade for the node graph view
 * declared in <vgui/vgui_editor.h>.
 *
 * Node/port/connection getters return C views that borrow handle-owned scratch
 * (strings plus port arrays), valid until the next getter on the same handle.
 * Kit callbacks capture fn/user/owner by value and never the handle, because a
 * shell-borrowed handle can die before the graph.  The create-node handler
 * round-trips its descriptor through lambda-local scratch, which the kit only
 * reads before the call returns. */
#include "editor_c_api.h"
#include "editor_c_api_borrow.h"

#include <cstring>
#include <new>
#include <string>
#include <vector>

#include <vgui/editor/node_graph_view.h>

using namespace vgui_editor_kit;

/* -------------------------------------------------------------------------- */
/* Handle storage                                                             */
/* -------------------------------------------------------------------------- */

struct vgui_editor_node_graph : vgeditor::c_object<node_graph_view> {
	using c_object::c_object;

	/* Borrowed-string scratch for the last node/port getter. */
	std::string scratch;
	std::string title;
	std::string category;
	std::string port_name;
	std::vector<std::string> port_names;
	std::vector<vgui_editor_node_port> input_view;
	std::vector<vgui_editor_node_port> output_view;

	/* C callback slots. */
	vgeditor::callback_slot<vgui_editor_node_selection_fn> on_selection;
	vgeditor::callback_slot<vgui_editor_node_change_fn> on_change;
	vgeditor::callback_slot<vgui_editor_node_create_fn> create_node;
};

namespace {

	static_assert((int)VGUI_EDITOR_NODE_PORT_INPUT ==
		(int)node_port_direction::input, "port direction drift");
	static_assert((int)VGUI_EDITOR_NODE_PORT_OUTPUT ==
		(int)node_port_direction::output, "port direction drift");

	static_assert((int)VGUI_EDITOR_NODE_TYPE_FLOW == (int)node_port_type::flow,
		"port type drift");
	static_assert((int)VGUI_EDITOR_NODE_TYPE_NUMBER == (int)node_port_type::number,
		"port type drift");
	static_assert((int)VGUI_EDITOR_NODE_TYPE_STRING == (int)node_port_type::string,
		"port type drift");
	static_assert((int)VGUI_EDITOR_NODE_TYPE_BOOLEAN ==
		(int)node_port_type::boolean, "port type drift");
	static_assert((int)VGUI_EDITOR_NODE_TYPE_COLOR == (int)node_port_type::color,
		"port type drift");
	static_assert((int)VGUI_EDITOR_NODE_TYPE_VECTOR == (int)node_port_type::vector,
		"port type drift");
	static_assert((int)VGUI_EDITOR_NODE_TYPE_OBJECT == (int)node_port_type::object,
		"port type drift");
	static_assert((int)VGUI_EDITOR_NODE_TYPE_CUSTOM == (int)node_port_type::custom,
		"port type drift");

	static_assert((int)VGUI_EDITOR_NODE_GESTURE_NONE ==
		(int)node_graph_gesture::none, "gesture drift");
	static_assert((int)VGUI_EDITOR_NODE_GESTURE_PENDING_MOVE ==
		(int)node_graph_gesture::pending_move, "gesture drift");
	static_assert((int)VGUI_EDITOR_NODE_GESTURE_MOVE ==
		(int)node_graph_gesture::move, "gesture drift");
	static_assert((int)VGUI_EDITOR_NODE_GESTURE_PENDING_MARQUEE ==
		(int)node_graph_gesture::pending_marquee, "gesture drift");
	static_assert((int)VGUI_EDITOR_NODE_GESTURE_MARQUEE ==
		(int)node_graph_gesture::marquee, "gesture drift");
	static_assert((int)VGUI_EDITOR_NODE_GESTURE_PAN ==
		(int)node_graph_gesture::pan, "gesture drift");
	static_assert((int)VGUI_EDITOR_NODE_GESTURE_CONNECT ==
		(int)node_graph_gesture::connect, "gesture drift");
	static_assert((int)VGUI_EDITOR_NODE_GESTURE_MINIMAP ==
		(int)node_graph_gesture::minimap, "gesture drift");

	static_assert((int)VGUI_EDITOR_NODE_ACTION_ADD_NODE ==
		(int)node_graph_action::add_node, "action drift");
	static_assert((int)VGUI_EDITOR_NODE_ACTION_DUPLICATE ==
		(int)node_graph_action::duplicate, "action drift");
	static_assert((int)VGUI_EDITOR_NODE_ACTION_DELETE_SELECTION ==
		(int)node_graph_action::delete_selection, "action drift");
	static_assert((int)VGUI_EDITOR_NODE_ACTION_DISCONNECT ==
		(int)node_graph_action::disconnect, "action drift");
	static_assert((int)VGUI_EDITOR_NODE_ACTION_FRAME_SELECTION ==
		(int)node_graph_action::frame_selection, "action drift");
	static_assert((int)VGUI_EDITOR_NODE_ACTION_FIT_ALL ==
		(int)node_graph_action::fit_all, "action drift");
	static_assert((int)VGUI_EDITOR_NODE_ACTION_SELECT_ALL ==
		(int)node_graph_action::select_all, "action drift");
	static_assert((int)VGUI_EDITOR_NODE_ACTION_TOGGLE_GRID ==
		(int)node_graph_action::toggle_grid, "action drift");
	static_assert((int)VGUI_EDITOR_NODE_ACTION_TOGGLE_MINIMAP ==
		(int)node_graph_action::toggle_minimap, "action drift");
	static_assert((int)VGUI_EDITOR_NODE_ACTION_TOGGLE_SNAP ==
		(int)node_graph_action::toggle_snap, "action drift");

	node_port_direction to_direction(vgui_editor_node_port_direction dir)
	{
		return dir == VGUI_EDITOR_NODE_PORT_OUTPUT
			? node_port_direction::output
			: node_port_direction::input;
	}

	vgui_editor_node_port_direction from_direction(node_port_direction dir)
	{
		return dir == node_port_direction::output ? VGUI_EDITOR_NODE_PORT_OUTPUT
			: VGUI_EDITOR_NODE_PORT_INPUT;
	}

	node_port_type to_port_type(vgui_editor_node_port_type type)
	{
		switch (type) {
		case VGUI_EDITOR_NODE_TYPE_NUMBER:
			return node_port_type::number;
		case VGUI_EDITOR_NODE_TYPE_STRING:
			return node_port_type::string;
		case VGUI_EDITOR_NODE_TYPE_BOOLEAN:
			return node_port_type::boolean;
		case VGUI_EDITOR_NODE_TYPE_COLOR:
			return node_port_type::color;
		case VGUI_EDITOR_NODE_TYPE_VECTOR:
			return node_port_type::vector;
		case VGUI_EDITOR_NODE_TYPE_OBJECT:
			return node_port_type::object;
		case VGUI_EDITOR_NODE_TYPE_CUSTOM:
			return node_port_type::custom;
		default:
			return node_port_type::flow;
		}
	}

	vgui_editor_node_port_type from_port_type(node_port_type type)
	{
		switch (type) {
		case node_port_type::number:
			return VGUI_EDITOR_NODE_TYPE_NUMBER;
		case node_port_type::string:
			return VGUI_EDITOR_NODE_TYPE_STRING;
		case node_port_type::boolean:
			return VGUI_EDITOR_NODE_TYPE_BOOLEAN;
		case node_port_type::color:
			return VGUI_EDITOR_NODE_TYPE_COLOR;
		case node_port_type::vector:
			return VGUI_EDITOR_NODE_TYPE_VECTOR;
		case node_port_type::object:
			return VGUI_EDITOR_NODE_TYPE_OBJECT;
		case node_port_type::custom:
			return VGUI_EDITOR_NODE_TYPE_CUSTOM;
		default:
			return VGUI_EDITOR_NODE_TYPE_FLOW;
		}
	}

	vgui_editor_node_graph_gesture from_gesture(node_graph_gesture gesture)
	{
		switch (gesture) {
		case node_graph_gesture::pending_move:
			return VGUI_EDITOR_NODE_GESTURE_PENDING_MOVE;
		case node_graph_gesture::move:
			return VGUI_EDITOR_NODE_GESTURE_MOVE;
		case node_graph_gesture::pending_marquee:
			return VGUI_EDITOR_NODE_GESTURE_PENDING_MARQUEE;
		case node_graph_gesture::marquee:
			return VGUI_EDITOR_NODE_GESTURE_MARQUEE;
		case node_graph_gesture::pan:
			return VGUI_EDITOR_NODE_GESTURE_PAN;
		case node_graph_gesture::connect:
			return VGUI_EDITOR_NODE_GESTURE_CONNECT;
		case node_graph_gesture::minimap:
			return VGUI_EDITOR_NODE_GESTURE_MINIMAP;
		default:
			return VGUI_EDITOR_NODE_GESTURE_NONE;
		}
	}

	node_graph_action to_action(vgui_editor_node_graph_action action)
	{
		switch (action) {
		case VGUI_EDITOR_NODE_ACTION_DUPLICATE:
			return node_graph_action::duplicate;
		case VGUI_EDITOR_NODE_ACTION_DELETE_SELECTION:
			return node_graph_action::delete_selection;
		case VGUI_EDITOR_NODE_ACTION_DISCONNECT:
			return node_graph_action::disconnect;
		case VGUI_EDITOR_NODE_ACTION_FRAME_SELECTION:
			return node_graph_action::frame_selection;
		case VGUI_EDITOR_NODE_ACTION_FIT_ALL:
			return node_graph_action::fit_all;
		case VGUI_EDITOR_NODE_ACTION_SELECT_ALL:
			return node_graph_action::select_all;
		case VGUI_EDITOR_NODE_ACTION_TOGGLE_GRID:
			return node_graph_action::toggle_grid;
		case VGUI_EDITOR_NODE_ACTION_TOGGLE_MINIMAP:
			return node_graph_action::toggle_minimap;
		case VGUI_EDITOR_NODE_ACTION_TOGGLE_SNAP:
			return node_graph_action::toggle_snap;
		default:
			return node_graph_action::add_node;
		}
	}

	/* ---- C struct -> kit record ------------------------------------------ */

	node_port to_port(const vgui_editor_node_port& d)
	{
		node_port out;
		out.id = (object_id)d.id;
		out.name = vgeditor::to_text(d.name);
		out.direction = to_direction(d.direction);
		out.type = to_port_type(d.type);
		out.multi = d.multi != 0;
		return out;
	}

	std::vector<node_port> to_ports(const vgui_editor_node_port* src, size_t count)
	{
		std::vector<node_port> out;
		if (!src)
			return out;
		out.reserve(count);
		for (size_t i = 0; i < count; ++i) {
			const vgui_editor_node_port d = vgeditor::take_desc(src + i);
			out.push_back(to_port(d));
		}
		return out;
	}

	node_node to_node(const vgui_editor_node_node* src)
	{
		node_node out;
		if (!src)
			return out;
		const vgui_editor_node_node d = vgeditor::take_desc(src);
		out.id = (object_id)d.id;
		out.title = vgeditor::to_text(d.title);
		out.category = vgeditor::to_text(d.category);
		out.x = d.x;
		out.y = d.y;
		out.width = d.width;
		out.collapsed = d.collapsed != 0;
		out.visible = d.visible != 0;
		out.locked = d.locked != 0;
		out.accent = d.accent;
		out.inputs = to_ports(d.inputs, d.input_count);
		out.outputs = to_ports(d.outputs, d.output_count);
		return out;
	}

	node_connection to_connection(const vgui_editor_node_connection* src)
	{
		node_connection out;
		if (!src)
			return out;
		const vgui_editor_node_connection d = vgeditor::take_desc(src);
		out.id = (object_id)d.id;
		out.from_node = (object_id)d.from_node;
		out.from_port = (object_id)d.from_port;
		out.to_node = (object_id)d.to_node;
		out.to_port = (object_id)d.to_port;
		out.color = d.color;
		out.visible = d.visible != 0;
		return out;
	}

	/* ---- kit record -> C struct ------------------------------------------- */

	/* `names` borrows the caller's port-name strings; the vectors must already
	 * hold every entry so no push can invalidate an earlier c_str(). */
	void fill_port_view(std::vector<vgui_editor_node_port>& view,
		const std::vector<node_port>& ports, const std::string* names)
	{
		view.clear();
		view.resize(ports.size());
		for (size_t i = 0; i < ports.size(); ++i) {
			vgui_editor_node_port& out_port = view[i];
			std::memset(&out_port, 0, sizeof(out_port));
			out_port.struct_size = (uint32_t)sizeof(out_port);
			out_port.id = (vgui_editor_object_id)ports[i].id;
			out_port.name = names[i].c_str();
			out_port.direction = from_direction(ports[i].direction);
			out_port.type = from_port_type(ports[i].type);
			out_port.multi = ports[i].multi ? 1 : 0;
		}
	}

	/* Headroom the create-node callback writes into: a fresh node has no ports,
	 * so the scratch it sees carries VGUI_EDITOR_NODE_CREATE_PORTS blank slots
	 * past the reported count.  `names` must already hold one empty string per
	 * slot (no growth after the pointers are taken). */
	void append_blank_ports(std::vector<vgui_editor_node_port>& view,
		const std::string* names, size_t count)
	{
		const size_t base = view.size();
		view.resize(base + count);
		for (size_t i = 0; i < count; ++i) {
			vgui_editor_node_port& port = view[base + i];
			std::memset(&port, 0, sizeof(port));
			port.struct_size = (uint32_t)sizeof(port);
			port.id = VGUI_EDITOR_INVALID_OBJECT;
			port.name = names[i].c_str();
			port.direction = VGUI_EDITOR_NODE_PORT_INPUT;
			port.type = VGUI_EDITOR_NODE_TYPE_FLOW;
		}
	}

	void fill_node_view(vgui_editor_node_node* out, const node_node& node,
		const std::string& title, const std::string& category,
		std::vector<vgui_editor_node_port>& inputs,
		std::vector<vgui_editor_node_port>& outputs)
	{
		std::memset(out, 0, sizeof(*out));
		out->struct_size = (uint32_t)sizeof(*out);
		out->id = (vgui_editor_object_id)node.id;
		out->title = title.c_str();
		out->category = category.c_str();
		out->x = node.x;
		out->y = node.y;
		out->width = node.width;
		out->collapsed = node.collapsed ? 1 : 0;
		out->visible = node.visible ? 1 : 0;
		out->locked = node.locked ? 1 : 0;
		out->accent = node.accent;
		out->inputs = inputs.empty() ? nullptr : inputs.data();
		out->input_count = inputs.size();
		out->outputs = outputs.empty() ? nullptr : outputs.data();
		out->output_count = outputs.size();
	}

	/* Fills the handle scratch, then points the C view at it. */
	void capture_node(vgui_editor_node_graph* graph, const node_node& node,
		vgui_editor_node_node* out)
	{
		graph->title = node.title;
		graph->category = node.category;

		const size_t in_count = node.inputs.size();
		graph->port_names.clear();
		graph->port_names.reserve(in_count + node.outputs.size());
		for (const node_port& port : node.inputs)
			graph->port_names.push_back(port.name);
		for (const node_port& port : node.outputs)
			graph->port_names.push_back(port.name);

		const std::string* names = graph->port_names.data();
		fill_port_view(graph->input_view, node.inputs, names);
		fill_port_view(graph->output_view, node.outputs, names + in_count);
		fill_node_view(out, node, graph->title, graph->category,
			graph->input_view, graph->output_view);
	}

	void capture_port(vgui_editor_node_graph* graph, const node_port& port,
		vgui_editor_node_port* out)
	{
		graph->port_name = port.name;
		std::memset(out, 0, sizeof(*out));
		out->struct_size = (uint32_t)sizeof(*out);
		out->id = (vgui_editor_object_id)port.id;
		out->name = graph->port_name.c_str();
		out->direction = from_direction(port.direction);
		out->type = from_port_type(port.type);
		out->multi = port.multi ? 1 : 0;
	}

	void capture_connection(const node_connection& connection,
		vgui_editor_node_connection* out)
	{
		std::memset(out, 0, sizeof(*out));
		out->struct_size = (uint32_t)sizeof(*out);
		out->id = (vgui_editor_object_id)connection.id;
		out->from_node = (vgui_editor_object_id)connection.from_node;
		out->from_port = (vgui_editor_object_id)connection.from_port;
		out->to_node = (vgui_editor_object_id)connection.to_node;
		out->to_port = (vgui_editor_object_id)connection.to_port;
		out->color = connection.color;
		out->visible = connection.visible ? 1 : 0;
	}

	/* A host editing the in-place arrays may grow the count; reading past our
	 * scratch would be UB, so cap it when the array pointer is still ours. */
	size_t safe_count(const vgui_editor_node_port* src, size_t count,
		const std::vector<vgui_editor_node_port>& scratch)
	{
		if (!src)
			return 0;
		if (src == scratch.data() && count > scratch.size())
			return scratch.size();
		return count;
	}

	/* Installs the C create-node callback as a kit handler.  Every string the
	 * view borrows lives in the lambda's own scope: the handle pointer must not
	 * appear here, and the kit reads the record back before returning. */
	void install_create_node(vgui_editor_node_graph* graph,
		vgui_editor_node_create_fn fn, vgeditor::user_owner_ptr owner)
	{
		graph->impl.set_create_node_handler([fn, owner](node_node& node) -> bool {
			std::string title = node.title;
			std::string category = node.category;
			const size_t real_inputs = node.inputs.size();
			const size_t real_outputs = node.outputs.size();
			std::vector<std::string> port_names;
			port_names.reserve(real_inputs + real_outputs
				+ 2 * VGUI_EDITOR_NODE_CREATE_PORTS);
			for (const node_port& port : node.inputs)
				port_names.push_back(port.name);
			for (const node_port& port : node.outputs)
				port_names.push_back(port.name);
			for (size_t i = 0; i < 2 * VGUI_EDITOR_NODE_CREATE_PORTS; ++i)
				port_names.emplace_back();

			std::vector<vgui_editor_node_port> inputs;
			std::vector<vgui_editor_node_port> outputs;
			const size_t blank_base = real_inputs + real_outputs;
			fill_port_view(inputs, node.inputs, port_names.data());
			append_blank_ports(inputs, port_names.data() + blank_base,
				VGUI_EDITOR_NODE_CREATE_PORTS);
			fill_port_view(outputs, node.outputs, port_names.data() + real_inputs);
			append_blank_ports(outputs,
				port_names.data() + blank_base + VGUI_EDITOR_NODE_CREATE_PORTS,
				VGUI_EDITOR_NODE_CREATE_PORTS);

			vgui_editor_node_node view;
			fill_node_view(&view, node, title, category, inputs, outputs);
			/* The counts the host sees describe the filled ports only; the extra
			 * slots are writable headroom it may publish by raising them. */
			view.input_count = real_inputs;
			view.output_count = real_outputs;
			if (!fn(owner->user, &view))
				return false;

			node.id = (object_id)view.id;
			node.title = vgeditor::to_text(view.title);
			node.category = vgeditor::to_text(view.category);
			node.x = view.x;
			node.y = view.y;
			node.width = view.width;
			node.collapsed = view.collapsed != 0;
			node.visible = view.visible != 0;
			node.locked = view.locked != 0;
			node.accent = view.accent;
			node.inputs = to_ports(view.inputs, safe_count(view.inputs,
				view.input_count, inputs));
			node.outputs = to_ports(view.outputs, safe_count(view.outputs,
				view.output_count, outputs));
			return true;
		});
	}

	/* Copies a kit point/rect into the C value structs (returned by value). */
	vgui_editor_node_point to_c_point(const node_graph_point& point)
	{
		vgui_editor_node_point out = { point.x, point.y };
		return out;
	}

	vgui_editor_node_rect to_c_rect(const node_graph_rect& rect)
	{
		vgui_editor_node_rect out = { rect.x, rect.y, rect.width, rect.height };
		return out;
	}

} /* namespace */

extern "C" {

	void vgui_editor_node_port_default(vgui_editor_node_port* port)
	{
		if (!port)
			return;
		std::memset(port, 0, sizeof(*port));
		port->struct_size = (uint32_t)sizeof(*port);
		port->id = VGUI_EDITOR_INVALID_OBJECT;
		port->direction = VGUI_EDITOR_NODE_PORT_INPUT;
		port->type = VGUI_EDITOR_NODE_TYPE_FLOW;
		port->multi = 0;
	}

	void vgui_editor_node_node_default(vgui_editor_node_node* node)
	{
		if (!node)
			return;
		std::memset(node, 0, sizeof(*node));
		node->struct_size = (uint32_t)sizeof(*node);
		node->id = VGUI_EDITOR_INVALID_OBJECT;
		node->width = 180.0;
		node->visible = 1;
	}

	void vgui_editor_node_connection_default(
		vgui_editor_node_connection* connection)
	{
		if (!connection)
			return;
		std::memset(connection, 0, sizeof(*connection));
		connection->struct_size = (uint32_t)sizeof(*connection);
		connection->id = VGUI_EDITOR_INVALID_OBJECT;
		connection->from_node = VGUI_EDITOR_INVALID_OBJECT;
		connection->from_port = VGUI_EDITOR_INVALID_OBJECT;
		connection->to_node = VGUI_EDITOR_INVALID_OBJECT;
		connection->to_port = VGUI_EDITOR_INVALID_OBJECT;
		connection->visible = 1;
	}

	void vgui_editor_node_view_state_default(vgui_editor_node_view_state* state)
	{
		if (!state)
			return;
		std::memset(state, 0, sizeof(*state));
		state->struct_size = (uint32_t)sizeof(*state);
		state->zoom = 1.0;
	}

	vgui_editor_node_graph* vgui_editor_node_graph_create(vgui_widget* parent)
	{
		if (!parent)
			return nullptr;
		std::unique_ptr<vgui_editor_node_graph> graph(
			vgeditor::own_object<vgui_editor_node_graph>(
				new (std::nothrow) node_graph_view()));
		if (!graph)
			return nullptr;
		graph->impl.build(parent);
		if (!graph->impl.widget())
			return nullptr;
		return graph.release();
	}

	void vgui_editor_node_graph_destroy(vgui_editor_node_graph* graph)
	{
		/* A shell-borrowed handle must not tear down the shell's graph. */
		if (!graph || !graph->owns_object())
			return;
		vgui_widget* widget = graph->impl.widget();
		graph->impl.set_selection_listener(nullptr);
		graph->impl.set_change_listener(nullptr);
		graph->impl.set_create_node_handler(nullptr);
		graph->impl.set_command_stack(nullptr);
		delete graph;
		if (widget)
			vgui_widget_destroy(widget);
	}

	vgui_widget* vgui_editor_node_graph_widget(vgui_editor_node_graph* graph)
	{
		return graph ? graph->impl.widget() : nullptr;
	}

	void vgui_editor_node_graph_focus(vgui_editor_node_graph* graph)
	{
		if (!graph)
			return;
		graph->impl.request_focus();
	}

	int vgui_editor_node_graph_focused(vgui_editor_node_graph* graph)
	{
		return graph && graph->impl.focused() ? 1 : 0;
	}

	int vgui_editor_node_graph_built(vgui_editor_node_graph* graph)
	{
		return graph && graph->impl.built() ? 1 : 0;
	}

	void vgui_editor_node_graph_set_enabled(vgui_editor_node_graph* graph,
		int enabled)
	{
		if (!graph)
			return;
		graph->impl.set_enabled(enabled != 0);
	}

	int vgui_editor_node_graph_enabled(vgui_editor_node_graph* graph)
	{
		if (!graph)
			return 0;
		return graph->impl.enabled() ? 1 : 0;
	}

	void vgui_editor_node_graph_set_drag_threshold(vgui_editor_node_graph* graph,
		float pixels)
	{
		if (!graph)
			return;
		graph->impl.set_drag_threshold(pixels);
	}

	float vgui_editor_node_graph_drag_threshold(vgui_editor_node_graph* graph)
	{
		return graph ? graph->impl.drag_threshold() : 0.0f;
	}

	vgui_editor_object_id vgui_editor_node_graph_add_node(
		vgui_editor_node_graph* graph, const vgui_editor_node_node* node)
	{
		if (!graph || !node)
			return VGUI_EDITOR_INVALID_OBJECT;
		return (vgui_editor_object_id)graph->impl.add_node(to_node(node));
	}

	int vgui_editor_node_graph_update_node(vgui_editor_node_graph* graph,
		const vgui_editor_node_node* node)
	{
		if (!graph || !node)
			return 0;
		return graph->impl.update_node(to_node(node)) ? 1 : 0;
	}

	int vgui_editor_node_graph_remove_node(vgui_editor_node_graph* graph,
		vgui_editor_object_id id)
	{
		if (!graph)
			return 0;
		return graph->impl.remove_node((object_id)id) ? 1 : 0;
	}

	void vgui_editor_node_graph_clear_nodes(vgui_editor_node_graph* graph)
	{
		if (!graph)
			return;
		graph->impl.clear_nodes();
	}

	size_t vgui_editor_node_graph_node_count(vgui_editor_node_graph* graph)
	{
		return graph ? graph->impl.node_count() : 0;
	}

	int vgui_editor_node_graph_get_node(vgui_editor_node_graph* graph,
		size_t index, vgui_editor_node_node* out)
	{
		if (!graph || !out)
			return 0;
		const std::vector<node_node>& nodes = graph->impl.nodes();
		if (index >= nodes.size())
			return 0;
		capture_node(graph, nodes[index], out);
		return 1;
	}

	int vgui_editor_node_graph_find_node(vgui_editor_node_graph* graph,
		vgui_editor_object_id id, vgui_editor_node_node* out)
	{
		if (!graph || !out)
			return 0;
		const node_node* node = graph->impl.find_node((object_id)id);
		if (!node)
			return 0;
		capture_node(graph, *node, out);
		return 1;
	}

	vgui_editor_object_id vgui_editor_node_graph_add_port(
		vgui_editor_node_graph* graph, vgui_editor_object_id node,
		const vgui_editor_node_port* port)
	{
		if (!graph || !port)
			return VGUI_EDITOR_INVALID_OBJECT;
		const vgui_editor_node_port d = vgeditor::take_desc(port);
		const node_port record = to_port(d);
		return (vgui_editor_object_id)graph->impl.add_port((object_id)node,
			record, record.direction);
	}

	int vgui_editor_node_graph_update_port(vgui_editor_node_graph* graph,
		const vgui_editor_node_port* port)
	{
		if (!graph || !port)
			return 0;
		const vgui_editor_node_port d = vgeditor::take_desc(port);
		return graph->impl.update_port((object_id)d.id, to_port(d)) ? 1 : 0;
	}

	int vgui_editor_node_graph_remove_port(vgui_editor_node_graph* graph,
		vgui_editor_object_id port)
	{
		if (!graph)
			return 0;
		return graph->impl.remove_port((object_id)port) ? 1 : 0;
	}

	int vgui_editor_node_graph_find_port(vgui_editor_node_graph* graph,
		vgui_editor_object_id port, vgui_editor_node_port* out,
		vgui_editor_object_id* out_node,
		vgui_editor_node_port_direction* out_direction)
	{
		if (!graph)
			return 0;
		object_id owner = VGUI_EDITOR_INVALID_OBJECT;
		node_port_direction direction = node_port_direction::input;
		const node_port* record = graph->impl.find_port((object_id)port,
			&owner, &direction);
		if (!record)
			return 0;
		if (out)
			capture_port(graph, *record, out);
		if (out_node)
			*out_node = (vgui_editor_object_id)owner;
		if (out_direction)
			*out_direction = from_direction(direction);
		return 1;
	}

	vgui_editor_object_id vgui_editor_node_graph_add_connection(
		vgui_editor_node_graph* graph,
		const vgui_editor_node_connection* connection)
	{
		if (!graph || !connection)
			return VGUI_EDITOR_INVALID_OBJECT;
		return (vgui_editor_object_id)graph->impl.add_connection(
			to_connection(connection));
	}

	int vgui_editor_node_graph_update_connection(vgui_editor_node_graph* graph,
		const vgui_editor_node_connection* connection)
	{
		if (!graph || !connection)
			return 0;
		return graph->impl.update_connection(to_connection(connection)) ? 1 : 0;
	}

	int vgui_editor_node_graph_remove_connection(vgui_editor_node_graph* graph,
		vgui_editor_object_id id)
	{
		if (!graph)
			return 0;
		return graph->impl.remove_connection((object_id)id) ? 1 : 0;
	}

	void vgui_editor_node_graph_clear_connections(vgui_editor_node_graph* graph)
	{
		if (!graph)
			return;
		graph->impl.clear_connections();
	}

	size_t vgui_editor_node_graph_connection_count(vgui_editor_node_graph* graph)
	{
		return graph ? graph->impl.connection_count() : 0;
	}

	int vgui_editor_node_graph_get_connection(vgui_editor_node_graph* graph,
		size_t index, vgui_editor_node_connection* out)
	{
		if (!graph || !out)
			return 0;
		const std::vector<node_connection>& connections = graph->impl.connections();
		if (index >= connections.size())
			return 0;
		capture_connection(connections[index], out);
		return 1;
	}

	int vgui_editor_node_graph_find_connection(vgui_editor_node_graph* graph,
		vgui_editor_object_id id, vgui_editor_node_connection* out)
	{
		if (!graph || !out)
			return 0;
		const node_connection* connection = graph->impl.find_connection(
			(object_id)id);
		if (!connection)
			return 0;
		capture_connection(*connection, out);
		return 1;
	}

	void vgui_editor_node_graph_set_selection(vgui_editor_node_graph* graph,
		const vgui_editor_object_id* ids, size_t id_count,
		vgui_editor_object_id primary)
	{
		if (!graph)
			return;
		std::vector<object_id> list;
		if (ids) {
			list.reserve(id_count);
			for (size_t i = 0; i < id_count; ++i)
				list.push_back((object_id)ids[i]);
		}
		graph->impl.set_selection(list, (object_id)primary);
	}

	void vgui_editor_node_graph_clear_selection(vgui_editor_node_graph* graph)
	{
		if (!graph)
			return;
		graph->impl.clear_selection();
	}

	void vgui_editor_node_graph_select_all(vgui_editor_node_graph* graph)
	{
		if (!graph)
			return;
		graph->impl.select_all();
	}

	size_t vgui_editor_node_graph_selection_count(vgui_editor_node_graph* graph)
	{
		return graph ? graph->impl.selection_ids().size() : 0;
	}

	vgui_editor_object_id vgui_editor_node_graph_selection_id_at(
		vgui_editor_node_graph* graph, size_t index)
	{
		if (!graph)
			return VGUI_EDITOR_INVALID_OBJECT;
		const std::vector<object_id>& ids = graph->impl.selection_ids();
		if (index >= ids.size())
			return VGUI_EDITOR_INVALID_OBJECT;
		return (vgui_editor_object_id)ids[index];
	}

	vgui_editor_object_id vgui_editor_node_graph_primary_selection(
		vgui_editor_node_graph* graph)
	{
		if (!graph)
			return VGUI_EDITOR_INVALID_OBJECT;
		return (vgui_editor_object_id)graph->impl.primary_selection();
	}

	int vgui_editor_node_graph_is_selected(vgui_editor_node_graph* graph,
		vgui_editor_object_id id)
	{
		if (!graph)
			return 0;
		return graph->impl.is_selected((object_id)id) ? 1 : 0;
	}

	void vgui_editor_node_graph_set_multi_select(vgui_editor_node_graph* graph,
		int enabled)
	{
		if (!graph)
			return;
		graph->impl.set_multi_select(enabled != 0);
	}

	int vgui_editor_node_graph_multi_select(vgui_editor_node_graph* graph)
	{
		if (!graph)
			return 0;
		return graph->impl.multi_select() ? 1 : 0;
	}

	void vgui_editor_node_graph_set_on_selection(vgui_editor_node_graph* graph,
		vgui_editor_node_selection_fn fn, void* user)
	{
		if (!graph)
			return;
		graph->on_selection.set(fn, user, nullptr);
		if (!fn) {
			graph->impl.set_selection_listener(nullptr);
			return;
		}
		const vgeditor::user_owner_ptr owner = graph->on_selection.owner;
		graph->impl.set_selection_listener([fn, owner](
			const std::vector<object_id>& ids) {
			std::vector<vgui_editor_object_id> copy(ids.begin(), ids.end());
			fn(owner->user, copy.data(), copy.size());
		});
	}

	void vgui_editor_node_graph_set_on_change(vgui_editor_node_graph* graph,
		vgui_editor_node_change_fn fn, void* user)
	{
		if (!graph)
			return;
		graph->on_change.set(fn, user, nullptr);
		if (!fn) {
			graph->impl.set_change_listener(nullptr);
			return;
		}
		const vgeditor::user_owner_ptr owner = graph->on_change.owner;
		graph->impl.set_change_listener([fn, owner]() {
			fn(owner->user);
		});
	}

	void vgui_editor_node_graph_get_view_state(vgui_editor_node_graph* graph,
		vgui_editor_node_view_state* out)
	{
		if (!graph || !out)
			return;
		const node_graph_view_state state = graph->impl.view_state();
		std::memset(out, 0, sizeof(*out));
		out->struct_size = (uint32_t)sizeof(*out);
		out->offset_x = state.offset_x;
		out->offset_y = state.offset_y;
		out->zoom = state.zoom;
	}

	void vgui_editor_node_graph_set_view_state(vgui_editor_node_graph* graph,
		const vgui_editor_node_view_state* state)
	{
		if (!graph || !state)
			return;
		const vgui_editor_node_view_state d = vgeditor::take_desc(state);
		node_graph_view_state value;
		value.offset_x = d.offset_x;
		value.offset_y = d.offset_y;
		value.zoom = d.zoom;
		graph->impl.set_view_state(value);
	}

	double vgui_editor_node_graph_zoom(vgui_editor_node_graph* graph)
	{
		return graph ? graph->impl.zoom() : 0.0;
	}

	void vgui_editor_node_graph_set_zoom_limits(vgui_editor_node_graph* graph,
		double minimum, double maximum)
	{
		if (!graph)
			return;
		graph->impl.set_zoom_limits(minimum, maximum);
	}

	void vgui_editor_node_graph_zoom_at(vgui_editor_node_graph* graph,
		float content_x, float content_y, double factor)
	{
		if (!graph)
			return;
		graph->impl.zoom_at(content_x, content_y, factor);
	}

	void vgui_editor_node_graph_pan_by_pixels(vgui_editor_node_graph* graph,
		float delta_x, float delta_y)
	{
		if (!graph)
			return;
		graph->impl.pan_by_pixels(delta_x, delta_y);
	}

	void vgui_editor_node_graph_reset_view(vgui_editor_node_graph* graph)
	{
		if (!graph)
			return;
		graph->impl.reset_view();
	}

	void vgui_editor_node_graph_frame_selection(vgui_editor_node_graph* graph)
	{
		if (!graph)
			return;
		graph->impl.frame_selection();
	}

	void vgui_editor_node_graph_fit_all(vgui_editor_node_graph* graph)
	{
		if (!graph)
			return;
		graph->impl.fit_all();
	}

	void vgui_editor_node_graph_set_grid_visible(vgui_editor_node_graph* graph,
		int visible)
	{
		if (!graph)
			return;
		graph->impl.set_grid_visible(visible != 0);
	}

	int vgui_editor_node_graph_grid_visible(vgui_editor_node_graph* graph)
	{
		if (!graph)
			return 0;
		return graph->impl.grid_visible() ? 1 : 0;
	}

	void vgui_editor_node_graph_set_grid_step(vgui_editor_node_graph* graph,
		double step)
	{
		if (!graph)
			return;
		graph->impl.set_grid_step(step);
	}

	double vgui_editor_node_graph_grid_step(vgui_editor_node_graph* graph)
	{
		return graph ? graph->impl.grid_step() : 0.0;
	}

	double vgui_editor_node_graph_effective_grid_step(
		vgui_editor_node_graph* graph)
	{
		return graph ? graph->impl.effective_grid_step() : 0.0;
	}

	void vgui_editor_node_graph_set_snap_to_grid(vgui_editor_node_graph* graph,
		int enabled)
	{
		if (!graph)
			return;
		graph->impl.set_snap_to_grid(enabled != 0);
	}

	int vgui_editor_node_graph_snap_to_grid(vgui_editor_node_graph* graph)
	{
		if (!graph)
			return 0;
		return graph->impl.snap_to_grid() ? 1 : 0;
	}

	vgui_editor_node_point vgui_editor_node_graph_snap_point(
		vgui_editor_node_graph* graph, vgui_editor_node_point point)
	{
		if (!graph)
			return to_c_point(node_graph_point{});
		return to_c_point(graph->impl.snap_point(
			node_graph_point{ point.x, point.y }));
	}

	void vgui_editor_node_graph_set_minimap_visible(vgui_editor_node_graph* graph,
		int visible)
	{
		if (!graph)
			return;
		graph->impl.set_minimap_visible(visible != 0);
	}

	int vgui_editor_node_graph_minimap_visible(vgui_editor_node_graph* graph)
	{
		if (!graph)
			return 0;
		return graph->impl.minimap_visible() ? 1 : 0;
	}

	int vgui_editor_node_graph_minimap_rect(vgui_editor_node_graph* graph,
		float* x, float* y, float* width, float* height)
	{
		if (!graph)
			return 0;
		float left = 0.0f;
		float top = 0.0f;
		float wide = 0.0f;
		float high = 0.0f;
		if (!graph->impl.minimap_rect(&left, &top, &wide, &high))
			return 0;
		if (x)
			*x = left;
		if (y)
			*y = top;
		if (width)
			*width = wide;
		if (height)
			*height = high;
		return 1;
	}

	vgui_editor_node_point vgui_editor_node_graph_screen_to_graph(
		vgui_editor_node_graph* graph, float content_x, float content_y)
	{
		if (!graph)
			return to_c_point(node_graph_point{});
		return to_c_point(graph->impl.screen_to_graph(content_x, content_y));
	}

	vgui_editor_node_point vgui_editor_node_graph_graph_to_screen(
		vgui_editor_node_graph* graph, double graph_x, double graph_y)
	{
		if (!graph)
			return to_c_point(node_graph_point{});
		return to_c_point(graph->impl.graph_to_screen(graph_x, graph_y));
	}

	vgui_editor_node_rect vgui_editor_node_graph_node_bounds(
		vgui_editor_node_graph* graph, vgui_editor_object_id node)
	{
		if (!graph)
			return to_c_rect(node_graph_rect{});
		return to_c_rect(graph->impl.node_rect((object_id)node));
	}

	vgui_editor_node_rect vgui_editor_node_graph_port_bounds(
		vgui_editor_node_graph* graph, vgui_editor_object_id port)
	{
		if (!graph)
			return to_c_rect(node_graph_rect{});
		return to_c_rect(graph->impl.port_rect((object_id)port));
	}

	int vgui_editor_node_graph_port_position(vgui_editor_node_graph* graph,
		vgui_editor_object_id port, vgui_editor_node_point* out)
	{
		if (!graph || !out)
			return 0;
		node_graph_point point;
		if (!graph->impl.port_position((object_id)port, &point))
			return 0;
		*out = to_c_point(point);
		return 1;
	}

	vgui_editor_node_rect vgui_editor_node_graph_selection_bounds(
		vgui_editor_node_graph* graph)
	{
		if (!graph)
			return to_c_rect(node_graph_rect{});
		return to_c_rect(graph->impl.selection_bounds());
	}

	vgui_editor_node_rect vgui_editor_node_graph_content_bounds(
		vgui_editor_node_graph* graph)
	{
		if (!graph)
			return to_c_rect(node_graph_rect{});
		return to_c_rect(graph->impl.content_bounds());
	}

	double vgui_editor_node_graph_connection_tolerance(
		vgui_editor_node_graph* graph)
	{
		return graph ? graph->impl.connection_hit_tolerance() : 0.0;
	}

	vgui_editor_object_id vgui_editor_node_graph_node_hit(
		vgui_editor_node_graph* graph, float content_x, float content_y)
	{
		if (!graph)
			return VGUI_EDITOR_INVALID_OBJECT;
		return (vgui_editor_object_id)graph->impl.node_at(content_x, content_y);
	}

	vgui_editor_object_id vgui_editor_node_graph_port_hit(
		vgui_editor_node_graph* graph, float content_x, float content_y)
	{
		if (!graph)
			return VGUI_EDITOR_INVALID_OBJECT;
		return (vgui_editor_object_id)graph->impl.port_at(content_x, content_y);
	}

	vgui_editor_object_id vgui_editor_node_graph_connection_hit(
		vgui_editor_node_graph* graph, float content_x, float content_y)
	{
		if (!graph)
			return VGUI_EDITOR_INVALID_OBJECT;
		return (vgui_editor_object_id)graph->impl.connection_at(content_x,
			content_y);
	}

	vgui_editor_object_id vgui_editor_node_graph_hit_test(
		vgui_editor_node_graph* graph, float content_x, float content_y)
	{
		if (!graph)
			return VGUI_EDITOR_INVALID_OBJECT;
		return (vgui_editor_object_id)graph->impl.hit_test(content_x, content_y);
	}

	int vgui_editor_node_graph_dragging(vgui_editor_node_graph* graph)
	{
		return graph && graph->impl.dragging() ? 1 : 0;
	}

	vgui_editor_node_graph_gesture vgui_editor_node_graph_get_gesture(
		vgui_editor_node_graph* graph)
	{
		if (!graph)
			return VGUI_EDITOR_NODE_GESTURE_NONE;
		return from_gesture(graph->impl.gesture());
	}

	vgui_editor_object_id vgui_editor_node_graph_pending_port(
		vgui_editor_node_graph* graph)
	{
		if (!graph)
			return VGUI_EDITOR_INVALID_OBJECT;
		return (vgui_editor_object_id)graph->impl.pending_connection_port();
	}

	void vgui_editor_node_graph_cancel_gesture(vgui_editor_node_graph* graph)
	{
		if (!graph)
			return;
		graph->impl.cancel_gesture();
	}

	int vgui_editor_node_graph_connect_ports(vgui_editor_node_graph* graph,
		vgui_editor_object_id port_a, vgui_editor_object_id port_b)
	{
		if (!graph)
			return 0;
		return graph->impl.connect_ports((object_id)port_a, (object_id)port_b)
			? 1
			: 0;
	}

	int vgui_editor_node_graph_disconnect_selection(vgui_editor_node_graph* graph)
	{
		if (!graph)
			return 0;
		return graph->impl.disconnect_selection() ? 1 : 0;
	}

	int vgui_editor_node_graph_delete_selection(vgui_editor_node_graph* graph)
	{
		if (!graph)
			return 0;
		return graph->impl.delete_selection() ? 1 : 0;
	}

	int vgui_editor_node_graph_duplicate_selection(vgui_editor_node_graph* graph)
	{
		if (!graph)
			return 0;
		return graph->impl.duplicate_selection() ? 1 : 0;
	}

	void vgui_editor_node_graph_set_create_node(vgui_editor_node_graph* graph,
		vgui_editor_node_create_fn fn, void* user,
		vgui_editor_release_fn release_user)
	{
		if (!graph)
			return;
		graph->create_node.set(fn, user, release_user);
		if (!fn) {
			graph->impl.set_create_node_handler(nullptr);
			return;
		}
		install_create_node(graph, fn, graph->create_node.owner);
	}

	int vgui_editor_node_graph_perform_action(vgui_editor_node_graph* graph,
		vgui_editor_node_graph_action action)
	{
		if (!graph)
			return 0;
		return graph->impl.perform_action(to_action(action)) ? 1 : 0;
	}

	vgui_widget* vgui_editor_node_graph_menu(vgui_editor_node_graph* graph)
	{
		return graph ? graph->impl.menu() : nullptr;
	}

	void vgui_editor_node_graph_open_context_menu(vgui_editor_node_graph* graph,
		float content_x, float content_y)
	{
		if (!graph)
			return;
		graph->impl.open_context_menu(content_x, content_y);
	}

	void vgui_editor_node_graph_set_search(vgui_editor_node_graph* graph,
		const char* text)
	{
		if (!graph)
			return;
		graph->impl.set_search_text(vgeditor::to_text(text));
	}

	size_t vgui_editor_node_graph_search(vgui_editor_node_graph* graph,
		char* out, size_t cap)
	{
		if (!graph)
			return 0;
		return vgeditor::copy_string(out, cap, graph->impl.search_text().c_str());
	}

	void vgui_editor_node_graph_clear_search(vgui_editor_node_graph* graph)
	{
		if (!graph)
			return;
		graph->impl.clear_search();
	}

	void vgui_editor_node_graph_focus_search(vgui_editor_node_graph* graph)
	{
		if (!graph)
			return;
		graph->impl.focus_search();
	}

	vgui_widget* vgui_editor_node_graph_search_widget(vgui_editor_node_graph* graph)
	{
		return graph ? graph->impl.search_widget() : nullptr;
	}

	size_t vgui_editor_node_graph_match_count(vgui_editor_node_graph* graph)
	{
		return graph ? graph->impl.search_match_count() : 0;
	}

	vgui_editor_object_id vgui_editor_node_graph_match_at(
		vgui_editor_node_graph* graph, size_t index)
	{
		if (!graph)
			return VGUI_EDITOR_INVALID_OBJECT;
		const std::vector<object_id>& matches = graph->impl.search_matches();
		if (index >= matches.size())
			return VGUI_EDITOR_INVALID_OBJECT;
		return (vgui_editor_object_id)matches[index];
	}

	int vgui_editor_node_graph_node_matches(vgui_editor_node_graph* graph,
		vgui_editor_object_id node)
	{
		if (!graph)
			return 0;
		return graph->impl.node_matches_search((object_id)node) ? 1 : 0;
	}

	vgui_editor_object_id vgui_editor_node_graph_next_match(
		vgui_editor_node_graph* graph)
	{
		if (!graph)
			return VGUI_EDITOR_INVALID_OBJECT;
		return (vgui_editor_object_id)graph->impl.next_search_match();
	}

	void vgui_editor_node_graph_set_command_stack(vgui_editor_node_graph* graph,
		vgui_editor_command_stack* stack)
	{
		if (!graph)
			return;
		graph->impl.set_command_stack(stack ? vgeditor::stack_impl(stack)
			: nullptr);
	}

	size_t vgui_editor_node_graph_capture_state_json(
		vgui_editor_node_graph* graph, char* out, size_t cap)
	{
		if (!graph)
			return 0;
		const std::string text = node_graph_state_to_json(graph->impl.capture_state());
		return vgeditor::copy_string(out, cap, text.c_str());
	}

	int vgui_editor_node_graph_apply_state_json(vgui_editor_node_graph* graph,
		const char* json)
	{
		if (!graph)
			return 0;
		node_graph_state state;
		std::string error;
		if (!node_graph_state_from_json(vgeditor::to_text(json), state, &error)) {
			vgeditor::set_error(error.empty()
				? "invalid node graph state json"
				: error);
			return 0;
		}
		if (!graph->impl.apply_state(state, &error)) {
			vgeditor::set_error(error.empty()
				? "node graph state rejected"
				: error);
			return 0;
		}
		vgeditor::set_error(std::string());
		return 1;
	}

} /* extern "C" */

namespace vgeditor {

	vgui_editor_node_graph* borrow_node_graph(vek::node_graph_view& graph)
	{
		return borrow_object<vgui_editor_node_graph>(graph);
	}

	void release_borrowed_node_graph(vgui_editor_node_graph* handle)
	{
		/* Wrapper only: a borrowed handle's C++ object belongs to its container. */
		if (handle && !handle->owns_object())
			delete handle;
	}


} /* namespace vgeditor */
