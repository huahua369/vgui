#include "canvas_view.h"

#include "pch.h"
#include "tools/tools.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace vgui_editor_kit {

	namespace {

		constexpr float kBorderInset = 1.0f;
		constexpr float kPi = 3.14159265358979323846f;
		constexpr float kTwoPi = 2.0f * kPi;
		constexpr float kDegreesPerRadian = 180.0f / kPi;
		constexpr float kRadiansPerDegree = kPi / 180.0f;
		constexpr float kEpsilon = 1e-4f;
		constexpr int kEllipseSegments = 40;

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

		bool near(float a, float b)
		{
			return std::fabs(a - b) <= kEpsilon;
		}

		bool same_rect(const canvas_rect& a, const canvas_rect& b)
		{
			return near(a.x, b.x) && near(a.y, b.y) &&
				near(a.width, b.width) && near(a.height, b.height);
		}

		float normalize_angle(float radians)
		{
			if (!std::isfinite(radians))
				return 0.0f;
			return std::remainder(radians, kTwoPi);
		}

		canvas_point rotate_point(canvas_point point, float radians)
		{
			const float c = std::cos(radians);
			const float s = std::sin(radians);
			return { point.x * c - point.y * s, point.x * s + point.y * c };
		}

		canvas_handle handle_from_index(int index)
		{
			static const canvas_handle handles[8] = {
				canvas_handle::north_west,
				canvas_handle::north,
				canvas_handle::north_east,
				canvas_handle::east,
				canvas_handle::south_east,
				canvas_handle::south,
				canvas_handle::south_west,
				canvas_handle::west
			};
			if (index < 0 || index >= 8)
				return canvas_handle::none;
			return handles[index];
		}

		class canvas_command final : public command
		{
		public:
			using apply_fn = std::function<void(
				const std::vector<canvas_item>&)>;

			canvas_command(std::string name, std::vector<canvas_item> before,
				std::vector<canvas_item> after, apply_fn apply)
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
					apply_(after_);
				return true;
			}

			void undo() override
			{
				if (apply_)
					apply_(before_);
			}

		private:
			std::string name_;
			std::vector<canvas_item> before_;
			std::vector<canvas_item> after_;
			apply_fn apply_;
		};

	} /* namespace */

	canvas_view::~canvas_view()
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

	void canvas_view::build(vgui_widget* parent)
	{
		if (widget_ || !parent)
			return;

		context_ = vgui_widget_get_context(parent);
		widget_ = vgui_widget_add(parent, VGUI_CUSTOM, nullptr);
		vgui_widget_set_id(widget_, "canvas_view");
		vgui_widget_set_grow(widget_, 1.0f);
		vgui_widget_set_min_size(widget_, 240, 120);
		vgui_widget_set_background(widget_, 0xFF101419);
		vgui_widget_set_border(widget_, 0xFF2A3038, 1);
		vgui_widget_set_focusable(widget_, 1);
		vgui_custom_set_draw(widget_, &canvas_view::on_draw, this);
		vgui_custom_set_event(widget_, &canvas_view::on_event, this);
		if (context_)
			focus_token_ = vgui_context_add_focus_listener(context_,
				&canvas_view::on_focus_change, this);
	}

	vgui_widget* canvas_view::widget() const noexcept
	{
		return widget_;
	}

	bool canvas_view::built() const noexcept
	{
		return widget_ != nullptr;
	}

	object_id canvas_view::add_item(canvas_item item)
	{
		if (item.id == invalid_object)
		{
			while (find_item(next_item_id_) != nullptr)
				++next_item_id_;
			item.id = next_item_id_++;
		}
		else
		{
			if (find_item(item.id) != nullptr)
				return invalid_object;
			if (item.id >= next_item_id_)
				next_item_id_ = item.id + 1;
		}
		if (!std::isfinite(item.bounds.width))
			item.bounds.width = 0.0f;
		if (!std::isfinite(item.bounds.height))
			item.bounds.height = 0.0f;
		const object_id id = item.id;
		items_.push_back(std::move(item));
		notify_change();
		return id;
	}

	bool canvas_view::update_item(const canvas_item& item)
	{
		if (item.id == invalid_object)
			return false;
		for (canvas_item& current : items_)
		{
			if (current.id != item.id)
				continue;
			current = item;
			notify_change();
			return true;
		}
		return false;
	}

	bool canvas_view::remove_item(object_id id)
	{
		for (auto it = items_.begin(); it != items_.end(); ++it)
		{
			if (it->id != id)
				continue;
			items_.erase(it);
			prune_selection();
			notify_change();
			return true;
		}
		return false;
	}

	void canvas_view::clear_items()
	{
		if (items_.empty())
			return;
		items_.clear();
		if (!selection_.empty() || primary_ != invalid_object)
		{
			selection_.clear();
			primary_ = invalid_object;
			notify_selection();
		}
		notify_change();
	}

	const std::vector<canvas_item>& canvas_view::items() const noexcept
	{
		return items_;
	}

	const canvas_item* canvas_view::find_item(object_id id) const noexcept
	{
		if (id == invalid_object)
			return nullptr;
		for (const canvas_item& item : items_)
			if (item.id == id)
				return &item;
		return nullptr;
	}

	canvas_item* canvas_view::find_item(object_id id) noexcept
	{
		if (id == invalid_object)
			return nullptr;
		for (canvas_item& item : items_)
			if (item.id == id)
				return &item;
		return nullptr;
	}

	void canvas_view::set_selection(const std::vector<object_id>& ids,
		object_id primary)
	{
		std::vector<object_id> filtered;
		filtered.reserve(ids.size());
		for (object_id id : ids)
		{
			if (id == invalid_object || find_item(id) == nullptr)
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

	void canvas_view::clear_selection()
	{
		set_selection({});
	}

	const std::vector<object_id>& canvas_view::selection_ids() const noexcept
	{
		return selection_;
	}

	object_id canvas_view::primary_selection() const noexcept
	{
		return primary_;
	}

	bool canvas_view::is_selected(object_id id) const noexcept
	{
		return std::find(selection_.begin(), selection_.end(), id) !=
			selection_.end();
	}

	void canvas_view::set_multi_select(bool enabled)
	{
		multi_select_ = enabled;
	}

	bool canvas_view::multi_select() const noexcept
	{
		return multi_select_;
	}

	void canvas_view::set_selection_listener(selection_listener listener)
	{
		selection_listener_ = std::move(listener);
	}

	void canvas_view::set_change_listener(change_listener listener)
	{
		change_listener_ = std::move(listener);
	}

	canvas_view_state canvas_view::view_state() const noexcept
	{
		return view_;
	}

	void canvas_view::set_view_state(const canvas_view_state& state)
	{
		if (gesture_ != canvas_gesture::none)
			cancel_gesture();
		canvas_view_state next = state;
		if (!std::isfinite(next.offset_x))
			next.offset_x = 0.0f;
		if (!std::isfinite(next.offset_y))
			next.offset_y = 0.0f;
		if (!std::isfinite(next.zoom) || next.zoom <= 0.0f)
			next.zoom = 1.0f;
		next.zoom = std::clamp(next.zoom, min_zoom_, max_zoom_);
		if (near(next.offset_x, view_.offset_x) &&
			near(next.offset_y, view_.offset_y) &&
			near(next.zoom, view_.zoom))
			return;
		view_ = next;
		notify_change();
	}

	float canvas_view::zoom() const noexcept
	{
		return view_.zoom;
	}

	void canvas_view::zoom_at(float content_x, float content_y, float factor)
	{
		if (!std::isfinite(factor) || factor <= 0.0f)
			return;
		if (gesture_ != canvas_gesture::none)
			cancel_gesture();
		const canvas_point anchor = screen_to_canvas(content_x, content_y);
		const float zoom = std::clamp(view_.zoom * factor, min_zoom_, max_zoom_);
		if (near(zoom, view_.zoom))
			return;
		view_.zoom = zoom;
		view_.offset_x = anchor.x - content_x / zoom;
		view_.offset_y = anchor.y - content_y / zoom;
		notify_change();
	}

	void canvas_view::set_zoom_limits(float minimum, float maximum)
	{
		min_zoom_ = std::max(0.001f, minimum);
		max_zoom_ = std::max(min_zoom_, maximum);
		canvas_view_state state = view_;
		state.zoom = std::clamp(state.zoom, min_zoom_, max_zoom_);
		if (!near(state.zoom, view_.zoom))
		{
			view_.zoom = state.zoom;
			notify_change();
		}
	}

	void canvas_view::reset_view()
	{
		set_view_state({});
	}

	void canvas_view::set_grid_visible(bool visible)
	{
		if (grid_visible_ == visible)
			return;
		grid_visible_ = visible;
		notify_change();
	}

	bool canvas_view::grid_visible() const noexcept
	{
		return grid_visible_;
	}

	void canvas_view::set_grid_step(float step)
	{
		step = std::max(0.0001f, step);
		if (near(grid_step_, step))
			return;
		grid_step_ = step;
		notify_change();
	}

	float canvas_view::grid_step() const noexcept
	{
		return grid_step_;
	}

	void canvas_view::set_grid_pixel_range(float minimum, float maximum)
	{
		const float minimum_pixels = std::max(1.0f, minimum);
		const float maximum_pixels = std::max(minimum_pixels, maximum);
		if (near(min_grid_pixels_, minimum_pixels) &&
			near(max_grid_pixels_, maximum_pixels))
			return;
		min_grid_pixels_ = minimum_pixels;
		max_grid_pixels_ = maximum_pixels;
		notify_change();
	}

	float canvas_view::effective_grid_step() const noexcept
	{
		float step = std::max(0.0001f, grid_step_);
		if (!(view_.zoom > 0.0f))
			return step;
		for (int i = 0; i < 64 && step * view_.zoom < min_grid_pixels_; ++i)
			step *= 2.0f;
		for (int i = 0; i < 64 && step * view_.zoom > max_grid_pixels_; ++i)
			step *= 0.5f;
		return step;
	}

	void canvas_view::set_snap_settings(const snap_settings* snap)
	{
		snap_ = snap;
	}

	void canvas_view::set_command_stack(command_stack* stack)
	{
		commands_ = stack;
	}

	command_stack* canvas_view::command_stack_ptr() const noexcept
	{
		return commands_;
	}

	canvas_point canvas_view::screen_to_canvas(float content_x,
		float content_y) const
	{
		const float zoom = view_.zoom > 0.0f ? view_.zoom : 1.0f;
		return { content_x / zoom + view_.offset_x,
			content_y / zoom + view_.offset_y };
	}

	canvas_point canvas_view::canvas_to_screen(float canvas_x,
		float canvas_y) const
	{
		return { (canvas_x - view_.offset_x) * view_.zoom,
			(canvas_y - view_.offset_y) * view_.zoom };
	}

	canvas_rect canvas_view::item_bounds(object_id id) const
	{
		const canvas_item* item = find_item(id);
		if (!item)
			return {};
		if (std::fabs(item->rotation) <= kEpsilon)
			return item->bounds;

		const float hw = item->bounds.width * 0.5f;
		const float hh = item->bounds.height * 0.5f;
		const canvas_point corners[4] = {
			item_point(*item, -hw, -hh),
			item_point(*item, hw, -hh),
			item_point(*item, hw, hh),
			item_point(*item, -hw, hh)
		};
		float min_x = corners[0].x, max_x = corners[0].x;
		float min_y = corners[0].y, max_y = corners[0].y;
		for (int i = 1; i < 4; ++i)
		{
			min_x = std::min(min_x, corners[i].x);
			max_x = std::max(max_x, corners[i].x);
			min_y = std::min(min_y, corners[i].y);
			max_y = std::max(max_y, corners[i].y);
		}
		return { min_x, min_y, max_x - min_x, max_y - min_y };
	}

	canvas_rect canvas_view::selection_bounds() const
	{
		canvas_rect result{};
		bool first = true;
		for (object_id id : selection_)
		{
			const canvas_rect bounds = item_bounds(id);
			if (bounds.empty())
				continue;
			if (first)
			{
				result = bounds;
				first = false;
			}
			else
			{
				const float min_x = std::min(result.x, bounds.x);
				const float min_y = std::min(result.y, bounds.y);
				const float max_x = std::max(result.right(), bounds.right());
				const float max_y = std::max(result.bottom(), bounds.bottom());
				result = { min_x, min_y, max_x - min_x, max_y - min_y };
			}
		}
		return result;
	}

	object_id canvas_view::hit_test(float content_x, float content_y) const
	{
		const canvas_point point = screen_to_canvas(content_x, content_y);
		for (auto it = items_.rbegin(); it != items_.rend(); ++it)
		{
			if (!it->visible || it->locked)
				continue;
			if (item_contains(*it, point))
				return it->id;
		}
		return invalid_object;
	}

	canvas_handle canvas_view::handle_at(float content_x,
		float content_y) const
	{
		if (selection_.size() != 1)
			return canvas_handle::none;
		const canvas_item* item = find_item(selection_.front());
		if (!item || !item->visible || item->locked)
			return canvas_handle::none;

		auto distance_to = [&](canvas_point canvas) {
			const canvas_point screen = canvas_to_screen(canvas.x, canvas.y);
			return std::hypot(screen.x - content_x, screen.y - content_y);
		};
		const float tolerance = handle_tolerance_ + handle_size_ * 0.5f;
		if (distance_to(rotate_handle_point(*item)) <= tolerance)
			return canvas_handle::rotate;
		for (int i = 0; i < 8; ++i)
		{
			const canvas_handle handle = handle_from_index(i);
			if (distance_to(item_handle_point(*item, handle)) <= tolerance)
				return handle;
		}
		return canvas_handle::none;
	}

	bool canvas_view::dragging() const noexcept
	{
		return gesture_ != canvas_gesture::none;
	}

	canvas_gesture canvas_view::gesture() const noexcept
	{
		return gesture_;
	}

	canvas_handle canvas_view::active_handle() const noexcept
	{
		return active_handle_;
	}

	canvas_handle canvas_view::hovered_handle() const noexcept
	{
		return hovered_handle_;
	}

	void canvas_view::cancel_gesture()
	{
		cancel_transform();
	}

	void canvas_view::set_drag_threshold(float pixels)
	{
		drag_threshold_ = pixels;
	}

	float canvas_view::drag_threshold() const noexcept
	{
		return drag_threshold_;
	}

	void canvas_view::set_min_item_size(float size)
	{
		min_item_size_ = std::max(0.5f, size);
	}

	void canvas_view::set_enabled(bool enabled)
	{
		if (enabled_ == enabled)
			return;
		if (!enabled)
		{
			cancel_gesture();
			hovered_handle_ = canvas_handle::none;
		}
		enabled_ = enabled;
	}

	bool canvas_view::enabled() const noexcept
	{
		return enabled_;
	}

	void canvas_view::request_focus()
	{
		if (context_ && widget_)
			vgui_set_focus(context_, widget_);
	}

	bool canvas_view::focused() const
	{
		return context_ && widget_ && vgui_get_focus(context_) == widget_;
	}

	bool canvas_view::content_geometry(float* offset_x, float* offset_y,
		float* width, float* height) const
	{
		if (!widget_)
		{
			if (offset_x)
				*offset_x = kBorderInset;
			if (offset_y)
				*offset_y = kBorderInset;
			if (width)
				*width = 0.0f;
			if (height)
				*height = 0.0f;
			return false;
		}
		float wx = 0.0f, wy = 0.0f, ww = 0.0f, wh = 0.0f;
		vgui_widget_get_rect(widget_, &wx, &wy, &ww, &wh);
		if (offset_x)
			*offset_x = kBorderInset;
		if (offset_y)
			*offset_y = kBorderInset;
		if (width)
			*width = std::max(0.0f, ww - kBorderInset * 2.0f);
		if (height)
			*height = std::max(0.0f, wh - kBorderInset * 2.0f);
		return ww > kBorderInset * 2.0f && wh > kBorderInset * 2.0f;
	}

	float canvas_view::effective_threshold() const
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

	void canvas_view::notify_change()
	{
		if (change_listener_)
			change_listener_();
	}

	void canvas_view::notify_selection()
	{
		if (selection_listener_)
			selection_listener_(selection_);
	}

	void canvas_view::prune_selection()
	{
		std::vector<object_id> filtered;
		filtered.reserve(selection_.size());
		for (object_id id : selection_)
			if (find_item(id) != nullptr)
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

	canvas_point canvas_view::item_point(const canvas_item& item, float local_x,
		float local_y) const
	{
		const canvas_point center = {
			item.bounds.x + item.bounds.width * 0.5f,
			item.bounds.y + item.bounds.height * 0.5f
		};
		const canvas_point rotated = rotate_point({ local_x, local_y },
			item.rotation);
		return { center.x + rotated.x, center.y + rotated.y };
	}

	canvas_point canvas_view::item_handle_point(const canvas_item& item,
		canvas_handle handle) const
	{
		const float hw = item.bounds.width * 0.5f;
		const float hh = item.bounds.height * 0.5f;
		switch (handle)
		{
		case canvas_handle::north_west: return item_point(item, -hw, -hh);
		case canvas_handle::north: return item_point(item, 0.0f, -hh);
		case canvas_handle::north_east: return item_point(item, hw, -hh);
		case canvas_handle::east: return item_point(item, hw, 0.0f);
		case canvas_handle::south_east: return item_point(item, hw, hh);
		case canvas_handle::south: return item_point(item, 0.0f, hh);
		case canvas_handle::south_west: return item_point(item, -hw, hh);
		case canvas_handle::west: return item_point(item, -hw, 0.0f);
		default: return item_point(item, 0.0f, 0.0f);
		}
	}

	canvas_point canvas_view::rotate_handle_point(
		const canvas_item& item) const
	{
		const float zoom = view_.zoom > 0.0f ? view_.zoom : 1.0f;
		return item_point(item, 0.0f,
			-item.bounds.height * 0.5f - rotate_handle_distance_ / zoom);
	}

	bool canvas_view::item_contains(const canvas_item& item,
		canvas_point point) const
	{
		const canvas_point center = {
			item.bounds.x + item.bounds.width * 0.5f,
			item.bounds.y + item.bounds.height * 0.5f
		};
		const float hw = item.bounds.width * 0.5f;
		const float hh = item.bounds.height * 0.5f;
		if (hw <= 0.0f || hh <= 0.0f)
			return false;
		const canvas_point local = rotate_point(
			{ point.x - center.x, point.y - center.y }, -item.rotation);
		switch (item.shape)
		{
		case canvas_shape::ellipse:
			return (local.x * local.x) / (hw * hw) +
				(local.y * local.y) / (hh * hh) <= 1.0f;
		case canvas_shape::diamond:
			return std::fabs(local.x) / hw + std::fabs(local.y) / hh <= 1.0f;
		case canvas_shape::rectangle:
		default:
			return std::fabs(local.x) <= hw && std::fabs(local.y) <= hh;
		}
	}

	void canvas_view::apply_selection(const std::vector<object_id>& picked,
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
				else if (find_item(id) != nullptr)
					next.push_back(id);
			}
			primary = primary_;
		}
		else if (shift)
		{
			next = selection_;
			for (object_id id : picked)
			{
				if (find_item(id) != nullptr &&
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

	std::vector<object_id> canvas_view::items_in_rect(canvas_rect rect) const
	{
		std::vector<object_id> result;
		if (rect.empty())
			return result;
		for (const canvas_item& item : items_)
		{
			if (!item.visible || item.locked)
				continue;
			const canvas_rect bounds = item_bounds(item.id);
			if (bounds.empty())
				continue;
			if (bounds.x < rect.right() && bounds.right() > rect.x &&
				bounds.y < rect.bottom() && bounds.bottom() > rect.y)
				result.push_back(item.id);
		}
		return result;
	}

	void canvas_view::capture_gesture_items()
	{
		gesture_start_.clear();
		for (object_id id : selection_)
		{
			const canvas_item* item = find_item(id);
			if (item && item->visible && !item->locked)
				gesture_start_.push_back(*item);
		}
	}

	void canvas_view::begin_transform(canvas_handle handle,
		canvas_point point)
	{
		if (handle == canvas_handle::none)
			return;
		capture_gesture_items();
		if (gesture_start_.empty())
			return;
		const canvas_item& item = gesture_start_.front();
		const canvas_point center = {
			item.bounds.x + item.bounds.width * 0.5f,
			item.bounds.y + item.bounds.height * 0.5f
		};
		const canvas_point canvas = screen_to_canvas(point.x, point.y);
		rotate_start_angle_ = std::atan2(canvas.y - center.y,
			canvas.x - center.x);
		active_handle_ = handle;
		if (handle == canvas_handle::rotate)
			gesture_ = canvas_gesture::rotate;
		else
			gesture_ = canvas_gesture::resize;
		press_x_ = point.x;
		press_y_ = point.y;
	}

	float canvas_view::snap_translation(float value) const
	{
		if (!snap_ || !snap_->enabled || !snap_->translate ||
			!(snap_->translate_step > 0.0f))
			return value;
		return snap_->snap_translation(value);
	}

	void canvas_view::update_move(canvas_point point)
	{
		if (gesture_start_.empty())
			return;
		const canvas_point start = screen_to_canvas(press_x_, press_y_);
		const canvas_point current = screen_to_canvas(point.x, point.y);
		float dx = current.x - start.x;
		float dy = current.y - start.y;

		const canvas_item* primary = nullptr;
		for (const canvas_item& item : gesture_start_)
			if (item.id == primary_)
			{
				primary = &item;
				break;
			}
		if (!primary)
			primary = &gesture_start_.front();

		if (snap_ && snap_->enabled && snap_->translate &&
			snap_->translate_step > 0.0f)
		{
			const float target_x = snap_translation(primary->bounds.x + dx);
			const float target_y = snap_translation(primary->bounds.y + dy);
			dx = target_x - primary->bounds.x;
			dy = target_y - primary->bounds.y;
		}

		bool changed = false;
		for (const canvas_item& start_item : gesture_start_)
		{
			canvas_item* item = find_item(start_item.id);
			if (!item)
				continue;
			const canvas_rect bounds = {
				start_item.bounds.x + dx,
				start_item.bounds.y + dy,
				start_item.bounds.width,
				start_item.bounds.height
			};
			if (!same_rect(item->bounds, bounds))
			{
				item->bounds = bounds;
				changed = true;
			}
		}
		if (changed)
			notify_change();
	}

	void canvas_view::update_resize(canvas_point point)
	{
		if (gesture_start_.empty())
			return;
		const canvas_item& start_item = gesture_start_.front();
		canvas_item* item = find_item(start_item.id);
		if (!item)
			return;

		const canvas_handle handle = active_handle_;
		const bool left = handle == canvas_handle::north_west ||
			handle == canvas_handle::west ||
			handle == canvas_handle::south_west;
		const bool right = handle == canvas_handle::north_east ||
			handle == canvas_handle::east ||
			handle == canvas_handle::south_east;
		const bool top = handle == canvas_handle::north_west ||
			handle == canvas_handle::north ||
			handle == canvas_handle::north_east;
		const bool bottom = handle == canvas_handle::south_west ||
			handle == canvas_handle::south ||
			handle == canvas_handle::south_east;
		if (!left && !right && !top && !bottom)
			return;

		const float w0 = start_item.bounds.width;
		const float h0 = start_item.bounds.height;
		const canvas_point center0 = {
			start_item.bounds.x + w0 * 0.5f,
			start_item.bounds.y + h0 * 0.5f
		};
		const float fixed_x = left ? w0 * 0.5f : right ? -w0 * 0.5f : 0.0f;
		const float fixed_y = top ? h0 * 0.5f : bottom ? -h0 * 0.5f : 0.0f;
		const canvas_point anchor = item_point(start_item, fixed_x, fixed_y);
		canvas_point pointer = screen_to_canvas(point.x, point.y);

		const bool snap_active = snap_ && snap_->enabled &&
			snap_->translate && snap_->translate_step > 0.0f;
		const bool unrotated = std::fabs(start_item.rotation) <= kEpsilon;
		if (snap_active && unrotated)
		{
			/* Snap the moving edge to the document grid.  The fixed edge is
			 * left untouched even when it does not lie on the grid. */
			if (left || right)
				pointer.x = snap_translation(pointer.x);
			if (top || bottom)
				pointer.y = snap_translation(pointer.y);
		}

		const canvas_point offset = rotate_point(
			{ pointer.x - anchor.x, pointer.y - anchor.y },
			-start_item.rotation);
		float width = w0;
		float height = h0;
		if (left || right)
			width = std::max(min_item_size_, left ? -offset.x : offset.x);
		if (top || bottom)
			height = std::max(min_item_size_, top ? -offset.y : offset.y);

		if (snap_active && !unrotated)
		{
			if (left || right)
				width = std::max(min_item_size_, snap_translation(width));
			if (top || bottom)
				height = std::max(min_item_size_, snap_translation(height));
		}

		const float sign_x = right ? 1.0f : left ? -1.0f : 0.0f;
		const float sign_y = bottom ? 1.0f : top ? -1.0f : 0.0f;
		const canvas_point local_center = {
			fixed_x + sign_x * width * 0.5f,
			fixed_y + sign_y * height * 0.5f
		};
		const canvas_point center_offset = rotate_point(
			{ local_center.x - fixed_x, local_center.y - fixed_y },
			start_item.rotation);
		const canvas_point center = { anchor.x + center_offset.x,
			anchor.y + center_offset.y };
		const canvas_rect bounds = { center.x - width * 0.5f,
			center.y - height * 0.5f, width, height };
		if (!same_rect(item->bounds, bounds))
		{
			item->bounds = bounds;
			notify_change();
		}
	}

	void canvas_view::update_rotate(canvas_point point)
	{
		if (gesture_start_.empty())
			return;
		const canvas_item& start_item = gesture_start_.front();
		canvas_item* item = find_item(start_item.id);
		if (!item)
			return;

		const canvas_point center = {
			start_item.bounds.x + start_item.bounds.width * 0.5f,
			start_item.bounds.y + start_item.bounds.height * 0.5f
		};
		const canvas_point canvas = screen_to_canvas(point.x, point.y);
		const float angle = std::atan2(canvas.y - center.y,
			canvas.x - center.x);
		float delta = normalize_angle(angle - rotate_start_angle_);
		float target = start_item.rotation + delta;
		if (snap_ && snap_->enabled && snap_->rotate)
			target = snap_->snap_rotation_degrees(
				target * kDegreesPerRadian) * kRadiansPerDegree;
		target = normalize_angle(target);
		if (!near(item->rotation, target))
		{
			item->rotation = target;
			notify_change();
		}
	}

	void canvas_view::finish_transform(canvas_gesture kind,
		canvas_point point)
	{
		if (kind == canvas_gesture::move)
			update_move(point);
		else if (kind == canvas_gesture::resize)
			update_resize(point);
		else if (kind == canvas_gesture::rotate)
			update_rotate(point);

		if (gesture_changed())
		{
			std::vector<canvas_item> after;
			after.reserve(gesture_start_.size());
			for (const canvas_item& before : gesture_start_)
			{
				const canvas_item* item = find_item(before.id);
				if (item)
					after.push_back(*item);
			}
			const char* name = kind == canvas_gesture::resize ? "Resize"
				: kind == canvas_gesture::rotate ? "Rotate" : "Move";
			push_gesture_command(name, gesture_start_, after);
		}
		reset_gesture();
	}

	void canvas_view::cancel_transform()
	{
		if (gesture_ == canvas_gesture::move ||
			gesture_ == canvas_gesture::resize ||
			gesture_ == canvas_gesture::rotate)
		{
			if (!gesture_start_.empty())
			{
				apply_item_states(gesture_start_);
				notify_change();
			}
		}
		else if (gesture_ == canvas_gesture::pan)
		{
			if (!near(gesture_view_.offset_x, view_.offset_x) ||
				!near(gesture_view_.offset_y, view_.offset_y))
			{
				view_ = gesture_view_;
				notify_change();
			}
		}
		reset_gesture();
	}

	void canvas_view::reset_gesture()
	{
		gesture_ = canvas_gesture::none;
		active_handle_ = canvas_handle::none;
		gesture_start_.clear();
		rotate_start_angle_ = 0.0f;
	}

	bool canvas_view::gesture_changed() const
	{
		for (const canvas_item& before : gesture_start_)
		{
			const canvas_item* item = find_item(before.id);
			if (!item)
				return true;
			if (!same_rect(before.bounds, item->bounds) ||
				!near(before.rotation, item->rotation))
				return true;
		}
		return false;
	}

	void canvas_view::apply_item_states(
		const std::vector<canvas_item>& states)
	{
		bool changed = false;
		for (const canvas_item& state : states)
		{
			canvas_item* item = find_item(state.id);
			if (!item)
				continue;
			if (!same_rect(item->bounds, state.bounds))
			{
				item->bounds = state.bounds;
				changed = true;
			}
			if (!near(item->rotation, state.rotation))
			{
				item->rotation = state.rotation;
				changed = true;
			}
		}
		if (changed)
			notify_change();
	}

	void canvas_view::push_gesture_command(const char* name,
		const std::vector<canvas_item>& before,
		const std::vector<canvas_item>& after)
	{
		if (!commands_ || before.empty() || after.empty())
			return;
		std::weak_ptr<int> alive = alive_token_;
		auto apply = [this, alive](const std::vector<canvas_item>& states) {
			if (!alive.expired())
				apply_item_states(states);
		};
		commands_->push(std::make_unique<canvas_command>(
			name ? name : "Canvas", before, after, std::move(apply)));
	}

	int canvas_view::on_event(vgui_widget* widget, const vgui_event* event,
		void* user)
	{
		auto* self = static_cast<canvas_view*>(user);
		if (!self || !event || !widget || !self->enabled_)
			return 0;

		float ox = 0.0f, oy = 0.0f, cw = 0.0f, ch = 0.0f;
		self->content_geometry(&ox, &oy, &cw, &ch);
		/* Custom-widget mouse events are local to the widget; strip the 1px
		 * border so the canvas works in content coordinates. */
		const float x = event->x - ox;
		const float y = event->y - oy;
		const bool inside = x >= 0.0f && y >= 0.0f &&
			x < cw && y < ch;

		switch (event->type)
		{
		case VGUI_EVENT_MOUSE_DOWN:
			if (self->gesture_ != canvas_gesture::none)
				return 0;
			if (event->button == 2 && inside)
			{
				self->gesture_ = canvas_gesture::pan;
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
				const canvas_handle handle = self->handle_at(x, y);
				if (handle != canvas_handle::none)
				{
					self->begin_transform(handle, { x, y });
					return self->gesture_ != canvas_gesture::none ? 1 : 0;
				}

				const object_id hit = self->hit_test(x, y);
				if (hit != invalid_object)
				{
					const int mods = self->context_
						? vgui_context_get_mods(self->context_) : 0;
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
							primary = next.empty()
								? invalid_object : next.front();
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
						self->capture_gesture_items();
						if (!self->gesture_start_.empty())
						{
							self->gesture_ = canvas_gesture::pending_move;
							self->press_x_ = x;
							self->press_y_ = y;
						}
					}
					return 1;
				}

				self->gesture_ = canvas_gesture::pending_marquee;
				self->press_x_ = x;
				self->press_y_ = y;
				self->marquee_x_ = x;
				self->marquee_y_ = y;
				return 1;
			}

		case VGUI_EVENT_MOUSE_MOVE:
			if (self->gesture_ == canvas_gesture::pan)
			{
				const float zoom = self->view_.zoom > 0.0f
					? self->view_.zoom : 1.0f;
				self->view_.offset_x = self->gesture_view_.offset_x -
					(x - self->press_x_) / zoom;
				self->view_.offset_y = self->gesture_view_.offset_y -
					(y - self->press_y_) / zoom;
				self->notify_change();
				return 1;
			}
			if (self->gesture_ == canvas_gesture::pending_move ||
				self->gesture_ == canvas_gesture::pending_marquee)
			{
				const float threshold = self->effective_threshold();
				if (std::fabs(x - self->press_x_) > threshold ||
					std::fabs(y - self->press_y_) > threshold)
				{
					self->gesture_ =
						self->gesture_ == canvas_gesture::pending_move
							? canvas_gesture::move
							: canvas_gesture::marquee;
				}
				else
				{
					return 1;
				}
			}
			if (self->gesture_ == canvas_gesture::move)
			{
				self->update_move({ x, y });
				return 1;
			}
			if (self->gesture_ == canvas_gesture::resize)
			{
				self->update_resize({ x, y });
				return 1;
			}
			if (self->gesture_ == canvas_gesture::rotate)
			{
				self->update_rotate({ x, y });
				return 1;
			}
			if (self->gesture_ == canvas_gesture::marquee)
			{
				self->marquee_x_ = x;
				self->marquee_y_ = y;
				return 1;
			}
			self->hovered_handle_ = inside
				? self->handle_at(x, y) : canvas_handle::none;
			return self->hovered_handle_ != canvas_handle::none ? 1 : 0;

		case VGUI_EVENT_MOUSE_UP:
			if (event->button == 2 && self->gesture_ == canvas_gesture::pan)
			{
				self->reset_gesture();
				self->hovered_handle_ = inside
					? self->handle_at(x, y) : canvas_handle::none;
				return 1;
			}
			if (event->button != 1)
				return 0;
			if (self->gesture_ == canvas_gesture::pending_move)
			{
				self->reset_gesture();
				return 1;
			}
			if (self->gesture_ == canvas_gesture::move ||
				self->gesture_ == canvas_gesture::resize ||
				self->gesture_ == canvas_gesture::rotate)
			{
				const canvas_gesture kind = self->gesture_;
				self->finish_transform(kind, { x, y });
				self->hovered_handle_ = inside
					? self->handle_at(x, y) : canvas_handle::none;
				return 1;
			}
			if (self->gesture_ == canvas_gesture::pending_marquee ||
				self->gesture_ == canvas_gesture::marquee)
			{
				const bool box = self->gesture_ == canvas_gesture::marquee;
				const int mods = self->context_
					? vgui_context_get_mods(self->context_) : 0;
				if (box)
				{
					const canvas_point a = self->screen_to_canvas(
						self->press_x_, self->press_y_);
					const canvas_point b = self->screen_to_canvas(x, y);
					const canvas_rect rect = {
						std::min(a.x, b.x), std::min(a.y, b.y),
						std::fabs(b.x - a.x), std::fabs(b.y - a.y)
					};
					self->apply_selection(self->items_in_rect(rect), mods);
				}
				else
				{
					self->apply_selection({}, mods);
				}
				self->reset_gesture();
				return 1;
			}
			return 0;

		case VGUI_EVENT_MOUSE_WHEEL:
			if (!inside || event->wheel_y == 0.0f ||
				self->gesture_ != canvas_gesture::none)
				return 0;
			self->zoom_at(x, y, std::exp(event->wheel_y * 0.15f));
			return 1;

		case VGUI_EVENT_KEY_DOWN:
			if (event->key != VGUI_KEY_ESCAPE ||
				self->gesture_ == canvas_gesture::none)
				return 0;
			self->cancel_gesture();
			return 1;

		case VGUI_EVENT_CANCEL:
			if (self->gesture_ == canvas_gesture::none)
				return 0;
			self->cancel_gesture();
			return 1;

		case VGUI_EVENT_LEAVE:
			if (self->gesture_ == canvas_gesture::none)
				self->hovered_handle_ = canvas_handle::none;
			return 0;

		default:
			return 0;
		}
	}

	void canvas_view::on_focus_change(vgui_context* ctx, vgui_widget* old_focus,
		vgui_widget* new_focus, void* user)
	{
		(void)ctx;
		auto* self = static_cast<canvas_view*>(user);
		if (!self || !self->widget_)
			return;
		const bool was = old_focus == self->widget_;
		const bool now = new_focus == self->widget_;
		if (was == now)
			return;
		if (was && self->gesture_ != canvas_gesture::none)
			self->cancel_gesture();
	}

	void canvas_view::on_draw(vgui_widget* widget, ovg_ctx_cb* cb, rvg_t* vg,
		float abs_x, float abs_y, float abs_w, float abs_h,
		float delta_time, void* user)
	{
		auto* self = static_cast<canvas_view*>(user);
		if (!self || !widget || !cb || !vg)
			return;
		self->draw(cb, vg, widget, abs_x + kBorderInset,
			abs_y + kBorderInset, abs_w - kBorderInset * 2.0f,
			abs_h - kBorderInset * 2.0f, delta_time);
	}

	void canvas_view::draw(ovg_ctx_cb* cb, rvg_t* vg, vgui_widget* widget,
		float x, float y, float width, float height, float delta_time)
	{
		if (width <= 0.0f || height <= 0.0f)
			return;

		vgui_theme theme = {};
		if (context_)
			vgui_context_get_theme(context_, &theme);
		vgui_color grid_base = theme.panel_border
			? theme.panel_border : 0xFF2A3038u;
		const uint32_t minor = to_ovg(with_alpha(grid_base, 0x2E));
		const uint32_t major = to_ovg(with_alpha(grid_base, 0x66));

		if (grid_visible_)
			draw_grid(cb, vg, x, y, width, height, minor, major);

		for (const canvas_item& item : items_)
		{
			if (item.visible)
				draw_item(cb, vg, widget, item, x, y);
		}

		const vgui_color accent_color = theme.accent
			? theme.accent : 0xFF409EFFu;
		const vgui_color hover_color = theme.accent_hover
			? theme.accent_hover : 0xFF74B6FFu;
		draw_selection(cb, vg, x, y, to_ovg(accent_color),
			to_ovg(hover_color));

		if (gesture_ == canvas_gesture::marquee)
		{
			marquee_offset_ += std::max(0.0f, delta_time) * 24.0f;
			ovg_tools::marquee(cb, vg, x + press_x_, y + press_y_,
				x + marquee_x_, y + marquee_y_,
				to_ovg(hover_color), 1.0f, marquee_offset_);
		}
	}

	void canvas_view::draw_grid(ovg_ctx_cb* cb, rvg_t* vg, float x, float y,
		float width, float height, uint32_t minor, uint32_t major) const
	{
		const float zoom = view_.zoom > 0.0f ? view_.zoom : 1.0f;
		const float step = effective_grid_step();
		const float spacing = step * zoom;
		if (spacing < 3.0f || !std::isfinite(spacing))
			return;

		const int first_x = (int)std::ceil(view_.offset_x / step);
		const int last_x = (int)std::floor(
			(view_.offset_x + width / zoom) / step);
		const int first_y = (int)std::ceil(view_.offset_y / step);
		const int last_y = (int)std::floor(
			(view_.offset_y + height / zoom) / step);
		const int count_x = std::min(2048, last_x - first_x + 1);
		const int count_y = std::min(2048, last_y - first_y + 1);
		for (int i = 0; i < count_x; ++i)
		{
			const int index = first_x + i;
			const float sx = ((float)index * step - view_.offset_x) * zoom;
			const uint32_t color = index % 5 == 0 ? major : minor;
			ovg_tools::line(cb, vg, x + sx, y, x + sx, y + height, color,
				1.0f, VG_LINE_CAP_BUTT);
		}
		for (int i = 0; i < count_y; ++i)
		{
			const int index = first_y + i;
			const float sy = ((float)index * step - view_.offset_y) * zoom;
			const uint32_t color = index % 5 == 0 ? major : minor;
			ovg_tools::line(cb, vg, x, y + sy, x + width, y + sy, color,
				1.0f, VG_LINE_CAP_BUTT);
		}

		const canvas_point origin = canvas_to_screen(0.0f, 0.0f);
		if (origin.x >= -40.0f && origin.x <= width + 40.0f &&
			origin.y >= -40.0f && origin.y <= height + 40.0f)
			ovg_tools::origin_axes(cb, vg, x + origin.x, y + origin.y, 30.0f);
	}

	void canvas_view::draw_item(ovg_ctx_cb* cb, rvg_t* vg,
		vgui_widget* widget, const canvas_item& item, float x, float y) const
	{
		auto add_point = [&](std::vector<glm::vec2>& points, float local_x,
			float local_y)
		{
			const canvas_point canvas = item_point(item, local_x, local_y);
			const canvas_point screen = canvas_to_screen(canvas.x, canvas.y);
			points.push_back({ x + screen.x, y + screen.y });
		};

		const float hw = item.bounds.width * 0.5f;
		const float hh = item.bounds.height * 0.5f;
		const bool unrotated = std::fabs(item.rotation) <= kEpsilon;
		if (item.shape == canvas_shape::rectangle && unrotated &&
			item.corner_radius > 0.0f)
		{
			const canvas_point screen = canvas_to_screen(item.bounds.x,
				item.bounds.y);
			const float w = item.bounds.width * view_.zoom;
			const float h = item.bounds.height * view_.zoom;
			const float radius = item.corner_radius * view_.zoom;
			if ((item.fill >> 24) != 0)
				ovg_tools::fill_round_rect(cb, vg, x + screen.x,
					y + screen.y, w, h, radius, to_ovg(item.fill));
			if ((item.border >> 24) != 0 && item.border_width > 0.0f)
				ovg_tools::stroke_round_rect(cb, vg, x + screen.x,
					y + screen.y, w, h, radius, to_ovg(item.border),
					std::max(1.0f, item.border_width * view_.zoom));
		}
		else
		{
			std::vector<glm::vec2> points;
			points.reserve(kEllipseSegments);
			if (item.shape == canvas_shape::ellipse)
			{
				for (int i = 0; i < kEllipseSegments; ++i)
				{
					const float angle = kTwoPi * (float)i /
						(float)kEllipseSegments;
					add_point(points, std::cos(angle) * hw,
						std::sin(angle) * hh);
				}
			}
			else if (item.shape == canvas_shape::diamond)
			{
				add_point(points, 0.0f, -hh);
				add_point(points, hw, 0.0f);
				add_point(points, 0.0f, hh);
				add_point(points, -hw, 0.0f);
			}
			else
			{
				add_point(points, -hw, -hh);
				add_point(points, hw, -hh);
				add_point(points, hw, hh);
				add_point(points, -hw, hh);
			}
			if ((item.fill >> 24) != 0)
				ovg_tools::fill_polygon(cb, vg, points.data(),
					(int)points.size(), to_ovg(item.fill));
			if ((item.border >> 24) != 0 && item.border_width > 0.0f)
				ovg_tools::stroke_polyline(cb, vg, points.data(),
					(int)points.size(), to_ovg(item.border),
					std::max(1.0f, item.border_width * view_.zoom),
					VG_LINE_CAP_ROUND, VG_LINE_JOIN_ROUND, true);
		}

		if (!item.label.empty())
		{
			const canvas_rect bounds = item_bounds(item.id);
			const canvas_point top_left = canvas_to_screen(bounds.x, bounds.y);
			const float w = std::max(1.0f, bounds.width * view_.zoom);
			const float h = std::max(1.0f, bounds.height * view_.zoom);
			vgui_custom_draw_text(widget, cb, vg, item.label.c_str(),
				x + top_left.x, y + top_left.y, w, h, item.text_color,
				0.5f, 0.5f);
		}
	}

	void canvas_view::draw_selection(ovg_ctx_cb* cb, rvg_t* vg, float x,
		float y, uint32_t accent, uint32_t accent_hover) const
	{
		if (selection_.empty())
			return;

		for (object_id id : selection_)
		{
			const canvas_item* item = find_item(id);
			if (!item || !item->visible)
				continue;
			const uint32_t color = item->locked
				? 0x8098A0B0u
				: (id == primary_ ? accent_hover : accent);
			const float hw = item->bounds.width * 0.5f;
			const float hh = item->bounds.height * 0.5f;
			glm::vec2 points[4] = {};
			const float local[4][2] = {
				{ -hw, -hh }, { hw, -hh }, { hw, hh }, { -hw, hh }
			};
			for (int i = 0; i < 4; ++i)
			{
				const canvas_point canvas = item_point(*item, local[i][0],
					local[i][1]);
				const canvas_point screen = canvas_to_screen(canvas.x,
					canvas.y);
				points[i] = { x + screen.x, y + screen.y };
			}
			ovg_tools::stroke_polyline(cb, vg, points, 4, 0xA0000000u, 3.0f,
				VG_LINE_CAP_ROUND, VG_LINE_JOIN_ROUND, true);
			ovg_tools::stroke_polyline(cb, vg, points, 4, color, 1.5f,
				VG_LINE_CAP_ROUND, VG_LINE_JOIN_ROUND, true);
		}

		if (selection_.size() != 1)
		{
			const canvas_rect bounds = selection_bounds();
			if (!bounds.empty())
			{
				const canvas_point a = canvas_to_screen(bounds.x, bounds.y);
				const canvas_point b = canvas_to_screen(bounds.right(),
					bounds.bottom());
				ovg_tools::marquee(cb, vg, x + a.x, y + a.y, x + b.x,
					y + b.y, accent, 1.0f, marquee_offset_);
			}
			return;
		}

		const canvas_item* item = find_item(selection_.front());
		if (!item || !item->visible || item->locked)
			return;
		const uint32_t color = accent_hover;
		for (int i = 0; i < 8; ++i)
		{
			const canvas_handle handle = handle_from_index(i);
			const canvas_point canvas = item_handle_point(*item, handle);
			const canvas_point screen = canvas_to_screen(canvas.x, canvas.y);
			const bool hot = handle == hovered_handle_ ||
				handle == active_handle_;
			const float size = hot ? handle_size_ + 2.0f : handle_size_;
			ovg_tools::selection_handle(cb, vg, x + screen.x, y + screen.y,
				size, hot ? color : 0xFFF2F6FCu, color);
		}

		const canvas_point top = item_handle_point(*item,
			canvas_handle::north);
		const canvas_point handle = rotate_handle_point(*item);
		const canvas_point top_screen = canvas_to_screen(top.x, top.y);
		const canvas_point handle_screen = canvas_to_screen(handle.x,
			handle.y);
		ovg_tools::line(cb, vg, x + top_screen.x, y + top_screen.y,
			x + handle_screen.x, y + handle_screen.y, 0xA0000000u, 3.0f,
			VG_LINE_CAP_ROUND);
		ovg_tools::line(cb, vg, x + top_screen.x, y + top_screen.y,
			x + handle_screen.x, y + handle_screen.y, color, 1.5f,
			VG_LINE_CAP_ROUND);
		const float radius = hovered_handle_ == canvas_handle::rotate ||
			active_handle_ == canvas_handle::rotate ? 5.0f : 4.0f;
		ovg_tools::fill_circle(cb, vg, x + handle_screen.x,
			y + handle_screen.y, radius, 0xFF141820u);
		ovg_tools::stroke_circle(cb, vg, x + handle_screen.x,
			y + handle_screen.y, radius, color, 1.5f);
	}

} /* namespace vgui_editor_kit */
