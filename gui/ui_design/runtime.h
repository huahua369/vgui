#pragma once

#include "model.h"
#include <vgui/vgui.h>

#include <memory>

namespace vgui::ui_design {

// Resolve a document-relative resource to a canonical regular file inside the project.
// Returns an empty path for missing files or targets outside the project (including symlinks).
std::filesystem::path resolve_project_resource(const std::filesystem::path& project_root, const std::string& relative);

struct runtime_bindings {
    // Borrowed texture from ctx. Keep it alive through mount(); the mounted
    // tree retains assigned textures. nullptr means unavailable.
    // Only called for a canonical regular file inside the canonical project
    // directory, once per referenced resource key per mount. The host must
    // keep that directory stable during path resolution and loading.
    using image_loader = vgui_texture* (*)(vgui_context* ctx, std::string_view key,
        const std::filesystem::path& file, void* user);
    using action_handler = void (*)(std::string_view id, std::string_view action, void* user) noexcept;

    std::filesystem::path project_root;
    image_loader load_image = nullptr;
    action_handler on_action = nullptr; // Button/menu activation or a native row-button press.
    void* user = nullptr; // Host-owned; must outlive action dispatch.

    // Borrowed family; keep it alive until the mounted widgets are destroyed
    // (including during remount). nullptr falls back to the context family.
    // Called once per referenced font key per mount. Built-ins get an empty
    // path; resources get a canonical regular file inside the project root,
    // with the same directory stability requirement as image_loader.
    using font_loader = font_familys_t* (*)(std::string_view key, const std::filesystem::path& file, void* user);
    font_loader load_font = nullptr;

    // Alternative borrowed texture loader for images held by the host in
    // memory. Called once per referenced image key per mount, with no path
    // resolution or fallback to load_image (even if this returns nullptr).
    // Same lifetime and context requirements as image_loader.
    using embedded_image_loader = vgui_texture* (*)(vgui_context* ctx, std::string_view key, void* user);
    embedded_image_loader load_embedded_image = nullptr;

    // Leave authored modal panels in the page layout and do not arm their commands.
    bool show_modal_design = false;
};

struct viewport {
    int width = 0, height = 0; // Logical units.
    float scale = 1.0f;       // Physical pixels per logical unit, [0.25, 4].
};

struct pixel_rect { float x = 0, y = 0, width = 0, height = 0; };

using gradient_geometry_draft = std::array<std::optional<float>, gradient_geometry_channels.size()>;
struct gradient_stop_offset {
    std::size_t index;
    float offset;
    bool operator==(const gradient_stop_offset&) const = default;
};
struct gradient_stop_color {
    std::size_t index;
    std::uint32_t color; // VGUI ARGB, before keyframe tint and opacity.
    bool operator==(const gradient_stop_color&) const = default;
};
struct gradient_keyframe_draft {
    gradient_geometry_draft geometry{}; // In document channel order.
    std::optional<gradient_stop_offset> stop;
    std::optional<gradient_stop_color> color;
    bool operator==(const gradient_keyframe_draft&) const = default;
};
struct gradient_shape_preview {
    std::string_view node;
    gradient_keyframe_draft draft;
};

struct node_layout {
    std::string id;
    pixel_rect rect_px;        // Absolute context pixels, directly from VGUI.
    pixel_rect visible_px;     // Bounds intersected with ancestor panels/viewport.
    bool visible = false;      // Effective tree visibility (may be clipped out).
    bool enabled = false;      // Effective tree enabled state.
    bool image_placeholder = false;
};

struct layout_snapshot {
    std::uint64_t revision = 0; // Source revision supplied to mount().
    std::uint64_t serial = 0;   // Increases per capture on this runtime instance.
    viewport view;
    int pixel_width = 0, pixel_height = 0;
    std::vector<node_layout> nodes; // Preorder in document child order; no handles.
};

struct data_target_snapshot {
    std::string node;
    std::string_view property; // Refers to the static binding schema.
    value authored, effective; // Design fallback and current widget-local value.
    std::string text_key; // Authored localization key, if any.
};

struct data_field_snapshot {
    value_type type;
    std::optional<value> published;
    std::vector<data_target_snapshot> targets; // Active bindings in mount order.
};

enum class data_update { transition, immediate };

// Timing for an absolute playhead across all the animations in a document.
// A gradient in keyframes mode contributes timing even with no keys.
struct animation_span {
    double duration = 0; // Longest configured clip, in seconds; zero if none.
    bool loops = false;
};

// Capture a serializable pose using the same sampler as runtime::seek_animations.
// Supply a validated track and its authored geometry/stops; time is local
// seconds in [0, duration], with the caller resolving playback loop boundaries.
gradient_keyframe capture_gradient_pose(std::string_view kind,
    const std::array<double, gradient_geometry_channels.size()>& authored,
    std::span<const gradient_stop> stops, std::span<const gradient_keyframe> keys,
    double time, double duration);

// Internal C++ adapter. Use on the VGUI owner thread with a dedicated context
// whose built-in root retains its default layout and padding. The caller owns
// the context; it must outlive runtime and all its mounts. Callbacks must not
// re-enter the runtime or mutate/destroy the mounted tree during dispatch.
class runtime {
public:
    explicit runtime(vgui_context* ctx);
    ~runtime();
    runtime(const runtime&) = delete;
    runtime& operator=(const runtime&) = delete;

    // Rebuilds just the document subtree. An invalid document leaves the old
    // mount intact. Recoverable image errors are reported in diagnostics();
    // collection rows stay mounted with default rendering for missing icons.
    // The document is not stored or mutated.
    bool mount(const document& doc, std::uint64_t revision, const runtime_bindings& bindings = {},
        diagnostic* error = nullptr);
    void unmount();
    // Borrowed until mount/unmount; do not reparent, restyle or destroy handles.
    vgui_widget* widget(std::string_view id) const;
    std::span<const diagnostic> diagnostics() const;

    // True if the mount has authored modal panels, including in Design.
    bool has_modals() const;
    // Service the first authored open/close command received since the last call.
    // Call once per frame on the owner thread, after input dispatch.
    void service_modals();

    // Publish a single field on the owner thread, outside input dispatch.
    // Returns false for an unknown field or invalid type/value. nullopt
    // immediately restores every target's authored value (including its text key).
    // Progress targets can opt into a transition; immediate snaps and cancels
    // any in-flight transition, even for the same publication. Otherwise a
    // same-value update is a no-op. Layout keeps live data; a remount starts fresh.
    // Neither publishing nor clearing changes the document.
    bool set_data(std::string_view field, std::optional<value> data,
        data_update update = data_update::transition);

    // Tick live value transitions independently of layout, visibility and
    // gradient playheads. Returns true only if a displayed value changed.
    bool advance_data_transitions(double delta_seconds);

    // Read a field from the current mount without changing widgets or layout.
    // Empty when unmounted or the field has no active bindings. The snapshot
    // owns its data (apart from static schema names) and survives remounting.
    std::optional<data_field_snapshot> inspect_data(std::string_view field) const;

    // Applies DPI + viewport then runs VGUI layout (no drawing). Empty if not
    // mounted or the view is invalid. A zero root width/height fills that axis
    // of the viewport; explicit dimensions and x/y are in logical units.
    // Relayout keeps live edit/focus state; remount restores document values.
    std::optional<layout_snapshot> layout(viewport view);

    // Focus the authored start node after a valid layout of this mount, only
    // when it is eligible and the context does not already have focus.
    // Returns false for an unset or unavailable start (no geometric fallback).
    bool focus_default();

    // Tick live value transitions, then advance mounted gradient animations on
    // the host clock, after layout and before drawing. For gradients, mount and
    // layout keep the authored paint and 0 samples the start pose. Hidden or
    // off-screen layers pause and one-shots hold their last value.
    void advance_animations(double delta_seconds);

    // Sample the visible animations at an absolute time after layout. Looping
    // effects use their own period; one-shots clamp to their final frame.
    animation_span animation_timing() const;
    // Include the terminal key when explicitly inspecting a keyframed loop's
    // boundary. Normal playback and seeking still wrap to the next cycle.
    // An optional editor draft changes active shape channels or one stop's
    // offset or color. Seeking without it restores the normal sampled paint.
    void seek_animations(double seconds, bool inspect_loop_end = false,
        const gradient_shape_preview* preview = nullptr);

private:
    struct mounted_tree;
    vgui_context* ctx_;
    std::unique_ptr<mounted_tree> tree_;
    std::uint64_t serial_ = 0;
};

} // namespace vgui::ui_design
