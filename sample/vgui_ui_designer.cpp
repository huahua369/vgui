// Standalone SDL3 workbench; the document and preview own no OS window.
#include "pch.h"

#include "delivery.h"
#include "ui_designer_delivery_smoke.h"
#include "ui_designer_export_isolation_smoke.h"
#include "ui_designer_font_import.h"
#include "ui_designer_geometry_smoke.h"
#include "ui_designer_image_smoke_fixture.h"
#include "ui_designer_locale.h"
#include "ui_designer_menu_smoke.h"
#include "ui_designer_options.h"
#include "ui_designer_pages_smoke.h"
#include "ui_designer_preview.h"
#include "ui_designer_workbench.h"

#include <vgui/backend/vgui_multiwindow.h>
#include <vgui/backend/vgui_render_target.h>
#include <SDL3/SDL_main.h>

#ifdef _WIN32
#include "ui_designer_native_dialog_exit_smoke.h"
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
namespace fs = std::filesystem;
namespace ui = vgui::ui_design;

std::string utf8(const fs::path& path) {
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}

std::string describe(const ui::diagnostic& error) {
    return error.path.empty() ? error.message : error.path + ": " + error.message;
}

enum class command { none, create, open, save, save_as, save_all, export_package, quit, close, import_font, options };

bool valid_package_name(std::string_view name) {
    // A single portable name component; the parent is chosen separately.
    return !name.empty() && name.size() <= 120 && name.front() != ' ' &&
        name.back() != '.' && name.back() != ' ' &&
        name.find_first_of("/\\:<>\"|?*") == std::string_view::npos &&
        std::none_of(name.begin(), name.end(), [](unsigned char c) { return c < 32 || c == 127; });
}

vgui_widget* find_widget(vgui_widget* w, std::string_view id, std::optional<vgui_widget_kind> kind = {}) {
    if (!w || (id == vgui_widget_get_id(w) && (!kind || vgui_widget_get_kind(w) == *kind))) return w;
    for (std::size_t i = 0; i < vgui_widget_get_child_count(w); ++i)
        if (auto* match = find_widget(vgui_widget_get_child(w, i), id, kind)) return match;
    return nullptr;
}

// The shell context is distinct from each page's document/render context.
vgui_theme light_shell_theme() {
    return {
        .text = 0xFF202D40, .panel_bg = 0xFFF4F7FB, .panel_border = 0xFFCCD6E2,
        .button_bg = 0xFFE8EEF6, .button_hover = 0xFFDBE8FA, .button_active = 0xFFC7DAF3,
        .button_border = 0xFFBACADC, .accent = 0xFF5886CD, .accent_hover = 0xFF4479C8,
        .track = 0xFFDAE1EB, .edit_bg = 0xFFFFFFFF, .edit_selection = 0x55387FD4,
        .scrollbar = 0xFFA4B1C3, .disabled = 0xFF68768A, .scrollbar_hover = 0xFF8093AC,
        // Menus and checkboxes share this ink color: contrast with both the
        // light menu surface and the checked control's blue fill.
        .checkmark = 0xFF14263E, .switch_thumb = 0xFFFFFFFF,
        .list_bg = 0xFFFFFFFF, .list_border = 0xFFCCD6E2, .row_text = 0xFF26354B,
        .row_hover = 0xFFF0F5FC, .row_selected = 0xFFD5E6FD, .row_selected_text = 0xFF173E76,
        .tree_guide = 0xFFCFD9E5,
        .menu_bg = 0xFFFFFFFF, .menu_border = 0xFFBFCBDD, .menu_hover = 0xFFE3EEFD,
        .menu_disabled_text = 0xFF68768A, .tooltip_bg = 0xFFFFFDF5,
        .tooltip_border = 0xFFCCC2A8, .tooltip_text = 0xFF273143,
        .modal_bg = 0xFFFFFFFF, .modal_border = 0xFFBFCBDD, .modal_danger = 0xFFB52939,
        .dock_bg = 0xFFEAF0F7, .dock_border = 0xFFCAD6E5, .dock_tab_bg = 0xFFE5EDF7,
        .dock_tab_active = 0xFFFFFFFF, .dock_tab_text = 0xFF273A55,
        .dock_floating_bg = 0xFFF5F8FC, .dock_floating_border = 0xFFBFCBDD,
        .dock_floating_title = 0xFFE2ECF8,
    };
}

class designer {
public:
    using message_box_fn = std::function<bool(const SDL_MessageBoxData*, int*)>;

    designer(os_window& window, vgpu_device_t* gpu, vgui_context* shell, font_familys_t* font,
             vgpu_ctx_t* renderer, const ui::designer_strings& strings, fs::path settings_path, message_box_fn message_box)
        : window_(window), shell_(shell), gpu_(gpu), font_(font), renderer_(renderer),
          strings_(strings), settings_path_(std::move(settings_path)),
          message_box_(std::move(message_box)),
          workbench_(shell, session_, {
              [this] { request(command::create); }, [this] { request(command::open); },
              [this] { request(command::save); }, [this] { request(command::save_as); },
              [this] { request(command::export_package); }, [this] { request(command::quit); },
              [this](ui::designer_language language) { pending_language_ = language; },
              [this](ui::designer_view_action action) { pending_view_ = action; },
              [this](ui::designer_view_action action) { return active_ && view_checked(action); },
              [this] { request(command::close); }, [this] { request_layout_save(); },
              [this] { request(command::save_all); }, [this] { return can_save_all(); },
              [this] { request(command::import_font); },
              [this] { request(command::options); },
              [this](ui::designer_theme theme) { pending_theme_ = theme; },
              [this] { open_icon_region(); }}, strings_), dock_(workbench_.document_dock()) {
        vgui_context_get_theme(shell_, &dark_shell_theme_);
        apply_theme(ui::load_designer_theme(settings_path_));
        grid_settings_ = ui::load_designer_grid_settings(settings_path_);
        saved_layout_ = ui::load_designer_dock_widths(settings_path_);
        workbench_.set_dock_widths(saved_layout_);
        vgui_dock_on_activate(dock_, [](vgui_widget*, vgui_item_id item, void* user) {
            auto& self = *static_cast<designer*>(user);
            if (!self.projecting_tabs_ && item != self.active_ && self.find_page(item)) self.pending_tab_ = item;
        }, this);
        vgui_dock_on_close_request(dock_, [](vgui_widget*, vgui_item_id item, void* user) {
            static_cast<designer*>(user)->request(command::close, item);
        }, this);
        update_empty();
    }

    ~designer() {
        cancel_icon_region();
        if (package_ && vgui_modal_panel(shell_) == package_->panel)
            vgui_modal_close(shell_, VGUI_MODAL_RESULT_CANCEL);
        vgui_dock_on_activate(dock_, nullptr, nullptr);
        vgui_dock_on_close_request(dock_, nullptr, nullptr);
        pages_.clear(); // Destroy previews before the workbench destroys their parents.
    }

    void request(command action, vgui_item_id page = 0) {
        if (chooser_ || pending_ != command::none || pending_tab_) return;
        if (icon_region_) {
            if (action != command::quit && action != command::close) return;
            cancel_icon_region();
        }
        if (package_ && vgui_modal_panel(shell_) == package_->panel) {
            if (action != command::quit) return;
            vgui_modal_close(shell_, VGUI_MODAL_RESULT_CANCEL);
        }
        pending_ = action;
        command_page_ = page ? page : active_;
    }
    bool quitting() const { return quitting_; }
    bool chooser_open() const { return chooser_; }
    void request_layout_save() { layout_save_pending_ = true; }
    bool tab_change_pending() const { return pending_tab_ != 0 || pending_ == command::close; }
    ui::designer_preview& preview() { return *find_page(active_)->preview; }
    const ui::designer_strings& strings() const { return strings_; }
    void report_bad_settings() {
        workbench_.status(tr("settings.invalid", "Invalid language settings; using Simplified Chinese."));
    }
    const ui::session& history() const { return session_; }
    std::string_view selection() const { return workbench_.selection(); }
    bool preview_event(const SDL_Event& event, vgui_cursor& cursor) {
        // An active drag-and-drop also owns keyboard events such as Escape.
        if (event.type != SDL_EVENT_WINDOW_FOCUS_LOST && vgui_drag_active(shell_)) return false;
        // The runtime preview must not steal mouse input from a shell press,
        // even on the first motion into the canvas before a drag has started.
        switch (event.type) {
        case SDL_EVENT_MOUSE_MOTION:
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
        case SDL_EVENT_MOUSE_WHEEL:
            if (vgui_context_has_pointer_capture(shell_)) return false;
            break;
        default: break;
        }
        // F10 belongs to the workbench even while a runtime input owns text focus.
        if ((event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP) && event.key.key == SDLK_F10)
            return false;
        // Canvas input is dispatched before the shell. Activate the page under
        // a press so that clicking a visible background page (or any floating
        // canvas) works without first having to click its title.
        if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.windowID == window_.id &&
            (event.button.button == SDL_BUTTON_LEFT || event.button.button == SDL_BUTTON_MIDDLE)) {
            const float ratio = window_.pixel_ratio();
            auto* hit = vgui_hit_test(shell_, std::round(event.button.x * ratio), std::round(event.button.y * ratio));
            for (; hit && hit != dock_; hit = vgui_widget_get_parent(hit)) {
                if (vgui_widget_get_parent(hit) != dock_) continue;
                const auto it = std::find_if(pages_.begin(), pages_.end(), [hit](const auto& page) { return page.host == hit; });
                if (it != pages_.end()) activate(it->id);
                break;
            }
        }
        auto* page = find_page(active_);
        return page && (page->last_model_revision == session_.revision() || event.type == SDL_EVENT_WINDOW_FOCUS_LOST) &&
            page->preview->handle_event(event, cursor);
    }

    bool open(const fs::path& path) {
        const auto file = document_path(path);
        if (!file) return false;
        if (const auto existing = find_path(*file)) {
            activate(existing);
        } else {
            ui::session loaded;
            ui::diagnostic error;
            if (!loaded.load(*file, &error)) {
                workbench_.status(std::string(tr("file.open_failed", "Open failed: ")) + describe(error));
                return false;
            }
            add_page(std::move(loaded), *file);
        }
        workbench_.status(std::string(tr("file.opened", "Opened ")) + utf8(*file));
        return true;
    }

    void new_page() {
        add_page(ui::session{}, {});
        workbench_.status(tr("file.new_hint", "New document. Save it in your project folder before adding relative image paths."));
    }

    void service() {
        if (std::exchange(layout_save_pending_, false)) save_layout();
        if (picked_) finish_chooser();
        if (std::exchange(package_browse_, false) && package_ && !chooser_)
            choose(VGUI_FILE_DIALOG_FOLDER, package_->page, command::export_package);
        if (package_name_) finish_package();
        if (auto selected = std::exchange(pending_language_, std::nullopt)) {
            if (ui::save_designer_language(settings_path_, *selected)) {
                if (*selected != strings_.language()) {
                    // Relabel after input dispatch; keep the document, dock and runtime alive.
                    strings_ = ui::designer_strings(*selected);
                    workbench_.refresh_language();
                    for (auto& page : pages_) if (page.preview) page.preview->refresh_language();
                }
                workbench_.status(tr("settings.saved", "Language preference applied and saved."));
            } else {
                workbench_.status(std::string(tr("settings.save_failed", "Cannot save language settings: ")) + utf8(settings_path_));
            }
        }
        if (auto theme = std::exchange(pending_theme_, std::nullopt); theme && *theme != theme_) {
            if (ui::save_designer_theme(settings_path_, *theme)) {
                apply_theme(*theme);
                workbench_.status(tr("settings.theme_saved", "Theme applied and saved."));
            } else {
                workbench_.status(std::string(tr("settings.theme_save_failed", "Cannot save theme: ")) + utf8(settings_path_));
            }
        }
        if (auto view = std::exchange(pending_view_, std::nullopt); view && active_) apply_view(*view);
        if (const auto tab = std::exchange(pending_tab_, 0); tab && !chooser_ && !package_) activate(tab);
        const auto action = std::exchange(pending_, command::none);
        const auto page = std::exchange(command_page_, 0);
        if (action == command::none) return;
        workbench_.finish_edit();
        switch (action) {
        case command::create: new_page(); break;
        case command::open: choose(VGUI_FILE_DIALOG_OPEN, page); break;
        case command::save:
        case command::save_as:
            if (auto* entry = find_page(page)) {
                if (action == command::save && !entry->path.empty()) save(page, entry->path);
                else choose(VGUI_FILE_DIALOG_SAVE, page);
            }
            break;
        case command::save_all:
            save_all_queue_.clear();
            queue_dirty_pages(save_all_queue_);
            continue_save_all();
            break;
        case command::export_package: show_package_options(page); break;
        case command::import_font:
            if (const auto* entry = find_page(page); entry && !entry->path.empty())
                choose(VGUI_FILE_DIALOG_OPEN, page, command::import_font);
            else workbench_.status(tr("font.save_first", "Save this page before importing a project font."));
            break;
        case command::close: close_page(page); break;
        case command::options:
            options_.show(shell_, strings_, grid_settings_, theme_, workbench_.dock_widths(), utf8(settings_path_),
                [this](ui::designer_grid_settings grid) { apply_grid_settings(grid); },
                [this](ui::designer_theme theme) { apply_theme(theme); },
                [this](std::optional<ui::designer_grid_settings> grid, std::optional<ui::designer_theme> theme) {
                    return ui::save_designer_options(settings_path_, grid, theme);
                });
            break;
        case command::quit:
            exit_queue_.clear();
            queue_dirty_pages(exit_queue_);
            continue_exit();
            break;
        default: break;
        }
    }

    void refresh() {
        auto title = std::string(tr("app.title", "VGUI UI Designer"));
        if (const auto* page = find_page(active_))
            title = page_title(*page) + (session_.dirty() ? " * — " : " — ") + title;
        if (title != window_title_) {
            window_title_ = std::move(title);
            SDL_SetWindowTitle(window_.window, window_title_.c_str());
        }
        for (const auto& page : pages_) {
            const auto label = page_title(page);
            if (label != vgui_dock_get_title(dock_, page.id)) vgui_dock_set_title(dock_, page.id, label.c_str());
            const bool dirty = page_session(page).dirty();
            if (dirty != (vgui_dock_get_unsaved(dock_, page.id) != 0)) vgui_dock_set_unsaved(dock_, page.id, dirty);
        }
        workbench_.refresh();
    }

    void render_preview(float dt) {
        const auto center = vgui_dock_get_active(dock_, VGUI_DOCK_CENTER);
        // Keep at most one recently used hidden preview, to avoid reallocating
        // the MSAA texture on every back-and-forth tab switch. Don't render it.
        for (auto& page : pages_) {
            if (page.id != center && !vgui_dock_is_floating(dock_, page.id)) {
                if (page.preview && page.id != cached_tab_) {
                    page.view = page.preview->capture_view();
                    page.preview.reset();
                }
                continue;
            }
            auto& view = ensure_preview(page);
            const auto& model = page_session(page);
            if (page.last_model_revision != model.revision()) {
                page.last_model_revision = model.revision();
                page.preview_revision = ++next_preview_revision_;
                page.last_key_revision.reset();
            }
            const auto selected = page.id == active_ ? workbench_.selection() : std::string_view(page.selection);
            view.sync(model.current(), page.preview_revision, resource_root(page), selected);
            view.render(dt);
        }
    }

    bool preview_ready(bool require_image) const {
        const auto* page = find_page(active_);
        const auto* snapshot = page && page->preview ? page->preview->snapshot() : nullptr;
        if (!snapshot || snapshot->revision != page->preview_revision || snapshot->nodes.size() != session_.current().nodes.size())
            return false;
        // The texture can render successfully even if a shell layout bug
        // collapses its canvas. Assert that the native viewport is usable too.
        auto* canvas = find_widget(page->preview->widget(), "designer.preview.canvas");
        if (!canvas) return false;
        float x, y, w, h;
        vgui_widget_get_rect(canvas, &x, &y, &w, &h);
        if (w < 100 || h < 100 || vgui_hit_test(vgui_widget_get_context(canvas), x + w / 2, y + h / 2) != canvas)
            return false;
        return !require_image || std::any_of(snapshot->nodes.begin(), snapshot->nodes.end(), [this](const auto& n) {
            return session_.current().nodes.at(n.id).type == "image" && n.visible && !n.image_placeholder;
        });
    }

private:
    const char* tr(std::string_view key, const char* fallback) const { return strings_.text(key, fallback); }

    void apply_theme(ui::designer_theme theme) {
        if (theme_ == theme) return;
        theme_ = theme;
        const auto palette = theme_ == ui::designer_theme::light ? light_shell_theme() : dark_shell_theme_;
        vgui_context_set_theme(shell_, &palette);
        workbench_.set_theme(theme_);
        for (auto& page : pages_) if (page.preview) page.preview->set_theme(theme_);
        vgui_widget_mark_dirty(vgui_root(shell_));
    }

    void apply_grid_settings(ui::designer_grid_settings grid) {
        if (grid_settings_ == grid) return;
        grid_settings_ = grid;
        for (auto& page : pages_) if (page.preview) page.preview->set_grid_settings(grid_settings_);
    }

    void save_layout() {
        const auto widths = workbench_.dock_widths();
        // Converting pixels back to logical widths can introduce DPI rounding noise.
        if (std::abs(widths.left - saved_layout_.left) < .01f &&
            std::abs(widths.right - saved_layout_.right) < .01f &&
            widths.left_collapsed == saved_layout_.left_collapsed &&
            widths.right_collapsed == saved_layout_.right_collapsed) return;
        if (ui::save_designer_dock_widths(settings_path_, widths)) saved_layout_ = widths;
        else workbench_.status(std::string(tr("settings.layout_save_failed", "Cannot save layout widths: ")) + utf8(settings_path_));
    }

    using view_action = ui::designer_view_action;
    using preset = ui::designer_preview::preset;

    void apply_view(view_action action) {
        switch (action) {
        case view_action::design: preview().set_running(false); break;
        case view_action::run: preview().set_running(true); break;
        case view_action::desktop: preview().set_preset(preset::desktop); break;
        case view_action::tablet: preview().set_preset(preset::tablet); break;
        case view_action::phone: preview().set_preset(preset::phone); break;
        case view_action::fit: preview().reset_zoom(true); break;
        case view_action::actual: preview().reset_zoom(false); break;
        }
    }

    bool view_checked(view_action action) const {
        const auto& view = *find_page(active_)->preview;
        switch (action) {
        case view_action::design: return !view.running();
        case view_action::run: return view.running();
        case view_action::desktop: return view.current_preset() == preset::desktop;
        case view_action::tablet: return view.current_preset() == preset::tablet;
        case view_action::phone: return view.current_preset() == preset::phone;
        default: return false;
        }
    }

    struct file_result {
        vgui_file_dialog_kind kind;
        std::string path, error;
    };

    void commit_geometry(vgui_item_id id, ui::geometry_edit change) {
        // Input may follow another edit in the same SDL event batch. Do not
        // apply deltas computed from a stale preview to the current document.
        const auto* page = find_page(id);
        if (!page || id != active_ || change.revision != page->preview_revision ||
            page->last_model_revision != session_.revision()) return;
        const auto& doc = session_.current();
        const auto target = doc.nodes.find(change.id);
        if (target == doc.nodes.end() || change.id == doc.root) return;
        const auto parent = std::find_if(doc.nodes.begin(), doc.nodes.end(), [&](const auto& entry) {
            const auto& children = entry.second.children;
            return std::find(children.begin(), children.end(), change.id) != children.end();
        });
        if (parent == doc.nodes.end() || parent->second.type != "panel") return;
        const auto layout = parent->second.props.find("layout");
        const auto& mode = layout == parent->second.props.end() ?
            ui::find_property("panel", "layout")->default_value : layout->second;
        if (std::get<std::string>(mode) != "absolute") return;

        std::vector<ui::edit> batch;
        for (const auto& [key, number] : {std::pair{"x", change.x}, std::pair{"y", change.y},
                                         std::pair{"width", change.width}, std::pair{"height", change.height},
                                         std::pair{"anchor_right", change.anchor_right}, std::pair{"anchor_bottom", change.anchor_bottom}}) {
            if (!number) continue;
            const auto& old = target->second.props;
            const auto& def = ui::find_property(target->second.type, key)->default_value;
            const auto it = old.find(key);
            const ui::value next = *number;
            if ((it == old.end() ? def : it->second) == next) continue;
            batch.emplace_back(ui::set_property{change.id, key,
                next == def ? std::nullopt : std::optional<ui::value>(next)});
        }
        if (batch.empty()) return;
        ui::diagnostic error;
        if (!session_.apply(batch, &error)) workbench_.status(std::string(tr("geometry.failed", "Geometry edit failed: ")) + describe(error));
        else workbench_.status(std::string(change.resized ? tr("geometry.resized", "Resized ") : tr("geometry.moved", "Moved ")) + change.id);
    }

    bool commit_keyframes(vgui_item_id id, std::uint64_t revision, const std::vector<ui::edit>& edits) {
        auto* page = find_page(id);
        if (!page || id != active_ || revision != page->preview_revision ||
            (session_.revision() != page->last_model_revision && session_.revision() != page->last_key_revision)) return false;
        ui::diagnostic error;
        if (!session_.apply(edits, &error)) {
            workbench_.status(std::string(tr("timeline.failed", "Keyframe edit failed: ")) + describe(error));
            return false;
        }
        page->last_key_revision = session_.revision();
        return true;
    }

    void commit_navigation(vgui_item_id id, std::uint64_t revision, ui::set_property change) {
        const auto* page = find_page(id);
        if (!page || id != active_ || !page->preview || page->preview->running() ||
            page->preview_revision != revision || page->last_model_revision != session_.revision()) return;
        const bool initial = change.key == "nav_start";
        const bool return_focus = change.key == "modal_return_focus";
        const bool open_modal = change.key == "open_modal";
        const bool close_modal = change.key == "close_modal";
        if ((return_focus || open_modal || close_modal) && change.id != workbench_.selection()) return;
        std::string target;
        if (initial) {
            const auto& doc = session_.current();
            const auto& root_props = doc.nodes.at(doc.root).props;
            if (change.data) target = std::get<std::string>(*change.data);
            const auto previous = root_props.find("nav_start");
            const std::string_view selected = change.data ? target :
                previous != root_props.end() ? std::get<std::string>(previous->second) : std::string_view{};
            if (selected.empty() || selected != workbench_.selection()) return; // Ignore stale toolbar selection.
        }
        if ((return_focus || open_modal) && change.data) target = std::get<std::string>(*change.data);
        workbench_.finish_edit();
        const bool linked = change.data.has_value();
        const auto link = change.id + " · " + change.key;
        const ui::edit change_property = std::move(change);
        ui::diagnostic error;
        if (!session_.apply(std::span{&change_property, 1}, &error)) {
            workbench_.status(std::string(tr("preview.navigation_edit.failed", "Navigation link edit failed: ")) + describe(error));
        } else if (initial) {
            workbench_.status(linked ? std::string(tr("preview.navigation_start.saved", "Initial focus:")) + " " + target :
                tr("preview.navigation_start.cleared", "Initial focus cleared"));
        } else if (return_focus) {
            workbench_.status(linked ? std::string(tr("preview.navigation_return.saved", "Return focus:")) + " " + target :
                tr("preview.navigation_return.cleared", "Return focus cleared"));
        } else if (open_modal) {
            workbench_.status(linked ? std::string(tr("preview.navigation_open.saved", "Opens modal:")) + " " + target :
                tr("preview.navigation_open.cleared", "Open target cleared"));
        } else if (close_modal) {
            workbench_.status(linked ? tr("preview.navigation_close.enabled", "Close modal enabled") :
                tr("preview.navigation_close.disabled", "Close modal disabled"));
        } else {
            workbench_.status(std::string(linked ? tr("preview.navigation_edit.linked", "Linked") :
                tr("preview.navigation_edit.cleared", "Cleared")) + " " + link);
        }
    }

    struct page_state {
        vgui_item_id id = 0;
        std::uint64_t untitled_number = 0;
        fs::path path;
        // The active session resides in session_, whose address the workbench
        // keeps. Park it here before switching; hidden pages never render.
        ui::session history;
        std::string selection;
        ui::designer_preview::view_state view;
        vgui_widget* host = nullptr; // Owned by the dock.
        std::unique_ptr<ui::designer_preview> preview;
        std::optional<std::uint64_t> last_model_revision;
        std::uint64_t preview_revision = 0;
        std::optional<std::uint64_t> last_key_revision;
    };

    page_state* find_page(vgui_item_id id) {
        const auto it = std::find_if(pages_.begin(), pages_.end(), [id](const auto& page) { return page.id == id; });
        return it == pages_.end() ? nullptr : &*it;
    }

    const page_state* find_page(vgui_item_id id) const {
        const auto it = std::find_if(pages_.begin(), pages_.end(), [id](const auto& page) { return page.id == id; });
        return it == pages_.end() ? nullptr : &*it;
    }

    void queue_dirty_pages(auto& queue) const {
        const int count = vgui_dock_get_zone_count(dock_, VGUI_DOCK_CENTER);
        for (int i = 0; i < count; ++i) {
            const auto id = vgui_dock_get_zone_at(dock_, VGUI_DOCK_CENTER, i);
            if (const auto* page = find_page(id); page && page_session(*page).dirty()) queue.push_back(id);
        }
        // Focus raises floating frames; it must not change the order of Save All or Exit.
        for (const auto& page : pages_)
            if (vgui_dock_is_floating(dock_, page.id) && page_session(page).dirty()) queue.push_back(page.id);
    }

    static bool same_path(const fs::path& a, const fs::path& b) {
        if (a == b) return true;
        std::error_code error;
        return fs::equivalent(a, b, error);
    }

    vgui_item_id find_path(const fs::path& path) const {
        for (const auto& page : pages_) if (!page.path.empty() && same_path(page.path, path)) return page.id;
        return 0;
    }

    ui::session& page_session(page_state& page) { return page.id == active_ ? session_ : page.history; }
    const ui::session& page_session(const page_state& page) const { return page.id == active_ ? session_ : page.history; }

    fs::path resource_root(const page_state& page) const {
        return page.path.empty() ? new_root_ : page.path.parent_path();
    }

    std::string page_title(const page_state& page) const {
        if (!page.path.empty()) return utf8(page.path.filename());
        return std::string(tr("file.untitled", "Untitled")) + " " + std::to_string(page.untitled_number);
    }

    void open_icon_region() {
        const auto* page = find_page(active_);
        if (!page || chooser_ || icon_region_ || vgui_modal_active(shell_)) return;
        const std::string node_id(workbench_.selection());
        const auto& doc = session_.current();
        const auto node = doc.nodes.find(node_id);
        if (node == doc.nodes.end()) return;
        const bool indicator = node->second.type == "checkbox" || node->second.type == "radio";
        if (!indicator && node->second.type != "button") return;
        const auto* fields = indicator ? ui::indicator_source_fields : ui::icon_source_fields;
        const auto icon = node->second.props.find(indicator ? "indicator_atlas" : "icon");
        if (icon == node->second.props.end()) return;
        const auto resource = doc.resources.find(std::get<std::string>(icon->second));
        if (resource == doc.resources.end()) return;
        ui::icon_source_rect authored{};
        for (std::size_t i = 0; i < authored.size(); ++i) {
            const auto it = node->second.props.find(fields[i]);
            if (it != node->second.props.end()) authored[i] = static_cast<int>(std::lround(std::get<double>(it->second)));
        }
        icon_region_ = ui::open_icon_region_picker(shell_, strings_, resource_root(*page), resource->second, authored, indicator,
            [this, page_id = page->id, rev = session_.revision(), node_id, fields, indicator](std::optional<ui::icon_source_rect> region) {
                icon_region_ = nullptr;
                if (!region || page_id != active_ || rev != session_.revision() || node_id != workbench_.selection()) return;
                std::vector<ui::edit> batch;
                for (std::size_t i = 0; i < region->size(); ++i) {
                    const auto value = (*region)[i];
                    batch.emplace_back(ui::set_property{node_id, fields[i],
                        value ? std::optional<ui::value>(double(value)) : std::nullopt});
                }
                ui::diagnostic error;
                if (!session_.apply(batch, &error)) workbench_.status(describe(error));
                else workbench_.status(indicator ? tr("indicator_region.applied", "Four-state region updated.")
                    : tr("icon_region.applied", "Button icon region updated."));
            });
        if (!icon_region_)
            workbench_.status(indicator ? tr("indicator_region.unavailable", "Cannot read this atlas. Check that it is a supported image inside the project.")
                : tr("icon_region.unavailable", "Cannot read this icon image. Check that it is a supported image inside the project."));
    }

    void cancel_icon_region() {
        if (icon_region_ && vgui_modal_panel(shell_) == icon_region_)
            vgui_modal_close(shell_, VGUI_MODAL_RESULT_CANCEL);
    }

    std::optional<ui::canvas_control_drop> control_drop_at(const ui::type_descriptor& type, float x, float y) {
        const auto* page = find_page(active_);
        if (!page || !page->preview || page->last_model_revision != session_.revision()) return {};
        return page->preview->control_drop_at(session_.current(), page->preview_revision, type, x, y);
    }

    ui::designer_preview& ensure_preview(page_state& page) {
        if (page.preview) return *page.preview;
        const auto id = page.id;
        page.preview = std::make_unique<ui::designer_preview>(page.host, gpu_, font_, preview_fonts_,
            [this, id](std::string node) { if (active_ == id) workbench_.select(std::move(node)); },
            [this, id](std::string message) { if (active_ == id) workbench_.status(std::move(message)); },
            [this, id](ui::geometry_edit change) { commit_geometry(id, std::move(change)); }, strings_,
            [this, id](auto revision, const auto& edits) { return commit_keyframes(id, revision, edits); });
        page.preview->set_navigation_editor([this, id](auto revision, ui::set_property change) {
            commit_navigation(id, revision, std::move(change));
        });
        page.preview->set_binding_locator([this, id](std::string node) {
            if (active_ == id) workbench_.locate_in_outline(std::move(node));
        });
        page.preview->set_control_drop_target({
            [](void* user, const vgui_drag_payload* payload) -> int {
                auto& self = *static_cast<designer*>(user);
                const auto* active = self.find_page(self.active_);
                return active && active->preview && !active->preview->running() && self.workbench_.dragged_control(payload);
            },
            [](void* user, const vgui_drag_payload* payload, float x, float y, vgui_drop_hint* hint) {
                auto& self = *static_cast<designer*>(user);
                if (const auto* type = self.workbench_.dragged_control(payload))
                    if (const auto drop = self.control_drop_at(*type, x, y); drop && drop->hint.width > 0 && drop->hint.height > 0) {
                        const auto& r = drop->hint;
                        *hint = {VGUI_DROP_HINT_RECT, r.x, r.y, r.width, r.height};
                    }
            },
            [](void* user, const vgui_drag_payload* payload, float x, float y) -> int {
                auto& self = *static_cast<designer*>(user);
                const auto* type = self.workbench_.dragged_control(payload);
                if (!type) return 0;
                const auto drop = self.control_drop_at(*type, x, y);
                return drop && self.workbench_.insert_control(type->key, drop->parent, drop->position);
            },
            this
        });
        page.preview->set_render_context(renderer_);
        page.preview->set_grid_settings(grid_settings_);
        page.preview->set_theme(theme_);
        page.preview->restore_view(page.view);
        page.last_model_revision.reset();
        return *page.preview;
    }

    void activate(vgui_item_id id) {
        auto* next = find_page(id);
        if (!next || id == active_) return;
        cancel_icon_region();
        workbench_.finish_edit();
        if (auto* current = find_page(active_)) {
            // When a tab switch hides this canvas, keep it as the single
            // cache entry. Floating previews do not occupy that slot.
            if (!vgui_dock_is_floating(dock_, active_) && !vgui_dock_is_floating(dock_, id))
                cached_tab_ = current->id;
            current->selection = workbench_.selection();
            current->view = current->preview->capture_view();
            // Cancel the old capture/IME. As before, switching pages resets
            // temporary runtime input without touching the design document.
            if (current->view.running) current->preview->restore_view(current->view);
            else current->preview->cancel_interaction();
            current->history = std::move(session_);
        }
        if (vgui_get_focus(shell_) != dock_) vgui_set_focus(shell_, nullptr);
        active_ = id;
        session_ = std::move(next->history);
        projecting_tabs_ = true;
        vgui_dock_activate(dock_, id);
        projecting_tabs_ = false;
        ensure_preview(*next);
        workbench_.set_page_active(true);
        workbench_.document_replaced(next->selection);
        // An edit in the outgoing inspector must not retain an SDL text session.
        window_.stop_text_input();
    }

    void add_page(ui::session model, fs::path path) {
        auto* host = vgui_widget_create(shell_, VGUI_PANEL, nullptr);
        vgui_widget_set_id(host, "designer.preview.page");
        vgui_widget_set_layout(host, VGUI_LAYOUT_COLUMN);
        vgui_widget_set_align_items(host, VGUI_ALIGN_STRETCH);
        vgui_widget_set_padding(host, 0, 0, 0, 0);
        vgui_widget_set_background(host, 0);
        vgui_widget_set_border(host, 0, 0);
        const auto number = path.empty() ? next_untitled_++ : 0;
        page_state page{.untitled_number = number, .path = std::move(path),
            .history = std::move(model), .host = host};
        const auto label = page_title(page);
        vgui_dock_panel_desc desc;
        vgui_dock_panel_desc_default(&desc);
        desc.title = label.c_str();
        desc.panel = host;
        desc.zone = VGUI_DOCK_CENTER;
        page.id = vgui_dock_add(dock_, &desc);
        if (!page.id) { vgui_widget_destroy(host); return; }
        if (!page.path.empty()) vgui_dock_set_tooltip(dock_, page.id, utf8(page.path).c_str());
        const auto id = page.id;
        pages_.push_back(std::move(page));
        activate(id);
    }

    void update_empty() {
        workbench_.set_page_active(false);
        workbench_.status(tr("file.empty_workspace", "No page open. Use File to open or create a page."));
    }

    void remove_page(vgui_item_id id) {
        auto* page = find_page(id);
        if (!page) return;
        if (id == active_) {
            vgui_set_focus(shell_, nullptr);
            active_ = 0;
            session_ = ui::session{};
            workbench_.document_replaced();
            window_.stop_text_input();
        }
        if (cached_tab_ == id) cached_tab_ = 0;
        page->preview.reset(); // Before its dock parent is destroyed.
        projecting_tabs_ = true;
        vgui_dock_remove(dock_, id);
        projecting_tabs_ = false;
        std::erase_if(pages_, [id](const auto& page) { return page.id == id; });
        if (!active_) {
            if (const auto next = vgui_dock_get_active(dock_, VGUI_DOCK_CENTER)) activate(next);
            else if (const auto floating = vgui_dock_get_active(dock_, VGUI_DOCK_FLOATING)) activate(floating);
            else update_empty();
        }
    }

    std::optional<fs::path> document_path(const fs::path& path) {
        std::error_code ec;
        const auto absolute = fs::absolute(path, ec);
        if (!ec) {
            auto folder = fs::canonical(absolute.parent_path(), ec);
            if (!ec) return folder / absolute.filename();
        }
        workbench_.status(std::string(tr("file.resolve_failed", "Cannot resolve document folder: ")) + ec.message());
        return std::nullopt;
    }

    bool save(vgui_item_id id, const fs::path& path) {
        auto* page = find_page(id);
        if (!page) return false;
        const auto file = document_path(path);
        if (!file) return false;
        if (const auto other = find_path(*file); other && other != id) {
            workbench_.status(tr("file.save_as_open", "Save As cannot replace another open page."));
            return false;
        }
        auto& history = page_session(*page);
        // No implicit asset copying or reinterpretation of relative paths.
        if (!history.current().resources.empty() && file->parent_path() != resource_root(*page)) {
            workbench_.status(tr("file.save_as_root", "Save As must stay in the project folder while relative assets are registered."));
            return false;
        }
        ui::diagnostic error;
        if (!history.save(*file, &error)) {
            workbench_.status(std::string(tr("file.save_failed", "Save failed: ")) + describe(error));
            return false;
        }
        page->path = *file;
        const auto path_text = utf8(*file);
        vgui_dock_set_tooltip(dock_, id, path_text.c_str());
        workbench_.status(std::string(tr("file.saved", "Saved ")) + path_text);
        return true;
    }

    bool can_save_all() const {
        return !chooser_ && std::any_of(pages_.begin(), pages_.end(),
            [this](const auto& page) { return page_session(page).dirty(); });
    }

    void continue_save_all() {
        // Iterate stable page IDs in tab order without switching the active page.
        // Each completed save stands on its own if a later page cannot be saved.
        while (!save_all_queue_.empty()) {
            const auto id = save_all_queue_.front();
            save_all_queue_.pop_front();
            const auto* page = find_page(id);
            if (!page || !page_session(*page).dirty()) continue;
            if (page->path.empty()) {
                if (!choose(VGUI_FILE_DIALOG_SAVE, id, command::save_all)) save_all_queue_.clear();
                return;
            }
            if (!save(id, page->path)) {
                save_all_queue_.clear();
                return;
            }
        }
        workbench_.status(tr("file.save_all_done", "All changes saved."));
    }

    void close_page(vgui_item_id id) {
        auto* page = find_page(id);
        if (!page) return;
        if (page_session(*page).dirty()) {
            const int answer = confirm_unsaved(*page);
            if (answer == 1) {
                if (page->path.empty()) { choose(VGUI_FILE_DIALOG_SAVE, id, command::close); return; }
                if (!save(id, page->path)) return;
            } else if (answer != 2) return;
        }
        remove_page(id);
    }

    void continue_exit() {
        // Do not remove any pages until all decisions succeed. Discard choices
        // cannot destroy a page if a subsequent prompt is cancelled.
        while (!exit_queue_.empty()) {
            const auto id = exit_queue_.front();
            exit_queue_.erase(exit_queue_.begin());
            auto* page = find_page(id);
            if (!page || !page_session(*page).dirty()) continue;
            const int answer = confirm_unsaved(*page);
            if (answer == 1) {
                if (page->path.empty()) {
                    if (!choose(VGUI_FILE_DIALOG_SAVE, id, command::quit)) exit_queue_.clear();
                    return;
                }
                if (save(id, page->path)) continue;
            } else if (answer == 2) continue;
            exit_queue_.clear();
            return;
        }
        quitting_ = true;
    }

    int confirm_unsaved(const page_state& page) {
        const SDL_MessageBoxButtonData buttons[] = {
            {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, tr("common.cancel", "Cancel")},
            {0, 2, tr("file.discard", "Discard")},
            {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, tr("file.save", "Save")}};
        const auto message = page_title(page) + "\n\n" + tr("file.confirm_save", "Save changes to this design before continuing?");
        const SDL_MessageBoxData box{SDL_MESSAGEBOX_WARNING, window_.window, tr("file.unsaved", "Unsaved changes"),
            message.c_str(), 3, buttons, nullptr};
        int selected = 0;
        if (!message_box_(&box, &selected)) {
            workbench_.status(std::string(tr("file.confirm_failed", "Cannot show unsaved-changes prompt: ")) + SDL_GetError());
            return 0;
        }
        return selected;
    }

    static void on_pick(void* user, vgui_file_dialog_kind kind,
                        const char* const* paths, int count, const char* error) {
        static_cast<designer*>(user)->picked_ = file_result{kind,
            paths && count > 0 ? paths[0] : "", error ? error : ""};
    }

    void finish_font_import(vgui_item_id id, const fs::path& source) {
        auto* page = find_page(id);
        if (!page || page->path.empty()) return;
        auto& history = page_session(*page);
        std::string error;
        auto imported = ui::import_project_font(resource_root(*page), source, error);
        if (!imported) {
            workbench_.status(std::string(tr("font.import_failed", "Font import failed: ")) + error);
            return;
        }
        const auto& resources = history.current().resources;
        const auto existing = std::find_if(resources.begin(), resources.end(), [&](const auto& entry) {
            return !ui::is_builtin_font(entry.first) && entry.second == imported->relative_path;
        });
        if (existing == resources.end()) {
            std::string key;
            for (std::size_t i = 1; i <= ui::max_resources + 1; ++i) {
                key = "font_" + std::to_string(i);
                if (!resources.contains(key)) break;
            }
            ui::diagnostic diagnostic;
            const ui::edit add = ui::set_resource{std::move(key), imported->relative_path};
            if (!history.apply(std::span<const ui::edit>(&add, 1), &diagnostic)) {
                if (!imported->copied_file.empty()) {
                    std::error_code ignored;
                    fs::remove(imported->copied_file, ignored);
                }
                workbench_.status(std::string(tr("font.import_failed", "Font import failed: ")) + describe(diagnostic));
                return;
            }
        }
        workbench_.status(std::string(tr("font.imported", "Project font available in the Font family list: ")) + imported->relative_path);
    }

    bool choose(vgui_file_dialog_kind kind, vgui_item_id id, command followup = command::none) {
        const auto* page = find_page(id);
        const bool import_font = followup == command::import_font;
        const vgui_file_filter filters[] = {{import_font ? tr("font.filter", "TrueType/OpenType font") :
            tr("file.filter_design", "VGUI design"), import_font ? "*.ttf;*.otf" : "*.vgui.json"},
            {tr("file.filter_all", "All files"), "*"}};
        const auto location = utf8(followup == command::export_package && package_ ? package_->parent :
            page ? resource_root(*page) : new_root_);
        const bool batch = page && followup == command::save_all;
        const auto name = page && !page->path.empty() ? utf8(page->path.filename()) : batch ?
            "untitled-" + std::to_string(page->untitled_number) + ".vgui.json" : "untitled.vgui.json";
        const auto batch_title = batch ? std::string(tr("file.save_page_prefix", "Save page: ")) + page_title(*page) : std::string{};
        vgui_file_dialog_desc desc;
        vgui_file_dialog_desc_init(&desc);
        desc.title = import_font ? tr("font.dialog", "Import Project Font") : batch ? batch_title.c_str() :
            kind == VGUI_FILE_DIALOG_FOLDER ? tr("export.choose_parent", "Choose parent folder for the package") :
            kind == VGUI_FILE_DIALOG_SAVE ? tr("file.save_dialog", "Save UI design") : tr("file.open_dialog", "Open UI design");
        desc.default_location = location.c_str();
        if (kind == VGUI_FILE_DIALOG_FOLDER) desc.accept_label = tr("export.choose_folder", "Choose Folder");
        else {
            desc.default_name = name.c_str();
            desc.filters = filters;
            desc.filter_count = 2;
        }
        chooser_ = vgui_file_dialog_show(kind, &desc, on_pick, this) != 0;
        chooser_page_ = chooser_ ? id : 0;
        chooser_followup_ = chooser_ ? followup : command::none;
        if (!chooser_) workbench_.status(tr("file.chooser_failed", "The native file chooser could not be opened."));
        return chooser_;
    }

    struct package_request {
        vgui_item_id page;
        ui::document doc;
        fs::path root, parent;
        bool embedded = false;
        vgui_widget *panel = nullptr, *directory = nullptr, *single = nullptr, *location = nullptr;
        vgui_widget *name = nullptr, *summary = nullptr, *hint = nullptr, *error = nullptr;
        vgui_widget *browse = nullptr, *cancel = nullptr, *confirm = nullptr;
    };

    static void package_closed(vgui_context* ctx, vgui_widget*, int result, void* user) {
        auto& self = *static_cast<designer*>(user);
        self.package_.reset();
        self.package_name_.reset();
        self.package_browse_ = false;
        if (std::exchange(self.package_text_input_, false) && !vgui_context_wants_text_input(ctx))
            self.window_.stop_text_input();
        if (result != VGUI_MODAL_RESULT_CONFIRM)
            self.workbench_.status(self.tr("export.cancelled", "Package export cancelled."));
    }

    static void package_event(vgui_widget* widget, const vgui_event* event, void* user) {
        auto& self = *static_cast<designer*>(user);
        if (!self.package_ || self.chooser_) return;
        auto& p = *self.package_;
        if ((event->type == VGUI_EVENT_CLICK && widget == p.confirm) ||
            (event->type == VGUI_EVENT_SUBMIT && widget == p.name)) {
            if (vgui_widget_get_enabled(p.confirm)) self.package_name_ = vgui_edit_get_text(p.name);
        } else if (event->type == VGUI_EVENT_CLICK) {
            if (widget == p.browse) self.package_browse_ = true;
            else if (widget == p.cancel) vgui_modal_close(self.shell_, VGUI_MODAL_RESULT_CANCEL);
            else if (widget == p.directory || widget == p.single) {
                p.embedded = widget == p.single;
                self.refresh_package_summary();
            }
        }
    }

    void package_error(const std::string& message) {
        vgui_widget_set_text(package_->error, message.c_str());
        vgui_widget_set_tooltip(package_->error, message.c_str());
    }

    void refresh_package_summary() {
        auto& p = *package_;
        vgui_widget_set_checked(p.directory, !p.embedded);
        vgui_widget_set_checked(p.single, p.embedded);
        vgui_theme palette{};
        vgui_context_get_theme(shell_, &palette);
        vgui_widget_set_border(p.directory, p.embedded ? palette.button_border : palette.accent, 1);
        vgui_widget_set_border(p.single, p.embedded ? palette.accent : palette.button_border, 1);
        vgui_widget_set_text(p.hint, p.embedded ?
            tr("export.single_hint", "Adds .vgui.package.json. Use a directory package for project fonts.") :
            tr("export.directory_hint", "Creates a new folder with design, manifest and assets."));
        ui::delivery_summary summary;
        ui::diagnostic error;
        const bool ready = ui::summarize_delivery(p.doc, p.root, p.embedded, summary, &error);
        vgui_widget_set_enabled(p.confirm, ready);
        if (!ready) {
            vgui_widget_set_text(p.summary, "");
            package_error(std::string(tr("export.failed", "Export failed: ")) + describe(error));
            return;
        }
        const auto bytes = [](std::size_t size) {
            if (size < 1024) return std::to_string(size) + " B";
            char formatted[40];
            std::snprintf(formatted, sizeof(formatted), "%.1f %s", size < (1u << 20) ? size / 1024.0 : size / 1048576.0,
                size < (1u << 20) ? "KiB" : "MiB");
            return std::string(formatted);
        };
        std::string text = std::to_string(summary.resource_count) + " " + tr("export.resource_count", "resources") +
            " · " + tr("export.source_size", "Raw: ") + bytes(summary.resource_bytes);
        if (p.embedded) text += " · " + std::string(tr("export.encoded_size", "Base64 (assets): ")) + bytes(summary.base64_bytes);
        vgui_widget_set_text(p.summary, text.c_str());
        package_error("");
    }

    void show_package_options(vgui_item_id id) {
        const auto* page = find_page(id);
        if (!page || vgui_modal_active(shell_)) return;
        auto name = page->path.empty() ? std::string("untitled") : utf8(page->path.filename());
        constexpr std::string_view suffix = ".vgui.json";
        if (name.ends_with(suffix)) name.resize(name.size() - suffix.size());
        else if (!page->path.empty()) name = utf8(page->path.stem());
        name += "-delivery";
        if (!valid_package_name(name)) name = "ui-delivery";
        // Freeze the initiating page and resource root before the first dialog or chooser.
        auto root = resource_root(*page);
        auto& p = package_.emplace(package_request{id, page_session(*page).current(), root, root});
        p.panel = vgui_widget_create(shell_, VGUI_PANEL, nullptr);
        vgui_widget_set_id(p.panel, "designer.export");
        vgui_widget_set_layout(p.panel, VGUI_LAYOUT_COLUMN);
        vgui_widget_set_align_items(p.panel, VGUI_ALIGN_STRETCH);
        vgui_widget_set_padding(p.panel, 16, 14, 16, 14);
        vgui_widget_set_size(p.panel, 590, 355);
        vgui_widget_set_gap(p.panel, 8);
        vgui_theme palette{};
        vgui_context_get_theme(shell_, &palette);
        vgui_widget_set_border(p.panel, palette.panel_border, 1);
        vgui_widget_set_rounding(p.panel, 6);
        const auto label = [&](const char* text, float size = 0.f) {
            auto* w = vgui_widget_add(p.panel, VGUI_LABEL, text);
            vgui_widget_set_text_align(w, 0, .5f);
            if (size) vgui_widget_set_font_size(w, size);
            return w;
        };
        const auto row = [&] {
            auto* w = vgui_widget_add(p.panel, VGUI_PANEL, nullptr);
            vgui_widget_set_background(w, 0);
            vgui_widget_set_border(w, 0, 0);
            vgui_widget_set_padding(w, 0, 0, 0, 0);
            vgui_widget_set_layout(w, VGUI_LAYOUT_ROW);
            vgui_widget_set_align_items(w, VGUI_ALIGN_CENTER);
            vgui_widget_set_gap(w, 8);
            return w;
        };
        const auto button = [&](vgui_widget* parent, const char* id, const char* text, float width) {
            auto* w = vgui_widget_add(parent, VGUI_BUTTON, text);
            vgui_widget_set_id(w, id);
            vgui_widget_set_size(w, width, 30);
            vgui_widget_set_text_align(w, .5f, .5f);
            vgui_widget_on_event(w, &package_event, this);
            return w;
        };
        const auto edit = [](vgui_widget* parent, const char* id, const char* text) {
            auto* w = vgui_widget_add(parent, VGUI_EDIT, nullptr);
            vgui_widget_set_id(w, id);
            vgui_widget_set_size(w, 0, 30);
            vgui_widget_set_min_size(w, 0, 0);
            vgui_edit_set_text(w, text);
            return w;
        };
        auto* title = label(tr("export.options", "Export Configuration"), 18);
        vgui_widget_set_font_weight(title, 600);
        const std::string from = std::string(tr("export.source_page", "Source page: ")) + page_title(*page);
        auto* source = label(from.c_str());
        vgui_widget_set_tooltip(source, page->path.empty() ? from.c_str() : utf8(page->path).c_str());
        auto* formats = row();
        p.directory = button(formats, "designer.export.directory", tr("export.directory", "Directory package"), 166);
        p.single = button(formats, "designer.export.single", tr("export.single", "Single JSON (Base64)"), 193);
        p.hint = label("");
        label(tr("export.destination", "Destination folder"));
        auto* locations = row();
        const auto location = utf8(p.parent);
        p.location = edit(locations, "designer.export.location", location.c_str());
        vgui_widget_set_a11y_name(p.location, tr("export.destination", "Destination folder"));
        vgui_widget_set_grow(p.location, 1);
        vgui_edit_set_readonly(p.location, 1);
        vgui_widget_set_tooltip(p.location, location.c_str());
        p.browse = button(locations, "designer.export.browse", tr("export.browse", "Browse…"), 100);
        label(tr("export.name", "Package name"));
        p.name = edit(p.panel, "designer.export.name", name.c_str());
        vgui_widget_set_a11y_name(p.name, tr("export.name", "Package name"));
        vgui_widget_on_event(p.name, &package_event, this);
        p.summary = label("");
        vgui_widget_set_id(p.summary, "designer.export.summary");
        p.error = label("");
        vgui_widget_set_id(p.error, "designer.export.error");
        vgui_widget_set_text_color(p.error, theme_ == ui::designer_theme::light ? 0xFFA62B39u : 0xFFF08D96u);
        auto* footer = row();
        vgui_widget_set_justify(footer, VGUI_JUSTIFY_END);
        p.cancel = button(footer, "designer.export.cancel", tr("common.cancel", "Cancel"), 88);
        p.confirm = button(footer, "designer.export.confirm", tr("export.confirm", "Export"), 88);
        vgui_widget_set_border(p.confirm, palette.accent, 1);
        refresh_package_summary();
        vgui_modal_options options;
        vgui_modal_options_default(&options);
        options.close_on_backdrop = 0;
        options.destroy_on_close = 1;
        options.on_close = &package_closed;
        options.user = this;
        if (!vgui_modal_show(shell_, p.panel, &options)) {
            vgui_widget_destroy(p.panel);
            package_.reset();
            workbench_.status(tr("export.prompt_failed", "Cannot open the export configuration."));
            return;
        }
        vgui_modal_set_drag_handle(shell_, title);
        focus_package_name();
    }

    void focus_package_name() {
        vgui_set_focus(shell_, package_->name);
        if (!window_.text_input_active()) {
            window_.start_text_input();
            package_text_input_ = window_.text_input_active();
        }
        vgui_layout(shell_);
        glm::ivec4 area{};
        if (vgui_context_get_text_input_rect(shell_, &area.x, &area.y, &area.z, &area.w))
            window_.set_ime_pos(area);
    }

    void finish_package() {
        auto name = std::exchange(package_name_, std::nullopt);
        if (!package_ || !find_page(package_->page)) return;
        auto& p = *package_;
        if (!valid_package_name(*name)) {
            package_error(tr("export.bad_name", "Enter one name without path separators or trailing dots/spaces."));
            focus_package_name();
            return;
        }
        if (p.embedded && !name->ends_with(ui::json_delivery_suffix)) *name += ui::json_delivery_suffix;
        const auto destination = p.parent / fs::u8path(*name);
        ui::diagnostic error;
        const bool written = p.embedded ? ui::export_json_delivery(p.doc, p.root, destination, &error) :
            ui::export_delivery(p.doc, p.root, destination, &error);
        if (!written) {
            const auto message = std::string(tr("export.failed", "Export failed: ")) + describe(error);
            package_error(message);
            workbench_.status(message);
            return;
        }
        ui::embedded_delivery embedded;
        ui::document directory;
        const bool verified = p.embedded ? ui::read_json_delivery(destination, embedded, &error) :
            ui::read_delivery(destination, directory, &error);
        const auto message = verified ? std::string(tr("export.done", "Exported and verified: ")) + utf8(destination) :
            std::string(tr("export.verify_failed", "Package written, but verification failed: ")) + describe(error);
        vgui_modal_close(shell_, VGUI_MODAL_RESULT_CONFIRM);
        workbench_.status(message);
    }

    void finish_chooser() {
        auto selected = std::move(*picked_);
        picked_.reset();
        chooser_ = false;
        const auto next = std::exchange(chooser_followup_, command::none);
        const auto id = std::exchange(chooser_page_, 0);
        if (next == command::export_package && package_ && package_->page == id) {
            if (!selected.error.empty())
                package_error(std::string(tr("file.chooser_error", "File chooser: ")) + selected.error);
            else if (selected.path.empty())
                package_error(tr("file.chooser_cancelled", "File selection cancelled."));
            else if (selected.kind == VGUI_FILE_DIALOG_FOLDER) {
                package_->parent = fs::u8path(selected.path);
                vgui_edit_set_text(package_->location, selected.path.c_str());
                vgui_widget_set_tooltip(package_->location, selected.path.c_str());
                refresh_package_summary();
            }
            focus_package_name();
            return;
        }
        if (!selected.error.empty()) workbench_.status(std::string(tr("file.chooser_error", "File chooser: ")) + selected.error);
        else if (selected.path.empty()) workbench_.status(tr("file.chooser_cancelled", "File selection cancelled."));
        else if (selected.kind == VGUI_FILE_DIALOG_OPEN) {
            if (next == command::import_font) finish_font_import(id, fs::u8path(selected.path));
            else open(fs::u8path(selected.path));
        }
        else if (selected.kind == VGUI_FILE_DIALOG_SAVE && save(id, fs::u8path(selected.path))) {
            if (next == command::close) remove_page(id);
            if (next == command::quit) { continue_exit(); return; }
            if (next == command::save_all) { continue_save_all(); return; }
        }
        if (next == command::quit) exit_queue_.clear();
        if (next == command::save_all) save_all_queue_.clear();
    }

    os_window& window_;
    vgui_context* shell_;
    vgpu_device_t* gpu_;
    font_familys_t* font_;
    vgpu_ctx_t* renderer_;
    ui::designer_strings strings_;
    fs::path settings_path_;
    message_box_fn message_box_;
    ui::designer_dock_widths saved_layout_;
    ui::designer_grid_settings grid_settings_;
    ui::designer_theme theme_ = ui::default_designer_theme;
    vgui_theme dark_shell_theme_{};
    bool layout_save_pending_ = false;
    std::optional<ui::designer_language> pending_language_;
    std::optional<ui::designer_theme> pending_theme_;
    std::optional<view_action> pending_view_;
    const fs::path new_root_ = fs::canonical(fs::current_path());
    std::vector<page_state> pages_;
    std::deque<vgui_item_id> save_all_queue_;
    std::vector<vgui_item_id> exit_queue_;
    vgui_item_id active_ = 0, pending_tab_ = 0, command_page_ = 0, chooser_page_ = 0;
    std::uint64_t next_untitled_ = 1;
    vgui_item_id cached_tab_ = 0;
    bool projecting_tabs_ = false;
    ui::session session_;
    std::uint64_t next_preview_revision_ = 0;
    std::string window_title_;
    command pending_ = command::none, chooser_followup_ = command::none;
    bool chooser_ = false, quitting_ = false;
    std::optional<file_result> picked_;
    std::optional<package_request> package_;
    std::optional<std::string> package_name_;
    bool package_browse_ = false, package_text_input_ = false;
    ui::designer_preview_fonts preview_fonts_;
    ui::designer_workbench workbench_;
    vgui_widget* dock_;
    ui::designer_options options_;
    vgui_widget* icon_region_ = nullptr;
};

struct smoke_unsaved_prompt {
    enum choice { cancel = 0, save = 1, discard = 2 };
    std::deque<std::pair<std::string, choice>> expected;

    void expect(std::string_view page, choice answer) { expected.emplace_back(page, answer); }
    bool empty() const { return expected.empty(); }

    bool show(const SDL_MessageBoxData* box, int* selected) {
        const std::string_view message(box->message);
        if (expected.empty()) throw std::runtime_error("Unexpected unsaved prompt: " + std::string(message));
        const auto& [page, answer] = expected.front();
        if (!message.starts_with(page + "\n\n"))
            throw std::runtime_error("Expected unsaved prompt for " + page + ", got: " + std::string(message));
        if (!box->window || box->numbuttons != 3)
            throw std::runtime_error("Unsaved prompt needs a parent window and three choices");
        const auto* buttons = box->buttons;
        if (buttons[0].buttonID != cancel || buttons[0].flags != SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT ||
            buttons[1].buttonID != discard || buttons[1].flags != 0 ||
            buttons[2].buttonID != save || buttons[2].flags != SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT)
            throw std::runtime_error("Unexpected choices or keyboard defaults in unsaved prompt");
        *selected = answer;
        expected.pop_front();
        return true;
    }
};

// Exercise the async file-result flow while keeping the smoke test unattended.
// The platform chooser itself remains the normal provider in interactive use.
struct smoke_file_picker {
    vgui_file_dialog_provider_fn previous;
    void* previous_user = nullptr;
    int open_requests = 0, save_requests = 0, folder_requests = 0, font_requests = 0;
    vgui_file_dialog_fn delayed_open = nullptr, delayed_save = nullptr, delayed_folder = nullptr;
    void* delayed_open_user = nullptr;
    void* delayed_save_user = nullptr;
    void* delayed_folder_user = nullptr;
    bool cancel_folder = false, defer_saves = false, defer_folders = false;
    std::string save_suggestion;
    fs::path sandbox, package_parent, save_target;

    smoke_file_picker() {
        const auto temp = fs::temp_directory_path();
        std::random_device random;
        for (int attempt = 0; attempt < 32; ++attempt) {
            auto path = temp / ("vgui-package-smoke-" + std::to_string(random()) + "-" + std::to_string(random()));
            if (fs::create_directory(path)) {
                sandbox = std::move(path);
                break;
            }
        }
        if (sandbox.empty()) throw std::runtime_error("Cannot reserve a package smoke test directory");
        package_parent = sandbox / "chosen";
        if (std::error_code ec; !fs::create_directory(package_parent, ec)) {
            fs::remove(sandbox);
            throw std::runtime_error("Cannot create the package smoke test parent: " + ec.message());
        }
        previous = vgui_file_dialog_get_provider(&previous_user);
        vgui_file_dialog_set_provider(show, this);
    }
    ~smoke_file_picker() {
        vgui_file_dialog_set_provider(previous, previous_user);
        std::error_code ignored;
        fs::remove_all(sandbox, ignored);
    }

    bool complete_cancelled_open() {
        auto callback = std::exchange(delayed_open, nullptr);
        return callback && vgui_file_dialog_post_result(VGUI_FILE_DIALOG_OPEN, callback, delayed_open_user,
            nullptr, 0, nullptr);
    }

    bool complete_font_import(const fs::path& path = {}) {
        return post_path(VGUI_FILE_DIALOG_OPEN, std::exchange(delayed_open, nullptr), delayed_open_user, path);
    }

    bool complete_save(const fs::path& path = {}) {
        return post_path(VGUI_FILE_DIALOG_SAVE, std::exchange(delayed_save, nullptr), delayed_save_user, path);
    }

    bool complete_folder(const fs::path& path) {
        return post_path(VGUI_FILE_DIALOG_FOLDER, std::exchange(delayed_folder, nullptr), delayed_folder_user, path);
    }

    static bool post_path(vgui_file_dialog_kind kind, vgui_file_dialog_fn callback, void* user, const fs::path& path) {
        if (!callback) return false;
        const auto name = utf8(path);
        const char* paths[] = {name.c_str()};
        return vgui_file_dialog_post_result(kind, callback, user, path.empty() ? nullptr : paths, path.empty() ? 0 : 1, nullptr);
    }

    static int show(void* user, vgui_file_dialog_kind kind, const vgui_file_dialog_desc* desc,
                    vgui_file_dialog_fn callback, void* callback_user) {
        auto& self = *static_cast<smoke_file_picker*>(user);
        if (kind == VGUI_FILE_DIALOG_FOLDER) {
            ++self.folder_requests;
            if (self.defer_folders) {
                if (self.delayed_folder) return 0;
                self.delayed_folder = callback;
                self.delayed_folder_user = callback_user;
                return 1;
            }
            return post_path(kind, callback, callback_user, self.cancel_folder ? fs::path{} : self.package_parent);
        }
        if (kind == VGUI_FILE_DIALOG_SAVE) {
            ++self.save_requests;
            self.save_suggestion = desc && desc->default_name ? desc->default_name : "";
            if (self.defer_saves) {
                if (self.delayed_save) return 0;
                self.delayed_save = callback;
                self.delayed_save_user = callback_user;
                return 1;
            }
            if (self.save_target.empty()) return 0;
            const auto path = utf8(std::exchange(self.save_target, fs::path{}));
            const char* paths[] = {path.c_str()};
            return vgui_file_dialog_post_result(kind, callback, callback_user, paths, 1, nullptr);
        }
        if (kind != VGUI_FILE_DIALOG_OPEN) return 0;
        const bool font = desc && desc->filters && desc->filter_count &&
            std::string_view(desc->filters[0].pattern) == "*.ttf;*.otf";
        if (font) ++self.font_requests;
        if (font || ++self.open_requests == 1) {
            if (self.delayed_open) return 0;
            self.delayed_open = callback;
            self.delayed_open_user = callback_user;
            return 1;
        }
        // Return an existing file in a non-design format.
        return post_path(kind, callback, callback_user, self.sandbox / ui::smoke_badge_file);
    }
};

// Queue ordinary SDL events and advance the real host loop. Never write the
// preview widget, invoke its input handler directly, or mutate the document.
void exercise_runtime_input(os_window& win, vgui_context* shell, designer& editor,
                            const std::function<void()>& frame, const int& submitted_windows) {
    const auto require = [](bool passed, const char* message) {
        if (!passed) throw std::runtime_error(std::string("Native runtime input: ") + message);
    };
    const auto push = [&](SDL_Event e) { require(SDL_PushEvent(&e), "failed to queue SDL event"); };
    const auto mouse = [&](bool down, glm::vec2 px) {
        SDL_Event e{};
        e.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
        e.button.windowID = win.id;
        e.button.button = SDL_BUTTON_LEFT;
        e.button.down = down;
        e.button.clicks = 1;
        const auto points = px / win.pixel_ratio();
        e.button.x = points.x;
        e.button.y = points.y;
        push(e);
    };
    const auto click = [&](glm::vec2 px) { mouse(true, px); mouse(false, px); frame(); };
    const auto key = [&](SDL_Keycode code) {
        for (bool down : {true, false}) {
            SDL_Event e{};
            e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
            e.key.windowID = win.id;
            e.key.key = code;
            e.key.down = down;
            push(e);
        }
        frame();
    };
    const auto text = [&](const char* utf8) {
        SDL_Event e{};
        e.type = SDL_EVENT_TEXT_INPUT;
        e.text.windowID = win.id;
        e.text.text = utf8;
        push(e);
        frame();
    };
    const auto center = [&](std::string_view id) {
        const auto board = editor.preview().artboard_rect_px();
        const auto* layout = editor.preview().snapshot();
        require(board && layout, "missing presented artboard/layout");
        const auto it = std::find_if(layout->nodes.begin(), layout->nodes.end(),
            [id](const auto& n) { return n.id == id && n.visible; });
        require(it != layout->nodes.end(), "test node is missing or clipped");
        const auto& r = it->visible_px;
        return glm::vec2{board->x + (r.x + r.width / 2) * board->width / layout->pixel_width,
            board->y + (r.y + r.height / 2) * board->height / layout->pixel_height};
    };
    const auto toggle_mode = [&] {
        auto* mode = find_widget(vgui_root(shell), "designer.preview.mode");
        require(mode != nullptr, "missing Design/Run button");
        float x, y, w, h;
        vgui_widget_get_rect(mode, &x, &y, &w, &h);
        click({x + w / 2, y + h / 2});
    };
    auto* status = find_widget(vgui_root(shell), "designer.status");
    require(status != nullptr, "missing workbench status");
    const auto& history = editor.history();
    const auto doc = history.current();
    const auto revision = history.revision();
    const std::string before_action = vgui_widget_get_text(status);
    auto& preview = editor.preview();
    const auto original_name = preview.edit_text("name"), original_email = preview.edit_text("email");
    require(original_name == "" && original_email == "", "expected the empty form fixture");

    require(!preview.running(), "expected the initial Design mode");
    click(center("submit"));
    require(editor.selection() == "submit" && before_action == vgui_widget_get_text(status),
        "Design click must select the button without activating it");
    toggle_mode();
    require(preview.running(), "toolbar did not enter Run mode");
    click(center("name"));
    require(SDL_TextInputActive(win.window), "edit focus did not activate SDL text input");
    require(preview.snapshot() != nullptr, "missing focused runtime preview");
    const auto focused_serial = preview.snapshot()->serial;
    frame();
    require(preview.snapshot() && preview.snapshot()->serial != focused_serial && submitted_windows == 1,
        "focused runtime animation did not repaint without input");
    text("Mira\xc3\xa9");
    require(preview.edit_text("name") == "Mira\xc3\xa9", "UTF-8 text did not reach the focused edit");
    key(SDLK_BACKSPACE);
    require(preview.edit_text("name") == "Mira", "Backspace did not remove the full character");
    key(SDLK_TAB);
    text("mira@example.test");
    require(preview.edit_text("email") == "mira@example.test" && preview.edit_text("name") == "Mira",
        "Tab did not move focus to the next runtime edit");

    const auto submit = center("submit");
    mouse(true, submit);
    frame();
    require(before_action == vgui_widget_get_text(status), "button activated before mouse-up");
    SDL_Event lost{};
    lost.type = SDL_EVENT_WINDOW_FOCUS_LOST;
    lost.window.windowID = win.id;
    push(lost);
    mouse(false, submit);
    frame();
    require(before_action == vgui_widget_get_text(status), "lost focus did not cancel the pending click");
    click(submit);
    require(std::string_view(vgui_widget_get_text(status)) ==
        std::string(editor.strings().text("preview.status.action_prefix", "Action: ")) + "contact.submit [submit]",
        "button mouse-up did not publish the action");
    click(center("email"));
    require(SDL_TextInputActive(win.window), "could not restore edit focus before leaving Run mode");
    toggle_mode();
    require(!preview.running() && !SDL_TextInputActive(win.window), "return to Design did not release text input");
    require(preview.edit_text("name") == original_name && preview.edit_text("email") == original_email,
        "return to Design did not restore document text");
    require(history.current() == doc && history.revision() == revision &&
        !history.dirty() && !history.can_undo() && !history.can_redo(),
        "runtime input changed the design document or its history");

    // The previous Design selection was Submit. Type into the actual inspector
    // and check the real canvas layout before Enter or even a focus change.
    const auto button_width = [&] {
        if (const auto* layout = preview.snapshot()) {
            for (const auto& item : layout->nodes) if (item.id == "submit") return item.rect_px.width;
        }
        throw std::runtime_error("Live inspector: missing preview button");
    };
    const float old_width = button_width();
    auto* width_edit = find_widget(vgui_root(shell), "property:width", VGUI_EDIT);
    require(width_edit != nullptr, "missing inspector width field");
    float x, y, w, h;
    vgui_widget_get_rect(width_edit, &x, &y, &w, &h);
    click({x + w / 2, y + h / 2});
    require(vgui_get_focus(shell) == width_edit, "inspector width field did not receive focus");
    vgui_edit_select_all(width_edit);
    text("104");
    require(button_width() == 104.f && history.dirty(), "inspector change did not reach the canvas without Enter");
    text("x");
    require(button_width() == 104.f, "invalid inspector input replaced the previous valid preview");
    key(SDLK_ESCAPE);
    require(button_width() == old_width && history.current() == doc && !history.can_undo(),
        "cancelling live inspector input did not restore the canvas and history");

    auto* size_edit = find_widget(vgui_root(shell), "property:font_size", VGUI_EDIT);
    require(size_edit != nullptr, "missing inspector font size field");
    vgui_widget_get_rect(size_edit, &x, &y, &w, &h);
    click({x + w / 2, y + h / 2});
    require(vgui_get_focus(shell) == size_edit && preview.snapshot(), "could not focus font size in the inspector");
    const auto before_font_revision = preview.snapshot()->revision;
    vgui_edit_select_all(size_edit);
    text("24");
    const auto& props = history.current().nodes.at("submit").props;
    const auto size = props.find("font_size");
    require(size != props.end() && size->second == ui::value{24.0} && preview.snapshot() &&
        preview.snapshot()->revision != before_font_revision && submitted_windows == 1,
        "font size did not update the document and repaint before Enter");
    key(SDLK_ESCAPE);
    require(history.current() == doc && !history.dirty() && !history.can_undo(),
        "cancelling a live font change did not restore the original document");

    auto* swatch = find_widget(vgui_root(shell), "property:text_color", VGUI_PANEL);
    require(swatch != nullptr, "missing inspector color swatch");
    const auto click_swatch = [&] {
        vgui_widget_get_rect(swatch, &x, &y, &w, &h);
        click({x + w / 2, y + h / 2});
        require(vgui_popup_active(shell), "color swatch did not open a popup");
    };
    click_swatch();
    auto* saturation = find_widget(vgui_popup_content(shell), "property-grid.color-picker.sv");
    require(saturation && preview.snapshot(), "missing color picker or preview");
    const auto before_color = preview.snapshot()->revision;
    vgui_widget_get_rect(saturation, &x, &y, &w, &h);
    const glm::vec2 pick{x + w * .6f, y + h * .4f};
    require(vgui_hit_test(shell, pick.x, pick.y) == saturation, "picker surface is not hittable");
    SDL_Event hover{};
    hover.type = SDL_EVENT_MOUSE_MOTION;
    hover.motion.windowID = win.id;
    hover.motion.x = pick.x / win.pixel_ratio();
    hover.motion.y = pick.y / win.pixel_ratio();
    push(hover);
    click(pick);
    require(history.current().nodes.at("submit").props.contains("text_color"), "color gesture did not reach the document");
    require(preview.snapshot() && preview.snapshot()->revision != before_color, "color gesture did not repaint the canvas");
    require(history.committed() == doc && vgui_popup_active(shell), "color gesture committed or closed the picker prematurely");
    key(SDLK_ESCAPE);
    require(!vgui_popup_active(shell) && history.current() == doc && !history.dirty() && !history.can_undo(),
        "Escape did not roll back the color preview");
    click_swatch();
    click(center("title"));
    require(!vgui_popup_active(shell) && editor.selection() == "submit" && history.current() == doc,
        "the click dismissing the picker leaked through to the artboard");
}

void exercise_project_fonts(os_window& win, vgui_context* shell, designer& editor, smoke_file_picker& picker,
                            const std::function<void()>& frame, const fs::path& other_page) {
    const auto require = [](bool passed, const char* message) {
        if (!passed) throw std::runtime_error(std::string("Native project font: ") + message);
    };
    const ui::native_menu_input menus(win, shell, frame);
    const auto root = picker.sandbox / "font-project";
    require(fs::create_directory(root), "cannot prepare project directory");
    const auto file = root / "fonts.vgui.json";
    fs::copy_file(VGUI_SOURCE_DIR "/sample/res/form.vgui.json", file);
    const auto external = picker.sandbox / fs::u8path(u8"项目字体.ttf");
    fs::copy_file(VGUI_SOURCE_DIR "/sample/res/fonts/DejaVuSans.ttf", external);
    const auto invalid = picker.sandbox / "invalid.ttf";
    { std::ofstream stream(invalid, std::ios::binary); stream << "not a font"; }
    require(editor.open(file), "cannot open saved project");
    frame();
    const auto& history = editor.history();
    const auto source = history.current();
    const auto start_import = [&](int count) {
        menus.activate(menus.open(2), 1); // Resources > Import Project Font
        require(editor.chooser_open() && picker.font_requests == count, "expected the font picker and its filter");
    };
    const auto finish = [&](const fs::path& path = {}) {
        require(picker.complete_font_import(path), "failed to post font picker result");
        for (int i = 0; i < 4 && editor.chooser_open(); ++i) frame();
        require(!editor.chooser_open(), "picker remained open");
    };
    start_import(1);
    finish();
    start_import(2);
    finish(invalid);
    require(history.current() == source && !history.dirty() && !history.can_undo() && !fs::exists(root / "fonts"),
        "cancel or invalid input changed the project");

    start_import(3);
    require(editor.open(other_page), "could not change the active page during the pending chooser");
    frame();
    const auto other = history.current();
    finish(external);
    require(history.current() == other && !history.dirty(), "late import changed the wrong page");
    require(editor.open(file), "cannot return to the origin page");
    frame();
    const auto imported = history.current();
    require(imported.resources.size() == 1 && imported.nodes == source.nodes && history.dirty(), "origin did not register one font");
    const auto& [key, relative] = *imported.resources.begin();
    const auto bytes = [&](const fs::path& path) {
        std::ifstream input(path, std::ios::binary);
        require(bool(input), "cannot read font for byte comparison");
        return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    };
    const auto font_bytes = bytes(external);
    require(relative.starts_with("fonts/") && bytes(root / fs::u8path(relative)) == font_bytes,
        "project did not receive an exact font copy");

    const auto click = [&](float x, float y) {
        for (bool down : {true, false}) {
            SDL_Event e{};
            e.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
            e.button.windowID = win.id;
            e.button.button = SDL_BUTTON_LEFT;
            e.button.down = down;
            e.button.clicks = 1;
            e.button.x = x / win.pixel_ratio();
            e.button.y = y / win.pixel_ratio();
            require(SDL_PushEvent(&e), "cannot queue inspector input");
        }
        frame();
    };
    const auto board = editor.preview().artboard_rect_px();
    const auto* layout = editor.preview().snapshot();
    require(board && layout, "missing project preview");
    const auto title = std::find_if(layout->nodes.begin(), layout->nodes.end(),
        [](const auto& node) { return node.id == "title"; });
    require(title != layout->nodes.end() && title->visible, "missing selectable title");
    const auto r = title->visible_px;
    click(board->x + (r.x + r.width / 2) * board->width / layout->pixel_width,
        board->y + (r.y + r.height / 2) * board->height / layout->pixel_height);
    require(editor.selection() == "title", "could not select the title from the canvas");
    auto* field = find_widget(vgui_root(shell), "property:font_family", VGUI_BUTTON);
    require(field != nullptr, "missing font family inspector field");
    float x, y, w, h;
    vgui_widget_get_rect(field, &x, &y, &w, &h);
    require(w > 0 && h > 0, "font family is not visible");
    click(x + w / 2, y + h / 2);
    auto* choices = vgui_context_get_open_menu(shell);
    require(choices && vgui_menu_get_count(choices) == 4 &&
        std::string_view(vgui_menu_get_item_text(choices, vgui_menu_get_item_at(choices, 3))) == relative,
        "imported font missing from the inspector");
    menus.activate(choices, 3);
    require(history.current().nodes.at("title").props.at("font_family") == ui::value{key} && editor.preview_ready(false),
        "choosing the project font did not update and repaint the document");
    auto* status = find_widget(editor.preview().widget(), "designer.preview.status");
    require(status && std::string_view(vgui_widget_get_text(status)).find("/props/font_family") == std::string_view::npos,
        "preview could not load the project font");
    menus.activate(menus.open(0), 2); // File > Save
    ui::document saved;
    require(ui::read_document(file, saved) && saved == history.current() && !history.dirty(), "saved page lost its selected font");
    ui::diagnostic error;
    const auto package = picker.package_parent / "project-font-delivery";
    ui::document delivered;
    require(ui::export_delivery(saved, root, package, &error) && ui::read_delivery(package, delivered, &error),
        "could not export or verify the project font");
    require(delivered.nodes == saved.nodes && delivered.resources.at(key).starts_with("assets/") &&
        bytes(package / fs::u8path(delivered.resources.at(key))) == font_bytes,
        "delivery did not preserve the selected font and embed its asset");
}

void exercise_image_button(os_window& win, vgui_context* shell, designer& editor, const std::function<void()>& frame) {
    const auto require = [](bool passed, const char* message) {
        if (!passed) throw std::runtime_error(std::string("Native image button: ") + message);
    };
    require(editor.open(VGUI_SOURCE_DIR "/sample/res/image-button.vgui.json"), "cannot open the example");
    frame();
    auto& preview = editor.preview();
    const auto click = [&](float x, float y) {
        for (bool down : {true, false}) {
            SDL_Event e{};
            e.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
            e.button.windowID = win.id;
            e.button.button = SDL_BUTTON_LEFT;
            e.button.down = down;
            e.button.clicks = 1;
            e.button.x = x / win.pixel_ratio();
            e.button.y = y / win.pixel_ratio();
            require(SDL_PushEvent(&e), "cannot enqueue pointer event");
        }
        frame();
    };
    const auto click_button = [&](std::string_view id, bool enabled) {
        const auto board = preview.artboard_rect_px();
        const auto* layout = preview.snapshot();
        require(board && layout, "missing rendered sample");
        const auto node = std::find_if(layout->nodes.begin(), layout->nodes.end(),
            [id](const auto& entry) { return entry.id == id; });
        require(node != layout->nodes.end() && node->visible && node->enabled == enabled && !node->image_placeholder,
            "sample button is missing, misconfigured or has no atlas");
        const auto& r = node->visible_px;
        click(board->x + (r.x + r.width / 2) * board->width / layout->pixel_width,
            board->y + (r.y + r.height / 2) * board->height / layout->pixel_height);
    };
    auto* mode = find_widget(vgui_root(shell), "designer.preview.mode");
    auto* status = find_widget(vgui_root(shell), "designer.status");
    require(mode && status && !preview.running() && editor.preview_ready(false), "sample did not open in Design mode");
    const auto& history = editor.history();
    const auto doc = history.current();
    const auto revision = history.revision();
    const std::string before = vgui_widget_get_text(status);
    click_button("play", true);
    require(editor.selection() == "play" && before == vgui_widget_get_text(status), "Design click must select, not activate");
    float x, y, width, height;
    vgui_widget_get_rect(mode, &x, &y, &width, &height);
    click(x + width / 2, y + height / 2);
    require(preview.running(), "toolbar did not enter Run mode");
    const std::string before_run_click = vgui_widget_get_text(status);
    click_button("disabled", false);
    require(before_run_click == vgui_widget_get_text(status), "disabled sample button unexpectedly activated");
    click_button("play", true);
    require(std::string_view(vgui_widget_get_text(status)).ends_with("demo.play [play]"), "Run click did not dispatch the sample action");
    require(history.current() == doc && history.revision() == revision && !history.dirty(), "preview interaction modified the design");
}

void exercise_language_and_layout(os_window& win, vgui_context* shell, designer& editor,
                                  const fs::path& settings, const std::function<void()>& frame) {
    const auto require = [](bool passed, const char* message) {
        if (!passed) throw std::runtime_error(std::string("Native language/layout: ") + message);
    };
    const auto widget = [&](const char* id) {
        auto* w = find_widget(vgui_root(shell), id);
        require(w != nullptr, id);
        return w;
    };
    const auto push = [&](SDL_Event e) { require(SDL_PushEvent(&e), "cannot enqueue SDL event"); };
    const auto click = [&](float x, float y) {
        for (bool down : {true, false}) {
            SDL_Event e{};
            e.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
            e.button.windowID = win.id;
            e.button.button = SDL_BUTTON_LEFT;
            e.button.down = down;
            e.button.clicks = 1;
            e.button.x = x / win.pixel_ratio();
            e.button.y = y / win.pixel_ratio();
            push(e);
        }
        frame();
    };
    const ui::native_menu_input menus(win, shell, frame);
    const auto label = [&](const char* id) { return std::string_view(vgui_widget_get_text(widget(id))); };

    auto& preview = editor.preview();
    const auto& history = editor.history();
    require(label("designer.file.save") == "保存" &&
        label("designer.preview.mode") == "设计", "expected the default Chinese UI");
    require(history.dirty() && history.can_undo(), "expected unsaved edits from the delivery test");
    auto* view = menus.open(3);
    const auto design_item = vgui_menu_get_item_at(view, 0), run_item = vgui_menu_get_item_at(view, 1);
    require(vgui_menu_get_item_checked(view, design_item) && !vgui_menu_get_item_checked(view, run_item),
        "View menu did not reflect Design mode");
    menus.activate(view, 1);
    require(preview.running(), "View menu did not enter Run mode");
    view = menus.open(3);
    require(!vgui_menu_get_item_checked(view, design_item) && vgui_menu_get_item_checked(view, run_item),
        "View menu did not reflect Run mode");
    menus.key(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE);
    auto board = preview.artboard_rect_px();
    const auto* layout = preview.snapshot();
    require(board && layout, "missing runtime artboard");
    const auto field = std::find_if(layout->nodes.begin(), layout->nodes.end(), [](const auto& n) { return n.id == "name"; });
    require(field != layout->nodes.end() && field->visible, "missing runtime input field");
    const auto& r = field->visible_px;
    click(board->x + (r.x + r.width / 2) * board->width / layout->pixel_width,
          board->y + (r.y + r.height / 2) * board->height / layout->pixel_height);
    SDL_Event text{};
    text.type = SDL_EVENT_TEXT_INPUT;
    text.text.windowID = win.id;
    text.text.text = "Keep this input";
    push(text);
    frame();
    require(preview.edit_text("name") == "Keep this input", "runtime edit did not receive text");
    menus.key(SDLK_F10, SDL_SCANCODE_F10);
    require(vgui_context_get_open_menu(shell), "F10 did not open the main menu during runtime text input");
    menus.key(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE);
    require(!vgui_context_get_open_menu(shell) && preview.edit_text("name") == "Keep this input",
        "menu dismissal changed runtime input");

    const auto revision = history.revision();
    const auto baseline = history.current(), committed = history.committed();
    const std::string selection(editor.selection());
    const bool dirty = history.dirty(), undo = history.can_undo(), redo = history.can_redo();
    const auto* canvas = widget("designer.preview.canvas");
    const auto* outline = widget("designer.outliner");
    const auto switch_to = [&](bool english) {
        auto* tools = menus.open(4);
        require(std::string_view(vgui_menu_get_item_text(tools, vgui_menu_get_item_at(tools, 0))) ==
            (english ? "语言" : "Language"), "Tools menu was not translated");
        auto* menu = menus.activate(tools, 0);
        require(menu && vgui_menu_get_count(menu) == 2, "language menu did not open");
        const auto zh = vgui_menu_get_item_at(menu, 0), en = vgui_menu_get_item_at(menu, 1);
        require(std::string_view(vgui_menu_get_item_text(menu, zh)) == "简体中文" &&
            std::string_view(vgui_menu_get_item_text(menu, en)) == "English" &&
            vgui_menu_get_item_checked(menu, english ? zh : en) &&
            !vgui_menu_get_item_checked(menu, english ? en : zh), "wrong active locale in the menu");
        menus.activate(menu, english ? 1 : 0);
        require(!vgui_context_get_open_menu(shell) && ui::load_designer_language(settings) ==
            (english ? ui::designer_language::english : ui::designer_language::simplified_chinese), "wrong saved preference");
        require(label("designer.file.save") == (english ? "Save" : "保存") &&
            label("designer.preview.phone") == (english ? "Phone" : "手机") &&
            label("designer.preview.mode") == (english ? "Run" : "运行") &&
            label("designer.status") == (english ? "Language preference applied and saved." : "语言设置已保存并生效。") &&
            std::string_view(SDL_GetWindowTitle(win.window)) == (english ? "form.vgui.json * — VGUI UI Designer" :
                "form.vgui.json * — VGUI 界面设计器"), "language did not apply to the same window immediately");
        const auto now = preview.artboard_rect_px();
        require(widget("designer.preview.canvas") == canvas && widget("designer.outliner") == outline &&
            now && now->x == board->x && now->y == board->y && now->width == board->width && now->height == board->height &&
            preview.running() && preview.edit_text("name") == "Keep this input" && editor.selection() == selection,
            "switching language reset the preview or selection");
        require(history.revision() == revision && history.current() == baseline && history.committed() == committed &&
            history.dirty() == dirty && history.can_undo() == undo && history.can_redo() == redo,
            "switching language changed the design or its undo history");
    };
    const auto check_configuration = [&](bool english) {
        const auto read_settings = [&] {
            std::ifstream in(settings, std::ios::binary);
            require(in.is_open(), "configuration fixture is missing");
            return std::string(std::istreambuf_iterator<char>(in), {});
        };
        const auto saved = read_settings();
        const std::string_view title = english ? "Configuration Information" : "配置信息";
        auto* tools = menus.open(4);
        require(vgui_menu_get_count(tools) == 3 &&
            std::string_view(vgui_menu_get_item_text(tools, vgui_menu_get_item_at(tools, 1))) == (english ? "Options" : "选项"),
            "configuration should only be accessible through Options");
        menus.activate(tools, 1);
        auto* panel = vgui_modal_panel(shell);
        require(panel && std::string_view(vgui_widget_get_id(panel)) == "designer.options", "options dialog did not open");
        const auto child = [&](std::string_view id) {
            auto* item = find_widget(panel, id);
            require(item != nullptr, "missing configuration widget in Options");
            return item;
        };
        const auto press = [&](vgui_widget* w) {
            float x, y, width, height;
            vgui_widget_get_rect(w, &x, &y, &width, &height);
            require(width > 0 && height > 0, "configuration widget is not laid out");
            click(x + width / 2, y + height / 2);
        };
        auto* entry = child("designer.options.nav.configuration");
        auto* config = child("designer.options.configuration");
        auto* grid = child("designer.options.grid");
        require(std::string_view(vgui_widget_get_text(entry)) == title && !vgui_widget_get_visible(config) &&
            vgui_widget_get_visible(grid), "grid should be the default Options page");
        press(entry);
        require(vgui_modal_panel(shell) == panel && vgui_widget_get_visible(config) && !vgui_widget_get_visible(grid) &&
            !vgui_widget_get_enabled(child("designer.options.reset")) &&
            std::string_view(vgui_widget_get_text(child("designer.options.configuration.title"))) == title,
            "clicking Configuration Information did not switch the current dialog to a read-only page");
        const auto text = [&](std::string_view id) { return std::string_view(vgui_widget_get_text(child(id))); };
        require(text("designer.options.configuration.language") == (english ? "Interface language: English" : "界面语言：简体中文") &&
            text("designer.options.configuration.theme") == (english ? "Interface theme: Light" : "界面主题：浅色") &&
            text("designer.options.configuration.left") == (english ? "Left sidebar: 280 logical px · Collapsed" : "左侧栏：280 逻辑像素 · 已展开") &&
            text("designer.options.configuration.right") == (english ? "Right sidebar: 375 logical px · Expanded" : "右侧栏：375 逻辑像素 · 已展开"),
            "configuration page did not show the current language, theme and preferred dock sizes/states");
        auto* path = child("designer.options.configuration.path");
        vgui_a11y_info path_info{};
        vgui_widget_get_a11y_info(path, &path_info);
        require(vgui_widget_get_kind(path) == VGUI_EDIT && (path_info.states & VGUI_A11Y_READONLY) &&
            text("designer.options.configuration.path_label") == (english ? "Settings file (JSON)" : "配置文件（JSON）") &&
            std::string_view(vgui_edit_get_text(path)) == utf8(settings), "configuration path was missing or not read-only");
        auto* ok = child("designer.options.ok");
        require(std::string_view(vgui_widget_get_text(ok)) == (english ? "OK" : "确定"),
            "configuration page is missing its localized dialog button");
        if (english) press(ok);
        else menus.key(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE);
        require(!vgui_modal_active(shell) && read_settings() == saved && preview.running() &&
            preview.edit_text("name") == "Keep this input" && editor.selection() == selection &&
            history.revision() == revision && history.current() == baseline && history.committed() == committed &&
            history.dirty() == dirty && history.can_undo() == undo && history.can_redo() == redo,
            "opening or closing configuration changed settings, runtime input, selection or history");
    };
    check_configuration(false);
    switch_to(true);
    auto* dock = widget("designer.dock");
    const auto dock_width = [&](vgui_dock_zone zone) {
        float width = 0;
        require(vgui_dock_get_zone_rect(dock, zone, nullptr, nullptr, &width, nullptr),
            "missing dock region");
        return width / vgui_context_get_ui_scale(shell);
    };
    constexpr ui::designer_dock_widths defaults{};
    require(dock_width(VGUI_DOCK_LEFT) > defaults.left && dock_width(VGUI_DOCK_RIGHT) > defaults.right,
        "custom widths were not restored before layout reset");
    const auto custom = ui::load_designer_dock_widths(settings);
    const auto saved_sides = [&](bool left, bool right) {
        const auto saved = ui::load_designer_dock_widths(settings);
        return std::abs(saved.left - custom.left) < .01f && std::abs(saved.right - custom.right) < .01f &&
            saved.left_collapsed == left && saved.right_collapsed == right;
    };
    const auto original_center = dock_width(VGUI_DOCK_CENTER);
    const auto press_side = [&](const char* id) {
        auto* button = widget(id);
        ui::pixel_rect bounds{};
        vgui_widget_get_rect(button, &bounds.x, &bounds.y, &bounds.width, &bounds.height);
        const float x = bounds.x + bounds.width * .5f, y = bounds.y + bounds.height * .5f;
        require(bounds.width > 0 && bounds.height > 0 && vgui_hit_test(shell, x, y) == button,
            "side panel toggle is not clickable");
        click(x, y);
    };
    const auto before = preview.capture_view();
    press_side("designer.dock.collapse_left");
    require(dock_width(VGUI_DOCK_LEFT) < 60 && std::abs(dock_width(VGUI_DOCK_RIGHT) - custom.right) < .01f &&
        dock_width(VGUI_DOCK_CENTER) > original_center &&
        saved_sides(true, false),
        "folding Outline did not widen the board, changed Inspector, or preferences were not saved");
    check_configuration(true);
    press_side("designer.dock.collapse_right");
    require(dock_width(VGUI_DOCK_RIGHT) < 60 &&
        saved_sides(true, true),
        "Inspector did not fold independently");
    press_side("designer.dock.collapse_left");
    require(std::abs(dock_width(VGUI_DOCK_LEFT) - custom.left) < .01f && dock_width(VGUI_DOCK_RIGHT) < 60 &&
        saved_sides(false, true),
        "reopening Outline did not restore its preferred width or changed Inspector");
    auto* view_menu = menus.open(3);
    const int reset_index = vgui_menu_get_count(view_menu) - 1;
    require(std::string_view(vgui_menu_get_item_text(view_menu, vgui_menu_get_item_at(view_menu, reset_index))) ==
        "Restore Default Layout", "layout command missing from the English View menu");
    menus.activate(view_menu, reset_index);
    require(std::abs(dock_width(VGUI_DOCK_LEFT) - defaults.left) < .01f &&
        std::abs(dock_width(VGUI_DOCK_RIGHT) - defaults.right) < .01f,
        "reset did not update both side widths immediately");
    require(ui::load_designer_dock_widths(settings) == defaults &&
        ui::load_designer_language(settings) == ui::designer_language::english &&
        label("designer.status") == "Default layout restored.", "reset did not save widths without changing the language");
    const auto after = preview.capture_view();
    require(after.screen == before.screen && after.fit == before.fit && after.zoom == before.zoom &&
        after.running && preview.edit_text("name") == "Keep this input" && editor.selection() == selection &&
        history.current() == baseline && history.revision() == revision && history.dirty() == dirty &&
        history.can_undo() == undo && history.can_redo() == redo, "layout reset changed the preview, selection or undo history");

    // Continue the language test with the new canvas bounds.
    board = preview.artboard_rect_px();
    require(board.has_value(), "layout reset removed the artboard");
    switch_to(false);
    view_menu = menus.open(3);
    require(std::string_view(vgui_menu_get_item_text(view_menu, vgui_menu_get_item_at(view_menu, reset_index))) ==
        "恢复默认布局", "layout command missing from the Chinese View menu");
    menus.key(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE);
}

void exercise_options(os_window& win, vgui_context* shell, designer& editor, const fs::path& settings,
                      const std::function<void()>& frame) {
    const auto require = [](bool passed, const char* message) {
        if (!passed) throw std::runtime_error(std::string("Native options: ") + message);
    };
    const auto push = [&](SDL_Event e) { require(SDL_PushEvent(&e), "cannot enqueue SDL event"); };
    const auto button = [&](bool down, float x, float y) {
        SDL_Event e{};
        e.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
        e.button.windowID = win.id;
        e.button.button = SDL_BUTTON_LEFT;
        e.button.down = down;
        e.button.clicks = 1;
        e.button.x = x / win.pixel_ratio();
        e.button.y = y / win.pixel_ratio();
        push(e);
    };
    const auto widget = [&](std::string_view id, vgui_widget_kind kind) {
        auto* w = find_widget(vgui_modal_panel(shell), id, kind);
        require(w != nullptr, "could not find the requested options control");
        return w;
    };
    const auto click = [&](vgui_widget* w) {
        float x, y, width, height;
        vgui_widget_get_rect(w, &x, &y, &width, &height);
        require(width > 0 && height > 0, "options control is not laid out");
        button(true, x + width / 2, y + height / 2);
        button(false, x + width / 2, y + height / 2);
        frame();
    };
    const ui::native_menu_input menus(win, shell, frame);
    const auto open = [&] {
        auto* tools = menus.open(4);
        const int count = vgui_menu_get_count(tools);
        int index = 0;
        for (; index < count; ++index) {
            if (std::string_view(vgui_menu_get_item_text(tools, vgui_menu_get_item_at(tools, index))) == "选项") break;
        }
        require(index < count, "localized entry missing from Tools");
        menus.activate(tools, index);
        require(vgui_modal_active(shell) && std::string_view(vgui_widget_get_id(vgui_modal_panel(shell))) == "designer.options",
            "activating the Tools item did not open the options modal");
    };
    using ui::designer_theme;
    const auto original = editor.preview().grid_settings();
    const auto before = editor.history().current();
    const auto revision = editor.history().revision();
    const auto selection = std::string(editor.selection());
    const auto live_name = editor.preview().edit_text("name");
    const auto shell_is_light = [&] {
        vgui_theme palette{};
        vgui_context_get_theme(shell, &palette);
        return palette.panel_bg == light_shell_theme().panel_bg;
    };
    const auto theme_menu = [&](designer_theme checked) {
        auto* menu = menus.activate(menus.open(4), 2); // Tools > Theme.
        require(menu && vgui_menu_get_count(menu) == 2, "could not open the Theme submenu");
        const auto dark = vgui_menu_get_item_at(menu, 0), light = vgui_menu_get_item_at(menu, 1);
        require(std::string_view(vgui_menu_get_item_text(menu, dark)) == "深色" &&
            std::string_view(vgui_menu_get_item_text(menu, light)) == "浅色" &&
            bool(vgui_menu_get_item_checked(menu, dark)) == (checked == designer_theme::dark) &&
            bool(vgui_menu_get_item_checked(menu, light)) == (checked == designer_theme::light),
            "Theme menu did not mark the current appearance");
        return menu;
    };
    const auto options_theme = [&](const char* id) {
        click(widget("designer.options.nav.appearance", VGUI_BUTTON));
        require(vgui_widget_get_visible(widget("designer.options.appearance", VGUI_PANEL)), "Appearance page did not open");
        auto* choice = widget(id, VGUI_BUTTON);
        click(choice);
        require(vgui_widget_get_checked(choice), "Options theme picker did not mark the selected appearance");
    };
    require(original.spacing == 24 && ui::load_designer_grid_settings(settings) == original,
        "startup settings were not applied to the canvas");
    require(shell_is_light() && ui::load_designer_theme(settings) == designer_theme::light,
        "startup did not restore the saved light theme");
    menus.activate(theme_menu(designer_theme::light), 0);
    require(!shell_is_light() && ui::load_designer_theme(settings) == designer_theme::dark &&
        ui::load_designer_grid_settings(settings) == original, "Tools > Theme did not apply and save dark independently of grid settings");
    open();
    auto* panel = vgui_modal_panel(shell);
    require(!find_widget(panel, "designer.options.nav.grid") &&
        std::string_view(vgui_widget_get_text(widget("designer.options.nav.canvas", VGUI_BUTTON))) == "画布" &&
        std::string_view(vgui_widget_get_text(widget("designer.options.grid.title", VGUI_LABEL))) == "网格",
        "Canvas must be a direct navigation entry with Grid as the right-hand title");
    auto* title = find_widget(panel, "designer.options.title");
    require(title != nullptr, "missing title drag handle");
    float x, y, w, h, panel_x, panel_y;
    vgui_widget_get_rect(panel, &panel_x, &panel_y, nullptr, nullptr);
    vgui_widget_get_rect(title, &x, &y, &w, &h);
    x += w / 2; y += h / 2;
    button(true, x, y);
    SDL_Event motion{};
    motion.type = SDL_EVENT_MOUSE_MOTION;
    motion.motion.windowID = win.id;
    motion.motion.state = SDL_BUTTON_LMASK;
    motion.motion.x = (x + 36) / win.pixel_ratio();
    motion.motion.y = (y + 50) / win.pixel_ratio();
    push(motion);
    button(false, x + 36, y + 50);
    frame();
    vgui_widget_get_rect(panel, &x, &y, nullptr, nullptr);
    require(x > panel_x + 20 && y > panel_y + 20, "dragging the title did not move the modal");

    const auto replace_text = [&](std::string_view id, const char* text) {
        auto* edit = widget(id, VGUI_EDIT);
        click(edit);
        require(vgui_get_focus(shell) == edit, "options entry did not receive focus");
        vgui_edit_select_all(edit);
        menus.key(SDLK_BACKSPACE, SDL_SCANCODE_BACKSPACE);
        SDL_Event e{};
        e.type = SDL_EVENT_TEXT_INPUT;
        e.text.windowID = win.id;
        e.text.text = text;
        push(e);
        frame();
    };
    replace_text("property:spacing", "36.5");
    require(editor.preview().grid_settings().spacing == 36.5f && ui::load_designer_grid_settings(settings) == original,
        "typing a valid spacing did not preview it before Enter or incorrectly saved it");
    replace_text("property:spacing", "0");
    require(editor.preview().grid_settings().spacing == 36.5f, "invalid spacing overwrote the last valid preview");
    click(widget("designer.options.nav.configuration", VGUI_BUTTON));
    require(vgui_modal_panel(shell) == panel && editor.preview().grid_settings().spacing == 36.5f &&
        ui::load_designer_grid_settings(settings) == original, "switching to configuration changed or saved the grid draft");
    click(widget("designer.options.nav.canvas", VGUI_BUTTON));
    require(vgui_widget_get_visible(widget("designer.options.grid", VGUI_PANEL)) &&
        vgui_widget_get_enabled(widget("designer.options.reset", VGUI_BUTTON)) &&
        std::string_view(vgui_edit_get_text(widget("property:spacing", VGUI_EDIT))) == "36.5",
        "returning to the grid did not restore the last valid draft");
    click(widget("property:visible", VGUI_CHECKBOX));
    require(!editor.preview().grid_settings().visible, "checkbox did not preview the visibility change");
    options_theme("designer.options.theme.light");
    require(vgui_modal_panel(shell) == panel && shell_is_light() && ui::load_designer_theme(settings) == designer_theme::dark,
        "switching the theme in Options did not preview it without saving or recreated the dialog");
    click(widget("designer.options.cancel", VGUI_BUTTON));
    require(!vgui_modal_active(shell) && editor.preview().grid_settings() == original &&
        ui::load_designer_grid_settings(settings) == original && !shell_is_light() &&
        ui::load_designer_theme(settings) == designer_theme::dark, "Cancel did not restore theme and grid without writing them");

    open();
    click(widget("designer.options.reset", VGUI_BUTTON));
    require(editor.preview().grid_settings() == ui::designer_grid_settings{} &&
        ui::load_designer_grid_settings(settings) == original, "Restore Defaults did not preview without writing");
    replace_text("property:spacing", "32");
    replace_text("property:dark_axis_color", "#12345680");
    auto expected = ui::designer_grid_settings{};
    expected.spacing = 32;
    expected.dark_colors.axis_color = 0x80123456u;
    require(editor.preview().grid_settings() == expected, "typing an RGBA color did not immediately preview it");
    auto* color_swatch = widget("property:dark_axis_color", VGUI_PANEL);
    click(color_swatch);
    auto* picker = vgui_popup_content(shell);
    auto* red = find_widget(picker, "property-grid.color-picker.swatch.2", VGUI_PANEL);
    require(red, "color picker did not open over the options modal");
    click(red);
    require(editor.preview().grid_settings().dark_colors.axis_color == 0xFFFF0000u, "color picker did not preview its preset");
    menus.key(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE);
    require(vgui_modal_active(shell) && !vgui_popup_active(shell) && editor.preview().grid_settings() == expected,
        "Escape did not cancel only the nested color picker");
    auto* style = widget("property:style", VGUI_BUTTON);
    click(style);
    require(vgui_context_get_open_menu(shell) != nullptr, "style menu did not open over the options modal");
    menus.key(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE);
    require(vgui_modal_active(shell) && !vgui_context_get_open_menu(shell), "Escape did not close only the style menu");
    click(style);
    menus.activate(vgui_context_get_open_menu(shell), 1);
    expected.style = ui::designer_grid_style::dots;
    require(editor.preview().grid_settings() == expected, "grid style did not preview the selected option");
    click(widget("category:dark_colors", VGUI_BUTTON));
    replace_text("property:light_axis_color", "#6385B0");
    expected.light_colors.axis_color = 0xFF6385B0u;
    require(editor.preview().grid_settings() == expected && !shell_is_light() &&
        ui::load_designer_grid_settings(settings) == original,
        "editing the light palette while dark is active did not stay independent and unsaved");
    options_theme("designer.options.theme.light");
    click(widget("designer.options.nav.configuration", VGUI_BUTTON));
    require(std::string_view(vgui_widget_get_text(widget("designer.options.configuration.theme", VGUI_LABEL))) == "界面主题：浅色",
        "configuration summary did not follow the live theme draft");
    click(widget("designer.options.ok", VGUI_BUTTON));
    require(!vgui_modal_active(shell) && editor.preview().grid_settings() == expected && shell_is_light() &&
        ui::load_designer_theme(settings) == designer_theme::light && ui::load_designer_grid_settings(settings) == expected,
        "OK did not persist the current theme and grid values");
    open();
    click(widget("designer.options.nav.appearance", VGUI_BUTTON));
    require(vgui_widget_get_checked(widget("designer.options.theme.light", VGUI_BUTTON)),
        "opening Options did not show the saved theme selection");
    click(widget("designer.options.reset", VGUI_BUTTON));
    require(!shell_is_light() && editor.preview().grid_settings() == expected &&
        ui::load_designer_theme(settings) == designer_theme::light, "Appearance Restore Defaults also changed the grid or saved prematurely");
    menus.key(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE);
    require(!vgui_modal_active(shell) && shell_is_light(), "Escape did not restore the previous appearance");
    menus.activate(theme_menu(designer_theme::light), 0);
    require(!shell_is_light() && ui::load_designer_theme(settings) == designer_theme::dark &&
        ui::load_designer_grid_settings(settings) == expected, "Tools theme choice did not reflect Options or preserve its grid changes");
    require(editor.history().revision() == revision && editor.history().current() == before &&
        editor.selection() == selection && editor.preview().edit_text("name") == live_name,
        "changing application-level options modified the design, selection or runtime input");
}

void exercise_grid_layers(app_mgr& app, font_familys_t* font, const ui::designer_strings& strings) {
    const auto require = [](bool passed, const char* message) {
        if (!passed) throw std::runtime_error(std::string("Canvas grid: ") + message);
    };
    constexpr int width = 640, height = 440;
    std::unique_ptr<ovg_ctx_cb, decltype(&free_ctx_cb)> cb(new_ctx_cb(), free_ctx_cb);
    std::unique_ptr<vgui_context, decltype(&vgui_destroy)> shell(vgui_create(font, 14), vgui_destroy);
    require(cb && shell, "cannot create readback context");
    vgui_context_set_size(shell.get(), width, height);
    ui::designer_preview_fonts fonts;
    ui::designer_preview preview(vgui_root(shell.get()), app.vgdev, font, fonts, {}, {}, {}, strings);
    vgui_widget_set_size(preview.widget(), width, height);
    preview.set_render_context(app.vgctx);

    vgui_render_target_desc desc;
    vgui_render_target_desc_default(&desc);
    desc.width = width;
    desc.height = height;
    desc.sample_count = 1;
    std::unique_ptr<vgui_render_target, decltype(&vgui_render_target_destroy)> target(
        vgui_render_target_create(shell.get(), app.vgctx, cb.get(), &desc), vgui_render_target_destroy);
    require(bool(target), "cannot create readback target");
    std::vector<uint32_t> pixels(width * height);
    ui::designer_grid_settings grid{.spacing = 80, .line_width = 3};
    grid.dark_colors = {0xFF191919u, 0xFF777777u, 0xFF888888u, 0xFF999999u};
    grid.light_colors = {0xFFE0E0E0u, 0xFF444444u, 0xFF333333u, 0xFF222222u};
    auto doc = ui::make_document();
    auto& root = doc.nodes.at(doc.root);
    const auto default_props = root.props;
    root.props["background"] = std::string("#00000000");
    root.children = {"solid"};
    doc.nodes.emplace("solid", ui::node{"panel", {}, {{"x", 160.0}, {"y", 40.0},
        {"width", 160.0}, {"height", 160.0}, {"background", std::string("#2458D0")}}, {}});
    uint64_t revision = 0;
    const auto mount = [&] {
        preview.sync(doc, ++revision, {}, {});
        preview.render(0);
        require(preview.artboard_rect_px().has_value(), "preview did not render");
    };
    struct samples { uint32_t guide, plain, control; };
    const auto sample = [&](bool visible) {
        grid.visible = visible;
        preview.set_grid_settings(grid);
        require(vgui_render_target_submit_context(target.get(), shell.get()), "cannot composite preview");
        const size_t bytes = pixels.size() * sizeof(pixels[0]);
        require(vgui_render_target_download(target.get(), pixels.data(), bytes) == bytes, "cannot read compositor pixels");
        const auto board = *preview.artboard_rect_px();
        const auto view = preview.snapshot()->view;
        const auto pixel = [&](float x, float y) {
            const int px = int(std::lround(board.x + x * board.width / view.width));
            const int py = int(std::lround(board.y + y * board.height / view.height));
            return pixels.at(size_t(py) * width + px);
        };
        return samples{pixel(80, 80), pixel(110, 110), pixel(240, 80)};
    };
    mount();
    const auto with_grid = sample(true), no_grid = sample(false);
    require(no_grid.control != no_grid.plain, "fixture's opaque control did not render");
    require(with_grid.control == no_grid.control, "lines painted over an opaque control");
    require(with_grid.guide != no_grid.guide && with_grid.plain == no_grid.plain,
        "lines did not show through a transparent root");
    preview.set_theme(ui::designer_theme::light);
    const auto light_grid = sample(true);
    require((with_grid.plain & 0x00FFFFFFu) == 0x191919u && (with_grid.guide & 0x00FFFFFFu) == 0x777777u &&
        (light_grid.plain & 0x00FFFFFFu) == 0xE0E0E0u && (light_grid.guide & 0x00FFFFFFu) == 0x444444u &&
        light_grid.control == with_grid.control, "each theme must draw its own canvas background and grid without recoloring controls");
    preview.set_theme(ui::designer_theme::dark);
    require(sample(true).guide == with_grid.guide, "switching back did not restore the dark grid palette");
    grid.style = ui::designer_grid_style::dots;
    const auto dots = sample(true);
    require(dots.control == no_grid.control && dots.guide != no_grid.guide,
        "dots did not stay behind the controls");

    root.props = default_props;
    const auto entry = root.props.find("background");
    require(entry != root.props.end() && entry->second == ui::value{std::string("#00000000")},
        "new documents should expose a transparent root background in the inspector");
    mount();
    require(sample(true).guide != sample(false).guide, "new document root hid the grid");
    root.props.erase("background");
    mount();
    require(sample(true).guide != sample(false).guide, "unset root background hid the grid");
    root.props["background"] = std::string("#21306580");
    mount();
    const auto translucent = sample(true), translucent_base = sample(false);
    // The offscreen target follows the renderer's 8-bit format, which can
    // be either RGBA or BGRA. Measure the RGB difference in either order.
    const auto contrast = [](uint32_t a, uint32_t b) {
        int difference = 0;
        for (int shift : {0, 8, 16})
            difference += std::abs(int((a >> shift) & 255u) - int((b >> shift) & 255u));
        return difference;
    };
    const int full = contrast(dots.guide, no_grid.guide);
    const int through_root = contrast(translucent.guide, translucent_base.guide);
    require(translucent_base.guide != no_grid.guide && through_root > 0 && through_root < full,
        "a translucent root did not tint and attenuate the grid");
    root.props["background"] = std::string("#213065");
    mount();
    require(sample(true).guide == sample(false).guide, "opaque root did not cover the grid");

    root.props = default_props;
    mount();
    preview.set_running(true);
    const auto run_grid = sample(true), run_clear = sample(false);
    require(run_grid.guide == run_clear.guide && run_grid.plain == run_clear.plain,
        "Run mode rendered design-only guides");
}

int smoke_animation_preview() {
    const auto require = [](bool passed, const char* message) {
        if (!passed) throw std::runtime_error(std::string("Animation preview: ") + message);
    };
    const auto near = [](double a, double b) { return std::abs(a - b) < .001; };
    constexpr int width = 800, height = 500;
    std::unique_ptr<vgui_context, decltype(&vgui_destroy)> shell(vgui_create(nullptr, 14), vgui_destroy);
    require(bool(shell), "create test shell");
    vgui_context_set_size(shell.get(), width, height);
    ui::designer_strings strings(ui::designer_language::english);
    ui::designer_preview_fonts fonts;
    ui::designer_preview preview(vgui_root(shell.get()), nullptr, nullptr, fonts, {}, {}, {}, strings);
    vgui_widget_set_size(preview.widget(), width, height);
    ui::session model;
    const fs::path file = VGUI_SOURCE_DIR "/sample/res/gradient-animation.vgui.json";
    require(model.load(file), "load animation example");
    const auto original = model.current();
    const auto revision = model.revision();
    const auto sync = [&](const ui::document& doc, uint64_t rev) { preview.sync(doc, rev, file.parent_path(), {}); };
    const auto state = [&] { return preview.capture_view().animation; };
    auto* transport = find_widget(preview.widget(), "designer.preview.animation");
    auto* play = find_widget(transport, "designer.preview.animation.play", VGUI_BUTTON);
    auto* restart = find_widget(transport, "designer.preview.animation.restart", VGUI_BUTTON);
    auto* slider = find_widget(transport, "designer.preview.animation.seek", VGUI_SLIDER);
    auto* mode = find_widget(preview.widget(), "designer.preview.mode", VGUI_BUTTON);
    require(transport && play && restart && slider && mode, "transport widgets are missing");
    const auto point_at = [&](vgui_widget* widget, float fraction) {
        vgui_layout(shell.get());
        ui::pixel_rect rect;
        vgui_widget_get_rect(widget, &rect.x, &rect.y, &rect.width, &rect.height);
        require(rect.width > 0 && rect.height > 0, "transport widget collapsed");
        return std::pair{rect.x + fraction * rect.width, rect.y + rect.height * .5f};
    };
    const auto click = [&](vgui_widget* widget) {
        const auto [x, y] = point_at(widget, .5f);
        require(vgui_hit_test(shell.get(), x, y) == widget, "transport widget cannot receive input");
        vgui_input_mouse_move(shell.get(), x, y);
        vgui_input_mouse_down(shell.get(), 1, x, y, 1);
        vgui_input_mouse_up(shell.get(), 1, x, y, 1);
    };
    sync(model.current(), revision);
    preview.render(0);
    require(preview.snapshot() && !vgui_widget_get_visible(transport), "Design must not show the transport");
    click(mode);
    preview.render(.25f);
    require(vgui_widget_get_visible(transport) && state().playing && state().time == 0, "Run begins playback at zero");
    const auto serial = preview.snapshot()->serial;
    preview.render(.8f);
    require(near(state().time, .8) && near(vgui_widget_get_value(slider), .25), "the thumb follows the playback clock");
    click(play);
    preview.render(1.f);
    require(!state().playing && near(state().time, .8), "pause holds the current frame");
    click(play);
    preview.render(.4f);
    require(state().playing && near(state().time, 1.2), "play resumes instead of restarting");

    const auto [x, y] = point_at(slider, .2f);
    const auto [end_x, end_y] = point_at(slider, .75f);
    require(vgui_hit_test(shell.get(), x, y) == slider, "the seek track cannot receive input");
    vgui_input_mouse_move(shell.get(), x, y);
    vgui_input_mouse_down(shell.get(), 1, x, y, 1);
    vgui_input_mouse_move(shell.get(), end_x, end_y);
    preview.render(1.f); // Scrubbing must take effect even before release.
    require(!state().playing && near(state().time, 2.4), "drag pauses playback and seeks to the requested frame");
    vgui_input_mouse_up(shell.get(), 1, end_x, end_y, 1);
    require(preview.snapshot()->serial == serial, "animation-only samples must not rebuild the layout snapshot");
    const auto saved_view = preview.capture_view();
    preview.restore_view({}); // Open a static page, then recreate the first preview from its saved view.
    sync(ui::make_document(), revision + 1);
    preview.set_running(true);
    preview.render(4.f);
    require(!vgui_widget_get_visible(transport), "static Run pages must not show the transport");
    preview.restore_view(saved_view);
    sync(model.current(), revision);
    preview.render(4.f);
    require(!state().playing && near(state().time, 2.4) && near(vgui_widget_get_value(slider), .75),
        "restoring a Run tab preserves the exact paused time");

    click(play);
    preview.render(2.f);
    require(near(state().time, 4.4) && near(state().window_start, 3.2) && near(vgui_widget_get_value(slider), .375),
        "looping pages continue in a new ruler interval without resetting the clock");
    click(slider);
    preview.render(.1f);
    require(!state().playing && near(state().time, 4.8), "seek on a later interval must not jump back to the first one");
    click(restart);
    preview.render(.5f);
    require(state().playing && state().time == 0 && state().window_start == 0, "restart samples the first frame and plays");

    auto once = original;
    once.nodes.at("sweep").props["gradient_animation_loop"] = false;
    once.nodes.at("tint").props["gradient_animation_loop"] = false;
    sync(once, revision + 2);
    preview.render(0.f);
    preview.render(4.f);
    require(!state().playing && near(state().time, 3.2) && vgui_widget_get_value(slider) == 1,
        "an all-one-shot page pauses at the end");
    click(mode);
    sync(model.current(), revision);
    require(!vgui_widget_get_visible(transport), "returning to Design hides the transport");
    click(mode);
    preview.render(.5f);
    require(state().playing && state().time == 0, "re-entering Run starts from the beginning");
    require(model.current() == original && model.revision() == revision && !model.dirty() && !model.can_undo(),
        "preview controls must not change document history");
    return 0;
}

int smoke_gradient_keyframes() {
    const auto require = [](bool ok, const char* message) {
        if (!ok) throw std::runtime_error(std::string("Gradient timeline: ") + message);
    };
    const auto near = [](double a, double b) { return std::abs(a - b) < .004; };
    const fs::path file = VGUI_SOURCE_DIR "/sample/res/gradient-keyframes.vgui.json";
    ui::session model;
    require(model.load(file), "load the two-track example");
    const auto before = model.current();
    std::unique_ptr<vgui_context, decltype(&vgui_destroy)> shell(vgui_create(nullptr, 14), vgui_destroy);
    require(bool(shell), "create test context");
    vgui_context_set_size(shell.get(), 900, 620);
    ui::designer_preview_fonts fonts;
    const ui::designer_strings strings(ui::designer_language::english);
    std::string status;
    ui::designer_preview preview(vgui_root(shell.get()), nullptr, nullptr, fonts, {},
        [&](std::string reason) { status = std::move(reason); }, {}, strings,
        [&](std::uint64_t revision, const std::vector<ui::edit>& batch) {
            return revision == model.revision() && model.apply(batch);
        });
    vgui_widget_set_size(preview.widget(), 900, 620);
    const auto update = [&] {
        preview.sync(model.current(), model.revision(), file.parent_path(), "highlight");
        preview.render(0);
        vgui_layout(shell.get());
    };
    const auto widget = [&](const char* id) {
        auto* w = find_widget(preview.widget(), id);
        require(w != nullptr, id);
        return w;
    };
    const auto click_at = [&](float x, float y, int clicks = 1) {
        vgui_input_mouse_move(shell.get(), x, y);
        vgui_input_mouse_down(shell.get(), 1, x, y, clicks);
        vgui_input_mouse_up(shell.get(), 1, x, y, clicks);
    };
    const auto click = [&](const char* id) {
        auto* w = widget(id);
        float x, y, width, height;
        vgui_widget_get_rect(w, &x, &y, &width, &height);
        require(vgui_hit_test(shell.get(), x + width / 2, y + height / 2) == w, id);
        click_at(x + width / 2, y + height / 2);
    };
    const auto press = [&](int key, int mods = 0) {
        const bool handled = vgui_input_key(shell.get(), key, 1, mods) != 0;
        vgui_input_key(shell.get(), key, 0, 0);
        return handled;
    };
    const auto submit = [&](vgui_widget* edit, const char* text) {
        vgui_set_focus(shell.get(), edit);
        press('A', VGUI_MOD_CTRL);
        vgui_input_text(shell.get(), text);
        press(VGUI_KEY_ENTER);
    };
    const auto edit_tint = [&](const char* hex) {
        click("designer.timeline.tint");
        auto* color_hex = find_widget(vgui_popup_content(shell.get()), "designer.timeline.tint.popup.hex", VGUI_EDIT);
        require(color_hex != nullptr, "open the selected key's tint picker");
        submit(color_hex, hex);
        vgui_popup_close(shell.get());
        update();
    };
    const auto keyframes = [&](const char* node) {
        std::vector<ui::gradient_keyframe> frames;
        std::string error;
        const auto& props = model.current().nodes.at(node).props;
        require(ui::decode_gradient_keyframes(std::get<std::string>(props.at("gradient_animation_keyframes")), frames, error),
            "decode committed keyframes");
        return frames;
    };
    update();
    require(!vgui_widget_get_visible(widget("designer.timeline")), "timeline starts collapsed");
    click("designer.preview.timeline");
    update();
    require(vgui_widget_get_visible(widget("designer.timeline")) && !preview.running(), "timeline opens underneath Design");
    auto* tracks = widget("designer.timeline.tracks");
    const auto point_at = [&](double seconds, float row) {
        float x, y, width;
        vgui_widget_get_rect(tracks, &x, &y, &width, nullptr);
        // TimelineView's 1px inset, 150px header and 8px start gutter at this width.
        return std::pair{x + 159 + static_cast<float>(seconds / 3.2) * (width - 174), y + 1 + row};
    };
    const auto state = [&] { return preview.capture_view().animation; };
    require(!vgui_widget_get_checked(widget("designer.timeline.snap")), "frame snapping starts off");
    click("designer.timeline.snap");
    update();
    const auto seek_with_ruler = [&](double time) {
        const auto [x, y] = point_at(time, 10);
        require(vgui_hit_test(shell.get(), x, y) == tracks, "ruler is hit-testable");
        click_at(x, y);
        update();
    };
    const auto precision_revision = model.revision();
    seek_with_ruler(.647);
    require(!state().playing && std::abs(state().time - 19. / 30.) < 1e-9 &&
        near(vgui_widget_get_value(widget("designer.preview.animation.seek")), (19. / 30.) / 3.2),
        "snapped ruler seeks to a 30 fps frame and synchronizes the preview transport");
    auto* playhead_edit = widget("designer.timeline.playhead");
    click("designer.preview.animation.play");
    preview.render(.05f);
    require(state().playing, "resume playback before submitting an exact time");
    vgui_set_focus(shell.get(), playhead_edit);
    press('A', VGUI_MOD_CTRL);
    preview.render(.05f); // A tick while the visible time is selected must not overwrite the edit.
    vgui_input_text(shell.get(), ".713125");
    preview.render(.05f);
    require(std::string_view(vgui_edit_get_text(playhead_edit)) == ".713125" && state().playing,
        "the running clock does not overwrite an in-progress edit");
    press(VGUI_KEY_ENTER);
    update();
    require(!state().playing && state().time == .713125 && !state().inspect_end,
        "typed time bypasses snapping and pauses at the exact subframe");
    submit(playhead_edit, "inf");
    update();
    require(state().time == .713125 && status == strings.text("timeline.playhead_invalid", ""),
        "invalid input cannot move the playhead");
    press(VGUI_KEY_ESCAPE);
    require(std::string_view(vgui_edit_get_text(playhead_edit)) == "0.713125", "Escape restores the current time");
    status.clear();
    submit(playhead_edit, "2.4");
    update();
    require(state().time == 2.4 && state().inspect_end, "an exact time can inspect the shorter looping track's terminal key");
    submit(playhead_edit, "90");
    update();
    require(state().time == 3.2 && state().inspect_end, "time above the ruler limits clamps to its exact end");
    submit(playhead_edit, "-1");
    update();
    require(state().time == 0. && !state().inspect_end && model.revision() == precision_revision && model.current() == before,
        "time below the ruler limits clamps to zero, with no history edits");

    const auto initial_keys = keyframes("highlight");
    const auto unsnapped_key_time = initial_keys[1].offset * 2.4;
    constexpr auto previous_key = "designer.timeline.prev_key";
    constexpr auto next_key = "designer.timeline.next_key";
    const auto enabled = [&](const char* control) { return vgui_widget_get_enabled(widget(control)) != 0; };
    const auto click_update = [&](const char* control) { click(control); update(); };
    const auto select_lane = [&](float row) {
        const auto [x, y] = point_at(0., row);
        require(vgui_hit_test(shell.get(), x - 80, y) == tracks, "timeline track header is hit-testable");
        click_at(x - 80, y);
        update();
    };
    require(!enabled(previous_key) && enabled(next_key), "there is no previous key at the first key's time");
    click("designer.preview.animation.play");
    preview.render(.05f);
    require(state().playing && enabled(previous_key), "advancing playback makes the first key reachable backwards");
    click_update(next_key);
    require(state().time == unsnapped_key_time && !state().playing && !state().inspect_end &&
        enabled("designer.timeline.geometry") && std::string_view(vgui_widget_get_text(widget("designer.timeline.tint"))) == initial_keys[1].tint,
        "next key pauses exactly at the unsnapped key and exposes its pose");
    submit(playhead_edit, ".713125"); // Leave the middle key selected but move the preview away from it.
    update();
    click_update(next_key);
    require(state().time == unsnapped_key_time && !state().playing, "jump can return to the already-selected key");
    click_update(next_key);
    require(state().time == 2.4 && state().inspect_end && !enabled(next_key) && enabled(previous_key),
        "the last key on a looping track shows its terminal pose and cannot wrap or jump to another track");
    select_lane(63);
    click_update(previous_key);
    require(state().time == keyframes("tint")[2].offset * 3.2 && !state().inspect_end &&
        std::string_view(vgui_widget_get_text(widget("designer.timeline.tint"))) == keyframes("tint")[2].tint,
        "switching track headers changes which track's keys are visited");
    select_lane(37);
    click_update(previous_key);
    click_update(previous_key);
    require(state().time == 0. && !state().inspect_end && !enabled(previous_key) && enabled(next_key),
        "reverse navigation stops at the selected track's first key");
    const auto [zoom_x, zoom_y] = point_at(0., 10);
    vgui_input_mouse_move(shell.get(), zoom_x, zoom_y);
    vgui_input_key(shell.get(), 0, 0, VGUI_MOD_CTRL);
    vgui_input_mouse_wheel(shell.get(), 0, 16);
    vgui_input_key(shell.get(), 0, 0, 0);
    click_update(next_key);
    require(state().time == unsnapped_key_time, "zooming the ruler does not change the next key's exact time");
    float ruler_left = 0, ruler_width = 0;
    vgui_widget_get_rect(tracks, &ruler_left, nullptr, &ruler_width, nullptr);
    click_at(ruler_left + ruler_width - 9, zoom_y);
    update();
    require(state().time == 1., "jump pans the off-screen middle key into the right edge of the zoomed ruler");
    click_update("designer.timeline.fit");
    require(model.revision() == precision_revision && model.current() == before && !model.dirty() && !model.can_undo(),
        "key navigation cannot edit the document or add undo entries");

    const auto [key_x, key_y] = point_at(unsnapped_key_time, 37);
    click_at(key_x, key_y);
    update();
    require(state().time == unsnapped_key_time, "selecting an existing subframe key does not snap its preview time");
    const auto [target_x, target_y] = point_at(1.174, 37);
    vgui_input_mouse_down(shell.get(), 1, key_x, key_y, 1);
    vgui_input_mouse_move(shell.get(), target_x, target_y);
    update();
    require(model.revision() == precision_revision, "snapped drag stays transient until release");
    vgui_input_mouse_up(shell.get(), 1, target_x, target_y, 1);
    update();
    require(model.revision() == precision_revision + 1 && std::abs(keyframes("highlight")[1].offset * 2.4 - 35. / 30.) < 1e-9 &&
        std::abs(state().time - 35. / 30.) < 1e-9, "drag snaps only the chosen key and updates the playhead in one edit");
    require(model.undo() && model.current() == before, "one undo restores the original subframe key time");
    update();
    submit(playhead_edit, ".713125");
    update();
    click("designer.timeline.add");
    update();
    const auto added = keyframes("highlight");
    require(added.size() == initial_keys.size() + 1 && std::abs(added[1].offset * 2.4 - 21. / 30.) < 1e-9,
        "Add Key snaps the insertion while the exact seek remains available");
    require(model.undo() && model.current() == before, "undo the snapped insertion");
    update();
    click("designer.timeline.snap");
    update();
    seek_with_ruler(.647);
    require(near(state().time, .647) && std::abs(state().time - 19. / 30.) > .01,
        "turning off snapping restores free ruler positioning");

    const auto [ruler_x, ruler_y] = point_at(.6, 10);
    require(vgui_hit_test(shell.get(), ruler_x, ruler_y) == tracks, "ruler is hit-testable");
    click_at(ruler_x, ruler_y);
    update();
    require(preview.running() && !preview.capture_view().animation.playing && near(preview.capture_view().animation.time, .6) &&
        model.current() == before && !model.dirty(), "ruler seeks and pauses without editing the page");
    const auto [insert_x, insert_y] = point_at(.6, 37);
    click_at(insert_x, insert_y, 2);
    update();
    const auto double_clicked = keyframes("highlight");
    require(double_clicked.size() == 4 && near(double_clicked[1].offset * 2.4, .6) &&
        double_clicked[1] == ui::gradient_keyframe{.offset = double_clicked[1].offset},
        "double-click inserts a base key, not a sampled pose");
    require(model.undo() && model.current() == before, "undo the double-click insertion");
    update();
    click_at(ruler_x, ruler_y);
    update();
    click("designer.timeline.add");
    update();
    auto frames = keyframes("highlight");
    require(frames.size() == 4 && near(frames[1].offset * 2.4, .6) && keyframes("tint").size() == 4 &&
        frames[1] == ui::gradient_keyframe{.offset = frames[1].offset},
        "Add Key inserts a base key in the chosen track, not a sampled pose or a key in its neighbour");
    submit(widget("designer.timeline.x_edit"), ".25");
    update();
    require(near(keyframes("highlight")[1].x, .25), "editing a numeric pose field updates the selected key");
    edit_tint("#658DCE80");
    frames = keyframes("highlight");
    require(frames[1].tint == "#658DCE" && near(frames[1].opacity, 128. / 255), "picker edits both RGB tint and opacity");

    const auto [ax, ay] = point_at(.6, 37);
    const auto [bx, by] = point_at(.8, 37);
    const auto before_drag = model.revision();
    vgui_input_mouse_move(shell.get(), ax, ay);
    vgui_input_mouse_down(shell.get(), 1, ax, ay, 1);
    vgui_input_mouse_move(shell.get(), bx, by);
    update();
    require(model.revision() == before_drag, "drag motion must not create document undo steps");
    vgui_input_mouse_up(shell.get(), 1, bx, by, 1);
    update();
    frames = keyframes("highlight");
    require(model.revision() == before_drag + 1 && near(frames[1].offset * 2.4, .8) && near(frames[1].x, .25),
        "drag commits one retiming transaction and preserves pose values");
    require(model.undo(), "undo the keyframe move");
    update();
    require(near(keyframes("highlight")[1].offset * 2.4, .6), "undo restores the keyframe's time");
    require(model.redo(), "redo the keyframe move");
    update();
    const auto [selected_x, selected_y] = point_at(.8, 37);
    click_at(selected_x, selected_y);
    update();
    click("designer.timeline.delete");
    update();
    require(keyframes("highlight").size() == 3, "Delete removes the selected key");
    require(model.undo(), "undo the keyframe removal");
    update();
    require(keyframes("highlight").size() == 4 && keyframes("tint").size() == 4,
        "undo restores the edited lane and leaves the second lane untouched");
    click_at(point_at(.2, 37).first, point_at(.2, 37).second);
    update();
    const auto before_resize = keyframes("highlight");
    auto* duration_edit = find_widget(preview.widget(), "designer.timeline.duration_edit", VGUI_EDIT);
    require(duration_edit != nullptr, "timeline exposes a duration editor");
    submit(duration_edit, "3.61");
    update();
    require(model.current().nodes.at("highlight").props.at("gradient_animation_duration") == ui::value{3.61},
        "editing timeline duration updates the selected gradient property");
    const auto resized = keyframes("highlight");
    require(resized.size() == before_resize.size() &&
        std::ranges::equal(resized, before_resize, [&](const auto& a, const auto& b) { return near(a.offset, b.offset); }),
        "duration editing preserves normalized keyframe timing");
    click("designer.timeline.snap");
    update();
    submit(playhead_edit, "3.61");
    update();
    require(state().time == 3.61 && state().inspect_end,
        "a terminal key outside the 30 fps grid can still be inspected exactly");
    click("designer.timeline.snap");
    update();
    click("designer.timeline.loop");
    update();
    require(model.current().nodes.at("highlight").props.at("gradient_animation_loop") == ui::value{false},
        "timeline loop checkbox updates the selected gradient property");
    const auto saved = preview.capture_view();
    preview.restore_view({});
    update();
    require(!vgui_widget_get_visible(widget("designer.timeline")), "another tab has its own collapsed state");
    preview.restore_view(saved);
    update();
    require(vgui_widget_get_visible(widget("designer.timeline")) && status.empty(), "restoring a tab reopens its timeline");

    require(model.load(file), "reload the untouched two-track example for clipboard and batch-edit tests");
    update();
    const auto highlight = keyframes("highlight");
    const auto tint = keyframes("tint");
    constexpr double highlight_duration = 2.4, tint_duration = 3.2;
    constexpr float ruler_row = 10, highlight_row = 37, tint_row = 63;
    const auto highlight_key = highlight[1].offset * highlight_duration;
    const auto tint_key = tint[1].offset * tint_duration;
    const auto tint_late_key = tint[2].offset * tint_duration;
    const auto click_timeline = [&](double second, float row, int mods = 0) {
        const auto [x, y] = point_at(second, row);
        require(vgui_hit_test(shell.get(), x, y) == tracks, "timeline can receive mouse input");
        // Mouse events read modifiers from the context, which key input updates.
        vgui_input_key(shell.get(), 0, 0, mods);
        click_at(x, y);
        vgui_input_key(shell.get(), 0, 0, 0);
        update();
    };
    const auto activate_track = [&](float row) {
        const auto [x, y] = point_at(0, row);
        require(vgui_hit_test(shell.get(), x - 100, y) == tracks, "track header can receive mouse input");
        click_at(x - 100, y);
        update();
    };
    const auto matches_pose_at = [&](const auto& actual, ui::gradient_keyframe source, double duration, double time) {
        source.offset = actual.offset;
        return actual == source && near(actual.offset * duration, time);
    };

    const auto copy_revision = model.revision();
    click_timeline(0, highlight_row);
    click_timeline(highlight_key, highlight_row, VGUI_MOD_SHIFT);
    click("designer.timeline.copy");
    update();
    require(model.revision() == copy_revision && model.current() == before && !model.can_undo(),
        "Copy does not edit the document or history");
    activate_track(tint_row);
    constexpr double single_anchor = .8;
    click_timeline(single_anchor, ruler_row);
    require(vgui_get_focus(shell.get()) == tracks && press('V', VGUI_MOD_CTRL), "focused timeline handles Ctrl+V");
    update();
    const auto pasted = keyframes("tint");
    require(model.revision() == copy_revision + 1 && pasted.size() == 6 && keyframes("highlight") == highlight &&
        matches_pose_at(pasted[1], highlight[0], tint_duration, single_anchor) &&
        matches_pose_at(pasted[3], highlight[1], tint_duration, single_anchor + highlight_key) &&
        std::vector{pasted[0], pasted[2], pasted[4], pasted[5]} == tint,
        "single-source paste preserves full poses and second spacing on the active track's different duration");
    const auto after_single_paste = model.current();
    const auto single_revision = model.revision();
    click_timeline(2.6, ruler_row); // The first key fits but the second would exceed 3.2 s.
    press('V', VGUI_MOD_CTRL);
    update();
    require(model.revision() == single_revision && model.current() == after_single_paste,
        "an out-of-duration paste leaves even the keys that would fit unpasted");
    require(model.undo() && model.current() == before && !model.can_undo(), "one undo removes the entire paste");
    update();
    require(model.redo() && model.current() == after_single_paste, "redo restores the exact pasted keys");
    update();

    activate_track(highlight_row);
    click_timeline(highlight_key, highlight_row);
    click_timeline(tint_key, tint_row, VGUI_MOD_CTRL);
    require(press('C', VGUI_MOD_CTRL), "focused timeline handles Ctrl+C");
    update();
    require(model.undo() && model.current() == before, "rebuild the timeline projection after copying");
    update();
    activate_track(tint_row);
    constexpr double group_anchor = .34;
    const auto track_delta = tint_key - highlight_key;
    click_timeline(group_anchor, ruler_row);
    const auto multi_revision = model.revision();
    click("designer.timeline.paste");
    update();
    auto pasted_highlight = keyframes("highlight");
    auto pasted_tint = keyframes("tint");
    require(model.revision() == multi_revision + 1 && pasted_highlight.size() == 4 && pasted_tint.size() == 5 &&
        matches_pose_at(pasted_highlight[1], highlight[1], highlight_duration, group_anchor) &&
        matches_pose_at(pasted_tint[1], tint[1], tint_duration, group_anchor + track_delta),
        "multi-source paste anchors the whole group and returns keys to their original tracks after a rebuild");
    pasted_highlight.erase(pasted_highlight.begin() + 1);
    pasted_tint.erase(pasted_tint.begin() + 1);
    require(pasted_highlight == highlight && pasted_tint == tint, "pasting does not replace existing keys");
    const auto after_multi_paste = model.current();
    activate_track(tint_row);
    click_timeline(tint_late_key - track_delta, ruler_row); // Only the second track would overlap.
    click("designer.timeline.paste");
    update();
    require(model.revision() == multi_revision + 1 && model.current() == after_multi_paste,
        "overlap on one track rejects the entire paste with no history entry");
    require(model.undo() && model.current() == before && !model.can_undo(), "one undo removes a paste across tracks");
    update();
    require(model.redo() && model.current() == after_multi_paste, "redo restores both tracks of the paste");
    update();

    const auto select_mixed_poses = [&] {
        activate_track(highlight_row);
        click_timeline(0, highlight_row);
        click_timeline(tint_late_key, tint_row, VGUI_MOD_CTRL); // Keep the highlight key primary.
    };
    select_mixed_poses();
    auto expected_highlight = keyframes("highlight");
    auto expected_tint = keyframes("tint");
    const auto tint_index = std::size_t{3}; // The pasted key sorted before the selected original tint key.
    const auto batch_revision = model.revision();
    submit(widget("designer.timeline.x_edit"), "-0.9"); // Already the primary key's X.
    update();
    expected_tint[tint_index].x = expected_highlight[0].x;
    require(model.revision() == batch_revision + 1 && keyframes("highlight") == expected_highlight &&
        keyframes("tint") == expected_tint, "submitting the primary X resolves a mixed field without changing other pose values");
    const auto after_x = model.current();
    submit(widget("designer.timeline.y_edit"), ".4");
    update();
    expected_highlight[0].y = expected_tint[tint_index].y = .4;
    require(model.revision() == batch_revision + 2 && keyframes("highlight") == expected_highlight &&
        keyframes("tint") == expected_tint, "batch editing Y changes only Y on the selected keys across tracks");
    const auto after_y = model.current();
    require(model.undo() && model.current() == after_x, "one undo restores both tracks of the batch edit");
    update();
    require(model.redo() && model.current() == after_y, "redo restores both tracks of the batch edit");
    update();
    select_mixed_poses();
    const auto tint_revision = model.revision();
    edit_tint("#658DCE40"); // 0x40 is the unchanged rounded alpha of the primary key (opacity 0.25).
    expected_highlight[0].tint = expected_tint[tint_index].tint = "#658DCE";
    require(model.revision() == tint_revision + 1 && keyframes("highlight") == expected_highlight &&
        keyframes("tint") == expected_tint, "tint-only batch editing preserves each key's exact opacity and other fields");
    require(model.undo() && model.current() == after_y, "one undo restores both tints");
    update();

    // Two linear tracks can share an endpoint edit even when their authored
    // endpoints differ. An inherited value on either key must remain undoable.
    select_mixed_poses();
    click("designer.timeline.geometry");
    auto* shape = vgui_popup_content(shell.get());
    auto* field = find_widget(shape, "designer.timeline.geometry.end_y_edit", VGUI_EDIT);
    require(field && std::string_view(vgui_edit_get_text(field)).empty(), "mixed inherited endpoints show a placeholder");
    auto shape_highlight = keyframes("highlight"), shape_tint = keyframes("tint");
    const auto shape_revision = model.revision();
    submit(field, ".8");
    update();
    constexpr auto end_y = std::size_t(ui::gradient_geometry_axis::end_y);
    shape_highlight[0].geometry[end_y] = shape_tint[tint_index].geometry[end_y] = .8;
    require(model.revision() == shape_revision + 1 && keyframes("highlight") == shape_highlight &&
        keyframes("tint") == shape_tint, "one shape edit updates only the chosen channel on both tracks");
    vgui_popup_close(shell.get());
    require(model.undo() && model.current() == after_y, "one undo clears both endpoint overrides");
    update();

    // A copied palette has positional stop identities. The 5-stop highlight
    // cannot be pasted onto the 3-stop tint track, even at a free time.
    auto with_palette = keyframes("highlight");
    std::string stop_error;
    const auto& stops = model.current().nodes.at("highlight").props.at("gradient_stops");
    require(ui::decode_gradient_stops(std::get<std::string>(stops), with_palette[0].stops, stop_error),
        "load the highlight's five stops");
    const std::vector<ui::edit> seed_palette{ui::set_property{"highlight", "gradient_animation_keyframes",
        ui::encode_gradient_keyframes(with_palette)}};
    require(model.apply(seed_palette), "prepare a copied key with explicit color stops");
    update();
    activate_track(highlight_row); // Clear the preceding multi-selection.
    click_timeline(0, highlight_row);
    click("designer.timeline.copy");
    update();
    activate_track(tint_row);
    click_timeline(.8, ruler_row);
    const auto palette_revision = model.revision();
    const auto palette_doc = model.current();
    status.clear();
    click("designer.timeline.paste");
    update();
    require(model.revision() == palette_revision && model.current() == palette_doc &&
        status == strings.text("timeline.paste_stops", ""), "a mismatched stop count atomically rejects paste");

    // With a first animation key later than zero, Design still displays the
    // authored paint, not the pose that the running sampler would hold at 0s.
    require(model.load(file), "restore the original track for a Design capture");
    auto delayed = keyframes("highlight");
    delayed.front().offset = .125;
    const std::vector<ui::edit> delay_first{ui::set_property{"highlight", "gradient_animation_keyframes",
        ui::encode_gradient_keyframes(delayed)}};
    require(model.apply(delay_first), "delay the first key");
    update();
    if (preview.running()) { click("designer.preview.mode"); update(); }
    activate_track(highlight_row);
    click("designer.timeline.capture");
    update();
    const auto design_keys = keyframes("highlight");
    require(design_keys.size() == delayed.size() + 1 && design_keys.front().offset == 0. &&
        design_keys.front().x == 0. && design_keys.front().opacity == 1. && design_keys.front().stops.size() == 5 &&
        std::equal(delayed.begin(), delayed.end(), design_keys.begin() + 1),
        "Design capture at zero stores authored paint rather than the delayed first animation key");
    return 0;
}

struct native_keyframe_input {
    os_window& win;
    vgui_context* shell;
    ui::designer_preview& preview;
    const std::function<void()>& frame;
    void (*require)(bool, std::string_view);

    vgui_widget* widget(const char* id) const {
        auto* w = find_widget(preview.widget(), id);
        require(w != nullptr, id);
        return w;
    }
    static ui::pixel_rect rect(vgui_widget* w) {
        ui::pixel_rect r;
        vgui_widget_get_rect(w, &r.x, &r.y, &r.width, &r.height);
        return r;
    }
    void push(SDL_Event e) const { require(SDL_PushEvent(&e), "cannot queue SDL input"); }
    void button(bool down, glm::vec2 px, Uint8 mouse_button = SDL_BUTTON_LEFT) const {
        const auto p = px / win.pixel_ratio();
        SDL_Event e{};
        e.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
        e.button.windowID = win.id;
        e.button.button = mouse_button;
        e.button.down = down;
        e.button.clicks = 1;
        e.button.x = p.x;
        e.button.y = p.y;
        push(e);
    }
    void motion(glm::vec2 from, glm::vec2 to, SDL_MouseButtonFlags buttons = SDL_BUTTON_LMASK) const {
        const auto p = to / win.pixel_ratio(), rel = (to - from) / win.pixel_ratio();
        SDL_Event e{};
        e.type = SDL_EVENT_MOUSE_MOTION;
        e.motion.windowID = win.id;
        e.motion.state = buttons;
        e.motion.x = p.x;
        e.motion.y = p.y;
        e.motion.xrel = rel.x;
        e.motion.yrel = rel.y;
        push(e);
    }
    void click_at(vgui_widget* w, glm::vec2 p, Uint8 mouse_button = SDL_BUTTON_LEFT) const {
        require(vgui_hit_test(shell, p.x, p.y) == w, "click does not hit the intended control");
        button(true, p, mouse_button);
        button(false, p, mouse_button);
        frame();
    }
    void click(vgui_widget* w) const {
        const auto r = rect(w);
        click_at(w, {r.x + r.width / 2, r.y + r.height / 2});
    }
    void click(const char* id) const { click(widget(id)); }

    vgui_widget* picker_widget(const char* id, vgui_widget_kind kind) const {
        auto* w = find_widget(vgui_popup_content(shell), id, kind);
        require(w != nullptr, id);
        return w;
    }
    vgui_widget* picker_hex() const { return picker_widget("designer.preview.stop_color.hex", VGUI_EDIT); }
    void open_stop_picker(vgui_widget* canvas, glm::vec2 p, const ui::gradient_stop& stop) const {
        require(!vgui_popup_active(shell), "a popup is already open before the stop click");
        click_at(canvas, p, SDL_BUTTON_RIGHT); // Down and up must both be dispatched before layout/render.
        auto* popup = vgui_popup_content(shell);
        require(popup && find_widget(popup, "designer.preview.stop_color", VGUI_PANEL) == popup &&
            !vgui_widget_get_parent(popup), "right click did not open the detached canvas stop picker");
        require(ui::parse_text_color(vgui_edit_get_text(picker_hex())) == ui::parse_text_color(stop.color),
            "picker did not open at the targeted stop's original color and alpha");
    }
    void type_stop_hex(const char* value) const {
        auto* hex = picker_hex();
        click(hex);
        require(vgui_get_focus(shell) == hex, "Hex did not receive focus");
        vgui_edit_select_all(hex);
        SDL_Event text{};
        text.type = SDL_EVENT_TEXT_INPUT;
        text.text.windowID = win.id;
        text.text.text = value;
        push(text);
        frame(); // No Enter or other submission event.
    }
    void adjust_stop_alpha(std::uint32_t expected_rgb) const {
        auto* alpha = picker_widget("designer.preview.stop_color.alpha", VGUI_CUSTOM);
        const auto r = rect(alpha);
        click_at(alpha, {std::round(r.x + r.width / 4), r.y + r.height / 2});
        auto* hex = picker_hex();
        const auto color = ui::parse_text_color(vgui_edit_get_text(hex));
        require(vgui_get_focus(shell) != hex && color && (*color & 0xFFFFFFu) == expected_rgb &&
            std::abs(static_cast<int>(*color >> 24) - 64) <= 2,
            "alpha click did not preview roughly 25% opacity without altering the current RGB");
    }
    void close_stop_picker(const char* id) const {
        click(picker_widget(id, VGUI_BUTTON));
        require(!vgui_popup_active(shell), "picker button did not close the popup");
    }
    glm::vec2 fitted_key(vgui_widget* tracks, float fraction = .5f, float row = 37) const {
        const auto r = rect(tracks);
        const float scale = vgui_context_get_ui_scale(shell);
        // Fit leaves an 8px leading gutter and uses width minus the header and 24px for the ruler.
        const float header = std::clamp((r.width - 2) * .28f, 92 * scale, 150 * scale);
        return {r.x + 1 + header + 8 + (r.width - header - 24) * fraction, r.y + 1 + row * scale};
    }
};

void exercise_icon_region(os_window& win, vgui_context* shell, designer& editor,
                          const std::function<void()>& frame, const fs::path& sandbox) {
    const auto require = [](bool passed, std::string_view message) {
        if (!passed) throw std::runtime_error("Native icon region: " + std::string(message));
    };
    const auto file = sandbox / "icon-region.vgui.json";
    const auto source = sandbox / "icon-region.png";
    require(fs::copy_file(fs::path(VGUI_SOURCE_DIR) / "sample/res/image-button-states.png", source), "copy 384 × 128 fixture");
    require(fs::copy_file(fs::path(VGUI_SOURCE_DIR) / "sample/res/choice-indicator-sheet.png",
        sandbox / "shared-atlas.png"), "copy 273 × 139 choice atlas");
    auto doc = ui::make_document();
    doc.resources = {{"icons", "icon-region.png"}, {"missing", "missing-region.png"}, {"shared", "shared-atlas.png"}};
    doc.nodes.at(doc.root).children = {"crop", "missing", "radio", "check"};
    doc.nodes.emplace("crop", ui::node{"button", "Crop", {{"x", 42.}, {"y", 42.}, {"width", 174.}, {"height", 66.},
        {"icon", std::string("icons")}, {"icon_source_x", 16.}, {"icon_source_y", 8.}, {"icon_source_width", 48.},
        {"icon_source_height", 32.}}, {}});
    doc.nodes.emplace("missing", ui::node{"button", "Missing", {{"x", 260.}, {"y", 42.}, {"width", 130.}, {"height", 50.},
        {"icon", std::string("missing")}}, {}});
    doc.nodes.emplace("radio", ui::node{"radio", "Radio", {{"x", 42.}, {"y", 125.}, {"width", 174.}, {"height", 34.},
        {"indicator_atlas", std::string("shared")}, {"indicator_source_x", 5.}, {"indicator_source_y", 5.},
        {"indicator_source_width", 128.}, {"indicator_source_height", 128.}}, {}});
    doc.nodes.emplace("check", ui::node{"checkbox", "Check", {{"x", 260.}, {"y", 125.}, {"width", 130.}, {"height", 34.},
        {"indicator_atlas", std::string("shared")}}, {}});
    require(ui::write_document(file, doc) && editor.open(file), "open crop fixture");
    frame();
    const native_keyframe_input input{win, shell, editor.preview(), frame, require};
    const ui::native_menu_input menus(win, shell, frame);
    auto* open = find_widget(vgui_root(shell), "designer.icon_region.open", VGUI_BUTTON);
    require(open != nullptr && !vgui_widget_get_visible(open), "crop action must not appear for a panel");
    const auto select_node = [&](const char* id) {
        const auto* layout = editor.preview().snapshot();
        const auto board = editor.preview().artboard_rect_px();
        require(layout && board, "rendered fixture is missing");
        const auto entry = std::ranges::find(layout->nodes, id, &ui::node_layout::id);
        require(entry != layout->nodes.end() && entry->visible, "control is not on the board");
        const auto& r = entry->visible_px;
        const glm::vec2 point{board->x + (r.x + r.width / 2) * board->width / layout->pixel_width,
            board->y + (r.y + r.height / 2) * board->height / layout->pixel_height};
        input.click_at(input.widget("designer.preview.canvas"), point);
        require(editor.selection() == id && vgui_widget_get_visible(open) && vgui_widget_get_enabled(open), "crop action not enabled for the selected icon");
    };
    const auto& history = editor.history();
    const auto initial = history.revision();
    select_node("missing");
    input.click(open);
    require(!vgui_modal_active(shell) && history.current() == doc && history.revision() == initial, "missing image opened a picker or changed history");
    select_node("crop");
    const auto modal_widget = [&](const char* id, vgui_widget_kind kind) {
        auto* w = find_widget(vgui_modal_panel(shell), id, kind);
        require(w != nullptr, id);
        return w;
    };
    constexpr std::array zoom_ids{"designer.icon_region.zoom_out", "designer.icon_region.zoom_in",
        "designer.icon_region.zoom_fit", "designer.icon_region.zoom_actual"};
    constexpr auto zoom_label = "designer.icon_region.zoom_level";
    constexpr std::array field_ids{"designer.icon_region.x", "designer.icon_region.y",
        "designer.icon_region.width", "designer.icon_region.height"};
    const auto coords = [&] { return std::string_view(vgui_widget_get_text(modal_widget("designer.icon_region.coordinates", VGUI_LABEL))); };
    const auto picker = [&] {
        input.click(open);
        auto* panel = vgui_modal_panel(shell);
        require(panel && std::string_view(vgui_widget_get_id(panel)) == "designer.icon_region", "inspector did not open the crop modal");
        require(coords().starts_with("X: 16     Y: 8     48 × 32 px"), "picker did not show the authored source area");
        for (const auto* id : zoom_ids) require(!find_widget(panel, id, VGUI_BUTTON), "button picker acquired a four-state zoom button");
        for (const auto* id : field_ids) require(!find_widget(panel, id, VGUI_EDIT), "button picker acquired a four-state edit field");
        require(!find_widget(panel, zoom_label, VGUI_LABEL), "button picker acquired a four-state zoom readout");
    };
    const auto canvas = [&] { return modal_widget("designer.icon_region.image", VGUI_CUSTOM); };
    const auto drag_at = [&](glm::vec2 from, glm::vec2 to, Uint8 button, SDL_MouseButtonFlags mask) {
        require(vgui_hit_test(shell, from.x, from.y) == canvas(), "drag does not start on the canvas");
        input.motion(to, from, 0);
        input.button(true, from, button);
        input.motion(from, to, mask);
        input.button(false, to, button);
        frame();
    };
    const auto fit_scale = [](ui::pixel_rect r, int width, int height) {
        return std::min(r.width / width, r.height / height);
    };
    const auto centered_pixel = [](ui::pixel_rect r, int width, int height, float scale, glm::vec2 p) {
        return glm::vec2{
            std::round(r.x + r.width / 2 + (p.x + .5f - width / 2.f) * scale),
            std::round(r.y + r.height / 2 + (p.y + .5f - height / 2.f) * scale)};
    };
    const auto drag_pixels = [&](int width, int height, glm::vec2 a, glm::vec2 b) {
        const auto r = input.rect(canvas());
        const auto scale = fit_scale(r, width, height);
        require(scale > 1, "image test requires at least one device pixel per source pixel");
        drag_at(centered_pixel(r, width, height, scale, a), centered_pixel(r, width, height, scale, b),
            SDL_BUTTON_LEFT, SDL_BUTTON_LMASK);
    };
    const auto drag = [&] {
        drag_pixels(384, 128, {149, 91}, {64, 22});
        require(coords().starts_with("X: 64     Y: 22     86 × 70 px"), "reverse drag did not map to the original image pixels");
        require(history.current() == doc && history.revision() == initial && !history.can_undo(), "drag created a document edit before confirmation");
    };
    picker();
    drag();
    menus.key(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE);
    require(!vgui_modal_active(shell) && history.current() == doc && history.revision() == initial, "Escape did not cancel the draft");
    picker();
    drag();
    input.click(modal_widget("designer.icon_region.ok", VGUI_BUTTON));
    auto expected = doc;
    auto& crop = expected.nodes.at("crop").props;
    const ui::icon_source_rect region{64, 22, 86, 70};
    for (std::size_t i = 0; i < region.size(); ++i) crop[ui::icon_source_fields[i]] = double(region[i]);
    require(!vgui_modal_active(shell) && history.current() == expected && history.revision() == initial + 1 && history.can_undo(),
        "confirmation must write only the four fields in one transaction");
    menus.activate(menus.open(1), 0);
    require(history.current() == doc && !history.can_undo() && history.can_redo(), "one undo did not restore the original rectangle");
    picker();
    input.click(modal_widget("designer.icon_region.whole", VGUI_BUTTON));
    require(coords().starts_with("X: 0     Y: 0     384 × 128 px") && history.current() == doc, "whole image should only change the draft");
    input.click(modal_widget("designer.icon_region.ok", VGUI_BUTTON));
    expected = doc;
    for (const auto* key : ui::icon_source_fields) expected.nodes.at("crop").props.erase(key);
    require(history.current() == expected && history.can_undo(), "whole image did not clear the source rectangle");
    menus.activate(menus.open(1), 0);
    picker();
    const auto unchanged = history.revision();
    input.click(modal_widget("designer.icon_region.ok", VGUI_BUTTON));
    require(history.current() == doc && history.revision() == unchanged && !history.can_undo(), "unmodified confirmation created history");

    const auto open_choice_region = [&](const char* id) {
        select_node(id);
        require(std::string_view(vgui_widget_get_text(open)) ==
            editor.strings().text("indicator_region.open", "Select Four-State Region…"), "choice did not show the four-state action");
        input.click(open);
        require(vgui_modal_panel(shell) != nullptr, "choice did not open the image picker");
    };
    const auto ok = [&] { return modal_widget("designer.icon_region.ok", VGUI_BUTTON); };
    const auto field = [&](std::size_t index) { return modal_widget(field_ids[index], VGUI_EDIT); };
    const auto draft_is = [&](ui::icon_source_rect region, bool valid, const char* message) {
        const auto [x, y, w, h] = region;
        require(coords().starts_with("X: " + std::to_string(x) + "     Y: " + std::to_string(y) + "     " +
            std::to_string(w) + " × " + std::to_string(h) + " px") && (vgui_widget_get_enabled(ok()) != 0) == valid, message);
        for (std::size_t i = 0; i < region.size(); ++i)
            require(std::string_view(vgui_edit_get_text(field(i))) == std::to_string(region[i]), message);
    };
    const auto replace_text = [&](std::size_t index, const char* value) {
        auto* edit = field(index);
        input.click(edit);
        require(vgui_get_focus(shell) == edit, "numeric field did not take focus");
        for (bool down : {true, false}) {
            SDL_Event e{};
            e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
            e.key.windowID = win.id;
            e.key.key = SDLK_A;
            e.key.scancode = SDL_SCANCODE_A;
            e.key.mod = SDL_KMOD_LCTRL;
            e.key.down = down;
            input.push(e);
        }
        if (*value) {
            SDL_Event e{};
            e.type = SDL_EVENT_TEXT_INPUT;
            e.text.windowID = win.id;
            e.text.text = value;
            input.push(e);
            frame(); // Assert the draft before Enter or blur.
        } else menus.key(SDLK_BACKSPACE, SDL_SCANCODE_BACKSPACE);
        require(std::string_view(vgui_edit_get_text(edit)) == value, "Ctrl+A did not replace the complete field text");
    };
    const auto zoom_text = [&] {
        return std::string_view(vgui_widget_get_text(modal_widget(zoom_label, VGUI_LABEL)));
    };
    const auto fit_text = editor.strings().text("indicator_region.zoom_fitted", "Auto");
    const auto zoom = [&] {
        const std::string text(zoom_text());
        const int percent = std::stoi(text);
        require(text == std::to_string(percent) + "%", "zoom readout must be a device-pixel percentage");
        return percent;
    };
    const auto wheel = [&](glm::vec2 at, float delta, int steps = 1) {
        require(vgui_hit_test(shell, at.x, at.y) == canvas(), "wheel does not hit canvas");
        input.motion(at, at, 0);
        for (int i = 0; i < steps; ++i) {
            SDL_Event e{};
            e.type = SDL_EVENT_MOUSE_WHEEL;
            e.wheel.windowID = win.id;
            e.wheel.y = delta;
            e.wheel.mouse_x = at.x / win.pixel_ratio();
            e.wheel.mouse_y = at.y / win.pixel_ratio();
            input.push(e);
        }
        frame();
    };
    open_choice_region("check");
    draft_is({0, 0, 273, 139}, false, "odd-sized sheet must require a four-state subregion");
    require(!vgui_widget_get_enabled(modal_widget("designer.icon_region.whole", VGUI_BUTTON)),
        "odd-sized sheet must require a four-state subregion");
    constexpr int atlas_w = 273, atlas_h = 139;
    const auto r = input.rect(canvas());
    const auto fitted = fit_scale(r, atlas_w, atlas_h);
    const int fit_percent = int(std::round(fitted * 100));
    require(zoom_text() == fit_text, "picker did not open in fit mode");
    input.click(modal_widget(zoom_ids[1], VGUI_BUTTON));
    const int increased = zoom();
    require(increased > fit_percent, "zoom in button did not increase magnification");
    input.click(modal_widget(zoom_ids[0], VGUI_BUTTON));
    require(zoom() < increased, "zoom out button did not decrease magnification");
    input.click(modal_widget(zoom_ids[2], VGUI_BUTTON));

    // Offset the cursor along the fit-limited axis, where zoom can preserve the anchor.
    // Center the other axis in case the first step still leaves a letterbox there.
    const glm::vec2 source_anchor = r.width / atlas_w <= r.height / atlas_h ?
        glm::vec2{103, 69} : glm::vec2{136, 53};
    const auto anchor = centered_pixel(r, atlas_w, atlas_h, fitted, source_anchor);
    wheel(anchor, 1);
    require(std::abs(zoom() - fitted * 125) <= 2, "wheel did not zoom one approximately 1.25× step");
    wheel(anchor, 1, 2);
    input.click_at(canvas(), anchor);
    const int ax = int(source_anchor.x), ay = int(source_anchor.y);
    draft_is({ax, ay, 1, 1}, false, "wheel zoom did not keep the original source pixel under the cursor");

    // Small drag at this zoom stays away from the clamp; test the hit mapping, not internal pan state.
    const auto shifted = anchor + glm::vec2{29, -23};
    drag_at(anchor, shifted, SDL_BUTTON_MIDDLE, SDL_BUTTON_MMASK);
    draft_is({ax, ay, 1, 1}, false, "middle drag changed the selection");
    const auto scale = zoom() / 100.f;
    const auto to = shifted + glm::vec2{std::round(3 * scale), std::round(5 * scale)};
    drag_at(shifted, to, SDL_BUTTON_LEFT, SDL_BUTTON_LMASK);
    draft_is({ax, ay, 4, 6}, true, "drag after zoom and pan did not select the expected source pixels");
    require(vgui_get_focus(shell) == canvas(), "selection drag did not focus the canvas");
    menus.key(SDLK_RIGHT, SDL_SCANCODE_RIGHT);
    menus.key(SDLK_DOWN, SDL_SCANCODE_DOWN);
    draft_is({ax + 1, ay + 1, 4, 6}, true, "canvas arrows must move one original pixel without resizing");

    wheel(anchor, 1, 24);
    require(zoom() == 1600, "zoom did not stop at 16:1");
    wheel(anchor, 1);
    require(zoom() == 1600, "wheel changed the zoom at its maximum");
    wheel(anchor, -1, 24);
    require(zoom() == std::min(fit_percent, 100), "wheel did not stop at the minimum zoom");
    wheel(anchor, -1);
    require(zoom() == std::min(fit_percent, 100), "wheel changed the zoom at its minimum");
    input.click(modal_widget(zoom_ids[3], VGUI_BUTTON));
    require(zoom() == 100, "actual size must be exactly one device pixel per source pixel");
    draft_is({ax + 1, ay + 1, 4, 6}, true, "zoom navigation changed the draft");
    // SDL mouse coordinates are rounded to whole device pixels. At 1:1 choose
    // the integer inside each source pixel, including when its left/top edge is integral.
    const glm::vec2 actual_origin{std::ceil(r.x + (r.width - atlas_w) / 2),
        std::ceil(r.y + (r.height - atlas_h) / 2)};
    input.click_at(canvas(), actual_origin + glm::vec2{81, 51});
    draft_is({81, 51, 1, 1}, false, "1:1 selection did not map to the original image pixels");
    input.click(modal_widget(zoom_ids[2], VGUI_BUTTON));
    require(zoom_text() == fit_text, "fit did not restore automatic magnification");
    drag_pixels(273, 139, {266, 132}, {140, 5});
    draft_is({140, 5, 127, 128}, false, "fit did not reset the pan, or odd width was accepted");
    input.click(field(2));
    menus.key(SDLK_UP, SDL_SCANCODE_UP);
    draft_is({140, 5, 128, 128}, true, "Width Up did not recover an odd crop by adding one pixel");
    replace_text(0, "");
    require(!vgui_widget_get_enabled(ok()), "empty X was accepted");
    replace_text(0, "bad");
    require(!vgui_widget_get_enabled(ok()), "noninteger X was accepted");
    replace_text(0, "146");
    require(!vgui_widget_get_enabled(ok()), "an otherwise valid crop extending past the atlas was accepted");
    replace_text(0, "141");
    draft_is({141, 5, 128, 128}, true, "typing a valid X did not immediately recover the draft");
    replace_text(1, "7");
    draft_is({141, 7, 128, 128}, true, "typing Y did not immediately update the draft");
    menus.key(SDLK_DOWN, SDL_SCANCODE_DOWN);
    draft_is({141, 6, 128, 128}, true, "Y Down did not subtract one pixel");
    replace_text(2, "126");
    draft_is({141, 6, 126, 128}, true, "typing Width did not immediately update the draft");
    replace_text(3, "127");
    draft_is({141, 6, 126, 127}, false, "typing an odd Height must disable confirmation");
    menus.key(SDLK_UP, SDL_SCANCODE_UP);
    draft_is({141, 6, 126, 128}, true, "Height Up did not recover an odd crop by adding one pixel");
    require(history.current() == doc && history.revision() == unchanged && !history.can_undo(),
        "zoom, pan, selection, or numeric edits mutated the document before confirmation");
    input.click(ok());
    expected = doc;
    const auto set_indicator_region = [&](const char* id, ui::icon_source_rect region) {
        for (std::size_t i = 0; i < region.size(); ++i)
            expected.nodes.at(id).props[ui::indicator_source_fields[i]] = double(region[i]);
    };
    set_indicator_region("check", {141, 6, 126, 128});
    require(history.current() == expected && history.revision() == unchanged + 1, "checkbox confirmation was not one isolated edit");
    menus.activate(menus.open(1), 0);
    require(history.current() == doc && !history.can_undo(), "one undo did not restore the checkbox's whole-image default");
    menus.activate(menus.open(1), 1);
    require(history.current() == expected, "redo did not reapply the checkbox crop");
    open_choice_region("radio");
    require(coords().starts_with("X: 5     Y: 5     128 × 128 px") && vgui_widget_get_enabled(ok()),
        "radio picker mixed the radio and checkbox selections");
    const auto before_cancel = history.revision();
    replace_text(0, "9");
    draft_is({9, 5, 128, 128}, true, "radio field edit did not update its draft");
    input.click(modal_widget(zoom_ids[1], VGUI_BUTTON));
    replace_text(3, "");
    require(!vgui_widget_get_enabled(ok()), "empty radio draft was accepted");
    menus.key(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE);
    require(!vgui_modal_active(shell) && history.current() == expected && history.revision() == before_cancel,
        "cancel changed the radio selection or history");
    open_choice_region("radio");
    drag_pixels(273, 139, {9, 9}, {128, 128});
    require(coords().starts_with("X: 9     Y: 9     120 × 120 px") && vgui_widget_get_enabled(ok()), "radio could not select its own region");
    input.click(ok());
    set_indicator_region("radio", {9, 9, 120, 120});
    require(history.current() == expected, "radio confirmation affected the other crop or resource");
    editor.request(command::save);
    frame();
    ui::document saved;
    require(!history.dirty() && ui::read_document(file, saved) && saved == expected, "saving lost the shared atlas or either region");
    editor.request(command::close);
    frame();
    require(editor.open(file), "failed to reopen the shared-atlas page");
    frame();
    require(history.current() == expected, "reopening did not restore both independent regions");
    editor.request(command::close);
    frame();
}

void exercise_control_drag(os_window& win, vgui_context* shell, designer& editor,
                           const std::function<void()>& frame, const fs::path& sandbox, const fs::path& other_page) {
    const auto require = [](bool passed, std::string_view message) {
        if (!passed) throw std::runtime_error("Native control palette: " + std::string(message));
    };
    auto fixture = ui::make_document();
    fixture.nodes.at(fixture.root).children = {"padded", "flow"};
    fixture.nodes.emplace("padded", ui::node{"panel", {}, {{"x", 90.}, {"y", 80.}, {"width", 430.}, {"height", 280.},
        {"pad_left", 18.}, {"pad_top", 12.}}, {"leaf"}});
    fixture.nodes.emplace("leaf", ui::node{"label", {}, {{"x", 16.}, {"y", 10.}, {"width", 140.}, {"height", 48.},
        {"text", std::string("Drop here")}}, {}});
    fixture.nodes.emplace("flow", ui::node{"panel", {}, {{"x", 540.}, {"y", 80.}, {"width", 390.}, {"height", 280.},
        {"layout", std::string("row")}}, {}});
    const auto file = sandbox / "palette-drop.vgui.json";
    require(ui::write_document(file, fixture) && editor.open(file), "load the nested layout fixture");
    frame();
    auto& preview = editor.preview();
    auto view = preview.capture_view();
    view.fit = false;
    view.zoom = .66f;
    view.pan_x = 21;
    view.pan_y = -17;
    preview.restore_view(view);
    frame();
    const native_keyframe_input input{win, shell, preview, frame, require};
    auto* dock = find_widget(vgui_root(shell), "designer.dock", VGUI_DOCK);
    auto* list = find_widget(vgui_root(shell), "designer.controls", VGUI_LIST);
    require(dock && list && vgui_list_get_count(list) == static_cast<int>(ui::types().size()), "missing built-in controls list");
    const auto click_tab = [&](const char* id) {
        ui::pixel_rect r;
        const auto tab = vgui_dock_find(dock, id);
        require(vgui_dock_get_tab_rect(dock, tab, &r.x, &r.y, &r.width, &r.height), "left dock tab is not visible");
        input.click_at(dock, {r.x + r.width / 2, r.y + r.height / 2});
        require(vgui_dock_get_active(dock, VGUI_DOCK_LEFT) == tab, "did not activate the left dock tab");
    };
    click_tab("controls");
    auto* canvas = input.widget("designer.preview.canvas");
    const auto point_on = [&](glm::vec2 logical) {
        const auto board = preview.artboard_rect_px();
        const auto* layout = preview.snapshot();
        require(board && layout, "artboard must be presented");
        return glm::round(glm::vec2{board->x, board->y} + logical *
            glm::vec2{board->width / layout->pixel_width, board->height / layout->pixel_height});
    };
    const auto inside_leaf = point_on({90 + 18 + 45, 80 + 12 + 32});
    const auto inside_flow = point_on({650, 145});
    const auto& history = editor.history();
    const auto row = input.rect(list);
    const glm::vec2 icon_point{row.x + 14, row.y + 12};
    const auto panel_item = vgui_list_get_item_at(list, 0);
    require(vgui_list_get_item_icon(list, panel_item), "first palette row has no emoji icon");
    const auto before_click = history.revision();
    input.click_at(list, icon_point);
    require(vgui_list_is_selected(list, panel_item), "clicking the icon did not select the Panel row");
    vgui_list_clear_selection(list);
    input.click_at(list, {row.x + 42, icon_point.y});
    require(vgui_list_is_selected(list, panel_item) && history.revision() == before_click,
        "clicking the label must select the same row without creating a control");
    const auto begin_drag = [&](glm::vec2 target) {
        // Start on the icon (not on the text) to exercise the whole-row drag.
        const auto source = icon_point;
        require(vgui_hit_test(shell, source.x, source.y) == list, "palette icon not hit-testable");
        // Move back to the source, like a real pointer, before starting the next drag.
        input.motion(target, source, 0);
        input.button(true, source);
        input.motion(source, target);
        frame();
        require(vgui_drag_active(shell), preview.running() ? "palette drag did not start in Run" : "palette drag did not start in Design");
    };
    const auto drag = [&](glm::vec2 target, bool valid, bool cancel = false) {
        const auto revision = history.revision();
        begin_drag(target);
        vgui_drop_hint hint{};
        require((vgui_drag_hint(shell, &hint) != 0) == valid, valid ? "accepted drop has no drag hint" : "rejected drop has a drag hint");
        require(!valid || (hint.kind == VGUI_DROP_HINT_RECT && hint.w > 0 && hint.h > 0), "drag hint has no visible bounds");
        require(history.revision() == revision && history.current() == fixture, "hover added a control before release");
        if (cancel) ui::native_menu_input(win, shell, frame).key(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE);
        input.button(false, target);
        frame();
        require(!vgui_drag_active(shell), "release left the drag active");
        require(history.revision() == revision + (valid && !cancel ? 1 : 0), "drop did not create exactly the expected undo entry");
        if (!valid || cancel) require(history.current() == fixture, "cancelled or invalid drop changed the page");
    };
    require(vgui_hit_test(shell, inside_leaf.x, inside_leaf.y) == canvas, "fixture drop does not hit the canvas");
    drag(inside_leaf, true, true);
    const auto toolbar = input.rect(input.widget("designer.preview.toolbar"));
    drag({toolbar.x + toolbar.width / 2, toolbar.y + toolbar.height / 2}, false);
    input.click("designer.preview.mode");
    drag(inside_leaf, false);
    input.click("designer.preview.mode");
    drag(inside_leaf, true);
    const auto verify_insert = [&](const char* parent, bool absolute, const char* kind = "panel") {
        const std::string id(editor.selection());
        const auto& actual_parent = history.current().nodes.at(parent);
        require(!actual_parent.children.empty() && actual_parent.children.back() == id, "drop resolved to the wrong container");
        const auto& added = history.current().nodes.at(id);
        const auto* type = ui::find_type(kind);
        require(added.type == type->key && std::get<double>(added.props.at("width")) == type->initial_width &&
            std::get<double>(added.props.at("height")) == type->initial_height, "drop did not use the menu's default control size");
        if (absolute) {
            require(std::abs(std::get<double>(added.props.at("x")) - 45.) <= 1. &&
                std::abs(std::get<double>(added.props.at("y")) - 32.) <= 1., "drop lost zoom, pan or parent padding");
        } else {
            require(!added.props.contains("x") && !added.props.contains("y"), "auto-layout drop wrote absolute coordinates");
        }
        auto expected = fixture;
        expected.nodes.at(parent).children.push_back(id);
        expected.nodes.emplace(id, added);
        require(history.current() == expected && history.can_undo() && history.dirty(), "drop altered unrelated nodes or history");
        const ui::native_menu_input menus(win, shell, frame);
        menus.activate(menus.open(1), 0); // Edit > Undo
        require(history.current() == fixture && !history.can_undo() && history.can_redo() && !history.dirty(),
            "one undo did not remove the newly created control");
    };
    verify_insert("padded", true);
    drag(inside_flow, true);
    verify_insert("flow", false);

    auto* search = find_widget(vgui_root(shell), "designer.controls.search", VGUI_EDIT);
    require(search, "missing palette search field");
    input.click(search);
    SDL_Event text{};
    text.type = SDL_EVENT_TEXT_INPUT;
    text.text.windowID = win.id;
    text.text.text = "复选";
    input.push(text);
    frame(); // Filter before any Enter or blur.
    require(vgui_list_get_count(list) == 1 && vgui_list_get_item_icon(list, vgui_list_get_item_at(list, 0)),
        "Chinese search did not show one result with its icon");
    const ui::native_menu_input keys(win, shell, frame);
    const auto before_keys = history.revision();
    keys.key(SDLK_DOWN, SDL_SCANCODE_DOWN);
    require(vgui_get_focus(shell) == list && vgui_list_is_selected(list, vgui_list_get_item_at(list, 0)) &&
        history.revision() == before_keys, "Down did not focus and select the search result without adding it");
    keys.key(SDLK_RETURN, SDL_SCANCODE_RETURN);
    require(history.revision() == before_keys + 1, "Enter did not create one undoable control");
    verify_insert(fixture.root.c_str(), false, "checkbox");

    drag(inside_flow, true);
    verify_insert("flow", false, "checkbox");
    input.click(search);
    keys.key(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE);
    require(vgui_list_get_count(list) == static_cast<int>(ui::types().size()), "clearing search did not restore all rows");

    begin_drag(inside_leaf);
    require(editor.open(other_page), "switch pages while a palette drag is active");
    frame();
    require(!vgui_drag_active(shell), "page switch retained the palette drag");
    const auto other_doc = history.current();
    const auto other_revision = history.revision();
    input.button(false, inside_leaf);
    frame();
    require(history.current() == other_doc && history.revision() == other_revision, "old release edited the new page");
    require(editor.open(file) && history.current() == fixture, "page switch edited the drag's source document");
    frame();
    click_tab("structure");
    editor.request(command::close); // Do not leave this scratch tab in the multi-page fixture.
    frame();
}

void exercise_view_tools(app_mgr& app, os_window& win, vgui_context* shell, designer& editor,
                         const std::function<void()>& frame, const fs::path& sandbox, const fs::path& other) {
    const auto require = [](bool passed, std::string_view message) {
        if (!passed) throw std::runtime_error("Native view tools: " + std::string(message));
    };
    constexpr auto pointer_id = "designer.preview.pointer", hand_id = "designer.preview.hand";
    const auto expect_tool = [&](bool hand) {
        auto& preview = editor.preview();
        auto* pointer = find_widget(preview.widget(), pointer_id, VGUI_BUTTON);
        auto* grip = find_widget(preview.widget(), hand_id, VGUI_BUTTON);
        require(pointer && grip, "missing Pointer or Hand tool");
        for (auto* button : {pointer, grip}) {
            auto* glyph = find_widget(vgui_widget_get_parent(button), "designer.preview.tool.glyph", VGUI_CUSTOM);
            require(glyph && std::string_view(vgui_widget_get_text(button)).empty(), "tool must use a vector glyph without button text");
            const auto b = native_keyframe_input::rect(button), g = native_keyframe_input::rect(glyph);
            require(b.width > 0 && b.height > 0 && b.x == g.x && b.y == g.y && b.width == g.width && b.height == g.height,
                "vector glyph did not cover the native button");
            require(vgui_hit_test(shell, b.x + b.width / 2, b.y + b.height / 2) == button,
                "vector decoration must not intercept input");
        }
        // Ordinary VGUI buttons tint the checked state even when not hovered.
        require(preview.capture_view().hand_tool == hand && (vgui_widget_get_checked(pointer) != 0) == !hand &&
            (vgui_widget_get_checked(grip) != 0) == hand, "toolbar does not show the active canvas tool");
    };

    auto doc = ui::make_document();
    doc.nodes.at(doc.root).children = {"trigger", "entry"};
    doc.nodes.emplace("trigger", ui::node{"button", {}, {{"x", 510.}, {"y", 276.}, {"width", 230.}, {"height", 68.},
        {"text", std::string("Trigger")}, {"action", std::string("view.trigger")}}, {}});
    doc.nodes.emplace("entry", ui::node{"edit", {}, {{"x", 510.}, {"y", 376.}, {"width", 230.}, {"height", 52.}}, {}});
    const auto file = sandbox / "preview-view-tools.vgui.json";
    require(ui::write_document(file, doc) && editor.open(file), "open the view tools fixture");
    frame();
    auto& preview = editor.preview();
    const native_keyframe_input input{win, shell, preview, frame, require};
    auto* canvas = input.widget("designer.preview.canvas");
    auto* status = find_widget(vgui_root(shell), "designer.status");
    require(status != nullptr, "missing workbench status");
    const auto revision = editor.history().revision();
    const auto unchanged_document = [&] {
        const auto& history = editor.history();
        require(history.revision() == revision && history.current() == doc &&
            !history.dirty() && !history.can_undo() && !history.can_redo(), "canvas tools changed the document or its history");
    };
    const auto board = [&] {
        const auto r = preview.artboard_rect_px();
        require(r.has_value(), "artboard must be presented");
        return *r;
    };
    const auto center = [&](std::string_view id) {
        const auto r = board();
        const auto* layout = preview.snapshot();
        require(layout && layout->pixel_width > 0 && layout->pixel_height > 0, "missing presented layout");
        const auto node = std::ranges::find(layout->nodes, id, &ui::node_layout::id);
        require(node != layout->nodes.end() && node->visible, "fixture object is not visible");
        const auto& bounds = node->visible_px;
        const glm::vec2 p = glm::round(glm::vec2{r.x + (bounds.x + bounds.width / 2) * r.width / layout->pixel_width,
            r.y + (bounds.y + bounds.height / 2) * r.height / layout->pixel_height});
        require(vgui_hit_test(shell, p.x, p.y) == canvas, "fixture object center is not on the canvas");
        return p;
    };
    const auto expect_pan = [&](const ui::designer_preview::view_state& view, ui::pixel_rect r, glm::vec2 shift) {
        const auto now = preview.capture_view();
        const auto b = board();
        const auto scale = vgui_context_get_ui_scale(shell);
        require(std::abs(now.pan_x - view.pan_x - shift.x / scale) < .01f &&
            std::abs(now.pan_y - view.pan_y - shift.y / scale) < .01f &&
            std::abs(b.x - r.x - shift.x) <= 1.f && std::abs(b.y - r.y - shift.y) <= 1.f &&
            b.width == r.width && b.height == r.height && now.fit == view.fit && now.zoom == view.zoom,
            "drag did not just translate the artboard by the SDL pointer delta");
    };

    require(!preview.running(), "expected to start in Design");
    expect_tool(false);
    input.click_at(canvas, center("entry"));
    require(editor.selection() == "entry", "default Pointer did not select the object");
    input.click(hand_id);
    expect_tool(true);
    const auto bar = input.rect(input.widget("designer.preview.status"));
    const glm::vec2 outside = glm::round(glm::vec2{bar.x + bar.width / 2, bar.y + bar.height / 2});
    const auto hover = [&](glm::vec2 from, glm::vec2 to) {
        input.motion(from, to, 0);
        frame();
    };
    const auto cursor_is = [&](SDL_SystemCursor shape, const char* why) {
        require(app.system_cursor[shape] && SDL_GetCursor() == app.system_cursor[shape], why);
    };
    const auto over = center("trigger");
    hover(over, outside);
    cursor_is(SDL_SYSTEM_CURSOR_DEFAULT, "outside the canvas must use the normal cursor");
    hover(outside, over);
    cursor_is(SDL_SYSTEM_CURSOR_POINTER, "re-entering the canvas did not restore the Hand cursor");
    const auto initial_view = preview.capture_view();
    const auto initial_board = board();
    const auto delta = glm::round(glm::vec2{initial_board.width / 24, -initial_board.height / 24});
    const auto start = center("trigger");
    hover(over, outside);
    input.button(true, start);
    frame();
    cursor_is(SDL_SYSTEM_CURSOR_POINTER, "starting a Hand drag did not set the Hand cursor");
    app.set_defcursor(cursor_st::cursor_arrow); // Simulate the cursor being replaced while capture is active.
    input.motion(start, start + delta);
    frame();
    cursor_is(SDL_SYSTEM_CURSOR_POINTER, "captured Hand motion did not restore the Hand cursor");
    expect_pan(initial_view, initial_board, delta);
    require(editor.selection() == "entry", "Design Hand drag selected the object under it");
    unchanged_document();
    input.button(false, start + delta);
    input.motion(start + delta, start, 0);
    frame();
    expect_pan(initial_view, initial_board, delta);
    expect_tool(true);

    input.click_at(canvas, center("trigger"), SDL_BUTTON_RIGHT);
    expect_tool(false);
    cursor_is(SDL_SYSTEM_CURSOR_DEFAULT, "right-click leaving Hand did not restore the normal cursor");
    require(editor.selection() == "entry", "right click leaving Hand selected the object");
    const auto middle = center("trigger");
    input.button(true, middle, SDL_BUTTON_MIDDLE);
    input.motion(middle, middle - delta, SDL_BUTTON_MMASK);
    input.button(false, middle - delta, SDL_BUTTON_MIDDLE);
    frame();
    expect_pan(initial_view, initial_board, {});
    require(editor.selection() == "entry", "middle-button pan selected the object");
    input.click_at(canvas, center("trigger"));
    require(editor.selection() == "trigger", "click after leaving Hand did not restore Design selection");
    unchanged_document();

    input.click("designer.preview.mode");
    require(preview.running(), "toolbar did not enter Run mode");
    input.click(hand_id);
    expect_tool(true);
    const std::string quiet = vgui_widget_get_text(status);
    const auto no_runtime_input = [&] {
        require(quiet == vgui_widget_get_text(status) && !SDL_TextInputActive(win.window) &&
            vgui_get_focus(shell) != canvas, "Hand input reached a runtime button or edit");
    };
    input.click_at(canvas, center("trigger"));
    input.click_at(canvas, center("entry"));
    no_runtime_input();
    const auto run_view = preview.capture_view();
    const auto run_board = board();
    const auto at = center("trigger");
    input.button(true, at);
    input.motion(at, at + delta);
    frame();
    expect_pan(run_view, run_board, delta);
    no_runtime_input();

    input.button(true, at + delta, SDL_BUTTON_RIGHT);
    frame();
    expect_tool(false);
    cursor_is(SDL_SYSTEM_CURSOR_DEFAULT, "right-click cancel during a Hand drag left the cursor stuck");
    input.motion(at + delta, at, SDL_BUTTON_LMASK | SDL_BUTTON_RMASK);
    input.button(false, at, SDL_BUTTON_RIGHT);
    input.button(false, at);
    frame();
    expect_pan(run_view, run_board, delta);
    no_runtime_input();
    input.click_at(canvas, center("entry"));
    require(SDL_TextInputActive(win.window), "right-click exit did not restore Run edit focus");

    input.click(hand_id);
    expect_tool(true);
    input.click(pointer_id);
    expect_tool(false);
    const auto trigger = center("trigger");
    input.button(true, trigger);
    frame();
    require(quiet == vgui_widget_get_text(status), "runtime button activated on press instead of release");
    input.button(false, trigger);
    frame();
    require(std::string_view(vgui_widget_get_text(status)).ends_with("view.trigger [trigger]"),
        "Pointer did not restore runtime button action on release");
    unchanged_document();

    input.click(hand_id);
    require(editor.open(other), "activate the other page");
    frame();
    expect_tool(false);
    require(editor.open(file), "return to the Hand fixture");
    frame();
    expect_tool(true);
    editor.request(command::close);
    frame();
}

void exercise_custom_viewport(os_window& win, vgui_context* shell, designer& editor,
                              const std::function<void()>& frame, const fs::path& sandbox, const fs::path& other) {
    const auto require = [](bool passed, std::string_view message) {
        if (!passed) throw std::runtime_error("Native custom viewport: " + std::string(message));
    };
    auto doc = ui::make_document();
    doc.nodes.at(doc.root).children = {"message", "bar"};
    doc.nodes.emplace("message", ui::node{"edit", {}, {{"x", 12.}, {"y", -10.}, {"width", 140.}, {"height", 32.},
        {"anchor_y", std::string("end")}, {"anchor_safe_area", true}}, {}});
    doc.nodes.emplace("bar", ui::node{"panel", {}, {{"x", 16.}, {"y", 40.}, {"width", 80.}, {"height", 48.},
        {"anchor_x", std::string("stretch")}, {"anchor_right", 30.}, {"anchor_safe_area", true}}, {}});
    const auto file = sandbox / "custom-viewport.vgui.json";
    require(ui::write_document(file, doc) && editor.open(file), "open the anchored fixture");
    frame();
    auto& preview = editor.preview();
    const native_keyframe_input input{win, shell, preview, frame, require};
    const auto prefix = std::string("designer.preview.viewport");
    const auto modal_widget = [&](const char* suffix, vgui_widget_kind kind) {
        const auto id = prefix + suffix;
        auto* w = find_widget(vgui_modal_panel(shell), id, kind);
        require(w != nullptr, id);
        return w;
    };
    const auto edit = [&](const char* suffix, const char* value) {
        auto* w = modal_widget(suffix, VGUI_EDIT);
        input.click(w);
        vgui_edit_select_all(w);
        SDL_Event text{};
        text.type = SDL_EVENT_TEXT_INPUT;
        text.text.windowID = win.id;
        text.text.text = value;
        input.push(text);
        frame();
    };
    const auto rev = editor.history().revision();
    input.click(prefix.c_str());
    require(vgui_modal_active(shell), "viewport controls did not open");
    edit(".width", "640");
    edit(".height", "320");
    edit(".scale", "1.5");
    edit(".left", "44");
    edit(".bottom", "30");
    edit(".right", "640");
    input.click(modal_widget(".ok", VGUI_BUTTON));
    require(vgui_modal_active(shell) &&
        *vgui_widget_get_text(modal_widget(".error", VGUI_LABEL)), "invalid opposing insets were applied instead of diagnosed");
    edit(".right", "18");
    require(!preview.capture_view().custom_viewport, "draft viewport changed before confirmation");
    input.click(modal_widget(".ok", VGUI_BUTTON));
    require(!vgui_modal_active(shell), "valid viewport was not applied");
    auto view = preview.capture_view();
    const auto* layout = preview.snapshot();
    require(editor.history().revision() == rev && view.custom_viewport && view.safe_area.left == 44 && view.safe_area.bottom == 30 &&
        layout && layout->pixel_width == 960 && layout->pixel_height == 480, "DPI or safe area did not reach the preview");
    const auto anchored = std::ranges::find(layout->nodes, "message", &ui::node_layout::id);
    require(anchored != layout->nodes.end() && anchored->visible, "anchor was not laid out");
    const auto rect = anchored->rect_px;
    require(rect.x == 84 && rect.y == 372 && rect.width == 210 && rect.height == 48,
        "preview does not use the safe-area anchor formula in physical pixels");

    // Resize a stretched axis through SDL. The opposite edge stays fixed,
    // and the authored width is retained for later use in other layout modes.
    const auto bounds = preview.artboard_rect_px();
    const auto bar = std::ranges::find(layout->nodes, "bar", &ui::node_layout::id);
    require(bounds && bar != layout->nodes.end() && bar->visible, "missing stretched bar");
    const auto project = [&](float x, float y) {
        return glm::round(glm::vec2{bounds->x + x * bounds->width / 960.f, bounds->y + y * bounds->height / 480.f});
    };
    const auto original_bar = bar->rect_px;
    auto* canvas = input.widget("designer.preview.canvas");
    input.click_at(canvas, project(original_bar.x + original_bar.width / 2, original_bar.y + original_bar.height / 2));
    require(editor.selection() == "bar", "stretched bar could not be selected");
    const auto end = project(original_bar.x + original_bar.width, original_bar.y + original_bar.height);
    const auto moved = end - glm::vec2{24.f, 0.f};
    input.button(true, end);
    input.motion(end, moved);
    input.button(false, moved);
    frame();
    const auto& props = editor.history().current().nodes.at("bar").props;
    require(std::get<double>(props.at("anchor_right")) > 30. && std::get<double>(props.at("width")) == 80. &&
        std::get<double>(props.at("x")) == 16., "trailing resize did not update only the trailing inset");
    const auto* resized = preview.snapshot();
    require(resized != nullptr, "resize lost its preview");
    const auto current_bar = std::ranges::find(resized->nodes, "bar", &ui::node_layout::id);
    require(current_bar != resized->nodes.end() && current_bar->rect_px.x == original_bar.x &&
        current_bar->rect_px.width < original_bar.width, "stretched resize moved the opposite edge or did not resize");
    const ui::native_menu_input menus(win, shell, frame);
    menus.activate(menus.open(1), 0);
    require(!editor.history().can_undo() && editor.history().current() == doc, "stretched resize did not undo in one step");
    const auto after_undo = editor.history().revision();
    auto* view_menu = menus.open(3);
    for (int index : {2, 3, 4})
        require(!vgui_menu_get_item_checked(view_menu, vgui_menu_get_item_at(view_menu, index)),
            "the View menu marked an inactive preset as selected");
    menus.key(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE);

    input.click("designer.preview.mode");
    layout = preview.snapshot();
    const auto board = preview.artboard_rect_px();
    require(preview.running() && layout && board, "cannot run the custom viewport");
    const glm::vec2 point{board->x + (rect.x + rect.width / 2) * board->width / layout->pixel_width,
        board->y + (rect.y + rect.height / 2) * board->height / layout->pixel_height};
    input.click_at(input.widget("designer.preview.canvas"), glm::round(point));
    SDL_Event text{};
    text.type = SDL_EVENT_TEXT_INPUT;
    text.text.windowID = win.id;
    text.text.text = "Live";
    input.push(text);
    frame();
    require(preview.edit_text("message") == "Live", "DPI-scaled canvas did not route input to the anchored edit");

    input.click(prefix.c_str());
    edit(".width", "123");
    input.click(modal_widget(".cancel", VGUI_BUTTON));
    require(!vgui_modal_active(shell) && preview.capture_view().custom_viewport->width == 640 &&
        editor.history().revision() == after_undo && editor.history().current() == doc, "viewport changed on cancel or wrote into the document");
    require(editor.open(other) && !editor.preview().capture_view().custom_viewport, "the viewport leaked to another page");
    require(editor.open(file) && editor.preview().capture_view().custom_viewport &&
        editor.preview().capture_view().safe_area.left == 44, "the custom page viewport was not restored");
    frame();
    input.click("designer.preview.desktop");
    require(!preview.capture_view().custom_viewport && preview.capture_view().safe_area.left == 0 &&
        preview.snapshot() && preview.snapshot()->view.scale == 1, "preset did not reset DPI and safe area");
    editor.request(command::close);
    frame();
}

void exercise_game_data(os_window& win, vgui_context* shell, designer& editor,
                        const std::function<void()>& frame, const std::function<void(float)>& tick_preview,
                        const fs::path& sandbox) {
    const auto require = [](bool passed, std::string_view message) {
        if (!passed) throw std::runtime_error("Native game data: " + std::string(message));
    };
    ui::document doc;
    require(ui::decode_json(R"({"format":"vgui-ui-design","version":1,"root":"root","nodes":{
        "root":{"type":"panel","children":["caption","meter","button","group"]},
        "caption":{"type":"label","props":{"x":20,"y":20,"width":280,"height":32,"text":"Design",
            "bind_text":"hud.caption","bind_visible":"hud.ready"}},
        "meter":{"type":"progress","props":{"x":20,"y":64,"width":180,"height":22,"value":0.5,"step":0.5,"bind_value":"hud.hp"}},
        "button":{"type":"button","props":{"x":20,"y":96,"width":98,"height":32,"text":"Ready","bind_enabled":"hud.ready"}},
        "group":{"type":"panel","props":{"y":140,"width":220,"height":40},"children":["mirror"]},
        "mirror":{"type":"progress","props":{"x":20,"width":180,"height":22,"max_value":0.125,"value":0.0625,
            "bind_value":"hud.hp","bind_value_transition_ms":1000}}
    }})", doc), "decode the data binding fixture");
    const auto source = sandbox / "game-data-source.vgui.json", second = sandbox / "game-data-second.vgui.json";
    require(ui::write_document(source, doc) && ui::write_document(second, doc) && editor.open(source), "open fixture");
    frame();
    auto& preview = editor.preview();
    const native_keyframe_input input{win, shell, preview, frame, require};
    const ui::native_menu_input keys(win, shell, frame);
    constexpr auto mode = "designer.preview.mode", simulate = "designer.preview.simulation";
    const auto revision = editor.history().revision();
    const auto field = [&](const char* suffix, vgui_widget_kind kind) {
        const auto id = std::string(simulate) + suffix;
        auto* w = find_widget(vgui_modal_panel(shell), id, kind);
        require(w && vgui_widget_get_visible(w), id);
        return w;
    };
    const auto click = [&](const char* suffix, vgui_widget_kind kind = VGUI_BUTTON) { input.click(field(suffix, kind)); };
    const auto type = [&](const char* value, const char* suffix = ".value") {
        auto* edit = field(suffix, VGUI_EDIT);
        input.click(edit);
        vgui_edit_select_all(edit);
        SDL_Event text{};
        text.type = SDL_EVENT_TEXT_INPUT;
        text.text.windowID = win.id;
        text.text.text = value;
        input.push(text);
        frame();
    };
    const auto read = [&](const char* suffix) {
        return std::string(vgui_edit_get_text(field(suffix, VGUI_EDIT)));
    };
    const auto first_row = [&](vgui_widget* list) {
        const auto r = input.rect(list);
        input.click_at(list, {r.x + r.width * .5f, r.y + 12 * vgui_context_get_ui_scale(shell)});
    };
    const auto state = [&](std::string_view id) -> ui::node_layout {
        const auto* snapshot = preview.snapshot();
        require(snapshot != nullptr, "no canvas snapshot");
        const auto it = std::ranges::find(snapshot->nodes, id, &ui::node_layout::id);
        require(it != snapshot->nodes.end(), "bound target is absent from layout");
        return *it;
    };
    input.click(mode);
    require(preview.running() && preview.node_text("caption") == "Design" && state("caption").visible,
        "an unpublished binding did not keep its design value");
    input.click(simulate);
    auto* list = field(".fields", VGUI_LIST);
    require(vgui_list_get_count(list) == 3 &&
        vgui_list_is_selected(list, vgui_list_get_item_at(list, 0)), "dialog did not list distinct field keys in order");
    click(".override", VGUI_CHECKBOX);
    type("Pilot ready");
    require(preview.node_text("caption") == "Design", "editing a draft updated the canvas before confirmation");
    click(".debug_tab");
    const auto label = [&](const char* suffix) { return std::string(vgui_widget_get_text(field(suffix, VGUI_LABEL))); };
    require(label(".source").ends_with(editor.strings().text("preview.simulation.source_design", "Design values")) &&
        read(".published") == "—" && read(".authored") == "\"Design\"" && read(".effective") == "\"Design\"",
        "field debug showed the unapplied text instead of the design value");
    click(".edit_tab");
    require(read(".value") == "Pilot ready", "switching back from field debug lost the draft");
    click(".ok");
    require(!vgui_modal_active(shell) && preview.node_text("caption") == "Pilot ready", "confirmation did not set live text");

    input.click(simulate);
    list = field(".fields", VGUI_LIST);
    first_row(list);
    keys.key(SDLK_DOWN, SDL_SCANCODE_DOWN);
    require(vgui_list_is_selected(list, vgui_list_get_item_at(list, 1)), "could not select the numeric field");
    click(".override", VGUI_CHECKBOX);
    type("invalid");
    click(".ok");
    require(vgui_modal_active(shell) && *vgui_widget_get_text(field(".error", VGUI_LABEL)), "invalid numeric data was applied");
    type("0.375");
    first_row(list);
    keys.key(SDLK_END, SDL_SCANCODE_END);
    require(vgui_list_is_selected(list, vgui_list_get_item_at(list, 2)), "could not select the boolean field");
    click(".override", VGUI_CHECKBOX);
    if (vgui_widget_get_checked(field(".boolean", VGUI_CHECKBOX))) click(".boolean", VGUI_CHECKBOX);
    click(".ok");
    const auto simulation = preview.capture_view().simulation;
    require(simulation.at("hud.hp") == ui::value{.375} && simulation.at("hud.ready") == ui::value{false} &&
        !state("caption").visible && !state("button").enabled, "numeric or boolean overrides did not reach the preview");

    auto* dock = find_widget(vgui_root(shell), "designer.dock", VGUI_DOCK);
    auto* tree = find_widget(vgui_root(shell), "designer.outliner", VGUI_TREE);
    require(dock && tree, "missing dock or outline for the locate test");
    const auto root = vgui_tree_get_root(tree, 0);
    const auto group = vgui_tree_get_child(tree, root, 3);
    const auto mirror = vgui_tree_get_child(tree, group, 0);
    require(mirror && !vgui_tree_get_item_expanded(tree, group), "the target's ancestor must start collapsed");
    vgui_dock_activate(dock, vgui_dock_find(dock, "controls"));
    vgui_dock_set_side_collapsed(dock, VGUI_DOCK_LEFT, 1);
    frame();
    input.click(simulate);
    type("Unconfirmed");
    click(".debug_tab");
    require(read(".published") == "\"Pilot ready\"" && read(".effective") == "\"Pilot ready\"",
        "field debug did not distinguish an applied text value from a new draft");
    first_row(field(".fields", VGUI_LIST));
    keys.key(SDLK_DOWN, SDL_SCANCODE_DOWN);
    auto* targets = field(".targets", VGUI_LIST);
    require(vgui_list_get_count(targets) == 2 && std::string_view(vgui_list_get_item_text(targets, vgui_list_get_item_at(targets, 0))) == "meter" &&
        read(".published") == "0.375" && read(".authored") == "0.5" && read(".effective") == "0.5",
        "progress step was not reflected in the first binding target");
    first_row(targets);
    keys.key(SDLK_DOWN, SDL_SCANCODE_DOWN);
    require(read(".effective") == "0.0625" && read(".published") == "0.375",
        "the transitioned target should still show its initial value before a clock tick");
    const auto selected_target = vgui_list_get_selected(targets, 0);
    tick_preview(0.25f);
    const auto halfway = std::stof(read(".effective"));
    require(halfway > 0.0625f && halfway < 0.125f && read(".published") == "0.375" &&
        vgui_list_get_selected(targets, 0) == selected_target,
        "the open debug view should update the effective value without moving the target selection");
    tick_preview(0.75f);
    require(read(".authored") == "0.0625" && read(".effective") == "0.125",
        "the second target did not show its own design and clamped actual value");
    type("Not editable", ".effective");
    require(read(".effective") == "0.125", "the field diagnostics are editable");
    click(".locate");
    require(!vgui_modal_active(shell) && preview.running() && editor.selection() == "mirror" &&
        vgui_dock_get_active(dock, VGUI_DOCK_LEFT) == vgui_dock_find(dock, "structure") &&
        !vgui_dock_get_side_collapsed(dock, VGUI_DOCK_LEFT) && vgui_tree_get_item_expanded(tree, group) &&
        vgui_tree_is_selected(tree, mirror) && vgui_tree_get_cursor(tree) == mirror,
        "locating did not reveal and select the nested target in the outline");
    require(preview.node_text("caption") == "Pilot ready" && preview.capture_view().simulation == simulation &&
        editor.history().revision() == revision && editor.history().current() == doc && !editor.history().can_undo(),
        "locating applied a draft or changed the document");

    input.click(simulate);
    click(".reset_all");
    click(".cancel");
    require(preview.capture_view().simulation == simulation && !state("caption").visible, "cancel committed a reset");
    input.click(mode);
    require(!preview.running() && preview.node_text("caption") == "Design" && state("caption").visible && state("button").enabled,
        "Design mode is using mock data");
    input.click(mode);
    require(preview.node_text("caption") == "Pilot ready" && !state("caption").visible,
        "returning to Run lost the mock values");
    require(editor.open(second), "open a second page with identical keys");
    frame();
    require(editor.preview().capture_view().simulation.empty() && editor.preview().node_text("caption") == "Design",
        "mock data leaked to another page");
    editor.request(command::close);
    frame();
    require(editor.open(source), "return to the first page");
    frame();
    require(preview.capture_view().simulation == simulation && !state("caption").visible, "page switch lost its mock values");
    input.click(simulate);
    click(".reset_all");
    click(".ok");
    require(preview.capture_view().simulation.empty() && preview.node_text("caption") == "Design" &&
        state("caption").visible && state("button").enabled && editor.history().revision() >= revision &&
        editor.history().current() == doc && !editor.history().dirty() && !editor.history().can_undo(),
        "reset did not restore the design or mock data changed document history");
    ui::document saved;
    require(ui::read_document(source, saved) && saved == doc, "mock data was written to disk");
    editor.request(command::close);
    frame();
}

class native_gamepad_input {
public:
    native_gamepad_input(SDL_Window* window, const std::function<void()>& frame, void (*require)(bool, std::string_view))
        : window_(window), frame_(frame), require_(require) {}
    native_gamepad_input(const native_gamepad_input&) = delete;
    native_gamepad_input& operator=(const native_gamepad_input&) = delete;
    ~native_gamepad_input() { detach(); }

    void plug() {
        if (SDL_GetKeyboardFocus() != window_) {
            // CTest may be started by a background launcher on Windows.
            // Ask SDL to activate the actual test window, not to bypass the
            // foreground check in the editor's input path.
            const auto* hint = SDL_GetHint(SDL_HINT_FORCE_RAISEWINDOW);
            const auto old = hint ? std::optional<std::string>(hint) : std::nullopt;
            SDL_SetHint(SDL_HINT_FORCE_RAISEWINDOW, "1");
            const bool topmost = (SDL_GetWindowFlags(window_) & SDL_WINDOW_ALWAYS_ON_TOP) != 0;
            if (!topmost) SDL_SetWindowAlwaysOnTop(window_, true);
            // Some window managers apply the raise asynchronously.
            for (int i = 0; i < 12 && SDL_GetKeyboardFocus() != window_; ++i) {
                if (i % 4 == 0) SDL_RaiseWindow(window_);
                frame_();
                SDL_Delay(16);
            }
            if (!topmost) SDL_SetWindowAlwaysOnTop(window_, false);
            if (old) SDL_SetHint(SDL_HINT_FORCE_RAISEWINDOW, old->c_str());
            else SDL_ResetHint(SDL_HINT_FORCE_RAISEWINDOW);
        }
        require_(SDL_GetKeyboardFocus() == window_, "the smoke window must have SDL keyboard focus");
        SDL_VirtualJoystickDesc desc;
        SDL_INIT_INTERFACE(&desc);
        desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        // Identity mapping: the virtual joystick index matches SDL_GamepadButton.
        desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
        desc.button_mask = (Uint32{1} << SDL_GAMEPAD_BUTTON_COUNT) - 1;
        desc.name = "VGUI designer smoke gamepad";
        id_ = SDL_AttachVirtualJoystick(&desc);
        require_(id_ && SDL_IsGamepad(id_), "cannot attach a virtual SDL gamepad");
        joystick_ = SDL_OpenJoystick(id_);
        require_(joystick_ != nullptr, "cannot open the virtual SDL joystick");
        frame_();
        require_(SDL_GetGamepadFromID(id_) != nullptr, "the editor did not open the hot-plugged gamepad");
    }

    void tap(SDL_GamepadButton button) {
        require_(SDL_GetKeyboardFocus() == window_, "the smoke window lost SDL keyboard focus");
        for (const bool down : {true, false}) {
            require_(SDL_SetJoystickVirtualButton(joystick_, button, down), "cannot set a virtual gamepad button");
            // Dispatch each real SDL edge before scheduling the next one.
            SDL_UpdateJoysticks();
            frame_();
        }
    }

    void unplug() {
        const auto id = id_;
        require_(detach(), "cannot detach the virtual SDL gamepad");
        frame_();
        require_(SDL_GetGamepadFromID(id) == nullptr, "the editor kept the unplugged gamepad open");
    }

private:
    bool detach() {
        if (joystick_) SDL_CloseJoystick(std::exchange(joystick_, nullptr));
        if (id_ && !SDL_DetachVirtualJoystick(id_)) return false;
        id_ = 0;
        return true;
    }

    SDL_Window* window_;
    const std::function<void()>& frame_;
    void (*require_)(bool, std::string_view);
    SDL_JoystickID id_ = 0;
    SDL_Joystick* joystick_ = nullptr;
};

void exercise_navigation_edit(os_window& win, vgui_context* shell, designer& editor,
                              const std::function<void()>& frame, const fs::path& sandbox,
                              vgui_render_target* target, const vgui_render_target_desc& desc,
                              std::vector<uint8_t>& pixels, bool bgra) {
    const auto require = [](bool passed, std::string_view message) {
        if (!passed) throw std::runtime_error("Design navigation: " + std::string(message));
    };
    ui::document doc;
    require(ui::decode_json(R"({"format":"vgui-ui-design","version":1,"root":"root","nodes":{
        "root":{"type":"panel","props":{"layout":"absolute","nav_start":"source"},"children":["source","near","far","dialog","inactive"]},
        "source":{"type":"button","props":{"x":176,"y":140,"width":128,"height":80,"nav_right":"far","text":"Source","focus_alpha":0}},
        "near":{"type":"button","props":{"x":362,"y":148,"width":112,"height":64,"text":"Near"}},
        "far":{"type":"button","props":{"x":525,"y":315,"width":120,"height":64,"text":"Far","action":"nav.edit.far"}},
        "dialog":{"type":"panel","props":{"x":98,"y":300,"width":245,"height":160,"modal":true},"children":["inside"]},
        "inside":{"type":"button","props":{"x":22,"y":27,"width":120,"height":64,"text":"Inside modal"}},
        "inactive":{"type":"button","props":{"x":374,"y":385,"width":108,"height":52,"text":"Not focusable","focusable":"no"}}
    }})", doc), "decode the link editor fixture");
    const auto file = sandbox / "nav-edit.vgui.json";
    require(ui::write_document(file, doc) && editor.open(file), "open the link editor fixture");
    frame();

    auto& preview = editor.preview();
    const auto& history = editor.history();
    const native_keyframe_input input{win, shell, preview, frame, require};
    const ui::native_menu_input keys(win, shell, frame);
    auto* canvas = input.widget("designer.preview.canvas");
    auto* toggle = find_widget(preview.widget(), "designer.preview.navigation_edit", VGUI_BUTTON);
    auto* initial_focus = find_widget(preview.widget(), "designer.preview.navigation_start", VGUI_BUTTON);
    require(!preview.running() && toggle && vgui_widget_get_visible(toggle) && vgui_widget_get_enabled(toggle) &&
        !vgui_widget_get_checked(toggle), "the navigation toggle must be available and off in Design");
    require(initial_focus && !vgui_widget_get_visible(initial_focus),
        "the initial focus button must start hidden with the navigation tool off");
    const auto node = [&](std::string_view id) {
        const auto* layout = preview.snapshot();
        require(layout != nullptr, "missing Design layout");
        const auto it = std::ranges::find(layout->nodes, id, &ui::node_layout::id);
        require(it != layout->nodes.end() && it->visible, id);
        return it->visible_px;
    };
    const auto project = [&](float x, float y) {
        const auto board = preview.artboard_rect_px();
        const auto* layout = preview.snapshot();
        require(board && layout && layout->pixel_width > 0 && layout->pixel_height > 0, "missing Design artboard");
        const auto pos = glm::round(glm::vec2{board->x + x * board->width / layout->pixel_width,
            board->y + y * board->height / layout->pixel_height});
        require(vgui_hit_test(shell, pos.x, pos.y) == canvas, "fixture point is outside the canvas");
        return pos;
    };
    const auto at = [&](std::string_view id, float u = .5f, float v = .5f) {
        const auto r = node(id);
        return project(r.x + u * r.width, r.y + v * r.height);
    };
    const auto read_navigation_pixels = [&] {
        require(vgui_render_target_submit_context(target, shell), "cannot composite Design navigation overlay");
        require(vgui_render_target_download(target, pixels.data(), pixels.size()) == pixels.size(),
            "cannot read Design navigation overlay pixels");
    };
    const auto sample_handles = [&] {
        read_navigation_pixels();
        // Look for the navigation stroke in small neighborhoods around the four ports.
        std::array<int, 4> samples{};
        const std::array centers{at("source", .5f, 0), at("source", .5f, 1),
            at("source", 0, .5f), at("source", 1, .5f)};
        const int radius = std::max(2, int(std::ceil(7.f * vgui_context_get_ui_scale(shell))));
        for (size_t i = 0; i < centers.size(); ++i) {
            const int x = int(centers[i].x), y = int(centers[i].y);
            require(x >= radius && y >= radius && x + radius < int(desc.width) && y + radius < int(desc.height),
                "handle sample is outside the render target");
            for (int row = y - radius; row <= y + radius; ++row) for (int col = x - radius; col <= x + radius; ++col) {
                const auto* p = pixels.data() + (size_t(row) * desc.width + col) * 4;
                samples[i] += std::abs(p[bgra ? 2 : 0] - 0x64) <= 16 && std::abs(p[1] - 0xDF) <= 16 &&
                    std::abs(p[bgra ? 0 : 2] - 0xFF) <= 16;
            }
        }
        return samples;
    };
    const auto inspector_is = [&](std::string_view key, std::string_view value) {
        auto* field = find_widget(vgui_root(shell), std::string("property:").append(key), VGUI_EDIT);
        require(field && std::string_view(vgui_edit_get_text(field)) == value, "inspector navigation value is stale");
    };
    input.click_at(canvas, at("source"));
    require(editor.selection() == "source", "could not select the link source");
    auto* search = find_widget(vgui_root(shell), "__property_search", VGUI_EDIT);
    require(search != nullptr, "missing inspector search");
    input.click(search);
    SDL_Event text{};
    text.type = SDL_EVENT_TEXT_INPUT;
    text.text.windowID = win.id;
    text.text.text = "nav_";
    input.push(text);
    frame();
    inspector_is("nav_right", "far");

    const auto original_revision = history.revision();
    const auto untouched = [&](std::string_view message) {
        require(history.revision() == original_revision && history.current() == doc &&
            !history.dirty() && !history.can_undo() && editor.selection() == "source", message);
    };
    input.click_at(canvas, at("source", 1, .5f), SDL_BUTTON_RIGHT);
    untouched("the default-off tool cleared an authored link");
    const std::array<int, 4> no_handles{};
    require(sample_handles() == no_handles, "navigation handles are visible with the tool initially off");
    input.click(toggle);
    require(vgui_widget_get_checked(toggle) &&
        std::ranges::all_of(sample_handles(), [](int count) { return count >= 4; }),
        "enabling the tool did not paint all four side midpoint handles");
    untouched("enabling the tool edited the document");

    const auto begin_drag = [&](glm::vec2 from, glm::vec2 to) {
        input.button(true, from);
        input.motion(from, to);
    };
    const auto release = [&](glm::vec2 to) {
        input.button(false, to);
        frame();
    };
    const auto up = [&] { return at("source", .5f, 0); };
    const auto right = [&] { return at("source", 1, .5f); };
    const auto near = [&] { return at("near"); };
    begin_drag(up(), near());
    frame();
    untouched("drag motion committed before release");
    keys.key(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE);
    release(near());
    untouched("Escape did not cancel the link through mouse-up");
    const auto invalid_drop = [&](glm::vec2 end, std::string_view message) {
        begin_drag(right(), end);
        release(end);
        untouched(message);
    };
    invalid_drop(project(650, 540), "a blank drop changed the existing override");
    invalid_drop(at("source"), "a self drop changed the existing override");
    invalid_drop(at("inside"), "a cross-modal drop changed the existing override");

    const auto check_edit = [&](const ui::document& expected, uint64_t revision, std::string_view message) {
        require(history.revision() == revision + 1 && history.current() == expected &&
            history.committed() == expected && history.dirty() && history.can_undo() &&
            editor.selection() == "source", message);
    };
    auto expected = doc;
    begin_drag(up(), near());
    release(near());
    expected.nodes.at("source").props["nav_up"] = std::string("near");
    check_edit(expected, original_revision, "adding a link did not commit one edit on the selected source");
    inspector_is("nav_up", "near");

    auto revision = history.revision();
    begin_drag(right(), near());
    release(near());
    expected.nodes.at("source").props["nav_right"] = std::string("near");
    check_edit(expected, revision, "replacing a link did not commit one edit on the selected source");
    inspector_is("nav_right", "near");

    revision = history.revision();
    input.click_at(canvas, right(), SDL_BUTTON_RIGHT);
    const auto replaced = expected;
    expected.nodes.at("source").props.erase("nav_right");
    check_edit(expected, revision, "right click did not remove just the authored override in one edit");
    inspector_is("nav_right", "");
    keys.activate(keys.open(1), 0); // Edit > Undo
    require(history.current() == replaced, "one Undo did not restore the right override");
    inspector_is("nav_right", "near");
    keys.activate(keys.open(1), 1); // Edit > Redo
    require(history.current() == expected, "one Redo did not remove the right override");
    inspector_is("nav_right", "");

    input.click(toggle);
    require(!vgui_widget_get_checked(toggle) && sample_handles() == no_handles,
        "turning off the tool left navigation handles on the canvas");
    const auto start = at("source");
    const auto moved = start + glm::vec2{24, 16};
    revision = history.revision();
    begin_drag(start, moved);
    release(moved);
    auto geometry_only = expected;
    const auto& props = history.current().nodes.at("source").props;
    require(std::get<double>(props.at("x")) > 176 && std::get<double>(props.at("y")) > 140,
        "the ordinary Design move did not resume with the navigation tool off");
    geometry_only.nodes.at("source").props["x"] = props.at("x");
    geometry_only.nodes.at("source").props["y"] = props.at("y");
    check_edit(geometry_only, revision, "a geometry move with the tool off changed unrelated properties");
    keys.activate(keys.open(1), 0);
    require(history.current() == expected, "one Undo did not restore the source position");

    editor.request(command::save);
    frame();
    ui::document saved;
    require(ui::read_document(file, saved) && saved == expected && !history.dirty(),
        "the new link or the removal of the old override did not survive saving");
    revision = history.revision();
    input.click(toggle);
    input.click("designer.preview.mode");
    require(preview.running() && !vgui_widget_get_visible(toggle) && sample_handles() == no_handles,
        "the Design navigation toggle or handles are visible in Run");
    require(!vgui_widget_get_visible(initial_focus), "the initial focus button is visible in Run");
    input.click_at(canvas, up(), SDL_BUTTON_RIGHT);
    require(history.current() == expected && history.revision() == revision && !history.dirty(),
        "the Design navigation tool edited the document in Run");

    const auto sample_flag = [&](std::string_view id) {
        read_navigation_pixels();
        // The flag belongs just inside the upper-left corner, away from the side midpoints.
        const auto corner = at(id, 0, 0);
        const auto opposite = at(id, 1, 1);
        const float scale = std::min({vgui_context_get_ui_scale(shell),
            (opposite.x - corner.x) / 38.f, (opposite.y - corner.y) / 22.f});
        const int x = int(corner.x), y = int(corner.y), extent = int(std::ceil(18 * scale));
        require(extent > 0 && x >= 0 && y >= 0 && x + extent < int(desc.width) && y + extent < int(desc.height),
            "initial focus flag sample is outside the render target");
        int gold = 0;
        for (int row = y; row <= y + extent; ++row) for (int col = x; col <= x + extent; ++col) {
            const auto* p = pixels.data() + (size_t(row) * desc.width + col) * 4;
            gold += std::abs(p[bgra ? 2 : 0] - 0xFF) <= 12 && std::abs(p[1] - 0xCE) <= 12 &&
                std::abs(p[bgra ? 0 : 2] - 0x64) <= 12;
        }
        return gold;
    };
    const auto select = [&](std::string_view id) {
        input.click_at(canvas, id == doc.root ? project(650, 540) : at(id));
        require(editor.selection() == id, "could not select the initial focus test node");
    };
    const auto change_start = [&](std::string_view target_id) {
        const auto before = history.current();
        auto after = before;
        auto& root_props = after.nodes.at(doc.root).props;
        if (target_id.empty()) root_props.erase("nav_start");
        else root_props["nav_start"] = std::string(target_id);
        const auto selected = std::string(editor.selection());
        const auto start_revision = history.revision();
        require(vgui_widget_get_visible(initial_focus) && vgui_widget_get_enabled(initial_focus),
            "the initial focus button is unavailable for a valid edit");
        input.click(initial_focus);
        require(history.revision() == start_revision + 1 && history.current() == after &&
            history.committed() == after && history.dirty() && editor.selection() == selected,
            "initial focus did not commit exactly one edit without changing other properties or selection");
        keys.activate(keys.open(1), 0);
        require(history.current() == before, "one Undo did not restore the previous initial focus");
        keys.activate(keys.open(1), 1);
        require(history.current() == after, "one Redo did not reapply the initial focus edit");
    };

    input.click("designer.preview.mode");
    if (!vgui_widget_get_checked(toggle)) input.click(toggle);
    select("inside");
    require(vgui_widget_get_visible(initial_focus) && !vgui_widget_get_enabled(initial_focus),
        "a focusable control inside a modal can be set as the page's initial focus");
    select("inactive");
    require(!vgui_widget_get_enabled(initial_focus), "a non-focusable control can be set as the initial focus");
    select(doc.root);
    require(!vgui_widget_get_enabled(initial_focus), "the root can be set as its own initial focus");
    inspector_is("nav_start", "source");
    require(sample_flag("source") >= 4, "the configured initial focus was not marked when unselected");
    input.click(toggle);
    require(!vgui_widget_get_visible(initial_focus) && sample_flag("source") == 0,
        "the initial focus button or flag remained visible with the tool off");
    input.click(toggle);

    // The inspector can author a reference even when that target cannot take focus.
    auto* property = find_widget(vgui_root(shell), "property:nav_start", VGUI_EDIT);
    input.click(property);
    vgui_edit_select_all(property);
    text.text.text = "inactive";
    input.push(text);
    frame();
    keys.key(SDLK_RETURN, SDL_SCANCODE_RETURN);
    auto inactive_start = expected;
    inactive_start.nodes.at(doc.root).props["nav_start"] = std::string("inactive");
    require(history.committed() == inactive_start, "could not configure a non-focusable initial target via the inspector");
    select("inactive");
    change_start({});
    select(doc.root);
    inspector_is("nav_start", "");

    select("near");
    change_start("near");
    require(sample_flag("near") >= 4 && sample_flag("source") == 0,
        "setting initial focus did not place the gold flag on the new target");
    select("far");
    change_start("far");
    require(sample_flag("far") >= 4 && sample_flag("near") == 0,
        "replacing initial focus did not move the gold flag to the replacement");
    select(doc.root);
    inspector_is("nav_start", "far");
    select("far");
    change_start({});
    require(sample_flag("far") == 0, "clearing initial focus left a gold flag on the former target");
    select(doc.root);
    inspector_is("nav_start", "");
    keys.activate(keys.open(1), 0);
    inspector_is("nav_start", "far");

    auto saved_start = expected;
    saved_start.nodes.at(doc.root).props["nav_start"] = std::string("far");
    editor.request(command::save);
    frame();
    require(ui::read_document(file, saved) && saved == saved_start && !history.dirty(),
        "the authored initial focus or original navigation links did not survive saving");
    revision = history.revision();
    input.click("designer.preview.mode");
    require(preview.running() && vgui_get_focus(shell) == canvas &&
        !vgui_widget_get_visible(initial_focus) && sample_flag("far") == 0,
        "Run did not focus the canvas or still displays Design initial focus authoring");
    keys.key(SDLK_RETURN, SDL_SCANCODE_RETURN);
    auto* status = find_widget(vgui_root(shell), "designer.status");
    require(status && std::string_view(vgui_widget_get_text(status)).ends_with("nav.edit.far [far]") &&
        history.current() == saved_start && history.revision() == revision && !history.dirty(),
        "Run did not activate the newly configured initial focus without editing the document");
}

void exercise_navigation(vgpu_ctx_t* gpu, ovg_ctx_cb* cb, os_window& win, vgui_context* shell,
                         designer& editor, const std::function<void()>& frame, const fs::path& sandbox) {
    const auto require = [](bool passed, std::string_view message) {
        if (!passed) throw std::runtime_error("Native navigation: " + std::string(message));
    };
    ui::document doc;
    require(ui::decode_json(R"({"format":"vgui-ui-design","version":1,"root":"root","nodes":{
        "root":{"type":"panel","props":{"nav_start":"start"},"children":["near","start","far"]},
        "near":{"type":"button","props":{"x":150,"y":40,"width":100,"height":36,"text":"Near","action":"nav.near"}},
        "start":{"type":"button","props":{"x":20,"y":40,"width":100,"height":36,"text":"Start","action":"nav.start",
            "nav_right":"far","focus_alpha":1}},
        "far":{"type":"button","props":{"x":340,"y":150,"width":100,"height":36,"text":"Far","action":"nav.far",
            "nav_left":"start","focus_alpha":1,"bind_enabled":"hud.far"}}
    }})", doc), "decode the navigation fixture");
    const auto file = sandbox / "nav.vgui.json";
    require(ui::write_document(file, doc) && editor.open(file), "open the navigation fixture");
    frame();

    auto& preview = editor.preview();
    const auto revision = editor.history().revision();
    const native_keyframe_input input{win, shell, preview, frame, require};
    const ui::native_menu_input keys(win, shell, frame);
    native_gamepad_input pad(win.window, frame, require);
    const auto status_text = [&] {
        auto* status = find_widget(vgui_root(shell), "designer.status");
        require(status != nullptr, "missing action status");
        return std::string_view(vgui_widget_get_text(status));
    };
    const auto action = [&](std::string_view id) {
        keys.key(SDLK_RETURN, SDL_SCANCODE_RETURN);
        require(status_text().ends_with(id), id);
    };
    constexpr auto mode = "designer.preview.mode";
    constexpr auto debug = "designer.preview.navigation_debug";
    const auto debug_control = [&](std::string_view suffix, vgui_widget_kind kind) {
        return find_widget(preview.widget(), std::string(debug).append(suffix), kind);
    };
    const auto strip_visible = [&] {
        return vgui_widget_get_visible(debug_control(".panel", VGUI_PANEL));
    };
    const auto label = [&](std::string_view suffix) {
        auto* w = debug_control(suffix, VGUI_LABEL);
        require(vgui_widget_get_visible(w), suffix);
        return std::string_view(vgui_widget_get_text(w));
    };
    const auto localized = [&](std::string_view suffix, const char* fallback) {
        return editor.strings().text(std::string("preview.navigation_debug").append(suffix), fallback);
    };
    const auto focus_is = [&](std::string_view id) {
        const auto text = label(".focus");
        return text.contains(localized(".focus", "Focus")) && text.contains(id);
    };
    require(!vgui_widget_get_visible(debug_control("", VGUI_BUTTON)) && !strip_visible(),
        "focus debug should be hidden in Design");
    pad.plug();
    auto* mode_button = input.widget(mode);
    // If A leaks to the shell as Enter, this focused button will enter Run.
    vgui_set_focus(shell, mode_button);
    require(vgui_get_focus(shell) == mode_button, "cannot focus the Design mode toggle");
    const auto design_status = std::string(status_text());
    pad.tap(SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
    require(vgui_get_focus(shell) == mode_button, "D-pad moved shell focus in Design");
    pad.tap(SDL_GAMEPAD_BUTTON_SOUTH);
    require(!preview.running() && status_text() == design_status, "gamepad triggered an action in Design");
    input.click(mode_button);
    auto* canvas = input.widget("designer.preview.canvas");
    require(preview.running() && vgui_get_focus(shell) == canvas,
        "entering Run did not focus the artboard with a configured default");
    auto* toggle = debug_control("", VGUI_BUTTON);
    require(vgui_widget_get_visible(toggle) && vgui_widget_get_enabled(toggle) && !strip_visible(),
        "Run should start with the focus debug toggle available and the strip closed");

    // Record after the host submits, into the target's separate canvas. Borrow
    // the host's callback table since vgui_draw retains it on the shell.
    vgui_render_target_desc desc;
    vgui_render_target_desc_default(&desc);
    vgui_context_get_size(shell, &desc.width, &desc.height);
    desc.sample_count = 1;
    const auto format = vgpu_color_format(gpu); // The SDR window can use either byte order.
    const bool bgra = format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
    require(bgra || format == SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, "unexpected readback format");
    std::unique_ptr<vgui_render_target, decltype(&vgui_render_target_destroy)> target(
        vgui_render_target_create(shell, gpu, cb, &desc), vgui_render_target_destroy);
    require(bool(target), "cannot create navigation readback target");
    std::vector<uint8_t> pixels(size_t(desc.width) * desc.height * 4);
    struct link_counts {
        int authored = 0, automatic = 0;
        bool empty() const { return !authored && !automatic; }
    };
    struct link_samples { link_counts near_gap, far_gap; };
    const auto sample_links = [&] {
        require(vgui_render_target_submit_context(target.get(), shell), "cannot composite navigation overlay");
        require(vgui_render_target_download(target.get(), pixels.data(), pixels.size()) == pixels.size(),
            "cannot read navigation overlay pixels");
        const auto board = preview.artboard_rect_px();
        const auto* layout = preview.snapshot();
        require(board && layout && layout->pixel_width > 0, "missing navigation board for readback");
        const auto bounds = [&](std::string_view id) {
            const auto node = std::ranges::find(layout->nodes, id, &ui::node_layout::id);
            require(node != layout->nodes.end() && node->visible, "missing navigation node for readback");
            return node->visible_px;
        };
        const auto area = input.rect(canvas);
        const int y0 = int(std::ceil(std::max({0.f, board->y, area.y})));
        const int y1 = int(std::floor(std::min({float(desc.height), board->y + board->height, area.y + area.height})));
        const auto gap = [&](ui::pixel_rect left, ui::pixel_rect right) {
            // Scan the open space between controls, away from the endpoints.
            // Use the current board since opening the strip changes the fit.
            const float scale = board->width / layout->pixel_width;
            const float lo = board->x + (left.x + left.width) * scale, hi = board->x + right.x * scale;
            const float pad = (hi - lo) * .2f;
            const int x0 = int(std::ceil(std::max({0.f, area.x, lo + pad})));
            const int x1 = int(std::floor(std::min({float(desc.width), area.x + area.width, hi - pad})));
            require(x1 - x0 >= 4 && y1 > y0, "no visible space between navigation fixture controls");
            link_counts counts;
            for (int y = y0; y < y1; ++y) for (int x = x0; x < x1; ++x) {
                const auto* p = pixels.data() + (size_t(y) * desc.width + x) * 4;
                const auto matches = [&](int r, int g, int b) {
                    // Count near-opaque stroke interiors; tolerate edge antialiasing.
                    return std::abs(p[bgra ? 2 : 0] - r) <= 16 && std::abs(p[1] - g) <= 16 &&
                        std::abs(p[bgra ? 0 : 2] - b) <= 16;
                };
                counts.authored += matches(0x64, 0xDF, 0xFF);
                counts.automatic += matches(0xFF, 0xC7, 0x77);
            }
            return counts;
        };
        const auto start_bounds = bounds("start"), near_bounds = bounds("near"), far_bounds = bounds("far");
        return link_samples{gap(start_bounds, near_bounds), gap(near_bounds, far_bounds)};
    };

    input.click(toggle);
    require(strip_visible() && vgui_get_focus(shell) == canvas && focus_is("start"),
        "opening focus debug did not preserve the configured focus and keyboard routing");
    const auto right = label(".right");
    require(right.contains(localized(".right", "Right")) && right.contains("far") &&
        right.contains(localized(".explicit", "Explicit")), "Right should show the explicit far target");
    const auto up = label(".up");
    require(up.contains(localized(".up", "Up")) && up.contains("far") && up.contains(localized(".automatic", "Auto")) &&
        label(".down").contains(localized(".down", "Down")) && label(".left").contains(localized(".left", "Left")),
        "the strip must show all four directions, including the automatic Up wrap to far");
    const auto opened_links = sample_links();
    require(opened_links.far_gap.authored >= 4 && opened_links.far_gap.automatic >= 4,
        "opening focus debug did not paint both cyan explicit and amber automatic links to far");
    const auto fit_board = *preview.artboard_rect_px();
    SDL_Event wheel{};
    wheel.type = SDL_EVENT_MOUSE_WHEEL;
    wheel.wheel.windowID = win.id;
    wheel.wheel.y = -1.f;
    wheel.wheel.mouse_x = (fit_board.x + fit_board.width / 2) / win.pixel_ratio();
    wheel.wheel.mouse_y = (fit_board.y + fit_board.height / 2) / win.pixel_ratio();
    input.push(wheel);
    const auto old_mods = SDL_GetModState();
    SDL_SetModState(SDL_KMOD_LCTRL);
    frame();
    SDL_SetModState(old_mods);
    const auto zoomed_board = preview.artboard_rect_px();
    require(zoomed_board && zoomed_board->width < fit_board.width && focus_is("start"),
        "Ctrl+wheel did not zoom around the canvas while preserving focus");
    const auto zoomed_links = sample_links();
    require(zoomed_links.far_gap.authored >= 4 && zoomed_links.far_gap.automatic >= 4,
        "the navigation lines did not follow the zoomed board");
    preview.reset_zoom(true);
    frame();
    action("nav.start [start]");
    require(preview.snapshot() != nullptr, "missing focused preview snapshot");
    const auto before = preview.snapshot()->serial;
    pad.tap(SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
    require(preview.snapshot() && preview.snapshot()->serial != before, "direction move did not repaint the focus ring");
    require(focus_is("far") && label(".left").contains("start") &&
        label(".left").contains(localized(".explicit", "Explicit")),
        "D-pad Right did not follow the explicit link or update the debug strip");
    pad.tap(SDL_GAMEPAD_BUTTON_SOUTH);
    require(status_text().ends_with("nav.far [far]"), "A did not activate the focused far button");
    const auto center = [&](vgui_widget* w) {
        const auto r = input.rect(w);
        return glm::vec2{r.x + r.width / 2, r.y + r.height / 2};
    };
    const auto press = center(toggle), release = center(mode_button);
    require(vgui_hit_test(shell, press.x, press.y) == toggle && vgui_hit_test(shell, release.x, release.y) == mode_button,
        "cannot exercise a release outside the focus debug toggle");
    input.button(true, press);
    input.motion(press, release);
    input.button(false, release);
    frame();
    require(preview.running() && strip_visible() && vgui_get_focus(shell) == canvas && focus_is("far"),
        "pressing the toggle and releasing elsewhere changed focus or toggled the strip");
    keys.key(SDLK_LEFT, SDL_SCANCODE_LEFT);
    action("nav.start [start]");

    input.click("designer.preview.simulation");
    auto* simulation_panel = vgui_modal_panel(shell);
    require(vgui_modal_active(shell) && simulation_panel, "missing shell simulation modal");
    pad.tap(SDL_GAMEPAD_BUTTON_EAST);
    require(vgui_modal_active(shell) && vgui_modal_panel(shell) == simulation_panel,
        "B dismissed the shell simulation modal");
    const auto modal = [&](const char* id, vgui_widget_kind kind) {
        auto* w = find_widget(vgui_modal_panel(shell), id, kind);
        require(w != nullptr, id);
        return w;
    };
    input.click(modal("designer.preview.simulation.override", VGUI_CHECKBOX));
    require(!vgui_widget_get_checked(modal("designer.preview.simulation.boolean", VGUI_CHECKBOX)),
        "expected a false draft for the enable field");
    input.click(modal("designer.preview.simulation.ok", VGUI_BUTTON));
    // The simulation dialog releases live focus. Refocus start without remounting
    // or reopening the strip so this also checks updates in the current Run session.
    const auto board = preview.artboard_rect_px();
    const auto* layout = preview.snapshot();
    require(board && layout, "missing preview after applying simulation");
    const auto start = std::ranges::find(layout->nodes, "start", &ui::node_layout::id);
    require(start != layout->nodes.end() && start->visible && start->enabled, "start is no longer available");
    const auto& bounds = start->visible_px;
    input.click_at(canvas, {board->x + (bounds.x + bounds.width / 2) * board->width / layout->pixel_width,
        board->y + (bounds.y + bounds.height / 2) * board->height / layout->pixel_height});
    require(focus_is("start") && label(".right").contains("near") &&
        label(".right").contains(localized(".automatic", "Auto")),
        "disabling far did not refresh the strip with the automatic near target");
    require(label(".up").contains(localized(".none", "None")),
        "disabling the only off-row target should leave Up without a target");
    const auto fallback_links = sample_links();
    require(fallback_links.far_gap.empty() && !fallback_links.near_gap.authored &&
        fallback_links.near_gap.automatic >= 4,
        "disabling far should clear its lines and paint the automatic fallback to near");
    pad.tap(SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
    require(focus_is("near"), "D-pad Right did not follow the automatic fallback in the same Run view");
    action("nav.near [near]");
    input.click(toggle);
    require(!strip_visible() && vgui_widget_get_visible(toggle) && vgui_widget_get_enabled(toggle) &&
        vgui_get_focus(shell) == canvas, "closing focus debug did not leave the canvas and toggle available");
    require(preview.snapshot() != nullptr, "missing preview after closing focus debug");
    const auto closed = preview.snapshot()->serial;
    keys.key(SDLK_LEFT, SDL_SCANCODE_LEFT);
    require(preview.snapshot() && preview.snapshot()->serial != closed,
        "closing the strip must not reset the current focus or swallow the next arrow");
    action("nav.start [start]");
    const auto closed_links = sample_links(); // Same focus and availability as the fallback sample.
    require(closed_links.near_gap.empty() && closed_links.far_gap.empty(),
        "closing focus debug did not remove the navigation lines");
    input.click(toggle);
    input.click(mode);
    require(!preview.running() && !vgui_widget_get_visible(debug_control("", VGUI_BUTTON)) && !strip_visible(),
        "returning to Design must hide an open focus debug strip and its toggle");
    pad.unplug();
    require(editor.history().revision() == revision && editor.history().current() == doc && !editor.history().dirty(),
        "navigating, toggling focus debug, or simulating availability changed the design");
    exercise_navigation_edit(win, shell, editor, frame, sandbox, target.get(), desc, pixels, bgra);
}

void exercise_modal_return(os_window& win, vgui_context* shell, designer& editor,
                           const std::function<void()>& frame, const fs::path& sandbox) {
    const auto require = [](bool passed, std::string_view message) {
        if (!passed) throw std::runtime_error("Native modal return: " + std::string(message));
    };
    ui::document doc;
    require(ui::decode_json(R"({"format":"vgui-ui-design","version":1,"root":"root","nodes":{
        "root":{"type":"panel","props":{"nav_start":"open"},"children":["open","return_to","new_return","inactive","dialog"]},
        "open":{"type":"button","props":{"x":16,"y":20,"width":120,"height":36,"text":"Open","open_modal":"dialog","action":"modal.open"}},
        "return_to":{"type":"button","props":{"x":154,"y":20,"width":120,"height":36,"text":"Restored","action":"restored"}},
        "new_return":{"type":"button","props":{"x":292,"y":20,"width":120,"height":36,"text":"New target","action":"modal.new_return"}},
        "inactive":{"type":"button","props":{"x":365,"y":106,"width":120,"height":36,"text":"Not focusable","focusable":"no"}},
        "dialog":{"type":"panel","props":{"modal":true,"modal_return_focus":"return_to","x":40,"y":80,"width":300,"height":120,
            "layout":"row","gap":12,"pad_left":12,"pad_top":12},"children":["inside","close"]},
        "inside":{"type":"button","props":{"width":120,"height":36,"text":"Inside","action":"modal.inside","nav_right":"close"}},
        "close":{"type":"button","props":{"width":120,"height":36,"text":"Close","close_modal":true,"action":"modal.close"}}
    }})", doc), "decode the modal fixture");
    const auto file = sandbox / "modal-return.vgui.json";
    require(ui::write_document(file, doc) && editor.open(file), "open the modal fixture");
    frame();

    auto& preview = editor.preview();
    const auto& history = editor.history();
    const auto revision = history.revision();
    const native_keyframe_input input{win, shell, preview, frame, require};
    const ui::native_menu_input keys(win, shell, frame);
    native_gamepad_input pad(win.window, frame, require);
    pad.plug();
    const auto node = [&](std::string_view id) {
        auto* snapshot = preview.snapshot();
        require(snapshot != nullptr, "missing modal layout");
        const auto it = std::ranges::find(snapshot->nodes, id, &ui::node_layout::id);
        require(it != snapshot->nodes.end(), "missing modal node");
        return *it;
    };
    const auto action = [&](std::string_view expected) {
        keys.key(SDLK_RETURN, SDL_SCANCODE_RETURN);
        auto* status = find_widget(vgui_root(shell), "designer.status");
        require(status && std::string_view(vgui_widget_get_text(status)).ends_with(expected), expected);
    };
    constexpr auto mode = "designer.preview.mode";
    const auto authored = node("dialog");
    require(authored.visible && authored.rect_px.x == 40 && authored.rect_px.y == 80,
        "the modal must stay at its authored position in Design");
    input.click(mode);
    require(preview.running() && !node("dialog").visible && !node("inside").visible,
        "the modal must start closed in Run");
    action("modal.open [open]");
    const auto opened = node("dialog");
    require(opened.visible && node("inside").visible && opened.rect_px.x > authored.rect_px.x,
        "opening did not display a centered modal");
    action("modal.inside [inside]");
    keys.key(SDLK_RIGHT, SDL_SCANCODE_RIGHT);
    action("modal.close [close]");
    require(!node("dialog").visible && !node("close").visible, "the close button did not dismiss the modal");
    action("restored [return_to]");

    keys.key(SDLK_LEFT, SDL_SCANCODE_LEFT);
    action("modal.open [open]");
    keys.key(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE);
    require(preview.running() && !node("dialog").visible, "Escape closed the Run view instead of the modal");
    action("restored [return_to]");

    keys.key(SDLK_LEFT, SDL_SCANCODE_LEFT);
    action("modal.open [open]");
    pad.tap(SDL_GAMEPAD_BUTTON_EAST);
    require(preview.running() && !node("dialog").visible && !node("inside").visible,
        "B did not dismiss the Run modal");
    pad.tap(SDL_GAMEPAD_BUTTON_EAST);
    require(vgui_get_focus(shell) == input.widget("designer.preview.canvas"),
        "unconsumed B blurred the Run canvas");
    action("restored [return_to]");

    keys.key(SDLK_LEFT, SDL_SCANCODE_LEFT);
    action("modal.open [open]");
    input.click(mode);
    require(!preview.running() && node("dialog").visible && node("dialog").rect_px.x == authored.rect_px.x,
        "returning to Design did not restore the authored layout");
    pad.unplug();
    require(history.revision() == revision && history.current() == doc && !history.dirty(),
        "opening/closing a dialog modified the design");

    auto* canvas = input.widget("designer.preview.canvas");
    auto* links = find_widget(preview.widget(), "designer.preview.navigation_edit", VGUI_BUTTON);
    auto* pick = find_widget(preview.widget(), "designer.preview.navigation_return", VGUI_BUTTON);
    auto* clear = find_widget(preview.widget(), "designer.preview.navigation_return.clear", VGUI_BUTTON);
    require(links && pick && clear, "missing Design navigation buttons");
    const auto click_tool = [&](vgui_widget* button) {
        const auto r = input.rect(button);
        const glm::vec2 center{r.x + r.width / 2, r.y + r.height / 2};
        require(vgui_hit_test(shell, center.x, center.y) == button, vgui_widget_get_id(button));
        input.click_at(button, center);
    };
    const auto controls = [&](bool show_pick, bool show_clear, bool picking = false) {
        require((vgui_widget_get_visible(pick) != 0) == show_pick &&
            (vgui_widget_get_visible(clear) != 0) == show_clear,
            "modal return buttons have the wrong visibility for the current tool, selection, or target");
        require(!show_pick || (vgui_widget_get_enabled(pick) &&
            (vgui_widget_get_checked(pick) != 0) == picking), "modal return picker is disabled or has the wrong checked state");
        require(!show_clear || vgui_widget_get_enabled(clear), "clearing the explicit target is unavailable");
    };
    const auto project = [&](float x, float y) {
        const auto board = preview.artboard_rect_px();
        const auto* layout = preview.snapshot();
        require(board && layout && layout->pixel_width > 0 && layout->pixel_height > 0, "missing modal artboard");
        return glm::round(glm::vec2{board->x + x * board->width / layout->pixel_width,
            board->y + y * board->height / layout->pixel_height});
    };
    const auto at = [&](std::string_view id, float v = .5f) {
        const auto item = node(id);
        require(item.visible, id);
        const auto r = item.visible_px;
        return project(r.x + r.width / 2, r.y + v * r.height);
    };
    const auto select = [&](std::string_view id, float v = .5f) {
        input.click_at(canvas, at(id, v));
        require(editor.selection() == id, "could not select the expected Design node");
    };
    const auto select_dialog = [&] { select("dialog", .8f); }; // Below both child buttons.
    select_dialog();
    require(!vgui_widget_get_checked(links), "Design navigation must start off");
    controls(false, false);
    select("open");
    input.click(links);
    require(vgui_widget_get_checked(links), "could not enable Design navigation");
    controls(false, false);
    select_dialog();
    controls(true, true);

    const auto untouched = [&](std::string_view message) {
        require(history.revision() == revision && history.current() == doc && history.committed() == doc &&
            !history.dirty() && !history.can_undo() && !history.can_redo() && editor.selection() == "dialog", message);
    };
    click_tool(pick);
    controls(true, true, true);
    untouched("starting a pick changed the design or selection");
    click_tool(pick);
    controls(true, true);
    untouched("cancelling a pick with the toolbar changed the design or selection");

    click_tool(pick);
    const auto reject = [&](glm::vec2 point, std::string_view message) {
        input.click_at(canvas, point);
        untouched(message);
        controls(true, true, true);
    };
    reject(at("inside"), "an in-modal target changed the design or selection");
    reject(at("dialog", .8f), "a self target changed the design or selection");
    reject(at("inactive"), "a non-focusable target changed the design or selection");
    reject(project(22, 110), "a blank pick changed the design or selection");
    keys.key(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE);
    controls(true, true);
    untouched("Escape changed the design or selection instead of just cancelling the pick");

    constexpr auto return_property = "modal_return_focus";
    auto* search = find_widget(vgui_root(shell), "__property_search", VGUI_EDIT);
    require(search != nullptr, "missing inspector property filter");
    input.click(search);
    SDL_Event text{};
    text.type = SDL_EVENT_TEXT_INPUT;
    text.text.windowID = win.id;
    text.text.text = return_property;
    input.push(text);
    frame();
    const auto edit_return = [&](const ui::document& expected, const auto& edit) {
        const auto before = history.current();
        const auto start_revision = history.revision();
        const auto check = [&](const ui::document& state, std::uint64_t expected_revision, std::string_view message) {
            require(history.revision() == expected_revision && history.current() == state &&
                history.committed() == state && editor.selection() == "dialog", message);
            const auto& props = state.nodes.at("dialog").props;
            const auto found = props.find(return_property);
            controls(true, found != props.end());
            auto* field = find_widget(vgui_root(shell), "property:modal_return_focus", VGUI_EDIT);
            const std::string_view target = found == props.end() ? std::string_view{} : std::get<std::string>(found->second);
            require(field && std::string_view(vgui_edit_get_text(field)) == target, "return focus inspector value is stale");
        };
        edit();
        check(expected, start_revision + 1, "return edit did not change just the modal target in one step");
        keys.activate(keys.open(1), 0); // Edit > Undo
        check(before, start_revision + 2, "one Undo did not restore the previous return target");
        keys.activate(keys.open(1), 1); // Edit > Redo
        check(expected, start_revision + 3, "one Redo did not restore the edited return target");
    };
    const auto test_run_return = [&](std::string_view expected) {
        const auto before = history.current();
        const auto before_revision = history.revision();
        input.click(mode);
        require(preview.running() && !node("dialog").visible, "Run did not start with a closed modal after the edit");
        controls(false, false);
        input.click_at(canvas, at("open"));
        require(node("dialog").visible, "the opener did not open the Run modal with the mouse");
        action("modal.inside [inside]");
        keys.key(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE);
        require(preview.running() && !node("dialog").visible, "Escape did not close the Run modal after the edit");
        action(expected); // Activate the restored focus without any intervening pointer input.
        require(history.revision() == before_revision && history.current() == before, "using the edited modal changed its design");
    };
    auto expected = doc;
    expected.nodes.at("dialog").props.erase(return_property);
    edit_return(expected, [&] { click_tool(clear); });
    test_run_return("modal.open [open]"); // With no explicit target, the opener gets focus back.
    input.click(mode);
    if (!vgui_widget_get_checked(links)) input.click(links);
    select_dialog();
    controls(true, false);

    const auto assign = [&](std::string_view id) {
        expected.nodes.at("dialog").props[return_property] = std::string(id);
        const bool had_target = history.current().nodes.at("dialog").props.contains(return_property);
        edit_return(expected, [&] {
            click_tool(pick);
            controls(true, had_target, true);
            input.click_at(canvas, at(id));
        });
    };
    assign("return_to");
    assign("new_return");
    editor.request(command::save);
    frame();
    ui::document saved;
    require(ui::read_document(file, saved) && saved == expected && !history.dirty(),
        "the edited return target did not survive saving");
    test_run_return("modal.new_return [new_return]");
}

void exercise_modal_open(os_window& win, vgui_context* shell, designer& editor,
                         const std::function<void()>& frame, const fs::path& sandbox) {
    const auto require = [](bool passed, std::string_view message) {
        if (!passed) throw std::runtime_error("Native modal open: " + std::string(message));
    };
    ui::document doc;
    require(ui::decode_json(R"({"format":"vgui-ui-design","version":1,"root":"root","nodes":{
        "root":{"type":"panel","props":{"nav_start":"opener"},"children":["opener","plain","first","second"]},
        "opener":{"type":"button","props":{"x":16,"y":20,"width":120,"height":36,"text":"Open","open_modal":"first"}},
        "plain":{"type":"button","props":{"x":154,"y":20,"width":120,"height":36,"text":"Ordinary button"}},
        "first":{"type":"panel","props":{"modal":true,"modal_return_focus":"opener","x":40,"y":80,"width":220,"height":110}},
        "second":{"type":"panel","props":{"modal":true,"x":300,"y":80,"width":220,"height":110},"children":["group"]},
        "group":{"type":"panel","props":{"x":12,"y":16,"width":190,"height":74},"children":["caption","inside"]},
        "caption":{"type":"label","props":{"x":10,"y":4,"width":130,"height":20,"text":"Second dialog"}},
        "inside":{"type":"button","props":{"x":10,"y":32,"width":130,"height":32,"text":"Close","close_modal":true}}
    }})", doc), "decode the modal opener fixture");
    const auto file = sandbox / "modal-open.vgui.json";
    require(ui::write_document(file, doc) && editor.open(file), "open the modal opener fixture");
    frame();

    auto& preview = editor.preview();
    const auto& history = editor.history();
    const auto revision = history.revision();
    const native_keyframe_input input{win, shell, preview, frame, require};
    const ui::native_menu_input keys(win, shell, frame);
    auto* canvas = input.widget("designer.preview.canvas");
    auto* links = input.widget("designer.preview.navigation_edit");
    const auto button = [&](const char* id) {
        auto* result = find_widget(preview.widget(), id, VGUI_BUTTON);
        require(result != nullptr, id);
        return result;
    };
    auto* pick = button("designer.preview.navigation_open");
    auto* clear = button("designer.preview.navigation_open.clear");
    auto* return_pick = button("designer.preview.navigation_return");
    auto* return_clear = button("designer.preview.navigation_return.clear");
    auto* close = button("designer.preview.navigation_close");
    auto* bar = vgui_widget_get_parent(pick);
    require(bar && vgui_widget_get_parent(clear) == bar && vgui_widget_get_parent(return_pick) == bar &&
        vgui_widget_get_parent(return_clear) == bar && vgui_widget_get_parent(close) == bar,
        "Open, Return, and Close must share the contextual toolbar");
    const auto controls = [&](bool show_pick, bool show_clear, bool picking = false, bool show_return = false,
                              std::optional<bool> close_checked = std::nullopt) {
        const auto visible = [](vgui_widget* widget) { return vgui_widget_get_visible(widget) != 0; };
        require(visible(bar) == (show_pick || show_return || close_checked.has_value()) &&
            visible(pick) == show_pick && visible(clear) == show_clear &&
            visible(return_pick) == show_return && visible(return_clear) == show_return &&
            visible(close) == close_checked.has_value(), "wrong Open/Return/Close toolbar visibility");
        require(!show_pick || (vgui_widget_get_enabled(pick) && (vgui_widget_get_checked(pick) != 0) == picking),
            "Open picker is disabled or has the wrong checked state");
        require(!show_clear || vgui_widget_get_enabled(clear), "cannot clear the explicit opener target");
        require(!close_checked.has_value() ||
            (vgui_widget_get_enabled(close) && (vgui_widget_get_checked(close) != 0) == *close_checked),
            "Close toggle is disabled or has the wrong checked state");
    };
    const auto node = [&](std::string_view id) {
        const auto* layout = preview.snapshot();
        require(layout != nullptr, "missing modal opener layout");
        const auto it = std::ranges::find(layout->nodes, id, &ui::node_layout::id);
        require(it != layout->nodes.end(), id);
        return *it;
    };
    const auto project = [&](float x, float y) {
        const auto board = preview.artboard_rect_px();
        const auto* layout = preview.snapshot();
        require(board && layout && layout->pixel_width > 0 && layout->pixel_height > 0, "missing modal opener artboard");
        return glm::round(glm::vec2{board->x + x * board->width / layout->pixel_width,
            board->y + y * board->height / layout->pixel_height});
    };
    const auto click_node = [&](std::string_view id) {
        const auto item = node(id);
        require(item.visible, id);
        const auto r = item.visible_px;
        input.click_at(canvas, project(r.x + r.width / 2, r.y + r.height / 2));
    };
    const auto select = [&](std::string_view id) {
        click_node(id);
        require(editor.selection() == id, "could not select the expected Design node");
    };
    select("opener");
    require(!preview.running() && !vgui_widget_get_checked(links), "the new page must start in Design with navigation off");
    controls(false, false);
    input.click(links);
    require(vgui_widget_get_checked(links), "could not enable Design navigation");
    controls(true, true);
    select("inside");
    controls(false, false, false, false, true);
    select("caption");
    controls(false, false);
    select("first");
    controls(false, false, false, true);
    select("plain");
    controls(true, false);
    select("opener");

    const auto state_is = [&](const ui::document& expected, auto expected_revision, std::string_view selected = "opener") {
        return history.revision() == expected_revision && history.current() == expected &&
            history.committed() == expected && editor.selection() == selected;
    };
    const auto untouched = [&](std::string_view message) {
        require(state_is(doc, revision) && !history.dirty() && !history.can_undo() && !history.can_redo(), message);
    };
    input.click(pick);
    controls(true, true, true);
    untouched("starting a pick changed the design or opener selection");
    click_node("plain");
    controls(true, true, true);
    untouched("picking an ordinary control changed the design or opener selection");
    input.click_at(canvas, project(22, 120));
    controls(true, true, true);
    untouched("picking blank space changed the design or opener selection");
    keys.key(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE);
    controls(true, true);
    untouched("Escape changed the design or opener selection");

    constexpr auto open_property = "open_modal";
    const auto filter_properties = [&](const char* query) {
        auto* search = find_widget(vgui_root(shell), "__property_search", VGUI_EDIT);
        require(search != nullptr, "missing property filter");
        input.click(search);
        // The property search is shared across pages and selections; replace the prior query.
        for (bool down : {true, false}) {
            SDL_Event key{};
            key.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
            key.key.windowID = win.id;
            key.key.key = SDLK_A;
            key.key.scancode = SDL_SCANCODE_A;
            key.key.mod = SDL_KMOD_LCTRL;
            key.key.down = down;
            input.push(key);
        }
        SDL_Event text{};
        text.type = SDL_EVENT_TEXT_INPUT;
        text.text.windowID = win.id;
        text.text.text = query;
        input.push(text);
        frame();
        require(std::string_view(vgui_edit_get_text(search)) == query, "property filter did not replace the prior query");
    };
    filter_properties(open_property);
    const auto inspector_is = [&](const ui::document& state) {
        const auto& properties = state.nodes.at("opener").props;
        const auto found = properties.find(open_property);
        const std::string_view target = found == properties.end() ? std::string_view{} : std::get<std::string>(found->second);
        auto* field = find_widget(vgui_root(shell), "property:open_modal", VGUI_EDIT);
        require(field && std::string_view(vgui_edit_get_text(field)) == target, "open modal inspector value is stale");
    };
    auto expected = doc;
    auto& props = expected.nodes.at("opener").props;
    const auto edit_open = [&](const auto& edit) {
        const auto before = history.current();
        const auto start_revision = history.revision();
        edit();
        require(state_is(expected, start_revision + 1), "the edit must only change open_modal and keep the opener selected");
        inspector_is(expected);
        controls(true, props.contains(open_property));
        keys.activate(keys.open(1), 0); // Edit > Undo
        require(state_is(before, start_revision + 2), "one Undo did not restore the previous opener target");
        inspector_is(before);
        controls(true, before.nodes.at("opener").props.contains(open_property));
        keys.activate(keys.open(1), 1); // Edit > Redo
        require(state_is(expected, start_revision + 3), "one Redo did not restore the edited opener target");
        inspector_is(expected);
        controls(true, props.contains(open_property));
    };
    props.erase(open_property);
    edit_open([&] { input.click(clear); });
    input.click("designer.preview.mode");
    click_node("opener");
    require(preview.running() && !node("first").visible && !node("second").visible,
        "clearing the link must leave both modals closed when the opener is clicked");
    input.click("designer.preview.mode");
    controls(true, false);
    const auto assign = [&](const char* target, std::string_view hit) {
        props[open_property] = std::string(target);
        const bool had_target = history.current().nodes.at("opener").props.contains(open_property);
        edit_open([&] {
            input.click(pick);
            controls(true, had_target, true);
            click_node(hit);
        });
    };
    assign("first", "first"); // Pick the panel body itself.
    assign("second", "caption"); // A visible non-focusable grandchild resolves to its modal ancestor.
    const auto save_is = [&](const ui::document& state, std::string_view message) {
        editor.request(command::save);
        frame();
        ui::document saved;
        require(ui::read_document(file, saved) && saved == state && !history.dirty(), message);
    };
    save_is(expected, "the edited opener target did not survive saving");

    const auto test_run_close = [&](const ui::document& state, bool closes) {
        const auto start_revision = history.revision();
        const std::string selected(editor.selection());
        input.click("designer.preview.mode");
        require(preview.running() && !node("first").visible && !node("second").visible, "Run must start with both dialogs closed");
        controls(false, false);
        click_node("opener");
        require(!node("first").visible && node("second").visible && node("caption").visible,
            "the opener did not show only the newly bound Run dialog");
        require(state_is(state, start_revision, selected) && !history.dirty(),
            "using the opener in Run changed the design or selection");
        controls(false, false);
        click_node("inside");
        require(preview.running() && !node("first").visible &&
            node("second").visible == !closes && node("inside").visible == !closes,
            closes ? "clicking inside did not close the Run modal" : "the disabled close action dismissed the Run modal");
        if (!closes) {
            keys.key(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE);
            require(preview.running() && !node("second").visible && !node("inside").visible,
                "Escape did not dismiss the Run modal after clicking the disabled close action");
        }
        require(state_is(state, start_revision, selected) && !history.dirty(),
            "using the close button in Run changed the design or selection");
    };
    test_run_close(expected, true);
    input.click("designer.preview.mode");
    select("inside");
    controls(false, false, false, false, true);
    input.click(links);
    require(!vgui_widget_get_checked(links) && editor.selection() == "inside", "could not disable navigation for the selected close button");
    controls(false, false);
    input.click(links);

    constexpr auto close_property = "close_modal";
    filter_properties(close_property);
    const auto close_is = [&](const ui::document& state, auto expected_revision, bool checked, std::string_view message) {
        require(!preview.running() && !vgui_modal_active(shell) && state_is(state, expected_revision, "inside"), message);
        controls(false, false, false, false, checked);
        auto* field = find_widget(vgui_root(shell), "property:close_modal", VGUI_CHECKBOX);
        require(field && (vgui_widget_get_checked(field) != 0) == checked, "close modal inspector value is stale");
    };
    auto without_close = expected;
    without_close.nodes.at("inside").props.erase(close_property);
    const auto close_revision = history.revision();
    close_is(expected, close_revision, true, "the authored close action must be enabled");
    input.click(close);
    close_is(without_close, close_revision + 1, false, "the toggle must remove only close_modal in one step");
    keys.activate(keys.open(1), 0); // Edit > Undo
    close_is(expected, close_revision + 2, true, "one Undo did not restore the close action");
    keys.activate(keys.open(1), 1); // Edit > Redo
    close_is(without_close, close_revision + 3, false, "one Redo did not remove the close action");
    save_is(without_close, "saving a disabled close action did not preserve the absent default property");
    test_run_close(without_close, false);
    input.click("designer.preview.mode");
    close_is(without_close, close_revision + 3, false, "returning to Design changed the close action or selection");
    input.click(close);
    close_is(expected, close_revision + 4, true, "the toggle must restore only close_modal in one step");
    save_is(expected, "the re-enabled close action did not survive saving");
    test_run_close(expected, true);
}

void exercise_keyframe_capture(os_window& win, vgui_context* shell, designer& editor,
                               const std::function<void()>& frame, const fs::path& sandbox) {
    const auto require = [](bool passed, std::string_view message) {
        if (!passed) throw std::runtime_error("Native keyframe capture: " + std::string(message));
    };
    constexpr auto track_property = "gradient_animation_keyframes";
    const auto read_keys = [&](const ui::document& doc, const char* node) {
        std::vector<ui::gradient_keyframe> keys;
        std::string error;
        require(ui::decode_gradient_keyframes(std::get<std::string>(doc.nodes.at(node).props.at(track_property)),
            keys, error), "decode the gradient track");
        return keys;
    };
    ui::document fixture;
    require(ui::read_document(VGUI_SOURCE_DIR "/sample/res/gradient-geometry.vgui.json", fixture), "load geometry example");
    auto& linear = fixture.nodes.at("linear").props;
    linear["gradient_x0"] = .125;
    linear["gradient_y0"] = linear["gradient_y1"] = .25;
    linear["gradient_x1"] = .875;
    const ui::gradient_keyframe first{.x = .125, .y = -.5, .tint = "#203041", .opacity = .25, .interpolation = "smooth",
        .geometry = {0., {}, 1.125, {}, .5}, // Y inherits; center_x is dormant on a linear track.
        .stops = {{0, "#FF0000"}, {.25f, "#FF000000"}, {1, "#DDEEFF"}}};
    const ui::gradient_keyframe last{.offset = 1, .x = .625, .y = .25, .tint = "#617082", .opacity = .75,
        .geometry = {.75, .75, .375, .75}, .stops = {{0, "#0000FF"}, {.75f, "#0000FF80"}, {1, "#DDEEFF"}}};
    linear[track_property] = ui::encode_gradient_keyframes(std::array{first, last});

    // Use the longer radial lane's diamonds as exact seek targets, avoiding clock or subpixel scrubber rounding.
    auto markers = read_keys(fixture, "radial");
    require(markers.size() == 3, "expected three seek-marker keys");
    markers[0].offset = .25;
    markers[1].offset = .375;
    markers[2].offset = .875;
    auto& radial = fixture.nodes.at("radial").props;
    radial["gradient_animation_duration"] = 8.;
    radial[track_property] = ui::encode_gradient_keyframes(markers);
    auto& sweep = fixture.nodes.at("sweep").props;
    sweep.erase(track_property);
    sweep["gradient_center_x"] = .25;
    sweep["gradient_center_y"] = .375;
    sweep["gradient_angle0"] = 45.;
    sweep["gradient_angle1"] = 270.;
    std::vector<ui::gradient_stop> sweep_stops;
    std::string error;
    require(ui::decode_gradient_stops(std::get<std::string>(sweep.at("gradient_stops")), sweep_stops, error),
        "decode the empty lane's authored palette");
    const auto file = sandbox / "capture-gradient-pose.vgui.json";
    require(ui::write_document(file, fixture) && editor.open(file), "open the capture fixture");
    frame();

    const auto& history = editor.history();
    const auto before = history.current();
    const auto revision = history.revision();
    const auto outline = std::string(editor.selection());
    auto& preview = editor.preview();
    const native_keyframe_input input{win, shell, preview, frame, require};
    input.click("designer.preview.timeline");
    input.click("designer.timeline.fit");
    auto* tracks = input.widget("designer.timeline.tracks");
    constexpr float linear_row = 37, radial_row = 63, sweep_row = 89;
    const auto select_lane = [&](float row) {
        const auto r = input.rect(tracks);
        input.click_at(tracks, {r.x + 12 * vgui_context_get_ui_scale(shell), input.fitted_key(tracks, 0, row).y});
    };
    const auto paused_at = [&](double time) {
        const auto view = preview.capture_view();
        return view.timeline_open && view.running && !view.animation.playing && view.animation.time == time;
    };
    const auto seek_linear = [&](std::size_t marker) {
        const auto offset = markers[marker].offset;
        input.click_at(tracks, input.fitted_key(tracks, static_cast<float>(offset), radial_row));
        select_lane(linear_row);
        require(paused_at(offset * 8) && !vgui_widget_get_enabled(input.widget("designer.timeline.delete")),
            "select the linear lane without moving the exact paused playhead or retaining the radial key selection");
    };
    constexpr char capture[] = "designer.timeline.capture";
    seek_linear(0);
    require(outline != "linear" && history.current() == before && history.revision() == revision && !history.can_undo(),
        "setting up the playhead and lane must not select the linear outline node or edit history");
    input.click(capture);
    // The first stop blends in linear light; the second blends with premultiplied alpha.
    // Neither should include the separate tint or opacity of the sampled pose.
    const ui::gradient_keyframe midpoint{.offset = .5, .x = .375, .y = -.125, .tint = "#415062", .opacity = .5,
        .interpolation = "smooth", .geometry = {.375, .5, .75, .5},
        .stops = {{0, "#BC00BCFF"}, {.5f, "#0000FF40"}, {1, "#DDEEFFFF"}}};
    auto expected = before;
    expected.nodes.at("linear").props[track_property] = ui::encode_gradient_keyframes(std::array{first, midpoint, last});
    require(history.current() == expected && history.revision() == revision + 1 && history.dirty() && history.can_undo() &&
        !history.can_redo(), "capture writes exactly the sampled key, including all active geometry and untinted stops, in one edit");
    require(paused_at(2) && editor.selection() == outline && vgui_widget_get_enabled(input.widget("designer.timeline.delete")) &&
        std::string_view(vgui_widget_get_text(input.widget("designer.timeline.tint"))) == midpoint.tint,
        "capture selects the new key at the paused playhead without changing the outline selection");

    auto* status = find_widget(vgui_root(shell), "designer.status");
    require(status != nullptr, "missing workbench status");
    input.click(capture);
    require(history.current() == expected && history.revision() == revision + 1 &&
        std::string_view(vgui_widget_get_text(status)) == editor.strings().text("timeline.capture_overlap", ""),
        "a second capture at the same time reports overlap without replacing the key or adding history");
    const ui::native_menu_input menus(win, shell, frame);
    menus.activate(menus.open(1), 0); // Edit > Undo
    require(history.current() == before && !history.dirty() && !history.can_undo() && history.can_redo(),
        "one Undo removes the capture");
    menus.activate(menus.open(1), 1); // Edit > Redo
    require(history.current() == expected && history.dirty() && history.can_undo() && !history.can_redo(),
        "Redo restores the exact captured pose");
    seek_linear(2); // 7s on an 8s axis; wrapping the linear track would incorrectly insert at 3s.
    const auto outside_revision = history.revision();
    input.click(capture);
    require(history.current() == expected && history.revision() == outside_revision && paused_at(7) &&
        std::string_view(vgui_widget_get_text(status)) == editor.strings().text("timeline.capture_outside", ""),
        "a playhead beyond the first cycle reports rejection and leaves the document, history and playhead unchanged");

    seek_linear(1); // At 3s the two interpolated endpoints coincide; preview falls back to authored geometry.
    input.click(capture);
    const auto fallback = read_keys(history.current(), "linear");
    const decltype(first.geometry) base_geometry{.125, .25, .875, .25};
    require(fallback.size() == 4 && fallback[2].offset == .75 && fallback[2].geometry == base_geometry &&
        fallback[2].x == .5 && fallback[2].y == .0625 && fallback[2].opacity == .625 && fallback[2].interpolation == "smooth",
        "invalid sampled geometry uses authored endpoints without discarding the rest of the sampled pose");
    select_lane(sweep_row);
    input.click(capture);
    const ui::gradient_keyframe authored{.offset = .75, .geometry = {{{}, {}, {}, {}, .25, .375, {}, {}, 45., 270.}},
        .stops = sweep_stops};
    require(paused_at(3) && read_keys(history.current(), "sweep") == std::vector{authored},
        "capturing the first key on an empty lane materializes the authored palette and active geometry");
}

void exercise_keyframe_endpoint_drag(os_window& win, vgui_context* shell, designer& editor,
                                    const std::function<void()>& frame, const fs::path& sandbox) {
    const auto require = [](bool passed, std::string_view message) {
        if (!passed) throw std::runtime_error("Native keyframe endpoint drag: " + std::string(message));
    };
    constexpr auto track_property = "gradient_animation_keyframes";
    constexpr auto start_x = std::size_t(ui::gradient_geometry_axis::start_x);
    constexpr auto start_y = std::size_t(ui::gradient_geometry_axis::start_y);
    const auto read_keys = [&](const ui::document& doc) {
        std::vector<ui::gradient_keyframe> keys;
        std::string error;
        const auto& props = doc.nodes.at("linear").props;
        require(ui::decode_gradient_keyframes(std::get<std::string>(props.at(track_property)), keys, error), "decode linear track");
        return keys;
    };
    ui::document fixture;
    require(ui::read_document(VGUI_SOURCE_DIR "/sample/res/gradient-geometry.vgui.json", fixture), "load geometry example");
    for (const auto* node : {"radial", "sweep"}) fixture.nodes.at(node).props.erase("gradient_animation");
    auto keys = read_keys(fixture);
    require(keys.size() == 3 && keys[1].offset == .5 && keys[1].geometry[start_x], "expected the middle linear key");
    auto& middle = keys[1];
    middle.x = .20;
    middle.y = -.18;
    middle.geometry[start_y].reset();
    // Keep X motion on one authored axis, despite the nonidentity scale and shear.
    constexpr double xx = .8, xy = .25, yy = .875, tx = .05, ty = .12, inherited_y = .25;
    auto& props = fixture.nodes.at("linear").props;
    props["gradient_affine_xx"] = xx;
    props["gradient_affine_xy"] = xy;
    props["gradient_affine_yy"] = yy;
    props["gradient_affine_tx"] = tx;
    props["gradient_affine_ty"] = ty;
    props["gradient_y0"] = inherited_y;
    props[track_property] = ui::encode_gradient_keyframes(keys);
    const auto file = sandbox / "linear-endpoint-motion.vgui.json";
    require(ui::write_document(file, fixture) && editor.open(file), "open the isolated linear track");
    frame();

    const auto& history = editor.history();
    const auto before = history.current();
    const auto revision = history.revision();
    const auto unchanged = [&](std::string_view message) {
        require(history.current() == before && history.revision() == revision &&
            !history.dirty() && !history.can_undo() && !history.can_redo(), message);
    };
    auto& preview = editor.preview();
    const native_keyframe_input input{win, shell, preview, frame, require};
    input.click("designer.preview.timeline");
    input.click("designer.timeline.fit");
    auto* tracks = input.widget("designer.timeline.tracks");
    const auto key_at = input.fitted_key(tracks);
    input.click_at(tracks, key_at);
    const auto view = preview.capture_view();
    require(view.timeline_open && view.running && !view.animation.playing && view.animation.time == 2.0 &&
        vgui_widget_get_enabled(input.widget("designer.timeline.geometry")) && editor.selection() != "linear",
        "select the middle key and pause Run at its exact time with a different outline selection");
    unchanged("selecting a timeline key edited the document");

    const auto board = preview.artboard_rect_px();
    const auto* layout = preview.snapshot();
    require(board && layout && layout->pixel_width > 0 && layout->pixel_height > 0, "missing presented board or layout");
    const auto node = std::ranges::find(layout->nodes, "linear", &ui::node_layout::id);
    require(node != layout->nodes.end() && node->visible, "linear gradient is not on the board");
    const auto& box = node->rect_px;
    const glm::vec2 board_scale{board->width / layout->pixel_width, board->height / layout->pixel_height};
    const auto widget_pixels = glm::vec2{box.width, box.height} * board_scale;
    const glm::vec2 painted_start{float(xx * *middle.geometry[start_x] + xy * inherited_y + tx + middle.x),
        float(yy * inherited_y + ty + middle.y)};
    const auto center = glm::vec2{board->x, board->y} +
        glm::vec2{box.x, box.y} * board_scale + widget_pixels * painted_start;
    const glm::vec2 start{std::round(center.x) + 3, std::round(center.y)}; // Deliberately grab off-center.
    const auto finish = start + glm::vec2{std::round(widget_pixels.x * float(xx) * .22f), 0};
    require(vgui_hit_test(shell, start.x, start.y) == input.widget("designer.preview.canvas") && finish.x - start.x > 10,
        "projected start handle is not reachable on the canvas");
    const auto move = [&](glm::vec2 to) {
        input.button(true, start);
        frame();
        unchanged("press changed the document or history");
        input.motion(start, to);
        frame();
        unchanged("motion edited the document before release");
    };
    const auto release = [&](glm::vec2 at) { input.button(false, at); frame(); };
    auto* seek = input.widget("designer.preview.animation.seek");
    const auto scrubber = input.rect(seek);
    input.click_at(seek,
        {scrubber.x + scrubber.width * .25f, scrubber.y + scrubber.height / 2});
    require(preview.capture_view().animation.time != 2.0, "scrub away from the selected key");
    move(finish);
    release(finish);
    unchanged("a handle edited the key while the playhead was away");
    input.click_at(tracks, key_at);
    const auto paused = preview.capture_view().animation;
    require(paused.time == 2.0 && !paused.playing, "reselecting the same key returns to its exact pose");

    const auto sub_slop = start + glm::vec2{2, 0};
    move(sub_slop);
    release(sub_slop);
    unchanged("motion below the three-pixel slop created an edit");

    const ui::native_menu_input menus(win, shell, frame);
    move(finish);
    menus.key(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE);
    release(finish);
    unchanged("Escape did not cancel the drag through mouse-up");

    move(finish);
    SDL_Event lost{};
    lost.type = SDL_EVENT_WINDOW_FOCUS_LOST;
    lost.window.windowID = win.id;
    input.push(lost);
    frame();
    release(finish);
    unchanged("focus loss did not cancel the drag through mouse-up");

    move(finish);
    release(finish);
    const auto after = history.current();
    require(history.revision() == revision + 1 && history.dirty() && history.can_undo() && !history.can_redo(),
        "release did not create exactly one transaction");
    require(editor.selection() != "linear", "endpoint drag changed the outline selection to the gradient");
    const auto edited = read_keys(after);
    const double expected_x = *middle.geometry[start_x] + (finish.x - start.x) / (widget_pixels.x * xx);
    require(edited.size() == keys.size() && edited[1].geometry[start_x] &&
        std::abs(*edited[1].geometry[start_x] - expected_x) < .001,
        "start X did not follow the pointer delta in authored axes with its grab offset preserved");
    middle.geometry[start_x] = edited[1].geometry[start_x];
    auto expected = before;
    expected.nodes.at("linear").props[track_property] = ui::encode_gradient_keyframes(keys);
    require(after == expected, "drag changed anything other than the middle key's start X (including inherited axes)");
    menus.activate(menus.open(1), 0); // Edit > Undo
    require(history.current() == before && !history.dirty() && !history.can_undo() && history.can_redo(),
        "one undo did not restore the original document");
    menus.activate(menus.open(1), 1); // Edit > Redo
    require(history.current() == after && history.dirty() && history.can_undo() && !history.can_redo(),
        "redo did not restore the endpoint edit exactly");
}

void exercise_keyframe_stop_drag(os_window& win, vgui_context* shell, designer& editor,
                                 const std::function<void()>& frame, const fs::path& sandbox) {
    const auto require = [](bool passed, std::string_view message) {
        if (!passed) throw std::runtime_error("Native keyframe stop drag: " + std::string(message));
    };
    constexpr auto track_property = "gradient_animation_keyframes";
    const auto read_keys = [&](const ui::document& doc) {
        std::vector<ui::gradient_keyframe> keys;
        std::string error;
        require(ui::decode_gradient_keyframes(std::get<std::string>(doc.nodes.at("linear").props.at(track_property)),
            keys, error), "decode the linear track");
        return keys;
    };
    ui::document fixture;
    require(ui::read_document(VGUI_SOURCE_DIR "/sample/res/gradient-geometry.vgui.json", fixture), "load geometry example");
    fixture.nodes.at(fixture.root).children = {"linear"};
    std::erase_if(fixture.nodes, [&](const auto& entry) { return entry.first != fixture.root && entry.first != "linear"; });
    auto keys = read_keys(fixture);
    require(keys.size() == 3 && keys[1].offset == .5, "expected the middle linear key");
    constexpr glm::dvec2 from{.10, .20}, to{.78, .77};
    constexpr double xx = .86, yx = .30, xy = .23, yy = .82, tx = .04, ty = .13;
    auto& middle = keys[1];
    middle.x = .07;
    middle.y = -.12;
    middle.tint = "#B680D1";
    middle.opacity = .62;
    middle.geometry = {from.x, {}, to.x, to.y}; // Inherit start Y along with the palette.
    middle.stops.clear();
    const std::vector<ui::gradient_stop> authored{{0, "#132B55"}, {.25f, "#12A95480"}, {.5f, "#5DD9FA"},
        {.75f, "#FFBD7DC0"}, {1, "#4C49AD"}};
    auto& props = fixture.nodes.at("linear").props;
    props["x"] = 32.;
    props["y"] = 75.;
    props["width"] = 680.; // Keep the stops on the first row, even with the board fitted.
    props["height"] = 220.;
    props["gradient_y0"] = from.y;
    props["gradient_affine_xx"] = xx;
    props["gradient_affine_yx"] = yx;
    props["gradient_affine_xy"] = xy;
    props["gradient_affine_yy"] = yy;
    props["gradient_affine_tx"] = tx;
    props["gradient_affine_ty"] = ty;
    props["gradient_stops"] = ui::encode_gradient_stops(authored);
    props[track_property] = ui::encode_gradient_keyframes(keys);
    const auto file = sandbox / "linear-stop-motion.vgui.json";
    require(ui::write_document(file, fixture) && editor.open(file), "open the isolated linear track");
    frame();

    const auto& history = editor.history();
    const auto before = history.current();
    const auto revision = history.revision();
    const auto unchanged = [&](std::string_view message) {
        require(history.current() == before && history.revision() == revision &&
            !history.dirty() && !history.can_undo() && !history.can_redo(), message);
    };
    const std::string outline(editor.selection());
    auto& preview = editor.preview();
    const native_keyframe_input input{win, shell, preview, frame, require};
    input.click("designer.preview.timeline");
    input.click("designer.timeline.fit");
    auto* tracks = input.widget("designer.timeline.tracks");
    const auto select_middle = [&] {
        input.click_at(tracks, input.fitted_key(tracks));
        const auto view = preview.capture_view();
        require(view.timeline_open && view.running && !view.animation.playing && view.animation.time == 2.0 &&
            vgui_widget_get_enabled(input.widget("designer.timeline.geometry")) && editor.selection() == outline &&
            outline != "linear", "pause at the exact middle key without selecting the gradient in the outline");
    };
    select_middle();
    unchanged("selecting the timeline key edited the document");

    const auto board = preview.artboard_rect_px();
    const auto* layout = preview.snapshot();
    require(board && layout && layout->pixel_width > 0 && layout->pixel_height > 0, "missing presented board or layout");
    const auto node = std::ranges::find(layout->nodes, "linear", &ui::node_layout::id);
    require(node != layout->nodes.end() && node->visible, "linear gradient is not on the board");
    const auto& box = node->rect_px;
    const glm::dvec2 board_scale{double(board->width) / layout->pixel_width, double(board->height) / layout->pixel_height};
    const auto shell_size = board_scale * glm::dvec2{box.width, box.height};
    const auto origin = glm::dvec2{board->x, board->y} + board_scale * glm::dvec2{box.x, box.y};
    const glm::dmat2 affine{xx, yx, xy, yy};
    const auto start_at = origin + shell_size * (affine * from + glm::dvec2{tx + middle.x, ty + middle.y});
    const auto screen_axis = shell_size * (affine * (to - from));
    const double scale = vgui_context_get_ui_scale(shell);
    const auto normal = glm::normalize(glm::dvec2{screen_axis.y, -screen_axis.x});
    const auto grab_at = [&](float offset, double side = 1.) {
        const auto knob = start_at + screen_axis * double(offset) + normal * (side * 18. * scale);
        return glm::round(glm::vec2(knob)) + glm::vec2{3, 1}; // Deliberately grab off-center.
    };
    auto* canvas = input.widget("designer.preview.canvas");
    const ui::native_menu_input menus(win, shell, frame);
    const auto move = [&](glm::vec2 start, glm::vec2 finish) {
        require(vgui_hit_test(shell, start.x, start.y) == canvas, "projected stop handle is not reachable on the canvas");
        const auto prior = history.current();
        const auto drag_revision = history.revision();
        input.button(true, start);
        frame();
        require(history.current() == prior && history.revision() == drag_revision, "press edited the document");
        input.motion(start, finish);
        frame();
        require(history.current() == prior && history.revision() == drag_revision, "motion edited the document before release");
    };
    const auto release = [&](glm::vec2 at) { input.button(false, at); frame(); };
    constexpr std::size_t stop_index = 2;
    const auto expected_edit = [&](float offset) {
        auto edited = keys;
        edited[1].stops = authored;
        edited[1].stops[stop_index].offset = offset;
        auto expected = before;
        expected.nodes.at("linear").props[track_property] = ui::encode_gradient_keyframes(edited);
        return expected;
    };
    const auto edited_offset = [&] {
        const auto edited = read_keys(history.current());
        require(edited.size() == keys.size() && edited[1].stops.size() == authored.size(), "drag did not materialize the palette");
        return edited[1].stops[stop_index].offset;
    };
    const auto start = grab_at(authored[stop_index].offset);
    input.open_stop_picker(canvas, start, authored[stop_index]);
    unchanged("right-clicking the linear stop changed the document");
    input.type_stop_hex("#A436D8");
    unchanged("typing an RGB color changed history before confirmation");
    // The alpha gesture reads the picker's current RGB, not the text in the edit.
    input.adjust_stop_alpha(0xA436D8u);
    unchanged("adjusting alpha changed history before confirmation");
    input.close_stop_picker("designer.preview.stop_color.cancel");
    unchanged("Cancel changed the linear palette or history");
    input.open_stop_picker(canvas, start, authored[stop_index]);
    input.close_stop_picker("designer.preview.stop_color.ok");
    unchanged("confirming the original color materialized the inherited palette");
    require(editor.selection() == outline, "picking a stop changed the outline selection");

    // Include transverse motion so the result must use projection onto the shell axis.
    const auto delta = glm::round(glm::vec2(screen_axis * .09 + normal * (16. * scale)));
    const auto finish = start + delta;
    const auto sub_slop = start + glm::vec2{2, 0};
    move(start, sub_slop);
    release(sub_slop);
    unchanged("motion below three-pixel slop materialized the palette");
    move(start, finish);
    menus.key(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE);
    release(finish);
    unchanged("Escape did not cancel the palette draft through mouse-up");

    move(start, finish);
    release(finish);
    require(history.revision() == revision + 1 && history.dirty() && history.can_undo() && !history.can_redo(),
        "release did not create exactly one transaction");
    const auto offset = edited_offset();
    const double expected_offset = .5 + glm::dot(glm::dvec2(delta), screen_axis) / glm::dot(screen_axis, screen_axis);
    require(std::abs(offset - expected_offset) < .001, "stop did not follow the projected pointer delta from its off-center grab");
    const auto after = expected_edit(offset);
    require(history.current() == after && editor.selection() == outline,
        "drag changed more than one stop offset, altered inherited colors, or changed the outline selection");
    menus.activate(menus.open(1), 0); // Edit > Undo
    require(history.current() == before && !history.dirty() && !history.can_undo() && history.can_redo(),
        "one undo did not restore palette inheritance and the original document");
    menus.activate(menus.open(1), 1); // Edit > Redo
    require(history.current() == after && history.dirty() && history.can_undo() && !history.can_redo(),
        "redo did not restore the exact stop edit");

    select_middle();
    const auto clamp_revision = history.revision();
    const auto moved_grab = grab_at(offset);
    const auto past_neighbor = moved_grab - glm::round(glm::vec2(screen_axis * .44));
    move(moved_grab, past_neighbor);
    release(past_neighbor);
    const auto clamped = edited_offset();
    const float prev = authored[stop_index - 1].offset;
    // At .25, simply adding the float gap rounds down into the forbidden range.
    require(clamped - prev >= ui::gradient_stop_min_gap && std::nextafter(clamped, prev) - prev < ui::gradient_stop_min_gap,
        "stop did not clamp to the closest model-valid float above its neighbor");
    require(history.revision() == clamp_revision + 1 && history.current() == expected_edit(clamped),
        "clamping did not update only the dragged stop in one transaction");

    // At one legal gap from the previous stop the anchors visually coincide.
    // The moved stop's knob on the opposite side must still be independently grabbable.
    select_middle();
    const auto dense_revision = history.revision();
    const auto dense_grab = grab_at(clamped, -1.);
    const auto separate = glm::round(glm::vec2(screen_axis * .1));
    move(dense_grab, dense_grab + separate);
    release(dense_grab + separate);
    const auto spaced = edited_offset();
    const double expected_spaced = clamped + glm::dot(glm::dvec2(separate), screen_axis) / glm::dot(screen_axis, screen_axis);
    require(history.revision() == dense_revision + 1 && std::abs(spaced - expected_spaced) < .001 &&
        history.current() == expected_edit(spaced), "the staggered stop can be moved without selecting its near-coincident neighbor");
}

void exercise_keyframe_radial_drag(os_window& win, vgui_context* shell, designer& editor,
                                  const std::function<void()>& frame, const fs::path& sandbox) {
    const auto require = [](bool passed, std::string_view message) {
        if (!passed) throw std::runtime_error("Native keyframe radial drag: " + std::string(message));
    };
    constexpr auto track_property = "gradient_animation_keyframes";
    using axis = ui::gradient_geometry_axis;
    constexpr auto center_x = std::size_t(axis::center_x), center_y = std::size_t(axis::center_y);
    constexpr auto outer_x = std::size_t(axis::outer_x), outer_y = std::size_t(axis::outer_y);
    constexpr auto inner_radius = std::size_t(axis::inner_radius), outer_radius = std::size_t(axis::outer_radius);
    const auto read_keys = [&](const ui::document& doc) {
        std::vector<ui::gradient_keyframe> keys;
        std::string error;
        const auto& props = doc.nodes.at("radial").props;
        require(ui::decode_gradient_keyframes(std::get<std::string>(props.at(track_property)), keys, error), "decode radial track");
        return keys;
    };
    ui::document fixture;
    require(ui::read_document(VGUI_SOURCE_DIR "/sample/res/gradient-geometry.vgui.json", fixture), "load geometry example");
    for (const auto* node : {"linear", "sweep"}) fixture.nodes.at(node).props.erase("gradient_animation");
    auto keys = read_keys(fixture);
    require(keys.size() == 3 && keys[1].offset == .5, "expected the middle radial key");
    // The inner circle is outside the outer circle's paintable focal range.
    // Target the authored geometry, not the focal position clamped by OVG.
    constexpr glm::dvec2 inner{.35, .63}, outer{.62, .42};
    constexpr double r_inner = .23, r_outer = .48;
    constexpr double xx = .82, yx = .32, xy = .21, yy = .81, tx = .03, ty = .09;
    auto& middle = keys[1];
    middle.x = .10;
    middle.y = -.16;
    middle.geometry[center_x] = inner.x;
    middle.geometry[center_y].reset();
    middle.geometry[outer_x] = outer.x;
    middle.geometry[outer_y] = outer.y;
    middle.geometry[inner_radius].reset();
    middle.geometry[outer_radius] = r_outer;
    auto& props = fixture.nodes.at("radial").props;
    std::vector<ui::gradient_stop> inherited_stops;
    std::string error;
    require(middle.stops.empty() && ui::decode_gradient_stops(std::get<std::string>(props.at("gradient_stops")),
        inherited_stops, error) && inherited_stops.size() == 3, "expected three inherited radial stops");
    constexpr std::size_t stop_index = 1;
    inherited_stops[stop_index].offset = .5f; // Close enough to the first stop to use the opposite row.
    props["gradient_stops"] = ui::encode_gradient_stops(inherited_stops);
    props["gradient_center_y"] = inner.y;
    props["gradient_radius0"] = r_inner;
    props["gradient_ellipse"] = true;
    props["gradient_affine_xx"] = xx;
    props["gradient_affine_yx"] = yx;
    props["gradient_affine_xy"] = xy;
    props["gradient_affine_yy"] = yy;
    props["gradient_affine_tx"] = tx;
    props["gradient_affine_ty"] = ty;
    props[track_property] = ui::encode_gradient_keyframes(keys);
    const auto file = sandbox / "radial-handle-motion.vgui.json";
    require(ui::write_document(file, fixture) && editor.open(file), "open the isolated radial track");
    frame();

    const auto& history = editor.history();
    const auto before = history.current();
    const auto initial_revision = history.revision();
    const std::string outline_selection(editor.selection());
    auto& preview = editor.preview();
    const native_keyframe_input input{win, shell, preview, frame, require};
    input.click("designer.preview.timeline");
    input.click("designer.timeline.fit");
    auto* tracks = input.widget("designer.timeline.tracks");
    const auto select_middle = [&] {
        input.click_at(tracks, input.fitted_key(tracks));
        const auto view = preview.capture_view();
        require(view.timeline_open && view.running && !view.animation.playing && view.animation.time == 2.0 &&
            vgui_widget_get_enabled(input.widget("designer.timeline.geometry")) && editor.selection() == outline_selection &&
            outline_selection != "radial", "select the middle radial key and pause at its exact time without changing the outline");
    };
    select_middle();
    require(history.current() == before && history.revision() == initial_revision &&
        !history.dirty() && !history.can_undo() && !history.can_redo(), "selecting the key edited the document");

    const auto board = preview.artboard_rect_px();
    const auto* layout = preview.snapshot();
    require(board && layout && layout->pixel_width > 0 && layout->pixel_height > 0, "missing presented board or layout");
    const auto node = std::ranges::find(layout->nodes, "radial", &ui::node_layout::id);
    require(node != layout->nodes.end() && node->visible, "radial gradient is not on the board");
    const auto box = node->rect_px;
    require(box.width > box.height && box.height > 0, "radial fixture needs a non-square widget");
    const glm::dvec2 board_scale{double(board->width) / layout->pixel_width, double(board->height) / layout->pixel_height};
    const auto shell_size = board_scale * glm::dvec2{box.width, box.height};
    const glm::dmat2 affine{xx, yx, xy, yy};
    const auto origin = glm::dvec2{board->x, board->y} + board_scale * glm::dvec2{box.x, box.y} +
        shell_size * glm::dvec2{tx + middle.x, ty + middle.y};
    const auto project_delta = [&](glm::dvec2 delta) { return shell_size * (affine * delta); };
    const auto inner_at = origin + project_delta({2 * inner.x - outer.x, inner.y});
    const auto outer_at = origin + project_delta(outer);
    // Width is the long side. One authored radius unit spans the widget's height
    // in paint pixels, stretched 2× on X before applying the affine.
    const auto radius_axis = project_delta({2.0 * box.height / box.width, 0});
    struct drag_case { const char* name; std::size_t channel; glm::dvec2 anchor; glm::vec2 motion; };
    const drag_case cases[]{
        {"inner center", center_x, inner_at, {25, -16}},
        {"outer center", outer_x, outer_at, {-20, 12}},
        {"inner radius", inner_radius, inner_at - r_inner * radius_axis, {-18, 17}},
        {"outer radius", outer_radius, outer_at + r_outer * radius_axis, {22, -19}},
    };
    for (std::size_t i = 0; i < std::size(cases); ++i)
        for (std::size_t j = 0; j < i; ++j)
            require(glm::length(cases[i].anchor - cases[j].anchor) > 20, "authored knobs must be well separated");

    auto* canvas = input.widget("designer.preview.canvas");
    const ui::native_menu_input menus(win, shell, frame);
    for (const auto& test : cases) {
        const auto check = [&](bool passed, std::string_view message) {
            if (!passed) require(false, std::string(test.name) + ": " + std::string(message));
        };
        const auto revision = history.revision();
        const bool had_redo = history.can_redo();
        const auto unchanged = [&](std::string_view message) {
            check(history.current() == before && history.revision() == revision &&
                !history.dirty() && !history.can_undo() && history.can_redo() == had_redo, message);
        };
        const auto start = glm::round(glm::vec2(test.anchor)) + glm::vec2{2, -2}; // Preserve a nonzero grab offset.
        const auto finish = start + test.motion;
        check(vgui_hit_test(shell, start.x, start.y) == canvas, "authored knob is not reachable on the canvas");
        const auto move = [&](glm::vec2 to) {
            input.button(true, start);
            frame();
            unchanged("press changed the document or history");
            input.motion(start, to);
            frame();
            unchanged("motion edited the document before release");
        };
        const auto release = [&](glm::vec2 at) { input.button(false, at); frame(); };
        if (test.channel == inner_radius) {
            const auto sub_slop = start + glm::vec2{-2, 0};
            move(sub_slop);
            release(sub_slop);
            unchanged("motion below the three-pixel slop created an edit");
            move(finish);
            menus.key(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE);
            release(finish);
            unchanged("Escape did not cancel the draft through mouse-up");
        }
        move(finish);
        release(finish);
        check(history.revision() == revision + 1 && history.dirty() && history.can_undo() && !history.can_redo(),
            "release did not create exactly one transaction");
        check(editor.selection() == outline_selection, "drag changed the outline selection");
        const auto edited = read_keys(history.current());
        check(edited.size() == keys.size(), "drag changed the number of keys");
        auto expected_keys = keys;
        const auto expect_axis = [&](std::size_t channel, double value) {
            const auto actual = edited[1].geometry[channel];
            check(actual && std::abs(*actual - value) < .001, "edited channel did not follow the pointer delta in authored units");
            expected_keys[1].geometry[channel] = actual;
        };
        const auto delta = glm::dvec2(test.motion);
        if (test.channel == center_x || test.channel == outer_x) {
            const auto paint = glm::inverse(affine) * (delta / shell_size);
            const bool is_inner = test.channel == center_x;
            const auto center = is_inner ? inner : outer;
            expect_axis(test.channel, center.x + paint.x / (is_inner ? 2 : 1));
            expect_axis(test.channel + 1, center.y + paint.y);
        } else {
            const bool is_inner = test.channel == inner_radius;
            // The diagonal motion has a transverse component: only its screen-space
            // projection along the signed radius axis changes the radius.
            const auto signed_axis = is_inner ? -radius_axis : radius_axis;
            expect_axis(test.channel, (is_inner ? r_inner : r_outer) + glm::dot(delta, signed_axis) / glm::dot(signed_axis, signed_axis));
        }
        auto expected = before;
        expected.nodes.at("radial").props[track_property] = ui::encode_gradient_keyframes(expected_keys);
        check(history.current() == expected, "drag changed unrelated channels, keys, or document properties");
        menus.activate(menus.open(1), 0); // Edit > Undo
        check(history.current() == before && !history.dirty() && !history.can_undo() && history.can_redo(),
            "one undo did not restore the original document (including inherited channels)");
        select_middle();
    }

    // Unlike the authored geometry knobs, the stop spoke follows the effective
    // paint circles: OVG pulls the focal center inside the outer circle in paint pixels.
    const glm::dvec2 paint_size{box.width, box.height};
    const double short_side = std::min(paint_size.x, paint_size.y);
    const double outer_px = std::max(2., r_outer * short_side);
    const double inner_px = std::min(r_inner * short_side, outer_px - 1.01);
    const auto outer_center = outer * paint_size;
    const auto focal_delta = inner * paint_size - outer_center;
    const double focal_limit = outer_px - inner_px - 1.;
    require(glm::length(focal_delta) > focal_limit, "radial stop fixture must exercise focal clamping");
    const auto focal_center = outer_center + glm::normalize(focal_delta) * focal_limit;
    const auto upper_spoke = [&](glm::dvec2 center, double radius) {
        const glm::dvec2 at{2 * center.x - outer_center.x, center.y - radius};
        return origin + project_delta(at / paint_size);
    };
    const auto spoke_start = upper_spoke(focal_center, inner_px);
    const auto spoke_axis = upper_spoke(outer_center, outer_px) - spoke_start;
    const auto anchor_at = [&](float offset) { return spoke_start + spoke_axis * double(offset); };
    for (const auto& stop : inherited_stops) {
        const auto p = glm::vec2(anchor_at(stop.offset));
        require(p.x >= board->x && p.x < board->x + board->width && p.y >= board->y && p.y < board->y + board->height &&
            vgui_hit_test(shell, p.x, p.y) == canvas, "all upper-spoke anchors must be visible");
    }
    const double scale = vgui_context_get_ui_scale(shell);
    const auto normal = glm::normalize(glm::dvec2{spoke_axis.y, -spoke_axis.x});
    const auto middle_anchor = anchor_at(inherited_stops[stop_index].offset);
    const double gap = 12. * scale + 6.;
    require(glm::length(middle_anchor - anchor_at(inherited_stops.front().offset)) < gap,
        "the two upper-spoke swatches must touch the same row's hit spacing");
    const auto knob = middle_anchor - normal * (18. * scale); // The first stop occupies the +normal row.
    const auto start = glm::round(glm::vec2(knob)) + glm::vec2{2, -2}; // Grab off-center, with a component along the spoke.
    const auto delta = glm::round(glm::vec2(spoke_axis * .16 + normal * (16. * scale)));
    const auto finish = start + delta;
    require(vgui_hit_test(shell, start.x, start.y) == canvas, "radial stop swatch is not reachable on the canvas");
    const auto revision = history.revision();
    const auto unchanged = [&](std::string_view message) {
        require(history.current() == before && history.revision() == revision &&
            !history.dirty() && !history.can_undo() && history.can_redo(), message);
    };
    const auto move_stop = [&] {
        input.button(true, start);
        input.motion(start, finish);
        frame();
        unchanged("radial stop motion edited the document before release");
    };
    const auto release_stop = [&] { input.button(false, finish); frame(); };
    move_stop();
    menus.key(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE);
    release_stop();
    unchanged("Escape did not cancel the radial stop draft through mouse-up");

    move_stop();
    release_stop();
    require(history.revision() == revision + 1 && history.dirty() && history.can_undo() && !history.can_redo(),
        "radial stop release did not create exactly one transaction");
    const auto edited = read_keys(history.current());
    require(edited.size() == keys.size() && edited[1].stops.size() == inherited_stops.size(),
        "radial stop drag did not materialize the inherited palette");
    const auto offset = edited[1].stops[stop_index].offset;
    const double expected_offset = inherited_stops[stop_index].offset +
        glm::dot(glm::dvec2(delta), spoke_axis) / glm::dot(spoke_axis, spoke_axis);
    require(std::abs(offset - expected_offset) < .001,
        "radial stop did not follow the projected pointer delta along the effective paint spoke");
    auto expected_keys = keys;
    expected_keys[1].stops = inherited_stops;
    expected_keys[1].stops[stop_index].offset = offset;
    auto expected = before;
    expected.nodes.at("radial").props[track_property] = ui::encode_gradient_keyframes(expected_keys);
    require(history.current() == expected && editor.selection() == outline_selection,
        "radial stop drag changed other offsets, colors, geometry, or the outline selection");
    menus.activate(menus.open(1), 0); // Edit > Undo
    require(history.current() == before && !history.dirty() && !history.can_undo() && history.can_redo(),
        "one undo did not restore radial palette inheritance and the original document");

    select_middle();
    const auto color_revision = history.revision();
    const auto color_unchanged = [&](std::string_view message) {
        require(history.current() == before && history.revision() == color_revision &&
            !history.dirty() && !history.can_undo() && history.can_redo(), message);
    };
    input.open_stop_picker(canvas, start, inherited_stops[stop_index]);
    color_unchanged("right-clicking the radial stop changed history");
    constexpr auto rgba = "#2975BC48";
    input.type_stop_hex(rgba);
    color_unchanged("typing an RGBA color changed history before confirmation");
    input.close_stop_picker("designer.preview.stop_color.ok");
    expected_keys[1].stops = inherited_stops;
    expected_keys[1].stops[stop_index].color = rgba;
    expected.nodes.at("radial").props[track_property] = ui::encode_gradient_keyframes(expected_keys);
    require(history.current() == expected && history.revision() == color_revision + 1 && history.dirty() &&
        history.can_undo() && !history.can_redo() && editor.selection() == outline_selection,
        "confirming an RGBA color without Enter did not change exactly one stop color in one transaction");
    menus.activate(menus.open(1), 0); // Edit > Undo
    require(history.current() == before && !history.dirty() && !history.can_undo() && history.can_redo(),
        "one undo did not restore the original color and radial palette inheritance");
}

void exercise_keyframe_sweep_drag(os_window& win, vgui_context* shell, designer& editor,
                                  const std::function<void()>& frame, const fs::path& sandbox) {
    const auto require = [](bool passed, std::string_view message) {
        if (!passed) throw std::runtime_error("Native keyframe sweep drag: " + std::string(message));
    };
    constexpr auto track_property = "gradient_animation_keyframes";
    using axis = ui::gradient_geometry_axis;
    constexpr auto center_x = std::size_t(axis::center_x), center_y = std::size_t(axis::center_y);
    constexpr auto start_angle = std::size_t(axis::start_angle), end_angle = std::size_t(axis::end_angle);
    const auto read_keys = [&](const ui::document& doc) {
        std::vector<ui::gradient_keyframe> keys;
        std::string error;
        const auto& props = doc.nodes.at("sweep").props;
        require(ui::decode_gradient_keyframes(std::get<std::string>(props.at(track_property)), keys, error), "decode sweep track");
        return keys;
    };
    ui::document fixture;
    require(ui::read_document(VGUI_SOURCE_DIR "/sample/res/gradient-geometry.vgui.json", fixture), "load geometry example");
    for (const auto* node : {"linear", "radial"}) fixture.nodes.at(node).props.erase("gradient_animation");
    auto keys = read_keys(fixture);
    require(keys.size() == 3 && keys.front().offset == 0. && keys[1].offset == .5, "expected the first and middle sweep keys");
    constexpr glm::dvec2 center{.43, .56};
    constexpr double start_degrees = 0., end_degrees = 180.;
    // Shear X while preserving the horizontal 0°/180° ray, so an actual
    // integer-pixel pointer can reach exact angle equality under any scale.
    constexpr double xx = .82, yx = 0., xy = .34, yy = .90, tx = -.09, ty = .11;
    auto& middle = keys[1];
    middle.x = .14;
    middle.y = -.12;
    middle.geometry[center_x] = center.x;
    middle.geometry[center_y].reset();
    middle.geometry[start_angle].reset();
    middle.geometry[end_angle] = end_degrees;
    constexpr double circle_start = 360., circle_end = 0.;
    auto& first = keys.front();
    first.x = middle.x;
    first.y = middle.y;
    first.geometry = middle.geometry;
    first.geometry[start_angle] = circle_start;
    first.geometry[end_angle] = circle_end;
    first.stops = {{0, "#123640"}, {.25f, "#44D2A8AD"}, {1, "#B95D7A"}};
    auto& props = fixture.nodes.at("sweep").props;
    props["height"] = 136.;
    props["gradient_center_y"] = center.y;
    props["gradient_angle0"] = start_degrees;
    props["gradient_affine_xx"] = xx;
    props["gradient_affine_yx"] = yx;
    props["gradient_affine_xy"] = xy;
    props["gradient_affine_yy"] = yy;
    props["gradient_affine_tx"] = tx;
    props["gradient_affine_ty"] = ty;
    props[track_property] = ui::encode_gradient_keyframes(keys);
    const auto file = sandbox / "sweep-handle-motion.vgui.json";
    require(ui::write_document(file, fixture) && editor.open(file), "open the isolated sweep track");
    frame();

    const auto& history = editor.history();
    const auto before = history.current();
    const auto initial_revision = history.revision();
    const std::string outline_selection(editor.selection());
    auto& preview = editor.preview();
    const native_keyframe_input input{win, shell, preview, frame, require};
    input.click("designer.preview.timeline");
    input.click("designer.timeline.fit");
    const auto select_key = [&](float offset) {
        auto* tracks = input.widget("designer.timeline.tracks");
        input.click_at(tracks, input.fitted_key(tracks, offset));
        const auto view = preview.capture_view();
        require(view.timeline_open && view.running && !view.animation.playing && view.animation.time == 4. * offset &&
            vgui_widget_get_enabled(input.widget("designer.timeline.geometry")) && editor.selection() == outline_selection &&
            outline_selection != "sweep", "select the sweep key and pause at its exact time without changing the outline");
    };
    select_key(.5f);
    require(history.current() == before && history.revision() == initial_revision &&
        !history.dirty() && !history.can_undo() && !history.can_redo(), "selecting the key edited the document");

    const auto board = preview.artboard_rect_px();
    const auto* layout = preview.snapshot();
    require(board && layout && layout->pixel_width > 0 && layout->pixel_height > 0, "missing presented board or layout");
    const auto node = std::ranges::find(layout->nodes, "sweep", &ui::node_layout::id);
    require(node != layout->nodes.end() && node->visible, "sweep gradient is not on the board");
    const auto box = node->rect_px;
    require(box.width > box.height && box.height > 0, "sweep fixture needs a non-square widget");
    const glm::dvec2 board_scale{double(board->width) / layout->pixel_width, double(board->height) / layout->pixel_height};
    const auto shell_size = board_scale * glm::dvec2{box.width, box.height};
    const glm::dmat2 affine{xx, yx, xy, yy};
    const auto center_at = glm::dvec2{board->x, board->y} + board_scale * glm::dvec2{box.x, box.y} +
        shell_size * (affine * center + glm::dvec2{tx + middle.x, ty + middle.y});
    // A sweep angle is a clockwise ray in paint pixels, not normalized widget coordinates.
    const glm::dmat2 paint_affine{xx, yx * box.height / box.width, xy * box.width / box.height, yy};
    const double radius = .42 * std::min(box.width, box.height);
    const auto spoke = [&](double degrees) {
        const auto angle = glm::radians(degrees);
        return board_scale * (paint_affine * (radius * glm::dvec2{std::cos(angle), std::sin(angle)}));
    };
    const auto start_spoke = spoke(start_degrees), end_spoke = spoke(end_degrees);
    const auto expected_angle = [&](double authored, glm::dvec2 delta) {
        // Native pointer positions are rounded to shell pixels before input.
        const auto ray = glm::inverse(paint_affine) * ((spoke(authored) + glm::round(delta)) / board_scale);
        const auto angle = glm::degrees(std::atan2(ray.y, ray.x));
        return angle < 0. ? angle + 360. : angle;
    };
    const double separation = 20 * vgui_context_get_ui_scale(shell);
    require(glm::length(start_spoke) > separation && glm::length(end_spoke) > separation &&
        glm::length(start_spoke - end_spoke) > separation, "authored knobs must be well separated");

    auto* canvas = input.widget("designer.preview.canvas");
    const ui::native_menu_input menus(win, shell, frame);
    const auto drag = [&](std::string_view name, glm::dvec2 anchor, std::initializer_list<glm::dvec2> deltas,
                          std::initializer_list<std::pair<std::size_t, double>> changed, bool cancel_first = false) {
        const auto check = [&](bool passed, std::string_view message) {
            if (!passed) require(false, std::string(name) + ": " + std::string(message));
        };
        const auto revision = history.revision();
        const bool had_redo = history.can_redo();
        const auto unchanged = [&](std::string_view message) {
            check(history.current() == before && history.revision() == revision &&
                !history.dirty() && !history.can_undo() && history.can_redo() == had_redo, message);
        };
        const auto grab = glm::round(glm::vec2(anchor)) + glm::vec2{3, -2}; // Preserve the off-center grab.
        check(vgui_hit_test(shell, grab.x, grab.y) == canvas, "authored knob is not reachable on the canvas");
        const auto move = [&] {
            input.button(true, grab);
            frame();
            unchanged("press changed the document or history");
            auto at = grab;
            for (const auto delta : deltas) {
                const auto next = grab + glm::vec2(delta);
                input.motion(at, next);
                frame();
                unchanged("motion edited the document before release");
                at = next;
            }
            return at;
        };
        const auto release = [&](glm::vec2 at) { input.button(false, at); frame(); };
        if (cancel_first) {
            const auto at = move();
            menus.key(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE);
            release(at);
            unchanged("Escape did not cancel the draft through mouse-up");
        }
        release(move());
        check(history.revision() == revision + 1 && history.dirty() && history.can_undo() && !history.can_redo(),
            "release did not create exactly one transaction");
        check(editor.selection() == outline_selection, "drag changed the outline selection");
        const auto edited = read_keys(history.current());
        check(edited.size() == keys.size(), "drag changed the number of keys");
        auto expected_keys = keys;
        for (const auto& [channel, value] : changed) {
            const auto actual = edited[1].geometry[channel];
            check(actual && (value == 360. ? *actual == value : std::abs(*actual - value) < .001),
                "edited channel did not reach the expected authored value");
            expected_keys[1].geometry[channel] = actual;
        }
        auto expected = before;
        expected.nodes.at("sweep").props[track_property] = ui::encode_gradient_keyframes(expected_keys);
        check(history.current() == expected, "drag changed unrelated channels, keys, or document properties");
        menus.activate(menus.open(1), 0); // Edit > Undo
        check(history.current() == before && !history.dirty() && !history.can_undo() && history.can_redo(),
            "one undo did not restore the original document (including inherited channels)");
    };
    const glm::dvec2 center_motion{22, -14};
    const auto moved_center = center + glm::inverse(affine) * (center_motion / shell_size);
    drag("center", center_at, {center_motion}, {{center_x, moved_center.x}, {center_y, moved_center.y}});
    select_key(.5f);
    // Reach exact equality with a horizontal integer-pixel move. The last
    // accepted preview is still committed (without changing the opposite angle).
    const auto valid_move = spoke(56.) - start_spoke;
    const glm::dvec2 equality_move{-std::ceil(2 * start_spoke.x), 0.};
    drag("start angle", center_at + start_spoke, {valid_move, equality_move},
        {{start_angle, expected_angle(start_degrees, valid_move)}}, true);
    select_key(.5f);
    // Cross 360° and continue clockwise by more than half a turn from the grab.
    // The final pointer ray is 205°, but consecutive deltas must keep the value at 360°.
    drag("end angle", center_at + end_spoke, {spoke(245.) - end_spoke, spoke(315.) - end_spoke,
        spoke(385.) - end_spoke, spoke(480.) - end_spoke, spoke(565.) - end_spoke}, {{end_angle, 360.}});

    select_key(0.f);
    require(vgui_widget_get_enabled(input.widget("designer.timeline.stops")), "first sweep key has no editable palette");
    constexpr std::size_t stop_index = 1;
    const auto original_offset = first.stops[stop_index].offset;
    const auto stop_spoke = spoke(std::lerp(circle_start, circle_end, double(original_offset)));
    // This stop is away from the seam handles and uses the first (outward) row.
    const double scale = vgui_context_get_ui_scale(shell);
    const auto knob = center_at + stop_spoke + glm::normalize(stop_spoke) * (18. * scale);
    const auto grab = glm::round(glm::vec2(knob)) + glm::vec2{3, -2};
    require(vgui_hit_test(shell, grab.x, grab.y) == canvas, "projected sweep stop swatch is not reachable on the canvas");
    const auto pointer_at = [&](double degrees) { return glm::round(glm::vec2(center_at + spoke(degrees))); };
    const auto finish = pointer_at(45.);
    const auto revision = history.revision();
    const auto unchanged = [&](std::string_view message) {
        require(history.current() == before && history.revision() == revision &&
            !history.dirty() && !history.can_undo() && history.can_redo(), message);
    };
    input.open_stop_picker(canvas, grab, first.stops[stop_index]);
    unchanged("right-clicking the sweep stop changed history");
    input.adjust_stop_alpha(0x44D2A8u);
    unchanged("adjusting the sweep stop's alpha changed history before confirmation");
    menus.key(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE);
    require(!vgui_popup_active(shell) && editor.selection() == outline_selection,
        "Escape did not dismiss the sweep picker without changing the outline selection");
    unchanged("Escape did not restore the original sweep stop");

    const auto move_stop = [&] {
        input.button(true, grab);
        auto at = grab;
        // From ~270°, travel counterclockwise past 0°, then unwind toward 45°.
        // Each motion is less than half a turn; the net motion exceeds half a turn.
        for (const auto next : {pointer_at(155.), pointer_at(40.), pointer_at(-25.), finish}) {
            input.motion(at, next);
            at = next;
        }
        frame();
        unchanged("sweep stop motion edited the document before release");
    };
    const auto release_stop = [&] { input.button(false, finish); frame(); };
    move_stop();
    menus.key(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE);
    release_stop();
    unchanged("Escape did not cancel the sweep stop draft through mouse-up");

    move_stop();
    release_stop();
    require(history.revision() == revision + 1 && history.dirty() && history.can_undo() && !history.can_redo(),
        "sweep stop release did not create exactly one transaction");
    const auto edited = read_keys(history.current());
    require(edited.size() == keys.size() && edited.front().stops.size() == first.stops.size(),
        "sweep stop drag changed the palette or track topology");
    const auto offset = edited.front().stops[stop_index].offset;
    const auto mouse_angle = [&](glm::vec2 p) {
        const auto ray = glm::inverse(paint_affine) * ((glm::dvec2(p) - center_at) / board_scale);
        return glm::degrees(std::atan2(ray.y, ray.x));
    };
    // atan2 reports the grab near -90°; the path above reaches +45° from 270°.
    const double angular_delta = mouse_angle(finish) - (mouse_angle(grab) + 360.);
    const double expected_offset = original_offset + angular_delta / (circle_end - circle_start);
    require(std::abs(offset - expected_offset) < .001,
        "sweep stop did not follow the accumulated paint-space angle from the off-center grab over a signed span");
    auto expected_keys = keys;
    expected_keys.front().stops[stop_index].offset = offset;
    auto expected = before;
    expected.nodes.at("sweep").props[track_property] = ui::encode_gradient_keyframes(expected_keys);
    require(history.current() == expected && editor.selection() == outline_selection,
        "sweep stop drag changed another offset, color, geometry channel, or the outline selection");
    menus.activate(menus.open(1), 0); // Edit > Undo
    require(history.current() == before && !history.dirty() && !history.can_undo() && history.can_redo(),
        "one undo did not restore the original sweep stop offset");
}

void exercise_keyframe_delivery(os_window& win, vgui_context* shell, designer& editor,
                                const std::function<void()>& frame, const fs::path& sandbox) {
    const auto require = [](bool passed, std::string_view message) {
        if (!passed) throw std::runtime_error("Native keyframe delivery: " + std::string(message));
    };
    const auto sample = fs::path(VGUI_SOURCE_DIR) / "sample/res/gradient-keyframes.vgui.json";
    const auto source = sandbox / "motion-a.vgui.json", other = sandbox / "motion-b.vgui.json";
    fs::copy_file(sample, source);
    fs::copy_file(sample, other);
    const auto& history = editor.history();
    auto* dock = find_widget(vgui_root(shell), "designer.dock", VGUI_DOCK);
    require(dock != nullptr, "missing document dock");
    const ui::native_page_dock_input input(win, shell, dock, frame, require);
    const ui::native_menu_input menus(win, shell, frame);
    const auto click = [&](const char* id, float fraction = .5f) {
        auto* widget = find_widget(vgui_popup_content(shell), id);
        if (!widget) widget = find_widget(editor.preview().widget(), id);
        require(widget != nullptr, id);
        float x, y, w, h;
        vgui_widget_get_rect(widget, &x, &y, &w, &h);
        input.click(widget, x + w * fraction, y + h / 2);
        return widget;
    };
    const auto submit = [&](const char* id, const char* value) {
        auto* edit = click(id);
        require(vgui_get_focus(shell) == edit, "numeric input did not gain focus");
        vgui_edit_select_all(edit);
        SDL_Event text{};
        text.type = SDL_EVENT_TEXT_INPUT;
        text.text.windowID = win.id;
        text.text.text = value;
        require(SDL_PushEvent(&text), "cannot enqueue pose or duration input");
        menus.key(SDLK_RETURN, SDL_SCANCODE_RETURN);
    };
    const auto read_keys = [&](const ui::document& doc, const char* node = "highlight") {
        std::vector<ui::gradient_keyframe> frames;
        std::string error;
        require(ui::decode_gradient_keyframes(std::get<std::string>(doc.nodes.at(node).props.at(
            "gradient_animation_keyframes")), frames, error), "decode gradient track");
        return frames;
    };
    require(editor.open(source), "open the first copy of the gradient example");
    frame();
    const auto original = history.current();
    const auto tab = vgui_dock_get_active(dock, VGUI_DOCK_CENTER);
    require(editor.preview_ready(false), "image-free example did not render");
    click("designer.preview.timeline");
    click("designer.preview.mode");
    click("designer.preview.animation.seek", .22f);
    click("designer.timeline.add");
    submit("designer.timeline.opacity_edit", ".5");
    constexpr auto start_x = std::size_t(ui::gradient_geometry_axis::start_x);
    constexpr char shape[] = "designer.timeline.geometry";
    constexpr char start_x_edit[] = "designer.timeline.geometry.start_x_edit";
    click(shape);
    auto* popup = vgui_popup_content(shell);
    require(popup && std::string_view(vgui_widget_get_id(popup)) == "designer.timeline.geometry.popup", "open the shape popup");
    auto* start = find_widget(popup, start_x_edit, VGUI_EDIT);
    require(start && std::string_view(vgui_edit_get_text(start)).empty(), "a legacy key inherits the original endpoint");
    submit(start_x_edit, ".12");
    require(read_keys(history.current())[1].geometry[start_x] == .12, "endpoint override did not reach the key");
    click("designer.timeline.geometry.start_x.reset");
    require(!read_keys(history.current())[1].geometry[start_x], "Reset did not restore the authored endpoint");
    submit(start_x_edit, ".12");
    vgui_popup_close(shell);
    frame();
    constexpr char stop_button[] = "designer.timeline.stops";
    constexpr char stop_editor[] = "designer.timeline.stops.editor";
    click(stop_button);
    popup = vgui_popup_content(shell);
    require(popup && find_widget(popup, stop_editor), "open the keyframe stop editor");
    for (const auto* id : {"designer.timeline.stops.editor.add", "designer.timeline.stops.editor.remove"}) {
        auto* topology_button = find_widget(popup, id);
        require(topology_button && !vgui_widget_get_visible(topology_button), "keyframe cannot add or remove stops");
    }
    require(read_keys(history.current())[1].stops.empty(), "opening does not materialize inherited stops");
    submit("designer.timeline.stops.editor.time_edit", ".10");
    submit("designer.timeline.stops.editor.picker.hex", "#FF884480");
    const auto palette = read_keys(history.current())[1].stops;
    require(palette.size() == 5 && std::abs(palette[0].offset - .10f) < 1e-6f &&
        ui::parse_text_color(palette[0].color) == 0x80FF8844u && palette[1].color == "#BFF8FF26",
        "changing a stop commits its offset and RGBA without changing neighboring colors");
    click("designer.timeline.stops.reset");
    require(read_keys(history.current())[1].stops.empty(), "reset removes the palette override");
    vgui_popup_close(shell);
    menus.activate(menus.open(1), 0); // Undo the reset.
    require(read_keys(history.current())[1].stops == palette, "Undo restores the whole keyframe palette");
    const auto created = read_keys(history.current());
    require(created.size() == 4 && created[1].opacity == .5 && created[1].geometry[start_x] == .12 && history.dirty(),
        "pose and shape did not reach the active document");
    const auto before_paste = history.current();
    click("designer.timeline.copy");

    require(editor.open(other), "open a second copy with identical node IDs");
    frame();
    click("designer.preview.timeline");
    auto* paste = find_widget(editor.preview().widget(), "designer.timeline.paste");
    require(paste && !vgui_widget_get_enabled(paste) && history.current() == original && !history.can_undo(),
        "clipboard or undo history leaked into another page");
    input.select_tab(tab);
    require(vgui_dock_get_active(dock, VGUI_DOCK_CENTER) == tab && history.current() == before_paste,
        "switching tabs lost the first page's animation edits");
    const auto before_paste_playhead = editor.preview().capture_view().animation.time;
    click("designer.preview.animation.seek");
    const auto anchor = editor.preview().capture_view().animation;
    require(!anchor.playing && anchor.time > 1.3 && anchor.time < 1.9 && anchor.time != before_paste_playhead,
        "the playback scrubber did not pause at a new position");
    click("designer.timeline.paste");
    const auto pasted = read_keys(history.current());
    require(pasted.size() == 5 && std::abs(pasted[3].offset * 2.4 - anchor.time) < .0001,
        "paste did not anchor the new key at the playhead");
    auto expected_pose = created[1];
    expected_pose.offset = pasted[3].offset;
    require(pasted[3] == expected_pose && std::vector{pasted[0], pasted[1], pasted[2], pasted[4]} == created,
        "paste must preserve the copied pose and all existing keys");
    const auto after_paste = history.current();
    menus.activate(menus.open(1), 0); // Edit > Undo
    require(history.current() == before_paste, "Undo did not remove the paste as a single transaction");
    menus.activate(menus.open(1), 1); // Edit > Redo
    require(history.current() == after_paste, "Redo did not restore the exact pose and time");
    submit("designer.timeline.duration_edit", "3.6");
    click("designer.timeline.loop");
    const auto edited = history.current();
    const auto& props = edited.nodes.at("highlight").props;
    require(props.at("gradient_animation_duration") == ui::value{3.6} &&
        props.at("gradient_animation_loop") == ui::value{false} && read_keys(edited) == pasted &&
        edited.nodes.at("tint") == original.nodes.at("tint"), "timing edits must preserve normalized poses and the other lane");

    menus.activate(menus.open(0), 2); // File > Save
    ui::document saved;
    require(!history.dirty() && history.can_undo() && ui::read_document(source, saved) && saved == edited,
        "saved JSON did not preserve the edited animation or its undo history");
    ui::document unchanged;
    require(ui::read_document(other, unchanged) && unchanged == original, "saving the animated tab changed another page's file");
    const auto directory = sandbox / "motion-delivery", single = sandbox / "motion.vgui.package.json";
    ui::document directory_design;
    ui::embedded_delivery embedded;
    require(ui::export_delivery(saved, sandbox, directory) && ui::read_delivery(directory, directory_design) &&
        directory_design == edited, "verified directory delivery did not preserve the edited track");
    require(ui::export_json_delivery(saved, sandbox, single) && ui::read_json_delivery(single, embedded) &&
        embedded.design == edited && embedded.images.empty(), "verified single-file delivery did not preserve the image-free animation");

    // Sample the actual highlight paint in a standalone host from each verified
    // package. The middle stop's authored alpha is 0xCF; the pasted pose halves it.
    std::unique_ptr<vgui_context, decltype(&vgui_destroy)> ctx(vgui_create(nullptr, 16), vgui_destroy);
    std::unique_ptr<ovg_ctx_cb, decltype(&free_ctx_cb)> cb(new_ctx_cb(), free_ctx_cb);
    require(bool(ctx) && bool(cb), "create host rendering context");
    std::unique_ptr<rvg_t, void (*)(rvg_t*)> canvas(cb->new_rvg(cb->ac), cb->destroy_rvg);
    require(bool(canvas), "create host recording canvas");
    canvas->width = 760;
    canvas->height = 370;
    static float highlight_alpha, highlight_start_x, highlight_first_stop;
    static glm::vec4 highlight_first_color;
    static const auto set_source = cb->set_source;
    cb->set_source = [](rvg_t* view, vg_pattern_t* paint) {
        if (paint && paint->type == VG_PATTERN_TYPE_LINEAR && paint->data) {
            const auto& fill = *static_cast<vg_gradient_t*>(paint->data);
            if (fill.count == 5) {
                highlight_alpha = fill.colors[2].w;
                highlight_start_x = fill.cp[0].x;
                highlight_first_stop = fill.stops[0];
                highlight_first_color = fill.colors[0];
            }
        }
        set_source(view, paint);
    };
    ui::runtime host(ctx.get());
    const auto expect_highlight = [&](double time, int alpha, std::optional<float> x = {}) {
        host.seek_animations(time);
        highlight_alpha = -1;
        highlight_start_x = -1;
        cb->clear(canvas.get());
        vgui_draw(ctx.get(), cb.get(), canvas.get());
        require(highlight_alpha == alpha / 255.f, "host did not draw the expected painted opacity");
        if (x) require(std::abs(highlight_start_x - *x) < .01f, "host did not draw the exported endpoint");
    };
    for (const auto* doc : {&directory_design, &embedded.design}) {
        require(host.mount(*doc, 1) && host.layout({760, 370, 1}), "mount the validated design without the workbench");
        const auto timing = host.animation_timing();
        require(timing.duration == 3.6 && !timing.loops, "runtime timing did not match the saved clip");
        const double pasted_time = pasted[3].offset * 3.6;
        expect_highlight(pasted_time, 104, 32 + 20 + 296 * .12f);
        require(std::abs(highlight_first_stop - .10f) < 1e-6f && highlight_first_color.x == 1.f &&
            highlight_first_color.y == 136 / 255.f && highlight_first_color.z == 68 / 255.f &&
            highlight_first_color.w == 64 / 255.f, "verified package hosts draw the keyed stop and apply pose opacity");
        expect_highlight(pasted_time + 3.6, 52); // Non-looping: hold the last key, not the pasted pose.
    }

    // Work from the sample, leaving only its radial track active so the
    // toolbar can add a key without relying on clock or lane pixel positions.
    ui::document radius_design;
    require(ui::read_document(sample.parent_path() / "gradient-geometry.vgui.json", radius_design), "load radial example");
    for (const auto* node : {"linear", "sweep"}) radius_design.nodes.at(node).props.erase("gradient_animation");
    const auto radial_file = sandbox / "radial-motion.vgui.json";
    require(ui::write_document(radial_file, radius_design) && editor.open(radial_file), "open isolated radial track");
    frame();
    click("designer.preview.timeline");
    click("designer.preview.mode");
    click("designer.preview.animation.seek", .22f);
    click("designer.timeline.add");
    auto expect_radii = [&](std::optional<double> inner, std::optional<double> outer) {
        constexpr auto inner_axis = std::size_t(ui::gradient_geometry_axis::inner_radius);
        constexpr auto outer_axis = std::size_t(ui::gradient_geometry_axis::outer_radius);
        const auto keys = read_keys(history.current(), "radial");
        require(keys.size() == 4 && keys[1].geometry[inner_axis] == inner && keys[1].geometry[outer_axis] == outer,
            "the newly added radial key did not have the expected radius overrides");
    };
    click(shape);
    constexpr char inner_edit[] = "designer.timeline.geometry.inner_radius_edit";
    constexpr char outer_edit[] = "designer.timeline.geometry.outer_radius_edit";
    expect_radii({}, {});
    submit(outer_edit, ".95");
    submit(inner_edit, ".35");
    expect_radii(.35, .95);
    const auto radius_revision = history.revision();
    submit(inner_edit, ".95");
    require(history.revision() == radius_revision, "equal radii must not create a history entry");
    expect_radii(.35, .95);
    if (!vgui_popup_content(shell)) click(shape);
    click("designer.timeline.geometry.inner_radius.reset");
    expect_radii({}, .95);
    vgui_popup_close(shell);
    menus.activate(menus.open(1), 0); // Edit > Undo
    expect_radii(.35, .95);
    menus.activate(menus.open(0), 2); // File > Save
    require(!history.dirty() && ui::read_document(radial_file, saved) && saved == history.current(),
        "saved JSON did not preserve the edited radial key");
    const auto radial_dir = sandbox / "radial-delivery", radial_single = sandbox / "radial.vgui.package.json";
    require(ui::export_delivery(saved, sandbox, radial_dir) && ui::read_delivery(radial_dir, directory_design) && directory_design == saved &&
        ui::export_json_delivery(saved, sandbox, radial_single) && ui::read_json_delivery(radial_single, embedded) && embedded.design == saved,
        "both delivery formats must preserve the radius overrides");
    exercise_keyframe_capture(win, shell, editor, frame, sandbox);
    exercise_keyframe_endpoint_drag(win, shell, editor, frame, sandbox);
    exercise_keyframe_stop_drag(win, shell, editor, frame, sandbox);
    exercise_keyframe_radial_drag(win, shell, editor, frame, sandbox);
    exercise_keyframe_sweep_drag(win, shell, editor, frame, sandbox);
}

fs::path designer_settings_path() {
    if (const char* override_path = SDL_getenv("VGUI_DESIGNER_SETTINGS"); override_path && *override_path)
        return fs::u8path(override_path);
    std::unique_ptr<char, decltype(&SDL_free)> pref(SDL_GetPrefPath("vgui", "ui_designer"), SDL_free);
    return pref ? fs::u8path(pref.get()) / "settings.json" : fs::path{};
}

enum class native_smoke { none, workbench, game_data, navigation, modal_return };

int run(native_smoke smoke, fs::path open_path, command native_dialog_exit = command::none) {
    const bool workbench_smoke = smoke == native_smoke::workbench;
    const bool testing = smoke != native_smoke::none;
    const bool test_environment = testing || native_dialog_exit != command::none;
    app_mgr app;
    app.msaa_samples = 8; // Request 8× before creating the window and its GPU context.
    if (!app.init_gpu(true)) {
        std::fprintf(stderr, "Cannot initialize SDL3 Vulkan: %s\n", SDL_GetError());
        return test_environment ? 77 : 1;
    }
    std::optional<smoke_file_picker> picker;
    fs::path absolute_fixture;
    if (testing) picker.emplace();
    if (workbench_smoke) {
        const fs::path fixtures = VGUI_SOURCE_DIR "/sample/res";
        open_path = picker->sandbox / "form.vgui.json";
        absolute_fixture = picker->sandbox / "absolute-form.vgui.json";
        // Save All may write both pages. Use scratch designs and attach a
        // generated image only to the test's copy of the example form.
        ui::document form;
        ui::diagnostic error;
        if (!ui::read_document(fixtures / open_path.filename(), form, &error))
            throw std::runtime_error("Cannot read the form smoke fixture: " + describe(error));
        form.nodes.emplace("mark", ui::node{"image", {}, {{"resource", std::string(ui::smoke_badge_key)},
            {"width", 32.0}, {"height", 24.0}}, {}});
        auto& footer = form.nodes.at("footer").children;
        footer.insert(footer.begin(), "mark");
        form.resources.emplace(ui::smoke_badge_key, ui::smoke_badge_file);
        if (!ui::write_smoke_badge(picker->sandbox / ui::smoke_badge_file))
            throw std::runtime_error("Cannot generate the image smoke fixture");
        if (!ui::write_document(open_path, form, &error))
            throw std::runtime_error("Cannot write the form smoke fixture: " + describe(error));
        fs::copy_file(fixtures / absolute_fixture.filename(), absolute_fixture);
    }
    const auto settings = testing ? picker->sandbox / "settings.json" :
        test_environment ? fs::path{} : designer_settings_path();
    if (workbench_smoke && (!ui::save_designer_dock_widths(settings, {280, 375}) ||
                  !ui::save_designer_options(settings, ui::designer_grid_settings{.spacing = 24}, ui::designer_theme::light)))
        throw std::runtime_error("Cannot seed custom workbench settings for the smoke test");
    bool invalid_settings = false;
    const ui::designer_strings strings(ui::load_designer_language(settings, &invalid_settings));
    // The inspector and private artboard context need identical coverage,
    // independent of the editor's selected language or installed OS fonts.
    const bool fonts_loaded = load_font_from_path(app.font_ctx, VGUI_SOURCE_DIR "/sample/res/fonts/DejaVuSans.ttf", "UIDesigner") &&
        load_font_from_path(app.font_ctx, VGUI_SOURCE_DIR "/sample/res/fonts/DesignerSansSC.ttf", "UIDesignerSC");
    std::unique_ptr<font_familys_t, decltype(&delete_font_family)> font(
        fonts_loaded ? new_font_family(app.font_ctx, "UIDesigner,UIDesignerSC", nullptr) : nullptr, delete_font_family);
    if (!font || font->count < 2) {
        std::fprintf(stderr, "Cannot load the designer fonts\n");
        return 1;
    }
    auto* win = app.create(strings.text("app.title", "VGUI UI Designer"), 1370, 850, 0, true);
    if (!win) {
        std::fprintf(stderr, "Cannot open the designer window: %s\n", SDL_GetError());
        return test_environment ? 77 : 1;
    }
    SDL_SetWindowMinimumSize(win->window, 950, 560);
    std::unique_ptr<ovg_ctx_cb, decltype(&free_ctx_cb)> cb(new_ctx_cb(), free_ctx_cb);
    std::unique_ptr<vgui_context, decltype(&vgui_destroy)> shell(vgui_create(font.get(), 15), vgui_destroy);
    multiwindow_host host(&app, cb.get());
    if (!shell || !host.attach(win, shell.get())) {
        std::fprintf(stderr, "Cannot initialize the designer shell\n");
        return 1;
    }
    vgui_style style;
    vgui_style_default(&style);
    // Leave room for the full preview toolbar in both UI languages.
    style.dock.floating_width = 480;
    style.dock.floating_height = 320;
    vgui_context_set_style(shell.get(), &style);
    host.set_on_demand_render(true);
    // The viewport usually owns its UI; here the lifetime belongs to shell.
    struct binding_guard {
        os_window* window;
        ~binding_guard() { window->viewport->ui = nullptr; }
    } detach{win};
    smoke_unsaved_prompt prompt;
    designer::message_box_fn message_box = SDL_ShowMessageBox;
    if (testing) message_box = [&prompt](const SDL_MessageBoxData* box, int* selected) { return prompt.show(box, selected); };
    designer editor(*win, app.vgdev, shell.get(), font.get(), app.vgctx, strings, settings, std::move(message_box));
    if (open_path.empty() || !editor.open(open_path)) {
        if (test_environment && !open_path.empty()) return 1;
        if (open_path.empty()) editor.new_page();
    }
    if (invalid_settings) {
        std::fprintf(stderr, "Invalid designer settings: %s\n", utf8(settings).c_str());
        editor.report_bad_settings();
    }

    using gamepad_ptr = std::unique_ptr<SDL_Gamepad, decltype(&SDL_CloseGamepad)>;
    std::map<SDL_JoystickID, gamepad_ptr> gamepads;
    const auto open_gamepad = [&](SDL_JoystickID id) {
        if (gamepads.contains(id)) return;
        if (gamepad_ptr pad(SDL_OpenGamepad(id), SDL_CloseGamepad); pad) gamepads.emplace(id, std::move(pad));
    };
    {
        int count = 0;
        std::unique_ptr<SDL_JoystickID[], decltype(&SDL_free)> connected(SDL_GetGamepads(&count), SDL_free);
        if (connected) for (int i = 0; i < count; ++i) open_gamepad(connected[i]);
    }
    const auto preview_input_available = [&] {
        return !editor.chooser_open() && !vgui_modal_active(shell.get()) &&
            !vgui_popup_active(shell.get()) && !vgui_context_get_open_menu(shell.get());
    };
    const auto dispatch = [&](const SDL_Event& e) {
        // Controller events carry an instance ID, not a window ID. Never
        // forward them as shell input (including unused axes and releases).
        if (e.type >= SDL_EVENT_JOYSTICK_AXIS_MOTION && e.type < SDL_EVENT_FINGER_DOWN) {
            if (e.type == SDL_EVENT_GAMEPAD_ADDED) open_gamepad(e.gdevice.which);
            else if (e.type == SDL_EVENT_GAMEPAD_REMOVED) gamepads.erase(e.gdevice.which);
            else if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && gamepads.contains(e.gbutton.which) &&
                     SDL_GetKeyboardFocus() == win->window && preview_input_available() &&
                     !vgui_context_has_pointer_capture(shell.get())) {
                vgui_cursor unused = VGUI_CURSOR_NULL;
                editor.preview_event(e, unused);
            }
            return;
        }
        // Compare widths after input dispatch, never write settings on drag motion.
        if ((e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.windowID == win->id && e.button.button == SDL_BUTTON_LEFT) ||
            (e.type == SDL_EVENT_WINDOW_FOCUS_LOST && e.window.windowID == win->id))
            editor.request_layout_save();
        if (e.type == SDL_EVENT_QUIT ||
            (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && e.window.windowID == win->id)) {
            editor.request(command::quit);
            return;
        }
        // Re-exposure must repaint even when a native chooser is blocking shell input.
        if (e.type == SDL_EVENT_WINDOW_EXPOSED && e.window.windowID == win->id)
            win->request_render(0);
        const bool focus_lost = e.type == SDL_EVENT_WINDOW_FOCUS_LOST;
        // Shell overlays own input even when the pointer is over the artboard.
        vgui_cursor preview_cursor = VGUI_CURSOR_NULL;
        if ((focus_lost || preview_input_available()) && editor.preview_event(e, preview_cursor)) {
            // A consumed preview event skips the SDL backend's usual cursor update.
            if (preview_cursor != VGUI_CURSOR_NULL) app.set_defcursor(static_cast<cursor_st>(preview_cursor));
            return;
        }
        if (!editor.chooser_open() || focus_lost) app.process_event(&e);
    };
    auto last = SDL_GetTicks();
    int submitted_windows = 0;
    float game_data_step = 0.0f; // The data smoke advances its clock explicitly.
    const std::function<void()> advance = [&] {
        SDL_Event e;
        // Commit a requested transition between SDL events, without leaving
        // a mouse release queued on the other side of the poll boundary.
        const auto switch_page = [&] {
            if (editor.tab_change_pending()) {
                editor.service();
                editor.refresh();
            }
        };
        switch_page();
        while (!editor.quitting() && SDL_PollEvent(&e)) {
            dispatch(e);
            switch_page();
        }
        // Native chooser callbacks are drained by vgui_update, not an SDL event.
        // If the shell has no frame due, pump the result without rendering it.
        if (editor.chooser_open() && !app.should_render(win)) vgui_update(shell.get(), 0.f);
        editor.service();
        if (editor.quitting()) return;
        app.sync_ui_scales();
        // The preview may need the current canvas layout for drag/IME positioning
        // before begin_frame syncs the window's recording buffer.
        int width = 0, height = 0, last_width = 0, last_height = 0;
        vgui_context_get_size(shell.get(), &last_width, &last_height);
        if (SDL_GetWindowSizeInPixels(win->window, &width, &height) && width > 0 && height > 0 &&
            (width != last_width || height != last_height))
            vgui_context_set_size(shell.get(), width, height);
        editor.refresh();
        const auto now = SDL_GetTicks();
        const float elapsed = float(now - last) / 1000.0f;
        const auto dt = std::clamp(elapsed, 0.0f, 0.05f);
        last = now;
        // The independent preview may be the only thing that changed. Let it
        // invalidate the shell before begin_frame decides which canvases to clear.
        editor.render_preview(smoke == native_smoke::game_data ? std::exchange(game_data_step, 0.0f) : elapsed);
        host.begin_frame(dt);
        host.draw_all();
        submitted_windows = host.end_frame();
    };
    if (testing && !workbench_smoke) {
        try {
            advance();
            if (smoke == native_smoke::game_data) {
                exercise_game_data(*win, shell.get(), editor, advance, [&](float seconds) {
                    game_data_step = seconds;
                    advance();
                }, picker->sandbox);
                std::puts("ui_designer_game_data: typed overrides, validation, cancellation, and page isolation passed");
            } else if (smoke == native_smoke::navigation) {
                exercise_navigation(app.vgctx, cb.get(), *win, shell.get(), editor, advance, picker->sandbox);
                std::puts("ui_designer_navigation: Run focus, overrides, fallback, debug pixels, and Design link and initial focus editing passed");
            } else {
                exercise_modal_return(*win, shell.get(), editor, advance, picker->sandbox);
                exercise_modal_open(*win, shell.get(), editor, advance, picker->sandbox);
                std::puts("ui_designer_modal_return: Design modal open/return picking, clearing, cancellation, undo/redo, and saving; Run opening, closing, and focus restoration passed");
            }
        } catch (...) {
            app.wait_gpu();
            throw;
        }
    } else if (!workbench_smoke) {
        host.idle_delay_ms = 0; // Wait for SDL events instead of sleeping and polling every 8 ms.
        const auto tick = [&] {
            advance();
            if (!editor.quitting() && submitted_windows == 0) {
                SDL_Event e;
                // A worker can post a chooser result without waking SDL, so keep the wait bounded.
                if (SDL_WaitEventTimeout(&e, editor.chooser_open() ? 16 : 50)) dispatch(e);
            }
        };
#ifdef _WIN32
        if (native_dialog_exit != command::none) {
            tick(); // Draw the page and service initial window messages.
            editor.request(native_dialog_exit);
            tick();
            ui::cancel_native_dialog_and_close(win->window, tick, [&] { return editor.chooser_open(); }, [&] {
                auto* status = find_widget(vgui_root(shell.get()), "designer.status");
                return status && std::string_view(vgui_widget_get_text(status)) ==
                    strings.text("file.chooser_cancelled", "File selection cancelled.");
            });
        }
#endif
        while (!editor.quitting()) tick();
    } else {
        const ui::native_menu_input menus(*win, shell.get(), advance);
        const auto require = [](bool passed, const char* message) {
            if (!passed) throw std::runtime_error(std::string("Native on-demand render: ") + message);
        };
        const auto wait_for_idle = [&] {
            for (int i = 0; i < 60; ++i) {
                advance();
                if (submitted_windows == 0) return;
            }
            require(false, "window never stopped submitting frames");
        };
        try {
            auto expected_samples = SDL_GPU_SAMPLECOUNT_1;
            for (const auto candidate : {SDL_GPU_SAMPLECOUNT_8, SDL_GPU_SAMPLECOUNT_4, SDL_GPU_SAMPLECOUNT_2}) {
                if (SDL_GPUTextureSupportsSampleCount(app.device, vgpu_color_format(app.vgctx), candidate)) {
                    expected_samples = candidate;
                    break;
                }
            }
            require(vgpu_context_samples(app.vgctx) == expected_samples && vgpu_fbo_samples(&win->fbo) == expected_samples,
                "window did not use the highest supported MSAA setting up to 8x");
            std::printf("ui_designer_native: requested 8x MSAA, using %ux\n", 1u << unsigned(expected_samples));
            advance();
            require(submitted_windows == 1 && editor.preview_ready(true), "first frame did not present the form and image");
            const auto initial = *editor.preview().snapshot();
            const auto same_preview = [&] {
                const auto* snapshot = editor.preview().snapshot();
                return snapshot && snapshot->revision == initial.revision && snapshot->serial == initial.serial;
            };
            wait_for_idle();
            for (int i = 0; i < 4; ++i) {
                advance();
                require(submitted_windows == 0 && same_preview(), "idle tick rendered the window or updated its preview texture");
            }
            menus.activate(menus.open(0), 1); // File > Open
            require(editor.chooser_open() && picker->open_requests == 1, "file chooser did not open");
            wait_for_idle();
            SDL_Event expose{};
            expose.type = SDL_EVENT_WINDOW_EXPOSED;
            expose.window.windowID = win->id;
            require(SDL_PushEvent(&expose), "cannot queue expose event");
            advance();
            require(submitted_windows == 1 && same_preview(), "expose during file selection did not repaint from the cached preview");

            wait_for_idle();
            require(editor.chooser_open() && picker->complete_cancelled_open(), "cannot post a delayed chooser result");
            advance(); // No SDL event accompanies the worker's result.
            require(!editor.chooser_open() && submitted_windows == 1 && same_preview(),
                "idle file chooser did not process cancellation, repaint and preserve the document");
            menus.activate(menus.open(0), 1); // Mock chooser returns a non-design file.
            for (int i = 0; i < 4 && editor.chooser_open(); ++i) advance();
            require(picker->open_requests == 2 && !editor.chooser_open() && same_preview(),
                "invalid file selection changed the preview");

            menus.activate(menus.open(3), 4); // View > Phone
            const auto* snapshot = editor.preview().snapshot();
            require(submitted_windows == 1 && editor.preview_ready(true) && snapshot &&
                snapshot->revision == initial.revision && snapshot->serial != initial.serial &&
                snapshot->view.width < initial.view.width && snapshot->view.width < snapshot->view.height,
                "preset change did not rebuild and present the preview");
            exercise_runtime_input(*win, shell.get(), editor, advance, submitted_windows);
            ui::exercise_delivery(*win, shell.get(), editor.history(), advance, open_path,
                                  picker->package_parent, picker->cancel_folder, editor.strings());
            // Preserve the delivery test's unsaved edit and actual Run-mode input.
            exercise_language_and_layout(*win, shell.get(), editor, settings, advance);
            exercise_grid_layers(app, font.get(), strings);
            exercise_options(*win, shell.get(), editor, settings, advance);
            exercise_control_drag(*win, shell.get(), editor, advance, picker->sandbox, open_path);
            exercise_custom_viewport(*win, shell.get(), editor, advance, picker->sandbox, open_path);
            exercise_view_tools(app, *win, shell.get(), editor, advance, picker->sandbox, open_path);
            if (!editor.open(absolute_fixture))
                throw std::runtime_error("Cannot open the absolute geometry test fixture");
            advance();
            ui::exercise_geometry(*win, shell.get(), editor.preview(), editor.history(), advance);
            ui::exercise_pages(*win, shell.get(), editor, advance, *picker, prompt, open_path, absolute_fixture);
            ui::exercise_page_docking(*win, shell.get(), editor, advance, submitted_windows);
            ui::exercise_export_isolation(*win, shell.get(), editor, advance, *picker, open_path);
            exercise_image_button(*win, shell.get(), editor, advance);
            exercise_icon_region(*win, shell.get(), editor, advance, picker->sandbox);
            exercise_project_fonts(*win, shell.get(), editor, *picker, advance, absolute_fixture);
            require(editor.open(fs::path(VGUI_SOURCE_DIR) / "sample/res/control-gallery.vgui.json"), "could not open the core control gallery");
            advance();
            require(editor.preview_ready(true) && editor.preview().snapshot()->nodes.size() == editor.history().current().nodes.size(),
                "control gallery did not render the full design");
            exercise_keyframe_delivery(*win, shell.get(), editor, advance, picker->sandbox);
        } catch (...) {
            app.wait_gpu();
            throw;
        }
        std::puts("ui_designer_native: rendering, input, view tools, image buttons, icon and four-state region picking, geometry, control palette drag, language, theme, grid layering, layout, page docking, multi-page save/close/exit, delivery isolation, keyframe delivery, pose capture and gradient shape/stop handle dragging passed");
    }
    app.wait_gpu();
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    try {
        const std::string_view arg = argc == 2 ? argv[1] : "";
        if (arg == "--smoke-test") return run(native_smoke::workbench, {});
        if (arg == "--smoke-game-data") return run(native_smoke::game_data, {});
        if (arg == "--smoke-navigation") return run(native_smoke::navigation, {});
        if (arg == "--smoke-modal-return") return run(native_smoke::modal_return, {});
        if (argc == 2 && std::string_view(argv[1]) == "--smoke-animation-preview") return smoke_animation_preview();
        if (argc == 2 && std::string_view(argv[1]) == "--smoke-gradient-keyframes") return smoke_gradient_keyframes();
#ifdef _WIN32
        if (argc == 2) {
            if (arg == "--smoke-native-open-exit" || arg == "--smoke-native-save-exit") {
                return run(native_smoke::none, fs::path(VGUI_SOURCE_DIR) / "sample/res/gradient-keyframes.vgui.json",
                    arg == "--smoke-native-open-exit" ? command::open : command::save_as);
            }
        }
#endif
        if (argc > 2) {
            std::fprintf(stderr, "Usage: vgui_ui_designer [design.vgui.json | --smoke-test | --smoke-game-data | --smoke-navigation | --smoke-modal-return | --smoke-animation-preview | --smoke-gradient-keyframes]\n");
            return 2;
        }
        return run(native_smoke::none, argc == 2 ? fs::u8path(argv[1]) : fs::path{});
    } catch (const std::exception& error) {
        std::fprintf(stderr, "VGUI UI Designer: %s\n", error.what());
        return 1;
    }
}
