#pragma once

#include "../vgui_button_gradient.h"

#include <string_view>

namespace vgui::ui_design::detail {

inline auto button_gradient_palette(std::string_view preset)
{
    using palette = decltype(vgui::detail::default_button_gradient);
    if (preset == "blue") return palette{0xFF4A8FE7, 0xFF2D6FD9, 0xFF6AA8F2, 0xFF3E82E0,
        0xFF1E4FB0, 0xFF163D99, 0xFF7DB8FF, 0xFF0D2E66};
    if (preset == "sunset") return palette{0xFFF59E0B, 0xFFDC2626, 0xFFFBBF24, 0xFFEF4444,
        0xFFB45309, 0xFF991B1B, 0xFFFFE28A, 0xFF450A0A};
    if (preset == "green") return palette{0xFF22C55E, 0xFF16A34A, 0xFF4ADE80, 0xFF22C55E,
        0xFF0F7A2E, 0xFF065F22, 0xFF86EFAC, 0xFF052E16};
    return vgui::detail::default_button_gradient;
}

} // namespace vgui::ui_design::detail
