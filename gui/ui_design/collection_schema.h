#pragma once

#include "model.h"

#include <algorithm>
#include <initializer_list>
#include <set>
#include <utility>

#include <nlohmann/json.hpp>

namespace vgui::ui_design::detail {

inline constexpr std::string_view collection_selection_modes[] = {"none", "single", "multiple"};
inline const property_descriptor collection_scrollable_y = {"scrollable_y", "Scroll vertically", value_type::boolean, true};
inline const property_descriptor collection_scrollbar_width_x = {
    "scrollbar_width_x", "Horizontal scrollbar thickness", value_type::number, 0.0, 0, 1000000};
inline const property_descriptor collection_scrollbar_width_y = {
    "scrollbar_width_y", "Vertical scrollbar thickness", value_type::number, 0.0, 0, 1000000};
inline const property_descriptor collection_scrollbar_inset = {
    "scrollbar_inset", "Scrollbar inset", value_type::number, -1.0, -1, 1000000};
inline const property_descriptor list_properties[] = {
    {"items", "Items (JSON)", value_type::items, std::string{"[]"}},
    {"selection_mode", "Selection mode", value_type::choice, std::string{"single"}, 0, 0, collection_selection_modes},
    {"empty_text", "Empty text", value_type::text, std::string{}},
    {"scrollable_x", "Scroll horizontally", value_type::boolean, false},
    collection_scrollable_y, collection_scrollbar_width_x, collection_scrollbar_width_y, collection_scrollbar_inset,
};
inline const property_descriptor tree_properties[] = {
    list_properties[0], list_properties[1], list_properties[2],
    {"scrollable_x", "Scroll horizontally", value_type::boolean, true},
    collection_scrollable_y, collection_scrollbar_width_x, collection_scrollbar_width_y, collection_scrollbar_inset,
};
inline const property_descriptor menu_properties[] = {
    {"items", "Items (JSON)", value_type::items, std::string{"[]"}},
    {"text", "Menu title", value_type::text, std::string{"Menu"}},
    {"menu_width", "Menu width", value_type::number, 0.0, 0, 1000000},
    {"menu_max_height", "Maximum menu height", value_type::number, 0.0, 0, 1000000},
    {"close_on_activate", "Close on activate", value_type::boolean, true},
};
inline const property_descriptor menubar_properties[] = {
    {"items", "Items (JSON)", value_type::items, std::string{"[]"}},
};

// List rows are strings or {text, enabled?, selected?, detail?, text_color?, tooltip?, icon?, buttons?}.
// Tree rows are {text, enabled?, selected?, expanded?, children?, text_color?, tooltip?, icon?, buttons?}.
// Inline buttons are ordered {icon?, action?} objects; icons are project resource keys in saved documents.
// Menu entries are leaf {text, enabled?, checked?, action?, shortcut?, emoji?, icon?},
// submenu {text, enabled?, children, shortcut?, emoji?, icon?}, or {separator:true};
// shortcut is display-only. Menubar entries are headings {text, enabled?, children}.
// 'children' is always an array. 'checked' (even false) makes a leaf checkable.
// Enabled defaults to true; selected/expanded default to false. In single
// selection mode the last selected row in document order wins.
namespace collection_items {
using json = nlohmann::json;
inline constexpr std::size_t max_bytes = 256 * 1024;
inline constexpr std::size_t max_items = 4096;
inline constexpr std::size_t max_item_depth = 12; // Fits inside the document's JSON nesting limit.
inline constexpr std::size_t max_row_buttons = 4;
inline constexpr std::size_t max_label_bytes = 16384;

enum class shape { list, tree, menu, menubar };
struct invalid { std::string reason; };

[[noreturn]] inline void reject(const std::string& path, std::string_view reason)
{
    throw invalid{path + ": " + std::string(reason)};
}

inline void check_fields(const json& object, const std::string& path,
    std::initializer_list<std::string_view> allowed)
{
    for (auto field = object.begin(); field != object.end(); ++field) {
        if (std::find(allowed.begin(), allowed.end(), field.key()) == allowed.end())
            reject(path, "unknown item member");
    }
}

// The JSON parser validates string encoding (including escaped surrogates).
// It accepts escaped NUL, which the native C-string APIs cannot represent.
inline const std::string& check_text(const json& value, const std::string& path)
{
    if (!value.is_string()) reject(path, "expected a string");
    const auto& text = value.get_ref<const std::string&>();
    if (text.size() > max_label_bytes) reject(path, "text too long");
    if (text.find('\0') != std::string::npos) reject(path, "NUL is not allowed in text");
    return text;
}

inline void check_boolean(const json& item, const char* key, const std::string& path)
{
    const auto field = item.find(key);
    if (field != item.end() && !field->is_boolean())
        reject(path + "." + key, "expected a boolean");
}

inline bool valid_action_key(std::string_view key)
{
    // Match the document's optional button action key syntax.
    return key.empty() || (key.size() <= 64 && key != "." && key != ".." &&
        std::all_of(key.begin(), key.end(), [](unsigned char c) {
            return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
        }));
}

inline void check_action(const json& item, const std::string& path)
{
    if (const auto action = item.find("action"); action != item.end() &&
        (!action->is_string() || !valid_action_key(action->get_ref<const std::string&>())))
        reject(path + ".action", "invalid action key");
}

inline void check_icon(const json& item, const std::string& path)
{
    if (const auto icon = item.find("icon"); icon != item.end() && check_text(*icon, path + ".icon").empty())
        reject(path + ".icon", "expected a nonempty image reference");
}

inline void check_buttons(const json& buttons, const std::string& path)
{
    if (!buttons.is_array()) reject(path, "expected an array");
    if (buttons.size() > max_row_buttons) reject(path, "at most four buttons per row");
    for (std::size_t i = 0; i < buttons.size(); ++i) {
        const auto at = path + "[" + std::to_string(i) + "]";
        const auto& button = buttons[i];
        if (!button.is_object()) reject(at, "expected a button object");
        check_fields(button, at, {"icon", "action"});
        check_icon(button, at);
        check_action(button, at);
    }
}

inline void check_array(const json& items, shape kind, const std::string& path,
    std::size_t depth, std::size_t& count)
{
    if (!items.is_array()) reject(path, "expected an array");
    if (!items.empty() && depth > max_item_depth) reject(path, "item nesting limit exceeded");
    if (items.size() > max_items - count) reject(path, "item count limit exceeded");
    count += items.size();
    for (std::size_t i = 0; i < items.size(); ++i) {
        const auto& item = items[i];
        const auto at = path + "[" + std::to_string(i) + "]";
        if (kind == shape::list && item.is_string()) {
            check_text(item, at);
            continue;
        }
        if (!item.is_object()) reject(at, kind == shape::list ? "expected a string or object" : "expected an object");
        if (kind == shape::menu && item.contains("separator")) {
            check_fields(item, at, {"separator"});
            const auto& separator = item.at("separator");
            if (!separator.is_boolean() || !separator.get<bool>()) reject(at + ".separator", "expected true");
            continue;
        }
        const auto children = item.find("children");
        switch (kind) {
        case shape::list:
            check_fields(item, at, {"text", "enabled", "selected", "detail", "text_color", "tooltip", "icon", "buttons"});
            break;
        case shape::tree:
            check_fields(item, at, {"text", "enabled", "selected", "expanded", "children", "text_color", "tooltip", "icon", "buttons"});
            break;
        case shape::menu:
            if (children != item.end()) check_fields(item, at, {"text", "enabled", "children", "shortcut", "emoji", "icon"});
            else check_fields(item, at, {"text", "enabled", "checked", "action", "shortcut", "emoji", "icon"});
            break;
        case shape::menubar:
            check_fields(item, at, {"text", "enabled", "children"});
            if (children == item.end()) reject(at + ".children", "missing required array");
            break;
        }
        const auto label = item.find("text");
        if (label == item.end()) reject(at + ".text", "missing required text");
        check_text(*label, at + ".text");
        for (const char* key : {"detail", "tooltip", "shortcut", "emoji"}) {
            if (const auto field = item.find(key); field != item.end()) check_text(*field, at + "." + key);
        }
        if (const auto color = item.find("text_color"); color != item.end()) {
            const auto color_path = at + ".text_color";
            if (!parse_text_color(check_text(*color, color_path))) reject(color_path, "expected #RRGGBB or #RRGGBBAA");
        }
        check_boolean(item, "enabled", at);
        check_boolean(item, "selected", at);
        check_boolean(item, "expanded", at);
        check_boolean(item, "checked", at);
        check_action(item, at);
        check_icon(item, at);
        if (const auto buttons = item.find("buttons"); buttons != item.end()) check_buttons(*buttons, at + ".buttons");
        if (children != item.end())
            check_array(*children, kind == shape::menubar ? shape::menu : kind,
                at + ".children", depth + 1, count);
    }
}

inline bool parse(std::string_view type, std::string_view source, json& out, std::string& reason)
{
    const auto kind = type == "list" ? std::optional{shape::list} : type == "tree" ? std::optional{shape::tree} :
        type == "menu" ? std::optional{shape::menu} : type == "menubar" ? std::optional{shape::menubar} : std::nullopt;
    reason.clear();
    try {
        if (!kind) reject("items", "widget type does not support collection items");
        if (source.size() > max_bytes) reject("items", "JSON text too long");
        if (source.find('\0') != source.npos) reject("items", "NUL is not allowed in JSON");
        using event = json::parse_event_t;
        std::vector<std::set<std::string, std::less<>>> keys;
        // Check depth and duplicate keys before DOM insertion. Throw rather
        // than discarding a subtree, so parsing stops at the limit.
        auto items = json::parse(source.begin(), source.end(), [&](int, event what, json& value) {
            switch (what) {
            case event::object_start:
            case event::array_start:
                // Deepest row still has space for its button array and objects.
                if (keys.size() >= 2 * max_item_depth + 2) reject("items", "JSON nesting limit exceeded");
                keys.emplace_back();
                break;
            case event::key:
                if (!keys.back().insert(value.get_ref<const std::string&>()).second)
                    reject("items", "duplicate object member");
                break;
            case event::object_end:
            case event::array_end:
                keys.pop_back();
                break;
            default: break;
            }
            return true;
        });
        std::size_t count = 0;
        check_array(items, *kind, "items", 1, count);
        out = std::move(items);
        return true;
    } catch (const invalid& e) {
        reason = e.reason;
    } catch (const json::parse_error& e) {
        reason = "items: invalid JSON or UTF-8 at byte " + std::to_string(e.byte);
    } catch (const json::exception&) {
        reason = "items: invalid JSON";
    }
    return false;
}
// Visit only after parsing the item schema. The same traversal checks saved
// references and lets the Inspector replace entered paths with resource keys.
template<class Json, class Visitor>
bool visit_icons(Json& items, const std::string& path, const Visitor& visit)
{
    for (std::size_t i = 0; i < items.size(); ++i) {
        auto& item = items[i];
        if (!item.is_object()) continue;
        const auto at = path + "/" + std::to_string(i);
        if (const auto icon = item.find("icon"); icon != item.end() && !visit(*icon, at + "/icon")) return false;
        if (const auto buttons = item.find("buttons"); buttons != item.end()) {
            for (std::size_t b = 0; b < buttons->size(); ++b) {
                auto& button = (*buttons)[b];
                if (const auto icon = button.find("icon"); icon != button.end() &&
                    !visit(*icon, at + "/buttons/" + std::to_string(b) + "/icon")) return false;
            }
        }
        if (const auto children = item.find("children"); children != item.end() &&
            !visit_icons(*children, at + "/children", visit)) return false;
    }
    return true;
}
} // namespace collection_items

} // namespace vgui::ui_design::detail
