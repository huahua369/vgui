#include "model.h"
#include "primitive_schema.h"
#include "collection_schema.h"
#include "gradient_schema.h"
#include "container_schema.h"

#include <array>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <set>
#include <type_traits>
#include <utility>

namespace vgui::ui_design {
namespace {

using sv = std::string_view;
constexpr std::size_t max_text_bytes = 16384;
constexpr std::size_t max_path_bytes = 1024;
constexpr std::size_t max_name_bytes = 256;
constexpr double max_geometry = 1000000;
constexpr sv align_self_choices[] = {"auto", "start", "center", "end", "stretch"};
constexpr sv cursor_choices[] = {"default", "arrow", "text", "wait", "no", "hand", "size_h", "size_v"};
constexpr sv tooltip_anchors[] = {"pointer", "widget"};
constexpr sv focus_choices[] = {"default", "yes", "no"};

const property_descriptor common[] = {
    {"x", "X", value_type::number, 0.0, -max_geometry, max_geometry},
    {"y", "Y", value_type::number, 0.0, -max_geometry, max_geometry},
    {"width", "Width", value_type::number, 0.0, 0, max_geometry},
    {"height", "Height", value_type::number, 0.0, 0, max_geometry},
    {"grow", "Grow", value_type::number, 0.0, 0, max_geometry},
    {"visible", "Visible", value_type::boolean, true},
    {"enabled", "Enabled", value_type::boolean, true},
    {"min_width", "Minimum width", value_type::number, 0.0, 0, max_geometry},
    {"min_height", "Minimum height", value_type::number, 0.0, 0, max_geometry},
    {"max_width", "Maximum width", value_type::number, 0.0, 0, max_geometry},
    {"max_height", "Maximum height", value_type::number, 0.0, 0, max_geometry},
    {"margin_left", "Left margin", value_type::number, 0.0, 0, max_geometry},
    {"margin_top", "Top margin", value_type::number, 0.0, 0, max_geometry},
    {"margin_right", "Right margin", value_type::number, 0.0, 0, max_geometry},
    {"margin_bottom", "Bottom margin", value_type::number, 0.0, 0, max_geometry},
    {"align_self", "Align self", value_type::choice, std::string("auto"), 0, 0, align_self_choices},
    {"background", "Background", value_type::color, std::string{}},
    {"border_color", "Border color", value_type::color, std::string{}},
    {"border_width", "Border width", value_type::number, 0.0, 0, 256},
    {"rounding", "Corner radius", value_type::number, 0.0, 0, max_geometry},
    {"opacity", "Opacity", value_type::number, 1.0, 0, 1},
    {"input_enabled", "Accept input", value_type::boolean, true},
    {"focusable", "Keyboard focus", value_type::choice, std::string("default"), 0, 0, focus_choices},
    {"focus_alpha", "Focus ring opacity", value_type::number, 1.0, 0, 1},
    {"cursor", "Mouse cursor", value_type::choice, std::string("default"), 0, 0, cursor_choices},
    {"tooltip", "Tooltip", value_type::text, std::string{}},
    {"tooltip_enabled", "Enable tooltip", value_type::boolean, true},
    {"tooltip_delay", "Tooltip delay (s)", value_type::number, -1.0, -1, 60},
    {"tooltip_anchor", "Tooltip anchor", value_type::choice, std::string("pointer"), 0, 0, tooltip_anchors},
    {"tooltip_when_disabled", "Tooltip even when disabled", value_type::boolean, true},
    {"a11y_name", "Accessible name", value_type::text, std::string{}},
};
constexpr sv layouts[] = {"absolute", "row", "column", "wrap"};
constexpr sv alignment[] = {"start", "center", "end", "stretch"};
constexpr sv justification[] = {"start", "center", "end", "space-between", "space-around"};
const property_descriptor panel[] = {
    {"layout", "Layout", value_type::choice, std::string("absolute"), 0, 0, layouts},
    {"align_items", "Align items", value_type::choice, std::string("start"), 0, 0, alignment},
    {"justify", "Justify", value_type::choice, std::string("start"), 0, 0, justification},
    {"gap", "Gap", value_type::number, 0.0, 0, max_geometry},
    {"pad_left", "Left padding", value_type::number, 0.0, 0, max_geometry},
    {"pad_top", "Top padding", value_type::number, 0.0, 0, max_geometry},
    {"pad_right", "Right padding", value_type::number, 0.0, 0, max_geometry},
    {"pad_bottom", "Bottom padding", value_type::number, 0.0, 0, max_geometry},
};
constexpr sv font_weights[] = {"default", "regular", "medium", "semi_bold", "bold"};
constexpr sv font_slants[] = {"default", "normal", "italic", "oblique"};
const property_descriptor font_family = {"font_family", "Font family", value_type::font_key, std::string{}};
const property_descriptor label[] = {
    {"text", "Text", value_type::text, std::string{}},
    {"text_key", "Localization key", value_type::localization_key, std::string{}},
    {"font_size", "Font size", value_type::number, 0.0, 0.0, 256.0},
    {"font_weight", "Font weight", value_type::choice, std::string("default"), 0, 0, font_weights},
    {"font_slant", "Font slant", value_type::choice, std::string("default"), 0, 0, font_slants},
    {"text_color", "Text color", value_type::color, std::string{}},
    font_family,
};
const property_descriptor button[] = {
    label[0], label[1], {"action", "Action", value_type::action_key, std::string{}},
    {"atlas", "Button atlas path", value_type::resource_key, std::string{}},
    label[2], label[3], label[4], label[5], font_family,
};
const property_descriptor edit_box[] = {
    label[0], label[1], {"placeholder", "Placeholder", value_type::text, std::string{}},
    label[2], label[3], label[4], label[5], font_family,
};
const property_descriptor image[] = {
    {"resource", "Resource key", value_type::resource_key, std::string{}},
};
template <class... Groups>
auto properties(const Groups&... groups)
{
    std::array<property_descriptor, (std::extent_v<Groups> + ...)> result;
    auto next = result.begin();
    ((next = std::copy(std::begin(groups), std::end(groups), next)), ...);
    return result;
}

const auto panel_properties = properties(panel, detail::extra_panel_properties, detail::page_properties);
const auto label_properties = properties(label, detail::extra_label_properties);
const auto button_properties = properties(button, detail::extra_button_properties, detail::extra_button_gradient_properties);
const auto edit_properties = properties(edit_box, detail::extra_edit_properties);
const auto image_properties = properties(image, detail::extra_image_properties);
const type_descriptor descriptors[] = {
    {"panel", "Panel", true, panel_properties, 240, 160},
    {"label", "Label", false, label_properties, 120, 24},
    {"button", "Button", false, button_properties, 120, 36},
    {"edit", "Edit", false, edit_properties, 200, 36},
    {"image", "Image", false, image_properties, 128, 128},
    {"gradient", "Gradient", false, detail::gradient_properties, 160, 90},
    {"checkbox", "Checkbox", false, detail::checkbox_properties, 144, 28},
    {"radio", "Radio Button", false, detail::radio_properties, 144, 28},
    {"switch", "Switch", false, detail::switch_properties, 144, 28},
    {"slider", "Slider", false, detail::slider_properties, 180, 28},
    {"progress", "Progress Bar", false, detail::progress_properties, 180, 24},
    {"separator", "Separator", false, detail::separator_properties, 160, 8},
    {"list", "List", false, detail::list_properties, 210, 165},
    {"tree", "Tree", false, detail::tree_properties, 210, 180},
    {"menu", "Menu", false, detail::menu_properties, 120, 32},
    {"menubar", "Menu Bar", false, detail::menubar_properties, 240, 32},
    {"splitter", "Splitter", true, detail::splitter_properties, 300, 180, 2},
    {"tabview", "Tab View", true, detail::tabview_properties, 300, 180, max_nodes, true},
    {"dock", "Dock", true, detail::dock_properties, 400, 240, max_nodes, true},
};

bool fail(diagnostic* error, std::string path, std::string message)
{
    if (error) *error = {std::move(path), std::move(message)};
    return false;
}

std::string pointer_token(sv key)
{
    std::string result;
    for (char ch : key) {
        if (ch == '~') result += "~0";
        else if (ch == '/') result += "~1";
        else result += ch;
    }
    return result;
}

std::string node_path(sv id) { return "/nodes/" + pointer_token(id); }

bool is_key(sv key)
{
    return !key.empty() && key.size() <= 64 && key != "." && key != ".." &&
        std::all_of(key.begin(), key.end(), [](unsigned char c) {
            return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
        });
}

// Reject NUL (also invalid for VGUI's eventual C string projection), truncated
// encodings, overlong scalars, surrogates and values above U+10FFFF.
bool valid_text(sv text, std::size_t limit)
{
    if (text.size() > limit) return false;
    const auto byte = [&](std::size_t i) { return static_cast<unsigned char>(text[i]); };
    for (std::size_t i = 0; i < text.size();) {
        const auto c = byte(i++);
        if (c == 0) return false;
        if (c < 0x80) continue;
        const std::size_t extra = c >= 0xC2 && c <= 0xDF ? 1 :
            c >= 0xE0 && c <= 0xEF ? 2 : c >= 0xF0 && c <= 0xF4 ? 3 : 0;
        if (!extra || extra > text.size() - i) return false;
        const auto first = byte(i);
        if ((c == 0xE0 && first < 0xA0) || (c == 0xED && first >= 0xA0) ||
            (c == 0xF0 && first < 0x90) || (c == 0xF4 && first >= 0x90)) return false;
        for (std::size_t n = 0; n < extra; ++n)
            if ((byte(i++) & 0xC0) != 0x80) return false;
    }
    return true;
}

bool valid_resource_path(sv path)
{
    if (path.empty() || !valid_text(path, max_path_bytes)) return false;
    // Store portable slash-separated relative names, not host paths or URIs.
    for (unsigned char c : path)
        if (c < 32 || c == 127 || sv("\\:<>\"|?*").find(c) != sv::npos) return false;
    for (;;) {
        const auto end = path.find('/');
        const auto part = path.substr(0, end);
        if (part.empty() || part.back() == '.' || part.back() == ' ') return false;
        if (end == sv::npos) return true;
        path.remove_prefix(end + 1);
    }
}

bool check_value(const document& doc, const property_descriptor& desc, const value& val,
    const std::string& path, diagnostic* error)
{
    if (desc.type == value_type::boolean)
        return std::holds_alternative<bool>(val) || fail(error, path, "expected a boolean");
    if (desc.type == value_type::number) {
        const auto* n = std::get_if<double>(&val);
        if (!n || !std::isfinite(*n) || *n < desc.min || *n > desc.max)
            return fail(error, path, "number outside valid range");
        // 0 inherits the context font. 4 logical pixels still produce at least
        // one device pixel at the runtime's smallest supported scale (0.25).
        return desc.key != "font_size" || *n == 0 || *n >= 4 ||
            fail(error, path, "font size must be 0 (default) or at least 4");
    }
    const auto* text = std::get_if<std::string>(&val);
    if (!text) return fail(error, path, "expected a string");
    if (desc.key == "radio_group")
        return (valid_text(*text, max_name_bytes) && std::none_of(text->begin(), text->end(), [](unsigned char c) {
            return c < 32 || c == 127;
        })) || fail(error, path, "expected a single-line UTF-8 group name of at most 256 bytes");
    if (desc.type == value_type::text)
        return valid_text(*text, max_text_bytes) || fail(error, path, "invalid or too long UTF-8 text");
    if (desc.type == value_type::color)
        return text->empty() || parse_text_color(*text).has_value() ||
            fail(error, path, "expected #RRGGBB or #RRGGBBAA");
    if (desc.type == value_type::choice)
        return std::find(desc.choices.begin(), desc.choices.end(), *text) != desc.choices.end() ||
            fail(error, path, "unknown choice");
    if (text->empty()) return true;
    if (!is_key(*text)) return fail(error, path, "invalid key");
    if (desc.type == value_type::font_key && !is_builtin_font(*text)) {
        const auto resource = doc.resources.find(*text);
        if (resource == doc.resources.end()) return fail(error, path, "unknown font resource key");
        return is_font_path(resource->second) || fail(error, path, "font resource must be a .ttf or .otf file");
    }
    return desc.type != value_type::resource_key || doc.resources.contains(*text) ||
        fail(error, path, "unknown resource key");
}

// These helpers operate on the valid tree topology maintained after each edit.
// Other fields and depth are validated once against the entire batch.
auto find_parent(document& doc, sv id)
{
    return std::find_if(doc.nodes.begin(), doc.nodes.end(), [&](const auto& entry) {
        const auto& children = entry.second.children;
        return std::find(children.begin(), children.end(), id) != children.end();
    });
}

std::vector<std::string> subtree(const document& doc, const std::string& id)
{
    std::vector<std::string> result{id};
    for (std::size_t i = 0; i < result.size(); ++i) {
        const auto& children = doc.nodes.at(result[i]).children;
        result.insert(result.end(), children.begin(), children.end());
    }
    return result;
}

bool insert_child(node& parent, const std::string& id, std::size_t index)
{
    if (index == append) index = parent.children.size();
    if (index > parent.children.size()) return false;
    parent.children.insert(parent.children.begin() + index, id);
    return true;
}

bool apply_one(document& doc, const edit& operation, diagnostic* error)
{
    return std::visit([&](const auto& op) -> bool {
        using T = std::decay_t<decltype(op)>;
        if constexpr (std::is_same_v<T, set_resource>) {
            const auto path = "/resources/" + pointer_token(op.key);
            if (!is_key(op.key)) return fail(error, path, "invalid resource key");
            if (op.path) {
                if (!valid_resource_path(*op.path)) return fail(error, path, "invalid project-relative path");
                if (doc.resources.size() >= max_resources && !doc.resources.contains(op.key))
                    return fail(error, "/resources", "too many resources");
                doc.resources.insert_or_assign(op.key, *op.path);
            } else doc.resources.erase(op.key);
            return true;
        } else {
            const auto path = node_path(op.id);
            auto found = doc.nodes.find(op.id);
            if constexpr (std::is_same_v<T, insert_node> || std::is_same_v<T, move_node> ||
                std::is_same_v<T, copy_subtree>) {
                auto parent = doc.nodes.find(op.parent);
                const auto* type = parent == doc.nodes.end() ? nullptr : find_type(parent->second.type);
                if (!type || !type->container)
                    return fail(error, node_path(op.parent), "parent must be a container");
                if constexpr (std::is_same_v<T, insert_node>) {
                    if (!is_key(op.id) || found != doc.nodes.end()) return fail(error, path, "invalid or duplicate ID");
                    if (!op.data.children.empty()) return fail(error, path + "/children", "insert one node at a time");
                    if (!find_type(op.data.type)) return fail(error, path + "/type", "unknown node type");
                    if (doc.nodes.size() >= max_nodes) return fail(error, "/nodes", "too many nodes");
                    if (!insert_child(parent->second, op.id, op.index))
                        return fail(error, node_path(op.parent) + "/children", "invalid insertion index");
                    doc.nodes.emplace(op.id, op.data);
                    return true;
                } else {
                    if (found == doc.nodes.end()) return fail(error, path, "unknown node");
                    const auto ids = subtree(doc, op.id);
                    if constexpr (std::is_same_v<T, move_node>) {
                        if (op.id == doc.root) return fail(error, path, "cannot move the root");
                        if (std::find(ids.begin(), ids.end(), op.parent) != ids.end())
                            return fail(error, node_path(op.parent), "move would create a cycle");
                        auto& children = find_parent(doc, op.id)->second.children;
                        children.erase(std::find(children.begin(), children.end(), op.id));
                        return insert_child(parent->second, op.id, op.index) ||
                            fail(error, node_path(op.parent) + "/children", "invalid insertion index");
                    } else {
                        if (ids.size() != op.ids.size()) return fail(error, path, "incomplete subtree ID mapping");
                        if (ids.size() > max_nodes - doc.nodes.size()) return fail(error, "/nodes", "too many nodes");
                        std::set<std::string_view> new_ids;
                        for (const auto& id : ids) {
                            const auto it = op.ids.find(id);
                            if (it == op.ids.end() || !is_key(it->second) || doc.nodes.contains(it->second) ||
                                !new_ids.insert(it->second).second)
                                return fail(error, node_path(id), "invalid or duplicate copy ID");
                        }
                        if (op.index != append && op.index > parent->second.children.size())
                            return fail(error, node_path(op.parent) + "/children", "invalid insertion index");
                        // Copy before inserting into the destination's children: the
                        // destination may itself belong to the source subtree.
                        for (const auto& id : ids) {
                            auto item = doc.nodes.at(id);
                            for (auto& child : item.children) child = op.ids.at(child);
                            doc.nodes.emplace(op.ids.at(id), std::move(item));
                        }
                        insert_child(parent->second, op.ids.at(op.id), op.index);
                        return true;
                    }
                }
            } else {
                if (found == doc.nodes.end()) return fail(error, path, "unknown node");
                auto& item = found->second;
                if constexpr (std::is_same_v<T, erase_node>) {
                    if (op.id == doc.root) return fail(error, path, "cannot erase the root");
                    auto& children = find_parent(doc, op.id)->second.children;
                    children.erase(std::find(children.begin(), children.end(), op.id));
                    for (const auto& id : subtree(doc, op.id)) doc.nodes.erase(id);
                } else if constexpr (std::is_same_v<T, rename_node>) {
                    item.name = op.name;
                } else {
                    if (!find_property(item.type, op.key))
                        return fail(error, path + "/props/" + pointer_token(op.key), "unknown property");
                    if (op.data) item.props.insert_or_assign(op.key, *op.data);
                    else item.props.erase(op.key);
                }
                return true;
            }
        }
    }, operation);
}

std::optional<document> apply_batch(const document& base, std::span<const edit> edits, diagnostic* error)
{
    if (error) *error = {};
    document candidate = base;
    for (const auto& op : edits)
        if (!apply_one(candidate, op, error)) return std::nullopt;
    if (!validate(candidate, error)) return std::nullopt;
    return candidate;
}

} // namespace

bool is_font_path(sv path)
{
    const auto slash = path.find_last_of("/\\");
    if (slash != sv::npos) path.remove_prefix(slash + 1);
    // A leading dot alone doesn't introduce an extension (as in export).
    if (path.size() <= 4 || path[path.size() - 4] != '.') return false;
    const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c; };
    const auto ext = path.substr(path.size() - 3);
    return (lower(ext[0]) == 't' || lower(ext[0]) == 'o') && lower(ext[1]) == 't' && lower(ext[2]) == 'f';
}

bool is_builtin_font(sv key)
{
    return key == "builtin.sans" || key == "builtin.sans_sc";
}

std::optional<std::uint32_t> parse_text_color(sv text)
{
    if ((text.size() != 7 && text.size() != 9) || text.front() != '#') return std::nullopt;
    const auto* end = text.data() + text.size();
    std::uint32_t color = 0;
    const auto parsed = std::from_chars(text.data() + 1, end, color, 16);
    if (parsed.ec != std::errc{} || parsed.ptr != end) return std::nullopt;
    if (text.size() == 7) return 0xff000000u | color;
    return (color >> 8) | ((color & 0xffu) << 24);
}

bool decode_gradient_stops(sv input, std::vector<gradient_stop>& out, std::string& error)
{
    using json = detail::collection_items::json;
    const auto reject = [&](const char* reason) { error = reason; return false; };
    if (input.size() > 4096) return reject("color stop data is too long");
    const auto stops = json::parse(input, nullptr, false);
    if (!stops.is_array() || stops.size() < 2 || stops.size() > max_gradient_stops)
        return reject("expected 2–32 color stops");
    std::vector<gradient_stop> next;
    for (const auto& stop : stops) {
        if (!stop.is_object() || stop.size() != 2 || !stop.contains("offset") || !stop["offset"].is_number() ||
            !stop.contains("color") || !stop["color"].is_string()) return reject("expected {offset, color} for each stop");
        const double n = stop["offset"].get<double>();
        const auto offset = static_cast<float>(n);
        const auto& color = stop["color"].get_ref<const std::string&>();
        if (!(n >= 0 && n <= 1) || (!next.empty() && offset - next.back().offset < 1e-4f))
            return reject("color stop offsets must be ordered and at least 0.0001 apart in [0, 1]");
        if (!parse_text_color(color)) return reject("expected #RRGGBB or #RRGGBBAA for each stop color");
        next.push_back({offset, color});
    }
    out = std::move(next);
    error.clear();
    return true;
}

std::string encode_gradient_stops(std::span<const gradient_stop> stops)
{
    using json = detail::collection_items::json;
    auto array = json::array();
    for (const auto& stop : stops) array.push_back({{"offset", stop.offset}, {"color", stop.color}});
    return array.dump();
}

std::span<const type_descriptor> types() { return descriptors; }
std::span<const property_descriptor> common_properties() { return common; }

const type_descriptor* find_type(sv type)
{
    for (const auto& desc : types()) if (desc.key == type) return &desc;
    return nullptr;
}

const property_descriptor* find_property(sv type, sv key)
{
    const auto* desc = find_type(type);
    if (!desc) return nullptr;
    for (const auto& prop : desc->properties) if (prop.key == key) return &prop;
    for (const auto& prop : common_properties()) if (prop.key == key) return &prop;
    return nullptr;
}

bool accepts_child(const type_descriptor& parent, std::string_view child_type, std::size_t existing_children)
{
    return parent.container && existing_children < parent.max_children &&
        (!parent.panel_children_only || child_type == "panel");
}

document make_document()
{
    document doc;
    doc.root = "root";
    doc.nodes.emplace(doc.root, node{"panel", "Root", {{"background", std::string("#00000000")}}, {}});
    return doc;
}

bool prepare_items_edit(const document& doc, sv type, sv input, std::string& result,
    std::vector<edit>& batch, std::string& reason)
{
    namespace collection = detail::collection_items;
    collection::json items;
    if (!collection::parse(type, input, items, reason)) return false;
    std::vector<edit> resources;
    std::optional<std::map<std::string, std::string, std::less<>>> keys_by_path;
    std::size_t sequence = 0;
    if (!collection::visit_icons(items, "items", [&](collection::json& icon, const std::string& path) {
        const auto& image = icon.get_ref<const std::string&>();
        if (doc.resources.contains(image)) return true;
        if (!valid_resource_path(image)) {
            reason = path + ": expected an existing resource key or a project-relative image path";
            return false;
        }
        if (!keys_by_path) {
            keys_by_path.emplace();
            for (const auto& [key, file] : doc.resources) keys_by_path->try_emplace(file, key);
        }
        auto key = keys_by_path->find(image);
        if (key == keys_by_path->end()) {
            if (doc.resources.size() + resources.size() >= max_resources) {
                reason = path + ": too many resources";
                return false;
            }
            std::string generated;
            do generated = "image_" + std::to_string(++sequence);
            while (doc.resources.contains(generated));
            key = keys_by_path->emplace(image, generated).first;
            resources.emplace_back(set_resource{std::move(generated), image});
        }
        icon = key->second;
        return true;
    })) return false;
    result = items.dump();
    batch = std::move(resources);
    return true;
}

bool validate(const document& doc, diagnostic* error)
{
    if (error) *error = {};
    if (doc.nodes.size() > max_nodes) return fail(error, "/nodes", "too many nodes");
    if (doc.resources.size() > max_resources) return fail(error, "/resources", "too many resources");
    const auto root = doc.nodes.find(doc.root);
    if (root == doc.nodes.end() || root->second.type != "panel")
        return fail(error, "/root", "root must refer to an existing panel");
    for (const auto& [key, path] : doc.resources) {
        if (!is_key(key) || !valid_resource_path(path))
            return fail(error, "/resources/" + pointer_token(key), "invalid resource key or project-relative path");
    }

    std::set<sv> parented;
    for (const auto& [id, item] : doc.nodes) {
        const auto path = node_path(id);
        if (!is_key(id)) return fail(error, path, "invalid node ID");
        const auto* type = find_type(item.type);
        if (!type) return fail(error, path + "/type", "unknown node type");
        if (!valid_text(item.name, max_name_bytes)) return fail(error, path + "/name", "invalid or too long UTF-8 name");
        for (const auto& [key, val] : item.props) {
            const auto prop_path = path + "/props/" + pointer_token(key);
            const auto* prop = find_property(item.type, key);
            if (!prop) return fail(error, prop_path, "unknown property");
            if (prop->type == value_type::items) {
                const auto* source = std::get_if<std::string>(&val);
                std::string reason;
                if (!source) return fail(error, prop_path, "expected an item array");
                namespace collection = detail::collection_items;
                collection::json items;
                if (!collection::parse(item.type, *source, items, reason)) return fail(error, prop_path, std::move(reason));
                if (!collection::visit_icons(items, prop_path, [&](const collection::json& icon, const std::string& icon_path) {
                    const auto& image = icon.get_ref<const std::string&>();
                    return (is_key(image) && doc.resources.contains(image)) ||
                        fail(error, icon_path, "unknown image resource key");
                })) return false;
            } else if (prop->type == value_type::gradient_stops) {
                const auto* source = std::get_if<std::string>(&val);
                std::vector<gradient_stop> stops;
                std::string reason;
                if (!source) return fail(error, prop_path, "expected a color stop array");
                if (!decode_gradient_stops(*source, stops, reason)) return fail(error, prop_path, std::move(reason));
            } else if (!check_value(doc, *prop, val, prop_path, error)) return false;
        }
        const auto n = [&](std::string_view key) {
            const auto it = item.props.find(key);
            return std::get<double>(it == item.props.end() ? find_property(item.type, key)->default_value : it->second);
        };
        if ((n("max_width") > 0 && n("min_width") > n("max_width")) ||
            (n("max_height") > 0 && n("min_height") > n("max_height")))
            return fail(error, path + "/props", "maximum size must be at least minimum size, or zero for no limit");
        if (item.type == "gradient") {
            const auto f = [&](sv key) { return static_cast<float>(n(key)); };
            const auto choice = [&](sv key) -> const std::string& {
                const auto it = item.props.find(key);
                return std::get<std::string>(it == item.props.end() ? find_property(item.type, key)->default_value : it->second);
            };
            const auto& kind = choice("gradient_kind");
            if (kind == "linear" && std::hypot(f("gradient_x1") - f("gradient_x0"), f("gradient_y1") - f("gradient_y0")) < 1e-4f)
                return fail(error, path + "/props/gradient_x1", "linear start and end points must be different");
            if (kind == "radial" && f("gradient_radius1") <= f("gradient_radius0"))
                return fail(error, path + "/props/gradient_radius1", "outer radius must be larger than inner radius");
            if (kind == "sweep" && std::abs(f("gradient_angle1") - f("gradient_angle0")) < 0.001f)
                return fail(error, path + "/props/gradient_angle1", "sweep angles must be different");
            if (std::abs(f("gradient_affine_xx") * f("gradient_affine_yy") - f("gradient_affine_yx") * f("gradient_affine_xy")) < 1e-4f)
                return fail(error, path + "/props/gradient_affine_xx", "gradient transform must be invertible");
            if (choice("gradient_animation") == "shine") {
                const auto within = [&](sv axis, sv travel) {
                    return std::abs(n(axis)) + std::abs(n(travel)) * 0.5 <= detail::gradient_limit;
                };
                if (!within("gradient_affine_tx", "gradient_animation_dx") || !within("gradient_affine_ty", "gradient_animation_dy"))
                    return fail(error, path + "/props/gradient_animation", "shine travel exceeds the supported gradient translation range");
            }
        }
        if (item.type == "slider" || item.type == "progress") {
            const auto low = static_cast<float>(n("min_value")), high = static_cast<float>(n("max_value"));
            if (!(low < high)) return fail(error, path + "/props/min_value", "minimum must be less than maximum");
            if (item.props.contains("value") && (static_cast<float>(n("value")) < low || static_cast<float>(n("value")) > high))
                return fail(error, path + "/props/value", "value outside configured range");
            if (n("step") > double(high) - low)
                return fail(error, path + "/props/step", "step must not exceed the configured range");
        }
        if (!type->container && !item.children.empty()) return fail(error, path + "/children", "node cannot contain children");
        std::set<sv> checked_radio_groups;
        for (std::size_t i = 0; i < item.children.size(); ++i) {
            const auto& child = item.children[i];
            const auto child_path = path + "/children/" + std::to_string(i);
            const auto target = doc.nodes.find(child);
            if (child == doc.root || target == doc.nodes.end() || !parented.insert(child).second)
                return fail(error, child_path, "child is missing, is the root, or already has a parent");
            if (!accepts_child(*type, target->second.type, i))
                return fail(error, child_path, "child type or count is not supported by this container");
            if (target->second.type == "radio") {
                const auto flag = target->second.props.find("checked");
                const bool* checked = flag == target->second.props.end() ? nullptr : std::get_if<bool>(&flag->second);
                if (checked && *checked) {
                    const auto name = target->second.props.find("radio_group");
                    const auto* group = name == target->second.props.end() ? nullptr : std::get_if<std::string>(&name->second);
                    // A child may not have had its own property types checked yet.
                    if (name != target->second.props.end() && !group) continue;
                    if (!checked_radio_groups.emplace(group ? sv(*group) : sv{}).second)
                        return fail(error, node_path(child) + "/props/checked", "only one radio in a group can be initially checked under the same parent");
                }
            }
        }
    }

    // Each non-root node can have only one incoming edge; any cycle left
    // after the root-parent check must be outside the root's reachable tree.
    std::size_t reached = 0;
    auto visit = [&](auto&& self, const auto& entry, std::size_t depth) -> bool {
        if (depth > max_tree_depth) return fail(error, node_path(entry.first), "tree depth limit exceeded");
        ++reached;
        for (const auto& child : entry.second.children)
            if (!self(self, *doc.nodes.find(child), depth + 1)) return false;
        return true;
    };
    if (!visit(visit, *root, 1)) return false;
    if (reached != doc.nodes.size()) {
        for (const auto& [id, item] : doc.nodes)
            if (id != doc.root && !parented.contains(id)) return fail(error, node_path(id), "orphan node");
        return fail(error, "/nodes", "cycle outside the root tree");
    }
    return true;
}

session::session() : doc_(make_document()) {}

bool session::apply(std::span<const edit> edits, diagnostic* error)
{
    if (edits.empty()) {
        if (error) *error = {};
        finish_live();
        return true;
    }
    auto candidate = apply_batch(current(), edits, error);
    if (!candidate) return false;
    finish_live();
    if (*candidate != doc_) {
        record(std::move(*candidate));
        ++revision_;
    }
    return true;
}

bool session::apply_live(std::span<const edit> edits, diagnostic* error)
{
    if (edits.empty()) {
        if (error) *error = {};
        finish_live(false);
        return true;
    }
    auto candidate = apply_batch(doc_, edits, error);
    if (!candidate) return false;
    if (*candidate == doc_) finish_live(false);
    else if (!live_ || *candidate != *live_) {
        live_ = std::move(*candidate);
        ++revision_;
    }
    return true;
}

void session::finish_live(bool commit)
{
    if (!live_) return;
    if (commit) record(std::move(*live_));
    else ++revision_;
    live_.reset();
}

void session::record(document&& candidate)
{
    transaction tx;
    auto diff = [](const auto& before, const auto& after, auto& changes) {
        auto b = before.begin(), a = after.begin();
        while (b != before.end() || a != after.end()) {
            if (a == after.end() || (b != before.end() && b->first < a->first)) {
                changes.push_back({b->first, b->second, std::nullopt});
                ++b;
            } else if (b == before.end() || a->first < b->first) {
                changes.push_back({a->first, std::nullopt, a->second});
                ++a;
            } else {
                if (b->second != a->second) changes.push_back({a->first, b->second, a->second});
                ++a;
                ++b;
            }
        }
    };
    diff(doc_.nodes, candidate.nodes, tx.nodes);
    diff(doc_.resources, candidate.resources, tx.resources);
    tx.before = state_;
    tx.after = next_state_ + 1;
    // Appending may allocate, but this branch has no redo to discard.
    if (cursor_ == history_.size()) history_.push_back(std::move(tx));
    else {
        history_[cursor_] = std::move(tx);
        history_.resize(cursor_ + 1);
    }
    ++cursor_;
    doc_ = std::move(candidate);
    state_ = ++next_state_;
}

void session::replay(const transaction& tx, bool forward)
{
    document candidate = doc_;
    auto write = [forward](auto& map, const auto& changes) {
        for (const auto& diff : changes) {
            const auto& val = forward ? diff.after : diff.before;
            if (val) map.insert_or_assign(diff.key, *val);
            else map.erase(diff.key);
        }
    };
    write(candidate.nodes, tx.nodes);
    write(candidate.resources, tx.resources);
    doc_ = std::move(candidate);
    state_ = forward ? tx.after : tx.before;
    ++revision_;
}

bool session::undo()
{
    if (!can_undo()) return false;
    finish_live();
    replay(history_[cursor_ - 1], false);
    --cursor_;
    return true;
}

bool session::redo()
{
    if (!can_redo()) return false;
    replay(history_[cursor_], true);
    ++cursor_;
    return true;
}

bool session::load(const std::filesystem::path& path, diagnostic* error)
{
    document candidate;
    if (!read_document(path, candidate, error)) return false;
    live_.reset();
    doc_ = std::move(candidate);
    history_.clear();
    cursor_ = 0;
    state_ = saved_state_ = ++next_state_;
    ++revision_;
    return true;
}

bool session::save(const std::filesystem::path& path, diagnostic* error)
{
    if (!write_document(path, current(), error)) return false;
    finish_live();
    saved_state_ = state_;
    return true;
}

} // namespace vgui::ui_design
