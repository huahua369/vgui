#pragma once

#include "ui_designer_locale.h"

#include <functional>
#include <memory>
#include <vgui/vgui.h>

namespace vgui::ui_design {

// Owns a modal options form. Destroy it before the UI context.
class designer_options {
public:
    designer_options();
    ~designer_options();

    void show(vgui_context*, const designer_strings&, designer_grid_settings initial,
        std::function<void(designer_grid_settings)> preview,
        std::function<bool(designer_grid_settings)> save);

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

} // namespace vgui::ui_design
