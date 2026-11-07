#pragma once
/*
 * vgui public C API for the editor kit components (vgui_editor_kit).
 *
 * This header is plain C99/C11, like vgui.h: no classes, templates or std::
 * types appear here, so an editor host written in C can use the same panels a
 * C++ host uses.  The C++ headers under <vgui/editor/> stay available and are
 * still the richer surface; this facade covers the operations a host needs to
 * build tool UIs (outliner, properties, assets, console, table, timeline, node
 * graph, code, canvas, viewport overlays, shell) plus the small editor widgets.
 *
 * Conventions (identical to vgui_chart.h):
 *   - Handles are opaque pointers created by a *_create() call.  They own the
 *     C++ component, so a host never has to reason about C++ lifetimes.
 *   - Every extensible input struct starts with uint32_t struct_size.  Set it
 *     to sizeof(*desc) via the matching *_default() helper; a smaller value is
 *     accepted and read as "an older, shorter struct".
 *   - Callbacks are plain C function pointers plus a void* user pointer.  The
 *     kit calls them on the UI thread, from inside the widget handlers.
 *   - Input strings are UTF-8 and copied by the kit.  Returned const char*
 *     strings are borrowed: they stay valid until the next call that mutates
 *     the same handle (or until the handle is destroyed).
 *   - int is used for booleans (0 = false).  Non-NULL pointers are required for
 *     handles; passing NULL to a setter is a no-op rather than a crash.
 *
 * Ownership and destruction order:
 *   - A component handle must outlive its widget tree, so destroy child
 *     components before their parent widgets and every editor handle before the
 *     vgui_context.  vgui_editor_*_destroy() removes the C++ object first and
 *     then destroys the widget it created, which is the order the components'
 *     destructors expect.
 *   - Handles created by a container (toolbar buttons, segmented controls,
 *     shell panels) are owned by that container: use them, never destroy them.
 *
 * Typical usage:
 *
 *     vgui_context* ui = vgui_create("Arial", 16.0f);
 *     vgui_editor_context* ctx = vgui_editor_context_create(ui);
 *     vgui_editor_command_stack* stack = vgui_editor_context_commands(ctx);
 *
 *     vgui_editor_spinbox* radius =
 *         vgui_editor_spinbox_create(vgui_root(ui), "Radius");
 *     vgui_editor_spinbox_set_range(radius, 0.0, 100.0);
 *     vgui_editor_spinbox_set_on_commit(radius, on_radius, user);
 *
 *     // per frame: vgui_update() / vgui_draw() as usual
 *     vgui_editor_spinbox_destroy(radius);
 *     vgui_editor_context_destroy(ctx);
 *
 * Linking: the C functions compile into the vgui_editor_kit library, so a C
 * host links vgui::editor_kit (C++ is pulled in by the link language).  The kit
 * is built with the static MSVC runtime, so a consumer must use /MT too.
 */
#include <stddef.h>
#include <stdint.h>

#include <vgui/vgui.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef VGUI_API
#define VGUI_API
#endif

	/* Stable object id shared by selection, outliner, picking and assets. */
	typedef uint64_t vgui_editor_object_id;
#define VGUI_EDITOR_INVALID_OBJECT ((vgui_editor_object_id)0)

	/* ------------------------------------------------------------------------- */
	/* Shared value types                                                        */
	/* ------------------------------------------------------------------------- */

	/* Editor panels whose visibility the layout (and the shell) controls. */
	typedef enum vgui_editor_panel {
		VGUI_EDITOR_PANEL_OUTLINER = 0,
		VGUI_EDITOR_PANEL_VIEWPORT,
		VGUI_EDITOR_PANEL_PROPERTIES,
		VGUI_EDITOR_PANEL_BOTTOM
	} vgui_editor_panel;

	/* Text returned by a borrowed-string getter that does not fit the caller's
	 * buffer is truncated and NUL-terminated; every such helper returns the
	 * number of bytes (excluding NUL) the full text would need. */
	VGUI_API size_t vgui_editor_copy_string(char* out, size_t cap, const char* text);

	/* Host string callbacks (property value sources, per-property getters) use
	 * the same return convention: write the value, return the bytes the full
	 * text needs, and the kit calls again with an exact-size buffer when the
	 * first pass truncated it.  Return this value instead to report "no value",
	 * which rejects the read. */
#define VGUI_EDITOR_NO_VALUE ((size_t)-1)

	/* Message from the last failed editor operation on this thread (layout and
	 * document parsing, file I/O, rejected commands).  Returns "" when the last
	 * operation succeeded.  Borrowed; valid until the next editor call here. */
	VGUI_API const char* vgui_editor_last_error(void);

	/* ------------------------------------------------------------------------- */
	/* Editor context (selection + shared command stack)                         */
	/* ------------------------------------------------------------------------- */

	typedef struct vgui_editor_context vgui_editor_context;
	typedef struct vgui_editor_command_stack vgui_editor_command_stack;
	typedef struct vgui_editor_command_registry vgui_editor_command_registry;
	typedef struct vgui_editor_binding_undo vgui_editor_binding_undo;
	typedef struct vgui_editor_layout vgui_editor_layout;
	typedef struct vgui_editor_drag_target vgui_editor_drag_target;

	/* Single-id selection observer (primary id) and full-set observer. */
	typedef void (*vgui_editor_selection_fn)(void* user, vgui_editor_object_id id);
	typedef void (*vgui_editor_selection_set_fn)(void* user,
		const vgui_editor_object_id* ids, size_t id_count);

	VGUI_API vgui_editor_context* vgui_editor_context_create(vgui_context* ui);
	/* Does not destroy the vgui_context.  Destroy every editor handle that
	 * references this context (or its widgets) before calling it. */
	VGUI_API void vgui_editor_context_destroy(vgui_editor_context* ctx);
	VGUI_API vgui_context* vgui_editor_context_ui(vgui_editor_context* ctx);
	VGUI_API vgui_widget* vgui_editor_context_root(vgui_editor_context* ctx);
	/* Owned by the context. */
	VGUI_API vgui_editor_command_stack* vgui_editor_context_commands(
		vgui_editor_context* ctx);

	VGUI_API vgui_editor_object_id vgui_editor_context_selection(
		vgui_editor_context* ctx);
	VGUI_API size_t vgui_editor_context_selection_count(vgui_editor_context* ctx);
	VGUI_API vgui_editor_object_id vgui_editor_context_selection_id_at(
		vgui_editor_context* ctx, size_t index);
	VGUI_API int vgui_editor_context_is_selected(vgui_editor_context* ctx,
		vgui_editor_object_id id);
	VGUI_API void vgui_editor_context_set_selection(vgui_editor_context* ctx,
		vgui_editor_object_id id);
	/* Replaces the ordered set.  primary == VGUI_EDITOR_INVALID_OBJECT selects
	 * ids[0].  Duplicates and invalid ids are dropped. */
	VGUI_API void vgui_editor_context_set_selection_ids(vgui_editor_context* ctx,
		const vgui_editor_object_id* ids, size_t id_count,
		vgui_editor_object_id primary);
	VGUI_API void vgui_editor_context_add_to_selection(vgui_editor_context* ctx,
		vgui_editor_object_id id);
	VGUI_API void vgui_editor_context_remove_from_selection(
		vgui_editor_context* ctx, vgui_editor_object_id id);
	VGUI_API void vgui_editor_context_toggle_selection(vgui_editor_context* ctx,
		vgui_editor_object_id id);
	VGUI_API void vgui_editor_context_clear_selection(vgui_editor_context* ctx);

	/* Returns a non-zero token; 0 when the listener could not be installed. */
	VGUI_API size_t vgui_editor_context_add_selection_listener(
		vgui_editor_context* ctx, vgui_editor_selection_fn fn, void* user);
	VGUI_API int vgui_editor_context_remove_selection_listener(
		vgui_editor_context* ctx, size_t token);
	VGUI_API size_t vgui_editor_context_add_selection_set_listener(
		vgui_editor_context* ctx, vgui_editor_selection_set_fn fn, void* user);
	VGUI_API int vgui_editor_context_remove_selection_set_listener(
		vgui_editor_context* ctx, size_t token);

	/* ------------------------------------------------------------------------- */
	/* Command stack (undo / redo)                                               */
	/* ------------------------------------------------------------------------- */

	/* Applies the edit; returning 0 marks the command as failed so the stack
	 * does not record it. */
	typedef int (*vgui_editor_execute_fn)(void* user);
	/* Reverts the edit performed by the matching execute callback. */
	typedef void (*vgui_editor_undo_fn)(void* user);
	/* Optional: releases `user` when the command record is discarded (stack
	 * cleared, history truncated by a new command, stack destroyed). */
	typedef void (*vgui_editor_release_fn)(void* user);

	/* Executes and records on success.  Returns 0 when the command failed. */
	VGUI_API int vgui_editor_command_stack_execute(vgui_editor_command_stack* stack,
		const char* name, vgui_editor_execute_fn execute,
		vgui_editor_undo_fn undo, void* user, vgui_editor_release_fn release_user);
	/* Records a command whose effect has already been applied.  Inside a
	 * transaction it joins the current group.  reapply is only invoked later, by
	 * redo(); pass NULL when the redo path is handled by undo of a fresh edit. */
	VGUI_API void vgui_editor_command_stack_push(vgui_editor_command_stack* stack,
		const char* name, vgui_editor_execute_fn reapply, vgui_editor_undo_fn undo,
		void* user, vgui_editor_release_fn release_user);

	VGUI_API int vgui_editor_command_stack_undo(vgui_editor_command_stack* stack);
	VGUI_API int vgui_editor_command_stack_redo(vgui_editor_command_stack* stack);
	VGUI_API void vgui_editor_command_stack_clear(vgui_editor_command_stack* stack);
	VGUI_API int vgui_editor_command_stack_can_undo(
		vgui_editor_command_stack* stack);
	VGUI_API int vgui_editor_command_stack_can_redo(
		vgui_editor_command_stack* stack);
	VGUI_API size_t vgui_editor_command_stack_undo_count(
		vgui_editor_command_stack* stack);
	VGUI_API size_t vgui_editor_command_stack_redo_count(
		vgui_editor_command_stack* stack);
	/* Borrowed label of the next undo/redo record, or "" when empty. */
	VGUI_API const char* vgui_editor_command_stack_next_undo_name(
		vgui_editor_command_stack* stack);
	VGUI_API const char* vgui_editor_command_stack_next_redo_name(
		vgui_editor_command_stack* stack);

	/* Groups subsequent execute()/push() calls into one undo record. */
	VGUI_API void vgui_editor_command_stack_begin_transaction(
		vgui_editor_command_stack* stack, const char* name);
	VGUI_API int vgui_editor_command_stack_commit_transaction(
		vgui_editor_command_stack* stack);
	VGUI_API void vgui_editor_command_stack_rollback_transaction(
		vgui_editor_command_stack* stack);
	VGUI_API int vgui_editor_command_stack_in_transaction(
		vgui_editor_command_stack* stack);
	/* Destroys a stack created by vgui_editor_command_stack_create().  The
	 * stack owned by an editor context must not be destroyed here. */
	VGUI_API void vgui_editor_command_stack_destroy(vgui_editor_command_stack* stack);
	VGUI_API vgui_editor_command_stack* vgui_editor_command_stack_create(void);

	/* ------------------------------------------------------------------------- */
	/* Command registry (string ids, shortcuts, enabled / checked state)         */
	/* ------------------------------------------------------------------------- */

	typedef void (*vgui_editor_action_fn)(void* user);
	/* Predicate for enabled/checked state; 1 = true. */
	typedef int (*vgui_editor_predicate_fn)(void* user);

	typedef struct vgui_editor_command_info {
		uint32_t struct_size;
		const char* id;
		const char* title;
		/* Formatted shortcut such as "Ctrl+S"; "" when none is bound. */
		const char* shortcut;
		int enabled;
		int checked;
	} vgui_editor_command_info;

	VGUI_API void vgui_editor_command_info_default(vgui_editor_command_info* info);

	VGUI_API vgui_editor_command_registry* vgui_editor_command_registry_create(void);
	VGUI_API void vgui_editor_command_registry_destroy(
		vgui_editor_command_registry* registry);
	/* Owned by the registry, or borrowed from an editor shell / toolbar. */

	/* Adds a command, or updates the title/action of an existing id while
	 * keeping its shortcut and predicates.  Re-adding with the same `user`
	 * keeps release_user from being called twice. */
	VGUI_API int vgui_editor_command_registry_add(vgui_editor_command_registry* registry,
		const char* id, const char* title, vgui_editor_action_fn run, void* user,
		vgui_editor_release_fn release_user);
	VGUI_API int vgui_editor_command_registry_remove(
		vgui_editor_command_registry* registry, const char* id);
	VGUI_API void vgui_editor_command_registry_clear(
		vgui_editor_command_registry* registry);
	VGUI_API int vgui_editor_command_registry_has(vgui_editor_command_registry* registry,
		const char* id);
	VGUI_API size_t vgui_editor_command_registry_size(
		vgui_editor_command_registry* registry);
	/* Fills info with borrowed strings valid until the next mutation of the
	 * registry.  Returns 0 when index is out of range. */
	VGUI_API int vgui_editor_command_registry_entry_at(
		vgui_editor_command_registry* registry, size_t index,
		vgui_editor_command_info* info);
	VGUI_API int vgui_editor_command_registry_find(
		vgui_editor_command_registry* registry, const char* id,
		vgui_editor_command_info* info);

	/* Parses "Ctrl+Shift+Z", "Delete", ...  Returns 0 when the text is not a
	 * key combination; an empty string clears the shortcut. */
	VGUI_API int vgui_editor_command_registry_set_shortcut(
		vgui_editor_command_registry* registry, const char* id,
		const char* shortcut);
	/* Reads back the formatted shortcut.  Returns the full length. */
	VGUI_API size_t vgui_editor_command_registry_get_shortcut(
		vgui_editor_command_registry* registry, const char* id, char* out,
		size_t cap);
	VGUI_API int vgui_editor_command_registry_set_enabled(
		vgui_editor_command_registry* registry, const char* id,
		vgui_editor_predicate_fn predicate, void* user);
	VGUI_API int vgui_editor_command_registry_set_checked(
		vgui_editor_command_registry* registry, const char* id,
		vgui_editor_predicate_fn predicate, void* user);

	/* Runs the command when it exists and is enabled.  Returns 1 when the
	 * action ran. */
	VGUI_API int vgui_editor_command_registry_invoke(
		vgui_editor_command_registry* registry, const char* id);
	VGUI_API int vgui_editor_command_registry_is_enabled(
		vgui_editor_command_registry* registry, const char* id);
	VGUI_API int vgui_editor_command_registry_is_checked(
		vgui_editor_command_registry* registry, const char* id);

	/* Keyboard dispatch; only key-down events fire commands.  Returns 1 when
	 * the event was consumed (including a disabled shortcut). */
	VGUI_API int vgui_editor_command_registry_handle_key(
		vgui_editor_command_registry* registry, int key, int down, int mods);
	VGUI_API int vgui_editor_command_registry_matches_shortcut(
		vgui_editor_command_registry* registry, int key, int mods);

	/* ------------------------------------------------------------------------- */
	/* Undo for the core data-binding layer                                      */
	/* ------------------------------------------------------------------------- */

	/* Installs the context's binding recorder; `stack` must outlive the handle.
	 * Destroy the handle before the vgui_context. */
	VGUI_API vgui_editor_binding_undo* vgui_editor_binding_undo_create(
		vgui_context* ctx, vgui_editor_command_stack* stack);
	VGUI_API void vgui_editor_binding_undo_destroy(vgui_editor_binding_undo* undo);
	VGUI_API void vgui_editor_binding_undo_set_label(vgui_editor_binding_undo* undo,
		const char* label);
	VGUI_API const char* vgui_editor_binding_undo_label(vgui_editor_binding_undo* undo);
	/* Consecutive edits of the same target inside this window merge into one
	 * undo record.  0 disables merging.  Default 500 ms. */
	VGUI_API void vgui_editor_binding_undo_set_merge_window(
		vgui_editor_binding_undo* undo, int milliseconds);
	VGUI_API int vgui_editor_binding_undo_merge_window(
		vgui_editor_binding_undo* undo);
	VGUI_API void vgui_editor_binding_undo_set_enabled(
		vgui_editor_binding_undo* undo, int enabled);
	VGUI_API int vgui_editor_binding_undo_enabled(vgui_editor_binding_undo* undo);
	VGUI_API size_t vgui_editor_binding_undo_recorded(vgui_editor_binding_undo* undo);
	VGUI_API int vgui_editor_binding_undo_attached(vgui_editor_binding_undo* undo);
	VGUI_API int vgui_editor_binding_undo_owns_recorder(vgui_context* ctx);

	/* ------------------------------------------------------------------------- */
	/* Drag & drop bridge                                                        */
	/* ------------------------------------------------------------------------- */

	typedef enum vgui_editor_drag_kind {
		VGUI_EDITOR_DRAG_NONE = 0,
		VGUI_EDITOR_DRAG_TEXT,
		VGUI_EDITOR_DRAG_FILES,
		VGUI_EDITOR_DRAG_ASSET,
		VGUI_EDITOR_DRAG_SCENE_OBJECT,
		VGUI_EDITOR_DRAG_CUSTOM
	} vgui_editor_drag_kind;

	/* Payload exchanged by drags.  All strings are borrowed on the way in (they
	 * are copied by the kit) and point into kit storage on the way out (valid
	 * until the callback returns). */
	typedef struct vgui_editor_drag_payload {
		uint32_t struct_size;
		vgui_editor_drag_kind kind;
		const char* text;
		const char* const* files;
		int file_count;
		/* Stable string id of the dragged object (asset path, object id...). */
		const char* id;
		const void* data;
	} vgui_editor_drag_payload;

	VGUI_API void vgui_editor_drag_payload_default(vgui_editor_drag_payload* payload);

	/* Drop target callbacks.  can_accept decides routing; hover may fill hint
	 * (leave hint->kind == VGUI_DROP_HINT_NONE to reject the position); drop
	 * returns 1 when the payload was consumed. */
	typedef struct vgui_editor_drop_target_desc {
		uint32_t struct_size;
		int (*can_accept)(void* user, const vgui_editor_drag_payload* payload);
		void (*hover)(void* user, const vgui_editor_drag_payload* payload,
			vgui_widget* target, float x, float y, vgui_drop_hint* hint);
		int (*drop)(void* user, const vgui_editor_drag_payload* payload,
			float x, float y);
		vgui_editor_release_fn release_user;
	} vgui_editor_drop_target_desc;

	VGUI_API void vgui_editor_drop_target_desc_default(
		vgui_editor_drop_target_desc* desc);

	VGUI_API vgui_editor_drag_target* vgui_editor_drag_target_create(
		const vgui_editor_drop_target_desc* desc, void* user);
	VGUI_API void vgui_editor_drag_target_destroy(vgui_editor_drag_target* target);
	/* Attaches to a widget (pass widget = NULL to detach).  One widget can hold
	 * one target; attaching again detaches the previous one. */
	VGUI_API int vgui_editor_drag_target_attach(vgui_editor_drag_target* target,
		vgui_widget* widget);
	VGUI_API vgui_widget* vgui_editor_drag_target_widget(
		vgui_editor_drag_target* target);

	/* Starts a drag from an editor payload.  Returns 0 when another drag is
	 * already in flight or the payload is empty. */
	VGUI_API int vgui_editor_begin_drag(vgui_context* ctx, vgui_widget* source,
		const vgui_editor_drag_payload* payload);

	/* Row drag source for lists and trees: fill `out` for the pressed row and
	 * return 1 to start the drag.  Pass fn = NULL to detach.  The kit converts
	 * the payload for the core drag layer. */
	typedef int (*vgui_editor_row_drag_fn)(void* user, vgui_item_id item,
		int index, vgui_editor_drag_payload* out);
	VGUI_API void vgui_editor_widget_set_row_drag(vgui_widget* widget, int tree,
		vgui_editor_row_drag_fn fn, void* user);

	/* ------------------------------------------------------------------------- */
	/* Persistent shell layout                                                   */
	/* ------------------------------------------------------------------------- */

	VGUI_API vgui_editor_layout* vgui_editor_layout_create(void);
	VGUI_API void vgui_editor_layout_destroy(vgui_editor_layout* layout);
	/* Fills info with the current layout values.  The version field reports the
	 * format (7 for a single-window layout). */
	VGUI_API int vgui_editor_layout_get_version(vgui_editor_layout* layout);
	VGUI_API void vgui_editor_layout_set_panel_visible(
		vgui_editor_layout* layout, vgui_editor_panel panel, int visible);
	VGUI_API int vgui_editor_layout_panel_visible(vgui_editor_layout* layout,
		vgui_editor_panel panel);
	VGUI_API void vgui_editor_layout_set_ratios(vgui_editor_layout* layout,
		float body_ratio, float workspace_ratio, float center_ratio);
	VGUI_API void vgui_editor_layout_get_ratios(vgui_editor_layout* layout,
		float* body_ratio, float* workspace_ratio, float* center_ratio);
	/* Opaque per-view state strings (table / timeline / node graph / code).  The
	 * layout carries them verbatim; the owning view produces and consumes them.
	 * Setters copy the text; getters return borrowed strings ("" when unset). */
	VGUI_API void vgui_editor_layout_set_view_state(vgui_editor_layout* layout,
		const char* which, const char* state);
	VGUI_API const char* vgui_editor_layout_view_state(vgui_editor_layout* layout,
		const char* which);
	/* JSON round-trip.  to_json returns the borrowed text of the last capture;
	 * from_json returns 0 and sets vgui_editor_last_error() on rejection. */
	VGUI_API const char* vgui_editor_layout_to_json(vgui_editor_layout* layout);
	VGUI_API int vgui_editor_layout_from_json(vgui_editor_layout* layout,
		const char* json);
	VGUI_API int vgui_editor_layout_save_file(vgui_editor_layout* layout,
		const char* path);
	VGUI_API int vgui_editor_layout_load_file(vgui_editor_layout* layout,
		const char* path);

	/* ------------------------------------------------------------------------- */
	/* Spinbox                                                                   */
	/* ------------------------------------------------------------------------- */

	typedef struct vgui_editor_spinbox vgui_editor_spinbox;

	typedef enum vgui_editor_value_type {
		VGUI_EDITOR_VALUE_INTEGER = 0,
		VGUI_EDITOR_VALUE_NUMBER
	} vgui_editor_value_type;

	typedef void (*vgui_editor_value_fn)(void* user, double value);

	VGUI_API vgui_editor_spinbox* vgui_editor_spinbox_create(vgui_widget* parent,
		const char* label);
	VGUI_API void vgui_editor_spinbox_destroy(vgui_editor_spinbox* spin);
	VGUI_API vgui_widget* vgui_editor_spinbox_widget(vgui_editor_spinbox* spin);
	VGUI_API vgui_widget* vgui_editor_spinbox_edit_widget(vgui_editor_spinbox* spin);
	VGUI_API vgui_widget* vgui_editor_spinbox_label_widget(vgui_editor_spinbox* spin);
	VGUI_API vgui_widget* vgui_editor_spinbox_decrement_widget(
		vgui_editor_spinbox* spin);
	VGUI_API vgui_widget* vgui_editor_spinbox_increment_widget(
		vgui_editor_spinbox* spin);

	/* Widget id prefix.  Call before or after creation; applying it later renames
	 * the existing widgets. */
	VGUI_API void vgui_editor_spinbox_set_id(vgui_editor_spinbox* spin,
		const char* id);
	VGUI_API const char* vgui_editor_spinbox_id(vgui_editor_spinbox* spin);
	VGUI_API void vgui_editor_spinbox_set_label(vgui_editor_spinbox* spin,
		const char* label);
	VGUI_API const char* vgui_editor_spinbox_label(vgui_editor_spinbox* spin);
	/* Label column width (default 72 logical px). */
	VGUI_API void vgui_editor_spinbox_set_label_width(vgui_editor_spinbox* spin,
		float width);
	VGUI_API float vgui_editor_spinbox_label_width(vgui_editor_spinbox* spin);
	/* Chrome font size; 0 follows the context font.  Row height is font + 10,
	 * at least 24. */
	VGUI_API void vgui_editor_spinbox_set_font_size(vgui_editor_spinbox* spin,
		float size);
	VGUI_API float vgui_editor_spinbox_font_size(vgui_editor_spinbox* spin);
	/* Edit width: 0 follows the theme's edit.min_width. */
	VGUI_API void vgui_editor_spinbox_set_edit_min_width(vgui_editor_spinbox* spin,
		float width);
	VGUI_API void vgui_editor_spinbox_set_auto_width(vgui_editor_spinbox* spin,
		int enabled);
	VGUI_API int vgui_editor_spinbox_auto_width(vgui_editor_spinbox* spin);
	VGUI_API void vgui_editor_spinbox_set_buttons_visible(vgui_editor_spinbox* spin,
		int visible);
	VGUI_API int vgui_editor_spinbox_buttons_visible(vgui_editor_spinbox* spin);

	VGUI_API void vgui_editor_spinbox_set_value_type(vgui_editor_spinbox* spin,
		vgui_editor_value_type type);
	VGUI_API vgui_editor_value_type vgui_editor_spinbox_value_type(
		vgui_editor_spinbox* spin);
	/* Inclusive range; setting it clamps without notifying. */
	VGUI_API void vgui_editor_spinbox_set_range(vgui_editor_spinbox* spin,
		double min_value, double max_value);
	VGUI_API void vgui_editor_spinbox_clear_range(vgui_editor_spinbox* spin);
	VGUI_API int vgui_editor_spinbox_has_range(vgui_editor_spinbox* spin);
	VGUI_API double vgui_editor_spinbox_min_value(vgui_editor_spinbox* spin);
	VGUI_API double vgui_editor_spinbox_max_value(vgui_editor_spinbox* spin);
	/* 0 selects the default step (1 for integers, 0.1 for numbers). */
	VGUI_API void vgui_editor_spinbox_set_step(vgui_editor_spinbox* spin,
		double step);
	VGUI_API double vgui_editor_spinbox_step(vgui_editor_spinbox* spin);

	/* Silent programmatic update. */
	VGUI_API void vgui_editor_spinbox_set_value(vgui_editor_spinbox* spin,
		double value);
	VGUI_API double vgui_editor_spinbox_value(vgui_editor_spinbox* spin);
	VGUI_API const char* vgui_editor_spinbox_value_text(vgui_editor_spinbox* spin);
	/* User-style edits: reject read-only/disabled controls, validate, clamp and
	 * notify.  Return 0 when the edit was rejected. */
	VGUI_API int vgui_editor_spinbox_commit_text(vgui_editor_spinbox* spin,
		const char* text);
	VGUI_API int vgui_editor_spinbox_commit_value(vgui_editor_spinbox* spin,
		double value);
	VGUI_API void vgui_editor_spinbox_step_by(vgui_editor_spinbox* spin, int steps);

	VGUI_API void vgui_editor_spinbox_set_read_only(vgui_editor_spinbox* spin,
		int read_only);
	VGUI_API int vgui_editor_spinbox_read_only(vgui_editor_spinbox* spin);
	VGUI_API void vgui_editor_spinbox_set_enabled(vgui_editor_spinbox* spin,
		int enabled);
	VGUI_API int vgui_editor_spinbox_enabled(vgui_editor_spinbox* spin);
	/* Validation message shown by the widget; "" clears it. */
	VGUI_API void vgui_editor_spinbox_set_error(vgui_editor_spinbox* spin,
		const char* error);
	VGUI_API const char* vgui_editor_spinbox_error(vgui_editor_spinbox* spin);
	VGUI_API void vgui_editor_spinbox_focus(vgui_editor_spinbox* spin);
	/* change fires for every value change, commit once per finished edit. */
	VGUI_API void vgui_editor_spinbox_set_on_change(vgui_editor_spinbox* spin,
		vgui_editor_value_fn fn, void* user);
	VGUI_API void vgui_editor_spinbox_set_on_commit(vgui_editor_spinbox* spin,
		vgui_editor_value_fn fn, void* user);

	/* ------------------------------------------------------------------------- */
	/* Combo box                                                                 */
	/* ------------------------------------------------------------------------- */

	typedef struct vgui_editor_combo_box vgui_editor_combo_box;

	typedef struct vgui_editor_combo_item {
		uint32_t struct_size;
		const char* label;
		const char* value;
		int enabled;
	} vgui_editor_combo_item;

	VGUI_API void vgui_editor_combo_item_default(vgui_editor_combo_item* item);

	/* index is -1 when nothing is selected; value is "" then. */
	typedef void (*vgui_editor_combo_fn)(void* user, int index, const char* value);

	VGUI_API vgui_editor_combo_box* vgui_editor_combo_box_create(vgui_widget* parent);
	VGUI_API void vgui_editor_combo_box_destroy(vgui_editor_combo_box* combo);
	VGUI_API vgui_widget* vgui_editor_combo_box_widget(vgui_editor_combo_box* combo);
	VGUI_API void vgui_editor_combo_box_set_id(vgui_editor_combo_box* combo,
		const char* id);
	VGUI_API const char* vgui_editor_combo_box_id(vgui_editor_combo_box* combo);

	/* Replaces the list; keeps the current value when an item still matches. */
	VGUI_API void vgui_editor_combo_box_set_items(vgui_editor_combo_box* combo,
		const vgui_editor_combo_item* items, size_t count);
	VGUI_API size_t vgui_editor_combo_box_item_count(vgui_editor_combo_box* combo);
	/* Borrowed item strings; valid until the next set_items()/set_item_enabled().
	 * Returns 0 when index is out of range. */
	VGUI_API int vgui_editor_combo_box_item_at(vgui_editor_combo_box* combo,
		int index, vgui_editor_combo_item* out);
	VGUI_API int vgui_editor_combo_box_index_of_value(vgui_editor_combo_box* combo,
		const char* value);
	VGUI_API int vgui_editor_combo_box_set_item_enabled(vgui_editor_combo_box* combo,
		int index, int enabled);

	VGUI_API void vgui_editor_combo_box_set_placeholder(vgui_editor_combo_box* combo,
		const char* text);
	VGUI_API const char* vgui_editor_combo_box_placeholder(
		vgui_editor_combo_box* combo);
	/* Selected label, or the placeholder when nothing is selected. */
	VGUI_API const char* vgui_editor_combo_box_display_text(
		vgui_editor_combo_box* combo);

	/* Silent selection changes.  Index -1 clears the selection; a silent setter
	 * may select a disabled item. */
	VGUI_API int vgui_editor_combo_box_set_selected_index(
		vgui_editor_combo_box* combo, int index);
	VGUI_API int vgui_editor_combo_box_set_value(vgui_editor_combo_box* combo,
		const char* value);
	VGUI_API void vgui_editor_combo_box_clear_selection(vgui_editor_combo_box* combo);
	/* User-style selection: rejects disabled/out-of-range items and notifies. */
	VGUI_API int vgui_editor_combo_box_select_index(vgui_editor_combo_box* combo,
		int index);
	VGUI_API int vgui_editor_combo_box_select_value(vgui_editor_combo_box* combo,
		const char* value);
	VGUI_API int vgui_editor_combo_box_select_none(vgui_editor_combo_box* combo);

	VGUI_API int vgui_editor_combo_box_selected_index(vgui_editor_combo_box* combo);
	VGUI_API const char* vgui_editor_combo_box_value(vgui_editor_combo_box* combo);

	VGUI_API int vgui_editor_combo_box_open(vgui_editor_combo_box* combo);
	VGUI_API void vgui_editor_combo_box_close(vgui_editor_combo_box* combo);
	VGUI_API int vgui_editor_combo_box_is_open(vgui_editor_combo_box* combo);
	VGUI_API void vgui_editor_combo_box_toggle(vgui_editor_combo_box* combo);

	VGUI_API void vgui_editor_combo_box_set_enabled(vgui_editor_combo_box* combo,
		int enabled);
	VGUI_API int vgui_editor_combo_box_enabled(vgui_editor_combo_box* combo);
	VGUI_API void vgui_editor_combo_box_set_read_only(vgui_editor_combo_box* combo,
		int read_only);
	VGUI_API int vgui_editor_combo_box_read_only(vgui_editor_combo_box* combo);
	VGUI_API void vgui_editor_combo_box_focus(vgui_editor_combo_box* combo);
	VGUI_API void vgui_editor_combo_box_set_on_change(vgui_editor_combo_box* combo,
		vgui_editor_combo_fn fn, void* user);
	VGUI_API void vgui_editor_combo_box_set_on_commit(vgui_editor_combo_box* combo,
		vgui_editor_combo_fn fn, void* user);

	/* ------------------------------------------------------------------------- */
	/* Search box                                                                */
	/* ------------------------------------------------------------------------- */

	typedef struct vgui_editor_search_box vgui_editor_search_box;
	typedef void (*vgui_editor_text_fn)(void* user, const char* text);

	VGUI_API vgui_editor_search_box* vgui_editor_search_box_create(
		vgui_widget* parent, const char* placeholder);
	VGUI_API void vgui_editor_search_box_destroy(vgui_editor_search_box* search);
	VGUI_API vgui_widget* vgui_editor_search_box_widget(vgui_editor_search_box* search);
	VGUI_API vgui_widget* vgui_editor_search_box_edit_widget(
		vgui_editor_search_box* search);
	VGUI_API vgui_widget* vgui_editor_search_box_clear_widget(
		vgui_editor_search_box* search);
	VGUI_API void vgui_editor_search_box_set_placeholder(
		vgui_editor_search_box* search, const char* text);
	/* Silent programmatic update. */
	VGUI_API void vgui_editor_search_box_set_text(vgui_editor_search_box* search,
		const char* text);
	VGUI_API const char* vgui_editor_search_box_text(vgui_editor_search_box* search);
	/* User-style clear / submit: both notify. */
	VGUI_API void vgui_editor_search_box_clear(vgui_editor_search_box* search);
	VGUI_API void vgui_editor_search_box_submit(vgui_editor_search_box* search);
	VGUI_API void vgui_editor_search_box_focus(vgui_editor_search_box* search);
	VGUI_API void vgui_editor_search_box_set_on_change(
		vgui_editor_search_box* search, vgui_editor_text_fn fn, void* user);
	VGUI_API void vgui_editor_search_box_set_on_submit(
		vgui_editor_search_box* search, vgui_editor_text_fn fn, void* user);

	/* ------------------------------------------------------------------------- */
	/* Breadcrumb                                                                */
	/* ------------------------------------------------------------------------- */

	typedef struct vgui_editor_breadcrumb vgui_editor_breadcrumb;

	typedef struct vgui_editor_breadcrumb_item {
		uint32_t struct_size;
		const char* label;
		/* Optional stable id; may be NULL. */
		const char* id;
	} vgui_editor_breadcrumb_item;

	VGUI_API void vgui_editor_breadcrumb_item_default(vgui_editor_breadcrumb_item* item);

	typedef void (*vgui_editor_navigate_fn)(void* user, size_t index,
		const char* id);

	VGUI_API vgui_editor_breadcrumb* vgui_editor_breadcrumb_create(vgui_widget* parent);
	VGUI_API void vgui_editor_breadcrumb_destroy(vgui_editor_breadcrumb* path);
	VGUI_API vgui_widget* vgui_editor_breadcrumb_widget(vgui_editor_breadcrumb* path);
	VGUI_API void vgui_editor_breadcrumb_add_item(vgui_editor_breadcrumb* path,
		const char* label, const char* id);
	VGUI_API void vgui_editor_breadcrumb_set_items(vgui_editor_breadcrumb* path,
		const vgui_editor_breadcrumb_item* items, size_t count);
	VGUI_API void vgui_editor_breadcrumb_clear(vgui_editor_breadcrumb* path);
	VGUI_API size_t vgui_editor_breadcrumb_count(vgui_editor_breadcrumb* path);
	/* Borrowed strings, valid until the next mutation.  Returns 0 out of range. */
	VGUI_API int vgui_editor_breadcrumb_item_at(vgui_editor_breadcrumb* path,
		size_t index, vgui_editor_breadcrumb_item* out);
	VGUI_API vgui_widget* vgui_editor_breadcrumb_item_widget(
		vgui_editor_breadcrumb* path, size_t index);
	/* User-style navigation (notifies); set_active is the silent form. */
	VGUI_API int vgui_editor_breadcrumb_navigate(vgui_editor_breadcrumb* path,
		size_t index);
	VGUI_API void vgui_editor_breadcrumb_set_active(vgui_editor_breadcrumb* path,
		size_t index);
	VGUI_API size_t vgui_editor_breadcrumb_active(vgui_editor_breadcrumb* path);
	VGUI_API void vgui_editor_breadcrumb_set_on_navigate(
		vgui_editor_breadcrumb* path, vgui_editor_navigate_fn fn, void* user);

	/* ------------------------------------------------------------------------- */
	/* Toolbar family                                                            */
	/* ------------------------------------------------------------------------- */

	typedef struct vgui_editor_icon_button vgui_editor_icon_button;
	typedef struct vgui_editor_toggle_button vgui_editor_toggle_button;
	typedef struct vgui_editor_segmented_control vgui_editor_segmented_control;
	typedef struct vgui_editor_toolbar vgui_editor_toolbar;

	typedef void (*vgui_editor_click_fn)(void* user);
	typedef void (*vgui_editor_toggle_fn)(void* user, int checked);
	typedef void (*vgui_editor_segment_fn)(void* user, size_t index);

	VGUI_API vgui_editor_icon_button* vgui_editor_icon_button_create(
		vgui_widget* parent, const char* text);
	VGUI_API void vgui_editor_icon_button_destroy(vgui_editor_icon_button* button);
	VGUI_API vgui_widget* vgui_editor_icon_button_widget(
		vgui_editor_icon_button* button);
	/* width/height 0 keeps the texture's own size.  Pass texture = NULL to drop
	 * the icon. */
	VGUI_API void vgui_editor_icon_button_set_icon(vgui_editor_icon_button* button,
		vgui_texture* texture, int width, int height);
	VGUI_API void vgui_editor_icon_button_set_text(vgui_editor_icon_button* button,
		const char* text);
	VGUI_API void vgui_editor_icon_button_set_tooltip(vgui_editor_icon_button* button,
		const char* text);
	VGUI_API void vgui_editor_icon_button_set_enabled(
		vgui_editor_icon_button* button, int enabled);
	VGUI_API int vgui_editor_icon_button_enabled(vgui_editor_icon_button* button);
	VGUI_API void vgui_editor_icon_button_set_checked(
		vgui_editor_icon_button* button, int checked);
	VGUI_API int vgui_editor_icon_button_checked(vgui_editor_icon_button* button);
	VGUI_API void vgui_editor_icon_button_set_on_click(
		vgui_editor_icon_button* button, vgui_editor_click_fn fn, void* user);
	/* Simulates a click (notifies the listener). */
	VGUI_API void vgui_editor_icon_button_click(vgui_editor_icon_button* button);

	VGUI_API vgui_editor_toggle_button* vgui_editor_toggle_button_create(
		vgui_widget* parent, const char* text);
	VGUI_API void vgui_editor_toggle_button_destroy(vgui_editor_toggle_button* button);
	VGUI_API vgui_widget* vgui_editor_toggle_button_widget(
		vgui_editor_toggle_button* button);
	VGUI_API void vgui_editor_toggle_button_set_icon(
		vgui_editor_toggle_button* button, vgui_texture* texture, int width,
		int height);
	VGUI_API void vgui_editor_toggle_button_set_text(
		vgui_editor_toggle_button* button, const char* text);
	VGUI_API void vgui_editor_toggle_button_set_tooltip(
		vgui_editor_toggle_button* button, const char* text);
	VGUI_API void vgui_editor_toggle_button_set_enabled(
		vgui_editor_toggle_button* button, int enabled);
	VGUI_API int vgui_editor_toggle_button_enabled(vgui_editor_toggle_button* button);
	VGUI_API void vgui_editor_toggle_button_set_checked(
		vgui_editor_toggle_button* button, int checked);
	VGUI_API int vgui_editor_toggle_button_checked(vgui_editor_toggle_button* button);
	/* User-style flip (notifies). */
	VGUI_API void vgui_editor_toggle_button_toggle(vgui_editor_toggle_button* button);
	VGUI_API void vgui_editor_toggle_button_set_on_change(
		vgui_editor_toggle_button* button, vgui_editor_toggle_fn fn, void* user);

	VGUI_API vgui_editor_segmented_control* vgui_editor_segmented_control_create(
		vgui_widget* parent);
	VGUI_API void vgui_editor_segmented_control_destroy(
		vgui_editor_segmented_control* seg);
	VGUI_API vgui_widget* vgui_editor_segmented_control_widget(
		vgui_editor_segmented_control* seg);
	VGUI_API size_t vgui_editor_segmented_control_add_segment(
		vgui_editor_segmented_control* seg, const char* text);
	VGUI_API void vgui_editor_segmented_control_clear(
		vgui_editor_segmented_control* seg);
	VGUI_API size_t vgui_editor_segmented_control_count(
		vgui_editor_segmented_control* seg);
	/* User-style select (notifies) vs silent set_selected. */
	VGUI_API int vgui_editor_segmented_control_select(
		vgui_editor_segmented_control* seg, size_t index);
	VGUI_API void vgui_editor_segmented_control_set_selected(
		vgui_editor_segmented_control* seg, size_t index);
	VGUI_API size_t vgui_editor_segmented_control_selected(
		vgui_editor_segmented_control* seg);
	VGUI_API int vgui_editor_segmented_control_has_selection(
		vgui_editor_segmented_control* seg);
	VGUI_API void vgui_editor_segmented_control_set_segment_enabled(
		vgui_editor_segmented_control* seg, size_t index, int enabled);
	VGUI_API int vgui_editor_segmented_control_segment_enabled(
		vgui_editor_segmented_control* seg, size_t index);
	VGUI_API void vgui_editor_segmented_control_set_on_change(
		vgui_editor_segmented_control* seg, vgui_editor_segment_fn fn, void* user);
	VGUI_API vgui_widget* vgui_editor_segmented_control_segment_widget(
		vgui_editor_segmented_control* seg, size_t index);

	VGUI_API vgui_editor_toolbar* vgui_editor_toolbar_create(vgui_widget* parent);
	VGUI_API void vgui_editor_toolbar_destroy(vgui_editor_toolbar* toolbar);
	VGUI_API vgui_widget* vgui_editor_toolbar_widget(vgui_editor_toolbar* toolbar);
	/* Owned by the toolbar: use them, do not destroy them individually. */
	VGUI_API vgui_editor_icon_button* vgui_editor_toolbar_add_icon_button(
		vgui_editor_toolbar* toolbar, const char* text, vgui_texture* icon,
		int icon_width, int icon_height);
	VGUI_API vgui_editor_toggle_button* vgui_editor_toolbar_add_toggle_button(
		vgui_editor_toolbar* toolbar, const char* text, int checked);
	VGUI_API vgui_editor_segmented_control* vgui_editor_toolbar_add_segmented_control(
		vgui_editor_toolbar* toolbar);
	/* Binds a button to a registry command so enabled/checked state follows the
	 * registry.  Returns NULL for an unknown id.  The registry must outlive the
	 * toolbar. */
	VGUI_API vgui_editor_icon_button* vgui_editor_toolbar_add_command_button(
		vgui_editor_toolbar* toolbar, vgui_editor_command_registry* registry,
		const char* id, vgui_texture* icon, int icon_width, int icon_height);
	VGUI_API vgui_widget* vgui_editor_toolbar_add_label(vgui_editor_toolbar* toolbar,
		const char* text, int grow);
	VGUI_API vgui_widget* vgui_editor_toolbar_add_spacer(vgui_editor_toolbar* toolbar,
		float grow);
	VGUI_API vgui_widget* vgui_editor_toolbar_add_separator(vgui_editor_toolbar* toolbar);
	/* Re-reads enabled/checked state for every command button. */
	VGUI_API void vgui_editor_toolbar_refresh_commands(vgui_editor_toolbar* toolbar);

	/* ------------------------------------------------------------------------- */
	/* Status bar                                                                */
	/* ------------------------------------------------------------------------- */

	typedef struct vgui_editor_status_bar vgui_editor_status_bar;

	VGUI_API vgui_editor_status_bar* vgui_editor_status_bar_create(vgui_widget* parent);
	VGUI_API void vgui_editor_status_bar_destroy(vgui_editor_status_bar* status);
	VGUI_API vgui_widget* vgui_editor_status_bar_widget(vgui_editor_status_bar* status);
	VGUI_API vgui_widget* vgui_editor_status_bar_message_widget(
		vgui_editor_status_bar* status);
	VGUI_API vgui_widget* vgui_editor_status_bar_selection_widget(
		vgui_editor_status_bar* status);
	VGUI_API vgui_widget* vgui_editor_status_bar_coordinate_widget(
		vgui_editor_status_bar* status);
	VGUI_API vgui_widget* vgui_editor_status_bar_error_widget(
		vgui_editor_status_bar* status);
	/* Fields can be set before the widgets exist; the text is applied at build. */
	VGUI_API void vgui_editor_status_bar_set_message(vgui_editor_status_bar* status,
		const char* text);
	VGUI_API const char* vgui_editor_status_bar_message(vgui_editor_status_bar* status);
	/* Shows "Selected: <id>"; VGUI_EDITOR_INVALID_OBJECT clears the field. */
	VGUI_API void vgui_editor_status_bar_set_selected_object(
		vgui_editor_status_bar* status, vgui_editor_object_id id);
	VGUI_API void vgui_editor_status_bar_set_selection_text(
		vgui_editor_status_bar* status, const char* text);
	VGUI_API const char* vgui_editor_status_bar_selection_text(
		vgui_editor_status_bar* status);
	/* Shows "x 1.25  y -2.00". */
	VGUI_API void vgui_editor_status_bar_set_coordinates(vgui_editor_status_bar* status,
		float x, float y);
	VGUI_API void vgui_editor_status_bar_set_coordinate_text(
		vgui_editor_status_bar* status, const char* text);
	VGUI_API const char* vgui_editor_status_bar_coordinate_text(
		vgui_editor_status_bar* status);
	VGUI_API void vgui_editor_status_bar_set_error(vgui_editor_status_bar* status,
		const char* text);
	VGUI_API void vgui_editor_status_bar_clear_error(vgui_editor_status_bar* status);
	VGUI_API const char* vgui_editor_status_bar_error(vgui_editor_status_bar* status);

	/* ------------------------------------------------------------------------- */
	/* Color picker                                                              */
	/* ------------------------------------------------------------------------- */

	typedef struct vgui_editor_color_picker vgui_editor_color_picker;

	/* Canonical value-domain hex ("#RRGGBB" / "#RRGGBBAA"). */
	typedef void (*vgui_editor_color_fn)(void* user, const char* hex);

	/* Value-domain <-> edit-domain conversion (for example sRGB <-> linear).
	 * Write the converted hex into out and return the bytes the full text needs.
	 * Returning VGUI_EDITOR_NO_VALUE makes that direction produce an empty
	 * string, which the picker rejects on the value-domain path - a from_edit
	 * transform should therefore always produce parseable hex. */
	typedef size_t (*vgui_editor_color_transform_fn)(const char* hex, char* out,
		size_t cap, void* user);

	/* Builds the detached panel that vgui_popup_open() expects. */
	VGUI_API vgui_editor_color_picker* vgui_editor_color_picker_create(
		vgui_context* ui);
	VGUI_API void vgui_editor_color_picker_destroy(vgui_editor_color_picker* picker);
	VGUI_API vgui_widget* vgui_editor_color_picker_widget(
		vgui_editor_color_picker* picker);
	VGUI_API void vgui_editor_color_picker_set_id(vgui_editor_color_picker* picker,
		const char* id);

	/* Silent programmatic updates.  NULL is the empty string. */
	VGUI_API void vgui_editor_color_picker_set_value(vgui_editor_color_picker* picker,
		const char* hex);
	/* Borrowed; valid until the next mutation of this handle. */
	VGUI_API const char* vgui_editor_color_picker_value(
		vgui_editor_color_picker* picker);
	/* Session start color, shown as the "current" swatch. */
	VGUI_API void vgui_editor_color_picker_set_current(
		vgui_editor_color_picker* picker, const char* hex);

	/* press fires on the mouse-down that starts a gesture, change on every
	 * update, commit once when the gesture finishes. */
	VGUI_API void vgui_editor_color_picker_set_on_press(
		vgui_editor_color_picker* picker, vgui_editor_color_fn fn, void* user);
	VGUI_API void vgui_editor_color_picker_set_on_change(
		vgui_editor_color_picker* picker, vgui_editor_color_fn fn, void* user);
	VGUI_API void vgui_editor_color_picker_set_on_commit(
		vgui_editor_color_picker* picker, vgui_editor_color_fn fn, void* user);

	VGUI_API void vgui_editor_color_picker_set_presets(
		vgui_editor_color_picker* picker, const char* const* hexes, size_t count);
	VGUI_API void vgui_editor_color_picker_set_recent(
		vgui_editor_color_picker* picker, const char* const* hexes, size_t count);

	/* NULL fn installs the identity transform for that direction. */
	VGUI_API void vgui_editor_color_picker_set_color_transform(
		vgui_editor_color_picker* picker, vgui_editor_color_transform_fn to_edit,
		void* to_edit_user, vgui_editor_color_transform_fn from_edit,
		void* from_edit_user);

	VGUI_API int vgui_editor_color_picker_valid(vgui_editor_color_picker* picker);
	/* Current hue in degrees (0..360), kept at the hue-bar position even at the
	 * wrap-around end. */
	VGUI_API float vgui_editor_color_picker_hue_degrees(
		vgui_editor_color_picker* picker);

	/* ------------------------------------------------------------------------- */
	/* Vector editor (Vector2/3/4)                                               */
	/* ------------------------------------------------------------------------- */

	typedef struct vgui_editor_vector_editor vgui_editor_vector_editor;

#define VGUI_EDITOR_VECTOR_MAX_COMPONENTS 4

	typedef enum vgui_editor_vector_semantic {
		VGUI_EDITOR_VECTOR_GENERIC = 0,
		VGUI_EDITOR_VECTOR_POSITION,
		VGUI_EDITOR_VECTOR_COLOR
	} vgui_editor_vector_semantic;

	/* values holds VGUI_EDITOR_VECTOR_MAX_COMPONENTS doubles; only the first
	 * `dimension` are meaningful. */
	typedef void (*vgui_editor_vector_fn)(void* user, const double* values,
		int dimension);
	/* Writes the vector and returns non-zero when the source has a value. */
	typedef int (*vgui_editor_vector_source_fn)(void* user, double* values,
		int dimension);

	/* dimension is 2, 3 or 4; 0 uses the default (3).  It cannot change after
	 * the widget exists, so use set_dimension() before create() to pick another
	 * count - here, passing 0 after a previous build keeps it. */
	VGUI_API vgui_editor_vector_editor* vgui_editor_vector_editor_create(
		vgui_widget* parent, int dimension);
	VGUI_API void vgui_editor_vector_editor_destroy(
		vgui_editor_vector_editor* editor);
	VGUI_API vgui_widget* vgui_editor_vector_editor_widget(
		vgui_editor_vector_editor* editor);
	VGUI_API vgui_widget* vgui_editor_vector_editor_component_widget(
		vgui_editor_vector_editor* editor, int index);
	VGUI_API vgui_widget* vgui_editor_vector_editor_component_edit_widget(
		vgui_editor_vector_editor* editor, int index);
	VGUI_API vgui_widget* vgui_editor_vector_editor_component_label_widget(
		vgui_editor_vector_editor* editor, int index);
	VGUI_API void vgui_editor_vector_editor_set_id(
		vgui_editor_vector_editor* editor, const char* id);
	VGUI_API const char* vgui_editor_vector_editor_id(
		vgui_editor_vector_editor* editor);

	/* False after build() (dimensions are fixed once the widgets exist). */
	VGUI_API int vgui_editor_vector_editor_set_dimension(
		vgui_editor_vector_editor* editor, int dimension);
	VGUI_API int vgui_editor_vector_editor_dimension(
		vgui_editor_vector_editor* editor);

	/* Semantic presets reset labels/colors to their defaults; custom values set
	 * afterwards take precedence.  counts are clamped to the dimension. */
	VGUI_API void vgui_editor_vector_editor_set_semantic(
		vgui_editor_vector_editor* editor, vgui_editor_vector_semantic semantic);
	VGUI_API vgui_editor_vector_semantic vgui_editor_vector_editor_semantic(
		vgui_editor_vector_editor* editor);
	VGUI_API void vgui_editor_vector_editor_set_axis_labels(
		vgui_editor_vector_editor* editor, const char* const* labels, int count);
	VGUI_API void vgui_editor_vector_editor_set_axis_label(
		vgui_editor_vector_editor* editor, int index, const char* label);
	VGUI_API const char* vgui_editor_vector_editor_axis_label(
		vgui_editor_vector_editor* editor, int index);
	VGUI_API void vgui_editor_vector_editor_set_axis_colors(
		vgui_editor_vector_editor* editor, const vgui_color* colors, int count);
	VGUI_API void vgui_editor_vector_editor_set_axis_color(
		vgui_editor_vector_editor* editor, int index, vgui_color color);
	VGUI_API vgui_color vgui_editor_vector_editor_axis_color(
		vgui_editor_vector_editor* editor, int index);

	VGUI_API void vgui_editor_vector_editor_set_value_type(
		vgui_editor_vector_editor* editor, vgui_editor_value_type type);
	VGUI_API vgui_editor_value_type vgui_editor_vector_editor_value_type(
		vgui_editor_vector_editor* editor);
	/* The range is inclusive and applies to every component; setting it clamps
	 * silently. */
	VGUI_API void vgui_editor_vector_editor_set_range(
		vgui_editor_vector_editor* editor, double min_value, double max_value);
	VGUI_API void vgui_editor_vector_editor_clear_range(
		vgui_editor_vector_editor* editor);
	VGUI_API int vgui_editor_vector_editor_has_range(
		vgui_editor_vector_editor* editor);
	VGUI_API double vgui_editor_vector_editor_min_value(
		vgui_editor_vector_editor* editor);
	VGUI_API double vgui_editor_vector_editor_max_value(
		vgui_editor_vector_editor* editor);
	VGUI_API void vgui_editor_vector_editor_set_step(
		vgui_editor_vector_editor* editor, double step);
	VGUI_API double vgui_editor_vector_editor_step(
		vgui_editor_vector_editor* editor);

	/* Silent programmatic updates; extra components beyond dimension() are
	 * ignored. */
	VGUI_API void vgui_editor_vector_editor_set_value(
		vgui_editor_vector_editor* editor, const double* values);
	VGUI_API void vgui_editor_vector_editor_set_component_value(
		vgui_editor_vector_editor* editor, int index, double value);
	/* Reads dimension() entries into values. */
	VGUI_API void vgui_editor_vector_editor_value(vgui_editor_vector_editor* editor,
		double* values);
	VGUI_API const char* vgui_editor_vector_editor_value_text(
		vgui_editor_vector_editor* editor);

	/* User-style edits: reject read-only/disabled controls, validate and clamp,
	 * then notify. */
	VGUI_API int vgui_editor_vector_editor_commit_component_text(
		vgui_editor_vector_editor* editor, int index, const char* text);
	VGUI_API int vgui_editor_vector_editor_commit_component_value(
		vgui_editor_vector_editor* editor, int index, double value);

	VGUI_API void vgui_editor_vector_editor_set_read_only(
		vgui_editor_vector_editor* editor, int read_only);
	VGUI_API int vgui_editor_vector_editor_read_only(
		vgui_editor_vector_editor* editor);
	VGUI_API void vgui_editor_vector_editor_set_enabled(
		vgui_editor_vector_editor* editor, int enabled);
	VGUI_API int vgui_editor_vector_editor_enabled(vgui_editor_vector_editor* editor);

	VGUI_API void vgui_editor_vector_editor_set_component_error(
		vgui_editor_vector_editor* editor, int index, const char* error);
	VGUI_API void vgui_editor_vector_editor_clear_component_errors(
		vgui_editor_vector_editor* editor);
	VGUI_API const char* vgui_editor_vector_editor_component_error(
		vgui_editor_vector_editor* editor, int index);
	VGUI_API void vgui_editor_vector_editor_focus_component(
		vgui_editor_vector_editor* editor, int index);

	VGUI_API void vgui_editor_vector_editor_set_on_change(
		vgui_editor_vector_editor* editor, vgui_editor_vector_fn fn, void* user);
	VGUI_API void vgui_editor_vector_editor_set_on_commit(
		vgui_editor_vector_editor* editor, vgui_editor_vector_fn fn, void* user);
	/* Host-to-editor binding; refresh() reads the source silently. */
	VGUI_API void vgui_editor_vector_editor_set_value_source(
		vgui_editor_vector_editor* editor, vgui_editor_vector_source_fn fn,
		void* user);
	VGUI_API void vgui_editor_vector_editor_refresh(
		vgui_editor_vector_editor* editor);

	/* Canonical text helpers, shared with the property grid.  parse() accepts
	 * spaces, semicolons and surrounding brackets, sets last_error() and returns
	 * 0 when the text is rejected; format() returns the bytes the text needs. */
	VGUI_API int vgui_editor_vector_parse(const char* text, int dimension,
		double* values);
	VGUI_API size_t vgui_editor_vector_format(const double* values, int dimension,
		char* out, size_t cap);
	/* NULL when index/semantic is out of range. */
	VGUI_API const char* vgui_editor_vector_default_axis_label(int index,
		vgui_editor_vector_semantic semantic);
	VGUI_API vgui_color vgui_editor_vector_default_axis_color(int index,
		vgui_editor_vector_semantic semantic);

	/* ------------------------------------------------------------------------- */
	/* Curve editor                                                              */
	/* ------------------------------------------------------------------------- */

	typedef struct vgui_editor_curve_editor vgui_editor_curve_editor;

	typedef enum vgui_editor_curve_interpolation {
		VGUI_EDITOR_CURVE_LINEAR = 0,
		VGUI_EDITOR_CURVE_STEP,
		VGUI_EDITOR_CURVE_CUBIC
	} vgui_editor_curve_interpolation;

	/* t and value are clamped to 0..1; tangents are slopes in value/time units
	 * and only apply to cubic segments. */
	typedef struct vgui_editor_curve_key {
		uint32_t struct_size;
		float t;
		float value;
		float in_tangent;
		float out_tangent;
	} vgui_editor_curve_key;

	VGUI_API void vgui_editor_curve_key_default(vgui_editor_curve_key* key);

	typedef void (*vgui_editor_curve_fn)(void* user,
		vgui_editor_curve_interpolation interpolation,
		const vgui_editor_curve_key* keys, size_t key_count);

	/* keys must hold at least two entries.  User gestures notify through
	 * press/change/commit; programmatic sets are silent. */
	VGUI_API int vgui_editor_curve_parse(const char* text,
		vgui_editor_curve_interpolation* interpolation,
		vgui_editor_curve_key* keys, size_t key_cap, size_t* key_count);
	VGUI_API size_t vgui_editor_curve_format(
		vgui_editor_curve_interpolation interpolation,
		const vgui_editor_curve_key* keys, size_t key_count, char* out, size_t cap);
	VGUI_API const char* vgui_editor_curve_interpolation_name(
		vgui_editor_curve_interpolation mode);
	VGUI_API const char* vgui_editor_curve_interpolation_label(
		vgui_editor_curve_interpolation mode);

	VGUI_API vgui_editor_curve_editor* vgui_editor_curve_editor_create(
		vgui_widget* parent);
	/* Builds the detached editor that vgui_popup_open() expects. */
	VGUI_API vgui_editor_curve_editor* vgui_editor_curve_editor_create_popup(
		vgui_context* ui);
	VGUI_API void vgui_editor_curve_editor_destroy(
		vgui_editor_curve_editor* editor);
	VGUI_API vgui_widget* vgui_editor_curve_editor_widget(
		vgui_editor_curve_editor* editor);
	VGUI_API vgui_widget* vgui_editor_curve_editor_graph_widget(
		vgui_editor_curve_editor* editor);
	VGUI_API vgui_widget* vgui_editor_curve_editor_mode_widget(
		vgui_editor_curve_editor* editor, vgui_editor_curve_interpolation mode);
	VGUI_API vgui_widget* vgui_editor_curve_editor_add_widget(
		vgui_editor_curve_editor* editor);
	VGUI_API vgui_widget* vgui_editor_curve_editor_remove_widget(
		vgui_editor_curve_editor* editor);
	VGUI_API vgui_widget* vgui_editor_curve_editor_time_widget(
		vgui_editor_curve_editor* editor);
	VGUI_API vgui_widget* vgui_editor_curve_editor_value_widget(
		vgui_editor_curve_editor* editor);
	VGUI_API vgui_widget* vgui_editor_curve_editor_in_tangent_widget(
		vgui_editor_curve_editor* editor);
	VGUI_API vgui_widget* vgui_editor_curve_editor_out_tangent_widget(
		vgui_editor_curve_editor* editor);
	VGUI_API void vgui_editor_curve_editor_set_id(vgui_editor_curve_editor* editor,
		const char* id);
	VGUI_API const char* vgui_editor_curve_editor_id(vgui_editor_curve_editor* editor);

	VGUI_API int vgui_editor_curve_editor_set_value(vgui_editor_curve_editor* editor,
		vgui_editor_curve_interpolation interpolation,
		const vgui_editor_curve_key* keys, size_t key_count);
	/* Invalid text leaves the editor unchanged, returns 0 and sets
	 * last_error(). */
	VGUI_API int vgui_editor_curve_editor_set_value_text(
		vgui_editor_curve_editor* editor, const char* text);
	VGUI_API const char* vgui_editor_curve_editor_value_text(
		vgui_editor_curve_editor* editor);
	/* Copies min(cap, key_count()) keys and returns the total count. */
	VGUI_API size_t vgui_editor_curve_editor_keys(vgui_editor_curve_editor* editor,
		vgui_editor_curve_key* out, size_t cap);
	VGUI_API size_t vgui_editor_curve_editor_key_count(
		vgui_editor_curve_editor* editor);

	VGUI_API void vgui_editor_curve_editor_set_read_only(
		vgui_editor_curve_editor* editor, int read_only);
	VGUI_API int vgui_editor_curve_editor_read_only(
		vgui_editor_curve_editor* editor);
	VGUI_API void vgui_editor_curve_editor_set_enabled(
		vgui_editor_curve_editor* editor, int enabled);
	VGUI_API int vgui_editor_curve_editor_enabled(vgui_editor_curve_editor* editor);

	/* Selection indexes into the sorted key list; -1 when nothing is selected. */
	VGUI_API int vgui_editor_curve_editor_select_key(
		vgui_editor_curve_editor* editor, int index);
	VGUI_API int vgui_editor_curve_editor_selected_key(
		vgui_editor_curve_editor* editor);
	/* User-style operations: they notify and commit once. */
	VGUI_API int vgui_editor_curve_editor_add_key(vgui_editor_curve_editor* editor,
		float t, float value);
	VGUI_API int vgui_editor_curve_editor_remove_selected_key(
		vgui_editor_curve_editor* editor);
	VGUI_API int vgui_editor_curve_editor_commit_interpolation(
		vgui_editor_curve_editor* editor, vgui_editor_curve_interpolation mode);
	VGUI_API vgui_editor_curve_interpolation vgui_editor_curve_editor_interpolation(
		vgui_editor_curve_editor* editor);

	VGUI_API void vgui_editor_curve_editor_set_on_press(
		vgui_editor_curve_editor* editor, vgui_editor_curve_fn fn, void* user);
	VGUI_API void vgui_editor_curve_editor_set_on_change(
		vgui_editor_curve_editor* editor, vgui_editor_curve_fn fn, void* user);
	VGUI_API void vgui_editor_curve_editor_set_on_commit(
		vgui_editor_curve_editor* editor, vgui_editor_curve_fn fn, void* user);

	/* ------------------------------------------------------------------------- */
	/* Gradient editor                                                           */
	/* ------------------------------------------------------------------------- */

	typedef struct vgui_editor_gradient_editor vgui_editor_gradient_editor;

	/* color is canonical "#RRGGBB" or "#RRGGBBAA"; the 16-byte field always fits
	 * the full text (9 characters + NUL). */
	typedef struct vgui_editor_gradient_stop {
		uint32_t struct_size;
		float t;
		char color[16];
	} vgui_editor_gradient_stop;

	VGUI_API void vgui_editor_gradient_stop_default(vgui_editor_gradient_stop* stop);

	typedef void (*vgui_editor_gradient_fn)(void* user,
		const vgui_editor_gradient_stop* stops, size_t stop_count);

	/* stops must hold at least two entries. */
	VGUI_API int vgui_editor_gradient_parse(const char* text,
		vgui_editor_gradient_stop* stops, size_t stop_cap, size_t* stop_count);
	VGUI_API size_t vgui_editor_gradient_format(
		const vgui_editor_gradient_stop* stops, size_t stop_count, char* out,
		size_t cap);
	/* Preset names exposed by the editor's own preset row. */
	VGUI_API size_t vgui_editor_gradient_preset_count(void);
	VGUI_API const char* vgui_editor_gradient_preset_name(size_t index);

	VGUI_API vgui_editor_gradient_editor* vgui_editor_gradient_editor_create(
		vgui_widget* parent);
	VGUI_API vgui_editor_gradient_editor* vgui_editor_gradient_editor_create_popup(
		vgui_context* ui);
	VGUI_API void vgui_editor_gradient_editor_destroy(
		vgui_editor_gradient_editor* editor);
	VGUI_API vgui_widget* vgui_editor_gradient_editor_widget(
		vgui_editor_gradient_editor* editor);
	VGUI_API vgui_widget* vgui_editor_gradient_editor_bar_widget(
		vgui_editor_gradient_editor* editor);
	VGUI_API vgui_widget* vgui_editor_gradient_editor_add_widget(
		vgui_editor_gradient_editor* editor);
	VGUI_API vgui_widget* vgui_editor_gradient_editor_remove_widget(
		vgui_editor_gradient_editor* editor);
	VGUI_API vgui_widget* vgui_editor_gradient_editor_time_widget(
		vgui_editor_gradient_editor* editor);
	VGUI_API vgui_widget* vgui_editor_gradient_editor_preset_widget(
		vgui_editor_gradient_editor* editor, size_t index);
	/* The embedded color picker's root widget (the picker itself is owned by the
	 * gradient editor). */
	VGUI_API vgui_widget* vgui_editor_gradient_editor_picker_widget(
		vgui_editor_gradient_editor* editor);
	VGUI_API void vgui_editor_gradient_editor_set_id(
		vgui_editor_gradient_editor* editor, const char* id);
	VGUI_API const char* vgui_editor_gradient_editor_id(
		vgui_editor_gradient_editor* editor);

	VGUI_API int vgui_editor_gradient_editor_set_value(
		vgui_editor_gradient_editor* editor, const vgui_editor_gradient_stop* stops,
		size_t stop_count);
	VGUI_API int vgui_editor_gradient_editor_set_value_text(
		vgui_editor_gradient_editor* editor, const char* text);
	VGUI_API const char* vgui_editor_gradient_editor_value_text(
		vgui_editor_gradient_editor* editor);
	VGUI_API size_t vgui_editor_gradient_editor_stops(
		vgui_editor_gradient_editor* editor, vgui_editor_gradient_stop* out,
		size_t cap);
	VGUI_API size_t vgui_editor_gradient_editor_stop_count(
		vgui_editor_gradient_editor* editor);

	VGUI_API void vgui_editor_gradient_editor_set_read_only(
		vgui_editor_gradient_editor* editor, int read_only);
	VGUI_API int vgui_editor_gradient_editor_read_only(
		vgui_editor_gradient_editor* editor);
	VGUI_API void vgui_editor_gradient_editor_set_enabled(
		vgui_editor_gradient_editor* editor, int enabled);
	VGUI_API int vgui_editor_gradient_editor_enabled(
		vgui_editor_gradient_editor* editor);

	VGUI_API int vgui_editor_gradient_editor_select_stop(
		vgui_editor_gradient_editor* editor, int index);
	VGUI_API int vgui_editor_gradient_editor_selected_stop(
		vgui_editor_gradient_editor* editor);
	/* add_stop() samples the interpolated color at t. */
	VGUI_API int vgui_editor_gradient_editor_add_stop(
		vgui_editor_gradient_editor* editor, float t);
	VGUI_API int vgui_editor_gradient_editor_remove_selected_stop(
		vgui_editor_gradient_editor* editor);
	VGUI_API int vgui_editor_gradient_editor_set_selected_color(
		vgui_editor_gradient_editor* editor, const char* hex);
	/* Whole-gradient commit, as if a preset button was pressed. */
	VGUI_API int vgui_editor_gradient_editor_commit_preset(
		vgui_editor_gradient_editor* editor, size_t index);

	VGUI_API void vgui_editor_gradient_editor_set_on_press(
		vgui_editor_gradient_editor* editor, vgui_editor_gradient_fn fn, void* user);
	VGUI_API void vgui_editor_gradient_editor_set_on_change(
		vgui_editor_gradient_editor* editor, vgui_editor_gradient_fn fn, void* user);
	VGUI_API void vgui_editor_gradient_editor_set_on_commit(
		vgui_editor_gradient_editor* editor, vgui_editor_gradient_fn fn, void* user);

	/* ------------------------------------------------------------------------- */
	/* Property grid                                                             */
	/* ------------------------------------------------------------------------- */

	typedef struct vgui_editor_property_grid vgui_editor_property_grid;

	typedef enum vgui_editor_property_type {
		VGUI_EDITOR_PROPERTY_TEXT = 0,
		VGUI_EDITOR_PROPERTY_INTEGER,
		VGUI_EDITOR_PROPERTY_NUMBER,
		VGUI_EDITOR_PROPERTY_BOOLEAN,
		VGUI_EDITOR_PROPERTY_ENUMERATION,
		VGUI_EDITOR_PROPERTY_COLOR,
		VGUI_EDITOR_PROPERTY_VECTOR2,
		VGUI_EDITOR_PROPERTY_VECTOR3,
		VGUI_EDITOR_PROPERTY_VECTOR4,
		/* Reference rows: the value is the referenced id ("" for none) and the
		 * host resolves names/thumbnails through the query handlers. */
		VGUI_EDITOR_PROPERTY_ASSET,
		VGUI_EDITOR_PROPERTY_OBJECT_REF,
		/* Custom editors: values are the canonical curve/gradient strings; rows
		 * show a preview and open the full editor in a popup. */
		VGUI_EDITOR_PROPERTY_CURVE,
		VGUI_EDITOR_PROPERTY_GRADIENT
	} vgui_editor_property_type;

	/* One host-owned value binding.  The getters use the string-callback
	 * convention (bytes needed, or VGUI_EDITOR_NO_VALUE when unavailable). */
	typedef size_t (*vgui_editor_value_get_fn)(void* user, char* out, size_t cap);
	typedef void (*vgui_editor_value_set_fn)(void* user, const char* value);

	typedef struct vgui_editor_property_binding {
		uint32_t struct_size;
		vgui_editor_value_get_fn get_value;
		vgui_editor_value_set_fn set_value;
		void* user;
		/* Runs once, when the grid drops the binding (row removed or grid
		 * destroyed). */
		vgui_editor_release_fn release_user;
	} vgui_editor_property_binding;

	VGUI_API void vgui_editor_property_binding_default(
		vgui_editor_property_binding* binding);

	/* Describes one row.  Every string is copied at add time, so a host may
	 * reuse its buffers right after the call; NULL means the empty string
	 * except for `category`, where NULL selects "General".  `value` and
	 * `default_value` use the canonical text form of the type - see the
	 * per-type helpers above. */
	typedef struct vgui_editor_property_desc {
		uint32_t struct_size;
		const char* id;
		const char* label;
		vgui_editor_property_type type;
		const char* value;
		const char* category;
		const char* tooltip;
		const char* placeholder;
		const char* default_value;
		int has_default;
		int read_only;
		/* Inclusive numeric range; also applied to every vector component. */
		int has_range;
		double min_value;
		double max_value;
		double step;
		/* Display labels; when choice_values is NULL the labels double as
		 * values.  choice_enabled optionally holds one flag byte per choice
		 * (0 disables that entry). */
		const char* const* choices;
		size_t choice_count;
		const char* const* choice_values;
		size_t choice_value_count;
		const unsigned char* choice_enabled;
		/* Enumeration only: allows the empty value, shown as placeholder (or
		 * "(None)") and offered first. */
		int allow_empty;
		/* Vector rows: semantic selects axis labels/colors; the explicit arrays
		 * override them. */
		vgui_editor_vector_semantic semantic;
		const char* const* component_labels;
		size_t component_label_count;
		const vgui_color* component_colors;
		size_t component_color_count;
		/* asset/object_ref rows: host-interpreted filter tag ("mesh", ".png"),
		 * passed through to the query and drop-check handlers. */
		const char* asset_filter;
		/* Optional per-property binding; NULL uses the grid-level source.  A
		 * bound row with no `value` reads its starting value from
		 * `binding.get_value`, because the grid validates `value` at add time. */
		const vgui_editor_property_binding* binding;
	} vgui_editor_property_desc;

	VGUI_API void vgui_editor_property_desc_default(vgui_editor_property_desc* desc);

	/* Resolves a reference for display.  Fill `out` and return non-zero; return
	 * 0 when the id is not resolvable (the row then shows the missing state).
	 * `name` and `kind` are borrowed and copied by the grid before it returns. */
	typedef struct vgui_editor_asset_info {
		uint32_t struct_size;
		const char* name;
		const char* kind;
		vgui_texture* thumbnail;
		int missing;
	} vgui_editor_asset_info;

	VGUI_API void vgui_editor_asset_info_default(vgui_editor_asset_info* info);

	typedef int (*vgui_editor_asset_query_fn)(void* user, const char* id,
		const char* filter, vgui_editor_asset_info* out);
	typedef void (*vgui_editor_asset_browse_fn)(void* user, const char* id,
		const char* filter);
	typedef void (*vgui_editor_object_activate_fn)(void* user,
		vgui_editor_object_id id);
	typedef int (*vgui_editor_asset_filter_fn)(void* user, const char* id,
		const char* filter);
	typedef void (*vgui_editor_missing_asset_fn)(void* user, const char* id,
		const char* filter);
	typedef int (*vgui_editor_asset_drop_fn)(void* user, const char* asset);
	typedef void (*vgui_editor_property_change_fn)(void* user, const char* id,
		const char* value);
	/* Grid-level value pull, using the string-callback convention; return
	 * VGUI_EDITOR_NO_VALUE to leave the row untouched. */
	typedef size_t (*vgui_editor_property_source_fn)(void* user, const char* id,
		char* out, size_t cap);
	typedef size_t (*vgui_editor_multi_get_fn)(void* user, const char* id,
		vgui_editor_object_id target, char* out, size_t cap);
	typedef void (*vgui_editor_multi_set_fn)(void* user, const char* id,
		vgui_editor_object_id target, const char* value);
	/* Rebuilds the grid for the new object id (call clear() and add rows). */
	typedef void (*vgui_editor_property_bind_fn)(void* user,
		vgui_editor_property_grid* grid, vgui_editor_object_id id);

	VGUI_API vgui_editor_property_grid* vgui_editor_property_grid_create(
		vgui_widget* parent);
	VGUI_API void vgui_editor_property_grid_destroy(vgui_editor_property_grid* grid);
	VGUI_API vgui_widget* vgui_editor_property_grid_widget(
		vgui_editor_property_grid* grid);
	VGUI_API vgui_widget* vgui_editor_property_grid_editor(
		vgui_editor_property_grid* grid, const char* id);

	/* Removes all rows but keeps category collapse state and the filter. */
	VGUI_API void vgui_editor_property_grid_clear(vgui_editor_property_grid* grid);
	VGUI_API int vgui_editor_property_grid_add_property(
		vgui_editor_property_grid* grid, const vgui_editor_property_desc* desc);
	/* Applies a complete row set in one pass, refreshing rows whose id and type
	 * are unchanged in place.  Returns the number of rows added, removed or
	 * rebuilt (0 means a pure refresh); invalid and duplicate descriptors are
	 * skipped. */
	VGUI_API size_t vgui_editor_property_grid_set_properties(
		vgui_editor_property_grid* grid, const vgui_editor_property_desc* descs,
		size_t count);
	VGUI_API int vgui_editor_property_grid_remove_property(
		vgui_editor_property_grid* grid, const char* id);
	VGUI_API int vgui_editor_property_grid_has_property(
		vgui_editor_property_grid* grid, const char* id);

	/* Compatibility rows: add_row() makes a text property with an
	 * auto-generated id in "General". */
	VGUI_API void vgui_editor_property_grid_add_row(vgui_editor_property_grid* grid,
		const char* label, const char* value);
	VGUI_API int vgui_editor_property_grid_set_row_value(
		vgui_editor_property_grid* grid, size_t index, const char* value);
	VGUI_API size_t vgui_editor_property_grid_row_count(
		vgui_editor_property_grid* grid);
	/* Borrowed from one shared per-grid buffer: valid until the next string
	 * getter or mutation of this grid.  NULL when the index is out of range. */
	VGUI_API const char* vgui_editor_property_grid_row_value(
		vgui_editor_property_grid* grid, size_t index);
	VGUI_API const char* vgui_editor_property_grid_property_value(
		vgui_editor_property_grid* grid, const char* id);

	/* Programmatic sync: validates and updates the row without notifying or
	 * recording a command.  commit_property_value() is the user-style form: it
	 * notifies and, with a stack attached, records one undo step.  Both reject
	 * a value the type cannot represent, while a numeric value outside the
	 * declared range is clamped instead of refused.  Consecutive commits to the
	 * same row merge into one undo record, as they do for a dragged field. */
	VGUI_API int vgui_editor_property_grid_set_property_value(
		vgui_editor_property_grid* grid, const char* id, const char* value);
	VGUI_API int vgui_editor_property_grid_commit_property_value(
		vgui_editor_property_grid* grid, const char* id, const char* value);
	VGUI_API int vgui_editor_property_grid_reset_property(
		vgui_editor_property_grid* grid, const char* id);
	VGUI_API void vgui_editor_property_grid_reset_all(vgui_editor_property_grid* grid);

	/* Borrowed validation message ("" when the row is clean). */
	VGUI_API const char* vgui_editor_property_grid_property_error(
		vgui_editor_property_grid* grid, const char* id);
	VGUI_API int vgui_editor_property_grid_set_property_error(
		vgui_editor_property_grid* grid, const char* id, const char* error);

	VGUI_API int vgui_editor_property_grid_property_read_only(
		vgui_editor_property_grid* grid, const char* id);
	VGUI_API int vgui_editor_property_grid_set_property_read_only(
		vgui_editor_property_grid* grid, const char* id, int read_only);
	VGUI_API void vgui_editor_property_grid_set_read_only(
		vgui_editor_property_grid* grid, int read_only);
	VGUI_API int vgui_editor_property_grid_read_only(
		vgui_editor_property_grid* grid);

	VGUI_API void vgui_editor_property_grid_set_filter(
		vgui_editor_property_grid* grid, const char* text);
	VGUI_API const char* vgui_editor_property_grid_filter(
		vgui_editor_property_grid* grid);
	VGUI_API size_t vgui_editor_property_grid_visible_row_count(
		vgui_editor_property_grid* grid);
	VGUI_API int vgui_editor_property_grid_property_visible(
		vgui_editor_property_grid* grid, const char* id);
	/* Rows whose editor widgets exist right now (bounded by the visible
	 * window). */
	VGUI_API size_t vgui_editor_property_grid_materialized_row_count(
		vgui_editor_property_grid* grid);

	VGUI_API int vgui_editor_property_grid_set_category_collapsed(
		vgui_editor_property_grid* grid, const char* category, int collapsed);
	VGUI_API int vgui_editor_property_grid_category_collapsed(
		vgui_editor_property_grid* grid, const char* category);

	/* Owned by whoever created the stack (usually the editor context). */
	VGUI_API void vgui_editor_property_grid_set_command_stack(
		vgui_editor_property_grid* grid, vgui_editor_command_stack* stack);

	/* F03/A04: grid-wide asset drop.  Returns non-zero when the host assigned
	 * the asset. */
	VGUI_API void vgui_editor_property_grid_set_asset_drop_handler(
		vgui_editor_property_grid* grid, vgui_editor_asset_drop_fn fn, void* user);
	VGUI_API void vgui_editor_property_grid_set_asset_query(
		vgui_editor_property_grid* grid, vgui_editor_asset_query_fn fn, void* user);
	VGUI_API void vgui_editor_property_grid_set_asset_browse_handler(
		vgui_editor_property_grid* grid, vgui_editor_asset_browse_fn fn, void* user);
	VGUI_API void vgui_editor_property_grid_set_object_activated_handler(
		vgui_editor_property_grid* grid, vgui_editor_object_activate_fn fn,
		void* user);
	VGUI_API void vgui_editor_property_grid_set_asset_filter_check(
		vgui_editor_property_grid* grid, vgui_editor_asset_filter_fn fn, void* user);
	VGUI_API void vgui_editor_property_grid_set_missing_asset_handler(
		vgui_editor_property_grid* grid, vgui_editor_missing_asset_fn fn,
		void* user);

	/* P07: wraps commits in one undoable transaction.  Nested batches collapse
	 * into the outer one; each commit still applies immediately.  Both return 0
	 * when no stack is attached or the batching is unbalanced. */
	VGUI_API int vgui_editor_property_grid_begin_batch(
		vgui_editor_property_grid* grid, const char* name);
	VGUI_API int vgui_editor_property_grid_end_batch(
		vgui_editor_property_grid* grid);
	VGUI_API size_t vgui_editor_property_grid_batch_depth(
		vgui_editor_property_grid* grid);

	/* P08: multi-target editing.  With a non-empty list the grid pulls
	 * per-object values through the getter, shows differing values as mixed and
	 * applies commits to every target in one transaction (desc bindings are not
	 * used).  An empty list restores single-object mode. */
	VGUI_API void vgui_editor_property_grid_set_multi_targets(
		vgui_editor_property_grid* grid, const vgui_editor_object_id* targets,
		size_t count);
	VGUI_API size_t vgui_editor_property_grid_multi_targets(
		vgui_editor_property_grid* grid, vgui_editor_object_id* out, size_t cap);
	VGUI_API void vgui_editor_property_grid_set_multi_getter(
		vgui_editor_property_grid* grid, vgui_editor_multi_get_fn fn, void* user);
	VGUI_API void vgui_editor_property_grid_set_multi_setter(
		vgui_editor_property_grid* grid, vgui_editor_multi_set_fn fn, void* user);
	VGUI_API int vgui_editor_property_grid_property_mixed(
		vgui_editor_property_grid* grid, const char* id);

	/* P08: property clipboard, in-process and shared across grids.  paste
	 * matches rows by id AND type and returns the pasted count. */
	VGUI_API size_t vgui_editor_property_grid_copy_properties(
		vgui_editor_property_grid* grid);
	VGUI_API size_t vgui_editor_property_grid_paste_properties(
		vgui_editor_property_grid* grid);
	VGUI_API size_t vgui_editor_property_grid_clipboard_count(void);

	/* P08: per-row favorites; favorites_only narrows the view (on top of the
	 * filter and category collapse). */
	VGUI_API void vgui_editor_property_grid_set_property_favorite(
		vgui_editor_property_grid* grid, const char* id, int favorite);
	VGUI_API int vgui_editor_property_grid_property_favorite(
		vgui_editor_property_grid* grid, const char* id);
	VGUI_API size_t vgui_editor_property_grid_favorite_count(
		vgui_editor_property_grid* grid);
	VGUI_API const char* vgui_editor_property_grid_favorite_id_at(
		vgui_editor_property_grid* grid, size_t index);
	VGUI_API void vgui_editor_property_grid_set_favorites_only(
		vgui_editor_property_grid* grid, int only);

	VGUI_API void vgui_editor_property_grid_set_change_listener(
		vgui_editor_property_grid* grid, vgui_editor_property_change_fn fn,
		void* user);
	VGUI_API void vgui_editor_property_grid_set_value_source(
		vgui_editor_property_grid* grid, vgui_editor_property_source_fn fn,
		void* user);
	VGUI_API void vgui_editor_property_grid_refresh(vgui_editor_property_grid* grid);

	/* Rebinds on every editor-context selection change; the initial selection is
	 * delivered immediately.  Call unbind before the context is destroyed if
	 * the grid outlives it. */
	VGUI_API void vgui_editor_property_grid_bind_selection(
		vgui_editor_property_grid* grid, vgui_editor_context* context,
		vgui_editor_property_bind_fn fn, void* user);
	VGUI_API void vgui_editor_property_grid_unbind_selection(
		vgui_editor_property_grid* grid);

	/* Validates and canonicalizes text for a descriptor - the same normalization
	 * the grid and table cells apply.  Writes the canonical value and returns
	 * the bytes it needs; returns VGUI_EDITOR_NO_VALUE and sets last_error()
	 * when rejected. */
	VGUI_API size_t vgui_editor_property_normalize_value(
		const vgui_editor_property_desc* desc, const char* text, char* out,
		size_t cap);

	/* ------------------------------------------------------------------------- */
	/* Console (log view)                                                        */
	/* ------------------------------------------------------------------------- */

	typedef struct vgui_editor_console vgui_editor_console;

	typedef enum vgui_editor_log_level {
		VGUI_EDITOR_LOG_TRACE = 0,
		VGUI_EDITOR_LOG_DEBUG,
		VGUI_EDITOR_LOG_INFO,
		VGUI_EDITOR_LOG_WARNING,
		VGUI_EDITOR_LOG_ERROR
	} vgui_editor_log_level;

#define VGUI_EDITOR_LOG_LEVEL_COUNT 5

	/* Jump target: asset_id and path are alternatives; line/column mainly make
	 * sense for scripts. */
	typedef struct vgui_editor_log_location {
		uint32_t struct_size;
		vgui_editor_object_id asset_id;
		const char* path;
		int line;
		int column;
	} vgui_editor_log_location;

	typedef struct vgui_editor_log_frame {
		uint32_t struct_size;
		const char* function;
		vgui_editor_log_location where;
	} vgui_editor_log_frame;

	/* One log record.  On input every string is copied immediately and NULL is
	 * the empty string; timestamp NULL lets the console keep its own stamp.  On
	 * output (entry_at) the strings are borrowed from the console handle and
	 * stay valid until the next mutation of that handle. */
	typedef struct vgui_editor_log_entry {
		uint32_t struct_size;
		vgui_editor_log_level level;
		const char* message;
		const char* source;
		const char* timestamp;
		vgui_editor_log_location where;
		const vgui_editor_log_frame* frames;
		size_t frame_count;
	} vgui_editor_log_entry;

	VGUI_API void vgui_editor_log_location_default(vgui_editor_log_location* where);
	VGUI_API void vgui_editor_log_frame_default(vgui_editor_log_frame* frame);
	VGUI_API void vgui_editor_log_entry_default(vgui_editor_log_entry* entry);

	VGUI_API const char* vgui_editor_log_level_name(vgui_editor_log_level level);

	/* Producer callbacks run on any thread; append() only queues. */
	typedef int (*vgui_editor_jump_fn)(void* user,
		const vgui_editor_log_location* where);

	VGUI_API vgui_editor_console* vgui_editor_console_create(vgui_widget* parent);
	VGUI_API void vgui_editor_console_destroy(vgui_editor_console* console);
	VGUI_API vgui_widget* vgui_editor_console_widget(vgui_editor_console* console);
	VGUI_API vgui_widget* vgui_editor_console_tree(vgui_editor_console* console);
	VGUI_API vgui_widget* vgui_editor_console_ticker(vgui_editor_console* console);
	VGUI_API vgui_widget* vgui_editor_console_search_widget(
		vgui_editor_console* console);
	VGUI_API vgui_widget* vgui_editor_console_auto_scroll_widget(
		vgui_editor_console* console);
	VGUI_API vgui_widget* vgui_editor_console_level_widget(
		vgui_editor_console* console, vgui_editor_log_level level);
	VGUI_API vgui_widget* vgui_editor_console_copy_widget(
		vgui_editor_console* console);
	VGUI_API vgui_widget* vgui_editor_console_clear_widget(
		vgui_editor_console* console);

	/* Returns the stable entry id, or VGUI_EDITOR_INVALID_OBJECT when the record
	 * could not be queued.  The simple form is the common log call. */
	VGUI_API vgui_editor_object_id vgui_editor_console_append(
		vgui_editor_console* console, const vgui_editor_log_entry* entry);
	VGUI_API vgui_editor_object_id vgui_editor_console_append_message(
		vgui_editor_console* console, vgui_editor_log_level level,
		const char* message, const char* source);
	/* UI thread: drains up to drain_batch() queued records per call. */
	VGUI_API size_t vgui_editor_console_poll(vgui_editor_console* console);
	VGUI_API void vgui_editor_console_tick(vgui_editor_console* console,
		float delta_time);
	VGUI_API int vgui_editor_console_idle(vgui_editor_console* console);
	VGUI_API size_t vgui_editor_console_pending_count(vgui_editor_console* console);
	VGUI_API size_t vgui_editor_console_entry_count(vgui_editor_console* console);
	VGUI_API size_t vgui_editor_console_visible_entry_count(
		vgui_editor_console* console);
	VGUI_API size_t vgui_editor_console_visible_row_count(
		vgui_editor_console* console);
	/* Records dropped because max_entries() was reached. */
	VGUI_API size_t vgui_editor_console_dropped_count(vgui_editor_console* console);
	VGUI_API vgui_editor_object_id vgui_editor_console_last_entry_id(
		vgui_editor_console* console);
	/* Copies the retained record; strings are borrowed from the handle. */
	VGUI_API int vgui_editor_console_entry_at(vgui_editor_console* console,
		vgui_editor_object_id entry_id, vgui_editor_log_entry* out);

	VGUI_API void vgui_editor_console_clear(vgui_editor_console* console);
	VGUI_API void vgui_editor_console_set_max_entries(vgui_editor_console* console,
		size_t max_entries);
	VGUI_API size_t vgui_editor_console_max_entries(vgui_editor_console* console);
	VGUI_API void vgui_editor_console_set_drain_batch(vgui_editor_console* console,
		size_t count);
	VGUI_API size_t vgui_editor_console_drain_batch(vgui_editor_console* console);

	/* Level and text filters are independent; filter_active() reports whether
	 * anything is filtered out. */
	VGUI_API void vgui_editor_console_set_level_enabled(
		vgui_editor_console* console, vgui_editor_log_level level, int enabled);
	VGUI_API int vgui_editor_console_level_enabled(vgui_editor_console* console,
		vgui_editor_log_level level);
	VGUI_API void vgui_editor_console_set_search_text(vgui_editor_console* console,
		const char* text);
	VGUI_API const char* vgui_editor_console_search_text(
		vgui_editor_console* console);
	VGUI_API int vgui_editor_console_filter_active(vgui_editor_console* console);

	VGUI_API void vgui_editor_console_set_auto_scroll(vgui_editor_console* console,
		int enabled);
	VGUI_API int vgui_editor_console_auto_scroll(vgui_editor_console* console);
	VGUI_API void vgui_editor_console_scroll_to_latest(
		vgui_editor_console* console);

	/* Stack traces stay collapsed per entry until expanded. */
	VGUI_API int vgui_editor_console_stack_expanded(vgui_editor_console* console,
		vgui_editor_object_id entry_id);
	VGUI_API int vgui_editor_console_set_stack_expanded(vgui_editor_console* console,
		vgui_editor_object_id entry_id, int expanded);
	VGUI_API int vgui_editor_console_toggle_stack(vgui_editor_console* console,
		vgui_editor_object_id entry_id);

	/* Copy support.  Both writers use the string-callback convention; when the
	 * first pass truncates, the console retries with an exact-size buffer.
	 * copy_selection() puts the selected rows (or all visible rows) on the
	 * system clipboard. */
	VGUI_API size_t vgui_editor_console_selected_text(vgui_editor_console* console,
		char* out, size_t cap);
	VGUI_API size_t vgui_editor_console_visible_text(vgui_editor_console* console,
		char* out, size_t cap);
	VGUI_API int vgui_editor_console_copy_selection(vgui_editor_console* console);

	/* Double-click / Enter on a row with a location: invokes the jump handler. */
	VGUI_API int vgui_editor_console_activate_row(vgui_editor_console* console,
		vgui_editor_object_id row_id);
	VGUI_API int vgui_editor_console_activate_selected(
		vgui_editor_console* console);
	VGUI_API void vgui_editor_console_set_jump_handler(vgui_editor_console* console,
		vgui_editor_jump_fn fn, void* user);

	/* Per-level text colours; unconfigured levels follow the theme. */
	VGUI_API void vgui_editor_console_set_level_color(vgui_editor_console* console,
		vgui_editor_log_level level, vgui_color color);
	VGUI_API vgui_color vgui_editor_console_level_color(vgui_editor_console* console,
		vgui_editor_log_level level);

	/* Visible-row index helpers.  Row ids stay stable while their entry is
	 * retained, including across filter changes; frame is -1 for the log row
	 * itself and the stack index for an expanded frame row. */
	VGUI_API vgui_editor_object_id vgui_editor_console_row_id_at(
		vgui_editor_console* console, int visible_index);
	VGUI_API int vgui_editor_console_row_entry(vgui_editor_console* console,
		vgui_editor_object_id row_id, vgui_editor_object_id* entry_id, int* frame);

	/* ------------------------------------------------------------------------- */
	/* Outliner                                                                  */
	/* ------------------------------------------------------------------------- */

	/* Scene tree.  Rows are addressed two ways: by vgui_item_id (the tree item,
	 * returned by add_node() and read back from the tree widget) and by
	 * vgui_editor_object_id (the host scene object).  The widget() handle is a
	 * VGUI_TREE, so the vgui_tree_* C functions work on it directly. */
	typedef struct vgui_editor_outliner vgui_editor_outliner;

	/* Context-menu / shortcut entries routed through the host handler.  Rename
	 * and the two flag toggles are applied by the view itself. */
	typedef enum vgui_editor_outliner_action {
		VGUI_EDITOR_OUTLINER_NEW_CHILD = 0,
		VGUI_EDITOR_OUTLINER_DUPLICATE,
		VGUI_EDITOR_OUTLINER_RENAME,
		VGUI_EDITOR_OUTLINER_REMOVE,
		VGUI_EDITOR_OUTLINER_FOCUS,
		VGUI_EDITOR_OUTLINER_TOGGLE_VISIBILITY,
		VGUI_EDITOR_OUTLINER_TOGGLE_LOCK
	} vgui_editor_outliner_action;

	/* Where a drop lands relative to the row under the pointer; roots means the
	 * empty area below every row. */
	typedef enum vgui_editor_drop_kind {
		VGUI_EDITOR_DROP_NONE = 0,
		VGUI_EDITOR_DROP_BEFORE,
		VGUI_EDITOR_DROP_AFTER,
		VGUI_EDITOR_DROP_ONTO,
		VGUI_EDITOR_DROP_ROOTS
	} vgui_editor_drop_kind;

	typedef struct vgui_editor_drop_location {
		uint32_t struct_size;
		vgui_editor_drop_kind kind;
		vgui_editor_object_id parent;
		size_t index;
		vgui_item_id row;
	} vgui_editor_drop_location;

	VGUI_API void vgui_editor_drop_location_default(
		vgui_editor_drop_location* location);

	/* Callbacks.  Move and rename return 0 to reject, which also skips the undo
	 * record.  name/tooltip return borrowed text that the view copies before it
	 * returns. */
	typedef void (*vgui_editor_object_fn)(void* user, vgui_editor_object_id id);
	typedef int (*vgui_editor_move_fn)(void* user, vgui_editor_object_id node,
		vgui_editor_object_id parent, size_t index);
	typedef int (*vgui_editor_rename_fn)(void* user, vgui_editor_object_id node,
		const char* name);
	typedef void (*vgui_editor_flag_fn)(void* user, vgui_editor_object_id node,
		int state);
	typedef int (*vgui_editor_filter_match_fn)(void* user,
		vgui_editor_object_id node, const char* name);
	typedef int (*vgui_editor_outliner_action_fn)(void* user,
		vgui_editor_outliner_action action, const vgui_editor_object_id* nodes,
		size_t count);
	typedef int (*vgui_editor_asset_drop_node_fn)(void* user, const char* asset,
		vgui_editor_object_id parent, size_t index);

	/* Virtual-mode row source (O05).  root_count/root_at/child_count/child_at/
	 * name are required; the rest fall back to neutral defaults. */
	typedef struct vgui_editor_scene_source {
		uint32_t struct_size;
		size_t (*root_count)(void* user);
		vgui_editor_object_id (*root_at)(void* user, size_t index);
		size_t (*child_count)(void* user, vgui_editor_object_id node);
		vgui_editor_object_id (*child_at)(void* user, vgui_editor_object_id parent,
			size_t index);
		const char* (*name)(void* user, vgui_editor_object_id node);
		vgui_editor_object_id (*parent_of)(void* user, vgui_editor_object_id node);
		vgui_texture* (*icon)(void* user, vgui_editor_object_id node);
		int (*enabled)(void* user, vgui_editor_object_id node);
		const char* (*tooltip)(void* user, vgui_editor_object_id node);
		vgui_color (*text_color)(void* user, vgui_editor_object_id node);
		void* user;
		vgui_editor_release_fn release_user;
	} vgui_editor_scene_source;

	VGUI_API void vgui_editor_scene_source_default(
		vgui_editor_scene_source* source);
	VGUI_API void vgui_editor_drop_target_desc_default(
		vgui_editor_drop_target_desc* desc);

	VGUI_API vgui_editor_outliner* vgui_editor_outliner_create(
		vgui_editor_context* context, vgui_widget* parent);
	VGUI_API void vgui_editor_outliner_destroy(vgui_editor_outliner* outliner);
	VGUI_API vgui_widget* vgui_editor_outliner_widget(
		vgui_editor_outliner* outliner);

	/* Full-model mirroring; ignored once a scene source is installed. */
	VGUI_API void vgui_editor_outliner_clear(vgui_editor_outliner* outliner);
	VGUI_API vgui_item_id vgui_editor_outliner_add_node(
		vgui_editor_outliner* outliner, const char* text, vgui_item_id parent,
		vgui_editor_object_id object);
	VGUI_API void vgui_editor_outliner_expand(vgui_editor_outliner* outliner,
		vgui_item_id node);
	VGUI_API vgui_item_id vgui_editor_outliner_selected(
		vgui_editor_outliner* outliner);
	/* Object <-> item mapping; returns VGUI_EDITOR_INVALID_OBJECT / 0 when the
	 * id is unknown. */
	VGUI_API vgui_editor_object_id vgui_editor_outliner_object_for_item(
		vgui_editor_outliner* outliner, vgui_item_id item);
	VGUI_API vgui_item_id vgui_editor_outliner_item_for_object(
		vgui_editor_outliner* outliner, vgui_editor_object_id object);

	VGUI_API void vgui_editor_outliner_set_on_selection(
		vgui_editor_outliner* outliner, vgui_editor_object_fn fn, void* user);
	VGUI_API void vgui_editor_outliner_set_command_stack(
		vgui_editor_outliner* outliner, vgui_editor_command_stack* stack);
	VGUI_API void vgui_editor_outliner_set_move_handler(
		vgui_editor_outliner* outliner, vgui_editor_move_fn fn, void* user);
	VGUI_API int vgui_editor_outliner_move_node(vgui_editor_outliner* outliner,
		vgui_editor_object_id node, vgui_editor_object_id new_parent, size_t index);

	/* Row icons, colours and the visibility/lock buttons.  Button clicks never
	 * change the selection. */
	VGUI_API void vgui_editor_outliner_set_node_icon(
		vgui_editor_outliner* outliner, vgui_item_id node, vgui_texture* icon);
	VGUI_API void vgui_editor_outliner_set_node_text_color(
		vgui_editor_outliner* outliner, vgui_item_id node, vgui_color color);
	VGUI_API void vgui_editor_outliner_enable_row_buttons(
		vgui_editor_outliner* outliner);
	VGUI_API void vgui_editor_outliner_set_row_button_icons(
		vgui_editor_outliner* outliner, vgui_texture* visible_on,
		vgui_texture* visible_off, vgui_texture* locked_on,
		vgui_texture* locked_off);
	VGUI_API void vgui_editor_outliner_set_object_visibility(
		vgui_editor_outliner* outliner, vgui_editor_object_id object, int visible);
	VGUI_API void vgui_editor_outliner_set_object_locked(
		vgui_editor_outliner* outliner, vgui_editor_object_id object, int locked);
	VGUI_API void vgui_editor_outliner_toggle_object_visibility(
		vgui_editor_outliner* outliner, vgui_editor_object_id object);
	VGUI_API void vgui_editor_outliner_toggle_object_lock(
		vgui_editor_outliner* outliner, vgui_editor_object_id object);
	VGUI_API int vgui_editor_outliner_object_visibility(
		vgui_editor_outliner* outliner, vgui_editor_object_id object);
	VGUI_API int vgui_editor_outliner_object_locked(
		vgui_editor_outliner* outliner, vgui_editor_object_id object);
	VGUI_API void vgui_editor_outliner_set_on_visibility(
		vgui_editor_outliner* outliner, vgui_editor_flag_fn fn, void* user);
	VGUI_API void vgui_editor_outliner_set_on_lock(vgui_editor_outliner* outliner,
		vgui_editor_flag_fn fn, void* user);

	/* Inline rename (F2 / double-click once enabled). */
	VGUI_API void vgui_editor_outliner_set_rename_handler(
		vgui_editor_outliner* outliner, vgui_editor_rename_fn fn, void* user);
	VGUI_API int vgui_editor_outliner_rename_node(vgui_editor_outliner* outliner,
		vgui_editor_object_id node, const char* name);
	VGUI_API void vgui_editor_outliner_enable_inline_rename(
		vgui_editor_outliner* outliner);
	VGUI_API void vgui_editor_outliner_begin_rename(vgui_editor_outliner* outliner,
		vgui_item_id node);
	VGUI_API int vgui_editor_outliner_renaming(vgui_editor_outliner* outliner);

	/* Drag & drop (O03).  Rows drag as scene_object payloads whose id text is a
	 * comma-joined object list; the multi-selection follows when enabled. */
	VGUI_API void vgui_editor_outliner_enable_drag_drop(
		vgui_editor_outliner* outliner);
	VGUI_API void vgui_editor_outliner_set_multi_select(
		vgui_editor_outliner* outliner, int enabled);
	VGUI_API int vgui_editor_outliner_multi_select(
		vgui_editor_outliner* outliner);
	VGUI_API void vgui_editor_outliner_set_asset_drop_handler(
		vgui_editor_outliner* outliner, vgui_editor_asset_drop_node_fn fn,
		void* user);
	VGUI_API int vgui_editor_outliner_asset_drop_enabled(
		vgui_editor_outliner* outliner);
	/* x/y are absolute window coordinates, as a pointer event reports them. */
	VGUI_API int vgui_editor_outliner_apply_asset_drop(
		vgui_editor_outliner* outliner, const char* asset, float x, float y);
	VGUI_API int vgui_editor_outliner_apply_drop_payload(
		vgui_editor_outliner* outliner, const char* payload_id, float x, float y);
	VGUI_API void vgui_editor_outliner_drop_location_at(
		vgui_editor_outliner* outliner, float x, float y,
		vgui_editor_drop_location* out);
	/* Legacy form: 0 when the point resolves to VGUI_EDITOR_DROP_NONE. */
	VGUI_API int vgui_editor_outliner_row_drop_location(
		vgui_editor_outliner* outliner, float x, float y,
		vgui_editor_object_id* out_parent, size_t* out_index);

	/* Filter (O04).  Case-insensitive name substring; the match handler adds a
	 * predicate, e.g. a type filter. */
	VGUI_API void vgui_editor_outliner_set_filter(vgui_editor_outliner* outliner,
		const char* text);
	VGUI_API const char* vgui_editor_outliner_filter(
		vgui_editor_outliner* outliner);
	VGUI_API void vgui_editor_outliner_set_filter_match_handler(
		vgui_editor_outliner* outliner, vgui_editor_filter_match_fn fn, void* user);
	/* Follows the context selection: selects, expands and scrolls the row. */
	VGUI_API void vgui_editor_outliner_enable_selection_sync(
		vgui_editor_outliner* outliner);

	/* Virtual mode (O05).  Passing NULL clears the source and returns to the
	 * mirrored model. */
	VGUI_API void vgui_editor_outliner_set_scene_source(
		vgui_editor_outliner* outliner, const vgui_editor_scene_source* source);
	VGUI_API int vgui_editor_outliner_is_virtual(vgui_editor_outliner* outliner);
	VGUI_API void vgui_editor_outliner_scene_changed(
		vgui_editor_outliner* outliner);
	VGUI_API void vgui_editor_outliner_scene_row_changed(
		vgui_editor_outliner* outliner, vgui_editor_object_id object);
	VGUI_API size_t vgui_editor_outliner_projected_rows(
		vgui_editor_outliner* outliner);
	/* Selected objects in visual order; returns the full count, so a host can
	 * call once for the size and again with the real buffer.  Pass NULL out to
	 * query the count.  Feed the result to the property grid's multi-targets. */
	VGUI_API size_t vgui_editor_outliner_selection_ids(
		vgui_editor_outliner* outliner, vgui_editor_object_id* out, size_t cap);

	/* Context menu (O06): New Child / Duplicate / Rename / Delete / Focus plus
	 * the visibility and lock toggles, also reachable from Delete, F2, Ctrl+D. */
	VGUI_API void vgui_editor_outliner_enable_context_menu(
		vgui_editor_outliner* outliner);
	VGUI_API void vgui_editor_outliner_set_action_handler(
		vgui_editor_outliner* outliner, vgui_editor_outliner_action_fn fn,
		void* user);

	/* ------------------------------------------------------------------------- */
	/* Table view                                                                */
	/* ------------------------------------------------------------------------- */

	/* Virtual columns of a pull-based model.  Sorting, editing and persistence
	 * are built in; the host answers row_count / cell_text queries. */
	typedef struct vgui_editor_table vgui_editor_table;

	/* column == VGUI_EDITOR_TABLE_NO_COLUMN means "no cell". */
	#define VGUI_EDITOR_TABLE_NO_COLUMN (-1)

	typedef enum vgui_editor_table_alignment {
		VGUI_EDITOR_TABLE_ALIGN_LEFT = 0,
		VGUI_EDITOR_TABLE_ALIGN_CENTER,
		VGUI_EDITOR_TABLE_ALIGN_RIGHT
	} vgui_editor_table_alignment;

	typedef struct vgui_editor_table_cell {
		uint32_t struct_size;
		size_t row;		/* source row index */
		int column;		/* column index, or VGUI_EDITOR_TABLE_NO_COLUMN */
	} vgui_editor_table_cell;

	VGUI_API void vgui_editor_table_cell_default(vgui_editor_table_cell* cell);

	/* Per-cell presentation overrides; a zero colour keeps the theme default. */
	typedef struct vgui_editor_table_cell_style {
		uint32_t struct_size;
		vgui_color text_color;
		vgui_color background;
	} vgui_editor_table_cell_style;

	VGUI_API void vgui_editor_table_cell_style_default(
		vgui_editor_table_cell_style* style);

	/* One column.  `editor` describes the cell editor with the same descriptor
	 * the property grid uses; its own id/label fields are ignored in favour of
	 * the column's. */
	typedef struct vgui_editor_table_column {
		uint32_t struct_size;
		const char* id;
		const char* label;
		float width;
		float min_width;
		vgui_editor_table_alignment alignment;
		int sortable;
		int editable;
		const vgui_editor_property_desc* editor;
		/* Optional comparator returning <0, 0 or >0.  Without it the table
		 * compares numerically for number columns and case-insensitively for
		 * text. */
		int (*compare)(void* user, const char* a, const char* b);
		/* Optional per-column setter; return 0 to reject the value.  Without it
		 * the table calls its edit handler. */
		int (*set_value)(void* user, size_t row, const char* value);
		void* user;
		vgui_editor_release_fn release_user;
	} vgui_editor_table_column;

	VGUI_API void vgui_editor_table_column_default(
		vgui_editor_table_column* column);

	/* Pull model.  cell_text uses the string-callback convention (bytes needed,
	 * or VGUI_EDITOR_NO_VALUE); the table retries with an exact-size buffer. */
	typedef struct vgui_editor_table_source {
		uint32_t struct_size;
		size_t (*row_count)(void* user);
		size_t (*cell_text)(void* user, size_t row, const char* column, char* out,
			size_t cap);
		/* Optional stable row id; when set, selection and undo follow the row
		 * across re-sorts.  Return VGUI_EDITOR_INVALID_OBJECT for none. */
		vgui_editor_object_id (*row_id)(void* user, size_t row);
		void (*cell_style)(void* user, size_t row, const char* column,
			vgui_editor_table_cell_style* out);
		void* user;
		vgui_editor_release_fn release_user;
	} vgui_editor_table_source;

	VGUI_API void vgui_editor_table_source_default(
		vgui_editor_table_source* source);

	/* Editing callbacks.  The handler applies a value for columns without
	 * set_value; the listener runs after a value was accepted. */
	typedef void (*vgui_editor_table_select_fn)(void* user,
		const vgui_editor_table_cell* cell);
	typedef int (*vgui_editor_table_edit_fn)(void* user,
		const vgui_editor_table_cell* cell, const char* value);

	VGUI_API vgui_editor_table* vgui_editor_table_create(vgui_widget* parent);
	VGUI_API void vgui_editor_table_destroy(vgui_editor_table* table);
	VGUI_API vgui_widget* vgui_editor_table_widget(vgui_editor_table* table);
	VGUI_API void vgui_editor_table_focus(vgui_editor_table* table);
	VGUI_API int vgui_editor_table_built(vgui_editor_table* table);

	VGUI_API void vgui_editor_table_set_source(vgui_editor_table* table,
		const vgui_editor_table_source* source);

	/* Columns.  An empty id is generated from the index; a duplicate id is
	 * rejected (add) or skipped (set_columns). */
	VGUI_API int vgui_editor_table_add_column(vgui_editor_table* table,
		const vgui_editor_table_column* column);
	VGUI_API void vgui_editor_table_set_columns(vgui_editor_table* table,
		const vgui_editor_table_column* columns, size_t count);
	VGUI_API void vgui_editor_table_clear_columns(vgui_editor_table* table);
	VGUI_API size_t vgui_editor_table_column_count(vgui_editor_table* table);
	/* Borrowed column text; valid until the next column mutation. */
	VGUI_API int vgui_editor_table_column_at(vgui_editor_table* table, size_t index,
		vgui_editor_table_column* out);
	VGUI_API int vgui_editor_table_set_column_width(vgui_editor_table* table,
		size_t index, float width);
	VGUI_API float vgui_editor_table_column_width(vgui_editor_table* table,
		size_t index);

	/* Rows / sorting.  refresh() re-reads the row count and rebuilds the order;
	 * refresh_row() re-reads one row. */
	VGUI_API void vgui_editor_table_refresh(vgui_editor_table* table);
	VGUI_API void vgui_editor_table_refresh_row(vgui_editor_table* table,
		size_t source_row);
	VGUI_API size_t vgui_editor_table_row_count(vgui_editor_table* table);
	VGUI_API size_t vgui_editor_table_display_row_count(vgui_editor_table* table);
	/* Display -> source mapping; out of range returns 0.  The reverse mapping
	 * returns -1 for a hidden or invalid row. */
	VGUI_API size_t vgui_editor_table_source_row_at(vgui_editor_table* table,
		size_t display_row);
	VGUI_API int vgui_editor_table_display_index_of(vgui_editor_table* table,
		size_t source_row);
	/* Materialized window of rows; either out may be NULL. */
	VGUI_API void vgui_editor_table_visible_row_range(vgui_editor_table* table,
		size_t* first, size_t* count);
	VGUI_API int vgui_editor_table_sort_column(vgui_editor_table* table);
	VGUI_API int vgui_editor_table_sort_ascending(vgui_editor_table* table);
	/* column < 0 clears the sort; 0 when the column is not sortable. */
	VGUI_API int vgui_editor_table_set_sort(vgui_editor_table* table, int column,
		int ascending);
	VGUI_API void vgui_editor_table_clear_sort(vgui_editor_table* table);

	/* Selection and hit-testing (content coordinates). */
	VGUI_API void vgui_editor_table_selected_cell(vgui_editor_table* table,
		vgui_editor_table_cell* out);
	VGUI_API int vgui_editor_table_select_cell(vgui_editor_table* table,
		size_t source_row, int column);
	VGUI_API void vgui_editor_table_clear_selection(vgui_editor_table* table);
	VGUI_API void vgui_editor_table_hit_test(vgui_editor_table* table,
		float content_x, float content_y, vgui_editor_table_cell* out);
	VGUI_API void vgui_editor_table_set_on_selection(vgui_editor_table* table,
		vgui_editor_table_select_fn fn, void* user);

	/* Absolute rectangles; cell_rect also answers for off-screen rows.  Any out
	 * pointer may be NULL. */
	VGUI_API int vgui_editor_table_cell_rect(vgui_editor_table* table,
		size_t source_row, int column, float* x, float* y, float* width,
		float* height);
	VGUI_API int vgui_editor_table_header_rect(vgui_editor_table* table, int column,
		float* x, float* y, float* width, float* height);

	VGUI_API float vgui_editor_table_scroll_x(vgui_editor_table* table);
	VGUI_API float vgui_editor_table_scroll_y(vgui_editor_table* table);
	VGUI_API void vgui_editor_table_set_scroll(vgui_editor_table* table, float x,
		float y);
	VGUI_API float vgui_editor_table_max_scroll_x(vgui_editor_table* table);
	VGUI_API float vgui_editor_table_max_scroll_y(vgui_editor_table* table);
	VGUI_API void vgui_editor_table_scroll_to_row(vgui_editor_table* table,
		size_t source_row);

	VGUI_API void vgui_editor_table_set_row_height(vgui_editor_table* table,
		float height);
	VGUI_API float vgui_editor_table_row_height(vgui_editor_table* table);
	VGUI_API float vgui_editor_table_header_height(vgui_editor_table* table);

	/* Editing.  begin_edit() opens the editor for the selected cell; commit_edit()
	 * is the programmatic form and returns 0 when validation or the host setter
	 * rejects the value. */
	VGUI_API int vgui_editor_table_begin_edit(vgui_editor_table* table);
	VGUI_API int vgui_editor_table_begin_edit_cell(vgui_editor_table* table,
		size_t source_row, int column);
	VGUI_API int vgui_editor_table_cancel_edit(vgui_editor_table* table);
	VGUI_API int vgui_editor_table_editing(vgui_editor_table* table);
	VGUI_API void vgui_editor_table_editing_cell(vgui_editor_table* table,
		vgui_editor_table_cell* out);
	VGUI_API int vgui_editor_table_commit_edit(vgui_editor_table* table,
		const char* value);
	VGUI_API void vgui_editor_table_set_edit_handler(vgui_editor_table* table,
		vgui_editor_table_edit_fn fn, void* user);
	VGUI_API void vgui_editor_table_set_on_edit(vgui_editor_table* table,
		vgui_editor_table_edit_fn fn, void* user);
	VGUI_API void vgui_editor_table_set_command_stack(vgui_editor_table* table,
		vgui_editor_command_stack* stack);

	/* Column widths and sort as JSON.  capture_state_json() returns the bytes
	 * needed; apply_state_json() clears an invalid sort column and ignores
	 * widths beyond the current column count. */
	VGUI_API size_t vgui_editor_table_capture_state_json(vgui_editor_table* table,
		char* out, size_t cap);
	VGUI_API int vgui_editor_table_apply_state_json(vgui_editor_table* table,
		const char* json);

	/* Cell-text queries performed by the last paint: proof that only the
	 * visible rows were read. */
	VGUI_API size_t vgui_editor_table_last_render_cell_queries(
		vgui_editor_table* table);

	/* ------------------------------------------------------------------------- */
	/* Timeline                                                                  */
	/* ------------------------------------------------------------------------- */

	typedef struct vgui_editor_timeline vgui_editor_timeline;

	/* Keyframe interpolation.  The ordering mirrors
	 * vgui_editor_kit::timeline_interpolation. */
	typedef enum vgui_editor_timeline_interpolation {
		VGUI_EDITOR_TIMELINE_STEP = 0,
		VGUI_EDITOR_TIMELINE_LINEAR,
		VGUI_EDITOR_TIMELINE_SMOOTH
	} vgui_editor_timeline_interpolation;

	/* Live pointer gesture, reported for debugging and hosts that snapshot the
	 * model before a drag. */
	typedef enum vgui_editor_timeline_gesture {
		VGUI_EDITOR_TIMELINE_GESTURE_NONE = 0,
		VGUI_EDITOR_TIMELINE_GESTURE_PENDING_MOVE,
		VGUI_EDITOR_TIMELINE_GESTURE_MOVE,
		VGUI_EDITOR_TIMELINE_GESTURE_PENDING_MARQUEE,
		VGUI_EDITOR_TIMELINE_GESTURE_MARQUEE,
		VGUI_EDITOR_TIMELINE_GESTURE_PAN,
		VGUI_EDITOR_TIMELINE_GESTURE_SCRUB
	} vgui_editor_timeline_gesture;

	typedef struct vgui_editor_timeline_keyframe {
		/* Stable id; 0 lets the timeline allocate one on add. */
		vgui_editor_object_id id;
		double time;
		double value;
		vgui_editor_timeline_interpolation interpolation;
	} vgui_editor_timeline_keyframe;

	/* One track.  `keyframes` is a borrowed array the kit copies; the track's
	 * own keyframe list is replaced wholesale. */
	typedef struct vgui_editor_timeline_track {
		uint32_t struct_size;
		vgui_editor_object_id id;
		const char* name;
		int visible;
		int locked;
		int muted;
		/* Row height in logical pixels; clamped to a readable range. */
		float height;
		vgui_color color;
		const vgui_editor_timeline_keyframe* keyframes;
		size_t keyframe_count;
	} vgui_editor_timeline_track;

	VGUI_API void vgui_editor_timeline_track_default(
		vgui_editor_timeline_track* track);
	VGUI_API void vgui_editor_timeline_keyframe_default(
		vgui_editor_timeline_keyframe* keyframe);

	/* Horizontal view settings, in seconds and pixels. */
	typedef struct vgui_editor_timeline_view_state {
		uint32_t struct_size;
		/* Time shown at the left edge of the lane area. */
		double view_start;
		/* Positive seconds -> pixels scale. */
		double pixels_per_second;
		/* Vertical track scroll in logical pixels. */
		float scroll_y;
	} vgui_editor_timeline_view_state;

	VGUI_API void vgui_editor_timeline_view_state_default(
		vgui_editor_timeline_view_state* state);

	VGUI_API vgui_editor_timeline* vgui_editor_timeline_create(
		vgui_widget* parent);
	VGUI_API void vgui_editor_timeline_destroy(vgui_editor_timeline* timeline);
	VGUI_API vgui_widget* vgui_editor_timeline_widget(
		vgui_editor_timeline* timeline);
	VGUI_API void vgui_editor_timeline_focus(vgui_editor_timeline* timeline);
	VGUI_API int vgui_editor_timeline_focused(vgui_editor_timeline* timeline);
	VGUI_API int vgui_editor_timeline_built(vgui_editor_timeline* timeline);
	VGUI_API void vgui_editor_timeline_set_enabled(
		vgui_editor_timeline* timeline, int enabled);
	VGUI_API int vgui_editor_timeline_enabled(vgui_editor_timeline* timeline);
	VGUI_API void vgui_editor_timeline_set_drag_threshold(
		vgui_editor_timeline* timeline, float pixels);
	VGUI_API float vgui_editor_timeline_drag_threshold(
		vgui_editor_timeline* timeline);

	/* Model.  add_* returns the (possibly allocated) id, or
	 * VGUI_EDITOR_INVALID_OBJECT for a duplicate explicit id.  Direct model
	 * edits notify change listeners but never push undo commands; hosts use
	 * them while loading a document. */
	VGUI_API vgui_editor_object_id vgui_editor_timeline_add_track(
		vgui_editor_timeline* timeline, const vgui_editor_timeline_track* track);
	VGUI_API int vgui_editor_timeline_update_track(
		vgui_editor_timeline* timeline, const vgui_editor_timeline_track* track);
	VGUI_API int vgui_editor_timeline_remove_track(
		vgui_editor_timeline* timeline, vgui_editor_object_id id);
	VGUI_API void vgui_editor_timeline_clear_tracks(
		vgui_editor_timeline* timeline);
	VGUI_API size_t vgui_editor_timeline_track_count(
		vgui_editor_timeline* timeline);
	/* Index access; 0 out of range.  `out->keyframes` borrows the kit's list and
	 * stays valid until the next mutation of this timeline. */
	VGUI_API int vgui_editor_timeline_track_at(
		vgui_editor_timeline* timeline, size_t index,
		vgui_editor_timeline_track* out);
	VGUI_API int vgui_editor_timeline_find_track(
		vgui_editor_timeline* timeline, vgui_editor_object_id id,
		vgui_editor_timeline_track* out);

	VGUI_API vgui_editor_object_id vgui_editor_timeline_add_keyframe(
		vgui_editor_timeline* timeline, vgui_editor_object_id track,
		const vgui_editor_timeline_keyframe* keyframe);
	VGUI_API int vgui_editor_timeline_update_keyframe(
		vgui_editor_timeline* timeline, vgui_editor_object_id track,
		const vgui_editor_timeline_keyframe* keyframe);
	VGUI_API int vgui_editor_timeline_remove_keyframe(
		vgui_editor_timeline* timeline, vgui_editor_object_id track,
		vgui_editor_object_id keyframe);
	VGUI_API size_t vgui_editor_timeline_keyframe_count(
		vgui_editor_timeline* timeline, vgui_editor_object_id track);
	VGUI_API int vgui_editor_timeline_keyframe_at(
		vgui_editor_timeline* timeline, vgui_editor_object_id track, size_t index,
		vgui_editor_timeline_keyframe* out);
	/* Searches every track; out_track may be NULL. */
	VGUI_API int vgui_editor_timeline_find_keyframe(
		vgui_editor_timeline* timeline, vgui_editor_object_id keyframe,
		vgui_editor_object_id* out_track, vgui_editor_timeline_keyframe* out);
	/* Samples a track with its per-keyframe interpolation mode. */
	VGUI_API double vgui_editor_timeline_evaluate(
		vgui_editor_timeline* timeline, vgui_editor_object_id track, double time);

	/* Keyframe selection. */
	VGUI_API void vgui_editor_timeline_set_selection(
		vgui_editor_timeline* timeline, const vgui_editor_object_id* ids,
		size_t id_count, vgui_editor_object_id primary);
	VGUI_API void vgui_editor_timeline_clear_selection(
		vgui_editor_timeline* timeline);
	VGUI_API void vgui_editor_timeline_select_all(
		vgui_editor_timeline* timeline);
	VGUI_API size_t vgui_editor_timeline_selection_count(
		vgui_editor_timeline* timeline);
	VGUI_API vgui_editor_object_id vgui_editor_timeline_selection_id_at(
		vgui_editor_timeline* timeline, size_t index);
	VGUI_API vgui_editor_object_id vgui_editor_timeline_primary_selection(
		vgui_editor_timeline* timeline);
	VGUI_API int vgui_editor_timeline_is_selected(
		vgui_editor_timeline* timeline, vgui_editor_object_id id);
	VGUI_API void vgui_editor_timeline_set_multi_select(
		vgui_editor_timeline* timeline, int enabled);
	VGUI_API int vgui_editor_timeline_multi_select(
		vgui_editor_timeline* timeline);
	typedef void (*vgui_editor_timeline_selection_fn)(void* user,
		const vgui_editor_object_id* ids, size_t id_count);
	VGUI_API void vgui_editor_timeline_set_on_selection(
		vgui_editor_timeline* timeline, vgui_editor_timeline_selection_fn fn,
		void* user);
	/* Track whose header is highlighted and receives new keyframes. */
	VGUI_API vgui_editor_object_id vgui_editor_timeline_selected_track(
		vgui_editor_timeline* timeline);
	VGUI_API void vgui_editor_timeline_set_selected_track(
		vgui_editor_timeline* timeline, vgui_editor_object_id track);

	/* Fired for model, view, playback and gesture changes (not for keyframe
	 * selection). */
	typedef void (*vgui_editor_timeline_change_fn)(void* user);
	VGUI_API void vgui_editor_timeline_set_on_change(
		vgui_editor_timeline* timeline, vgui_editor_timeline_change_fn fn,
		void* user);

	/* View / zoom / pan. */
	VGUI_API void vgui_editor_timeline_get_view_state(
		vgui_editor_timeline* timeline, vgui_editor_timeline_view_state* out);
	/* Cancels an in-flight gesture so the view never changes under a drag. */
	VGUI_API void vgui_editor_timeline_set_view_state(
		vgui_editor_timeline* timeline,
		const vgui_editor_timeline_view_state* state);
	VGUI_API double vgui_editor_timeline_zoom(vgui_editor_timeline* timeline);
	VGUI_API void vgui_editor_timeline_set_zoom_limits(
		vgui_editor_timeline* timeline, double minimum, double maximum);
	/* Zooms by `factor`, keeping lane-local x fixed. */
	VGUI_API void vgui_editor_timeline_zoom_at(vgui_editor_timeline* timeline,
		float lane_x, double factor);
	VGUI_API void vgui_editor_timeline_pan_by_pixels(
		vgui_editor_timeline* timeline, float delta_pixels);
	VGUI_API void vgui_editor_timeline_reset_view(
		vgui_editor_timeline* timeline);
	/* Lane-local content x <-> time and the variants that include the track
	 * header column. */
	VGUI_API double vgui_editor_timeline_time_to_x(
		vgui_editor_timeline* timeline, double time);
	VGUI_API double vgui_editor_timeline_x_to_time(
		vgui_editor_timeline* timeline, double lane_x);
	VGUI_API double vgui_editor_timeline_time_at_content_x(
		vgui_editor_timeline* timeline, float content_x);
	VGUI_API float vgui_editor_timeline_content_x_at_time(
		vgui_editor_timeline* timeline, double time);

	/* Snapping / timing.  set_frame_rate() keeps the step in sync until a custom
	 * step is supplied. */
	VGUI_API void vgui_editor_timeline_set_snap_enabled(
		vgui_editor_timeline* timeline, int enabled);
	VGUI_API int vgui_editor_timeline_snap_enabled(
		vgui_editor_timeline* timeline);
	VGUI_API void vgui_editor_timeline_set_snap_step(
		vgui_editor_timeline* timeline, double seconds);
	VGUI_API double vgui_editor_timeline_snap_step(
		vgui_editor_timeline* timeline);
	VGUI_API void vgui_editor_timeline_set_snap_to_frames(
		vgui_editor_timeline* timeline, int enabled);
	VGUI_API int vgui_editor_timeline_snap_to_frames(
		vgui_editor_timeline* timeline);
	VGUI_API double vgui_editor_timeline_snap_time(
		vgui_editor_timeline* timeline, double time);
	VGUI_API void vgui_editor_timeline_set_frame_rate(
		vgui_editor_timeline* timeline, double fps);
	VGUI_API double vgui_editor_timeline_frame_rate(
		vgui_editor_timeline* timeline);
	VGUI_API void vgui_editor_timeline_set_duration(
		vgui_editor_timeline* timeline, double seconds);
	VGUI_API double vgui_editor_timeline_duration(
		vgui_editor_timeline* timeline);

	/* Playback; the host drives time with tick(). */
	VGUI_API void vgui_editor_timeline_set_playhead(
		vgui_editor_timeline* timeline, double seconds);
	VGUI_API double vgui_editor_timeline_playhead(vgui_editor_timeline* timeline);
	VGUI_API void vgui_editor_timeline_play(vgui_editor_timeline* timeline);
	VGUI_API void vgui_editor_timeline_pause(vgui_editor_timeline* timeline);
	/* Pauses and rewinds to 0. */
	VGUI_API void vgui_editor_timeline_stop(vgui_editor_timeline* timeline);
	VGUI_API void vgui_editor_timeline_toggle_play(vgui_editor_timeline* timeline);
	VGUI_API int vgui_editor_timeline_playing(vgui_editor_timeline* timeline);
	VGUI_API void vgui_editor_timeline_set_loop(vgui_editor_timeline* timeline,
		int loop);
	VGUI_API int vgui_editor_timeline_loop(vgui_editor_timeline* timeline);
	VGUI_API void vgui_editor_timeline_tick(vgui_editor_timeline* timeline,
		double delta_seconds);
	/* Pauses and moves the playhead by whole frames. */
	VGUI_API void vgui_editor_timeline_step_frames(vgui_editor_timeline* timeline,
		int frames);
	typedef void (*vgui_editor_timeline_playhead_fn)(void* user, double seconds);
	VGUI_API void vgui_editor_timeline_set_on_playhead(
		vgui_editor_timeline* timeline, vgui_editor_timeline_playhead_fn fn,
		void* user);

	/* Editing.  These are the user-action forms and push one undo command each
	 * when a command stack is installed. */
	VGUI_API void vgui_editor_timeline_copy_selection(
		vgui_editor_timeline* timeline);
	VGUI_API void vgui_editor_timeline_cut_selection(
		vgui_editor_timeline* timeline);
	/* Pastes at the (snapped) playhead, preserving relative times; 0 when the
	 * clipboard is empty. */
	VGUI_API int vgui_editor_timeline_paste_at_playhead(
		vgui_editor_timeline* timeline);
	VGUI_API int vgui_editor_timeline_delete_selection(
		vgui_editor_timeline* timeline);
	VGUI_API vgui_editor_object_id vgui_editor_timeline_insert_keyframe(
		vgui_editor_timeline* timeline, vgui_editor_object_id track, double time,
		double value);
	/* Uses selected_track(), or the first track when none is selected. */
	VGUI_API vgui_editor_object_id vgui_editor_timeline_add_keyframe_at_playhead(
		vgui_editor_timeline* timeline);
	VGUI_API size_t vgui_editor_timeline_clipboard_count(
		vgui_editor_timeline* timeline);

	/* Geometry (content-local rectangles, inside the 1px widget border).  Any
	 * out pointer may be NULL. */
	VGUI_API int vgui_editor_timeline_track_rect(
		vgui_editor_timeline* timeline, vgui_editor_object_id track, float* x,
		float* y, float* width, float* height);
	VGUI_API int vgui_editor_timeline_keyframe_rect(
		vgui_editor_timeline* timeline, vgui_editor_object_id keyframe, float* x,
		float* y, float* width, float* height);
	/* 0 when no keyframe is hit. */
	VGUI_API vgui_editor_object_id vgui_editor_timeline_hit_test(
		vgui_editor_timeline* timeline, float content_x, float content_y);
	/* Track under a content y, including its header row. */
	VGUI_API vgui_editor_object_id vgui_editor_timeline_track_at_y(
		vgui_editor_timeline* timeline, float content_y);
	VGUI_API int vgui_editor_timeline_dragging(vgui_editor_timeline* timeline);
	VGUI_API vgui_editor_timeline_gesture vgui_editor_timeline_get_gesture(
		vgui_editor_timeline* timeline);
	VGUI_API void vgui_editor_timeline_cancel_gesture(
		vgui_editor_timeline* timeline);

	VGUI_API void vgui_editor_timeline_set_command_stack(
		vgui_editor_timeline* timeline, vgui_editor_command_stack* stack);

	/* View settings, playhead, duration, frame rate, snapping and loop as JSON.
	 * capture_state_json() returns the bytes needed; apply_state_json()
	 * validates the whole payload before mutating anything. */
	VGUI_API size_t vgui_editor_timeline_capture_state_json(
		vgui_editor_timeline* timeline, char* out, size_t cap);
	VGUI_API int vgui_editor_timeline_apply_state_json(
		vgui_editor_timeline* timeline, const char* json);

	/* ------------------------------------------------------------------------- */
	/* Node graph                                                                */
	/* ------------------------------------------------------------------------- */

	typedef struct vgui_editor_node_graph vgui_editor_node_graph;

	/* The ordering of these enums mirrors the kit's own enumerations. */
	typedef enum vgui_editor_node_port_direction {
		VGUI_EDITOR_NODE_PORT_INPUT = 0,
		VGUI_EDITOR_NODE_PORT_OUTPUT
	} vgui_editor_node_port_direction;

	typedef enum vgui_editor_node_port_type {
		VGUI_EDITOR_NODE_TYPE_FLOW = 0,
		VGUI_EDITOR_NODE_TYPE_NUMBER,
		VGUI_EDITOR_NODE_TYPE_STRING,
		VGUI_EDITOR_NODE_TYPE_BOOLEAN,
		VGUI_EDITOR_NODE_TYPE_COLOR,
		VGUI_EDITOR_NODE_TYPE_VECTOR,
		VGUI_EDITOR_NODE_TYPE_OBJECT,
		VGUI_EDITOR_NODE_TYPE_CUSTOM
	} vgui_editor_node_port_type;

	typedef enum vgui_editor_node_graph_gesture {
		VGUI_EDITOR_NODE_GESTURE_NONE = 0,
		VGUI_EDITOR_NODE_GESTURE_PENDING_MOVE,
		VGUI_EDITOR_NODE_GESTURE_MOVE,
		VGUI_EDITOR_NODE_GESTURE_PENDING_MARQUEE,
		VGUI_EDITOR_NODE_GESTURE_MARQUEE,
		VGUI_EDITOR_NODE_GESTURE_PAN,
		VGUI_EDITOR_NODE_GESTURE_CONNECT,
		VGUI_EDITOR_NODE_GESTURE_MINIMAP
	} vgui_editor_node_graph_gesture;

	/* Context-menu / command verbs. */
	typedef enum vgui_editor_node_graph_action {
		VGUI_EDITOR_NODE_ACTION_ADD_NODE = 0,
		VGUI_EDITOR_NODE_ACTION_DUPLICATE,
		VGUI_EDITOR_NODE_ACTION_DELETE_SELECTION,
		VGUI_EDITOR_NODE_ACTION_DISCONNECT,
		VGUI_EDITOR_NODE_ACTION_FRAME_SELECTION,
		VGUI_EDITOR_NODE_ACTION_FIT_ALL,
		VGUI_EDITOR_NODE_ACTION_SELECT_ALL,
		VGUI_EDITOR_NODE_ACTION_TOGGLE_GRID,
		VGUI_EDITOR_NODE_ACTION_TOGGLE_MINIMAP,
		VGUI_EDITOR_NODE_ACTION_TOGGLE_SNAP
	} vgui_editor_node_graph_action;

	typedef struct vgui_editor_node_point {
		double x;
		double y;
	} vgui_editor_node_point;

	typedef struct vgui_editor_node_rect {
		double x;
		double y;
		double width;
		double height;
	} vgui_editor_node_rect;

	typedef struct vgui_editor_node_port {
		uint32_t struct_size;
		/* Stable id; 0 lets the graph allocate one. */
		vgui_editor_object_id id;
		const char* name;
		vgui_editor_node_port_direction direction;
		vgui_editor_node_port_type type;
		/* False inputs accept one connection; a new link replaces the old. */
		int multi;
	} vgui_editor_node_port;

	typedef struct vgui_editor_node_node {
		uint32_t struct_size;
		vgui_editor_object_id id;
		const char* title;
		const char* category;
		/* Top-left corner in graph coordinates. */
		double x;
		double y;
		double width;
		int collapsed;
		int visible;
		/* Locked nodes stay selectable but cannot be moved. */
		int locked;
		/* Header accent; 0 uses the theme accent. */
		vgui_color accent;
		/* Port arrays.  add/update copy them and the host owns them for the call;
		 * the create-node callback receives borrowed scratch it writes in place. */
		vgui_editor_node_port* inputs;
		size_t input_count;
		vgui_editor_node_port* outputs;
		size_t output_count;
	} vgui_editor_node_node;

	typedef struct vgui_editor_node_connection {
		uint32_t struct_size;
		vgui_editor_object_id id;
		vgui_editor_object_id from_node;
		vgui_editor_object_id from_port;
		vgui_editor_object_id to_node;
		vgui_editor_object_id to_port;
		/* 0 uses the source port type colour. */
		vgui_color color;
		int visible;
	} vgui_editor_node_connection;

	typedef struct vgui_editor_node_view_state {
		uint32_t struct_size;
		/* Graph coordinate shown at the content top-left. */
		double offset_x;
		double offset_y;
		/* Positive graph units -> content pixels. */
		double zoom;
	} vgui_editor_node_view_state;

	VGUI_API void vgui_editor_node_port_default(vgui_editor_node_port* port);
	VGUI_API void vgui_editor_node_node_default(vgui_editor_node_node* node);
	VGUI_API void vgui_editor_node_connection_default(
		vgui_editor_node_connection* connection);
	VGUI_API void vgui_editor_node_view_state_default(
		vgui_editor_node_view_state* state);

	VGUI_API vgui_editor_node_graph* vgui_editor_node_graph_create(
		vgui_widget* parent);
	VGUI_API void vgui_editor_node_graph_destroy(
		vgui_editor_node_graph* graph);
	VGUI_API vgui_widget* vgui_editor_node_graph_widget(
		vgui_editor_node_graph* graph);
	VGUI_API void vgui_editor_node_graph_focus(vgui_editor_node_graph* graph);
	VGUI_API int vgui_editor_node_graph_focused(vgui_editor_node_graph* graph);
	VGUI_API int vgui_editor_node_graph_built(vgui_editor_node_graph* graph);
	VGUI_API void vgui_editor_node_graph_set_enabled(
		vgui_editor_node_graph* graph, int enabled);
	VGUI_API int vgui_editor_node_graph_enabled(vgui_editor_node_graph* graph);
	VGUI_API void vgui_editor_node_graph_set_drag_threshold(
		vgui_editor_node_graph* graph, float pixels);
	VGUI_API float vgui_editor_node_graph_drag_threshold(
		vgui_editor_node_graph* graph);

	/* Model.  Node, port and connection ids share one space.  add_* returns the
	 * id or VGUI_EDITOR_INVALID_OBJECT for a duplicate explicit id. */
	VGUI_API vgui_editor_object_id vgui_editor_node_graph_add_node(
		vgui_editor_node_graph* graph, const vgui_editor_node_node* node);
	/* Replaces the record and its port lists; connections referencing removed
	 * ports are dropped. */
	VGUI_API int vgui_editor_node_graph_update_node(
		vgui_editor_node_graph* graph, const vgui_editor_node_node* node);
	VGUI_API int vgui_editor_node_graph_remove_node(
		vgui_editor_node_graph* graph, vgui_editor_object_id id);
	VGUI_API void vgui_editor_node_graph_clear_nodes(
		vgui_editor_node_graph* graph);
	VGUI_API size_t vgui_editor_node_graph_node_count(
		vgui_editor_node_graph* graph);
	/* `out` borrows the kit's port arrays; valid until the next mutation.  Any
	 * out pointer may be NULL. */
	VGUI_API int vgui_editor_node_graph_get_node(
		vgui_editor_node_graph* graph, size_t index, vgui_editor_node_node* out);
	VGUI_API int vgui_editor_node_graph_find_node(
		vgui_editor_node_graph* graph, vgui_editor_object_id id,
		vgui_editor_node_node* out);

	VGUI_API vgui_editor_object_id vgui_editor_node_graph_add_port(
		vgui_editor_node_graph* graph, vgui_editor_object_id node,
		const vgui_editor_node_port* port);
	/* Keeps the existing port id; a direction change moves the port between the
	 * lists and drops now-invalid connections. */
	VGUI_API int vgui_editor_node_graph_update_port(
		vgui_editor_node_graph* graph, const vgui_editor_node_port* port);
	VGUI_API int vgui_editor_node_graph_remove_port(
		vgui_editor_node_graph* graph, vgui_editor_object_id port);
	VGUI_API int vgui_editor_node_graph_find_port(
		vgui_editor_node_graph* graph, vgui_editor_object_id port,
		vgui_editor_node_port* out, vgui_editor_object_id* out_node,
		vgui_editor_node_port_direction* out_direction);

	VGUI_API vgui_editor_object_id vgui_editor_node_graph_add_connection(
		vgui_editor_node_graph* graph,
		const vgui_editor_node_connection* connection);
	VGUI_API int vgui_editor_node_graph_update_connection(
		vgui_editor_node_graph* graph,
		const vgui_editor_node_connection* connection);
	VGUI_API int vgui_editor_node_graph_remove_connection(
		vgui_editor_node_graph* graph, vgui_editor_object_id id);
	VGUI_API void vgui_editor_node_graph_clear_connections(
		vgui_editor_node_graph* graph);
	VGUI_API size_t vgui_editor_node_graph_connection_count(
		vgui_editor_node_graph* graph);
	VGUI_API int vgui_editor_node_graph_get_connection(
		vgui_editor_node_graph* graph, size_t index,
		vgui_editor_node_connection* out);
	VGUI_API int vgui_editor_node_graph_find_connection(
		vgui_editor_node_graph* graph, vgui_editor_object_id id,
		vgui_editor_node_connection* out);

	/* Selection. */
	VGUI_API void vgui_editor_node_graph_set_selection(
		vgui_editor_node_graph* graph, const vgui_editor_object_id* ids,
		size_t id_count, vgui_editor_object_id primary);
	VGUI_API void vgui_editor_node_graph_clear_selection(
		vgui_editor_node_graph* graph);
	VGUI_API void vgui_editor_node_graph_select_all(
		vgui_editor_node_graph* graph);
	VGUI_API size_t vgui_editor_node_graph_selection_count(
		vgui_editor_node_graph* graph);
	VGUI_API vgui_editor_object_id vgui_editor_node_graph_selection_id_at(
		vgui_editor_node_graph* graph, size_t index);
	VGUI_API vgui_editor_object_id vgui_editor_node_graph_primary_selection(
		vgui_editor_node_graph* graph);
	VGUI_API int vgui_editor_node_graph_is_selected(
		vgui_editor_node_graph* graph, vgui_editor_object_id id);
	VGUI_API void vgui_editor_node_graph_set_multi_select(
		vgui_editor_node_graph* graph, int enabled);
	VGUI_API int vgui_editor_node_graph_multi_select(
		vgui_editor_node_graph* graph);
	typedef void (*vgui_editor_node_selection_fn)(void* user,
		const vgui_editor_object_id* ids, size_t id_count);
	VGUI_API void vgui_editor_node_graph_set_on_selection(
		vgui_editor_node_graph* graph, vgui_editor_node_selection_fn fn,
		void* user);
	typedef void (*vgui_editor_node_change_fn)(void* user);
	VGUI_API void vgui_editor_node_graph_set_on_change(
		vgui_editor_node_graph* graph, vgui_editor_node_change_fn fn,
		void* user);

	/* View / zoom / pan. */
	VGUI_API void vgui_editor_node_graph_get_view_state(
		vgui_editor_node_graph* graph, vgui_editor_node_view_state* out);
	VGUI_API void vgui_editor_node_graph_set_view_state(
		vgui_editor_node_graph* graph, const vgui_editor_node_view_state* state);
	VGUI_API double vgui_editor_node_graph_zoom(vgui_editor_node_graph* graph);
	VGUI_API void vgui_editor_node_graph_set_zoom_limits(
		vgui_editor_node_graph* graph, double minimum, double maximum);
	VGUI_API void vgui_editor_node_graph_zoom_at(
		vgui_editor_node_graph* graph, float content_x, float content_y,
		double factor);
	VGUI_API void vgui_editor_node_graph_pan_by_pixels(
		vgui_editor_node_graph* graph, float delta_x, float delta_y);
	VGUI_API void vgui_editor_node_graph_reset_view(
		vgui_editor_node_graph* graph);
	VGUI_API void vgui_editor_node_graph_frame_selection(
		vgui_editor_node_graph* graph);
	VGUI_API void vgui_editor_node_graph_fit_all(vgui_editor_node_graph* graph);

	/* Grid, snapping and minimap. */
	VGUI_API void vgui_editor_node_graph_set_grid_visible(
		vgui_editor_node_graph* graph, int visible);
	VGUI_API int vgui_editor_node_graph_grid_visible(vgui_editor_node_graph* graph);
	VGUI_API void vgui_editor_node_graph_set_grid_step(
		vgui_editor_node_graph* graph, double step);
	VGUI_API double vgui_editor_node_graph_grid_step(vgui_editor_node_graph* graph);
	/* Base step after adaptive density for the current zoom. */
	VGUI_API double vgui_editor_node_graph_effective_grid_step(
		vgui_editor_node_graph* graph);
	VGUI_API void vgui_editor_node_graph_set_snap_to_grid(
		vgui_editor_node_graph* graph, int enabled);
	VGUI_API int vgui_editor_node_graph_snap_to_grid(
		vgui_editor_node_graph* graph);
	VGUI_API vgui_editor_node_point vgui_editor_node_graph_snap_point(
		vgui_editor_node_graph* graph, vgui_editor_node_point point);
	VGUI_API void vgui_editor_node_graph_set_minimap_visible(
		vgui_editor_node_graph* graph, int visible);
	VGUI_API int vgui_editor_node_graph_minimap_visible(
		vgui_editor_node_graph* graph);
	/* 0 when the minimap is hidden or the view is too small. */
	VGUI_API int vgui_editor_node_graph_minimap_rect(
		vgui_editor_node_graph* graph, float* x, float* y, float* width,
		float* height);

	/* Coordinates / geometry.  Screen points are content-local (inside the 1px
	 * border); rectangles are graph coordinates. */
	VGUI_API vgui_editor_node_point vgui_editor_node_graph_screen_to_graph(
		vgui_editor_node_graph* graph, float content_x, float content_y);
	VGUI_API vgui_editor_node_point vgui_editor_node_graph_graph_to_screen(
		vgui_editor_node_graph* graph, double graph_x, double graph_y);
	VGUI_API vgui_editor_node_rect vgui_editor_node_graph_node_bounds(
		vgui_editor_node_graph* graph, vgui_editor_object_id node);
	VGUI_API vgui_editor_node_rect vgui_editor_node_graph_port_bounds(
		vgui_editor_node_graph* graph, vgui_editor_object_id port);
	VGUI_API int vgui_editor_node_graph_port_position(
		vgui_editor_node_graph* graph, vgui_editor_object_id port,
		vgui_editor_node_point* out);
	VGUI_API vgui_editor_node_rect vgui_editor_node_graph_selection_bounds(
		vgui_editor_node_graph* graph);
	VGUI_API vgui_editor_node_rect vgui_editor_node_graph_content_bounds(
		vgui_editor_node_graph* graph);
	/* 6 screen pixels expressed in graph units at the current zoom. */
	VGUI_API double vgui_editor_node_graph_connection_tolerance(
		vgui_editor_node_graph* graph);

	/* Picking; VGUI_EDITOR_INVALID_OBJECT when nothing is hit.  node_hit ignores
	 * ports, port_hit ignores node bodies, hit_test prefers ports, then nodes,
	 * then connections. */
	VGUI_API vgui_editor_object_id vgui_editor_node_graph_node_hit(
		vgui_editor_node_graph* graph, float content_x, float content_y);
	VGUI_API vgui_editor_object_id vgui_editor_node_graph_port_hit(
		vgui_editor_node_graph* graph, float content_x, float content_y);
	VGUI_API vgui_editor_object_id vgui_editor_node_graph_connection_hit(
		vgui_editor_node_graph* graph, float content_x, float content_y);
	VGUI_API vgui_editor_object_id vgui_editor_node_graph_hit_test(
		vgui_editor_node_graph* graph, float content_x, float content_y);

	/* Interaction state. */
	VGUI_API int vgui_editor_node_graph_dragging(vgui_editor_node_graph* graph);
	VGUI_API vgui_editor_node_graph_gesture vgui_editor_node_graph_get_gesture(
		vgui_editor_node_graph* graph);
	VGUI_API vgui_editor_object_id vgui_editor_node_graph_pending_port(
		vgui_editor_node_graph* graph);
	VGUI_API void vgui_editor_node_graph_cancel_gesture(
		vgui_editor_node_graph* graph);

	/* Editing actions (one undo command each when a stack is installed).  0 when
	 * nothing changed. */
	VGUI_API int vgui_editor_node_graph_connect_ports(
		vgui_editor_node_graph* graph, vgui_editor_object_id port_a,
		vgui_editor_object_id port_b);
	VGUI_API int vgui_editor_node_graph_disconnect_selection(
		vgui_editor_node_graph* graph);
	VGUI_API int vgui_editor_node_graph_delete_selection(
		vgui_editor_node_graph* graph);
	VGUI_API int vgui_editor_node_graph_duplicate_selection(
		vgui_editor_node_graph* graph);

	/* Node creation callback: fill `out` (its x/y already hold the drop
	 * location) and return 0 to cancel.  out->inputs / out->outputs are library
	 * scratch holding VGUI_EDITOR_NODE_CREATE_PORTS slots beyond the reported
	 * count, so write into them and raise the count; storage stays valid until
	 * the library reads the record back right after the callback returns. */
	#define VGUI_EDITOR_NODE_CREATE_PORTS 8
	typedef int (*vgui_editor_node_create_fn)(void* user,
		vgui_editor_node_node* out);
	VGUI_API void vgui_editor_node_graph_set_create_node(
		vgui_editor_node_graph* graph, vgui_editor_node_create_fn fn,
		void* user, vgui_editor_release_fn release_user);
	VGUI_API int vgui_editor_node_graph_perform_action(
		vgui_editor_node_graph* graph, vgui_editor_node_graph_action action);
	/* The graph's own menu widget (borrowed, owned by the graph). */
	VGUI_API vgui_widget* vgui_editor_node_graph_menu(
		vgui_editor_node_graph* graph);
	VGUI_API void vgui_editor_node_graph_open_context_menu(
		vgui_editor_node_graph* graph, float content_x, float content_y);

	/* Search. */
	VGUI_API void vgui_editor_node_graph_set_search(vgui_editor_node_graph* graph,
		const char* text);
	VGUI_API size_t vgui_editor_node_graph_search(vgui_editor_node_graph* graph,
		char* out, size_t cap);
	VGUI_API void vgui_editor_node_graph_clear_search(
		vgui_editor_node_graph* graph);
	VGUI_API void vgui_editor_node_graph_focus_search(
		vgui_editor_node_graph* graph);
	VGUI_API vgui_widget* vgui_editor_node_graph_search_widget(
		vgui_editor_node_graph* graph);
	VGUI_API size_t vgui_editor_node_graph_match_count(
		vgui_editor_node_graph* graph);
	VGUI_API vgui_editor_object_id vgui_editor_node_graph_match_at(
		vgui_editor_node_graph* graph, size_t index);
	VGUI_API int vgui_editor_node_graph_node_matches(
		vgui_editor_node_graph* graph, vgui_editor_object_id node);
	/* Selects and centres the next matching node. */
	VGUI_API vgui_editor_object_id vgui_editor_node_graph_next_match(
		vgui_editor_node_graph* graph);

	VGUI_API void vgui_editor_node_graph_set_command_stack(
		vgui_editor_node_graph* graph, vgui_editor_command_stack* stack);

	/* View settings, grid/minimap/snap flags and selection as JSON. */
	VGUI_API size_t vgui_editor_node_graph_capture_state_json(
		vgui_editor_node_graph* graph, char* out, size_t cap);
	VGUI_API int vgui_editor_node_graph_apply_state_json(
		vgui_editor_node_graph* graph, const char* json);

	/* ------------------------------------------------------------------------- */
	/* 2D Canvas                                                                 */
	/* ------------------------------------------------------------------------- */

	typedef struct vgui_editor_canvas vgui_editor_canvas;

	/* Shared by the canvas and the viewport: the snap steps a transform gesture
	 * rounds to.  The kit reads it live, so a handle keeps a copy. */
	typedef struct vgui_editor_snap_settings {
		uint32_t struct_size;
		int enabled;
		int translate;
		int rotate;
		int scale;
		float translate_step;
		/* Degrees. */
		float rotate_step_deg;
		float scale_step;
	} vgui_editor_snap_settings;

	VGUI_API void vgui_editor_snap_settings_default(
		vgui_editor_snap_settings* settings);

	typedef enum vgui_editor_canvas_shape {
		VGUI_EDITOR_CANVAS_RECTANGLE = 0,
		VGUI_EDITOR_CANVAS_ELLIPSE,
		VGUI_EDITOR_CANVAS_DIAMOND
	} vgui_editor_canvas_shape;

	/* The eight resize directions around the frame, plus none and rotate. */
	typedef enum vgui_editor_canvas_handle {
		VGUI_EDITOR_CANVAS_HANDLE_NONE = 0,
		VGUI_EDITOR_CANVAS_HANDLE_NORTH_WEST,
		VGUI_EDITOR_CANVAS_HANDLE_NORTH,
		VGUI_EDITOR_CANVAS_HANDLE_NORTH_EAST,
		VGUI_EDITOR_CANVAS_HANDLE_EAST,
		VGUI_EDITOR_CANVAS_HANDLE_SOUTH_EAST,
		VGUI_EDITOR_CANVAS_HANDLE_SOUTH,
		VGUI_EDITOR_CANVAS_HANDLE_SOUTH_WEST,
		VGUI_EDITOR_CANVAS_HANDLE_WEST,
		VGUI_EDITOR_CANVAS_HANDLE_ROTATE
	} vgui_editor_canvas_handle;

	typedef enum vgui_editor_canvas_gesture {
		VGUI_EDITOR_CANVAS_GESTURE_NONE = 0,
		VGUI_EDITOR_CANVAS_GESTURE_PENDING_MOVE,
		VGUI_EDITOR_CANVAS_GESTURE_MOVE,
		VGUI_EDITOR_CANVAS_GESTURE_PENDING_MARQUEE,
		VGUI_EDITOR_CANVAS_GESTURE_MARQUEE,
		VGUI_EDITOR_CANVAS_GESTURE_RESIZE,
		VGUI_EDITOR_CANVAS_GESTURE_ROTATE,
		VGUI_EDITOR_CANVAS_GESTURE_PAN
	} vgui_editor_canvas_gesture;

	typedef struct vgui_editor_canvas_point {
		float x;
		float y;
	} vgui_editor_canvas_point;

	typedef struct vgui_editor_canvas_rect {
		float x;
		float y;
		float width;
		float height;
	} vgui_editor_canvas_rect;

	typedef struct vgui_editor_canvas_item {
		uint32_t struct_size;
		/* Stable id; 0 lets the canvas allocate one on add. */
		vgui_editor_object_id id;
		/* Unrotated rectangle in canvas coordinates; rotation is around its
		 * centre. */
		vgui_editor_canvas_rect bounds;
		/* Radians, clockwise in screen space. */
		float rotation;
		vgui_editor_canvas_shape shape;
		vgui_color fill;
		vgui_color border;
		vgui_color text_color;
		float border_width;
		float corner_radius;
		const char* label;
		int visible;
		/* Locked items are drawn but ignored by hit testing and gestures. */
		int locked;
	} vgui_editor_canvas_item;

	VGUI_API void vgui_editor_canvas_item_default(
		vgui_editor_canvas_item* item);

	typedef struct vgui_editor_canvas_view_state {
		uint32_t struct_size;
		/* Canvas coordinate shown at the content top-left. */
		float offset_x;
		float offset_y;
		/* Positive canvas units -> content pixels. */
		float zoom;
	} vgui_editor_canvas_view_state;

	VGUI_API void vgui_editor_canvas_view_state_default(
		vgui_editor_canvas_view_state* state);

	VGUI_API vgui_editor_canvas* vgui_editor_canvas_create(vgui_widget* parent);
	VGUI_API void vgui_editor_canvas_destroy(vgui_editor_canvas* canvas);
	VGUI_API vgui_widget* vgui_editor_canvas_widget(vgui_editor_canvas* canvas);
	VGUI_API void vgui_editor_canvas_focus(vgui_editor_canvas* canvas);
	VGUI_API int vgui_editor_canvas_focused(vgui_editor_canvas* canvas);
	VGUI_API int vgui_editor_canvas_built(vgui_editor_canvas* canvas);
	VGUI_API void vgui_editor_canvas_set_enabled(vgui_editor_canvas* canvas,
		int enabled);
	VGUI_API int vgui_editor_canvas_enabled(vgui_editor_canvas* canvas);

	/* Model.  add returns the id or VGUI_EDITOR_INVALID_OBJECT for a duplicate
	 * explicit id.  `out->label` borrows the kit's string, valid until the next
	 * mutation of this canvas. */
	VGUI_API vgui_editor_object_id vgui_editor_canvas_add_item(
		vgui_editor_canvas* canvas, const vgui_editor_canvas_item* item);
	VGUI_API int vgui_editor_canvas_update_item(vgui_editor_canvas* canvas,
		const vgui_editor_canvas_item* item);
	VGUI_API int vgui_editor_canvas_remove_item(vgui_editor_canvas* canvas,
		vgui_editor_object_id id);
	VGUI_API void vgui_editor_canvas_clear_items(vgui_editor_canvas* canvas);
	VGUI_API size_t vgui_editor_canvas_item_count(vgui_editor_canvas* canvas);
	VGUI_API int vgui_editor_canvas_get_item(vgui_editor_canvas* canvas,
		size_t index, vgui_editor_canvas_item* out);
	VGUI_API int vgui_editor_canvas_find_item(vgui_editor_canvas* canvas,
		vgui_editor_object_id id, vgui_editor_canvas_item* out);

	/* Selection. */
	VGUI_API void vgui_editor_canvas_set_selection(vgui_editor_canvas* canvas,
		const vgui_editor_object_id* ids, size_t id_count,
		vgui_editor_object_id primary);
	VGUI_API void vgui_editor_canvas_clear_selection(vgui_editor_canvas* canvas);
	VGUI_API size_t vgui_editor_canvas_selection_count(
		vgui_editor_canvas* canvas);
	VGUI_API vgui_editor_object_id vgui_editor_canvas_selection_id_at(
		vgui_editor_canvas* canvas, size_t index);
	VGUI_API vgui_editor_object_id vgui_editor_canvas_primary_selection(
		vgui_editor_canvas* canvas);
	VGUI_API int vgui_editor_canvas_is_selected(vgui_editor_canvas* canvas,
		vgui_editor_object_id id);
	VGUI_API void vgui_editor_canvas_set_multi_select(vgui_editor_canvas* canvas,
		int enabled);
	VGUI_API int vgui_editor_canvas_multi_select(vgui_editor_canvas* canvas);
	typedef void (*vgui_editor_canvas_selection_fn)(void* user,
		const vgui_editor_object_id* ids, size_t id_count);
	VGUI_API void vgui_editor_canvas_set_on_selection(
		vgui_editor_canvas* canvas, vgui_editor_canvas_selection_fn fn,
		void* user);
	typedef void (*vgui_editor_canvas_change_fn)(void* user);
	VGUI_API void vgui_editor_canvas_set_on_change(vgui_editor_canvas* canvas,
		vgui_editor_canvas_change_fn fn, void* user);

	/* View / zoom / pan. */
	VGUI_API void vgui_editor_canvas_get_view_state(vgui_editor_canvas* canvas,
		vgui_editor_canvas_view_state* out);
	VGUI_API void vgui_editor_canvas_set_view_state(vgui_editor_canvas* canvas,
		const vgui_editor_canvas_view_state* state);
	VGUI_API float vgui_editor_canvas_zoom(vgui_editor_canvas* canvas);
	VGUI_API void vgui_editor_canvas_zoom_at(vgui_editor_canvas* canvas,
		float content_x, float content_y, float factor);
	VGUI_API void vgui_editor_canvas_set_zoom_limits(vgui_editor_canvas* canvas,
		float minimum, float maximum);
	VGUI_API void vgui_editor_canvas_reset_view(vgui_editor_canvas* canvas);

	/* Grid. */
	VGUI_API void vgui_editor_canvas_set_grid_visible(vgui_editor_canvas* canvas,
		int visible);
	VGUI_API int vgui_editor_canvas_grid_visible(vgui_editor_canvas* canvas);
	VGUI_API void vgui_editor_canvas_set_grid_step(vgui_editor_canvas* canvas,
		float step);
	VGUI_API float vgui_editor_canvas_grid_step(vgui_editor_canvas* canvas);
	VGUI_API void vgui_editor_canvas_set_grid_pixel_range(
		vgui_editor_canvas* canvas, float minimum, float maximum);
	/* Spacing actually drawn at the current zoom. */
	VGUI_API float vgui_editor_canvas_effective_grid_step(
		vgui_editor_canvas* canvas);

	VGUI_API void vgui_editor_canvas_set_snap(vgui_editor_canvas* canvas,
		const vgui_editor_snap_settings* settings);
	VGUI_API void vgui_editor_canvas_get_snap(vgui_editor_canvas* canvas,
		vgui_editor_snap_settings* out);
	VGUI_API void vgui_editor_canvas_set_command_stack(
		vgui_editor_canvas* canvas, vgui_editor_command_stack* stack);

	/* Coordinates / geometry.  Screen points are content-local (inside the 1px
	 * border). */
	VGUI_API vgui_editor_canvas_point vgui_editor_canvas_screen_to_canvas(
		vgui_editor_canvas* canvas, float content_x, float content_y);
	VGUI_API vgui_editor_canvas_point vgui_editor_canvas_canvas_to_screen(
		vgui_editor_canvas* canvas, float canvas_x, float canvas_y);
	/* Rotated item bounds as an axis-aligned canvas rectangle. */
	VGUI_API vgui_editor_canvas_rect vgui_editor_canvas_item_bounds(
		vgui_editor_canvas* canvas, vgui_editor_object_id id);
	VGUI_API vgui_editor_canvas_rect vgui_editor_canvas_selection_bounds(
		vgui_editor_canvas* canvas);
	/* Content-local; VGUI_EDITOR_INVALID_OBJECT when nothing selectable is hit. */
	VGUI_API vgui_editor_object_id vgui_editor_canvas_hit_test(
		vgui_editor_canvas* canvas, float content_x, float content_y);
	VGUI_API vgui_editor_canvas_handle vgui_editor_canvas_handle_at(
		vgui_editor_canvas* canvas, float content_x, float content_y);

	/* Interaction state. */
	VGUI_API int vgui_editor_canvas_dragging(vgui_editor_canvas* canvas);
	VGUI_API vgui_editor_canvas_gesture vgui_editor_canvas_get_gesture(
		vgui_editor_canvas* canvas);
	VGUI_API vgui_editor_canvas_handle vgui_editor_canvas_active_handle(
		vgui_editor_canvas* canvas);
	VGUI_API vgui_editor_canvas_handle vgui_editor_canvas_hovered_handle(
		vgui_editor_canvas* canvas);
	VGUI_API void vgui_editor_canvas_cancel_gesture(vgui_editor_canvas* canvas);
	VGUI_API void vgui_editor_canvas_set_drag_threshold(
		vgui_editor_canvas* canvas, float pixels);
	VGUI_API float vgui_editor_canvas_drag_threshold(vgui_editor_canvas* canvas);
	VGUI_API void vgui_editor_canvas_set_min_item_size(
		vgui_editor_canvas* canvas, float size);

	/* ------------------------------------------------------------------------- */
	/* Code editor                                                               */
	/* ------------------------------------------------------------------------- */

	typedef struct vgui_editor_code_editor vgui_editor_code_editor;

	/* Ordering mirrors vgui_editor_kit::code_language. */
	typedef enum vgui_editor_code_language {
		VGUI_EDITOR_CODE_PLAIN = 0,
		VGUI_EDITOR_CODE_CPP,
		VGUI_EDITOR_CODE_CSHARP,
		VGUI_EDITOR_CODE_GLSL,
		VGUI_EDITOR_CODE_JSON,
		VGUI_EDITOR_CODE_LUA,
		VGUI_EDITOR_CODE_PYTHON,
		VGUI_EDITOR_CODE_INI,
		VGUI_EDITOR_CODE_MARKDOWN
	} vgui_editor_code_language;

	/* Ordering mirrors vgui_editor_kit::code_token_kind. */
	typedef enum vgui_editor_code_token_kind {
		VGUI_EDITOR_CODE_TOKEN_TEXT = 0,
		VGUI_EDITOR_CODE_TOKEN_KEYWORD,
		VGUI_EDITOR_CODE_TOKEN_TYPE,
		VGUI_EDITOR_CODE_TOKEN_STRING_LITERAL,
		VGUI_EDITOR_CODE_TOKEN_NUMBER,
		VGUI_EDITOR_CODE_TOKEN_COMMENT,
		VGUI_EDITOR_CODE_TOKEN_PREPROCESSOR,
		VGUI_EDITOR_CODE_TOKEN_FUNCTION,
		VGUI_EDITOR_CODE_TOKEN_OPERATOR,
		VGUI_EDITOR_CODE_TOKEN_PUNCTUATION,
		VGUI_EDITOR_CODE_TOKEN_ATTRIBUTE,
		VGUI_EDITOR_CODE_TOKEN_TAG,
		VGUI_EDITOR_CODE_TOKEN_HEADING,
		VGUI_EDITOR_CODE_TOKEN_HEADING_MARKER,
		VGUI_EDITOR_CODE_TOKEN_EMPHASIS,
		VGUI_EDITOR_CODE_TOKEN_STRONG,
		VGUI_EDITOR_CODE_TOKEN_STRIKE,
		VGUI_EDITOR_CODE_TOKEN_LINK,
		VGUI_EDITOR_CODE_TOKEN_LINK_URL,
		VGUI_EDITOR_CODE_TOKEN_INLINE_CODE,
		VGUI_EDITOR_CODE_TOKEN_QUOTE,
		VGUI_EDITOR_CODE_TOKEN_LIST_MARKER,
		VGUI_EDITOR_CODE_TOKEN_TABLE,
		VGUI_EDITOR_CODE_TOKEN_HORIZONTAL_RULE
	} vgui_editor_code_token_kind;

#define VGUI_EDITOR_CODE_TOKEN_KIND_COUNT 24

	/* Line is 0-based; column is a byte offset on a UTF-8 boundary. */
	typedef struct vgui_editor_code_position {
		int line;
		int column;
	} vgui_editor_code_position;

	typedef struct vgui_editor_code_span {
		/* Byte offsets within the line. */
		int start;
		int length;
		vgui_editor_code_token_kind kind;
		/* Non-zero overrides the token colour (used by rich log spans). */
		vgui_color color;
	} vgui_editor_code_span;

	typedef struct vgui_editor_code_match {
		int line;
		int column;
		int length;
	} vgui_editor_code_match;

	VGUI_API const char* vgui_editor_code_language_name(
		vgui_editor_code_language language);
	/* 0 when the name is not a known language. */
	VGUI_API int vgui_editor_code_language_from_name(const char* name,
		vgui_editor_code_language* out);
	/* Detects a language from a file extension; unknown extensions are plain. */
	VGUI_API vgui_editor_code_language vgui_editor_code_language_from_path(
		const char* path);
	VGUI_API const char* vgui_editor_code_token_kind_name(
		vgui_editor_code_token_kind kind);
	VGUI_API int vgui_editor_code_token_kind_from_name(const char* name,
		vgui_editor_code_token_kind* out);

	VGUI_API vgui_editor_code_editor* vgui_editor_code_editor_create(
		vgui_widget* parent);
	VGUI_API void vgui_editor_code_editor_destroy(
		vgui_editor_code_editor* editor);
	VGUI_API vgui_widget* vgui_editor_code_editor_widget(
		vgui_editor_code_editor* editor);
	/* Inner text widget and find bar (borrowed, owned by the editor). */
	VGUI_API vgui_widget* vgui_editor_code_editor_inner(
		vgui_editor_code_editor* editor);
	VGUI_API vgui_widget* vgui_editor_code_editor_find_bar(
		vgui_editor_code_editor* editor);
	VGUI_API int vgui_editor_code_editor_built(vgui_editor_code_editor* editor);
	VGUI_API void vgui_editor_code_editor_focus(vgui_editor_code_editor* editor);
	VGUI_API int vgui_editor_code_editor_focused(vgui_editor_code_editor* editor);

	/* Document.  set_text normalizes CRLF/CR to LF and resets caret, selection,
	 * scroll and undo history; append_text does not move the caret or record
	 * undo.  Text getters use the string-callback convention (bytes needed). */
	VGUI_API void vgui_editor_code_editor_set_text(
		vgui_editor_code_editor* editor, const char* text);
	VGUI_API size_t vgui_editor_code_editor_text(vgui_editor_code_editor* editor,
		char* out, size_t cap);
	VGUI_API void vgui_editor_code_editor_clear(vgui_editor_code_editor* editor);
	VGUI_API void vgui_editor_code_editor_append_text(
		vgui_editor_code_editor* editor, const char* text);
	VGUI_API size_t vgui_editor_code_editor_line_count(
		vgui_editor_code_editor* editor);
	/* Out-of-range line yields the empty string. */
	VGUI_API size_t vgui_editor_code_editor_line_text(
		vgui_editor_code_editor* editor, int line, char* out, size_t cap);
	VGUI_API int vgui_editor_code_editor_set_line_text(
		vgui_editor_code_editor* editor, int line, const char* text);
	/* Token spans, computed lazily.  Pass out == NULL to ask for the count. */
	VGUI_API size_t vgui_editor_code_editor_line_spans(
		vgui_editor_code_editor* editor, int line, vgui_editor_code_span* out,
		size_t cap);
	/* Replaces one line's spans and disables tokenization until it is edited. */
	VGUI_API int vgui_editor_code_editor_set_line_spans(
		vgui_editor_code_editor* editor, int line,
		const vgui_editor_code_span* spans, size_t count);
	/* False when the document is too long to highlight completely. */
	VGUI_API int vgui_editor_code_editor_highlight_valid(
		vgui_editor_code_editor* editor, size_t* count);

	/* Language / presentation. */
	VGUI_API void vgui_editor_code_editor_set_language(
		vgui_editor_code_editor* editor, vgui_editor_code_language language);
	VGUI_API vgui_editor_code_language vgui_editor_code_editor_language(
		vgui_editor_code_editor* editor);
	VGUI_API void vgui_editor_code_editor_set_language_from_path(
		vgui_editor_code_editor* editor, const char* path);
	VGUI_API void vgui_editor_code_editor_set_read_only(
		vgui_editor_code_editor* editor, int read_only);
	VGUI_API int vgui_editor_code_editor_read_only(
		vgui_editor_code_editor* editor);
	VGUI_API void vgui_editor_code_editor_set_show_line_numbers(
		vgui_editor_code_editor* editor, int show);
	VGUI_API int vgui_editor_code_editor_show_line_numbers(
		vgui_editor_code_editor* editor);
	VGUI_API void vgui_editor_code_editor_set_tab_size(
		vgui_editor_code_editor* editor, int tab_size);
	VGUI_API int vgui_editor_code_editor_tab_size(
		vgui_editor_code_editor* editor);
	VGUI_API void vgui_editor_code_editor_set_font_size(
		vgui_editor_code_editor* editor, float size);
	VGUI_API float vgui_editor_code_editor_font_size(
		vgui_editor_code_editor* editor);

	/* Token colours and palettes. */
	VGUI_API void vgui_editor_code_editor_set_token_color(
		vgui_editor_code_editor* editor, vgui_editor_code_token_kind kind,
		vgui_color color);
	VGUI_API vgui_color vgui_editor_code_editor_token_color(
		vgui_editor_code_editor* editor, vgui_editor_code_token_kind kind);
	VGUI_API void vgui_editor_code_editor_reset_token_colors(
		vgui_editor_code_editor* editor);
	/* Theme JSON: { "version":1, "name":"...", "background":"#FFFFFF",
	 * "colors":{ "keyword":"#C678DD", ... } }.  0 (and the reason in
	 * vgui_editor_last_error()) when the text does not parse. */
	VGUI_API int vgui_editor_code_editor_set_theme_json(
		vgui_editor_code_editor* editor, const char* json);
	VGUI_API size_t vgui_editor_code_editor_theme_json(
		vgui_editor_code_editor* editor, char* out, size_t cap);
	/* Built-in presets: "dark", "light", "solarized", "high_contrast". */
	VGUI_API int vgui_editor_code_editor_set_theme_preset(
		vgui_editor_code_editor* editor, const char* name);
	VGUI_API size_t vgui_editor_code_editor_theme_preset_count(void);
	VGUI_API size_t vgui_editor_code_editor_theme_preset_name(size_t index,
		char* out, size_t cap);

	/* Editing.  0 when the editor rejected the change (read-only, empty
	 * selection). */
	VGUI_API int vgui_editor_code_editor_insert_text(
		vgui_editor_code_editor* editor, const char* text);
	VGUI_API int vgui_editor_code_editor_backspace(
		vgui_editor_code_editor* editor);
	VGUI_API int vgui_editor_code_editor_delete_forward(
		vgui_editor_code_editor* editor);
	VGUI_API int vgui_editor_code_editor_newline(vgui_editor_code_editor* editor);
	VGUI_API int vgui_editor_code_editor_indent_selection(
		vgui_editor_code_editor* editor);
	VGUI_API int vgui_editor_code_editor_outdent_selection(
		vgui_editor_code_editor* editor);
	VGUI_API int vgui_editor_code_editor_undo(vgui_editor_code_editor* editor);
	VGUI_API int vgui_editor_code_editor_redo(vgui_editor_code_editor* editor);
	VGUI_API int vgui_editor_code_editor_can_undo(
		vgui_editor_code_editor* editor);
	VGUI_API int vgui_editor_code_editor_can_redo(
		vgui_editor_code_editor* editor);
	/* Without a stack the editor keeps its own local history. */
	VGUI_API void vgui_editor_code_editor_set_command_stack(
		vgui_editor_code_editor* editor, vgui_editor_command_stack* stack);

	/* Caret / selection. */
	VGUI_API void vgui_editor_code_editor_get_caret(
		vgui_editor_code_editor* editor, vgui_editor_code_position* out);
	VGUI_API void vgui_editor_code_editor_set_caret(
		vgui_editor_code_editor* editor, int line, int column);
	VGUI_API int vgui_editor_code_editor_caret_line(
		vgui_editor_code_editor* editor);
	VGUI_API int vgui_editor_code_editor_caret_column(
		vgui_editor_code_editor* editor);
	VGUI_API void vgui_editor_code_editor_selection_anchor(
		vgui_editor_code_editor* editor, vgui_editor_code_position* out);
	VGUI_API int vgui_editor_code_editor_has_selection(
		vgui_editor_code_editor* editor);
	VGUI_API void vgui_editor_code_editor_set_selection(
		vgui_editor_code_editor* editor, vgui_editor_code_position anchor,
		vgui_editor_code_position caret);
	VGUI_API void vgui_editor_code_editor_select_all(
		vgui_editor_code_editor* editor);
	VGUI_API void vgui_editor_code_editor_clear_selection(
		vgui_editor_code_editor* editor);
	VGUI_API size_t vgui_editor_code_editor_selected_text(
		vgui_editor_code_editor* editor, char* out, size_t cap);
	/* Runs after the caret moves (typing, mouse, find, programmatic). */
	typedef void (*vgui_editor_code_caret_fn)(void* user,
		vgui_editor_code_position position);
	VGUI_API void vgui_editor_code_editor_set_on_caret(
		vgui_editor_code_editor* editor, vgui_editor_code_caret_fn fn,
		void* user);

	/* Clipboard.  Without handlers the editor bridges through a hidden edit
	 * widget so copy/paste uses the platform clipboard. */
	typedef size_t (*vgui_editor_clipboard_get_fn)(void* user, char* out,
		size_t cap);
	typedef void (*vgui_editor_clipboard_set_fn)(void* user, const char* text);
	VGUI_API void vgui_editor_code_editor_set_clipboard_handlers(
		vgui_editor_code_editor* editor, vgui_editor_clipboard_get_fn getter,
		vgui_editor_clipboard_set_fn setter, void* user,
		vgui_editor_release_fn release_user);
	VGUI_API void vgui_editor_code_editor_clear_clipboard_handlers(
		vgui_editor_code_editor* editor);
	VGUI_API int vgui_editor_code_editor_copy(vgui_editor_code_editor* editor);
	VGUI_API int vgui_editor_code_editor_cut(vgui_editor_code_editor* editor);
	VGUI_API int vgui_editor_code_editor_paste(vgui_editor_code_editor* editor);

	/* Find / replace. */
	VGUI_API void vgui_editor_code_editor_set_find_query(
		vgui_editor_code_editor* editor, const char* text);
	VGUI_API size_t vgui_editor_code_editor_find_query(
		vgui_editor_code_editor* editor, char* out, size_t cap);
	VGUI_API void vgui_editor_code_editor_set_find_case_sensitive(
		vgui_editor_code_editor* editor, int sensitive);
	VGUI_API int vgui_editor_code_editor_find_case_sensitive(
		vgui_editor_code_editor* editor);
	VGUI_API void vgui_editor_code_editor_open_find(
		vgui_editor_code_editor* editor, int replace_mode);
	VGUI_API void vgui_editor_code_editor_close_find(
		vgui_editor_code_editor* editor);
	VGUI_API int vgui_editor_code_editor_find_visible(
		vgui_editor_code_editor* editor);
	VGUI_API int vgui_editor_code_editor_find_next(vgui_editor_code_editor* editor);
	VGUI_API int vgui_editor_code_editor_find_previous(
		vgui_editor_code_editor* editor);
	/* Searches the whole document; the results also feed match_at(). */
	VGUI_API size_t vgui_editor_code_editor_find_all(
		vgui_editor_code_editor* editor, const char* needle,
		int case_sensitive);
	VGUI_API size_t vgui_editor_code_editor_match_count(
		vgui_editor_code_editor* editor);
	VGUI_API int vgui_editor_code_editor_match_at(
		vgui_editor_code_editor* editor, size_t index,
		vgui_editor_code_match* out);
	VGUI_API int vgui_editor_code_editor_active_match(vgui_editor_code_editor* editor);
	VGUI_API int vgui_editor_code_editor_replace_current(
		vgui_editor_code_editor* editor, const char* replacement);
	VGUI_API int vgui_editor_code_editor_replace_all(
		vgui_editor_code_editor* editor, const char* needle,
		const char* replacement, int case_sensitive);

	/* Rich log mode (same level enum as the console). */
	VGUI_API void vgui_editor_code_editor_append_log(
		vgui_editor_code_editor* editor, vgui_editor_log_level level,
		const char* text, const char* timestamp);
	VGUI_API void vgui_editor_code_editor_set_level_color(
		vgui_editor_code_editor* editor, vgui_editor_log_level level,
		vgui_color color);
	VGUI_API vgui_color vgui_editor_code_editor_level_color(
		vgui_editor_code_editor* editor, vgui_editor_log_level level);
	VGUI_API void vgui_editor_code_editor_set_auto_scroll(
		vgui_editor_code_editor* editor, int enabled);
	VGUI_API int vgui_editor_code_editor_auto_scroll(
		vgui_editor_code_editor* editor);
	VGUI_API void vgui_editor_code_editor_set_max_lines(
		vgui_editor_code_editor* editor, size_t max_lines);
	VGUI_API size_t vgui_editor_code_editor_max_lines(
		vgui_editor_code_editor* editor);
	VGUI_API size_t vgui_editor_code_editor_dropped_lines(
		vgui_editor_code_editor* editor);

	/* Incremental loading.  open_file streams path into an empty document and
	 * leaves the previous text untouched when the file cannot be opened (see
	 * vgui_editor_last_error()). */
	VGUI_API int vgui_editor_code_editor_open_file(
		vgui_editor_code_editor* editor, const char* path);
	VGUI_API void vgui_editor_code_editor_begin_incremental_load(
		vgui_editor_code_editor* editor, const char* text);
	VGUI_API int vgui_editor_code_editor_loading(vgui_editor_code_editor* editor);
	VGUI_API float vgui_editor_code_editor_load_progress(
		vgui_editor_code_editor* editor);
	VGUI_API void vgui_editor_code_editor_set_load_chunk_size(
		vgui_editor_code_editor* editor, size_t bytes);
	VGUI_API size_t vgui_editor_code_editor_load_chunk_size(
		vgui_editor_code_editor* editor);
	/* Consumes one chunk; 1 while more data remains. */
	VGUI_API int vgui_editor_code_editor_process_load_step(
		vgui_editor_code_editor* editor);
	VGUI_API void vgui_editor_code_editor_cancel_load(
		vgui_editor_code_editor* editor);
	VGUI_API void vgui_editor_code_editor_tick(vgui_editor_code_editor* editor,
		float delta_time);

	VGUI_API void vgui_editor_code_editor_set_scroll(
		vgui_editor_code_editor* editor, float x, float y);
	VGUI_API float vgui_editor_code_editor_scroll_x(
		vgui_editor_code_editor* editor);
	VGUI_API float vgui_editor_code_editor_scroll_y(
		vgui_editor_code_editor* editor);
	VGUI_API void vgui_editor_code_editor_scroll_to_caret(
		vgui_editor_code_editor* editor);

	/* Language, flags, caret and scroll as JSON; 0 with a message in
	 * vgui_editor_last_error() when the payload is invalid. */
	VGUI_API size_t vgui_editor_code_editor_capture_state_json(
		vgui_editor_code_editor* editor, char* out, size_t cap);
	VGUI_API int vgui_editor_code_editor_apply_state_json(
		vgui_editor_code_editor* editor, const char* json);
	typedef void (*vgui_editor_code_change_fn)(void* user);
	VGUI_API void vgui_editor_code_editor_set_on_change(
		vgui_editor_code_editor* editor, vgui_editor_code_change_fn fn,
		void* user);

	/* ------------------------------------------------------------------------- */
	/* Markdown preview                                                          */
	/* ------------------------------------------------------------------------- */

	typedef struct vgui_editor_markdown vgui_editor_markdown;

	typedef enum vgui_editor_md_media_kind {
		VGUI_EDITOR_MD_IMAGE = 0,
		VGUI_EDITOR_MD_VIDEO
	} vgui_editor_md_media_kind;

	typedef enum vgui_editor_md_media_status {
		VGUI_EDITOR_MD_PENDING = 0,
		VGUI_EDITOR_MD_READY,
		VGUI_EDITOR_MD_FAILED
	} vgui_editor_md_media_status;

	/* Media state owned by the host.  `texture` is borrowed and must stay alive
	 * while it is returned; the view never destroys textures.  `error` is copied
	 * by the kit, so it may be a temporary.  aspect is width / height; 0 falls
	 * back to the texture dimensions, then 16:9. */
	typedef struct vgui_editor_md_media {
		uint32_t struct_size;
		vgui_editor_md_media_status status;
		vgui_texture* texture;
		int pixel_width;
		int pixel_height;
		float aspect;
		const char* error;
	} vgui_editor_md_media;

	VGUI_API void vgui_editor_md_media_default(vgui_editor_md_media* media);

	typedef void (*vgui_editor_md_resolve_fn)(void* user, const char* source,
		vgui_editor_md_media_kind kind, vgui_editor_md_media* out);
	typedef void (*vgui_editor_md_clipboard_fn)(void* user, const char* text);

	VGUI_API vgui_editor_markdown* vgui_editor_markdown_create(
		vgui_widget* parent);
	VGUI_API void vgui_editor_markdown_destroy(vgui_editor_markdown* view);
	VGUI_API vgui_widget* vgui_editor_markdown_widget(
		vgui_editor_markdown* view);
	VGUI_API int vgui_editor_markdown_built(vgui_editor_markdown* view);

	/* Document.  Input beyond 1 MiB is truncated on a UTF-8 boundary and
	 * reflected by truncated(). */
	VGUI_API void vgui_editor_markdown_set_text(vgui_editor_markdown* view,
		const char* text);
	VGUI_API size_t vgui_editor_markdown_text(vgui_editor_markdown* view,
		char* out, size_t cap);
	VGUI_API int vgui_editor_markdown_truncated(vgui_editor_markdown* view);
	/* Plain text as Ctrl+C would produce (markup removed). */
	VGUI_API size_t vgui_editor_markdown_rendered_text(
		vgui_editor_markdown* view, char* out, size_t cap);

	VGUI_API void vgui_editor_markdown_set_font_size(vgui_editor_markdown* view,
		float size);
	VGUI_API float vgui_editor_markdown_font_size(vgui_editor_markdown* view);
	/* Same theme JSON document as the code editor. */
	VGUI_API int vgui_editor_markdown_set_theme_json(
		vgui_editor_markdown* view, const char* json);
	VGUI_API size_t vgui_editor_markdown_theme_json(vgui_editor_markdown* view,
		char* out, size_t cap);
	VGUI_API int vgui_editor_markdown_set_theme_preset(
		vgui_editor_markdown* view, const char* name);

	/* Scroll (effective-pixel coordinates). */
	VGUI_API float vgui_editor_markdown_content_height(
		vgui_editor_markdown* view);
	VGUI_API void vgui_editor_markdown_set_scroll(vgui_editor_markdown* view,
		float y);
	VGUI_API float vgui_editor_markdown_scroll_y(vgui_editor_markdown* view);
	VGUI_API void vgui_editor_markdown_scroll_to_block(
		vgui_editor_markdown* view, int block, int center);
	VGUI_API void vgui_editor_markdown_scroll_to_source_line(
		vgui_editor_markdown* view, int line, int center);

	/* Block <-> source-line mapping (both 0-based). */
	VGUI_API int vgui_editor_markdown_block_count(vgui_editor_markdown* view);
	VGUI_API int vgui_editor_markdown_block_source_line(
		vgui_editor_markdown* view, int block);
	VGUI_API int vgui_editor_markdown_block_source_end_line(
		vgui_editor_markdown* view, int block);
	/* Block containing a source line, or -1. */
	VGUI_API int vgui_editor_markdown_block_for_source_line(
		vgui_editor_markdown* view, int line);
	/* Source line at local coordinates, clamped to the document. */
	VGUI_API int vgui_editor_markdown_source_line_at(
		vgui_editor_markdown* view, float x, float y);
	/* Tints the block covering this source line; -1 clears it. */
	VGUI_API void vgui_editor_markdown_set_active_source_line(
		vgui_editor_markdown* view, int line);
	VGUI_API int vgui_editor_markdown_active_source_line(
		vgui_editor_markdown* view);

	/* Selection and copy. */
	VGUI_API int vgui_editor_markdown_has_selection(vgui_editor_markdown* view);
	VGUI_API void vgui_editor_markdown_select_all(vgui_editor_markdown* view);
	VGUI_API void vgui_editor_markdown_clear_selection(
		vgui_editor_markdown* view);
	VGUI_API size_t vgui_editor_markdown_selected_text(
		vgui_editor_markdown* view, char* out, size_t cap);
	VGUI_API int vgui_editor_markdown_copy(vgui_editor_markdown* view);
	/* Replaces the system-clipboard bridge. */
	VGUI_API void vgui_editor_markdown_set_clipboard_handler(
		vgui_editor_markdown* view, vgui_editor_md_clipboard_fn fn, void* user,
		vgui_editor_release_fn release_user);

	/* Media.  The resolver runs for every media item when the document changes
	 * and again from invalidate_media(); it should return cached state quickly,
	 * and the host calls invalidate_media(source) once an async load finishes. */
	VGUI_API void vgui_editor_markdown_set_media_resolver(
		vgui_editor_markdown* view, vgui_editor_md_resolve_fn fn, void* user,
		vgui_editor_release_fn release_user);
	/* NULL source re-resolves every item. */
	VGUI_API void vgui_editor_markdown_invalidate_media(
		vgui_editor_markdown* view, const char* source);
	VGUI_API void vgui_editor_markdown_set_max_media_height(
		vgui_editor_markdown* view, float logical_height);
	VGUI_API float vgui_editor_markdown_max_media_height(
		vgui_editor_markdown* view);

	/* ------------------------------------------------------------------------- */
	/* Thumbnail cache                                                           */
	/* ------------------------------------------------------------------------- */

	typedef struct vgui_editor_thumbnails vgui_editor_thumbnails;

	/* Ordering mirrors vgui_editor_kit::thumbnail_cache::state. */
	typedef enum vgui_editor_thumbnail_state {
		VGUI_EDITOR_THUMB_MISSING = 0,
		VGUI_EDITOR_THUMB_QUEUED,
		VGUI_EDITOR_THUMB_LOADING,
		VGUI_EDITOR_THUMB_READY,
		VGUI_EDITOR_THUMB_FAILED
	} vgui_editor_thumbnail_state;

	typedef enum vgui_editor_thumbnail_priority {
		VGUI_EDITOR_THUMB_LOW = 0,
		VGUI_EDITOR_THUMB_HIGH
	} vgui_editor_thumbnail_priority;

	typedef struct vgui_editor_thumbnail_options {
		uint32_t struct_size;
		/* Maximum uploaded textures kept in the LRU; keep this larger than the
		 * visible range so on-screen items are never evicted. */
		size_t memory_limit;
		/* Total attempts including the first load.  1 disables retries. */
		int max_attempts;
		float retry_delay;
		/* 0 keeps the callbacks but bypasses the disk cache. */
		int disk_cache;
	} vgui_editor_thumbnail_options;

	VGUI_API void vgui_editor_thumbnail_options_default(
		vgui_editor_thumbnail_options* options);

	/* Cache key plus the display name/type a loader may branch on. */
	typedef struct vgui_editor_thumbnail_request {
		uint32_t struct_size;
		const char* key;
		const char* name;
		const char* type;
	} vgui_editor_thumbnail_request;

	VGUI_API void vgui_editor_thumbnail_request_default(
		vgui_editor_thumbnail_request* request);

	/* CPU pixels.  A loader points `pixels` at memory it owns; the library
	 * copies the bytes once the handler has returned, so `pixels` and `error`
	 * must still be alive at that point - host-owned static or heap storage, a
	 * string literal, or the worker's persistent result slot, never a handler
	 * local. */
	typedef struct vgui_editor_thumbnail_image {
		uint32_t struct_size;
		int width;
		int height;
		/* Bytes per row; 0 means width * bytes-per-pixel of `format`. */
		int stride;
		vgui_texture_format format;
		const unsigned char* pixels;
		size_t pixel_count;
		const char* error;
	} vgui_editor_thumbnail_image;

	VGUI_API void vgui_editor_thumbnail_image_default(
		vgui_editor_thumbnail_image* image);

	typedef struct vgui_editor_thumbnail_stats {
		uint32_t struct_size;
		size_t memory_entries;
		size_t memory_limit;
		size_t pending;
		unsigned long long memory_hits;
		unsigned long long disk_hits;
		unsigned long long loads;
		unsigned long long uploads;
		unsigned long long failures;
		unsigned long long retries;
		unsigned long long cancellations;
	} vgui_editor_thumbnail_stats;

	/* Runs on a worker thread: never touch vgui or the cache from here.  Return
	 * 0 (optionally setting out->error) to schedule a retry. */
	typedef int (*vgui_editor_thumbnail_load_fn)(void* user,
		const vgui_editor_thumbnail_request* request,
		vgui_editor_thumbnail_image* out);
	typedef int (*vgui_editor_thumbnail_disk_read_fn)(void* user,
		const vgui_editor_thumbnail_request* request,
		vgui_editor_thumbnail_image* out);
	/* `image` borrows the kit's bytes for the duration of the call. */
	typedef int (*vgui_editor_thumbnail_disk_write_fn)(void* user,
		const vgui_editor_thumbnail_request* request,
		const vgui_editor_thumbnail_image* image);

	/* A cache created here is standalone; the asset browser exposes its own
	 * cache through vgui_editor_asset_browser_thumbnails() (borrowed). */
	VGUI_API vgui_editor_thumbnails* vgui_editor_thumbnails_create(
		vgui_context* ui);
	VGUI_API void vgui_editor_thumbnails_destroy(vgui_editor_thumbnails* cache);
	VGUI_API void vgui_editor_thumbnails_set_context(
		vgui_editor_thumbnails* cache, vgui_context* ui);
	VGUI_API vgui_context* vgui_editor_thumbnails_context(
		vgui_editor_thumbnails* cache);
	VGUI_API void vgui_editor_thumbnails_set_options(
		vgui_editor_thumbnails* cache,
		const vgui_editor_thumbnail_options* options);
	VGUI_API void vgui_editor_thumbnails_get_options(
		vgui_editor_thumbnails* cache, vgui_editor_thumbnail_options* out);
	VGUI_API void vgui_editor_thumbnails_set_load_handler(
		vgui_editor_thumbnails* cache, vgui_editor_thumbnail_load_fn fn,
		void* user, vgui_editor_release_fn release_user);
	VGUI_API void vgui_editor_thumbnails_set_disk_cache(
		vgui_editor_thumbnails* cache, vgui_editor_thumbnail_disk_read_fn read,
		vgui_editor_thumbnail_disk_write_fn write, void* user,
		vgui_editor_release_fn release_user);
	VGUI_API void vgui_editor_thumbnails_clear_disk_cache(
		vgui_editor_thumbnails* cache);
	/* True when at least one of the loader or disk-read callbacks is set. */
	VGUI_API int vgui_editor_thumbnails_configured(vgui_editor_thumbnails* cache);
	/* A standalone cache is passive: the host drives it once per frame.  tick()
	 * advances retry timers and drains completed jobs, poll() only drains and
	 * uploads; both return non-zero when at least one key changed state.  idle()
	 * is non-zero when nothing is queued or in flight.  A cache reached through
	 * vgui_editor_asset_browser_thumbnails() is driven by the browser. */
	VGUI_API int vgui_editor_thumbnails_tick(vgui_editor_thumbnails* cache,
		double delta_time);
	VGUI_API int vgui_editor_thumbnails_poll(vgui_editor_thumbnails* cache);
	VGUI_API int vgui_editor_thumbnails_idle(vgui_editor_thumbnails* cache);
	VGUI_API void vgui_editor_thumbnails_enqueue(vgui_editor_thumbnails* cache,
		const vgui_editor_thumbnail_request* request,
		vgui_editor_thumbnail_priority priority);
	/* Cancels a queued job and invalidates an in-flight result; a cached texture
	 * is kept. */
	VGUI_API void vgui_editor_thumbnails_cancel(vgui_editor_thumbnails* cache,
		const char* key);
	VGUI_API void vgui_editor_thumbnails_cancel_all(
		vgui_editor_thumbnails* cache);
	/* Restarts a failed key from attempt one / drops the entry and its texture. */
	VGUI_API void vgui_editor_thumbnails_retry(vgui_editor_thumbnails* cache,
		const char* key);
	VGUI_API void vgui_editor_thumbnails_erase(vgui_editor_thumbnails* cache,
		const char* key);
	/* lookup() does not touch the LRU; texture() does and uploads a finished
	 * job.  Both return NULL when nothing is cached. */
	VGUI_API vgui_texture* vgui_editor_thumbnails_lookup(
		vgui_editor_thumbnails* cache, const char* key);
	VGUI_API vgui_texture* vgui_editor_thumbnails_texture(
		vgui_editor_thumbnails* cache, const char* key);
	VGUI_API vgui_editor_thumbnail_state vgui_editor_thumbnails_state(
		vgui_editor_thumbnails* cache, const char* key);
	VGUI_API size_t vgui_editor_thumbnails_error(
		vgui_editor_thumbnails* cache, const char* key, char* out, size_t cap);
	VGUI_API void vgui_editor_thumbnails_get_stats(
		vgui_editor_thumbnails* cache, vgui_editor_thumbnail_stats* out);
	VGUI_API void vgui_editor_thumbnails_clear(vgui_editor_thumbnails* cache);

	/* ------------------------------------------------------------------------- */
	/* Asset browser                                                             */
	/* ------------------------------------------------------------------------- */

	typedef struct vgui_editor_asset_browser vgui_editor_asset_browser;

	typedef enum vgui_editor_asset_view_mode {
		VGUI_EDITOR_ASSET_LIST = 0,
		VGUI_EDITOR_ASSET_GRID
	} vgui_editor_asset_view_mode;

	/* A06 operation kinds and the fixed error categories the UI words come from.
	 * Both orderings mirror the kit's enumerations. */
	typedef enum vgui_editor_asset_operation {
		VGUI_EDITOR_ASSET_OP_RENAME = 0,
		VGUI_EDITOR_ASSET_OP_REMOVE,
		VGUI_EDITOR_ASSET_OP_DUPLICATE,
		VGUI_EDITOR_ASSET_OP_MOVE,
		VGUI_EDITOR_ASSET_OP_REIMPORT
	} vgui_editor_asset_operation;

	typedef enum vgui_editor_asset_operation_error {
		VGUI_EDITOR_ASSET_ERR_NONE = 0,
		VGUI_EDITOR_ASSET_ERR_NO_SELECTION,
		VGUI_EDITOR_ASSET_ERR_INVALID_NAME,
		VGUI_EDITOR_ASSET_ERR_INVALID_TARGET,
		VGUI_EDITOR_ASSET_ERR_NOT_FOUND,
		VGUI_EDITOR_ASSET_ERR_CONFLICT,
		VGUI_EDITOR_ASSET_ERR_READ_ONLY,
		VGUI_EDITOR_ASSET_ERR_IN_USE,
		VGUI_EDITOR_ASSET_ERR_UNSUPPORTED,
		VGUI_EDITOR_ASSET_ERR_IO_ERROR
	} vgui_editor_asset_operation_error;

	/* Menu / shortcut verbs dispatched by perform_action(). */
	typedef enum vgui_editor_asset_action {
		VGUI_EDITOR_ASSET_ACTION_RENAME = 0,
		VGUI_EDITOR_ASSET_ACTION_DUPLICATE,
		VGUI_EDITOR_ASSET_ACTION_REMOVE,
		VGUI_EDITOR_ASSET_ACTION_MOVE,
		VGUI_EDITOR_ASSET_ACTION_REIMPORT,
		VGUI_EDITOR_ASSET_ACTION_IMPORT,
		VGUI_EDITOR_ASSET_ACTION_DEPENDENCIES
	} vgui_editor_asset_action;

	/* One asset row.  `name` and `type` are borrowed by the kit after a callback
	 * fills them, and returned as borrowed text from getters. */
	typedef struct vgui_editor_asset_entry {
		uint32_t struct_size;
		const char* name;
		const char* type;
		vgui_texture* thumbnail;
		vgui_item_id id;
	} vgui_editor_asset_entry;

	VGUI_API void vgui_editor_asset_entry_default(vgui_editor_asset_entry* info);

	/* One breadcrumb segment; `id` is passed back to the navigate handler. */
	typedef struct vgui_editor_asset_path_item {
		uint32_t struct_size;
		const char* label;
		const char* id;
	} vgui_editor_asset_path_item;

	VGUI_API void vgui_editor_asset_path_item_default(
		vgui_editor_asset_path_item* item);

	/* Optional read-only virtual model.  count and item_at are required;
	 * item_at runs only for indices in the visible range.  Without id_at the ids
	 * default to index + 1; provide index_of for O(1) lookups on huge
	 * directories. */
	typedef struct vgui_editor_asset_source {
		uint32_t struct_size;
		int (*count)(void* user);
		int (*item_at)(void* user, int index, vgui_editor_asset_entry* out);
		vgui_item_id (*id_at)(void* user, int index);
		int (*index_of)(void* user, vgui_item_id item);
		void* user;
		vgui_editor_release_fn release_user;
	} vgui_editor_asset_source;

	VGUI_API void vgui_editor_asset_source_default(
		vgui_editor_asset_source* source);

	/* Contents of one directory, produced by the navigate handler.  `path`
	 * becomes the breadcrumb strip (the last segment is the current location).
	 * Provide either a source or an items array; the source wins when both are
	 * set.  The library reads the arrays after the handler returns, so they must
	 * be storage the host owns (static or heap) rather than callback locals;
	 * `source` avoids the constraint because its rows are queried one at a time. */
	typedef struct vgui_editor_asset_directory {
		uint32_t struct_size;
		const vgui_editor_asset_path_item* path;
		size_t path_count;
		const vgui_editor_asset_entry* items;
		size_t item_count;
		const vgui_editor_asset_source* source;
		/* Customises the zero-item placeholder; NULL keeps the default. */
		const char* empty_text;
	} vgui_editor_asset_directory;

	VGUI_API void vgui_editor_asset_directory_default(
		vgui_editor_asset_directory* directory);

	/* A06: the host owns the database and the files.  Fill `result` and return 0
	 * on failure (set error/message).  Return 1 once the database has been
	 * updated; the browser then reloads the current directory.  Set both undo
	 * and redo plus undoable to record the batch as one command; leave undoable
	 * 0 when the operation cannot be reversed.  `select` / `created` borrow the
	 * kit's storage for the duration of the callback. */
	typedef int (*vgui_editor_asset_undo_fn)(void* user);

	typedef struct vgui_editor_asset_operation_request {
		uint32_t struct_size;
		vgui_editor_asset_operation kind;
		const vgui_item_id* items;
		size_t item_count;
		/* rename target / current directory / move destination. */
		const char* name;
		const char* path_id;
		const char* target_path_id;
	} vgui_editor_asset_operation_request;

	typedef struct vgui_editor_asset_operation_result {
		uint32_t struct_size;
		vgui_editor_asset_operation_error error;
		/* May name the offending asset.  The library copies it right after the
		 * handler returns, so it must not be a callback-local buffer. */
		const char* message;
		int undoable;
		vgui_editor_asset_undo_fn undo;
		vgui_editor_asset_undo_fn redo;
		void* user;
		vgui_editor_release_fn release_user;
		/* Selection to apply after success / ids created by a duplicate.  Both
		 * arrays are read right after the handler returns, so they need host-owned
		 * storage (static or heap), not callback locals. */
		const vgui_item_id* select;
		size_t select_count;
		const vgui_item_id* created;
		size_t created_count;
	} vgui_editor_asset_operation_result;

	VGUI_API void vgui_editor_asset_operation_result_default(
		vgui_editor_asset_operation_result* result);

	typedef struct vgui_editor_asset_move_target {
		uint32_t struct_size;
		const char* label;
		const char* path_id;
	} vgui_editor_asset_move_target;

	VGUI_API void vgui_editor_asset_move_target_default(
		vgui_editor_asset_move_target* target);

	typedef struct vgui_editor_asset_dependency {
		uint32_t struct_size;
		const char* name;
		const char* type;
		/* Optional location for display. */
		const char* path_id;
		vgui_item_id id;
	} vgui_editor_asset_dependency;

	VGUI_API void vgui_editor_asset_dependency_default(
		vgui_editor_asset_dependency* dependency);

	typedef int (*vgui_editor_asset_navigate_fn)(void* user, const char* path_id,
		vgui_editor_asset_directory* out);
	typedef int (*vgui_editor_asset_rename_fn)(void* user, vgui_item_id item,
		const char* name);
	typedef int (*vgui_editor_asset_operation_fn)(void* user,
		const vgui_editor_asset_operation_request* request,
		vgui_editor_asset_operation_result* result);
	/* Fills `out` (capacity `out_count`) with the move destinations and returns
	 * how many there are. */
	typedef size_t (*vgui_editor_asset_move_targets_fn)(void* user,
		vgui_editor_asset_move_target* out, size_t out_count);
	typedef size_t (*vgui_editor_asset_dependency_fn)(void* user,
		vgui_item_id item, vgui_editor_asset_dependency* out, size_t out_count);
	typedef int (*vgui_editor_asset_import_fn)(void* user,
		const char* const* files, size_t file_count, const char* path_id);

	VGUI_API vgui_editor_asset_browser* vgui_editor_asset_browser_create(
		vgui_widget* parent);
	VGUI_API void vgui_editor_asset_browser_destroy(
		vgui_editor_asset_browser* browser);
	VGUI_API vgui_widget* vgui_editor_asset_browser_widget(
		vgui_editor_asset_browser* browser);

	/* Full model helpers (mutually exclusive with a virtual source). */
	VGUI_API void vgui_editor_asset_browser_clear(
		vgui_editor_asset_browser* browser);
	VGUI_API vgui_item_id vgui_editor_asset_browser_add_item(
		vgui_editor_asset_browser* browser, const char* text,
		const char* detail);
	VGUI_API int vgui_editor_asset_browser_set_item_thumbnail(
		vgui_editor_asset_browser* browser, vgui_item_id item,
		vgui_texture* texture);
	VGUI_API vgui_texture* vgui_editor_asset_browser_item_thumbnail(
		vgui_editor_asset_browser* browser, vgui_item_id item);
	/* Host textures always win over the automatic ones.  The browser drives
	 * requests from its visible range inside tick(); the cache handle is
	 * borrowed and owned by the browser. */
	VGUI_API vgui_editor_thumbnails* vgui_editor_asset_browser_thumbnails(
		vgui_editor_asset_browser* browser);
	VGUI_API size_t vgui_editor_asset_browser_thumbnail_key(
		vgui_editor_asset_browser* browser, vgui_item_id item, char* out,
		size_t cap);
	VGUI_API vgui_editor_thumbnail_state vgui_editor_asset_browser_thumbnail_state(
		vgui_editor_asset_browser* browser, vgui_item_id item);
	/* 0 when the item is unknown or already has a texture. */
	VGUI_API int vgui_editor_asset_browser_retry_thumbnail(
		vgui_editor_asset_browser* browser, vgui_item_id item);

	/* Replaces the model with a virtual source (copied).  Pass NULL to detach. */
	VGUI_API void vgui_editor_asset_browser_set_source(
		vgui_editor_asset_browser* browser,
		const vgui_editor_asset_source* source);
	VGUI_API int vgui_editor_asset_browser_is_virtual(
		vgui_editor_asset_browser* browser);
	VGUI_API int vgui_editor_asset_browser_item_count(
		vgui_editor_asset_browser* browser);
	VGUI_API int vgui_editor_asset_browser_get_item(
		vgui_editor_asset_browser* browser, int index,
		vgui_editor_asset_entry* out);
	VGUI_API vgui_item_id vgui_editor_asset_browser_id_at(
		vgui_editor_asset_browser* browser, int index);
	VGUI_API int vgui_editor_asset_browser_index_of(
		vgui_editor_asset_browser* browser, vgui_item_id item);
	/* Resolves an item even when filters hide it (asset_preview uses this). */
	VGUI_API int vgui_editor_asset_browser_item_info(
		vgui_editor_asset_browser* browser, vgui_item_id item,
		vgui_editor_asset_entry* out);

	VGUI_API vgui_editor_asset_view_mode vgui_editor_asset_browser_view(
		vgui_editor_asset_browser* browser);
	VGUI_API void vgui_editor_asset_browser_set_view(
		vgui_editor_asset_browser* browser, vgui_editor_asset_view_mode mode);
	VGUI_API void vgui_editor_asset_browser_set_view_switch_visible(
		vgui_editor_asset_browser* browser, int visible);
	VGUI_API int vgui_editor_asset_browser_view_switch_visible(
		vgui_editor_asset_browser* browser);

	/* Path navigation.  The handler resolves a path id into breadcrumbs plus
	 * contents; return 0 to reject the navigation. */
	VGUI_API void vgui_editor_asset_browser_set_navigate_handler(
		vgui_editor_asset_browser* browser, vgui_editor_asset_navigate_fn fn,
		void* user, vgui_editor_release_fn release_user);
	typedef void (*vgui_editor_asset_path_fn)(void* user, const char* path_id);
	VGUI_API void vgui_editor_asset_browser_set_on_path(
		vgui_editor_asset_browser* browser, vgui_editor_asset_path_fn fn,
		void* user);
	/* Resolves path_id, clears the forward stack and snapshots the previous
	 * location for go_back(). */
	VGUI_API int vgui_editor_asset_browser_navigate_to(
		vgui_editor_asset_browser* browser, const char* path_id);
	VGUI_API int vgui_editor_asset_browser_can_go_back(
		vgui_editor_asset_browser* browser);
	VGUI_API int vgui_editor_asset_browser_can_go_forward(
		vgui_editor_asset_browser* browser);
	VGUI_API int vgui_editor_asset_browser_go_back(
		vgui_editor_asset_browser* browser);
	VGUI_API int vgui_editor_asset_browser_go_forward(
		vgui_editor_asset_browser* browser);
	/* Re-resolves the current path without touching the history stacks. */
	VGUI_API int vgui_editor_asset_browser_reload(
		vgui_editor_asset_browser* browser);
	VGUI_API size_t vgui_editor_asset_browser_current_path(
		vgui_editor_asset_browser* browser, char* out, size_t cap);
	VGUI_API void vgui_editor_asset_browser_set_navigation_visible(
		vgui_editor_asset_browser* browser, int visible);
	VGUI_API int vgui_editor_asset_browser_navigation_visible(
		vgui_editor_asset_browser* browser);

	/* Debounced search.  Text reaches the filter after the debounce interval;
	 * immediate != 0 (and submit) applies at once.  The widget drives its own
	 * timer; headless hosts call tick() directly. */
	VGUI_API void vgui_editor_asset_browser_set_search_debounce(
		vgui_editor_asset_browser* browser, float seconds);
	VGUI_API float vgui_editor_asset_browser_search_debounce(
		vgui_editor_asset_browser* browser);
	VGUI_API void vgui_editor_asset_browser_set_search(
		vgui_editor_asset_browser* browser, const char* text, int immediate);
	VGUI_API size_t vgui_editor_asset_browser_search(
		vgui_editor_asset_browser* browser, char* out, size_t cap);
	VGUI_API int vgui_editor_asset_browser_search_pending(
		vgui_editor_asset_browser* browser);
	VGUI_API void vgui_editor_asset_browser_apply_search(
		vgui_editor_asset_browser* browser);
	VGUI_API int vgui_editor_asset_browser_filter_active(
		vgui_editor_asset_browser* browser);
	/* Defaults: "No assets" and "No matches"; a directory's own empty_text still
	 * wins over the default.  NULL restores a default. */
	VGUI_API void vgui_editor_asset_browser_set_empty_texts(
		vgui_editor_asset_browser* browser, const char* empty,
		const char* no_matches);
	VGUI_API size_t vgui_editor_asset_browser_empty_state_text(
		vgui_editor_asset_browser* browser, char* out, size_t cap);
	VGUI_API void vgui_editor_asset_browser_tick(
		vgui_editor_asset_browser* browser, float delta_time);

	/* Type filter. */
	VGUI_API void vgui_editor_asset_browser_set_type_options(
		vgui_editor_asset_browser* browser, const char* const* types,
		size_t count);
	VGUI_API size_t vgui_editor_asset_browser_type_option_count(
		vgui_editor_asset_browser* browser);
	VGUI_API size_t vgui_editor_asset_browser_type_option_at(
		vgui_editor_asset_browser* browser, size_t index, char* out, size_t cap);
	VGUI_API void vgui_editor_asset_browser_set_type_filter(
		vgui_editor_asset_browser* browser, const char* type);
	VGUI_API size_t vgui_editor_asset_browser_type_filter(
		vgui_editor_asset_browser* browser, char* out, size_t cap);
	VGUI_API void vgui_editor_asset_browser_set_type_filter_visible(
		vgui_editor_asset_browser* browser, int visible);
	VGUI_API int vgui_editor_asset_browser_type_filter_visible(
		vgui_editor_asset_browser* browser);
	/* Items before the search/type filters. */
	VGUI_API int vgui_editor_asset_browser_total_item_count(
		vgui_editor_asset_browser* browser);

	/* Visible range (last is exclusive) - the same math the renderer uses, so a
	 * headless host can verify virtualization without a draw pass. */
	VGUI_API int vgui_editor_asset_browser_visible_first(
		vgui_editor_asset_browser* browser);
	VGUI_API int vgui_editor_asset_browser_visible_last(
		vgui_editor_asset_browser* browser);
	VGUI_API int vgui_editor_asset_browser_visible_count(
		vgui_editor_asset_browser* browser);
	/* Copies min(cap, visible_count()) ids and returns the full visible count,
	 * so a host that passed a short array knows how many rows it missed.  Pass
	 * out == NULL to ask for the count only. */
	VGUI_API size_t vgui_editor_asset_browser_visible_items(
		vgui_editor_asset_browser* browser, vgui_item_id* out, size_t cap);
	VGUI_API int vgui_editor_asset_browser_grid_columns(
		vgui_editor_asset_browser* browser);
	/* Absolute window coordinates: the row rect in list mode, the cell rect in
	 * grid mode.  Any out pointer may be NULL. */
	VGUI_API int vgui_editor_asset_browser_item_rect(
		vgui_editor_asset_browser* browser, int index, float* x, float* y,
		float* width, float* height);
	/* Recomputes grid metrics and re-uses row widgets, selection and cached
	 * thumbnails; only the visible range requests missing thumbnails. */
	VGUI_API void vgui_editor_asset_browser_refresh(
		vgui_editor_asset_browser* browser);

	/* Selection. */
	VGUI_API vgui_item_id vgui_editor_asset_browser_selected(
		vgui_editor_asset_browser* browser);
	VGUI_API vgui_item_id vgui_editor_asset_browser_cursor(
		vgui_editor_asset_browser* browser);
	VGUI_API size_t vgui_editor_asset_browser_selection(
		vgui_editor_asset_browser* browser, vgui_item_id* out, size_t cap);
	VGUI_API void vgui_editor_asset_browser_set_selection(
		vgui_editor_asset_browser* browser, const vgui_item_id* items,
		size_t count);
	VGUI_API void vgui_editor_asset_browser_clear_selection(
		vgui_editor_asset_browser* browser);
	VGUI_API int vgui_editor_asset_browser_is_selected(
		vgui_editor_asset_browser* browser, vgui_item_id item);
	/* Legacy single listener plus the removable multi-listener form. */
	typedef void (*vgui_editor_asset_select_fn)(void* user, vgui_item_id item);
	VGUI_API void vgui_editor_asset_browser_set_on_selection(
		vgui_editor_asset_browser* browser, vgui_editor_asset_select_fn fn,
		void* user);
	VGUI_API size_t vgui_editor_asset_browser_add_selection_listener(
		vgui_editor_asset_browser* browser, vgui_editor_asset_select_fn fn,
		void* user);
	VGUI_API int vgui_editor_asset_browser_remove_selection_listener(
		vgui_editor_asset_browser* browser, size_t token);

	/* Renaming.  The handler applies the name to the host model and returns 0 to
	 * reject (a rejected rename is not recorded). */
	VGUI_API void vgui_editor_asset_browser_set_rename_handler(
		vgui_editor_asset_browser* browser, vgui_editor_asset_rename_fn fn,
		void* user);
	VGUI_API void vgui_editor_asset_browser_set_command_stack(
		vgui_editor_asset_browser* browser, vgui_editor_command_stack* stack);
	VGUI_API int vgui_editor_asset_browser_rename_item(
		vgui_editor_asset_browser* browser, vgui_item_id item, const char* name);

	/* Double-click / Enter activation (for example to enter a folder).  item 0
	 * means the primary selection for the begin_rename() / show_dependencies()
	 * helpers. */
	VGUI_API void vgui_editor_asset_browser_set_on_activate(
		vgui_editor_asset_browser* browser, vgui_editor_asset_select_fn fn,
		void* user);
	VGUI_API void vgui_editor_asset_browser_activate_selected(
		vgui_editor_asset_browser* browser);
	/* F03: rows and grid cells drag as asset payloads carrying the name. */
	VGUI_API void vgui_editor_asset_browser_enable_row_drag(
		vgui_editor_asset_browser* browser);

	/* OS file drops and import.  The root accepts VGUI_DRAG_FILES and routes
	 * them through the same entry as import_files(); returning 0 rejects the
	 * whole batch. */
	VGUI_API void vgui_editor_asset_browser_set_import_handler(
		vgui_editor_asset_browser* browser, vgui_editor_asset_import_fn fn,
		void* user);
	VGUI_API int vgui_editor_asset_browser_import_files(
		vgui_editor_asset_browser* browser, const char* const* files,
		size_t file_count);
	VGUI_API int vgui_editor_asset_browser_request_import(
		vgui_editor_asset_browser* browser);

	/* A06 operations. */
	VGUI_API void vgui_editor_asset_browser_set_operation_handler(
		vgui_editor_asset_browser* browser, vgui_editor_asset_operation_fn fn,
		void* user, vgui_editor_release_fn release_user);
	/* Final human-readable message for every operation; error is ERR_NONE on
	 * success. */
	typedef void (*vgui_editor_asset_notice_fn)(void* user, const char* message,
		vgui_editor_asset_operation_error error);
	VGUI_API void vgui_editor_asset_browser_set_on_notice(
		vgui_editor_asset_browser* browser, vgui_editor_asset_notice_fn fn,
		void* user);
	VGUI_API void vgui_editor_asset_browser_set_move_targets(
		vgui_editor_asset_browser* browser,
		vgui_editor_asset_move_targets_fn fn, void* user);
	VGUI_API void vgui_editor_asset_browser_set_dependency_handler(
		vgui_editor_asset_browser* browser, vgui_editor_asset_dependency_fn fn,
		void* user);
	/* Invoked by the Import... action; the host opens its dialog and calls
	 * import_files() with the chosen paths. */
	typedef void (*vgui_editor_asset_void_fn)(void* user);
	VGUI_API void vgui_editor_asset_browser_set_import_prompt(
		vgui_editor_asset_browser* browser, vgui_editor_asset_void_fn fn,
		void* user);

	/* Programmatic operations: applied immediately (no confirmation dialog) and
	 * 0 when the host rejected the request. */
	VGUI_API int vgui_editor_asset_browser_remove_items(
		vgui_editor_asset_browser* browser, const vgui_item_id* items,
		size_t count);
	VGUI_API int vgui_editor_asset_browser_remove_selected(
		vgui_editor_asset_browser* browser);
	VGUI_API int vgui_editor_asset_browser_duplicate_items(
		vgui_editor_asset_browser* browser, const vgui_item_id* items,
		size_t count);
	VGUI_API int vgui_editor_asset_browser_duplicate_selected(
		vgui_editor_asset_browser* browser);
	VGUI_API int vgui_editor_asset_browser_move_items(
		vgui_editor_asset_browser* browser, const vgui_item_id* items,
		size_t count, const char* target_path_id);
	VGUI_API int vgui_editor_asset_browser_move_selected(
		vgui_editor_asset_browser* browser, const char* target_path_id);
	VGUI_API int vgui_editor_asset_browser_reimport_items(
		vgui_editor_asset_browser* browser, const vgui_item_id* items,
		size_t count);
	VGUI_API int vgui_editor_asset_browser_reimport_selected(
		vgui_editor_asset_browser* browser);

	/* Right-click menu plus F2 / Delete / Ctrl+D / F5 / Ctrl+I over both list
	 * rows and grid cells. */
	VGUI_API void vgui_editor_asset_browser_enable_context_menu(
		vgui_editor_asset_browser* browser);
	VGUI_API int vgui_editor_asset_browser_context_menu_enabled(
		vgui_editor_asset_browser* browser);
	/* argument is the destination path for ACTION_MOVE, NULL otherwise.  An
	 * action outside vgui_editor_asset_action is rejected with 0. */
	VGUI_API int vgui_editor_asset_browser_perform_action(
		vgui_editor_asset_browser* browser, vgui_editor_asset_action kind,
		const char* argument);
	/* These three open a dialog on the browser's context and return whether it
	 * opened.  The core allows one modal at a time, so an earlier dialog the
	 * host never closed makes the next call return 0; close it with
	 * vgui_modal_close() before retrying.  The result arrives through the
	 * rename / operation handlers when the user answers, never here. */
	VGUI_API int vgui_editor_asset_browser_begin_rename(
		vgui_editor_asset_browser* browser, vgui_item_id item);
	VGUI_API int vgui_editor_asset_browser_confirm_remove(
		vgui_editor_asset_browser* browser, const vgui_item_id* items,
		size_t count);
	VGUI_API int vgui_editor_asset_browser_confirm_remove_selected(
		vgui_editor_asset_browser* browser);
	/* Fills `out` with the host dependency list and returns how many there are. */
	VGUI_API size_t vgui_editor_asset_browser_dependencies(
		vgui_editor_asset_browser* browser, vgui_item_id item,
		vgui_editor_asset_dependency* out, size_t out_count);
	/* Formats the list in a modal alert and updates the last message. */
	VGUI_API int vgui_editor_asset_browser_show_dependencies(
		vgui_editor_asset_browser* browser, vgui_item_id item);

	/* Outcome of the last operation. */
	VGUI_API vgui_editor_asset_operation_error
		vgui_editor_asset_browser_last_error(
			vgui_editor_asset_browser* browser);
	VGUI_API size_t vgui_editor_asset_browser_last_message(
		vgui_editor_asset_browser* browser, char* out, size_t cap);
	VGUI_API int vgui_editor_asset_browser_last_undoable(
		vgui_editor_asset_browser* browser);

	/* Child widgets (borrowed, owned by the browser).  The switch, path, search
	 * and type combo are the kit C++ widgets; use them through the browser's own
	 * accessors. */
	VGUI_API vgui_widget* vgui_editor_asset_browser_root(vgui_editor_asset_browser* browser);
	VGUI_API vgui_widget* vgui_editor_asset_browser_grid(vgui_editor_asset_browser* browser);
	VGUI_API vgui_widget* vgui_editor_asset_browser_scroll(vgui_editor_asset_browser* browser);
	VGUI_API vgui_widget* vgui_editor_asset_browser_view_switch_widget(
		vgui_editor_asset_browser* browser);
	VGUI_API vgui_widget* vgui_editor_asset_browser_path_widget(
		vgui_editor_asset_browser* browser);
	VGUI_API vgui_widget* vgui_editor_asset_browser_search_widget(
		vgui_editor_asset_browser* browser);
	VGUI_API vgui_widget* vgui_editor_asset_browser_type_combo_widget(
		vgui_editor_asset_browser* browser);

	/* ------------------------------------------------------------------------- */
	/* Asset preview                                                             */
	/* ------------------------------------------------------------------------- */

	typedef struct vgui_editor_asset_preview vgui_editor_asset_preview;

	/* Ordering mirrors vgui_editor_kit::asset_preview::preview_kind. */
	typedef enum vgui_editor_preview_kind {
		VGUI_EDITOR_PREVIEW_NONE = 0,
		VGUI_EDITOR_PREVIEW_IMAGE,
		VGUI_EDITOR_PREVIEW_MODEL,
		VGUI_EDITOR_PREVIEW_MATERIAL,
		VGUI_EDITOR_PREVIEW_TEXT,
		VGUI_EDITOR_PREVIEW_AUDIO,
		VGUI_EDITOR_PREVIEW_CUSTOM
	} vgui_editor_preview_kind;

	typedef struct vgui_editor_preview_field {
		uint32_t struct_size;
		const char* label;
		const char* value;
	} vgui_editor_preview_field;

	VGUI_API void vgui_editor_preview_field_default(
		vgui_editor_preview_field* field);

	/* Loader result.  name/type fall back to the request when left empty; error
	 * marks a failed load and the preview keeps the item with the error text over
	 * a placeholder.  The library copies every string, byte and array as soon as
	 * the handler returns, so they must be host-owned storage (static or heap)
	 * that is still alive at that point - not callback locals. */
	typedef struct vgui_editor_preview_data {
		uint32_t struct_size;
		vgui_editor_preview_kind kind;
		const char* name;
		const char* type;
		const char* detail;
		const char* error;
		const char* text;
		const vgui_editor_preview_field* fields;
		size_t field_count;
		int image_width;
		int image_height;
		/* Bytes per row; 0 means width * 4 for the RGBA8 buffer below. */
		int image_stride;
		const unsigned char* image_pixels;
		size_t image_pixel_count;
		vgui_color accent;
		vgui_color secondary;
		const vgui_color* swatches;
		size_t swatch_count;
		const float* waveform;
		size_t waveform_count;
	} vgui_editor_preview_data;

	VGUI_API void vgui_editor_preview_data_default(vgui_editor_preview_data* data);

	typedef struct vgui_editor_preview_request {
		uint32_t struct_size;
		vgui_item_id id;
		const char* name;
		const char* type;
	} vgui_editor_preview_request;

	/* Runs on a worker thread: only touch host data here, never vgui or the
	 * preview.  Return 0 (and set out->error) for the failure placeholder. */
	typedef int (*vgui_editor_preview_load_fn)(void* user,
		const vgui_editor_preview_request* request, vgui_editor_preview_data* out);

	VGUI_API vgui_editor_asset_preview* vgui_editor_asset_preview_create(
		vgui_widget* parent);
	VGUI_API void vgui_editor_asset_preview_destroy(
		vgui_editor_asset_preview* preview);
	VGUI_API vgui_widget* vgui_editor_asset_preview_widget(
		vgui_editor_asset_preview* preview);
	/* The image stage (borrowed). */
	VGUI_API vgui_widget* vgui_editor_asset_preview_stage(
		vgui_editor_asset_preview* preview);
	VGUI_API void vgui_editor_asset_preview_set_load_handler(
		vgui_editor_asset_preview* preview, vgui_editor_preview_load_fn fn,
		void* user, vgui_editor_release_fn release_user);
	/* NULL name/type keep the browser's values. */
	VGUI_API void vgui_editor_asset_preview_set_item(
		vgui_editor_asset_preview* preview, vgui_item_id item,
		const char* name, const char* type);
	VGUI_API void vgui_editor_asset_preview_clear(
		vgui_editor_asset_preview* preview);
	/* Re-runs the loader for the current item. */
	VGUI_API void vgui_editor_asset_preview_reload(
		vgui_editor_asset_preview* preview);
	VGUI_API vgui_item_id vgui_editor_asset_preview_item(
		vgui_editor_asset_preview* preview);
	VGUI_API int vgui_editor_asset_preview_loading(
		vgui_editor_asset_preview* preview);
	VGUI_API int vgui_editor_asset_preview_has_error(
		vgui_editor_asset_preview* preview);
	VGUI_API size_t vgui_editor_asset_preview_error(
		vgui_editor_asset_preview* preview, char* out, size_t cap);
	VGUI_API size_t vgui_editor_asset_preview_status_text(
		vgui_editor_asset_preview* preview, char* out, size_t cap);
	VGUI_API vgui_texture* vgui_editor_asset_preview_texture(
		vgui_editor_asset_preview* preview);
	/* Defaults: "Select an asset to preview", "Loading preview...",
	 * "Preview failed"; NULL restores one. */
	VGUI_API void vgui_editor_asset_preview_set_texts(
		vgui_editor_asset_preview* preview, const char* empty,
		const char* loading, const char* failed);
	VGUI_API void vgui_editor_asset_preview_tick(
		vgui_editor_asset_preview* preview, float delta_time);
	/* Applies a completed background load; 1 when new data arrived. */
	VGUI_API int vgui_editor_asset_preview_poll(vgui_editor_asset_preview* preview);

	/* ------------------------------------------------------------------------- */
	/* Viewport surface, camera, grid, picking and gizmo                         */
	/* ------------------------------------------------------------------------- */

	typedef struct vgui_editor_viewport vgui_editor_viewport;
	typedef struct vgui_editor_camera vgui_editor_camera;
	typedef struct vgui_editor_grid vgui_editor_grid;
	typedef struct vgui_editor_picking vgui_editor_picking;
	typedef struct vgui_editor_gizmo vgui_editor_gizmo;

	typedef struct vgui_editor_vec3 {
		float x;
		float y;
		float z;
	} vgui_editor_vec3;

	/* Serializable orbit state.  The camera position derives from target, yaw,
	 * pitch and distance, so restoring these restores the view. */
	typedef struct vgui_editor_camera_state {
		uint32_t struct_size;
		vgui_editor_vec3 target;
		/* Radians around the world Y axis. */
		float yaw;
		/* Radians; positive means above the target. */
		float pitch;
		float distance;
		float fov_deg;
		float near_plane;
		float far_plane;
	} vgui_editor_camera_state;

	VGUI_API void vgui_editor_camera_state_default(
		vgui_editor_camera_state* state);

	typedef struct vgui_editor_camera_focus {
		uint32_t struct_size;
		vgui_editor_vec3 center;
		float radius;
	} vgui_editor_camera_focus;

	/* Overlay drawing on top of the bound texture.  x/y/width/height is the
	 * absolute content rect (the area inside the 1px border).  The three slots
	 * draw in a fixed order: selection highlights, interaction feedback (the
	 * box-select marquee), then foreground (gizmos). */
	typedef void (*vgui_editor_overlay_draw_fn)(void* user,
		struct ovg_ctx_cb* cb, struct rvg_t* vg, float x, float y, float width,
		float height, float delta_time);
	/* Raw input; x/y are surface-local and may be outside the surface while a
	 * press is captured.  Return 1 when the event was consumed. */
	typedef int (*vgui_editor_viewport_event_fn)(void* user,
		const vgui_event* event);
	typedef void (*vgui_editor_viewport_resize_fn)(void* user, int width,
		int height);
	typedef void (*vgui_editor_viewport_focus_fn)(void* user, int focused);

	VGUI_API vgui_editor_viewport* vgui_editor_viewport_create(
		vgui_widget* parent);
	VGUI_API void vgui_editor_viewport_destroy(vgui_editor_viewport* viewport);
	VGUI_API vgui_widget* vgui_editor_viewport_widget(
		vgui_editor_viewport* viewport);
	/* The viewport borrows the texture.  Native textures stay valid across FBO
	 * rebuilds through vgui_sdl3_texture_update_native(); CPU textures must
	 * outlive the binding. */
	VGUI_API void vgui_editor_viewport_set_texture(
		vgui_editor_viewport* viewport, vgui_texture* texture);
	VGUI_API vgui_texture* vgui_editor_viewport_texture(
		vgui_editor_viewport* viewport);
	/* Layout -> render size.  Call once per frame after vgui_layout() and before
	 * rendering the scene; 1 when the size changed and the listener ran. */
	VGUI_API int vgui_editor_viewport_poll_resize(
		vgui_editor_viewport* viewport);
	VGUI_API int vgui_editor_viewport_render_width(
		vgui_editor_viewport* viewport);
	VGUI_API int vgui_editor_viewport_render_height(
		vgui_editor_viewport* viewport);
	/* Any out pointer may be NULL. */
	VGUI_API void vgui_editor_viewport_get_render_size(
		vgui_editor_viewport* viewport, int* width, int* height);
	/* Surface-local content rect (excludes the 1px border). */
	VGUI_API void vgui_editor_viewport_get_content_rect(
		vgui_editor_viewport* viewport, float* x, float* y, float* width,
		float* height);
	VGUI_API void vgui_editor_viewport_set_on_resize(
		vgui_editor_viewport* viewport, vgui_editor_viewport_resize_fn fn,
		void* user);
	VGUI_API void vgui_editor_viewport_set_selection_overlay(
		vgui_editor_viewport* viewport, vgui_editor_overlay_draw_fn fn,
		void* user, vgui_editor_release_fn release_user);
	VGUI_API void vgui_editor_viewport_set_interaction_overlay(
		vgui_editor_viewport* viewport, vgui_editor_overlay_draw_fn fn,
		void* user, vgui_editor_release_fn release_user);
	VGUI_API void vgui_editor_viewport_set_foreground_overlay(
		vgui_editor_viewport* viewport, vgui_editor_overlay_draw_fn fn,
		void* user, vgui_editor_release_fn release_user);
	VGUI_API void vgui_editor_viewport_set_on_event(
		vgui_editor_viewport* viewport, vgui_editor_viewport_event_fn fn,
		void* user);
	/* Extra observers layered on the surface (camera, gizmo).  They run after
	 * the main listener in registration order, and the first one to consume an
	 * event stops the rest - so a gizmo can own the pointer before picking
	 * starts.  Returns a non-zero token. */
	VGUI_API int vgui_editor_viewport_add_event_listener(
		vgui_editor_viewport* viewport, vgui_editor_viewport_event_fn fn,
		void* user);
	VGUI_API void vgui_editor_viewport_remove_event_listener(
		vgui_editor_viewport* viewport, int token);
	VGUI_API void vgui_editor_viewport_set_on_focus(
		vgui_editor_viewport* viewport, vgui_editor_viewport_focus_fn fn,
		void* user);
	/* True between a mouse-down inside the surface and its matching release. */
	VGUI_API int vgui_editor_viewport_dragging(vgui_editor_viewport* viewport);
	VGUI_API int vgui_editor_viewport_drag_button(vgui_editor_viewport* viewport);
	/* Ends the gesture and forwards a synthetic VGUI_EVENT_CANCEL. */
	VGUI_API void vgui_editor_viewport_cancel_interaction(
		vgui_editor_viewport* viewport);
	VGUI_API int vgui_editor_viewport_focused(vgui_editor_viewport* viewport);
	/* Like every core focus setter, this is refused while a modal is open: the
	 * core keeps focus inside the modal stage until it closes. */
	VGUI_API void vgui_editor_viewport_focus(vgui_editor_viewport* viewport);
	/* F03/A04: accepts asset / scene_object drops.  Return 0 to reject. */
	typedef int (*vgui_editor_viewport_drop_fn)(void* user,
		const vgui_editor_drag_payload* payload, float x, float y);
	VGUI_API void vgui_editor_viewport_set_on_drop(
		vgui_editor_viewport* viewport, vgui_editor_viewport_drop_fn fn,
		void* user);
	/* The last accepted drop (payload strings borrow until the next mutation of
	 * this viewport). */
	VGUI_API int vgui_editor_viewport_last_drop(vgui_editor_viewport* viewport,
		vgui_editor_drag_payload* out, float* x, float* y);

	/* Orbit camera.  Attaching twice detaches first, so one camera drives one
	 * surface. */
	VGUI_API vgui_editor_camera* vgui_editor_camera_create(void);
	VGUI_API void vgui_editor_camera_destroy(vgui_editor_camera* camera);
	VGUI_API int vgui_editor_camera_attach(vgui_editor_camera* camera,
		vgui_editor_viewport* viewport);
	VGUI_API void vgui_editor_camera_detach(vgui_editor_camera* camera);
	VGUI_API int vgui_editor_camera_attached(vgui_editor_camera* camera);
	/* Disabling stops navigation and cancels an in-flight drag; the position
	 * stays.  Hosts use this while a gizmo owns the pointer. */
	VGUI_API void vgui_editor_camera_set_navigation_enabled(
		vgui_editor_camera* camera, int enabled);
	VGUI_API int vgui_editor_camera_navigation_enabled(
		vgui_editor_camera* camera);
	/* 0 disables the gesture.  Defaults: pan = 2 (middle), orbit = 3 (right). */
	VGUI_API void vgui_editor_camera_set_pan_button(vgui_editor_camera* camera,
		int button);
	VGUI_API void vgui_editor_camera_set_orbit_button(vgui_editor_camera* camera,
		int button);
	VGUI_API void vgui_editor_camera_set_orbit_sensitivity(
		vgui_editor_camera* camera, float radians_per_pixel);
	VGUI_API void vgui_editor_camera_set_zoom_speed(vgui_editor_camera* camera,
		float speed);
	VGUI_API void vgui_editor_camera_set_distance_range(
		vgui_editor_camera* camera, float min_distance, float max_distance);
	/* Fill `out` and return 1 when there is something to frame. */
	typedef int (*vgui_editor_camera_focus_fn)(void* user,
		vgui_editor_camera_focus* out);
	VGUI_API void vgui_editor_camera_set_focus_provider(
		vgui_editor_camera* camera, vgui_editor_camera_focus_fn fn, void* user);
	/* Frames the provider target (the F key additionally needs viewport focus). */
	VGUI_API int vgui_editor_camera_focus_selection(vgui_editor_camera* camera);
	VGUI_API int vgui_editor_camera_panning(vgui_editor_camera* camera);
	VGUI_API int vgui_editor_camera_orbiting(vgui_editor_camera* camera);
	VGUI_API int vgui_editor_camera_navigating(vgui_editor_camera* camera);
	VGUI_API void vgui_editor_camera_cancel_navigation(
		vgui_editor_camera* camera);
	VGUI_API void vgui_editor_camera_get_state(vgui_editor_camera* camera,
		vgui_editor_camera_state* out);
	/* Clamps pitch, distance, fov and clip planes into safe ranges. */
	VGUI_API void vgui_editor_camera_set_state(vgui_editor_camera* camera,
		const vgui_editor_camera_state* state);
	VGUI_API void vgui_editor_camera_reset(vgui_editor_camera* camera);
	VGUI_API vgui_editor_vec3 vgui_editor_camera_position(
		vgui_editor_camera* camera);
	VGUI_API vgui_editor_vec3 vgui_editor_camera_forward(
		vgui_editor_camera* camera);
	VGUI_API vgui_editor_vec3 vgui_editor_camera_right(vgui_editor_camera* camera);
	VGUI_API vgui_editor_vec3 vgui_editor_camera_up(vgui_editor_camera* camera);
	typedef void (*vgui_editor_camera_change_fn)(void* user);
	VGUI_API void vgui_editor_camera_set_on_change(vgui_editor_camera* camera,
		vgui_editor_camera_change_fn fn, void* user);

	/* World grid.  The host renders the generated lines itself. */
	typedef struct vgui_editor_grid_line {
		uint32_t struct_size;
		vgui_editor_vec3 a;
		vgui_editor_vec3 b;
		/* 0xAARRGGBB. */
		vgui_color color;
		/* World-unit line width. */
		float width;
	} vgui_editor_grid_line;

	VGUI_API vgui_editor_grid* vgui_editor_grid_create(void);
	VGUI_API void vgui_editor_grid_destroy(vgui_editor_grid* grid);
	VGUI_API void vgui_editor_grid_set_visible(vgui_editor_grid* grid,
		int visible);
	VGUI_API int vgui_editor_grid_visible(vgui_editor_grid* grid);
	/* Auto picks the cell size from the camera; off uses cell_size(). */
	VGUI_API void vgui_editor_grid_set_auto_density(vgui_editor_grid* grid,
		int enabled);
	VGUI_API int vgui_editor_grid_auto_density(vgui_editor_grid* grid);
	VGUI_API void vgui_editor_grid_set_cell_size(vgui_editor_grid* grid,
		float cell);
	VGUI_API float vgui_editor_grid_cell_size(vgui_editor_grid* grid);
	VGUI_API void vgui_editor_grid_set_min_cell_pixels(vgui_editor_grid* grid,
		float pixels);
	VGUI_API float vgui_editor_grid_min_cell_pixels(vgui_editor_grid* grid);
	VGUI_API void vgui_editor_grid_set_max_cell_pixels(vgui_editor_grid* grid,
		float pixels);
	VGUI_API float vgui_editor_grid_max_cell_pixels(vgui_editor_grid* grid);
	VGUI_API int vgui_editor_grid_half_cell_count(vgui_editor_grid* grid);
	VGUI_API void vgui_editor_grid_set_half_cell_count(vgui_editor_grid* grid,
		int cells);
	VGUI_API int vgui_editor_grid_major_every(vgui_editor_grid* grid);
	VGUI_API void vgui_editor_grid_set_major_every(vgui_editor_grid* grid,
		int cells);
	VGUI_API void vgui_editor_grid_set_plane_height(vgui_editor_grid* grid,
		float y);
	VGUI_API float vgui_editor_grid_plane_height(vgui_editor_grid* grid);
	VGUI_API void vgui_editor_grid_set_axes_visible(vgui_editor_grid* grid,
		int visible);
	VGUI_API int vgui_editor_grid_axes_visible(vgui_editor_grid* grid);
	VGUI_API void vgui_editor_grid_set_axis_length_cells(vgui_editor_grid* grid,
		float cells);
	VGUI_API void vgui_editor_grid_set_colors(vgui_editor_grid* grid,
		vgui_color minor, vgui_color major, vgui_color axis_x, vgui_color axis_y,
		vgui_color axis_z);
	VGUI_API float vgui_editor_grid_effective_cell_size(vgui_editor_grid* grid,
		float distance, int viewport_height, float fov_deg);
	/* Generates world-space grid + axis lines around the camera target and
	 * returns how many there are (0 when invisible or the parameters are
	 * unusable).  Pass out == NULL to ask for the count only. */
	VGUI_API size_t vgui_editor_grid_build(vgui_editor_grid* grid,
		const vgui_editor_camera_state* camera, int viewport_width,
		int viewport_height, vgui_editor_grid_line* out, size_t out_count);

	/* Left-button picking / box selection.  Selection state lives in the editor
	 * context and is mirrored by the outliner. */
	typedef struct vgui_editor_pick_point {
		uint32_t struct_size;
		float x;
		float y;
		int width;
		int height;
		int mods;
	} vgui_editor_pick_point;

	typedef struct vgui_editor_pick_box {
		uint32_t struct_size;
		float x0;
		float y0;
		float x1;
		float y1;
		int width;
		int height;
		int mods;
	} vgui_editor_pick_box;

	/* Screen box for one selected object, in viewport content coordinates. */
	typedef struct vgui_editor_highlight_box {
		uint32_t struct_size;
		vgui_editor_object_id id;
		float x;
		float y;
		float width;
		float height;
	} vgui_editor_highlight_box;

	typedef vgui_editor_object_id (*vgui_editor_point_pick_fn)(void* user,
		const vgui_editor_pick_point* query);
	/* Fills `out` with the picked ids and returns how many there are. */
	typedef size_t (*vgui_editor_box_pick_fn)(void* user,
		const vgui_editor_pick_box* query, vgui_editor_object_id* out,
		size_t out_count);
	typedef size_t (*vgui_editor_highlight_fn)(void* user,
		const vgui_editor_object_id* ids, size_t id_count,
		vgui_editor_highlight_box* out, size_t out_count);

	VGUI_API vgui_editor_picking* vgui_editor_picking_create(void);
	VGUI_API void vgui_editor_picking_destroy(vgui_editor_picking* picking);
	VGUI_API int vgui_editor_picking_attach(vgui_editor_picking* picking,
		vgui_editor_viewport* viewport, vgui_editor_context* ctx);
	VGUI_API void vgui_editor_picking_detach(vgui_editor_picking* picking);
	VGUI_API int vgui_editor_picking_attached(vgui_editor_picking* picking);
	VGUI_API void vgui_editor_picking_set_point_picker(vgui_editor_picking* picking,
		vgui_editor_point_pick_fn fn, void* user);
	VGUI_API void vgui_editor_picking_set_box_picker(vgui_editor_picking* picking,
		vgui_editor_box_pick_fn fn, void* user);
	VGUI_API void vgui_editor_picking_set_highlight_provider(
		vgui_editor_picking* picking, vgui_editor_highlight_fn fn, void* user);
	VGUI_API void vgui_editor_picking_set_enabled(vgui_editor_picking* picking,
		int enabled);
	VGUI_API int vgui_editor_picking_enabled(vgui_editor_picking* picking);
	/* 0 ignores Ctrl/Shift and always replaces the selection. */
	VGUI_API void vgui_editor_picking_set_multi_select(vgui_editor_picking* picking,
		int enabled);
	VGUI_API int vgui_editor_picking_multi_select(vgui_editor_picking* picking);
	/* Negative selects the vgui style dock.drag_threshold (default 4px). */
	VGUI_API void vgui_editor_picking_set_drag_threshold(
		vgui_editor_picking* picking, float pixels);
	VGUI_API int vgui_editor_picking_pending(vgui_editor_picking* picking);
	VGUI_API int vgui_editor_picking_selecting(vgui_editor_picking* picking);
	/* Aborts the gesture without changing the selection. */
	VGUI_API void vgui_editor_picking_cancel_selection(
		vgui_editor_picking* picking);
	/* Mirrors the context selection. */
	VGUI_API size_t vgui_editor_picking_selected_count(vgui_editor_picking* picking);
	VGUI_API vgui_editor_object_id vgui_editor_picking_selected_at(
		vgui_editor_picking* picking, size_t index);

	/* Translate / rotate / scale gizmo.  It draws in the foreground overlay, so
	 * attach it before picking to win the left-button hit test. */
	typedef enum vgui_editor_gizmo_mode {
		VGUI_EDITOR_GIZMO_TRANSLATE = 0,
		VGUI_EDITOR_GIZMO_ROTATE,
		VGUI_EDITOR_GIZMO_SCALE
	} vgui_editor_gizmo_mode;

	/* The host resolves every space into frame axes: `local` uses the object's
	 * own orientation, `screen` typically the camera basis. */
	typedef enum vgui_editor_gizmo_space {
		VGUI_EDITOR_GIZMO_SPACE_WORLD = 0,
		VGUI_EDITOR_GIZMO_SPACE_LOCAL,
		VGUI_EDITOR_GIZMO_SPACE_PARENT,
		VGUI_EDITOR_GIZMO_SPACE_SCREEN
	} vgui_editor_gizmo_space;

	typedef enum vgui_editor_gizmo_handle {
		VGUI_EDITOR_GIZMO_HANDLE_NONE = 0,
		VGUI_EDITOR_GIZMO_HANDLE_AXIS_X,
		VGUI_EDITOR_GIZMO_HANDLE_AXIS_Y,
		VGUI_EDITOR_GIZMO_HANDLE_AXIS_Z,
		VGUI_EDITOR_GIZMO_HANDLE_PLANE_YZ,
		VGUI_EDITOR_GIZMO_HANDLE_PLANE_ZX,
		VGUI_EDITOR_GIZMO_HANDLE_PLANE_XY,
		VGUI_EDITOR_GIZMO_HANDLE_ROTATE_X,
		VGUI_EDITOR_GIZMO_HANDLE_ROTATE_Y,
		VGUI_EDITOR_GIZMO_HANDLE_ROTATE_Z,
		VGUI_EDITOR_GIZMO_HANDLE_SCALE_X,
		VGUI_EDITOR_GIZMO_HANDLE_SCALE_Y,
		VGUI_EDITOR_GIZMO_HANDLE_SCALE_Z,
		VGUI_EDITOR_GIZMO_HANDLE_SCALE_UNIFORM
	} vgui_editor_gizmo_handle;

	/* Drag lifecycle: begin has a zero delta, and cancel follows begin for every
	 * interrupted drag. */
	typedef enum vgui_editor_gizmo_phase {
		VGUI_EDITOR_GIZMO_PHASE_BEGIN = 0,
		VGUI_EDITOR_GIZMO_PHASE_PREVIEW,
		VGUI_EDITOR_GIZMO_PHASE_COMMIT,
		VGUI_EDITOR_GIZMO_PHASE_CANCEL
	} vgui_editor_gizmo_phase;

	typedef struct vgui_editor_gizmo_frame {
		uint32_t struct_size;
		int valid;
		vgui_editor_vec3 origin;
		/* Orthonormal world-space axes, already resolved for the current space. */
		vgui_editor_vec3 axis[3];
		/* > 0 overrides the gizmo's screen_length(). */
		float screen_length;
	} vgui_editor_gizmo_frame;

	VGUI_API void vgui_editor_gizmo_frame_default(vgui_editor_gizmo_frame* frame);

	typedef struct vgui_editor_gizmo_ray {
		vgui_editor_vec3 origin;
		vgui_editor_vec3 direction;
	} vgui_editor_gizmo_ray;

	/* Payload shared by every mode; unused fields keep their neutral values
	 * (translation/rotation {0,0,0}, scale {1,1,1}).  Rotation is a world-space
	 * rotation vector (axis * radians); scale components multiply along the
	 * frame axes captured at begin. */
	typedef struct vgui_editor_gizmo_transform {
		uint32_t struct_size;
		vgui_editor_gizmo_mode mode;
		vgui_editor_gizmo_handle handle;
		vgui_editor_vec3 origin;
		vgui_editor_vec3 axis[3];
		vgui_editor_vec3 translation;
		vgui_editor_vec3 rotation;
		vgui_editor_vec3 scale;
	} vgui_editor_gizmo_transform;

	VGUI_API void vgui_editor_gizmo_transform_default(
		vgui_editor_gizmo_transform* transform);

	/* Fills the frame the gizmo draws and hit-tests against; return 0 when there
	 * is nothing to show.  The provider must consult mode()/space(). */
	typedef int (*vgui_editor_gizmo_frame_fn)(void* user,
		vgui_editor_gizmo_frame* out);
	/* world -> content-local screen coordinates; 0 when the point is behind the
	 * camera or otherwise not drawable. */
	typedef int (*vgui_editor_gizmo_project_fn)(void* user,
		vgui_editor_vec3 world, float* x, float* y);
	/* content-local coordinates -> world ray; 0 when the ray is unusable. */
	typedef int (*vgui_editor_gizmo_ray_fn)(void* user, float x, float y,
		vgui_editor_gizmo_ray* out);
	/* Full payload for every mode. */
	typedef void (*vgui_editor_gizmo_transform_fn)(void* user,
		vgui_editor_gizmo_phase phase,
		const vgui_editor_gizmo_transform* transform);
	/* Legacy translate-only form: same world delta, translate gestures only. */
	typedef void (*vgui_editor_gizmo_drag_fn)(void* user,
		vgui_editor_gizmo_phase phase, vgui_editor_vec3 delta);

	VGUI_API vgui_editor_gizmo* vgui_editor_gizmo_create(void);
	VGUI_API void vgui_editor_gizmo_destroy(vgui_editor_gizmo* gizmo);
	VGUI_API int vgui_editor_gizmo_attach(vgui_editor_gizmo* gizmo,
		vgui_editor_viewport* viewport);
	VGUI_API void vgui_editor_gizmo_detach(vgui_editor_gizmo* gizmo);
	VGUI_API int vgui_editor_gizmo_attached(vgui_editor_gizmo* gizmo);
	VGUI_API void vgui_editor_gizmo_set_frame_provider(vgui_editor_gizmo* gizmo,
		vgui_editor_gizmo_frame_fn fn, void* user);
	VGUI_API void vgui_editor_gizmo_set_projection(vgui_editor_gizmo* gizmo,
		vgui_editor_gizmo_project_fn fn, void* user);
	VGUI_API void vgui_editor_gizmo_set_ray_provider(vgui_editor_gizmo* gizmo,
		vgui_editor_gizmo_ray_fn fn, void* user);
	VGUI_API void vgui_editor_gizmo_set_transform_handler(
		vgui_editor_gizmo* gizmo, vgui_editor_gizmo_transform_fn fn, void* user);
	VGUI_API void vgui_editor_gizmo_set_drag_handler(vgui_editor_gizmo* gizmo,
		vgui_editor_gizmo_drag_fn fn, void* user);
	/* Snapping is read at drag time, so changes to the struct take effect
	 * immediately. */
	VGUI_API void vgui_editor_gizmo_set_snap(vgui_editor_gizmo* gizmo,
		const vgui_editor_snap_settings* settings);
	VGUI_API void vgui_editor_gizmo_get_snap(vgui_editor_gizmo* gizmo,
		vgui_editor_snap_settings* out);
	/* Hiding cancels an in-flight drag. */
	VGUI_API void vgui_editor_gizmo_set_visible(vgui_editor_gizmo* gizmo,
		int visible);
	VGUI_API int vgui_editor_gizmo_visible(vgui_editor_gizmo* gizmo);
	/* Changing mode or space cancels an in-flight drag. */
	VGUI_API void vgui_editor_gizmo_set_mode(vgui_editor_gizmo* gizmo,
		vgui_editor_gizmo_mode mode);
	VGUI_API vgui_editor_gizmo_mode vgui_editor_gizmo_mode_of(
		vgui_editor_gizmo* gizmo);
	VGUI_API void vgui_editor_gizmo_set_space(vgui_editor_gizmo* gizmo,
		vgui_editor_gizmo_space space);
	VGUI_API vgui_editor_gizmo_space vgui_editor_gizmo_space_of(
		vgui_editor_gizmo* gizmo);
	VGUI_API void vgui_editor_gizmo_set_screen_length(vgui_editor_gizmo* gizmo,
		float pixels);
	VGUI_API float vgui_editor_gizmo_screen_length(vgui_editor_gizmo* gizmo);
	VGUI_API void vgui_editor_gizmo_set_handle_tolerance(vgui_editor_gizmo* gizmo,
		float pixels);
	VGUI_API void vgui_editor_gizmo_set_axis_width(vgui_editor_gizmo* gizmo,
		float pixels);
	VGUI_API int vgui_editor_gizmo_dragging(vgui_editor_gizmo* gizmo);
	VGUI_API vgui_editor_gizmo_handle vgui_editor_gizmo_active_handle(
		vgui_editor_gizmo* gizmo);
	VGUI_API vgui_editor_gizmo_handle vgui_editor_gizmo_hovered_handle(
		vgui_editor_gizmo* gizmo);
	/* Ends a drag with a cancel phase; safe without an active drag. */
	VGUI_API void vgui_editor_gizmo_cancel_drag(vgui_editor_gizmo* gizmo);
	VGUI_API void vgui_editor_gizmo_reset_hover(vgui_editor_gizmo* gizmo);

	/* ------------------------------------------------------------------------- */
	/* Editor shell                                                              */
	/* ------------------------------------------------------------------------- */

	typedef struct vgui_editor_shell vgui_editor_shell;

	/* V09: per-view render request mirrored by the shell.  The shell owns no GPU
	 * device; the host applies these values to its renderer each frame. */
	typedef struct vgui_editor_viewport_settings {
		uint32_t struct_size;
		int hdr;
		/* 1, 2, 4 or 8; the host may clamp to what the device supports. */
		int msaa_samples;
		int depth_preview;
	} vgui_editor_viewport_settings;

	VGUI_API void vgui_editor_viewport_settings_default(
		vgui_editor_viewport_settings* settings);

	/* S08: document lifecycle hooks.  Unset callbacks keep the shell's
	 * status-only fallback.  delete_selection returns 0 to let the shell clear
	 * the selection instead. */
	typedef void (*vgui_editor_document_fn)(void* user);
	typedef int (*vgui_editor_delete_selection_fn)(void* user,
		const vgui_editor_object_id* ids, size_t id_count);

	typedef struct vgui_editor_document_handlers {
		uint32_t struct_size;
		vgui_editor_document_fn new_document;
		vgui_editor_document_fn open_document;
		vgui_editor_document_fn save_document;
		vgui_editor_document_fn save_document_as;
		vgui_editor_document_fn exit_editor;
		vgui_editor_delete_selection_fn delete_selection;
		void* user;
		vgui_editor_release_fn release_user;
	} vgui_editor_document_handlers;

	VGUI_API void vgui_editor_document_handlers_default(
		vgui_editor_document_handlers* handlers);

	/* The shell needs an editor context for selection and the shared command
	 * stack, and builds its dock tree under that context's root.  Destroy the
	 * shell before the vgui context. */
	VGUI_API vgui_editor_shell* vgui_editor_shell_create(
		vgui_editor_context* ctx);
	VGUI_API void vgui_editor_shell_destroy(vgui_editor_shell* shell);
	VGUI_API int vgui_editor_shell_built(vgui_editor_shell* shell);
	VGUI_API vgui_widget* vgui_editor_shell_widget(vgui_editor_shell* shell);

	/* Menu bar plus the command registry / stack (borrowed handles owned by the
	 * shell).  Re-adding a default command id overrides it; menus pick the new
	 * title and state up after refresh_commands(). */
	VGUI_API vgui_widget* vgui_editor_shell_menubar(vgui_editor_shell* shell);
	VGUI_API vgui_editor_command_registry* vgui_editor_shell_commands(
		vgui_editor_shell* shell);
	VGUI_API int vgui_editor_shell_run_command(vgui_editor_shell* shell,
		const char* id);
	VGUI_API void vgui_editor_shell_refresh_commands(vgui_editor_shell* shell);

	/* Passing NULL restores the status-only defaults. */
	VGUI_API void vgui_editor_shell_set_document_handlers(
		vgui_editor_shell* shell, const vgui_editor_document_handlers* handlers);

	/* The shell's Dock.  Panel ids are "outliner", "viewport", "properties" and
	 * the seven bottom views "assets", "console", "canvas", "table", "timeline",
	 * "nodegraph" and "code"; rearrange, float and query them with the core
	 * vgui_dock_* API. */
	VGUI_API vgui_widget* vgui_editor_shell_dock(vgui_editor_shell* shell);

	/* Panel visibility.  VGUI_EDITOR_PANEL_BOTTOM addresses the whole bottom
	 * group: showing it reopens every closed bottom view, hiding it closes the
	 * open ones.  Individual views are also reachable through the view.*
	 * commands. */
	VGUI_API void vgui_editor_shell_set_panel_visible(vgui_editor_shell* shell,
		vgui_editor_panel panel, int visible);
	VGUI_API int vgui_editor_shell_panel_visible(vgui_editor_shell* shell,
		vgui_editor_panel panel);

	/* Layout persistence.  A damaged or version-mismatched payload falls back to
	 * the layout captured right after build(); legacy splitter records are still
	 * accepted and mapped onto the Dock. */
	VGUI_API size_t vgui_editor_shell_capture_layout_json(vgui_editor_shell* shell,
		char* out, size_t cap);
	VGUI_API int vgui_editor_shell_apply_layout_json(vgui_editor_shell* shell,
		const char* json);
	VGUI_API int vgui_editor_shell_save_layout(vgui_editor_shell* shell,
		const char* path);
	VGUI_API int vgui_editor_shell_load_layout(vgui_editor_shell* shell,
		const char* path);
	VGUI_API int vgui_editor_shell_reset_layout(vgui_editor_shell* shell);

	/* Component accessors.  Every handle is borrowed and owned by the shell: use
	 * it, never destroy it.  They stay valid while the shell lives. */
	VGUI_API vgui_editor_outliner* vgui_editor_shell_outliner(
		vgui_editor_shell* shell);
	VGUI_API vgui_editor_property_grid* vgui_editor_shell_properties(
		vgui_editor_shell* shell);
	VGUI_API vgui_editor_asset_browser* vgui_editor_shell_assets(
		vgui_editor_shell* shell);
	VGUI_API vgui_editor_asset_preview* vgui_editor_shell_preview(
		vgui_editor_shell* shell);
	VGUI_API vgui_editor_console* vgui_editor_shell_console(
		vgui_editor_shell* shell);
	VGUI_API vgui_editor_canvas* vgui_editor_shell_canvas(vgui_editor_shell* shell);
	VGUI_API vgui_editor_table* vgui_editor_shell_table(vgui_editor_shell* shell);
	VGUI_API vgui_editor_timeline* vgui_editor_shell_timeline(
		vgui_editor_shell* shell);
	VGUI_API vgui_editor_node_graph* vgui_editor_shell_node_graph(
		vgui_editor_shell* shell);
	VGUI_API vgui_editor_code_editor* vgui_editor_shell_code(
		vgui_editor_shell* shell);
	VGUI_API vgui_editor_toolbar* vgui_editor_shell_toolbar(vgui_editor_shell* shell);
	VGUI_API vgui_editor_status_bar* vgui_editor_shell_status_bar(
		vgui_editor_shell* shell);
	VGUI_API vgui_editor_search_box* vgui_editor_shell_search(
		vgui_editor_shell* shell);
	VGUI_API vgui_editor_breadcrumb* vgui_editor_shell_path(vgui_editor_shell* shell);
	VGUI_API vgui_editor_viewport* vgui_editor_shell_viewport(vgui_editor_shell* shell);
	/* V09: a second camera surface and a depth-preview surface under the main
	 * viewport, each with its own texture and (for the preview) camera. */
	VGUI_API vgui_editor_viewport* vgui_editor_shell_preview_viewport(
		vgui_editor_shell* shell);
	VGUI_API vgui_editor_viewport* vgui_editor_shell_depth_viewport(
		vgui_editor_shell* shell);
	VGUI_API vgui_editor_camera* vgui_editor_shell_camera(vgui_editor_shell* shell);
	VGUI_API vgui_editor_camera* vgui_editor_shell_preview_camera(
		vgui_editor_shell* shell);
	VGUI_API vgui_editor_picking* vgui_editor_shell_picking(vgui_editor_shell* shell);
	VGUI_API vgui_editor_gizmo* vgui_editor_shell_gizmo(vgui_editor_shell* shell);
	VGUI_API vgui_editor_grid* vgui_editor_shell_grid(vgui_editor_shell* shell);
	VGUI_API vgui_editor_thumbnails* vgui_editor_shell_thumbnails(
		vgui_editor_shell* shell);

	VGUI_API void vgui_editor_shell_get_viewport_settings(
		vgui_editor_shell* shell, vgui_editor_viewport_settings* out);
	VGUI_API void vgui_editor_shell_set_viewport_settings(
		vgui_editor_shell* shell, const vgui_editor_viewport_settings* settings);
	/* Shared snapping policy for the gizmo, grid and canvas. */
	VGUI_API void vgui_editor_shell_get_snap(vgui_editor_shell* shell,
		vgui_editor_snap_settings* out);
	VGUI_API void vgui_editor_shell_set_snap(vgui_editor_shell* shell,
		const vgui_editor_snap_settings* settings);

	/* E05: shell-wide high-contrast palette.  Enabling stores the current theme
	 * and moves the shell chrome to black/white/yellow; disabling restores it. */
	VGUI_API int vgui_editor_shell_high_contrast(vgui_editor_shell* shell);
	VGUI_API void vgui_editor_shell_set_high_contrast(vgui_editor_shell* shell,
		int enabled);
	VGUI_API void vgui_editor_shell_set_status(vgui_editor_shell* shell,
		const char* text);

#ifdef __cplusplus
} /* extern "C" */
#endif
