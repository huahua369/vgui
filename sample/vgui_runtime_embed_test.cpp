// sample/vgui_runtime_embed_test.cpp: R0 runtime embedding self-test.
//
// Drives the minimal engine-embedding loop of the core C API with no window,
// no GPU, no font and no SDL -- exactly what a 2D/3D game engine does to own
// its main loop and renderer (docs/runtime_roadmap.md R0):
//
//   * create a context, build a small tree, lay it out
//   * inject mouse input with vgui_input_* and observe widget callbacks
//   * double-click counting driven by an injected (non-OS) clock
//   * vgui_update(dt) + vgui_draw() produce consumable ovg_draw_data_t
//   * vgui_input_cancel() drops in-progress press state
//
// Build against vgui::vgui only; run directly, returns 0 when all checks pass.

#include <cstdio>
#include <cstring>
#include <vector>

#include "pch.h"		// glm etc. (required by ovg_c.h)
#include <ovg_c.h>		// rvg_t / ovg_ctx_cb / get_draw_list
#include <vgui/vgui.h>

static int g_pass = 0, g_fail = 0;

static void check(bool ok, const char* what)
{
	if (ok)
	{
		++g_pass;
		printf("ok  %s\n", what);
	}
	else
	{
		++g_fail;
		printf("FAIL %s\n", what);
	}
}

// ---- injected clock: replaces the OS monotonic clock -----------------------
struct fake_clock { double now = 0.0; };

static double fake_time_fn(void* user)
{
	return static_cast<fake_clock*>(user)->now;
}

// ---- event recording --------------------------------------------------------
struct click_log
{
	int clicks = 0;		// last click count delivered
	int click_events = 0;
	int double_events = 0;
};

static void on_button_event(vgui_widget*, const vgui_event* e, void* user)
{
	click_log* log = static_cast<click_log*>(user);
	if (e->type == VGUI_EVENT_CLICK)
	{
		++log->click_events;
		log->clicks = e->clicks;
	}
	else if (e->type == VGUI_EVENT_DOUBLE_CLICK)
	{
		++log->double_events;
		log->clicks = e->clicks;
	}
}

// Sends a full left press/release over the widget centre and runs one frame.
static void click_center(vgui_context* ui, ovg_ctx_cb* cb, rvg_t* vg,
	vgui_widget* w, fake_clock* clock)
{
	float wx = 0, wy = 0, ww = 0, wh = 0;
	vgui_widget_get_rect(w, &wx, &wy, &ww, &wh);
	vgui_input_mouse_move(ui, wx + ww * 0.5f, wy + wh * 0.5f);
	vgui_input_mouse_down(ui, 1, wx + ww * 0.5f, wy + wh * 0.5f, 0);
	vgui_input_mouse_up(ui, 1, wx + ww * 0.5f, wy + wh * 0.5f, 0);
	clock->now += 1.0f / 60.0f;
	vgui_update(ui, 1.0f / 60.0f);
	vgui_draw(ui, cb, vg);
}

int main()
{
	// ---- ovg recording context (no GPU; draw data is consumed as data) ----
	ovg_ctx_cb* cb = new_ctx_cb();
	check(cb != nullptr, "new_ctx_cb");
	rvg_t* vg = cb ? cb->new_rvg(cb->ac) : nullptr;
	check(vg != nullptr, "cb->new_rvg");

	// ---- core context: NULL font keeps the test OS/font-free ----
	vgui_context* ui = vgui_create(nullptr, 16.0f);
	check(ui != nullptr, "vgui_create(NULL font)");
	vgui_context_set_size(ui, 320, 240);

	// Fixed geometry keeps hit tests independent of font metrics.
	vgui_style style;
	vgui_style_default(&style);
	style.list.row_height = 24.0f;
	style.tree.row_height = 24.0f;
	style.menu.item_height = 26.0f;
	vgui_context_set_style(ui, &style);

	// Injected clock: double-click timing must follow the host, not the OS.
	fake_clock clock;
	vgui_context_set_time_fn(ui, fake_time_fn, &clock);

	// ---- build a tiny tree ----
	vgui_widget* root = vgui_root(ui);
	vgui_widget_set_layout(root, VGUI_LAYOUT_ABSOLUTE);

	vgui_widget* button = vgui_widget_add(root, VGUI_BUTTON, "Start");
	vgui_widget_set_pos(button, 16.0f, 16.0f);
	vgui_widget_set_size(button, 120.0f, 32.0f);
	click_log log;
	vgui_widget_on_event(button, on_button_event, &log);

	vgui_update(ui, 1.0f / 60.0f);	// one layout pass for meaningful rects

	float bx = -1, by = -1, bw = 0, bh = 0;
	if (button) vgui_widget_get_rect(button, &bx, &by, &bw, &bh);
	check(bw > 0 && bh > 0, "button laid out with explicit size");

	// ---- input injection: click-through and press state ----
	const int before_empty = log.click_events + log.double_events;
	vgui_input_mouse_move(ui, 300.0f, 220.0f);
	vgui_input_mouse_down(ui, 1, 300.0f, 220.0f, 0);
	vgui_input_mouse_up(ui, 1, 300.0f, 220.0f, 0);
	clock.now += 1.0f / 60.0f;
	vgui_update(ui, 1.0f / 60.0f);
	check(log.click_events + log.double_events == before_empty,
		"empty-area press does not reach the button");

	// Long advance: the click counter must start fresh for the button.
	clock.now += 2.0;
	const int consumed_btn = vgui_input_mouse_down(ui, 1,
		bx + bw * 0.5f, by + bh * 0.5f, 0);
	check(consumed_btn != 0, "button press is consumed");
	vgui_input_mouse_up(ui, 1, bx + bw * 0.5f, by + bh * 0.5f, 0);
	clock.now += 1.0f / 60.0f;
	vgui_update(ui, 1.0f / 60.0f);
	check(log.click_events == 1 && log.clicks == 1, "first click delivered");

	// ---- double-click window follows the injected clock ----
	// 0.2s later: still inside the 0.45s window -> double click.
	clock.now += 0.2;	vgui_input_mouse_move(ui, bx + bw * 0.5f, by + bh * 0.5f);
	vgui_input_mouse_down(ui, 1, bx + bw * 0.5f, by + bh * 0.5f, 0);
	vgui_input_mouse_up(ui, 1, bx + bw * 0.5f, by + bh * 0.5f, 0);
	clock.now += 1.0f / 60.0f;
	vgui_update(ui, 1.0f / 60.0f);
	check(log.double_events == 1 && log.clicks == 2, "double click at +0.2s");

	// Long pause: the click counter must reset.
	clock.now += 2.0;
	vgui_input_mouse_down(ui, 1, bx + bw * 0.5f, by + bh * 0.5f, 0);
	vgui_input_mouse_up(ui, 1, bx + bw * 0.5f, by + bh * 0.5f, 0);
	clock.now += 1.0f / 60.0f;
	vgui_update(ui, 1.0f / 60.0f);
	check(log.click_events == 2 && log.clicks == 1,
		"click counter resets after injected pause");

	// ---- checkbox toggle through injected input ----
	// Advance the clock first: this press must be a fresh single click, not
	// the second half of a double click (checkboxes ignore clicks >= 2).
	clock.now += 1.0;
	vgui_widget* check_w = vgui_widget_add(root, VGUI_CHECKBOX, "VSync");
	vgui_widget_set_pos(check_w, 16.0f, 64.0f);
	vgui_widget_set_size(check_w, 120.0f, 24.0f);
	vgui_update(ui, 1.0f / 60.0f);
	float cx = 0, cy = 0, cw = 0, ch = 0;
	vgui_widget_get_rect(check_w, &cx, &cy, &cw, &ch);
	vgui_input_mouse_move(ui, cx + cw * 0.5f, cy + ch * 0.5f);
	vgui_input_mouse_down(ui, 1, cx + cw * 0.5f, cy + ch * 0.5f, 0);
	vgui_input_mouse_up(ui, 1, cx + cw * 0.5f, cy + ch * 0.5f, 0);
	vgui_update(ui, 1.0f / 60.0f);
	check(vgui_widget_get_checked(check_w) != 0, "checkbox toggles via input");

	// ---- vgui_input_cancel drops in-progress press state ----
	vgui_input_mouse_move(ui, bx + bw * 0.5f, by + bh * 0.5f);
	vgui_input_mouse_down(ui, 1, bx + bw * 0.5f, by + bh * 0.5f, 0);
	const int clicks_before = log.click_events;
	vgui_input_cancel(ui);
	vgui_input_mouse_up(ui, 1, bx + bw * 0.5f, by + bh * 0.5f, 0);
	vgui_update(ui, 1.0f / 60.0f);
	check(log.click_events == clicks_before, "cancel drops in-progress click");

	// ---- per-frame draw: the host consumes ovg_draw_data_t ----
	vgui_draw(ui, cb, vg);
	ovg_draw_data_t dd = get_draw_list(vg);
	check(dd.count > 0, "draw list has render commands");
	check(dd.v_count > 0, "draw list has vertices");

	// ---- R0 font C API: path / dir / memory registration -----------------
	// Pure fontconfig + harfbuzz work, no window or GPU required.
	font_cache_cx* fcache = new_font_cache();
	check(fcache != nullptr, "new_font_cache");
	if (fcache)
	{
		const std::string ttf =
			std::string(VGUI_SOURCE_DIR) + "/sample/res/fonts/DejaVuSans.ttf";

		check(load_font_from_path(fcache, ttf.c_str(), "EmbedPathDejaVu") != 0,
			"load_font_from_path registers an alias immediately");
		font_familys_t* fam1 = new_font_family(fcache, "EmbedPathDejaVu", nullptr);
		check(fam1 && fam1->count > 0, "path-registered family resolves");
		delete_font_family(fam1);

		check(add_font_dir(fcache,
			(std::string(VGUI_SOURCE_DIR) + "/sample/res/fonts").c_str()) != 0,
			"add_font_dir");

		FILE* ttf_file = fopen(ttf.c_str(), "rb");
		check(ttf_file != nullptr, "open ttf for memory registration");
		if (ttf_file)
		{
			std::vector<unsigned char> buf;
			fseek(ttf_file, 0, SEEK_END);
			long size = ftell(ttf_file);
			fseek(ttf_file, 0, SEEK_SET);
			buf.resize((size_t)size);
			check(fread(buf.data(), 1, (size_t)size, ttf_file) == (size_t)size,
				"read ttf into memory");
			fclose(ttf_file);

			check(load_font_from_memory(fcache, buf.data(), size,
				"EmbedMemDejaVu") != 0, "load_font_from_memory");
			font_familys_t* fam2 = new_font_family(fcache, "EmbedMemDejaVu", nullptr);
			check(fam2 && fam2->count > 0, "memory-registered family resolves");
			delete_font_family(fam2);
		}
		free_font_cache(fcache);
	}

	// ---- cleanup ----
	vgui_destroy(ui);
	if (vg && cb) cb->destroy_rvg(vg);
	if (cb) free_ctx_cb(cb);

	printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
