/* gui/editor/editor_c_api_context.cpp - C facade for the editor foundations:
 * context/selection, command stack, command registry, binding undo, the drag
 * bridge and persistent layout. */
#include "editor_c_api.h"
#include "editor_c_api_borrow.h"

#include <deque>
#include <new>
#include <utility>

#include <vgui/editor/binding_undo.h>

using namespace vgui_editor_kit;

/* -------------------------------------------------------------------------- */
/* Handle storage                                                            */
/* -------------------------------------------------------------------------- */

/* Command stacks are either owned (standalone create) or borrowed from an
 * editor context / shell; a borrowed handle ignores destroy(). */
struct vgui_editor_command_stack {
	command_stack impl;
	command_stack* stack = &impl;
	bool borrowed = false;
};

struct vgui_editor_context {
	explicit vgui_editor_context(vgui_context* ui)
		: impl(ui)
	{
		commands = new (std::nothrow) vgui_editor_command_stack();
		if (commands)
		{
			commands->stack = &impl.commands();
			commands->borrowed = true;
		}
	}

	~vgui_editor_context()
	{
		delete commands;
	}

	editor_context impl;
	vgui_editor_command_stack* commands = nullptr;
};

struct vgui_editor_command_registry : vgeditor::c_object<command_registry> {
	using c_object::c_object;

	/* Formatted shortcut text.  A deque keeps the addresses of the strings
	 * stable, so the borrowed char* returned by entry_at() survives later
	 * inserts; the cache is rebuilt after every mutation. */
	std::deque<std::string> shortcuts;
	bool shortcuts_dirty = true;
};

struct vgui_editor_binding_undo {
	vgui_editor_binding_undo(vgui_context* ctx, command_stack& stack)
		: impl(ctx, stack)
	{
	}

	binding_undo impl;
};

struct vgui_editor_drag_target {
	vgeditor::c_drop_target impl;
	vgui_widget* widget = nullptr;
};

struct vgui_editor_layout {
	editor_layout impl;
	std::string json;
};

namespace {

	void sync_shortcuts(vgui_editor_command_registry* registry)
	{
		if (!registry->shortcuts_dirty)
			return;
		registry->shortcuts.clear();
		for (const command_entry& entry : registry->impl.entries())
			registry->shortcuts.push_back(vgeditor::shortcut_text(entry.shortcut));
		registry->shortcuts_dirty = false;
	}

	void fill_info(vgui_editor_command_registry* registry, std::size_t index,
		const command_entry& entry, vgui_editor_command_info* info)
	{
		std::memset(info, 0, sizeof(*info));
		info->struct_size = (uint32_t)sizeof(*info);
		info->id = entry.id.c_str();
		info->title = entry.title.c_str();
		info->shortcut = index < registry->shortcuts.size() ?
			registry->shortcuts[index].c_str() : "";
		info->enabled = registry->impl.is_enabled(entry.id.c_str()) ? 1 : 0;
		info->checked = registry->impl.is_checked(entry.id.c_str()) ? 1 : 0;
	}

	std::string* layout_view_state(vgui_editor_layout* layout, const char* which)
	{
		if (!layout || !which)
			return nullptr;
		const std::string key(which);
		if (key == "table")
			return &layout->impl.table_state;
		if (key == "timeline")
			return &layout->impl.timeline_state;
		if (key == "node_graph" || key == "nodegraph")
			return &layout->impl.node_graph_state;
		if (key == "code")
			return &layout->impl.code_editor_state;
		return nullptr;
	}

} /* namespace */

/* Handle internals shared with other facade translation units. */
namespace vgeditor {

	command_registry* registry_impl(vgui_editor_command_registry* registry)
	{
		return registry ? &registry->impl : nullptr;
	}

	command_stack* stack_impl(vgui_editor_command_stack* stack)
	{
		return stack ? stack->stack : nullptr;
	}

	editor_context* context_impl(vgui_editor_context* context)
	{
		return context ? &context->impl : nullptr;
	}

	vgui_editor_command_registry* borrow_command_registry(
		command_registry& registry)
	{
		return borrow_object<vgui_editor_command_registry>(registry);
	}

	void release_borrowed_command_registry(vgui_editor_command_registry* handle)
	{
		/* Wrapper only: a borrowed handle's C++ object belongs to its container. */
		if (handle && !handle->owns_object())
			delete handle;
	}


} /* namespace vgeditor */

extern "C" {

	/* ------------------------------------------------------------------ */
	/* Editor context                                                     */
	/* ------------------------------------------------------------------ */

	vgui_editor_context* vgui_editor_context_create(vgui_context* ui)
	{
		if (!ui)
			return nullptr;
		return new (std::nothrow) vgui_editor_context(ui);
	}

	void vgui_editor_context_destroy(vgui_editor_context* ctx)
	{
		delete ctx;
	}

	vgui_context* vgui_editor_context_ui(vgui_editor_context* ctx)
	{
		return ctx ? ctx->impl.ui() : nullptr;
	}

	vgui_widget* vgui_editor_context_root(vgui_editor_context* ctx)
	{
		return ctx ? ctx->impl.root() : nullptr;
	}

	vgui_editor_command_stack* vgui_editor_context_commands(vgui_editor_context* ctx)
	{
		return ctx ? ctx->commands : nullptr;
	}

	vgui_editor_object_id vgui_editor_context_selection(vgui_editor_context* ctx)
	{
		return ctx ? ctx->impl.selection() : VGUI_EDITOR_INVALID_OBJECT;
	}

	size_t vgui_editor_context_selection_count(vgui_editor_context* ctx)
	{
		return ctx ? ctx->impl.selection_count() : 0;
	}

	vgui_editor_object_id vgui_editor_context_selection_id_at(vgui_editor_context* ctx,
		size_t index)
	{
		if (!ctx)
			return VGUI_EDITOR_INVALID_OBJECT;
		const std::vector<object_id>& ids = ctx->impl.selection_ids();
		return index < ids.size() ? ids[index] : VGUI_EDITOR_INVALID_OBJECT;
	}

	int vgui_editor_context_is_selected(vgui_editor_context* ctx,
		vgui_editor_object_id id)
	{
		return ctx && ctx->impl.is_selected(id) ? 1 : 0;
	}

	void vgui_editor_context_set_selection(vgui_editor_context* ctx,
		vgui_editor_object_id id)
	{
		if (ctx)
			ctx->impl.set_selection(id);
	}

	void vgui_editor_context_set_selection_ids(vgui_editor_context* ctx,
		const vgui_editor_object_id* ids, size_t id_count,
		vgui_editor_object_id primary)
	{
		if (!ctx)
			return;
		std::vector<object_id> value;
		if (ids)
			value.assign(ids, ids + id_count);
		ctx->impl.set_selection(value, primary);
	}

	void vgui_editor_context_add_to_selection(vgui_editor_context* ctx,
		vgui_editor_object_id id)
	{
		if (ctx)
			ctx->impl.add_to_selection(id);
	}

	void vgui_editor_context_remove_from_selection(vgui_editor_context* ctx,
		vgui_editor_object_id id)
	{
		if (ctx)
			ctx->impl.remove_from_selection(id);
	}

	void vgui_editor_context_toggle_selection(vgui_editor_context* ctx,
		vgui_editor_object_id id)
	{
		if (ctx)
			ctx->impl.toggle_selection(id);
	}

	void vgui_editor_context_clear_selection(vgui_editor_context* ctx)
	{
		if (ctx)
			ctx->impl.clear_selection();
	}

	size_t vgui_editor_context_add_selection_listener(vgui_editor_context* ctx,
		vgui_editor_selection_fn fn, void* user)
	{
		if (!ctx || !fn)
			return 0;
		return ctx->impl.add_selection_listener(
			[fn, user](object_id id) { fn(user, id); });
	}

	int vgui_editor_context_remove_selection_listener(vgui_editor_context* ctx,
		size_t token)
	{
		return ctx && ctx->impl.remove_selection_listener(token) ? 1 : 0;
	}

	size_t vgui_editor_context_add_selection_set_listener(vgui_editor_context* ctx,
		vgui_editor_selection_set_fn fn, void* user)
	{
		if (!ctx || !fn)
			return 0;
		return ctx->impl.add_selection_set_listener(
			[fn, user](const std::vector<object_id>& ids) {
				fn(user, ids.empty() ? nullptr : ids.data(), ids.size());
			});
	}

	int vgui_editor_context_remove_selection_set_listener(vgui_editor_context* ctx,
		size_t token)
	{
		return ctx && ctx->impl.remove_selection_set_listener(token) ? 1 : 0;
	}

	/* ------------------------------------------------------------------ */
	/* Command stack                                                      */
	/* ------------------------------------------------------------------ */

	vgui_editor_command_stack* vgui_editor_command_stack_create(void)
	{
		return new (std::nothrow) vgui_editor_command_stack();
	}

	void vgui_editor_command_stack_destroy(vgui_editor_command_stack* stack)
	{
		if (!stack || stack->borrowed)
			return;
		delete stack;
	}

	int vgui_editor_command_stack_execute(vgui_editor_command_stack* stack,
		const char* name, vgui_editor_execute_fn execute, vgui_editor_undo_fn undo,
		void* user, vgui_editor_release_fn release_user)
	{
		if (!stack)
			return 0;
		std::unique_ptr<vgeditor::c_command> cmd(new (std::nothrow) vgeditor::c_command(
			vgeditor::to_text(name), execute, undo,
			vgeditor::make_owner(user, release_user)));
		if (!cmd)
			return 0;
		return stack->stack->execute(std::move(cmd)) ? 1 : 0;
	}

	void vgui_editor_command_stack_push(vgui_editor_command_stack* stack,
		const char* name, vgui_editor_execute_fn reapply, vgui_editor_undo_fn undo,
		void* user, vgui_editor_release_fn release_user)
	{
		if (!stack)
			return;
		std::unique_ptr<vgeditor::c_command> cmd(new (std::nothrow) vgeditor::c_command(
			vgeditor::to_text(name), reapply, undo,
			vgeditor::make_owner(user, release_user)));
		if (!cmd)
			return;
		stack->stack->push(std::move(cmd));
	}

	int vgui_editor_command_stack_undo(vgui_editor_command_stack* stack)
	{
		return stack && stack->stack->undo() ? 1 : 0;
	}

	int vgui_editor_command_stack_redo(vgui_editor_command_stack* stack)
	{
		return stack && stack->stack->redo() ? 1 : 0;
	}

	void vgui_editor_command_stack_clear(vgui_editor_command_stack* stack)
	{
		if (stack)
			stack->stack->clear();
	}

	int vgui_editor_command_stack_can_undo(vgui_editor_command_stack* stack)
	{
		return stack && stack->stack->can_undo() ? 1 : 0;
	}

	int vgui_editor_command_stack_can_redo(vgui_editor_command_stack* stack)
	{
		return stack && stack->stack->can_redo() ? 1 : 0;
	}

	size_t vgui_editor_command_stack_undo_count(vgui_editor_command_stack* stack)
	{
		return stack ? stack->stack->undo_count() : 0;
	}

	size_t vgui_editor_command_stack_redo_count(vgui_editor_command_stack* stack)
	{
		return stack ? stack->stack->redo_count() : 0;
	}

	const char* vgui_editor_command_stack_next_undo_name(vgui_editor_command_stack* stack)
	{
		if (!stack)
			return "";
		const char* name = stack->stack->next_undo_name();
		return name ? name : "";
	}

	const char* vgui_editor_command_stack_next_redo_name(vgui_editor_command_stack* stack)
	{
		if (!stack)
			return "";
		const char* name = stack->stack->next_redo_name();
		return name ? name : "";
	}

	void vgui_editor_command_stack_begin_transaction(vgui_editor_command_stack* stack,
		const char* name)
	{
		if (stack)
			stack->stack->begin_transaction(vgeditor::to_text(name));
	}

	int vgui_editor_command_stack_commit_transaction(vgui_editor_command_stack* stack)
	{
		return stack && stack->stack->commit_transaction() ? 1 : 0;
	}

	void vgui_editor_command_stack_rollback_transaction(vgui_editor_command_stack* stack)
	{
		if (stack)
			stack->stack->rollback_transaction();
	}

	int vgui_editor_command_stack_in_transaction(vgui_editor_command_stack* stack)
	{
		return stack && stack->stack->in_transaction() ? 1 : 0;
	}

	/* ------------------------------------------------------------------ */
	/* Command registry                                                   */
	/* ------------------------------------------------------------------ */

	vgui_editor_command_registry* vgui_editor_command_registry_create(void)
	{
		return vgeditor::own_object<vgui_editor_command_registry>(
			new (std::nothrow) command_registry());
	}

	void vgui_editor_command_registry_destroy(vgui_editor_command_registry* registry)
	{
		if (!registry || !registry->owns_object())
			return;
		delete registry;
	}

	int vgui_editor_command_registry_add(vgui_editor_command_registry* registry,
		const char* id, const char* title, vgui_editor_action_fn run, void* user,
		vgui_editor_release_fn release_user)
	{
		if (!registry || !id || !*id)
			return 0;
		vgeditor::user_owner_ptr owner = vgeditor::make_owner(user, release_user);
		registry->impl.add(std::string(id), vgeditor::to_text(title),
			[run, owner]() {
				if (run)
					run(owner->user);
			});
		registry->shortcuts_dirty = true;
		return 1;
	}

	int vgui_editor_command_registry_remove(vgui_editor_command_registry* registry,
		const char* id)
	{
		if (!registry)
			return 0;
		const bool removed = registry->impl.remove(id ? id : "");
		registry->shortcuts_dirty = true;
		return removed ? 1 : 0;
	}

	void vgui_editor_command_registry_clear(vgui_editor_command_registry* registry)
	{
		if (!registry)
			return;
		registry->impl.clear();
		registry->shortcuts_dirty = true;
	}

	int vgui_editor_command_registry_has(vgui_editor_command_registry* registry,
		const char* id)
	{
		return registry && registry->impl.has(id) ? 1 : 0;
	}

	size_t vgui_editor_command_registry_size(vgui_editor_command_registry* registry)
	{
		return registry ? registry->impl.size() : 0;
	}

	int vgui_editor_command_registry_entry_at(vgui_editor_command_registry* registry,
		size_t index, vgui_editor_command_info* info)
	{
		if (!registry || !info)
			return 0;
		const std::vector<command_entry>& entries = registry->impl.entries();
		if (index >= entries.size())
			return 0;
		sync_shortcuts(registry);
		fill_info(registry, index, entries[index], info);
		return 1;
	}

	int vgui_editor_command_registry_find(vgui_editor_command_registry* registry,
		const char* id, vgui_editor_command_info* info)
	{
		if (!registry || !info)
		{
			return 0;
		}
		std::memset(info, 0, sizeof(*info));
		info->struct_size = (uint32_t)sizeof(*info);
		if (!id)
			return 0;
		const std::vector<command_entry>& entries = registry->impl.entries();
		for (std::size_t i = 0; i < entries.size(); ++i)
		{
			if (entries[i].id != id)
				continue;
			sync_shortcuts(registry);
			fill_info(registry, i, entries[i], info);
			return 1;
		}
		return 0;
	}

	int vgui_editor_command_registry_set_shortcut(vgui_editor_command_registry* registry,
		const char* id, const char* shortcut)
	{
		if (!registry)
			return 0;
		const bool ok = registry->impl.set_shortcut(id,
			vgeditor::parse_shortcut(shortcut));
		registry->shortcuts_dirty = true;
		return ok ? 1 : 0;
	}

	size_t vgui_editor_command_registry_get_shortcut(
		vgui_editor_command_registry* registry, const char* id, char* out, size_t cap)
	{
		if (!registry)
			return 0;
		const command_entry* entry = registry->impl.find(id);
		if (!entry)
			return vgeditor::copy_string(out, cap, "");
		const std::string text = vgeditor::shortcut_text(entry->shortcut);
		return vgeditor::copy_string(out, cap, text.c_str());
	}

	int vgui_editor_command_registry_set_enabled(vgui_editor_command_registry* registry,
		const char* id, vgui_editor_predicate_fn predicate, void* user)
	{
		if (!registry)
			return 0;
		return registry->impl.set_enabled(id,
			[predicate, user]() {
				return predicate ? predicate(user) != 0 : true;
			}) ? 1 : 0;
	}

	int vgui_editor_command_registry_set_checked(vgui_editor_command_registry* registry,
		const char* id, vgui_editor_predicate_fn predicate, void* user)
	{
		if (!registry)
			return 0;
		return registry->impl.set_checked(id,
			[predicate, user]() {
				return predicate ? predicate(user) != 0 : true;
			}) ? 1 : 0;
	}

	int vgui_editor_command_registry_invoke(vgui_editor_command_registry* registry,
		const char* id)
	{
		return registry && registry->impl.invoke(id) ? 1 : 0;
	}

	int vgui_editor_command_registry_is_enabled(vgui_editor_command_registry* registry,
		const char* id)
	{
		return registry && registry->impl.is_enabled(id) ? 1 : 0;
	}

	int vgui_editor_command_registry_is_checked(vgui_editor_command_registry* registry,
		const char* id)
	{
		return registry && registry->impl.is_checked(id) ? 1 : 0;
	}

	int vgui_editor_command_registry_handle_key(vgui_editor_command_registry* registry,
		int key, int down, int mods)
	{
		return registry && registry->impl.handle_key(key, down, mods) ? 1 : 0;
	}

	int vgui_editor_command_registry_matches_shortcut(
		vgui_editor_command_registry* registry, int key, int mods)
	{
		return registry && registry->impl.matches_shortcut(key, mods) ? 1 : 0;
	}

	/* ------------------------------------------------------------------ */
	/* Binding undo                                                       */
	/* ------------------------------------------------------------------ */

	vgui_editor_binding_undo* vgui_editor_binding_undo_create(vgui_context* ctx,
		vgui_editor_command_stack* stack)
	{
		if (!ctx || !stack)
			return nullptr;
		return new (std::nothrow) vgui_editor_binding_undo(ctx, *stack->stack);
	}

	void vgui_editor_binding_undo_destroy(vgui_editor_binding_undo* undo)
	{
		delete undo;
	}

	void vgui_editor_binding_undo_set_label(vgui_editor_binding_undo* undo,
		const char* label)
	{
		if (undo)
			undo->impl.set_label(vgeditor::to_text(label));
	}

	const char* vgui_editor_binding_undo_label(vgui_editor_binding_undo* undo)
	{
		return undo ? undo->impl.label().c_str() : "";
	}

	void vgui_editor_binding_undo_set_merge_window(vgui_editor_binding_undo* undo,
		int milliseconds)
	{
		if (undo)
			undo->impl.set_merge_window(milliseconds);
	}

	int vgui_editor_binding_undo_merge_window(vgui_editor_binding_undo* undo)
	{
		return undo ? undo->impl.merge_window() : 0;
	}

	void vgui_editor_binding_undo_set_enabled(vgui_editor_binding_undo* undo,
		int enabled)
	{
		if (undo)
			undo->impl.set_enabled(enabled != 0);
	}

	int vgui_editor_binding_undo_enabled(vgui_editor_binding_undo* undo)
	{
		return undo && undo->impl.enabled() ? 1 : 0;
	}

	size_t vgui_editor_binding_undo_recorded(vgui_editor_binding_undo* undo)
	{
		return undo ? undo->impl.recorded() : 0;
	}

	int vgui_editor_binding_undo_attached(vgui_editor_binding_undo* undo)
	{
		return undo && undo->impl.attached() ? 1 : 0;
	}

	int vgui_editor_binding_undo_owns_recorder(vgui_context* ctx)
	{
		return binding_undo::owns_recorder(ctx) ? 1 : 0;
	}

	/* ------------------------------------------------------------------ */
	/* Drag & drop bridge                                                 */
	/* ------------------------------------------------------------------ */

	vgui_editor_drag_target* vgui_editor_drag_target_create(
		const vgui_editor_drop_target_desc* desc, void* user)
	{
		vgui_editor_drag_target* target = new (std::nothrow) vgui_editor_drag_target();
		if (!target)
			return nullptr;
		target->impl.desc = vgeditor::take_desc(desc);
		target->impl.desc.struct_size = (uint32_t)sizeof(target->impl.desc);
		target->impl.user = user;
		return target;
	}

	void vgui_editor_drag_target_destroy(vgui_editor_drag_target* target)
	{
		if (!target)
			return;
		if (target->widget)
			set_editor_drop_target(target->widget, nullptr);
		if (target->impl.desc.release_user && target->impl.user)
			target->impl.desc.release_user(target->impl.user);
		delete target;
	}

	int vgui_editor_drag_target_attach(vgui_editor_drag_target* target,
		vgui_widget* widget)
	{
		if (!target)
			return 0;
		if (target->widget)
			set_editor_drop_target(target->widget, nullptr);
		target->widget = nullptr;
		if (!widget)
			return 1;
		set_editor_drop_target(widget, &target->impl);
		target->widget = widget;
		return 1;
	}

	vgui_widget* vgui_editor_drag_target_widget(vgui_editor_drag_target* target)
	{
		return target ? target->widget : nullptr;
	}

	int vgui_editor_begin_drag(vgui_context* ctx, vgui_widget* source,
		const vgui_editor_drag_payload* payload)
	{
		if (!ctx || !payload)
			return 0;
		return begin_editor_drag(ctx, source, vgeditor::from_c_payload(payload)) ? 1 : 0;
	}

	void vgui_editor_widget_set_row_drag(vgui_widget* widget, int tree,
		vgui_editor_row_drag_fn fn, void* user)
	{
		if (!widget)
			return;
		set_editor_row_drag(widget, tree != 0,
			[fn, user](vgui_item_id item, int index, drag_payload& out) {
				vgui_editor_drag_payload view;
				vgui_editor_drag_payload_default(&view);
				if (!fn)
					return false;
				const int ok = fn(user, item, index, &view);
				out = vgeditor::from_c_payload(&view);
				return ok != 0;
			});
	}

	/* ------------------------------------------------------------------ */
	/* Persistent shell layout                                            */
	/* ------------------------------------------------------------------ */

	vgui_editor_layout* vgui_editor_layout_create(void)
	{
		return new (std::nothrow) vgui_editor_layout();
	}

	void vgui_editor_layout_destroy(vgui_editor_layout* layout)
	{
		delete layout;
	}

	int vgui_editor_layout_get_version(vgui_editor_layout* layout)
	{
		return layout ? layout->impl.version : 0;
	}

	void vgui_editor_layout_set_panel_visible(vgui_editor_layout* layout,
		vgui_editor_panel panel, int visible)
	{
		if (!layout)
			return;
		switch (panel)
		{
		case VGUI_EDITOR_PANEL_OUTLINER:
			layout->impl.outliner_visible = visible != 0;
			break;
		case VGUI_EDITOR_PANEL_VIEWPORT:
			layout->impl.viewport_visible = visible != 0;
			break;
		case VGUI_EDITOR_PANEL_PROPERTIES:
			layout->impl.properties_visible = visible != 0;
			break;
		case VGUI_EDITOR_PANEL_BOTTOM:
			layout->impl.bottom_visible = visible != 0;
			break;
		default:
			break;
		}
	}

	int vgui_editor_layout_panel_visible(vgui_editor_layout* layout,
		vgui_editor_panel panel)
	{
		if (!layout)
			return 0;
		switch (panel)
		{
		case VGUI_EDITOR_PANEL_OUTLINER:
			return layout->impl.outliner_visible ? 1 : 0;
		case VGUI_EDITOR_PANEL_VIEWPORT:
			return layout->impl.viewport_visible ? 1 : 0;
		case VGUI_EDITOR_PANEL_PROPERTIES:
			return layout->impl.properties_visible ? 1 : 0;
		case VGUI_EDITOR_PANEL_BOTTOM:
			return layout->impl.bottom_visible ? 1 : 0;
		default:
			return 0;
		}
	}

	void vgui_editor_layout_set_ratios(vgui_editor_layout* layout, float body_ratio,
		float workspace_ratio, float center_ratio)
	{
		if (!layout)
			return;
		layout->impl.body_ratio = body_ratio;
		layout->impl.workspace_ratio = workspace_ratio;
		layout->impl.center_ratio = center_ratio;
	}

	void vgui_editor_layout_get_ratios(vgui_editor_layout* layout, float* body_ratio,
		float* workspace_ratio, float* center_ratio)
	{
		if (!layout)
			return;
		if (body_ratio)
			*body_ratio = layout->impl.body_ratio;
		if (workspace_ratio)
			*workspace_ratio = layout->impl.workspace_ratio;
		if (center_ratio)
			*center_ratio = layout->impl.center_ratio;
	}

	void vgui_editor_layout_set_view_state(vgui_editor_layout* layout,
		const char* which, const char* state)
	{
		std::string* slot = layout_view_state(layout, which);
		if (slot)
			*slot = vgeditor::to_text(state);
	}

	const char* vgui_editor_layout_view_state(vgui_editor_layout* layout,
		const char* which)
	{
		const std::string* slot = layout_view_state(layout, which);
		return slot ? slot->c_str() : "";
	}

	const char* vgui_editor_layout_to_json(vgui_editor_layout* layout)
	{
		if (!layout)
			return "";
		layout->json = editor_layout_to_json(layout->impl);
		return layout->json.c_str();
	}

	int vgui_editor_layout_from_json(vgui_editor_layout* layout, const char* json)
	{
		if (!layout)
			return 0;
		std::string error;
		if (!editor_layout_from_json(vgeditor::to_text(json), layout->impl, &error))
		{
			vgeditor::set_error(error.empty() ? "invalid editor layout json" : error);
			return 0;
		}
		vgeditor::error_storage().clear();
		return 1;
	}

	int vgui_editor_layout_save_file(vgui_editor_layout* layout, const char* path)
	{
		if (!layout || !path || !*path)
			return 0;
		std::string error;
		if (!editor_layout_save_file(path, layout->impl, &error))
		{
			vgeditor::set_error(error.empty() ? "layout save failed" : error);
			return 0;
		}
		vgeditor::error_storage().clear();
		return 1;
	}

	int vgui_editor_layout_load_file(vgui_editor_layout* layout, const char* path)
	{
		if (!layout || !path || !*path)
			return 0;
		std::string error;
		if (!editor_layout_load_file(path, layout->impl, &error))
		{
			vgeditor::set_error(error.empty() ? "layout load failed" : error);
			return 0;
		}
		vgeditor::error_storage().clear();
		return 1;
	}

} /* extern "C" */
