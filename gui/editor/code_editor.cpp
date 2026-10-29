#include <vgui/editor/code_editor.h>

#include "tools/tools.hpp"
#include "ui_scale.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <unordered_set>
#include <utility>

#include <nlohmann/json.hpp>

namespace vgui_editor_kit {

	namespace {

		constexpr vgui_color kFallbackText = 0xFFD8DEE9;
		constexpr vgui_color kFallbackMuted = 0xFF8A93A3;
		constexpr vgui_color kFallbackBg = 0xFF1E2229;
		constexpr vgui_color kFallbackGutter = 0xFF232833;
		constexpr vgui_color kFallbackBorder = 0xFF3A4150;

		/* OVG uses 0xAABBGGRR while vgui uses 0xAARRGGBB. */
		uint32_t to_ovg(vgui_color color)
		{
			return (color & 0xFF00FF00u) |
				((color & 0x000000FFu) << 16) |
				((color >> 16) & 0x000000FFu);
		}

		vgui_color with_alpha(vgui_color color, uint32_t alpha)
		{
			return (color & 0x00FFFFFFu) | ((alpha & 0xFFu) << 24);
		}

		vgui_color theme_color(vgui_color color, vgui_color fallback)
		{
			return color ? color : fallback;
		}

		vgui_theme theme_for(vgui_widget* widget)
		{
			vgui_theme theme = {};
			vgui_context* ctx = widget ? vgui_widget_get_context(widget) : nullptr;
			if (ctx)
				vgui_context_get_theme(ctx, &theme);
			theme.text = theme_color(theme.text, kFallbackText);
			theme.panel_bg = theme_color(theme.panel_bg, kFallbackBg);
			theme.panel_border = theme_color(theme.panel_border, kFallbackBorder);
			theme.edit_bg = theme_color(theme.edit_bg, kFallbackBg);
			theme.edit_selection = theme_color(theme.edit_selection,
				with_alpha(theme.accent ? theme.accent : 0xFF61AFEF, 0x55));
			theme.scrollbar = theme_color(theme.scrollbar, 0xFF4B5263);
			theme.scrollbar_hover = theme_color(theme.scrollbar_hover,
				theme.scrollbar);
			theme.disabled = theme_color(theme.disabled, kFallbackMuted);
			return theme;
		}

		bool valid_token_kind(int kind)
		{
			return kind >= 0 && kind < code_token_kind_count;
		}

		bool valid_log_level(int level)
		{
			return level >= 0 && level < code_log_level_count;
		}

		constexpr vgui_color kDefaultTokenColors[code_token_kind_count] = {
			0xFFD8DEE9,	/* text */
			0xFFC678DD,	/* keyword */
			0xFFE5C07B,	/* type */
			0xFF98C379,	/* string_literal */
			0xFFD19A66,	/* number */
			0xFF7F848E,	/* comment */
			0xFF56B6C2,	/* preprocessor */
			0xFF61AFEF,	/* function */
			0xFF56B6C2,	/* operator_token */
			0xFFABB2BF,	/* punctuation */
			0xFFE06C75,	/* attribute */
			0xFFE06C75	/* tag */
		};

		constexpr vgui_color kDefaultLogColors[code_log_level_count] = {
			0xFF7F8A99,	/* trace */
			0xFFA9B6C8,	/* debug */
			0xFFD8DEE9,	/* info */
			0xFFE5B567,	/* warning */
			0xFFE06C75	/* error */
		};

		const char* const kLanguageNames[] = {
			"plain", "cpp", "csharp", "glsl", "json", "lua", "python",
			"ini", "markdown"
		};
		static_assert(sizeof(kLanguageNames) / sizeof(kLanguageNames[0]) == 9,
			"language name table out of sync");

		std::string normalize_newlines(const std::string& text)
		{
			std::string out;
			out.reserve(text.size());
			for (std::size_t i = 0; i < text.size(); ++i)
			{
				const char c = text[i];
				if (c == '\r')
				{
					out.push_back('\n');
					if (i + 1 < text.size() && text[i + 1] == '\n')
						++i;
				}
				else
				{
					out.push_back(c);
				}
			}
			return out;
		}

		std::string ascii_lower(std::string text)
		{
			for (char& c : text)
				c = (char)std::tolower((unsigned char)c);
			return text;
		}

		std::size_t utf8_char_len(unsigned char c)
		{
			if (c < 0x80)
				return 1;
			if ((c & 0xE0) == 0xC0)
				return 2;
			if ((c & 0xF0) == 0xE0)
				return 3;
			if ((c & 0xF8) == 0xF0)
				return 4;
			return 1;
		}

		bool is_utf8_continuation(unsigned char c)
		{
			return (c & 0xC0) == 0x80;
		}

		bool is_ident_start(unsigned char c)
		{
			return std::isalpha(c) || c == '_' || c >= 0x80;
		}

		bool is_ident_continue(unsigned char c)
		{
			return std::isalnum(c) || c == '_' || c >= 0x80;
		}

		bool is_space(char c)
		{
			return c == ' ' || c == '\t' || c == '\v' || c == '\f';
		}

		int tab_advance(int column, int tab_size)
		{
			if (tab_size <= 0)
				tab_size = 1;
			return column + tab_size - (column % tab_size);
		}

		const std::unordered_set<std::string>& keyword_set(code_language language)
		{
			static const std::unordered_set<std::string> empty;
			static const std::unordered_set<std::string> cpp = {
				"alignas", "alignof", "and", "asm", "auto", "break", "case",
				"catch", "class", "concept", "const", "consteval", "constexpr",
				"constinit", "const_cast", "continue", "co_await", "co_return",
				"co_yield", "decltype", "default", "delete", "do", "dynamic_cast",
				"else", "enum", "explicit", "export", "extern", "false", "final",
				"for", "friend", "goto", "if", "inline", "mutable", "namespace",
				"new", "noexcept", "nullptr", "operator", "override", "private",
				"protected", "public", "register", "reinterpret_cast", "requires",
				"return", "sizeof", "static", "static_assert", "static_cast",
				"struct", "switch", "template", "this", "thread_local", "throw",
				"true", "try", "typedef", "typeid", "typename", "union", "using",
				"virtual", "volatile", "while"
			};
			static const std::unordered_set<std::string> csharp = {
				"abstract", "as", "async", "await", "base", "break", "case",
				"catch", "checked", "class", "const", "continue", "default",
				"delegate", "do", "else", "enum", "event", "explicit", "extern",
				"false", "finally", "fixed", "for", "foreach", "get", "goto",
				"if", "implicit", "in", "interface", "internal", "is", "lock",
				"namespace", "new", "null", "operator", "out", "override",
				"params", "partial", "private", "protected", "public",
				"readonly", "record", "ref", "return", "sealed", "set",
				"sizeof", "stackalloc", "static", "struct", "switch", "this",
				"throw", "true", "try", "typeof", "unchecked", "unsafe",
				"using", "var", "virtual", "volatile", "while", "yield"
			};
			static const std::unordered_set<std::string> glsl = {
				"attribute", "break", "buffer", "case", "centroid", "coherent",
				"const", "continue", "default", "discard", "do", "else", "false",
				"flat", "for", "if", "in", "inout", "invariant", "layout",
				"noperspective", "out", "patch", "precise", "readonly",
				"restrict", "return", "sample", "shared", "smooth", "struct",
				"switch", "true", "uniform", "varying", "volatile", "while",
				"writeonly"
			};
			static const std::unordered_set<std::string> json = {
				"true", "false", "null"
			};
			static const std::unordered_set<std::string> lua = {
				"and", "break", "do", "else", "elseif", "end", "false", "for",
				"function", "goto", "if", "in", "local", "nil", "not", "or",
				"repeat", "return", "then", "true", "until", "while"
			};
			static const std::unordered_set<std::string> python = {
				"False", "None", "True", "and", "as", "assert", "async",
				"await", "break", "case", "class", "continue", "def", "del",
				"elif", "else", "except", "finally", "for", "from", "global",
				"if", "import", "in", "is", "lambda", "match", "nonlocal",
				"not", "or", "pass", "raise", "return", "self", "try", "while",
				"with", "yield"
			};
			static const std::unordered_set<std::string> ini = {
				"true", "false", "yes", "no", "on", "off", "null"
			};
			switch (language)
			{
			case code_language::cpp: return cpp;
			case code_language::csharp: return csharp;
			case code_language::glsl: return glsl;
			case code_language::json: return json;
			case code_language::lua: return lua;
			case code_language::python: return python;
			case code_language::ini: return ini;
			default: return empty;
			}
		}

		const std::unordered_set<std::string>& type_set(code_language language)
		{
			static const std::unordered_set<std::string> empty;
			static const std::unordered_set<std::string> cpp = {
				"bool", "char", "char8_t", "char16_t", "char32_t", "double",
				"float", "int", "long", "short", "signed", "unsigned", "void",
				"wchar_t", "size_t", "ptrdiff_t", "int8_t", "int16_t",
				"int32_t", "int64_t", "uint8_t", "uint16_t", "uint32_t",
				"uint64_t", "std"
			};
			static const std::unordered_set<std::string> csharp = {
				"bool", "byte", "char", "decimal", "double", "dynamic",
				"float", "int", "long", "object", "sbyte", "short", "string",
				"uint", "ulong", "ushort", "void"
			};
			static const std::unordered_set<std::string> glsl = {
				"bool", "bvec2", "bvec3", "bvec4", "double", "dvec2", "dvec3",
				"dvec4", "float", "int", "ivec2", "ivec3", "ivec4", "mat2",
				"mat3", "mat4", "mat2x2", "mat2x3", "mat2x4", "mat3x2",
				"mat3x3", "mat3x4", "mat4x2", "mat4x3", "mat4x4", "sampler1D",
				"sampler2D", "sampler3D", "samplerCube", "sampler2DArray",
				"sampler2DShadow", "samplerCubeShadow", "uvec2", "uvec3",
				"uvec4", "uint", "vec2", "vec3", "vec4", "void"
			};
			static const std::unordered_set<std::string> python = {
				"bool", "bytes", "dict", "float", "int", "list", "object",
				"set", "str", "tuple"
			};
			static const std::unordered_set<std::string> lua = {
				"boolean", "nil", "number", "string", "table", "thread",
				"userdata"
			};
			switch (language)
			{
			case code_language::cpp: return cpp;
			case code_language::csharp: return csharp;
			case code_language::glsl: return glsl;
			case code_language::lua: return lua;
			case code_language::python: return python;
			default: return empty;
			}
		}

		bool has_c_block_comments(code_language language)
		{
			return language == code_language::cpp ||
				language == code_language::csharp ||
				language == code_language::glsl;
		}

		bool has_slash_line_comments(code_language language)
		{
			return has_c_block_comments(language);
		}

		enum lex_state
		{
			lex_none = 0,
			lex_c_block_comment = 1,
			lex_python_single = 2,
			lex_python_double = 3,
			lex_lua_comment = 4,
			lex_lua_string = 5,
			lex_markdown_fence = 6
		};

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

	} /* namespace */

	/* ------------------------------------------------------------------------- */
	/* language helpers                                                          */
	/* ------------------------------------------------------------------------- */

	const char* code_language_name(code_language language) noexcept
	{
		const int index = static_cast<int>(language);
		if (index < 0 || index >= (int)(sizeof(kLanguageNames) /
			sizeof(kLanguageNames[0])))
			return "plain";
		return kLanguageNames[index];
	}

	bool code_language_from_name(const std::string& name,
		code_language* language) noexcept
	{
		const std::string lower = ascii_lower(name);
		for (int i = 0; i < (int)(sizeof(kLanguageNames) /
			sizeof(kLanguageNames[0])); ++i)
		{
			if (lower == kLanguageNames[i])
			{
				if (language)
					*language = static_cast<code_language>(i);
				return true;
			}
		}
		if (lower == "c++" || lower == "cxx")
		{
			if (language) *language = code_language::cpp;
			return true;
		}
		if (lower == "c#" || lower == "cs")
		{
			if (language) *language = code_language::csharp;
			return true;
		}
		if (lower == "py")
		{
			if (language) *language = code_language::python;
			return true;
		}
		if (lower == "md")
		{
			if (language) *language = code_language::markdown;
			return true;
		}
		return false;
	}

	code_language code_language_from_path(const std::string& path) noexcept
	{
		const std::size_t slash = path.find_last_of("/\\");
		const std::size_t dot = path.find_last_of('.');
		if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
			return code_language::plain;
		const std::string ext = ascii_lower(path.substr(dot + 1));
		if (ext == "c" || ext == "h" || ext == "cc" || ext == "hh" ||
			ext == "cpp" || ext == "hpp" || ext == "cxx" || ext == "hxx" ||
			ext == "inl" || ext == "ipp" || ext == "m" || ext == "mm")
			return code_language::cpp;
		if (ext == "cs")
			return code_language::csharp;
		if (ext == "glsl" || ext == "vert" || ext == "frag" ||
			ext == "geom" || ext == "tesc" || ext == "tese" ||
			ext == "comp" || ext == "hlsl" || ext == "shader")
			return code_language::glsl;
		if (ext == "json" || ext == "jsonc")
			return code_language::json;
		if (ext == "lua")
			return code_language::lua;
		if (ext == "py" || ext == "pyw")
			return code_language::python;
		if (ext == "ini" || ext == "cfg" || ext == "conf" || ext == "toml")
			return code_language::ini;
		if (ext == "md" || ext == "markdown")
			return code_language::markdown;
		return code_language::plain;
	}

	/* ------------------------------------------------------------------------- */
	/* tokenizer                                                                 */
	/* ------------------------------------------------------------------------- */

	void code_editor::tokenize_line(const std::string& text, int lex_in,
		std::vector<code_span>& spans, int* lex_out) const
	{
		spans.clear();
		int state = lex_in;
		if (state < lex_none || state > lex_markdown_fence)
			state = lex_none;

		const int length = (int)text.size();
		int pos = 0;

		auto find_from = [&](const char* needle, int from) -> int
			{
				const std::size_t found = text.find(needle, (std::size_t)from);
				return found == std::string::npos ? -1 : (int)found;
			};

		auto starts_with = [&](int at, const char* value) -> bool
			{
				const std::size_t n = std::strlen(value);
				return at >= 0 && (std::size_t)at + n <= text.size() &&
					text.compare((std::size_t)at, n, value) == 0;
			};

		while (state != lex_none && pos < length)
		{
			if (state == lex_c_block_comment)
			{
				const int close = find_from("*/", pos);
				if (close < 0)
				{
					append_span(spans, pos, length, code_token_kind::comment);
					pos = length;
					break;
				}
				append_span(spans, pos, close + 2, code_token_kind::comment);
				pos = close + 2;
				state = lex_none;
				break;
			}
			if (state == lex_python_single || state == lex_python_double)
			{
				const char* close_text = state == lex_python_single ? "'''" : "\"\"\"";
				const int close = find_from(close_text, pos);
				if (close < 0)
				{
					append_span(spans, pos, length,
						code_token_kind::string_literal);
					pos = length;
					break;
				}
				append_span(spans, pos, close + 3,
					code_token_kind::string_literal);
				pos = close + 3;
				state = lex_none;
				break;
			}
			if (state == lex_lua_comment || state == lex_lua_string)
			{
				const int close = find_from("]]", pos);
				if (close < 0)
				{
					append_span(spans, pos, length,
						state == lex_lua_comment ? code_token_kind::comment :
						code_token_kind::string_literal);
					pos = length;
					break;
				}
				append_span(spans, pos, close + 2,
					state == lex_lua_comment ? code_token_kind::comment :
					code_token_kind::string_literal);
				pos = close + 2;
				state = lex_none;
				break;
			}
			if (state == lex_markdown_fence)
			{
				int first = 0;
				while (first < length && is_space(text[(std::size_t)first]))
					++first;
				const bool closing = starts_with(first, "```") ||
					starts_with(first, "~~~");
				append_span(spans, 0, length,
					closing ? code_token_kind::comment :
					code_token_kind::string_literal);
				pos = length;
				state = closing ? lex_none : lex_markdown_fence;
				break;
			}
			break;
		}

		if (language_ == code_language::markdown)
		{
			if (state == lex_markdown_fence)
			{
				*lex_out = state;
				return;
			}
			while (pos < length)
			{
				const char c = text[(std::size_t)pos];
				if (is_space(c))
				{
					++pos;
					continue;
				}
				bool line_start = true;
				for (int i = pos - 1; i >= 0; --i)
				{
					if (!is_space(text[(std::size_t)i]))
					{
						line_start = false;
						break;
					}
				}
				if (line_start && (c == '#' || c == '>'))
				{
					append_span(spans, pos, length, code_token_kind::attribute);
					pos = length;
					break;
				}
				if (starts_with(pos, "```") || starts_with(pos, "~~~"))
				{
					append_span(spans, pos, length, code_token_kind::comment);
					pos = length;
					state = lex_markdown_fence;
					break;
				}
				if (c == '`')
				{
					const int close = find_from("`", pos + 1);
					if (close > pos)
					{
						append_span(spans, pos, close + 1,
							code_token_kind::string_literal);
						pos = close + 1;
						continue;
					}
				}
				if (c == '*' || c == '_')
				{
					const char pair[3] = { c, c, 0 };
					const int close = text.find(pair, (std::size_t)pos + 1) ==
						std::string::npos ? -1 :
						(int)text.find(pair, (std::size_t)pos + 1);
					if (close > pos + 1)
					{
						append_span(spans, pos, close + 2,
							code_token_kind::keyword);
						pos = close + 2;
						continue;
					}
				}
				if (c == '[')
				{
					const int close = find_from("](", pos + 1);
					const int end = close < 0 ? -1 : find_from(")", close + 2);
					if (close > pos && end > close)
					{
						append_span(spans, pos, end + 1, code_token_kind::tag);
						pos = end + 1;
						continue;
					}
				}
				++pos;
			}
			*lex_out = state;
			return;
		}

		if (pos >= length)
		{
			*lex_out = state;
			return;
		}

		const bool c_family = has_c_block_comments(language_);
		const bool slash_comments = has_slash_line_comments(language_);
		const bool hash_comments = language_ == code_language::python ||
			language_ == code_language::ini;
		const bool semi_comments = language_ == code_language::ini;
		const bool json = language_ == code_language::json;
		const bool lua = language_ == code_language::lua;
		const bool python = language_ == code_language::python;
		const bool glsl = language_ == code_language::glsl;

		/* A preprocessor directive owns the whole line in C-family languages. */
		if (c_family)
		{
			int first = pos;
			while (first < length && is_space(text[(std::size_t)first]))
				++first;
			if (first < length && text[(std::size_t)first] == '#')
			{
				append_span(spans, first, length, code_token_kind::preprocessor);
				*lex_out = lex_none;
				return;
			}
		}

		const auto& keywords = keyword_set(language_);
		const auto& types = type_set(language_);

		while (pos < length)
		{
			const char c = text[(std::size_t)pos];
			if (is_space(c))
			{
				++pos;
				continue;
			}

			if (slash_comments && c == '/' && pos + 1 < length &&
				text[(std::size_t)pos + 1] == '/')
			{
				append_span(spans, pos, length, code_token_kind::comment);
				pos = length;
				break;
			}
			if (c_family && c == '/' && pos + 1 < length &&
				text[(std::size_t)pos + 1] == '*')
			{
				const int close = find_from("*/", pos + 2);
				if (close < 0)
				{
					append_span(spans, pos, length, code_token_kind::comment);
					pos = length;
					state = lex_c_block_comment;
					break;
				}
				append_span(spans, pos, close + 2, code_token_kind::comment);
				pos = close + 2;
				continue;
			}
			if (lua && c == '-' && starts_with(pos, "--"))
			{
				if (starts_with(pos + 2, "[["))
				{
					const int close = find_from("]]", pos + 4);
					if (close < 0)
					{
						append_span(spans, pos, length, code_token_kind::comment);
						pos = length;
						state = lex_lua_comment;
						break;
					}
					append_span(spans, pos, close + 2,
						code_token_kind::comment);
					pos = close + 2;
					continue;
				}
				append_span(spans, pos, length, code_token_kind::comment);
				pos = length;
				break;
			}
			if (python && c == '#')
			{
				append_span(spans, pos, length, code_token_kind::comment);
				pos = length;
				break;
			}
			if (hash_comments && c == '#')
			{
				append_span(spans, pos, length, code_token_kind::comment);
				pos = length;
				break;
			}
			if (semi_comments && c == ';')
			{
				append_span(spans, pos, length, code_token_kind::comment);
				pos = length;
				break;
			}

			if (c == '"' || c == '\'')
			{
				if (python && pos + 2 < length)
				{
					if (starts_with(pos, "'''") || starts_with(pos, "\"\"\""))
					{
						const bool single = c == '\'';
						const char* close_text = single ? "'''" : "\"\"\"";
						const int close = find_from(close_text, pos + 3);
						if (close < 0)
						{
							append_span(spans, pos, length,
								code_token_kind::string_literal);
							pos = length;
							state = single ? lex_python_single :
								lex_python_double;
							break;
						}
						append_span(spans, pos, close + 3,
							code_token_kind::string_literal);
						pos = close + 3;
						continue;
					}
				}
				if (lua && c == '\'')
				{
					/* Lua short strings use the same escaping rules. */
				}
				const char quote = c;
				int i = pos + 1;
				bool closed = false;
				while (i < length)
				{
					const char s = text[(std::size_t)i];
					if (s == '\\')
					{
						i += (i + 1 < length) ? 2 : 1;
						continue;
					}
					if (s == quote)
					{
						++i;
						closed = true;
						break;
					}
					if (s == '\n')
						break;
					i += (int)utf8_char_len((unsigned char)s);
				}
				append_span(spans, pos, i, code_token_kind::string_literal);
				pos = i;
				if (!closed)
					break;
				continue;
			}

			if (lua && c == '[' && starts_with(pos, "[["))
			{
				const int close = find_from("]]", pos + 2);
				if (close < 0)
				{
					append_span(spans, pos, length,
						code_token_kind::string_literal);
					pos = length;
					state = lex_lua_string;
					break;
				}
				append_span(spans, pos, close + 2,
					code_token_kind::string_literal);
				pos = close + 2;
				continue;
			}

			if (json && c == '/' && pos + 1 < length &&
				(text[(std::size_t)pos + 1] == '/' ||
					text[(std::size_t)pos + 1] == '*'))
			{
				if (text[(std::size_t)pos + 1] == '/')
				{
					append_span(spans, pos, length, code_token_kind::comment);
					pos = length;
					break;
				}
				const int close = find_from("*/", pos + 2);
				if (close < 0)
				{
					append_span(spans, pos, length, code_token_kind::comment);
					pos = length;
					state = lex_c_block_comment;
					break;
				}
				append_span(spans, pos, close + 2, code_token_kind::comment);
				pos = close + 2;
				continue;
			}

			if (c == '#' && (language_ == code_language::ini))
			{
				append_span(spans, pos, length, code_token_kind::comment);
				break;
			}
			if (language_ == code_language::ini)
			{
				if (c == '[')
				{
					const int close = find_from("]", pos + 1);
					append_span(spans, pos, close < 0 ? length : close + 1,
						code_token_kind::attribute);
					pos = close < 0 ? length : close + 1;
					continue;
				}
			}

			if (std::isdigit((unsigned char)c) ||
				(c == '.' && pos + 1 < length &&
					std::isdigit((unsigned char)text[(std::size_t)pos + 1])))
			{
				int i = pos;
				if (text[(std::size_t)i] == '0' && i + 1 < length &&
					(text[(std::size_t)i + 1] == 'x' ||
						text[(std::size_t)i + 1] == 'X'))
				{
					i += 2;
					while (i < length &&
						std::isxdigit((unsigned char)text[(std::size_t)i]))
						++i;
				}
				else
				{
					bool exponent = false;
					while (i < length)
					{
						const char n = text[(std::size_t)i];
						if (std::isdigit((unsigned char)n) || n == '.')
						{
							++i;
							continue;
						}
						if ((n == 'e' || n == 'E' || n == 'p' || n == 'P') &&
							!exponent)
						{
							exponent = true;
							++i;
							if (i < length && (text[(std::size_t)i] == '+' ||
								text[(std::size_t)i] == '-'))
								++i;
							continue;
						}
						break;
					}
					if (i < length && (text[(std::size_t)i] == 'f' ||
						text[(std::size_t)i] == 'F' ||
						text[(std::size_t)i] == 'u' ||
						text[(std::size_t)i] == 'U' ||
						text[(std::size_t)i] == 'l' ||
						text[(std::size_t)i] == 'L'))
						++i;
				}
				append_span(spans, pos, i, code_token_kind::number);
				pos = i;
				continue;
			}

			if (is_ident_start((unsigned char)c))
			{
				int i = pos + 1;
				while (i < length &&
					is_ident_continue((unsigned char)text[(std::size_t)i]))
					++i;
				const std::string word = text.substr((std::size_t)pos,
					(std::size_t)(i - pos));
				int next = i;
				while (next < length && is_space(text[(std::size_t)next]))
					++next;
				const bool call = next < length &&
					text[(std::size_t)next] == '(';
				if (keywords.find(word) != keywords.end())
					append_span(spans, pos, i, code_token_kind::keyword);
				else if (types.find(word) != types.end())
					append_span(spans, pos, i, code_token_kind::type);
				else if (call)
					append_span(spans, pos, i, code_token_kind::function);
				pos = i;
				continue;
			}

			if (std::strchr("{}[]();,.:?", c))
			{
				append_span(spans, pos, pos + 1,
					code_token_kind::punctuation);
				++pos;
				continue;
			}

			if (std::strchr("+-*/%=<>!&|^~@", c))
			{
				int i = pos + 1;
				while (i < length && std::strchr("+-*/%=<>!&|^~@:",
					text[(std::size_t)i]))
					++i;
				append_span(spans, pos, i,
					code_token_kind::operator_token);
				pos = i;
				continue;
			}

			(void)glsl;
			++pos;
		}

		*lex_out = state;
	}

	class code_edit_command final : public command
	{
	public:
		code_edit_command(code_editor* owner, code_position start,
			std::string removed, std::string inserted, int merge_serial,
			std::string name)
			: owner_(owner), start_(start), removed_(std::move(removed)),
			inserted_(std::move(inserted)), merge_serial_(merge_serial),
			name_(std::move(name))
		{}

		const char* name() const noexcept override
		{
			return name_.c_str();
		}

		bool execute() override
		{
			if (!owner_ || !owner_->apply_change(start_, removed_, inserted_))
				return false;
			owner_->notify_change();
			return true;
		}

		void undo() override
		{
			if (!owner_)
				return;
			if (owner_->apply_change(start_, inserted_, removed_))
				owner_->notify_change();
		}

		bool merge_with(command& next) override
		{
			auto* other = dynamic_cast<code_edit_command*>(&next);
			if (!other || other->owner_ != owner_ || merge_serial_ == 0 ||
				merge_serial_ != other->merge_serial_ ||
				other->name_ != name_)
				return false;
			if (start_.line != other->start_.line)
				return false;

			/* Forward typing: the new character directly follows the old text. */
			if (removed_.empty() && other->removed_.empty() &&
				start_.column + (int)inserted_.size() == other->start_.column)
			{
				inserted_ += other->inserted_;
				return true;
			}
			/* Backspace: the new deletion precedes the old one. */
			if (inserted_.empty() && other->inserted_.empty() &&
				other->start_.column + (int)other->removed_.size() ==
				start_.column)
			{
				start_.column = other->start_.column;
				removed_ = other->removed_ + removed_;
				return true;
			}
			/* Forward delete: repeated deletion at the same offset. */
			if (inserted_.empty() && other->inserted_.empty() &&
				start_.column == other->start_.column)
			{
				removed_ += other->removed_;
				return true;
			}
			return false;
		}

	private:
		code_editor* owner_ = nullptr;
		code_position start_{};
		std::string removed_;
		std::string inserted_;
		int merge_serial_ = 0;
		std::string name_;
	};

	/* ------------------------------------------------------------------------- */
	/* construction                                                              */
	/* ------------------------------------------------------------------------- */

	code_editor::code_editor()
	{
		for (int i = 0; i < code_token_kind_count; ++i)
			token_colors_[i] = kDefaultTokenColors[i];
		for (int i = 0; i < code_log_level_count; ++i)
			log_colors_[i] = kDefaultLogColors[i];
		set_document_raw(std::string());
	}

	code_editor::~code_editor() = default;

	void code_editor::build(vgui_widget* parent)
	{
		if (root_ || !parent)
			return;

		context_ = vgui_widget_get_context(parent);

		root_ = vgui_widget_add(parent, VGUI_PANEL, nullptr);
		vgui_widget_set_id(root_, "code_editor");
		vgui_widget_set_a11y_name(root_, "Code editor");
		vgui_widget_set_grow(root_, 1.0f);
		vgui_widget_set_min_size(root_, 200, 100);
		vgui_widget_set_size(root_, 0, 0);
		vgui_widget_set_align_self(root_, VGUI_ALIGN_STRETCH);
		vgui_widget_set_layout(root_, VGUI_LAYOUT_COLUMN);
		vgui_widget_set_align_items(root_, VGUI_ALIGN_STRETCH);
		vgui_widget_set_padding(root_, 0, 0, 0, 0);
		vgui_widget_set_gap(root_, 2);
		vgui_widget_set_background(root_, 0);
		vgui_widget_set_border(root_, 0, 0);

		find_bar_ = vgui_widget_add(root_, VGUI_PANEL, nullptr);
		vgui_widget_set_id(find_bar_, "code_editor_find_bar");
		vgui_widget_set_layout(find_bar_, VGUI_LAYOUT_ROW);
		vgui_widget_set_align_items(find_bar_, VGUI_ALIGN_CENTER);
		vgui_widget_set_padding(find_bar_, 4, 2, 4, 2);
		vgui_widget_set_gap(find_bar_, 4);
		vgui_widget_set_size(find_bar_, 0, 30);
		vgui_widget_set_min_size(find_bar_, 0, 30);
		vgui_widget_set_background(find_bar_, 0);
		vgui_widget_set_border(find_bar_, 0, 0);
		vgui_widget_set_visible(find_bar_, 0);

		prev_button_ = vgui_widget_add(find_bar_, VGUI_BUTTON, "<");
		vgui_widget_set_id(prev_button_, "code_editor_find_prev");
		vgui_widget_set_size(prev_button_, 24, 22);
		vgui_widget_set_tooltip(prev_button_, "Previous match (Shift+Enter)");
		vgui_widget_on_event(prev_button_, &code_editor::on_button_event, this);

		next_button_ = vgui_widget_add(find_bar_, VGUI_BUTTON, ">");
		vgui_widget_set_id(next_button_, "code_editor_find_next");
		vgui_widget_set_size(next_button_, 24, 22);
		vgui_widget_set_tooltip(next_button_, "Next match (Enter)");
		vgui_widget_on_event(next_button_, &code_editor::on_button_event, this);

		case_button_ = vgui_widget_add(find_bar_, VGUI_CHECKBOX, "Aa");
		vgui_widget_set_id(case_button_, "code_editor_find_case");
		vgui_widget_set_size(case_button_, 44, 22);
		vgui_widget_set_min_size(case_button_, 38, 22);
		vgui_widget_set_font_size(case_button_, 11);
		vgui_widget_set_tooltip(case_button_, "Case sensitive");
		vgui_widget_set_checked(case_button_, find_case_sensitive_ ? 1 : 0);
		vgui_widget_on_event(case_button_, &code_editor::on_button_event, this);

		close_find_button_ = vgui_widget_add(find_bar_, VGUI_BUTTON, "x");
		vgui_widget_set_id(close_find_button_, "code_editor_find_close");
		vgui_widget_set_size(close_find_button_, 24, 22);
		vgui_widget_set_tooltip(close_find_button_, "Close find bar (Escape)");
		vgui_widget_on_event(close_find_button_,
			&code_editor::on_button_event, this);

		search_.build(find_bar_, "Find");
		vgui_widget_set_id(search_.widget(), "code_editor_find");
		vgui_widget_set_grow(search_.widget(), 1.0f);
		vgui_widget_set_min_size(search_.widget(), 100, 24);
		search_.set_on_change([this](const std::string& text)
			{
				on_find_changed(text, this);
			});
		search_.set_on_submit([this](const std::string& text)
			{
				on_find_submit(text, this);
			});

		replace_edit_ = vgui_widget_add(find_bar_, VGUI_EDIT, nullptr);
		vgui_widget_set_id(replace_edit_, "code_editor_replace");
		vgui_widget_set_size(replace_edit_, 150, 24);
		vgui_widget_set_min_size(replace_edit_, 90, 24);
		vgui_edit_set_placeholder(replace_edit_, "Replace");
		vgui_widget_on_event(replace_edit_, &code_editor::on_replace_event, this);
		vgui_widget_set_visible(replace_edit_, 0);

		replace_button_ = vgui_widget_add(find_bar_, VGUI_BUTTON, "Replace");
		vgui_widget_set_id(replace_button_, "code_editor_replace_one");
		vgui_widget_set_size(replace_button_, 70, 22);
		vgui_widget_on_event(replace_button_, &code_editor::on_button_event, this);
		vgui_widget_set_visible(replace_button_, 0);

		replace_all_button_ = vgui_widget_add(find_bar_, VGUI_BUTTON, "All");
		vgui_widget_set_id(replace_all_button_, "code_editor_replace_all");
		vgui_widget_set_size(replace_all_button_, 42, 22);
		vgui_widget_set_tooltip(replace_all_button_, "Replace all matches");
		vgui_widget_on_event(replace_all_button_,
			&code_editor::on_button_event, this);
		vgui_widget_set_visible(replace_all_button_, 0);

		editor_ = vgui_widget_add(root_, VGUI_CUSTOM, nullptr);
		vgui_widget_set_id(editor_, "code_editor_surface");
		vgui_widget_set_grow(editor_, 1.0f);
		vgui_widget_set_min_size(editor_, 0, 40);
		vgui_widget_set_size(editor_, 0, 0);
		vgui_widget_set_align_self(editor_, VGUI_ALIGN_STRETCH);
		vgui_widget_set_focusable(editor_, 1);
		vgui_widget_set_cursor(editor_, VGUI_CURSOR_IBEAM);
		vgui_widget_set_font_size(editor_, font_size_);
		vgui_widget_set_wants_text_input(editor_, 1);
		vgui_custom_set_draw(editor_, &code_editor::on_draw, this);
		vgui_custom_set_event(editor_, &code_editor::on_event, this);
		vgui_custom_set_clip_content(editor_, 1);

		/* Hidden single-line editor used as the system-clipboard bridge. */
		clipboard_sink_ = vgui_widget_add(root_, VGUI_EDIT, nullptr);
		vgui_widget_set_id(clipboard_sink_, "code_editor_clipboard");
		vgui_widget_set_size(clipboard_sink_, 1, 1);
		vgui_widget_set_visible(clipboard_sink_, 0);

		search_.set_text(find_query_);
		update_find_visibility();
	}

	bool code_editor::built() const noexcept
	{
		return root_ != nullptr;
	}

	vgui_widget* code_editor::widget() const noexcept
	{
		return root_;
	}

	vgui_widget* code_editor::editor_widget() const noexcept
	{
		return editor_;
	}

	vgui_widget* code_editor::find_bar() const noexcept
	{
		return find_bar_;
	}

	void code_editor::request_focus()
	{
		if (context_ && editor_)
			vgui_set_focus(context_, editor_);
	}

	bool code_editor::focused() const
	{
		return context_ && editor_ && vgui_get_focus(context_) == editor_;
	}

	/* ------------------------------------------------------------------------- */
	/* document                                                                  */
	/* ------------------------------------------------------------------------- */

	void code_editor::set_document_raw(std::string text)
	{
		text = normalize_newlines(text);
		lines_.clear();
		max_line_bytes_ = 0;
		std::size_t begin = 0;
		while (true)
		{
			const std::size_t end = text.find('\n', begin);
			line_record record;
			if (end == std::string::npos)
			{
				record.text = text.substr(begin);
				lines_.push_back(std::move(record));
				break;
			}
			record.text = text.substr(begin, end - begin);
			lines_.push_back(std::move(record));
			begin = end + 1;
			if (begin == text.size())
			{
				record = line_record();
				lines_.push_back(std::move(record));
				break;
			}
		}
		if (lines_.empty())
			lines_.push_back(line_record());
		for (const line_record& line : lines_)
			max_line_bytes_ = std::max(max_line_bytes_, line.text.size());
		highlight_valid_ = 0;
	}

	void code_editor::set_text(std::string text)
	{
		set_document_raw(std::move(text));
		caret_ = code_position{};
		anchor_ = code_position{};
		scroll_x_ = 0.0f;
		scroll_y_ = 0.0f;
		edit_group_ = 1;
		local_history_.clear();
		local_history_index_ = 0;
		matches_.clear();
		active_match_ = -1;
		loading_ = false;
		load_stream_.close();
		load_memory_.clear();
		load_pending_.clear();
		load_memory_offset_ = 0;
		load_read_bytes_ = 0;
		load_total_bytes_ = 0;
		if (command_stack_)
			command_stack_->clear();
		update_text_input_rect();
		notify_change();
	}

	std::string code_editor::text() const
	{
		std::string out;
		std::size_t total = 0;
		for (const line_record& line : lines_)
			total += line.text.size() + 1;
		out.reserve(total);
		for (std::size_t i = 0; i < lines_.size(); ++i)
		{
			if (i)
				out.push_back('\n');
			out += lines_[i].text;
		}
		return out;
	}

	void code_editor::clear()
	{
		set_text(std::string());
	}

	void code_editor::append_text(std::string text)
	{
		text = normalize_newlines(text);
		if (text.empty())
			return;
		if (lines_.empty())
			lines_.push_back(line_record());
		const std::size_t previous_last = lines_.size() - 1;
		std::string combined = lines_.back().text;
		combined += text;
		lines_.pop_back();

		std::size_t begin = 0;
		while (true)
		{
			const std::size_t end = combined.find('\n', begin);
			line_record record;
			if (end == std::string::npos)
			{
				record.text = combined.substr(begin);
				lines_.push_back(std::move(record));
				break;
			}
			record.text = combined.substr(begin, end - begin);
			lines_.push_back(std::move(record));
			begin = end + 1;
			if (begin == combined.size())
			{
				record = line_record();
				lines_.push_back(std::move(record));
				break;
			}
		}
		for (const line_record& line : lines_)
			max_line_bytes_ = std::max(max_line_bytes_, line.text.size());
		highlight_valid_ = std::min(highlight_valid_, previous_last);
		caret_ = clamp_position(caret_);
		anchor_ = clamp_position(anchor_);
		update_text_input_rect();
		notify_change();
	}

	std::size_t code_editor::line_count() const noexcept
	{
		return lines_.size();
	}

	std::string code_editor::line_text(int line) const
	{
		if (line < 0 || (std::size_t)line >= lines_.size())
			return std::string();
		return lines_[(std::size_t)line].text;
	}

	bool code_editor::set_line_text(int line, std::string text)
	{
		if (line < 0 || (std::size_t)line >= lines_.size())
			return false;
		text = normalize_newlines(std::move(text));
		const code_position start{ line, 0 };
		const code_position end{ line, line_length(line) };
		return replace_range(start, end, std::move(text), 0, "Set Line");
	}

	const std::vector<code_span>& code_editor::line_spans(int line)
	{
		static const std::vector<code_span> empty;
		if (line < 0 || (std::size_t)line >= lines_.size())
			return empty;
		ensure_highlight(line);
		return lines_[(std::size_t)line].spans;
	}

	bool code_editor::set_line_spans(int line, std::vector<code_span> spans)
	{
		if (line < 0 || (std::size_t)line >= lines_.size())
			return false;
		const int length = line_length(line);
		for (const code_span& span : spans)
		{
			if (span.start < 0 || span.length < 0 ||
				span.start > length || span.length > length - span.start)
				return false;
			if (!valid_token_kind((int)span.kind))
				return false;
		}
		std::sort(spans.begin(), spans.end(),
			[](const code_span& a, const code_span& b)
			{
				return a.start < b.start;
			});
		line_record& record = lines_[(std::size_t)line];
		record.spans = std::move(spans);
		record.manual_spans = true;
		record.lex_after = lex_none;
		if (highlight_valid_ > (std::size_t)line + 1)
			highlight_valid_ = (std::size_t)line + 1;
		return true;
	}

	bool code_editor::highlight_valid_upto(std::size_t* count) const noexcept
	{
		if (count)
			*count = highlight_valid_;
		return highlight_valid_ >= lines_.size();
	}

	/* ------------------------------------------------------------------------- */
	/* language / presentation                                                   */
	/* ------------------------------------------------------------------------- */

	void code_editor::set_language(code_language language)
	{
		if (language_ == language)
			return;
		language_ = language;
		highlight_valid_ = 0;
		notify_change();
	}

	code_language code_editor::language() const noexcept
	{
		return language_;
	}

	void code_editor::set_language_from_path(const std::string& path)
	{
		set_language(code_language_from_path(path));
	}

	void code_editor::set_read_only(bool read_only)
	{
		read_only_ = read_only;
		if (read_only_)
			close_find();
	}

	bool code_editor::read_only() const noexcept
	{
		return read_only_;
	}

	void code_editor::set_show_line_numbers(bool show)
	{
		show_line_numbers_ = show;
		update_text_input_rect();
	}

	bool code_editor::show_line_numbers() const noexcept
	{
		return show_line_numbers_;
	}

	void code_editor::set_tab_size(int tab_size)
	{
		tab_size_ = std::clamp(tab_size, 1, 16);
		update_text_input_rect();
	}

	int code_editor::tab_size() const noexcept
	{
		return tab_size_;
	}

	void code_editor::set_font_size(float size)
	{
		font_size_ = std::clamp(size, 6.0f, 96.0f);
		if (editor_)
			vgui_widget_set_font_size(editor_, font_size_);
		update_text_input_rect();
	}

	float code_editor::font_size() const noexcept
	{
		return font_size_;
	}

	void code_editor::set_token_color(code_token_kind kind, vgui_color color)
	{
		const int index = static_cast<int>(kind);
		if (valid_token_kind(index))
			token_colors_[index] = color;
	}

	vgui_color code_editor::token_color(code_token_kind kind) const noexcept
	{
		const int index = static_cast<int>(kind);
		if (!valid_token_kind(index) || !token_colors_[index])
			return valid_token_kind(index) ? kDefaultTokenColors[index] :
			kDefaultTokenColors[0];
		return token_colors_[index];
	}

	void code_editor::reset_token_colors()
	{
		for (int i = 0; i < code_token_kind_count; ++i)
			token_colors_[i] = kDefaultTokenColors[i];
	}

	/* ------------------------------------------------------------------------- */
	/* model helpers                                                             */
	/* ------------------------------------------------------------------------- */

	void code_editor::notify_change()
	{
		caret_ = clamp_position(caret_);
		anchor_ = clamp_position(anchor_);
		if (!find_query_.empty())
			rebuild_matches();
		else if (!matches_.empty())
		{
			matches_.clear();
			active_match_ = -1;
		}
		update_text_input_rect();
		if (change_listener_)
			change_listener_();
	}

	void code_editor::normalize_after_change(int first_line)
	{
		if (lines_.empty())
			lines_.push_back(line_record());
		if (first_line < 0)
			first_line = 0;
		if ((std::size_t)first_line >= lines_.size())
			first_line = (int)lines_.size() - 1;
		if (highlight_valid_ > (std::size_t)first_line)
			highlight_valid_ = (std::size_t)first_line;
		for (std::size_t i = (std::size_t)first_line; i < lines_.size(); ++i)
		{
			lines_[i].spans.clear();
			lines_[i].manual_spans = false;
			lines_[i].lex_after = lex_none;
		}
	}

	void code_editor::ensure_highlight(int line)
	{
		if (line < 0 || (std::size_t)line >= lines_.size())
			return;
		if ((std::size_t)line < highlight_valid_)
			return;
		for (std::size_t i = highlight_valid_; i <= (std::size_t)line; ++i)
		{
			line_record& record = lines_[i];
			if (record.manual_spans)
			{
				record.lex_after = lex_none;
			}
			else
			{
				int lex_in = lex_none;
				if (i > 0)
					lex_in = lines_[i - 1].lex_after;
				int lex_out = lex_none;
				tokenize_line(record.text, lex_in, record.spans, &lex_out);
				record.lex_after = lex_out;
			}
			if (i + 1 > highlight_valid_)
				highlight_valid_ = i + 1;
		}
	}

	code_position code_editor::clamp_position(code_position value) const
	{
		if (lines_.empty())
			return code_position{};
		if (value.line < 0)
			value.line = 0;
		if ((std::size_t)value.line >= lines_.size())
			value.line = (int)lines_.size() - 1;
		value.column = clamp_column(value.line, value.column);
		return value;
	}

	int code_editor::line_length(int line) const
	{
		if (line < 0 || (std::size_t)line >= lines_.size())
			return 0;
		return (int)lines_[(std::size_t)line].text.size();
	}

	int code_editor::clamp_column(int line, int column) const
	{
		const int length = line_length(line);
		if (column <= 0)
			return 0;
		if (column >= length)
			return length;
		const std::string& text = lines_[(std::size_t)line].text;
		while (column > 0 && column < length &&
			is_utf8_continuation((unsigned char)text[(std::size_t)column]))
			--column;
		return column;
	}

	int code_editor::advance_column(const std::string& text, int column)
	{
		if (column < 0)
			column = 0;
		if ((std::size_t)column >= text.size())
			return (int)text.size();
		const int step = (int)utf8_char_len((unsigned char)text[(std::size_t)column]);
		return std::min((int)text.size(), column + std::max(1, step));
	}

	int code_editor::retreat_column(const std::string& text, int column)
	{
		if (column <= 0)
			return 0;
		if ((std::size_t)column > text.size())
			column = (int)text.size();
		--column;
		while (column > 0 &&
			is_utf8_continuation((unsigned char)text[(std::size_t)column]))
			--column;
		return column;
	}

	code_position code_editor::advance_position(
		const std::vector<line_record>& lines, code_position start,
		const std::string& text)
	{
		code_position result;
		if (lines.empty())
			return result;
		result.line = std::clamp(start.line, 0, (int)lines.size() - 1);
		result.column = std::clamp(start.column, 0,
			(int)lines[(std::size_t)result.line].text.size());
		for (std::size_t i = 0; i < text.size();)
		{
			const char c = text[i];
			if (c == '\n')
			{
				if (result.line + 1 < (int)lines.size())
					++result.line;
				result.column = 0;
				++i;
				continue;
			}
			result.column += (int)utf8_char_len((unsigned char)c);
			i += utf8_char_len((unsigned char)c);
		}
		const int length = (int)lines[(std::size_t)result.line].text.size();
		if (result.column > length)
			result.column = length;
		if (result.column < 0)
			result.column = 0;
		return result;
	}

	std::string code_editor::text_range(code_position start,
		code_position end) const
	{
		return text_range_raw(lines_, start, end);
	}

	std::string code_editor::text_range_raw(
		const std::vector<line_record>& lines, code_position start,
		code_position end) const
	{
		if (lines.empty())
			return std::string();
		start.line = std::clamp(start.line, 0, (int)lines.size() - 1);
		end.line = std::clamp(end.line, 0, (int)lines.size() - 1);
		start.column = std::clamp(start.column, 0,
			(int)lines[(std::size_t)start.line].text.size());
		end.column = std::clamp(end.column, 0,
			(int)lines[(std::size_t)end.line].text.size());
		if (end < start)
			std::swap(start, end);
		if (start.line == end.line)
			return lines[(std::size_t)start.line].text.substr(
				(std::size_t)start.column,
				(std::size_t)(end.column - start.column));
		std::string out;
		out += lines[(std::size_t)start.line].text.substr(
			(std::size_t)start.column);
		out.push_back('\n');
		for (int line = start.line + 1; line < end.line; ++line)
		{
			out += lines[(std::size_t)line].text;
			out.push_back('\n');
		}
		out += lines[(std::size_t)end.line].text.substr(0,
			(std::size_t)end.column);
		return out;
	}

	bool code_editor::apply_change(code_position start, const std::string& removed,
		const std::string& inserted)
	{
		if (lines_.empty())
			lines_.push_back(line_record());
		start = clamp_position(start);
		const code_position end = clamp_position(
			advance_position(lines_, start, removed));
		if (text_range(start, end) != removed)
			return false;

		const std::string head = lines_[(std::size_t)start.line].text.substr(
			0, (std::size_t)start.column);
		const std::string tail = lines_[(std::size_t)end.line].text.substr(
			(std::size_t)end.column);
		const std::string combined = head + inserted + tail;

		std::vector<line_record> replacement;
		std::size_t begin = 0;
		while (true)
		{
			const std::size_t newline = combined.find('\n', begin);
			line_record record;
			if (newline == std::string::npos)
			{
				record.text = combined.substr(begin);
				replacement.push_back(std::move(record));
				break;
			}
			record.text = combined.substr(begin, newline - begin);
			replacement.push_back(std::move(record));
			begin = newline + 1;
			if (begin == combined.size())
			{
				record = line_record();
				replacement.push_back(std::move(record));
				break;
			}
		}
		if (replacement.empty())
			replacement.push_back(line_record());

		lines_.erase(lines_.begin() + start.line,
			lines_.begin() + end.line + 1);
		lines_.insert(lines_.begin() + start.line,
			std::make_move_iterator(replacement.begin()),
			std::make_move_iterator(replacement.end()));

		if (inserted.size() > removed.size())
			max_line_bytes_ = std::max(max_line_bytes_, inserted.size());
		normalize_after_change(start.line);
		caret_ = clamp_position(caret_);
		anchor_ = clamp_position(anchor_);
		return true;
	}

	bool code_editor::replace_range(code_position start, code_position end,
		std::string inserted, int merge_serial, const char* command_name)
	{
		start = clamp_position(start);
		end = clamp_position(end);
		if (end < start)
			std::swap(start, end);
		std::string removed = text_range(start, end);
		if (removed.empty() && inserted.empty())
			return false;

		if (!apply_change(start, removed, inserted))
			return false;

		const code_position new_caret =
			clamp_position(advance_position(lines_, start, inserted));
		caret_ = new_caret;
		anchor_ = new_caret;

		if (command_stack_)
		{
			push_edit_command(start, std::move(removed), std::move(inserted),
				merge_serial, command_name);
		}
		else
		{
			text_change change;
			change.start = start;
			change.removed = removed;
			change.inserted = inserted;
			change.merge_serial = merge_serial;
			if (local_history_index_ < local_history_.size())
				local_history_.erase(local_history_.begin() +
					(std::ptrdiff_t)local_history_index_, local_history_.end());
			bool merged = false;
			if (merge_serial != 0 && !local_history_.empty() &&
				local_history_.back().merge_serial == merge_serial)
			{
				text_change& last = local_history_.back();
				if (last.start.line == start.line && last.removed.empty() &&
					change.removed.empty() &&
					last.start.column + (int)last.inserted.size() ==
					start.column)
				{
					last.inserted += change.inserted;
					merged = true;
				}
				else if (last.start.line == start.line && last.inserted.empty() &&
					change.inserted.empty() &&
					start.column + (int)change.removed.size() ==
					last.start.column)
				{
					last.start.column = start.column;
					last.removed = change.removed + last.removed;
					merged = true;
				}
				else if (last.start.line == start.line &&
					last.start.column == start.column && last.inserted.empty() &&
					change.inserted.empty())
				{
					last.removed += change.removed;
					merged = true;
				}
			}
			if (!merged)
				local_history_.push_back(std::move(change));
			if (local_history_.size() > 1024)
			{
				local_history_.erase(local_history_.begin());
				if (local_history_index_ > 0)
					--local_history_index_;
			}
			local_history_index_ = local_history_.size();
		}

		ensure_caret_visible();
		update_text_input_rect();
		notify_change();
		return true;
	}

	bool code_editor::push_edit_command(code_position start, std::string removed,
		std::string inserted, int merge_serial, const char* command_name)
	{
		if (!command_stack_)
			return false;
		auto cmd = std::make_unique<code_edit_command>(this, start,
			std::move(removed), std::move(inserted), merge_serial,
			command_name ? command_name : "Edit");
		command_stack_->push(std::move(cmd));
		return true;
	}

	bool code_editor::apply_local_undo()
	{
		if (local_history_index_ == 0 || local_history_.empty())
			return false;
		--local_history_index_;
		const text_change& change = local_history_[local_history_index_];
		if (!apply_change(change.start, change.inserted, change.removed))
			return false;
		caret_ = clamp_position(advance_position(lines_, change.start,
			change.removed));
		anchor_ = caret_;
		ensure_caret_visible();
		notify_change();
		return true;
	}

	bool code_editor::apply_local_redo()
	{
		if (local_history_index_ >= local_history_.size())
			return false;
		const text_change& change = local_history_[local_history_index_];
		if (!apply_change(change.start, change.removed, change.inserted))
			return false;
		++local_history_index_;
		caret_ = clamp_position(advance_position(lines_, change.start,
			change.inserted));
		anchor_ = caret_;
		ensure_caret_visible();
		notify_change();
		return true;
	}

	bool code_editor::undo()
	{
		if (command_stack_)
			return command_stack_->undo();
		return apply_local_undo();
	}

	bool code_editor::redo()
	{
		if (command_stack_)
			return command_stack_->redo();
		return apply_local_redo();
	}

	bool code_editor::can_undo() const
	{
		if (command_stack_)
			return command_stack_->can_undo();
		return local_history_index_ > 0;
	}

	bool code_editor::can_redo() const
	{
		if (command_stack_)
			return command_stack_->can_redo();
		return local_history_index_ < local_history_.size();
	}

	void code_editor::set_command_stack(class command_stack* stack)
	{
		command_stack_ = stack;
		if (!stack)
		{
			local_history_.clear();
			local_history_index_ = 0;
		}
	}

	class command_stack* code_editor::command_stack() const noexcept
	{
		return command_stack_;
	}

	/* ------------------------------------------------------------------------- */
	/* editing                                                                   */
	/* ------------------------------------------------------------------------- */

	bool code_editor::insert_text(std::string text)
	{
		if (read_only_)
			return false;
		text = normalize_newlines(std::move(text));
		if (text.empty())
			return false;
		code_position start = caret_;
		code_position end = caret_;
		code_position sel_start;
		code_position sel_end;
		if (selection_range(&sel_start, &sel_end))
		{
			start = sel_start;
			end = sel_end;
		}
		return replace_range(start, end, std::move(text), edit_group_,
			"Insert");
	}

	bool code_editor::backspace()
	{
		if (read_only_)
			return false;
		code_position start = caret_;
		code_position end = caret_;
		code_position sel_start;
		code_position sel_end;
		if (selection_range(&sel_start, &sel_end))
		{
			start = sel_start;
			end = sel_end;
		}
		else if (caret_.column > 0)
		{
			start.column = retreat_column(
				lines_[(std::size_t)caret_.line].text, caret_.column);
		}
		else if (caret_.line > 0)
		{
			start.line = caret_.line - 1;
			start.column = line_length(start.line);
		}
		else
		{
			return false;
		}
		if (start == end && start == caret_ && caret_.column == 0 &&
			caret_.line == 0)
			return false;
		return replace_range(start, end, std::string(), edit_group_,
			"Backspace");
	}

	bool code_editor::delete_forward()
	{
		if (read_only_)
			return false;
		code_position start = caret_;
		code_position end = caret_;
		code_position sel_start;
		code_position sel_end;
		if (selection_range(&sel_start, &sel_end))
		{
			start = sel_start;
			end = sel_end;
		}
		else if (caret_.column < line_length(caret_.line))
		{
			end.column = advance_column(
				lines_[(std::size_t)caret_.line].text, caret_.column);
		}
		else if (caret_.line + 1 < (int)lines_.size())
		{
			end.line = caret_.line + 1;
			end.column = 0;
		}
		else
		{
			return false;
		}
		return replace_range(start, end, std::string(), edit_group_,
			"Delete");
	}

	bool code_editor::newline()
	{
		if (read_only_)
			return false;
		code_position start = caret_;
		code_position end = caret_;
		code_position sel_start;
		code_position sel_end;
		if (selection_range(&sel_start, &sel_end))
		{
			start = sel_start;
			end = sel_end;
		}
		const std::string& line = lines_[(std::size_t)start.line].text;
		std::string indent;
		std::size_t i = 0;
		while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
		{
			indent.push_back(line[i]);
			++i;
		}
		int last = start.column - 1;
		while (last >= 0 && (line[(std::size_t)last] == ' ' ||
			line[(std::size_t)last] == '\t'))
			--last;
		if (last >= 0 && (line[(std::size_t)last] == '{' ||
			line[(std::size_t)last] == '[' ||
			line[(std::size_t)last] == '(' ||
			line[(std::size_t)last] == ':'))
			indent.push_back('\t');
		return replace_range(start, end, "\n" + indent, 0, "New Line");
	}

	bool code_editor::indent_selection()
	{
		if (read_only_)
			return false;
		code_position start;
		code_position end;
		if (!selection_range(&start, &end))
		{
			start = code_position{ caret_.line, 0 };
			end = code_position{ caret_.line, line_length(caret_.line) };
		}
		else if (end.column == 0 && end.line > start.line)
		{
			--end.line;
			end.column = line_length(end.line);
		}
		if (end < start)
			return false;
		const std::string old = text_range(start, end);
		std::string out;
		out.reserve(old.size() + 16);
		bool first = true;
		std::size_t begin = 0;
		while (true)
		{
			const std::size_t newline = old.find('\n', begin);
			if (!first)
				out.push_back('\n');
			first = false;
			out.push_back('\t');
			if (newline == std::string::npos)
			{
				out += old.substr(begin);
				break;
			}
			out += old.substr(begin, newline - begin);
			begin = newline + 1;
		}
		return replace_range(start, end, out, 0, "Indent");
	}

	bool code_editor::outdent_selection()
	{
		if (read_only_)
			return false;
		code_position start;
		code_position end;
		if (!selection_range(&start, &end))
		{
			start = code_position{ caret_.line, 0 };
			end = code_position{ caret_.line, line_length(caret_.line) };
		}
		else if (end.column == 0 && end.line > start.line)
		{
			--end.line;
			end.column = line_length(end.line);
		}
		const std::string old = text_range(start, end);
		std::string out;
		out.reserve(old.size());
		std::size_t begin = 0;
		while (true)
		{
			const std::size_t newline = old.find('\n', begin);
			const std::string each = newline == std::string::npos ?
				old.substr(begin) : old.substr(begin, newline - begin);
			std::size_t remove = 0;
			if (!each.empty() && each[0] == '\t')
				remove = 1;
			else
			{
				while (remove < each.size() && remove < (std::size_t)tab_size_ &&
					each[remove] == ' ')
					++remove;
			}
			out += each.substr(remove);
			if (newline == std::string::npos)
				break;
			out.push_back('\n');
			begin = newline + 1;
		}
		if (out == old)
			return false;
		return replace_range(start, end, out, 0, "Outdent");
	}

	/* ------------------------------------------------------------------------- */
	/* caret / selection                                                         */
	/* ------------------------------------------------------------------------- */

	code_position code_editor::caret() const noexcept
	{
		return caret_;
	}

	void code_editor::set_caret(code_position position)
	{
		set_caret_internal(position, false);
	}

	void code_editor::set_caret(int line, int column)
	{
		set_caret(code_position{ line, column });
	}

	int code_editor::caret_line() const noexcept
	{
		return caret_.line;
	}

	int code_editor::caret_column() const noexcept
	{
		return caret_.column;
	}

	code_position code_editor::selection_anchor() const noexcept
	{
		return anchor_;
	}

	bool code_editor::has_selection() const noexcept
	{
		return anchor_ != caret_;
	}

	void code_editor::set_selection(code_position anchor, code_position caret)
	{
		anchor_ = clamp_position(anchor);
		caret_ = clamp_position(caret);
		++edit_group_;
		update_text_input_rect();
	}

	void code_editor::select_all()
	{
		anchor_ = code_position{ 0, 0 };
		caret_.line = (int)lines_.size() - 1;
		caret_.column = line_length(caret_.line);
		++edit_group_;
		update_text_input_rect();
	}

	void code_editor::clear_selection()
	{
		anchor_ = caret_;
		++edit_group_;
	}

	std::string code_editor::selected_text() const
	{
		code_position start;
		code_position end;
		if (!selection_range(&start, &end))
			return std::string();
		return text_range(start, end);
	}

	void code_editor::set_caret_internal(code_position position,
		bool keep_selection)
	{
		caret_ = clamp_position(position);
		if (!keep_selection)
			anchor_ = caret_;
		++edit_group_;
		update_text_input_rect();
	}

	void code_editor::move_caret(code_position position, bool extend_selection)
	{
		caret_ = clamp_position(position);
		if (!extend_selection)
			anchor_ = caret_;
		++edit_group_;
		ensure_caret_visible();
		update_text_input_rect();
	}

	void code_editor::select_word_at(code_position position)
	{
		position = clamp_position(position);
		const std::string& text = lines_[(std::size_t)position.line].text;
		if (text.empty())
		{
			anchor_ = caret_ = position;
			return;
		}
		int column = clamp_column(position.line, position.column);
		if (column >= (int)text.size())
			column = retreat_column(text, (int)text.size());
		auto word_char = [](unsigned char c)
			{
				return std::isalnum(c) || c == '_' || c >= 0x80;
			};
		if (!word_char((unsigned char)text[(std::size_t)column]))
		{
			const int end = advance_column(text, column);
			anchor_ = code_position{ position.line, column };
			caret_ = code_position{ position.line, end };
			++edit_group_;
			return;
		}
		int start = column;
		while (start > 0)
		{
			const int previous = retreat_column(text, start);
			if (!word_char((unsigned char)text[(std::size_t)previous]))
				break;
			start = previous;
		}
		int end = column;
		while (end < (int)text.size() &&
			word_char((unsigned char)text[(std::size_t)end]))
			end = advance_column(text, end);
		anchor_ = code_position{ position.line, start };
		caret_ = code_position{ position.line, end };
		++edit_group_;
	}

	void code_editor::select_line_at(int line)
	{
		if (lines_.empty())
			return;
		line = std::clamp(line, 0, (int)lines_.size() - 1);
		anchor_ = code_position{ line, 0 };
		caret_ = code_position{ line, line_length(line) };
		++edit_group_;
	}

	bool code_editor::selection_range(code_position* start,
		code_position* end) const
	{
		if (anchor_ == caret_)
			return false;
		if (caret_ < anchor_)
		{
			if (start) *start = caret_;
			if (end) *end = anchor_;
		}
		else
		{
			if (start) *start = anchor_;
			if (end) *end = caret_;
		}
		return true;
	}

	/* ------------------------------------------------------------------------- */
	/* clipboard                                                                 */
	/* ------------------------------------------------------------------------- */

	void code_editor::set_clipboard_handlers(clipboard_getter getter,
		clipboard_setter setter)
	{
		clipboard_getter_ = std::move(getter);
		clipboard_setter_ = std::move(setter);
	}

	void code_editor::clear_clipboard_handlers()
	{
		clipboard_getter_ = nullptr;
		clipboard_setter_ = nullptr;
	}

	std::string code_editor::read_clipboard() const
	{
		if (clipboard_getter_)
			return clipboard_getter_();
		if (!clipboard_sink_)
			return std::string();
		vgui_edit_set_text(clipboard_sink_, "");
		if (!vgui_edit_paste(clipboard_sink_))
			return std::string();
		const char* text = vgui_edit_get_text(clipboard_sink_);
		return text ? std::string(text) : std::string();
	}

	void code_editor::write_clipboard(const std::string& text)
	{
		if (clipboard_setter_)
		{
			clipboard_setter_(text);
			return;
		}
		if (!clipboard_sink_)
			return;
		vgui_edit_set_text(clipboard_sink_, text.c_str());
		vgui_edit_select_all(clipboard_sink_);
		(void)vgui_edit_copy(clipboard_sink_);
	}

	bool code_editor::copy()
	{
		const std::string selected = selected_text();
		if (selected.empty())
			return false;
		write_clipboard(selected);
		return true;
	}

	bool code_editor::cut()
	{
		if (read_only_)
			return false;
		code_position start;
		code_position end;
		if (!selection_range(&start, &end))
			return false;
		const std::string selected = text_range(start, end);
		if (selected.empty())
			return false;
		write_clipboard(selected);
		return replace_range(start, end, std::string(), 0, "Cut");
	}

	bool code_editor::paste()
	{
		if (read_only_)
			return false;
		const std::string clip = normalize_newlines(read_clipboard());
		if (clip.empty())
			return false;
		return insert_text(clip);
	}

	/* ------------------------------------------------------------------------- */
	/* find / replace                                                            */
	/* ------------------------------------------------------------------------- */

	void code_editor::set_find_query(std::string text)
	{
		find_query_ = std::move(text);
		active_match_ = -1;
		rebuild_matches();
		if (search_.widget() && search_.text() != find_query_ && !updating_find_)
		{
			updating_find_ = true;
			search_.set_text(find_query_);
			updating_find_ = false;
		}
	}

	const std::string& code_editor::find_query() const noexcept
	{
		return find_query_;
	}

	void code_editor::set_find_case_sensitive(bool sensitive)
	{
		find_case_sensitive_ = sensitive;
		if (case_button_)
			vgui_widget_set_checked(case_button_, sensitive ? 1 : 0);
		active_match_ = -1;
		rebuild_matches();
	}

	bool code_editor::find_case_sensitive() const noexcept
	{
		return find_case_sensitive_;
	}

	void code_editor::toggle_find_case()
	{
		set_find_case_sensitive(!find_case_sensitive_);
		if (!matches_.empty())
			select_match(0);
	}

	void code_editor::open_find(bool replace_mode)
	{
		if (!built())
			return;
		if (find_query_.empty() && has_selection())
		{
			const std::string selected = selected_text();
			if (selected.find('\n') == std::string::npos)
				set_find_query(selected);
		}
		find_replace_mode_ = replace_mode;
		find_open_ = true;
		update_find_visibility();
		rebuild_matches();
		if (replace_mode && replace_edit_)
			vgui_set_focus(context_, replace_edit_);
		else
			search_.focus();
	}

	void code_editor::close_find()
	{
		if (!built())
			return;
		find_open_ = false;
		find_replace_mode_ = false;
		update_find_visibility();
		if (context_ && editor_)
			vgui_set_focus(context_, editor_);
	}

	bool code_editor::find_visible() const noexcept
	{
		return built() && find_open_;
	}

	void code_editor::update_find_visibility()
	{
		if (!find_bar_)
			return;
		const bool visible = find_open_;
		vgui_widget_set_visible(find_bar_, visible ? 1 : 0);
		const int replace_visible = visible && find_replace_mode_ ? 1 : 0;
		if (replace_edit_)
			vgui_widget_set_visible(replace_edit_, replace_visible);
		if (replace_button_)
			vgui_widget_set_visible(replace_button_, replace_visible);
		if (replace_all_button_)
			vgui_widget_set_visible(replace_all_button_, replace_visible);
	}

	bool code_editor::find_next()
	{
		if (find_query_.empty())
			return false;
		rebuild_matches();
		if (matches_.empty())
			return false;

		int index = -1;
		if (active_match_ >= 0 &&
			active_match_ < (int)matches_.size() && match_at_caret())
		{
			index = (active_match_ + 1) % (int)matches_.size();
		}
		else
		{
			code_position from = caret_;
			for (int i = 0; i < (int)matches_.size(); ++i)
			{
				const code_match& match = matches_[(std::size_t)i];
				if (code_position{ match.line, match.column } >= from)
				{
					index = i;
					break;
				}
			}
			if (index < 0)
				index = 0;
		}
		select_match(index);
		return true;
	}

	bool code_editor::find_previous()
	{
		if (find_query_.empty())
			return false;
		rebuild_matches();
		if (matches_.empty())
			return false;

		int index = -1;
		if (active_match_ >= 0 &&
			active_match_ < (int)matches_.size() && match_at_caret())
		{
			index = active_match_ - 1;
			if (index < 0)
				index = (int)matches_.size() - 1;
		}
		else
		{
			const code_position from = caret_;
			for (int i = (int)matches_.size() - 1; i >= 0; --i)
			{
				const code_match& match = matches_[(std::size_t)i];
				if (code_position{ match.line, match.column } < from)
				{
					index = i;
					break;
				}
			}
			if (index < 0)
				index = (int)matches_.size() - 1;
		}
		select_match(index);
		return true;
	}

	std::vector<code_match> code_editor::find_all(const std::string& needle,
		bool case_sensitive) const
	{
		std::vector<code_match> result;
		if (needle.empty())
			return result;
		const std::string lower_needle = case_sensitive ? needle :
			ascii_lower(needle);
		for (int line = 0; line < (int)lines_.size(); ++line)
		{
			const std::string& text = lines_[(std::size_t)line].text;
			const std::string haystack = case_sensitive ? text : ascii_lower(text);
			std::size_t offset = 0;
			while (offset <= haystack.size())
			{
				const std::size_t found = haystack.find(lower_needle, offset);
				if (found == std::string::npos)
					break;
				code_match match;
				match.line = line;
				match.column = (int)found;
				match.length = (int)needle.size();
				result.push_back(match);
				offset = found + std::max<std::size_t>(1, needle.size());
			}
		}
		return result;
	}

	const std::vector<code_match>& code_editor::matches() const noexcept
	{
		return matches_;
	}

	int code_editor::active_match() const noexcept
	{
		return active_match_;
	}

	void code_editor::rebuild_matches()
	{
		int previous = active_match_;
		matches_ = find_all(find_query_, find_case_sensitive_);
		if (matches_.empty())
			active_match_ = -1;
		else if (previous >= 0 && previous < (int)matches_.size())
			active_match_ = previous;
		else
			active_match_ = -1;
	}

	void code_editor::select_match(int index)
	{
		if (index < 0 || index >= (int)matches_.size())
			return;
		const code_match& match = matches_[(std::size_t)index];
		anchor_ = code_position{ match.line, match.column };
		code_position end{ match.line, match.column + match.length };
		end = clamp_position(end);
		caret_ = end;
		active_match_ = index;
		++edit_group_;
		scroll_to_line(match.line);
		ensure_caret_visible();
		update_text_input_rect();
	}

	bool code_editor::match_at_caret() const
	{
		code_position start;
		code_position end;
		if (!selection_range(&start, &end))
			return false;
		for (const code_match& match : matches_)
		{
			if (match.line == start.line && match.column == start.column &&
				match.line == end.line &&
				match.column + match.length == end.column)
				return true;
		}
		return false;
	}

	bool code_editor::replace_match(int index, const std::string& replacement)
	{
		if (read_only_ || index < 0 || index >= (int)matches_.size())
			return false;
		const code_match match = matches_[(std::size_t)index];
		code_position start{ match.line, match.column };
		code_position end = start;
		end.column += match.length;
		end = clamp_position(end);
		if (!replace_range(start, end, replacement, 0, "Replace"))
			return false;
		rebuild_matches();
		if (!matches_.empty())
		{
			int next = -1;
			for (int i = 0; i < (int)matches_.size(); ++i)
			{
				const code_match& candidate = matches_[(std::size_t)i];
				if (code_position{ candidate.line, candidate.column } >= caret_)
				{
					next = i;
					break;
				}
			}
			if (next < 0)
				next = 0;
			select_match(next);
		}
		return true;
	}

	bool code_editor::replace_current(std::string replacement)
	{
		if (read_only_)
			return false;
		rebuild_matches();
		if (matches_.empty())
			return false;

		int index = -1;
		code_position sel_start;
		code_position sel_end;
		if (selection_range(&sel_start, &sel_end))
		{
			for (int i = 0; i < (int)matches_.size(); ++i)
			{
				const code_match& match = matches_[(std::size_t)i];
				if (match.line == sel_start.line &&
					match.column == sel_start.column &&
					match.line == sel_end.line &&
					match.column + match.length == sel_end.column)
				{
					index = i;
					break;
				}
			}
		}
		if (index < 0)
		{
			if (!find_next())
				return false;
			index = active_match_;
		}
		replacement = normalize_newlines(std::move(replacement));
		return replace_match(index, replacement);
	}

	int code_editor::replace_all(std::string needle, std::string replacement,
		bool case_sensitive)
	{
		if (read_only_ || needle.empty())
			return 0;
		replacement = normalize_newlines(std::move(replacement));
		const std::vector<code_match> found = find_all(needle, case_sensitive);
		if (found.empty())
			return 0;

		std::string out;
		std::size_t match_cursor = 0;
		for (int line = 0; line < (int)lines_.size(); ++line)
		{
			if (line)
				out.push_back('\n');
			const std::string& text = lines_[(std::size_t)line].text;
			std::size_t offset = 0;
			while (match_cursor < found.size() &&
				found[match_cursor].line == line)
			{
				const code_match& match = found[match_cursor];
				const std::size_t column = (std::size_t)match.column;
				if (column >= offset && column <= text.size())
				{
					out += text.substr(offset, column - offset);
					out += replacement;
					offset = std::min(text.size(),
						column + (std::size_t)match.length);
				}
				++match_cursor;
			}
			out += text.substr(offset);
		}
		const int count = (int)found.size();
		code_position end{ (int)lines_.size() - 1,
			line_length((int)lines_.size() - 1) };
		replace_range(code_position{}, end, out, 0, "Replace All");
		rebuild_matches();
		if (find_query_ != needle)
			active_match_ = -1;
		return count;
	}

	void code_editor::on_find_changed(const std::string& text, void* user)
	{
		auto* self = static_cast<code_editor*>(user);
		if (!self || self->updating_find_)
			return;
		self->find_query_ = text;
		self->active_match_ = -1;
		self->rebuild_matches();
		if (!self->matches_.empty())
			self->select_match(0);
	}

	void code_editor::on_find_submit(const std::string& text, void* user)
	{
		(void)text;
		auto* self = static_cast<code_editor*>(user);
		if (self)
			(void)self->find_next();
	}

	void code_editor::on_replace_event(vgui_widget* widget,
		const vgui_event* event, void* user)
	{
		(void)widget;
		auto* self = static_cast<code_editor*>(user);
		if (!self || !event || event->type != VGUI_EVENT_KEY_DOWN)
			return;
		if (event->key == VGUI_KEY_ENTER)
		{
			if (self->replace_edit_)
			{
				const char* text = vgui_edit_get_text(self->replace_edit_);
				(void)self->replace_current(text ? text : "");
			}
		}
		else if (event->key == VGUI_KEY_ESCAPE)
		{
			self->close_find();
		}
	}

	void code_editor::on_button_event(vgui_widget* widget,
		const vgui_event* event, void* user)
	{
		auto* self = static_cast<code_editor*>(user);
		if (!self || !event || event->type != VGUI_EVENT_CLICK)
			return;
		if (widget == self->prev_button_)
			(void)self->find_previous();
		else if (widget == self->next_button_)
			(void)self->find_next();
		else if (widget == self->case_button_)
			self->toggle_find_case();
		else if (widget == self->close_find_button_)
			self->close_find();
		else if (widget == self->replace_button_)
		{
			const char* text = self->replace_edit_ ?
				vgui_edit_get_text(self->replace_edit_) : nullptr;
			(void)self->replace_current(text ? text : "");
		}
		else if (widget == self->replace_all_button_)
		{
			const char* text = self->replace_edit_ ?
				vgui_edit_get_text(self->replace_edit_) : nullptr;
			(void)self->replace_all(self->find_query_, text ? text : "",
				self->find_case_sensitive_);
		}
	}

	/* ------------------------------------------------------------------------- */
	/* rich log                                                                  */
	/* ------------------------------------------------------------------------- */

	void code_editor::append_log(code_log_level level, std::string text,
		std::string timestamp)
	{
		if (!valid_log_level((int)level))
			level = code_log_level::info;
		text = normalize_newlines(std::move(text));

		std::vector<std::string> parts;
		std::size_t begin = 0;
		while (true)
		{
			const std::size_t newline = text.find('\n', begin);
			if (newline == std::string::npos)
			{
				parts.push_back(text.substr(begin));
				break;
			}
			parts.push_back(text.substr(begin, newline - begin));
			begin = newline + 1;
			if (begin == text.size())
			{
				parts.push_back(std::string());
				break;
			}
		}
		if (parts.empty())
			parts.push_back(std::string());

		/* A freshly cleared editor has a single empty placeholder line.  It is
		 * not part of the log, so drop it before appending the first record. */
		if (lines_.size() == 1 && lines_.front().text.empty() &&
			!lines_.front().manual_spans)
		{
			lines_.clear();
			highlight_valid_ = 0;
		}

		std::string prefix;
		if (!timestamp.empty())
		{
			prefix = timestamp;
			prefix.push_back(' ');
		}
		for (std::size_t part = 0; part < parts.size(); ++part)
		{
			line_record record;
			record.manual_spans = true;
			const std::string& body = parts[part];
			const std::size_t message_start = part == 0 ? prefix.size() : 0;
			record.text = part == 0 ? prefix + body : body;
			if (message_start > 0)
			{
				append_span(record.spans, 0, (int)message_start,
					code_token_kind::comment);
				if (!record.spans.empty())
					record.spans.back().color = with_alpha(
						log_colors_[(int)code_log_level::trace], 0xC0);
			}
			append_span(record.spans, (int)message_start,
				(int)record.text.size(), code_token_kind::text);
			if (!record.spans.empty())
				record.spans.back().color = log_colors_[(int)level];
			lines_.push_back(std::move(record));
		}

		if (max_lines_ > 0 && lines_.size() > max_lines_)
		{
			const std::size_t drop = lines_.size() - max_lines_;
			lines_.erase(lines_.begin(), lines_.begin() + (std::ptrdiff_t)drop);
			dropped_lines_ += drop;
			if (highlight_valid_ > drop)
				highlight_valid_ -= drop;
			else
				highlight_valid_ = 0;
			caret_.line -= (int)drop;
			anchor_.line -= (int)drop;
			caret_ = clamp_position(caret_);
			anchor_ = clamp_position(anchor_);
		}
		for (const line_record& line : lines_)
			max_line_bytes_ = std::max(max_line_bytes_, line.text.size());
		if (auto_scroll_)
			scroll_to_line((int)lines_.size() - 1);
		update_text_input_rect();
		notify_change();
	}

	void code_editor::set_log_level_color(code_log_level level,
		vgui_color color)
	{
		const int index = static_cast<int>(level);
		if (valid_log_level(index))
		{
			log_colors_[index] = color;
			if (!lines_.empty() && !read_only_)
				highlight_valid_ = 0; /* manual spans keep their stored colours */
		}
	}

	vgui_color code_editor::log_level_color(code_log_level level) const noexcept
	{
		const int index = static_cast<int>(level);
		if (!valid_log_level(index))
			return kDefaultLogColors[0];
		return log_colors_[index] ? log_colors_[index] : kDefaultLogColors[index];
	}

	void code_editor::set_auto_scroll(bool enabled)
	{
		auto_scroll_ = enabled;
		if (auto_scroll_ && !lines_.empty())
			scroll_to_line((int)lines_.size() - 1);
	}

	bool code_editor::auto_scroll() const noexcept
	{
		return auto_scroll_;
	}

	void code_editor::set_max_lines(std::size_t max_lines)
	{
		max_lines_ = max_lines;
		if (max_lines_ > 0 && lines_.size() > max_lines_)
		{
			const std::size_t drop = lines_.size() - max_lines_;
			lines_.erase(lines_.begin(), lines_.begin() + (std::ptrdiff_t)drop);
			dropped_lines_ += drop;
			if (highlight_valid_ > drop)
				highlight_valid_ -= drop;
			else
				highlight_valid_ = 0;
			caret_.line -= (int)drop;
			anchor_.line -= (int)drop;
			caret_ = clamp_position(caret_);
			anchor_ = clamp_position(anchor_);
			notify_change();
		}
	}

	std::size_t code_editor::max_lines() const noexcept
	{
		return max_lines_;
	}

	std::size_t code_editor::dropped_lines() const noexcept
	{
		return dropped_lines_;
	}

	/* ------------------------------------------------------------------------- */
	/* incremental file loading                                                  */
	/* ------------------------------------------------------------------------- */

	bool code_editor::open_file(const std::string& path, std::string* error)
	{
		std::ifstream stream(path, std::ios::binary);
		if (!stream)
		{
			if (error)
				*error = "cannot open file: " + path;
			return false;
		}
		stream.seekg(0, std::ios::end);
		const std::streamoff size = stream.tellg();
		stream.seekg(0, std::ios::beg);
		if (!stream)
		{
			if (error)
				*error = "cannot read file: " + path;
			return false;
		}

		load_stream_ = std::move(stream);
		load_memory_.clear();
		load_memory_offset_ = 0;
		load_pending_.clear();
		load_total_bytes_ = size > 0 ? (std::size_t)size : 0;
		load_read_bytes_ = 0;
		load_from_memory_ = false;

		set_document_raw(std::string());
		caret_ = code_position{};
		anchor_ = code_position{};
		scroll_x_ = 0.0f;
		scroll_y_ = 0.0f;
		local_history_.clear();
		local_history_index_ = 0;
		if (command_stack_)
			command_stack_->clear();
		matches_.clear();
		active_match_ = -1;
		set_language_from_path(path);
		loading_ = true;
		notify_change();
		return true;
	}

	void code_editor::begin_incremental_load(std::string text)
	{
		load_stream_.close();
		load_memory_ = std::move(text);
		load_memory_offset_ = 0;
		load_pending_.clear();
		load_total_bytes_ = load_memory_.size();
		load_read_bytes_ = 0;
		load_from_memory_ = true;

		set_document_raw(std::string());
		caret_ = code_position{};
		anchor_ = code_position{};
		scroll_x_ = 0.0f;
		scroll_y_ = 0.0f;
		local_history_.clear();
		local_history_index_ = 0;
		if (command_stack_)
			command_stack_->clear();
		matches_.clear();
		active_match_ = -1;
		loading_ = true;
		notify_change();
	}

	bool code_editor::loading() const noexcept
	{
		return loading_;
	}

	float code_editor::load_progress() const noexcept
	{
		if (!loading_)
			return 1.0f;
		if (load_total_bytes_ == 0)
			return 0.0f;
		const float progress = (float)load_read_bytes_ /
			(float)load_total_bytes_;
		return std::clamp(progress, 0.0f, 1.0f);
	}

	void code_editor::set_load_chunk_size(std::size_t bytes)
	{
		load_chunk_size_ = std::clamp<std::size_t>(bytes, 1,
			16 * 1024 * 1024);
	}

	std::size_t code_editor::load_chunk_size() const noexcept
	{
		return load_chunk_size_;
	}

	bool code_editor::process_load_step()
	{
		if (!loading_)
			return false;

		std::string chunk;
		bool eof = false;
		if (load_from_memory_)
		{
			if (load_memory_offset_ < load_memory_.size())
			{
				const std::size_t count = std::min(load_chunk_size_,
					load_memory_.size() - load_memory_offset_);
				chunk = load_memory_.substr(load_memory_offset_, count);
				load_memory_offset_ += count;
				load_read_bytes_ = load_memory_offset_;
			}
			eof = load_memory_offset_ >= load_memory_.size();
		}
		else
		{
			if (load_stream_)
			{
				chunk.resize(load_chunk_size_);
				load_stream_.read(&chunk[0],
					(std::streamsize)load_chunk_size_);
				const std::streamsize got = load_stream_.gcount();
				chunk.resize(got > 0 ? (std::size_t)got : 0);
				load_read_bytes_ += chunk.size();
			}
			eof = !load_stream_ ||
				load_read_bytes_ >= load_total_bytes_;
			if (eof && load_read_bytes_ < load_total_bytes_)
				load_read_bytes_ = load_total_bytes_;
		}

		load_pending_ += chunk;
		if (eof)
		{
			if (!load_pending_.empty())
			{
				const std::string tail = normalize_newlines(load_pending_);
				load_pending_.clear();
				append_text(tail);
			}
			loading_ = false;
			load_stream_.close();
			if (auto_scroll_ && !lines_.empty())
				scroll_to_line((int)lines_.size() - 1);
			return false;
		}

		const std::size_t last_newline = load_pending_.find_last_of('\n');
		if (last_newline != std::string::npos)
		{
			std::string complete = load_pending_.substr(0, last_newline + 1);
			load_pending_.erase(0, last_newline + 1);
			complete = normalize_newlines(complete);
			append_text(complete);
		}
		return loading_;
	}

	void code_editor::cancel_load()
	{
		loading_ = false;
		load_stream_.close();
		load_memory_.clear();
		load_pending_.clear();
		load_memory_offset_ = 0;
		load_read_bytes_ = 0;
		load_total_bytes_ = 0;
	}

	void code_editor::tick(float delta_time)
	{
		if (delta_time > 0.0f && std::isfinite(delta_time))
			tick_time_ += delta_time;
		if (loading_)
			(void)process_load_step();
		if (auto_scroll_ && !loading_ && read_only_ && !lines_.empty())
			scroll_to_line((int)lines_.size() - 1);
	}

	/* ------------------------------------------------------------------------- */
	/* scrolling                                                                 */
	/* ------------------------------------------------------------------------- */

	void code_editor::set_scroll(float x, float y)
	{
		scroll_x_ = std::isfinite(x) ? x : 0.0f;
		scroll_y_ = std::isfinite(y) ? y : 0.0f;
		clamp_scroll();
		update_text_input_rect();
	}

	float code_editor::scroll_x() const noexcept
	{
		return scroll_x_;
	}

	float code_editor::scroll_y() const noexcept
	{
		return scroll_y_;
	}

	void code_editor::scroll_to_caret()
	{
		scroll_to_line(caret_.line);
		ensure_caret_visible();
	}

	void code_editor::scroll_to_line(int line)
	{
		geometry g = editor_geometry();
		if (g.line_height <= 0.0f)
			return;
		float top = (float)line * g.line_height;
		float bottom = top + g.line_height;
		if (top < scroll_y_)
			scroll_y_ = top;
		else if (bottom > scroll_y_ + g.text_height)
			scroll_y_ = bottom - g.text_height;
		clamp_scroll();
	}

	void code_editor::ensure_caret_visible()
	{
		if (!editor_)
			return;
		geometry g = editor_geometry();
		if (g.line_height <= 0.0f || g.text_width <= 0.0f)
			return;
		const float top = (float)caret_.line * g.line_height;
		const float bottom = top + g.line_height;
		if (top < scroll_y_)
			scroll_y_ = top;
		else if (bottom > scroll_y_ + g.text_height)
			scroll_y_ = bottom - g.text_height;

		const float x = x_for_column(caret_.line, caret_.column);
		const float margin = 2.0f * ui_scale();
		if (x < scroll_x_)
			scroll_x_ = x;
		else if (x > scroll_x_ + g.text_width - margin)
			scroll_x_ = x - g.text_width + margin;
		clamp_scroll();
		update_text_input_rect();
	}

	void code_editor::clamp_scroll()
	{
		geometry g = editor_geometry();
		const float content_height = (float)lines_.size() * g.line_height;
		const float content = content_width();
		const float max_y = std::max(0.0f, content_height - g.text_height);
		const float max_x = std::max(0.0f, content - g.text_width);
		scroll_y_ = std::clamp(scroll_y_, 0.0f, max_y);
		scroll_x_ = std::clamp(scroll_x_, 0.0f, max_x);
	}

	/* ------------------------------------------------------------------------- */
	/* geometry                                                                  */
	/* ------------------------------------------------------------------------- */

	float code_editor::ui_scale() const
	{
		return ui_scale_of(editor_);
	}

	code_editor::geometry code_editor::editor_geometry() const
	{
		geometry g;
		if (!editor_)
			return g;
		const float scale = ui_scale();
		vgui_widget_get_rect(editor_, &g.x, &g.y, &g.width, &g.height);
		g.width = std::max(1.0f, g.width);
		g.height = std::max(1.0f, g.height);
		g.line_height = vgui_widget_get_line_height(editor_);
		if (g.line_height <= 0.0f)
			g.line_height = std::max(10.0f * scale,
				font_size_ * scale * 1.35f);

		int digits = 1;
		if (!lines_.empty())
		{
			int count = (int)lines_.size();
			while (count >= 10)
			{
				++digits;
				count /= 10;
			}
		}
		if (show_line_numbers_)
		{
			const float digit_width = vgui_widget_measure_text(editor_, "0");
			g.gutter = (digit_width > 0.0f ? digit_width :
				font_size_ * scale * 0.6f) * (float)digits +
				14.0f * scale;
		}
		else
		{
			g.gutter = 6.0f * scale;
		}
		g.gutter = std::min(g.gutter, g.width * 0.4f);

		const float bar = 10.0f * scale;
		const float content_height = (float)lines_.size() * g.line_height;
		bool vertical = content_height > g.height;
		float text_width = g.width - g.gutter - (vertical ? bar : 0.0f);
		bool horizontal = content_width() > text_width;
		float text_height = g.height - (horizontal ? bar : 0.0f);
		vertical = content_height > text_height;
		text_width = g.width - g.gutter - (vertical ? bar : 0.0f);
		horizontal = content_width() > text_width;
		text_height = g.height - (horizontal ? bar : 0.0f);

		g.text_x = g.x + g.gutter;
		g.text_width = std::max(1.0f, text_width);
		g.text_height = std::max(1.0f, text_height);
		g.vertical_bar = vertical ? bar : 0.0f;
		g.horizontal_bar = horizontal ? bar : 0.0f;
		g.vertical_scroll = vertical;
		g.horizontal_scroll = horizontal;
		return g;
	}

	float code_editor::content_width() const
	{
		if (!editor_)
			return 0.0f;
		float width = 0.0f;
		if (lines_.size() <= 1200)
		{
			for (int line = 0; line < (int)lines_.size(); ++line)
				width = std::max(width,
					x_for_column(line, line_length(line)));
			return width;
		}
		const float character = vgui_widget_measure_text(editor_, "M");
		const float unit = character > 0.0f ? character :
			font_size_ * ui_scale() * 0.6f;
		return (float)max_line_bytes_ * unit;
	}

	std::string code_editor::display_prefix(int line, int column) const
	{
		return display_segment(line, 0, column);
	}

	std::string code_editor::display_segment(int line, int start_column,
		int end_column) const
	{
		if (line < 0 || (std::size_t)line >= lines_.size())
			return std::string();
		const std::string& text = lines_[(std::size_t)line].text;
		start_column = clamp_column(line, start_column);
		end_column = clamp_column(line, end_column);
		if (end_column < start_column)
			std::swap(start_column, end_column);
		if (start_column == end_column)
			return std::string();

		std::string out;
		out.reserve((std::size_t)(end_column - start_column));
		int column = 0;
		std::size_t i = 0;
		while (i < text.size() && (int)i < end_column)
		{
			const char c = text[i];
			if (c == '\t')
			{
				const int next = tab_advance(column, tab_size_);
				if ((int)i >= start_column)
				{
					for (int space = column; space < next; ++space)
						out.push_back(' ');
				}
				column = next;
				++i;
				continue;
			}
			const std::size_t length = utf8_char_len((unsigned char)c);
			if ((int)i >= start_column)
				out.append(text, i, length);
			i += length;
			++column;
		}
		return out;
	}

	float code_editor::x_for_column(int line, int column) const
	{
		if (!editor_)
			return 0.0f;
		const std::string prefix = display_prefix(line, column);
		if (prefix.empty())
			return 0.0f;
		return vgui_widget_measure_text(editor_, prefix.c_str());
	}

	int code_editor::column_at_x(int line, float x) const
	{
		if (line < 0 || (std::size_t)line >= lines_.size())
			return 0;
		const std::string& text = lines_[(std::size_t)line].text;
		if (x <= 0.0f || text.empty())
			return 0;
		if (!editor_)
			return 0;

		std::string expanded;
		int column = 0;
		float last_x = 0.0f;
		std::size_t i = 0;
		while (i < text.size())
		{
			const char c = text[i];
			std::string next = expanded;
			std::size_t next_i = i + 1;
			int next_column = column + 1;
			if (c == '\t')
			{
				const int stop = tab_advance(column, tab_size_);
				for (int space = column; space < stop; ++space)
					next.push_back(' ');
				next_column = stop;
			}
			else
			{
				const std::size_t length = utf8_char_len((unsigned char)c);
				next.append(text, i, length);
				next_i = i + length;
				next_column = column + 1;
			}
			const float next_x = vgui_widget_measure_text(editor_, next.c_str());
			if (next_x >= x)
			{
				if (x - last_x <= next_x - x)
					return (int)i;
				return (int)next_i;
			}
			expanded.swap(next);
			last_x = next_x;
			column = next_column;
			i = next_i;
		}
		return (int)text.size();
	}

	void code_editor::update_text_input_rect()
	{
		if (!editor_)
			return;
		const geometry g = editor_geometry();
		const float x = std::clamp(g.gutter +
			x_for_column(caret_.line, caret_.column) - scroll_x_,
			0.0f, std::max(0.0f, g.text_width - 2.0f));
		const float y = std::clamp((float)caret_.line * g.line_height -
			scroll_y_, 0.0f, std::max(0.0f, g.text_height - g.line_height));
		vgui_widget_set_text_input_rect(editor_, x, y, 2.0f * ui_scale(),
			g.line_height);
	}

	/* ------------------------------------------------------------------------- */
	/* events                                                                    */
	/* ------------------------------------------------------------------------- */

	int code_editor::on_event(vgui_widget* widget, const vgui_event* event,
		void* user)
	{
		(void)widget;
		auto* self = static_cast<code_editor*>(user);
		if (!self || !event)
			return 0;
		const int type = event->type;

		/* The scrollbars share the editor surface, so the I-beam cursor must be
		 * swapped for the normal arrow while the pointer is over them. */
		const auto over_scrollbar = [](const geometry& g, float x, float y)
			{
				if (g.vertical_scroll && g.vertical_bar > 0.0f &&
					x >= g.width - g.vertical_bar && y < g.text_height)
					return true;
				if (g.horizontal_scroll && g.horizontal_bar > 0.0f &&
					y >= g.height - g.horizontal_bar)
					return true;
				return false;
			};
		const auto update_cursor = [self, &over_scrollbar](float x, float y)
			{
				vgui_widget_set_cursor(self->editor_,
					over_scrollbar(self->editor_geometry(), x, y) ?
					VGUI_CURSOR_ARROW : VGUI_CURSOR_IBEAM);
			};

		if (type == VGUI_EVENT_MOUSE_WHEEL)
		{
			const geometry g = self->editor_geometry();
			if (event->wheel_y > 0.0f)
				self->auto_scroll_ = false;
			if (event->wheel_x != 0.0f ||
				(event->mods & VGUI_MOD_SHIFT) != 0)
			{
				const float delta = (event->wheel_x != 0.0f ?
					event->wheel_x : event->wheel_y) * g.line_height * 3.0f;
				self->set_scroll(self->scroll_x_ - delta, self->scroll_y_);
			}
			else
			{
				self->set_scroll(self->scroll_x_, self->scroll_y_ -
					event->wheel_y * g.line_height * 3.0f);
			}
			return 1;
		}

		if (type == VGUI_EVENT_DOUBLE_CLICK || type == VGUI_EVENT_TRIPLE_CLICK)
		{
			if (event->button != 0 && event->button != 1)
				return 0;
			if (self->context_)
				vgui_set_focus(self->context_, self->editor_);
			const geometry g = self->editor_geometry();
			int line = (int)std::floor((event->y + self->scroll_y_) /
				std::max(1.0f, g.line_height));
			line = std::clamp(line, 0, (int)self->lines_.size() - 1);
			if (type == VGUI_EVENT_DOUBLE_CLICK)
			{
				const float x = event->x - g.gutter + self->scroll_x_;
				const int column = self->column_at_x(line, x);
				self->select_word_at(code_position{ line, column });
				self->click_mode_ = 2;
			}
			else
			{
				self->select_line_at(line);
				self->click_mode_ = 3;
			}
			self->ensure_caret_visible();
			self->update_text_input_rect();
			return 1;
		}

		if (type == VGUI_EVENT_MOUSE_DOWN)
		{
			if (event->button != 1)
				return 0;
			if (self->context_)
				vgui_set_focus(self->context_, self->editor_);
			const geometry g = self->editor_geometry();
			const float local_x = event->x;
			const float local_y = event->y;

			if (g.vertical_scroll && local_x >= g.width - g.vertical_bar &&
				local_y < g.text_height)
			{
				vgui_widget_set_cursor(self->editor_, VGUI_CURSOR_ARROW);
				self->drag_mode_ = 2;
				const float content = (float)self->lines_.size() * g.line_height;
				const float max_y = std::max(1.0f,
					content - g.text_height);
				self->set_scroll(self->scroll_x_,
					std::clamp(local_y / std::max(1.0f, g.text_height),
						0.0f, 1.0f) * max_y);
				return 1;
			}
			if (g.horizontal_scroll && local_y >= g.height - g.horizontal_bar)
			{
				vgui_widget_set_cursor(self->editor_, VGUI_CURSOR_ARROW);
				self->drag_mode_ = 3;
				const float max_x = std::max(1.0f,
					self->content_width() - g.text_width);
				self->set_scroll(std::clamp(local_x /
					std::max(1.0f, g.text_width), 0.0f, 1.0f) * max_x,
					self->scroll_y_);
				return 1;
			}

			int line = (int)std::floor((local_y + self->scroll_y_) /
				std::max(1.0f, g.line_height));
			line = std::clamp(line, 0, (int)self->lines_.size() - 1);
			float text_x = local_x - g.gutter + self->scroll_x_;
			if (local_x < g.gutter)
				text_x = 0.0f;
			const int column = self->column_at_x(line, text_x);
			const bool shift = (event->mods & VGUI_MOD_SHIFT) != 0;
			self->click_mode_ = 1;
			self->drag_mode_ = 1;
			self->move_caret(code_position{ line, column }, shift);
			return 1;
		}

		if (type == VGUI_EVENT_MOUSE_MOVE)
		{
			if (self->drag_mode_ == 0)
			{
				update_cursor(event->x, event->y);
				return 0;
			}
			const geometry g = self->editor_geometry();
			if (self->drag_mode_ == 1)
			{
				int line = (int)std::floor((event->y + self->scroll_y_) /
					std::max(1.0f, g.line_height));
				line = std::clamp(line, 0, (int)self->lines_.size() - 1);
				const float text_x = event->x - g.gutter + self->scroll_x_;
				const int column = self->column_at_x(line, text_x);
				self->move_caret(code_position{ line, column }, true);
			}
			else if (self->drag_mode_ == 2)
			{
				const float content = (float)self->lines_.size() * g.line_height;
				const float max_y = std::max(1.0f, content - g.text_height);
				self->set_scroll(self->scroll_x_,
					std::clamp(event->y / std::max(1.0f, g.text_height),
						0.0f, 1.0f) * max_y);
			}
			else if (self->drag_mode_ == 3)
			{
				const float max_x = std::max(1.0f,
					self->content_width() - g.text_width);
				self->set_scroll(std::clamp(event->x /
					std::max(1.0f, g.text_width), 0.0f, 1.0f) * max_x,
					self->scroll_y_);
			}
			return 1;
		}

		if (type == VGUI_EVENT_MOUSE_UP)
		{
			if (self->drag_mode_ != 0)
			{
				self->drag_mode_ = 0;
				update_cursor(event->x, event->y);
				return 1;
			}
			return 0;
		}

		if (type == VGUI_EVENT_TEXT)
		{
			if (event->text && *event->text)
			{
				(void)self->insert_text(event->text);
				self->ensure_caret_visible();
			}
			return 1;
		}

		if (type != VGUI_EVENT_KEY_DOWN)
			return 0;

		const bool ctrl = (event->mods & (VGUI_MOD_CTRL | VGUI_MOD_GUI)) != 0;
		const bool shift = (event->mods & VGUI_MOD_SHIFT) != 0;
		const int key = event->key;

		if (ctrl)
		{
			switch (key)
			{
			case 'a': case 'A':
				self->select_all();
				self->ensure_caret_visible();
				return 1;
			case 'c': case 'C':
				(void)self->copy();
				return 1;
			case 'x': case 'X':
				(void)self->cut();
				return 1;
			case 'v': case 'V':
				(void)self->paste();
				return 1;
			case 'z': case 'Z':
				self->edit_group_++;
				if (shift)
					(void)self->redo();
				else
					(void)self->undo();
				return 1;
			case 'y': case 'Y':
				self->edit_group_++;
				(void)self->redo();
				return 1;
			case 'f': case 'F':
				self->open_find(false);
				return 1;
			case 'h': case 'H':
				self->open_find(true);
				return 1;
			case VGUI_KEY_HOME:
				self->move_caret(code_position{ 0, 0 }, shift);
				return 1;
			case VGUI_KEY_END:
				self->move_caret(code_position{ (int)self->lines_.size() - 1,
					self->line_length((int)self->lines_.size() - 1) }, shift);
				return 1;
			default:
				break;
			}
		}

		switch (key)
		{
		case VGUI_KEY_LEFT:
		{
			code_position pos = self->caret_;
			if (!shift && self->has_selection())
			{
				code_position start;
				code_position end;
				self->selection_range(&start, &end);
				pos = start;
			}
			else if (pos.column > 0)
			{
				pos.column = retreat_column(
					self->lines_[(std::size_t)pos.line].text, pos.column);
			}
			else if (pos.line > 0)
			{
				--pos.line;
				pos.column = self->line_length(pos.line);
			}
			self->move_caret(pos, shift);
			return 1;
		}
		case VGUI_KEY_RIGHT:
		{
			code_position pos = self->caret_;
			if (!shift && self->has_selection())
			{
				code_position start;
				code_position end;
				self->selection_range(&start, &end);
				pos = end;
			}
			else if (pos.column < self->line_length(pos.line))
			{
				pos.column = advance_column(
					self->lines_[(std::size_t)pos.line].text, pos.column);
			}
			else if (pos.line + 1 < (int)self->lines_.size())
			{
				++pos.line;
				pos.column = 0;
			}
			self->move_caret(pos, shift);
			return 1;
		}
		case VGUI_KEY_UP:
		case VGUI_KEY_DOWN:
		{
			const geometry g = self->editor_geometry();
			int line = self->caret_.line + (key == VGUI_KEY_UP ? -1 : 1);
			line = std::clamp(line, 0, (int)self->lines_.size() - 1);
			const float x = self->x_for_column(self->caret_.line,
				self->caret_.column);
			const int column = self->column_at_x(line, x);
			(void)g;
			self->move_caret(code_position{ line, column }, shift);
			return 1;
		}
		case VGUI_KEY_HOME:
			self->move_caret(code_position{ self->caret_.line, 0 }, shift);
			return 1;
		case VGUI_KEY_END:
			self->move_caret(code_position{ self->caret_.line,
				self->line_length(self->caret_.line) }, shift);
			return 1;
		case VGUI_KEY_PAGE_UP:
		case VGUI_KEY_PAGE_DOWN:
		{
			const geometry g = self->editor_geometry();
			const int rows = std::max(1,
				(int)std::floor(g.text_height / g.line_height) - 1);
			int line = self->caret_.line +
				(key == VGUI_KEY_PAGE_UP ? -rows : rows);
			line = std::clamp(line, 0, (int)self->lines_.size() - 1);
			const float x = self->x_for_column(self->caret_.line,
				self->caret_.column);
			const int column = self->column_at_x(line, x);
			self->move_caret(code_position{ line, column }, shift);
			return 1;
		}
		case VGUI_KEY_BACKSPACE:
			return (void)self->backspace(), 1;
		case VGUI_KEY_DELETE:
			return (void)self->delete_forward(), 1;
		case VGUI_KEY_ENTER:
			return (void)self->newline(), 1;
		case VGUI_KEY_TAB:
			if (shift)
				(void)self->outdent_selection();
			else
				(void)self->indent_selection();
			return 1;
		case VGUI_KEY_ESCAPE:
			if (self->find_open_)
				self->close_find();
			else
				self->clear_selection();
			return 1;
		default:
			break;
		}
		return 0;
	}

	/* ------------------------------------------------------------------------- */
	/* drawing                                                                   */
	/* ------------------------------------------------------------------------- */

	void code_editor::on_draw(vgui_widget* widget, ovg_ctx_cb* cb,
		rvg_t* vg, float abs_x, float abs_y, float abs_w, float abs_h,
		float delta_time, void* user)
	{
		(void)widget;
		(void)delta_time;
		auto* self = static_cast<code_editor*>(user);
		if (!self || !self->editor_ || !cb || !vg)
			return;

		const vgui_theme theme = theme_for(self->editor_);
		geometry g = self->editor_geometry();
		g.x = abs_x;
		g.y = abs_y;
		g.width = std::max(1.0f, abs_w);
		g.height = std::max(1.0f, abs_h);

		ovg_tools::fill_rect(cb, vg, g.x, g.y, g.width, g.height,
			to_ovg(theme.edit_bg));

		if (self->lines_.empty())
			return;

		const float line_height = g.line_height;
		const int first_line = std::max(0,
			(int)std::floor(self->scroll_y_ / std::max(1.0f, line_height)));
		const int last_line = std::min((int)self->lines_.size() - 1,
			(int)std::ceil((self->scroll_y_ + g.text_height) /
				std::max(1.0f, line_height)));

		/* Current-line band. */
		if (self->focused() && !self->read_only_)
		{
			const float y = g.y + (float)self->caret_.line * line_height -
				self->scroll_y_;
			if (y + line_height > g.y && y < g.y + g.text_height)
			{
				ovg_tools::fill_rect(cb, vg, g.text_x, y, g.text_width,
					line_height, to_ovg(with_alpha(theme.accent, 0x0C)));
			}
		}

		/* Selection bands. */
		code_position sel_start;
		code_position sel_end;
		if (self->selection_range(&sel_start, &sel_end))
		{
			const vgui_color selection = with_alpha(theme.edit_selection, 0x60);
			for (int line = first_line; line <= last_line; ++line)
			{
				if (line < sel_start.line || line > sel_end.line)
					continue;
				int start_column = line == sel_start.line ?
					sel_start.column : 0;
				int end_column = line == sel_end.line ?
					sel_end.column : self->line_length(line);
				if (line == sel_end.line && end_column == 0 &&
					line > sel_start.line)
					continue;
				if (end_column <= start_column)
					continue;
				const float x0 = g.text_x +
					self->x_for_column(line, start_column) - self->scroll_x_;
				const float x1 = g.text_x +
					self->x_for_column(line, end_column) - self->scroll_x_;
				const float left = std::max(x0, g.text_x);
				const float right = std::min(x1, g.text_x + g.text_width);
				if (right <= left)
					continue;
				const float y = g.y + (float)line * line_height -
					self->scroll_y_;
				ovg_tools::fill_rect(cb, vg, left, y, right - left,
					line_height, to_ovg(selection));
			}
		}

		/* Text runs.  Gutter and scrollbars are painted afterwards so text
		 * scrolled under them is covered. */
		for (int line = first_line; line <= last_line; ++line)
		{
			const float y = g.y + (float)line * line_height - self->scroll_y_;
			const std::string& source = self->lines_[(std::size_t)line].text;
			const int length = (int)source.size();
			const std::vector<code_span>& spans = self->line_spans(line);

			auto draw_run = [&](int start, int end, vgui_color color)
				{
					if (end <= start)
						return;
					const std::string text = self->display_segment(line, start, end);
					if (text.empty())
						return;
					const float x = g.text_x +
						self->x_for_column(line, start) - self->scroll_x_;
					if (x >= g.text_x + g.text_width)
						return;
					const float width = std::max(1.0f,
						g.text_x + g.text_width - x);
					vgui_custom_draw_text(self->editor_, cb, vg, text.c_str(),
						x, y, width, line_height, color, 0.0f, 0.0f);
				};

			int cursor = 0;
			for (const code_span& span : spans)
			{
				const int start = std::clamp(span.start, 0, length);
				const int end = std::clamp(span.start + span.length, start,
					length);
				if (start > cursor)
					draw_run(cursor, start, theme.text);
				vgui_color color = theme.text;
				if (span.color)
					color = span.color;
				else if (span.kind != code_token_kind::text)
					color = self->token_color(span.kind);
				draw_run(start, end, color);
				cursor = std::max(cursor, end);
			}
			if (cursor < length)
				draw_run(cursor, length, theme.text);
		}

		/* Gutter overlay and line numbers. */
		if (g.gutter > 0.0f)
		{
			ovg_tools::fill_rect(cb, vg, g.x, g.y, g.gutter, g.height,
				to_ovg(with_alpha(theme.text, 0x0A)));
			ovg_tools::fill_rect(cb, vg, g.text_x - 1.0f, g.y, 1.0f,
				g.height, to_ovg(with_alpha(theme.panel_border, 0xB0)));
			if (self->show_line_numbers_)
			{
				for (int line = first_line; line <= last_line; ++line)
				{
					const std::string number = std::to_string(line + 1);
					const vgui_color color = line == self->caret_.line &&
						self->focused() ? theme.text : theme.disabled;
					const float y = g.y + (float)line * line_height -
						self->scroll_y_;
					vgui_custom_draw_text(self->editor_, cb, vg,
						number.c_str(), g.x, y, g.gutter - 7.0f,
						line_height, color, 1.0f, 0.0f);
				}
			}
		}

		/* Scrollbars. */
		if (g.vertical_scroll && g.vertical_bar > 0.0f)
		{
			const float track_x = g.x + g.width - g.vertical_bar;
			ovg_tools::fill_rect(cb, vg, track_x, g.y, g.vertical_bar,
				g.text_height, to_ovg(with_alpha(theme.panel_bg, 0xB0)));
			const float content = (float)self->lines_.size() * line_height;
			const float thumb = std::max(20.0f, g.text_height * g.text_height /
				std::max(1.0f, content));
			const float max_scroll = std::max(0.0f, content - g.text_height);
			const float ratio = max_scroll > 0.0f ?
				std::clamp(self->scroll_y_ / max_scroll, 0.0f, 1.0f) : 0.0f;
			const float thumb_y = g.y + ratio *
				std::max(0.0f, g.text_height - thumb);
			ovg_tools::fill_rect(cb, vg, track_x + 2.0f, thumb_y,
				std::max(1.0f, g.vertical_bar - 4.0f), thumb,
				to_ovg(theme.scrollbar));
		}
		if (g.horizontal_scroll && g.horizontal_bar > 0.0f)
		{
			const float track_y = g.y + g.height - g.horizontal_bar;
			ovg_tools::fill_rect(cb, vg, g.text_x, track_y, g.text_width,
				g.horizontal_bar, to_ovg(with_alpha(theme.panel_bg, 0xB0)));
			const float content = self->content_width();
			const float thumb = std::max(20.0f, g.text_width * g.text_width /
				std::max(1.0f, content));
			const float max_scroll = std::max(0.0f, content - g.text_width);
			const float ratio = max_scroll > 0.0f ?
				std::clamp(self->scroll_x_ / max_scroll, 0.0f, 1.0f) : 0.0f;
			const float thumb_x = g.text_x + ratio *
				std::max(0.0f, g.text_width - thumb);
			ovg_tools::fill_rect(cb, vg, thumb_x, track_y + 2.0f, thumb,
				std::max(1.0f, g.horizontal_bar - 4.0f),
				to_ovg(theme.scrollbar));
		}
		/* Bottom-right corner where the two bars meet. */
		if (g.vertical_bar > 0.0f && g.horizontal_bar > 0.0f)
		{
			ovg_tools::fill_rect(cb, vg, g.x + g.width - g.vertical_bar,
				g.y + g.height - g.horizontal_bar, g.vertical_bar,
				g.horizontal_bar, to_ovg(with_alpha(theme.panel_bg, 0xB0)));
		}

		if (self->loading_)
		{
			const float progress = self->load_progress();
			ovg_tools::fill_rect(cb, vg, g.text_x,
				g.y + g.text_height - 3.0f, g.text_width * progress, 3.0f,
				to_ovg(theme.accent));
		}

		/* Caret. */
		if (self->focused() && !self->read_only_)
		{
			const float phase = std::fmod(std::max(0.0f, self->tick_time_),
				1.0f);
			if (phase < 0.55f)
			{
				const float x = g.text_x +
					self->x_for_column(self->caret_.line,
						self->caret_.column) - self->scroll_x_;
				const float y = g.y + (float)self->caret_.line *
					line_height - self->scroll_y_;
				if (x >= g.text_x - 1.0f &&
					x <= g.text_x + g.text_width + 1.0f &&
					y + line_height > g.y && y < g.y + g.text_height)
				{
					ovg_tools::fill_rect(cb, vg, x, y, self->caret_width, line_height, to_ovg(theme.text));
				}
			}
		}

		const vgui_color border = self->focused() ?
			with_alpha(theme.accent, 0xD0) : theme.panel_border;
		ovg_tools::fill_rect(cb, vg, g.x, g.y, g.width, 1.0f,
			to_ovg(border));
		ovg_tools::fill_rect(cb, vg, g.x, g.y + g.height - 1.0f, g.width,
			1.0f, to_ovg(border));
		ovg_tools::fill_rect(cb, vg, g.x, g.y, 1.0f, g.height,
			to_ovg(border));
		ovg_tools::fill_rect(cb, vg, g.x + g.width - 1.0f, g.y, 1.0f,
			g.height, to_ovg(border));
	}

	/* ------------------------------------------------------------------------- */
	/* state                                                                     */
	/* ------------------------------------------------------------------------- */

	std::string code_editor_state_to_json(const code_editor_state& state)
	{
		nlohmann::json root;
		root["version"] = state.version;
		root["language"] = code_language_name(state.language);
		root["read_only"] = state.read_only;
		root["show_line_numbers"] = state.show_line_numbers;
		root["tab_size"] = state.tab_size;
		root["caret"] = { { "line", state.caret.line },
			{ "column", state.caret.column } };
		root["scroll"] = { { "x", state.scroll_x },{ "y", state.scroll_y } };
		return root.dump();
	}

	bool code_editor_state_from_json(const std::string& text,
		code_editor_state& out, std::string* error)
	{
		try
		{
			const nlohmann::json root = nlohmann::json::parse(text);
			if (!root.is_object())
				throw std::runtime_error("expected object");
			code_editor_state state;
			state.version = root.value("version", 1);
			if (state.version < 1 ||
				state.version > code_editor_state::current_version)
				throw std::runtime_error("unsupported code editor state version");

			code_language language = code_language::plain;
			const std::string name = root.value("language", std::string("plain"));
			if (!code_language_from_name(name, &language))
				throw std::runtime_error("unknown language: " + name);
			state.language = language;
			state.read_only = root.value("read_only", false);
			state.show_line_numbers = root.value("show_line_numbers", true);
			state.tab_size = std::clamp(root.value("tab_size", 4), 1, 16);

			if (root.contains("caret"))
			{
				const nlohmann::json& caret = root.at("caret");
				if (!caret.is_object())
					throw std::runtime_error("expected caret object");
				state.caret.line = caret.value("line", 0);
				state.caret.column = caret.value("column", 0);
			}
			if (state.caret.line < 0 || state.caret.column < 0)
				throw std::runtime_error("negative caret position");

			if (root.contains("scroll"))
			{
				const nlohmann::json& scroll = root.at("scroll");
				if (!scroll.is_object())
					throw std::runtime_error("expected scroll object");
				state.scroll_x = scroll.value("x", 0.0f);
				state.scroll_y = scroll.value("y", 0.0f);
				if (!std::isfinite(state.scroll_x) ||
					!std::isfinite(state.scroll_y))
					throw std::runtime_error("non-finite scroll offset");
				state.scroll_x = std::max(0.0f, state.scroll_x);
				state.scroll_y = std::max(0.0f, state.scroll_y);
			}
			out = state;
			return true;
		}
		catch (const std::exception& exception)
		{
			if (error)
				*error = exception.what();
			return false;
		}
	}

	code_editor_state code_editor::capture_state() const
	{
		code_editor_state state;
		state.version = code_editor_state::current_version;
		state.language = language_;
		state.read_only = read_only_;
		state.show_line_numbers = show_line_numbers_;
		state.tab_size = tab_size_;
		state.caret = caret_;
		state.scroll_x = scroll_x_;
		state.scroll_y = scroll_y_;
		return state;
	}

	bool code_editor::apply_state(const code_editor_state& state,
		std::string* error)
	{
		if (state.version < 1 ||
			state.version > code_editor_state::current_version)
		{
			if (error)
				*error = "unsupported code editor state version";
			return false;
		}
		if (!std::isfinite(state.scroll_x) || !std::isfinite(state.scroll_y))
		{
			if (error)
				*error = "non-finite scroll offset";
			return false;
		}
		set_language(state.language);
		set_read_only(state.read_only);
		set_show_line_numbers(state.show_line_numbers);
		set_tab_size(state.tab_size);
		caret_ = clamp_position(state.caret);
		anchor_ = caret_;
		scroll_x_ = std::max(0.0f, state.scroll_x);
		scroll_y_ = std::max(0.0f, state.scroll_y);
		clamp_scroll();
		update_text_input_rect();
		return true;
	}

	void code_editor::set_change_listener(change_listener listener)
	{
		change_listener_ = std::move(listener);
	}

} /* namespace vgui_editor_kit */