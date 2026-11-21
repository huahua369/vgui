#pragma once

#include "model.h"

namespace vgui::ui_design::detail {

inline constexpr double primitive_extent = 1000000.0;
inline constexpr std::string_view primitive_font_weights[] = {"default", "regular", "medium", "semi_bold", "bold"};
inline constexpr std::string_view primitive_font_slants[] = {"default", "normal", "italic", "oblique"};
inline constexpr std::string_view text_alignment_x[] = {"start", "center", "end"};
inline constexpr std::string_view text_alignment_y[] = {"top", "center", "bottom"};
inline constexpr std::string_view label_text_orientations[] = {"horizontal", "vertical"};
inline constexpr std::string_view content_alignment[] = {
    "start", "center", "end", "space-between", "space-around", "space-evenly", "stretch"
};
inline constexpr std::string_view fill_styles[] = {"solid", "gradient"};
inline constexpr std::string_view gradient_directions[] = {"vertical", "horizontal"};
inline constexpr std::uint32_t default_panel_gradient_start = 0xFF43576B;
inline constexpr std::uint32_t default_panel_gradient_end = 0xFF192532;
inline constexpr std::string_view button_gradient_presets[] = {"default", "blue", "sunset", "green"};
inline constexpr std::string_view button_icon_positions[] = {"left", "right", "top", "bottom"};
inline constexpr std::string_view image_fits[] = {"stretch", "contain", "cover", "none"};

inline const property_descriptor primitive_text = {"text", "Text", value_type::text, std::string{}};
inline const property_descriptor primitive_text_key = {"text_key", "Localization key", value_type::localization_key, std::string{}};
inline const property_descriptor primitive_font_size = {"font_size", "Font size", value_type::number, 0.0, 0, 256};
inline const property_descriptor primitive_font_weight = {"font_weight", "Font weight", value_type::choice, std::string("default"), 0, 0, primitive_font_weights};
inline const property_descriptor primitive_font_slant = {"font_slant", "Font slant", value_type::choice, std::string("default"), 0, 0, primitive_font_slants};
inline const property_descriptor primitive_text_color = {"text_color", "Text color", value_type::color, std::string{}};
inline const property_descriptor primitive_font_family = {"font_family", "Font family", value_type::font_key, std::string{}};

inline const property_descriptor text_align_start = {"text_align_x", "Horizontal text alignment", value_type::choice, std::string("start"), 0, 0, text_alignment_x};
inline const property_descriptor text_align_center = {"text_align_x", "Horizontal text alignment", value_type::choice, std::string("center"), 0, 0, text_alignment_x};
inline const property_descriptor text_align_middle = {"text_align_y", "Vertical text alignment", value_type::choice, std::string("center"), 0, 0, text_alignment_y};
inline const property_descriptor initially_checked = {"checked", "Initially checked", value_type::boolean, false};
inline const property_descriptor accent_color = {"accent_color", "Accent color", value_type::color, std::string{}};
inline const property_descriptor track_color = {"track_color", "Track color", value_type::color, std::string{}};
inline const property_descriptor vertical = {"vertical", "Vertical", value_type::boolean, false};
inline const property_descriptor range_min = {"min_value", "Minimum value", value_type::number, 0.0, -primitive_extent, primitive_extent};
inline const property_descriptor range_max = {"max_value", "Maximum value", value_type::number, 1.0, -primitive_extent, primitive_extent};
inline const property_descriptor range_value = {"value", "Initial value", value_type::number, 0.0, -primitive_extent, primitive_extent};
inline const property_descriptor range_step = {"step", "Step", value_type::number, 0.0, 0, 2 * primitive_extent};

inline const property_descriptor checkbox_properties[] = {
    primitive_text, primitive_text_key, initially_checked, accent_color,
    text_align_start, text_align_middle, primitive_font_size, primitive_font_weight,
    primitive_font_slant, primitive_text_color, primitive_font_family,
};
inline const property_descriptor radio_properties[] = {
    primitive_text, primitive_text_key, {"radio_group", "Group name", value_type::text, std::string{}},
    initially_checked, accent_color,
    text_align_start, text_align_middle, primitive_font_size, primitive_font_weight,
    primitive_font_slant, primitive_text_color, primitive_font_family,
};
inline const property_descriptor switch_properties[] = {
    primitive_text, primitive_text_key, initially_checked, accent_color,
    text_align_start, text_align_middle, primitive_font_size, primitive_font_weight,
    primitive_font_slant, primitive_text_color, primitive_font_family,
};
inline const property_descriptor slider_properties[] = {
    range_min, range_max, range_value, range_step, vertical, accent_color, track_color,
};
inline const property_descriptor progress_properties[] = {
    range_min, range_max, range_value, range_step, vertical,
    {"radial", "Radial", value_type::boolean, false},
    {"show_text", "Show progress text", value_type::boolean, true},
    {"accent_color", "Accent color", value_type::color, std::string{"#8CCBFF"}},
    track_color, primitive_text, primitive_text_key, text_align_center,
    text_align_middle, primitive_font_size, primitive_font_weight, primitive_font_slant,
    primitive_text_color, primitive_font_family,
};
inline const property_descriptor separator_properties[] = {
    {"thickness", "Auto-height thickness", value_type::number, 0.0, 0, primitive_extent},
};

inline const property_descriptor extra_panel_properties[] = {
    {"background_style", "Background style", value_type::choice, std::string("solid"), 0, 0, fill_styles},
    {"background_gradient_direction", "Gradient direction", value_type::choice, std::string("vertical"), 0, 0, gradient_directions},
    {"background_gradient_start", "Gradient start", value_type::color, std::string{}},
    {"background_gradient_end", "Gradient end", value_type::color, std::string{}},
    {"align_content", "Align wrapped rows", value_type::choice, std::string("start"), 0, 0, content_alignment},
    {"scrollable_x", "Scroll horizontally", value_type::boolean, false},
    {"scrollable_y", "Scroll vertically", value_type::boolean, false},
    {"scrollbar_width_x", "Horizontal scrollbar thickness", value_type::number, 0.0, 0, primitive_extent},
    {"scrollbar_width_y", "Vertical scrollbar thickness", value_type::number, 0.0, 0, primitive_extent},
    {"scrollbar_inset", "Scrollbar inset", value_type::number, -1.0, -1, primitive_extent},
};
inline const property_descriptor extra_label_properties[] = {
    {"text_orientation", "Text orientation", value_type::choice, std::string("horizontal"), 0, 0, label_text_orientations},
    text_align_start, text_align_middle,
};
inline const property_descriptor extra_button_properties[] = {
    text_align_center, text_align_middle,
    {"button_style", "Button style", value_type::choice, std::string("solid"), 0, 0, fill_styles},
    {"gradient_preset", "Gradient preset", value_type::choice, std::string("default"), 0, 0, button_gradient_presets},
    {"checked", "Initially active", value_type::boolean, false},
    {"icon", "Button icon path", value_type::resource_key, std::string{}},
    {"icon_width", "Icon width", value_type::number, 0.0, 0, primitive_extent},
    {"icon_height", "Icon height", value_type::number, 0.0, 0, primitive_extent},
    {"icon_position", "Icon position", value_type::choice, std::string("left"), 0, 0, button_icon_positions},
    {"icon_gap", "Icon gap", value_type::number, 0.0, 0, primitive_extent},
    {"icon_tint", "Icon tint", value_type::color, std::string{}},
};
inline const property_descriptor extra_button_gradient_properties[] = {
    {"gradient_normal_top", "Normal top", value_type::color, std::string{}},
    {"gradient_normal_bottom", "Normal bottom", value_type::color, std::string{}},
    {"gradient_hover_top", "Hover top", value_type::color, std::string{}},
    {"gradient_hover_bottom", "Hover bottom", value_type::color, std::string{}},
    {"gradient_pressed_top", "Pressed top", value_type::color, std::string{}},
    {"gradient_pressed_bottom", "Pressed bottom", value_type::color, std::string{}},
    {"gradient_border_light", "Highlight edge", value_type::color, std::string{}},
    {"gradient_border_dark", "Shadow edge", value_type::color, std::string{}},
};
inline const property_descriptor extra_edit_properties[] = {
    {"multiline", "Multiline", value_type::boolean, false},
    {"readonly", "Read-only", value_type::boolean, false},
    {"password", "Mask text", value_type::boolean, false},
    {"background_transparent", "Transparent edit background", value_type::boolean, false},
    {"selection_color", "Selection color", value_type::color, std::string{}},
    {"selection_rounding", "Selection rounding", value_type::number, -1.0, -1, primitive_extent},
    {"caret_width", "Caret width", value_type::number, -1.0, -1, primitive_extent},
};
inline const property_descriptor extra_image_properties[] = {
    {"fit", "Image fit", value_type::choice, std::string("stretch"), 0, 0, image_fits},
    {"source_x", "Source X", value_type::number, 0.0, 0, primitive_extent},
    {"source_y", "Source Y", value_type::number, 0.0, 0, primitive_extent},
    {"source_width", "Source width", value_type::number, 0.0, 0, primitive_extent},
    {"source_height", "Source height", value_type::number, 0.0, 0, primitive_extent},
    {"tint", "Image tint", value_type::color, std::string{}},
    {"flip_x", "Flip horizontally", value_type::boolean, false},
    {"flip_y", "Flip vertically", value_type::boolean, false},
    {"pixel_snap", "Snap image to pixels", value_type::boolean, false},
    {"rotation", "Image rotation (degrees)", value_type::number, 0.0, -360, 360},
    // Native nine-slice is enabled by a positive left border.
    {"slice_left", "Nine-slice left border", value_type::number, 0.0, 0, primitive_extent},
    {"slice_top", "Nine-slice top border", value_type::number, 0.0, 0, primitive_extent},
    {"slice_right", "Nine-slice right border", value_type::number, 0.0, 0, primitive_extent},
    {"slice_bottom", "Nine-slice bottom border", value_type::number, 0.0, 0, primitive_extent},
    {"rotation_center_enabled", "Use custom rotation center", value_type::boolean, false},
    {"rotation_center_x", "Rotation center X (px)", value_type::number, 0.0, -primitive_extent, primitive_extent},
    {"rotation_center_y", "Rotation center Y (px)", value_type::number, 0.0, -primitive_extent, primitive_extent},
    {"matrix_enabled", "Enable image matrix", value_type::boolean, false},
    {"matrix_xx", "Matrix XX", value_type::number, 1.0, -primitive_extent, primitive_extent},
    {"matrix_yx", "Matrix YX", value_type::number, 0.0, -primitive_extent, primitive_extent},
    {"matrix_xy", "Matrix XY", value_type::number, 0.0, -primitive_extent, primitive_extent},
    {"matrix_yy", "Matrix YY", value_type::number, 1.0, -primitive_extent, primitive_extent},
    {"matrix_tx", "Matrix translate X (px)", value_type::number, 0.0, -primitive_extent, primitive_extent},
    {"matrix_ty", "Matrix translate Y (px)", value_type::number, 0.0, -primitive_extent, primitive_extent},
};

} // namespace vgui::ui_design::detail
