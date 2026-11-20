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

constexpr vgui_color chrome = 0xFF232831;
constexpr vgui_color dialog_background = 0xFF1B1E24;
constexpr vgui_color edge = 0xFF374150;
constexpr vgui_color accent = 0xFF294C72;

} // namespace

struct designer_options::impl {
    vgui_context* ui = nullptr;
    vgui_widget* panel = nullptr;
    vgui_widget* status = nullptr;
    int focus_listener = 0;
    bool closing = false;
    std::optional<std::size_t> pending_inline;
    std::unique_ptr<kit::property_grid> grid;
    settings initial, draft;
    std::function<void(settings)> preview;
    std::function<bool(settings)> save;
    const char* save_error = nullptr;

    ~impl()
    {
        if (panel && vgui_modal_panel(ui) == panel)
            vgui_modal_close(ui, VGUI_MODAL_RESULT_CANCEL);
    }

    void update(settings next)
    {
        if (draft == next) return;
        draft = next;
        if (preview) preview(draft);
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

    void on_button(vgui_widget* button)
    {
        const std::string_view id = vgui_widget_get_id(button);
        if (id == "designer.options.cancel") {
            vgui_modal_close(ui, VGUI_MODAL_RESULT_CANCEL);
        } else if (id == "designer.options.reset") {
            pending_inline.reset();
            update(settings{});
            grid->refresh();
        } else if (id == "designer.options.ok") {
            finish_inline();
            if (save && !save(draft)) {
                vgui_widget_set_text(status, save_error);
                vgui_widget_set_visible(status, 1);
            } else {
                vgui_modal_close(ui, VGUI_MODAL_RESULT_CONFIRM);
            }
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
        panel = status = nullptr;
        ui = nullptr;
        closing = false;
    }

    static void on_close(vgui_context*, vgui_widget*, int result, void* user)
    {
        auto& self = *static_cast<impl*>(user);
        const auto initial = self.initial;
        const bool rollback = result != VGUI_MODAL_RESULT_CONFIRM && self.draft != initial;
        auto preview = std::move(self.preview);
        self.save = {};
        self.teardown();
        if (rollback && preview) preview(initial);
    }

    void show(vgui_context* ctx, const designer_strings& strings, settings start,
        std::function<void(settings)> preview_fn, std::function<bool(settings)> save_fn)
    {
        if (!ctx || panel || vgui_modal_active(ctx)) return;
        ui = ctx;
        initial = draft = start;
        preview = std::move(preview_fn);
        save = std::move(save_fn);
        save_error = strings.text("options.save_failed", "Could not save grid settings.");

        panel = vgui_widget_create(ui, VGUI_PANEL, nullptr);
        vgui_widget_set_id(panel, "designer.options");
        vgui_widget_set_a11y_name(panel, strings.text("options.title", "Options"));
        vgui_widget_set_layout(panel, VGUI_LAYOUT_COLUMN);
        vgui_widget_set_padding(panel, 0, 0, 0, 0);
        vgui_widget_set_align_items(panel, VGUI_ALIGN_STRETCH);
        vgui_widget_set_size(panel, 750, 565);
        vgui_widget_set_background(panel, dialog_background);
        vgui_widget_set_border(panel, edge, 1);
        vgui_widget_set_rounding(panel, 4);

        auto* title = vgui_widget_add(panel, VGUI_LABEL, strings.text("options.title", "Options"));
        vgui_widget_set_id(title, "designer.options.title");
        vgui_widget_set_size(title, 0, 38);
        vgui_widget_set_padding(title, 14, 0, 14, 0);
        vgui_widget_set_text_align(title, 0, .5f);
        vgui_widget_set_font_size(title, 16);
        vgui_widget_set_font_weight(title, 600);
        vgui_widget_set_background(title, chrome);

        auto* body = make_panel(panel, VGUI_LAYOUT_ROW);
        vgui_widget_set_grow(body, 1);
        auto* nav = make_panel(body, VGUI_LAYOUT_COLUMN);
        vgui_widget_set_id(nav, "designer.options.navigation");
        vgui_widget_set_size(nav, 132, 0);
        vgui_widget_set_background(nav, 0xFF171A20);
        vgui_widget_set_padding(nav, 8, 8, 8, 8);
        auto* canvas = vgui_widget_add(nav, VGUI_LABEL, strings.text("options.canvas", "Canvas"));
        vgui_widget_set_size(canvas, 0, 28);
        vgui_widget_set_padding(canvas, 6, 0, 6, 0);
        vgui_widget_set_text_align(canvas, 0, .5f);
        vgui_widget_set_font_weight(canvas, 600);
        auto* selected = vgui_widget_add(nav, VGUI_LABEL, strings.text("options.grid", "Grid"));
        vgui_widget_set_size(selected, 0, 28);
        vgui_widget_set_padding(selected, 18, 0, 6, 0);
        vgui_widget_set_text_align(selected, 0, .5f);
        vgui_widget_set_background(selected, accent);

        auto* area = make_panel(body, VGUI_LAYOUT_COLUMN);
        vgui_widget_set_grow(area, 1);
        grid = std::make_unique<kit::property_grid>();
        grid->build_with_labels(area, {
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
        grid->set_edit_field_background(0xFF15181E);
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

        auto* footer = make_panel(panel, VGUI_LAYOUT_ROW);
        vgui_widget_set_size(footer, 0, 50);
        vgui_widget_set_padding(footer, 12, 6, 12, 6);
        vgui_widget_set_gap(footer, 8);
        vgui_widget_set_align_items(footer, VGUI_ALIGN_CENTER);
        vgui_widget_set_background(footer, chrome);
        button(footer, "designer.options.reset", strings.text("options.reset", "Restore defaults"), 130);
        auto* status_slot = make_panel(footer, VGUI_LAYOUT_COLUMN);
        vgui_widget_set_grow(status_slot, 1);
        status = vgui_widget_add(status_slot, VGUI_LABEL, "");
        vgui_widget_set_text_color(status, 0xFFF08D96);
        vgui_widget_set_visible(status, 0);
        button(footer, "designer.options.cancel", strings.text("common.cancel", "Cancel"), 76);
        auto* ok = button(footer, "designer.options.ok", strings.text("common.ok", "OK"), 76);
        vgui_widget_set_background(ok, accent);

        vgui_modal_options options;
        vgui_modal_options_default(&options);
        options.destroy_on_close = 1;
        options.on_close = &on_close;
        options.user = this;
        if (!vgui_modal_show(ui, panel, &options)) {
            auto* failed_panel = panel;
            teardown();
            vgui_widget_destroy(failed_panel);
            preview = {};
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

void designer_options::show(vgui_context* ctx, const designer_strings& strings, settings initial,
    std::function<void(settings)> preview, std::function<bool(settings)> save)
{
    impl_->show(ctx, strings, initial, std::move(preview), std::move(save));
}

} // namespace vgui::ui_design
