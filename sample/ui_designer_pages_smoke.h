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
// Drive the workbench through SDL apart from duplicate Open and mock chooser results.
template <class Editor, class Picker>
void exercise_pages(os_window& win, vgui_context* shell, Editor& editor, const std::function<void()>& frame,
                    Picker& picker, const std::filesystem::path& form_fixture,
                    const std::filesystem::path& absolute_fixture) {
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
        auto r = tab_rect(id);
        pixel_rect strip;
        vgui_widget_get_rect(tabs, &strip.x, &strip.y, &strip.width, &strip.height);
        const float x = strip.x + strip.width / 2, y = r.y + r.height / 2;
        // Save All can put more tabs on the strip than fit on screen.
        for (int i = 0; i < 8 && (r.x + r.width / 2 < strip.x || r.x + r.width / 2 >= strip.x + strip.width); ++i) {
            SDL_Event motion{}, wheel{};
            motion.type = SDL_EVENT_MOUSE_MOTION;
            motion.motion.windowID = win.id;
            motion.motion.x = x / win.pixel_ratio();
            motion.motion.y = y / win.pixel_ratio();
            wheel.type = SDL_EVENT_MOUSE_WHEEL;
            wheel.wheel.windowID = win.id;
            wheel.wheel.mouse_x = motion.motion.x;
            wheel.wheel.mouse_y = motion.motion.y;
            wheel.wheel.y = r.x + r.width / 2 < x ? 3.f : -3.f;
            require(SDL_PushEvent(&motion) && SDL_PushEvent(&wheel), "could not scroll the tab strip");
            frame();
            r = tab_rect(id);
        }
        click(tabs, r.x + r.width / 2, y);
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
            after.view.pan_x == before.view.pan_x && after.view.pan_y == before.view.pan_y &&
            after.layout_view.width == before.layout_view.width && after.layout_view.height == before.layout_view.height &&
            after.layout_view.scale == before.layout_view.scale &&
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
    require(editor.open(form_fixture.parent_path() / "." / form_fixture.filename()), "duplicate Open failed");
    frame();
    require(vgui_tab_get_count(tabs) == 3 && vgui_tab_get_at(tabs, 0) == form_tab &&
        vgui_tab_get_at(tabs, 1) == absolute_tab && vgui_tab_get_at(tabs, 2) == label_tab,
        "duplicate Open created or replaced a tab");
    expect(form_tab, form);
    require(dirty(label_tab) && dirty(absolute_tab) == absolute.model.dirty,
        "closing and reopening changed an inactive tab's unsaved marker");

    step = "save only the active page";
    select(label_tab);
    const auto saved_file = picker.sandbox / "saved-label.vgui.json";
    picker.save_target = saved_file;
    menus.activate(menus.open(0), 2); // File > Save
    for (int i = 0; i < 4 && editor.chooser_open(); ++i) frame();
    document saved;
    require(!editor.chooser_open() && read_document(saved_file, saved) && saved == zoomed_label.model.current &&
        !history.dirty() && !dirty(label_tab) && history.can_undo() && dirty(form_tab) &&
        std::string_view(vgui_tab_get_title(tabs, label_tab)) == "saved-label.vgui.json",
        "saving the active tab did not keep the other page dirty or preserve this page's undo history");
    select(form_tab);
    expect(form_tab, form);

    step = "prepare Save All pages";
    namespace fs = std::filesystem;
    const auto title = [&](vgui_item_id id) { return std::string_view(vgui_tab_get_title(tabs, id)); };
    const auto saved_state = [](page_state before) {
        before.model.dirty = false;
        return before;
    };
    const auto expect_file = [&](const fs::path& path, const page_state& state) {
        document disk;
        require(read_document(path, disk) && disk == state.model.current,
            "unexpected saved document: " + path.filename().string());
    };
    document original_form, original_absolute;
    require(form.model.dirty && absolute.model.dirty && read_document(form_fixture, original_form) &&
        read_document(absolute_fixture, original_absolute) && original_form != form.model.current &&
        original_absolute != absolute.model.current, "expected two dirty named pages with unsaved edits");
    const auto label_write_time = fs::last_write_time(saved_file);
    const auto a_tab = create(3);
    const auto a = add(1, "label", clean);
    const auto b_tab = create(4);
    const auto b_first = add(2, "button", clean);
    const auto b_next = add(1, "label", b_first.model);
    menus.activate(menus.open(1), 0); // Keep a redo entry on dirty B.
    const auto b = page();
    require(b.model.current == b_first.model.current && b.model.dirty && b.model.undo && b.model.redo,
        "expected B to keep both undo and redo history");
    const auto c_tab = create(5);
    const auto c = add(3, "edit", clean);
    const std::string b_title(title(b_tab)), c_title(title(c_tab));
    const auto a_file = picker.sandbox / "a.vgui.json", b_file = picker.sandbox / "b.vgui.json",
        c_file = picker.sandbox / "c.vgui.json";
    select(label_tab);
    const auto saved_label = saved_state(zoomed_label);
    expect(label_tab, saved_label);

    picker.defer_saves = true;
    const int initial_requests = picker.save_requests;
    const auto save_all_title = editor.strings().text("workbench.menu.save_all", "Save All");
    constexpr int save_all_index = 4;
    const auto open_save_all_menu = [&] {
        auto* menu = menus.open(0);
        require(vgui_menu_get_count(menu) > save_all_index &&
            std::string_view(vgui_menu_get_item_text(menu, vgui_menu_get_item_at(menu, save_all_index))) == save_all_title,
            "missing File > Save All after Save As");
        return menu;
    };
    const auto save_all = [&] { menus.activate(open_save_all_menu(), save_all_index); };
    const auto expect_active = [&](std::uint64_t preview_revision) {
        expect(label_tab, saved_label);
        require(preview.snapshot()->revision == preview_revision && vgui_tab_get_count(tabs) == 6,
            "Save All remounted the active preview or changed the tab count");
    };
    const auto expect_chooser = [&](int requests, std::string_view suggestion, std::uint64_t revision) {
        require(editor.chooser_open() && picker.save_requests == initial_requests + requests &&
            picker.save_suggestion == suggestion, "expected one pending chooser suggesting " + std::string(suggestion));
        expect_active(revision);
    };
    const auto complete = [&](int requests, const fs::path& path = {}) {
        require(editor.chooser_open() && picker.complete_save(path), "could not post a delayed Save All result");
        // A continuation leaves chooser_open() true but increments save_requests.
        for (int i = 0; i < 4 && editor.chooser_open() && picker.save_requests == initial_requests + requests; ++i) frame();
    };
    const auto expect_earlier_saves = [&] {
        require(!dirty(form_tab) && !dirty(absolute_tab) && !dirty(a_tab) && title(a_tab) == "a.vgui.json",
            "earlier writes did not remain saved on their own tabs");
        expect_file(form_fixture, form);
        expect_file(absolute_fixture, absolute);
        expect_file(a_file, a);
        expect_file(saved_file, saved_label);
        require(fs::last_write_time(saved_file) == label_write_time, "Save All rewrote the already-clean label page");
    };
    auto* status = find(find, vgui_root(shell), "designer.status");
    require(status != nullptr, "missing workbench status");
    const auto expect_stopped = [&](int requests, std::uint64_t revision, std::string_view message) {
        frame();
        require(!editor.chooser_open() && picker.save_requests == initial_requests + requests && !vgui_modal_active(shell),
            "Save All did not stop without opening the next chooser");
        require(vgui_widget_get_text(status) == message, "Save All did not report why it stopped");
        expect_active(revision);
        expect_earlier_saves();
        require(dirty(b_tab) && dirty(c_tab) && title(b_tab) == b_title && title(c_tab) == c_title &&
            !fs::exists(b_file) && !fs::exists(c_file), "an uncompleted page was written, renamed or marked clean");
        select(b_tab);
        expect(b_tab, b);
        select(c_tab);
        expect(c_tab, c);
        select(label_tab);
        expect(label_tab, saved_label);
    };

    // The previous label and button pages consumed numbers 1 and 2.
    constexpr std::string_view a_suggestion = "untitled-3.vgui.json", b_suggestion = "untitled-4.vgui.json",
        c_suggestion = "untitled-5.vgui.json";
    step = "Save All saves named pages before the first chooser";
    auto preview_revision = preview.snapshot()->revision;
    save_all();
    frame(); // No result posted yet; the batch must still be waiting on A.
    expect_chooser(1, a_suggestion, preview_revision);
    require(!dirty(form_tab) && !dirty(absolute_tab) && dirty(a_tab) && dirty(b_tab) && dirty(c_tab) &&
        !fs::exists(a_file) && !fs::exists(b_file) && !fs::exists(c_file), "Save All saved the wrong pages before choosing a path");
    expect_file(form_fixture, form);
    expect_file(absolute_fixture, absolute);

    step = "cancel Save All after saving A";
    complete(1, a_file);
    expect_chooser(2, b_suggestion, preview_revision);
    require(!dirty(a_tab) && title(a_tab) == "a.vgui.json" && dirty(b_tab) && dirty(c_tab), "the first chooser did not save A only");
    expect_file(a_file, a);
    complete(2); // Cancel B; C must not be prompted.
    expect_stopped(2, preview_revision, editor.strings().text("file.chooser_cancelled", "File selection cancelled."));

    step = "Save All rejects a path belonging to another open page";
    preview_revision = preview.snapshot()->revision;
    save_all();
    expect_chooser(3, b_suggestion, preview_revision);
    complete(3, a_file); // B must not overwrite the now-named A.
    expect_stopped(3, preview_revision,
        editor.strings().text("file.save_as_open", "Save As cannot replace another open page."));

    step = "retry Save All with valid paths";
    preview_revision = preview.snapshot()->revision;
    save_all();
    expect_chooser(4, b_suggestion, preview_revision);
    complete(4, b_file);
    expect_chooser(5, c_suggestion, preview_revision);
    require(!dirty(b_tab) && title(b_tab) == "b.vgui.json" && dirty(c_tab) && title(c_tab) == c_title && !fs::exists(c_file),
        "the retry did not save only B before prompting for C");
    expect_file(b_file, b);
    complete(5, c_file);
    require(!editor.chooser_open() && picker.save_requests == initial_requests + 5 && !vgui_modal_active(shell) &&
        !dirty(c_tab) && title(c_tab) == "c.vgui.json", "the retry did not finish saving C");
    expect_active(preview_revision);
    expect_earlier_saves();
    expect_file(c_file, c);
    picker.defer_saves = false;

    step = "Save All preserves independent history and preview";
    select(form_tab);
    expect(form_tab, saved_state(form));
    select(absolute_tab);
    expect(absolute_tab, saved_state(absolute));
    select(a_tab);
    expect(a_tab, saved_state(a));
    select(c_tab);
    expect(c_tab, saved_state(c));
    select(b_tab);
    auto saved_b = saved_state(b);
    expect(b_tab, saved_b);
    menus.activate(menus.open(1), 1); // Replay the redo entry that survived cancel, failure and save.
    auto redone_b = b_next;
    redone_b.model.selection = b.model.selection;
    redone_b.model.revision = b.model.revision + 1;
    expect(b_tab, redone_b);
    expect_earlier_saves();
    require(!dirty(label_tab) && !dirty(c_tab), "Redo changed the dirty state of another page");
    menus.activate(menus.open(1), 0);
    saved_b.model.revision += 2;
    expect(b_tab, saved_b);
    expect_file(b_file, b);

    step = "disable Save All when every page is clean";
    select(label_tab);
    auto* clean_menu = open_save_all_menu();
    require(!vgui_menu_get_item_enabled(clean_menu, vgui_menu_get_item_at(clean_menu, save_all_index)),
        "File > Save All is enabled even though every page is clean");
    menus.key(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE);
    expect(label_tab, saved_label);
}

} // namespace vgui::ui_design
