// Headless canvas grid preferences tests; only the locale library is required.
#include "ui_designer_locale.h"

#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace {
namespace fs = std::filesystem;
namespace ui = vgui::ui_design;

void check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

struct temp_dir {
    fs::path path;

    temp_dir() {
        std::random_device random;
        const auto parent = fs::temp_directory_path();
        for (int attempt = 0; attempt < 16; ++attempt) {
            path = parent / ("vgui-designer-grid-test-" + std::to_string(random()));
            if (fs::create_directory(path)) return;
        }
        throw std::runtime_error("cannot create temporary test directory");
    }
    ~temp_dir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void write(const fs::path& path, std::string_view text) {
    std::ofstream output(path, std::ios::binary);
    output << text;
    output.close();
    check(bool(output), "cannot seed preferences");
}

std::string read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    check(bool(input), "cannot read preferences");
    return std::string(std::istreambuf_iterator<char>(input), {});
}

void test_grid_preferences(const fs::path& path) {
    constexpr ui::designer_grid_settings defaults;
    check(ui::load_designer_grid_settings(path) == defaults && !fs::exists(path),
        "missing preferences did not use defaults or created a file");
    check(ui::save_designer_grid_settings(path, defaults) && ui::load_designer_grid_settings(path) == defaults,
        "could not create preferences from defaults");
    write(path, R"({"canvas_grid":["dots"]})");
    check(ui::load_designer_grid_settings(path) == defaults, "non-object grid settings did not use defaults");

    write(path, R"({"canvas_grid":{"visible":false,"style":"unknown","spacing":0.999999999,)"
        R"("major_every":7.5,"hide_below":64,"line_width":8.00000001,"background":"#12abEF80",)"
        R"("minor_color":"#0x1234","major_color":123,"axis_color":"#00000000"}})");
    auto partial = defaults;
    partial.visible = false;
    partial.hide_below = 64.f;
    partial.background = 0x8012ABEFu;
    partial.axis_color = 0;
    check(ui::load_designer_grid_settings(path) == partial, "invalid grid members did not default individually");

    write(path, R"({"language":"en-US","layout":{"left_width":300,"left_collapsed":true},)"
        R"("theme":"custom-theme","canvas_grid":{"extension":"grid-extension","major_every":18446744073709551615, "spacing":1e100}})");
    check(ui::load_designer_grid_settings(path) == defaults, "oversized numeric members did not default");

    const ui::designer_grid_settings low{false, ui::designer_grid_style::dots, 1.f, 1, 0.f, 0.25f,
        0x8012ABEFu, 0xFF345678u, 0x00123456u, 0u};
    check(ui::save_designer_grid_settings(path, low) && ui::load_designer_grid_settings(path) == low,
        "lower inclusive bounds, dots, or colors did not round-trip");
    const auto saved = read(path);
    check(ui::load_designer_language(path) == ui::designer_language::english &&
        ui::load_designer_dock_widths(path) == ui::designer_dock_widths{300, 345, true, false} &&
        saved.find("\"custom-theme\"") != std::string::npos && saved.find("\"grid-extension\"") != std::string::npos,
        "saving grid settings dropped unrelated preferences");
    check(saved.find("\"#12ABEF80\"") != std::string::npos && saved.find("\"#345678\"") != std::string::npos &&
        saved.find("\"#12345600\"") != std::string::npos && saved.find("\"#00000000\"") != std::string::npos,
        "saved colors did not use RGB with optional trailing alpha");

    auto high = defaults;
    high.spacing = 4096.f;
    high.major_every = 100;
    high.hide_below = 64.f;
    high.line_width = 8.f;
    check(ui::save_designer_grid_settings(path, high) && ui::load_designer_grid_settings(path) == high,
        "upper inclusive bounds or lines did not round-trip");
    check(ui::save_designer_language(path, ui::designer_language::simplified_chinese) &&
        ui::save_designer_dock_widths(path, {350, 400}) && ui::load_designer_grid_settings(path) == high,
        "saving other preferences overwrote grid settings");

    const auto before_invalid = read(path);
    auto invalid = high;
    invalid.style = static_cast<ui::designer_grid_style>(-1);
    check(!ui::save_designer_grid_settings(path, invalid), "invalid grid style was saved");
    invalid = high;
    invalid.spacing = std::numeric_limits<float>::quiet_NaN();
    check(!ui::save_designer_grid_settings(path, invalid), "non-finite spacing was saved");
    invalid = high;
    invalid.major_every = 101;
    check(!ui::save_designer_grid_settings(path, invalid), "out-of-range major interval was saved");
    invalid = high;
    invalid.hide_below = std::numeric_limits<float>::infinity();
    check(!ui::save_designer_grid_settings(path, invalid), "non-finite hide threshold was saved");
    invalid = high;
    invalid.line_width = 0.f;
    check(!ui::save_designer_grid_settings(path, invalid) && read(path) == before_invalid,
        "invalid grid values were saved or altered the preferences file");

    std::string large = R"({"canvas_grid":{"visible":false},"extension":")";
    large.append(65536 - large.size() - 2, 'a');
    large += "\"}";
    write(path, large);
    check(!ui::load_designer_grid_settings(path).visible && !ui::save_designer_grid_settings(path, high) &&
        read(path) == large, "saving an oversized result replaced readable preferences");

    for (const std::string_view broken : {"{broken", "[]"}) {
        write(path, broken);
        check(ui::load_designer_grid_settings(path) == defaults &&
            !ui::save_designer_grid_settings(path, high) && read(path) == broken,
            "malformed or non-object preferences were not guarded");
    }
    check(!ui::save_designer_grid_settings({}, high) && !ui::save_designer_grid_settings(path.parent_path(), high),
        "empty or directory preferences path was accepted");
}
} // namespace

int main() try {
    const temp_dir test_dir;
    test_grid_preferences(test_dir.path / "settings.json");
    std::puts("ui_designer_locale_persistence_test passed");
    return 0;
} catch (const std::exception& error) {
    std::fprintf(stderr, "ui_designer_locale_persistence_test: %s\n", error.what());
    return 1;
}
