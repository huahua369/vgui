/* gui/editor/editor_c_api_code.cpp - C facade for the code editor and markdown
 * preview declared in <vgui/vgui_editor.h>.
 *
 * Both views keep their document on the C++ side, so every text operation is a
 * copy: inputs go through to_text(), getters use the caller-buffer convention and
 * return the length the full text needs.  Nothing borrows handle-owned strings,
 * so no scratch is needed.
 * The C token/span structs travel by value: vgui_editor_code_span has no
 * struct_size field, so it converts field by field instead of through take_desc().
 * Kit callbacks capture fn/user/owner by value and never the handle, because a
 * shell-borrowed handle can die before the view. */
#include "editor_c_api.h"
#include "editor_c_api_borrow.h"

#include <cstring>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include <vgui/editor/code_editor.h>
#include <vgui/editor/markdown_view.h>

using namespace vgui_editor_kit;

/* -------------------------------------------------------------------------- */
/* Handle storage                                                             */
/* -------------------------------------------------------------------------- */

struct vgui_editor_code_editor : vgeditor::c_object<code_editor> {
	using c_object::c_object;

	/* C callback slots.  The clipboard getter and setter share one user pointer,
	 * so the getter's slot also owns its lifetime. */
	vgeditor::callback_slot<vgui_editor_code_caret_fn> on_caret;
	vgeditor::callback_slot<vgui_editor_code_change_fn> on_change;
	vgeditor::callback_slot<vgui_editor_clipboard_get_fn> clip_get;
	vgui_editor_clipboard_set_fn clip_set = nullptr;
};

struct vgui_editor_markdown : vgeditor::c_object<markdown_view> {
	using c_object::c_object;

	vgeditor::callback_slot<vgui_editor_md_clipboard_fn> clip;
	vgeditor::callback_slot<vgui_editor_md_resolve_fn> resolve;
};

namespace {

	/* The kit enumerators and the C enumerators are pinned to the same numbering
	 * below, so the conversions clamp and cast instead of switching. */
	static_assert((int)VGUI_EDITOR_CODE_PLAIN == (int)code_language::plain,
		"language drift");
	static_assert((int)VGUI_EDITOR_CODE_CPP == (int)code_language::cpp,
		"language drift");
	static_assert((int)VGUI_EDITOR_CODE_CSHARP == (int)code_language::csharp,
		"language drift");
	static_assert((int)VGUI_EDITOR_CODE_GLSL == (int)code_language::glsl,
		"language drift");
	static_assert((int)VGUI_EDITOR_CODE_JSON == (int)code_language::json,
		"language drift");
	static_assert((int)VGUI_EDITOR_CODE_LUA == (int)code_language::lua,
		"language drift");
	static_assert((int)VGUI_EDITOR_CODE_PYTHON == (int)code_language::python,
		"language drift");
	static_assert((int)VGUI_EDITOR_CODE_INI == (int)code_language::ini,
		"language drift");
	static_assert((int)VGUI_EDITOR_CODE_MARKDOWN == (int)code_language::markdown,
		"language drift");

	static_assert((int)VGUI_EDITOR_CODE_TOKEN_TEXT ==
		(int)code_token_kind::text, "token kind drift");
	static_assert((int)VGUI_EDITOR_CODE_TOKEN_KEYWORD ==
		(int)code_token_kind::keyword, "token kind drift");
	static_assert((int)VGUI_EDITOR_CODE_TOKEN_TYPE ==
		(int)code_token_kind::type, "token kind drift");
	static_assert((int)VGUI_EDITOR_CODE_TOKEN_STRING_LITERAL ==
		(int)code_token_kind::string_literal, "token kind drift");
	static_assert((int)VGUI_EDITOR_CODE_TOKEN_NUMBER ==
		(int)code_token_kind::number, "token kind drift");
	static_assert((int)VGUI_EDITOR_CODE_TOKEN_COMMENT ==
		(int)code_token_kind::comment, "token kind drift");
	static_assert((int)VGUI_EDITOR_CODE_TOKEN_PREPROCESSOR ==
		(int)code_token_kind::preprocessor, "token kind drift");
	static_assert((int)VGUI_EDITOR_CODE_TOKEN_FUNCTION ==
		(int)code_token_kind::function, "token kind drift");
	static_assert((int)VGUI_EDITOR_CODE_TOKEN_OPERATOR ==
		(int)code_token_kind::operator_token, "token kind drift");
	static_assert((int)VGUI_EDITOR_CODE_TOKEN_PUNCTUATION ==
		(int)code_token_kind::punctuation, "token kind drift");
	static_assert((int)VGUI_EDITOR_CODE_TOKEN_ATTRIBUTE ==
		(int)code_token_kind::attribute, "token kind drift");
	static_assert((int)VGUI_EDITOR_CODE_TOKEN_TAG ==
		(int)code_token_kind::tag, "token kind drift");
	static_assert((int)VGUI_EDITOR_CODE_TOKEN_HEADING ==
		(int)code_token_kind::heading, "token kind drift");
	static_assert((int)VGUI_EDITOR_CODE_TOKEN_HEADING_MARKER ==
		(int)code_token_kind::heading_marker, "token kind drift");
	static_assert((int)VGUI_EDITOR_CODE_TOKEN_EMPHASIS ==
		(int)code_token_kind::emphasis, "token kind drift");
	static_assert((int)VGUI_EDITOR_CODE_TOKEN_STRONG ==
		(int)code_token_kind::strong, "token kind drift");
	static_assert((int)VGUI_EDITOR_CODE_TOKEN_STRIKE ==
		(int)code_token_kind::strike, "token kind drift");
	static_assert((int)VGUI_EDITOR_CODE_TOKEN_LINK ==
		(int)code_token_kind::link, "token kind drift");
	static_assert((int)VGUI_EDITOR_CODE_TOKEN_LINK_URL ==
		(int)code_token_kind::link_url, "token kind drift");
	static_assert((int)VGUI_EDITOR_CODE_TOKEN_INLINE_CODE ==
		(int)code_token_kind::inline_code, "token kind drift");
	static_assert((int)VGUI_EDITOR_CODE_TOKEN_QUOTE ==
		(int)code_token_kind::quote, "token kind drift");
	static_assert((int)VGUI_EDITOR_CODE_TOKEN_LIST_MARKER ==
		(int)code_token_kind::list_marker, "token kind drift");
	static_assert((int)VGUI_EDITOR_CODE_TOKEN_TABLE ==
		(int)code_token_kind::table, "token kind drift");
	static_assert((int)VGUI_EDITOR_CODE_TOKEN_HORIZONTAL_RULE ==
		(int)code_token_kind::horizontal_rule, "token kind drift");
	static_assert(VGUI_EDITOR_CODE_TOKEN_KIND_COUNT == code_token_kind_count,
		"token kind count drift");

	static_assert((int)VGUI_EDITOR_LOG_TRACE == (int)code_log_level::trace,
		"log level drift");
	static_assert((int)VGUI_EDITOR_LOG_DEBUG == (int)code_log_level::debug,
		"log level drift");
	static_assert((int)VGUI_EDITOR_LOG_INFO == (int)code_log_level::info,
		"log level drift");
	static_assert((int)VGUI_EDITOR_LOG_WARNING == (int)code_log_level::warning,
		"log level drift");
	static_assert((int)VGUI_EDITOR_LOG_ERROR == (int)code_log_level::error,
		"log level drift");
	static_assert(VGUI_EDITOR_LOG_LEVEL_COUNT == code_log_level_count,
		"log level count drift");

	static_assert((int)VGUI_EDITOR_MD_IMAGE == (int)md_media_kind::image,
		"media kind drift");
	static_assert((int)VGUI_EDITOR_MD_VIDEO == (int)md_media_kind::video,
		"media kind drift");

	static_assert((int)VGUI_EDITOR_MD_PENDING == (int)md_media_status::pending,
		"media status drift");
	static_assert((int)VGUI_EDITOR_MD_READY == (int)md_media_status::ready,
		"media status drift");
	static_assert((int)VGUI_EDITOR_MD_FAILED == (int)md_media_status::failed,
		"media status drift");

	code_language to_language(vgui_editor_code_language language)
	{
		const int value = (int)language;
		const int last = (int)code_language::markdown;
		return (code_language)(value < 0 || value > last ? 0 : value);
	}

	vgui_editor_code_language from_language(code_language language)
	{
		const int value = (int)language;
		const int last = (int)code_language::markdown;
		return (vgui_editor_code_language)(value < 0 || value > last ? 0 : value);
	}

	code_token_kind to_token_kind(vgui_editor_code_token_kind kind)
	{
		const int value = (int)kind;
		return (code_token_kind)(value < 0 || value >= code_token_kind_count ?
			0 : value);
	}

	vgui_editor_code_token_kind from_token_kind(code_token_kind kind)
	{
		const int value = (int)kind;
		return (vgui_editor_code_token_kind)(value < 0 ||
			value >= code_token_kind_count ? 0 : value);
	}

	/* No C call reports a log level back to the host, so only the inbound
	 * direction is converted. */
	code_log_level to_log_level(vgui_editor_log_level level)
	{
		const int value = (int)level;
		return (code_log_level)(value < 0 || value >= code_log_level_count ?
			(int)code_log_level::info : value);
	}

	/* The resolver is the only place a media kind crosses back to C. */
	vgui_editor_md_media_kind from_media_kind(md_media_kind kind)
	{
		return kind == md_media_kind::video ? VGUI_EDITOR_MD_VIDEO :
			VGUI_EDITOR_MD_IMAGE;
	}

	md_media_status to_media_status(vgui_editor_md_media_status status)
	{
		switch (status) {
		case VGUI_EDITOR_MD_READY:
			return md_media_status::ready;
		case VGUI_EDITOR_MD_FAILED:
			return md_media_status::failed;
		default:
			return md_media_status::pending;
		}
	}

	code_position to_position(const vgui_editor_code_position& position)
	{
		code_position out;
		out.line = position.line;
		out.column = position.column;
		return out;
	}

	vgui_editor_code_position from_position(const code_position& position)
	{
		vgui_editor_code_position out;
		out.line = position.line;
		out.column = position.column;
		return out;
	}

	code_span to_span(const vgui_editor_code_span& src)
	{
		code_span out;
		out.start = src.start;
		out.length = src.length;
		out.kind = to_token_kind(src.kind);
		out.color = src.color;
		return out;
	}

	void fill_span(const code_span& src, vgui_editor_code_span* out)
	{
		out->start = src.start;
		out->length = src.length;
		out->kind = from_token_kind(src.kind);
		out->color = src.color;
	}

	void fill_match(const code_match& src, vgui_editor_code_match* out)
	{
		out->line = src.line;
		out->column = src.column;
		out->length = src.length;
	}

	/* Turns the host's media struct into the kit record.  The C view is only valid
	 * for the duration of the resolver call, and the kit copies `error`, so the
	 * string is taken here rather than borrowed. */
	md_media_info to_media(const vgui_editor_md_media* src)
	{
		md_media_info out;
		if (!src)
			return out;
		const vgui_editor_md_media d = vgeditor::take_desc(src);
		out.status = to_media_status(d.status);
		out.texture = d.texture;
		out.pixel_width = d.pixel_width;
		out.pixel_height = d.pixel_height;
		out.aspect = d.aspect;
		out.error = vgeditor::to_text(d.error);
		return out;
	}

} /* namespace */

extern "C" {

	const char* vgui_editor_code_language_name(vgui_editor_code_language language)
	{
		return code_language_name(to_language(language));
	}

	int vgui_editor_code_language_from_name(const char* name,
		vgui_editor_code_language* out)
	{
		if (!name)
			return 0;
		code_language found = code_language::plain;
		if (!code_language_from_name(vgeditor::to_text(name), &found))
			return 0;
		if (out)
			*out = from_language(found);
		return 1;
	}

	vgui_editor_code_language vgui_editor_code_language_from_path(const char* path)
	{
		return from_language(code_language_from_path(vgeditor::to_text(path)));
	}

	const char* vgui_editor_code_token_kind_name(vgui_editor_code_token_kind kind)
	{
		return code_token_kind_name(to_token_kind(kind));
	}

	int vgui_editor_code_token_kind_from_name(const char* name,
		vgui_editor_code_token_kind* out)
	{
		if (!name)
			return 0;
		code_token_kind found = code_token_kind::text;
		if (!code_token_kind_from_name(vgeditor::to_text(name), &found))
			return 0;
		if (out)
			*out = from_token_kind(found);
		return 1;
	}

	vgui_editor_code_editor* vgui_editor_code_editor_create(vgui_widget* parent)
	{
		if (!parent)
			return nullptr;
		std::unique_ptr<vgui_editor_code_editor> editor(
			vgeditor::own_object<vgui_editor_code_editor>(
				new (std::nothrow) code_editor()));
		if (!editor)
			return nullptr;
		editor->impl.build(parent);
		if (!editor->impl.widget())
			return nullptr;
		return editor.release();
	}

	void vgui_editor_code_editor_destroy(vgui_editor_code_editor* editor)
	{
		/* A shell-borrowed handle must not tear down the shell's editor. */
		if (!editor || !editor->owns_object())
			return;
		editor->impl.set_caret_listener(nullptr);
		editor->impl.set_change_listener(nullptr);
		editor->impl.set_clipboard_handlers(nullptr, nullptr);
		editor->impl.set_command_stack(nullptr);
		editor->on_caret.clear();
		editor->on_change.clear();
		editor->clip_get.clear();
		editor->clip_set = nullptr;
		vgui_widget* widget = editor->impl.widget();
		delete editor;
		if (widget)
			vgui_widget_destroy(widget);
	}

	vgui_widget* vgui_editor_code_editor_widget(vgui_editor_code_editor* editor)
	{
		return editor ? editor->impl.widget() : nullptr;
	}

	vgui_widget* vgui_editor_code_editor_inner(vgui_editor_code_editor* editor)
	{
		return editor ? editor->impl.editor_widget() : nullptr;
	}

	vgui_widget* vgui_editor_code_editor_find_bar(vgui_editor_code_editor* editor)
	{
		return editor ? editor->impl.find_bar() : nullptr;
	}

	int vgui_editor_code_editor_built(vgui_editor_code_editor* editor)
	{
		return editor && editor->impl.built() ? 1 : 0;
	}

	void vgui_editor_code_editor_focus(vgui_editor_code_editor* editor)
	{
		if (!editor)
			return;
		editor->impl.request_focus();
	}

	int vgui_editor_code_editor_focused(vgui_editor_code_editor* editor)
	{
		return editor && editor->impl.focused() ? 1 : 0;
	}

	void vgui_editor_code_editor_set_text(vgui_editor_code_editor* editor,
		const char* text)
	{
		if (!editor)
			return;
		editor->impl.set_text(vgeditor::to_text(text));
	}

	size_t vgui_editor_code_editor_text(vgui_editor_code_editor* editor,
		char* out, size_t cap)
	{
		if (!editor)
			return 0;
		return vgeditor::copy_string(out, cap, editor->impl.text().c_str());
	}

	void vgui_editor_code_editor_clear(vgui_editor_code_editor* editor)
	{
		if (!editor)
			return;
		editor->impl.clear();
	}

	void vgui_editor_code_editor_append_text(vgui_editor_code_editor* editor,
		const char* text)
	{
		if (!editor)
			return;
		editor->impl.append_text(vgeditor::to_text(text));
	}

	size_t vgui_editor_code_editor_line_count(vgui_editor_code_editor* editor)
	{
		return editor ? editor->impl.line_count() : 0;
	}

	size_t vgui_editor_code_editor_line_text(vgui_editor_code_editor* editor,
		int line, char* out, size_t cap)
	{
		if (!editor)
			return 0;
		const std::string text = editor->impl.line_text(line);
		return vgeditor::copy_string(out, cap, text.c_str());
	}

	int vgui_editor_code_editor_set_line_text(vgui_editor_code_editor* editor,
		int line, const char* text)
	{
		if (!editor)
			return 0;
		return editor->impl.set_line_text(line, vgeditor::to_text(text)) ? 1 : 0;
	}

	size_t vgui_editor_code_editor_line_spans(vgui_editor_code_editor* editor,
		int line, vgui_editor_code_span* out, size_t cap)
	{
		if (!editor)
			return 0;
		const std::vector<code_span>& spans = editor->impl.line_spans(line);
		if (!out)
			return spans.size();
		const size_t count = spans.size() < cap ? spans.size() : cap;
		for (size_t i = 0; i < count; ++i)
			fill_span(spans[i], out + i);
		/* Like the other array getters, report the true count so a caller that
		 * passed a short buffer knows how many spans it missed. */
		return spans.size();
	}

	int vgui_editor_code_editor_set_line_spans(vgui_editor_code_editor* editor,
		int line, const vgui_editor_code_span* spans, size_t count)
	{
		if (!editor)
			return 0;
		std::vector<code_span> value;
		if (spans) {
			value.reserve(count);
			for (size_t i = 0; i < count; ++i)
				value.push_back(to_span(spans[i]));
		}
		return editor->impl.set_line_spans(line, std::move(value)) ? 1 : 0;
	}

	int vgui_editor_code_editor_highlight_valid(vgui_editor_code_editor* editor,
		size_t* count)
	{
		if (!editor)
			return 0;
		return editor->impl.highlight_valid_upto(count) ? 1 : 0;
	}

	void vgui_editor_code_editor_set_language(vgui_editor_code_editor* editor,
		vgui_editor_code_language language)
	{
		if (!editor)
			return;
		editor->impl.set_language(to_language(language));
	}

	vgui_editor_code_language vgui_editor_code_editor_language(
		vgui_editor_code_editor* editor)
	{
		if (!editor)
			return VGUI_EDITOR_CODE_PLAIN;
		return from_language(editor->impl.language());
	}

	void vgui_editor_code_editor_set_language_from_path(
		vgui_editor_code_editor* editor, const char* path)
	{
		if (!editor)
			return;
		editor->impl.set_language_from_path(vgeditor::to_text(path));
	}

	void vgui_editor_code_editor_set_read_only(vgui_editor_code_editor* editor,
		int read_only)
	{
		if (!editor)
			return;
		editor->impl.set_read_only(read_only != 0);
	}

	int vgui_editor_code_editor_read_only(vgui_editor_code_editor* editor)
	{
		return editor && editor->impl.read_only() ? 1 : 0;
	}

	void vgui_editor_code_editor_set_show_line_numbers(
		vgui_editor_code_editor* editor, int show)
	{
		if (!editor)
			return;
		editor->impl.set_show_line_numbers(show != 0);
	}

	int vgui_editor_code_editor_show_line_numbers(vgui_editor_code_editor* editor)
	{
		return editor && editor->impl.show_line_numbers() ? 1 : 0;
	}

	void vgui_editor_code_editor_set_tab_size(vgui_editor_code_editor* editor,
		int tab_size)
	{
		if (!editor)
			return;
		editor->impl.set_tab_size(tab_size);
	}

	int vgui_editor_code_editor_tab_size(vgui_editor_code_editor* editor)
	{
		return editor ? editor->impl.tab_size() : 0;
	}

	void vgui_editor_code_editor_set_font_size(vgui_editor_code_editor* editor,
		float size)
	{
		if (!editor)
			return;
		editor->impl.set_font_size(size);
	}

	float vgui_editor_code_editor_font_size(vgui_editor_code_editor* editor)
	{
		return editor ? editor->impl.font_size() : 0.0f;
	}

	void vgui_editor_code_editor_set_token_color(vgui_editor_code_editor* editor,
		vgui_editor_code_token_kind kind, vgui_color color)
	{
		if (!editor)
			return;
		editor->impl.set_token_color(to_token_kind(kind), color);
	}

	vgui_color vgui_editor_code_editor_token_color(vgui_editor_code_editor* editor,
		vgui_editor_code_token_kind kind)
	{
		if (!editor)
			return 0;
		return editor->impl.token_color(to_token_kind(kind));
	}

	void vgui_editor_code_editor_reset_token_colors(
		vgui_editor_code_editor* editor)
	{
		if (!editor)
			return;
		editor->impl.reset_token_colors();
	}

	int vgui_editor_code_editor_set_theme_json(vgui_editor_code_editor* editor,
		const char* json)
	{
		if (!editor || !json)
			return 0;
		code_highlight_theme theme;
		std::string error;
		if (!code_highlight_theme_from_json(vgeditor::to_text(json), theme,
			&error)) {
			vgeditor::set_error(error.empty() ?
				std::string("invalid highlight theme json") : error);
			return 0;
		}
		editor->impl.set_highlight_theme(theme);
		vgeditor::set_error(std::string());
		return 1;
	}

	size_t vgui_editor_code_editor_theme_json(vgui_editor_code_editor* editor,
		char* out, size_t cap)
	{
		if (!editor)
			return 0;
		const std::string text = code_highlight_theme_to_json(
			editor->impl.highlight_theme());
		return vgeditor::copy_string(out, cap, text.c_str());
	}

	int vgui_editor_code_editor_set_theme_preset(vgui_editor_code_editor* editor,
		const char* name)
	{
		if (!editor || !name)
			return 0;
		return editor->impl.set_highlight_preset(vgeditor::to_text(name)) ? 1 : 0;
	}

	size_t vgui_editor_code_editor_theme_preset_count(void)
	{
		return code_highlight_theme_preset_names().size();
	}

	size_t vgui_editor_code_editor_theme_preset_name(size_t index, char* out,
		size_t cap)
	{
		const std::vector<std::string> names = code_highlight_theme_preset_names();
		if (index >= names.size())
			return vgeditor::copy_string(out, cap, "");
		return vgeditor::copy_string(out, cap, names[index].c_str());
	}

	int vgui_editor_code_editor_insert_text(vgui_editor_code_editor* editor,
		const char* text)
	{
		if (!editor)
			return 0;
		return editor->impl.insert_text(vgeditor::to_text(text)) ? 1 : 0;
	}

	int vgui_editor_code_editor_backspace(vgui_editor_code_editor* editor)
	{
		return editor && editor->impl.backspace() ? 1 : 0;
	}

	int vgui_editor_code_editor_delete_forward(vgui_editor_code_editor* editor)
	{
		return editor && editor->impl.delete_forward() ? 1 : 0;
	}

	int vgui_editor_code_editor_newline(vgui_editor_code_editor* editor)
	{
		return editor && editor->impl.newline() ? 1 : 0;
	}

	int vgui_editor_code_editor_indent_selection(vgui_editor_code_editor* editor)
	{
		return editor && editor->impl.indent_selection() ? 1 : 0;
	}

	int vgui_editor_code_editor_outdent_selection(vgui_editor_code_editor* editor)
	{
		return editor && editor->impl.outdent_selection() ? 1 : 0;
	}

	int vgui_editor_code_editor_undo(vgui_editor_code_editor* editor)
	{
		return editor && editor->impl.undo() ? 1 : 0;
	}

	int vgui_editor_code_editor_redo(vgui_editor_code_editor* editor)
	{
		return editor && editor->impl.redo() ? 1 : 0;
	}

	int vgui_editor_code_editor_can_undo(vgui_editor_code_editor* editor)
	{
		return editor && editor->impl.can_undo() ? 1 : 0;
	}

	int vgui_editor_code_editor_can_redo(vgui_editor_code_editor* editor)
	{
		return editor && editor->impl.can_redo() ? 1 : 0;
	}

	void vgui_editor_code_editor_set_command_stack(
		vgui_editor_code_editor* editor, vgui_editor_command_stack* stack)
	{
		if (!editor)
			return;
		editor->impl.set_command_stack(vgeditor::stack_impl(stack));
	}

	void vgui_editor_code_editor_get_caret(vgui_editor_code_editor* editor,
		vgui_editor_code_position* out)
	{
		if (!editor || !out)
			return;
		*out = from_position(editor->impl.caret());
	}

	void vgui_editor_code_editor_set_caret(vgui_editor_code_editor* editor,
		int line, int column)
	{
		if (!editor)
			return;
		editor->impl.set_caret(line, column);
	}

	int vgui_editor_code_editor_caret_line(vgui_editor_code_editor* editor)
	{
		return editor ? editor->impl.caret_line() : 0;
	}

	int vgui_editor_code_editor_caret_column(vgui_editor_code_editor* editor)
	{
		return editor ? editor->impl.caret_column() : 0;
	}

	void vgui_editor_code_editor_selection_anchor(vgui_editor_code_editor* editor,
		vgui_editor_code_position* out)
	{
		if (!editor || !out)
			return;
		*out = from_position(editor->impl.selection_anchor());
	}

	int vgui_editor_code_editor_has_selection(vgui_editor_code_editor* editor)
	{
		return editor && editor->impl.has_selection() ? 1 : 0;
	}

	void vgui_editor_code_editor_set_selection(vgui_editor_code_editor* editor,
		vgui_editor_code_position anchor, vgui_editor_code_position caret)
	{
		if (!editor)
			return;
		editor->impl.set_selection(to_position(anchor), to_position(caret));
	}

	void vgui_editor_code_editor_select_all(vgui_editor_code_editor* editor)
	{
		if (!editor)
			return;
		editor->impl.select_all();
	}

	void vgui_editor_code_editor_clear_selection(vgui_editor_code_editor* editor)
	{
		if (!editor)
			return;
		editor->impl.clear_selection();
	}

	size_t vgui_editor_code_editor_selected_text(vgui_editor_code_editor* editor,
		char* out, size_t cap)
	{
		if (!editor)
			return 0;
		const std::string text = editor->impl.selected_text();
		return vgeditor::copy_string(out, cap, text.c_str());
	}

	void vgui_editor_code_editor_set_on_caret(vgui_editor_code_editor* editor,
		vgui_editor_code_caret_fn fn, void* user)
	{
		if (!editor)
			return;
		editor->on_caret.set(fn, user, nullptr);
		if (!fn) {
			editor->impl.set_caret_listener(nullptr);
			return;
		}
		const vgui_editor_code_caret_fn slot_fn = editor->on_caret.fn;
		void* slot_user = editor->on_caret.user;
		vgeditor::user_owner_ptr owner = editor->on_caret.owner;
		editor->impl.set_caret_listener([slot_fn, slot_user, owner](
			code_position position) {
			slot_fn(slot_user, from_position(position));
		});
	}

	void vgui_editor_code_editor_set_clipboard_handlers(
		vgui_editor_code_editor* editor, vgui_editor_clipboard_get_fn getter,
		vgui_editor_clipboard_set_fn setter, void* user,
		vgui_editor_release_fn release_user)
	{
		if (!editor)
			return;
		/* One owner for both callbacks; an unset handler leaves the kit's hidden
		 * edit widget bridge in place, so it is passed as an empty std::function. */
		editor->clip_get.set(getter, user, release_user);
		editor->clip_set = setter;
		const vgui_editor_clipboard_get_fn get_fn = editor->clip_get.fn;
		const vgui_editor_clipboard_set_fn set_fn = editor->clip_set;
		vgeditor::user_owner_ptr owner = editor->clip_get.owner;
		code_editor::clipboard_getter read;
		code_editor::clipboard_setter write;
		if (get_fn) {
			read = [get_fn, owner]() -> std::string {
				return vgeditor::read_text([get_fn, owner](char* text, size_t cap) {
					return get_fn(owner->user, text, cap);
				}).value;
			};
		}
		if (set_fn) {
			write = [set_fn, owner](const std::string& text) {
				set_fn(owner->user, text.c_str());
			};
		}
		editor->impl.set_clipboard_handlers(std::move(read), std::move(write));
	}

	void vgui_editor_code_editor_clear_clipboard_handlers(
		vgui_editor_code_editor* editor)
	{
		if (!editor)
			return;
		editor->impl.clear_clipboard_handlers();
		editor->clip_get.clear();
		editor->clip_set = nullptr;
	}

	int vgui_editor_code_editor_copy(vgui_editor_code_editor* editor)
	{
		return editor && editor->impl.copy() ? 1 : 0;
	}

	int vgui_editor_code_editor_cut(vgui_editor_code_editor* editor)
	{
		return editor && editor->impl.cut() ? 1 : 0;
	}

	int vgui_editor_code_editor_paste(vgui_editor_code_editor* editor)
	{
		return editor && editor->impl.paste() ? 1 : 0;
	}

	void vgui_editor_code_editor_set_find_query(vgui_editor_code_editor* editor,
		const char* text)
	{
		if (!editor)
			return;
		editor->impl.set_find_query(vgeditor::to_text(text));
	}

	size_t vgui_editor_code_editor_find_query(vgui_editor_code_editor* editor,
		char* out, size_t cap)
	{
		if (!editor)
			return 0;
		return vgeditor::copy_string(out, cap,
			editor->impl.find_query().c_str());
	}

	void vgui_editor_code_editor_set_find_case_sensitive(
		vgui_editor_code_editor* editor, int sensitive)
	{
		if (!editor)
			return;
		editor->impl.set_find_case_sensitive(sensitive != 0);
	}

	int vgui_editor_code_editor_find_case_sensitive(vgui_editor_code_editor* editor)
	{
		return editor && editor->impl.find_case_sensitive() ? 1 : 0;
	}

	void vgui_editor_code_editor_open_find(vgui_editor_code_editor* editor,
		int replace_mode)
	{
		if (!editor)
			return;
		editor->impl.open_find(replace_mode != 0);
	}

	void vgui_editor_code_editor_close_find(vgui_editor_code_editor* editor)
	{
		if (!editor)
			return;
		editor->impl.close_find();
	}

	int vgui_editor_code_editor_find_visible(vgui_editor_code_editor* editor)
	{
		return editor && editor->impl.find_visible() ? 1 : 0;
	}

	int vgui_editor_code_editor_find_next(vgui_editor_code_editor* editor)
	{
		return editor && editor->impl.find_next() ? 1 : 0;
	}

	int vgui_editor_code_editor_find_previous(vgui_editor_code_editor* editor)
	{
		return editor && editor->impl.find_previous() ? 1 : 0;
	}

	size_t vgui_editor_code_editor_find_all(vgui_editor_code_editor* editor,
		const char* needle, int case_sensitive)
	{
		if (!editor)
			return 0;
		/* The kit's own find_all() is a stateless query, but the C contract says the
		 * results also feed match_at().  Publishing the query through the setters
		 * rebuilds the view's match list with the same needle and flag. */
		editor->impl.set_find_case_sensitive(case_sensitive != 0);
		editor->impl.set_find_query(vgeditor::to_text(needle));
		return editor->impl.matches().size();
	}

	size_t vgui_editor_code_editor_match_count(vgui_editor_code_editor* editor)
	{
		return editor ? editor->impl.matches().size() : 0;
	}

	int vgui_editor_code_editor_match_at(vgui_editor_code_editor* editor,
		size_t index, vgui_editor_code_match* out)
	{
		if (!editor || !out)
			return 0;
		const std::vector<code_match>& matches = editor->impl.matches();
		if (index >= matches.size())
			return 0;
		fill_match(matches[index], out);
		return 1;
	}

	int vgui_editor_code_editor_active_match(vgui_editor_code_editor* editor)
	{
		return editor ? editor->impl.active_match() : 0;
	}

	int vgui_editor_code_editor_replace_current(vgui_editor_code_editor* editor,
		const char* replacement)
	{
		if (!editor)
			return 0;
		return editor->impl.replace_current(vgeditor::to_text(replacement)) ?
			1 : 0;
	}

	int vgui_editor_code_editor_replace_all(vgui_editor_code_editor* editor,
		const char* needle, const char* replacement, int case_sensitive)
	{
		if (!editor)
			return 0;
		return editor->impl.replace_all(vgeditor::to_text(needle),
			vgeditor::to_text(replacement), case_sensitive != 0);
	}

	void vgui_editor_code_editor_append_log(vgui_editor_code_editor* editor,
		vgui_editor_log_level level, const char* text, const char* timestamp)
	{
		if (!editor)
			return;
		editor->impl.append_log(to_log_level(level), vgeditor::to_text(text),
			vgeditor::to_text(timestamp));
	}

	void vgui_editor_code_editor_set_level_color(vgui_editor_code_editor* editor,
		vgui_editor_log_level level, vgui_color color)
	{
		if (!editor)
			return;
		editor->impl.set_log_level_color(to_log_level(level), color);
	}

	vgui_color vgui_editor_code_editor_level_color(vgui_editor_code_editor* editor,
		vgui_editor_log_level level)
	{
		if (!editor)
			return 0;
		return editor->impl.log_level_color(to_log_level(level));
	}

	void vgui_editor_code_editor_set_auto_scroll(vgui_editor_code_editor* editor,
		int enabled)
	{
		if (!editor)
			return;
		editor->impl.set_auto_scroll(enabled != 0);
	}

	int vgui_editor_code_editor_auto_scroll(vgui_editor_code_editor* editor)
	{
		return editor && editor->impl.auto_scroll() ? 1 : 0;
	}

	void vgui_editor_code_editor_set_max_lines(vgui_editor_code_editor* editor,
		size_t max_lines)
	{
		if (!editor)
			return;
		editor->impl.set_max_lines(max_lines);
	}

	size_t vgui_editor_code_editor_max_lines(vgui_editor_code_editor* editor)
	{
		return editor ? editor->impl.max_lines() : 0;
	}

	size_t vgui_editor_code_editor_dropped_lines(vgui_editor_code_editor* editor)
	{
		return editor ? editor->impl.dropped_lines() : 0;
	}

	int vgui_editor_code_editor_open_file(vgui_editor_code_editor* editor,
		const char* path)
	{
		if (!editor || !path)
			return 0;
		std::string error;
		if (!editor->impl.open_file(vgeditor::to_text(path), &error)) {
			vgeditor::set_error(error.empty() ?
				std::string("cannot open file") : error);
			return 0;
		}
		vgeditor::set_error(std::string());
		return 1;
	}

	void vgui_editor_code_editor_begin_incremental_load(
		vgui_editor_code_editor* editor, const char* text)
	{
		if (!editor)
			return;
		editor->impl.begin_incremental_load(vgeditor::to_text(text));
	}

	int vgui_editor_code_editor_loading(vgui_editor_code_editor* editor)
	{
		return editor && editor->impl.loading() ? 1 : 0;
	}

	float vgui_editor_code_editor_load_progress(vgui_editor_code_editor* editor)
	{
		return editor ? editor->impl.load_progress() : 0.0f;
	}

	void vgui_editor_code_editor_set_load_chunk_size(
		vgui_editor_code_editor* editor, size_t bytes)
	{
		if (!editor)
			return;
		editor->impl.set_load_chunk_size(bytes);
	}

	size_t vgui_editor_code_editor_load_chunk_size(vgui_editor_code_editor* editor)
	{
		return editor ? editor->impl.load_chunk_size() : 0;
	}

	int vgui_editor_code_editor_process_load_step(vgui_editor_code_editor* editor)
	{
		return editor && editor->impl.process_load_step() ? 1 : 0;
	}

	void vgui_editor_code_editor_cancel_load(vgui_editor_code_editor* editor)
	{
		if (!editor)
			return;
		editor->impl.cancel_load();
	}

	void vgui_editor_code_editor_tick(vgui_editor_code_editor* editor,
		float delta_time)
	{
		if (!editor)
			return;
		editor->impl.tick(delta_time);
	}

	void vgui_editor_code_editor_set_scroll(vgui_editor_code_editor* editor,
		float x, float y)
	{
		if (!editor)
			return;
		editor->impl.set_scroll(x, y);
	}

	float vgui_editor_code_editor_scroll_x(vgui_editor_code_editor* editor)
	{
		return editor ? editor->impl.scroll_x() : 0.0f;
	}

	float vgui_editor_code_editor_scroll_y(vgui_editor_code_editor* editor)
	{
		return editor ? editor->impl.scroll_y() : 0.0f;
	}

	void vgui_editor_code_editor_scroll_to_caret(vgui_editor_code_editor* editor)
	{
		if (!editor)
			return;
		editor->impl.scroll_to_caret();
	}

	size_t vgui_editor_code_editor_capture_state_json(
		vgui_editor_code_editor* editor, char* out, size_t cap)
	{
		if (!editor)
			return 0;
		const std::string text = code_editor_state_to_json(
			editor->impl.capture_state());
		return vgeditor::copy_string(out, cap, text.c_str());
	}

	int vgui_editor_code_editor_apply_state_json(vgui_editor_code_editor* editor,
		const char* json)
	{
		if (!editor || !json)
			return 0;
		code_editor_state state;
		std::string error;
		if (!code_editor_state_from_json(vgeditor::to_text(json), state, &error)) {
			vgeditor::set_error(error.empty() ?
				std::string("invalid code editor state json") : error);
			return 0;
		}
		if (!editor->impl.apply_state(state, &error)) {
			vgeditor::set_error(error.empty() ?
				std::string("code editor state rejected") : error);
			return 0;
		}
		vgeditor::set_error(std::string());
		return 1;
	}

	void vgui_editor_code_editor_set_on_change(vgui_editor_code_editor* editor,
		vgui_editor_code_change_fn fn, void* user)
	{
		if (!editor)
			return;
		editor->on_change.set(fn, user, nullptr);
		if (!fn) {
			editor->impl.set_change_listener(nullptr);
			return;
		}
		const vgui_editor_code_change_fn slot_fn = editor->on_change.fn;
		void* slot_user = editor->on_change.user;
		vgeditor::user_owner_ptr owner = editor->on_change.owner;
		editor->impl.set_change_listener([slot_fn, slot_user, owner]() {
			slot_fn(slot_user);
		});
	}

	/* ---------------------------------------------------------------------- */
	/* Markdown preview                                                       */
	/* ---------------------------------------------------------------------- */

	void vgui_editor_md_media_default(vgui_editor_md_media* media)
	{
		if (!media)
			return;
		std::memset(media, 0, sizeof(*media));
		media->struct_size = (uint32_t)sizeof(*media);
		media->status = VGUI_EDITOR_MD_PENDING;
		media->texture = nullptr;
		media->pixel_width = 0;
		media->pixel_height = 0;
		media->aspect = 0.0f;
		media->error = nullptr;
	}

	vgui_editor_markdown* vgui_editor_markdown_create(vgui_widget* parent)
	{
		if (!parent)
			return nullptr;
		std::unique_ptr<vgui_editor_markdown> view(
			vgeditor::own_object<vgui_editor_markdown>(
				new (std::nothrow) markdown_view()));
		if (!view)
			return nullptr;
		view->impl.build(parent);
		if (!view->impl.widget())
			return nullptr;
		return view.release();
	}

	void vgui_editor_markdown_destroy(vgui_editor_markdown* view)
	{
		if (!view || !view->owns_object())
			return;
		/* Dropping the resolver first keeps the view from calling into a host
		 * whose handle is already gone. */
		view->impl.set_media_resolver(nullptr);
		view->impl.set_clipboard_setter(nullptr);
		view->resolve.clear();
		view->clip.clear();
		vgui_widget* widget = view->impl.widget();
		delete view;
		if (widget)
			vgui_widget_destroy(widget);
	}

	vgui_widget* vgui_editor_markdown_widget(vgui_editor_markdown* view)
	{
		return view ? view->impl.widget() : nullptr;
	}

	int vgui_editor_markdown_built(vgui_editor_markdown* view)
	{
		return view && view->impl.built() ? 1 : 0;
	}

	void vgui_editor_markdown_set_text(vgui_editor_markdown* view, const char* text)
	{
		if (!view)
			return;
		view->impl.set_markdown(vgeditor::to_text(text));
	}

	size_t vgui_editor_markdown_text(vgui_editor_markdown* view, char* out,
		size_t cap)
	{
		if (!view)
			return 0;
		return vgeditor::copy_string(out, cap, view->impl.markdown().c_str());
	}

	int vgui_editor_markdown_truncated(vgui_editor_markdown* view)
	{
		return view && view->impl.truncated() ? 1 : 0;
	}

	size_t vgui_editor_markdown_rendered_text(vgui_editor_markdown* view,
		char* out, size_t cap)
	{
		if (!view)
			return 0;
		const std::string text = view->impl.rendered_text();
		return vgeditor::copy_string(out, cap, text.c_str());
	}

	void vgui_editor_markdown_set_font_size(vgui_editor_markdown* view, float size)
	{
		if (!view)
			return;
		view->impl.set_font_size(size);
	}

	float vgui_editor_markdown_font_size(vgui_editor_markdown* view)
	{
		return view ? view->impl.font_size() : 0.0f;
	}

	int vgui_editor_markdown_set_theme_json(vgui_editor_markdown* view,
		const char* json)
	{
		if (!view || !json)
			return 0;
		code_highlight_theme theme;
		std::string error;
		if (!code_highlight_theme_from_json(vgeditor::to_text(json), theme,
			&error)) {
			vgeditor::set_error(error.empty() ?
				std::string("invalid highlight theme json") : error);
			return 0;
		}
		view->impl.set_highlight_theme(theme);
		vgeditor::set_error(std::string());
		return 1;
	}

	size_t vgui_editor_markdown_theme_json(vgui_editor_markdown* view, char* out,
		size_t cap)
	{
		if (!view)
			return 0;
		const std::string text = code_highlight_theme_to_json(
			view->impl.highlight_theme());
		return vgeditor::copy_string(out, cap, text.c_str());
	}

	int vgui_editor_markdown_set_theme_preset(vgui_editor_markdown* view,
		const char* name)
	{
		if (!view || !name)
			return 0;
		return view->impl.set_highlight_preset(vgeditor::to_text(name)) ? 1 : 0;
	}

	float vgui_editor_markdown_content_height(vgui_editor_markdown* view)
	{
		return view ? view->impl.content_height() : 0.0f;
	}

	void vgui_editor_markdown_set_scroll(vgui_editor_markdown* view, float y)
	{
		if (!view)
			return;
		view->impl.set_scroll(y);
	}

	float vgui_editor_markdown_scroll_y(vgui_editor_markdown* view)
	{
		return view ? view->impl.scroll_y() : 0.0f;
	}

	void vgui_editor_markdown_scroll_to_block(vgui_editor_markdown* view, int block,
		int center)
	{
		if (!view)
			return;
		view->impl.scroll_to_block(block, center != 0);
	}

	void vgui_editor_markdown_scroll_to_source_line(vgui_editor_markdown* view,
		int line, int center)
	{
		if (!view)
			return;
		view->impl.scroll_to_source_line(line, center != 0);
	}

	int vgui_editor_markdown_block_count(vgui_editor_markdown* view)
	{
		return view ? view->impl.block_count() : 0;
	}

	int vgui_editor_markdown_block_source_line(vgui_editor_markdown* view,
		int block)
	{
		return view ? view->impl.block_source_line(block) : 0;
	}

	int vgui_editor_markdown_block_source_end_line(vgui_editor_markdown* view,
		int block)
	{
		return view ? view->impl.block_source_end_line(block) : 0;
	}

	int vgui_editor_markdown_block_for_source_line(vgui_editor_markdown* view,
		int line)
	{
		if (!view)
			return -1;
		return view->impl.block_for_source_line(line);
	}

	int vgui_editor_markdown_source_line_at(vgui_editor_markdown* view, float x,
		float y)
	{
		if (!view)
			return -1;
		return view->impl.source_line_at(x, y);
	}

	void vgui_editor_markdown_set_active_source_line(vgui_editor_markdown* view,
		int line)
	{
		if (!view)
			return;
		view->impl.set_active_source_line(line);
	}

	int vgui_editor_markdown_active_source_line(vgui_editor_markdown* view)
	{
		if (!view)
			return -1;
		return view->impl.active_source_line();
	}

	int vgui_editor_markdown_has_selection(vgui_editor_markdown* view)
	{
		return view && view->impl.has_selection() ? 1 : 0;
	}

	void vgui_editor_markdown_select_all(vgui_editor_markdown* view)
	{
		if (!view)
			return;
		view->impl.select_all();
	}

	void vgui_editor_markdown_clear_selection(vgui_editor_markdown* view)
	{
		if (!view)
			return;
		view->impl.clear_selection();
	}

	size_t vgui_editor_markdown_selected_text(vgui_editor_markdown* view,
		char* out, size_t cap)
	{
		if (!view)
			return 0;
		const std::string text = view->impl.selected_text();
		return vgeditor::copy_string(out, cap, text.c_str());
	}

	int vgui_editor_markdown_copy(vgui_editor_markdown* view)
	{
		return view && view->impl.copy() ? 1 : 0;
	}

	void vgui_editor_markdown_set_clipboard_handler(vgui_editor_markdown* view,
		vgui_editor_md_clipboard_fn fn, void* user,
		vgui_editor_release_fn release_user)
	{
		if (!view)
			return;
		view->clip.set(fn, user, release_user);
		if (!fn) {
			/* An empty setter restores the view's system-clipboard bridge. */
			view->impl.set_clipboard_setter(nullptr);
			return;
		}
		const vgui_editor_md_clipboard_fn slot_fn = view->clip.fn;
		void* slot_user = view->clip.user;
		vgeditor::user_owner_ptr owner = view->clip.owner;
		view->impl.set_clipboard_setter([slot_fn, slot_user, owner](
			const std::string& text) {
			slot_fn(slot_user, text.c_str());
		});
	}

	void vgui_editor_markdown_set_media_resolver(vgui_editor_markdown* view,
		vgui_editor_md_resolve_fn fn, void* user,
		vgui_editor_release_fn release_user)
	{
		if (!view)
			return;
		view->resolve.set(fn, user, release_user);
		if (!fn) {
			view->impl.set_media_resolver(nullptr);
			return;
		}
		const vgui_editor_md_resolve_fn slot_fn = view->resolve.fn;
		void* slot_user = view->resolve.user;
		vgeditor::user_owner_ptr owner = view->resolve.owner;
		view->impl.set_media_resolver([slot_fn, slot_user, owner](
			const std::string& source, md_media_kind kind) {
			/* The host may leave the struct untouched, so it starts at the
			 * documented default; `error` is borrowed from the kit string below. */
			vgui_editor_md_media media;
			vgui_editor_md_media_default(&media);
			slot_fn(slot_user, source.c_str(), from_media_kind(kind), &media);
			return to_media(&media);
		});
	}

	void vgui_editor_markdown_invalidate_media(vgui_editor_markdown* view,
		const char* source)
	{
		if (!view)
			return;
		/* NULL or empty re-resolves every media item. */
		view->impl.invalidate_media(vgeditor::to_text(source));
	}

	void vgui_editor_markdown_set_max_media_height(vgui_editor_markdown* view,
		float logical_height)
	{
		if (!view)
			return;
		view->impl.set_max_media_height(logical_height);
	}

	float vgui_editor_markdown_max_media_height(vgui_editor_markdown* view)
	{
		return view ? view->impl.max_media_height() : 0.0f;
	}

} /* extern "C" */

namespace vgeditor {

	vgui_editor_code_editor* borrow_code_editor(vek::code_editor& editor)
	{
		return borrow_object<vgui_editor_code_editor>(editor);
	}

	void release_borrowed_code_editor(vgui_editor_code_editor* handle)
	{
		/* Wrapper only: a borrowed handle's C++ object belongs to its container. */
		if (handle && !handle->owns_object())
			delete handle;
	}


} /* namespace vgeditor */
