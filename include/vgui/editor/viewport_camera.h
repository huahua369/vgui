#pragma once
/*
 * viewport_camera.h - orbit camera navigation for viewport_view (V02).
 *
 * The component owns a small, renderer-agnostic camera state and turns
 * viewport input into navigation:
 *
 *  * middle drag pans the orbit target in screen space
 *  * right drag orbits the camera around the target
 *  * wheel dollies towards / away from the target
 *  * F (with the viewport focused) frames the host-provided selection bounds
 *
 * The left button is deliberately untouched so picking (V03) and gizmos (V05)
 * can own it.  Camera changes are transient view state, so they never enter
 * the command stack; hosts persist camera_state together with their document.
 *
 * The component registers an additive listener on viewport_view, so a host
 * listener installed with set_event_listener() keeps receiving events.
 */
#include <functional>

#include "viewport_view.h"

namespace vgui_editor_kit {

	struct camera_vec3
	{
		float x = 0.0f;
		float y = 0.0f;
		float z = 0.0f;
	};

	/* Serializable orbit state.  The camera position is derived from target,
	 * yaw, pitch and distance, so restoring these fields restores the view. */
	struct camera_state
	{
		camera_vec3 target{};
		float yaw = 0.62f;      /* radians around the world Y axis */
		float pitch = 0.47f;    /* radians; positive means above the target */
		float distance = 6.2f;
		float fov_deg = 50.0f;
		float near_plane = 0.1f;
		float far_plane = 1000.0f;
	};

	struct camera_focus
	{
		camera_vec3 center{};
		float radius = 0.0f;
	};

	class viewport_camera
	{
	public:
		using focus_provider = std::function<bool(camera_focus* out)>;
		using change_listener = std::function<void()>;

		viewport_camera() = default;
		~viewport_camera();

		viewport_camera(const viewport_camera&) = delete;
		viewport_camera& operator=(const viewport_camera&) = delete;

		/* Attaching twice detaches first, so one camera drives one surface. */
		void attach(viewport_view& viewport);
		void detach();
		bool attached() const noexcept;

		/* Disabling stops navigation and cancels an in-flight drag; the camera
		 * keeps its current position.  Hosts use this while a gizmo owns the
		 * pointer. */
		void set_navigation_enabled(bool enabled);
		bool navigation_enabled() const noexcept;

		/* 0 disables the gesture.  Defaults: pan = middle (2), orbit = right (3). */
		void set_pan_button(int button);
		void set_orbit_button(int button);

		void set_orbit_sensitivity(float radians_per_pixel);
		void set_zoom_speed(float speed);
		void set_distance_range(float min_distance, float max_distance);

		void set_focus_provider(focus_provider provider);
		/* Frames the provider target.  Callable directly from menus/toolbars;
		 * the F shortcut additionally requires viewport focus. */
		bool focus_selection();

		bool panning() const noexcept;
		bool orbiting() const noexcept;
		bool navigating() const noexcept;
		/* Ends the drag without moving the camera further. */
		void cancel_navigation();

		camera_state get_state() const;
		/* Clamps pitch, distance, fov and clip planes into safe ranges. */
		void set_state(const camera_state& state);
		void reset();

		camera_vec3 position() const;
		camera_vec3 forward() const;
		camera_vec3 right() const;
		camera_vec3 up() const;

		void set_change_listener(change_listener listener);

	private:
		enum class nav_mode
		{
			none,
			pan,
			orbit
		};

		int on_input(const vgui_event* event);
		void begin_navigation(nav_mode mode, float x, float y);
		void end_navigation();
		void apply_navigation(float x, float y);
		void apply_zoom(float wheel_y);
		void notify_change();
		bool inside_surface(float x, float y) const;
		float viewport_height() const;
		camera_vec3 direction() const;

		viewport_view* viewport_ = nullptr;
		int listener_token_ = 0;

		nav_mode mode_ = nav_mode::none;
		float last_x_ = 0.0f;
		float last_y_ = 0.0f;

		camera_state state_{};
		float min_distance_ = 0.1f;
		float max_distance_ = 1000.0f;
		int pan_button_ = 2;
		int orbit_button_ = 3;
		float orbit_sensitivity_ = 0.006f;
		float zoom_speed_ = 0.12f;
		bool navigation_enabled_ = true;

		focus_provider focus_provider_;
		change_listener change_listener_;
	};

} /* namespace vgui_editor_kit */
