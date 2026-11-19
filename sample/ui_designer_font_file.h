#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace vgui::ui_design {

inline constexpr std::size_t max_project_font_file = 32u << 20;

// HarfBuzz can ignore out-of-bounds tables and still construct a face from a
// truncated file. Reject incomplete table data before asking OVG to register it.
inline bool valid_font_file(std::span<const unsigned char> bytes) {
    if (bytes.size() < 12 || bytes.size() > max_project_font_file) return false;
    const auto u32 = [&](std::size_t at) {
        return (std::uint32_t(bytes[at]) << 24) | (std::uint32_t(bytes[at + 1]) << 16) |
            (std::uint32_t(bytes[at + 2]) << 8) | std::uint32_t(bytes[at + 3]);
    };
    const auto signature = u32(0);
    if (signature != 0x00010000 && signature != 0x4F54544F && signature != 0x74727565) return false; // TTF, OTTO, true
    const std::size_t count = (std::size_t(bytes[4]) << 8) | bytes[5];
    const std::size_t end = 12 + 16 * count;
    if (count == 0 || end > bytes.size()) return false;
    for (std::size_t at = 12; at < end; at += 16) {
        const auto offset = u32(at + 8), length = u32(at + 12);
        if (offset > bytes.size() || (length && offset < end) || length > bytes.size() - offset) return false;
    }
    return true;
}

} // namespace vgui::ui_design
