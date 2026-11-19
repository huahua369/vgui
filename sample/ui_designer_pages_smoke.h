#pragma once

#include "ui_designer_locale.h"
#include "ui_designer_menu_smoke.h"
#include "ui_designer_preview.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace vgui::ui_design {

// Run after opening the absolute fixture, with the edited form still open.
// All actions except duplicate Open go through the regular SDL event loop.
template <class Editor>
void exercise_pages(os_window& win, vgui_context* shell, Editor& editor, const std::function<void()>& frame,
                    std::filesystem::path& save_target, const std::filesystem::path& sandbox) {
    const native_menu_input menus(win, shell, frame);
    std::string_view step = "initial pages";
    const auto require = [&](bool passed, std::string_view message) {
        if (!passed) throw std::runtime_error("Native design pages (" + std::string(step) + "): " + std::string(message));
    };
    const auto find = [](const auto& self, vgui_widget* w, std::string_view id) -> vgui_widget* {
        if (!w || id == vgui_widget_get_id(w)) return w;
        for (std::size_t i = 0; i < vgui_widget_get_child_count(w); ++i)
            if (auto* match = self(self, vgui_widget_get_child(w, i), id)) return match;
        return nullptr;
    };
    auto* tabs = find(find, vgui_root(shell), "designer.page_tabs");
    require(tabs && vgui_widget_get_kind(tabs) == VGUI_TABVIEW && vgui_tab_get_count(tabs) == 2,
        "expected two fixture tabs");
    const auto click = [&](vgui_widget* target, float x, float y) {
        require(vgui_hit_test(shell, x, y) == target, "pointer does not hit the intended control");
        for (bool down : {true, false}) {
            SDL_Event e{};
            e.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
            e.button.windowID = win.id;
            e.button.button = SDL_BUTTON_LEFT;
            e.button.down = down;
            e.button.clicks = 1;
            e.button.x = x / win.pixel_ratio();
            e.button.y = y / win.pixel_ratio();
            require(SDL_PushEvent(&e), "could not enqueue SDL pointer event");
        }
        frame();
    };
    const auto tab_rect = [&](vgui_item_id id) {
        pixel_rect r;
        require(vgui_tab_get_rect(tabs, id, &r.x, &r.y, &r.width, &r.height) && r.width > 0 && r.height > 0,
            "tab has no pointer geometry");
        return r;
    };
    const auto select = [&](vgui_item_id id) {
        const auto r = tab_rect(id);
        click(tabs, r.x + r.width / 2, r.y + r.height / 2);
        require(vgui_tab_get_active(tabs) == id, "pointer did not activate the requested tab");
    };
    const auto dirty = [&](vgui_item_id id) { return vgui_tab_get_unsaved(tabs, id) != 0; };
    auto& preview = editor.preview();
    const auto& history = editor.history();
    struct model_state {
        document current, committed;
        std::string selection;
        std::uint64_t revision;
        bool dirty, undo, redo;
        bool operator==(const model_state&) const = default;
    };
    const auto model = [&] {
        return model_state{history.current(), history.committed(), std::string(editor.selection()),
            history.revision(), history.dirty(), history.can_undo(), history.can_redo()};
    };
    struct page_state {
        model_state model;
        designer_preview::view_state view;
        viewport layout_view;
        pixel_rect board;
    };
    const auto page = [&] {
        const auto* layout = preview.snapshot();
        const auto board = preview.artboard_rect_px();
        const auto& doc = history.current();
        require(layout && board && layout->nodes.size() == doc.nodes.size() && !layout->nodes.empty() &&
            layout->nodes.front().id == doc.root && std::all_of(layout->nodes.begin(), layout->nodes.end(),
                [&](const auto& entry) { return doc.nodes.contains(entry.id); }), "preview is not mounted for the current page");
        return page_state{model(), preview.capture_view(), layout->view, *board};
    };
    const auto expect = [&](vgui_item_id id, const page_state& before) {
        const auto after = page();
        require(vgui_tab_get_active(tabs) == id && after.model == before.model && dirty(id) == before.model.dirty,
            "tab changed the active document, history, selection or unsaved marker");
        require(after.view.screen == before.view.screen && after.view.running == before.view.running &&
            after.view.fit == before.view.fit && after.view.zoom == before.view.zoom &&
            after.layout_view.width == before.layout_view.width && after.layout_view.height == before.layout_view.height &&
            after.board.width == before.board.width && after.board.height == before.board.height,
            "tab did not restore its viewport, mode or zoom");
    };

    const auto form_tab = vgui_tab_get_at(tabs, 0), absolute_tab = vgui_tab_get_at(tabs, 1);
    require(std::string_view(vgui_tab_get_title(tabs, form_tab)) == "form.vgui.json" &&
        std::string_view(vgui_tab_get_title(tabs, absolute_tab)) == "absolute-form.vgui.json" &&
        vgui_tab_get_active(tabs) == absolute_tab && history.current().root == "absolute_form",
        "expected the form and the active absolute fixture");
    const auto absolute = page();
    require(dirty(form_tab) && dirty(absolute_tab) == absolute.model.dirty, "incorrect fixture unsaved markers");

    step = "restore the original form";
    select(form_tab);
    const auto form = page();
    require(form.model.dirty && form.model.undo && form.model.current.nodes.contains("submit") &&
        form.view.screen == designer_preview::preset::phone && form.view.running,
        "form lost its delivery edit or Phone/Run view");
    require(preview.edit_text("name") == "", "returning to the form restored temporary runtime input");
    require(!SDL_TextInputActive(win.window), "returning to the form left SDL text input active");
    select(absolute_tab);
    expect(absolute_tab, absolute);

    step = "create two independent pages";
    const auto create = [&](int index) {
        menus.activate(menus.open(0), 0); // File > New Page
        const auto id = vgui_tab_get_at(tabs, index);
        require(id && vgui_tab_get_count(tabs) == index + 1 && vgui_tab_get_active(tabs) == id && !dirty(id),
            "New Page did not activate a new clean tab");
        require(!history.dirty() && !history.can_undo() && !history.can_redo() &&
            history.current() == make_document() && editor.selection() == history.current().root,
            "new tab inherited the previous document, selection or history");
        const auto view = preview.capture_view();
        require(view.screen == designer_preview::preset::desktop && view.fit && !view.running,
            "new tab inherited the previous preview view");
        return id;
    };
    const auto add = [&](int index, std::string_view type, const model_state& before) {
        menus.activate(menus.activate(menus.open(2), 0), index); // Resources > New Control
        const auto after = page();
        const auto& added = after.model;
        const auto it = added.current.nodes.find(added.selection);
        require(it != added.current.nodes.end() && it->second.type == type &&
            added.current.nodes.size() == before.current.nodes.size() + 1 && added.current == added.committed &&
            added.revision == before.revision + 1 && added.dirty && added.undo && !added.redo,
            "New Control did not add and select one committed control");
        return after;
    };
    const auto label_tab = create(2);
    const auto clean = model();
    const auto label = add(1, "label", clean);
    const auto button_tab = create(3);
    require(model() == clean, "new pages did not start with identical model revisions and contents");
    const auto button = add(2, "button", clean);
    require(button.model.revision == label.model.revision && button.model.selection != label.model.selection &&
        dirty(label_tab) && dirty(button_tab), "expected two dirty pages with different selections at the same revision");

    step = "switch at equal model revisions";
    select(label_tab);
    expect(label_tab, label);
    select(button_tab);
    expect(button_tab, button);

    step = "preserve page-specific view and history";
    select(label_tab);
    menus.activate(menus.open(3), 3); // View > Tablet
    menus.activate(menus.open(3), 6); // View > Actual Size
    auto* zoom_in = find(find, vgui_root(shell), "designer.preview.in");
    require(zoom_in != nullptr, "missing Zoom In button");
    pixel_rect zoom_button;
    vgui_widget_get_rect(zoom_in, &zoom_button.x, &zoom_button.y, &zoom_button.width, &zoom_button.height);
    click(zoom_in, zoom_button.x + zoom_button.width / 2, zoom_button.y + zoom_button.height / 2);
    const auto zoomed_label = page();
    require(zoomed_label.model == label.model && zoomed_label.view.screen == designer_preview::preset::tablet &&
        !zoomed_label.view.fit && zoomed_label.view.zoom > 1, "view controls did not establish an independent zoom");
    select(button_tab);
    expect(button_tab, button);
    menus.activate(menus.open(1), 0); // Edit > Undo
    const auto undone = page();
    auto expected = clean;
    expected.revision = button.model.revision + 1;
    expected.redo = true;
    require(undone.model == expected && !dirty(button_tab) && dirty(label_tab),
        "Undo did not clean just the active tab and keep its redo transaction");
    select(label_tab);
    expect(label_tab, zoomed_label);
    select(button_tab);
    expect(button_tab, undone);

    step = "close only the clean page";
    require(!dirty(button_tab) && vgui_tab_get_closeable(tabs, button_tab), "expected a clean, closeable tab");
    auto* file = menus.open(0);
    const std::string_view close_title = editor.strings().text("workbench.menu.close_page", "Close Page");
    int close_index = 0;
    for (; close_index < vgui_menu_get_count(file); ++close_index) {
        if (close_title == vgui_menu_get_item_text(file, vgui_menu_get_item_at(file, close_index))) break;
    }
    require(close_index < vgui_menu_get_count(file), "missing File > Close Page");
    menus.activate(file, close_index);
    require(vgui_tab_get_count(tabs) == 3 && vgui_tab_get_index(tabs, button_tab) < 0 && !vgui_modal_active(shell),
        "clean tab close did not remove exactly that page without a prompt");
    expect(label_tab, zoomed_label);

    step = "reopen the same form path";
    require(editor.open(std::filesystem::path(VGUI_SOURCE_DIR "/sample/res/./form.vgui.json")), "duplicate Open failed");
    frame();
    require(vgui_tab_get_count(tabs) == 3 && vgui_tab_get_at(tabs, 0) == form_tab &&
        vgui_tab_get_at(tabs, 1) == absolute_tab && vgui_tab_get_at(tabs, 2) == label_tab,
        "duplicate Open created or replaced a tab");
    expect(form_tab, form);
    require(dirty(label_tab) && dirty(absolute_tab) == absolute.model.dirty,
        "closing and reopening changed an inactive tab's unsaved marker");

    step = "save only the active page";
    select(label_tab);
    const auto saved_file = sandbox / "saved-label.vgui.json";
    save_target = saved_file;
    menus.activate(menus.open(0), 2); // File > Save
    for (int i = 0; i < 4 && editor.chooser_open(); ++i) frame();
    document saved;
    require(!editor.chooser_open() && read_document(saved_file, saved) && saved == zoomed_label.model.current &&
        !history.dirty() && !dirty(label_tab) && history.can_undo() && dirty(form_tab) &&
        std::string_view(vgui_tab_get_title(tabs, label_tab)) == "saved-label.vgui.json",
        "saving the active tab did not keep the other page dirty or preserve this page's undo history");
    select(form_tab);
    expect(form_tab, form);
}

} // namespace vgui::ui_design
