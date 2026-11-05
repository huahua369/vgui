/* sample/vgui_editor_c_api_test.c: pure C consumer smoke test for vgui_editor.
 *
 * Compiled as C99 so it catches any accidental C++ dependency in the public
 * editor header.  It runs headless: vgui_create() without a font or GPU is
 * enough for widget construction, models, commands and JSON state, which is
 * what the editor facade exposes.
 */
#include "vgui_editor_c_api_test.h"

int g_pass;
int g_fail;
vgui_context* g_ui;

void check(int ok, const char* what)
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
	fflush(stdout);
}

void frame(int count)
{
	int i;
	for (i = 0; i < count; ++i)
		vgui_update(g_ui, 1.0f / 60.0f);
}

/* -------------------------------------------------------------------------- */
/* Editor context, selection                                                   */
/* -------------------------------------------------------------------------- */

static void on_selection(void* user, vgui_editor_object_id id)
{
	probe* p = (probe*)user;
	++p->selection_count;
	p->last_selection = id;
}

static void on_selection_set(void* user, const vgui_editor_object_id* ids,
	size_t count)
{
	probe* p = (probe*)user;
	(void)ids;
	++p->selection_set_count;
	(void)count;
}

static void test_context(probe* p)
{
	memset(p, 0, sizeof(*p));
	vgui_editor_context* ctx = vgui_editor_context_create(g_ui);
	size_t token, set_token, before;
	vgui_editor_object_id ids[3];

	check(ctx != NULL, "context create");
	check(vgui_editor_context_ui(ctx) == g_ui, "context exposes vgui_context");
	check(vgui_editor_context_root(ctx) == vgui_root(g_ui), "context exposes root");
	check(vgui_editor_context_commands(ctx) != NULL, "context owns a command stack");

	check(vgui_editor_context_selection(ctx) == VGUI_EDITOR_INVALID_OBJECT,
		"selection starts empty");
	vgui_editor_context_set_selection(ctx, 42);
	check(vgui_editor_context_selection(ctx) == 42, "set_selection applies");

	token = vgui_editor_context_add_selection_listener(ctx, on_selection, p);
	check(token != 0, "selection listener returns a token");
	set_token = vgui_editor_context_add_selection_set_listener(ctx,
		on_selection_set, p);
	check(set_token != 0, "selection set listener returns a token");

	vgui_editor_context_set_selection(ctx, 7);
	check(p->selection_count == 1 && p->last_selection == 7, "listener notified");

	ids[0] = 11;
	ids[1] = 12;
	ids[2] = 13;
	vgui_editor_context_set_selection_ids(ctx, ids, 3, 12);
	check(vgui_editor_context_selection_count(ctx) == 3, "selection set size");
	check(vgui_editor_context_selection(ctx) == 12, "selection set primary");
	check(vgui_editor_context_selection_id_at(ctx, 1) == 12, "selection id at");
	check(vgui_editor_context_is_selected(ctx, 13), "is_selected");
	check(vgui_editor_context_selection_id_at(ctx, 99) == VGUI_EDITOR_INVALID_OBJECT,
		"selection id out of range is invalid");

	vgui_editor_context_remove_from_selection(ctx, 12);
	check(!vgui_editor_context_is_selected(ctx, 12), "remove_from_selection");
	vgui_editor_context_toggle_selection(ctx, 20);
	check(vgui_editor_context_is_selected(ctx, 20), "toggle_selection adds");
	vgui_editor_context_toggle_selection(ctx, 20);
	check(!vgui_editor_context_is_selected(ctx, 20), "toggle_selection removes");
	check(p->selection_set_count >= 2, "set listener notified");

	vgui_editor_context_clear_selection(ctx);
	check(vgui_editor_context_selection_count(ctx) == 0, "clear_selection");
	before = p->selection_count;

	check(vgui_editor_context_remove_selection_listener(ctx, token),
		"remove_selection_listener");
	check(vgui_editor_context_remove_selection_listener(ctx, token) == 0,
		"removing a stale token fails");
	vgui_editor_context_set_selection(ctx, 99);
	check(p->selection_count == before, "removed listener is silent");
	vgui_editor_context_remove_selection_set_listener(ctx, set_token);
	vgui_editor_context_destroy(ctx);
	check(vgui_editor_context_selection(NULL) == VGUI_EDITOR_INVALID_OBJECT,
		"NULL handle is a no-op");
}

/* -------------------------------------------------------------------------- */
/* Command stack                                                               */
/* -------------------------------------------------------------------------- */

typedef struct counter {
	int value;
	int releases;
	int undo_calls;
	int reads;
	int writes;
} counter;

static int bump_execute(void* user)
{
	counter* c = (counter*)user;
	++c->value;
	return 1;
}

static void bump_undo(void* user)
{
	counter* c = (counter*)user;
	--c->value;
	c->undo_calls++;
}

static void counter_release(void* user)
{
	counter* c = (counter*)user;
	++c->releases;
}

static int failing_execute(void* user)
{
	counter* c = (counter*)user;
	c->value += 100;
	return 0;
}

static void test_command_stack(void)
{
	vgui_editor_command_stack* stack = vgui_editor_command_stack_create();
	counter c;
	int before;
	memset(&c, 0, sizeof(c));

	check(stack != NULL, "standalone command stack");
	check(vgui_editor_command_stack_can_undo(stack) == 0, "stack starts empty");

	vgui_editor_command_stack_execute(stack, "bump", bump_execute, bump_undo, &c,
		counter_release);
	check(c.value == 1, "execute applies the edit");
	check(vgui_editor_command_stack_undo_count(stack) == 1, "recorded once");
	check(strcmp(vgui_editor_command_stack_next_undo_name(stack), "bump") == 0,
		"undo name");

	vgui_editor_command_stack_undo(stack);
	check(c.value == 0, "undo reverts the edit");
	check(vgui_editor_command_stack_can_redo(stack), "redo available");
	check(vgui_editor_command_stack_redo(stack), "redo succeeds");
	check(c.value == 1, "redo re-applies the edit");

	/* A rejected edit is not recorded and the stack stays untouched. */
	check(vgui_editor_command_stack_execute(stack, "bad", failing_execute, bump_undo,
		&c, NULL) == 0, "failed execute returns 0");
	check(c.value == 101, "failed execute still ran the edit");
	check(vgui_editor_command_stack_undo_count(stack) == 1, "failed edit not recorded");

	vgui_editor_command_stack_begin_transaction(stack, "drag");
	check(vgui_editor_command_stack_in_transaction(stack), "in_transaction");
	before = c.value;
	vgui_editor_command_stack_execute(stack, "bump", bump_execute, bump_undo, &c, NULL);
	vgui_editor_command_stack_execute(stack, "bump", bump_execute, bump_undo, &c, NULL);
	check(c.value == before + 2, "transaction applies every edit");
	check(vgui_editor_command_stack_commit_transaction(stack), "commit transaction");
	check(vgui_editor_command_stack_undo_count(stack) == 2,
		"transaction is one undo record");
	vgui_editor_command_stack_undo(stack);
	check(c.value == before, "undo rewinds the whole transaction");

	vgui_editor_command_stack_begin_transaction(stack, "rollback");
	vgui_editor_command_stack_execute(stack, "bump", bump_execute, bump_undo, &c, NULL);
	vgui_editor_command_stack_rollback_transaction(stack);
	check(vgui_editor_command_stack_in_transaction(stack) == 0, "rollback ends group");

	/* push() records an already applied edit; redo runs the reapply callback. */
	c.value = 5;
	vgui_editor_command_stack_push(stack, "set", bump_execute, bump_undo, &c, NULL);
	check(vgui_editor_command_stack_undo_count(stack) == 2, "push records");
	check(c.value == 5, "push does not re-run the edit");

	vgui_editor_command_stack_clear(stack);
	check(vgui_editor_command_stack_undo_count(stack) == 0, "clear empties history");
	check(c.releases >= 1, "release callback ran for owned records");

	/* An editor-context stack is borrowed: destroy() must leave it usable. */
	{
		vgui_editor_context* ctx = vgui_editor_context_create(g_ui);
		vgui_editor_command_stack* owned = vgui_editor_command_stack_create();
		vgui_editor_command_stack* borrowed = vgui_editor_context_commands(ctx);
		vgui_editor_command_stack_destroy(borrowed);
		vgui_editor_command_stack_execute(borrowed, "bump", bump_execute, bump_undo,
			&c, NULL);
		check(vgui_editor_command_stack_undo_count(borrowed) == 1,
			"borrowed stack still records after a destroy call");
		vgui_editor_command_stack_undo(borrowed);
		vgui_editor_context_destroy(ctx);
		vgui_editor_command_stack_destroy(owned);
	}
	vgui_editor_command_stack_destroy(stack);
	/* The context-owned stack refuses destroy(), so a borrowed handle stays
	 * valid; the owned handle above is freed and never touched again. */
	check(vgui_editor_command_stack_undo(NULL) == 0, "NULL stack is a no-op");
}

/* -------------------------------------------------------------------------- */
/* Command registry                                                            */
/* -------------------------------------------------------------------------- */

static void action_count(void* user)
{
	probe* p = (probe*)user;
	++p->action_count;
}

static int always_true(void* user)
{
	(void)user;
	return 1;
}

static int always_false(void* user)
{
	(void)user;
	return 0;
}

static void test_registry(probe* p)
{
	memset(p, 0, sizeof(*p));
	vgui_editor_command_registry* reg = vgui_editor_command_registry_create();
	vgui_editor_command_info info;
	char shortcut[32];
	size_t count;

	check(reg != NULL, "registry create");
	check(vgui_editor_command_registry_add(reg, "test.fire", "Fire", action_count, p,
		NULL), "add command");
	check(vgui_editor_command_registry_has(reg, "test.fire"), "has command");
	check(vgui_editor_command_registry_size(reg) == 1, "registry size");
	check(vgui_editor_command_registry_invoke(reg, "test.fire"), "invoke succeeds");
	check(p->action_count == 1, "action ran");
	check(vgui_editor_command_registry_invoke(reg, "test.missing") == 0,
		"unknown id does not run");

	check(vgui_editor_command_registry_set_shortcut(reg, "test.fire", "Ctrl+Shift+Z"),
		"set shortcut");
	count = vgui_editor_command_registry_get_shortcut(reg, "test.fire", shortcut,
		sizeof(shortcut));
	check(count > 0 && strcmp(shortcut, "Ctrl+Shift+Z") == 0, "shortcut round-trip");
	check(vgui_editor_command_registry_get_shortcut(reg, "test.missing", shortcut,
		sizeof(shortcut)) == 0, "unknown shortcut is empty");

	vgui_editor_command_registry_set_enabled(reg, "test.fire", always_false, NULL);
	check(vgui_editor_command_registry_is_enabled(reg, "test.fire") == 0,
		"disabled by predicate");
	check(vgui_editor_command_registry_invoke(reg, "test.fire") == 0,
		"disabled command not invoked");
	/* A registered but disabled shortcut still consumes the key. */
	check(vgui_editor_command_registry_handle_key(reg, 'Z', 1,
		VGUI_MOD_CTRL | VGUI_MOD_SHIFT), "disabled shortcut consumes key");
	check(vgui_editor_command_registry_handle_key(reg, 'Z', 0,
		VGUI_MOD_CTRL | VGUI_MOD_SHIFT) == 0, "key-up not consumed");
	check(vgui_editor_command_registry_matches_shortcut(reg, 'Z',
		VGUI_MOD_CTRL | VGUI_MOD_SHIFT), "matches_shortcut");

	vgui_editor_command_registry_set_enabled(reg, "test.fire", always_true, NULL);
	vgui_editor_command_registry_set_checked(reg, "test.fire", always_true, NULL);
	check(vgui_editor_command_registry_is_checked(reg, "test.fire"), "checked predicate");
	check(vgui_editor_command_registry_handle_key(reg, 'Z', 1,
		VGUI_MOD_CTRL | VGUI_MOD_SHIFT), "shortcut runs command");
	check(p->action_count == 2, "shortcut action ran");

	check(vgui_editor_command_registry_find(reg, "test.fire", &info), "find command");
	check(info.struct_size == sizeof(info) && strcmp(info.id, "test.fire") == 0 &&
		strcmp(info.title, "Fire") == 0 && info.enabled && info.checked,
		"command info fields");
	check(vgui_editor_command_registry_entry_at(reg, 0, &info), "entry_at");
	check(vgui_editor_command_registry_entry_at(reg, 5, &info) == 0,
		"entry_at out of range");

	/* Re-adding the same id updates the title and keeps shortcut/predicates. */
	vgui_editor_command_registry_add(reg, "test.fire", "Fire again", action_count, p,
		NULL);
	check(vgui_editor_command_registry_size(reg) == 1, "re-add keeps one entry");
	check(vgui_editor_command_registry_find(reg, "test.fire", &info) &&
		strcmp(info.title, "Fire again") == 0, "re-add updates title");
	check(strcmp(info.shortcut, "Ctrl+Shift+Z") == 0, "re-add keeps shortcut");

	check(vgui_editor_command_registry_remove(reg, "test.fire"), "remove command");
	check(vgui_editor_command_registry_has(reg, "test.fire") == 0, "removed");
	vgui_editor_command_registry_clear(reg);
	check(vgui_editor_command_registry_size(reg) == 0, "clear empties registry");
	vgui_editor_command_registry_destroy(reg);
}

/* -------------------------------------------------------------------------- */
/* Binding undo                                                                */
/* -------------------------------------------------------------------------- */

static void test_binding_undo(void)
{
	vgui_editor_context* ctx = vgui_editor_context_create(g_ui);
	vgui_editor_command_stack* stack = vgui_editor_context_commands(ctx);
	vgui_editor_binding_undo* undo = vgui_editor_binding_undo_create(g_ui, stack);

	check(undo != NULL, "binding undo create");
	check(vgui_editor_binding_undo_attached(undo), "binding undo attached");
	check(vgui_editor_binding_undo_owns_recorder(g_ui), "recorder installed");
	check(strcmp(vgui_editor_binding_undo_label(undo), "Edit") == 0, "default label");
	vgui_editor_binding_undo_set_label(undo, "Set");
	check(strcmp(vgui_editor_binding_undo_label(undo), "Set") == 0, "label updated");
	check(vgui_editor_binding_undo_merge_window(undo) == 500, "default merge window");
	vgui_editor_binding_undo_set_merge_window(undo, 120);
	check(vgui_editor_binding_undo_merge_window(undo) == 120, "merge window updated");
	check(vgui_editor_binding_undo_enabled(undo), "recording by default");
	vgui_editor_binding_undo_set_enabled(undo, 0);
	check(vgui_editor_binding_undo_enabled(undo) == 0, "recording can pause");
	check(vgui_editor_binding_undo_recorded(undo) == 0, "nothing recorded yet");

	vgui_editor_binding_undo_destroy(undo);
	check(vgui_editor_binding_undo_attached(NULL) == 0, "NULL handle is a no-op");
	check(!vgui_editor_binding_undo_owns_recorder(g_ui), "recorder restored");
	vgui_editor_context_destroy(ctx);
}

/* -------------------------------------------------------------------------- */
/* Drag & drop bridge                                                          */
/* -------------------------------------------------------------------------- */

static int drop_can_accept(void* user, const vgui_editor_drag_payload* payload)
{
	int* calls = (int*)user;
	++*calls;
	return payload->kind == VGUI_EDITOR_DRAG_ASSET;
}

static void drop_hover(void* user, const vgui_editor_drag_payload* payload,
	vgui_widget* target, float x, float y, vgui_drop_hint* hint)
{
	int* calls = (int*)user;
	++*calls;
	(void)payload;
	(void)target;
	(void)x;
	(void)y;
	if (hint)
	{
		hint->kind = VGUI_DROP_HINT_RECT;
		hint->x = 1.0f;
		hint->y = 2.0f;
		hint->w = 3.0f;
		hint->h = 4.0f;
	}
}

static int drop_drop(void* user, const vgui_editor_drag_payload* payload, float x,
	float y)
{
	int* calls = (int*)user;
	++*calls;
	(void)x;
	(void)y;
	return payload->id && strcmp(payload->id, "asset:mesh") == 0;
}

static int row_drag_provider(void* user, vgui_item_id item, int index,
	vgui_editor_drag_payload* out)
{
	int* calls = (int*)user;
	(void)item;
	++*calls;
	if (index < 0)
		return 0;
	out->kind = VGUI_EDITOR_DRAG_ASSET;
	out->id = "asset:mesh";
	return 1;
}

static void test_drag_drop(void)
{
	int calls = 0;
	vgui_editor_drag_target* target = NULL;
	vgui_widget* panel = vgui_widget_add(vgui_root(g_ui), VGUI_PANEL, NULL);
	const char* files[2];
	vgui_editor_drag_payload payload;

	vgui_editor_drag_payload_default(&payload);
	check(payload.struct_size == sizeof(payload) && payload.kind == VGUI_EDITOR_DRAG_NONE,
		"payload default initializes");

	target = vgui_editor_drag_target_create(NULL, NULL);
	check(target != NULL, "drop target with default behaviour");
	vgui_editor_drag_target_destroy(target);

	{
		vgui_editor_drop_target_desc desc;
		vgui_editor_drop_target_desc_default(&desc);
		check(desc.struct_size == sizeof(desc), "drop target desc default");
		desc.can_accept = drop_can_accept;
		desc.hover = drop_hover;
		desc.drop = drop_drop;
		target = vgui_editor_drag_target_create(&desc, &calls);
	}
	check(target != NULL, "drop target create");
	check(vgui_editor_drag_target_attach(target, panel), "drop target attach");
	check(vgui_editor_drag_target_widget(target) == panel, "attach records the widget");
	check(vgui_editor_drag_target_attach(target, NULL), "detach on request");
	check(vgui_editor_drag_target_widget(target) == NULL, "detached");

	files[0] = "a.txt";
	files[1] = "b.txt";
	payload.kind = VGUI_EDITOR_DRAG_ASSET;
	payload.id = "asset:mesh";
	payload.text = "Mesh";
	payload.files = files;
	payload.file_count = 2;
	check(vgui_editor_begin_drag(g_ui, panel, &payload), "begin drag");
	check(vgui_drag_active(g_ui), "core drag is active");
	check(vgui_editor_begin_drag(g_ui, panel, &payload) == 0, "second drag refused");
	vgui_cancel_drag(g_ui);
	check(!vgui_drag_active(g_ui), "drag cancelled");

	payload.kind = VGUI_EDITOR_DRAG_TEXT;
	check(vgui_editor_begin_drag(g_ui, panel, NULL) == 0, "NULL payload rejected");

	vgui_editor_drag_target_destroy(target);

	vgui_editor_widget_set_row_drag(panel, 0, row_drag_provider, &calls);
	vgui_editor_widget_set_row_drag(panel, 0, NULL, NULL);
	vgui_widget_destroy(panel);
	frame(1);
}

/* -------------------------------------------------------------------------- */
/* Layout                                                                      */
/* -------------------------------------------------------------------------- */

static void test_layout(void)
{
	vgui_editor_layout* layout = vgui_editor_layout_create();
	const char* json;
	float body = 0.0f, workspace = 0.0f, center = 0.0f;
	char path[256];
	FILE* tmp;

	check(layout != NULL, "layout create");
	check(vgui_editor_layout_get_version(layout) == 7, "layout version");
	check(vgui_editor_layout_panel_visible(layout, VGUI_EDITOR_PANEL_OUTLINER),
		"panels visible by default");
	vgui_editor_layout_set_panel_visible(layout, VGUI_EDITOR_PANEL_BOTTOM, 0);
	check(vgui_editor_layout_panel_visible(layout, VGUI_EDITOR_PANEL_BOTTOM) == 0,
		"visibility setter applies");
	vgui_editor_layout_set_ratios(layout, 0.5f, 0.25f, 0.6f);
	vgui_editor_layout_get_ratios(layout, &body, &workspace, &center);
	check(body == 0.5f && workspace == 0.25f && center == 0.6f, "ratio round-trip");

	vgui_editor_layout_set_view_state(layout, "table", "{\"a\":1}");
	check(strcmp(vgui_editor_layout_view_state(layout, "table"), "{\"a\":1}") == 0,
		"view state round-trip");
	check(vgui_editor_layout_view_state(layout, "unknown")[0] == '\0',
		"unknown view state is empty");

	json = vgui_editor_layout_to_json(layout);
	check(json && strstr(json, "\"version\"") != NULL, "layout serializes to json");
	check(strstr(json, "0.5") != NULL || strstr(json, "0.50") != NULL ||
		strstr(json, "0.500") != NULL, "json carries the ratio");

	{
		vgui_editor_layout* other = vgui_editor_layout_create();
		check(vgui_editor_layout_from_json(other, json), "json parses back");
		check(vgui_editor_layout_panel_visible(other, VGUI_EDITOR_PANEL_BOTTOM) == 0,
			"restored visibility");
		vgui_editor_layout_get_ratios(other, &body, &workspace, &center);
		check(body == 0.5f && workspace == 0.25f && center == 0.6f, "restored ratios");
		vgui_editor_layout_destroy(other);
	}

	check(vgui_editor_layout_from_json(layout, "{not json") == 0,
		"invalid json rejected");
	check(strlen(vgui_editor_last_error()) > 0, "last error reports the failure");

	tmp = fopen("vgui_editor_layout.json", "wb");
	if (tmp)
	{
		fclose(tmp);
		strcpy(path, "vgui_editor_layout.json");
		check(vgui_editor_layout_save_file(layout, path), "save layout file");
		{
			vgui_editor_layout* loaded = vgui_editor_layout_create();
			check(vgui_editor_layout_load_file(loaded, path), "load layout file");
			vgui_editor_layout_destroy(loaded);
		}
		check(vgui_editor_layout_save_file(layout, "") == 0, "empty path rejected");
		check(vgui_editor_layout_load_file(layout, "no_such_dir/x.json") == 0,
			"missing file reports failure");
		remove(path);
	}

	vgui_editor_layout_destroy(layout);
}

/* -------------------------------------------------------------------------- */
/* Spinbox                                                                     */
/* -------------------------------------------------------------------------- */

static void on_value(void* user, double value)
{
	probe* p = (probe*)user;
	++p->change_count;
	p->last_value = value;
}

static void on_value_commit(void* user, double value)
{
	probe* p = (probe*)user;
	++p->commit_count;
	p->last_value = value;
}

static void test_spinbox(probe* p)
{
	memset(p, 0, sizeof(*p));
	vgui_editor_spinbox* spin = vgui_editor_spinbox_create(vgui_root(g_ui), "Radius");

	check(spin != NULL, "spinbox create");
	check(vgui_editor_spinbox_widget(spin) != NULL, "spinbox widget");
	check(vgui_editor_spinbox_edit_widget(spin) != NULL, "spinbox edit widget");
	check(vgui_editor_spinbox_label_widget(spin) != NULL, "spinbox label widget");
	check(strcmp(vgui_editor_spinbox_label(spin), "Radius") == 0, "spinbox label");
	check(vgui_editor_spinbox_increment_widget(spin) != NULL, "increment button");
	check(vgui_editor_spinbox_decrement_widget(spin) != NULL, "decrement button");

	vgui_editor_spinbox_set_id(spin, "radius_spin");
	check(strcmp(vgui_editor_spinbox_id(spin), "radius_spin") == 0, "spinbox id");
	vgui_editor_spinbox_set_label(spin, "Radius");
	vgui_editor_spinbox_set_label_width(spin, 60.0f);
	check(vgui_editor_spinbox_label_width(spin) == 60.0f, "label width");
	vgui_editor_spinbox_set_auto_width(spin, 1);
	check(vgui_editor_spinbox_auto_width(spin), "auto width");
	vgui_editor_spinbox_set_buttons_visible(spin, 0);
	check(vgui_editor_spinbox_buttons_visible(spin) == 0, "buttons hidden");
	vgui_editor_spinbox_set_buttons_visible(spin, 1);

	vgui_editor_spinbox_set_range(spin, 0.0, 10.0);
	check(vgui_editor_spinbox_has_range(spin), "range set");
	check(vgui_editor_spinbox_min_value(spin) == 0.0, "min value");
	check(vgui_editor_spinbox_max_value(spin) == 10.0, "max value");
	vgui_editor_spinbox_set_step(spin, 0.5);
	check(vgui_editor_spinbox_step(spin) == 0.5, "step");

	/* Silent programmatic update. */
	vgui_editor_spinbox_set_on_change(spin, on_value, p);
	vgui_editor_spinbox_set_on_commit(spin, on_value_commit, p);
	vgui_editor_spinbox_set_value(spin, 3.0);
	check(vgui_editor_spinbox_value(spin) == 3.0, "set_value applies");
	check(p->change_count == 0, "set_value stays silent");
	check(strlen(vgui_editor_spinbox_value_text(spin)) > 0, "value text");

	check(vgui_editor_spinbox_commit_value(spin, 4.0), "commit_value succeeds");
	check(p->change_count == 1 && p->commit_count == 1, "commit notifies once");
	check(vgui_editor_spinbox_value(spin) == 4.0, "committed value");
	check(vgui_editor_spinbox_commit_value(spin, 50.0) == 0 ||
		vgui_editor_spinbox_value(spin) == 10.0, "commit clamps to range");
	vgui_editor_spinbox_set_value(spin, 4.0);
	p->change_count = p->commit_count = 0;

	vgui_editor_spinbox_step_by(spin, 2);
	check(vgui_editor_spinbox_value(spin) == 5.0, "step_by snaps by step");
	check(p->change_count == 1 && p->commit_count == 1, "step_by notifies");

	check(vgui_editor_spinbox_commit_text(spin, "7.5"), "commit_text parses");
	check(vgui_editor_spinbox_value(spin) == 7.5, "commit_text applies");
	check(vgui_editor_spinbox_commit_text(spin, "abc") == 0, "commit_text rejects");
	check(strlen(vgui_editor_spinbox_error(spin)) > 0, "invalid text reports error");
	vgui_editor_spinbox_set_error(spin, "");
	check(vgui_editor_spinbox_error(spin)[0] == '\0', "error clears");

	vgui_editor_spinbox_set_value_type(spin, VGUI_EDITOR_VALUE_INTEGER);
	check(vgui_editor_spinbox_value_type(spin) == VGUI_EDITOR_VALUE_INTEGER,
		"integer type");
	vgui_editor_spinbox_set_value(spin, 6.7);
	check(vgui_editor_spinbox_value(spin) == 7.0, "integer rounds");
	vgui_editor_spinbox_clear_range(spin);
	check(vgui_editor_spinbox_has_range(spin) == 0, "range cleared");

	vgui_editor_spinbox_set_read_only(spin, 1);
	check(vgui_editor_spinbox_read_only(spin), "read only");
	check(vgui_editor_spinbox_commit_value(spin, 2.0) == 0,
		"read only rejects user edits");
	vgui_editor_spinbox_set_read_only(spin, 0);
	vgui_editor_spinbox_set_enabled(spin, 0);
	check(vgui_editor_spinbox_enabled(spin) == 0, "disabled");
	vgui_editor_spinbox_set_enabled(spin, 1);

	vgui_editor_spinbox_focus(spin);
	frame(1);
	vgui_editor_spinbox_destroy(spin);
	check(vgui_editor_spinbox_value(NULL) == 0.0, "NULL handle is a no-op");
}

/* -------------------------------------------------------------------------- */
/* Combo box                                                                   */
/* -------------------------------------------------------------------------- */

static void on_combo(void* user, int index, const char* value)
{
	probe* p = (probe*)user;
	++p->change_count;
	p->combo_index = index;
	strncpy(p->combo_value, value ? value : "", sizeof(p->combo_value) - 1);
}

static void test_combo_box(probe* p)
{
	memset(p, 0, sizeof(*p));
	vgui_editor_combo_box* combo = vgui_editor_combo_box_create(vgui_root(g_ui));
	vgui_editor_combo_item items[3];
	vgui_editor_combo_item out;
	size_t i;

	check(combo != NULL, "combo create");
	check(vgui_editor_combo_box_widget(combo) != NULL, "combo widget");
	vgui_editor_combo_box_set_id(combo, "shape_combo");
	check(strcmp(vgui_editor_combo_box_id(combo), "shape_combo") == 0, "combo id");

	for (i = 0; i < 3; ++i)
	{
		vgui_editor_combo_item_default(&items[i]);
		items[i].label = i == 1 ? "" : (i == 0 ? "Sphere" : "Cube");
		items[i].value = i == 0 ? "sphere" : (i == 1 ? "hidden" : "cube");
		items[i].enabled = i != 1;
	}
	vgui_editor_combo_box_set_items(combo, items, 3);
	check(vgui_editor_combo_box_item_count(combo) == 3, "items installed");
	check(vgui_editor_combo_box_item_at(combo, 0, &out) &&
		strcmp(out.label, "Sphere") == 0 && out.enabled, "item_at");
	check(vgui_editor_combo_box_item_at(combo, 9, &out) == 0, "item_at range");
	check(vgui_editor_combo_box_index_of_value(combo, "cube") == 2, "index_of_value");

	vgui_editor_combo_box_set_on_change(combo, on_combo, p);
	check(vgui_editor_combo_box_select_index(combo, 2), "select_index user-style");
	check(p->change_count == 1 && p->combo_index == 2 &&
		strcmp(p->combo_value, "cube") == 0, "selection notifies");
	check(vgui_editor_combo_box_select_index(combo, 1) == 0,
		"disabled item rejected");
	check(vgui_editor_combo_box_select_index(combo, 2), "reselect current succeeds");
	check(p->change_count == 1, "reselect does not notify");

	vgui_editor_combo_box_set_selected_index(combo, 1);
	check(vgui_editor_combo_box_selected_index(combo) == 1, "silent select allows disabled");
	check(strcmp(vgui_editor_combo_box_value(combo), "hidden") == 0, "combo value");
	vgui_editor_combo_box_set_value(combo, "sphere");
	check(vgui_editor_combo_box_selected_index(combo) == 0, "set_value by value");
	check(vgui_editor_combo_box_set_value(combo, "none") == 0, "unknown value fails");
	check(vgui_editor_combo_box_set_item_enabled(combo, 1, 1), "enable item");
	check(vgui_editor_combo_box_select_index(combo, 1), "now selectable");

	vgui_editor_combo_box_set_placeholder(combo, "pick");
	check(strcmp(vgui_editor_combo_box_placeholder(combo), "pick") == 0, "placeholder");
	vgui_editor_combo_box_clear_selection(combo);
	check(vgui_editor_combo_box_selected_index(combo) == -1, "clear selection");
	check(strcmp(vgui_editor_combo_box_display_text(combo), "pick") == 0,
		"display falls back to placeholder");

	check(vgui_editor_combo_box_open(combo), "open popup");
	check(vgui_editor_combo_box_is_open(combo), "popup is open");
	vgui_editor_combo_box_toggle(combo);
	check(vgui_editor_combo_box_is_open(combo) == 0, "toggle closes");
	vgui_editor_combo_box_close(combo);

	vgui_editor_combo_box_set_enabled(combo, 0);
	check(vgui_editor_combo_box_enabled(combo) == 0, "combo disabled");
	vgui_editor_combo_box_set_enabled(combo, 1);
	vgui_editor_combo_box_set_read_only(combo, 1);
	check(vgui_editor_combo_box_read_only(combo), "combo read only");
	vgui_editor_combo_box_set_read_only(combo, 0);
	vgui_editor_combo_box_focus(combo);
	frame(1);
	vgui_editor_combo_box_destroy(combo);
}

/* -------------------------------------------------------------------------- */
/* Search box and breadcrumb                                                   */
/* -------------------------------------------------------------------------- */

static void on_text(void* user, const char* text)
{
	probe* p = (probe*)user;
	++p->change_count;
	strncpy(p->combo_value, text ? text : "", sizeof(p->combo_value) - 1);
}

static void test_search_box(probe* p)
{
	memset(p, 0, sizeof(*p));
	vgui_editor_search_box* search = vgui_editor_search_box_create(vgui_root(g_ui),
		"Filter");
	check(search != NULL, "search box create");
	check(vgui_editor_search_box_widget(search) != NULL, "search widget");
	check(vgui_editor_search_box_edit_widget(search) != NULL, "search edit widget");
	check(vgui_editor_search_box_clear_widget(search) != NULL, "search clear widget");
	check(strcmp(vgui_editor_search_box_text(search), "") == 0, "text starts empty");

	vgui_editor_search_box_set_on_change(search, on_text, p);
	vgui_editor_search_box_set_on_submit(search, on_text, p);
	vgui_editor_search_box_set_text(search, "mesh");
	check(strcmp(vgui_editor_search_box_text(search), "mesh") == 0, "set_text applies");
	check(p->change_count == 0, "set_text stays silent");
	vgui_editor_search_box_submit(search);
	check(p->change_count == 1 && strcmp(p->combo_value, "mesh") == 0, "submit notifies");
	vgui_editor_search_box_clear(search);
	check(vgui_editor_search_box_text(search)[0] == '\0', "clear empties text");
	check(p->change_count == 2, "clear notifies");
	vgui_editor_search_box_set_placeholder(search, "Search");
	vgui_editor_search_box_focus(search);
	frame(1);
	vgui_editor_search_box_destroy(search);
}

static void on_navigate(void* user, size_t index, const char* id)
{
	probe* p = (probe*)user;
	++p->navigate_count;
	p->navigate_index = index;
	strncpy(p->navigate_id, id ? id : "", sizeof(p->navigate_id) - 1);
}

static void test_breadcrumb(probe* p)
{
	memset(p, 0, sizeof(*p));
	vgui_editor_breadcrumb* path = vgui_editor_breadcrumb_create(vgui_root(g_ui));
	vgui_editor_breadcrumb_item items[2];
	vgui_editor_breadcrumb_item out;
	size_t i;

	check(path != NULL, "breadcrumb create");
	check(vgui_editor_breadcrumb_widget(path) != NULL, "breadcrumb widget");
	vgui_editor_breadcrumb_add_item(path, "Scene", "scene");
	vgui_editor_breadcrumb_add_item(path, "Meshes", "meshes");
	check(vgui_editor_breadcrumb_count(path) == 2, "items added");
	check(vgui_editor_breadcrumb_item_at(path, 1, &out) &&
		strcmp(out.label, "Meshes") == 0 && strcmp(out.id, "meshes") == 0,
		"item_at fields");
	check(vgui_editor_breadcrumb_item_at(path, 7, &out) == 0, "item_at range");
	check(vgui_editor_breadcrumb_item_widget(path, 0) != NULL, "item widget");

	vgui_editor_breadcrumb_set_on_navigate(path, on_navigate, p);
	check(vgui_editor_breadcrumb_navigate(path, 0), "navigate succeeds");
	check(p->navigate_count == 1 && p->navigate_index == 0 &&
		strcmp(p->navigate_id, "scene") == 0, "navigate notifies");
	check(vgui_editor_breadcrumb_navigate(path, 9) == 0, "navigate range rejected");
	vgui_editor_breadcrumb_set_active(path, 1);
	check(vgui_editor_breadcrumb_active(path) == 1, "set_active silent");
	check(p->navigate_count == 1, "set_active does not notify");

	for (i = 0; i < 2; ++i)
	{
		vgui_editor_breadcrumb_item_default(&items[i]);
		items[i].label = i == 0 ? "Root" : "Leaf";
		items[i].id = i == 0 ? "root" : NULL;
	}
	vgui_editor_breadcrumb_set_items(path, items, 2);
	check(vgui_editor_breadcrumb_count(path) == 2, "set_items replaces");
	vgui_editor_breadcrumb_clear(path);
	check(vgui_editor_breadcrumb_count(path) == 0, "clear empties path");
	frame(1);
	vgui_editor_breadcrumb_destroy(path);
}

/* -------------------------------------------------------------------------- */
/* Toolbar and status bar                                                      */
/* -------------------------------------------------------------------------- */

static void on_click(void* user)
{
	probe* p = (probe*)user;
	++p->click_count;
}

static void on_toggle(void* user, int checked)
{
	probe* p = (probe*)user;
	++p->toggle_count;
	p->last_value = checked ? 1.0 : 0.0;
}

static void on_segment(void* user, size_t index)
{
	probe* p = (probe*)user;
	++p->navigate_count;
	p->navigate_index = index;
}

static void test_toolbar(probe* p)
{
	memset(p, 0, sizeof(*p));
	vgui_editor_command_registry* reg = vgui_editor_command_registry_create();
	vgui_editor_toolbar* bar = vgui_editor_toolbar_create(vgui_root(g_ui));
	vgui_editor_icon_button* icon;
	vgui_editor_toggle_button* toggle;
	vgui_editor_segmented_control* seg;

	vgui_editor_command_registry_add(reg, "tool.run", "Run", action_count, p, NULL);

	check(bar != NULL, "toolbar create");
	check(vgui_editor_toolbar_widget(bar) != NULL, "toolbar widget");
	icon = vgui_editor_toolbar_add_icon_button(bar, "Go", NULL, 0, 0);
	check(icon != NULL, "toolbar icon button");
	vgui_editor_icon_button_set_tooltip(icon, "go tooltip");
	vgui_editor_icon_button_set_on_click(icon, on_click, p);
	vgui_editor_icon_button_click(icon);
	check(p->click_count == 1, "icon button click notifies");
	vgui_editor_icon_button_set_checked(icon, 1);
	check(vgui_editor_icon_button_checked(icon), "icon button checked");
	vgui_editor_icon_button_set_enabled(icon, 0);
	check(vgui_editor_icon_button_enabled(icon) == 0, "icon button disabled");
	/* Toolbar-owned wrappers ignore destroy(). */
	vgui_editor_icon_button_destroy(icon);
	check(vgui_editor_icon_button_widget(icon) != NULL, "owned wrapper survives");

	toggle = vgui_editor_toolbar_add_toggle_button(bar, "Snap", 0);
	check(toggle != NULL, "toolbar toggle button");
	vgui_editor_toggle_button_set_on_change(toggle, on_toggle, p);
	vgui_editor_toggle_button_toggle(toggle);
	check(vgui_editor_toggle_button_checked(toggle), "toggle flips");
	check(p->toggle_count == 1 && p->last_value == 1.0, "toggle notifies");

	seg = vgui_editor_toolbar_add_segmented_control(bar);
	check(seg != NULL, "toolbar segmented control");
	check(vgui_editor_segmented_control_add_segment(seg, "Move") == 0, "segment 0");
	check(vgui_editor_segmented_control_add_segment(seg, "Rotate") == 1, "segment 1");
	check(vgui_editor_segmented_control_count(seg) == 2, "segment count");
	vgui_editor_segmented_control_set_on_change(seg, on_segment, p);
	check(vgui_editor_segmented_control_select(seg, 1), "segment select");
	check(p->navigate_count == 1 && p->navigate_index == 1, "segment notifies");
	check(vgui_editor_segmented_control_has_selection(seg), "segment has selection");
	vgui_editor_segmented_control_set_segment_enabled(seg, 0, 0);
	check(vgui_editor_segmented_control_segment_enabled(seg, 0) == 0, "segment disabled");
	check(vgui_editor_segmented_control_select(seg, 0) == 0, "disabled segment rejected");
	check(vgui_editor_segmented_control_segment_widget(seg, 1) != NULL, "segment widget");

	{
		vgui_editor_icon_button* cmd = vgui_editor_toolbar_add_command_button(bar, reg,
			"tool.run", NULL, 0, 0);
		check(cmd != NULL, "command button created");
		vgui_editor_icon_button_click(cmd);
		check(p->action_count == 1, "command button invokes registry");
		check(vgui_editor_toolbar_add_command_button(bar, reg, "missing", NULL, 0, 0) ==
			NULL, "unknown command id yields NULL");
		vgui_editor_command_registry_set_enabled(reg, "tool.run", always_false, NULL);
		vgui_editor_toolbar_refresh_commands(bar);
		check(vgui_editor_icon_button_enabled(cmd) == 0, "refresh follows predicates");
		vgui_editor_command_registry_set_enabled(reg, "tool.run", always_true, NULL);
		vgui_editor_toolbar_refresh_commands(bar);
		check(vgui_editor_icon_button_enabled(cmd), "refresh re-enables");
	}

	check(vgui_editor_toolbar_add_label(bar, "label", 1) != NULL, "toolbar label");
	check(vgui_editor_toolbar_add_spacer(bar, 1.0f) != NULL, "toolbar spacer");
	check(vgui_editor_toolbar_add_separator(bar) != NULL, "toolbar separator");
	frame(1);

	/* Standalone buttons own their C++ object and widget. */
	{
		vgui_editor_icon_button* solo = vgui_editor_icon_button_create(vgui_root(g_ui),
			"Solo");
		check(solo != NULL, "standalone icon button");
		vgui_editor_icon_button_set_text(solo, "Solo again");
		vgui_editor_icon_button_set_on_click(solo, on_click, p);
		vgui_editor_icon_button_click(solo);
		check(p->click_count == 2, "standalone click notifies");
		vgui_editor_icon_button_destroy(solo);
		frame(1);
		{
			vgui_editor_segmented_control* solo_seg =
				vgui_editor_segmented_control_create(vgui_root(g_ui));
			check(solo_seg != NULL, "standalone segmented control");
			vgui_editor_segmented_control_add_segment(solo_seg, "A");
			vgui_editor_segmented_control_clear(solo_seg);
			check(vgui_editor_segmented_control_count(solo_seg) == 0, "clear segments");
			vgui_editor_segmented_control_destroy(solo_seg);
			frame(1);
		}
	}

	vgui_editor_toolbar_destroy(bar);
	frame(1);
	vgui_editor_command_registry_destroy(reg);
}

static void test_status_bar(void)
{
	vgui_editor_status_bar* status = vgui_editor_status_bar_create(vgui_root(g_ui));
	char text[64];

	check(status != NULL, "status bar create");
	check(vgui_editor_status_bar_widget(status) != NULL, "status widget");
	check(vgui_editor_status_bar_message_widget(status) != NULL, "message field");
	check(vgui_editor_status_bar_selection_widget(status) != NULL, "selection field");
	check(vgui_editor_status_bar_coordinate_widget(status) != NULL, "coordinate field");
	check(vgui_editor_status_bar_error_widget(status) != NULL, "error field");

	check(strcmp(vgui_editor_status_bar_message(status), "ready") == 0,
		"default message");
	vgui_editor_status_bar_set_message(status, "hello");
	check(strcmp(vgui_editor_status_bar_message(status), "hello") == 0, "message set");

	vgui_editor_status_bar_set_selected_object(status, 17);
	strncpy(text, vgui_editor_status_bar_selection_text(status), sizeof(text) - 1);
	text[sizeof(text) - 1] = '\0';
	check(strstr(text, "17") != NULL, "selected object formats id");
	vgui_editor_status_bar_set_selected_object(status, VGUI_EDITOR_INVALID_OBJECT);
	check(vgui_editor_status_bar_selection_text(status)[0] == '\0',
		"invalid id clears the field");
	vgui_editor_status_bar_set_selection_text(status, "custom");
	check(strcmp(vgui_editor_status_bar_selection_text(status), "custom") == 0,
		"selection text override");

	vgui_editor_status_bar_set_coordinates(status, 1.25f, -2.0f);
	check(strstr(vgui_editor_status_bar_coordinate_text(status), "1.25") != NULL,
		"coordinates format x");
	vgui_editor_status_bar_set_coordinate_text(status, "off");
	check(strcmp(vgui_editor_status_bar_coordinate_text(status), "off") == 0,
		"coordinate text override");

	check(vgui_editor_status_bar_error(status)[0] == '\0', "no error initially");
	vgui_editor_status_bar_set_error(status, "boom");
	check(strcmp(vgui_editor_status_bar_error(status), "boom") == 0, "error set");
	vgui_editor_status_bar_clear_error(status);
	check(vgui_editor_status_bar_error(status)[0] == '\0', "error cleared");
	frame(1);
	vgui_editor_status_bar_destroy(status);
}

/* -------------------------------------------------------------------------- */
/* Value editors: color picker, vector, curve, gradient                        */
/* -------------------------------------------------------------------------- */

static void color_change(void* user, const char* hex)
{
	probe* p = (probe*)user;
	++p->change_count;
	strncpy(p->combo_value, hex ? hex : "", sizeof(p->combo_value) - 1);
}

static size_t double_hex(const char* hex, char* out, size_t cap, void* user)
{
	unsigned int rgb = 0;
	char text[16];
	(void)user;
	if (!hex || sscanf(hex, "#%06x", &rgb) != 1)
		return VGUI_EDITOR_NO_VALUE;
	/* Darken by half so the edit domain differs from the stored value. */
	rgb = ((rgb & 0xFEFEFEu) >> 1);
	snprintf(text, sizeof(text), "#%06X", rgb);
	return vgui_editor_copy_string(out, cap, text);
}

static void test_color_picker(probe* p)
{
	vgui_editor_color_picker* picker = vgui_editor_color_picker_create(g_ui);
	const char* presets[2];

	memset(p, 0, sizeof(*p));
	check(picker != NULL, "color picker popup built");
	vgui_editor_color_picker_set_id(picker, "c.pick");
	vgui_editor_color_picker_set_value(picker, "#ff0000");
	check(strcmp(vgui_editor_color_picker_value(picker), "#ff0000") == 0,
		"picker keeps the host text verbatim");
	vgui_editor_color_picker_set_on_change(picker, color_change, p);
	vgui_editor_color_picker_set_value(picker, "#00FF00FF");
	check(p->change_count == 0, "programmatic set is silent");
	check(strcmp(vgui_editor_color_picker_value(picker), "#00FF00FF") == 0,
		"set_value keeps an eight digit colour verbatim");
	check(vgui_editor_color_picker_valid(picker), "picker colour parses");
	check(vgui_editor_color_picker_hue_degrees(picker) > 119.0f &&
		vgui_editor_color_picker_hue_degrees(picker) < 121.0f,
		"green hue is 120 degrees");

	presets[0] = "#FF0000";
	presets[1] = "#0000FF";
	vgui_editor_color_picker_set_presets(picker, presets, 2);
	vgui_editor_color_picker_set_recent(picker, presets, 1);
	vgui_editor_color_picker_set_current(picker, "#102030");
	frame(1);

	/* An edit-domain transform keeps the stored value in the value domain. */
	vgui_editor_color_picker_set_color_transform(picker, double_hex, p,
		double_hex, p);
	vgui_editor_color_picker_set_value(picker, "#808080");
	check(strcmp(vgui_editor_color_picker_value(picker), "#808080") == 0,
		"transform keeps the value domain");
	vgui_editor_color_picker_set_color_transform(picker, NULL, NULL, NULL, NULL);
	check(vgui_editor_color_picker_valid(picker), "identity transform valid");

	check(vgui_editor_color_picker_widget(picker) != NULL, "picker widget");
	vgui_editor_color_picker_set_on_change(picker, NULL, NULL);
	vgui_editor_color_picker_set_on_press(picker, NULL, NULL);
	vgui_editor_color_picker_set_on_commit(picker, NULL, NULL);
	vgui_editor_color_picker_destroy(picker);
	vgui_editor_color_picker_set_value(NULL, "#FFFFFF");
}

static void vector_change(void* user, const double* values, int dimension)
{
	probe* p = (probe*)user;
	++p->change_count;
	p->last_value = dimension > 0 ? values[0] : 0.0;
}

static void vector_commit(void* user, const double* values, int dimension)
{
	probe* p = (probe*)user;
	++p->commit_count;
	p->navigate_index = dimension;
}

static int vector_source(void* user, double* values, int dimension)
{
	probe* p = (probe*)user;
	int i;
	++p->action_count;
	for (i = 0; i < dimension; ++i)
		values[i] = 0.25 * (double)(i + 1);
	return 1;
}

static void test_vector_editor(probe* p)
{
	vgui_editor_vector_editor* editor;
	double values[4] = { 0.0, 0.0, 0.0, 0.0 };
	char text[64];
	double parsed[4];

	memset(p, 0, sizeof(*p));
	editor = vgui_editor_vector_editor_create(vgui_root(g_ui), 3);
	check(editor != NULL, "vector editor built");
	check(vgui_editor_vector_editor_dimension(editor) == 3, "dimension 3");
	vgui_editor_vector_editor_set_id(editor, "c.vector");
	check(strcmp(vgui_editor_vector_editor_id(editor), "c.vector") == 0,
		"vector id applied");
	check(vgui_editor_vector_editor_set_dimension(editor, 2) == 0,
		"dimension is fixed after build");

	vgui_editor_vector_editor_set_semantic(editor, VGUI_EDITOR_VECTOR_POSITION);
	check(strcmp(vgui_editor_vector_editor_axis_label(editor, 0), "X") == 0,
		"position axis label X");
	check(vgui_editor_vector_editor_axis_color(editor, 0) ==
		vgui_editor_vector_default_axis_color(0, VGUI_EDITOR_VECTOR_POSITION),
		"position axis colour matches the default");
	check(strcmp(vgui_editor_vector_default_axis_label(2,
		VGUI_EDITOR_VECTOR_COLOR), "B") == 0, "color axis label B");

	vgui_editor_vector_editor_set_range(editor, -10.0, 10.0);
	check(vgui_editor_vector_editor_has_range(editor), "vector range set");
	check(vgui_editor_vector_editor_max_value(editor) == 10.0, "vector max");
	vgui_editor_vector_editor_set_step(editor, 0.5);
	check(vgui_editor_vector_editor_step(editor) == 0.5, "vector step");

	values[0] = 1.5;
	values[1] = 2.5;
	values[2] = 3.5;
	vgui_editor_vector_editor_set_on_change(editor, vector_change, p);
	vgui_editor_vector_editor_set_on_commit(editor, vector_commit, p);
	vgui_editor_vector_editor_set_value(editor, values);
	vgui_editor_vector_editor_value(editor, values);
	memset(p, 0, sizeof(*p));
	check(values[1] == 2.5 && vgui_editor_vector_editor_component_error(editor,
		1)[0] == '\0', "silent set stores every component");
	check(p->change_count == 0 && p->commit_count == 0, "silent set notifies none");

	check(vgui_editor_vector_editor_commit_component_value(editor, 2, 20.0),
		"commit clamps into range");
	vgui_editor_vector_editor_value(editor, values);
	check(values[2] == 10.0, "component clamped to max");
	check(p->commit_count == 1 && p->change_count == 1 && p->navigate_index == 3,
		"a field commit notifies change and commit");

	check(vgui_editor_vector_editor_commit_component_text(editor, 0, "4"),
		"commit by text");
	vgui_editor_vector_editor_value(editor, values);
	check(values[0] == 4.0, "text commit applied");
	check(vgui_editor_vector_editor_commit_component_text(editor, 0, "abc") == 0,
		"invalid text rejected");

	check(vgui_editor_vector_editor_value_text(editor) != NULL,
		"value_text available");
	check(strcmp(vgui_editor_vector_editor_value_text(editor),
		"4, 2.5, 10") == 0, "value_text formats the vector");

	vgui_editor_vector_editor_set_component_error(editor, 1, "bad");
	check(strcmp(vgui_editor_vector_editor_component_error(editor, 1), "bad") == 0,
		"component error stored");
	vgui_editor_vector_editor_clear_component_errors(editor);
	check(vgui_editor_vector_editor_component_error(editor, 1)[0] == '\0',
		"errors cleared");

	vgui_editor_vector_editor_set_read_only(editor, 1);
	check(vgui_editor_vector_editor_commit_component_value(editor, 0, 1.0) == 0,
		"read-only rejects edits");
	vgui_editor_vector_editor_set_read_only(editor, 0);
	vgui_editor_vector_editor_set_enabled(editor, 0);
	check(vgui_editor_vector_editor_enabled(editor) == 0, "disabled reported");
	vgui_editor_vector_editor_set_enabled(editor, 1);

	vgui_editor_vector_editor_set_value_source(editor, vector_source, p);
	memset(p, 0, sizeof(*p));
	vgui_editor_vector_editor_refresh(editor);
	check(p->action_count == 1, "value source read on refresh");
	vgui_editor_vector_editor_value(editor, values);
	check(values[2] == 0.75, "refresh applied the source");
	check(p->change_count == 0, "refresh stays silent");

	check(vgui_editor_vector_parse("1, 2, 3", 3, parsed), "vector parse");
	check(parsed[1] == 2.0, "vector parse components");
	check(vgui_editor_vector_parse("(1; 2)", 3, parsed) == 0,
		"vector parse rejects a short vector");
	check(vgui_editor_last_error()[0] != '\0', "parse failure sets last error");
	check(vgui_editor_vector_parse("[1, 2, 3]", 4, parsed) == 0,
		"vector parse rejects a dimension mismatch");
	{
		size_t needed;
		parsed[0] = 1.0;
		parsed[1] = 2.0;
		parsed[2] = 3.0;
		needed = vgui_editor_vector_format(parsed, 3, text, sizeof(text));
		check(needed == 7 && strcmp(text, "1, 2, 3") == 0, "vector format");
		needed = vgui_editor_vector_format(parsed, 3, text, 4);
		check(needed == 7 && strcmp(text, "1, ") == 0, "vector format truncates");
	}
	check(vgui_editor_vector_editor_component_widget(editor, 0) != NULL,
		"component widget");
	check(vgui_editor_vector_editor_component_widget(editor, 9) == NULL,
		"out-of-range component widget is NULL");

	vgui_editor_vector_editor_destroy(editor);
	vgui_editor_vector_editor_set_value(NULL, values);
}

static void curve_change(void* user, vgui_editor_curve_interpolation mode,
	const vgui_editor_curve_key* keys, size_t count)
{
	probe* p = (probe*)user;
	++p->change_count;
	p->last_value = count > 0 ? (double)keys[0].value : -1.0;
	p->navigate_index = count;
	(void)mode;
}

static void test_curve_editor(probe* p)
{
	vgui_editor_curve_editor* editor = vgui_editor_curve_editor_create(
		vgui_root(g_ui));
	vgui_editor_curve_key keys[4];
	vgui_editor_curve_key parsed[4];
	char text[128];
	size_t count = 0;
	size_t needed;

	memset(p, 0, sizeof(*p));
	memset(keys, 0, sizeof(keys));
	check(editor != NULL, "curve editor built");
	vgui_editor_curve_editor_set_id(editor, "c.curve");
	check(strcmp(vgui_editor_curve_editor_id(editor), "c.curve") == 0,
		"curve id applied");

	vgui_editor_curve_key_default(&keys[0]);
	keys[0].t = 0.0f;
	keys[0].value = 0.0f;
	vgui_editor_curve_key_default(&keys[1]);
	keys[1].t = 1.0f;
	keys[1].value = 1.0f;
	check(vgui_editor_curve_editor_set_value(editor, VGUI_EDITOR_CURVE_LINEAR,
		keys, 2), "curve set_value");
	check(vgui_editor_curve_editor_key_count(editor) == 2, "curve key count");
	check(vgui_editor_curve_editor_interpolation(editor) ==
		VGUI_EDITOR_CURVE_LINEAR, "curve interpolation stored");
	check(vgui_editor_curve_editor_value_text(editor) != NULL, "curve value text");

	needed = vgui_editor_curve_editor_keys(editor, parsed, 4);
	check(needed == 2 && parsed[1].value == 1.0f, "curve keys read back");
	needed = vgui_editor_curve_editor_keys(editor, parsed, 1);
	check(needed == 2, "curve keys reports the full count");

	check(strcmp(vgui_editor_curve_editor_value_text(editor),
		"linear;0,0,0,0;1,1,0,0") == 0, "curve canonical text");

	check(vgui_editor_curve_editor_set_value_text(editor, "nonsense") == 0,
		"invalid curve text rejected");
	check(vgui_editor_last_error()[0] != '\0', "curve rejection sets last error");
	check(vgui_editor_curve_editor_set_value_text(editor,
		"cubic;0,0,0,0;0.5,1,0,0;1,0,0,0"), "curve text accepted");
	check(vgui_editor_curve_editor_key_count(editor) == 3, "curve text key count");
	check(vgui_editor_curve_editor_interpolation(editor) ==
		VGUI_EDITOR_CURVE_CUBIC, "curve text interpolation");

	vgui_editor_curve_editor_set_on_change(editor, curve_change, p);
	check(vgui_editor_curve_editor_add_key(editor, 0.25f, 0.5f), "curve add key");
	check(p->change_count == 1, "add key notified once");
	check(vgui_editor_curve_editor_key_count(editor) == 4, "curve key added");
	check(vgui_editor_curve_editor_selected_key(editor) == 1,
		"added key becomes the selection");
	{
		int before = p->commit_count;
		check(vgui_editor_curve_editor_remove_selected_key(editor),
			"curve remove key");
		check(p->commit_count >= before, "remove does not fake a commit");
	}
	check(vgui_editor_curve_editor_select_key(editor, 99) == 0,
		"out-of-range select rejected");
	check(vgui_editor_curve_editor_select_key(editor, 0), "select key");
	check(vgui_editor_curve_editor_selected_key(editor) == 0, "selection stored");

	check(vgui_editor_curve_editor_commit_interpolation(editor,
		VGUI_EDITOR_CURVE_STEP), "curve interpolation commit");
	check(strncmp(vgui_editor_curve_editor_value_text(editor), "step;", 5) == 0,
		"step text written");

	vgui_editor_curve_editor_set_read_only(editor, 1);
	check(vgui_editor_curve_editor_add_key(editor, 0.1f, 0.1f) == 0,
		"read-only curve rejects user edits");
	vgui_editor_curve_editor_set_read_only(editor, 0);

	check(vgui_editor_curve_parse("linear;0,0;0.5,0.25;1,1", NULL, parsed, 4,
		&count), "curve parse");
	check(count == 3 && parsed[1].value == 0.25f, "curve parse keys");
	check(vgui_editor_curve_parse("linear;0,0", NULL, parsed, 4, &count) == 0,
		"curve parse needs two keys");
	check(vgui_editor_curve_parse("linear;0,0;1,1", NULL, parsed, 1, &count) == 0,
		"curve parse respects the buffer");
	check(count == 2, "curve parse reports the needed count");
	{
		vgui_editor_curve_interpolation mode = VGUI_EDITOR_CURVE_LINEAR;
		needed = vgui_editor_curve_format(mode, parsed, 2, text, sizeof(text));
		check(needed == 27 && strcmp(text, "linear;0,0,0,0;0.5,0.25,0,0") == 0,
			"curve format");
	}
	check(strcmp(vgui_editor_curve_interpolation_name(VGUI_EDITOR_CURVE_CUBIC),
		"cubic") == 0, "interpolation name");
	check(vgui_editor_curve_interpolation_label(VGUI_EDITOR_CURVE_STEP) != NULL,
		"interpolation label");

	check(vgui_editor_curve_editor_graph_widget(editor) != NULL, "graph widget");
	check(vgui_editor_curve_editor_mode_widget(editor, VGUI_EDITOR_CURVE_CUBIC)
		!= NULL, "mode widget");
	check(vgui_editor_curve_editor_time_widget(editor) != NULL, "time widget");
	check(vgui_editor_curve_editor_in_tangent_widget(editor) != NULL,
		"in tangent widget");
	vgui_editor_curve_editor_set_on_press(editor, NULL, NULL);
	vgui_editor_curve_editor_set_on_commit(editor, NULL, NULL);
	vgui_editor_curve_editor_destroy(editor);

	/* Popup variant, as the property grid opens it. */
	editor = vgui_editor_curve_editor_create_popup(g_ui);
	check(editor != NULL, "curve popup built");
	vgui_editor_curve_editor_destroy(editor);
	vgui_editor_curve_editor_set_enabled(NULL, 1);
}

static void gradient_change(void* user, const vgui_editor_gradient_stop* stops,
	size_t count)
{
	probe* p = (probe*)user;
	++p->change_count;
	p->navigate_index = count;
	p->combo_value[0] = '\0';
	if (count > 0)
		strncpy(p->combo_value, stops[0].color, sizeof(p->combo_value) - 1);
}

static void test_gradient_editor(probe* p)
{
	vgui_editor_gradient_editor* editor = vgui_editor_gradient_editor_create(
		vgui_root(g_ui));
	vgui_editor_gradient_stop stops[4];
	vgui_editor_gradient_stop parsed[4];
	char text[128];
	size_t count = 0;
	size_t needed;

	memset(p, 0, sizeof(*p));
	check(editor != NULL, "gradient editor built");
	check(vgui_editor_gradient_preset_count() == 4, "four gradient presets");
	check(strcmp(vgui_editor_gradient_preset_name(0), "Mono") == 0,
		"first preset name");
	check(vgui_editor_gradient_preset_name(99)[0] == '\0', "unknown preset name");

	vgui_editor_gradient_stop_default(&stops[0]);
	stops[0].t = 0.0f;
	strncpy(stops[0].color, "#FF0000FF", sizeof(stops[0].color) - 1);
	vgui_editor_gradient_stop_default(&stops[1]);
	stops[1].t = 1.0f;
	strncpy(stops[1].color, "#00FF0080", sizeof(stops[1].color) - 1);
	check(vgui_editor_gradient_editor_set_value(editor, stops, 2),
		"gradient set_value");
	check(vgui_editor_gradient_editor_stop_count(editor) == 2, "stop count");
	/* The value domain is canonical hex: a redundant FF alpha drops, a real
	 * one stays. */
	check(strcmp(vgui_editor_gradient_editor_value_text(editor),
		"0,#FF0000;1,#00FF0080") == 0, "gradient canonical text");

	needed = vgui_editor_gradient_editor_stops(editor, parsed, 4);
	check(needed == 2 && strcmp(parsed[0].color, "#FF0000") == 0 &&
		strcmp(parsed[1].color, "#00FF0080") == 0, "stops read back");

	check(vgui_editor_gradient_editor_set_value_text(editor, "0,#GG0000;1,#FFF")
		== 0, "invalid gradient rejected");
	check(vgui_editor_last_error()[0] != '\0', "gradient rejection reports");

	vgui_editor_gradient_editor_set_on_change(editor, gradient_change, p);
	check(vgui_editor_gradient_editor_add_stop(editor, 0.5f), "gradient add stop");
	check(p->change_count == 1, "add stop notified once");
	check(vgui_editor_gradient_editor_stop_count(editor) == 3, "stop inserted");
	check(vgui_editor_gradient_editor_selected_stop(editor) == 1,
		"new stop selected");

	check(vgui_editor_gradient_editor_set_selected_color(editor, "#00FF00"),
		"set selected colour");
	check(vgui_editor_gradient_editor_set_selected_color(editor, "nope") == 0,
		"bad colour rejected");

	check(vgui_editor_gradient_editor_select_stop(editor, 0), "select stop");
	check(vgui_editor_gradient_editor_remove_selected_stop(editor),
		"remove stop");
	check(vgui_editor_gradient_editor_stop_count(editor) == 2, "stop removed");

	check(vgui_editor_gradient_editor_commit_preset(editor, 2), "preset commit");
	check(strstr(vgui_editor_gradient_editor_value_text(editor), "#001B3A") != NULL,
		"preset applied");
	check(vgui_editor_gradient_editor_commit_preset(editor, 2) == 0,
		"same preset is a no-op");

	vgui_editor_gradient_editor_set_read_only(editor, 1);
	check(vgui_editor_gradient_editor_add_stop(editor, 0.25f) == 0,
		"read-only gradient rejects edits");
	vgui_editor_gradient_editor_set_read_only(editor, 0);

	count = 0;
	check(vgui_editor_gradient_parse("0,#000000;1,#FFFFFF", parsed, 4, &count),
		"gradient parse");
	check(count == 2 && strcmp(parsed[1].color, "#FFFFFF") == 0,
		"gradient parse stops");
	check(vgui_editor_gradient_parse("0,#000000", parsed, 4, &count) == 0,
		"gradient parse needs two stops");
	needed = vgui_editor_gradient_format(parsed, count, text, sizeof(text));
	check(needed == 19 && strcmp(text, "0,#000000;1,#FFFFFF") == 0,
		"gradient format");

	check(vgui_editor_gradient_editor_bar_widget(editor) != NULL, "bar widget");
	check(vgui_editor_gradient_editor_time_widget(editor) != NULL,
		"gradient time widget");
	check(vgui_editor_gradient_editor_preset_widget(editor, 0) != NULL,
		"preset widget");
	check(vgui_editor_gradient_editor_picker_widget(editor) != NULL,
		"embedded picker widget");
	vgui_editor_gradient_editor_set_on_press(editor, NULL, NULL);
	vgui_editor_gradient_editor_set_on_commit(editor, NULL, NULL);
	vgui_editor_gradient_editor_destroy(editor);
	vgui_editor_gradient_editor_set_enabled(NULL, 1);
}

/* -------------------------------------------------------------------------- */
/* Property grid                                                               */
/* -------------------------------------------------------------------------- */

static size_t bind_get(void* user, char* out, size_t cap)
{
	counter* c = (counter*)user;
	++c->reads;
	return vgui_editor_copy_string(out, cap, "7");
}

static void bind_set(void* user, const char* value)
{
	counter* c = (counter*)user;
	++c->writes;
	c->value = value ? atoi(value) : 0;
}

static void grid_change(void* user, const char* id, const char* value)
{
	probe* p = (probe*)user;
	++p->change_count;
	strncpy(p->combo_value, value ? value : "", sizeof(p->combo_value) - 1);
	strncpy(p->navigate_id, id ? id : "", sizeof(p->navigate_id) - 1);
}

static size_t grid_source(void* user, const char* id, char* out, size_t cap)
{
	probe* p = (probe*)user;
	++p->action_count;
	if (strcmp(id, "count") != 0)
		return VGUI_EDITOR_NO_VALUE;
	return vgui_editor_copy_string(out, cap, "7");
}

static size_t long_source(void* user, const char* id, char* out, size_t cap)
{
	char filler[200];
	size_t i;
	(void)id;
	++((probe*)user)->selection_count;
	for (i = 0; i + 1 < sizeof(filler); ++i)
		filler[i] = (char)('a' + (int)(i % 26));
	filler[sizeof(filler) - 1] = '\0';
	return vgui_editor_copy_string(out, cap, filler);
}

static int asset_query(void* user, const char* id, const char* filter,
	vgui_editor_asset_info* out)
{
	probe* p = (probe*)user;
	++p->click_count;
	if (strcmp(id, "missing.asset") == 0)
		return 0;
	out->name = "Hero";
	out->kind = filter;
	out->thumbnail = NULL;
	out->missing = 0;
	return 1;
}

static int asset_accepts(void* user, const char* id, const char* filter)
{
	(void)id;
	++((probe*)user)->toggle_count;
	return filter && strcmp(filter, "mesh") == 0;
}

static void asset_browse(void* user, const char* id, const char* filter)
{
	probe* p = (probe*)user;
	++p->navigate_count;
	strncpy(p->navigate_id, filter ? filter : "", sizeof(p->navigate_id) - 1);
	(void)id;
}

static size_t multi_get(void* user, const char* id, vgui_editor_object_id target,
	char* out, size_t cap)
{
	probe* p = (probe*)user;
	++p->action_count;
	(void)id;
	return vgui_editor_copy_string(out, cap, target == 1 ? "10" : "20");
}

static void multi_set(void* user, const char* id, vgui_editor_object_id target,
	const char* value)
{
	probe* p = (probe*)user;
	++p->commit_count;
	p->last_value = atof(value);
	p->last_selection = target;
	(void)id;
}

static void test_property_grid(probe* p)
{
	vgui_editor_property_grid* grid;
	vgui_editor_command_stack* stack;
	vgui_editor_context* ctx;
	vgui_editor_property_desc descs[3];
	vgui_editor_property_desc binding_desc;
	vgui_editor_property_binding binding;
	vgui_editor_asset_info info;
	counter bound;
	char text[64];
	size_t changed;

	memset(&bound, 0, sizeof(bound));
	memset(p, 0, sizeof(*p));
	ctx = vgui_editor_context_create(g_ui);
	stack = vgui_editor_context_commands(ctx);
	grid = vgui_editor_property_grid_create(vgui_root(g_ui));
	check(grid != NULL, "property grid built");
	vgui_editor_property_grid_set_command_stack(grid, stack);

	memset(descs, 0, sizeof(descs));
	vgui_editor_property_desc_default(&descs[0]);
	descs[0].id = "count";
	descs[0].label = "Count";
	descs[0].type = VGUI_EDITOR_PROPERTY_INTEGER;
	descs[0].value = "1";
	descs[0].has_default = 1;
	descs[0].default_value = "1";
	descs[0].has_range = 1;
	descs[0].min_value = 0;
	descs[0].max_value = 10;
	vgui_editor_property_desc_default(&descs[1]);
	descs[1].id = "name";
	descs[1].label = "Name";
	descs[1].category = "Identity";
	vgui_editor_property_desc_default(&descs[2]);
	descs[2].id = "mode";
	descs[2].label = "Mode";
	descs[2].type = VGUI_EDITOR_PROPERTY_ENUMERATION;
	{
		const char* choices[2];
		choices[0] = "Fast";
		choices[1] = "Slow";
		descs[2].choices = choices;
		descs[2].choice_count = 2;
		descs[2].value = "Slow";
	}

	changed = vgui_editor_property_grid_set_properties(grid, descs, 3);
	check(changed == 3, "set_properties adds three rows");
	check(vgui_editor_property_grid_row_count(grid) == 3, "grid row count");
	check(vgui_editor_property_grid_has_property(grid, "mode"), "has mode row");
	check(vgui_editor_property_grid_set_properties(grid, descs, 3) == 0,
		"same schema refreshes in place");
	check(strcmp(vgui_editor_property_grid_property_value(grid, "count"),
		"1") == 0, "integer value read back");
	check(vgui_editor_property_grid_row_value(grid, 9) == NULL,
		"out-of-range row value is NULL");

	vgui_editor_property_grid_set_change_listener(grid, grid_change, p);
	check(vgui_editor_property_grid_commit_property_value(grid, "count", "5"),
		"commit integer");
	check(p->change_count == 1 && strcmp(p->combo_value, "5") == 0,
		"change listener reports the value");
	check(vgui_editor_property_grid_commit_property_value(grid, "count", "99"),
		"out-of-range commit clamps");
	check(strcmp(vgui_editor_property_grid_property_value(grid, "count"),
		"10") == 0, "clamped into the declared range");
	check(vgui_editor_property_grid_commit_property_value(grid, "count", "x") == 0,
		"non-integer commit rejected");
	check(p->change_count == 2, "a rejected commit does not notify");
	/* An unbroken chain of edits on one row merges into a single record. */
	check(vgui_editor_command_stack_undo_count(stack) == 1,
		"consecutive edits share one undo record");
	vgui_editor_command_stack_undo(stack);
	check(strcmp(vgui_editor_property_grid_property_value(grid, "count"),
		"1") == 0, "undo rewinds the whole chain");
	vgui_editor_command_stack_redo(stack);
	check(strcmp(vgui_editor_property_grid_property_value(grid, "count"),
		"10") == 0, "redo re-applies the chain");
	check(p->change_count == 4, "undo and redo notify");

	check(vgui_editor_property_grid_set_property_value(grid, "count", "7"),
		"programmatic set");
	check(p->change_count == 4 && strcmp(vgui_editor_property_grid_property_value(
		grid, "count"), "7") == 0, "programmatic set stays silent");
	check(vgui_editor_property_grid_reset_property(grid, "count"),
		"reset to default");
	check(strcmp(vgui_editor_property_grid_property_value(grid, "count"),
		"1") == 0, "reset applied the default");

	check(vgui_editor_property_grid_set_property_error(grid, "count", "bad"),
		"error set");
	check(strcmp(vgui_editor_property_grid_property_error(grid, "count"),
		"bad") == 0, "error read back");
	vgui_editor_property_grid_set_property_error(grid, "count", "");
	check(vgui_editor_property_grid_property_error(grid, "count")[0] == '\0',
		"error cleared");

	check(vgui_editor_property_grid_set_property_read_only(grid, "name", 1),
		"per-row read-only");
	check(vgui_editor_property_grid_property_read_only(grid, "name"),
		"read-only reported");
	vgui_editor_property_grid_set_read_only(grid, 1);
	check(vgui_editor_property_grid_read_only(grid), "grid read-only");
	vgui_editor_property_grid_set_read_only(grid, 0);

	vgui_editor_property_grid_set_filter(grid, "mod");
	check(vgui_editor_property_grid_visible_row_count(grid) == 1,
		"filter narrows the rows");
	check(vgui_editor_property_grid_property_visible(grid, "mode"),
		"matching row visible");
	check(vgui_editor_property_grid_property_visible(grid, "count") == 0,
		"non-matching row hidden");
	vgui_editor_property_grid_set_filter(grid, "");
	check(vgui_editor_property_grid_visible_row_count(grid) == 3,
		"cleared filter shows all rows");
	check(vgui_editor_property_grid_category_collapsed(grid, "Identity") == 0,
		"category starts expanded");
	check(vgui_editor_property_grid_set_category_collapsed(grid, "Identity", 1),
		"category collapsed");
	check(vgui_editor_property_grid_category_collapsed(grid, "Identity"),
		"collapse reported");
	vgui_editor_property_grid_set_category_collapsed(grid, "Identity", 0);

	check(vgui_editor_property_grid_materialized_row_count(grid) <= 3,
		"materialized rows bounded");
	check(vgui_editor_property_grid_editor(grid, "count") != NULL,
		"row editor widget");

	/* Favorites. */
	vgui_editor_property_grid_set_property_favorite(grid, "count", 1);
	check(vgui_editor_property_grid_property_favorite(grid, "count"),
		"favorite set");
	check(vgui_editor_property_grid_favorite_count(grid) == 1, "favorite count");
	check(strcmp(vgui_editor_property_grid_favorite_id_at(grid, 0), "count") == 0,
		"favorite id");
	vgui_editor_property_grid_set_favorites_only(grid, 1);
	check(vgui_editor_property_grid_visible_row_count(grid) == 1,
		"favorites-only view");
	vgui_editor_property_grid_set_favorites_only(grid, 0);

	/* Value source, including the long-value retry path. */
	vgui_editor_property_grid_set_value_source(grid, grid_source, p);
	vgui_editor_property_grid_set_property_value(grid, "name", "keep");
	vgui_editor_property_grid_refresh(grid);
	check(strcmp(vgui_editor_property_grid_property_value(grid, "count"),
		"7") == 0, "value source pulled on refresh");
	check(strcmp(vgui_editor_property_grid_property_value(grid, "name"),
		"keep") == 0, "rows the source skips keep their value");
	check(p->action_count >= 3, "source queried for every row");
	vgui_editor_property_grid_set_value_source(grid, long_source, p);
	memset(p, 0, sizeof(*p));
	vgui_editor_property_grid_refresh(grid);
	check(p->selection_count >= 6, "long values get an exact-size retry pass");
	vgui_editor_property_grid_set_value_source(grid, NULL, NULL);

	/* Per-row binding. */
	memset(&binding, 0, sizeof(binding));
	vgui_editor_property_binding_default(&binding);
	binding.get_value = bind_get;
	binding.set_value = bind_set;
	binding.user = &bound;
	binding.release_user = counter_release;
	memset(&binding_desc, 0, sizeof(binding_desc));
	vgui_editor_property_desc_default(&binding_desc);
	binding_desc.id = "bound";
	binding_desc.label = "Bound";
	binding_desc.type = VGUI_EDITOR_PROPERTY_INTEGER;
	binding_desc.binding = &binding;
	check(vgui_editor_property_grid_add_property(grid, &binding_desc),
		"bound row added");
	check(bound.reads >= 1, "binding getter used");
	check(vgui_editor_property_grid_commit_property_value(grid, "bound", "9"),
		"bound commit");
	check(bound.writes == 1 && bound.value == 9, "binding setter used");
	check(vgui_editor_property_grid_remove_property(grid, "bound"),
		"bound row removed");
	check(bound.releases == 1, "binding user released once");

	/* Batches group commits into one undo record. */
	{
		char start[32];
		size_t before = vgui_editor_command_stack_undo_count(stack);
		vgui_editor_copy_string(start, sizeof(start),
			vgui_editor_property_grid_property_value(grid, "count"));
		check(vgui_editor_property_grid_begin_batch(grid, "batch"), "begin batch");
		check(vgui_editor_property_grid_batch_depth(grid) == 1, "batch depth");
		vgui_editor_property_grid_commit_property_value(grid, "count", "2");
		vgui_editor_property_grid_commit_property_value(grid, "count", "3");
		check(vgui_editor_property_grid_end_batch(grid), "end batch");
		check(vgui_editor_command_stack_undo_count(stack) == before + 1,
			"batch is one undo record");
		vgui_editor_command_stack_undo(stack);
		check(strcmp(vgui_editor_property_grid_property_value(grid, "count"),
			start) == 0, "batch undo rewinds both edits");
	}

	/* Multi-target editing. */
	{
		vgui_editor_object_id targets[2];
		targets[0] = 1;
		targets[1] = 2;
		vgui_editor_property_grid_set_multi_getter(grid, multi_get, p);
		vgui_editor_property_grid_set_multi_setter(grid, multi_set, p);
		vgui_editor_property_grid_set_multi_targets(grid, targets, 2);
		check(vgui_editor_property_grid_multi_targets(grid, NULL, 0) == 2,
			"multi targets reported");
		check(vgui_editor_property_grid_property_mixed(grid, "count"),
			"differing values are mixed");
		memset(p, 0, sizeof(*p));
		check(vgui_editor_property_grid_commit_property_value(grid, "count", "8"),
			"multi commit accepted");
		check(p->commit_count == 2, "commit applied to every target");
		vgui_editor_property_grid_set_multi_targets(grid, NULL, 0);
		check(vgui_editor_property_grid_multi_targets(grid, NULL, 0) == 0,
			"multi mode cleared");
		vgui_editor_property_grid_set_multi_getter(grid, NULL, NULL);
		vgui_editor_property_grid_set_multi_setter(grid, NULL, NULL);
	}

	/* Reference rows. */
	memset(&descs[0], 0, sizeof(descs[0]));
	vgui_editor_property_desc_default(&descs[0]);
	descs[0].id = "mesh";
	descs[0].label = "Mesh";
	descs[0].type = VGUI_EDITOR_PROPERTY_ASSET;
	descs[0].value = "asset://hero";
	descs[0].asset_filter = "mesh";
	{
		vgui_editor_property_grid_set_asset_query(grid, asset_query, p);
		vgui_editor_property_grid_set_asset_filter_check(grid, asset_accepts, p);
		vgui_editor_property_grid_set_asset_browse_handler(grid, asset_browse, p);
		check(vgui_editor_property_grid_add_property(grid, &descs[0]),
			"asset row added");
		frame(1);
		check(p->click_count >= 1, "asset query ran");
		vgui_editor_property_grid_remove_property(grid, "mesh");
		descs[0].value = "missing.asset";
		vgui_editor_property_grid_add_property(grid, &descs[0]);
		frame(1);
		check(p->click_count >= 2, "missing asset queried");
	}
	vgui_editor_asset_info_default(&info);
	check(info.struct_size == sizeof(info) && info.name == NULL,
		"asset info default");
	vgui_editor_property_grid_set_asset_query(grid, NULL, NULL);
	vgui_editor_property_grid_set_asset_filter_check(grid, NULL, NULL);
	vgui_editor_property_grid_set_asset_browse_handler(grid, NULL, NULL);
	vgui_editor_property_grid_set_object_activated_handler(grid, NULL, NULL);
	vgui_editor_property_grid_set_missing_asset_handler(grid, NULL, NULL);
	vgui_editor_property_grid_set_asset_drop_handler(grid, NULL, NULL);

	/* Clipboard. */
	check(vgui_editor_property_grid_copy_properties(grid) >= 3, "copy rows");
	check(vgui_editor_property_grid_clipboard_count() >= 3, "clipboard count");
	check(vgui_editor_property_grid_paste_properties(grid) >= 3, "paste rows");

	/* Normalisation helper shared with table cells. */
	{
		vgui_editor_property_desc number;
		size_t needed = vgui_editor_property_normalize_value(&descs[1], " hi ",
			text, sizeof(text));
		check(needed == 4 && strcmp(text, " hi ") == 0,
			"text rows keep their value verbatim");
		memset(&number, 0, sizeof(number));
		vgui_editor_property_desc_default(&number);
		number.id = "n";
		number.type = VGUI_EDITOR_PROPERTY_INTEGER;
		number.has_range = 1;
		number.min_value = 0;
		number.max_value = 10;
		needed = vgui_editor_property_normalize_value(&number, " 99 ", text,
			sizeof(text));
		check(needed == 2 && strcmp(text, "10") == 0, "numbers trim and clamp");
		needed = vgui_editor_property_normalize_value(&descs[0], "", text,
			sizeof(text));
		check(needed == 0, "reference accepts the empty id");
		needed = vgui_editor_property_normalize_value(&descs[2], "Nope", text,
			sizeof(text));
		check(needed == VGUI_EDITOR_NO_VALUE, "normalize rejects unknown choice");
		check(vgui_editor_last_error()[0] != '\0', "normalize reports why");
	}

	check(vgui_editor_property_grid_widget(grid) != NULL, "grid widget");
	vgui_editor_property_grid_destroy(grid);
	vgui_editor_property_grid_set_filter(NULL, "x");
	vgui_editor_context_destroy(ctx);
}

static void grid_bind(void* user, vgui_editor_property_grid* grid,
	vgui_editor_object_id id)
{
	probe* p = (probe*)user;
	++p->selection_count;
	p->last_selection = id;
	(void)grid;
}

static void test_property_selection_binding(probe* p)
{
	vgui_editor_context* ctx = vgui_editor_context_create(g_ui);
	vgui_editor_property_grid* grid = vgui_editor_property_grid_create(
		vgui_root(g_ui));

	memset(p, 0, sizeof(*p));
	vgui_editor_property_grid_bind_selection(grid, ctx, grid_bind, p);
	check(p->selection_count == 1, "binder called for the initial selection");
	vgui_editor_context_set_selection(ctx, 77);
	check(p->selection_count == 2 && p->last_selection == 77,
		"selection change rebinds");
	vgui_editor_property_grid_unbind_selection(grid);
	vgui_editor_context_set_selection(ctx, 78);
	check(p->selection_count == 2, "unbind stops the callbacks");
	vgui_editor_property_grid_destroy(grid);
	vgui_editor_context_destroy(ctx);
}

/* -------------------------------------------------------------------------- */
/* Console                                                                     */
/* -------------------------------------------------------------------------- */

static int jump_to(void* user, const vgui_editor_log_location* where)
{
	probe* p = (probe*)user;
	++p->action_count;
	strncpy(p->combo_value, where->path ? where->path : "",
		sizeof(p->combo_value) - 1);
	p->navigate_index = (size_t)where->line;
	return 1;
}

static void test_console(probe* p)
{
	vgui_editor_console* console = vgui_editor_console_create(vgui_root(g_ui));
	vgui_editor_log_entry entry;
	vgui_editor_log_frame frames[2];
	vgui_editor_log_entry read_back;
	vgui_editor_object_id id;
	vgui_editor_object_id row;
	vgui_editor_object_id entry_id = VGUI_EDITOR_INVALID_OBJECT;
	char text[512];
	int frame_index = -1;
	size_t needed;

	memset(p, 0, sizeof(*p));
	memset(&entry, 0, sizeof(entry));
	memset(frames, 0, sizeof(frames));
	memset(&read_back, 0, sizeof(read_back));

	check(console != NULL, "console built");
	check(vgui_editor_console_idle(console), "console starts idle");
	check(strcmp(vgui_editor_log_level_name(VGUI_EDITOR_LOG_WARNING),
		"warning") == 0, "level name");

	id = vgui_editor_console_append_message(console, VGUI_EDITOR_LOG_INFO,
		"hello", "boot");
	check(id != VGUI_EDITOR_INVALID_OBJECT, "append queued");
	check(vgui_editor_console_pending_count(console) == 1, "record pending");
	check(vgui_editor_console_poll(console) == 1, "poll drained one");
	check(vgui_editor_console_idle(console), "idle after poll");
	check(vgui_editor_console_entry_count(console) == 1, "one entry retained");

	vgui_editor_log_entry_default(&entry);
	entry.level = VGUI_EDITOR_LOG_ERROR;
	entry.message = "bad thing";
	entry.source = "renderer";
	entry.timestamp = "12:34:56";
	entry.where.path = "game/render.hlsl";
	entry.where.line = 42;
	vgui_editor_log_frame_default(&frames[0]);
	frames[0].function = "DrawScene";
	frames[0].where.path = "game/render.hlsl";
	frames[0].where.line = 40;
	vgui_editor_log_frame_default(&frames[1]);
	frames[1].function = "main";
	frames[1].where.asset_id = 1234;
	entry.frames = frames;
	entry.frame_count = 2;
	id = vgui_editor_console_append(console, &entry);
	vgui_editor_console_tick(console, 1.0f / 60.0f);
	check(vgui_editor_console_entry_count(console) == 2, "tick drained the batch");
	check(vgui_editor_console_last_entry_id(console) == id, "last entry id");

	check(vgui_editor_console_entry_at(console, id, &read_back), "entry_at");
	check(read_back.level == VGUI_EDITOR_LOG_ERROR, "level round-trips");
	check(strcmp(read_back.message, "bad thing") == 0, "message round-trips");
	check(strcmp(read_back.timestamp, "12:34:56") == 0, "timestamp round-trips");
	check(strcmp(read_back.where.path, "game/render.hlsl") == 0,
		"location path round-trips");
	check(read_back.where.line == 42, "location line round-trips");
	check(read_back.frame_count == 2, "stack round-trips");
	check(strcmp(read_back.frames[0].function, "DrawScene") == 0,
		"frame function");
	check(read_back.frames[1].where.asset_id == 1234, "frame asset id");
	check(vgui_editor_console_entry_at(console, 999999, &read_back) == 0,
		"unknown entry id rejected");

	/* Stack rows only appear once the trace is expanded. */
	check(vgui_editor_console_stack_expanded(console, id) == 0,
		"stack collapsed by default");
	needed = vgui_editor_console_visible_row_count(console);
	check(vgui_editor_console_set_stack_expanded(console, id, 1),
		"expand stack");
	check(vgui_editor_console_visible_row_count(console) == needed + 2,
		"expanded rows added");
	check(vgui_editor_console_toggle_stack(console, id), "toggle stack");
	check(vgui_editor_console_stack_expanded(console, id) == 0, "collapsed again");
	vgui_editor_console_set_stack_expanded(console, id, 1);

	row = vgui_editor_console_row_id_at(console, 0);
	check(row != VGUI_EDITOR_INVALID_OBJECT, "row id at index 0");
	{
		vgui_editor_console_row_entry(console, row, &entry_id, &frame_index);
		check(entry_id != VGUI_EDITOR_INVALID_OBJECT && frame_index == -1,
			"row decodes to a log entry");
	}
	check(vgui_editor_console_row_entry(console, 0, NULL, NULL) == 0,
		"invalid row id rejected");

	/* Filters. */
	check(vgui_editor_console_visible_entry_count(console) == 2,
		"both entries visible");
	check(vgui_editor_console_filter_active(console) == 0, "no filter active");
	vgui_editor_console_set_level_enabled(console, VGUI_EDITOR_LOG_INFO, 0);
	check(vgui_editor_console_level_enabled(console, VGUI_EDITOR_LOG_INFO) == 0,
		"level disabled");
	check(vgui_editor_console_filter_active(console), "filter active");
	check(vgui_editor_console_visible_entry_count(console) == 1,
		"info row filtered out");
	vgui_editor_console_set_level_enabled(console, VGUI_EDITOR_LOG_INFO, 1);
	vgui_editor_console_set_search_text(console, "bad");
	check(strcmp(vgui_editor_console_search_text(console), "bad") == 0,
		"search text stored");
	check(vgui_editor_console_visible_entry_count(console) == 1,
		"search filters entries");
	vgui_editor_console_set_search_text(console, "");

	/* Jump handling. */
	vgui_editor_console_set_jump_handler(console, jump_to, p);
	row = vgui_editor_console_row_id_at(console, 1);
	check(vgui_editor_console_activate_row(console, row), "activate row");
	check(p->action_count == 1, "jump handler invoked");
	check(strcmp(p->combo_value, "game/render.hlsl") == 0, "jump location passed");
	check(p->navigate_index == 42, "jump line passed");
	check(vgui_editor_console_activate_selected(console) >= 0,
		"activate selected is safe");

	/* Text export and clipping. */
	needed = vgui_editor_console_visible_text(console, text, sizeof(text));
	check(needed > 0 && strstr(text, "bad thing") != NULL, "visible text");
	check(vgui_editor_console_selected_text(console, text, sizeof(text)) == 0,
		"nothing selected means no selected text");
	vgui_editor_console_set_stack_expanded(console, id, 0);
	row = vgui_editor_console_row_id_at(console, 0);
	vgui_tree_select(vgui_editor_console_tree(console), (vgui_item_id)row, 1);
	needed = vgui_editor_console_selected_text(console, text, sizeof(text));
	check(needed > 0 && strstr(text, "hello") != NULL, "selected row exported");
	check(vgui_editor_console_copy_selection(console) >= 0, "copy selection");

	/* Ring buffer behaviour. */
	vgui_editor_console_set_max_entries(console, 4);
	check(vgui_editor_console_max_entries(console) == 4, "max entries stored");
	{
		int i;
		for (i = 0; i < 10; ++i)
			vgui_editor_console_append_message(console, VGUI_EDITOR_LOG_DEBUG,
				"tick", "loop");
		vgui_editor_console_poll(console);
	}
	check(vgui_editor_console_entry_count(console) <= 4, "entry cap applied");
	check(vgui_editor_console_dropped_count(console) > 0, "drops counted");
	vgui_editor_console_set_drain_batch(console, 2);
	check(vgui_editor_console_drain_batch(console) == 2, "drain batch stored");
	{
		int i;
		for (i = 0; i < 4; ++i)
			vgui_editor_console_append_message(console, VGUI_EDITOR_LOG_TRACE,
				"slow", "loop");
		check(vgui_editor_console_poll(console) == 2, "drain batch respected");
		vgui_editor_console_poll(console);
	}

	check(vgui_editor_console_auto_scroll(console), "auto scroll default");
	vgui_editor_console_set_auto_scroll(console, 0);
	check(vgui_editor_console_auto_scroll(console) == 0, "auto scroll off");
	vgui_editor_console_scroll_to_latest(console);
	vgui_editor_console_set_level_color(console, VGUI_EDITOR_LOG_ERROR,
		0xFFFF0000);
	check(vgui_editor_console_level_color(console, VGUI_EDITOR_LOG_ERROR) ==
		0xFFFF0000, "level colour set");

	check(vgui_editor_console_tree(console) != NULL, "tree widget");
	check(vgui_editor_console_ticker(console) != NULL, "ticker widget");
	check(vgui_editor_console_search_widget(console) != NULL, "search widget");
	check(vgui_editor_console_auto_scroll_widget(console) != NULL,
		"auto scroll widget");
	check(vgui_editor_console_level_widget(console, VGUI_EDITOR_LOG_DEBUG) != NULL,
		"level widget");
	check(vgui_editor_console_copy_widget(console) != NULL, "copy widget");
	check(vgui_editor_console_clear_widget(console) != NULL, "clear widget");

	vgui_editor_console_clear(console);
	check(vgui_editor_console_entry_count(console) == 0, "cleared");
	vgui_editor_console_set_jump_handler(console, NULL, NULL);
	vgui_editor_console_destroy(console);
	vgui_editor_console_tick(NULL, 0.0f);
}

/* -------------------------------------------------------------------------- */
/* Helpers shared by the tests above                                           */
/* -------------------------------------------------------------------------- */

static void test_string_helpers(void)
{
	char buffer[8];
	size_t needed;

	needed = vgui_editor_copy_string(buffer, sizeof(buffer), "0123456789");
	check(needed == 10, "copy_string reports the full length");
	check(strcmp(buffer, "0123456") == 0, "copy_string truncates and terminates");
	check(vgui_editor_copy_string(NULL, 0, "text") == 4, "copy_string handles NULL out");
	check(vgui_editor_copy_string(buffer, sizeof(buffer), NULL) == 0,
		"NULL text copies an empty string");
	check(buffer[0] == '\0', "NULL text terminates");
}

int main(void)
{
	probe p;
	vgui_editor_context* ctx;

	memset(&p, 0, sizeof(p));

	g_ui = vgui_create(NULL, 16.0f);
	if (!g_ui)
	{
		fprintf(stderr, "FAIL: cannot create headless vgui context\n");
		return 1;
	}
	vgui_context_set_size(g_ui, 1280, 800);

	printf("== editor C API smoke test ==\n");
	test_string_helpers();

	ctx = vgui_editor_context_create(g_ui);
	check(ctx != NULL, "root context available for shell-free widgets");
	vgui_editor_context_destroy(ctx);

	test_context(&p);
	test_command_stack();
	test_registry(&p);
	test_binding_undo();
	test_drag_drop();
	test_layout();
	test_spinbox(&p);
	test_combo_box(&p);
	test_search_box(&p);
	test_breadcrumb(&p);
	test_toolbar(&p);
	test_status_bar();
	test_color_picker(&p);
	test_vector_editor(&p);
	test_curve_editor(&p);
	test_gradient_editor(&p);
	test_property_grid(&p);
	test_property_selection_binding(&p);
	test_console(&p);
	test_outliner(&p);
	test_table(&p);
	test_timeline(&p);
	test_node_graph(&p);
	test_canvas(&p);
	test_code_editor(&p);
	test_markdown(&p);
	test_thumbnails(&p);
	test_asset_browser(&p);
	test_asset_preview(&p);
	test_viewport(&p);
	test_camera_grid_picking(&p);
	test_gizmo(&p);
	test_shell(&p);

	frame(2);
	vgui_destroy(g_ui);

	printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
