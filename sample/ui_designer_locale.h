#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>

namespace vgui::ui_design {

enum class designer_language { simplified_chinese, english };
inline constexpr auto default_designer_language = designer_language::simplified_chinese;

enum class designer_theme { dark, light };
inline constexpr auto default_designer_theme = designer_theme::dark;
designer_theme load_designer_theme(const std::filesystem::path& path);
bool save_designer_theme(const std::filesystem::path& path, designer_theme theme);

class designer_strings {
public:
    explicit designer_strings(designer_language language = default_designer_language) : language_(language) {}
    designer_language language() const { return language_; }
    // Catalogs are immutable and live for the duration of the app. A missing key uses the supplied fallback.
    const char* text(std::string_view key, const char* fallback) const;
private:
    designer_language language_;
};

// A missing file or language selects the default. Invalid preferences select the default and set invalid.
designer_language load_designer_language(const std::filesystem::path& path, bool* invalid = nullptr);
// Preserve unrelated keys, and do not replace unreadable or malformed settings.
bool save_designer_language(const std::filesystem::path& path, designer_language language);

struct designer_dock_widths {
    float left = 240.f, right = 345.f; // Logical pixels, independent of DPI and window width.
    bool left_collapsed = false, right_collapsed = false;
    bool operator==(const designer_dock_widths&) const = default;
};

// Missing or invalid individual layout fields fall back to their defaults.
designer_dock_widths load_designer_dock_widths(const std::filesystem::path& path);
bool save_designer_dock_widths(const std::filesystem::path& path, designer_dock_widths widths);

enum class designer_grid_style { lines, dots };

inline constexpr float designer_grid_min_spacing = 1.f, designer_grid_max_spacing = 4096.f;
inline constexpr int designer_grid_min_major_every = 1, designer_grid_max_major_every = 100;
inline constexpr float designer_grid_min_hide_below = 0.f, designer_grid_max_hide_below = 64.f;
inline constexpr float designer_grid_min_line_width = 0.25f, designer_grid_max_line_width = 8.f;

struct designer_grid_settings {
    bool visible = true;
    designer_grid_style style = designer_grid_style::lines;
    float spacing = 20.f;
    int major_every = 5;
    float hide_below = 6.f;
    float line_width = 1.f;
    // Packed 0xAARRGGBB, persisted as #RRGGBB or #RRGGBBAA.
    std::uint32_t background = 0xFF171A1F, minor_color = 0xFF242A31, major_color = 0xFF333C46, axis_color = 0xFFE7122D;
    bool operator==(const designer_grid_settings&) const = default;
};

// Stored under canvas_grid; missing or invalid fields fall back individually.
designer_grid_settings load_designer_grid_settings(const std::filesystem::path& path);
// Reject invalid settings or unreadable/malformed files; preserve unrelated members on save.
bool save_designer_grid_settings(const std::filesystem::path& path, designer_grid_settings settings);
// Options can change both values in one atomic write. Omitted values are preserved.
bool save_designer_options(const std::filesystem::path& path, std::optional<designer_grid_settings> grid,
    std::optional<designer_theme> theme);

} // namespace vgui::ui_design
