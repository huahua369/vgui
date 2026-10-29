#include <vgui/editor/timeline_view.h>

#include "tools/tools.hpp"
#include "ui_scale.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <utility>

#include <nlohmann/json.hpp>

namespace vgui_editor_kit {

	namespace {

		constexpr float kBorderInset = 1.0f;
		constexpr float kTransportHeight = 26.0f;
		constexpr float kRulerHeight = 24.0f;
		constexpr float kHeaderWidth = 150.0f;
		constexpr float kMinTrackHeight = 18.0f;
		constexpr float kMaxTrackHeight = 80.0f;
		constexpr float kKeyframeSize = 5.0f;
		constexpr float kKeyframeHit = 7.0f;
		constexpr double kEpsilon = 1e-9;
		constexpr double kMinPixelsPerSecond = 0.001;

		/* vgui 0xAARRGGBB -> OVG 0xAABBGGRR. */
		uint32_t to_ovg(vgui_color color)
		{
			return (color & 0xFF00FF00u) |
				((color & 0x000000FFu) << 16) |
				((color >> 16) & 0x000000FFu);
		}

		vgui_color with_alpha(vgui_color color, uint32_t alpha)
		{
			return (color & 0x00FFFFFFu) | ((alpha & 0xFFu) << 24);
		}

		vgui_color theme_color(vgui_color color, vgui_color fallback)
		{
			return color ? color : fallback;
		}

		bool near_time(double a, double b)
		{
			return std::fabs(a - b) <= kEpsilon;
		}

		float clamp_track_height(float height)
		{
			if (!std::isfinite(height))
				return 26.0f;
			return std::clamp(height, kMinTrackHeight, kMaxTrackHeight);
		}

		bool validate_timeline_state(const timeline_state& state,
			std::string* error)
		{
			auto fail = [error](const char* message) {
				if (error)
					*error = message;
				return false;
			};
			if (state.version < 1 ||
				state.version > timeline_state::current_version)
				return fail("unsupported timeline state version");
			if (!std::isfinite(state.view.view_start))
				return fail("timeline view_start must be finite");
			if (!std::isfinite(state.view.pixels_per_second) ||
				state.view.pixels_per_second <= 0.0)
				return fail("timeline pixels_per_second must be positive");
			if (!std::isfinite(state.view.scroll_y) || state.view.scroll_y < 0.0f)
				return fail("timeline scroll_y must be non-negative");
			if (!std::isfinite(state.playhead) || state.playhead < 0.0)
				return fail("timeline playhead must be non-negative");
			if (!std::isfinite(state.duration) || state.duration <= 0.0)
				return fail("timeline duration must be positive");
			if (state.playhead > state.duration + kEpsilon)
				return fail("timeline playhead is outside the duration");
			if (!std::isfinite(state.frame_rate) || state.frame_rate <= 0.0)
				return fail("timeline frame_rate must be positive");
			if (!std::isfinite(state.snap_step) || state.snap_step <= 0.0)
				return fail("timeline snap_step must be positive");
			return true;
		}

		class timeline_command final : public command
		{
		public:
			using apply_fn = std::function<void(
				const std::vector<timeline_view::keyframe_ref>&,
				const std::vector<timeline_view::keyframe_ref>&)>;

			timeline_command(std::string name,
				std::vector<timeline_view::keyframe_ref> before,
				std::vector<timeline_view::keyframe_ref> after,
				apply_fn apply)
				: name_(std::move(name)), before_(std::move(before)),
				after_(std::move(after)), apply_(std::move(apply))
			{
			}

			const char* name() const noexcept override
			{
				return name_.c_str();
			}

			bool execute() override
			{
				if (apply_)
					apply_(before_, after_);
				return true;
			}

			void undo() override
			{
				/* Remove every id involved (the union of before/after) and
				 * restore the before states.  For inserts and pastes the
				 * before set is empty, so this is what deletes them again. */
				if (apply_)
					apply_(after_, before_);
			}

		private:
			std::string name_;
			std::vector<timeline_view::keyframe_ref> before_;
			std::vector<timeline_view::keyframe_ref> after_;
			apply_fn apply_;
		};

	} /* namespace */

	/* --------------------------------------------------------------------- */
	/* State serialization                                                   */
	/* --------------------------------------------------------------------- */

	std::string timeline_state_to_json(const timeline_state& state)
	{
		nlohmann::json root;
		root["version"] = timeline_state::current_version;
		root["view"] = {
			{ "start", state.view.view_start },
			{ "pixels_per_second", state.view.pixels_per_second },
			{ "scroll_y", state.view.scroll_y }
		};
		root["playhead"] = state.playhead;
		root["duration"] = state.duration;
		root["frame_rate"] = state.frame_rate;
		root["snap"] = {
			{ "enabled", state.snap_enabled },
			{ "step", state.snap_step }
		};
		root["loop"] = state.loop;
		root["selected_track"] = state.selected_track;
		return root.dump();
	}

	bool timeline_state_from_json(const std::string& text, timeline_state& out,
		std::string* error)
	{
		out = timeline_state{};
		try
		{
			nlohmann::json root = nlohmann::json::parse(text);
			if (!root.is_object())
				throw std::runtime_error(
					"timeline state root must be an object");

			auto version = root.find("version");
			if (version == root.end() || !version->is_number_integer())
				throw std::runtime_error("timeline state version is missing");
			const int file_version = version->get<int>();
			if (file_version < 1 ||
				file_version > timeline_state::current_version)
				throw std::runtime_error(
					"unsupported timeline state version");

			timeline_state parsed;
			parsed.version = file_version;

			auto number = [](const nlohmann::json& object, const char* key,
				double fallback) {
				auto it = object.find(key);
				if (it == object.end())
					return fallback;
				if (!it->is_number())
					throw std::runtime_error(std::string(
						"expected number: ") + key);
				return it->get<double>();
			};
			auto boolean = [](const nlohmann::json& object, const char* key,
				bool fallback) {
				auto it = object.find(key);
				if (it == object.end())
					return fallback;
				if (!it->is_boolean())
					throw std::runtime_error(std::string(
						"expected boolean: ") + key);
				return it->get<bool>();
			};

			auto view = root.find("view");
			if (view != root.end())
			{
				if (!view->is_object())
					throw std::runtime_error("timeline view must be an object");
				parsed.view.view_start = number(*view, "start",
					parsed.view.view_start);
				parsed.view.pixels_per_second = number(*view,
					"pixels_per_second", parsed.view.pixels_per_second);
				parsed.view.scroll_y = (float)number(*view, "scroll_y",
					(double)parsed.view.scroll_y);
			}

			parsed.playhead = number(root, "playhead", parsed.playhead);
			parsed.duration = number(root, "duration", parsed.duration);
			parsed.frame_rate = number(root, "frame_rate",
				parsed.frame_rate);

			auto snap = root.find("snap");
			if (snap != root.end())
			{
				if (!snap->is_object())
					throw std::runtime_error("timeline snap must be an object");
				parsed.snap_enabled = boolean(*snap, "enabled",
					parsed.snap_enabled);
				parsed.snap_step = number(*snap, "step", parsed.snap_step);
			}

			parsed.loop = boolean(root, "loop", parsed.loop);

			auto selected = root.find("selected_track");
			if (selected != root.end())
			{
				if (!selected->is_number_integer() &&
					!selected->is_number_unsigned())
					throw std::runtime_error(
						"timeline selected_track must be an integer");
				const long long value = selected->get<long long>();
				if (value < 0)
					throw std::runtime_error(
						"timeline selected_track is invalid");
				parsed.selected_track = (object_id)value;
			}

			std::string reason;
			if (!validate_timeline_state(parsed, &reason))
			{
				if (error)
					*error = reason;
				out = timeline_state{};
				return false;
			}
			out = parsed;
			return true;
		}
		catch (const std::exception& e)
		{
			out = timeline_state{};
			if (error)
				*error = e.what();
			return false;
		}
	}

	/* --------------------------------------------------------------------- */
	/* Construction / model                                                  */
	/* --------------------------------------------------------------------- */

	timeline_view::~timeline_view()
	{
		if (context_ && focus_token_)
			vgui_context_remove_focus_listener(context_, focus_token_);
		focus_token_ = 0;
		if (widget_)
		{
			/* The widget may outlive this component: remove the callbacks so a
			 * stale user pointer is never invoked while the tree is destroyed. */
			vgui_custom_set_draw(widget_, nullptr, nullptr);
			vgui_custom_set_event(widget_, nullptr, nullptr);
		}
		widget_ = nullptr;
		context_ = nullptr;
	}

	void timeline_view::build(vgui_widget* parent)
	{
		if (widget_ || !parent)
			return;

		context_ = vgui_widget_get_context(parent);
		widget_ = vgui_widget_add(parent, VGUI_CUSTOM, nullptr);
		vgui_widget_set_id(widget_, "timeline_view");
		vgui_widget_set_a11y_name(widget_, "Timeline");
		vgui_widget_set_grow(widget_, 1.0f);
		vgui_widget_set_min_size(widget_, 240, 120);
		vgui_widget_set_background(widget_, 0xFF101419);
		vgui_widget_set_border(widget_, 0xFF2A3038, 1);
		vgui_widget_set_focusable(widget_, 1);
		vgui_custom_set_draw(widget_, &timeline_view::on_draw, this);
		vgui_custom_set_event(widget_, &timeline_view::on_event, this);
		if (context_)
			focus_token_ = vgui_context_add_focus_listener(context_,
				&timeline_view::on_focus_change, this);
	}

	vgui_widget* timeline_view::widget() const noexcept
	{
		return widget_;
	}

	bool timeline_view::built() const noexcept
	{
		return widget_ != nullptr;
	}

	object_id timeline_view::add_track(timeline_track track)
	{
		if (track.id == invalid_object)
		{
			while (find_track(next_track_id_) != nullptr)
				++next_track_id_;
			track.id = next_track_id_++;
		}
		else
		{
			if (find_track(track.id) != nullptr)
				return invalid_object;
			if (track.id >= next_track_id_)
				next_track_id_ = track.id + 1;
		}

		std::vector<timeline_keyframe> keyframes = std::move(track.keyframes);
		track.keyframes.clear();
		std::vector<object_id> used;
		used.reserve(keyframes.size());
		for (timeline_keyframe& keyframe : keyframes)
		{
			if (!std::isfinite(keyframe.time))
				keyframe.time = 0.0;
			if (!std::isfinite(keyframe.value))
				keyframe.value = 0.0;
			if (keyframe.id == invalid_object)
			{
				while (find_keyframe(next_keyframe_id_) != nullptr ||
					std::find(used.begin(), used.end(), next_keyframe_id_) !=
						used.end())
					++next_keyframe_id_;
				keyframe.id = next_keyframe_id_++;
			}
			else
			{
				if (find_keyframe(keyframe.id) != nullptr ||
					std::find(used.begin(), used.end(), keyframe.id) !=
						used.end())
					return invalid_object;
				if (keyframe.id >= next_keyframe_id_)
					next_keyframe_id_ = keyframe.id + 1;
			}
			used.push_back(keyframe.id);
			track.keyframes.push_back(keyframe);
		}

		if (track.name.empty())
			track.name = "Track " + std::to_string(track.id);
		track.height = clamp_track_height(track.height);
		sort_keyframes(track);
		const object_id id = track.id;
		tracks_.push_back(std::move(track));
		if (selected_track_ == invalid_object)
			selected_track_ = id;
		notify_change();
		return id;
	}

	bool timeline_view::update_track(const timeline_track& track)
	{
		if (track.id == invalid_object)
			return false;
		auto found = std::find_if(tracks_.begin(), tracks_.end(),
			[&](const timeline_track& current) {
				return current.id == track.id;
			});
		if (found == tracks_.end())
			return false;

		timeline_track next = track;
		next.height = clamp_track_height(next.height);
		std::vector<timeline_keyframe> keyframes = std::move(next.keyframes);
		next.keyframes.clear();

		std::vector<object_id> used;
		used.reserve(keyframes.size());
		for (timeline_keyframe& keyframe : keyframes)
		{
			if (!std::isfinite(keyframe.time))
				keyframe.time = 0.0;
			if (!std::isfinite(keyframe.value))
				keyframe.value = 0.0;
			if (keyframe.id == invalid_object)
			{
				while (true)
				{
					bool conflict = std::find(used.begin(), used.end(),
						next_keyframe_id_) != used.end();
					if (!conflict)
					{
						for (const timeline_track& other : tracks_)
						{
							if (other.id == next.id)
								continue;
							if (std::any_of(other.keyframes.begin(),
								other.keyframes.end(),
								[&](const timeline_keyframe& kf) {
									return kf.id == next_keyframe_id_;
								}))
							{
								conflict = true;
								break;
							}
						}
					}
					if (!conflict)
						break;
					++next_keyframe_id_;
				}
				keyframe.id = next_keyframe_id_++;
			}
			else
			{
				bool conflict = std::find(used.begin(), used.end(),
					keyframe.id) != used.end();
				if (!conflict)
				{
					for (const timeline_track& other : tracks_)
					{
						if (other.id == next.id)
							continue;
						if (std::any_of(other.keyframes.begin(),
							other.keyframes.end(),
							[&](const timeline_keyframe& kf) {
								return kf.id == keyframe.id;
							}))
						{
							conflict = true;
							break;
						}
					}
				}
				if (conflict)
					return false;
				if (keyframe.id >= next_keyframe_id_)
					next_keyframe_id_ = keyframe.id + 1;
			}
			used.push_back(keyframe.id);
			next.keyframes.push_back(keyframe);
		}

		sort_keyframes(next);
		*found = std::move(next);
		prune_selection();
		if (selected_track_ != invalid_object &&
			find_track(selected_track_) == nullptr)
			selected_track_ = tracks_.empty() ? invalid_object
				: tracks_.front().id;
		notify_change();
		return true;
	}

	bool timeline_view::remove_track(object_id id)
	{
		const auto found = std::find_if(tracks_.begin(), tracks_.end(),
			[&](const timeline_track& track) { return track.id == id; });
		if (found == tracks_.end())
			return false;
		tracks_.erase(found);
		prune_selection();
		if (selected_track_ == id)
			selected_track_ = tracks_.empty() ? invalid_object
				: tracks_.front().id;
		clamp_scroll();
		notify_change();
		return true;
	}

	void timeline_view::clear_tracks()
	{
		if (tracks_.empty())
			return;
		tracks_.clear();
		if (!selection_.empty() || primary_ != invalid_object)
		{
			selection_.clear();
			primary_ = invalid_object;
			notify_selection();
		}
		selected_track_ = invalid_object;
		notify_change();
	}

	const std::vector<timeline_track>& timeline_view::tracks() const noexcept
	{
		return tracks_;
	}

	const timeline_track* timeline_view::find_track(object_id id) const noexcept
	{
		if (id == invalid_object)
			return nullptr;
		for (const timeline_track& track : tracks_)
			if (track.id == id)
				return &track;
		return nullptr;
	}

	timeline_track* timeline_view::find_track(object_id id) noexcept
	{
		if (id == invalid_object)
			return nullptr;
		for (timeline_track& track : tracks_)
			if (track.id == id)
				return &track;
		return nullptr;
	}

	std::size_t timeline_view::track_count() const noexcept
	{
		return tracks_.size();
	}

	object_id timeline_view::add_keyframe(object_id track_id,
		timeline_keyframe keyframe)
	{
		timeline_track* track = find_track(track_id);
		if (!track || !std::isfinite(keyframe.time) ||
			!std::isfinite(keyframe.value))
			return invalid_object;
		if (keyframe.id == invalid_object)
		{
			while (find_keyframe(next_keyframe_id_) != nullptr)
				++next_keyframe_id_;
			keyframe.id = next_keyframe_id_++;
		}
		else
		{
			if (find_keyframe(keyframe.id) != nullptr)
				return invalid_object;
			if (keyframe.id >= next_keyframe_id_)
				next_keyframe_id_ = keyframe.id + 1;
		}
		const object_id id = keyframe.id;
		track->keyframes.push_back(keyframe);
		sort_keyframes(*track);
		notify_change();
		return id;
	}

	bool timeline_view::update_keyframe(object_id track_id,
		const timeline_keyframe& keyframe)
	{
		timeline_track* track = find_track(track_id);
		if (!track || keyframe.id == invalid_object ||
			!std::isfinite(keyframe.time) ||
			!std::isfinite(keyframe.value))
			return false;
		for (timeline_keyframe& current : track->keyframes)
		{
			if (current.id != keyframe.id)
				continue;
			current = keyframe;
			sort_keyframes(*track);
			notify_change();
			return true;
		}
		return false;
	}

	bool timeline_view::remove_keyframe(object_id track_id,
		object_id keyframe_id)
	{
		timeline_track* track = find_track(track_id);
		if (!track)
			return false;
		for (auto it = track->keyframes.begin(); it != track->keyframes.end();
			++it)
		{
			if (it->id != keyframe_id)
				continue;
			track->keyframes.erase(it);
			prune_selection();
			notify_change();
			return true;
		}
		return false;
	}

	const timeline_keyframe* timeline_view::find_keyframe(object_id keyframe,
		object_id* track) const noexcept
	{
		if (keyframe == invalid_object)
			return nullptr;
		for (const timeline_track& current : tracks_)
		{
			for (const timeline_keyframe& keyframe_entry : current.keyframes)
			{
				if (keyframe_entry.id != keyframe)
					continue;
				if (track)
					*track = current.id;
				return &keyframe_entry;
			}
		}
		return nullptr;
	}

	timeline_keyframe* timeline_view::find_keyframe(object_id keyframe,
		object_id* track) noexcept
	{
		if (keyframe == invalid_object)
			return nullptr;
		for (timeline_track& current : tracks_)
		{
			for (timeline_keyframe& keyframe_entry : current.keyframes)
			{
				if (keyframe_entry.id != keyframe)
					continue;
				if (track)
					*track = current.id;
				return &keyframe_entry;
			}
		}
		return nullptr;
	}

	std::size_t timeline_view::keyframe_count() const noexcept
	{
		std::size_t count = 0;
		for (const timeline_track& track : tracks_)
			count += track.keyframes.size();
		return count;
	}

	double timeline_view::evaluate(object_id track_id, double time) const noexcept
	{
		const timeline_track* track = find_track(track_id);
		if (!track || track->keyframes.empty() || !std::isfinite(time))
			return 0.0;
		const auto& keys = track->keyframes;
		if (time <= keys.front().time)
			return keys.front().value;
		if (time >= keys.back().time)
			return keys.back().value;
		auto upper = std::upper_bound(keys.begin(), keys.end(), time,
			[](double value, const timeline_keyframe& keyframe) {
				return value < keyframe.time;
			});
		if (upper == keys.begin())
			return keys.front().value;
		if (upper == keys.end())
			return keys.back().value;
		const timeline_keyframe& b = *upper;
		const timeline_keyframe& a = *(upper - 1);
		const double span = b.time - a.time;
		if (!(span > 0.0))
			return b.value;
		double u = (time - a.time) / span;
		u = std::clamp(u, 0.0, 1.0);
		switch (a.interpolation)
		{
		case timeline_interpolation::step:
			return a.value;
		case timeline_interpolation::smooth:
			u = u * u * (3.0 - 2.0 * u);
			break;
		case timeline_interpolation::linear:
		default:
			break;
		}
		return a.value + (b.value - a.value) * u;
	}

	/* --------------------------------------------------------------------- */
	/* Selection                                                             */
	/* --------------------------------------------------------------------- */

	void timeline_view::set_selection(const std::vector<object_id>& ids,
		object_id primary)
	{
		std::vector<object_id> filtered;
		filtered.reserve(ids.size());
		for (object_id id : ids)
		{
			if (id == invalid_object || find_keyframe(id) == nullptr)
				continue;
			if (std::find(filtered.begin(), filtered.end(), id) !=
				filtered.end())
				continue;
			filtered.push_back(id);
		}
		if (primary == invalid_object ||
			std::find(filtered.begin(), filtered.end(), primary) ==
				filtered.end())
			primary = filtered.empty() ? invalid_object : filtered.front();
		if (filtered == selection_ && primary == primary_)
			return;
		selection_ = std::move(filtered);
		primary_ = primary;
		notify_selection();
	}

	void timeline_view::clear_selection()
	{
		set_selection({});
	}

	void timeline_view::select_all()
	{
		std::vector<object_id> ids;
		ids.reserve(keyframe_count());
		for (const timeline_track& track : tracks_)
			for (const timeline_keyframe& keyframe : track.keyframes)
				ids.push_back(keyframe.id);
		set_selection(ids, ids.empty() ? invalid_object : ids.front());
	}

	const std::vector<object_id>& timeline_view::selection_ids() const noexcept
	{
		return selection_;
	}

	object_id timeline_view::primary_selection() const noexcept
	{
		return primary_;
	}

	bool timeline_view::is_selected(object_id id) const noexcept
	{
		return std::find(selection_.begin(), selection_.end(), id) !=
			selection_.end();
	}

	void timeline_view::set_multi_select(bool enabled)
	{
		multi_select_ = enabled;
	}

	bool timeline_view::multi_select() const noexcept
	{
		return multi_select_;
	}

	void timeline_view::set_selection_listener(selection_listener listener)
	{
		selection_listener_ = std::move(listener);
	}

	void timeline_view::set_change_listener(change_listener listener)
	{
		change_listener_ = std::move(listener);
	}

	object_id timeline_view::selected_track() const noexcept
	{
		return selected_track_;
	}

	void timeline_view::set_selected_track(object_id track)
	{
		if (track != invalid_object && find_track(track) == nullptr)
			track = invalid_object;
		if (selected_track_ == track)
			return;
		selected_track_ = track;
		notify_change();
	}

	/* --------------------------------------------------------------------- */
	/* View / zoom / pan                                                     */
	/* --------------------------------------------------------------------- */

	timeline_view_state timeline_view::view_state() const noexcept
	{
		return view_;
	}

	void timeline_view::set_view_state(const timeline_view_state& state)
	{
		if (gesture_ != timeline_gesture::none)
			cancel_gesture();
		timeline_view_state next = state;
		if (!std::isfinite(next.view_start))
			next.view_start = 0.0;
		if (!std::isfinite(next.pixels_per_second) ||
			next.pixels_per_second <= 0.0)
			next.pixels_per_second = 120.0;
		next.pixels_per_second = std::clamp(next.pixels_per_second,
			min_zoom_, max_zoom_);
		if (!std::isfinite(next.scroll_y) || next.scroll_y < 0.0f)
			next.scroll_y = 0.0f;
		if (near_time(next.view_start, view_.view_start) &&
			near_time(next.pixels_per_second, view_.pixels_per_second) &&
			std::fabs(next.scroll_y - view_.scroll_y) <= 1e-4f)
			return;
		view_ = next;
		clamp_scroll();
		notify_change();
	}

	double timeline_view::zoom() const noexcept
	{
		return view_.pixels_per_second;
	}

	void timeline_view::set_zoom_limits(double minimum, double maximum)
	{
		min_zoom_ = std::max(kMinPixelsPerSecond, minimum);
		max_zoom_ = std::max(min_zoom_, maximum);
		timeline_view_state state = view_;
		state.pixels_per_second = std::clamp(state.pixels_per_second,
			min_zoom_, max_zoom_);
		if (!near_time(state.pixels_per_second, view_.pixels_per_second))
		{
			view_.pixels_per_second = state.pixels_per_second;
			notify_change();
		}
	}

	void timeline_view::zoom_at(float lane_x, double factor)
	{
		if (!std::isfinite(factor) || factor <= 0.0)
			return;
		if (gesture_ != timeline_gesture::none)
			cancel_gesture();
		const double anchor = x_to_time(lane_x);
		const double pixels_per_second = std::clamp(
			view_.pixels_per_second * factor, min_zoom_, max_zoom_);
		if (near_time(pixels_per_second, view_.pixels_per_second))
			return;
		view_.pixels_per_second = pixels_per_second;
		view_.view_start = anchor - (double)lane_x / pixels_per_second;
		notify_change();
	}

	void timeline_view::pan_by_pixels(float delta_pixels)
	{
		if (!std::isfinite(delta_pixels) || delta_pixels == 0.0f)
			return;
		if (gesture_ != timeline_gesture::none)
			cancel_gesture();
		view_.view_start -= (double)delta_pixels / view_.pixels_per_second;
		notify_change();
	}

	void timeline_view::reset_view()
	{
		set_view_state({});
	}

	double timeline_view::time_to_x(double time) const noexcept
	{
		return (time - view_.view_start) * view_.pixels_per_second;
	}

	double timeline_view::x_to_time(double lane_x) const noexcept
	{
		return view_.view_start + lane_x / view_.pixels_per_second;
	}

	double timeline_view::time_at_content_x(float content_x) const
	{
		const metrics m = layout_metrics();
		return x_to_time((double)content_x - m.lanes_x);
	}

	float timeline_view::content_x_at_time(double time) const
	{
		const metrics m = layout_metrics();
		return (float)(m.lanes_x + time_to_x(time));
	}

	/* --------------------------------------------------------------------- */
	/* Snapping / timing                                                     */
	/* --------------------------------------------------------------------- */

	void timeline_view::set_snap_enabled(bool enabled)
	{
		if (snap_enabled_ == enabled)
			return;
		snap_enabled_ = enabled;
		notify_change();
	}

	bool timeline_view::snap_enabled() const noexcept
	{
		return snap_enabled_;
	}

	void timeline_view::set_snap_step(double seconds)
	{
		if (!std::isfinite(seconds) || seconds <= 0.0)
			return;
		const bool changed = !near_time(snap_step_, seconds) || snap_to_frames_;
		snap_step_ = seconds;
		snap_to_frames_ = false;
		if (changed)
			notify_change();
	}

	double timeline_view::snap_step() const noexcept
	{
		return snap_step_;
	}

	void timeline_view::set_snap_to_frames(bool enabled)
	{
		if (snap_to_frames_ == enabled)
			return;
		snap_to_frames_ = enabled;
		if (enabled)
			snap_step_ = 1.0 / frame_rate_;
		notify_change();
	}

	bool timeline_view::snap_to_frames() const noexcept
	{
		return snap_to_frames_;
	}

	double timeline_view::snap_time(double time) const noexcept
	{
		return snap_time_value(time);
	}

	double timeline_view::snap_time_value(double time) const noexcept
	{
		if (!snap_enabled_ || !(snap_step_ > 0.0) || !std::isfinite(time))
			return time;
		return std::round(time / snap_step_) * snap_step_;
	}

	void timeline_view::set_frame_rate(double fps)
	{
		if (!std::isfinite(fps) || fps <= 0.0)
			return;
		if (near_time(frame_rate_, fps))
			return;
		frame_rate_ = fps;
		if (snap_to_frames_)
			snap_step_ = 1.0 / frame_rate_;
		notify_change();
	}

	double timeline_view::frame_rate() const noexcept
	{
		return frame_rate_;
	}

	void timeline_view::set_duration(double seconds)
	{
		if (!std::isfinite(seconds) || seconds <= 0.0)
			return;
		if (near_time(duration_, seconds))
			return;
		duration_ = seconds;
		if (playhead_ > duration_)
		{
			playhead_ = duration_;
			notify_playhead();
		}
		notify_change();
	}

	double timeline_view::duration() const noexcept
	{
		return duration_;
	}

	/* --------------------------------------------------------------------- */
	/* Playback                                                              */
	/* --------------------------------------------------------------------- */

	void timeline_view::set_playhead(double seconds)
	{
		if (!std::isfinite(seconds))
			return;
		const double next = std::clamp(seconds, 0.0, duration_);
		if (near_time(next, playhead_))
			return;
		playhead_ = next;
		notify_playhead();
		notify_change();
	}

	double timeline_view::playhead() const noexcept
	{
		return playhead_;
	}

	void timeline_view::play()
	{
		if (playing_)
			return;
		if (playhead_ >= duration_ && !loop_)
		{
			playhead_ = 0.0;
			notify_playhead();
		}
		playing_ = true;
		notify_change();
	}

	void timeline_view::pause()
	{
		if (!playing_)
			return;
		playing_ = false;
		notify_change();
	}

	void timeline_view::stop()
	{
		const bool was_playing = playing_;
		const bool had_time = !near_time(playhead_, 0.0);
		playing_ = false;
		playhead_ = 0.0;
		if (had_time)
			notify_playhead();
		if (was_playing || had_time)
			notify_change();
	}

	void timeline_view::toggle_play()
	{
		if (playing_)
			pause();
		else
			play();
	}

	bool timeline_view::playing() const noexcept
	{
		return playing_;
	}

	void timeline_view::set_loop(bool loop)
	{
		if (loop_ == loop)
			return;
		loop_ = loop;
		notify_change();
	}

	bool timeline_view::loop() const noexcept
	{
		return loop_;
	}

	void timeline_view::tick(double delta_seconds)
	{
		if (!playing_ || !std::isfinite(delta_seconds) || delta_seconds <= 0.0)
			return;
		double next = playhead_ + delta_seconds;
		bool stopped = false;
		if (loop_)
		{
			if (duration_ > 0.0)
				next = std::fmod(next, duration_);
			if (next < 0.0)
				next += duration_;
		}
		else if (next >= duration_)
		{
			next = duration_;
			playing_ = false;
			stopped = true;
		}
		const bool moved = !near_time(next, playhead_);
		if (moved)
		{
			playhead_ = next;
			notify_playhead();
		}
		if (moved || stopped)
			notify_change();
	}

	void timeline_view::step_frames(int frames)
	{
		if (frames == 0 || !(frame_rate_ > 0.0))
			return;
		const bool was_playing = playing_;
		playing_ = false;
		const double next = std::clamp(
			playhead_ + (double)frames / frame_rate_, 0.0, duration_);
		const bool moved = !near_time(next, playhead_);
		if (moved)
		{
			playhead_ = next;
			notify_playhead();
		}
		if (moved || was_playing)
			notify_change();
	}

	void timeline_view::set_playhead_listener(playhead_listener listener)
	{
		playhead_listener_ = std::move(listener);
	}

	/* --------------------------------------------------------------------- */
	/* Editing                                                               */
	/* --------------------------------------------------------------------- */

	void timeline_view::set_command_stack(command_stack* stack)
	{
		commands_ = stack;
	}

	command_stack* timeline_view::command_stack_ptr() const noexcept
	{
		return commands_;
	}

	std::vector<timeline_view::keyframe_ref> timeline_view::snapshot_selection() const
	{
		std::vector<keyframe_ref> refs;
		refs.reserve(selection_.size());
		for (const timeline_track& track : tracks_)
		{
			for (const timeline_keyframe& keyframe : track.keyframes)
			{
				if (std::find(selection_.begin(), selection_.end(),
					keyframe.id) == selection_.end())
					continue;
				refs.push_back({ track.id, keyframe });
			}
		}
		return refs;
	}

	std::vector<timeline_view::keyframe_ref> timeline_view::selection_refs() const
	{
		return snapshot_selection();
	}

	void timeline_view::copy_selection()
	{
		std::vector<keyframe_ref> refs = snapshot_selection();
		if (refs.empty())
		{
			clipboard_.clear();
			return;
		}
		double earliest = refs.front().keyframe.time;
		for (const keyframe_ref& ref : refs)
			earliest = std::min(earliest, ref.keyframe.time);
		for (keyframe_ref& ref : refs)
			ref.keyframe.time -= earliest;
		clipboard_ = std::move(refs);
	}

	void timeline_view::cut_selection()
	{
		copy_selection();
		delete_selection();
	}

	bool timeline_view::paste_at_playhead()
	{
		if (clipboard_.empty())
			return false;

		const double base = snap_enabled_ ? snap_time_value(playhead_)
			: playhead_;
		std::vector<keyframe_ref> after;
		after.reserve(clipboard_.size());
		for (const keyframe_ref& entry : clipboard_)
		{
			object_id target = entry.track;
			if (find_track(target) == nullptr)
				target = selected_track_;
			if (find_track(target) == nullptr && !tracks_.empty())
				target = tracks_.front().id;
			if (find_track(target) == nullptr)
				continue;

			timeline_keyframe keyframe = entry.keyframe;
			keyframe.time = std::clamp(base + keyframe.time, 0.0, duration_);
			keyframe.id = invalid_object;
			const object_id id = add_keyframe(target, keyframe);
			if (id == invalid_object)
				continue;
			const timeline_keyframe* added = find_keyframe(id);
			if (added)
				after.push_back({ target, *added });
		}
		if (after.empty())
			return false;

		push_keyframe_command("Paste keyframes", {}, after);
		std::vector<object_id> ids;
		ids.reserve(after.size());
		for (const keyframe_ref& ref : after)
			ids.push_back(ref.keyframe.id);
		set_selected_track(after.front().track);
		set_selection(ids, ids.front());
		notify_change();
		return true;
	}

	bool timeline_view::delete_selection()
	{
		std::vector<keyframe_ref> before = snapshot_selection();
		if (before.empty())
			return false;
		for (const keyframe_ref& ref : before)
		{
			timeline_track* track = find_track(ref.track);
			if (!track)
				continue;
			track->keyframes.erase(std::remove_if(track->keyframes.begin(),
				track->keyframes.end(),
				[&](const timeline_keyframe& keyframe) {
					return keyframe.id == ref.keyframe.id;
				}), track->keyframes.end());
		}
		push_keyframe_command("Delete keyframes", before, {});
		selection_.clear();
		primary_ = invalid_object;
		notify_selection();
		notify_change();
		return true;
	}

	object_id timeline_view::insert_keyframe(object_id track_id, double time,
		double value)
	{
		timeline_track* track = find_track(track_id);
		if (!track || track->locked || !std::isfinite(time) ||
			!std::isfinite(value))
			return invalid_object;
		double target = std::clamp(time, 0.0, duration_);
		if (snap_enabled_)
			target = std::clamp(snap_time_value(target), 0.0, duration_);
		timeline_keyframe keyframe;
		keyframe.time = target;
		keyframe.value = value;
		const object_id id = add_keyframe(track_id, keyframe);
		if (id == invalid_object)
			return invalid_object;
		const timeline_keyframe* added = find_keyframe(id);
		if (added)
			push_keyframe_command("Add keyframe", {},
				{ { track_id, *added } });
		set_selected_track(track_id);
		set_selection({ id }, id);
		return id;
	}

	object_id timeline_view::add_keyframe_at_playhead()
	{
		object_id target = selected_track_;
		if (find_track(target) == nullptr)
		{
			for (const timeline_track& track : tracks_)
			{
				if (!track.locked)
				{
					target = track.id;
					break;
				}
			}
		}
		if (find_track(target) == nullptr)
			return invalid_object;
		return insert_keyframe(target, playhead_, 0.0);
	}

	std::size_t timeline_view::clipboard_count() const noexcept
	{
		return clipboard_.size();
	}

	/* --------------------------------------------------------------------- */
	/* Geometry                                                              */
	/* --------------------------------------------------------------------- */

	bool timeline_view::content_geometry(float* offset_x, float* offset_y,
		float* width, float* height) const
	{
		if (offset_x)
			*offset_x = kBorderInset;
		if (offset_y)
			*offset_y = kBorderInset;
		if (!widget_)
		{
			if (width)
				*width = 0.0f;
			if (height)
				*height = 0.0f;
			return false;
		}
		float wx = 0.0f, wy = 0.0f, ww = 0.0f, wh = 0.0f;
		vgui_widget_get_rect(widget_, &wx, &wy, &ww, &wh);
		(void)wx;
		(void)wy;
		if (width)
			*width = std::max(0.0f, ww - kBorderInset * 2.0f);
		if (height)
			*height = std::max(0.0f, wh - kBorderInset * 2.0f);
		return ww > kBorderInset * 2.0f && wh > kBorderInset * 2.0f;
	}

	timeline_view::metrics timeline_view::layout_metrics() const
	{
		metrics m;
		float width = 0.0f, height = 0.0f;
		content_geometry(nullptr, nullptr, &width, &height);
		const float scale = ui_scale();
		m.width = width;
		m.height = height;
		m.transport_y = 0.0f;
		m.transport_height = std::min(kTransportHeight * scale,
			std::max(0.0f, height));
		m.ruler_y = m.transport_height;
		m.ruler_height = std::min(kRulerHeight * scale,
			std::max(0.0f, height - m.ruler_y));
		m.header_width = std::clamp(width * 0.28f, 92.0f * scale,
			kHeaderWidth * scale);
		m.lanes_x = m.header_width;
		m.lanes_y = m.ruler_y + m.ruler_height;
		m.lanes_width = std::max(0.0f, width - m.lanes_x);
		m.lanes_height = std::max(0.0f, height - m.lanes_y);
		return m;
	}

	float timeline_view::track_height_at(std::size_t index) const
	{
		if (index >= tracks_.size())
			return 0.0f;
		/* Track height is a logical model value; scroll positions stay in
		 * effective pixels like the table view. */
		return clamp_track_height(tracks_[index].height) * ui_scale();
	}

	float timeline_view::total_track_height() const
	{
		float total = 0.0f;
		for (std::size_t i = 0; i < tracks_.size(); ++i)
			total += track_height_at(i);
		return total;
	}

	float timeline_view::max_scroll_y(const metrics& m) const
	{
		return std::max(0.0f, total_track_height() - m.lanes_height);
	}

	void timeline_view::clamp_scroll()
	{
		const metrics m = layout_metrics();
		const float maximum = max_scroll_y(m);
		if (view_.scroll_y < 0.0f)
			view_.scroll_y = 0.0f;
		else if (view_.scroll_y > maximum)
			view_.scroll_y = maximum;
	}

	float timeline_view::track_y(const metrics& m, std::size_t index) const
	{
		float y = m.lanes_y - view_.scroll_y;
		for (std::size_t i = 0; i < index && i < tracks_.size(); ++i)
			y += track_height_at(i);
		return y;
	}

	float timeline_view::track_center_y(const metrics& m,
		std::size_t index) const
	{
		return track_y(m, index) + track_height_at(index) * 0.5f;
	}

	bool timeline_view::track_rect(object_id track, float* x, float* y,
		float* width, float* height) const
	{
		const metrics m = layout_metrics();
		for (std::size_t i = 0; i < tracks_.size(); ++i)
		{
			if (tracks_[i].id != track)
				continue;
			if (x)
				*x = 0.0f;
			if (y)
				*y = track_y(m, i);
			if (width)
				*width = m.width;
			if (height)
				*height = track_height_at(i);
			return true;
		}
		return false;
	}

	bool timeline_view::keyframe_rect(object_id keyframe, float* x, float* y,
		float* width, float* height) const
	{
		const metrics m = layout_metrics();
		const float hit = kKeyframeHit * ui_scale();
		for (std::size_t i = 0; i < tracks_.size(); ++i)
		{
			const timeline_track& track = tracks_[i];
			for (const timeline_keyframe& current : track.keyframes)
			{
				if (current.id != keyframe)
					continue;
				const float cx = (float)(m.lanes_x + time_to_x(current.time));
				const float cy = track_center_y(m, i);
				if (x)
					*x = cx - hit;
				if (y)
					*y = cy - hit;
				if (width)
					*width = hit * 2.0f;
				if (height)
					*height = hit * 2.0f;
				return true;
			}
		}
		return false;
	}

	object_id timeline_view::hit_test(float content_x,
		float content_y) const
	{
		const metrics m = layout_metrics();
		const float hit = kKeyframeHit * ui_scale();
		if (content_y < m.lanes_y || content_y >= m.y + m.height ||
			content_x < m.lanes_x || content_x >= m.x + m.width)
			return invalid_object;
		for (std::size_t i = 0; i < tracks_.size(); ++i)
		{
			const timeline_track& track = tracks_[i];
			if (!track.visible || track.locked)
				continue;
			const float center_y = track_center_y(m, i);
			if (std::fabs(content_y - center_y) > hit)
				continue;
			for (auto it = track.keyframes.rbegin();
				it != track.keyframes.rend(); ++it)
			{
				const float center_x = (float)(m.lanes_x +
					time_to_x(it->time));
				if (std::fabs(content_x - center_x) <= hit)
					return it->id;
			}
		}
		return invalid_object;
	}

	object_id timeline_view::track_at(float content_y) const
	{
		const metrics m = layout_metrics();
		if (content_y < m.lanes_y || content_y >= m.y + m.height)
			return invalid_object;
		for (std::size_t i = 0; i < tracks_.size(); ++i)
		{
			const float top = track_y(m, i);
			if (content_y >= top && content_y < top + track_height_at(i))
				return tracks_[i].id;
		}
		return invalid_object;
	}

	bool timeline_view::dragging() const noexcept
	{
		return gesture_ != timeline_gesture::none;
	}

	timeline_gesture timeline_view::gesture() const noexcept
	{
		return gesture_;
	}

	void timeline_view::set_drag_threshold(float pixels)
	{
		drag_threshold_ = pixels;
	}

	float timeline_view::drag_threshold() const noexcept
	{
		return drag_threshold_;
	}

	void timeline_view::set_enabled(bool enabled)
	{
		if (enabled_ == enabled)
			return;
		if (!enabled)
			cancel_gesture();
		enabled_ = enabled;
	}

	bool timeline_view::enabled() const noexcept
	{
		return enabled_;
	}

	void timeline_view::request_focus()
	{
		if (context_ && widget_)
			vgui_set_focus(context_, widget_);
	}

	bool timeline_view::focused() const
	{
		return context_ && widget_ && vgui_get_focus(context_) == widget_;
	}

	/* --------------------------------------------------------------------- */
	/* Persistence                                                           */
	/* --------------------------------------------------------------------- */

	timeline_state timeline_view::capture_state() const
	{
		timeline_state state;
		state.view = view_;
		state.playhead = playhead_;
		state.duration = duration_;
		state.frame_rate = frame_rate_;
		state.snap_enabled = snap_enabled_;
		state.snap_step = snap_step_;
		state.loop = loop_;
		state.selected_track = selected_track_;
		return state;
	}

	bool timeline_view::apply_state(const timeline_state& state,
		std::string* error)
	{
		std::string reason;
		if (!validate_timeline_state(state, &reason))
		{
			if (error)
				*error = reason;
			return false;
		}
		if (gesture_ != timeline_gesture::none)
			cancel_gesture();

		const double old_playhead = playhead_;
		view_.view_start = state.view.view_start;
		view_.pixels_per_second = std::clamp(state.view.pixels_per_second,
			min_zoom_, max_zoom_);
		view_.scroll_y = state.view.scroll_y;
		duration_ = state.duration;
		playhead_ = std::clamp(state.playhead, 0.0, duration_);
		frame_rate_ = state.frame_rate;
		snap_enabled_ = state.snap_enabled;
		snap_step_ = state.snap_step;
		snap_to_frames_ = std::fabs(snap_step_ - 1.0 / frame_rate_) <=
			1e-12;
		loop_ = state.loop;
		selected_track_ = find_track(state.selected_track) != nullptr
			? state.selected_track : invalid_object;
		clamp_scroll();
		if (!near_time(old_playhead, playhead_))
			notify_playhead();
		notify_change();
		return true;
	}

	/* --------------------------------------------------------------------- */
	/* Helpers                                                               */
	/* --------------------------------------------------------------------- */

	void timeline_view::notify_change()
	{
		if (change_listener_)
			change_listener_();
	}

	void timeline_view::notify_selection()
	{
		if (selection_listener_)
			selection_listener_(selection_);
	}

	void timeline_view::notify_playhead()
	{
		if (playhead_listener_)
			playhead_listener_(playhead_);
	}

	void timeline_view::sort_keyframes(timeline_track& track)
	{
		std::stable_sort(track.keyframes.begin(), track.keyframes.end(),
			[](const timeline_keyframe& a, const timeline_keyframe& b) {
				if (a.time != b.time)
					return a.time < b.time;
				return a.id < b.id;
			});
	}

	void timeline_view::sort_all_keyframes()
	{
		for (timeline_track& track : tracks_)
			sort_keyframes(track);
	}

	void timeline_view::prune_selection()
	{
		std::vector<object_id> filtered;
		filtered.reserve(selection_.size());
		for (object_id id : selection_)
			if (find_keyframe(id) != nullptr)
				filtered.push_back(id);
		if (filtered.size() == selection_.size())
			return;
		object_id primary = primary_;
		if (std::find(filtered.begin(), filtered.end(), primary) ==
			filtered.end())
			primary = filtered.empty() ? invalid_object : filtered.front();
		selection_ = std::move(filtered);
		primary_ = primary;
		notify_selection();
	}

	bool timeline_view::append_keyframe(object_id track_id,
		timeline_keyframe keyframe)
	{
		timeline_track* track = find_track(track_id);
		if (!track)
			return false;
		if (keyframe.id == invalid_object)
		{
			while (find_keyframe(next_keyframe_id_) != nullptr)
				++next_keyframe_id_;
			keyframe.id = next_keyframe_id_++;
		}
		else
		{
			if (find_keyframe(keyframe.id) != nullptr)
				return false;
			if (keyframe.id >= next_keyframe_id_)
				next_keyframe_id_ = keyframe.id + 1;
		}
		track->keyframes.push_back(keyframe);
		sort_keyframes(*track);
		if (selected_track_ == invalid_object)
			selected_track_ = track_id;
		return true;
	}

	void timeline_view::apply_selection(const std::vector<object_id>& picked,
		int mods)
	{
		const bool ctrl = multi_select_ && (mods & VGUI_MOD_CTRL) != 0;
		const bool shift = multi_select_ && (mods & VGUI_MOD_SHIFT) != 0;
		if (picked.empty() && (ctrl || shift))
			return;

		std::vector<object_id> next;
		object_id primary = invalid_object;
		if (ctrl)
		{
			next = selection_;
			for (object_id id : picked)
			{
				auto it = std::find(next.begin(), next.end(), id);
				if (it != next.end())
					next.erase(it);
				else if (find_keyframe(id) != nullptr)
					next.push_back(id);
			}
			primary = primary_;
		}
		else if (shift)
		{
			next = selection_;
			for (object_id id : picked)
			{
				if (find_keyframe(id) != nullptr &&
					std::find(next.begin(), next.end(), id) == next.end())
					next.push_back(id);
			}
			primary = primary_;
		}
		else
		{
			next = picked;
			if (!next.empty())
				primary = next.front();
		}

		if (primary == invalid_object ||
			std::find(next.begin(), next.end(), primary) == next.end())
			primary = next.empty() ? invalid_object : next.front();
		set_selection(next, primary);
	}

	std::vector<object_id> timeline_view::keyframes_in_rect(float x0,
		float y0, float x1, float y1) const
	{
		std::vector<object_id> result;
		const float left = std::min(x0, x1);
		const float right = std::max(x0, x1);
		const float top = std::min(y0, y1);
		const float bottom = std::max(y0, y1);
		if (right - left <= 0.0f || bottom - top <= 0.0f)
			return result;
		const metrics m = layout_metrics();
		for (std::size_t i = 0; i < tracks_.size(); ++i)
		{
			const timeline_track& track = tracks_[i];
			if (!track.visible || track.locked)
				continue;
			const float center_y = track_center_y(m, i);
			if (center_y < top || center_y > bottom)
				continue;
			for (const timeline_keyframe& keyframe : track.keyframes)
			{
				const float center_x = (float)(m.lanes_x +
					time_to_x(keyframe.time));
				if (center_x >= left && center_x <= right)
					result.push_back(keyframe.id);
			}
		}
		return result;
	}

	void timeline_view::capture_gesture_keyframes()
	{
		gesture_start_.clear();
		for (const timeline_track& track : tracks_)
		{
			if (!track.visible || track.locked)
				continue;
			for (const timeline_keyframe& keyframe : track.keyframes)
			{
				if (is_selected(keyframe.id))
					gesture_start_.push_back({ track.id, keyframe });
			}
		}
	}

	void timeline_view::update_move(float content_x, float content_y)
	{
		(void)content_y;
		if (gesture_start_.empty())
			return;
		const keyframe_ref* primary = nullptr;
		for (const keyframe_ref& ref : gesture_start_)
		{
			if (ref.keyframe.id == primary_)
			{
				primary = &ref;
				break;
			}
		}
		if (!primary)
			primary = &gesture_start_.front();

		const metrics m = layout_metrics();
		const double pointer_time = x_to_time((double)content_x - m.lanes_x);
		const double target = snap_enabled_ ? snap_time_value(pointer_time)
			: pointer_time;
		double delta = target - primary->keyframe.time;

		double minimum = std::numeric_limits<double>::max();
		double maximum = std::numeric_limits<double>::lowest();
		for (const keyframe_ref& ref : gesture_start_)
		{
			minimum = std::min(minimum, ref.keyframe.time);
			maximum = std::max(maximum, ref.keyframe.time);
		}
		delta = std::clamp(delta, -minimum, duration_ - maximum);

		bool changed = false;
		std::vector<object_id> touched;
		for (const keyframe_ref& ref : gesture_start_)
		{
			timeline_keyframe* keyframe = find_keyframe(ref.keyframe.id);
			if (!keyframe)
				continue;
			const double next = ref.keyframe.time + delta;
			if (near_time(keyframe->time, next))
				continue;
			keyframe->time = next;
			changed = true;
			if (std::find(touched.begin(), touched.end(), ref.track) ==
				touched.end())
				touched.push_back(ref.track);
		}
		if (!changed)
			return;
		for (object_id track : touched)
		{
			timeline_track* found = find_track(track);
			if (found)
				sort_keyframes(*found);
		}
		notify_change();
	}

	void timeline_view::finish_move(float content_x, float content_y)
	{
		update_move(content_x, content_y);
		if (gesture_changed())
		{
			std::vector<keyframe_ref> after;
			after.reserve(gesture_start_.size());
			for (const keyframe_ref& ref : gesture_start_)
			{
				object_id track = invalid_object;
				const timeline_keyframe* keyframe = find_keyframe(
					ref.keyframe.id, &track);
				if (keyframe)
					after.push_back({ track, *keyframe });
			}
			push_keyframe_command("Move keyframes", gesture_start_, after);
		}
		reset_gesture();
	}

	bool timeline_view::gesture_changed() const
	{
		for (const keyframe_ref& ref : gesture_start_)
		{
			const timeline_keyframe* keyframe = find_keyframe(ref.keyframe.id);
			if (!keyframe)
				return true;
			if (!near_time(keyframe->time, ref.keyframe.time))
				return true;
		}
		return false;
	}

	void timeline_view::apply_keyframe_states(
		const std::vector<keyframe_ref>& before,
		const std::vector<keyframe_ref>& after)
	{
		std::vector<object_id> ids;
		ids.reserve(before.size() + after.size());
		for (const keyframe_ref& ref : before)
			ids.push_back(ref.keyframe.id);
		for (const keyframe_ref& ref : after)
			ids.push_back(ref.keyframe.id);
		for (timeline_track& track : tracks_)
		{
			track.keyframes.erase(std::remove_if(track.keyframes.begin(),
				track.keyframes.end(),
				[&](const timeline_keyframe& keyframe) {
					return std::find(ids.begin(), ids.end(), keyframe.id) !=
						ids.end();
				}), track.keyframes.end());
		}
		for (const keyframe_ref& ref : after)
		{
			timeline_track* track = find_track(ref.track);
			if (!track)
				continue;
			track->keyframes.push_back(ref.keyframe);
		}
		sort_all_keyframes();
		prune_selection();
		notify_change();
	}

	void timeline_view::push_keyframe_command(const char* name,
		const std::vector<keyframe_ref>& before,
		const std::vector<keyframe_ref>& after)
	{
		if (!commands_ || (before.empty() && after.empty()))
			return;
		std::weak_ptr<int> alive = alive_token_;
		auto apply = [this, alive](const std::vector<keyframe_ref>& undo_states,
			const std::vector<keyframe_ref>& redo_states)
		{
			if (!alive.expired())
				apply_keyframe_states(undo_states, redo_states);
		};
		commands_->push(std::make_unique<timeline_command>(
			name ? name : "Timeline", before, after, std::move(apply)));
	}

	void timeline_view::cancel_gesture_impl()
	{
		if (gesture_ == timeline_gesture::move ||
			gesture_ == timeline_gesture::pending_move)
		{
			bool changed = false;
			for (const keyframe_ref& ref : gesture_start_)
			{
				timeline_keyframe* keyframe = find_keyframe(ref.keyframe.id);
				if (!keyframe)
					continue;
				if (!near_time(keyframe->time, ref.keyframe.time))
				{
					keyframe->time = ref.keyframe.time;
					changed = true;
				}
			}
			if (changed)
			{
				sort_all_keyframes();
				notify_change();
			}
		}
		else if (gesture_ == timeline_gesture::pan)
		{
			if (!near_time(gesture_view_.view_start, view_.view_start))
			{
				view_.view_start = gesture_view_.view_start;
				notify_change();
			}
		}
		else if (gesture_ == timeline_gesture::scrub)
		{
			if (!near_time(scrub_start_playhead_, playhead_))
				set_playhead(scrub_start_playhead_);
		}
		reset_gesture();
	}

	void timeline_view::cancel_gesture()
	{
		cancel_gesture_impl();
	}

	void timeline_view::reset_gesture()
	{
		gesture_ = timeline_gesture::none;
		gesture_start_.clear();
		scrub_start_playhead_ = 0.0;
	}

	float timeline_view::effective_drag_threshold() const
	{
		if (drag_threshold_ >= 0.0f)
			return drag_threshold_;
		if (context_)
		{
			vgui_style style = {};
			vgui_context_get_style(context_, &style);
			return std::max(1.0f, style.dock.drag_threshold);
		}
		return 4.0f;
	}

	float timeline_view::ui_scale() const
	{
		return ui_scale_of(widget_);
	}

	int timeline_view::transport_hit(float content_x, float content_y) const
	{
		const metrics m = layout_metrics();
		if (content_y < m.transport_y ||
			content_y >= m.transport_y + m.transport_height ||
			content_x < 0.0f || content_x >= m.width)
			return -1;
		const float scale = ui_scale();
		static const float widths[5] = { 34.0f, 28.0f, 48.0f, 28.0f, 40.0f };
		float x = 6.0f * scale;
		for (int i = 0; i < 5; ++i)
		{
			const float width = widths[i] * scale;
			if (content_x >= x && content_x < x + width)
				return i;
			x += width + 4.0f * scale;
		}
		return -1;
	}

	float timeline_view::transport_button_x(int button) const
	{
		const float scale = ui_scale();
		static const float widths[5] = { 34.0f, 28.0f, 48.0f, 28.0f, 40.0f };
		float x = 6.0f * scale;
		for (int i = 0; i < 5 && i < button; ++i)
			x += widths[i] * scale + 4.0f * scale;
		return x;
	}

	double timeline_view::choose_time_step() const
	{
		static const double steps[] = {
			1.0 / 30.0, 1.0 / 15.0, 0.1, 0.2, 0.5, 1.0, 2.0, 5.0,
			10.0, 30.0, 60.0, 300.0, 600.0
		};
		const double min_label_width = 14.0 * (double)ui_scale();
		for (double step : steps)
		{
			if (step * view_.pixels_per_second >= min_label_width)
				return step;
		}
		return steps[sizeof(steps) / sizeof(steps[0]) - 1];
	}

	/* --------------------------------------------------------------------- */
	/* Input                                                                 */
	/* --------------------------------------------------------------------- */

	int timeline_view::on_event(vgui_widget* widget, const vgui_event* event,
		void* user)
	{
		auto* self = static_cast<timeline_view*>(user);
		if (!self || !event || !widget || !self->enabled_)
			return 0;

		float ox = 0.0f, oy = 0.0f, cw = 0.0f, ch = 0.0f;
		self->content_geometry(&ox, &oy, &cw, &ch);
		/* Custom-widget mouse events are local to the widget; strip the 1px
		 * border so the timeline works in content coordinates. */
		const float x = event->x - ox;
		const float y = event->y - oy;
		const bool inside = x >= 0.0f && y >= 0.0f &&
			x < cw && y < ch;
		const metrics m = self->layout_metrics();
		const int mods = self->context_
			? vgui_context_get_mods(self->context_) : event->mods;
		const float wheel_step = 40.0f * self->ui_scale();

		switch (event->type)
		{
		case VGUI_EVENT_MOUSE_DOWN:
		{
			if (self->gesture_ != timeline_gesture::none)
				return 0;
			if (event->button == 2 && inside)
			{
				self->gesture_ = timeline_gesture::pan;
				self->gesture_view_ = self->view_;
				self->press_x_ = x;
				self->press_y_ = y;
				self->request_focus();
				return 1;
			}
			if (event->button != 1 || !inside)
				return 0;
			self->request_focus();

			{
				const int transport = self->transport_hit(x, y);
				if (transport >= 0)
				{
					switch (transport)
					{
					case 0: self->stop(); break;
					case 1: self->step_frames(-1); break;
					case 2: self->toggle_play(); break;
					case 3: self->step_frames(1); break;
					case 4: self->set_loop(!self->loop_); break;
					default: break;
					}
					return 1;
				}
			}

			if (y >= m.ruler_y && y < m.lanes_y)
			{
				if (x >= m.lanes_x)
				{
					self->gesture_ = timeline_gesture::scrub;
					self->press_x_ = x;
					self->press_y_ = y;
					self->scrub_start_playhead_ = self->playhead_;
					const double time = self->x_to_time((double)x -
						m.lanes_x);
					self->set_playhead(self->snap_enabled_
						? self->snap_time_value(time) : time);
				}
				return 1;
			}
			if (y < m.lanes_y)
				return 1;

			const object_id hit = self->hit_test(x, y);
			if (hit != invalid_object)
			{
				const bool ctrl = self->multi_select_ &&
					(mods & VGUI_MOD_CTRL) != 0;
				const bool shift = self->multi_select_ &&
					(mods & VGUI_MOD_SHIFT) != 0;
				std::vector<object_id> next = self->selection_;
				object_id primary = self->primary_;
				if (ctrl)
				{
					auto it = std::find(next.begin(), next.end(), hit);
					if (it != next.end())
						next.erase(it);
					else
						next.push_back(hit);
					if (std::find(next.begin(), next.end(), primary) ==
						next.end())
						primary = next.empty() ? invalid_object
							: next.front();
				}
				else if (shift)
				{
					if (std::find(next.begin(), next.end(), hit) ==
						next.end())
						next.push_back(hit);
					primary = hit;
				}
				else if (!self->is_selected(hit))
				{
					next = { hit };
					primary = hit;
				}
				else
				{
					primary = hit;
				}
				self->set_selection(next, primary);
				if (self->is_selected(hit))
				{
					self->capture_gesture_keyframes();
					if (!self->gesture_start_.empty())
					{
						self->gesture_ = timeline_gesture::pending_move;
						self->press_x_ = x;
						self->press_y_ = y;
					}
				}
				return 1;
			}

			if (x < m.lanes_x)
			{
				const object_id track = self->track_at(y);
				if (track != invalid_object)
				{
					self->set_selected_track(track);
					self->clear_selection();
				}
				return 1;
			}

			if (event->clicks >= 2)
			{
				const object_id track = self->track_at(y);
				if (track != invalid_object)
					self->insert_keyframe(track,
						self->x_to_time((double)x - m.lanes_x));
				return 1;
			}

			self->gesture_ = timeline_gesture::pending_marquee;
			self->press_x_ = x;
			self->press_y_ = y;
			self->marquee_x_ = x;
			self->marquee_y_ = y;
			return 1;
		}

		case VGUI_EVENT_MOUSE_MOVE:
			if (self->gesture_ == timeline_gesture::pan)
			{
				self->view_.view_start = self->gesture_view_.view_start -
					(double)(x - self->press_x_) /
						self->view_.pixels_per_second;
				self->notify_change();
				return 1;
			}
			if (self->gesture_ == timeline_gesture::scrub)
			{
				const double time = self->x_to_time((double)x - m.lanes_x);
				self->set_playhead(self->snap_enabled_
					? self->snap_time_value(time) : time);
				return 1;
			}
			if (self->gesture_ == timeline_gesture::pending_move ||
				self->gesture_ == timeline_gesture::pending_marquee)
			{
				const float threshold = self->effective_drag_threshold();
				if (std::fabs(x - self->press_x_) > threshold ||
					std::fabs(y - self->press_y_) > threshold)
				{
					self->gesture_ =
						self->gesture_ == timeline_gesture::pending_move
							? timeline_gesture::move
							: timeline_gesture::marquee;
				}
				else
				{
					return 1;
				}
			}
			if (self->gesture_ == timeline_gesture::move)
			{
				self->update_move(x, y);
				return 1;
			}
			if (self->gesture_ == timeline_gesture::marquee)
			{
				self->marquee_x_ = x;
				self->marquee_y_ = y;
				return 1;
			}
			return 0;

		case VGUI_EVENT_MOUSE_UP:
			if (event->button == 2 && self->gesture_ == timeline_gesture::pan)
			{
				self->reset_gesture();
				return 1;
			}
			if (event->button != 1)
				return 0;
			if (self->gesture_ == timeline_gesture::pending_move)
			{
				self->reset_gesture();
				return 1;
			}
			if (self->gesture_ == timeline_gesture::move)
			{
				self->finish_move(x, y);
				return 1;
			}
			if (self->gesture_ == timeline_gesture::pending_marquee)
			{
				self->apply_selection({}, mods);
				self->reset_gesture();
				return 1;
			}
			if (self->gesture_ == timeline_gesture::marquee)
			{
				const std::vector<object_id> picked =
					self->keyframes_in_rect(self->press_x_,
						self->press_y_, x, y);
				self->apply_selection(picked, mods);
				self->reset_gesture();
				return 1;
			}
			if (self->gesture_ == timeline_gesture::scrub)
			{
				self->reset_gesture();
				return 1;
			}
			return 0;

		case VGUI_EVENT_MOUSE_WHEEL:
			if (!inside || self->gesture_ != timeline_gesture::none)
				return 0;
			if ((mods & VGUI_MOD_CTRL) != 0 && event->wheel_y != 0.0f)
			{
				self->zoom_at(x - m.lanes_x,
					std::exp(event->wheel_y * 0.15));
				return 1;
			}
			if ((mods & VGUI_MOD_SHIFT) != 0 && event->wheel_y != 0.0f)
			{
				self->view_.view_start -= event->wheel_y * wheel_step /
					self->view_.pixels_per_second;
				self->notify_change();
				return 1;
			}
			if (event->wheel_x != 0.0f)
			{
				self->view_.view_start -= event->wheel_x * wheel_step /
					self->view_.pixels_per_second;
				self->notify_change();
				return 1;
			}
			if (event->wheel_y != 0.0f)
			{
				const float maximum = self->max_scroll_y(m);
				const float next = std::clamp(self->view_.scroll_y -
					event->wheel_y * 30.0f * self->ui_scale(), 0.0f,
					maximum);
				if (std::fabs(next - self->view_.scroll_y) > 1e-4f)
				{
					self->view_.scroll_y = next;
					self->notify_change();
				}
				return 1;
			}
			return 0;

		case VGUI_EVENT_KEY_DOWN:
		{
			const int key_mods = event->mods;
			const bool ctrl = (key_mods & VGUI_MOD_CTRL) != 0;
			const int key = event->key;
			if (key == ' ')
			{
				self->toggle_play();
				return 1;
			}
			if (key == VGUI_KEY_DELETE || key == VGUI_KEY_BACKSPACE)
				return self->delete_selection() ? 1 : 0;
			if (ctrl && (key == 'C' || key == 'c'))
			{
				if (self->selection_.empty())
					return 0;
				self->copy_selection();
				return 1;
			}
			if (ctrl && (key == 'X' || key == 'x'))
			{
				if (self->selection_.empty())
					return 0;
				self->cut_selection();
				return 1;
			}
			if (ctrl && (key == 'V' || key == 'v'))
				return self->paste_at_playhead() ? 1 : 0;
			if (ctrl && (key == 'A' || key == 'a'))
			{
				self->select_all();
				return 1;
			}
			if (key == VGUI_KEY_LEFT)
			{
				self->step_frames(-1);
				return 1;
			}
			if (key == VGUI_KEY_RIGHT)
			{
				self->step_frames(1);
				return 1;
			}
			if (key == VGUI_KEY_HOME)
			{
				self->set_playhead(0.0);
				return 1;
			}
			if (key == VGUI_KEY_END)
			{
				self->set_playhead(self->duration_);
				return 1;
			}
			if (key == VGUI_KEY_ESCAPE)
			{
				if (self->gesture_ != timeline_gesture::none)
				{
					self->cancel_gesture();
					return 1;
				}
				if (!self->selection_.empty())
				{
					self->clear_selection();
					return 1;
				}
			}
			return 0;
		}

		case VGUI_EVENT_CANCEL:
			if (self->gesture_ == timeline_gesture::none)
				return 0;
			self->cancel_gesture();
			return 1;

		case VGUI_EVENT_LEAVE:
			return 0;

		default:
			return 0;
		}
	}

	void timeline_view::on_focus_change(vgui_context* ctx,
		vgui_widget* old_focus, vgui_widget* new_focus, void* user)
	{
		(void)ctx;
		auto* self = static_cast<timeline_view*>(user);
		if (!self || !self->widget_)
			return;
		const bool was = old_focus == self->widget_;
		const bool now = new_focus == self->widget_;
		if (was == now)
			return;
		if (was && self->gesture_ != timeline_gesture::none)
			self->cancel_gesture();
	}

	/* --------------------------------------------------------------------- */
	/* Drawing                                                               */
	/* --------------------------------------------------------------------- */

	void timeline_view::on_draw(vgui_widget* widget, ovg_ctx_cb* cb,
		rvg_t* vg, float abs_x, float abs_y, float abs_w, float abs_h,
		float delta_time, void* user)
	{
		auto* self = static_cast<timeline_view*>(user);
		if (!self || !widget || !cb || !vg)
			return;
		self->draw(cb, vg, widget, abs_x + kBorderInset,
			abs_y + kBorderInset, abs_w - kBorderInset * 2.0f,
			abs_h - kBorderInset * 2.0f, delta_time);
	}

	void timeline_view::draw(ovg_ctx_cb* cb, rvg_t* vg, vgui_widget* widget,
		float x, float y, float width, float height, float delta_time)
	{
		if (width <= 0.0f || height <= 0.0f)
			return;

		vgui_theme theme = {};
		if (context_)
			vgui_context_get_theme(context_, &theme);

		const vgui_color panel_bg = theme_color(theme.panel_bg,
			0xFF101419u);
		const vgui_color text = theme_color(theme.text, 0xFFE8EEF8u);
		const vgui_color accent = theme_color(theme.accent, 0xFF409EFFu);
		const vgui_color accent_hover = theme_color(theme.accent_hover,
			0xFF74B6FFu);

		ovg_tools::fill_rect(cb, vg, x, y, width, height,
			to_ovg(panel_bg));

		const metrics m = layout_metrics();
		draw_lanes(cb, vg, widget, x, y, m);
		draw_keyframes(cb, vg, x, y, m);
		draw_ruler(cb, vg, widget, x, y, m);
		draw_transport(cb, vg, widget, x, y, m);

		const float scale = ui_scale();
		if (m.lanes_width > 0.0f)
		{
			const float px = x + m.lanes_x +
				(float)time_to_x(playhead_);
			if (px >= x + m.lanes_x - 8.0f * scale &&
				px <= x + m.lanes_x + m.lanes_width + 8.0f * scale)
			{
				ovg_tools::playhead(cb, vg, px, y + m.ruler_y,
					y + m.height, to_ovg(accent));
			}
		}

		if (gesture_ == timeline_gesture::marquee)
		{
			marquee_offset_ += std::max(0.0f, delta_time) * 24.0f * scale;
			ovg_tools::marquee(cb, vg, x + press_x_, y + press_y_,
				x + marquee_x_, y + marquee_y_,
				to_ovg(accent_hover), 1.0f, marquee_offset_);
		}

		(void)text;
	}

	void timeline_view::draw_lanes(ovg_ctx_cb* cb, rvg_t* vg,
		vgui_widget* widget, float x, float y, const metrics& m) const
	{
		if (m.lanes_width <= 0.0f || m.lanes_height <= 0.0f)
			return;

		vgui_theme theme = {};
		if (context_)
			vgui_context_get_theme(context_, &theme);
		const vgui_color text = theme_color(theme.text, 0xFFE8EEF8u);
		const vgui_color accent = theme_color(theme.accent, 0xFF409EFFu);
		const vgui_color panel_border = theme_color(theme.panel_border,
			0xFF2A3038u);
		const vgui_color lane0 = theme_color(theme.panel_bg, 0xFF141820u);
		const vgui_color lane1 = theme_color(theme.button_bg, 0xFF181D25u);
		const float scale = ui_scale();

		ovg_tools::fill_rect(cb, vg, x + m.lanes_x, y + m.lanes_y,
			m.lanes_width, m.lanes_height, to_ovg(lane0));

		for (std::size_t i = 0; i < tracks_.size(); ++i)
		{
			const float row_y = track_y(m, i);
			const float row_height = track_height_at(i);
			if (row_y + row_height < m.lanes_y ||
				row_y > m.height)
				continue;
			ovg_tools::track_lane(cb, vg, x + m.lanes_x, y + row_y,
				m.lanes_width, row_height, (int)i, to_ovg(lane0),
				to_ovg(lane1));
		}

		/* Time grid. */
		const double step = choose_time_step();
		const double end = x_to_time(m.lanes_width);
		double time = std::ceil(view_.view_start / step - 1e-9) * step;
		long long index = (long long)std::llround(time / step);
		const uint32_t minor = to_ovg(with_alpha(panel_border, 0x18));
		const uint32_t major = to_ovg(with_alpha(panel_border, 0x38));
		for (int guard = 0; guard < 4096 && time <= end + 1e-9;
			++guard, time += step, ++index)
		{
			const float px = x + m.lanes_x + (float)time_to_x(time);
			if (px < x + m.lanes_x - 1.0f ||
				px > x + m.lanes_x + m.lanes_width + 1.0f)
				continue;
			ovg_tools::line(cb, vg, px, y + m.lanes_y, px,
				y + m.lanes_y + m.lanes_height,
				((index % 5) + 5) % 5 == 0 ? major : minor, 1.0f,
				VG_LINE_CAP_BUTT);
		}

		for (std::size_t i = 0; i < tracks_.size(); ++i)
		{
			const float row_y = track_y(m, i);
			const float row_height = track_height_at(i);
			ovg_tools::line(cb, vg, x + m.lanes_x, y + row_y,
				x + m.lanes_x + m.lanes_width, y + row_y,
				to_ovg(with_alpha(panel_border, 0x55)), 1.0f,
				VG_LINE_CAP_BUTT);
			(void)row_height;
		}

		/* Header column. */
		ovg_tools::fill_rect(cb, vg, x, y + m.lanes_y, m.header_width,
			m.lanes_height, to_ovg(theme_color(theme.button_bg,
				0xFF181D25u)));
		for (std::size_t i = 0; i < tracks_.size(); ++i)
		{
			const timeline_track& track = tracks_[i];
			const float row_y = track_y(m, i);
			const float row_height = track_height_at(i);
			if (row_y + row_height < m.lanes_y || row_y > m.height)
				continue;
			const bool selected = track.id == selected_track_;
			if (selected)
			{
				ovg_tools::fill_rect(cb, vg, x, y + row_y,
					m.header_width, row_height,
					to_ovg(with_alpha(accent, 0x2E)));
				ovg_tools::fill_rect(cb, vg, x, y + row_y, 2.0f * scale,
					row_height, to_ovg(accent));
			}
			ovg_tools::fill_rect(cb, vg, x + 7.0f * scale,
				y + row_y + row_height * 0.5f - 4.0f * scale,
				8.0f * scale, 8.0f * scale,
				to_ovg(track.visible ? track.color
					: with_alpha(track.color, 0x60)));
			std::string label = track.name;
			if (track.locked)
				label += "  L";
			if (track.muted)
				label += "  M";
			const vgui_color row_text = track.visible
				? text : with_alpha(text, 0x80);
			vgui_custom_draw_text(widget, cb, vg, label.c_str(),
				x + 19.0f * scale, y + row_y,
				std::max(1.0f, m.header_width - 26.0f * scale),
				row_height, row_text, 0.0f, 0.5f);
		}

		if (tracks_.empty())
		{
			vgui_custom_draw_text(widget, cb, vg,
				"No tracks - add a timeline_track", x + m.lanes_x,
				y + m.lanes_y, m.lanes_width, m.lanes_height,
				with_alpha(text, 0x80), 0.5f, 0.5f);
		}
	}

	void timeline_view::draw_keyframes(ovg_ctx_cb* cb, rvg_t* vg, float x,
		float y, const metrics& m) const
	{
		if (m.lanes_width <= 0.0f || m.lanes_height <= 0.0f)
			return;

		vgui_theme theme = {};
		if (context_)
			vgui_context_get_theme(context_, &theme);
		const vgui_color accent_hover = theme_color(theme.accent_hover,
			0xFF74B6FFu);
		const vgui_color accent = theme_color(theme.accent, 0xFF409EFFu);
		const float scale = ui_scale();
		const float keyframe_size = kKeyframeSize * scale;
		const float lane_left = x + m.lanes_x;
		const float lane_right = lane_left + m.lanes_width;

		for (std::size_t i = 0; i < tracks_.size(); ++i)
		{
			const timeline_track& track = tracks_[i];
			if (!track.visible)
				continue;
			const float center_y = y + track_center_y(m, i);
			if (center_y < y + m.lanes_y - 8.0f * scale ||
				center_y > y + m.height + 8.0f * scale)
				continue;
			const vgui_color base = track.locked
				? with_alpha(track.color, 0x70) : track.color;
			const uint32_t link_color = to_ovg(with_alpha(
				track.muted ? with_alpha(track.color, 0x80)
					: accent, 0x66));
			const float* previous_x = nullptr;
			float last_x = 0.0f;
			for (const timeline_keyframe& keyframe : track.keyframes)
			{
				const float px = x + m.lanes_x +
					(float)time_to_x(keyframe.time);
				if (previous_x && !track.locked &&
					px > lane_left && last_x < lane_right)
				{
					/* Clip links to the lane area so a keyframe scrolled
					 * behind the track headers stays hidden. */
					const float link_start =
						std::clamp(last_x, lane_left, lane_right);
					const float link_end =
						std::clamp(px, lane_left, lane_right);
					if (link_end > link_start)
						ovg_tools::keyframe_link(cb, vg, link_start,
							center_y, link_end, center_y, link_color);
				}
				last_x = px;
				previous_x = &last_x;
				if (px < lane_left - keyframe_size ||
					px > lane_right + keyframe_size)
					continue;
				const bool selected = is_selected(keyframe.id);
				const ovg_tools::kf_state state = selected
					? ovg_tools::kf_state::selected
					: keyframe.interpolation == timeline_interpolation::step
						? ovg_tools::kf_state::hollow
						: ovg_tools::kf_state::solid;
				const uint32_t color = selected ? to_ovg(accent_hover)
					: to_ovg(base);
				ovg_tools::keyframe(cb, vg, px, center_y, keyframe_size,
					state, color, to_ovg(accent_hover));
			}
		}
	}

	void timeline_view::draw_ruler(ovg_ctx_cb* cb, rvg_t* vg,
		vgui_widget* widget, float x, float y, const metrics& m) const
	{
		vgui_theme theme = {};
		if (context_)
			vgui_context_get_theme(context_, &theme);
		const vgui_color text = theme_color(theme.text, 0xFFE8EEF8u);
		const vgui_color panel_border = theme_color(theme.panel_border,
			0xFF2A3038u);
		const float scale = ui_scale();

		ovg_tools::fill_rect(cb, vg, x + m.lanes_x, y + m.ruler_y,
			m.lanes_width, m.ruler_height, to_ovg(theme_color(
				theme.button_bg, 0xFF181D25u)));
		ovg_tools::fill_rect(cb, vg, x, y + m.ruler_y, m.header_width,
			m.ruler_height, to_ovg(theme_color(theme.panel_bg,
				0xFF141820u)));
		ovg_tools::line(cb, vg, x, y + m.lanes_y, x + m.width,
			y + m.lanes_y, to_ovg(panel_border), 1.0f,
			VG_LINE_CAP_BUTT);
		vgui_custom_draw_text(widget, cb, vg, "TRACKS", x + 8.0f * scale,
			y + m.ruler_y, std::max(1.0f, m.header_width - 16.0f * scale),
			m.ruler_height, with_alpha(text, 0xA0), 0.0f, 0.5f);

		if (m.lanes_width <= 0.0f)
			return;

		const double step = choose_time_step();
		const uint32_t minor = to_ovg(with_alpha(panel_border, 0xA0));
		const uint32_t major = to_ovg(with_alpha(text, 0xC0));
		ovg_tools::time_ruler(cb, vg, x + m.lanes_x, y + m.ruler_y,
			m.lanes_width, m.ruler_height, view_.view_start,
			x_to_time(m.lanes_width), step, minor, major, nullptr);

		const char* format = step >= 1.0 ? "%.0f"
			: step >= 0.1 ? "%.1f" : "%.2f";
		double time = std::ceil(view_.view_start / step - 1e-9) * step;
		long long index = (long long)std::llround(time / step);
		const double end = x_to_time(m.lanes_width);
		for (int guard = 0; guard < 4096 && time <= end + 1e-9;
			++guard, time += step, ++index)
		{
			if (((index % 5) + 5) % 5 != 0)
				continue;
			const float px = x + m.lanes_x + (float)time_to_x(time);
			if (px < x + m.lanes_x - 1.0f ||
				px > x + m.lanes_x + m.lanes_width - 4.0f * scale)
				continue;
			char label[48];
			std::snprintf(label, sizeof(label), format, time);
			vgui_custom_draw_text(widget, cb, vg, label, px + 2.0f * scale,
				y + m.ruler_y + 1.0f * scale, 52.0f * scale,
				std::max(1.0f, m.ruler_height - 2.0f * scale), text,
				0.0f, 0.0f);
		}
	}

	void timeline_view::draw_transport(ovg_ctx_cb* cb, rvg_t* vg,
		vgui_widget* widget, float x, float y, const metrics& m) const
	{
		vgui_theme theme = {};
		if (context_)
			vgui_context_get_theme(context_, &theme);
		const vgui_color text = theme_color(theme.text, 0xFFE8EEF8u);
		const vgui_color accent = theme_color(theme.accent, 0xFF409EFFu);
		const vgui_color button_bg = theme_color(theme.button_bg,
			0xFF262D38u);
		const vgui_color button_border = theme_color(theme.button_border,
			0xFF3B4656u);
		const vgui_color panel_border = theme_color(theme.panel_border,
			0xFF2A3038u);
		const float scale = ui_scale();

		ovg_tools::fill_rect(cb, vg, x, y + m.transport_y, m.width,
			m.transport_height, to_ovg(theme_color(theme.panel_bg,
				0xFF141820u)));
		ovg_tools::line(cb, vg, x, y + m.transport_y + m.transport_height,
			x + m.width, y + m.transport_y + m.transport_height,
			to_ovg(panel_border), 1.0f, VG_LINE_CAP_BUTT);

		static const float widths[5] = { 34.0f, 28.0f, 48.0f, 28.0f,
			40.0f };
		const char* labels[5] = { "Stop", "|<",
			playing_ ? "Pause" : "Play", ">|", "Loop" };
		for (int i = 0; i < 5; ++i)
		{
			const bool checked = i == 4 && loop_;
			const float bx = x + transport_button_x(i);
			const float by = y + m.transport_y + 4.0f * scale;
			const float bw = widths[i] * scale;
			const float bh = 18.0f * scale;
			ovg_tools::fill_round_rect(cb, vg, bx, by, bw, bh,
				4.0f * scale, to_ovg(checked ? with_alpha(accent, 0x50)
					: button_bg));
			ovg_tools::stroke_round_rect(cb, vg, bx, by, bw, bh,
				4.0f * scale, to_ovg(checked ? accent : button_border),
				1.0f);
			vgui_custom_draw_text(widget, cb, vg, labels[i], bx, by,
				bw, bh, checked ? accent : text, 0.5f, 0.5f);
		}

		if (m.width > 360.0f * scale)
		{
			char buffer[64];
			std::snprintf(buffer, sizeof(buffer), "%.3f / %.3f s",
				playhead_, duration_);
			vgui_custom_draw_text(widget, cb, vg, buffer,
				x + m.width - 190.0f * scale, y + m.transport_y,
				182.0f * scale, m.transport_height,
				with_alpha(text, 0xD0), 1.0f, 0.5f);
		}
	}

} /* namespace vgui_editor_kit */
