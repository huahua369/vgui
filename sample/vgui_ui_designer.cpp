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
#include <SDL3/SDL_main.h>

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
    // A single portable directory component; the parent is chosen separately.
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
              [this] { request(command::options); }}, strings_), dock_(workbench_.document_dock()) {
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
        vgui_dock_on_activate(dock_, nullptr, nullptr);
        vgui_dock_on_close_request(dock_, nullptr, nullptr);
        pages_.clear(); // Destroy previews before the workbench destroys their parents.
    }

    void request(command action, vgui_item_id page = 0) {
        if (chooser_ || pending_ != command::none || pending_tab_) return;
        if (package_parent_ && vgui_modal_active(shell_)) {
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
        if (auto view = std::exchange(pending_view_, std::nullopt); view && active_) apply_view(*view);
        if (const auto tab = std::exchange(pending_tab_, 0); tab && !chooser_ && !package_parent_) activate(tab);
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
        case command::export_package:
            if (find_page(page)) choose(VGUI_FILE_DIALOG_FOLDER, page);
            break;
        case command::import_font:
            if (const auto* entry = find_page(page); entry && !entry->path.empty())
                choose(VGUI_FILE_DIALOG_OPEN, page, command::import_font);
            else workbench_.status(tr("font.save_first", "Save this page before importing a project font."));
            break;
        case command::close: close_page(page); break;
        case command::options:
            options_.show(shell_, strings_, grid_settings_, workbench_.dock_widths(), utf8(settings_path_),
                [this](ui::designer_grid_settings grid) { apply_grid_settings(grid); },
                [this](ui::designer_grid_settings grid) { return ui::save_designer_grid_settings(settings_path_, grid); });
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
            [this, id](ui::geometry_edit change) { commit_geometry(id, std::move(change)); }, strings_);
        page.preview->set_render_context(renderer_);
        page.preview->set_grid_settings(grid_settings_);
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
        const auto location = utf8(page ? resource_root(*page) : new_root_);
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

    static void on_package_name(vgui_context* ctx, int result, const char* text, void* user) {
        auto& self = *static_cast<designer*>(user);
        if (std::exchange(self.package_text_input_, false) && !vgui_context_wants_text_input(ctx))
            self.window_.stop_text_input();
        if (result == VGUI_MODAL_RESULT_CONFIRM) self.package_name_ = text ? text : "";
        else {
            self.package_parent_.reset();
            self.package_page_ = 0;
            self.workbench_.status(self.tr("export.cancelled", "Package export cancelled."));
        }
    }

    void choose_package_name(vgui_item_id id, fs::path parent) {
        const auto* page = find_page(id);
        if (!page) return;
        auto name = page->path.empty() ? std::string("untitled") : utf8(page->path.filename());
        constexpr std::string_view suffix = ".vgui.json";
        if (name.ends_with(suffix)) name.resize(name.size() - suffix.size());
        else if (!page->path.empty()) name = utf8(page->path.stem());
        name += "-delivery";
        if (!valid_package_name(name)) name = "ui-delivery";
        const auto message = std::string(tr("export.prompt_text", "Create a new package folder in:\n")) + utf8(parent);
        vgui_dialog_desc desc;
        vgui_dialog_desc_default(&desc);
        desc.kind = VGUI_DIALOG_PROMPT;
        desc.title = tr("file.export", "Export Package");
        desc.message = message.c_str();
        desc.confirm_text = tr("export.confirm", "Export");
        desc.cancel_text = tr("common.cancel", "Cancel");
        desc.initial_text = name.c_str();
        desc.on_result = on_package_name;
        desc.user = this;
        if (!vgui_dialog_show(shell_, &desc)) {
            workbench_.status(tr("export.prompt_failed", "Cannot open the package name prompt."));
            return;
        }
        package_page_ = id;
        package_parent_ = std::move(parent);
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
        auto parent = std::exchange(package_parent_, std::nullopt);
        auto* page = find_page(std::exchange(package_page_, 0));
        if (!page) return;
        if (!parent || !valid_package_name(*name)) {
            workbench_.status(tr("export.bad_name", "Export failed: enter a single folder name (no path separators or trailing dots/spaces)."));
            return;
        }
        const auto destination = *parent / fs::u8path(*name);
        ui::diagnostic error;
        if (!ui::export_delivery(page_session(*page).current(), resource_root(*page), destination, &error))
            workbench_.status(std::string(tr("export.failed", "Export failed: ")) + describe(error));
        else workbench_.status(std::string(tr("export.done", "Exported package to ")) + utf8(destination));
    }

    void finish_chooser() {
        auto selected = std::move(*picked_);
        picked_.reset();
        chooser_ = false;
        const auto next = std::exchange(chooser_followup_, command::none);
        const auto id = std::exchange(chooser_page_, 0);
        if (!selected.error.empty()) workbench_.status(std::string(tr("file.chooser_error", "File chooser: ")) + selected.error);
        else if (selected.path.empty()) workbench_.status(tr("file.chooser_cancelled", "File selection cancelled."));
        else if (selected.kind == VGUI_FILE_DIALOG_OPEN) {
            if (next == command::import_font) finish_font_import(id, fs::u8path(selected.path));
            else open(fs::u8path(selected.path));
        }
        else if (selected.kind == VGUI_FILE_DIALOG_FOLDER) choose_package_name(id, fs::u8path(selected.path));
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
    bool layout_save_pending_ = false;
    std::optional<ui::designer_language> pending_language_;
    std::optional<view_action> pending_view_;
    const fs::path new_root_ = fs::canonical(fs::current_path());
    std::vector<page_state> pages_;
    std::deque<vgui_item_id> save_all_queue_;
    std::vector<vgui_item_id> exit_queue_;
    vgui_item_id active_ = 0, pending_tab_ = 0, command_page_ = 0, chooser_page_ = 0, package_page_ = 0;
    std::uint64_t next_untitled_ = 1;
    vgui_item_id cached_tab_ = 0;
    bool projecting_tabs_ = false;
    ui::session session_;
    std::uint64_t next_preview_revision_ = 0;
    std::string window_title_;
    command pending_ = command::none, chooser_followup_ = command::none;
    bool chooser_ = false, quitting_ = false;
    std::optional<file_result> picked_;
    std::optional<fs::path> package_parent_;
    std::optional<std::string> package_name_;
    bool package_text_input_ = false;
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
        require(vgui_menu_get_count(tools) == 2 &&
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
            text("designer.options.configuration.left") == (english ? "Left sidebar: 280 logical px · Collapsed" : "左侧栏：280 逻辑像素 · 已展开") &&
            text("designer.options.configuration.right") == (english ? "Right sidebar: 375 logical px · Expanded" : "右侧栏：375 逻辑像素 · 已展开"),
            "configuration page did not show the current language and preferred dock sizes/states");
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
    const auto original = editor.preview().grid_settings();
    const auto before = editor.history().current();
    const auto revision = editor.history().revision();
    require(original.spacing == 24 && ui::load_designer_grid_settings(settings) == original,
        "startup settings were not applied to the canvas");
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
    click(widget("designer.options.cancel", VGUI_BUTTON));
    require(!vgui_modal_active(shell) && editor.preview().grid_settings() == original &&
        ui::load_designer_grid_settings(settings) == original, "Cancel did not restore settings without writing them");

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
    click(widget("designer.options.nav.configuration", VGUI_BUTTON));
    click(widget("designer.options.ok", VGUI_BUTTON));
    require(!vgui_modal_active(shell) && editor.preview().grid_settings() == expected &&
        ui::load_designer_grid_settings(settings) == expected, "OK did not persist the current live values");
    require(editor.history().revision() == revision && editor.history().current() == before,
        "changing application-level options modified the design");
}

fs::path designer_settings_path() {
    if (const char* override_path = SDL_getenv("VGUI_DESIGNER_SETTINGS"); override_path && *override_path)
        return fs::u8path(override_path);
    std::unique_ptr<char, decltype(&SDL_free)> pref(SDL_GetPrefPath("vgui", "ui_designer"), SDL_free);
    return pref ? fs::u8path(pref.get()) / "settings.json" : fs::path{};
}

int run(bool smoke, fs::path open_path) {
    app_mgr app;
    app.msaa_samples = 8; // Request 8× before creating the window and its GPU context.
    if (!app.init_gpu(true)) {
        std::fprintf(stderr, "Cannot initialize SDL3 Vulkan: %s\n", SDL_GetError());
        return smoke ? 77 : 1;
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
    const auto settings = smoke ? picker->sandbox / "settings.json" : designer_settings_path();
    if (smoke && (!ui::save_designer_dock_widths(settings, {280, 375}) ||
                  !ui::save_designer_grid_settings(settings, {.spacing = 24})))
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
        return smoke ? 77 : 1;
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
        if (smoke) return 1;
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
        const auto dt = std::clamp(float(now - last) / 1000.0f, 0.0f, 0.05f);
        last = now;
        // The independent preview may be the only thing that changed. Let it
        // invalidate the shell before begin_frame decides which canvases to clear.
        editor.render_preview(dt);
        host.begin_frame(dt);
        host.draw_all();
        submitted_windows = host.end_frame();
    };
    if (!smoke) {
        host.idle_delay_ms = 0; // Wait for SDL events instead of sleeping and polling every 8 ms.
        while (!editor.quitting()) {
            advance();
            if (!editor.quitting() && submitted_windows == 0) {
                SDL_Event e;
                // A worker can post a chooser result without waking SDL, so keep the wait bounded.
                if (SDL_WaitEventTimeout(&e, editor.chooser_open() ? 16 : 50)) dispatch(e);
            }
        }
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
        } catch (...) {
            app.wait_gpu();
            throw;
        }
        std::puts("ui_designer_native: rendering, input, image buttons, geometry, language, layout, page docking, multi-page save/close/exit and delivery isolation passed");
    }
    app.wait_gpu();
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    try {
        const bool smoke = argc == 2 && std::string_view(argv[1]) == "--smoke-test";
        if (argc > 2) {
            std::fprintf(stderr, "Usage: vgui_ui_designer [design.vgui.json | --smoke-test]\n");
            return 2;
        }
        return run(smoke, argc == 2 && !smoke ? fs::u8path(argv[1]) : fs::path{});
    } catch (const std::exception& error) {
        std::fprintf(stderr, "VGUI UI Designer: %s\n", error.what());
        return 1;
    }
}
