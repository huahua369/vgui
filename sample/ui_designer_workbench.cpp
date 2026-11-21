#include "ui_designer_workbench.h"

#include "button_gradient.h"
#include "primitive_schema.h"
#include "ui_designer_locale.h"
#include <vgui/editor/command_registry.h>
#include <vgui/editor/gradient_editor.h>
#include <vgui/editor/outliner_view.h>
#include <vgui/editor/property_grid.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
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
struct chrome_palette {
    vgui_color background, edge, text, primary_background, primary_text, move_active, edit_background;
};
constexpr chrome_palette dark_chrome{
    0xFF202733, 0xFF354151, 0xFFE5ECF6, 0xFF2F65CB, 0xFFFFFFFF, 0xFF88B9FF, 0xFF2A3443};
// The foreground must also contrast against the native light hover/press fills.
constexpr chrome_palette light_chrome{
    0xFFEAF0F7, 0xFFBCC8D8, 0xFF22364E, 0xFFAECDF6, 0xFF173E76, 0xFF1C60BE, 0xFFF5F8FC};
constexpr float toolbar_padding_right = 10;
constexpr float toolbar_gap = 10;

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

struct icon_stroke { float x0, y0, x1, y1; };
using outline_texture = std::unique_ptr<vgui_texture, decltype(&vgui_texture_destroy)>;
constexpr int outline_icon_size = 18;

// A small antialiased white mask keeps the icons independent of platform fonts.
outline_texture make_outline_icon(vgui_context* ui, std::initializer_list<icon_stroke> strokes)
{
    constexpr int size = 48;
    constexpr float density = size / 16.f;
    std::array<unsigned char, size * size * 4> pixels;
    pixels.fill(255);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            float coverage = 0;
            for (const auto& s : strokes) {
                const float dx = s.x1 - s.x0, dy = s.y1 - s.y0;
                const float px = (x + .5f) / density - s.x0, py = (y + .5f) / density - s.y0;
                const float t = std::clamp((px * dx + py * dy) / (dx * dx + dy * dy), 0.f, 1.f);
                const float distance = std::hypot(px - t * dx, py - t * dy);
                coverage = std::max(coverage, std::clamp((.72f - distance) * density + .5f, 0.f, 1.f));
            }
            pixels[4 * (y * size + x) + 3] = static_cast<unsigned char>(coverage * 255.f + .5f);
        }
    }
    const vgui_texture_desc desc{size, size, 0, VGUI_TEXTURE_FORMAT_RGBA8, pixels.data()};
    return {vgui_texture_create(ui, &desc), vgui_texture_destroy};
}

void describe_outline_button(vgui_widget* button, const char* label)
{
    vgui_widget_set_tooltip(button, label);
    vgui_widget_set_a11y_name(button, label);
}

struct localized_text {
    std::string key;
    const char* fallback;
};

struct dock_side {
    vgui_dock_zone zone;
    const char* command;
    localized_text title, collapse, expand;
};

const dock_side sides[] = {
    {VGUI_DOCK_LEFT, "designer.dock.collapse_left", {"workbench.outline.heading", "Outline"},
        {"workbench.dock.collapse_outline", "Collapse Outline"}, {"workbench.dock.expand_outline", "Expand Outline"}},
    {VGUI_DOCK_RIGHT, "designer.dock.collapse_right", {"workbench.dock.inspector", "Inspector"},
        {"workbench.dock.collapse_inspector", "Collapse Inspector"}, {"workbench.dock.expand_inspector", "Expand Inspector"}},
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
        std::map<std::string, std::string, std::less<>> values, font_resources;
        bool operator==(const inspector_state&) const = default;
    };

    struct information_dialog {
        const char* command;
        std::function<std::string()> message;
    };

    vgui_context* ui;
    session& history;
    const designer_strings& strings;
    kit::editor_context editor;
    kit::command_registry commands;
    std::function<void()> layout_changed;
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
    status_message message = localized_text{"workbench.status.ready", "Ready"};
    std::optional<information_dialog> pending_dialog;
    inspector_state inspected;
    struct live_field {
        vgui_widget* widget;
        std::string key;
        std::uint64_t revision;
        bool popup;
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
    designer_theme theme = default_designer_theme;
    vgui_widget *shell = nullptr, *dock = nullptr;
    std::array<vgui_widget*, std::size(sides)> side_buttons{};
    std::vector<vgui_widget*> outline_buttons;
    vgui_widget *status_label = nullptr, *pickup = nullptr, *outline_actions = nullptr;
    outline_texture pick_icon{nullptr, vgui_texture_destroy}, cancel_move_icon{nullptr, vgui_texture_destroy};
    float outline_actions_min_width = 0;
    vgui_widget *toolbar = nullptr, *menubar = nullptr, *quick_actions = nullptr;
    vgui_widget *save_button = nullptr, *export_button = nullptr;
    int toolbar_width = -1;
    float toolbar_scale = 0;
    std::optional<designer_language> toolbar_language;

    impl(vgui_context* context, session& model, designer_commands file, const designer_strings& locale)
        : ui(context), history(model), strings(locale), editor(context),
          layout_changed(std::move(file.layout_changed)), selected(model.current().root)
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
        vgui_dock_set_document_tabs(dock, 1);

        auto* structure = page("structure", sides[0].title, VGUI_DOCK_LEFT);
        build_structure(structure);
        auto* properties = page("inspector", sides[1].title, VGUI_DOCK_RIGHT);
        build_side_buttons();
        grid->build_with_labels(properties, inspector_labels());
        grid->set_change_listener([this](const std::string& key, const std::string& input) {
            finish_property_edit();
            edit_property(key, input);
        });
        grid->set_text_edit_listener([this](vgui_widget* widget, const std::string& key, const std::string& input,
            kit::property_grid::text_edit_event event) {
            return on_property_text(widget, key, input, event);
        });
        const auto preview_paint = [this](vgui_widget* widget, const std::string& key,
            const std::string& input, kit::property_grid::text_edit_event event) {
            return on_property_text(widget, key, input, event, true);
        };
        grid->set_color_popup_edit_listener(preview_paint);
        grid->set_gradient_popup_edit_listener(preview_paint);
        focus_listener = vgui_context_add_focus_listener(ui, [](vgui_context*, vgui_widget* old, vgui_widget*, void* user) {
            auto& self = *static_cast<impl*>(user);
            // The picker owns its transaction until the popup closes, even
            // when focus moves between the picker controls.
            if (self.live && !self.live->popup && self.live->widget == old) self.finish_property_edit();
        }, this);

        status_label = vgui_widget_add(shell, VGUI_LABEL, nullptr);
        vgui_widget_set_id(status_label, "designer.status");
        vgui_widget_set_margin(status_label, 6, 0, 6, 4);
        apply_chrome_palette();
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

    void add_information_command(vgui_widget* menu, const char* id, localized_text title,
        std::function<std::string()> make_message)
    {
        add_menu_command(menu, id, std::move(title),
            [this, dialog = information_dialog{id, std::move(make_message)}] {
                // Let menu activation close the popup chain before opening the dialog.
                pending_dialog = dialog;
            });
    }

    void show_information(const information_dialog& dialog)
    {
        const auto body = dialog.message();
        vgui_dialog_desc desc;
        vgui_dialog_desc_default(&desc);
        desc.title = commands.find(dialog.command)->title.c_str();
        desc.message = body.c_str();
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
                with_active_page([this, key] { return insertion_parent(key).has_value(); }));
        }

        if (callbacks.import_font)
            add_menu_command(resources, "designer.resource.import_font", {"workbench.menu.import_font", "Import Project Font…"},
                std::move(callbacks.import_font), with_active_page());

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
        for (const auto& side : sides) {
            add_menu_command(view, side.command, side.title, [this, zone = side.zone] {
                vgui_dock_set_side_collapsed(dock, zone, !vgui_dock_get_side_collapsed(dock, zone));
                refresh_side_buttons();
                if (layout_changed) layout_changed();
            }, with_active_page(), nullptr, with_active_page([this, zone = side.zone] {
                return !vgui_dock_get_side_collapsed(dock, zone);
            }));
        }
        vgui_menu_add_separator(view);
        add_menu_command(view, "designer.layout.restore_default",
            {"workbench.menu.restore_default_layout", "Restore Default Layout"}, [this] {
                set_dock_widths({});
                message = localized_text{"workbench.status.default_layout_restored", "Default layout restored."};
                if (layout_changed) layout_changed();
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
        add_menu_command(tools, "designer.tools.options", {"options.title", "Options"}, std::move(callbacks.open_options));

        auto* themes = add_submenu(tools, {"settings.theme", "Theme"});
        const auto add_theme = [&](const char* id, localized_text label, designer_theme value) {
            add_menu_command(themes, id, std::move(label), [set = callbacks.set_theme, value] { set(value); },
                [available = bool(callbacks.set_theme)] { return available; }, nullptr,
                [this, value] { return theme == value; });
        };
        add_theme("designer.theme.dark", {"settings.theme.dark", "Dark"}, designer_theme::dark);
        add_theme("designer.theme.light", {"settings.theme.light", "Light"}, designer_theme::light);

        auto* help = add_menu({"workbench.menu.help", "Help"});
        const auto add_help = [&](const char* id, localized_text label, localized_text explanation) {
            add_information_command(help, id, std::move(label), [this, explanation = std::move(explanation)] {
                return std::string(translate(explanation));
            });
        };
        add_help("designer.help.instructions", {"workbench.menu.instructions", "Instructions"},
            {"workbench.help.instructions", "Add controls from Resources > New Control. A selected container receives a child; a selected leaf receives a sibling.\n\n"
                "Select a control in the Outline and edit its properties in the Inspector. Use View > Run to try the page. "
                "Save writes the design; Export Package creates a separate folder without saving the design."});
        add_information_command(help, "designer.help.shortcuts", {"workbench.menu.shortcuts", "Shortcuts"}, [this] {
            std::string body = strings.text("workbench.help.shortcuts", "F10 opens the menu bar. Use the arrow keys and Enter to navigate, or Esc to close it. "
                "While a text field is focused, undo and redo apply to that field first.\n");
            for (const auto& command : commands.entries()) {
                if (command.shortcut.valid()) body += "\n" + shortcut_text(command.shortcut) + " — " + command.title;
            }
            return body;
        });
        add_help("designer.help.about", {"workbench.menu.about", "About"},
            {"workbench.help.about", "VGUI UI Designer is the VGUI sample editor for .vgui.json design files. "
                "Run mode previews the active page without changing its design."});
    }

    void build_toolbar(designer_commands callbacks)
    {
        auto* bar = toolbar = make_row(shell);
        vgui_widget_set_id(bar, "designer.toolbar");
        vgui_widget_set_size(bar, 0, 48);
        vgui_widget_set_padding(bar, 0, 0, toolbar_padding_right, 0);
        vgui_widget_set_gap(bar, toolbar_gap);
        vgui_widget_set_align_items(bar, VGUI_ALIGN_CENTER);
        vgui_widget_set_border(bar, 0, 0);
        vgui_widget_set_rounding(bar, 0);

        menubar = vgui_widget_add(bar, VGUI_MENUBAR, nullptr);
        vgui_widget_set_id(menubar, "designer.menubar");
        vgui_widget_set_background(menubar, 0);
        vgui_widget_set_border(menubar, 0, 0);
        build_menus(std::move(callbacks));

        const auto style_action = [](vgui_widget* button) {
            vgui_widget_set_size(button, 0, 30);
            vgui_widget_set_padding(button, 9, 0, 9, 0);
            vgui_widget_set_font_size(button, 13);
            vgui_widget_set_text_align(button, .5f, .5f);
            vgui_widget_set_rounding(button, 5);
            return button;
        };
        auto* spacer = vgui_widget_add(bar, VGUI_PANEL, nullptr);
        vgui_widget_set_grow(spacer, 1);
        vgui_widget_set_padding(spacer, 0, 0, 0, 0);
        vgui_widget_set_background(spacer, 0);
        vgui_widget_set_border(spacer, 0, 0);
        auto* actions = quick_actions = make_row(bar);
        vgui_widget_set_id(actions, "designer.toolbar.actions");
        vgui_widget_set_align_self(actions, VGUI_ALIGN_CENTER);
        vgui_widget_set_align_items(actions, VGUI_ALIGN_CENTER);
        vgui_widget_set_padding(actions, 3, 0, 3, 0);
        vgui_widget_set_background(actions, 0);
        vgui_widget_set_border(actions, 0, 0);
        vgui_widget_set_gap(actions, 8);
        save_button = style_action(add_button(actions, "designer.file.save", {"workbench.toolbar.save", "Save"}));
        export_button = style_action(add_button(actions, "designer.file.export", {"workbench.toolbar.export", "Export Package"}));
        vgui_widget_set_border(export_button, 0, 0);
        vgui_widget_set_padding(export_button, 13, 0, 13, 0);
        vgui_widget_set_font_weight(export_button, VGUI_FONT_WEIGHT_SEMI_BOLD);
        bind_label(export_button, {"workbench.toolbar.export_hint", "Export the current design and resources to a new folder (does not save the design)."}, vgui_widget_set_tooltip);
    }

    const chrome_palette& palette() const
    {
        return theme == designer_theme::light ? light_chrome : dark_chrome;
    }

    void apply_chrome_palette()
    {
        const auto& colors = palette();
        vgui_widget_set_background(toolbar, colors.background);
        vgui_widget_set_text_color(menubar, colors.text);
        vgui_widget_set_text_color(save_button, colors.text);
        vgui_widget_set_background(export_button, colors.primary_background);
        vgui_widget_set_text_color(export_button, colors.primary_text);
        const auto style_neutral_button = [&](vgui_widget* button) {
            vgui_widget_set_background(button, colors.background);
            vgui_widget_set_border(button, colors.edge, 1);
        };
        style_neutral_button(save_button);
        for (auto* button : side_buttons) style_neutral_button(button);
        for (auto* button : outline_buttons)
            vgui_button_set_icon_tint(button, button == pickup && !move_source.empty() ? colors.move_active : colors.text);
        grid->set_light_theme(theme == designer_theme::light);
        grid->set_edit_field_background(colors.edit_background);
        vgui_widget_mark_dirty(shell);
    }

    void set_theme(designer_theme value)
    {
        if (theme == value) return;
        theme = value;
        apply_chrome_palette();
        refresh_command_states();
    }

    void refresh_toolbar_layout()
    {
        const auto scale = vgui_context_get_ui_scale(ui);
        const auto language = strings.language();
        int context_width = 0;
        vgui_context_get_size(ui, &context_width, nullptr);
        if (context_width == toolbar_width && scale == toolbar_scale && language == toolbar_language) return;
        toolbar_width = context_width;
        toolbar_scale = scale;
        toolbar_language = language;

        float menu_width = 0, actions_width = 0;
        vgui_widget_get_intrinsic_size(menubar, &menu_width, nullptr);
        vgui_widget_get_intrinsic_size(quick_actions, &actions_width, nullptr);
        const float chrome = (toolbar_padding_right + 2 * toolbar_gap) * scale;
        const bool show = context_width <= 0 || context_width >= menu_width + actions_width + chrome;
        if ((vgui_widget_get_visible(quick_actions) != 0) != show) vgui_widget_set_visible(quick_actions, show);
    }

    designer_dock_widths dock_widths() const
    {
        const float scale = vgui_context_get_ui_scale(ui);
        return {vgui_dock_get_zone_size(dock, VGUI_DOCK_LEFT) / scale,
                vgui_dock_get_zone_size(dock, VGUI_DOCK_RIGHT) / scale,
                vgui_dock_get_side_collapsed(dock, VGUI_DOCK_LEFT) != 0,
                vgui_dock_get_side_collapsed(dock, VGUI_DOCK_RIGHT) != 0};
    }

    void set_dock_widths(designer_dock_widths widths)
    {
        vgui_dock_set_zone_size(dock, VGUI_DOCK_LEFT, widths.left);
        vgui_dock_set_zone_size(dock, VGUI_DOCK_RIGHT, widths.right);
        vgui_dock_set_side_collapsed(dock, VGUI_DOCK_LEFT, widths.left_collapsed);
        vgui_dock_set_side_collapsed(dock, VGUI_DOCK_RIGHT, widths.right_collapsed);
        refresh_side_buttons();
    }

    void build_side_buttons()
    {
        for (std::size_t i = 0; i < side_buttons.size(); ++i) {
            const auto& side = sides[i];
            auto* button = side_buttons[i] = make_button(dock, side.command, nullptr, false);
            vgui_widget_set_padding(button, 0, 0, 0, 0);
            vgui_widget_set_text_align(button, .5f, .5f);
            vgui_widget_set_font_size(button, 17);
            vgui_widget_set_rounding(button, 4);
            vgui_widget_set_cursor(button, VGUI_CURSOR_HAND);
            vgui_widget_set_tooltip_anchor(button, VGUI_TOOLTIP_ANCHOR_WIDGET);
            vgui_dock_set_side_button(dock, side.zone, button);
        }
        refresh_side_buttons();
    }

    void refresh_side_buttons()
    {
        for (std::size_t i = 0; i < side_buttons.size(); ++i) {
            auto* button = side_buttons[i];
            if (!button) continue;
            const auto& side = sides[i];
            const bool collapsed = vgui_dock_get_side_collapsed(dock, side.zone) != 0;
            const char* label = translate(collapsed ? side.expand : side.collapse);
            const bool arrow_left = (side.zone == VGUI_DOCK_LEFT) != collapsed;
            vgui_widget_set_text(button, arrow_left ? "«" : "»");
            vgui_widget_set_tooltip(button, label);
            vgui_widget_set_a11y_name(button, label);
        }
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
                {"Font", strings.text("workbench.inspector.category.font", "Font")},
                {"Appearance", strings.text("workbench.inspector.category.appearance", "Appearance")},
                {"Gradient colors", strings.text("workbench.inspector.category.gradient_colors", "Gradient colors")},
                {"Gradient paint", strings.text("workbench.inspector.category.gradient_paint", "Gradient paint")},
                {"Gradient animation", strings.text("workbench.inspector.category.gradient_animation", "Gradient animation")},
                {"Gradient transform", strings.text("workbench.inspector.category.gradient_transform", "Gradient transform")},
                {"Image", strings.text("workbench.inspector.category.image", "Image")},
                {"Layout details", strings.text("workbench.inspector.category.layout_details", "Layout details")},
                {"Interaction", strings.text("workbench.inspector.category.interaction", "Interaction")},
                {"Pages", strings.text("workbench.inspector.category.pages", "Pages")},
                {"Geometry & state", strings.text("workbench.inspector.category.geometry_state", "Geometry & state")},
            },
            .pick_color = strings.text("workbench.inspector.font.pick_color", "Pick color"),
            .gradient_offset = strings.text("workbench.inspector.gradient_popup.offset", "Offset"),
            .gradient_stops = strings.text("workbench.inspector.gradient_popup.stops", "Stops"),
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

    vgui_widget* make_outline_button(vgui_widget* row, const char* id, vgui_texture* icon)
    {
        auto* button = make_button(row, id, nullptr, true);
        vgui_widget_set_size(button, 38, 32);
        vgui_widget_set_padding(button, 0, 0, 0, 0);
        vgui_widget_set_text_align(button, .5f, .5f);
        vgui_widget_set_tooltip_anchor(button, VGUI_TOOLTIP_ANCHOR_WIDGET);
        vgui_button_set_icon(button, icon, outline_icon_size, outline_icon_size, VGUI_BUTTON_ICON_LEFT);
        outline_buttons.push_back(button);
        return button;
    }

    void add_outline_button(vgui_widget* row, const char* id, localized_text label,
        std::initializer_list<icon_stroke> strokes)
    {
        const auto icon = make_outline_icon(ui, strokes);
        bind_label(make_outline_button(row, id, icon.get()), std::move(label), describe_outline_button);
    }

    void refresh_pickup_button()
    {
        const char* label = pickup_label();
        if (std::string_view(label) == vgui_widget_get_a11y_name(pickup)) return;
        describe_outline_button(pickup, label);
        commands.find(vgui_widget_get_id(pickup))->title = label;
        const bool moving = !move_source.empty();
        auto* icon = moving ? cancel_move_icon.get() : pick_icon.get();
        vgui_button_set_icon(pickup, icon, outline_icon_size, outline_icon_size, VGUI_BUTTON_ICON_LEFT);
        const auto& colors = palette();
        vgui_button_set_icon_tint(pickup, moving ? colors.move_active : colors.text);
        if (!moving)
            vgui_widget_set_tooltip(pickup, strings.text("workbench.outline.pick_hint", "Pick up a node to move it to a different container"));
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
        if (desc.key == "icon")
            return {"workbench.inspector.icon_path", "Button icon path"};
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
            if (value == "wrap") return strings.text("workbench.inspector.layout.wrap", "Wrap");
        } else if (desc.key == "align_items" || desc.key == "justify" || desc.key == "align_self" || desc.key == "align_content" || desc.key == "text_align_x") {
            if (value == "start") return strings.text("workbench.inspector.alignment.start", "Start");
            if (value == "center") return strings.text("workbench.inspector.alignment.center", "Center");
            if (value == "end") return strings.text("workbench.inspector.alignment.end", "End");
            if (value == "stretch") return strings.text("workbench.inspector.alignment.stretch", "Stretch");
            if (value == "space-between") return strings.text("workbench.inspector.alignment.space_between", "Space between");
            if (value == "space-around") return strings.text("workbench.inspector.alignment.space_around", "Space around");
        } else if (desc.key == "background_gradient_direction") {
            if (value == "horizontal") return strings.text("workbench.inspector.gradient_direction.horizontal", "Left to right");
            if (value == "vertical") return strings.text("workbench.inspector.gradient_direction.vertical", "Top to bottom");
        } else if (desc.key == "gradient_animation") {
            if (value == "none") return strings.text("workbench.inspector.gradient_animation.none", "None");
            if (value == "shine") return strings.text("workbench.inspector.gradient_animation.shine", "Shine sweep");
            if (value == "color") return strings.text("workbench.inspector.gradient_animation.color", "Color fade");
        } else if (desc.key == "text_orientation") {
            if (value == "horizontal") return strings.text("workbench.inspector.text_orientation.horizontal", "Horizontal");
            if (value == "vertical") return strings.text("workbench.inspector.text_orientation.vertical", "Vertical (single column)");
        } else if (desc.key == "font_weight" || desc.key == "font_slant") {
            if (value == "default") return strings.text("workbench.inspector.font.default", "Use default");
            if (value == "regular") return strings.text("workbench.inspector.font.regular", "Regular");
            if (value == "medium") return strings.text("workbench.inspector.font.medium", "Medium");
            if (value == "semi_bold") return strings.text("workbench.inspector.font.semi_bold", "Semi-bold");
            if (value == "bold") return strings.text("workbench.inspector.font.bold", "Bold");
            if (value == "normal") return strings.text("workbench.inspector.font.normal", "Upright");
            if (value == "italic") return strings.text("workbench.inspector.font.italic", "Italic");
            if (value == "oblique") return strings.text("workbench.inspector.font.oblique", "Oblique");
        }
        const std::string fallback(value);
        return strings.text("workbench.inspector.choice." + fallback, fallback.c_str());
    }

    void build_structure(vgui_widget* panel)
    {
        vgui_widget_set_padding(panel, 6, 5, 6, 5);
        vgui_widget_set_gap(panel, 4);
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
        add_outline_button(row, "designer.move.up", {"workbench.outline.up", "Up"},
            {{8, 13, 8, 3}, {4, 7, 8, 3}, {8, 3, 12, 7}});
        add_outline_button(row, "designer.move.down", {"workbench.outline.down", "Down"},
            {{8, 3, 8, 13}, {4, 9, 8, 13}, {8, 13, 12, 9}});
        add_outline_button(row, "designer.duplicate", {"workbench.outline.copy", "Copy"},
            {{3, 10, 3, 2.5f}, {3, 2.5f, 10, 2.5f}, {6, 5.5f, 13, 5.5f},
             {13, 5.5f, 13, 13.5f}, {13, 13.5f, 6, 13.5f}, {6, 13.5f, 6, 5.5f}});
        add_outline_button(row, "designer.delete", {"workbench.outline.delete", "Delete"},
            {{2.5f, 4.5f, 13.5f, 4.5f}, {6, 2.5f, 10, 2.5f}, {4, 5, 5, 13.5f},
             {5, 13.5f, 11, 13.5f}, {11, 13.5f, 12, 5}, {7, 7.5f, 7, 11}, {9, 7.5f, 9, 11}});
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
        pick_icon = make_outline_icon(ui, {{3, 6, 3, 13}, {3, 13, 10, 13}, {10, 13, 10, 10}, {3, 6, 6, 6},
            {7, 9, 13, 3}, {8.5f, 3, 13, 3}, {13, 3, 13, 7.5f}});
        cancel_move_icon = make_outline_icon(ui, {{4, 4, 12, 12}, {4, 12, 12, 4}});
        pickup = make_outline_button(row, "designer.move.pick", pick_icon.get());
        const localized_text move_here{"workbench.outline.move_here", "Move Here"};
        define_command("designer.move.here", translate(move_here), [this] {
            if (apply({move_node{move_source, selected}}, moved_status())) {
                select(std::exchange(move_source, {}));
            }
        }, with_active_page([this] {
            const auto place = location(move_source);
            return place && place->first != selected && can_move(move_source, selected);
        }));
        add_outline_button(row, "designer.move.here", move_here,
            {{3, 9, 3, 13}, {3, 13, 13, 13}, {13, 13, 13, 9}, {8, 2.5f, 8, 10},
             {5.5f, 7.5f, 8, 10}, {8, 10, 10.5f, 7.5f}});
    }

    bool apply(std::initializer_list<edit> edits, status_message success, std::string_view field = {}, bool interactive = false)
    {
        return apply_batch({edits.begin(), edits.size()}, std::move(success), field, interactive);
    }

    bool apply_batch(std::span<const edit> batch, status_message success, std::string_view field, bool interactive)
    {
        diagnostic error;
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
        // panels rather than hiding their widgets; the host owns the document pages.
        for (const auto* id : {"structure", "inspector"}) {
            const auto panel = vgui_dock_find(dock, id);
            if (active) vgui_dock_restore(dock, panel);
            else vgui_dock_close(dock, panel);
        }
        vgui_widget_set_enabled(dock, active);
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

    std::optional<std::string> insertion_parent(std::string_view type) const
    {
        const auto& doc = history.current();
        if (doc.nodes.size() >= max_nodes) return std::nullopt;
        auto target = doc.nodes.find(selected);
        if (target == doc.nodes.end()) return std::nullopt;
        if (!is_container(target->second)) {
            const auto place = location(selected);
            if (!place) return std::nullopt;
            target = doc.nodes.find(place->first);
        }
        const auto* parent = find_type(target->second.type);
        if (!parent || !accepts_child(*parent, type, target->second.children.size())) return std::nullopt;
        return target->first;
    }

    void add_node(const std::string& type)
    {
        const auto parent = insertion_parent(type);
        const auto* descriptor = find_type(type);
        if (!parent || !descriptor) return;
        node data;
        data.type = type;
        auto id = fresh_key("node_", history.current().nodes);
        // Seed fresh controls; zero or missing dimensions in existing documents still mean auto-size.
        data.props.emplace("width", descriptor->initial_width);
        data.props.emplace("height", descriptor->initial_height);
        if (apply({insert_node{id, *parent, std::move(data)}},
                formatted_status{{"workbench.status.added_type", "Added {value}"}, type_label(*descriptor)})) select(std::move(id));
    }

    void erase()
    {
        const auto place = location(selected);
        if (!place) return;
        if (apply({erase_node{selected}}, localized_text{"workbench.status.deleted_subtree", "Deleted subtree"})) select(place->first);
    }

    bool can_duplicate() const
    {
        const auto place = location(selected);
        const auto& doc = history.current();
        if (!place || doc.nodes.size() >= max_nodes) return false;
        const auto& parent = doc.nodes.at(place->first);
        if (!accepts_child(*find_type(parent.type), doc.nodes.at(selected).type, parent.children.size())) return false;
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
        if (!place || !can_duplicate()) return;
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
        if (id == doc.root || !doc.nodes.contains(id) || target == doc.nodes.end()) return false;
        const auto* descriptor = find_type(target->second.type);
        const auto already_child = std::find(target->second.children.begin(), target->second.children.end(), id) != target->second.children.end();
        if (!descriptor || !accepts_child(*descriptor, doc.nodes.find(id)->second.type,
                target->second.children.size() - (already_child ? 1 : 0))) return false;
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
        kit::property_grid::text_edit_event event, bool popup = false)
    {
        using text_event = kit::property_grid::text_edit_event;
        if (refreshing || replaced || !page_active || inspected.id != selected) return true;
        if (event != text_event::change) {
            if (live && live->widget == widget && live->popup == popup)
                finish_property_edit(event == text_event::submit);
            return true;
        }
        if (live && (live->widget != widget || live->revision != history.revision())) finish_property_edit();
        if (!live) live = live_field{widget, key, history.revision(), popup};
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

        const auto updated = formatted_status{{"workbench.status.updated_property", "Updated {value}"}, property_label(*desc)};
        if (desc->type == value_type::gradient_stops) {
            kit::gradient_value input_stops;
            std::string reason;
            if (!kit::gradient_editor::parse_value(input, input_stops, &reason)) {
                field_errors[key] = reason;
                sync_inspector = true;
                return;
            }
            std::vector<gradient_stop> stops;
            for (const auto& stop : input_stops.stops) stops.push_back({stop.t, stop.color});
            auto canonical = encode_gradient_stops(stops);
            auto stored = canonical == std::get<std::string>(desc->default_value) ? std::nullopt :
                std::optional<value>(std::move(canonical));
            apply({set_property{inspected.id, key, std::move(stored)}}, updated, key, interactive);
            return;
        }
        if (desc->type == value_type::items) {
            std::string canonical, reason;
            std::vector<edit> batch;
            if (!prepare_items_edit(doc, found->second.type, input, canonical, batch, reason)) {
                field_errors[key] = reason;
                message = formatted_status{{"workbench.status.invalid_items", "Invalid items: {value}"}, std::move(reason)};
                sync_inspector = true;
                return;
            }
            auto stored = canonical == std::get<std::string>(desc->default_value) ? std::nullopt :
                std::optional<value>(std::move(canonical));
            batch.emplace_back(set_property{inspected.id, key, std::move(stored)});
            apply_batch(batch, updated, key, interactive);
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
        const auto& item = found->second;
        if (item.type == "radio" && (key == "radio_group" || key == "checked")) {
            const auto* group_property = find_property("radio", "radio_group");
            const auto* checked_property = find_property("radio", "checked");
            const bool checked = key == "checked" ? input == "true" : std::get<bool>(property_value(item, *checked_property));
            const auto place = location(inspected.id);
            if (checked && place) {
                const auto& group = key == "radio_group" ? input : std::get<std::string>(property_value(item, *group_property));
                for (const auto& peer_id : doc.nodes.at(place->first).children) {
                    if (peer_id == inspected.id) continue;
                    const auto& peer = doc.nodes.at(peer_id);
                    if (peer.type == "radio" && std::get<bool>(property_value(peer, *checked_property)) &&
                        std::get<std::string>(property_value(peer, *group_property)) == group) {
                        // A valid group has at most one checked member. Transfer
                        // the choice in the same edit, including live renames.
                        apply({set_property{peer_id, "checked", std::nullopt}, set_property{inspected.id, key, std::move(stored)}},
                            updated, key, interactive);
                        return;
                    }
                }
            }
        }
        apply({set_property{inspected.id, key, std::move(stored)}}, updated, key, interactive);
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
        inspector_state next{selected, item.type, {}, {}};
        std::vector<kit::property_desc> rows;
        const auto add = [&](kit::property_desc row) {
            next.values.emplace(row.id, row.value);
            rows.push_back(std::move(row));
        };
        add({.id = node_id, .label = strings.text("workbench.inspector.node_id", "Node ID"), .value = selected, .read_only = true});
        add({.id = node_name, .label = strings.text("workbench.inspector.name", "Name"), .value = item.name, .has_default = true});
        const auto place = location(selected);
        std::optional<std::array<vgui_color, 8>> gradient;
        if (item.type == "button" && std::get<std::string>(property_value(item, *find_property("button", "button_style"))) == "gradient")
            gradient = detail::button_gradient_palette(std::get<std::string>(property_value(item, *find_property("button", "gradient_preset"))));
        const auto gradient_fields = std::span(detail::extra_button_gradient_properties);
        const bool panel_gradient = item.type == "panel" &&
            std::get<std::string>(property_value(item, *find_property("panel", "background_style"))) == "gradient";
        const bool decorative_gradient = item.type == "gradient";
        const auto kind = decorative_gradient ? std::string_view(std::get<std::string>(property_value(item, *find_property("gradient", "gradient_kind")))) : std::string_view{};
        const auto animation = decorative_gradient ? std::string_view(std::get<std::string>(property_value(item, *find_property("gradient", "gradient_animation")))) : std::string_view{};
        const auto add_descriptors = [&](std::span<const property_descriptor> descriptors, const char* category) {
            for (const auto& desc : descriptors) {
                const auto key = desc.key;
                const bool interaction = key.starts_with("tooltip") || key.starts_with("focus") || key == "input_enabled" ||
                    key == "cursor" || key == "a11y_name";
                if (decorative_gradient) {
                    if (interaction || key == "enabled") continue;
                    if ((key == "gradient_x0" || key == "gradient_x1" || key == "gradient_y0" || key == "gradient_y1") && kind != "linear") continue;
                    if (key.starts_with("gradient_center_") && kind == "linear") continue;
                    if ((key.starts_with("gradient_outer_") || key.starts_with("gradient_radius") || key == "gradient_ellipse") && kind != "radial") continue;
                    if (key.starts_with("gradient_angle") && kind != "sweep") continue;
                    if (key.starts_with("gradient_animation_")) {
                        if (animation == "none") continue;
                        if (key == "gradient_animation_color" && animation != "color") continue;
                        if ((key == "gradient_animation_dx" || key == "gradient_animation_dy") && animation != "shine") continue;
                    }
                }
                const auto gradient_field = std::ranges::find(gradient_fields, desc.key, &property_descriptor::key);
                const bool gradient_color = gradient_field != gradient_fields.end();
                const bool panel_gradient_field = desc.key.starts_with("background_gradient_");
                if ((gradient_color && !gradient) || (panel_gradient_field && !panel_gradient)) continue;
                kit::property_desc row;
                row.id = desc.key;
                row.label = translate(property_label(desc));
                row.category = desc.key.starts_with("font_") || desc.key == "text_color" ? "Font" : category;
                if (decorative_gradient && key.starts_with("gradient_animation")) row.category = "Gradient animation";
                else if (decorative_gradient && key.starts_with("gradient_"))
                    row.category = key.starts_with("gradient_affine_") ? "Gradient transform" : "Gradient paint";
                else if (gradient_color || panel_gradient_field) row.category = "Gradient colors";
                else if (desc.key.starts_with("dock_") || desc.key == "page_title" || desc.key == "tab_enabled") {
                    if (!place) continue;
                    const auto& parent = doc.nodes.at(place->first).type;
                    if (desc.key == "page_title" ? (parent != "dock" && parent != "tabview") :
                        (desc.key == "tab_enabled" ? parent != "tabview" : parent != "dock")) continue;
                    row.category = "Pages";
                } else if (interaction) row.category = "Interaction";
                else if (item.type == "image" && desc.key != "resource" && std::string_view(category) == "Content")
                    row.category = "Image";
                else if (desc.key.starts_with("scroll") || desc.key.starts_with("margin_") || desc.key == "max_width" ||
                         desc.key == "max_height" || desc.key == "min_width" || desc.key == "min_height" || desc.key == "align_self" || desc.key == "align_content")
                    row.category = "Layout details";
                else if ((desc.type == value_type::color && desc.key != "text_color") || desc.key == "border_width" ||
                         desc.key == "rounding" || desc.key == "opacity" || desc.key == "background_style" || desc.key == "button_style" ||
                         desc.key == "gradient_preset" || desc.key == "icon" || desc.key.starts_with("icon_") ||
                         desc.key.starts_with("text_align_")) row.category = "Appearance";
                row.value = display_value(property_value(item, desc));
                row.default_value = display_value(desc.default_value);
                row.has_default = true;
                if (desc.key == "radio_group") {
                    row.placeholder = strings.text("workbench.inspector.radio_group.default", "Default group");
                    row.tooltip = strings.text("workbench.inspector.radio_group.hint",
                        "Only radio buttons with the same group name in this container exclude one another. Leave blank for the default group.");
                } else if (desc.key == "gradient_preset") {
                    row.tooltip = strings.text("workbench.inspector.gradient_preset.hint", "Applies when Button style is Gradient.");
                } else if (desc.key == "text_orientation") {
                    row.tooltip = strings.text("workbench.inspector.text_orientation.hint",
                        "Vertical text uses one column. Increase the height or set it to 0 for automatic sizing.");
                }
                if (key == "gradient_animation")
                    row.tooltip = strings.text("workbench.inspector.gradient_animation.hint", "Plays automatically in Run. Design shows the original gradient.");
                else if (key == "gradient_animation_dx" || key == "gradient_animation_dy")
                    row.tooltip = strings.text("workbench.inspector.gradient_animation.travel_hint", "Total travel relative to the widget size, centred on the original gradient. Use transparent endpoint colors for a moving highlight.");
                else if (key == "gradient_animation_duration")
                    row.tooltip = strings.text("workbench.inspector.gradient_animation.duration_hint", "Seconds for one sweep, one fade, or one complete out-and-back color loop.");
                else if (decorative_gradient && row.category == "Gradient transform")
                    row.tooltip = strings.text("workbench.inspector.gradient_transform.hint", "Transforms the paint, not the shape. Translation is relative to the widget size.");
                else if (decorative_gradient && key.starts_with("gradient_angle"))
                    row.tooltip = strings.text("workbench.inspector.gradient_angle.hint", "Degrees clockwise from the right (0–360). Reverse the angles to reverse the sweep.");
                else if (decorative_gradient && key.starts_with("gradient_radius"))
                    row.tooltip = strings.text("workbench.inspector.gradient_radius.hint", "Relative to the shorter side; 0.5 is half of that side.");
                else if (decorative_gradient && desc.type == value_type::number && row.category == "Gradient paint")
                    row.tooltip = strings.text("workbench.inspector.gradient_position.hint", "Relative to the widget. 0 is the left or top edge; 1 is the right or bottom edge.");
                switch (desc.type) {
                case value_type::boolean:
                    row.type = kit::property_type::boolean;
                    if (item.type == "radio" && desc.key == "checked")
                        row.tooltip = strings.text("workbench.inspector.radio_group.checked_hint",
                            "Selecting this as the initial choice clears the previous choice in the same group.");
                    break;
                case value_type::number:
                    row.type = kit::property_type::number;
                    row.has_range = true;
                    row.min_value = desc.min;
                    row.max_value = desc.max;
                    if (desc.key == "font_size") row.tooltip = strings.text("workbench.inspector.font.size_hint",
                        "0 uses the default size. Custom size: 4–256 logical pixels.");
                    break;
                case value_type::color:
                    row.type = kit::property_type::color;
                    row.allow_empty = true;
                    if (key == "gradient_animation_color") {
                        row.inherited_color = parse_text_color(std::get<std::string>(desc.default_value));
                        row.placeholder = std::get<std::string>(desc.default_value);
                        row.tooltip = strings.text("workbench.inspector.gradient_animation.color_hint", "Fades all stops toward this color. Alpha dims the original gradient without making transparent stops opaque.");
                    } else if (gradient_color) {
                        row.inherited_color = (*gradient)[gradient_field - gradient_fields.begin()];
                        row.placeholder = strings.text("workbench.inspector.gradient_color.inherit", "Use preset");
                        row.tooltip = strings.text("workbench.inspector.gradient_color.hint",
                            "Enter #RRGGBB or #RRGGBBAA, or use the swatch. Clear to use the selected preset color.");
                    } else {
                        if (panel_gradient_field)
                            row.inherited_color = desc.key == "background_gradient_start"
                                ? detail::default_panel_gradient_start : detail::default_panel_gradient_end;
                        row.placeholder = "#RRGGBB";
                        row.tooltip = desc.key == "text_color" ? strings.text("workbench.inspector.font.color_hint",
                            "Enter #RRGGBB or #RRGGBBAA, or use the swatch. Clear for the theme text color.") :
                            strings.text("workbench.inspector.color_hint", "Enter #RRGGBB or #RRGGBBAA, or use the swatch. Clear for the default color.");
                    }
                    break;
                case value_type::gradient_stops: {
                    const auto editor_text = [](const std::string& json) {
                        std::vector<gradient_stop> stops;
                        std::string error;
                        kit::gradient_value value;
                        if (decode_gradient_stops(json, stops, error))
                            for (const auto& stop : stops) value.stops.push_back({stop.offset, stop.color});
                        return kit::gradient_editor::format_value(value);
                    };
                    row.type = kit::property_type::gradient;
                    row.value = editor_text(row.value);
                    row.default_value = editor_text(row.default_value);
                    row.gradient_stop_limit = max_gradient_stops;
                    row.tooltip = strings.text("workbench.inspector.gradient_stops.hint", "Click to edit up to 32 color stops and their opacity. Changes preview immediately.");
                    break;
                }
                case value_type::items:
                    row.multiline = true;
                    row.value = format_items(row.value);
                    row.tooltip = item.type == "list" || item.type == "tree" ?
                        strings.text("workbench.inspector.row_icons_hint",
                            R"(Optional row "icon" and up to four "buttons": [{"icon":"icons/open.png","action":"row.open"}]. Use a project-relative path or resource key. Valid edits apply immediately.)") :
                        strings.text("workbench.inspector.menu_icons_hint",
                            R"(Add "icon":"icons/open.png" to a menu item or submenu. Use a project-relative path or resource key. Valid edits apply immediately.)");
                    break;
                case value_type::choice:
                    row.type = kit::property_type::enumeration;
                    for (const auto choice : desc.choices) {
                        row.choices.push_back(choice_label(desc, choice));
                        row.choice_values.emplace_back(choice);
                    }
                    break;
                case value_type::font_key:
                    row.type = kit::property_type::enumeration;
                    row.allow_empty = true;
                    row.placeholder = strings.text("workbench.inspector.font.default", "Use default");
                    row.tooltip = strings.text("workbench.inspector.font.family_hint",
                        "Choose a bundled family or use Resources > Import Project Font to add one.");
                    row.choices = {strings.text("workbench.inspector.font.builtin_sans", "DejaVu Sans"),
                        strings.text("workbench.inspector.font.builtin_sans_sc", "Designer Sans SC (Chinese first)")};
                    row.choice_values = {"builtin.sans", "builtin.sans_sc"};
                    for (const auto& [key, path] : doc.resources) {
                        if (!is_font_path(path) || is_builtin_font(key)) continue;
                        row.choices.push_back(path);
                        row.choice_values.push_back(key);
                        next.font_resources.emplace(key, path);
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
                    auto resource_category = row.category;
                    add(std::move(row));
                    add({.id = desc.key == "icon" ? std::string("@icon-resource-key") : resource_key,
                        .label = strings.text(desc.key == "icon" ? "workbench.inspector.icon_resource_key" : "workbench.inspector.resource_key",
                            desc.key == "icon" ? "Icon resource key" : "Resource key"), .value = key,
                        .category = std::move(resource_category), .read_only = true});
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
        // Keep common authoring fields above optional chrome, regardless of
        // the order in which the schema registers a kind's appearance rows.
        constexpr std::string_view order[] = {"General", "Container", "Content", "Font", "Gradient paint", "Gradient animation", "Geometry & state", "Pages",
            "Appearance", "Gradient colors", "Gradient transform", "Image", "Layout details", "Interaction"};
        const auto rank = [&](const kit::property_desc& row) {
            return std::find(std::begin(order), std::end(order), row.category) - std::begin(order);
        };
        std::stable_sort(rows.begin(), rows.end(), [&](const auto& left, const auto& right) { return rank(left) < rank(right); });

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
        refresh_pickup_button();
        // Keep the icon hit targets at their natural size when the dock narrows.
        float width = 0;
        vgui_widget_get_intrinsic_size(outline_actions, &width, nullptr);
        const float min_width = width / vgui_context_get_ui_scale(ui);
        if (min_width != outline_actions_min_width) {
            outline_actions_min_width = min_width;
            vgui_widget_set_min_size(outline_actions, min_width, 0);
        }
        refresh_toolbar_layout();
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
        refresh_side_buttons();
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
        if (auto dialog = std::exchange(pending_dialog, std::nullopt))
            show_information(*dialog);
    }
};

designer_workbench::designer_workbench(vgui_context* ui, session& history, designer_commands commands,
    const designer_strings& strings)
    : impl_(std::make_unique<impl>(ui, history, std::move(commands), strings)) {}

designer_workbench::~designer_workbench() = default;
vgui_widget* designer_workbench::document_dock() const { return impl_->dock; }
designer_dock_widths designer_workbench::dock_widths() const { return impl_->dock_widths(); }
void designer_workbench::set_dock_widths(designer_dock_widths widths) { impl_->set_dock_widths(widths); }
void designer_workbench::set_page_active(bool active) { impl_->set_page_active(active); }
std::string_view designer_workbench::selection() const { return impl_->selected; }
void designer_workbench::select(std::string id) { impl_->select(std::move(id)); }
void designer_workbench::finish_edit() { impl_->finish_property_edit(); }
void designer_workbench::document_replaced(std::string selection) { impl_->document_replaced(std::move(selection)); }
void designer_workbench::refresh() { impl_->refresh(); }
void designer_workbench::refresh_language() { impl_->refresh_language(); }
void designer_workbench::set_theme(designer_theme theme) { impl_->set_theme(theme); }
void designer_workbench::status(std::string message) { impl_->message = std::move(message); }

} // namespace vgui::ui_design
