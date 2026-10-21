/*
 * ui_editor.cpp - document model, JSON persistence, C++ code generation and
 * the actual editor UI (built entirely with the public vgui.h C API).
 */
#include "ui_editor.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>

#include <nlohmann/json.hpp>

namespace vgui_editor {

	using json = nlohmann::json;

	/* ------------------------------------------------------------------------- */
	/* small helpers                                                             */
	/* ------------------------------------------------------------------------- */

	const char* widget_kind_name(vgui_widget_kind kind)
	{
		switch (kind)
		{
		case VGUI_PANEL: return "panel";
		case VGUI_LABEL: return "label";
		case VGUI_BUTTON: return "button";
		case VGUI_CHECKBOX: return "checkbox";
		case VGUI_RADIO: return "radio";
		case VGUI_SWITCH: return "switch";
		case VGUI_SLIDER: return "slider";
		case VGUI_PROGRESS: return "progress";
		case VGUI_EDIT: return "edit";
		case VGUI_SEPARATOR: return "separator";
		}
		return "panel";
	}

	bool widget_kind_from_name(const std::string& name, vgui_widget_kind* kind)
	{
		static const vgui_widget_kind kinds[] = {
			VGUI_PANEL, VGUI_LABEL, VGUI_BUTTON, VGUI_CHECKBOX, VGUI_RADIO,
			VGUI_SWITCH, VGUI_SLIDER, VGUI_PROGRESS, VGUI_EDIT, VGUI_SEPARATOR
		};
		for (vgui_widget_kind k : kinds)
		{
			if (name == widget_kind_name(k))
			{
				if (kind) *kind = k;
				return true;
			}
		}
		return false;
	}

	static bool is_text_kind(vgui_widget_kind kind)
	{
		return kind == VGUI_LABEL || kind == VGUI_BUTTON || kind == VGUI_CHECKBOX ||
			kind == VGUI_RADIO || kind == VGUI_SWITCH;
	}

	static bool is_value_kind(vgui_widget_kind kind)
	{
		return kind == VGUI_CHECKBOX || kind == VGUI_RADIO || kind == VGUI_SWITCH ||
			kind == VGUI_SLIDER || kind == VGUI_PROGRESS;
	}

	static std::string trim_copy(const std::string& s)
	{
		size_t a = 0, b = s.size();
		while (a < b && std::isspace((unsigned char)s[a])) ++a;
		while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
		return s.substr(a, b - a);
	}

	static bool parse_color(const std::string& text, uint32_t* out)
	{
		std::string s = trim_copy(text);
		if (s.empty()) return false;
		if (s.size() > 2 && (s[0] == '0') && (s[1] == 'x' || s[1] == 'X')) s = s.substr(2);
		if (!s.empty() && s[0] == '#') s = s.substr(1);
		for (char c : s)
			if (!std::isxdigit((unsigned char)c)) return false;
		unsigned long long v = 0;
		try
		{
			v = std::stoull(s, nullptr, 16);
		}
		catch (...)
		{
			return false;
		}
		if (s.size() <= 6) v |= 0xFF000000ull;
		if (out) *out = (uint32_t)v;
		return true;
	}

	static std::string color_text(uint32_t c)
	{
		char buf[16] = {};
		snprintf(buf, sizeof(buf), "#%08X", c);
		return buf;
	}

	static std::string number_text(float v)
	{
		char buf[64] = {};
		snprintf(buf, sizeof(buf), "%g", (double)v);
		std::string s = buf;
		if (s == "-0") s = "0";
		return s;
	}

	static bool parse_number(const std::string& text, float* out)
	{
		std::string s = trim_copy(text);
		if (s.empty()) return false;
		char* end = nullptr;
		float v = strtof(s.c_str(), &end);
		if (end == s.c_str() || *end != '\0') return false;
		if (out) *out = v;
		return true;
	}

	static std::string escape_c_string(const std::string& s)
	{
		std::string out;
		out.reserve(s.size() + 8);
		for (char c : s)
		{
			switch (c)
			{
			case '\\': out += "\\\\"; break;
			case '"': out += "\\\""; break;
			case '\n': out += "\\n"; break;
			case '\r': out += "\\r"; break;
			case '\t': out += "\\t"; break;
			default: out += c; break;
			}
		}
		return out;
	}

	static bool write_file(const std::string& path, const std::string& data, std::string* error)
	{
		namespace fs = std::filesystem;
		std::error_code ec;
		fs::path p = fs::path(path);
		if (p.has_parent_path())
			fs::create_directories(p.parent_path(), ec);
		std::ofstream f(p, std::ios::binary | std::ios::trunc);
		if (!f)
		{
			if (error) *error = "无法写入文件: " + path;
			return false;
		}
		f.write(data.data(), (std::streamsize)data.size());
		if (!f)
		{
			if (error) *error = "写入文件失败: " + path;
			return false;
		}
		return true;
	}

	static bool read_file(const std::string& path, std::string* out, std::string* error)
	{
		std::ifstream f(std::filesystem::path(path), std::ios::binary);
		if (!f)
		{
			if (error) *error = "无法打开文件: " + path;
			return false;
		}
		std::ostringstream ss;
		ss << f.rdbuf();
		*out = ss.str();
		return true;
	}

	/* ------------------------------------------------------------------------- */
	/* default nodes                                                             */
	/* ------------------------------------------------------------------------- */

	std::unique_ptr<ui_node> make_default_node(vgui_widget_kind kind)
	{
		auto n = std::make_unique<ui_node>();
		n->kind = kind;

		switch (kind)
		{
		case VGUI_PANEL:
			n->w = 240.0f;
			n->h = 160.0f;
			n->bg = 0xFF2A2B2E;
			n->border_color = 0xFF3C3F44;
			n->border_width = 1.0f;
			n->rounding = 4.0f;
			n->padding[0] = n->padding[1] = n->padding[2] = n->padding[3] = 8.0f;
			n->layout = VGUI_LAYOUT_ABSOLUTE;
			break;
		case VGUI_LABEL:
			n->w = 140.0f;
			n->h = 24.0f;
			n->text = "标签";
			n->text_color = 0xFFE6E6E6;
			n->text_align_x = 0.0f;
			break;
		case VGUI_BUTTON:
			n->w = 120.0f;
			n->h = 32.0f;
			n->text = "按钮";
			n->bg = 0xFF3A3A3A;
			n->border_color = 0xFF555555;
			n->border_width = 1.0f;
			n->rounding = 4.0f;
			n->focusable = true;
			break;
		case VGUI_CHECKBOX:
			n->w = 140.0f;
			n->h = 26.0f;
			n->text = "复选框";
			n->text_color = 0xFFE6E6E6;
			n->checked = true;
			n->focusable = true;
			break;
		case VGUI_RADIO:
			n->w = 140.0f;
			n->h = 26.0f;
			n->text = "单选项";
			n->text_color = 0xFFE6E6E6;
			n->focusable = true;
			break;
		case VGUI_SWITCH:
			n->w = 150.0f;
			n->h = 26.0f;
			n->text = "开关";
			n->text_color = 0xFFE6E6E6;
			n->checked = true;
			n->focusable = true;
			break;
		case VGUI_SLIDER:
			n->w = 200.0f;
			n->h = 28.0f;
			n->range_min = 0.0f;
			n->range_max = 100.0f;
			n->value = 40.0f;
			n->focusable = true;
			break;
		case VGUI_PROGRESS:
			n->w = 200.0f;
			n->h = 20.0f;
			n->range_min = 0.0f;
			n->range_max = 100.0f;
			n->value = 60.0f;
			break;
		case VGUI_EDIT:
			n->w = 220.0f;
			n->h = 32.0f;
			n->edit_text = "";
			n->placeholder = "请输入";
			n->bg = 0xFF252525;
			n->border_color = 0xFF555555;
			n->border_width = 1.0f;
			n->rounding = 3.0f;
			n->focusable = true;
			break;
		case VGUI_SEPARATOR:
			n->w = 200.0f;
			n->h = 1.0f;
			break;
		}
		return n;
	}

	ui_node* ui_node::add_child(vgui_widget_kind child_kind)
	{
		auto child = make_default_node(child_kind);
		child->parent = this;
		children.push_back(std::move(child));
		return children.back().get();
	}

	void ui_node::remove_child(ui_node* child)
	{
		for (auto it = children.begin(); it != children.end(); ++it)
		{
			if (it->get() == child)
			{
				children.erase(it);
				return;
			}
		}
	}

	/* ------------------------------------------------------------------------- */
	/* document                                                                  */
	/* ------------------------------------------------------------------------- */

	ui_document::ui_document()
	{
		reset();
	}

	void ui_document::reset()
	{
		name = "untitled";
		canvas_width = 800;
		canvas_height = 600;
		root = ui_node();
		root.kind = VGUI_PANEL;
		root.id = "root";
		root.layout = VGUI_LAYOUT_ABSOLUTE;
		root.w = (float)canvas_width;
		root.h = (float)canvas_height;
		root.bg = 0xFF1E1E1E;

		ui_node* title = root.add_child(VGUI_LABEL);
		title->id = "title";
		title->text = "Hello vgui";
		title->x = 24.0f;
		title->y = 24.0f;
		title->w = 200.0f;
		title->h = 30.0f;
		title->font_size = 22.0f;

		ui_node* button = root.add_child(VGUI_BUTTON);
		button->id = "button";
		button->text = "Click me";
		button->x = 24.0f;
		button->y = 70.0f;
		button->w = 140.0f;
		button->h = 36.0f;
	}

	/* JSON --------------------------------------------------------------------- */

	static json node_to_json(const ui_node& n)
	{
		json j;
		j["kind"] = widget_kind_name(n.kind);
		j["id"] = n.id;
		j["text"] = n.text;

		j["layout"] = n.layout;
		j["x"] = n.x;
		j["y"] = n.y;
		j["w"] = n.w;
		j["h"] = n.h;
		j["min_w"] = n.min_w;
		j["min_h"] = n.min_h;
		j["max_w"] = n.max_w;
		j["max_h"] = n.max_h;
		j["margin"] = json::array({ n.margin[0], n.margin[1], n.margin[2], n.margin[3] });
		j["padding"] = json::array({ n.padding[0], n.padding[1], n.padding[2], n.padding[3] });
		j["gap"] = n.gap;
		j["grow"] = n.grow;
		j["align_items"] = n.align_items;
		j["align_self"] = n.align_self;
		j["justify"] = n.justify;

		j["bg"] = color_text(n.bg);
		j["border_color"] = color_text(n.border_color);
		j["text_color"] = color_text(n.text_color);
		j["border_width"] = n.border_width;
		j["rounding"] = n.rounding;
		j["font_size"] = n.font_size;
		j["opacity"] = n.opacity;
		j["text_align_x"] = n.text_align_x;
		j["text_align_y"] = n.text_align_y;

		j["visible"] = n.visible;
		j["enabled"] = n.enabled;
		j["focusable"] = n.focusable;

		j["value"] = n.value;
		j["range_min"] = n.range_min;
		j["range_max"] = n.range_max;
		j["step"] = n.step;
		j["checked"] = n.checked;
		j["vertical"] = n.vertical;

		j["edit_text"] = n.edit_text;
		j["placeholder"] = n.placeholder;
		j["multiline"] = n.multiline;
		j["password"] = n.password;
		j["readonly"] = n.readonly;

		j["children"] = json::array();
		for (const auto& child : n.children)
			j["children"].push_back(node_to_json(*child));
		return j;
	}

	static uint32_t json_color(const json& j, const char* key, uint32_t def)
	{
		auto it = j.find(key);
		if (it == j.end()) return def;
		if (it->is_string())
		{
			uint32_t c = def;
			if (parse_color(it->get<std::string>(), &c)) return c;
			return def;
		}
		if (it->is_number_unsigned()) return (uint32_t)it->get<uint64_t>();
		if (it->is_number_integer()) return (uint32_t)it->get<int64_t>();
		return def;
	}

	static float json_float(const json& j, const char* key, float def)
	{
		auto it = j.find(key);
		if (it == j.end() || !it->is_number()) return def;
		return it->get<float>();
	}

	static int json_int(const json& j, const char* key, int def)
	{
		auto it = j.find(key);
		if (it == j.end() || !it->is_number()) return def;
		return it->get<int>();
	}

	static bool json_bool(const json& j, const char* key, bool def)
	{
		auto it = j.find(key);
		if (it == j.end() || !it->is_boolean()) return def;
		return it->get<bool>();
	}

	static std::string json_string(const json& j, const char* key, const std::string& def)
	{
		auto it = j.find(key);
		if (it == j.end() || !it->is_string()) return def;
		return it->get<std::string>();
	}

	static void node_from_json(const json& j, ui_node& n)
	{
		std::string kind_name = json_string(j, "kind", widget_kind_name(n.kind));
		widget_kind_from_name(kind_name, &n.kind);

		n.id = json_string(j, "id", n.id);
		n.text = json_string(j, "text", n.text);
		n.layout = json_int(j, "layout", n.layout);
		n.x = json_float(j, "x", n.x);
		n.y = json_float(j, "y", n.y);
		n.w = json_float(j, "w", n.w);
		n.h = json_float(j, "h", n.h);
		n.min_w = json_float(j, "min_w", n.min_w);
		n.min_h = json_float(j, "min_h", n.min_h);
		n.max_w = json_float(j, "max_w", n.max_w);
		n.max_h = json_float(j, "max_h", n.max_h);
		n.gap = json_float(j, "gap", n.gap);
		n.grow = json_float(j, "grow", n.grow);
		n.align_items = json_int(j, "align_items", n.align_items);
		n.align_self = json_int(j, "align_self", n.align_self);
		n.justify = json_int(j, "justify", n.justify);

		auto read4 = [&](const char* key, float* dst)
			{
				auto it = j.find(key);
				if (it == j.end() || !it->is_array() || it->size() < 4) return;
				for (int i = 0; i < 4; ++i)
					if ((*it)[i].is_number()) dst[i] = (*it)[i].get<float>();
			};
		read4("margin", n.margin);
		read4("padding", n.padding);

		n.bg = json_color(j, "bg", n.bg);
		n.border_color = json_color(j, "border_color", n.border_color);
		n.text_color = json_color(j, "text_color", n.text_color);
		n.border_width = json_float(j, "border_width", n.border_width);
		n.rounding = json_float(j, "rounding", n.rounding);
		n.font_size = json_float(j, "font_size", n.font_size);
		n.opacity = json_float(j, "opacity", n.opacity);
		n.text_align_x = json_float(j, "text_align_x", n.text_align_x);
		n.text_align_y = json_float(j, "text_align_y", n.text_align_y);

		n.visible = json_bool(j, "visible", n.visible);
		n.enabled = json_bool(j, "enabled", n.enabled);
		n.focusable = json_bool(j, "focusable", n.focusable);

		n.value = json_float(j, "value", n.value);
		n.range_min = json_float(j, "range_min", n.range_min);
		n.range_max = json_float(j, "range_max", n.range_max);
		n.step = json_float(j, "step", n.step);
		n.checked = json_bool(j, "checked", n.checked);
		n.vertical = json_bool(j, "vertical", n.vertical);

		n.edit_text = json_string(j, "edit_text", n.edit_text);
		n.placeholder = json_string(j, "placeholder", n.placeholder);
		n.multiline = json_bool(j, "multiline", n.multiline);
		n.password = json_bool(j, "password", n.password);
		n.readonly = json_bool(j, "readonly", n.readonly);

		n.children.clear();
		auto it = j.find("children");
		if (it != j.end() && it->is_array())
		{
			for (const json& cj : *it)
			{
				if (!cj.is_object()) continue;
				std::string ck = json_string(cj, "kind", "panel");
				vgui_widget_kind kind = VGUI_PANEL;
				widget_kind_from_name(ck, &kind);
				auto child = make_default_node(kind);
				child->parent = &n;
				node_from_json(cj, *child);
				n.children.push_back(std::move(child));
			}
		}
	}

	static void fix_parents(ui_node& n)
	{
		for (auto& child : n.children)
		{
			child->parent = &n;
			fix_parents(*child);
		}
	}

	std::string ui_document::to_json_string(int indent) const
	{
		json j;
		j["format"] = "vgui-ui-editor";
		j["version"] = 1;
		j["name"] = name;
		j["canvas"] = { { "width", canvas_width },{ "height", canvas_height } };
		j["root"] = node_to_json(root);
		return j.dump(indent);
	}

	bool ui_document::from_json_string(const std::string& text, std::string* error)
	{
		try
		{
			json j = json::parse(text);
			if (!j.is_object())
			{
				if (error) *error = "JSON 根节点不是对象";
				return false;
			}

			ui_document tmp;
			tmp.name = json_string(j, "name", "untitled");
			auto canvas = j.find("canvas");
			if (canvas != j.end() && canvas->is_object())
			{
				tmp.canvas_width = json_int(*canvas, "width", tmp.canvas_width);
				tmp.canvas_height = json_int(*canvas, "height", tmp.canvas_height);
			}
			if (tmp.canvas_width <= 0) tmp.canvas_width = 800;
			if (tmp.canvas_height <= 0) tmp.canvas_height = 600;

			auto root_it = j.find("root");
			if (root_it == j.end() || !root_it->is_object())
			{
				if (error) *error = "缺少 root 节点";
				return false;
			}

			tmp.root = ui_node();
			tmp.root.kind = VGUI_PANEL;
			tmp.root.id = "root";
			node_from_json(*root_it, tmp.root);

			name = tmp.name;
			canvas_width = tmp.canvas_width;
			canvas_height = tmp.canvas_height;
			root = std::move(tmp.root);
			fix_parents(root);
			return true;
		}
		catch (const std::exception& ex)
		{
			if (error) *error = std::string("JSON 解析失败: ") + ex.what();
			return false;
		}
	}

	/* ------------------------------------------------------------------------- */
	/* C++ code generation                                                       */
	/* ------------------------------------------------------------------------- */

	static const char* layout_name(int v)
	{
		switch (v)
		{
		case VGUI_LAYOUT_ROW: return "VGUI_LAYOUT_ROW";
		case VGUI_LAYOUT_COLUMN: return "VGUI_LAYOUT_COLUMN";
		default: return "VGUI_LAYOUT_ABSOLUTE";
		}
	}

	static const char* align_name(int v)
	{
		switch (v)
		{
		case VGUI_ALIGN_START: return "VGUI_ALIGN_START";
		case VGUI_ALIGN_CENTER: return "VGUI_ALIGN_CENTER";
		case VGUI_ALIGN_END: return "VGUI_ALIGN_END";
		case VGUI_ALIGN_STRETCH: return "VGUI_ALIGN_STRETCH";
		default: return "VGUI_ALIGN_AUTO";
		}
	}

	static const char* justify_name(int v)
	{
		switch (v)
		{
		case VGUI_JUSTIFY_CENTER: return "VGUI_JUSTIFY_CENTER";
		case VGUI_JUSTIFY_END: return "VGUI_JUSTIFY_END";
		case VGUI_JUSTIFY_SPACE_BETWEEN: return "VGUI_JUSTIFY_SPACE_BETWEEN";
		case VGUI_JUSTIFY_SPACE_AROUND: return "VGUI_JUSTIFY_SPACE_AROUND";
		default: return "VGUI_JUSTIFY_START";
		}
	}

	static const char* kind_enum_name(vgui_widget_kind kind)
	{
		switch (kind)
		{
		case VGUI_PANEL: return "VGUI_PANEL";
		case VGUI_LABEL: return "VGUI_LABEL";
		case VGUI_BUTTON: return "VGUI_BUTTON";
		case VGUI_CHECKBOX: return "VGUI_CHECKBOX";
		case VGUI_RADIO: return "VGUI_RADIO";
		case VGUI_SWITCH: return "VGUI_SWITCH";
		case VGUI_SLIDER: return "VGUI_SLIDER";
		case VGUI_PROGRESS: return "VGUI_PROGRESS";
		case VGUI_EDIT: return "VGUI_EDIT";
		case VGUI_SEPARATOR: return "VGUI_SEPARATOR";
		}
		return "VGUI_PANEL";
	}

	static std::string color_literal(uint32_t c)
	{
		char buf[16] = {};
		snprintf(buf, sizeof(buf), "0x%08X", c);
		return buf;
	}

	static std::string sanitize_identifier(const std::string& text)
	{
		std::string out;
		for (char c : text)
		{
			if (std::isalnum((unsigned char)c) || c == '_')
				out += c;
			else
				out += '_';
		}
		if (out.empty()) out = "w";
		if (std::isdigit((unsigned char)out[0])) out = "w_" + out;
		return out;
	}

	namespace {

		struct codegen_state
		{
			const ui_export_options* opt = nullptr;
			std::ostringstream out;
			std::map<const ui_node*, std::string> names;
			std::set<std::string> used;

			std::string unique_name(const std::string& base)
			{
				std::string name = base;
				int suffix = 2;
				while (used.count(name))
				{
					name = base + "_" + std::to_string(suffix++);
				}
				used.insert(name);
				return name;
			}

			void collect_names(const ui_node& n, bool is_root, int index)
			{
				std::string base;
				if (is_root)
					base = "root";
				else if (!n.id.empty())
					base = sanitize_identifier(n.id);
				else
					base = std::string(widget_kind_name(n.kind)) + "_" + std::to_string(index);
				names[&n] = unique_name(base);
				int i = 0;
				for (const auto& c : n.children)
					collect_names(*c, false, ++i);
			}

			void line(const std::string& text)
			{
				out << "    " << text << "\n";
			}

			void emit_node(const ui_node& n, bool is_root)
			{
				const std::string v = names[&n];

				if (is_root)
				{
					line("vgui_widget* " + v + " = vgui_root(ui);");
				}
				else
				{
					const std::string parent = names[n.parent];
					std::string text_arg = "nullptr";
					if (is_text_kind(n.kind))
						text_arg = "\"" + escape_c_string(n.text) + "\"";
					else if (n.kind == VGUI_EDIT)
						text_arg = "\"\"";
					line("vgui_widget* " + v + " = vgui_widget_add(" + parent + ", " +
						kind_enum_name(n.kind) + ", " + text_arg + ");");
				}

				if (!n.id.empty())
					line("vgui_widget_set_id(" + v + ", \"" + escape_c_string(n.id) + "\");");

				/* layout */
				if (n.kind == VGUI_PANEL)
				{
					if (n.layout != VGUI_LAYOUT_ABSOLUTE)
						line("vgui_widget_set_layout(" + v + ", " + layout_name(n.layout) + ");");
				}
				if (!is_root)
				{
					if (n.x != 0.0f || n.y != 0.0f)
						line("vgui_widget_set_pos(" + v + ", " + number_text(n.x) + ", " + number_text(n.y) + ");");
					if (n.w != 0.0f || n.h != 0.0f)
						line("vgui_widget_set_size(" + v + ", " + number_text(n.w) + ", " + number_text(n.h) + ");");
				}
				if (n.min_w > 0.0f || n.min_h > 0.0f)
					line("vgui_widget_set_min_size(" + v + ", " + number_text(n.min_w) + ", " + number_text(n.min_h) + ");");
				if (n.max_w > 0.0f || n.max_h > 0.0f)
					line("vgui_widget_set_max_size(" + v + ", " + number_text(n.max_w) + ", " + number_text(n.max_h) + ");");
				if (n.margin[0] != 0.0f || n.margin[1] != 0.0f || n.margin[2] != 0.0f || n.margin[3] != 0.0f)
					line("vgui_widget_set_margin(" + v + ", " + number_text(n.margin[0]) + ", " +
						number_text(n.margin[1]) + ", " + number_text(n.margin[2]) + ", " + number_text(n.margin[3]) + ");");
				if (n.padding[0] != 0.0f || n.padding[1] != 0.0f || n.padding[2] != 0.0f || n.padding[3] != 0.0f)
					line("vgui_widget_set_padding(" + v + ", " + number_text(n.padding[0]) + ", " +
						number_text(n.padding[1]) + ", " + number_text(n.padding[2]) + ", " + number_text(n.padding[3]) + ");");
				if (n.gap != 0.0f)
					line("vgui_widget_set_gap(" + v + ", " + number_text(n.gap) + ");");
				if (n.grow != 0.0f)
					line("vgui_widget_set_grow(" + v + ", " + number_text(n.grow) + ");");
				if (n.align_items != VGUI_ALIGN_START)
					line("vgui_widget_set_align_items(" + v + ", " + align_name(n.align_items) + ");");
				if (n.align_self != VGUI_ALIGN_AUTO)
					line("vgui_widget_set_align_self(" + v + ", " + align_name(n.align_self) + ");");
				if (n.justify != VGUI_JUSTIFY_START)
					line("vgui_widget_set_justify(" + v + ", " + justify_name(n.justify) + ");");

				/* visual */
				if (n.bg != 0)
					line("vgui_widget_set_background(" + v + ", " + color_literal(n.bg) + ");");
				if (n.border_color != 0 && n.border_width > 0.0f)
					line("vgui_widget_set_border(" + v + ", " + color_literal(n.border_color) + ", " +
						number_text(n.border_width) + ");");
				if (n.rounding > 0.0f)
					line("vgui_widget_set_rounding(" + v + ", " + number_text(n.rounding) + ");");
				if (n.text_color != 0)
					line("vgui_widget_set_text_color(" + v + ", " + color_literal(n.text_color) + ");");
				if (n.font_size > 0.0f)
					line("vgui_widget_set_font_size(" + v + ", " + number_text(n.font_size) + ");");
				if (n.opacity != 1.0f)
					line("vgui_widget_set_opacity(" + v + ", " + number_text(n.opacity) + ");");
				if (n.text_align_x != 0.5f || n.text_align_y != 0.5f)
					line("vgui_widget_set_text_align(" + v + ", " + number_text(n.text_align_x) + ", " +
						number_text(n.text_align_y) + ");");

				if (!n.visible)
					line("vgui_widget_set_visible(" + v + ", 0);");
				if (!n.enabled)
					line("vgui_widget_set_enabled(" + v + ", 0);");
				if (n.focusable)
					line("vgui_widget_set_focusable(" + v + ", 1);");

				/* values */
				if (n.kind == VGUI_CHECKBOX || n.kind == VGUI_RADIO || n.kind == VGUI_SWITCH)
				{
					if (n.checked)
						line("vgui_widget_set_checked(" + v + ", 1);");
				}
				if (n.kind == VGUI_SLIDER || n.kind == VGUI_PROGRESS)
				{
					line("vgui_widget_set_range(" + v + ", " + number_text(n.range_min) + ", " +
						number_text(n.range_max) + ");");
					line("vgui_widget_set_value(" + v + ", " + number_text(n.value) + ");");
					if (n.step > 0.0f)
						line("vgui_widget_set_step(" + v + ", " + number_text(n.step) + ");");
					if (n.kind == VGUI_SLIDER && n.vertical)
						line("vgui_widget_set_vertical(" + v + ", 1);");
				}

				/* edit */
				if (n.kind == VGUI_EDIT)
				{
					if (!n.edit_text.empty())
						line("vgui_edit_set_text(" + v + ", \"" + escape_c_string(n.edit_text) + "\");");
					if (!n.placeholder.empty())
						line("vgui_edit_set_placeholder(" + v + ", \"" + escape_c_string(n.placeholder) + "\");");
					if (n.multiline)
						line("vgui_edit_set_multiline(" + v + ", 1);");
					if (n.password)
						line("vgui_edit_set_password(" + v + ", '*');");
					if (n.readonly)
						line("vgui_edit_set_readonly(" + v + ", 1);");
				}

				for (const auto& c : n.children)
					emit_node(*c, false);
			}

			std::string run(const ui_document& doc)
			{
				std::string fn = sanitize_identifier(opt->function_name);
				if (fn.empty()) fn = "build_ui";

				collect_names(doc.root, true, 0);

				out << "// Generated by vgui_editor. Do not edit by hand.\n";
				if (opt->include_comments)
				{
					out << "// Document: " << doc.name << "\n";
					out << "// Canvas: " << doc.canvas_width << " x " << doc.canvas_height << "\n";
				}
				out << "\n#include \"vgui.h\"\n\n";
				out << "void " << fn << "(vgui_context* ui)\n{\n";
				line("vgui_context_set_size(ui, " + std::to_string(doc.canvas_width) + ", " +
					std::to_string(doc.canvas_height) + ");");
				emit_node(doc.root, true);
				out << "}\n";
				return out.str();
			}
		};

	} /* namespace */

	std::string ui_generate_cpp(const ui_document& doc, const ui_export_options& options)
	{
		codegen_state st;
		st.opt = &options;
		return st.run(doc);
	}

	/* ------------------------------------------------------------------------- */
	/* editor implementation                                                     */
	/* ------------------------------------------------------------------------- */

	const char* default_cpp_file_name()
	{
		return "generated_ui.cpp";
	}
	static std::string source_gui_dir()
	{
		namespace fs = std::filesystem;
#ifdef VGUI_SOURCE_DIR
		std::string base = VGUI_SOURCE_DIR;
		if (!base.empty())
		{
			fs::path p = fs::path(base) / "gui";
			std::error_code ec;
			if (fs::is_directory(p, ec))
				return p.generic_string();
		}
#endif
		std::error_code ec;
		fs::path cwd = fs::current_path(ec);
		return ec ? std::string(".") : cwd.generic_string();
	}

	std::string default_json_path()
	{
		return source_gui_dir() + "/ui_document.json";
	}

	std::string default_cpp_path()
	{
		return source_gui_dir() + "/" + default_cpp_file_name();
	}

	enum ui_prop_id
	{
		P_ID = 1,
		P_TEXT,
		P_CANVAS_W,
		P_CANVAS_H,
		P_X,
		P_Y,
		P_W,
		P_H,
		P_MIN_W,
		P_MIN_H,
		P_MAX_W,
		P_MAX_H,
		P_MARGIN_L,
		P_MARGIN_T,
		P_MARGIN_R,
		P_MARGIN_B,
		P_PAD_L,
		P_PAD_T,
		P_PAD_R,
		P_PAD_B,
		P_GAP,
		P_GROW,
		P_BG,
		P_BORDER_COLOR,
		P_BORDER_W,
		P_ROUNDING,
		P_FONT_SIZE,
		P_TEXT_COLOR,
		P_OPACITY,
		P_ALIGN_X,
		P_ALIGN_Y,
		P_VALUE,
		P_RANGE_MIN,
		P_RANGE_MAX,
		P_STEP,
		P_EDIT_TEXT,
		P_PLACEHOLDER
	};

	enum ui_bool_prop_id
	{
		B_VISIBLE = 100,
		B_ENABLED,
		B_FOCUSABLE,
		B_CHECKED,
		B_VERTICAL,
		B_MULTILINE,
		B_PASSWORD,
		B_READONLY
	};

	enum ui_enum_prop_id
	{
		E_LAYOUT = 200,
		E_ALIGN_ITEMS,
		E_ALIGN_SELF,
		E_JUSTIFY
	};

	struct ui_editor::impl
	{
		ui_editor* owner = nullptr;
		vgui_context* ctx = nullptr;
		vgui_widget* root = nullptr;
		vgui_widget* status_label = nullptr;
		vgui_widget* json_path_edit = nullptr;
		vgui_widget* cpp_path_edit = nullptr;
		vgui_widget* hierarchy_body = nullptr;
		vgui_widget* props_body = nullptr;
		vgui_widget* canvas = nullptr;
		vgui_widget* preview_root = nullptr;
		vgui_widget* selection_frame = nullptr;
		ui_node* selected = nullptr;

		std::unordered_map<ui_node*, vgui_widget*> by_node;
		std::unordered_map<vgui_widget*, ui_node*> by_widget;

		struct binding
		{
			impl* ed = nullptr;
			ui_node* node = nullptr;
			int prop = 0;
			int aux = 0;
		};
		std::vector<std::unique_ptr<binding>> row_bindings;
		std::vector<std::unique_ptr<binding>> prop_bindings;

		bool dragging = false;
		ui_node* drag_node = nullptr;
		float drag_start_mouse_x = 0.0f;
		float drag_start_mouse_y = 0.0f;
		float drag_start_x = 0.0f;
		float drag_start_y = 0.0f;

		int applied_canvas_w = -1;
		int applied_canvas_h = -1;
		float frame_x = -1, frame_y = -1, frame_w = -1, frame_h = -1;

		ui_document& doc() { return owner->document(); }

		/* --- UI construction -------------------------------------------------- */

		vgui_widget* add_button(vgui_widget* parent, const char* text, const char* cmd);
		vgui_widget* add_row(vgui_widget* parent);
		vgui_widget* build_toolbar();
		vgui_widget* build_paths();
		vgui_widget* build_addbar();
		vgui_widget* build_main();
		void build();

		/* --- preview ---------------------------------------------------------- */

		void apply_node(vgui_widget* w, const ui_node& n);
		vgui_widget* build_preview_child(vgui_widget* parent, ui_node& n);
		void rebuild_preview();
		void refresh_node_visual(ui_node* n);
		void sync_page();
		void sync_selection_frame();

		/* --- panels ----------------------------------------------------------- */

		void rebuild_hierarchy();
		void rebuild_properties();
		void rebuild_all();

		void select(ui_node* node);
		void add_node_of_kind(vgui_widget_kind kind);
		void delete_selected_node();
		void new_document();

		/* --- property plumbing ------------------------------------------------ */

		void apply_text_prop(ui_node* n, int prop, const char* text);
		void apply_bool_prop(ui_node* n, int prop, bool value);
		void apply_enum_prop(ui_node* n, int prop, int value);

		/* --- commands --------------------------------------------------------- */

		void handle_command(const std::string& cmd);
		std::string unique_id(vgui_widget_kind kind) const;
		std::string node_label(const ui_node& n) const;
		void set_status(const std::string& text);

		/* --- misc ------------------------------------------------------------- */

		std::string json_path_value() const;
		std::string cpp_path_value() const;
	};

	/* ---------------------------- callbacks ---------------------------------- */

	static void on_command(vgui_widget* w, const vgui_event* e, void* user)
	{
		if (!e || e->type != VGUI_EVENT_CLICK) return;
		auto* ed = static_cast<ui_editor::impl*>(user);
		ed->handle_command(vgui_widget_get_id(w));
	}

	static void on_preview_event(vgui_widget* w, const vgui_event* e, void* user)
	{
		auto* ed = static_cast<ui_editor::impl*>(user);
		if (!e) return;
		auto it = ed->by_widget.find(w);

		if (e->type == VGUI_EVENT_MOUSE_DOWN)
		{
			if (it == ed->by_widget.end())
			{
				/* Empty canvas area outside the page selects the document root. */
				ed->select(&ed->doc().root);
				return;
			}
			ui_node* n = it->second;
			ed->select(n);

			ed->dragging = false;
			ed->drag_node = nullptr;
			if (n != &ed->doc().root && n->parent && n->parent->layout == VGUI_LAYOUT_ABSOLUTE &&
				n->kind != VGUI_EDIT)
			{
				float rx = 0, ry = 0, rw = 0, rh = 0;
				vgui_widget_get_rect(w, &rx, &ry, &rw, &rh);
				ed->dragging = true;
				ed->drag_node = n;
				ed->drag_start_mouse_x = rx + e->x;
				ed->drag_start_mouse_y = ry + e->y;
				ed->drag_start_x = n->x;
				ed->drag_start_y = n->y;
			}
			return;
		}

		if (e->type == VGUI_EVENT_MOUSE_MOVE && ed->dragging && ed->drag_node)
		{
			float rx = 0, ry = 0, rw = 0, rh = 0;
			vgui_widget_get_rect(w, &rx, &ry, &rw, &rh);
			float mx = rx + e->x;
			float my = ry + e->y;
			ed->drag_node->x = ed->drag_start_x + (mx - ed->drag_start_mouse_x);
			ed->drag_node->y = ed->drag_start_y + (my - ed->drag_start_mouse_y);
			ed->refresh_node_visual(ed->drag_node);
			return;
		}

		if (e->type == VGUI_EVENT_MOUSE_UP)
		{
			ed->dragging = false;
			ed->drag_node = nullptr;
			return;
		}
	}

	static void on_row_event(vgui_widget* w, const vgui_event* e, void* user)
	{
		(void)w;
		auto* b = static_cast<ui_editor::impl::binding*>(user);
		if (!e) return;
		if (e->type == VGUI_EVENT_CLICK || e->type == VGUI_EVENT_MOUSE_DOWN)
		{
			ui_editor::impl* ed = b->ed;
			ui_node* n = b->node;
			if (ed) ed->select(n);
		}
	}

	static void on_prop_text(vgui_widget* w, const char* text, void* user)
	{
		(void)w;
		auto* b = static_cast<ui_editor::impl::binding*>(user);
		if (!b || !b->ed) return;
		b->ed->apply_text_prop(b->node, b->prop, text ? text : "");
	}

	static void on_prop_bool(vgui_widget* w, const vgui_event* e, void* user)
	{
		auto* b = static_cast<ui_editor::impl::binding*>(user);
		if (!b || !b->ed || !e) return;
		if (e->type != VGUI_EVENT_VALUE_CHANGED && e->type != VGUI_EVENT_CLICK) return;
		b->ed->apply_bool_prop(b->node, b->prop, vgui_widget_get_checked(w) != 0);
	}

	static void on_prop_enum(vgui_widget* w, const vgui_event* e, void* user)
	{
		(void)w;
		auto* b = static_cast<ui_editor::impl::binding*>(user);
		if (!b || !b->ed || !e || e->type != VGUI_EVENT_CLICK) return;
		ui_editor::impl* ed = b->ed;
		ui_node* n = b->node;
		int prop = b->prop;
		int value = b->aux;
		ed->apply_enum_prop(n, prop, value);
		ed->rebuild_properties();
	}

	/* ---------------------------- UI construction ---------------------------- */

	vgui_widget* ui_editor::impl::add_button(vgui_widget* parent, const char* text, const char* cmd)
	{
		vgui_widget* b = vgui_widget_add(parent, VGUI_BUTTON, text);
		vgui_widget_set_id(b, cmd);
		vgui_widget_on_event(b, on_command, this);
		return b;
	}

	vgui_widget* ui_editor::impl::add_row(vgui_widget* parent)
	{
		vgui_widget* row = vgui_widget_add(parent, VGUI_PANEL, nullptr);
		vgui_widget_set_layout(row, VGUI_LAYOUT_ROW);
		vgui_widget_set_background(row, 0);
		vgui_widget_set_padding(row, 0, 0, 0, 0);
		vgui_widget_set_gap(row, 6);
		vgui_widget_set_align_items(row, VGUI_ALIGN_CENTER);
		return row;
	}

	vgui_widget* ui_editor::impl::build_toolbar()
	{
		vgui_widget* row = add_row(root);
		add_button(row, "新建", "cmd:new");
		add_button(row, "删除", "cmd:delete");
		add_button(row, "保存 JSON", "cmd:save");
		add_button(row, "载入 JSON", "cmd:load");
		add_button(row, "导出 C++", "cmd:export");

		status_label = vgui_widget_add(row, VGUI_LABEL, "就绪");
		vgui_widget_set_grow(status_label, 1.0f);
		vgui_widget_set_text_align(status_label, 1.0f, 0.5f);
		vgui_widget_set_text_color(status_label, 0xFF9CC7A4);
		vgui_widget_set_font_size(status_label, 14.0f);
		vgui_widget_set_padding(status_label, 8, 0, 8, 0);
		return row;
	}

	vgui_widget* ui_editor::impl::build_paths()
	{
		vgui_widget* row = add_row(root);
		vgui_widget* json_label = vgui_widget_add(row, VGUI_LABEL, "JSON");
		vgui_widget_set_size(json_label, 44.0f, 26.0f);
		json_path_edit = vgui_widget_add(row, VGUI_EDIT, "");
		vgui_widget_set_grow(json_path_edit, 1.0f);
		vgui_widget_set_size(json_path_edit, 0.0f, 28.0f);
		vgui_widget_set_font_size(json_path_edit, 13.0f);
		vgui_edit_set_text(json_path_edit, default_json_path().c_str());

		vgui_widget* cpp_label = vgui_widget_add(row, VGUI_LABEL, "CPP");
		vgui_widget_set_size(cpp_label, 44.0f, 26.0f);
		cpp_path_edit = vgui_widget_add(row, VGUI_EDIT, "");
		vgui_widget_set_grow(cpp_path_edit, 1.0f);
		vgui_widget_set_size(cpp_path_edit, 0.0f, 28.0f);
		vgui_widget_set_font_size(cpp_path_edit, 13.0f);
		vgui_edit_set_text(cpp_path_edit, default_cpp_path().c_str());
		return row;
	}

	vgui_widget* ui_editor::impl::build_addbar()
	{
		vgui_widget* row = add_row(root);
		vgui_widget* label = vgui_widget_add(row, VGUI_LABEL, "添加");
		vgui_widget_set_size(label, 44.0f, 26.0f);

		static const vgui_widget_kind kinds[] = {
			VGUI_PANEL, VGUI_LABEL, VGUI_BUTTON, VGUI_CHECKBOX, VGUI_RADIO,
			VGUI_SWITCH, VGUI_SLIDER, VGUI_PROGRESS, VGUI_EDIT, VGUI_SEPARATOR
		};
		static const char* names[] = {
			"面板", "标签", "按钮", "复选", "单选", "开关", "滑杆", "进度", "输入", "分隔"
		};
		for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); ++i)
		{
			char cmd[64] = {};
			snprintf(cmd, sizeof(cmd), "add:%d", (int)kinds[i]);
			add_button(row, names[i], cmd);
		}
		return row;
	}

	vgui_widget* ui_editor::impl::build_main()
	{
		vgui_widget* main = add_row(root);
		vgui_widget_set_grow(main, 1.0f);
		vgui_widget_set_align_items(main, VGUI_ALIGN_STRETCH);
		vgui_widget_set_gap(main, 8);

		/* hierarchy ---------------------------------------------------------- */
		vgui_widget* hierarchy = vgui_widget_add(main, VGUI_PANEL, nullptr);
		vgui_widget_set_size(hierarchy, 240.0f, 0.0f);
		vgui_widget_set_layout(hierarchy, VGUI_LAYOUT_COLUMN);
		vgui_widget_set_align_items(hierarchy, VGUI_ALIGN_STRETCH);
		vgui_widget_set_background(hierarchy, 0xFF202124);
		vgui_widget_set_border(hierarchy, 0xFF33363A, 1.0f);
		vgui_widget_set_rounding(hierarchy, 4.0f);
		vgui_widget_set_padding(hierarchy, 8, 8, 8, 8);
		vgui_widget_set_gap(hierarchy, 6);

		vgui_widget* hierarchy_title = vgui_widget_add(hierarchy, VGUI_LABEL, "层级");
		vgui_widget_set_font_size(hierarchy_title, 14.0f);
		vgui_widget_set_text_color(hierarchy_title, 0xFFAEB4BC);

		hierarchy_body = vgui_widget_add(hierarchy, VGUI_PANEL, nullptr);
		vgui_widget_set_grow(hierarchy_body, 1.0f);
		vgui_widget_set_layout(hierarchy_body, VGUI_LAYOUT_COLUMN);
		vgui_widget_set_align_items(hierarchy_body, VGUI_ALIGN_STRETCH);
		vgui_widget_set_background(hierarchy_body, 0);
		vgui_widget_set_padding(hierarchy_body, 0, 0, 0, 0);
		vgui_widget_set_gap(hierarchy_body, 2);
		vgui_panel_set_scrollable(hierarchy_body, 0, 1);

		/* canvas ------------------------------------------------------------- */
		canvas = vgui_widget_add(main, VGUI_PANEL, nullptr);
		vgui_widget_set_grow(canvas, 1.0f);
		vgui_widget_set_min_size(canvas, 160.0f, 0.0f);
		vgui_widget_set_layout(canvas, VGUI_LAYOUT_ABSOLUTE);
		vgui_widget_set_background(canvas, 0xFF0B0C0E);
		vgui_widget_set_border(canvas, 0xFF2A2D31, 1.0f);
		vgui_widget_set_padding(canvas, 0, 0, 0, 0);
		vgui_widget_on_event(canvas, on_preview_event, this);

		preview_root = vgui_widget_add(canvas, VGUI_PANEL, nullptr);
		vgui_widget_set_pos(preview_root, 0, 0);
		vgui_widget_set_size(preview_root, (float)doc().canvas_width, (float)doc().canvas_height);

		selection_frame = vgui_widget_add(canvas, VGUI_PANEL, nullptr);
		vgui_widget_set_background(selection_frame, 0);
		vgui_widget_set_border(selection_frame, 0xFF409EFF, 2.0f);
		vgui_widget_set_rounding(selection_frame, 2.0f);
		vgui_widget_set_enabled(selection_frame, 0);
		vgui_widget_set_visible(selection_frame, 0);

		/* properties --------------------------------------------------------- */
		vgui_widget* props = vgui_widget_add(main, VGUI_PANEL, nullptr);
		vgui_widget_set_size(props, 300.0f, 0.0f);
		vgui_widget_set_layout(props, VGUI_LAYOUT_COLUMN);
		vgui_widget_set_align_items(props, VGUI_ALIGN_STRETCH);
		vgui_widget_set_background(props, 0xFF202124);
		vgui_widget_set_border(props, 0xFF33363A, 1.0f);
		vgui_widget_set_rounding(props, 4.0f);
		vgui_widget_set_padding(props, 8, 8, 8, 8);
		vgui_widget_set_gap(props, 6);

		vgui_widget* props_title = vgui_widget_add(props, VGUI_LABEL, "属性");
		vgui_widget_set_font_size(props_title, 14.0f);
		vgui_widget_set_text_color(props_title, 0xFFAEB4BC);

		props_body = vgui_widget_add(props, VGUI_PANEL, nullptr);
		vgui_widget_set_grow(props_body, 1.0f);
		vgui_widget_set_layout(props_body, VGUI_LAYOUT_COLUMN);
		vgui_widget_set_align_items(props_body, VGUI_ALIGN_STRETCH);
		vgui_widget_set_background(props_body, 0);
		vgui_widget_set_padding(props_body, 2, 2, 2, 2);
		vgui_widget_set_gap(props_body, 4);
		vgui_panel_set_scrollable(props_body, 0, 1);

		return main;
	}

	void ui_editor::impl::build()
	{
		root = vgui_root(ctx);
		vgui_widget_set_background(root, 0xFF17181A);
		vgui_widget_set_layout(root, VGUI_LAYOUT_COLUMN);
		vgui_widget_set_align_items(root, VGUI_ALIGN_STRETCH);
		vgui_widget_set_padding(root, 8, 8, 8, 8);
		vgui_widget_set_gap(root, 6);

		build_toolbar();
		build_paths();
		build_addbar();
		build_main();

		applied_canvas_w = -1;
		applied_canvas_h = -1;
		selected = &doc().root;
		rebuild_preview();
		rebuild_hierarchy();
		rebuild_properties();
		set_status("就绪：点击层级或画布中的控件进行选择");
	}

	/* ---------------------------- preview ------------------------------------ */

	void ui_editor::impl::apply_node(vgui_widget* w, const ui_node& n)
	{
		if (!w) return;

		vgui_widget_set_id(w, n.id.c_str());

		if (n.kind == VGUI_EDIT)
		{
			vgui_edit_set_text(w, n.edit_text.c_str());
			vgui_edit_set_placeholder(w, n.placeholder.c_str());
			vgui_edit_set_multiline(w, n.multiline ? 1 : 0);
			vgui_edit_set_password(w, n.password ? '*' : 0);
			vgui_edit_set_readonly(w, n.readonly ? 1 : 0);
		}
		else if (is_text_kind(n.kind))
		{
			vgui_widget_set_text(w, n.text.c_str());
		}

		if (n.kind == VGUI_PANEL)
			vgui_widget_set_layout(w, (vgui_layout_kind)n.layout);

		if (&n != &doc().root)
		{
			vgui_widget_set_pos(w, n.x, n.y);
			vgui_widget_set_size(w, n.w, n.h);
		}
		vgui_widget_set_min_size(w, n.min_w, n.min_h);
		vgui_widget_set_max_size(w, n.max_w, n.max_h);
		vgui_widget_set_margin(w, n.margin[0], n.margin[1], n.margin[2], n.margin[3]);
		/* 只有面板提供 padding 编辑；其它控件保留主题默认内边距，
		 * 否则编辑框/按钮的文字会贴到边框上。 */
		if (n.kind == VGUI_PANEL)
			vgui_widget_set_padding(w, n.padding[0], n.padding[1], n.padding[2], n.padding[3]);
		vgui_widget_set_gap(w, n.gap);
		vgui_widget_set_grow(w, n.grow);
		vgui_widget_set_align_items(w, (vgui_align)n.align_items);
		vgui_widget_set_align_self(w, (vgui_align)n.align_self);
		vgui_widget_set_justify(w, (vgui_justify)n.justify);

		vgui_widget_set_background(w, n.bg);
		vgui_widget_set_border(w, n.border_color, n.border_width);
		vgui_widget_set_rounding(w, n.rounding);

		vgui_theme theme = {};
		vgui_context_get_theme(ctx, &theme);
		vgui_widget_set_text_color(w, n.text_color ? n.text_color : theme.text);
		if (n.font_size > 0.0f)
			vgui_widget_set_font_size(w, n.font_size);
		vgui_widget_set_opacity(w, n.opacity);
		vgui_widget_set_text_align(w, n.text_align_x, n.text_align_y);

		vgui_widget_set_visible(w, n.visible ? 1 : 0);
		vgui_widget_set_enabled(w, n.enabled ? 1 : 0);
		vgui_widget_set_focusable(w, n.focusable ? 1 : 0);

		if (n.kind == VGUI_CHECKBOX || n.kind == VGUI_RADIO || n.kind == VGUI_SWITCH)
		{
			vgui_widget_set_checked(w, n.checked ? 1 : 0);
		}
		if (n.kind == VGUI_SLIDER || n.kind == VGUI_PROGRESS)
		{
			vgui_widget_set_range(w, n.range_min, n.range_max);
			vgui_widget_set_value(w, n.value);
			if (n.step > 0.0f)
				vgui_widget_set_step(w, n.step);
			if (n.kind == VGUI_SLIDER)
				vgui_widget_set_vertical(w, n.vertical ? 1 : 0);
		}
	}

	vgui_widget* ui_editor::impl::build_preview_child(vgui_widget* parent, ui_node& n)
	{
		const char* text = is_text_kind(n.kind) ? n.text.c_str() : (n.kind == VGUI_EDIT ? "" : nullptr);
		vgui_widget* w = vgui_widget_add(parent, n.kind, text);
		if (!w) return nullptr;
		by_node[&n] = w;
		by_widget[w] = &n;
		apply_node(w, n);
		vgui_widget_on_event(w, on_preview_event, this);

		for (auto& child : n.children)
			build_preview_child(w, *child);
		return w;
	}

	void ui_editor::impl::rebuild_preview()
	{
		by_node.clear();
		by_widget.clear();
		vgui_widget_clear(preview_root);

		apply_node(preview_root, doc().root);
		by_node[&doc().root] = preview_root;
		by_widget[preview_root] = &doc().root;
		vgui_widget_on_event(preview_root, on_preview_event, this);

		for (auto& child : doc().root.children)
			build_preview_child(preview_root, *child);

		applied_canvas_w = doc().canvas_width;
		applied_canvas_h = doc().canvas_height;
		vgui_widget_set_size(preview_root, (float)applied_canvas_w, (float)applied_canvas_h);
		frame_x = frame_y = frame_w = frame_h = -1;
	}

	void ui_editor::impl::refresh_node_visual(ui_node* n)
	{
		if (!n) return;
		auto it = by_node.find(n);
		if (it == by_node.end() || !it->second) return;
		apply_node(it->second, *n);
		if (n == &doc().root)
			vgui_widget_set_size(it->second, (float)doc().canvas_width, (float)doc().canvas_height);
	}

	void ui_editor::impl::sync_page()
	{
		if (!preview_root) return;
		if (applied_canvas_w != doc().canvas_width || applied_canvas_h != doc().canvas_height)
		{
			applied_canvas_w = doc().canvas_width;
			applied_canvas_h = doc().canvas_height;
			vgui_widget_set_size(preview_root, (float)applied_canvas_w, (float)applied_canvas_h);
		}
	}

	void ui_editor::impl::sync_selection_frame()
	{
		if (!selection_frame || !canvas) return;
		if (!selected)
		{
			vgui_widget_set_visible(selection_frame, 0);
			frame_x = frame_y = frame_w = frame_h = -1;
			return;
		}
		auto it = by_node.find(selected);
		if (it == by_node.end() || !it->second || !vgui_widget_get_visible(it->second))
		{
			vgui_widget_set_visible(selection_frame, 0);
			frame_x = frame_y = frame_w = frame_h = -1;
			return;
		}

		float wx = 0, wy = 0, ww = 0, wh = 0;
		vgui_widget_get_rect(it->second, &wx, &wy, &ww, &wh);
		float cx = 0, cy = 0, cw = 0, ch = 0;
		vgui_widget_get_rect(canvas, &cx, &cy, &cw, &ch);
		float fx = wx - cx;
		float fy = wy - cy;
		if (fx != frame_x || fy != frame_y || ww != frame_w || wh != frame_h)
		{
			frame_x = fx;
			frame_y = fy;
			frame_w = ww;
			frame_h = wh;
			vgui_widget_set_pos(selection_frame, fx, fy);
			vgui_widget_set_size(selection_frame, ww, wh);
		}
		vgui_widget_set_visible(selection_frame, 1);
	}

	/* ---------------------------- panels ------------------------------------- */

	std::string ui_editor::impl::node_label(const ui_node& n) const
	{
		if (n.id.empty())
			return std::string("(") + widget_kind_name(n.kind) + ")";
		return n.id + "  (" + widget_kind_name(n.kind) + ")";
	}

	void ui_editor::impl::rebuild_hierarchy()
	{
		if (!hierarchy_body) return;
		vgui_widget_clear(hierarchy_body);
		row_bindings.clear();

		std::vector<std::pair<ui_node*, int>> stack;
		stack.push_back({ &doc().root, 0 });
		while (!stack.empty())
		{
			auto [n, depth] = stack.back();
			stack.pop_back();

			auto b = std::make_unique<binding>();
			b->ed = this;
			b->node = n;
			binding* raw = b.get();
			row_bindings.push_back(std::move(b));

			std::string label = node_label(*n);

			vgui_widget* row = vgui_widget_add(hierarchy_body, VGUI_BUTTON, label.c_str());
			vgui_widget_set_size(row, 0.0f, 26.0f);
			vgui_widget_set_font_size(row, 13.0f);
			/* 用 padding 做层级缩进：前导空格在文本排版里几乎不可见。 */
			vgui_widget_set_padding(row, 8.0f + (float)depth * 18.0f, 2.0f, 8.0f, 2.0f);
			vgui_widget_set_align_items(row, VGUI_ALIGN_START);
			vgui_widget_set_text_align(row, 0.0f, 0.5f);
			if (n == selected)
				vgui_widget_set_background(row, 0xFF2F5E9E);
			vgui_widget_on_event(row, on_row_event, raw);

			/* Push children reversed so the first child is processed first. */
			for (auto it = n->children.rbegin(); it != n->children.rend(); ++it)
				stack.push_back({ it->get(), depth + 1 });
		}
	}

	static int align_list_count() { return 5; }

	void ui_editor::impl::rebuild_properties()
	{
		if (!props_body) return;
		vgui_widget_clear(props_body);
		prop_bindings.clear();

		ui_node* n = selected;
		if (!n)
		{
			vgui_widget* hint = vgui_widget_add(props_body, VGUI_LABEL, "未选中控件");
			vgui_widget_set_text_color(hint, 0xFF9AA0A6);
			return;
		}

		const bool is_root = (n == &doc().root);

		auto section = [&](const char* title)
			{
				vgui_widget* l = vgui_widget_add(props_body, VGUI_LABEL, title);
				vgui_widget_set_font_size(l, 13.0f);
				vgui_widget_set_text_color(l, 0xFF7FB2FF);
				vgui_widget_set_padding(l, 0, 6, 0, 2);
			};

		auto row = [&]()
			{
				vgui_widget* r = vgui_widget_add(props_body, VGUI_PANEL, nullptr);
				vgui_widget_set_layout(r, VGUI_LAYOUT_ROW);
				vgui_widget_set_background(r, 0);
				vgui_widget_set_padding(r, 0, 0, 0, 0);
				vgui_widget_set_gap(r, 6);
				vgui_widget_set_align_items(r, VGUI_ALIGN_CENTER);
				return r;
			};

		auto text_row = [&](const char* label, const std::string& value, int prop)
			{
				vgui_widget* r = row();
				vgui_widget* l = vgui_widget_add(r, VGUI_LABEL, label);
				vgui_widget_set_size(l, 86.0f, 24.0f);
				vgui_widget_set_font_size(l, 12.0f);
				vgui_widget_set_text_color(l, 0xFFAEB4BC);
				vgui_widget* e = vgui_widget_add(r, VGUI_EDIT, "");
				vgui_widget_set_grow(e, 1.0f);
				vgui_widget_set_size(e, 0.0f, 26.0f);
				vgui_widget_set_font_size(e, 12.0f);
				vgui_edit_set_text(e, value.c_str());

				auto b = std::make_unique<binding>();
				b->ed = this;
				b->node = n;
				b->prop = prop;
				binding* raw = b.get();
				prop_bindings.push_back(std::move(b));
				vgui_widget_on_text(e, on_prop_text, raw);
				return e;
			};

		auto bool_row = [&](const char* label, bool value, int prop)
			{
				vgui_widget* r = row();
				vgui_widget* l = vgui_widget_add(r, VGUI_LABEL, label);
				vgui_widget_set_size(l, 86.0f, 24.0f);
				vgui_widget_set_font_size(l, 12.0f);
				vgui_widget_set_text_color(l, 0xFFAEB4BC);
				vgui_widget* c = vgui_widget_add(r, VGUI_CHECKBOX, "");
				vgui_widget_set_checked(c, value ? 1 : 0);
				vgui_widget_set_focusable(c, 1);

				auto b = std::make_unique<binding>();
				b->ed = this;
				b->node = n;
				b->prop = prop;
				binding* raw = b.get();
				prop_bindings.push_back(std::move(b));
				vgui_widget_on_event(c, on_prop_bool, raw);
				return c;
			};

		auto enum_row = [&](const char* label, int prop, const char* const* names, int count, int current)
			{
				vgui_widget* r = row();
				vgui_widget* l = vgui_widget_add(r, VGUI_LABEL, label);
				vgui_widget_set_size(l, 86.0f, 24.0f);
				vgui_widget_set_font_size(l, 12.0f);
				vgui_widget_set_text_color(l, 0xFFAEB4BC);
				vgui_widget* group = vgui_widget_add(r, VGUI_PANEL, nullptr);
				vgui_widget_set_layout(group, VGUI_LAYOUT_ROW);
				vgui_widget_set_background(group, 0);
				vgui_widget_set_padding(group, 0, 0, 0, 0);
				vgui_widget_set_gap(group, 2);
				vgui_widget_set_grow(group, 1.0f);
				for (int i = 0; i < count; ++i)
				{
					vgui_widget* b = vgui_widget_add(group, VGUI_BUTTON, names[i]);
					vgui_widget_set_size(b, 0.0f, 24.0f);
					vgui_widget_set_font_size(b, 11.0f);
					vgui_widget_set_padding(b, 4, 1, 4, 1);
					if (i == current)
						vgui_widget_set_background(b, 0xFF2F5E9E);

					auto bind = std::make_unique<binding>();
					bind->ed = this;
					bind->node = n;
					bind->prop = prop;
					bind->aux = i;
					binding* raw = bind.get();
					prop_bindings.push_back(std::move(bind));
					vgui_widget_on_event(b, on_prop_enum, raw);
				}
			};

		/* common ------------------------------------------------------------- */
		section("通用");
		text_row("ID", n->id, P_ID);
		if (is_text_kind(n->kind))
			text_row("文本", n->text, P_TEXT);
		bool_row("可见", n->visible, B_VISIBLE);
		bool_row("启用", n->enabled, B_ENABLED);
		bool_row("可聚焦", n->focusable, B_FOCUSABLE);

		/* layout ------------------------------------------------------------- */
		if (n->kind == VGUI_PANEL && !is_root)
		{
			section("布局");
			static const char* layouts[] = { "绝对", "行", "列" };
			enum_row("排列", E_LAYOUT, layouts, 3, n->layout);
			static const char* aligns[] = { "自动", "起", "中", "末", "拉伸" };
			enum_row("对齐", E_ALIGN_ITEMS, aligns, align_list_count(), n->align_items);
			enum_row("自身对齐", E_ALIGN_SELF, aligns, align_list_count(), n->align_self);
			static const char* justifies[] = { "起", "中", "末", "两端", "环绕" };
			enum_row("主轴", E_JUSTIFY, justifies, 5, n->justify);
		}

		if (is_root)
		{
			section("画布");
			text_row("宽度", number_text((float)doc().canvas_width), P_CANVAS_W);
			text_row("高度", number_text((float)doc().canvas_height), P_CANVAS_H);
		}

		if (!is_root)
		{
			section("位置与尺寸");
			text_row("X", number_text(n->x), P_X);
			text_row("Y", number_text(n->y), P_Y);
			text_row("宽", number_text(n->w), P_W);
			text_row("高", number_text(n->h), P_H);
			text_row("最小宽", number_text(n->min_w), P_MIN_W);
			text_row("最小高", number_text(n->min_h), P_MIN_H);
			text_row("最大宽", number_text(n->max_w), P_MAX_W);
			text_row("最大高", number_text(n->max_h), P_MAX_H);
		}

		if (n->kind == VGUI_PANEL)
		{
			section("内边距 / 间距");
			text_row("左", number_text(n->padding[0]), P_PAD_L);
			text_row("上", number_text(n->padding[1]), P_PAD_T);
			text_row("右", number_text(n->padding[2]), P_PAD_R);
			text_row("下", number_text(n->padding[3]), P_PAD_B);
			text_row("间距", number_text(n->gap), P_GAP);
			text_row("伸缩", number_text(n->grow), P_GROW);
		}

		section("外观");
		text_row("背景色", color_text(n->bg), P_BG);
		text_row("边框色", color_text(n->border_color), P_BORDER_COLOR);
		text_row("边框宽", number_text(n->border_width), P_BORDER_W);
		text_row("圆角", number_text(n->rounding), P_ROUNDING);
		if (is_text_kind(n->kind) || n->kind == VGUI_EDIT)
		{
			text_row("文字色", color_text(n->text_color), P_TEXT_COLOR);
			text_row("字号", number_text(n->font_size), P_FONT_SIZE);
		}
		if (is_text_kind(n->kind))
		{
			text_row("水平对齐", number_text(n->text_align_x), P_ALIGN_X);
			text_row("垂直对齐", number_text(n->text_align_y), P_ALIGN_Y);
		}
		text_row("透明度", number_text(n->opacity), P_OPACITY);

		if (is_value_kind(n->kind))
		{
			section("数值");
			if (n->kind == VGUI_CHECKBOX || n->kind == VGUI_RADIO || n->kind == VGUI_SWITCH)
			{
				bool_row("选中", n->checked, B_CHECKED);
			}
			else
			{
				text_row("最小值", number_text(n->range_min), P_RANGE_MIN);
				text_row("最大值", number_text(n->range_max), P_RANGE_MAX);
				text_row("当前值", number_text(n->value), P_VALUE);
				text_row("步长", number_text(n->step), P_STEP);
				if (n->kind == VGUI_SLIDER)
					bool_row("垂直", n->vertical, B_VERTICAL);
			}
		}

		if (n->kind == VGUI_EDIT)
		{
			section("编辑框");
			text_row("内容", n->edit_text, P_EDIT_TEXT);
			text_row("占位符", n->placeholder, P_PLACEHOLDER);
			bool_row("多行", n->multiline, B_MULTILINE);
			bool_row("密码", n->password, B_PASSWORD);
			bool_row("只读", n->readonly, B_READONLY);
		}
	}

	void ui_editor::impl::rebuild_all()
	{
		rebuild_preview();
		rebuild_hierarchy();
		rebuild_properties();
	}

	/* ---------------------------- property application ----------------------- */

	void ui_editor::impl::apply_text_prop(ui_node* n, int prop, const char* text)
	{
		if (!n || !text) return;
		std::string value = text;

		switch (prop)
		{
		case P_ID:
			n->id = value;
			break;
		case P_TEXT:
			n->text = value;
			break;
		case P_EDIT_TEXT:
			n->edit_text = value;
			break;
		case P_PLACEHOLDER:
			n->placeholder = value;
			break;
		case P_BG:
		case P_BORDER_COLOR:
		case P_TEXT_COLOR:
		{
			uint32_t color = 0;
			if (!parse_color(value, &color)) return;
			if (prop == P_BG) n->bg = color;
			else if (prop == P_BORDER_COLOR) n->border_color = color;
			else n->text_color = color;
			break;
		}
		case P_CANVAS_W:
		case P_CANVAS_H:
		{
			float v = 0;
			if (!parse_number(value, &v)) return;
			int iv = (int)(v + 0.5f);
			if (iv < 32) iv = 32;
			if (prop == P_CANVAS_W) doc().canvas_width = iv;
			else doc().canvas_height = iv;
			n->w = (float)doc().canvas_width;
			n->h = (float)doc().canvas_height;
			sync_page();
			return;
		}
		default:
		{
			float v = 0;
			if (!parse_number(value, &v)) return;
			switch (prop)
			{
			case P_X: n->x = v; break;
			case P_Y: n->y = v; break;
			case P_W: n->w = v; break;
			case P_H: n->h = v; break;
			case P_MIN_W: n->min_w = v; break;
			case P_MIN_H: n->min_h = v; break;
			case P_MAX_W: n->max_w = v; break;
			case P_MAX_H: n->max_h = v; break;
			case P_MARGIN_L: n->margin[0] = v; break;
			case P_MARGIN_T: n->margin[1] = v; break;
			case P_MARGIN_R: n->margin[2] = v; break;
			case P_MARGIN_B: n->margin[3] = v; break;
			case P_PAD_L: n->padding[0] = v; break;
			case P_PAD_T: n->padding[1] = v; break;
			case P_PAD_R: n->padding[2] = v; break;
			case P_PAD_B: n->padding[3] = v; break;
			case P_GAP: n->gap = v; break;
			case P_GROW: n->grow = v; break;
			case P_BORDER_W: n->border_width = v; break;
			case P_ROUNDING: n->rounding = v; break;
			case P_FONT_SIZE: n->font_size = v; break;
			case P_OPACITY: n->opacity = v; break;
			case P_ALIGN_X: n->text_align_x = v; break;
			case P_ALIGN_Y: n->text_align_y = v; break;
			case P_VALUE: n->value = v; break;
			case P_RANGE_MIN: n->range_min = v; break;
			case P_RANGE_MAX: n->range_max = v; break;
			case P_STEP: n->step = v; break;
			default: return;
			}
			break;
		}
		}

		refresh_node_visual(n);
		if (prop == P_ID)
		{
			set_status("ID 已改为: " + n->id);
			rebuild_hierarchy();
		}
	}

	void ui_editor::impl::apply_bool_prop(ui_node* n, int prop, bool value)
	{
		if (!n) return;
		switch (prop)
		{
		case B_VISIBLE: n->visible = value; break;
		case B_ENABLED: n->enabled = value; break;
		case B_FOCUSABLE: n->focusable = value; break;
		case B_CHECKED: n->checked = value; break;
		case B_VERTICAL: n->vertical = value; break;
		case B_MULTILINE: n->multiline = value; break;
		case B_PASSWORD: n->password = value; break;
		case B_READONLY: n->readonly = value; break;
		default: return;
		}
		refresh_node_visual(n);
		if (n == drag_node) sync_selection_frame();
	}

	void ui_editor::impl::apply_enum_prop(ui_node* n, int prop, int value)
	{
		if (!n) return;
		switch (prop)
		{
		case E_LAYOUT: n->layout = value; break;
		case E_ALIGN_ITEMS: n->align_items = value; break;
		case E_ALIGN_SELF: n->align_self = value; break;
		case E_JUSTIFY: n->justify = value; break;
		default: return;
		}
		refresh_node_visual(n);
	}

	/* ---------------------------- commands ----------------------------------- */

	std::string ui_editor::impl::unique_id(vgui_widget_kind kind) const
	{
		std::set<std::string> ids;
		std::vector<const ui_node*> stack;
		stack.push_back(&owner->document().root);
		while (!stack.empty())
		{
			const ui_node* n = stack.back();
			stack.pop_back();
			if (!n->id.empty()) ids.insert(n->id);
			for (const auto& c : n->children) stack.push_back(c.get());
		}

		std::string base = widget_kind_name(kind);
		int index = 1;
		std::string candidate;
		do
		{
			candidate = base + "_" + std::to_string(index++);
		} while (ids.count(candidate));
		return candidate;
	}

	void ui_editor::impl::add_node_of_kind(vgui_widget_kind kind)
	{
		ui_node* parent = selected;
		if (!parent || parent->kind != VGUI_PANEL)
			parent = parent ? parent->parent : &doc().root;
		if (!parent || parent->kind != VGUI_PANEL)
			parent = &doc().root;

		auto node = make_default_node(kind);
		node->id = unique_id(kind);
		node->parent = parent;
		int cascade = (int)parent->children.size() % 10;
		node->x = 16.0f + cascade * 12.0f;
		node->y = 16.0f + cascade * 12.0f;
		ui_node* raw = node.get();
		parent->children.push_back(std::move(node));

		selected = raw;
		rebuild_all();
		set_status("已添加 " + std::string(widget_kind_name(kind)) + " (id=" + raw->id + ")");
	}

	void ui_editor::impl::delete_selected_node()
	{
		if (!selected || selected == &doc().root)
		{
			set_status("根节点不能删除");
			return;
		}
		ui_node* parent = selected->parent ? selected->parent : &doc().root;
		std::string id = selected->id;
		parent->remove_child(selected);
		selected = parent;
		rebuild_all();
		set_status("已删除 " + id);
	}

	void ui_editor::impl::new_document()
	{
		doc().reset();
		selected = &doc().root;
		dragging = false;
		drag_node = nullptr;
		rebuild_all();
		set_status("已新建文档");
	}

	void ui_editor::impl::handle_command(const std::string& cmd)
	{
		if (cmd == "cmd:new")
		{
			new_document();
		}
		else if (cmd == "cmd:delete")
		{
			delete_selected_node();
		}
		else if (cmd == "cmd:save")
		{
			std::string error;
			if (owner->save_json(json_path_value(), &error))
				set_status("已保存: " + json_path_value());
			else
				set_status(error);
		}
		else if (cmd == "cmd:load")
		{
			std::string error;
			if (owner->load_json(json_path_value(), &error))
				set_status("已载入: " + json_path_value());
			else
				set_status(error);
		}
		else if (cmd == "cmd:export")
		{
			std::string error;
			if (owner->export_cpp(cpp_path_value(), &error))
				set_status("已生成: " + cpp_path_value());
			else
				set_status(error);
		}
		else if (cmd.rfind("add:", 0) == 0)
		{
			try
			{
				int kind = std::stoi(cmd.substr(4));
				if (kind >= VGUI_PANEL && kind <= VGUI_SEPARATOR)
					add_node_of_kind((vgui_widget_kind)kind);
			}
			catch (...)
			{
			}
		}
	}

	std::string ui_editor::impl::json_path_value() const
	{
		const char* text = json_path_edit ? vgui_widget_get_text(json_path_edit) : "";
		return text && *text ? text : default_json_path();
	}

	std::string ui_editor::impl::cpp_path_value() const
	{
		const char* text = cpp_path_edit ? vgui_widget_get_text(cpp_path_edit) : "";
		return text && *text ? text : default_cpp_path();
	}

	/* ---------------------------- editor public API -------------------------- */

	ui_editor::ui_editor()
		: p_(std::make_unique<impl>()), doc_()
	{
		p_->owner = this;
	}

	ui_editor::~ui_editor() = default;

	void ui_editor::build(vgui_context* ctx)
	{
		p_->ctx = ctx;
		p_->build();
	}

	void ui_editor::tick()
	{
		if (!p_->ctx) return;
		p_->sync_page();
		p_->sync_selection_frame();
	}

	ui_node* ui_editor::selected() const
	{
		return p_->selected;
	}

	void ui_editor::select(ui_node* node)
	{
		p_->select(node);
	}

	void ui_editor::impl::select(ui_node* node)
	{
		selected = node;
		dragging = false;
		drag_node = nullptr;
		rebuild_hierarchy();
		rebuild_properties();
		frame_x = frame_y = frame_w = frame_h = -1;
		if (node)
		{
			const char* id = node->id.empty() ? "(unnamed)" : node->id.c_str();
			set_status(std::string("选中: ") + id + " [" + widget_kind_name(node->kind) + "]");
		}
		else
		{
			set_status("未选中控件");
		}
	}

	void ui_editor::add_widget(vgui_widget_kind kind)
	{
		p_->add_node_of_kind(kind);
	}

	void ui_editor::delete_selected()
	{
		p_->delete_selected_node();
	}

	bool ui_editor::save_json(const std::string& path, std::string* error)
	{
		std::string data = doc_.to_json_string(2);
		if (!write_file(path, data, error)) return false;
		return true;
	}

	bool ui_editor::load_json(const std::string& path, std::string* error)
	{
		std::string text;
		if (!read_file(path, &text, error)) return false;
		if (!doc_.from_json_string(text, error)) return false;

		p_->selected = &doc_.root;
		p_->dragging = false;
		p_->drag_node = nullptr;
		p_->rebuild_all();
		if (p_->status_label)
			p_->set_status("已载入: " + path);
		return true;
	}

	bool ui_editor::export_cpp(const std::string& path, std::string* error)
	{
		ui_export_options opt;
		std::string code = ui_generate_cpp(doc_, opt);
		if (!write_file(path, code, error)) return false;
		return true;
	}

	void ui_editor::set_status(const std::string& text)
	{
		p_->set_status(text);
	}

	std::string ui_editor::status() const
	{
		if (!p_->status_label) return {};
		const char* text = vgui_widget_get_text(p_->status_label);
		return text ? text : "";
	}

	std::string ui_editor::json_path() const
	{
		return p_->json_path_value();
	}

	std::string ui_editor::cpp_path() const
	{
		return p_->cpp_path_value();
	}

	void ui_editor::set_json_path(const std::string& path)
	{
		if (p_->json_path_edit) vgui_edit_set_text(p_->json_path_edit, path.c_str());
	}

	void ui_editor::set_cpp_path(const std::string& path)
	{
		if (p_->cpp_path_edit) vgui_edit_set_text(p_->cpp_path_edit, path.c_str());
	}

	void ui_editor::refresh_all()
	{
		p_->rebuild_all();
	}

	void ui_editor::impl::set_status(const std::string& text)
	{
		if (status_label)
			vgui_widget_set_text(status_label, text.c_str());
	}

} /* namespace vgui_editor */
