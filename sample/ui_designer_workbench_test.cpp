// U2 workbench smoke test: real VGUI input, without a window or GPU.
#include "ui_designer_locale.h"
#include "ui_designer_workbench.h"
#include "../gui/ui_design/model.h"

#include <glm/glm.hpp> // ovg_c.h uses GLM types in C++.
#include <ovg_c.h>
#include <vgui/vgui.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
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
    int new_calls = 0, quick_save_calls = 0, export_calls = 0, quit_calls = 0;
    bool io_ok = false;
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
    check_menu_items(file, {"New Page", "Open Page", "Save", "Save As", {}, "Export Package", {}, "Exit"});
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

    auto* submenu = workbench.open_new_control();
    auto* resources = vgui_context_get_open_menu(ctx);
    check_menu_items(submenu, {"Panel", "Label", "Button", "Edit", "Image"});
    check(vgui_menu_get_count(submenu) == static_cast<int>(ui::types().size()), "New Control does not match the schema");
    workbench.highlight(submenu, "Image");
    check(workbench.key(VGUI_KEY_ESCAPE) && !vgui_menu_is_open(submenu) &&
        vgui_context_get_open_menu(ctx) == resources, "first Escape should close only New Control");
    check(workbench.key(VGUI_KEY_ESCAPE) && !vgui_context_get_open_menu(ctx) && !vgui_menubar_get_active(bar),
        "second Escape should close Resources and deactivate its title");
    check(model.revision() == initial_revision && !model.can_undo() && !model.dirty(),
        "cancelling New Control changed the document or its history");

    const auto root = model.current().root;
    const auto panel = workbench.add("panel", root);
    std::vector<std::string> leaves;
    for (const auto& type : ui::types()) {
        if (type.key == "panel") continue;
        // The first leaf is added to the selected Panel. The others must be
        // added beside the selected leaf, without changing earlier siblings.
        leaves.push_back(workbench.add(type.key, panel));
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
    const std::string filename = "界面设计器界面设计器界面设计器界面设计器界面设计器界面设计器.vgui.json";
    workbench.bench.document_title(filename);
    auto* title = workbench.widget("designer.title", VGUI_LABEL);
    auto* marker = workbench.widget("designer.document.state", VGUI_PANEL);
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
        const auto caption = bounds(title);
        auto* menubar = workbench.menubar();
        const auto menus = bounds(menubar);
        check(inside(toolbar, {0, 0, 950, 650}) && toolbar.x == 0 && toolbar.y == 0 && toolbar.w == 950,
            "toolbar must fill the top edge of the window content area");
        check(menus.x == toolbar.x, "MenuBar has a left inset inside the toolbar");
        check(inside(caption, toolbar) && inside(menus, toolbar) && separate(caption, menus),
            "document title and MenuBar are clipped or overlapping");
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
            check(inside(r, toolbar) && separate(r, menus) && separate(r, caption),
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
    const std::string_view shortened = vgui_widget_get_text(title);
    check(shortened.ends_with("…") && filename.starts_with(shortened.substr(0, shortened.size() - 3)) &&
        std::string_view(vgui_widget_get_tooltip(title)) == filename,
        "long document name was not shortened while retaining the complete name");
    check(std::string_view(vgui_widget_get_tooltip(marker)).empty(), "clean document has an unsaved marker");
    workbench.add("label", workbench.model.current().root);
    check(std::string_view(vgui_widget_get_tooltip(marker)) == "未保存的修改" &&
        std::string_view(vgui_widget_get_a11y_name(title)) == filename + " — 未保存的修改",
        "dirty document status was not exposed independently of its name");
    workbench.strings = ui::designer_strings(ui::designer_language::english);
    workbench.bench.refresh_language();
    vgui_context_set_ui_scale(ctx, 1.25f);
    workbench.frame();
    check_layout("File", "Save", "Export Package");
    check(std::string_view(vgui_widget_get_tooltip(marker)) == "Unsaved changes", "dirty marker did not translate");
    workbench.edit("Undo");
    check(!workbench.model.dirty() && std::string_view(vgui_widget_get_tooltip(marker)).empty(), "undo did not clear the dirty marker");
}

void exercise_workbench()
{
    temp_dir directory;
    test_workbench workbench(directory.path / "form.vgui.json");
    auto& model = workbench.model;
    check(workbench.bench.preview_host(), "workbench must provide a preview host");
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
    check_menu_items(submenu, {"面板", "标签", "按钮", "输入框", "图片"});
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
        check_menu_items(submenu, {"Panel", "Label", "Button", "Edit", "Image"});
        if (vgui_menu_is_open(submenu))
            check(workbench.key(VGUI_KEY_ESCAPE) && !vgui_menu_is_open(submenu),
                "translated New Control popup did not dismiss with Escape");
        check(workbench.key(VGUI_KEY_ESCAPE) && !vgui_context_get_open_menu(ctx),
            "translated Resources popup did not dismiss with Escape");
    }
    check(workbench.model.revision() == revision && !workbench.model.dirty(),
        "translating the active menu changed the document");
    submenu = workbench.open_new_control();
    check_menu_items(submenu, {"Panel", "Label", "Button", "Edit", "Image"});
    check(workbench.key(VGUI_KEY_ESCAPE) && workbench.key(VGUI_KEY_ESCAPE) && !vgui_context_get_open_menu(ctx),
        "could not dismiss the reopened New Control popup chain");
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
        exercise_toolbar();
        exercise_workbench();
        exercise_live_properties();
        exercise_outline_drag();
        exercise_menu_language();
        exercise_language_preferences();
        std::puts("UI Designer workbench smoke test passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "UI Designer workbench smoke test failed: %s\n", error.what());
        return 1;
    }
}
