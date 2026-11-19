#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>

struct vgui_context;
struct vgui_widget;

namespace vgui::ui_design {

class session;
class designer_strings;
enum class designer_language;
struct designer_dock_widths;
enum class designer_view_action { design, run, desktop, tablet, phone, fit, actual };

struct designer_commands {
    std::function<void()> new_document, open_document, save_document, save_as, export_package;
    std::function<void()> quit;
    std::function<void(designer_language)> set_language;
    std::function<void(designer_view_action)> view;
    std::function<bool(designer_view_action)> view_checked;
    std::function<void()> close_document = {};
    std::function<void()> layout_changed = {}; // The host can persist workbench layout commands.
};

// The shell context, working session and strings all outlive the workbench.
class designer_workbench {
public:
    designer_workbench(vgui_context*, session&, designer_commands, const designer_strings&);
    ~designer_workbench();

    vgui_widget* preview_host() const;
    // Preferred side widths in logical pixels, even when the window constrains the actual layout.
    designer_dock_widths dock_widths() const;
    void set_dock_widths(designer_dock_widths widths);
    // Replaces the document chip with a native strip. The host owns tab state
    // and callbacks; add tabs with nullptr pages, not preview widgets.
    vgui_widget* enable_page_tabs();
    // Hide/show the document UI without replacing the working session. Call
    // between event dispatch and layout/render, after finishing outgoing edits.
    void set_page_active(bool active);
    std::string_view selection() const;
    void select(std::string id);

    // Commit the active property edit before moving out the working session.
    void finish_edit();
    // Call between event dispatch and refresh() after installing the next page,
    // even if the revision matches. Restores selection or falls back to the root.
    void document_replaced(std::string selection = {});
    // Call between event dispatch and layout/render; idle calls are cheap.
    void refresh();
    // Call after assigning the active locale to the existing designer_strings,
    // between event dispatch and layout/render. If an inspector edit is live,
    // its labels update on the first refresh() after that edit ends.
    void refresh_language();
    void status(std::string message);
    void document_title(std::string title);

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

} // namespace vgui::ui_design
