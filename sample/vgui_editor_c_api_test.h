/* sample/vgui_editor_c_api_test.h - shared harness for the pure-C editor facade
 * smoke test.
 *
 * vgui_editor_c_api_test.c holds the harness (check()/frame()/probe) and the
 * component tests for the foundations, widgets and editors; the remaining
 * component groups live in vgui_editor_c_api_test_views.c,
 * vgui_editor_c_api_test_data.c and vgui_editor_c_api_test_shell.c and are
 * driven from the same main().  Everything here compiles as C99, which is what
 * proves <vgui/vgui_editor.h> has no C++ dependency. */
#ifndef VGUI_EDITOR_C_API_TEST_H
#define VGUI_EDITOR_C_API_TEST_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vgui/vgui_editor.h>

extern int g_pass;
extern int g_fail;

/* Headless UI context every test builds its widgets under. */
extern vgui_context* g_ui;

/* Records one PASS/FAIL line. */
void check(int ok, const char* what);

/* Runs `count` vgui_update() ticks so layout, timers and deferred work happen. */
void frame(int count);

/* Callback bookkeeping shared by the component tests.  The fields are reused
 * per component, so a test that needs several independent counters declares its
 * own (see the `counter` type in the main file). */
typedef struct probe {
	int change_count;
	int commit_count;
	double last_value;
	int combo_index;
	char combo_value[64];
	int navigate_count;
	size_t navigate_index;
	char navigate_id[64];
	int click_count;
	int toggle_count;
	int action_count;
	int selection_count;
	int selection_set_count;
	vgui_editor_object_id last_selection;
	int released;
} probe;

/* Views: outliner, table, timeline, node graph and 2D canvas. */
void test_outliner(probe* p);
void test_table(probe* p);
void test_timeline(probe* p);
void test_node_graph(probe* p);
void test_canvas(probe* p);

/* Data surfaces: code editor, markdown, thumbnails, asset browser and preview. */
void test_code_editor(probe* p);
void test_markdown(probe* p);
void test_thumbnails(probe* p);
void test_asset_browser(probe* p);
void test_asset_preview(probe* p);

/* Viewport group and the shell that owns one of everything. */
void test_viewport(probe* p);
void test_camera_grid_picking(probe* p);
void test_gizmo(probe* p);
void test_shell(probe* p);

#endif /* VGUI_EDITOR_C_API_TEST_H */
