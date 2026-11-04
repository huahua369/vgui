// vgui_multiwindow_demo.cpp - two independent OS windows, one Dock each.
//
// Goal A of docs/dock_multiwindow_assessment.md: prove that the SDL3 backend
// can drive several windows (each with its own vgui_context / FBO / rvg) in a
// single frame, and that libvgui's Dock stays independent per context.
//
// Run interactively:      vgui_multiwindow_demo.exe
// Run as a bounded probe: VGUI_MW_FRAMES=90 vgui_multiwindow_demo.exe
//   (renders 90 frames, prints SELFTEST lines, exits with 0/1)
//
// The probe needs a real GPU window; it returns 77 (ctest "skip") when the
// GPU device or a window cannot be created.

#include "pch.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <ovg_c.h>
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

struct window_demo
{
	vgui_context* ui = nullptr;
	vgui_widget* dock = nullptr;
	vgui_item_id first_panel = 0;
};

static vgui_widget* make_page(vgui_context* ui, const char* heading, const char* body)
{
	vgui_widget* page = vgui_widget_create(ui, VGUI_PANEL, nullptr);
	vgui_widget_set_layout(page, VGUI_LAYOUT_ABSOLUTE);
	vgui_widget* title = vgui_widget_add(page, VGUI_LABEL, heading);
	vgui_widget_set_pos(title, 18.0f, 16.0f);
	vgui_widget_set_size(title, 420.0f, 30.0f);
	vgui_widget* text = vgui_widget_add(page, VGUI_LABEL, body);
	vgui_widget_set_pos(text, 18.0f, 54.0f);
	vgui_widget_set_size(text, 420.0f, 60.0f);
	return page;
}

// The two windows get a different number of panels so their recorded draw
// lists are observably different (proving the canvases are independent).
static window_demo build_window(font_familys_t* familys, int w, int h,
	const char* tag, int panel_count)
{
	window_demo d;
	d.ui = vgui_create(familys, 16.0f);
	vgui_context_set_size(d.ui, w, h);
	vgui_widget_set_layout(vgui_root(d.ui), VGUI_LAYOUT_ABSOLUTE);

	d.dock = vgui_widget_add(vgui_root(d.ui), VGUI_DOCK, nullptr);
	vgui_widget_set_pos(d.dock, 0.0f, 0.0f);
	vgui_widget_set_size(d.dock, (float)w, (float)h);
	vgui_dock_set_zone_size(d.dock, VGUI_DOCK_LEFT, 190.0f);
	vgui_dock_set_zone_size(d.dock, VGUI_DOCK_RIGHT, 170.0f);
	vgui_dock_set_zone_size(d.dock, VGUI_DOCK_BOTTOM, 120.0f);

	static const vgui_dock_zone zones[] = {
		VGUI_DOCK_LEFT, VGUI_DOCK_CENTER, VGUI_DOCK_BOTTOM, VGUI_DOCK_RIGHT };
	static const char* zone_names[] = { "LEFT", "CENTER", "BOTTOM", "RIGHT" };

	for (int i = 0; i < panel_count; ++i)
	{
		vgui_dock_panel_desc desc;
		vgui_dock_panel_desc_default(&desc);
		std::string id = std::string(tag) + "-panel-" + std::to_string(i);
		std::string title = std::string(tag) + " / " + zone_names[i];
		std::string heading = title + " — 独立 vgui_context";
		std::string body = std::string("窗口 ") + tag + " 的第 " + std::to_string(i + 1)
			+ " 个面板。拖标签可在本窗口内重排、跨区或浮出。";
		desc.id = id.c_str();
		desc.title = title.c_str();
		desc.panel = make_page(d.ui, heading.c_str(), body.c_str());
		desc.zone = zones[i];
		vgui_item_id item = vgui_dock_add(d.dock, &desc);
		if (i == 0) d.first_panel = item;
	}
	return d;
}

int main()
{
	setvbuf(stdout, nullptr, _IONBF, 0);

	int max_frames = 0;
	if (const char* env = std::getenv("VGUI_MW_FRAMES"); env && *env)
		max_frames = std::atoi(env);
	const bool probe = max_frames > 0;

	int samples = 4;
	if (const char* env = std::getenv("VGUI_MW_MSAA"); env && *env)
	{
		const int v = std::atoi(env);
		if (v == 1 || v == 2 || v == 4 || v == 8) samples = v;
	}

	auto* wg = new app_mgr();
	wg->set_msaa_samples(samples);
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
	printf("MSAA: %dx\n", wg->msaa_samples);

	const int wa_w = 900, wa_h = 620, wb_w = 720, wb_h = 480;
	os_window* win_a = wg->create("vgui multiwindow A (Dock)", wa_w, wa_h, 0, true);
	os_window* win_b = wg->create("vgui multiwindow B (Dock)", wb_w, wb_h, 0, true);
	if (!win_a || !win_b)
	{
		printf("SKIP window creation failed\n");
		delete_font_family(familys);
		free_ctx_cb(cb);
		delete wg;
		return 77;
	}
	// create() has no x/y parameters, so offset the second window explicitly.
	SDL_SetWindowPosition(win_b->window, 970, 130);

	window_demo a = build_window(familys, wa_w, wa_h, "A", 3);
	window_demo b = build_window(familys, wb_w, wb_h, "B", 2);

	{
		multiwindow_host host(wg, cb);
		check(host.attach(win_a, a.ui), "attach window A");
		check(host.attach(win_b, b.ui), "attach window B");
		check(wg->window_count() == 2, "app_mgr owns two OS windows");
		check(wg->main_window == win_a, "first window becomes main_window");
		check(win_a->viewport->ui == a.ui && win_b->viewport->ui == b.ui,
			"each viewport holds its own context");
		check(a.ui != b.ui && a.dock != b.dock, "distinct UI trees and Docks");
		check(host.size() == 2 && host.slots()[0].canvas != host.slots()[1].canvas,
			"each window has a private rvg canvas");
		check(win_a->viewport->io.mks != win_b->viewport->io.mks,
			"key state is per window");
		check(vgui_dock_get_count(a.dock) == 3, "dock A has 3 panels");
		check(vgui_dock_get_count(b.dock) == 2, "dock B has 2 panels");
		check(vgui_dock_get_at(a.dock, 0) == vgui_dock_get_at(b.dock, 0),
			"dock item ids are per-dock local (documented constraint)");
		const char* alias = vgui_dock_get_title(b.dock, a.first_panel);
		check(alias != nullptr && std::strcmp(alias, "B / LEFT") == 0,
			"local ids collide across docks (never pass them between windows)");

		FpsCounter fps;
		int submitted = 0;
		int frame = 0;
		bool running = true;
		bool lists_checked = false;
		while (running)
		{
			if (wg->get_event() < 0) running = false;
			const float delta = fps.tick();
			host.begin_frame(delta);
			host.draw_all();
			submitted = host.end_frame();

			++frame;
			if (probe && !lists_checked && frame >= 3)
			{
				lists_checked = true;
				ovg_draw_data_t la = get_draw_list(host.slots()[0].canvas);
				ovg_draw_data_t lb = get_draw_list(host.slots()[1].canvas);
				check(la.d != nullptr && lb.d != nullptr && la.d != lb.d,
					"draw lists live in separate buffers");
				check(la.count > 0 && lb.count > 0, "both windows recorded draw commands");
				check(la.count != lb.count, "windows recorded different content");
				printf("INFO draw commands: A=%zu B=%zu\n", la.count, lb.count);
			}
			if (probe && frame >= max_frames) running = false;
		}

		check(submitted == 2, "one frame submits both windows in one command buffer");
		check(lists_checked || !probe, "draw list probe ran");

		// Closing one window must not disturb the other.  destroy() clears
		// main_window / active_viewport_ / children so nothing dangles.
		wg->destroy(win_b->id);
		host.prune();
		check(host.size() == 1, "prune drops the closed window's slot");
		check(wg->main_window == win_a, "main_window stays valid after close");
		host.begin_frame(0.016f);
		host.draw_all();
		check(host.end_frame() == 1, "remaining window still renders");
		check(vgui_dock_get_count(a.dock) == 3, "dock A survived window B closing");

		// Manual-destroy pattern: viewport must not free the context again.
		for (const multiwindow_slot& s : host.slots())
		{
			vgui_destroy(s.ui);
			s.window->viewport->ui = nullptr;
		}
	} // host destroys its rvg canvases while cb is still alive

	wg->wait_gpu();
	delete_font_family(familys);
	free_ctx_cb(cb);
	delete wg;

	printf("SELFTEST %s (%d pass, %d fail)\n", g_fail ? "FAIL" : "PASS", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
