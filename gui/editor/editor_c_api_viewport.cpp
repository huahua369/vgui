/* gui/editor/editor_c_api_viewport.cpp - C facade for the viewport surface, camera, grid, picking and gizmo declared in <vgui/vgui_editor.h>. */
/*
 * Only the viewport owns a widget: build() creates it and destroy() tears it
 * down, while camera / grid / picking / gizmo are plain objects that attach to a
 * viewport surface by reference.  Overlay callbacks forward the ovg_ctx_cb and
 * rvg_t pointers unchanged because the kit draws with the very same objects.
 * The gizmo borrows its snap policy by address, so the handle owns the copy it
 * installs; a borrowed (shell-owned) handle must outlive that install.  Host
 * callbacks capture the C function pointer and user by value - never the handle
 * - so a listener stays valid after the wrapper that installed it dies.
 */
#include "editor_c_api.h"
#include "editor_c_api_borrow.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include <vgui/editor/viewport_view.h>
#include <vgui/editor/viewport_camera.h>
#include <vgui/editor/viewport_grid.h>
#include <vgui/editor/viewport_selection.h>
#include <vgui/editor/viewport_gizmo.h>

using namespace vgui_editor_kit;

/* -------------------------------------------------------------------------- */
/* Handle storage                                                            */
/* -------------------------------------------------------------------------- */

struct vgui_editor_viewport : vgeditor::c_object<viewport_view> {
	using c_object::c_object;

	/* Overlay slots carry an optional user release, so they keep an owner. */
	vgeditor::callback_slot<vgui_editor_overlay_draw_fn> selection_overlay;
	vgeditor::callback_slot<vgui_editor_overlay_draw_fn> interaction_overlay;
	vgeditor::callback_slot<vgui_editor_overlay_draw_fn> foreground_overlay;

	vgui_editor_viewport_resize_fn resize_fn = nullptr;
	void* resize_user = nullptr;
	vgui_editor_viewport_event_fn event_fn = nullptr;
	void* event_user = nullptr;
	vgui_editor_viewport_focus_fn focus_fn = nullptr;
	void* focus_user = nullptr;
	vgui_editor_viewport_drop_fn drop_fn = nullptr;
	void* drop_user = nullptr;
	/* Tokens of the observers added through add_event_listener(), so destroy()
	 * can drop them again. */
	std::vector<int> observer_tokens;
};

struct vgui_editor_camera : vgeditor::c_object<viewport_camera> {
	using c_object::c_object;

	vgui_editor_camera_focus_fn focus_fn = nullptr;
	void* focus_user = nullptr;
	vgui_editor_camera_change_fn change_fn = nullptr;
	void* change_user = nullptr;
};

struct vgui_editor_grid : vgeditor::c_object<viewport_grid> {
	using c_object::c_object;
};

struct vgui_editor_picking : vgeditor::c_object<viewport_selection> {
	using c_object::c_object;

	vgui_editor_point_pick_fn point_fn = nullptr;
	void* point_user = nullptr;
	vgui_editor_box_pick_fn box_fn = nullptr;
	void* box_user = nullptr;
	vgui_editor_highlight_fn highlight_fn = nullptr;
	void* highlight_user = nullptr;
};

struct vgui_editor_gizmo : vgeditor::c_object<viewport_gizmo> {
	using c_object::c_object;

	/* Installed into the gizmo by address; the gizmo never copies it. */
	snap_settings snap{};
	vgui_editor_gizmo_frame_fn frame_fn = nullptr;
	void* frame_user = nullptr;
	vgui_editor_gizmo_project_fn project_fn = nullptr;
	void* project_user = nullptr;
	vgui_editor_gizmo_ray_fn ray_fn = nullptr;
	void* ray_user = nullptr;
	vgui_editor_gizmo_transform_fn transform_fn = nullptr;
	void* transform_user = nullptr;
	vgui_editor_gizmo_drag_fn drag_fn = nullptr;
	void* drag_user = nullptr;
};

namespace {

	/* The kit grows its box/highlight results on demand; these are the first
	 * capacities offered to a host callback. */
	constexpr size_t kPickChunk = 128;
	constexpr size_t kHighlightChunk = 64;

	static_assert((int)VGUI_EDITOR_GIZMO_TRANSLATE == (int)gizmo_mode::translate,
		"gizmo mode drift");
	static_assert((int)VGUI_EDITOR_GIZMO_ROTATE == (int)gizmo_mode::rotate,
		"gizmo mode drift");
	static_assert((int)VGUI_EDITOR_GIZMO_SCALE == (int)gizmo_mode::scale,
		"gizmo mode drift");

	static_assert((int)VGUI_EDITOR_GIZMO_SPACE_WORLD == (int)gizmo_space::world,
		"gizmo space drift");
	static_assert((int)VGUI_EDITOR_GIZMO_SPACE_LOCAL == (int)gizmo_space::local,
		"gizmo space drift");
	static_assert((int)VGUI_EDITOR_GIZMO_SPACE_PARENT == (int)gizmo_space::parent,
		"gizmo space drift");
	static_assert((int)VGUI_EDITOR_GIZMO_SPACE_SCREEN == (int)gizmo_space::screen,
		"gizmo space drift");

	static_assert((int)VGUI_EDITOR_GIZMO_HANDLE_NONE == (int)gizmo_handle::none,
		"gizmo handle drift");
	static_assert((int)VGUI_EDITOR_GIZMO_HANDLE_AXIS_X == (int)gizmo_handle::axis_x,
		"gizmo handle drift");
	static_assert((int)VGUI_EDITOR_GIZMO_HANDLE_AXIS_Y == (int)gizmo_handle::axis_y,
		"gizmo handle drift");
	static_assert((int)VGUI_EDITOR_GIZMO_HANDLE_AXIS_Z == (int)gizmo_handle::axis_z,
		"gizmo handle drift");
	static_assert((int)VGUI_EDITOR_GIZMO_HANDLE_PLANE_YZ ==
		(int)gizmo_handle::plane_yz, "gizmo handle drift");
	static_assert((int)VGUI_EDITOR_GIZMO_HANDLE_PLANE_ZX ==
		(int)gizmo_handle::plane_zx, "gizmo handle drift");
	static_assert((int)VGUI_EDITOR_GIZMO_HANDLE_PLANE_XY ==
		(int)gizmo_handle::plane_xy, "gizmo handle drift");
	static_assert((int)VGUI_EDITOR_GIZMO_HANDLE_ROTATE_X ==
		(int)gizmo_handle::rotate_x, "gizmo handle drift");
	static_assert((int)VGUI_EDITOR_GIZMO_HANDLE_ROTATE_Y ==
		(int)gizmo_handle::rotate_y, "gizmo handle drift");
	static_assert((int)VGUI_EDITOR_GIZMO_HANDLE_ROTATE_Z ==
		(int)gizmo_handle::rotate_z, "gizmo handle drift");
	static_assert((int)VGUI_EDITOR_GIZMO_HANDLE_SCALE_X ==
		(int)gizmo_handle::scale_x, "gizmo handle drift");
	static_assert((int)VGUI_EDITOR_GIZMO_HANDLE_SCALE_Y ==
		(int)gizmo_handle::scale_y, "gizmo handle drift");
	static_assert((int)VGUI_EDITOR_GIZMO_HANDLE_SCALE_Z ==
		(int)gizmo_handle::scale_z, "gizmo handle drift");
	static_assert((int)VGUI_EDITOR_GIZMO_HANDLE_SCALE_UNIFORM ==
		(int)gizmo_handle::scale_uniform, "gizmo handle drift");

	static_assert((int)VGUI_EDITOR_GIZMO_PHASE_BEGIN == (int)gizmo_phase::begin,
		"gizmo phase drift");
	static_assert((int)VGUI_EDITOR_GIZMO_PHASE_PREVIEW == (int)gizmo_phase::preview,
		"gizmo phase drift");
	static_assert((int)VGUI_EDITOR_GIZMO_PHASE_COMMIT == (int)gizmo_phase::commit,
		"gizmo phase drift");
	static_assert((int)VGUI_EDITOR_GIZMO_PHASE_CANCEL == (int)gizmo_phase::cancel,
		"gizmo phase drift");

	/* The asserts above pin the enumerator order, so the conversions only clamp
	 * out-of-range C values onto the first enumerator. */
	gizmo_mode to_mode(vgui_editor_gizmo_mode mode)
	{
		if ((int)mode < (int)VGUI_EDITOR_GIZMO_TRANSLATE ||
			(int)mode > (int)VGUI_EDITOR_GIZMO_SCALE)
			mode = VGUI_EDITOR_GIZMO_TRANSLATE;
		return (gizmo_mode)(int)mode;
	}

	vgui_editor_gizmo_mode from_mode(gizmo_mode mode)
	{
		if ((int)mode < (int)gizmo_mode::translate ||
			(int)mode > (int)gizmo_mode::scale)
			return VGUI_EDITOR_GIZMO_TRANSLATE;
		return (vgui_editor_gizmo_mode)(int)mode;
	}

	gizmo_space to_space(vgui_editor_gizmo_space space)
	{
		if ((int)space < (int)VGUI_EDITOR_GIZMO_SPACE_WORLD ||
			(int)space > (int)VGUI_EDITOR_GIZMO_SPACE_SCREEN)
			space = VGUI_EDITOR_GIZMO_SPACE_WORLD;
		return (gizmo_space)(int)space;
	}

	vgui_editor_gizmo_space from_space(gizmo_space space)
	{
		if ((int)space < (int)gizmo_space::world || (int)space > (int)gizmo_space::screen)
			return VGUI_EDITOR_GIZMO_SPACE_WORLD;
		return (vgui_editor_gizmo_space)(int)space;
	}

	vgui_editor_gizmo_handle from_handle(gizmo_handle handle)
	{
		if ((int)handle < (int)gizmo_handle::none ||
			(int)handle > (int)gizmo_handle::scale_uniform)
			return VGUI_EDITOR_GIZMO_HANDLE_NONE;
		return (vgui_editor_gizmo_handle)(int)handle;
	}

	vgui_editor_gizmo_phase from_phase(gizmo_phase phase)
	{
		if ((int)phase < (int)gizmo_phase::begin || (int)phase > (int)gizmo_phase::cancel)
			return VGUI_EDITOR_GIZMO_PHASE_BEGIN;
		return (vgui_editor_gizmo_phase)(int)phase;
	}

	vgui_editor_vec3 to_vec3(const camera_vec3& value)
	{
		vgui_editor_vec3 out;
		out.x = value.x;
		out.y = value.y;
		out.z = value.z;
		return out;
	}

	camera_vec3 from_vec3(const vgui_editor_vec3& value)
	{
		camera_vec3 out;
		out.x = value.x;
		out.y = value.y;
		out.z = value.z;
		return out;
	}

	vgui_editor_vec3 zero_vec3()
	{
		const vgui_editor_vec3 out = { 0.0f, 0.0f, 0.0f };
		return out;
	}

	camera_state to_camera_state(const vgui_editor_camera_state* src)
	{
		camera_state out;
		if (!src)
			return out;
		const vgui_editor_camera_state d = vgeditor::take_desc(src);
		out.target = from_vec3(d.target);
		out.yaw = d.yaw;
		out.pitch = d.pitch;
		out.distance = d.distance;
		out.fov_deg = d.fov_deg;
		out.near_plane = d.near_plane;
		out.far_plane = d.far_plane;
		return out;
	}

	void fill_camera_state(const camera_state& state, vgui_editor_camera_state* out)
	{
		std::memset(out, 0, sizeof(*out));
		out->struct_size = (uint32_t)sizeof(*out);
		out->target = to_vec3(state.target);
		out->yaw = state.yaw;
		out->pitch = state.pitch;
		out->distance = state.distance;
		out->fov_deg = state.fov_deg;
		out->near_plane = state.near_plane;
		out->far_plane = state.far_plane;
	}

	/* Same channels as the canvas adapter reads; the shared default helper lives
	 * in editor_c_api_canvas.cpp. */
	snap_settings to_snap(const vgui_editor_snap_settings* src)
	{
		snap_settings out;
		if (!src)
			return out;
		const vgui_editor_snap_settings d = vgeditor::take_desc(src);
		out.enabled = d.enabled != 0;
		out.translate = d.translate != 0;
		out.rotate = d.rotate != 0;
		out.scale = d.scale != 0;
		out.translate_step = d.translate_step;
		out.rotate_step_deg = d.rotate_step_deg;
		out.scale_step = d.scale_step;
		return out;
	}

	void fill_snap(const snap_settings& snap, vgui_editor_snap_settings* out)
	{
		std::memset(out, 0, sizeof(*out));
		out->struct_size = (uint32_t)sizeof(*out);
		out->enabled = snap.enabled ? 1 : 0;
		out->translate = snap.translate ? 1 : 0;
		out->rotate = snap.rotate ? 1 : 0;
		out->scale = snap.scale ? 1 : 0;
		out->translate_step = snap.translate_step;
		out->rotate_step_deg = snap.rotate_step_deg;
		out->scale_step = snap.scale_step;
	}

	/* Generated lines are read-only output; a shorter caller struct leaves the
	 * trailing fields at zero, which the host reads as an unset colour. */
	void fill_grid_line(const viewport_grid_line& line, vgui_editor_grid_line* out)
	{
		std::memset(out, 0, sizeof(*out));
		out->struct_size = (uint32_t)sizeof(*out);
		out->a = to_vec3(line.a);
		out->b = to_vec3(line.b);
		out->color = line.color;
		out->width = line.width;
	}

	gizmo_frame to_gizmo_frame(const vgui_editor_gizmo_frame* src)
	{
		gizmo_frame out;
		if (!src)
			return out;
		const vgui_editor_gizmo_frame d = vgeditor::take_desc(src);
		out.valid = d.valid != 0;
		out.origin = from_vec3(d.origin);
		for (int i = 0; i < 3; ++i)
			out.axis[i] = from_vec3(d.axis[i]);
		out.screen_length = d.screen_length;
		return out;
	}

	void fill_gizmo_frame(const gizmo_frame& frame, vgui_editor_gizmo_frame* out)
	{
		std::memset(out, 0, sizeof(*out));
		out->struct_size = (uint32_t)sizeof(*out);
		out->valid = frame.valid ? 1 : 0;
		out->origin = to_vec3(frame.origin);
		for (int i = 0; i < 3; ++i)
			out->axis[i] = to_vec3(frame.axis[i]);
		out->screen_length = frame.screen_length;
	}

	void fill_gizmo_transform(const gizmo_transform& transform,
		vgui_editor_gizmo_transform* out)
	{
		std::memset(out, 0, sizeof(*out));
		out->struct_size = (uint32_t)sizeof(*out);
		out->mode = from_mode(transform.mode);
		out->handle = from_handle(transform.handle);
		out->origin = to_vec3(transform.origin);
		for (int i = 0; i < 3; ++i)
			out->axis[i] = to_vec3(transform.axis[i]);
		out->translation = to_vec3(transform.translation);
		out->rotation = to_vec3(transform.rotation);
		out->scale = to_vec3(transform.scale);
	}

	/* One overlay slot: the two drawing handles pass through unchanged, and the
	 * shared owner keeps the C user pointer alive as long as the kit holds the
	 * listener. */
	void set_overlay(vgui_editor_viewport* viewport,
		vgeditor::callback_slot<vgui_editor_overlay_draw_fn>& slot,
		vgui_editor_overlay_draw_fn fn, void* user,
		vgui_editor_release_fn release_user,
		void (viewport_view::*setter)(viewport_view::overlay_draw_fn))
	{
		slot.set(fn, user, release_user);
		if (!fn) {
			(viewport->impl.*setter)(nullptr);
			return;
		}
		vgeditor::user_owner_ptr owner = slot.owner;
		(viewport->impl.*setter)([fn, owner](ovg_ctx_cb* cb, rvg_t* vg, float x,
			float y, float width, float height, float delta_time) {
			fn(owner->user, cb, vg, x, y, width, height, delta_time);
		});
	}

} /* namespace */

extern "C" {

	/* ------------------------------------------------------------------ */
	/* Value types                                                        */
	/* ------------------------------------------------------------------ */

	void vgui_editor_camera_state_default(vgui_editor_camera_state* state)
	{
		if (!state)
			return;
		std::memset(state, 0, sizeof(*state));
		state->struct_size = (uint32_t)sizeof(*state);
		/* The same defaults viewport_camera::state_ starts with. */
		state->target.x = 0.0f;
		state->target.y = 0.0f;
		state->target.z = 0.0f;
		state->yaw = 0.62f;
		state->pitch = 0.47f;
		state->distance = 6.2f;
		state->fov_deg = 50.0f;
		state->near_plane = 0.1f;
		state->far_plane = 1000.0f;
	}

	void vgui_editor_gizmo_frame_default(vgui_editor_gizmo_frame* frame)
	{
		if (!frame)
			return;
		std::memset(frame, 0, sizeof(*frame));
		frame->struct_size = (uint32_t)sizeof(*frame);
		frame->valid = 0;
		frame->origin = zero_vec3();
		frame->axis[0].x = 1.0f;
		frame->axis[0].y = 0.0f;
		frame->axis[0].z = 0.0f;
		frame->axis[1].x = 0.0f;
		frame->axis[1].y = 1.0f;
		frame->axis[1].z = 0.0f;
		frame->axis[2].x = 0.0f;
		frame->axis[2].y = 0.0f;
		frame->axis[2].z = 1.0f;
		frame->screen_length = 0.0f;
	}

	void vgui_editor_gizmo_transform_default(vgui_editor_gizmo_transform* transform)
	{
		if (!transform)
			return;
		std::memset(transform, 0, sizeof(*transform));
		transform->struct_size = (uint32_t)sizeof(*transform);
		transform->mode = VGUI_EDITOR_GIZMO_TRANSLATE;
		transform->handle = VGUI_EDITOR_GIZMO_HANDLE_NONE;
		transform->origin = zero_vec3();
		transform->axis[0].x = 1.0f;
		transform->axis[1].y = 1.0f;
		transform->axis[2].z = 1.0f;
		transform->translation = zero_vec3();
		transform->rotation = zero_vec3();
		/* Neutral scale, matching gizmo_transform's default. */
		transform->scale.x = 1.0f;
		transform->scale.y = 1.0f;
		transform->scale.z = 1.0f;
	}

	/* ------------------------------------------------------------------ */
	/* Viewport surface                                                   */
	/* ------------------------------------------------------------------ */

	vgui_editor_viewport* vgui_editor_viewport_create(vgui_widget* parent)
	{
		if (!parent)
			return nullptr;
		std::unique_ptr<vgui_editor_viewport> viewport(
			vgeditor::own_object<vgui_editor_viewport>(
				new (std::nothrow) viewport_view()));
		if (!viewport)
			return nullptr;
		viewport->impl.build(parent);
		if (!viewport->impl.widget())
			return nullptr;
		return viewport.release();
	}

	void vgui_editor_viewport_destroy(vgui_editor_viewport* viewport)
	{
		/* A shell-borrowed handle must not tear down the shell's surface. */
		if (!viewport || !viewport->owns_object())
			return;
		vgui_widget* widget = viewport->impl.widget();
		viewport->impl.set_resize_listener(nullptr);
		viewport->impl.set_event_listener(nullptr);
		viewport->impl.set_focus_listener(nullptr);
		viewport->impl.set_drop_listener(nullptr);
		viewport->impl.set_selection_overlay(nullptr);
		viewport->impl.set_interaction_overlay(nullptr);
		viewport->impl.set_foreground_overlay(nullptr);
		for (int token : viewport->observer_tokens)
			viewport->impl.remove_event_listener(token);
		viewport->observer_tokens.clear();
		viewport->resize_fn = nullptr;
		viewport->event_fn = nullptr;
		viewport->focus_fn = nullptr;
		viewport->drop_fn = nullptr;
		viewport->selection_overlay.clear();
		viewport->interaction_overlay.clear();
		viewport->foreground_overlay.clear();
		delete viewport;
		if (widget)
			vgui_widget_destroy(widget);
	}

	vgui_widget* vgui_editor_viewport_widget(vgui_editor_viewport* viewport)
	{
		return viewport ? viewport->impl.widget() : nullptr;
	}

	void vgui_editor_viewport_set_texture(vgui_editor_viewport* viewport,
		vgui_texture* texture)
	{
		if (!viewport)
			return;
		viewport->impl.set_texture(texture);
	}

	vgui_texture* vgui_editor_viewport_texture(vgui_editor_viewport* viewport)
	{
		return viewport ? viewport->impl.texture() : nullptr;
	}

	int vgui_editor_viewport_poll_resize(vgui_editor_viewport* viewport)
	{
		if (!viewport)
			return 0;
		return viewport->impl.poll_resize() ? 1 : 0;
	}

	int vgui_editor_viewport_render_width(vgui_editor_viewport* viewport)
	{
		return viewport ? viewport->impl.render_width() : 0;
	}

	int vgui_editor_viewport_render_height(vgui_editor_viewport* viewport)
	{
		return viewport ? viewport->impl.render_height() : 0;
	}

	void vgui_editor_viewport_get_render_size(vgui_editor_viewport* viewport,
		int* width, int* height)
	{
		if (!viewport)
			return;
		viewport->impl.get_render_size(width, height);
	}

	void vgui_editor_viewport_get_content_rect(vgui_editor_viewport* viewport,
		float* x, float* y, float* width, float* height)
	{
		if (!viewport) {
			if (x)
				*x = 0.0f;
			if (y)
				*y = 0.0f;
			if (width)
				*width = 0.0f;
			if (height)
				*height = 0.0f;
			return;
		}
		viewport->impl.get_content_rect(x, y, width, height);
	}

	void vgui_editor_viewport_set_on_resize(vgui_editor_viewport* viewport,
		vgui_editor_viewport_resize_fn fn, void* user)
	{
		if (!viewport)
			return;
		viewport->resize_fn = fn;
		viewport->resize_user = user;
		if (!fn) {
			viewport->impl.set_resize_listener(nullptr);
			return;
		}
		viewport->impl.set_resize_listener([fn, user](int width, int height) {
			fn(user, width, height);
		});
	}

	void vgui_editor_viewport_set_selection_overlay(vgui_editor_viewport* viewport,
		vgui_editor_overlay_draw_fn fn, void* user,
		vgui_editor_release_fn release_user)
	{
		if (!viewport)
			return;
		set_overlay(viewport, viewport->selection_overlay, fn, user, release_user,
			&viewport_view::set_selection_overlay);
	}

	void vgui_editor_viewport_set_interaction_overlay(
		vgui_editor_viewport* viewport, vgui_editor_overlay_draw_fn fn,
		void* user, vgui_editor_release_fn release_user)
	{
		if (!viewport)
			return;
		set_overlay(viewport, viewport->interaction_overlay, fn, user, release_user,
			&viewport_view::set_interaction_overlay);
	}

	void vgui_editor_viewport_set_foreground_overlay(
		vgui_editor_viewport* viewport, vgui_editor_overlay_draw_fn fn,
		void* user, vgui_editor_release_fn release_user)
	{
		if (!viewport)
			return;
		set_overlay(viewport, viewport->foreground_overlay, fn, user, release_user,
			&viewport_view::set_foreground_overlay);
	}

	void vgui_editor_viewport_set_on_event(vgui_editor_viewport* viewport,
		vgui_editor_viewport_event_fn fn, void* user)
	{
		if (!viewport)
			return;
		viewport->event_fn = fn;
		viewport->event_user = user;
		if (!fn) {
			viewport->impl.set_event_listener(nullptr);
			return;
		}
		viewport->impl.set_event_listener([fn, user](const vgui_event* event) -> bool {
			return fn(user, event) != 0;
		});
	}

	int vgui_editor_viewport_add_event_listener(vgui_editor_viewport* viewport,
		vgui_editor_viewport_event_fn fn, void* user)
	{
		if (!viewport || !fn)
			return 0;
		const int token = viewport->impl.add_event_listener(
			[fn, user](const vgui_event* event) -> bool {
				return fn(user, event) != 0;
			});
		if (token)
			viewport->observer_tokens.push_back(token);
		return token;
	}

	void vgui_editor_viewport_remove_event_listener(vgui_editor_viewport* viewport,
		int token)
	{
		if (!viewport)
			return;
		viewport->impl.remove_event_listener(token);
		std::vector<int>& tokens = viewport->observer_tokens;
		tokens.erase(std::remove(tokens.begin(), tokens.end(), token), tokens.end());
	}

	void vgui_editor_viewport_set_on_focus(vgui_editor_viewport* viewport,
		vgui_editor_viewport_focus_fn fn, void* user)
	{
		if (!viewport)
			return;
		viewport->focus_fn = fn;
		viewport->focus_user = user;
		if (!fn) {
			viewport->impl.set_focus_listener(nullptr);
			return;
		}
		viewport->impl.set_focus_listener([fn, user](bool focused) {
			fn(user, focused ? 1 : 0);
		});
	}

	int vgui_editor_viewport_dragging(vgui_editor_viewport* viewport)
	{
		return viewport && viewport->impl.dragging() ? 1 : 0;
	}

	int vgui_editor_viewport_drag_button(vgui_editor_viewport* viewport)
	{
		return viewport ? viewport->impl.drag_button() : 0;
	}

	void vgui_editor_viewport_cancel_interaction(vgui_editor_viewport* viewport)
	{
		if (!viewport)
			return;
		viewport->impl.cancel_interaction();
	}

	int vgui_editor_viewport_focused(vgui_editor_viewport* viewport)
	{
		return viewport && viewport->impl.focused() ? 1 : 0;
	}

	void vgui_editor_viewport_focus(vgui_editor_viewport* viewport)
	{
		if (!viewport)
			return;
		viewport->impl.request_focus();
	}

	void vgui_editor_viewport_set_on_drop(vgui_editor_viewport* viewport,
		vgui_editor_viewport_drop_fn fn, void* user)
	{
		if (!viewport)
			return;
		viewport->drop_fn = fn;
		viewport->drop_user = user;
		if (!fn) {
			viewport->impl.set_drop_listener(nullptr);
			return;
		}
		/* The C view borrows the payload strings, which live on the call stack of
		 * the drag target for exactly as long as the callback runs. */
		viewport->impl.set_drop_listener([fn, user](const drag_payload& payload,
			float x, float y) -> bool {
			std::vector<const char*> file_ptrs;
			vgui_editor_drag_payload view;
			vgeditor::fill_c_payload(&view, payload, file_ptrs);
			return fn(user, &view, x, y) != 0;
		});
	}

	int vgui_editor_viewport_last_drop(vgui_editor_viewport* viewport,
		vgui_editor_drag_payload* out, float* x, float* y)
	{
		if (!viewport || !out)
			return 0;
		const drag_payload& payload = viewport->impl.last_drop();
		if (payload.empty()) {
			std::memset(out, 0, sizeof(*out));
			out->struct_size = (uint32_t)sizeof(*out);
			out->kind = VGUI_EDITOR_DRAG_NONE;
			if (x)
				*x = 0.0f;
			if (y)
				*y = 0.0f;
			return 0;
		}
		std::vector<const char*> file_ptrs;
		vgeditor::fill_c_payload(out, payload, file_ptrs);
		if (x)
			*x = viewport->impl.last_drop_x();
		if (y)
			*y = viewport->impl.last_drop_y();
		return 1;
	}

	/* ------------------------------------------------------------------ */
	/* Orbit camera                                                       */
	/* ------------------------------------------------------------------ */

	vgui_editor_camera* vgui_editor_camera_create(void)
	{
		return vgeditor::own_object<vgui_editor_camera>(
			new (std::nothrow) viewport_camera());
	}

	void vgui_editor_camera_destroy(vgui_editor_camera* camera)
	{
		if (!camera || !camera->owns_object())
			return;
		/* Detaching first removes the observer the camera installed on the
		 * surface, so the viewport never calls into a dead camera. */
		camera->impl.detach();
		camera->impl.set_focus_provider(nullptr);
		camera->impl.set_change_listener(nullptr);
		camera->focus_fn = nullptr;
		camera->change_fn = nullptr;
		delete camera;
	}

	int vgui_editor_camera_attach(vgui_editor_camera* camera,
		vgui_editor_viewport* viewport)
	{
		if (!camera || !viewport)
			return 0;
		camera->impl.attach(viewport->impl);
		return camera->impl.attached() ? 1 : 0;
	}

	void vgui_editor_camera_detach(vgui_editor_camera* camera)
	{
		if (!camera)
			return;
		camera->impl.detach();
	}

	int vgui_editor_camera_attached(vgui_editor_camera* camera)
	{
		return camera && camera->impl.attached() ? 1 : 0;
	}

	void vgui_editor_camera_set_navigation_enabled(vgui_editor_camera* camera,
		int enabled)
	{
		if (!camera)
			return;
		camera->impl.set_navigation_enabled(enabled != 0);
	}

	int vgui_editor_camera_navigation_enabled(vgui_editor_camera* camera)
	{
		if (!camera)
			return 0;
		return camera->impl.navigation_enabled() ? 1 : 0;
	}

	void vgui_editor_camera_set_pan_button(vgui_editor_camera* camera, int button)
	{
		if (!camera)
			return;
		camera->impl.set_pan_button(button);
	}

	void vgui_editor_camera_set_orbit_button(vgui_editor_camera* camera, int button)
	{
		if (!camera)
			return;
		camera->impl.set_orbit_button(button);
	}

	void vgui_editor_camera_set_orbit_sensitivity(vgui_editor_camera* camera,
		float radians_per_pixel)
	{
		if (!camera)
			return;
		camera->impl.set_orbit_sensitivity(radians_per_pixel);
	}

	void vgui_editor_camera_set_zoom_speed(vgui_editor_camera* camera, float speed)
	{
		if (!camera)
			return;
		camera->impl.set_zoom_speed(speed);
	}

	void vgui_editor_camera_set_distance_range(vgui_editor_camera* camera,
		float min_distance, float max_distance)
	{
		if (!camera)
			return;
		camera->impl.set_distance_range(min_distance, max_distance);
	}

	void vgui_editor_camera_set_focus_provider(vgui_editor_camera* camera,
		vgui_editor_camera_focus_fn fn, void* user)
	{
		if (!camera)
			return;
		camera->focus_fn = fn;
		camera->focus_user = user;
		if (!fn) {
			camera->impl.set_focus_provider(nullptr);
			return;
		}
		camera->impl.set_focus_provider([fn, user](camera_focus* out) -> bool {
			if (!out)
				return false;
			vgui_editor_camera_focus view;
			std::memset(&view, 0, sizeof(view));
			view.struct_size = (uint32_t)sizeof(view);
			if (!fn(user, &view))
				return false;
			out->center = from_vec3(view.center);
			out->radius = view.radius;
			return true;
		});
	}

	int vgui_editor_camera_focus_selection(vgui_editor_camera* camera)
	{
		if (!camera)
			return 0;
		return camera->impl.focus_selection() ? 1 : 0;
	}

	int vgui_editor_camera_panning(vgui_editor_camera* camera)
	{
		return camera && camera->impl.panning() ? 1 : 0;
	}

	int vgui_editor_camera_orbiting(vgui_editor_camera* camera)
	{
		return camera && camera->impl.orbiting() ? 1 : 0;
	}

	int vgui_editor_camera_navigating(vgui_editor_camera* camera)
	{
		return camera && camera->impl.navigating() ? 1 : 0;
	}

	void vgui_editor_camera_cancel_navigation(vgui_editor_camera* camera)
	{
		if (!camera)
			return;
		camera->impl.cancel_navigation();
	}

	void vgui_editor_camera_get_state(vgui_editor_camera* camera,
		vgui_editor_camera_state* out)
	{
		if (!camera || !out)
			return;
		fill_camera_state(camera->impl.get_state(), out);
	}

	void vgui_editor_camera_set_state(vgui_editor_camera* camera,
		const vgui_editor_camera_state* state)
	{
		if (!camera || !state)
			return;
		/* The kit clamps pitch, distance, fov and clip planes itself. */
		camera->impl.set_state(to_camera_state(state));
	}

	void vgui_editor_camera_reset(vgui_editor_camera* camera)
	{
		if (!camera)
			return;
		camera->impl.reset();
	}

	vgui_editor_vec3 vgui_editor_camera_position(vgui_editor_camera* camera)
	{
		if (!camera)
			return zero_vec3();
		return to_vec3(camera->impl.position());
	}

	vgui_editor_vec3 vgui_editor_camera_forward(vgui_editor_camera* camera)
	{
		if (!camera)
			return zero_vec3();
		return to_vec3(camera->impl.forward());
	}

	vgui_editor_vec3 vgui_editor_camera_right(vgui_editor_camera* camera)
	{
		if (!camera)
			return zero_vec3();
		return to_vec3(camera->impl.right());
	}

	vgui_editor_vec3 vgui_editor_camera_up(vgui_editor_camera* camera)
	{
		if (!camera)
			return zero_vec3();
		return to_vec3(camera->impl.up());
	}

	void vgui_editor_camera_set_on_change(vgui_editor_camera* camera,
		vgui_editor_camera_change_fn fn, void* user)
	{
		if (!camera)
			return;
		camera->change_fn = fn;
		camera->change_user = user;
		if (!fn) {
			camera->impl.set_change_listener(nullptr);
			return;
		}
		camera->impl.set_change_listener([fn, user]() {
			fn(user);
		});
	}

	/* ------------------------------------------------------------------ */
	/* World grid                                                         */
	/* ------------------------------------------------------------------ */

	vgui_editor_grid* vgui_editor_grid_create(void)
	{
		return vgeditor::own_object<vgui_editor_grid>(
			new (std::nothrow) viewport_grid());
	}

	void vgui_editor_grid_destroy(vgui_editor_grid* grid)
	{
		/* The grid owns no widget and installs nothing, so only the wrapper dies. */
		if (!grid || !grid->owns_object())
			return;
		delete grid;
	}

	void vgui_editor_grid_set_visible(vgui_editor_grid* grid, int visible)
	{
		if (!grid)
			return;
		grid->impl.set_visible(visible != 0);
	}

	int vgui_editor_grid_visible(vgui_editor_grid* grid)
	{
		return grid && grid->impl.visible() ? 1 : 0;
	}

	void vgui_editor_grid_set_auto_density(vgui_editor_grid* grid, int enabled)
	{
		if (!grid)
			return;
		grid->impl.set_auto_density(enabled != 0);
	}

	int vgui_editor_grid_auto_density(vgui_editor_grid* grid)
	{
		if (!grid)
			return 0;
		return grid->impl.auto_density() ? 1 : 0;
	}

	void vgui_editor_grid_set_cell_size(vgui_editor_grid* grid, float cell)
	{
		if (!grid)
			return;
		grid->impl.set_cell_size(cell);
	}

	float vgui_editor_grid_cell_size(vgui_editor_grid* grid)
	{
		if (!grid)
			return 0.0f;
		return grid->impl.cell_size();
	}

	void vgui_editor_grid_set_min_cell_pixels(vgui_editor_grid* grid, float pixels)
	{
		if (!grid)
			return;
		grid->impl.set_min_cell_pixels(pixels);
	}

	float vgui_editor_grid_min_cell_pixels(vgui_editor_grid* grid)
	{
		if (!grid)
			return 0.0f;
		return grid->impl.min_cell_pixels();
	}

	void vgui_editor_grid_set_max_cell_pixels(vgui_editor_grid* grid, float pixels)
	{
		if (!grid)
			return;
		grid->impl.set_max_cell_pixels(pixels);
	}

	float vgui_editor_grid_max_cell_pixels(vgui_editor_grid* grid)
	{
		if (!grid)
			return 0.0f;
		return grid->impl.max_cell_pixels();
	}

	int vgui_editor_grid_half_cell_count(vgui_editor_grid* grid)
	{
		return grid ? grid->impl.half_cell_count() : 0;
	}

	void vgui_editor_grid_set_half_cell_count(vgui_editor_grid* grid, int cells)
	{
		if (!grid)
			return;
		grid->impl.set_half_cell_count(cells);
	}

	int vgui_editor_grid_major_every(vgui_editor_grid* grid)
	{
		return grid ? grid->impl.major_every() : 0;
	}

	void vgui_editor_grid_set_major_every(vgui_editor_grid* grid, int cells)
	{
		if (!grid)
			return;
		grid->impl.set_major_every(cells);
	}

	void vgui_editor_grid_set_plane_height(vgui_editor_grid* grid, float y)
	{
		if (!grid)
			return;
		grid->impl.set_plane_height(y);
	}

	float vgui_editor_grid_plane_height(vgui_editor_grid* grid)
	{
		if (!grid)
			return 0.0f;
		return grid->impl.plane_height();
	}

	void vgui_editor_grid_set_axes_visible(vgui_editor_grid* grid, int visible)
	{
		if (!grid)
			return;
		grid->impl.set_axes_visible(visible != 0);
	}

	int vgui_editor_grid_axes_visible(vgui_editor_grid* grid)
	{
		return grid && grid->impl.axes_visible() ? 1 : 0;
	}

	void vgui_editor_grid_set_axis_length_cells(vgui_editor_grid* grid, float cells)
	{
		if (!grid)
			return;
		grid->impl.set_axis_length_cells(cells);
	}

	void vgui_editor_grid_set_colors(vgui_editor_grid* grid, vgui_color minor,
		vgui_color major, vgui_color axis_x, vgui_color axis_y, vgui_color axis_z)
	{
		if (!grid)
			return;
		grid->impl.set_colors(minor, major, axis_x, axis_y, axis_z);
	}

	float vgui_editor_grid_effective_cell_size(vgui_editor_grid* grid,
		float distance, int viewport_height, float fov_deg)
	{
		if (!grid)
			return 0.0f;
		return grid->impl.effective_cell_size(distance, viewport_height, fov_deg);
	}

	size_t vgui_editor_grid_build(vgui_editor_grid* grid,
		const vgui_editor_camera_state* camera, int viewport_width,
		int viewport_height, vgui_editor_grid_line* out, size_t out_count)
	{
		if (!grid || !camera)
			return 0;
		std::vector<viewport_grid_line> lines;
		if (!grid->impl.build(to_camera_state(camera), viewport_width,
			viewport_height, &lines))
			return 0;
		/* out == NULL asks for the count only; otherwise the caller gets as many
		 * lines as its array can hold. */
		if (!out)
			return lines.size();
		const size_t written = lines.size() < out_count ? lines.size() : out_count;
		for (size_t i = 0; i < written; ++i)
			fill_grid_line(lines[i], out + i);
		return written;
	}

	/* ------------------------------------------------------------------ */
	/* Picking / box selection                                            */
	/* ------------------------------------------------------------------ */

	vgui_editor_picking* vgui_editor_picking_create(void)
	{
		return vgeditor::own_object<vgui_editor_picking>(
			new (std::nothrow) viewport_selection());
	}

	void vgui_editor_picking_destroy(vgui_editor_picking* picking)
	{
		if (!picking || !picking->owns_object())
			return;
		picking->impl.detach();
		picking->impl.set_point_picker(nullptr);
		picking->impl.set_box_picker(nullptr);
		picking->impl.set_highlight_provider(nullptr);
		picking->point_fn = nullptr;
		picking->box_fn = nullptr;
		picking->highlight_fn = nullptr;
		delete picking;
	}

	int vgui_editor_picking_attach(vgui_editor_picking* picking,
		vgui_editor_viewport* viewport, vgui_editor_context* ctx)
	{
		if (!picking || !viewport)
			return 0;
		editor_context* context = vgeditor::context_impl(ctx);
		if (!context)
			return 0;
		picking->impl.attach(viewport->impl, *context);
		return picking->impl.attached() ? 1 : 0;
	}

	void vgui_editor_picking_detach(vgui_editor_picking* picking)
	{
		if (!picking)
			return;
		picking->impl.detach();
	}

	int vgui_editor_picking_attached(vgui_editor_picking* picking)
	{
		return picking && picking->impl.attached() ? 1 : 0;
	}

	void vgui_editor_picking_set_point_picker(vgui_editor_picking* picking,
		vgui_editor_point_pick_fn fn, void* user)
	{
		if (!picking)
			return;
		picking->point_fn = fn;
		picking->point_user = user;
		if (!fn) {
			picking->impl.set_point_picker(nullptr);
			return;
		}
		picking->impl.set_point_picker(
			[fn, user](const selection_point_query& query) -> object_id {
			vgui_editor_pick_point view;
			std::memset(&view, 0, sizeof(view));
			view.struct_size = (uint32_t)sizeof(view);
			view.x = query.x;
			view.y = query.y;
			view.width = query.width;
			view.height = query.height;
			view.mods = query.mods;
			return (object_id)fn(user, &view);
		});
	}

	void vgui_editor_picking_set_box_picker(vgui_editor_picking* picking,
		vgui_editor_box_pick_fn fn, void* user)
	{
		if (!picking)
			return;
		picking->box_fn = fn;
		picking->box_user = user;
		if (!fn) {
			picking->impl.set_box_picker(nullptr);
			return;
		}
		picking->impl.set_box_picker(
			[fn, user](const selection_box_query& query) -> std::vector<object_id> {
			vgui_editor_pick_box view;
			std::memset(&view, 0, sizeof(view));
			view.struct_size = (uint32_t)sizeof(view);
			view.x0 = query.x0;
			view.y0 = query.y0;
			view.x1 = query.x1;
			view.y1 = query.y1;
			view.width = query.width;
			view.height = query.height;
			view.mods = query.mods;

			/* A host that reports more ids than the array holds is asked again with
			 * the exact capacity, so a large box selection is never truncated. */
			std::vector<vgui_editor_object_id> buffer(kPickChunk);
			size_t count = 0;
			for (;;) {
				count = fn(user, &view, buffer.data(), buffer.size());
				if (count <= buffer.size())
					break;
				buffer.resize(count);
			}
			std::vector<object_id> picked;
			picked.reserve(count);
			for (size_t i = 0; i < count; ++i)
				picked.push_back((object_id)buffer[i]);
			return picked;
		});
	}

	void vgui_editor_picking_set_highlight_provider(vgui_editor_picking* picking,
		vgui_editor_highlight_fn fn, void* user)
	{
		if (!picking)
			return;
		picking->highlight_fn = fn;
		picking->highlight_user = user;
		if (!fn) {
			picking->impl.set_highlight_provider(nullptr);
			return;
		}
		picking->impl.set_highlight_provider(
			[fn, user](const std::vector<object_id>& ids) -> std::vector<selection_box> {
			std::vector<vgui_editor_object_id> id_view;
			id_view.reserve(ids.size());
			for (object_id id : ids)
				id_view.push_back((vgui_editor_object_id)id);

			std::vector<vgui_editor_highlight_box> buffer(kHighlightChunk);
			size_t count = 0;
			for (;;) {
				for (size_t i = 0; i < buffer.size(); ++i) {
					std::memset(&buffer[i], 0, sizeof(buffer[i]));
					buffer[i].struct_size = (uint32_t)sizeof(buffer[i]);
				}
				count = fn(user, id_view.empty() ? nullptr : id_view.data(),
					id_view.size(), buffer.data(), buffer.size());
				if (count <= buffer.size())
					break;
				buffer.resize(count);
			}
			std::vector<selection_box> boxes;
			boxes.reserve(count);
			for (size_t i = 0; i < count; ++i) {
				selection_box box;
				box.id = (object_id)buffer[i].id;
				box.x = buffer[i].x;
				box.y = buffer[i].y;
				box.width = buffer[i].width;
				box.height = buffer[i].height;
				boxes.push_back(box);
			}
			return boxes;
		});
	}

	void vgui_editor_picking_set_enabled(vgui_editor_picking* picking, int enabled)
	{
		if (!picking)
			return;
		picking->impl.set_enabled(enabled != 0);
	}

	int vgui_editor_picking_enabled(vgui_editor_picking* picking)
	{
		if (!picking)
			return 0;
		return picking->impl.enabled() ? 1 : 0;
	}

	void vgui_editor_picking_set_multi_select(vgui_editor_picking* picking,
		int enabled)
	{
		if (!picking)
			return;
		picking->impl.set_multi_select(enabled != 0);
	}

	int vgui_editor_picking_multi_select(vgui_editor_picking* picking)
	{
		if (!picking)
			return 0;
		return picking->impl.multi_select() ? 1 : 0;
	}

	void vgui_editor_picking_set_drag_threshold(vgui_editor_picking* picking,
		float pixels)
	{
		if (!picking)
			return;
		picking->impl.set_drag_threshold(pixels);
	}

	int vgui_editor_picking_pending(vgui_editor_picking* picking)
	{
		return picking && picking->impl.pending() ? 1 : 0;
	}

	int vgui_editor_picking_selecting(vgui_editor_picking* picking)
	{
		return picking && picking->impl.selecting() ? 1 : 0;
	}

	void vgui_editor_picking_cancel_selection(vgui_editor_picking* picking)
	{
		if (!picking)
			return;
		picking->impl.cancel_selection();
	}

	size_t vgui_editor_picking_selected_count(vgui_editor_picking* picking)
	{
		return picking ? picking->impl.selected_ids().size() : 0;
	}

	vgui_editor_object_id vgui_editor_picking_selected_at(
		vgui_editor_picking* picking, size_t index)
	{
		if (!picking)
			return VGUI_EDITOR_INVALID_OBJECT;
		const std::vector<object_id>& ids = picking->impl.selected_ids();
		if (index >= ids.size())
			return VGUI_EDITOR_INVALID_OBJECT;
		return (vgui_editor_object_id)ids[index];
	}

	/* ------------------------------------------------------------------ */
	/* Transform gizmo                                                    */
	/* ------------------------------------------------------------------ */

	vgui_editor_gizmo* vgui_editor_gizmo_create(void)
	{
		return vgeditor::own_object<vgui_editor_gizmo>(
			new (std::nothrow) viewport_gizmo());
	}

	void vgui_editor_gizmo_destroy(vgui_editor_gizmo* gizmo)
	{
		if (!gizmo || !gizmo->owns_object())
			return;
		/* detach() drops the foreground overlay and the event observer before the
		 * object dies; the snap pointer must go too. */
		gizmo->impl.detach();
		gizmo->impl.set_frame_provider(nullptr);
		gizmo->impl.set_projection(nullptr);
		gizmo->impl.set_ray_provider(nullptr);
		gizmo->impl.set_transform_handler(nullptr);
		gizmo->impl.set_drag_handler(nullptr);
		gizmo->impl.set_snap_settings(nullptr);
		gizmo->frame_fn = nullptr;
		gizmo->project_fn = nullptr;
		gizmo->ray_fn = nullptr;
		gizmo->transform_fn = nullptr;
		gizmo->drag_fn = nullptr;
		delete gizmo;
	}

	int vgui_editor_gizmo_attach(vgui_editor_gizmo* gizmo,
		vgui_editor_viewport* viewport)
	{
		if (!gizmo || !viewport)
			return 0;
		gizmo->impl.attach(viewport->impl);
		return gizmo->impl.attached() ? 1 : 0;
	}

	void vgui_editor_gizmo_detach(vgui_editor_gizmo* gizmo)
	{
		if (!gizmo)
			return;
		gizmo->impl.detach();
	}

	int vgui_editor_gizmo_attached(vgui_editor_gizmo* gizmo)
	{
		return gizmo && gizmo->impl.attached() ? 1 : 0;
	}

	void vgui_editor_gizmo_set_frame_provider(vgui_editor_gizmo* gizmo,
		vgui_editor_gizmo_frame_fn fn, void* user)
	{
		if (!gizmo)
			return;
		gizmo->frame_fn = fn;
		gizmo->frame_user = user;
		if (!fn) {
			gizmo->impl.set_frame_provider(nullptr);
			return;
		}
		/* A rejected query becomes an invalid frame, which the gizmo draws as
		 * nothing and never hit-tests. */
		gizmo->impl.set_frame_provider([fn, user]() -> gizmo_frame {
			vgui_editor_gizmo_frame view;
			vgui_editor_gizmo_frame_default(&view);
			if (!fn(user, &view))
				return gizmo_frame();
			return to_gizmo_frame(&view);
		});
	}

	void vgui_editor_gizmo_set_projection(vgui_editor_gizmo* gizmo,
		vgui_editor_gizmo_project_fn fn, void* user)
	{
		if (!gizmo)
			return;
		gizmo->project_fn = fn;
		gizmo->project_user = user;
		if (!fn) {
			gizmo->impl.set_projection(nullptr);
			return;
		}
		gizmo->impl.set_projection([fn, user](const camera_vec3& world, float* x,
			float* y) -> bool {
			return fn(user, to_vec3(world), x, y) != 0;
		});
	}

	void vgui_editor_gizmo_set_ray_provider(vgui_editor_gizmo* gizmo,
		vgui_editor_gizmo_ray_fn fn, void* user)
	{
		if (!gizmo)
			return;
		gizmo->ray_fn = fn;
		gizmo->ray_user = user;
		if (!fn) {
			gizmo->impl.set_ray_provider(nullptr);
			return;
		}
		gizmo->impl.set_ray_provider([fn, user](float x, float y,
			gizmo_ray* out) -> bool {
			if (!out)
				return false;
			vgui_editor_gizmo_ray view;
			std::memset(&view, 0, sizeof(view));
			if (!fn(user, x, y, &view))
				return false;
			out->origin = from_vec3(view.origin);
			out->direction = from_vec3(view.direction);
			return true;
		});
	}

	void vgui_editor_gizmo_set_transform_handler(vgui_editor_gizmo* gizmo,
		vgui_editor_gizmo_transform_fn fn, void* user)
	{
		if (!gizmo)
			return;
		gizmo->transform_fn = fn;
		gizmo->transform_user = user;
		if (!fn) {
			gizmo->impl.set_transform_handler(nullptr);
			return;
		}
		gizmo->impl.set_transform_handler([fn, user](gizmo_phase phase,
			const gizmo_transform& transform) {
			vgui_editor_gizmo_transform view;
			fill_gizmo_transform(transform, &view);
			fn(user, from_phase(phase), &view);
		});
	}

	void vgui_editor_gizmo_set_drag_handler(vgui_editor_gizmo* gizmo,
		vgui_editor_gizmo_drag_fn fn, void* user)
	{
		if (!gizmo)
			return;
		gizmo->drag_fn = fn;
		gizmo->drag_user = user;
		if (!fn) {
			gizmo->impl.set_drag_handler(nullptr);
			return;
		}
		/* Legacy translate-only form: rotate/scale gestures simply never reach it. */
		gizmo->impl.set_drag_handler([fn, user](gizmo_phase phase,
			const camera_vec3& delta) {
			fn(user, from_phase(phase), to_vec3(delta));
		});
	}

	void vgui_editor_gizmo_set_snap(vgui_editor_gizmo* gizmo,
		const vgui_editor_snap_settings* settings)
	{
		if (!gizmo)
			return;
		if (!settings) {
			/* NULL detaches the policy: the gizmo snaps nothing, and the handle copy
			 * mirrors that so get_snap reports what is live. */
			gizmo->snap = snap_settings();
			gizmo->snap.enabled = false;
			gizmo->impl.set_snap_settings(nullptr);
			return;
		}
		gizmo->snap = to_snap(settings);
		/* The gizmo reads the struct at drag time, so the handle has to outlive the
		 * install - including for a borrowed handle. */
		gizmo->impl.set_snap_settings(&gizmo->snap);
	}

	void vgui_editor_gizmo_get_snap(vgui_editor_gizmo* gizmo,
		vgui_editor_snap_settings* out)
	{
		if (!gizmo || !out)
			return;
		/* Before the first set_snap the copy still holds the kit defaults. */
		fill_snap(gizmo->snap, out);
	}

	void vgui_editor_gizmo_set_visible(vgui_editor_gizmo* gizmo, int visible)
	{
		if (!gizmo)
			return;
		gizmo->impl.set_visible(visible != 0);
	}

	int vgui_editor_gizmo_visible(vgui_editor_gizmo* gizmo)
	{
		if (!gizmo)
			return 0;
		return gizmo->impl.visible() ? 1 : 0;
	}

	void vgui_editor_gizmo_set_mode(vgui_editor_gizmo* gizmo,
		vgui_editor_gizmo_mode mode)
	{
		if (!gizmo)
			return;
		gizmo->impl.set_mode(to_mode(mode));
	}

	vgui_editor_gizmo_mode vgui_editor_gizmo_mode_of(vgui_editor_gizmo* gizmo)
	{
		if (!gizmo)
			return VGUI_EDITOR_GIZMO_TRANSLATE;
		return from_mode(gizmo->impl.mode());
	}

	void vgui_editor_gizmo_set_space(vgui_editor_gizmo* gizmo,
		vgui_editor_gizmo_space space)
	{
		if (!gizmo)
			return;
		gizmo->impl.set_space(to_space(space));
	}

	vgui_editor_gizmo_space vgui_editor_gizmo_space_of(vgui_editor_gizmo* gizmo)
	{
		if (!gizmo)
			return VGUI_EDITOR_GIZMO_SPACE_WORLD;
		return from_space(gizmo->impl.space());
	}

	void vgui_editor_gizmo_set_screen_length(vgui_editor_gizmo* gizmo, float pixels)
	{
		if (!gizmo)
			return;
		gizmo->impl.set_screen_length(pixels);
	}

	float vgui_editor_gizmo_screen_length(vgui_editor_gizmo* gizmo)
	{
		if (!gizmo)
			return 0.0f;
		return gizmo->impl.screen_length();
	}

	void vgui_editor_gizmo_set_handle_tolerance(vgui_editor_gizmo* gizmo,
		float pixels)
	{
		if (!gizmo)
			return;
		gizmo->impl.set_handle_tolerance(pixels);
	}

	void vgui_editor_gizmo_set_axis_width(vgui_editor_gizmo* gizmo, float pixels)
	{
		if (!gizmo)
			return;
		gizmo->impl.set_axis_width(pixels);
	}

	int vgui_editor_gizmo_dragging(vgui_editor_gizmo* gizmo)
	{
		return gizmo && gizmo->impl.dragging() ? 1 : 0;
	}

	vgui_editor_gizmo_handle vgui_editor_gizmo_active_handle(
		vgui_editor_gizmo* gizmo)
	{
		if (!gizmo)
			return VGUI_EDITOR_GIZMO_HANDLE_NONE;
		return from_handle(gizmo->impl.active_handle());
	}

	vgui_editor_gizmo_handle vgui_editor_gizmo_hovered_handle(
		vgui_editor_gizmo* gizmo)
	{
		if (!gizmo)
			return VGUI_EDITOR_GIZMO_HANDLE_NONE;
		return from_handle(gizmo->impl.hovered_handle());
	}

	void vgui_editor_gizmo_cancel_drag(vgui_editor_gizmo* gizmo)
	{
		if (!gizmo)
			return;
		gizmo->impl.cancel_drag();
	}

	void vgui_editor_gizmo_reset_hover(vgui_editor_gizmo* gizmo)
	{
		if (!gizmo)
			return;
		gizmo->impl.reset_hover();
	}

} /* extern "C" */

namespace vgeditor {

	vgui_editor_viewport* borrow_viewport(vek::viewport_view& viewport)
	{
		return borrow_object<vgui_editor_viewport>(viewport);
	}

	vgui_editor_camera* borrow_camera(vek::viewport_camera& camera)
	{
		return borrow_object<vgui_editor_camera>(camera);
	}

	vgui_editor_grid* borrow_grid(vek::viewport_grid& grid)
	{
		return borrow_object<vgui_editor_grid>(grid);
	}

	vgui_editor_picking* borrow_picking(vek::viewport_selection& selection)
	{
		return borrow_object<vgui_editor_picking>(selection);
	}

	vgui_editor_gizmo* borrow_gizmo(vek::viewport_gizmo& gizmo)
	{
		return borrow_object<vgui_editor_gizmo>(gizmo);
	}

	void release_borrowed_viewport(vgui_editor_viewport* handle)
	{
		/* Wrapper only: a borrowed handle's C++ object belongs to its container. */
		if (handle && !handle->owns_object())
			delete handle;
	}


	void release_borrowed_camera(vgui_editor_camera* handle)
	{
		/* Wrapper only: a borrowed handle's C++ object belongs to its container. */
		if (handle && !handle->owns_object())
			delete handle;
	}


	void release_borrowed_grid(vgui_editor_grid* handle)
	{
		/* Wrapper only: a borrowed handle's C++ object belongs to its container. */
		if (handle && !handle->owns_object())
			delete handle;
	}


	void release_borrowed_picking(vgui_editor_picking* handle)
	{
		/* Wrapper only: a borrowed handle's C++ object belongs to its container. */
		if (handle && !handle->owns_object())
			delete handle;
	}


	void release_borrowed_gizmo(vgui_editor_gizmo* handle)
	{
		/* Wrapper only: a borrowed handle's C++ object belongs to its container. */
		if (handle && !handle->owns_object())
			delete handle;
	}


} /* namespace vgeditor */
