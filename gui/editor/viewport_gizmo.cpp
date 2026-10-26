#include "viewport_gizmo.h"

#include "pch.h"
#include "tools/tools.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace vgui_editor_kit {

	namespace {

		constexpr float kEpsilon = 1e-6f;
		constexpr float kDefaultScreenLength = 80.0f;
		constexpr float kPlaneLengthRatio = 0.32f;

		/* OVG native 0xAABBGGRR.  X/Y/Z use the usual red/green/blue axes. */
		constexpr uint32_t kAxisColors[3] = {
			ovg_tools::rgba(230, 82, 74),
			ovg_tools::rgba(84, 202, 112),
			ovg_tools::rgba(76, 134, 234)
		};

		bool finite(float value)
		{
			return std::isfinite(value);
		}

		bool finite_vec(const camera_vec3& value)
		{
			return finite(value.x) && finite(value.y) && finite(value.z);
		}

		float dot(const camera_vec3& a, const camera_vec3& b)
		{
			return a.x * b.x + a.y * b.y + a.z * b.z;
		}

		camera_vec3 add(const camera_vec3& a, const camera_vec3& b)
		{
			return { a.x + b.x, a.y + b.y, a.z + b.z };
		}

		camera_vec3 sub(const camera_vec3& a, const camera_vec3& b)
		{
			return { a.x - b.x, a.y - b.y, a.z - b.z };
		}

		camera_vec3 scale(const camera_vec3& value, float factor)
		{
			return { value.x * factor, value.y * factor, value.z * factor };
		}

		float length(const camera_vec3& value)
		{
			return std::sqrt(dot(value, value));
		}

		bool normalize(camera_vec3* value)
		{
			if (!value)
				return false;
			const float len = length(*value);
			if (!(len > kEpsilon) || !finite(len))
				return false;
			value->x /= len;
			value->y /= len;
			value->z /= len;
			return true;
		}

		int axis_index(gizmo_handle handle)
		{
			switch (handle)
			{
			case gizmo_handle::axis_x: return 0;
			case gizmo_handle::axis_y: return 1;
			case gizmo_handle::axis_z: return 2;
			default: return -1;
			}
		}

		/* Plane handles are named by their two in-plane axes; the normal index is
		 * the remaining axis. */
		int plane_normal_index(gizmo_handle handle)
		{
			switch (handle)
			{
			case gizmo_handle::plane_yz: return 0;
			case gizmo_handle::plane_zx: return 1;
			case gizmo_handle::plane_xy: return 2;
			default: return -1;
			}
		}

		gizmo_handle axis_handle(int index)
		{
			switch (index)
			{
			case 0: return gizmo_handle::axis_x;
			case 1: return gizmo_handle::axis_y;
			case 2: return gizmo_handle::axis_z;
			default: return gizmo_handle::none;
			}
		}

		gizmo_handle plane_handle(int normal_index)
		{
			switch (normal_index)
			{
			case 0: return gizmo_handle::plane_yz;
			case 1: return gizmo_handle::plane_zx;
			case 2: return gizmo_handle::plane_xy;
			default: return gizmo_handle::none;
			}
		}

		void plane_axes(int normal_index, int* first, int* second)
		{
			*first = (normal_index + 1) % 3;
			*second = (normal_index + 2) % 3;
		}

		float point_segment_distance(float px, float py, float ax, float ay,
			float bx, float by)
		{
			const float vx = bx - ax;
			const float vy = by - ay;
			const float length_sq = vx * vx + vy * vy;
			if (length_sq <= kEpsilon)
				return std::hypot(px - ax, py - ay);
			float t = ((px - ax) * vx + (py - ay) * vy) / length_sq;
			t = std::clamp(t, 0.0f, 1.0f);
			return std::hypot(px - (ax + vx * t), py - (ay + vy * t));
		}

		bool point_in_quad(float px, float py, const float* xs, const float* ys)
		{
			bool positive = false;
			bool negative = false;
			for (int i = 0; i < 4; ++i)
			{
				const int j = (i + 1) & 3;
				const float ex = xs[j] - xs[i];
				const float ey = ys[j] - ys[i];
				const float cross = ex * (py - ys[i]) - ey * (px - xs[i]);
				if (cross > 1e-4f)
					positive = true;
				else if (cross < -1e-4f)
					negative = true;
				if (positive && negative)
					return false;
			}
			return true;
		}

		/* Closest point on an infinite axis line to a ray.  Returns the axis
		 * parameter (origin + axis * s) and false when they are parallel. */
		bool axis_parameter(const gizmo_ray& ray, const camera_vec3& origin,
			const camera_vec3& axis, float* out)
		{
			const camera_vec3 w0 = sub(origin, ray.origin);
			const float bb = dot(ray.direction, ray.direction);
			const float cc = dot(axis, axis);
			const float bc = dot(ray.direction, axis);
			const float wb = dot(ray.direction, w0);
			const float wc = dot(axis, w0);
			const float denom = bb * cc - bc * bc;
			if (std::fabs(denom) <= 1e-5f)
				return false;
			*out = (bc * wb - bb * wc) / denom;
			return finite(*out);
		}

		bool ray_plane(const gizmo_ray& ray, const camera_vec3& origin,
			const camera_vec3& normal, camera_vec3* out)
		{
			const float denom = dot(ray.direction, normal);
			if (std::fabs(denom) <= kEpsilon)
				return false;
			const float t = dot(sub(origin, ray.origin), normal) / denom;
			if (!finite(t))
				return false;
			*out = add(ray.origin, scale(ray.direction, t));
			return finite_vec(*out);
		}

	} /* namespace */

	viewport_gizmo::~viewport_gizmo()
	{
		detach();
	}

	void viewport_gizmo::attach(viewport_view& viewport)
	{
		detach();
		viewport_ = &viewport;
		listener_token_ = viewport.add_event_listener(
			[this](const vgui_event* event) { return on_input(event); });
		if (!listener_token_)
		{
			viewport_ = nullptr;
			return;
		}
		viewport.set_foreground_overlay(
			[this](ovg_ctx_cb* cb, rvg_t* vg, float x, float y,
				float width, float height, float delta_time)
			{
				draw(cb, vg, x, y, width, height, delta_time);
			});
	}

	void viewport_gizmo::detach()
	{
		cancel_drag();
		if (viewport_ && listener_token_)
			viewport_->remove_event_listener(listener_token_);
		if (viewport_)
			viewport_->set_foreground_overlay(nullptr);
		viewport_ = nullptr;
		listener_token_ = 0;
		hovered_ = gizmo_handle::none;
	}

	bool viewport_gizmo::attached() const noexcept
	{
		return viewport_ != nullptr;
	}

	void viewport_gizmo::set_frame_provider(frame_provider provider)
	{
		frame_provider_ = std::move(provider);
		hovered_ = gizmo_handle::none;
	}

	void viewport_gizmo::set_projection(project_fn projection)
	{
		projection_ = std::move(projection);
	}

	void viewport_gizmo::set_ray_provider(ray_provider provider)
	{
		ray_provider_ = std::move(provider);
	}

	void viewport_gizmo::set_drag_handler(drag_handler handler)
	{
		drag_handler_ = std::move(handler);
	}

	void viewport_gizmo::set_snap_settings(const snap_settings* snap)
	{
		snap_ = snap;
	}

	void viewport_gizmo::set_visible(bool visible)
	{
		if (visible_ == visible)
			return;
		if (!visible)
			cancel_drag();
		visible_ = visible;
		if (!visible)
			hovered_ = gizmo_handle::none;
	}

	bool viewport_gizmo::visible() const noexcept
	{
		return visible_;
	}

	void viewport_gizmo::set_space(gizmo_space space)
	{
		if (space_ == space)
			return;
		/* Axes change under the drag, so the host cannot keep applying the old
		 * constrained delta. */
		cancel_drag();
		space_ = space;
		hovered_ = gizmo_handle::none;
	}

	gizmo_space viewport_gizmo::space() const noexcept
	{
		return space_;
	}

	void viewport_gizmo::set_screen_length(float pixels)
	{
		screen_length_ = std::max(0.0f, pixels);
	}

	float viewport_gizmo::screen_length() const noexcept
	{
		return screen_length_;
	}

	void viewport_gizmo::set_handle_tolerance(float pixels)
	{
		handle_tolerance_ = std::max(2.0f, pixels);
	}

	void viewport_gizmo::set_axis_width(float pixels)
	{
		axis_width_ = std::max(0.5f, pixels);
	}

	bool viewport_gizmo::dragging() const noexcept
	{
		return dragging_;
	}

	gizmo_handle viewport_gizmo::active_handle() const noexcept
	{
		return dragging_ ? active_ : gizmo_handle::none;
	}

	gizmo_handle viewport_gizmo::hovered_handle() const noexcept
	{
		return hovered_;
	}

	void viewport_gizmo::cancel_drag()
	{
		if (!dragging_)
			return;
		const camera_vec3 delta = last_delta_;
		dragging_ = false;
		active_ = gizmo_handle::none;
		last_delta_ = {};
		if (drag_handler_)
			drag_handler_(gizmo_phase::cancel, delta);
	}

	void viewport_gizmo::reset_hover()
	{
		hovered_ = gizmo_handle::none;
	}

	void viewport_gizmo::content_geometry(float* offset_x, float* offset_y,
		int* width, int* height) const
	{
		float ox = 1.0f, oy = 1.0f, cw = 0.0f, ch = 0.0f;
		if (viewport_)
			viewport_->get_content_rect(&ox, &oy, &cw, &ch);
		if (offset_x)
			*offset_x = ox;
		if (offset_y)
			*offset_y = oy;
		if (width)
			*width = std::max(1, (int)std::lround(cw));
		if (height)
			*height = std::max(1, (int)std::lround(ch));
	}

	bool viewport_gizmo::query_frame(gizmo_frame* out) const
	{
		if (!out || !frame_provider_)
			return false;
		gizmo_frame frame = frame_provider_();
		if (!frame.valid || !finite_vec(frame.origin))
			return false;
		for (int i = 0; i < 3; ++i)
		{
			if (normalize(&frame.axis[i]))
				continue;
			/* Degenerate host data should not erase the whole gizmo. */
			frame.axis[i] = { 0.0f, 0.0f, 0.0f };
			if (i == 0)
				frame.axis[i].x = 1.0f;
			else if (i == 1)
				frame.axis[i].y = 1.0f;
			else
				frame.axis[i].z = 1.0f;
		}
		*out = frame;
		return true;
	}

	bool viewport_gizmo::build_geometry(const gizmo_frame& frame, int width,
		int height, screen_geometry* out) const
	{
		if (!out || !projection_ || width <= 0 || height <= 0)
			return false;
		*out = screen_geometry{};

		float ox = 0.0f, oy = 0.0f;
		if (!projection_(frame.origin, &ox, &oy))
			return false;
		if (!finite(ox) || !finite(oy))
			return false;

		float requested = frame.screen_length > 0.0f
			? frame.screen_length : screen_length_;
		if (!(requested > 0.0f))
			requested = kDefaultScreenLength;
		const float minimum_side = (float)std::min(width, height);
		const float len = std::min(requested, minimum_side * 0.42f);
		if (len < 18.0f)
			return false;

		out->ox = ox;
		out->oy = oy;
		out->length = len;

		/* One world unit can be sub-pixel at large camera distances; step the
		 * sample outwards until the projected direction is stable. */
		for (int i = 0; i < 3; ++i)
		{
			float world_scale = 1.0f;
			for (int attempt = 0; attempt < 4; ++attempt)
			{
				float px = 0.0f, py = 0.0f;
				if (projection_(add(frame.origin, scale(frame.axis[i],
					world_scale)), &px, &py))
				{
					const float dx = px - ox;
					const float dy = py - oy;
					const float distance = std::hypot(dx, dy);
					if (distance >= 2.0f && finite(distance))
					{
						screen_axis& axis = out->axis[i];
						axis.valid = true;
						axis.dx = dx / distance;
						axis.dy = dy / distance;
						axis.ex = ox + axis.dx * len;
						axis.ey = oy + axis.dy * len;
						break;
					}
				}
				world_scale *= 10.0f;
			}
		}

		const float plane_len = len * kPlaneLengthRatio;
		for (int normal = 0; normal < 3; ++normal)
		{
			int first = 0, second = 0;
			plane_axes(normal, &first, &second);
			const screen_axis& a = out->axis[first];
			const screen_axis& b = out->axis[second];
			if (!a.valid || !b.valid)
				continue;

			screen_plane& plane = out->plane[normal];
			plane.valid = true;
			plane.x[0] = ox;
			plane.y[0] = oy;
			plane.x[1] = ox + a.dx * plane_len;
			plane.y[1] = oy + a.dy * plane_len;
			plane.x[2] = plane.x[1] + b.dx * plane_len;
			plane.y[2] = plane.y[1] + b.dy * plane_len;
			plane.x[3] = ox + b.dx * plane_len;
			plane.y[3] = oy + b.dy * plane_len;
		}

		out->valid = out->axis[0].valid || out->axis[1].valid ||
			out->axis[2].valid;
		return out->valid;
	}

	gizmo_handle viewport_gizmo::hit_test(float x, float y, int width,
		int height) const
	{
		gizmo_frame frame;
		if (!visible_ || !query_frame(&frame))
			return gizmo_handle::none;
		screen_geometry geometry;
		if (!build_geometry(frame, width, height, &geometry))
			return gizmo_handle::none;

		float best_distance = std::numeric_limits<float>::max();
		int best_axis = -1;
		for (int i = 0; i < 3; ++i)
		{
			const screen_axis& axis = geometry.axis[i];
			if (!axis.valid)
				continue;
			const float distance = point_segment_distance(x, y, geometry.ox,
				geometry.oy, axis.ex, axis.ey);
			if (distance <= handle_tolerance_ && distance < best_distance)
			{
				best_distance = distance;
				best_axis = i;
			}
		}
		if (best_axis >= 0)
			return axis_handle(best_axis);

		for (int normal = 0; normal < 3; ++normal)
		{
			const screen_plane& plane = geometry.plane[normal];
			if (plane.valid && point_in_quad(x, y, plane.x, plane.y))
				return plane_handle(normal);
		}
		return gizmo_handle::none;
	}

	bool viewport_gizmo::begin_drag(gizmo_handle handle, float x, float y,
		int width, int height)
	{
		if (!ray_provider_)
			return false;
		gizmo_frame frame;
		if (!query_frame(&frame))
			return false;
		/* Use the same geometry the hit test saw; a frame that suddenly shrank
		 * below the handle threshold should not start a drag. */
		screen_geometry geometry;
		if (!build_geometry(frame, width, height, &geometry))
			return false;

		gizmo_ray ray;
		if (!ray_provider_(x, y, &ray))
			return false;
		if (!finite_vec(ray.origin) || !normalize(&ray.direction))
			return false;

		const int axis = axis_index(handle);
		const int normal = plane_normal_index(handle);
		if (axis >= 0)
		{
			if (!axis_parameter(ray, frame.origin, frame.axis[axis],
				&axis_start_))
				return false;
		}
		else if (normal >= 0)
		{
			if (!ray_plane(ray, frame.origin, frame.axis[normal],
				&plane_start_))
				return false;
		}
		else
		{
			return false;
		}

		drag_frame_ = frame;
		active_ = handle;
		hovered_ = handle;
		dragging_ = true;
		last_delta_ = {};
		if (drag_handler_)
			drag_handler_(gizmo_phase::begin, {});
		return true;
	}

	float viewport_gizmo::snap_scalar(float value) const
	{
		if (!snap_ || !snap_->enabled || !snap_->translate)
			return value;
		return snap_->snap_translation(value);
	}

	bool viewport_gizmo::compute_delta(float x, float y, camera_vec3* delta) const
	{
		if (!delta || !ray_provider_ || !dragging_)
			return false;
		gizmo_ray ray;
		if (!ray_provider_(x, y, &ray))
			return false;
		if (!finite_vec(ray.origin) || !normalize(&ray.direction))
			return false;

		const int axis = axis_index(active_);
		const int normal = plane_normal_index(active_);
		if (axis >= 0)
		{
			float parameter = 0.0f;
			if (!axis_parameter(ray, drag_frame_.origin, drag_frame_.axis[axis],
				&parameter))
				return false;
			float amount = snap_scalar(parameter - axis_start_);
			*delta = scale(drag_frame_.axis[axis], amount);
			return true;
		}
		if (normal < 0)
			return false;

		camera_vec3 hit;
		if (!ray_plane(ray, drag_frame_.origin, drag_frame_.axis[normal], &hit))
			return false;
		int first = 0, second = 0;
		plane_axes(normal, &first, &second);
		const camera_vec3 offset = sub(hit, plane_start_);
		const float u = snap_scalar(dot(offset, drag_frame_.axis[first]));
		const float v = snap_scalar(dot(offset, drag_frame_.axis[second]));
		*delta = add(scale(drag_frame_.axis[first], u),
			scale(drag_frame_.axis[second], v));
		return true;
	}

	void viewport_gizmo::update_drag(float x, float y)
	{
		camera_vec3 delta = last_delta_;
		if (compute_delta(x, y, &delta))
			last_delta_ = delta;
		if (drag_handler_)
			drag_handler_(gizmo_phase::preview, last_delta_);
	}

	void viewport_gizmo::finish_drag(bool commit, float x, float y)
	{
		if (!dragging_)
			return;
		camera_vec3 delta = last_delta_;
		if (commit && !compute_delta(x, y, &delta))
			delta = last_delta_;
		dragging_ = false;
		active_ = gizmo_handle::none;
		hovered_ = gizmo_handle::none;
		last_delta_ = {};
		if (drag_handler_)
			drag_handler_(commit ? gizmo_phase::commit : gizmo_phase::cancel,
				delta);
	}

	int viewport_gizmo::on_input(const vgui_event* event)
	{
		if (!event || !viewport_ || !visible_)
			return 0;

		float ox = 0.0f, oy = 0.0f;
		int cw = 0, ch = 0;
		content_geometry(&ox, &oy, &cw, &ch);
		const float x = event->x - ox;
		const float y = event->y - oy;
		const bool inside = x >= 0.0f && y >= 0.0f &&
			x < (float)cw && y < (float)ch;

		switch (event->type)
		{
		case VGUI_EVENT_MOUSE_DOWN:
			if (event->button != 1 || dragging_ || !inside)
				return 0;
			hovered_ = hit_test(x, y, cw, ch);
			if (hovered_ == gizmo_handle::none)
				return 0;
			if (!begin_drag(hovered_, x, y, cw, ch))
			{
				hovered_ = gizmo_handle::none;
				return 0;
			}
			return 1;

		case VGUI_EVENT_MOUSE_MOVE:
			if (dragging_)
			{
				update_drag(x, y);
				return 1;
			}
			hovered_ = inside ? hit_test(x, y, cw, ch) : gizmo_handle::none;
			return hovered_ != gizmo_handle::none ? 1 : 0;

		case VGUI_EVENT_MOUSE_UP:
			if (event->button != 1 || !dragging_)
				return 0;
			finish_drag(true, x, y);
			return 1;

		case VGUI_EVENT_CANCEL:
			if (!dragging_)
				return 0;
			cancel_drag();
			return 1;

		case VGUI_EVENT_KEY_DOWN:
			if (event->key != VGUI_KEY_ESCAPE || !dragging_)
				return 0;
			cancel_drag();
			return 1;

		case VGUI_EVENT_LEAVE:
			if (!dragging_)
				hovered_ = gizmo_handle::none;
			return 0;

		default:
			return 0;
		}
	}

	void viewport_gizmo::draw(ovg_ctx_cb* cb, rvg_t* vg, float x, float y,
		float width, float height, float delta_time)
	{
		(void)delta_time;
		if (!visible_ || !cb || !vg)
			return;
		gizmo_frame frame;
		if (!query_frame(&frame))
			return;
		screen_geometry geometry;
		if (!build_geometry(frame, (int)std::lround(width),
			(int)std::lround(height), &geometry))
			return;

		for (int normal = 0; normal < 3; ++normal)
		{
			const screen_plane& plane = geometry.plane[normal];
			if (!plane.valid)
				continue;
			const gizmo_handle handle = plane_handle(normal);
			const bool highlighted = handle == hovered_ ||
				(dragging_ && handle == active_);
			uint32_t color = kAxisColors[normal];
			if (dragging_ && !highlighted)
				color = ovg_tools::with_alpha(color, 60);
			else
				color = ovg_tools::with_alpha(color, highlighted ? 112 : 44);

			glm::vec2 points[4] = {
				{ x + plane.x[0], y + plane.y[0] },
				{ x + plane.x[1], y + plane.y[1] },
				{ x + plane.x[2], y + plane.y[2] },
				{ x + plane.x[3], y + plane.y[3] }
			};
			ovg_tools::fill_polygon(cb, vg, points, 4, color);
			if (highlighted)
			{
				ovg_tools::stroke_polyline(cb, vg, points, 4,
					0xFFFFFFFFu, 1.0f, VG_LINE_CAP_ROUND,
					VG_LINE_JOIN_ROUND, true);
			}
		}

		for (int i = 0; i < 3; ++i)
		{
			const screen_axis& axis = geometry.axis[i];
			if (!axis.valid)
				continue;
			const gizmo_handle handle = axis_handle(i);
			const bool highlighted = handle == hovered_ ||
				(dragging_ && handle == active_);
			uint32_t color = kAxisColors[i];
			float line_width = axis_width_;
			if (highlighted)
			{
				color = ovg_tools::lerp_color(color, 0xFFFFFFFFu, 0.4f);
				line_width += 1.0f;
			}
			else if (dragging_)
			{
				color = ovg_tools::with_alpha(color, 80);
			}
			ovg_tools::arrow(cb, vg, x + geometry.ox, y + geometry.oy,
				x + axis.ex, y + axis.ey, 0xB4000000u, line_width + 2.5f,
				11.0f);
			ovg_tools::arrow(cb, vg, x + geometry.ox, y + geometry.oy,
				x + axis.ex, y + axis.ey, color, line_width, 9.0f);
		}

		/* Non-interactive pivot marker keeps the origin readable over busy
		 * scenes without competing with the axis hit boxes. */
		ovg_tools::fill_circle(cb, vg, x + geometry.ox, y + geometry.oy,
			3.5f, ovg_tools::rgba(18, 20, 24, 220));
		ovg_tools::stroke_circle(cb, vg, x + geometry.ox, y + geometry.oy,
			3.5f, ovg_tools::rgba(220, 226, 236, 240), 1.0f);
	}

} /* namespace vgui_editor_kit */
