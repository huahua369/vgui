#pragma once

#include "model.h"

#include <cmath>
#include <vgui/vgui.h>

namespace vgui::ui_design::detail {

inline std::optional<vgui_widget_kind> primitive_kind(std::string_view type)
{
    if (type == "checkbox") return VGUI_CHECKBOX;
    if (type == "radio") return VGUI_RADIO;
    if (type == "switch") return VGUI_SWITCH;
    if (type == "slider") return VGUI_SLIDER;
    if (type == "progress") return VGUI_PROGRESS;
    if (type == "separator") return VGUI_SEPARATOR;
    return std::nullopt;
}

inline vgui_align_content primitive_content_alignment(std::string_view value)
{
    if (value == "center") return VGUI_ALIGN_CONTENT_CENTER;
    if (value == "end") return VGUI_ALIGN_CONTENT_END;
    if (value == "space-between") return VGUI_ALIGN_CONTENT_SPACE_BETWEEN;
    if (value == "space-around") return VGUI_ALIGN_CONTENT_SPACE_AROUND;
    if (value == "space-evenly") return VGUI_ALIGN_CONTENT_SPACE_EVENLY;
    if (value == "stretch") return VGUI_ALIGN_CONTENT_STRETCH;
    return VGUI_ALIGN_CONTENT_START;
}

inline vgui_image_fit primitive_image_fit(std::string_view value)
{
    if (value == "contain") return VGUI_IMAGE_FIT_CONTAIN;
    if (value == "cover") return VGUI_IMAGE_FIT_COVER;
    if (value == "none") return VGUI_IMAGE_FIT_NONE;
    return VGUI_IMAGE_FIT_STRETCH;
}

// These setters interpret zero as inheritance, but a document can also supply
// explicit transparent black. Keep its alpha zero and set a low RGB bit.
inline vgui_color primitive_theme_override(std::uint32_t color) { return color == 0 ? 1u : color; }

template<class Access>
void apply_primitive_text_alignment(vgui_widget* widget, const Access& props)
{
    if (!props.has("text_align_x") && !props.has("text_align_y")) return;
    const auto& x = props.text("text_align_x");
    const auto& y = props.text("text_align_y");
    vgui_widget_set_text_align(widget, x == "start" ? 0.0f : x == "end" ? 1.0f : 0.5f,
        y == "top" ? 0.0f : y == "bottom" ? 1.0f : 0.5f);
}

// Apply after the caller resolves a same-context button icon texture.
// A missing texture leaves the newly created button's native defaults alone.
template<class Access>
void apply_button_icon(vgui_widget* widget, const Access& props, vgui_texture* texture)
{
    if (!texture) return;
    const auto& position = props.text("icon_position");
    const auto pos = position == "right" ? VGUI_BUTTON_ICON_RIGHT : position == "top" ? VGUI_BUTTON_ICON_TOP :
        position == "bottom" ? VGUI_BUTTON_ICON_BOTTOM : VGUI_BUTTON_ICON_LEFT;
    vgui_button_set_icon(widget, texture, static_cast<int>(std::lround(props.number("icon_width"))),
        static_cast<int>(std::lround(props.number("icon_height"))), pos);
    if (props.has("icon_gap")) vgui_button_set_icon_gap(widget, props.number("icon_gap"));
    if (const auto color = props.color("icon_tint")) vgui_button_set_icon_tint(widget, primitive_theme_override(*color));
}

// Apply after common size/layout, to a newly created widget. The caller owns
// localized text, font/resource resolution and generic widget appearance.
template<class Access>
void apply_primitive(vgui_widget* widget, const node& n, const Access& props)
{
    const std::string_view type = n.type;
    if (type == "panel") {
        if (props.has("align_content"))
            vgui_widget_set_align_content(widget, primitive_content_alignment(props.text("align_content")));
        if (props.has("scrollable_x") || props.has("scrollable_y"))
            vgui_panel_set_scrollable(widget, props.boolean("scrollable_x"), props.boolean("scrollable_y"));
        if (props.has("scrollbar_width_x") || props.has("scrollbar_width_y") || props.has("scrollbar_inset"))
            vgui_panel_set_scrollbar_size(widget, props.number("scrollbar_width_x"),
                props.number("scrollbar_width_y"), props.number("scrollbar_inset"));
    } else if (type == "label") {
        apply_primitive_text_alignment(widget, props);
    } else if (type == "button") {
        apply_primitive_text_alignment(widget, props);
        if (props.has("button_style")) vgui_button_set_style(widget,
            props.text("button_style") == "gradient" ? VGUI_BUTTON_STYLE_GRADIENT : VGUI_BUTTON_STYLE_SOLID);
        if (props.has("checked")) vgui_widget_set_checked(widget, props.boolean("checked"));
    } else if (type == "edit") {
        if (props.has("multiline")) vgui_edit_set_multiline(widget, props.boolean("multiline"));
        if (props.has("readonly")) vgui_edit_set_readonly(widget, props.boolean("readonly"));
        if (props.has("password")) vgui_edit_set_password(widget, props.boolean("password") ? '*' : '\0');
        if (props.has("background_transparent"))
            vgui_edit_set_background_transparent(widget, props.boolean("background_transparent"));
        if (const auto color = props.color("selection_color"))
            vgui_edit_set_selection_color(widget, primitive_theme_override(*color));
        if (const auto radius = props.number("selection_rounding"); radius >= 0)
            vgui_edit_set_selection_rounding(widget, radius);
        if (const auto width = props.number("caret_width"); width >= 0)
            vgui_edit_set_caret_width(widget, width);
    } else if (type == "image") {
        const auto pixel = [&](std::string_view key) { return static_cast<int>(std::lround(props.number(key))); };
        if (props.has("fit")) vgui_image_set_fit(widget, primitive_image_fit(props.text("fit")));
        if (props.has("source_x") || props.has("source_y") || props.has("source_width") || props.has("source_height"))
            vgui_image_set_source(widget, pixel("source_x"), pixel("source_y"), pixel("source_width"), pixel("source_height"));
        if (const auto color = props.color("tint")) vgui_image_set_tint(widget, *color);
        if (props.has("flip_x") || props.has("flip_y"))
            vgui_image_set_flip(widget, props.boolean("flip_x"), props.boolean("flip_y"));
        if (props.has("pixel_snap")) vgui_image_set_pixel_snap(widget, props.boolean("pixel_snap"));
        if (props.has("rotation")) vgui_image_set_rotation(widget, props.number("rotation"));
        if (props.has("slice_left") || props.has("slice_top") || props.has("slice_right") || props.has("slice_bottom"))
            vgui_image_set_nine_slice(widget, pixel("slice_left"), pixel("slice_top"), pixel("slice_right"), pixel("slice_bottom"));
        if (props.has("rotation_center_enabled") && props.boolean("rotation_center_enabled"))
            vgui_image_set_rotation_center(widget, props.number("rotation_center_x"), props.number("rotation_center_y"), 1);
        if (props.has("matrix_enabled") && props.boolean("matrix_enabled")) {
            const float matrix[] = {props.number("matrix_xx"), props.number("matrix_yx"),
                props.number("matrix_xy"), props.number("matrix_yy"), props.number("matrix_tx"), props.number("matrix_ty")};
            vgui_image_set_matrix(widget, matrix);
        }
    } else if (type == "checkbox" || type == "radio" || type == "switch") {
        apply_primitive_text_alignment(widget, props);
        if (type == "radio" && props.has("radio_group"))
            vgui_radio_set_group(widget, props.text("radio_group").c_str());
        if (props.has("checked")) vgui_widget_set_checked(widget, props.boolean("checked"));
        if (const auto color = props.color("accent_color"))
            vgui_widget_set_accent(widget, primitive_theme_override(*color));
    } else if (type == "slider" || type == "progress") {
        // The C setter accepts values in range units. Install range and step
        // first; the setter clamps and quantizes against the newly set range.
        vgui_widget_set_range(widget, props.number("min_value"), props.number("max_value"));
        vgui_widget_set_step(widget, props.number("step"));
        vgui_widget_set_value(widget, props.number("value"));
        if (props.has("vertical")) vgui_widget_set_vertical(widget, props.boolean("vertical"));
        auto accent = props.color("accent_color");
        if (!accent && type == "progress") {
            // Clearing the inspector's color restores the progress default.
            const auto& fallback = std::get<std::string>(find_property(type, "accent_color")->default_value);
            accent = parse_text_color(fallback);
        }
        if (accent) vgui_widget_set_accent(widget, primitive_theme_override(*accent));
        if (const auto color = props.color("track_color"))
            vgui_widget_set_track(widget, primitive_theme_override(*color));
        if (type == "progress") {
            apply_primitive_text_alignment(widget, props);
            if (props.has("radial")) vgui_widget_set_radial(widget, props.boolean("radial"));
            if (props.has("show_text")) vgui_widget_set_progress_text_visible(widget, props.boolean("show_text"));
        }
    } else if (type == "separator") {
        const auto thickness = props.number("thickness");
        if (thickness > 0 && props.number("height") == 0)
            vgui_widget_set_size(widget, props.number("width"), thickness);
    }
}

} // namespace vgui::ui_design::detail
