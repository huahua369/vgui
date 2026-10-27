#include "node_graph_view.h"

#include "tools/tools.hpp"
#include "ui_scale.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <stdexcept>
#include <utility>

#include <nlohmann/json.hpp>

namespace vgui_editor_kit {

	namespace {

		constexpr float kBorderInset = 1.0f;
		constexpr double kMinNodeWidth = 120.0;
		constexpr double kMaxNodeWidth = 600.0;
		constexpr double kDefaultNodeWidth = 180.0;
		constexpr double kNodeHeaderHeight = 24.0;
		constexpr double kNodeRowHeight = 20.0;
		constexpr double kNodeBodyPadding = 6.0;
		constexpr double kPortInset = 9.0;
		constexpr double kPortRadius = 4.0;
		constexpr double kBezierOffset = 40.0;
		constexpr double kMinimapMargin = 10.0f;
		constexpr double kMinimapInset = 5.0;
		constexpr double kSelectionMargin = 42.0f;
		constexpr double kDuplicateOffset = 24.0;
		constexpr double kEpsilon = 1e-9;
		constexpr int kMaxGridLines = 4096;

		/* vgui 0xAARRGGBB -> OVG 0xAABBGGRR. */
		uint32_t to_ovg(vgui_color color)
		{
			return (color & 0xFF00FF00u) |
				((color & 0x000000FFu) << 16) |
				((color >> 16) & 0x000000FFu);
		}

		vgui_color with_alpha(vgui_color color, uint32_t alpha)
		{
			return (color & 0x00FFFFFFu) | ((alpha & 0xFFu) << 24);
		}

		vgui_color theme_color(vgui_color color, vgui_color fallback)
		{
			return color ? color : fallback;
		}

		bool finite(double value)
		{
			return std::isfinite(value);
		}

		double clamp_width(double width)
		{
			if (!finite(width))
				return kDefaultNodeWidth;
			return std::clamp(width, kMinNodeWidth, kMaxNodeWidth);
		}

		node_graph_point make_point(double x, double y)
		{
			node_graph_point point;
			point.x = finite(x) ? x : 0.0;
			point.y = finite(y) ? y : 0.0;
			return point;
		}

		double distance_to_segment(double px, double py, double x0, double y0,
			double x1, double y1)
		{
			const double dx = x1 - x0;
			const double dy = y1 - y0;
			const double length_squared = dx * dx + dy * dy;
			if (length_squared <= kEpsilon)
				return std::hypot(px - x0, py - y0);
			double t = ((px - x0) * dx + (py - y0) * dy) /
				length_squared;
			t = std::clamp(t, 0.0, 1.0);
			return std::hypot(px - (x0 + dx * t), py - (y0 + dy * t));
		}

		/* Liang-Barsky segment/rectangle overlap test. */
		bool segment_intersects_rect(double x0, double y0, double x1,
			double y1, const node_graph_rect& rect)
		{
			if (rect.empty())
				return false;
			if (rect.contains(x0, y0) || rect.contains(x1, y1))
				return true;
			double t0 = 0.0;
			double t1 = 1.0;
			const double dx = x1 - x0;
			const double dy = y1 - y0;
			const double p[4] = { -dx, dx, -dy, dy };
			const double q[4] = { x0 - rect.x, rect.right() - x0,
				y0 - rect.y, rect.bottom() - y0 };
			for (int i = 0; i < 4; ++i)
			{
				if (std::fabs(p[i]) <= kEpsilon)
				{
					if (q[i] < 0.0)
						return false;
					continue;
				}
				const double r = q[i] / p[i];
				if (p[i] < 0.0)
				{
					if (r > t1)
						return false;
					if (r > t0)
						t0 = r;
				}
				else
				{
					if (r < t0)
						return false;
					if (r < t1)
						t1 = r;
				}
			}
			return t0 <= t1;
		}

		void bezier_controls(node_graph_point from, node_graph_point to,
			node_graph_point* c1, node_graph_point* c2)
		{
			double offset = std::max(kBezierOffset,
				std::fabs(to.x - from.x) * 0.5);
			if (c1)
				*c1 = { from.x + offset, from.y };
			if (c2)
				*c2 = { to.x - offset, to.y };
		}

		node_graph_point cubic_at(node_graph_point p0, node_graph_point p1,
			node_graph_point p2, node_graph_point p3, double t)
		{
			const double u = 1.0 - t;
			const double a = u * u * u;
			const double b = 3.0 * u * u * t;
			const double c = 3.0 * u * t * t;
			const double d = t * t * t;
			return { a * p0.x + b * p1.x + c * p2.x + d * p3.x,
				a * p0.y + b * p1.y + c * p2.y + d * p3.y };
		}

		vgui_color port_type_color(node_port_type type)
		{
			switch (type)
			{
			case node_port_type::number:
				return 0xFF8FD16Au;
			case node_port_type::string:
				return 0xFFE3B341u;
			case node_port_type::boolean:
				return 0xFFE06C75u;
			case node_port_type::color:
				return 0xFFC678DDu;
			case node_port_type::vector:
				return 0xFF56B6C2u;
			case node_port_type::object:
				return 0xFFB0BEC5u;
			case node_port_type::custom:
				return 0xFF9AA4B2u;
			case node_port_type::flow:
			default:
				return 0xFF63C7FFu;
			}
		}

		std::string lower_text(const std::string& text)
		{
			std::string result = text;
			for (char& ch : result)
			{
				const unsigned char value = (unsigned char)ch;
				ch = (char)std::tolower(value);
			}
			return result;
		}

		bool validate_node_graph_state(const node_graph_state& state,
			std::string* error)
		{
			auto fail = [error](const char* message) {
				if (error)
					*error = message;
				return false;
			};
			if (state.version < 1 ||
				state.version > node_graph_state::current_version)
				return fail("unsupported node graph state version");
			if (!finite(state.view.offset_x) ||
				!finite(state.view.offset_y))
				return fail("node graph view offset must be finite");
			if (!finite(state.view.zoom) || state.view.zoom <= 0.0)
				return fail("node graph zoom must be positive");
			if (!finite(state.grid_step) || state.grid_step <= 0.0)
				return fail("node graph grid step must be positive");
			for (object_id id : state.selection)
			{
				if (id == invalid_object)
					return fail("node graph selection contains an invalid id");
			}
			return true;
		}

	} /* namespace */

	/* --------------------------------------------------------------------- */
	/* State serialization                                                   */
	/* --------------------------------------------------------------------- */

	std::string node_graph_state_to_json(const node_graph_state& state)
	{
		nlohmann::json root;
		root["version"] = node_graph_state::current_version;
		root["view"] = {
			{ "x", state.view.offset_x },
			{ "y", state.view.offset_y },
			{ "zoom", state.view.zoom }
		};
		root["grid"] = {
			{ "visible", state.grid_visible },
			{ "step", state.grid_step }
		};
		root["minimap"] = {
			{ "visible", state.minimap_visible }
		};
		root["snap"] = {
			{ "enabled", state.snap_to_grid }
		};
		root["primary"] = state.primary;
		root["selection"] = nlohmann::json::array();
		for (object_id id : state.selection)
			root["selection"].push_back(id);
		return root.dump();
	}

	bool node_graph_state_from_json(const std::string& text,
		node_graph_state& out, std::string* error)
	{
		out = node_graph_state{};
		try
		{
			nlohmann::json root = nlohmann::json::parse(text);
			if (!root.is_object())
				throw std::runtime_error(
					"node graph state root must be an object");

			auto version = root.find("version");
			if (version == root.end() || !version->is_number_integer())
				throw std::runtime_error(
					"node graph state version is missing");
			const int file_version = version->get<int>();
			if (file_version < 1 ||
				file_version > node_graph_state::current_version)
				throw std::runtime_error(
					"unsupported node graph state version");

			node_graph_state parsed;
			parsed.version = file_version;

			auto number = [](const nlohmann::json& object, const char* key,
				double fallback) {
				auto it = object.find(key);
				if (it == object.end())
					return fallback;
				if (!it->is_number())
					throw std::runtime_error(std::string(
						"expected number: ") + key);
				return it->get<double>();
			};
			auto boolean = [](const nlohmann::json& object, const char* key,
				bool fallback) {
				auto it = object.find(key);
				if (it == object.end())
					return fallback;
				if (!it->is_boolean())
					throw std::runtime_error(std::string(
						"expected boolean: ") + key);
				return it->get<bool>();
			};
			auto id_value = [](const nlohmann::json& value, const char* key) {
				if (!value.is_number_integer() &&
					!value.is_number_unsigned())
					throw std::runtime_error(std::string(
						"expected integer: ") + key);
				const long long parsed = value.get<long long>();
				if (parsed < 0)
					throw std::runtime_error(std::string(
						"negative id: ") + key);
				return (object_id)parsed;
			};

			auto view = root.find("view");
			if (view != root.end())
			{
				if (!view->is_object())
					throw std::runtime_error(
						"node graph view must be an object");
				parsed.view.offset_x = number(*view, "x",
					parsed.view.offset_x);
				parsed.view.offset_y = number(*view, "y",
					parsed.view.offset_y);
				parsed.view.zoom = number(*view, "zoom",
					parsed.view.zoom);
			}

			auto grid = root.find("grid");
			if (grid != root.end())
			{
				if (!grid->is_object())
					throw std::runtime_error(
						"node graph grid must be an object");
				parsed.grid_visible = boolean(*grid, "visible",
					parsed.grid_visible);
				parsed.grid_step = number(*grid, "step",
					parsed.grid_step);
			}

			auto minimap = root.find("minimap");
			if (minimap != root.end())
			{
				if (!minimap->is_object())
					throw std::runtime_error(
						"node graph minimap must be an object");
				parsed.minimap_visible = boolean(*minimap, "visible",
					parsed.minimap_visible);
			}

			auto snap = root.find("snap");
			if (snap != root.end())
			{
				if (!snap->is_object())
					throw std::runtime_error(
						"node graph snap must be an object");
				parsed.snap_to_grid = boolean(*snap, "enabled",
					parsed.snap_to_grid);
			}

			auto primary = root.find("primary");
			if (primary != root.end())
				parsed.primary = id_value(*primary, "primary");

			auto selection = root.find("selection");
			if (selection != root.end())
			{
				if (!selection->is_array())
					throw std::runtime_error(
						"node graph selection must be an array");
				for (const auto& entry : *selection)
					parsed.selection.push_back(
						id_value(entry, "selection"));
			}

			std::string reason;
			if (!validate_node_graph_state(parsed, &reason))
			{
				if (error)
					*error = reason;
				out = node_graph_state{};
				return false;
			}
			out = parsed;
			return true;
		}
		catch (const std::exception& e)
		{
			out = node_graph_state{};
			if (error)
				*error = e.what();
			return false;
		}
	}

	/* --------------------------------------------------------------------- */
	/* Construction / model                                                  */
	/* --------------------------------------------------------------------- */

	node_graph_view::~node_graph_view()
	{
		if (context_ && focus_token_)
			vgui_context_remove_focus_listener(context_, focus_token_);
		focus_token_ = 0;
		if (search_edit_)
		{
			vgui_widget_on_text(search_edit_, nullptr, nullptr);
			vgui_widget_on_event(search_edit_, nullptr, nullptr);
			search_edit_ = nullptr;
		}
		if (menu_)
		{
			vgui_menu_on_activate(menu_, nullptr, nullptr);
			menu_ = nullptr;
		}
		if (widget_)
		{
			vgui_custom_set_draw(widget_, nullptr, nullptr);
			vgui_custom_set_event(widget_, nullptr, nullptr);
		}
		widget_ = nullptr;
		context_ = nullptr;
	}

	void node_graph_view::build(vgui_widget* parent)
	{
		if (widget_ || !parent)
			return;

		context_ = vgui_widget_get_context(parent);
		widget_ = vgui_widget_add(parent, VGUI_CUSTOM, nullptr);
		vgui_widget_set_id(widget_, "node_graph_view");
		vgui_widget_set_a11y_name(widget_, "Node graph");
		vgui_widget_set_grow(widget_, 1.0f);
		vgui_widget_set_min_size(widget_, 240, 120);
		vgui_widget_set_background(widget_, 0xFF101419);
		vgui_widget_set_border(widget_, 0xFF2A3038, 1);
		vgui_widget_set_focusable(widget_, 1);
		vgui_custom_set_draw(widget_, &node_graph_view::on_draw, this);
		vgui_custom_set_event(widget_, &node_graph_view::on_event, this);
		if (context_)
			focus_token_ = vgui_context_add_focus_listener(context_,
				&node_graph_view::on_focus_change, this);

		search_edit_ = vgui_widget_add(widget_, VGUI_EDIT, nullptr);
		vgui_widget_set_id(search_edit_, "node_graph_search");
		vgui_widget_set_pos(search_edit_, 9, 9);
		vgui_widget_set_size(search_edit_, 180, 24);
		vgui_widget_set_font_size(search_edit_, 12);
		vgui_edit_set_placeholder(search_edit_, "Search nodes");
		vgui_widget_on_text(search_edit_, &node_graph_view::on_search_text,
			this);
		vgui_widget_on_event(search_edit_, &node_graph_view::on_search_event,
			this);

		if (context_)
		{
			menu_ = vgui_widget_create(context_, VGUI_MENU, nullptr);
			if (menu_)
				vgui_menu_on_activate(menu_, &node_graph_view::on_menu_activate,
					this);
		}
	}

	vgui_widget* node_graph_view::widget() const noexcept
	{
		return widget_;
	}

	bool node_graph_view::built() const noexcept
	{
		return widget_ != nullptr;
	}

	bool node_graph_view::id_used(object_id id) const noexcept
	{
		if (id == invalid_object)
			return true;
		for (const node_node& node : nodes_)
		{
			if (node.id == id)
				return true;
			for (const node_port& port : node.inputs)
				if (port.id == id)
					return true;
			for (const node_port& port : node.outputs)
				if (port.id == id)
					return true;
		}
		for (const node_connection& connection : connections_)
			if (connection.id == id)
				return true;
		return false;
	}

	bool node_graph_view::port_id_used_elsewhere(object_id id,
		object_id node_id) const noexcept
	{
		if (id == invalid_object)
			return true;
		for (const node_node& node : nodes_)
		{
			if (node.id == id)
				return true;
			if (node.id == node_id)
				continue;
			for (const node_port& port : node.inputs)
				if (port.id == id)
					return true;
			for (const node_port& port : node.outputs)
				if (port.id == id)
					return true;
		}
		for (const node_connection& connection : connections_)
			if (connection.id == id)
				return true;
		return false;
	}

	object_id node_graph_view::allocate_id()
	{
		while (id_used(next_id_))
			++next_id_;
		return next_id_++;
	}

	object_id node_graph_view::add_node(node_node node)
	{
		if (!finite(node.x))
			node.x = 0.0;
		if (!finite(node.y))
			node.y = 0.0;
		node.width = clamp_width(node.width);

		std::vector<object_id> assigned;
		auto conflict = [&](object_id id) {
			if (id == invalid_object)
				return true;
			if (node.id == id)
				return true;
			if (id_used(id))
				return true;
			return std::find(assigned.begin(), assigned.end(), id) !=
				assigned.end();
		};

		if (node.id == invalid_object)
		{
			while (conflict(next_id_))
				++next_id_;
			node.id = next_id_++;
		}
		else
		{
			if (id_used(node.id))
				return invalid_object;
			if (node.id >= next_id_)
				next_id_ = node.id + 1;
		}

		auto assign_ports = [&](std::vector<node_port>& ports,
			node_port_direction direction) -> bool {
			for (node_port& port : ports)
			{
				port.direction = direction;
				if (port.id == invalid_object)
				{
					while (conflict(next_id_))
						++next_id_;
					port.id = next_id_++;
				}
				else if (conflict(port.id))
				{
					return false;
				}
				if (port.id >= next_id_)
					next_id_ = port.id + 1;
				assigned.push_back(port.id);
			}
			return true;
		};
		if (!assign_ports(node.inputs, node_port_direction::input) ||
			!assign_ports(node.outputs, node_port_direction::output))
			return invalid_object;

		const object_id id = node.id;
		nodes_.push_back(std::move(node));
		notify_change();
		return id;
	}

	bool node_graph_view::update_node(const node_node& node)
	{
		node_node* current = find_node(node.id);
		if (!current || node.id == invalid_object)
			return false;

		node_node next = node;
		if (!finite(next.x))
			next.x = 0.0;
		if (!finite(next.y))
			next.y = 0.0;
		next.width = clamp_width(next.width);

		std::vector<object_id> assigned;
		auto conflict = [&](object_id id) {
			if (id == invalid_object || id == next.id)
				return true;
			for (const node_node& other : nodes_)
			{
				if (other.id == next.id)
					continue;
				if (other.id == id)
					return true;
				for (const node_port& port : other.inputs)
					if (port.id == id)
						return true;
				for (const node_port& port : other.outputs)
					if (port.id == id)
						return true;
			}
			for (const node_connection& connection : connections_)
				if (connection.id == id)
					return true;
			return std::find(assigned.begin(), assigned.end(), id) !=
				assigned.end();
		};

		auto assign_ports = [&](std::vector<node_port>& ports,
			node_port_direction direction) -> bool {
			for (node_port& port : ports)
			{
				port.direction = direction;
				if (port.id == invalid_object)
				{
					while (conflict(next_id_))
						++next_id_;
					port.id = next_id_++;
				}
				else if (conflict(port.id))
				{
					return false;
				}
				if (port.id >= next_id_)
					next_id_ = port.id + 1;
				assigned.push_back(port.id);
			}
			return true;
		};
		if (!assign_ports(next.inputs, node_port_direction::input) ||
			!assign_ports(next.outputs, node_port_direction::output))
			return false;

		*current = std::move(next);
		drop_invalid_connections();
		prune_selection();
		refresh_search_matches();
		notify_change();
		return true;
	}

	bool node_graph_view::remove_node(object_id id)
	{
		auto found = std::find_if(nodes_.begin(), nodes_.end(),
			[&](const node_node& node) { return node.id == id; });
		if (found == nodes_.end())
			return false;
		nodes_.erase(found);
		drop_connections_for_node(id);
		drop_invalid_connections();
		prune_selection();
		refresh_search_matches();
		notify_change();
		return true;
	}

	void node_graph_view::clear_nodes()
	{
		if (nodes_.empty() && connections_.empty())
			return;
		const bool had_selection = !selection_.empty() ||
			primary_ != invalid_object;
		nodes_.clear();
		connections_.clear();
		selection_.clear();
		primary_ = invalid_object;
		refresh_search_matches();
		if (had_selection)
			notify_selection();
		notify_change();
	}

	const std::vector<node_node>& node_graph_view::nodes() const noexcept
	{
		return nodes_;
	}

	const node_node* node_graph_view::find_node(object_id id) const noexcept
	{
		if (id == invalid_object)
			return nullptr;
		for (const node_node& node : nodes_)
			if (node.id == id)
				return &node;
		return nullptr;
	}

	node_node* node_graph_view::find_node(object_id id) noexcept
	{
		if (id == invalid_object)
			return nullptr;
		for (node_node& node : nodes_)
			if (node.id == id)
				return &node;
		return nullptr;
	}

	std::size_t node_graph_view::node_count() const noexcept
	{
		return nodes_.size();
	}

	object_id node_graph_view::add_port(object_id node_id, node_port port,
		node_port_direction direction)
	{
		node_node* node = find_node(node_id);
		if (!node)
			return invalid_object;
		port.direction = direction;
		if (port.id == invalid_object)
		{
			while (id_used(next_id_))
				++next_id_;
			port.id = next_id_++;
		}
		else
		{
			if (id_used(port.id))
				return invalid_object;
			if (port.id >= next_id_)
				next_id_ = port.id + 1;
		}
		const object_id id = port.id;
		if (direction == node_port_direction::input)
			node->inputs.push_back(std::move(port));
		else
			node->outputs.push_back(std::move(port));
		notify_change();
		return id;
	}

	bool node_graph_view::update_port(object_id port_id,
		const node_port& port_record)
	{
		object_id node_id = invalid_object;
		node_port_direction direction = node_port_direction::input;
		node_port* existing = find_port(port_id, &node_id, &direction);
		if (!existing)
			return false;
		node_node* node = find_node(node_id);
		if (!node)
			return false;

		node_port next = port_record;
		next.id = port_id;
		const node_port_direction next_direction = next.direction;

		std::vector<node_port>& source =
			direction == node_port_direction::input ? node->inputs
				: node->outputs;
		auto found = std::find_if(source.begin(), source.end(),
			[&](const node_port& port) { return port.id == port_id; });
		if (found == source.end())
			return false;
		source.erase(found);

		std::vector<node_port>& target =
			next_direction == node_port_direction::input ? node->inputs
				: node->outputs;
		target.push_back(std::move(next));
		drop_invalid_connections();
		prune_selection();
		notify_change();
		return true;
	}

	bool node_graph_view::remove_port(object_id port_id)
	{
		object_id node_id = invalid_object;
		node_port_direction direction = node_port_direction::input;
		if (find_port(port_id, &node_id, &direction) == nullptr)
			return false;
		node_node* node = find_node(node_id);
		if (!node)
			return false;
		std::vector<node_port>& ports =
			direction == node_port_direction::input ? node->inputs
				: node->outputs;
		ports.erase(std::remove_if(ports.begin(), ports.end(),
			[&](const node_port& port) { return port.id == port_id; }),
			ports.end());
		drop_connections_for_port(port_id);
		drop_invalid_connections();
		prune_selection();
		notify_change();
		return true;
	}

	const node_port* node_graph_view::find_port(object_id port_id,
		object_id* node_id, node_port_direction* direction) const noexcept
	{
		if (port_id == invalid_object)
			return nullptr;
		for (const node_node& node : nodes_)
		{
			for (const node_port& port : node.inputs)
			{
				if (port.id != port_id)
					continue;
				if (node_id)
					*node_id = node.id;
				if (direction)
					*direction = node_port_direction::input;
				return &port;
			}
			for (const node_port& port : node.outputs)
			{
				if (port.id != port_id)
					continue;
				if (node_id)
					*node_id = node.id;
				if (direction)
					*direction = node_port_direction::output;
				return &port;
			}
		}
		return nullptr;
	}

	node_port* node_graph_view::find_port(object_id port_id,
		object_id* node_id, node_port_direction* direction) noexcept
	{
		if (port_id == invalid_object)
			return nullptr;
		for (node_node& node : nodes_)
		{
			for (node_port& port : node.inputs)
			{
				if (port.id != port_id)
					continue;
				if (node_id)
					*node_id = node.id;
				if (direction)
					*direction = node_port_direction::input;
				return &port;
			}
			for (node_port& port : node.outputs)
			{
				if (port.id != port_id)
					continue;
				if (node_id)
					*node_id = node.id;
				if (direction)
					*direction = node_port_direction::output;
				return &port;
			}
		}
		return nullptr;
	}

	bool node_graph_view::connection_exists(object_id from_port,
		object_id to_port) const noexcept
	{
		for (const node_connection& connection : connections_)
		{
			if (connection.from_port == from_port &&
				connection.to_port == to_port)
				return true;
			if (connection.from_port == to_port &&
				connection.to_port == from_port)
				return true;
		}
		return false;
	}

	bool node_graph_view::ports_compatible(const node_port& a,
		const node_port& b) const noexcept
	{
		return a.type == b.type || a.type == node_port_type::custom ||
			b.type == node_port_type::custom;
	}

	bool node_graph_view::resolve_connection_ports(object_id port_a,
		object_id port_b, object_id* from_node, object_id* from_port,
		object_id* to_node, object_id* to_port, const node_port** from,
		const node_port** to) const
	{
		object_id node_a = invalid_object;
		object_id node_b = invalid_object;
		node_port_direction direction_a = node_port_direction::input;
		node_port_direction direction_b = node_port_direction::input;
		const node_port* port_a_entry = find_port(port_a, &node_a, &direction_a);
		const node_port* port_b_entry = find_port(port_b, &node_b, &direction_b);
		if (!port_a_entry || !port_b_entry)
			return false;
		if (direction_a == direction_b)
			return false;
		if (!ports_compatible(*port_a_entry, *port_b_entry))
			return false;

		const bool a_is_output = direction_a == node_port_direction::output;
		if (from_node)
			*from_node = a_is_output ? node_a : node_b;
		if (from_port)
			*from_port = a_is_output ? port_a : port_b;
		if (to_node)
			*to_node = a_is_output ? node_b : node_a;
		if (to_port)
			*to_port = a_is_output ? port_b : port_a;
		if (from)
			*from = a_is_output ? port_a_entry : port_b_entry;
		if (to)
			*to = a_is_output ? port_b_entry : port_a_entry;
		return true;
	}

	object_id node_graph_view::add_connection(node_connection connection)
	{
		object_id from_node = invalid_object;
		object_id from_port = invalid_object;
		object_id to_node = invalid_object;
		object_id to_port = invalid_object;
		const node_port* from = nullptr;
		const node_port* to = nullptr;
		if (!resolve_connection_ports(connection.from_port, connection.to_port,
			&from_node, &from_port, &to_node, &to_port, &from, &to))
			return invalid_object;
		if (connection.from_node != invalid_object &&
			connection.from_node != from_node)
			return invalid_object;
		if (connection.to_node != invalid_object &&
			connection.to_node != to_node)
			return invalid_object;
		if (connection_exists(from_port, to_port))
			return invalid_object;

		if (connection.id == invalid_object)
		{
			while (id_used(next_id_))
				++next_id_;
			connection.id = next_id_++;
		}
		else
		{
			if (id_used(connection.id))
				return invalid_object;
			if (connection.id >= next_id_)
				next_id_ = connection.id + 1;
		}

		if (!to->multi)
		{
			connections_.erase(std::remove_if(connections_.begin(),
				connections_.end(), [&](const node_connection& current) {
					return current.to_port == to_port &&
						current.from_port != from_port;
				}), connections_.end());
		}

		connection.from_node = from_node;
		connection.from_port = from_port;
		connection.to_node = to_node;
		connection.to_port = to_port;
		const object_id id = connection.id;
		connections_.push_back(std::move(connection));
		prune_selection();
		notify_change();
		return id;
	}

	bool node_graph_view::update_connection(const node_connection& connection)
	{
		node_connection* existing = find_connection(connection.id);
		if (!existing)
			return false;

		object_id from_node = invalid_object;
		object_id from_port = invalid_object;
		object_id to_node = invalid_object;
		object_id to_port = invalid_object;
		const node_port* from = nullptr;
		const node_port* to = nullptr;
		if (!resolve_connection_ports(connection.from_port, connection.to_port,
			&from_node, &from_port, &to_node, &to_port, &from, &to))
			return false;
		if (connection.from_node != invalid_object &&
			connection.from_node != from_node)
			return false;
		if (connection.to_node != invalid_object &&
			connection.to_node != to_node)
			return false;

		for (const node_connection& current : connections_)
		{
			if (current.id == connection.id)
				continue;
			if ((current.from_port == from_port &&
				current.to_port == to_port) ||
				(current.from_port == to_port &&
					current.to_port == from_port))
				return false;
		}

		if (!to->multi)
		{
			connections_.erase(std::remove_if(connections_.begin(),
				connections_.end(), [&](const node_connection& current) {
					return current.id != connection.id &&
						current.to_port == to_port;
				}), connections_.end());
			existing = find_connection(connection.id);
			if (!existing)
				return false;
		}

		node_connection next = connection;
		next.from_node = from_node;
		next.from_port = from_port;
		next.to_node = to_node;
		next.to_port = to_port;
		*existing = std::move(next);
		prune_selection();
		notify_change();
		return true;
	}

	bool node_graph_view::remove_connection(object_id id)
	{
		auto found = std::find_if(connections_.begin(), connections_.end(),
			[&](const node_connection& connection) {
				return connection.id == id;
			});
		if (found == connections_.end())
			return false;
		connections_.erase(found);
		prune_selection();
		notify_change();
		return true;
	}

	void node_graph_view::clear_connections()
	{
		if (connections_.empty())
			return;
		connections_.clear();
		prune_selection();
		notify_change();
	}

	const std::vector<node_connection>& node_graph_view::connections()
		const noexcept
	{
		return connections_;
	}

	const node_connection* node_graph_view::find_connection(object_id id)
		const noexcept
	{
		if (id == invalid_object)
			return nullptr;
		for (const node_connection& connection : connections_)
			if (connection.id == id)
				return &connection;
		return nullptr;
	}

	node_connection* node_graph_view::find_connection(object_id id) noexcept
	{
		if (id == invalid_object)
			return nullptr;
		for (node_connection& connection : connections_)
			if (connection.id == id)
				return &connection;
		return nullptr;
	}

	std::size_t node_graph_view::connection_count() const noexcept
	{
		return connections_.size();
	}

	bool node_graph_view::drop_invalid_connections()
	{
		std::vector<node_connection> kept;
		kept.reserve(connections_.size());
		std::vector<object_id> ids;
		std::vector<object_id> single_inputs;
		ids.reserve(connections_.size());
		for (const node_connection& connection : connections_)
		{
			if (connection.id == invalid_object ||
				std::find(ids.begin(), ids.end(), connection.id) != ids.end())
				continue;
			object_id from_node = invalid_object;
			object_id to_node = invalid_object;
			node_port_direction from_direction = node_port_direction::input;
			node_port_direction to_direction = node_port_direction::input;
			const node_port* from = find_port(connection.from_port, &from_node,
				&from_direction);
			const node_port* to = find_port(connection.to_port, &to_node,
				&to_direction);
			if (!from || !to ||
				from_direction != node_port_direction::output ||
				to_direction != node_port_direction::input)
				continue;
			if (connection.from_node != from_node ||
				connection.to_node != to_node)
				continue;
			if (!find_node(from_node) || !find_node(to_node))
				continue;
			if (!ports_compatible(*from, *to))
				continue;
			if (!to->multi &&
				std::find(single_inputs.begin(), single_inputs.end(),
					connection.to_port) != single_inputs.end())
				continue;
			if (!to->multi)
				single_inputs.push_back(connection.to_port);
			ids.push_back(connection.id);
			kept.push_back(connection);
		}
		if (kept.size() == connections_.size())
			return false;
		connections_ = std::move(kept);
		return true;
	}

	void node_graph_view::drop_connections_for_port(object_id port)
	{
		connections_.erase(std::remove_if(connections_.begin(),
			connections_.end(), [&](const node_connection& connection) {
				return connection.from_port == port ||
					connection.to_port == port;
			}), connections_.end());
	}

	void node_graph_view::drop_connections_for_node(object_id node)
	{
		connections_.erase(std::remove_if(connections_.begin(),
			connections_.end(), [&](const node_connection& connection) {
				return connection.from_node == node ||
					connection.to_node == node;
			}), connections_.end());
	}

	/* --------------------------------------------------------------------- */
	/* Selection                                                             */
	/* --------------------------------------------------------------------- */

	void node_graph_view::set_selection(const std::vector<object_id>& ids,
		object_id primary)
	{
		std::vector<object_id> filtered;
		filtered.reserve(ids.size());
		for (object_id id : ids)
		{
			if (id == invalid_object ||
				(find_node(id) == nullptr &&
					find_connection(id) == nullptr))
				continue;
			if (std::find(filtered.begin(), filtered.end(), id) !=
				filtered.end())
				continue;
			filtered.push_back(id);
		}
		if (primary == invalid_object ||
			std::find(filtered.begin(), filtered.end(), primary) ==
				filtered.end())
			primary = filtered.empty() ? invalid_object : filtered.front();
		if (filtered == selection_ && primary == primary_)
			return;
		selection_ = std::move(filtered);
		primary_ = primary;
		notify_selection();
	}

	void node_graph_view::clear_selection()
	{
		set_selection({});
	}

	void node_graph_view::select_all()
	{
		std::vector<object_id> ids;
		ids.reserve(nodes_.size() + connections_.size());
		for (const node_node& node : nodes_)
			ids.push_back(node.id);
		for (const node_connection& connection : connections_)
			ids.push_back(connection.id);
		set_selection(ids, ids.empty() ? invalid_object : ids.front());
	}

	const std::vector<object_id>& node_graph_view::selection_ids() const noexcept
	{
		return selection_;
	}

	object_id node_graph_view::primary_selection() const noexcept
	{
		return primary_;
	}

	bool node_graph_view::is_selected(object_id id) const noexcept
	{
		return std::find(selection_.begin(), selection_.end(), id) !=
			selection_.end();
	}

	void node_graph_view::set_multi_select(bool enabled)
	{
		multi_select_ = enabled;
	}

	bool node_graph_view::multi_select() const noexcept
	{
		return multi_select_;
	}

	void node_graph_view::set_selection_listener(selection_listener listener)
	{
		selection_listener_ = std::move(listener);
	}

	void node_graph_view::set_change_listener(change_listener listener)
	{
		change_listener_ = std::move(listener);
	}

	void node_graph_view::notify_change()
	{
		if (change_listener_)
			change_listener_();
	}

	void node_graph_view::notify_selection()
	{
		if (selection_listener_)
			selection_listener_(selection_);
	}

	void node_graph_view::prune_selection()
	{
		std::vector<object_id> filtered;
		filtered.reserve(selection_.size());
		for (object_id id : selection_)
		{
			if (find_node(id) != nullptr ||
				find_connection(id) != nullptr)
				filtered.push_back(id);
		}
		if (filtered.size() == selection_.size())
			return;
		object_id primary = primary_;
		if (std::find(filtered.begin(), filtered.end(), primary) ==
			filtered.end())
			primary = filtered.empty() ? invalid_object : filtered.front();
		selection_ = std::move(filtered);
		primary_ = primary;
		notify_selection();
	}

	void node_graph_view::apply_selection(const std::vector<object_id>& picked,
		int mods)
	{
		const bool ctrl = multi_select_ && (mods & VGUI_MOD_CTRL) != 0;
		const bool shift = multi_select_ && (mods & VGUI_MOD_SHIFT) != 0;
		if (picked.empty() && (ctrl || shift))
			return;

		std::vector<object_id> next;
		object_id primary = invalid_object;
		if (ctrl)
		{
			next = selection_;
			for (object_id id : picked)
			{
				if (find_node(id) == nullptr &&
					find_connection(id) == nullptr)
					continue;
				auto found = std::find(next.begin(), next.end(), id);
				if (found != next.end())
					next.erase(found);
				else
					next.push_back(id);
			}
			primary = primary_;
		}
		else if (shift)
		{
			next = selection_;
			for (object_id id : picked)
			{
				if ((find_node(id) != nullptr ||
					find_connection(id) != nullptr) &&
					std::find(next.begin(), next.end(), id) == next.end())
					next.push_back(id);
			}
			primary = primary_;
		}
		else
		{
			next = picked;
			if (!next.empty())
				primary = next.front();
		}
		if (primary == invalid_object ||
			std::find(next.begin(), next.end(), primary) == next.end())
			primary = next.empty() ? invalid_object : next.front();
		set_selection(next, primary);
	}

	std::vector<object_id> node_graph_view::entities_in_rect(double x0,
		double y0, double x1, double y1) const
	{
		std::vector<object_id> result;
		const double left = std::min(x0, x1);
		const double right = std::max(x0, x1);
		const double top = std::min(y0, y1);
		const double bottom = std::max(y0, y1);
		if (right - left <= 0.0 || bottom - top <= 0.0)
			return result;

		for (const node_node& node : nodes_)
		{
			if (!node.visible)
				continue;
			const node_graph_rect rect = node_rect(node.id);
			if (rect.empty())
				continue;
			if (rect.right() < left || rect.x > right ||
				rect.bottom() < top || rect.y > bottom)
				continue;
			result.push_back(node.id);
		}

		const node_graph_rect marquee = { left, top, right - left,
			bottom - top };
		for (const node_connection& connection : connections_)
		{
			if (!connection.visible)
				continue;
			const node_node* from_node = find_node(connection.from_node);
			const node_node* to_node = find_node(connection.to_node);
			if (!from_node || !to_node || !from_node->visible ||
				!to_node->visible)
				continue;
			if (curve_intersects_rect(connection, marquee))
				result.push_back(connection.id);
		}
		return result;
	}

	/* --------------------------------------------------------------------- */
	/* View / zoom / pan / grid                                              */
	/* --------------------------------------------------------------------- */

	node_graph_view_state node_graph_view::view_state() const noexcept
	{
		return view_;
	}

	void node_graph_view::set_view_state(const node_graph_view_state& state)
	{
		if (gesture_ != node_graph_gesture::none)
			cancel_gesture();
		node_graph_view_state next = state;
		if (!finite(next.offset_x))
			next.offset_x = 0.0;
		if (!finite(next.offset_y))
			next.offset_y = 0.0;
		if (!finite(next.zoom) || next.zoom <= 0.0)
			next.zoom = 1.0;
		next.zoom = std::clamp(next.zoom, min_zoom_, max_zoom_);
		if (std::fabs(next.offset_x - view_.offset_x) <= kEpsilon &&
			std::fabs(next.offset_y - view_.offset_y) <= kEpsilon &&
			std::fabs(next.zoom - view_.zoom) <= kEpsilon)
			return;
		view_ = next;
		notify_change();
	}

	double node_graph_view::zoom() const noexcept
	{
		return view_.zoom;
	}

	float node_graph_view::ui_scale() const noexcept
	{
		return ui_scale_of(widget_);
	}

	double node_graph_view::effective_zoom() const noexcept
	{
		return view_.zoom * (double)ui_scale();
	}

	void node_graph_view::set_zoom_limits(double minimum, double maximum)
	{
		if (!finite(minimum) || minimum <= 0.0)
			minimum = 0.001;
		if (!finite(maximum) || maximum < minimum)
			maximum = minimum;
		min_zoom_ = minimum;
		max_zoom_ = maximum;
		if (view_.zoom < min_zoom_ || view_.zoom > max_zoom_)
		{
			view_.zoom = std::clamp(view_.zoom, min_zoom_, max_zoom_);
			notify_change();
		}
	}

	void node_graph_view::zoom_at(float content_x, float content_y,
		double factor)
	{
		if (!finite(factor) || factor <= 0.0)
			return;
		if (gesture_ != node_graph_gesture::none)
			cancel_gesture();
		const node_graph_point anchor = screen_to_graph(content_x,
			content_y);
		const double next = std::clamp(view_.zoom * factor, min_zoom_,
			max_zoom_);
		if (std::fabs(next - view_.zoom) <= kEpsilon)
			return;
		view_.zoom = next;
		const double zoom = effective_zoom();
		view_.offset_x = anchor.x - (double)content_x / zoom;
		view_.offset_y = anchor.y - (double)content_y / zoom;
		notify_change();
	}

	void node_graph_view::pan_by_pixels(float delta_x, float delta_y)
	{
		if (!finite(delta_x) || !finite(delta_y))
			return;
		if (delta_x == 0.0f && delta_y == 0.0f)
			return;
		if (gesture_ != node_graph_gesture::none)
			cancel_gesture();
		const double zoom = effective_zoom();
		view_.offset_x -= (double)delta_x / zoom;
		view_.offset_y -= (double)delta_y / zoom;
		notify_change();
	}

	void node_graph_view::reset_view()
	{
		node_graph_view_state next;
		next.zoom = std::clamp(1.0, min_zoom_, max_zoom_);
		set_view_state(next);
	}

	void node_graph_view::frame_selection()
	{
		const node_graph_rect bounds = selection_bounds();
		if (bounds.empty())
		{
			fit_all();
			return;
		}
		apply_view_to_bounds(bounds, kSelectionMargin);
	}

	void node_graph_view::fit_all()
	{
		const node_graph_rect bounds = content_bounds();
		if (bounds.empty())
		{
			node_graph_view_state next;
			next.zoom = std::clamp(1.0, min_zoom_, max_zoom_);
			next.offset_x = -16.0;
			next.offset_y = -16.0;
			set_view_state(next);
			return;
		}
		apply_view_to_bounds(bounds, kSelectionMargin);
	}

	void node_graph_view::apply_view_to_bounds(const node_graph_rect& bounds,
		float margin_pixels)
	{
		if (bounds.empty())
			return;
		float width = 0.0f, height = 0.0f;
		content_geometry(nullptr, nullptr, &width, &height);
		const double scale = (double)ui_scale();
		const double margin = std::max(8.0, (double)margin_pixels) * scale;
		double zoom = view_.zoom;
		if (width > margin * 2.0f && height > margin * 2.0f &&
			bounds.width > 0.0 && bounds.height > 0.0)
		{
			/* Fit into render pixels, then store the logical zoom. */
			zoom = std::min((double)(width - margin * 2.0f) / bounds.width,
				(double)(height - margin * 2.0f) / bounds.height) / scale;
		}
		zoom = std::clamp(zoom, min_zoom_, max_zoom_);
		const double content_width = width > 0.0f ? width : 640.0 * scale;
		const double content_height = height > 0.0f ? height : 360.0 * scale;
		const double effective = zoom * scale;
		node_graph_view_state next;
		next.zoom = zoom;
		next.offset_x = bounds.x + bounds.width * 0.5 -
			content_width * 0.5 / effective;
		next.offset_y = bounds.y + bounds.height * 0.5 -
			content_height * 0.5 / effective;
		set_view_state(next);
	}

	void node_graph_view::ensure_node_visible(object_id node_id)
	{
		const node_node* node = find_node(node_id);
		if (!node)
			return;
		apply_view_to_bounds(node_rect(node_id), 70.0f);
	}

	void node_graph_view::set_grid_visible(bool visible)
	{
		if (grid_visible_ == visible)
			return;
		grid_visible_ = visible;
		notify_change();
	}

	bool node_graph_view::grid_visible() const noexcept
	{
		return grid_visible_;
	}

	void node_graph_view::set_grid_step(double step)
	{
		if (!finite(step) || step <= 0.0)
			return;
		step = std::clamp(step, 1.0, 4096.0);
		if (std::fabs(step - grid_step_) <= kEpsilon)
			return;
		grid_step_ = step;
		notify_change();
	}

	double node_graph_view::grid_step() const noexcept
	{
		return grid_step_;
	}

	double node_graph_view::effective_grid_step() const noexcept
	{
		double step = finite(grid_step_) && grid_step_ > 0.0
			? grid_step_ : 24.0;
		for (int guard = 0; guard < 64 && step * view_.zoom < 12.0; ++guard)
			step *= 2.0;
		return step;
	}

	void node_graph_view::set_snap_to_grid(bool enabled)
	{
		if (snap_to_grid_ == enabled)
			return;
		snap_to_grid_ = enabled;
		notify_change();
	}

	bool node_graph_view::snap_to_grid() const noexcept
	{
		return snap_to_grid_;
	}

	node_graph_point node_graph_view::snap_point(node_graph_point point) const
		noexcept
	{
		if (!snap_to_grid_)
			return point;
		const double step = effective_grid_step();
		if (!(step > 0.0) || !finite(step))
			return point;
		return { std::round(point.x / step) * step,
			std::round(point.y / step) * step };
	}

	void node_graph_view::set_minimap_visible(bool visible)
	{
		if (minimap_visible_ == visible)
			return;
		minimap_visible_ = visible;
		notify_change();
	}

	bool node_graph_view::minimap_visible() const noexcept
	{
		return minimap_visible_;
	}

	node_graph_rect node_graph_view::minimap_content_rect() const
	{
		node_graph_rect empty;
		if (!minimap_visible_)
			return empty;
		float width = 0.0f, height = 0.0f;
		if (!content_geometry(nullptr, nullptr, &width, &height))
			return empty;
		const double scale = (double)ui_scale();
		if (width < 220.0f * scale || height < 130.0f * scale)
			return empty;
		const double mini_width = std::clamp((double)width * 0.19,
			110.0 * scale, 200.0 * scale);
		const double mini_height = std::clamp((double)height * 0.26,
			70.0 * scale, 140.0 * scale);
		node_graph_rect rect;
		rect.width = mini_width;
		rect.height = mini_height;
		rect.x = (double)width - mini_width - kMinimapMargin * scale;
		rect.y = (double)height - mini_height - kMinimapMargin * scale;
		if (rect.x < 0.0 || rect.y < 0.0)
			return empty;
		return rect;
	}

	bool node_graph_view::minimap_rect(float* x, float* y, float* width,
		float* height) const
	{
		const node_graph_rect rect = minimap_content_rect();
		if (rect.empty())
			return false;
		if (x)
			*x = (float)rect.x;
		if (y)
			*y = (float)rect.y;
		if (width)
			*width = (float)rect.width;
		if (height)
			*height = (float)rect.height;
		return true;
	}

	node_graph_rect node_graph_view::minimap_graph_bounds() const
	{
		node_graph_rect bounds = content_bounds();
		float width = 0.0f, height = 0.0f;
		content_geometry(nullptr, nullptr, &width, &height);
		const double zoom = effective_zoom();
		const node_graph_rect viewport = {
			view_.offset_x, view_.offset_y,
			(double)width / zoom, (double)height / zoom
		};
		if (bounds.empty())
		{
			bounds = viewport;
		}
		else if (!viewport.empty())
		{
			const double left = std::min(bounds.x, viewport.x);
			const double top = std::min(bounds.y, viewport.y);
			const double right = std::max(bounds.right(), viewport.right());
			const double bottom = std::max(bounds.bottom(), viewport.bottom());
			bounds = { left, top, right - left, bottom - top };
		}
		if (!(bounds.width > 0.0) || !finite(bounds.width))
			bounds.width = 1.0;
		if (!(bounds.height > 0.0) || !finite(bounds.height))
			bounds.height = 1.0;
		const double pad_x = bounds.width * 0.04;
		const double pad_y = bounds.height * 0.04;
		bounds.x -= pad_x;
		bounds.y -= pad_y;
		bounds.width += pad_x * 2.0;
		bounds.height += pad_y * 2.0;
		return bounds;
	}

	void node_graph_view::navigate_minimap(float content_x, float content_y)
	{
		const node_graph_rect rect = minimap_content_rect();
		if (rect.empty())
			return;
		float width = 0.0f, height = 0.0f;
		if (!content_geometry(nullptr, nullptr, &width, &height))
			return;
		const double inset = kMinimapInset * (double)ui_scale();
		const double inner_x = rect.x + inset;
		const double inner_y = rect.y + inset;
		const double inner_w = std::max(1.0, rect.width - inset * 2.0);
		const double inner_h = std::max(1.0, rect.height - inset * 2.0);
		const double u = std::clamp(((double)content_x - inner_x) / inner_w,
			0.0, 1.0);
		const double v = std::clamp(((double)content_y - inner_y) / inner_h,
			0.0, 1.0);
		const node_graph_rect bounds = minimap_graph_bounds();
		const double graph_x = bounds.x + bounds.width * u;
		const double graph_y = bounds.y + bounds.height * v;
		const double zoom = effective_zoom();
		view_.offset_x = graph_x - (double)width * 0.5 / zoom;
		view_.offset_y = graph_y - (double)height * 0.5 / zoom;
		notify_change();
	}

	/* --------------------------------------------------------------------- */
	/* Coordinates / geometry                                                */
	/* --------------------------------------------------------------------- */

	bool node_graph_view::content_geometry(float* offset_x, float* offset_y,
		float* width, float* height) const
	{
		if (offset_x)
			*offset_x = kBorderInset;
		if (offset_y)
			*offset_y = kBorderInset;
		if (!widget_)
		{
			if (width)
				*width = 0.0f;
			if (height)
				*height = 0.0f;
			return false;
		}
		float wx = 0.0f, wy = 0.0f, ww = 0.0f, wh = 0.0f;
		vgui_widget_get_rect(widget_, &wx, &wy, &ww, &wh);
		(void)wx;
		(void)wy;
		if (width)
			*width = std::max(0.0f, ww - kBorderInset * 2.0f);
		if (height)
			*height = std::max(0.0f, wh - kBorderInset * 2.0f);
		return ww > kBorderInset * 2.0f && wh > kBorderInset * 2.0f;
	}

	node_graph_point node_graph_view::screen_to_graph(float content_x,
		float content_y) const noexcept
	{
		const double zoom = effective_zoom();
		return { view_.offset_x + (double)content_x / zoom,
			view_.offset_y + (double)content_y / zoom };
	}

	node_graph_point node_graph_view::graph_to_screen(double graph_x,
		double graph_y) const noexcept
	{
		const double zoom = effective_zoom();
		return { (graph_x - view_.offset_x) * zoom,
			(graph_y - view_.offset_y) * zoom };
	}

	double node_graph_view::node_header_height() const noexcept
	{
		return kNodeHeaderHeight;
	}

	double node_graph_view::node_row_height() const noexcept
	{
		return kNodeRowHeight;
	}

	double node_graph_view::node_height(const node_node& node) const noexcept
	{
		if (node.collapsed)
			return kNodeHeaderHeight;
		const std::size_t rows = std::max<std::size_t>(1,
			std::max(node.inputs.size(), node.outputs.size()));
		return kNodeHeaderHeight + (double)rows * kNodeRowHeight +
			kNodeBodyPadding;
	}

	double node_graph_view::port_radius() const noexcept
	{
		return kPortRadius;
	}

	node_graph_rect node_graph_view::node_rect(object_id node_id) const
	{
		const node_node* node = find_node(node_id);
		if (!node)
			return {};
		const double x = finite(node->x) ? node->x : 0.0;
		const double y = finite(node->y) ? node->y : 0.0;
		return { x, y, clamp_width(node->width), node_height(*node) };
	}

	bool node_graph_view::port_screen_position(const node_node& node,
		const node_port& port, node_port_direction direction,
		node_graph_point* graph, node_graph_point* screen) const
	{
		if (node.collapsed)
			return false;
		const std::vector<node_port>& ports =
			direction == node_port_direction::input ? node.inputs
				: node.outputs;
		auto found = std::find_if(ports.begin(), ports.end(),
			[&](const node_port& entry) { return entry.id == port.id; });
		if (found == ports.end())
			return false;
		const std::size_t index = (std::size_t)(found - ports.begin());
		const double x = direction == node_port_direction::input
			? node.x + kPortInset
			: node.x + clamp_width(node.width) - kPortInset;
		const double y = node.y + kNodeHeaderHeight +
			((double)index + 0.5) * kNodeRowHeight;
		if (graph)
			*graph = { x, y };
		if (screen)
			*screen = graph_to_screen(x, y);
		return true;
	}

	node_graph_rect node_graph_view::port_rect(object_id port_id) const
	{
		node_graph_point center;
		if (!port_position(port_id, &center))
			return {};
		const double radius = port_radius();
		return { center.x - radius, center.y - radius, radius * 2.0,
			radius * 2.0 };
	}

	bool node_graph_view::port_position(object_id port_id,
		node_graph_point* point) const
	{
		object_id node_id = invalid_object;
		node_port_direction direction = node_port_direction::input;
		const node_port* port = find_port(port_id, &node_id, &direction);
		if (!port)
			return false;
		const node_node* node = find_node(node_id);
		if (!node)
			return false;
		node_graph_point graph;
		if (!port_screen_position(*node, *port, direction, &graph, nullptr))
			return false;
		if (point)
			*point = graph;
		return true;
	}

	node_graph_rect node_graph_view::selection_bounds() const
	{
		bool any = false;
		double left = 0.0;
		double top = 0.0;
		double right = 0.0;
		double bottom = 0.0;
		auto include = [&](double x, double y) {
			if (!any)
			{
				left = right = x;
				top = bottom = y;
				any = true;
				return;
			}
			left = std::min(left, x);
			right = std::max(right, x);
			top = std::min(top, y);
			bottom = std::max(bottom, y);
		};
		for (object_id id : selection_)
		{
			if (const node_node* node = find_node(id))
			{
				const node_graph_rect rect = node_rect(node->id);
				if (!rect.empty())
				{
					include(rect.x, rect.y);
					include(rect.right(), rect.bottom());
				}
				continue;
			}
			const node_connection* connection = find_connection(id);
			if (!connection || !connection->visible)
				continue;
			for (int i = 0; i <= 24; ++i)
			{
				const node_graph_point point = sample_connection(*connection,
					(double)i / 24.0);
				include(point.x, point.y);
			}
		}
		if (!any)
			return {};
		return { left, top, right - left, bottom - top };
	}

	node_graph_rect node_graph_view::content_bounds() const
	{
		bool any = false;
		double left = 0.0;
		double top = 0.0;
		double right = 0.0;
		double bottom = 0.0;
		for (const node_node& node : nodes_)
		{
			if (!node.visible)
				continue;
			const node_graph_rect rect = node_rect(node.id);
			if (rect.empty())
				continue;
			if (!any)
			{
				left = rect.x;
				top = rect.y;
				right = rect.right();
				bottom = rect.bottom();
				any = true;
				continue;
			}
			left = std::min(left, rect.x);
			top = std::min(top, rect.y);
			right = std::max(right, rect.right());
			bottom = std::max(bottom, rect.bottom());
		}
		if (!any)
			return {};
		return { left, top, right - left, bottom - top };
	}

	double node_graph_view::connection_hit_tolerance() const noexcept
	{
		return 6.0 * (double)ui_scale() / effective_zoom();
	}

	object_id node_graph_view::node_at(float content_x,
		float content_y) const
	{
		const node_graph_point graph = screen_to_graph(content_x, content_y);
		for (auto it = nodes_.rbegin(); it != nodes_.rend(); ++it)
		{
			if (!it->visible)
				continue;
			const node_graph_rect rect = node_rect(it->id);
			if (rect.contains(graph.x, graph.y))
				return it->id;
		}
		return invalid_object;
	}

	object_id node_graph_view::port_at(float content_x,
		float content_y) const
	{
		const double scale = (double)ui_scale();
		const double threshold = std::max(6.0 * scale,
			port_radius() * effective_zoom() + 2.0 * scale);
		for (auto it = nodes_.rbegin(); it != nodes_.rend(); ++it)
		{
			if (!it->visible || it->collapsed)
				continue;
			auto test = [&](const std::vector<node_port>& ports,
				node_port_direction direction) -> object_id {
				for (const node_port& port : ports)
				{
					node_graph_point screen;
					if (!port_screen_position(*it, port, direction,
						nullptr, &screen))
						continue;
					if (std::hypot(screen.x - (double)content_x,
						screen.y - (double)content_y) <= threshold)
						return port.id;
				}
				return invalid_object;
			};
			const object_id input = test(it->inputs,
				node_port_direction::input);
			if (input != invalid_object)
				return input;
			const object_id output = test(it->outputs,
				node_port_direction::output);
			if (output != invalid_object)
				return output;
		}
		return invalid_object;
	}

	object_id node_graph_view::connection_at(float content_x,
		float content_y) const
	{
		const double tolerance = 6.0;
		for (auto it = connections_.rbegin(); it != connections_.rend(); ++it)
		{
			if (!it->visible)
				continue;
			const node_connection& connection = *it;
			const node_node* from_node = find_node(connection.from_node);
			const node_node* to_node = find_node(connection.to_node);
			if (!from_node || !to_node || !from_node->visible ||
				!to_node->visible)
				continue;
			node_graph_point from;
			node_graph_point to;
			connection_points(connection, &from, &to);
			node_graph_point c1;
			node_graph_point c2;
			bezier_controls(from, to, &c1, &c2);
			node_graph_point previous = from;
			node_graph_point previous_screen = graph_to_screen(previous.x,
				previous.y);
			for (int i = 1; i <= 48; ++i)
			{
				const node_graph_point point = cubic_at(from, c1, c2, to,
					(double)i / 48.0);
				const node_graph_point screen = graph_to_screen(point.x,
					point.y);
				if (distance_to_segment(content_x, content_y,
					previous_screen.x, previous_screen.y, screen.x,
					screen.y) <= tolerance)
					return connection.id;
				previous = point;
				previous_screen = screen;
			}
		}
		return invalid_object;
	}

	object_id node_graph_view::hit_test(float content_x,
		float content_y) const
	{
		const object_id port = port_at(content_x, content_y);
		if (port != invalid_object)
			return port;
		const object_id node = node_at(content_x, content_y);
		if (node != invalid_object)
			return node;
		return connection_at(content_x, content_y);
	}

	void node_graph_view::connection_points(
		const node_connection& connection, node_graph_point* from,
		node_graph_point* to) const
	{
		node_graph_point from_point;
		node_graph_point to_point;
		if (!port_position(connection.from_port, &from_point) ||
			!port_position(connection.to_port, &to_point))
		{
			if (from)
				*from = {};
			if (to)
				*to = {};
			return;
		}
		if (from)
			*from = from_point;
		if (to)
			*to = to_point;
	}

	node_graph_point node_graph_view::sample_connection(
		const node_connection& connection, double t) const
	{
		node_graph_point from;
		node_graph_point to;
		connection_points(connection, &from, &to);
		node_graph_point c1;
		node_graph_point c2;
		bezier_controls(from, to, &c1, &c2);
		return cubic_at(from, c1, c2, to, std::clamp(t, 0.0, 1.0));
	}

	bool node_graph_view::curve_intersects_rect(
		const node_connection& connection, const node_graph_rect& rect) const
	{
		node_graph_point from;
		node_graph_point to;
		connection_points(connection, &from, &to);
		node_graph_point c1;
		node_graph_point c2;
		bezier_controls(from, to, &c1, &c2);
		node_graph_point previous = from;
		for (int i = 1; i <= 40; ++i)
		{
			const node_graph_point point = cubic_at(from, c1, c2, to,
				(double)i / 40.0);
			if (segment_intersects_rect(previous.x, previous.y, point.x,
				point.y, rect))
				return true;
			previous = point;
		}
		return false;
	}

	/* --------------------------------------------------------------------- */
	/* Editing / undo                                                        */
	/* --------------------------------------------------------------------- */

	bool node_graph_view::dragging() const noexcept
	{
		return gesture_ != node_graph_gesture::none;
	}

	node_graph_gesture node_graph_view::gesture() const noexcept
	{
		return gesture_;
	}

	object_id node_graph_view::pending_connection_port() const noexcept
	{
		return gesture_ == node_graph_gesture::connect ? connect_from_port_
			: invalid_object;
	}

	void node_graph_view::set_drag_threshold(float pixels)
	{
		drag_threshold_ = pixels;
	}

	float node_graph_view::drag_threshold() const noexcept
	{
		return drag_threshold_;
	}

	float node_graph_view::effective_drag_threshold() const
	{
		if (drag_threshold_ >= 0.0f)
			return drag_threshold_;
		if (context_)
		{
			vgui_style style = {};
			vgui_context_get_style(context_, &style);
			return std::max(1.0f, style.dock.drag_threshold);
		}
		return 4.0f;
	}

	bool node_graph_view::connect_ports(object_id port_a, object_id port_b)
	{
		object_id from_node = invalid_object;
		object_id from_port = invalid_object;
		object_id to_node = invalid_object;
		object_id to_port = invalid_object;
		const node_port* from = nullptr;
		const node_port* to = nullptr;
		if (!resolve_connection_ports(port_a, port_b, &from_node, &from_port,
			&to_node, &to_port, &from, &to))
			return false;
		if (connection_exists(from_port, to_port))
			return false;

		const snapshot before = capture_snapshot();
		node_connection connection;
		connection.from_node = from_node;
		connection.from_port = from_port;
		connection.to_node = to_node;
		connection.to_port = to_port;
		if (add_connection(connection) == invalid_object)
			return false;
		push_command("Connect nodes", before);
		return true;
	}

	bool node_graph_view::disconnect_selection()
	{
		if (selection_.empty())
			return false;
		const snapshot before = capture_snapshot();
		std::vector<object_id> nodes;
		std::vector<object_id> direct;
		for (object_id id : selection_)
		{
			if (find_node(id) != nullptr)
				nodes.push_back(id);
			else if (find_connection(id) != nullptr)
				direct.push_back(id);
		}
		std::size_t removed = 0;
		for (object_id id : direct)
		{
			if (remove_connection(id))
				++removed;
		}
		for (object_id node : nodes)
		{
			for (auto it = connections_.begin(); it != connections_.end();)
			{
				if (it->from_node != node && it->to_node != node)
				{
					++it;
					continue;
				}
				it = connections_.erase(it);
				++removed;
			}
		}
		if (removed == 0)
			return false;
		prune_selection();
		notify_change();
		push_command("Disconnect nodes", before);
		return true;
	}

	bool node_graph_view::delete_selection()
	{
		if (selection_.empty())
			return false;
		const snapshot before = capture_snapshot();
		std::vector<object_id> nodes;
		std::vector<object_id> direct;
		for (object_id id : selection_)
		{
			if (find_node(id) != nullptr)
				nodes.push_back(id);
			else if (find_connection(id) != nullptr)
				direct.push_back(id);
		}
		bool changed = false;
		for (object_id id : direct)
			changed = remove_connection(id) || changed;
		for (object_id node : nodes)
		{
			auto found = std::find_if(nodes_.begin(), nodes_.end(),
				[&](const node_node& current) {
					return current.id == node;
				});
			if (found == nodes_.end())
				continue;
			nodes_.erase(found);
			drop_connections_for_node(node);
			changed = true;
		}
		if (!changed)
			return false;
		selection_.clear();
		primary_ = invalid_object;
		refresh_search_matches();
		notify_selection();
		notify_change();
		push_command("Delete nodes", before);
		return true;
	}

	bool node_graph_view::duplicate_selection()
	{
		std::vector<object_id> selected_nodes;
		for (object_id id : selection_)
			if (find_node(id) != nullptr)
				selected_nodes.push_back(id);
		if (selected_nodes.empty())
			return false;

		const snapshot before = capture_snapshot();
		std::vector<object_id> created_nodes;
		std::vector<object_id> created_connections;
		std::vector<object_id> new_selection;
		std::map<object_id, object_id> node_map;
		std::map<object_id, object_id> port_map;

		for (object_id id : selected_nodes)
		{
			const node_node* source = find_node(id);
			if (!source)
				continue;
			node_node copy = *source;
			copy.id = invalid_object;
			copy.x += kDuplicateOffset;
			copy.y += kDuplicateOffset;
			for (node_port& port : copy.inputs)
				port.id = invalid_object;
			for (node_port& port : copy.outputs)
				port.id = invalid_object;
			const object_id created = add_node(std::move(copy));
			if (created == invalid_object)
				continue;
			node_map[id] = created;
			created_nodes.push_back(created);
			new_selection.push_back(created);
			const node_node* added = find_node(created);
			const node_node* original = find_node(id);
			if (!added || !original)
				continue;
			for (std::size_t i = 0; i < original->inputs.size() &&
				i < added->inputs.size(); ++i)
				port_map[original->inputs[i].id] = added->inputs[i].id;
			for (std::size_t i = 0; i < original->outputs.size() &&
				i < added->outputs.size(); ++i)
				port_map[original->outputs[i].id] = added->outputs[i].id;
		}
		if (created_nodes.empty())
			return false;

		const std::vector<node_connection> original_connections =
			connections_;
		for (const node_connection& source : original_connections)
		{
			auto from = node_map.find(source.from_node);
			auto to = node_map.find(source.to_node);
			if (from == node_map.end() || to == node_map.end())
				continue;
			auto from_port = port_map.find(source.from_port);
			auto to_port = port_map.find(source.to_port);
			if (from_port == port_map.end() || to_port == port_map.end())
				continue;
			node_connection copy = source;
			copy.id = invalid_object;
			copy.from_node = from->second;
			copy.from_port = from_port->second;
			copy.to_node = to->second;
			copy.to_port = to_port->second;
			const object_id created = add_connection(copy);
			if (created != invalid_object)
			{
				created_connections.push_back(created);
				new_selection.push_back(created);
			}
		}

		set_selection(new_selection,
			new_selection.empty() ? invalid_object : new_selection.front());
		notify_change();
		push_command("Duplicate nodes", before);
		return true;
	}

	bool node_graph_view::default_node_at(node_node* node) const
	{
		if (!node)
			return false;
		node->id = invalid_object;
		node->title = "Node";
		node->category = "General";
		node->x = context_graph_.x;
		node->y = context_graph_.y;
		node->width = kDefaultNodeWidth;
		node->inputs.clear();
		node->outputs.clear();
		node_port input;
		input.name = "In";
		input.direction = node_port_direction::input;
		input.type = node_port_type::flow;
		node->inputs.push_back(input);
		node_port output;
		output.name = "Out";
		output.direction = node_port_direction::output;
		output.type = node_port_type::flow;
		node->outputs.push_back(output);
		return true;
	}

	bool node_graph_view::perform_action(node_graph_action action)
	{
		switch (action)
		{
		case node_graph_action::add_node:
		{
			node_node node;
			node.x = context_graph_.x;
			node.y = context_graph_.y;
			if (create_node_handler_)
			{
				if (!create_node_handler_(node))
					return false;
			}
			else if (!default_node_at(&node))
			{
				return false;
			}
			if (!finite(node.x))
				node.x = context_graph_.x;
			if (!finite(node.y))
				node.y = context_graph_.y;
			if (node.title.empty())
				node.title = "Node";
			node.width = clamp_width(node.width);
			if (snap_to_grid_)
			{
				const node_graph_point snapped = snap_point({ node.x, node.y });
				node.x = snapped.x;
				node.y = snapped.y;
			}
			const snapshot before = capture_snapshot();
			const object_id id = add_node(std::move(node));
			if (id == invalid_object)
				return false;
			set_selection({ id }, id);
			push_command("Add node", before);
			return true;
		}
		case node_graph_action::duplicate:
			return duplicate_selection();
		case node_graph_action::delete_selection:
			return delete_selection();
		case node_graph_action::disconnect:
			return disconnect_selection();
		case node_graph_action::frame_selection:
			frame_selection();
			return true;
		case node_graph_action::fit_all:
			fit_all();
			return true;
		case node_graph_action::select_all:
			select_all();
			return !selection_.empty();
		case node_graph_action::toggle_grid:
			set_grid_visible(!grid_visible_);
			return true;
		case node_graph_action::toggle_minimap:
			set_minimap_visible(!minimap_visible_);
			return true;
		case node_graph_action::toggle_snap:
			set_snap_to_grid(!snap_to_grid_);
			return true;
		default:
			return false;
		}
	}

	vgui_widget* node_graph_view::menu() const noexcept
	{
		return menu_;
	}

	void node_graph_view::set_create_node_handler(create_node_handler handler)
	{
		create_node_handler_ = std::move(handler);
	}

	void node_graph_view::open_context_menu(float content_x, float content_y)
	{
		context_graph_ = screen_to_graph(content_x, content_y);
		if (!menu_)
			return;

		vgui_menu_clear(menu_);
		auto add = [&](const char* text, node_graph_action action,
			const char* shortcut, bool checkable, bool checked) {
			const vgui_item_id item = checkable
				? vgui_menu_add_check_item(menu_, text, checked ? 1 : 0)
				: vgui_menu_add_item(menu_, text);
			if (!item)
				return;
			vgui_menu_set_item_user(menu_, item,
				reinterpret_cast<void*>(
					static_cast<std::uintptr_t>((int)action)));
			if (shortcut)
				vgui_menu_set_item_shortcut(menu_, item, shortcut);
		};
		add("Add Node", node_graph_action::add_node, nullptr, false, false);
		add("Duplicate", node_graph_action::duplicate, "Ctrl+D", false,
			false);
		add("Delete", node_graph_action::delete_selection, "Del", false,
			false);
		add("Disconnect", node_graph_action::disconnect, nullptr, false,
			false);
		vgui_menu_add_separator(menu_);
		add("Frame Selection", node_graph_action::frame_selection, "F", false,
			false);
		add("Fit All", node_graph_action::fit_all, "Home", false, false);
		add("Select All", node_graph_action::select_all, "Ctrl+A", false,
			false);
		vgui_menu_add_separator(menu_);
		add("Grid", node_graph_action::toggle_grid, nullptr, true,
			grid_visible_);
		add("Minimap", node_graph_action::toggle_minimap, nullptr, true,
			minimap_visible_);
		add("Snap to Grid", node_graph_action::toggle_snap, nullptr, true,
			snap_to_grid_);

		if (widget_)
			vgui_menu_open_for(menu_, widget_, content_x, content_y);
	}

	void node_graph_view::on_menu_activate(vgui_widget* menu,
		vgui_item_id item, void* user)
	{
		(void)menu;
		auto* self = static_cast<node_graph_view*>(user);
		if (!self || !self->menu_ || item == 0)
			return;
		const int action = (int)(std::uintptr_t)vgui_menu_get_item_user(
			self->menu_, item);
		self->perform_action((node_graph_action)action);
	}

	/* --------------------------------------------------------------------- */
	/* Search                                                                */
	/* --------------------------------------------------------------------- */

	void node_graph_view::refresh_search_matches()
	{
		search_matches_.clear();
		if (search_text_.empty())
			return;
		const std::string needle = lower_text(search_text_);
		for (const node_node& node : nodes_)
		{
			const std::string title = lower_text(node.title);
			const std::string category = lower_text(node.category);
			if (title.find(needle) != std::string::npos ||
				category.find(needle) != std::string::npos)
				search_matches_.push_back(node.id);
		}
		if (search_matches_.empty())
			search_match_index_ = 0;
		else
			search_match_index_ %= search_matches_.size();
	}

	void node_graph_view::set_search_text(std::string text)
	{
		if (search_text_ == text)
			return;
		search_text_ = std::move(text);
		refresh_search_matches();
		if (search_edit_)
		{
			const char* current = vgui_edit_get_text(search_edit_);
			if (!current || search_text_ != current)
				vgui_edit_set_text(search_edit_, search_text_.c_str());
		}
		notify_change();
	}

	const std::string& node_graph_view::search_text() const noexcept
	{
		return search_text_;
	}

	void node_graph_view::clear_search()
	{
		if (search_text_.empty() && search_matches_.empty())
			return;
		search_text_.clear();
		refresh_search_matches();
		if (search_edit_)
			vgui_edit_set_text(search_edit_, "");
		notify_change();
	}

	void node_graph_view::focus_search()
	{
		if (!context_ || !search_edit_)
			return;
		vgui_set_focus(context_, search_edit_);
		vgui_edit_select_all(search_edit_);
	}

	vgui_widget* node_graph_view::search_widget() const noexcept
	{
		return search_edit_;
	}

	std::size_t node_graph_view::search_match_count() const noexcept
	{
		return search_matches_.size();
	}

	const std::vector<object_id>& node_graph_view::search_matches() const noexcept
	{
		return search_matches_;
	}

	bool node_graph_view::node_matches_search(object_id node) const noexcept
	{
		if (search_text_.empty())
			return true;
		return std::find(search_matches_.begin(), search_matches_.end(),
			node) != search_matches_.end();
	}

	object_id node_graph_view::next_search_match()
	{
		if (search_matches_.empty())
			return invalid_object;
		if (search_match_index_ >= search_matches_.size())
			search_match_index_ = 0;
		const object_id id = search_matches_[search_match_index_];
		search_match_index_ = (search_match_index_ + 1) %
			search_matches_.size();
		if (find_node(id) == nullptr)
			return invalid_object;
		set_selection({ id }, id);
		ensure_node_visible(id);
		return id;
	}

	void node_graph_view::on_search_text(vgui_widget* widget,
		const char* text, void* user)
	{
		(void)widget;
		auto* self = static_cast<node_graph_view*>(user);
		if (!self)
			return;
		self->set_search_text(text ? text : "");
	}

	void node_graph_view::on_search_event(vgui_widget* widget,
		const vgui_event* event, void* user)
	{
		(void)widget;
		auto* self = static_cast<node_graph_view*>(user);
		if (!self || !event)
			return;
		if (event->type == VGUI_EVENT_KEY_DOWN)
		{
			if (event->key == VGUI_KEY_ENTER)
			{
				self->next_search_match();
				return;
			}
			if (event->key == VGUI_KEY_ESCAPE)
			{
				self->clear_search();
				self->request_focus();
				return;
			}
			return;
		}
		if (event->type == VGUI_EVENT_CANCEL)
		{
			self->clear_search();
			self->request_focus();
		}
	}

	/* --------------------------------------------------------------------- */
	/* Commands / persistence                                                */
	/* --------------------------------------------------------------------- */

	void node_graph_view::set_command_stack(command_stack* stack)
	{
		commands_ = stack;
	}

	command_stack* node_graph_view::command_stack_ptr() const noexcept
	{
		return commands_;
	}

	node_graph_view::snapshot node_graph_view::capture_snapshot() const
	{
		snapshot state;
		state.nodes = nodes_;
		state.connections = connections_;
		state.selection = selection_;
		state.primary = primary_;
		state.next_id = next_id_;
		return state;
	}

	void node_graph_view::restore_snapshot(const snapshot& state)
	{
		if (gesture_ != node_graph_gesture::none)
			reset_gesture();
		nodes_ = state.nodes;
		connections_ = state.connections;
		selection_ = state.selection;
		primary_ = state.primary;
		next_id_ = state.next_id;
		if (next_id_ == invalid_object)
			next_id_ = 1;
		prune_selection();
		refresh_search_matches();
		notify_selection();
		notify_change();
	}

	void node_graph_view::push_command(const char* name,
		const snapshot& before)
	{
		if (!commands_)
			return;
		const snapshot after = capture_snapshot();
		std::weak_ptr<int> alive = alive_token_;
		auto apply = [this, alive](const snapshot& state) {
			if (!alive.expired())
				restore_snapshot(state);
		};
		commands_->push(std::make_unique<function_command>(
			name ? name : "Node graph", [apply, after]() { apply(after); },
			[apply, before]() { apply(before); }));
	}

	bool node_graph_view::move_changed() const
	{
		for (const node_node& start : gesture_start_nodes_)
		{
			const node_node* current = find_node(start.id);
			if (!current)
				return true;
			if (std::fabs(current->x - start.x) > kEpsilon ||
				std::fabs(current->y - start.y) > kEpsilon)
				return true;
		}
		return false;
	}

	void node_graph_view::update_move(float content_x, float content_y)
	{
		if (gesture_start_nodes_.empty())
			return;
		const node_graph_point pointer = screen_to_graph(content_x, content_y);
		const node_node* anchor = nullptr;
		for (const node_node& node : gesture_start_nodes_)
		{
			if (node.id == primary_)
			{
				anchor = &node;
				break;
			}
		}
		if (!anchor)
			anchor = &gesture_start_nodes_.front();
		double delta_x = pointer.x - press_graph_.x;
		double delta_y = pointer.y - press_graph_.y;
		if (snap_to_grid_)
		{
			const node_graph_point target = snap_point(
				{ anchor->x + delta_x, anchor->y + delta_y });
			delta_x = target.x - anchor->x;
			delta_y = target.y - anchor->y;
		}

		bool changed = false;
		for (const node_node& start : gesture_start_nodes_)
		{
			node_node* node = find_node(start.id);
			if (!node || node->locked)
				continue;
			const double x = start.x + delta_x;
			const double y = start.y + delta_y;
			if (std::fabs(node->x - x) <= kEpsilon &&
				std::fabs(node->y - y) <= kEpsilon)
				continue;
			node->x = x;
			node->y = y;
			changed = true;
		}
		if (changed)
			notify_change();
	}

	void node_graph_view::finish_move()
	{
		if (move_changed())
			push_command("Move nodes", gesture_snapshot_);
		reset_gesture();
	}

	void node_graph_view::cancel_gesture_impl()
	{
		if (gesture_ == node_graph_gesture::move ||
			gesture_ == node_graph_gesture::pending_move)
		{
			bool changed = false;
			for (const node_node& start : gesture_start_nodes_)
			{
				node_node* node = find_node(start.id);
				if (!node)
					continue;
				if (std::fabs(node->x - start.x) <= kEpsilon &&
					std::fabs(node->y - start.y) <= kEpsilon)
					continue;
				node->x = start.x;
				node->y = start.y;
				changed = true;
			}
			if (changed)
				notify_change();
		}
		else if (gesture_ == node_graph_gesture::pan ||
			gesture_ == node_graph_gesture::minimap)
		{
			if (std::fabs(gesture_view_.offset_x - view_.offset_x) >
				kEpsilon ||
				std::fabs(gesture_view_.offset_y - view_.offset_y) >
				kEpsilon ||
				std::fabs(gesture_view_.zoom - view_.zoom) > kEpsilon)
			{
				view_ = gesture_view_;
				notify_change();
			}
		}
		reset_gesture();
	}

	void node_graph_view::cancel_gesture()
	{
		cancel_gesture_impl();
	}

	void node_graph_view::reset_gesture()
	{
		gesture_ = node_graph_gesture::none;
		gesture_start_nodes_.clear();
		gesture_snapshot_ = snapshot{};
		gesture_view_ = {};
		connect_from_port_ = invalid_object;
		connect_to_port_ = invalid_object;
	}

	void node_graph_view::set_enabled(bool enabled)
	{
		if (enabled_ == enabled)
			return;
		if (!enabled)
			cancel_gesture();
		enabled_ = enabled;
		notify_change();
	}

	bool node_graph_view::enabled() const noexcept
	{
		return enabled_;
	}

	void node_graph_view::request_focus()
	{
		if (context_ && widget_)
			vgui_set_focus(context_, widget_);
	}

	bool node_graph_view::focused() const
	{
		return context_ && widget_ && vgui_get_focus(context_) == widget_;
	}

	node_graph_state node_graph_view::capture_state() const
	{
		node_graph_state state;
		state.view = view_;
		state.grid_visible = grid_visible_;
		state.minimap_visible = minimap_visible_;
		state.snap_to_grid = snap_to_grid_;
		state.grid_step = grid_step_;
		state.primary = primary_;
		state.selection = selection_;
		return state;
	}

	bool node_graph_view::apply_state(const node_graph_state& state,
		std::string* error)
	{
		std::string reason;
		if (!validate_node_graph_state(state, &reason))
		{
			if (error)
				*error = reason;
			return false;
		}
		if (gesture_ != node_graph_gesture::none)
			cancel_gesture();

		std::vector<object_id> selection;
		for (object_id id : state.selection)
		{
			if (find_node(id) == nullptr && find_connection(id) == nullptr)
				continue;
			if (std::find(selection.begin(), selection.end(), id) !=
				selection.end())
				continue;
			selection.push_back(id);
		}
		object_id primary = state.primary;
		if (primary == invalid_object ||
			std::find(selection.begin(), selection.end(), primary) ==
				selection.end())
			primary = selection.empty() ? invalid_object : selection.front();

		view_.offset_x = state.view.offset_x;
		view_.offset_y = state.view.offset_y;
		view_.zoom = std::clamp(state.view.zoom, min_zoom_, max_zoom_);
		grid_visible_ = state.grid_visible;
		minimap_visible_ = state.minimap_visible;
		snap_to_grid_ = state.snap_to_grid;
		grid_step_ = std::clamp(state.grid_step, 1.0, 4096.0);
		selection_ = std::move(selection);
		primary_ = primary;
		notify_selection();
		notify_change();
		return true;
	}

	/* --------------------------------------------------------------------- */
	/* Input                                                                 */
	/* --------------------------------------------------------------------- */

	int node_graph_view::on_event(vgui_widget* widget, const vgui_event* event,
		void* user)
	{
		auto* self = static_cast<node_graph_view*>(user);
		if (!self || !event || !widget || !self->enabled_)
			return 0;

		float ox = 0.0f, oy = 0.0f, cw = 0.0f, ch = 0.0f;
		self->content_geometry(&ox, &oy, &cw, &ch);
		const float x = event->x - ox;
		const float y = event->y - oy;
		const bool inside = x >= 0.0f && y >= 0.0f &&
			x < cw && y < ch;
		const int mods = self->context_
			? vgui_context_get_mods(self->context_) : event->mods;

		switch (event->type)
		{
		case VGUI_EVENT_MOUSE_DOWN:
		{
			if (self->gesture_ != node_graph_gesture::none)
				return 0;

			if (event->button == 2 && inside)
			{
				self->gesture_ = node_graph_gesture::pan;
				self->gesture_view_ = self->view_;
				self->press_x_ = x;
				self->press_y_ = y;
				self->request_focus();
				return 1;
			}

			if (event->button == 3 && inside)
			{
				const object_id hit = self->hit_test(x, y);
				if (hit != invalid_object)
				{
					if (!self->is_selected(hit))
						self->set_selection({ hit }, hit);
				}
				else if (!self->selection_.empty())
				{
					self->clear_selection();
				}
				self->request_focus();
				self->open_context_menu(x, y);
				return 1;
			}

			if (event->button != 1 || !inside)
				return 0;
			self->request_focus();

			float mx = 0.0f, my = 0.0f, mw = 0.0f, mh = 0.0f;
			if (self->minimap_rect(&mx, &my, &mw, &mh) &&
				x >= mx && x <= mx + mw && y >= my && y <= my + mh)
			{
				self->gesture_ = node_graph_gesture::minimap;
				self->gesture_view_ = self->view_;
				self->press_x_ = x;
				self->press_y_ = y;
				self->navigate_minimap(x, y);
				return 1;
			}

			const object_id port = self->port_at(x, y);
			if (port != invalid_object)
			{
				self->gesture_ = node_graph_gesture::connect;
				self->connect_from_port_ = port;
				self->connect_to_port_ = invalid_object;
				self->press_x_ = x;
				self->press_y_ = y;
				self->marquee_x_ = x;
				self->marquee_y_ = y;
				self->press_graph_ = self->screen_to_graph(x, y);
				return 1;
			}

			const object_id node = self->node_at(x, y);
			if (node != invalid_object)
			{
				const bool ctrl = self->multi_select_ &&
					(mods & VGUI_MOD_CTRL) != 0;
				const bool shift = self->multi_select_ &&
					(mods & VGUI_MOD_SHIFT) != 0;
				std::vector<object_id> next = self->selection_;
				object_id primary = self->primary_;
				if (ctrl)
				{
					auto found = std::find(next.begin(), next.end(), node);
					if (found != next.end())
						next.erase(found);
					else
						next.push_back(node);
					if (std::find(next.begin(), next.end(), primary) ==
						next.end())
						primary = next.empty() ? invalid_object
							: next.front();
				}
				else if (shift)
				{
					if (std::find(next.begin(), next.end(), node) ==
						next.end())
						next.push_back(node);
					primary = node;
				}
				else if (!self->is_selected(node))
				{
					next = { node };
					primary = node;
				}
				else
				{
					primary = node;
				}
				self->set_selection(next, primary);
				if (self->is_selected(node))
				{
					self->gesture_start_nodes_.clear();
					for (object_id id : self->selection_)
					{
						const node_node* selected = self->find_node(id);
						if (selected && !selected->locked)
							self->gesture_start_nodes_.push_back(*selected);
					}
					if (!self->gesture_start_nodes_.empty())
					{
						self->gesture_ = node_graph_gesture::pending_move;
						self->gesture_snapshot_ = self->capture_snapshot();
						self->press_x_ = x;
						self->press_y_ = y;
						self->press_graph_ = self->screen_to_graph(x, y);
					}
				}
				return 1;
			}

			const object_id connection = self->connection_at(x, y);
			if (connection != invalid_object)
			{
				const bool ctrl = self->multi_select_ &&
					(mods & VGUI_MOD_CTRL) != 0;
				const bool shift = self->multi_select_ &&
					(mods & VGUI_MOD_SHIFT) != 0;
				std::vector<object_id> next = self->selection_;
				object_id primary = self->primary_;
				if (ctrl)
				{
					auto found = std::find(next.begin(), next.end(),
						connection);
					if (found != next.end())
						next.erase(found);
					else
						next.push_back(connection);
					if (std::find(next.begin(), next.end(), primary) ==
						next.end())
						primary = next.empty() ? invalid_object
							: next.front();
				}
				else if (shift)
				{
					if (std::find(next.begin(), next.end(), connection) ==
						next.end())
						next.push_back(connection);
					primary = connection;
				}
				else
				{
					next = { connection };
					primary = connection;
				}
				self->set_selection(next, primary);
				return 1;
			}

			self->gesture_ = node_graph_gesture::pending_marquee;
			self->press_x_ = x;
			self->press_y_ = y;
			self->marquee_x_ = x;
			self->marquee_y_ = y;
			return 1;
		}

		case VGUI_EVENT_MOUSE_MOVE:
			if (self->gesture_ == node_graph_gesture::pan)
			{
				const double zoom = self->effective_zoom();
				self->view_.offset_x = self->gesture_view_.offset_x -
					(double)(x - self->press_x_) / zoom;
				self->view_.offset_y = self->gesture_view_.offset_y -
					(double)(y - self->press_y_) / zoom;
				self->notify_change();
				return 1;
			}
			if (self->gesture_ == node_graph_gesture::minimap)
			{
				self->navigate_minimap(x, y);
				return 1;
			}
			if (self->gesture_ == node_graph_gesture::connect)
			{
				self->marquee_x_ = x;
				self->marquee_y_ = y;
				const object_id target = self->port_at(x, y);
				if (target != self->connect_to_port_)
				{
					self->connect_to_port_ = target;
					self->notify_change();
				}
				return 1;
			}
			if (self->gesture_ == node_graph_gesture::pending_move ||
				self->gesture_ == node_graph_gesture::pending_marquee)
			{
				const float threshold = self->effective_drag_threshold();
				if (std::fabs(x - self->press_x_) > threshold ||
					std::fabs(y - self->press_y_) > threshold)
				{
					self->gesture_ =
						self->gesture_ == node_graph_gesture::pending_move
							? node_graph_gesture::move
							: node_graph_gesture::marquee;
				}
				else
				{
					return 1;
				}
			}
			if (self->gesture_ == node_graph_gesture::move)
			{
				self->update_move(x, y);
				return 1;
			}
			if (self->gesture_ == node_graph_gesture::marquee)
			{
				self->marquee_x_ = x;
				self->marquee_y_ = y;
				return 1;
			}
			return 0;

		case VGUI_EVENT_MOUSE_UP:
			if (event->button == 2 &&
				self->gesture_ == node_graph_gesture::pan)
			{
				self->reset_gesture();
				return 1;
			}
			if (event->button != 1)
				return 0;
			if (self->gesture_ == node_graph_gesture::pending_move)
			{
				self->reset_gesture();
				return 1;
			}
			if (self->gesture_ == node_graph_gesture::move)
			{
				self->update_move(x, y);
				self->finish_move();
				return 1;
			}
			if (self->gesture_ == node_graph_gesture::pending_marquee)
			{
				self->apply_selection({}, mods);
				self->reset_gesture();
				return 1;
			}
			if (self->gesture_ == node_graph_gesture::marquee)
			{
				const node_graph_point start = self->screen_to_graph(
					self->press_x_, self->press_y_);
				const node_graph_point end = self->screen_to_graph(x, y);
				const std::vector<object_id> picked =
					self->entities_in_rect(start.x, start.y, end.x, end.y);
				self->apply_selection(picked, mods);
				self->reset_gesture();
				return 1;
			}
			if (self->gesture_ == node_graph_gesture::connect)
			{
				const object_id target = self->port_at(x, y);
				const object_id source = self->connect_from_port_;
				self->reset_gesture();
				if (target != invalid_object && target != source)
					self->connect_ports(source, target);
				return 1;
			}
			if (self->gesture_ == node_graph_gesture::minimap)
			{
				self->navigate_minimap(x, y);
				self->reset_gesture();
				return 1;
			}
			return 0;

		case VGUI_EVENT_MOUSE_WHEEL:
			if (!inside || self->gesture_ != node_graph_gesture::none)
				return 0;
			if ((mods & VGUI_MOD_SHIFT) != 0 &&
				(event->wheel_y != 0.0f || event->wheel_x != 0.0f))
			{
				self->pan_by_pixels(event->wheel_x * 40.0f,
					event->wheel_y * 40.0f);
				return 1;
			}
			if (event->wheel_y != 0.0f)
			{
				self->zoom_at(x, y, std::exp(event->wheel_y * 0.15));
				return 1;
			}
			if (event->wheel_x != 0.0f)
			{
				self->pan_by_pixels(event->wheel_x * 40.0f, 0.0f);
				return 1;
			}
			return 0;

		case VGUI_EVENT_KEY_DOWN:
		{
			const int key_mods = event->mods;
			const bool ctrl = (key_mods & VGUI_MOD_CTRL) != 0;
			const int key = event->key;
			if (key == VGUI_KEY_DELETE || key == VGUI_KEY_BACKSPACE)
				return self->delete_selection() ? 1 : 0;
			if (ctrl && (key == 'A' || key == 'a'))
			{
				self->select_all();
				return 1;
			}
			if (ctrl && (key == 'D' || key == 'd'))
				return self->duplicate_selection() ? 1 : 0;
			if (ctrl && (key == 'F' || key == 'f'))
			{
				self->focus_search();
				return 1;
			}
			if (!ctrl && (key == 'F' || key == 'f'))
			{
				self->frame_selection();
				return 1;
			}
			if (key == VGUI_KEY_HOME)
			{
				self->fit_all();
				return 1;
			}
			if (key == VGUI_KEY_ESCAPE)
			{
				if (self->gesture_ != node_graph_gesture::none)
				{
					self->cancel_gesture();
					return 1;
				}
				if (!self->search_text_.empty())
				{
					self->clear_search();
					return 1;
				}
				if (!self->selection_.empty())
				{
					self->clear_selection();
					return 1;
				}
			}
			return 0;
		}

		case VGUI_EVENT_CANCEL:
			if (self->gesture_ == node_graph_gesture::none)
				return 0;
			self->cancel_gesture();
			return 1;

		case VGUI_EVENT_LEAVE:
			return 0;

		default:
			return 0;
		}
	}

	void node_graph_view::on_focus_change(vgui_context* ctx,
		vgui_widget* old_focus, vgui_widget* new_focus, void* user)
	{
		(void)ctx;
		auto* self = static_cast<node_graph_view*>(user);
		if (!self || !self->widget_)
			return;
		const bool was = old_focus == self->widget_;
		const bool now = new_focus == self->widget_;
		if (was == now)
			return;
		if (was && self->gesture_ != node_graph_gesture::none)
			self->cancel_gesture();
	}

	/* --------------------------------------------------------------------- */
	/* Drawing                                                               */
	/* --------------------------------------------------------------------- */

	void node_graph_view::on_draw(vgui_widget* widget, ovg_ctx_cb* cb,
		rvg_t* vg, float abs_x, float abs_y, float abs_w, float abs_h,
		float delta_time, void* user)
	{
		auto* self = static_cast<node_graph_view*>(user);
		if (!self || !widget || !cb || !vg)
			return;
		self->draw(cb, vg, widget, abs_x + kBorderInset,
			abs_y + kBorderInset, abs_w - kBorderInset * 2.0f,
			abs_h - kBorderInset * 2.0f, delta_time);
	}

	void node_graph_view::draw(ovg_ctx_cb* cb, rvg_t* vg,
		vgui_widget* widget, float x, float y, float width, float height,
		float delta_time)
	{
		if (width <= 0.0f || height <= 0.0f)
			return;

		vgui_theme theme = {};
		if (context_)
			vgui_context_get_theme(context_, &theme);
		const vgui_color panel_bg = theme_color(theme.panel_bg,
			0xFF101419u);
		const vgui_color text = theme_color(theme.text, 0xFFE8EEF8u);
		const vgui_color accent_hover = theme_color(theme.accent_hover,
			0xFF74B6FFu);

		ovg_tools::fill_rect(cb, vg, x, y, width, height,
			to_ovg(panel_bg));

		draw_grid(cb, vg, x, y, width, height);
		draw_connections(cb, vg, widget, x, y, width, height);
		draw_nodes(cb, vg, widget, x, y, width, height);
		draw_pending_connection(cb, vg, x, y);

		if (gesture_ == node_graph_gesture::marquee)
		{
			marquee_offset_ += std::max(0.0f, delta_time) * 24.0f;
			ovg_tools::marquee(cb, vg, x + press_x_, y + press_y_,
				x + marquee_x_, y + marquee_y_,
				to_ovg(accent_hover), 1.0f, marquee_offset_);
		}

		draw_minimap(cb, vg, x, y, width, height);

		if (nodes_.empty())
		{
			vgui_custom_draw_text(widget, cb, vg,
				"No nodes - right-click to add one", x, y, width, height,
				with_alpha(text, 0x90), 0.5f, 0.5f);
		}
	}

	void node_graph_view::draw_grid(ovg_ctx_cb* cb, rvg_t* vg, float x,
		float y, float width, float height) const
	{
		if (!grid_visible_ || width <= 0.0f || height <= 0.0f)
			return;
		vgui_theme theme = {};
		if (context_)
			vgui_context_get_theme(context_, &theme);
		const vgui_color panel_border = theme_color(theme.panel_border,
			0xFF2A3038u);
		const vgui_color accent = theme_color(theme.accent, 0xFF409EFFu);
		const uint32_t minor = to_ovg(with_alpha(panel_border, 0x24));
		const uint32_t major = to_ovg(with_alpha(panel_border, 0x48));
		const double step = effective_grid_step();
		if (!(step > 0.0) || !finite(step))
			return;
		const double zoom = effective_zoom();
		const double left = view_.offset_x;
		const double top = view_.offset_y;
		const double right = view_.offset_x + (double)width / zoom;
		const double bottom = view_.offset_y + (double)height / zoom;

		double gx = std::floor(left / step) * step;
		long long index = (long long)std::llround(gx / step);
		for (int guard = 0; guard < kMaxGridLines && gx <= right + step;
			++guard, gx += step, ++index)
		{
			const float px = x + (float)((gx - view_.offset_x) * zoom);
			if (px < x - 1.0f || px > x + width + 1.0f)
				continue;
			ovg_tools::line(cb, vg, px, y, px, y + height,
				((index % 5) + 5) % 5 == 0 ? major : minor, 1.0f,
				VG_LINE_CAP_BUTT);
		}

		double gy = std::floor(top / step) * step;
		index = (long long)std::llround(gy / step);
		for (int guard = 0; guard < kMaxGridLines && gy <= bottom + step;
			++guard, gy += step, ++index)
		{
			const float py = y + (float)((gy - view_.offset_y) * zoom);
			if (py < y - 1.0f || py > y + height + 1.0f)
				continue;
			ovg_tools::line(cb, vg, x, py, x + width, py,
				((index % 5) + 5) % 5 == 0 ? major : minor, 1.0f,
				VG_LINE_CAP_BUTT);
		}

		const node_graph_point origin = graph_to_screen(0.0, 0.0);
		const uint32_t axis = to_ovg(with_alpha(accent, 0x50));
		if (origin.x >= 0.0 && origin.x <= width)
			ovg_tools::line(cb, vg, x + (float)origin.x, y,
				x + (float)origin.x, y + height, axis, 1.0f,
				VG_LINE_CAP_BUTT);
		if (origin.y >= 0.0 && origin.y <= height)
			ovg_tools::line(cb, vg, x, y + (float)origin.y, x + width,
				y + (float)origin.y, axis, 1.0f, VG_LINE_CAP_BUTT);
	}

	void node_graph_view::draw_connections(ovg_ctx_cb* cb, rvg_t* vg,
		vgui_widget* widget, float x, float y, float width,
		float height) const
	{
		(void)widget;
		vgui_theme theme = {};
		if (context_)
			vgui_context_get_theme(context_, &theme);
		const vgui_color accent_hover = theme_color(theme.accent_hover,
			0xFF74B6FFu);
		const float scale = ui_scale();

		for (const node_connection& connection : connections_)
		{
			if (!connection.visible)
				continue;
			const node_node* from_node = find_node(connection.from_node);
			const node_node* to_node = find_node(connection.to_node);
			if (!from_node || !to_node || !from_node->visible ||
				!to_node->visible)
				continue;
			node_graph_point from;
			node_graph_point to;
			connection_points(connection, &from, &to);
			const node_graph_point from_screen = graph_to_screen(from.x,
				from.y);
			const node_graph_point to_screen = graph_to_screen(to.x,
				to.y);
			node_graph_point c1_graph;
			node_graph_point c2_graph;
			bezier_controls(from, to, &c1_graph, &c2_graph);
			const node_graph_point c1 = graph_to_screen(c1_graph.x,
				c1_graph.y);
			const node_graph_point c2 = graph_to_screen(c2_graph.x,
				c2_graph.y);
			const bool selected = is_selected(connection.id);
			vgui_color color = connection.color;
			if (!color)
			{
				object_id node_id = invalid_object;
				node_port_direction direction = node_port_direction::input;
				const node_port* port = find_port(connection.from_port,
					&node_id, &direction);
				color = port ? port_type_color(port->type)
					: theme_color(theme.accent, 0xFF409EFFu);
			}
			if (selected)
				color = accent_hover;
			cubic_v_t curve{};
			curve.p0 = { x + (float)from_screen.x,
				y + (float)from_screen.y };
			curve.p1 = { x + (float)c1.x, y + (float)c1.y };
			curve.p2 = { x + (float)c2.x, y + (float)c2.y };
			curve.p3 = { x + (float)to_screen.x,
				y + (float)to_screen.y };
			if (selected)
				ovg_tools::bezier3(cb, vg, curve,
					to_ovg(with_alpha(accent_hover, 0x50)), 5.0f * scale);
			ovg_tools::bezier3(cb, vg, curve,
				to_ovg(with_alpha(color, selected ? 0xFF : 0xCC)),
				(selected ? 2.5f : 1.8f) * scale);
		}
		(void)width;
		(void)height;
	}

	void node_graph_view::draw_nodes(ovg_ctx_cb* cb, rvg_t* vg,
		vgui_widget* widget, float x, float y, float width,
		float height) const
	{
		vgui_theme theme = {};
		if (context_)
			vgui_context_get_theme(context_, &theme);
		const vgui_color text = theme_color(theme.text, 0xFFE8EEF8u);
		const vgui_color accent = theme_color(theme.accent, 0xFF409EFFu);
		const vgui_color accent_hover = theme_color(theme.accent_hover,
			0xFF74B6FFu);
		const vgui_color body_color = theme_color(theme.button_bg,
			0xFF1B222Cu);
		const vgui_color panel_border = theme_color(theme.panel_border,
			0xFF2A3038u);
		const float scale = ui_scale();
		const double zoom = effective_zoom();

		for (const node_node& node : nodes_)
		{
			if (!node.visible)
				continue;
			const node_graph_rect rect = node_rect(node.id);
			const node_graph_point top_left = graph_to_screen(rect.x, rect.y);
			const float nx = x + (float)top_left.x;
			const float ny = y + (float)top_left.y;
			const float nw = (float)(rect.width * zoom);
			const float nh = (float)(rect.height * zoom);
			if (nx + nw < x - 8.0f * scale ||
				nx > x + width + 8.0f * scale ||
				ny + nh < y - 8.0f * scale ||
				ny > y + height + 8.0f * scale)
				continue;

			const bool searched = !search_text_.empty() &&
				!node_matches_search(node.id);
			const uint32_t dim = searched ? 0x55 : 0xFF;
			const vgui_color node_accent = node.accent
				? node.accent : accent;
			const float radius = (float)std::clamp(6.0 * zoom,
				2.0 * scale, 8.0 * scale);
			const float header_height = node.collapsed
				? nh : (float)(kNodeHeaderHeight * zoom);

			ovg_tools::fill_round_rect(cb, vg, nx, ny, nw, nh, radius,
				to_ovg(with_alpha(body_color, dim)));
			if (header_height > 0.5f)
			{
				const float radii[4] = { radius, radius, 0.0f, 0.0f };
				ovg_tools::fill_round_rect4(cb, vg, nx, ny, nw,
					std::min(header_height, nh), radii,
					to_ovg(with_alpha(node_accent, searched ? 0x40 : 0xB0)));
			}
			ovg_tools::stroke_round_rect(cb, vg, nx, ny, nw, nh, radius,
				to_ovg(with_alpha(is_selected(node.id) ? accent_hover
					: panel_border, is_selected(node.id) ? 0xFF : 0xAA)),
				(is_selected(node.id) ? 2.0f : 1.0f) * scale);

			if (header_height >= 11.0f * scale && nw > 36.0f * scale)
			{
				vgui_custom_draw_text(widget, cb, vg,
					node.title.c_str(), nx + 8.0f * scale, ny,
					std::max(1.0f, nw - 16.0f * scale), header_height,
					with_alpha(text, dim), 0.0f, 0.5f);
				if (node.locked)
					vgui_custom_draw_text(widget, cb, vg, "LOCK",
						nx, ny, std::max(1.0f, nw - 6.0f * scale),
						header_height,
						with_alpha(text, searched ? 0x40 : 0x90),
						1.0f, 0.5f);
			}

			if (node.collapsed || zoom < 0.22)
				continue;

			const float row_height = (float)(kNodeRowHeight * zoom);
			const float port_radius = (float)std::clamp(kPortRadius *
				zoom, 1.6 * scale, 7.0 * scale);
			const float name_size = row_height;
			auto draw_port_row = [&](const std::vector<node_port>& ports,
				node_port_direction direction) {
				for (std::size_t i = 0; i < ports.size(); ++i)
				{
					const node_port& port = ports[i];
					const double gx = direction == node_port_direction::input
						? node.x + kPortInset
						: node.x + clamp_width(node.width) - kPortInset;
					const double gy = node.y + kNodeHeaderHeight +
						((double)i + 0.5) * kNodeRowHeight;
					const node_graph_point screen = graph_to_screen(gx, gy);
					const float px = x + (float)screen.x;
					const float py = y + (float)screen.y;
					vgui_color color = port_type_color(port.type);
					if (searched)
						color = with_alpha(color, 0x50);
					ovg_tools::fill_circle(cb, vg, px, py, port_radius,
						to_ovg(color));
					if (!port.multi &&
						direction == node_port_direction::input)
						ovg_tools::stroke_circle(cb, vg, px, py,
							port_radius + 1.5f * scale,
							to_ovg(with_alpha(panel_border, 0xC0)),
							1.0f * scale);
					if (name_size >= 7.0f * scale)
					{
						const float label_x = direction ==
							node_port_direction::input
							? nx + port_radius + 6.0f * scale
							: nx + 6.0f * scale;
						const float label_w = std::max(1.0f,
							nw - port_radius * 2.0f - 18.0f * scale);
						vgui_custom_draw_text(widget, cb, vg,
							port.name.c_str(), label_x,
							py - row_height * 0.5f, label_w,
							row_height, with_alpha(text,
								searched ? 0x50 : 0xC0),
							direction == node_port_direction::input
								? 0.0f : 1.0f, 0.5f);
					}
				}
			};
			draw_port_row(node.inputs, node_port_direction::input);
			draw_port_row(node.outputs, node_port_direction::output);
		}
	}

	void node_graph_view::draw_pending_connection(ovg_ctx_cb* cb,
		rvg_t* vg, float x, float y) const
	{
		if (gesture_ != node_graph_gesture::connect)
			return;
		node_graph_point from;
		if (!port_position(connect_from_port_, &from))
			return;
		node_graph_point to = screen_to_graph(marquee_x_, marquee_y_);
		if (connect_to_port_ != invalid_object)
		{
			node_graph_point target;
			if (port_position(connect_to_port_, &target))
				to = target;
		}
		const node_graph_point from_screen = graph_to_screen(from.x, from.y);
		const node_graph_point to_screen = graph_to_screen(to.x, to.y);
		node_graph_point c1_graph;
		node_graph_point c2_graph;
		bezier_controls(from, to, &c1_graph, &c2_graph);
		const node_graph_point c1 = graph_to_screen(c1_graph.x,
			c1_graph.y);
		const node_graph_point c2 = graph_to_screen(c2_graph.x,
			c2_graph.y);
		vgui_theme theme = {};
		if (context_)
			vgui_context_get_theme(context_, &theme);
		const vgui_color accent_hover = theme_color(theme.accent_hover,
			0xFF74B6FFu);
		cubic_v_t curve{};
		curve.p0 = { x + (float)from_screen.x,
			y + (float)from_screen.y };
		curve.p1 = { x + (float)c1.x, y + (float)c1.y };
		curve.p2 = { x + (float)c2.x, y + (float)c2.y };
		curve.p3 = { x + (float)to_screen.x, y + (float)to_screen.y };
		const float scale = ui_scale();
		ovg_tools::bezier3(cb, vg, curve,
			to_ovg(with_alpha(accent_hover, 0xCC)), 2.0f * scale);
		ovg_tools::fill_circle(cb, vg, curve.p3.x, curve.p3.y,
			3.0f * scale, to_ovg(accent_hover));
	}

	void node_graph_view::draw_minimap(ovg_ctx_cb* cb, rvg_t* vg, float x,
		float y, float width, float height) const
	{
		(void)width;
		(void)height;
		const node_graph_rect rect = minimap_content_rect();
		if (rect.empty())
			return;
		vgui_theme theme = {};
		if (context_)
			vgui_context_get_theme(context_, &theme);
		const vgui_color panel_bg = theme_color(theme.panel_bg,
			0xFF101419u);
		const vgui_color panel_border = theme_color(theme.panel_border,
			0xFF2A3038u);
		const vgui_color accent = theme_color(theme.accent, 0xFF409EFFu);
		const vgui_color accent_hover = theme_color(theme.accent_hover,
			0xFF74B6FFu);

		const float mx = x + (float)rect.x;
		const float my = y + (float)rect.y;
		const float mw = (float)rect.width;
		const float mh = (float)rect.height;
		const float scale = ui_scale();
		ovg_tools::fill_round_rect(cb, vg, mx, my, mw, mh, 4.0f * scale,
			to_ovg(with_alpha(panel_bg, 0xE8)));
		ovg_tools::stroke_round_rect(cb, vg, mx, my, mw, mh,
			4.0f * scale, to_ovg(with_alpha(panel_border, 0xE0)),
			1.0f * scale);

		const node_graph_rect bounds = minimap_graph_bounds();
		const double inset = kMinimapInset * (double)scale;
		const double inner_x = mx + inset;
		const double inner_y = my + inset;
		const double inner_w = std::max(1.0, (double)mw - inset * 2.0);
		const double inner_h = std::max(1.0, (double)mh - inset * 2.0);
		auto map = [&](double gx, double gy) {
			return node_graph_point{
				inner_x + (gx - bounds.x) / bounds.width * inner_w,
				inner_y + (gy - bounds.y) / bounds.height * inner_h };
		};

		for (const node_connection& connection : connections_)
		{
			if (!connection.visible)
				continue;
			const node_node* from_node = find_node(connection.from_node);
			const node_node* to_node = find_node(connection.to_node);
			if (!from_node || !to_node || !from_node->visible ||
				!to_node->visible)
				continue;
			node_graph_point from;
			node_graph_point to;
			connection_points(connection, &from, &to);
			const node_graph_point p0 = map(from.x, from.y);
			const node_graph_point p1 = map(to.x, to.y);
			ovg_tools::line(cb, vg, (float)p0.x, (float)p0.y,
				(float)p1.x, (float)p1.y,
				to_ovg(with_alpha(accent, 0x38)), 1.0f * scale,
				VG_LINE_CAP_BUTT);
		}

		for (const node_node& node : nodes_)
		{
			if (!node.visible)
				continue;
			const node_graph_rect node_bounds = node_rect(node.id);
			const node_graph_point p0 = map(node_bounds.x, node_bounds.y);
			const node_graph_point p1 = map(node_bounds.right(),
				node_bounds.bottom());
			const bool selected = is_selected(node.id);
			ovg_tools::fill_rect(cb, vg, (float)p0.x, (float)p0.y,
				std::max(1.0f, (float)(p1.x - p0.x)),
				std::max(1.0f, (float)(p1.y - p0.y)),
				to_ovg(with_alpha(selected ? accent_hover : accent,
					selected ? 0xE0 : 0x90)));
		}

		float cw = 0.0f, ch = 0.0f;
		if (!content_geometry(nullptr, nullptr, &cw, &ch))
			return;
		const double zoom = effective_zoom();
		const node_graph_point view0 = map(view_.offset_x, view_.offset_y);
		const node_graph_point viewport1 = map(view_.offset_x +
			(double)cw / zoom, view_.offset_y + (double)ch / zoom);
		ovg_tools::stroke_rect(cb, vg, (float)view0.x, (float)view0.y,
			std::max(1.0f, (float)(viewport1.x - view0.x)),
			std::max(1.0f, (float)(viewport1.y - view0.y)),
			to_ovg(accent_hover), 1.0f * scale);
	}

} /* namespace vgui_editor_kit */
