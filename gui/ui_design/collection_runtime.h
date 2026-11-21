#pragma once

#include "collection_schema.h"

#include <vgui/vgui.h>

namespace vgui::ui_design::detail {

// The native kind of the collection itself (not necessarily its layout host).
inline std::optional<vgui_widget_kind> collection_kind(std::string_view type)
{
    if (type == "list") return VGUI_LIST;
    if (type == "tree") return VGUI_TREE;
    if (type == "menu") return VGUI_MENU;
    if (type == "menubar") return VGUI_MENUBAR;
    return std::nullopt;
}

inline std::optional<vgui_widget_kind> collection_mount_kind(std::string_view type)
{
    // Menus are context-level popups, not layout children. A standalone menu
    // document node is a one-title menubar that triggers and owns the popup.
    if (type == "menu") return VGUI_MENUBAR;
    return collection_kind(type);
}

namespace collection_projection {
using collection_items::json;

inline const char* c_text(const json& text)
{
    // JSON owns the string for the duration of projection; VGUI copies it.
    return text.get_ref<const std::string&>().c_str();
}

inline vgui_selection_mode selection_mode(std::string_view mode)
{
    return mode == "none" ? VGUI_SELECTION_NONE :
        mode == "multiple" ? VGUI_SELECTION_MULTIPLE : VGUI_SELECTION_SINGLE;
}

template<class Access>
void apply_scroll(vgui_widget* widget, const Access& props)
{
    if (props.has("scrollable_x") || props.has("scrollable_y"))
        vgui_panel_set_scrollable(widget, props.boolean("scrollable_x"), props.boolean("scrollable_y"));
    if (props.has("scrollbar_width_x") || props.has("scrollbar_width_y") || props.has("scrollbar_inset"))
        vgui_panel_set_scrollbar_size(widget, props.number("scrollbar_width_x"),
            props.number("scrollbar_width_y"), props.number("scrollbar_inset"));
}

inline vgui_color row_text_color(const json& item)
{
    if (const auto field = item.find("text_color"); field != item.end()) {
        if (const auto color = parse_text_color(field->get_ref<const std::string&>()))
            return *color ? *color : 1u; // Native zero inherits the theme; preserve explicit transparent black.
    }
    return 0;
}

template<class Load>
vgui_texture* item_icon(const json& item, const std::string& path, const Load& load_icon)
{
    const auto icon = item.find("icon");
    return icon == item.end() ? nullptr : load_icon(icon->get_ref<const std::string&>(), path + "/icon");
}

static_assert(collection_items::max_row_buttons == VGUI_MAX_ROW_BUTTONS);

template<class Load, class Bind>
void apply_row_decorations(vgui_widget* widget, vgui_item_id id, const json& item, const std::string& path,
    const Load& load_icon, const Bind& bind_row_action)
{
    const bool tree = vgui_widget_get_kind(widget) == VGUI_TREE;
    const auto set_icon = tree ? vgui_tree_set_item_icon : vgui_list_set_item_icon;
    if (auto* icon = item_icon(item, path, load_icon)) set_icon(widget, id, icon);

    const auto buttons = item.find("buttons");
    if (buttons == item.end() || buttons->empty()) return;
    const auto set_buttons = tree ? vgui_tree_set_item_buttons : vgui_list_set_item_buttons;
    const auto set_button_icon = tree ? vgui_tree_set_item_button_icon : vgui_list_set_item_button_icon;
    set_buttons(widget, id, static_cast<int>(buttons->size()));
    for (int b = 0; const auto& button : *buttons) {
        if (auto* icon = item_icon(button, path + "/buttons/" + std::to_string(b), load_icon))
            set_button_icon(widget, id, b, icon);
        if (const auto action = button.find("action"); action != button.end())
            bind_row_action(id, b, action->get_ref<const std::string&>());
        ++b;
    }
}

template<class Load, class Bind>
bool add_list_rows(vgui_widget* list, const json& items, const std::string& path,
    const Load& load_icon, const Bind& bind_row_action)
{
    for (std::size_t i = 0; i < items.size(); ++i) {
        const auto& item = items[i];
        const auto id = vgui_list_add_item(list, c_text(item.is_string() ? item : item.at("text")), nullptr);
        if (!id) return false;
        if (item.is_object()) {
            vgui_list_set_item_enabled(list, id, item.value("enabled", true));
            if (item.value("selected", false)) vgui_list_select(list, id, 1);
            if (const auto detail = item.find("detail"); detail != item.end()) vgui_list_set_item_detail(list, id, c_text(*detail));
            if (const auto color = row_text_color(item)) vgui_list_set_item_text_color(list, id, color);
            if (const auto tooltip = item.find("tooltip"); tooltip != item.end()) vgui_list_set_item_tooltip(list, id, c_text(*tooltip));
            apply_row_decorations(list, id, item, path + "/" + std::to_string(i), load_icon, bind_row_action);
        }
    }
    return true;
}

template<class Load, class Bind>
bool add_tree_rows(vgui_widget* tree, vgui_item_id parent, const json& items, const std::string& path,
    const Load& load_icon, const Bind& bind_row_action)
{
    for (std::size_t i = 0; i < items.size(); ++i) {
        const auto& item = items[i];
        const auto id = vgui_tree_add_node(tree, parent, c_text(item.at("text")), nullptr);
        if (!id) return false;
        vgui_tree_set_item_enabled(tree, id, item.value("enabled", true));
        if (item.value("selected", false)) vgui_tree_select(tree, id, 1);
        if (const auto color = row_text_color(item)) vgui_tree_set_item_text_color(tree, id, color);
        if (const auto tooltip = item.find("tooltip"); tooltip != item.end()) vgui_tree_set_item_tooltip(tree, id, c_text(*tooltip));
        const auto row_path = path + "/" + std::to_string(i);
        apply_row_decorations(tree, id, item, row_path, load_icon, bind_row_action);
        if (const auto children = item.find("children"); children != item.end() &&
            !add_tree_rows(tree, id, *children, row_path + "/children", load_icon, bind_row_action)) return false;
        vgui_tree_set_item_expanded(tree, id, item.value("expanded", false));
    }
    return true;
}

template<class Load, class Bind>
bool add_menu_items(vgui_widget* menu, const json& items, const std::string& path, bool close_on_activate,
    const Load& load_icon, const Bind& bind_menu_action)
{
    vgui_menu_set_close_on_activate(menu, close_on_activate);
    for (std::size_t i = 0; i < items.size(); ++i) {
        const auto& item = items[i];
        if (item.contains("separator")) {
            if (!vgui_menu_add_separator(menu)) return false;
            continue;
        }
        const auto* label = c_text(item.at("text"));
        const auto children = item.find("children");
        vgui_widget* submenu = nullptr;
        vgui_item_id id;
        if (children != item.end()) {
            id = vgui_menu_add_submenu(menu, label, &submenu);
            if (!submenu) return false;
        } else {
            const auto checked = item.find("checked");
            id = checked != item.end() ? vgui_menu_add_check_item(menu, label, checked->get<bool>()) :
                vgui_menu_add_item(menu, label);
        }
        if (!id) return false;
        vgui_menu_set_item_enabled(menu, id, item.value("enabled", true));
        if (const auto shortcut = item.find("shortcut"); shortcut != item.end())
            vgui_menu_set_item_shortcut(menu, id, c_text(*shortcut));
        if (const auto emoji = item.find("emoji"); emoji != item.end())
            vgui_menu_set_item_emoji(menu, id, c_text(*emoji));
        const auto row_path = path + "/" + std::to_string(i);
        if (auto* icon = item_icon(item, row_path, load_icon)) vgui_menu_set_item_icon(menu, id, icon);
        if (submenu) {
            if (!add_menu_items(submenu, *children, row_path + "/children", close_on_activate, load_icon, bind_menu_action))
                return false;
        } else if (const auto action = item.find("action"); action != item.end()) {
            bind_menu_action(menu, id, action->get_ref<const std::string&>());
        }
    }
    return true;
}

inline vgui_widget* add_popup(vgui_widget* menubar, const char* title, bool enabled)
{
    const auto id = vgui_menubar_add_item(menubar, title);
    if (!id) return nullptr;
    vgui_menubar_set_item_enabled(menubar, id, enabled);
    return vgui_menubar_get_menu(menubar, id);
}
} // namespace collection_projection

// Projection onto a freshly created widget. Returns false for unsupported
// types, mismatched widgets, invalid items or a native creation failure.
// Access supplies has(key) and text/number/boolean(key) with descriptor defaults resolved.
// Menu actions are scoped to the real popup (possibly a nested submenu);
// list/tree actions are scoped to the layout widget, native row id and button index.
// load_icon(key, path) reports unavailable images without failing projection.
template<class Access, class Load, class MenuBind, class RowBind>
bool apply_collection(vgui_widget* widget, const node& n, const Access& properties, const std::string& items_path,
    const Load& load_icon, const MenuBind& bind_menu_action, const RowBind& bind_row_action)
{
    using namespace collection_projection;
    const auto kind = collection_kind(n.type);
    if (!widget || !kind) return false;
    const auto mounted = vgui_widget_get_kind(widget);
    if (mounted != *kind && !(*kind == VGUI_MENU && mounted == VGUI_MENUBAR)) return false;

    json items;
    std::string reason;
    if (!collection_items::parse(n.type, properties.text("items"), items, reason)) return false;
    switch (*kind) {
    case VGUI_LIST: {
        apply_scroll(widget, properties);
        const std::string empty(properties.text("empty_text"));
        vgui_list_set_empty_text(widget, empty.c_str());
        vgui_list_set_selection_mode(widget, selection_mode(properties.text("selection_mode")));
        return add_list_rows(widget, items, items_path, load_icon, bind_row_action);
    }
    case VGUI_TREE: {
        apply_scroll(widget, properties);
        const std::string empty(properties.text("empty_text"));
        vgui_tree_set_empty_text(widget, empty.c_str());
        vgui_tree_set_selection_mode(widget, selection_mode(properties.text("selection_mode")));
        return add_tree_rows(widget, 0, items, items_path, load_icon, bind_row_action);
    }
    case VGUI_MENU: {
        auto* menu = widget;
        if (mounted == VGUI_MENUBAR) {
            const std::string title(properties.text("text"));
            menu = add_popup(widget, title.c_str(), true);
            if (!menu) return false;
        }
        vgui_menu_set_width(menu, properties.number("menu_width"));
        vgui_menu_set_max_height(menu, properties.number("menu_max_height"));
        return add_menu_items(menu, items, items_path, properties.boolean("close_on_activate"), load_icon, bind_menu_action);
    }
    case VGUI_MENUBAR:
        for (std::size_t i = 0; i < items.size(); ++i) {
            const auto& heading = items[i];
            auto* menu = add_popup(widget, c_text(heading.at("text")), heading.value("enabled", true));
            if (!menu || !add_menu_items(menu, heading.at("children"), items_path + "/" + std::to_string(i) + "/children",
                true, load_icon, bind_menu_action)) return false;
        }
        return true;
    default:
        return false;
    }
}

} // namespace vgui::ui_design::detail
