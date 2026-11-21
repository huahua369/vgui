#pragma once

#include "model.h"

#include <cmath>
#include <vgui/vgui.h>

namespace vgui::ui_design::detail {

inline std::optional<vgui_widget_kind> container_kind(std::string_view type)
{
    if (type == "splitter") return VGUI_SPLITTER;
    if (type == "tabview") return VGUI_TABVIEW;
    if (type == "dock") return VGUI_DOCK;
    return std::nullopt;
}

template<class Access>
void apply_container(vgui_widget* widget, const node& n, const Access& props)
{
    if (n.type == "splitter") {
        vgui_splitter_set_vertical(widget, props.text("orientation") == "vertical");
        vgui_splitter_set_ratio(widget, static_cast<float>(props.number("ratio")));
        vgui_splitter_set_sash_size(widget, static_cast<float>(props.number("sash_size")));
    } else if (n.type == "tabview") {
        vgui_tab_set_tab_height(widget, static_cast<float>(props.number("tab_height")));
    } else if (n.type == "dock") {
        vgui_dock_set_document_tabs(widget, props.boolean("document_tabs"));
    }
}

inline vgui_dock_zone page_dock_zone(std::string_view zone)
{
    if (zone == "left") return VGUI_DOCK_LEFT;
    if (zone == "right") return VGUI_DOCK_RIGHT;
    if (zone == "bottom") return VGUI_DOCK_BOTTOM;
    if (zone == "floating") return VGUI_DOCK_FLOATING;
    return VGUI_DOCK_CENTER;
}

// Call after vgui_widget_add and vgui_widget_set_id on the child. Registration
// also attaches pages natively, a no-op for an already attached child. On
// failure the caller destroys the staged tree, preserving the previous mount.
template<class Access>
bool insert_content(vgui_widget* parent, const node& parent_node, vgui_widget* child,
    const node& child_node, const Access& child_props)
{
    if (parent_node.type != "tabview" && parent_node.type != "dock") return true;
    if (!parent || !child || vgui_widget_get_parent(child) != parent) return false;

    std::string title(child_props.text("page_title"));
    if (title.empty()) title = child_node.name;
    if (title.empty()) title = vgui_widget_get_id(child);
    if (parent_node.type == "tabview") {
        const auto item = vgui_tab_add(parent, title.c_str(), child);
        if (!item || vgui_tab_get_page(parent, item) != child) return false;
        // Native tab close destroys the page. The document controls ownership.
        vgui_tab_set_closeable(parent, item, 0);
        vgui_tab_set_enabled(parent, item, child_props.boolean("tab_enabled"));
        return true;
    }

    vgui_dock_panel_desc desc;
    vgui_dock_panel_desc_default(&desc);
    desc.id = vgui_widget_get_id(child);
    desc.title = title.c_str();
    desc.panel = child;
    desc.zone = page_dock_zone(child_props.text("dock_zone"));
    desc.closeable = child_props.boolean("dock_closeable");
    desc.open = child_props.boolean("dock_open");
    desc.x = child_props.number("dock_float_x");
    desc.y = child_props.number("dock_float_y");
    desc.width = child_props.number("dock_float_width");
    desc.height = child_props.number("dock_float_height");
    desc.emoji = child_props.text("dock_emoji").c_str();
    const auto item = vgui_dock_add(parent, &desc);
    if (!item || vgui_dock_get_panel(parent, item) != child) return false;
    vgui_dock_set_tooltip(parent, item, child_props.text("dock_tooltip").c_str());
    return true;
}

template<class Access>
void finish_container(vgui_widget* widget, const node& n, const Access& props)
{
    if (n.type == "tabview") {
        // The accessor may narrow dimensions to float. Check the stored
        // double so a fractional document index cannot round to an integer.
        const auto active = n.props.find("active_index");
        if (active == n.props.end()) return;
        const auto* index = std::get_if<double>(&active->second);
        if (index && *index >= 0 && *index < vgui_tab_get_count(widget) && std::trunc(*index) == *index) {
            const auto item = vgui_tab_get_at(widget, static_cast<int>(*index));
            if (item) vgui_tab_set_active(widget, item);
        }
    } else if (n.type == "dock") {
        vgui_dock_set_zone_size(widget, VGUI_DOCK_LEFT, static_cast<float>(props.number("left_width")));
        vgui_dock_set_zone_size(widget, VGUI_DOCK_RIGHT, static_cast<float>(props.number("right_width")));
        vgui_dock_set_zone_size(widget, VGUI_DOCK_BOTTOM, static_cast<float>(props.number("bottom_height")));
        vgui_dock_set_side_resize(widget, static_cast<float>(props.number("min_left_width")),
            static_cast<float>(props.number("min_center_width")), static_cast<float>(props.number("min_right_width")));
        vgui_dock_set_side_collapsed(widget, VGUI_DOCK_LEFT, props.boolean("left_collapsed"));
        vgui_dock_set_side_collapsed(widget, VGUI_DOCK_RIGHT, props.boolean("right_collapsed"));
    }
}

} // namespace vgui::ui_design::detail
