// sample/vgui_phase0.cpp: Phase 0 3D viewport foundation.
//
// Demonstrates the Phase 0 building blocks:
//   * a native SDL_GPUTexture produced by an offscreen 3D pass
//   * the texture sampled by a focusable VGUI_IMAGE
//   * VGUI_LAYOUT_WRAP for a small toolbar
//
// Frame order is explicit:
//   vgui_update -> layout -> resize 3D target -> submit 3D -> submit UI
//
// Run with --smoke to render a few frames and exit with a status code.

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "pch.h"
#include <SDL3/SDL.h>
#include <vgui_sdl3.h>
#include <vgui/vgui.h>

#include "phase0_renderer.h"

struct phase0_state
{
	vgui_context* ui = nullptr;
	vgui_widget* main_panel = nullptr;
	vgui_widget* viewport = nullptr;
	vgui_widget* status = nullptr;
	vgui_texture* native_texture = nullptr;
	phase0_renderer* renderer = nullptr;
	float time = 0.0f;
	int frames = 0;
};

static void build_ui(phase0_state* st, font_familys_t* font, int width, int height)
{
	st->ui = vgui_create(font, 16.0f);
	vgui_context_set_size(st->ui, width, height);

	vgui_widget* root = vgui_root(st->ui);

	st->main_panel = vgui_widget_add(root, VGUI_PANEL, nullptr);
	vgui_widget_set_id(st->main_panel, "phase0.main");
	vgui_widget_set_pos(st->main_panel, 0.0f, 0.0f);
	vgui_widget_set_size(st->main_panel, (float)width, (float)height);
	vgui_widget_set_layout(st->main_panel, VGUI_LAYOUT_COLUMN);
	vgui_widget_set_padding(st->main_panel, 16, 14, 16, 14);
	vgui_widget_set_gap(st->main_panel, 10.0f);
	vgui_widget_set_align_items(st->main_panel, VGUI_ALIGN_STRETCH);

	vgui_widget* title = vgui_widget_add(st->main_panel, VGUI_LABEL,
		"Phase 0  \xC2\xB7  3D viewport foundation");
	vgui_widget_set_id(title, "phase0.title");

	vgui_widget* hint = vgui_widget_add(st->main_panel, VGUI_LABEL,
		"SDL_GPU FBO is sampled by VGUI_IMAGE; wrap layout uses OVG flex.");
	vgui_widget_set_id(hint, "phase0.hint");

	st->viewport = vgui_widget_add(st->main_panel, VGUI_IMAGE, nullptr);
	vgui_widget_set_id(st->viewport, "phase0.viewport");
	vgui_widget_set_size(st->viewport, 0.0f, 0.0f);
	vgui_widget_set_min_size(st->viewport, 160.0f, 200.0f);
	vgui_widget_set_grow(st->viewport, 1.0f);
	vgui_widget_set_align_self(st->viewport, VGUI_ALIGN_STRETCH);
	vgui_widget_set_border(st->viewport, 0xFF3C3C3C, 1.0f);
	vgui_image_set_fit(st->viewport, VGUI_IMAGE_FIT_STRETCH);
	vgui_image_set_pixel_snap(st->viewport, 1);

	// A small wrap strip exercises VGUI_LAYOUT_WRAP in a real window.
	vgui_widget* tools = vgui_widget_add(st->main_panel, VGUI_PANEL, nullptr);
	vgui_widget_set_id(tools, "phase0.tools");
	vgui_widget_set_layout(tools, VGUI_LAYOUT_WRAP);
	vgui_widget_set_background(tools, 0);
	vgui_widget_set_border(tools, 0, 0);
	vgui_widget_set_padding(tools, 0, 0, 0, 0);
	vgui_widget_set_gap(tools, 6.0f);
	vgui_widget_set_size(tools, 0.0f, 66.0f);
	vgui_widget_set_align_content(tools, VGUI_ALIGN_CONTENT_CENTER);

	// CPU texture path: a tiny checker is uploaded lazily and sampled like any
	// other VGUI_IMAGE.  Grayscale pixels are endian-neutral.
	std::vector<uint32_t> checker(8 * 8);
	for (int y = 0; y < 8; ++y)
		for (int x = 0; x < 8; ++x)
			checker[y * 8 + x] = ((x ^ y) & 1) ? 0xFFE0E0E0u : 0xFF202020u;
	vgui_texture_desc checker_desc = {};
	checker_desc.width = 8;
	checker_desc.height = 8;
	checker_desc.format = VGUI_TEXTURE_FORMAT_RGBA8;
	checker_desc.pixels = checker.data();
	vgui_texture* checker_texture = vgui_texture_create(st->ui, &checker_desc);
	vgui_widget* checker_image = vgui_widget_add(tools, VGUI_IMAGE, nullptr);
	vgui_widget_set_id(checker_image, "phase0.checker");
	vgui_widget_set_size(checker_image, 28.0f, 28.0f);
	vgui_widget_set_border(checker_image, 0, 0);
	vgui_image_set_fit(checker_image, VGUI_IMAGE_FIT_STRETCH);
	vgui_image_set_pixel_snap(checker_image, 1);
	vgui_image_set_texture(checker_image, checker_texture);
	vgui_texture_destroy(checker_texture);

	const char* names[] = { "Select", "Move", "Rotate", "Scale", "Frame" };
	for (const char* name : names)
	{
		vgui_widget* button = vgui_widget_add(tools, VGUI_BUTTON, name);
		vgui_widget_set_size(button, 96.0f, 28.0f);
	}

	st->status = vgui_widget_add(st->main_panel, VGUI_LABEL, "Booting renderer...");
	vgui_widget_set_id(st->status, "phase0.status");
}

static void update_layout_size(phase0_state* st, int width, int height)
{
	if (!st || !st->main_panel) return;
	vgui_context_set_size(st->ui, width, height);
	vgui_widget_set_size(st->main_panel, (float)width, (float)height);
}

int main(int argc, char** argv)
{
	bool smoke = false;
	for (int i = 1; i < argc; ++i)
	{
		if (std::strcmp(argv[i], "--smoke") == 0)
			smoke = true;
	}

	auto* wg = new app_mgr();
	font_familys_t* familys = new_font_family(wg->font_ctx,
		"Consolas,\xE5\xBE\xAE\xE8\xBD\xAF\xE9\x9B\x85\xE9\xBB\x91", 0);
	auto* cb = new_ctx_cb();
	rvg_t* vg = cb->new_rvg(cb->ac);

	if (!wg->init_gpu(true))
	{
		std::fprintf(stderr, "phase0: init_gpu failed\n");
		return 2;
	}

	const int win_w = 1180;
	const int win_h = 820;
	os_window* form = wg->create("vgui Phase 0 - 3D viewport", win_w, win_h, 0, true);
	if (!form || !form->viewport)
	{
		std::fprintf(stderr, "phase0: window creation failed\n");
		return 2;
	}

	phase0_state st;
	build_ui(&st, familys, win_w, win_h);
	form->viewport->ui = st.ui;

	st.renderer = phase0_renderer_create(wg->device);
	if (!st.renderer)
	{
		std::fprintf(stderr, "phase0: 3D renderer creation failed\n");
		vgui_destroy(st.ui);
		form->viewport->ui = nullptr;
		delete_font_family(familys);
		cb->destroy_rvg(vg);
		free_ctx_cb(cb);
		delete wg;
		return 2;
	}

	vg_fbo_t* fbo = &form->fbo;
	FpsCounter fps;
	bool running = true;
	int last_w = win_w;
	int last_h = win_h;
	int smoke_frames = 0;

	while (running)
	{
		const float delta = fps.tick();
		if (wg->get_event() < 0)
			running = false;

		if (!vgpu_acquire_swapchain(wg->vgctx, fbo, 1, true))
			continue;

		const int cw = (int)fbo->display_size.x;
		const int ch = (int)fbo->display_size.y;
		if (cw != last_w || ch != last_h)
		{
			last_w = cw;
			last_h = ch;
			update_layout_size(&st, cw, ch);
		}
		if (fbo->upsize)
		{
			fbo->upsize = false;
			vg->width = fbo->display_size.x;
			vg->height = fbo->display_size.y;
		}

		st.time += delta;
		vgui_update(st.ui, delta);
		vgui_layout(st.ui);

		// The image rectangle is the source of truth for the 3D target size.
		float vx = 0, vy = 0, vw = 0, vh = 0;
		vgui_widget_get_rect(st.viewport, &vx, &vy, &vw, &vh);
		const int target_w = (int)std::lround(std::max(1.0f, vw));
		const int target_h = (int)std::lround(std::max(1.0f, vh));
		if (phase0_renderer_resize(st.renderer, target_w, target_h))
		{
			SDL_GPUTexture* color = phase0_renderer_color_texture(st.renderer);
			if (color)
			{
				if (!st.native_texture)
				{
					st.native_texture = vgui_sdl3_texture_create_native(st.ui, wg->vgctx,
						color, target_w, target_h, VGUI_TEXTURE_FORMAT_RGBA8);
					if (st.native_texture)
						vgui_image_set_texture(st.viewport, st.native_texture);
				}
				else
				{
					vgui_sdl3_texture_update_native(st.native_texture, wg->vgctx,
						color, target_w, target_h, VGUI_TEXTURE_FORMAT_RGBA8);
				}
			}
		}

		// 3D first: it submits its own command buffer.
		phase0_renderer_render(st.renderer, st.time);

		// UI second: the image samples the completed offscreen texture.
		cb->clear(vg);
		cb->reset_clip(vg, 1);
		vgui_draw(st.ui, cb, vg);

		draw_datas_cx drawable;
		drawable.set(fbo);
		drawable.add(vg);
		drawable.render(wg->vgctx);

		++st.frames;
		if (smoke && ++smoke_frames >= 20)
			running = false;

		if (st.frames % 20 == 0 && st.status)
		{
			char text[160] = {};
			std::snprintf(text, sizeof(text),
				"FBO %dx%d  \xC2\xB7  %s  \xC2\xB7  %d fps",
				target_w, target_h,
				st.native_texture ? "native texture bound" : "waiting for texture",
				fps.get_fps());
			vgui_widget_set_text(st.status, text);
		}
	}

	const bool ok = st.native_texture != nullptr && st.frames > 0;
	wg->wait_gpu();
	vgui_destroy(st.ui);
	form->viewport->ui = nullptr;
	phase0_renderer_destroy(st.renderer);
	delete_font_family(familys);
	cb->destroy_rvg(vg);
	free_ctx_cb(cb);
	delete wg;

	if (smoke)
	{
		std::printf("phase0 smoke: %s (%d frames)\n", ok ? "ok" : "failed", st.frames);
		return ok ? 0 : 1;
	}
	return 0;
}
