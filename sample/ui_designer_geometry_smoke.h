#pragma once

#include "ui_designer_preview.h"

#include "ui_designer_menu_smoke.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace vgui::ui_design {

// With absolute-form.vgui.json open and presented, advance the same host loop
// as interactive input. Only a local copy of the document is used as an oracle.
inline void exercise_geometry(os_window& win, vgui_context* shell, designer_preview& preview,
                            const session& history, const std::function<void()>& frame) {
    const native_menu_input menus(win, shell, frame);
    std::string_view step = "setup";
    const auto require = [&](bool passed, std::string_view message) {
        if (!passed) throw std::runtime_error("Native design geometry (" + std::string(step) + "): " + std::string(message));
    };
    const auto push = [&](SDL_Event e) {
        if (!SDL_PushEvent(&e)) require(false, std::string("could not queue SDL event: ") + SDL_GetError());
    };
    const auto rounded = [](glm::vec2 p) { return glm::vec2{std::round(p.x), std::round(p.y)}; };
    const auto mouse = [&](bool down, glm::vec2 px, Uint8 button = SDL_BUTTON_LEFT) {
        SDL_Event e{};
        e.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
        e.button.windowID = win.id;
        e.button.button = button;
        e.button.down = down;
        e.button.clicks = 1;
        const auto p = px / win.pixel_ratio();
        e.button.x = p.x;
        e.button.y = p.y;
        push(e);
    };
    const auto motion = [&](glm::vec2 px, glm::vec2 last, SDL_MouseButtonFlags buttons = SDL_BUTTON_LMASK) {
        SDL_Event e{};
        e.type = SDL_EVENT_MOUSE_MOTION;
        e.motion.windowID = win.id;
        e.motion.state = buttons;
        const auto p = px / win.pixel_ratio(), rel = (px - last) / win.pixel_ratio();
        e.motion.x = p.x;
        e.motion.y = p.y;
        e.motion.xrel = rel.x;
        e.motion.yrel = rel.y;
        push(e);
    };
    const auto click = [&](glm::vec2 px) { px = rounded(px); mouse(true, px); mouse(false, px); frame(); };
    const auto find = [](const auto& self, vgui_widget* w, std::string_view id) -> vgui_widget* {
        if (!w || id == vgui_widget_get_id(w)) return w;
        for (std::size_t i = 0; i < vgui_widget_get_child_count(w); ++i)
            if (auto* match = self(self, vgui_widget_get_child(w, i), id)) return match;
        return nullptr;
    };
    const auto shell_widget = [&](std::string_view id) {
        auto* w = find(find, vgui_root(shell), id);
        require(w != nullptr, id);
        return w;
    };
    const auto button = [&](const char* id) {
        float x, y, w, h;
        vgui_widget_get_rect(shell_widget(id), &x, &y, &w, &h);
        click({x + w / 2, y + h / 2});
    };
    auto* canvas = shell_widget("designer.preview.canvas");
    const auto layout = [&] {
        const auto* s = preview.snapshot();
        require(s && s->pixel_width > 0 && s->pixel_height > 0 && s->view.scale > 0, "missing layout");
        return s;
    };
    const auto board = [&] {
        const auto r = preview.artboard_rect_px();
        require(r && r->width > 0 && r->height > 0, "missing artboard");
        return *r;
    };
    const auto rect = [&](std::string_view id) {
        const auto* s = layout();
        const auto it = std::find_if(s->nodes.begin(), s->nodes.end(), [id](const auto& n) { return n.id == id; });
        require(it != s->nodes.end() && it->visible, id);
        return it->rect_px;
    };
    const auto board_scale = [&](pixel_rect b) {
        const auto* s = layout();
        return glm::vec2{b.width / s->pixel_width, b.height / s->pixel_height};
    };
    const auto point = [&](pixel_rect r, float fx, float fy) {
        const auto b = board();
        return glm::vec2{b.x, b.y} + glm::vec2{r.x + r.width * fx, r.y + r.height * fy} * board_scale(b);
    };
    struct state {
        document doc;
        std::uint64_t revision;
        bool dirty, undo, redo;
        bool operator==(const state&) const = default;
    };
    const auto read = [&] { return state{history.current(), history.revision(), history.dirty(), history.can_undo(), history.can_redo()}; };
    enum class cancel { none, escape, focus };
    const auto drag = [&](glm::vec2 start, glm::vec2 delta, cancel how = cancel::none) {
        const auto before = read();
        // SDL takes window points; our projected coordinates are shell pixels.
        // Account for rounding each endpoint once when predicting the delta.
        const auto scale = board_scale(board()) * layout()->view.scale;
        start = rounded(start);
        const auto mid = rounded(start + delta * scale * .5f), end = rounded(start + delta * scale);
        require(vgui_hit_test(shell, start.x, start.y) == canvas, "gesture did not start on the canvas");
        mouse(true, start);
        frame();
        require(read() == before, "mouse-down changed the document or history");
        motion(mid, start);
        frame();
        require(read() == before, "first motion changed the document or history before release");
        motion(end, mid);
        frame();
        require(read() == before, "second motion changed the document or history before release");
        if (how != cancel::none) {
            SDL_Event e{};
            if (how == cancel::escape) {
                e.type = SDL_EVENT_KEY_DOWN;
                e.key.windowID = win.id;
                e.key.key = SDLK_ESCAPE;
                e.key.scancode = SDL_SCANCODE_ESCAPE;
                e.key.down = true;
                push(e);
                e.type = SDL_EVENT_KEY_UP;
                e.key.down = false;
            } else {
                e.type = SDL_EVENT_WINDOW_FOCUS_LOST;
                e.window.windowID = win.id;
            }
            push(e);
            frame();
            require(read() == before, "cancellation changed the document or history");
            motion(mid, end);
        }
        mouse(false, how == cancel::none ? end : mid);
        frame();
        return (end - start) / scale;
    };
    const auto prop = [](const document& doc, const char* id, const char* key) {
        return std::get<double>(doc.nodes.at(id).props.at(key));
    };
    const auto edited = [&](const state& before, const char* id,
                        std::initializer_list<std::pair<const char*, double>> changes) {
        const auto after = read();
        require(after.revision == before.revision + 1 && after.dirty && after.undo && !after.redo,
            "release did not create exactly one edit");
        auto expected = before.doc;
        for (const auto& [key, target] : changes) {
            const double actual = prop(after.doc, id, key);
            require(std::abs(actual - target) <= 1.0, std::string(key) + " does not match the pointer delta: expected " +
                std::to_string(target) + ", got " + std::to_string(actual));
            expected.nodes.at(id).props[key] = actual;
        }
        require(after.doc == expected, "gesture changed properties outside its geometry edit");
        return after;
    };
    const auto same_rect = [&](pixel_rect a, pixel_rect b, std::string_view message) {
        constexpr float epsilon = .05f;
        require(std::abs(a.x - b.x) <= epsilon && std::abs(a.y - b.y) <= epsilon &&
            std::abs(a.width - b.width) <= epsilon && std::abs(a.height - b.height) <= epsilon, message);
    };
    const auto check_rect = [&](const char* id, pixel_rect intrinsic) {
        const auto& doc = history.current();
        constexpr auto parent_id = "geometry_panel";
        const auto parent = rect(parent_id);
        const auto s = layout()->view.scale;
        const auto w = prop(doc, id, "width"), h = prop(doc, id, "height");
        same_rect(rect(id), {parent.x + float(prop(doc, parent_id, "pad_left") + prop(doc, id, "x")) * s,
            parent.y + float(prop(doc, parent_id, "pad_top") + prop(doc, id, "y")) * s,
            w > 0 ? float(w) * s : intrinsic.width, h > 0 ? float(h) * s : intrinsic.height},
            "layout does not match properties relative to the padded parent");
    };

    const auto initial = read();
    require(initial.doc.root == "absolute_form" && !initial.dirty && !initial.undo && !initial.redo,
        "expected freshly opened absolute-form.vgui.json");
    if (preview.running()) button("designer.preview.mode");
    require(!preview.running(), "expected Design mode");
    button("designer.preview.phone");
    button("designer.preview.fit");

    step = "return to 100% after zoom and pan";
    const auto fitted = board();
    button("designer.preview.in");
    const auto zoomed = board();
    require(zoomed.width > fitted.width, "zoom in did not enlarge the board");
    pixel_rect area;
    vgui_widget_get_rect(canvas, &area.x, &area.y, &area.width, &area.height);
    const auto center = rounded({area.x + area.width / 2, area.y + area.height / 2});
    const auto panned = center + glm::vec2{31, -27};
    mouse(true, center, SDL_BUTTON_MIDDLE);
    motion(panned, center, SDL_BUTTON_MMASK);
    mouse(false, panned, SDL_BUTTON_MIDDLE);
    frame();
    require(board().x > zoomed.x + 25 && board().y < zoomed.y - 20, "middle-drag did not pan the board");

    button("designer.preview.actual");
    const auto scale = vgui_context_get_ui_scale(shell);
    const float width = std::round(390 * scale), height = std::round(844 * scale);
    const pixel_rect actual{std::round(area.x + (area.width - width) / 2),
        std::round(area.y + (area.height - height) / 2), width, height};
    same_rect(board(), actual, "1:1 did not restore and center the board at actual size");
    const auto readout = shell_widget("designer.preview.zoom");
    require(std::string_view(vgui_widget_get_text(readout)) == "100%", "zoom readout did not return to 100%");
    require(layout()->view.width == 390 && layout()->view.height == 844 && read() == initial,
        "1:1 changed the viewport preset, document or undo history");

    const auto at_actual_size = [&] {
        const auto current = board();
        return current.width == actual.width && current.height == actual.height &&
            std::string_view(vgui_widget_get_text(readout)) == "100%";
    };
    const auto wheel = [&](float delta) {
        SDL_Event e{};
        e.type = SDL_EVENT_MOUSE_WHEEL;
        e.wheel.windowID = win.id;
        e.wheel.y = delta;
        e.wheel.mouse_x = center.x / win.pixel_ratio();
        e.wheel.mouse_y = center.y / win.pixel_ratio();
        push(e);
        frame();
    };

    step = "zoom in across 100%";
    wheel(-1); // 100% -> 87%; one + would otherwise jump to 109%.
    require(std::string_view(vgui_widget_get_text(readout)) == "87%", "wheel did not establish zoom below 100%");
    button("designer.preview.in");
    require(at_actual_size(), "zoom in skipped 100%");
    button("designer.preview.in");
    require(std::string_view(vgui_widget_get_text(readout)) == "125%", "zoom in got stuck at 100%");
    button("designer.preview.out");
    require(at_actual_size(), "reciprocal zoom steps drifted from actual size");

    step = "zoom out across 100%";
    wheel(1); // 100% -> 115%; one - would otherwise jump to 92%.
    require(std::string_view(vgui_widget_get_text(readout)) == "115%", "wheel did not establish zoom above 100%");
    button("designer.preview.out");
    require(at_actual_size(), "zoom out skipped 100%");
    button("designer.preview.out");
    require(std::string_view(vgui_widget_get_text(readout)) == "80%", "zoom out got stuck at 100%");
    wheel(2);
    require(std::string_view(vgui_widget_get_text(readout)) == "106%", "button stop affected continuous wheel zoom");
    require(read() == initial, "zoom changed the document or undo history");
    button("designer.preview.fit");
    same_rect(board(), fitted, "Fit no longer restored the fitted view");
    const auto fit_width = board().width;
    constexpr auto auto_id = "auto_button", fixed_id = "fixed_button";
    const auto intrinsic = rect(auto_id);
    require(prop(initial.doc, auto_id, "width") == 0 && prop(initial.doc, auto_id, "height") == 0 &&
        intrinsic.width > 0 && intrinsic.height > 0, "expected a measured auto-size button");
    check_rect(auto_id, intrinsic);
    click(point(intrinsic, .5f, .5f));
    require(read() == initial, "selection or toolbar click changed the document");

    step = "move auto-sized child at fit";
    const auto move = drag(point(intrinsic, .5f, .5f), {26, 34});
    const auto moved = edited(initial, auto_id, {{"x", prop(initial.doc, auto_id, "x") + move.x},
        {"y", prop(initial.doc, auto_id, "y") + move.y}});
    check_rect(auto_id, intrinsic); // x/y only; width and height must remain auto.

    step = "resize auto-sized child at increased zoom (bottom right)";
    button("designer.preview.in");
    require(board().width > fit_width * 1.1f && read() == moved, "zoom did not enlarge the artboard independently of the document");
    const auto auto_before = rect(auto_id);
    const auto grow = drag(point(auto_before, 1, 1), {37, 29});
    const auto grown = edited(moved, auto_id, {{"width", auto_before.width / layout()->view.scale + grow.x},
        {"height", auto_before.height / layout()->view.scale + grow.y}});
    check_rect(auto_id, intrinsic);

    step = "resize fixed child (top left)";
    const auto fixed_before = rect(fixed_id);
    click(point(fixed_before, .5f, .5f));
    require(read() == grown, "selecting fixed child changed the document");
    const auto corner = drag(point(fixed_before, 0, 0), {-31, -25});
    const auto resized = edited(grown, fixed_id, {{"x", prop(grown.doc, fixed_id, "x") + corner.x},
        {"y", prop(grown.doc, fixed_id, "y") + corner.y}, {"width", prop(grown.doc, fixed_id, "width") - corner.x},
        {"height", prop(grown.doc, fixed_id, "height") - corner.y}});
    check_rect(fixed_id, fixed_before);
    const auto fixed_after = rect(fixed_id);
    require(std::abs(fixed_after.x + fixed_after.width - fixed_before.x - fixed_before.width) <= .05f &&
        std::abs(fixed_after.y + fixed_after.height - fixed_before.y - fixed_before.height) <= .05f,
        "top-left resize moved the opposite corner");

    step = "undo/redo one resize transaction";
    menus.activate(menus.open(1), 0);
    require(read() == state{grown.doc, resized.revision + 1, true, true, true}, "one Undo did not restore all four properties");
    same_rect(rect(fixed_id), fixed_before, "Undo did not restore the layout");
    menus.activate(menus.open(1), 1);
    const auto final = read();
    require(final == state{resized.doc, resized.revision + 2, true, true, false}, "one Redo did not reapply the full resize");
    same_rect(rect(fixed_id), fixed_after, "Redo did not restore the resized layout");

    step = "Escape cancels move";
    drag(point(fixed_after, .5f, .5f), {35, 32}, cancel::escape);
    require(read() == final, "release after Escape committed a move");
    same_rect(rect(fixed_id), fixed_after, "Escape did not restore the layout");
    step = "focus loss cancels resize";
    drag(point(fixed_after, 1, 1), {37, 25}, cancel::focus);
    require(read() == final, "release after focus loss committed a resize");
    same_rect(rect(fixed_id), fixed_after, "focus loss did not restore the layout");

    step = "flow child cannot move or resize";
    button("designer.preview.fit");
    const auto flow = rect("flow_button");
    click(point(flow, .5f, .5f));
    drag(point(flow, .5f, .5f), {32, 25});
    drag(point(flow, 0, 0), {-28, -24});
    require(read() == final, "flow child acquired geometry properties");
    step = "root cannot move or resize";
    const auto root = rect(initial.doc.root);
    const auto background = point(root, .66f, .83f); // Empty area below the nested panel.
    click(background);
    drag(background, {25, -33});
    drag(point(root, 0, 0), {33, 25});
    require(read() == final, "root acquired geometry properties");
}

} // namespace vgui::ui_design
