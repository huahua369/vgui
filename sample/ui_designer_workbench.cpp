#include "ui_designer_workbench.h"

#include "model.h"
#include "ui_designer_locale.h"
#include <vgui/editor/command_registry.h>
#include <vgui/editor/outliner_view.h>
#include <vgui/editor/property_grid.h>

#include <algorithm>
#include <charconv>
#include <initializer_list>
#include <map>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

namespace vgui::ui_design {
namespace {
namespace kit = vgui_editor_kit;

constexpr auto node_name = "@name";
constexpr auto node_id = "@id";
constexpr auto resource_key = "@resource-key";

// Workbench chrome only; these colors never enter the design or its preview.
constexpr vgui_color toolbar_bg = 0xFF202733;
constexpr vgui_color toolbar_edge = 0xFF354151;
constexpr vgui_color toolbar_text = 0xFFE5ECF6;
constexpr vgui_color toolbar_inset = 0xFF19202B;
constexpr vgui_color toolbar_primary = 0xFF2F65CB;
constexpr vgui_color document_ready = 0xFF7DAAFF;
constexpr vgui_color document_dirty = 0xFFF5BA63;
constexpr float toolbar_padding_right = 10;
constexpr float toolbar_gap = 10;
constexpr float document_chrome_width = 33; // Left/right padding, marker, and gap.
constexpr float page_tab_height = 32;

// Seed new controls only. A missing or zero dimension in a design still means auto-size.
constexpr std::pair<double, double> initial_control_size(std::string_view type)
{
    if (type == "panel") return {240, 160};
    if (type == "label") return {120, 24};
    if (type == "edit") return {200, 36};
    if (type == "image") return {128, 128};
    return {120, 36}; // Button and fallback for any newly registered type.
}

std::string display_value(const value& data)
{
    if (const auto* b = std::get_if<bool>(&data)) return *b ? "true" : "false";
    if (const auto* s = std::get_if<std::string>(&data)) return *s;
    char buffer[64];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), std::get<double>(data));
    return std::string(buffer, result.ptr);
}

const value& property_value(const node& item, const property_descriptor& desc)
{
    const auto found = item.props.find(desc.key);
    return found == item.props.end() ? desc.default_value : found->second;
}

bool is_container(const node& item)
{
    const auto* type = find_type(item.type);
    return type && type->container;
}

vgui_widget* make_row(vgui_widget* parent)
{
    auto* row = vgui_widget_add(parent, VGUI_PANEL, nullptr);
    vgui_widget_set_layout(row, VGUI_LAYOUT_ROW);
    vgui_widget_set_align_self(row, VGUI_ALIGN_STRETCH);
    vgui_widget_set_gap(row, 4);
    return row;
}

struct localized_text {
    std::string key;
    const char* fallback;
};

using status_value = std::variant<std::string, localized_text>;
struct formatted_status {
    localized_text pattern;
    status_value value;
};
using status_message = std::variant<std::string, localized_text, formatted_status>;

// Status templates can place their (untranslated or localized) argument
// anywhere in the sentence, including next to ideographic punctuation.
std::string status_with_value(std::string pattern, std::string_view value)
{
    constexpr std::string_view token = "{value}";
    if (const auto at = pattern.find(token); at != std::string::npos)
        pattern.replace(at, token.size(), value);
    return pattern;
}

void update_text(vgui_widget* widget, const std::string& text)
{
    if (text != vgui_widget_get_text(widget)) vgui_widget_set_text(widget, text.c_str());
}
} // namespace

struct designer_workbench::impl {
    struct outline_row {
        kit::object_id object = kit::invalid_object;
        vgui_item_id item = 0;
        std::string parent;
        std::size_t index = 0;
    };
    struct inspector_state {
        std::string id, type;
        std::map<std::string, std::string, std::less<>> values;
        bool operator==(const inspector_state&) const = default;
    };

    vgui_context* ui;
    session& history;
    const designer_strings& strings;
    kit::editor_context editor;
    kit::command_registry commands;
    std::optional<kit::outliner_view> outliner{std::in_place};
    std::optional<kit::property_grid> grid{std::in_place};
    std::vector<vgui_widget*> buttons;
    struct menu_binding {
        vgui_widget* menu;
        vgui_item_id item;
        std::string command;
        localized_text label;
        bool page_only = false; // For submenu entries, which have no command predicate.
    };
    // The native items keep stable pointers, even when the registry grows.
    std::vector<std::unique_ptr<menu_binding>> menu_commands;
    std::vector<std::pair<vgui_item_id, localized_text>> menu_headings;
    std::vector<menu_binding> submenu_labels;
    using label_setter = void (*)(vgui_widget*, const char*);
    struct widget_label {
        vgui_widget* widget;
        localized_text text;
        label_setter set;
    };
    std::vector<widget_label> widget_labels;
    std::vector<std::pair<vgui_item_id, localized_text>> dock_labels;
    std::map<std::string, outline_row, std::less<>> outline;
    std::map<kit::object_id, std::string> node_ids;
    kit::object_id next_object = 1;
    std::uint64_t next_generated_id = 1;
    std::uint64_t displayed_revision = 0;
    std::string selected, outlined_selection, move_source;
    std::optional<std::string> title;
    status_message message = localized_text{"workbench.status.ready", "Ready"};
    std::optional<std::pair<const char*, localized_text>> pending_help;
    inspector_state inspected;
    struct live_field {
        vgui_widget* widget;
        std::string key;
        std::uint64_t revision;
    };
    std::optional<live_field> live;
    int focus_listener = 0;
    int key_handler = 0;
    std::map<std::string, std::string, std::less<>> field_errors;
    bool replaced = true;
    bool sync_inspector = true;
    bool sync_inspector_language = false;
    bool refreshing = false;
    bool invoking_command = false;
    bool page_active = true;
    vgui_widget *shell = nullptr, *dock = nullptr;
    vgui_widget *preview_section = nullptr, *preview = nullptr, *page_tabs = nullptr;
    vgui_widget *title_label = nullptr, *document_chip = nullptr, *document_marker = nullptr;
    vgui_widget *status_label = nullptr, *pickup = nullptr, *outline_actions = nullptr;
    float outline_actions_min_width = 0;
    vgui_widget *menubar = nullptr, *quick_actions = nullptr;
    std::string displayed_title;
    int title_context_width = -1;
    float title_scale = 0;
    std::optional<bool> title_dirty;
    std::optional<designer_language> title_language;

    impl(vgui_context* context, session& model, designer_commands file, const designer_strings& locale)
        : ui(context), history(model), strings(locale), editor(context), selected(model.current().root)
    {
        auto* root = vgui_root(ui);
        vgui_widget_set_layout(root, VGUI_LAYOUT_COLUMN);
        shell = vgui_widget_add(root, VGUI_PANEL, nullptr);
        vgui_widget_set_id(shell, "designer.workbench");
        vgui_widget_set_grow(shell, 1);
        vgui_widget_set_align_self(shell, VGUI_ALIGN_STRETCH);
        vgui_widget_set_layout(shell, VGUI_LAYOUT_COLUMN);
        vgui_widget_set_padding(shell, 0, 0, 0, 0);
        vgui_widget_set_gap(shell, 4);

        build_toolbar(std::move(file));
        key_handler = vgui_context_add_key_handler(ui, [](vgui_context*, int key, int down, int mods, void* user) {
            return down && static_cast<impl*>(user)->handle_key(key, mods) ? 1 : 0;
        }, this);

        dock = vgui_widget_add(shell, VGUI_DOCK, nullptr);
        vgui_widget_set_id(dock, "designer.dock");
        vgui_widget_set_grow(dock, 1);
        vgui_widget_set_align_self(dock, VGUI_ALIGN_STRETCH);
        set_dock_widths({});
        vgui_dock_set_side_resize(dock, 150, 260, 220);

        auto* structure = page("structure", {"workbench.dock.structure", "Structure"}, VGUI_DOCK_LEFT);
        build_structure(structure);
        preview_section = page("preview", {"workbench.dock.preview", "Preview"}, VGUI_DOCK_CENTER);
        preview = vgui_widget_add(preview_section, VGUI_PANEL, nullptr);
        vgui_widget_set_id(preview, "designer.preview.page");
        vgui_widget_set_layout(preview, VGUI_LAYOUT_COLUMN);
        vgui_widget_set_align_items(preview, VGUI_ALIGN_STRETCH);
        vgui_widget_set_grow(preview, 1);
        vgui_widget_set_padding(preview, 0, 0, 0, 0);
        vgui_widget_set_background(preview, 0);
        vgui_widget_set_border(preview, 0, 0);
        auto* properties = page("inspector", {"workbench.dock.inspector", "Inspector"}, VGUI_DOCK_RIGHT);
        grid->build_with_labels(properties, inspector_labels());
        grid->set_change_listener([this](const std::string& key, const std::string& input) {
            finish_property_edit();
            edit_property(key, input);
        });
        grid->set_text_edit_listener([this](vgui_widget* widget, const std::string& key, const std::string& input,
            kit::property_grid::text_edit_event event) {
            return on_property_text(widget, key, input, event);
        });
        focus_listener = vgui_context_add_focus_listener(ui, [](vgui_context*, vgui_widget* old, vgui_widget*, void* user) {
            auto& self = *static_cast<impl*>(user);
            if (self.live && self.live->widget == old) self.finish_property_edit();
        }, this);

        status_label = vgui_widget_add(shell, VGUI_LABEL, nullptr);
        vgui_widget_set_id(status_label, "designer.status");
        vgui_widget_set_margin(status_label, 6, 0, 6, 4);
        refresh();
    }

    ~impl()
    {
        refreshing = true; // Tearing down a focused field must not commit it.
        vgui_context_remove_key_handler(ui, key_handler);
        vgui_context_remove_focus_listener(ui, focus_listener);
        finish_property_edit(false);
        auto* tree = outliner->widget();
        vgui_tree_set_row_drag(tree, nullptr, nullptr);
        vgui_widget_set_drop_target(tree, nullptr);
        outliner.reset();
        grid.reset();
        vgui_widget_destroy(shell);
    }

    const char* translate(const localized_text& text) const
    {
        return strings.text(text.key, text.fallback);
    }

    void bind_label(vgui_widget* widget, localized_text text, label_setter set = vgui_widget_set_text)
    {
        set(widget, translate(text));
        widget_labels.push_back({widget, std::move(text), set});
    }

    kit::command_registry::predicate with_active_page(kit::command_registry::predicate enabled = {})
    {
        return [this, enabled = std::move(enabled)] { return page_active && (!enabled || enabled()); };
    }

    void define_command(const std::string& id, const char* title, kit::command_registry::action run,
        kit::command_registry::predicate enabled = {}, const char* shortcut = nullptr,
        kit::command_registry::predicate checked = {})
    {
        if (!run) enabled = [] { return false; };
        auto& command = commands.add(id, title, std::move(run));
        command.enabled = std::move(enabled);
        command.shortcut = kit::command_shortcut::parse(shortcut);
        command.checked = std::move(checked);
    }

    void invoke_command(const char* id)
    {
        if (refreshing || invoking_command) return;
        invoking_command = true;
        finish_property_edit();
        commands.invoke(id);
        invoking_command = false;
        refresh_command_states();
    }

    bool handle_key(int key, int mods)
    {
        // Accept the platform Command modifier as an alternative to Control.
        if ((mods & (VGUI_MOD_CTRL | VGUI_MOD_GUI)) == VGUI_MOD_GUI)
            mods = (mods & ~VGUI_MOD_GUI) | VGUI_MOD_CTRL;
        for (const auto& command : commands.entries()) {
            if (!command.shortcut.matches(key, mods)) continue;
            // Core handles Ctrl+Z first. If Command+Z (or a lower-case key)
            // reaches us with an edit focused, keep undo/redo local to it.
            auto* focus = vgui_get_focus(ui);
            if (focus && vgui_widget_get_kind(focus) == VGUI_EDIT) {
                if (command.id == "designer.undo") { vgui_edit_undo(focus); return true; }
                if (command.id == "designer.redo") { vgui_edit_redo(focus); return true; }
            }
            invoke_command(command.id.c_str());
            return true;
        }
        return false;
    }

    static std::string shortcut_text(const kit::command_shortcut& shortcut)
    {
        auto label = shortcut.to_string();
#ifdef __APPLE__
        if (label.starts_with("Ctrl+")) label.replace(0, 4, "Cmd");
#endif
        return label;
    }

    void refresh_command_states()
    {
        for (auto* button : buttons) {
            const bool enabled = commands.is_enabled(vgui_widget_get_id(button));
            if ((vgui_widget_get_enabled(button) != 0) != enabled) vgui_widget_set_enabled(button, enabled);
        }
        for (const auto& binding : menu_commands) {
            const auto id = binding->command.c_str();
            const bool enabled = commands.is_enabled(id), checked = commands.is_checked(id);
            if ((vgui_menu_get_item_enabled(binding->menu, binding->item) != 0) != enabled)
                vgui_menu_set_item_enabled(binding->menu, binding->item, enabled);
            if ((vgui_menu_get_item_checked(binding->menu, binding->item) != 0) != checked)
                vgui_menu_set_item_checked(binding->menu, binding->item, checked);
        }
        for (const auto& binding : submenu_labels) {
            if (binding.page_only && (vgui_menu_get_item_enabled(binding.menu, binding.item) != 0) != page_active)
                vgui_menu_set_item_enabled(binding.menu, binding.item, page_active);
        }
    }

    void wire_menu(vgui_widget* menu)
    {
        vgui_menu_on_activate(menu, [](vgui_widget* menu, vgui_item_id item, void* user) {
            const auto* binding = static_cast<const menu_binding*>(vgui_menu_get_item_user(menu, item));
            if (binding) static_cast<impl*>(user)->invoke_command(binding->command.c_str());
        }, this);
        vgui_menu_on_open(menu, [](vgui_widget*, void* user) {
            static_cast<impl*>(user)->refresh_command_states();
        }, this);
    }

    vgui_widget* add_menu(localized_text label)
    {
        const auto item = vgui_menubar_add_item(menubar, translate(label));
        menu_headings.emplace_back(item, std::move(label));
        auto* menu = vgui_menubar_get_menu(menubar, item);
        wire_menu(menu);
        return menu;
    }

    vgui_widget* add_submenu(vgui_widget* menu, localized_text label, bool page_only = false)
    {
        vgui_widget* submenu = nullptr;
        const auto item = vgui_menu_add_submenu(menu, translate(label), &submenu);
        submenu_labels.push_back({menu, item, {}, std::move(label), page_only});
        wire_menu(submenu);
        return submenu;
    }

    void add_menu_command(vgui_widget* menu, std::string id, localized_text label,
        kit::command_registry::action run, kit::command_registry::predicate enabled = {},
        const char* shortcut = nullptr, kit::command_registry::predicate checked = {})
    {
        const auto* title = translate(label);
        const auto item = checked ? vgui_menu_add_check_item(menu, title, checked()) : vgui_menu_add_item(menu, title);
        define_command(id, title, std::move(run), std::move(enabled), shortcut, std::move(checked));
        if (shortcut) vgui_menu_set_item_shortcut(menu, item, shortcut_text(commands.find(id.c_str())->shortcut).c_str());
        auto binding = std::make_unique<menu_binding>(menu_binding{menu, item, std::move(id), std::move(label)});
        vgui_menu_set_item_user(menu, item, binding.get());
        menu_commands.push_back(std::move(binding));
    }

    void show_help(const char* id, localized_text explanation)
    {
        std::string message = translate(explanation);
        if (std::string_view(id) == "designer.help.shortcuts") {
            for (const auto& command : commands.entries()) {
                if (command.shortcut.valid()) message += "\n" + shortcut_text(command.shortcut) + " — " + command.title;
            }
        }
        vgui_dialog_desc desc;
        vgui_dialog_desc_default(&desc);
        desc.title = commands.find(id)->title.c_str();
        desc.message = message.c_str();
        desc.confirm_text = strings.text("common.ok", "OK");
        vgui_dialog_show(ui, &desc);
    }

    void build_menus(designer_commands callbacks)
    {
        auto* file = add_menu({"workbench.menu.file", "File"});
        add_menu_command(file, "designer.file.new", {"workbench.menu.new_page", "New Page"}, std::move(callbacks.new_document), {}, "Ctrl+N");
        add_menu_command(file, "designer.file.open", {"workbench.menu.open_page", "Open Page"}, std::move(callbacks.open_document), {}, "Ctrl+O");
        add_menu_command(file, "designer.file.save", {"workbench.toolbar.save", "Save"}, std::move(callbacks.save_document),
            with_active_page(), "Ctrl+S");
        add_menu_command(file, "designer.file.save_as", {"workbench.toolbar.save_as", "Save As"}, std::move(callbacks.save_as),
            with_active_page(), "Ctrl+Shift+S");
        if (callbacks.save_all)
            add_menu_command(file, "designer.file.save_all", {"workbench.menu.save_all", "Save All"},
                std::move(callbacks.save_all), std::move(callbacks.can_save_all));
        vgui_menu_add_separator(file);
        add_menu_command(file, "designer.file.export", {"workbench.toolbar.export", "Export Package"}, std::move(callbacks.export_package),
            with_active_page());
        vgui_menu_add_separator(file);
        add_menu_command(file, "designer.file.exit", {"workbench.menu.exit", "Exit"}, std::move(callbacks.quit));
        if (callbacks.close_document) {
            vgui_menu_add_separator(file);
            add_menu_command(file, "designer.file.close", {"workbench.menu.close_page", "Close Page"},
                std::move(callbacks.close_document), with_active_page());
        }

        auto* edit = add_menu({"workbench.menu.edit", "Edit"});
        add_menu_command(edit, "designer.undo", {"workbench.toolbar.undo", "Undo"}, [this] { replay(false); },
            with_active_page([this] { return history.can_undo(); }), "Ctrl+Z");
        add_menu_command(edit, "designer.redo", {"workbench.toolbar.redo", "Redo"}, [this] { replay(true); },
            with_active_page([this] { return history.can_redo(); }), "Ctrl+Shift+Z");
        vgui_menu_add_separator(edit);
        add_menu_command(edit, "designer.duplicate", {"workbench.outline.copy", "Copy"}, [this] { duplicate(); },
            with_active_page([this] { return can_duplicate(); }));
        add_menu_command(edit, "designer.delete", {"workbench.outline.delete", "Delete"}, [this] { erase(); },
            with_active_page([this] { return location(selected).has_value(); }));
        add_menu_command(edit, "designer.move.up", {"workbench.menu.move_up", "Move Up"}, [this] { reorder(-1); },
            with_active_page([this] { return can_reorder(-1); }));
        add_menu_command(edit, "designer.move.down", {"workbench.menu.move_down", "Move Down"}, [this] { reorder(1); },
            with_active_page([this] { return can_reorder(1); }));

        auto* resources = add_menu({"workbench.menu.resources", "Resources"});
        auto* new_control = add_submenu(resources, {"workbench.menu.new_control", "New Control"}, true);
        for (const auto& type : types()) {
            const std::string key(type.key);
            add_menu_command(new_control, "designer.add." + key, type_label(type), [this, key] { add_node(key); },
                with_active_page([this] { return history.current().nodes.size() < max_nodes; }));
        }

        auto* view = add_menu({"workbench.menu.view", "View"});
        using action = designer_view_action;
        const auto add_view = [&](const char* id, localized_text label, action value, bool checkable = true) {
            kit::command_registry::predicate checked;
            if (checkable) checked = with_active_page([check = callbacks.view_checked, value] { return check && check(value); });
            add_menu_command(view, id, std::move(label), [run = callbacks.view, value] { run(value); },
                with_active_page([available = bool(callbacks.view)] { return available; }), nullptr, std::move(checked));
        };
        add_view("designer.view.design", {"preview.mode.design", "Design"}, action::design);
        add_view("designer.view.run", {"preview.mode.run", "Run"}, action::run);
        add_view("designer.view.desktop", {"preview.desktop", "Desktop"}, action::desktop);
        add_view("designer.view.tablet", {"preview.tablet", "Tablet"}, action::tablet);
        add_view("designer.view.phone", {"preview.phone", "Phone"}, action::phone);
        add_view("designer.view.fit", {"preview.fit", "Fit"}, action::fit, false);
        add_view("designer.view.actual", {"workbench.menu.actual_size", "Actual Size"}, action::actual, false);
        vgui_menu_add_separator(view);
        add_menu_command(view, "designer.layout.restore_default",
            {"workbench.menu.restore_default_layout", "Restore Default Layout"},
            [this, changed = std::move(callbacks.layout_changed)] {
                set_dock_widths({});
                message = localized_text{"workbench.status.default_layout_restored", "Default layout restored."};
                if (changed) changed();
            });

        auto* tools = add_menu({"workbench.menu.tools", "Tools"});
        auto* language = add_submenu(tools, {"settings.language", "Language"});
        const auto add_language = [&](const char* id, localized_text label, designer_language value) {
            add_menu_command(language, id, std::move(label), [set = callbacks.set_language, value] { set(value); },
                [available = bool(callbacks.set_language)] { return available; }, nullptr,
                [this, value] { return strings.language() == value; });
        };
        add_language("designer.language.zh-CN", {"settings.zh-CN", "简体中文"}, designer_language::simplified_chinese);
        add_language("designer.language.en-US", {"settings.en-US", "English"}, designer_language::english);

        auto* help = add_menu({"workbench.menu.help", "Help"});
        const auto add_help = [&](const char* id, localized_text label, localized_text explanation) {
            add_menu_command(help, id, std::move(label), [this, id, explanation = std::move(explanation)] {
                // Activation returns to core before it closes the popup chain.
                pending_help.emplace(id, explanation);
            });
        };
        add_help("designer.help.instructions", {"workbench.menu.instructions", "Instructions"},
            {"workbench.help.instructions", "Add controls from Resources > New Control. A selected container receives a child; a selected leaf receives a sibling.\n\n"
                "Select a control in the Outline and edit its properties in the Inspector. Use View > Run to try the page. "
                "Save writes the design; Export Package creates a separate folder without saving the design."});
        add_help("designer.help.shortcuts", {"workbench.menu.shortcuts", "Shortcuts"},
            {"workbench.help.shortcuts", "F10 opens the menu bar. Use the arrow keys and Enter to navigate, or Esc to close it. "
                "While a text field is focused, undo and redo apply to that field first.\n"});
        add_help("designer.help.about", {"workbench.menu.about", "About"},
            {"workbench.help.about", "VGUI UI Designer is the VGUI sample editor for .vgui.json design files. "
                "Run mode previews the active page without changing its design."});
    }

    void build_toolbar(designer_commands callbacks)
    {
        auto* bar = make_row(shell);
        vgui_widget_set_id(bar, "designer.toolbar");
        vgui_widget_set_size(bar, 0, 48);
        vgui_widget_set_padding(bar, 0, 0, toolbar_padding_right, 0);
        vgui_widget_set_gap(bar, toolbar_gap);
        vgui_widget_set_align_items(bar, VGUI_ALIGN_CENTER);
        vgui_widget_set_background(bar, toolbar_bg);
        vgui_widget_set_border(bar, 0, 0);
        vgui_widget_set_rounding(bar, 0);

        menubar = vgui_widget_add(bar, VGUI_MENUBAR, nullptr);
        vgui_widget_set_id(menubar, "designer.menubar");
        vgui_widget_set_background(menubar, 0);
        vgui_widget_set_border(menubar, 0, 0);
        vgui_widget_set_text_color(menubar, toolbar_text);
        build_menus(std::move(callbacks));

        const auto group = [&](const char* id, vgui_color background = 0) {
            auto* row = make_row(bar);
            vgui_widget_set_id(row, id);
            vgui_widget_set_align_self(row, VGUI_ALIGN_CENTER);
            vgui_widget_set_align_items(row, VGUI_ALIGN_CENTER);
            vgui_widget_set_padding(row, 3, 0, 3, 0);
            vgui_widget_set_background(row, background);
            vgui_widget_set_border(row, 0, 0);
            vgui_widget_set_rounding(row, 7);
            return row;
        };
        auto* document = document_chip = group("designer.toolbar.document", toolbar_inset);
        vgui_widget_set_padding(document, 10, 0, 8, 0);
        vgui_widget_set_gap(document, 8);
        vgui_widget_set_size(document, 200, 33);
        document_marker = vgui_widget_add(document, VGUI_PANEL, nullptr);
        vgui_widget_set_id(document_marker, "designer.document.state");
        vgui_widget_set_size(document_marker, 7, 7);
        vgui_widget_set_padding(document_marker, 0, 0, 0, 0);
        vgui_widget_set_border(document_marker, 0, 0);
        vgui_widget_set_rounding(document_marker, 3.5f);
        title_label = vgui_widget_add(document, VGUI_LABEL, nullptr);
        vgui_widget_set_id(title_label, "designer.title");
        vgui_widget_set_size(title_label, 200 - document_chrome_width, 30);
        vgui_widget_set_padding(title_label, 0, 0, 0, 0);
        vgui_widget_set_font_size(title_label, 14);
        vgui_widget_set_font_weight(title_label, VGUI_FONT_WEIGHT_SEMI_BOLD);
        vgui_widget_set_text_color(title_label, toolbar_text);

        const auto style_action = [](vgui_widget* button, vgui_color background, vgui_color edge = 0) {
            vgui_widget_set_size(button, 0, 30);
            vgui_widget_set_padding(button, 9, 0, 9, 0);
            vgui_widget_set_font_size(button, 13);
            vgui_widget_set_text_align(button, .5f, .5f);
            vgui_widget_set_text_color(button, toolbar_text);
            vgui_widget_set_background(button, background);
            vgui_widget_set_border(button, edge, edge ? 1 : 0);
            vgui_widget_set_rounding(button, 5);
            return button;
        };
        auto* spacer = vgui_widget_add(bar, VGUI_PANEL, nullptr);
        vgui_widget_set_grow(spacer, 1);
        vgui_widget_set_padding(spacer, 0, 0, 0, 0);
        vgui_widget_set_background(spacer, 0);
        vgui_widget_set_border(spacer, 0, 0);
        auto* actions = quick_actions = group("designer.toolbar.actions");
        vgui_widget_set_gap(actions, 8);
        style_action(add_button(actions, "designer.file.save", {"workbench.toolbar.save", "Save"}), toolbar_bg, toolbar_edge);
        auto* export_button = style_action(add_button(actions, "designer.file.export", {"workbench.toolbar.export", "Export Package"}), toolbar_primary);
        vgui_widget_set_padding(export_button, 13, 0, 13, 0);
        vgui_widget_set_text_color(export_button, 0xFFFFFFFF);
        vgui_widget_set_font_weight(export_button, VGUI_FONT_WEIGHT_SEMI_BOLD);
        bind_label(export_button, {"workbench.toolbar.export_hint", "Export the current design and resources to a new folder (does not save the design)."}, vgui_widget_set_tooltip);
    }

    void refresh_document_caption()
    {
        const bool use_chip = page_active && !page_tabs;
        const auto dirty = use_chip && history.dirty();
        const auto scale = vgui_context_get_ui_scale(ui);
        const auto language = strings.language();
        const std::string_view filename = !use_chip ? std::string_view{} :
            title ? std::string_view(*title) : strings.text("workbench.title.untitled", "Untitled");
        int context_width = 0;
        vgui_context_get_size(ui, &context_width, nullptr);
        if (filename == displayed_title && context_width == title_context_width && scale == title_scale &&
            dirty == title_dirty && language == title_language) return;
        displayed_title = filename;
        title_context_width = context_width;
        title_scale = scale;
        title_dirty = dirty;
        title_language = language;

        // Reserve the natural menu width first. Quick actions yield before
        // the document chip; the six menus do not compete with either.
        const float chrome = (toolbar_padding_right + (use_chip ? 2 : 1) * toolbar_gap) * scale;
        float menu_width = 0, actions_width = 0;
        vgui_widget_get_intrinsic_size(menubar, &menu_width, nullptr);
        vgui_widget_get_intrinsic_size(quick_actions, &actions_width, nullptr);
        float available = context_width - menu_width - chrome;
        constexpr float min_chip_width = 64.f;
        const float quick_width = actions_width + toolbar_gap * scale;
        const bool show_actions = context_width <= 0 || available >= quick_width + (use_chip ? min_chip_width * scale : 0);
        if ((vgui_widget_get_visible(quick_actions) != 0) != show_actions)
            vgui_widget_set_visible(quick_actions, show_actions);
        if (show_actions) available -= quick_width;
        const bool show_document = use_chip && (context_width <= 0 || available >= min_chip_width * scale);
        if ((vgui_widget_get_visible(document_chip) != 0) != show_document)
            vgui_widget_set_visible(document_chip, show_document);
        if (!use_chip) return;
        const float chip_width = context_width > 0 ? std::clamp(available / scale, min_chip_width, 200.f) : 200.f;
        const float label_width = chip_width - document_chrome_width;
        vgui_widget_set_size(document_chip, chip_width, 33);
        vgui_widget_set_size(title_label, label_width, 30);
        std::string caption(filename);
        const float width = label_width * scale;
        if (!caption.empty() && vgui_widget_measure_text(title_label, caption.c_str()) > width) {
            // Trim only on UTF-8 character boundaries. The full name stays in the tooltip.
            auto end = caption.size();
            do {
                do { --end; } while (end && (static_cast<unsigned char>(filename[end]) & 0xC0) == 0x80);
                caption.assign(filename.substr(0, end));
                caption += "…";
            } while (end && vgui_widget_measure_text(title_label, caption.c_str()) > width);
        }
        update_text(title_label, caption);
        const auto* unsaved = strings.text("file.unsaved", "Unsaved changes");
        const auto description = dirty ? displayed_title + " — " + unsaved : displayed_title;
        vgui_widget_set_tooltip(title_label, description.c_str());
        vgui_widget_set_a11y_name(title_label, description.c_str());
        vgui_widget_set_background(document_marker, dirty ? document_dirty : document_ready);
        vgui_widget_set_tooltip(document_marker, dirty ? unsaved : nullptr);
    }

    designer_dock_widths dock_widths() const
    {
        const float scale = vgui_context_get_ui_scale(ui);
        return {vgui_dock_get_zone_size(dock, VGUI_DOCK_LEFT) / scale,
                vgui_dock_get_zone_size(dock, VGUI_DOCK_RIGHT) / scale};
    }

    void set_dock_widths(designer_dock_widths widths)
    {
        vgui_dock_set_zone_size(dock, VGUI_DOCK_LEFT, widths.left);
        vgui_dock_set_zone_size(dock, VGUI_DOCK_RIGHT, widths.right);
    }

    vgui_widget* enable_page_tabs()
    {
        if (page_tabs) return page_tabs;
        vgui_widget_set_visible(document_chip, 0);
        title_context_width = -1;
        vgui_widget_set_padding(preview_section, 0, 0, 0, 0);
        vgui_widget_set_gap(preview_section, 0);
        vgui_widget_set_rounding(preview_section, 0);

        // Insert the strip ahead of the stable host, even if the preview is already mounted.
        vgui_widget_remove(preview);
        page_tabs = vgui_widget_add(preview_section, VGUI_TABVIEW, nullptr);
        vgui_widget_set_id(page_tabs, "designer.page_tabs");
        vgui_widget_set_align_self(page_tabs, VGUI_ALIGN_STRETCH);
        vgui_widget_set_padding(page_tabs, 0, 0, 0, 0);
        vgui_widget_set_rounding(page_tabs, 0);
        vgui_widget_set_border(page_tabs, 0, 0);
        vgui_widget_set_size(page_tabs, 0, page_tab_height);
        vgui_tab_set_tab_height(page_tabs, page_tab_height);
        vgui_widget_attach(preview_section, preview);
        refresh_document_caption();
        return page_tabs;
    }

    kit::property_grid::ui_labels inspector_labels() const
    {
        return {
            .properties = strings.text("workbench.inspector.properties_a11y", "Properties"),
            .search = strings.text("workbench.inspector.search", "Search properties"),
            .favorites_only = strings.text("workbench.inspector.favorites_only", "Favorites only"),
            .toggle_favorite = strings.text("workbench.inspector.toggle_favorite", "Toggle favorite"),
            .toggle_favorite_hint = strings.text("workbench.inspector.toggle_favorite_hint", "Toggle favorite (common filter)"),
            .reset_button = strings.text("workbench.inspector.reset_button", "R"),
            .reset_to_default = strings.text("workbench.inspector.reset_to_default", "Reset to default"),
            .categories = {
                {"General", strings.text("workbench.inspector.category.general", "General")},
                {"Container", strings.text("workbench.inspector.category.container", "Container")},
                {"Content", strings.text("workbench.inspector.category.content", "Content")},
                {"Geometry & state", strings.text("workbench.inspector.category.geometry_state", "Geometry & state")},
            },
        };
    }

    vgui_widget* page(const char* id, localized_text name, vgui_dock_zone zone)
    {
        auto* panel = vgui_widget_create(ui, VGUI_PANEL, nullptr);
        vgui_widget_set_layout(panel, VGUI_LAYOUT_COLUMN);
        vgui_widget_set_align_items(panel, VGUI_ALIGN_STRETCH);
        vgui_dock_panel_desc desc;
        vgui_dock_panel_desc_default(&desc);
        desc.id = id;
        desc.title = translate(name);
        desc.panel = panel;
        desc.zone = zone;
        desc.closeable = 0;
        dock_labels.emplace_back(vgui_dock_add(dock, &desc), std::move(name));
        return panel;
    }

    vgui_widget* add_button(vgui_widget* row, const char* id, localized_text label, bool grow = false)
    {
        auto* button = make_button(row, id, translate(label), grow);
        widget_labels.push_back({button, std::move(label), vgui_widget_set_text});
        return button;
    }

    vgui_widget* make_button(vgui_widget* row, const char* id, const char* label, bool grow)
    {
        auto* button = vgui_widget_add(row, VGUI_BUTTON, label);
        vgui_widget_set_id(button, id);
        if (grow) vgui_widget_set_grow(button, 1);
        vgui_widget_on_event(button, [](vgui_widget* widget, const vgui_event* event, void* user) {
            if (event->type == VGUI_EVENT_CLICK)
                static_cast<impl*>(user)->invoke_command(vgui_widget_get_id(widget));
        }, this);
        buttons.push_back(button);
        return button;
    }

    const char* pickup_label() const
    {
        return move_source.empty() ? strings.text("workbench.outline.pick", "Pick Up")
            : strings.text("workbench.outline.cancel_move", "Cancel Move");
    }

    static localized_text moved_status() { return {"workbench.status.moved_node", "Moved node"}; }

    static localized_text type_label(const type_descriptor& type)
    {
        // The built-in schema descriptors refer to static string literals.
        return {"workbench.type." + std::string(type.key), type.label.data()};
    }

    static localized_text property_label(const property_descriptor& desc)
    {
        if (desc.key == "atlas")
            return {"workbench.inspector.atlas_path", "Button atlas path"};
        if (desc.type == value_type::resource_key)
            return {"workbench.inspector.image_path", "Image path"};
        return {"workbench.inspector.property." + std::string(desc.key), desc.label.data()};
    }

    std::string choice_label(const property_descriptor& desc, std::string_view value) const
    {
        // Keep the static, human-facing labels independent of serialized values.
        // Alignment and justification share the common positional terms.
        if (desc.key == "layout") {
            if (value == "absolute") return strings.text("workbench.inspector.layout.absolute", "Absolute");
            if (value == "row") return strings.text("workbench.inspector.layout.row", "Row");
            if (value == "column") return strings.text("workbench.inspector.layout.column", "Column");
        } else if (desc.key == "align_items" || desc.key == "justify") {
            if (value == "start") return strings.text("workbench.inspector.alignment.start", "Start");
            if (value == "center") return strings.text("workbench.inspector.alignment.center", "Center");
            if (value == "end") return strings.text("workbench.inspector.alignment.end", "End");
            if (value == "stretch") return strings.text("workbench.inspector.alignment.stretch", "Stretch");
            if (value == "space-between") return strings.text("workbench.inspector.alignment.space_between", "Space between");
            if (value == "space-around") return strings.text("workbench.inspector.alignment.space_around", "Space around");
        }
        return std::string(value);
    }

    void build_structure(vgui_widget* panel)
    {
        vgui_widget_set_padding(panel, 6, 5, 6, 5);
        vgui_widget_set_gap(panel, 4);
        bind_label(vgui_widget_add(panel, VGUI_LABEL, nullptr), {"workbench.outline.heading", "Outline"});
        outliner->build(editor, panel);
        auto* tree = outliner->widget();
        vgui_widget_set_size(tree, 0, 0); // Stretch to the dock width instead of the tree's fixed default width.
        vgui_widget_set_id(tree, "designer.outliner");
        bind_label(tree, {"workbench.outline.a11y", "Document outline"}, vgui_widget_set_a11y_name);
        outliner->set_selection_listener([this](kit::object_id object) {
            if (refreshing || replaced || !page_active) return;
            const auto found = node_ids.find(object);
            if (found != node_ids.end()) select(found->second);
        });
        build_drag_drop(tree);

        auto* actions_view = vgui_widget_add(panel, VGUI_PANEL, nullptr);
        vgui_widget_set_id(actions_view, "designer.outline.actions");
        vgui_widget_set_layout(actions_view, VGUI_LAYOUT_COLUMN);
        vgui_panel_set_scrollable(actions_view, 1, 0);
        vgui_panel_set_scroll_listener(actions_view, [](vgui_widget* panel, float, float, void*) {
            vgui_widget_mark_dirty(panel); // Refresh the on-demand frame after scrolling.
        }, nullptr);
        constexpr float bar_size = 8, bar_inset = 2;
        vgui_panel_set_scrollbar_size(actions_view, bar_size, 0, bar_inset);
        vgui_widget_set_padding(actions_view, 0, 0, 0, bar_size + bar_inset + 2);
        vgui_widget_set_background(actions_view, 0);
        vgui_widget_set_border(actions_view, 0, 0);
        outline_actions = vgui_widget_add(actions_view, VGUI_PANEL, nullptr);
        vgui_widget_set_layout(outline_actions, VGUI_LAYOUT_COLUMN);
        vgui_widget_set_align_self(outline_actions, VGUI_ALIGN_STRETCH);
        vgui_widget_set_padding(outline_actions, 0, 0, 0, 0);
        vgui_widget_set_gap(outline_actions, 4);
        vgui_widget_set_background(outline_actions, 0);
        vgui_widget_set_border(outline_actions, 0, 0);

        auto* row = make_row(outline_actions);
        add_button(row, "designer.move.up", {"workbench.outline.up", "Up"}, true);
        add_button(row, "designer.move.down", {"workbench.outline.down", "Down"}, true);
        add_button(row, "designer.duplicate", {"workbench.outline.copy", "Copy"}, true);
        add_button(row, "designer.delete", {"workbench.outline.delete", "Delete"}, true);
        row = make_row(outline_actions);
        define_command("designer.move.pick", pickup_label(), [this] {
            if (move_source.empty()) {
                move_source = selected;
                message = localized_text{"workbench.status.move_target", "Select a different container, then click Move Here."};
            } else {
                move_source.clear();
                message = localized_text{"workbench.status.move_cancelled", "Move cancelled"};
            }
        }, with_active_page([this] { return !move_source.empty() || location(selected).has_value(); }));
        pickup = make_button(row, "designer.move.pick", pickup_label(), true);
        bind_label(pickup, {"workbench.outline.pick_hint", "Pick up a node to move it to a different container"}, vgui_widget_set_tooltip);
        const localized_text move_here{"workbench.outline.move_here", "Move Here"};
        define_command("designer.move.here", translate(move_here), [this] {
            if (apply({move_node{move_source, selected}}, moved_status())) {
                select(std::exchange(move_source, {}));
            }
        }, with_active_page([this] {
            const auto place = location(move_source);
            return place && place->first != selected && can_move(move_source, selected);
        }));
        add_button(row, "designer.move.here", move_here, true);
    }

    bool apply(std::initializer_list<edit> edits, status_message success, std::string_view field = {}, bool interactive = false)
    {
        diagnostic error;
        const std::span<const edit> batch{edits.begin(), edits.size()};
        const bool ok = interactive ? history.apply_live(batch, &error) : history.apply(batch, &error);
        if (!ok) {
            message = error.path.empty() ? error.message : error.path + ": " + error.message;
            if (!field.empty()) {
                field_errors.insert_or_assign(std::string(field), std::move(error.message));
                sync_inspector = true;
            }
            return false;
        }
        message = std::move(success);
        return true;
    }

    void select(std::string id)
    {
        if (!page_active) return;
        const auto& doc = history.current();
        if (!doc.nodes.contains(id)) id = doc.root;
        if (selected == id) return;
        finish_property_edit();
        selected = std::move(id);
        field_errors.clear();
        sync_inspector = true;
    }

    void reset_document_transients()
    {
        live.reset(); // The outgoing edit must be finished before swapping the session.
        replaced = sync_inspector = true;
        move_source.clear();
        field_errors.clear();
        message = localized_text{"workbench.status.ready", "Ready"};

        // Clear both an active payload and a press that has not started dragging yet.
        auto* tree = outliner->widget();
        vgui_cancel_drag(ui);
        vgui_tree_set_row_drag(tree, nullptr, nullptr);
        build_drag_drop(tree);
        for (auto* focus = vgui_get_focus(ui); focus; focus = vgui_widget_get_parent(focus)) {
            if (focus != tree && focus != grid->widget()) continue;
            vgui_set_focus(ui, nullptr);
            break;
        }
    }

    void document_replaced(std::string selection)
    {
        reset_document_transients();
        const auto& doc = history.current();
        selected = !selection.empty() && doc.nodes.contains(selection) ? std::move(selection) : doc.root;
    }

    void set_page_active(bool active)
    {
        if (page_active == active) return;
        page_active = active;
        if (!active) {
            reset_document_transients();
            selected.clear();
            vgui_dock_end_drag(dock);
        }
        // The Dock manages visibility of its immediate pages. Close the side
        // panels rather than hiding their widgets; leave the strip in the center.
        for (const auto* id : {"structure", "inspector"}) {
            const auto panel = vgui_dock_find(dock, id);
            if (active) vgui_dock_restore(dock, panel);
            else vgui_dock_close(dock, panel);
        }
        vgui_widget_set_enabled(dock, active);
        vgui_widget_set_visible(preview, active);
        title_context_width = -1;
        refresh_command_states();
        refresh_captions();
    }

    void replay(bool redo)
    {
        if (!(redo ? history.redo() : history.undo())) return;
        sync_inspector = true;
        move_source.clear();
        field_errors.clear();
        select(selected);
        message = redo ? localized_text{"workbench.status.redone", "Redone"} : localized_text{"workbench.status.undone", "Undone"};
    }

    std::string fresh_key(std::string_view prefix, const auto& entries)
    {
        std::string key;
        do key = std::string(prefix) + std::to_string(next_generated_id++);
        while (entries.contains(key));
        return key;
    }

    // The shell can dispatch several input events between refreshes. When
    // the outline projection is stale, use the current model for commands.
    std::optional<std::pair<std::string, std::size_t>> location(std::string_view id) const
    {
        if (!replaced && displayed_revision == history.revision()) {
            const auto it = outline.find(id);
            if (it == outline.end() || it->second.parent.empty()) return std::nullopt;
            return {{it->second.parent, it->second.index}};
        }
        for (const auto& [parent, item] : history.current().nodes) {
            const auto it = std::find(item.children.begin(), item.children.end(), id);
            if (it != item.children.end()) return {{parent, std::size_t(it - item.children.begin())}};
        }
        return std::nullopt;
    }

    void add_node(const std::string& type)
    {
        const auto& doc = history.current();
        const auto target = doc.nodes.find(selected);
        if (target == doc.nodes.end()) return;
        node data;
        data.type = type;
        std::string parent = selected;
        if (!is_container(target->second)) {
            const auto place = location(selected);
            if (!place) return;
            parent = place->first;
        }
        auto id = fresh_key("node_", doc.nodes);
        const auto* descriptor = find_type(type);
        const auto [width, height] = initial_control_size(type);
        data.props.emplace("width", width);
        data.props.emplace("height", height);
        if (apply({insert_node{id, parent, std::move(data)}},
                formatted_status{{"workbench.status.added_type", "Added {value}"},
                    descriptor ? status_value{type_label(*descriptor)} : status_value{type}})) select(std::move(id));
    }

    void erase()
    {
        const auto place = location(selected);
        if (!place) return;
        if (apply({erase_node{selected}}, localized_text{"workbench.status.deleted_subtree", "Deleted subtree"})) select(place->first);
    }

    bool can_duplicate() const
    {
        const auto& doc = history.current();
        if (!location(selected) || doc.nodes.size() >= max_nodes) return false;
        auto remaining = max_nodes - doc.nodes.size();
        const auto fits = [&](const auto& visit, const std::string& id) -> bool {
            if (!remaining) return false;
            --remaining;
            for (const auto& child : doc.nodes.at(id).children)
                if (!visit(visit, child)) return false;
            return true;
        };
        return fits(fits, selected);
    }

    void duplicate()
    {
        const auto& doc = history.current();
        const auto place = location(selected);
        if (!place) return;
        copy_subtree copy{selected, place->first, {}, place->second + 1};
        const auto walk = [&](const auto& visit, const std::string& id) -> void {
            copy.ids.emplace(id, fresh_key("node_", doc.nodes));
            for (const auto& child : doc.nodes.at(id).children) visit(visit, child);
        };
        walk(walk, selected);
        const auto id = copy.ids.at(selected);
        if (apply({std::move(copy)}, localized_text{"workbench.status.copied_subtree", "Copied subtree"})) select(id);
    }

    bool can_reorder(int direction) const
    {
        const auto place = location(selected);
        return place && (direction < 0 ? place->second > 0
            : place->second + 1 < history.current().nodes.at(place->first).children.size());
    }

    void reorder(int direction)
    {
        if (!can_reorder(direction)) return;
        const auto place = location(selected);
        const auto index = direction < 0 ? place->second - 1 : place->second + 1;
        apply({move_node{selected, place->first, index}}, localized_text{"workbench.status.reordered_node", "Reordered node"});
    }

    bool can_move(std::string_view id, std::string_view parent) const
    {
        const auto& doc = history.current();
        const auto target = doc.nodes.find(parent);
        if (id == doc.root || !doc.nodes.contains(id) || target == doc.nodes.end() ||
            !is_container(target->second)) return false;
        for (auto it = outline.find(parent); it != outline.end(); it = outline.find(it->second.parent))
            if (it->first == id) return false;
        return true;
    }

    // The kit supplies row hit-testing and insertion hints. Gate those hints on
    // this document's only valid drop targets, and pass an after-removal index.
    std::optional<move_node> drop_move(const vgui_drag_payload* payload, float x, float y,
        kit::outliner_view::drop_location& location) const
    {
        if (payload->kind != VGUI_DRAG_CUSTOM || payload->data != this || replaced || !page_active ||
            history.revision() != displayed_revision) return std::nullopt;
        const auto from = node_ids.find(payload->id);
        if (from == node_ids.end()) return std::nullopt;
        location = outliner->drop_location_at(x, y);
        const auto to = node_ids.find(location.parent);
        if (to == node_ids.end() || !can_move(from->second, to->second)) return std::nullopt;
        const auto& source = outline.at(from->second);
        auto index = location.index;
        if (source.parent == to->second) {
            if (source.index < index) --index;
            if (source.index == index) return std::nullopt;
        }
        return move_node{from->second, to->second, index};
    }

    void build_drag_drop(vgui_widget* tree)
    {
        vgui_tree_set_row_drag(tree, [](void* user, vgui_item_id item, int, vgui_drag_payload* out) -> int {
            auto& self = *static_cast<impl*>(user);
            if (self.replaced || !self.page_active || self.history.revision() != self.displayed_revision) return 0;
            const auto object = static_cast<kit::object_id>(reinterpret_cast<std::uintptr_t>(
                vgui_tree_get_item_user(self.outliner->widget(), item)));
            const auto from = self.node_ids.find(object);
            if (from == self.node_ids.end() || from->second == self.history.current().root) return 0;
            out->kind = VGUI_DRAG_CUSTOM;
            out->id = object;
            out->data = user;
            return 1;
        }, this);
        const vgui_drop_target target{
            [](void* user, const vgui_drag_payload* payload) -> int {
                return payload->kind == VGUI_DRAG_CUSTOM && payload->data == user;
            },
            [](void* user, const vgui_drag_payload* payload, float x, float y, vgui_drop_hint* hint) {
                auto& self = *static_cast<impl*>(user);
                kit::outliner_view::drop_location location;
                if (self.drop_move(payload, x, y, location)) self.outliner->emit_hover_hint(location, hint);
            },
            [](void* user, const vgui_drag_payload* payload, float x, float y) -> int {
                auto& self = *static_cast<impl*>(user);
                kit::outliner_view::drop_location location;
                auto move = self.drop_move(payload, x, y, location);
                if (!move || !self.apply({*move}, self.moved_status())) return 0;
                self.select(move->id);
                self.move_source.clear();
                return 1;
            },
            this
        };
        vgui_widget_set_drop_target(tree, &target);
    }

    void finish_property_edit(bool commit = true)
    {
        if (!live) return;
        auto field = std::exchange(live, std::nullopt);
        if (field->revision == history.revision()) history.finish_live(commit);
        if (!commit) message = localized_text{"workbench.status.edit_cancelled", "Edit cancelled"};
        field_errors.erase(field->key);
        sync_inspector = true;
    }

    bool on_property_text(vgui_widget* widget, const std::string& key, const std::string& input,
        kit::property_grid::text_edit_event event)
    {
        using text_event = kit::property_grid::text_edit_event;
        if (refreshing || replaced || !page_active || inspected.id != selected) return true;
        if (event != text_event::change) {
            if (live && live->widget == widget) finish_property_edit(event == text_event::submit);
            return true;
        }
        if (live && (live->widget != widget || live->revision != history.revision())) finish_property_edit();
        if (!live) live = live_field{widget, key, history.revision()};
        edit_property(key, input, true);
        live->revision = history.revision();
        return true;
    }

    void edit_property(const std::string& key, const std::string& input, bool interactive = false)
    {
        if (refreshing || replaced || !page_active || inspected.id != selected) return;
        const auto& doc = interactive ? history.committed() : history.current();
        const auto found = doc.nodes.find(inspected.id);
        if (found == doc.nodes.end() || found->second.type != inspected.type) return;
        field_errors.erase(key);
        if (key == node_name) {
            apply({rename_node{inspected.id, input}}, localized_text{"workbench.status.renamed_node", "Renamed node"}, key, interactive);
            return;
        }
        const auto* desc = find_property(found->second.type, key);
        if (!desc) return;
        if (desc->type == value_type::resource_key && !input.empty()) {
            const auto& current_key = std::get<std::string>(property_value(found->second, *desc));
            const auto current = doc.resources.find(current_key);
            if (current != doc.resources.end() && current->second == input) {
                if (interactive) apply({}, localized_text{"workbench.status.image_path_unchanged", "Image path unchanged"}, key, true);
                return;
            }
            const auto resource = std::find_if(doc.resources.begin(), doc.resources.end(),
                [&](const auto& entry) { return entry.second == input; });
            if (resource == doc.resources.end()) {
                auto id = fresh_key("image_", doc.resources);
                apply({set_resource{id, input}, set_property{inspected.id, key, value{id}}},
                    formatted_status{{"workbench.status.registered_image", "Registered and assigned {value}"}, id}, key, interactive);
            } else {
                apply({set_property{inspected.id, key, value{resource->first}}}, localized_text{"workbench.status.assigned_image", "Assigned image resource"}, key, interactive);
            }
            return;
        }

        value data = input;
        if (desc->type == value_type::boolean) data = (input == "true");
        if (desc->type == value_type::number) {
            double number = 0;
            const char* end = input.data() + input.size();
            const auto parsed = std::from_chars(input.data(), end, number);
            if (parsed.ec != std::errc() || parsed.ptr != end) {
                message = localized_text{"workbench.status.invalid_number", "Invalid number"};
                field_errors[key] = translate(std::get<localized_text>(message));
                sync_inspector = true;
                return;
            }
            data = number;
        }
        auto stored = data == desc->default_value ? std::nullopt : std::optional<value>(std::move(data));
        apply({set_property{inspected.id, key, std::move(stored)}},
            formatted_status{{"workbench.status.updated_property", "Updated {value}"}, property_label(*desc)}, key, interactive);
    }

    std::string outline_label(const std::string& id, const node& item) const
    {
        if (!item.name.empty()) return item.name + " [" + id + "]";
        const auto* type = find_type(item.type);
        return std::string(type ? translate(type_label(*type)) : item.type.c_str()) + " [" + id + "]";
    }

    void refresh_outline_labels()
    {
        const auto& nodes = history.current().nodes;
        auto* tree = outliner->widget();
        for (const auto& [id, row] : outline) {
            const auto item = nodes.find(id);
            if (item == nodes.end()) continue;
            const auto label = outline_label(id, item->second);
            if (label != vgui_tree_get_item_text(tree, row.item)) vgui_tree_set_item_text(tree, row.item, label.c_str());
        }
    }

    void refresh_outline()
    {
        const auto& doc = history.current();
        bool rebuild = replaced || outline.size() != doc.nodes.size() || !outline.contains(doc.root);
        for (const auto& [parent, item] : doc.nodes) {
            if (rebuild) break;
            for (std::size_t i = 0; i < item.children.size(); ++i) {
                const auto old = outline.find(item.children[i]);
                if (old == outline.end() || old->second.parent != parent || old->second.index != i) {
                    rebuild = true;
                    break;
                }
            }
        }
        auto* tree = outliner->widget();
        if (!rebuild) {
            refresh_outline_labels();
            return;
        }

        auto old = std::exchange(outline, {});
        if (replaced) old.clear(); // New page, new objects even when node IDs match.
        std::vector<kit::object_id> expanded;
        for (const auto& [id, row] : old)
            if (vgui_tree_get_item_expanded(tree, row.item)) expanded.push_back(row.object);
        std::sort(expanded.begin(), expanded.end());
        outliner->clear();
        node_ids.clear();
        const auto visit = [&](const auto& walk, const std::string& id, vgui_item_id parent_item,
                               const std::string& parent, std::size_t index) -> void {
            const auto previous = old.find(id);
            const auto object = previous == old.end() ? next_object++ : previous->second.object;
            const auto& item = doc.nodes.at(id);
            const auto row = outliner->add_node(outline_label(id, item).c_str(), parent_item, object);
            outline.emplace(id, outline_row{object, row, parent, index});
            node_ids.emplace(object, id);
            for (std::size_t i = 0; i < item.children.size(); ++i) walk(walk, item.children[i], row, id, i);
            if ((previous == old.end() && id == doc.root) ||
                std::binary_search(expanded.begin(), expanded.end(), object)) outliner->expand(row);
        };
        visit(visit, doc.root, 0, {}, 0);
        outlined_selection.clear();
    }

    void refresh_inspector(bool force)
    {
        const auto& doc = history.current();
        const auto& item = doc.nodes.at(selected);
        inspector_state next{selected, item.type, {}};
        std::vector<kit::property_desc> rows;
        const auto add = [&](kit::property_desc row) {
            next.values.emplace(row.id, row.value);
            rows.push_back(std::move(row));
        };
        add({.id = node_id, .label = strings.text("workbench.inspector.node_id", "Node ID"), .value = selected, .read_only = true});
        add({.id = node_name, .label = strings.text("workbench.inspector.name", "Name"), .value = item.name, .has_default = true});
        const auto add_descriptors = [&](std::span<const property_descriptor> descriptors, const char* category) {
            for (const auto& desc : descriptors) {
                kit::property_desc row;
                row.id = desc.key;
                row.label = translate(property_label(desc));
                row.category = category;
                row.value = display_value(property_value(item, desc));
                row.default_value = display_value(desc.default_value);
                row.has_default = true;
                switch (desc.type) {
                case value_type::boolean:
                    row.type = kit::property_type::boolean;
                    break;
                case value_type::number:
                    row.type = kit::property_type::number;
                    row.has_range = true;
                    row.min_value = desc.min;
                    row.max_value = desc.max;
                    break;
                case value_type::choice:
                    row.type = kit::property_type::enumeration;
                    for (const auto choice : desc.choices) {
                        row.choices.push_back(choice_label(desc, choice));
                        row.choice_values.emplace_back(choice);
                    }
                    break;
                case value_type::resource_key: {
                    const auto key = row.value;
                    const auto path = doc.resources.find(key);
                    row.value = path == doc.resources.end() ? std::string() : path->second;
                    row.placeholder = strings.text("workbench.inspector.image_path_example", "images/example.png");
                    row.tooltip = desc.key == "atlas" ? strings.text("workbench.inspector.atlas_path_hint",
                        "Project-relative path to an even-sized 2×2 atlas: normal, hover, pressed, disabled. Clear to use the default button style.") :
                        strings.text("workbench.inspector.image_path_hint", "Project-relative path, using /. Clear to detach the image.");
                    add(std::move(row));
                    add({.id = resource_key, .label = strings.text("workbench.inspector.resource_key", "Resource key"), .value = key,
                        .category = category, .read_only = true});
                    continue;
                }
                default:
                    break; // Text, action and localization keys are edited as text.
                }
                add(std::move(row));
            }
        };
        if (const auto* type = find_type(item.type)) add_descriptors(type->properties, type->container ? "Container" : "Content");
        add_descriptors(common_properties(), "Geometry & state");

        if (replaced || inspected.id != next.id || inspected.type != next.type) grid->clear();
        if (sync_inspector_language) grid->set_ui_labels(inspector_labels());
        if (force || sync_inspector_language || inspected != next) grid->set_properties(std::move(rows));
        sync_inspector_language = false;
        for (const auto& [id, unused] : next.values) {
            const auto it = field_errors.find(id);
            const std::string error = it == field_errors.end() ? std::string{} : it->second;
            if (grid->property_error(id) != error) grid->set_property_error(id, error);
        }
        inspected = std::move(next);
    }

    std::string message_text() const
    {
        if (const auto* literal = std::get_if<std::string>(&message)) return *literal;
        if (const auto* label = std::get_if<localized_text>(&message)) return translate(*label);
        const auto& format = std::get<formatted_status>(message);
        const auto* literal = std::get_if<std::string>(&format.value);
        return status_with_value(translate(format.pattern),
            literal ? std::string_view(*literal) : translate(std::get<localized_text>(format.value)));
    }

    void refresh_captions()
    {
        const char* pick = pickup_label();
        if (std::string_view(pick) != vgui_widget_get_text(pickup)) {
            vgui_widget_set_text(pickup, pick);
            commands.find(vgui_widget_get_id(pickup))->title = pick;
        }
        // Size the scrollable content from the current captions (including
        // the alternate pickup label), not from the dock's viewport width.
        float width = 0;
        vgui_widget_get_intrinsic_size(outline_actions, &width, nullptr);
        const float min_width = width / vgui_context_get_ui_scale(ui);
        if (min_width != outline_actions_min_width) {
            outline_actions_min_width = min_width;
            vgui_widget_set_min_size(outline_actions, min_width, 0);
        }
        refresh_document_caption();
        update_text(status_label, message_text());
    }

    void refresh_language()
    {
        refreshing = true;
        for (const auto& label : widget_labels) label.set(label.widget, translate(label.text));
        for (const auto& [item, label] : menu_headings)
            vgui_menubar_set_item_text(menubar, item, translate(label));
        for (const auto& binding : submenu_labels)
            vgui_menu_set_item_text(binding.menu, binding.item, translate(binding.label));
        for (const auto& binding : menu_commands) {
            const char* title = translate(binding->label);
            commands.find(binding->command.c_str())->title = title;
            vgui_menu_set_item_text(binding->menu, binding->item, title);
        }
        refresh_command_states();
        for (const auto& [id, label] : dock_labels) vgui_dock_set_title(dock, id, translate(label));
        if (page_active && !replaced) refresh_outline_labels();
        sync_inspector_language = true;
        // Do not rebind a live text editor (or finish its undo transaction)
        // just because the language changed. Normal refresh catches up when
        // the edit ends or a pending document/selection change is projected.
        if (page_active && !live && !replaced && inspected.id == selected) {
            const auto item = history.current().nodes.find(selected);
            if (item != history.current().nodes.end() && item->second.type == inspected.type)
                refresh_inspector(false);
        }
        refresh_captions();
        refreshing = false;
    }

    void refresh_page()
    {
        if (live && live->revision != history.revision()) finish_property_edit();
        const bool changed = replaced || displayed_revision != history.revision();
        if (changed) {
            select(selected);
            if (!history.current().nodes.contains(move_source)) move_source.clear();
            refresh_outline();
            displayed_revision = history.revision();
        }
        const bool selection_changed = selected != outlined_selection;
        if (selection_changed) {
            auto* tree = outliner->widget();
            const auto item = outline.at(selected).item;
            if (!vgui_tree_is_selected(tree, item)) {
                vgui_tree_expand_to(tree, item);
                vgui_tree_clear_selection(tree);
                vgui_tree_select(tree, item, 1);
                vgui_tree_set_cursor(tree, item);
                vgui_tree_scroll_to_node(tree, item, VGUI_SCROLL_ALIGN_NEAREST);
            }
            outlined_selection = selected;
        }
        // Keep the active edit widget untouched: set_properties() rewrites its
        // text, which would move the caret and discard incomplete input/IME.
        if (!live && (changed || sync_inspector || sync_inspector_language || selection_changed)) {
            refresh_inspector(sync_inspector || replaced);
            sync_inspector = false;
        } else if (live) {
            const auto key = live->key;
            const auto it = field_errors.find(key);
            const std::string error = it == field_errors.end() ? std::string{} : it->second;
            if (grid->property_error(key) != error) grid->set_property_error(key, error);
        }
        replaced = false;
    }

    void refresh()
    {
        if (refreshing || invoking_command) return;
        refreshing = true;
        if (page_active) refresh_page();
        refresh_command_states();
        refresh_captions();
        refreshing = false;
        if (auto help = std::exchange(pending_help, std::nullopt))
            show_help(help->first, help->second);
    }
};

designer_workbench::designer_workbench(vgui_context* ui, session& history, designer_commands commands,
    const designer_strings& strings)
    : impl_(std::make_unique<impl>(ui, history, std::move(commands), strings)) {}

designer_workbench::~designer_workbench() = default;
vgui_widget* designer_workbench::preview_host() const { return impl_->preview; }
designer_dock_widths designer_workbench::dock_widths() const { return impl_->dock_widths(); }
void designer_workbench::set_dock_widths(designer_dock_widths widths) { impl_->set_dock_widths(widths); }
vgui_widget* designer_workbench::enable_page_tabs() { return impl_->enable_page_tabs(); }
void designer_workbench::set_page_active(bool active) { impl_->set_page_active(active); }
std::string_view designer_workbench::selection() const { return impl_->selected; }
void designer_workbench::select(std::string id) { impl_->select(std::move(id)); }
void designer_workbench::finish_edit() { impl_->finish_property_edit(); }
void designer_workbench::document_replaced(std::string selection) { impl_->document_replaced(std::move(selection)); }
void designer_workbench::refresh() { impl_->refresh(); }
void designer_workbench::refresh_language() { impl_->refresh_language(); }
void designer_workbench::status(std::string message) { impl_->message = std::move(message); }
void designer_workbench::document_title(std::string title) { impl_->title = std::move(title); }

} // namespace vgui::ui_design
