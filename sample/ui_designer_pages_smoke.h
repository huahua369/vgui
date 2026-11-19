#pragma once

#include "ui_designer_locale.h"
#include "ui_designer_menu_smoke.h"
#include "ui_designer_preview.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace vgui::ui_design {

// Run after opening the absolute fixture, with the edited form still open.
// Drive the workbench through SDL apart from host activation and native dialog results.
template <class Editor, class Picker, class Prompt>
void exercise_pages(os_window& win, vgui_context* shell, Editor& editor, const std::function<void()>& frame,
                    Picker& picker, Prompt& prompt, const std::filesystem::path& form_fixture,
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
    const auto reveal_tab = [&](vgui_item_id id, float right_margin = 0.f) {
        auto r = tab_rect(id);
        const auto target_x = [&] { return right_margin > 0 ? r.x + r.width - right_margin : r.x + r.width / 2; };
        pixel_rect strip;
        vgui_widget_get_rect(tabs, &strip.x, &strip.y, &strip.width, &strip.height);
        const float x = strip.x + strip.width / 2, y = r.y + r.height / 2;
        // Save All can put more tabs on the strip than fit on screen.
        for (int i = 0; i < 8 && (target_x() < strip.x || target_x() >= strip.x + strip.width); ++i) {
            SDL_Event motion{}, wheel{};
            motion.type = SDL_EVENT_MOUSE_MOTION;
            motion.motion.windowID = win.id;
            motion.motion.x = x / win.pixel_ratio();
            motion.motion.y = y / win.pixel_ratio();
            wheel.type = SDL_EVENT_MOUSE_WHEEL;
            wheel.wheel.windowID = win.id;
            wheel.wheel.mouse_x = motion.motion.x;
            wheel.wheel.mouse_y = motion.motion.y;
            wheel.wheel.y = target_x() < x ? 3.f : -3.f;
            require(SDL_PushEvent(&motion) && SDL_PushEvent(&wheel), "could not scroll the tab strip");
            frame();
            r = tab_rect(id);
        }
        require(target_x() >= strip.x && target_x() < strip.x + strip.width, "could not reveal the tab target");
        return r;
    };
    const auto select = [&](vgui_item_id id) {
        const auto r = reveal_tab(id);
        click(tabs, r.x + r.width / 2, r.y + r.height / 2);
        require(vgui_tab_get_active(tabs) == id, "pointer did not activate the requested tab");
    };
    const auto close_inactive_tab = [&](vgui_item_id id) {
        require(vgui_tab_get_active(tabs) != id && vgui_tab_get_closeable(tabs, id),
            "need an inactive, closeable tab");
        // Default tab style: 12 px horizontal padding, 16 px close target.
        const float margin = (12.f + 16.f) / 2 * vgui_context_get_ui_scale(shell);
        const auto r = reveal_tab(id, margin);
        click(tabs, r.x + r.width - margin, r.y + r.height / 2);
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

    const auto tab_ids = [&] {
        std::vector<vgui_item_id> ids;
        for (int i = 0; i < vgui_tab_get_count(tabs); ++i) ids.push_back(vgui_tab_get_at(tabs, i));
        return ids;
    };
    const auto file_bytes = [&](const fs::path& path) {
        std::ifstream file(path, std::ios::binary);
        require(bool(file), "cannot read " + path.filename().string());
        return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    };
    const auto request_exit = [&] {
        SDL_Event event{};
        event.type = SDL_EVENT_WINDOW_CLOSE_REQUESTED;
        event.window.windowID = win.id;
        require(SDL_PushEvent(&event), "cannot enqueue the window close request");
        frame();
    };
    const auto expect_no_dialog = [&] {
        require(prompt.empty() && !editor.chooser_open() && !editor.quitting(),
            "expected all prompts consumed, no pending chooser and the window still open");
    };

    step = "cancel then discard an inactive dirty tab";
    auto expected_tabs = tab_ids();
    const auto b_bytes = file_bytes(b_file);
    const int saves_before_close = picker.save_requests;
    select(b_tab);
    menus.activate(menus.open(1), 1); // Make the saved B dirty without changing its path.
    auto dirty_b = redone_b;
    dirty_b.model.revision += 2;
    expect(b_tab, dirty_b);
    select(label_tab);
    prompt.expect(title(b_tab), Prompt::cancel);
    close_inactive_tab(b_tab);
    expect_no_dialog();
    expect(label_tab, saved_label);
    require(tab_ids() == expected_tabs && file_bytes(b_file) == b_bytes, "cancel changed the tabs or wrote B");
    select(b_tab);
    expect(b_tab, dirty_b);
    select(label_tab);
    prompt.expect(title(b_tab), Prompt::discard);
    close_inactive_tab(b_tab);
    expect_no_dialog();
    expect(label_tab, saved_label);
    std::erase(expected_tabs, b_tab);
    require(tab_ids() == expected_tabs && file_bytes(b_file) == b_bytes && picker.save_requests == saves_before_close,
        "discard did not close just B without saving it");

    step = "cancel Save As while closing an inactive tab";
    const auto unnamed_tab = create(vgui_tab_get_count(tabs));
    const auto unnamed = add(3, "edit", clean);
    const std::string unnamed_title(title(unnamed_tab));
    const auto with_unnamed = tab_ids();
    const auto close_file = picker.sandbox / "closed-by-save.vgui.json";
    select(label_tab);
    picker.defer_saves = true;
    const auto expect_save_prompt = [&](int requests) {
        require(prompt.empty() && editor.chooser_open() && !editor.quitting() &&
            picker.save_requests == saves_before_close + requests && tab_ids() == with_unnamed &&
            dirty(unnamed_tab) && title(unnamed_tab) == unnamed_title && !fs::exists(close_file),
            "expected a pending Save As without removing, renaming or writing a tab");
        expect(label_tab, saved_label);
    };
    const auto complete_save = [&](const fs::path& path = {}) {
        require(editor.chooser_open() && picker.complete_save(path), "cannot post the delayed Save As result");
        for (int i = 0; i < 4 && editor.chooser_open(); ++i) frame();
        expect_no_dialog();
    };
    const auto expect_cancelled_save = [&] {
        expect(label_tab, saved_label);
        require(tab_ids() == with_unnamed && title(unnamed_tab) == unnamed_title && !fs::exists(close_file),
            "cancelled Save As renamed or closed a tab, or wrote a file");
        select(unnamed_tab);
        expect(unnamed_tab, unnamed);
        select(label_tab);
    };
    prompt.expect(unnamed_title, Prompt::save);
    close_inactive_tab(unnamed_tab);
    expect_save_prompt(1);
    complete_save();
    expect_cancelled_save();

    step = "cancel exit in the Save As picker";
    prompt.expect(unnamed_title, Prompt::save);
    request_exit();
    expect_save_prompt(2);
    complete_save();
    expect_cancelled_save();

    step = "late Save As closes only its original tab";
    prompt.expect(unnamed_title, Prompt::save);
    close_inactive_tab(unnamed_tab);
    expect_save_prompt(3);
    // The OS picker blocks interactive tab clicks. Simulate a host activation
    // while it is pending by reopening the already-open absolute design.
    require(editor.open(absolute_fixture), "could not activate the other page during Save As");
    frame();
    expect(absolute_tab, saved_state(absolute));
    complete_save(close_file);
    expect(absolute_tab, saved_state(absolute));
    require(tab_ids() == expected_tabs && picker.save_requests == saves_before_close + 3,
        "the late result did not close just the unnamed origin tab");
    expect_file(close_file, unnamed);
    expect_file(absolute_fixture, absolute);
    picker.defer_saves = false;

    step = "cancel exit after an earlier discard";
    const auto a_bytes = file_bytes(a_file), c_bytes = file_bytes(c_file);
    const auto undo = [&](vgui_item_id id) {
        select(id);
        menus.activate(menus.open(1), 0);
        const auto undone = page();
        require(undone.model.dirty && undone.model.redo, "expected an unsaved undo with redo history");
        return undone;
    };
    const auto dirty_a = undo(a_tab), dirty_c = undo(c_tab);
    select(label_tab);
    prompt.expect(title(a_tab), Prompt::discard);
    prompt.expect(title(c_tab), Prompt::cancel);
    request_exit();
    const auto expect_exit_cancelled = [&] {
        expect_no_dialog();
        expect(label_tab, saved_label);
        require(tab_ids() == expected_tabs && file_bytes(a_file) == a_bytes && file_bytes(c_file) == c_bytes &&
            picker.save_requests == saves_before_close + 3, "cancelled exit closed or wrote a page");
        select(a_tab);
        expect(a_tab, dirty_a);
        select(c_tab);
        expect(c_tab, dirty_c);
        select(label_tab);
    };
    expect_exit_cancelled();

    step = "retry exit asks again about the earlier discarded page";
    prompt.expect(title(a_tab), Prompt::cancel);
    menus.activate(menus.open(0), 8); // File > Exit
    expect_exit_cancelled();
    for (const auto id : {a_tab, c_tab}) {
        select(id);
        menus.activate(menus.open(1), 1); // Restore the saved revision's content for the export test.
        require(!dirty(id), "redo did not return the page to its save point");
    }
    select(label_tab);
    expect(label_tab, saved_label);
}

} // namespace vgui::ui_design
