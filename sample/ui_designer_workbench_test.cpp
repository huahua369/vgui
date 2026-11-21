// U2 workbench smoke test: real VGUI input, without a window or GPU.
#include "ui_designer_locale.h"
#include "ui_designer_workbench.h"
#include "../gui/ui_design/model.h"

#include <glm/glm.hpp> // ovg_c.h uses GLM types in C++.
#include <ovg_c.h>
#include <vgui/vgui.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <map>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {
namespace ui = vgui::ui_design;
namespace fs = std::filesystem;

void check(bool condition, std::string_view what)
{
    if (!condition) throw std::runtime_error(std::string(what));
}

class temp_dir {
public:
    temp_dir()
    {
        const auto parent = fs::temp_directory_path();
        std::random_device random;
        for (int attempt = 0; attempt < 32; ++attempt) {
            auto candidate = parent / ("vgui-u2-" + std::to_string(random()) + "-" + std::to_string(random()));
            std::error_code error;
            if (fs::create_directory(candidate, error)) {
                path = candidate;
                return;
            }
            if (error && error != std::errc::file_exists)
                throw fs::filesystem_error("create workbench test directory", candidate, error);
        }
        throw std::runtime_error("could not reserve workbench test directory");
    }

    ~temp_dir()
    {
        std::error_code error;
        fs::remove_all(path, error);
    }

    fs::path path;
};

vgui_widget* find_widget(vgui_widget* widget, std::string_view id, vgui_widget_kind kind)
{
    const auto* actual = vgui_widget_get_id(widget);
    if (actual && id == actual && vgui_widget_get_kind(widget) == kind) return widget;
    for (std::size_t i = 0; i < vgui_widget_get_child_count(widget); ++i)
        if (auto* found = find_widget(vgui_widget_get_child(widget, i), id, kind)) return found;
    return nullptr;
}

vgui_item_id menu_item(vgui_widget* menu, std::string_view label)
{
    for (int i = 0; i < vgui_menu_get_count(menu); ++i) {
        const auto item = vgui_menu_get_item_at(menu, i);
        if (label == vgui_menu_get_item_text(menu, item)) return item;
    }
    throw std::runtime_error("missing menu item: " + std::string(label));
}

void check_menu_items(vgui_widget* menu, std::initializer_list<std::string_view> labels)
{
    check(menu && vgui_menu_get_count(menu) == static_cast<int>(labels.size()), "unexpected menu item count");
    int index = 0;
    for (const auto label : labels) {
        const auto item = vgui_menu_get_item_at(menu, index++);
        check(item && vgui_menu_get_item_type(menu, item) ==
            (label.empty() ? VGUI_MENU_ITEM_SEPARATOR : VGUI_MENU_ITEM_NORMAL), "unexpected menu item type");
        if (!label.empty())
            check(label == vgui_menu_get_item_text(menu, item), "expected menu item: " + std::string(label));
    }
}

class test_workbench {
public:
    explicit test_workbench(fs::path file, ui::designer_language language = ui::designer_language::english,
                            font_familys_t* font = nullptr)
        : file_(std::move(file)), ctx(vgui_create(font, 16), &vgui_destroy), strings(language),
          bench(ctx.get(), model, {
              .new_document = [this] { ++new_calls; },
              .open_document = [this] { open(); },
              .save_document = [this] { ++quick_save_calls; },
              .save_as = [this] { save(); },
              .export_package = [this] { ++export_calls; },
              .quit = [this] { ++quit_calls; },
              .layout_changed = [this] { if (on_layout_changed) on_layout_changed(); },
              .save_all = [this] { ++save_all_calls; },
              .can_save_all = [this] { return model.dirty(); },
              .open_options = [this] { ++options_calls; },
          }, strings)
    {
        vgui_context_set_size(ctx.get(), 1440, 1000);
        frame();
    }

    void frame()
    {
        bench.refresh();
        vgui_layout(ctx.get());
    }

    vgui_widget* widget(std::string_view id, vgui_widget_kind kind) const
    {
        auto* result = find_widget(vgui_root(ctx.get()), id, kind);
        check(result, "missing workbench widget: " + std::string(id));
        return result;
    }

    void click_at(float x, float y)
    {
        vgui_input_mouse_move(ctx.get(), x, y);
        check(vgui_input_mouse_down(ctx.get(), 1, x, y, 1), "workbench did not consume mouse down");
        vgui_input_mouse_up(ctx.get(), 1, x, y, 1);
    }

    void click(vgui_widget* target)
    {
        float x, y, w, h;
        vgui_widget_get_rect(target, &x, &y, &w, &h);
        check(w > 0 && h > 0, "workbench widget has no layout rect");
        click_at(x + w * 0.5f, y + h * 0.5f);
    }

    void press(std::string_view id)
    {
        auto* button = widget(id, VGUI_BUTTON);
        check(vgui_widget_get_enabled(button), "workbench button is disabled: " + std::string(id));
        click(button);
        frame();
    }

    bool key(int code)
    {
        const bool consumed = vgui_input_key(ctx.get(), code, 1, 0);
        vgui_input_key(ctx.get(), code, 0, 0);
        frame();
        return consumed;
    }

    vgui_widget* menubar() const { return widget("designer.menubar", VGUI_MENUBAR); }

    vgui_widget* open_menu(std::string_view label)
    {
        auto* bar = menubar();
        check(!vgui_context_get_open_menu(ctx.get()), "another popup is already open");
        for (int i = 0; i < vgui_menubar_get_count(bar); ++i) {
            const auto item = vgui_menubar_get_item_at(bar, i);
            if (label != vgui_menubar_get_item_text(bar, item)) continue;
            auto* menu = vgui_menubar_get_menu(bar, item);
            float x, y, w, h;
            check(vgui_menubar_get_item_rect(bar, item, &x, &y, &w, &h) && w > 0 && h > 0,
                "missing layout rect for menu: " + std::string(label));
            click_at(x + w * 0.5f, y + h * 0.5f);
            frame();
            check(menu && vgui_context_get_open_menu(ctx.get()) == menu && vgui_menubar_get_active(bar) == item,
                "pointer did not open menu: " + std::string(label));
            return menu;
        }
        throw std::runtime_error("missing menu title: " + std::string(label));
    }

    void highlight(vgui_widget* menu, std::string_view label)
    {
        check(vgui_menu_is_open(menu), "cannot navigate a closed menu");
        const auto item = menu_item(menu, label);
        check(vgui_menu_get_item_enabled(menu, item), "menu item is disabled: " + std::string(label));
        for (int steps = 0; vgui_menu_get_cursor(menu) != item && steps < vgui_menu_get_count(menu); ++steps)
            check(key(VGUI_KEY_DOWN), "menu did not consume Down");
        check(vgui_menu_get_cursor(menu) == item, "keyboard could not reach menu item: " + std::string(label));
    }

    void activate(vgui_widget* menu, std::string_view label)
    {
        highlight(menu, label);
        check(key(VGUI_KEY_ENTER), "menu did not consume Enter");
        check(!vgui_context_get_open_menu(ctx.get()), "activating the menu item did not close the popup chain");
    }

    void command(std::string_view title, std::string_view label) { activate(open_menu(title), label); }
    void file(std::string_view label) { command("File", label); }
    void edit(std::string_view label) { command("Edit", label); }

    vgui_widget* open_new_control()
    {
        const bool chinese = strings.language() == ui::designer_language::simplified_chinese;
        auto* menu = open_menu(chinese ? "资源" : "Resources");
        const auto label = chinese ? "新建控件" : "New Control";
        check(vgui_menu_get_count(menu) == 1, "Resources must contain just the New Control submenu");
        const auto item = menu_item(menu, label);
        check(vgui_menu_get_item_type(menu, item) == VGUI_MENU_ITEM_SUBMENU, "New Control is not a submenu");
        auto* submenu = vgui_menu_get_submenu(menu, item);
        highlight(menu, label);
        check(submenu && !vgui_menu_is_open(submenu), "New Control opened before keyboard navigation");
        check(key(VGUI_KEY_RIGHT) && vgui_menu_is_open(submenu) && vgui_context_get_open_menu(ctx.get()) == menu,
            "Right did not open the nested New Control menu");
        return submenu;
    }

    void set_text(std::string_view property, const char* text)
    {
        auto* editor = widget("property:" + std::string(property), VGUI_EDIT);
        click(editor);
        check(vgui_get_focus(ctx.get()) == editor, "inspector did not focus: " + std::string(property));
        vgui_edit_select_all(editor);
        check(vgui_input_text(ctx.get(), text), "inspector did not consume text");
        key(VGUI_KEY_ENTER);
    }

    void set_layout(std::string_view layout)
    {
        click(widget("property:layout", VGUI_BUTTON));
        auto* menu = vgui_context_get_open_menu(ctx.get());
        check(menu, "Layout inspector did not open its menu");
        activate(menu, layout);
    }

    std::string add(std::string_view type, const std::string& parent)
    {
        const auto* descriptor = ui::find_type(type);
        check(descriptor, "unknown test control type: " + std::string(type));
        const auto before = model.current().nodes.size();
        auto expected = model.current().nodes.at(parent).children;
        activate(open_new_control(), strings.text("workbench.type." + std::string(type), descriptor->label.data()));
        const auto& doc = model.current();
        const auto& children = doc.nodes.at(parent).children;
        check(doc.nodes.size() == before + 1 && children.size() == expected.size() + 1,
            "New Control failed to add " + std::string(type) + " to the expected panel");
        const auto id = children.back();
        check(doc.nodes.at(id).type == type, "New Control added the wrong node type");
        expected.push_back(id);
        check(children == expected, "New Control changed the order of existing children");
        return id;
    }

    vgui_widget* outline() const { return widget("designer.outliner", VGUI_TREE); }

    vgui_item_id panel_item() const
    {
        auto* tree = outline();
        check(vgui_tree_get_root_count(tree) == 1, "outline must show one document root");
        const auto root_item = vgui_tree_get_root(tree, 0);
        check(vgui_tree_get_child_count(tree, root_item) == 1, "outline did not show the nested panel");
        return vgui_tree_get_child(tree, root_item, 0);
    }

    void select_row(vgui_item_id item, std::string_view id)
    {
        check(item != 0, "missing outline row for " + std::string(id));
        auto* tree = outline();
        const auto revision = model.revision();
        vgui_tree_select(tree, item, 1);
        frame();
        check(bench.selection() == id, "outline did not select " + std::string(id));
        check(model.revision() == revision, "selecting an outline row changed the document");
    }

    void select_child(int index, std::string_view id)
    {
        select_row(vgui_tree_get_child(outline(), panel_item(), index), id);
    }

private:
    void save()
    {
        ++save_calls;
        // Capture I/O success without throwing from an input callback.
        io_ok = model.save(file_, &io_error);
    }

    void open()
    {
        ++open_calls;
        io_ok = model.load(file_, &io_error);
        if (io_ok) bench.document_replaced();
    }

    fs::path file_;

public:
    std::unique_ptr<vgui_context, decltype(&vgui_destroy)> ctx;
    ui::session model;
    ui::diagnostic io_error;
    int save_calls = 0, open_calls = 0;
    int new_calls = 0, quick_save_calls = 0, save_all_calls = 0, export_calls = 0, quit_calls = 0, options_calls = 0;
    bool io_ok = false;
    std::function<void()> on_layout_changed;
    ui::designer_strings strings;
    ui::designer_workbench bench;
};

void check_menu_titles(vgui_widget* bar, std::initializer_list<std::string_view> titles)
{
    check(vgui_menubar_get_count(bar) == static_cast<int>(titles.size()), "unexpected MenuBar title count");
    int index = 0;
    for (const auto title : titles) {
        const auto item = vgui_menubar_get_item_at(bar, index++);
        check(item && title == vgui_menubar_get_item_text(bar, item) && vgui_menubar_get_item_enabled(bar, item) &&
            vgui_menubar_get_menu(bar, item), "expected enabled MenuBar title: " + std::string(title));
    }
}

void check_no_palette(vgui_widget* widget)
{
    const auto* id = vgui_widget_get_id(widget);
    check(!id || !std::string_view(id).starts_with("designer.add."), "legacy palette control still exists");
    if (vgui_widget_get_kind(widget) == VGUI_LABEL) {
        const std::string_view text = vgui_widget_get_text(widget);
        check(text != "Palette", "legacy palette heading still exists");
    }
    for (std::size_t i = 0; i < vgui_widget_get_child_count(widget); ++i)
        check_no_palette(vgui_widget_get_child(widget, i));
}

void exercise_menus()
{
    temp_dir directory;
    test_workbench workbench(directory.path / "menus.vgui.json");
    auto* ctx = workbench.ctx.get();
    auto& model = workbench.model;
    auto* bar = workbench.menubar();
    check_menu_titles(bar, {"File", "Edit", "Resources", "View", "Tools", "Help"});
    check_no_palette(vgui_root(ctx));

    auto* file = workbench.open_menu("File");
    check_menu_items(file, {"New Page", "Open Page", "Save", "Save As", "Save All", {}, "Export Package", {}, "Exit"});
    check(!vgui_menu_get_item_enabled(file, menu_item(file, "Save All")), "Save All should be disabled without unsaved pages");
    check(workbench.key(VGUI_KEY_RIGHT), "MenuBar did not consume Right");
    auto* edit = vgui_context_get_open_menu(ctx);
    const auto edit_title = vgui_menubar_get_item_at(bar, 1);
    check(edit == vgui_menubar_get_menu(bar, edit_title) && vgui_menubar_get_active(bar) == edit_title &&
        !vgui_menu_is_open(file), "Right did not switch from File to Edit");
    check_menu_items(edit, {"Undo", "Redo", {}, "Copy", "Delete", "Move Up", "Move Down"});
    for (const auto label : {"Undo", "Redo", "Copy", "Delete", "Move Up", "Move Down"})
        check(!vgui_menu_get_item_enabled(edit, menu_item(edit, label)),
            std::string("command should be disabled on a fresh root: ") + label);
    const auto initial_revision = model.revision();
    check(workbench.key(VGUI_KEY_DOWN) && vgui_menu_get_cursor(edit) == 0,
        "keyboard selected a disabled command or separator");
    check(workbench.key(VGUI_KEY_ENTER) && vgui_context_get_open_menu(ctx) == edit &&
        model.revision() == initial_revision && !model.dirty(), "Enter on the disabled Edit menu was not inert");
    check(workbench.key(VGUI_KEY_ESCAPE) && !vgui_context_get_open_menu(ctx), "Escape did not dismiss Edit");

    workbench.file("New Page");
    workbench.file("Save");
    workbench.file("Export Package");
    workbench.file("Exit");
    check(workbench.new_calls == 1 && workbench.quick_save_calls == 1 && workbench.save_calls == 0 &&
        workbench.export_calls == 1 && workbench.quit_calls == 1, "File menu did not dispatch the expected commands");
    check(vgui_input_key(ctx, 'S', 1, VGUI_MOD_CTRL) && workbench.quick_save_calls == 2,
        "Ctrl+S did not use the same Save command as the File menu");
    vgui_input_key(ctx, 'S', 0, VGUI_MOD_CTRL);
    for (const auto topic : {"Instructions", "Shortcuts", "About"}) {
        workbench.command("Help", topic);
        check(vgui_modal_active(ctx), "Help did not show a dialog: " + std::string(topic));
        check(workbench.key(VGUI_KEY_ESCAPE) && !vgui_modal_active(ctx), "could not dismiss Help dialog");
    }

    workbench.bench.set_page_active(false);
    auto* tools = workbench.open_menu("Tools");
    const auto language_item = vgui_menu_get_item_at(tools, 0);
    const auto options_item = vgui_menu_get_item_at(tools, 1);
    const auto theme_item = vgui_menu_get_item_at(tools, 2);
    check(vgui_menu_get_count(tools) == 3 && language_item && options_item && theme_item &&
        vgui_menu_get_item_type(tools, language_item) == VGUI_MENU_ITEM_SUBMENU &&
        std::string_view(vgui_menu_get_item_text(tools, language_item)) == "Language" &&
        vgui_menu_get_item_type(tools, options_item) == VGUI_MENU_ITEM_NORMAL &&
        std::string_view(vgui_menu_get_item_text(tools, options_item)) == "Options" &&
        vgui_menu_get_item_type(tools, theme_item) == VGUI_MENU_ITEM_SUBMENU &&
        std::string_view(vgui_menu_get_item_text(tools, theme_item)) == "Theme",
        "Tools should offer Language, Options and Theme without a separate configuration entry");
    workbench.activate(tools, "Options");
    check(workbench.options_calls == 1, "Tools did not request Options without an active page");
    workbench.bench.set_page_active(true);
    workbench.frame();

    auto* submenu = workbench.open_new_control();
    auto* resources = vgui_context_get_open_menu(ctx);
    check_menu_items(submenu, {"Panel", "Label", "Button", "Edit", "Image", "Gradient", "Checkbox", "Radio Button", "Switch",
        "Slider", "Progress Bar", "Separator", "List", "Tree", "Menu", "Menu Bar", "Splitter", "Tab View", "Dock"});
    check(vgui_menu_get_count(submenu) == static_cast<int>(ui::types().size()), "New Control does not match the schema");
    workbench.highlight(submenu, "Image");
    check(workbench.key(VGUI_KEY_ESCAPE) && !vgui_menu_is_open(submenu) &&
        vgui_context_get_open_menu(ctx) == resources, "first Escape should close only New Control");
    check(workbench.key(VGUI_KEY_ESCAPE) && !vgui_context_get_open_menu(ctx) && !vgui_menubar_get_active(bar),
        "second Escape should close Resources and deactivate its title");
    check(model.revision() == initial_revision && !model.can_undo() && !model.dirty(),
        "cancelling New Control changed the document or its history");

    struct control_size { std::string_view type; double width, height; };
    constexpr control_size sizes[] = {{"panel", 240, 160}, {"label", 120, 24}, {"button", 120, 36},
        {"edit", 200, 36}, {"image", 128, 128}};
    const auto add_sized = [&](const control_size& size, const std::string& parent) {
        const auto revision = model.revision();
        const auto id = workbench.add(size.type, parent);
        const auto& props = model.current().nodes.at(id).props;
        const auto width = props.find("width"), height = props.find("height");
        check(width != props.end() && width->second == ui::value{size.width} &&
            height != props.end() && height->second == ui::value{size.height},
            "New Control did not set the initial width and height: " + std::string(size.type));
        check(std::string_view(vgui_edit_get_text(workbench.widget("property:width", VGUI_EDIT))) ==
                std::to_string(static_cast<int>(size.width)) &&
            std::string_view(vgui_edit_get_text(workbench.widget("property:height", VGUI_EDIT))) ==
                std::to_string(static_cast<int>(size.height)), "inspector did not show the new control's size");
        check(model.revision() == revision + 1, "setting the initial size must use the creation transaction");
        return id;
    };
    const auto root = model.current().root;
    const auto panel = add_sized(sizes[0], root);
    std::vector<std::string> leaves;
    for (std::size_t i = 1; i < std::size(sizes); ++i) {
        // The first leaf is added to the selected Panel. The others must be
        // added beside the selected leaf, without changing earlier siblings.
        leaves.push_back(add_sized(sizes[i], panel));
    }
    check(model.current().nodes.at(panel).children == leaves && leaves.size() == 4,
        "the New Control choices did not create all five schema types");

    const auto expect_edit = [&](std::initializer_list<std::string_view> enabled) {
        auto* menu = workbench.open_menu("Edit");
        for (const std::string_view label : {"Undo", "Redo", "Copy", "Delete", "Move Up", "Move Down"}) {
            bool expected = false;
            for (const auto candidate : enabled) expected |= (candidate == label);
            check(bool(vgui_menu_get_item_enabled(menu, menu_item(menu, label))) == expected,
                "unexpected enabled state for Edit > " + std::string(label));
        }
        check(workbench.key(VGUI_KEY_ESCAPE) && !vgui_context_get_open_menu(ctx), "Escape did not dismiss Edit");
    };
    const auto root_row = vgui_tree_get_root(workbench.outline(), 0);
    workbench.select_row(root_row, root);
    expect_edit({"Undo"});
    workbench.select_child(3, leaves[3]);
    expect_edit({"Undo", "Copy", "Delete", "Move Up"});
    workbench.select_child(0, leaves[0]);
    expect_edit({"Undo", "Copy", "Delete", "Move Down"});
    workbench.edit("Move Down");
    check(model.current().nodes.at(panel).children == std::vector<std::string>{leaves[1], leaves[0], leaves[2], leaves[3]},
        "Edit > Move Down did not reorder the selected control");
    const auto before_copy = model.current();
    workbench.edit("Copy");
    const std::string copy(workbench.bench.selection());
    check(copy != leaves[0] && model.current().nodes.size() == before_copy.nodes.size() + 1 &&
        model.current().nodes.at(copy) == before_copy.nodes.at(leaves[0]) &&
        model.current().nodes.at(panel).children == std::vector<std::string>{leaves[1], leaves[0], copy, leaves[2], leaves[3]},
        "Edit > Copy did not insert and select a duplicate beside the original");
    workbench.edit("Delete");
    check(model.current() == before_copy && workbench.bench.selection() == panel,
        "Edit > Delete did not remove the copy and select its parent");
    workbench.edit("Undo");
    expect_edit({"Undo", "Redo", "Copy", "Delete"});
    const auto before_save_all = model.current();
    workbench.file("Save All");
    check(workbench.save_all_calls == 1 && model.current() == before_save_all && model.can_redo(),
        "File > Save All did not dispatch without editing the document");
}

void exercise_side_resize()
{
    // Use real label metrics, as the native editor does, to exercise genuine overflow.
    std::unique_ptr<font_cache_cx, decltype(&free_font_cache)> cache(new_font_cache(), free_font_cache);
    check(cache && load_font_from_path(cache.get(), VGUI_SOURCE_DIR "/sample/res/fonts/DejaVuSans.ttf", "WorkbenchLatin"),
        "could not load the workbench layout test font");
    std::unique_ptr<font_familys_t, decltype(&delete_font_family)> font(
        new_font_family(cache.get(), "WorkbenchLatin", nullptr), delete_font_family);
    check(font && font->count == 1, "could not create the workbench layout test font family");
    temp_dir directory;
    test_workbench workbench(directory.path / "layout.vgui.json", ui::designer_language::english, font.get());
    auto* ctx = workbench.ctx.get();
    auto* dock = workbench.widget("designer.dock", VGUI_DOCK);
    const auto initial = workbench.model.current();
    const auto revision = workbench.model.revision();
    struct rect { float x, y, w, h; };
    const auto zone = [&](vgui_dock_zone which) {
        rect r{};
        check(vgui_dock_get_zone_rect(dock, which, &r.x, &r.y, &r.w, &r.h) != 0,
            "resize hid a dock region");
        return r;
    };
    const auto drag = [&](vgui_dock_zone left, vgui_dock_zone right, float delta) {
        const auto a = zone(left), b = zone(right);
        const float x = (a.x + a.w + b.x) * .5f, y = a.y + a.h * .5f;
        check(a.x + a.w < b.x && vgui_hit_test(ctx, x, y) == dock, "dock divider does not have a hit region");
        vgui_input_mouse_move(ctx, x, y);
        check(vgui_context_get_cursor(ctx) == VGUI_CURSOR_SIZE_H, "hovering the divider did not change the pointer");
        check(vgui_input_mouse_down(ctx, 1, x, y, 1) != 0, "dock divider did not accept a press");
        vgui_input_mouse_move(ctx, x + delta, y);
        workbench.frame(); // The layout must update during the drag, not only on release.
        const float resized = zone(left).w;
        vgui_input_mouse_up(ctx, 1, x + delta, y, 1);
        vgui_input_mouse_move(ctx, x, y);
        workbench.frame();
        check(zone(left).w == resized, "the dock divider did not retain its width after release");
        return resized;
    };

    const auto left = zone(VGUI_DOCK_LEFT), center = zone(VGUI_DOCK_CENTER), right = zone(VGUI_DOCK_RIGHT);
    const auto paint = vgui_context_paint_revision(ctx);
    vgui_input_mouse_move(ctx, (left.x + left.w + center.x) * .5f, left.y + left.h * .5f);
    check(vgui_context_paint_revision(ctx) != paint, "divider hover did not invalidate the on-demand render");
    check(drag(VGUI_DOCK_LEFT, VGUI_DOCK_CENTER, 72) == left.w + 72 &&
        zone(VGUI_DOCK_CENTER).w == center.w - 72 && zone(VGUI_DOCK_RIGHT).w == right.w,
        "left divider did not resize only the outline and center regions");
    check(drag(VGUI_DOCK_CENTER, VGUI_DOCK_RIGHT, -63) == center.w - 72 - 63 &&
        zone(VGUI_DOCK_LEFT).w == left.w + 72 && zone(VGUI_DOCK_RIGHT).w == right.w + 63,
        "right divider did not resize only the inspector and center regions");

    const auto before_cancel = zone(VGUI_DOCK_LEFT), beside_cancel = zone(VGUI_DOCK_CENTER);
    const float cancel_x = (before_cancel.x + before_cancel.w + beside_cancel.x) * .5f;
    const float cancel_y = before_cancel.y + before_cancel.h * .5f;
    vgui_input_mouse_down(ctx, 1, cancel_x, cancel_y, 1);
    vgui_input_cancel(ctx); // The SDL backend uses this path on window focus loss.
    vgui_input_mouse_move(ctx, cancel_x - 70, cancel_y);
    vgui_input_mouse_up(ctx, 1, cancel_x - 70, cancel_y, 1);
    workbench.frame();
    check(zone(VGUI_DOCK_LEFT).w == before_cancel.w, "a canceled divider drag kept following the pointer");

    drag(VGUI_DOCK_LEFT, VGUI_DOCK_CENTER, 2000);
    check(zone(VGUI_DOCK_CENTER).w >= 260 && zone(VGUI_DOCK_RIGHT).w == right.w + 63,
        "dragging past the right edge collapsed the center or resized the other pane");
    auto* remove = workbench.widget("designer.delete", VGUI_BUTTON);
    auto* layout = workbench.widget("property:layout", VGUI_BUTTON);
    const auto bounds = [](vgui_widget* widget) {
        rect r{};
        vgui_widget_get_rect(widget, &r.x, &r.y, &r.w, &r.h);
        return r;
    };
    float remove_min = 0;
    vgui_widget_get_intrinsic_size(remove, &remove_min, nullptr);

    drag(VGUI_DOCK_LEFT, VGUI_DOCK_CENTER, -2000);
    drag(VGUI_DOCK_CENTER, VGUI_DOCK_RIGHT, zone(VGUI_DOCK_RIGHT).w - 270);
    const auto compact_layout = bounds(layout);
    drag(VGUI_DOCK_CENTER, VGUI_DOCK_RIGHT, 2000);
    check(zone(VGUI_DOCK_LEFT).w == 150 && zone(VGUI_DOCK_RIGHT).w == 220,
        "side pane minimum widths were not applied");
    check(bounds(remove).w >= remove_min - 0.01f, "narrow outline squeezed the Delete button below its natural width");
    check(bounds(layout).w == compact_layout.w, "narrow inspector squeezed the Layout button below its scrollable width");

    auto* actions = workbench.widget("designer.outline.actions", VGUI_PANEL);
    const auto viewport = bounds(actions), hidden_button = bounds(remove);
    const auto tree = bounds(workbench.outline());
    check(hidden_button.x + hidden_button.w > viewport.x + viewport.w,
        "outline button should extend past the viewport before scrolling (button right=" +
        std::to_string(hidden_button.x + hidden_button.w) + ", viewport right=" +
        std::to_string(viewport.x + viewport.w) + ")");
    check(tree.x + tree.w <= zone(VGUI_DOCK_LEFT).x + zone(VGUI_DOCK_LEFT).w,
        "the overflowing actions widened the tree");
    // Drag the horizontal thumb to its right edge, then verify that the
    // rightmost button came into view without changing size.
    const float bar_x = viewport.x + viewport.w * .25f, bar_y = viewport.y + viewport.h - 6;
    vgui_input_mouse_move(ctx, bar_x, bar_y);
    const auto before_scroll = vgui_context_paint_revision(ctx);
    check(vgui_input_mouse_down(ctx, 1, bar_x, bar_y, 1), "outline horizontal scrollbar did not accept a press");
    vgui_input_mouse_move(ctx, bar_x + viewport.w, bar_y);
    vgui_input_mouse_up(ctx, 1, bar_x + viewport.w, bar_y, 1);
    workbench.frame();
    float scroll = 0;
    vgui_panel_get_scroll(actions, &scroll, nullptr);
    const auto visible_button = bounds(remove);
    check(scroll > 0, "dragging the outline scrollbar did not change its offset");
    check(visible_button.x + visible_button.w <= viewport.x + viewport.w + 0.01f &&
        std::abs(visible_button.w - hidden_button.w) < 0.01f,
        "scrollbar did not reveal the rightmost outline button at the same width (offset=" +
        std::to_string(scroll) + ", right=" + std::to_string(visible_button.x + visible_button.w) + ", viewport right=" +
        std::to_string(viewport.x + viewport.w) + ")");
    check(vgui_context_paint_revision(ctx) != before_scroll, "scrolling did not invalidate the on-demand render");
    check(bounds(workbench.outline()).x == tree.x, "scrolling the actions also shifted the tree");

    auto* inspector = vgui_dock_get_panel(dock, vgui_dock_find(dock, "inspector"));
    auto* grid = vgui_widget_get_child(inspector, 0);
    const auto layout_before_scroll = bounds(layout);
    vgui_input_mouse_move(ctx, layout_before_scroll.x - 8, layout_before_scroll.y + layout_before_scroll.h / 2);
    check(vgui_input_mouse_wheel(ctx, -5, 0), "inspector did not accept horizontal scrolling");
    workbench.frame();
    check(bounds(layout).x < layout_before_scroll.x && bounds(layout).w == layout_before_scroll.w,
        "inspector scrolling did not reveal the property editor without squeezing it");

    drag(VGUI_DOCK_LEFT, VGUI_DOCK_CENTER, left.w + 72 - zone(VGUI_DOCK_LEFT).w);
    drag(VGUI_DOCK_CENTER, VGUI_DOCK_RIGHT, zone(VGUI_DOCK_RIGHT).w - (right.w + 63));
    float grid_scroll = 0;
    vgui_panel_get_scroll(actions, &scroll, nullptr);
    vgui_panel_get_scroll(grid, &grid_scroll, nullptr);
    check(scroll == 0 && grid_scroll == 0, "widening the side panes did not clear horizontal overflow");

    vgui_context_set_size(ctx, 950, 650);
    vgui_context_set_ui_scale(ctx, 1.25f);
    workbench.frame();
    drag(VGUI_DOCK_CENTER, VGUI_DOCK_RIGHT, -2000);
    check(zone(VGUI_DOCK_LEFT).w >= 150 * 1.25f && zone(VGUI_DOCK_CENTER).w >= 260 * 1.25f &&
        zone(VGUI_DOCK_RIGHT).w >= 220 * 1.25f, "minimum widths did not scale in a narrow window");

    check(workbench.model.current() == initial && workbench.model.revision() == revision &&
        !workbench.model.dirty() && !workbench.model.can_undo(), "sash resize changed the document or its history");
}

float dock_zone_width(vgui_widget* dock, vgui_dock_zone zone)
{
    float width = 0;
    check(vgui_dock_get_zone_rect(dock, zone, nullptr, nullptr, &width, nullptr) && width > 0,
        "expected a nonempty dock zone rect");
    return width;
}

void exercise_side_collapse()
{
    temp_dir directory;
    std::vector<ui::designer_dock_widths> changes;
    test_workbench workbench(directory.path / "collapse.vgui.json");
    auto* ctx = workbench.ctx.get();
    auto* dock = workbench.widget("designer.dock", VGUI_DOCK);
    const auto initial = workbench.model.current();
    const auto revision = workbench.model.revision();
    workbench.on_layout_changed = [&] { changes.push_back(workbench.bench.dock_widths()); };

    constexpr ui::designer_dock_widths defaults{}, expanded{304.5f, 390.f},
        left_only{expanded.left, expanded.right, true, false},
        right_only{expanded.left, expanded.right, false, true}, both{expanded.left, expanded.right, true, true};
    constexpr std::string_view left_id = "designer.dock.collapse_left", right_id = "designer.dock.collapse_right";
    const auto button = [&](std::string_view id) { return workbench.widget(id, VGUI_BUTTON); };
    const auto arrow = [&](std::string_view id) { return std::string(vgui_widget_get_text(button(id))); };
    const auto expanded_left = arrow(left_id), expanded_right = arrow(right_id);
    const auto check_state = [&](ui::designer_dock_widths expected) {
        check(workbench.bench.dock_widths() == expected &&
            bool(vgui_dock_get_side_collapsed(dock, VGUI_DOCK_LEFT)) == expected.left_collapsed &&
            bool(vgui_dock_get_side_collapsed(dock, VGUI_DOCK_RIGHT)) == expected.right_collapsed,
            "dock collapse state or preferred widths do not match");
        const auto left = arrow(left_id), right = arrow(right_id);
        check(!left.empty() && !right.empty() && (left != expanded_left) == expected.left_collapsed &&
            (right != expanded_right) == expected.right_collapsed, "dock buttons do not indicate their side's state");
    };
    const auto check_geometry = [&](ui::designer_dock_widths expected, float scale = 1.f) {
        const float left = dock_zone_width(dock, VGUI_DOCK_LEFT), right = dock_zone_width(dock, VGUI_DOCK_RIGHT);
        const float left_full = expected.left * scale, right_full = expected.right * scale;
        check(expected.left_collapsed ? left < left_full * .5f : left == left_full, "incorrect Outline zone width");
        check(expected.right_collapsed ? right < right_full * .5f : right == right_full, "incorrect Inspector zone width");
        return dock_zone_width(dock, VGUI_DOCK_CENTER);
    };
    const auto change = [&](ui::designer_dock_widths expected, const auto& input) {
        const auto before = changes.size();
        input();
        check(changes.size() == before + 1 && changes.back() == expected,
            "layout input did not immediately notify the host once with the updated preferences");
        check_state(expected);
        workbench.frame();
    };
    const auto click_side = [&](std::string_view id, ui::designer_dock_widths expected) {
        change(expected, [&] {
            auto* target = button(id);
            float x = 0, y = 0, w = 0, h = 0;
            vgui_widget_get_rect(target, &x, &y, &w, &h);
            x += w * .5f;
            y += h * .5f;
            check(w > 0 && h > 0 && vgui_widget_get_enabled(target) && vgui_hit_test(ctx, x, y) == target,
                "dock button is not clickable: " + std::string(id));
            workbench.click_at(x, y);
        });
    };
    const auto check_view = [&](vgui_widget* menu, ui::designer_dock_widths expected) {
        const auto outline = menu_item(menu, "Outline"), inspector = menu_item(menu, "Inspector");
        check(vgui_menu_get_item_checkable(menu, outline) && vgui_menu_get_item_checkable(menu, inspector) &&
            bool(vgui_menu_get_item_checked(menu, outline)) == !expected.left_collapsed &&
            bool(vgui_menu_get_item_checked(menu, inspector)) == !expected.right_collapsed,
            "View menu checks do not track expanded sides");
    };
    const auto activate = [&](vgui_widget* menu, std::string_view label, ui::designer_dock_widths expected) {
        workbench.highlight(menu, label);
        change(expected, [&] {
            check(vgui_input_key(ctx, VGUI_KEY_ENTER, 1, 0), "View command did not consume Enter");
            vgui_input_key(ctx, VGUI_KEY_ENTER, 0, 0);
            check(!vgui_context_get_open_menu(ctx), "View command did not dismiss the menu");
        });
    };

    check_state(defaults);
    workbench.bench.set_dock_widths(expanded);
    workbench.frame();
    const float center_expanded = check_geometry(expanded);
    click_side(left_id, left_only);
    const float center_left = check_geometry(left_only);
    check(center_left > center_expanded, "collapsing Outline did not widen the center");
    check_view(workbench.open_menu("View"), left_only);
    check(workbench.key(VGUI_KEY_ESCAPE), "could not dismiss View");
    click_side(right_id, both);
    const float center_both = check_geometry(both);
    check(center_both > center_left, "collapsing Inspector did not widen the center");
    click_side(left_id, right_only);
    const float center_right = check_geometry(right_only);
    check(center_right > center_expanded && center_right < center_both, "center did not track independent side collapses");
    click_side(right_id, expanded);
    check(check_geometry(expanded) == center_expanded, "expanding both sides did not restore the original center width");

    auto* view = workbench.open_menu("View");
    check_view(view, expanded);
    check(vgui_menu_get_item_at(view, vgui_menu_get_count(view) - 1) == menu_item(view, "Restore Default Layout"),
        "Restore Default Layout is not the last View command");
    activate(view, "Outline", left_only);
    view = workbench.open_menu("View");
    check_view(view, left_only);
    activate(view, "Inspector", both);

    workbench.strings = ui::designer_strings(ui::designer_language::simplified_chinese);
    workbench.bench.refresh_language();
    workbench.frame();
    check(std::string_view(vgui_widget_get_tooltip(button(left_id))) == "展开大纲" &&
        std::string_view(vgui_widget_get_tooltip(button(right_id))) == "展开属性检查器",
        "collapsed dock buttons did not get localized expand hints");
    workbench.strings = ui::designer_strings(ui::designer_language::english);
    workbench.bench.refresh_language();

    workbench.bench.set_page_active(false);
    workbench.frame();
    check_state(both);
    workbench.bench.set_page_active(true);
    workbench.frame();
    check_state(both);
    check(check_geometry(both) == center_both, "reopening the workspace lost the collapsed side rails");

    workbench.bench.set_page_active(false);
    vgui_context_set_ui_scale(ctx, 1.25f);
    workbench.frame();
    activate(workbench.open_menu("View"), "Restore Default Layout", defaults);
    workbench.bench.set_page_active(true);
    workbench.frame();
    check_state(defaults);
    check_geometry(defaults, 1.25f);
    check_view(workbench.open_menu("View"), defaults);
    check(workbench.key(VGUI_KEY_ESCAPE), "could not dismiss View after layout reset");
    check(workbench.model.current() == initial && workbench.model.revision() == revision &&
        !workbench.model.dirty() && !workbench.model.can_undo(), "side layout commands changed the design or its history");
}

void exercise_toolbar()
{
    std::unique_ptr<font_cache_cx, decltype(&free_font_cache)> cache(new_font_cache(), free_font_cache);
    check(cache && load_font_from_path(cache.get(), VGUI_SOURCE_DIR "/sample/res/fonts/DejaVuSans.ttf", "ToolbarLatin") &&
        load_font_from_path(cache.get(), VGUI_SOURCE_DIR "/sample/res/fonts/DesignerSansSC.ttf", "ToolbarChinese"),
        "could not load fonts for the toolbar layout test");
    std::unique_ptr<font_familys_t, decltype(&delete_font_family)> font(
        new_font_family(cache.get(), "ToolbarLatin,ToolbarChinese", nullptr), delete_font_family);
    check(font && font->count == 2, "could not create toolbar test font family");
    temp_dir directory;
    test_workbench workbench(directory.path / "form.vgui.json", ui::designer_language::simplified_chinese, font.get());
    auto* bar = workbench.widget("designer.toolbar", VGUI_PANEL);
    auto* dock = workbench.widget("designer.dock", VGUI_DOCK);
    auto* ctx = workbench.ctx.get();
    const auto check_layout = [&](std::string_view file_menu, std::string_view save_label, std::string_view export_label) {
        struct rect { float x, y, w, h; };
        const auto bounds = [](vgui_widget* widget) {
            rect r{};
            vgui_widget_get_rect(widget, &r.x, &r.y, &r.w, &r.h);
            return r;
        };
        const auto inside = [](rect inner, rect outer) {
            constexpr float rounding = 0.5f;
            return inner.w > 0 && inner.h > 0 && inner.x >= outer.x - rounding && inner.y >= outer.y - rounding &&
                inner.x + inner.w <= outer.x + outer.w + rounding && inner.y + inner.h <= outer.y + outer.h + rounding;
        };
        const auto separate = [](rect a, rect b) {
            return a.x + a.w <= b.x + 0.5f || b.x + b.w <= a.x + 0.5f;
        };
        const auto toolbar = bounds(bar);
        auto* menubar = workbench.menubar();
        const auto menus = bounds(menubar);
        check(inside(toolbar, {0, 0, 950, 650}) && toolbar.x == 0 && toolbar.y == 0 && toolbar.w == 950,
            "toolbar must fill the top edge of the window content area");
        check(menus.x == toolbar.x, "MenuBar has a left inset inside the toolbar");
        check(inside(menus, toolbar), "MenuBar is clipped");
        check(vgui_menubar_get_count(menubar) == 6, "narrow MenuBar must retain all six titles");
        float previous_right = menus.x;
        for (int i = 0; i < vgui_menubar_get_count(menubar); ++i) {
            const auto item = vgui_menubar_get_item_at(menubar, i);
            rect r{};
            check(vgui_menubar_get_item_rect(menubar, item, &r.x, &r.y, &r.w, &r.h) && inside(r, menus) &&
                r.x >= previous_right - 0.5f, "MenuBar title is clipped or overlaps its neighbor");
            previous_right = r.x + r.w;
        }
        check(bounds(dock).y >= toolbar.y + toolbar.h - 0.5f, "toolbar overlaps the editor panels");

        const auto shown = [](vgui_widget* widget) {
            for (auto* ancestor = widget; ancestor; ancestor = vgui_widget_get_parent(ancestor))
                if (!vgui_widget_get_visible(ancestor)) return false;
            return true;
        };
        const auto before_save = workbench.quick_save_calls;
        const auto before_export = workbench.export_calls;
        const auto check_action = [&](const char* id, std::string_view label) {
            auto* button = workbench.widget(id, VGUI_BUTTON);
            if (!shown(button)) {
                workbench.command(file_menu, label);
                return;
            }
            const auto r = bounds(button);
            check(inside(r, toolbar) && separate(r, menus),
                std::string("toolbar action clipped or overlapping: ") + id);
            workbench.press(id);
        };
        auto* save = workbench.widget("designer.file.save", VGUI_BUTTON);
        auto* export_button = workbench.widget("designer.file.export", VGUI_BUTTON);
        if (shown(save) && shown(export_button))
            check(separate(bounds(save), bounds(export_button)), "quick actions overlap");
        check_action("designer.file.save", save_label);
        check_action("designer.file.export", export_label);
        check(workbench.quick_save_calls == before_save + 1 && workbench.export_calls == before_export + 1,
            "Save and Export were not reachable from the narrow toolbar or File menu");
    };
    vgui_context_set_size(ctx, 950, 650);
    workbench.frame();
    check_layout("文件", "保存", "导出交付包");
    const auto outline = vgui_dock_get_active(dock, VGUI_DOCK_LEFT);
    check(outline && std::string_view(vgui_dock_get_title(dock, outline)) == "大纲" &&
        !vgui_dock_get_zone_count(dock, VGUI_DOCK_CENTER), "unexpected workspace headings in Chinese");
    workbench.strings = ui::designer_strings(ui::designer_language::english);
    workbench.bench.refresh_language();
    vgui_context_set_ui_scale(ctx, 1.25f);
    workbench.frame();
    check_layout("File", "Save", "Export Package");
    check(std::string_view(vgui_dock_get_title(dock, outline)) == "Outline",
        "outline dock heading did not translate");
}

void exercise_workbench()
{
    temp_dir directory;
    test_workbench workbench(directory.path / "form.vgui.json");
    auto& model = workbench.model;
    check(workbench.bench.document_dock(), "workbench must provide a document dock");
    const auto root = model.current().root;
    check(workbench.bench.selection() == root, "initial selection must be the root");

    const auto panel = workbench.add("panel", root);
    workbench.select_row(workbench.panel_item(), panel);
    workbench.set_layout("Column");
    check(model.current().nodes.at(panel).props.at("layout") == ui::value{std::string("column")},
        "Panel inspector did not set column layout");
    const auto label = workbench.add("label", panel);
    workbench.select_row(workbench.panel_item(), panel);
    const auto field = workbench.add("edit", panel);
    workbench.select_row(workbench.panel_item(), panel);
    const auto image = workbench.add("image", panel);
    check(vgui_tree_get_child_count(workbench.outline(), workbench.panel_item()) == 3,
        "outline did not show the three children of the panel");

    workbench.select_child(0, label);
    workbench.set_text("text", "Contact details");
    check(model.current().nodes.at(label).props.at("text") == ui::value{std::string("Contact details")},
        "Label inspector text did not reach the session");
    workbench.select_child(1, field);
    workbench.set_text("placeholder", "Email address");
    check(model.current().nodes.at(field).props.at("placeholder") == ui::value{std::string("Email address")},
        "Edit placeholder did not reach the session");
    workbench.select_child(2, image);
    const auto before_resource = model.current();
    const auto revision = model.revision();
    workbench.set_text("resource", "../outside.png");
    check(model.current() == before_resource && model.revision() == revision,
        "invalid image path changed document or history");
    workbench.set_text("resource", "images/logo.png");
    const auto resource = std::get<std::string>(model.current().nodes.at(image).props.at("resource"));
    check(model.current().resources.at(resource) == "images/logo.png",
        "Image inspector did not register and assign the project-relative resource");
    const auto before_width = model.current();
    workbench.set_text("width", "96");
    check(model.current().nodes.at(image).props.at("width") == ui::value{96.0},
        "Image width did not reach the session as a number");
    const auto before_move = model.current();

    workbench.edit("Move Up");
    check(model.current().nodes.at(panel).children == std::vector<std::string>{label, image, field},
        "Move Up did not update sibling order in the session");
    const auto final_document = model.current();
    workbench.select_child(1, image);

    workbench.edit("Undo");
    check(model.current() == before_move && model.can_redo(), "Undo did not restore the pre-move document");
    workbench.select_child(2, image);
    workbench.edit("Undo");
    check(model.current() == before_width, "second Undo did not restore the pre-edit document");
    workbench.edit("Redo");
    check(model.current() == before_move, "Redo did not restore the inspector edit");
    workbench.edit("Redo");
    check(model.current() == final_document && !model.can_redo(), "second Redo did not restore the reordering");

    check(model.dirty(), "workbench changes must make the session dirty");
    workbench.file("Save As");
    check(workbench.save_calls == 1 && workbench.io_ok, "Save As failed: " + workbench.io_error.message);
    check(!model.dirty(), "Save As did not establish a session save point");
    workbench.edit("Undo");
    check(model.current() == before_move && model.dirty(), "Undo after saving did not diverge from the saved file");
    workbench.file("Open Page");
    check(workbench.open_calls == 1 && workbench.io_ok, "Open failed: " + workbench.io_error.message);
    check(model.current() == final_document, "reopened session lost hierarchy, sibling order or properties");
    check(!model.dirty() && !model.can_undo() && !model.can_redo(), "reopened session retained dirty state or history");
    check(workbench.bench.selection() == root, "reopened workbench did not select the root");
    workbench.select_child(1, image);
    check(std::string_view(vgui_edit_get_text(workbench.widget("property:resource", VGUI_EDIT))) == "images/logo.png",
        "inspector did not resolve the reopened image resource key to its path");
    check(std::string_view(vgui_edit_get_text(workbench.widget("property:width", VGUI_EDIT))) == "96",
        "inspector did not recover the saved image width");
}

void exercise_button_atlas()
{
    temp_dir directory;
    test_workbench workbench(directory.path / "atlas.vgui.json", ui::designer_language::simplified_chinese);
    auto& model = workbench.model;
    const auto root = model.current().root;
    const auto image = workbench.add("image", root);
    const auto button = workbench.add("button", root);
    const auto atlas_label = [&] {
        return std::string_view(vgui_widget_get_text(workbench.widget("property:atlas", VGUI_LABEL)));
    };
    const auto atlas_path = [&] {
        return std::string_view(vgui_edit_get_text(workbench.widget("property:atlas", VGUI_EDIT)));
    };
    check(atlas_label() == "四态图集路径" && atlas_path().empty(), "missing Chinese atlas label or empty default path");
    workbench.strings = ui::designer_strings(ui::designer_language::english);
    workbench.bench.refresh_language();
    workbench.frame();
    check(atlas_label() == "Button atlas path", "atlas path label did not switch to English");

    const auto before_atlas = model.current();
    const auto revision = model.revision();
    workbench.set_text("atlas", "../outside.png");
    check(model.current() == before_atlas && model.revision() == revision, "invalid atlas path changed document or history");

    constexpr char path[] = "images/button-states.png";
    workbench.set_text("atlas", path);
    const auto with_atlas = model.current();
    const auto& key = std::get<std::string>(with_atlas.nodes.at(button).props.at("atlas"));
    check(key.starts_with("image_") && with_atlas.resources.at(key) == path && atlas_path() == path,
        "atlas path did not register an image key or display the resolved path");
    workbench.edit("Undo");
    check(model.current() == before_atlas && atlas_path().empty(), "one Undo did not remove the atlas binding and resource");
    workbench.edit("Redo");
    check(model.current() == with_atlas && atlas_path() == path, "Redo did not restore the atlas binding and path");

    const auto select = [&](int index, std::string_view id) {
        auto* tree = workbench.outline();
        workbench.select_row(vgui_tree_get_child(tree, vgui_tree_get_root(tree, 0), index), id);
    };
    select(0, image);
    workbench.set_text("resource", path);
    check(model.current().nodes.at(image).props.at("resource") == ui::value{key} &&
        model.current().resources == with_atlas.resources, "Image path did not reuse the button atlas resource key");

    const auto saved = model.current();
    workbench.file("Save As");
    check(workbench.save_calls == 1 && workbench.io_ok && !model.dirty(),
        "could not save the shared atlas resource: " + workbench.io_error.message);
    workbench.file("Open Page");
    check(workbench.open_calls == 1 && workbench.io_ok && model.current() == saved,
        "reopening lost the shared atlas resource or binding");
    select(1, button);
    check(atlas_path() == path, "reopened atlas editor displayed the key instead of the path");
}

void exercise_live_properties()
{
    temp_dir directory;
    test_workbench workbench(directory.path / "live.vgui.json");
    auto& model = workbench.model;
    const auto id = workbench.add("label", model.current().root);
    workbench.file("Save As");
    workbench.file("Open Page");
    check(workbench.io_ok && !model.can_undo(), "failed to establish clean inspector baseline");
    const auto base = model.current();
    workbench.select_row(vgui_tree_get_child(workbench.outline(), vgui_tree_get_root(workbench.outline(), 0), 0), id);
    auto* ctx = workbench.ctx.get();
    auto* position = workbench.widget("property:x", VGUI_EDIT);
    workbench.click(position);
    vgui_edit_select_all(position);
    const auto type = [&](const char* text) {
        check(vgui_input_text(ctx, text), "inspector did not consume live text");
        workbench.frame();
    };
    type("-");
    check(model.current() == base && std::string_view(vgui_edit_get_text(position)) == "-",
        "incomplete number should stay in the field, not in the document");
    type("2");
    check(model.current().nodes.at(id).props.at("x") == ui::value{-2.0}, "valid number did not apply before Enter");
    type("5");
    check(model.current().nodes.at(id).props.at("x") == ui::value{-25.0}, "subsequent digit did not update the document");
    type("x");
    check(std::string_view(vgui_edit_get_text(position)) == "-25x" &&
        model.current().nodes.at(id).props.at("x") == ui::value{-25.0}, "invalid number replaced the last valid value");
    vgui_set_focus(ctx, nullptr);
    workbench.frame();
    check(std::string_view(vgui_edit_get_text(position)) == "-25", "blur did not restore the valid display text");
    workbench.edit("Undo");
    check(model.current() == base && !model.can_undo() && !model.dirty(), "one Undo did not restore the clean baseline");
    workbench.edit("Redo");
    const auto numeric = model.current();
    check(numeric.nodes.at(id).props.at("x") == ui::value{-25.0}, "Redo did not restore the final number");

    auto* label = workbench.widget("property:text", VGUI_EDIT);
    workbench.click(label);
    vgui_edit_select_all(label);
    type("First");
    workbench.key(VGUI_KEY_HOME);
    type("New ");
    check(model.current().nodes.at(id).props.at("text") == ui::value{std::string("New First")},
        "live refresh reset the caret or did not publish text");
    workbench.key(VGUI_KEY_ESCAPE);
    check(model.current() == numeric, "Escape did not discard live text");
    workbench.edit("Undo");
    check(model.current() == base && !model.can_undo(), "cancelled text left an undo entry");
}

void exercise_property_favorites()
{
    // Keep the real OVG draw list and intercept only the CPU image calls.
    std::unique_ptr<ovg_ctx_cb, decltype(&free_ctx_cb)> cb(new_ctx_cb(), free_ctx_cb);
    check(cb != nullptr, "could not create the favorites draw context");
    std::unique_ptr<rvg_t, void (*)(rvg_t*)> vg(cb->new_rvg(cb->ac), cb->destroy_rvg);
    check(vg != nullptr, "could not create the favorites draw list");
    struct image_draw { ivec4 dst; vgui_color tint; uint32_t id; };
    static std::vector<image_draw> images;
    cb->image_update = [](rvg_t*, vg_image_t* img, vg_image_desc_t*) {
        static uint32_t next_id = 1;
        img->id = next_id++;
    };
    cb->add_image = [](rvg_t*, ovg_image_r* img) {
        images.push_back({img->dst, img->color, img->img->id});
    };

    temp_dir directory;
    test_workbench workbench(directory.path / "favorites.vgui.json");
    auto* ctx = workbench.ctx.get();
    vgui_context_get_size(ctx, &vg->width, &vg->height);
    const auto baseline = workbench.model.current();
    const auto revision = workbench.model.revision();
    constexpr auto x_id = "property:x";
    constexpr auto y_id = "property:y";
    constexpr auto filter_id = "__property_fav_filter";
    const auto filter = [&] { return workbench.widget(filter_id, VGUI_BUTTON); };
    const auto star = [&](std::string_view id) -> vgui_widget* {
        auto* line = vgui_widget_get_parent(workbench.widget(id, VGUI_LABEL));
        for (std::size_t i = 0; i < vgui_widget_get_child_count(line); ++i) {
            auto* child = vgui_widget_get_child(line, i);
            if (vgui_widget_get_kind(child) == VGUI_BUTTON &&
                vgui_widget_get_a11y_role(child) == VGUI_A11Y_ROLE_TOGGLE &&
                id == vgui_widget_get_id(child)) return child;
        }
        throw std::runtime_error("missing favorite toggle: " + std::string(id));
    };
    const auto draw = [&] {
        check(workbench.model.current() == baseline && workbench.model.revision() == revision &&
            !workbench.model.dirty() && !workbench.model.can_undo() && !workbench.model.can_redo(),
            "property favorites changed the document or its history");
        images.clear();
        cb->clear(vg.get());
        vgui_draw(ctx, cb.get(), vg.get());
    };
    const auto check_icon = [&](vgui_widget* button, bool checked) {
        const std::string id = vgui_widget_get_id(button);
        check(!!vgui_widget_get_checked(button) == checked, "wrong checked state: " + id);
        check(std::string_view(vgui_widget_get_text(button)).empty(), "favorite toggle still draws text: " + id);
        float x, y, w, h;
        vgui_widget_get_rect(button, &x, &y, &w, &h);
        constexpr int size = 14;
        const ivec4 expected{static_cast<int>(std::lround(x + (w - size) * 0.5f)),
            static_cast<int>(std::lround(y + (h - size) * 0.5f)), size, size};
        // OVG receives 0xAABBGGRR, unlike VGUI's public 0xAARRGGBB colors.
        constexpr vgui_color gray = 0xffc4b09f, gold = 0xff66c7ff;
        uint32_t id_drawn = 0;
        for (const auto& image : images) {
            const float cx = image.dst.x + image.dst.z * 0.5f;
            const float cy = image.dst.y + image.dst.w * 0.5f;
            if (cx < x || cx >= x + w || cy < y || cy >= y + h) continue;
            check(!id_drawn && image.id && image.dst == expected, "expected one centered 14px favorite icon: " + id);
            check(image.tint == (checked ? gold : gray), "wrong favorite icon tint: " + id);
            id_drawn = image.id;
        }
        check(id_drawn, "missing favorite icon draw: " + id);
        return id_drawn;
    };
    const auto click = [&](vgui_widget* button) {
        workbench.click(button);
        workbench.frame();
        draw();
    };

    draw();
    const auto row_outline = check_icon(star(x_id), false);
    check_icon(star(y_id), false);
    const auto filter_outline = check_icon(filter(), false);
    check(std::string_view(vgui_widget_get_a11y_name(star(x_id))) == "Toggle favorite" &&
        std::string_view(vgui_widget_get_a11y_name(filter())) == "Favorites only",
        "favorite toggles lost their accessible labels");

    click(star(x_id));
    check(check_icon(star(x_id), true) != row_outline, "starred row did not switch CPU textures");
    check_icon(star(y_id), false);
    check_icon(filter(), false);

    click(filter());
    check(check_icon(filter(), true) != filter_outline, "active favorites filter did not switch CPU textures");
    check_icon(star(x_id), true);
    check(!find_widget(vgui_root(ctx), y_id, VGUI_LABEL), "favorites filter kept the unstarred row");

    click(filter());
    check_icon(filter(), false);
    check_icon(star(x_id), true);
    check_icon(star(y_id), false);

    click(star(x_id));
    check_icon(star(x_id), false);
    check_icon(star(y_id), false);
}

void exercise_text_color_picker()
{
    temp_dir directory;
    test_workbench workbench(directory.path / "text-color.vgui.json");
    auto& model = workbench.model;
    auto* ctx = workbench.ctx.get();
    vgui_theme theme{};
    vgui_context_get_theme(ctx, &theme);
    theme.text = 0xFF3489B7u;
    vgui_context_set_theme(ctx, &theme);
    const auto id = workbench.add("label", model.current().root);
    workbench.file("Save As");
    workbench.file("Open Page");
    check(workbench.io_ok && !model.can_undo() && !model.dirty(), "could not reopen the color fixture without history");
    auto* tree = workbench.outline();
    workbench.select_row(vgui_tree_get_child(tree, vgui_tree_get_root(tree, 0), 0), id);
    const auto inherited = model.current();

    constexpr auto row_id = "property:text_color";
    const std::string picker_id = "property-grid.color-picker";
    const auto row_edit = [&] { return workbench.widget(row_id, VGUI_EDIT); };
    const auto row_text = [&] { return std::string_view(vgui_edit_get_text(row_edit())); };
    const auto color = [&](const ui::document& doc) {
        const auto& props = doc.nodes.at(id).props;
        const auto it = props.find("text_color");
        return it == props.end() ? std::string{} : std::get<std::string>(it->second);
    };
    const auto open_picker = [&] {
        auto* swatch = workbench.widget(row_id, VGUI_PANEL);
        float w, h;
        vgui_widget_get_rect(swatch, nullptr, nullptr, &w, &h);
        check(w == 20 && h == 20, "text-color swatch is not 20x20");
        workbench.click(swatch);
        workbench.frame();
        auto* popup = vgui_popup_content(ctx);
        check(popup && picker_id == vgui_widget_get_id(popup), "text-color swatch did not open the grid picker");
        return popup;
    };
    const auto picker_hex = [&](vgui_widget* popup) {
        auto* hex = find_widget(popup, picker_id + ".hex", VGUI_EDIT);
        check(hex, "grid picker is missing its hex field");
        return ui::parse_text_color(vgui_edit_get_text(hex));
    };
    const auto swatch = [&](vgui_widget* popup, int index) {
        auto* widget = find_widget(popup, picker_id + ".swatch." + std::to_string(index), VGUI_PANEL);
        check(widget, "grid picker is missing a color swatch");
        return widget;
    };
    const auto choose_swatch = [&](vgui_widget* popup, int index, vgui_color expected) {
        const auto committed = model.committed();
        workbench.click(swatch(popup, index));
        workbench.frame();
        const auto preview = color(model.current());
        check(vgui_popup_content(ctx) == popup && picker_hex(popup) == expected &&
            ui::parse_text_color(preview) == expected && row_text() == preview,
            "clicking a swatch did not preview its color in the picker, document and property row");
        check(model.committed() == committed, "clicking a swatch committed before dismissal");
    };
    const auto adjust = [&](vgui_widget* popup, const char* control, float fx, float fy) {
        const auto previous = color(model.current());
        const auto committed = color(model.committed());
        auto* surface = find_widget(popup, picker_id + control, VGUI_CUSTOM);
        check(surface, std::string("grid picker is missing ") + control);
        float x, y, w, h;
        vgui_widget_get_rect(surface, &x, &y, &w, &h);
        check(w > 0 && h > 0, "picker control has no layout rect");
        workbench.click_at(x + w * fx, y + h * fy);
        workbench.frame();
        const auto preview = color(model.current());
        check(vgui_popup_content(ctx) == popup && preview != previous && ui::parse_text_color(preview) &&
            row_text() == preview, std::string("picker did not preview ") + control + " in the document and row");
        check(color(model.committed()) == committed, std::string("picker committed ") + control + " before dismissal");
    };

    check(!inherited.nodes.at(id).props.contains("text_color") && row_text().empty(),
        "default text color should inherit rather than store a hex override");
    auto* popup = open_picker();
    check(picker_hex(popup) == theme.text && model.current() == inherited && !model.can_undo(),
        "opening the inherited picker did not use the theme color without setting an override");
    choose_swatch(popup, 2, 0xFFFF0000u);
    float first_x, first_y, last_x, last_y;
    vgui_widget_get_rect(swatch(popup, 0), &first_x, &first_y, nullptr, nullptr);
    vgui_widget_get_rect(swatch(popup, 9), &last_x, &last_y, nullptr, nullptr);
    check(last_x > first_x && last_y == first_y, "the ten presets should fit in one horizontal row");
    adjust(popup, ".sv", 0.75f, 0.25f);
    adjust(popup, ".hue", 0.33f, 0.5f);
    adjust(popup, ".alpha", 0.4f, 0.5f);
    check(model.committed() == inherited, "picker adjustments committed before dismissal");
    check(workbench.key(VGUI_KEY_ESCAPE) && !vgui_popup_active(ctx), "Escape did not dismiss the color picker");
    check(model.current() == inherited && row_text().empty() && !model.can_undo() && !model.dirty(),
        "Escape did not restore inheritance without adding an undo entry");

    popup = open_picker();
    adjust(popup, ".sv", 0.6f, 0.15f);
    adjust(popup, ".hue", 0.8f, 0.5f);
    adjust(popup, ".alpha", 0.65f, 0.5f);
    const auto picked = model.current();
    check(model.committed() == inherited && picker_hex(popup) == ui::parse_text_color(color(picked)),
        "final RGBA preview committed early or disagreed with the picker hex field");
    workbench.click(workbench.outline());
    workbench.frame();
    check(!vgui_popup_active(ctx) && model.committed() == picked && model.current() == picked && model.can_undo(),
        "clicking outside did not dismiss the picker and commit the final preview");
    workbench.edit("Undo");
    check(model.current() == inherited && row_text().empty() && !model.can_undo() && !model.dirty(),
        "one Undo did not restore the inherited color and clean history");
    workbench.edit("Redo");
    check(model.current() == picked && row_text() == color(picked), "Redo did not restore the final picked color");

    popup = open_picker();
    check(picker_hex(popup) == ui::parse_text_color(color(picked)), "picker did not reopen with the stored color");
    float preset_y, recent_y;
    vgui_widget_get_rect(swatch(popup, 9), nullptr, &preset_y, nullptr, nullptr);
    vgui_widget_get_rect(swatch(popup, 10), nullptr, &recent_y, nullptr, nullptr);
    check(recent_y > preset_y, "recent colors should have a separate row below the presets");
    choose_swatch(popup, 3, 0xFF00FF00u);
    const auto picked_argb = ui::parse_text_color(color(picked));
    check(picked_argb && (*picked_argb >> 24) != 0xFFu, "recent color fixture should have a non-opaque alpha");
    choose_swatch(popup, 10, *picked_argb);
    adjust(popup, ".alpha", 0.2f, 0.5f);
    check(workbench.key(VGUI_KEY_ESCAPE) && !vgui_popup_active(ctx) && model.current() == picked &&
        model.committed() == picked && row_text() == color(picked), "Escape did not restore the explicit starting color");

    workbench.click(row_edit());
    check(vgui_get_focus(ctx) == row_edit(), "manual color field did not receive focus");
    const bool selected = vgui_input_key(ctx, 'A', 1, VGUI_MOD_CTRL);
    vgui_input_key(ctx, 'A', 0, VGUI_MOD_CTRL);
    check(selected && workbench.key(VGUI_KEY_BACKSPACE) && row_text().empty() && model.current() == inherited &&
        model.committed() == picked, "clearing the hex field did not preview inherited text color");
    workbench.key(VGUI_KEY_ENTER);
    check(model.committed() == inherited && row_text().empty(), "clearing the hex field did not commit inheritance");
    workbench.edit("Undo");
    check(model.current() == picked, "Undo did not restore the color cleared by typing");

    // The favorite button shares the row ID; select Reset by its action label.
    auto* line = vgui_widget_get_parent(row_edit());
    vgui_widget* reset = nullptr;
    for (std::size_t i = 0; i < vgui_widget_get_child_count(line); ++i) {
        auto* child = vgui_widget_get_child(line, i);
        if (vgui_widget_get_kind(child) == VGUI_BUTTON &&
            std::string_view(vgui_widget_get_tooltip(child)) == "Reset to default") reset = child;
    }
    check(reset, "text-color row is missing its Reset button");
    workbench.click(reset);
    workbench.frame();
    check(model.current() == inherited && model.committed() == inherited && row_text().empty(),
        "reset did not clear the override and restore inherited text color");
}

void exercise_font_properties()
{
    temp_dir directory;
    test_workbench workbench(directory.path / "font.vgui.json");
    auto& model = workbench.model;
    auto* ctx = workbench.ctx.get();
    const auto root = model.current().root;
    const auto panel = workbench.add("panel", root);
    workbench.select_row(workbench.panel_item(), panel);
    const auto label = workbench.add("label", panel);
    workbench.select_row(workbench.panel_item(), panel);
    const auto button = workbench.add("button", panel);
    workbench.select_row(workbench.panel_item(), panel);
    const auto edit = workbench.add("edit", panel);

    // Remove fixture history so live edits and the single Undo below are exact.
    workbench.file("Save As");
    check(workbench.io_ok, "could not save font inspector fixture: " + workbench.io_error.message);
    workbench.file("Open Page");
    check(workbench.io_ok && !model.dirty() && !model.can_undo() && workbench.bench.selection() == root,
        "could not reopen font inspector fixture without history");
    const auto baseline = model.current();

    const auto check_surface = [&](std::string_view category, std::string_view size_label,
                                   std::string_view weight_label, std::string_view slant_label,
                                   std::string_view color_label, std::string_view default_choice) {
        auto* category_button = workbench.widget("category:Font", VGUI_BUTTON);
        check(std::string_view(vgui_widget_get_text(category_button)) == "v  " + std::string(category),
            "font inspector category has the wrong visible label");
        check(std::string_view(vgui_widget_get_text(workbench.widget("property:font_size", VGUI_LABEL))) == size_label &&
            std::string_view(vgui_widget_get_text(workbench.widget("property:font_weight", VGUI_LABEL))) == weight_label &&
            std::string_view(vgui_widget_get_text(workbench.widget("property:font_slant", VGUI_LABEL))) == slant_label &&
            std::string_view(vgui_widget_get_text(workbench.widget("property:text_color", VGUI_LABEL))) == color_label,
            "font inspector field labels are incomplete or untranslated");
        check(std::string_view(vgui_edit_get_text(workbench.widget("property:font_size", VGUI_EDIT))) == "0" &&
            std::string_view(vgui_widget_get_text(workbench.widget("property:font_weight", VGUI_BUTTON))) == default_choice &&
            std::string_view(vgui_widget_get_text(workbench.widget("property:font_slant", VGUI_BUTTON))) == default_choice &&
            std::string_view(vgui_edit_get_text(workbench.widget("property:text_color", VGUI_EDIT))).empty(),
            "font inspector did not expose the expected inherited defaults");
    };
    const auto open_choices = [&](std::string_view property) {
        workbench.click(workbench.widget("property:" + std::string(property), VGUI_BUTTON));
        workbench.frame();
        auto* menu = vgui_context_get_open_menu(ctx);
        check(menu, "font choice editor did not open a menu: " + std::string(property));
        return menu;
    };
    const auto check_choices = [&](std::string_view property, std::initializer_list<std::string_view> labels) {
        check_menu_items(open_choices(property), labels);
        check(workbench.key(VGUI_KEY_ESCAPE) && !vgui_context_get_open_menu(ctx),
            "font choice menu did not close: " + std::string(property));
    };

    const std::string controls[] = {label, button, edit};
    for (int index = 0; index < 3; ++index) {
        workbench.select_child(index, controls[index]);
        check_surface("Font", "Font size", "Font weight", "Font slant", "Text color", "Use default");
    }

    workbench.select_child(0, label);
    check_choices("font_weight", {"Use default", "Regular", "Medium", "Semi-bold", "Bold"});
    check_choices("font_slant", {"Use default", "Upright", "Italic", "Oblique"});
    const auto focus_text = [&](const char* id) {
        auto* editor = workbench.widget(id, VGUI_EDIT);
        workbench.click(editor);
        check(vgui_get_focus(ctx) == editor, std::string("font editor did not receive focus: ") + id);
        vgui_edit_select_all(editor);
        return editor;
    };
    const auto type = [&](const char* text) {
        check(vgui_input_text(ctx, text), "font inspector did not consume live text");
        workbench.frame();
    };
    auto* size = focus_text("property:font_size");
    type("2");
    check(model.current() == baseline && std::string_view(vgui_edit_get_text(size)) == "2",
        "out-of-range intermediate font size changed the document");
    type("4");
    check(model.current().nodes.at(label).props.at("font_size") == ui::value{24.0},
        "valid font size did not apply before Enter");
    type("0");
    check(model.current().nodes.at(label).props.at("font_size") == ui::value{240.0},
        "subsequent valid font size did not apply before Enter");
    type("0");
    check(model.current().nodes.at(label).props.at("font_size") == ui::value{240.0} &&
        std::string_view(vgui_edit_get_text(size)) == "2400", "invalid font size replaced the last valid value");
    vgui_set_focus(ctx, nullptr);
    workbench.frame();
    workbench.edit("Undo");
    check(model.current() == baseline && !model.dirty() && !model.can_undo(),
        "one Undo did not remove the live font size edit");
    workbench.edit("Redo");
    check(model.current().nodes.at(label).props.at("font_size") == ui::value{240.0},
        "Redo did not restore the final valid font size");

    workbench.activate(open_choices("font_weight"), "Bold");
    workbench.activate(open_choices("font_slant"), "Italic");
    focus_text("property:text_color");
    type("#12AB34EF");
    const auto saved = model.current();
    const auto& props = saved.nodes.at(label).props;
    check(props.at("font_weight") == ui::value{std::string("bold")} &&
        props.at("font_slant") == ui::value{std::string("italic")} &&
        props.at("text_color") == ui::value{std::string("#12AB34EF")},
        "font choices or live hex text did not reach the model");
    workbench.file("Save As");
    check(workbench.io_ok && !model.dirty(), "could not save font properties: " + workbench.io_error.message);
    workbench.file("Open Page");
    check(workbench.io_ok && model.current() == saved && workbench.bench.selection() == root,
        "reopening lost font properties or selection state");
    workbench.select_child(0, label);
    check(std::string_view(vgui_edit_get_text(workbench.widget("property:font_size", VGUI_EDIT))) == "240" &&
        std::string_view(vgui_widget_get_text(workbench.widget("property:font_weight", VGUI_BUTTON))) == "Bold" &&
        std::string_view(vgui_widget_get_text(workbench.widget("property:font_slant", VGUI_BUTTON))) == "Italic" &&
        std::string_view(vgui_edit_get_text(workbench.widget("property:text_color", VGUI_EDIT))) == "#12AB34EF",
        "reopened font inspector did not display the saved values");

    workbench.strings = ui::designer_strings(ui::designer_language::simplified_chinese);
    workbench.bench.refresh_language();
    workbench.frame();
    const auto chinese_category = std::string_view(vgui_widget_get_text(workbench.widget("category:Font", VGUI_BUTTON)));
    check(chinese_category == "v  字体", "font inspector category did not translate to Chinese");
    check(std::string_view(vgui_widget_get_text(workbench.widget("property:font_size", VGUI_LABEL))) == "字号" &&
        std::string_view(vgui_widget_get_text(workbench.widget("property:font_weight", VGUI_LABEL))) == "字重" &&
        std::string_view(vgui_widget_get_text(workbench.widget("property:font_slant", VGUI_LABEL))) == "字形" &&
        std::string_view(vgui_widget_get_text(workbench.widget("property:text_color", VGUI_LABEL))) == "文字颜色" &&
        std::string_view(vgui_widget_get_text(workbench.widget("property:font_weight", VGUI_BUTTON))) == "粗体" &&
        std::string_view(vgui_widget_get_text(workbench.widget("property:font_slant", VGUI_BUTTON))) == "斜体",
        "reopened font inspector did not translate its fields or values");
    check_choices("font_weight", {"使用默认", "常规", "中等", "半粗", "粗体"});
    check_choices("font_slant", {"使用默认", "直立", "斜体", "倾斜"});
}

void exercise_font_family()
{
    temp_dir directory;
    test_workbench workbench(directory.path / "font-family.vgui.json");
    auto& model = workbench.model;
    auto* ctx = workbench.ctx.get();
    const auto root = model.current().root;
    const ui::edit fixture[] = {
        ui::insert_node{"label", root, {"label", "", {}, {}}},
        ui::insert_node{"button", root, {"button", "", {}, {}}},
        ui::insert_node{"edit", root, {"edit", "", {}, {}}},
        ui::set_resource{"logo", "assets/logo.png"},
    };
    check(model.apply(fixture), "could not seed the font family inspector fixture");
    workbench.frame();

    const auto select = [&](int index, std::string_view id) {
        auto* tree = workbench.outline();
        workbench.select_row(vgui_tree_get_child(tree, vgui_tree_get_root(tree, 0), index), id);
    };
    const auto caption = [&] {
        return std::string_view(vgui_widget_get_text(workbench.widget("property:font_family", VGUI_BUTTON)));
    };
    const auto open_choices = [&] {
        workbench.press("property:font_family");
        auto* menu = vgui_context_get_open_menu(ctx);
        check(menu, "font family editor did not open its choice menu");
        return menu;
    };
    const auto check_choices = [&](std::initializer_list<std::string_view> labels) {
        check_menu_items(open_choices(), labels);
        check(workbench.key(VGUI_KEY_ESCAPE) && !vgui_context_get_open_menu(ctx), "font family menu did not close");
    };
    const auto family = [&](const char* id) {
        const auto& props = model.current().nodes.at(id).props;
        const auto found = props.find("font_family");
        return found == props.end() ? ui::value{std::string{}} : found->second;
    };
    constexpr auto default_label = "Use default";
    constexpr auto sans_label = "DejaVu Sans";
    constexpr auto sans_sc_label = "Designer Sans SC (Chinese first)";
    constexpr auto font_path = "assets/fonts/Editorial.oTf";
    constexpr auto font_key = "project.editorial";

    select(0, "label");
    check(caption() == default_label, "label font family did not inherit by default");
    workbench.activate(open_choices(), sans_label);
    check(family("label") == ui::value{std::string("builtin.sans")} && caption() == sans_label,
        "selecting DejaVu Sans did not set the label's built-in family");
    select(1, "button");
    check(caption() == default_label, "button font family did not inherit by default");
    workbench.activate(open_choices(), sans_sc_label);
    check(family("button") == ui::value{std::string("builtin.sans_sc")} && caption() == sans_sc_label,
        "selecting Designer Sans SC did not set the button's built-in family");
    select(2, "edit");
    check(caption() == default_label, "edit font family did not inherit by default");
    check_choices({default_label, sans_label, sans_sc_label});

    // Simulate the host registering a font while the edit control stays inspected.
    const ui::edit add_font = ui::set_resource{font_key, font_path};
    check(model.apply({&add_font, 1}), "could not register the project font");
    workbench.frame();
    check(workbench.bench.selection() == "edit" && caption() == default_label,
        "registering a font disturbed the inspected control or its value");
    auto* menu = open_choices();
    check_menu_items(menu, {default_label, sans_label, sans_sc_label, font_path});
    workbench.activate(menu, font_path);
    check(family("edit") == ui::value{std::string(font_key)} && caption() == font_path,
        "the imported family must display the relative path but store the resource key");
    const auto saved = model.current();
    workbench.edit("Undo");
    check(workbench.bench.selection() == "edit" && caption() == default_label &&
        family("edit") == ui::value{std::string{}} && model.current().resources.contains(font_key),
        "Undo did not clear just the edit control's family choice");
    workbench.edit("Redo");
    check(model.current() == saved && caption() == font_path, "Redo did not restore the imported font choice");

    workbench.file("Save As");
    check(workbench.io_ok && !model.dirty(), "could not save the font family fixture");
    workbench.file("Open Page");
    check(workbench.io_ok && model.current() == saved, "reopening lost font family choices or resource paths");
    select(2, "edit");
    check(caption() == font_path, "the reopened inspector did not show the font's relative path");

    workbench.strings = ui::designer_strings(ui::designer_language::simplified_chinese);
    workbench.bench.refresh_language();
    workbench.frame();
    menu = open_choices();
    check_menu_items(menu, {"使用默认", sans_label, "Designer Sans SC（中文优先）", font_path});
    workbench.activate(menu, "使用默认");
    check(family("edit") == ui::value{std::string{}} && caption() == "使用默认",
        "the translated default choice did not clear the imported font");
}

void exercise_outline_icons()
{
    temp_dir directory;
    test_workbench workbench(directory.path / "outline-icons.vgui.json");
    const auto button = [&](std::string_view id) { return workbench.widget(id, VGUI_BUTTON); };
    constexpr std::pair<std::string_view, std::string_view> labels[] = {
        {"designer.move.up", "Up"}, {"designer.move.down", "Down"},
        {"designer.duplicate", "Copy"}, {"designer.delete", "Delete"},
        {"designer.move.pick", "Pick Up"}, {"designer.move.here", "Move Here"}};
    for (const auto& [id, label] : labels) {
        auto* icon = button(id);
        check(std::string_view(vgui_widget_get_text(icon)).empty() &&
            std::string_view(vgui_widget_get_a11y_name(icon)) == label && *vgui_widget_get_tooltip(icon),
            "outline button must be icon-only with an accessible name and tooltip");
    }
    auto* pick = button("designer.move.pick");
    auto* move = button("designer.move.here");
    check(!vgui_widget_get_enabled(pick) && !vgui_widget_get_enabled(move), "root must not be movable");

    auto& model = workbench.model;
    const auto root = model.current().root;
    const auto target = workbench.add("panel", root);
    workbench.bench.select(root);
    workbench.frame();
    const auto leaf = workbench.add("label", root);
    workbench.bench.select(leaf);
    workbench.frame();
    workbench.press("designer.move.up");
    check(model.current().nodes.at(root).children.front() == leaf, "up icon did not move the node");
    workbench.press("designer.move.down");
    check(model.current().nodes.at(root).children.back() == leaf, "down icon did not move the node");
    workbench.press("designer.duplicate");
    const std::string copy(workbench.bench.selection());
    check(copy != leaf && model.current().nodes.size() == 4, "copy icon did not duplicate the node");
    workbench.press("designer.delete");
    check(!model.current().nodes.contains(copy) && model.current().nodes.contains(leaf), "delete icon removed the wrong node");

    workbench.bench.select(leaf);
    workbench.frame();
    const auto revision = model.revision();
    workbench.press("designer.move.pick");
    check(std::string_view(vgui_widget_get_tooltip(pick)) == "Cancel Move" &&
        !vgui_widget_get_enabled(move) && model.revision() == revision, "pickup did not enter cancel mode safely");
    workbench.press("designer.move.pick");
    check(std::string_view(vgui_widget_get_a11y_name(pick)) == "Pick Up" && model.revision() == revision,
        "cancel icon did not clear the pending move");

    workbench.press("designer.move.pick");
    workbench.strings = ui::designer_strings(ui::designer_language::simplified_chinese);
    workbench.bench.refresh_language();
    workbench.frame();
    check(std::string_view(vgui_widget_get_tooltip(pick)) == "取消移动" &&
        std::string_view(vgui_widget_get_tooltip(button("designer.delete"))) == "删除", "tooltips did not switch language");
    workbench.bench.select(target);
    workbench.frame();
    workbench.press("designer.move.here");
    check(model.current().nodes.at(target).children == std::vector<std::string>{leaf} && model.revision() == revision + 1 &&
        std::string_view(vgui_widget_get_a11y_name(pick)) == "拾取" && !vgui_widget_get_enabled(move),
        "move-here icon did not commit one move and reset pickup state");
}

void exercise_outline_drag()
{
    temp_dir directory;
    test_workbench workbench(directory.path / "outline.vgui.json");
    auto* ctx = workbench.ctx.get();
    auto* tree = workbench.outline();
    auto& model = workbench.model;
    const auto root = model.current().root;

    // Resolve by tree path each time: a move, undo or open can rebuild the rows.
    const auto row = [&](std::initializer_list<int> path) {
        auto item = vgui_tree_get_root(tree, 0);
        for (const int index : path) item = vgui_tree_get_child(tree, item, index);
        check(item != 0, "missing drag fixture outline row");
        return item;
    };
    struct point { float x, y; };
    const auto row_point = [&](vgui_item_id item, float fraction) -> point {
        float x, y, w, h;
        check(vgui_tree_get_item_rect(tree, item, &x, &y, &w, &h) && w > 0 && h > 0,
            "drag fixture outline row has no visible rect");
        return {x + w * 0.5f, y + h * fraction};
    };

    const auto source = workbench.add("panel", root);
    const auto label = workbench.add("label", source);
    workbench.select_row(row({0}), source);
    const auto field = workbench.add("edit", source);
    workbench.select_row(row({0}), source);
    const auto nested = workbench.add("panel", source);
    workbench.select_row(row({}), root);
    const auto destination = workbench.add("panel", root);
    const auto button = workbench.add("button", destination);

    // Clear setup history through the UI so we can count undo entries precisely.
    const auto baseline = model.current();
    workbench.file("Save As");
    check(workbench.io_ok, "could not save drag fixture: " + workbench.io_error.message);
    workbench.file("Open Page");
    check(workbench.io_ok && model.current() == baseline && !model.dirty() &&
        !model.can_undo() && !model.can_redo(), "could not reopen drag fixture without history");
    vgui_tree_expand_all(tree);
    workbench.frame();

    const auto drag = [&](std::string_view name, point from, point to,
                          vgui_drop_hint_kind expected_hint, bool cancel = false) {
        const auto before = model.current();
        const auto revision = model.revision();
        const auto dirty = model.dirty();
        const auto can_undo = model.can_undo();
        const auto can_redo = model.can_redo();
        const auto expect = [&](bool condition, std::string_view what) {
            check(condition, std::string(name) + ": " + std::string(what));
        };
        const auto unchanged = [&] {
            return model.current() == before && model.revision() == revision &&
                model.dirty() == dirty && model.can_undo() == can_undo && model.can_redo() == can_redo;
        };

        vgui_input_mouse_move(ctx, from.x, from.y);
        expect(vgui_input_mouse_down(ctx, 1, from.x, from.y, 1), "outline did not consume mouse down");
        workbench.frame();
        expect(unchanged(), "press changed document or history");
        vgui_input_mouse_move(ctx, to.x, to.y);
        workbench.frame();
        expect(vgui_drag_active(ctx), "row drag did not start");
        vgui_drop_hint hint{};
        vgui_drag_hint(ctx, &hint);
        expect(hint.kind == expected_hint, "unexpected drop hint at destination");
        expect(unchanged(), "hover changed document or history before release");

        if (cancel) {
            expect(vgui_input_key(ctx, VGUI_KEY_ESCAPE, 1, 0), "Escape was not consumed");
            vgui_input_key(ctx, VGUI_KEY_ESCAPE, 0, 0);
            expect(!vgui_drag_active(ctx) && unchanged(), "Escape did not cancel the drag without editing");
        }
        // Even after Escape, send the physical release over the valid target.
        vgui_input_mouse_up(ctx, 1, to.x, to.y, 1);
        workbench.frame();
        expect(!vgui_drag_active(ctx), "mouse release left a drag active");
        if (cancel || expected_hint == VGUI_DROP_HINT_NONE)
            expect(unchanged(), "rejected or cancelled drop changed document or history");
    };
    const auto verify_move = [&](std::string_view name, const ui::document& expected, auto revision) {
        check(model.current() == expected && model.revision() == revision + 1 && model.dirty(),
            std::string(name) + ": release did not commit exactly the expected move");
        workbench.edit("Undo");
        check(model.current() == baseline && !model.dirty() && !model.can_undo() && model.can_redo(),
            std::string(name) + ": one Undo did not restore the clean baseline without extra entries");
    };

    // The lower edge means "after"; the third child catches an off-by-one
    // when the insertion index isn't adjusted for removal of the source.
    auto reordered = baseline;
    reordered.nodes.at(source).children = {field, label, nested};
    const auto reorder_revision = model.revision();
    drag("same-parent reorder", row_point(row({0, 0}), 0.5f), row_point(row({0, 1}), 0.875f),
        VGUI_DROP_HINT_LINE);
    verify_move("same-parent reorder", reordered, reorder_revision);

    // Run rejected drags with a redo entry present, so losing that entry also fails.
    drag("drop onto non-Panel", row_point(row({0, 1}), 0.5f), row_point(row({0, 0}), 0.5f),
        VGUI_DROP_HINT_NONE);
    drag("drop Panel onto its descendant", row_point(row({0}), 0.5f), row_point(row({0, 2}), 0.5f),
        VGUI_DROP_HINT_NONE);
    drag("Escape over valid Panel", row_point(row({0, 0}), 0.5f), row_point(row({1}), 0.5f),
        VGUI_DROP_HINT_RECT, true);

    auto reparented = baseline;
    reparented.nodes.at(source).children = {field, nested};
    reparented.nodes.at(destination).children = {button, label};
    const auto reparent_revision = model.revision();
    drag("cross-Panel reparent", row_point(row({0, 0}), 0.5f), row_point(row({1}), 0.5f),
        VGUI_DROP_HINT_RECT);
    verify_move("cross-Panel reparent", reparented, reparent_revision);
}

void exercise_control_schema()
{
    temp_dir directory;
    test_workbench workbench(directory.path / "controls.vgui.json");
    auto& model = workbench.model;
    auto* ctx = workbench.ctx.get();
    const auto root = model.current().root;
    std::map<std::string, std::string, std::less<>> created;
    for (const auto& type : ui::types()) {
        workbench.bench.select(root);
        workbench.frame();
        const auto id = workbench.add(type.key, root);
        const auto& node = model.current().nodes.at(id);
        check(std::get<double>(node.props.at("width")) > 0 && std::get<double>(node.props.at("height")) > 0,
            "new schema control has no default size: " + std::string(type.key));
        created.emplace(type.key, id);
    }
    check(created.size() == 19 && created.contains("gradient"), "some declarative core controls are missing from the Resource menu");

    const auto tabs = created.at("tabview");
    workbench.bench.select(tabs);
    workbench.frame();
    auto* submenu = workbench.open_new_control();
    check(!vgui_menu_get_item_enabled(submenu, menu_item(submenu, "Label")) &&
        vgui_menu_get_item_enabled(submenu, menu_item(submenu, "Panel")), "Tab View should accept panels only");
    check(workbench.key(VGUI_KEY_ESCAPE) && workbench.key(VGUI_KEY_ESCAPE), "could not close the restricted creation menu");
    workbench.add("panel", tabs);
    auto* search = workbench.widget("__property_search", VGUI_EDIT);
    workbench.click(search);
    check(vgui_input_text(ctx, "Page title"), "could not search for the tab page title property");
    workbench.frame();
    workbench.set_text("page_title", "Overview");
    check(std::get<std::string>(model.current().nodes.at(std::string(workbench.bench.selection())).props.at("page_title")) ==
        "Overview", "tab page title did not apply in the Inspector");
    workbench.click(search);
    vgui_edit_select_all(search);
    check(workbench.key(VGUI_KEY_BACKSPACE), "could not clear the Inspector search");

    const auto list = created.at("list");
    workbench.bench.select(list);
    workbench.frame();
    auto* editor = workbench.widget("property:items", VGUI_EDIT);
    vgui_a11y_info info{};
    vgui_widget_get_a11y_info(editor, &info);
    check(info.role == VGUI_A11Y_ROLE_MULTILINE_EDIT, "items should have a multiline inspector field");
    workbench.click(editor);
    vgui_edit_select_all(editor);
    const auto baseline = model.current();
    check(vgui_input_text(ctx, R"(["First",{"text":"Second","selected":true,"icon":"icons/open.png","buttons":[{"icon":"icons/open.png","action":"row.open"}]}])"),
        "could not edit list icon and inline button");
    workbench.frame();
    const auto preview = model.current();
    check(preview.resources.size() == 1, "the live inspector must register a single shared image");
    const auto icon_key = preview.resources.begin()->first;
    const auto item_json = std::string(R"(["First",{"buttons":[{"action":"row.open","icon":")") + icon_key +
        R"("}],"icon":")" + icon_key + R"(","selected":true,"text":"Second"}])";
    check(preview.resources.begin()->second == "icons/open.png" &&
        std::get<std::string>(preview.nodes.at(list).props.at("items")) == item_json && model.committed() == baseline,
        "the live inspector registers a shared image and rewrites both icon references before commit");
    vgui_edit_select_all(editor);
    check(vgui_input_text(ctx, R"([{"text":"Invalid","icon":"icons/new.png","buttons":[{"icon":"../outside.png"}]}])"),
        "could not enter an unsafe icon path");
    workbench.frame();
    check(model.current() == preview, "invalid nested icon path overwrote the last valid preview or registered other resources");
    vgui_edit_select_all(editor);
    check(vgui_input_text(ctx, "["), "could not enter invalid item JSON");
    workbench.frame();
    check(model.current() == preview, "invalid item JSON overwrote the last valid values");
    workbench.bench.select(root);
    workbench.frame();
    check(model.committed() == preview, "leaving the item field did not commit the last valid configuration");
    check(model.undo() && model.current() == baseline && model.redo() && model.current() == preview,
        "the item edit and its newly registered image must undo and redo as one step");
    workbench.frame();
    workbench.file("Save As");
    workbench.file("Open Page");
    check(workbench.io_ok && model.current() == preview, "new control/item edits did not survive save and reopen");
}

void exercise_label_orientation()
{
    temp_dir directory;
    test_workbench workbench(directory.path / "vertical-label.vgui.json", ui::designer_language::simplified_chinese);
    auto& model = workbench.model;
    const auto root = model.current().root;
    const ui::edit controls[] = {
        ui::insert_node{"caption", root, {"label", {}, {{"text", std::string("纵向标签")}}, {}}},
        ui::insert_node{"button", root, {"button", {}, {}, {}}},
    };
    check(model.apply(controls), "could not seed the label inspector fixture");
    workbench.bench.select("caption");
    workbench.frame();
    const auto initial = model.current();
    constexpr auto property = "property:text_orientation";
    check(std::string_view(vgui_widget_get_text(workbench.widget(property, VGUI_LABEL))) == "文字方向" &&
        std::string_view(vgui_widget_get_text(workbench.widget(property, VGUI_BUTTON))) == "横排",
        "label inspector did not expose localized horizontal default");
    workbench.press(property);
    auto* menu = vgui_context_get_open_menu(workbench.ctx.get());
    check(menu, "text orientation choices did not open");
    check_menu_items(menu, {"横排", "竖排（单列）"});
    workbench.activate(menu, "竖排（单列）");
    const auto vertical = model.current();
    check(vertical.nodes.at("caption").props.at("text_orientation") == ui::value{std::string("vertical")} &&
        model.committed() == vertical, "orientation choice did not apply immediately with the stable JSON value");
    check(model.undo() && model.current() == initial && model.redo() && model.current() == vertical,
        "orientation selection must be one undoable edit without changing geometry or content");
    workbench.bench.select("button");
    workbench.frame();
    check(!find_widget(vgui_root(workbench.ctx.get()), property, VGUI_BUTTON), "non-label inspector exposes label-only orientation");

    workbench.strings = ui::designer_strings(ui::designer_language::english);
    workbench.bench.refresh_language();
    workbench.frame();
    workbench.file("Save As");
    workbench.file("Open Page");
    check(workbench.io_ok && model.current() == vertical, "vertical label was lost on save/reopen");
    workbench.bench.select("caption");
    workbench.frame();
    check(std::string_view(vgui_widget_get_text(workbench.widget(property, VGUI_BUTTON))) == "Vertical (single column)",
        "reopened label did not display the localized vertical choice");
}

void exercise_button_gradients()
{
    temp_dir directory;
    test_workbench workbench(directory.path / "button-gradient.vgui.json");
    auto& model = workbench.model;
    auto* ctx = workbench.ctx.get();
    auto* root = vgui_root(ctx);
    const ui::edit design[] = {ui::insert_node{"button", model.current().root,
        {"button", {}, {{"gradient_preset", std::string("blue")}}, {}}}};
    check(model.apply(design), "could not seed the gradient inspector");
    workbench.bench.select("button");
    workbench.frame();
    constexpr auto row = "property:gradient_normal_top";
    const auto filter = [&](const char* label) {
        auto* edit = workbench.widget("__property_search", VGUI_EDIT);
        workbench.click(edit);
        vgui_edit_select_all(edit);
        check(vgui_input_text(ctx, label), "could not filter the inspector");
        workbench.frame();
    };
    const auto choose = [&](const char* label, const char* property, const char* choice) {
        filter(label);
        workbench.press(property);
        auto* menu = vgui_context_get_open_menu(ctx);
        check(menu, "gradient inspector choices did not open");
        workbench.activate(menu, choice);
    };
    const auto open_picker = [&] {
        workbench.click(workbench.widget(row, VGUI_PANEL));
        workbench.frame();
        auto* hex = find_widget(vgui_popup_content(ctx), "property-grid.color-picker.hex", VGUI_EDIT);
        check(hex, "gradient swatch did not open the color picker");
        return hex;
    };
    check(!find_widget(root, row, VGUI_EDIT), "solid buttons should not show gradient color rows");
    choose("Button style", "property:button_style", "Gradient");
    filter("Normal top");
    const auto inherited = model.committed();
    check(!inherited.nodes.at("button").props.contains("gradient_normal_top"), "showing inherited color should not create an override");
    auto* hex = open_picker();
    check(ui::parse_text_color(vgui_edit_get_text(hex)) == 0xFF4A8FE7u && model.current() == inherited,
        "empty gradient color should start from the current preset, not the theme color");
    workbench.click(hex);
    vgui_edit_select_all(hex);
    check(vgui_input_text(ctx, "#12345680"), "picker did not consume the RGBA value");
    workbench.frame();
    const auto custom = model.current();
    check(custom.nodes.at("button").props.at("gradient_normal_top") == ui::value{std::string("#12345680")} &&
        model.committed() == inherited, "gradient color should preview without Enter or an early commit");
    workbench.click(workbench.outline());
    workbench.frame();
    check(model.committed() == custom, "closing the picker should commit the custom color");
    check(model.undo() && model.current() == inherited && model.redo() && model.current() == custom,
        "one undo/redo should restore inheritance and the custom color");
    workbench.frame();
    choose("Button style", "property:button_style", "Solid");
    filter("Normal top");
    check(!find_widget(root, row, VGUI_EDIT) && model.current().nodes.at("button").props.contains("gradient_normal_top"),
        "switching to solid should hide, not remove, custom gradient colors");
    choose("Button style", "property:button_style", "Gradient");
    choose("Gradient preset", "property:gradient_preset", "Forest green");
    const auto saved = model.current();
    workbench.file("Save As");
    workbench.file("Open Page");
    check(workbench.io_ok && model.current() == saved, "custom gradient state was lost on save/reopen");
    workbench.bench.select("button");
    workbench.frame();
    filter("Normal top");
    auto* edit = workbench.widget(row, VGUI_EDIT);
    check(std::string_view(vgui_edit_get_text(edit)) == "#12345680", "preset changes must keep explicit overrides");
    workbench.click(edit);
    vgui_edit_select_all(edit);
    check(workbench.key(VGUI_KEY_BACKSPACE) && !model.current().nodes.at("button").props.contains("gradient_normal_top"),
        "clearing the override should restore inheritance without Enter");
    vgui_set_focus(ctx, nullptr);
    workbench.frame();
    check(ui::parse_text_color(vgui_edit_get_text(open_picker())) == 0xFF22C55Eu,
        "clearing the override should pick up the newly selected preset color");
    workbench.key(VGUI_KEY_ESCAPE);
}

void exercise_panel_gradient()
{
    temp_dir directory;
    test_workbench workbench(directory.path / "panel-gradient.vgui.json");
    auto& model = workbench.model;
    auto* ctx = workbench.ctx.get();
    const auto root = model.current().root;
    constexpr auto style = "property:background_style";
    constexpr auto direction = "property:background_gradient_direction";
    constexpr auto end = "property:background_gradient_end";
    const auto filter = [&](const char* text) {
        auto* search = workbench.widget("__property_search", VGUI_EDIT);
        workbench.click(search);
        vgui_edit_select_all(search);
        check(vgui_input_text(ctx, text), "could not filter the panel inspector");
        workbench.frame();
    };
    const auto choose = [&](const char* field, const char* option) {
        workbench.press(field);
        auto* menu = vgui_context_get_open_menu(ctx);
        check(menu, "panel gradient choices did not open");
        workbench.activate(menu, option);
    };
    filter("background");
    check(!find_widget(vgui_root(ctx), direction, VGUI_BUTTON), "solid panel shows gradient-only settings");
    choose(style, "Gradient");
    check(std::string_view(vgui_widget_get_text(workbench.widget(direction, VGUI_BUTTON))) == "Top to bottom",
        "new gradients should show the default vertical direction");
    choose(direction, "Left to right");
    const auto before = model.committed();
    auto* edit = workbench.widget(end, VGUI_EDIT);
    workbench.click(edit);
    vgui_edit_select_all(edit);
    check(vgui_input_text(ctx, "#25A4387F"), "gradient color edit did not receive input");
    workbench.frame();
    const auto after = model.current();
    const auto& props = after.nodes.at(root).props;
    check(model.committed() == before && props.at("background_style") == ui::value{std::string("gradient")} &&
        props.at("background_gradient_direction") == ui::value{std::string("horizontal")} &&
        props.at("background_gradient_end") == ui::value{std::string("#25A4387F")},
        "panel gradient properties should preview instantly using stable JSON keys");
    vgui_set_focus(ctx, nullptr);
    workbench.frame();
    workbench.file("Save As");
    workbench.file("Open Page");
    check(workbench.io_ok && model.current() == after, "panel gradient did not survive save/reopen");
    choose(style, "Solid");
    check(!find_widget(vgui_root(ctx), direction, VGUI_BUTTON) &&
        model.current().nodes.at(root).props.at("background_gradient_end") == props.at("background_gradient_end"),
        "returning to solid should hide, not discard, gradient properties");
}

void exercise_decorative_gradient()
{
    temp_dir directory;
    test_workbench workbench(directory.path / "gradient.vgui.json");
    auto& model = workbench.model;
    auto* ctx = workbench.ctx.get();
    const auto root = model.current().root;
    const auto id = workbench.add("gradient", root);
    auto* filter = workbench.widget("__property_search", VGUI_EDIT);
    workbench.click(filter);
    check(vgui_input_text(ctx, "gradient"), "could not search for gradient parameters");
    workbench.frame();
    constexpr auto field = "property:gradient_kind";
    constexpr auto stops_id = "property:gradient_stops";
    const auto choose = [&](const char* option) {
        workbench.press(field);
        auto* menu = vgui_context_get_open_menu(ctx);
        check(menu, "gradient type choices did not open");
        workbench.activate(menu, option);
    };
    choose("Radial");
    check(find_widget(vgui_root(ctx), "property:gradient_radius1", VGUI_EDIT) &&
        !find_widget(vgui_root(ctx), "property:gradient_angle0", VGUI_EDIT), "radial inspector exposed the wrong geometry fields");
    choose("Sweep (conic)");
    check(find_widget(vgui_root(ctx), "property:gradient_angle0", VGUI_EDIT) &&
        !find_widget(vgui_root(ctx), "property:gradient_radius1", VGUI_EDIT), "sweep inspector exposed the wrong geometry fields");
    const auto open = [&] {
        workbench.click(workbench.widget(stops_id, VGUI_CUSTOM));
        workbench.frame();
        auto* popup = vgui_popup_content(ctx);
        check(popup && std::string_view(vgui_widget_get_id(popup)) == "property-grid.gradient-editor", "gradient swatch did not open the color-stop editor");
        return popup;
    };
    const auto preset = [&](vgui_widget* popup) {
        auto* button = find_widget(popup, "property-grid.gradient-editor.preset.1", VGUI_BUTTON);
        check(button, "gradient popup did not expose its presets");
        workbench.click(button);
        workbench.frame();
    };
    const auto baseline = model.committed();
    preset(open());
    check(model.current().nodes.at(id).props.contains("gradient_stops") && model.committed() == baseline,
        "gradient stop changes must preview before the popup closes");
    check(workbench.key(VGUI_KEY_ESCAPE) && model.current() == baseline, "Escape should cancel pending changes to gradient stops");
    preset(open());
    workbench.click(workbench.widget("designer.outliner", VGUI_TREE));
    workbench.frame();
    check(!vgui_popup_content(ctx) && model.committed().nodes.at(id).props.contains("gradient_stops"),
        "dismissing the gradient popup should commit its last preview");
    const auto saved = model.committed();
    workbench.file("Save As");
    workbench.file("Open Page");
    check(workbench.io_ok && model.current() == saved, "gradient kind and stops did not survive reopening");
}

void exercise_radio_group_inspector()
{
    temp_dir directory;
    test_workbench workbench(directory.path / "radio-groups.vgui.json");
    auto& model = workbench.model;
    auto* ctx = workbench.ctx.get();
    const auto root = model.current().root;
    const ui::edit radios[] = {
        ui::insert_node{"light", root, {"radio", "Light", {{"radio_group", std::string("主题")}, {"checked", true}}, {}}},
        ui::insert_node{"dark", root, {"radio", "Dark", {{"radio_group", std::string("主题")}}, {}}},
        ui::insert_node{"language", root, {"radio", "Language", {{"radio_group", std::string("语言")}, {"checked", true}}, {}}},
        ui::insert_node{"unnamed", root, {"radio", "Default", {{"checked", true}}, {}}},
    };
    check(model.apply(radios), "could not seed same-container independent radio groups");
    workbench.bench.select("dark");
    workbench.frame();
    const auto checked = [&](const char* id) {
        const auto& props = model.current().nodes.at(id).props;
        const auto value = props.find("checked");
        return value != props.end() && std::get<bool>(value->second);
    };
    const auto initial = model.current();
    workbench.click(workbench.widget("property:checked", VGUI_CHECKBOX));
    workbench.frame();
    const auto changed = model.current();
    check(checked("dark") && !checked("light") && checked("language") && checked("unnamed"),
        "Initially checked must transfer the default choice only within the named group");
    check(model.undo() && model.current() == initial && model.redo() && model.current() == changed,
        "transferring the default choice is one undo/redo operation");
    workbench.frame();

    const auto type_group = [&](const char* group) {
        auto* edit = workbench.widget("property:radio_group", VGUI_EDIT);
        workbench.click(edit);
        vgui_edit_select_all(edit);
        check(vgui_input_text(ctx, group), "group editor did not accept a name");
        workbench.frame();
    };
    type_group("语言");
    const auto moved = model.current();
    check(model.committed() == changed && moved != changed && checked("dark") && !checked("language") && checked("unnamed") &&
        std::get<std::string>(moved.nodes.at("dark").props.at("radio_group")) == "语言",
        "moving a checked radio into a group must update peer selection live without Enter");
    check(workbench.key(VGUI_KEY_ESCAPE) && model.current() == changed, "Escape must revert the live group and its peer's check state");
    workbench.frame();
    type_group("语言");
    workbench.bench.select(root);
    workbench.frame();
    check(model.committed() == moved && model.undo() && model.current() == changed && model.redo() && model.current() == moved,
        "changing group membership and the destination choice commits and undoes atomically");
    workbench.bench.select("dark");
    workbench.frame();
    auto* group_edit = workbench.widget("property:radio_group", VGUI_EDIT);
    workbench.click(group_edit);
    vgui_edit_select_all(group_edit);
    check(workbench.key(VGUI_KEY_BACKSPACE) && !model.current().nodes.at("dark").props.contains("radio_group") &&
        checked("dark") && !checked("unnamed"), "clearing a checked radio's name moves it to the default group");
    check(workbench.key(VGUI_KEY_ESCAPE) && model.current() == moved, "cancelling default regroup restores named and default choices");
    workbench.frame();
    workbench.file("Save As");
    workbench.file("Open Page");
    check(workbench.io_ok && model.current() == moved, "group names and updated initial choices must survive save/reopen");
    workbench.bench.select("dark");
    workbench.strings = ui::designer_strings(ui::designer_language::simplified_chinese);
    workbench.bench.refresh_language();
    workbench.frame();
    check(std::string_view(vgui_widget_get_a11y_name(workbench.widget("property:radio_group", VGUI_EDIT))) == "组名",
        "the radio group field must have a localized Chinese label");
}

void exercise_menu_language()
{
    using ui::designer_language;
    temp_dir directory;
    test_workbench workbench(directory.path / "locale.vgui.json", designer_language::simplified_chinese);
    auto* ctx = workbench.ctx.get();
    check_menu_titles(workbench.menubar(), {"文件", "编辑", "资源", "视图", "工具", "帮助"});
    auto* file = workbench.open_menu("文件");
    menu_item(file, "保存");
    menu_item(file, "另存为");
    check(workbench.key(VGUI_KEY_ESCAPE), "could not dismiss localized File menu");
    auto* edit = workbench.open_menu("编辑");
    check(!vgui_menu_get_item_enabled(edit, menu_item(edit, "撤销")) &&
        !vgui_menu_get_item_enabled(edit, menu_item(edit, "重做")), "localized Undo and Redo lost their disabled state");
    check(workbench.key(VGUI_KEY_ESCAPE), "could not dismiss localized Edit menu");

    auto* submenu = workbench.open_new_control();
    check_menu_items(submenu, {"面板", "标签", "按钮", "输入框", "图片", "渐变", "复选框", "单选按钮", "开关",
        "滑块", "进度条", "分隔线", "列表", "树", "菜单", "菜单栏", "分栏", "选项卡", "停靠容器"});
    workbench.highlight(submenu, "按钮");
    const auto revision = workbench.model.revision();
    workbench.strings = ui::designer_strings(designer_language::english);
    workbench.bench.refresh_language();
    workbench.frame();
    check_menu_titles(workbench.menubar(), {"File", "Edit", "Resources", "View", "Tools", "Help"});
    if (auto* resources = vgui_context_get_open_menu(ctx)) {
        // If the popup stays open, its labels must not lag behind the bar.
        const auto item = menu_item(resources, "New Control");
        submenu = vgui_menu_get_submenu(resources, item);
        check_menu_items(submenu, {"Panel", "Label", "Button", "Edit", "Image", "Gradient", "Checkbox", "Radio Button", "Switch",
            "Slider", "Progress Bar", "Separator", "List", "Tree", "Menu", "Menu Bar", "Splitter", "Tab View", "Dock"});
        if (vgui_menu_is_open(submenu))
            check(workbench.key(VGUI_KEY_ESCAPE) && !vgui_menu_is_open(submenu),
                "translated New Control popup did not dismiss with Escape");
        check(workbench.key(VGUI_KEY_ESCAPE) && !vgui_context_get_open_menu(ctx),
            "translated Resources popup did not dismiss with Escape");
    }
    check(workbench.model.revision() == revision && !workbench.model.dirty(),
        "translating the active menu changed the document");
    submenu = workbench.open_new_control();
    check_menu_items(submenu, {"Panel", "Label", "Button", "Edit", "Image", "Gradient", "Checkbox", "Radio Button", "Switch",
        "Slider", "Progress Bar", "Separator", "List", "Tree", "Menu", "Menu Bar", "Splitter", "Tab View", "Dock"});
    check(workbench.key(VGUI_KEY_ESCAPE) && workbench.key(VGUI_KEY_ESCAPE) && !vgui_context_get_open_menu(ctx),
        "could not dismiss the reopened New Control popup chain");
}

void exercise_layout_preferences()
{
    temp_dir directory;
    const auto settings = directory.path / "settings.json";
    const auto write = [&](const char* text) { check(bool(std::ofstream(settings, std::ios::binary) << text), "cannot seed layout settings"); };
    const auto read = [&] {
        std::ifstream input(settings, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(input), {});
    };
    constexpr ui::designer_dock_widths defaults{240, 345}, expanded{304.5f, 390.f},
        saved_left{expanded.left, expanded.right, true, false}, saved_right{expanded.left, expanded.right, false, true};
    check(ui::load_designer_dock_widths(settings) == defaults && !fs::exists(settings),
        "missing preferences did not use the defaults or created a file");
    write(R"({"layout":{"left_width":304.5,"right_width":390}})");
    check(ui::load_designer_dock_widths(settings) == expanded, "legacy width-only preferences did not default to expanded sides");
    write(R"({"layout":{"left_width":304.5,"right_width":[],"left_collapsed":true,"right_collapsed":1}})");
    check(ui::load_designer_dock_widths(settings) == ui::designer_dock_widths{304.5f, 345, true, false},
        "malformed right-side members did not default independently of the valid left-side members");
    write(R"({"language":"en-US","theme":"custom","layout":{)"
          R"("left_width":"invalid","right_width":390,"left_collapsed":"true","right_collapsed":true,"other":true}})");
    check(ui::load_designer_dock_widths(settings) == ui::designer_dock_widths{240, 390, false, true},
        "malformed left-side members did not default independently of the valid right-side members");
    check(ui::save_designer_dock_widths(settings, saved_left) && ui::load_designer_dock_widths(settings) == saved_left &&
        ui::save_designer_dock_widths(settings, saved_right) && ui::load_designer_dock_widths(settings) == saved_right,
        "side preferences did not round-trip independent flags and fractional widths");
    const auto saved_settings = read();
    check(ui::load_designer_language(settings) == ui::designer_language::english &&
        saved_settings.find("\"theme\": \"custom\"") != std::string::npos &&
        saved_settings.find("\"other\": true") != std::string::npos, "saving the side preferences lost unrelated settings");
    check(ui::save_designer_language(settings, ui::designer_language::simplified_chinese) &&
        ui::load_designer_dock_widths(settings) == saved_right, "saving language overwrote the layout preference");

    test_workbench restored(directory.path / "document.vgui.json");
    auto* ctx = restored.ctx.get();
    auto* dock = restored.widget("designer.dock", VGUI_DOCK);
    const auto actual_width = [&](vgui_dock_zone zone) { return dock_zone_width(dock, zone); };
    vgui_context_set_ui_scale(ctx, 1.25f);
    restored.bench.set_dock_widths(ui::load_designer_dock_widths(settings));
    restored.frame();
    check(restored.bench.dock_widths() == saved_right &&
        actual_width(VGUI_DOCK_LEFT) == expanded.left * 1.25f && actual_width(VGUI_DOCK_RIGHT) < expanded.right * 1.25f,
        "applying saved preferences did not restore the Inspector rail and scaled Outline width");
    restored.bench.set_dock_widths(expanded);
    restored.frame();
    check(actual_width(VGUI_DOCK_LEFT) == expanded.left * 1.25f && actual_width(VGUI_DOCK_RIGHT) == expanded.right * 1.25f,
        "restored sizes did not scale to the display DPI");
    vgui_context_set_size(ctx, 950, 650);
    restored.frame();
    check(actual_width(VGUI_DOCK_LEFT) < expanded.left * 1.25f && restored.bench.dock_widths() == expanded &&
        !restored.model.dirty(), "temporary layout constraints changed the stored widths or document");

    write(R"({"layout":{"left_width":-5,"right_width":100000,"left_collapsed":null,"right_collapsed":[]}})");
    check(ui::load_designer_dock_widths(settings) == defaults, "out-of-range widths or invalid collapse flags were not rejected");
    write("{bad json");
    check(ui::load_designer_dock_widths(settings) == defaults && !ui::save_designer_dock_widths(settings, saved_right) &&
        read() == "{bad json", "saving the layout silently replaced a malformed settings file");
}

void exercise_language_preferences()
{
    temp_dir directory;
    const auto settings = directory.path / "settings.json";
    const auto design = directory.path / "form.vgui.json";
    const auto write = [&](const char* text) {
        std::ofstream file(settings, std::ios::binary | std::ios::trunc);
        file << text;
        file.close();
        check(bool(file), "could not write test settings");
    };
    const auto read = [&] {
        std::ifstream file(settings, std::ios::binary);
        check(bool(file), "could not read test settings");
        return std::string(std::istreambuf_iterator<char>(file), {});
    };
    using ui::designer_language;
    bool invalid = true;
    const auto default_language = ui::load_designer_language(settings, &invalid);
    check(default_language == designer_language::simplified_chinese && !invalid && !fs::exists(settings),
        "default locale is not Chinese or reading created a settings file");
    test_workbench chinese(design, default_language);
    check(std::string_view(vgui_widget_get_text(chinese.widget("designer.file.save", VGUI_BUTTON))) == "保存",
        "Chinese catalog did not reach the toolbar");
    chinese.set_layout("纵向排列");
    check(chinese.model.current().nodes.at(chinese.model.current().root).props.at("layout") == ui::value{std::string("column")},
        "localized inspector choice changed the serialized layout value");

    const auto display = [&](std::string_view id, vgui_widget_kind kind) {
        return std::string_view(vgui_widget_get_text(chinese.widget(id, kind)));
    };
    auto* tree = chinese.outline();
    const auto root = chinese.model.current().root;
    const auto child = chinese.add("label", root);
    const auto root_row = vgui_tree_get_root(tree, 0);
    chinese.select_row(root_row, root);
    const auto child_row = vgui_tree_get_child(tree, root_row, 0);
    check(std::string_view(vgui_tree_get_item_text(tree, child_row)) == "标签 [" + child + "]", "missing translated outline row");
    vgui_tree_set_item_expanded(tree, root_row, 0);
    chinese.frame();
    chinese.press("category:General");
    check(display("category:General", VGUI_BUTTON) == ">  常规", "could not collapse General");
    auto* search = chinese.widget("__property_search", VGUI_EDIT);
    chinese.click(search);
    check(vgui_input_text(chinese.ctx.get(), "layout"), "could not enter an inspector filter");
    chinese.frame();
    const auto revision = chinese.model.revision();

    chinese.strings = ui::designer_strings(designer_language::english);
    chinese.bench.refresh_language();
    chinese.frame();
    check(display("designer.file.save", VGUI_BUTTON) == "Save" &&
        display("property:layout", VGUI_BUTTON) == "Column", "English labels were not applied in place");
    check(chinese.widget("__property_search", VGUI_EDIT) == search &&
        std::string_view(vgui_edit_get_text(search)) == "layout" &&
        std::string_view(vgui_widget_get_a11y_name(search)) == "Search properties",
        "relabeling reset or failed to translate the inspector search");
    check(chinese.outline() == tree && vgui_tree_get_root(tree, 0) == root_row &&
        !vgui_tree_get_item_expanded(tree, root_row) && vgui_tree_get_child(tree, root_row, 0) == child_row &&
        std::string_view(vgui_tree_get_item_text(tree, child_row)) == "Label [" + child + "]" &&
        chinese.model.revision() == revision && chinese.model.dirty(),
        "relabeling changed the outline or document");
    vgui_edit_select_all(search);
    chinese.key(VGUI_KEY_BACKSPACE);
    chinese.frame();
    check(display("category:General", VGUI_BUTTON) == ">  General", "relabeling lost the collapsed inspector group");

    write(R"({"theme":"custom"})");
    check(ui::save_designer_language(settings, designer_language::english), "could not save English preference");
    const auto saved = ui::load_designer_language(settings, &invalid);
    check(saved == designer_language::english && !invalid && read().find("\"theme\": \"custom\"") != std::string::npos,
        "reloading lost the language or another preference");
    test_workbench english(design, saved);
    check(std::string_view(vgui_widget_get_text(english.widget("designer.file.save", VGUI_BUTTON))) == "Save",
        "reloading English did not change the toolbar");
    check(ui::save_designer_language(settings, designer_language::simplified_chinese) &&
        ui::load_designer_language(settings) == designer_language::simplified_chinese &&
        read().find("zh-CN") != std::string::npos, "could not switch back to Chinese");

    for (const char* unsupported : {R"({"language":"fr-FR"})", R"({"language":15})", "{broken json"}) {
        write(unsupported);
        check(ui::load_designer_language(settings, &invalid) == designer_language::simplified_chinese && invalid &&
            read() == unsupported, "unsupported settings did not fall back without rewriting");
    }
    check(!ui::save_designer_language(settings, designer_language::english) && read() == "{broken json",
        "saving a language silently replaced a malformed settings file");
}
} // namespace

int main()
{
    try {
        exercise_menus();
        exercise_side_resize();
        exercise_side_collapse();
        exercise_toolbar();
        exercise_workbench();
        exercise_button_atlas();
        exercise_live_properties();
        exercise_property_favorites();
        exercise_text_color_picker();
        exercise_font_properties();
        exercise_font_family();
        exercise_outline_icons();
        exercise_outline_drag();
        exercise_control_schema();
        exercise_label_orientation();
        exercise_button_gradients();
        exercise_panel_gradient();
        exercise_decorative_gradient();
        exercise_radio_group_inspector();
        exercise_menu_language();
        exercise_language_preferences();
        exercise_layout_preferences();
        std::puts("UI Designer workbench smoke test passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "UI Designer workbench smoke test failed: %s\n", error.what());
        return 1;
    }
}
