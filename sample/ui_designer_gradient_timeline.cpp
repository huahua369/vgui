#include "ui_designer_gradient_timeline.h"

#include <vgui/editor/color_picker.h>
#include <vgui/editor/combo_box.h>
#include <vgui/editor/spinbox.h>
#include <vgui/editor/timeline_view.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <utility>

namespace vgui::ui_design {
namespace kit = vgui_editor_kit;
namespace {
constexpr auto property_key = "gradient_animation_keyframes";
constexpr double ruler_min_duration = .1;
using id = kit::object_id;

const value& property(const node& item, std::string_view key) {
    const auto it = item.props.find(key);
    return it == item.props.end() ? find_property(item.type, key)->default_value : it->second;
}

kit::timeline_interpolation mode(std::string_view name) {
    if (name == "step") return kit::timeline_interpolation::step;
    if (name == "smooth") return kit::timeline_interpolation::smooth;
    return kit::timeline_interpolation::linear;
}

struct gradient_track {
    std::string node, name;
    double duration, tx, ty;
    bool loop = true;
    std::vector<gradient_keyframe> frames;
    bool operator==(const gradient_track&) const = default;
};
} // namespace

struct designer_gradient_timeline::impl {
    const designer_strings& strings;
    gradient_timeline_commit commit;
    std::function<void(double, bool)> seek;
    std::function<void(std::string)> notify;
    vgui_widget *panel, *add, *remove, *fit, *info, *fields, *tint, *loop_widget;
    vgui_context* shell;
    kit::timeline_view timeline;
    kit::spinbox duration_box;
    kit::spinbox x, y, opacity;
    kit::combo_box easing;
    kit::color_picker picker;
    std::optional<std::uint64_t> revision;
    std::string outline;
    std::vector<gradient_track> model;
    std::map<id, std::size_t> tracks;
    std::map<id, gradient_keyframe> poses;
    bool updating = false, rejected = false, visible = false, needs_fit = true;

    impl(vgui_widget* parent, const designer_strings& locale, gradient_timeline_commit apply,
            std::function<void(double, bool)> go, std::function<void(std::string)> status)
        : strings(locale), commit(std::move(apply)), seek(std::move(go)), notify(std::move(status)),
          shell(vgui_widget_get_context(parent)) {
        panel = vgui_widget_add(parent, VGUI_PANEL, nullptr);
        vgui_widget_set_id(panel, "designer.timeline");
        vgui_widget_set_layout(panel, VGUI_LAYOUT_COLUMN);
        vgui_widget_set_align_items(panel, VGUI_ALIGN_STRETCH);
        vgui_widget_set_size(panel, 0, 176);
        vgui_widget_set_min_size(panel, 0, 104);
        vgui_widget_set_padding(panel, 5, 3, 5, 3);
        vgui_widget_set_gap(panel, 3);
        vgui_widget_set_visible(panel, 0);
        auto* header = row(panel);
        add = button(header, "designer.timeline.add");
        remove = button(header, "designer.timeline.delete");
        fit = button(header, "designer.timeline.fit");
        duration_box.set_id("designer.timeline.duration");
        duration_box.set_label_width(62);
        duration_box.set_edit_min_width(48);
        duration_box.set_buttons_visible(false);
        duration_box.set_range(.1, 60);
        duration_box.set_step(.1);
        duration_box.build(header);
        vgui_widget_set_size(duration_box.widget(), 124, 0);
        duration_box.set_on_commit([this](double value) { edit_track_duration(value); });
        loop_widget = vgui_widget_add(header, VGUI_CHECKBOX, nullptr);
        vgui_widget_set_id(loop_widget, "designer.timeline.loop");
        vgui_widget_set_size(loop_widget, 72, 22);
        vgui_widget_on_value(loop_widget, on_loop_value, this);
        info = vgui_widget_add(header, VGUI_LABEL, nullptr);
        vgui_widget_set_grow(info, 1);
        timeline.build(panel);
        timeline.set_transport_visible(false);
        timeline.set_clipboard_shortcuts_enabled(false);
        timeline.set_snap_enabled(false);
        timeline.set_zoom_limits(1, 8000);
        vgui_widget_set_id(timeline.widget(), "designer.timeline.tracks");
        vgui_widget_set_min_size(timeline.widget(), 0, 42);
        fields = row(panel);
        const auto number = [&](kit::spinbox& box, const char* id, float width, double lo, double hi, double gradient_keyframe::*property) {
            box.set_id(id);
            box.set_label_width(width);
            box.set_edit_min_width(48);
            box.set_buttons_visible(false);
            box.set_range(lo, hi);
            box.set_step(.01);
            box.build(fields);
            vgui_widget_set_size(box.widget(), width + 56, 0);
            box.set_on_commit([this, property](double value) { edit_pose([&](auto& frame) { frame.*property = value; }); });
        };
        number(x, "designer.timeline.x", 20, -16, 16, &gradient_keyframe::x);
        number(y, "designer.timeline.y", 20, -16, 16, &gradient_keyframe::y);
        number(opacity, "designer.timeline.opacity", 70, 0, 1, &gradient_keyframe::opacity);
        tint = button(fields, "designer.timeline.tint");
        easing.set_id("designer.timeline.interpolation");
        easing.build(fields);
        easing.set_on_commit([this](int, const std::string& value) {
            edit_pose([&](auto& frame) { frame.interpolation = value; });
        });
        timeline.set_keyframe_edit_listener([this](const auto& before, const auto& after) { edit_keys(before, after); });
        timeline.set_playhead_listener([this](double seconds) { if (!updating && !tracks.empty()) seek(seconds, false); });
        timeline.set_change_listener([this] { vgui_widget_mark_dirty(timeline.widget()); });
        timeline.set_selection_listener([this](const auto&) {
            if (updating) return;
            // Defer field refresh until after dispatch; this may be a color popup closing.
            id track = 0;
            const auto* key = timeline.find_keyframe(timeline.primary_selection(), &track);
            if (key && !rejected) {
                timeline.set_selected_track(track);
                seek_key(*key);
            }
            vgui_widget_mark_dirty(timeline.widget());
        });
        refresh_language();
    }

    ~impl() {
        close_picker();
        if (picker.widget()) vgui_widget_destroy(picker.widget());
    }

    void close_picker() {
        if (picker.widget() && vgui_popup_content(shell) == picker.widget()) vgui_popup_close(shell);
    }

    const char* tr(std::string_view key, const char* fallback) const { return strings.text(key, fallback); }

    static vgui_widget* row(vgui_widget* parent) {
        auto* result = vgui_widget_add(parent, VGUI_PANEL, nullptr);
        vgui_widget_set_layout(result, VGUI_LAYOUT_ROW);
        vgui_widget_set_align_items(result, VGUI_ALIGN_CENTER);
        vgui_widget_set_size(result, 0, 29);
        vgui_widget_set_padding(result, 0, 0, 0, 0);
        vgui_widget_set_gap(result, 6);
        vgui_widget_set_border(result, 0, 0);
        return result;
    }

    vgui_widget* button(vgui_widget* parent, const char* id) {
        auto* w = vgui_widget_add(parent, VGUI_BUTTON, nullptr);
        vgui_widget_set_id(w, id);
        vgui_widget_set_padding(w, 7, 2, 7, 2);
        vgui_widget_on_event(w, event, this);
        return w;
    }

    static void on_loop_value(vgui_widget*, float, void* user) {
        auto& self = *static_cast<impl*>(user);
        if (!self.updating) self.edit_track_loop(vgui_widget_get_checked(self.loop_widget) != 0);
    }

    static void event(vgui_widget* w, const vgui_event* e, void* user) {
        if (e->type != VGUI_EVENT_CLICK) return;
        auto& self = *static_cast<impl*>(user);
        if (w == self.fit) self.fit_view();
        else if (w == self.add) self.insert_key();
        else if (w == self.remove) self.timeline.delete_selection();
        else if (w == self.tint) self.open_picker();
    }

    void refresh_language() {
        vgui_widget_set_text(add, tr("timeline.add", "Add Key"));
        vgui_widget_set_text(remove, tr("timeline.delete", "Delete Key"));
        vgui_widget_set_text(fit, tr("timeline.fit", "Fit Time"));
        duration_box.set_label(tr("timeline.duration", "Duration"));
        vgui_widget_set_text(loop_widget, tr("timeline.loop", "Loop"));
        timeline.set_track_header(tr("timeline.tracks", "Tracks"));
        vgui_widget_set_a11y_name(timeline.widget(), tr("timeline.title", "Gradient Timeline"));
        vgui_widget_set_tooltip(timeline.widget(), tr("timeline.hint", "Drag the ruler to preview. Drag a diamond to move a key. Scroll to see more tracks."));
        vgui_widget_set_tooltip(duration_box.widget(), tr("timeline.duration_hint", "Duration of the selected gradient animation, in seconds (0.1–60)."));
        vgui_widget_set_tooltip(loop_widget, tr("timeline.loop_hint", "Repeat the selected gradient animation independently."));
        vgui_widget_set_tooltip(tint, tr("timeline.tint", "Edit multiply tint and opacity"));
        x.set_label("X");
        y.set_label("Y");
        opacity.set_label(tr("timeline.opacity", "Opacity"));
        easing.set_items({{tr("timeline.step", "Step"), "step"}, {tr("timeline.linear", "Linear"), "linear"},
            {tr("timeline.smooth", "Smooth"), "smooth"}});
        shown_pose.reset();
        refresh_fields();
    }

    std::optional<std::pair<id, gradient_keyframe>> shown_pose;

    void refresh_fields() {
        const auto primary = timeline.primary_selection();
        id selected_lane = 0;
        timeline.find_keyframe(primary, &selected_lane);
        if (!selected_lane) selected_lane = timeline.selected_track();
        const bool has_track = tracks.contains(selected_lane) && !rejected;
        vgui_widget_set_enabled(duration_box.widget(), has_track);
        vgui_widget_set_enabled(loop_widget, has_track);
        if (has_track) {
            const auto& selected_layer = model.at(tracks.at(selected_lane));
            duration_box.set_value(selected_layer.duration);
            vgui_widget_set_checked(loop_widget, selected_layer.loop ? 1 : 0);
            timeline.set_loop(selected_layer.loop);
        }
        const auto key = primary;
        const auto it = poses.find(key);
        const bool has_key = it != poses.end() && !rejected;
        const auto to_show = has_key ? std::optional(std::pair{key, it->second}) : std::nullopt;
        vgui_widget_set_visible(fields, has_key);
        vgui_widget_set_enabled(add, !tracks.empty() && !rejected);
        vgui_widget_set_enabled(remove, has_key);
        vgui_widget_set_enabled(fit, !tracks.empty());
        const char* help = tr("timeline.select", "Select a key to edit its pose");
        if (tracks.empty()) help = tr("timeline.empty", "Set a gradient's animation to Keyframes to add a track here.");
        char time[48];
        if (has_key) {
            id track = 0;
            const auto* mark = timeline.find_keyframe(key, &track);
            std::snprintf(time, sizeof(time), "%s: %.3f s", tr("timeline.time", "Key time"), mark ? mark->time : 0.);
            help = time;
        }
        vgui_widget_set_text(info, help);
        if (shown_pose == to_show) return;
        shown_pose = to_show;
        if (!has_key) return;
        id track = 0;
        timeline.find_keyframe(key, &track);
        const auto& layer = model.at(tracks.at(track));
        const auto& frame = it->second;
        duration_box.set_value(layer.duration);
        vgui_widget_set_checked(loop_widget, layer.loop ? 1 : 0);
        timeline.set_loop(layer.loop);
        x.set_range(std::max(-16., -16. - layer.tx), std::min(16., 16. - layer.tx));
        y.set_range(std::max(-16., -16. - layer.ty), std::min(16., 16. - layer.ty));
        x.set_value(frame.x);
        y.set_value(frame.y);
        opacity.set_value(frame.opacity);
        vgui_widget_set_text(tint, frame.tint.c_str());
        easing.set_value(frame.interpolation);
    }

    void fit_view() {
        float width = 0;
        vgui_widget_get_rect(timeline.widget(), nullptr, nullptr, &width, nullptr);
        if (width <= 150) return;
        auto view = timeline.view_state();
        const auto ruler_start = timeline.content_x_at_time(0) - timeline.time_to_x(0);
        view.pixels_per_second = std::max(1., (width - ruler_start - 24) / timeline.duration());
        view.view_start = -8 / view.pixels_per_second; // Leave the initial diamond fully visible.
        timeline.set_view_state(view);
        needs_fit = false;
    }

    void sync(const document& doc, std::uint64_t rev, std::string_view selected) {
        if (!visible) return;
        if (!revision || *revision != rev || rejected) {
            std::vector<gradient_track> next;
            for (const auto& [node_id, item] : doc.nodes) {
                if (item.type != "gradient" || std::get<std::string>(property(item, "gradient_animation")) != "keyframes") continue;
                gradient_track layer{node_id, item.name.empty() ? node_id : item.name,
                    std::get<double>(property(item, "gradient_animation_duration")),
                    std::get<double>(property(item, "gradient_affine_tx")),
                    std::get<double>(property(item, "gradient_affine_ty")),
                    std::get<bool>(property(item, "gradient_animation_loop")), {}};
                std::string error;
                decode_gradient_keyframes(std::get<std::string>(property(item, property_key)), layer.frames, error);
                next.push_back(std::move(layer));
            }
            if (next != model || rejected) rebuild(std::move(next));
            rejected = false;
            revision = rev;
        }
        if (outline != selected) {
            outline = selected;
            for (const auto& [track, index] : tracks) if (model[index].node == selected) {
                if (timeline.selected_track() != track) {
                    updating = true;
                    timeline.set_selected_track(track);
                    timeline.clear_selection();
                    updating = false;
                }
                break;
            }
        }
        refresh_fields();
    }

    void rebuild(std::vector<gradient_track> next) {
        id selected_track = timeline.selected_track();
        const auto selected_key = timeline.primary_selection();
        timeline.find_keyframe(selected_key, &selected_track);
        std::string keep;
        std::optional<double> offset;
        if (const auto it = tracks.find(selected_track); it != tracks.end()) keep = model[it->second].node;
        if (const auto it = poses.find(selected_key); it != poses.end()) offset = it->second.offset;
        close_picker();
        updating = true;
        timeline.clear_tracks();
        tracks.clear();
        poses.clear();
        model = std::move(next);
        auto duration = ruler_min_duration;
        for (std::size_t i = 0; i < model.size(); ++i) {
            const auto& layer = model[i];
            duration = std::max(duration, layer.duration);
            kit::timeline_track lane;
            lane.name = layer.name;
            const auto track = timeline.add_track(std::move(lane));
            tracks.emplace(track, i);
            if (layer.node == keep) timeline.set_selected_track(track);
            for (const auto& frame : layer.frames) {
                const auto key = timeline.add_keyframe(track, {0, layer.duration * frame.offset, 0, mode(frame.interpolation)});
                poses.emplace(key, frame);
                if (layer.node == keep && offset && frame.offset == *offset) timeline.set_selection({key}, key);
            }
        }
        if (timeline.duration() != duration) needs_fit = true;
        timeline.set_duration(duration);
        updating = false;
    }

    void insert_key() {
        const auto track = timeline.selected_track();
        if (tracks.contains(track)) timeline.insert_keyframe(track, timeline.playhead());
    }

    void open_picker() {
        const auto it = poses.find(timeline.primary_selection());
        if (it == poses.end() || rejected) return;
        picker.set_id("designer.timeline.tint.popup");
        picker.build(shell);
        const auto& frame = it->second;
        char color[10];
        std::snprintf(color, sizeof(color), "%s%02X", frame.tint.c_str(), static_cast<unsigned>(std::lround(frame.opacity * 255)));
        picker.set_value(color);
        picker.set_current(color);
        picker.set_on_commit([this](const std::string& hex) {
            if (const auto packed = parse_text_color(hex)) {
                const auto alpha = static_cast<int>(*packed >> 24);
                edit_pose([&](auto& frame) {
                    frame.tint = hex.substr(0, 7);
                    // A tint-only edit must not quantize the authored opacity.
                    if (alpha != std::lround(frame.opacity * 255)) frame.opacity = alpha / 255.;
                });
            }
        });
        float ax, ay, aw, ah;
        vgui_widget_get_rect(tint, &ax, &ay, &aw, &ah);
        vgui_popup_open_anchored(shell, picker.widget(), ax, ay, aw, ah, 4);
    }

    void seek_key(const kit::timeline_keyframe& key) {
        const auto found = poses.find(key.id);
        if (found != poses.end()) seek(key.time, found->second.offset == 1);
    }

    template<class Change> void edit_pose(Change change) {
        const auto key = timeline.primary_selection();
        auto it = poses.find(key);
        if (rejected || it == poses.end()) return;
        id lane = 0;
        const auto* mark = timeline.find_keyframe(key, &lane);
        if (!mark) return;
        const auto old = it->second;
        auto next = old;
        change(next);
        if (next == old) return;
        auto& layer = model.at(tracks.at(lane));
        auto frames = layer.frames;
        const auto pos = std::ranges::find(frames, old.offset, &gradient_keyframe::offset);
        if (pos == frames.end()) return;
        *pos = next;
        if (apply({set_frame_edit(layer, frames)})) {
            layer.frames = std::move(frames);
            it->second = next;
            auto display = *mark;
            display.interpolation = mode(next.interpolation);
            timeline.update_keyframe(lane, display);
            seek_key(display);
        }
    }

    static set_property set_animation_duration(const gradient_track& layer, double value) {
        const auto* desc = find_property("gradient", "gradient_animation_duration");
        return {layer.node, "gradient_animation_duration", value == std::get<double>(desc->default_value)
            ? std::nullopt : std::optional<vgui::ui_design::value>(value)};
    }

    static set_property set_animation_loop(const gradient_track& layer, bool value) {
        const auto* desc = find_property("gradient", "gradient_animation_loop");
        return {layer.node, "gradient_animation_loop", value == std::get<bool>(desc->default_value)
            ? std::nullopt : std::optional<vgui::ui_design::value>(value)};
    }

    id selected_track() const {
        id track = 0;
        timeline.find_keyframe(timeline.primary_selection(), &track);
        return track ? track : timeline.selected_track();
    }

    void edit_track_duration(double duration) {
        const auto track = selected_track();
        if (rejected || !tracks.contains(track)) return;
        auto& layer = model.at(tracks.at(track));
        if (std::abs(layer.duration - duration) < 1e-9) return;
        if (!apply({set_animation_duration(layer, duration)})) return;
        layer.duration = duration;
        if (auto* lane = timeline.find_track(track)) {
            for (auto key : lane->keyframes) {
                const auto pose = poses.find(key.id);
                if (pose == poses.end()) continue;
                key.time = pose->second.offset * duration;
                timeline.update_keyframe(track, key);
            }
        }
        auto axis = ruler_min_duration;
        for (const auto& item : model) axis = std::max(axis, item.duration);
        if (std::abs(timeline.duration() - axis) > 1e-9) needs_fit = true;
        timeline.set_duration(axis);
        duration_box.set_value(duration);
    }

    void edit_track_loop(bool loop) {
        const auto track = selected_track();
        if (rejected || !tracks.contains(track)) return;
        auto& layer = model.at(tracks.at(track));
        if (layer.loop == loop) return;
        if (!apply({set_animation_loop(layer, loop)})) return;
        layer.loop = loop;
        timeline.set_loop(loop);
    }

    static set_property set_frame_edit(const gradient_track& layer, const std::vector<gradient_keyframe>& frames) {
        return {layer.node, property_key, frames.empty() ? std::nullopt : std::optional<value>(encode_gradient_keyframes(frames))};
    }

    bool apply(const std::vector<edit>& commands) {
        if (revision && !commands.empty() && commit && commit(*revision, commands)) return true;
        rejected = true;
        shown_pose.reset();
        return false;
    }

    void edit_keys(const std::vector<kit::timeline_view::keyframe_ref>& before,
            const std::vector<kit::timeline_view::keyframe_ref>& after) {
        if (rejected) return;
        auto projected = poses;
        std::set<id> touched;
        for (const auto& ref : before) {
            projected.erase(ref.keyframe.id);
            touched.emplace(ref.track);
        }
        for (const auto& ref : after) {
            const auto& layer = model.at(tracks.at(ref.track));
            const auto offset = ref.keyframe.time / layer.duration;
            if (offset > 1 || offset < 0) {
                reject(tr("timeline.outside", "Move the key inside its track's duration."));
                return;
            }
            auto frame = gradient_keyframe{};
            if (const auto found = poses.find(ref.keyframe.id); found != poses.end()) frame = found->second;
            frame.offset = offset;
            projected.insert_or_assign(ref.keyframe.id, std::move(frame));
            touched.emplace(ref.track);
        }
        std::vector<edit> commands;
        std::map<id, std::vector<gradient_keyframe>> changes;
        for (const auto lane : touched) {
            auto& frames = changes[lane];
            const auto* track = timeline.find_track(lane);
            if (!track) return;
            for (const auto& key : track->keyframes) frames.push_back(projected.at(key.id));
            if (frames.size() > max_gradient_keyframes || std::adjacent_find(frames.begin(), frames.end(), [](const auto& a, const auto& b) {
                    return b.offset < a.offset + gradient_keyframe_min_gap;
                }) != frames.end()) {
                reject(tr("timeline.overlap", "Keys cannot overlap, and each track supports up to 128 keys."));
                return;
            }
            commands.emplace_back(set_frame_edit(model.at(tracks.at(lane)), frames));
        }
        if (!apply(commands)) return;
        poses = std::move(projected);
        for (auto& [lane, frames] : changes) model.at(tracks.at(lane)).frames = std::move(frames);
        if (before.size() == 1 && after.size() == 1) seek_key(after.front().keyframe);
    }

    void reject(const char* message) {
        rejected = true;
        shown_pose.reset();
        if (notify) notify(message);
    }
};

designer_gradient_timeline::designer_gradient_timeline(vgui_widget* parent, const designer_strings& strings,
        gradient_timeline_commit commit, std::function<void(double, bool)> seek, std::function<void(std::string)> notify)
    : p_(std::make_unique<impl>(parent, strings, std::move(commit), std::move(seek), std::move(notify))) {}
designer_gradient_timeline::~designer_gradient_timeline() = default;
void designer_gradient_timeline::set_visible(bool visible) {
    if (!visible && p_->visible) {
        p_->timeline.cancel_gesture();
        p_->close_picker();
    }
    p_->visible = visible;
    vgui_widget_set_visible(p_->panel, visible);
}
void designer_gradient_timeline::sync(const document& doc, std::uint64_t revision, std::string_view selected) { p_->sync(doc, revision, selected); }
void designer_gradient_timeline::show_time(double time) {
    if (!p_->visible) return;
    if (p_->needs_fit) {
        vgui_layout(p_->shell);
        p_->fit_view();
    }
    p_->updating = true;
    p_->timeline.set_playhead(time);
    p_->updating = false;
}
void designer_gradient_timeline::refresh_language() { p_->refresh_language(); }

} // namespace vgui::ui_design
