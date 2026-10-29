// sample/ovg_scene_json_test.cpp
//
// A08: OVGS image node resource resolution.
//
// Exercises SceneRenderer's image_ref -> vg_image_t* upload cache with a mock
// ovg_ctx_cb, so the test needs no window, GPU or OVG renderer.  Checks:
//   * TextureRes pixels are uploaded once with the expected descriptor;
//   * repeated draws reuse the same vg_image_t;
//   * image nodes with a missing/wrong/malformed resource (including a
//     stride smaller than one row) are skipped;
//   * src_rect/dst_rect/sliced/color/flip reach add_image unchanged;
//   * default source/destination rectangles use the uploaded image size;
//   * clear_image_cache destroys the caller-owned image handles.

#include "pch.h"		// glm types used by ovg_c.h
#include <ovg_c.h>

#include "ovg_scene_json.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace {

	struct mock_state
	{
		int updates = 0;
		int adds = 0;
		int destroys = 0;
		vg_image_t* last_image = nullptr;
		vg_image_desc_t last_desc = {};
		ovg_image_r last_rect = {};
	};

	mock_state* g_mock = nullptr;

	void mock_image_update(rvg_t*, vg_image_t* image, vg_image_desc_t* desc)
	{
		if (!g_mock)
			return;
		++g_mock->updates;
		g_mock->last_image = image;
		if (desc)
			g_mock->last_desc = *desc;
	}

	void mock_add_image(rvg_t*, ovg_image_r* rect)
	{
		if (!g_mock)
			return;
		++g_mock->adds;
		if (rect)
			g_mock->last_rect = *rect;
	}

	void mock_image_destroy(rvg_t*, vg_image_t* image)
	{
		if (!g_mock)
			return;
		++g_mock->destroys;
		if (g_mock->last_image == image)
			g_mock->last_image = nullptr;
	}

	int fail(const char* message)
	{
		std::fprintf(stderr, "FAIL: %s\n", message);
		return 1;
	}

	ovg::TextureRes make_texture()
	{
		ovg::TextureRes texture;
		texture.width = 2;
		texture.height = 2;
		texture.format = VG_FORMAT_RGBA8;
		texture.stride = 8;
		texture.pixels = {
			0xFF, 0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF,
			0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
		};
		return texture;
	}

} // namespace

int main()
{
	using namespace ovg;

	const ovg::TextureRes texture = make_texture();

	// First scene: defaulted src/dst rectangles derive from the image size.
	Scene defaults;
	defaults.width = 64;
	defaults.height = 64;
	defaults.background = 0;
	defaults.add_texture("logo", texture);

	ImageNode defaulted;
	defaulted.image_ref = "logo";		// '@' is optional
	defaulted.dst_rect = { 8, 9, 0, 0 };
	defaults.add(defaulted);

	// Second scene: explicit image parameters plus missing/wrong/malformed
	// resources that must all be skipped.
	Scene scene;
	scene.width = 64;
	scene.height = 64;
	scene.background = 0;
	scene.add_texture("logo", texture);

	ImageNode image;
	image.image_ref = "@logo";
	image.src_rect = { 0, 0, 2, 2 };
	image.dst_rect = { 4, 5, 20, 10 };
	image.sliced = { 1, 2, 3, 4 };
	image.color = 0x80A0C0E0u;
	image.flip = FLIP_HORIZONTAL;
	scene.add(image);

	ImageNode missing;
	missing.image_ref = "@missing";
	scene.add(missing);

	scene.add_solid("solid", 0xFF112233u);
	ImageNode solid;
	solid.image_ref = "@solid";
	scene.add(solid);

	TextureRes broken;
	broken.width = 4;
	broken.height = 4;
	broken.format = VG_FORMAT_RGBA8;
	broken.pixels.assign(8, 0);
	scene.add_texture("broken", broken);
	ImageNode broken_node;
	broken_node.image_ref = "@broken";
	scene.add(broken_node);

	// A stride smaller than width * bytes-per-pixel would make OVG read
	// across row boundaries, so it must be rejected even when the total
	// byte count looks large enough.
	TextureRes bad_stride;
	bad_stride.width = 2;
	bad_stride.height = 2;
	bad_stride.format = VG_FORMAT_RGBA8;
	bad_stride.stride = 4;
	bad_stride.pixels.assign(16, 0);
	scene.add_texture("bad_stride", bad_stride);
	ImageNode bad_stride_node;
	bad_stride_node.image_ref = "@bad_stride";
	scene.add(bad_stride_node);

	mock_state state;
	g_mock = &state;

	ovg_ctx_cb cb = {};
	cb.image_update = &mock_image_update;
	cb.add_image = &mock_add_image;
	cb.image_destroy = &mock_image_destroy;

	rvg_t vg = {};
	{
		SceneRenderer renderer(&cb);

		renderer.render(&vg, defaults);
		if (state.updates != 1)
			return fail("first render uploads one texture");
		if (state.adds != 1)
			return fail("defaulted image node draws");
		if (!state.last_image || !state.last_rect.img)
			return fail("add_image receives a resolved image");

		const vg_image_desc_t& desc = state.last_desc;
		if (desc.img != state.last_image || desc.width != 2 ||
			desc.height != 2 || desc.format != VG_FORMAT_RGBA8 ||
			desc.stride != 8 || desc.px_size != 16 || !desc.is_copy ||
			!desc.pixels)
			return fail("image_update descriptor");
		if (state.last_image->width != 2 || state.last_image->height != 2)
			return fail("image dimensions");

		const ovg_image_r& default_rect = state.last_rect;
		if (default_rect.img != state.last_image ||
			default_rect.rc.x != 0 || default_rect.rc.y != 0 ||
			default_rect.rc.z != 2 || default_rect.rc.w != 2 ||
			default_rect.dst.x != 8 || default_rect.dst.y != 9 ||
			default_rect.dst.z != 2 || default_rect.dst.w != 2)
			return fail("default source/destination rectangles");

		// Switching scenes drops the old cache (destroy #1) and uploads the
		// texture again for the new scene.
		renderer.render(&vg, scene);
		if (state.destroys != 1)
			return fail("scene switch releases the old image");
		if (state.updates != 2)
			return fail("scene switch uploads the new scene texture");
		if (state.adds != 2)
			return fail("missing/wrong/malformed resources are skipped");

		const ovg_image_r& explicit_rect = state.last_rect;
		if (explicit_rect.rc.x != 0 || explicit_rect.rc.y != 0 ||
			explicit_rect.rc.z != 2 || explicit_rect.rc.w != 2 ||
			explicit_rect.dst.x != 4 || explicit_rect.dst.y != 5 ||
			explicit_rect.dst.z != 20 || explicit_rect.dst.w != 10 ||
			explicit_rect.sliced.x != 1 || explicit_rect.sliced.y != 2 ||
			explicit_rect.sliced.z != 3 || explicit_rect.sliced.w != 4 ||
			explicit_rect.color != 0x80A0C0E0u ||
			explicit_rect.type != FLIP_HORIZONTAL)
			return fail("image node draw parameters");

		// Re-rendering must reuse the cached upload.
		renderer.render(&vg, scene);
		if (state.updates != 2)
			return fail("second render reuses the cached image");
		if (state.adds != 3)
			return fail("second render draws the valid image again");

		renderer.clear_image_cache();
		if (state.destroys != 2)
			return fail("clear_image_cache destroys the image");
		if (!renderer.get_image(&vg, "@logo"))
			return fail("image cache rebuilds after clear");
		if (state.updates != 3)
			return fail("cleared cache uploads again");
	}

	// The destructor releases the re-uploaded handle as well.
	if (state.destroys != 3)
		return fail("renderer destructor releases images");

	g_mock = nullptr;
	std::printf("ovg_scene_json_test: OK\n");
	return 0;
}
