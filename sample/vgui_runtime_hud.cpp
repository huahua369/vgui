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
// The screen HUD is loaded from an editor design or delivery package; the host
// supplies safe-area insets, simulation values, action handlers and custom
// minimap/crosshair draws. The design owns screen-space placement.
// Run with --smoke to render both modes or --check-design for a no-window check.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "pch.h"
#include "ovg_main.h"
#include <SDL3/SDL.h>
#include <vgui/backend/vgui_sdl3.h>
#include <vgui/vgui.h>
#include <ovg_fonts.h>

#include "phase0_renderer.h"
#include "delivery.h"
#include "runtime.h"
#include "ui_json_images.h"

namespace ui = vgui::ui_design;
namespace fs = std::filesystem;

static constexpr int kInitialWidth = 1280;
static constexpr int kInitialHeight = 800;
static constexpr int kEntityCount = 12;
static constexpr std::array kSkillActions = {
	"hud.skill.1", "hud.skill.2", "hud.skill.3", "hud.skill.4", "hud.skill.5", "hud.skill.6"
};
enum hud_progress_index { hp_progress, shield_progress, ult_progress, first_skill_progress };

struct hud_progress
{
	const char* id;
	const char* key;
	vgui_widget* widget = nullptr;
	bool bound = false;

	void set(ui::runtime& design, float ratio) const
	{
		if (!bound)
			vgui_widget_set_value(widget, ratio); // Older HUD designs have no field.
		else if (!design.set_data(key, ui::value{static_cast<double>(ratio)}))
			std::fprintf(stderr, "hud: cannot update data field '%s'\n", key);
	}
};

struct hud_input
{
	ui::embedded_delivery content;
	fs::path project_root;
	bool embedded = false;
};

static bool read_hud(const fs::path& source, hud_input& input, ui::diagnostic& error)
{
	std::error_code ec;
	if (fs::is_directory(source, ec))
	{
		input.project_root = source;
		return ui::read_delivery(source, input.content.design, &error);
	}
	input.embedded = source.native().ends_with(fs::path(ui::json_delivery_suffix).native());
	if (input.embedded) return ui::read_json_delivery(source, input.content, &error);
	input.project_root = source.has_parent_path() ? source.parent_path() : fs::path(".");
	return ui::read_document(source, input.content.design, &error);
}

struct hud_mount_assets
{
	const hud_input& input;
	font_cache_cx* fonts;
	vgui_sample::json_image_decoder images;
	std::size_t font_bytes = 0;
};

using hud_insets = std::array<int, 4>; // Logical left, top, right, bottom.

struct hud_state
{
	vgui_context* ui = nullptr;
	std::unique_ptr<ui::runtime> design;
	hud_mount_assets* loading = nullptr; // Only during the synchronous mount.
	std::vector<std::unique_ptr<font_familys_t, decltype(&delete_font_family)>> design_fonts;
	phase0_renderer* renderer = nullptr;

	// HUD widgets bound to simulated game state.
	std::array<hud_progress, first_skill_progress + kSkillActions.size()> progress{{
		{"hp", "player.hp_ratio"},
		{"shield", "player.shield_ratio"},
		{"ult", "player.ult_cooldown"},
		{"cooldown1", "skill.1.cooldown"},
		{"cooldown2", "skill.2.cooldown"},
		{"cooldown3", "skill.3.cooldown"},
		{"cooldown4", "skill.4.cooldown"},
		{"cooldown5", "skill.5.cooldown"},
		{"cooldown6", "skill.6.cooldown"},
	}};
	vgui_widget* status = nullptr;
	std::array<bool, kSkillActions.size()> pending_skills{};

	// R4-2 demo: radial ultimate cooldown + safe-area cycling.
	float ult_cd = 0.0f;
	std::size_t safe_level = 0;
	std::optional<hud_insets> pending_safe_area;

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
// Input: M toggles the compositing mode, Q/W/E/R/T/Y use hotbar skills
// (TAB itself is consumed by the core as the focus-traversal key, so app key
// handlers never see it.)
// ---------------------------------------------------------------------------

static const char kSkillKeys[6] = { 'Q', 'W', 'E', 'R', 'T', 'Y' };
static void activate_skill(hud_state* st, int slot)
{
	st->skill_cd[slot] = 3.0f;
	// R4-2 floating text demo: report the fresh cooldown above the slot.
	float x, y, width, height;
	vgui_widget_get_rect(st->progress[first_skill_progress + slot].widget, &x, &y, &width, &height);
	vgui_spawn_float_text(st->ui, x + width * 0.5f, y - 6.0f, "3.0s", 0xFFFFE080, 1.2f, 26.0f);
}

static void apply_hud_actions(hud_state* st)
{
	// The JSON callback runs during input dispatch; update live widgets afterward.
	for (int i = 0; i < (int)st->pending_skills.size(); ++i)
		if (std::exchange(st->pending_skills[i], false)) activate_skill(st, i);
}

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
			st->pending_skills[i] = true;
			return 1;
		}
	}
	if (key == 'U')
	{
		st->ult_cd = 5.0f;
		return 1;
	}
	if (key == 'B')
	{
		static constexpr std::array kMargins = {0, 60, 100};
		st->safe_level = (st->safe_level + 1) % kMargins.size();
		const int inset = kMargins[st->safe_level];
		st->pending_safe_area = hud_insets{inset, inset, inset, inset};
		return 1;
	}
	return 0;
}

// ---------------------------------------------------------------------------
// HUD tree
// ---------------------------------------------------------------------------

static bool layout_hud(hud_state* st, int width, int height)
{
	if (!st->pending_safe_area && st->last_w == width && st->last_h == height) return true;
	// Apply safe-area changes after input dispatch, even at the same viewport size.
	if (st->pending_safe_area)
	{
		const auto [left, top, right, bottom] = *st->pending_safe_area;
		vgui_context_set_safe_area(st->ui, left, top, right, bottom);
	}
	if (!st->design->layout({width, height, 1.0f})) return false;
	st->pending_safe_area.reset();
	st->last_w = width;
	st->last_h = height;
	return true;
}

static font_familys_t* load_hud_font(std::string_view key, const fs::path& path, void* user)
{
	auto* st = static_cast<hud_state*>(user);
	auto& assets = *st->loading;
	if (!assets.fonts) return nullptr;
	const std::string alias = key == "builtin.sans" ? "HudBundled" : "hud.design." + std::string(key);
	if (key != "builtin.sans")
	{
		const auto file = path.empty() ? fs::u8path(VGUI_SOURCE_DIR) / "sample/res/fonts/DesignerSansSC.ttf" : path;
		constexpr std::size_t max_font_file = 16u << 20, max_font_bytes = 64u << 20;
		auto bytes = vgui_sample::read_asset(file, std::min(max_font_file, max_font_bytes - assets.font_bytes));
		assets.font_bytes += bytes.size();
		if (bytes.empty() || !load_font_from_memory(assets.fonts, bytes.data(), (long)bytes.size(), alias.c_str()))
			return nullptr;
	}
	std::unique_ptr<font_familys_t, decltype(&delete_font_family)> family(
		new_font_family(assets.fonts, alias.c_str(), nullptr), delete_font_family);
	if (!family || family->count <= 0) return nullptr;
	return st->design_fonts.emplace_back(std::move(family)).get();
}

static bool build_hud(hud_state* st, const hud_input& input, font_cache_cx* cache, font_familys_t* family)
{
	st->ui = vgui_create(family, 16.0f);
	if (!st->ui) return false;
	st->design = std::make_unique<ui::runtime>(st->ui);
	const auto& document = input.content.design;
	ui::runtime_bindings bindings;
	bindings.project_root = input.project_root;
	bindings.user = st;
	bindings.load_font = load_hud_font;
	if (input.embedded)
		bindings.load_embedded_image = [](vgui_context* ctx, std::string_view key, void* user) -> vgui_texture* {
			auto& assets = *static_cast<hud_state*>(user)->loading;
			const auto it = assets.input.content.images.find(key);
			return it == assets.input.content.images.end() ? nullptr :
				assets.images.load(ctx, std::span<const std::uint8_t>(it->second));
		};
	else
		bindings.load_image = [](vgui_context* ctx, std::string_view, const fs::path& file, void* user) {
			return static_cast<hud_state*>(user)->loading->images.load(ctx, file);
		};
	bindings.on_action = [](std::string_view, std::string_view action, void* user) noexcept {
		auto* state = static_cast<hud_state*>(user);
		for (std::size_t i = 0; i < kSkillActions.size(); ++i)
			if (action == kSkillActions[i]) state->pending_skills[i] = true;
	};
	ui::diagnostic error;
	bool mounted = false;
	{
		hud_mount_assets assets{input, cache, {}, 0};
		st->loading = &assets;
		mounted = st->design->mount(document, 1, bindings, &error);
		st->loading = nullptr;
	} // Drop loader refs; the mounted widgets retain assigned textures.
	if (!mounted)
	{
		std::fprintf(stderr, "hud: cannot mount design (%s): %s\n", error.path.c_str(), error.message.c_str());
		return false;
	}
	for (const auto& warning : st->design->diagnostics())
		std::fprintf(stderr, "hud: resource warning (%s): %s\n", warning.path.c_str(), warning.message.c_str());

	const auto require = [&](const char* id, vgui_widget_kind kind) -> vgui_widget* {
		auto* w = st->design->widget(id);
		if (w && vgui_widget_get_kind(w) == kind) return w;
		std::fprintf(stderr, "hud: missing or incompatible widget ID '%s'\n", id);
		return nullptr;
	};
	auto* root = vgui_root(st->ui);
	auto* page = st->design->widget(document.root);
	const auto& root_props = document.nodes.at(document.root).props;
	const auto root_number = [&](const char* key) {
		const auto it = root_props.find(key);
		return it == root_props.end() ? 0.0f : (float)std::get<double>(it->second);
	};
	for (const auto* key : {"x", "y", "width", "height"})
		if (root_number(key) != 0.0f)
		{
			std::fprintf(stderr, "hud: root must fill the viewport at (0, 0); set '%s' to zero\n", key);
			return false;
		}
	if (const auto it = root_props.find("layout"); it != root_props.end() && std::get<std::string>(it->second) != "absolute")
	{
		std::fprintf(stderr, "hud: root must use absolute layout\n");
		return false;
	}
	const auto fields = ui::data_fields(document);
	for (auto& field : st->progress)
	{
		field.widget = require(field.id, VGUI_PROGRESS);
		if (!field.widget) return false;
		const auto& props = document.nodes.at(field.id).props;
		const auto prop = props.find("bind_value");
		const std::string_view declared = prop == props.end() ?
			std::string_view{} : std::string_view(std::get<std::string>(prop->second));
		field.bound = declared == field.key;
		// Only an undeclared field and an unbound widget qualify for the
		// legacy direct setter.
		if (!field.bound && (!declared.empty() || fields.contains(field.key)))
		{
			std::fprintf(stderr, "hud: widget '%s' has an incompatible binding; expected bind_value '%s'\n",
				field.id, field.key);
			return false;
		}
	}
	st->status = require("status", VGUI_LABEL);
	if (!st->status) return false;

	auto* minimap = require("minimap", VGUI_PANEL);
	auto* crosshair = require("crosshair", VGUI_PANEL);
	if (!minimap || !crosshair) return false;

	// JSON owns the bounds; these two panels host the engine's custom draws.
	const auto add_canvas = [&](vgui_widget* panel, vgui_custom_draw_fn draw) {
		auto* canvas = vgui_widget_add(panel, VGUI_CUSTOM, nullptr);
		vgui_widget_set_grow(canvas, 1.0f);
		vgui_widget_set_align_self(canvas, VGUI_ALIGN_STRETCH);
		vgui_custom_set_draw(canvas, draw, st);
	};
	add_canvas(minimap, draw_minimap);
	add_canvas(crosshair, draw_crosshair);

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
	vgui_widget_on_event(page, on_root_event, st); // The loaded panel covers the built-in root.

	vgui_context_add_key_handler(st->ui, on_key, st);
	if (!layout_hud(st, kInitialWidth, kInitialHeight)) return false;
	st->design->focus_default();
	vgui_update(st->ui, 0.0f);
	return true;
}

static void destroy_hud(hud_state* st)
{
	st->design.reset(); // Mounted widgets borrow the context and the fonts.
	vgui_destroy(st->ui);
	st->design_fonts.clear();
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

// ---------------------------------------------------------------------------
// Game simulation (stands in for the engine)
// ---------------------------------------------------------------------------

static void simulate(hud_state* st, float delta)
{
	st->time += delta;
	st->hp_value = 0.62f + 0.36f * std::sin(st->time * 0.6f);
	st->shield_value = 0.45f + 0.45f * std::sin(st->time * 0.9f + 1.3f);
	auto& design = *st->design;
	st->progress[hp_progress].set(design, st->hp_value);
	st->progress[shield_progress].set(design, st->shield_value);
	for (std::size_t i = 0; i < kSkillActions.size(); ++i)
	{
		if (st->skill_cd[i] > 0.0f)
			st->skill_cd[i] = std::max(0.0f, st->skill_cd[i] - delta);
		st->progress[first_skill_progress + i].set(design, st->skill_cd[i] / 3.0f);
	}
	if (st->ult_cd > 0.0f)
		st->ult_cd = std::max(0.0f, st->ult_cd - delta);
	st->progress[ult_progress].set(design, st->ult_cd / 5.0f);
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
}

static int check_hud(const hud_input& input)
{
	hud_state st;
	if (!build_hud(&st, input, nullptr, nullptr))
	{
		destroy_hud(&st);
		return 1;
	}
	bool ok = true;
	const auto check = [&](bool pass, const char* message) {
		if (!pass) std::fprintf(stderr, "hud design check: %s\n", message);
		ok = ok && pass;
	};
	const auto rect = [](vgui_widget* widget) {
		ui::pixel_rect r;
		vgui_widget_get_rect(widget, &r.x, &r.y, &r.width, &r.height);
		return r;
	};
	const auto near = [](float a, float b) { return std::abs(a - b) < 0.01f; };
	const auto same_size = [&](ui::pixel_rect actual, ui::pixel_rect authored) {
		return actual.width > 0 && actual.height > 0 &&
			near(actual.width, authored.width) && near(actual.height, authored.height);
	};
	const auto initial_hotbar = rect(st.design->widget("hotbar"));
	const auto initial_minimap = rect(st.design->widget("minimap"));
	const auto initial_reticle = rect(st.design->widget("crosshair"));
	const auto& hp = st.progress[hp_progress];
	const auto initial_hp_rect = rect(hp.widget);
	const auto hotbar_margin = kInitialHeight - initial_hotbar.y - initial_hotbar.height;
	const auto minimap_margin = kInitialHeight - initial_minimap.y - initial_minimap.height;

	const auto check_layout = [&](int width, int height, hud_insets safe) {
		const bool laid_out = layout_hud(&st, width, height);
		check(laid_out, "could not lay out the mounted design");
		if (!laid_out) return;
		const auto [left, top, right, bottom] = safe;
		const float safe_right = (float)(width - right);
		const float safe_bottom = (float)(height - bottom);
		const auto hp_rect = rect(hp.widget);
		check(same_size(hp_rect, initial_hp_rect) && near(hp_rect.x - left, initial_hp_rect.x) &&
			near(hp_rect.y - top, initial_hp_rect.y), "vitals must move with the upper-left safe-area edge");
		const auto hotbar = rect(st.design->widget("hotbar"));
		check(same_size(hotbar, initial_hotbar) &&
			near(hotbar.x + hotbar.width * 0.5f, (left + safe_right) * 0.5f) &&
			near(safe_bottom - hotbar.y - hotbar.height, hotbar_margin),
			"hotbar must be bottom-centered in the safe area, with authored size and margin");
		const auto minimap = rect(st.design->widget("minimap"));
		check(same_size(minimap, initial_minimap) &&
			near(minimap.x - left, initial_minimap.x) &&
			near(safe_bottom - minimap.y - minimap.height, minimap_margin) &&
			minimap.x >= left && minimap.x + minimap.width <= safe_right &&
			minimap.y >= top && minimap.y + minimap.height <= safe_bottom,
			"minimap must keep its lower-left margins inside the safe area");
		auto* crosshair = st.design->widget("crosshair");
		const auto reticle = rect(crosshair);
		check(same_size(reticle, initial_reticle) &&
			near(reticle.x + reticle.width * 0.5f, width * 0.5f) &&
			near(reticle.y + reticle.height * 0.5f, height * 0.5f),
			"crosshair must stay at the full viewport center");
		const auto canvas = rect(vgui_widget_get_child(crosshair, 0));
		check(same_size(canvas, reticle) && near(canvas.x, reticle.x) && near(canvas.y, reticle.y),
			"engine canvas must fill the authored reticle panel");
		const auto ult = rect(st.design->widget("ult"));
		check(ult.x >= hotbar.x + hotbar.width || ult.x + ult.width <= hotbar.x ||
			ult.y >= hotbar.y + hotbar.height || ult.y + ult.height <= hotbar.y,
			"ultimate ring must not overlap the hotbar");
	};

	check_layout(1600, 900, {});
	auto* skill = st.design->widget("skill1");
	check(skill && vgui_widget_get_kind(skill) == VGUI_BUTTON, "expected a mounted skill1 button");
	if (skill && vgui_widget_get_kind(skill) == VGUI_BUTTON)
	{
		std::array<float, first_skill_progress + kSkillActions.size()> authored{}, live{}, expected{};
		std::array<double, first_skill_progress + kSkillActions.size()> transitions{};
		for (std::size_t i = 0; i < st.progress.size(); ++i)
		{
			const auto& field = st.progress[i];
			if (!field.bound)
				std::fprintf(stderr, "hud design check: missing bind_value '%s' on '%s'\n", field.key, field.id);
			ok = ok && field.bound;
			authored[i] = vgui_widget_get_value(field.widget);
			const auto& props = input.content.design.nodes.at(field.id).props;
			if (const auto duration = props.find("bind_value_transition_ms"); duration != props.end())
				transitions[i] = std::get<double>(duration->second) / 1000.0;
		}
		const auto button = rect(skill);
		const float x = button.x + button.width * 0.5f;
		const float y = button.y + button.height * 0.5f;
		vgui_input_mouse_move(st.ui, x, y);
		vgui_input_mouse_down(st.ui, 1, x, y, 1);
		check(!st.pending_skills[0], "skill cannot activate until the click completes");
		vgui_input_mouse_up(st.ui, 1, x, y, 1);
		check(st.pending_skills[0] && st.skill_cd[0] == 0, "click must queue the JSON action without updating in dispatch");
		apply_hud_actions(&st);
		for (std::size_t i = 1; i < kSkillActions.size(); ++i)
			st.skill_cd[i] = 1.0f + 0.25f * static_cast<float>(i);
		st.ult_cd = 4.0f;
		simulate(&st, 0.1f);
		expected[hp_progress] = st.hp_value;
		expected[shield_progress] = st.shield_value;
		expected[ult_progress] = st.ult_cd / 5.0f;
		for (std::size_t i = 0; i < kSkillActions.size(); ++i)
			expected[first_skill_progress + i] = st.skill_cd[i] / 3.0f;
		for (std::size_t i = 0; i < st.progress.size(); ++i)
			check(near(vgui_widget_get_value(st.progress[i].widget), transitions[i] > 0.0 ? authored[i] : expected[i]),
				"only configured meters should defer their displayed value");
		st.design->advance_animations(*std::max_element(transitions.begin(), transitions.end()));
		for (std::size_t i = 0; i < st.progress.size(); ++i)
		{
			live[i] = vgui_widget_get_value(st.progress[i].widget);
			check(near(live[i], expected[i]), "simulation data must reach each HUD meter by its transition end");
		}
		check(live[first_skill_progress] > 0.9f, "JSON action did not start the live cooldown");
		check(!st.design->set_data("hud.unknown", ui::value{0.25}), "unknown fields must be rejected");
		check(!st.design->set_data(hp.key, ui::value{true}) && vgui_widget_get_value(hp.widget) == live[hp_progress],
			"invalid data must leave the live HP unchanged");

		constexpr int wide_width = 2520, wide_height = 1080; // 21:9
		check_layout(wide_width, wide_height, {});
		check(vgui_input_key(st.ui, 'B', 1, 0) != 0, "B must cycle the safe-area preset");
		check_layout(wide_width, wide_height, {60, 60, 60, 60});
		constexpr hud_insets asymmetric{96, 40, 32, 104};
		st.pending_safe_area = asymmetric;
		check_layout(wide_width, wide_height, asymmetric);

		check(st.design->widget("skill1") == skill, "viewport and safe-area changes must not remount skill1");
		for (std::size_t i = 0; i < st.progress.size(); ++i)
		{
			const auto& field = st.progress[i];
			auto* current = st.design->widget(field.id);
			check(current == field.widget, "viewport and safe-area changes must not remount HUD meters");
			check(current && vgui_widget_get_value(current) == live[i],
				"viewport and safe-area changes must not reset live HUD values");
			// Reverting the override proves simulate() wrote to the field,
			// not directly to the widget as it would for a legacy design.
			check(st.design->set_data(field.key, std::nullopt) && current &&
				vgui_widget_get_value(current) == authored[i], "clearing the override must restore the design value");
		}
	}
	destroy_hud(&st);
	if (ok) std::puts("hud design check: ok (no remount)");
	return ok ? 0 : 1;
}

int main(int argc, char** argv)
{
	bool smoke = false, check = false;
	fs::path source;
	for (int i = 1; i < argc; ++i)
	{
		const std::string_view arg = argv[i];
		if (arg == "--smoke") smoke = true;
		else if (arg == "--check-design") check = true;
		else if (arg == "--help")
		{
			std::puts("Usage: vgui_runtime_hud [--smoke | --check-design] [design.vgui.json | package.vgui.package.json | delivery-directory]");
			return 0;
		}
		else if (!arg.starts_with('-') && source.empty()) source = fs::u8path(arg);
		else
		{
			std::fprintf(stderr, "hud: unexpected argument '%s' (see --help)\n", argv[i]);
			return 2;
		}
	}
	if (source.empty()) source = fs::u8path(VGUI_SOURCE_DIR) / "sample/res/game-hud.vgui.json";
	hud_input input;
	ui::diagnostic error;
	if (!read_hud(source, input, error))
	{
		std::fprintf(stderr, "hud: cannot load design (%s): %s\n", error.path.c_str(), error.message.c_str());
		return 1;
	}
	if (check) return check_hud(input);

	auto* wg = new app_mgr();
	// Runtime apps bundle their fonts (sample/res/fonts, see
	// LICENSE-DejaVu.txt) instead of relying on the system fontconfig
	// database: register the file from memory via the C API, then fall back
	// to the system lookup if the bundled file is unavailable.
	font_familys_t* familys = nullptr;
	{
		auto bytes = vgui_sample::read_asset(fs::u8path(VGUI_SOURCE_DIR) / "sample/res/fonts/DejaVuSans.ttf", 16u << 20);
		if (!bytes.empty()) load_font_from_memory(wg->font_ctx, bytes.data(), (long)bytes.size(), "HudBundled");
		familys = new_font_family(wg->font_ctx, "HudBundled", nullptr);
		if (!familys || familys->count <= 0)
			familys = new_font_family(wg->font_ctx,
				"微软雅黑,Tahoma,Arial Unicode MS,Segoe UI,Arial,Consolas,Segoe UI Emoji", 0);
	}
	if (!familys || familys->count <= 0)
		std::fprintf(stderr, "hud: font family has no faces, text is disabled\n");
	hud_state st;
	const auto destroy_host = [&] {
		destroy_hud(&st);
		delete_font_family(familys);
		delete wg;
	};
	if (!build_hud(&st, input, wg->font_ctx, familys))
	{
		destroy_host();
		return 1;
	}
	if (!wg->init_gpu(true))
	{
		std::fprintf(stderr, "hud: init_gpu failed\n");
		destroy_host();
		return smoke ? 77 : 2;
	}

	os_window* form = wg->create("vgui runtime HUD (M toggles compositing)",
		kInitialWidth, kInitialHeight, ef_resizable, true);
	if (!form || !form->viewport)
	{
		std::fprintf(stderr, "hud: window creation failed\n");
		destroy_host();
		return smoke ? 77 : 2;
	}
	st.renderer = phase0_renderer_create(wg->device);
	if (!st.renderer)
	{
		std::fprintf(stderr, "hud: 3D renderer creation failed\n");
		wg->wait_gpu();
		destroy_host();
		return 2;
	}
	form->viewport->ui = st.ui;
	auto* cb = new_ctx_cb();
	rvg_t* vg = cb->new_rvg(cb->ac);
	rvg_t* vg_composite = cb->new_rvg(cb->ac);
	// R5-1: the world panel's FBO (cycle_target: rewritten every frame and
	// sampled by phase0 one submission earlier) and the billboard hookup.
	st.panel_fbo = vgpu_create_fbo(wg->vgctx, 256, 128, nullptr);
	st.panel_fbo.cycle_target = true;
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
	int smoke_frames = 0;
	bool ok = true;
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
		if (!layout_hud(&st, cw, ch))
		{
			std::fprintf(stderr, "hud: invalid viewport %dx%d\n", cw, ch);
			ok = false;
			break;
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

		st.design->service_modals();
		apply_hud_actions(&st);
		simulate(&st, delta);
		update_nameplates(&st, cw, ch);
		rtc.begin();
		st.design->advance_animations(delta);
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
			{
				ok = ok && st.selected >= 0;
				std::printf("pick self-test: %s (selected=%d)\n",
					st.selected >= 0 ? "ok" : "FAIL", st.selected);
			}
			if (smoke_frames == 18)
			{
				vgui_input_mouse_move(st.ui, 4.0f, 4.0f);
				vgui_input_mouse_down(st.ui, 3, 4.0f, 4.0f, 0);
				vgui_input_mouse_up(st.ui, 3, 4.0f, 4.0f, 0);
			}
			if (smoke_frames == 19)
			{
				ok = ok && st.selected == -1;
				std::printf("deselect self-test: %s (selected=%d)\n",
					st.selected == -1 ? "ok" : "FAIL", st.selected);
			}
			// Exercise both modes before exiting (unless pinned).
			if (!mode_pinned && st.frames == 20) st.mode = 1;
			if (++smoke_frames >= 40)
				break;
		}
	}

	if (smoke) ok = ok && smoke_frames == 40;
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
	form->viewport->ui = nullptr;
	destroy_hud(&st);
	delete_font_family(familys);
	cb->destroy_rvg(vg);
	cb->destroy_rvg(vg_composite);
	free_ctx_cb(cb);
	delete wg;

	if (smoke)
		std::printf("hud smoke: %s (%d frames, mode %d)\n",
			ok ? "ok" : "failed", st.frames, st.mode);
	return ok ? 0 : 1;
}
