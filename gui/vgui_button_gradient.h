#pragma once

#include <array>
#include <cstdint>

namespace vgui::detail {

// AARRGGBB pairs in normal, hover, pressed order, then highlight and shadow.
inline constexpr std::array<std::uint32_t, 8> default_button_gradient = {
    0xFF4A4A4A, 0xFF3A3A3A, 0x80404040, 0x80303030,
    0xFF292929, 0xFF1D1D1D, 0xFF5C5C5C, 0xFF1D1D1D,
};

} // namespace vgui::detail
