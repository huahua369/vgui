#pragma once

#include <filesystem>
#include <string_view>

namespace vgui::ui_design {

enum class designer_language { simplified_chinese, english };
inline constexpr auto default_designer_language = designer_language::simplified_chinese;

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
    bool operator==(const designer_dock_widths&) const = default;
};

// Missing or invalid individual widths fall back to the defaults.
designer_dock_widths load_designer_dock_widths(const std::filesystem::path& path);
bool save_designer_dock_widths(const std::filesystem::path& path, designer_dock_widths widths);

} // namespace vgui::ui_design
