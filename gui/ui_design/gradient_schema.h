#pragma once

#include "model.h"

namespace vgui::ui_design::detail {

inline constexpr std::string_view gradient_kinds[] = {"linear", "radial", "sweep"};
inline constexpr std::string_view gradient_extends[] = {"pad", "repeat", "reflect"};
inline constexpr std::string_view gradient_animations[] = {"none", "shine", "color", "keyframes"};
inline constexpr std::string_view gradient_keyframe_interpolations[] = {"step", "linear", "smooth"};
inline constexpr double gradient_limit = 16.0;
inline const property_descriptor gradient_properties[] = {
    {"gradient_kind", "Gradient type", value_type::choice, std::string("linear"), 0, 0, gradient_kinds},
    {"gradient_extend", "Spread", value_type::choice, std::string("pad"), 0, 0, gradient_extends},
    {"gradient_stops", "Color stops", value_type::gradient_stops,
        std::string(R"([{"color":"#5E9CEA","offset":0.0},{"color":"#1C3E70","offset":1.0}])")},
    {"gradient_x0", "Start X", value_type::number, 0.5, -gradient_limit, gradient_limit},
    {"gradient_y0", "Start Y", value_type::number, 0.0, -gradient_limit, gradient_limit},
    {"gradient_x1", "End X", value_type::number, 0.5, -gradient_limit, gradient_limit},
    {"gradient_y1", "End Y", value_type::number, 1.0, -gradient_limit, gradient_limit},
    {"gradient_center_x", "Center X", value_type::number, 0.5, -gradient_limit, gradient_limit},
    {"gradient_center_y", "Center Y", value_type::number, 0.5, -gradient_limit, gradient_limit},
    {"gradient_outer_x", "Outer center X", value_type::number, 0.5, -gradient_limit, gradient_limit},
    {"gradient_outer_y", "Outer center Y", value_type::number, 0.5, -gradient_limit, gradient_limit},
    {"gradient_radius0", "Inner radius", value_type::number, 0.0, 0, gradient_limit},
    {"gradient_radius1", "Outer radius", value_type::number, 0.5, 0, gradient_limit},
    {"gradient_ellipse", "Elliptical (2:1)", value_type::boolean, false},
    {"gradient_angle0", "Start angle (°)", value_type::number, 0.0, 0, 360},
    {"gradient_angle1", "End angle (°)", value_type::number, 360.0, 0, 360},
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
