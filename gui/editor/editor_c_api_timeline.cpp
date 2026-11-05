/* gui/editor/editor_c_api_timeline.cpp - C facade for the timeline view declared
 * in <vgui/vgui_editor.h>.
 *
 * The C++ view owns the animation model, so the track/keyframe structs only
 * travel across this boundary as copies: add/update convert a caller struct into
 * a kit record (the keyframe array is copied wholesale) and track_at/find_track
 * fill handle-owned scratch whose pointers the caller borrows until the next
 * mutation of the same handle.  Time is double throughout, so no precision is
 * lost in conversion.  Listener callbacks capture fn/user by value, never the
 * handle, because a shell-borrowed handle can die before the view. */
#include "editor_c_api.h"
#include "editor_c_api_borrow.h"

#include <new>
#include <utility>
#include <vector>

#include <vgui/editor/timeline_view.h>

using namespace vgui_editor_kit;

/* -------------------------------------------------------------------------- */
/* Handle storage                                                            */
/* -------------------------------------------------------------------------- */

struct vgui_editor_timeline : vgeditor::c_object<timeline_view> {
	using c_object::c_object;

	/* Borrowed-string/array scratch for the last track_at() or find_track() call.
	 * The vector is resized before it is filled, so out->keyframes stays valid. */
	std::string scratch;
	std::vector<vgui_editor_timeline_keyframe> keyframes;

	vgeditor::callback_slot<vgui_editor_timeline_selection_fn> selection_slot;
	vgeditor::callback_slot<vgui_editor_timeline_change_fn> change_slot;
	vgeditor::callback_slot<vgui_editor_timeline_playhead_fn> playhead_slot;
};

namespace {

	static_assert((int)VGUI_EDITOR_TIMELINE_STEP == (int)timeline_interpolation::step,
		"interpolation drift");
	static_assert((int)VGUI_EDITOR_TIMELINE_LINEAR == (int)timeline_interpolation::linear,
		"interpolation drift");
	static_assert((int)VGUI_EDITOR_TIMELINE_SMOOTH == (int)timeline_interpolation::smooth,
		"interpolation drift");

	static_assert((int)VGUI_EDITOR_TIMELINE_GESTURE_NONE ==
		(int)timeline_gesture::none, "gesture drift");
	static_assert((int)VGUI_EDITOR_TIMELINE_GESTURE_PENDING_MOVE ==
		(int)timeline_gesture::pending_move, "gesture drift");
	static_assert((int)VGUI_EDITOR_TIMELINE_GESTURE_MOVE ==
		(int)timeline_gesture::move, "gesture drift");
	static_assert((int)VGUI_EDITOR_TIMELINE_GESTURE_PENDING_MARQUEE ==
		(int)timeline_gesture::pending_marquee, "gesture drift");
	static_assert((int)VGUI_EDITOR_TIMELINE_GESTURE_MARQUEE ==
		(int)timeline_gesture::marquee, "gesture drift");
	static_assert((int)VGUI_EDITOR_TIMELINE_GESTURE_PAN ==
		(int)timeline_gesture::pan, "gesture drift");
	static_assert((int)VGUI_EDITOR_TIMELINE_GESTURE_SCRUB ==
		(int)timeline_gesture::scrub, "gesture drift");

	timeline_interpolation to_interpolation(vgui_editor_timeline_interpolation mode)
	{
		switch (mode) {
		case VGUI_EDITOR_TIMELINE_STEP:
			return timeline_interpolation::step;
		case VGUI_EDITOR_TIMELINE_SMOOTH:
			return timeline_interpolation::smooth;
		default:
			return timeline_interpolation::linear;
		}
	}

	vgui_editor_timeline_interpolation from_interpolation(timeline_interpolation mode)
	{
		switch (mode) {
		case timeline_interpolation::step:
			return VGUI_EDITOR_TIMELINE_STEP;
		case timeline_interpolation::smooth:
			return VGUI_EDITOR_TIMELINE_SMOOTH;
		default:
			return VGUI_EDITOR_TIMELINE_LINEAR;
		}
	}

	vgui_editor_timeline_gesture from_gesture(timeline_gesture gesture)
	{
		switch (gesture) {
		case timeline_gesture::pending_move:
			return VGUI_EDITOR_TIMELINE_GESTURE_PENDING_MOVE;
		case timeline_gesture::move:
			return VGUI_EDITOR_TIMELINE_GESTURE_MOVE;
		case timeline_gesture::pending_marquee:
			return VGUI_EDITOR_TIMELINE_GESTURE_PENDING_MARQUEE;
		case timeline_gesture::marquee:
			return VGUI_EDITOR_TIMELINE_GESTURE_MARQUEE;
		case timeline_gesture::pan:
			return VGUI_EDITOR_TIMELINE_GESTURE_PAN;
		case timeline_gesture::scrub:
			return VGUI_EDITOR_TIMELINE_GESTURE_SCRUB;
		default:
			return VGUI_EDITOR_TIMELINE_GESTURE_NONE;
		}
	}

	/* The keyframe struct has no struct_size field, so it converts field by field
	 * instead of through take_desc(). */
	timeline_keyframe to_keyframe(const vgui_editor_timeline_keyframe& src)
	{
		timeline_keyframe out;
		out.id = (object_id)src.id;
		out.time = src.time;
		out.value = src.value;
		out.interpolation = to_interpolation(src.interpolation);
		return out;
	}

	void fill_keyframe(const timeline_keyframe& src, vgui_editor_timeline_keyframe* out)
	{
		out->id = (vgui_editor_object_id)src.id;
		out->time = src.time;
		out->value = src.value;
		out->interpolation = from_interpolation(src.interpolation);
	}

	timeline_track to_track(const vgui_editor_timeline_track* src)
	{
		timeline_track out;
		if (!src)
			return out;
		const vgui_editor_timeline_track d = vgeditor::take_desc(src);
		out.id = (object_id)d.id;
		out.name = vgeditor::to_text(d.name);
		out.visible = d.visible != 0;
		out.locked = d.locked != 0;
		out.muted = d.muted != 0;
		out.height = d.height;
		out.color = d.color;
		if (d.keyframes) {
			out.keyframes.reserve(d.keyframe_count);
			for (size_t i = 0; i < d.keyframe_count; ++i)
				out.keyframes.push_back(to_keyframe(d.keyframes[i]));
		}
		return out;
	}

	/* Copies a kit track into the caller struct, pointing borrowed fields at the
	 * handle scratch (name) and the resized scratch keyframe array. */
	void fill_track(vgui_editor_timeline* timeline, const timeline_track& src,
		vgui_editor_timeline_track* out)
	{
		std::memset(out, 0, sizeof(*out));
		out->struct_size = (uint32_t)sizeof(*out);
		out->id = (vgui_editor_object_id)src.id;
		timeline->scratch = src.name;
		out->name = timeline->scratch.c_str();
		out->visible = src.visible ? 1 : 0;
		out->locked = src.locked ? 1 : 0;
		out->muted = src.muted ? 1 : 0;
		out->height = src.height;
		out->color = src.color;

		const size_t count = src.keyframes.size();
		timeline->keyframes.clear();
		timeline->keyframes.resize(count);
		for (size_t i = 0; i < count; ++i)
			fill_keyframe(src.keyframes[i], &timeline->keyframes[i]);
		out->keyframes = timeline->keyframes.empty() ? nullptr :
			timeline->keyframes.data();
		out->keyframe_count = count;
	}

} /* namespace */

extern "C" {

	void vgui_editor_timeline_track_default(vgui_editor_timeline_track* track)
	{
		if (!track)
			return;
		std::memset(track, 0, sizeof(*track));
		track->struct_size = (uint32_t)sizeof(*track);
		track->id = VGUI_EDITOR_INVALID_OBJECT;
		track->name = nullptr;
		track->visible = 1;
		track->locked = 0;
		track->muted = 0;
		track->height = 26.0f;
		track->color = 0xFF5FA8FF;
		track->keyframes = nullptr;
		track->keyframe_count = 0;
	}

	void vgui_editor_timeline_keyframe_default(
		vgui_editor_timeline_keyframe* keyframe)
	{
		if (!keyframe)
			return;
		keyframe->id = VGUI_EDITOR_INVALID_OBJECT;
		keyframe->time = 0.0;
		keyframe->value = 0.0;
		keyframe->interpolation = VGUI_EDITOR_TIMELINE_LINEAR;
	}

	void vgui_editor_timeline_view_state_default(
		vgui_editor_timeline_view_state* state)
	{
		if (!state)
			return;
		std::memset(state, 0, sizeof(*state));
		state->struct_size = (uint32_t)sizeof(*state);
		state->view_start = 0.0;
		state->pixels_per_second = 120.0;
		state->scroll_y = 0.0f;
	}

	vgui_editor_timeline* vgui_editor_timeline_create(vgui_widget* parent)
	{
		if (!parent)
			return nullptr;
		std::unique_ptr<vgui_editor_timeline> timeline(
			vgeditor::own_object<vgui_editor_timeline>(
				new (std::nothrow) timeline_view()));
		if (!timeline)
			return nullptr;
		timeline->impl.build(parent);
		if (!timeline->impl.widget())
			return nullptr;
		return timeline.release();
	}

	void vgui_editor_timeline_destroy(vgui_editor_timeline* timeline)
	{
		/* A shell-borrowed handle must not tear down the shell's timeline. */
		if (!timeline || !timeline->owns_object())
			return;
		timeline->impl.set_selection_listener(nullptr);
		timeline->impl.set_change_listener(nullptr);
		timeline->impl.set_playhead_listener(nullptr);
		timeline->impl.set_command_stack(nullptr);
		timeline->selection_slot.clear();
		timeline->change_slot.clear();
		timeline->playhead_slot.clear();
		vgui_widget* widget = timeline->impl.widget();
		delete timeline;
		if (widget)
			vgui_widget_destroy(widget);
	}

	vgui_widget* vgui_editor_timeline_widget(vgui_editor_timeline* timeline)
	{
		return timeline ? timeline->impl.widget() : nullptr;
	}

	void vgui_editor_timeline_focus(vgui_editor_timeline* timeline)
	{
		if (!timeline)
			return;
		timeline->impl.request_focus();
	}

	int vgui_editor_timeline_focused(vgui_editor_timeline* timeline)
	{
		return timeline && timeline->impl.focused() ? 1 : 0;
	}

	int vgui_editor_timeline_built(vgui_editor_timeline* timeline)
	{
		return timeline && timeline->impl.built() ? 1 : 0;
	}

	void vgui_editor_timeline_set_enabled(vgui_editor_timeline* timeline,
		int enabled)
	{
		if (!timeline)
			return;
		timeline->impl.set_enabled(enabled != 0);
	}

	int vgui_editor_timeline_enabled(vgui_editor_timeline* timeline)
	{
		return timeline && timeline->impl.enabled() ? 1 : 0;
	}

	void vgui_editor_timeline_set_drag_threshold(vgui_editor_timeline* timeline,
		float pixels)
	{
		if (!timeline)
			return;
		timeline->impl.set_drag_threshold(pixels);
	}

	float vgui_editor_timeline_drag_threshold(vgui_editor_timeline* timeline)
	{
		return timeline ? timeline->impl.drag_threshold() : 0.0f;
	}

	vgui_editor_object_id vgui_editor_timeline_add_track(
		vgui_editor_timeline* timeline, const vgui_editor_timeline_track* track)
	{
		if (!timeline || !track)
			return VGUI_EDITOR_INVALID_OBJECT;
		return (vgui_editor_object_id)timeline->impl.add_track(to_track(track));
	}

	int vgui_editor_timeline_update_track(vgui_editor_timeline* timeline,
		const vgui_editor_timeline_track* track)
	{
		if (!timeline || !track)
			return 0;
		return timeline->impl.update_track(to_track(track)) ? 1 : 0;
	}

	int vgui_editor_timeline_remove_track(vgui_editor_timeline* timeline,
		vgui_editor_object_id id)
	{
		if (!timeline)
			return 0;
		return timeline->impl.remove_track((object_id)id) ? 1 : 0;
	}

	void vgui_editor_timeline_clear_tracks(vgui_editor_timeline* timeline)
	{
		if (!timeline)
			return;
		timeline->impl.clear_tracks();
	}

	size_t vgui_editor_timeline_track_count(vgui_editor_timeline* timeline)
	{
		return timeline ? timeline->impl.track_count() : 0;
	}

	int vgui_editor_timeline_track_at(vgui_editor_timeline* timeline, size_t index,
		vgui_editor_timeline_track* out)
	{
		if (!timeline || !out)
			return 0;
		const std::vector<timeline_track>& tracks = timeline->impl.tracks();
		if (index >= tracks.size())
			return 0;
		fill_track(timeline, tracks[index], out);
		return 1;
	}

	int vgui_editor_timeline_find_track(vgui_editor_timeline* timeline,
		vgui_editor_object_id id, vgui_editor_timeline_track* out)
	{
		if (!timeline || !out)
			return 0;
		const timeline_track* found = timeline->impl.find_track((object_id)id);
		if (!found)
			return 0;
		fill_track(timeline, *found, out);
		return 1;
	}

	vgui_editor_object_id vgui_editor_timeline_add_keyframe(
		vgui_editor_timeline* timeline, vgui_editor_object_id track,
		const vgui_editor_timeline_keyframe* keyframe)
	{
		if (!timeline || !keyframe)
			return VGUI_EDITOR_INVALID_OBJECT;
		return (vgui_editor_object_id)timeline->impl.add_keyframe((object_id)track,
			to_keyframe(*keyframe));
	}

	int vgui_editor_timeline_update_keyframe(vgui_editor_timeline* timeline,
		vgui_editor_object_id track, const vgui_editor_timeline_keyframe* keyframe)
	{
		if (!timeline || !keyframe)
			return 0;
		return timeline->impl.update_keyframe((object_id)track,
			to_keyframe(*keyframe)) ? 1 : 0;
	}

	int vgui_editor_timeline_remove_keyframe(vgui_editor_timeline* timeline,
		vgui_editor_object_id track, vgui_editor_object_id keyframe)
	{
		if (!timeline)
			return 0;
		return timeline->impl.remove_keyframe((object_id)track,
			(object_id)keyframe) ? 1 : 0;
	}

	size_t vgui_editor_timeline_keyframe_count(vgui_editor_timeline* timeline,
		vgui_editor_object_id track)
	{
		if (!timeline)
			return 0;
		const timeline_track* found = timeline->impl.find_track((object_id)track);
		return found ? found->keyframes.size() : 0;
	}

	int vgui_editor_timeline_keyframe_at(vgui_editor_timeline* timeline,
		vgui_editor_object_id track, size_t index,
		vgui_editor_timeline_keyframe* out)
	{
		if (!timeline || !out)
			return 0;
		const timeline_track* found = timeline->impl.find_track((object_id)track);
		if (!found || index >= found->keyframes.size())
			return 0;
		fill_keyframe(found->keyframes[index], out);
		return 1;
	}

	int vgui_editor_timeline_find_keyframe(vgui_editor_timeline* timeline,
		vgui_editor_object_id keyframe, vgui_editor_object_id* out_track,
		vgui_editor_timeline_keyframe* out)
	{
		if (!timeline)
			return 0;
		object_id track_id = invalid_object;
		const timeline_keyframe* found = timeline->impl.find_keyframe(
			(object_id)keyframe, &track_id);
		if (!found)
			return 0;
		if (out_track)
			*out_track = (vgui_editor_object_id)track_id;
		if (out)
			fill_keyframe(*found, out);
		return 1;
	}

	double vgui_editor_timeline_evaluate(vgui_editor_timeline* timeline,
		vgui_editor_object_id track, double time)
	{
		if (!timeline)
			return 0.0;
		return timeline->impl.evaluate((object_id)track, time);
	}

	void vgui_editor_timeline_set_selection(vgui_editor_timeline* timeline,
		const vgui_editor_object_id* ids, size_t id_count,
		vgui_editor_object_id primary)
	{
		if (!timeline)
			return;
		std::vector<object_id> vec;
		if (ids)
			vec.assign(ids, ids + id_count);
		timeline->impl.set_selection(vec, (object_id)primary);
	}

	void vgui_editor_timeline_clear_selection(vgui_editor_timeline* timeline)
	{
		if (!timeline)
			return;
		timeline->impl.clear_selection();
	}

	void vgui_editor_timeline_select_all(vgui_editor_timeline* timeline)
	{
		if (!timeline)
			return;
		timeline->impl.select_all();
	}

	size_t vgui_editor_timeline_selection_count(vgui_editor_timeline* timeline)
	{
		if (!timeline)
			return 0;
		return timeline->impl.selection_ids().size();
	}

	vgui_editor_object_id vgui_editor_timeline_selection_id_at(
		vgui_editor_timeline* timeline, size_t index)
	{
		if (!timeline)
			return VGUI_EDITOR_INVALID_OBJECT;
		const std::vector<object_id>& ids = timeline->impl.selection_ids();
		if (index >= ids.size())
			return VGUI_EDITOR_INVALID_OBJECT;
		return (vgui_editor_object_id)ids[index];
	}

	vgui_editor_object_id vgui_editor_timeline_primary_selection(
		vgui_editor_timeline* timeline)
	{
		if (!timeline)
			return VGUI_EDITOR_INVALID_OBJECT;
		return (vgui_editor_object_id)timeline->impl.primary_selection();
	}

	int vgui_editor_timeline_is_selected(vgui_editor_timeline* timeline,
		vgui_editor_object_id id)
	{
		if (!timeline)
			return 0;
		return timeline->impl.is_selected((object_id)id) ? 1 : 0;
	}

	void vgui_editor_timeline_set_multi_select(vgui_editor_timeline* timeline,
		int enabled)
	{
		if (!timeline)
			return;
		timeline->impl.set_multi_select(enabled != 0);
	}

	int vgui_editor_timeline_multi_select(vgui_editor_timeline* timeline)
	{
		return timeline && timeline->impl.multi_select() ? 1 : 0;
	}

	void vgui_editor_timeline_set_on_selection(vgui_editor_timeline* timeline,
		vgui_editor_timeline_selection_fn fn, void* user)
	{
		if (!timeline)
			return;
		if (!fn) {
			timeline->selection_slot.clear();
			timeline->impl.set_selection_listener(nullptr);
			return;
		}
		timeline->selection_slot.set(fn, user, nullptr);
		const vgui_editor_timeline_selection_fn slot_fn = timeline->selection_slot.fn;
		void* slot_user = timeline->selection_slot.user;
		vgeditor::user_owner_ptr owner = timeline->selection_slot.owner;
		timeline->impl.set_selection_listener(
			[slot_fn, slot_user, owner](const std::vector<object_id>& ids) {
				slot_fn(slot_user, ids.empty() ? nullptr : ids.data(), ids.size());
			});
	}

	vgui_editor_object_id vgui_editor_timeline_selected_track(
		vgui_editor_timeline* timeline)
	{
		if (!timeline)
			return VGUI_EDITOR_INVALID_OBJECT;
		return (vgui_editor_object_id)timeline->impl.selected_track();
	}

	void vgui_editor_timeline_set_selected_track(vgui_editor_timeline* timeline,
		vgui_editor_object_id track)
	{
		if (!timeline)
			return;
		timeline->impl.set_selected_track((object_id)track);
	}

	void vgui_editor_timeline_set_on_change(vgui_editor_timeline* timeline,
		vgui_editor_timeline_change_fn fn, void* user)
	{
		if (!timeline)
			return;
		if (!fn) {
			timeline->change_slot.clear();
			timeline->impl.set_change_listener(nullptr);
			return;
		}
		timeline->change_slot.set(fn, user, nullptr);
		const vgui_editor_timeline_change_fn slot_fn = timeline->change_slot.fn;
		void* slot_user = timeline->change_slot.user;
		vgeditor::user_owner_ptr owner = timeline->change_slot.owner;
		timeline->impl.set_change_listener([slot_fn, slot_user, owner]() {
			slot_fn(slot_user);
		});
	}

	void vgui_editor_timeline_get_view_state(vgui_editor_timeline* timeline,
		vgui_editor_timeline_view_state* out)
	{
		if (!timeline || !out)
			return;
		const timeline_view_state view = timeline->impl.view_state();
		std::memset(out, 0, sizeof(*out));
		out->struct_size = (uint32_t)sizeof(*out);
		out->view_start = view.view_start;
		out->pixels_per_second = view.pixels_per_second;
		out->scroll_y = view.scroll_y;
	}

	void vgui_editor_timeline_set_view_state(vgui_editor_timeline* timeline,
		const vgui_editor_timeline_view_state* state)
	{
		if (!timeline || !state)
			return;
		const vgui_editor_timeline_view_state d = vgeditor::take_desc(state);
		timeline_view_state view;
		view.view_start = d.view_start;
		view.pixels_per_second = d.pixels_per_second;
		view.scroll_y = d.scroll_y;
		timeline->impl.set_view_state(view);
	}

	double vgui_editor_timeline_zoom(vgui_editor_timeline* timeline)
	{
		return timeline ? timeline->impl.zoom() : 0.0;
	}

	void vgui_editor_timeline_set_zoom_limits(vgui_editor_timeline* timeline,
		double minimum, double maximum)
	{
		if (!timeline)
			return;
		timeline->impl.set_zoom_limits(minimum, maximum);
	}

	void vgui_editor_timeline_zoom_at(vgui_editor_timeline* timeline, float lane_x,
		double factor)
	{
		if (!timeline)
			return;
		timeline->impl.zoom_at(lane_x, factor);
	}

	void vgui_editor_timeline_pan_by_pixels(vgui_editor_timeline* timeline,
		float delta_pixels)
	{
		if (!timeline)
			return;
		timeline->impl.pan_by_pixels(delta_pixels);
	}

	void vgui_editor_timeline_reset_view(vgui_editor_timeline* timeline)
	{
		if (!timeline)
			return;
		timeline->impl.reset_view();
	}

	double vgui_editor_timeline_time_to_x(vgui_editor_timeline* timeline,
		double time)
	{
		return timeline ? timeline->impl.time_to_x(time) : 0.0;
	}

	double vgui_editor_timeline_x_to_time(vgui_editor_timeline* timeline,
		double lane_x)
	{
		return timeline ? timeline->impl.x_to_time(lane_x) : 0.0;
	}

	double vgui_editor_timeline_time_at_content_x(vgui_editor_timeline* timeline,
		float content_x)
	{
		return timeline ? timeline->impl.time_at_content_x(content_x) : 0.0;
	}

	float vgui_editor_timeline_content_x_at_time(vgui_editor_timeline* timeline,
		double time)
	{
		return timeline ? timeline->impl.content_x_at_time(time) : 0.0f;
	}

	void vgui_editor_timeline_set_snap_enabled(vgui_editor_timeline* timeline,
		int enabled)
	{
		if (!timeline)
			return;
		timeline->impl.set_snap_enabled(enabled != 0);
	}

	int vgui_editor_timeline_snap_enabled(vgui_editor_timeline* timeline)
	{
		return timeline && timeline->impl.snap_enabled() ? 1 : 0;
	}

	void vgui_editor_timeline_set_snap_step(vgui_editor_timeline* timeline,
		double seconds)
	{
		if (!timeline)
			return;
		timeline->impl.set_snap_step(seconds);
	}

	double vgui_editor_timeline_snap_step(vgui_editor_timeline* timeline)
	{
		return timeline ? timeline->impl.snap_step() : 0.0;
	}

	void vgui_editor_timeline_set_snap_to_frames(vgui_editor_timeline* timeline,
		int enabled)
	{
		if (!timeline)
			return;
		timeline->impl.set_snap_to_frames(enabled != 0);
	}

	int vgui_editor_timeline_snap_to_frames(vgui_editor_timeline* timeline)
	{
		return timeline && timeline->impl.snap_to_frames() ? 1 : 0;
	}

	double vgui_editor_timeline_snap_time(vgui_editor_timeline* timeline,
		double time)
	{
		return timeline ? timeline->impl.snap_time(time) : 0.0;
	}

	void vgui_editor_timeline_set_frame_rate(vgui_editor_timeline* timeline,
		double fps)
	{
		if (!timeline)
			return;
		timeline->impl.set_frame_rate(fps);
	}

	double vgui_editor_timeline_frame_rate(vgui_editor_timeline* timeline)
	{
		return timeline ? timeline->impl.frame_rate() : 0.0;
	}

	void vgui_editor_timeline_set_duration(vgui_editor_timeline* timeline,
		double seconds)
	{
		if (!timeline)
			return;
		timeline->impl.set_duration(seconds);
	}

	double vgui_editor_timeline_duration(vgui_editor_timeline* timeline)
	{
		return timeline ? timeline->impl.duration() : 0.0;
	}

	void vgui_editor_timeline_set_playhead(vgui_editor_timeline* timeline,
		double seconds)
	{
		if (!timeline)
			return;
		timeline->impl.set_playhead(seconds);
	}

	double vgui_editor_timeline_playhead(vgui_editor_timeline* timeline)
	{
		return timeline ? timeline->impl.playhead() : 0.0;
	}

	void vgui_editor_timeline_play(vgui_editor_timeline* timeline)
	{
		if (!timeline)
			return;
		timeline->impl.play();
	}

	void vgui_editor_timeline_pause(vgui_editor_timeline* timeline)
	{
		if (!timeline)
			return;
		timeline->impl.pause();
	}

	void vgui_editor_timeline_stop(vgui_editor_timeline* timeline)
	{
		if (!timeline)
			return;
		timeline->impl.stop();
	}

	void vgui_editor_timeline_toggle_play(vgui_editor_timeline* timeline)
	{
		if (!timeline)
			return;
		timeline->impl.toggle_play();
	}

	int vgui_editor_timeline_playing(vgui_editor_timeline* timeline)
	{
		return timeline && timeline->impl.playing() ? 1 : 0;
	}

	void vgui_editor_timeline_set_loop(vgui_editor_timeline* timeline, int loop)
	{
		if (!timeline)
			return;
		timeline->impl.set_loop(loop != 0);
	}

	int vgui_editor_timeline_loop(vgui_editor_timeline* timeline)
	{
		return timeline && timeline->impl.loop() ? 1 : 0;
	}

	void vgui_editor_timeline_tick(vgui_editor_timeline* timeline,
		double delta_seconds)
	{
		if (!timeline)
			return;
		timeline->impl.tick(delta_seconds);
	}

	void vgui_editor_timeline_step_frames(vgui_editor_timeline* timeline, int frames)
	{
		if (!timeline)
			return;
		timeline->impl.step_frames(frames);
	}

	void vgui_editor_timeline_set_on_playhead(vgui_editor_timeline* timeline,
		vgui_editor_timeline_playhead_fn fn, void* user)
	{
		if (!timeline)
			return;
		if (!fn) {
			timeline->playhead_slot.clear();
			timeline->impl.set_playhead_listener(nullptr);
			return;
		}
		timeline->playhead_slot.set(fn, user, nullptr);
		const vgui_editor_timeline_playhead_fn slot_fn = timeline->playhead_slot.fn;
		void* slot_user = timeline->playhead_slot.user;
		vgeditor::user_owner_ptr owner = timeline->playhead_slot.owner;
		timeline->impl.set_playhead_listener([slot_fn, slot_user, owner](double seconds) {
			slot_fn(slot_user, seconds);
		});
	}

	void vgui_editor_timeline_copy_selection(vgui_editor_timeline* timeline)
	{
		if (!timeline)
			return;
		timeline->impl.copy_selection();
	}

	void vgui_editor_timeline_cut_selection(vgui_editor_timeline* timeline)
	{
		if (!timeline)
			return;
		timeline->impl.cut_selection();
	}

	int vgui_editor_timeline_paste_at_playhead(vgui_editor_timeline* timeline)
	{
		if (!timeline)
			return 0;
		return timeline->impl.paste_at_playhead() ? 1 : 0;
	}

	int vgui_editor_timeline_delete_selection(vgui_editor_timeline* timeline)
	{
		if (!timeline)
			return 0;
		return timeline->impl.delete_selection() ? 1 : 0;
	}

	vgui_editor_object_id vgui_editor_timeline_insert_keyframe(
		vgui_editor_timeline* timeline, vgui_editor_object_id track, double time,
		double value)
	{
		if (!timeline)
			return VGUI_EDITOR_INVALID_OBJECT;
		return (vgui_editor_object_id)timeline->impl.insert_keyframe(
			(object_id)track, time, value);
	}

	vgui_editor_object_id vgui_editor_timeline_add_keyframe_at_playhead(
		vgui_editor_timeline* timeline)
	{
		if (!timeline)
			return VGUI_EDITOR_INVALID_OBJECT;
		return (vgui_editor_object_id)timeline->impl.add_keyframe_at_playhead();
	}

	size_t vgui_editor_timeline_clipboard_count(vgui_editor_timeline* timeline)
	{
		return timeline ? timeline->impl.clipboard_count() : 0;
	}

	int vgui_editor_timeline_track_rect(vgui_editor_timeline* timeline,
		vgui_editor_object_id track, float* x, float* y, float* width,
		float* height)
	{
		if (!timeline)
			return 0;
		return timeline->impl.track_rect((object_id)track, x, y, width, height) ?
			1 : 0;
	}

	int vgui_editor_timeline_keyframe_rect(vgui_editor_timeline* timeline,
		vgui_editor_object_id keyframe, float* x, float* y, float* width,
		float* height)
	{
		if (!timeline)
			return 0;
		return timeline->impl.keyframe_rect((object_id)keyframe, x, y, width,
			height) ? 1 : 0;
	}

	vgui_editor_object_id vgui_editor_timeline_hit_test(
		vgui_editor_timeline* timeline, float content_x, float content_y)
	{
		if (!timeline)
			return VGUI_EDITOR_INVALID_OBJECT;
		return (vgui_editor_object_id)timeline->impl.hit_test(content_x, content_y);
	}

	vgui_editor_object_id vgui_editor_timeline_track_at_y(
		vgui_editor_timeline* timeline, float content_y)
	{
		if (!timeline)
			return VGUI_EDITOR_INVALID_OBJECT;
		return (vgui_editor_object_id)timeline->impl.track_at(content_y);
	}

	int vgui_editor_timeline_dragging(vgui_editor_timeline* timeline)
	{
		return timeline && timeline->impl.dragging() ? 1 : 0;
	}

	vgui_editor_timeline_gesture vgui_editor_timeline_get_gesture(
		vgui_editor_timeline* timeline)
	{
		if (!timeline)
			return VGUI_EDITOR_TIMELINE_GESTURE_NONE;
		return from_gesture(timeline->impl.gesture());
	}

	void vgui_editor_timeline_cancel_gesture(vgui_editor_timeline* timeline)
	{
		if (!timeline)
			return;
		timeline->impl.cancel_gesture();
	}

	void vgui_editor_timeline_set_command_stack(vgui_editor_timeline* timeline,
		vgui_editor_command_stack* stack)
	{
		if (!timeline)
			return;
		timeline->impl.set_command_stack(vgeditor::stack_impl(stack));
	}

	size_t vgui_editor_timeline_capture_state_json(vgui_editor_timeline* timeline,
		char* out, size_t cap)
	{
		if (!timeline)
			return 0;
		const std::string text = timeline_state_to_json(timeline->impl.capture_state());
		return vgeditor::copy_string(out, cap, text.c_str());
	}

	int vgui_editor_timeline_apply_state_json(vgui_editor_timeline* timeline,
		const char* json)
	{
		if (!timeline || !json)
			return 0;
		timeline_state state;
		std::string error;
		if (!timeline_state_from_json(vgeditor::to_text(json), state, &error))
			return 0;
		return timeline->impl.apply_state(state, &error) ? 1 : 0;
	}

} /* extern "C" */

namespace vgeditor {

	vgui_editor_timeline* borrow_timeline(vek::timeline_view& timeline)
	{
		return borrow_object<vgui_editor_timeline>(timeline);
	}

	void release_borrowed_timeline(vgui_editor_timeline* handle)
	{
		/* Wrapper only: a borrowed handle's C++ object belongs to its container. */
		if (handle && !handle->owns_object())
			delete handle;
	}


} /* namespace vgeditor */
