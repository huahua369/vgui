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

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "pch.h"		// glm etc. (required by ovg_c.h)
#include <ovg_c.h>		// font_familys_t
#include <vgui_sdl3.h>	// app_mgr / new_font_family
#include <vgui/vgui.h>		// public C API

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

static void on_button_click_only(vgui_widget*, const vgui_event* event, void* user)
{
	if (event && event->type == VGUI_EVENT_CLICK) (*(int*)user)++;
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

/* ---------------------------- virtual row source ------------------------- */

struct virtual_rows
{
	int count = 0;
};

static int vrows_count(void* user)
{
	return ((virtual_rows*)user)->count;
}

static int vrows_info(void* user, int index, const char** text, const char** detail,
	void** item_user)
{
	(void)user;
	static char buf[32];
	std::snprintf(buf, sizeof(buf), "row %d", index);
	if (text) *text = buf;
	if (detail) *detail = nullptr;
	if (item_user) *item_user = nullptr;
	return 1;
}

static vgui_item_id vrows_id(void* user, int index)
{
	(void)user;
	return (vgui_item_id)(index + 1);
}

static int vrows_index(void* user, vgui_item_id item)
{
	(void)user;
	return item >= 1 ? (int)item - 1 : -1;
}

static void test_list_virtual(vgui_context* ui)
{
	vgui_widget* list = vgui_widget_add(vgui_root(ui), VGUI_LIST, nullptr);
	vgui_widget_set_id(list, "virtual-list");
	vgui_widget_set_pos(list, 10.0f, 260.0f);
	vgui_widget_set_size(list, 240.0f, 200.0f);

	virtual_rows rows;
	rows.count = 1000000;

	vgui_row_source src = {};
	src.row_count = vrows_count;
	src.row_info = vrows_info;
	src.row_id = vrows_id;
	src.row_id_index = vrows_index;
	src.user = &rows;
	vgui_list_set_row_source(list, &src);

	check(vgui_list_is_virtual(list) == 1, "virtual: is_virtual reports attached source");
	check(vgui_list_get_count(list) == 1000000, "virtual: count comes from the source");
	check(vgui_list_get_item_at(list, 123) == 124, "virtual: item_at uses row_id");
	check(std::string(vgui_list_get_item_text(list, 124)) == "row 123",
		"virtual: item text comes from row_info");
	check(vgui_list_get_item_at(list, -1) == 0, "virtual: negative index rejected");
	check(vgui_list_get_item_at(list, 1000000) == 0, "virtual: out-of-range index rejected");

	check(vgui_list_add_item(list, "x", nullptr) == 0, "virtual: add_item is a no-op");
	check(vgui_list_get_count(list) == 1000000, "virtual: count unchanged after add");
	vgui_list_set_item_text(list, 124, "changed");
	check(std::string(vgui_list_get_item_text(list, 124)) == "row 123",
		"virtual: set_item_text is a no-op");
	vgui_list_clear(list);
	check(vgui_list_get_count(list) == 1000000, "virtual: clear is a no-op");

	// Layout must derive the scrollable height from the source count.
	vgui_update(ui, 0.016f);
	float cw = 0.0f, ch = 0.0f;
	vgui_panel_get_content_size(list, &cw, &ch);
	(void)cw;
	check(ch >= 1000000.0f, "virtual: content height covers every row");

	// Selection and cursor stay id-based in virtual mode.
	vgui_list_select(list, 500, 1);
	check(vgui_list_is_selected(list, 500), "virtual: selection by id");
	check(vgui_list_get_selected_count(list) == 1, "virtual: single selection count");

	// Detaching returns the control to the materialized model.
	vgui_list_set_row_source(list, nullptr);
	check(vgui_list_is_virtual(list) == 0, "virtual: detach clears virtual mode");
	check(vgui_list_get_count(list) == 0, "virtual: empty after detach");
	check(vgui_list_get_item_at(list, 0) == 0, "virtual: detached list has no rows");
}

/* Tree virtual mode: rows are the flattened visible rows, the host owns
 * expansion state and calls vgui_tree_source_changed() after it changes. */

struct virtual_tree_rows
{
	int expanded = 0;		// 宿主控制的展开状态
	int disabled_index = -1;	// 该行返回 enabled=0
	int toggles = 0;		// row_toggle 被调用的次数
};

constexpr int kVirtualTreeChildRows = 50;

static int vtrows_count(void* user)
{
	return ((virtual_tree_rows*)user)->expanded ? kVirtualTreeChildRows : 1;
}

static int vtrows_info(void* user, int index, const char** text, const char** detail,
	void** item_user)
{
	(void)user;
	static char buf[32];
	if (index == 0)
		std::snprintf(buf, sizeof(buf), "root");
	else
		std::snprintf(buf, sizeof(buf), "child %d", index);
	if (text) *text = buf;
	if (detail) *detail = nullptr;
	if (item_user) *item_user = nullptr;
	return 1;
}

static vgui_item_id vtrows_id(void* user, int index)
{
	(void)user;
	return (vgui_item_id)(100 + index);
}

static int vtrows_index(void* user, vgui_item_id item)
{
	(void)user;
	return item >= 100 ? (int)item - 100 : -1;
}

static int vtrows_depth(void* user, int index)
{
	(void)user;
	return index == 0 ? 0 : 1;
}

static int vtrows_has_children(void* user, int index)
{
	(void)user;
	return index == 0 ? 1 : 0;
}

static int vtrows_expanded(void* user, int index)
{
	return (index == 0 && ((virtual_tree_rows*)user)->expanded) ? 1 : 0;
}

static void vtrows_toggle(void* user, int index)
{
	virtual_tree_rows* rows = (virtual_tree_rows*)user;
	if (index != 0) return;
	rows->toggles++;
	rows->expanded ^= 1;
}

static int vtrows_enabled(void* user, int index)
{
	return index == ((virtual_tree_rows*)user)->disabled_index ? 0 : 1;
}

static float vtrows_max_width(void* user)
{
	(void)user;
	return 333.0f;
}

static void test_tree_virtual(vgui_context* ui, const vgui_style& style)
{
	vgui_widget* tree = vgui_widget_add(vgui_root(ui), VGUI_TREE, nullptr);
	vgui_widget_set_id(tree, "virtual-tree");
	vgui_widget_set_pos(tree, 300.0f, 260.0f);
	vgui_widget_set_size(tree, 240.0f, 200.0f);

	virtual_tree_rows rows;

	vgui_row_source src = {};
	src.row_count = vtrows_count;
	src.row_info = vtrows_info;
	src.row_id = vtrows_id;
	src.row_id_index = vtrows_index;
	src.row_depth = vtrows_depth;
	src.row_has_children = vtrows_has_children;
	src.row_expanded = vtrows_expanded;
	src.row_toggle = vtrows_toggle;
	src.row_enabled = vtrows_enabled;
	src.user = &rows;
	vgui_tree_set_row_source(tree, &src);

	// ---- 只读查询 ------------------------------------------------------
	check(vgui_tree_is_virtual(tree) == 1, "tree virtual: is_virtual reports attached source");
	check(vgui_tree_get_node_count(tree) == 1, "tree virtual: count is the visible row count");
	check(vgui_tree_get_item_enabled(tree, 100) == 1, "tree virtual: row enabled from source");
	check(std::string(vgui_tree_get_item_text(tree, 100)) == "root",
		"tree virtual: text comes from row_info");
	check(vgui_tree_get_depth(tree, 101) == 1, "tree virtual: depth query is safe");
	check(vgui_tree_get_depth(tree, 100) == 0, "tree virtual: root depth");
	check(vgui_tree_get_item_expanded(tree, 100) == 0, "tree virtual: collapsed by default");

	// ---- 虚拟模式下模型修改接口无效 ------------------------------------
	check(vgui_tree_add_node(tree, 0, "x", nullptr) == 0, "tree virtual: add_node is a no-op");
	check(vgui_tree_get_node_count(tree) == 1, "tree virtual: count unchanged after add");
	check(vgui_tree_toggle(tree, 100) == 0, "tree virtual: model toggle is a no-op");
	vgui_tree_expand_all(tree);
	check(vgui_tree_get_node_count(tree) == 1, "tree virtual: expand_all is a no-op");
	vgui_tree_set_item_expanded(tree, 100, 1);
	check(vgui_tree_get_item_expanded(tree, 100) == 0,
		"tree virtual: set_item_expanded is a no-op");
	vgui_tree_clear(tree);
	check(vgui_tree_get_node_count(tree) == 1, "tree virtual: clear is a no-op");

	// ---- 命中测试、箭头展开与选择 ---------------------------------------
	vgui_update(ui, 0.016f);
	float tx = 0.0f, ty = 0.0f, tw = 0.0f, th = 0.0f;
	vgui_widget_get_rect(tree, &tx, &ty, &tw, &th);
	(void)tw; (void)th;
	float row_h = style.tree.row_height;
	check(row_h > 0.0f, "tree virtual: test style has fixed row height");
	float row0_y = ty + style.tree.padding_y + row_h * 0.5f;
	float arrow_x = tx + style.tree.padding_x + style.tree.arrow_size * 0.5f;

	cb_state sels;
	vgui_tree_on_select(tree, on_select, &sels);

	click_at(ui, arrow_x, row0_y);
	check(rows.toggles == 1, "tree virtual: arrow click calls row_toggle");
	check(rows.expanded == 1, "tree virtual: host expansion state flipped");
	check(vgui_tree_get_selected_count(tree) == 0, "tree virtual: arrow click does not select");
	vgui_update(ui, 0.016f);
	check(vgui_tree_get_node_count(tree) == kVirtualTreeChildRows,
		"tree virtual: visible rows grow after expand");

	float row1_y = row0_y + row_h;
	click_at(ui, tx + 40.0f, row1_y);
	check(vgui_tree_get_cursor(tree) == 101, "tree virtual: click sets cursor to row id");
	check(vgui_tree_is_selected(tree, 101), "tree virtual: click selects row id");
	check(sels.last_select == 101, "tree virtual: select callback gets the row id");

	// ---- 键盘导航 ------------------------------------------------------
	vgui_set_focus(ui, tree);
	vgui_input_key(ui, VGUI_KEY_HOME, 1, 0);
	vgui_input_key(ui, VGUI_KEY_HOME, 0, 0);
	check(vgui_tree_get_cursor(tree) == 100, "tree virtual: HOME moves cursor to first row");

	vgui_input_key(ui, VGUI_KEY_DOWN, 1, 0);
	vgui_input_key(ui, VGUI_KEY_DOWN, 0, 0);
	check(vgui_tree_get_cursor(tree) == 101, "tree virtual: DOWN moves to next row");

	vgui_input_key(ui, VGUI_KEY_END, 1, 0);
	vgui_input_key(ui, VGUI_KEY_END, 0, 0);
	check(vgui_tree_get_cursor(tree) == 100 + kVirtualTreeChildRows - 1,
		"tree virtual: END moves cursor to last row");

	// 禁用行必须被键盘导航跳过。
	rows.disabled_index = 2;
	vgui_input_key(ui, VGUI_KEY_HOME, 1, 0);
	vgui_input_key(ui, VGUI_KEY_HOME, 0, 0);
	vgui_input_key(ui, VGUI_KEY_DOWN, 1, 0);
	vgui_input_key(ui, VGUI_KEY_DOWN, 0, 0);
	vgui_input_key(ui, VGUI_KEY_DOWN, 1, 0);
	vgui_input_key(ui, VGUI_KEY_DOWN, 0, 0);
	check(vgui_tree_get_cursor(tree) == 103, "tree virtual: keyboard skips disabled rows");

	// ---- Ctrl+A 全选 ---------------------------------------------------
	vgui_tree_set_selection_mode(tree, VGUI_SELECTION_MULTIPLE);
	vgui_tree_clear_selection(tree);
	vgui_input_key(ui, 'A', 1, VGUI_MOD_CTRL);
	vgui_input_key(ui, 'A', 0, VGUI_MOD_CTRL);
	check(vgui_tree_get_selected_count(tree) == kVirtualTreeChildRows - 1,
		"tree virtual: ctrl+A selects every enabled visible row");

	// ---- 空格展开/折叠 ------------------------------------------------
	vgui_tree_set_selection_mode(tree, VGUI_SELECTION_SINGLE);
	vgui_input_key(ui, VGUI_KEY_HOME, 1, 0);
	vgui_input_key(ui, VGUI_KEY_HOME, 0, 0);
	vgui_input_key(ui, ' ', 1, 0);
	vgui_input_key(ui, ' ', 0, 0);
	check(rows.toggles == 2, "tree virtual: space calls row_toggle");
	check(rows.expanded == 0, "tree virtual: space collapses host state");
	vgui_update(ui, 0.016f);
	check(vgui_tree_get_node_count(tree) == 1, "tree virtual: collapsed row count");

	// ---- 内容尺寸与滚动 -----------------------------------------------
	rows.disabled_index = -1;
	rows.expanded = 1;
	vgui_tree_source_changed(tree);
	vgui_update(ui, 0.016f);
	float cw = 0.0f, ch = 0.0f;
	vgui_panel_get_content_size(tree, &cw, &ch);
	(void)cw;
	check(ch >= (float)kVirtualTreeChildRows, "tree virtual: content height covers visible rows");

	vgui_tree_scroll_to_node(tree, 100 + kVirtualTreeChildRows - 1, VGUI_SCROLL_ALIGN_BOTTOM);
	float sx = 0.0f, sy = 0.0f;
	vgui_panel_get_scroll(tree, &sx, &sy);
	check(sy > 0.0f, "tree virtual: scroll to last row scrolls the view");

	// ---- 层级查询在虚拟模式下为空 ---------------------------------------
	check(vgui_tree_get_root_count(tree) == 0, "tree virtual: hierarchy queries are empty");
	check(vgui_tree_get_parent(tree, 101) == 0, "tree virtual: parent is unknown");
	check(vgui_tree_get_child_count(tree, 100) == 0, "tree virtual: child count is empty");

	// ---- 最宽行回调与缺省回退 -------------------------------------------
	vgui_widget* plain = vgui_widget_add(vgui_root(ui), VGUI_TREE, nullptr);
	vgui_widget_set_pos(plain, 560.0f, 260.0f);
	vgui_widget_set_size(plain, 0.0f, 200.0f);	// 自动宽度才会计算 content_w
	vgui_tree_set_row_source(plain, &src);
	vgui_update(ui, 0.016f);
	float pcw = 0.0f, pch = 0.0f;
	vgui_panel_get_content_size(plain, &pcw, &pch);
	(void)pch;
	check(pcw >= style.tree.req_width - 1.0f,
		"tree virtual: content width falls back to the style width");

	vgui_widget* wide = vgui_widget_add(vgui_root(ui), VGUI_TREE, nullptr);
	vgui_widget_set_pos(wide, 760.0f, 260.0f);
	vgui_widget_set_size(wide, 0.0f, 200.0f);
	vgui_row_source wsrc = src;
	wsrc.row_max_width = vtrows_max_width;
	vgui_tree_set_row_source(wide, &wsrc);
	vgui_update(ui, 0.016f);
	float wcw = 0.0f, wch = 0.0f;
	vgui_panel_get_content_size(wide, &wcw, &wch);
	(void)wch;
	check(wcw > 330.0f, "tree virtual: row_max_width feeds the content width");

	vgui_tree_set_row_source(wide, nullptr);
	vgui_tree_set_row_source(plain, nullptr);

	// ---- 解除数据源 ----------------------------------------------------
	vgui_tree_set_row_source(tree, nullptr);
	check(vgui_tree_is_virtual(tree) == 0, "tree virtual: detach clears virtual mode");
	check(vgui_tree_get_node_count(tree) == 0, "tree virtual: empty after detach");
	check(vgui_tree_get_selected_count(tree) == 0, "tree virtual: detach clears selection");
}

static void test_tree_virtual_edges(vgui_context* ui, const vgui_style& style)
{
	// ---- 空数据源 ------------------------------------------------------
	vgui_widget* empty = vgui_widget_add(vgui_root(ui), VGUI_TREE, nullptr);
	vgui_widget_set_pos(empty, 300.0f, 480.0f);
	vgui_widget_set_size(empty, 240.0f, 120.0f);
	vgui_tree_set_empty_text(empty, "empty");

	vgui_row_source esrc = {};
	esrc.row_count = [](void*) -> int { return 0; };
	esrc.row_info = [](void*, int, const char**, const char**, void**) -> int { return 0; };
	esrc.row_id = [](void*, int) -> vgui_item_id { return 0; };
	vgui_tree_set_row_source(empty, &esrc);

	check(vgui_tree_is_virtual(empty) == 1, "tree virtual edge: empty source is virtual");
	check(vgui_tree_get_node_count(empty) == 0, "tree virtual edge: empty source has no rows");
	check(std::string(vgui_tree_get_item_text(empty, 1)).empty(),
		"tree virtual edge: unknown id returns empty text");
	vgui_update(ui, 0.016f);
	float eh = 0.0f, ecw = 0.0f;
	vgui_panel_get_content_size(empty, &ecw, &eh);
	(void)ecw;
	check(eh <= 0.0f, "tree virtual edge: empty source has no content height");
	vgui_tree_set_row_source(empty, nullptr);

	// ---- 没有 row_toggle 时的箭头点击 ----------------------------------
	vgui_widget* noToggle = vgui_widget_add(vgui_root(ui), VGUI_TREE, nullptr);
	vgui_widget_set_pos(noToggle, 560.0f, 480.0f);
	vgui_widget_set_size(noToggle, 240.0f, 120.0f);

	virtual_tree_rows rows;
	vgui_row_source nsrc = {};
	nsrc.row_count = vtrows_count;
	nsrc.row_info = vtrows_info;
	nsrc.row_id = vtrows_id;
	nsrc.row_id_index = vtrows_index;
	nsrc.row_depth = vtrows_depth;
	nsrc.row_has_children = vtrows_has_children;
	nsrc.row_expanded = vtrows_expanded;
	nsrc.user = &rows;	// 故意不设置 row_toggle
	vgui_tree_set_row_source(noToggle, &nsrc);

	vgui_update(ui, 0.016f);
	float nx = 0.0f, ny = 0.0f, nw = 0.0f, nh = 0.0f;
	vgui_widget_get_rect(noToggle, &nx, &ny, &nw, &nh);
	(void)nw; (void)nh;
	float nrow_y = ny + style.tree.padding_y + style.tree.row_height * 0.5f;
	float narrow_x = nx + style.tree.padding_x + style.tree.arrow_size * 0.5f;
	click_at(ui, narrow_x, nrow_y);
	check(rows.toggles == 0 && rows.expanded == 0,
		"tree virtual edge: arrow click without row_toggle is a no-op");
	check(vgui_tree_get_node_count(noToggle) == 1,
		"tree virtual edge: row count unchanged without row_toggle");
	vgui_tree_set_row_source(noToggle, nullptr);

	// ---- 解除后重新挂载 -------------------------------------------------
	virtual_tree_rows rows2;
	vgui_widget* recycle = vgui_widget_add(vgui_root(ui), VGUI_TREE, nullptr);
	vgui_widget_set_pos(recycle, 800.0f, 480.0f);
	vgui_widget_set_size(recycle, 240.0f, 120.0f);

	vgui_row_source rsrc = {};
	rsrc.row_count = vtrows_count;
	rsrc.row_info = vtrows_info;
	rsrc.row_id = vtrows_id;
	rsrc.row_id_index = vtrows_index;
	rsrc.row_depth = vtrows_depth;
	rsrc.row_has_children = vtrows_has_children;
	rsrc.row_expanded = vtrows_expanded;
	rsrc.row_toggle = vtrows_toggle;
	rsrc.user = &rows2;

	vgui_tree_set_row_source(recycle, &rsrc);
	vgui_tree_select(recycle, 100, 1);
	vgui_tree_set_row_source(recycle, nullptr);
	check(vgui_tree_get_selected_count(recycle) == 0, "tree virtual edge: detach drops selection");

	vgui_tree_set_row_source(recycle, &rsrc);
	check(vgui_tree_is_virtual(recycle) == 1, "tree virtual edge: re-attach restores virtual mode");
	check(vgui_tree_get_node_count(recycle) == 1, "tree virtual edge: re-attach restores row count");
	check(vgui_tree_get_selected_count(recycle) == 0, "tree virtual edge: re-attach starts clean");
	vgui_tree_set_row_source(recycle, nullptr);
}

/* F04 row extensions: per-row icons, text color overrides and trailing inline
 * buttons in both the full model and the virtual row source modes.  Inline
 * buttons must never change the row selection. */

struct row_button_state
{
	int clicks = 0;
	vgui_item_id last_item = 0;
	int last_button = -1;
	int source_clicks = 0;
	int last_source_row = -1;
	vgui_texture* icon0 = nullptr;
	vgui_texture* icon1 = nullptr;
	virtual_rows rows;				// 复用 test_list_virtual 的数据源
	virtual_tree_rows tree_rows;	// 复用 test_tree_virtual 的数据源
};

static void on_row_button(vgui_widget*, vgui_item_id item, int button, void* user)
{
	row_button_state* s = (row_button_state*)user;
	s->clicks++;
	s->last_item = item;
	s->last_button = button;
}

static void on_source_row_button(void* user, int index, int button)
{
	row_button_state* s = (row_button_state*)user;
	s->source_clicks++;
	s->last_source_row = index;
	s->last_button = button;
}

/* 数据源回调包装：把 user 从 row_button_state 偏移到内嵌的数据结构。 */
static int rext_count(void* user)
{
	return vrows_count(&((row_button_state*)user)->rows);
}

static int rext_info(void* user, int index, const char** text, const char** detail,
	void** item_user)
{
	return vrows_info(&((row_button_state*)user)->rows, index, text, detail, item_user);
}

static vgui_item_id rext_id(void* user, int index)
{
	return vrows_id(&((row_button_state*)user)->rows, index);
}

static int rext_index(void* user, vgui_item_id item)
{
	return vrows_index(&((row_button_state*)user)->rows, item);
}

static int rext_tree_count(void* user)
{
	return vtrows_count(&((row_button_state*)user)->tree_rows);
}

static int rext_tree_info(void* user, int index, const char** text, const char** detail,
	void** item_user)
{
	return vtrows_info(&((row_button_state*)user)->tree_rows, index, text, detail, item_user);
}

static vgui_item_id rext_tree_id(void* user, int index)
{
	return vtrows_id(&((row_button_state*)user)->tree_rows, index);
}

static int rext_tree_index(void* user, vgui_item_id item)
{
	return vtrows_index(&((row_button_state*)user)->tree_rows, item);
}

static int rext_tree_depth(void* user, int index)
{
	return vtrows_depth(&((row_button_state*)user)->tree_rows, index);
}

static int rext_tree_has_children(void* user, int index)
{
	return vtrows_has_children(&((row_button_state*)user)->tree_rows, index);
}

static int rext_tree_expanded(void* user, int index)
{
	return vtrows_expanded(&((row_button_state*)user)->tree_rows, index);
}

static void rext_tree_toggle(void* user, int index)
{
	vtrows_toggle(&((row_button_state*)user)->tree_rows, index);
}

static int rext_icon(void* user, int index, vgui_texture** out_texture)
{
	row_button_state* s = (row_button_state*)user;
	if (out_texture) *out_texture = (index % 2 == 0) ? s->icon0 : s->icon1;
	return 1;
}

static vgui_color rext_text_color(void* user, int index)
{
	(void)user;
	return (index == 1) ? 0xFF00FF00u : 0;
}

static int rext_buttons(void* user, int index)
{
	(void)user;
	return (index == 1) ? 2 : 0;
}

static int rext_button_icon(void* user, int index, int button, vgui_texture** out_texture)
{
	(void)index;
	row_button_state* s = (row_button_state*)user;
	if (out_texture) *out_texture = (button == 0) ? s->icon0 : s->icon1;
	return 1;
}

static void test_list_row_extensions(vgui_context* ui, const vgui_style& style)
{
	uint32_t pixels[4] = { 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu };
	vgui_texture_desc desc = {};
	desc.width = 2;
	desc.height = 2;
	desc.format = VGUI_TEXTURE_FORMAT_RGBA8;
	desc.pixels = pixels;
	vgui_texture* tex = vgui_texture_create(ui, &desc);
	check(tex != nullptr, "row ext: texture created");

	/* Full model mode. */
	{
		vgui_widget* list = vgui_widget_add(vgui_root(ui), VGUI_LIST, nullptr);
		vgui_widget_set_id(list, "row-ext-list");
		vgui_widget_set_pos(list, 10.0f, 10.0f);
		vgui_widget_set_size(list, 240.0f, 200.0f);

		cb_state cb;
		vgui_list_on_select(list, on_select, &cb);
		row_button_state bs;
		bs.icon0 = tex;
		bs.icon1 = tex;
		vgui_list_on_item_button(list, on_row_button, &bs);

		vgui_item_id a = vgui_list_add_item(list, "Alpha", nullptr);
		vgui_item_id b = vgui_list_add_item(list, "Beta", nullptr);
		vgui_item_id c = vgui_list_add_item(list, "Gamma", nullptr);

		vgui_list_set_item_icon(list, a, tex);
		vgui_list_set_item_text_color(list, b, 0xFF00FF00u);
		vgui_list_set_item_buttons(list, a, 2);
		vgui_list_set_item_button_icon(list, a, 0, tex);
		check(vgui_list_get_item_icon(list, a) == tex, "list row ext: icon setter/getter");
		check(vgui_list_get_item_text_color(list, b) == 0xFF00FF00u,
			"list row ext: text color setter/getter");
		check(vgui_list_get_item_text_color(list, c) == 0,
			"list row ext: default text color is zero");
		check(vgui_list_get_item_buttons(list, a) == 2, "list row ext: button count");
		check(vgui_list_get_item_button_icon(list, a, 0) == tex,
			"list row ext: button icon");
		check(vgui_list_get_item_button_icon(list, a, 1) == nullptr,
			"list row ext: unset button icon is null");
		vgui_list_set_item_buttons(list, a, 99);
		check(vgui_list_get_item_buttons(list, a) == VGUI_MAX_ROW_BUTTONS,
			"list row ext: button count clamped");
		vgui_list_set_item_buttons(list, a, 2);

		// Insert/remove must not lose an id-based selection.
		vgui_list_select(list, b, 1);
		vgui_item_id inserted = vgui_list_insert_item(list, 0, "Head", nullptr);
		check(vgui_list_is_selected(list, b), "list row ext: insert keeps selection");
		vgui_list_remove_item(list, inserted);
		check(vgui_list_is_selected(list, b), "list row ext: remove keeps selection");

		// Click positions: trailing buttons sit at the right edge of the content
		// box (inside the widget's right padding).  Item a is back at row 0
		// after the insert/remove round-trip, and the list has no visible
		// scrollbar here, so no bar reserve shifts the buttons.
		vgui_update(ui, 0.016f);
		float lx = 0.0f, ly = 0.0f, lw = 0.0f, lh = 0.0f;
		vgui_widget_get_rect(list, &lx, &ly, &lw, &lh);
		(void)lh;
		const float row_h = style.list.row_height;
		const float row0_y = ly + style.list.padding_y + row_h * 0.5f;
		const float bsize = std::max(10.0f, std::min(18.0f, row_h - 8.0f));
		const float content_right = lx + lw - style.list.padding_x;
		const float btn0_x = content_right - 4.0f - bsize - (bsize + 4.0f);
		const float btn1_x = content_right - 4.0f - bsize;

		click_at(ui, btn0_x, row0_y);
		check(bs.clicks == 1 && bs.last_item == a && bs.last_button == 0,
			"list row ext: first button click fires callback");
		check(vgui_list_is_selected(list, a) == 0,
			"list row ext: button click does not select");

		click_at(ui, btn1_x, row0_y);
		check(bs.clicks == 2 && bs.last_button == 1, "list row ext: second button click");

		click_at(ui, btn0_x + bsize + 2.0f, row0_y);	// gap between buttons
		check(bs.clicks == 2, "list row ext: gap between buttons is inert");
		check(vgui_list_is_selected(list, a),
			"list row ext: gap click behaves as a row click");

		const int selects_before_text = cb.selects;
		vgui_list_clear_selection(list);
		click_at(ui, lx + 30.0f, row0_y);
		check(cb.selects == selects_before_text + 1 && vgui_list_is_selected(list, a),
			"list row ext: text area still selects");
		check(bs.clicks == 2, "list row ext: text area not a button");
	}

	/* Virtual mode: the same extensions come from the row source callbacks. */
	{
		vgui_widget* list = vgui_widget_add(vgui_root(ui), VGUI_LIST, nullptr);
		vgui_widget_set_id(list, "row-ext-vlist");
		vgui_widget_set_pos(list, 400.0f, 10.0f);
		vgui_widget_set_size(list, 240.0f, 200.0f);

		row_button_state bs;
		bs.icon0 = tex;
		bs.icon1 = tex;
		bs.rows.count = 1000000;
		cb_state cb;
		vgui_list_on_select(list, on_select, &cb);

		vgui_row_source src = {};
		src.row_count = rext_count;
		src.row_info = rext_info;
		src.row_id = rext_id;
		src.row_id_index = rext_index;
		src.row_icon = rext_icon;
		src.row_text_color = rext_text_color;
		src.row_buttons = rext_buttons;
		src.row_button_icon = rext_button_icon;
		src.row_button_click = on_source_row_button;
		src.user = &bs;
		vgui_list_set_row_source(list, &src);

		check(vgui_list_get_item_icon(list, 2) == tex,
			"list row ext: virtual icon from row_icon");
		check(vgui_list_get_item_text_color(list, 1) == 0,
			"list row ext: virtual text color defaults to zero");
		check(vgui_list_get_item_text_color(list, 2) == 0xFF00FF00u,
			"list row ext: virtual text color override (row 1)");
		check(vgui_list_get_item_buttons(list, 2) == 2,
			"list row ext: virtual button count (row 1)");
		check(vgui_list_get_item_buttons(list, 1) == 0,
			"list row ext: virtual button count default");
		check(vgui_list_get_item_button_icon(list, 2, 1) == tex,
			"list row ext: virtual button icon");
		vgui_list_set_item_buttons(list, 2, 3);
		check(vgui_list_get_item_buttons(list, 2) == 2,
			"list row ext: virtual setters are no-ops");

		// Hover must only materialize the hovered row, not walk the source.
		vgui_update(ui, 0.016f);
		float lx = 0.0f, ly = 0.0f, lw = 0.0f, lh = 0.0f;
		vgui_widget_get_rect(list, &lx, &ly, &lw, &lh);
		(void)lh;
		const float row_h = style.list.row_height;
		const float row1_y = ly + style.list.padding_y + row_h + row_h * 0.5f;
		const float bsize = std::max(10.0f, std::min(18.0f, row_h - 8.0f));
		// 长列表有可见滚动条，按钮向内避让 bar 命中区。
		const float bar = style.panel.scrollbar_width + style.panel.scrollbar_inset
			+ style.panel.scrollbar_hit_padding + 2.0f;
		const float content_right = lx + lw - style.list.padding_x;
		const float btn0_x = content_right - bar - 4.0f - bsize - (bsize + 4.0f);
		click_at(ui, btn0_x, row1_y);
		check(bs.source_clicks == 1 && bs.last_source_row == 1 && bs.last_button == 0,
			"list row ext: virtual row_button_click fires");
		check(vgui_list_get_selected_count(list) == 0,
			"list row ext: virtual button click does not select");

		// Fine-grained invalidation is safe and keeps the virtual contract.
		vgui_list_source_rows_changed(list, 1, 1);
		check(vgui_list_is_virtual(list) == 1, "list row ext: rows_changed keeps virtual mode");
		check(vgui_list_get_count(list) == 1000000, "list row ext: rows_changed keeps count");
		vgui_list_source_rows_changed(list, -5, 0);	// 边界参数不允许破坏状态
		check(vgui_list_get_count(list) == 1000000, "list row ext: rows_changed tolerates odd range");
	}
}

static void test_tree_row_extensions(vgui_context* ui, const vgui_style& style)
{
	uint32_t pixels[4] = { 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu };
	vgui_texture_desc desc = {};
	desc.width = 2;
	desc.height = 2;
	desc.format = VGUI_TEXTURE_FORMAT_RGBA8;
	desc.pixels = pixels;
	vgui_texture* tex = vgui_texture_create(ui, &desc);
	check(tex != nullptr, "tree row ext: texture created");

	/* Full model mode. */
	{
		vgui_widget* tree = vgui_widget_add(vgui_root(ui), VGUI_TREE, nullptr);
		vgui_widget_set_id(tree, "row-ext-tree");
		vgui_widget_set_pos(tree, 10.0f, 260.0f);
		vgui_widget_set_size(tree, 240.0f, 200.0f);

		cb_state cb;
		vgui_tree_on_select(tree, on_select, &cb);
		row_button_state bs;
		bs.icon0 = tex;
		bs.icon1 = tex;
		vgui_tree_on_item_button(tree, on_row_button, &bs);

		vgui_item_id root = vgui_tree_add_node(tree, 0, "Root", nullptr);
		vgui_item_id child = vgui_tree_add_node(tree, root, "Child", nullptr);
		check(root && child, "tree row ext: nodes added");

		vgui_tree_set_item_icon(tree, root, tex);
		vgui_tree_set_item_text_color(tree, child, 0xFFFF8040u);
		vgui_tree_set_item_buttons(tree, root, 2);
		vgui_tree_set_item_button_icon(tree, root, 0, tex);
		check(vgui_tree_get_item_icon(tree, root) == tex, "tree row ext: icon setter/getter");
		check(vgui_tree_get_item_text_color(tree, child) == 0xFFFF8040u,
			"tree row ext: text color setter/getter");
		check(vgui_tree_get_item_buttons(tree, root) == 2, "tree row ext: button count");
		check(vgui_tree_get_item_button_icon(tree, root, 0) == tex,
			"tree row ext: button icon");

		vgui_update(ui, 0.016f);
		float tx = 0.0f, ty = 0.0f, tw = 0.0f, th = 0.0f;
		vgui_widget_get_rect(tree, &tx, &ty, &tw, &th);
		(void)th;
		const float row_h = style.tree.row_height;
		const float row0_y = ty + style.tree.padding_y + row_h * 0.5f;
		const float bsize = std::max(10.0f, std::min(18.0f, row_h - 8.0f));
		const float content_right = tx + tw - style.tree.padding_x;
		const float btn1_x = content_right - 4.0f - bsize;

		// Buttons must not toggle expansion either.
		vgui_tree_set_item_expanded(tree, root, 0);
		click_at(ui, btn1_x, row0_y);
		check(bs.clicks == 1 && bs.last_item == root && bs.last_button == 1,
			"tree row ext: button click fires callback");
		check(vgui_tree_is_selected(tree, root) == 0,
			"tree row ext: button click does not select");
		check(vgui_tree_get_item_expanded(tree, root) == 0,
			"tree row ext: button click does not toggle");

		click_at(ui, tx + 30.0f, row0_y);
		check(cb.selects == 1 && vgui_tree_is_selected(tree, root),
			"tree row ext: text area still selects");
	}

	/* Virtual mode. */
	{
		vgui_widget* tree = vgui_widget_add(vgui_root(ui), VGUI_TREE, nullptr);
		vgui_widget_set_id(tree, "row-ext-vtree");
		vgui_widget_set_pos(tree, 400.0f, 260.0f);
		vgui_widget_set_size(tree, 240.0f, 200.0f);

		row_button_state bs;
		bs.icon0 = tex;
		bs.icon1 = tex;
		bs.tree_rows.expanded = 1;
		cb_state cb;
		vgui_tree_on_select(tree, on_select, &cb);

		vgui_row_source src = {};
		src.row_count = rext_tree_count;
		src.row_info = rext_tree_info;
		src.row_id = rext_tree_id;
		src.row_id_index = rext_tree_index;
		src.row_depth = rext_tree_depth;
		src.row_has_children = rext_tree_has_children;
		src.row_expanded = rext_tree_expanded;
		src.row_toggle = rext_tree_toggle;
		src.row_icon = rext_icon;
		src.row_text_color = rext_text_color;
		src.row_buttons = rext_buttons;
		src.row_button_icon = rext_button_icon;
		src.row_button_click = on_source_row_button;
		src.user = &bs;
		vgui_tree_set_row_source(tree, &src);

		check(vgui_tree_get_item_icon(tree, 100) == tex,
			"tree row ext: virtual icon from row_icon");
		check(vgui_tree_get_item_text_color(tree, 101) == 0xFF00FF00u,
			"tree row ext: virtual text color (row 1)");
		check(vgui_tree_get_item_buttons(tree, 101) == 2,
			"tree row ext: virtual button count (row 1)");
		check(vgui_tree_get_item_buttons(tree, 100) == 0,
			"tree row ext: virtual button count default");

		vgui_update(ui, 0.016f);
		float tx = 0.0f, ty = 0.0f, tw = 0.0f, th = 0.0f;
		vgui_widget_get_rect(tree, &tx, &ty, &tw, &th);
		(void)th;
		const float row_h = style.tree.row_height;
		const float row1_y = ty + style.tree.padding_y + row_h + row_h * 0.5f;
		const float bsize = std::max(10.0f, std::min(18.0f, row_h - 8.0f));
		// 51 行超出视口，滚动条可见，按钮向内避让。
		const float bar = style.panel.scrollbar_width + style.panel.scrollbar_inset
			+ style.panel.scrollbar_hit_padding + 2.0f;
		const float content_right = tx + tw - style.tree.padding_x;
		const float btn0_x = content_right - bar - 4.0f - bsize - (bsize + 4.0f);
		click_at(ui, btn0_x, row1_y);
		check(bs.source_clicks >= 1 && bs.last_button == 0,
			"tree row ext: virtual row_button_click fires");
		check(vgui_tree_get_selected_count(tree) == 0,
			"tree row ext: virtual button click does not select");

		vgui_tree_source_rows_changed(tree, 1, 1);
		check(vgui_tree_is_virtual(tree) == 1, "tree row ext: rows_changed keeps virtual mode");
		check(vgui_tree_get_node_count(tree) == kVirtualTreeChildRows,
			"tree row ext: rows_changed keeps row count");
	}
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

	// The keyboard cursor can be positioned explicitly (used by combo boxes)
	// and ignores separators.
	vgui_menu_open(menu, 100.0f, 100.0f);
	vgui_menu_set_cursor(menu, normal);
	check(vgui_menu_get_cursor(menu) == normal, "menu: set cursor");
	vgui_menu_set_cursor(menu, sep);
	check(vgui_menu_get_cursor(menu) == normal, "menu: separator cursor rejected");
	vgui_menu_close(menu);

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

static void test_menubar(vgui_context* ui, const vgui_style& style)
{
	vgui_widget* bar = vgui_widget_add(vgui_root(ui), VGUI_MENUBAR, nullptr);
	vgui_widget_set_pos(bar, 20.0f, 20.0f);
	vgui_widget_set_size(bar, 360.0f, 30.0f);
	check(vgui_widget_get_kind(bar) == VGUI_MENUBAR, "menubar: kind");
	check(vgui_menubar_get_count(bar) == 0, "menubar: starts empty");

	vgui_widget* file_menu = vgui_widget_create(ui, VGUI_MENU, nullptr);
	vgui_item_id save = vgui_menu_add_item(file_menu, "Save");
	vgui_widget* edit_menu = vgui_widget_create(ui, VGUI_MENU, nullptr);
	vgui_item_id undo = vgui_menu_add_item(edit_menu, "Undo");
	cb_state cb;
	vgui_menu_on_activate(file_menu, on_activate, &cb);
	vgui_menu_on_activate(edit_menu, on_activate, &cb);

	vgui_item_id file = vgui_menubar_add_menu(bar, "File", file_menu);
	vgui_item_id edit = vgui_menubar_add_menu(bar, "Edit", edit_menu);
	check(file && edit && file != edit, "menubar: add menus");
	check(vgui_menubar_get_count(bar) == 2, "menubar: item count");
	check(vgui_menubar_get_item_at(bar, 0) == file, "menubar: item at index");
	check(std::string(vgui_menubar_get_item_text(bar, file)) == "File", "menubar: title");
	check(vgui_menubar_get_menu(bar, file) == file_menu, "menubar: menu handle");
	check(vgui_widget_get_parent(file_menu) == nullptr, "menubar: popup stays detached");
	check(vgui_widget_get_parent(bar) == vgui_root(ui), "menubar: layout child");

	vgui_layout(ui);
	float fx = 0, fy = 0, fw = 0, fh = 0;
	check(vgui_menubar_get_item_rect(bar, file, &fx, &fy, &fw, &fh) == 1,
		"menubar: item rect");
	check(fw > 0.0f && fh > 0.0f && fx >= 20.0f && fy >= 20.0f,
		"menubar: item rect is absolute");
	check(vgui_menubar_item_at(bar, fx - 20.0f + fw * 0.5f, fy - 20.0f + fh * 0.5f) == file,
		"menubar: local hit test");
	check(vgui_menubar_item_at(bar, -10.0f, -10.0f) == 0, "menubar: outside hit test");

	// Clicking a title opens its popup below the bar.
	click_at(ui, fx + fw * 0.5f, fy + fh * 0.5f);
	check(vgui_menu_is_open(file_menu) == 1, "menubar: click opens menu");
	check(vgui_menubar_get_active(bar) == file, "menubar: click sets active title");
	check(vgui_context_get_open_menu(ui) == file_menu, "menubar: popup is the open menu");

	// Hovering another title switches menus without a click.
	float ex = 0, ey = 0, ew = 0, eh = 0;
	check(vgui_menubar_get_item_rect(bar, edit, &ex, &ey, &ew, &eh) == 1,
		"menubar: edit rect");
	vgui_input_mouse_move(ui, ex + ew * 0.5f, ey + eh * 0.5f);
	check(vgui_menu_is_open(edit_menu) == 1, "menubar: hover switches menu");
	check(vgui_menu_is_open(file_menu) == 0, "menubar: previous menu closes on hover");
	check(vgui_menubar_get_active(bar) == edit, "menubar: hover sets active title");

	// Clicking the active title toggles the menu closed.
	click_at(ui, ex + ew * 0.5f, ey + eh * 0.5f);
	check(vgui_menu_is_open(edit_menu) == 0, "menubar: click closes active menu");
	check(vgui_menubar_get_active(bar) == 0, "menubar: closed menu clears active");

	// Arrow keys walk the titles while a menu is open.
	check(vgui_menubar_open_item(bar, edit) == 1, "menubar: open item");
	check(vgui_menubar_get_active(bar) == edit, "menubar: open item sets active");
	int handled = vgui_input_key(ui, VGUI_KEY_RIGHT, 1, 0);
	vgui_input_key(ui, VGUI_KEY_RIGHT, 0, 0);
	check(handled == 1 && vgui_menu_is_open(file_menu), "menubar: right switches title");
	check(vgui_menubar_get_active(bar) == file, "menubar: right active title");
	vgui_input_key(ui, VGUI_KEY_LEFT, 1, 0);
	vgui_input_key(ui, VGUI_KEY_LEFT, 0, 0);
	check(vgui_menu_is_open(edit_menu), "menubar: left switches title");
	check(vgui_menubar_get_active(bar) == edit, "menubar: left active title");

	// Activating a menu item closes the chain and clears the active title.
	float mx = 0, my = 0, mw = 0, mh = 0;
	vgui_widget_get_rect(edit_menu, &mx, &my, &mw, &mh);
	click_at(ui, mx + 20.0f, my + style.menu.padding_y + style.menu.item_height * 0.5f);
	check(cb.activates == 1 && cb.last_activate == undo, "menubar: menu item activates");
	check(vgui_menubar_get_active(bar) == 0, "menubar: activation clears active");
	check(vgui_context_get_open_menu(ui) == nullptr, "menubar: activation closes popup");
	(void)save;
	(void)mw;
	(void)mh;

	// Escape closes and clears the active title.
	check(vgui_menubar_open_item(bar, file) == 1, "menubar: reopen item");
	vgui_input_key(ui, VGUI_KEY_ESCAPE, 1, 0);
	vgui_input_key(ui, VGUI_KEY_ESCAPE, 0, 0);
	check(vgui_menu_is_open(file_menu) == 0, "menubar: escape closes");
	check(vgui_menubar_get_active(bar) == 0, "menubar: escape clears active");

	// Disabled titles cannot open.
	vgui_menubar_set_item_enabled(bar, edit, 0);
	check(vgui_menubar_get_item_enabled(bar, edit) == 0, "menubar: disable item");
	check(vgui_menubar_open_item(bar, edit) == 0, "menubar: disabled item does not open");
	vgui_menubar_set_item_enabled(bar, edit, 1);

	// An item created without a menu still owns an empty popup.
	vgui_item_id auto_item = vgui_menubar_add_item(bar, "Auto");
	check(auto_item != 0 && vgui_menubar_get_menu(bar, auto_item) != nullptr,
		"menubar: add item creates a menu");
	check(vgui_menubar_remove_item(bar, auto_item) == 1, "menubar: remove item");
	check(vgui_menubar_get_count(bar) == 2, "menubar: remove updates count");

	// A popup can only belong to one bar item at a time.
	vgui_widget* temp_menu = vgui_widget_create(ui, VGUI_MENU, nullptr);
	vgui_menu_add_item(temp_menu, "Temp Item");
	vgui_item_id temp = vgui_menubar_add_menu(bar, "Temp", temp_menu);
	check(temp != 0 && vgui_menubar_get_count(bar) == 3, "menubar: add explicit menu");
	check(vgui_menubar_add_menu(bar, "Temp2", temp_menu) == 0,
		"menubar: popup cannot be attached twice");

	// Destroying an attached popup removes its title and clears the active state.
	check(vgui_menubar_open_item(bar, temp) == 1, "menubar: open temp menu");
	vgui_widget_destroy(temp_menu);
	vgui_update(ui, 0.016f);
	check(vgui_menubar_get_count(bar) == 2, "menubar: destroyed popup detaches title");
	check(vgui_menubar_get_active(bar) == 0, "menubar: destroyed popup clears active");

	check(vgui_menubar_remove_item(bar, file) == 1, "menubar: remove file menu");
	check(vgui_menubar_get_count(bar) == 1, "menubar: remove file count");
	check(vgui_menubar_get_menu(bar, file) == nullptr, "menubar: removed menu handle is gone");

	vgui_menubar_clear(bar);
	check(vgui_menubar_get_count(bar) == 0, "menubar: clear removes items");
	vgui_widget_destroy(bar);
}

struct key_filter_state
{
	int downs = 0;
	int ups = 0;
	int order[4] = {};
	int order_count = 0;
	int last_key = 0;
};

static int on_key_filter_a(vgui_context*, int key, int down, int, void* user)
{
	key_filter_state* st = (key_filter_state*)user;
	st->order[st->order_count++] = 1;
	if (down) st->downs++;
	else st->ups++;
	st->last_key = key;
	return 0;
}

static int on_key_filter_b(vgui_context*, int key, int down, int, void* user)
{
	key_filter_state* st = (key_filter_state*)user;
	st->order[st->order_count++] = 2;
	if (down) st->downs++;
	else st->ups++;
	st->last_key = key;
	return 1;
}

static void test_key_handlers(vgui_context* ui)
{
	vgui_set_focus(ui, nullptr);
	check(vgui_context_wants_keyboard(ui) == 0, "key handler: no handler, no focus");

	key_filter_state st;
	int token_a = vgui_context_add_key_handler(ui, on_key_filter_a, &st);
	check(token_a > 0, "key handler: register");
	check(vgui_context_wants_keyboard(ui) == 1, "key handler: requests keyboard");

	int handled = vgui_input_key(ui, 'J', 1, 0);
	vgui_input_key(ui, 'J', 0, 0);
	check(handled == 0, "key handler: unconsumed key passes through");
	check(st.downs == 1 && st.ups == 1, "key handler: sees down and up");
	check(st.last_key == 'J', "key handler: receives key code");

	int token_b = vgui_context_add_key_handler(ui, on_key_filter_b, &st);
	check(token_b > 0 && token_b != token_a, "key handler: second token");
	st.order_count = 0;
	handled = vgui_input_key(ui, 'K', 1, VGUI_MOD_CTRL);
	vgui_input_key(ui, 'K', 0, VGUI_MOD_CTRL);
	check(handled == 1, "key handler: last registered consumes");
	check(st.order_count >= 1 && st.order[0] == 2, "key handler: reverse registration order");

	vgui_context_remove_key_handler(ui, token_b);
	st.order_count = 0;
	handled = vgui_input_key(ui, 'K', 1, VGUI_MOD_CTRL);
	vgui_input_key(ui, 'K', 0, VGUI_MOD_CTRL);
	check(handled == 0 && st.order_count >= 1 && st.order[0] == 1,
		"key handler: removal restores previous handler");

	vgui_context_remove_key_handler(ui, token_a);
	vgui_context_remove_key_handler(ui, token_a);
	check(vgui_context_wants_keyboard(ui) == 0, "key handler: removal drops keyboard request");
	handled = vgui_input_key(ui, 'J', 1, 0);
	check(handled == 0, "key handler: no handlers after removal");
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

struct scroll_probe
{
	int calls = 0;
	float x = 0.0f;
	float y = 0.0f;
};

static void on_panel_scroll(vgui_widget* widget, float x, float y, void* user)
{
	(void)widget;
	auto* probe = static_cast<scroll_probe*>(user);
	if (!probe) return;
	probe->calls++;
	probe->x = x;
	probe->y = y;
}

static void test_scroll_listener(vgui_context* ui)
{
	vgui_widget* panel = vgui_widget_add(vgui_root(ui), VGUI_PANEL, nullptr);
	vgui_widget_set_pos(panel, 20.0f, 20.0f);
	vgui_widget_set_size(panel, 200.0f, 100.0f);
	vgui_panel_set_scrollable(panel, 0, 1);
	vgui_widget* content = vgui_widget_add(panel, VGUI_PANEL, nullptr);
	vgui_widget_set_size(content, 200.0f, 400.0f);
	vgui_layout(ui);

	scroll_probe probe;
	vgui_panel_set_scroll_listener(panel, on_panel_scroll, &probe);
	vgui_panel_set_scroll(panel, 0.0f, 30.0f);
	check(probe.calls == 1 && near(probe.x, 0.0f) && near(probe.y, 30.0f),
		"scroll listener: programmatic scroll");

	vgui_panel_scroll_by(panel, 0.0f, 10.0f);
	check(probe.calls == 2 && near(probe.y, 40.0f), "scroll listener: scroll_by");

	vgui_input_mouse_move(ui, 120.0f, 70.0f);
	vgui_input_mouse_wheel(ui, 0.0f, -1.0f);
	check(probe.calls == 3 && probe.y > 40.0f, "scroll listener: wheel");

	vgui_panel_set_scroll_listener(panel, nullptr, nullptr);
	vgui_panel_set_scroll(panel, 0.0f, 0.0f);
	check(probe.calls == 3, "scroll listener: removal");

	vgui_widget_destroy(panel);
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

/* ------------------------------- tooltip --------------------------------- */

struct tooltip_virtual_rows
{
	int count = 4;
};

static int tip_rows_count(void* user)
{
	return ((tooltip_virtual_rows*)user)->count;
}

static int tip_rows_info(void* user, int index, const char** text, const char** detail,
	void** item_user)
{
	(void)user;
	static char buf[32];
	std::snprintf(buf, sizeof(buf), "row %d", index);
	if (text) *text = buf;
	if (detail) *detail = nullptr;
	if (item_user) *item_user = nullptr;
	return 1;
}

static vgui_item_id tip_rows_id(void* user, int index)
{
	(void)user;
	return (vgui_item_id)(index + 1);
}

static int tip_rows_index(void* user, vgui_item_id item)
{
	(void)user;
	return item >= 1 ? (int)item - 1 : -1;
}

static const char* tip_rows_tooltip(void* user, int index)
{
	(void)user;
	static char buf[40];
	std::snprintf(buf, sizeof(buf), "virtual row help %d", index);
	return buf;
}

static int g_tooltip_provider_calls = 0;

static const char* tooltip_provider(vgui_widget*, void* user)
{
	g_tooltip_provider_calls++;
	return (const char*)user;
}

static void test_tooltip(vgui_context* ui, const vgui_style& base)
{
	vgui_style style = base;
	style.tooltip.delay = 0.2f;
	vgui_context_set_style(ui, &style);

	vgui_widget* tip = vgui_widget_add(vgui_root(ui), VGUI_BUTTON, "Tip");
	vgui_widget_set_pos(tip, 560.0f, 600.0f);
	vgui_widget_set_size(tip, 120.0f, 30.0f);
	vgui_widget_set_tooltip(tip, "Static help");
	check(std::string(vgui_widget_get_tooltip(tip)) == "Static help",
		"tooltip: static text stored");

	vgui_layout(ui);
	float tx = 0, ty = 0, tw = 0, th = 0;
	vgui_widget_get_rect(tip, &tx, &ty, &tw, &th);
	(void)tw;
	(void)th;

	// Delay: hovering does not show immediately, update() starts the timer.
	vgui_input_mouse_move(ui, tx + 30.0f, ty + 15.0f);
	vgui_update(ui, 0.1f);
	check(vgui_context_tooltip_visible(ui) == 0,
		"tooltip: hidden before the hover delay elapses");
	vgui_update(ui, 0.15f);
	check(vgui_context_tooltip_visible(ui) == 1,
		"tooltip: appears after the hover delay");
	check(std::string(vgui_context_tooltip_text(ui)) == "Static help",
		"tooltip: reports its text");
	check(vgui_context_tooltip_owner(ui) == tip,
		"tooltip: reports its owner widget");

	float rx = 0, ry = 0, rw = 0, rh = 0;
	check(vgui_context_tooltip_rect(ui, &rx, &ry, &rw, &rh) == 1,
		"tooltip: rect is available while visible");
	check(near(rx, tx + 30.0f + style.tooltip.pointer_offset_x) &&
		near(ry, ty + 15.0f + style.tooltip.pointer_offset_y),
		"tooltip: pointer anchor uses the configured offset");
	check(rw > 0.0f && rh > 0.0f, "tooltip: measured rect is non-empty");

	// Leaving the widget hides the overlay immediately.
	vgui_input_mouse_move(ui, 10.0f, 690.0f);
	check(vgui_context_tooltip_visible(ui) == 0,
		"tooltip: moving away hides immediately");

	// Provider callbacks take precedence over the static text.
	g_tooltip_provider_calls = 0;
	vgui_widget_set_tooltip_provider(tip, tooltip_provider, (void*)"Dynamic help");
	vgui_input_mouse_move(ui, tx + 30.0f, ty + 15.0f);
	vgui_update(ui, 0.25f);
	check(vgui_context_tooltip_visible(ui) &&
		std::string(vgui_context_tooltip_text(ui)) == "Dynamic help",
		"tooltip: provider text is used");
	check(g_tooltip_provider_calls > 0, "tooltip: provider is invoked");
	vgui_widget_set_tooltip_provider(tip, nullptr, nullptr);

	// Keyboard input dismisses the overlay but pointer movement re-arms it.
	vgui_input_key(ui, 'A', 1, 0);
	vgui_input_key(ui, 'A', 0, 0);
	check(vgui_context_tooltip_visible(ui) == 0,
		"tooltip: key input hides the overlay");
	vgui_input_mouse_move(ui, tx + 40.0f, ty + 15.0f);
	vgui_update(ui, 0.25f);
	check(vgui_context_tooltip_visible(ui) == 1,
		"tooltip: pointer movement re-arms after input");

	// Explicit hide cancels the hover until the pointer moves again.
	vgui_context_hide_tooltip(ui);
	check(vgui_context_tooltip_visible(ui) == 0,
		"tooltip: explicit hide");
	vgui_update(ui, 0.3f);
	check(vgui_context_tooltip_visible(ui) == 0,
		"tooltip: explicit hide is not undone by update");

	// Disabled widgets show tooltips by default, and can opt out.
	vgui_widget_set_enabled(tip, 0);
	vgui_input_mouse_move(ui, 10.0f, 690.0f);
	vgui_input_mouse_move(ui, tx + 30.0f, ty + 15.0f);
	vgui_update(ui, 0.25f);
	check(vgui_context_tooltip_visible(ui) == 1,
		"tooltip: disabled widget shows help by default");
	vgui_widget_set_tooltip_when_disabled(tip, 0);
	vgui_context_hide_tooltip(ui);
	vgui_input_mouse_move(ui, 10.0f, 690.0f);
	vgui_input_mouse_move(ui, tx + 30.0f, ty + 15.0f);
	vgui_update(ui, 0.25f);
	check(vgui_context_tooltip_visible(ui) == 0,
		"tooltip: disabled widget can suppress help");
	vgui_widget_set_enabled(tip, 1);
	vgui_widget_set_tooltip_when_disabled(tip, 1);

	// Widget anchor: centred below the control.
	vgui_widget_set_tooltip_anchor(tip, VGUI_TOOLTIP_ANCHOR_WIDGET);
	vgui_input_mouse_move(ui, 10.0f, 690.0f);
	vgui_input_mouse_move(ui, tx + 30.0f, ty + 15.0f);
	vgui_update(ui, 0.25f);
	check(vgui_context_tooltip_rect(ui, &rx, &ry, &rw, &rh) == 1,
		"tooltip: widget anchor visible");
	check(ry >= ty + th + style.tooltip.widget_gap - 0.1f,
		"tooltip: widget anchor sits below the control");
	check(near(rx, tx + (120.0f - rw) * 0.5f),
		"tooltip: widget anchor is horizontally centred");

	// A control near the bottom edge flips the tooltip above itself.
	vgui_widget* low = vgui_widget_add(vgui_root(ui), VGUI_BUTTON, "Low");
	vgui_widget_set_pos(low, 560.0f, 655.0f);
	vgui_widget_set_size(low, 120.0f, 26.0f);
	vgui_widget_set_tooltip(low, "Low help");
	vgui_widget_set_tooltip_anchor(low, VGUI_TOOLTIP_ANCHOR_WIDGET);
	vgui_layout(ui);
	float lx = 0, ly = 0, lw = 0, lh = 0;
	vgui_widget_get_rect(low, &lx, &ly, &lw, &lh);
	vgui_input_mouse_move(ui, lx + 40.0f, ly + 13.0f);
	vgui_update(ui, 0.25f);
	check(vgui_context_tooltip_rect(ui, &rx, &ry, &rw, &rh) == 1,
		"tooltip: edge anchor visible");
	check(ry + rh <= 700.0f - style.tooltip.margin + 0.01f,
		"tooltip: edge anchor stays inside the context");
	check(ry < ly, "tooltip: edge anchor flips above the control");

	// Pointer anchor clamps at the right/bottom context edge.
	vgui_widget_set_tooltip(vgui_root(ui), "Edge help");
	vgui_input_mouse_move(ui, 895.0f, 695.0f);
	vgui_update(ui, 0.25f);
	check(vgui_context_tooltip_rect(ui, &rx, &ry, &rw, &rh) == 1,
		"tooltip: edge pointer tooltip visible");
	check(rx >= style.tooltip.margin - 0.01f &&
		ry >= style.tooltip.margin - 0.01f &&
		rx + rw <= 900.0f - style.tooltip.margin + 0.01f &&
		ry + rh <= 700.0f - style.tooltip.margin + 0.01f,
		"tooltip: pointer tooltip is clamped inside the context");
	vgui_widget_set_tooltip(vgui_root(ui), nullptr);

	// Newlines produce multiple lines and a taller box.
	vgui_widget_set_tooltip_anchor(tip, VGUI_TOOLTIP_ANCHOR_POINTER);
	vgui_widget_set_tooltip(tip, "single line");
	vgui_input_mouse_move(ui, tx + 30.0f, ty + 15.0f);
	vgui_update(ui, 0.25f);
	check(vgui_context_tooltip_rect(ui, &rx, &ry, &rw, &rh) == 1,
		"tooltip: single-line tooltip visible");
	float single_h = rh;
	vgui_widget_set_tooltip(tip, "first line\nsecond line");
	check(vgui_context_tooltip_rect(ui, &rx, &ry, &rw, &rh) == 1,
		"tooltip: multiline tooltip visible");
	check(rh > single_h + style.tooltip.padding_y,
		"tooltip: newline increases the measured height");

	// Per-item list tooltips and local row hit testing.
	vgui_widget* list = vgui_widget_add(vgui_root(ui), VGUI_LIST, nullptr);
	vgui_widget_set_pos(list, 560.0f, 300.0f);
	vgui_widget_set_size(list, 220.0f, 120.0f);
	vgui_item_id item0 = vgui_list_add_item(list, "One", nullptr);
	vgui_item_id item1 = vgui_list_add_item(list, "Two", nullptr);
	vgui_list_set_item_tooltip(list, item1, "Item help");
	check(std::string(vgui_list_get_item_tooltip(list, item1)) == "Item help",
		"tooltip: list item tooltip stored");
	vgui_layout(ui);
	float lx0 = 0, ly0 = 0, lw0 = 0, lh0 = 0;
	vgui_widget_get_rect(list, &lx0, &ly0, &lw0, &lh0);
	(void)lw0;
	(void)lh0;
	float row_h = style.list.row_height;
	float y_item1 = style.list.padding_y + row_h * 1.5f;
	check(vgui_list_item_at_point(list, 30.0f, y_item1) == item1,
		"tooltip: list item_at_point resolves a row");
	check(vgui_list_item_at_point(list, 30.0f, style.list.padding_y * 0.5f) == 0,
		"tooltip: list item_at_point rejects padding");
	vgui_input_mouse_move(ui, lx0 + 30.0f, ly0 + y_item1);
	vgui_update(ui, 0.25f);
	check(vgui_context_tooltip_visible(ui) &&
		std::string(vgui_context_tooltip_text(ui)) == "Item help" &&
		vgui_context_tooltip_owner(ui) == list,
		"tooltip: list row tooltip is used");
	vgui_input_mouse_move(ui, lx0 + 30.0f, ly0 + style.list.padding_y + row_h * 0.5f);
	vgui_update(ui, 0.25f);
	check(vgui_context_tooltip_visible(ui) == 0,
		"tooltip: rows without tooltip stay quiet");
	(void)item0;

	// Virtual rows provide tooltips through row_tooltip.
	tooltip_virtual_rows rows;
	vgui_row_source src = {};
	src.row_count = tip_rows_count;
	src.row_info = tip_rows_info;
	src.row_id = tip_rows_id;
	src.row_id_index = tip_rows_index;
	src.row_tooltip = tip_rows_tooltip;
	src.user = &rows;
	vgui_widget* vlist = vgui_widget_add(vgui_root(ui), VGUI_LIST, nullptr);
	vgui_widget_set_pos(vlist, 10.0f, 510.0f);
	vgui_widget_set_size(vlist, 240.0f, 96.0f);
	vgui_list_set_row_source(vlist, &src);
	vgui_layout(ui);
	float vx = 0, vy = 0, vw = 0, vh = 0;
	vgui_widget_get_rect(vlist, &vx, &vy, &vw, &vh);
	(void)vw;
	(void)vh;
	float vy1 = style.list.padding_y + row_h * 1.5f;
	check(vgui_list_item_at_point(vlist, 20.0f, vy1) == 2,
		"tooltip: virtual item_at_point returns the row id");
	vgui_input_mouse_move(ui, vx + 20.0f, vy + vy1);
	vgui_update(ui, 0.25f);
	check(vgui_context_tooltip_visible(ui) &&
		std::string(vgui_context_tooltip_text(ui)) == "virtual row help 1",
		"tooltip: virtual row_tooltip is used");

	// Tree nodes reuse the same mechanism.
	vgui_widget* tree = vgui_widget_add(vgui_root(ui), VGUI_TREE, nullptr);
	vgui_widget_set_pos(tree, 600.0f, 20.0f);
	vgui_widget_set_size(tree, 240.0f, 120.0f);
	vgui_item_id root = vgui_tree_add_node(tree, 0, "Root", nullptr);
	vgui_item_id child = vgui_tree_add_node(tree, root, "Child", nullptr);
	vgui_tree_set_item_tooltip(tree, child, "Node help");
	check(std::string(vgui_tree_get_item_tooltip(tree, child)) == "Node help",
		"tooltip: tree node tooltip stored");
	vgui_tree_expand(tree, root);
	vgui_layout(ui);
	float qx = 0, qy = 0, qw = 0, qh = 0;
	vgui_widget_get_rect(tree, &qx, &qy, &qw, &qh);
	(void)qw;
	(void)qh;
	float node_y = style.tree.padding_y + style.tree.row_height * 1.5f;
	check(vgui_tree_item_at_point(tree, 30.0f, node_y) == child,
		"tooltip: tree item_at_point resolves a visible row");
	vgui_input_mouse_move(ui, qx + 30.0f, qy + node_y);
	vgui_update(ui, 0.25f);
	check(vgui_context_tooltip_visible(ui) &&
		std::string(vgui_context_tooltip_text(ui)) == "Node help" &&
		vgui_context_tooltip_owner(ui) == tree,
		"tooltip: tree node tooltip is used");

	// Destroying the owner drops the overlay and its references.
	vgui_widget* doomed = vgui_widget_add(vgui_root(ui), VGUI_BUTTON, "Doomed");
	vgui_widget_set_pos(doomed, 800.0f, 600.0f);
	vgui_widget_set_size(doomed, 80.0f, 24.0f);
	vgui_widget_set_tooltip(doomed, "Gone soon");
	vgui_layout(ui);
	float dx = 0, dy = 0, dw = 0, dh = 0;
	vgui_widget_get_rect(doomed, &dx, &dy, &dw, &dh);
	(void)dw;
	(void)dh;
	vgui_input_mouse_move(ui, dx + 20.0f, dy + 12.0f);
	vgui_update(ui, 0.25f);
	check(vgui_context_tooltip_visible(ui) == 1,
		"tooltip: doomed owner tooltip visible");
	vgui_widget_destroy(doomed);
	check(vgui_context_tooltip_visible(ui) == 0,
		"tooltip: destroying the owner hides the overlay");

	// The context switch disables all overlay behaviour.
	vgui_input_mouse_move(ui, tx + 30.0f, ty + 15.0f);
	vgui_update(ui, 0.25f);
	check(vgui_context_tooltip_visible(ui) == 1,
		"tooltip: visible before disabling the context");
	vgui_context_set_tooltip_enabled(ui, 0);
	check(vgui_context_tooltip_visible(ui) == 0,
		"tooltip: context switch hides immediately");
	vgui_input_mouse_move(ui, 10.0f, 690.0f);
	vgui_input_mouse_move(ui, tx + 30.0f, ty + 15.0f);
	vgui_update(ui, 0.25f);
	check(vgui_context_tooltip_visible(ui) == 0,
		"tooltip: context switch prevents new tooltips");
	vgui_context_set_tooltip_enabled(ui, 1);
}

struct modal_cb
{
	int closes = 0;
	int result = -1;
	vgui_widget* panel = nullptr;
	int active_at_close = -1;
	int panel_rect_valid = 0;
};

static void on_modal_close(vgui_context* ctx, vgui_widget* panel, int result,
	void* user)
{
	modal_cb* s = (modal_cb*)user;
	s->closes++;
	s->result = result;
	s->panel = panel;
	s->active_at_close = vgui_modal_active(ctx);
	if (panel)
	{
		float x = 0, y = 0, w = 0, h = 0;
		vgui_widget_get_rect(panel, &x, &y, &w, &h);
		s->panel_rect_valid = (w > 0.0f && h > 0.0f);
	}
}

static void test_modal(vgui_context* ui)
{
	vgui_modal_close(ui, 0);
	vgui_set_focus(ui, nullptr);
	vgui_toast_clear(ui);
	vgui_update(ui, 0.016f);

	int bg_clicks = 0;
	vgui_widget* bg = vgui_widget_add(vgui_root(ui), VGUI_BUTTON, "Background");
	vgui_widget_set_id(bg, "modal-bg");
	vgui_widget_set_pos(bg, 20.0f, 20.0f);
	vgui_widget_set_size(bg, 140.0f, 32.0f);
	vgui_widget_on_event(bg, on_button_click, &bg_clicks);
	vgui_layout(ui);
	vgui_set_focus(ui, bg);
	check(vgui_get_focus(ui) == bg, "modal: background focus before show");

	check(vgui_modal_show(ui, nullptr, nullptr) == 0, "modal: rejects null panel");
	check(vgui_modal_show(ui, vgui_root(ui), nullptr) == 0, "modal: rejects the root");

	int ok_clicks = 0;
	vgui_widget* panel = vgui_widget_create(ui, VGUI_PANEL, nullptr);
	vgui_widget_set_id(panel, "modal-panel");
	vgui_widget_set_size(panel, 320.0f, 180.0f);
	vgui_widget_set_background(panel, 0xFF202020);
	vgui_widget* ok = vgui_widget_add(panel, VGUI_BUTTON, "OK");
	vgui_widget_set_pos(ok, 20.0f, 20.0f);
	vgui_widget_set_size(ok, 90.0f, 30.0f);
	vgui_widget_on_event(ok, on_button_click_only, &ok_clicks);
	vgui_widget* other = vgui_widget_add(panel, VGUI_BUTTON, "Other");
	vgui_widget_set_pos(other, 20.0f, 70.0f);
	vgui_widget_set_size(other, 90.0f, 30.0f);

	vgui_modal_options opts;
	vgui_modal_options_default(&opts);
	check(opts.close_on_escape == 1 && opts.close_on_backdrop == 0 &&
		opts.dim_background == 1 && opts.destroy_on_close == 0 &&
		opts.margin < 0.0f && opts.backdrop_alpha < 0.0f,
		"modal: option defaults");

	modal_cb mc;
	opts.on_close = on_modal_close;
	opts.user = &mc;
	check(vgui_modal_show(ui, panel, &opts) == 1, "modal: show");
	check(vgui_modal_active(ui) == 1, "modal: active after show");
	check(vgui_modal_panel(ui) == panel, "modal: panel accessor");
	check(vgui_modal_result(ui) == VGUI_MODAL_RESULT_NONE,
		"modal: result reset on show");
	check(vgui_get_focus(ui) == ok, "modal: first focusable receives focus");
	check(vgui_modal_show(ui, panel, nullptr) == 0,
		"modal: rejects a second modal");

	vgui_layout(ui);
	float px = 0, py = 0, pw = 0, ph = 0;
	vgui_widget_get_rect(panel, &px, &py, &pw, &ph);
	check(near(pw, 320.0f) && near(ph, 180.0f),
		"modal: panel keeps its requested size");
	check(near(px, (900.0f - 320.0f) * 0.5f) &&
		near(py, (700.0f - 180.0f) * 0.5f),
		"modal: panel is centred in the context");

	// The background tree is inert while the modal is open.
	click_at(ui, 90.0f, 36.0f);
	check(bg_clicks == 0, "modal: background click is blocked");
	check(vgui_get_focus(ui) == ok, "modal: background click cannot move focus");
	check(vgui_modal_active(ui) == 1,
		"modal: backdrop click does not close by default");

	vgui_set_focus(ui, bg);
	check(vgui_get_focus(ui) == ok,
		"modal: public focus rejects background widgets");

	// Tab cannot escape the modal panel.
	vgui_input_key(ui, VGUI_KEY_TAB, 1, 0);
	check(vgui_get_focus(ui) == other, "modal: Tab moves to the next control");
	vgui_input_key(ui, VGUI_KEY_TAB, 1, 0);
	check(vgui_get_focus(ui) == ok, "modal: Tab wraps inside the modal");
	vgui_input_key(ui, VGUI_KEY_TAB, 1, VGUI_MOD_SHIFT);
	check(vgui_get_focus(ui) == other, "modal: Shift+Tab reverses inside the modal");

	float bx = 0, by = 0, bw = 0, bh = 0;
	vgui_widget_get_rect(ok, &bx, &by, &bw, &bh);
	click_at(ui, bx + bw * 0.5f, by + bh * 0.5f);
	check(ok_clicks == 1, "modal: modal control receives clicks");
	check(vgui_modal_active(ui) == 1, "modal: control click keeps the modal open");

	// Context key handlers are background input and stay bypassed.
	key_filter_state ks = {};
	int token = vgui_context_add_key_handler(ui, on_key_filter_a, &ks);
	vgui_input_key(ui, 'A', 1, 0);
	vgui_input_key(ui, 'A', 0, 0);
	check(ks.downs == 0 && ks.ups == 0, "modal: background key handlers are blocked");
	vgui_context_remove_key_handler(ui, token);

	check(vgui_input_key(ui, VGUI_KEY_ESCAPE, 1, 0) == 1, "modal: Esc is consumed");
	check(vgui_modal_active(ui) == 0, "modal: Esc closes");
	check(vgui_modal_result(ui) == VGUI_MODAL_RESULT_CANCEL, "modal: Esc result");
	check(mc.closes == 1 && mc.result == VGUI_MODAL_RESULT_CANCEL,
		"modal: close callback result");
	check(mc.panel == panel && mc.active_at_close == 0,
		"modal: callback sees a detached panel after deactivation");
	check(mc.panel_rect_valid == 1, "modal: callback panel is still valid");
	check(vgui_get_focus(ui) == bg, "modal: focus restored after close");
	vgui_update(ui, 0.016f);
	vgui_widget_destroy(panel);

	// Backdrop closing, a custom result and disabled Esc.
	vgui_widget* panel2 = vgui_widget_create(ui, VGUI_PANEL, nullptr);
	vgui_widget_set_size(panel2, 200.0f, 100.0f);
	vgui_widget* one = vgui_widget_add(panel2, VGUI_BUTTON, "One");
	vgui_widget_set_size(one, 80.0f, 28.0f);
	modal_cb mc2;
	vgui_modal_options o2;
	vgui_modal_options_default(&o2);
	o2.close_on_escape = 0;
	o2.close_on_backdrop = 1;
	o2.backdrop_result = 7;
	o2.on_close = on_modal_close;
	o2.user = &mc2;
	check(vgui_modal_show(ui, panel2, &o2) == 1, "modal: second modal show");
	check(vgui_input_key(ui, VGUI_KEY_ESCAPE, 1, 0) == 1,
		"modal: Esc is consumed when closing is disabled");
	check(vgui_modal_active(ui) == 1,
		"modal: close_on_escape=0 keeps the modal open");
	click_at(ui, 5.0f, 5.0f);
	check(vgui_modal_active(ui) == 0, "modal: backdrop click closes when enabled");
	check(vgui_modal_result(ui) == 7 && mc2.result == 7, "modal: backdrop result");
	check(vgui_get_focus(ui) == bg, "modal: focus restored after backdrop close");
	vgui_update(ui, 0.016f);
	vgui_widget_destroy(panel2);

	// destroy_on_close keeps focus out of the panel and removes it afterwards.
	vgui_widget* panel3 = vgui_widget_create(ui, VGUI_PANEL, nullptr);
	vgui_widget_set_size(panel3, 200.0f, 100.0f);
	vgui_widget* bye = vgui_widget_add(panel3, VGUI_BUTTON, "Bye");
	vgui_widget_set_size(bye, 80.0f, 28.0f);
	modal_cb mc3;
	vgui_modal_options o3;
	vgui_modal_options_default(&o3);
	o3.destroy_on_close = 1;
	o3.on_close = on_modal_close;
	o3.user = &mc3;
	check(vgui_modal_show(ui, panel3, &o3) == 1, "modal: destroy-on-close show");
	vgui_modal_close(ui, VGUI_MODAL_RESULT_CONFIRM);
	check(vgui_modal_active(ui) == 0, "modal: explicit close");
	check(vgui_modal_result(ui) == VGUI_MODAL_RESULT_CONFIRM,
		"modal: explicit close result");
	check(mc3.closes == 1 && mc3.result == VGUI_MODAL_RESULT_CONFIRM,
		"modal: explicit close callback");
	check(vgui_get_focus(ui) == bg,
		"modal: focus restored after destroy-on-close");
	vgui_update(ui, 0.016f);
	check(vgui_modal_panel(ui) == nullptr, "modal: panel accessor empty after close");

	int before = vgui_modal_result(ui);
	vgui_modal_close(ui, VGUI_MODAL_RESULT_CANCEL);
	check(vgui_modal_result(ui) == before,
		"modal: closing without a modal is a no-op");

	vgui_widget_destroy(bg);
	vgui_set_focus(ui, nullptr);
	vgui_update(ui, 0.016f);
}

struct dialog_cb
{
	int closes = 0;
	int result = -1;
	std::string text;
	int active_at_close = -1;
};

static void on_dialog_result(vgui_context* ctx, int result, const char* text,
	void* user)
{
	dialog_cb* s = (dialog_cb*)user;
	s->closes++;
	s->result = result;
	s->text = text ? text : "";
	s->active_at_close = vgui_modal_active(ctx);
}

static vgui_widget* dialog_button_row(vgui_widget* panel)
{
	if (!panel) return nullptr;
	size_t count = vgui_widget_get_child_count(panel);
	if (count == 0) return nullptr;
	return vgui_widget_get_child(panel, count - 1);
}

static void click_widget(vgui_context* ui, vgui_widget* widget)
{
	if (!ui || !widget) return;
	float x = 0, y = 0, w = 0, h = 0;
	vgui_widget_get_rect(widget, &x, &y, &w, &h);
	click_at(ui, x + w * 0.5f, y + h * 0.5f);
}

static void test_dialog(vgui_context* ui)
{
	vgui_modal_close(ui, 0);
	vgui_set_focus(ui, nullptr);
	vgui_update(ui, 0.016f);

	vgui_dialog_desc desc;
	vgui_dialog_desc_default(&desc);
	check(desc.kind == VGUI_DIALOG_ALERT && desc.danger == 0 &&
		near(desc.width, 0.0f), "dialog: desc defaults");

	// Alert: one button, Enter/click confirms, Esc cancels.
	dialog_cb dc;
	vgui_widget* alert = vgui_dialog_alert(ui, "Alert", "Something happened",
		on_dialog_result, &dc);
	check(alert != nullptr && vgui_modal_active(ui) == 1, "dialog: alert opens");
	check(vgui_dialog_input(ui) == nullptr, "dialog: alert has no input");
	vgui_widget* focus = vgui_get_focus(ui);
	check(focus && vgui_widget_get_kind(focus) == VGUI_BUTTON,
		"dialog: alert focuses a button");
	check(std::string(vgui_widget_get_text(focus)) == "OK",
		"dialog: alert confirm text");
	check(vgui_dialog_alert(ui, "Again", "Nested", nullptr, nullptr) == nullptr,
		"dialog: rejects a second dialog");

	vgui_layout(ui);
	vgui_widget* row = dialog_button_row(alert);
	check(row != nullptr && vgui_widget_get_child_count(row) == 1,
		"dialog: alert has one button");
	vgui_widget* confirm = vgui_widget_get_child(row, 0);
	click_widget(ui, confirm);
	check(vgui_modal_active(ui) == 0, "dialog: alert confirm closes");
	check(dc.closes == 1 && dc.result == VGUI_MODAL_RESULT_CONFIRM,
		"dialog: alert confirm result");
	check(dc.text.empty(), "dialog: alert result text is empty");
	check(dc.active_at_close == 0, "dialog: result callback runs after deactivation");
	vgui_update(ui, 0.016f);

	dc = dialog_cb();
	vgui_widget* alert2 = vgui_dialog_alert(ui, "Alert", "Esc closes",
		on_dialog_result, &dc);
	check(alert2 != nullptr, "dialog: alert reopen");
	check(vgui_input_key(ui, VGUI_KEY_ESCAPE, 1, 0) == 1,
		"dialog: Esc is consumed");
	check(vgui_modal_active(ui) == 0 && dc.result == VGUI_MODAL_RESULT_CANCEL,
		"dialog: Esc cancels");
	check(dc.closes == 1 && dc.text.empty(), "dialog: Esc callback text");
	vgui_update(ui, 0.016f);

	// Confirm: Cancel and OK, Enter confirms the default button.
	dc = dialog_cb();
	vgui_widget* confirm_panel = vgui_dialog_confirm(ui, "Delete",
		"Are you sure?", 1, on_dialog_result, &dc);
	check(confirm_panel != nullptr && vgui_modal_active(ui) == 1,
		"dialog: confirm opens");
	focus = vgui_get_focus(ui);
	check(focus && vgui_widget_get_kind(focus) == VGUI_BUTTON &&
		std::string(vgui_widget_get_text(focus)) == "OK",
		"dialog: confirm focuses OK");
	vgui_layout(ui);
	row = dialog_button_row(confirm_panel);
	check(row != nullptr && vgui_widget_get_child_count(row) == 2,
		"dialog: confirm has cancel and OK");
	vgui_widget* cancel = vgui_widget_get_child(row, 0);
	check(std::string(vgui_widget_get_text(cancel)) == "Cancel",
		"dialog: cancel text");
	click_widget(ui, cancel);
	check(vgui_modal_active(ui) == 0 && dc.result == VGUI_MODAL_RESULT_CANCEL,
		"dialog: cancel button result");
	check(dc.text.empty(), "dialog: confirm callback text is empty");
	vgui_update(ui, 0.016f);

	dc = dialog_cb();
	vgui_widget* confirm2 = vgui_dialog_confirm(ui, "Go", "Continue?", 0,
		on_dialog_result, &dc);
	check(confirm2 != nullptr, "dialog: confirm reopen");
	check(vgui_input_key(ui, VGUI_KEY_ENTER, 1, 0) == 1,
		"dialog: Enter is consumed");
	check(vgui_modal_active(ui) == 0 && dc.result == VGUI_MODAL_RESULT_CONFIRM,
		"dialog: Enter confirms");
	vgui_update(ui, 0.016f);

	// Prompt: edit accessor, initial text, typing and the result string.
	dc = dialog_cb();
	vgui_widget* prompt = vgui_dialog_prompt(ui, "Name", "Enter a name",
		"Bob", on_dialog_result, &dc);
	check(prompt != nullptr && vgui_modal_active(ui) == 1, "dialog: prompt opens");
	vgui_widget* input = vgui_dialog_input(ui);
	check(input && vgui_widget_get_kind(input) == VGUI_EDIT,
		"dialog: prompt input accessor");
	check(std::string(vgui_edit_get_text(input)) == "Bob",
		"dialog: prompt initial text");
	check(vgui_get_focus(ui) == input, "dialog: prompt focuses the edit box");
	vgui_edit_set_text(input, "Alice");
	check(vgui_input_text(ui, "X") == 1, "dialog: prompt accepts typed text");
	check(std::string(vgui_edit_get_text(input)) == "AliceX",
		"dialog: prompt stores typed text");
	check(vgui_input_key(ui, VGUI_KEY_ENTER, 1, 0) == 1,
		"dialog: prompt Enter is consumed");
	check(vgui_modal_active(ui) == 0, "dialog: prompt Enter closes");
	check(dc.closes == 1 && dc.result == VGUI_MODAL_RESULT_CONFIRM &&
		dc.text == "AliceX", "dialog: prompt result text");
	check(vgui_dialog_input(ui) == nullptr,
		"dialog: input accessor is empty after close");
	vgui_update(ui, 0.016f);

	dc = dialog_cb();
	prompt = vgui_dialog_prompt(ui, "Name", "Cancel keeps text", "Bob",
		on_dialog_result, &dc);
	input = vgui_dialog_input(ui);
	vgui_edit_set_text(input, "Carol");
	vgui_layout(ui);
	row = dialog_button_row(prompt);
	cancel = vgui_widget_get_child(row, 0);
	click_widget(ui, cancel);
	check(vgui_modal_active(ui) == 0 && dc.result == VGUI_MODAL_RESULT_CANCEL,
		"dialog: prompt cancel result");
	check(dc.text == "Carol", "dialog: prompt cancel reports the text");
	vgui_update(ui, 0.016f);

	// A small requested width is clamped to the style minimum.
	vgui_dialog_desc narrow;
	vgui_dialog_desc_default(&narrow);
	narrow.kind = VGUI_DIALOG_ALERT;
	narrow.title = "Narrow";
	narrow.message = "Body";
	narrow.width = 40.0f;
	vgui_widget* narrow_panel = vgui_dialog_show(ui, &narrow);
	check(narrow_panel != nullptr, "dialog: custom width opens");
	vgui_layout(ui);
	float nx = 0, ny = 0, nw = 0, nh = 0;
	vgui_widget_get_rect(narrow_panel, &nx, &ny, &nw, &nh);
	check(nw >= 219.0f, "dialog: width clamps to the style minimum");
	vgui_modal_close(ui, VGUI_MODAL_RESULT_CANCEL);
	vgui_update(ui, 0.016f);

	vgui_set_focus(ui, nullptr);
}

static void test_toast(vgui_context* ui, const vgui_style& base)
{
	vgui_toast_clear(ui);
	vgui_modal_close(ui, 0);
	vgui_update(ui, 0.016f);

	vgui_toast_options def;
	vgui_toast_options_default(&def);
	check(def.position == VGUI_TOAST_TOP_RIGHT &&
		def.level == VGUI_TOAST_INFO && def.duration < 0.0f,
		"toast: option defaults");

	vgui_style style = base;
	style.toast.duration = 0.5f;
	style.toast.max_visible = 2;
	style.toast.margin = 10.0f;
	style.toast.gap = 6.0f;
	style.toast.max_width = 300.0f;
	vgui_context_set_style(ui, &style);

	check(vgui_toast_show(ui, nullptr, nullptr) == 0,
		"toast: rejects null text");
	check(vgui_toast_show(ui, "", nullptr) == 0, "toast: rejects empty text");
	check(vgui_toast_count(nullptr) == 0, "toast: null context count");
	check(vgui_toast_get_rect(nullptr, 1, nullptr, nullptr, nullptr, nullptr) == 0,
		"toast: null context rect");

	vgui_toast_id first = vgui_toast_show(ui, "Saved", nullptr);
	check(first != 0, "toast: show returns an id");
	check(vgui_toast_count(ui) == 1, "toast: count after show");

	float x = 0, y = 0, w = 0, h = 0;
	check(vgui_toast_get_rect(ui, first, &x, &y, &w, &h) == 1,
		"toast: rect available");
	check(w > 0.0f && h > 0.0f, "toast: rect is non-empty");
	check(near(x + w, 900.0f - 10.0f) && near(y, 10.0f),
		"toast: top-right placement");

	vgui_toast_options sticky_opts;
	vgui_toast_options_default(&sticky_opts);
	sticky_opts.duration = 0.0f;
	sticky_opts.position = VGUI_TOAST_BOTTOM_LEFT;
	sticky_opts.level = VGUI_TOAST_SUCCESS;
	vgui_toast_id sticky = vgui_toast_show(ui, "Sticky", &sticky_opts);
	check(sticky != 0 && vgui_toast_count(ui) == 2, "toast: sticky toast added");
	float sx = 0, sy = 0, sw = 0, sh = 0;
	check(vgui_toast_get_rect(ui, sticky, &sx, &sy, &sw, &sh) == 1,
		"toast: sticky rect available");
	check(near(sx, 10.0f) && near(sy + sh, 700.0f - 10.0f),
		"toast: bottom-left placement");

	vgui_update(ui, 0.2f);
	check(vgui_toast_count(ui) == 2, "toast: not expired before duration");
	vgui_update(ui, 0.4f);
	check(vgui_toast_count(ui) == 1, "toast: expires after duration");
	check(vgui_toast_get_rect(ui, first, nullptr, nullptr, nullptr, nullptr) == 0,
		"toast: expired id has no rect");
	check(vgui_toast_get_rect(ui, sticky, nullptr, nullptr, nullptr, nullptr) == 1,
		"toast: sticky toast survives updates");
	vgui_update(ui, 10.0f);
	check(vgui_toast_count(ui) == 1, "toast: duration 0 never expires");
	check(vgui_toast_dismiss(ui, sticky) == 1, "toast: dismiss");
	check(vgui_toast_count(ui) == 0, "toast: count after dismiss");
	check(vgui_toast_dismiss(ui, sticky) == 0, "toast: dismiss unknown id");

	// Every corner and centre anchor.
	vgui_toast_options o;
	vgui_toast_options_default(&o);
	o.duration = 0.0f;
	o.position = VGUI_TOAST_TOP_LEFT;
	vgui_toast_id tl = vgui_toast_show(ui, "TL", &o);
	vgui_toast_get_rect(ui, tl, &x, &y, &w, &h);
	check(near(x, 10.0f) && near(y, 10.0f), "toast: top-left placement");
	vgui_toast_clear(ui);

	o.position = VGUI_TOAST_TOP_CENTER;
	vgui_toast_id tc = vgui_toast_show(ui, "TC", &o);
	vgui_toast_get_rect(ui, tc, &x, &y, &w, &h);
	check(near(x, (900.0f - w) * 0.5f) && near(y, 10.0f),
		"toast: top-center placement");
	vgui_toast_clear(ui);

	o.position = VGUI_TOAST_BOTTOM_RIGHT;
	vgui_toast_id br = vgui_toast_show(ui, "BR", &o);
	vgui_toast_get_rect(ui, br, &x, &y, &w, &h);
	check(near(x + w, 900.0f - 10.0f) && near(y + h, 700.0f - 10.0f),
		"toast: bottom-right placement");
	vgui_toast_clear(ui);

	o.position = VGUI_TOAST_BOTTOM_CENTER;
	vgui_toast_id bc = vgui_toast_show(ui, "BC", &o);
	vgui_toast_get_rect(ui, bc, &x, &y, &w, &h);
	check(near(x, (900.0f - w) * 0.5f) && near(y + h, 700.0f - 10.0f),
		"toast: bottom-center placement");
	vgui_toast_clear(ui);

	// Same-corner toasts stack with the configured gap.
	o.position = VGUI_TOAST_TOP_RIGHT;
	vgui_toast_id t1 = vgui_toast_show(ui, "One", &o);
	vgui_toast_id t2 = vgui_toast_show(ui, "Two", &o);
	float x1 = 0, y1 = 0, w1 = 0, h1 = 0, x2 = 0, y2 = 0;
	vgui_toast_get_rect(ui, t1, &x1, &y1, &w1, &h1);
	vgui_toast_get_rect(ui, t2, &x2, &y2, nullptr, nullptr);
	check(near(x2, x1) && near(y2, y1 + h1 + 6.0f), "toast: stack with gap");
	vgui_toast_clear(ui);

	// max_visible hides extra toasts but they still age out.
	o.duration = 0.5f;
	vgui_toast_id m1 = vgui_toast_show(ui, "M1", &o);
	vgui_toast_id m2 = vgui_toast_show(ui, "M2", &o);
	vgui_toast_id m3 = vgui_toast_show(ui, "M3", &o);
	check(m1 && m2 && m3 && vgui_toast_count(ui) == 3,
		"toast: hidden toasts remain in the model");
	check(vgui_toast_get_rect(ui, m1, nullptr, nullptr, nullptr, nullptr) == 1 &&
		vgui_toast_get_rect(ui, m2, nullptr, nullptr, nullptr, nullptr) == 1,
		"toast: visible slots");
	check(vgui_toast_get_rect(ui, m3, nullptr, nullptr, nullptr, nullptr) == 0,
		"toast: max_visible hides extra toasts");
	vgui_update(ui, 0.6f);
	check(vgui_toast_count(ui) == 0, "toast: hidden toasts still expire");
	vgui_toast_clear(ui);

	// A legacy theme with zeroed toast colours falls back to the palette.
	vgui_theme saved_theme;
	vgui_context_get_theme(ui, &saved_theme);
	vgui_theme old = {};
	old.text = 0xFFEEEEEE;
	old.panel_bg = 0xFF202020;
	old.button_border = 0xFF555555;
	old.menu_bg = 0xFF282828;
	old.menu_border = 0xFF4A4A4A;
	old.accent = 0xFF409EFF;
	old.modal_danger = 0xFFD9544D;
	vgui_context_set_theme(ui, &old);
	vgui_theme got;
	vgui_context_get_theme(ui, &got);
	check(got.toast_bg == old.menu_bg, "toast: theme fallback background");
	check(got.toast_border == old.menu_border, "toast: theme fallback border");
	check(got.toast_text == old.text, "toast: theme fallback text");
	check(got.toast_info == old.accent, "toast: theme fallback info colour");
	check(got.toast_success != 0 && got.toast_warning != 0 &&
		got.toast_error == old.modal_danger,
		"toast: theme fallback level colours");
	vgui_toast_id themed = vgui_toast_show(ui, "Themed", nullptr);
	check(vgui_toast_get_rect(ui, themed, &x, &y, &w, &h) == 1 && w > 0.0f,
		"toast: layout works with a legacy theme");
	vgui_toast_clear(ui);
	vgui_context_set_theme(ui, &saved_theme);

	// A style from before the overlay controls existed restores both blocks.
	vgui_style zeroed = base;
	zeroed.modal = {};
	zeroed.toast = {};
	vgui_context_set_style(ui, &zeroed);
	vgui_style got_style;
	vgui_context_get_style(ui, &got_style);
	check(near(got_style.modal.dialog_width, 360.0f) &&
		near(got_style.modal.backdrop_alpha, 0.45f),
		"toast: zeroed modal style restores defaults");
	check(near(got_style.toast.duration, 3.0f) &&
		got_style.toast.max_visible == 3,
		"toast: zeroed toast style restores defaults");
	vgui_context_set_style(ui, &base);
	vgui_toast_clear(ui);
}

/* --------------------------------- dock ---------------------------------- */

struct dock_cb
{
	int activates = 0;
	int closes = 0;
	int changes = 0;
	vgui_item_id last_activate = 0;
	vgui_item_id last_close = 0;
};

static void on_dock_activate(vgui_widget*, vgui_item_id item, void* user)
{
	dock_cb* cb = (dock_cb*)user;
	cb->activates++;
	cb->last_activate = item;
}

static void on_dock_close(vgui_widget*, vgui_item_id item, void* user)
{
	dock_cb* cb = (dock_cb*)user;
	cb->closes++;
	cb->last_close = item;
}

static void on_dock_change(vgui_widget*, vgui_item_id, void* user)
{
	((dock_cb*)user)->changes++;
}

static void test_dock(vgui_context* ui, const vgui_style& base)
{
	vgui_style style = base;
	style.dock.left_width = 140.0f;
	style.dock.right_width = 140.0f;
	style.dock.bottom_height = 120.0f;
	style.dock.gap = 4.0f;
	style.dock.tab_height = 24.0f;
	style.dock.tab_padding = 8.0f;
	style.dock.tab_gap = 2.0f;
	style.dock.tab_min_width = 40.0f;
	style.dock.tab_max_width = 90.0f;
	style.dock.close_size = 12.0f;
	style.dock.floating_title_height = 24.0f;
	style.dock.floating_width = 240.0f;
	style.dock.floating_height = 160.0f;
	style.dock.floating_min_width = 120.0f;
	style.dock.floating_min_height = 80.0f;
	style.dock.floating_rounding = 4.0f;
	style.dock.floating_border_width = 1.0f;
	style.dock.shadow = 4.0f;
	style.dock.drag_threshold = 4.0f;
	vgui_context_set_style(ui, &style);

	vgui_widget* dock = vgui_widget_add(vgui_root(ui), VGUI_DOCK, nullptr);
	vgui_widget_set_pos(dock, 20.0f, 20.0f);
	vgui_widget_set_size(dock, 600.0f, 420.0f);

	vgui_dock_panel_desc desc;
	vgui_dock_panel_desc_default(&desc);
	check(desc.zone == VGUI_DOCK_CENTER && desc.index == -1 && desc.closeable == 1 &&
		desc.open == 1 && !desc.id && !desc.panel, "dock: descriptor defaults");

	vgui_widget* pa = vgui_widget_create(ui, VGUI_PANEL, nullptr);
	vgui_widget* pb = vgui_widget_create(ui, VGUI_PANEL, nullptr);
	vgui_widget* pr = vgui_widget_create(ui, VGUI_PANEL, nullptr);
	vgui_widget* pc = vgui_widget_create(ui, VGUI_PANEL, nullptr);
	vgui_widget* pm = vgui_widget_create(ui, VGUI_PANEL, nullptr);
	auto add = [&](const char* id, const char* title, vgui_dock_zone zone, vgui_widget* page)
		{
			vgui_dock_panel_desc d;
			vgui_dock_panel_desc_default(&d);
			d.id = id;
			d.title = title;
			d.zone = zone;
			d.panel = page;
			return vgui_dock_add(dock, &d);
		};
	vgui_item_id ia = add("a", "A", VGUI_DOCK_LEFT, pa);
	vgui_item_id ib = add("b", "B", VGUI_DOCK_LEFT, pb);
	vgui_item_id ir = add("r", "R", VGUI_DOCK_RIGHT, pr);
	vgui_item_id icons = add("c", "C", VGUI_DOCK_BOTTOM, pc);
	vgui_item_id im = add("m", "M", VGUI_DOCK_CENTER, pm);
	check(ia && ib && ir && icons && im && ia != ib && ib != ir && ir != icons &&
		icons != im, "dock: add assigns unique ids");
	check(vgui_dock_get_count(dock) == 5, "dock: count after add");
	check(vgui_dock_get_at(dock, 0) == ia && vgui_dock_get_at(dock, 4) == im,
		"dock: stable add order");
	check(vgui_dock_find(dock, "b") == ib, "dock: find by string id");
	check(std::string(vgui_dock_get_id(dock, ib)) == "b", "dock: string id query");
	check(std::string(vgui_dock_get_title(dock, ib)) == "B", "dock: title query");
	check(vgui_dock_get_panel(dock, ib) == pb, "dock: page query");
	check(vgui_widget_get_parent(pa) == dock, "dock: page is attached");
	check(vgui_dock_get_zone(dock, ia) == VGUI_DOCK_LEFT &&
		vgui_dock_get_zone(dock, ir) == VGUI_DOCK_RIGHT &&
		vgui_dock_get_zone(dock, icons) == VGUI_DOCK_BOTTOM &&
		vgui_dock_get_zone(dock, im) == VGUI_DOCK_CENTER,
		"dock: zone queries");
	check(vgui_dock_get_zone_count(dock, VGUI_DOCK_LEFT) == 2 &&
		vgui_dock_get_zone_at(dock, VGUI_DOCK_LEFT, 0) == ia &&
		vgui_dock_get_zone_at(dock, VGUI_DOCK_LEFT, 1) == ib,
		"dock: zone order");
	check(vgui_dock_get_index(dock, ia) == 0 && vgui_dock_get_index(dock, ib) == 1,
		"dock: index query");
	check(vgui_dock_get_active(dock, VGUI_DOCK_LEFT) == ib &&
		vgui_dock_get_active(dock, VGUI_DOCK_RIGHT) == ir &&
		vgui_dock_get_active(dock, VGUI_DOCK_BOTTOM) == icons &&
		vgui_dock_get_active(dock, VGUI_DOCK_CENTER) == im,
		"dock: last added panel is active");

	vgui_dock_panel_desc dup;
	vgui_dock_panel_desc_default(&dup);
	dup.panel = pa;
	check(vgui_dock_add(dock, &dup) == 0, "dock: duplicate page rejected");
	vgui_widget* temp = vgui_widget_create(ui, VGUI_PANEL, nullptr);
	dup.panel = temp;
	dup.id = "a";
	check(vgui_dock_add(dock, &dup) == 0, "dock: duplicate string id rejected");
	vgui_widget_destroy(temp);

	vgui_layout(ui);
	float x = 0, y = 0, w = 0, h = 0;
	check(vgui_dock_get_zone_rect(dock, VGUI_DOCK_LEFT, &x, &y, &w, &h) &&
		near(x, 20.0f) && near(y, 20.0f) && near(w, 140.0f) && near(h, 420.0f),
		"dock: left zone geometry");
	check(vgui_dock_get_zone_rect(dock, VGUI_DOCK_RIGHT, &x, &y, &w, &h) &&
		near(x, 480.0f) && near(y, 20.0f) && near(w, 140.0f) && near(h, 420.0f),
		"dock: right zone geometry");
	check(vgui_dock_get_zone_rect(dock, VGUI_DOCK_CENTER, &x, &y, &w, &h) &&
		near(x, 164.0f) && near(y, 20.0f) && near(w, 312.0f) && near(h, 296.0f),
		"dock: centre zone geometry");
	check(vgui_dock_get_zone_rect(dock, VGUI_DOCK_BOTTOM, &x, &y, &w, &h) &&
		near(x, 164.0f) && near(y, 320.0f) && near(w, 312.0f) && near(h, 120.0f),
		"dock: bottom zone geometry");
	check(vgui_widget_get_visible(pa) == 0 && vgui_widget_get_visible(pb) == 1,
		"dock: only active left page is visible");
	check(vgui_widget_get_visible(pr) == 1 && vgui_widget_get_visible(pc) == 1 &&
		vgui_widget_get_visible(pm) == 1, "dock: active pages in other zones are visible");
	vgui_widget_get_rect(pb, &x, &y, &w, &h);
	check(near(x, 20.0f) && near(y, 44.0f) && near(w, 140.0f) && near(h, 396.0f),
		"dock: active page fills zone content");
	float tx = 0, ty = 0, tw = 0, th = 0;
	check(vgui_dock_get_tab_rect(dock, ib, &tx, &ty, &tw, &th) && tw > 0.0f &&
		near(th, 24.0f), "dock: tab geometry");

	vgui_dock_set_zone_size(dock, VGUI_DOCK_LEFT, 160.0f);
	check(near(vgui_dock_get_zone_size(dock, VGUI_DOCK_LEFT), 160.0f),
		"dock: zone size override");
	vgui_layout(ui);
	check(vgui_dock_get_zone_rect(dock, VGUI_DOCK_LEFT, &x, &y, &w, &h) && near(w, 160.0f),
		"dock: overridden zone geometry");
	check(vgui_dock_get_zone_rect(dock, VGUI_DOCK_CENTER, &x, &y, &w, &h) &&
		near(x, 184.0f) && near(w, 292.0f), "dock: centre follows zone size");
	vgui_dock_set_zone_size(dock, VGUI_DOCK_LEFT, -1.0f);
	check(near(vgui_dock_get_zone_size(dock, VGUI_DOCK_LEFT), 140.0f),
		"dock: negative zone size restores default");
	vgui_layout(ui);

	dock_cb cb;
	vgui_dock_on_activate(dock, on_dock_activate, &cb);
	vgui_dock_on_close(dock, on_dock_close, &cb);
	vgui_dock_on_change(dock, on_dock_change, &cb);

	check(vgui_dock_activate(dock, ia) == 1 &&
		vgui_dock_get_active(dock, VGUI_DOCK_LEFT) == ia, "dock: activate selects panel");
	check(cb.activates == 1 && cb.last_activate == ia, "dock: activate callback");
	vgui_layout(ui);
	check(vgui_widget_get_visible(pa) == 1 && vgui_widget_get_visible(pb) == 0,
		"dock: activation switches visible page");

	vgui_layout(ui);
	check(vgui_dock_get_tab_rect(dock, ib, &tx, &ty, &tw, &th), "dock: tab rect for click");
	click_at(ui, tx + tw * 0.5f, ty + th * 0.5f);
	check(vgui_dock_get_active(dock, VGUI_DOCK_LEFT) == ib && cb.activates == 2,
		"dock: clicking a tab activates it");

	vgui_layout(ui);
	vgui_dock_get_tab_rect(dock, ib, &tx, &ty, &tw, &th);
	click_at(ui, tx + tw - 10.0f, ty + th * 0.5f);
	check(vgui_dock_is_open(dock, ib) == 0 && cb.closes == 1 && cb.last_close == ib,
		"dock: tab close glyph closes panel");
	check(vgui_dock_get_active(dock, VGUI_DOCK_LEFT) == ia,
		"dock: closing activates another zone panel");
	check(vgui_dock_get_zone_count(dock, VGUI_DOCK_LEFT) == 2 &&
		vgui_dock_get_index(dock, ib) == 1, "dock: closed panel stays in zone order");
	check(vgui_dock_get_tab_rect(dock, ib, &tx, &ty, &tw, &th) == 0,
		"dock: closed panel has no tab rect");
	vgui_layout(ui);
	check(vgui_widget_get_visible(pb) == 0, "dock: closed panel stays hidden");

	check(vgui_dock_restore(dock, ib) == 1 && vgui_dock_is_open(dock, ib) == 1 &&
		vgui_dock_get_active(dock, VGUI_DOCK_LEFT) == ib, "dock: restore reopens panel");
	check(cb.activates == 3, "dock: restore emits activate callback");
	vgui_layout(ui);
	check(vgui_widget_get_visible(pb) == 1, "dock: restored panel is visible");

	vgui_dock_set_title(dock, ib, "Bee");
	check(std::string(vgui_dock_get_title(dock, ib)) == "Bee", "dock: set title");
	vgui_dock_set_title(dock, ib, "B");

	vgui_dock_set_closeable(dock, ib, 0);
	check(vgui_dock_get_closeable(dock, ib) == 0, "dock: clear closeable");
	vgui_layout(ui);
	vgui_dock_get_tab_rect(dock, ib, &tx, &ty, &tw, &th);
	int closes_before = cb.closes;
	click_at(ui, tx + tw - 10.0f, ty + th * 0.5f);
	check(vgui_dock_is_open(dock, ib) == 1 && cb.closes == closes_before,
		"dock: non-closeable tab ignores close glyph");
	vgui_dock_set_closeable(dock, ib, 1);

	check(vgui_dock_close(dock, ib) == 1 && vgui_dock_is_open(dock, ib) == 0 &&
		cb.closes == closes_before + 1, "dock: API close");
	check(vgui_dock_restore(dock, ib) == 1 && vgui_dock_is_open(dock, ib) == 1,
		"dock: API restore");

	int activates_before = cb.activates;
	check(vgui_dock_set_zone(dock, ia, VGUI_DOCK_RIGHT, 0) == 1,
		"dock: move panel to another zone");
	check(vgui_dock_get_zone(dock, ia) == VGUI_DOCK_RIGHT &&
		vgui_dock_get_zone_count(dock, VGUI_DOCK_LEFT) == 1 &&
		vgui_dock_get_zone_count(dock, VGUI_DOCK_RIGHT) == 2,
		"dock: zone counts after move");
	check(vgui_dock_get_zone_at(dock, VGUI_DOCK_RIGHT, 0) == ia &&
		vgui_dock_get_active(dock, VGUI_DOCK_RIGHT) == ia,
		"dock: moved panel takes requested index and becomes active");
	check(cb.activates == activates_before + 1, "dock: move emits activate callback");
	check(vgui_dock_set_zone(dock, ia, VGUI_DOCK_LEFT, 1) == 1 &&
		vgui_dock_get_zone_at(dock, VGUI_DOCK_LEFT, 0) == ib &&
		vgui_dock_get_zone_at(dock, VGUI_DOCK_LEFT, 1) == ia &&
		vgui_dock_get_active(dock, VGUI_DOCK_LEFT) == ia,
		"dock: move back to requested index");

	vgui_layout(ui);
	float ax = 0, ay = 0, aw = 0, ah = 0;
	float bx = 0, by = 0, bw = 0, bh = 0;
	vgui_dock_get_tab_rect(dock, ia, &ax, &ay, &aw, &ah);
	vgui_dock_get_tab_rect(dock, ib, &bx, &by, &bw, &bh);
	vgui_input_mouse_move(ui, ax + aw * 0.5f, ay + ah * 0.5f);
	vgui_input_mouse_down(ui, 1, ax + aw * 0.5f, ay + ah * 0.5f, 1);
	vgui_input_mouse_move(ui, bx + bw * 0.25f, by + bh * 0.5f);
	vgui_input_mouse_up(ui, 1, bx + bw * 0.25f, by + bh * 0.5f, 1);
	check(vgui_dock_get_zone_at(dock, VGUI_DOCK_LEFT, 0) == ia &&
		vgui_dock_get_zone_at(dock, VGUI_DOCK_LEFT, 1) == ib,
		"dock: dragging a tab reorders within its zone");

	activates_before = cb.activates;
	check(vgui_dock_float(dock, ir, 100.0f, 100.0f, 240.0f, 160.0f) == 1,
		"dock: float panel");
	check(vgui_dock_is_floating(dock, ir) == 1 &&
		vgui_dock_get_zone(dock, ir) == VGUI_DOCK_FLOATING &&
		vgui_dock_get_zone_count(dock, VGUI_DOCK_RIGHT) == 0 &&
		vgui_dock_get_zone_count(dock, VGUI_DOCK_FLOATING) == 1,
		"dock: floating panel leaves its zone");
	check(cb.activates == activates_before + 1, "dock: float emits activate callback");
	float fx = 0, fy = 0, fw = 0, fh = 0;
	check(vgui_dock_get_floating_rect(dock, ir, &fx, &fy, &fw, &fh) &&
		near(fx, 100.0f) && near(fy, 100.0f) && near(fw, 240.0f) && near(fh, 160.0f),
		"dock: floating rect");
	vgui_layout(ui);
	vgui_widget_get_rect(pr, &x, &y, &w, &h);
	check(near(x, 100.0f) && near(y, 124.0f) && near(w, 240.0f) && near(h, 136.0f),
		"dock: floating page content rect");

	vgui_dock_set_floating_rect(dock, ir, -100.0f, -100.0f, 240.0f, 160.0f);
	vgui_dock_get_floating_rect(dock, ir, &fx, &fy, &fw, &fh);
	check(near(fx, 20.0f) && near(fy, 20.0f), "dock: floating rect is clamped to dock");
	vgui_dock_set_floating_rect(dock, ir, 100.0f, 100.0f, 240.0f, 160.0f);

	check(vgui_dock_float(dock, icons, 0.0f, 0.0f, 0.0f, 0.0f) == 1,
		"dock: float with default size");
	vgui_dock_get_floating_rect(dock, icons, &fx, &fy, &fw, &fh);
	check(near(fx, 200.0f) && near(fy, 150.0f) && near(fw, 240.0f) && near(fh, 160.0f),
		"dock: origin centres default floating frame");
	check(vgui_dock_get_zone_count(dock, VGUI_DOCK_BOTTOM) == 0 &&
		vgui_dock_get_zone_count(dock, VGUI_DOCK_FLOATING) == 2,
		"dock: multiple floating panels");

	vgui_dock_activate(dock, ir);
	check(vgui_dock_get_zone_at(dock, VGUI_DOCK_FLOATING, 1) == ir &&
		vgui_dock_get_active(dock, VGUI_DOCK_FLOATING) == ir,
		"dock: activating floating panel raises it");

	// Restoring a closed frame that is not topmost must raise it again.
	vgui_dock_activate(dock, icons);
	check(vgui_dock_get_zone_at(dock, VGUI_DOCK_FLOATING, 1) == icons,
		"dock: activating the other floating panel raises it");
	vgui_dock_activate(dock, ir);
	check(vgui_dock_close(dock, icons) == 1 && vgui_dock_is_open(dock, icons) == 0,
		"dock: close lower floating panel");
	check(vgui_dock_restore(dock, icons) == 1 &&
		vgui_dock_get_zone_at(dock, VGUI_DOCK_FLOATING, 1) == icons &&
		vgui_dock_get_active(dock, VGUI_DOCK_FLOATING) == icons,
		"dock: restoring a floating panel raises it");
	vgui_dock_activate(dock, ir);

	vgui_layout(ui);
	vgui_dock_get_floating_rect(dock, ir, &fx, &fy, &fw, &fh);
	closes_before = cb.closes;
	click_at(ui, fx + fw - 10.0f, fy + 12.0f);
	check(vgui_dock_is_open(dock, ir) == 0 && cb.closes == closes_before + 1,
		"dock: floating close glyph closes panel");
	check(vgui_dock_get_zone_count(dock, VGUI_DOCK_FLOATING) == 2 &&
		vgui_dock_get_tab_rect(dock, ir, &tx, &ty, &tw, &th) == 0,
		"dock: closed floating panel stays in float order");
	check(vgui_dock_restore(dock, ir) == 1 && vgui_dock_is_open(dock, ir) == 1 &&
		vgui_dock_get_active(dock, VGUI_DOCK_FLOATING) == ir,
		"dock: restore floating panel");

	vgui_layout(ui);
	vgui_dock_get_floating_rect(dock, ir, &fx, &fy, &fw, &fh);
	vgui_input_mouse_move(ui, fx + 30.0f, fy + 10.0f);
	vgui_input_mouse_down(ui, 1, fx + 30.0f, fy + 10.0f, 1);
	vgui_input_mouse_move(ui, fx + 60.0f, fy + 40.0f);
	vgui_input_mouse_up(ui, 1, fx + 60.0f, fy + 40.0f, 1);
	vgui_dock_get_floating_rect(dock, ir, &fx, &fy, &fw, &fh);
	check(near(fx, 130.0f) && near(fy, 130.0f),
		"dock: dragging the floating title moves the frame");

	vgui_layout(ui);
	vgui_dock_get_floating_rect(dock, ir, &fx, &fy, &fw, &fh);
	vgui_input_mouse_move(ui, fx + 30.0f, fy + 10.0f);
	vgui_input_mouse_down(ui, 1, fx + 30.0f, fy + 10.0f, 1);
	vgui_input_mouse_move(ui, 155.0f, 32.0f);
	vgui_input_mouse_up(ui, 1, 155.0f, 32.0f, 1);
	check(vgui_dock_is_floating(dock, ir) == 0 &&
		vgui_dock_get_zone(dock, ir) == VGUI_DOCK_LEFT &&
		vgui_dock_get_zone_count(dock, VGUI_DOCK_LEFT) == 3,
		"dock: dragging a frame to a tab bar redocks it");
	check(vgui_dock_get_active(dock, VGUI_DOCK_LEFT) == ir &&
		vgui_dock_get_zone_at(dock, VGUI_DOCK_LEFT, 2) == ir,
		"dock: redocked panel is active at the drop index");

	// Persistence round trip: zone order and floating geometry are public state.
	float saved_fx = 0, saved_fy = 0, saved_fw = 0, saved_fh = 0;
	vgui_dock_get_floating_rect(dock, icons, &saved_fx, &saved_fy, &saved_fw, &saved_fh);
	int saved_index = vgui_dock_get_index(dock, ir);
	vgui_widget* dock2 = vgui_widget_add(vgui_root(ui), VGUI_DOCK, nullptr);
	vgui_widget_set_pos(dock2, 20.0f, 20.0f);
	vgui_widget_set_size(dock2, 600.0f, 420.0f);
	auto add2 = [&](const char* id, const char* title, vgui_dock_zone zone, vgui_widget* page)
		{
			vgui_dock_panel_desc d;
			vgui_dock_panel_desc_default(&d);
			d.id = id;
			d.title = title;
			d.zone = zone;
			d.panel = page;
			return vgui_dock_add(dock2, &d);
		};
	vgui_item_id jb = add2("b", "B", VGUI_DOCK_LEFT, vgui_widget_create(ui, VGUI_PANEL, nullptr));
	vgui_item_id ja = add2("a", "A", VGUI_DOCK_LEFT, vgui_widget_create(ui, VGUI_PANEL, nullptr));
	vgui_item_id jr = add2("r", "R", VGUI_DOCK_LEFT, vgui_widget_create(ui, VGUI_PANEL, nullptr));
	vgui_dock_panel_desc fd;
	vgui_dock_panel_desc_default(&fd);
	fd.id = "c";
	fd.title = "C";
	fd.panel = vgui_widget_create(ui, VGUI_PANEL, nullptr);
	fd.zone = VGUI_DOCK_FLOATING;
	fd.x = saved_fx;
	fd.y = saved_fy;
	fd.width = saved_fw;
	fd.height = saved_fh;
	vgui_item_id jc = vgui_dock_add(dock2, &fd);
	check(jb && ja && jr && jc, "dock: recreated panels get ids");
	check(vgui_dock_get_zone_count(dock2, VGUI_DOCK_LEFT) == 3 &&
		vgui_dock_get_zone_at(dock2, VGUI_DOCK_LEFT, 2) == jr &&
		vgui_dock_get_index(dock2, jr) == saved_index,
		"dock: persistence restores zone order");
	check(vgui_dock_get_floating_rect(dock2, jc, &x, &y, &w, &h) &&
		near(x, saved_fx) && near(y, saved_fy) && near(w, saved_fw) && near(h, saved_fh),
		"dock: persistence restores floating rect");
	vgui_dock_clear(dock2);
	vgui_widget_destroy(dock2);

	// Collapse every side zone, then drop tabs near the dock edges.  Only
	// collapsed zones use positional inference.
	vgui_dock_float(dock, ia, 0.0f, 0.0f, 0.0f, 0.0f);
	vgui_dock_float(dock, ir, 300.0f, 160.0f, 0.0f, 0.0f);
	vgui_dock_set_zone(dock, ib, VGUI_DOCK_CENTER, 0);
	vgui_layout(ui);
	check(vgui_dock_get_zone_rect(dock, VGUI_DOCK_LEFT, &x, &y, &w, &h) == 0 &&
		vgui_dock_get_zone_rect(dock, VGUI_DOCK_RIGHT, &x, &y, &w, &h) == 0 &&
		vgui_dock_get_zone_rect(dock, VGUI_DOCK_BOTTOM, &x, &y, &w, &h) == 0,
		"dock: empty side zones collapse");

	vgui_layout(ui);
	vgui_dock_get_tab_rect(dock, ib, &tx, &ty, &tw, &th);
	vgui_input_mouse_move(ui, tx + tw * 0.5f, ty + th * 0.5f);
	vgui_input_mouse_down(ui, 1, tx + tw * 0.5f, ty + th * 0.5f, 1);
	vgui_input_mouse_move(ui, 30.0f, 200.0f);
	vgui_input_mouse_up(ui, 1, 30.0f, 200.0f, 1);
	check(vgui_dock_get_zone(dock, ib) == VGUI_DOCK_LEFT &&
		vgui_dock_get_zone_count(dock, VGUI_DOCK_LEFT) == 1,
		"dock: drop infers a collapsed left zone");

	vgui_layout(ui);
	vgui_dock_get_tab_rect(dock, ib, &tx, &ty, &tw, &th);
	vgui_input_mouse_move(ui, tx + tw * 0.5f, ty + th * 0.5f);
	vgui_input_mouse_down(ui, 1, tx + tw * 0.5f, ty + th * 0.5f, 1);
	vgui_input_mouse_move(ui, 610.0f, 200.0f);
	vgui_input_mouse_up(ui, 1, 610.0f, 200.0f, 1);
	check(vgui_dock_get_zone(dock, ib) == VGUI_DOCK_RIGHT &&
		vgui_dock_get_zone_count(dock, VGUI_DOCK_RIGHT) == 1,
		"dock: drop infers a collapsed right zone");

	vgui_layout(ui);
	vgui_dock_get_tab_rect(dock, ib, &tx, &ty, &tw, &th);
	vgui_input_mouse_move(ui, tx + tw * 0.5f, ty + th * 0.5f);
	vgui_input_mouse_down(ui, 1, tx + tw * 0.5f, ty + th * 0.5f, 1);
	vgui_input_mouse_move(ui, 300.0f, 410.0f);
	vgui_input_mouse_up(ui, 1, 300.0f, 410.0f, 1);
	check(vgui_dock_get_zone(dock, ib) == VGUI_DOCK_BOTTOM &&
		vgui_dock_get_zone_count(dock, VGUI_DOCK_BOTTOM) == 1,
		"dock: drop infers a collapsed bottom zone");

	// Wide tabs overflow the centre strip.  Wheel scrolling must survive the
	// next layout instead of snapping the active tab back into view.
	vgui_style wide = style;
	wide.dock.tab_min_width = 100.0f;
	wide.dock.tab_max_width = 100.0f;
	vgui_context_set_style(ui, &wide);
	vgui_item_id wheel_last = 0;
	for (int i = 0; i < 7; ++i)
	{
		char title[8];
		char sid[8];
		std::snprintf(title, sizeof(title), "T%d", i + 1);
		std::snprintf(sid, sizeof(sid), "t%d", i + 1);
		vgui_widget* page = vgui_widget_create(ui, VGUI_PANEL, nullptr);
		vgui_dock_panel_desc d;
		vgui_dock_panel_desc_default(&d);
		d.id = sid;
		d.title = title;
		d.panel = page;
		d.zone = VGUI_DOCK_CENTER;
		vgui_item_id added = vgui_dock_add(dock, &d);
		if (i == 6) wheel_last = added;
	}
	check(wheel_last != 0 && vgui_dock_get_zone_count(dock, VGUI_DOCK_CENTER) == 8,
		"dock: centre tabs overflow");
	vgui_layout(ui);
	vgui_dock_get_tab_rect(dock, wheel_last, &tx, &ty, &tw, &th);
	float before_wheel_x = tx;
	vgui_dock_get_zone_rect(dock, VGUI_DOCK_CENTER, &x, &y, &w, &h);
	vgui_input_mouse_move(ui, x + 10.0f, y + 12.0f);
	vgui_input_mouse_wheel(ui, 0.0f, 1.0f);
	vgui_layout(ui);
	vgui_dock_get_tab_rect(dock, wheel_last, &tx, &ty, &tw, &th);
	check(tx > before_wheel_x + 1.0f, "dock: wheel scrolls the tab strip");
	float wheel_x = tx;
	vgui_layout(ui);
	vgui_dock_get_tab_rect(dock, wheel_last, &tx, &ty, &tw, &th);
	check(near(tx, wheel_x), "dock: wheel scroll is not reset by layout");

	int count_before = vgui_dock_get_count(dock);
	vgui_widget* detached = vgui_dock_detach(dock, im);
	check(detached == pm && vgui_dock_get_count(dock) == count_before - 1,
		"dock: detach removes panel");
	check(vgui_widget_get_parent(pm) == nullptr && vgui_widget_get_visible(pm) == 1 &&
		vgui_dock_get_panel(dock, im) == nullptr && vgui_dock_find(dock, "m") == 0,
		"dock: detach returns page ownership");
	vgui_widget_destroy(pm);

	count_before = vgui_dock_get_count(dock);
	vgui_item_id first_center = vgui_dock_get_zone_at(dock, VGUI_DOCK_CENTER, 0);
	check(vgui_dock_remove(dock, first_center) == 1 &&
		vgui_dock_get_count(dock) == count_before - 1 &&
		vgui_dock_get_panel(dock, first_center) == nullptr,
		"dock: remove destroys panel");

	vgui_dock_clear(dock);
	check(vgui_dock_get_count(dock) == 0, "dock: clear removes all panels");
	check(vgui_dock_get_zone_count(dock, VGUI_DOCK_LEFT) == 0 &&
		vgui_dock_get_zone_count(dock, VGUI_DOCK_RIGHT) == 0 &&
		vgui_dock_get_zone_count(dock, VGUI_DOCK_BOTTOM) == 0 &&
		vgui_dock_get_zone_count(dock, VGUI_DOCK_CENTER) == 0 &&
		vgui_dock_get_zone_count(dock, VGUI_DOCK_FLOATING) == 0,
		"dock: clear resets zone orders");
	vgui_widget_destroy(dock);

	// A style block from before the dock existed restores every default.
	vgui_style zeroed = wide;
	zeroed.dock = {};
	vgui_context_set_style(ui, &zeroed);
	vgui_style got_style;
	vgui_context_get_style(ui, &got_style);
	check(near(got_style.dock.left_width, 200.0f) &&
		near(got_style.dock.right_width, 220.0f) &&
		near(got_style.dock.bottom_height, 180.0f) &&
		near(got_style.dock.tab_height, 30.0f) &&
		near(got_style.dock.floating_width, 360.0f) &&
		near(got_style.dock.floating_height, 260.0f) &&
		near(got_style.dock.drag_threshold, 4.0f),
		"dock: zeroed style restores defaults");

	// A legacy theme with zeroed dock colours falls back to the palette.
	vgui_theme saved_theme;
	vgui_context_get_theme(ui, &saved_theme);
	vgui_theme old = {};
	old.text = 0xFFEEEEEE;
	old.panel_bg = 0xFF202020;
	old.panel_border = 0xFF555555;
	old.button_bg = 0xFF303030;
	old.button_border = 0xFF555555;
	old.menu_bg = 0xFF282828;
	old.menu_border = 0xFF4A4A4A;
	vgui_context_set_theme(ui, &old);
	vgui_theme got_theme;
	vgui_context_get_theme(ui, &got_theme);
	check(got_theme.dock_bg == old.panel_bg &&
		got_theme.dock_border == old.panel_border &&
		got_theme.dock_tab_bg == old.button_bg &&
		got_theme.dock_tab_active == old.panel_bg &&
		got_theme.dock_tab_text == old.text &&
		got_theme.dock_floating_bg == old.menu_bg &&
		got_theme.dock_floating_border == old.menu_border &&
		got_theme.dock_floating_title == old.button_bg,
		"dock: legacy theme fallback colours");
	vgui_context_set_theme(ui, &saved_theme);
	vgui_context_set_style(ui, &base);
}

/* F03 generic drag & drop: row drag sources, drop target routing, accept /
 * reject feedback and the OS file drop bridge. */

struct drag_state
{
	int accepts = 0;
	int hovers = 0;
	int drops = 0;
	int drag_calls = 0;
	bool reject = false;
	std::string last_text;
	std::vector<std::string> last_files;
	vgui_drop_hint last_hint = {};
};

static int dd_can_accept(void* user, const vgui_drag_payload* payload)
{
	drag_state* s = (drag_state*)user;
	if (s->reject || !payload) return 0;
	if (payload->kind == VGUI_DRAG_TEXT || payload->kind == VGUI_DRAG_FILES)
	{
		s->accepts++;
		return 1;
	}
	return 0;
}

static void dd_hover(void* user, const vgui_drag_payload* payload, float x, float y,
	vgui_drop_hint* hint)
{
	(void)payload;
	(void)x;
	drag_state* s = (drag_state*)user;
	s->hovers++;
	if (hint)
	{
		hint->kind = VGUI_DROP_HINT_LINE;
		hint->x = 0.0f;
		hint->y = y;
		hint->w = 100.0f;
		hint->h = 2.0f;
		s->last_hint = *hint;
	}
}

static int dd_drop(void* user, const vgui_drag_payload* payload, float x, float y)
{
	(void)x;
	(void)y;
	drag_state* s = (drag_state*)user;
	s->drops++;
	if (payload)
	{
		s->last_text = payload->text ? payload->text : "";
		s->last_files.clear();
		for (int i = 0; i < payload->file_count; ++i)
		{
			if (payload->files && payload->files[i])
				s->last_files.push_back(payload->files[i]);
		}
	}
	return 1;
}

static int dd_row_drag(void* user, vgui_item_id item, int index, vgui_drag_payload* out)
{
	(void)item;
	(void)index;
	static const char* kDraggedText = "dragged row";
	((drag_state*)user)->drag_calls++;
	if (out)
	{
		out->kind = VGUI_DRAG_TEXT;
		out->text = kDraggedText;
	}
	return 1;
}

static void test_drag_drop(vgui_context* ui, const vgui_style& style)
{
	vgui_widget* list = vgui_widget_add(vgui_root(ui), VGUI_LIST, nullptr);
	vgui_widget_set_id(list, "drag-list");
	vgui_widget_set_pos(list, 10.0f, 10.0f);
	vgui_widget_set_size(list, 200.0f, 150.0f);
	vgui_list_add_item(list, "one", nullptr);
	vgui_list_add_item(list, "two", nullptr);
	vgui_list_add_item(list, "three", nullptr);

	drag_state ds;
	vgui_list_set_row_drag(list, dd_row_drag, &ds);

	vgui_widget* zone = vgui_widget_add(vgui_root(ui), VGUI_PANEL, nullptr);
	vgui_widget_set_id(zone, "drop-zone");
	vgui_widget_set_pos(zone, 400.0f, 10.0f);
	vgui_widget_set_size(zone, 200.0f, 150.0f);

	vgui_drop_target target = {};
	target.can_accept = dd_can_accept;
	target.hover = dd_hover;
	target.drop = dd_drop;
	target.user = &ds;
	vgui_widget_set_drop_target(zone, &target);

	vgui_update(ui, 0.016f);
	float lx = 0.0f, ly = 0.0f, lw = 0.0f, lh = 0.0f;
	vgui_widget_get_rect(list, &lx, &ly, &lw, &lh);
	(void)lw;
	(void)lh;
	float zx = 0.0f, zy = 0.0f, zw = 0.0f, zh = 0.0f;
	vgui_widget_get_rect(zone, &zx, &zy, &zw, &zh);
	const float row0_y = ly + style.list.padding_y + style.list.row_height * 0.5f;
	const float zone_cx = zx + zw * 0.5f;
	const float zone_cy = zy + zh * 0.5f;

	// Sub-threshold moves do not start a drag.
	vgui_input_mouse_move(ui, lx + 30.0f, row0_y);
	vgui_input_mouse_down(ui, 1, lx + 30.0f, row0_y, 1);
	vgui_input_mouse_move(ui, lx + 32.0f, row0_y + 2.0f);
	check(vgui_drag_active(ui) == 0 && ds.drag_calls == 0,
		"drag: threshold suppresses tiny moves");

	// Crossing the threshold starts the drag and routes the hover.
	vgui_input_mouse_move(ui, zone_cx, zone_cy);
	check(vgui_drag_active(ui) == 1 && ds.drag_calls == 1,
		"drag: row drag starts past the threshold");
	check(vgui_drag_hover_target(ui) == zone, "drag: hover target resolves");
	check(ds.accepts == 1 && ds.hovers >= 1, "drag: accept + hover callbacks");
	check(ds.last_hint.kind == VGUI_DROP_HINT_LINE, "drag: hover hint recorded");
	{
		vgui_drop_hint live = {};
		check(vgui_drag_hint(ui, &live) == 1 &&
			live.kind == ds.last_hint.kind && live.w == ds.last_hint.w &&
			live.x == ds.last_hint.x, "drag: live hint query");
		check(vgui_drag_hint(ui, nullptr) == 0, "drag: null hint rejected");
	}

	// Release delivers the payload and ends the drag.
	vgui_input_mouse_up(ui, 1, zone_cx, zone_cy, 1);
	check(ds.drops == 1 && ds.last_text == "dragged row",
		"drag: drop delivers the payload");
	check(vgui_drag_active(ui) == 0, "drag: release ends the drag");
	check(vgui_drag_hint(ui, &ds.last_hint) == 0,
		"drag: hint query clears after drop");

	// A rejected payload never reaches drop.
	ds.reject = true;
	vgui_input_mouse_move(ui, lx + 30.0f, row0_y);
	vgui_input_mouse_down(ui, 1, lx + 30.0f, row0_y, 1);
	vgui_input_mouse_move(ui, zone_cx, zone_cy);
	check(vgui_drag_active(ui) == 1 && vgui_drag_hover_target(ui) == nullptr,
		"drag: rejected payload has no hover target");
	vgui_input_mouse_up(ui, 1, zone_cx, zone_cy, 1);
	check(ds.drops == 1, "drag: rejected drop is not delivered");

	// begin/cancel via the C API.
	vgui_drag_payload payload = {};
	payload.kind = VGUI_DRAG_TEXT;
	payload.text = "manual";
	check(vgui_begin_drag(ui, list, &payload) == 1, "drag: begin_drag starts");
	check(vgui_drag_active(ui) == 1, "drag: manual drag is active");
	vgui_cancel_drag(ui);
	check(vgui_drag_active(ui) == 0, "drag: cancel ends the drag");
	payload.kind = VGUI_DRAG_NONE;
	check(vgui_begin_drag(ui, list, &payload) == 0, "drag: none payload rejected");

	// OS file drop bridge: one-shot hover + drop over the zone.
	ds.reject = false;
	const char* files[] = { "assets/texture.png", "assets/model.glb" };
	check(vgui_input_drop_files(ui, files, 2, zone_cx, zone_cy) == 1,
		"drag: file drop accepted");
	check(ds.last_files.size() == 2 && ds.last_files[1] == "assets/model.glb",
		"drag: file payload carried through");
	check(vgui_input_drop_files(ui, files, 2, 100.0f, 600.0f) == 0,
		"drag: file drop outside any target is refused");
	check(vgui_drag_active(ui) == 0, "drag: file drop leaves no drag state");
}

static void test_i18n_rtl(vgui_context* ui)
{
	// ---- string table + text-key bindings ----
	vgui_context_set_string(ui, "en", "greeting", "Hello");
	vgui_context_set_string(ui, "zh", "greeting", "你好");
	vgui_context_set_string(ui, "en", "fallback.only", "Fallback");
	vgui_context_set_language(ui, "en");
	vgui_widget* label = vgui_widget_add(vgui_root(ui), VGUI_LABEL, nullptr);
	vgui_widget_set_text_key(label, "greeting");
	check(std::string(vgui_widget_get_text(label)) == "Hello", "i18n: key uses the en default");
	check(std::string(vgui_widget_get_text_key(label)) == "greeting", "i18n: key is queryable");

	vgui_context_set_language(ui, "zh");
	check(std::string(vgui_widget_get_text(label)) == "你好",
		"i18n: language switch re-applies keys");
	vgui_context_set_language(ui, "ar");
	check(std::string(vgui_widget_get_text(label)) == "Hello",
		"i18n: missing language falls back to en");
	check(std::string(vgui_context_translate(ui, "fallback.only")) == "Fallback",
		"i18n: translate falls back to en");
	check(std::string(vgui_context_translate(ui, "no.such.key")) == "no.such.key",
		"i18n: unknown key returns itself");

	vgui_widget_set_text(label, "literal");
	vgui_context_set_language(ui, "zh");
	check(std::string(vgui_widget_get_text(label)) == "literal",
		"i18n: set_text clears the key binding");
	vgui_widget_set_text_key(label, "greeting");
	check(std::string(vgui_widget_get_text(label)) == "你好",
		"i18n: rebinding applies the active language");
	vgui_context_set_language(ui, "");
	check(std::string(vgui_widget_get_text(label)) == "greeting",
		"i18n: empty language shows the key verbatim");
	vgui_widget_destroy(label);

	// ---- RTL row: the first child moves to the right edge ----
	vgui_widget* row = vgui_widget_add(vgui_root(ui), VGUI_PANEL, nullptr);
	vgui_widget_set_layout(row, VGUI_LAYOUT_ROW);
	vgui_widget_set_pos(row, 10.0f, 10.0f);
	vgui_widget_set_size(row, 200.0f, 40.0f);
	vgui_widget_set_padding(row, 0, 0, 0, 0);
	vgui_widget_set_gap(row, 10.0f);
	vgui_widget* a = vgui_widget_add(row, VGUI_PANEL, nullptr);
	vgui_widget* b = vgui_widget_add(row, VGUI_PANEL, nullptr);
	vgui_widget* c = vgui_widget_add(row, VGUI_PANEL, nullptr);
	vgui_widget_set_size(a, 40, 20);
	vgui_widget_set_size(b, 60, 20);
	vgui_widget_set_size(c, 30, 20);

	vgui_context_set_direction(ui, VGUI_DIRECTION_LTR);
	check(vgui_context_get_direction(ui) == VGUI_DIRECTION_LTR,
		"rtl: context reports LTR");
	vgui_layout(ui);
	float ax = 0, bx = 0, cx = 0;
	vgui_widget_get_rect(a, &ax, nullptr, nullptr, nullptr);
	vgui_widget_get_rect(b, &bx, nullptr, nullptr, nullptr);
	vgui_widget_get_rect(c, &cx, nullptr, nullptr, nullptr);
	check(near(ax, 10) && near(bx, 60) && near(cx, 130), "rtl: LTR row order");

	vgui_context_set_direction(ui, VGUI_DIRECTION_RTL);
	check(vgui_context_get_direction(ui) == VGUI_DIRECTION_RTL,
		"rtl: context reports RTL");
	vgui_layout(ui);
	vgui_widget_get_rect(a, &ax, nullptr, nullptr, nullptr);
	vgui_widget_get_rect(b, &bx, nullptr, nullptr, nullptr);
	vgui_widget_get_rect(c, &cx, nullptr, nullptr, nullptr);
	check(near(ax, 170) && near(bx, 100) && near(cx, 60), "rtl: row is mirrored");

	// ---- column cross-axis alignment flips START/END ----
	vgui_widget* col = vgui_widget_add(vgui_root(ui), VGUI_PANEL, nullptr);
	vgui_widget_set_layout(col, VGUI_LAYOUT_COLUMN);
	vgui_widget_set_pos(col, 10.0f, 60.0f);
	vgui_widget_set_size(col, 200.0f, 100.0f);
	vgui_widget_set_padding(col, 0, 0, 0, 0);
	vgui_widget_set_align_items(col, VGUI_ALIGN_START);
	vgui_widget* c1 = vgui_widget_add(col, VGUI_PANEL, nullptr);
	vgui_widget_set_size(c1, 40, 20);
	vgui_layout(ui);
	float c1x = 0;
	vgui_widget_get_rect(c1, &c1x, nullptr, nullptr, nullptr);
	check(near(c1x, 170), "rtl: column START alignment moves to the right");

	// ---- wrap lines mirror their x positions ----
	vgui_widget* wrap = vgui_widget_add(vgui_root(ui), VGUI_PANEL, nullptr);
	vgui_widget_set_layout(wrap, VGUI_LAYOUT_WRAP);
	vgui_widget_set_pos(wrap, 10.0f, 200.0f);
	vgui_widget_set_size(wrap, 100.0f, 100.0f);
	vgui_widget_set_padding(wrap, 0, 0, 0, 0);
	vgui_widget_set_gap(wrap, 10.0f);
	vgui_widget* w1 = vgui_widget_add(wrap, VGUI_PANEL, nullptr);
	vgui_widget* w2 = vgui_widget_add(wrap, VGUI_PANEL, nullptr);
	vgui_widget_set_size(w1, 30, 20);
	vgui_widget_set_size(w2, 30, 20);
	vgui_layout(ui);
	float w1x = 0, w2x = 0;
	vgui_widget_get_rect(w1, &w1x, nullptr, nullptr, nullptr);
	vgui_widget_get_rect(w2, &w2x, nullptr, nullptr, nullptr);
	check(near(w1x, 80) && near(w2x, 40), "rtl: wrap line starts on the right");

	// ---- menubar item order ----
	vgui_widget* bar = vgui_widget_add(vgui_root(ui), VGUI_MENUBAR, nullptr);
	vgui_widget_set_pos(bar, 0.0f, 400.0f);
	vgui_widget_set_size(bar, 300.0f, 30.0f);
	vgui_item_id m1 = vgui_menubar_add_item(bar, "One");
	vgui_item_id m2 = vgui_menubar_add_item(bar, "Two");
	vgui_layout(ui);
	float m1x = 0, m1w = 0, m2x = 0, m2w = 0;
	vgui_menubar_get_item_rect(bar, m1, &m1x, nullptr, &m1w, nullptr);
	vgui_menubar_get_item_rect(bar, m2, &m2x, nullptr, &m2w, nullptr);
	check(m2x < m1x && near(m2x + m2w + 2.0f, m1x) && near(m1x + m1w, 296.0f),
		"rtl: menubar items start at the right");

	// ---- tab strip order ----
	vgui_widget* tabs = vgui_widget_add(vgui_root(ui), VGUI_TABVIEW, nullptr);
	vgui_widget_set_pos(tabs, 0.0f, 450.0f);
	vgui_widget_set_size(tabs, 400.0f, 120.0f);
	vgui_item_id ta = vgui_tab_add(tabs, "Alpha", vgui_widget_create(ui, VGUI_PANEL, nullptr));
	vgui_item_id tb = vgui_tab_add(tabs, "Beta", vgui_widget_create(ui, VGUI_PANEL, nullptr));
	vgui_layout(ui);
	float tax = 0, tbx = 0;
	vgui_tab_get_rect(tabs, ta, &tax, nullptr, nullptr, nullptr);
	vgui_tab_get_rect(tabs, tb, &tbx, nullptr, nullptr, nullptr);
	check(tax > tbx, "rtl: tabs start at the right");

	vgui_context_set_direction(ui, VGUI_DIRECTION_LTR);
	vgui_context_set_language(ui, "");
	vgui_widget_destroy(row);
	vgui_widget_destroy(col);
	vgui_widget_destroy(wrap);
	vgui_widget_destroy(bar);
	vgui_widget_destroy(tabs);
}

//----------------------------------------------------------------------------
// tree filter / match highlight / item rect / edit cancel / focus listener
//----------------------------------------------------------------------------

static bool contains_ci(const char* hay, const char* needle)
{
	if (!hay || !needle || !*needle) return false;
	const std::string h(hay), n(needle);
	for (size_t i = 0; i + n.size() <= h.size(); ++i)
	{
		size_t j = 0;
		auto lower = [](char c) { return (c >= 'A' && c <= 'Z') ? char(c + 32) : c; };
		while (j < n.size() && lower(h[i + j]) == lower(n[j])) ++j;
		if (j == n.size()) return true;
	}
	return false;
}

struct tree_filter_user
{
	const char* needle = "";
};

static int match_contains(vgui_widget* tree, vgui_item_id node, void* user)
{
	tree_filter_user* f = (tree_filter_user*)user;
	return contains_ci(vgui_tree_get_item_text(tree, node), f->needle) ? 1 : 0;
}

struct edit_event_counter
{
	int cancels = 0;
	int submits = 0;
};

static void on_edit_event(vgui_widget*, const vgui_event* e, void* user)
{
	edit_event_counter* c = (edit_event_counter*)user;
	if (e->type == VGUI_EVENT_CANCEL) c->cancels++;
	if (e->type == VGUI_EVENT_SUBMIT) c->submits++;
}

struct focus_change_counter
{
	int changes = 0;
	vgui_widget* last_old = nullptr;
	vgui_widget* last_new = nullptr;
};

static void on_focus_change(vgui_context*, vgui_widget* old_w, vgui_widget* new_w, void* user)
{
	focus_change_counter* c = (focus_change_counter*)user;
	c->changes++;
	c->last_old = old_w;
	c->last_new = new_w;
}

static void test_tree_filter_and_focus(vgui_context* ui, const vgui_style& style)
{
	vgui_widget* tree = vgui_widget_add(vgui_root(ui), VGUI_TREE, nullptr);
	vgui_widget_set_id(tree, "filter-tree");
	vgui_widget_set_pos(tree, 10.0f, 480.0f);
	vgui_widget_set_size(tree, 240.0f, 200.0f);

	vgui_item_id alpha = vgui_tree_add_node(tree, 0, "Alpha", nullptr);
	vgui_item_id beta = vgui_tree_add_node(tree, 0, "Beta", nullptr);
	vgui_item_id alpha_child = vgui_tree_add_node(tree, alpha, "alpha-child", nullptr);
	vgui_item_id beta_child = vgui_tree_add_node(tree, beta, "gamma", nullptr);
	check(alpha && beta && alpha_child && beta_child, "tree filter: nodes added");
	vgui_tree_set_item_expanded(tree, alpha, 1);
	// beta 保持折叠,gamma 不可见。

	vgui_tree_select(tree, beta, 1);
	check(vgui_tree_is_selected(tree, beta), "tree filter: selection set");

	tree_filter_user match;
	match.needle = "alp";
	vgui_tree_set_match_text(tree, "alp");
	vgui_tree_set_filter(tree, match_contains, &match);
	vgui_update(ui, 0.016f);

	float tx = 0, ty = 0, tw = 0, th = 0;
	vgui_widget_get_rect(tree, &tx, &ty, &tw, &th);
	(void)th;
	const float row_h = style.tree.row_height;
	const auto row_at = [&](int row) {
		// item_at_point 接收控件局部坐标。
		return vgui_tree_item_at_point(tree, 30.0f,
			style.tree.padding_y + row_h * (float)row + row_h * 0.5f);
	};
	// 过滤后只保留 Alpha / alpha-child;折叠的 beta 行被过滤隐藏,但其折叠
	// 后代的命中测试行 2 应为 0(无第三行)。
	check(row_at(0) == alpha, "tree filter: matching root kept");
	check(row_at(1) == alpha_child, "tree filter: matching child kept");
	check(row_at(2) == 0, "tree filter: non-matching rows hidden");
	// beta 被过滤隐藏,但选择仍保留(节点未销毁)。
	check(vgui_tree_is_selected(tree, beta), "tree filter: hidden selection preserved");
	// 过滤忽略折叠状态:折叠的 beta 若命中也应显示其后代。换过滤词需
	// 重新 set_filter 触发行投影重建(match 文本只影响绘制)。
	vgui_tree_set_match_text(tree, "gam");
	match.needle = "gam";
	vgui_tree_set_filter(tree, match_contains, &match);
	vgui_update(ui, 0.016f);
	check(row_at(0) == beta, "tree filter: ancestor of match shown");
	check(row_at(1) == beta_child, "tree filter: match under collapsed node shown");

	// 清除过滤:行投影恢复,展开状态未被动过(beta 仍折叠)。
	vgui_tree_set_match_text(tree, nullptr);
	vgui_tree_set_filter(tree, nullptr, nullptr);
	vgui_update(ui, 0.016f);
	check(row_at(0) == alpha, "tree filter: clear restores rows");
	check(row_at(1) == alpha_child, "tree filter: clear keeps expansion of alpha");
	check(row_at(2) == beta, "tree filter: clear keeps beta collapsed");
	check(vgui_tree_is_selected(tree, beta), "tree filter: clear keeps selection");

	// 行矩形:可见行返回内容区矩形,折叠后代返回 0。
	float rx = 0, ry = 0, rw = 0, rh = 0;
	check(vgui_tree_get_item_rect(tree, alpha_child, &rx, &ry, &rw, &rh) == 1,
		"tree rect: visible row returns rect");
	check(near(rx, tx + style.tree.padding_x) && near(rw, tw - 2 * style.tree.padding_x)
		&& near(ry, ty + style.tree.padding_y + row_h) && near(rh, row_h),
		"tree rect: rect values");
	check(vgui_tree_get_item_rect(tree, beta_child, &rx, &ry, &rw, &rh) == 0,
		"tree rect: hidden row returns 0");
	vgui_widget_destroy(tree);

	// 编辑取消事件 + 焦点监听。
	vgui_widget* edit = vgui_widget_add(vgui_root(ui), VGUI_EDIT, nullptr);
	vgui_widget_set_id(edit, "cancel-edit");
	vgui_widget_set_pos(edit, 10.0f, 700.0f);
	vgui_widget_set_size(edit, 200.0f, 28.0f);
	vgui_edit_set_text(edit, "x");
	edit_event_counter events;
	vgui_widget_on_event(edit, on_edit_event, &events);
	focus_change_counter focus;
	vgui_context_set_focus_listener(ui, on_focus_change, &focus);
	vgui_update(ui, 0.016f);

	vgui_set_focus(ui, edit);
	check(focus.changes == 1 && focus.last_new == edit && focus.last_old == nullptr,
		"focus listener: focus gained");
	vgui_input_key(ui, VGUI_KEY_ESCAPE, 1, 0);
	vgui_input_key(ui, VGUI_KEY_ESCAPE, 0, 0);
	check(events.cancels == 1, "edit cancel: Escape fires CANCEL");
	check(vgui_get_focus(ui) == nullptr, "edit cancel: Escape clears focus");
	check(focus.changes == 2 && focus.last_old == edit && focus.last_new == nullptr,
		"focus listener: blur reported");

	vgui_set_focus(ui, edit);
	vgui_input_key(ui, VGUI_KEY_ENTER, 1, 0);
	vgui_input_key(ui, VGUI_KEY_ENTER, 0, 0);
	check(events.submits == 1 && events.cancels == 1, "edit cancel: Enter still submits");
	vgui_context_set_focus_listener(ui, nullptr, nullptr);
	vgui_widget_destroy(edit);
}

//----------------------------------------------------------------------------
// context-level popup
//----------------------------------------------------------------------------

static void test_popup(vgui_context* ui)
{
	/* detached 内容面板:一个按钮子控件。 */
	vgui_widget* content = vgui_widget_create(ui, VGUI_PANEL, nullptr);
	vgui_widget_set_id(content, "popup-content");
	vgui_widget_set_size(content, 160.0f, 90.0f);
	vgui_widget_set_background(content, 0xFF202020);
	vgui_widget_set_border(content, 0xFF404040, 1.0f);
	vgui_widget* btn = vgui_widget_add(content, VGUI_BUTTON, "inside");
	vgui_widget_set_pos(btn, 10.0f, 10.0f);
	vgui_widget_set_size(btn, 80.0f, 26.0f);
	int clicks = 0;
	vgui_widget_on_event(btn, [](vgui_widget*, const vgui_event* e, void* u) {
		if (e->type == VGUI_EVENT_CLICK) (*(int*)u)++;
	}, &clicks);

	int closes = 0;
	vgui_popup_on_close(ui, [](void* u, int) { (*(int*)u)++; }, &closes);

	/* 打开:激活、内容定位、钳制。 */
	vgui_widget_set_pos(vgui_root(ui), 0.0f, 0.0f);
	check(vgui_popup_open(ui, content, 500.0f, 400.0f) == 1, "popup: open");
	check(vgui_popup_active(ui) == 1 && vgui_popup_content(ui) == content,
		"popup: active");
	vgui_update(ui, 0.016f);
	vgui_layout(ui);
	float px = 0, py = 0, pw = 0, ph = 0;
	vgui_widget_get_rect(content, &px, &py, &pw, &ph);
	std::fprintf(stderr, "DBG popup rect %.1f %.1f %.1f %.1f\n", px, py, pw, ph);
	check(near(px, 500.0f) && near(py, 400.0f) && near(pw, 160.0f),
		"popup: positioned at anchor");

	/* 边缘钳制:超出窗口右下角时收回窗口内。 */
	vgui_popup_open(ui, content, 1200.0f, 900.0f);
	vgui_update(ui, 0.016f);
	vgui_layout(ui);
	vgui_widget_get_rect(content, &px, &py, &pw, &ph);
	check(px + pw <= 900.0f + 0.5f && py + ph <= 700.0f + 0.5f,
		"popup: clamped to window");

	/* 内容大于窗口:框保持内容尺寸(边框穿过内容的回归),只钳位置。 */
	vgui_widget_set_size(content, 1200.0f, 900.0f);
	vgui_popup_open(ui, content, 100.0f, 100.0f);
	vgui_update(ui, 0.016f);
	vgui_layout(ui);
	vgui_widget_get_rect(content, &px, &py, &pw, &ph);
	check(near(pw, 1200.0f) && near(ph, 900.0f),
		"popup: frame never smaller than content");
	check(px >= 8.0f - 0.5f && py >= 8.0f - 0.5f,
		"popup: oversized keeps position clamp");
	vgui_widget_set_size(content, 160.0f, 90.0f);
	vgui_popup_open(ui, content, 1200.0f, 900.0f);
	vgui_update(ui, 0.016f);
	vgui_layout(ui);
	vgui_widget_get_rect(content, &px, &py, &pw, &ph);

	/* 内容内点击正常路由(不关闭)。 */
	click_at(ui, px + 30.0f, py + 20.0f);
	check(clicks == 1 && vgui_popup_active(ui) == 1,
		"popup: inside click routes to content");

	/* 外点关闭并吞掉(背景按钮不收到该次点击)。 */
	click_at(ui, 30.0f, 30.0f);
	check(vgui_popup_active(ui) == 0 && closes == 1,
		"popup: outside click closes");

	/* 重新打开后 Esc 关闭(焦点不在内容内)。 */
	vgui_popup_open(ui, content, 100.0f, 100.0f);
	vgui_update(ui, 0.016f);
	vgui_layout(ui);
	vgui_input_key(ui, VGUI_KEY_ESCAPE, 1, 0);
	vgui_input_key(ui, VGUI_KEY_ESCAPE, 0, 0);
	check(vgui_popup_active(ui) == 0 && closes == 2, "popup: escape closes");

	/* 内容内 edit:焦点进入,Esc 先取消编辑(清焦点),再按才关 popup。 */
	vgui_widget* edit = vgui_widget_add(content, VGUI_EDIT, nullptr);
	vgui_widget_set_pos(edit, 10.0f, 50.0f);
	vgui_widget_set_size(edit, 120.0f, 24.0f);
	vgui_popup_open(ui, content, 100.0f, 100.0f);
	vgui_update(ui, 0.016f);
	vgui_layout(ui);
	vgui_widget_get_rect(content, &px, &py, &pw, &ph);
	click_at(ui, px + 30.0f, py + 62.0f);
	check(vgui_get_focus(ui) == edit, "popup: edit inside takes focus");
	vgui_input_text(ui, "abc");
	vgui_input_key(ui, VGUI_KEY_ESCAPE, 1, 0);
	vgui_input_key(ui, VGUI_KEY_ESCAPE, 0, 0);
	check(vgui_popup_active(ui) == 1 && vgui_get_focus(ui) == nullptr,
		"popup: first escape cancels edit only");
	vgui_input_key(ui, VGUI_KEY_ESCAPE, 1, 0);
	vgui_input_key(ui, VGUI_KEY_ESCAPE, 0, 0);
	check(vgui_popup_active(ui) == 0 && closes == 3,
		"popup: second escape closes");

	/* 菜单与 popup 互斥:开菜单关 popup。 */
	vgui_widget* menu = vgui_widget_create(ui, VGUI_MENU, nullptr);
	vgui_menu_add_item(menu, "item");
	vgui_popup_open(ui, content, 100.0f, 100.0f);
	vgui_menu_open(menu, 300.0f, 300.0f);
	check(vgui_popup_active(ui) == 0 && closes == 4,
		"popup: menu opening closes popup");
	vgui_menu_close(menu);
	vgui_widget_destroy(menu);

	/* 滚轮在外:关闭。 */
	vgui_popup_open(ui, content, 100.0f, 100.0f);
	vgui_update(ui, 0.016f);
	vgui_layout(ui);
	vgui_widget_get_rect(content, &px, &py, &pw, &ph);
	vgui_input_mouse_move(ui, 20.0f, 20.0f);
	vgui_input_mouse_wheel(ui, 0.0f, 1.0f);
	check(vgui_popup_active(ui) == 0 && closes == 5,
		"popup: outside wheel closes");

	/* 重复打开同一内容仅更新锚点,不触发 close 回调。 */
	vgui_popup_open(ui, content, 100.0f, 100.0f);
	vgui_popup_open(ui, content, 200.0f, 200.0f);
	check(vgui_popup_active(ui) == 1 && closes == 5,
		"popup: reopen same content keeps session");
	vgui_popup_close(ui);
	check(closes == 6, "popup: explicit close notifies");

	/* 挂到树上的控件拒绝作为 popup 内容。 */
	vgui_widget* attached = vgui_widget_add(vgui_root(ui), VGUI_PANEL, nullptr);
	check(vgui_popup_open(ui, attached, 10.0f, 10.0f) == 0,
		"popup: attached widget rejected");
	vgui_widget_destroy(attached);
	vgui_widget_destroy(content);
}

	/* E01: UI scale keeps logical inputs stable while effective geometry, style
	 * and fonts scale with the monitor DPI. */
	struct ui_scale_state
	{
		int calls = 0;
		float last = 0.0f;
	};

	static void on_ui_scale(vgui_context*, float scale, void* user)
	{
		auto* state = static_cast<ui_scale_state*>(user);
		if (!state)
			return;
		state->calls++;
		state->last = scale;
	}

	static void test_ui_scale(vgui_context* ui)
	{
		vgui_context_set_ui_scale(ui, 1.0f);

		vgui_widget* box = vgui_widget_add(vgui_root(ui), VGUI_PANEL, nullptr);
		vgui_widget_set_pos(box, 10.0f, 20.0f);
		vgui_widget_set_size(box, 100.0f, 40.0f);
		vgui_widget_set_padding(box, 5.0f, 6.0f, 7.0f, 8.0f);
		vgui_widget_set_font_size(box, 20.0f);
		vgui_layout(ui);

		float x = 0, y = 0, w = 0, h = 0;
		vgui_widget_get_rect(box, &x, &y, &w, &h);
		check(near(x, 10.0f) && near(y, 20.0f) && near(w, 100.0f) &&
			near(h, 40.0f), "ui_scale: 100% keeps logical geometry");
		check(near(vgui_widget_get_font_size(box), 20.0f),
			"ui_scale: widget font at 100%");

		vgui_style base_style;
		vgui_style_default(&base_style);

		vgui_context_set_ui_scale(ui, 2.0f);
		vgui_layout(ui);
		check(near(vgui_context_get_ui_scale(ui), 2.0f),
			"ui_scale: context reports 200%");
		check(near(vgui_context_get_font_size(ui), 32.0f),
			"ui_scale: context font doubles");
		check(near(vgui_widget_get_font_size(box), 40.0f),
			"ui_scale: widget font follows scale");

		vgui_style effective;
		vgui_context_get_style(ui, &effective);
		check(near(effective.panel.padding, base_style.panel.padding * 2.0f),
			"ui_scale: effective style doubles");
		check(near(effective.animation_speed, base_style.animation_speed),
			"ui_scale: timing style is unchanged");

		vgui_widget_get_rect(box, &x, &y, &w, &h);
		check(near(x, 20.0f) && near(y, 40.0f) && near(w, 200.0f) &&
			near(h, 80.0f), "ui_scale: live tree rescales to 200%");

		/* Logical setters at a non-1.0 scale must not compound. */
		vgui_widget_set_pos(box, 15.0f, 25.0f);
		vgui_widget_set_size(box, 50.0f, 30.0f);
		vgui_layout(ui);
		vgui_widget_get_rect(box, &x, &y, &w, &h);
		check(near(x, 30.0f) && near(y, 50.0f) && near(w, 100.0f) &&
			near(h, 60.0f), "ui_scale: setters take logical units");

		vgui_context_set_ui_scale(ui, 1.25f);
		vgui_layout(ui);
		vgui_widget_get_rect(box, &x, &y, &w, &h);
		check(near(x, 18.75f) && near(y, 31.25f) && near(w, 62.5f) &&
			near(h, 37.5f), "ui_scale: rescale keeps logical size at 125%");

		vgui_context_set_ui_scale(ui, 1.0f);
		vgui_layout(ui);
		vgui_widget_get_rect(box, &x, &y, &w, &h);
		check(near(x, 15.0f) && near(y, 25.0f) && near(w, 50.0f) &&
			near(h, 30.0f), "ui_scale: returning to 100% restores geometry");
		check(near(vgui_widget_get_font_size(box), 20.0f),
			"ui_scale: explicit font base survives rescaling");

		/* Listener add/remove. */
		ui_scale_state state;
		const int token = vgui_context_add_ui_scale_listener(ui,
			&on_ui_scale, &state);
		check(token > 0, "ui_scale: listener token");
		vgui_context_set_ui_scale(ui, 1.5f);
		check(state.calls == 1 && near(state.last, 1.5f),
			"ui_scale: listener sees new scale");
		vgui_context_remove_ui_scale_listener(ui, token);
		vgui_context_set_ui_scale(ui, 1.0f);
		check(state.calls == 1, "ui_scale: removed listener is silent");

		/* Clamp and invalid handling. */
		vgui_context_set_ui_scale(ui, 10.0f);
		check(near(vgui_context_get_ui_scale(ui), 4.0f),
			"ui_scale: clamps above 400%");
		vgui_context_set_ui_scale(ui, 0.1f);
		check(near(vgui_context_get_ui_scale(ui), 0.25f),
			"ui_scale: clamps below 25%");
		vgui_context_set_ui_scale(ui, 0.0f);
		check(near(vgui_context_get_ui_scale(ui), 1.0f),
			"ui_scale: zero falls back to 100%");
		vgui_context_set_ui_scale(ui, std::nanf(""));
		check(near(vgui_context_get_ui_scale(ui), 1.0f),
			"ui_scale: non-finite falls back to 100%");

		/* A stack layout must not overlap at any supported scale. */
		vgui_widget* column = vgui_widget_add(vgui_root(ui), VGUI_PANEL,
			nullptr);
		vgui_widget_set_pos(column, 40.0f, 40.0f);
		vgui_widget_set_size(column, 200.0f, 100.0f);
		vgui_widget_set_layout(column, VGUI_LAYOUT_COLUMN);
		vgui_widget_set_padding(column, 0, 0, 0, 0);
		vgui_widget_set_gap(column, 0);
		vgui_widget* rows[3] = {};
		for (int i = 0; i < 3; ++i)
		{
			rows[i] = vgui_widget_add(column, VGUI_PANEL, nullptr);
			vgui_widget_set_size(rows[i], 0.0f, 20.0f);
			vgui_widget_set_align_self(rows[i], VGUI_ALIGN_STRETCH);
		}

		const float scales[4] = { 1.0f, 1.25f, 1.5f, 2.0f };
		for (float scale : scales)
		{
			vgui_context_set_ui_scale(ui, scale);
			vgui_layout(ui);
			float cx = 0, cy = 0, cw = 0, ch = 0;
			vgui_widget_get_rect(column, &cx, &cy, &cw, &ch);
			check(near(cw, 200.0f * scale) && near(ch, 100.0f * scale),
				"ui_scale: stack keeps its logical box");
			float prev_bottom = cy;
			for (int i = 0; i < 3; ++i)
			{
				float rx = 0, ry = 0, rw = 0, rh = 0;
				vgui_widget_get_rect(rows[i], &rx, &ry, &rw, &rh);
				check(near(rh, 20.0f * scale),
					"ui_scale: stack rows scale their height");
				check(ry >= prev_bottom - 0.01f &&
					ry + rh <= cy + ch + 0.01f &&
					rx >= cx - 0.01f && rx + rw <= cx + cw + 0.01f,
					"ui_scale: stack rows stay inside and do not overlap");
				prev_bottom = ry + rh;
			}
		}

		vgui_context_set_ui_scale(ui, 1.0f);
		vgui_layout(ui);
		vgui_widget_destroy(column);
		vgui_widget_destroy(box);

		vgui_style scaled;
		vgui_style_default(&scaled);
		const float padding = scaled.panel.padding;
		const float rounding = scaled.button.rounding;
		vgui_style_scale(&scaled, 2.0f);
		check(near(scaled.panel.padding, padding * 2.0f) &&
			near(scaled.button.rounding, rounding * 2.0f),
			"ui_scale: style_scale scales geometry");
		check(scaled.animation_speed == base_style.animation_speed,
			"ui_scale: style_scale leaves timing alone");
	}

	struct a11y_walk_capture
{
	vgui_widget* edit = nullptr;
	vgui_widget* checkbox = nullptr;
	vgui_widget* hidden = nullptr;
	int total = 0;
	bool saw_edit = false;
	bool saw_checkbox = false;
	bool saw_hidden = false;
};

static void on_a11y_walk(vgui_widget* widget, void* user)
{
	a11y_walk_capture* s = (a11y_walk_capture*)user;
	if (!s || !widget) return;
	s->total++;
	if (widget == s->edit) s->saw_edit = true;
	if (widget == s->checkbox) s->saw_checkbox = true;
	if (widget == s->hidden) s->saw_hidden = true;
}

struct a11y_event_capture
{
	int focus = 0;
	int value = 0;
	int state = 0;
	int selection = 0;
	int alert = 0;
};

static void on_a11y_event(vgui_context*, vgui_widget* widget,
	vgui_a11y_event event, void* user)
{
	a11y_event_capture* s = (a11y_event_capture*)user;
	if (!s) return;
	switch (event)
	{
	case VGUI_A11Y_EVENT_FOCUS_CHANGED: if (widget) s->focus++; break;
	case VGUI_A11Y_EVENT_VALUE_CHANGED: if (widget) s->value++; break;
	case VGUI_A11Y_EVENT_STATE_CHANGED: if (widget) s->state++; break;
	case VGUI_A11Y_EVENT_SELECTION_CHANGED: if (widget) s->selection++; break;
	case VGUI_A11Y_EVENT_ALERT: if (!widget) s->alert++; break;
	default: break;
	}
}

static float srgb_linear(uint8_t v)
{
	const float c = (float)v / 255.0f;
	return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

static float contrast_ratio(vgui_color a, vgui_color b)
{
	auto luminance = [](vgui_color c)
	{
		const float r = srgb_linear((uint8_t)((c >> 16) & 0xFF));
		const float g = srgb_linear((uint8_t)((c >> 8) & 0xFF));
		const float bl = srgb_linear((uint8_t)(c & 0xFF));
		return 0.2126f * r + 0.7152f * g + 0.0722f * bl;
	};
	const float la = luminance(a);
	const float lb = luminance(b);
	const float hi = std::max(la, lb);
	const float lo = std::min(la, lb);
	return (hi + 0.05f) / (lo + 0.05f);
}

static void test_accessibility_e05(vgui_context* ui)
{
	vgui_widget_clear(vgui_root(ui));
	vgui_set_focus(ui, nullptr);

	vgui_widget* page = vgui_widget_add(vgui_root(ui), VGUI_PANEL, nullptr);
	vgui_widget_set_pos(page, 20, 20);
	vgui_widget_set_size(page, 420, 340);
	vgui_widget_set_layout(page, VGUI_LAYOUT_COLUMN);
	vgui_widget_set_padding(page, 8, 8, 8, 8);
	vgui_widget_set_gap(page, 4);

	vgui_widget* label = vgui_widget_add(page, VGUI_LABEL, "Name");
	vgui_widget* edit = vgui_widget_add(page, VGUI_EDIT, "");
	vgui_widget_set_a11y_name(edit, "Name field");
	vgui_edit_set_text(edit, "hello");
	vgui_widget* checkbox = vgui_widget_add(page, VGUI_CHECKBOX, "Enabled");
	vgui_widget_set_checked(checkbox, 1);
	vgui_widget* button = vgui_widget_add(page, VGUI_BUTTON, "Save");
	vgui_widget* list = vgui_widget_add(page, VGUI_LIST, nullptr);
	vgui_widget* hidden = vgui_widget_add(page, VGUI_LABEL, "Hidden");
	vgui_widget_set_visible(hidden, 0);
	const vgui_item_id alpha = vgui_list_add_item(list, "Alpha", nullptr);
	const vgui_item_id beta = vgui_list_add_item(list, "Beta", nullptr);

	vgui_layout(ui);

	vgui_a11y_info info = {};
	vgui_widget_get_a11y_info(label, &info);
	check(info.role == VGUI_A11Y_ROLE_LABEL, "a11y: label role");
	check(info.name && std::strcmp(info.name, "Name") == 0, "a11y: label name");

	vgui_widget_get_a11y_info(edit, &info);
	check(info.role == VGUI_A11Y_ROLE_EDIT, "a11y: edit role");
	check(info.name && std::strcmp(info.name, "Name field") == 0, "a11y: explicit name");
	check(info.value && std::strcmp(info.value, "hello") == 0, "a11y: edit value");

	vgui_widget_get_a11y_info(checkbox, &info);
	check(info.role == VGUI_A11Y_ROLE_TOGGLE, "a11y: checkbox toggle role");
	check((info.states & VGUI_A11Y_CHECKED) != 0, "a11y: checked state");
	check(info.value && std::strcmp(info.value, "on") == 0, "a11y: toggle value on");

	vgui_set_focus(ui, edit);
	vgui_widget_get_a11y_info(edit, &info);
	check((info.states & VGUI_A11Y_FOCUSED) != 0, "a11y: focused state");
	vgui_edit_set_readonly(edit, 1);
	vgui_widget_get_a11y_info(edit, &info);
	check((info.states & VGUI_A11Y_READONLY) != 0, "a11y: readonly state");
	vgui_edit_set_readonly(edit, 0);

	vgui_widget_set_enabled(checkbox, 0);
	vgui_widget_get_a11y_info(checkbox, &info);
	check((info.states & VGUI_A11Y_DISABLED) != 0, "a11y: disabled state");
	vgui_widget_set_enabled(checkbox, 1);

	vgui_list_select(list, beta, 1);
	vgui_list_set_cursor(list, beta);
	vgui_widget_get_a11y_info(list, &info);
	check(info.role == VGUI_A11Y_ROLE_LIST, "a11y: list role");
	check(info.item == beta, "a11y: list current item");
	check(info.value && std::strcmp(info.value, "Beta") == 0, "a11y: list value");
	check((info.states & VGUI_A11Y_SELECTED) != 0, "a11y: list selected state");
	(void)alpha;

	vgui_widget_set_a11y_name(button, "Save button");
	vgui_widget_set_a11y_role(button, VGUI_A11Y_ROLE_SLIDER);
	check(std::strcmp(vgui_widget_get_a11y_name(button), "Save button") == 0,
		"a11y: name override round-trip");
	check(vgui_widget_get_a11y_role(button) == VGUI_A11Y_ROLE_SLIDER,
		"a11y: role override round-trip");
	vgui_widget_set_a11y_role(button, VGUI_A11Y_ROLE_NONE);
	check(vgui_widget_get_a11y_role(button) == VGUI_A11Y_ROLE_BUTTON,
		"a11y: role falls back to derived");

	a11y_walk_capture walk;
	walk.edit = edit;
	walk.checkbox = checkbox;
	walk.hidden = hidden;
	vgui_context_a11y_walk(ui, on_a11y_walk, &walk);
	check(walk.total > 0, "a11y: walk visits widgets");
	check(walk.saw_edit && walk.saw_checkbox, "a11y: walk sees visible widgets");
	check(!walk.saw_hidden, "a11y: walk skips hidden widgets");

	a11y_event_capture events;
	const int token = vgui_context_add_a11y_listener(ui, on_a11y_event, &events);
	check(token != 0, "a11y: listener registered");
	vgui_set_focus(ui, nullptr);
	vgui_set_focus(ui, edit);
	check(events.focus == 1, "a11y: focus event");
	vgui_set_focus(ui, checkbox);
	vgui_input_key(ui, ' ', 1, 0);
	check(events.state == 1, "a11y: toggle state event");
	vgui_list_select(list, alpha, 1);
	check(events.selection >= 1, "a11y: selection event");
	vgui_context_announce(ui, "saved");
	check(events.alert == 1, "a11y: alert event");
	check(std::strcmp(vgui_context_get_a11y_announcement(ui), "saved") == 0,
		"a11y: announcement text");
	vgui_context_remove_a11y_listener(ui, token);
	vgui_context_announce(ui, "ignored");
	check(events.alert == 1, "a11y: listener removal stops events");

	/* Focus trap: Tab cycles inside the trapped panel. */
	vgui_widget* trap = vgui_widget_add(vgui_root(ui), VGUI_PANEL, nullptr);
	vgui_widget_set_pos(trap, 460, 20);
	vgui_widget_set_size(trap, 200, 100);
	vgui_widget_set_layout(trap, VGUI_LAYOUT_COLUMN);
	vgui_widget* a1 = vgui_widget_add(trap, VGUI_BUTTON, "A1");
	vgui_widget* a2 = vgui_widget_add(trap, VGUI_BUTTON, "A2");
	vgui_widget_set_focus_trap(trap, 1);
	check(vgui_widget_get_focus_trap(trap) == 1, "focus trap: getter");
	vgui_layout(ui);
	vgui_set_focus(ui, a1);
	vgui_input_key(ui, VGUI_KEY_TAB, 1, 0);
	check(vgui_get_focus(ui) == a2, "focus trap: Tab moves inside");
	vgui_input_key(ui, VGUI_KEY_TAB, 1, 0);
	check(vgui_get_focus(ui) == a1, "focus trap: Tab wraps inside");
	vgui_input_key(ui, VGUI_KEY_TAB, 1, VGUI_MOD_SHIFT);
	check(vgui_get_focus(ui) == a2, "focus trap: Shift+Tab reverses inside");
	vgui_widget_set_focus_trap(trap, 0);
	check(vgui_widget_get_focus_trap(trap) == 0, "focus trap: clear");

	/* Floating Dock pages trap focus automatically. */
	vgui_widget* dock = vgui_widget_add(vgui_root(ui), VGUI_DOCK, nullptr);
	vgui_widget_set_pos(dock, 460, 140);
	vgui_widget_set_size(dock, 300, 200);
	vgui_widget* dock_page = vgui_widget_create(ui, VGUI_PANEL, nullptr);
	vgui_dock_panel_desc desc;
	vgui_dock_panel_desc_default(&desc);
	desc.id = "trap_test";
	desc.title = "Trap";
	desc.panel = dock_page;
	desc.zone = VGUI_DOCK_CENTER;
	const vgui_item_id dock_item = vgui_dock_add(dock, &desc);
	check(dock_item != 0, "focus trap: dock panel added");
	check(vgui_dock_float(dock, dock_item, 460, 140, 300, 200) == 1,
		"focus trap: dock float");
	check(vgui_widget_get_focus_trap(dock_page) == 1,
		"focus trap: floating dock page traps");
	check(vgui_dock_set_zone(dock, dock_item, VGUI_DOCK_CENTER, 0) == 1,
		"focus trap: dock re-dock");
	check(vgui_widget_get_focus_trap(dock_page) == 0,
		"focus trap: docked page does not trap");

	/* F10 opens the first MenuBar title using the keyboard only. */
	vgui_widget* bar = vgui_widget_add(vgui_root(ui), VGUI_MENUBAR, nullptr);
	vgui_widget_set_pos(bar, 20, 380);
	vgui_widget_set_size(bar, 300, 26);
	vgui_widget* menu = vgui_widget_create(ui, VGUI_MENU, nullptr);
	vgui_menu_add_item(menu, "Save");
	const vgui_item_id file_item = vgui_menubar_add_menu(bar, "File", menu);
	check(file_item != 0, "keyboard menu: menubar title");
	vgui_set_focus(ui, button);
	check(vgui_input_key(ui, VGUI_KEY_F10, 1, 0) == 1,
		"keyboard menu: F10 opens");
	check(vgui_menu_is_open(menu) == 1, "keyboard menu: menu is open");
	check(vgui_menubar_get_active(bar) == file_item,
		"keyboard menu: active title");
	check(vgui_input_key(ui, VGUI_KEY_ESCAPE, 1, 0) == 1,
		"keyboard menu: Esc closes");
	check(vgui_menu_is_open(menu) == 0, "keyboard menu: menu closed");

	/* High contrast palette: WCAG contrast for core surfaces. */
	vgui_theme hc = {};
	vgui_theme_high_contrast(&hc);
	check(contrast_ratio(hc.text, hc.panel_bg) >= 7.0f,
		"high contrast: text/panel >= 7:1");
	check(contrast_ratio(hc.row_text, hc.list_bg) >= 7.0f,
		"high contrast: row text/list >= 7:1");
	check(contrast_ratio(hc.row_selected_text, hc.row_selected) >= 7.0f,
		"high contrast: selected row >= 7:1");
	check(contrast_ratio(hc.accent, hc.panel_bg) >= 4.5f,
		"high contrast: accent/panel >= 4.5:1");
	check(contrast_ratio(hc.disabled, hc.panel_bg) >= 4.5f,
		"high contrast: disabled/panel >= 4.5:1");

	vgui_set_focus(ui, nullptr);
	vgui_widget_clear(vgui_root(ui));
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
	test_menubar(ui, style);
	test_key_handlers(ui);
	test_texture_image(ui);
	test_wrap(ui);
	test_splitter(ui);
	test_scroll_listener(ui);
	test_tabview(ui);
	test_list_virtual(ui);
	test_tree_virtual(ui, style);
	test_tree_virtual_edges(ui, style);
	test_list_row_extensions(ui, style);
	test_tree_row_extensions(ui, style);
	test_drag_drop(ui, style);
	test_tooltip(ui, style);
	test_modal(ui);
	test_dialog(ui);
	test_toast(ui, style);
	test_dock(ui, style);
	test_i18n_rtl(ui);
	test_tree_filter_and_focus(ui, style);
	test_popup(ui);
	test_accessibility_e05(ui);
	test_ui_scale(ui);

	vgui_destroy(ui);
	delete_font_family(familys);
	delete wg;

	printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail ? 1 : 0;
}
