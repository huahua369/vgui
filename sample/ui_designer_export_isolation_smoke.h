#pragma once

#include "delivery.h"
#include "ui_designer_image_smoke_fixture.h"
#include "ui_designer_menu_smoke.h"
#include "ui_designer_preview.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace vgui::ui_design {

// Run after exercise_pages. Leaves the two scratch tabs dirty, with B active.
template <class Editor, class Picker>
void exercise_export_isolation(os_window& win, vgui_context* shell, Editor& editor,
                               const std::function<void()>& frame, Picker& picker,
                               const std::filesystem::path& form_fixture) {
    namespace fs = std::filesystem;
    const native_menu_input menus(win, shell, frame);
    std::string_view step = "setup";
    const auto require = [&](bool passed, std::string_view message) {
        if (!passed) throw std::runtime_error("Native export isolation (" + std::string(step) + "): " + std::string(message));
    };
    const auto bytes = [&](const fs::path& path) {
        std::ifstream in(path, std::ios::binary);
        require(bool(in), "missing or unreadable file: " + path.generic_string());
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    };
    const auto find_tabs = [](const auto& self, vgui_widget* widget) -> vgui_widget* {
        if (std::string_view(vgui_widget_get_id(widget)) == "designer.page_tabs") return widget;
        for (std::size_t i = 0; i < vgui_widget_get_child_count(widget); ++i)
            if (auto* found = self(self, vgui_widget_get_child(widget, i))) return found;
        return nullptr;
    };
    auto* tabs = find_tabs(find_tabs, vgui_root(shell));
    require(tabs && vgui_widget_get_kind(tabs) == VGUI_TABVIEW, "missing page tabs");
    const auto prior_count = vgui_tab_get_count(tabs);
    const auto tab_state = [&] {
        std::vector<std::tuple<vgui_item_id, std::string, bool>> result;
        for (int i = 0; i < vgui_tab_get_count(tabs); ++i) {
            const auto id = vgui_tab_get_at(tabs, i);
            result.emplace_back(id, vgui_tab_get_title(tabs, id), vgui_tab_get_unsaved(tabs, id) != 0);
        }
        return result;
    };
    auto expected_tabs = tab_state();
    for (const auto& prior : expected_tabs) require(!std::get<2>(prior), "expected clean prior tabs");

    using preset = designer_preview::preset;
    struct page_state {
        document current, committed;
        std::string selection;
        std::uint64_t revision;
        bool dirty, undo, redo;
        std::tuple<preset, bool, float, float, float, bool> view;
        bool operator==(const page_state&) const = default;
    };
    const auto page = [&] {
        const auto& history = editor.history();
        const auto v = editor.preview().capture_view();
        return page_state{history.current(), history.committed(), std::string(editor.selection()),
            history.revision(), history.dirty(), history.can_undo(), history.can_redo(),
            {v.screen, v.fit, v.zoom, v.pan_x, v.pan_y, v.running}};
    };
    const auto open = [&](const fs::path& path) {
        require(editor.open(path), "could not activate " + path.generic_string());
        frame();
    };
    const auto expect_active = [&](vgui_item_id id, const page_state& state) {
        require(vgui_tab_get_active(tabs) == id && page() == state,
            "wrong active tab or changed document, history, selection or view");
    };

    constexpr auto filename = "isolation.vgui.json";
    constexpr auto package_name = "isolation-delivery";
    const auto a_file = picker.sandbox / "export-a" / filename;
    const auto b_file = picker.sandbox / "export-b" / filename;
    const auto package = picker.package_parent / package_name;
    document saved;
    require(read_document(form_fixture, saved) && saved.resources.size() == 1 &&
        saved.resources.contains(smoke_badge_key) && saved.resources.at(smoke_badge_key) == smoke_badge_file,
        "expected the saved form with its generated badge");
    const auto saved_bytes = bytes(form_fixture);
    const auto fixture_asset = form_fixture.parent_path() / smoke_badge_file;
    const auto a_asset = a_file.parent_path() / smoke_badge_file, b_asset = b_file.parent_path() / smoke_badge_file;
    const auto a_bytes = bytes(fixture_asset);
    require(fs::create_directory(a_file.parent_path()) && fs::create_directory(b_file.parent_path()) &&
        fs::copy_file(form_fixture, a_file) && fs::copy_file(form_fixture, b_file) && fs::copy_file(fixture_asset, a_asset) &&
        write_smoke_badge(b_asset, true), "could not create the two scratch projects");
    const auto b_bytes = bytes(b_asset);
    require(a_bytes != b_bytes, "expected different images at the same relative path");

    step = "independent dirty pages";
    const auto open_scratch = [&](const fs::path& path, int index) {
        open(path);
        const auto id = vgui_tab_get_at(tabs, index);
        const auto clean = page();
        require(id && vgui_tab_get_count(tabs) == index + 1 && vgui_tab_get_active(tabs) == id &&
            clean.current == saved && clean.committed == saved && !clean.dirty && !clean.undo && !clean.redo,
            "expected a new clean copy of the saved form");
        expected_tabs.emplace_back(id, filename, true); // Both must be dirty before export.
        return id;
    };
    const auto add = [&](int index) { menus.activate(menus.activate(menus.open(2), 0), index); };
    const auto a_tab = open_scratch(a_file, prior_count);
    add(1); // Resources > New Control > Label
    menus.activate(menus.open(3), 4); // View > Phone
    const auto a = page();
    require(a.dirty && a.undo && !a.redo && a.current == a.committed && a.current.resources == saved.resources &&
        a.current.nodes.size() == saved.nodes.size() + 1 && a.current.nodes.at(a.selection).type == "label" &&
        std::get<0>(a.view) == preset::phone, "A did not keep an unsaved Label and its own preview preset");

    const auto b_tab = open_scratch(b_file, prior_count + 1);
    add(2); // Button, so B's unsaved document is not A's.
    const auto b_first = page();
    add(3); // Edit
    const auto b_second = page();
    menus.activate(menus.open(1), 0); // Edit > Undo; leave B dirty with a redo entry.
    const auto b = page();
    require(b.dirty && b.undo && b.redo && b.current == b.committed && b.current == b_first.current &&
        b.current != a.current && b.current.resources == saved.resources && b_second.current != b.current &&
        b.selection == saved.root && b.selection != a.selection && b.view != a.view && tab_state() == expected_tabs,
        "expected B to be independently dirty with a different selection, view and redo entry");

    step = "late folder result";
    open(a_file);
    expect_active(a_tab, a);
    picker.defer_folders = true;
    const auto requests = picker.folder_requests;
    menus.activate(menus.open(0), 6); // File > Export Package
    require(editor.chooser_open() && picker.folder_requests == requests + 1 && !vgui_modal_active(shell),
        "export did not leave a folder request outstanding");
    // The picker blocks interactive SDL tab selection. Duplicate Open models a
    // host-driven activation without creating or reloading a tab.
    open(b_file);
    expect_active(b_tab, b);
    const auto* preview = editor.preview().snapshot();
    require(preview != nullptr, "missing B preview after host activation");
    const auto preview_revision = preview->revision;
    require(editor.chooser_open() && !vgui_modal_active(shell) && picker.complete_folder(picker.package_parent),
        "could not post the delayed folder result while B was active");
    picker.defer_folders = false;
    for (int i = 0; i < 4 && editor.chooser_open(); ++i) frame();
    auto* input = vgui_dialog_input(shell);
    require(!editor.chooser_open() && vgui_modal_active(shell) && input && vgui_get_focus(shell) == input &&
        std::string_view(vgui_widget_get_text(input)) == package_name && !fs::exists(package),
        "expected a focused prompt with the shared default name and no package before confirmation");
    require(vgui_tab_get_active(tabs) == b_tab, "completing the chooser switched away from B");

    step = "confirm and verify A's delivery";
    menus.key(SDLK_RETURN, SDL_SCANCODE_RETURN);
    require(!vgui_modal_active(shell), "Enter did not confirm the package name");
    expect_active(b_tab, b);
    preview = editor.preview().snapshot();
    require(preview && preview->revision == preview_revision && tab_state() == expected_tabs,
        "export remounted B or changed the tab order, titles or unsaved markers");
    document delivered;
    diagnostic error;
    // read_delivery validates the canonical manifest and recomputes the SHA-256 of the document and assets.
    if (!read_delivery(package, delivered, &error)) require(false, error.path + ": " + error.message);
    const auto badge = delivered.resources.find(smoke_badge_key);
    require(badge != delivered.resources.end() && badge->second != smoke_badge_file, "badge was not remapped");
    const auto exported_bytes = bytes(package / fs::u8path(badge->second));
    require(exported_bytes == a_bytes && exported_bytes != b_bytes, "packaged badge did not come from A's project root");
    delivered.resources.at(smoke_badge_key) = smoke_badge_file;
    require(delivered == a.current && delivered != b.current && delivered != saved, "export did not contain A's unsaved edits");
    require(bytes(a_file) == saved_bytes && bytes(b_file) == saved_bytes && bytes(form_fixture) == saved_bytes &&
        bytes(a_asset) == a_bytes && bytes(b_asset) == b_bytes && bytes(fixture_asset) == a_bytes,
        "export rewrote a source document or asset");

    step = "source history survived export";
    open(a_file);
    expect_active(a_tab, a);
    open(b_file);
    expect_active(b_tab, b);
    menus.activate(menus.open(1), 1); // B's redo must still replay its own Edit control.
    auto redone = b_second;
    redone.selection = b.selection;
    redone.revision = b.revision + 1;
    expect_active(b_tab, redone);
    menus.activate(menus.open(1), 0);
    auto restored = b;
    restored.revision += 2;
    expect_active(b_tab, restored);
}

} // namespace vgui::ui_design
