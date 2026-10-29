#include <vgui/editor/viewport_camera.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace vgui_editor_kit {

	namespace {

		constexpr float kPi = 3.14159265358979323846f;
		/* ~86 度：再大就接近万向锁，right/up 会退化。 */
		constexpr float kMaxPitch = 1.5f;

		camera_vec3 vmake(float x, float y, float z)
		{
			camera_vec3 v;
			v.x = x;
			v.y = y;
			v.z = z;
			return v;
		}

		camera_vec3 vadd(const camera_vec3& a, const camera_vec3& b)
		{
			return vmake(a.x + b.x, a.y + b.y, a.z + b.z);
		}

		camera_vec3 vsub(const camera_vec3& a, const camera_vec3& b)
		{
			return vmake(a.x - b.x, a.y - b.y, a.z - b.z);
		}

		camera_vec3 vscale(const camera_vec3& v, float s)
		{
			return vmake(v.x * s, v.y * s, v.z * s);
		}

		float vdot(const camera_vec3& a, const camera_vec3& b)
		{
			return a.x * b.x + a.y * b.y + a.z * b.z;
		}

		camera_vec3 vcross(const camera_vec3& a, const camera_vec3& b)
		{
			return vmake(a.y * b.z - a.z * b.y,
				a.z * b.x - a.x * b.z,
				a.x * b.y - a.y * b.x);
		}

		float vlength(const camera_vec3& v)
		{
			return std::sqrt(vdot(v, v));
		}

		camera_vec3 vnormalize(const camera_vec3& v)
		{
			const float len = vlength(v);
			if (len < 1e-6f)
				return vmake(0.0f, 0.0f, 0.0f);
			return vscale(v, 1.0f / len);
		}

		float clampf(float v, float lo, float hi)
		{
			return v < lo ? lo : (v > hi ? hi : v);
		}

		bool finite_vec(const camera_vec3& v)
		{
			return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
		}

	} /* namespace */

	viewport_camera::~viewport_camera()
	{
		detach();
	}

	void viewport_camera::attach(viewport_view& viewport)
	{
		detach();
		viewport_ = &viewport;
		listener_token_ = viewport.add_event_listener(
			[this](const vgui_event* event) { return on_input(event); });
		if (!listener_token_)
			viewport_ = nullptr;
	}

	void viewport_camera::detach()
	{
		if (viewport_ && listener_token_)
			viewport_->remove_event_listener(listener_token_);
		viewport_ = nullptr;
		listener_token_ = 0;
		mode_ = nav_mode::none;
	}

	bool viewport_camera::attached() const noexcept
	{
		return viewport_ != nullptr;
	}

	void viewport_camera::set_navigation_enabled(bool enabled)
	{
		if (navigation_enabled_ == enabled)
			return;
		navigation_enabled_ = enabled;
		if (!enabled)
			end_navigation();
	}

	bool viewport_camera::navigation_enabled() const noexcept
	{
		return navigation_enabled_;
	}

	void viewport_camera::set_pan_button(int button)
	{
		pan_button_ = std::max(0, button);
		if (mode_ == nav_mode::pan && pan_button_ == 0)
			end_navigation();
	}

	void viewport_camera::set_orbit_button(int button)
	{
		orbit_button_ = std::max(0, button);
		if (mode_ == nav_mode::orbit && orbit_button_ == 0)
			end_navigation();
	}

	void viewport_camera::set_orbit_sensitivity(float radians_per_pixel)
	{
		orbit_sensitivity_ = std::max(0.0f, radians_per_pixel);
	}

	void viewport_camera::set_zoom_speed(float speed)
	{
		zoom_speed_ = std::max(0.0f, speed);
	}

	void viewport_camera::set_distance_range(float min_distance, float max_distance)
	{
		min_distance_ = std::max(0.001f, min_distance);
		max_distance_ = std::max(min_distance_ + 0.001f, max_distance);
		const float previous = state_.distance;
		state_.distance = clampf(state_.distance, min_distance_, max_distance_);
		if (state_.distance != previous)
			notify_change();
	}

	void viewport_camera::set_focus_provider(focus_provider provider)
	{
		focus_provider_ = std::move(provider);
	}

	bool viewport_camera::focus_selection()
	{
		if (!focus_provider_)
			return false;
		camera_focus focus;
		if (!focus_provider_(&focus))
			return false;

		state_.target = focus.center;
		if (focus.radius > 0.0f)
		{
			const float half_fov = state_.fov_deg * 0.5f * kPi / 180.0f;
			float sine = std::sin(half_fov);
			if (sine < 0.05f)
				sine = 0.05f;
			state_.distance = clampf(focus.radius / sine * 1.15f,
				min_distance_, max_distance_);
		}
		notify_change();
		return true;
	}

	bool viewport_camera::panning() const noexcept
	{
		return mode_ == nav_mode::pan;
	}

	bool viewport_camera::orbiting() const noexcept
	{
		return mode_ == nav_mode::orbit;
	}

	bool viewport_camera::navigating() const noexcept
	{
		return mode_ != nav_mode::none;
	}

	void viewport_camera::cancel_navigation()
	{
		end_navigation();
	}

	camera_state viewport_camera::get_state() const
	{
		return state_;
	}

	void viewport_camera::set_state(const camera_state& state)
	{
		state_ = state;
		if (!finite_vec(state_.target))
			state_.target = {};
		if (!std::isfinite(state_.yaw))
			state_.yaw = camera_state{}.yaw;
		state_.pitch = clampf(state_.pitch, -kMaxPitch, kMaxPitch);
		state_.distance = clampf(state_.distance, min_distance_, max_distance_);
		state_.fov_deg = clampf(state_.fov_deg, 5.0f, 120.0f);
		if (!(state_.near_plane > 0.0f) || !std::isfinite(state_.near_plane))
			state_.near_plane = 0.01f;
		if (!(state_.far_plane > state_.near_plane) || !std::isfinite(state_.far_plane))
			state_.far_plane = std::max(state_.near_plane * 1000.0f, 1000.0f);
		notify_change();
	}

	void viewport_camera::reset()
	{
		set_state(camera_state{});
	}

	camera_vec3 viewport_camera::position() const
	{
		const camera_vec3 dir = direction();
		return vadd(state_.target, vscale(dir, state_.distance));
	}

	camera_vec3 viewport_camera::forward() const
	{
		return vscale(direction(), -1.0f);
	}

	camera_vec3 viewport_camera::right() const
	{
		const camera_vec3 side = vcross(forward(), vmake(0.0f, 1.0f, 0.0f));
		if (vlength(side) < 1e-6f)
			return vmake(1.0f, 0.0f, 0.0f);
		return vnormalize(side);
	}

	camera_vec3 viewport_camera::up() const
	{
		return vnormalize(vcross(right(), forward()));
	}

	void viewport_camera::set_change_listener(change_listener listener)
	{
		change_listener_ = std::move(listener);
	}

	int viewport_camera::on_input(const vgui_event* event)
	{
		if (!event || !viewport_ || !navigation_enabled_)
			return 0;

		switch (event->type)
		{
		case VGUI_EVENT_MOUSE_DOWN:
			if (!inside_surface(event->x, event->y))
				return 0;
			if (orbit_button_ > 0 && event->button == orbit_button_)
			{
				begin_navigation(nav_mode::orbit, event->x, event->y);
				return 0;
			}
			if (pan_button_ > 0 && event->button == pan_button_)
			{
				begin_navigation(nav_mode::pan, event->x, event->y);
				return 0;
			}
			return 0;

		case VGUI_EVENT_MOUSE_MOVE:
			if (mode_ == nav_mode::none)
				return 0;
			apply_navigation(event->x, event->y);
			return 1;

		case VGUI_EVENT_MOUSE_UP:
			if (mode_ == nav_mode::orbit && event->button == orbit_button_)
			{
				end_navigation();
				return 1;
			}
			if (mode_ == nav_mode::pan && event->button == pan_button_)
			{
				end_navigation();
				return 1;
			}
			return 0;

		case VGUI_EVENT_CANCEL:
			if (mode_ == nav_mode::none)
				return 0;
			end_navigation();
			return 1;

		case VGUI_EVENT_MOUSE_WHEEL:
			if (event->wheel_y == 0.0f)
				return 0;
			apply_zoom(event->wheel_y);
			return 1;

		case VGUI_EVENT_KEY_DOWN:
			if (event->key != 'F' || !viewport_->focused())
				return 0;
			if (event->mods & (VGUI_MOD_SHIFT | VGUI_MOD_CTRL |
				VGUI_MOD_ALT | VGUI_MOD_GUI))
				return 0;
			return focus_selection() ? 1 : 0;

		default:
			return 0;
		}
	}

	void viewport_camera::begin_navigation(nav_mode mode, float x, float y)
	{
		if (mode_ != nav_mode::none || mode == nav_mode::none)
			return;
		mode_ = mode;
		last_x_ = x;
		last_y_ = y;
	}

	void viewport_camera::end_navigation()
	{
		mode_ = nav_mode::none;
	}

	void viewport_camera::apply_navigation(float x, float y)
	{
		const float dx = x - last_x_;
		const float dy = y - last_y_;
		last_x_ = x;
		last_y_ = y;
		if (dx == 0.0f && dy == 0.0f)
			return;

		if (mode_ == nav_mode::orbit)
		{
			state_.yaw += dx * orbit_sensitivity_;
			state_.pitch = clampf(state_.pitch + dy * orbit_sensitivity_,
				-kMaxPitch, kMaxPitch);
			notify_change();
			return;
		}
		if (mode_ != nav_mode::pan)
			return;

		const float height = viewport_height();
		const float half_fov = state_.fov_deg * 0.5f * kPi / 180.0f;
		const float speed = 2.0f * state_.distance * std::tan(half_fov) /
			std::max(height, 1.0f);
		const camera_vec3 side = right();
		const camera_vec3 up_axis = up();
		state_.target.x += (-side.x * dx + up_axis.x * dy) * speed;
		state_.target.y += (-side.y * dx + up_axis.y * dy) * speed;
		state_.target.z += (-side.z * dx + up_axis.z * dy) * speed;
		notify_change();
	}

	void viewport_camera::apply_zoom(float wheel_y)
	{
		const float exponent = clampf(-wheel_y * zoom_speed_, -30.0f, 30.0f);
		state_.distance = clampf(state_.distance * std::exp(exponent),
			min_distance_, max_distance_);
		notify_change();
	}

	void viewport_camera::notify_change()
	{
		if (change_listener_)
			change_listener_();
	}

	bool viewport_camera::inside_surface(float x, float y) const
	{
		if (!viewport_ || !viewport_->widget())
			return false;
		float wx = 0, wy = 0, ww = 0, wh = 0;
		vgui_widget_get_rect(viewport_->widget(), &wx, &wy, &ww, &wh);
		return x >= 0.0f && y >= 0.0f && x < ww && y < wh;
	}

	float viewport_camera::viewport_height() const
	{
		if (viewport_)
		{
			const int rendered = viewport_->render_height();
			if (rendered > 0)
				return (float)rendered;
			if (viewport_->widget())
			{
				float wx = 0, wy = 0, ww = 0, wh = 0;
				vgui_widget_get_rect(viewport_->widget(), &wx, &wy, &ww, &wh);
				if (wh > 0.0f)
					return wh;
			}
		}
		return 600.0f;
	}

	camera_vec3 viewport_camera::direction() const
	{
		const float cos_pitch = std::cos(state_.pitch);
		return vmake(cos_pitch * std::sin(state_.yaw),
			std::sin(state_.pitch),
			cos_pitch * std::cos(state_.yaw));
	}

} /* namespace vgui_editor_kit */
