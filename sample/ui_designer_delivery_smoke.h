#pragma once

#include "delivery.h"
#include "ui_designer_locale.h"

#include "ui_designer_menu_smoke.h"

#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>

namespace vgui::ui_design {

// Run after the runtime smoke: the clean form fixture is still open and Submit is selected.
// Drive the workbench through the real SDL event loop; only the folder picker is mocked.
inline void exercise_delivery(os_window& win, vgui_context* shell, const session& history,
                              const std::function<void()>& frame, const std::filesystem::path& source,
                              const std::filesystem::path& parent, bool& cancel_folder, const designer_strings& strings) {
    const native_menu_input menus(win, shell, frame);
    namespace fs = std::filesystem;
    std::string_view step = "setup";
    const auto require = [&](bool passed, std::string_view message) {
        if (!passed) throw std::runtime_error("Native delivery (" + std::string(step) + "): " + std::string(message));
    };
    const auto push = [&](SDL_Event e) {
        if (!SDL_PushEvent(&e)) require(false, std::string("could not queue SDL event: ") + SDL_GetError());
    };
    const auto find = [](const auto& self, vgui_widget* w, std::string_view id) -> vgui_widget* {
        if (!w || id == vgui_widget_get_id(w)) return w;
        for (std::size_t i = 0; i < vgui_widget_get_child_count(w); ++i)
            if (auto* match = self(self, vgui_widget_get_child(w, i), id)) return match;
        return nullptr;
    };
    const auto widget = [&](std::string_view id) {
        auto* w = find(find, vgui_root(shell), id);
        require(w != nullptr, id);
        return w;
    };
    const auto click = [&](std::string_view id) {
        auto* target = widget(id);
        float x, y, w, h;
        vgui_widget_get_rect(target, &x, &y, &w, &h);
        x += w / 2;
        y += h / 2;
        require(w > 0 && h > 0 && vgui_hit_test(shell, x, y) == target, id);
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
    const auto key = [&](SDL_Keycode code, SDL_Scancode scan, SDL_Keymod mod = SDL_KMOD_NONE) {
        for (bool down : {true, false}) {
            SDL_Event e{};
            e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
            e.key.windowID = win.id;
            e.key.key = code;
            e.key.scancode = scan;
            e.key.mod = mod;
            e.key.down = down;
            push(e);
        }
        frame();
    };
    const auto bytes = [&](const fs::path& file) {
        std::ifstream in(file, std::ios::binary);
        require(bool(in), "missing or unreadable file: " + file.generic_string());
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    };
    struct state {
        document current, committed;
        std::uint64_t revision;
        bool dirty, undo, redo;
        std::string title;
        bool operator==(const state&) const = default;
    };
    const auto snapshot = [&] {
        return state{history.current(), history.committed(), history.revision(), history.dirty(),
            history.can_undo(), history.can_redo(), SDL_GetWindowTitle(win.window)};
    };
    const auto original_bytes = bytes(source);
    const auto original_mtime = fs::last_write_time(source);
    const auto unchanged = [&](const state& before) {
        require(snapshot() == before, "export changed the source title, document, revision, dirty flag or history");
        require(fs::last_write_time(source) == original_mtime && bytes(source) == original_bytes,
            "export rewrote the source document");
    };
    const auto status = [&] { return std::string_view(vgui_widget_get_text(widget("designer.status"))); };
    constexpr auto export_id = "designer.file.export";
    constexpr auto default_name = "form-delivery";
    const auto open_prompt = [&] {
        click(export_id);
        // The mock result is posted asynchronously and serviced on a later frame.
        for (int i = 0; i < 4 && !vgui_modal_active(shell); ++i) frame();
        auto* input = vgui_dialog_input(shell);
        require(input && vgui_get_focus(shell) == input, "folder selection did not open a focused prompt");
        require(SDL_TextInputActive(win.window), "focused prompt did not enable native text input");
        require(std::string_view(vgui_widget_get_text(input)) == default_name, "wrong default package name");
        return input;
    };
    const auto replace_prompt_name = [&](vgui_widget* input, const char* name) {
        key(SDLK_A, SDL_SCANCODE_A, SDL_KMOD_CTRL);
        SDL_Event text{};
        text.type = SDL_EVENT_TEXT_INPUT;
        text.text.windowID = win.id;
        text.text.text = name;
        push(text);
        frame();
        auto* typed_input = vgui_dialog_input(shell);
        require(typed_input == input && std::string_view(vgui_widget_get_text(typed_input)) == name,
            "native text did not replace the prompt value");
    };

    const auto initial = snapshot();
    document source_doc;
    require(read_document(source, source_doc) && initial.current == source_doc && initial.committed == source_doc &&
        !initial.dirty && !initial.undo && !initial.redo && fs::is_empty(parent), "expected the clean form and an empty destination");

    step = "unsaved edit";
    menus.activate(menus.activate(menus.open(2), 0), 1);
    const auto edited = snapshot();
    auto expected = source_doc;
    const auto& children = edited.current.nodes.at("footer").children;
    require(children.size() == expected.nodes.at("footer").children.size() + 1, "Resources menu did not add a sibling of Submit");
    const auto& label_id = children.back();
    require(expected.nodes.emplace(label_id, node{"label", {}, {{"width", 120.0}, {"height", 24.0}}, {}}).second,
        "Resources menu did not create a fresh Label with its initial size");
    expected.nodes.at("footer").children.push_back(label_id);
    require(edited.current == expected && edited.committed == expected && edited.revision == initial.revision + 1 &&
        edited.dirty && edited.undo && !edited.redo, "expected exactly one dirty structural edit");

    step = "folder cancel";
    cancel_folder = true;
    menus.activate(menus.open(0), 6); // File > Export Package
    for (int i = 0; i < 4 && status() != strings.text("file.chooser_cancelled", "File selection cancelled."); ++i) frame();
    cancel_folder = false;
    require(status() == strings.text("file.chooser_cancelled", "File selection cancelled.") && !vgui_modal_active(shell) && fs::is_empty(parent),
        "cancelling the folder chooser did not end the export without creating files");
    unchanged(edited);

    step = "prompt cancel";
    replace_prompt_name(open_prompt(), "cancelled-delivery");
    key(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE);
    require(!vgui_modal_active(shell) && fs::is_empty(parent), "Esc did not cancel the prompt without creating files");
    unchanged(edited);

    step = "reject path components";
    replace_prompt_name(open_prompt(), "../escaped-delivery");
    key(SDLK_RETURN, SDL_SCANCODE_RETURN);
    require(!vgui_modal_active(shell) && status() == strings.text("export.bad_name", "Export failed: enter a single folder name (no path separators or trailing dots/spaces).") && fs::is_empty(parent) &&
        !fs::exists(parent.parent_path() / "escaped-delivery"), "package name was treated as a path");
    unchanged(edited);

    step = "export current document";
    constexpr auto package_name = "form-review";
    replace_prompt_name(open_prompt(), package_name);
    require(fs::is_empty(parent), "export started before confirmation");
    key(SDLK_RETURN, SDL_SCANCODE_RETURN);
    require(!vgui_modal_active(shell), "Enter did not close the prompt");
    require(status().starts_with(strings.text("export.done", "Exported package to ")), "successful export did not report the package location");
    const auto package = parent / package_name;
    document delivered;
    diagnostic error;
    // read_delivery verifies the canonical manifest, including sizes and SHA-256 of the document and every asset.
    if (!read_delivery(package, delivered, &error)) require(false, error.path + ": " + error.message);
    require(delivered.resources.size() == expected.resources.size(), "export changed the resource keys");
    for (auto& [id, path] : expected.resources) {
        const auto it = delivered.resources.find(id);
        require(it != delivered.resources.end() && it->second != path, "resource path was not remapped");
        require(bytes(package / fs::u8path(it->second)) == bytes(source.parent_path() / fs::u8path(path)),
            "packaged resource differs from the project asset");
        path = it->second;
    }
    require(delivered == expected, "package did not contain the unsaved edit");
    unchanged(edited);

    step = "refuse overwrite";
    // Retry with a different document so replacing the old package cannot pass a byte-for-byte check.
    const auto manifest = bytes(package / delivery_manifest);
    menus.activate(menus.open(1), 0);
    const auto undone = snapshot();
    require(undone.current == source_doc && undone.committed == source_doc && !undone.dirty && !undone.undo && undone.redo,
        "export did not preserve the undo transaction");
    replace_prompt_name(open_prompt(), package_name);
    key(SDLK_RETURN, SDL_SCANCODE_RETURN);
    require(!vgui_modal_active(shell) && status().starts_with(strings.text("export.failed", "Export failed: ")), "same-name export was not rejected");
    document after_retry;
    require(read_delivery(package, after_retry) && after_retry == delivered &&
        bytes(package / delivery_manifest) == manifest, "second export changed the existing package");
    unchanged(undone);
    menus.activate(menus.open(1), 1);
    require(history.current() == edited.current && history.dirty() && history.can_undo() && !history.can_redo(),
        "duplicate export did not preserve the redo transaction");
}

} // namespace vgui::ui_design
