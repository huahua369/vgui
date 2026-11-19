#pragma once

#include <stb_image_write.h>

#include <filesystem>
#include <fstream>

namespace vgui::ui_design {

inline constexpr auto smoke_badge_key = "badge";
inline constexpr auto smoke_badge_file = "smoke-badge.png";

// Keep the example form self-contained. Native tests generate their own real
// images, including a second variant for detecting a wrong export resource root.
inline bool write_smoke_badge(const std::filesystem::path& path, bool alternate = false) {
    constexpr unsigned char pixels_a[] = {0x32, 0x87, 0xe0, 0x83, 0xce, 0xff};
    constexpr unsigned char pixels_b[] = {0xb6, 0x47, 0x22, 0xff, 0xab, 0x65};
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    const auto write = [](void* user, void* data, int size) {
        static_cast<std::ofstream*>(user)->write(static_cast<const char*>(data), size);
    };
    const bool encoded = stbi_write_png_to_func(write, &out, 2, 1, 3, alternate ? pixels_b : pixels_a, 6) != 0;
    out.close();
    return encoded && bool(out);
}

} // namespace vgui::ui_design
