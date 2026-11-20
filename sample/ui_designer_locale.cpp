#include "ui_designer_locale.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
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
json read_settings(const fs::path& path) {
    if (path.empty()) return json::object();
    std::error_code ec;
    const auto status = fs::status(path, ec);
    if (status.type() == fs::file_type::not_found) return json::object();
    if (ec || !fs::is_regular_file(status)) return {};
    std::ifstream input(path, std::ios::binary);
    std::array<char, 65537> buffer;
    input.read(buffer.data(), buffer.size());
    if (!input.eof() || input.bad()) return {};
    const auto end = buffer.begin() + input.gcount();
    if (std::find(buffer.begin(), end, '\0') != end) return {};
    return json::parse(buffer.begin(), end, nullptr, false);
}

bool write_settings(const fs::path& path, const json& config) {
    const auto bytes = config.dump(2) + '\n';

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

} // namespace vgui::ui_design
