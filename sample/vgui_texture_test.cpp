// sample/vgui_texture_test.cpp
//
// Offscreen readback test for the vgui texture path:
// create -> VGUI_IMAGE draw -> source crop / tint / flip -> update -> release.
// V10 adds nine-slice, rotation and arbitrary matrix coverage for both native
// (ovg::add_texture) and CPU (generated add_geometry) textures. Also exercises
// the VGUI_BUTTON atlas with public input events and GPU readback.
//
// The image widget geometry and vgui clip state are handled by vgui; the
// sampled pixels verify that VGUI_IMAGE maps its state onto the draw paths
// correctly.  All texture sampling is linear + clamp.

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#include <glm/glm.hpp>

#include "ovg.h"
#include <vgui/richtext.h>
#include <vgui/vgui.h>
#include <vgui/backend/vgui_sdl3.h>
#include <vgui/backend/vgui_renderer_sdl3.h>
#include "gpu_test_util.h"

#include <cstdio>
#include <filesystem>
#include <memory>
#include <vector>

struct vgui_test_context
{
	SDL_GPUDevice* device = nullptr;
	vgpu_device_t* vgpu_device = nullptr;
	vgpu_ctx_t* vgpu_ctx = nullptr;
	vg_fbo_t fbo = {};
	ovg_ctx_cb* cb = nullptr;
	rvg_t* vg = nullptr;
	int failures = 0;
	int checks = 0;
	int width = 0;
	int height = 0;
	std::vector<uint32_t> pixels;
};

static void expect_pixel(vgui_test_context& t, int x, int y, uint32_t expected, const char* label)
{
	++t.checks;
	if (x < 0 || y < 0 || x >= t.width || y >= t.height ||
		t.pixels.size() < (size_t)t.width * t.height)
	{
		std::printf("FAIL %-24s out of range (%d,%d)\n", label, x, y);
		++t.failures;
		return;
	}
	const uint32_t actual = t.pixels[(size_t)y * t.width + x];
	if (actual != expected)
	{
		std::printf("FAIL %-24s at (%d,%d): got 0x%08X expected 0x%08X\n",
			label, x, y, actual, expected);
		++t.failures;
	}
}

static vgui_widget* add_image(vgui_context* ui, vgui_texture* texture,
	float x, float y, float w, float h)
{
	vgui_widget* image = vgui_widget_add(vgui_root(ui), VGUI_IMAGE, nullptr);
	vgui_widget_set_pos(image, x, y);
	vgui_widget_set_size(image, w, h);
	vgui_widget_set_background(image, 0);
	vgui_widget_set_border(image, 0, 0);
	vgui_image_set_fit(image, VGUI_IMAGE_FIT_STRETCH);
	vgui_image_set_pixel_snap(image, 1);
	vgui_image_set_texture(image, texture);
	return image;
}

static bool render_vgui(vgui_test_context& t, vgui_context* ui)
{
	t.cb->clear(t.vg);
	t.cb->reset_clip(t.vg, 1);
	vgui_draw(ui, t.cb, t.vg);

	ovg_draw_data_t data = get_draw_list(t.vg);
	data.fbo = &t.fbo;
	vgpu_render_frame(t.vgpu_ctx, &data, 1);
	vgpu_wait_idle(t.vgpu_ctx);

	return download_texture(t.device, vgpu_fbo_color_texture(&t.fbo),
		t.width, t.height, t.pixels);
}

static void test_button_atlas(vgui_test_context& t, vgui_context* ui,
	vgui_texture* native_icon, vgui_texture* native_2x2)
{
	auto expect = [&](bool ok, const char* label) {
		++t.checks;
		if (!ok)
		{
			std::printf("FAIL %s\n", label);
			++t.failures;
		}
		return ok;
	};
	auto capture = [&](const char* label) {
		if (render_vgui(t, ui)) return true;
		return expect(false, label);
	};

	constexpr int x = 216, y = 50, w = 136, h = 64;
	vgui_widget* panel = vgui_widget_add(vgui_root(ui), VGUI_PANEL, nullptr);
	vgui_widget_set_pos(panel, x - 16, y - 10);
	vgui_widget_set_size(panel, w + 32, h + 20);
	vgui_widget_set_padding(panel, 0, 0, 0, 0);
	vgui_widget_set_background(panel, 0xFF602080u);
	vgui_widget_set_border(panel, 0, 0);

	vgui_widget* button = vgui_widget_add(panel, VGUI_BUTTON, "H");
	vgui_widget_set_pos(button, 16, 10);
	vgui_widget_set_size(button, w, h);
	vgui_widget_set_background(button, 0xFF804020u);
	vgui_widget_set_border(button, 0, 0);
	vgui_widget_set_padding(button, 44, 0, 0, 0);
	vgui_widget_set_text_align(button, 0.0f, 0.5f);
	vgui_widget_set_text_color(button, 0xFFFFFFFFu);
	vgui_widget_set_font_size(button, 24.0f);
	vgui_widget_set_focus_alpha(button, 1.0f);
	vgui_button_set_icon(button, native_icon, 12, 12, VGUI_BUTTON_ICON_LEFT);

	// Four 16x12 cells with clear strips on the left. Sample well inside
	// the solid/clear areas so linear filtering at texel boundaries is irrelevant.
	const uint32_t colors[] = { rgba(255, 0, 0), rgba(0, 255, 0),
		rgba(0, 0, 255), rgba(255, 255, 0) };
	uint32_t pixels[32 * 24];
	for (int py = 0; py < 24; ++py)
		for (int px = 0; px < 32; ++px)
			pixels[py * 32 + px] = px % 16 < 4 ? 0 : colors[(py / 12) * 2 + px / 16];

	using texture_ptr = std::unique_ptr<vgui_texture, decltype(&vgui_texture_destroy)>;
	auto make_texture = [](vgui_context* ctx, int width, int height, const uint32_t* data) {
		const vgui_texture_desc desc{width, height, 0, VGUI_TEXTURE_FORMAT_RGBA8, data};
		return texture_ptr(vgui_texture_create(ctx, &desc), vgui_texture_destroy);
	};
	std::unique_ptr<vgui_context, decltype(&vgui_destroy)> foreign_ui(vgui_create(nullptr, 16.0f), vgui_destroy);
	const uint32_t wrong_pixels[] = { 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
		0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu };
	auto atlas = make_texture(ui, 32, 24, pixels);
	auto odd_width = make_texture(ui, 3, 2, wrong_pixels);
	auto odd_height = make_texture(ui, 2, 3, wrong_pixels);
	auto foreign = make_texture(foreign_ui.get(), 2, 2, wrong_pixels);
	if (!expect(atlas && odd_width && odd_height && foreign, "button atlas texture setup")) return;

	// Reuse the checked-in screenshot font rather than relying on system fonts.
	std::unique_ptr<font_cache_cx, decltype(&free_font_cache)> font_cache(new_font_cache(), free_font_cache);
	const auto font_path = std::filesystem::path(__FILE__).parent_path() / "res/fonts/DejaVuSans.ttf";
	if (!expect(font_cache && load_font_from_path(font_cache.get(), font_path.string().c_str(), "AtlasTest"),
		"button atlas font setup")) return;
	std::unique_ptr<font_familys_t, decltype(&delete_font_family)> font(
		new_font_family(font_cache.get(), "AtlasTest", nullptr), delete_font_family);
	if (!expect(font && font->count > 0, "button atlas font resolve")) return;
	vgui_context_set_font(ui, font.get(), 16.0f);

	expect(vgui_button_set_atlas(nullptr, atlas.get()) == 0, "button atlas rejects null widget");
	expect(vgui_button_set_atlas(panel, atlas.get()) == 0, "button atlas rejects panel");
	expect(vgui_button_set_atlas(button, atlas.get()) == 1, "button atlas binds CPU texture");
	atlas.reset();  // The button must keep the binding alive without the caller's reference.
	vgui_layout(ui);

	auto frame = [&](uint32_t color, const char* label) {
		if (!capture(label)) return false;
		expect_pixel(t, x + w - 24, y + 12, color, label);
		return true;
	};
	auto contains = [&](int x0, int y0, int x1, int y1, auto matches) {
		for (int py = y0; py < y1; ++py)
			for (int px = x0; px < x1; ++px)
				if (matches(t.pixels[(size_t)py * t.width + px])) return true;
		return false;
	};
	if (frame(colors[0], "button atlas normal"))
	{
		expect_pixel(t, x + 43, y + h - 12, colors[0], "button atlas stretched");
		expect_pixel(t, x + 12, y + h / 2, rgba(96, 32, 128), "button atlas replaces fill");
		// The native icon fixture has a green center, distinct from the red frame.
		expect_pixel(t, x + 50, y + h / 2, rgba(0, 255, 0), "button atlas icon overlay");
		expect(contains(x + 60, y + 16, x + 100, y + 48, [](uint32_t p) {
			return (p & 255u) > 224 && ((p >> 8) & 255u) > 224 && ((p >> 16) & 255u) > 224;
		}), "button atlas text overlay");
	}

	for (vgui_texture* invalid : { odd_width.get(), odd_height.get(), foreign.get() })
	{
		const char* label = invalid == odd_width.get() ? "button atlas rejects odd width"
			: invalid == odd_height.get() ? "button atlas rejects odd height" : "button atlas rejects foreign texture";
		expect(vgui_button_set_atlas(button, invalid) == 0, label);
		frame(colors[0], label);  // The failed setter must leave the sampled binding unchanged.
	}

	const int mx = x + w / 2, my = y + h / 2;
	auto focus_visible = [&] {
		return contains(x + 50, y, x + w - 12, y + 6,
			[](uint32_t p) { return ((p >> 16) & 255u) > 100; });
	};
	vgui_input_mouse_move(ui, mx, my);
	if (frame(colors[1], "button atlas hovered"))
		expect(!focus_visible(), "button atlas unfocused edge");
	vgui_input_mouse_down(ui, SDL_BUTTON_LEFT, mx, my, 1);
	frame(colors[2], "button atlas pressed over hovered");
	vgui_input_mouse_up(ui, SDL_BUTTON_LEFT, mx, my, 1);
	if (frame(colors[1], "button atlas release inside"))
		expect(focus_visible(), "button atlas focus overlay");

	vgui_input_mouse_down(ui, SDL_BUTTON_LEFT, mx, my, 1);
	vgui_widget_set_enabled(button, 0);
	frame(colors[3], "button atlas disabled during press");
	vgui_input_mouse_up(ui, SDL_BUTTON_LEFT, mx, my, 1);
	vgui_widget_set_enabled(button, 1);
	vgui_input_mouse_move(ui, mx, my);
	vgui_widget_set_enabled(panel, 0);
	frame(colors[3], "button atlas disabled by parent");
	vgui_widget_set_enabled(panel, 1);

	vgui_input_mouse_move(ui, mx, my);
	vgui_input_mouse_down(ui, SDL_BUTTON_LEFT, mx, my, 1);
	vgui_input_mouse_move(ui, x - 6, my);
	vgui_input_mouse_up(ui, SDL_BUTTON_LEFT, x - 6, my, 1);
	frame(colors[0], "button atlas release outside");

	expect(vgui_button_set_atlas(button, nullptr) == 1, "button atlas clears");
	frame(rgba(128, 64, 32), "button atlas restores default fill");
	expect(vgui_button_set_atlas(button, native_2x2) == 1, "button atlas binds minimum native 2x2");
	frame(rgba(255, 128, 0), "button atlas native 2x2");
	vgui_widget_remove(panel);
	vgui_context_set_font(ui, nullptr, 16.0f);
}

int main()
{
	if (!SDL_Init(SDL_INIT_VIDEO))
	{
		std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
		return 2;
	}
	SDL_GPUDevice* device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, true, "vulkan");
	if (!device)
	{
		std::fprintf(stderr, "SDL_CreateGPUDevice failed: %s\n", SDL_GetError());
		SDL_Quit();
		return 2;
	}

	vgui_test_context t;
	t.device = device;
	t.width = 512;
	t.height = 256;
	t.vgpu_device = vgpu_create_device(device);
	t.vgpu_ctx = vgpu_create_context(t.vgpu_device,
		SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
		SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT,
		SDL_GPU_SAMPLECOUNT_8);
	if (!t.vgpu_ctx)
	{
		std::fprintf(stderr, "vgpu_create_context failed\n");
		return 2;
	}
	t.fbo = vgpu_create_fbo(t.vgpu_ctx, t.width, t.height, nullptr);
	t.cb = new_ctx_cb();
	t.vg = t.cb->new_rvg(t.cb->ac);
	t.vg->width = t.width;
	t.vg->height = t.height;

	const uint32_t red = rgba(255, 0, 0);
	const uint32_t green = rgba(0, 255, 0);
	const uint32_t blue = rgba(0, 0, 255);
	const uint32_t yellow = rgba(255, 255, 0);
	const uint32_t orange = rgba(255, 128, 0);
	const uint32_t clear = rgba(0, 0, 0, 0);

	const uint32_t quad[4] = { red, green, blue, yellow };
	SDL_GPUTexture* src_quad = create_texture(device, 2, 2, quad);
	if (!src_quad)
	{
		std::fprintf(stderr, "create_texture(quad) failed: %s\n", SDL_GetError());
		return 2;
	}

	// 16x16 quadrants leave enough margin for bilinear sampling after rotation.
	uint32_t quad16[16 * 16];
	for (int y = 0; y < 16; ++y)
		for (int x = 0; x < 16; ++x)
			quad16[y * 16 + x] = x < 8 ? (y < 8 ? red : blue)
				: (y < 8 ? green : yellow);

	// 8x8 frame: 2px red border, green center, blue top-left corner marker.
	uint32_t frame[8 * 8];
	for (int y = 0; y < 8; ++y)
		for (int x = 0; x < 8; ++x)
		{
			uint32_t c = (x < 2 || x >= 6 || y < 2 || y >= 6) ? red : green;
			if (x < 2 && y < 2)
				c = blue;
			frame[y * 8 + x] = c;
		}

	vgui_context* ui = vgui_create(nullptr, 16.0f);
	vgui_context_set_size(ui, t.width, t.height);
	vgui_texture* native = vgui_sdl3_texture_create_native(ui, t.vgpu_ctx,
		src_quad, 2, 2, VGUI_TEXTURE_FORMAT_RGBA8);
	if (!native)
	{
		std::fprintf(stderr, "vgui_sdl3_texture_create_native failed\n");
		return 2;
	}

	SDL_GPUTexture* src_quad16 = create_texture(device, 16, 16, quad16);
	SDL_GPUTexture* src_frame = create_texture(device, 8, 8, frame);
	if (!src_quad16 || !src_frame)
	{
		std::fprintf(stderr, "create_texture(transform) failed: %s\n", SDL_GetError());
		return 2;
	}
	vgui_texture* native_quad16 = vgui_sdl3_texture_create_native(ui, t.vgpu_ctx,
		src_quad16, 16, 16, VGUI_TEXTURE_FORMAT_RGBA8);
	vgui_texture* native_frame = vgui_sdl3_texture_create_native(ui, t.vgpu_ctx,
		src_frame, 8, 8, VGUI_TEXTURE_FORMAT_RGBA8);
	if (!native_quad16 || !native_frame)
	{
		std::fprintf(stderr, "vgui_sdl3_texture_create_native(transform) failed\n");
		return 2;
	}

	vgui_texture_desc cpu_quad_desc = {};
	cpu_quad_desc.width = 16;
	cpu_quad_desc.height = 16;
	cpu_quad_desc.stride = 16 * 4;
	cpu_quad_desc.format = VGUI_TEXTURE_FORMAT_RGBA8;
	cpu_quad_desc.pixels = quad16;
	vgui_texture* cpu_quad = vgui_texture_create(ui, &cpu_quad_desc);

	vgui_texture_desc cpu_frame_desc = {};
	cpu_frame_desc.width = 8;
	cpu_frame_desc.height = 8;
	cpu_frame_desc.stride = 8 * 4;
	cpu_frame_desc.format = VGUI_TEXTURE_FORMAT_RGBA8;
	cpu_frame_desc.pixels = frame;
	vgui_texture* cpu_frame = vgui_texture_create(ui, &cpu_frame_desc);

	vgui_texture_desc cpu_empty_desc = {};
	cpu_empty_desc.width = 32;
	cpu_empty_desc.height = 32;
	cpu_empty_desc.format = VGUI_TEXTURE_FORMAT_RGBA8;
	vgui_texture* cpu_empty = vgui_texture_create(ui, &cpu_empty_desc);
	if (!cpu_quad || !cpu_frame || !cpu_empty)
	{
		std::fprintf(stderr, "vgui_texture_create(cpu transform) failed\n");
		return 2;
	}

	vgui_widget* dir = add_image(ui, native, 0, 0, 32, 32);
	(void)dir;
	vgui_widget* crop = add_image(ui, native, 32, 0, 16, 32);
	vgui_image_set_source(crop, 1, 0, 1, 2);
	vgui_widget* tinted = add_image(ui, native, 48, 0, 32, 32);
	// vgui_color is public 0xAARRGGBB; the draw path converts it to OVG's
	// channel order.  The texture pixels use memory RGBA order, so `red` below
	// is the texture's red texel, not the same literal as the public tint.
	vgui_image_set_tint(tinted, 0xFFFF0000u);
	vgui_widget* flip_h = add_image(ui, native, 0, 32, 32, 32);
	vgui_image_set_flip(flip_h, 1, 0);
	vgui_widget* flip_v = add_image(ui, native, 0, 64, 32, 32);
	vgui_image_set_flip(flip_v, 0, 1);
	add_image(ui, native, 192, 0, 16, 16);

	// V10: rotation (native + CPU), flip->rotation order, arbitrary matrices,
	// nine-slice, source+rotation and nine-slice+rotation.
	vgui_widget* rotate = add_image(ui, native_quad16, 0, 128, 64, 64);
	vgui_image_set_rotation(rotate, 90.0f);

	vgui_widget* flip_rotate = add_image(ui, native_quad16, 64, 128, 64, 64);
	vgui_image_set_rotation(flip_rotate, 90.0f);
	vgui_image_set_flip(flip_rotate, 1, 0);

	const float translate6[6] = { 1.0f, 0.0f, 0.0f, 1.0f, 16.0f, 0.0f };
	vgui_widget* matrix_move = add_image(ui, native_quad16, 128, 128, 64, 64);
	vgui_image_set_matrix(matrix_move, translate6);

	const float scale6[6] = { 0.5f, 0.0f, 0.0f, 0.5f, 0.0f, 0.0f };
	vgui_widget* matrix_scale = add_image(ui, native_quad16, 192, 128, 64, 64);
	vgui_image_set_matrix(matrix_scale, scale6);

	vgui_widget* slice = add_image(ui, native_frame, 256, 128, 32, 32);
	vgui_image_set_nine_slice(slice, 2, 2, 2, 2);

	vgui_widget* cpu_rotate = add_image(ui, cpu_quad, 288, 128, 64, 64);
	vgui_image_set_rotation(cpu_rotate, 90.0f);

	vgui_widget* cpu_slice = add_image(ui, cpu_frame, 352, 128, 32, 32);
	vgui_image_set_nine_slice(cpu_slice, 2, 2, 2, 2);

	vgui_widget* source_rotate = add_image(ui, native_quad16, 384, 128, 64, 64);
	vgui_image_set_source(source_rotate, 0, 0, 1, 16);
	vgui_image_set_rotation(source_rotate, 90.0f);

	vgui_widget* slice_rotate = add_image(ui, native_frame, 448, 128, 32, 32);
	vgui_image_set_nine_slice(slice_rotate, 2, 2, 2, 2);
	vgui_image_set_rotation(slice_rotate, 90.0f);

	// Custom rotation center: the 16x16 image is centered in a 64x64 widget
	// (fit NONE) and rotated 90° around its own top-right corner.  The image
	// moves into the margin above the original rect but stays inside the
	// widget clip.
	vgui_widget* center_rotate = add_image(ui, native_quad16, 0, 192, 64, 64);
	vgui_image_set_fit(center_rotate, VGUI_IMAGE_FIT_NONE);
	vgui_image_set_rotation(center_rotate, 90.0f);
	vgui_image_set_rotation_center(center_rotate, 16.0f, 0.0f, 1);

	vgui_widget* cpu_center_rotate = add_image(ui, cpu_quad, 64, 192, 64, 64);
	vgui_image_set_fit(cpu_center_rotate, VGUI_IMAGE_FIT_NONE);
	vgui_image_set_rotation(cpu_center_rotate, 90.0f);
	vgui_image_set_rotation_center(cpu_center_rotate, 16.0f, 0.0f, 1);

	// Rotation then matrix: +16 px translation around the same pivot.
	vgui_widget* rotate_matrix = add_image(ui, native_quad16, 128, 192, 64, 64);
	vgui_image_set_rotation(rotate_matrix, 90.0f);
	vgui_image_set_matrix(rotate_matrix, translate6);

	// Plain CPU texture (no transform) must also draw on its first frame, and
	// an empty CPU texture must not fall back to the white placeholder.
	add_image(ui, cpu_quad, 448, 0, 32, 32);
	add_image(ui, cpu_empty, 448, 32, 32, 32);

	// Regression: a CPU texture larger than its parent panel must be clipped
	// by the panel.  The CPU path goes through ovg::add_image, which used to
	// ignore the active clip and let thumbnails escape scroll viewports.
	vgui_widget* clip_panel = vgui_widget_add(vgui_root(ui), VGUI_PANEL, nullptr);
	vgui_widget_set_pos(clip_panel, 384, 32);
	vgui_widget_set_size(clip_panel, 48, 32);
	vgui_widget_set_padding(clip_panel, 0, 0, 0, 0);
	vgui_widget_set_background(clip_panel, 0);
	vgui_widget_set_border(clip_panel, 0, 0);
	vgui_widget* clip_cpu = vgui_widget_add(clip_panel, VGUI_IMAGE, nullptr);
	vgui_widget_set_pos(clip_cpu, 0, -16);
	vgui_widget_set_size(clip_cpu, 64, 64);
	vgui_widget_set_background(clip_cpu, 0);
	vgui_widget_set_border(clip_cpu, 0, 0);
	vgui_image_set_fit(clip_cpu, VGUI_IMAGE_FIT_STRETCH);
	vgui_image_set_pixel_snap(clip_cpu, 1);
	vgui_image_set_texture(clip_cpu, cpu_quad);

	// The 40x24 inline box contains a centered 24x24 square. The rich-text
	// rectangle cuts it on the right and the parent panel cuts it below.
	vgui_widget* inline_panel = vgui_widget_add(vgui_root(ui), VGUI_PANEL, nullptr);
	vgui_widget_set_pos(inline_panel, 225, 210);
	vgui_widget_set_size(inline_panel, 50, 20);
	vgui_widget_set_padding(inline_panel, 0, 0, 0, 0);
	vgui_widget_set_background(inline_panel, 0);
	vgui_widget_set_border(inline_panel, 0, 0);
	vgui_widget* inline_view = vgui_widget_add(inline_panel, VGUI_CUSTOM, nullptr);
	vgui_widget_set_size(inline_view, 50, 40);
	vgui_widget_set_background(inline_view, 0);
	vgui_widget_set_border(inline_view, 0, 0);
	vgui_custom_set_draw(inline_view, [](vgui_widget* widget, ovg_ctx_cb* cb, rvg_t* vg,
		float x, float y, float, float, float, void* user) {
		const vgui_richtext_image image{0, static_cast<vgui_texture*>(user), 40, 24, 0, 0};
		const vgui_richtext_content content{VGUI_RICHTEXT_IMAGE_MARKER, nullptr, 0, &image, 1};
		vgui_custom_draw_richtext(widget, cb, vg, &content, x + 8, y, 24, 40,
			VGUI_RICHTEXT_WRAP_UNICODE, 0xFFFFFFFFu, 0, 0, 16);
	}, native_quad16);

	vgui_update(ui, 0.0f);
	vgui_layout(ui);
	if (!render_vgui(t, ui))
	{
		std::fprintf(stderr, "first render/download failed\n");
		return 2;
	}

	expect_pixel(t, 4, 4, red, "direction top-left");
	expect_pixel(t, 28, 4, green, "direction top-right");
	expect_pixel(t, 4, 28, blue, "direction bottom-left");
	expect_pixel(t, 28, 28, yellow, "direction bottom-right");

	expect_pixel(t, 44, 4, green, "crop top");
	expect_pixel(t, 44, 28, yellow, "crop bottom");

	expect_pixel(t, 52, 4, red, "tint red texel");
	expect_pixel(t, 76, 4, rgba(0, 0, 0), "tint green texel");

	expect_pixel(t, 4, 36, green, "flip h left");
	expect_pixel(t, 28, 36, red, "flip h right");
	expect_pixel(t, 4, 68, blue, "flip v top");
	expect_pixel(t, 4, 92, red, "flip v bottom");

	// Plain CPU texture on its first frame.
	expect_pixel(t, 452, 4, red, "cpu first frame tl");
	expect_pixel(t, 476, 4, green, "cpu first frame tr");
	expect_pixel(t, 452, 28, blue, "cpu first frame bl");
	expect_pixel(t, 476, 28, yellow, "cpu first frame br");
	expect_pixel(t, 460, 48, clear, "cpu empty skipped");

	// CPU texture clipped by the 48x32 parent panel.
	expect_pixel(t, 388, 36, red, "cpu parent clip tl");
	expect_pixel(t, 424, 36, green, "cpu parent clip tr");
	expect_pixel(t, 388, 52, blue, "cpu parent clip bl");
	expect_pixel(t, 424, 52, yellow, "cpu parent clip br");
	expect_pixel(t, 436, 36, clear, "cpu parent clip right");
	expect_pixel(t, 388, 28, clear, "cpu parent clip above");
	expect_pixel(t, 388, 68, clear, "cpu parent clip below");

	// Rotation: source TL/TR/BL/BR -> screen BL/TL/BR/TR (90° clockwise).
	expect_pixel(t, 16, 144, blue, "rotation top-left");
	expect_pixel(t, 48, 144, red, "rotation top-right");
	expect_pixel(t, 16, 176, yellow, "rotation bottom-left");
	expect_pixel(t, 48, 176, green, "rotation bottom-right");

	// Flip runs before rotation: flip_x then 90° produces TL Y, TR G, BL B, BR R.
	expect_pixel(t, 80, 144, yellow, "flip+rotation top-left");
	expect_pixel(t, 112, 144, green, "flip+rotation top-right");
	expect_pixel(t, 80, 176, blue, "flip+rotation bottom-left");
	expect_pixel(t, 112, 176, red, "flip+rotation bottom-right");

	// Matrix translation +16 px around the destination center; the widget clip
	// cuts the shifted image on the right.
	expect_pixel(t, 136, 144, clear, "matrix move empty left");
	expect_pixel(t, 152, 144, red, "matrix move top-left");
	expect_pixel(t, 184, 144, green, "matrix move top-right");
	expect_pixel(t, 152, 176, blue, "matrix move bottom-left");
	expect_pixel(t, 184, 176, yellow, "matrix move bottom-right");
	expect_pixel(t, 196, 144, clear, "matrix move clipped");

	// Matrix scale 0.5 around the center: 64x64 image shrinks to 32x32.
	expect_pixel(t, 200, 136, clear, "matrix scale empty");
	expect_pixel(t, 212, 148, red, "matrix scale top-left");
	expect_pixel(t, 236, 148, green, "matrix scale top-right");
	expect_pixel(t, 212, 172, blue, "matrix scale bottom-left");
	expect_pixel(t, 236, 172, yellow, "matrix scale bottom-right");

	// Native nine-slice: 2px source border stays 2px at the destination.
	expect_pixel(t, 257, 129, blue, "9-slice corner");
	expect_pixel(t, 262, 134, green, "9-slice center");
	expect_pixel(t, 257, 144, red, "9-slice left edge");
	expect_pixel(t, 272, 129, red, "9-slice top edge");
	expect_pixel(t, 286, 158, red, "9-slice corner br");
	expect_pixel(t, 272, 144, green, "9-slice center 2");

	// CPU texture rotation uses the generated geometry path.
	expect_pixel(t, 304, 144, blue, "cpu rotation top-left");
	expect_pixel(t, 336, 144, red, "cpu rotation top-right");
	expect_pixel(t, 304, 176, yellow, "cpu rotation bottom-left");
	expect_pixel(t, 336, 176, green, "cpu rotation bottom-right");

	// CPU texture nine-slice.
	expect_pixel(t, 353, 129, blue, "cpu 9-slice corner");
	expect_pixel(t, 358, 134, green, "cpu 9-slice center");
	expect_pixel(t, 353, 144, red, "cpu 9-slice left edge");
	expect_pixel(t, 368, 129, red, "cpu 9-slice top edge");
	expect_pixel(t, 382, 158, red, "cpu 9-slice corner br");

	// Source crop + rotation: left texture column (red top, blue bottom) after
	// 90° clockwise gives blue on the left and red on the right.
	expect_pixel(t, 392, 160, blue, "source+rotation left");
	expect_pixel(t, 440, 160, red, "source+rotation right");

	// Nine-slice + rotation: the blue TL corner moves to the top-right.
	expect_pixel(t, 449, 129, red, "9-slice+rotation tl");
	expect_pixel(t, 478, 129, blue, "9-slice+rotation tr");
	expect_pixel(t, 464, 144, green, "9-slice+rotation center");
	expect_pixel(t, 449, 158, red, "9-slice+rotation bl");

	// Custom rotation center at the image top-right: the rotated image sits
	// 16 px above the original position; TL blue, TR red, BL yellow, BR green.
	expect_pixel(t, 28, 204, blue, "center rotation tl");
	expect_pixel(t, 36, 204, red, "center rotation tr");
	expect_pixel(t, 28, 212, yellow, "center rotation bl");
	expect_pixel(t, 36, 212, green, "center rotation br");

	// CPU custom rotation center.
	expect_pixel(t, 92, 204, blue, "cpu center rotation tl");
	expect_pixel(t, 100, 204, red, "cpu center rotation tr");
	expect_pixel(t, 92, 212, yellow, "cpu center rotation bl");
	expect_pixel(t, 100, 212, green, "cpu center rotation br");

	// Rotation then matrix: 90° clockwise followed by +16 px translation.
	expect_pixel(t, 136, 208, clear, "rotation+matrix empty");
	expect_pixel(t, 152, 208, blue, "rotation+matrix tl");
	expect_pixel(t, 184, 208, red, "rotation+matrix tr");
	expect_pixel(t, 152, 240, yellow, "rotation+matrix bl");
	expect_pixel(t, 184, 240, green, "rotation+matrix br");

	// The inline image's left letterbox and both clipping edges stay clear.
	expect_pixel(t, 236, 214, clear, "inline image letterbox");
	expect_pixel(t, 244, 214, red, "inline image fitted top");
	expect_pixel(t, 244, 227, blue, "inline image fitted bottom");
	expect_pixel(t, 259, 214, clear, "inline image content clip");
	expect_pixel(t, 244, 232, clear, "inline image parent clip");

	expect_pixel(t, 500, 240, clear, "untouched background");

	// In-place update: same vgui_texture handle, new SDL_GPUTexture pointer.
	const uint32_t update_pixels[4] = { orange, orange, orange, orange };
	SDL_GPUTexture* src_update = create_texture(device, 2, 2, update_pixels);
	if (!src_update)
	{
		std::fprintf(stderr, "create_texture(update) failed: %s\n", SDL_GetError());
		return 2;
	}
	if (!vgui_sdl3_texture_update_native(native, t.vgpu_ctx, src_update,
		2, 2, VGUI_TEXTURE_FORMAT_RGBA8))
	{
		std::fprintf(stderr, "vgui_sdl3_texture_update_native failed\n");
		return 2;
	}
	if (!render_vgui(t, ui))
	{
		std::fprintf(stderr, "update render/download failed\n");
		return 2;
	}
	expect_pixel(t, 196, 4, orange, "update new pixels");

	test_button_atlas(t, ui, native_frame, native);

	// Drop the creator reference first; the widgets still hold references.
	vgui_texture_destroy(native);
	vgui_texture_destroy(native_quad16);
	vgui_texture_destroy(native_frame);
	vgui_texture_destroy(cpu_quad);
	vgui_texture_destroy(cpu_frame);
	vgui_texture_destroy(cpu_empty);

	// Release through VGUI.  vgui defers the backend release until the next
	// draw; vgpu_destroy_context flushes anything still pending.
	vgui_destroy(ui);
	t.cb->destroy_rvg(t.vg);
	free_ctx_cb(t.cb);
	vgpu_destroy_fbo(&t.fbo);
	vgpu_destroy_context(t.vgpu_ctx);
	vgpu_destroy_device(t.vgpu_device);
	SDL_ReleaseGPUTexture(device, src_update);
	SDL_ReleaseGPUTexture(device, src_quad16);
	SDL_ReleaseGPUTexture(device, src_frame);
	SDL_ReleaseGPUTexture(device, src_quad);
	SDL_DestroyGPUDevice(device);
	SDL_Quit();

	if (t.failures)
	{
		std::printf("vgui texture test: %d/%d checks failed\n", t.failures, t.checks);
		return 1;
	}
	std::printf("vgui texture test: ok (%d checks)\n", t.checks);
	return 0;
}
