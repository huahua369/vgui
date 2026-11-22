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
#include <cmath>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
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
              [this](ui::designer_theme theme) { pending_theme_ = theme; }}, strings_), dock_(workbench_.document_dock()) {
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
        if (package_ && vgui_modal_panel(shell_) == package_->panel)
            vgui_modal_close(shell_, VGUI_MODAL_RESULT_CANCEL);
        vgui_dock_on_activate(dock_, nullptr, nullptr);
        vgui_dock_on_close_request(dock_, nullptr, nullptr);
        pages_.clear(); // Destroy previews before the workbench destroys their parents.
    }

    void request(command action, vgui_item_id page = 0) {
        if (chooser_ || pending_ != command::none || pending_tab_) return;
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
    bool preview_event(const SDL_Event& event) {
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
            page->preview->handle_event(event);
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
                                         std::pair{"width", change.width}, std::pair{"height", change.height}}) {
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
        else workbench_.status(std::string(change.width || change.height ? tr("geometry.resized", "Resized ") : tr("geometry.moved", "Moved ")) + change.id);
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

    ui::designer_preview& ensure_preview(page_state& page) {
        if (page.preview) return *page.preview;
        const auto id = page.id;
        page.preview = std::make_unique<ui::designer_preview>(page.host, gpu_, font_, preview_fonts_,
            [this, id](std::string node) { if (active_ == id) workbench_.select(std::move(node)); },
            [this, id](std::string message) { if (active_ == id) workbench_.status(std::move(message)); },
            [this, id](ui::geometry_edit change) { commit_geometry(id, std::move(change)); }, strings_,
            [this, id](auto revision, const auto& edits) { return commit_keyframes(id, revision, edits); });
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
    click(widget("designer.options.nav.grid", VGUI_BUTTON));
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
    replace_text("property:axis_color", "#12345680");
    auto expected = ui::designer_grid_settings{};
    expected.spacing = 32;
    expected.axis_color = 0x80123456u;
    require(editor.preview().grid_settings() == expected, "typing an RGBA color did not immediately preview it");
    auto* color_swatch = widget("property:axis_color", VGUI_PANEL);
    click(color_swatch);
    auto* picker = vgui_popup_content(shell);
    auto* red = find_widget(picker, "property-grid.color-picker.swatch.2", VGUI_PANEL);
    require(red, "color picker did not open over the options modal");
    click(red);
    require(editor.preview().grid_settings().axis_color == 0xFFFF0000u, "color picker did not preview its preset");
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
    ui::designer_grid_settings grid{.spacing = 80, .line_width = 3, .minor_color = 0xFFFF0000u};
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
    const auto [ruler_x, ruler_y] = point_at(.6, 10);
    require(vgui_hit_test(shell.get(), ruler_x, ruler_y) == tracks, "ruler is hit-testable");
    click_at(ruler_x, ruler_y);
    update();
    require(preview.running() && !preview.capture_view().animation.playing && near(preview.capture_view().animation.time, .6) &&
        model.current() == before && !model.dirty(), "ruler seeks and pauses without editing the page");
    click("designer.timeline.add");
    update();
    auto frames = keyframes("highlight");
    require(frames.size() == 4 && near(frames[1].offset * 2.4, .6) && keyframes("tint").size() == 4,
        "add writes the chosen track, not its neighbour");
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
    submit(duration_edit, "3.6");
    update();
    require(model.current().nodes.at("highlight").props.at("gradient_animation_duration") == ui::value{3.6},
        "editing timeline duration updates the selected gradient property");
    const auto resized = keyframes("highlight");
    require(resized.size() == before_resize.size() &&
        std::ranges::equal(resized, before_resize, [&](const auto& a, const auto& b) { return near(a.offset, b.offset); }),
        "duration editing preserves normalized keyframe timing");
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
    return 0;
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
    const auto widget = [&](const char* id) {
        auto* w = find_widget(preview.widget(), id);
        require(w != nullptr, id);
        return w;
    };
    const auto rect = [](vgui_widget* w) {
        ui::pixel_rect r;
        vgui_widget_get_rect(w, &r.x, &r.y, &r.width, &r.height);
        return r;
    };
    const auto push = [&](SDL_Event e) { require(SDL_PushEvent(&e), "cannot queue SDL input"); };
    const auto button = [&](bool down, glm::vec2 px) {
        const auto p = px / win.pixel_ratio();
        SDL_Event e{};
        e.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
        e.button.windowID = win.id;
        e.button.button = SDL_BUTTON_LEFT;
        e.button.down = down;
        e.button.clicks = 1;
        e.button.x = p.x;
        e.button.y = p.y;
        push(e);
    };
    const auto click_at = [&](vgui_widget* w, glm::vec2 p) {
        require(vgui_hit_test(shell, p.x, p.y) == w, "click does not hit the intended control");
        button(true, p);
        button(false, p);
        frame();
    };
    const auto click = [&](const char* id) {
        auto* w = widget(id);
        const auto r = rect(w);
        click_at(w, {r.x + r.width / 2, r.y + r.height / 2});
    };
    click("designer.preview.timeline");
    click("designer.timeline.fit");
    auto* tracks = widget("designer.timeline.tracks");
    const auto r = rect(tracks);
    const float scale = vgui_context_get_ui_scale(shell);
    // Fit leaves an 8px leading gutter and uses width minus the header and 24px for the four-second ruler.
    const float header = std::clamp((r.width - 2) * .28f, 92 * scale, 150 * scale);
    const glm::vec2 key_at{r.x + 1 + header + 8 + (r.width - header - 24) / 2, r.y + 1 + 37 * scale};
    click_at(tracks, key_at);
    const auto view = preview.capture_view();
    require(view.timeline_open && view.running && !view.animation.playing && view.animation.time == 2.0 &&
        vgui_widget_get_enabled(widget("designer.timeline.geometry")) && editor.selection() != "linear",
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
    require(vgui_hit_test(shell, start.x, start.y) == widget("designer.preview.canvas") && finish.x - start.x > 10,
        "projected start handle is not reachable on the canvas");
    const auto motion = [&](glm::vec2 px) {
        const auto p = px / win.pixel_ratio(), rel = (px - start) / win.pixel_ratio();
        SDL_Event e{};
        e.type = SDL_EVENT_MOUSE_MOTION;
        e.motion.windowID = win.id;
        e.motion.state = SDL_BUTTON_LMASK;
        e.motion.x = p.x;
        e.motion.y = p.y;
        e.motion.xrel = rel.x;
        e.motion.yrel = rel.y;
        push(e);
    };
    const auto move = [&](glm::vec2 to) {
        button(true, start);
        frame();
        unchanged("press changed the document or history");
        motion(to);
        frame();
        unchanged("motion edited the document before release");
    };
    const auto release = [&](glm::vec2 at) { button(false, at); frame(); };
    const auto scrubber = rect(widget("designer.preview.animation.seek"));
    click_at(widget("designer.preview.animation.seek"),
        {scrubber.x + scrubber.width * .25f, scrubber.y + scrubber.height / 2});
    require(preview.capture_view().animation.time != 2.0, "scrub away from the selected key");
    move(finish);
    release(finish);
    unchanged("a handle edited the key while the playhead was away");
    click_at(tracks, key_at);
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
    push(lost);
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
    exercise_keyframe_endpoint_drag(win, shell, editor, frame, sandbox);
}

fs::path designer_settings_path() {
    if (const char* override_path = SDL_getenv("VGUI_DESIGNER_SETTINGS"); override_path && *override_path)
        return fs::u8path(override_path);
    std::unique_ptr<char, decltype(&SDL_free)> pref(SDL_GetPrefPath("vgui", "ui_designer"), SDL_free);
    return pref ? fs::u8path(pref.get()) / "settings.json" : fs::path{};
}

int run(bool smoke, fs::path open_path, command native_dialog_exit = command::none) {
    const bool test_environment = smoke || native_dialog_exit != command::none;
    app_mgr app;
    app.msaa_samples = 8; // Request 8× before creating the window and its GPU context.
    if (!app.init_gpu(true)) {
        std::fprintf(stderr, "Cannot initialize SDL3 Vulkan: %s\n", SDL_GetError());
        return test_environment ? 77 : 1;
    }
    std::optional<smoke_file_picker> picker;
    fs::path absolute_fixture;
    if (smoke) {
        picker.emplace();
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
    const auto settings = smoke ? picker->sandbox / "settings.json" :
        test_environment ? fs::path{} : designer_settings_path();
    if (smoke && (!ui::save_designer_dock_widths(settings, {280, 375}) ||
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
    if (smoke) message_box = [&prompt](const SDL_MessageBoxData* box, int* selected) { return prompt.show(box, selected); };
    designer editor(*win, app.vgdev, shell.get(), font.get(), app.vgctx, strings, settings, std::move(message_box));
    if (open_path.empty() || !editor.open(open_path)) {
        if (test_environment && !open_path.empty()) return 1;
        if (open_path.empty()) editor.new_page();
    }
    if (invalid_settings) {
        std::fprintf(stderr, "Invalid designer settings: %s\n", utf8(settings).c_str());
        editor.report_bad_settings();
    }

    const auto dispatch = [&](const SDL_Event& e) {
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
        if ((focus_lost || (!editor.chooser_open() && !vgui_modal_active(shell.get()) &&
            !vgui_popup_active(shell.get()) && !vgui_context_get_open_menu(shell.get()))) && editor.preview_event(e))
            return;
        if (!editor.chooser_open() || focus_lost) app.process_event(&e);
    };
    auto last = SDL_GetTicks();
    int submitted_windows = 0;
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
        editor.render_preview(elapsed);
        host.begin_frame(dt);
        host.draw_all();
        submitted_windows = host.end_frame();
    };
    if (!smoke) {
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
            if (!editor.open(absolute_fixture))
                throw std::runtime_error("Cannot open the absolute geometry test fixture");
            advance();
            ui::exercise_geometry(*win, shell.get(), editor.preview(), editor.history(), advance);
            ui::exercise_pages(*win, shell.get(), editor, advance, *picker, prompt, open_path, absolute_fixture);
            ui::exercise_page_docking(*win, shell.get(), editor, advance);
            ui::exercise_export_isolation(*win, shell.get(), editor, advance, *picker, open_path);
            exercise_image_button(*win, shell.get(), editor, advance);
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
        std::puts("ui_designer_native: rendering, input, image buttons, geometry, language, theme, grid layering, layout, page docking, multi-page save/close/exit, delivery isolation, keyframe delivery and endpoint dragging passed");
    }
    app.wait_gpu();
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    try {
        const bool smoke = argc == 2 && std::string_view(argv[1]) == "--smoke-test";
        if (argc == 2 && std::string_view(argv[1]) == "--smoke-animation-preview") return smoke_animation_preview();
        if (argc == 2 && std::string_view(argv[1]) == "--smoke-gradient-keyframes") return smoke_gradient_keyframes();
#ifdef _WIN32
        if (argc == 2) {
            const std::string_view arg(argv[1]);
            if (arg == "--smoke-native-open-exit" || arg == "--smoke-native-save-exit") {
                return run(false, fs::path(VGUI_SOURCE_DIR) / "sample/res/gradient-keyframes.vgui.json",
                    arg == "--smoke-native-open-exit" ? command::open : command::save_as);
            }
        }
#endif
        if (argc > 2) {
            std::fprintf(stderr, "Usage: vgui_ui_designer [design.vgui.json | --smoke-test | --smoke-animation-preview | --smoke-gradient-keyframes]\n");
            return 2;
        }
        return run(smoke, argc == 2 && !smoke ? fs::u8path(argv[1]) : fs::path{});
    } catch (const std::exception& error) {
        std::fprintf(stderr, "VGUI UI Designer: %s\n", error.what());
        return 1;
    }
}
