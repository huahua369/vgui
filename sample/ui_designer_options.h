#pragma once

#include "ui_designer_locale.h"

#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <vgui/vgui.h>

namespace vgui::ui_design {

// Owns a modal options form. Destroy it before the UI context.
class designer_options {
public:
    designer_options();
    ~designer_options();

    void show(vgui_context*, const designer_strings&, designer_grid_settings initial, designer_theme theme,
        designer_dock_widths layout, std::string_view settings_path,
        std::function<void(designer_grid_settings)> preview_grid,
        std::function<void(designer_theme)> preview_theme,
        std::function<bool(std::optional<designer_grid_settings>, std::optional<designer_theme>)> save);

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

} // namespace vgui::ui_design
