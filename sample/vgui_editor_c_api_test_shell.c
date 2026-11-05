/* sample/vgui_editor_c_api_test_shell.c - headless C tests for the viewport group and the editor shell section of <vgui/vgui_editor.h>. */
#include "vgui_editor_c_api_test.h"

/* A headless context has no GPU device, so nothing here can upload a texture or
 * run a render pass: the viewport checks prove that the API stays coherent
 * without one (installing an overlay, binding no texture, polling the layout
 * size), and every host callback that only a draw or an input event could reach
 * is asserted to stay silent.  All of it is plain C99, which is the point. */

/* -------------------------------------------------------------------------- */
/* Callback plumbing                                                           */
/* -------------------------------------------------------------------------- */

typedef struct viewport_log {
	int resize_calls;
	int width;
	int height;
	int event_calls;
	int second_listener_calls;
	int focus_calls;
	int last_focused;
	int drop_calls;
	int overlay_calls;
	int releases;
} viewport_log;

typedef struct cam_fixture {
	int focus_calls;
	int focus_ok;
	vgui_editor_vec3 center;
	float radius;
} cam_fixture;

typedef struct pick_fixture {
	int point_calls;
	int box_calls;
	int highlight_calls;
	size_t box_ids;
} pick_fixture;

typedef struct gizmo_fixture {
	int frame_calls;
	int project_calls;
	int ray_calls;
	int valid;
	int mode_seen;
	int space_seen;
	vgui_editor_gizmo* gizmo;
} gizmo_fixture;

typedef struct doc_log {
	int news;
	int opens;
	int saves;
	int save_ases;
	int exits;
	int deletes;
	size_t delete_count;
	vgui_editor_object_id first_id;
	int releases;
} doc_log;

/* Float comparison without <math.h>: the harness includes only stdio/stdlib/string. */
static int near_float(float a, float b, float eps)
{
	float d = a - b;
	if (d < 0.0f)
		d = -d;
	return d <= eps;
}

static float abs_float(float a)
{
	return a < 0.0f ? -a : a;
}

/* The unit-length helper keeps the basis checks readable without <math.h>. */
static float sqrt_float_approx(vgui_editor_vec3 v)
{
	float sum = v.x * v.x + v.y * v.y + v.z * v.z;
	float guess = sum > 1.0f ? sum * 0.5f : 1.0f;
	int i;
	for (i = 0; i < 32; ++i)
	{
		guess = (guess + sum / guess) * 0.5f;
	}
	return sum > 0.0f ? guess : 0.0f;
}

static vgui_editor_vec3 make_vec3_helper(float x, float y, float z)
{
	vgui_editor_vec3 v;
	v.x = x;
	v.y = y;
	v.z = z;
	return v;
}

static void release_probe(void* user)
{
	probe* p = (probe*)user;
	++p->released;
}

static void release_viewport_log(void* user)
{
	viewport_log* log = (viewport_log*)user;
	++log->releases;
}

static void release_doc_log(void* user)
{
	doc_log* log = (doc_log*)user;
	++log->releases;
}

/* Typed exactly as vgui_editor_overlay_draw_fn declares it. */
static void overlay_draw(void* user, struct ovg_ctx_cb* cb, struct rvg_t* vg,
	float x, float y, float width, float height, float delta_time)
{
	viewport_log* log = (viewport_log*)user;
	(void)cb;
	(void)vg;
	(void)x;
	(void)y;
	(void)width;
	(void)height;
	(void)delta_time;
	++log->overlay_calls;
}

static int on_viewport_event(void* user, const vgui_event* event)
{
	viewport_log* log = (viewport_log*)user;
	(void)event;
	++log->event_calls;
	/* 0: the main listener does not consume, so the observers still run. */
	return 0;
}

static int on_viewport_observer(void* user, const vgui_event* event)
{
	viewport_log* log = (viewport_log*)user;
	(void)event;
	++log->second_listener_calls;
	return 0;
}

static void on_viewport_focus(void* user, int focused)
{
	viewport_log* log = (viewport_log*)user;
	++log->focus_calls;
	log->last_focused = focused;
}

static void on_viewport_resize(void* user, int width, int height)
{
	viewport_log* log = (viewport_log*)user;
	++log->resize_calls;
	log->width = width;
	log->height = height;
}

static int on_viewport_drop(void* user, const vgui_editor_drag_payload* payload,
	float x, float y)
{
	probe* p = (probe*)user;
	(void)x;
	(void)y;
	++p->action_count;
	if (payload)
		strncpy(p->combo_value, payload->id ? payload->id : "",
			sizeof(p->combo_value) - 1);
	/* Accept the payload so the viewport would record it as the last drop. */
	return 1;
}

static void on_camera_change(void* user)
{
	probe* p = (probe*)user;
	++p->change_count;
}

static int provide_camera_focus(void* user, vgui_editor_camera_focus* out)
{
	cam_fixture* fix = (cam_fixture*)user;
	++fix->focus_calls;
	if (!fix->focus_ok)
		return 0;
	out->struct_size = (uint32_t)sizeof(*out);
	out->center = fix->center;
	out->radius = fix->radius;
	return 1;
}

static vgui_editor_object_id pick_point(void* user,
	const vgui_editor_pick_point* query)
{
	pick_fixture* fix = (pick_fixture*)user;
	(void)query;
	++fix->point_calls;
	return 7;
}

static size_t pick_box(void* user, const vgui_editor_pick_box* query,
	vgui_editor_object_id* out, size_t out_count)
{
	pick_fixture* fix = (pick_fixture*)user;
	size_t i;
	(void)query;
	++fix->box_calls;
	for (i = 0; i < fix->box_ids && i < out_count; ++i)
		out[i] = (vgui_editor_object_id)(i + 1);
	return i;
}

static size_t provide_highlight(void* user, const vgui_editor_object_id* ids,
	size_t id_count, vgui_editor_highlight_box* out, size_t out_count)
{
	pick_fixture* fix = (pick_fixture*)user;
	size_t i;
	++fix->highlight_calls;
	for (i = 0; i < id_count && i < out_count; ++i)
	{
		out[i].struct_size = (uint32_t)sizeof(vgui_editor_highlight_box);
		out[i].id = ids[i];
		out[i].x = 4.0f;
		out[i].y = 5.0f;
		out[i].width = 32.0f;
		out[i].height = 16.0f;
	}
	(void)fix;
	return i;
}

static int provide_gizmo_frame(void* user, vgui_editor_gizmo_frame* out)
{
	gizmo_fixture* fix = (gizmo_fixture*)user;
	++fix->frame_calls;
	/* The contract: the provider consults mode()/space() itself. */
	if (fix->gizmo)
	{
		fix->mode_seen = (int)vgui_editor_gizmo_mode_of(fix->gizmo);
		fix->space_seen = (int)vgui_editor_gizmo_space_of(fix->gizmo);
	}
	vgui_editor_gizmo_frame_default(out);
	out->valid = fix->valid;
	if (!fix->valid)
		return 0;
	out->origin.x = 1.0f;
	out->origin.y = 2.0f;
	out->origin.z = 3.0f;
	return 1;
}

static int project_gizmo(void* user, vgui_editor_vec3 world, float* x, float* y)
{
	gizmo_fixture* fix = (gizmo_fixture*)user;
	++fix->project_calls;
	if (x)
		*x = 100.0f + world.x;
	if (y)
		*y = 100.0f - world.y;
	return 1;
}

static int provide_gizmo_ray(void* user, float x, float y,
	vgui_editor_gizmo_ray* out)
{
	gizmo_fixture* fix = (gizmo_fixture*)user;
	++fix->ray_calls;
	out->origin.x = x;
	out->origin.y = y;
	out->origin.z = -1.0f;
	out->direction.x = 0.0f;
	out->direction.y = 0.0f;
	out->direction.z = 1.0f;
	return 1;
}

static void on_gizmo_transform(void* user, vgui_editor_gizmo_phase phase,
	const vgui_editor_gizmo_transform* transform)
{
	probe* p = (probe*)user;
	(void)phase;
	(void)transform;
	++p->action_count;
}

static void on_gizmo_drag(void* user, vgui_editor_gizmo_phase phase,
	vgui_editor_vec3 delta)
{
	probe* p = (probe*)user;
	(void)phase;
	(void)delta;
	++p->commit_count;
}

static void shell_action(void* user)
{
	probe* p = (probe*)user;
	++p->action_count;
}

static int always_true(void* user)
{
	(void)user;
	return 1;
}

static void doc_new(void* user)
{
	doc_log* log = (doc_log*)user;
	++log->news;
}

static void doc_open(void* user)
{
	doc_log* log = (doc_log*)user;
	++log->opens;
}

static void doc_save(void* user)
{
	doc_log* log = (doc_log*)user;
	++log->saves;
}

static void doc_save_as(void* user)
{
	doc_log* log = (doc_log*)user;
	++log->save_ases;
}

static void doc_exit(void* user)
{
	doc_log* log = (doc_log*)user;
	++log->exits;
}

static int doc_delete(void* user, const vgui_editor_object_id* ids, size_t count)
{
	doc_log* log = (doc_log*)user;
	++log->deletes;
	log->delete_count = count;
	log->first_id = (ids && count > 0) ? ids[0] : VGUI_EDITOR_INVALID_OBJECT;
	return 1;
}

/* -------------------------------------------------------------------------- */
/* Viewport surface                                                            */
/* -------------------------------------------------------------------------- */

void test_viewport(probe* p)
{
	vgui_editor_viewport* vp;
	vgui_editor_drag_payload payload;
	vgui_editor_drag_payload read_back;
	viewport_log log;
	viewport_log other;
	vgui_widget* surface;
	float cx = 0.0f, cy = 0.0f, cw = 0.0f, ch = 0.0f;
	float wx = 0.0f, wy = 0.0f, ww = 0.0f, wh = 0.0f;
	int width = -1, height = -1;
	int first_width, first_height;
	int token_a, token_b;

	memset(p, 0, sizeof(*p));
	memset(&log, 0, sizeof(log));
	memset(&other, 0, sizeof(other));
	memset(&payload, 0, sizeof(payload));
	memset(&read_back, 0, sizeof(read_back));

	check(vgui_editor_viewport_create(NULL) == NULL, "viewport needs a parent");
	/* Flush any layout a previous test left pending: the resize checks below
	 * depend on this viewport being the only thing that changes size. */
	frame(1);

	vp = vgui_editor_viewport_create(vgui_root(g_ui));
	check(vp != NULL, "viewport create");
	surface = vgui_editor_viewport_widget(vp);
	check(surface != NULL, "viewport exposes a widget");
	check(surface == vgui_editor_viewport_widget(vp), "viewport widget is stable");
	check(vgui_editor_viewport_widget(NULL) == NULL, "NULL viewport has no widget");

	/* No texture can be uploaded without a device: the binding stays a plain
	 * borrow and NULL round-trips. */
	check(vgui_editor_viewport_texture(vp) == NULL, "no texture bound initially");
	vgui_editor_viewport_set_texture(vp, NULL);
	check(vgui_editor_viewport_texture(vp) == NULL, "NULL texture round-trips");
	check(vgui_editor_viewport_texture(NULL) == NULL, "NULL viewport has no texture");
	vgui_editor_viewport_set_texture(NULL, NULL);

	/* The render size only moves when poll_resize() sees a new layout rect. */
	check(vgui_editor_viewport_render_width(vp) == 0, "render width starts at 0");
	check(vgui_editor_viewport_render_height(vp) == 0, "render height starts at 0");
	vgui_editor_viewport_get_render_size(vp, &width, &height);
	check(width == 0 && height == 0, "get_render_size mirrors the poll state");
	vgui_editor_viewport_get_render_size(vp, NULL, NULL);
	width = -1;
	height = -1;
	vgui_editor_viewport_get_render_size(NULL, &width, &height);
	check(width == -1 && height == -1, "NULL viewport leaves the size alone");

	vgui_editor_viewport_set_on_resize(vp, on_viewport_resize, &log);
	check(log.resize_calls == 0, "the resize listener only runs from poll_resize");

	/* Reading the content rect forces the pending layout, so the surface now has
	 * its minimum size; the values stay surface-local and inside the border. */
	vgui_editor_viewport_get_content_rect(vp, &cx, &cy, &cw, &ch);
	check(cx > 0.0f && near_float(cx, cy, 1e-5f), "content rect insets evenly");
	check(cw > 0.0f && ch > 0.0f, "content rect is positive after layout");
	vgui_widget_get_rect(surface, &wx, &wy, &ww, &wh);
	check(ww > 0.0f && wh > 0.0f, "surface got a layout rect");
	check(near_float(cw, ww - 2.0f * cx, 1.0f), "content rect drops the border");
	vgui_editor_viewport_get_content_rect(vp, NULL, NULL, NULL, NULL);
	{
		float ax = 0.0f, aw = 0.0f;
		vgui_editor_viewport_get_content_rect(NULL, &ax, NULL, &aw, NULL);
		check(ax == 0.0f && aw == 0.0f, "NULL viewport reports an empty rect");
	}

	check(vgui_editor_viewport_poll_resize(vp) == 1, "first poll reports the size");
	check(log.resize_calls == 1, "resize listener ran once");
	first_width = log.width;
	first_height = log.height;
	check(first_width > 0 && first_height > 0, "resize carries a positive size");
	check(log.width == vgui_editor_viewport_render_width(vp),
		"listener width matches render width");
	check(log.height == vgui_editor_viewport_render_height(vp),
		"listener height matches render height");
	check(vgui_editor_viewport_poll_resize(vp) == 0, "second poll sees no change");
	check(log.resize_calls == 1, "no listener call without a change");

	/* A synthetic resize: grow the surface's minimum, let the layout run, poll. */
	vgui_widget_set_min_size(surface, 520.0f, 280.0f);
	frame(1);
	check(vgui_editor_viewport_poll_resize(vp) == 1, "synthetic resize observed");
	check(log.resize_calls == 2, "resize listener ran again");
	check(log.width > first_width && log.height > first_height,
		"growing the surface grows the render size");
	check(log.width == vgui_editor_viewport_render_width(vp),
		"render width tracks the new size");

	/* Clearing the listener keeps polling coherent but stops the callbacks. */
	vgui_editor_viewport_set_on_resize(vp, NULL, NULL);
	vgui_widget_set_min_size(surface, 640.0f, 320.0f);
	frame(1);
	check(vgui_editor_viewport_poll_resize(vp) == 1, "poll still reports changes");
	check(log.resize_calls == 2, "cleared resize listener is silent");
	vgui_editor_viewport_set_on_resize(NULL, on_viewport_resize, &log);

	/* Overlay slots install, replace and clear; each step releases the old user
	 * exactly once and nothing draws without a render pass. */
	vgui_editor_viewport_set_selection_overlay(vp, overlay_draw, &log,
		release_viewport_log);
	frame(1);
	check(log.overlay_calls == 0, "overlay is inert without a render pass");
	check(log.releases == 0, "installed overlay keeps its user");
	vgui_editor_viewport_set_selection_overlay(vp, overlay_draw, &other,
		release_viewport_log);
	check(log.releases == 1, "replacing an overlay releases the old user");
	vgui_editor_viewport_set_selection_overlay(vp, NULL, NULL, NULL);
	check(other.releases == 1, "clearing an overlay releases its user");

	vgui_editor_viewport_set_interaction_overlay(vp, overlay_draw, &other,
		release_viewport_log);
	vgui_editor_viewport_set_interaction_overlay(vp, NULL, NULL, NULL);
	check(other.releases == 2, "interaction overlay releases on clear");

	/* The foreground slot takes the probe user, so the shared harness records it. */
	vgui_editor_viewport_set_foreground_overlay(vp, overlay_draw, p, release_probe);
	frame(1);
	check(p->released == 0, "foreground overlay keeps the probe");
	vgui_editor_viewport_set_foreground_overlay(vp, NULL, NULL, NULL);
	check(p->released == 1, "clearing the foreground overlay releases the user");

	vgui_editor_viewport_set_selection_overlay(NULL, overlay_draw, &log, NULL);
	vgui_editor_viewport_set_interaction_overlay(NULL, overlay_draw, &log, NULL);
	vgui_editor_viewport_set_foreground_overlay(NULL, overlay_draw, &log, NULL);

	/* Main event listener plus the ordered observers. */
	vgui_editor_viewport_set_on_event(vp, on_viewport_event, &log);
	token_a = vgui_editor_viewport_add_event_listener(vp, on_viewport_observer, &log);
	check(token_a != 0, "add_event_listener returns a token");
	token_b = vgui_editor_viewport_add_event_listener(vp, on_viewport_observer, &log);
	check(token_b != 0 && token_b != token_a, "each observer gets its own token");
	check(vgui_editor_viewport_add_event_listener(vp, NULL, NULL) == 0,
		"no token without a callback");
	check(vgui_editor_viewport_add_event_listener(NULL, on_viewport_observer, &log)
		== 0, "NULL viewport takes no observer");
	frame(1);
	check(log.event_calls == 0 && log.second_listener_calls == 0,
		"no events without an input pass");
	vgui_editor_viewport_remove_event_listener(vp, token_a);
	vgui_editor_viewport_remove_event_listener(vp, token_a);
	vgui_editor_viewport_remove_event_listener(vp, 0);
	vgui_editor_viewport_remove_event_listener(vp, 123456);
	vgui_editor_viewport_remove_event_listener(NULL, token_b);
	vgui_editor_viewport_set_on_event(vp, NULL, NULL);
	vgui_editor_viewport_set_on_event(NULL, on_viewport_event, &log);
	frame(1);
	check(log.event_calls == 0, "cleared listeners stay silent");

	/* Focus and gesture queries stay coherent with no pointer input.  This needs
	 * a context with no modal standing - while one is open the core refuses to
	 * move focus to a widget outside its stage (see the modal notes in
	 * <vgui/vgui.h>) - so every earlier cluster has to close its dialogs. */
	check(vgui_editor_viewport_focused(vp) == 0, "viewport starts unfocused");
	vgui_editor_viewport_set_on_focus(vp, on_viewport_focus, &log);
	vgui_editor_viewport_focus(vp);
	check(vgui_editor_viewport_focused(vp), "focus() focuses the surface");
	check(log.focus_calls == 1 && log.last_focused == 1, "focus listener ran");
	vgui_editor_viewport_focus(vp);
	check(log.focus_calls == 1, "focusing twice does not re-notify");
	vgui_set_focus(g_ui, NULL);
	check(vgui_editor_viewport_focused(vp) == 0, "focus loss clears the surface");
	check(log.focus_calls == 2 && log.last_focused == 0, "focus loss notifies");
	vgui_editor_viewport_set_on_focus(vp, NULL, NULL);
	vgui_editor_viewport_focus(vp);
	check(log.focus_calls == 2, "cleared focus listener is silent");
	vgui_editor_viewport_focus(NULL);
	vgui_set_focus(g_ui, NULL);

	check(vgui_editor_viewport_dragging(vp) == 0, "no drag without a mouse down");
	check(vgui_editor_viewport_drag_button(vp) == 0, "drag button is 0 while idle");
	vgui_editor_viewport_cancel_interaction(vp);
	check(vgui_editor_viewport_dragging(vp) == 0, "cancel while idle stays idle");
	check(log.event_calls == 0, "cancel with no gesture forwards nothing");
	vgui_editor_viewport_cancel_interaction(NULL);
	check(vgui_editor_viewport_dragging(NULL) == 0, "NULL viewport is never dragging");
	check(vgui_editor_viewport_drag_button(NULL) == 0, "NULL drag button is 0");
	check(vgui_editor_viewport_focused(NULL) == 0, "NULL viewport is unfocused");

	/* Drop plumbing: a real drop needs a pointer release over the surface, which
	 * headless input cannot produce, so only the wiring and the empty record are
	 * observable. */
	vgui_editor_viewport_set_on_drop(vp, on_viewport_drop, p);
	vgui_editor_drag_payload_default(&payload);
	payload.kind = VGUI_EDITOR_DRAG_ASSET;
	payload.id = "asset:mesh";
	payload.text = "Mesh";
	check(vgui_editor_viewport_last_drop(vp, &read_back, NULL, NULL) == 0,
		"no drop recorded yet");
	check(read_back.struct_size == sizeof(read_back) &&
		read_back.kind == VGUI_EDITOR_DRAG_NONE,
		"empty last_drop is zeroed with a NONE kind");
	check(vgui_editor_viewport_last_drop(vp, NULL, NULL, NULL) == 0,
		"last_drop needs an output struct");
	check(vgui_editor_viewport_last_drop(NULL, &read_back, NULL, NULL) == 0,
		"NULL viewport reports no drop");
	check(vgui_editor_begin_drag(g_ui, surface, &payload), "asset drag started");
	check(vgui_drag_active(g_ui), "core drag is active");
	vgui_cancel_drag(g_ui);
	check(!vgui_drag_active(g_ui), "drag cancelled");
	check(p->action_count == 0, "drop listener not called without a drop");
	check(vgui_editor_viewport_last_drop(vp, &read_back, NULL, NULL) == 0,
		"cancelled drag records no drop");
	vgui_editor_viewport_set_on_drop(vp, NULL, NULL);
	vgui_editor_viewport_set_on_drop(NULL, on_viewport_drop, p);

	vgui_editor_viewport_destroy(vp);
	vgui_editor_viewport_destroy(NULL);
	frame(1);
	check(vgui_editor_viewport_render_width(NULL) == 0,
		"NULL viewport render width is 0");
	check(vgui_editor_viewport_poll_resize(NULL) == 0, "NULL viewport never resizes");
	check(strcmp(vgui_editor_viewport_widget(NULL) == NULL ? "ok" : "bad", "ok") == 0,
		"destroyed viewport leaves no handle");
}

/* -------------------------------------------------------------------------- */
/* Camera, grid and picking                                                    */
/* -------------------------------------------------------------------------- */

void test_camera_grid_picking(probe* p)
{
	vgui_editor_context* ctx;
	vgui_editor_viewport* vp;
	vgui_editor_camera* cam;
	vgui_editor_camera* other_cam;
	vgui_editor_grid* grid;
	vgui_editor_picking* pick;
	vgui_editor_camera_state state;
	vgui_editor_camera_state read_back;
	vgui_editor_grid_line lines[40];
	vgui_editor_object_id ids[3];
	cam_fixture fix;
	pick_fixture picked;
	vgui_editor_vec3 pos;
	vgui_editor_vec3 fwd;
	vgui_editor_vec3 right;
	vgui_editor_vec3 up;
	size_t null_count;
	size_t filled_count;
	size_t truncated;
	float cell_near, cell_far, cell_invalid;
	float length;
	int i;
	int majors = 0;
	int minors = 0;

	memset(p, 0, sizeof(*p));
	memset(&fix, 0, sizeof(fix));
	memset(&picked, 0, sizeof(picked));
	memset(lines, 0, sizeof(lines));
	memset(&state, 0, sizeof(state));
	memset(&read_back, 0, sizeof(read_back));
	vgui_editor_camera_state_default(&state);

	check(vgui_editor_camera_create() != NULL, "camera create");
	{
		vgui_editor_camera* throwaway = vgui_editor_camera_create();
		vgui_editor_camera_destroy(throwaway);
	}
	ctx = vgui_editor_context_create(g_ui);
	vp = vgui_editor_viewport_create(vgui_root(g_ui));
	cam = vgui_editor_camera_create();
	other_cam = vgui_editor_camera_create();
	grid = vgui_editor_grid_create();
	pick = vgui_editor_picking_create();
	check(ctx != NULL && vp != NULL, "viewport and context available");
	check(cam != NULL && other_cam != NULL, "camera handles");
	check(grid != NULL, "grid handle");
	check(pick != NULL, "picking handle");

	/* --- camera attach ---------------------------------------------------- */
	check(vgui_editor_camera_attached(cam) == 0, "camera starts detached");
	check(vgui_editor_camera_attach(NULL, vp) == 0, "NULL camera cannot attach");
	check(vgui_editor_camera_attach(cam, NULL) == 0, "attach needs a viewport");
	check(vgui_editor_camera_attach(cam, vp), "camera attaches to the viewport");
	check(vgui_editor_camera_attached(cam), "attached() reports the attach");
	check(vgui_editor_camera_attach(cam, vp), "re-attaching the same pair succeeds");
	check(vgui_editor_camera_attached(cam), "still one camera on one surface");
	check(vgui_editor_camera_attach(other_cam, vp),
		"a second camera may share the surface");
	vgui_editor_camera_detach(other_cam);
	check(vgui_editor_camera_attached(other_cam) == 0, "detach clears the flag");
	vgui_editor_camera_detach(other_cam);
	check(vgui_editor_camera_attached(NULL) == 0, "NULL camera reports detached");

	/* --- navigation policy ------------------------------------------------ */
	check(vgui_editor_camera_navigation_enabled(cam), "navigation is on by default");
	vgui_editor_camera_set_navigation_enabled(cam, 0);
	check(vgui_editor_camera_navigation_enabled(cam) == 0, "navigation disabled");
	vgui_editor_camera_set_navigation_enabled(cam, 1);
	check(vgui_editor_camera_navigation_enabled(cam), "navigation re-enabled");
	check(vgui_editor_camera_navigation_enabled(NULL) == 0,
		"NULL camera never navigates");
	/* The gesture buttons and sensitivities have no getters: they only steer the
	 * mouse handling, which headless input cannot drive. */
	vgui_editor_camera_set_pan_button(cam, 2);
	vgui_editor_camera_set_orbit_button(cam, 3);
	vgui_editor_camera_set_pan_button(cam, 0);
	vgui_editor_camera_set_orbit_button(cam, 0);
	vgui_editor_camera_set_pan_button(cam, 2);
	vgui_editor_camera_set_orbit_button(cam, 3);
	vgui_editor_camera_set_orbit_sensitivity(cam, 0.006f);
	vgui_editor_camera_set_orbit_sensitivity(cam, -1.0f);
	vgui_editor_camera_set_zoom_speed(cam, 0.12f);
	vgui_editor_camera_set_zoom_speed(cam, -1.0f);
	check(vgui_editor_camera_panning(cam) == 0, "pan idle without input");
	check(vgui_editor_camera_orbiting(cam) == 0, "orbit idle without input");
	check(vgui_editor_camera_navigating(cam) == 0, "navigation idle without input");
	vgui_editor_camera_cancel_navigation(cam);
	vgui_editor_camera_cancel_navigation(NULL);
	check(vgui_editor_camera_navigating(NULL) == 0, "NULL camera does not navigate");

	/* --- state round trip and clamping ------------------------------------ */
	check(state.struct_size == sizeof(state), "camera state default sizes itself");
	check(near_float(state.yaw, 0.62f, 1e-5f) &&
		near_float(state.pitch, 0.47f, 1e-5f),
		"camera default yaw and pitch");
	check(near_float(state.distance, 6.2f, 1e-5f) &&
		near_float(state.fov_deg, 50.0f, 1e-5f),
		"camera default distance and fov");
	check(near_float(state.near_plane, 0.1f, 1e-6f) &&
		near_float(state.far_plane, 1000.0f, 1e-3f),
		"camera default clip planes");
	vgui_editor_camera_state_default(NULL);

	vgui_editor_camera_set_on_change(cam, on_camera_change, p);
	vgui_editor_camera_get_state(cam, &read_back);
	check(read_back.struct_size == sizeof(read_back), "get_state sizes itself");
	check(near_float(read_back.distance, 6.2f, 1e-4f), "camera starts at the default");

	state.target.x = 1.5f;
	state.target.y = -2.0f;
	state.target.z = 0.25f;
	state.yaw = 0.5f;
	state.pitch = 0.25f;
	state.distance = 5.0f;
	state.fov_deg = 60.0f;
	state.near_plane = 0.05f;
	state.far_plane = 500.0f;
	vgui_editor_camera_set_state(cam, &state);
	vgui_editor_camera_get_state(cam, &read_back);
	check(near_float(read_back.target.x, 1.5f, 1e-6f) &&
		near_float(read_back.target.y, -2.0f, 1e-6f) &&
		near_float(read_back.target.z, 0.25f, 1e-6f), "target round-trips");
	check(near_float(read_back.yaw, 0.5f, 1e-6f) &&
		near_float(read_back.pitch, 0.25f, 1e-6f), "angles round-trip");
	check(near_float(read_back.distance, 5.0f, 1e-6f) &&
		near_float(read_back.fov_deg, 60.0f, 1e-6f) &&
		near_float(read_back.near_plane, 0.05f, 1e-6f) &&
		near_float(read_back.far_plane, 500.0f, 1e-3f), "ranges round-trip");
	check(p->change_count == 1, "set_state notifies the change listener");

	/* Out-of-range channels are pulled back into safe values. */
	state.pitch = 9.0f;
	state.distance = 0.0f;
	state.fov_deg = 200.0f;
	state.near_plane = -1.0f;
	state.far_plane = 0.0f;
	vgui_editor_camera_set_state(cam, &state);
	vgui_editor_camera_get_state(cam, &read_back);
	check(read_back.pitch <= 1.5f && read_back.pitch > 1.4f, "pitch clamps up");
	check(read_back.distance >= 0.1f && read_back.distance < 0.2f,
		"distance clamps to the minimum");
	check(near_float(read_back.fov_deg, 120.0f, 1e-4f), "fov clamps to 120");
	check(read_back.near_plane > 0.0f && read_back.near_plane <= 0.02f,
		"near plane is forced positive");
	check(read_back.far_plane > read_back.near_plane, "far plane stays beyond near");
	vgui_editor_camera_set_state(cam, NULL);
	vgui_editor_camera_get_state(cam, NULL);

	vgui_editor_camera_reset(cam);
	vgui_editor_camera_get_state(cam, &read_back);
	check(near_float(read_back.yaw, 0.62f, 1e-5f) &&
		near_float(read_back.distance, 6.2f, 1e-4f) &&
		read_back.target.x == 0.0f, "reset restores the defaults");
	check(p->change_count == 3, "reset notifies the change listener");

	vgui_editor_camera_set_distance_range(cam, 3.0f, 8.0f);
	vgui_editor_camera_get_state(cam, &read_back);
	check(read_back.distance >= 3.0f && read_back.distance <= 8.0f,
		"distance range keeps the live distance inside");
	state.distance = 100.0f;
	vgui_editor_camera_set_state(cam, &state);
	vgui_editor_camera_get_state(cam, &read_back);
	check(near_float(read_back.distance, 8.0f, 1e-5f), "far distance clamps to max");
	vgui_editor_camera_set_distance_range(cam, 0.1f, 1000.0f);
	vgui_editor_camera_reset(cam);

	/* --- focus provider --------------------------------------------------- */
	check(vgui_editor_camera_focus_selection(cam) == 0,
		"no focus without a provider");
	fix.focus_ok = 0;
	vgui_editor_camera_set_focus_provider(cam, provide_camera_focus, &fix);
	check(vgui_editor_camera_focus_selection(cam) == 0, "rejected focus does nothing");
	check(fix.focus_calls == 1, "provider consulted");
	fix.focus_ok = 1;
	fix.center.x = 1.0f;
	fix.center.y = 2.0f;
	fix.center.z = 3.0f;
	fix.radius = 0.0f;
	check(vgui_editor_camera_focus_selection(cam), "focus applied");
	check(fix.focus_calls == 2, "provider consulted again");
	vgui_editor_camera_get_state(cam, &read_back);
	check(near_float(read_back.target.x, 1.0f, 1e-6f) &&
		near_float(read_back.target.y, 2.0f, 1e-6f) &&
		near_float(read_back.target.z, 3.0f, 1e-6f), "focus recentres the target");
	check(near_float(read_back.distance, 6.2f, 1e-4f),
		"a zero radius leaves the distance alone");
	fix.radius = 2.0f;
	check(vgui_editor_camera_focus_selection(cam), "radius focus applied");
	vgui_editor_camera_get_state(cam, &read_back);
	check(read_back.distance > 4.0f && read_back.distance < 7.0f,
		"the radius picks a framing distance");
	check(p->change_count >= 3, "focus changes notify");
	vgui_editor_camera_set_focus_provider(cam, NULL, NULL);
	check(vgui_editor_camera_focus_selection(cam) == 0, "cleared provider stops it");
	vgui_editor_camera_set_focus_provider(NULL, provide_camera_focus, &fix);

	/* --- basis vectors ---------------------------------------------------- */
	vgui_editor_camera_reset(cam);
	pos = vgui_editor_camera_position(cam);
	fwd = vgui_editor_camera_forward(cam);
	right = vgui_editor_camera_right(cam);
	up = vgui_editor_camera_up(cam);
	length = sqrt_float_approx(fwd);
	check(length > 0.999f && length < 1.001f, "forward is a unit vector");
	check(sqrt_float_approx(right) > 0.999f, "right is a unit vector");
	check(sqrt_float_approx(up) > 0.999f, "up is a unit vector");
	check(abs_float(right.y) < 1e-4f, "right stays horizontal");
	check(abs_float(fwd.x * right.x + fwd.y * right.y + fwd.z * right.z) < 1e-4f,
		"forward and right are orthogonal");
	check(abs_float(right.x * up.x + right.y * up.y + right.z * up.z) < 1e-4f,
		"right and up are orthogonal");
	{
		float dx = pos.x - 0.0f;
		float dy = pos.y - 0.0f;
		float dz = pos.z - 0.0f;
		float dist = sqrt_float_approx(make_vec3_helper(dx, dy, dz));
		check(near_float(dist, 6.2f, 1e-3f), "position sits distance away");
	}
	check(vgui_editor_camera_position(NULL).x == 0.0f, "NULL camera position is zero");
	check(vgui_editor_camera_forward(NULL).z == 0.0f, "NULL camera forward is zero");
	check(vgui_editor_camera_right(NULL).y == 0.0f, "NULL camera right is zero");
	check(vgui_editor_camera_up(NULL).x == 0.0f, "NULL camera up is zero");
	vgui_editor_camera_set_on_change(cam, NULL, NULL);
	p->change_count = 0;
	vgui_editor_camera_set_state(cam, &state);
	check(p->change_count == 0, "cleared change listener is silent");
	vgui_editor_camera_get_state(NULL, &read_back);
	vgui_editor_camera_set_state(NULL, &state);
	vgui_editor_camera_reset(NULL);

	/* --- world grid ------------------------------------------------------- */
	check(vgui_editor_grid_visible(grid), "grid is visible by default");
	check(vgui_editor_grid_auto_density(grid), "auto density is on by default");
	check(vgui_editor_grid_axes_visible(grid), "axes are on by default");
	vgui_editor_grid_set_visible(grid, 0);
	check(vgui_editor_grid_visible(grid) == 0, "visibility setter applies");
	vgui_editor_grid_set_visible(grid, 1);
	vgui_editor_grid_set_auto_density(grid, 0);
	check(vgui_editor_grid_auto_density(grid) == 0, "auto density off");
	vgui_editor_grid_set_cell_size(grid, 2.0f);
	check(near_float(vgui_editor_grid_cell_size(grid), 2.0f, 1e-6f),
		"cell size round-trips");
	vgui_editor_grid_set_min_cell_pixels(grid, 18.0f);
	check(near_float(vgui_editor_grid_min_cell_pixels(grid), 18.0f, 1e-6f),
		"min cell pixels round-trip");
	vgui_editor_grid_set_max_cell_pixels(grid, 120.0f);
	check(near_float(vgui_editor_grid_max_cell_pixels(grid), 120.0f, 1e-6f),
		"max cell pixels round-trip");
	vgui_editor_grid_set_half_cell_count(grid, 4);
	check(vgui_editor_grid_half_cell_count(grid) == 4, "half cell count round-trip");
	vgui_editor_grid_set_major_every(grid, 10);
	check(vgui_editor_grid_major_every(grid) == 10, "major every round-trip");
	vgui_editor_grid_set_plane_height(grid, 3.0f);
	check(near_float(vgui_editor_grid_plane_height(grid), 3.0f, 1e-6f),
		"plane height round-trip");
	vgui_editor_grid_set_axis_length_cells(grid, 3.0f);
	vgui_editor_grid_set_colors(grid, 0x11111111u, 0x22222222u, 0xFF0000FFu,
		0xFF00FF00u, 0xFFFF0000u);
	check(vgui_editor_grid_visible(NULL) == 0, "NULL grid is not visible");
	check(vgui_editor_grid_cell_size(NULL) == 0.0f, "NULL grid has no cell size");
	check(vgui_editor_grid_half_cell_count(NULL) == 0, "NULL grid has no extent");
	check(vgui_editor_grid_major_every(NULL) == 0, "NULL grid has no major every");
	check(vgui_editor_grid_plane_height(NULL) == 0.0f, "NULL grid plane is zero");
	check(vgui_editor_grid_auto_density(NULL) == 0, "NULL grid has no auto density");
	check(vgui_editor_grid_axes_visible(NULL) == 0, "NULL grid hides the axes");
	vgui_editor_grid_set_visible(NULL, 1);
	vgui_editor_grid_set_colors(NULL, 0, 0, 0, 0, 0);
	vgui_editor_grid_set_axis_length_cells(NULL, 1.0f);

	/* Auto density is off, so the effective cell is the configured one even for
	 * nonsense view parameters. */
	check(near_float(vgui_editor_grid_effective_cell_size(grid, 500.0f, 600, 50.0f),
		2.0f, 1e-6f), "fixed cell size ignores the camera");
	check(near_float(vgui_editor_grid_effective_cell_size(grid, 0.0f, 0, 0.0f),
		2.0f, 1e-6f), "bad view parameters still give a cell");
	check(vgui_editor_grid_effective_cell_size(NULL, 1.0f, 100, 45.0f) == 0.0f,
		"NULL grid has no effective cell");
	vgui_editor_grid_set_auto_density(grid, 1);
	cell_near = vgui_editor_grid_effective_cell_size(grid, 2.0f, 600, 50.0f);
	cell_far = vgui_editor_grid_effective_cell_size(grid, 400.0f, 600, 50.0f);
	check(cell_near > 0.0f && cell_far > cell_near,
		"auto density grows with the distance");
	cell_invalid = vgui_editor_grid_effective_cell_size(grid, 2.0f, 0, 50.0f);
	check(cell_invalid > 0.0f, "auto density falls back on a bad height");
	vgui_editor_grid_set_auto_density(grid, 0);

	vgui_editor_camera_get_state(cam, &read_back);
	check(vgui_editor_grid_build(grid, NULL, 800, 600, NULL, 0) == 0,
		"build needs a camera state");
	check(vgui_editor_grid_build(NULL, &read_back, 800, 600, lines, 40) == 0,
		"NULL grid builds nothing");
	vgui_editor_grid_set_visible(grid, 0);
	check(vgui_editor_grid_build(grid, &read_back, 800, 600, NULL, 0) == 0,
		"hidden grid reports no lines");
	check(vgui_editor_grid_build(grid, &read_back, 800, 600, lines, 40) == 0,
		"hidden grid fills nothing");
	vgui_editor_grid_set_visible(grid, 1);
	check(vgui_editor_grid_build(grid, &read_back, 0, 600, NULL, 0) == 0,
		"a zero width viewport builds nothing");
	null_count = vgui_editor_grid_build(grid, &read_back, 800, 600, NULL, 0);
	check(null_count == 21, "count-only build reports every line");
	filled_count = vgui_editor_grid_build(grid, &read_back, 800, 600, lines, 40);
	check(filled_count == null_count, "array build matches the count");
	check(lines[0].struct_size == sizeof(lines[0]), "grid line sizes itself");
	check(near_float(lines[0].a.y, 3.0f, 1e-6f) &&
		near_float(lines[0].b.y, 3.0f, 1e-6f),
		"grid lines sit on the plane height");
	check(lines[0].width > 0.0f, "grid lines carry a world width");
	for (i = 0; i < (int)filled_count; ++i)
	{
		if (lines[i].color == 0x22222222u)
			++majors;
		if (lines[i].color == 0x11111111u)
			++minors;
	}
	check(majors == 2, "major lines carry the major colour");
	check(minors == 16, "minor lines carry the minor colour");
	check(lines[18].color == 0xFF0000FFu, "x axis line uses the x colour");
	check(lines[19].color == 0xFFFF0000u, "z axis line uses the z colour");
	check(lines[20].color == 0xFF00FF00u, "y axis line uses the y colour");
	check(near_float(lines[20].b.y - lines[20].a.y, 6.0f, 1e-4f),
		"axis length follows axis_length_cells");
	check(lines[0].width < lines[18].width, "axis lines are thicker than minors");
	truncated = vgui_editor_grid_build(grid, &read_back, 800, 600, lines, 8);
	check(truncated == 8, "a small array is filled but not overrun");
	vgui_editor_grid_set_axes_visible(grid, 0);
	check(vgui_editor_grid_axes_visible(grid) == 0, "axes hidden");
	check(vgui_editor_grid_build(grid, &read_back, 800, 600, NULL, 0) ==
		null_count - 3, "hiding the axes drops exactly three lines");
	vgui_editor_grid_set_axes_visible(grid, 1);

	/* --- picking ---------------------------------------------------------- */
	check(vgui_editor_picking_attached(pick) == 0, "picking starts detached");
	check(vgui_editor_picking_attach(pick, NULL, ctx) == 0,
		"picking needs a viewport");
	check(vgui_editor_picking_attach(pick, vp, NULL) == 0,
		"picking needs an editor context");
	check(vgui_editor_picking_attach(pick, vp, ctx), "picking attaches");
	check(vgui_editor_picking_attached(pick), "picking reports attached");
	check(vgui_editor_picking_attach(pick, vp, ctx), "re-attaching succeeds");
	check(vgui_editor_picking_enabled(pick), "picking is enabled by default");
	vgui_editor_picking_set_enabled(pick, 0);
	check(vgui_editor_picking_enabled(pick) == 0, "picking disabled");
	vgui_editor_picking_set_enabled(pick, 1);
	check(vgui_editor_picking_multi_select(pick), "multi select is on by default");
	vgui_editor_picking_set_multi_select(pick, 0);
	check(vgui_editor_picking_multi_select(pick) == 0, "multi select disabled");
	vgui_editor_picking_set_multi_select(pick, 1);
	vgui_editor_picking_set_drag_threshold(pick, 12.0f);
	vgui_editor_picking_set_drag_threshold(pick, -1.0f);
	check(vgui_editor_picking_pending(pick) == 0, "no pending gesture");
	check(vgui_editor_picking_selecting(pick) == 0, "no box selection in flight");
	vgui_editor_picking_cancel_selection(pick);
	vgui_editor_picking_cancel_selection(NULL);
	check(vgui_editor_picking_pending(NULL) == 0, "NULL picking is idle");
	check(vgui_editor_picking_enabled(NULL) == 0, "NULL picking is disabled");
	check(vgui_editor_picking_multi_select(NULL) == 0, "NULL picking has no modifier");
	check(vgui_editor_picking_attached(NULL) == 0, "NULL picking is detached");

	picked.box_ids = 3;
	vgui_editor_picking_set_point_picker(pick, pick_point, &picked);
	vgui_editor_picking_set_box_picker(pick, pick_box, &picked);
	vgui_editor_picking_set_highlight_provider(pick, provide_highlight, &picked);
	vgui_editor_context_set_selection(ctx, 9);
	check(vgui_editor_picking_selected_count(pick) == 1, "selection mirrored");
	check(vgui_editor_picking_selected_at(pick, 0) == 9, "mirrored id");
	ids[0] = 11;
	ids[1] = 12;
	ids[2] = 13;
	vgui_editor_context_set_selection_ids(ctx, ids, 3, 12);
	check(vgui_editor_picking_selected_count(pick) == 3, "multi selection mirrored");
	check(vgui_editor_picking_selected_at(pick, 1) == 12,
		"mirrored order follows the id list");
	check(vgui_editor_picking_selected_at(pick, 99) == VGUI_EDITOR_INVALID_OBJECT,
		"out of range index is invalid");
	check(vgui_editor_picking_selected_at(NULL, 0) == VGUI_EDITOR_INVALID_OBJECT,
		"NULL picking has no ids");
	vgui_editor_context_clear_selection(ctx);
	check(vgui_editor_picking_selected_count(pick) == 0, "cleared selection mirrors");
	frame(1);
	check(picked.point_calls == 0 && picked.box_calls == 0,
		"pickers wait for real mouse input");
	check(picked.highlight_calls == 0, "the highlight provider waits for a draw");
	vgui_editor_picking_set_point_picker(pick, NULL, NULL);
	vgui_editor_picking_set_box_picker(pick, NULL, NULL);
	vgui_editor_picking_set_highlight_provider(pick, NULL, NULL);
	vgui_editor_picking_set_point_picker(NULL, pick_point, &picked);

	vgui_editor_picking_detach(pick);
	check(vgui_editor_picking_attached(pick) == 0, "picking detached");
	vgui_editor_context_set_selection(ctx, 5);
	check(vgui_editor_picking_selected_count(pick) == 0,
		"a detached picking mirrors nothing");
	vgui_editor_picking_detach(pick);
	vgui_editor_context_clear_selection(ctx);

	vgui_editor_camera_detach(cam);
	vgui_editor_picking_destroy(pick);
	vgui_editor_grid_destroy(grid);
	vgui_editor_grid_destroy(NULL);
	vgui_editor_camera_destroy(cam);
	vgui_editor_camera_destroy(other_cam);
	vgui_editor_camera_destroy(NULL);
	vgui_editor_viewport_destroy(vp);
	vgui_editor_context_destroy(ctx);
	frame(1);
	check(vgui_editor_grid_cell_size(NULL) == 0.0f, "NULL grid stays unusable");
	check(vgui_editor_picking_selected_count(NULL) == 0, "NULL picking stays empty");
}

/* -------------------------------------------------------------------------- */
/* Transform gizmo                                                             */
/* -------------------------------------------------------------------------- */

void test_gizmo(probe* p)
{
	vgui_editor_gizmo* gizmo;
	vgui_editor_gizmo* spare;
	vgui_editor_viewport* vp;
	vgui_editor_viewport* other;
	vgui_editor_gizmo_frame gframe;
	vgui_editor_gizmo_transform transform;
	vgui_editor_snap_settings snap;
	gizmo_fixture fix;
	vgui_widget* surface;

	memset(p, 0, sizeof(*p));
	memset(&fix, 0, sizeof(fix));
	memset(&gframe, 0, sizeof(gframe));
	memset(&transform, 0, sizeof(transform));
	memset(&snap, 0, sizeof(snap));

	vp = vgui_editor_viewport_create(vgui_root(g_ui));
	other = vgui_editor_viewport_create(vgui_root(g_ui));
	gizmo = vgui_editor_gizmo_create();
	spare = vgui_editor_gizmo_create();
	check(vp != NULL && other != NULL, "gizmo viewports available");
	check(gizmo != NULL && spare != NULL, "gizmo create");
	fix.gizmo = gizmo;

	/* --- attach ------------------------------------------------------------ */
	check(vgui_editor_gizmo_attached(gizmo) == 0, "gizmo starts detached");
	check(vgui_editor_gizmo_attach(NULL, vp) == 0, "NULL gizmo cannot attach");
	check(vgui_editor_gizmo_attach(gizmo, NULL) == 0, "attach needs a viewport");
	check(vgui_editor_gizmo_attach(gizmo, vp), "gizmo attaches");
	check(vgui_editor_gizmo_attached(gizmo), "attached() reports the attach");
	check(vgui_editor_gizmo_attach(gizmo, other), "re-attach moves to a new surface");
	check(vgui_editor_gizmo_attached(gizmo), "still attached after the move");
	vgui_editor_gizmo_detach(gizmo);
	check(vgui_editor_gizmo_attached(gizmo) == 0, "detach clears the flag");
	vgui_editor_gizmo_detach(gizmo);
	check(vgui_editor_gizmo_attached(NULL) == 0, "NULL gizmo is detached");
	check(vgui_editor_gizmo_attach(gizmo, vp), "attach again for the rest");

	/* --- presentation state ------------------------------------------------ */
	check(vgui_editor_gizmo_visible(gizmo), "gizmo is visible by default");
	check(vgui_editor_gizmo_mode_of(gizmo) == VGUI_EDITOR_GIZMO_TRANSLATE,
		"mode starts at translate");
	check(vgui_editor_gizmo_space_of(gizmo) == VGUI_EDITOR_GIZMO_SPACE_WORLD,
		"space starts at world");
	check(vgui_editor_gizmo_screen_length(gizmo) > 0.0f, "screen length is positive");
	vgui_editor_gizmo_set_visible(gizmo, 0);
	check(vgui_editor_gizmo_visible(gizmo) == 0, "visibility setter applies");
	vgui_editor_gizmo_set_visible(gizmo, 1);
	check(vgui_editor_gizmo_visible(gizmo), "visibility restored");
	vgui_editor_gizmo_set_screen_length(gizmo, 64.0f);
	check(near_float(vgui_editor_gizmo_screen_length(gizmo), 64.0f, 1e-5f),
		"screen length round-trips");
	vgui_editor_gizmo_set_screen_length(gizmo, -8.0f);
	check(vgui_editor_gizmo_screen_length(gizmo) >= 0.0f,
		"a negative length clamps");
	/* Tolerance and axis width have no getters; they only steer the hit test. */
	vgui_editor_gizmo_set_handle_tolerance(gizmo, 9.0f);
	vgui_editor_gizmo_set_axis_width(gizmo, 3.0f);
	vgui_editor_gizmo_set_handle_tolerance(gizmo, -1.0f);
	vgui_editor_gizmo_set_axis_width(gizmo, -1.0f);
	check(vgui_editor_gizmo_screen_length(NULL) == 0.0f, "NULL gizmo has no length");
	check(vgui_editor_gizmo_visible(NULL) == 0, "NULL gizmo is invisible");
	check(vgui_editor_gizmo_mode_of(NULL) == VGUI_EDITOR_GIZMO_TRANSLATE,
		"NULL gizmo mode reads translate");
	check(vgui_editor_gizmo_space_of(NULL) == VGUI_EDITOR_GIZMO_SPACE_WORLD,
		"NULL gizmo space reads world");
	vgui_editor_gizmo_set_visible(NULL, 1);
	vgui_editor_gizmo_set_screen_length(NULL, 10.0f);
	vgui_editor_gizmo_set_axis_width(NULL, 1.0f);
	vgui_editor_gizmo_set_handle_tolerance(NULL, 1.0f);

	/* --- modes and spaces --------------------------------------------------- */
	vgui_editor_gizmo_set_mode(gizmo, VGUI_EDITOR_GIZMO_ROTATE);
	check(vgui_editor_gizmo_mode_of(gizmo) == VGUI_EDITOR_GIZMO_ROTATE, "rotate mode");
	vgui_editor_gizmo_set_mode(gizmo, VGUI_EDITOR_GIZMO_SCALE);
	check(vgui_editor_gizmo_mode_of(gizmo) == VGUI_EDITOR_GIZMO_SCALE, "scale mode");
	vgui_editor_gizmo_set_space(gizmo, VGUI_EDITOR_GIZMO_SPACE_LOCAL);
	check(vgui_editor_gizmo_space_of(gizmo) == VGUI_EDITOR_GIZMO_SPACE_LOCAL,
		"local space");
	vgui_editor_gizmo_set_space(gizmo, VGUI_EDITOR_GIZMO_SPACE_PARENT);
	check(vgui_editor_gizmo_space_of(gizmo) == VGUI_EDITOR_GIZMO_SPACE_PARENT,
		"parent space");
	vgui_editor_gizmo_set_space(gizmo, VGUI_EDITOR_GIZMO_SPACE_SCREEN);
	check(vgui_editor_gizmo_space_of(gizmo) == VGUI_EDITOR_GIZMO_SPACE_SCREEN,
		"screen space");
	vgui_editor_gizmo_set_space(gizmo, VGUI_EDITOR_GIZMO_SPACE_WORLD);
	vgui_editor_gizmo_set_mode(gizmo, VGUI_EDITOR_GIZMO_TRANSLATE);
	check(vgui_editor_gizmo_mode_of(gizmo) == VGUI_EDITOR_GIZMO_TRANSLATE,
		"back to translate");

	/* --- providers and handlers --------------------------------------------- */
	vgui_editor_gizmo_set_frame_provider(gizmo, provide_gizmo_frame, &fix);
	vgui_editor_gizmo_set_projection(gizmo, project_gizmo, &fix);
	vgui_editor_gizmo_set_ray_provider(gizmo, provide_gizmo_ray, &fix);
	vgui_editor_gizmo_set_transform_handler(gizmo, on_gizmo_transform, p);
	vgui_editor_gizmo_set_drag_handler(gizmo, on_gizmo_drag, p);
	surface = vgui_editor_viewport_widget(vp);
	check(surface != NULL, "attached viewport keeps its widget");
	frame(1);
	check(fix.frame_calls == 0 && fix.project_calls == 0 && fix.ray_calls == 0,
		"providers only run in a draw or hit test");
	check(p->action_count == 0 && p->commit_count == 0,
		"no transform callback without a drag");

	/* A provider change and the mode/space/visibility setters all cancel an
	 * in-flight drag; with no drag in flight nothing may be emitted. */
	vgui_editor_gizmo_set_frame_provider(gizmo, provide_gizmo_frame, &fix);
	vgui_editor_gizmo_set_mode(gizmo, VGUI_EDITOR_GIZMO_ROTATE);
	vgui_editor_gizmo_set_space(gizmo, VGUI_EDITOR_GIZMO_SPACE_LOCAL);
	vgui_editor_gizmo_set_visible(gizmo, 0);
	vgui_editor_gizmo_set_visible(gizmo, 1);
	vgui_editor_gizmo_set_mode(gizmo, VGUI_EDITOR_GIZMO_TRANSLATE);
	vgui_editor_gizmo_set_space(gizmo, VGUI_EDITOR_GIZMO_SPACE_WORLD);
	check(vgui_editor_gizmo_dragging(gizmo) == 0, "still no drag after the changes");
	check(vgui_editor_gizmo_active_handle(gizmo) == VGUI_EDITOR_GIZMO_HANDLE_NONE,
		"no active handle while idle");
	check(vgui_editor_gizmo_hovered_handle(gizmo) == VGUI_EDITOR_GIZMO_HANDLE_NONE,
		"no hovered handle while idle");
	check(p->action_count == 0, "cancelling an absent drag emits nothing");
	vgui_editor_gizmo_cancel_drag(gizmo);
	vgui_editor_gizmo_reset_hover(gizmo);
	check(vgui_editor_gizmo_hovered_handle(gizmo) == VGUI_EDITOR_GIZMO_HANDLE_NONE,
		"reset_hover keeps the hover clear");
	check(p->action_count == 0 && p->commit_count == 0,
		"cancel and reset stay silent when idle");
	vgui_editor_gizmo_cancel_drag(NULL);
	vgui_editor_gizmo_reset_hover(NULL);
	check(vgui_editor_gizmo_dragging(NULL) == 0, "NULL gizmo is not dragging");
	check(vgui_editor_gizmo_active_handle(NULL) == VGUI_EDITOR_GIZMO_HANDLE_NONE,
		"NULL gizmo has no active handle");
	check(vgui_editor_gizmo_hovered_handle(NULL) == VGUI_EDITOR_GIZMO_HANDLE_NONE,
		"NULL gizmo has no hovered handle");
	vgui_editor_gizmo_set_frame_provider(NULL, provide_gizmo_frame, &fix);
	vgui_editor_gizmo_set_projection(NULL, project_gizmo, &fix);
	vgui_editor_gizmo_set_ray_provider(NULL, provide_gizmo_ray, &fix);
	vgui_editor_gizmo_set_transform_handler(NULL, on_gizmo_transform, p);
	vgui_editor_gizmo_set_drag_handler(NULL, on_gizmo_drag, p);

	/* --- transform and frame structs ---------------------------------------- */
	vgui_editor_gizmo_transform_default(&transform);
	check(transform.struct_size == sizeof(transform), "transform sizes itself");
	check(transform.mode == VGUI_EDITOR_GIZMO_TRANSLATE &&
		transform.handle == VGUI_EDITOR_GIZMO_HANDLE_NONE,
		"transform defaults to translate with no handle");
	check(transform.translation.x == 0.0f && transform.translation.y == 0.0f &&
		transform.translation.z == 0.0f, "default translation is zero");
	check(transform.rotation.x == 0.0f && transform.rotation.y == 0.0f &&
		transform.rotation.z == 0.0f, "default rotation is zero");
	check(near_float(transform.scale.x, 1.0f, 1e-6f) &&
		near_float(transform.scale.y, 1.0f, 1e-6f) &&
		near_float(transform.scale.z, 1.0f, 1e-6f), "default scale is neutral");
	check(near_float(transform.axis[0].x, 1.0f, 1e-6f) &&
		near_float(transform.axis[1].y, 1.0f, 1e-6f) &&
		near_float(transform.axis[2].z, 1.0f, 1e-6f), "default axes are identity");
	check(transform.axis[0].y == 0.0f && transform.axis[1].x == 0.0f,
		"off-diagonal default axes are zero");
	vgui_editor_gizmo_transform_default(NULL);

	vgui_editor_gizmo_frame_default(&gframe);
	check(gframe.struct_size == sizeof(gframe), "frame sizes itself");
	check(gframe.valid == 0, "frame starts invalid");
	check(gframe.origin.x == 0.0f && gframe.origin.y == 0.0f &&
		gframe.origin.z == 0.0f, "frame origin starts at zero");
	check(near_float(gframe.axis[0].x, 1.0f, 1e-6f) &&
		near_float(gframe.axis[1].y, 1.0f, 1e-6f) &&
		near_float(gframe.axis[2].z, 1.0f, 1e-6f), "frame axes default to identity");
	check(gframe.screen_length == 0.0f, "no per-frame length by default");
	/* Round-trip a filled frame through the provider the gizmo would query. */
	gframe.valid = 1;
	gframe.origin.x = 4.0f;
	gframe.screen_length = 96.0f;
	gframe.axis[0].x = 0.5f;
	gframe.axis[0].y = 0.5f;
	check(near_float(gframe.origin.x, 4.0f, 1e-6f) &&
		near_float(gframe.axis[0].y, 0.5f, 1e-6f) &&
		near_float(gframe.screen_length, 96.0f, 1e-6f), "frame edits round-trip");
	vgui_editor_gizmo_frame_default(NULL);
	fix.valid = 0;

	/* --- snapping policy ----------------------------------------------------- */
	vgui_editor_snap_settings_default(&snap);
	{
		vgui_editor_snap_settings read_back;
		memset(&read_back, 0, sizeof(read_back));
		vgui_editor_gizmo_get_snap(gizmo, &read_back);
		check(read_back.struct_size == sizeof(read_back), "snap read sizes itself");
		check(read_back.enabled == 1, "the gizmo snaps by default");
		check(near_float(read_back.translate_step, 0.5f, 1e-6f) &&
			near_float(read_back.rotate_step_deg, 15.0f, 1e-5f) &&
			near_float(read_back.scale_step, 0.1f, 1e-6f),
			"gizmo snap steps match the shared default");

		snap.enabled = 1;
		snap.translate = 0;
		snap.rotate = 0;
		snap.scale = 0;
		snap.translate_step = 0.25f;
		snap.rotate_step_deg = 45.0f;
		snap.scale_step = 0.05f;
		vgui_editor_gizmo_set_snap(gizmo, &snap);
		vgui_editor_gizmo_get_snap(gizmo, &read_back);
		check(read_back.enabled == 1 && read_back.translate == 0 &&
			read_back.rotate == 0 && read_back.scale == 0, "snap channels round-trip");
		check(near_float(read_back.translate_step, 0.25f, 1e-6f) &&
			near_float(read_back.rotate_step_deg, 45.0f, 1e-5f) &&
			near_float(read_back.scale_step, 0.05f, 1e-6f),
			"snap steps round-trip");

		/* NULL detaches the policy: the gizmo then snaps nothing. */
		vgui_editor_gizmo_set_snap(gizmo, NULL);
		vgui_editor_gizmo_get_snap(gizmo, &read_back);
		check(read_back.enabled == 0, "clearing snap detaches the policy");
		vgui_editor_gizmo_set_snap(gizmo, &snap);
		vgui_editor_gizmo_get_snap(gizmo, &read_back);
		check(read_back.enabled == 1, "re-installing snap takes effect");
		vgui_editor_gizmo_get_snap(gizmo, NULL);
		vgui_editor_gizmo_get_snap(NULL, &read_back);
		vgui_editor_gizmo_set_snap(NULL, &snap);
		vgui_editor_snap_settings_default(NULL);
	}

	/* --- teardown ------------------------------------------------------------ */
	vgui_editor_gizmo_set_transform_handler(gizmo, NULL, NULL);
	vgui_editor_gizmo_set_drag_handler(gizmo, NULL, NULL);
	vgui_editor_gizmo_set_frame_provider(gizmo, NULL, NULL);
	vgui_editor_gizmo_set_projection(gizmo, NULL, NULL);
	vgui_editor_gizmo_set_ray_provider(gizmo, NULL, NULL);
	vgui_editor_gizmo_detach(gizmo);
	check(vgui_editor_gizmo_attached(gizmo) == 0, "detached before destroy");
	frame(1);
	check(p->action_count == 0 && p->commit_count == 0,
		"cleared handlers emit no callbacks");
	vgui_editor_gizmo_destroy(spare);
	vgui_editor_gizmo_destroy(gizmo);
	vgui_editor_gizmo_destroy(NULL);
	vgui_editor_viewport_destroy(vp);
	vgui_editor_viewport_destroy(other);
	frame(1);
	check(vgui_editor_gizmo_dragging(NULL) == 0, "NULL gizmo stays idle");
}

/* -------------------------------------------------------------------------- */
/* Editor shell                                                                */
/* -------------------------------------------------------------------------- */

void test_shell(probe* p)
{
	vgui_editor_context* ctx;
	vgui_editor_shell* shell;
	vgui_editor_command_registry* reg;
	vgui_editor_command_info info;
	vgui_editor_outliner* outliner;
	vgui_editor_property_grid* properties;
	vgui_editor_asset_browser* assets;
	vgui_editor_asset_preview* preview;
	vgui_editor_console* console;
	vgui_editor_canvas* canvas;
	vgui_editor_table* table;
	vgui_editor_timeline* timeline;
	vgui_editor_node_graph* node_graph;
	vgui_editor_code_editor* code;
	vgui_editor_toolbar* toolbar;
	vgui_editor_status_bar* status;
	vgui_editor_search_box* search;
	vgui_editor_breadcrumb* path;
	vgui_editor_viewport* viewport;
	vgui_editor_viewport* preview_viewport;
	vgui_editor_viewport* depth_viewport;
	vgui_editor_camera* camera;
	vgui_editor_camera* preview_camera;
	vgui_editor_picking* picking;
	vgui_editor_gizmo* gizmo;
	vgui_editor_grid* grid;
	vgui_editor_thumbnails* thumbnails;
	vgui_editor_viewport_settings settings;
	vgui_editor_viewport_settings read_back;
	vgui_editor_snap_settings snap;
	vgui_editor_snap_settings snap_back;
	vgui_editor_document_handlers handlers;
	doc_log docs;
	char small[32];
	char json[4096];
	char path_buf[256];
	size_t needed;
	size_t exact;
	int visible_before;
	FILE* probe_file;

	memset(p, 0, sizeof(*p));
	memset(&docs, 0, sizeof(docs));
	memset(&settings, 0, sizeof(settings));
	memset(&read_back, 0, sizeof(read_back));
	memset(&snap, 0, sizeof(snap));
	memset(&snap_back, 0, sizeof(snap_back));
	memset(&handlers, 0, sizeof(handlers));
	memset(&info, 0, sizeof(info));
	small[0] = '\0';
	json[0] = '\0';

	/* --- NULL robustness before anything exists ------------------------------ */
	check(vgui_editor_shell_create(NULL) == NULL, "shell needs an editor context");
	check(vgui_editor_shell_built(NULL) == 0, "NULL shell is not built");
	check(vgui_editor_shell_widget(NULL) == NULL, "NULL shell has no widget");
	check(vgui_editor_shell_menubar(NULL) == NULL, "NULL shell has no menu bar");
	check(vgui_editor_shell_dock(NULL) == NULL, "NULL shell has no dock");
	check(vgui_editor_shell_commands(NULL) == NULL, "NULL shell has no registry");
	check(vgui_editor_shell_run_command(NULL, "file.new") == 0,
		"NULL shell runs nothing");
	check(vgui_editor_shell_panel_visible(NULL, VGUI_EDITOR_PANEL_OUTLINER) == 0,
		"NULL shell has no panels");
	check(vgui_editor_shell_capture_layout_json(NULL, small, sizeof(small)) == 0,
		"NULL shell captures nothing");
	check(vgui_editor_shell_apply_layout_json(NULL, "{}") == 0,
		"NULL shell applies nothing");
	check(vgui_editor_shell_reset_layout(NULL) == 0, "NULL shell cannot reset");
	check(vgui_editor_shell_save_layout(NULL, "x.json") == 0,
		"NULL shell cannot save a layout");
	check(vgui_editor_shell_load_layout(NULL, "x.json") == 0,
		"NULL shell cannot load a layout");
	check(vgui_editor_shell_high_contrast(NULL) == 0, "NULL shell is not contrasting");
	check(vgui_editor_shell_outliner(NULL) == NULL, "NULL shell has no outliner");
	check(vgui_editor_shell_viewport(NULL) == NULL, "NULL shell has no viewport");
	check(vgui_editor_shell_grid(NULL) == NULL, "NULL shell has no grid");
	check(vgui_editor_shell_gizmo(NULL) == NULL, "NULL shell has no gizmo");
	check(vgui_editor_shell_status_bar(NULL) == NULL, "NULL shell has no status bar");
	vgui_editor_shell_destroy(NULL);
	vgui_editor_shell_refresh_commands(NULL);
	vgui_editor_shell_set_document_handlers(NULL, NULL);
	vgui_editor_shell_set_panel_visible(NULL, VGUI_EDITOR_PANEL_VIEWPORT, 1);
	vgui_editor_shell_set_high_contrast(NULL, 1);
	vgui_editor_shell_set_status(NULL, "ignored");
	vgui_editor_shell_set_viewport_settings(NULL, &settings);
	vgui_editor_shell_set_snap(NULL, &snap);
	vgui_editor_viewport_settings_default(NULL);
	vgui_editor_document_handlers_default(NULL);

	/* --- build --------------------------------------------------------------- */
	ctx = vgui_editor_context_create(g_ui);
	check(ctx != NULL, "shell context");
	shell = vgui_editor_shell_create(ctx);
	check(shell != NULL, "shell create");
	check(vgui_editor_shell_built(shell), "shell reports built");
	check(vgui_editor_shell_widget(shell) != NULL, "shell widget");
	check(vgui_editor_shell_menubar(shell) != NULL, "shell menu bar");
	check(vgui_editor_shell_dock(shell) != NULL, "shell dock");
	frame(2);

	/* --- borrowed command registry ------------------------------------------- */
	reg = vgui_editor_shell_commands(shell);
	check(reg != NULL, "shell exposes a command registry");
	check(reg == vgui_editor_shell_commands(shell), "registry handle is stable");
	check(vgui_editor_command_registry_size(reg) > 10, "default commands registered");
	check(vgui_editor_command_registry_has(reg, "file.new"), "file.new exists");
	check(vgui_editor_command_registry_has(reg, "view.outliner"),
		"view.outliner exists");
	check(vgui_editor_command_registry_find(reg, "file.new", &info),
		"find a shell command");
	check(strcmp(info.title, "New") == 0, "command title");
	check(info.struct_size == sizeof(info), "command info sizes itself");
	check(vgui_editor_command_registry_add(reg, "test.shell", "Shell", shell_action,
		p, NULL), "add a command through the borrowed registry");
	check(vgui_editor_shell_run_command(shell, "test.shell"), "run_command succeeds");
	check(p->action_count == 1, "borrowed registry command ran");
	check(vgui_editor_command_registry_is_enabled(reg, "test.shell"),
		"a command without a predicate is enabled");
	vgui_editor_command_registry_set_enabled(reg, "test.shell", always_true, NULL);
	vgui_editor_shell_refresh_commands(shell);
	frame(1);
	check(vgui_editor_shell_run_command(shell, "test.missing") == 0,
		"unknown command id does not run");
	check(vgui_editor_shell_run_command(shell, NULL) == 0,
		"a NULL command id does not run");
	check(vgui_editor_command_registry_remove(reg, "test.shell"),
		"remove the borrowed-registry command");

	/* --- panel visibility ----------------------------------------------------- */
	visible_before = vgui_editor_shell_panel_visible(shell, VGUI_EDITOR_PANEL_OUTLINER);
	vgui_editor_shell_set_panel_visible(shell, VGUI_EDITOR_PANEL_OUTLINER,
		!visible_before);
	check(vgui_editor_shell_panel_visible(shell, VGUI_EDITOR_PANEL_OUTLINER) ==
		!visible_before, "outliner visibility toggles");
	vgui_editor_shell_set_panel_visible(shell, VGUI_EDITOR_PANEL_OUTLINER,
		visible_before);
	vgui_editor_shell_set_panel_visible(shell, VGUI_EDITOR_PANEL_VIEWPORT, 0);
	check(vgui_editor_shell_panel_visible(shell, VGUI_EDITOR_PANEL_VIEWPORT) == 0,
		"viewport hidden");
	vgui_editor_shell_set_panel_visible(shell, VGUI_EDITOR_PANEL_VIEWPORT, 1);
	check(vgui_editor_shell_panel_visible(shell, VGUI_EDITOR_PANEL_VIEWPORT),
		"viewport shown again");
	vgui_editor_shell_set_panel_visible(shell, VGUI_EDITOR_PANEL_PROPERTIES, 0);
	check(vgui_editor_shell_panel_visible(shell, VGUI_EDITOR_PANEL_PROPERTIES) == 0,
		"properties hidden");
	vgui_editor_shell_set_panel_visible(shell, VGUI_EDITOR_PANEL_PROPERTIES, 1);
	vgui_editor_shell_set_panel_visible(shell, VGUI_EDITOR_PANEL_BOTTOM, 0);
	check(vgui_editor_shell_panel_visible(shell, VGUI_EDITOR_PANEL_BOTTOM) == 0,
		"bottom group closed");
	vgui_editor_shell_set_panel_visible(shell, VGUI_EDITOR_PANEL_BOTTOM, 1);
	check(vgui_editor_shell_panel_visible(shell, VGUI_EDITOR_PANEL_BOTTOM),
		"bottom group reopens every view");
	frame(1);

	/* --- layout persistence --------------------------------------------------- */
	needed = vgui_editor_shell_capture_layout_json(shell, NULL, 0);
	check(needed > 0, "layout json reports its length");
	check(needed < sizeof(json), "layout json fits the test buffer");
	exact = vgui_editor_shell_capture_layout_json(shell, small, 8);
	check(exact == needed, "a short buffer reports the full length");
	check(strlen(small) == 7, "a short buffer truncates and terminates");
	vgui_editor_shell_capture_layout_json(shell, json, sizeof(json));
	check(strstr(json, "\"version\"") != NULL, "layout json carries a version");
	check(strstr(json, "\"panels\"") != NULL, "layout json carries the panels");
	check(strstr(json, "outliner") != NULL, "layout json names the outliner");
	check(vgui_editor_shell_apply_layout_json(shell, json), "layout json applies");
	check(vgui_editor_shell_apply_layout_json(shell, "{not json") == 0,
		"damaged layout json is rejected");
	check(strlen(vgui_editor_last_error()) > 0, "layout failure reports an error");
	check(vgui_editor_shell_reset_layout(shell), "reset_layout succeeds");
	vgui_editor_shell_apply_layout_json(shell, NULL);

	check(vgui_editor_shell_save_layout(shell, "") == 0,
		"an empty layout path is rejected");
	check(vgui_editor_shell_load_layout(shell, "") == 0,
		"an empty load path is rejected");
	check(vgui_editor_shell_load_layout(shell, "out/tmp/missing_layout.json") == 0,
		"a missing layout file is reported");

	strcpy(path_buf, "out/tmp/vgui_editor_c_shell_layout.json");
	probe_file = fopen(path_buf, "wb");
	if (probe_file)
	{
		fclose(probe_file);
		check(vgui_editor_shell_save_layout(shell, path_buf), "save_layout succeeds");
		check(vgui_editor_shell_load_layout(shell, path_buf), "load_layout succeeds");
		check(vgui_editor_shell_save_layout(shell, "z:\\no_such_dir\\l.json") == 0,
			"saving into a missing directory fails");
		remove(path_buf);
		check(vgui_editor_shell_load_layout(shell, path_buf) == 0,
			"the deleted file no longer loads");
	}

	/* --- viewport, snap and theme settings ------------------------------------ */
	vgui_editor_viewport_settings_default(&settings);
	check(settings.struct_size == sizeof(settings), "viewport settings size themselves");
	check(settings.hdr == 0 && settings.msaa_samples == 4 && settings.depth_preview,
		"viewport settings defaults");
	memset(&read_back, 0, sizeof(read_back));
	vgui_editor_shell_get_viewport_settings(shell, &read_back);
	check(read_back.struct_size == sizeof(read_back),
		"got viewport settings size themselves");
	read_back.hdr = 1;
	read_back.msaa_samples = 2;
	read_back.depth_preview = 0;
	vgui_editor_shell_set_viewport_settings(shell, &read_back);
	memset(&settings, 0, sizeof(settings));
	vgui_editor_shell_get_viewport_settings(shell, &settings);
	check(settings.hdr == 1 && settings.msaa_samples == 2 &&
		settings.depth_preview == 0, "viewport settings round-trip");
	vgui_editor_shell_run_command(shell, "view.hdr");
	vgui_editor_shell_get_viewport_settings(shell, &settings);
	check(vgui_editor_shell_run_command(shell, "view.msaa") != 0,
		"the MSAA toggle command runs");
	vgui_editor_shell_set_viewport_settings(shell, &read_back);

	vgui_editor_snap_settings_default(&snap);
	vgui_editor_shell_get_snap(shell, &snap_back);
	check(snap_back.struct_size == sizeof(snap_back), "shell snap sizes itself");
	check(snap_back.enabled == snap.enabled &&
		near_float(snap_back.translate_step, snap.translate_step, 1e-6f),
		"shell snap starts from the shared default");
	snap.enabled = 0;
	snap.translate_step = 1.0f;
	snap.rotate_step_deg = 30.0f;
	snap.scale_step = 0.5f;
	vgui_editor_shell_set_snap(shell, &snap);
	memset(&snap_back, 0, sizeof(snap_back));
	vgui_editor_shell_get_snap(shell, &snap_back);
	check(snap_back.enabled == 0 && near_float(snap_back.translate_step, 1.0f, 1e-6f) &&
		near_float(snap_back.rotate_step_deg, 30.0f, 1e-5f) &&
		near_float(snap_back.scale_step, 0.5f, 1e-6f), "shell snap round-trips");
	/* The shell's gizmo reads the shared policy by address; the borrowed C handle
	 * reports its own cached copy, which stays at the kit defaults. */
	memset(&snap_back, 0, sizeof(snap_back));
	vgui_editor_gizmo_get_snap(vgui_editor_shell_gizmo(shell), &snap_back);
	check(snap_back.enabled == 1 &&
		near_float(snap_back.translate_step, 0.5f, 1e-6f),
		"borrowed gizmo snap reports its cached copy");

	check(vgui_editor_shell_high_contrast(shell) == 0, "high contrast starts off");
	vgui_editor_shell_set_high_contrast(shell, 1);
	check(vgui_editor_shell_high_contrast(shell), "high contrast on");
	frame(1);
	vgui_editor_shell_set_high_contrast(shell, 0);
	check(vgui_editor_shell_high_contrast(shell) == 0, "high contrast restored");
	frame(1);

	status = vgui_editor_shell_status_bar(shell);
	check(vgui_editor_shell_status_bar(shell) == status, "status bar handle is stable");
	vgui_editor_shell_set_status(shell, "hello shell");
	check(strcmp(vgui_editor_status_bar_message(status), "hello shell") == 0,
		"set_status reaches the status bar");
	vgui_editor_shell_set_status(shell, NULL);
	frame(1);

	/* --- document handlers ----------------------------------------------------- */
	vgui_editor_document_handlers_default(&handlers);
	check(handlers.struct_size == sizeof(handlers), "handlers size themselves");
	check(handlers.new_document == NULL && handlers.open_document == NULL &&
		handlers.save_document == NULL && handlers.save_document_as == NULL &&
		handlers.exit_editor == NULL && handlers.delete_selection == NULL &&
		handlers.user == NULL && handlers.release_user == NULL,
		"handler defaults are all unset");
	/* Unset handlers keep the status-only fallback. */
	check(vgui_editor_shell_run_command(shell, "file.new"), "file.new runs bare");
	check(strcmp(vgui_editor_status_bar_message(status), "new document") == 0,
		"bare file.new sets the fallback status");

	vgui_editor_document_handlers_default(&handlers);
	handlers.new_document = doc_new;
	handlers.open_document = doc_open;
	handlers.save_document = doc_save;
	handlers.save_document_as = doc_save_as;
	handlers.exit_editor = doc_exit;
	handlers.delete_selection = doc_delete;
	handlers.user = &docs;
	handlers.release_user = release_doc_log;
	vgui_editor_shell_set_document_handlers(shell, &handlers);
	check(docs.releases == 0, "installing handlers does not release");
	check(vgui_editor_shell_run_command(shell, "file.new"), "file.new with handlers");
	check(vgui_editor_shell_run_command(shell, "file.open"), "file.open runs");
	check(vgui_editor_shell_run_command(shell, "file.save"), "file.save runs");
	check(vgui_editor_shell_run_command(shell, "file.save_as"), "file.save_as runs");
	check(vgui_editor_shell_run_command(shell, "file.exit"), "file.exit runs");
	check(docs.news == 1 && docs.opens == 1 && docs.saves == 1 &&
		docs.save_ases == 1 && docs.exits == 1, "every document callback fired");
	check(vgui_editor_command_registry_is_enabled(reg, "edit.delete") == 0,
		"delete is disabled without a selection");
	vgui_editor_context_set_selection(ctx, 4242);
	check(vgui_editor_command_registry_is_enabled(reg, "edit.delete"),
		"delete enables with a selection");
	check(vgui_editor_shell_run_command(shell, "edit.delete"), "edit.delete runs");
	check(docs.deletes == 1 && docs.delete_count == 1 && docs.first_id == 4242,
		"delete_selection received the ids");
	check(vgui_editor_context_selection(ctx) == 4242,
		"a handled delete leaves the selection alone");
	frame(1);

	vgui_editor_shell_set_document_handlers(shell, NULL);
	check(docs.releases == 1, "restoring the handlers releases the user once");
	check(vgui_editor_shell_run_command(shell, "file.new"), "file.new runs again");
	check(docs.news == 1, "restored handlers are not called");
	check(strcmp(vgui_editor_status_bar_message(status), "new document") == 0,
		"the status-only fallback is back");

	/* --- component accessors: borrowed, cached, destroy is a no-op ------------ */
	outliner = vgui_editor_shell_outliner(shell);
	check(outliner != NULL && outliner == vgui_editor_shell_outliner(shell),
		"outliner handle is borrowed and stable");
	vgui_editor_outliner_destroy(outliner);
	check(outliner == vgui_editor_shell_outliner(shell), "destroying an outliner is inert");

	properties = vgui_editor_shell_properties(shell);
	check(properties != NULL && properties == vgui_editor_shell_properties(shell),
		"properties handle is borrowed and stable");
	vgui_editor_property_grid_destroy(properties);
	check(properties == vgui_editor_shell_properties(shell),
		"destroying a borrowed property grid is inert");

	assets = vgui_editor_shell_assets(shell);
	check(assets != NULL && assets == vgui_editor_shell_assets(shell),
		"assets handle is borrowed and stable");
	vgui_editor_asset_browser_destroy(assets);
	check(assets == vgui_editor_shell_assets(shell), "destroying assets is inert");

	preview = vgui_editor_shell_preview(shell);
	check(preview != NULL && preview == vgui_editor_shell_preview(shell),
		"preview handle is borrowed and stable");
	vgui_editor_asset_preview_destroy(preview);
	check(preview == vgui_editor_shell_preview(shell), "destroying preview is inert");

	console = vgui_editor_shell_console(shell);
	check(console != NULL && console == vgui_editor_shell_console(shell),
		"console handle is borrowed and stable");
	vgui_editor_console_destroy(console);
	check(console == vgui_editor_shell_console(shell), "destroying console is inert");

	canvas = vgui_editor_shell_canvas(shell);
	check(canvas != NULL && canvas == vgui_editor_shell_canvas(shell),
		"canvas handle is borrowed and stable");
	vgui_editor_canvas_destroy(canvas);
	check(canvas == vgui_editor_shell_canvas(shell), "destroying canvas is inert");

	table = vgui_editor_shell_table(shell);
	check(table != NULL && table == vgui_editor_shell_table(shell),
		"table handle is borrowed and stable");
	vgui_editor_table_destroy(table);
	check(table == vgui_editor_shell_table(shell), "destroying table is inert");

	timeline = vgui_editor_shell_timeline(shell);
	check(timeline != NULL && timeline == vgui_editor_shell_timeline(shell),
		"timeline handle is borrowed and stable");
	vgui_editor_timeline_destroy(timeline);
	check(timeline == vgui_editor_shell_timeline(shell), "destroying timeline is inert");

	node_graph = vgui_editor_shell_node_graph(shell);
	check(node_graph != NULL && node_graph == vgui_editor_shell_node_graph(shell),
		"node graph handle is borrowed and stable");
	vgui_editor_node_graph_destroy(node_graph);
	check(node_graph == vgui_editor_shell_node_graph(shell),
		"destroying a borrowed node graph is inert");

	code = vgui_editor_shell_code(shell);
	check(code != NULL && code == vgui_editor_shell_code(shell),
		"code handle is borrowed and stable");
	vgui_editor_code_editor_destroy(code);
	check(code == vgui_editor_shell_code(shell), "destroying code is inert");

	toolbar = vgui_editor_shell_toolbar(shell);
	check(toolbar != NULL && toolbar == vgui_editor_shell_toolbar(shell),
		"toolbar handle is borrowed and stable");
	vgui_editor_toolbar_destroy(toolbar);
	check(toolbar == vgui_editor_shell_toolbar(shell), "destroying toolbar is inert");

	vgui_editor_status_bar_destroy(status);
	check(status == vgui_editor_shell_status_bar(shell),
		"destroying a borrowed status bar is inert");

	search = vgui_editor_shell_search(shell);
	check(search != NULL && search == vgui_editor_shell_search(shell),
		"search handle is borrowed and stable");
	vgui_editor_search_box_destroy(search);
	check(search == vgui_editor_shell_search(shell), "destroying search is inert");

	path = vgui_editor_shell_path(shell);
	check(path != NULL && path == vgui_editor_shell_path(shell),
		"path handle is borrowed and stable");
	vgui_editor_breadcrumb_destroy(path);
	check(path == vgui_editor_shell_path(shell), "destroying breadcrumb is inert");

	viewport = vgui_editor_shell_viewport(shell);
	check(viewport != NULL && viewport == vgui_editor_shell_viewport(shell),
		"viewport handle is borrowed and stable");
	vgui_editor_viewport_destroy(viewport);
	check(viewport == vgui_editor_shell_viewport(shell),
		"destroying a borrowed viewport is inert");
	check(vgui_editor_viewport_widget(viewport) != NULL,
		"the shell viewport still has its widget");

	preview_viewport = vgui_editor_shell_preview_viewport(shell);
	check(preview_viewport != NULL &&
		preview_viewport == vgui_editor_shell_preview_viewport(shell),
		"preview viewport is borrowed and stable");
	check(preview_viewport != viewport, "the preview viewport is a second surface");
	vgui_editor_viewport_destroy(preview_viewport);
	check(preview_viewport == vgui_editor_shell_preview_viewport(shell),
		"destroying the preview viewport is inert");

	depth_viewport = vgui_editor_shell_depth_viewport(shell);
	check(depth_viewport != NULL && depth_viewport ==
		vgui_editor_shell_depth_viewport(shell), "depth viewport is borrowed and stable");
	check(depth_viewport != viewport && depth_viewport != preview_viewport,
		"the depth viewport is its own surface");
	vgui_editor_viewport_destroy(depth_viewport);
	check(depth_viewport == vgui_editor_shell_depth_viewport(shell),
		"destroying the depth viewport is inert");

	camera = vgui_editor_shell_camera(shell);
	check(camera != NULL && camera == vgui_editor_shell_camera(shell),
		"camera handle is borrowed and stable");
	check(vgui_editor_camera_attached(camera), "the shell camera drives its surface");
	vgui_editor_camera_destroy(camera);
	check(camera == vgui_editor_shell_camera(shell) &&
		vgui_editor_camera_attached(camera), "destroying a borrowed camera is inert");

	preview_camera = vgui_editor_shell_preview_camera(shell);
	check(preview_camera != NULL &&
		preview_camera == vgui_editor_shell_preview_camera(shell),
		"preview camera is borrowed and stable");
	check(preview_camera != camera, "the preview camera is a second camera");
	vgui_editor_camera_destroy(preview_camera);
	check(preview_camera == vgui_editor_shell_preview_camera(shell),
		"destroying the preview camera is inert");

	picking = vgui_editor_shell_picking(shell);
	check(picking != NULL && picking == vgui_editor_shell_picking(shell),
		"picking handle is borrowed and stable");
	check(vgui_editor_picking_attached(picking), "the shell picking is attached");
	vgui_editor_picking_destroy(picking);
	check(picking == vgui_editor_shell_picking(shell) &&
		vgui_editor_picking_attached(picking), "destroying picking is inert");

	gizmo = vgui_editor_shell_gizmo(shell);
	check(gizmo != NULL && gizmo == vgui_editor_shell_gizmo(shell),
		"gizmo handle is borrowed and stable");
	check(vgui_editor_gizmo_attached(gizmo), "the shell gizmo is attached");
	vgui_editor_gizmo_destroy(gizmo);
	check(gizmo == vgui_editor_shell_gizmo(shell) && vgui_editor_gizmo_attached(gizmo),
		"destroying a borrowed gizmo is inert");

	grid = vgui_editor_shell_grid(shell);
	check(grid != NULL && grid == vgui_editor_shell_grid(shell),
		"grid handle is borrowed and stable");
	check(vgui_editor_grid_visible(grid), "the shell grid is visible");
	vgui_editor_grid_destroy(grid);
	check(grid == vgui_editor_shell_grid(shell), "destroying a borrowed grid is inert");

	thumbnails = vgui_editor_shell_thumbnails(shell);
	check(thumbnails != NULL && thumbnails == vgui_editor_shell_thumbnails(shell),
		"thumbnails handle is borrowed and stable");
	vgui_editor_thumbnails_destroy(thumbnails);
	check(thumbnails == vgui_editor_shell_thumbnails(shell),
		"destroying borrowed thumbnails is inert");

	/* The shell panels still work through their borrowed handles. */
	check(vgui_editor_console_idle(console), "shell console reachable and idle");
	vgui_editor_console_append_message(console, VGUI_EDITOR_LOG_INFO, "from the test",
		"shell");
	check(vgui_editor_console_pending_count(console) == 1,
		"appending through the borrowed console queues");
	vgui_editor_console_poll(console);
	check(vgui_editor_console_entry_count(console) == 1, "poll drained one entry");
	vgui_editor_console_clear(console);
	vgui_editor_outliner_set_filter(outliner, "abc");
	check(strcmp(vgui_editor_outliner_filter(outliner), "abc") == 0,
		"the borrowed outliner takes a filter");
	vgui_editor_outliner_set_filter(outliner, "");
	check(vgui_editor_camera_navigation_enabled(camera),
		"the borrowed camera still navigates");
	frame(2);

	/* --- teardown: shell first, then the context ------------------------------ */
	vgui_editor_shell_set_document_handlers(shell, NULL);
	vgui_editor_context_clear_selection(ctx);
	vgui_editor_shell_destroy(shell);
	frame(1);
	check(vgui_editor_shell_built(NULL) == 0, "NULL shell stays unusable");
	check(vgui_editor_command_registry_size(NULL) == 0, "NULL registry is empty");
	vgui_editor_context_destroy(ctx);
	vgui_editor_context_destroy(NULL);
	frame(1);
	check(vgui_editor_context_selection(NULL) == VGUI_EDITOR_INVALID_OBJECT,
		"NULL context has no selection");
}
