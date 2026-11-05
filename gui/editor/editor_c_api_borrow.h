#pragma once
/*
 * editor_c_api_borrow.h - internal: borrowed C handles for components that
 * belong to another object rather than to the host.
 *
 * The editor shell owns its panels (outliner, viewport helpers, property grid,
 * the seven dock views) and the asset browser owns a thumbnail cache, but a C
 * host still needs to drive them.  Each factory allocates the same handle the
 * corresponding vgui_editor_*_create() would, binds it to the existing C++
 * object and marks it borrowed, so destroying the handle leaves the object
 * alone.  The shell caches the wrappers it hands out, which keeps the pointers
 * a host receives stable.
 *
 * Every factory is defined in the translation unit that declares the handle
 * layout; only <vgui/vgui_editor.h> users see the opaque types.
 */
#include <vgui/vgui_editor.h>

#include <vgui/editor/vgui_editor_kit.h>

namespace vek = vgui_editor_kit;

namespace vgeditor {

	vgui_editor_outliner* borrow_outliner(vek::outliner_view& view);
	vgui_editor_command_registry* borrow_command_registry(
		vek::command_registry& registry);
	vgui_editor_property_grid* borrow_property_grid(vek::property_grid& grid);
	vgui_editor_asset_browser* borrow_asset_browser(vek::asset_browser& browser);
	vgui_editor_asset_preview* borrow_asset_preview(vek::asset_preview& preview);
	vgui_editor_thumbnails* borrow_thumbnails(vek::thumbnail_cache& cache);
	vgui_editor_console* borrow_console(vek::console_view& console);
	vgui_editor_canvas* borrow_canvas(vek::canvas_view& canvas);
	vgui_editor_table* borrow_table(vek::table_view& table);
	vgui_editor_timeline* borrow_timeline(vek::timeline_view& timeline);
	vgui_editor_node_graph* borrow_node_graph(vek::node_graph_view& graph);
	vgui_editor_code_editor* borrow_code_editor(vek::code_editor& editor);
	vgui_editor_toolbar* borrow_toolbar(vek::toolbar_view& toolbar);
	vgui_editor_status_bar* borrow_status_bar(vek::status_bar& bar);
	vgui_editor_search_box* borrow_search_box(vek::search_box& search);
	vgui_editor_breadcrumb* borrow_breadcrumb(vek::breadcrumb& path);
	vgui_editor_viewport* borrow_viewport(vek::viewport_view& viewport);
	vgui_editor_camera* borrow_camera(vek::viewport_camera& camera);
	vgui_editor_grid* borrow_grid(vek::viewport_grid& grid);
	vgui_editor_picking* borrow_picking(vek::viewport_selection& selection);
	vgui_editor_gizmo* borrow_gizmo(vek::viewport_gizmo& gizmo);

	/* Releases one wrapper created by a borrow factory above; the C++ object it
	 * points at stays with its owner.  A container that caches borrowed handles
	 * (the editor shell) calls these when it dies, which is why the factories
	 * hand back a plain pointer and the destroy() entry points stay no-ops for
	 * borrowed handles. */
	void release_borrowed_outliner(vgui_editor_outliner* handle);
	void release_borrowed_command_registry(vgui_editor_command_registry* handle);
	void release_borrowed_property_grid(vgui_editor_property_grid* handle);
	void release_borrowed_asset_browser(vgui_editor_asset_browser* handle);
	void release_borrowed_asset_preview(vgui_editor_asset_preview* handle);
	void release_borrowed_thumbnails(vgui_editor_thumbnails* handle);
	void release_borrowed_console(vgui_editor_console* handle);
	void release_borrowed_canvas(vgui_editor_canvas* handle);
	void release_borrowed_table(vgui_editor_table* handle);
	void release_borrowed_timeline(vgui_editor_timeline* handle);
	void release_borrowed_node_graph(vgui_editor_node_graph* handle);
	void release_borrowed_code_editor(vgui_editor_code_editor* handle);
	void release_borrowed_toolbar(vgui_editor_toolbar* handle);
	void release_borrowed_status_bar(vgui_editor_status_bar* handle);
	void release_borrowed_search_box(vgui_editor_search_box* handle);
	void release_borrowed_breadcrumb(vgui_editor_breadcrumb* handle);
	void release_borrowed_viewport(vgui_editor_viewport* handle);
	void release_borrowed_camera(vgui_editor_camera* handle);
	void release_borrowed_grid(vgui_editor_grid* handle);
	void release_borrowed_picking(vgui_editor_picking* handle);
	void release_borrowed_gizmo(vgui_editor_gizmo* handle);

} /* namespace vgeditor */
