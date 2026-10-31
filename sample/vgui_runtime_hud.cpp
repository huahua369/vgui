// sample/vgui_runtime_hud.cpp: R1 runtime HUD compositing demo.
//
// Uses vgui the way a 2D/3D game engine would at runtime -- the engine owns
// the frame, vgui draws the overlaying HUD.  A phase0 3D scene (rotating cube
// ring) renders first; the HUD on top is a regular retained vgui tree:
// vitals (progress), a minimap and crosshair (VGUI_CUSTOM), a hotbar with
// per-frame cooldown bars, all bound to simulated game state each frame.
//
// Two HUD compositing modes, toggled with TAB (see docs/runtime_roadmap.md R1):
//   A "load-op": the engine blits the scene into the swapchain inside vgpu's
//                command buffer, then the HUD pass runs with
//                vg_fbo_t::clear = false and blends on top.
//   B "offscreen": the HUD renders into its own FBO; the engine then blits
//                the scene and samples the HUD color texture as a full-screen
//                image in a composite pass (the "UI as engine texture" path).
//
// Run with --smoke to render both modes for a few frames and exit.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "pch.h"
#include "ovg_main.h"
#include <SDL3/SDL.h>
#include <vgui/backend/vgui_sdl3.h>
#include <vgui/vgui.h>

#include "phase0_renderer.h"

static const int kEntityCount = 12;

struct hud_state
{
	vgui_context* ui = nullptr;
	phase0_renderer* renderer = nullptr;

	// HUD widgets bound to simulated game state.
	vgui_widget* hp = nullptr;
	vgui_widget* shield = nullptr;
	vgui_widget* status = nullptr;
	vgui_widget* status_plate = nullptr;
	vgui_widget* minimap = nullptr;
	vgui_widget* crosshair = nullptr;
	vgui_widget* hotbar = nullptr;
	vgui_widget* cooldown[6] = {};

	// Simulation state (stands in for the game).
	float time = 0.0f;
	float hp_value = 1.0f;
	float shield_value = 0.5f;
	float skill_cd[6] = {};
	float ent_x[kEntityCount] = {};
	float ent_z[kEntityCount] = {};
	int frames = 0;

	// HUD compositing mode: 0 = load-op direct, 1 = offscreen texture.
	int mode = 0;
	bool quit = false;

	// Mode B offscreen target: the rendered HUD, borrowed as a texture.
	vg_fbo_t ui_fbo{};
	ovg_texture_t ui_tex{};
	bool ui_tex_ready = false;
};

// ---------------------------------------------------------------------------
// HUD custom widgets
// ---------------------------------------------------------------------------

// Minimap: world XZ [-9,9] mapped onto the widget, player at the centre.
static void draw_minimap(vgui_widget*, ovg_ctx_cb* cb, rvg_t* vg,
	float x, float y, float w, float h, float, void* user)
{
	hud_state* st = static_cast<hud_state*>(user);

	// Backdrop: translucent dark plate so the 3D scene stays visible.
	cb->set_source_rgba(vg, 0.05f, 0.07f, 0.10f, 0.72f);
	cb->rectangle(vg, x, y, w, h);
	cb->fill(vg);

	// Grid: one line every 3 world units.
	const float world = 9.0f;
	const float scale = w / (2.0f * world);
	cb->set_source_rgba(vg, 1.0f, 1.0f, 1.0f, 0.10f);
	cb->set_line_width(vg, 1.0f);
	for (int g = -6; g <= 6; g += 3)
	{
		if (g == 0) continue;
		const float off = g * scale;
		cb->move_to(vg, x + w * 0.5f + off, y);
		cb->line_to(vg, x + w * 0.5f + off, y + h);
		cb->move_to(vg, x, y + h * 0.5f + off);
		cb->line_to(vg, x + w, y + h * 0.5f + off);
	}
	cb->stroke(vg);

	// Enemies.
	cb->set_source_rgba(vg, 0.95f, 0.30f, 0.25f, 1.0f);
	for (int i = 0; i < kEntityCount; ++i)
		cb->circle(vg, x + w * 0.5f + st->ent_x[i] * scale,
			y + h * 0.5f + st->ent_z[i] * scale, 3.0f);
	cb->fill(vg);

	// Player: a facing triangle at the centre.
	cb->set_source_rgba(vg, 0.40f, 0.90f, 1.0f, 1.0f);
	const float cx = x + w * 0.5f, cy = y + h * 0.5f;
	cb->move_to(vg, cx, cy - 6.0f);
	cb->line_to(vg, cx - 4.5f, cy + 5.0f);
	cb->line_to(vg, cx + 4.5f, cy + 5.0f);
	cb->close_path(vg);
	cb->fill(vg);
}

// Crosshair: four ticks around a centre dot.
static void draw_crosshair(vgui_widget*, ovg_ctx_cb* cb, rvg_t* vg,
	float x, float y, float w, float h, float, void*)
{
	const float cx = x + w * 0.5f, cy = y + h * 0.5f;
	const float gap = 5.0f, len = 7.0f;
	cb->set_source_rgba(vg, 1.0f, 1.0f, 1.0f, 0.85f);
	cb->set_line_width(vg, 1.5f);
	cb->move_to(vg, cx - gap - len, cy); cb->line_to(vg, cx - gap, cy);
	cb->move_to(vg, cx + gap, cy);       cb->line_to(vg, cx + gap + len, cy);
	cb->move_to(vg, cx, cy - gap - len); cb->line_to(vg, cx, cy - gap);
	cb->move_to(vg, cx, cy + gap);       cb->line_to(vg, cx, cy + gap + len);
	cb->stroke(vg);
	cb->circle(vg, cx, cy, 1.4f);
	cb->fill(vg);
}

// ---------------------------------------------------------------------------
// Input: TAB toggles the compositing mode, 1-6 use hotbar skills
// ---------------------------------------------------------------------------

static int on_key(vgui_context*, int key, int down, int, void* user)
{
	hud_state* st = static_cast<hud_state*>(user);
	if (!down) return 0;
	if (key == VGUI_KEY_TAB)
	{
		st->mode = st->mode ? 0 : 1;
		return 1;
	}
	if (key >= '1' && key <= '6')
	{
		st->skill_cd[key - '1'] = 3.0f;
		return 1;
	}
	return 0;
}

// ---------------------------------------------------------------------------
// HUD tree
// ---------------------------------------------------------------------------

static void layout_anchors(hud_state* st, int width, int height);

static void build_hud(hud_state* st, int width, int height)
{
	st->ui = vgui_create(nullptr, 16.0f);
	vgui_context_set_size(st->ui, width, height);
	// The root never paints and never takes mouse input away from the game;
	// widgets are placed absolutely like a real HUD layer.
	vgui_widget* root = vgui_root(st->ui);
	vgui_widget_set_layout(root, VGUI_LAYOUT_ABSOLUTE);
	vgui_widget_set_background(root, 0);

	// Top-left vitals plate.
	vgui_widget* vitals = vgui_widget_add(root, VGUI_PANEL, nullptr);
	vgui_widget_set_pos(vitals, 16.0f, 16.0f);
	vgui_widget_set_size(vitals, 280.0f, 76.0f);
	vgui_widget_set_layout(vitals, VGUI_LAYOUT_COLUMN);
	vgui_widget_set_padding(vitals, 10, 8, 10, 8);
	vgui_widget_set_gap(vitals, 6.0f);
	vgui_widget_set_background(vitals, 0x98101418);

	vgui_widget* hp_row = vgui_widget_add(vitals, VGUI_PANEL, nullptr);
	vgui_widget_set_layout(hp_row, VGUI_LAYOUT_ROW);
	vgui_widget_set_gap(hp_row, 8.0f);
	vgui_widget_set_background(hp_row, 0);
	vgui_widget* hp_label = vgui_widget_add(hp_row, VGUI_LABEL, "HP");
	vgui_widget_set_size(hp_label, 48.0f, 0.0f);
	st->hp = vgui_widget_add(hp_row, VGUI_PROGRESS, nullptr);
	vgui_widget_set_grow(st->hp, 1.0f);
	vgui_widget_set_size(st->hp, 0.0f, 14.0f);
	vgui_widget_set_value(st->hp, 1.0f);

	vgui_widget* sh_row = vgui_widget_add(vitals, VGUI_PANEL, nullptr);
	vgui_widget_set_layout(sh_row, VGUI_LAYOUT_ROW);
	vgui_widget_set_gap(sh_row, 8.0f);
	vgui_widget_set_background(sh_row, 0);
	vgui_widget* sh_label = vgui_widget_add(sh_row, VGUI_LABEL, "SHLD");
	vgui_widget_set_size(sh_label, 48.0f, 0.0f);
	st->shield = vgui_widget_add(sh_row, VGUI_PROGRESS, nullptr);
	vgui_widget_set_grow(st->shield, 1.0f);
	vgui_widget_set_size(st->shield, 0.0f, 14.0f);
	vgui_widget_set_value(st->shield, 0.5f);

	// Top-right status plate (updated once every few frames).
	st->status_plate = vgui_widget_add(root, VGUI_PANEL, nullptr);
	vgui_widget_set_padding(st->status_plate, 10, 8, 10, 8);
	vgui_widget_set_background(st->status_plate, 0x98101418);
	st->status = vgui_widget_add(st->status_plate, VGUI_LABEL,
		"TAB: compositing mode");
	vgui_widget_set_id(st->status, "hud.status");

	// Bottom-left minimap.
	st->minimap = vgui_widget_add(root, VGUI_CUSTOM, nullptr);
	vgui_widget_set_size(st->minimap, 180.0f, 180.0f);
	vgui_custom_set_draw(st->minimap, draw_minimap, st);

	// Bottom-centre hotbar: digit button + cooldown bar per slot.
	st->hotbar = vgui_widget_add(root, VGUI_PANEL, nullptr);
	vgui_widget_set_size(st->hotbar, 416.0f, 68.0f);
	vgui_widget_set_layout(st->hotbar, VGUI_LAYOUT_ROW);
	vgui_widget_set_gap(st->hotbar, 8.0f);
	vgui_widget_set_padding(st->hotbar, 8, 8, 8, 8);
	vgui_widget_set_background(st->hotbar, 0x80101418);
	for (int i = 0; i < 6; ++i)
	{
		vgui_widget* slot = vgui_widget_add(st->hotbar, VGUI_PANEL, nullptr);
		vgui_widget_set_layout(slot, VGUI_LAYOUT_COLUMN);
		vgui_widget_set_gap(slot, 4.0f);
		vgui_widget_set_background(slot, 0);
		char label[8] = {};
		std::snprintf(label, sizeof(label), "%d", i + 1);
		vgui_widget* key = vgui_widget_add(slot, VGUI_BUTTON, label);
		vgui_widget_set_size(key, 56.0f, 36.0f);
		st->cooldown[i] = vgui_widget_add(slot, VGUI_PROGRESS, nullptr);
		vgui_widget_set_size(st->cooldown[i], 56.0f, 6.0f);
		vgui_widget_set_value(st->cooldown[i], 0.0f);
	}

	// Centre crosshair.
	st->crosshair = vgui_widget_add(root, VGUI_CUSTOM, nullptr);
	vgui_widget_set_size(st->crosshair, 32.0f, 32.0f);
	vgui_custom_set_draw(st->crosshair, draw_crosshair, st);

	vgui_context_add_key_handler(st->ui, on_key, st);
	layout_anchors(st, width, height);
	vgui_update(st->ui, 0.0f);
}

// Edge/centre anchors for the absolutely-positioned HUD plates; called on
// build and whenever the window resizes.
static void layout_anchors(hud_state* st, int width, int height)
{
	vgui_widget_set_pos(st->status_plate, (float)width - 296.0f, 16.0f);
	vgui_widget_set_size(st->status_plate, 280.0f, 56.0f);
	vgui_widget_set_pos(st->minimap, 16.0f, (float)height - 196.0f);
	vgui_widget_set_pos(st->hotbar, (float)width * 0.5f - 208.0f,
		(float)height - 84.0f);
	vgui_widget_set_pos(st->crosshair, (float)width * 0.5f - 16.0f,
		(float)height * 0.5f - 16.0f);
}

// ---------------------------------------------------------------------------
// Game simulation (stands in for the engine)
// ---------------------------------------------------------------------------

static void simulate(hud_state* st, float delta)
{
	st->time += delta;
	st->hp_value = 0.62f + 0.36f * std::sin(st->time * 0.6f);
	st->shield_value = 0.45f + 0.45f * std::sin(st->time * 0.9f + 1.3f);
	vgui_widget_set_value(st->hp, st->hp_value);
	vgui_widget_set_value(st->shield, st->shield_value);
	for (int i = 0; i < 6; ++i)
	{
		if (st->skill_cd[i] > 0.0f)
			st->skill_cd[i] = std::max(0.0f, st->skill_cd[i] - delta);
		vgui_widget_set_value(st->cooldown[i], st->skill_cd[i] / 3.0f);
	}
	for (int i = 0; i < kEntityCount; ++i)
	{
		const float angle = st->time * 0.4f
			+ (float)i * (6.2831853f / (float)kEntityCount);
		const float radius = 4.5f + 2.0f * std::sin(st->time * 0.23f + (float)i);
		st->ent_x[i] = std::cos(angle) * radius;
		st->ent_z[i] = std::sin(angle) * radius;
	}
}

static void feed_scene(hud_state* st)
{
	phase0_instance insts[kEntityCount] = {};
	for (int i = 0; i < kEntityCount; ++i)
	{
		insts[i].position[0] = st->ent_x[i];
		insts[i].position[1] = 0.5f;
		insts[i].position[2] = st->ent_z[i];
		const float yaw = st->time * 0.8f + (float)i;
		insts[i].rotation[1] = std::sin(yaw * 0.5f);
		insts[i].rotation[3] = std::cos(yaw * 0.5f);
		insts[i].scale[0] = insts[i].scale[1] = insts[i].scale[2] = 0.8f;
	}
	phase0_renderer_set_instances(st->renderer, insts, kEntityCount);
}

// Blits the completed 3D scene into the swapchain inside vgpu's pending
// command buffer, so the HUD pass that follows stays ordered after it.
static void blit_scene_to_swapchain(vgpu_ctx_t* vgctx, vg_fbo_t* fbo,
	hud_state* st)
{
	SDL_GPUBlitInfo info = {};
	info.source.texture = phase0_renderer_color_texture(st->renderer);
	info.source.w = static_cast<Uint32>(phase0_renderer_width(st->renderer));
	info.source.h = static_cast<Uint32>(phase0_renderer_height(st->renderer));
	info.destination.texture = fbo->swapchain;
	info.destination.w = fbo->width;
	info.destination.h = fbo->height;
	info.load_op = SDL_GPU_LOADOP_DONT_CARE;
	info.filter = SDL_GPU_FILTER_LINEAR;
	SDL_BlitGPUTexture(vgpu_current_command_buffer(vgctx), &info);
}

// Mode B: keep the offscreen HUD target and its borrowed texture in sync with
// the window size.
static void sync_ui_fbo(vgpu_ctx_t* vgctx, hud_state* st, int w, int h)
{
	if (!st->ui_fbo.ctx)
		st->ui_fbo = vgpu_create_fbo(vgctx, w, h, nullptr);
	else if (st->ui_fbo.width != (uint32_t)w || st->ui_fbo.height != (uint32_t)h)
		vgpu_reset_fbo(&st->ui_fbo, w, h);
	else
		return;

	// The FBO texture was (re)created: (re)borrow it as an ovg texture.
	if (st->ui_tex_ready)
	{
		SDL_GPUTexture* tex = vgpu_fbo_color_texture(&st->ui_fbo);
		vgpu_texture_update_native(vgctx, &st->ui_tex, tex, w, h,
			vgpu_color_format(vgctx));
		return;
	}
	std::memset(&st->ui_tex, 0, sizeof(st->ui_tex));
	vgpu_texture_init_native(vgctx, &st->ui_tex,
		vgpu_fbo_color_texture(&st->ui_fbo), w, h, vgpu_color_format(vgctx));
	st->ui_tex_ready = true;
}

static void draw_status(hud_state* st, FpsCounter* fps, float upms, float drawms)
{
	char text[160] = {};
	std::snprintf(text, sizeof(text),
		"%s  \xC2\xB7  %d fps  \xC2\xB7  ui %.2f/%.2f ms  \xC2\xB7  TAB: mode",
		st->mode == 0 ? "mode A: load-op" : "mode B: offscreen",
		fps->get_fps(), upms * 0.001, drawms * 0.001);
	vgui_widget_set_text(st->status, text);
}

int main(int argc, char** argv)
{
	bool smoke = false;
	for (int i = 1; i < argc; ++i)
		if (std::strcmp(argv[i], "--smoke") == 0)
			smoke = true;

	auto* wg = new app_mgr();
	font_familys_t* familys = new_font_family(wg->font_ctx,
		"Consolas,\xE5\xBE\xAE\xE8\xBD\xAF\xE9\x9B\x85\xE9\xBB\x91", 0);
	auto* cb = new_ctx_cb();
	rvg_t* vg = cb->new_rvg(cb->ac);
	rvg_t* vg_composite = cb->new_rvg(cb->ac);	// mode B composite recording

	if (!wg->init_gpu(true))
	{
		std::fprintf(stderr, "hud: init_gpu failed\n");
		return 2;
	}

	const int win_w = 1280;
	const int win_h = 800;
	os_window* form = wg->create("vgui runtime HUD (TAB toggles compositing)",
		win_w, win_h, 0, true);
	if (!form || !form->viewport)
	{
		std::fprintf(stderr, "hud: window creation failed\n");
		return 2;
	}

	hud_state st;
	build_hud(&st, win_w, win_h);
	form->viewport->ui = st.ui;

	st.renderer = phase0_renderer_create(wg->device);
	if (!st.renderer)
	{
		std::fprintf(stderr, "hud: 3D renderer creation failed\n");
		wg->wait_gpu();
		vgui_destroy(st.ui);
		form->viewport->ui = nullptr;
		delete_font_family(familys);
		cb->destroy_rvg(vg);
		cb->destroy_rvg(vg_composite);
		free_ctx_cb(cb);
		delete wg;
		return 2;
	}
	// Player camera: slightly above the arena looking at the centre.
	phase0_camera cam;
	cam.eye[0] = 0.0f; cam.eye[1] = 6.5f; cam.eye[2] = 9.0f;
	phase0_renderer_set_camera(st.renderer, &cam);

	vg_fbo_t* fbo = &form->fbo;
	FpsCounter fps;
	runtime_cx rtc;
	int last_w = win_w;
	int last_h = win_h;
	int smoke_frames = 0;
	float upms = 0.0f, drawms = 0.0f;

	while (!st.quit)
	{
		const float delta = fps.tick();
		if (wg->get_event() < 0)
			break;

		if (!vgpu_acquire_swapchain(wg->vgctx, fbo, 1, true))
			continue;

		const int cw = (int)fbo->display_size.x;
		const int ch = (int)fbo->display_size.y;
		if (cw != last_w || ch != last_h)
		{
			last_w = cw;
			last_h = ch;
			vgui_context_set_size(st.ui, cw, ch);
			layout_anchors(&st, cw, ch);
		}
		if (fbo->upsize)
		{
			fbo->upsize = false;
			vg->width = fbo->display_size.x;
			vg->height = fbo->display_size.y;
			vg_composite->width = fbo->display_size.x;
			vg_composite->height = fbo->display_size.y;
		}
		if (st.mode == 1)
			sync_ui_fbo(wg->vgctx, &st, cw, ch);

		simulate(&st, delta);
		rtc.begin();
		vgui_update(st.ui, delta);
		upms = (float)rtc.end();

		// 3D first: it owns its command buffer and submits immediately.
		feed_scene(&st);
		phase0_renderer_resize(st.renderer, cw, ch);
		phase0_renderer_render(st.renderer, st.time);

		rtc.begin();
		cb->clear(vg);
		cb->reset_clip(vg, 1);
		vgui_draw(st.ui, cb, vg);
		drawms = (float)rtc.end();

		if (st.mode == 0)
		{
			// Mode A: scene blit, then HUD pass with load-op LOAD.
			fbo->clear = false;
			blit_scene_to_swapchain(wg->vgctx, fbo, &st);
			draw_datas_cx drawable;
			drawable.set(fbo);
			drawable.add(vg);
			drawable.render(wg->vgctx);
		}
		else
		{
			// Mode B: HUD into its own FBO, then scene blit, then an engine
			// composite pass that samples the HUD texture over the scene.
			fbo->clear = false;
			draw_datas_cx hud_drawable;
			hud_drawable.set(&st.ui_fbo);
			hud_drawable.add(vg);
			hud_drawable.render(wg->vgctx);

			blit_scene_to_swapchain(wg->vgctx, fbo, &st);

			ovg_texture_r composite = {};
			composite.tex = &st.ui_tex;
			composite.rc = { -1, 0, 0, 0 };		// whole texture
			composite.sliced = { 0, 0, 0, 0 };	// no 9-slice
			composite.dst = { 0, 0, cw, ch };
			composite.color = 0xFFFFFFFFu;
			cb->clear(vg_composite);
			cb->reset_clip(vg_composite, 1);
			cb->add_texture(vg_composite, &composite);

			draw_datas_cx comp_drawable;
			comp_drawable.set(fbo);
			comp_drawable.add(vg_composite);
			comp_drawable.render(wg->vgctx);
		}

		++st.frames;
		if (st.frames % 20 == 0)
			draw_status(&st, &fps, upms, drawms);

		if (smoke)
		{
			// Exercise both modes before exiting.
			if (st.frames == 15) st.mode = 1;
			if (++smoke_frames >= 30)
				break;
		}
	}

	const bool ok = st.frames > 0;
	wg->wait_gpu();
	if (st.ui_tex_ready)
		vgpu_texture_fini_native(wg->vgctx, &st.ui_tex);
	if (st.ui_fbo.ctx)
		vgpu_destroy_fbo(&st.ui_fbo);
	phase0_renderer_destroy(st.renderer);
	vgui_destroy(st.ui);
	form->viewport->ui = nullptr;
	delete_font_family(familys);
	cb->destroy_rvg(vg);
	cb->destroy_rvg(vg_composite);
	free_ctx_cb(cb);
	delete wg;

	if (smoke)
	{
		std::printf("hud smoke: %s (%d frames, mode %d)\n",
			ok ? "ok" : "failed", st.frames, st.mode);
		return ok ? 0 : 1;
	}
	return 0;
}
