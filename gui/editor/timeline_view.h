#pragma once
/*
 * timeline_view.h - animation timeline editor for vgui_editor_kit (T02).
 *
 * The timeline owns a small, renderer-independent animation model: tracks with
 * stable object_id values and keyframes carrying a time in seconds, a scalar
 * value and an interpolation mode.  Time is stored as double throughout, so
 * hit testing, snapping and playback keep sub-frame precision at every zoom
 * level.
 *
 * Interaction:
 *  * LMB on a keyframe selects it.  Ctrl toggles, Shift appends; dragging a
 *    selected keyframe moves the whole selection in time.  LMB on empty lane
 *    space starts a marquee; the selection is applied on release.  Double-click
 *    on a lane inserts a keyframe.
 *  * The ruler scrubs the playhead; the transport buttons stop, step, play and
 *    toggle looping.  Space toggles playback, Left/Right step one frame,
 *    Home/End jump to the start/end, Delete removes selected keyframes and
 *    Ctrl+C / Ctrl+V / Ctrl+X copy, paste and cut.
 *  * MMB pans horizontally, Ctrl+wheel zooms around the cursor, Shift+wheel
 *    pans horizontally and the plain wheel scrolls the track list.  Tracks can
 *    also be selected by clicking their header.
 *  * Snap defaults to 1/30 s.  set_frame_rate() keeps the step in sync until a
 *    custom step is supplied with set_snap_step().
 *
 * Command integration:
 *  * set_command_stack() makes keyframe move, insert, paste and delete actions
 *    push one command each ("Move keyframes", "Add keyframe", "Paste
 *    keyframes", "Delete keyframes").  Model edits performed directly through
 *    add_keyframe()/update_keyframe() do not record commands; hosts use those
 *    while loading a document.
 *
 * Playback is host driven: call tick(delta_seconds) from the main loop.  A
 * playhead listener lets the host sample the animation and apply it to its own
 * scene; set_playhead() keeps an externally clocked playhead authoritative.
 *
 * View settings, playhead, duration, frame rate, snapping and looping persist
 * through timeline_state_to_json()/timeline_state_from_json(); editor_shell
 * stores that opaque payload in editor_layout (version 4).
 */
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "command_stack.h"
#include "editor_types.h"
#include "vgui.h"

namespace vgui_editor_kit {

	enum class timeline_interpolation
	{
		step = 0,
		linear,
		smooth
	};

	struct timeline_keyframe
	{
		/* Stable per-timeline id.  add_keyframe() allocates one when this is 0. */
		object_id id = invalid_object;
		double time = 0.0;
		double value = 0.0;
		timeline_interpolation interpolation =
			timeline_interpolation::linear;
	};

	struct timeline_track
	{
		/* Stable per-timeline id.  add_track() allocates one when this is 0. */
		object_id id = invalid_object;
		std::string name;
		bool visible = true;
		bool locked = false;
		bool muted = false;
		/* Row height in pixels, clamped to a readable range. */
		float height = 26.0f;
		/* 0xAARRGGBB.  Drawn as a swatch in the track header. */
		vgui_color color = 0xFF5FA8FF;
		std::vector<timeline_keyframe> keyframes;
	};

	struct timeline_view_state
	{
		/* Time displayed at the left edge of the lane area. */
		double view_start = 0.0;
		/* Positive scale from seconds to pixels. */
		double pixels_per_second = 120.0;
		/* Vertical track scroll in pixels. */
		float scroll_y = 0.0f;
	};

	struct timeline_state
	{
		static constexpr int current_version = 1;

		int version = current_version;
		timeline_view_state view{};
		double playhead = 0.0;
		double duration = 10.0;
		double frame_rate = 30.0;
		bool snap_enabled = true;
		double snap_step = 1.0 / 30.0;
		bool loop = false;
		object_id selected_track = invalid_object;
	};

	std::string timeline_state_to_json(const timeline_state& state);
	bool timeline_state_from_json(const std::string& text, timeline_state& out,
		std::string* error = nullptr);

	enum class timeline_gesture
	{
		none = 0,
		pending_move,
		move,
		pending_marquee,
		marquee,
		pan,
		scrub
	};

	class timeline_view
	{
	public:
		struct keyframe_ref
		{
			object_id track = invalid_object;
			timeline_keyframe keyframe;
		};

		using selection_listener =
			std::function<void(const std::vector<object_id>&)>;
		using playhead_listener = std::function<void(double)>;
		using change_listener = std::function<void()>;

		timeline_view() = default;
		~timeline_view();

		timeline_view(const timeline_view&) = delete;
		timeline_view& operator=(const timeline_view&) = delete;

		void build(vgui_widget* parent);
		vgui_widget* widget() const noexcept;
		bool built() const noexcept;

		/* ---- model ---------------------------------------------------- */
		/* Returns the track id, or invalid_object for a duplicate explicit id. */
		object_id add_track(timeline_track track);
		/* Replaces the track record, including its keyframe list. */
		bool update_track(const timeline_track& track);
		bool remove_track(object_id id);
		void clear_tracks();
		const std::vector<timeline_track>& tracks() const noexcept;
		const timeline_track* find_track(object_id id) const noexcept;
		timeline_track* find_track(object_id id) noexcept;
		std::size_t track_count() const noexcept;

		/* Direct model edits; they notify change listeners but never push undo
		 * commands.  Use the edit helpers below for user actions. */
		object_id add_keyframe(object_id track, timeline_keyframe keyframe);
		bool update_keyframe(object_id track, const timeline_keyframe& keyframe);
		bool remove_keyframe(object_id track, object_id keyframe);
		const timeline_keyframe* find_keyframe(object_id keyframe,
			object_id* track = nullptr) const noexcept;
		timeline_keyframe* find_keyframe(object_id keyframe,
			object_id* track = nullptr) noexcept;
		std::size_t keyframe_count() const noexcept;
		/* Sample a track at `time` with the per-keyframe interpolation mode. */
		double evaluate(object_id track, double time) const noexcept;

		/* ---- selection ------------------------------------------------ */
		void set_selection(const std::vector<object_id>& ids,
			object_id primary = invalid_object);
		void clear_selection();
		void select_all();
		const std::vector<object_id>& selection_ids() const noexcept;
		object_id primary_selection() const noexcept;
		bool is_selected(object_id id) const noexcept;
		void set_multi_select(bool enabled);
		bool multi_select() const noexcept;
		void set_selection_listener(selection_listener listener);
		/* Track whose header is highlighted and receives new keyframes. */
		object_id selected_track() const noexcept;
		void set_selected_track(object_id track);

		/* Fired for model, view, playback and gesture changes (not for
		 * keyframe selection; use the selection listener for that). */
		void set_change_listener(change_listener listener);

		/* ---- view / zoom / pan ---------------------------------------- */
		timeline_view_state view_state() const noexcept;
		/* Cancels an in-flight gesture so the view never changes underneath a
		 * drag snapshot. */
		void set_view_state(const timeline_view_state& state);
		/* Pixels per second. */
		double zoom() const noexcept;
		void set_zoom_limits(double minimum, double maximum);
		/* Zooms by `factor`, keeping lane-local x fixed. */
		void zoom_at(float lane_x, double factor);
		void pan_by_pixels(float delta_pixels);
		void reset_view();

		/* Lane-local content x <-> time. */
		double time_to_x(double time) const noexcept;
		double x_to_time(double lane_x) const noexcept;
		/* Content x (including the track-header column) <-> time. */
		double time_at_content_x(float content_x) const;
		float content_x_at_time(double time) const;

		/* ---- snapping / timing ---------------------------------------- */
		void set_snap_enabled(bool enabled);
		bool snap_enabled() const noexcept;
		/* Custom step in seconds; also stops following the frame rate. */
		void set_snap_step(double seconds);
		double snap_step() const noexcept;
		void set_snap_to_frames(bool enabled);
		bool snap_to_frames() const noexcept;
		double snap_time(double time) const noexcept;
		void set_frame_rate(double fps);
		double frame_rate() const noexcept;
		void set_duration(double seconds);
		double duration() const noexcept;

		/* ---- playback -------------------------------------------------- */
		/* Clamped to 0..duration.  Always authoritative, even while playing. */
		void set_playhead(double seconds);
		double playhead() const noexcept;
		void play();
		void pause();
		/* Pauses and rewinds to 0. */
		void stop();
		void toggle_play();
		bool playing() const noexcept;
		void set_loop(bool loop);
		bool loop() const noexcept;
		/* Advances the playhead by an external clock delta. */
		void tick(double delta_seconds);
		/* Pauses and moves the playhead by whole frames. */
		void step_frames(int frames);
		void set_playhead_listener(playhead_listener listener);

		/* ---- editing --------------------------------------------------- */
		void copy_selection();
		void cut_selection();
		/* Pastes the clipboard at the (snapped) playhead, preserving relative
		 * times.  Returns false for an empty clipboard. */
		bool paste_at_playhead();
		bool delete_selection();
		/* Inserts a keyframe on a track at an absolute time.  The time is
		 * snapped when snapping is enabled. */
		object_id insert_keyframe(object_id track, double time,
			double value = 0.0);
		/* Uses selected_track(), or the first track when none is selected. */
		object_id add_keyframe_at_playhead();
		std::size_t clipboard_count() const noexcept;

		/* ---- geometry / interaction ----------------------------------- */
		/* Content-local rectangles (inside the 1px widget border). */
		bool track_rect(object_id track, float* x, float* y, float* width,
			float* height) const;
		bool keyframe_rect(object_id keyframe, float* x, float* y,
			float* width, float* height) const;
		/* Content-local; 0 when no keyframe is hit. */
		object_id hit_test(float content_x, float content_y) const;
		/* Returns the track at content y, including its header row. */
		object_id track_at(float content_y) const;

		bool dragging() const noexcept;
		timeline_gesture gesture() const noexcept;
		/* Restores the live gesture snapshot; safe without a gesture. */
		void cancel_gesture();

		void set_command_stack(command_stack* stack);
		command_stack* command_stack_ptr() const noexcept;

		void set_drag_threshold(float pixels);
		float drag_threshold() const noexcept;
		void set_enabled(bool enabled);
		bool enabled() const noexcept;
		void request_focus();
		bool focused() const;

		/* ---- persistence ----------------------------------------------- */
		timeline_state capture_state() const;
		/* Validates the full state before mutating anything. */
		bool apply_state(const timeline_state& state, std::string* error = nullptr);

	private:
		struct metrics
		{
			float x = 0.0f, y = 0.0f, width = 0.0f, height = 0.0f;
			float transport_y = 0.0f, transport_height = 0.0f;
			float ruler_y = 0.0f, ruler_height = 0.0f;
			float header_width = 0.0f;
			float lanes_x = 0.0f, lanes_y = 0.0f;
			float lanes_width = 0.0f, lanes_height = 0.0f;
		};

		static void on_draw(vgui_widget* widget, ovg_ctx_cb* cb, rvg_t* vg,
			float abs_x, float abs_y, float abs_w, float abs_h,
			float delta_time, void* user);
		static int on_event(vgui_widget* widget, const vgui_event* event,
			void* user);
		static void on_focus_change(vgui_context* ctx, vgui_widget* old_focus,
			vgui_widget* new_focus, void* user);

		bool content_geometry(float* offset_x, float* offset_y, float* width,
			float* height) const;
		float effective_drag_threshold() const;
		/* E01: chrome metrics are logical and converted to render pixels. */
		float ui_scale() const;
		metrics layout_metrics() const;
		float total_track_height() const;
		float max_scroll_y(const metrics& m) const;
		void clamp_scroll();
		float track_y(const metrics& m, std::size_t index) const;
		float track_height_at(std::size_t index) const;
		float track_center_y(const metrics& m, std::size_t index) const;

		void notify_change();
		void notify_selection();
		void notify_playhead();
		void prune_selection();
		void sort_keyframes(timeline_track& track);
		void sort_all_keyframes();
		bool append_keyframe(object_id track, timeline_keyframe keyframe);
		void apply_selection(const std::vector<object_id>& picked, int mods);
		std::vector<object_id> keyframes_in_rect(float x0, float y0, float x1,
			float y1) const;

		void capture_gesture_keyframes();
		void update_move(float content_x, float content_y);
		void finish_move(float content_x, float content_y);
		void cancel_gesture_impl();
		void reset_gesture();
		bool gesture_changed() const;
		void apply_keyframe_states(const std::vector<keyframe_ref>& before,
			const std::vector<keyframe_ref>& after);
		void push_keyframe_command(const char* name,
			const std::vector<keyframe_ref>& before,
			const std::vector<keyframe_ref>& after);
		std::vector<keyframe_ref> snapshot_selection() const;
		std::vector<keyframe_ref> selection_refs() const;
		double snap_time_value(double time) const noexcept;

		int transport_hit(float content_x, float content_y) const;
		float transport_button_x(int button) const;
		double choose_time_step() const;

		void draw(ovg_ctx_cb* cb, rvg_t* vg, vgui_widget* widget, float x,
			float y, float width, float height, float delta_time);
		void draw_lanes(ovg_ctx_cb* cb, rvg_t* vg, vgui_widget* widget,
			float x, float y, const metrics& m) const;
		void draw_ruler(ovg_ctx_cb* cb, rvg_t* vg, vgui_widget* widget,
			float x, float y, const metrics& m) const;
		void draw_transport(ovg_ctx_cb* cb, rvg_t* vg, vgui_widget* widget,
			float x, float y, const metrics& m) const;
		void draw_keyframes(ovg_ctx_cb* cb, rvg_t* vg, float x, float y,
			const metrics& m) const;

		vgui_widget* widget_ = nullptr;
		vgui_context* context_ = nullptr;
		int focus_token_ = 0;

		std::vector<timeline_track> tracks_;
		object_id next_track_id_ = 1;
		object_id next_keyframe_id_ = 1;
		std::vector<object_id> selection_;
		object_id primary_ = invalid_object;
		object_id selected_track_ = invalid_object;
		selection_listener selection_listener_;
		change_listener change_listener_;
		playhead_listener playhead_listener_;

		timeline_view_state view_{};
		double min_zoom_ = 4.0;
		double max_zoom_ = 4000.0;
		double duration_ = 10.0;
		double playhead_ = 0.0;
		double frame_rate_ = 30.0;
		bool snap_enabled_ = true;
		bool snap_to_frames_ = true;
		double snap_step_ = 1.0 / 30.0;
		bool playing_ = false;
		bool loop_ = false;

		command_stack* commands_ = nullptr;

		bool enabled_ = true;
		bool multi_select_ = true;
		float drag_threshold_ = -1.0f;
		float marquee_offset_ = 0.0f;

		timeline_gesture gesture_ = timeline_gesture::none;
		float press_x_ = 0.0f;
		float press_y_ = 0.0f;
		float marquee_x_ = 0.0f;
		float marquee_y_ = 0.0f;
		timeline_view_state gesture_view_{};
		std::vector<keyframe_ref> gesture_start_;
		double scrub_start_playhead_ = 0.0;
		std::vector<keyframe_ref> clipboard_;

		mutable std::shared_ptr<int> alive_token_ = std::make_shared<int>(0);
	};

} /* namespace vgui_editor_kit */
