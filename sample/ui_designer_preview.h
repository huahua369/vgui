#pragma once

#include "../gui/ui_design/runtime.h"
#include "ui_designer_locale.h"
#include "ui_designer_gradient_timeline.h"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>

union SDL_Event;
struct vgpu_device_t;
struct vgpu_ctx_t;

namespace vgui::ui_design {

// Share glyph atlases and the bounded project-font cache across visible pages.
// The cache must outlive the previews borrowing its fonts.
class designer_preview_fonts {
public:
    designer_preview_fonts();
    ~designer_preview_fonts();
    designer_preview_fonts(const designer_preview_fonts&) = delete;
    designer_preview_fonts& operator=(const designer_preview_fonts&) = delete;

private:
    friend class designer_preview;
    struct impl;
    std::unique_ptr<impl> p_;
};

// Logical model values; an empty field keeps the source property unchanged.
struct geometry_edit {
    std::string id;
    std::uint64_t revision;
    std::optional<double> x, y, width, height;
    std::optional<double> anchor_right, anchor_bottom;
    bool resized = false; // A resize handle was dragged, even if only insets changed.
};

struct canvas_control_drop {
    std::string_view parent;
    std::optional<std::pair<double, double>> position; // X/Y in the parent's design units; empty for managed layout.
    pixel_rect hint; // Clipped shell-pixel bounds for the drag overlay.
};

using icon_source_rect = std::array<int, 4>; // X, Y, width, height in original image pixels.
inline constexpr const char* icon_source_fields[] = {
    "icon_source_x", "icon_source_y", "icon_source_width", "icon_source_height"
};
inline constexpr const char* indicator_source_fields[] = {
    "indicator_source_x", "indicator_source_y", "indicator_source_width", "indicator_source_height"
};
// Returns the modal panel or nullptr if the project image cannot be loaded. The
// close callback receives a rectangle only for an actual confirmed change.
vgui_widget* open_icon_region_picker(vgui_context*, const designer_strings&,
    const std::filesystem::path& project_root, const std::string& relative_path,
    icon_source_rect, bool four_state, std::function<void(std::optional<icon_source_rect>)> on_close);

// Owner-thread UI. Destroy before its parent, the shell context and the GPU.
// The strings provider must outlive the preview.
class designer_preview {
public:
    designer_preview(vgui_widget* parent, vgpu_device_t* gpu, font_familys_t* font, designer_preview_fonts&,
        std::function<void(std::string)> select, std::function<void(std::string)> notify,
        std::function<void(geometry_edit)> commit_geometry, const designer_strings& strings,
        gradient_timeline_commit commit_keys = {});
    ~designer_preview();
    designer_preview(const designer_preview&) = delete;
    designer_preview& operator=(const designer_preview&) = delete;

    // Call every frame with the current document. It is never retained or changed.
    void sync(const document&, uint64_t revision, const std::filesystem::path& project_root,
        std::string_view selection);
    // Bind the shell's VGPU context before rendering; native texture IDs live
    // in its texture table. It must use the constructor's device and outlive
    // this binding. The preview supports layout/input without a GPU.
    void set_render_context(vgpu_ctx_t*);
    // Call before the shell acquires its swapchain or records/submits a frame.
    void render(float dt);
    // Raw SDL events in window points, before shell shortcut/input dispatch.
    // Do not forward consumed events. Apply any non-null cursor they request.
    // DPI is read from the event's SDL window. Route gamepad button presses
    // only to the active preview for the foreground window.
    bool handle_event(const SDL_Event&, vgui_cursor& cursor);
    void set_navigation_editor(std::function<void(std::uint64_t, set_property)> commit);
    // Discard any open preview dialog and drop input ownership without invalidating a cached frame.
    void cancel_interaction();
    // Call after event dispatch, once the existing strings provider has been
    // assigned a new locale. Relabels shell controls without changing the mounted preview.
    void refresh_language();
    vgui_widget* widget() const;
    void set_control_drop_target(const vgui_drop_target&);
    void set_outline_locator(std::function<void(std::string)> locate);
    // The result borrows the mounted snapshot; use before the next sync/render.
    std::optional<canvas_control_drop> control_drop_at(const document&, std::uint64_t revision,
        const type_descriptor&, float x, float y) const;

    enum class preset { desktop, tablet, phone };
    struct safe_area_insets {
        int left = 0, top = 0, right = 0, bottom = 0; // Logical units.
        bool operator==(const safe_area_insets&) const = default;
    };
    struct animation_view {
        double time = 0, window_start = 0;
        bool playing = true;
        bool inspect_end = false; // Inspect a terminal key just before a loop wraps.
    };
    struct view_state {
        preset screen = preset::desktop;
        bool fit = true;
        float zoom = 1;
        float pan_x = 0, pan_y = 0;
        bool running = false;
        animation_view animation; // In-memory preview state, not saved to the document.
        bool timeline_open = false;
        std::optional<viewport> custom_viewport; // Absent when using a preset.
        safe_area_insets safe_area;
        std::map<std::string, value, std::less<>> simulation; // Page-local Run overrides, never serialized.
        bool hand_tool = false; // Page-local canvas input mode, never serialized.
        bool navigation_debug_open = false; // Page-local Run overlay, never serialized.
        bool navigation_edit_open = false; // Page-local Design tool, never serialized.
    };

    view_state capture_view() const;
    // Restore a saved or default view before syncing the next document.
    // Discards live input; snapshot() stays null until sync and render.
    void restore_view(const view_state&);

    void set_preset(preset);
    // No preset is selected while a custom viewport is active.
    std::optional<preset> current_preset() const;
    // Reset to Fit on a change. Invalid dimensions, DPI, insets or texture sizes
    // return false without changing the view. Like presets, this never edits the document.
    bool set_custom_viewport(viewport, safe_area_insets = {0, 0, 0, 0});
    void reset_zoom(bool fit);
    void set_running(bool);
    bool running() const;
    // Workspace-only appearance; independent of the document and saved view.
    void set_theme(designer_theme theme);
    void set_grid_settings(designer_grid_settings settings);
    designer_grid_settings grid_settings() const;
    // Borrowed; invalidated by sync/render or a preset/mode change.
    const layout_snapshot* snapshot() const;
    // Read-only views of the displayed board and private runtime widget state.
    std::optional<pixel_rect> artboard_rect_px() const;
    std::optional<std::string> edit_text(std::string_view id) const;
    std::optional<std::string> node_text(std::string_view id) const;

private:
    struct impl;
    std::unique_ptr<impl> p_;
};

} // namespace vgui::ui_design
