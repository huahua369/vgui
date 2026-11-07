/* gui/editor/editor_c_api_shell.cpp - C facade for the editor shell declared in
 * <vgui/vgui_editor.h>.
 *
 * The shell owns every panel, so the component handles handed out here are
 * borrowed wrappers the shell creates once and caches: asking twice returns the
 * same pointer, and a borrowed handle's *_destroy() is a no-op, so a host must
 * never destroy them.
 *
 * The shell builds its widget tree under the editor context root at create()
 * time and leaves that tree to the vgui context, so vgui_editor_shell_destroy()
 * releases only the C++ object and its wrappers - no vgui_widget_destroy() here.
 * Document handlers are installed as lambdas that capture the C function
 * pointers and a shared user owner by value, never the shell handle. */
#include "editor_c_api.h"
#include "editor_c_api_borrow.h"

#include <functional>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include <vgui/editor/editor_shell.h>

using namespace vgui_editor_kit;

/* -------------------------------------------------------------------------- */
/* Handle storage                                                            */
/* -------------------------------------------------------------------------- */

struct vgui_editor_shell : vgeditor::c_object<editor_shell> {
	using c_object::c_object;

	~vgui_editor_shell();

	/* One cached wrapper per shell-owned component, created on first use.  Each
	 * adapter's release_borrowed_*() frees its own wrapper, so this file never
	 * needs the concrete handle layouts. */
	vgui_editor_command_registry* registry = nullptr;
	vgui_editor_outliner* outliner = nullptr;
	vgui_editor_property_grid* properties = nullptr;
	vgui_editor_asset_browser* assets = nullptr;
	vgui_editor_asset_preview* preview = nullptr;
	vgui_editor_console* console = nullptr;
	vgui_editor_canvas* canvas = nullptr;
	vgui_editor_table* table = nullptr;
	vgui_editor_timeline* timeline = nullptr;
	vgui_editor_node_graph* node_graph = nullptr;
	vgui_editor_code_editor* code = nullptr;
	vgui_editor_toolbar* toolbar = nullptr;
	vgui_editor_status_bar* status_bar = nullptr;
	vgui_editor_search_box* search = nullptr;
	vgui_editor_breadcrumb* path = nullptr;
	vgui_editor_viewport* viewport = nullptr;
	vgui_editor_viewport* preview_viewport = nullptr;
	vgui_editor_viewport* depth_viewport = nullptr;
	vgui_editor_camera* camera = nullptr;
	vgui_editor_camera* preview_camera = nullptr;
	vgui_editor_picking* picking = nullptr;
	vgui_editor_gizmo* gizmo = nullptr;
	vgui_editor_grid* grid = nullptr;
	/* Owned by the asset browser, borrowed through it. */
	vgui_editor_thumbnails* thumbnails = nullptr;
};

vgui_editor_shell::~vgui_editor_shell()
{
	/* The shell is the only owner of the wrappers it handed out, so they die
	 * before the C++ shell does; each release frees the wrapper and leaves the
	 * panel alone. */
	vgeditor::release_borrowed_command_registry(registry);
	vgeditor::release_borrowed_outliner(outliner);
	vgeditor::release_borrowed_property_grid(properties);
	vgeditor::release_borrowed_asset_browser(assets);
	vgeditor::release_borrowed_asset_preview(preview);
	vgeditor::release_borrowed_console(console);
	vgeditor::release_borrowed_canvas(canvas);
	vgeditor::release_borrowed_table(table);
	vgeditor::release_borrowed_timeline(timeline);
	vgeditor::release_borrowed_node_graph(node_graph);
	vgeditor::release_borrowed_code_editor(code);
	vgeditor::release_borrowed_toolbar(toolbar);
	vgeditor::release_borrowed_status_bar(status_bar);
	vgeditor::release_borrowed_search_box(search);
	vgeditor::release_borrowed_breadcrumb(path);
	vgeditor::release_borrowed_viewport(viewport);
	vgeditor::release_borrowed_viewport(preview_viewport);
	vgeditor::release_borrowed_viewport(depth_viewport);
	vgeditor::release_borrowed_camera(camera);
	vgeditor::release_borrowed_camera(preview_camera);
	vgeditor::release_borrowed_picking(picking);
	vgeditor::release_borrowed_gizmo(gizmo);
	vgeditor::release_borrowed_grid(grid);
	vgeditor::release_borrowed_thumbnails(thumbnails);
	registry = nullptr;
	outliner = nullptr;
	properties = nullptr;
	assets = nullptr;
	preview = nullptr;
	console = nullptr;
	canvas = nullptr;
	table = nullptr;
	timeline = nullptr;
	node_graph = nullptr;
	code = nullptr;
	toolbar = nullptr;
	status_bar = nullptr;
	search = nullptr;
	path = nullptr;
	viewport = nullptr;
	preview_viewport = nullptr;
	depth_viewport = nullptr;
	camera = nullptr;
	preview_camera = nullptr;
	picking = nullptr;
	gizmo = nullptr;
	grid = nullptr;
	thumbnails = nullptr;
}

namespace {

	static_assert((int)VGUI_EDITOR_PANEL_OUTLINER == (int)editor_panel::outliner,
		"panel id drift");
	static_assert((int)VGUI_EDITOR_PANEL_VIEWPORT == (int)editor_panel::viewport,
		"panel id drift");
	static_assert((int)VGUI_EDITOR_PANEL_PROPERTIES == (int)editor_panel::properties,
		"panel id drift");
	static_assert((int)VGUI_EDITOR_PANEL_BOTTOM == (int)editor_panel::bottom,
		"panel id drift");

	/* Returns the shell's wrapper for one component, building it on first use so
	 * the pointer a host stores stays valid and stable for the shell's lifetime. */
	template <typename Handle, typename Object>
	Handle* cached(Handle*& slot, Object& object, Handle* (*wrap)(Object&))
	{
		if (!slot)
			slot = wrap(object);
		return slot;
	}

	editor_panel to_panel(vgui_editor_panel panel)
	{
		switch (panel) {
		case VGUI_EDITOR_PANEL_VIEWPORT:
			return editor_panel::viewport;
		case VGUI_EDITOR_PANEL_PROPERTIES:
			return editor_panel::properties;
		case VGUI_EDITOR_PANEL_BOTTOM:
			return editor_panel::bottom;
		case VGUI_EDITOR_PANEL_OUTLINER:
		default:
			return editor_panel::outliner;
		}
	}

	/* A shorter struct leaves the channels it does not know about at zero, which
	 * the kit reads as "snap nothing" rather than as an error. */
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

	/* Fills a struct the caller already defaulted, so struct_size and the
	 * untouched channels come from vgui_editor_snap_settings_default(). */
	void fill_snap(const snap_settings& snap, vgui_editor_snap_settings* out)
	{
		out->enabled = snap.enabled ? 1 : 0;
		out->translate = snap.translate ? 1 : 0;
		out->rotate = snap.rotate ? 1 : 0;
		out->scale = snap.scale ? 1 : 0;
		out->translate_step = snap.translate_step;
		out->rotate_step_deg = snap.rotate_step_deg;
		out->scale_step = snap.scale_step;
	}

	/* An unset C callback stays an empty std::function, which is exactly how the
	 * kit marks "use the status-only fallback". */
	std::function<void()> document_action(vgui_editor_document_fn fn,
		const vgeditor::user_owner_ptr& owner)
	{
		if (!fn)
			return std::function<void()>();
		return [fn, owner]() { fn(owner->user); };
	}

	/* Records why a layout call failed so vgui_editor_last_error() can explain
	 * it; the kit's own message wins when it has one. */
	void report_error(const std::string& error, const char* fallback)
	{
		vgeditor::set_error(error.empty() ? fallback : error);
	}

} /* namespace */

extern "C" {

	void vgui_editor_viewport_settings_default(
		vgui_editor_viewport_settings* settings)
	{
		if (!settings)
			return;
		std::memset(settings, 0, sizeof(*settings));
		settings->struct_size = (uint32_t)sizeof(*settings);
		settings->hdr = 0;
		settings->msaa_samples = 4;
		settings->depth_preview = 1;
	}

	/* Every slot unset: the shell keeps its status-only document fallback. */
	void vgui_editor_document_handlers_default(
		vgui_editor_document_handlers* handlers)
	{
		if (!handlers)
			return;
		std::memset(handlers, 0, sizeof(*handlers));
		handlers->struct_size = (uint32_t)sizeof(*handlers);
		handlers->new_document = nullptr;
		handlers->open_document = nullptr;
		handlers->save_document = nullptr;
		handlers->save_document_as = nullptr;
		handlers->exit_editor = nullptr;
		handlers->delete_selection = nullptr;
		handlers->user = nullptr;
		handlers->release_user = nullptr;
	}

	vgui_editor_shell* vgui_editor_shell_create(vgui_editor_context* ctx)
	{
		editor_context* context = vgeditor::context_impl(ctx);
		if (!context)
			return nullptr;
		vgui_editor_shell* shell = vgeditor::own_object<vgui_editor_shell>(
			new (std::nothrow) editor_shell(*context));
		if (!shell)
			return nullptr;
		shell->impl.build();
		if (!shell->impl.built()) {
			/* Nothing to build under: hand back nothing rather than a shell whose
			 * widget tree is missing. */
			vgeditor::set_error(
				"editor shell build failed: the editor context has no root widget");
			delete shell;
			return nullptr;
		}
		return shell;
	}

	void vgui_editor_shell_destroy(vgui_editor_shell* shell)
	{
		if (!shell || !shell->owns_object())
			return;
		/* The destructor releases the cached wrappers; the widget tree belongs to
		 * the editor context root and dies with the vgui context. */
		delete shell;
	}

	int vgui_editor_shell_built(vgui_editor_shell* shell)
	{
		return shell && shell->impl.built() ? 1 : 0;
	}

	vgui_widget* vgui_editor_shell_widget(vgui_editor_shell* shell)
	{
		return shell ? shell->impl.widget() : nullptr;
	}

	vgui_widget* vgui_editor_shell_menubar(vgui_editor_shell* shell)
	{
		return shell ? shell->impl.menubar() : nullptr;
	}

	vgui_editor_command_registry* vgui_editor_shell_commands(
		vgui_editor_shell* shell)
	{
		if (!shell)
			return nullptr;
		return cached(shell->registry, shell->impl.commands(),
			vgeditor::borrow_command_registry);
	}

	int vgui_editor_shell_run_command(vgui_editor_shell* shell, const char* id)
	{
		return shell && shell->impl.run_command(id) ? 1 : 0;
	}

	void vgui_editor_shell_refresh_commands(vgui_editor_shell* shell)
	{
		if (!shell)
			return;
		shell->impl.refresh_commands();
	}

	void vgui_editor_shell_set_document_handlers(vgui_editor_shell* shell,
		const vgui_editor_document_handlers* handlers)
	{
		if (!shell)
			return;
		/* A NULL struct reads as all-NULL, which restores the shell defaults. */
		const vgui_editor_document_handlers d = vgeditor::take_desc(handlers);
		vgeditor::user_owner_ptr owner = vgeditor::make_owner(d.user,
			d.release_user);
		document_handlers next;
		next.new_document = document_action(d.new_document, owner);
		next.open_document = document_action(d.open_document, owner);
		next.save_document = document_action(d.save_document, owner);
		next.save_document_as = document_action(d.save_document_as, owner);
		next.exit_editor = document_action(d.exit_editor, owner);
		if (d.delete_selection) {
			next.delete_selection = [fn = d.delete_selection, owner](
				const std::vector<object_id>& ids) {
				return fn(owner->user, ids.empty() ? nullptr : ids.data(),
					ids.size()) != 0;
			};
		}
		shell->impl.set_document_handlers(std::move(next));
	}

	vgui_widget* vgui_editor_shell_dock(vgui_editor_shell* shell)
	{
		return shell ? shell->impl.dock() : nullptr;
	}

	void vgui_editor_shell_set_panel_visible(vgui_editor_shell* shell,
		vgui_editor_panel panel, int visible)
	{
		if (!shell)
			return;
		shell->impl.set_panel_visible(to_panel(panel), visible != 0);
	}

	int vgui_editor_shell_panel_visible(vgui_editor_shell* shell,
		vgui_editor_panel panel)
	{
		if (!shell)
			return 0;
		return shell->impl.panel_visible(to_panel(panel)) ? 1 : 0;
	}

	size_t vgui_editor_shell_capture_layout_json(vgui_editor_shell* shell,
		char* out, size_t cap)
	{
		if (!shell)
			return 0;
		const std::string json = editor_layout_to_json(shell->impl.capture_layout());
		return vgeditor::copy_string(out, cap, json.c_str());
	}

	int vgui_editor_shell_apply_layout_json(vgui_editor_shell* shell,
		const char* json)
	{
		if (!shell)
			return 0;
		editor_layout parsed;
		std::string error;
		if (!editor_layout_from_json(vgeditor::to_text(json), parsed, &error)) {
			report_error(error, "invalid editor layout json");
			return 0;
		}
		if (!shell->impl.apply_layout(parsed, &error)) {
			report_error(error, "editor layout apply failed");
			return 0;
		}
		vgeditor::error_storage().clear();
		return 1;
	}

	int vgui_editor_shell_save_layout(vgui_editor_shell* shell, const char* path)
	{
		if (!shell)
			return 0;
		if (!path || !*path) {
			vgeditor::set_error("editor layout save needs a path");
			return 0;
		}
		std::string error;
		if (!shell->impl.save_layout(path, &error)) {
			report_error(error, "editor layout save failed");
			return 0;
		}
		vgeditor::error_storage().clear();
		return 1;
	}

	int vgui_editor_shell_load_layout(vgui_editor_shell* shell, const char* path)
	{
		if (!shell)
			return 0;
		if (!path || !*path) {
			vgeditor::set_error("editor layout load needs a path");
			return 0;
		}
		std::string error;
		/* A damaged or version-mismatched file is reported and leaves the shell
		 * on the layout captured right after build(). */
		if (!shell->impl.load_layout(path, &error)) {
			report_error(error, "editor layout load failed");
			return 0;
		}
		vgeditor::error_storage().clear();
		return 1;
	}

	int vgui_editor_shell_reset_layout(vgui_editor_shell* shell)
	{
		if (!shell)
			return 0;
		const bool ok = shell->impl.reset_layout();
		if (!ok)
			vgeditor::set_error("editor layout reset failed: shell not built");
		else
			vgeditor::error_storage().clear();
		return ok ? 1 : 0;
	}

	/* ------------------------------------------------------------------ */
	/* Component accessors                                                */
	/* ------------------------------------------------------------------ */

	vgui_editor_outliner* vgui_editor_shell_outliner(vgui_editor_shell* shell)
	{
		if (!shell)
			return nullptr;
		return cached(shell->outliner, shell->impl.scene(), vgeditor::borrow_outliner);
	}

	vgui_editor_property_grid* vgui_editor_shell_properties(
		vgui_editor_shell* shell)
	{
		if (!shell)
			return nullptr;
		return cached(shell->properties, shell->impl.properties(),
			vgeditor::borrow_property_grid);
	}

	vgui_editor_asset_browser* vgui_editor_shell_assets(vgui_editor_shell* shell)
	{
		if (!shell)
			return nullptr;
		return cached(shell->assets, shell->impl.assets(),
			vgeditor::borrow_asset_browser);
	}

	vgui_editor_asset_preview* vgui_editor_shell_preview(vgui_editor_shell* shell)
	{
		if (!shell)
			return nullptr;
		return cached(shell->preview, shell->impl.preview(),
			vgeditor::borrow_asset_preview);
	}

	vgui_editor_console* vgui_editor_shell_console(vgui_editor_shell* shell)
	{
		if (!shell)
			return nullptr;
		return cached(shell->console, shell->impl.console(), vgeditor::borrow_console);
	}

	vgui_editor_canvas* vgui_editor_shell_canvas(vgui_editor_shell* shell)
	{
		if (!shell)
			return nullptr;
		return cached(shell->canvas, shell->impl.canvas(), vgeditor::borrow_canvas);
	}

	vgui_editor_table* vgui_editor_shell_table(vgui_editor_shell* shell)
	{
		if (!shell)
			return nullptr;
		return cached(shell->table, shell->impl.table(), vgeditor::borrow_table);
	}

	vgui_editor_timeline* vgui_editor_shell_timeline(vgui_editor_shell* shell)
	{
		if (!shell)
			return nullptr;
		return cached(shell->timeline, shell->impl.timeline(),
			vgeditor::borrow_timeline);
	}

	vgui_editor_node_graph* vgui_editor_shell_node_graph(vgui_editor_shell* shell)
	{
		if (!shell)
			return nullptr;
		return cached(shell->node_graph, shell->impl.node_graph(),
			vgeditor::borrow_node_graph);
	}

	vgui_editor_code_editor* vgui_editor_shell_code(vgui_editor_shell* shell)
	{
		if (!shell)
			return nullptr;
		return cached(shell->code, shell->impl.code(), vgeditor::borrow_code_editor);
	}

	vgui_editor_toolbar* vgui_editor_shell_toolbar(vgui_editor_shell* shell)
	{
		if (!shell)
			return nullptr;
		return cached(shell->toolbar, shell->impl.toolbar(), vgeditor::borrow_toolbar);
	}

	vgui_editor_status_bar* vgui_editor_shell_status_bar(vgui_editor_shell* shell)
	{
		if (!shell)
			return nullptr;
		return cached(shell->status_bar, shell->impl.statusbar(),
			vgeditor::borrow_status_bar);
	}

	vgui_editor_search_box* vgui_editor_shell_search(vgui_editor_shell* shell)
	{
		if (!shell)
			return nullptr;
		return cached(shell->search, shell->impl.search(),
			vgeditor::borrow_search_box);
	}

	vgui_editor_breadcrumb* vgui_editor_shell_path(vgui_editor_shell* shell)
	{
		if (!shell)
			return nullptr;
		return cached(shell->path, shell->impl.path(), vgeditor::borrow_breadcrumb);
	}

	vgui_editor_viewport* vgui_editor_shell_viewport(vgui_editor_shell* shell)
	{
		if (!shell)
			return nullptr;
		return cached(shell->viewport, shell->impl.viewport(),
			vgeditor::borrow_viewport);
	}

	vgui_editor_viewport* vgui_editor_shell_preview_viewport(
		vgui_editor_shell* shell)
	{
		if (!shell)
			return nullptr;
		return cached(shell->preview_viewport, shell->impl.preview_viewport(),
			vgeditor::borrow_viewport);
	}

	vgui_editor_viewport* vgui_editor_shell_depth_viewport(vgui_editor_shell* shell)
	{
		if (!shell)
			return nullptr;
		return cached(shell->depth_viewport, shell->impl.depth_viewport(),
			vgeditor::borrow_viewport);
	}

	vgui_editor_camera* vgui_editor_shell_camera(vgui_editor_shell* shell)
	{
		if (!shell)
			return nullptr;
		return cached(shell->camera, shell->impl.camera(), vgeditor::borrow_camera);
	}

	vgui_editor_camera* vgui_editor_shell_preview_camera(vgui_editor_shell* shell)
	{
		if (!shell)
			return nullptr;
		return cached(shell->preview_camera, shell->impl.preview_camera(),
			vgeditor::borrow_camera);
	}

	vgui_editor_picking* vgui_editor_shell_picking(vgui_editor_shell* shell)
	{
		if (!shell)
			return nullptr;
		return cached(shell->picking, shell->impl.picking(), vgeditor::borrow_picking);
	}

	vgui_editor_gizmo* vgui_editor_shell_gizmo(vgui_editor_shell* shell)
	{
		if (!shell)
			return nullptr;
		return cached(shell->gizmo, shell->impl.gizmo(), vgeditor::borrow_gizmo);
	}

	vgui_editor_grid* vgui_editor_shell_grid(vgui_editor_shell* shell)
	{
		if (!shell)
			return nullptr;
		return cached(shell->grid, shell->impl.grid(), vgeditor::borrow_grid);
	}

	vgui_editor_thumbnails* vgui_editor_shell_thumbnails(vgui_editor_shell* shell)
	{
		if (!shell)
			return nullptr;
		/* The cache belongs to the asset browser, which keeps it alive for as long
		 * as the shell does. */
		return cached(shell->thumbnails, shell->impl.assets().thumbnails(),
			vgeditor::borrow_thumbnails);
	}

	/* ------------------------------------------------------------------ */
	/* Render and snap settings                                           */
	/* ------------------------------------------------------------------ */

	void vgui_editor_shell_get_viewport_settings(vgui_editor_shell* shell,
		vgui_editor_viewport_settings* out)
	{
		if (!shell || !out)
			return;
		/* The mirrored request always has concrete values, so the defaults only
		 * supply struct_size. */
		vgui_editor_viewport_settings_default(out);
		const viewport_render_settings& settings = shell->impl.viewport_settings();
		out->hdr = settings.hdr ? 1 : 0;
		out->msaa_samples = settings.msaa_samples;
		out->depth_preview = settings.depth_preview ? 1 : 0;
	}

	void vgui_editor_shell_set_viewport_settings(vgui_editor_shell* shell,
		const vgui_editor_viewport_settings* settings)
	{
		if (!shell || !settings)
			return;
		const vgui_editor_viewport_settings d = vgeditor::take_desc(settings);
		viewport_render_settings next;
		next.hdr = d.hdr != 0;
		/* Stored as requested: the host clamps to what its device supports. */
		next.msaa_samples = d.msaa_samples;
		next.depth_preview = d.depth_preview != 0;
		shell->impl.set_viewport_settings(next);
	}

	void vgui_editor_shell_get_snap(vgui_editor_shell* shell,
		vgui_editor_snap_settings* out)
	{
		if (!shell || !out)
			return;
		vgui_editor_snap_settings_default(out);
		fill_snap(shell->impl.snap(), out);
	}

	void vgui_editor_shell_set_snap(vgui_editor_shell* shell,
		const vgui_editor_snap_settings* settings)
	{
		if (!shell || !settings)
			return;
		/* Assigned through the reference: the gizmo, grid and canvas hold the
		 * shell's policy by address, so the object must not be replaced. */
		shell->impl.snap() = to_snap(settings);
	}

	/* ------------------------------------------------------------------ */
	/* Theme and status                                                   */
	/* ------------------------------------------------------------------ */

	int vgui_editor_shell_high_contrast(vgui_editor_shell* shell)
	{
		return shell && shell->impl.high_contrast() ? 1 : 0;
	}

	void vgui_editor_shell_set_high_contrast(vgui_editor_shell* shell, int enabled)
	{
		if (!shell)
			return;
		shell->impl.set_high_contrast(enabled != 0);
	}

	void vgui_editor_shell_set_status(vgui_editor_shell* shell, const char* text)
	{
		if (!shell)
			return;
		shell->impl.set_status(vgeditor::to_text(text));
	}

} /* extern "C" */
