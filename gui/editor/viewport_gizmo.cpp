#include <vgui/editor/viewport_gizmo.h>

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
		constexpr float kPi = 3.14159265358979323846f;
		constexpr float kDegreesPerRadian = 180.0f / kPi;

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

		int rotation_axis_index(gizmo_handle handle)
		{
			switch (handle)
			{
			case gizmo_handle::rotate_x: return 0;
			case gizmo_handle::rotate_y: return 1;
			case gizmo_handle::rotate_z: return 2;
			default: return -1;
			}
		}

		int scale_axis_index(gizmo_handle handle)
		{
			switch (handle)
			{
			case gizmo_handle::scale_x: return 0;
			case gizmo_handle::scale_y: return 1;
			case gizmo_handle::scale_z: return 2;
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

		gizmo_handle rotation_handle(int index)
		{
			switch (index)
			{
			case 0: return gizmo_handle::rotate_x;
			case 1: return gizmo_handle::rotate_y;
			case 2: return gizmo_handle::rotate_z;
			default: return gizmo_handle::none;
			}
		}

		gizmo_handle scale_handle(int index)
		{
			switch (index)
			{
			case 0: return gizmo_handle::scale_x;
			case 1: return gizmo_handle::scale_y;
			case 2: return gizmo_handle::scale_z;
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

		/* Angle of a point in a ring plane.  The two in-plane axes are ordered
		 * so u x v = normal, i.e. a positive angle turns around +normal. */
		bool ring_angle(const camera_vec3& point, const gizmo_frame& frame,
			int normal_index, float* out)
		{
			int first = 0, second = 0;
			plane_axes(normal_index, &first, &second);
			const camera_vec3 offset = sub(point, frame.origin);
			const float u = dot(offset, frame.axis[first]);
			const float v = dot(offset, frame.axis[second]);
			if (std::hypot(u, v) <= 1e-5f)
				return false;
			*out = std::atan2(v, u);
			return finite(*out);
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

	void viewport_gizmo::set_transform_handler(transform_handler handler)
	{
		transform_handler_ = std::move(handler);
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

	void viewport_gizmo::set_mode(gizmo_mode mode)
	{
		if (mode_ == mode)
			return;
		/* Handles and drag math change with the mode, so an in-flight drag
		 * cannot continue. */
		cancel_drag();
		mode_ = mode;
		hovered_ = gizmo_handle::none;
	}

	gizmo_mode viewport_gizmo::mode() const noexcept
	{
		return mode_;
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
		const gizmo_transform transform = last_transform_;
		dragging_ = false;
		active_ = gizmo_handle::none;
		last_transform_ = {};
		emit(gizmo_phase::cancel, transform);
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

		/* Rotation rings: estimate the world radius whose projection is about
		 * `len` pixels, then sample the circle.  The two in-plane axes are used
		 * for the estimate so an edge-on ring still gets a useful radius. */
		for (int normal = 0; normal < 3; ++normal)
		{
			int first = 0, second = 0;
			plane_axes(normal, &first, &second);

			float world_radius = 1.0f;
			for (int attempt = 0; attempt < 6; ++attempt)
			{
				float extent = 0.0f;
				for (int which = 0; which < 2; ++which)
				{
					const camera_vec3& direction = frame.axis[
						which == 0 ? first : second];
					float px = 0.0f, py = 0.0f;
					if (!projection_(add(frame.origin,
						scale(direction, world_radius)), &px, &py))
						continue;
					extent = std::max(extent, std::hypot(px - ox, py - oy));
				}
				if (!finite(extent) || !(world_radius > 0.0f))
					break;
				if (extent >= len - 0.25f && extent <= len + 0.25f)
					break;
				if (extent < 1e-3f)
				{
					world_radius *= 10.0f;
					continue;
				}
				world_radius *= len / extent;
			}
			if (!(world_radius > 0.0f) || !finite(world_radius) ||
				world_radius > 1e12f)
				continue;

			screen_ring& ring = out->ring[normal];
			float max_distance = 0.0f;
			bool valid = true;
			for (int segment = 0; segment < kRingSegments; ++segment)
			{
				const float angle = (2.0f * kPi * (float)segment) /
					(float)kRingSegments;
				const camera_vec3 point = add(frame.origin,
					add(scale(frame.axis[first], std::cos(angle) *
						world_radius),
						scale(frame.axis[second], std::sin(angle) *
							world_radius)));
				float px = 0.0f, py = 0.0f;
				if (!projection_(point, &px, &py))
				{
					valid = false;
					break;
				}
				ring.x[segment] = px;
				ring.y[segment] = py;
				max_distance = std::max(max_distance,
					std::hypot(px - ox, py - oy));
			}
			ring.valid = valid && finite(max_distance) &&
				max_distance >= 12.0f;
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

		if (mode_ == gizmo_mode::rotate)
		{
			float best_distance = std::numeric_limits<float>::max();
			int best_ring = -1;
			for (int normal = 0; normal < 3; ++normal)
			{
				const screen_ring& ring = geometry.ring[normal];
				if (!ring.valid)
					continue;
				for (int segment = 0; segment < kRingSegments; ++segment)
				{
					const int next = (segment + 1) % kRingSegments;
					const float distance = point_segment_distance(x, y,
						ring.x[segment], ring.y[segment], ring.x[next],
						ring.y[next]);
					if (distance <= handle_tolerance_ &&
						distance < best_distance)
					{
						best_distance = distance;
						best_ring = normal;
					}
				}
			}
			if (best_ring >= 0)
				return rotation_handle(best_ring);
			return gizmo_handle::none;
		}

		if (mode_ == gizmo_mode::scale)
		{
			/* The centre handle wins over the axis roots. */
			if (std::hypot(x - geometry.ox, y - geometry.oy) <=
				handle_tolerance_ + 1.0f)
				return gizmo_handle::scale_uniform;

			float best_distance = std::numeric_limits<float>::max();
			int best_axis = -1;
			for (int i = 0; i < 3; ++i)
			{
				const screen_axis& axis = geometry.axis[i];
				if (!axis.valid)
					continue;
				const float distance = point_segment_distance(x, y,
					geometry.ox, geometry.oy, axis.ex, axis.ey);
				if (distance <= handle_tolerance_ && distance < best_distance)
				{
					best_distance = distance;
					best_axis = i;
				}
			}
			if (best_axis >= 0)
				return scale_handle(best_axis);
			return gizmo_handle::none;
		}

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
		const bool valid_handle =
			(mode_ == gizmo_mode::translate &&
				(axis_index(handle) >= 0 || plane_normal_index(handle) >= 0)) ||
			(mode_ == gizmo_mode::rotate && rotation_axis_index(handle) >= 0) ||
			(mode_ == gizmo_mode::scale &&
				(scale_axis_index(handle) >= 0 ||
					handle == gizmo_handle::scale_uniform));
		if (!valid_handle)
			return false;

		gizmo_frame frame;
		if (!query_frame(&frame))
			return false;
		/* Use the same geometry the hit test saw; a frame that suddenly shrank
		 * below the handle threshold should not start a drag. */
		screen_geometry geometry;
		if (!build_geometry(frame, width, height, &geometry))
			return false;

		if (mode_ != gizmo_mode::scale && !ray_provider_)
			return false;

		gizmo_ray ray;
		const bool needs_ray = mode_ != gizmo_mode::scale;
		if (needs_ray)
		{
			if (!ray_provider_(x, y, &ray))
				return false;
			if (!finite_vec(ray.origin) || !normalize(&ray.direction))
				return false;
		}

		drag_frame_ = frame;
		drag_geometry_ = geometry;
		drag_transform_ = {};
		drag_transform_.mode = mode_;
		drag_transform_.handle = handle;
		drag_transform_.origin = frame.origin;
		for (int i = 0; i < 3; ++i)
			drag_transform_.axis[i] = frame.axis[i];

		const int axis = axis_index(handle);
		const int normal = plane_normal_index(handle);
		const int rotation = rotation_axis_index(handle);
		const int scale_axis = scale_axis_index(handle);
		drag_start_x_ = x;
		drag_start_y_ = y;
		drag_start_distance_ = std::hypot(x - geometry.ox, y - geometry.oy);

		if (mode_ == gizmo_mode::translate)
		{
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
		}
		else if (mode_ == gizmo_mode::rotate)
		{
			camera_vec3 hit;
			if (!ray_plane(ray, frame.origin, frame.axis[rotation], &hit))
				return false;
			if (!ring_angle(hit, frame, rotation, &ring_start_angle_))
				return false;
			ring_last_angle_ = 0.0f;
		}
		else
		{
			(void)scale_axis;
			/* Scale derives its factor from screen movement; the stored
			 * geometry and start point are all it needs. */
		}

		active_ = handle;
		hovered_ = handle;
		dragging_ = true;
		last_transform_ = drag_transform_;
		emit(gizmo_phase::begin, last_transform_);
		return true;
	}

	float viewport_gizmo::snap_scalar(float value) const
	{
		if (!snap_ || !snap_->enabled || !snap_->translate)
			return value;
		return snap_->snap_translation(value);
	}

	float viewport_gizmo::snap_rotation(float radians) const
	{
		if (!snap_)
			return radians;
		return snap_->snap_rotation_degrees(radians * kDegreesPerRadian) /
			kDegreesPerRadian;
	}

	float viewport_gizmo::snap_scale_value(float value) const
	{
		if (!snap_)
			return value;
		return snap_->snap_scale(value);
	}

	bool viewport_gizmo::compute_transform(float x, float y,
		gizmo_transform* out)
	{
		if (!out || !dragging_)
			return false;
		*out = drag_transform_;

		if (mode_ == gizmo_mode::translate)
		{
			if (!ray_provider_)
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
				if (!axis_parameter(ray, drag_frame_.origin,
					drag_frame_.axis[axis], &parameter))
					return false;
				float amount = snap_scalar(parameter - axis_start_);
				out->translation = scale(drag_frame_.axis[axis], amount);
				return true;
			}
			if (normal < 0)
				return false;

			camera_vec3 hit;
			if (!ray_plane(ray, drag_frame_.origin,
				drag_frame_.axis[normal], &hit))
				return false;
			int first = 0, second = 0;
			plane_axes(normal, &first, &second);
			const camera_vec3 offset = sub(hit, plane_start_);
			const float u = snap_scalar(dot(offset,
				drag_frame_.axis[first]));
			const float v = snap_scalar(dot(offset,
				drag_frame_.axis[second]));
			out->translation = add(scale(drag_frame_.axis[first], u),
				scale(drag_frame_.axis[second], v));
			return true;
		}

		if (mode_ == gizmo_mode::rotate)
		{
			if (!ray_provider_)
				return false;
			gizmo_ray ray;
			if (!ray_provider_(x, y, &ray))
				return false;
			if (!finite_vec(ray.origin) || !normalize(&ray.direction))
				return false;

			const int normal = rotation_axis_index(active_);
			if (normal < 0)
				return false;
			camera_vec3 hit;
			if (!ray_plane(ray, drag_frame_.origin,
				drag_frame_.axis[normal], &hit))
				return false;
			float angle = 0.0f;
			if (!ring_angle(hit, drag_frame_, normal, &angle))
				return false;

			float raw = angle - ring_start_angle_;
			while (raw - ring_last_angle_ > kPi)
				raw -= 2.0f * kPi;
			while (raw - ring_last_angle_ < -kPi)
				raw += 2.0f * kPi;
			ring_last_angle_ = raw;

			const float snapped = snap_rotation(raw);
			out->rotation = scale(drag_frame_.axis[normal], snapped);
			return true;
		}

		/* Scale: axes and the centre both use projected movement, no ray. */
		const int axis = scale_axis_index(active_);
		if (axis >= 0)
		{
			const screen_axis& screen_axis = drag_geometry_.axis[axis];
			if (!screen_axis.valid)
				return false;
			const float offset =
				(x - drag_start_x_) * screen_axis.dx +
				(y - drag_start_y_) * screen_axis.dy;
			const float denominator = std::max(drag_geometry_.length, 18.0f);
			float factor = 1.0f + offset / denominator;
			factor = snap_scale_value(factor);
			factor = std::max(factor, 0.01f);
			out->scale = { 1.0f, 1.0f, 1.0f };
			if (axis == 0)
				out->scale.x = factor;
			else if (axis == 1)
				out->scale.y = factor;
			else
				out->scale.z = factor;
			return true;
		}

		if (active_ != gizmo_handle::scale_uniform)
			return false;
		const float distance = std::hypot(x - drag_geometry_.ox,
			y - drag_geometry_.oy);
		const float denominator = std::max(drag_geometry_.length * 0.5f,
			18.0f);
		float factor = 1.0f + (distance - drag_start_distance_) /
			denominator;
		factor = snap_scale_value(factor);
		factor = std::max(factor, 0.01f);
		out->scale = { factor, factor, factor };
		return true;
	}

	void viewport_gizmo::update_drag(float x, float y)
	{
		gizmo_transform transform;
		if (compute_transform(x, y, &transform))
			last_transform_ = transform;
		emit(gizmo_phase::preview, last_transform_);
	}

	void viewport_gizmo::finish_drag(bool commit, float x, float y)
	{
		if (!dragging_)
			return;
		gizmo_transform transform = last_transform_;
		if (commit)
		{
			gizmo_transform candidate;
			if (compute_transform(x, y, &candidate))
				transform = candidate;
		}
		dragging_ = false;
		active_ = gizmo_handle::none;
		hovered_ = gizmo_handle::none;
		last_transform_ = {};
		emit(commit ? gizmo_phase::commit : gizmo_phase::cancel, transform);
	}

	void viewport_gizmo::emit(gizmo_phase phase,
		const gizmo_transform& transform)
	{
		if (drag_handler_ && transform.mode == gizmo_mode::translate)
			drag_handler_(phase, transform.translation);
		if (transform_handler_)
			transform_handler_(phase, transform);
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

		switch (mode_)
		{
		case gizmo_mode::translate:
			draw_translate(cb, vg, x, y, geometry);
			break;
		case gizmo_mode::rotate:
			draw_rotate(cb, vg, x, y, geometry);
			break;
		case gizmo_mode::scale:
			draw_scale(cb, vg, x, y, geometry);
			break;
		}

		if (mode_ != gizmo_mode::scale)
		{
			/* Non-interactive pivot marker keeps the origin readable over busy
			 * scenes without competing with the axis hit boxes. */
			ovg_tools::fill_circle(cb, vg, x + geometry.ox, y + geometry.oy,
				3.5f, ovg_tools::rgba(18, 20, 24, 220));
			ovg_tools::stroke_circle(cb, vg, x + geometry.ox, y + geometry.oy,
				3.5f, ovg_tools::rgba(220, 226, 236, 240), 1.0f);
		}
	}

	void viewport_gizmo::draw_translate(ovg_ctx_cb* cb, rvg_t* vg, float x,
		float y, const screen_geometry& geometry) const
	{
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
	}

	void viewport_gizmo::draw_rotate(ovg_ctx_cb* cb, rvg_t* vg, float x,
		float y, const screen_geometry& geometry) const
	{
		for (int normal = 0; normal < 3; ++normal)
		{
			const screen_ring& ring = geometry.ring[normal];
			if (!ring.valid)
				continue;
			const gizmo_handle handle = rotation_handle(normal);
			const bool highlighted = handle == hovered_ ||
				(dragging_ && handle == active_);
			uint32_t color = kAxisColors[normal];
			float line_width = axis_width_;
			if (highlighted)
			{
				color = ovg_tools::lerp_color(color, 0xFFFFFFFFu, 0.4f);
				line_width += 1.0f;
			}
			else if (dragging_)
			{
				color = ovg_tools::with_alpha(color, 72);
			}

			glm::vec2 points[kRingSegments] = {};
			for (int segment = 0; segment < kRingSegments; ++segment)
			{
				points[segment] = { x + ring.x[segment],
					y + ring.y[segment] };
			}
			ovg_tools::stroke_polyline(cb, vg, points, kRingSegments,
				0xB4000000u, line_width + 2.5f, VG_LINE_CAP_ROUND,
				VG_LINE_JOIN_ROUND, true);
			ovg_tools::stroke_polyline(cb, vg, points, kRingSegments,
				color, line_width, VG_LINE_CAP_ROUND, VG_LINE_JOIN_ROUND,
				true);
		}
	}

	void viewport_gizmo::draw_scale(ovg_ctx_cb* cb, rvg_t* vg, float x,
		float y, const screen_geometry& geometry) const
	{
		for (int i = 0; i < 3; ++i)
		{
			const screen_axis& axis = geometry.axis[i];
			if (!axis.valid)
				continue;
			const gizmo_handle handle = scale_handle(i);
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

			glm::vec2 points[2] = {
				{ x + geometry.ox, y + geometry.oy },
				{ x + axis.ex, y + axis.ey }
			};
			ovg_tools::stroke_polyline(cb, vg, points, 2, 0xB4000000u,
				line_width + 2.5f, VG_LINE_CAP_ROUND,
				VG_LINE_JOIN_ROUND, false);
			ovg_tools::stroke_polyline(cb, vg, points, 2, color,
				line_width, VG_LINE_CAP_ROUND, VG_LINE_JOIN_ROUND, false);

			const float handle_size = highlighted ? 8.0f : 6.5f;
			const float hx = x + axis.ex - handle_size * 0.5f;
			const float hy = y + axis.ey - handle_size * 0.5f;
			ovg_tools::fill_rect(cb, vg, hx - 1.0f, hy - 1.0f,
				handle_size + 2.0f, handle_size + 2.0f, 0xB4000000u);
			ovg_tools::fill_rect(cb, vg, hx, hy, handle_size, handle_size,
				color);
		}

		/* Centre square: uniform scale. */
		const bool center_highlighted = hovered_ == gizmo_handle::scale_uniform ||
			(dragging_ && active_ == gizmo_handle::scale_uniform);
		uint32_t center_color = ovg_tools::rgba(220, 226, 236, 240);
		float center_size = center_highlighted ? 9.0f : 7.0f;
		if (dragging_ && !center_highlighted)
			center_color = ovg_tools::with_alpha(center_color, 80);
		const float cx = x + geometry.ox;
		const float cy = y + geometry.oy;
		ovg_tools::fill_rect(cb, vg, cx - center_size * 0.5f - 1.0f,
			cy - center_size * 0.5f - 1.0f, center_size + 2.0f,
			center_size + 2.0f, 0xB4000000u);
		ovg_tools::fill_rect(cb, vg, cx - center_size * 0.5f,
			cy - center_size * 0.5f, center_size, center_size,
			center_color);
		ovg_tools::stroke_rect(cb, vg, cx - center_size * 0.5f,
			cy - center_size * 0.5f, center_size, center_size,
			center_highlighted ? 0xFFFFFFFFu : ovg_tools::rgba(18, 20, 24, 220),
			1.0f);
	}

} /* namespace vgui_editor_kit */
