#pragma once

#include "model.h"

namespace vgui::ui_design::detail {

inline constexpr std::string_view gradient_kinds[] = {"linear", "radial", "sweep"};
inline constexpr std::string_view gradient_extends[] = {"pad", "repeat", "reflect"};
inline constexpr std::string_view gradient_animations[] = {"none", "shine", "color", "keyframes"};
inline constexpr std::string_view gradient_keyframe_interpolations[] = {"step", "linear", "smooth"};
inline property_descriptor gradient_geometry_property(gradient_geometry_axis axis, std::string_view label, double fallback)
{
    const auto& channel = gradient_geometry_channels[static_cast<std::size_t>(axis)];
    return {channel.property, label, value_type::number, fallback, channel.min, channel.max};
}

inline const property_descriptor gradient_properties[] = {
    {"gradient_kind", "Gradient type", value_type::choice, std::string("linear"), 0, 0, gradient_kinds},
    {"gradient_extend", "Spread", value_type::choice, std::string("pad"), 0, 0, gradient_extends},
    {"gradient_stops", "Color stops", value_type::gradient_stops,
        std::string(R"([{"color":"#5E9CEA","offset":0.0},{"color":"#1C3E70","offset":1.0}])")},
    gradient_geometry_property(gradient_geometry_axis::start_x, "Start X", 0.5),
    gradient_geometry_property(gradient_geometry_axis::start_y, "Start Y", 0.0),
    gradient_geometry_property(gradient_geometry_axis::end_x, "End X", 0.5),
    gradient_geometry_property(gradient_geometry_axis::end_y, "End Y", 1.0),
    gradient_geometry_property(gradient_geometry_axis::center_x, "Center X", 0.5),
    gradient_geometry_property(gradient_geometry_axis::center_y, "Center Y", 0.5),
    gradient_geometry_property(gradient_geometry_axis::outer_x, "Outer center X", 0.5),
    gradient_geometry_property(gradient_geometry_axis::outer_y, "Outer center Y", 0.5),
    {"gradient_radius0", "Inner radius", value_type::number, 0.0, 0, gradient_limit},
    {"gradient_radius1", "Outer radius", value_type::number, 0.5, 0, gradient_limit},
    {"gradient_ellipse", "Elliptical (2:1)", value_type::boolean, false},
    gradient_geometry_property(gradient_geometry_axis::start_angle, "Start angle (°)", 0.0),
    gradient_geometry_property(gradient_geometry_axis::end_angle, "End angle (°)", 360.0),
    {"gradient_affine_xx", "XX (X scale)", value_type::number, 1.0, -gradient_limit, gradient_limit},
    {"gradient_affine_yx", "YX (X into Y)", value_type::number, 0.0, -gradient_limit, gradient_limit},
    {"gradient_affine_xy", "XY (Y into X)", value_type::number, 0.0, -gradient_limit, gradient_limit},
    {"gradient_affine_yy", "YY (Y scale)", value_type::number, 1.0, -gradient_limit, gradient_limit},
    {"gradient_affine_tx", "X translation", value_type::number, 0.0, -gradient_limit, gradient_limit},
    {"gradient_affine_ty", "Y translation", value_type::number, 0.0, -gradient_limit, gradient_limit},
    {"gradient_animation", "Animation", value_type::choice, std::string("none"), 0, 0, gradient_animations},
    {"gradient_animation_duration", "Duration (s)", value_type::number, 2.0, 0.1, 60.0},
    {"gradient_animation_loop", "Loop", value_type::boolean, true},
    {"gradient_animation_dx", "Shine travel X", value_type::number, 2.0, -4.0, 4.0},
    {"gradient_animation_dy", "Shine travel Y", value_type::number, 0.0, -4.0, 4.0},
    {"gradient_animation_color", "Target color", value_type::color, std::string("#70BFFF")},
    {"gradient_animation_keyframes", "Keyframes", value_type::gradient_keyframes, std::string("[]")},
};

} // namespace vgui::ui_design::detail
