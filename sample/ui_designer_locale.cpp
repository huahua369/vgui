#include "ui_designer_locale.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <fstream>
#include <map>
#include <random>
#include <string>

namespace vgui::ui_design {
namespace {
namespace fs = std::filesystem;
using json = nlohmann::json;
using catalog = std::map<std::string, std::string, std::less<>>;

#include "ui_designer_catalogs.h"

std::string_view language_code(designer_language language) {
    return language == designer_language::english ? "en-US" : "zh-CN";
}

catalog parse_catalog(const char* source) {
    catalog strings;
    const auto data = json::parse(source, nullptr, false);
    if (data.is_object()) {
        for (const auto& [key, value] : data.items()) {
            if (const auto* text = value.get_ptr<const std::string*>(); text && !text->empty() &&
                text->find('\0') == std::string::npos) strings.emplace(key, *text);
        }
    }
    return strings;
}

// Missing settings are normal. Limit reads to what a per-user preferences file should need.
constexpr std::size_t max_settings_bytes = 65536;

json read_settings(const fs::path& path) {
    if (path.empty()) return json::object();
    std::error_code ec;
    const auto status = fs::status(path, ec);
    if (status.type() == fs::file_type::not_found) return json::object();
    if (ec || !fs::is_regular_file(status)) return {};
    std::ifstream input(path, std::ios::binary);
    std::array<char, max_settings_bytes + 1> buffer;
    input.read(buffer.data(), buffer.size());
    if (!input.eof() || input.bad()) return {};
    const auto end = buffer.begin() + input.gcount();
    if (std::find(buffer.begin(), end, '\0') != end) return {};
    return json::parse(buffer.begin(), end, nullptr, false);
}

bool write_settings(const fs::path& path, const json& config) {
    const auto bytes = config.dump(2) + '\n';
    if (bytes.size() > max_settings_bytes) return false;

    // Write beside the destination, then replace it only after a successful close.
    std::error_code ec;
    const auto parent = path.has_parent_path() ? path.parent_path() : fs::path(".");
    std::random_device random;
    struct staging_dir {
        fs::path path;
        ~staging_dir() {
            if (path.empty()) return;
            std::error_code ec;
            fs::remove(path / "settings.json", ec);
            fs::remove(path, ec);
        }
    } staging;
    for (int attempt = 0; attempt < 16; ++attempt) {
        auto candidate = parent / (".vgui-designer-settings-" + std::to_string(random()));
        if (fs::create_directory(candidate, ec)) {
            staging.path = std::move(candidate);
            break;
        }
        if (ec && ec != std::errc::file_exists) return false;
    }
    if (staging.path.empty()) return false;
    const auto temp = staging.path / "settings.json";
    std::ofstream output(temp, std::ios::binary);
    output << bytes;
    output.close();
    if (!output) return false;
    fs::rename(temp, path, ec);
    return !ec;
}

bool valid_dock_width(double value) {
    return std::isfinite(value) && value >= 1.0 && value <= 16384.0;
}

bool valid_grid_number(double value, double minimum, double maximum) {
    return std::isfinite(value) && value >= minimum && value <= maximum;
}

std::uint32_t read_grid_color(const json& grid, const char* key, std::uint32_t fallback) {
    const auto it = grid.find(key);
    if (it == grid.end()) return fallback;
    const auto* text = it->get_ptr<const std::string*>();
    if (!text || (text->size() != 7 && text->size() != 9) || text->front() != '#') return fallback;
    std::uint32_t value = 0;
    const auto end = text->data() + text->size();
    const auto parsed = std::from_chars(text->data() + 1, end, value, 16);
    if (parsed.ec != std::errc{} || parsed.ptr != end) return fallback;
    return text->size() == 7 ? (0xff000000u | value) : ((value >> 8) | ((value & 0xffu) << 24));
}

std::string write_grid_color(std::uint32_t argb) {
    constexpr char hex[] = "0123456789ABCDEF";
    const auto alpha = argb >> 24;
    const bool opaque = alpha == 0xffu;
    auto value = opaque ? (argb & 0x00ffffffu) : ((argb << 8) | alpha);
    std::string text(opaque ? 7 : 9, '#');
    for (auto i = text.size() - 1; i > 0; --i) {
        text[i] = hex[value & 0xfu];
        value >>= 4;
    }
    return text;
}

} // namespace

const char* designer_strings::text(std::string_view key, const char* fallback) const {
    static const auto zh = parse_catalog(designer_zh_json);
    static const auto en = parse_catalog(designer_en_json);
    if (language_ == designer_language::simplified_chinese) {
        if (const auto it = zh.find(key); it != zh.end()) return it->second.c_str();
    }
    const auto it = en.find(key);
    return it == en.end() ? fallback : it->second.c_str();
}

designer_language load_designer_language(const fs::path& path, bool* invalid) {
    const auto config = read_settings(path);
    if (invalid) *invalid = false;
    if (config.is_object()) {
        const auto it = config.find("language");
        if (it == config.end()) return default_designer_language;
        if (const auto* code = it->get_ptr<const std::string*>()) {
            if (*code == language_code(default_designer_language)) return default_designer_language;
            if (*code == language_code(designer_language::english)) return designer_language::english;
        }
    }
    if (invalid) *invalid = true;
    return default_designer_language;
}

bool save_designer_language(const fs::path& path, designer_language language) {
    if (path.empty()) return false;
    auto config = read_settings(path);
    if (!config.is_object()) return false;
    config["language"] = language_code(language);
    return write_settings(path, config);
}

designer_dock_widths load_designer_dock_widths(const fs::path& path) {
    designer_dock_widths widths;
    const auto config = read_settings(path);
    const auto layout = config.find("layout");
    if (layout == config.end() || !layout->is_object()) return widths;
    const auto read_width = [&](const char* key, float fallback) {
        const auto it = layout->find(key);
        if (it == layout->end() || !it->is_number()) return fallback;
        const auto value = it->get<double>();
        return valid_dock_width(value) ? static_cast<float>(value) : fallback;
    };
    const auto read_collapsed = [&](const char* key) {
        const auto it = layout->find(key);
        return it != layout->end() && it->is_boolean() && it->get<bool>();
    };
    return {read_width("left_width", widths.left), read_width("right_width", widths.right),
            read_collapsed("left_collapsed"), read_collapsed("right_collapsed")};
}

bool save_designer_dock_widths(const fs::path& path, designer_dock_widths widths) {
    if (path.empty() || !valid_dock_width(widths.left) || !valid_dock_width(widths.right)) return false;
    auto config = read_settings(path);
    if (!config.is_object()) return false;
    auto& layout = config["layout"];
    if (!layout.is_object()) layout = json::object();
    layout["left_width"] = widths.left;
    layout["right_width"] = widths.right;
    layout["left_collapsed"] = widths.left_collapsed;
    layout["right_collapsed"] = widths.right_collapsed;
    return write_settings(path, config);
}

designer_grid_settings load_designer_grid_settings(const fs::path& path) {
    designer_grid_settings settings;
    const auto config = read_settings(path);
    const auto grid = config.find("canvas_grid");
    if (grid == config.end() || !grid->is_object()) return settings;

    if (const auto it = grid->find("visible"); it != grid->end() && it->is_boolean())
        settings.visible = it->get<bool>();
    if (const auto it = grid->find("style"); it != grid->end()) {
        if (const auto* style = it->get_ptr<const std::string*>()) {
            if (*style == "lines") settings.style = designer_grid_style::lines;
            else if (*style == "dots") settings.style = designer_grid_style::dots;
        }
    }
    const auto read_float = [&](const char* key, float fallback, float minimum, float maximum) {
        const auto it = grid->find(key);
        if (it == grid->end() || !it->is_number()) return fallback;
        const auto value = it->get<double>();
        return valid_grid_number(value, minimum, maximum) ? static_cast<float>(value) : fallback;
    };
    settings.spacing = read_float("spacing", settings.spacing, designer_grid_min_spacing, designer_grid_max_spacing);
    if (const auto it = grid->find("major_every"); it != grid->end() && it->is_number_integer()) {
        const auto value = it->get<double>();
        if (valid_grid_number(value, designer_grid_min_major_every, designer_grid_max_major_every))
            settings.major_every = static_cast<int>(value);
    }
    settings.hide_below = read_float("hide_below", settings.hide_below, designer_grid_min_hide_below, designer_grid_max_hide_below);
    settings.line_width = read_float("line_width", settings.line_width, designer_grid_min_line_width, designer_grid_max_line_width);
    settings.background = read_grid_color(*grid, "background", settings.background);
    settings.minor_color = read_grid_color(*grid, "minor_color", settings.minor_color);
    settings.major_color = read_grid_color(*grid, "major_color", settings.major_color);
    settings.axis_color = read_grid_color(*grid, "axis_color", settings.axis_color);
    return settings;
}

bool save_designer_grid_settings(const fs::path& path, designer_grid_settings settings) {
    if (path.empty() || (settings.style != designer_grid_style::lines && settings.style != designer_grid_style::dots) ||
        !valid_grid_number(settings.spacing, designer_grid_min_spacing, designer_grid_max_spacing) ||
        !valid_grid_number(settings.major_every, designer_grid_min_major_every, designer_grid_max_major_every) ||
        !valid_grid_number(settings.hide_below, designer_grid_min_hide_below, designer_grid_max_hide_below) ||
        !valid_grid_number(settings.line_width, designer_grid_min_line_width, designer_grid_max_line_width)) return false;

    auto config = read_settings(path);
    if (!config.is_object()) return false;
    auto& grid = config["canvas_grid"];
    if (!grid.is_object()) grid = json::object();
    grid["visible"] = settings.visible;
    grid["style"] = settings.style == designer_grid_style::dots ? "dots" : "lines";
    grid["spacing"] = settings.spacing;
    grid["major_every"] = settings.major_every;
    grid["hide_below"] = settings.hide_below;
    grid["line_width"] = settings.line_width;
    grid["background"] = write_grid_color(settings.background);
    grid["minor_color"] = write_grid_color(settings.minor_color);
    grid["major_color"] = write_grid_color(settings.major_color);
    grid["axis_color"] = write_grid_color(settings.axis_color);
    return write_settings(path, config);
}

} // namespace vgui::ui_design
