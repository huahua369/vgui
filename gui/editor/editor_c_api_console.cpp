/* gui/editor/editor_c_api_console.cpp - C facade for the editor log console
 * declared in <vgui/vgui_editor.h>.
 *
 * The console keeps its records in the C++ view; entry_at() copies a record into
 * handle-owned scratch strings whose pointers the caller borrows until the next
 * mutation.  append() stays callable from any thread because the kit queues the
 * record behind a mutex. */
#include "editor_c_api.h"
#include "editor_c_api_borrow.h"

#include <new>
#include <utility>
#include <vector>

#include <vgui/editor/console_view.h>

using namespace vgui_editor_kit;

/* -------------------------------------------------------------------------- */
/* Handle storage                                                            */
/* -------------------------------------------------------------------------- */

struct vgui_editor_console : vgeditor::c_object<console_view> {
	using c_object::c_object;

	/* Borrowed-string storage for the last entry_at() call. */
	std::string message;
	std::string source;
	std::string timestamp;
	std::string path;
	std::vector<std::string> frame_functions;
	std::vector<std::string> frame_paths;
	std::vector<vgui_editor_log_frame> frames;
	vgui_editor_jump_fn jump_fn = nullptr;
	void* jump_user = nullptr;
};

namespace {

	static_assert((int)VGUI_EDITOR_LOG_TRACE == (int)console_view::level::trace,
		"log level drift");
	static_assert((int)VGUI_EDITOR_LOG_INFO == (int)console_view::level::info,
		"log level drift");
	static_assert((int)VGUI_EDITOR_LOG_ERROR == (int)console_view::level::error,
		"log level drift");

	console_view::level to_level(vgui_editor_log_level level)
	{
		switch (level) {
		case VGUI_EDITOR_LOG_TRACE:
			return console_view::level::trace;
		case VGUI_EDITOR_LOG_DEBUG:
			return console_view::level::debug;
		case VGUI_EDITOR_LOG_WARNING:
			return console_view::level::warning;
		case VGUI_EDITOR_LOG_ERROR:
			return console_view::level::error;
		default:
			return console_view::level::info;
		}
	}

	vgui_editor_log_level from_level(console_view::level level)
	{
		switch (level) {
		case console_view::level::trace:
			return VGUI_EDITOR_LOG_TRACE;
		case console_view::level::debug:
			return VGUI_EDITOR_LOG_DEBUG;
		case console_view::level::warning:
			return VGUI_EDITOR_LOG_WARNING;
		case console_view::level::error:
			return VGUI_EDITOR_LOG_ERROR;
		default:
			return VGUI_EDITOR_LOG_INFO;
		}
	}

	console_view::location to_location(const vgui_editor_log_location& where)
	{
		console_view::location out;
		out.asset_id = (vgui_item_id)where.asset_id;
		out.path = vgeditor::to_text(where.path);
		out.line = where.line;
		out.column = where.column;
		return out;
	}

	console_view::entry to_entry(const vgui_editor_log_entry* src)
	{
		console_view::entry out;
		if (!src)
			return out;
		const vgui_editor_log_entry d = vgeditor::take_desc(src);
		out.severity = to_level(d.level);
		out.message = vgeditor::to_text(d.message);
		out.source = vgeditor::to_text(d.source);
		out.timestamp = vgeditor::to_text(d.timestamp);
		out.where = to_location(d.where);
		if (d.frames) {
			out.stack.reserve(d.frame_count);
			for (size_t i = 0; i < d.frame_count; ++i) {
				const vgui_editor_log_frame frame = vgeditor::take_desc(
					d.frames + i);
				console_view::stack_frame value;
				value.function = vgeditor::to_text(frame.function);
				value.where = to_location(frame.where);
				out.stack.push_back(std::move(value));
			}
		}
		return out;
	}

	/* Fills the handle scratch, then points the C view at it.  The string vectors
	 * are reserved up front so pushing never invalidates an earlier c_str(). */
	void capture_entry(vgui_editor_console* console, const console_view::entry& value,
		vgui_editor_log_entry* out)
	{
		console->message = value.message;
		console->source = value.source;
		console->timestamp = value.timestamp;
		console->path = value.where.path;

		const size_t count = value.stack.size();
		console->frame_functions.clear();
		console->frame_functions.reserve(count);
		console->frame_paths.clear();
		console->frame_paths.reserve(count);
		for (const console_view::stack_frame& frame : value.stack) {
			console->frame_functions.push_back(frame.function);
			console->frame_paths.push_back(frame.where.path);
		}

		console->frames.clear();
		console->frames.resize(count);
		for (size_t i = 0; i < count; ++i) {
			const console_view::stack_frame& frame = value.stack[i];
			vgui_editor_log_frame& out_frame = console->frames[i];
			std::memset(&out_frame, 0, sizeof(out_frame));
			out_frame.struct_size = (uint32_t)sizeof(out_frame);
			out_frame.function = console->frame_functions[i].c_str();
			out_frame.where.struct_size = (uint32_t)sizeof(out_frame.where);
			out_frame.where.asset_id = (vgui_editor_object_id)frame.where.asset_id;
			out_frame.where.path = console->frame_paths[i].c_str();
			out_frame.where.line = frame.where.line;
			out_frame.where.column = frame.where.column;
		}

		std::memset(out, 0, sizeof(*out));
		out->struct_size = (uint32_t)sizeof(*out);
		out->level = from_level(value.severity);
		out->message = console->message.c_str();
		out->source = console->source.c_str();
		out->timestamp = console->timestamp.c_str();
		out->where.struct_size = (uint32_t)sizeof(out->where);
		out->where.asset_id = (vgui_editor_object_id)value.where.asset_id;
		out->where.path = console->path.c_str();
		out->where.line = value.where.line;
		out->where.column = value.where.column;
		out->frames = console->frames.empty() ? nullptr : console->frames.data();
		out->frame_count = console->frames.size();
	}

} /* namespace */

extern "C" {

	vgui_editor_console* vgui_editor_console_create(vgui_widget* parent)
	{
		if (!parent)
			return nullptr;
		std::unique_ptr<vgui_editor_console> console(
			vgeditor::own_object<vgui_editor_console>(
				new (std::nothrow) console_view()));
		if (!console)
			return nullptr;
		console->impl.build(parent);
		if (!console->impl.widget())
			return nullptr;
		return console.release();
	}

	void vgui_editor_console_destroy(vgui_editor_console* console)
	{
		/* A shell-borrowed handle must not tear down the shell's console. */
		if (!console || !console->owns_object())
			return;
		vgui_widget* widget = console->impl.widget();
		console->impl.set_jump_handler(nullptr);
		delete console;
		if (widget)
			vgui_widget_destroy(widget);
	}

	vgui_widget* vgui_editor_console_widget(vgui_editor_console* console)
	{
		return console ? console->impl.widget() : nullptr;
	}

	vgui_widget* vgui_editor_console_tree(vgui_editor_console* console)
	{
		return console ? console->impl.tree() : nullptr;
	}

	vgui_widget* vgui_editor_console_ticker(vgui_editor_console* console)
	{
		return console ? console->impl.ticker() : nullptr;
	}

	vgui_widget* vgui_editor_console_search_widget(vgui_editor_console* console)
	{
		return console ? console->impl.search().widget() : nullptr;
	}

	vgui_widget* vgui_editor_console_auto_scroll_widget(
		vgui_editor_console* console)
	{
		return console ? console->impl.auto_scroll_widget() : nullptr;
	}

	vgui_widget* vgui_editor_console_level_widget(vgui_editor_console* console,
		vgui_editor_log_level level)
	{
		if (!console)
			return nullptr;
		return console->impl.level_widget(to_level(level));
	}

	vgui_widget* vgui_editor_console_copy_widget(vgui_editor_console* console)
	{
		return console ? console->impl.copy_widget() : nullptr;
	}

	vgui_widget* vgui_editor_console_clear_widget(vgui_editor_console* console)
	{
		return console ? console->impl.clear_widget() : nullptr;
	}

	vgui_editor_object_id vgui_editor_console_append(vgui_editor_console* console,
		const vgui_editor_log_entry* entry)
	{
		if (!console || !entry)
			return VGUI_EDITOR_INVALID_OBJECT;
		return (vgui_editor_object_id)console->impl.append(to_entry(entry));
	}

	vgui_editor_object_id vgui_editor_console_append_message(
		vgui_editor_console* console, vgui_editor_log_level level,
		const char* message, const char* source)
	{
		if (!console)
			return VGUI_EDITOR_INVALID_OBJECT;
		return (vgui_editor_object_id)console->impl.append(to_level(level),
			vgeditor::to_text(message), vgeditor::to_text(source));
	}

	size_t vgui_editor_console_poll(vgui_editor_console* console)
	{
		return console ? console->impl.poll() : 0;
	}

	void vgui_editor_console_tick(vgui_editor_console* console, float delta_time)
	{
		if (!console)
			return;
		console->impl.tick(delta_time);
	}

	int vgui_editor_console_idle(vgui_editor_console* console)
	{
		return console && console->impl.idle() ? 1 : 0;
	}

	size_t vgui_editor_console_pending_count(vgui_editor_console* console)
	{
		return console ? console->impl.pending_count() : 0;
	}

	size_t vgui_editor_console_entry_count(vgui_editor_console* console)
	{
		return console ? console->impl.entry_count() : 0;
	}

	size_t vgui_editor_console_visible_entry_count(vgui_editor_console* console)
	{
		return console ? console->impl.visible_entry_count() : 0;
	}

	size_t vgui_editor_console_visible_row_count(vgui_editor_console* console)
	{
		return console ? console->impl.visible_row_count() : 0;
	}

	size_t vgui_editor_console_dropped_count(vgui_editor_console* console)
	{
		return console ? console->impl.dropped_count() : 0;
	}

	vgui_editor_object_id vgui_editor_console_last_entry_id(
		vgui_editor_console* console)
	{
		return console ? (vgui_editor_object_id)console->impl.last_entry_id() :
			VGUI_EDITOR_INVALID_OBJECT;
	}

	int vgui_editor_console_entry_at(vgui_editor_console* console,
		vgui_editor_object_id entry_id, vgui_editor_log_entry* out)
	{
		if (!console || !out)
			return 0;
		console_view::entry value;
		if (!console->impl.entry_at((vgui_item_id)entry_id, &value))
			return 0;
		capture_entry(console, value, out);
		return 1;
	}

	void vgui_editor_console_clear(vgui_editor_console* console)
	{
		if (!console)
			return;
		console->impl.clear();
	}

	void vgui_editor_console_set_max_entries(vgui_editor_console* console,
		size_t max_entries)
	{
		if (!console)
			return;
		console->impl.set_max_entries(max_entries);
	}

	size_t vgui_editor_console_max_entries(vgui_editor_console* console)
	{
		return console ? console->impl.max_entries() : 0;
	}

	void vgui_editor_console_set_drain_batch(vgui_editor_console* console,
		size_t count)
	{
		if (!console)
			return;
		console->impl.set_drain_batch(count);
	}

	size_t vgui_editor_console_drain_batch(vgui_editor_console* console)
	{
		return console ? console->impl.drain_batch() : 0;
	}

	void vgui_editor_console_set_level_enabled(vgui_editor_console* console,
		vgui_editor_log_level level, int enabled)
	{
		if (!console)
			return;
		console->impl.set_level_enabled(to_level(level), enabled != 0);
	}

	int vgui_editor_console_level_enabled(vgui_editor_console* console,
		vgui_editor_log_level level)
	{
		if (!console)
			return 0;
		return console->impl.level_enabled(to_level(level)) ? 1 : 0;
	}

	void vgui_editor_console_set_search_text(vgui_editor_console* console,
		const char* text)
	{
		if (!console)
			return;
		console->impl.set_search_text(vgeditor::to_text(text));
	}

	const char* vgui_editor_console_search_text(vgui_editor_console* console)
	{
		return console ? console->impl.search_text().c_str() : "";
	}

	int vgui_editor_console_filter_active(vgui_editor_console* console)
	{
		return console && console->impl.filter_active() ? 1 : 0;
	}

	void vgui_editor_console_set_auto_scroll(vgui_editor_console* console,
		int enabled)
	{
		if (!console)
			return;
		console->impl.set_auto_scroll(enabled != 0);
	}

	int vgui_editor_console_auto_scroll(vgui_editor_console* console)
	{
		return console && console->impl.auto_scroll() ? 1 : 0;
	}

	void vgui_editor_console_scroll_to_latest(vgui_editor_console* console)
	{
		if (!console)
			return;
		console->impl.scroll_to_latest();
	}

	int vgui_editor_console_stack_expanded(vgui_editor_console* console,
		vgui_editor_object_id entry_id)
	{
		if (!console)
			return 0;
		return console->impl.stack_expanded((vgui_item_id)entry_id) ? 1 : 0;
	}

	int vgui_editor_console_set_stack_expanded(vgui_editor_console* console,
		vgui_editor_object_id entry_id, int expanded)
	{
		if (!console)
			return 0;
		return console->impl.set_stack_expanded((vgui_item_id)entry_id,
			expanded != 0) ? 1 : 0;
	}

	int vgui_editor_console_toggle_stack(vgui_editor_console* console,
		vgui_editor_object_id entry_id)
	{
		if (!console)
			return 0;
		return console->impl.toggle_stack((vgui_item_id)entry_id) ? 1 : 0;
	}

	size_t vgui_editor_console_selected_text(vgui_editor_console* console,
		char* out, size_t cap)
	{
		if (!console)
			return 0;
		const std::string text = console->impl.selected_text();
		return vgeditor::copy_string(out, cap, text.c_str());
	}

	size_t vgui_editor_console_visible_text(vgui_editor_console* console,
		char* out, size_t cap)
	{
		if (!console)
			return 0;
		const std::string text = console->impl.visible_text();
		return vgeditor::copy_string(out, cap, text.c_str());
	}

	int vgui_editor_console_copy_selection(vgui_editor_console* console)
	{
		if (!console)
			return 0;
		return console->impl.copy_selection() ? 1 : 0;
	}

	int vgui_editor_console_activate_row(vgui_editor_console* console,
		vgui_editor_object_id row_id)
	{
		if (!console)
			return 0;
		return console->impl.activate_row((vgui_item_id)row_id) ? 1 : 0;
	}

	int vgui_editor_console_activate_selected(vgui_editor_console* console)
	{
		if (!console)
			return 0;
		return console->impl.activate_selected() ? 1 : 0;
	}

	void vgui_editor_console_set_jump_handler(vgui_editor_console* console,
		vgui_editor_jump_fn fn, void* user)
	{
		if (!console)
			return;
		console->jump_fn = fn;
		console->jump_user = user;
		console->impl.set_jump_handler([console](const console_view::location& where) {
			if (!console->jump_fn)
				return false;
			vgui_editor_log_location view;
			std::memset(&view, 0, sizeof(view));
			view.struct_size = (uint32_t)sizeof(view);
			view.asset_id = (vgui_editor_object_id)where.asset_id;
			view.path = where.path.c_str();
			view.line = where.line;
			view.column = where.column;
			return console->jump_fn(console->jump_user, &view) != 0;
		});
	}

	void vgui_editor_console_set_level_color(vgui_editor_console* console,
		vgui_editor_log_level level, vgui_color color)
	{
		if (!console)
			return;
		console->impl.set_level_color(to_level(level), color);
	}

	vgui_color vgui_editor_console_level_color(vgui_editor_console* console,
		vgui_editor_log_level level)
	{
		if (!console)
			return 0;
		return console->impl.level_color(to_level(level));
	}

	vgui_editor_object_id vgui_editor_console_row_id_at(
		vgui_editor_console* console, int visible_index)
	{
		if (!console)
			return VGUI_EDITOR_INVALID_OBJECT;
		return (vgui_editor_object_id)console->impl.row_id_at(visible_index);
	}

	int vgui_editor_console_row_entry(vgui_editor_console* console,
		vgui_editor_object_id row_id, vgui_editor_object_id* entry_id, int* frame)
	{
		if (!console)
			return 0;
		vgui_item_id id = 0;
		int index = -1;
		if (!console->impl.row_entry((vgui_item_id)row_id, &id, &index))
			return 0;
		if (entry_id)
			*entry_id = (vgui_editor_object_id)id;
		if (frame)
			*frame = index;
		return 1;
	}

	const char* vgui_editor_log_level_name(vgui_editor_log_level level)
	{
		return console_view::level_name(to_level(level));
	}

} /* extern "C" */

namespace vgeditor {

	vgui_editor_console* borrow_console(vek::console_view& console)
	{
		return borrow_object<vgui_editor_console>(console);
	}

	void release_borrowed_console(vgui_editor_console* handle)
	{
		/* Wrapper only: a borrowed handle's C++ object belongs to its container. */
		if (handle && !handle->owns_object())
			delete handle;
	}


} /* namespace vgeditor */
