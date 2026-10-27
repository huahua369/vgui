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
static int g_focus_calls;
static int g_scale_calls;

static void c_focus_listener(vgui_context* ctx, vgui_widget* old_focus,
	vgui_widget* new_focus, void* user)
{
	(void)ctx;
	(void)old_focus;
	(void)new_focus;
	(void)user;
	++g_focus_calls;
}

static void c_scale_listener(vgui_context* ctx, float scale, void* user)
{
	(void)ctx;
	(void)scale;
	(void)user;
	++g_scale_calls;
}

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

	/* V10 transforms: compile/link check from plain C (no C++ types), then
	 * clear everything again. */
	{
		const float matrix6[6] = { 1.0f, 0.0f, 0.25f, 1.0f, 0.0f, 0.0f };
		vgui_image_set_nine_slice(image, 2, 2, 2, 2);
		vgui_image_set_nine_slice(image, 0, 0, 0, 0);
		vgui_image_set_rotation(image, 30.0f);
		vgui_image_set_rotation(image, 0.0f);
		vgui_image_set_rotation_center(image, 1.0f, 1.0f, 1);
		vgui_image_set_rotation_center(image, 0.0f, 0.0f, 0);
		vgui_image_set_matrix(image, matrix6);
		vgui_image_set_matrix(image, NULL);
	}

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

	/* E01: UI scale is a pure C API and setters/geometry use logical units. */
	{
		vgui_style style;
		vgui_style_default(&style);
		const float padding = style.panel.padding;
		vgui_style_scale(&style, 2.0f);
		check(style.panel.padding == padding * 2.0f,
			"C API: style scale");

		vgui_context_set_ui_scale(ui, 2.0f);
		check(vgui_context_get_ui_scale(ui) == 2.0f,
			"C API: ui scale getter");
		check(vgui_context_get_font_size(ui) == 32.0f,
			"C API: context font follows ui scale");
		vgui_layout(ui);
		vgui_widget_get_rect(image, &x, &y, &w, &h);
		check(w == 240.0f && h == 160.0f,
			"C API: live tree follows ui scale");

		int token = vgui_context_add_ui_scale_listener(ui,
			c_scale_listener, NULL);
		check(token != 0, "C API: ui scale listener token");
		vgui_context_set_ui_scale(ui, 1.25f);
		check(g_scale_calls == 1, "C API: ui scale listener fires");
		vgui_context_remove_ui_scale_listener(ui, token);
		vgui_context_set_ui_scale(ui, 1.0f);
		check(g_scale_calls == 1, "C API: ui scale listener removal");

		check(vgui_context_get_font_size(ui) == 16.0f,
			"C API: context font restores");
		vgui_layout(ui);
		vgui_widget_get_rect(image, &x, &y, &w, &h);
		check(w == 120.0f && h == 80.0f,
			"C API: ui scale restores logical geometry");
	}

	/* F04 row extensions through the plain C API. */
	{
		vgui_widget* list = vgui_widget_add(root, VGUI_LIST, NULL);
		vgui_widget* tree = vgui_widget_add(root, VGUI_TREE, NULL);
		check(list != NULL && tree != NULL, "C API: row ext widgets");

		vgui_item_id li = vgui_list_add_item(list, "row", NULL);
		vgui_item_id ti = vgui_tree_add_node(tree, 0, "node", NULL);
		check(li != 0 && ti != 0, "C API: row ext items");

		vgui_list_set_item_icon(list, li, texture);
		check(vgui_list_get_item_icon(list, li) == texture, "C API: list item icon");
		vgui_list_set_item_text_color(list, li, 0xFF00FF00u);
		check(vgui_list_get_item_text_color(list, li) == 0xFF00FF00u,
			"C API: list item text color");
		vgui_list_set_item_buttons(list, li, 2);
		check(vgui_list_get_item_buttons(list, li) == 2, "C API: list item buttons");
		vgui_list_set_item_button_icon(list, li, 1, texture);
		check(vgui_list_get_item_button_icon(list, li, 1) == texture,
			"C API: list item button icon");
		vgui_list_on_item_button(list, NULL, NULL);

		vgui_tree_set_item_icon(tree, ti, texture);
		check(vgui_tree_get_item_icon(tree, ti) == texture, "C API: tree item icon");
		vgui_tree_set_item_text_color(tree, ti, 0xFF8040FFu);
		check(vgui_tree_get_item_text_color(tree, ti) == 0xFF8040FFu,
			"C API: tree item text color");
		vgui_tree_set_item_buttons(tree, ti, 1);
		check(vgui_tree_get_item_buttons(tree, ti) == 1, "C API: tree item buttons");
		vgui_tree_on_item_button(tree, NULL, NULL);
	}

	/* V01: additional focus observers coexist with the legacy listener and the
	 * pointer-cancel entry point is callable from plain C. */
	{
		vgui_widget* focusable = vgui_widget_add(root, VGUI_BUTTON, "focus");
		vgui_widget_set_size(focusable, 80.0f, 24.0f);
		int token = vgui_context_add_focus_listener(ui, c_focus_listener, NULL);
		check(token != 0, "C API: focus listener token");
		vgui_set_focus(ui, focusable);
		check(g_focus_calls == 1, "C API: focus listener fires");
		vgui_context_remove_focus_listener(ui, token);
		vgui_input_cancel(ui);
		check(vgui_context_wants_mouse(ui) == 0, "C API: input cancel is safe");
	}

	/* The image keeps the texture alive after the caller drops its handle. */
	vgui_texture_destroy(texture);
	vgui_image_set_texture(image, NULL);
	vgui_widget_destroy(image);
	vgui_destroy(ui);

	printf("\nC API: %d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
