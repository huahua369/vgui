#pragma once
/*
 * viewport_gizmo.h - translate/rotate/scale gizmo for viewport_view (V05/V07).
 *
 * The kit owns the gizmo's screen-space geometry, hit testing, drag state and
 * overlay drawing; the host owns the scene, projection/ray math and undo
 * integration.  This mirrors V03: no camera, scene or command-stack dependency
 * leaks into the widget layer.
 *
 * Host contract:
 *  * frame_provider returns the world-space pivot and three orthonormal axes
 *    for the current selection (multi-select centre included).  The axes must
 *    already reflect world/local/parent/screen space: the gizmo only draws and
 *    constrains, it cannot derive local orientation from a scene.
 *  * projection maps a world point to viewport content coordinates.
 *  * ray_provider turns content coordinates into a world ray.
 *  * transform_handler receives begin / preview / commit / cancel plus a
 *    gizmo_transform: translation is a world-space delta, rotation is a
 *    world-space axis * angle (radians) vector, scale is a per-axis multiplier
 *    in the frame's axes.  The host snapshots transforms on begin, previews on
 *    every move, records one command on commit, and restores on cancel.
 *  * drag_handler is the legacy V05 translate-only callback.  It is invoked
 *    for translate gestures and receives the same world-space delta.
 *
 * Interaction:
 *  * translate: LMB on an axis constrains the drag to that axis; LMB inside a
 *    plane quad constrains the drag to that plane.
 *  * rotate: LMB on one of the three projected rings rotates around that
 *    ring's frame axis.  The signed angle uses ray/ring-plane intersection and
 *    unwraps across +/-pi, so a drag can pass multiple turns.
 *  * scale: LMB on an axis handle scales along that axis; LMB on the centre
 *    handle scales uniformly.  Factors derive from projected mouse movement,
 *    so no ray provider is needed for scale gestures.
 *  * Esc, VGUI_EVENT_CANCEL (focus loss) or cancel_drag() ends a drag with a
 *    cancel phase, so the host can roll back.
 *  * Snapping is applied to the drag increments via snap_settings (translation
 *    step, rotation degrees, scale factor), never to the selection's absolute
 *    transform, so it works for local axes too.
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

	enum class gizmo_mode
	{
		translate,
		rotate,
		scale
	};

	/* The host resolves every space into frame axes.  `screen` typically means
	 * the camera basis, which gives screen-facing rotation rings; `parent`
	 * means the primary selection's parent orientation. */
	enum class gizmo_space
	{
		world,
		local,
		parent,
		screen
	};

	enum class gizmo_handle
	{
		none,
		axis_x,
		axis_y,
		axis_z,
		plane_yz,
		plane_zx,
		plane_xy,
		rotate_x,
		rotate_y,
		rotate_z,
		scale_x,
		scale_y,
		scale_z,
		scale_uniform
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
		/* Orthonormal world-space axes, already resolved for world/local/
		 * parent/screen space. */
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

	/* Payload shared by every mode.  Unused fields keep their neutral values:
	 * translation {0,0,0}, rotation {0,0,0}, scale {1,1,1}.  rotation is a
	 * world-space rotation vector (axis * radians); scale components multiply
	 * along the frame axes at drag start. */
	struct gizmo_transform
	{
		gizmo_mode mode = gizmo_mode::translate;
		gizmo_handle handle = gizmo_handle::none;
		/* Pivot and axes captured at begin; rotation/scale are relative to
		 * these, not to the frame the provider returns while previewing. */
		camera_vec3 origin{};
		camera_vec3 axis[3] = {
			{ 1.0f, 0.0f, 0.0f },
			{ 0.0f, 1.0f, 0.0f },
			{ 0.0f, 0.0f, 1.0f }
		};
		camera_vec3 translation{};
		camera_vec3 rotation{};
		camera_vec3 scale{ 1.0f, 1.0f, 1.0f };
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
		/* Legacy V05 callback: translate gestures only, same world delta. */
		using drag_handler = std::function<void(gizmo_phase phase,
			const camera_vec3& delta)>;
		/* Full payload for every mode; see gizmo_transform. */
		using transform_handler = std::function<void(gizmo_phase phase,
			const gizmo_transform& transform)>;

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
		void set_transform_handler(transform_handler handler);

		/* Borrowed.  Snapping is read at drag time, so command/menu changes to
		 * the shared snap_settings take effect immediately. */
		void set_snap_settings(const snap_settings* snap);

		/* Hiding cancels an in-flight drag. */
		void set_visible(bool visible);
		bool visible() const noexcept;

		/* Changing mode or space cancels an in-flight drag; the host's frame
		 * provider must consult mode()/space() to build the matching axes. */
		void set_mode(gizmo_mode mode);
		gizmo_mode mode() const noexcept;
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
		static constexpr int kRingSegments = 64;

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

		struct screen_ring
		{
			bool valid = false;
			float x[kRingSegments] = {};
			float y[kRingSegments] = {};
		};

		struct screen_geometry
		{
			bool valid = false;
			float ox = 0.0f;
			float oy = 0.0f;
			float length = 0.0f;
			screen_axis axis[3];
			screen_plane plane[3];
			screen_ring ring[3];
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
		bool compute_transform(float x, float y, gizmo_transform* out);
		void update_drag(float x, float y);
		void finish_drag(bool commit, float x, float y);
		void emit(gizmo_phase phase, const gizmo_transform& transform);
		float snap_scalar(float value) const;
		float snap_rotation(float radians) const;
		float snap_scale_value(float value) const;
		void draw(ovg_ctx_cb* cb, rvg_t* vg, float x, float y, float width,
			float height, float delta_time);
		void draw_translate(ovg_ctx_cb* cb, rvg_t* vg, float x, float y,
			const screen_geometry& geometry) const;
		void draw_rotate(ovg_ctx_cb* cb, rvg_t* vg, float x, float y,
			const screen_geometry& geometry) const;
		void draw_scale(ovg_ctx_cb* cb, rvg_t* vg, float x, float y,
			const screen_geometry& geometry) const;

		viewport_view* viewport_ = nullptr;
		int listener_token_ = 0;

		frame_provider frame_provider_;
		project_fn projection_;
		ray_provider ray_provider_;
		drag_handler drag_handler_;
		transform_handler transform_handler_;
		const snap_settings* snap_ = nullptr;

		bool visible_ = true;
		gizmo_mode mode_ = gizmo_mode::translate;
		gizmo_space space_ = gizmo_space::world;
		float screen_length_ = 80.0f;
		float handle_tolerance_ = 7.0f;
		float axis_width_ = 2.0f;

		bool dragging_ = false;
		gizmo_handle hovered_ = gizmo_handle::none;
		gizmo_handle active_ = gizmo_handle::none;
		gizmo_frame drag_frame_{};
		screen_geometry drag_geometry_{};
		gizmo_transform drag_transform_{};
		gizmo_transform last_transform_{};
		float axis_start_ = 0.0f;
		camera_vec3 plane_start_{};
		float ring_start_angle_ = 0.0f;
		float ring_last_angle_ = 0.0f;
		float drag_start_x_ = 0.0f;
		float drag_start_y_ = 0.0f;
		float drag_start_distance_ = 0.0f;
	};

} /* namespace vgui_editor_kit */
