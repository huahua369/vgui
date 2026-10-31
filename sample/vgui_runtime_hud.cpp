// sample/vgui_runtime_hud.cpp: R1 runtime HUD compositing demo.
//
// Uses vgui the way a 2D/3D game engine would at runtime -- the engine owns
// the frame, vgui draws the overlaying HUD.  A phase0 3D scene (rotating cube
// ring) renders first; the HUD on top is a regular retained vgui tree:
// vitals (progress), a minimap and crosshair (VGUI_CUSTOM), a hotbar with
// per-frame cooldown bars, all bound to simulated game state each frame.
//
// Two HUD compositing modes, toggled with M (see docs/runtime_roadmap.md R1):
//   A "load-op": the engine blits the scene into the swapchain inside vgpu's
//                command buffer, then the HUD pass runs with
//                vg_fbo_t::clear = false and blends on top.
//   B "offscreen": the HUD renders into its own FBO in the same submission;
//                the engine then samples the HUD color texture as a
//                full-screen image in a composite pass (the "UI as engine
//                texture" path).
//
// All vgpu passes of a frame go through ONE vgpu_render_frame call so they
// share a single submission (SDL3 does not serialize submissions; separate
// ones would race on the shared vertex buffers).
//
// Run with --smoke to render both modes for a few frames and exit.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "pch.h"
#include "ovg_main.h"
#include <SDL3/SDL.h>
#include <vgui/backend/vgui_sdl3.h>
#include <vgui/vgui.h>
#include <ovg_fonts.h>

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

	// World-anchored nameplates: name + health bar + mana bar per entity,
	// projected from world space onto the screen every frame.
	vgui_widget* plates[kEntityCount] = {};
	vgui_widget* plate_name[kEntityCount] = {};
	vgui_widget* plate_hp[kEntityCount] = {};
	vgui_widget* plate_mp[kEntityCount] = {};
	float ent_hp[kEntityCount] = {};
	float ent_mp[kEntityCount] = {};
	int selected = -1;
	float cam_eye[3] = { 0.0f, 6.5f, 9.0f };
	float cam_target[3] = { 0.0f, 0.0f, 0.0f };
	float cam_fov = 50.0f;

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

	// Font family for text measurement; the status plate auto-fits its text
	// and keeps its right edge anchored.
	font_familys_t* font = nullptr;
	float status_w = 280.0f;
	int last_w = 0;
	int last_h = 0;

	// Mode B offscreen target: the rendered HUD, borrowed as a texture.
	vg_fbo_t ui_fbo{};
	ovg_texture_t ui_tex{};
	bool ui_tex_ready = false;

	// R5-1 world panel: its own tiny UI context rendered into an offscreen
	// FBO; phase0 samples that texture on a world-space billboard.
	vgui_context* panel_ui = nullptr;
	rvg_t* vg_panel = nullptr;
	vg_fbo_t panel_fbo{};
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
// Input: M toggles the compositing mode, 1-6 use hotbar skills
// (TAB itself is consumed by the core as the focus-traversal key, so app key
// handlers never see it.)
// ---------------------------------------------------------------------------

static const char kSkillKeys[6] = { 'Q', 'W', 'E', 'R', 'T', 'Y' };
static const char* kSkillNames[6] = { "Q", "W", "E", "R", "T", "Y" };

static int pick_unit(const hud_state* st, float mx, float my);
static void select_unit(hud_state* st, int index);

// R5-2: clicks that miss every HUD widget land on the root panel; unproject
// them into the scene and select the entity under the cursor.  Right click
// clears the selection.  Nameplates are input-disabled so clicks over them
// also end up here.
static void on_root_event(vgui_widget*, const vgui_event* e, void* user)
{
	hud_state* st = static_cast<hud_state*>(user);
	if (e->type != VGUI_EVENT_CLICK)
		return;
	if (e->button == 3)
	{
		select_unit(st, -1);
		return;
	}
	if (e->button == 1)
	{
		const int hit = pick_unit(st, e->x, e->y);
		if (hit >= 0)
			select_unit(st, hit);
	}
}

static int on_key(vgui_context*, int key, int down, int, void* user)
{
	hud_state* st = static_cast<hud_state*>(user);
	if (!down) return 0;
	if (key == 'M')
	{
		st->mode = st->mode ? 0 : 1;
		return 1;
	}
	// vgui_input_key uppercases a-z, so the handler sees 'Q'..'Y'.
	for (int i = 0; i < 6; ++i)
	{
		if (key == (int)(unsigned char)kSkillKeys[i])
		{
			st->skill_cd[i] = 3.0f;
			return 1;
		}
	}
	return 0;
}

// ---------------------------------------------------------------------------
// HUD tree
// ---------------------------------------------------------------------------

static void layout_anchors(hud_state* st, int width, int height);

static void build_hud(hud_state* st, font_familys_t* familys, int width, int height)
{
	st->font = familys;
	st->ui = vgui_create(familys, 16.0f);
	vgui_context_set_size(st->ui, width, height);
	// The root never paints and never takes mouse input away from the game;
	// widgets are placed absolutely like a real HUD layer.
	vgui_widget* root = vgui_root(st->ui);
	vgui_widget_set_layout(root, VGUI_LAYOUT_ABSOLUTE);
	vgui_widget_set_background(root, 0);


	// Top-left vitals plate.
	vgui_widget* vitals = vgui_widget_add(root, VGUI_PANEL, nullptr);
	vgui_widget_set_pos(vitals, 16.0f, 16.0f);
	vgui_widget_set_size(vitals, 266, 90.0f);
	vgui_widget_set_layout(vitals, VGUI_LAYOUT_COLUMN);
	vgui_widget_set_padding(vitals, 8, 8, 8, 8);
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
		"M: compositing mode");
	vgui_widget_set_id(st->status, "hud.status");

	// Bottom-left minimap.
	st->minimap = vgui_widget_add(root, VGUI_CUSTOM, nullptr);
	vgui_widget_set_size(st->minimap, 180.0f, 180.0f);
	vgui_custom_set_draw(st->minimap, draw_minimap, st);

	// Bottom-centre hotbar: digit button + cooldown bar per slot.
	st->hotbar = vgui_widget_add(root, VGUI_PANEL, nullptr);
	vgui_widget_set_size(st->hotbar, 488.0f, 80.0f);
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
		vgui_widget* key = vgui_widget_add(slot, VGUI_BUTTON, kSkillNames[i]);
		vgui_widget_set_size(key, 56.0f, 36.0f);
		st->cooldown[i] = vgui_widget_add(slot, VGUI_PROGRESS, nullptr);
		// 16px tall: the built-in percentage label fits inside the bar
		// instead of overflowing below the plate and getting clipped.
		vgui_widget_set_size(st->cooldown[i], 56.0f, 16.0f);
		vgui_widget_set_value(st->cooldown[i], 0.0f);
	}

	// Centre crosshair.
	st->crosshair = vgui_widget_add(root, VGUI_CUSTOM, nullptr);
	vgui_widget_set_size(st->crosshair, 32.0f, 32.0f);
	vgui_custom_set_draw(st->crosshair, draw_crosshair, st);

	// World-anchored nameplates: name + health bar + mana bar, one group per
	// entity, repositioned every frame from the projected world position.
	char plate_name[24] = {};
	for (int i = 0; i < kEntityCount; ++i)
	{
		std::snprintf(plate_name, sizeof(plate_name), "Unit %02d", i + 1);
		vgui_widget* plate = vgui_widget_add(root, VGUI_PANEL, nullptr);
		vgui_widget_set_size(plate, 76.0f, 26.0f);
		vgui_widget_set_layout(plate, VGUI_LAYOUT_COLUMN);
		vgui_widget_set_gap(plate, 1.0f);
		vgui_widget_set_padding(plate, 1, 1, 1, 1);
		vgui_widget_set_background(plate, 0x70101418);
		// Indicators, not interactive UI: clicks over a nameplate fall
		// through to the root's world-picking handler (R5-2).
		vgui_widget_set_input_enabled(plate, 0);

		vgui_widget* name = vgui_widget_add(plate, VGUI_LABEL, plate_name);
		st->plate_name[i] = name;
		vgui_widget_set_font_size(name, 10.0f);
		vgui_widget_set_text_align(name, 0.5f, 0.5f);

		// Health (red) / mana (blue) bars; the 5px bars carry no percentage
		// label (a single space replaces the auto-generated "NN%").
		st->plate_hp[i] = vgui_widget_add(plate, VGUI_PROGRESS, nullptr);
		vgui_widget_set_size(st->plate_hp[i], 0.0f, 5.0f);
		vgui_widget_set_grow(st->plate_hp[i], 1.0f);
		vgui_widget_set_accent(st->plate_hp[i], 0xFFE04040);
		vgui_widget_set_track(st->plate_hp[i], 0xFF402024);
		vgui_widget_set_text(st->plate_hp[i], " ");

		st->plate_mp[i] = vgui_widget_add(plate, VGUI_PROGRESS, nullptr);
		vgui_widget_set_size(st->plate_mp[i], 0.0f, 5.0f);
		vgui_widget_set_grow(st->plate_mp[i], 1.0f);
		vgui_widget_set_accent(st->plate_mp[i], 0xFF4070E0);
		vgui_widget_set_track(st->plate_mp[i], 0xFF202840);
		vgui_widget_set_text(st->plate_mp[i], " ");

		st->plates[i] = plate;
	}

	// R5-2 world picking: clicks that miss every widget land on the root
	// panel; the handler unprojects them into the scene and selects the
	// entity under the cursor.  Right click clears the selection.
	vgui_widget_on_event(root, on_root_event, st);

	vgui_context_add_key_handler(st->ui, on_key, st);
	layout_anchors(st, width, height);
	vgui_update(st->ui, 0.0f);
}

// Builds the same view-projection the phase0 camera renders with (RH_ZO
// projection with a Y flip, see phase0_renderer.cpp camera_matrices).
static glm::mat4 build_view_projection(const hud_state* st, int cw, int ch)
{
	glm::mat4 proj = glm::perspectiveRH_ZO(glm::radians(st->cam_fov),
		ch > 0 ? (float)cw / (float)ch : 1.0f, 0.1f, 100.0f)
		* glm::scale(glm::mat4(1.0f), glm::vec3(1.0f, -1.0f, 1.0f));
	glm::mat4 view = glm::lookAt(
		glm::vec3(st->cam_eye[0], st->cam_eye[1], st->cam_eye[2]),
		glm::vec3(st->cam_target[0], st->cam_target[1], st->cam_target[2]),
		glm::vec3(0.0f, 1.0f, 0.0f));
	return proj * view;
}

// Projects an entity's world anchor (above the cube) into screen coordinates;
// returns false when the anchor is behind the camera or far offscreen.
static bool project_entity(const hud_state* st, int index, int cw, int ch,
	float* out_x, float* out_y)
{
	glm::vec4 clip = build_view_projection(st, cw, ch)
		* glm::vec4(st->ent_x[index], 1.35f, st->ent_z[index], 1.0f);
	if (clip.w <= 0.05f)
		return false;
	const float ndc_x = clip.x / clip.w;
	const float ndc_y = clip.y / clip.w;
	if (ndc_x < -1.3f || ndc_x > 1.3f || ndc_y < -1.3f || ndc_y > 1.3f)
		return false;
	*out_x = (ndc_x * 0.5f + 0.5f) * (float)cw;
	*out_y = (ndc_y * 0.5f + 0.5f) * (float)ch;
	return true;
}

static void update_nameplates(hud_state* st, int cw, int ch)
{
	for (int i = 0; i < kEntityCount; ++i)
	{
		float sx = 0.0f, sy = 0.0f;
		const bool visible = project_entity(st, i, cw, ch, &sx, &sy);
		vgui_widget_set_visible(st->plates[i], visible ? 1 : 0);
		if (visible)
			vgui_widget_set_pos(st->plates[i], sx - 38.0f, sy - 30.0f);
		vgui_widget_set_value(st->plate_hp[i], st->ent_hp[i]);
		vgui_widget_set_value(st->plate_mp[i], st->ent_mp[i]);
	}
}

// R5-2 world picking: unproject the screen point into a camera ray and
// hit-test it against the entities' bounding spheres; returns the nearest
// hit or -1.
static int pick_unit(const hud_state* st, float mx, float my)
{
	const glm::mat4 inv = glm::inverse(build_view_projection(st,
		st->last_w, st->last_h));
	const float ndc_x = 2.0f * mx / (float)st->last_w - 1.0f;
	const float ndc_y = 2.0f * my / (float)st->last_h - 1.0f;
	glm::vec4 p0 = inv * glm::vec4(ndc_x, ndc_y, -1.0f, 1.0f);
	glm::vec4 p1 = inv * glm::vec4(ndc_x, ndc_y, 1.0f, 1.0f);
	if (p0.w == 0.0f || p1.w == 0.0f)
		return -1;
	const glm::vec3 origin(p0 / p0.w);
	const glm::vec3 far_point(p1 / p1.w);
	glm::vec3 dir = far_point - origin;
	const float dir_len = glm::length(dir);
	if (dir_len < 1e-6f)
		return -1;
	dir /= dir_len;

	int best = -1;
	float best_t = dir_len;
	for (int i = 0; i < kEntityCount; ++i)
	{
		// Cube half-diagonal is ~0.7 at scale 0.8; 0.9 keeps clicks forgiving.
		float t = 0.0f;
		if (glm::intersectRaySphere(origin, dir,
			glm::vec3(st->ent_x[i], 0.5f, st->ent_z[i]), 0.9f, t)
			&& t < best_t)
		{
			best_t = t;
			best = i;
		}
	}
	return best;
}

static void select_unit(hud_state* st, int index)
{
	if (st->selected >= 0 && st->selected < kEntityCount)
	{
		vgui_widget_set_border(st->plates[st->selected], 0, 0.0f);
		vgui_widget_set_text_color(st->plate_name[st->selected], 0);
	}
	st->selected = index;
	if (index >= 0)
	{
		vgui_widget_set_border(st->plates[index], 0xFF40A0FF, 1.5f);
		vgui_widget_set_text_color(st->plate_name[index], 0xFFFFFFFF);
	}
}

// Edge/centre anchors for the absolutely-positioned HUD plates; called on
// build and whenever the window resizes.
static void layout_anchors(hud_state* st, int width, int height)
{
	st->last_w = width;
	st->last_h = height;
	vgui_widget_set_pos(st->status_plate, (float)width - st->status_w - 16.0f, 16.0f);
	vgui_widget_set_size(st->status_plate, st->status_w, 56.0f);
	vgui_widget_set_pos(st->minimap, 16.0f, (float)height - 196.0f);
	vgui_widget_set_pos(st->hotbar, (float)width * 0.5f - 244.0f,
		(float)height - 96.0f);
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
		st->ent_hp[i] = 0.5f + 0.45f * std::sin(st->time * 0.7f + (float)i * 0.9f);
		st->ent_mp[i] = 0.5f + 0.45f * std::sin(st->time * 0.45f + (float)i * 1.7f);
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
	{
		st->ui_fbo = vgpu_create_fbo(vgctx, w, h, nullptr);
		// Rewritten every frame and sampled later in the same command buffer;
		// cycling avoids a write/write race against the still pending
		// previous frame.
		st->ui_fbo.cycle_target = true;
	}
	else if (st->ui_fbo.width != (uint32_t)w || st->ui_fbo.height != (uint32_t)h)
	{
		vgpu_reset_fbo(&st->ui_fbo, w, h);
		st->ui_fbo.cycle_target = true;
	}
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
		"%s  \xC2\xB7  %d fps  \xC2\xB7  ui %.2f/%.2f ms  \xC2\xB7  M: mode",
		st->mode == 0 ? "mode A: load-op" : "mode B: offscreen",
		fps->get_fps(), upms * 0.001, drawms * 0.001);
	if (st->selected >= 0)
	{
		char sel[32] = {};
		std::snprintf(sel, sizeof(sel), "  \xC2\xB7  sel Unit %02d", st->selected + 1);
		std::strncat(text, sel, sizeof(text) - std::strlen(text) - 1);
	}
	vgui_widget_set_text(st->status, text);

	// Fit the plate to the text so the status never overflows the window
	// edge: measure with the same family and effective (scaled) font size
	// the UI draws with.
	float tw = vg_measure_text_width(st->font,
		(int)(vgui_context_get_font_size(st->ui) + 0.5f),
		text, (int)std::strlen(text));
	float want = tw + 44.0f;	// plate padding (2x10) + anchor margin + rounding
	if (want < 200.0f) want = 200.0f;
	if (std::fabs(want - st->status_w) > 2.0f)
	{
		st->status_w = want;
		layout_anchors(st, st->last_w, st->last_h);
	}
}

int main(int argc, char** argv)
{
	bool smoke = false;
	for (int i = 1; i < argc; ++i)
		if (std::strcmp(argv[i], "--smoke") == 0)
			smoke = true;

	auto* wg = new app_mgr();
	// Runtime apps bundle their fonts (sample/res/fonts, see
	// LICENSE-DejaVu.txt) instead of relying on the system fontconfig
	// database: register the file from memory via the C API, then fall back
	// to the system lookup if the bundled file is unavailable.
	font_familys_t* familys = nullptr;
	{
		const char* ttf = VGUI_SOURCE_DIR "/sample/res/fonts/DejaVuSans.ttf";
		FILE* f = fopen(ttf, "rb");
		if (f)
		{
			fseek(f, 0, SEEK_END);
			long size = ftell(f);
			fseek(f, 0, SEEK_SET);
			std::vector<unsigned char> buf((size_t)size);
			if (fread(buf.data(), 1, (size_t)size, f) == (size_t)size)
				load_font_from_memory(wg->font_ctx, buf.data(), size, "HudBundled");
			fclose(f);
		}
		familys = new_font_family(wg->font_ctx, "HudBundled", nullptr);
		if (!familys || familys->count <= 0)
			familys = new_font_family(wg->font_ctx,
				"微软雅黑,Tahoma,Arial Unicode MS,Segoe UI,Arial,Consolas,Segoe UI Emoji", 0);
	}
	if (!familys || familys->count <= 0)
		std::fprintf(stderr, "hud: font family has no faces, text is disabled\n");
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
	os_window* form = wg->create("vgui runtime HUD (M toggles compositing)",
		win_w, win_h, 0, true);
	if (!form || !form->viewport)
	{
		std::fprintf(stderr, "hud: window creation failed\n");
		return 2;
	}

	hud_state st;
	build_hud(&st, familys, win_w, win_h);
	form->viewport->ui = st.ui;

	st.renderer = phase0_renderer_create(wg->device);
	// R5-1: the world panel's FBO (cycle_target: rewritten every frame and
	// sampled by phase0 one submission earlier) and the billboard hookup.
	st.panel_fbo = vgpu_create_fbo(wg->vgctx, 256, 128, nullptr);
	st.panel_fbo.cycle_target = true;
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
	// Player camera: slightly above the arena looking at the centre.  The
	// nameplate projection uses the same parameters.
	phase0_camera cam;
	cam.eye[0] = st.cam_eye[0]; cam.eye[1] = st.cam_eye[1]; cam.eye[2] = st.cam_eye[2];
	cam.target[0] = st.cam_target[0]; cam.target[1] = st.cam_target[1];
	cam.target[2] = st.cam_target[2];
	cam.fov_deg = st.cam_fov;
	phase0_renderer_set_camera(st.renderer, &cam);
	const float billboard_pos[3] = { 0.0f, 3.4f, 0.0f };
	phase0_renderer_set_billboard(st.renderer,
		vgpu_fbo_color_texture(&st.panel_fbo), billboard_pos, 3.2f, 1.6f);

	// R5-1 world panel: a small UI context rendered into its own FBO; the
	// texture is borrowed by phase0 for a camera-facing world billboard.
	st.panel_ui = vgui_create(familys, 16.0f);
	vgui_context_set_size(st.panel_ui, 256, 128);
	{
		vgui_widget* proot = vgui_root(st.panel_ui);
		vgui_widget_set_layout(proot, VGUI_LAYOUT_COLUMN);
		vgui_widget_set_padding(proot, 10, 8, 10, 8);
		vgui_widget_set_gap(proot, 5.0f);
		vgui_widget_set_background(proot, 0x98101418);
		vgui_widget* title = vgui_widget_add(proot, VGUI_LABEL, "ARENA STATUS");
		vgui_widget_set_font_size(title, 13.0f);
		vgui_widget* ph = vgui_widget_add(proot, VGUI_PROGRESS, nullptr);
		vgui_widget_set_size(ph, 0.0f, 12.0f);
		vgui_widget_set_accent(ph, 0xFFE04040);
		vgui_widget* pm = vgui_widget_add(proot, VGUI_PROGRESS, nullptr);
		vgui_widget_set_size(pm, 0.0f, 12.0f);
		vgui_widget_set_accent(pm, 0xFF4070E0);
	}
	st.vg_panel = cb->new_rvg(cb->ac);
	st.vg_panel->width = 256;
	st.vg_panel->height = 128;

	vg_fbo_t* fbo = &form->fbo;
	FpsCounter fps;
	runtime_cx rtc;
	int last_w = win_w;
	int last_h = win_h;
	int smoke_frames = 0;
	float upms = 0.0f, drawms = 0.0f;

	// Startup mode can be pinned with VGUI_HUD_MODE=0/1; with it set the smoke
	// run stays on one compositing path (handy for regression tests).
	bool mode_pinned = false;
	if (const char* m = std::getenv("VGUI_HUD_MODE"); m && *m)
	{
		st.mode = (*m == '1') ? 1 : 0;
		mode_pinned = true;
	}

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
		update_nameplates(&st, cw, ch);
		rtc.begin();
		vgui_update(st.ui, delta);
		upms = (float)rtc.end();

		// 3D first: it owns its command buffer and submits immediately.
		feed_scene(&st);
		phase0_renderer_resize(st.renderer, cw, ch);
		phase0_renderer_render(st.renderer, st.time);

		// Record the HUD.
		rtc.begin();
		cb->clear(vg);
		cb->reset_clip(vg, 1);
		vgui_draw(st.ui, cb, vg);
		drawms = (float)rtc.end();

		// The engine composites first: scene into the swapchain inside vgpu's
		// pending command buffer, then the HUD pass(es) with load-op LOAD.
		// Every vgpu pass of the frame goes through ONE vgpu_render_frame call
		// so all passes share one submission and SDL3's automatic barriers
		// order them (offscreen write -> sampled, blit -> LOAD).  Never split a
		// frame's passes across submissions: SDL3 does not serialize submits,
		// and uploads to the shared vertex buffers from a later submission can
		// clobber the previous one's in-flight geometry.
		fbo->clear = false;
		blit_scene_to_swapchain(wg->vgctx, fbo, &st);

		// R5-1 world panel: aggregate stats + record into its own FBO.
		{
			float sum_hp = 0.0f, sum_mp = 0.0f;
			for (int i = 0; i < kEntityCount; ++i)
			{
				sum_hp += st.ent_hp[i];
				sum_mp += st.ent_mp[i];
			}
			vgui_widget_set_value(vgui_widget_get_child(
				vgui_root(st.panel_ui), 1), sum_hp / (float)kEntityCount);
			vgui_widget_set_value(vgui_widget_get_child(
				vgui_root(st.panel_ui), 2), sum_mp / (float)kEntityCount);
			vgui_update(st.panel_ui, delta);
			cb->clear(st.vg_panel);
			cb->reset_clip(st.vg_panel, 1);
			vgui_draw(st.panel_ui, cb, st.vg_panel);
		}

		// All vgpu passes of the frame in one submission: the offscreen world
		// panel first (phase0 samples its texture next frame), then the HUD
		// passes (mode A direct / mode B via the offscreen HUD + composite).
		ovg_draw_data_t items[3] = {};
		size_t item_count = 0;
		items[item_count] = get_draw_list(st.vg_panel);
		items[item_count].fbo = &st.panel_fbo;
		++item_count;

		if (st.mode == 0)
		{
			items[item_count] = get_draw_list(vg);
			items[item_count].fbo = fbo;
			++item_count;
		}
		else
		{
			ovg_texture_r composite = {};
			composite.tex = &st.ui_tex;
			composite.rc = { -1, 0, 0, 0 };		// whole texture
			composite.sliced = { 0, 0, 0, 0 };	// no 9-slice
			composite.dst = { 0, 0, cw, ch };
			composite.color = 0xFFFFFFFFu;
			cb->clear(vg_composite);
			cb->reset_clip(vg_composite, 1);
			cb->add_texture(vg_composite, &composite);

			items[item_count] = get_draw_list(vg);
			items[item_count].fbo = &st.ui_fbo;
			++item_count;
			items[item_count] = get_draw_list(vg_composite);
			items[item_count].fbo = fbo;
			++item_count;
		}
		vgpu_render_frame(wg->vgctx, items, item_count);

		++st.frames;
		if (st.frames % 20 == 0)
			draw_status(&st, &fps, upms, drawms);

		if (smoke)
		{
			// Mid-run window resize exercises the swapchain-recreate path
			// (vg->width, ui_fbo reset, native texture rebind).
			if (smoke_frames == 10)
				SDL_SetWindowSize(form->window, 1000, 700);
			// R5-2 self-test: click on the first visible entity's projected
			// anchor, expect it selected; then right-click to deselect.
			if (smoke_frames == 14)
			{
				float px = 0.0f, py = 0.0f;
				int target = -1;
				for (int i = 0; i < kEntityCount && target < 0; ++i)
					if (project_entity(&st, i, st.last_w, st.last_h, &px, &py))
						target = i;
				if (target >= 0)
				{
					vgui_input_mouse_move(st.ui, px, py);
					vgui_input_mouse_down(st.ui, 1, px, py, 0);
					vgui_input_mouse_up(st.ui, 1, px, py, 0);
				}
			}
			if (smoke_frames == 15)
				std::printf("pick self-test: %s (selected=%d)\n",
					st.selected >= 0 ? "ok" : "FAIL", st.selected);
			if (smoke_frames == 18)
			{
				vgui_input_mouse_move(st.ui, 4.0f, 4.0f);
				vgui_input_mouse_down(st.ui, 3, 4.0f, 4.0f, 0);
				vgui_input_mouse_up(st.ui, 3, 4.0f, 4.0f, 0);
			}
			if (smoke_frames == 19)
				std::printf("deselect self-test: %s (selected=%d)\n",
					st.selected == -1 ? "ok" : "FAIL", st.selected);
			// Exercise both modes before exiting (unless pinned).
			if (!mode_pinned && st.frames == 20) st.mode = 1;
			if (++smoke_frames >= 40)
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
	if (st.panel_fbo.ctx)
		vgpu_destroy_fbo(&st.panel_fbo);
	vgui_destroy(st.panel_ui);
	cb->destroy_rvg(st.vg_panel);
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
