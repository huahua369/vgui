// vgui_crosswindow_dock_demo.cpp - cross-window Dock drop target resolution
// and panel migration (goal C of docs/dock_multiwindow_assessment.md).
//
// C1: register two Dock-bearing windows as drop targets, synthesise screen-
// point drop positions from each Dock's zone/tab geometry, and assert
// resolve_drop() picks the right window / zone / tab index.  Covers
// prune/unregister and the "outside all windows returns invalid" case.
// C2: tear a panel off window A into a floater, then migrate it into window B's
// Dock at a chosen zone/index via a deterministic request; asserts the panel
// leaves the floater, appears in B, the source Dock loses it, and a forced
// failure rolls the panel back to A.
// D1 (assessment §14): one-shot continuous drag -- a still-docked panel released
// over B migrates straight there with NO floating window (continuous_docking),
// plus an end-to-end case driven through the real core vgui_input_* chain.
// D2: target-dock drop compass set/switch/clear via the core preview API.
//
// Run interactively:      vgui_crosswindow_dock_demo.exe
// Run as a bounded probe: VGUI_XDOCK_SIM=1 vgui_crosswindow_dock_demo.exe
//   injects the queries + migrations, asserts, exits 0 (pass) / 1 (fail).
//   Needs a real GPU + window; returns 77 when GPU/window creation fails.

#include "pch.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <ovg_c.h>
#include <vgui/backend/vgui_crosswindow_dock.h>
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

struct dock_window
{
	os_window* win = nullptr;
	vgui_context* ui = nullptr;
	vgui_widget* dock = nullptr;
	const char* id = nullptr;
};

static vgui_widget* make_colored_page(vgui_context* ui, const char* heading,
	uint32_t bg)
{
	vgui_widget* page = vgui_widget_create(ui, VGUI_PANEL, nullptr);
	vgui_widget_set_layout(page, VGUI_LAYOUT_COLUMN);
	vgui_widget_set_padding(page, 16, 16, 16, 16);
	vgui_widget_set_background(page, bg);
	vgui_widget* title = vgui_widget_add(page, VGUI_LABEL, heading);
	vgui_widget_set_size(title, 0, 24);
	vgui_widget_set_text_color(title, 0xFFE6EDF5);
	return page;
}

static bool build_dock_window(app_mgr* wg, font_familys_t* familys,
	const char* title, int w, int h, int x, int y, const char* id,
	dock_window& out)
{
	out.win = wg->create(title, w, h, 0, true);
	if (!out.win) return false;
	SDL_SetWindowPosition(out.win->window, x, y);

	out.ui = vgui_create(familys, 16.0f);
	vgui_context_set_size(out.ui, w, h);
	vgui_widget* root = vgui_root(out.ui);
	vgui_widget_set_layout(root, VGUI_LAYOUT_COLUMN);
	vgui_widget_set_padding(root, 0, 0, 0, 0);
	vgui_widget_set_gap(root, 0);
	vgui_widget_set_align_items(root, VGUI_ALIGN_STRETCH);

	out.dock = vgui_widget_add(root, VGUI_DOCK, nullptr);
	vgui_widget_set_grow(out.dock, 1.0f);
	vgui_widget_set_align_self(out.dock, VGUI_ALIGN_STRETCH);
	vgui_dock_set_zone_size(out.dock, VGUI_DOCK_BOTTOM, 140.0f);
	out.id = id;
	return true;
}

struct probe_state
{
	crosswindow_dock_host* xdock = nullptr;
	vgui_context* foreign_ui = nullptr;	/* source ctx -> cross-ctx page -> add fails */
	bool force_fail = false;	/* make migrate_page return a cross-ctx page */
	int migrated_calls = 0;
	int last_panel = 0;
	crosswindow_drop_result last_drop;
	/* C3: complex-page resource lifecycle -- verify hooks run in the RIGHT
	 * context (mirrors GAME VIEW native-texture release / rebuild). */
	int teardown_calls = 0;
	int closing_calls = 0;
	vgui_context* last_migrate_ui = nullptr;
	vgui_context* last_restore_ui = nullptr;
	/* D1: how many times the release-point resolver was invoked. */
	int resolve_calls = 0;
};

/* C2 hooks.  resolve just forwards to the C1 component. */
static bool probe_resolve(float sx, float sy, crosswindow_drop_result& out,
	void* user)
{
	auto* st = static_cast<probe_state*>(user);
	if (st) ++st->resolve_calls;
	return st && st->xdock && st->xdock->resolve_drop(sx, sy, out);
}

static vgui_widget* probe_migrate_page(vgui_context* target_ui,
	vgui_widget* target_dock, const dock_floater_info& info,
	const crosswindow_drop_result& drop, void* user)
{
	(void)drop;
	auto* st = static_cast<probe_state*>(user);
	if (!st) return nullptr;
	st->last_migrate_ui = target_ui;
	if (st->force_fail && st->foreign_ui)
		/* A page built in the SOURCE context: vgui_dock_add() on the target
		 * dock rejects cross-context pages -> migrate_to rolls back. */
		return make_colored_page(st->foreign_ui, info.title, 0xFF3A1B1B);
	/* Rebuild a real page in the TARGET context (widgets cannot cross ctx). */
	return make_colored_page(target_ui, info.title, 0xFF243A1B);
}

/* C3: GAME VIEW analogue -- release the native resource registered against
 * the source context BEFORE the panel leaves it. */
static void probe_teardown(const dock_floater_info& info, void* user)
{
	(void)info;
	auto* st = static_cast<probe_state*>(user);
	if (st) ++st->teardown_calls;
}

static void probe_closing(const dock_floater_info& info, void* user)
{
	(void)info;
	auto* st = static_cast<probe_state*>(user);
	if (st) ++st->closing_calls;
}

static vgui_widget* probe_restore_page(vgui_widget* main_dock,
	const dock_floater_info& info, vgui_widget* orphan_page, void* user)
{
	(void)orphan_page;
	auto* st = static_cast<probe_state*>(user);
	/* Rebuild a real page in the SOURCE context on rollback. */
	vgui_context* src = vgui_widget_get_context(main_dock);
	if (st) st->last_restore_ui = src;
	return make_colored_page(src, info.title, 0xFF3A1B1B);
}

static void probe_migrated(const dock_floater_info& info,
	const crosswindow_drop_result& drop, vgui_item_id panel,
	vgui_widget* page, void* user)
{
	(void)info;
	(void)page;
	auto* st = static_cast<probe_state*>(user);
	if (!st) return;
	++st->migrated_calls;
	st->last_drop = drop;
	st->last_panel = panel;
}

int main()
{
	setvbuf(stdout, nullptr, _IONBF, 0);

	const char* sim_env = std::getenv("VGUI_XDOCK_SIM");
	const bool self_test = sim_env && std::strcmp(sim_env, "1") == 0;

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

	/* Two non-overlapping windows; create() ignores x/y for regular windows,
	 * so position explicitly for unambiguous screen-point hits. */
	dock_window a, b;
	if (!build_dock_window(wg, familys, "vgui crosswindow dock - A",
		900, 620, 40, 60, "win-a", a) ||
		!build_dock_window(wg, familys, "vgui crosswindow dock - B",
			760, 520, 1000, 90, "win-b", b))
	{
		printf("SKIP window creation failed\n");
		delete_font_family(familys);
		free_ctx_cb(cb);
		delete wg;
		return 77;
	}

	struct def { const char* id; const char* title; uint32_t bg; vgui_dock_zone zone; };
	const def defs_a[] = {
		{ "a-left",  "A LEFT",  0xFF3A1B1B, VGUI_DOCK_LEFT },
		{ "a-ctr-1", "A C1",    0xFF1B243A, VGUI_DOCK_CENTER },
		{ "a-ctr-2", "A C2",    0xFF2A243A, VGUI_DOCK_CENTER },
		{ "a-ctr-3", "A C3",    0xFF1B3A3A, VGUI_DOCK_CENTER }, /* D1 continuous */
		{ "a-ctr-4", "A C4",    0xFF3A3A1B, VGUI_DOCK_CENTER }, /* D1 rollback  */
		{ "a-e2e",   "A E2E",   0xFF2A3A2A, VGUI_DOCK_CENTER }, /* D1 real drag */
		{ "a-bot",   "A BOTTOM",0xFF1B3A24, VGUI_DOCK_BOTTOM },
	};
	const def defs_b[] = {
		{ "b-right", "B RIGHT", 0xFF3A2B1B, VGUI_DOCK_RIGHT },
		{ "b-ctr",   "B CENTER",0xFF1B2A3A, VGUI_DOCK_CENTER },
	};
	for (const def& d : defs_a)
	{
		vgui_dock_panel_desc desc;
		vgui_dock_panel_desc_default(&desc);
		desc.id = d.id;
		desc.title = d.title;
		desc.panel = make_colored_page(a.ui, d.title, d.bg);
		desc.zone = d.zone;
		vgui_dock_add(a.dock, &desc);
	}
	for (const def& d : defs_b)
	{
		vgui_dock_panel_desc desc;
		vgui_dock_panel_desc_default(&desc);
		desc.id = d.id;
		desc.title = d.title;
		desc.panel = make_colored_page(b.ui, d.title, d.bg);
		desc.zone = d.zone;
		vgui_dock_add(b.dock, &desc);
	}

	{
		multiwindow_host host(wg, cb);
		check(host.attach(a.win, a.ui), "attach window A");
		check(host.attach(b.win, b.ui), "attach window B");
		check(host.size() == 2, "two slots share one submit path");

		crosswindow_dock_host xdock(wg);
		check(xdock.register_target(a.win, a.dock, a.id, a.ui), "register target A");
		check(xdock.register_target(b.win, b.dock, b.id, b.ui), "register target B");
		check(xdock.target_count() == 2, "two drop targets registered");

		probe_state probe;
		probe.xdock = &xdock;
		probe.foreign_ui = a.ui;

		dock_floater_host::hooks fh;
		fh.resolve_target = probe_resolve;
		fh.migrate_page = probe_migrate_page;
		fh.restore_page = probe_restore_page;
		fh.teardown_page = probe_teardown;
		fh.floater_closing = probe_closing;
		fh.migrated = probe_migrated;
		fh.crosswindow_user = &probe;
		fh.lifecycle_user = &probe;

		dock_floater_opts fopts;
		fopts.continuous_docking = true;	/* D1: enable one-shot direct migration */
		dock_floater_host floater;
		check(floater.attach(wg, &host, a.dock, a.win, familys, 16.0f,
			fopts, fh), "attach floater host on window A");

		/* Force one layout pass so zone/tab rects are valid before synthesis. */
		host.begin_frame(1.0f / 60.0f);
		host.draw_all();

		if (self_test)
		{
			/* ---------- C1: pure hit-test queries ---------- */
			crosswindow_drop_result r;
			float sx = 0, sy = 0;
			check(xdock.screen_point_for(a.win, a.dock, VGUI_DOCK_LEFT, -1, sx, sy),
				"synthesise screen point for A.LEFT");
			check(xdock.resolve_drop(sx, sy, r), "resolve A.LEFT point hits a dock");
			check(r.window == a.win && r.dock == a.dock, "A.LEFT resolves to window A");
			check(r.ui == a.ui, "A.LEFT result carries target context");
			check(r.window_id == "win-a", "A.LEFT carries owner id win-a");
			check(r.zone == VGUI_DOCK_LEFT, "A.LEFT resolves to LEFT zone");

			check(xdock.screen_point_for(a.win, a.dock, VGUI_DOCK_CENTER, 1, sx, sy),
				"synthesise screen point for A.CENTER tab 1");
			check(xdock.resolve_drop(sx, sy, r), "resolve A.CENTER tab hits a dock");
			check(r.zone == VGUI_DOCK_CENTER && r.index == 1,
				"A.CENTER tab 1 resolves to zone CENTER index 1");

			check(xdock.screen_point_for(b.win, b.dock, VGUI_DOCK_RIGHT, -1, sx, sy),
				"synthesise screen point for B.RIGHT");
			check(xdock.resolve_drop(sx, sy, r), "resolve B.RIGHT point hits a dock");
			check(r.window == b.win && r.window_id == "win-b",
				"B.RIGHT resolves to window B");
			check(r.zone == VGUI_DOCK_RIGHT, "B.RIGHT resolves to RIGHT zone");

			check(xdock.screen_point_for(a.win, a.dock, VGUI_DOCK_BOTTOM, -1, sx, sy),
				"synthesise screen point for A.BOTTOM");
			check(xdock.resolve_drop(sx, sy, r), "resolve A.BOTTOM point hits a dock");
			check(r.zone == VGUI_DOCK_BOTTOM, "A.BOTTOM resolves to BOTTOM zone");

			check(!xdock.resolve_drop(-5000.0f, -5000.0f, r),
				"point outside all windows returns invalid");
			check(!r.valid(), "invalid result has no dock");

			/* ---------- C2: cross-window migration ---------- */
			const int a_count_before = vgui_dock_get_count(a.dock);
			const int b_count_before = vgui_dock_get_count(b.dock);
			const vgui_item_id a_ctr1 = vgui_dock_find(a.dock, "a-ctr-1");
			check(a_ctr1 != 0, "find source panel a-ctr-1 in dock A");

			/* Queue a tear-off, then a migration into B's LEFT zone.  The
			 * floater host processes one tear-off then the migration in tick(). */
			floater.request_tear_off(a_ctr1);
			floater.tick();
			check(floater.floating(a_ctr1), "panel torn off into a floater");
			check(vgui_dock_get_count(a.dock) == a_count_before - 1,
				"source dock A lost the torn-off panel");

			crosswindow_drop_result to_b;
			check(xdock.screen_point_for(b.win, b.dock, VGUI_DOCK_CENTER, -1,
				sx, sy), "synthesise drop point inside window B");
			check(xdock.resolve_drop(sx, sy, to_b) &&
				to_b.dock == b.dock && to_b.ui == b.ui,
				"drop resolves to window B target context");
			to_b.zone = VGUI_DOCK_CENTER;	/* deterministic target zone */
			to_b.index = -1;

			floater.request_migrate(a_ctr1, to_b);
			floater.tick();
			check(!floater.floating(a_ctr1), "panel no longer floating after migration");
			check(probe.migrated_calls == 1, "migrated hook fired once");
			check(probe.last_drop.window == b.win, "migrated hook saw target window B");
			check(vgui_dock_get_count(b.dock) == b_count_before + 1,
				"target dock B gained the panel");
			check(vgui_dock_find(b.dock, "a-ctr-1") != 0,
				"migrated panel keeps its stable string id in B");
			check(vgui_dock_get_zone(b.dock, vgui_dock_find(b.dock, "a-ctr-1"))
				== VGUI_DOCK_CENTER, "migrated panel landed in B's CENTER zone");

			/* C3: complex-page resource lifecycle across the context switch. */
			check(probe.last_migrate_ui == b.ui,
				"migrate_page rebuilt the page in the TARGET context B");
			check(probe.teardown_calls >= 1,
				"teardown_page released source-context resource before leaving");
			check(probe.closing_calls >= 1,
				"floater_closing released floating-context resource before destroy");

			/* Forced failure -> rollback.  Tear off a second panel, then make
			 * migrate_page return a page built in the SOURCE context; core
			 * refuses to add a cross-context page to B, so migrate_to rolls it
			 * back into A via the restore path. */
			const vgui_item_id a_ctr2 = vgui_dock_find(a.dock, "a-ctr-2");
			check(a_ctr2 != 0, "find source panel a-ctr-2 in dock A");
			floater.request_tear_off(a_ctr2);
			floater.tick();
			check(floater.floating(a_ctr2), "second panel torn off for rollback test");

			probe.force_fail = true;
			crosswindow_drop_result to_b2 = to_b;
			floater.request_migrate(a_ctr2, to_b2);
			floater.tick();
			probe.force_fail = false;
			check(!floater.floating(a_ctr2), "failed migration consumed the floater");
			check(probe.last_panel == 0, "migrated hook reports panel 0 on failure");
			check(vgui_dock_find(a.dock, "a-ctr-2") != 0,
				"failed migration rolled panel back into source dock A");
			check(vgui_dock_find(b.dock, "a-ctr-2") == 0,
				"rolled-back panel is absent from target dock B");
			check(probe.last_restore_ui == a.ui,
				"restore_page rebuilt the page in the SOURCE context A");

			/* Explicit-request guard: a drop with no target ui is not queued. */
			const size_t pend = floater.pending_migration_count();
			crosswindow_drop_result stale;
			stale.dock = b.dock; stale.window = b.win; stale.ui = nullptr;
			floater.request_migrate(a_ctr1, stale);
			check(floater.pending_migration_count() == pend,
				"invalid drop (no target ui) is not queued");

			/* ---------- D1: one-shot continuous drag (no floater) ---------- */
			/* A still-docked panel released over B's Dock migrates straight
			 * there: no floating window is ever created. */
			const int b_count_d1 = vgui_dock_get_count(b.dock);
			const int a_count_d1 = vgui_dock_get_count(a.dock);
			const vgui_item_id a_ctr3 = vgui_dock_find(a.dock, "a-ctr-3");
			check(a_ctr3 != 0 && !floater.floating(a_ctr3),
				"D1 source panel a-ctr-3 is still docked (no floater)");
			const int resolve_before = probe.resolve_calls;
			floater.request_direct_migrate(a_ctr3, sx, sy); /* sx,sy = B point */
			check(floater.pending_direct_migration_count() == 1,
				"D1 direct migration queued from a docked panel");
			floater.tick();
			check(probe.resolve_calls >= resolve_before + 1,
				"D1 release point re-resolved to a target");
			check(!floater.floating(a_ctr3),
				"D1 no floating window created for direct migration");
			check(vgui_dock_get_count(a.dock) == a_count_d1 - 1,
				"D1 source dock A lost the panel directly");
			check(vgui_dock_get_count(b.dock) == b_count_d1 + 1,
				"D1 target dock B gained the panel directly");
			check(vgui_dock_find(b.dock, "a-ctr-3") != 0,
				"D1 directly migrated panel keeps its stable string id");
			check(probe.last_migrate_ui == b.ui,
				"D1 page rebuilt in target context B");

			/* D1 miss: a screen point outside every window leaves the panel
			 * exactly where it was (no detach, no loss). */
			const int a_count_miss = vgui_dock_get_count(a.dock);
			const vgui_item_id a_ctr4 = vgui_dock_find(a.dock, "a-ctr-4");
			check(a_ctr4 != 0, "D1 source panel a-ctr-4 present for miss test");
			floater.request_direct_migrate(a_ctr4, -5000.0f, -5000.0f);
			floater.tick();
			check(vgui_dock_get_count(a.dock) == a_count_miss,
				"D1 miss leaves source dock panel count unchanged");
			check(vgui_dock_find(a.dock, "a-ctr-4") != 0 &&
				!floater.floating(a_ctr4),
				"D1 miss keeps the panel docked in A");

			/* D1 failure: target refuses a cross-context page -> rollback. */
			const int b_count_fail = vgui_dock_get_count(b.dock);
			probe.force_fail = true;
			floater.request_direct_migrate(a_ctr4, sx, sy);
			floater.tick();
			probe.force_fail = false;
			check(vgui_dock_get_count(b.dock) == b_count_fail,
				"D1 failed direct migration did not add to target B");
			check(vgui_dock_find(a.dock, "a-ctr-4") != 0,
				"D1 failed direct migration rolled back into source A");
			check(probe.last_restore_ui == a.ui,
				"D1 rollback rebuilt the page in source context A");

			/* D1 end-to-end: drive the REAL core drag state machine.  Under SDL
			 * capture the mouse-up lands on the SOURCE context with a local
			 * coord whose screen equivalent is over window B; core detects the
			 * out-of-window release and fires on_tear_off, whose continuous
			 * branch must redirect to a direct migration (no floater). */
			{
				const vgui_item_id panel = vgui_dock_find(a.dock, "a-e2e");
				check(panel != 0, "D1 e2e panel a-e2e present");
				host.begin_frame(1.0f / 60.0f);
				host.draw_all();
				float tx = 0, ty = 0, tw = 0, th = 0;
				check(vgui_dock_get_tab_rect(a.dock, panel, &tx, &ty, &tw, &th) && tw > 0,
					"D1 e2e source tab rect available");
				const float cx = tx + tw * 0.5f, cy = ty + th * 0.5f;
				/* Screen point inside B, converted to A-local render px. */
				float bsx = 0, bsy = 0;
				check(xdock.screen_point_for(b.win, b.dock, VGUI_DOCK_CENTER, -1,
					bsx, bsy), "D1 e2e synthesise B screen point");
				float lx = 0, ly = 0;
				vgui_dock_coords::screen_to_render(a.win, bsx, bsy, lx, ly);
				const int b_count_e2e = vgui_dock_get_count(b.dock);
				const int migrations_before = probe.migrated_calls;

				vgui_input_mouse_move(a.ui, cx, cy);
				vgui_input_mouse_down(a.ui, 1, cx, cy, 1);
				vgui_input_mouse_move(a.ui, cx + 14.0f, cy); /* cross threshold */
				vgui_input_mouse_move(a.ui, lx, ly);          /* over B */
				vgui_input_mouse_up(a.ui, 1, lx, ly, 1);      /* release */
				floater.tick();

				check(probe.migrated_calls == migrations_before + 1,
					"D1 e2e real mouse-up fired direct migration");
				check(!floater.floating(panel),
					"D1 e2e no floating window created");
				check(vgui_dock_find(b.dock, "a-e2e") != 0,
					"D1 e2e panel landed in target dock B");
				check(vgui_dock_get_count(b.dock) == b_count_e2e + 1,
					"D1 e2e target dock count increased");
				check(vgui_dock_find(a.dock, "a-e2e") == 0,
					"D1 e2e panel left source dock A");
			}

			/* ---------- D2: target-dock drop compass wiring ---------- */
			/* The host paints a preview on the TARGET dock via the core API.
			 * Verify target tracking, target switching and clear. */
			float hlx = 0, hly = 0;
			check(xdock.screen_point_for(b.win, b.dock, VGUI_DOCK_CENTER, -1,
				hlx, hly), "D2 synthesise target screen point");
			crosswindow_drop_result hl;
			check(xdock.resolve_drop(hlx, hly, hl) && hl.dock == b.dock,
				"D2 resolve preview target dock B");
			hl.zone = VGUI_DOCK_CENTER;
			hl.index = 0;
			floater.set_drop_preview(hl);
			check(floater.drop_preview_dock() == b.dock,
				"D2 set_drop_preview tracks target dock B");
			floater.set_drop_preview(hl); /* idempotent re-set */
			check(floater.drop_preview_dock() == b.dock,
				"D2 re-setting the same target keeps one preview dock");
			floater.clear_drop_preview();
			check(floater.drop_preview_dock() == nullptr,
				"D2 clear_drop_preview releases the target dock");

			printf("SELFTEST %s (%d pass, %d fail)\n",
				g_fail ? "FAIL" : "PASS", g_pass, g_fail);
		}
		else
		{
			FpsCounter fps;
			bool running = true;
			while (running)
			{
				if (wg->get_event() < 0) break;
				const float delta = fps.tick();
				floater.tick();
				host.begin_frame(delta);
				host.draw_all();
				host.end_frame();
			}
			(void)running;
		}

		floater.close_all();
	}

	a.win->viewport->ui = nullptr;
	b.win->viewport->ui = nullptr;
	vgui_destroy(a.ui);
	vgui_destroy(b.ui);
	delete_font_family(familys);
	free_ctx_cb(cb);
	delete wg;

	return g_fail ? 1 : 0;
}
