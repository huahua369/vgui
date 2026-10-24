#include "editor_layout.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
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
			if (version->get<int>() != editor_layout::current_version)
				throw std::runtime_error("unsupported layout version");

			editor_layout parsed;
			parsed.version = editor_layout::current_version;

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
