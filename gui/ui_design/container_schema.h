#pragma once

#include "model.h"

namespace vgui::ui_design::detail {

// A splitter takes up to two children of any design kind, in pane order.
// Native tab views and docks accept arbitrary widget kinds; require direct
// panel children in the design model so page metadata stays on editable panels.
inline constexpr std::string_view splitter_orientations[] = {"horizontal", "vertical"};
inline const property_descriptor splitter_properties[] = {
    {"orientation", "Orientation", value_type::choice, std::string("horizontal"), 0, 0, splitter_orientations},
    {"ratio", "First pane fraction", value_type::number, 0.5, 0, 1},
    {"sash_size", "Sash size", value_type::number, 4.0, 0, 1000000},
};

inline const property_descriptor tabview_properties[] = {
    {"tab_height", "Tab height", value_type::number, 30.0, 1, 1000000},
    // Document child index, not a native item ID. Missing, fractional, or
    // out-of-range values leave the native first-page selection in place.
    {"active_index", "Initial tab (0-based)", value_type::number, 0.0, 0, static_cast<double>(max_nodes - 1)},
};

inline const property_descriptor dock_properties[] = {
    {"document_tabs", "Document tabs", value_type::boolean, false},
    {"left_width", "Left width (-1: style)", value_type::number, -1.0, -1, 1000000},
    {"right_width", "Right width (-1: style)", value_type::number, -1.0, -1, 1000000},
    {"bottom_height", "Bottom height (-1: style)", value_type::number, -1.0, -1, 1000000},
    {"min_left_width", "Minimum left width", value_type::number, 0.0, 0, 1000000},
    {"min_center_width", "Minimum center width (0: no resize)", value_type::number, 0.0, 0, 1000000},
    {"min_right_width", "Minimum right width", value_type::number, 0.0, 0, 1000000},
    {"left_collapsed", "Left side starts collapsed", value_type::boolean, false},
    {"right_collapsed", "Right side starts collapsed", value_type::boolean, false},
};

// Only interpreted when the panel is a direct page of a tab view or dock.
// The page's own visible flag is managed by the native container; dock_open
// sets the initial open state. Tab pages cannot be closed because native tab
// closure destroys the page and invalidates the design runtime's widget map.
inline constexpr std::string_view page_zones[] = {"left", "right", "bottom", "center", "floating"};
inline const property_descriptor page_properties[] = {
    {"page_title", "Page title (blank: node name)", value_type::text, std::string{}},
    {"tab_enabled", "Enable tab", value_type::boolean, true},
    {"dock_emoji", "Dock tab glyph", value_type::text, std::string{}},
    {"dock_tooltip", "Dock tab tooltip", value_type::text, std::string{}},
    {"dock_float_x", "Floating X", value_type::number, 0.0, 0, 1000000},
    {"dock_float_y", "Floating Y", value_type::number, 0.0, 0, 1000000},
    {"dock_float_width", "Floating width (0: default)", value_type::number, 0.0, 0, 1000000},
    {"dock_float_height", "Floating height (0: default)", value_type::number, 0.0, 0, 1000000},
    {"dock_zone", "Dock zone", value_type::choice, std::string("center"), 0, 0, page_zones},
    {"dock_closeable", "Dock page can close", value_type::boolean, false},
    {"dock_open", "Dock page starts open", value_type::boolean, true},
};

} // namespace vgui::ui_design::detail
