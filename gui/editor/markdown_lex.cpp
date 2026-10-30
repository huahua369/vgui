#include "markdown_lex.h"

#include "code_lex.h"

#include <cctype>
#include <cstring>

namespace vgui_editor_kit {
namespace md_lex {

	namespace {

		bool is_space(char c)
		{
			return c == ' ' || c == '\t' || c == '\v' || c == '\f';
		}

		bool is_ident_continue(unsigned char c)
		{
			return std::isalnum(c) || c == '_' || c >= 0x80;
		}

		std::string ascii_lower(std::string text)
		{
			for (char& c : text)
				c = (char)std::tolower((unsigned char)c);
			return text;
		}

		void append_span(std::vector<code_span>& spans, int start, int end,
			code_token_kind kind)
		{
			if (end <= start)
				return;
			code_span span;
			span.start = start;
			span.length = end - start;
			span.kind = kind;
			spans.push_back(span);
		}

	} // namespace

	bool is_fence_state(int state)
	{
		return state == fence_plain || state >= fence_base;
	}

	int fence_state(code_language language, int inner)
	{
		return fence_base + static_cast<int>(language) * fence_stride +
			inner % fence_stride;
	}

	code_language fence_language(int state)
	{
		if (state < fence_base)
			return code_language::plain;
		return static_cast<code_language>((state - fence_base) / fence_stride);
	}

	int fence_inner_state(int state)
	{
		if (state < fence_base)
			return 0;
		return (state - fence_base) % fence_stride;
	}

	bool blank(const std::string& text)
	{
		for (char c : text)
		{
			if (!is_space(c))
				return false;
		}
		return true;
	}

	int first_nonspace(const std::string& text)
	{
		int pos = 0;
		while (pos < (int)text.size() && is_space(text[(std::size_t)pos]))
			++pos;
		return pos;
	}

	bool starts_with(const std::string& text, int at, const char* value)
	{
		const std::size_t n = std::strlen(value);
		return at >= 0 && (std::size_t)at + n <= text.size() &&
			text.compare((std::size_t)at, n, value) == 0;
	}

	int run(const std::string& text, int at, char c, int end)
	{
		int count = 0;
		while (at + count < end && text[(std::size_t)(at + count)] == c)
			++count;
		return count;
	}

	int find_backtick_close(const std::string& text, int from, int count,
		int end)
	{
		int pos = from;
		while (pos < end)
		{
			if (text[(std::size_t)pos] != '`')
			{
				++pos;
				continue;
			}
			const int len = run(text, pos, '`', end);
			if (len == count)
				return pos;
			pos += len;
		}
		return -1;
	}

	int find_close_bracket(const std::string& text, int open, int end)
	{
		int depth = 0;
		for (int i = open; i < end; ++i)
		{
			const char c = text[(std::size_t)i];
			if (c == '\\')
			{
				++i;
				continue;
			}
			if (c == '[')
				++depth;
			else if (c == ']')
			{
				--depth;
				if (depth == 0)
					return i;
			}
		}
		return -1;
	}

	int find_close_paren(const std::string& text, int open, int end)
	{
		int depth = 0;
		for (int i = open; i < end; ++i)
		{
			const char c = text[(std::size_t)i];
			if (c == '\\')
			{
				++i;
				continue;
			}
			if (c == '(')
				++depth;
			else if (c == ')')
			{
				--depth;
				if (depth == 0)
					return i;
			}
		}
		return -1;
	}

	bool thematic_break(const std::string& text, int begin, char* marker)
	{
		char kind = 0;
		int count = 0;
		for (int i = begin; i < (int)text.size(); ++i)
		{
			const char c = text[(std::size_t)i];
			if (is_space(c))
				continue;
			if (c != '-' && c != '*' && c != '_')
				return false;
			if (kind == 0)
				kind = c;
			else if (kind != c)
				return false;
			++count;
		}
		if (count < 3)
			return false;
		if (marker)
			*marker = kind;
		return true;
	}

	bool setext_underline(const std::string& text, int begin, char* marker)
	{
		char kind = 0;
		int count = 0;
		for (int i = begin; i < (int)text.size(); ++i)
		{
			const char c = text[(std::size_t)i];
			if (is_space(c))
				continue;
			if (c != '=' && c != '-')
				return false;
			if (kind == 0)
				kind = c;
			else if (kind != c)
				return false;
			++count;
		}
		if (count < 2)
			return false;
		if (marker)
			*marker = kind;
		return true;
	}

	bool table_separator(const std::string& text, int begin)
	{
		bool dash = false;
		bool pipe = false;
		for (int i = begin; i < (int)text.size(); ++i)
		{
			const char c = text[(std::size_t)i];
			if (c == '-')
				dash = true;
			else if (c == '|')
				pipe = true;
			else if (c != ':' && c != ' ' && c != '\t')
				return false;
		}
		return dash && pipe;
	}

	bool paragraph(const std::string& text)
	{
		if (blank(text))
			return false;
		const int begin = first_nonspace(text);
		const char first = text[(std::size_t)begin];
		if (first == '#' || first == '>' || first == '|')
			return false;
		if (table_separator(text, begin))
			return false;
		if (starts_with(text, begin, "```") ||
			starts_with(text, begin, "~~~"))
			return false;
		if ((first == '-' || first == '*' || first == '+') &&
			begin + 1 < (int)text.size() &&
			is_space(text[(std::size_t)begin + 1]))
			return false;
		if (std::isdigit((unsigned char)first))
		{
			int i = begin;
			while (i < (int)text.size() &&
				std::isdigit((unsigned char)text[(std::size_t)i]))
				++i;
			if (i < (int)text.size() &&
				(text[(std::size_t)i] == '.' ||
					text[(std::size_t)i] == ')') &&
				i + 1 < (int)text.size() &&
				is_space(text[(std::size_t)i + 1]))
				return false;
		}
		if (thematic_break(text, begin, nullptr))
			return false;
		return true;
	}

	code_language fence_language_from_info(std::string info)
	{
		if (info.size() >= 2 && info.front() == '{' && info.back() == '}')
			info = info.substr(1, info.size() - 2);
		const std::size_t start = info.find_first_not_of(".#");
		if (start == std::string::npos)
			return code_language::plain;
		info = info.substr(start);
		while (!info.empty() && (info.back() == '}' || info.back() == ',' ||
			info.back() == ';'))
			info.pop_back();

		const std::string lower = ascii_lower(info);
		if (lower == "c" || lower == "h" || lower == "hpp" ||
			lower == "cc" || lower == "cxx")
			return code_language::cpp;
		if (lower == "jsonc")
			return code_language::json;
		if (lower == "frag" || lower == "vert" || lower == "geom" ||
			lower == "shader")
			return code_language::glsl;
		if (lower == "toml" || lower == "cfg" || lower == "conf")
			return code_language::ini;

		code_language language = code_language::plain;
		if (!code_language_from_name(lower, &language))
			return code_language::plain;
		if (language == code_language::markdown)
			return code_language::plain;
		return language;
	}

	void scan_inline(const std::string& text, int begin, int end,
		code_token_kind base, std::vector<code_span>& spans)
	{
		if (begin >= end)
			return;
		int pos = begin;
		int plain_start = begin;
		auto flush = [&](int upto)
			{
				if (base != code_token_kind::text)
					append_span(spans, plain_start, upto, base);
			};

		while (pos < end)
		{
			const char c = text[(std::size_t)pos];
			if (c == '\\' && pos + 1 < end)
			{
				pos += 2;
				continue;
			}
			if (c == '`')
			{
				int len = run(text, pos, '`', end);
				if (len <= 0)
				{
					++pos;
					continue;
				}
				const int close = find_backtick_close(text, pos + len, len,
					end);
				if (close > pos + len)
				{
					flush(pos);
					append_span(spans, pos, close + len,
						code_token_kind::inline_code);
					pos = close + len;
					plain_start = pos;
					continue;
				}
				pos += len;
				continue;
			}
			if (c == '~' && pos + 1 < end &&
				text[(std::size_t)pos + 1] == '~')
			{
				int close = -1;
				for (int i = pos + 2; i + 1 < end; ++i)
				{
					if (text[(std::size_t)i] == '~' &&
						text[(std::size_t)i + 1] == '~')
					{
						close = i;
						break;
					}
				}
				if (close > pos + 2)
				{
					flush(pos);
					append_span(spans, pos, close + 2,
						code_token_kind::strike);
					pos = close + 2;
					plain_start = pos;
					continue;
				}
				++pos;
				continue;
			}
			if (c == '*' || c == '_')
			{
				int len = run(text, pos, c, end);
				const int marker = len >= 3 ? 3 : (len >= 2 ? 2 : 1);
				const bool word_left = c == '_' && pos > begin &&
					is_ident_continue((unsigned char)
						text[(std::size_t)pos - 1]);
				int close = -1;
				if (!word_left)
				{
					int i = pos + marker;
					while (i < end)
					{
						if (text[(std::size_t)i] != c)
						{
							++i;
							continue;
						}
						int close_run = run(text, i, c, end);
						if (close_run >= marker)
						{
							const bool right_ok = c != '_' ||
								i + marker >= end ||
								!is_ident_continue((unsigned char)
									text[(std::size_t)(i + marker)]);
							const bool left_ok = !is_space(
								text[(std::size_t)(pos + marker)]);
							const bool inner_ok = i > pos + marker &&
								!is_space(text[(std::size_t)(i - 1)]);
							if (right_ok && left_ok && inner_ok)
							{
								close = i;
								break;
							}
						}
						i += close_run;
					}
				}
				if (close > pos + marker)
				{
					flush(pos);
					append_span(spans, pos, close + marker,
						marker >= 2 ? code_token_kind::strong :
						code_token_kind::emphasis);
					pos = close + marker;
					plain_start = pos;
					continue;
				}
				pos += marker;
				continue;
			}
			if (c == '[' || (c == '!' && pos + 1 < end &&
				text[(std::size_t)pos + 1] == '['))
			{
				const int open = c == '!' ? pos + 1 : pos;
				const int close = find_close_bracket(text, open, end);
				if (close > open)
				{
					int url_end = -1;
					if (close + 1 < end &&
						text[(std::size_t)close + 1] == '(')
						url_end = find_close_paren(text, close + 1, end);
					else if (close + 1 < end &&
						text[(std::size_t)close + 1] == '[')
					{
						url_end = find_close_bracket(text, close + 1, end);
					}
					if (url_end > close + 1)
					{
						flush(pos);
						append_span(spans, pos, close + 1,
							code_token_kind::link);
						append_span(spans, close + 1, url_end + 1,
							code_token_kind::link_url);
						pos = url_end + 1;
						plain_start = pos;
						continue;
					}
				}
				++pos;
				continue;
			}
			if (c == '<')
			{
				int close = -1;
				for (int i = pos + 1; i < end; ++i)
				{
					if (text[(std::size_t)i] == '>')
					{
						close = i;
						break;
					}
				}
				if (close > pos + 1)
				{
					if (starts_with(text, pos, "<!--"))
					{
						int comment_end = -1;
						for (int i = pos + 4; i + 2 < end; ++i)
						{
							if (text.compare((std::size_t)i, 3, "-->") == 0)
							{
								comment_end = i + 3;
								break;
							}
						}
						if (comment_end > 0)
						{
							flush(pos);
							append_span(spans, pos, comment_end,
								code_token_kind::comment);
							pos = comment_end;
							plain_start = pos;
							continue;
						}
					}
					else
					{
						const std::size_t scheme =
							text.find("://", (std::size_t)pos + 1);
						const bool url = (scheme != std::string::npos &&
							(int)scheme < close) ||
							starts_with(text, pos + 1, "mailto:");
						const char next = text[(std::size_t)pos + 1];
						const bool tag = std::isalpha((unsigned char)next) ||
							next == '/' || next == '!' || next == '?';
						if (url || tag)
						{
							flush(pos);
							append_span(spans, pos, close + 1,
								url ? code_token_kind::link_url :
								code_token_kind::tag);
							pos = close + 1;
							plain_start = pos;
							continue;
						}
					}
				}
				++pos;
				continue;
			}
			++pos;
		}
		flush(end);
	}

	void tokenize_line(const std::string& text, const std::string* prev_line,
		const std::string* next_line, int lex_in, std::vector<code_span>& spans,
		int* lex_out)
	{
		spans.clear();
		const int length = (int)text.size();

		if (is_fence_state(lex_in))
		{
			const int first = first_nonspace(text);
			const bool closing = starts_with(text, first, "```") ||
				starts_with(text, first, "~~~");
			if (closing)
			{
				append_span(spans, 0, length, code_token_kind::comment);
				*lex_out = 0;
				return;
			}
			const code_language language = fence_language(lex_in);
			if (language == code_language::plain)
			{
				append_span(spans, first, length,
					code_token_kind::string_literal);
				*lex_out = lex_in;
				return;
			}
			int inner = 0;
			tokenize_code_line(language, text, fence_inner_state(lex_in),
				spans, &inner);
			*lex_out = fence_state(language, inner);
			return;
		}

		*lex_out = 0;
		if (blank(text))
			return;

		int indent = 0;
		int begin = 0;
		while (begin < length && is_space(text[(std::size_t)begin]))
		{
			indent += text[(std::size_t)begin] == '\t' ? 4 : 1;
			++begin;
		}
		/* Four-space indents are indented code or nested list content; leave
		 * them to the inline scanner to avoid mis-colouring list bodies. */
		if (indent >= 4)
		{
			scan_inline(text, begin, length, code_token_kind::text, spans);
			return;
		}

		/* Fenced code block. */
		if (starts_with(text, begin, "```") ||
			starts_with(text, begin, "~~~"))
		{
			const char fence_char = text[(std::size_t)begin];
			const int fence_len = run(text, begin, fence_char, length);
			if (fence_len >= 3)
			{
				const int info_from = first_nonspace(
					text.substr((std::size_t)(begin + fence_len))) +
					begin + fence_len;
				int info_to = info_from;
				while (info_to < length &&
					!is_space(text[(std::size_t)info_to]))
					++info_to;
				const code_language language = fence_language_from_info(
					text.substr((std::size_t)info_from,
						(std::size_t)(info_to - info_from)));
				append_span(spans, begin, begin + fence_len,
					code_token_kind::comment);
				append_span(spans, info_from, info_to,
					code_token_kind::attribute);
				append_span(spans, info_to, length,
					code_token_kind::comment);
				*lex_out = language == code_language::plain ?
					fence_plain : fence_state(language, 0);
				return;
			}
		}

		char underline = 0;
		if (setext_underline(text, begin, &underline))
		{
			const bool prev_paragraph = prev_line &&
				paragraph(*prev_line) &&
				!setext_underline(*prev_line, 0, nullptr);
			if (prev_paragraph)
			{
				append_span(spans, begin, length,
					code_token_kind::heading_marker);
				return;
			}
			if (underline == '-' && thematic_break(text, begin, nullptr))
			{
				append_span(spans, begin, length,
					code_token_kind::horizontal_rule);
				return;
			}
			if (underline == '=')
				append_span(spans, begin, length,
					code_token_kind::heading_marker);
			return;
		}

		/* ATX heading. */
		if (text[(std::size_t)begin] == '#')
		{
			const int hashes = run(text, begin, '#', length);
			if (hashes >= 1 && hashes <= 6 &&
				(begin + hashes >= length ||
					is_space(text[(std::size_t)(begin + hashes)])))
			{
				const int content_start = begin + hashes;
				int content_end = length;
				int tail_marker_start = -1;
				int tail = length;
				while (tail > content_start &&
					is_space(text[(std::size_t)tail - 1]))
					--tail;
				int tail_hashes = 0;
				while (tail - tail_hashes - 1 >= content_start &&
					text[(std::size_t)(tail - tail_hashes - 1)] == '#')
					++tail_hashes;
				if (tail_hashes > 0)
				{
					const int before = tail - tail_hashes - 1;
					if (before >= content_start &&
						is_space(text[(std::size_t)before]))
					{
						content_end = before;
						tail_marker_start = tail - tail_hashes;
					}
				}
				append_span(spans, begin, content_start,
					code_token_kind::heading_marker);
				scan_inline(text, content_start, content_end,
					code_token_kind::heading, spans);
				if (tail_marker_start >= 0)
					append_span(spans, tail_marker_start, tail,
						code_token_kind::heading_marker);
				return;
			}
		}

		/* Setext heading: a paragraph line whose next line underlines it. */
		char next_underline = 0;
		if (next_line && paragraph(text))
		{
			const int next_begin = first_nonspace(*next_line);
			if (setext_underline(*next_line, next_begin, &next_underline))
			{
				scan_inline(text, begin, length,
					code_token_kind::heading, spans);
				return;
			}
		}

		if (thematic_break(text, begin, nullptr))
		{
			append_span(spans, begin, length,
				code_token_kind::horizontal_rule);
			return;
		}

		if (table_separator(text, begin))
		{
			append_span(spans, begin, length, code_token_kind::table);
			return;
		}

		/* Blockquote: colour the markers, then the inline content. */
		if (text[(std::size_t)begin] == '>')
		{
			int pos = begin;
			while (pos < length && text[(std::size_t)pos] == '>')
			{
				append_span(spans, pos, pos + 1, code_token_kind::quote);
				++pos;
				if (pos < length && is_space(text[(std::size_t)pos]))
					++pos;
			}
			scan_inline(text, pos, length, code_token_kind::text, spans);
			return;
		}

		/* List item marker and optional task checkbox. */
		int marker_end = -1;
		const char first = text[(std::size_t)begin];
		if ((first == '-' || first == '*' || first == '+') &&
			begin + 1 < length && is_space(text[(std::size_t)begin + 1]))
		{
			marker_end = begin + 1;
		}
		else if (std::isdigit((unsigned char)first))
		{
			int i = begin;
			while (i < length &&
				std::isdigit((unsigned char)text[(std::size_t)i]))
				++i;
			if (i < length && (text[(std::size_t)i] == '.' ||
				text[(std::size_t)i] == ')') && i + 1 < length &&
				is_space(text[(std::size_t)i + 1]))
				marker_end = i + 1;
		}
		if (marker_end > begin)
		{
			append_span(spans, begin, marker_end,
				code_token_kind::list_marker);
			int content = marker_end;
			while (content < length && is_space(text[(std::size_t)content]))
				++content;
			if (content + 2 < length &&
				text[(std::size_t)content] == '[' &&
				(text[(std::size_t)content + 1] == ' ' ||
					text[(std::size_t)content + 1] == 'x' ||
					text[(std::size_t)content + 1] == 'X') &&
				text[(std::size_t)content + 2] == ']')
			{
				append_span(spans, content, content + 3,
					code_token_kind::attribute);
				content += 3;
			}
			scan_inline(text, content, length, code_token_kind::text, spans);
			return;
		}

		/* Table row: split cells at pipes and inline-scan the content. */
		int pipes = 0;
		for (int i = begin; i < length; ++i)
		{
			if (text[(std::size_t)i] == '\\')
				++i;
			else if (text[(std::size_t)i] == '|')
				++pipes;
		}
		if (pipes > 0)
		{
			bool table_row = pipes >= 2;
			if (!table_row && next_line)
			{
				const int next_begin = first_nonspace(*next_line);
				table_row = table_separator(*next_line, next_begin);
			}
			if (!table_row && prev_line)
			{
				const int prev_begin = first_nonspace(*prev_line);
				table_row = table_separator(*prev_line, prev_begin);
			}
			if (table_row)
			{
				int cell_start = begin;
				for (int i = begin; i < length; ++i)
				{
					if (text[(std::size_t)i] == '\\')
					{
						++i;
						continue;
					}
					if (text[(std::size_t)i] != '|')
						continue;
					scan_inline(text, cell_start, i,
						code_token_kind::text, spans);
					append_span(spans, i, i + 1, code_token_kind::table);
					cell_start = i + 1;
				}
				scan_inline(text, cell_start, length,
					code_token_kind::text, spans);
				return;
			}
		}

		scan_inline(text, begin, length, code_token_kind::text, spans);
	}

} // namespace md_lex
} // namespace vgui_editor_kit
