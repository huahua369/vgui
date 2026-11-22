#include "ui_designer_gradient_timeline.h"

#include <vgui/editor/color_picker.h>
#include <vgui/editor/combo_box.h>
#include <vgui/editor/gradient_editor.h>
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
    std::string kind;
    bool ellipse = false;
    std::array<float, 4> matrix{};
    std::array<double, gradient_geometry_channels.size()> authored{};
    std::vector<gradient_stop> authored_stops;
    bool operator==(const gradient_track&) const = default;
};
} // namespace

struct designer_gradient_timeline::impl {
    const designer_strings& strings;
    gradient_timeline_commit commit;
    std::function<void(double, bool)> seek;
    std::function<void(std::string)> notify;
    std::function<void()> selection_changed;
    vgui_widget *panel, *add, *remove, *copy, *paste, *fit, *info, *fields, *tint, *loop_widget, *geometry;
    vgui_widget *stops_button, *stops_popup = nullptr, *stops_reset = nullptr, *stops_info = nullptr;
    kit::gradient_editor stop_editor;
    vgui_widget* geometry_popup = nullptr;
    struct geometry_field {
        vgui_widget* row = nullptr;
        vgui_widget* reset = nullptr;
        kit::spinbox value;
    };
    std::array<geometry_field, gradient_geometry_channels.size()> geometry_fields;
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
    struct copied_key { std::string node; double delay; gradient_keyframe pose; };
    std::vector<copied_key> clipboard;
    int shortcut_handler = 0;
    bool updating = false, rejected = false, visible = false, needs_fit = true, fields_dirty = true;

    impl(vgui_widget* parent, const designer_strings& locale, gradient_timeline_commit apply,
            std::function<void(double, bool)> go, std::function<void(std::string)> status,
            std::function<void()> changed)
        : strings(locale), commit(std::move(apply)), seek(std::move(go)), notify(std::move(status)),
          selection_changed(std::move(changed)), shell(vgui_widget_get_context(parent)) {
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
        copy = button(header, "designer.timeline.copy");
        paste = button(header, "designer.timeline.paste");
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
        geometry = button(fields, "designer.timeline.geometry");
        stops_button = button(fields, "designer.timeline.stops");
        vgui_widget_set_tooltip_when_disabled(stops_button, 1);
        timeline.set_keyframe_edit_listener([this](const auto& before, const auto& after) { edit_keys(before, after); });
        timeline.set_playhead_listener([this](double seconds) { if (!updating && !tracks.empty()) seek(seconds, false); });
        timeline.set_keyframe_reselect_listener([this](const auto& key) { if (!rejected) seek_key(key); });
        timeline.set_change_listener([this] { vgui_widget_mark_dirty(timeline.widget()); });
        timeline.set_selection_listener([this](const auto&) {
            fields_dirty = true;
            if (updating) return;
            // Defer field refresh until after dispatch; this may be a color popup closing.
            id track = 0;
            const auto* key = timeline.find_keyframe(timeline.primary_selection(), &track);
            if (key && !rejected) {
                timeline.set_selected_track(track);
                seek_key(*key);
            }
            selection_changed();
            vgui_widget_mark_dirty(timeline.widget());
        });
        shortcut_handler = vgui_context_add_key_handler(shell, [](vgui_context*, int key, int down, int mods, void* user) {
            auto& self = *static_cast<impl*>(user);
            if (!down || !self.visible || !self.timeline.focused() ||
                !(mods & (VGUI_MOD_CTRL | VGUI_MOD_GUI)) || (mods & (VGUI_MOD_SHIFT | VGUI_MOD_ALT))) return 0;
            if (key == 'C' || key == 'c') { self.copy_keys(); return 1; }
            if (key == 'V' || key == 'v') { self.paste_keys(); return 1; }
            return 0;
        }, this);
        refresh_language();
    }

    ~impl() {
        if (shortcut_handler) vgui_context_remove_key_handler(shell, shortcut_handler);
        close_pose_popups();
        if (picker.widget()) vgui_widget_destroy(picker.widget());
        if (geometry_popup) vgui_widget_destroy(geometry_popup);
        if (stops_popup) vgui_widget_destroy(stops_popup);
    }

    void close_pose_popups() {
        auto* popup = vgui_popup_content(shell);
        if (popup && (popup == picker.widget() || popup == geometry_popup || popup == stops_popup)) vgui_popup_close(shell);
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
        else if (w == self.copy) self.copy_keys();
        else if (w == self.paste) self.paste_keys();
        else if (w == self.tint) self.open_picker();
        else if (w == self.geometry) self.open_geometry();
        else if (w == self.stops_button) self.open_stops();
        else if (w == self.stops_reset) self.edit_pose([](auto& frame) { frame.stops.clear(); });
        else {
            for (std::size_t i = 0; i < self.geometry_fields.size(); ++i)
                if (w == self.geometry_fields[i].reset) { self.edit_geometry(i, std::nullopt); break; }
        }
    }

    void refresh_language() {
        vgui_widget_set_text(add, tr("timeline.add", "Add Key"));
        vgui_widget_set_text(remove, tr("timeline.delete", "Delete Key"));
        vgui_widget_set_text(copy, tr("timeline.copy", "Copy"));
        vgui_widget_set_text(paste, tr("timeline.paste", "Paste"));
        vgui_widget_set_tooltip(copy, tr("timeline.copy_hint", "Copy selected keyframes and their poses (Ctrl+C)."));
        vgui_widget_set_tooltip(paste, tr("timeline.paste_hint", "Paste the copied keys at the playhead (Ctrl+V)."));
        vgui_widget_set_text(fit, tr("timeline.fit", "Fit Time"));
        duration_box.set_label(tr("timeline.duration", "Duration"));
        vgui_widget_set_text(loop_widget, tr("timeline.loop", "Loop"));
        timeline.set_track_header(tr("timeline.tracks", "Tracks"));
        vgui_widget_set_a11y_name(timeline.widget(), tr("timeline.title", "Gradient Timeline"));
        vgui_widget_set_tooltip(timeline.widget(), tr("timeline.hint", "Drag the ruler to preview. Drag a diamond to move a key. Scroll to see more tracks."));
        vgui_widget_set_tooltip(duration_box.widget(), tr("timeline.duration_hint", "Duration of the selected gradient animation, in seconds (0.1–60)."));
        vgui_widget_set_tooltip(loop_widget, tr("timeline.loop_hint", "Repeat the selected gradient animation independently."));
        vgui_widget_set_tooltip(tint, tr("timeline.tint", "Edit multiply tint and opacity"));
        vgui_widget_set_text(geometry, tr("timeline.geometry", "Shape…"));
        vgui_widget_set_tooltip(geometry, tr("timeline.geometry_hint", "Edit gradient geometry common to the selected keyframes."));
        refresh_geometry_labels();
        vgui_widget_set_text(stops_button, tr("timeline.stops", "Stops…"));
        vgui_widget_set_tooltip(stops_button, tr("timeline.stops_hint", "Select one key to edit its stop colors and offsets."));
        refresh_stop_labels();
        x.set_label("X");
        y.set_label("Y");
        opacity.set_label(tr("timeline.opacity", "Opacity"));
        easing.set_items({{tr("timeline.step", "Step"), "step"}, {tr("timeline.linear", "Linear"), "linear"},
            {tr("timeline.smooth", "Smooth"), "smooth"}});
        const auto* mixed = tr("timeline.mixed", "Mixed");
        for (auto* box : {&x, &y, &opacity}) vgui_edit_set_placeholder(box->edit_widget(), mixed);
        easing.set_placeholder(mixed);
        fields_dirty = true;
        refresh_fields();
    }

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
        const auto found = poses.find(primary);
        const auto& selection = timeline.selection_ids();
        const bool has_key = found != poses.end() && !rejected;
        const bool one_key = has_key && selection.size() == 1;
        vgui_widget_set_enabled(stops_button, one_key);
        vgui_widget_set_visible(fields, has_key);
        vgui_widget_set_enabled(geometry, has_key && std::ranges::any_of(gradient_geometry_channels,
            [this](const auto& channel) { return supports_geometry(channel); }));
        vgui_widget_set_enabled(add, has_track);
        vgui_widget_set_enabled(remove, has_key);
        vgui_widget_set_enabled(copy, has_key);
        vgui_widget_set_enabled(paste, has_track && !clipboard.empty());
        vgui_widget_set_enabled(fit, !tracks.empty());
        const char* help = tr("timeline.select", "Select a key to edit its pose");
        if (tracks.empty()) help = tr("timeline.empty", "Set a gradient's animation to Keyframes to add a track here.");
        char time[96];
        if (has_key) {
            if (selection.size() > 1) {
                std::snprintf(time, sizeof(time), tr("timeline.selected_count", "%zu keys selected"), selection.size());
            } else {
                const auto* mark = timeline.find_keyframe(primary);
                std::snprintf(time, sizeof(time), "%s: %.3f s", tr("timeline.time", "Key time"), mark ? mark->time : 0.);
            }
            help = time;
        }
        vgui_widget_set_text(info, help);
        if (auto* popup = vgui_popup_content(shell); popup && ((popup == geometry_popup && !has_key) ||
            (popup == stops_popup && !one_key))) vgui_popup_close(shell);
        if (!std::exchange(fields_dirty, false) || !has_key) return;
        if (stops_popup && vgui_popup_content(shell) == stops_popup) refresh_stops();
        if (geometry_popup && vgui_popup_content(shell) == geometry_popup) refresh_geometry();
        const auto& frame = found->second;
        auto x_range = translation_range(0), y_range = translation_range(0);
        bool mixed_x = false, mixed_y = false, mixed_opacity = false, mixed_tint = false, mixed_easing = false;
        for (const auto key : selection) {
            id lane = 0;
            timeline.find_keyframe(key, &lane);
            const auto& layer = model.at(tracks.at(lane));
            const auto& other = poses.at(key);
            const auto intersect = [](auto& range, double base) {
                const auto next = translation_range(base);
                range.first = std::max(range.first, next.first);
                range.second = std::min(range.second, next.second);
            };
            intersect(x_range, layer.tx);
            intersect(y_range, layer.ty);
            mixed_x |= other.x != frame.x;
            mixed_y |= other.y != frame.y;
            mixed_opacity |= other.opacity != frame.opacity;
            mixed_tint |= other.tint != frame.tint;
            mixed_easing |= other.interpolation != frame.interpolation;
        }
        x.set_range(x_range.first, x_range.second);
        y.set_range(y_range.first, y_range.second);
        x.set_value(frame.x);
        y.set_value(frame.y);
        opacity.set_value(frame.opacity);
        x.set_mixed(mixed_x);
        y.set_mixed(mixed_y);
        opacity.set_mixed(mixed_opacity);
        vgui_widget_set_text(tint, mixed_tint ? tr("timeline.mixed", "Mixed") : frame.tint.c_str());
        if (mixed_easing) easing.clear_selection();
        else easing.set_value(frame.interpolation);
    }

    static std::pair<double, double> translation_range(double base) {
        return {std::max(-16., -16. - base), std::min(16., 16. - base)};
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
                    std::get<bool>(property(item, "gradient_animation_loop")), {},
                    std::get<std::string>(property(item, "gradient_kind"))};
                layer.ellipse = std::get<bool>(property(item, "gradient_ellipse"));
                constexpr std::string_view matrix[] = {"gradient_affine_xx", "gradient_affine_yx",
                    "gradient_affine_xy", "gradient_affine_yy"};
                for (std::size_t i = 0; i < layer.matrix.size(); ++i)
                    layer.matrix[i] = static_cast<float>(std::get<double>(property(item, matrix[i])));
                for (std::size_t i = 0; i < layer.authored.size(); ++i)
                    layer.authored[i] = std::get<double>(property(item, gradient_geometry_channels[i].property));
                std::string error;
                decode_gradient_stops(std::get<std::string>(property(item, "gradient_stops")), layer.authored_stops, error);
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
        const auto primary = timeline.primary_selection();
        timeline.find_keyframe(primary, &selected_track);
        std::string keep;
        if (const auto it = tracks.find(selected_track); it != tracks.end()) keep = model[it->second].node;
        std::map<std::pair<std::string, double>, bool> selected;
        for (const auto key : timeline.selection_ids()) {
            id lane = 0;
            if (timeline.find_keyframe(key, &lane) && poses.contains(key))
                selected.emplace(std::pair{model[tracks.at(lane)].node, poses.at(key).offset}, key == primary);
        }
        close_pose_popups();
        updating = true;
        fields_dirty = true;
        std::vector<id> restored;
        id restored_primary = 0;
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
                if (const auto it = selected.find({layer.node, frame.offset}); it != selected.end()) {
                    restored.push_back(key);
                    if (it->second) restored_primary = key;
                }
            }
        }
        timeline.set_selection(restored, restored_primary);
        if (timeline.duration() != duration) needs_fit = true;
        timeline.set_duration(duration);
        updating = false;
    }

    void insert_key() {
        const auto track = timeline.selected_track();
        if (tracks.contains(track)) timeline.insert_keyframe(track, timeline.playhead());
    }

    void copy_keys() {
        if (rejected || timeline.gesture() != kit::timeline_gesture::none) return;
        std::vector<copied_key> next;
        for (const auto key : timeline.selection_ids()) {
            id lane = 0;
            const auto* mark = timeline.find_keyframe(key, &lane);
            if (mark && poses.contains(key)) next.push_back({model.at(tracks.at(lane)).node, mark->time, poses.at(key)});
        }
        if (next.empty()) return;
        std::stable_sort(next.begin(), next.end(), [](const auto& a, const auto& b) { return a.delay < b.delay; });
        const auto first = next.front().delay;
        for (auto& key : next) key.delay -= first;
        clipboard = std::move(next);
    }

    void paste_keys() {
        if (rejected || clipboard.empty() || timeline.gesture() != kit::timeline_gesture::none) return;
        const auto destination = selected_track();
        if (!tracks.contains(destination)) return;
        const bool single_track = std::ranges::all_of(clipboard, [&](const auto& key) { return key.node == clipboard.front().node; });
        std::map<std::string_view, id> lanes;
        if (!single_track)
            for (const auto& [lane, index] : tracks) lanes.emplace(model[index].node, lane);
        std::map<id, std::vector<gradient_keyframe>> changes;
        std::vector<std::pair<id, gradient_keyframe>> inserts;
        for (const auto& key : clipboard) {
            id lane = destination;
            if (!single_track) {
                const auto source = lanes.find(key.node);
                if (source == lanes.end()) {
                    if (notify) notify(tr("timeline.paste_missing", "A source track is missing. No keys were pasted."));
                    return;
                }
                lane = source->second;
            }
            const auto& layer = model.at(tracks.at(lane));
            const auto time = timeline.playhead() + key.delay;
            if (time > std::nextafter(layer.duration, 61.)) {
                if (notify) notify(tr("timeline.paste_outside", "Pasted keys would exceed a track's duration. No keys were pasted."));
                return;
            }
            if (std::abs(layer.tx + key.pose.x) > 16. || std::abs(layer.ty + key.pose.y) > 16.) {
                if (notify) notify(tr("timeline.paste_translation", "A pasted pose exceeds the target's translation range. No keys were pasted."));
                return;
            }
            if (!key.pose.stops.empty() && key.pose.stops.size() != layer.authored_stops.size()) {
                if (notify) notify(tr("timeline.paste_stops", "A copied key has a different number of stops. No keys were pasted."));
                return;
            }
            auto pose = key.pose;
            pose.offset = std::min(time / layer.duration, 1.);
            auto it = changes.find(lane);
            if (it == changes.end()) it = changes.emplace(lane, layer.frames).first;
            it->second.push_back(pose);
            inserts.emplace_back(lane, std::move(pose));
        }
        for (auto& [lane, frames] : changes) {
            std::ranges::sort(frames, {}, &gradient_keyframe::offset);
            if (!valid_spacing(frames)) {
                if (notify) notify(tr("timeline.paste_overlap", "Pasted keys would overlap or exceed 128 keys per track. No keys were pasted."));
                return;
            }
        }
        if (!apply_tracks(changes)) return;
        std::vector<id> keys;
        keys.reserve(inserts.size());
        for (const auto& [lane, pose] : inserts) {
            const auto& layer = model.at(tracks.at(lane));
            const auto key = timeline.add_keyframe(lane, {0, pose.offset * layer.duration, 0, mode(pose.interpolation)});
            poses.emplace(key, pose);
            keys.push_back(key);
        }
        timeline.set_selection(keys, keys.front());
    }

    const gradient_track& track_for_key(id key) const {
        id lane = 0;
        timeline.find_keyframe(key, &lane);
        return model.at(tracks.at(lane));
    }

    bool supports_geometry(const gradient_geometry_channel& channel) const {
        const auto& selection = timeline.selection_ids();
        return !selection.empty() && std::ranges::all_of(selection, [&](id key) {
            return channel.applies_to(track_for_key(key).kind);
        });
    }

    void refresh_geometry_labels() {
        if (!geometry_popup) return;
        for (std::size_t i = 0; i < geometry_fields.size(); ++i) {
            const auto& channel = gradient_geometry_channels[i];
            auto& field = geometry_fields[i];
            const auto key = "workbench.inspector.property." + std::string(channel.property);
            field.value.set_label(tr(key, find_property("gradient", channel.property)->label.data()));
            vgui_widget_set_text(field.reset, tr("timeline.geometry_reset", "Reset"));
            vgui_widget_set_tooltip(field.reset, tr("timeline.geometry_reset_hint", "Restore the authored gradient property."));
        }
    }

    void build_geometry() {
        geometry_popup = vgui_widget_create(shell, VGUI_PANEL, nullptr);
        vgui_widget_set_id(geometry_popup, "designer.timeline.geometry.popup");
        vgui_widget_set_layout(geometry_popup, VGUI_LAYOUT_COLUMN);
        vgui_widget_set_align_items(geometry_popup, VGUI_ALIGN_STRETCH);
        vgui_widget_set_padding(geometry_popup, 8, 8, 8, 8);
        vgui_widget_set_gap(geometry_popup, 4);
        for (std::size_t i = 0; i < geometry_fields.size(); ++i) {
            const auto& channel = gradient_geometry_channels[i];
            auto& field = geometry_fields[i];
            const auto prefix = "designer.timeline.geometry." + std::string(channel.key);
            field.row = row(geometry_popup);
            field.value.set_id(prefix);
            field.value.set_label_width(130);
            field.value.set_edit_min_width(80);
            field.value.set_buttons_visible(false);
            field.value.set_range(channel.min, channel.max);
            field.value.set_step(.01);
            field.value.build(field.row);
            vgui_widget_set_grow(field.value.widget(), 1);
            field.value.set_on_commit([this, i](double value) { edit_geometry(i, value); });
            field.reset = button(field.row, (prefix + ".reset").c_str());
            vgui_widget_set_size(field.reset, 56, 24);
        }
        refresh_geometry_labels();
    }

    void refresh_geometry() {
        const auto key = timeline.primary_selection();
        const auto& primary = poses.at(key);
        const auto& base = track_for_key(key).authored;
        int visible_fields = 0;
        for (std::size_t i = 0; i < geometry_fields.size(); ++i) {
            auto& field = geometry_fields[i];
            const bool supported = supports_geometry(gradient_geometry_channels[i]);
            vgui_widget_set_visible(field.row, supported);
            if (!supported) continue;
            ++visible_fields;
            const auto override = primary.geometry[i];
            const double effective = override.value_or(base[i]);
            bool mixed = false, resettable = false;
            for (const auto selected : timeline.selection_ids()) {
                const auto other = poses.at(selected).geometry[i];
                resettable |= other.has_value();
                mixed |= other != override || other.value_or(track_for_key(selected).authored[i]) != effective;
            }
            field.value.set_value(effective);
            // Inherited numbers are placeholders so explicitly entering the
            // same value still creates a key override rather than a no-op.
            const auto hint = mixed ? std::string(tr("timeline.mixed", "Mixed")) :
                tr("timeline.geometry_base", "Base: ") + field.value.value_text();
            vgui_edit_set_placeholder(field.value.edit_widget(), hint.c_str());
            field.value.set_mixed(mixed || !override);
            vgui_widget_set_enabled(field.reset, resettable);
        }
        vgui_widget_set_size(geometry_popup, 304, 16 + visible_fields * 33 - 4);
    }

    void open_geometry() {
        if (rejected || !poses.contains(timeline.primary_selection()) ||
            !std::ranges::any_of(gradient_geometry_channels, [this](const auto& channel) { return supports_geometry(channel); })) return;
        if (!geometry_popup) build_geometry();
        vgui_theme palette{};
        vgui_context_get_theme(shell, &palette);
        vgui_widget_set_background(geometry_popup, palette.panel_bg);
        vgui_widget_set_border(geometry_popup, palette.panel_border, 1);
        refresh_geometry();
        float x, y, width, height;
        vgui_widget_get_rect(geometry, &x, &y, &width, &height);
        vgui_popup_open_anchored(shell, geometry_popup, x, y, width, height, 4);
    }

    void edit_geometry(std::size_t index, std::optional<double> value) {
        if (supports_geometry(gradient_geometry_channels[index]))
            edit_pose([&](auto& frame) { frame.geometry[index] = value; });
    }

    void refresh_stop_labels() {
        if (!stops_popup) return;
        stop_editor.set_stop_labels(tr("workbench.inspector.gradient_popup.offset", "Offset"),
            tr("workbench.inspector.gradient_popup.stops", "Stops"));
        vgui_widget_set_text(stops_reset, tr("timeline.stops_reset", "Use original"));
        vgui_widget_set_tooltip(stop_editor.bar_widget(), tr("timeline.stops_drag", "Drag a stop to change its offset."));
    }

    void refresh_stops() {
        const auto key = timeline.primary_selection();
        const auto& frame = poses.at(key);
        const auto& stops = frame.stops.empty() ? track_for_key(key).authored_stops : frame.stops;
        kit::gradient_value value;
        for (const auto& stop : stops) value.stops.push_back({stop.offset, stop.color});
        stop_editor.set_value(value);
        vgui_widget_set_enabled(stops_reset, !frame.stops.empty());
        vgui_widget_set_text(stops_info, frame.stops.empty() ? tr("timeline.stops_inherited", "Original stops") :
            tr("timeline.stops_custom", "Keyframe stops"));
    }

    void open_stops() {
        if (rejected || timeline.selection_ids().size() != 1 || !poses.contains(timeline.primary_selection())) return;
        if (!stops_popup) {
            stops_popup = vgui_widget_create(shell, VGUI_PANEL, nullptr);
            vgui_widget_set_id(stops_popup, "designer.timeline.stops.popup");
            vgui_widget_set_layout(stops_popup, VGUI_LAYOUT_COLUMN);
            vgui_widget_set_align_items(stops_popup, VGUI_ALIGN_STRETCH);
            vgui_widget_set_padding(stops_popup, 6, 6, 6, 6);
            vgui_widget_set_gap(stops_popup, 4);
            vgui_panel_set_scrollable(stops_popup, 0, 1);
            auto* header = row(stops_popup);
            stops_info = vgui_widget_add(header, VGUI_LABEL, nullptr);
            vgui_widget_set_grow(stops_info, 1);
            stops_reset = button(header, "designer.timeline.stops.reset");
            stop_editor.set_id("designer.timeline.stops.editor");
            stop_editor.set_stop_limit(max_gradient_stops);
            stop_editor.set_show_stop_list(true);
            stop_editor.set_topology_locked(true);
            stop_editor.build(stops_popup);
            stop_editor.set_on_commit([this](const kit::gradient_value& value) {
                const auto key = timeline.primary_selection();
                if (rejected || vgui_popup_content(shell) != stops_popup || timeline.selection_ids().size() != 1 ||
                    !poses.contains(key)) return;
                const auto& current = poses.at(key).stops;
                const auto& effective = current.empty() ? track_for_key(key).authored_stops : current;
                std::vector<gradient_stop> stops;
                for (const auto& stop : value.stops) stops.push_back({stop.t, stop.color});
                if (std::ranges::equal(stops, effective, [](const auto& a, const auto& b) {
                    return a.offset == b.offset && parse_text_color(a.color) == parse_text_color(b.color);
                })) return;
                edit_pose([&](auto& frame) { frame.stops = stops; });
            });
            refresh_stop_labels();
        }
        refresh_stops();
        vgui_theme palette{};
        vgui_context_get_theme(shell, &palette);
        vgui_widget_set_background(stops_popup, palette.panel_bg);
        vgui_widget_set_border(stops_popup, palette.panel_border, 1);
        int height = 0;
        vgui_context_get_size(shell, nullptr, &height);
        vgui_widget_set_max_size(stops_popup, 0, std::max(120.f, static_cast<float>(height) - 16));
        vgui_panel_set_scroll(stops_popup, 0, 0);
        float x, y, w, h;
        vgui_widget_get_rect(stops_button, &x, &y, &w, &h);
        vgui_popup_open_anchored(shell, stops_popup, x, y, w, h, 4);
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
        picker.set_on_commit([this, previous = *parse_text_color(color)](const std::string& hex) mutable {
            if (const auto packed = parse_text_color(hex)) {
                const bool tint_changed = (*packed & 0xFFFFFFu) != (previous & 0xFFFFFFu);
                const bool opacity_changed = (*packed >> 24) != (previous >> 24);
                previous = *packed;
                edit_pose([&](auto& frame) {
                    if (tint_changed) frame.tint = hex.substr(0, 7);
                    if (opacity_changed) frame.opacity = (*packed >> 24) / 255.;
                });
            }
        });
        float ax, ay, aw, ah;
        vgui_widget_get_rect(tint, &ax, &ay, &aw, &ah);
        vgui_popup_open_anchored(shell, picker.widget(), ax, ay, aw, ah, 4);
    }

    std::optional<shape_keyframe> selected_shape_key() const {
        if (!visible || !revision || rejected || timeline.selection_ids().size() != 1) return {};
        const auto key = timeline.primary_selection();
        id lane = 0;
        const auto* mark = timeline.find_keyframe(key, &lane);
        const auto pose = poses.find(key);
        if (!mark || pose == poses.end()) return {};
        const auto& layer = model.at(tracks.at(lane));
        const bool radial = layer.kind == "radial";
        if (!radial && layer.kind != "linear") return {};
        const auto& frame = pose->second;
        const auto [xx, yx, xy, yy] = layer.matrix;
        shape_keyframe result{*revision, key, layer.node, mark->time, frame.offset == 1, radial, radial && layer.ellipse, {},
            {xx, yx, xy, yy, static_cast<float>(layer.tx + frame.x), static_cast<float>(layer.ty + frame.y)}};
        for (std::size_t i = 0; i < result.shape.size(); ++i)
            if (gradient_geometry_channels[i].applies_to(layer.kind))
                result.shape[i] = frame.geometry[i].value_or(layer.authored[i]);
        return result;
    }

    void edit_shape(const shape_keyframe& key, const keyframe_shape& shape) {
        if (selected_shape_key() != key) return;
        edit_pose([&](auto& frame) {
            for (std::size_t i = 0; i < shape.size(); ++i)
                if (shape[i] != key.shape[i]) frame.geometry[i] = shape[i];
        });
    }

    void seek_key(const kit::timeline_keyframe& key) {
        const auto found = poses.find(key.id);
        if (found != poses.end()) seek(key.time, found->second.offset == 1);
    }

    template<class Change> void edit_pose(Change change) {
        if (rejected) return;
        std::map<id, std::vector<gradient_keyframe>> changes;
        std::map<id, gradient_keyframe> changed_keys;
        for (const auto key : timeline.selection_ids()) {
            const auto it = poses.find(key);
            id lane = 0;
            if (it == poses.end() || !timeline.find_keyframe(key, &lane)) continue;
            auto next = it->second;
            change(next);
            if (next == it->second) continue;
            auto entry = changes.find(lane);
            if (entry == changes.end()) entry = changes.emplace(lane, model.at(tracks.at(lane)).frames).first;
            auto& frames = entry->second;
            const auto pos = std::ranges::find(frames, next.offset, &gradient_keyframe::offset);
            if (pos == frames.end()) return;
            *pos = next;
            changed_keys.emplace(key, std::move(next));
        }
        if (changes.empty() || !apply_tracks(changes)) return;
        for (const auto& [key, pose] : changed_keys) {
            poses.at(key) = pose;
            id lane = 0;
            auto mark = *timeline.find_keyframe(key, &lane);
            if (mark.interpolation != mode(pose.interpolation)) {
                mark.interpolation = mode(pose.interpolation);
                timeline.update_keyframe(lane, mark);
            }
        }
        fields_dirty = true;
        if (const auto* mark = timeline.find_keyframe(timeline.primary_selection())) seek_key(*mark);
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
        if (const auto* lane = timeline.find_track(track)) {
            auto resized = *lane;
            for (auto& key : resized.keyframes) key.time = poses.at(key.id).offset * duration;
            timeline.update_track(resized);
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
        fields_dirty = true;
        return false;
    }

    static bool valid_spacing(const std::vector<gradient_keyframe>& frames) {
        return frames.size() <= max_gradient_keyframes && std::adjacent_find(frames.begin(), frames.end(), [](const auto& a, const auto& b) {
            return b.offset < a.offset + gradient_keyframe_min_gap;
        }) == frames.end();
    }

    bool apply_tracks(std::map<id, std::vector<gradient_keyframe>>& changes) {
        std::vector<edit> commands;
        commands.reserve(changes.size());
        for (const auto& [lane, frames] : changes) commands.emplace_back(set_frame_edit(model.at(tracks.at(lane)), frames));
        if (!apply(commands)) return false;
        for (auto& [lane, frames] : changes) model.at(tracks.at(lane)).frames = std::move(frames);
        return true;
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
        std::map<id, std::vector<gradient_keyframe>> changes;
        for (const auto lane : touched) {
            auto& frames = changes[lane];
            const auto* track = timeline.find_track(lane);
            if (!track) return;
            for (const auto& key : track->keyframes) frames.push_back(projected.at(key.id));
            if (!valid_spacing(frames)) {
                reject(tr("timeline.overlap", "Keys cannot overlap, and each track supports up to 128 keys."));
                return;
            }
        }
        if (!apply_tracks(changes)) return;
        poses = std::move(projected);
        if (before.size() == 1 && after.size() == 1) seek_key(after.front().keyframe);
    }

    void reject(const char* message) {
        rejected = true;
        fields_dirty = true;
        if (notify) notify(message);
    }
};

designer_gradient_timeline::designer_gradient_timeline(vgui_widget* parent, const designer_strings& strings,
        gradient_timeline_commit commit, std::function<void(double, bool)> seek, std::function<void(std::string)> notify,
        std::function<void()> selection_changed)
    : p_(std::make_unique<impl>(parent, strings, std::move(commit), std::move(seek), std::move(notify),
          std::move(selection_changed))) {}
designer_gradient_timeline::~designer_gradient_timeline() = default;
void designer_gradient_timeline::set_visible(bool visible) {
    if (!visible && p_->visible) {
        p_->timeline.cancel_gesture();
        p_->close_pose_popups();
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
std::optional<shape_keyframe> designer_gradient_timeline::selected_shape_key() const { return p_->selected_shape_key(); }
void designer_gradient_timeline::edit_shape(const shape_keyframe& key, const keyframe_shape& shape) {
    p_->edit_shape(key, shape);
}
void designer_gradient_timeline::refresh_language() { p_->refresh_language(); }

} // namespace vgui::ui_design
