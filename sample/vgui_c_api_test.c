/* sample/vgui_c_api_test.c: pure C consumer smoke test.
 *
 * This intentionally compiles as C, not C++, so it catches headers that
 * accidentally depend on C++ or on C-only syntax being compiled as C++.
 */
#include <stdio.h>
#include <string.h>

#include <vgui.h>

static int g_pass;
static int g_fail;

static void check(int ok, const char* what)
{
	if (ok)
	{
		++g_pass;
		printf("PASS %s\n", what);
	}
	else
	{
		++g_fail;
		printf("FAIL %s\n", what);
	}
}

int main(void)
{
	const char* version = vgui_version();
	check(version != NULL && version[0] != '\0', "C API: version");

	vgui_context* ui = vgui_create(NULL, 16.0f);
	check(ui != NULL, "C API: context create");
	if (!ui)
		return 1;
	vgui_context_set_size(ui, 320, 240);
	vgui_context_set_size(ui, 640, 480);

	vgui_widget* root = vgui_root(ui);
	check(root != NULL, "C API: root");
	check(vgui_context_get_mods(ui) == 0, "C API: default modifier state");
	vgui_input_key(ui, 'A', 1, VGUI_MOD_SHIFT | VGUI_MOD_ALT);
	check(vgui_context_get_mods(ui) == (VGUI_MOD_SHIFT | VGUI_MOD_ALT),
		"C API: modifier state");
	vgui_input_key(ui, 'A', 0, 0);
	check(vgui_context_get_mods(ui) == 0, "C API: modifier state cleared");

	unsigned int pixels[4] = { 0xFFFFFFFFu, 0xFF00FF00u, 0xFFFF0000u, 0xFF0000FFu };
	vgui_texture_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.width = 2;
	desc.height = 2;
	desc.format = VGUI_TEXTURE_FORMAT_RGBA8;
	desc.pixels = pixels;
	vgui_texture* texture = vgui_texture_create(ui, &desc);
	check(texture != NULL, "C API: texture create");
	check(vgui_texture_get_width(texture) == 2 && vgui_texture_get_height(texture) == 2,
		"C API: texture size");

	vgui_widget* image = vgui_widget_add(root, VGUI_IMAGE, NULL);
	check(image != NULL && vgui_widget_get_kind(image) == VGUI_IMAGE,
		"C API: image widget");
	vgui_widget_set_size(image, 120.0f, 80.0f);
	vgui_image_set_texture(image, texture);
	vgui_image_set_source(image, 0, 0, 2, 2);
	vgui_image_set_fit(image, VGUI_IMAGE_FIT_CONTAIN);
	vgui_image_set_tint(image, 0xFF80C0FFu);
	vgui_image_set_flip(image, 1, 0);
	vgui_image_set_pixel_snap(image, 1);

	vgui_widget* wrap = vgui_widget_add(root, VGUI_PANEL, NULL);
	vgui_widget_set_layout(wrap, VGUI_LAYOUT_WRAP);
	vgui_widget_set_size(wrap, 100.0f, 100.0f);
	vgui_widget_set_gap(wrap, 8.0f);
	vgui_widget_set_align_content(wrap, VGUI_ALIGN_CONTENT_CENTER);
	check(vgui_widget_get_align_content(wrap) == VGUI_ALIGN_CONTENT_CENTER,
		"C API: align-content round trip");
	vgui_widget* child = vgui_widget_add(wrap, VGUI_PANEL, NULL);
	vgui_widget_set_size(child, 40.0f, 20.0f);

	vgui_layout(ui);
	float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
	vgui_widget_get_rect(image, &x, &y, &w, &h);
	check(w == 120.0f && h == 80.0f, "C API: layout rect");

	/* The image keeps the texture alive after the caller drops its handle. */
	vgui_texture_destroy(texture);
	vgui_image_set_texture(image, NULL);
	vgui_widget_destroy(image);
	vgui_destroy(ui);

	printf("\nC API: %d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
