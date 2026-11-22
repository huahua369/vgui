#include "runtime.h"
#include "primitive_runtime.h"
#include "collection_runtime.h"
#include "container_runtime.h"
#include "../vgui_internal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <system_error>

namespace vgui::ui_design {
namespace {
const value& property(const node& n, std::string_view key) {
    auto it = n.props.find(key);
    return it == n.props.end() ? find_property(n.type, key)->default_value : it->second;
}
float number(const node& n, std::string_view key) { return static_cast<float>(std::get<double>(property(n, key))); }
const std::string& text(const node& n, std::string_view key) { return std::get<std::string>(property(n, key)); }
bool boolean(const node& n, std::string_view key) { return std::get<bool>(property(n, key)); }

struct widget_properties {
    const node& item;
    bool has(std::string_view key) const { return item.props.contains(key); }
    float number(std::string_view key) const { return ui_design::number(item, key); }
    bool boolean(std::string_view key) const { return ui_design::boolean(item, key); }
    const std::string& text(std::string_view key) const { return ui_design::text(item, key); }
    std::optional<std::uint32_t> color(std::string_view key) const { return parse_text_color(text(key)); }
};

struct gradient_paint {
    vgui_gradient_desc desc{};
    std::vector<vgui_gradient_stop> stops;
};

bool read_gradient(gradient_paint& result, const widget_properties& props) {
    static_assert(max_gradient_stops == VGUI_GRADIENT_MAX_STOPS);
    auto& paint = result.desc;
    vgui_gradient_desc_default(&paint);
    const auto& kind = props.text("gradient_kind");
    const auto& spread = props.text("gradient_extend");
    paint.kind = kind == "radial" ? VGUI_GRADIENT_RADIAL : kind == "sweep" ? VGUI_GRADIENT_SWEEP : VGUI_GRADIENT_LINEAR;
    paint.extend = spread == "repeat" ? VGUI_GRADIENT_REPEAT : spread == "reflect" ? VGUI_GRADIENT_REFLECT : VGUI_GRADIENT_PAD;
    paint.x0 = props.number(kind == "linear" ? "gradient_x0" : "gradient_center_x");
    paint.y0 = props.number(kind == "linear" ? "gradient_y0" : "gradient_center_y");
    paint.x1 = props.number(kind == "linear" ? "gradient_x1" : "gradient_outer_x");
    paint.y1 = props.number(kind == "linear" ? "gradient_y1" : "gradient_outer_y");
    paint.radius0 = props.number("gradient_radius0");
    paint.radius1 = props.number("gradient_radius1");
    paint.ellipse = props.boolean("gradient_ellipse");
    paint.start_degrees = props.number("gradient_angle0");
    paint.end_degrees = props.number("gradient_angle1");
    constexpr std::string_view affine[] = {"gradient_affine_xx", "gradient_affine_yx", "gradient_affine_xy",
        "gradient_affine_yy", "gradient_affine_tx", "gradient_affine_ty"};
    for (std::size_t i = 0; i < std::size(affine); ++i) paint.affine[i] = props.number(affine[i]);
    std::vector<gradient_stop> decoded;
    std::string error;
    if (!decode_gradient_stops(props.text("gradient_stops"), decoded, error)) return false;
    result.stops.reserve(decoded.size());
    for (const auto& stop : decoded) result.stops.push_back({stop.offset, *parse_text_color(stop.color)});
    return true;
}

using linear_color = std::array<float, 4>;
linear_color linear_rgba(vgui_color c) {
    const auto linear_channel = [](unsigned x) {
        const float srgb = float(x & 255u) / 255.f;
        return srgb <= .04045f ? srgb / 12.92f : std::pow((srgb + .055f) / 1.055f, 2.4f);
    };
    return {linear_channel(c >> 16), linear_channel(c >> 8), linear_channel(c), float(c >> 24) / 255.f};
}

std::uint32_t color_byte(float x) {
    return static_cast<std::uint32_t>(std::lround(std::clamp(x, 0.f, 1.f) * 255.f));
}

std::uint32_t srgb_byte(float linear) {
    return color_byte(linear <= .0031308f ? 12.92f * linear : 1.055f * std::pow(linear, 1.f / 2.4f) - .055f);
}

vgui_color fade_color(const linear_color& source, const linear_color& target, float t) {
    // Keep the original alpha envelope. A partially transparent target can
    // dim a highlight, but can never turn its transparent endpoints opaque.
    const float target_alpha = source[3] * target[3];
    const float alpha = std::lerp(source[3], target_alpha, t);
    std::uint32_t packed = color_byte(alpha);
    for (int i = 0; i < 3; ++i) {
        const float channel = alpha > 0.f ? std::lerp(source[i] * source[3], target[i] * target_alpha, t) / alpha
            : std::lerp(source[i], target[i], t);
        packed = (packed << 8) | srgb_byte(channel);
    }
    return packed;
}

struct gradient_key_stop {
    vgui_gradient_stop authored;
    linear_color premultiplied;
};

gradient_key_stop key_stop(vgui_gradient_stop stop) {
    auto linear = linear_rgba(stop.color);
    for (int i = 0; i < 3; ++i) linear[i] *= linear[3];
    return {stop, linear};
}

vgui_gradient_stop blend_key_stop(const gradient_key_stop& from, const gradient_key_stop& to, double t) {
    if (&from == &to || t == 0.) return from.authored;
    if (t == 1.) return to.authored;
    // Both palettes have validated stop gaps, so one convex blend preserves their order.
    const auto offset = static_cast<float>(std::lerp(double(from.authored.offset), double(to.authored.offset), t));
    // In particular, preserve the RGB bytes of identical transparent colors.
    if (from.authored.color == to.authored.color) return {offset, from.authored.color};

    const auto& a = from.premultiplied;
    const auto& b = to.premultiplied;
    const double alpha = std::lerp(double(a[3]), double(b[3]), t);
    if (alpha == 0.) return {offset, 0};
    std::uint32_t packed = color_byte(static_cast<float>(alpha));
    for (int i = 0; i < 3; ++i) {
        const auto channel = static_cast<float>(std::lerp(double(a[i]), double(b[i]), t) / alpha);
        packed = (packed << 8) | srgb_byte(channel);
    }
    return {offset, packed};
}

// Descriptor fields in gradient_geometry_axis order; the gradient kinds share storage.
constexpr auto gradient_geometry_members = std::array{
    &vgui_gradient_desc::x0, &vgui_gradient_desc::y0, &vgui_gradient_desc::x1, &vgui_gradient_desc::y1,
    &vgui_gradient_desc::x0, &vgui_gradient_desc::y0, &vgui_gradient_desc::x1, &vgui_gradient_desc::y1,
    &vgui_gradient_desc::start_degrees, &vgui_gradient_desc::end_degrees,
    &vgui_gradient_desc::radius0, &vgui_gradient_desc::radius1,
};
static_assert(gradient_geometry_members.size() == gradient_geometry_channels.size());
using gradient_geometry_values = std::array<double, gradient_geometry_channels.size()>;

struct gradient_key_pose {
    double x, y;
    std::array<double, 3> tint;
    double opacity;
    gradient_geometry_values geometry;
};

gradient_key_pose key_pose(const gradient_keyframe& key, const gradient_geometry_values& authored) {
    const auto color = *parse_text_color(key.tint);
    const auto channel = [&](unsigned shift) { return double((color >> shift) & 255u) / 255.; };
    gradient_geometry_values geometry;
    for (std::size_t i = 0; i < geometry.size(); ++i) geometry[i] = key.geometry[i].value_or(authored[i]);
    return {key.x, key.y, {channel(16), channel(8), channel(0)}, key.opacity, geometry};
}

struct gradient_key {
    enum class easing { step, linear, smooth };
    double offset;
    easing interpolation;
    gradient_key_pose pose;
    std::vector<gradient_key_stop> palette; // Empty uses the widget's base palette.
};

struct gradient_key_sample {
    gradient_key_pose pose;
    const gradient_key& start;
    const gradient_key& end;
    double blend;
};

gradient_key_sample sample_gradient_keys(std::span<const gradient_key> keys, double seconds, double duration) {
    const auto next = std::upper_bound(keys.begin(), keys.end(), seconds,
        [&](double time, const gradient_key& key) { return time < key.offset * duration; });
    const auto& start = next == keys.begin() ? keys.front() : *(next - 1);
    const double start_time = start.offset * duration;
    if (next == keys.begin() || next == keys.end() || start.interpolation == gradient_key::easing::step || seconds == start_time)
        return {start.pose, start, start, 0.};
    double t = (seconds - start_time) / (next->offset * duration - start_time);
    if (start.interpolation == gradient_key::easing::smooth) t = t * t * (3. - 2. * t);
    auto pose = start.pose;
    const auto& end = next->pose;
    pose.x = std::lerp(pose.x, end.x, t);
    pose.y = std::lerp(pose.y, end.y, t);
    for (std::size_t i = 0; i < pose.tint.size(); ++i) pose.tint[i] = std::lerp(pose.tint[i], end.tint[i], t);
    pose.opacity = std::lerp(pose.opacity, end.opacity, t);
    for (std::size_t i = 0; i < pose.geometry.size(); ++i)
        pose.geometry[i] = std::lerp(pose.geometry[i], end.geometry[i], t);
    return {pose, start, *next, t};
}

vgui_color tint_color(vgui_color source, const gradient_key_pose& pose) {
    const auto scaled = [&](unsigned shift, double factor) {
        return static_cast<vgui_color>(std::lround(((source >> shift) & 255u) * factor)) << shift;
    };
    // Alpha is scaled separately so tint never changes the palette's alpha envelope.
    return scaled(24, pose.opacity) | scaled(16, pose.tint[0]) | scaled(8, pose.tint[1]) | scaled(0, pose.tint[2]);
}

struct gradient_animation {
    enum class effect { shine, color, keyframes };
    vgui_widget* widget;
    gradient_paint source;
    double duration;
    bool loop;
    effect mode;
    float dx = 0, dy = 0;
    linear_color target{};
    std::vector<linear_color> base_colors;
    std::vector<gradient_key_stop> base_palette;
    std::vector<gradient_key> keys;
    double tx = 0, ty = 0; // Keep authored translation in double precision for keyframe addition.
    std::array<bool, gradient_geometry_channels.size()> active_geometry{};
    double elapsed = 0;
    bool sampled = false;
    std::optional<std::array<float, 4>> draft;

    void resolve_keys(std::span<const gradient_keyframe> decoded, const gradient_geometry_values& authored) {
        if (decoded.empty()) return;
        base_palette.reserve(source.stops.size());
        for (const auto& stop : source.stops) base_palette.push_back(key_stop(stop));
        keys.reserve(decoded.size());
        using easing = gradient_key::easing;
        for (const auto& key : decoded) {
            const auto interpolation = key.interpolation == "step" ? easing::step :
                key.interpolation == "smooth" ? easing::smooth : easing::linear;
            auto& resolved = keys.emplace_back(key.offset, interpolation, key_pose(key, authored));
            resolved.palette.reserve(key.stops.size());
            for (const auto& stop : key.stops)
                resolved.palette.push_back(key_stop({stop.offset, *parse_text_color(stop.color)}));
        }
    }

    double loop_time(double seconds) const {
        const double remainder = std::fmod(seconds, duration);
        if (mode == effect::keyframes && remainder != 0) {
            // A computed multiple of a decimal duration can be just below the
            // mathematical boundary. Match the represented boundary exactly,
            // without wrapping the preceding representable time.
            const double cycles = std::round(seconds / duration);
            if (cycles < 0x1p53 && cycles * duration == seconds) return 0;
        }
        return remainder;
    }

    void advance(double delta) {
        sample(loop ? loop_time(elapsed + loop_time(delta)) : std::min(elapsed + delta, duration));
    }

    void seek(double seconds, bool inspect_loop_end, std::optional<std::array<float, 4>> preview) {
        auto local = loop ? loop_time(seconds) : std::min(seconds, duration);
        if (inspect_loop_end && loop && mode == effect::keyframes && seconds > 0 && local == 0)
            local = duration;
        sample(local, std::move(preview));
    }

    void sample(double next, std::optional<std::array<float, 4>> preview = {}) {
        if (mode != effect::keyframes || source.desc.kind != VGUI_GRADIENT_LINEAR) preview.reset();
        if (sampled && next == elapsed && draft == preview) return;
        elapsed = next;
        sampled = true;
        draft = std::move(preview);
        const auto& stops = source.stops;
        auto paint = source.desc;
        std::array<vgui_gradient_stop, max_gradient_stops> frame;
        if (mode == effect::keyframes) {
            if (keys.empty()) return;
            const auto key = sample_gradient_keys(keys, elapsed, duration);
            const auto& pose = key.pose;
            paint.affine[4] = static_cast<float>(tx + pose.x);
            paint.affine[5] = static_cast<float>(ty + pose.y);
            for (std::size_t i = 0; i < pose.geometry.size(); ++i)
                if (active_geometry[i]) paint.*gradient_geometry_members[i] = static_cast<float>(pose.geometry[i]);
            if (draft) {
                const auto [x0, y0, x1, y1] = *draft;
                paint.x0 = x0; paint.y0 = y0;
                paint.x1 = x1; paint.y1 = y1;
            }
            const auto& from = key.start.palette.empty() ? base_palette : key.start.palette;
            const auto& to = key.end.palette.empty() ? base_palette : key.end.palette;
            for (std::size_t i = 0; i < stops.size(); ++i) {
                frame[i] = blend_key_stop(from[i], to[i], key.blend);
                frame[i].color = tint_color(frame[i].color, pose);
            }
            if (!vgui_gradient_set(widget, &paint, frame.data(), stops.size())) {
                // Interpolated endpoints or angles can coincide between valid keys.
                // Even a positive radius gap can collapse when rounded to float.
                // Restore authored geometry, not a previous frame; keep this sample's translation and stops.
                auto fallback = source.desc;
                fallback.affine[4] = paint.affine[4];
                fallback.affine[5] = paint.affine[5];
                vgui_gradient_set(widget, &fallback, frame.data(), stops.size());
            }
            return;
        }
        const auto phase = static_cast<float>(elapsed / duration);
        if (mode == effect::shine) {
            paint.affine[4] += dx * (phase - .5f);
            paint.affine[5] += dy * (phase - .5f);
            vgui_gradient_set(widget, &paint, stops.data(), stops.size());
        } else {
            const float leg = loop ? 1.f - std::abs(2.f * phase - 1.f) : phase;
            const float t = leg * leg * (3.f - 2.f * leg);
            for (std::size_t i = 0; i < stops.size(); ++i)
                frame[i] = {stops[i].offset, t == 0.f ? stops[i].color : fade_color(base_colors[i], target, t)};
            vgui_gradient_set(widget, &paint, frame.data(), stops.size());
        }
    }
};

vgui_align alignment(std::string_view value) {
    if (value == "start") return VGUI_ALIGN_START;
    if (value == "end") return VGUI_ALIGN_END;
    if (value == "center") return VGUI_ALIGN_CENTER;
    if (value == "stretch") return VGUI_ALIGN_STRETCH;
    return VGUI_ALIGN_AUTO;
}

void apply_widget_properties(vgui_widget* widget, const widget_properties& p, bool document_root) {
    vgui_widget_set_pos(widget, p.number("x"), p.number("y"));
    vgui_widget_set_size(widget, p.number("width"), p.number("height"));
    vgui_widget_set_grow(widget, p.number("grow"));
    vgui_widget_set_visible(widget, p.boolean("visible"));
    vgui_widget_set_enabled(widget, p.boolean("enabled"));
    if (p.has("min_width") || p.has("min_height")) vgui_widget_set_min_size(widget, p.number("min_width"), p.number("min_height"));
    if (p.has("max_width") || p.has("max_height")) vgui_widget_set_max_size(widget, p.number("max_width"), p.number("max_height"));
    if (p.has("margin_left") || p.has("margin_top") || p.has("margin_right") || p.has("margin_bottom"))
        vgui_widget_set_margin(widget, p.number("margin_left"), p.number("margin_top"), p.number("margin_right"), p.number("margin_bottom"));
    if (p.has("align_self")) vgui_widget_set_align_self(widget, alignment(p.text("align_self")));
    // An unset page background leaves backing content visible, without
    // changing the normal panel defaults or overall widget opacity.
    const auto background = p.color("background");
    if (background || document_root)
        vgui_widget_set_background(widget, detail::primitive_theme_override(background.value_or(0)));
    const auto border = p.color("border_color");
    if (border || p.has("border_width"))
        vgui_widget_set_border(widget, border ? (*border ? *border : 1u) : 0,
            p.has("border_width") ? p.number("border_width") : 1.f);
    if (p.has("rounding")) vgui_widget_set_rounding(widget, p.number("rounding"));
    if (p.has("opacity")) vgui_widget_set_opacity(widget, p.number("opacity"));
    if (p.has("input_enabled")) vgui_widget_set_input_enabled(widget, p.boolean("input_enabled"));
    const auto& focusable = p.text("focusable");
    if (focusable != "default") vgui_widget_set_focusable(widget, focusable == "yes");
    if (p.has("focus_alpha")) vgui_widget_set_focus_alpha(widget, p.number("focus_alpha"));
    const auto& cursor = p.text("cursor");
    if (cursor != "default") {
        constexpr std::pair<std::string_view, vgui_cursor> cursors[] = {
            {"arrow", VGUI_CURSOR_ARROW}, {"text", VGUI_CURSOR_IBEAM}, {"wait", VGUI_CURSOR_WAIT},
            {"no", VGUI_CURSOR_NO}, {"hand", VGUI_CURSOR_HAND}, {"size_h", VGUI_CURSOR_SIZE_H}, {"size_v", VGUI_CURSOR_SIZE_V},
        };
        for (const auto& [name, shape] : cursors) if (cursor == name) vgui_widget_set_cursor(widget, shape);
    }
    if (p.has("tooltip")) vgui_widget_set_tooltip(widget, p.text("tooltip").c_str());
    if (p.has("tooltip_delay")) vgui_widget_set_tooltip_delay(widget, p.number("tooltip_delay"));
    if (p.has("tooltip_enabled")) vgui_widget_set_tooltip_enabled(widget, p.boolean("tooltip_enabled"));
    if (p.has("tooltip_when_disabled")) vgui_widget_set_tooltip_when_disabled(widget, p.boolean("tooltip_when_disabled"));
    if (p.text("tooltip_anchor") == "widget") vgui_widget_set_tooltip_anchor(widget, VGUI_TOOLTIP_ANCHOR_WIDGET);
    if (p.has("a11y_name")) vgui_widget_set_a11y_name(widget, p.text("a11y_name").c_str());
}

void apply_font(vgui_widget* widget, const node& n) {
    const float size = number(n, "font_size");
    if (size != 0) vgui_widget_set_font_size(widget, size);

    const auto& weight = text(n, "font_weight");
    if (weight == "regular") vgui_widget_set_font_weight(widget, VGUI_FONT_WEIGHT_REGULAR);
    else if (weight == "medium") vgui_widget_set_font_weight(widget, VGUI_FONT_WEIGHT_MEDIUM);
    else if (weight == "semi_bold") vgui_widget_set_font_weight(widget, VGUI_FONT_WEIGHT_SEMI_BOLD);
    else if (weight == "bold") vgui_widget_set_font_weight(widget, VGUI_FONT_WEIGHT_BOLD);

    const auto& slant = text(n, "font_slant");
    if (slant == "normal") vgui_widget_set_font_slant(widget, VGUI_FONT_SLANT_NORMAL);
    else if (slant == "italic") vgui_widget_set_font_slant(widget, VGUI_FONT_SLANT_ITALIC);
    else if (slant == "oblique") vgui_widget_set_font_slant(widget, VGUI_FONT_SLANT_OBLIQUE);

    if (const auto color = parse_text_color(text(n, "text_color"))) {
        // VGUI reserves 0 for theme inheritance. Transparent black still
        // needs a nonzero RGB bit to represent an explicit transparent color.
        vgui_widget_set_text_color(widget, *color == 0 ? 1u : *color);
    }
}
pixel_rect intersect(pixel_rect a, pixel_rect b) {
    float x = std::max(a.x, b.x), y = std::max(a.y, b.y);
    return {x, y, std::max(0.f, std::min(a.x+a.width,b.x+b.width)-x),
        std::max(0.f, std::min(a.y+a.height,b.y+b.height)-y)};
}
// Compare path components, not string prefixes. Canonicalization follows symlinks.
bool inside(const std::filesystem::path& root, const std::filesystem::path& file) {
    auto r = root.begin(), f = file.begin();
    for (; r != root.end(); ++r, ++f) if (f == file.end() || *r != *f) return false;
    return f != file.end();
}

std::filesystem::path resolve_resource(const std::filesystem::path& project_root, const std::string& relative) {
    if (project_root.empty()) return {};
    std::error_code ec;
    const auto root = std::filesystem::canonical(project_root, ec);
    if (ec || !std::filesystem::is_directory(root, ec)) return {};
    auto file = std::filesystem::canonical(root / std::filesystem::u8path(relative), ec);
    if (ec || !inside(root, file) || !std::filesystem::is_regular_file(file, ec)) return {};
    return file;
}
}

struct runtime::mounted_tree {
    struct entry {
        std::string id, action;
        vgui_widget* widget = nullptr;
        bool placeholder = false;
        runtime_bindings::action_handler handler = nullptr;
        void* user = nullptr;
        std::map<std::pair<vgui_item_id, int>, std::string> row_actions;
    };
    struct menu_actions {
        const entry* owner = nullptr;
        std::map<vgui_item_id, std::string> actions;
    };
    vgui_widget* root = nullptr;
    std::map<std::string, entry, std::less<>> entries;
    std::map<vgui_widget*, menu_actions> menus;
    std::set<vgui_texture*> collection_textures;
    std::vector<std::string> order;
    std::vector<diagnostic> diagnostics;
    std::vector<gradient_animation> gradient_animations;
    animation_span animation_timing;
    std::uint64_t revision = 0;
    float width = 0, height = 0;
    ~mounted_tree() {
        if (root) vgui_widget_destroy(root);
        for (auto* texture : collection_textures) vgui_texture_release(texture);
    }
    static void event(vgui_widget*, const vgui_event* event, void* user) {
        auto* e = static_cast<entry*>(user);
        if (event->type == VGUI_EVENT_CLICK && e->handler) e->handler(e->id, e->action, e->user);
    }
    static void menu_event(vgui_widget*, vgui_item_id item, void* user) {
        const auto& menu = *static_cast<menu_actions*>(user);
        const auto action = menu.actions.find(item);
        if (action != menu.actions.end() && menu.owner->handler)
            menu.owner->handler(menu.owner->id, action->second, menu.owner->user);
    }
    static void row_button_event(vgui_widget* widget, vgui_item_id item, int button, void* user) {
        const auto& e = *static_cast<entry*>(user);
        const auto action = e.row_actions.find({item, button});
        if (action == e.row_actions.end() || !e.handler) return;
        // Native button hits bypass the enabled check used by row selection.
        const bool enabled = vgui_widget_get_kind(widget) == VGUI_TREE ?
            vgui_tree_get_item_enabled(widget, item) : vgui_list_get_item_enabled(widget, item);
        if (enabled) e.handler(e.id, action->second, e.user);
    }
};

runtime::runtime(vgui_context* ctx) : ctx_(ctx) {}
runtime::~runtime() = default;
void runtime::unmount() { tree_.reset(); }
vgui_widget* runtime::widget(std::string_view id) const {
    if (!tree_) return nullptr;
    auto it = tree_->entries.find(id);
    return it == tree_->entries.end() ? nullptr : it->second.widget;
}
std::span<const diagnostic> runtime::diagnostics() const {
    return tree_ ? std::span<const diagnostic>(tree_->diagnostics) : std::span<const diagnostic>();
}

namespace {
bool animation_visible(vgui_widget* widget, int width, int height) {
    for (auto* w = widget; w; w = vgui_widget_get_parent(w))
        if (!vgui_widget_get_visible(w)) return false;
    pixel_rect rect;
    vgui_widget_get_rect(widget, &rect.x, &rect.y, &rect.width, &rect.height);
    const auto clipped = intersect(rect, {0, 0, float(width), float(height)});
    return clipped.width > 0 && clipped.height > 0;
}
} // namespace

void runtime::advance_animations(double delta_seconds) {
    if (!tree_ || tree_->gradient_animations.empty() || !std::isfinite(delta_seconds) || delta_seconds < 0) return;
    int width = 0, height = 0;
    vgui_context_get_size(ctx_, &width, &height);
    if (width <= 0 || height <= 0) return;
    for (auto& animation : tree_->gradient_animations) {
        if (!animation.loop && animation.sampled && !animation.draft && animation.elapsed >= animation.duration) continue;
        if (animation_visible(animation.widget, width, height)) animation.advance(delta_seconds);
    }
}

animation_span runtime::animation_timing() const {
    return tree_ ? tree_->animation_timing : animation_span{};
}

void runtime::seek_animations(double seconds, bool inspect_loop_end, const linear_gradient_preview* preview) {
    if (!tree_ || tree_->gradient_animations.empty() || !std::isfinite(seconds) || seconds < 0) return;
    int width = 0, height = 0;
    vgui_context_get_size(ctx_, &width, &height);
    if (width <= 0 || height <= 0) return;
    auto* target = preview ? widget(preview->node) : nullptr;
    for (auto& animation : tree_->gradient_animations) {
        if (animation_visible(animation.widget, width, height))
            animation.seek(seconds, inspect_loop_end,
                preview && animation.widget == target ? std::optional(preview->endpoints) : std::nullopt);
    }
}

bool runtime::mount(const document& doc, std::uint64_t revision, const runtime_bindings& bindings, diagnostic* error) {
    if (!validate(doc, error)) return false;
    if (!ctx_ || !vgui_root(ctx_)) {
        if (error) *error = {"", "missing VGUI context or root"};
        return false;
    }
    auto next = std::make_unique<mounted_tree>();
    next->revision = revision;
    const auto& root_node = doc.nodes.at(doc.root);
    next->width = number(root_node, "width"); next->height = number(root_node, "height");
    std::map<std::string, vgui_texture*, std::less<>> images;
    const auto load_image = [&](const std::string& key) -> vgui_texture* {
        auto [it, inserted] = images.emplace(key, nullptr);
        if (!inserted) return it->second;
        if (bindings.load_embedded_image) {
            it->second = bindings.load_embedded_image(ctx_, key, bindings.user);
        } else if (bindings.load_image) {
            const auto file = resolve_resource(bindings.project_root, doc.resources.at(key));
            if (!file.empty()) it->second = bindings.load_image(ctx_, key, file, bindings.user);
        }
        return it->second;
    };
    const auto image_unavailable = [&](std::string_view what, const std::string& key) {
        return std::string(what) + (bindings.load_embedded_image
            ? " unavailable: " : " unavailable or outside project root: ") + key;
    };
    const auto load_collection_icon = [&](const std::string& key, std::string path) -> vgui_texture* {
        auto* texture = load_image(key);
        if (!texture) next->diagnostics.push_back({std::move(path), image_unavailable("image resource", key)});
        // Native list/tree/menu item icon setters borrow the raw texture.
        else if (next->collection_textures.insert(texture).second) vgui_texture_retain(texture);
        return texture;
    };
    std::map<std::string, font_familys_t*, std::less<>> fonts;
    const auto load_font = [&](const std::string& key) -> font_familys_t* {
        auto [it, inserted] = fonts.emplace(key, nullptr);
        if (inserted && bindings.load_font) {
            if (is_builtin_font(key)) it->second = bindings.load_font(key, {}, bindings.user);
            else {
                const auto file = resolve_resource(bindings.project_root, doc.resources.at(key));
                if (!file.empty()) it->second = bindings.load_font(key, file, bindings.user);
            }
        }
        return it->second;
    };
    std::function<bool(const std::string&, vgui_widget*, const node*)> build =
        [&](const std::string& id, vgui_widget* parent, const node* parent_node) {
        const auto& n = doc.nodes.at(id);
        const auto kind = [&]() -> std::optional<vgui_widget_kind> {
            if (n.type == "panel") return VGUI_PANEL;
            if (n.type == "label") return VGUI_LABEL;
            if (n.type == "button") return VGUI_BUTTON;
            if (n.type == "edit") return VGUI_EDIT;
            if (n.type == "image") return VGUI_IMAGE;
            if (n.type == "gradient") return VGUI_GRADIENT;
            if (const auto result = detail::primitive_kind(n.type)) return result;
            if (const auto result = detail::collection_mount_kind(n.type)) return result;
            return detail::container_kind(n.type);
        }();
        if (!kind) return false;
        auto* w = vgui_widget_add(parent, *kind, "");
        if (!w) return false;
        if (!next->root) next->root = w;
        auto& e = next->entries[id]; e.id = id; e.widget = w;
        next->order.push_back(id);
        vgui_widget_set_id(w, id.c_str());
        const widget_properties p{n};
        apply_widget_properties(w, p, !parent_node);
        if (*kind == VGUI_GRADIENT) {
            gradient_paint paint;
            if (!read_gradient(paint, p) || !vgui_gradient_set(w, &paint.desc, paint.stops.data(), paint.stops.size())) return false;
            const auto& mode = p.text("gradient_animation");
            if (mode != "none") {
                using effect = gradient_animation::effect;
                auto& animation = next->gradient_animations.emplace_back(w, std::move(paint),
                    std::get<double>(property(n, "gradient_animation_duration")), p.boolean("gradient_animation_loop"),
                    mode == "shine" ? effect::shine : mode == "color" ? effect::color : effect::keyframes);
                auto& timing = next->animation_timing;
                timing.duration = std::max(timing.duration, animation.duration);
                timing.loops |= animation.loop;
                if (animation.mode == effect::shine) {
                    animation.dx = p.number("gradient_animation_dx");
                    animation.dy = p.number("gradient_animation_dy");
                } else if (animation.mode == effect::color) {
                    const auto& fallback = std::get<std::string>(find_property(n.type, "gradient_animation_color")->default_value);
                    animation.target = linear_rgba(p.color("gradient_animation_color").value_or(*parse_text_color(fallback)));
                    for (const auto& stop : animation.source.stops) animation.base_colors.push_back(linear_rgba(stop.color));
                } else {
                    std::vector<gradient_keyframe> keys;
                    std::string reason;
                    if (!decode_gradient_keyframes(p.text("gradient_animation_keyframes"), keys, reason)) return false;
                    animation.tx = std::get<double>(property(n, "gradient_affine_tx"));
                    animation.ty = std::get<double>(property(n, "gradient_affine_ty"));
                    const auto& gradient_kind = p.text("gradient_kind");
                    gradient_geometry_values authored{};
                    for (std::size_t i = 0; i < gradient_geometry_channels.size(); ++i) {
                        const auto& channel = gradient_geometry_channels[i];
                        if (!channel.applies_to(gradient_kind)) continue;
                        animation.active_geometry[i] = true;
                        authored[i] = std::get<double>(property(n, channel.property));
                    }
                    animation.resolve_keys(keys, authored);
                }
            }
        }
        detail::apply_container(w, n, p);
        if (n.type == "panel") {
            const auto& l = text(n,"layout");
            vgui_widget_set_layout(w, l == "row" ? VGUI_LAYOUT_ROW : l == "column" ? VGUI_LAYOUT_COLUMN :
                l == "wrap" ? VGUI_LAYOUT_WRAP : VGUI_LAYOUT_ABSOLUTE);
            vgui_widget_set_align_items(w, alignment(text(n,"align_items")));
            const auto& j = text(n,"justify");
            vgui_widget_set_justify(w, j == "center" ? VGUI_JUSTIFY_CENTER : j == "end" ? VGUI_JUSTIFY_END : j == "space-between" ? VGUI_JUSTIFY_SPACE_BETWEEN : j == "space-around" ? VGUI_JUSTIFY_SPACE_AROUND : VGUI_JUSTIFY_START);
            vgui_widget_set_gap(w, number(n,"gap"));
            vgui_widget_set_padding(w, number(n,"pad_left"),number(n,"pad_top"),number(n,"pad_right"),number(n,"pad_bottom"));
        } else if (n.type == "image") {
            const auto& key = text(n,"resource");
            if (!key.empty()) {
                if (auto* texture = load_image(key)) vgui_image_set_texture(w, texture);
                else {
                    e.placeholder = true;
                    vgui_widget_set_background(w, 0xFFFF00FF);
                    next->diagnostics.push_back({"/nodes/" + id + "/props/resource", image_unavailable("image resource", key)});
                }
            }
        } else if (find_property(n.type, "font_family")) {
            const auto& family = text(n, "font_family");
            if (!family.empty()) {
                if (auto* font = load_font(family)) vgui_widget_set_font_family(w, font);
                else next->diagnostics.push_back({"/nodes/" + id + "/props/font_family",
                    "font unavailable; using the context family: " + family});
            }
            apply_font(w, n);
            if (n.type == "edit") {
                vgui_edit_set_text(w, text(n,"text").c_str());
                vgui_edit_set_placeholder(w, text(n,"placeholder").c_str());
            } else if (find_property(n.type, "text")) vgui_widget_set_text(w, text(n,"text").c_str());
            if (find_property(n.type, "text_key") && !text(n,"text_key").empty())
                vgui_widget_set_text_key(w, text(n,"text_key").c_str());
            if (n.type == "button") {
                const auto& key = text(n,"atlas");
                if (!key.empty()) {
                    auto* atlas = load_image(key);
                    if (!atlas || !vgui_button_set_atlas(w, atlas)) {
                        e.placeholder = true;
                        next->diagnostics.push_back({"/nodes/" + id + "/props/atlas", !atlas ?
                            image_unavailable("button atlas", key) :
                            "button atlas requires an even-sized 2x2 image: " + key});
                    }
                }
                const auto& icon = text(n, "icon");
                if (!icon.empty()) {
                    if (auto* texture = load_image(icon)) detail::apply_button_icon(w, p, texture);
                    else next->diagnostics.push_back({"/nodes/" + id + "/props/icon",
                        image_unavailable("button icon", icon)});
                }
                if (!text(n,"action").empty()) {
                    e.action = text(n,"action"); e.handler = bindings.on_action; e.user = bindings.user;
                    vgui_widget_on_event(w, mounted_tree::event, &e);
                }
            }
        }
        detail::apply_primitive(w, n, p);
        if (detail::collection_kind(n.type)) {
            e.handler = bindings.on_action;
            e.user = bindings.user;
            const auto bind_menu_action = [&](vgui_widget* owner, vgui_item_id item, const std::string& action) {
                if (!e.handler || action.empty()) return;
                auto& menu = next->menus[owner];
                menu.owner = &e;
                menu.actions.emplace(item, action);
                vgui_menu_on_activate(owner, mounted_tree::menu_event, &menu);
            };
            const auto bind_row_action = [&](vgui_item_id item, int button, const std::string& action) {
                if (e.handler && !action.empty()) e.row_actions.emplace(std::pair{item, button}, action);
            };
            if (!detail::apply_collection(w, n, p, "/nodes/" + id + "/props/items", load_collection_icon,
                bind_menu_action, bind_row_action)) return false;
            if (!e.row_actions.empty()) {
                if (*kind == VGUI_TREE) vgui_tree_on_item_button(w, mounted_tree::row_button_event, &e);
                else vgui_list_on_item_button(w, mounted_tree::row_button_event, &e);
            }
        }
        if (parent_node && !detail::insert_content(parent, *parent_node, w, n, p)) return false;
        for (const auto& child : n.children) if (!build(child, w, &n)) return false;
        detail::finish_container(w, n, p);
        return true;
    };
    if (!build(doc.root, vgui_root(ctx_), nullptr)) {
        if (error) *error = {"", "failed to create VGUI widget"};
        return false;
    }
    tree_ = std::move(next);
    return true;
}

std::optional<layout_snapshot> runtime::layout(viewport view) {
    if (!tree_ || view.width <= 0 || view.height <= 0 || !std::isfinite(view.scale) || view.scale < .25f || view.scale > 4.f ||
        double(view.width)*view.scale > std::numeric_limits<int>::max() || double(view.height)*view.scale > std::numeric_limits<int>::max()) return std::nullopt;
    int width = static_cast<int>(std::lround(double(view.width)*view.scale));
    int height = static_cast<int>(std::lround(double(view.height)*view.scale));
    if (width <= 0 || height <= 0) return std::nullopt;
    vgui_context_set_ui_scale(ctx_, view.scale);
    vgui_context_set_size(ctx_, width, height);
    vgui_widget_set_size(tree_->root, tree_->width == 0 ? float(view.width) : tree_->width, tree_->height == 0 ? float(view.height) : tree_->height);
    vgui_layout(ctx_);
    layout_snapshot result; result.revision = tree_->revision; result.serial = ++serial_; result.view = view; result.pixel_width = width; result.pixel_height = height;
    struct state { pixel_rect clip; bool visible, enabled; };
    std::map<vgui_widget*, state> states;
    for (const auto& id : tree_->order) {
        const auto& e = tree_->entries.at(id);
        state parent{{0,0,float(width),float(height)},true,true};
        auto p = states.find(vgui_widget_get_parent(e.widget)); if (p != states.end()) parent = p->second;
        node_layout item; item.id = id; item.image_placeholder = e.placeholder;
        vgui_widget_get_rect(e.widget, &item.rect_px.x, &item.rect_px.y, &item.rect_px.width, &item.rect_px.height);
        item.visible_px = intersect(parent.clip, item.rect_px);
        item.visible = parent.visible && vgui_widget_get_visible(e.widget) && item.visible_px.width > 0 && item.visible_px.height > 0;
        item.enabled = parent.enabled && vgui_widget_get_enabled(e.widget);
        states.emplace(e.widget, state{item.visible_px, item.visible, item.enabled});
        result.nodes.push_back(std::move(item));
    }
    return result;
}
} // namespace vgui::ui_design
