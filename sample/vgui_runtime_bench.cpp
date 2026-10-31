// sample/vgui_runtime_bench.cpp: R3-2 runtime benchmark.
//
// Renders a synthetic HUD scene headlessly (offscreen FBO, no window) and
// reports per-segment frame timings so the per-frame cost of the retained
// tree (update / draw / submit) can be tracked over time:
//
//   status plate + 4 vitals plates + hotbar (6 slots)
//   + BENCH_WIDGETS static panels (4 labels + 2 bars each)
//   + 24 world-anchored nameplates repositioned every frame
//
// Environment variables:
//   BENCH_FRAMES    frames to run (default 600, first 30 are warmup)
//   BENCH_WIDGETS   extra static panels (default 0)
//
// Output: avg/max milliseconds for update / draw / submit.

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

static const int kWidth = 1280;
static const int kHeight = 800;
static const int kNameplates = 24;

struct bench_state
{
	vgui_context* ui = nullptr;
	font_familys_t* font = nullptr;
	std::vector<vgui_widget*> panels;		// BENCH_WIDGETS static panels
	vgui_widget* status = nullptr;

	vgui_widget* plates[kNameplates] = {};
	vgui_widget* plate_hp[kNameplates] = {};
	vgui_widget* plate_mp[kNameplates] = {};
	float ent_x[kNameplates] = {};
	float ent_z[kNameplates] = {};
	float ent_hp[kNameplates] = {};
	float ent_mp[kNameplates] = {};

	float time = 0.0f;
	int frames = 0;
	std::vector<vgui_widget*> cache_targets;
};

static void build_bench(bench_state* st, font_familys_t* familys, int widgets,
	int cache)
{
	st->ui = vgui_create(familys, 16.0f);
	vgui_context_set_size(st->ui, kWidth, kHeight);
	vgui_widget* root = vgui_root(st->ui);
	vgui_widget_set_layout(root, VGUI_LAYOUT_ABSOLUTE);
	vgui_widget_set_background(root, 0);

	// Top-right status plate.
	vgui_widget* plate = vgui_widget_add(root, VGUI_PANEL, nullptr);
	vgui_widget_set_pos(plate, (float)kWidth - 316.0f, 16.0f);
	vgui_widget_set_size(plate, 300.0f, 56.0f);
	vgui_widget_set_padding(plate, 10, 8, 10, 8);
	vgui_widget_set_background(plate, 0x98101418);
	st->status = vgui_widget_add(plate, VGUI_LABEL, "bench status line");
	if (cache)
		st->cache_targets.push_back(plate);

	// Four vitals plates.
	for (int i = 0; i < 4; ++i)
	{
		vgui_widget* p = vgui_widget_add(root, VGUI_PANEL, nullptr);
		vgui_widget_set_pos(p, 16.0f, 16.0f + i * 86.0f);
		vgui_widget_set_size(p, 280.0f, 76.0f);
		vgui_widget_set_layout(p, VGUI_LAYOUT_COLUMN);
		vgui_widget_set_padding(p, 10, 8, 10, 8);
		vgui_widget_set_gap(p, 6.0f);
		vgui_widget_set_background(p, 0x98101418);
		for (int b = 0; b < 2; ++b)
		{
			vgui_widget* bar = vgui_widget_add(p, VGUI_PROGRESS, nullptr);
			vgui_widget_set_size(bar, 0.0f, 14.0f);
			vgui_widget_set_value(bar, 0.5f);
		}
	}

	// Bottom hotbar: button + cooldown bar per slot.
	vgui_widget* hotbar = vgui_widget_add(root, VGUI_PANEL, nullptr);
	vgui_widget_set_pos(hotbar, (float)kWidth * 0.5f - 244.0f,
		(float)kHeight - 96.0f);
	vgui_widget_set_size(hotbar, 488.0f, 80.0f);
	vgui_widget_set_layout(hotbar, VGUI_LAYOUT_ROW);
	vgui_widget_set_gap(hotbar, 8.0f);
	vgui_widget_set_padding(hotbar, 8, 8, 8, 8);
	vgui_widget_set_background(hotbar, 0x80101418);
	if (cache)
		st->cache_targets.push_back(hotbar);
	for (int i = 0; i < 6; ++i)
	{
		vgui_widget* slot = vgui_widget_add(hotbar, VGUI_PANEL, nullptr);
		vgui_widget_set_layout(slot, VGUI_LAYOUT_COLUMN);
		vgui_widget_set_gap(slot, 4.0f);
		vgui_widget_set_background(slot, 0);
		vgui_widget* key = vgui_widget_add(slot, VGUI_BUTTON, "Q");
		vgui_widget_set_size(key, 56.0f, 36.0f);
		vgui_widget* cd = vgui_widget_add(slot, VGUI_PROGRESS, nullptr);
		vgui_widget_set_size(cd, 56.0f, 16.0f);
		vgui_widget_set_value(cd, 0.0f);
	}

	// BENCH_WIDGETS static panels: labels + bars laid out like HUD cards.
	for (int i = 0; i < widgets; ++i)
	{
		vgui_widget* p = vgui_widget_add(root, VGUI_PANEL, nullptr);
		vgui_widget_set_pos(p, 330.0f + (i % 5) * 190.0f,
			260.0f + (i / 5) * 90.0f);
		vgui_widget_set_size(p, 180.0f, 80.0f);
		vgui_widget_set_layout(p, VGUI_LAYOUT_COLUMN);
		vgui_widget_set_padding(p, 6, 4, 6, 4);
		vgui_widget_set_gap(p, 3.0f);
		vgui_widget_set_background(p, 0x58101418);
		char text[32] = {};
		std::snprintf(text, sizeof(text), "panel %d", i);
		vgui_widget* label = vgui_widget_add(p, VGUI_LABEL, "");  // A/B: no text
		// vgui_widget_set_font_size(label, 10.0f);  // A/B: font_size trigger
		for (int b = 0; b < 2; ++b)
		{
			vgui_widget* bar = vgui_widget_add(p, VGUI_PROGRESS, nullptr);
			vgui_widget_set_size(bar, 0.0f, 8.0f);
			vgui_widget_set_text(bar, " ");
			vgui_widget_set_value(bar, 0.25f + 0.5f * (float)b);
		}
		st->panels.push_back(p);
	}

	// World-anchored nameplates.
	for (int i = 0; i < kNameplates; ++i)
	{
		char name[24] = {};
		std::snprintf(name, sizeof(name), "Unit %02d", i + 1);
		vgui_widget* plate = vgui_widget_add(root, VGUI_PANEL, nullptr);
		vgui_widget_set_size(plate, 76.0f, 26.0f);
		vgui_widget_set_layout(plate, VGUI_LAYOUT_COLUMN);
		vgui_widget_set_gap(plate, 1.0f);
		vgui_widget_set_padding(plate, 1, 1, 1, 1);
		vgui_widget_set_background(plate, 0x70101418);
		vgui_widget_set_input_enabled(plate, 0);
		vgui_widget* label = vgui_widget_add(plate, VGUI_LABEL, name);
		vgui_widget_set_font_size(label, 10.0f);
		st->plates[i] = plate;
		st->plate_hp[i] = vgui_widget_add(plate, VGUI_PROGRESS, nullptr);
		vgui_widget_set_size(st->plate_hp[i], 0.0f, 5.0f);
		vgui_widget_set_text(st->plate_hp[i], " ");
		vgui_widget_set_accent(st->plate_hp[i], 0xFFE04040);
		st->plate_mp[i] = vgui_widget_add(plate, VGUI_PROGRESS, nullptr);
		vgui_widget_set_size(st->plate_mp[i], 0.0f, 5.0f);
		vgui_widget_set_text(st->plate_mp[i], " ");
		vgui_widget_set_accent(st->plate_mp[i], 0xFF4070E0);
	}

	vgui_update(st->ui, 0.0f);
}

static void bench_frame(bench_state* st)
{
	st->time += 1.0f / 60.0f;
	// Vitals + hotbar values.
	for (int i = 0; i < kNameplates; ++i)
	{
		const float angle = st->time * 0.4f
			+ (float)i * (6.2831853f / (float)kNameplates);
		const float radius = 4.5f + 2.0f * std::sin(st->time * 0.23f + (float)i);
		st->ent_x[i] = std::cos(angle) * radius;
		st->ent_z[i] = std::sin(angle) * radius;
		st->ent_hp[i] = 0.5f + 0.45f * std::sin(st->time * 0.7f + (float)i * 0.9f);
		st->ent_mp[i] = 0.5f + 0.45f * std::sin(st->time * 0.45f + (float)i * 1.7f);
	}
}

// Projects the nameplate anchors with the same camera the HUD sample uses
// (eye 0,6.5,9 -> origin, fov 50) into vgui coordinates.
static void update_nameplates(bench_state* st, int cw, int ch)
{
	glm::mat4 proj = glm::perspectiveRH_ZO(glm::radians(50.0f),
		ch > 0 ? (float)cw / (float)ch : 1.0f, 0.1f, 100.0f)
		* glm::scale(glm::mat4(1.0f), glm::vec3(1.0f, -1.0f, 1.0f));
	glm::mat4 view = glm::lookAt(glm::vec3(0.0f, 6.5f, 9.0f), glm::vec3(0.0f),
		glm::vec3(0.0f, 1.0f, 0.0f));
	const glm::mat4 vp = proj * view;
	for (int i = 0; i < kNameplates; ++i)
	{
		glm::vec4 clip = vp * glm::vec4(st->ent_x[i], 1.35f, st->ent_z[i], 1.0f);
		const bool visible = clip.w > 0.05f
			&& std::fabs(clip.x / clip.w) <= 1.3f
			&& std::fabs(clip.y / clip.w) <= 1.3f;
		vgui_widget_set_visible(st->plates[i], visible ? 1 : 0);
		if (visible)
		{
			const float ndc_x = clip.x / clip.w;
			const float ndc_y = clip.y / clip.w;
			vgui_widget_set_pos(st->plates[i],
				(ndc_x * 0.5f + 0.5f) * (float)cw - 38.0f,
				(ndc_y * 0.5f + 0.5f) * (float)ch - 30.0f);
		}
		vgui_widget_set_value(st->plate_hp[i], st->ent_hp[i]);
		vgui_widget_set_value(st->plate_mp[i], st->ent_mp[i]);
	}
}

struct bench_seg
{
	double sum = 0.0;
	float mx = 0.0f;
	long n = 0;
	void add(float ms)
	{
		sum += ms;
		if (ms > mx) mx = ms;
		++n;
	}
	float avg() const { return n ? (float)(sum / (double)n) : 0.0f; }
	float max() const { return mx; }
};

#include <exception>
static void bench_terminate_handler()
{
	std::fprintf(stderr, "TERMINATE CALLED (uncaught exception)\n");
	std::abort();
}

int main()
{
	long bench_frames = 600;
	long bench_widgets = 0;
	int bench_cache = 0;
	if (const char* e = std::getenv("BENCH_FRAMES"); e && *e)
		bench_frames = std::atol(e);
	if (const char* e = std::getenv("BENCH_WIDGETS"); e && *e)
		bench_widgets = std::atol(e);
	if (const char* e = std::getenv("BENCH_CACHE"); e && *e)
		bench_cache = std::atoi(e);

	if (!SDL_Init(SDL_INIT_VIDEO))
	{
		std::fprintf(stderr, "bench: SDL_Init failed\n");
		return 2;
	}
	SDL_GPUDevice* device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV,
		true, "vulkan");
	if (!device)
	{
		std::fprintf(stderr, "bench: SDL_CreateGPUDevice failed\n");
		return 2;
	}
	vgpu_device_t* vgdev = vgpu_create_device(device);
	vgpu_ctx_t* vgctx = vgpu_create_context(vgdev,
		SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
		SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT,
		SDL_GPU_SAMPLECOUNT_1);
	vg_fbo_t fbo = vgpu_create_fbo(vgctx, kWidth, kHeight, nullptr);
	if (!fbo.ctx)
	{
		std::fprintf(stderr, "bench: vgpu_create_fbo failed\n");
		return 2;
	}

	font_cache_cx* font_cache = new_font_cache();
	const char* font_paths[] = {		"res/fonts/DejaVuSans.ttf",
		VGUI_SOURCE_DIR "/sample/res/fonts/DejaVuSans.ttf",
	};
	for (const char* path : font_paths)
		if (font_cache->load_font_from_path(path, nullptr))
			break;
	font_familys_t* familys = new_font_family(font_cache, "DejaVu Sans", nullptr);

	ovg_ctx_cb* cb = new_ctx_cb();
	rvg_t* vg = cb->new_rvg(cb->ac);
	vg->width = kWidth;
	vg->height = kHeight;

	bench_state st;
	st.font = familys;
	build_bench(&st, familys, (int)bench_widgets, bench_cache);

	long frames = 0;
	bench_seg update_seg, draw_seg, submit_seg;
	const long warmup = 30;

	while (frames < bench_frames)
	{
		bench_frame(&st);

		runtime_cx rtc;
		rtc.begin();
		vgui_update(st.ui, 1.0f / 60.0f);
		update_nameplates(&st, kWidth, kHeight);
		const float up = (float)rtc.elapsed_micro() * 0.001f;

		rtc.begin();
		cb->clear(vg);
		cb->reset_clip(vg, 1);
		vgui_draw(st.ui, cb, vg);
		const float dr = (float)rtc.elapsed_micro() * 0.001f;

		rtc.begin();
		ovg_draw_data_t dd = get_draw_list(vg);
		dd.fbo = &fbo;
		vgpu_render_frame(vgctx, &dd, 1);
		const float su = (float)rtc.elapsed_micro() * 0.001f;

		++frames;
		++st.frames;
		if (frames > warmup)
		{
			update_seg.add(up);
			draw_seg.add(dr);
			submit_seg.add(su);
		}
	}

	vgpu_wait_idle(vgctx);
	std::printf("bench: frames=%ld widgets=%ld nameplates=%d cache=%d\n",
		frames, bench_widgets, kNameplates, bench_cache);
	std::printf("  update avg %.3f ms / max %.3f ms\n",
		update_seg.avg(), update_seg.max());
	std::printf("  draw   avg %.3f ms / max %.3f ms\n",
		draw_seg.avg(), draw_seg.max());
	std::printf("  submit avg %.3f ms / max %.3f ms\n",
		submit_seg.avg(), submit_seg.max());

	vgui_destroy(st.ui);
	delete_font_family(familys);
	free_font_cache(font_cache);
	cb->destroy_rvg(vg);
	free_ctx_cb(cb);
	vgpu_destroy_fbo(&fbo);
	vgpu_destroy_context(vgctx);
	vgpu_destroy_device(vgdev);
	SDL_DestroyGPUDevice(device);
	SDL_Quit();
	return 0;
}
