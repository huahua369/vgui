#pragma once
/*
 * viewport_gizmo.h - translate gizmo for viewport_view (V05).
 *
 * The kit owns the gizmo's screen-space geometry, hit testing, drag state and
 * overlay drawing; the host owns the scene, projection/ray math and undo
 * integration.  This mirrors V03: no camera, scene or command-stack dependency
 * leaks into the widget layer.
 *
 * Host contract:
 *  * frame_provider returns the world-space pivot and three orthonormal axes
 *    for the current selection (multi-select centre included).  The axes must
 *    already reflect world/local space: the gizmo only draws and constrains,
 *    it cannot derive local orientation from a scene.
 *  * projection maps a world point to viewport content coordinates.
 *  * ray_provider turns content coordinates into a world ray.
 *  * drag_handler receives begin / preview / commit / cancel and a world-space
 *    translation delta.  The host snapshots positions on begin, previews on
 *    every move, records one command on commit, and restores on cancel.
 *
 * Interaction:
 *  * LMB on an axis constrains the drag to that axis.
 *  * LMB inside a plane quad constrains the drag to that plane.
 *  * Esc, VGUI_EVENT_CANCEL (focus loss) or cancel_drag() ends a drag with a
 *    cancel phase, so the host can roll back.
 *  * Snapping is applied to the drag increments via snap_settings, never to
 *    the selection's absolute position, so it works for local axes too.
 *
 * The gizmo is drawn in viewport_view's foreground overlay, i.e. above the
 * selection highlight and the box-select marquee (the V03 layering contract).
 * Insert it before viewport_selection::attach() so its LMB hit test gets the
 * event before picking starts a select gesture.
 */
#include <functional>

#include "viewport_camera.h"
#include "viewport_grid.h"
#include "viewport_view.h"

namespace vgui_editor_kit {

	enum class gizmo_space
	{
		world,
		local
	};

	enum class gizmo_handle
	{
		none,
		axis_x,
		axis_y,
		axis_z,
		plane_yz,
		plane_zx,
		plane_xy
	};

	/* Drag lifecycle.  begin delta is zero; cancel is emitted for every
	 * interrupted drag after at least one begin. */
	enum class gizmo_phase
	{
		begin,
		preview,
		commit,
		cancel
	};

	struct gizmo_frame
	{
		bool valid = false;
		camera_vec3 origin{};
		/* Orthonormal world-space axes, already resolved for world/local space. */
		camera_vec3 axis[3] = {
			{ 1.0f, 0.0f, 0.0f },
			{ 0.0f, 1.0f, 0.0f },
			{ 0.0f, 0.0f, 1.0f }
		};
		/* Optional per-frame override; <= 0 uses viewport_gizmo::screen_length(). */
		float screen_length = 0.0f;
	};

	struct gizmo_ray
	{
		camera_vec3 origin{};
		camera_vec3 direction{};
	};

	class viewport_gizmo
	{
	public:
		using frame_provider = std::function<gizmo_frame()>;
		/* world -> content-local screen coordinates.  Return false when the
		 * point is behind the camera or otherwise not drawable. */
		using project_fn = std::function<bool(const camera_vec3& world,
			float* x, float* y)>;
		/* content-local coordinates -> world ray. */
		using ray_provider = std::function<bool(float x, float y,
			gizmo_ray* out)>;
		using drag_handler = std::function<void(gizmo_phase phase,
			const camera_vec3& delta)>;

		viewport_gizmo() = default;
		~viewport_gizmo();

		viewport_gizmo(const viewport_gizmo&) = delete;
		viewport_gizmo& operator=(const viewport_gizmo&) = delete;

		void attach(viewport_view& viewport);
		void detach();
		bool attached() const noexcept;

		void set_frame_provider(frame_provider provider);
		void set_projection(project_fn projection);
		void set_ray_provider(ray_provider provider);
		void set_drag_handler(drag_handler handler);

		/* Borrowed.  Snapping is read at drag time, so command/menu changes to
		 * the shared snap_settings take effect immediately. */
		void set_snap_settings(const snap_settings* snap);

		/* Hiding cancels an in-flight drag. */
		void set_visible(bool visible);
		bool visible() const noexcept;

		/* Changing space cancels an in-flight drag; the host's frame provider
		 * must consult space() to build the matching axes. */
		void set_space(gizmo_space space);
		gizmo_space space() const noexcept;

		void set_screen_length(float pixels);
		float screen_length() const noexcept;
		void set_handle_tolerance(float pixels);
		void set_axis_width(float pixels);

		bool dragging() const noexcept;
		gizmo_handle active_handle() const noexcept;
		gizmo_handle hovered_handle() const noexcept;

		/* Ends a drag with a cancel phase.  Safe without an active drag. */
		void cancel_drag();
		void reset_hover();

	private:
		struct screen_axis
		{
			bool valid = false;
			float dx = 1.0f;
			float dy = 0.0f;
			float ex = 0.0f;
			float ey = 0.0f;
		};

		struct screen_plane
		{
			bool valid = false;
			float x[4] = {};
			float y[4] = {};
		};

		struct screen_geometry
		{
			bool valid = false;
			float ox = 0.0f;
			float oy = 0.0f;
			float length = 0.0f;
			screen_axis axis[3];
			screen_plane plane[3];
		};

		int on_input(const vgui_event* event);
		void content_geometry(float* offset_x, float* offset_y,
			int* width, int* height) const;
		bool query_frame(gizmo_frame* out) const;
		bool build_geometry(const gizmo_frame& frame, int width, int height,
			screen_geometry* out) const;
		gizmo_handle hit_test(float x, float y, int width, int height) const;
		bool begin_drag(gizmo_handle handle, float x, float y,
			int width, int height);
		bool compute_delta(float x, float y, camera_vec3* delta) const;
		void update_drag(float x, float y);
		void finish_drag(bool commit, float x, float y);
		float snap_scalar(float value) const;
		void draw(ovg_ctx_cb* cb, rvg_t* vg, float x, float y, float width,
			float height, float delta_time);

		viewport_view* viewport_ = nullptr;
		int listener_token_ = 0;

		frame_provider frame_provider_;
		project_fn projection_;
		ray_provider ray_provider_;
		drag_handler drag_handler_;
		const snap_settings* snap_ = nullptr;

		bool visible_ = true;
		gizmo_space space_ = gizmo_space::world;
		float screen_length_ = 80.0f;
		float handle_tolerance_ = 7.0f;
		float axis_width_ = 2.0f;

		bool dragging_ = false;
		gizmo_handle hovered_ = gizmo_handle::none;
		gizmo_handle active_ = gizmo_handle::none;
		gizmo_frame drag_frame_{};
		float axis_start_ = 0.0f;
		camera_vec3 plane_start_{};
		camera_vec3 last_delta_{};
	};

} /* namespace vgui_editor_kit */
