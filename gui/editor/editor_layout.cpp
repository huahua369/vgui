#include "editor_layout.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <stdexcept>
#include <utility>

#include <nlohmann/json.hpp>

namespace vgui_editor_kit {

	namespace {
		using json = nlohmann::json;

		void set_error(std::string* error, const std::string& text)
		{
			if (error)
				*error = text;
		}

		const json& require_object(const json& parent, const char* key)
		{
			auto it = parent.find(key);
			if (it == parent.end())
				throw std::runtime_error(std::string("missing object: ") + key);
			if (!it->is_object())
				throw std::runtime_error(std::string("expected object: ") + key);
			return *it;
		}

		float read_ratio(const json& object, const char* key, float fallback)
		{
			auto it = object.find(key);
			if (it == object.end())
				return fallback;
			if (!it->is_number())
				throw std::runtime_error(std::string("expected number: ") + key);
			float value = it->get<float>();
			if (!std::isfinite(value))
				throw std::runtime_error(std::string("non-finite ratio: ") + key);
			return std::clamp(value, 0.0f, 1.0f);
		}

		float read_number(const json& object, const char* key, float fallback)
		{
			auto it = object.find(key);
			if (it == object.end())
				return fallback;
			if (!it->is_number())
				throw std::runtime_error(std::string("expected number: ") + key);
			float value = it->get<float>();
			if (!std::isfinite(value))
				throw std::runtime_error(std::string("non-finite number: ") + key);
			return value;
		}

		bool read_bool(const json& object, const char* key, bool fallback)
		{
			auto it = object.find(key);
			if (it == object.end())
				return fallback;
			if (!it->is_boolean())
				throw std::runtime_error(std::string("expected boolean: ") + key);
			return it->get<bool>();
		}

		std::string read_string(const json& object, const char* key,
			const std::string& fallback = {})
		{
			auto it = object.find(key);
			if (it == object.end())
				return fallback;
			if (!it->is_string())
				throw std::runtime_error(std::string("expected string: ") + key);
			return it->get<std::string>();
		}

		const char* dock_zone_name(vgui_dock_zone zone)
		{
			switch (zone)
			{
			case VGUI_DOCK_LEFT: return "left";
			case VGUI_DOCK_RIGHT: return "right";
			case VGUI_DOCK_BOTTOM: return "bottom";
			case VGUI_DOCK_CENTER: return "center";
			case VGUI_DOCK_FLOATING: return "floating";
			}
			return "center";
		}

		vgui_dock_zone read_dock_zone(const json& object)
		{
			const std::string name = read_string(object, "zone", "");
			if (name == "left")
				return VGUI_DOCK_LEFT;
			if (name == "right")
				return VGUI_DOCK_RIGHT;
			if (name == "bottom")
				return VGUI_DOCK_BOTTOM;
			if (name == "center")
				return VGUI_DOCK_CENTER;
			if (name == "floating")
				return VGUI_DOCK_FLOATING;
			throw std::runtime_error("unknown dock zone: " + name);
		}

		json dock_panel_to_json(const editor_dock_panel_layout& panel)
		{
			json entry;
			entry["id"] = panel.id;
			entry["zone"] = dock_zone_name(panel.zone);
			entry["open"] = panel.open;
			if (panel.zone == VGUI_DOCK_FLOATING && panel.has_floating_rect)
			{
				entry["x"] = panel.x;
				entry["y"] = panel.y;
				entry["width"] = panel.width;
				entry["height"] = panel.height;
			}
			return entry;
		}
	}

	std::string editor_layout_to_json(const editor_layout& layout)
	{
		json root;
		root["version"] = layout.version;
		root["splitters"] = {
			{ "body", layout.body_ratio },
			{ "workspace", layout.workspace_ratio },
			{ "center", layout.center_ratio }
		};
		root["panels"] = {
			{ "outliner", layout.outliner_visible },
			{ "viewport", layout.viewport_visible },
			{ "properties", layout.properties_visible },
			{ "bottom", layout.bottom_visible }
		};
		root["bottom_tabs"] = {
			{ "order", layout.bottom_tab_order },
			{ "active", layout.bottom_active_tab }
		};

		const bool has_dock_state = layout.version >= 2
			|| !layout.dock_panels.empty()
			|| layout.dock_left_size >= 0.0f
			|| layout.dock_right_size >= 0.0f
			|| layout.dock_bottom_size >= 0.0f;
		if (has_dock_state)
		{
			json dock;
			json sizes = json::object();
			if (layout.dock_left_size >= 0.0f)
				sizes["left"] = layout.dock_left_size;
			if (layout.dock_right_size >= 0.0f)
				sizes["right"] = layout.dock_right_size;
			if (layout.dock_bottom_size >= 0.0f)
				sizes["bottom"] = layout.dock_bottom_size;
			if (!sizes.empty())
				dock["sizes"] = std::move(sizes);

			json active = json::object();
			static const char* const zone_keys[4] = { "left", "right", "bottom", "center" };
			for (int i = 0; i < 4; ++i)
			{
				if (!layout.dock_active[(size_t)i].empty())
					active[zone_keys[i]] = layout.dock_active[(size_t)i];
			}
			if (!active.empty())
				dock["active"] = std::move(active);

			json panels = json::array();
			for (const editor_dock_panel_layout& panel : layout.dock_panels)
				panels.push_back(dock_panel_to_json(panel));
			dock["panels"] = std::move(panels);
			root["dock"] = std::move(dock);
		}

		if (!layout.table_state.empty())
			root["table"] = layout.table_state;

		if (!layout.timeline_state.empty())
			root["timeline"] = layout.timeline_state;

		if (!layout.node_graph_state.empty())
			root["node_graph"] = layout.node_graph_state;

		return root.dump(2) + "\n";
	}

	bool editor_layout_from_json(const std::string& text, editor_layout& out,
		std::string* error)
	{
		out = editor_layout{};
		try
		{
			json root = json::parse(text);
			if (!root.is_object())
				throw std::runtime_error("layout root must be an object");

			auto version = root.find("version");
			if (version == root.end() || !version->is_number_integer())
				throw std::runtime_error("layout version is missing");
			const int file_version = version->get<int>();
			if (file_version < 1 || file_version > editor_layout::current_version)
				throw std::runtime_error("unsupported layout version");

			editor_layout parsed;
			parsed.version = file_version;

			if (root.contains("splitters"))
			{
				const json& splitters = require_object(root, "splitters");
				parsed.body_ratio = read_ratio(splitters, "body", parsed.body_ratio);
				parsed.workspace_ratio = read_ratio(splitters, "workspace",
					parsed.workspace_ratio);
				parsed.center_ratio = read_ratio(splitters, "center", parsed.center_ratio);
			}

			if (root.contains("panels"))
			{
				const json& panels = require_object(root, "panels");
				parsed.outliner_visible = read_bool(panels, "outliner",
					parsed.outliner_visible);
				parsed.viewport_visible = read_bool(panels, "viewport",
					parsed.viewport_visible);
				parsed.properties_visible = read_bool(panels, "properties",
					parsed.properties_visible);
				parsed.bottom_visible = read_bool(panels, "bottom", parsed.bottom_visible);
			}

			if (root.contains("bottom_tabs"))
			{
				const json& tabs = require_object(root, "bottom_tabs");
				auto order = tabs.find("order");
				if (order != tabs.end())
				{
					if (!order->is_array())
						throw std::runtime_error("bottom_tabs.order must be an array");
					for (const auto& entry : *order)
					{
						if (!entry.is_string())
							throw std::runtime_error(
								"bottom_tabs.order entries must be strings");
						parsed.bottom_tab_order.push_back(entry.get<std::string>());
					}
				}
				parsed.bottom_active_tab = read_string(tabs, "active",
					parsed.bottom_active_tab);
			}

			if (root.contains("dock"))
			{
				const json& dock = require_object(root, "dock");

				if (dock.contains("sizes"))
				{
					const json& sizes = require_object(dock, "sizes");
					auto read_size = [&sizes](const char* key, float fallback) {
						float value = read_number(sizes, key, fallback);
						if (value < -1.0f)
							throw std::runtime_error(
								std::string("invalid dock zone size: ") + key);
						return value;
					};
					parsed.dock_left_size = read_size("left", parsed.dock_left_size);
					parsed.dock_right_size = read_size("right", parsed.dock_right_size);
					parsed.dock_bottom_size = read_size("bottom", parsed.dock_bottom_size);
				}

				if (dock.contains("active"))
				{
					const json& active = require_object(dock, "active");
					parsed.dock_active[VGUI_DOCK_LEFT] = read_string(active, "left");
					parsed.dock_active[VGUI_DOCK_RIGHT] = read_string(active, "right");
					parsed.dock_active[VGUI_DOCK_BOTTOM] = read_string(active, "bottom");
					parsed.dock_active[VGUI_DOCK_CENTER] = read_string(active, "center");
				}

				if (dock.contains("panels"))
				{
					const json& panels = dock.at("panels");
					if (!panels.is_array())
						throw std::runtime_error("dock.panels must be an array");
					std::set<std::string> ids;
					for (const auto& entry : panels)
					{
						if (!entry.is_object())
							throw std::runtime_error("dock.panels entries must be objects");

						editor_dock_panel_layout panel;
						panel.id = read_string(entry, "id", "");
						if (panel.id.empty())
							throw std::runtime_error("dock panel id is missing");
						if (!ids.insert(panel.id).second)
							throw std::runtime_error("duplicate dock panel id: " + panel.id);
						panel.zone = read_dock_zone(entry);
						panel.open = read_bool(entry, "open", true);
						if (panel.zone == VGUI_DOCK_FLOATING)
						{
							panel.x = read_number(entry, "x", 0.0f);
							panel.y = read_number(entry, "y", 0.0f);
							panel.width = read_number(entry, "width", 0.0f);
							panel.height = read_number(entry, "height", 0.0f);
							panel.has_floating_rect =
								panel.width > 0.0f && panel.height > 0.0f;
						}
						parsed.dock_panels.push_back(std::move(panel));
					}
				}
			}

			if (root.contains("table"))
			{
				parsed.table_state = read_string(root, "table");
			}

			if (root.contains("timeline"))
			{
				parsed.timeline_state = read_string(root, "timeline");
			}

			if (root.contains("node_graph"))
			{
				parsed.node_graph_state = read_string(root, "node_graph");
			}

			out = std::move(parsed);
			return true;
		}
		catch (const std::exception& e)
		{
			out = editor_layout{};
			set_error(error, e.what());
			return false;
		}
	}

	bool editor_layout_load_file(const std::string& path, editor_layout& out,
		std::string* error)
	{
		std::ifstream file(path, std::ios::binary);
		if (!file)
		{
			set_error(error, "cannot open layout file");
			out = editor_layout{};
			return false;
		}

		std::string text((std::istreambuf_iterator<char>(file)),
			std::istreambuf_iterator<char>());
		if (!file.good() && !file.eof())
		{
			set_error(error, "cannot read layout file");
			out = editor_layout{};
			return false;
		}

		return editor_layout_from_json(text, out, error);
	}

	bool editor_layout_save_file(const std::string& path, const editor_layout& layout,
		std::string* error)
	{
		if (path.empty())
		{
			set_error(error, "layout path is empty");
			return false;
		}

		std::error_code ec;
		std::filesystem::path target(path);
		std::filesystem::path parent = target.parent_path();
		if (!parent.empty())
		{
			std::filesystem::create_directories(parent, ec);
			if (ec)
			{
				set_error(error, "cannot create layout directory");
				return false;
			}
		}

		std::ofstream file(target, std::ios::binary | std::ios::trunc);
		if (!file)
		{
			set_error(error, "cannot open layout file for writing");
			return false;
		}

		file << editor_layout_to_json(layout);
		file.flush();
		if (!file)
		{
			set_error(error, "cannot write layout file");
			return false;
		}
		return true;
	}

} /* namespace vgui_editor_kit */
