/* gui/editor/editor_c_api_canvas.cpp - C facade for the 2D canvas view declared in <vgui/vgui_editor.h>.
 *
 * The kit borrows the snap policy by pointer and has no getter, so the handle
 * owns one snap_settings copy, installs its address and serves get_snap from
 * that same copy: a borrowed (shell-owned) handle must outlive any snap it
 * installs.  Item labels are copied into handle scratch, so the caller borrows
 * them until the next get_item/find_item.  Callbacks capture the C function
 * pointer and a shared user owner by value - never the handle - so a handle can
 * die before the view still holds the listener. */
#include "editor_c_api.h"
#include "editor_c_api_borrow.h"

#include <new>
#include <string>
#include <vector>

#include <vgui/editor/canvas_view.h>
#include <vgui/editor/viewport_grid.h>

using namespace vgui_editor_kit;

/* -------------------------------------------------------------------------- */
/* Handle storage                                                            */
/* -------------------------------------------------------------------------- */

struct vgui_editor_canvas : vgeditor::c_object<canvas_view> {
	using c_object::c_object;

	/* Borrowed-string storage for the last get_item()/find_item() call. */
	std::string scratch;
	/* Installed into the view by address; the view never copies it. */
	snap_settings snap{};
	vgeditor::callback_slot<vgui_editor_canvas_selection_fn> selection;
	vgeditor::callback_slot<vgui_editor_canvas_change_fn> change;
};

namespace {

	static_assert((int)VGUI_EDITOR_CANVAS_RECTANGLE == (int)canvas_shape::rectangle,
		"canvas shape drift");
	static_assert((int)VGUI_EDITOR_CANVAS_ELLIPSE == (int)canvas_shape::ellipse,
		"canvas shape drift");
	static_assert((int)VGUI_EDITOR_CANVAS_DIAMOND == (int)canvas_shape::diamond,
		"canvas shape drift");

	static_assert((int)VGUI_EDITOR_CANVAS_HANDLE_NONE == (int)canvas_handle::none,
		"canvas handle drift");
	static_assert((int)VGUI_EDITOR_CANVAS_HANDLE_NORTH_WEST ==
		(int)canvas_handle::north_west, "canvas handle drift");
	static_assert((int)VGUI_EDITOR_CANVAS_HANDLE_NORTH == (int)canvas_handle::north,
		"canvas handle drift");
	static_assert((int)VGUI_EDITOR_CANVAS_HANDLE_NORTH_EAST ==
		(int)canvas_handle::north_east, "canvas handle drift");
	static_assert((int)VGUI_EDITOR_CANVAS_HANDLE_EAST == (int)canvas_handle::east,
		"canvas handle drift");
	static_assert((int)VGUI_EDITOR_CANVAS_HANDLE_SOUTH_EAST ==
		(int)canvas_handle::south_east, "canvas handle drift");
	static_assert((int)VGUI_EDITOR_CANVAS_HANDLE_SOUTH == (int)canvas_handle::south,
		"canvas handle drift");
	static_assert((int)VGUI_EDITOR_CANVAS_HANDLE_SOUTH_WEST ==
		(int)canvas_handle::south_west, "canvas handle drift");
	static_assert((int)VGUI_EDITOR_CANVAS_HANDLE_WEST == (int)canvas_handle::west,
		"canvas handle drift");
	static_assert((int)VGUI_EDITOR_CANVAS_HANDLE_ROTATE == (int)canvas_handle::rotate,
		"canvas handle drift");

	static_assert((int)VGUI_EDITOR_CANVAS_GESTURE_NONE == (int)canvas_gesture::none,
		"canvas gesture drift");
	static_assert((int)VGUI_EDITOR_CANVAS_GESTURE_PENDING_MOVE ==
		(int)canvas_gesture::pending_move, "canvas gesture drift");
	static_assert((int)VGUI_EDITOR_CANVAS_GESTURE_MOVE == (int)canvas_gesture::move,
		"canvas gesture drift");
	static_assert((int)VGUI_EDITOR_CANVAS_GESTURE_PENDING_MARQUEE ==
		(int)canvas_gesture::pending_marquee, "canvas gesture drift");
	static_assert((int)VGUI_EDITOR_CANVAS_GESTURE_MARQUEE ==
		(int)canvas_gesture::marquee, "canvas gesture drift");
	static_assert((int)VGUI_EDITOR_CANVAS_GESTURE_RESIZE ==
		(int)canvas_gesture::resize, "canvas gesture drift");
	static_assert((int)VGUI_EDITOR_CANVAS_GESTURE_ROTATE ==
		(int)canvas_gesture::rotate, "canvas gesture drift");
	static_assert((int)VGUI_EDITOR_CANVAS_GESTURE_PAN == (int)canvas_gesture::pan,
		"canvas gesture drift");

	/* The asserts above pin the enumerator order, so the conversions only have to
	 * clamp out-of-range C values onto the first enumerator. */
	canvas_shape to_shape(vgui_editor_canvas_shape shape)
	{
		if ((int)shape < (int)VGUI_EDITOR_CANVAS_RECTANGLE ||
			(int)shape > (int)VGUI_EDITOR_CANVAS_DIAMOND)
			shape = VGUI_EDITOR_CANVAS_RECTANGLE;
		return (canvas_shape)(int)shape;
	}

	vgui_editor_canvas_shape from_shape(canvas_shape shape)
	{
		switch (shape) {
		case canvas_shape::ellipse:
			return VGUI_EDITOR_CANVAS_ELLIPSE;
		case canvas_shape::diamond:
			return VGUI_EDITOR_CANVAS_DIAMOND;
		case canvas_shape::rectangle:
		default:
			return VGUI_EDITOR_CANVAS_RECTANGLE;
		}
	}

	vgui_editor_canvas_handle from_handle(canvas_handle handle)
	{
		if ((int)handle < (int)canvas_handle::none ||
			(int)handle > (int)canvas_handle::rotate)
			return VGUI_EDITOR_CANVAS_HANDLE_NONE;
		return (vgui_editor_canvas_handle)(int)handle;
	}

	vgui_editor_canvas_gesture from_gesture(canvas_gesture gesture)
	{
		if ((int)gesture < (int)canvas_gesture::none ||
			(int)gesture > (int)canvas_gesture::pan)
			return VGUI_EDITOR_CANVAS_GESTURE_NONE;
		return (vgui_editor_canvas_gesture)(int)gesture;
	}

	/* A shorter struct leaves the remaining channels at zero, which the kit reads
	 * as "snap nothing" rather than as an error. */
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

	canvas_item to_item(const vgui_editor_canvas_item* src)
	{
		canvas_item out;
		if (!src)
			return out;
		const vgui_editor_canvas_item d = vgeditor::take_desc(src);
		out.id = (object_id)d.id;
		out.bounds.x = d.bounds.x;
		out.bounds.y = d.bounds.y;
		out.bounds.width = d.bounds.width;
		out.bounds.height = d.bounds.height;
		out.rotation = d.rotation;
		out.shape = to_shape(d.shape);
		out.fill = d.fill;
		out.border = d.border;
		out.text_color = d.text_color;
		out.border_width = d.border_width;
		out.corner_radius = d.corner_radius;
		out.label = vgeditor::to_text(d.label);
		out.visible = d.visible != 0;
		out.locked = d.locked != 0;
		return out;
	}

	void fill_item(vgui_editor_canvas* canvas, const canvas_item& item,
		vgui_editor_canvas_item* out)
	{
		canvas->scratch = item.label;

		std::memset(out, 0, sizeof(*out));
		out->struct_size = (uint32_t)sizeof(*out);
		out->id = (vgui_editor_object_id)item.id;
		out->bounds.x = item.bounds.x;
		out->bounds.y = item.bounds.y;
		out->bounds.width = item.bounds.width;
		out->bounds.height = item.bounds.height;
		out->rotation = item.rotation;
		out->shape = from_shape(item.shape);
		out->fill = item.fill;
		out->border = item.border;
		out->text_color = item.text_color;
		out->border_width = item.border_width;
		out->corner_radius = item.corner_radius;
		out->label = canvas->scratch.c_str();
		out->visible = item.visible ? 1 : 0;
		out->locked = item.locked ? 1 : 0;
	}

	std::vector<object_id> to_ids(const vgui_editor_object_id* ids, size_t count)
	{
		std::vector<object_id> out;
		if (!ids)
			return out;
		out.reserve(count);
		for (size_t i = 0; i < count; ++i)
			out.push_back((object_id)ids[i]);
		return out;
	}

	vgui_editor_canvas_point to_c_point(const canvas_point& point)
	{
		vgui_editor_canvas_point out;
		out.x = point.x;
		out.y = point.y;
		return out;
	}

	vgui_editor_canvas_rect to_c_rect(const canvas_rect& rect)
	{
		vgui_editor_canvas_rect out;
		out.x = rect.x;
		out.y = rect.y;
		out.width = rect.width;
		out.height = rect.height;
		return out;
	}

} /* namespace */

extern "C" {

	void vgui_editor_snap_settings_default(vgui_editor_snap_settings* settings)
	{
		if (!settings)
			return;
		std::memset(settings, 0, sizeof(*settings));
		settings->struct_size = (uint32_t)sizeof(*settings);
		settings->enabled = 1;
		settings->translate = 1;
		settings->rotate = 1;
		settings->scale = 1;
		settings->translate_step = 0.5f;
		settings->rotate_step_deg = 15.0f;
		settings->scale_step = 0.1f;
	}

	void vgui_editor_canvas_item_default(vgui_editor_canvas_item* item)
	{
		if (!item)
			return;
		std::memset(item, 0, sizeof(*item));
		item->struct_size = (uint32_t)sizeof(*item);
		item->id = VGUI_EDITOR_INVALID_OBJECT;
		/* The same presentation defaults the kit canvas_item carries. */
		item->bounds.x = 0.0f;
		item->bounds.y = 0.0f;
		item->bounds.width = 100.0f;
		item->bounds.height = 60.0f;
		item->rotation = 0.0f;
		item->shape = VGUI_EDITOR_CANVAS_RECTANGLE;
		item->fill = 0xFF2E4A6A;
		item->border = 0xFF7FB6FF;
		item->text_color = 0xFFE8EEF8;
		item->border_width = 1.0f;
		item->corner_radius = 0.0f;
		item->label = nullptr;
		item->visible = 1;
		item->locked = 0;
	}

	void vgui_editor_canvas_view_state_default(vgui_editor_canvas_view_state* state)
	{
		if (!state)
			return;
		std::memset(state, 0, sizeof(*state));
		state->struct_size = (uint32_t)sizeof(*state);
		state->offset_x = 0.0f;
		state->offset_y = 0.0f;
		state->zoom = 1.0f;
	}

	/* ------------------------------------------------------------------ */
	/* Lifecycle                                                          */
	/* ------------------------------------------------------------------ */

	vgui_editor_canvas* vgui_editor_canvas_create(vgui_widget* parent)
	{
		if (!parent)
			return nullptr;
		std::unique_ptr<vgui_editor_canvas> canvas(
			vgeditor::own_object<vgui_editor_canvas>(
				new (std::nothrow) canvas_view()));
		if (!canvas)
			return nullptr;
		canvas->impl.build(parent);
		if (!canvas->impl.widget())
			return nullptr;
		return canvas.release();
	}

	void vgui_editor_canvas_destroy(vgui_editor_canvas* canvas)
	{
		/* A shell-borrowed handle must not tear down the shell's canvas. */
		if (!canvas || !canvas->owns_object())
			return;
		vgui_widget* widget = canvas->impl.widget();
		canvas->impl.set_selection_listener(nullptr);
		canvas->impl.set_change_listener(nullptr);
		canvas->impl.set_snap_settings(nullptr);
		canvas->impl.set_command_stack(nullptr);
		canvas->selection.clear();
		canvas->change.clear();
		delete canvas;
		if (widget)
			vgui_widget_destroy(widget);
	}

	vgui_widget* vgui_editor_canvas_widget(vgui_editor_canvas* canvas)
	{
		return canvas ? canvas->impl.widget() : nullptr;
	}

	void vgui_editor_canvas_focus(vgui_editor_canvas* canvas)
	{
		if (!canvas)
			return;
		canvas->impl.request_focus();
	}

	int vgui_editor_canvas_focused(vgui_editor_canvas* canvas)
	{
		if (!canvas)
			return 0;
		return canvas->impl.focused() ? 1 : 0;
	}

	int vgui_editor_canvas_built(vgui_editor_canvas* canvas)
	{
		return canvas && canvas->impl.built() ? 1 : 0;
	}

	void vgui_editor_canvas_set_enabled(vgui_editor_canvas* canvas, int enabled)
	{
		if (!canvas)
			return;
		canvas->impl.set_enabled(enabled != 0);
	}

	int vgui_editor_canvas_enabled(vgui_editor_canvas* canvas)
	{
		if (!canvas)
			return 0;
		return canvas->impl.enabled() ? 1 : 0;
	}

	/* ------------------------------------------------------------------ */
	/* Model                                                              */
	/* ------------------------------------------------------------------ */

	vgui_editor_object_id vgui_editor_canvas_add_item(vgui_editor_canvas* canvas,
		const vgui_editor_canvas_item* item)
	{
		if (!canvas || !item)
			return VGUI_EDITOR_INVALID_OBJECT;
		return (vgui_editor_object_id)canvas->impl.add_item(to_item(item));
	}

	int vgui_editor_canvas_update_item(vgui_editor_canvas* canvas,
		const vgui_editor_canvas_item* item)
	{
		if (!canvas || !item)
			return 0;
		return canvas->impl.update_item(to_item(item)) ? 1 : 0;
	}

	int vgui_editor_canvas_remove_item(vgui_editor_canvas* canvas,
		vgui_editor_object_id id)
	{
		if (!canvas)
			return 0;
		return canvas->impl.remove_item((object_id)id) ? 1 : 0;
	}

	void vgui_editor_canvas_clear_items(vgui_editor_canvas* canvas)
	{
		if (!canvas)
			return;
		canvas->impl.clear_items();
	}

	size_t vgui_editor_canvas_item_count(vgui_editor_canvas* canvas)
	{
		return canvas ? canvas->impl.items().size() : 0;
	}

	int vgui_editor_canvas_get_item(vgui_editor_canvas* canvas, size_t index,
		vgui_editor_canvas_item* out)
	{
		if (!canvas || !out)
			return 0;
		const std::vector<canvas_item>& items = canvas->impl.items();
		if (index >= items.size())
			return 0;
		fill_item(canvas, items[index], out);
		return 1;
	}

	int vgui_editor_canvas_find_item(vgui_editor_canvas* canvas,
		vgui_editor_object_id id, vgui_editor_canvas_item* out)
	{
		if (!canvas || !out)
			return 0;
		const canvas_item* item = canvas->impl.find_item((object_id)id);
		if (!item)
			return 0;
		fill_item(canvas, *item, out);
		return 1;
	}

	/* ------------------------------------------------------------------ */
	/* Selection                                                          */
	/* ------------------------------------------------------------------ */

	void vgui_editor_canvas_set_selection(vgui_editor_canvas* canvas,
		const vgui_editor_object_id* ids, size_t id_count,
		vgui_editor_object_id primary)
	{
		if (!canvas)
			return;
		canvas->impl.set_selection(to_ids(ids, id_count), (object_id)primary);
	}

	void vgui_editor_canvas_clear_selection(vgui_editor_canvas* canvas)
	{
		if (!canvas)
			return;
		canvas->impl.clear_selection();
	}

	size_t vgui_editor_canvas_selection_count(vgui_editor_canvas* canvas)
	{
		return canvas ? canvas->impl.selection_ids().size() : 0;
	}

	vgui_editor_object_id vgui_editor_canvas_selection_id_at(
		vgui_editor_canvas* canvas, size_t index)
	{
		if (!canvas)
			return VGUI_EDITOR_INVALID_OBJECT;
		const std::vector<object_id>& ids = canvas->impl.selection_ids();
		if (index >= ids.size())
			return VGUI_EDITOR_INVALID_OBJECT;
		return (vgui_editor_object_id)ids[index];
	}

	vgui_editor_object_id vgui_editor_canvas_primary_selection(
		vgui_editor_canvas* canvas)
	{
		if (!canvas)
			return VGUI_EDITOR_INVALID_OBJECT;
		return (vgui_editor_object_id)canvas->impl.primary_selection();
	}

	int vgui_editor_canvas_is_selected(vgui_editor_canvas* canvas,
		vgui_editor_object_id id)
	{
		if (!canvas)
			return 0;
		return canvas->impl.is_selected((object_id)id) ? 1 : 0;
	}

	void vgui_editor_canvas_set_multi_select(vgui_editor_canvas* canvas,
		int enabled)
	{
		if (!canvas)
			return;
		canvas->impl.set_multi_select(enabled != 0);
	}

	int vgui_editor_canvas_multi_select(vgui_editor_canvas* canvas)
	{
		if (!canvas)
			return 0;
		return canvas->impl.multi_select() ? 1 : 0;
	}

	void vgui_editor_canvas_set_on_selection(vgui_editor_canvas* canvas,
		vgui_editor_canvas_selection_fn fn, void* user)
	{
		if (!canvas)
			return;
		canvas->selection.set(fn, user, nullptr);
		if (!fn) {
			canvas->impl.set_selection_listener(nullptr);
			return;
		}
		/* The id view lives on this call stack, so the C array is only valid for
		 * the duration of the callback - exactly what the C contract says. */
		vgeditor::user_owner_ptr owner = canvas->selection.owner;
		canvas->impl.set_selection_listener([fn, owner](const std::vector<object_id>& ids) {
			std::vector<vgui_editor_object_id> view;
			view.reserve(ids.size());
			for (object_id id : ids)
				view.push_back((vgui_editor_object_id)id);
			fn(owner->user, view.empty() ? nullptr : view.data(), view.size());
		});
	}

	void vgui_editor_canvas_set_on_change(vgui_editor_canvas* canvas,
		vgui_editor_canvas_change_fn fn, void* user)
	{
		if (!canvas)
			return;
		canvas->change.set(fn, user, nullptr);
		if (!fn) {
			canvas->impl.set_change_listener(nullptr);
			return;
		}
		vgeditor::user_owner_ptr owner = canvas->change.owner;
		canvas->impl.set_change_listener([fn, owner]() {
			fn(owner->user);
		});
	}

	/* ------------------------------------------------------------------ */
	/* View / zoom / pan                                                  */
	/* ------------------------------------------------------------------ */

	void vgui_editor_canvas_get_view_state(vgui_editor_canvas* canvas,
		vgui_editor_canvas_view_state* out)
	{
		if (!canvas || !out)
			return;
		const canvas_view_state state = canvas->impl.view_state();
		std::memset(out, 0, sizeof(*out));
		out->struct_size = (uint32_t)sizeof(*out);
		out->offset_x = state.offset_x;
		out->offset_y = state.offset_y;
		out->zoom = state.zoom;
	}

	void vgui_editor_canvas_set_view_state(vgui_editor_canvas* canvas,
		const vgui_editor_canvas_view_state* state)
	{
		if (!canvas || !state)
			return;
		const vgui_editor_canvas_view_state d = vgeditor::take_desc(state);
		canvas_view_state next;
		next.offset_x = d.offset_x;
		next.offset_y = d.offset_y;
		next.zoom = d.zoom;
		canvas->impl.set_view_state(next);
	}

	float vgui_editor_canvas_zoom(vgui_editor_canvas* canvas)
	{
		return canvas ? canvas->impl.zoom() : 0.0f;
	}

	void vgui_editor_canvas_zoom_at(vgui_editor_canvas* canvas, float content_x,
		float content_y, float factor)
	{
		if (!canvas)
			return;
		canvas->impl.zoom_at(content_x, content_y, factor);
	}

	void vgui_editor_canvas_set_zoom_limits(vgui_editor_canvas* canvas,
		float minimum, float maximum)
	{
		if (!canvas)
			return;
		canvas->impl.set_zoom_limits(minimum, maximum);
	}

	void vgui_editor_canvas_reset_view(vgui_editor_canvas* canvas)
	{
		if (!canvas)
			return;
		canvas->impl.reset_view();
	}

	/* ------------------------------------------------------------------ */
	/* Grid and snap                                                      */
	/* ------------------------------------------------------------------ */

	void vgui_editor_canvas_set_grid_visible(vgui_editor_canvas* canvas,
		int visible)
	{
		if (!canvas)
			return;
		canvas->impl.set_grid_visible(visible != 0);
	}

	int vgui_editor_canvas_grid_visible(vgui_editor_canvas* canvas)
	{
		if (!canvas)
			return 0;
		return canvas->impl.grid_visible() ? 1 : 0;
	}

	void vgui_editor_canvas_set_grid_step(vgui_editor_canvas* canvas, float step)
	{
		if (!canvas)
			return;
		canvas->impl.set_grid_step(step);
	}

	float vgui_editor_canvas_grid_step(vgui_editor_canvas* canvas)
	{
		if (!canvas)
			return 0.0f;
		return canvas->impl.grid_step();
	}

	void vgui_editor_canvas_set_grid_pixel_range(vgui_editor_canvas* canvas,
		float minimum, float maximum)
	{
		if (!canvas)
			return;
		canvas->impl.set_grid_pixel_range(minimum, maximum);
	}

	float vgui_editor_canvas_effective_grid_step(vgui_editor_canvas* canvas)
	{
		if (!canvas)
			return 0.0f;
		return canvas->impl.effective_grid_step();
	}

	void vgui_editor_canvas_set_snap(vgui_editor_canvas* canvas,
		const vgui_editor_snap_settings* settings)
	{
		if (!canvas)
			return;
		if (!settings) {
			/* NULL detaches the policy: the canvas reads no snap at all, so keep
			 * the copy at the kit defaults but report snapping as off. */
			canvas->snap = snap_settings();
			canvas->snap.enabled = false;
			canvas->impl.set_snap_settings(&canvas->snap);
			return;
		}
		canvas->snap = to_snap(settings);
		/* The view holds the address, so the handle has to outlive the install. */
		canvas->impl.set_snap_settings(&canvas->snap);
	}

	void vgui_editor_canvas_get_snap(vgui_editor_canvas* canvas,
		vgui_editor_snap_settings* out)
	{
		if (!canvas || !out)
			return;
		/* Before the first set_snap the copy still holds the kit defaults. */
		fill_snap(canvas->snap, out);
	}

	void vgui_editor_canvas_set_command_stack(vgui_editor_canvas* canvas,
		vgui_editor_command_stack* stack)
	{
		if (!canvas)
			return;
		canvas->impl.set_command_stack(vgeditor::stack_impl(stack));
	}

	/* ------------------------------------------------------------------ */
	/* Coordinates / geometry                                             */
	/* ------------------------------------------------------------------ */

	vgui_editor_canvas_point vgui_editor_canvas_screen_to_canvas(
		vgui_editor_canvas* canvas, float content_x, float content_y)
	{
		if (!canvas) {
			const vgui_editor_canvas_point zero = { 0.0f, 0.0f };
			return zero;
		}
		return to_c_point(canvas->impl.screen_to_canvas(content_x, content_y));
	}

	vgui_editor_canvas_point vgui_editor_canvas_canvas_to_screen(
		vgui_editor_canvas* canvas, float canvas_x, float canvas_y)
	{
		if (!canvas) {
			const vgui_editor_canvas_point zero = { 0.0f, 0.0f };
			return zero;
		}
		return to_c_point(canvas->impl.canvas_to_screen(canvas_x, canvas_y));
	}

	vgui_editor_canvas_rect vgui_editor_canvas_item_bounds(
		vgui_editor_canvas* canvas, vgui_editor_object_id id)
	{
		if (!canvas) {
			const vgui_editor_canvas_rect zero = { 0.0f, 0.0f, 0.0f, 0.0f };
			return zero;
		}
		return to_c_rect(canvas->impl.item_bounds((object_id)id));
	}

	vgui_editor_canvas_rect vgui_editor_canvas_selection_bounds(
		vgui_editor_canvas* canvas)
	{
		if (!canvas) {
			const vgui_editor_canvas_rect zero = { 0.0f, 0.0f, 0.0f, 0.0f };
			return zero;
		}
		return to_c_rect(canvas->impl.selection_bounds());
	}

	vgui_editor_object_id vgui_editor_canvas_hit_test(vgui_editor_canvas* canvas,
		float content_x, float content_y)
	{
		if (!canvas)
			return VGUI_EDITOR_INVALID_OBJECT;
		return (vgui_editor_object_id)canvas->impl.hit_test(content_x, content_y);
	}

	vgui_editor_canvas_handle vgui_editor_canvas_handle_at(
		vgui_editor_canvas* canvas, float content_x, float content_y)
	{
		if (!canvas)
			return VGUI_EDITOR_CANVAS_HANDLE_NONE;
		return from_handle(canvas->impl.handle_at(content_x, content_y));
	}

	/* ------------------------------------------------------------------ */
	/* Interaction state                                                  */
	/* ------------------------------------------------------------------ */

	int vgui_editor_canvas_dragging(vgui_editor_canvas* canvas)
	{
		return canvas && canvas->impl.dragging() ? 1 : 0;
	}

	vgui_editor_canvas_gesture vgui_editor_canvas_get_gesture(
		vgui_editor_canvas* canvas)
	{
		if (!canvas)
			return VGUI_EDITOR_CANVAS_GESTURE_NONE;
		return from_gesture(canvas->impl.gesture());
	}

	vgui_editor_canvas_handle vgui_editor_canvas_active_handle(
		vgui_editor_canvas* canvas)
	{
		if (!canvas)
			return VGUI_EDITOR_CANVAS_HANDLE_NONE;
		return from_handle(canvas->impl.active_handle());
	}

	vgui_editor_canvas_handle vgui_editor_canvas_hovered_handle(
		vgui_editor_canvas* canvas)
	{
		if (!canvas)
			return VGUI_EDITOR_CANVAS_HANDLE_NONE;
		return from_handle(canvas->impl.hovered_handle());
	}

	void vgui_editor_canvas_cancel_gesture(vgui_editor_canvas* canvas)
	{
		if (!canvas)
			return;
		canvas->impl.cancel_gesture();
	}

	void vgui_editor_canvas_set_drag_threshold(vgui_editor_canvas* canvas,
		float pixels)
	{
		if (!canvas)
			return;
		canvas->impl.set_drag_threshold(pixels);
	}

	float vgui_editor_canvas_drag_threshold(vgui_editor_canvas* canvas)
	{
		if (!canvas)
			return 0.0f;
		return canvas->impl.drag_threshold();
	}

	void vgui_editor_canvas_set_min_item_size(vgui_editor_canvas* canvas,
		float size)
	{
		if (!canvas)
			return;
		canvas->impl.set_min_item_size(size);
	}

} /* extern "C" */

namespace vgeditor {

	vgui_editor_canvas* borrow_canvas(vek::canvas_view& canvas)
	{
		return borrow_object<vgui_editor_canvas>(canvas);
	}

	void release_borrowed_canvas(vgui_editor_canvas* handle)
	{
		/* Wrapper only: a borrowed handle's C++ object belongs to its container. */
		if (handle && !handle->owns_object())
			delete handle;
	}


} /* namespace vgeditor */
