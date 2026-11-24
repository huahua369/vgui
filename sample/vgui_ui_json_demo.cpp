// Standalone SDL3 host for saved .vgui.json designs and .vgui.package.json deliveries.
#include "delivery.h"
#include "runtime.h"
#include "ui_json_images.h"

#include <vgui/backend/vgui_multiwindow.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace {
namespace ui = vgui::ui_design;
namespace fs = std::filesystem;

constexpr int initial_width = 460;
constexpr int initial_height = 300;
constexpr char submit_action[] = "contact.submit";
constexpr char help_action[] = "contact.help";
constexpr char back_action[] = "contact.back";
constexpr char help_file[] = VGUI_SOURCE_DIR "/sample/res/form-help.vgui.json";
constexpr char smoke_image_id[] = "logo";

struct form_input {
    ui::embedded_delivery content;
    fs::path project_root;
    bool packaged = false;
};

bool read_form(const fs::path& file, form_input& input, ui::diagnostic& error) {
    input.packaged = file.native().ends_with(fs::path(ui::json_delivery_suffix).native());
    if (input.packaged) return ui::read_json_delivery(file, input.content, &error);
    input.project_root = file.has_parent_path() ? file.parent_path() : fs::path(".");
    return ui::read_document(file, input.content.design, &error);
}

struct form_host {
    form_host(vgui_context* context, const form_input& input) : input(input), tree(context) {}

    bool mount() {
        auto bindings = action_bindings();
        if (input.packaged) {
            bindings.load_embedded_image = [](vgui_context* ctx, std::string_view key, void* user) -> vgui_texture* {
                auto& form = *static_cast<form_host*>(user);
                const auto& images = form.input.content.images;
                const auto it = images.find(key);
                return it == images.end() ? nullptr : form.decoder.load(ctx, it->second);
            };
        } else {
            bindings.project_root = input.project_root;
        }
        ui::diagnostic error;
        const bool mounted = tree.mount(input.content.design, 1, bindings, &error);
        decoder = {}; // Drop the loader's references; the mounted tree retains assigned textures.
        if (!mounted) {
            std::fprintf(stderr, "Cannot mount document (%s): %s\n", error.path.c_str(), error.message.c_str());
            return false;
        }
        for (const auto& warning : tree.diagnostics())
            std::fprintf(stderr, "Runtime warning (%s): %s\n", warning.path.c_str(), warning.message.c_str());
        return true;
    }

    void service() {
        if (std::exchange(submit_pending, false)) submit();
        switch (std::exchange(pending_page, page_action::none)) {
        case page_action::help: open_help(); break;
        case page_action::back: tree.pop_page(); break;
        case page_action::none: break;
        }
        tree.service_navigation();
    }

    const form_input& input;
    vgui_sample::json_image_decoder decoder;
    ui::runtime tree;

private:
    enum class page_action { none, help, back };
    page_action pending_page = page_action::none;
    bool submit_pending = false;

    ui::runtime_bindings action_bindings() {
        ui::runtime_bindings bindings;
        bindings.user = this;
        bindings.on_action = [](std::string_view, std::string_view action, void* user) noexcept {
            // Match only host-owned routing keys. Defer widget access and navigation
            // until input dispatch has returned.
            auto& form = *static_cast<form_host*>(user);
            if (action == submit_action) form.submit_pending = true;
            else if (form.pending_page == page_action::none) {
                if (action == help_action) form.pending_page = page_action::help;
                else if (action == back_action) form.pending_page = page_action::back;
            }
        };
        return bindings;
    }

    void open_help() {
        // The host chooses the document. The action's JSON never supplies its path.
        ui::document help;
        ui::diagnostic error;
        if (!ui::read_document(fs::u8path(help_file), help, &error) ||
            !tree.push_page(help, 2, action_bindings(), &error)) {
            std::fprintf(stderr, "Cannot open demo Help (%s): %s\n", error.path.c_str(), error.message.c_str());
        }
    }

    void submit() {
        // Access live widgets after input dispatch has returned. The JSON is never edited.
        auto* name = tree.widget("name");
        auto* hint = tree.widget("hint");
        if (hint && vgui_widget_get_kind(hint) == VGUI_LABEL) {
            const char* text = name && vgui_widget_get_kind(name) == VGUI_EDIT ? vgui_edit_get_text(name) : nullptr;
            const std::string message = text && *text ? std::string("Demo submitted for ") + text : "Demo submitted";
            vgui_widget_set_text(hint, message.c_str());
        }
        std::printf("Handled %s (demo only; nothing was sent).\n", submit_action);
    }
};

using context_ptr = std::unique_ptr<vgui_context, decltype(&vgui_destroy)>;

int smoke_test(const form_input& input) {
    context_ptr context(vgui_create(nullptr, 16), vgui_destroy);
    if (!context) return 1;
    form_host form(context.get(), input);
    if (!form.mount()) return 1;
    const auto layout = form.tree.layout({initial_width, initial_height, 1});
    if (!layout) return 1;
    if (input.packaged) {
        const auto image = std::find_if(layout->nodes.begin(), layout->nodes.end(), [](const auto& node) {
            return node.id == smoke_image_id;
        });
        // With no explicit size, only the actual PNG texture gives this node its 32x32 bounds.
        if (image == layout->nodes.end() || image->image_placeholder || !image->visible ||
            image->rect_px.width != 32 || image->rect_px.height != 32 || !form.tree.diagnostics().empty()) {
            std::fprintf(stderr, "Expected a mounted 32x32 embedded PNG, not an image placeholder.\n");
            return 1;
        }
    }

    auto* name = form.tree.widget("name");
    auto* button = form.tree.widget("submit");
    auto* hint = form.tree.widget("hint");
    auto* help = form.tree.widget("help");
    if (!name || !button || !hint || !help) return 1;
    auto press_center = [&](vgui_widget* widget, float& x, float& y) {
        float width = 0, height = 0;
        vgui_widget_get_rect(widget, &x, &y, &width, &height);
        x += width / 2;
        y += height / 2;
        vgui_input_mouse_move(context.get(), x, y);
        vgui_input_mouse_down(context.get(), 1, x, y, 1);
    };
    float x = 0, y = 0;
    press_center(name, x, y);
    vgui_input_mouse_up(context.get(), 1, x, y, 1);
    vgui_input_text(context.get(), "Ada");
    press_center(button, x, y);
    form.service();
    if (std::string_view(vgui_widget_get_text(hint)) != "Ready to send") return 1;
    vgui_input_mouse_up(context.get(), 1, x, y, 1);
    form.service();
    if (std::string_view(vgui_widget_get_text(hint)) != "Demo submitted for Ada") {
        std::fprintf(stderr, "Expected a Submit click to update the hint with the live name.\n");
        return 1;
    }

    const auto base_depth = form.tree.page_depth();
    const auto help_open = [&] {
        auto* back = form.tree.widget("back");
        return form.tree.page_depth() == base_depth + 1 && !form.tree.widget("name") &&
            back && vgui_get_focus(context.get()) == back;
    };
    const auto form_restored = [&] {
        return form.tree.page_depth() == base_depth && form.tree.widget("name") == name &&
            form.tree.widget("hint") == hint && form.tree.widget("help") == help &&
            vgui_get_focus(context.get()) == help && std::string_view(vgui_edit_get_text(name)) == "Ada" &&
            std::string_view(vgui_widget_get_text(hint)) == "Demo submitted for Ada";
    };
    press_center(help, x, y);
    vgui_input_mouse_up(context.get(), 1, x, y, 1);
    form.service();
    if (!help_open()) {
        std::fprintf(stderr, "Expected a Help click to open the Help page and focus Back.\n");
        return 1;
    }
    // Escape key-down is also the input sent by the SDL gamepad B button.
    const bool escaped = vgui_input_key(context.get(), VGUI_KEY_ESCAPE, 1, 0) != 0;
    form.service();
    if (!escaped || !form_restored()) {
        std::fprintf(stderr, "Expected Escape to restore the same form, draft, and Help focus.\n");
        return 1;
    }
    const auto press_enter = [&] {
        vgui_input_key(context.get(), VGUI_KEY_ENTER, 1, 0);
        vgui_input_key(context.get(), VGUI_KEY_ENTER, 0, 0);
        form.service();
    };
    press_enter();
    if (!help_open()) {
        std::fprintf(stderr, "Expected Enter on the restored Help button to open Help again.\n");
        return 1;
    }
    press_enter();
    if (!form_restored()) {
        std::fprintf(stderr, "Expected Back activation to restore the same form, draft, and Help focus.\n");
        return 1;
    }
    return 0;
}

int smoke_package(const fs::path& form_file) try {
    ui::document design;
    ui::diagnostic error;
    if (!ui::read_document(form_file, design, &error)) {
        std::fprintf(stderr, "Cannot load the smoke-test form (%s): %s\n", error.path.c_str(), error.message.c_str());
        return 1;
    }
    design.resources.emplace("smoke.logo", "gallery-row-folder.png");
    design.nodes.emplace(smoke_image_id, ui::node{"image", "", {{"resource", std::string("smoke.logo")}}, {}});
    auto& footer = design.nodes.at("footer").children;
    footer.insert(footer.begin(), smoke_image_id);

    struct temporary_directory {
        fs::path path;
        ~temporary_directory() {
            if (path.empty()) return;
            std::error_code ignored;
            fs::remove_all(path, ignored);
        }
    } temporary;
    std::random_device random;
    const auto parent = fs::temp_directory_path();
    for (int i = 0; i < 32; ++i) {
        auto candidate = parent / ("vgui-json-demo-" + std::to_string(random()) + "-" + std::to_string(random()));
        if (fs::create_directory(candidate)) {
            temporary.path = std::move(candidate);
            break;
        }
    }
    if (temporary.path.empty()) {
        std::fprintf(stderr, "Cannot reserve a smoke-test temporary directory.\n");
        return 1;
    }
    // Only the package is present in its directory. The packaged form must not need sidecar files.
    const auto package = temporary.path / (std::string("form") + std::string(ui::json_delivery_suffix));
    if (!ui::export_json_delivery(design, form_file.parent_path(), package, &error)) {
        std::fprintf(stderr, "Cannot export the smoke-test package (%s): %s\n", error.path.c_str(), error.message.c_str());
        return 1;
    }
    form_input input;
    if (!read_form(package, input, error)) {
        std::fprintf(stderr, "Cannot load the smoke-test package (%s): %s\n", error.path.c_str(), error.message.c_str());
        return 1;
    }
    return smoke_test(input);
} catch (const std::exception& error) {
    std::fprintf(stderr, "Package smoke test failed: %s\n", error.what());
    return 1;
}

int run_window(const form_input& input) {
    app_mgr app;
    const char* font_path = VGUI_SOURCE_DIR "/sample/res/fonts/DejaVuSans.ttf";
    std::unique_ptr<font_familys_t, decltype(&delete_font_family)> font(
        load_font_from_path(app.font_ctx, font_path, "JsonDemo") ?
            new_font_family(app.font_ctx, "JsonDemo", nullptr) : nullptr, delete_font_family);
    if (!font) {
        std::fprintf(stderr, "Cannot load demo font: %s\n", font_path);
        return 1;
    }
    if (!app.init_gpu(true)) {
        std::fprintf(stderr, "Cannot initialize the SDL3 renderer: %s\n", SDL_GetError());
        return 1;
    }
    auto* window = app.create("VGUI / JSON Demo", initial_width, initial_height, ef_resizable, true);
    if (!window) {
        std::fprintf(stderr, "Cannot open the JSON demo: %s\n", SDL_GetError());
        return 1;
    }

    context_ptr context(vgui_create(font.get(), 16), vgui_destroy);
    if (!context) return 1;
    form_host form(context.get(), input);
    if (!form.mount()) return 1;

    std::unique_ptr<ovg_ctx_cb, decltype(&free_ctx_cb)> cb(new_ctx_cb(), free_ctx_cb);
    multiwindow_host host(&app, cb.get());
    if (!host.attach(window, context.get())) {
        std::fprintf(stderr, "Cannot create the JSON demo canvas.\n");
        return 1;
    }
    host.set_on_demand_render(true);
    ui::viewport last_view{};
    auto layout_window = [&] {
        int pixels_w = 0, pixels_h = 0;
        if (!SDL_GetWindowSizeInPixels(window->window, &pixels_w, &pixels_h) || pixels_w <= 0 || pixels_h <= 0) return;
        const float scale = window->content_scale();
        const ui::viewport view{static_cast<int>(std::lround(pixels_w / scale)),
            static_cast<int>(std::lround(pixels_h / scale)), scale};
        if (view.width != last_view.width || view.height != last_view.height || view.scale != last_view.scale) {
            form.tree.layout(view);
            last_view = view;
        }
    };
    window->sync_ui_scale();
    layout_window();
    Uint64 last_frame = SDL_GetTicks();
    while (app.get_event() >= 0 && !window->pending_close) {
        layout_window();
        form.service();
        const Uint64 now = SDL_GetTicks();
        const float delta = static_cast<float>(now - last_frame) / 1000.0f;
        form.tree.advance_animations(delta);
        host.begin_frame(delta);
        last_frame = now;
        host.draw_all();
        host.end_frame();
    }
    app.wait_gpu();
    window->viewport->ui = nullptr; // The viewport borrows our context.
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    const std::string_view option = argc == 2 ? argv[1] : "";
    if (argc > 2 || option == "--help") {
        std::puts("Usage: vgui_ui_json_demo [design.vgui.json | design.vgui.package.json | --smoke-test | --smoke-test-package]");
        return argc > 2 ? 2 : 0;
    }
    constexpr char default_form[] = VGUI_SOURCE_DIR "/sample/res/form.vgui.json";
    if (option == "--smoke-test-package") return smoke_package(fs::u8path(default_form));
    const bool smoke = option == "--smoke-test";
    const char* filename = argc == 2 && !smoke ? argv[1] : default_form;
    form_input input;
    ui::diagnostic error;
    if (!read_form(fs::u8path(filename), input, error)) {
        std::fprintf(stderr, "Cannot load %s (%s): %s\n", filename, error.path.c_str(), error.message.c_str());
        return 1;
    }
    return smoke ? smoke_test(input) : run_window(input);
}
