#include "runtime.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
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
}

struct runtime::mounted_tree {
    struct entry {
        std::string id, action;
        vgui_widget* widget = nullptr;
        bool placeholder = false;
        runtime_bindings::action_handler handler = nullptr;
        void* user = nullptr;
    };
    vgui_widget* root = nullptr;
    std::map<std::string, entry, std::less<>> entries;
    std::vector<std::string> order;
    std::vector<diagnostic> diagnostics;
    std::uint64_t revision = 0;
    float width = 0, height = 0;
    ~mounted_tree() { if (root) vgui_widget_destroy(root); }
    static void event(vgui_widget*, const vgui_event* event, void* user) {
        auto* e = static_cast<entry*>(user);
        if (event->type == VGUI_EVENT_CLICK && e->handler) e->handler(e->id, e->action, e->user);
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
        if (inserted && bindings.load_image && !bindings.project_root.empty()) {
            std::error_code ec;
            auto root = std::filesystem::canonical(bindings.project_root, ec);
            if (!ec && std::filesystem::is_directory(root, ec) && !ec) {
                auto file = std::filesystem::canonical(root / std::filesystem::u8path(doc.resources.at(key)), ec);
                if (!ec && inside(root, file) && std::filesystem::is_regular_file(file, ec) && !ec)
                    it->second = bindings.load_image(ctx_, key, file, bindings.user);
            }
        }
        return it->second;
    };
    std::function<bool(const std::string&, vgui_widget*)> build = [&](const std::string& id, vgui_widget* parent) {
        const auto& n = doc.nodes.at(id);
        auto kind = n.type == "panel" ? VGUI_PANEL : n.type == "label" ? VGUI_LABEL :
            n.type == "button" ? VGUI_BUTTON : n.type == "edit" ? VGUI_EDIT : VGUI_IMAGE;
        auto* w = vgui_widget_add(parent, kind, "");
        if (!w) return false;
        if (!next->root) next->root = w;
        auto& e = next->entries[id]; e.id = id; e.widget = w;
        next->order.push_back(id);
        vgui_widget_set_id(w, id.c_str());
        vgui_widget_set_pos(w, number(n,"x"), number(n,"y"));
        vgui_widget_set_size(w, number(n,"width"), number(n,"height"));
        vgui_widget_set_grow(w, number(n,"grow"));
        vgui_widget_set_visible(w, boolean(n,"visible"));
        vgui_widget_set_enabled(w, boolean(n,"enabled"));
        if (n.type == "panel") {
            const auto& l = text(n,"layout");
            vgui_widget_set_layout(w, l == "row" ? VGUI_LAYOUT_ROW : l == "column" ? VGUI_LAYOUT_COLUMN : VGUI_LAYOUT_ABSOLUTE);
            const auto& a = text(n,"align_items");
            vgui_widget_set_align_items(w, a == "center" ? VGUI_ALIGN_CENTER : a == "end" ? VGUI_ALIGN_END : a == "stretch" ? VGUI_ALIGN_STRETCH : VGUI_ALIGN_START);
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
                    next->diagnostics.push_back({"/nodes/" + id + "/props/resource", "image resource unavailable or outside project root: " + key});
                }
            }
        } else {
            if (n.type == "edit") {
                vgui_edit_set_text(w, text(n,"text").c_str());
                vgui_edit_set_placeholder(w, text(n,"placeholder").c_str());
            } else vgui_widget_set_text(w, text(n,"text").c_str());
            if (!text(n,"text_key").empty()) vgui_widget_set_text_key(w, text(n,"text_key").c_str());
            if (n.type == "button") {
                const auto& key = text(n,"atlas");
                if (!key.empty()) {
                    auto* atlas = load_image(key);
                    if (!atlas || !vgui_button_set_atlas(w, atlas)) {
                        e.placeholder = true;
                        next->diagnostics.push_back({"/nodes/" + id + "/props/atlas", !atlas ?
                            "button atlas unavailable or outside project root: " + key :
                            "button atlas requires an even-sized 2x2 image: " + key});
                    }
                }
                if (!text(n,"action").empty()) {
                    e.action = text(n,"action"); e.handler = bindings.on_action; e.user = bindings.user;
                    vgui_widget_on_event(w, mounted_tree::event, &e);
                }
            }
        }
        for (const auto& child : n.children) if (!build(child, w)) return false;
        return true;
    };
    if (!build(doc.root, vgui_root(ctx_))) {
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
