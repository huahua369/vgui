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

	vgui_destroy(ui);
	delete_font_family(familys);
	delete wg;

	printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
