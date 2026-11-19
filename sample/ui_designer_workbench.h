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
enum class designer_view_action { design, run, desktop, tablet, phone, fit, actual };

struct designer_commands {
    std::function<void()> new_document, open_document, save_document, save_as, export_package;
    std::function<void()> quit;
    std::function<void(designer_language)> set_language;
    std::function<void(designer_view_action)> view;
    std::function<bool(designer_view_action)> view_checked;
};

// The shell context, session and strings all outlive the workbench.
class designer_workbench {
public:
    designer_workbench(vgui_context*, session&, designer_commands, const designer_strings&);
    ~designer_workbench();

    vgui_widget* preview_host() const;
    std::string_view selection() const;
    void select(std::string id);

    // Call between event dispatch and layout/render; idle calls are cheap.
    void refresh();
    // Call after assigning the active locale to the existing designer_strings,
    // between event dispatch and layout/render. If an inspector edit is live,
    // its labels update on the first refresh() after that edit ends.
    void refresh_language();
    // Call after a successful New/Open, even if the revision appears unchanged.
    // Immediately selects the new root; widget updates wait for refresh().
    void document_replaced();
    void status(std::string message);
    void document_title(std::string title);

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

} // namespace vgui::ui_design
