// vgui_dock_floater_demo.cpp - dock panel tear-off into real OS windows.
//
// Goal B step 1 of docs/dock_multiwindow_assessment.md: a docked panel can
// be dragged out of the main window into its own borderless OS window
// (dock_floater_host, include/vgui/backend/vgui_dock_windows.h) and merged
// back when the floater returns.  The real page stays in the main context
// (widgets cannot move between contexts); the floater shows a host-built
// page and the orphan page is re-added on merge.
//
// Run interactively:      vgui_dock_floater_demo.exe
//   drag a tab out of the window -> borderless floater follows the cursor;
//   drag the floater back (title bar or cursor) -> panel returns to the dock.
// Run as a bounded probe: VGUI_FLOATER_SIM=1 vgui_dock_floater_demo.exe
//   injects the drags, asserts tear-off / merge / restore, exits 0/1.
// The probe needs a real GPU window; it returns 77 (ctest "skip") when the
// GPU device or a window cannot be created.

#include "pch.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <ovg_c.h>
#include <vgui/backend/vgui_dock_windows.h>
#include <vgui/backend/vgui_multiwindow.h>
#include <vgui/backend/vgui_sdl3.h>
#include <vgui/vgui.h>

static int g_fail = 0;
static int g_pass = 0;

static void check(bool ok, const char* what)
{
	if (ok) { ++g_pass; printf("PASS %s\n", what); }
	else { ++g_fail; printf("FAIL %s\n", what); }
}

static vgui_widget* make_colored_page(vgui_context* ui, const char* heading,
	uint32_t bg)
{
	vgui_widget* page = vgui_widget_create(ui, VGUI_PANEL, nullptr);
	vgui_widget_set_layout(page, VGUI_LAYOUT_COLUMN);
	vgui_widget_set_padding(page, 16, 16, 16, 16);
	vgui_widget_set_gap(page, 10);
	vgui_widget_set_background(page, bg);
	vgui_widget* title = vgui_widget_add(page, VGUI_LABEL, heading);
	vgui_widget_set_size(title, 0, 24);
	vgui_widget_set_align_self(title, VGUI_ALIGN_STRETCH);
	vgui_widget_set_text_color(title, 0xFFE6EDF5);
	return page;
}

int main()
{
	setvbuf(stdout, nullptr, _IONBF, 0);

	const char* sim_env = std::getenv("VGUI_FLOATER_SIM");
	const bool self_test = sim_env && std::strcmp(sim_env, "1") == 0;
	const int max_frames = self_test ? 160 : 0;

	auto* wg = new app_mgr();
	wg->set_msaa_samples(4);
	font_familys_t* familys = new_font_family(wg->font_ctx,
		"微软雅黑,Segoe UI,Consolas", 0);
	auto* cb = new_ctx_cb();
	if (!wg->init_gpu(true))
	{
		printf("SKIP init_gpu failed\n");
		delete_font_family(familys);
		free_ctx_cb(cb);
		delete wg;
		return 77;
	}

	const int main_w = 900, main_h = 620;
	os_window* main_win = wg->create("vgui dock floater demo", main_w, main_h,
		0, true);
	if (!main_win)
	{
		printf("SKIP window creation failed\n");
		delete_font_family(familys);
		free_ctx_cb(cb);
		delete wg;
		return 77;
	}

	/* Root must be COLUMN + cross-axis stretch or the dock will not follow
	 * the window size (same trap the editor hit, assessment §11.7). */
	vgui_context* ui = vgui_create(familys, 16.0f);
	vgui_context_set_size(ui, main_w, main_h);
	{
		vgui_widget* root = vgui_root(ui);
		vgui_widget_set_layout(root, VGUI_LAYOUT_COLUMN);
		vgui_widget_set_padding(root, 0, 0, 0, 0);
		vgui_widget_set_gap(root, 0);
		vgui_widget_set_align_items(root, VGUI_ALIGN_STRETCH);
	}
	vgui_widget* dock = vgui_widget_add(vgui_root(ui), VGUI_DOCK, nullptr);
	vgui_widget_set_grow(dock, 1.0f);
	vgui_widget_set_align_self(dock, VGUI_ALIGN_STRETCH);
	vgui_dock_set_zone_size(dock, VGUI_DOCK_BOTTOM, 140.0f);

	struct panel_def
	{
		const char* id;
		const char* title;
		uint32_t bg;
	};
	const panel_def defs[] = {
		{ "p1", "ALPHA", 0xFF3A1B1B },
		{ "p2", "BETA", 0xFF1B3A24 },
		{ "p3", "GAMMA", 0xFF1B243A },
	};
	vgui_item_id panel_ids[3] = {};
	vgui_widget* panel_pages[3] = {};
	for (int i = 0; i < 3; ++i)
	{
		vgui_dock_panel_desc desc;
		vgui_dock_panel_desc_default(&desc);
		desc.id = defs[i].id;
		desc.title = defs[i].title;
		desc.panel = make_colored_page(ui, defs[i].title, defs[i].bg);
		desc.zone = i == 0 ? VGUI_DOCK_LEFT
			: i == 1 ? VGUI_DOCK_CENTER : VGUI_DOCK_BOTTOM;
		panel_ids[i] = vgui_dock_add(dock, &desc);
		panel_pages[i] = desc.panel;
	}

	{
		multiwindow_host host(wg, cb);
		check(host.attach(main_win, ui), "attach main window");
		check(vgui_dock_get_count(dock) == 3, "main dock has 3 panels");

		dock_floater_host floater;
		check(floater.attach(wg, &host, dock, main_win, familys, 16.0f),
			"attach floater host");

		FpsCounter fps;
		int frame = 0;
		bool running = true;
		bool tear_off_checked = false;
		bool merge_checked = false;
		bool second_teared = false;
		vgui_item_id p1 = panel_ids[0];
		vgui_item_id p2 = panel_ids[1];

		while (running)
		{
			if (wg->get_event() < 0)
				running = false;
			const float delta = fps.tick();

			if (self_test)
			{
				/* Frame 5..7: drag the p1 tab out of the window.  Injected
				 * events drive the core drag state machine; crossing the
				 * context bounds fires on_drag_out, releasing outside fires
				 * the tear-off callback (both only queue). */
				if (frame == 5)
				{
					float tx = 0, ty = 0, tw = 0, th = 0;
					vgui_dock_get_tab_rect(dock, p1, &tx, &ty, &tw, &th);
					const float cx = tx + tw * 0.5f;
					const float cy = ty + th * 0.5f;
					vgui_input_mouse_move(ui, cx, cy);
					vgui_input_mouse_down(ui, 1, cx, cy, 1);
					vgui_input_mouse_move(ui, cx + 12.0f, cy);
				}
				else if (frame == 6)
				{
					vgui_input_mouse_move(ui, -60.0f, 40.0f);
				}
				else if (frame == 7)
				{
					vgui_input_mouse_up(ui, 1, -60.0f, 40.0f, 1);
				}
				else if (frame == 12 && !tear_off_checked)
				{
					tear_off_checked = true;
					check(floater.floater_count() == 1 && floater.floating(p1),
						"tear-off creates one floater for p1");
					check(floater.floater_window(p1) != nullptr,
						"floater owns an OS window");
					check(host.size() == 2,
						"floater joined the single-submission host");
					check(vgui_dock_find(dock, "p1") == 0,
						"p1 left the main dock");
					check(vgui_dock_get_count(dock) == 2,
						"main dock keeps the other two panels");
				}
				else if (frame == 20 && !merge_checked)
				{
					/* settle frames (8) have expired: move the floater into
					 * the main window; the move detector (position changed
					 * while the button is up) merges it back. */
					if (os_window* fw_win = floater.floater_window(p1))
					{
						int mx = 0, my = 0;
						SDL_GetWindowPosition(main_win->window, &mx, &my);
						SDL_SetWindowPosition(fw_win->window, mx + 20, my + 20);
					}
				}
				else if (frame == 30 && !merge_checked)
				{
					merge_checked = true;
					check(floater.floater_count() == 0 && !floater.floating(p1),
						"floater merges back into the main dock");
					const vgui_item_id restored = vgui_dock_find(dock, "p1");
					check(restored != 0, "p1 is back in the main dock");
					check(restored != 0 &&
						vgui_dock_get_panel(dock, restored) == panel_pages[0],
						"the real page widget survived the round trip");
					check(vgui_dock_get_count(dock) == 3,
						"main dock back to 3 panels");
				}
				else if (frame == 34)
				{
					/* Second tear-off (idempotency of the queue path). */
					float tx = 0, ty = 0, tw = 0, th = 0;
					vgui_dock_get_tab_rect(dock, p2, &tx, &ty, &tw, &th);
					const float cx = tx + tw * 0.5f;
					const float cy = ty + th * 0.5f;
					vgui_input_mouse_move(ui, cx, cy);
					vgui_input_mouse_down(ui, 1, cx, cy, 1);
					vgui_input_mouse_move(ui, cx + 12.0f, cy);
				}
				else if (frame == 35)
				{
					vgui_input_mouse_move(ui, main_w + 60.0f, 40.0f);
				}
				else if (frame == 36)
				{
					vgui_input_mouse_up(ui, 1, main_w + 60.0f, 40.0f, 1);
				}
				else if (frame == 42 && !second_teared)
				{
					second_teared = true;
					check(floater.floater_count() == 1 && floater.floating(p2),
						"second tear-off floats p2");
					check(vgui_dock_find(dock, "p2") == 0,
						"p2 left the main dock");
				}
			}

			floater.tick();
			host.begin_frame(delta);
			host.draw_all();
			host.end_frame();

			++frame;
			if (max_frames > 0 && frame >= max_frames)
				running = false;
		}

		if (self_test)
		{
			check(second_teared && floater.floating(p2) &&
				floater.floater_count() == 1,
				"p2 stays floating at exit (manual merge path untouched)");
			check(vgui_dock_get_count(dock) == 2 && vgui_dock_find(dock, "p1")
				!= 0 && vgui_dock_find(dock, "p3") != 0,
				"main dock holds p1 + p3 at exit");
		}
		else
		{
			printf("interactive mode: drag a tab out of the window and back.\n");
		}

		/* The floater host destroys its own windows/contexts on scope exit
		 * (before the multiwindow_host drops the remaining main-window slot).
		 * The main window's context is destroyed manually -- manual-destroy
		 * pattern so ~gui_viewport does not double-free it. */
		check(host.size() >= 1, "main window slot alive at teardown");
		const vgui_context* main_ui = ui;
		for (const multiwindow_slot& s : host.slots())
		{
			if (s.ui == main_ui)
			{
				vgui_destroy(s.ui);
				s.window->viewport->ui = nullptr;
			}
		}
	} // host + floater destroyed: canvases freed while cb is alive

	wg->wait_gpu();
	delete_font_family(familys);
	free_ctx_cb(cb);
	delete wg;

	printf("SELFTEST %s (%d pass, %d fail)\n", g_fail ? "FAIL" : "PASS",
		g_pass, g_fail);
	return g_fail ? 1 : 0;
}
