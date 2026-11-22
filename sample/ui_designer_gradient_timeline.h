#pragma once

#include "../gui/ui_design/model.h"
#include "ui_designer_locale.h"

#include <functional>
#include <memory>

struct vgui_widget;

namespace vgui::ui_design {

using gradient_timeline_commit = std::function<bool(std::uint64_t, const std::vector<edit>&)>;

struct linear_keyframe {
    std::uint64_t revision, key;
    std::string node;
    double time;
    bool terminal;
    std::array<double, 4> endpoints;
    std::array<float, 6> affine; // XX, YX, XY, YY, TX, TY, including the key's translation.
    bool operator==(const linear_keyframe&) const = default;
};

// A document projection. Timeline gestures edit one or more gradient nodes in
// a single session transaction; playhead/view state never enters the document.
class designer_gradient_timeline {
public:
    designer_gradient_timeline(vgui_widget* parent, const designer_strings&, gradient_timeline_commit,
        std::function<void(double, bool)> seek, std::function<void(std::string)> notify,
        std::function<void()> selection_changed);
    ~designer_gradient_timeline();
    void set_visible(bool);
    void sync(const document&, std::uint64_t revision, std::string_view selected);
    void show_time(double);
    std::optional<linear_keyframe> selected_linear_key() const;
    void edit_endpoints(const linear_keyframe&, const std::array<double, 4>&);
    void refresh_language();

private:
    struct impl;
    std::unique_ptr<impl> p_;
};

} // namespace vgui::ui_design
