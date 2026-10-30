#pragma once
/*
 * Markdown scanner shared by code_editor's syntax highlighting and the
 * read-only markdown_view.  Both surfaces consume the same predicates and the
 * same inline/link scanner so their rendering rules cannot drift apart.
 *
 * All offsets are byte offsets into UTF-8 text, matching code_span.
 */
#include <vgui/editor/code_editor.h>

#include <string>
#include <vector>

namespace vgui_editor_kit {
namespace md_lex {

	/* Cross-line state of a fenced code block.  A fence without an embedded
	 * language uses fence_plain; otherwise the state packs the language id and
	 * that language's own state:
	 * fence_base + language * fence_stride + state. */
	constexpr int fence_plain = 6;
	constexpr int fence_base = 64;
	constexpr int fence_stride = 16;

	bool is_fence_state(int state);
	int fence_state(code_language language, int inner);
	code_language fence_language(int state);
	int fence_inner_state(int state);

	bool blank(const std::string& text);
	int first_nonspace(const std::string& text);
	bool starts_with(const std::string& text, int at, const char* value);
	int run(const std::string& text, int at, char c, int end);
	int find_backtick_close(const std::string& text, int from, int count,
		int end);
	int find_close_bracket(const std::string& text, int open, int end);
	int find_close_paren(const std::string& text, int open, int end);

	bool thematic_break(const std::string& text, int begin, char* marker);
	bool setext_underline(const std::string& text, int begin, char* marker);
	bool table_separator(const std::string& text, int begin);
	/* True when the line can be the paragraph above a setext underline. */
	bool paragraph(const std::string& text);
	/* Maps a fence info string ("cpp", "{.py}", "frag") to a language. */
	code_language fence_language_from_info(std::string info);

	/* Scans inline Markdown between begin and end and appends token spans.
	 * base colours plain text (heading, heading text fallback, ...). */
	void scan_inline(const std::string& text, int begin, int end,
		code_token_kind base, std::vector<code_span>& spans);

	void tokenize_line(const std::string& text, const std::string* prev_line,
		const std::string* next_line, int lex_in, std::vector<code_span>& spans,
		int* lex_out);

} // namespace md_lex
} // namespace vgui_editor_kit
