#pragma once

#include "../gui/ui_design/model.h"
#include "ui_designer_locale.h"

#include <functional>
#include <memory>
#include <optional>

struct vgui_widget;

namespace vgui::ui_design {

using gradient_timeline_commit = std::function<bool(std::uint64_t, const std::vector<edit>&)>;

using keyframe_shape = std::array<double, gradient_geometry_channels.size()>;

enum class shape_kind { linear, radial, sweep };

struct shape_keyframe {
    std::uint64_t revision, key;
    std::string node;
    double time;
    shape_kind kind;
    bool terminal, ellipse;
    keyframe_shape shape;
    std::array<float, 6> affine; // XX, YX, XY, YY, TX, TY, including the key's translation.
    std::vector<gradient_stop> stops; // Effective palette at the key, before multiply tint.
    bool operator==(const shape_keyframe&) const = default;
};

struct capture_playhead {
    double seconds;
    bool inspect_end;
};

// A document projection. Timeline gestures edit one or more gradient nodes in
// a single session transaction; playhead/view state never enters the document.
class designer_gradient_timeline {
public:
    designer_gradient_timeline(vgui_widget* parent, const designer_strings&, gradient_timeline_commit,
        std::function<void(double, bool)> seek, std::function<void(std::string)> notify,
        std::function<void()> selection_changed, std::function<std::optional<capture_playhead>()> preview_time);
    ~designer_gradient_timeline();
    void set_visible(bool);
    void sync(const document&, std::uint64_t revision, std::string_view selected);
    void show_time(double);
    std::optional<shape_keyframe> selected_shape_key() const;
    void edit_shape(const shape_keyframe&, const keyframe_shape&);
    void edit_stop_offset(const shape_keyframe&, std::size_t index, float offset);
    void edit_stop_color(const shape_keyframe&, std::size_t index, const std::string& hex);
    void refresh_language();

private:
    struct impl;
    std::unique_ptr<impl> p_;
};

} // namespace vgui::ui_design
