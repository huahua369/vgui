#include "ui_designer_options.h"

#include <vgui/editor/property_grid.h>

#include <array>
#include <charconv>
#include <cstdio>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace vgui::ui_design {
namespace {
namespace kit = vgui_editor_kit;
using kit::property_type;
using edit_event = kit::property_grid::text_edit_event;
using settings = designer_grid_settings;
using member = std::variant<bool settings::*, designer_grid_style settings::*, float settings::*,
    int settings::*, std::uint32_t settings::*>;

struct field {
    const char* id;
    const char* key;
    const char* label;
    property_type type;
    member value;
    double min = 0, max = 0;
};

constexpr std::array fields{
    field{"visible", "options.grid.visible", "Show grid", property_type::boolean, &settings::visible},
    field{"style", "options.grid.style", "Grid style", property_type::enumeration, &settings::style},
    field{"spacing", "options.grid.spacing", "Base spacing", property_type::number, &settings::spacing,
        designer_grid_min_spacing, designer_grid_max_spacing},
    field{"major_every", "options.grid.major_every", "Major interval", property_type::integer, &settings::major_every,
        designer_grid_min_major_every, designer_grid_max_major_every},
    field{"hide_below", "options.grid.hide_below", "Hide below", property_type::number, &settings::hide_below,
        designer_grid_min_hide_below, designer_grid_max_hide_below},
    field{"line_width", "options.grid.line_width", "Line width", property_type::number, &settings::line_width,
        designer_grid_min_line_width, designer_grid_max_line_width},
    field{"background", "options.grid.background", "Background", property_type::color, &settings::background},
    field{"minor_color", "options.grid.minor_color", "Minor grid line", property_type::color, &settings::minor_color},
    field{"major_color", "options.grid.major_color", "Major grid line", property_type::color, &settings::major_color},
    field{"axis_color", "options.grid.axis_color", "Axis", property_type::color, &settings::axis_color},
};

std::optional<std::size_t> field_index(std::string_view id)
{
    for (std::size_t i = 0; i < fields.size(); ++i)
        if (id == fields[i].id) return i;
    return std::nullopt;
}

std::string field_text(const field& item, const settings& grid)
{
    return std::visit([&](auto ptr) -> std::string {
        using type = std::remove_cvref_t<decltype(grid.*ptr)>;
        const auto value = grid.*ptr;
        if constexpr (std::is_same_v<type, bool>) {
            return value ? "true" : "false";
        } else if constexpr (std::is_same_v<type, designer_grid_style>) {
            return value == designer_grid_style::dots ? "dots" : "lines";
        } else if constexpr (std::is_same_v<type, std::uint32_t>) {
            const auto rgba = (value << 8) | (value >> 24);
            char text[10];
            std::snprintf(text, sizeof(text), "#%08X", static_cast<unsigned>(rgba));
            return text;
        } else {
            char text[32];
            const auto end = std::to_chars(text, text + sizeof(text), value);
            return std::string(text, end.ptr);
        }
    }, item.value);
}

// Input here is already canonicalized and range-checked by PropertyGrid.
void set_field(const field& item, settings& grid, std::string_view text)
{
    std::visit([&](auto ptr) {
        using type = std::remove_cvref_t<decltype(grid.*ptr)>;
        if constexpr (std::is_same_v<type, bool>) {
            grid.*ptr = text == "true";
        } else if constexpr (std::is_same_v<type, designer_grid_style>) {
            grid.*ptr = text == "dots" ? designer_grid_style::dots : designer_grid_style::lines;
        } else if constexpr (std::is_same_v<type, std::uint32_t>) {
            std::uint32_t hex = 0;
            const auto parsed = std::from_chars(text.data() + 1, text.data() + text.size(), hex, 16);
            if (parsed.ec == std::errc{})
                grid.*ptr = text.size() == 7 ? 0xFF000000u | hex : (hex << 24) | (hex >> 8);
        } else {
            type value{};
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
            if (parsed.ec == std::errc{}) grid.*ptr = value;
        }
    }, item.value);
}

kit::property_desc describe(const field& item, const designer_strings& strings, const settings& draft)
{
    kit::property_desc desc;
    desc.id = item.id;
    desc.label = strings.text(item.key, item.label);
    desc.type = item.type;
    desc.value = field_text(item, draft);
    desc.default_value = field_text(item, settings{});
    desc.has_default = true;
    desc.category = item.type == property_type::color ? "colors" : "common";
    desc.has_range = item.min < item.max;
    desc.min_value = item.min;
    desc.max_value = item.max;
    if (item.type == property_type::enumeration) {
        desc.choices = {strings.text("options.grid.lines", "Lines"), strings.text("options.grid.dots", "Dots")};
        desc.choice_values = {"lines", "dots"};
    }
    if (item.type == property_type::color)
        desc.tooltip = strings.text("options.grid.color_hint", "Enter #RRGGBB or #RRGGBBAA, or use the swatch.");
    if (item.value == member{&settings::major_every})
        desc.tooltip = strings.text("options.grid.major_hint", "Number of base intervals between major grid lines.");
    if (item.value == member{&settings::hide_below})
        desc.tooltip = strings.text("options.grid.hide_hint", "Hide the grid when on-screen spacing falls below this many pixels.");
    return desc;
}

vgui_widget* make_panel(vgui_widget* parent, vgui_layout_kind layout)
{
    auto* panel = vgui_widget_add(parent, VGUI_PANEL, nullptr);
    vgui_widget_set_layout(panel, layout);
    vgui_widget_set_align_items(panel, VGUI_ALIGN_STRETCH);
    vgui_widget_set_padding(panel, 0, 0, 0, 0);
    return panel;
}

struct options_palette {
    vgui_color chrome, background, navigation, edge, accent, selection_edge, field, hint, error;
};

constexpr options_palette dark_palette{
    0xFF232831, 0xFF1B1E24, 0xFF171A20, 0xFF374150, 0xFF294C72, 0xFF5483B0,
    0xFF15181E, 0xFFABB7C6, 0xFFF08D96,
};
constexpr options_palette light_palette{
    0xFFE7ECF2, 0xFFF7F9FC, 0xFFEFF3F8, 0xFFB8C7D8, 0xFFCCE0F8, 0xFF4677AB,
    0xFFFFFFFF, 0xFF4A5A70, 0xFFA62B39,
};

vgui_widget* add_page_label(vgui_widget* page, const char* id, const std::string& text)
{
    auto* label = vgui_widget_add(page, VGUI_LABEL, text.c_str());
    vgui_widget_set_id(label, id);
    vgui_widget_set_padding(label, 0, 0, 0, 0);
    vgui_widget_set_text_align(label, 0, .5f);
    return label;
}

void add_page_heading(vgui_widget* page, const char* id, const char* text)
{
    auto* heading = add_page_label(page, id, text);
    vgui_widget_set_font_size(heading, 18);
    vgui_widget_set_font_weight(heading, 600);
}

std::string with_value(std::string label, std::string_view value)
{
    constexpr std::string_view token = "{value}";
    if (const auto at = label.find(token); at != std::string::npos)
        label.replace(at, token.size(), value);
    return label;
}

vgui_widget* build_configuration(vgui_widget* page, const designer_strings& strings, designer_dock_widths layout,
    std::string_view settings_path)
{
    vgui_widget_set_padding(page, 18, 16, 18, 16);
    vgui_widget_set_gap(page, 10);
    vgui_panel_set_scrollable(page, 0, 1);
    add_page_heading(page, "designer.options.configuration.title", strings.text("settings.info", "Configuration Information"));
    const auto add_label = [page](const char* id, const std::string& text) {
        return add_page_label(page, id, text);
    };
    const auto describe_side = [&](float width, bool collapsed) {
        char buf[32];
        const auto end = std::to_chars(buf, buf + sizeof(buf), width, std::chars_format::fixed, 1).ptr;
        std::string number(buf, end);
        if (number.ends_with(".0")) number.resize(number.size() - 2);
        const auto* state = collapsed ? strings.text("settings.info.collapsed", "Collapsed")
                                      : strings.text("settings.info.expanded", "Expanded");
        return number + " " + strings.text("settings.info.logical_pixels", "logical px") + " · " + state;
    };
    const auto* language = strings.language() == designer_language::english
        ? strings.text("settings.en-US", "English") : strings.text("settings.zh-CN", "简体中文");
    add_label("designer.options.configuration.language",
        with_value(strings.text("settings.info.language", "Interface language: {value}"), language));
    auto* theme_label = add_label("designer.options.configuration.theme", "");
    add_label("designer.options.configuration.left",
        with_value(strings.text("settings.info.left", "Left sidebar: {value}"), describe_side(layout.left, layout.left_collapsed)));
    add_label("designer.options.configuration.right",
        with_value(strings.text("settings.info.right", "Right sidebar: {value}"), describe_side(layout.right, layout.right_collapsed)));

    const auto* path_label = strings.text("settings.info.path", "Settings file (JSON)");
    auto* label = add_label("designer.options.configuration.path_label", path_label);
    vgui_widget_set_margin(label, 0, 6, 0, 0);
    if (settings_path.empty()) {
        add_label("designer.options.configuration.path", strings.text("settings.info.path_unavailable", "No settings file path available"));
    } else {
        const std::string path(settings_path);
        auto* value = vgui_widget_add(page, VGUI_EDIT, nullptr);
        vgui_widget_set_id(value, "designer.options.configuration.path");
        vgui_widget_set_size(value, 0, 30);
        vgui_widget_set_min_size(value, 0, 0);
        vgui_widget_set_a11y_name(value, path_label);
        vgui_widget_set_tooltip(value, path.c_str());
        vgui_edit_set_text(value, path.c_str());
        vgui_edit_set_readonly(value, 1);
    }
    return theme_label;
}

} // namespace

struct designer_options::impl {
    enum class page { grid, appearance, configuration };
    using save_callback = std::function<bool(std::optional<settings>, std::optional<designer_theme>)>;

    vgui_context* ui = nullptr;
    vgui_widget *panel = nullptr, *title = nullptr, *nav = nullptr, *footer = nullptr;
    vgui_widget *status = nullptr, *hint = nullptr, *info_theme = nullptr;
    vgui_widget *grid_page = nullptr, *appearance_page = nullptr, *info_page = nullptr;
    vgui_widget *grid_nav = nullptr, *appearance_nav = nullptr, *info_nav = nullptr;
    vgui_widget *dark = nullptr, *light = nullptr, *reset = nullptr, *cancel = nullptr, *ok = nullptr;
    page active_page = page::grid;
    int focus_listener = 0;
    bool closing = false;
    std::optional<std::size_t> pending_inline;
    std::unique_ptr<kit::property_grid> grid;
    settings initial, draft;
    designer_theme initial_theme = default_designer_theme, draft_theme = default_designer_theme;
    std::function<void(settings)> preview_grid;
    std::function<void(designer_theme)> preview_theme;
    save_callback save;
    const char* save_error = nullptr;
    const char* info_theme_format = nullptr;

    ~impl()
    {
        if (panel && vgui_modal_panel(ui) == panel)
            vgui_modal_close(ui, VGUI_MODAL_RESULT_CANCEL);
    }

    void update(settings next)
    {
        if (draft == next) return;
        draft = next;
        if (preview_grid) preview_grid(draft);
    }

    void apply(std::size_t i, const std::string& canonical)
    {
        if (closing) return;
        auto next = draft;
        set_field(fields[i], next, canonical);
        update(next);
    }

    void finish_inline()
    {
        if (!pending_inline) return;
        const auto i = *std::exchange(pending_inline, std::nullopt);
        const auto& item = fields[i];
        (void)grid->set_property_value(item.id, field_text(item, draft));
    }

    bool inline_edit(const std::string& id, const std::string& text, edit_event event)
    {
        if (closing) return true;
        const auto i = field_index(id);
        if (!i) return false;
        if (event != edit_event::change) {
            finish_inline();
            return true;
        }
        pending_inline = i;
        if (fields[*i].type == property_type::color) {
            // Treat partial RGB/RGBA as intermediate text, not shorthand colors.
            const auto begin = text.find_first_not_of(" \t\r\n");
            if (begin == std::string::npos) return true;
            const auto digits = begin + (text[begin] == '#' ? 1u : 0u);
            const auto end = text.find_last_not_of(" \t\r\n") + 1;
            if (end != digits + 6 && end != digits + 8) return true;
        }
        const auto& item = fields[*i];
        // Validate before formatting: PropertyGrid normally clamps numbers,
        // which would turn an invalid intermediate edit into a valid preview.
        if (item.min < item.max) {
            const auto first = text.find_first_not_of(" \t\r\n");
            if (first == std::string::npos) return true;
            const char* begin = text.data() + first;
            const char* end = text.data() + text.find_last_not_of(" \t\r\n") + 1;
            if (*begin == '+') ++begin;
            double number = 0;
            const auto parsed = std::from_chars(begin, end, number);
            if (parsed.ec != std::errc{} || parsed.ptr != end || !(number >= item.min && number <= item.max))
                return true;
        }
        kit::property_desc input;
        input.type = item.type;
        const auto value = kit::property_normalize_value(input, text);
        if (value.ok) apply(*i, value.value);
        return true;
    }

    static void on_focus(vgui_context*, vgui_widget*, vgui_widget* next, void* user)
    {
        auto& self = *static_cast<impl*>(user);
        if (self.closing || !self.pending_inline || vgui_modal_panel(self.ui) != self.panel) return;
        const auto& item = fields[*self.pending_inline];
        if (next && next == self.grid->property_editor(item.id)) return;
        // A filtered grid can clear focus while virtualizing rows. Avoid
        // changing its search results from inside that layout pass.
        if (!next && !self.grid->filter().empty()) return;
        self.finish_inline();
    }

    const options_palette& palette() const
    {
        return draft_theme == designer_theme::light ? light_palette : dark_palette;
    }

    void style_navigation()
    {
        const auto& colors = palette();
        const auto select = [&colors](vgui_widget* item, bool selected) {
            vgui_widget_set_background(item, selected ? colors.accent : colors.navigation);
            vgui_widget_set_border(item, selected ? colors.selection_edge : 0, selected ? 1 : 0);
        };
        select(grid_nav, active_page == page::grid);
        select(appearance_nav, active_page == page::appearance);
        select(info_nav, active_page == page::configuration);
    }

    void style_dialog()
    {
        const bool light_theme = draft_theme == designer_theme::light;
        const auto& colors = palette();
        vgui_widget_set_background(panel, colors.background);
        vgui_widget_set_border(panel, colors.edge, 1);
        vgui_widget_set_background(title, colors.chrome);
        vgui_widget_set_background(nav, colors.navigation);
        vgui_widget_set_background(footer, colors.chrome);
        vgui_widget_set_background(info_page, colors.background);
        vgui_widget_set_background(appearance_page, colors.background);
        vgui_widget_set_background(ok, colors.accent);
        vgui_widget_set_text_color(hint, colors.hint);
        vgui_widget_set_text_color(status, colors.error);
        const auto theme_summary = with_value(info_theme_format, vgui_widget_get_text(light_theme ? light : dark));
        vgui_widget_set_text(info_theme, theme_summary.c_str());
        const auto select_theme = [&colors](vgui_widget* item, bool selected) {
            vgui_widget_set_checked(item, selected);
            vgui_widget_set_background(item, selected ? colors.accent : colors.navigation);
            vgui_widget_set_border(item, selected ? colors.selection_edge : colors.edge, 1);
        };
        select_theme(dark, !light_theme);
        select_theme(light, light_theme);
        style_navigation();
        grid->set_light_theme(light_theme);
        grid->set_edit_field_background(colors.field);
    }

    void update_theme(designer_theme next)
    {
        if (draft_theme == next) return;
        draft_theme = next;
        if (preview_theme) preview_theme(draft_theme);
        style_dialog();
    }

    void show_page(page next)
    {
        finish_inline();
        active_page = next;
        vgui_widget_set_visible(grid_page, next == page::grid);
        vgui_widget_set_visible(appearance_page, next == page::appearance);
        vgui_widget_set_visible(info_page, next == page::configuration);
        vgui_widget_set_enabled(reset, next != page::configuration);
        style_navigation();
    }

    void on_button(vgui_widget* button)
    {
        if (button == grid_nav) {
            show_page(page::grid);
        } else if (button == appearance_nav) {
            show_page(page::appearance);
        } else if (button == info_nav) {
            show_page(page::configuration);
        } else if (button == dark || button == light) {
            update_theme(button == light ? designer_theme::light : designer_theme::dark);
        } else if (button == reset) {
            if (active_page == page::grid) {
                pending_inline.reset();
                update(settings{});
                grid->refresh();
            } else if (active_page == page::appearance) {
                update_theme(default_designer_theme);
            }
        } else if (button == ok) {
            finish_inline();
            const auto changed_grid = draft == initial ? std::nullopt : std::optional{draft};
            const auto changed_theme = draft_theme == initial_theme ? std::nullopt : std::optional{draft_theme};
            if ((changed_grid || changed_theme) && save && !save(changed_grid, changed_theme)) {
                vgui_widget_set_text(status, save_error);
                vgui_widget_set_visible(status, 1);
            } else {
                vgui_modal_close(ui, VGUI_MODAL_RESULT_CONFIRM);
            }
        } else if (button == cancel) {
            vgui_modal_close(ui, VGUI_MODAL_RESULT_CANCEL);
        }
    }

    vgui_widget* button(vgui_widget* parent, const char* id, const char* text, float width)
    {
        auto* w = vgui_widget_add(parent, VGUI_BUTTON, text);
        vgui_widget_set_id(w, id);
        vgui_widget_set_size(w, width, 30);
        vgui_widget_on_event(w, [](vgui_widget* widget, const vgui_event* event, void* user) {
            if (event->type == VGUI_EVENT_CLICK) static_cast<impl*>(user)->on_button(widget);
        }, this);
        return w;
    }

    void teardown()
    {
        closing = true;
        vgui_context_remove_focus_listener(ui, std::exchange(focus_listener, 0));
        pending_inline.reset();
        // The kit closes its managed popup and unsets its context callback.
        // Keep the panel alive until its grid has been destroyed.
        grid.reset();
        panel = title = nav = footer = status = hint = info_theme = nullptr;
        grid_page = appearance_page = info_page = grid_nav = appearance_nav = info_nav = nullptr;
        dark = light = reset = cancel = ok = nullptr;
        ui = nullptr;
        closing = false;
    }

    static void on_close(vgui_context*, vgui_widget*, int result, void* user)
    {
        auto& self = *static_cast<impl*>(user);
        const auto initial = self.initial;
        const auto initial_theme = self.initial_theme;
        const bool cancel = result != VGUI_MODAL_RESULT_CONFIRM;
        const bool rollback_grid = cancel && self.draft != initial;
        const bool rollback_theme = cancel && self.draft_theme != initial_theme;
        auto preview_grid = std::move(self.preview_grid);
        auto preview_theme = std::move(self.preview_theme);
        self.save = {};
        self.teardown();
        if (rollback_theme && preview_theme) preview_theme(initial_theme);
        if (rollback_grid && preview_grid) preview_grid(initial);
    }

    void show(vgui_context* ctx, const designer_strings& strings, settings start, designer_theme theme,
        designer_dock_widths layout, std::string_view settings_path,
        std::function<void(settings)> preview_grid_fn, std::function<void(designer_theme)> preview_theme_fn,
        save_callback save_fn)
    {
        if (!ctx || panel || vgui_modal_active(ctx)) return;
        ui = ctx;
        initial = draft = start;
        initial_theme = draft_theme = theme;
        preview_grid = std::move(preview_grid_fn);
        preview_theme = std::move(preview_theme_fn);
        save = std::move(save_fn);
        save_error = strings.text("options.save_failed", "Could not save settings.");

        panel = vgui_widget_create(ui, VGUI_PANEL, nullptr);
        vgui_widget_set_id(panel, "designer.options");
        vgui_widget_set_a11y_name(panel, strings.text("options.title", "Options"));
        vgui_widget_set_layout(panel, VGUI_LAYOUT_COLUMN);
        vgui_widget_set_padding(panel, 0, 0, 0, 0);
        vgui_widget_set_align_items(panel, VGUI_ALIGN_STRETCH);
        vgui_widget_set_size(panel, 750, 565);
        vgui_widget_set_rounding(panel, 4);

        title = vgui_widget_add(panel, VGUI_LABEL, strings.text("options.title", "Options"));
        vgui_widget_set_id(title, "designer.options.title");
        vgui_widget_set_size(title, 0, 38);
        vgui_widget_set_padding(title, 14, 0, 14, 0);
        vgui_widget_set_text_align(title, 0, .5f);
        vgui_widget_set_font_size(title, 16);
        vgui_widget_set_font_weight(title, 600);

        auto* body = make_panel(panel, VGUI_LAYOUT_ROW);
        vgui_widget_set_grow(body, 1);
        nav = make_panel(body, VGUI_LAYOUT_COLUMN);
        vgui_widget_set_id(nav, "designer.options.navigation");
        vgui_widget_set_size(nav, strings.language() == designer_language::english ? 220 : 132, 0);
        vgui_widget_set_padding(nav, 8, 8, 8, 8);
        auto* canvas = vgui_widget_add(nav, VGUI_LABEL, strings.text("options.canvas", "Canvas"));
        vgui_widget_set_size(canvas, 0, 28);
        vgui_widget_set_padding(canvas, 6, 0, 6, 0);
        vgui_widget_set_text_align(canvas, 0, .5f);
        vgui_widget_set_font_weight(canvas, 600);
        const auto add_nav = [&](const char* id, const char* label, float indent) {
            auto* item = button(nav, id, label, 0);
            vgui_widget_set_padding(item, indent, 0, 6, 0);
            vgui_widget_set_text_align(item, 0, .5f);
            vgui_widget_set_border(item, 0, 0);
            return item;
        };
        grid_nav = add_nav("designer.options.nav.grid", strings.text("options.grid", "Grid"), 18);
        appearance_nav = add_nav("designer.options.nav.appearance", strings.text("options.appearance", "Appearance"), 6);
        vgui_widget_set_margin(appearance_nav, 0, 6, 0, 0);
        info_nav = add_nav("designer.options.nav.configuration", strings.text("settings.info", "Configuration Information"), 6);
        vgui_widget_set_margin(info_nav, 0, 6, 0, 0);

        grid_page = make_panel(body, VGUI_LAYOUT_COLUMN);
        vgui_widget_set_id(grid_page, "designer.options.grid");
        vgui_widget_set_grow(grid_page, 1);
        appearance_page = make_panel(body, VGUI_LAYOUT_COLUMN);
        vgui_widget_set_id(appearance_page, "designer.options.appearance");
        vgui_widget_set_grow(appearance_page, 1);
        vgui_widget_set_padding(appearance_page, 18, 16, 18, 16);
        vgui_widget_set_gap(appearance_page, 10);
        add_page_heading(appearance_page, "designer.options.appearance.title", strings.text("options.appearance", "Appearance"));
        add_page_label(appearance_page, "designer.options.theme", strings.text("settings.theme", "Theme"));
        auto* choices = make_panel(appearance_page, VGUI_LAYOUT_ROW);
        vgui_widget_set_gap(choices, 8);
        vgui_widget_set_background(choices, 0);
        dark = button(choices, "designer.options.theme.dark", strings.text("settings.theme.dark", "Dark"), 100);
        light = button(choices, "designer.options.theme.light", strings.text("settings.theme.light", "Light"), 100);
        vgui_widget_set_a11y_role(dark, VGUI_A11Y_ROLE_TOGGLE);
        vgui_widget_set_a11y_role(light, VGUI_A11Y_ROLE_TOGGLE);
        hint = add_page_label(appearance_page, "designer.options.theme.hint",
            strings.text("options.theme_hint", "Preview the theme for the entire editor."));

        info_page = make_panel(body, VGUI_LAYOUT_COLUMN);
        vgui_widget_set_id(info_page, "designer.options.configuration");
        vgui_widget_set_grow(info_page, 1);
        info_theme = build_configuration(info_page, strings, layout, settings_path);
        info_theme_format = strings.text("settings.info.theme", "Interface theme: {value}");
        grid = std::make_unique<kit::property_grid>();
        grid->build_with_labels(grid_page, {
            .properties = strings.text("options.grid", "Grid"),
            .search = strings.text("options.search", "Search settings"),
            .favorites_only = strings.text("workbench.inspector.favorites_only", "Favorites only"),
            .toggle_favorite = strings.text("workbench.inspector.toggle_favorite", "Toggle favorite"),
            .toggle_favorite_hint = strings.text("workbench.inspector.toggle_favorite_hint", "Toggle favorite"),
            .reset_button = strings.text("workbench.inspector.reset_button", "R"),
            .reset_to_default = strings.text("workbench.inspector.reset_to_default", "Reset to default"),
            .categories = {{"common", strings.text("options.common", "Common")},
                {"colors", strings.text("options.colors", "Colors")}},
            .pick_color = strings.text("workbench.inspector.font.pick_color", "Pick color"),
        });
        for (const auto& item : fields)
            grid->add_property(describe(item, strings, draft));
        grid->set_property_favorite("visible", true);
        grid->set_property_favorite("spacing", true);
        grid->set_value_source([this](const std::string& id, std::string& out) {
            const auto index = field_index(id);
            if (!index) return false;
            out = field_text(fields[*index], draft);
            return true;
        });
        grid->set_change_listener([this](const std::string& id, const std::string& canonical) {
            if (const auto i = field_index(id)) apply(*i, canonical);
        });
        grid->set_text_edit_listener([this](vgui_widget*, const std::string& id, const std::string& text, edit_event event) {
            return inline_edit(id, text, event);
        });
        grid->set_color_popup_edit_listener([this](vgui_widget*, const std::string& id, const std::string& text, edit_event) {
            // The picker sends its opening color on cancel, not the form snapshot.
            if (const auto i = field_index(id)) apply(*i, text);
            return true;
        });

        footer = make_panel(panel, VGUI_LAYOUT_ROW);
        vgui_widget_set_size(footer, 0, 50);
        vgui_widget_set_padding(footer, 12, 6, 12, 6);
        vgui_widget_set_gap(footer, 8);
        vgui_widget_set_align_items(footer, VGUI_ALIGN_CENTER);
        reset = button(footer, "designer.options.reset", strings.text("options.reset", "Restore defaults"), 130);
        auto* status_slot = make_panel(footer, VGUI_LAYOUT_COLUMN);
        vgui_widget_set_grow(status_slot, 1);
        vgui_widget_set_background(status_slot, 0);
        status = vgui_widget_add(status_slot, VGUI_LABEL, "");
        vgui_widget_set_id(status, "designer.options.status");
        vgui_widget_set_visible(status, 0);
        cancel = button(footer, "designer.options.cancel", strings.text("common.cancel", "Cancel"), 76);
        ok = button(footer, "designer.options.ok", strings.text("common.ok", "OK"), 76);
        show_page(page::grid);
        style_dialog();

        vgui_modal_options options;
        vgui_modal_options_default(&options);
        options.destroy_on_close = 1;
        options.on_close = &on_close;
        options.user = this;
        if (!vgui_modal_show(ui, panel, &options)) {
            auto* failed_panel = panel;
            teardown();
            vgui_widget_destroy(failed_panel);
            preview_grid = {};
            preview_theme = {};
            save = {};
            return;
        }
        vgui_modal_set_drag_handle(ui, title);
        vgui_set_focus(ui, ok); // Leave the search placeholder visible on open.
        focus_listener = vgui_context_add_focus_listener(ui, &on_focus, this);
    }
};

designer_options::designer_options() : impl_(std::make_unique<impl>()) {}
designer_options::~designer_options() = default;

void designer_options::show(vgui_context* ctx, const designer_strings& strings, settings initial, designer_theme theme,
    designer_dock_widths layout, std::string_view settings_path,
    std::function<void(settings)> preview_grid, std::function<void(designer_theme)> preview_theme,
    std::function<bool(std::optional<settings>, std::optional<designer_theme>)> save)
{
    impl_->show(ctx, strings, initial, theme, layout, settings_path,
        std::move(preview_grid), std::move(preview_theme), std::move(save));
}

} // namespace vgui::ui_design
