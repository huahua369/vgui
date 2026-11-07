/* sample/vgui_editor_c_api_test_views.c - headless C tests for the outliner, table, timeline, node graph and canvas sections of <vgui/vgui_editor.h>. */
#include "vgui_editor_c_api_test.h"

/* -------------------------------------------------------------------------- */
/* Shared helpers                                                             */
/* -------------------------------------------------------------------------- */

static int nearly(double a, double b)
{
	double d = a - b;
	if (d < 0.0)
		d = -d;
	return d < 1e-6;
}

/* Copies text into a host buffer with the string-callback convention: the
 * return value is the bytes the full text needs, terminator included. */
static size_t emit_text(char* out, size_t cap, const char* text)
{
	size_t needed;
	size_t i = 0;

	if (!text)
		return VGUI_EDITOR_NO_VALUE;
	needed = strlen(text) + 1;
	if (out && cap > 1)
	{
		for (; i + 1 < cap && text[i] != '\0'; ++i)
			out[i] = text[i];
	}
	if (out && cap > 0)
		out[i] = '\0';
	return needed;
}

static void copy_text(char* dst, size_t cap, const char* text)
{
	size_t i = 0;
	if (!dst || cap == 0)
		return;
	if (text)
	{
		for (; i + 1 < cap && text[i] != '\0'; ++i)
			dst[i] = text[i];
	}
	dst[i] = '\0';
}

static void release_probe(void* user)
{
	probe* p = (probe*)user;
	if (p)
		++p->released;
}

/* -------------------------------------------------------------------------- */
/* Outliner                                                                   */
/* -------------------------------------------------------------------------- */

static void otl_on_object(void* user, vgui_editor_object_id id)
{
	probe* p = (probe*)user;
	++p->selection_count;
	p->last_selection = id;
}

static int otl_move(void* user, vgui_editor_object_id node,
	vgui_editor_object_id parent, size_t index)
{
	probe* p = (probe*)user;
	++p->action_count;
	p->last_selection = node;
	p->navigate_index = index;
	p->combo_value[0] = '\0';
	(void)parent;
	return p->combo_index == 0 ? 1 : 0;
}

static int otl_rename(void* user, vgui_editor_object_id node, const char* name)
{
	probe* p = (probe*)user;
	++p->click_count;
	p->last_selection = node;
	copy_text(p->navigate_id, sizeof(p->navigate_id), name);
	return p->combo_index == 0 ? 1 : 0;
}

static void otl_flag(void* user, vgui_editor_object_id node, int state)
{
	probe* p = (probe*)user;
	++p->toggle_count;
	p->last_selection = node;
	p->navigate_index = (size_t)state;
}

static int otl_filter_match(void* user, vgui_editor_object_id node, const char* name)
{
	probe* p = (probe*)user;
	++p->navigate_count;
	(void)node;
	return name && name[0] != '\0' ? 1 : 0;
}

static int otl_action(void* user, vgui_editor_outliner_action action,
	const vgui_editor_object_id* nodes, size_t count)
{
	probe* p = (probe*)user;
	++p->commit_count;
	p->combo_index = (int)action;
	p->navigate_index = count;
	(void)nodes;
	return 1;
}

static int otl_asset_drop(void* user, const char* asset,
	vgui_editor_object_id parent, size_t index)
{
	probe* p = (probe*)user;
	++p->click_count;
	p->last_selection = parent;
	p->navigate_index = index;
	copy_text(p->combo_value, sizeof(p->combo_value), asset);
	return 1;
}

static size_t otl_root_count(void* user)
{
	(void)user;
	return 2;
}

static vgui_editor_object_id otl_root_at(void* user, size_t index)
{
	(void)user;
	return index == 0 ? (vgui_editor_object_id)201 : (vgui_editor_object_id)202;
}

static size_t otl_child_count(void* user, vgui_editor_object_id node)
{
	(void)user;
	return node == 201 ? 1 : 0;
}

static vgui_editor_object_id otl_child_at(void* user,
	vgui_editor_object_id parent, size_t index)
{
	(void)user;
	(void)index;
	return parent == 201 ? (vgui_editor_object_id)203 : VGUI_EDITOR_INVALID_OBJECT;
}

static const char* otl_name(void* user, vgui_editor_object_id node)
{
	(void)user;
	if (node == 201)
		return "Root A";
	if (node == 202)
		return "Root B";
	if (node == 203)
		return "Leaf A1";
	return NULL;
}

static vgui_editor_object_id otl_parent_of(void* user, vgui_editor_object_id node)
{
	(void)user;
	return node == 203 ? (vgui_editor_object_id)201 : VGUI_EDITOR_INVALID_OBJECT;
}

void test_outliner(probe* p)
{
	vgui_editor_context* ctx;
	vgui_editor_outliner* outliner;
	vgui_editor_scene_source source;
	vgui_editor_drop_location location;
	vgui_editor_object_id ids[2];
	vgui_editor_object_id listed[4];
	vgui_editor_object_id drop_parent;
	vgui_item_id item_a;
	vgui_item_id item_b;
	vgui_item_id item_child;
	int visible_before;
	int applied;

	memset(p, 0, sizeof(*p));
	memset(&source, 0, sizeof(source));
	memset(&location, 0, sizeof(location));
	memset(ids, 0, sizeof(ids));
	memset(listed, 0, sizeof(listed));

	ctx = vgui_editor_context_create(g_ui);
	check(ctx != NULL, "outliner: host context built");
	outliner = vgui_editor_outliner_create(ctx, vgui_root(g_ui));
	check(outliner != NULL, "outliner built");
	check(vgui_editor_outliner_create(NULL, vgui_root(g_ui)) == NULL,
		"outliner refuses a NULL context");
	check(vgui_editor_outliner_create(ctx, NULL) == NULL,
		"outliner refuses a NULL parent");
	check(vgui_editor_outliner_widget(outliner) != NULL, "outliner exposes its tree");

	/* Mirrored model: rows address scene objects both ways. */
	vgui_editor_outliner_clear(outliner);
	item_a = vgui_editor_outliner_add_node(outliner, "Alpha", 0, 101);
	item_b = vgui_editor_outliner_add_node(outliner, "Beta", 0, 102);
	item_child = vgui_editor_outliner_add_node(outliner, "Child", item_a, 103);
	check(item_a != 0 && item_b != 0 && item_child != 0, "rows allocated");
	check(vgui_editor_outliner_item_for_object(outliner, 101) == item_a,
		"object -> item mapping");
	check(vgui_editor_outliner_object_for_item(outliner, item_child) == 103,
		"item -> object mapping");
	check(vgui_editor_outliner_item_for_object(outliner, 9999) == 0,
		"an unknown object has no row");
	check(vgui_editor_outliner_object_for_item(outliner, 0)
		== VGUI_EDITOR_INVALID_OBJECT, "an unknown item maps to no object");
	vgui_editor_outliner_expand(outliner, item_a);
	check(vgui_editor_outliner_selected(outliner) == 0, "outliner starts unselected");

	/* Row flags: the view flips its own state and reports it to the host. */
	vgui_editor_outliner_enable_row_buttons(outliner);
	vgui_editor_outliner_set_row_button_icons(outliner, NULL, NULL, NULL, NULL);
	vgui_editor_outliner_set_node_icon(outliner, item_a, NULL);
	vgui_editor_outliner_set_node_text_color(outliner, item_a, 0xFFFFFFFFu);
	vgui_editor_outliner_set_on_visibility(outliner, otl_flag, p);
	vgui_editor_outliner_set_on_lock(outliner, otl_flag, p);
	visible_before = vgui_editor_outliner_object_visibility(outliner, 101);
	vgui_editor_outliner_toggle_object_visibility(outliner, 101);
	check(vgui_editor_outliner_object_visibility(outliner, 101) != visible_before,
		"toggling visibility flips the flag");
	check(p->toggle_count == 1, "visibility handler ran once");
	vgui_editor_outliner_set_object_visibility(outliner, 101, 1);
	check(vgui_editor_outliner_object_visibility(outliner, 101) == 1,
		"explicit visibility applies");
	vgui_editor_outliner_set_object_locked(outliner, 101, 1);
	check(vgui_editor_outliner_object_locked(outliner, 101) == 1, "lock state set");
	vgui_editor_outliner_toggle_object_lock(outliner, 101);
	check(vgui_editor_outliner_object_locked(outliner, 101) == 0, "lock toggle clears");
	check(p->toggle_count == 2, "lock handler ran too");

	/* Move and rename, accepted and rejected. */
	vgui_editor_outliner_set_move_handler(outliner, otl_move, p);
	vgui_editor_outliner_set_rename_handler(outliner, otl_rename, p);
	p->combo_index = 0;
	check(vgui_editor_outliner_rename_node(outliner, 103, "Renamed") == 1,
		"rename accepted by the handler");
	check(p->click_count == 1 && strcmp(p->navigate_id, "Renamed") == 0,
		"the rename handler saw the new name");
	p->combo_index = 1;
	check(vgui_editor_outliner_rename_node(outliner, 103, "Nope") == 0,
		"a rejected rename returns 0");
	p->combo_index = 0;
	check(vgui_editor_outliner_move_node(outliner, 103, 102, 0) == 1,
		"move accepted by the handler");
	check(p->action_count == 1 && p->last_selection == 103 && p->navigate_index == 0,
		"the move handler saw node, parent and index");
	vgui_editor_outliner_set_move_handler(outliner, NULL, NULL);
	vgui_editor_outliner_set_rename_handler(outliner, NULL, NULL);

	/* Inline rename and the context menu need real key/pointer input, so only the
	 * resting state is observable headless. */
	vgui_editor_outliner_enable_inline_rename(outliner);
	check(vgui_editor_outliner_renaming(outliner) == 0, "no inline rename pending");
	vgui_editor_outliner_enable_context_menu(outliner);
	vgui_editor_outliner_set_action_handler(outliner, otl_action, p);
	check(p->commit_count == 0, "no context action without user input");

	/* Drag & drop targets (O03). */
	vgui_editor_outliner_enable_drag_drop(outliner);
	vgui_editor_outliner_set_multi_select(outliner, 1);
	check(vgui_editor_outliner_multi_select(outliner) == 1, "multi select on");
	vgui_editor_outliner_set_multi_select(outliner, 0);
	check(vgui_editor_outliner_multi_select(outliner) == 0, "multi select off");
	vgui_editor_outliner_set_asset_drop_handler(outliner, otl_asset_drop, p);
	check(vgui_editor_outliner_asset_drop_enabled(outliner) == 1,
		"asset drop enabled once a handler exists");
	p->click_count = 0;
	applied = vgui_editor_outliner_apply_asset_drop(outliner, "mesh.fbx", 1.0e5f,
		1.0e5f);
	check(applied == 1 && p->click_count == 1, "asset drop routed to the handler");
	check(strcmp(p->combo_value, "mesh.fbx") == 0, "the drop carried the asset name");
	check(vgui_editor_outliner_apply_asset_drop(outliner, "", 1.0e5f, 1.0e5f) == 0,
		"an empty asset name is refused");
	vgui_editor_outliner_apply_drop_payload(outliner, "101,102", 1.0e5f, 1.0e5f);

	vgui_editor_drop_location_default(&location);
	check(location.struct_size == (uint32_t)sizeof(location),
		"drop location default stamps its size");
	check(location.kind == VGUI_EDITOR_DROP_NONE, "drop location default is none");
	memset(&location, 0, sizeof(location));
	vgui_editor_outliner_drop_location_at(outliner, 1.0e5f, 1.0e5f, &location);
	check(location.struct_size == (uint32_t)sizeof(location),
		"the drop location reports its size");
	check(location.kind == VGUI_EDITOR_DROP_ROOTS,
		"a point below every row drops on the roots");
	check(location.parent == VGUI_EDITOR_INVALID_OBJECT && location.index == 2,
		"a root drop names no parent and the root count");
	drop_parent = 0;
	vgui_editor_outliner_row_drop_location(outliner, 1.0e5f, 1.0e5f, &drop_parent,
		&location.index);
	vgui_editor_outliner_drop_location_at(outliner, 1.0e5f, 1.0e5f, NULL);

	/* Filter text (O04) round-trips; the predicate is additive. */
	vgui_editor_outliner_set_filter(outliner, "Al");
	check(vgui_editor_outliner_filter(outliner) != NULL
		&& strcmp(vgui_editor_outliner_filter(outliner), "Al") == 0,
		"the filter text round-trips");
	vgui_editor_outliner_set_filter_match_handler(outliner, otl_filter_match, p);
	vgui_editor_outliner_scene_row_changed(outliner, 101);
	vgui_editor_outliner_set_filter(outliner, "");
	check(vgui_editor_outliner_filter(outliner) != NULL
		&& vgui_editor_outliner_filter(outliner)[0] == '\0', "the filter cleared");
	vgui_editor_outliner_set_filter_match_handler(outliner, NULL, NULL);

	/* Virtual mode (O05): the host answers the tree queries. */
	vgui_editor_scene_source_default(&source);
	check(source.struct_size == (uint32_t)sizeof(source),
		"scene source default stamps its size");
	memset(&source, 0, sizeof(source));
	vgui_editor_scene_source_default(&source);
	source.root_count = otl_root_count;
	source.root_at = otl_root_at;
	source.child_count = otl_child_count;
	source.child_at = otl_child_at;
	source.name = otl_name;
	source.parent_of = otl_parent_of;
	source.user = p;
	source.release_user = release_probe;
	p->released = 0;
	vgui_editor_outliner_set_scene_source(outliner, &source);
	check(vgui_editor_outliner_is_virtual(outliner) == 1, "scene source enters virtual");
	check(vgui_editor_outliner_projected_rows(outliner) == 2,
		"virtual mode projects the collapsed roots");
	vgui_editor_outliner_scene_changed(outliner);

	/* struct_size prefix: a shorter struct keeps the fields the host knows about. */
	memset(&source, 0, sizeof(source));
	vgui_editor_scene_source_default(&source);
	source.struct_size = (uint32_t)offsetof(vgui_editor_scene_source, parent_of);
	source.root_count = otl_root_count;
	source.root_at = otl_root_at;
	source.child_count = otl_child_count;
	source.child_at = otl_child_at;
	source.name = otl_name;
	source.user = p;
	source.release_user = release_probe;
	vgui_editor_outliner_set_scene_source(outliner, &source);
	check(vgui_editor_outliner_is_virtual(outliner) == 1,
		"a partial struct still installs the required callbacks");
	check(vgui_editor_outliner_projected_rows(outliner) == 2,
		"a partial struct projects the same rows");
	check(p->released == 1, "replacing a scene source releases the old user once");
	vgui_editor_outliner_set_scene_source(outliner, NULL);
	check(vgui_editor_outliner_is_virtual(outliner) == 0, "NULL source ends virtual mode");
	check(vgui_editor_outliner_projected_rows(outliner) == 0,
		"mirrored mode reports no projected rows");

	/* Entering virtual mode drops the tree's own model, so the mirror is rebuilt. */
	vgui_editor_outliner_clear(outliner);
	item_a = vgui_editor_outliner_add_node(outliner, "Alpha", 0, 101);
	item_b = vgui_editor_outliner_add_node(outliner, "Beta", 0, 102);
	item_child = vgui_editor_outliner_add_node(outliner, "Child", item_a, 103);

	/* Selection through the editor context. */
	ids[0] = 101;
	ids[1] = 102;
	vgui_editor_outliner_set_multi_select(outliner, 1);
	vgui_editor_outliner_set_on_selection(outliner, otl_on_object, p);
	vgui_editor_outliner_enable_selection_sync(outliner);
	vgui_editor_outliner_set_command_stack(outliner, vgui_editor_context_commands(ctx));
	check(vgui_editor_context_selection_count(ctx) == 0,
		"the context starts with no selection");
	check(vgui_editor_context_is_selected(ctx, 101) == 0,
		"the context reports nothing selected");
	vgui_editor_context_set_selection_ids(ctx, ids, 2, 101);
	frame(2);
	check(vgui_editor_outliner_selection_ids(outliner, NULL, 0) == 2,
		"selection sync follows the context count");
	check(vgui_editor_outliner_selection_ids(outliner, listed, 4) == 2,
		"the id list returns the full count");
	check(listed[0] == 101 && listed[1] == 102, "selection ids keep visual order");
	check(vgui_editor_outliner_selected(outliner) != 0, "a row became selected");
	check(p->selection_count >= 1, "the selection listener ran");
	vgui_editor_context_clear_selection(ctx);
	frame(2);
	check(vgui_editor_outliner_selection_ids(outliner, NULL, 0) == 0,
		"clearing the context clears the rows");
	check(vgui_editor_outliner_selection_ids(outliner, listed, 1) == 0,
		"an empty selection lists nothing");

	/* Teardown: clear every callback, destroy, then prove NULL is a no-op. */
	vgui_editor_outliner_set_on_selection(outliner, NULL, NULL);
	vgui_editor_outliner_set_on_visibility(outliner, NULL, NULL);
	vgui_editor_outliner_set_on_lock(outliner, NULL, NULL);
	vgui_editor_outliner_set_filter_match_handler(outliner, NULL, NULL);
	vgui_editor_outliner_set_asset_drop_handler(outliner, NULL, NULL);
	vgui_editor_outliner_set_action_handler(outliner, NULL, NULL);
	vgui_editor_outliner_set_command_stack(outliner, NULL);
	vgui_editor_outliner_set_scene_source(outliner, NULL);
	vgui_editor_outliner_clear(outliner);
	vgui_editor_outliner_destroy(outliner);
	vgui_editor_context_destroy(ctx);
	check(vgui_editor_outliner_widget(NULL) == NULL, "a NULL outliner has no widget");
	check(vgui_editor_outliner_object_for_item(NULL, 5)
		== VGUI_EDITOR_INVALID_OBJECT, "a NULL outliner maps no item");
	check(vgui_editor_outliner_projected_rows(NULL) == 0,
		"a NULL outliner projects nothing");
	check(vgui_editor_outliner_selection_ids(NULL, listed, 4) == 0,
		"a NULL outliner lists no selection");
	check(vgui_editor_outliner_multi_select(NULL) == 0, "a NULL outliner is single-select");
	vgui_editor_outliner_expand(NULL, 0);
	memset(&location, 0, sizeof(location));
	vgui_editor_outliner_drop_location_at(NULL, 0.0f, 0.0f, &location);
	check(location.struct_size == 0 && location.kind == VGUI_EDITOR_DROP_NONE,
		"a NULL outliner leaves the drop location alone");
	vgui_editor_outliner_destroy(NULL);
	vgui_editor_scene_source_default(NULL);
	vgui_editor_drop_location_default(NULL);
}

/* -------------------------------------------------------------------------- */
/* Table view                                                                 */
/* -------------------------------------------------------------------------- */

static const char* const g_tbl_names[3] = { "gamma", "alpha", "beta" };
static const char* const g_tbl_qty[3] = { "30", "1", "12" };

static size_t tbl_row_count(void* user)
{
	probe* p = (probe*)user;
	if (p)
		++p->navigate_count;
	return 3;
}

static size_t tbl_cell_text(void* user, size_t row, const char* column, char* out,
	size_t cap)
{
	probe* p = (probe*)user;
	const char* value;
	if (p)
		++p->change_count;
	if (row >= 3)
		return VGUI_EDITOR_NO_VALUE;
	value = (column && strcmp(column, "qty") == 0) ? g_tbl_qty[row] : g_tbl_names[row];
	return emit_text(out, cap, value);
}

static vgui_editor_object_id tbl_row_id(void* user, size_t row)
{
	(void)user;
	return row < 3 ? (vgui_editor_object_id)(700 + row) : VGUI_EDITOR_INVALID_OBJECT;
}

static void tbl_cell_style(void* user, size_t row, const char* column,
	vgui_editor_table_cell_style* out)
{
	probe* p = (probe*)user;
	(void)column;
	if (p)
		++p->toggle_count;
	if (!out)
		return;
	vgui_editor_table_cell_style_default(out);
	out->text_color = row == 0 ? 0xFFFF0000u : 0u;
}

static int tbl_compare(void* user, const char* a, const char* b)
{
	probe* p = (probe*)user;
	long la;
	long lb;
	if (p)
		++p->click_count;
	la = a ? atol(a) : 0;
	lb = b ? atol(b) : 0;
	return la < lb ? -1 : (la > lb ? 1 : 0);
}

static int tbl_set_value(void* user, size_t row, const char* value)
{
	probe* p = (probe*)user;
	++p->commit_count;
	p->navigate_index = row;
	copy_text(p->navigate_id, sizeof(p->navigate_id), value);
	return p->combo_index == 0 ? 1 : 0;
}

static int tbl_edit(void* user, const vgui_editor_table_cell* cell, const char* value)
{
	probe* p = (probe*)user;
	++p->action_count;
	if (cell)
		p->navigate_index = cell->row;
	copy_text(p->combo_value, sizeof(p->combo_value), value);
	return p->combo_index == 0 ? 1 : 0;
}

static int tbl_on_edit(void* user, const vgui_editor_table_cell* cell,
	const char* value)
{
	probe* p = (probe*)user;
	(void)cell;
	(void)value;
	++p->selection_set_count;
	return 1;
}

static void tbl_on_select(void* user, const vgui_editor_table_cell* cell)
{
	probe* p = (probe*)user;
	++p->selection_count;
	if (cell)
		p->last_value = (double)cell->row + (double)cell->column * 0.001;
}

void test_table(probe* p)
{
	vgui_editor_table* table;
	vgui_editor_table_column columns[2];
	vgui_editor_table_column extra;
	vgui_editor_table_column read_col;
	vgui_editor_table_source source;
	vgui_editor_table_cell cell;
	vgui_editor_table_cell hit;
	char json[1024];
	float cx;
	float cy;
	float cw;
	float ch;
	size_t needed;
	size_t first;
	size_t shown;
	int applied;

	memset(p, 0, sizeof(*p));
	memset(columns, 0, sizeof(columns));
	memset(&extra, 0, sizeof(extra));
	memset(&read_col, 0, sizeof(read_col));
	memset(&source, 0, sizeof(source));
	memset(&cell, 0, sizeof(cell));
	memset(&hit, 0, sizeof(hit));
	memset(json, 0, sizeof(json));

	table = vgui_editor_table_create(vgui_root(g_ui));
	check(table != NULL, "table built");
	check(vgui_editor_table_create(NULL) == NULL, "table refuses a NULL parent");
	check(vgui_editor_table_widget(table) != NULL, "table exposes its widget");
	check(vgui_editor_table_built(table) == 1, "table reports itself built");

	vgui_editor_table_cell_default(&cell);
	check(cell.struct_size == (uint32_t)sizeof(cell), "cell default stamps its size");
	check(cell.column == VGUI_EDITOR_TABLE_NO_COLUMN, "the default cell has no column");
	check(cell.row == 0, "the default cell starts at row zero");
	vgui_editor_table_cell_default(NULL);
	{
		vgui_editor_table_cell_style style;
		vgui_editor_table_cell_style_default(&style);
		check(style.struct_size == (uint32_t)sizeof(style),
			"cell style default stamps its size");
		check(style.text_color == 0u && style.background == 0u,
			"a default style keeps both theme colours");
		vgui_editor_table_cell_style_default(NULL);
	}

	vgui_editor_table_column_default(&columns[0]);
	check(columns[0].struct_size == (uint32_t)sizeof(columns[0]),
		"column default stamps its size");
	check(columns[0].width == 120.0f && columns[0].min_width == 40.0f,
		"the column default carries its widths");
	check(columns[0].sortable == 1 && columns[0].editable == 1,
		"the column default is sortable and editable");
	check(columns[0].alignment == VGUI_EDITOR_TABLE_ALIGN_LEFT,
		"the column default aligns left");
	check(columns[0].id == NULL && columns[0].editor == NULL,
		"the column default has no id or editor");
	vgui_editor_table_column_default(NULL);
	vgui_editor_table_source_default(&source);
	check(source.struct_size == (uint32_t)sizeof(source),
		"table source default stamps its size");
	check(source.row_count == NULL && source.cell_text == NULL,
		"the table source default has no callbacks");
	vgui_editor_table_source_default(NULL);

	/* Pull model. */
	memset(&source, 0, sizeof(source));
	vgui_editor_table_source_default(&source);
	source.row_count = tbl_row_count;
	source.cell_text = tbl_cell_text;
	source.row_id = tbl_row_id;
	source.cell_style = tbl_cell_style;
	source.user = p;
	source.release_user = release_probe;
	p->released = 0;
	vgui_editor_table_set_source(table, &source);
	vgui_editor_table_refresh(table);
	check(vgui_editor_table_row_count(table) == 3, "the row count comes from the host");
	check(p->navigate_count >= 1, "the table pulled the row count");

	/* Columns. */
	vgui_editor_table_column_default(&columns[0]);
	columns[0].id = "name";
	columns[0].label = "Name";
	columns[0].width = 140.0f;
	vgui_editor_table_column_default(&columns[1]);
	columns[1].id = "qty";
	columns[1].label = "Qty";
	columns[1].width = 70.0f;
	columns[1].alignment = VGUI_EDITOR_TABLE_ALIGN_RIGHT;
	columns[1].sortable = 0;
	columns[1].compare = tbl_compare;
	columns[1].set_value = tbl_set_value;
	columns[1].user = p;
	columns[1].release_user = release_probe;
	vgui_editor_table_set_columns(table, columns, 2);
	check(vgui_editor_table_column_count(table) == 2, "two columns installed");
	check(vgui_editor_table_add_column(table, &columns[0]) == 0,
		"a duplicate column id is rejected");
	vgui_editor_table_column_default(&extra);
	extra.label = "Generated";
	check(vgui_editor_table_add_column(table, &extra) == 1,
		"an empty id is generated from the index");
	check(vgui_editor_table_column_count(table) == 3, "add_column grew the model");
	vgui_editor_table_clear_columns(table);
	check(vgui_editor_table_column_count(table) == 0, "clear_columns empties the model");
	vgui_editor_table_set_columns(table, columns, 2);
	check(vgui_editor_table_column_count(table) == 2, "set_columns replaces the model");

	memset(&read_col, 0, sizeof(read_col));
	check(vgui_editor_table_column_at(table, 0, &read_col) == 1, "column 0 is readable");
	check(read_col.struct_size == (uint32_t)sizeof(read_col),
		"the read column stamps its size");
	check(read_col.id && strcmp(read_col.id, "name") == 0, "the column id round-trips");
	check(read_col.label && strcmp(read_col.label, "Name") == 0, "label round-trips");
	check(read_col.width == 140.0f
		&& read_col.alignment == VGUI_EDITOR_TABLE_ALIGN_LEFT,
		"width and alignment round-trip");
	check(read_col.sortable == 1 && read_col.editable == 1, "the flags round-trip");
	check(read_col.compare == NULL && read_col.set_value == NULL
		&& read_col.user == NULL, "host callbacks are not read back");
	check(vgui_editor_table_column_at(table, 99, &read_col) == 0,
		"the column index is bounded");
	check(vgui_editor_table_set_column_width(table, 0, 170.0f) == 1, "width accepted");
	check(vgui_editor_table_column_width(table, 0) == 170.0f, "the width round-trips");
	check(vgui_editor_table_column_width(table, 99) == 0.0f,
		"an unknown column has no width");
	check(vgui_editor_table_set_column_width(table, 99, 10.0f) == 0,
		"an unknown column width is rejected");

	/* Rows, mapping and sorting. */
	vgui_editor_table_refresh(table);
	check(vgui_editor_table_display_row_count(table) == 3, "every row is displayed");
	check(vgui_editor_table_source_row_at(table, 1) == 1, "display maps to source");
	check(vgui_editor_table_display_index_of(table, 2) == 2, "source maps to display");
	check(vgui_editor_table_display_index_of(table, 99) == -1,
		"a hidden row has no display index");
	first = 0;
	shown = 0;
	vgui_editor_table_visible_row_range(table, &first, &shown);
	check(shown <= 3, "the visible window never exceeds the model");
	vgui_editor_table_visible_row_range(table, NULL, NULL);
	check(vgui_editor_table_set_sort(table, 1, 1) == 0, "an unsortable column rejects");
	check(vgui_editor_table_set_sort(table, 0, 1) == 1, "sorting column 0 accepted");
	check(vgui_editor_table_sort_column(table) == 0, "the sort column is remembered");
	check(vgui_editor_table_sort_ascending(table) == 1, "the sort direction is remembered");
	check(vgui_editor_table_display_row_count(table) == 3, "sorting keeps every row");
	check(vgui_editor_table_source_row_at(table, 0) == 1, "the text sort puts alpha first");
	vgui_editor_table_clear_sort(table);
	check(vgui_editor_table_sort_column(table) == -1, "clear_sort drops the column");
	check(vgui_editor_table_source_row_at(table, 0) == 0, "clearing the sort restores order");

	/* Selection and hit testing. */
	vgui_editor_table_set_on_selection(table, tbl_on_select, p);
	vgui_editor_table_clear_selection(table);
	vgui_editor_table_selected_cell(table, &cell);
	check(cell.column == VGUI_EDITOR_TABLE_NO_COLUMN, "no cell selected after a clear");
	vgui_editor_table_selected_cell(table, NULL);
	check(vgui_editor_table_select_cell(table, 1, 0) == 1, "selecting a cell works");
	vgui_editor_table_selected_cell(table, &cell);
	check(cell.struct_size == (uint32_t)sizeof(cell), "the selected cell stamps its size");
	check(cell.row == 1 && cell.column == 0, "the selected cell round-trips");
	check(p->selection_count >= 1, "the selection listener ran");
	check(vgui_editor_table_select_cell(table, 99, 0) == 0,
		"an out-of-range row is rejected");
	memset(&hit, 0, sizeof(hit));
	vgui_editor_table_hit_test(table, 1.0e5f, 1.0e5f, &hit);
	check(hit.column == VGUI_EDITOR_TABLE_NO_COLUMN,
		"an off-screen hit test finds no cell");
	vgui_editor_table_hit_test(table, 10.0f, 10.0f, NULL);
	check(vgui_editor_table_cell_rect(table, 999, 0, NULL, NULL, NULL, NULL) == 0,
		"an unknown row has no rect");
	check(vgui_editor_table_header_rect(table, 99, NULL, NULL, NULL, NULL) == 0,
		"an unknown column has no header rect");
	cx = cy = cw = ch = 0.0f;
	vgui_editor_table_cell_rect(table, 0, 0, &cx, &cy, &cw, &ch);
	vgui_editor_table_header_rect(table, 0, &cx, &cy, &cw, &ch);
	check(cw >= 0.0f && ch >= 0.0f, "cell rects are never negative");

	/* Scroll and metrics. */
	vgui_editor_table_set_scroll(table, 0.0f, 0.0f);
	check(vgui_editor_table_scroll_x(table) == 0.0f
		&& vgui_editor_table_scroll_y(table) == 0.0f, "scroll resets to zero");
	check(vgui_editor_table_max_scroll_x(table) >= 0.0f, "max scroll is non-negative");
	vgui_editor_table_scroll_to_row(table, 2);
	vgui_editor_table_set_row_height(table, 24.0f);
	check(vgui_editor_table_row_height(table) == 24.0f, "the row height round-trips");
	check(vgui_editor_table_header_height(table) > 0.0f, "the header keeps a height");

	/* Editing: the column setter first, then the shared edit handler. */
	vgui_editor_table_set_edit_handler(table, tbl_edit, p);
	vgui_editor_table_set_on_edit(table, tbl_on_edit, p);
	vgui_editor_table_set_command_stack(table, NULL);
	check(vgui_editor_table_editing(table) == 0, "the table is not editing");
	vgui_editor_table_editing_cell(table, &cell);
	check(cell.column == VGUI_EDITOR_TABLE_NO_COLUMN, "no cell is being edited");
	check(vgui_editor_table_select_cell(table, 0, 1) == 1, "an editable cell selected");
	p->combo_index = 0;
	p->commit_count = 0;
	vgui_editor_table_begin_edit(table);
	check(vgui_editor_table_commit_edit(table, "7") == 1, "the column setter accepted");
	check(p->commit_count == 1 && p->navigate_index == 0
		&& strcmp(p->navigate_id, "7") == 0, "the setter saw row and value");
	p->combo_index = 1;
	vgui_editor_table_begin_edit(table);
	check(vgui_editor_table_commit_edit(table, "8") == 0, "a rejecting setter returns 0");
	p->combo_index = 0;
	p->action_count = 0;
	check(vgui_editor_table_select_cell(table, 2, 0) == 1, "a plain column selected");
	vgui_editor_table_begin_edit(table);
	check(vgui_editor_table_commit_edit(table, "delta") == 1,
		"without a setter the edit handler applies");
	check(p->action_count == 1 && strcmp(p->combo_value, "delta") == 0,
		"the edit handler saw the value");
	applied = vgui_editor_table_begin_edit(table);
	vgui_editor_table_cancel_edit(table);
	check(applied == 0 || applied == 1, "begin_edit answers cleanly");

	/* struct_size prefix on a column: only the known fields land. */
	memset(&extra, 0, sizeof(extra));
	vgui_editor_table_column_default(&extra);
	extra.struct_size = (uint32_t)offsetof(vgui_editor_table_column, editor);
	extra.id = "short";
	extra.label = "Short";
	extra.width = 90.0f;
	extra.sortable = 1;
	extra.editable = 0;
	extra.set_value = tbl_set_value;
	extra.user = p;
	extra.release_user = release_probe;
	check(vgui_editor_table_add_column(table, &extra) == 1,
		"a short column struct was added");
	memset(&read_col, 0, sizeof(read_col));
	check(vgui_editor_table_column_at(table, 2, &read_col) == 1,
		"the short column is readable");
	check(read_col.id && strcmp(read_col.id, "short") == 0,
		"the short column kept its id");
	check(read_col.width == 90.0f && read_col.editable == 0,
		"the short column kept width and flags");

	/* struct_size prefix on the source: callbacks beyond it degrade to empty. */
	p->released = 0;
	memset(&source, 0, sizeof(source));
	vgui_editor_table_source_default(&source);
	source.struct_size = (uint32_t)offsetof(vgui_editor_table_source, cell_text);
	source.row_count = tbl_row_count;
	source.user = p;
	source.release_user = release_probe;
	vgui_editor_table_set_source(table, &source);
	vgui_editor_table_refresh(table);
	check(vgui_editor_table_row_count(table) == 3, "a partial source still counts rows");
	check(p->released == 1, "replacing a source releases the old user once");

	/* State JSON round-trip. */
	vgui_editor_table_clear_columns(table);
	vgui_editor_table_set_columns(table, columns, 2);
	memset(&source, 0, sizeof(source));
	vgui_editor_table_source_default(&source);
	source.row_count = tbl_row_count;
	source.cell_text = tbl_cell_text;
	source.user = p;
	source.release_user = release_probe;
	vgui_editor_table_set_source(table, &source);
	vgui_editor_table_refresh(table);
	vgui_editor_table_set_sort(table, 0, 0);
	vgui_editor_table_set_column_width(table, 1, 66.0f);
	needed = vgui_editor_table_capture_state_json(table, NULL, 0);
	check(needed > 0, "the table state json reports its length");
	check(vgui_editor_table_capture_state_json(table, json, sizeof(json)) < sizeof(json),
		"the table state json fits the buffer");
	check(json[0] == '{', "the table state json is an object");
	vgui_editor_table_clear_sort(table);
	vgui_editor_table_set_column_width(table, 1, 70.0f);
	check(vgui_editor_table_apply_state_json(table, json) == 1,
		"the table state json applies");
	check(vgui_editor_table_sort_column(table) == 0, "the sort column survived");
	check(vgui_editor_table_sort_ascending(table) == 0, "the sort direction survived");
	check(vgui_editor_table_column_width(table, 1) == 66.0f, "the column width survived");
	check(vgui_editor_table_apply_state_json(table, "{ not json") == 0,
		"garbage table json is rejected");
	check(vgui_editor_table_apply_state_json(table, NULL) == 0,
		"a NULL table json is rejected");
	check(vgui_editor_table_last_render_cell_queries(table) == 0,
		"no cell queries are counted without a paint");
	vgui_editor_table_focus(table);

	/* Teardown. */
	vgui_editor_table_set_on_selection(table, NULL, NULL);
	vgui_editor_table_set_edit_handler(table, NULL, NULL);
	vgui_editor_table_set_on_edit(table, NULL, NULL);
	p->released = 0;
	vgui_editor_table_clear_columns(table);
	vgui_editor_table_set_source(table, NULL);
	vgui_editor_table_clear_selection(table);
	vgui_editor_table_destroy(table);
	check(p->released == 2, "clearing the columns and the source released both users");
	check(vgui_editor_table_widget(NULL) == NULL, "a NULL table has no widget");
	check(vgui_editor_table_column_count(NULL) == 0, "a NULL table has no columns");
	check(vgui_editor_table_row_count(NULL) == 0, "a NULL table has no rows");
	check(vgui_editor_table_built(NULL) == 0, "a NULL table is not built");
	check(vgui_editor_table_capture_state_json(NULL, json, sizeof(json)) == 0,
		"a NULL table captures nothing");
	check(vgui_editor_table_select_cell(NULL, 0, 0) == 0, "a NULL table selects nothing");
	vgui_editor_table_destroy(NULL);
	vgui_editor_table_set_source(NULL, NULL);
	memset(&hit, 0, sizeof(hit));
	hit.struct_size = 1234567u;
	hit.row = 4242u;
	hit.column = 42;
	vgui_editor_table_hit_test(NULL, 0.0f, 0.0f, &hit);
	check(hit.struct_size == 1234567u && hit.row == 4242u && hit.column == 42,
		"a NULL table leaves the hit cell alone");
}

/* -------------------------------------------------------------------------- */
/* Timeline                                                                   */
/* -------------------------------------------------------------------------- */

static void tl_on_selection(void* user, const vgui_editor_object_id* ids, size_t count)
{
	probe* p = (probe*)user;
	++p->selection_count;
	p->navigate_index = count;
	p->last_selection = count > 0 && ids ? ids[0] : VGUI_EDITOR_INVALID_OBJECT;
}

static void tl_on_change(void* user)
{
	probe* p = (probe*)user;
	++p->change_count;
}

static void tl_on_playhead(void* user, double seconds)
{
	probe* p = (probe*)user;
	++p->commit_count;
	p->last_value = seconds;
}

void test_timeline(probe* p)
{
	vgui_editor_timeline* timeline;
	vgui_editor_timeline_track track;
	vgui_editor_timeline_track read_track;
	vgui_editor_timeline_keyframe keys[2];
	vgui_editor_timeline_keyframe key;
	vgui_editor_timeline_keyframe read_key;
	vgui_editor_timeline_view_state view;
	vgui_editor_command_stack* stack;
	vgui_editor_object_id ids[2];
	vgui_editor_object_id track_a;
	vgui_editor_object_id track_b;
	vgui_editor_object_id auto_id;
	vgui_editor_object_id found_track;
	vgui_editor_object_id kf_b;
	char json[1024];
	double x;
	size_t needed;

	memset(p, 0, sizeof(*p));
	memset(&track, 0, sizeof(track));
	memset(&read_track, 0, sizeof(read_track));
	memset(keys, 0, sizeof(keys));
	memset(&key, 0, sizeof(key));
	memset(&read_key, 0, sizeof(read_key));
	memset(&view, 0, sizeof(view));
	memset(ids, 0, sizeof(ids));
	memset(json, 0, sizeof(json));

	timeline = vgui_editor_timeline_create(vgui_root(g_ui));
	check(timeline != NULL, "timeline built");
	check(vgui_editor_timeline_create(NULL) == NULL, "timeline refuses a NULL parent");
	check(vgui_editor_timeline_widget(timeline) != NULL, "timeline exposes its widget");
	check(vgui_editor_timeline_built(timeline) == 1, "timeline reports itself built");
	check(vgui_editor_timeline_enabled(timeline) == 1, "timeline starts enabled");
	vgui_editor_timeline_set_enabled(timeline, 0);
	check(vgui_editor_timeline_enabled(timeline) == 0, "timeline can be disabled");
	vgui_editor_timeline_set_enabled(timeline, 1);
	check(vgui_editor_timeline_focused(timeline) == 0, "timeline starts unfocused");
	vgui_editor_timeline_focus(timeline);
	vgui_editor_timeline_set_drag_threshold(timeline, 5.0f);
	check(vgui_editor_timeline_drag_threshold(timeline) == 5.0f,
		"the drag threshold round-trips");

	/* Descriptor defaults. */
	vgui_editor_timeline_track_default(&track);
	check(track.struct_size == (uint32_t)sizeof(track), "track default stamps its size");
	check(track.id == VGUI_EDITOR_INVALID_OBJECT && track.visible == 1,
		"the track default is an unnamed visible row");
	check(track.height == 26.0f && track.color == 0xFF5FA8FF,
		"the track default carries height and colour");
	check(track.keyframes == NULL && track.keyframe_count == 0,
		"the track default has no keyframes");
	vgui_editor_timeline_track_default(NULL);
	vgui_editor_timeline_keyframe_default(&key);
	check(key.id == VGUI_EDITOR_INVALID_OBJECT && key.time == 0.0 && key.value == 0.0,
		"the keyframe default is a zero key at t=0");
	check(key.interpolation == VGUI_EDITOR_TIMELINE_LINEAR,
		"the keyframe default interpolates linearly");
	vgui_editor_timeline_keyframe_default(NULL);
	vgui_editor_timeline_view_state_default(&view);
	check(view.struct_size == (uint32_t)sizeof(view), "view state stamps its size");
	check(view.view_start == 0.0 && view.pixels_per_second == 120.0,
		"the view default is one second per 120 pixels");
	vgui_editor_timeline_view_state_default(NULL);

	/* Model CRUD. */
	vgui_editor_timeline_track_default(&track);
	track.id = 900;
	track.name = "Motion";
	vgui_editor_timeline_keyframe_default(&keys[0]);
	keys[0].id = 1000;
	keys[0].time = 0.0;
	keys[0].value = 0.0;
	vgui_editor_timeline_keyframe_default(&keys[1]);
	keys[1].id = 1001;
	keys[1].time = 2.0;
	keys[1].value = 10.0;
	track.keyframes = keys;
	track.keyframe_count = 2;
	track_a = vgui_editor_timeline_add_track(timeline, &track);
	check(track_a == 900, "an explicit track id is kept");
	check(vgui_editor_timeline_add_track(timeline, &track)
		== VGUI_EDITOR_INVALID_OBJECT, "a duplicate track id is rejected");
	check(vgui_editor_timeline_track_count(timeline) == 1, "one track in the model");
	check(vgui_editor_timeline_find_track(timeline, 900, &read_track) == 1,
		"the track was found by id");
	check(read_track.struct_size == (uint32_t)sizeof(read_track),
		"the read track stamps its size");
	check(read_track.name && strcmp(read_track.name, "Motion") == 0,
		"the track name round-trips");
	check(read_track.visible == 1 && read_track.height == 26.0f,
		"the track flags and height round-trip");
	check(read_track.keyframe_count == 2, "the kit copied the borrowed keyframes");
	check(vgui_editor_timeline_keyframe_count(timeline, 900) == 2, "the keyframe count");
	check(vgui_editor_timeline_keyframe_at(timeline, 900, 1, &read_key) == 1,
		"a keyframe is readable by index");
	check(read_key.id == 1001 && nearly(read_key.time, 2.0) && nearly(read_key.value, 10.0),
		"the keyframe fields round-trip");
	check(vgui_editor_timeline_keyframe_at(timeline, 900, 9, &read_key) == 0,
		"the keyframe index is bounded");
	check(vgui_editor_timeline_track_at(timeline, 0, &read_track) == 1
		&& read_track.id == 900, "the track is readable by index");
	check(vgui_editor_timeline_track_at(timeline, 7, &read_track) == 0,
		"the track index is bounded");
	check(vgui_editor_timeline_find_track(timeline, 9999, &read_track) == 0,
		"an unknown track id is not found");

	/* struct_size prefix: the keyframes beyond it are ignored. */
	vgui_editor_timeline_track_default(&track);
	track.struct_size = (uint32_t)offsetof(vgui_editor_timeline_track, keyframes);
	track.id = 901;
	track.name = "Prefix";
	track.keyframes = keys;
	track.keyframe_count = 2;
	track_b = vgui_editor_timeline_add_track(timeline, &track);
	check(track_b == 901, "a short track struct still adds the track");
	memset(&read_track, 0, sizeof(read_track));
	check(vgui_editor_timeline_find_track(timeline, 901, &read_track) == 1,
		"the short track struct is readable back");
	check(read_track.name && strcmp(read_track.name, "Prefix") == 0,
		"the short track struct kept its name");
	check(vgui_editor_timeline_keyframe_count(timeline, 901) == 0,
		"keyframes beyond the prefix were ignored");

	/* Update and remove. */
	vgui_editor_timeline_track_default(&track);
	track.id = 901;
	track.name = "Renamed";
	track.visible = 0;
	track.locked = 1;
	track.height = 40.0f;
	check(vgui_editor_timeline_update_track(timeline, &track) == 1, "the track updated");
	check(vgui_editor_timeline_find_track(timeline, 901, &read_track) == 1
		&& read_track.visible == 0 && read_track.locked == 1,
		"the track flags survived the update");
	check(nearly(read_track.height, 40.0), "the track height survived the update");
	track.id = 4242;
	check(vgui_editor_timeline_update_track(timeline, &track) == 0,
		"updating an unknown track fails");
	check(vgui_editor_timeline_remove_track(timeline, 4242) == 0,
		"removing an unknown track fails");

	/* Keyframes: add, update, find across tracks, remove. */
	vgui_editor_timeline_keyframe_default(&key);
	key.id = 1002;
	key.time = 1.0;
	key.value = 5.0;
	key.interpolation = VGUI_EDITOR_TIMELINE_STEP;
	check(vgui_editor_timeline_add_keyframe(timeline, 901, &key) == 1002,
		"an explicit keyframe id is kept");
	check(vgui_editor_timeline_add_keyframe(timeline, 901, &key)
		== VGUI_EDITOR_INVALID_OBJECT, "a duplicate keyframe id is rejected");
	vgui_editor_timeline_keyframe_default(&key);
	key.time = 3.0;
	key.value = 1.0;
	auto_id = vgui_editor_timeline_add_keyframe(timeline, 901, &key);
	check(auto_id != VGUI_EDITOR_INVALID_OBJECT, "a zero id gets allocated");
	kf_b = auto_id;
	check(vgui_editor_timeline_keyframe_count(timeline, 901) == 2, "two keys on track B");
	key.id = 1002;
	key.time = 1.0;
	key.value = 6.5;
	check(vgui_editor_timeline_update_keyframe(timeline, 901, &key) == 1,
		"the keyframe updated");
	check(vgui_editor_timeline_keyframe_at(timeline, 901, 0, &read_key) == 1
		&& nearly(read_key.value, 6.5), "the updated value reads back");
	check(vgui_editor_timeline_find_keyframe(timeline, 1002, &found_track, &read_key) == 1,
		"a keyframe is found across tracks");
	check(found_track == 901 && read_key.id == 1002, "the keyframe reported its track");
	check(vgui_editor_timeline_find_keyframe(timeline, 1002, NULL, NULL) == 1,
		"a keyframe search tolerates NULL outputs");
	check(vgui_editor_timeline_find_keyframe(timeline, 424242, &found_track, NULL) == 0,
		"an unknown keyframe is not found");
	check(vgui_editor_timeline_remove_keyframe(timeline, 901, kf_b) == 1,
		"the keyframe was removed");
	check(vgui_editor_timeline_remove_keyframe(timeline, 901, kf_b) == 0,
		"removing the same keyframe twice fails");
	check(vgui_editor_timeline_keyframe_count(timeline, 901) == 1, "one key left");

	/* Interpolation-aware sampling. */
	check(nearly(vgui_editor_timeline_evaluate(timeline, 900, 1.0), 5.0),
		"the linear track samples its midpoint");
	check(nearly(vgui_editor_timeline_evaluate(timeline, 900, 0.0), 0.0),
		"the linear track hits its first key");
	check(nearly(vgui_editor_timeline_evaluate(timeline, 901, 0.5), 6.5),
		"the step track holds its value");

	/* Selection. */
	vgui_editor_timeline_set_on_selection(timeline, tl_on_selection, p);
	vgui_editor_timeline_set_multi_select(timeline, 1);
	check(vgui_editor_timeline_multi_select(timeline) == 1, "timeline multi select on");
	vgui_editor_timeline_set_multi_select(timeline, 0);
	check(vgui_editor_timeline_multi_select(timeline) == 0, "timeline multi select off");
	check(vgui_editor_timeline_selection_count(timeline) == 0, "no keys selected yet");
	check(vgui_editor_timeline_primary_selection(timeline)
		== VGUI_EDITOR_INVALID_OBJECT, "there is no primary selection yet");
	check(vgui_editor_timeline_is_selected(timeline, 1000) == 0, "nothing is selected");
	check(vgui_editor_timeline_selection_id_at(timeline, 0)
		== VGUI_EDITOR_INVALID_OBJECT, "an empty selection lists nothing");
	ids[0] = 1000;
	ids[1] = 1001;
	p->selection_count = 0;
	vgui_editor_timeline_set_selection(timeline, ids, 2, 1000);
	check(vgui_editor_timeline_selection_count(timeline) == 2, "two keys selected");
	check(vgui_editor_timeline_primary_selection(timeline) == 1000,
		"the primary id is remembered");
	check(vgui_editor_timeline_selection_id_at(timeline, 1) == 1001, "ids listed in order");
	check(vgui_editor_timeline_is_selected(timeline, 1000) == 1, "is_selected agrees");
	check(p->selection_count == 1, "the selection listener ran once");
	vgui_editor_timeline_select_all(timeline);
	check(vgui_editor_timeline_selection_count(timeline)
		== vgui_editor_timeline_keyframe_count(timeline, 900)
		+ vgui_editor_timeline_keyframe_count(timeline, 901),
		"select all picks up every key");
	vgui_editor_timeline_clear_selection(timeline);
	check(vgui_editor_timeline_selection_count(timeline) == 0, "the selection cleared");
	vgui_editor_timeline_set_selected_track(timeline, 901);
	check(vgui_editor_timeline_selected_track(timeline) == 901, "the selected track is set");
	vgui_editor_timeline_set_selected_track(timeline, VGUI_EDITOR_INVALID_OBJECT);
	check(vgui_editor_timeline_selected_track(timeline) == VGUI_EDITOR_INVALID_OBJECT,
		"the selected track is cleared");

	/* Change listener. */
	vgui_editor_timeline_set_on_change(timeline, tl_on_change, p);
	p->change_count = 0;
	vgui_editor_timeline_set_duration(timeline, 12.0);
	check(vgui_editor_timeline_duration(timeline) == 12.0, "duration round-trips");
	check(p->change_count >= 1, "a model change notified the listeners");

	/* View, zoom, pan and coordinate conversion. */
	memset(&view, 0, sizeof(view));
	vgui_editor_timeline_get_view_state(timeline, &view);
	check(view.struct_size == (uint32_t)sizeof(view), "view state reads its size");
	vgui_editor_timeline_get_view_state(timeline, NULL);
	view.view_start = 2.0;
	view.pixels_per_second = 100.0;
	view.scroll_y = 4.0f;
	vgui_editor_timeline_set_view_state(timeline, &view);
	check(vgui_editor_timeline_zoom(timeline) == 100.0, "the view state sets the zoom");
	check(nearly(vgui_editor_timeline_time_to_x(timeline, 2.0), 0.0),
		"the view start maps to x zero");
	check(nearly(vgui_editor_timeline_x_to_time(timeline, 100.0), 3.0),
		"x maps back to time");
	x = vgui_editor_timeline_content_x_at_time(timeline, 4.0);
	check(nearly(vgui_editor_timeline_time_at_content_x(timeline, (float)x), 4.0),
		"content x and time round-trip");
	vgui_editor_timeline_set_zoom_limits(timeline, 50.0, 200.0);
	vgui_editor_timeline_zoom_at(timeline, 10.0f, 1000.0);
	check(vgui_editor_timeline_zoom(timeline) <= 200.0 + 1e-9
		&& vgui_editor_timeline_zoom(timeline) >= 50.0 - 1e-9,
		"zooming respects the limits");
	vgui_editor_timeline_pan_by_pixels(timeline, 25.0f);
	vgui_editor_timeline_reset_view(timeline);
	check(vgui_editor_timeline_zoom(timeline) > 0.0, "reset leaves a positive zoom");

	/* Snapping and frame rate. */
	vgui_editor_timeline_set_snap_enabled(timeline, 1);
	check(vgui_editor_timeline_snap_enabled(timeline) == 1, "snapping enabled");
	vgui_editor_timeline_set_frame_rate(timeline, 30.0);
	check(vgui_editor_timeline_frame_rate(timeline) == 30.0, "the frame rate round-trips");
	check(nearly(vgui_editor_timeline_snap_step(timeline), 1.0 / 30.0),
		"the frame rate keeps the snap step in sync");
	vgui_editor_timeline_set_snap_step(timeline, 0.5);
	check(nearly(vgui_editor_timeline_snap_step(timeline), 0.5), "a custom snap step");
	vgui_editor_timeline_set_snap_to_frames(timeline, 0);
	check(vgui_editor_timeline_snap_to_frames(timeline) == 0, "snap to frames off");
	check(nearly(vgui_editor_timeline_snap_time(timeline, 0.53), 0.5),
		"snap_time rounds to the step");
	vgui_editor_timeline_set_snap_enabled(timeline, 0);
	check(nearly(vgui_editor_timeline_snap_time(timeline, 0.53), 0.53),
		"a disabled snap leaves the time alone");
	vgui_editor_timeline_set_snap_enabled(timeline, 1);

	/* Playback. */
	vgui_editor_timeline_set_on_playhead(timeline, tl_on_playhead, p);
	p->commit_count = 0;
	vgui_editor_timeline_set_playhead(timeline, 1.25);
	check(nearly(vgui_editor_timeline_playhead(timeline), 1.25), "the playhead was set");
	check(p->commit_count >= 1 && nearly(p->last_value, 1.25),
		"the playhead listener ran");
	vgui_editor_timeline_play(timeline);
	check(vgui_editor_timeline_playing(timeline) == 1, "playing after play()");
	vgui_editor_timeline_tick(timeline, 0.5);
	check(nearly(vgui_editor_timeline_playhead(timeline), 1.75), "tick advances time");
	vgui_editor_timeline_pause(timeline);
	check(vgui_editor_timeline_playing(timeline) == 0, "paused");
	vgui_editor_timeline_toggle_play(timeline);
	check(vgui_editor_timeline_playing(timeline) == 1, "toggle_play starts playback");
	vgui_editor_timeline_stop(timeline);
	check(vgui_editor_timeline_playing(timeline) == 0, "stop pauses");
	check(nearly(vgui_editor_timeline_playhead(timeline), 0.0), "stop rewinds to zero");
	vgui_editor_timeline_set_loop(timeline, 1);
	check(vgui_editor_timeline_loop(timeline) == 1, "the loop flag round-trips");
	vgui_editor_timeline_set_loop(timeline, 0);
	vgui_editor_timeline_step_frames(timeline, 3);
	check(nearly(vgui_editor_timeline_playhead(timeline), 0.1), "step_frames at 30 fps");
	vgui_editor_timeline_step_frames(timeline, -10);
	check(vgui_editor_timeline_playhead(timeline) >= 0.0, "stepping back clamps at zero");

	/* User-action editing forms plus the undo stack. */
	stack = vgui_editor_command_stack_create();
	check(stack != NULL, "command stack built");
	vgui_editor_timeline_set_command_stack(timeline, stack);
	check(vgui_editor_command_stack_can_undo(stack) == 0, "the stack starts empty");
	vgui_editor_timeline_set_selected_track(timeline, 900);
	ids[0] = vgui_editor_timeline_insert_keyframe(timeline, 900, 1.5, 7.0);
	check(ids[0] != VGUI_EDITOR_INVALID_OBJECT, "insert_keyframe returned an id");
	check(vgui_editor_command_stack_can_undo(stack) == 1,
		"the user-action form recorded a command");
	vgui_editor_timeline_copy_selection(timeline);
	check(vgui_editor_timeline_clipboard_count(timeline) == 1, "copy filled the clipboard");
	vgui_editor_timeline_set_playhead(timeline, 5.0);
	check(vgui_editor_timeline_paste_at_playhead(timeline) == 1,
		"pasting at the playhead works");
	vgui_editor_timeline_cut_selection(timeline);
	check(vgui_editor_timeline_clipboard_count(timeline) > 0, "cut keeps the clipboard");
	/* Track 901 is locked, so the playhead form has to target the editable one. */
	vgui_editor_timeline_set_selected_track(timeline, 900);
	vgui_editor_timeline_set_playhead(timeline, 2.0);
	kf_b = vgui_editor_timeline_add_keyframe_at_playhead(timeline);
	check(kf_b != VGUI_EDITOR_INVALID_OBJECT, "a keyframe was added at the playhead");
	ids[1] = kf_b;
	vgui_editor_timeline_set_selection(timeline, ids + 1, 1, kf_b);
	check(vgui_editor_timeline_delete_selection(timeline) == 1, "delete_selection worked");
	check(vgui_editor_timeline_selection_count(timeline) == 0,
		"deleting clears the selection");
	vgui_editor_command_stack_clear(stack);
	check(vgui_editor_command_stack_can_undo(stack) == 0, "the cleared stack is empty");
	vgui_editor_timeline_set_command_stack(timeline, NULL);
	vgui_editor_command_stack_destroy(stack);

	/* Geometry and gestures. */
	check(vgui_editor_timeline_track_rect(timeline, 900, NULL, NULL, NULL, NULL) >= 0,
		"the track rect tolerates NULL outputs");
	check(vgui_editor_timeline_keyframe_rect(timeline, 1000, NULL, NULL, NULL, NULL) >= 0,
		"the keyframe rect tolerates NULL outputs");
	check(vgui_editor_timeline_hit_test(timeline, 1.0e5f, 1.0e5f)
		== VGUI_EDITOR_INVALID_OBJECT, "an off-screen hit test finds no key");
	check(vgui_editor_timeline_track_at_y(timeline, 1.0e5f)
		== VGUI_EDITOR_INVALID_OBJECT, "an off-screen y finds no track");
	check(vgui_editor_timeline_dragging(timeline) == 0, "nothing is being dragged");
	check(vgui_editor_timeline_get_gesture(timeline)
		== VGUI_EDITOR_TIMELINE_GESTURE_NONE, "the gesture state starts idle");
	vgui_editor_timeline_cancel_gesture(timeline);

	/* State JSON round-trip. */
	vgui_editor_timeline_set_snap_step(timeline, 0.25);
	vgui_editor_timeline_set_snap_to_frames(timeline, 0);
	vgui_editor_timeline_set_playhead(timeline, 3.5);
	vgui_editor_timeline_set_loop(timeline, 1);
	needed = vgui_editor_timeline_capture_state_json(timeline, NULL, 0);
	check(needed > 0, "the timeline state json reports its length");
	check(vgui_editor_timeline_capture_state_json(timeline, json, sizeof(json))
		< sizeof(json), "the timeline state json fits the buffer");
	vgui_editor_timeline_set_snap_step(timeline, 0.5);
	vgui_editor_timeline_set_snap_to_frames(timeline, 1);
	vgui_editor_timeline_set_playhead(timeline, 0.0);
	vgui_editor_timeline_set_loop(timeline, 0);
	check(vgui_editor_timeline_apply_state_json(timeline, json) == 1,
		"the timeline state json applies");
	check(nearly(vgui_editor_timeline_snap_step(timeline), 0.25), "the snap step survived");
	check(vgui_editor_timeline_snap_to_frames(timeline) == 0,
		"the snap-to-frames flag survived");
	check(nearly(vgui_editor_timeline_playhead(timeline), 3.5), "the playhead survived");
	check(vgui_editor_timeline_loop(timeline) == 1, "the loop flag survived");
	check(vgui_editor_timeline_apply_state_json(timeline, "{ nope") == 0,
		"garbage timeline json is rejected");
	check(vgui_editor_timeline_apply_state_json(timeline, NULL) == 0,
		"a NULL timeline json is rejected");
	vgui_editor_timeline_clear_selection(timeline);

	/* Teardown. */
	vgui_editor_timeline_set_on_selection(timeline, NULL, NULL);
	vgui_editor_timeline_set_on_change(timeline, NULL, NULL);
	vgui_editor_timeline_set_on_playhead(timeline, NULL, NULL);
	vgui_editor_timeline_clear_tracks(timeline);
	check(vgui_editor_timeline_track_count(timeline) == 0, "clear_tracks empties the model");
	vgui_editor_timeline_destroy(timeline);
	check(vgui_editor_timeline_widget(NULL) == NULL, "a NULL timeline has no widget");
	check(vgui_editor_timeline_track_count(NULL) == 0, "a NULL timeline has no tracks");
	check(vgui_editor_timeline_built(NULL) == 0, "a NULL timeline is not built");
	check(vgui_editor_timeline_hit_test(NULL, 0.0f, 0.0f)
		== VGUI_EDITOR_INVALID_OBJECT, "a NULL hit test finds nothing");
	check(vgui_editor_timeline_capture_state_json(NULL, json, sizeof(json)) == 0,
		"a NULL timeline captures nothing");
	vgui_editor_timeline_destroy(NULL);
	vgui_editor_timeline_track_default(NULL);
	vgui_editor_timeline_keyframe_default(NULL);
	vgui_editor_timeline_view_state_default(NULL);
	(void)track_a;
	(void)track_b;
}

/* -------------------------------------------------------------------------- */
/* Node graph                                                                 */
/* -------------------------------------------------------------------------- */

static int ng_create_node(void* user, vgui_editor_node_node* out)
{
	probe* p = (probe*)user;

	++p->action_count;
	if (!out || p->combo_index != 0)
		return 0;
	out->title = "From Host";
	out->category = "Test";
	/* The library hands out writable port slots; a host fills them in place
	 * rather than pointing at its own storage. */
	out->outputs[0].id = 4100;
	out->outputs[0].name = "host_out";
	out->outputs[0].direction = VGUI_EDITOR_NODE_PORT_OUTPUT;
	out->outputs[0].type = VGUI_EDITOR_NODE_TYPE_FLOW;
	out->outputs[0].multi = 0;
	out->output_count = 1;
	p->last_value = out->x;
	return 1;
}

static void ng_on_selection(void* user, const vgui_editor_object_id* ids, size_t count)
{
	probe* p = (probe*)user;
	++p->selection_count;
	p->navigate_index = count;
	p->last_selection = count > 0 && ids ? ids[0] : VGUI_EDITOR_INVALID_OBJECT;
}

static void ng_on_change(void* user)
{
	probe* p = (probe*)user;
	++p->change_count;
}

void test_node_graph(probe* p)
{
	vgui_editor_node_graph* graph;
	vgui_editor_node_node node;
	vgui_editor_node_node read_node;
	vgui_editor_node_port ports[2];
	vgui_editor_node_port new_port;
	vgui_editor_node_port read_port;
	vgui_editor_node_connection conn;
	vgui_editor_node_connection read_conn;
	vgui_editor_node_view_state view;
	vgui_editor_node_view_state read_view;
	vgui_editor_node_point point;
	vgui_editor_node_point back;
	vgui_editor_node_rect rect;
	vgui_editor_command_stack* stack;
	vgui_editor_object_id ids[2];
	vgui_editor_object_id auto_id;
	vgui_editor_object_id found_node;
	vgui_editor_object_id found_port;
	vgui_editor_node_port_direction direction;
	char json[1024];
	char text[128];
	size_t needed;

	memset(p, 0, sizeof(*p));
	memset(&node, 0, sizeof(node));
	memset(&read_node, 0, sizeof(read_node));
	memset(ports, 0, sizeof(ports));
	memset(&new_port, 0, sizeof(new_port));
	memset(&read_port, 0, sizeof(read_port));
	memset(&conn, 0, sizeof(conn));
	memset(&read_conn, 0, sizeof(read_conn));
	memset(&view, 0, sizeof(view));
	memset(&read_view, 0, sizeof(read_view));
	memset(&point, 0, sizeof(point));
	memset(&back, 0, sizeof(back));
	memset(&rect, 0, sizeof(rect));
	memset(ids, 0, sizeof(ids));
	memset(json, 0, sizeof(json));
	memset(text, 0, sizeof(text));

	graph = vgui_editor_node_graph_create(vgui_root(g_ui));
	check(graph != NULL, "node graph built");
	check(vgui_editor_node_graph_create(NULL) == NULL,
		"the node graph refuses a NULL parent");
	check(vgui_editor_node_graph_widget(graph) != NULL, "the graph exposes its widget");
	check(vgui_editor_node_graph_built(graph) == 1, "the graph reports itself built");
	check(vgui_editor_node_graph_enabled(graph) == 1, "the graph starts enabled");
	vgui_editor_node_graph_set_enabled(graph, 0);
	check(vgui_editor_node_graph_enabled(graph) == 0, "the graph can be disabled");
	vgui_editor_node_graph_set_enabled(graph, 1);
	check(vgui_editor_node_graph_focused(graph) == 0, "the graph starts unfocused");
	vgui_editor_node_graph_focus(graph);
	vgui_editor_node_graph_set_drag_threshold(graph, 4.0f);
	check(vgui_editor_node_graph_drag_threshold(graph) == 4.0f,
		"the drag threshold round-trips");

	/* Descriptor defaults. */
	vgui_editor_node_port_default(&new_port);
	check(new_port.struct_size == (uint32_t)sizeof(new_port),
		"port default stamps its size");
	check(new_port.id == VGUI_EDITOR_INVALID_OBJECT
		&& new_port.direction == VGUI_EDITOR_NODE_PORT_INPUT,
		"the port default is an unnamed input");
	check(new_port.type == VGUI_EDITOR_NODE_TYPE_FLOW && new_port.multi == 0,
		"the port default is a single flow pin");
	vgui_editor_node_node_default(&node);
	check(node.struct_size == (uint32_t)sizeof(node), "node default stamps its size");
	check(node.width == 180.0 && node.visible == 1 && node.collapsed == 0,
		"the node default is an expanded 180-wide card");
	check(node.inputs == NULL && node.input_count == 0, "the node default has no ports");
	vgui_editor_node_connection_default(&conn);
	check(conn.struct_size == (uint32_t)sizeof(conn),
		"connection default stamps its size");
	check(conn.visible == 1 && conn.id == VGUI_EDITOR_INVALID_OBJECT,
		"the connection default is a visible pending link");
	vgui_editor_node_view_state_default(&view);
	check(view.struct_size == (uint32_t)sizeof(view), "view state stamps its size");
	check(view.zoom == 1.0 && view.offset_x == 0.0,
		"the view default is 1:1 at the origin");
	vgui_editor_node_port_default(NULL);
	vgui_editor_node_node_default(NULL);
	vgui_editor_node_connection_default(NULL);
	vgui_editor_node_view_state_default(NULL);

	/* Two nodes with explicit port ids. */
	vgui_editor_node_port_default(&ports[0]);
	ports[0].id = 4001;
	ports[0].name = "flow_in";
	ports[0].direction = VGUI_EDITOR_NODE_PORT_INPUT;
	ports[0].type = VGUI_EDITOR_NODE_TYPE_FLOW;
	vgui_editor_node_port_default(&ports[1]);
	ports[1].id = 4002;
	ports[1].name = "flow_out";
	ports[1].direction = VGUI_EDITOR_NODE_PORT_OUTPUT;
	ports[1].type = VGUI_EDITOR_NODE_TYPE_FLOW;
	vgui_editor_node_graph_set_on_change(graph, ng_on_change, p);
	p->change_count = 0;
	vgui_editor_node_node_default(&node);
	node.id = 3001;
	node.title = "Producer";
	node.category = "Test";
	node.x = 40.0;
	node.y = 60.0;
	node.outputs = ports + 1;
	node.output_count = 1;
	check(vgui_editor_node_graph_add_node(graph, &node) == 3001, "node A added");
	node.id = 3002;
	node.title = "Consumer";
	node.x = 300.0;
	node.y = 60.0;
	node.outputs = NULL;
	node.output_count = 0;
	node.inputs = ports;
	node.input_count = 1;
	check(vgui_editor_node_graph_add_node(graph, &node) == 3002, "node B added");
	check(vgui_editor_node_graph_add_node(graph, &node) == VGUI_EDITOR_INVALID_OBJECT,
		"a duplicate node id is rejected");
	check(vgui_editor_node_graph_node_count(graph) == 2, "two nodes in the graph");
	check(p->change_count >= 1, "a model change notified the listeners");

	memset(&read_node, 0, sizeof(read_node));
	check(vgui_editor_node_graph_find_node(graph, 3001, &read_node) == 1,
		"the node was found by id");
	check(read_node.struct_size == (uint32_t)sizeof(read_node),
		"the read node stamps its size");
	check(read_node.title && strcmp(read_node.title, "Producer") == 0,
		"the title round-trips");
	check(read_node.category && strcmp(read_node.category, "Test") == 0,
		"the category round-trips");
	check(nearly(read_node.x, 40.0) && nearly(read_node.y, 60.0),
		"the position round-trips");
	check(read_node.input_count == 0 && read_node.output_count == 1,
		"the borrowed port arrays were copied");
	check(read_node.outputs[0].id == 4002 && read_node.outputs[0].name
		&& strcmp(read_node.outputs[0].name, "flow_out") == 0, "the port round-trips");
	check(vgui_editor_node_graph_get_node(graph, 1, &read_node) == 1
		&& read_node.id == 3002, "the node is readable by index");
	check(vgui_editor_node_graph_get_node(graph, 9, &read_node) == 0,
		"the node index is bounded");
	check(vgui_editor_node_graph_find_node(graph, 4242, &read_node) == 0,
		"an unknown node is not found");

	/* struct_size prefix: the ports beyond it are dropped. */
	vgui_editor_node_node_default(&node);
	node.struct_size = (uint32_t)offsetof(vgui_editor_node_node, inputs);
	node.id = 3003;
	node.title = "Short";
	node.x = 12.0;
	node.y = 13.0;
	node.inputs = ports;
	node.input_count = 1;
	auto_id = vgui_editor_node_graph_add_node(graph, &node);
	check(auto_id == 3003, "a short node struct still added");
	memset(&read_node, 0, sizeof(read_node));
	check(vgui_editor_node_graph_find_node(graph, 3003, &read_node) == 1,
		"the short node is readable back");
	check(read_node.title && strcmp(read_node.title, "Short") == 0,
		"the short node kept its title");
	check(nearly(read_node.x, 12.0), "the short node kept its position");
	check(read_node.input_count == 0 && read_node.output_count == 0,
		"ports beyond the prefix were ignored");

	/* Ports. */
	vgui_editor_node_port_default(&new_port);
	new_port.name = "value_in";
	new_port.direction = VGUI_EDITOR_NODE_PORT_INPUT;
	new_port.type = VGUI_EDITOR_NODE_TYPE_NUMBER;
	new_port.multi = 1;
	auto_id = vgui_editor_node_graph_add_port(graph, 3003, &new_port);
	check(auto_id != VGUI_EDITOR_INVALID_OBJECT, "the graph allocated a port id");
	memset(&read_port, 0, sizeof(read_port));
	direction = VGUI_EDITOR_NODE_PORT_OUTPUT;
	check(vgui_editor_node_graph_find_port(graph, auto_id, &read_port, &found_node,
		&direction) == 1, "the port was found");
	check(found_node == 3003 && direction == VGUI_EDITOR_NODE_PORT_INPUT,
		"the port reported its owner and direction");
	check(read_port.multi == 1 && read_port.type == VGUI_EDITOR_NODE_TYPE_NUMBER,
		"the port flags round-trip");
	read_port.name = "value_in2";
	check(vgui_editor_node_graph_update_port(graph, &read_port) == 1, "the port updated");
	memset(&read_port, 0, sizeof(read_port));
	check(vgui_editor_node_graph_find_port(graph, auto_id, &read_port, NULL, NULL) == 1,
		"a port search tolerates NULL outputs");
	check(read_port.name && strcmp(read_port.name, "value_in2") == 0,
		"the port rename round-trips");
	check(vgui_editor_node_graph_find_port(graph, 4242, &read_port, NULL, NULL) == 0,
		"an unknown port is not found");
	check(vgui_editor_node_graph_remove_port(graph, auto_id) == 1, "the port was removed");
	check(vgui_editor_node_graph_remove_port(graph, auto_id) == 0,
		"removing the same port twice fails");

	/* Connections. */
	vgui_editor_node_connection_default(&conn);
	conn.id = 5001;
	conn.from_node = 3001;
	conn.from_port = 4002;
	conn.to_node = 3002;
	conn.to_port = 4001;
	check(vgui_editor_node_graph_add_connection(graph, &conn) == 5001,
		"the connection was added");
	check(vgui_editor_node_graph_add_connection(graph, &conn)
		== VGUI_EDITOR_INVALID_OBJECT, "a duplicate connection id is rejected");
	check(vgui_editor_node_graph_connection_count(graph) == 1, "one connection");
	check(vgui_editor_node_graph_get_connection(graph, 0, &read_conn) == 1,
		"the connection is readable by index");
	check(read_conn.from_port == 4002 && read_conn.to_node == 3002,
		"the connection endpoints round-trip");
	check(vgui_editor_node_graph_find_connection(graph, 5001, &read_conn) == 1,
		"the connection was found by id");
	conn.visible = 0;
	check(vgui_editor_node_graph_update_connection(graph, &conn) == 1,
		"the connection updated");
	check(vgui_editor_node_graph_find_connection(graph, 5001, &read_conn) == 1
		&& read_conn.visible == 0, "the visibility survived the update");
	check(vgui_editor_node_graph_connect_ports(graph, 3001, 3002) == 0,
		"node ids are not ports");
	check(vgui_editor_node_graph_remove_connection(graph, 5001) == 1,
		"the connection was removed");
	check(vgui_editor_node_graph_connection_count(graph) == 0, "no connections left");
	check(vgui_editor_node_graph_connect_ports(graph, 4002, 4001) == 1,
		"connect_ports links an output to an input");
	check(vgui_editor_node_graph_connection_count(graph) == 1, "the link landed");
	check(vgui_editor_node_graph_remove_port(graph, 4001) == 1,
		"the linked input port was removed");
	check(vgui_editor_node_graph_connection_count(graph) == 0,
		"the dangling connection was dropped with its port");
	vgui_editor_node_graph_clear_connections(graph);
	check(vgui_editor_node_graph_connection_count(graph) == 0, "connections stay cleared");

	/* Give node B its input back with an explicit id. */
	vgui_editor_node_port_default(&new_port);
	new_port.id = 4001;
	new_port.name = "flow_in";
	new_port.direction = VGUI_EDITOR_NODE_PORT_INPUT;
	check(vgui_editor_node_graph_add_port(graph, 3002, &new_port) == 4001,
		"an explicit port id is kept");
	check(vgui_editor_node_graph_find_port(graph, 4001, &read_port, &found_node,
		&direction) == 1 && found_node == 3002, "the port found its node");
	(void)read_port;
	(void)direction;

	/* Selection. */
	vgui_editor_node_graph_set_on_selection(graph, ng_on_selection, p);
	vgui_editor_node_graph_set_multi_select(graph, 1);
	check(vgui_editor_node_graph_multi_select(graph) == 1, "graph multi select on");
	vgui_editor_node_graph_set_multi_select(graph, 0);
	check(vgui_editor_node_graph_multi_select(graph) == 0, "graph multi select off");
	check(vgui_editor_node_graph_selection_count(graph) == 0, "nothing selected yet");
	check(vgui_editor_node_graph_primary_selection(graph)
		== VGUI_EDITOR_INVALID_OBJECT, "there is no primary node yet");
	check(vgui_editor_node_graph_is_selected(graph, 3001) == 0, "the node is not selected");
	check(vgui_editor_node_graph_selection_id_at(graph, 0)
		== VGUI_EDITOR_INVALID_OBJECT, "an empty selection lists nothing");
	ids[0] = 3001;
	ids[1] = 3002;
	p->selection_count = 0;
	vgui_editor_node_graph_set_selection(graph, ids, 2, 3001);
	check(vgui_editor_node_graph_selection_count(graph) == 2, "two nodes selected");
	check(vgui_editor_node_graph_primary_selection(graph) == 3001,
		"the primary id is remembered");
	check(vgui_editor_node_graph_selection_id_at(graph, 1) == 3002, "ids listed in order");
	check(vgui_editor_node_graph_is_selected(graph, 3002) == 1, "is_selected agrees");
	check(p->selection_count == 1, "the selection listener ran once");
	vgui_editor_node_graph_select_all(graph);
	check(vgui_editor_node_graph_selection_count(graph)
		== vgui_editor_node_graph_node_count(graph), "select all covers every node");
	rect = vgui_editor_node_graph_selection_bounds(graph);
	check(rect.width >= 0.0 && rect.height >= 0.0, "selection bounds are non-negative");
	vgui_editor_node_graph_clear_selection(graph);
	check(vgui_editor_node_graph_selection_count(graph) == 0, "the selection cleared");

	/* View, zoom, pan and coordinate conversion. */
	memset(&read_view, 0, sizeof(read_view));
	vgui_editor_node_graph_get_view_state(graph, &read_view);
	check(read_view.struct_size == (uint32_t)sizeof(read_view),
		"the view state reads its size");
	vgui_editor_node_graph_get_view_state(graph, NULL);
	vgui_editor_node_view_state_default(&view);
	view.offset_x = 10.0;
	view.offset_y = 20.0;
	view.zoom = 2.0;
	vgui_editor_node_graph_set_view_state(graph, &view);
	check(vgui_editor_node_graph_zoom(graph) == 2.0, "the view state sets the zoom");
	point = vgui_editor_node_graph_graph_to_screen(graph, 110.0, 120.0);
	back = vgui_editor_node_graph_screen_to_graph(graph, (float)point.x, (float)point.y);
	check(nearly(back.x, 110.0) && nearly(back.y, 120.0),
		"graph <-> screen round-trips");
	vgui_editor_node_graph_set_zoom_limits(graph, 0.5, 3.0);
	vgui_editor_node_graph_zoom_at(graph, 20.0f, 20.0f, 100.0);
	check(vgui_editor_node_graph_zoom(graph) <= 3.0 + 1e-9,
		"zooming clamps at the maximum");
	vgui_editor_node_graph_reset_view(graph);
	check(vgui_editor_node_graph_zoom(graph) > 0.0, "reset keeps a positive zoom");
	vgui_editor_node_graph_pan_by_pixels(graph, 30.0f, 10.0f);
	check(vgui_editor_node_graph_zoom(graph) > 0.0, "panning keeps the view valid");
	vgui_editor_node_graph_fit_all(graph);
	check(vgui_editor_node_graph_zoom(graph) > 0.0, "fit_all leaves a positive zoom");
	rect = vgui_editor_node_graph_content_bounds(graph);
	check(rect.width > 0.0, "the content bounds cover the nodes");
	rect = vgui_editor_node_graph_node_bounds(graph, 3001);
	check(rect.width > 0.0, "the node bounds report a width");
	rect = vgui_editor_node_graph_node_bounds(graph, 4242);
	check(rect.width == 0.0, "an unknown node has no bounds");
	rect = vgui_editor_node_graph_port_bounds(graph, 4002);
	(void)rect;
	check(vgui_editor_node_graph_port_position(graph, 4002, &point) == 1,
		"the port position was reported");
	check(vgui_editor_node_graph_port_position(graph, 4242, &point) == 0,
		"an unknown port has no position");
	vgui_editor_node_graph_port_position(graph, 4002, NULL);
	check(vgui_editor_node_graph_connection_tolerance(graph) > 0.0,
		"the connection tolerance is positive");

	/* Grid, snapping and minimap. */
	vgui_editor_node_graph_set_grid_visible(graph, 0);
	check(vgui_editor_node_graph_grid_visible(graph) == 0, "the grid is hidden");
	vgui_editor_node_graph_set_grid_visible(graph, 1);
	check(vgui_editor_node_graph_grid_visible(graph) == 1, "the grid is shown");
	vgui_editor_node_graph_set_grid_step(graph, 10.0);
	check(vgui_editor_node_graph_grid_step(graph) == 10.0, "the grid step round-trips");
	check(vgui_editor_node_graph_effective_grid_step(graph) > 0.0,
		"the effective grid step is positive");
	/* snap_point follows the zoom-adaptive step, so pin the view to keep the
	 * configured step effective. */
	vgui_editor_node_view_state_default(&view);
	view.zoom = 2.0;
	vgui_editor_node_graph_set_view_state(graph, &view);
	vgui_editor_node_graph_set_snap_to_grid(graph, 1);
	check(vgui_editor_node_graph_snap_to_grid(graph) == 1, "snapping to grid is on");
	point.x = 13.0;
	point.y = 17.0;
	point = vgui_editor_node_graph_snap_point(graph, point);
	check(nearly(point.x, 10.0) && nearly(point.y, 20.0), "the point snapped to the grid");
	vgui_editor_node_graph_set_snap_to_grid(graph, 0);
	point.x = 13.0;
	point.y = 17.0;
	point = vgui_editor_node_graph_snap_point(graph, point);
	check(nearly(point.x, 13.0) && nearly(point.y, 17.0),
		"a disabled snap leaves the point alone");
	vgui_editor_node_graph_set_minimap_visible(graph, 0);
	check(vgui_editor_node_graph_minimap_visible(graph) == 0, "the minimap is hidden");
	check(vgui_editor_node_graph_minimap_rect(graph, NULL, NULL, NULL, NULL) == 0,
		"a hidden minimap reports no rect");
	vgui_editor_node_graph_set_minimap_visible(graph, 1);
	check(vgui_editor_node_graph_minimap_visible(graph) == 1, "the minimap is shown");

	/* Picking and interaction state. */
	check(vgui_editor_node_graph_node_hit(graph, 1.0e5f, 1.0e5f)
		== VGUI_EDITOR_INVALID_OBJECT, "an off-screen node hit finds nothing");
	check(vgui_editor_node_graph_port_hit(graph, 1.0e5f, 1.0e5f)
		== VGUI_EDITOR_INVALID_OBJECT, "an off-screen port hit finds nothing");
	check(vgui_editor_node_graph_connection_hit(graph, 1.0e5f, 1.0e5f)
		== VGUI_EDITOR_INVALID_OBJECT, "an off-screen link hit finds nothing");
	check(vgui_editor_node_graph_hit_test(graph, 1.0e5f, 1.0e5f)
		== VGUI_EDITOR_INVALID_OBJECT, "the off-screen hit test finds nothing");
	check(vgui_editor_node_graph_dragging(graph) == 0, "nothing is being dragged");
	check(vgui_editor_node_graph_get_gesture(graph) == VGUI_EDITOR_NODE_GESTURE_NONE,
		"the gesture state starts idle");
	check(vgui_editor_node_graph_pending_port(graph) == VGUI_EDITOR_INVALID_OBJECT,
		"no connection gesture is pending");
	vgui_editor_node_graph_cancel_gesture(graph);

	/* Undo stack and the connect/delete user actions. */
	stack = vgui_editor_command_stack_create();
	vgui_editor_node_graph_set_command_stack(graph, stack);
	ids[0] = 4002;
	ids[1] = 4001;
	found_port = vgui_editor_node_graph_connect_ports(graph, 4002, 4001);
	check(found_port == 1 || found_port == 0, "connect_ports answers cleanly");
	check(vgui_editor_command_stack_can_undo(stack)
		== (found_port == 1 ? 1 : 0), "an applied link recorded one command");
	vgui_editor_command_stack_clear(stack);
	vgui_editor_node_graph_set_command_stack(graph, NULL);
	vgui_editor_command_stack_destroy(stack);

	/* Node creation callback. */
	p->combo_index = 0;
	p->action_count = 0;
	p->released = 0;
	vgui_editor_node_graph_set_create_node(graph, ng_create_node, p, release_probe);
	check(vgui_editor_node_graph_perform_action(graph,
		VGUI_EDITOR_NODE_ACTION_ADD_NODE) == 1, "the add-node action created a node");
	check(p->action_count == 1, "the create handler ran once");
	check(vgui_editor_node_graph_node_count(graph) == 4, "the graph gained a node");
	memset(&read_port, 0, sizeof(read_port));
	found_node = VGUI_EDITOR_INVALID_OBJECT;
	check(vgui_editor_node_graph_find_port(graph, 4100, &read_port, &found_node,
		&direction) == 1, "the host port landed in the graph");
	check(found_node != 3001 && found_node != 3002 && found_node != 3003,
		"the host node took a fresh id");
	check(vgui_editor_node_graph_find_node(graph, found_node, &read_node) == 1
		&& read_node.title && strcmp(read_node.title, "From Host") == 0,
		"the host-supplied title was stored");
	p->combo_index = 1;
	check(vgui_editor_node_graph_perform_action(graph,
		VGUI_EDITOR_NODE_ACTION_ADD_NODE) == 0, "a cancelled handler adds nothing");
	check(vgui_editor_node_graph_node_count(graph) == 4, "the cancelled create kept the count");
	vgui_editor_node_graph_set_create_node(graph, NULL, NULL, NULL);
	check(p->released == 1, "clearing the create handler released the user once");

	/* Toggle verbs flip their own state. */
	vgui_editor_node_graph_set_grid_visible(graph, 1);
	check(vgui_editor_node_graph_perform_action(graph,
		VGUI_EDITOR_NODE_ACTION_TOGGLE_GRID) == 1, "toggle grid changed something");
	check(vgui_editor_node_graph_grid_visible(graph) == 0, "the toggle hid the grid");
	vgui_editor_node_graph_set_snap_to_grid(graph, 0);
	check(vgui_editor_node_graph_perform_action(graph,
		VGUI_EDITOR_NODE_ACTION_TOGGLE_SNAP) == 1, "toggle snap changed something");
	check(vgui_editor_node_graph_snap_to_grid(graph) == 1, "the toggle enabled snapping");
	check(vgui_editor_node_graph_perform_action(graph,
		VGUI_EDITOR_NODE_ACTION_SELECT_ALL) == 1, "select all changed the selection");
	vgui_editor_node_graph_clear_selection(graph);
	check(vgui_editor_node_graph_perform_action(graph,
		VGUI_EDITOR_NODE_ACTION_DELETE_SELECTION) == 0,
		"deleting an empty selection changes nothing");
	ids[0] = 3001;
	vgui_editor_node_graph_set_selection(graph, ids, 1, 3001);
	check(vgui_editor_node_graph_perform_action(graph,
		VGUI_EDITOR_NODE_ACTION_DUPLICATE) == 1, "duplicate made a change");
	/* Duplicate leaves the copies selected, so re-select the linked node. */
	ids[0] = 3001;
	vgui_editor_node_graph_set_selection(graph, ids, 1, 3001);
	check(vgui_editor_node_graph_perform_action(graph,
		VGUI_EDITOR_NODE_ACTION_DISCONNECT) == 1, "disconnect dropped the links");
	vgui_editor_node_graph_frame_selection(graph);
	check(vgui_editor_node_graph_menu(graph) != NULL, "the graph owns a menu widget");
	check(vgui_editor_node_graph_menu(graph) == vgui_editor_node_graph_menu(graph),
		"the borrowed menu handle is stable");
	vgui_editor_node_graph_open_context_menu(graph, 1.0e5f, 1.0e5f);

	/* Search. */
	vgui_editor_node_graph_set_search(graph, "consumer");
	needed = vgui_editor_node_graph_search(graph, NULL, 0);
	check(needed > 0, "the search text reports its length");
	check(vgui_editor_node_graph_search(graph, text, sizeof(text)) < sizeof(text),
		"the search text fits the buffer");
	check(strcmp(text, "consumer") == 0, "the search text round-trips");
	check(vgui_editor_node_graph_match_count(graph) == 1, "one node matched");
	check(vgui_editor_node_graph_match_at(graph, 0) == 3002, "the match id was listed");
	check(vgui_editor_node_graph_match_at(graph, 9) == VGUI_EDITOR_INVALID_OBJECT,
		"the match index is bounded");
	check(vgui_editor_node_graph_node_matches(graph, 3002) == 1, "the matcher agrees");
	check(vgui_editor_node_graph_node_matches(graph, 3001) == 0,
		"a non-matching node is reported as such");
	vgui_editor_node_graph_focus_search(graph);
	check(vgui_editor_node_graph_search_widget(graph) != NULL, "the search widget exists");
	check(vgui_editor_node_graph_next_match(graph) == 3002, "next_match found the node");
	vgui_editor_node_graph_clear_search(graph);
	check(vgui_editor_node_graph_match_count(graph) == 0, "clearing search clears matches");

	/* State JSON round-trip. */
	vgui_editor_node_view_state_default(&view);
	view.offset_x = 33.0;
	view.offset_y = 44.0;
	view.zoom = 1.5;
	vgui_editor_node_graph_set_view_state(graph, &view);
	vgui_editor_node_graph_set_grid_step(graph, 16.0);
	vgui_editor_node_graph_set_grid_visible(graph, 0);
	vgui_editor_node_graph_set_snap_to_grid(graph, 1);
	needed = vgui_editor_node_graph_capture_state_json(graph, NULL, 0);
	check(needed > 0, "the graph state json reports its length");
	check(vgui_editor_node_graph_capture_state_json(graph, json, sizeof(json))
		< sizeof(json), "the graph state json fits the buffer");
	vgui_editor_node_graph_reset_view(graph);
	vgui_editor_node_graph_set_grid_step(graph, 8.0);
	vgui_editor_node_graph_set_grid_visible(graph, 1);
	vgui_editor_node_graph_set_snap_to_grid(graph, 0);
	check(vgui_editor_node_graph_apply_state_json(graph, json) == 1,
		"the graph state json applies");
	memset(&read_view, 0, sizeof(read_view));
	vgui_editor_node_graph_get_view_state(graph, &read_view);
	check(nearly(read_view.offset_x, 33.0) && nearly(read_view.offset_y, 44.0),
		"the pan survived the round-trip");
	check(nearly(read_view.zoom, 1.5), "the zoom survived the round-trip");
	check(vgui_editor_node_graph_grid_step(graph) == 16.0, "the grid step survived");
	check(vgui_editor_node_graph_grid_visible(graph) == 0, "the grid flag survived");
	check(vgui_editor_node_graph_snap_to_grid(graph) == 1, "the snap flag survived");
	check(vgui_editor_node_graph_apply_state_json(graph, "{ bad") == 0,
		"garbage graph json is rejected");
	check(vgui_editor_node_graph_apply_state_json(graph, NULL) == 0,
		"a NULL graph json is rejected");

	/* Teardown. */
	vgui_editor_node_graph_set_on_selection(graph, NULL, NULL);
	vgui_editor_node_graph_set_on_change(graph, NULL, NULL);
	vgui_editor_node_graph_set_create_node(graph, NULL, NULL, NULL);
	vgui_editor_node_graph_clear_selection(graph);
	vgui_editor_node_graph_clear_connections(graph);
	vgui_editor_node_graph_clear_nodes(graph);
	check(vgui_editor_node_graph_node_count(graph) == 0, "clear_nodes empties the graph");
	vgui_editor_node_graph_destroy(graph);
	check(vgui_editor_node_graph_widget(NULL) == NULL, "a NULL graph has no widget");
	check(vgui_editor_node_graph_node_count(NULL) == 0, "a NULL graph has no nodes");
	check(vgui_editor_node_graph_connection_count(NULL) == 0, "a NULL graph has no links");
	check(vgui_editor_node_graph_menu(NULL) == NULL, "a NULL graph has no menu");
	check(vgui_editor_node_graph_node_hit(NULL, 0.0f, 0.0f)
		== VGUI_EDITOR_INVALID_OBJECT, "a NULL graph hits nothing");
	check(vgui_editor_node_graph_perform_action(NULL,
		VGUI_EDITOR_NODE_ACTION_SELECT_ALL) == 0, "a NULL graph performs no action");
	vgui_editor_node_graph_destroy(NULL);
	vgui_editor_node_graph_search(NULL, text, sizeof(text));
	(void)found_port;
	(void)ids;
}

/* -------------------------------------------------------------------------- */
/* 2D canvas                                                                  */
/* -------------------------------------------------------------------------- */

static void cv_on_selection(void* user, const vgui_editor_object_id* ids, size_t count)
{
	probe* p = (probe*)user;
	++p->selection_count;
	p->navigate_index = count;
	p->last_selection = count > 0 && ids ? ids[0] : VGUI_EDITOR_INVALID_OBJECT;
}

static void cv_on_change(void* user)
{
	probe* p = (probe*)user;
	++p->change_count;
}

void test_canvas(probe* p)
{
	vgui_editor_canvas* canvas;
	vgui_editor_canvas_item item;
	vgui_editor_canvas_item read_item;
	vgui_editor_canvas_view_state view;
	vgui_editor_canvas_view_state read_view;
	vgui_editor_canvas_point point;
	vgui_editor_canvas_point back;
	vgui_editor_canvas_rect rect;
	vgui_editor_snap_settings snap;
	vgui_editor_snap_settings read_snap;
	vgui_editor_command_stack* stack;
	vgui_editor_object_id ids[2];
	vgui_editor_object_id item_b;
	vgui_editor_object_id auto_id;

	memset(p, 0, sizeof(*p));
	memset(&item, 0, sizeof(item));
	memset(&read_item, 0, sizeof(read_item));
	memset(&view, 0, sizeof(view));
	memset(&read_view, 0, sizeof(read_view));
	memset(&point, 0, sizeof(point));
	memset(&back, 0, sizeof(back));
	memset(&rect, 0, sizeof(rect));
	memset(&snap, 0, sizeof(snap));
	memset(&read_snap, 0, sizeof(read_snap));
	memset(ids, 0, sizeof(ids));

	canvas = vgui_editor_canvas_create(vgui_root(g_ui));
	check(canvas != NULL, "canvas built");
	check(vgui_editor_canvas_create(NULL) == NULL, "canvas refuses a NULL parent");
	check(vgui_editor_canvas_widget(canvas) != NULL, "canvas exposes its widget");
	check(vgui_editor_canvas_built(canvas) == 1, "canvas reports itself built");
	check(vgui_editor_canvas_enabled(canvas) == 1, "canvas starts enabled");
	vgui_editor_canvas_set_enabled(canvas, 0);
	check(vgui_editor_canvas_enabled(canvas) == 0, "canvas can be disabled");
	vgui_editor_canvas_set_enabled(canvas, 1);
	check(vgui_editor_canvas_focused(canvas) == 0, "canvas starts unfocused");
	vgui_editor_canvas_focus(canvas);
	vgui_editor_canvas_set_drag_threshold(canvas, 3.0f);
	check(vgui_editor_canvas_drag_threshold(canvas) == 3.0f,
		"the drag threshold round-trips");
	vgui_editor_canvas_set_min_item_size(canvas, 8.0f);

	/* Descriptor defaults. */
	vgui_editor_snap_settings_default(&snap);
	check(snap.struct_size == (uint32_t)sizeof(snap), "snap default stamps its size");
	check(snap.enabled == 1 && snap.translate == 1 && snap.rotate == 1
		&& snap.scale == 1, "the snap default enables every axis");
	check(snap.translate_step == 0.5f && snap.rotate_step_deg == 15.0f
		&& snap.scale_step == 0.1f, "the snap default carries its steps");
	vgui_editor_snap_settings_default(NULL);
	vgui_editor_canvas_item_default(&item);
	check(item.struct_size == (uint32_t)sizeof(item), "item default stamps its size");
	check(item.id == VGUI_EDITOR_INVALID_OBJECT, "the item default has no id");
	check(item.bounds.width == 100.0f && item.bounds.height == 60.0f,
		"the item default carries a 100x60 frame");
	check(item.shape == VGUI_EDITOR_CANVAS_RECTANGLE, "the item default is a rectangle");
	check(item.fill == 0xFF2E4A6Au && item.border == 0xFF7FB6FFu,
		"the item default carries theme colours");
	vgui_editor_canvas_item_default(NULL);
	vgui_editor_canvas_view_state_default(&view);
	check(view.struct_size == (uint32_t)sizeof(view),
		"canvas view state stamps its size");
	check(view.zoom == 1.0f && view.offset_x == 0.0f,
		"the canvas view default is 1:1 at the origin");
	vgui_editor_canvas_view_state_default(NULL);

	/* Model CRUD. */
	vgui_editor_canvas_item_default(&item);
	item.id = 500;
	item.bounds.x = 20.0f;
	item.bounds.y = 30.0f;
	item.label = "Box";
	item.rotation = 0.25f;
	item.border_width = 2.0f;
	item.corner_radius = 4.0f;
	check(vgui_editor_canvas_add_item(canvas, &item) == 500,
		"an explicit item id is kept");
	check(vgui_editor_canvas_add_item(canvas, &item) == VGUI_EDITOR_INVALID_OBJECT,
		"a duplicate item id is rejected");
	item.id = 0;
	item.label = "Oval";
	item.shape = VGUI_EDITOR_CANVAS_ELLIPSE;
	item_b = vgui_editor_canvas_add_item(canvas, &item);
	check(item_b != VGUI_EDITOR_INVALID_OBJECT, "a zero id is allocated on add");
	check(vgui_editor_canvas_item_count(canvas) == 2, "two items in the model");
	memset(&read_item, 0, sizeof(read_item));
	check(vgui_editor_canvas_find_item(canvas, 500, &read_item) == 1, "the item was found");
	check(read_item.struct_size == (uint32_t)sizeof(read_item),
		"the read item stamps its size");
	check(read_item.label && strcmp(read_item.label, "Box") == 0, "the label round-trips");
	check(nearly((double)read_item.bounds.x, 20.0)
		&& nearly((double)read_item.bounds.width, 100.0), "the bounds round-trip");
	check(nearly((double)read_item.rotation, 0.25), "the rotation round-trips");
	check(read_item.shape == VGUI_EDITOR_CANVAS_RECTANGLE, "the shape round-trips");
	check(nearly((double)read_item.border_width, 2.0)
		&& nearly((double)read_item.corner_radius, 4.0), "the stroke settings round-trip");
	check(vgui_editor_canvas_get_item(canvas, 1, &read_item) == 1
		&& read_item.shape == VGUI_EDITOR_CANVAS_ELLIPSE, "the item is readable by index");
	check(vgui_editor_canvas_get_item(canvas, 9, &read_item) == 0,
		"the item index is bounded");
	check(vgui_editor_canvas_find_item(canvas, 4242, &read_item) == 0,
		"an unknown item is not found");

	/* struct_size prefix: fields the host does not know about land as zero. */
	memset(&item, 0, sizeof(item));
	item.struct_size = (uint32_t)offsetof(vgui_editor_canvas_item, bounds);
	item.id = 502;
	item.bounds.x = 7.0f;
	auto_id = vgui_editor_canvas_add_item(canvas, &item);
	check(auto_id == 502, "a short item struct still added");
	memset(&read_item, 0, sizeof(read_item));
	check(vgui_editor_canvas_find_item(canvas, 502, &read_item) == 1,
		"the short item is readable back");
	check(read_item.id == 502, "the id inside the prefix survived");
	check(read_item.bounds.width == 0.0f && read_item.bounds.height == 0.0f,
		"bounds beyond the prefix were dropped");

	/* Update and remove. */
	vgui_editor_canvas_item_default(&item);
	item.id = 500;
	item.label = "Renamed";
	item.bounds.x = 80.0f;
	item.visible = 0;
	item.locked = 1;
	check(vgui_editor_canvas_update_item(canvas, &item) == 1, "the item updated");
	memset(&read_item, 0, sizeof(read_item));
	check(vgui_editor_canvas_find_item(canvas, 500, &read_item) == 1
		&& read_item.visible == 0 && read_item.locked == 1,
		"the item flags survived the update");
	check(read_item.label && strcmp(read_item.label, "Renamed") == 0,
		"the item label survived the update");
	item.id = 4242;
	check(vgui_editor_canvas_update_item(canvas, &item) == 0,
		"updating an unknown item fails");
	check(vgui_editor_canvas_remove_item(canvas, 502) == 1, "the item was removed");
	check(vgui_editor_canvas_remove_item(canvas, 502) == 0,
		"removing the same item twice fails");
	check(vgui_editor_canvas_item_count(canvas) == 2, "the model is back to two items");

	/* Selection. */
	vgui_editor_canvas_set_on_selection(canvas, cv_on_selection, p);
	vgui_editor_canvas_set_multi_select(canvas, 1);
	check(vgui_editor_canvas_multi_select(canvas) == 1, "canvas multi select on");
	vgui_editor_canvas_set_multi_select(canvas, 0);
	check(vgui_editor_canvas_multi_select(canvas) == 0, "canvas multi select off");
	check(vgui_editor_canvas_selection_count(canvas) == 0, "nothing selected yet");
	check(vgui_editor_canvas_primary_selection(canvas)
		== VGUI_EDITOR_INVALID_OBJECT, "there is no primary item yet");
	check(vgui_editor_canvas_is_selected(canvas, 500) == 0, "the item is not selected");
	check(vgui_editor_canvas_selection_id_at(canvas, 0)
		== VGUI_EDITOR_INVALID_OBJECT, "an empty selection lists nothing");
	rect = vgui_editor_canvas_selection_bounds(canvas);
	check(rect.width == 0.0f && rect.height == 0.0f, "an empty selection has no bounds");
	ids[0] = item_b;
	ids[1] = 500;
	p->selection_count = 0;
	vgui_editor_canvas_set_selection(canvas, ids, 2, item_b);
	check(vgui_editor_canvas_selection_count(canvas) == 2, "two items selected");
	check(vgui_editor_canvas_primary_selection(canvas) == item_b,
		"the primary id is remembered");
	check(vgui_editor_canvas_selection_id_at(canvas, 1) == 500, "ids listed in order");
	check(vgui_editor_canvas_is_selected(canvas, item_b) == 1, "is_selected agrees");
	check(p->selection_count == 1, "the selection listener ran once");
	check(vgui_editor_canvas_hit_test(canvas, 1.0e5f, 1.0e5f)
		== VGUI_EDITOR_INVALID_OBJECT, "an off-screen hit test finds nothing");
	check(vgui_editor_canvas_handle_at(canvas, 1.0e5f, 1.0e5f)
		== VGUI_EDITOR_CANVAS_HANDLE_NONE, "an off-screen point hits no handle");
	rect = vgui_editor_canvas_selection_bounds(canvas);
	check(rect.width >= 0.0f, "a selection reports bounds");
	vgui_editor_canvas_clear_selection(canvas);
	check(vgui_editor_canvas_selection_count(canvas) == 0, "the selection cleared");

	/* Change listener. */
	vgui_editor_canvas_set_on_change(canvas, cv_on_change, p);
	p->change_count = 0;
	vgui_editor_canvas_set_grid_step(canvas, 16.0f);
	check(p->change_count >= 1, "a view change notified the listeners");

	/* View, zoom, pan and coordinate conversion. */
	memset(&read_view, 0, sizeof(read_view));
	vgui_editor_canvas_get_view_state(canvas, &read_view);
	check(read_view.struct_size == (uint32_t)sizeof(read_view),
		"the canvas view state reads its size");
	vgui_editor_canvas_get_view_state(canvas, NULL);
	vgui_editor_canvas_view_state_default(&view);
	view.offset_x = 20.0f;
	view.offset_y = 30.0f;
	view.zoom = 2.0f;
	vgui_editor_canvas_set_view_state(canvas, &view);
	check(vgui_editor_canvas_zoom(canvas) == 2.0f, "the view state sets the zoom");
	point = vgui_editor_canvas_canvas_to_screen(canvas, 70.0f, 80.0f);
	back = vgui_editor_canvas_screen_to_canvas(canvas, point.x, point.y);
	check(nearly((double)back.x, 70.0) && nearly((double)back.y, 80.0),
		"canvas <-> screen round-trips");
	vgui_editor_canvas_set_zoom_limits(canvas, 0.5f, 3.0f);
	vgui_editor_canvas_zoom_at(canvas, 10.0f, 10.0f, 100.0f);
	check(vgui_editor_canvas_zoom(canvas) <= 3.0f + 1e-5f,
		"zooming clamps at the maximum");
	vgui_editor_canvas_zoom_at(canvas, 10.0f, 10.0f, 0.0001f);
	check(vgui_editor_canvas_zoom(canvas) >= 0.5f - 1e-5f,
		"zooming clamps at the minimum");
	vgui_editor_canvas_reset_view(canvas);
	check(vgui_editor_canvas_zoom(canvas) > 0.0f, "reset keeps a positive zoom");

	/* Grid. */
	vgui_editor_canvas_set_grid_visible(canvas, 0);
	check(vgui_editor_canvas_grid_visible(canvas) == 0, "the canvas grid is hidden");
	vgui_editor_canvas_set_grid_visible(canvas, 1);
	check(vgui_editor_canvas_grid_visible(canvas) == 1, "the canvas grid is shown");
	vgui_editor_canvas_set_grid_step(canvas, 24.0f);
	check(vgui_editor_canvas_grid_step(canvas) == 24.0f, "the grid step round-trips");
	vgui_editor_canvas_set_grid_pixel_range(canvas, 8.0f, 128.0f);
	check(vgui_editor_canvas_effective_grid_step(canvas) > 0.0f,
		"the effective grid step is positive");

	/* Snap settings. */
	vgui_editor_snap_settings_default(&snap);
	snap.enabled = 0;
	snap.translate = 0;
	snap.translate_step = 2.0f;
	snap.rotate_step_deg = 45.0f;
	snap.scale_step = 0.25f;
	vgui_editor_canvas_set_snap(canvas, &snap);
	memset(&read_snap, 0, sizeof(read_snap));
	vgui_editor_canvas_get_snap(canvas, &read_snap);
	check(read_snap.struct_size == (uint32_t)sizeof(read_snap),
		"get_snap stamps the caller's struct size");
	check(read_snap.enabled == 0 && read_snap.translate == 0,
		"the snap flags survived the round-trip");
	check(nearly((double)read_snap.translate_step, 2.0), "the translate step survived");
	check(nearly((double)read_snap.rotate_step_deg, 45.0), "the rotate step survived");
	check(nearly((double)read_snap.scale_step, 0.25), "the scale step survived");
	/* A short struct zeroes the fields the host does not know about. */
	snap.struct_size = (uint32_t)offsetof(vgui_editor_snap_settings, translate_step);
	snap.enabled = 1;
	snap.translate = 1;
	vgui_editor_canvas_set_snap(canvas, &snap);
	memset(&read_snap, 0, sizeof(read_snap));
	vgui_editor_canvas_get_snap(canvas, &read_snap);
	check(read_snap.enabled == 1 && read_snap.translate == 1,
		"a partial snap struct applied its documented fields");
	check(read_snap.scale_step == 0.0f, "the steps beyond the prefix became zero");
	/* NULL detaches the policy entirely (adapter behaviour, not header prose). */
	vgui_editor_canvas_set_snap(canvas, NULL);
	memset(&read_snap, 0xFF, sizeof(read_snap));
	vgui_editor_canvas_get_snap(canvas, &read_snap);
	check(read_snap.enabled == 0, "a NULL snap reports snapping as off");
	vgui_editor_snap_settings_default(&snap);
	vgui_editor_canvas_set_snap(canvas, &snap);
	vgui_editor_canvas_get_snap(canvas, NULL);

	/* Geometry and interaction state. */
	rect = vgui_editor_canvas_item_bounds(canvas, 500);
	check(rect.width >= 0.0f, "item bounds reported for a known item");
	rect = vgui_editor_canvas_item_bounds(canvas, 4242);
	check(rect.width == 0.0f && rect.height == 0.0f, "an unknown item has no bounds");
	check(vgui_editor_canvas_dragging(canvas) == 0, "nothing is being dragged");
	check(vgui_editor_canvas_get_gesture(canvas) == VGUI_EDITOR_CANVAS_GESTURE_NONE,
		"the gesture state starts idle");
	check(vgui_editor_canvas_active_handle(canvas) == VGUI_EDITOR_CANVAS_HANDLE_NONE,
		"no handle is active");
	check(vgui_editor_canvas_hovered_handle(canvas) == VGUI_EDITOR_CANVAS_HANDLE_NONE,
		"no handle is hovered");
	vgui_editor_canvas_cancel_gesture(canvas);
	stack = vgui_editor_command_stack_create();
	check(stack != NULL, "command stack built");
	vgui_editor_canvas_set_command_stack(canvas, stack);
	vgui_editor_canvas_set_command_stack(canvas, NULL);
	vgui_editor_command_stack_destroy(stack);

	/* Teardown.  The canvas section exposes no state JSON: the view state, the
	 * grid settings and the snap settings above are its persistent form. */
	vgui_editor_canvas_set_on_selection(canvas, NULL, NULL);
	vgui_editor_canvas_set_on_change(canvas, NULL, NULL);
	vgui_editor_canvas_clear_selection(canvas);
	vgui_editor_canvas_clear_items(canvas);
	check(vgui_editor_canvas_item_count(canvas) == 0, "clear_items empties the model");
	vgui_editor_canvas_destroy(canvas);
	check(vgui_editor_canvas_widget(NULL) == NULL, "a NULL canvas has no widget");
	check(vgui_editor_canvas_item_count(NULL) == 0, "a NULL canvas has no items");
	check(vgui_editor_canvas_selection_count(NULL) == 0, "a NULL canvas selects nothing");
	check(vgui_editor_canvas_built(NULL) == 0, "a NULL canvas is not built");
	check(vgui_editor_canvas_hit_test(NULL, 0.0f, 0.0f) == VGUI_EDITOR_INVALID_OBJECT,
		"a NULL canvas hits nothing");
	check(vgui_editor_canvas_handle_at(NULL, 0.0f, 0.0f)
		== VGUI_EDITOR_CANVAS_HANDLE_NONE, "a NULL canvas hits no handle");
	vgui_editor_canvas_destroy(NULL);
	vgui_editor_canvas_get_snap(NULL, &read_snap);
	vgui_editor_canvas_item_bounds(NULL, 500);
	(void)auto_id;
}
