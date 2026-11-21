#include "runtime.h"
#include "primitive_runtime.h"
#include "collection_runtime.h"
#include "container_runtime.h"
#include "../vgui_internal.h"

#include <algorithm>
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
