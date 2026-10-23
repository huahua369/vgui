// sample/vgui_model_test.cpp: list / tree / menu model self-test (headless)
//
// Exercises the public C API without a window or GPU:
//   * stable item ids, insertion/removal, text/detail/user data
//   * single and multiple selection, mouse and keyboard navigation
//   * tree expansion, flattening, selection and subtree removal
//   * menu items, check state, submenus, activation, keyboard navigation
//   * outside-click consumption while a menu is open
//   * theme/style migration and input-state cleanup
//
// Run the executable directly; it returns 0 when every check passes.

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "pch.h"		// glm etc. (required by ovg_c.h)
#include <ovg_c.h>		// font_familys_t
#include <vgui_sdl3.h>	// app_mgr / new_font_family
#include <vgui.h>		// public C API

static int g_fail = 0;
static int g_pass = 0;

static void check(bool ok, const char* what)
{
	if (ok)
	{
		g_pass++;
		printf("PASS %s\n", what);
	}
	else
	{
		g_fail++;
		printf("FAIL %s\n", what);
	}
}

static bool near(float a, float b, float eps = 0.01f)
{
	return std::fabs(a - b) <= eps;
}

struct cb_state
{
	int selects = 0;
	int deselects = 0;
	int activates = 0;
	vgui_item_id last_select = 0;
	vgui_item_id last_activate = 0;
};

static void on_select(vgui_widget*, vgui_item_id item, int selected, void* user)
{
	cb_state* s = (cb_state*)user;
	if (selected)
	{
		s->selects++;
		s->last_select = item;
	}
	else
	{
		s->deselects++;
	}
}

static void on_activate(vgui_widget*, vgui_item_id item, void* user)
{
	cb_state* s = (cb_state*)user;
	s->activates++;
	s->last_activate = item;
}

static void on_menu_open(vgui_widget*, void* user)
{
	((cb_state*)user)->selects++;
}

static void on_menu_close(vgui_widget*, void* user)
{
	((cb_state*)user)->deselects++;
}

static void on_button_click(vgui_widget*, const vgui_event*, void* user)
{
	(*(int*)user)++;
}

static void click_at(vgui_context* ui, float x, float y, int clicks = 1)
{
	vgui_input_mouse_move(ui, x, y);
	vgui_input_mouse_down(ui, 1, x, y, clicks);
	vgui_input_mouse_up(ui, 1, x, y, clicks);
}

struct value_cb
{
	int changes = 0;
	float last = 0.0f;
};

static void on_value_changed(vgui_widget*, float value, void* user)
{
	value_cb* s = (value_cb*)user;
	s->changes++;
	s->last = value;
}

struct tab_cb
{
	int selects = 0;
	int closes = 0;
	vgui_item_id last_select = 0;
	vgui_item_id last_close = 0;
	int remove_on_close = 1;
};

static void on_tab_select(vgui_widget*, vgui_item_id item, void* user)
{
	tab_cb* s = (tab_cb*)user;
	s->selects++;
	s->last_select = item;
}

static void on_tab_close(vgui_widget* tabs, vgui_item_id item, void* user)
{
	tab_cb* s = (tab_cb*)user;
	s->closes++;
	s->last_close = item;
	if (s->remove_on_close)
		vgui_tab_remove(tabs, item);
}

struct tab_event_cb
{
	int right_downs = 0;
	vgui_item_id item = 0;
};

static void on_tab_event(vgui_widget*, const vgui_event* event, void* user)
{
	tab_event_cb* s = (tab_event_cb*)user;
	if (event && event->type == VGUI_EVENT_MOUSE_DOWN && event->button == 3)
	{
		s->right_downs++;
		s->item = event->item;
	}
}

static void test_list(vgui_context* ui, const vgui_style& style)
{
	vgui_widget* list = vgui_widget_add(vgui_root(ui), VGUI_LIST, nullptr);
	vgui_widget_set_id(list, "model-list");
	vgui_widget_set_pos(list, 10.0f, 10.0f);
	vgui_widget_set_size(list, 240.0f, 200.0f);

	cb_state cb;
	vgui_list_on_select(list, on_select, &cb);
	vgui_list_on_activate(list, on_activate, &cb);

	vgui_item_id a = vgui_list_add_item(list, "Alpha", (void*)0x11);
	vgui_item_id b = vgui_list_add_item(list, "Beta", (void*)0x22);
	vgui_item_id c = vgui_list_add_item(list, "Gamma", (void*)0x33);
	check(a && b && c && a != b && b != c, "list: add assigns unique ids");
	check(vgui_list_get_count(list) == 3, "list: count after add");
	check(vgui_list_get_item_at(list, 1) == b, "list: item-at returns stable id");
	check(std::string(vgui_list_get_item_text(list, b)) == "Beta", "list: item text");
	check(vgui_list_get_item_user(list, b) == (void*)0x22, "list: item user data");

	vgui_list_set_item_detail(list, b, "detail");
	check(std::string(vgui_list_get_item_detail(list, b)) == "detail", "list: item detail");

	vgui_item_id bb = vgui_list_insert_item(list, 1, "Bravo", nullptr);
	check(vgui_list_get_count(list) == 4, "list: count after insert");
	check(vgui_list_get_item_at(list, 1) == bb, "list: insert position");
	check(vgui_list_get_item_at(list, 2) == b, "list: insert keeps ids stable");
	check(vgui_list_remove_item(list, bb) == 1, "list: remove inserted item");
	check(vgui_list_get_count(list) == 3, "list: count after remove");

	vgui_list_set_item_enabled(list, b, 0);
	check(vgui_list_get_item_enabled(list, b) == 0, "list: disable item");
	vgui_list_set_item_enabled(list, b, 1);
	check(vgui_list_get_item_enabled(list, b) == 1, "list: enable item");

	check(vgui_list_get_selection_mode(list) == VGUI_SELECTION_SINGLE, "list: default single selection");
	vgui_list_select(list, a, 1);
	vgui_list_select(list, b, 1);
	check(vgui_list_get_selected_count(list) == 1, "list: single selection replaces");
	check(vgui_list_is_selected(list, a) == 0 && vgui_list_is_selected(list, b) == 1,
		"list: single selection contents");

	vgui_list_set_selection_mode(list, VGUI_SELECTION_MULTIPLE);
	check(vgui_list_get_selection_mode(list) == VGUI_SELECTION_MULTIPLE, "list: set multiple selection");
	vgui_list_select(list, c, 1);
	check(vgui_list_get_selected_count(list) == 2, "list: multiple selection adds");
	vgui_list_select(list, b, 0);
	check(vgui_list_get_selected_count(list) == 1 && vgui_list_is_selected(list, c),
		"list: multiple selection removes");

	// Switch back and exercise mouse selection.  Row coordinates come from the
	// public style, so the test does not depend on font metrics.
	vgui_list_set_selection_mode(list, VGUI_SELECTION_SINGLE);
	vgui_list_clear_selection(list);
	vgui_update(ui, 0.016f);
	float lx = 0.0f, ly = 0.0f, lw = 0.0f, lh = 0.0f;
	vgui_widget_get_rect(list, &lx, &ly, &lw, &lh);
	(void)lw;
	(void)lh;
	float row_h = style.list.row_height;
	check(row_h > 0.0f, "list: test style has fixed row height");
	float row0_y = ly + style.list.padding_y + row_h * 0.5f;
	float row1_y = row0_y + row_h;
	click_at(ui, lx + 30.0f, row0_y);
	check(vgui_list_is_selected(list, a) && vgui_list_get_selected_count(list) == 1,
		"list: mouse selects first row");
	check(cb.selects >= 2, "list: select callback fired");
	check(vgui_list_get_cursor(list) == a, "list: mouse updates cursor");

	click_at(ui, lx + 30.0f, row1_y, 2);
	check(vgui_list_is_selected(list, b) && vgui_list_get_selected_count(list) == 1,
		"list: double click selects second row");
	check(cb.activates == 1 && cb.last_activate == b, "list: double click activates item");

	// Keyboard navigation.
	vgui_set_focus(ui, list);
	int handled = vgui_input_key(ui, VGUI_KEY_DOWN, 1, 0);
	vgui_input_key(ui, VGUI_KEY_DOWN, 0, 0);
	check(handled == 1, "list: key down handled");
	check(vgui_list_get_cursor(list) == c, "list: keyboard moves cursor");
	check(vgui_list_is_selected(list, c), "list: keyboard selects cursor row");

	// Insert/remove while selected and clear the model.
	vgui_list_clear(list);
	check(vgui_list_get_count(list) == 0, "list: clear removes all items");
	check(std::string(vgui_list_get_item_text(list, a)).empty(), "list: cleared id is invalid");
}

static void test_tree(vgui_context* ui, const vgui_style& style)
{
	vgui_widget* tree = vgui_widget_add(vgui_root(ui), VGUI_TREE, nullptr);
	vgui_widget_set_id(tree, "model-tree");
	vgui_widget_set_pos(tree, 300.0f, 10.0f);
	vgui_widget_set_size(tree, 240.0f, 200.0f);

	cb_state cb;
	vgui_tree_on_select(tree, on_select, &cb);
	vgui_tree_on_activate(tree, on_activate, &cb);

	vgui_item_id a = vgui_tree_add_node(tree, 0, "Root A", nullptr);
	vgui_item_id a1 = vgui_tree_add_node(tree, a, "Child A1", nullptr);
	vgui_item_id a2 = vgui_tree_add_node(tree, a, "Child A2", nullptr);
	vgui_item_id b = vgui_tree_add_node(tree, 0, "Root B", nullptr);
	check(a && a1 && a2 && b && a != a1, "tree: add assigns unique ids");
	check(vgui_tree_get_node_count(tree) == 4, "tree: node count");
	check(vgui_tree_get_root_count(tree) == 2, "tree: root count");
	check(vgui_tree_get_root(tree, 0) == a && vgui_tree_get_root(tree, 1) == b,
		"tree: root order");
	check(vgui_tree_get_child_count(tree, a) == 2, "tree: child count");
	check(vgui_tree_get_child(tree, a, 1) == a2, "tree: child order");
	check(vgui_tree_get_parent(tree, a1) == a, "tree: parent lookup");
	check(vgui_tree_get_depth(tree, a1) == 1, "tree: depth");
	check(vgui_tree_get_item_expanded(tree, a) == 0, "tree: starts collapsed");

	// Expanding reveals children; the cursor/selection works on a flattened row.
	vgui_tree_expand(tree, a);
	check(vgui_tree_get_item_expanded(tree, a) == 1, "tree: expand");
	vgui_update(ui, 0.016f);

	float tx = 0.0f, ty = 0.0f, tw = 0.0f, th = 0.0f;
	vgui_widget_get_rect(tree, &tx, &ty, &tw, &th);
	(void)tw;
	(void)th;
	float row_h = style.tree.row_height;
	check(row_h > 0.0f, "tree: test style has fixed row height");
	float arrow_x = tx + style.tree.padding_x + style.tree.arrow_size * 0.5f;
	float root_y = ty + style.tree.padding_y + row_h * 0.5f;
	float child_y = root_y + row_h;

	// Click the arrow first: this collapses without changing selection.
	click_at(ui, arrow_x, root_y);
	check(vgui_tree_get_item_expanded(tree, a) == 0, "tree: arrow click collapses");
	click_at(ui, arrow_x, root_y);
	check(vgui_tree_get_item_expanded(tree, a) == 1, "tree: arrow click expands");
	vgui_update(ui, 0.016f);

	click_at(ui, tx + style.tree.indent + 24.0f, child_y);
	check(vgui_tree_is_selected(tree, a1), "tree: mouse selects child row");
	check(vgui_tree_get_selected_count(tree) == 1, "tree: single selection");
	check(vgui_tree_get_cursor(tree) == a1, "tree: child cursor");

	vgui_tree_collapse(tree, a);
	check(vgui_tree_get_item_expanded(tree, a) == 0, "tree: collapse");
	check(vgui_tree_is_selected(tree, a1), "tree: hidden selection is retained");
	vgui_tree_expand_to(tree, a1);
	check(vgui_tree_get_item_expanded(tree, a) == 1, "tree: expand-to opens ancestors");

	vgui_tree_set_selection_mode(tree, VGUI_SELECTION_MULTIPLE);
	vgui_tree_select(tree, b, 1);
	check(vgui_tree_get_selected_count(tree) == 2, "tree: multiple selection");
	vgui_tree_set_selection_mode(tree, VGUI_SELECTION_SINGLE);

	vgui_set_focus(ui, tree);
	vgui_tree_collapse(tree, a);
	vgui_tree_set_cursor(tree, a);
	vgui_update(ui, 0.016f);
	int handled = vgui_input_key(ui, VGUI_KEY_DOWN, 1, 0);
	vgui_input_key(ui, VGUI_KEY_DOWN, 0, 0);
	check(handled == 1, "tree: key down handled");
	check(vgui_tree_get_cursor(tree) == b, "tree: keyboard skips hidden children");

	check(vgui_tree_remove_node(tree, a) == 1, "tree: remove subtree");
	check(vgui_tree_get_node_count(tree) == 1, "tree: node count after subtree removal");
	check(vgui_tree_get_root_count(tree) == 1, "tree: root count after subtree removal");
	check(std::string(vgui_tree_get_item_text(tree, a1)).empty(), "tree: descendant removed");
	check(vgui_tree_remove_node(tree, b) == 1, "tree: remove last root");
	check(vgui_tree_get_node_count(tree) == 0, "tree: empty tree");
}

static void test_menu(vgui_context* ui, const vgui_style& style)
{
	vgui_widget* menu = vgui_widget_create(ui, VGUI_MENU, nullptr);
	vgui_widget_set_id(menu, "model-menu");
	check(vgui_widget_get_parent(menu) == nullptr, "menu: not a layout child");

	vgui_widget* added_menu = vgui_widget_add(vgui_root(ui), VGUI_MENU, nullptr);
	check(added_menu && vgui_widget_get_parent(added_menu) == nullptr,
		"menu: widget_add stays detached");
	vgui_widget_destroy(added_menu);

	cb_state cb;
	vgui_menu_on_activate(menu, on_activate, &cb);
	vgui_menu_on_open(menu, on_menu_open, &cb);
	vgui_menu_on_close(menu, on_menu_close, &cb);
	vgui_item_id check_item = vgui_menu_add_check_item(menu, "Check me", 0);
	vgui_item_id normal = vgui_menu_add_item(menu, "Normal");
	vgui_item_id sep = vgui_menu_add_separator(menu);
	vgui_widget* submenu = nullptr;
	vgui_item_id sub_item = vgui_menu_add_submenu(menu, "More", &submenu);
	check(check_item && normal && sep && sub_item && submenu, "menu: add items");
	check(vgui_menu_get_count(menu) == 4, "menu: item count");
	check(vgui_menu_get_item_type(menu, check_item) == VGUI_MENU_ITEM_NORMAL, "menu: check item type");
	check(vgui_menu_get_item_type(menu, sep) == VGUI_MENU_ITEM_SEPARATOR, "menu: separator type");
	check(vgui_menu_get_item_type(menu, sub_item) == VGUI_MENU_ITEM_SUBMENU, "menu: submenu type");
	check(vgui_menu_get_submenu(menu, sub_item) == submenu, "menu: submenu lookup");
	check(vgui_menu_get_item_checkable(menu, check_item) == 1, "menu: check item checkable");
	check(vgui_menu_get_item_checked(menu, check_item) == 0, "menu: check item starts unchecked");

	vgui_menu_set_item_shortcut(menu, normal, "Ctrl+N");
	check(std::string(vgui_menu_get_item_shortcut(menu, normal)) == "Ctrl+N", "menu: shortcut text");
	vgui_menu_set_item_checked(menu, check_item, 1);
	check(vgui_menu_get_item_checked(menu, check_item) == 1, "menu: set checked");
	vgui_menu_set_item_checked(menu, check_item, 0);

	vgui_item_id sub_child = vgui_menu_add_item(submenu, "Sub action");
	check(sub_child != 0, "menu: add submenu child");
	vgui_menu_on_activate(submenu, on_activate, &cb);

	vgui_menu_open(menu, 100.0f, 100.0f);
	check(vgui_menu_is_open(menu) == 1, "menu: open");
	check(vgui_context_get_open_menu(ui) == menu, "menu: context tracks open menu");
	check(cb.selects == 1, "menu: open callback");
	check(vgui_context_wants_mouse(ui) == 1, "menu: wants mouse while open");
	check(vgui_context_wants_keyboard(ui) == 1, "menu: wants keyboard while open");
	check(vgui_context_wants_text_input(ui) == 0, "menu: does not want text input while open");

	float mx = 0.0f, my = 0.0f, mw = 0.0f, mh = 0.0f;
	vgui_widget_get_rect(menu, &mx, &my, &mw, &mh);
	check(mw >= style.menu.min_width - 0.5f, "menu: minimum width");
	check(mh > 0.0f, "menu: measured height");
	(void)mw;

	float row0 = my + style.menu.padding_y + style.menu.item_height * 0.5f;
	float row1 = row0 + style.menu.item_height;
	float row2 = row1 + style.menu.item_height;
	float row3 = row2 + style.menu.separator_height;

	// Activate the check item.  It toggles and closes the menu by default.
	click_at(ui, mx + 30.0f, row0);
	check(vgui_menu_get_item_checked(menu, check_item) == 1, "menu: click toggles check item");
	check(cb.activates == 1 && cb.last_activate == check_item, "menu: click activates item");
	check(vgui_menu_is_open(menu) == 0, "menu: click closes menu");
	check(vgui_context_get_open_menu(ui) == nullptr, "menu: close clears context");
	check(cb.deselects == 1, "menu: close callback");

	// Open the submenu by clicking its row.
	vgui_menu_open(menu, 100.0f, 100.0f);
	vgui_widget_get_rect(menu, &mx, &my, &mw, &mh);
	row0 = my + style.menu.padding_y + style.menu.item_height * 0.5f;
	row1 = row0 + style.menu.item_height;
	row2 = row1 + style.menu.item_height;
	row3 = row2 + style.menu.separator_height;
	click_at(ui, mx + 30.0f, row3);
	check(vgui_menu_is_open(menu) == 1, "menu: parent stays open for submenu");
	check(vgui_menu_is_open(submenu) == 1, "menu: click opens submenu");
	check(vgui_context_get_open_menu(ui) == menu, "menu: context keeps parent as root");

	float sx = 0.0f, sy = 0.0f, sw = 0.0f, sh = 0.0f;
	vgui_widget_get_rect(submenu, &sx, &sy, &sw, &sh);
	check(sw >= style.menu.min_width - 0.5f, "menu: submenu width");
	(void)sh;
	click_at(ui, sx + 30.0f, sy + style.menu.padding_y + style.menu.item_height * 0.5f);
	check(cb.activates == 2 && cb.last_activate == sub_child, "menu: submenu item activates");
	check(vgui_menu_is_open(submenu) == 0, "menu: submenu closes after activation");
	check(vgui_menu_is_open(menu) == 0, "menu: activation closes the whole chain");

	// Escape closes the whole chain (reopen first).
	vgui_menu_open(menu, 100.0f, 100.0f);
	vgui_input_key(ui, VGUI_KEY_ESCAPE, 1, 0);
	vgui_input_key(ui, VGUI_KEY_ESCAPE, 0, 0);
	check(vgui_menu_is_open(menu) == 0, "menu: escape closes");
	check(vgui_context_get_open_menu(ui) == nullptr, "menu: escape clears context");

	// Keyboard navigation: Down moves to the second item, Enter activates it.
	vgui_menu_open(menu, 100.0f, 100.0f);
	int handled = vgui_input_key(ui, VGUI_KEY_DOWN, 1, 0);
	vgui_input_key(ui, VGUI_KEY_DOWN, 0, 0);
	check(handled == 1, "menu: key down handled");
	handled = vgui_input_key(ui, VGUI_KEY_ENTER, 1, 0);
	vgui_input_key(ui, VGUI_KEY_ENTER, 0, 0);
	check(handled == 1, "menu: enter handled");
	check(cb.activates == 3 && cb.last_activate == normal, "menu: keyboard activates item");
	check(vgui_menu_is_open(menu) == 0, "menu: activation closes keyboard menu");

	// Open below an anchor.
	vgui_widget* anchor = vgui_widget_add(vgui_root(ui), VGUI_BUTTON, "Anchor");
	vgui_widget_set_pos(anchor, 10.0f, 300.0f);
	vgui_widget_set_size(anchor, 120.0f, 32.0f);
	vgui_update(ui, 0.016f);
	vgui_menu_open_below(menu, anchor);
	vgui_widget_get_rect(menu, &mx, &my, &mw, &mh);
	float ax = 0.0f, ay = 0.0f, aw = 0.0f, ah = 0.0f;
	vgui_widget_get_rect(anchor, &ax, &ay, &aw, &ah);
	check(near(mx, ax) && my >= ay + ah - 0.5f, "menu: open below anchor");
	vgui_menu_close(menu);
	(void)mx;
	(void)my;
	(void)mw;
	(void)mh;
	(void)aw;

	// Outside clicks are consumed: the button underneath must not fire.
	int button_clicks = 0;
	vgui_widget_on_event(anchor, on_button_click, &button_clicks);
	vgui_menu_open(menu, 100.0f, 100.0f);
	int down = vgui_input_mouse_down(ui, 1, ax + 10.0f, ay + 10.0f, 1);
	int up = vgui_input_mouse_up(ui, 1, ax + 10.0f, ay + 10.0f, 1);
	check(down == 1 && up == 1, "menu: outside press is consumed");
	check(button_clicks == 0, "menu: outside press does not reach button");
	check(vgui_menu_is_open(menu) == 0, "menu: outside press closes menu");

	// Input clear closes a menu and resets transient state.
	vgui_menu_open(menu, 100.0f, 100.0f);
	vgui_input_clear(ui);
	check(vgui_menu_is_open(menu) == 0, "menu: input clear closes");
	check(vgui_context_get_open_menu(ui) == nullptr, "menu: input clear clears context");

	vgui_menu_clear(menu);
	check(vgui_menu_get_count(menu) == 0, "menu: clear removes items");
	vgui_widget_destroy(menu);
}

static void test_texture_image(vgui_context* ui)
{
	uint32_t pixels[4] = { 0xFFFFFFFFu, 0xFF00FF00u, 0xFFFF0000u, 0xFF0000FFu };
	vgui_texture_desc desc = {};
	desc.width = 2;
	desc.height = 2;
	desc.format = VGUI_TEXTURE_FORMAT_RGBA8;
	desc.pixels = pixels;
	vgui_texture* tex = vgui_texture_create(ui, &desc);
	check(tex != nullptr, "texture: create");
	check(vgui_texture_get_width(tex) == 2 && vgui_texture_get_height(tex) == 2,
		"texture: size");

	uint8_t bgra[2 * 16] = {};
	vgui_texture_desc update = {};
	update.width = 3;
	update.height = 2;
	update.stride = 16;
	update.format = VGUI_TEXTURE_FORMAT_BGRA8;
	update.pixels = bgra;
	check(vgui_texture_update(tex, &update) == 1, "texture: update with stride");
	check(vgui_texture_get_width(tex) == 3 && vgui_texture_get_height(tex) == 2,
		"texture: updated size");

	vgui_texture_desc empty = {};
	empty.width = 4;
	empty.height = 1;
	empty.format = VGUI_TEXTURE_FORMAT_RGBA8;
	vgui_texture* empty_tex = vgui_texture_create(ui, &empty);
	check(empty_tex != nullptr && vgui_texture_get_width(empty_tex) == 4,
		"texture: empty data is valid");

	vgui_texture_desc bad = {};
	bad.width = 0;
	bad.height = 1;
	bad.format = VGUI_TEXTURE_FORMAT_RGBA8;
	check(vgui_texture_create(ui, &bad) == nullptr, "texture: rejects zero width");
	bad.width = 1;
	bad.format = (vgui_texture_format)99;
	check(vgui_texture_create(ui, &bad) == nullptr, "texture: rejects bad format");

	vgui_widget* image = vgui_widget_add(vgui_root(ui), VGUI_IMAGE, nullptr);
	check(image && vgui_widget_get_kind(image) == VGUI_IMAGE, "image: create kind");
	vgui_widget_set_size(image, 120.0f, 80.0f);
	vgui_image_set_texture(image, tex);
	vgui_image_set_source(image, 0, 0, 2, 2);
	vgui_image_set_fit(image, VGUI_IMAGE_FIT_CONTAIN);
	vgui_image_set_tint(image, 0xFF80C0FFu);
	vgui_image_set_flip(image, 1, 1);
	vgui_image_set_pixel_snap(image, 1);

	float x = 0, y = 0, w = 0, h = 0;
	vgui_widget_get_rect(image, &x, &y, &w, &h);
	check(near(w, 120.0f) && near(h, 80.0f), "image: requested size");

	// With an explicit size cleared, a textured image adopts its source size.
	vgui_widget_set_size(image, 0.0f, 0.0f);
	vgui_layout(ui);
	vgui_widget_get_rect(image, &x, &y, &w, &h);
	check(near(w, 2.0f) && near(h, 2.0f), "image: auto size uses source dimensions");

	// A texture update that changes dimensions must dirty layout for auto-size
	// images even without touching the widget.
	vgui_image_set_source(image, 0, 0, 0, 0);
	uint8_t larger[4 * 3 * 4] = {};
	vgui_texture_desc larger_desc = {};
	larger_desc.width = 4;
	larger_desc.height = 3;
	larger_desc.format = VGUI_TEXTURE_FORMAT_RGBA8;
	larger_desc.pixels = larger;
	check(vgui_texture_update(tex, &larger_desc) == 1, "texture: grow update");
	vgui_layout(ui);
	vgui_widget_get_rect(image, &x, &y, &w, &h);
	check(near(w, 4.0f) && near(h, 3.0f), "image: auto size follows texture updates");

	// The image retains the texture.  Destroying the caller's reference must
	// not invalidate the widget binding.
	vgui_texture_destroy(tex);
	vgui_image_set_texture(image, nullptr);
	vgui_texture_destroy(empty_tex);
	vgui_widget_destroy(image);
}

static vgui_widget* add_wrap_child(vgui_widget* panel, const char* id,
	float x, float y, float w, float h)
{
	vgui_widget* child = vgui_widget_add(panel, VGUI_PANEL, nullptr);
	vgui_widget_set_id(child, id);
	vgui_widget_set_pos(child, x, y);
	vgui_widget_set_size(child, w, h);
	return child;
}

static void test_wrap(vgui_context* ui)
{
	vgui_widget* panel = vgui_widget_add(vgui_root(ui), VGUI_PANEL, nullptr);
	vgui_widget_set_layout(panel, VGUI_LAYOUT_WRAP);
	vgui_widget_set_padding(panel, 0, 0, 0, 0);
	vgui_widget_set_gap(panel, 10.0f);
	vgui_widget_set_size(panel, 100.0f, 100.0f);
	vgui_widget_set_align_items(panel, VGUI_ALIGN_START);
	vgui_widget_set_align_content(panel, VGUI_ALIGN_CONTENT_START);

	vgui_widget* a = add_wrap_child(panel, "wrap-a", 0, 0, 30, 20);
	vgui_widget* b = add_wrap_child(panel, "wrap-b", 0, 0, 30, 20);
	vgui_widget* c = add_wrap_child(panel, "wrap-c", 0, 0, 30, 20);
	vgui_widget* d = add_wrap_child(panel, "wrap-d", 0, 0, 30, 20);

	vgui_layout(ui);
	float ax = 0, ay = 0, bx = 0, by = 0, cx = 0, cy = 0, dx = 0, dy = 0;
	vgui_widget_get_rect(a, &ax, &ay, nullptr, nullptr);
	vgui_widget_get_rect(b, &bx, &by, nullptr, nullptr);
	vgui_widget_get_rect(c, &cx, &cy, nullptr, nullptr);
	vgui_widget_get_rect(d, &dx, &dy, nullptr, nullptr);
	check(near(ax, 0) && near(ay, 0) && near(bx, 40) && near(by, 0),
		"wrap: first line positions");
	check(near(cx, 0) && near(cy, 30) && near(dx, 40) && near(dy, 30),
		"wrap: second line positions");

	// Three 25px items plus two 10px gaps fit exactly in 95px, so they must
	// stay on one line even though the trailing-gap approximation is used.
	vgui_widget* exact = vgui_widget_add(vgui_root(ui), VGUI_PANEL, nullptr);
	vgui_widget_set_layout(exact, VGUI_LAYOUT_WRAP);
	vgui_widget_set_padding(exact, 0, 0, 0, 0);
	vgui_widget_set_gap(exact, 10.0f);
	vgui_widget_set_size(exact, 100.0f, 40.0f);
	vgui_widget* e1 = add_wrap_child(exact, "wrap-e1", 0, 0, 25, 20);
	vgui_widget* e2 = add_wrap_child(exact, "wrap-e2", 0, 0, 25, 20);
	vgui_widget* e3 = add_wrap_child(exact, "wrap-e3", 0, 0, 25, 20);
	vgui_layout(ui);
	float e1x = 0, e1y = 0, e2x = 0, e2y = 0, e3x = 0, e3y = 0;
	vgui_widget_get_rect(e1, &e1x, &e1y, nullptr, nullptr);
	vgui_widget_get_rect(e2, &e2x, &e2y, nullptr, nullptr);
	vgui_widget_get_rect(e3, &e3x, &e3y, nullptr, nullptr);
	check(near(e1x, 0) && near(e2x, 35) && near(e3x, 70) &&
		near(e1y, 0) && near(e2y, 0) && near(e3y, 0),
		"wrap: gap-aware line breaking");
	vgui_widget_destroy(exact);

	// Center the single line horizontally and vertically.
	vgui_widget_set_size(panel, 120.0f, 100.0f);
	vgui_widget_set_justify(panel, VGUI_JUSTIFY_CENTER);
	vgui_widget_set_align_content(panel, VGUI_ALIGN_CONTENT_CENTER);
	vgui_widget_set_align_items(panel, VGUI_ALIGN_CENTER);
	vgui_widget_set_visible(c, 0);
	vgui_widget_set_visible(d, 0);
	vgui_layout(ui);
	vgui_widget_get_rect(a, &ax, &ay, nullptr, nullptr);
	vgui_widget_get_rect(b, &bx, &by, nullptr, nullptr);
	check(near(ax, 25) && near(bx, 65) && near(ay, 40) && near(by, 40),
		"wrap: center justify and align-content");

	// Grow consumes the remaining line width.
	vgui_widget_set_justify(panel, VGUI_JUSTIFY_START);
	vgui_widget_set_visible(a, 1);
	vgui_widget_set_visible(b, 1);
	vgui_widget_set_visible(c, 0);
	vgui_widget_set_visible(d, 0);
	vgui_widget_set_gap(panel, 10.0f);
	vgui_widget_set_grow(a, 1.0f);
	vgui_layout(ui);
	float aw = 0, bw = 0;
	vgui_widget_get_rect(a, nullptr, nullptr, &aw, nullptr);
	vgui_widget_get_rect(b, nullptr, nullptr, &bw, nullptr);
	check(near(aw, 80.0f) && near(bw, 30.0f), "wrap: grow distributes free width");

	// align-content stretch grows the line, and auto-height children with
	// align-items stretch fill the stretched line.
	vgui_widget_set_grow(a, 0.0f);
	vgui_widget_set_size(a, 30.0f, 0.0f);
	vgui_widget_set_size(b, 30.0f, 0.0f);
	vgui_widget_set_align_items(panel, VGUI_ALIGN_STRETCH);
	vgui_widget_set_align_content(panel, VGUI_ALIGN_CONTENT_STRETCH);
	vgui_layout(ui);
	float ah = 0, bh = 0;
	vgui_widget_get_rect(a, nullptr, nullptr, nullptr, &ah);
	vgui_widget_get_rect(b, nullptr, nullptr, nullptr, &bh);
	check(near(ah, 100.0f) && near(bh, 100.0f),
		"wrap: align-content stretch stretches auto-height children");

	vgui_widget_destroy(panel);
}

static void test_splitter(vgui_context* ui)
{
	vgui_widget* splitter = vgui_widget_add(vgui_root(ui), VGUI_SPLITTER, nullptr);
	vgui_widget_set_pos(splitter, 20.0f, 20.0f);
	vgui_widget_set_size(splitter, 400.0f, 300.0f);
	vgui_splitter_set_sash_size(splitter, 4.0f);
	vgui_splitter_set_ratio(splitter, 0.25f);

	vgui_widget* left = vgui_widget_add(splitter, VGUI_PANEL, nullptr);
	vgui_widget* right = vgui_widget_add(splitter, VGUI_PANEL, nullptr);
	value_cb cb;
	vgui_widget_on_value(splitter, on_value_changed, &cb);

	vgui_layout(ui);
	float lx = 0, ly = 0, lw = 0, lh = 0;
	float rx = 0, ry = 0, rw = 0, rh = 0;
	vgui_widget_get_rect(left, &lx, &ly, &lw, &lh);
	vgui_widget_get_rect(right, &rx, &ry, &rw, &rh);
	check(near(lw, 99.0f) && near(lh, 300.0f), "splitter: horizontal ratio");
	check(near(rx, lx + lw + 4.0f) && near(rw, 297.0f), "splitter: sash separates panes");

	vgui_widget_set_min_size(left, 150.0f, 0.0f);
	vgui_splitter_set_ratio(splitter, 0.1f);
	vgui_layout(ui);
	vgui_widget_get_rect(left, nullptr, nullptr, &lw, nullptr);
	check(near(lw, 150.0f), "splitter: minimum width clamps ratio");

	vgui_widget_set_min_size(left, 0.0f, 0.0f);
	vgui_splitter_set_ratio(splitter, 0.0f);
	vgui_layout(ui);
	vgui_widget_get_rect(left, nullptr, nullptr, &lw, nullptr);
	vgui_widget_get_rect(right, nullptr, nullptr, &rw, nullptr);
	check(near(lw, 0.0f) && near(rw, 396.0f), "splitter: zero ratio collapses first pane");

	vgui_splitter_set_ratio(splitter, 1.0f);
	vgui_layout(ui);
	vgui_widget_get_rect(left, nullptr, nullptr, &lw, nullptr);
	vgui_widget_get_rect(right, nullptr, nullptr, &rw, nullptr);
	check(near(lw, 396.0f) && near(rw, 0.0f), "splitter: full ratio collapses second pane");

	vgui_splitter_set_ratio(splitter, 0.25f);
	vgui_layout(ui);
	vgui_widget_get_rect(left, &lx, &ly, &lw, &lh);

	float sash_x = lx + lw;
	vgui_input_mouse_move(ui, sash_x + 2.0f, ly + 20.0f);
	vgui_input_mouse_down(ui, 1, sash_x + 2.0f, ly + 20.0f, 1);
	vgui_input_mouse_move(ui, sash_x + 102.0f, ly + 20.0f);
	vgui_input_mouse_up(ui, 1, sash_x + 102.0f, ly + 20.0f, 1);
	vgui_layout(ui);
	vgui_widget_get_rect(left, &lx, &ly, &lw, &lh);
	check(lw > 180.0f, "splitter: dragging sash resizes panes");
	check(cb.changes >= 1 && cb.last > 0.4f, "splitter: drag notifies value callback");

	float new_sash = lx + lw;
	click_at(ui, new_sash + 2.0f, ly + 20.0f, 2);
	vgui_layout(ui);
	vgui_widget_get_rect(left, nullptr, nullptr, &lw, nullptr);
	check(near(lw, 198.0f) && near(vgui_splitter_get_ratio(splitter), 0.5f),
		"splitter: double-click resets ratio");

	vgui_splitter_set_vertical(splitter, 1);
	vgui_splitter_set_ratio(splitter, 0.5f);
	vgui_layout(ui);
	vgui_widget_get_rect(left, &lx, &ly, &lw, &lh);
	vgui_widget_get_rect(right, &rx, &ry, &rw, &rh);
	check(near(lh, 148.0f) && near(rh, 148.0f), "splitter: vertical ratio");
	check(near(ry, ly + lh + 4.0f), "splitter: vertical sash separates panes");

	vgui_widget_set_min_size(left, 0.0f, 120.0f);
	vgui_splitter_set_ratio(splitter, 0.1f);
	vgui_layout(ui);
	vgui_widget_get_rect(left, nullptr, nullptr, nullptr, &lh);
	check(near(lh, 120.0f), "splitter: minimum height clamps ratio");

	vgui_widget_destroy(splitter);

	// Nested splitters are ordinary children and keep their own sash/ratio.
	vgui_widget* outer = vgui_widget_add(vgui_root(ui), VGUI_SPLITTER, nullptr);
	vgui_widget_set_pos(outer, 450.0f, 20.0f);
	vgui_widget_set_size(outer, 300.0f, 300.0f);
	vgui_splitter_set_sash_size(outer, 4.0f);
	vgui_splitter_set_ratio(outer, 0.5f);
	vgui_widget* inner = vgui_widget_add(outer, VGUI_SPLITTER, nullptr);
	vgui_splitter_set_sash_size(inner, 4.0f);
	vgui_splitter_set_ratio(inner, 0.5f);
	vgui_widget* inner_left = vgui_widget_add(inner, VGUI_PANEL, nullptr);
	vgui_widget* inner_right = vgui_widget_add(inner, VGUI_PANEL, nullptr);
	vgui_widget_add(outer, VGUI_PANEL, nullptr);
	vgui_layout(ui);

	float ix = 0, iy = 0, iw = 0, ih = 0;
	float ilw = 0, irw = 0;
	vgui_widget_get_rect(inner, &ix, &iy, &iw, &ih);
	vgui_widget_get_rect(inner_left, nullptr, nullptr, &ilw, nullptr);
	vgui_widget_get_rect(inner_right, nullptr, nullptr, &irw, nullptr);
	check(near(iw, 148.0f) && near(ih, 300.0f), "splitter: nested pane size");
	check(near(ilw, 72.0f) && near(irw, 72.0f), "splitter: nested ratio and sash");

	vgui_widget_destroy(outer);
}

static void test_tabview(vgui_context* ui)
{
	vgui_widget* tabs = vgui_widget_add(vgui_root(ui), VGUI_TABVIEW, nullptr);
	vgui_widget_set_pos(tabs, 20.0f, 360.0f);
	vgui_widget_set_size(tabs, 340.0f, 220.0f);
	vgui_tab_set_tab_height(tabs, 28.0f);

	tab_cb cb;
	tab_event_cb ecb;
	vgui_tab_on_select(tabs, on_tab_select, &cb);
	vgui_tab_on_close(tabs, on_tab_close, &cb);
	vgui_widget_on_event(tabs, on_tab_event, &ecb);

	vgui_widget* pa = vgui_widget_create(ui, VGUI_PANEL, nullptr);
	vgui_widget* pb = vgui_widget_create(ui, VGUI_PANEL, nullptr);
	vgui_widget* pc = vgui_widget_create(ui, VGUI_PANEL, nullptr);
	vgui_item_id ida = vgui_tab_add(tabs, "Alpha", pa);
	vgui_item_id idb = vgui_tab_add(tabs, "Beta", pb);
	vgui_item_id idc = vgui_tab_add(tabs, "Gamma", pc);
	check(ida && idb && idc && ida != idb && idb != idc, "tab: add assigns unique ids");
	check(vgui_tab_get_count(tabs) == 3, "tab: count after add");
	check(vgui_tab_get_at(tabs, 1) == idb && vgui_tab_get_index(tabs, idc) == 2,
		"tab: stable order and index");
	check(std::string(vgui_tab_get_title(tabs, idb)) == "Beta", "tab: title");
	check(vgui_tab_get_active(tabs) == ida, "tab: first tab is active");
	check(vgui_widget_get_parent(pa) == tabs, "tab: page is attached");
	check(vgui_widget_get_visible(pa) == 1 && vgui_widget_get_visible(pb) == 0,
		"tab: only active page is visible");

	vgui_layout(ui);
	float px = 0, py = 0, pw = 0, ph = 0;
	vgui_widget_get_rect(pa, &px, &py, &pw, &ph);
	check(pw > 0.0f && ph > 0.0f, "tab: active page is laid out");

	float ax = 0, ay = 0, aw = 0, ah = 0;
	float bx = 0, by = 0, bw = 0, bh = 0;
	float cx = 0, cy = 0, cw = 0, ch = 0;
	check(vgui_tab_get_rect(tabs, ida, &ax, &ay, &aw, &ah) &&
		vgui_tab_get_rect(tabs, idb, &bx, &by, &bw, &bh) &&
		vgui_tab_get_rect(tabs, idc, &cx, &cy, &cw, &ch),
		"tab: get-rect returns tab geometry");

	vgui_input_mouse_move(ui, ax + aw * 0.5f, ay + ah * 0.5f);
	vgui_input_mouse_down(ui, 3, ax + aw * 0.5f, ay + ah * 0.5f, 1);
	vgui_input_mouse_up(ui, 3, ax + aw * 0.5f, ay + ah * 0.5f, 1);
	check(ecb.right_downs == 1 && ecb.item == ida,
		"tab: right-click emits item event for host menu");

	check(vgui_tab_set_active(tabs, idb) == 1, "tab: set active");
	check(cb.selects == 1 && cb.last_select == idb, "tab: select callback");
	vgui_layout(ui);
	check(vgui_widget_get_visible(pa) == 0 && vgui_widget_get_visible(pb) == 1,
		"tab: switching hides old page");

	click_at(ui, ax + aw * 0.5f, ay + ah * 0.5f);
	check(vgui_tab_get_active(tabs) == ida && cb.selects == 2,
		"tab: mouse click changes active tab");

	vgui_tab_set_enabled(tabs, idb, 0);
	click_at(ui, bx + bw * 0.5f, by + bh * 0.5f);
	check(vgui_tab_get_active(tabs) == ida && cb.selects == 2,
		"tab: disabled tab cannot be activated");
	vgui_tab_set_enabled(tabs, idb, 1);

	vgui_tab_set_unsaved(tabs, idb, 1);
	check(vgui_tab_get_unsaved(tabs, idb) == 1, "tab: unsaved marker");
	vgui_tab_set_closeable(tabs, idb, 0);
	vgui_layout(ui);
	vgui_tab_get_rect(tabs, idb, &bx, &by, &bw, &bh);
	click_at(ui, bx + bw - 14.0f, by + bh * 0.5f);
	check(vgui_tab_get_active(tabs) == idb && cb.closes == 0,
		"tab: non-closeable tab has no close hit");
	vgui_tab_set_closeable(tabs, idb, 1);

	vgui_layout(ui);
	vgui_tab_get_rect(tabs, idc, &cx, &cy, &cw, &ch);
	click_at(ui, cx + cw - 14.0f, cy + ch * 0.5f);
	check(cb.closes == 1 && cb.last_close == idc, "tab: close callback");
	check(vgui_tab_get_count(tabs) == 2 && vgui_tab_get_page(tabs, idc) == nullptr,
		"tab: close removes page");

	vgui_set_focus(ui, tabs);
	int handled = vgui_input_key(ui, VGUI_KEY_RIGHT, 1, 0);
	vgui_input_key(ui, VGUI_KEY_RIGHT, 0, 0);
	check(handled == 1 && vgui_tab_get_active(tabs) == ida,
		"tab: keyboard right wraps to next tab");
	vgui_input_key(ui, VGUI_KEY_LEFT, 1, 0);
	vgui_input_key(ui, VGUI_KEY_LEFT, 0, 0);
	check(vgui_tab_get_active(tabs) == idb, "tab: keyboard left selects previous tab");

	// Drag the first tab over the second to reorder.  The click selects it,
	// while the move past the midpoint commits the new order.
	vgui_layout(ui);
	vgui_tab_get_rect(tabs, ida, &ax, &ay, &aw, &ah);
	vgui_tab_get_rect(tabs, idb, &bx, &by, &bw, &bh);
	vgui_input_mouse_move(ui, ax + aw * 0.5f, ay + ah * 0.5f);
	vgui_input_mouse_down(ui, 1, ax + aw * 0.5f, ay + ah * 0.5f, 1);
	vgui_input_mouse_move(ui, bx + bw * 0.75f, by + bh * 0.5f);
	vgui_input_mouse_up(ui, 1, bx + bw * 0.75f, by + bh * 0.5f, 1);
	check(vgui_tab_get_at(tabs, 0) == idb && vgui_tab_get_at(tabs, 1) == ida,
		"tab: dragging reorders tabs");

	// Add enough tabs to overflow and scroll the strip with the wheel.
	vgui_tab_add(tabs, "Delta", vgui_widget_create(ui, VGUI_PANEL, nullptr));
	vgui_tab_add(tabs, "Epsilon", vgui_widget_create(ui, VGUI_PANEL, nullptr));
	vgui_item_id idf = vgui_tab_add(tabs, "Zeta", vgui_widget_create(ui, VGUI_PANEL, nullptr));
	vgui_layout(ui);
	float fx0 = 0, fy0 = 0, fw0 = 0, fh0 = 0;
	vgui_tab_get_rect(tabs, idf, &fx0, &fy0, &fw0, &fh0);
	float tab_x = 0, tab_y = 0, tab_w = 0, tab_h = 0;
	vgui_widget_get_rect(tabs, &tab_x, &tab_y, &tab_w, &tab_h);
	vgui_input_mouse_move(ui, tab_x + 10.0f, tab_y + 5.0f);
	vgui_input_mouse_wheel(ui, 0.0f, -1.0f);
	vgui_layout(ui);
	float fx1 = 0, fy1 = 0, fw1 = 0, fh1 = 0;
	vgui_tab_get_rect(tabs, idf, &fx1, &fy1, &fw1, &fh1);
	check(fx1 < fx0 - 1.0f, "tab: wheel scrolls overflowing tab strip");

	handled = vgui_input_key(ui, 'W', 1, VGUI_MOD_CTRL);
	vgui_input_key(ui, 'W', 0, VGUI_MOD_CTRL);
	check(handled == 1 && cb.closes == 2, "tab: ctrl+w closes active tab");
	check(vgui_tab_get_count(tabs) == 4, "tab: close keeps remaining tabs");

	vgui_widget_destroy(tabs);
}

int main()
{
	setvbuf(stdout, nullptr, _IONBF, 0);
	auto* wg = new app_mgr();
	font_familys_t* familys = new_font_family(wg->font_ctx, "Consolas,微软雅黑", 0);
	vgui_context* ui = vgui_create(familys, 16.0f);
	vgui_context_set_size(ui, 900, 700);
	vgui_widget_set_layout(vgui_root(ui), VGUI_LAYOUT_ABSOLUTE);

	check(std::string(vgui_version()) == "0.3.0", "version is 0.3.0");

	// Fixed geometry makes mouse hit tests independent of font metrics.
	vgui_style style;
	vgui_style_default(&style);
	style.list.row_height = 24.0f;
	style.tree.row_height = 24.0f;
	style.menu.item_height = 26.0f;
	style.menu.separator_height = 10.0f;
	vgui_context_set_style(ui, &style);

	test_list(ui, style);
	test_tree(ui, style);
	test_menu(ui, style);
	test_texture_image(ui);
	test_wrap(ui);
	test_splitter(ui);
	test_tabview(ui);

	vgui_destroy(ui);
	delete_font_family(familys);
	delete wg;

	printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
