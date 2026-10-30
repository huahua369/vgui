#include <vgui/editor/markdown_view.h>

#include "code_lex.h"
#include "editor_metrics.h"
#include "markdown_lex.h"
#include "ui_scale.h"

#include "tools/tools.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace vgui_editor_kit {

	namespace {

		constexpr std::size_t kMaxMarkdownBytes = 1024u * 1024u;

		constexpr vgui_color kFallbackText = 0xFFD8DEE9;
		constexpr vgui_color kFallbackMuted = 0xFF8A93A3;
		constexpr vgui_color kFallbackBg = 0xFF1E2229;
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

		vgui_color mix_color(vgui_color a, vgui_color b, float mix)
		{
			mix = std::clamp(mix, 0.0f, 1.0f);
			const auto blend = [mix](uint32_t ca, uint32_t cb)
				{
					return (uint32_t)std::lround(
						(float)(ca & 0xFF) * (1.0f - mix) +
						(float)(cb & 0xFF) * mix);
				};
			return 0xFF000000u |
				(blend(a >> 16, b >> 16) << 16) |
				(blend(a >> 8, b >> 8) << 8) |
				blend(a, b);
		}

		std::string ascii_lower(std::string text)
		{
			for (char& c : text)
				c = (char)std::tolower((unsigned char)c);
			return text;
		}

		bool is_space_char(char c)
		{
			return c == ' ' || c == '\t' || c == '\v' || c == '\f';
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

		int utf8_next(const std::string& text, int index)
		{
			if (index < 0 || index >= (int)text.size())
				return (int)text.size();
			return std::min((int)text.size(), index +
				(int)utf8_char_len((unsigned char)text[(std::size_t)index]));
		}

		uint32_t utf8_decode(const std::string& text, int index, int* next)
		{
			const unsigned char c = (unsigned char)text[(std::size_t)index];
			const int length = (int)utf8_char_len(c);
			const int end = std::min((int)text.size(), index + length);
			if (next)
				*next = end;
			if (length == 1)
				return c;
			uint32_t value = c & (uint32_t)(0x7F >> length);
			for (int i = index + 1; i < end; ++i)
				value = (value << 6) |
				((unsigned char)text[(std::size_t)i] & 0x3Fu);
			return value;
		}

		bool is_wide_codepoint(uint32_t cp)
		{
			return (cp >= 0x1100 && cp <= 0x115F) ||
				(cp >= 0x2E80 && cp <= 0xA4CF) ||
				(cp >= 0xAC00 && cp <= 0xD7AF) ||
				(cp >= 0xF900 && cp <= 0xFAFF) ||
				(cp >= 0xFE30 && cp <= 0xFE4F) ||
				(cp >= 0xFF00 && cp <= 0xFF60) ||
				(cp >= 0xFFE0 && cp <= 0xFFE6) ||
				(cp >= 0x1F000 && cp <= 0x1FAFF) ||
				(cp >= 0x20000 && cp <= 0x3FFFD);
		}

		int indent_columns(const std::string& line, int* begin)
		{
			int column = 0;
			int index = 0;
			while (index < (int)line.size())
			{
				const char c = line[(std::size_t)index];
				if (c == ' ')
					column += 1;
				else if (c == '\t')
					column += 4 - (column % 4);
				else
					break;
				++index;
			}
			if (begin)
				*begin = index;
			return column;
		}

		int trim_right(const std::string& line, int begin)
		{
			int end = (int)line.size();
			while (end > begin && is_space_char(line[(std::size_t)end - 1]))
				--end;
			return end;
		}

		std::string trim_copy(const std::string& text)
		{
			int begin = 0;
			while (begin < (int)text.size() && is_space_char(text[(std::size_t)begin]))
				++begin;
			int end = (int)text.size();
			while (end > begin && is_space_char(text[(std::size_t)end - 1]))
				--end;
			return text.substr((std::size_t)begin, (std::size_t)(end - begin));
		}

		std::string file_name_of(std::string source)
		{
			const std::size_t query = source.find_first_of("?#");
			if (query != std::string::npos)
				source = source.substr(0, query);
			const std::size_t slash = source.find_last_of("/\\");
			if (slash != std::string::npos)
				source = source.substr(slash + 1);
			return source;
		}

		bool is_video_source(std::string source)
		{
			source = ascii_lower(file_name_of(std::move(source)));
			const std::size_t dot = source.find_last_of('.');
			if (dot == std::string::npos)
				return false;
			const std::string ext = source.substr(dot + 1);
			return ext == "mp4" || ext == "webm" || ext == "mov" ||
				ext == "m4v" || ext == "mpeg" || ext == "mpg" ||
				ext == "ogv";
		}

		/* --------------------------------------------------------------------- */
		/* document model                                                        */
		/* --------------------------------------------------------------------- */

		enum class md_block_kind
		{
			paragraph,
			heading,
			quote,
			list_item,
			code,
			table,
			rule,
			media
		};

		struct md_style
		{
			bool bold = false;
			bool italic = false;
			bool strike = false;
			bool code = false;
			bool link = false;
			code_token_kind color = code_token_kind::text;
		};

		struct md_run
		{
			int start = 0;
			int length = 0;
			md_style style;
		};

		struct md_cell
		{
			std::string text;
			std::vector<md_run> runs;
			/* Byte offset of text inside the joined block.text. */
			int offset = 0;
		};

		struct md_table_row
		{
			std::vector<md_cell> cells;
			bool header = false;
			int source_line = 0;
		};

		struct md_media
		{
			md_media_kind kind = md_media_kind::image;
			std::string source;
			std::string alt;
			std::string title;
			std::string caption;
			md_media_status status = md_media_status::pending;
			vgui_texture* texture = nullptr;
			float aspect = 0.0f;
			std::string error;
		};

		struct md_block
		{
			md_block_kind kind = md_block_kind::paragraph;
			int level = 0;
			int source_first = 0;
			int source_last = 0;

			std::string text;
			std::vector<md_run> runs;
			/* paragraph/quote/list: display offset -> source line */
			std::vector<int> source_offsets;
			std::vector<int> source_lines;

			code_language language = code_language::plain;
			std::vector<std::string> code_lines;
			std::vector<int> code_line_offsets;

			std::vector<md_table_row> rows;
			std::vector<int> column_align;

			bool ordered = false;
			int ordinal = 0;
			bool task = false;
			bool checked = false;

			md_media media;

			/* layout results, refreshed by impl::layout() */
			float top = 0.0f;
			float height = 0.0f;
			float text_font = 0.0f;
			float line_height = 0.0f;
			int line_begin = 0;
			int line_count = 0;
			float media_x = 0.0f;
			float media_y = 0.0f;
			float media_w = 0.0f;
			float media_h = 0.0f;
			float caption_y = 0.0f;
			std::vector<float> column_widths;
		};

		struct md_segment
		{
			int start = 0;
			int end = 0;
			float x = 0.0f;
			float width = 0.0f;
			/* Table cells clip their runs at the cell edge; 0 = no extra clip. */
			float clip_w = 0.0f;
			md_style style;
		};

		struct md_line
		{
			int block = 0;
			int source_line = 0;
			float y = 0.0f;
			float height = 0.0f;
			float x = 0.0f;
			float width = 0.0f;
			std::vector<md_segment> segments;
		};

		/* --------------------------------------------------------------------- */
		/* inline parsing                                                        */
		/* --------------------------------------------------------------------- */

		void push_run(std::vector<md_run>& runs, int start, int length,
			const md_style& style)
		{
			if (length <= 0)
				return;
			if (!runs.empty())
			{
				md_run& last = runs.back();
				if (last.start + last.length == start &&
					last.style.bold == style.bold &&
					last.style.italic == style.italic &&
					last.style.strike == style.strike &&
					last.style.code == style.code &&
					last.style.link == style.link &&
					last.style.color == style.color)
				{
					last.length += length;
					return;
				}
			}
			md_run run;
			run.start = start;
			run.length = length;
			run.style = style;
			runs.push_back(run);
		}

		void append_escaped(std::string& out, const std::string& source,
			int begin, int end)
		{
			int pos = begin;
			while (pos < end)
			{
				if (source[(std::size_t)pos] == '\\' && pos + 1 < end)
				{
					out.push_back(source[(std::size_t)pos + 1]);
					pos += 2;
					continue;
				}
				out.push_back(source[(std::size_t)pos]);
				++pos;
			}
		}

		void append_styled(std::string& out, std::vector<md_run>& runs,
			const std::string& source, int begin, int end,
			const md_style& style)
		{
			if (end <= begin)
				return;
			const int start = (int)out.size();
			append_escaped(out, source, begin, end);
			push_run(runs, start, (int)out.size() - start, style);
		}

		void append_inline(std::string& out, std::vector<md_run>& runs,
			const std::string& source, int begin, int end,
			const md_style& base)
		{
			begin = std::clamp(begin, 0, (int)source.size());
			end = std::clamp(end, begin, (int)source.size());
			if (begin >= end)
				return;

			std::vector<code_span> spans;
			md_lex::scan_inline(source, begin, end, code_token_kind::text,
				spans);

			int pos = begin;
			std::size_t span_index = 0;
			while (pos < end)
			{
				while (span_index < spans.size() &&
					spans[span_index].start + spans[span_index].length <= pos)
					++span_index;
				if (span_index >= spans.size())
				{
					append_styled(out, runs, source, pos, end, base);
					break;
				}
				const code_span& span = spans[span_index];
				const int start = std::clamp(span.start, pos, end);
				const int stop = std::clamp(span.start + span.length, start,
					end);
				if (start > pos)
					append_styled(out, runs, source, pos, start, base);

				switch (span.kind)
				{
				case code_token_kind::inline_code:
				{
					const int marker = md_lex::run(source, start, '`', stop);
					if (stop - marker > start + marker)
					{
						md_style style = base;
						style.code = true;
						style.color = code_token_kind::inline_code;
						append_styled(out, runs, source, start + marker,
							stop - marker, style);
					}
					break;
				}
				case code_token_kind::strike:
				{
					if (stop - 2 > start + 2)
					{
						md_style style = base;
						style.strike = true;
						style.color = code_token_kind::strike;
						append_styled(out, runs, source, start + 2, stop - 2,
							style);
					}
					break;
				}
				case code_token_kind::emphasis:
				case code_token_kind::strong:
				{
					const char marker_char = source[(std::size_t)start];
					int opening = md_lex::run(source, start, marker_char, stop);
					int closing = 0;
					while (stop - 1 - closing >= start &&
						source[(std::size_t)(stop - 1 - closing)] == marker_char)
						++closing;
					const int marker = std::max(1,
						std::min({ opening, closing, 3 }));
					if (stop - marker > start + marker)
					{
						md_style style = base;
						style.bold = marker >= 2;
						style.italic = marker != 2;
						style.color = marker >= 2 ? code_token_kind::strong :
							code_token_kind::emphasis;
						append_inline(out, runs, source, start + marker,
							stop - marker, style);
					}
					break;
				}
				case code_token_kind::link:
				{
					const int opening = source[(std::size_t)start] == '!' ? 2 : 1;
					md_style style = base;
					style.link = true;
					style.color = code_token_kind::link;
					if (stop - 1 > start + opening)
						append_inline(out, runs, source, start + opening,
							stop - 1, style);
					if (span_index + 1 < spans.size() &&
						spans[span_index + 1].kind ==
						code_token_kind::link_url &&
						spans[span_index + 1].start >= stop)
					{
						bool spaces_only = true;
						for (int i = stop; i < spans[span_index + 1].start; ++i)
						{
							if (!is_space_char(source[(std::size_t)i]))
							{
								spaces_only = false;
								break;
							}
						}
						if (spaces_only)
						{
							pos = std::clamp(spans[span_index + 1].start +
								spans[span_index + 1].length, stop, end);
							span_index += 2;
							continue;
						}
					}
					break;
				}
				case code_token_kind::link_url:
				{
					int url_begin = start;
					int url_end = stop;
					if (url_begin < url_end &&
						source[(std::size_t)url_begin] == '<')
						++url_begin;
					if (url_end > url_begin &&
						source[(std::size_t)url_end - 1] == '>')
						--url_end;
					md_style style = base;
					style.link = true;
					style.color = code_token_kind::link;
					append_styled(out, runs, source, url_begin, url_end,
						style);
					break;
				}
				case code_token_kind::comment:
					break;
				default:
					append_styled(out, runs, source, start, stop, base);
					break;
				}

				pos = stop;
				++span_index;
			}
		}

		/* --------------------------------------------------------------------- */
		/* media syntax                                                          */
		/* --------------------------------------------------------------------- */

		void parse_attributes(const std::string& tag,
			std::vector<std::pair<std::string, std::string>>& attrs)
		{
			int pos = 0;
			while (pos < (int)tag.size() && tag[(std::size_t)pos] != ' ' &&
				tag[(std::size_t)pos] != '\t')
				++pos;
			while (pos < (int)tag.size())
			{
				if (tag[(std::size_t)pos] == '>')
					break;
				while (pos < (int)tag.size() &&
					(tag[(std::size_t)pos] == ' ' ||
						tag[(std::size_t)pos] == '\t' ||
						tag[(std::size_t)pos] == '/'))
					++pos;
				if (pos >= (int)tag.size() || tag[(std::size_t)pos] == '>')
					break;
				const int name_begin = pos;
				while (pos < (int)tag.size())
				{
					const char c = tag[(std::size_t)pos];
					if (c == '=' || c == ' ' || c == '\t' || c == '>' ||
						c == '/')
						break;
					++pos;
				}
				const std::string name = ascii_lower(tag.substr(
					(std::size_t)name_begin, (std::size_t)(pos - name_begin)));
				while (pos < (int)tag.size() &&
					(tag[(std::size_t)pos] == ' ' ||
						tag[(std::size_t)pos] == '\t'))
					++pos;
				std::string value;
				if (pos < (int)tag.size() && tag[(std::size_t)pos] == '=')
				{
					++pos;
					while (pos < (int)tag.size() &&
						(tag[(std::size_t)pos] == ' ' ||
							tag[(std::size_t)pos] == '\t'))
						++pos;
					if (pos < (int)tag.size() &&
						(tag[(std::size_t)pos] == '"' ||
							tag[(std::size_t)pos] == '\''))
					{
						const char quote = tag[(std::size_t)pos++];
						const int value_begin = pos;
						while (pos < (int)tag.size() &&
							tag[(std::size_t)pos] != quote)
							++pos;
						value = tag.substr((std::size_t)value_begin,
							(std::size_t)(pos - value_begin));
						if (pos < (int)tag.size())
							++pos;
					}
					else
					{
						const int value_begin = pos;
						while (pos < (int)tag.size() &&
							tag[(std::size_t)pos] != ' ' &&
							tag[(std::size_t)pos] != '\t' &&
							tag[(std::size_t)pos] != '>')
							++pos;
						value = tag.substr((std::size_t)value_begin,
							(std::size_t)(pos - value_begin));
					}
				}
				if (!name.empty())
					attrs.push_back({ name, value });
				else
					++pos;
			}
		}

		bool parse_html_media(const std::string& text, md_media* media)
		{
			const std::string lower = ascii_lower(text);
			const bool video = lower.rfind("<video", 0) == 0;
			if (!video && lower.rfind("<img", 0) != 0)
				return false;
			if (text.empty() || text.back() != '>')
				return false;

			std::vector<std::pair<std::string, std::string>> attrs;
			parse_attributes(text, attrs);
			std::string source;
			std::string poster;
			std::string width;
			std::string height;
			media->alt.clear();
			media->title.clear();
			for (const auto& attr : attrs)
			{
				if (attr.first == "src")
					source = attr.second;
				else if (attr.first == "poster")
					poster = attr.second;
				else if (attr.first == "alt")
					media->alt = attr.second;
				else if (attr.first == "title")
					media->title = attr.second;
				else if (attr.first == "width")
					width = attr.second;
				else if (attr.first == "height")
					height = attr.second;
			}
			if (source.empty())
				source = poster;
			if (source.empty())
				return false;
			media->source = source;
			media->kind = video ? md_media_kind::video : md_media_kind::image;
			if (!width.empty() && !height.empty())
			{
				const float w = (float)std::atof(width.c_str());
				const float h = (float)std::atof(height.c_str());
				if (w > 0.0f && h > 0.0f)
					media->aspect = w / h;
			}
			return true;
		}

		bool parse_markdown_media(const std::string& text, md_media* media)
		{
			if (text.size() < 4 || text[0] != '!' || text[1] != '[')
				return false;
			const int close = md_lex::find_close_bracket(text, 1,
				(int)text.size());
			if (close <= 1 || close + 1 >= (int)text.size())
				return false;
			const std::string alt = text.substr(2,
				(std::size_t)(close - 2));
			std::string source;
			std::string title;
			if (text[(std::size_t)close + 1] == '(')
			{
				const int url_end = md_lex::find_close_paren(text, close + 1,
					(int)text.size());
				if (url_end < 0)
					return false;
				for (int i = url_end + 1; i < (int)text.size(); ++i)
				{
					if (!is_space_char(text[(std::size_t)i]))
						return false;
				}
				std::string inner = trim_copy(text.substr(
					(std::size_t)(close + 2),
					(std::size_t)(url_end - close - 2)));
				if (inner.empty())
					return false;
				if (inner[0] == '<')
				{
					const std::size_t angle = inner.find('>');
					if (angle == std::string::npos)
						return false;
					source = inner.substr(1, angle - 1);
					inner = trim_copy(inner.substr(angle + 1));
				}
				else
				{
					const std::size_t space = inner.find_first_of(" \t");
					if (space == std::string::npos)
					{
						source = inner;
						inner.clear();
					}
					else
					{
						source = inner.substr(0, space);
						inner = trim_copy(inner.substr(space + 1));
					}
				}
				if (!inner.empty() && (inner.front() == '"' ||
					inner.front() == '\''))
				{
					const char quote = inner.front();
					const std::size_t end_quote = inner.find(quote, 1);
					title = inner.substr(1, end_quote == std::string::npos ?
						std::string::npos : end_quote - 1);
				}
			}
			else if (text[(std::size_t)close + 1] == '[')
			{
				const int ref_end = md_lex::find_close_bracket(text, close + 1,
					(int)text.size());
				if (ref_end < 0)
					return false;
				for (int i = ref_end + 1; i < (int)text.size(); ++i)
				{
					if (!is_space_char(text[(std::size_t)i]))
						return false;
				}
				source = trim_copy(text.substr((std::size_t)(close + 2),
					(std::size_t)(ref_end - close - 2)));
				if (source.empty())
					return false;
			}
			else
			{
				return false;
			}
			if (source.empty())
				return false;
			media->source = source;
			media->alt = alt;
			media->title = title;
			media->kind = is_video_source(source) ? md_media_kind::video :
				md_media_kind::image;
			return true;
		}

		bool parse_media_line(const std::string& line, md_media* media)
		{
			const int begin = md_lex::first_nonspace(line);
			const int end = trim_right(line, begin);
			if (end <= begin)
				return false;
			const std::string text = line.substr((std::size_t)begin,
				(std::size_t)(end - begin));
			return parse_html_media(text, media) ||
				parse_markdown_media(text, media);
		}

		/* --------------------------------------------------------------------- */
		/* block parsing                                                         */
		/* --------------------------------------------------------------------- */

		std::vector<std::string> split_lines(const std::string& text)
		{
			std::vector<std::string> lines;
			std::string current;
			for (char c : text)
			{
				if (c == '\r')
					continue;
				if (c == '\n')
				{
					lines.push_back(current);
					current.clear();
					continue;
				}
				current.push_back(c);
			}
			lines.push_back(current);
			return lines;
		}

		bool fence_open(const std::string& line, int begin, int indent,
			char* fence_char, int* fence_len)
		{
			if (indent > 3 || begin >= (int)line.size())
				return false;
			const char c = line[(std::size_t)begin];
			if (c != '`' && c != '~')
				return false;
			const int length = md_lex::run(line, begin, c, (int)line.size());
			if (length < 3)
				return false;
			*fence_char = c;
			*fence_len = length;
			return true;
		}

		bool atx_heading(const std::string& line, int begin, int indent,
			int* level, int* content_begin, int* content_end)
		{
			if (indent > 3 || begin >= (int)line.size() ||
				line[(std::size_t)begin] != '#')
				return false;
			const int hashes = md_lex::run(line, begin, '#', (int)line.size());
			if (hashes < 1 || hashes > 6)
				return false;
			if (begin + hashes < (int)line.size() &&
				!is_space_char(line[(std::size_t)(begin + hashes)]))
				return false;

			int start = begin + hashes;
			while (start < (int)line.size() &&
				is_space_char(line[(std::size_t)start]))
				++start;
			int end = trim_right(line, start);
			int tail = end;
			while (tail > start && line[(std::size_t)tail - 1] == '#')
				--tail;
			if (tail < end && (tail == start ||
				is_space_char(line[(std::size_t)tail - 1])))
			{
				end = tail;
				while (end > start && is_space_char(line[(std::size_t)end - 1]))
					--end;
			}
			*level = hashes;
			*content_begin = start;
			*content_end = end;
			return true;
		}

		bool list_marker(const std::string& line, int begin, int indent,
			int* marker_end, bool* ordered, int* ordinal, int* content_begin)
		{
			if (indent > 3 || begin >= (int)line.size())
				return false;
			const char c = line[(std::size_t)begin];
			if (c == '-' || c == '*' || c == '+')
			{
				if (begin + 1 >= (int)line.size() ||
					!is_space_char(line[(std::size_t)begin + 1]))
					return false;
				*marker_end = begin + 1;
				*ordered = false;
				*ordinal = 0;
			}
			else if (std::isdigit((unsigned char)c))
			{
				int index = begin;
				while (index < (int)line.size() &&
					std::isdigit((unsigned char)line[(std::size_t)index]))
					++index;
				if (index >= (int)line.size() ||
					(line[(std::size_t)index] != '.' &&
						line[(std::size_t)index] != ')') ||
					index + 1 >= (int)line.size() ||
					!is_space_char(line[(std::size_t)index + 1]))
					return false;
				*ordered = true;
				*ordinal = std::atoi(line.substr((std::size_t)begin,
					(std::size_t)(index - begin)).c_str());
				*marker_end = index + 1;
			}
			else
			{
				return false;
			}
			int content = *marker_end;
			while (content < (int)line.size() &&
				is_space_char(line[(std::size_t)content]))
				++content;
			*content_begin = content;
			return true;
		}

		bool has_pipe(const std::string& line)
		{
			for (int i = 0; i < (int)line.size(); ++i)
			{
				if (line[(std::size_t)i] == '\\')
				{
					++i;
					continue;
				}
				if (line[(std::size_t)i] == '|')
					return true;
			}
			return false;
		}

		bool table_start(const std::vector<std::string>& lines, int index)
		{
			if (index + 1 >= (int)lines.size())
				return false;
			if (!has_pipe(lines[(std::size_t)index]))
				return false;
			const std::string& separator = lines[(std::size_t)index + 1];
			return md_lex::table_separator(separator,
				md_lex::first_nonspace(separator));
		}

		bool line_starts_block(const std::vector<std::string>& lines, int index)
		{
			const std::string& line = lines[(std::size_t)index];
			if (md_lex::blank(line))
				return true;
			int begin = 0;
			const int indent = indent_columns(line, &begin);
			if (indent > 3)
				return false;
			char fence_char = 0;
			int fence_len = 0;
			if (fence_open(line, begin, indent, &fence_char, &fence_len))
				return true;
			int level = 0;
			int content_begin = 0;
			int content_end = 0;
			if (atx_heading(line, begin, indent, &level, &content_begin,
				&content_end))
				return true;
			if (begin < (int)line.size() &&
				line[(std::size_t)begin] == '>')
				return true;
			if (md_lex::thematic_break(line, begin, nullptr))
				return true;
			int marker_end = 0;
			bool ordered = false;
			int ordinal = 0;
			int list_content = 0;
			if (list_marker(line, begin, indent, &marker_end, &ordered,
				&ordinal, &list_content))
				return true;
			if (table_start(lines, index))
				return true;
			md_media media;
			if (parse_media_line(line, &media))
				return true;
			return false;
		}

		void build_code_block(md_block& block)
		{
			block.text.clear();
			block.code_line_offsets.clear();
			for (std::size_t i = 0; i < block.code_lines.size(); ++i)
			{
				block.code_line_offsets.push_back((int)block.text.size());
				block.text += block.code_lines[i];
				if (i + 1 < block.code_lines.size())
					block.text.push_back('\n');
			}
			if (block.language == code_language::plain)
				return;

			int state = 0;
			for (std::size_t i = 0; i < block.code_lines.size(); ++i)
			{
				std::vector<code_span> spans;
				tokenize_code_line(block.language, block.code_lines[i],
					state, spans, &state);
				const int offset = block.code_line_offsets[i];
				for (const code_span& span : spans)
				{
					if (span.kind == code_token_kind::text)
						continue;
					const int start = std::clamp(span.start, 0,
						(int)block.code_lines[i].size());
					const int end = std::clamp(span.start + span.length,
						start, (int)block.code_lines[i].size());
					md_style style;
					style.color = span.kind;
					push_run(block.runs, offset + start, end - start, style);
				}
			}
		}

		std::vector<std::string> split_table_row(const std::string& line)
		{
			std::vector<std::string> cells;
			int begin = md_lex::first_nonspace(line);
			int end = trim_right(line, begin);
			if (begin < end && line[(std::size_t)begin] == '|')
				++begin;
			if (end > begin && line[(std::size_t)end - 1] == '|')
				--end;
			std::string current;
			for (int i = begin; i < end; ++i)
			{
				const char c = line[(std::size_t)i];
				if (c == '\\' && i + 1 < end)
				{
					current.push_back(c);
					current.push_back(line[(std::size_t)i + 1]);
					++i;
					continue;
				}
				if (c == '|')
				{
					cells.push_back(trim_copy(current));
					current.clear();
					continue;
				}
				current.push_back(c);
			}
			cells.push_back(trim_copy(current));
			return cells;
		}

		void append_shifted_runs(std::vector<md_run>& destination,
			const std::vector<md_run>& source, int shift)
		{
			for (const md_run& run : source)
				push_run(destination, run.start + shift, run.length, run.style);
		}

		void build_table_block(md_block& block)
		{
			block.text.clear();
			block.runs.clear();
			for (std::size_t row = 0; row < block.rows.size(); ++row)
			{
				if (row > 0)
					block.text.push_back('\n');
				for (std::size_t cell = 0;
					cell < block.rows[row].cells.size(); ++cell)
				{
					if (cell > 0)
						block.text.push_back('\t');
					md_cell& item = block.rows[row].cells[cell];
					const int offset = (int)block.text.size();
					item.offset = offset;
					block.text += item.text;
					append_shifted_runs(block.runs, item.runs, offset);
				}
			}
		}

		int source_line_for_offset(const md_block& block, int offset)
		{
			int line = block.source_first;
			for (std::size_t i = 0; i < block.source_offsets.size(); ++i)
			{
				if (block.source_offsets[i] <= offset)
					line = block.source_lines[i];
				else
					break;
			}
			return line;
		}

		std::string expand_tabs(const std::string& text);

		void parse_document(const std::string& source,
			std::vector<md_block>& blocks)
		{
			const std::vector<std::string> lines = split_lines(source);
			const int count = (int)lines.size();
			int index = 0;
			while (index < count)
			{
				const std::string& line = lines[(std::size_t)index];
				if (md_lex::blank(line))
				{
					++index;
					continue;
				}
				int begin = 0;
				const int indent = indent_columns(line, &begin);

				/* Fenced code. */
				char fence_char = 0;
				int fence_len = 0;
				if (fence_open(line, begin, indent, &fence_char, &fence_len))
				{
					md_block block;
					block.kind = md_block_kind::code;
					block.source_first = index;
					const int info_begin = begin + fence_len;
					const int info_end = trim_right(line, info_begin);
					block.language = md_lex::fence_language_from_info(
						trim_copy(line.substr((std::size_t)info_begin,
							(std::size_t)(info_end - info_begin))));
					int next = index + 1;
					for (; next < count; ++next)
					{
						const std::string& body = lines[(std::size_t)next];
						int body_begin = 0;
						const int body_indent = indent_columns(body, &body_begin);
						if (body_indent <= 3 && body_begin < (int)body.size() &&
							body[(std::size_t)body_begin] == fence_char)
						{
							const int close_len = md_lex::run(body, body_begin,
								fence_char, (int)body.size());
							if (close_len >= fence_len &&
								trim_right(body, body_begin + close_len) ==
								body_begin + close_len)
							{
								++next;
								break;
							}
						}
						const std::size_t strip = (std::size_t)std::min(
							indent, body_begin);
						block.code_lines.push_back(
							expand_tabs(body.substr(strip)));
					}
					block.source_last = std::min(next, count) - 1;
					build_code_block(block);
					blocks.push_back(std::move(block));
					index = next;
					continue;
				}

				/* ATX heading. */
				int level = 0;
				int content_begin = 0;
				int content_end = 0;
				if (atx_heading(line, begin, indent, &level, &content_begin,
					&content_end))
				{
					md_block block;
					block.kind = md_block_kind::heading;
					block.level = level;
					block.source_first = block.source_last = index;
					block.source_offsets.push_back(0);
					block.source_lines.push_back(index);
					md_style style;
					style.color = code_token_kind::heading;
					style.bold = level <= 2;
					append_inline(block.text, block.runs, line, content_begin,
						content_end, style);
					blocks.push_back(std::move(block));
					++index;
					continue;
				}

				/* Thematic break. */
				if (md_lex::thematic_break(line, begin, nullptr))
				{
					md_block block;
					block.kind = md_block_kind::rule;
					block.source_first = block.source_last = index;
					blocks.push_back(std::move(block));
					++index;
					continue;
				}

				/* Table. */
				if (indent <= 3 && table_start(lines, index))
				{
					md_block block;
					block.kind = md_block_kind::table;
					block.source_first = index;
					const std::vector<std::string> separator =
						split_table_row(lines[(std::size_t)index + 1]);
					std::vector<std::string> header =
						split_table_row(line);
					const std::size_t columns = std::max(header.size(),
						separator.size());
					block.column_align.assign(columns, 0);
					for (std::size_t col = 0; col < separator.size() &&
						col < columns; ++col)
					{
						const std::string& cell = separator[col];
						const bool left = !cell.empty() && cell.front() == ':';
						const bool right = !cell.empty() && cell.back() == ':';
						block.column_align[col] = left && right ? 1 :
							(right ? 2 : 0);
					}
					header.resize(columns);

					md_table_row header_row;
					header_row.header = true;
					header_row.source_line = index;
					for (std::size_t col = 0; col < columns; ++col)
					{
						md_cell cell;
						md_style style;
						style.bold = true;
						append_inline(cell.text, cell.runs,
							header[col], 0, (int)header[col].size(), style);
						header_row.cells.push_back(std::move(cell));
					}
					block.rows.push_back(std::move(header_row));

					int next = index + 2;
					for (; next < count; ++next)
					{
						const std::string& body = lines[(std::size_t)next];
						if (md_lex::blank(body) || !has_pipe(body))
							break;
						int body_begin = 0;
						const int body_indent = indent_columns(body, &body_begin);
						if (body_indent > 3)
							break;
						std::vector<std::string> cells = split_table_row(body);
						cells.resize(columns);
						md_table_row row;
						row.source_line = next;
						for (std::size_t col = 0; col < columns; ++col)
						{
							md_cell cell;
							md_style style;
							append_inline(cell.text, cell.runs, cells[col], 0,
								(int)cells[col].size(), style);
							row.cells.push_back(std::move(cell));
						}
						block.rows.push_back(std::move(row));
					}
					block.source_last = next - 1;
					build_table_block(block);
					blocks.push_back(std::move(block));
					index = next;
					continue;
				}

				/* Blockquote. */
				if (indent <= 3 && begin < (int)line.size() &&
					line[(std::size_t)begin] == '>')
				{
					md_block block;
					block.kind = md_block_kind::quote;
					block.source_first = index;
					int next = index;
					int depth = 0;
					while (next < count)
					{
						const std::string& body = lines[(std::size_t)next];
						if (md_lex::blank(body))
							break;
						int quote_begin = 0;
						const int quote_indent = indent_columns(body,
							&quote_begin);
						if (quote_indent > 3 ||
							quote_begin >= (int)body.size() ||
							body[(std::size_t)quote_begin] != '>')
							break;
						int pos = quote_begin;
						int line_depth = 0;
						while (pos < (int)body.size() &&
							body[(std::size_t)pos] == '>')
						{
							++line_depth;
							++pos;
							if (pos < (int)body.size() &&
								is_space_char(body[(std::size_t)pos]))
								++pos;
						}
						depth = std::max(depth, line_depth);
						if (!block.text.empty())
							block.text.push_back(' ');
						block.source_offsets.push_back(
							(int)block.text.size());
						block.source_lines.push_back(next);
						md_style style;
						style.color = code_token_kind::quote;
						append_inline(block.text, block.runs, body, pos,
							trim_right(body, pos), style);
						block.source_last = next;
						++next;
					}
					block.level = depth;
					blocks.push_back(std::move(block));
					index = next;
					continue;
				}

				/* List item. */
				int marker_end = 0;
				bool ordered = false;
				int ordinal = 0;
				int list_content = 0;
				if (list_marker(line, begin, indent, &marker_end, &ordered,
					&ordinal, &list_content))
				{
					md_block block;
					block.kind = md_block_kind::list_item;
					block.ordered = ordered;
					block.ordinal = ordinal;
					block.level = indent / 2;
					block.source_first = index;
					int content = list_content;
					if (content + 2 < (int)line.size() &&
						line[(std::size_t)content] == '[' &&
						(line[(std::size_t)content + 1] == ' ' ||
							line[(std::size_t)content + 1] == 'x' ||
							line[(std::size_t)content + 1] == 'X') &&
						line[(std::size_t)content + 2] == ']')
					{
						block.task = true;
						block.checked = line[(std::size_t)content + 1] != ' ';
						content += 3;
						while (content < (int)line.size() &&
							is_space_char(line[(std::size_t)content]))
							++content;
					}
					block.source_offsets.push_back(0);
					block.source_lines.push_back(index);
					md_style style;
					append_inline(block.text, block.runs, line, content,
						trim_right(line, content), style);
					int next = index + 1;
					for (; next < count; ++next)
					{
						const std::string& body = lines[(std::size_t)next];
						if (md_lex::blank(body))
							break;
						int body_begin = 0;
						const int body_indent = indent_columns(body,
							&body_begin);
						if (body_indent < 2 || line_starts_block(lines, next))
							break;
						block.text.push_back(' ');
						block.source_offsets.push_back(
							(int)block.text.size());
						block.source_lines.push_back(next);
						append_inline(block.text, block.runs, body, body_begin,
							trim_right(body, body_begin), style);
						block.source_last = next;
					}
					if (block.source_last < index)
						block.source_last = index;
					blocks.push_back(std::move(block));
					index = next;
					continue;
				}

				/* Media (image/video on its own line). */
				md_media media;
				if (parse_media_line(line, &media))
				{
					md_block block;
					block.kind = md_block_kind::media;
					block.source_first = block.source_last = index;
					block.media = media;
					block.media.caption = media.alt.empty() ?
						file_name_of(media.source) : media.alt;
					if (!block.media.caption.empty())
					{
						md_style style;
						style.color = code_token_kind::comment;
						const std::string caption = block.media.caption;
						append_inline(block.text, block.runs, caption, 0,
							(int)caption.size(), style);
					}
					blocks.push_back(std::move(block));
					++index;
					continue;
				}

				/* Paragraph, possibly promoted to a setext heading. */
				{
					md_block block;
					block.kind = md_block_kind::paragraph;
					block.source_first = index;
					int next = index;
					bool setext = false;
					while (next < count)
					{
						const std::string& body = lines[(std::size_t)next];
						if (md_lex::blank(body))
							break;
						if (next > index)
						{
							int underline_begin = 0;
							const int underline_indent = indent_columns(body,
								&underline_begin);
							char marker = 0;
							if (underline_indent <= 3 &&
								md_lex::setext_underline(body, underline_begin,
									&marker))
							{
								block.kind = md_block_kind::heading;
								block.level = marker == '=' ? 1 : 2;
								block.source_last = next;
								++next;
								setext = true;
								break;
							}
							if (line_starts_block(lines, next))
								break;
						}
						int body_begin = 0;
						const int body_indent = indent_columns(body,
							&body_begin);
						const int body_start = body_indent <= 3 ? body_begin : 0;
						if (!block.text.empty())
							block.text.push_back(' ');
						block.source_offsets.push_back(
							(int)block.text.size());
						block.source_lines.push_back(next);
						md_style style;
						append_inline(block.text, block.runs, body, body_start,
							trim_right(body, body_start), style);
						block.source_last = next;
						++next;
					}
					if (block.text.empty() && setext)
					{
						/* Empty setext heading: keep a zero-height heading. */
					}
					if (setext)
					{
						for (md_run& run : block.runs)
						{
							run.style.color = code_token_kind::heading;
							run.style.bold = block.level <= 2;
						}
					}
					if (!block.text.empty() || setext)
						blocks.push_back(std::move(block));
					index = next;
					continue;
				}
			}
		}

		std::string expand_tabs(const std::string& text)
		{
			std::string out;
			int column = 0;
			for (char c : text)
			{
				if (c == '\t')
				{
					const int next = column + 4 - (column % 4);
					out.append((std::size_t)(next - column), ' ');
					column = next;
					continue;
				}
				out.push_back(c);
				if (((unsigned char)c & 0xC0) != 0x80)
					++column;
			}
			return out;
		}

		vgui_theme theme_for(vgui_widget* widget)
		{
			vgui_theme theme = {};
			vgui_context* ctx = widget ? vgui_widget_get_context(widget) : nullptr;
			if (ctx)
				vgui_context_get_theme(ctx, &theme);
			theme.text = theme.text ? theme.text : kFallbackText;
			theme.panel_bg = theme.panel_bg ? theme.panel_bg : kFallbackBg;
			theme.panel_border = theme.panel_border ?
				theme.panel_border : kFallbackBorder;
			theme.accent = theme.accent ? theme.accent : 0xFF61AFEF;
			theme.edit_selection = theme.edit_selection ? theme.edit_selection :
				with_alpha(theme.accent, 0x55);
			theme.scrollbar = theme.scrollbar ? theme.scrollbar : 0xFF4B5263;
			theme.scrollbar_hover = theme.scrollbar_hover ?
				theme.scrollbar_hover : theme.scrollbar;
			return theme;
		}

		/* Scissor rects ignore the OVG save/restore stack: intersect with the
		 * active clip and put the caller's clip back when leaving. */
		class clip_guard
		{
		public:
			clip_guard(ovg_ctx_cb* cb, rvg_t* vg, float x, float y, float w,
				float h) : cb_(cb), vg_(vg)
			{
				if (!cb_ || !vg_ || !cb_->clip_rect)
					return;
				if (cb_->get_clip_rect)
				{
					cb_->get_clip_rect(vg_, saved_);
					has_saved_ = true;
				}
				int x0 = (int)std::floor(x);
				int y0 = (int)std::floor(y);
				int x1 = (int)std::ceil(x + w);
				int y1 = (int)std::ceil(y + h);
				if (has_saved_ && saved_[2] > 0 && saved_[3] > 0)
				{
					x0 = std::max(x0, saved_[0]);
					y0 = std::max(y0, saved_[1]);
					x1 = std::min(x1, saved_[0] + saved_[2]);
					y1 = std::min(y1, saved_[1] + saved_[3]);
				}
				cb_->clip_rect(vg_, x0, y0, std::max(0, x1 - x0),
					std::max(0, y1 - y0));
				active_ = true;
			}

			~clip_guard()
			{
				if (!active_ || !cb_ || !vg_)
					return;
				if (has_saved_ && cb_->set_clip_rect)
					cb_->set_clip_rect(vg_, saved_);
				else if (cb_->clip_rect)
					cb_->clip_rect(vg_, 0, 0, -1, -1);
			}

			clip_guard(const clip_guard&) = delete;
			clip_guard& operator=(const clip_guard&) = delete;

		private:
			ovg_ctx_cb* cb_ = nullptr;
			rvg_t* vg_ = nullptr;
			int saved_[4] = { 0, 0, 0, 0 };
			bool has_saved_ = false;
			bool active_ = false;
		};

	} // namespace

	struct markdown_view::impl
	{
		vgui_context* ctx = nullptr;
		vgui_widget* root = nullptr;
		vgui_widget* surface = nullptr;
		vgui_widget* sink = nullptr;

		std::string markdown;
		std::vector<md_block> blocks;
		std::vector<md_line> lines;
		bool truncated = false;

		bool layout_valid = false;
		float layout_widget_w = -1.0f;
		float layout_widget_h = -1.0f;
		float layout_content_h = 0.0f;
		float layout_view_h = 0.0f;
		float max_scroll = 0.0f;
		float scroll = 0.0f;

		float font_size = 0.0f;
		code_highlight_theme theme{};
		float max_media_height = 320.0f;
		int active_line = -1;

		struct position
		{
			int block = 0;
			int offset = 0;
		};
		position anchor;
		position focus;
		bool has_anchor = false;
		bool has_focus = false;
		int drag_mode = 0;

		std::function<void(const std::string&)> clipboard_setter;
		md_media_resolver resolver;

		float scale() const
		{
			return ui_scale_of(root);
		}

		float base_font() const
		{
			return font_size > 0.0f ? font_size : editor_font_size(ctx);
		}

		float pad_left() const { return 12.0f * scale(); }
		float pad_right() const { return 12.0f * scale(); }
		float pad_top() const { return 8.0f * scale(); }
		float pad_bottom() const { return 12.0f * scale(); }
		float scrollbar_width() const { return 10.0f * scale(); }

		float measure(const std::string& text, float logical) const;
		float fallback_measure(const std::string& text, float logical) const;
		float line_height(float logical) const;
		std::vector<std::pair<int, int>> wrap(const std::string& text,
			float max_width, float logical) const;

		vgui_color color_of(code_token_kind kind) const;
		vgui_color background() const;
		vgui_color base_color(const md_block& block) const;

		void layout_now();
		void layout_content(float width);
		void ensure_layout();
		float x_at_offset(const md_block& block, const md_line& line,
			int offset) const;

		void ensure_positions();
		void clear_selection();
		bool ordered_selection(position* first, position* last) const;
		std::string selection_text() const;
		position pos_at(float x, float y);
		int top_source_line() const;
		void scroll_to_y(float y, bool center);

		void refresh_media(md_block& block);
		void refresh_all_media();
		void relayout_preserving_anchor();
		void write_clipboard(const std::string& text);

		void draw(ovg_ctx_cb* cb, rvg_t* vg, float abs_x, float abs_y,
			float abs_w, float abs_h);
		void draw_block_background(ovg_ctx_cb* cb, rvg_t* vg, md_block& block,
			float ox, float oy, float width, float s);
		void draw_line_text(ovg_ctx_cb* cb, rvg_t* vg, md_block& block,
			const md_line& line, float ox, float oy, float content_w,
			float s);

		int handle_event(const vgui_event* event);
		void select_word(position at);
		void select_block(position at);

		static void draw_cb(vgui_widget* widget, ovg_ctx_cb* cb, rvg_t* vg,
			float abs_x, float abs_y, float abs_w, float abs_h,
			float delta_time, void* user);
		static int event_cb(vgui_widget* widget, const vgui_event* event,
			void* user);
	};

	/* ------------------------------------------------------------------------- */
	/* helpers                                                                   */
	/* ------------------------------------------------------------------------- */

	float markdown_view::impl::fallback_measure(const std::string& text,
		float logical) const
	{
		const float font_px = std::max(1.0f, logical * scale());
		double width = 0.0;
		int pos = 0;
		while (pos < (int)text.size())
		{
			int next = 0;
			const uint32_t cp = utf8_decode(text, pos, &next);
			if (text[(std::size_t)pos] == ' ')
				width += 0.30 * font_px;
			else if (cp == '\t')
				width += 2.2 * font_px;
			else if (is_wide_codepoint(cp))
				width += 1.0 * font_px;
			else
				width += 0.55 * font_px;
			pos = next;
		}
		return (float)width;
	}

	float markdown_view::impl::measure(const std::string& text,
		float logical) const
	{
		if (text.empty())
			return 0.0f;
		if (root)
		{
			const float width = vgui_widget_measure_text_sized(root,
				text.c_str(), logical);
			if (width > 0.0f)
				return width;
		}
		return fallback_measure(text, logical);
	}

	float markdown_view::impl::line_height(float logical) const
	{
		if (root)
		{
			const float height = vgui_widget_get_line_height_sized(root, logical);
			if (height > 0.0f)
				return height;
		}
		return std::max(10.0f * scale(), logical * scale() * 1.35f);
	}

	std::vector<std::pair<int, int>> markdown_view::impl::wrap(
		const std::string& text, float max_width, float logical) const
	{
		std::vector<std::pair<int, int>> result;
		if (text.empty())
		{
			result.push_back({ 0, 0 });
			return result;
		}
		if (max_width <= 1.0f)
			max_width = 1.0f;

		struct unit
		{
			int start = 0;
			int end = 0;
			bool space = false;
			bool wide = false;
			float width = 0.0f;
		};
		std::vector<unit> units;
		const int length = (int)text.size();
		int pos = 0;
		while (pos < length)
		{
			int next = 0;
			const uint32_t cp = utf8_decode(text, pos, &next);
			if (is_space_char(text[(std::size_t)pos]))
			{
				int end = next;
				while (end < length &&
					is_space_char(text[(std::size_t)end]))
					end = utf8_next(text, end);
				unit item;
				item.start = pos;
				item.end = end;
				item.space = true;
				item.width = measure(text.substr((std::size_t)pos,
					(std::size_t)(end - pos)), logical);
				units.push_back(item);
				pos = end;
				continue;
			}
			if (is_wide_codepoint(cp))
			{
				unit item;
				item.start = pos;
				item.end = next;
				item.wide = true;
				item.width = measure(text.substr((std::size_t)pos,
					(std::size_t)(next - pos)), logical);
				units.push_back(item);
				pos = next;
				continue;
			}
			int end = next;
			while (end < length)
			{
				int after = 0;
				const uint32_t cp2 = utf8_decode(text, end, &after);
				if (is_space_char(text[(std::size_t)end]) ||
					is_wide_codepoint(cp2))
					break;
				end = after;
			}
			const float width = measure(text.substr((std::size_t)pos,
				(std::size_t)(end - pos)), logical);
			if (width > max_width)
			{
				int split = pos;
				while (split < end)
				{
					const int after = utf8_next(text, split);
					unit item;
					item.start = split;
					item.end = after;
					item.width = measure(text.substr((std::size_t)split,
						(std::size_t)(after - split)), logical);
					units.push_back(item);
					split = after;
				}
			}
			else
			{
				unit item;
				item.start = pos;
				item.end = end;
				item.width = width;
				units.push_back(item);
			}
			pos = end;
		}

		int line_start = -1;
		int last_break = -1;
		float line_width = 0.0f;
		auto flush = [&](int end)
			{
				while (end > line_start &&
					text[(std::size_t)end - 1] == ' ')
					--end;
				result.push_back({ line_start, end });
				line_start = -1;
				last_break = -1;
				line_width = 0.0f;
			};

		for (const unit& item : units)
		{
			if (item.space)
			{
				if (line_start < 0)
					continue;
				line_width += item.width;
				last_break = item.end;
				continue;
			}
			if (line_start < 0)
			{
				line_start = item.start;
				line_width = 0.0f;
				last_break = -1;
			}
			if (line_width + item.width > max_width &&
				item.start > line_start)
			{
				const int end = last_break > line_start ? last_break :
					item.start;
				flush(end);
				line_start = item.start;
				line_width = 0.0f;
			}
			line_width += item.width;
			if (item.wide)
				last_break = item.end;
		}
		if (line_start >= 0)
			flush(length);
		if (result.empty())
			result.push_back({ 0, 0 });
		return result;
	}

	vgui_color markdown_view::impl::color_of(code_token_kind kind) const
	{
		const int index = static_cast<int>(kind);
		if (index >= 0 && index < code_token_kind_count &&
			theme.colors[(std::size_t)index])
			return theme.colors[(std::size_t)index];
		if (kind == code_token_kind::comment ||
			kind == code_token_kind::quote ||
			kind == code_token_kind::list_marker ||
			kind == code_token_kind::table ||
			kind == code_token_kind::horizontal_rule)
			return kFallbackMuted;
		return kFallbackText;
	}

	vgui_color markdown_view::impl::background() const
	{
		return theme.background ? theme.background : kFallbackBg;
	}

	vgui_color markdown_view::impl::base_color(const md_block& block) const
	{
		if (block.kind == md_block_kind::heading)
			return color_of(code_token_kind::heading);
		if (block.kind == md_block_kind::quote)
			return color_of(code_token_kind::quote);
		if (block.kind == md_block_kind::media)
			return color_of(code_token_kind::comment);
		return color_of(code_token_kind::text);
	}

	/* ------------------------------------------------------------------------- */
	/* layout                                                                    */
	/* ------------------------------------------------------------------------- */

	void markdown_view::impl::ensure_layout()
	{
		float width = 600.0f;
		float height = 400.0f;
		if (root)
		{
			float x = 0.0f;
			float y = 0.0f;
			float w = 0.0f;
			float h = 0.0f;
			vgui_widget_get_rect(root, &x, &y, &w, &h);
			if (w > 0.0f)
				width = w;
			if (h > 0.0f)
				height = h;
		}
		if (layout_valid &&
			std::fabs(width - layout_widget_w) < 0.5f &&
			std::fabs(height - layout_widget_h) < 0.5f)
			return;
		layout_widget_w = width;
		layout_widget_h = height;
		layout_view_h = height;
		lines.clear();
		const float content_w = std::max(1.0f, width - pad_left() -
			pad_right() - scrollbar_width());
		layout_content(content_w);
		const float total = pad_top() + layout_content_h + pad_bottom();
		max_scroll = std::max(0.0f, total - height);
		scroll = std::clamp(scroll, 0.0f, max_scroll);
		layout_valid = true;
	}

	void markdown_view::impl::layout_now()
	{
		layout_valid = false;
		ensure_layout();
	}

	void markdown_view::impl::layout_content(float width)
	{
		const float s = scale();
		const float bf = base_font();
		float y = 0.0f;

		/* Intersects block.runs with [start, end) and appends positioned
		 * segments.  clip_right > 0 clips a segment at that x (table cells). */
		auto append_runs = [&](const md_block& block, md_line& line,
			int start, int end, float origin_x, float clip_right,
			const md_style& base)
			{
				int cursor = start;
				float x = origin_x;
				auto emit = [&](int a, int b, const md_style& style)
					{
						if (b <= a)
							return;
						md_segment segment;
						segment.start = a;
						segment.end = b;
						segment.style = style;
						segment.x = x;
						segment.width = measure(block.text.substr(
							(std::size_t)a, (std::size_t)(b - a)),
							block.text_font);
						if (clip_right > 0.0f)
							segment.clip_w = std::max(0.0f,
								clip_right - x);
						x += segment.width;
						line.width = std::max(line.width,
							x - line.x);
						line.segments.push_back(segment);
					};
				for (const md_run& run : block.runs)
				{
					const int a = std::max(run.start, start);
					const int b = std::min(run.start + run.length, end);
					if (b <= a)
						continue;
					if (a > cursor)
						emit(cursor, a, base);
					emit(a, b, run.style);
					cursor = std::max(cursor, b);
				}
				if (cursor < end)
					emit(cursor, end, base);
			};

		for (int index = 0; index < (int)blocks.size(); ++index)
		{
			md_block& block = blocks[(std::size_t)index];
			block.top = y;
			block.line_begin = (int)lines.size();
			block.media_x = block.media_y = 0.0f;
			block.media_w = block.media_h = 0.0f;
			block.caption_y = 0.0f;
			block.column_widths.clear();

			if (block.kind == md_block_kind::paragraph ||
				block.kind == md_block_kind::heading ||
				block.kind == md_block_kind::quote ||
				block.kind == md_block_kind::list_item)
			{
				static const float kHeadingScale[6] =
				{ 1.8f, 1.5f, 1.3f, 1.15f, 1.05f, 1.0f };
				float font = bf;
				if (block.kind == md_block_kind::heading)
				{
					const int level = std::clamp(block.level, 1, 6);
					font *= kHeadingScale[level - 1];
				}
				block.text_font = font;
				block.line_height = line_height(font);

				float text_indent = 0.0f;
				if (block.kind == md_block_kind::quote)
					text_indent = 12.0f * s +
					std::max(0, block.level - 1) * 10.0f * s;
				else if (block.kind == md_block_kind::list_item)
					text_indent = 18.0f * s +
					std::max(0, block.level) * 16.0f * s;

				const float wrap_width = std::max(8.0f,
					width - text_indent);
				const std::vector<std::pair<int, int>> wrapped = wrap(
					block.text, wrap_width, font);

				md_style base;
				if (block.kind == md_block_kind::heading)
				{
					base.color = code_token_kind::heading;
					base.bold = block.level <= 2;
				}
				else if (block.kind == md_block_kind::quote)
					base.color = code_token_kind::quote;
				else
					base.color = code_token_kind::text;

				for (std::size_t i = 0; i < wrapped.size(); ++i)
				{
					md_line line;
					line.block = index;
					line.source_line = block.kind == md_block_kind::heading ?
						block.source_first :
						source_line_for_offset(block, wrapped[i].first);
					line.y = (float)i * block.line_height;
					line.height = block.line_height;
					line.x = text_indent;
					append_runs(block, line, wrapped[i].first,
						wrapped[i].second, text_indent, 0.0f, base);
					lines.push_back(std::move(line));
				}
				block.height = std::max(block.line_height,
					(float)wrapped.size() * block.line_height);
			}
			else if (block.kind == md_block_kind::code)
			{
				block.text_font = bf * 0.95f;
				block.line_height = line_height(block.text_font);
				const float pad = 8.0f * s;
				md_style base;
				base.color = code_token_kind::text;
				for (std::size_t i = 0; i < block.code_lines.size(); ++i)
				{
					const int start = block.code_line_offsets[i];
					const int end = start + (int)block.code_lines[i].size();
					md_line line;
					line.block = index;
					line.source_line = std::min(block.source_first + 1 +
						(int)i, block.source_last);
					line.y = pad + (float)i * block.line_height;
					line.height = block.line_height;
					line.x = pad;
					append_runs(block, line, start, end, pad, 0.0f, base);
					lines.push_back(std::move(line));
				}
				const int rows = std::max(1, (int)block.code_lines.size());
				block.height = pad * 2.0f +
					(float)rows * block.line_height;
			}
			else if (block.kind == md_block_kind::table)
			{
				block.text_font = bf * 0.95f;
				block.line_height = line_height(block.text_font);
				const float pad_x = 8.0f * s;
				const float pad_y = 4.0f * s;
				const float min_col = 24.0f * s;
				std::size_t columns = block.column_align.size();
				for (const md_table_row& row : block.rows)
					columns = std::max(columns, row.cells.size());
				columns = std::max<std::size_t>(columns, 1);
				block.column_widths.assign(columns, min_col);
				for (const md_table_row& row : block.rows)
				{
					for (std::size_t col = 0; col < row.cells.size(); ++col)
					{
						const float text_w = measure(row.cells[col].text,
							block.text_font);
						block.column_widths[col] = std::max(
							block.column_widths[col],
							text_w + 2.0f * pad_x);
					}
				}
				float total = 0.0f;
				for (float col_w : block.column_widths)
					total += col_w;
				if (total > width && total > 0.0f)
				{
					const float factor = width / total;
					total = 0.0f;
					for (float& col_w : block.column_widths)
					{
						col_w = std::max(min_col * 0.6f, col_w * factor);
						total += col_w;
					}
					if (total > width && total > 0.0f)
					{
						const float fit = width / total;
						for (float& col_w : block.column_widths)
							col_w *= fit;
						total = width;
					}
				}
				const float row_h = block.line_height + 2.0f * pad_y;
				for (std::size_t r = 0; r < block.rows.size(); ++r)
				{
					const md_table_row& row = block.rows[r];
					md_line line;
					line.block = index;
					line.source_line = row.source_line;
					line.y = (float)r * row_h;
					line.height = row_h;
					line.x = 0.0f;
					line.width = total;
					float cell_x = 0.0f;
					for (std::size_t col = 0; col < columns; ++col)
					{
						const float col_w = block.column_widths[col];
						if (col < row.cells.size())
						{
							const md_cell& cell = row.cells[col];
							const float text_w = measure(cell.text,
								block.text_font);
							const float inner = std::max(0.0f,
								col_w - 2.0f * pad_x);
							float tx = cell_x + pad_x;
							const int align = col < block.column_align.size() ?
								block.column_align[col] : 0;
							if (align == 1)
								tx += std::max(0.0f,
									(inner - text_w) * 0.5f);
							else if (align == 2)
								tx += std::max(0.0f, inner - text_w);
							md_style base;
							base.color = code_token_kind::text;
							base.bold = row.header;
							append_runs(block, line, cell.offset,
								cell.offset + (int)cell.text.size(),
								tx, cell_x + col_w, base);
						}
						cell_x += col_w;
					}
					lines.push_back(std::move(line));
				}
				block.height = (float)block.rows.size() * row_h;
			}
			else if (block.kind == md_block_kind::media)
			{
				block.text_font = bf * 0.9f;
				block.line_height = line_height(block.text_font);
				float aspect = block.media.aspect;
				if (!(aspect > 0.0f) && block.media.texture &&
					vgui_texture_get_width(block.media.texture) > 0 &&
					vgui_texture_get_height(block.media.texture) > 0)
					aspect = (float)vgui_texture_get_width(block.media.texture) /
					(float)vgui_texture_get_height(block.media.texture);
				if (!(aspect > 0.0f))
					aspect = 16.0f / 9.0f;
				float media_w = width;
				float media_h = media_w / aspect;
				const float limit = std::max(24.0f * s,
					max_media_height * s);
				if (media_h > limit)
				{
					media_h = limit;
					media_w = media_h * aspect;
				}
				if (media_w > width)
				{
					media_w = width;
					media_h = media_w / aspect;
				}
				block.media_x = std::max(0.0f, (width - media_w) * 0.5f);
				block.media_y = 0.0f;
				block.media_w = media_w;
				block.media_h = media_h;

				float caption_y = media_h + 4.0f * s;
				if (!block.text.empty())
				{
					const std::vector<std::pair<int, int>> wrapped = wrap(
						block.text, width, block.text_font);
					md_style base;
					base.color = code_token_kind::comment;
					for (std::size_t i = 0; i < wrapped.size(); ++i)
					{
						md_line line;
						line.block = index;
						line.source_line = block.source_first;
						line.y = caption_y +
							(float)i * block.line_height;
						line.height = block.line_height;
						line.x = block.media_x;
						append_runs(block, line, wrapped[i].first,
							wrapped[i].second, block.media_x, 0.0f, base);
						lines.push_back(std::move(line));
					}
					caption_y += (float)wrapped.size() * block.line_height;
				}
				block.caption_y = caption_y;
				block.height = caption_y;
			}
			else /* rule */
			{
				block.height = 12.0f * s;
			}

			block.line_count = (int)lines.size() - block.line_begin;
			float gap = 10.0f * s;
			if (block.kind == md_block_kind::paragraph)
				gap = 8.0f * s;
			else if (block.kind == md_block_kind::heading)
				gap = 12.0f * s;
			else if (block.kind == md_block_kind::quote)
				gap = 8.0f * s;
			else if (block.kind == md_block_kind::list_item)
				gap = 2.0f * s;
			else if (block.kind == md_block_kind::rule)
				gap = 8.0f * s;
			y = block.top + block.height + gap;
		}

		layout_content_h = blocks.empty() ? 0.0f : y;
	}

	float markdown_view::impl::x_at_offset(const md_block& block,
		const md_line& line, int offset) const
	{
		if (line.segments.empty())
			return line.x;
		if (offset <= line.segments.front().start)
			return line.x + line.segments.front().x;
		for (const md_segment& segment : line.segments)
		{
			if (offset < segment.start)
				return line.x + segment.x;
			if (offset <= segment.end)
			{
				const int prefix = std::clamp(offset - segment.start, 0,
					segment.end - segment.start);
				return line.x + segment.x + measure(block.text.substr(
					(std::size_t)segment.start, (std::size_t)prefix),
					block.text_font);
			}
		}
		const md_segment& last = line.segments.back();
		return line.x + last.x + last.width;
	}

	/* ------------------------------------------------------------------------- */
	/* selection                                                                 */
	/* ------------------------------------------------------------------------- */

	void markdown_view::impl::ensure_positions()
	{
		if (blocks.empty())
		{
			has_anchor = false;
			has_focus = false;
			return;
		}
		const auto clamp_position = [this](position& value)
			{
				value.block = std::clamp(value.block, 0,
					(int)blocks.size() - 1);
				value.offset = std::clamp(value.offset, 0,
					(int)blocks[(std::size_t)value.block].text.size());
			};
		if (has_anchor)
			clamp_position(anchor);
		if (has_focus)
			clamp_position(focus);
	}

	void markdown_view::impl::clear_selection()
	{
		has_anchor = false;
		has_focus = false;
		anchor = position{};
		focus = position{};
	}

	bool markdown_view::impl::ordered_selection(position* first,
		position* last) const
	{
		if (!has_anchor || !has_focus)
			return false;
		const auto clamp_position = [this](position value)
			{
				if (blocks.empty())
					return position{};
				value.block = std::clamp(value.block, 0,
					(int)blocks.size() - 1);
				value.offset = std::clamp(value.offset, 0,
					(int)blocks[(std::size_t)value.block].text.size());
				return value;
			};
		position a = clamp_position(anchor);
		position b = clamp_position(focus);
		if (a.block > b.block || (a.block == b.block && a.offset > b.offset))
			std::swap(a, b);
		if (a.block == b.block && a.offset == b.offset)
			return false;
		if (first)
			*first = a;
		if (last)
			*last = b;
		return true;
	}

	markdown_view::impl::position markdown_view::impl::pos_at(float x, float y)
	{
		ensure_layout();
		position result;
		if (blocks.empty())
			return result;
		const float content_y = y - pad_top() + scroll;
		int index = (int)blocks.size() - 1;
		for (int i = 0; i < (int)blocks.size(); ++i)
		{
			if (content_y < blocks[(std::size_t)i].top +
				std::max(1.0f, blocks[(std::size_t)i].height))
			{
				index = i;
				break;
			}
		}
		md_block& block = blocks[(std::size_t)index];
		result.block = index;
		if (block.line_count <= 0)
			return result;

		const float local = content_y - block.top;
		int line_index = block.line_begin + block.line_count - 1;
		for (int i = block.line_begin;
			i < block.line_begin + block.line_count; ++i)
		{
			const md_line& line = lines[(std::size_t)i];
			if (local < line.y + std::max(1.0f, line.height))
			{
				line_index = i;
				break;
			}
		}
		const md_line& line = lines[(std::size_t)line_index];
		if (line.segments.empty())
			return result;
		const int start = line.segments.front().start;
		const int end = line.segments.back().end;
		if (local < line.y)
		{
			result.offset = start;
			return result;
		}
		if (local >= line.y + line.height)
		{
			result.offset = end;
			return result;
		}
		const float text_x = x - pad_left() - line.x;
		if (text_x <= line.segments.front().x)
		{
			result.offset = start;
			return result;
		}
		int best = start;
		float best_distance = std::fabs(text_x -
			line.segments.front().x);
		for (const md_segment& segment : line.segments)
		{
			int offset = segment.start;
			while (true)
			{
				const float at = segment.x + measure(block.text.substr(
					(std::size_t)segment.start,
					(std::size_t)(offset - segment.start)),
					block.text_font);
				const float distance = std::fabs(text_x - at);
				if (distance < best_distance)
				{
					best_distance = distance;
					best = offset;
				}
				if (offset >= segment.end)
					break;
				offset = std::min(segment.end,
					utf8_next(block.text, offset));
			}
		}
		result.offset = best;
		return result;
	}

	int markdown_view::impl::top_source_line() const
	{
		const_cast<impl*>(this)->ensure_layout();
		for (const md_line& line : lines)
		{
			const md_block& block = blocks[(std::size_t)line.block];
			const float top = pad_top() + block.top + line.y;
			if (top + line.height > scroll + 0.5f)
				return line.source_line;
		}
		if (blocks.empty())
			return 0;
		return blocks.back().source_last;
	}

	void markdown_view::impl::scroll_to_y(float y, bool center)
	{
		ensure_layout();
		if (center)
			y -= layout_view_h * 0.5f;
		scroll = std::clamp(y, 0.0f, max_scroll);
	}

	/* ------------------------------------------------------------------------- */
	/* media                                                                     */
	/* ------------------------------------------------------------------------- */

	void markdown_view::impl::refresh_media(md_block& block)
	{
		if (block.kind != md_block_kind::media)
			return;
		md_media media = block.media;
		media.status = md_media_status::pending;
		media.texture = nullptr;
		media.error.clear();
		if (resolver)
		{
			md_media_info info;
			try
			{
				info = resolver(media.source, media.kind);
			}
			catch (...)
			{
				info = md_media_info{};
				info.status = md_media_status::failed;
				info.error = "resolver failed";
			}
			media.status = info.status;
			media.texture = info.texture;
			media.error = info.error;
			if (info.aspect > 0.0f)
				media.aspect = info.aspect;
			else if (info.pixel_width > 0 && info.pixel_height > 0)
				media.aspect = (float)info.pixel_width /
				(float)info.pixel_height;
			else if (media.texture &&
				vgui_texture_get_width(media.texture) > 0 &&
				vgui_texture_get_height(media.texture) > 0)
				media.aspect = (float)vgui_texture_get_width(media.texture) /
				(float)vgui_texture_get_height(media.texture);
			if (media.status == md_media_status::ready && !media.texture)
			{
				media.status = md_media_status::failed;
				if (media.error.empty())
					media.error = "texture unavailable";
			}
		}
		if (!(media.aspect > 0.0f))
			media.aspect = 16.0f / 9.0f;
		block.media = media;
	}

	void markdown_view::impl::refresh_all_media()
	{
		for (md_block& block : blocks)
			refresh_media(block);
	}

	void markdown_view::impl::relayout_preserving_anchor()
	{
		ensure_layout();
		const int old_line_count = (int)lines.size();
		int anchor_line = -1;
		float anchor_offset = 0.0f;
		int anchor_source = 0;
		int anchor_block = 0;
		for (int i = 0; i < old_line_count; ++i)
		{
			const md_line& line = lines[(std::size_t)i];
			const md_block& block = blocks[(std::size_t)line.block];
			const float top = pad_top() + block.top + line.y;
			if (top + line.height > scroll + 0.5f)
			{
				anchor_line = i;
				anchor_offset = top - scroll;
				anchor_source = line.source_line;
				anchor_block = line.block;
				break;
			}
		}
		layout_valid = false;
		ensure_layout();
		if (anchor_line < 0)
			return;
		for (const md_line& line : lines)
		{
			if (line.block < anchor_block)
				continue;
			if (line.source_line < anchor_source)
				continue;
			const md_block& block = blocks[(std::size_t)line.block];
			const float top = pad_top() + block.top + line.y;
			scroll = std::clamp(top - anchor_offset, 0.0f, max_scroll);
			return;
		}
	}

	void markdown_view::impl::write_clipboard(const std::string& text)
	{
		if (clipboard_setter)
		{
			clipboard_setter(text);
			return;
		}
		if (!sink)
			return;
		vgui_edit_set_text(sink, text.c_str());
		vgui_edit_select_all(sink);
		(void)vgui_edit_copy(sink);
	}

	/* ------------------------------------------------------------------------- */
	/* drawing                                                                   */
	/* ------------------------------------------------------------------------- */

	void markdown_view::impl::draw_block_background(ovg_ctx_cb* cb, rvg_t* vg,
		md_block& block, float ox, float oy, float width, float s)
	{
		const vgui_color bg = background();
		const vgui_theme ui = theme_for(root);
		const vgui_color border = mix_color(bg, ui.panel_border, 0.75f);
		const vgui_color muted = color_of(code_token_kind::comment);
		const float top = oy + block.top;

		if (active_line >= 0 && active_line >= block.source_first &&
			active_line <= block.source_last && block.height > 0.0f)
		{
			const vgui_color accent = ui.accent;
			ovg_tools::fill_rect(cb, vg, ox - 4.0f * s, top,
				width + 8.0f * s, block.height,
				to_ovg(mix_color(bg, accent, 0.10f)));
			ovg_tools::fill_rect(cb, vg, ox - 4.0f * s, top, 2.0f * s,
				block.height, to_ovg(with_alpha(accent, 0xAA)));
		}

		if (block.kind == md_block_kind::quote)
		{
			ovg_tools::fill_round_rect(cb, vg, ox, top + 2.0f * s,
				3.0f * s, std::max(1.0f, block.height - 4.0f * s),
				1.5f * s, to_ovg(with_alpha(muted, 0xB0)));
		}
		else if (block.kind == md_block_kind::code)
		{
			ovg_tools::fill_round_rect(cb, vg, ox - 4.0f * s, top,
				width + 8.0f * s, block.height, 4.0f * s,
				to_ovg(mix_color(bg, color_of(code_token_kind::text), 0.07f)));
			ovg_tools::stroke_round_rect(cb, vg, ox - 4.0f * s, top,
				width + 8.0f * s, block.height, 4.0f * s,
				to_ovg(border), std::max(1.0f, s));
		}
		else if (block.kind == md_block_kind::table)
		{
			float table_w = 0.0f;
			for (float col_w : block.column_widths)
				table_w += col_w;
			table_w = std::min(width, table_w);
			const float row_h = block.line_height + 8.0f * s;
			if (!block.rows.empty())
				ovg_tools::fill_rect(cb, vg, ox, top, table_w, row_h,
					to_ovg(mix_color(bg, color_of(code_token_kind::text),
						0.08f)));
			for (std::size_t r = 1; r < block.rows.size(); ++r)
			{
				const float row_y = top + (float)r * row_h;
				ovg_tools::line(cb, vg, ox, row_y, ox + table_w, row_y,
					to_ovg(with_alpha(border, 0x88)), std::max(1.0f, s));
			}
			ovg_tools::stroke_rect(cb, vg, ox, top, table_w,
				(float)block.rows.size() * row_h,
				to_ovg(border), std::max(1.0f, s));
		}
		else if (block.kind == md_block_kind::rule)
		{
			ovg_tools::line(cb, vg, ox, top + block.height * 0.5f,
				ox + width, top + block.height * 0.5f,
				to_ovg(with_alpha(border, 0xCC)), std::max(1.0f, s));
		}
		else if (block.kind == md_block_kind::media)
		{
			const float mx = ox + block.media_x;
			const float my = top + block.media_y;
			const float mw = block.media_w;
			const float mh = block.media_h;
			if (mw <= 0.0f || mh <= 0.0f)
				return;
			const bool ready = block.media.status == md_media_status::ready &&
				block.media.texture != nullptr;
			if (ready && surface)
			{
				clip_guard clip(cb, vg, mx, my, mw, mh);
				vgui_custom_draw_texture(surface, cb, vg,
					block.media.texture, mx, my, mw, mh, 0xFFFFFFFF);
			}
			else
			{
				const bool failed =
					block.media.status == md_media_status::failed;
				const vgui_color edge = failed ? 0xFFE06C75 : border;
				ovg_tools::fill_round_rect(cb, vg, mx, my, mw, mh, 4.0f * s,
					to_ovg(mix_color(bg, color_of(code_token_kind::text),
						0.05f)));
				ovg_tools::stroke_round_rect(cb, vg, mx, my, mw, mh, 4.0f * s,
					to_ovg(edge), std::max(1.0f, s));
				if (surface && mh > block.line_height * 2.0f &&
					mw > 40.0f * s)
				{
					const std::string label =
						(block.media.kind == md_media_kind::video ?
							"video: " : "image: ") +
						file_name_of(block.media.source);
					const std::string status = failed ?
						(block.media.error.empty() ? "unavailable" :
							block.media.error) : "loading...";
					clip_guard clip(cb, vg, mx, my, mw, mh);
					vgui_custom_draw_text_sized(surface, cb, vg,
						label.c_str(), mx + 8.0f * s,
						my + mh * 0.5f - block.line_height,
						mw - 16.0f * s, block.line_height,
						to_ovg(muted), 0.0f, 0.5f, block.text_font);
					vgui_custom_draw_text_sized(surface, cb, vg,
						status.c_str(), mx + 8.0f * s,
						my + mh * 0.5f, mw - 16.0f * s,
						block.line_height,
						to_ovg(failed ? 0xFFE06C75 :
							with_alpha(muted, 0xAA)),
						0.0f, 0.5f, block.text_font);
				}
			}
			if (block.media.kind == md_media_kind::video)
			{
				const vgui_color accent = ui.accent;
				ovg_tools::fill_circle(cb, vg, mx + mw * 0.5f,
					my + mh * 0.5f, 14.0f * s,
					to_ovg(with_alpha(accent, ready ? 0xB0 : 0x66)));
				ovg_tools::tri_glyph(cb, vg, mx + mw * 0.5f,
					my + mh * 0.5f, 7.0f * s, 0,
					to_ovg(0xFFFFFFFF));
			}
		}
	}

	void markdown_view::impl::draw_line_text(ovg_ctx_cb* cb, rvg_t* vg,
		md_block& block, const md_line& line, float ox, float oy,
		float content_w, float s)
	{
		const float y = oy + block.top + line.y;
		const float x0 = ox + line.x;

		if (block.kind == md_block_kind::list_item &&
			&line == &lines[(std::size_t)block.line_begin])
		{
			const float cy = y + line.height * 0.5f;
			const vgui_color marker = color_of(code_token_kind::list_marker);
			if (block.task)
			{
				const float size = 10.0f * s;
				const float cx = x0 - 14.0f * s;
				ovg_tools::stroke_rect(cb, vg, cx - size * 0.5f,
					cy - size * 0.5f, size, size,
					to_ovg(marker), std::max(1.0f, s));
				if (block.checked)
					ovg_tools::check(cb, vg, cx, cy, size * 0.8f,
						to_ovg(color_of(code_token_kind::text)),
						std::max(1.0f, 1.6f * s));
			}
			else if (block.ordered)
			{
				const std::string ordinal = std::to_string(block.ordinal) + ".";
				if (surface)
					vgui_custom_draw_text_sized(surface, cb, vg,
						ordinal.c_str(), x0 - 28.0f * s, y,
						20.0f * s, line.height, to_ovg(marker), 1.0f,
						0.5f, block.text_font);
			}
			else
			{
				ovg_tools::fill_circle(cb, vg, x0 - 11.0f * s, cy,
					2.2f * s, to_ovg(marker));
			}
		}

		if (!surface)
			return;
		const vgui_color bg = background();
		for (const md_segment& segment : line.segments)
		{
			const int length = segment.end - segment.start;
			if (length <= 0)
				continue;
			const std::string text = block.text.substr(
				(std::size_t)segment.start, (std::size_t)length);
			if (text.empty())
				continue;
			const vgui_color color = color_of(segment.style.color);
			const float x = x0 + segment.x;
			float width = segment.clip_w > 0.0f ? segment.clip_w :
				std::max(1.0f, segment.width + 2.0f * s);
			width = std::min(width, std::max(1.0f,
				content_w - segment.x));
			if (width < 1.0f)
				continue;

			if (segment.style.code)
			{
				const float chip_w = std::min(
					std::max(segment.width + 4.0f * s, 2.0f * s),
					width + 4.0f * s);
				ovg_tools::fill_round_rect(cb, vg, x - 2.0f * s,
					y + line.height * 0.12f, chip_w,
					line.height * 0.76f, 2.0f * s,
					to_ovg(mix_color(bg, color, 0.15f)));
			}
			if (segment.style.strike)
				ovg_tools::line(cb, vg, x, y + line.height * 0.55f,
					x + std::min(segment.width, width),
					y + line.height * 0.55f, to_ovg(color),
					std::max(1.0f, s));

			vgui_custom_draw_text_sized(surface, cb, vg, text.c_str(), x,
				y, width, line.height, to_ovg(color), 0.0f, 0.5f,
				block.text_font);
			if (segment.style.bold && width > 1.0f)
				vgui_custom_draw_text_sized(surface, cb, vg,
					text.c_str(), x + 0.6f * s, y,
					std::max(1.0f, width - 0.6f * s), line.height,
					to_ovg(color), 0.0f, 0.5f, block.text_font);
			if (segment.style.link)
				ovg_tools::line(cb, vg, x, y + line.height * 0.84f,
					x + std::min(segment.width, width),
					y + line.height * 0.84f,
					to_ovg(with_alpha(color, 0xCC)),
					std::max(1.0f, s));
		}
	}

	void markdown_view::impl::draw(ovg_ctx_cb* cb, rvg_t* vg, float abs_x,
		float abs_y, float abs_w, float abs_h)
	{
		ensure_layout();
		if (!cb || !vg)
			return;
		const float s = scale();
		const vgui_theme ui = theme_for(root);
		ovg_tools::fill_rect(cb, vg, abs_x, abs_y, abs_w, abs_h,
			to_ovg(background()));
		if (blocks.empty())
			return;

		clip_guard clip(cb, vg, abs_x, abs_y, abs_w, abs_h);
		const float ox = abs_x + pad_left();
		const float oy = abs_y + pad_top() - scroll;
		const float content_w = std::max(1.0f, abs_w - pad_left() -
			pad_right() - scrollbar_width());

		for (md_block& block : blocks)
		{
			const float top = oy + block.top;
			if (top > abs_y + abs_h)
				break;
			if (top + block.height < abs_y)
				continue;
			draw_block_background(cb, vg, block, ox, oy, content_w, s);
		}

		position sel_first;
		position sel_last;
		if (ordered_selection(&sel_first, &sel_last))
		{
			const vgui_color selection = ui.edit_selection;
			for (const md_line& line : lines)
			{
				if (line.block < sel_first.block ||
					line.block > sel_last.block)
					continue;
				const md_block& block = blocks[(std::size_t)line.block];
				const float top = oy + block.top + line.y;
				if (top > abs_y + abs_h)
					break;
				if (top + line.height < abs_y || line.segments.empty())
					continue;
				int from = 0;
				int to = (int)block.text.size();
				if (line.block == sel_first.block)
					from = sel_first.offset;
				if (line.block == sel_last.block)
					to = sel_last.offset;
				const int line_start = line.segments.front().start;
				const int line_end = line.segments.back().end;
				if (to <= line_start || from >= line_end)
					continue;
				float x1 = 0.0f;
				float x2 = 0.0f;
				if (from <= line_start && to >= line_end)
				{
					x1 = line.x;
					x2 = line.x + line.width;
				}
				else
				{
					const int a = std::max(from, line_start);
					const int b = std::min(to, line_end);
					if (a >= b)
						continue;
					x1 = x_at_offset(block, line, a);
					x2 = x_at_offset(block, line, b);
					if (x2 < x1)
						std::swap(x1, x2);
				}
				if (x2 - x1 > 0.5f)
					ovg_tools::fill_rect(cb, vg, ox + x1, top,
						x2 - x1, line.height, to_ovg(selection));
			}
		}

		for (const md_line& line : lines)
		{
			md_block& block = blocks[(std::size_t)line.block];
			const float top = oy + block.top + line.y;
			if (top > abs_y + abs_h)
				break;
			if (top + line.height < abs_y)
				continue;
			draw_line_text(cb, vg, block, line, ox, oy, content_w, s);
		}

		if (max_scroll > 0.5f)
		{
			const float bar_w = scrollbar_width();
			const float track_x = abs_x + abs_w - bar_w + 2.0f * s;
			const float track_y = abs_y + 2.0f * s;
			const float track_h = std::max(1.0f, abs_h - 4.0f * s);
			const float total = pad_top() + layout_content_h + pad_bottom();
			const float thumb_h = std::clamp(
				track_h * layout_view_h / std::max(1.0f, total),
				24.0f * s, track_h);
			const float travel = std::max(0.0f, track_h - thumb_h);
			const float t = max_scroll > 0.0f ?
				std::clamp(scroll / max_scroll, 0.0f, 1.0f) : 0.0f;
			ovg_tools::fill_round_rect(cb, vg, track_x,
				track_y + t * travel, std::max(2.0f, bar_w - 4.0f * s),
				thumb_h, std::max(1.0f, (bar_w - 4.0f * s) * 0.5f),
				to_ovg(ui.scrollbar));
		}

		if (ctx && surface && vgui_get_focus(ctx) == surface)
		{
			const vgui_color accent = ui.accent;
			ovg_tools::stroke_round_rect(cb, vg, abs_x + 1.0f, abs_y + 1.0f,
				std::max(1.0f, abs_w - 2.0f), std::max(1.0f, abs_h - 2.0f),
				2.0f * s, to_ovg(with_alpha(accent, 0x66)),
				std::max(1.0f, s));
		}
	}

	/* ------------------------------------------------------------------------- */
	/* events                                                                    */
	/* ------------------------------------------------------------------------- */

	void markdown_view::impl::select_word(position at)
	{
		ensure_layout();
		if (blocks.empty())
			return;
		at.block = std::clamp(at.block, 0, (int)blocks.size() - 1);
		const md_block& block = blocks[(std::size_t)at.block];
		const std::string& text = block.text;
		const int length = (int)text.size();
		int begin = std::clamp(at.offset, 0, length);
		int end = begin;
		const auto is_word = [](unsigned char c)
			{
				return std::isalnum(c) != 0 || c == '_' || c >= 0x80;
			};
		if (begin < length && is_word((unsigned char)text[(std::size_t)begin]))
		{
			while (begin > 0 &&
				is_word((unsigned char)text[(std::size_t)begin - 1]))
				--begin;
			while (end < length &&
				is_word((unsigned char)text[(std::size_t)end]))
				++end;
		}
		else if (begin < length)
		{
			end = std::min(length, utf8_next(text, begin));
		}
		anchor = position{ at.block, begin };
		focus = position{ at.block, end };
		has_anchor = true;
		has_focus = true;
	}

	void markdown_view::impl::select_block(position at)
	{
		ensure_layout();
		if (blocks.empty())
			return;
		at.block = std::clamp(at.block, 0, (int)blocks.size() - 1);
		anchor = position{ at.block, 0 };
		focus = position{ at.block,
			(int)blocks[(std::size_t)at.block].text.size() };
		has_anchor = true;
		has_focus = true;
	}

	int markdown_view::impl::handle_event(const vgui_event* event)
	{
		if (!event)
			return 0;
		const int type = event->type;
		ensure_layout();

		const auto over_scrollbar = [this](float x)
			{
				return max_scroll > 0.5f && layout_widget_w > 0.0f &&
					x >= layout_widget_w - scrollbar_width();
			};
		const auto cursor_for = [this, &over_scrollbar](float x)
			{
				if (surface)
					vgui_widget_set_cursor(surface, over_scrollbar(x) ?
						VGUI_CURSOR_ARROW : VGUI_CURSOR_IBEAM);
			};
		const auto drag_scrollbar = [this](float y)
			{
				const float ratio = std::clamp(
					y / std::max(1.0f, layout_view_h), 0.0f, 1.0f);
				scroll = ratio * max_scroll;
			};

		if (type == VGUI_EVENT_MOUSE_WHEEL)
		{
			const float delta = event->wheel_y != 0.0f ?
				event->wheel_y : event->wheel_x;
			if (delta == 0.0f)
				return 0;
			const float step = line_height(base_font()) * 3.0f;
			scroll = std::clamp(scroll - delta * step, 0.0f, max_scroll);
			return 1;
		}

		if (type == VGUI_EVENT_MOUSE_DOWN)
		{
			if (event->button != 1)
				return 0;
			if (ctx && surface)
				vgui_set_focus(ctx, surface);
			if (over_scrollbar(event->x))
			{
				drag_mode = 2;
				drag_scrollbar(event->y);
				if (surface)
					vgui_widget_set_cursor(surface, VGUI_CURSOR_ARROW);
				return 1;
			}
			const position at = pos_at(event->x, event->y);
			anchor = at;
			focus = at;
			has_anchor = true;
			has_focus = true;
			drag_mode = 1;
			return 1;
		}

		if (type == VGUI_EVENT_DOUBLE_CLICK ||
			type == VGUI_EVENT_TRIPLE_CLICK)
		{
			if (event->button != 0 && event->button != 1)
				return 0;
			if (ctx && surface)
				vgui_set_focus(ctx, surface);
			const position at = pos_at(event->x, event->y);
			if (type == VGUI_EVENT_DOUBLE_CLICK)
				select_word(at);
			else
				select_block(at);
			drag_mode = 1;
			return 1;
		}

		if (type == VGUI_EVENT_MOUSE_MOVE)
		{
			if (drag_mode == 1)
			{
				focus = pos_at(event->x, event->y);
				has_focus = true;
				return 1;
			}
			if (drag_mode == 2)
			{
				drag_scrollbar(event->y);
				return 1;
			}
			cursor_for(event->x);
			return 0;
		}

		if (type == VGUI_EVENT_MOUSE_UP)
		{
			if (drag_mode != 0)
			{
				drag_mode = 0;
				cursor_for(event->x);
				return 1;
			}
			return 0;
		}

		if (type != VGUI_EVENT_KEY_DOWN)
			return 0;

		const bool ctrl = (event->mods & (VGUI_MOD_CTRL | VGUI_MOD_GUI)) != 0;
		const int key = event->key;
		if (ctrl)
		{
			switch (key)
			{
			case 'a':
			case 'A':
				if (!blocks.empty())
				{
					anchor = position{ 0, 0 };
					focus = position{ (int)blocks.size() - 1,
						(int)blocks.back().text.size() };
					has_anchor = true;
					has_focus = true;
				}
				return 1;
			case 'c':
			case 'C':
			{
				const std::string text = selection_text();
				if (!text.empty())
					write_clipboard(text);
				return 1;
			}
			case VGUI_KEY_HOME:
				scroll = 0.0f;
				return 1;
			case VGUI_KEY_END:
				scroll = max_scroll;
				return 1;
			default:
				break;
			}
			return 0;
		}

		const float page = layout_view_h * 0.9f;
		switch (key)
		{
		case VGUI_KEY_PAGE_UP:
			scroll = std::clamp(scroll - page, 0.0f, max_scroll);
			return 1;
		case VGUI_KEY_PAGE_DOWN:
			scroll = std::clamp(scroll + page, 0.0f, max_scroll);
			return 1;
		case VGUI_KEY_HOME:
			scroll = 0.0f;
			return 1;
		case VGUI_KEY_END:
			scroll = max_scroll;
			return 1;
		case VGUI_KEY_UP:
			scroll = std::clamp(scroll - line_height(base_font()), 0.0f,
				max_scroll);
			return 1;
		case VGUI_KEY_DOWN:
			scroll = std::clamp(scroll + line_height(base_font()), 0.0f,
				max_scroll);
			return 1;
		case VGUI_KEY_ESCAPE:
			clear_selection();
			return 1;
		default:
			break;
		}
		return 0;
	}

	void markdown_view::impl::draw_cb(vgui_widget* widget, ovg_ctx_cb* cb,
		rvg_t* vg, float abs_x, float abs_y, float abs_w, float abs_h,
		float delta_time, void* user)
	{
		(void)widget;
		(void)delta_time;
		auto* self = static_cast<impl*>(user);
		if (self)
			self->draw(cb, vg, abs_x, abs_y, abs_w, abs_h);
	}

	int markdown_view::impl::event_cb(vgui_widget* widget,
		const vgui_event* event, void* user)
	{
		(void)widget;
		auto* self = static_cast<impl*>(user);
		return self ? self->handle_event(event) : 0;
	}

	/* ------------------------------------------------------------------------- */
	/* markdown_view                                                             */
	/* ------------------------------------------------------------------------- */

	markdown_view::markdown_view() : impl_(std::make_unique<impl>())
	{
		code_highlight_theme theme;
		if (code_highlight_theme_preset("dark", theme))
			impl_->theme = theme;
	}

	markdown_view::~markdown_view() = default;

	void markdown_view::build(vgui_widget* parent)
	{
		if (!impl_ || impl_->root || !parent)
			return;
		impl* self = impl_.get();
		self->ctx = vgui_widget_get_context(parent);
		self->root = vgui_widget_add(parent, VGUI_PANEL, nullptr);
		vgui_widget_set_id(self->root, "markdown_view");
		vgui_widget_set_a11y_name(self->root, "Markdown preview");
		vgui_widget_set_grow(self->root, 1.0f);
		vgui_widget_set_min_size(self->root, 160, 80);
		vgui_widget_set_size(self->root, 0, 0);
		vgui_widget_set_align_self(self->root, VGUI_ALIGN_STRETCH);
		vgui_widget_set_layout(self->root, VGUI_LAYOUT_COLUMN);
		vgui_widget_set_align_items(self->root, VGUI_ALIGN_STRETCH);
		vgui_widget_set_padding(self->root, 0, 0, 0, 0);
		vgui_widget_set_gap(self->root, 0);
		vgui_widget_set_background(self->root, 0);
		vgui_widget_set_border(self->root, 0, 0);

		self->surface = vgui_widget_add(self->root, VGUI_CUSTOM, nullptr);
		vgui_widget_set_id(self->surface, "markdown_view_surface");
		vgui_widget_set_grow(self->surface, 1.0f);
		vgui_widget_set_min_size(self->surface, 80, 40);
		vgui_widget_set_size(self->surface, 0, 0);
		vgui_widget_set_align_self(self->surface, VGUI_ALIGN_STRETCH);
		vgui_widget_set_focusable(self->surface, 1);
		vgui_widget_set_cursor(self->surface, VGUI_CURSOR_IBEAM);
		vgui_custom_set_draw(self->surface, &impl::draw_cb, self);
		vgui_custom_set_event(self->surface, &impl::event_cb, self);
		vgui_custom_set_clip_content(self->surface, 1);

		self->sink = vgui_widget_add(self->root, VGUI_EDIT, nullptr);
		vgui_widget_set_id(self->sink, "markdown_view_clipboard");
		vgui_widget_set_size(self->sink, 1, 1);
		vgui_widget_set_visible(self->sink, 0);

		self->layout_valid = false;
	}

	bool markdown_view::built() const noexcept
	{
		return impl_ && impl_->root != nullptr;
	}

	vgui_widget* markdown_view::widget() const noexcept
	{
		return impl_ ? impl_->root : nullptr;
	}

	void markdown_view::set_markdown(std::string text)
	{
		if (!impl_)
			return;
		impl* self = impl_.get();
		const int top_line = self->layout_valid ? self->top_source_line() : 0;
		self->truncated = false;
		if (text.size() > kMaxMarkdownBytes)
		{
			std::size_t cut = kMaxMarkdownBytes;
			while (cut > 0 && ((unsigned char)text[cut] & 0xC0) == 0x80)
				--cut;
			text.resize(cut);
			self->truncated = true;
		}
		self->markdown = std::move(text);
		self->blocks.clear();
		self->lines.clear();
		parse_document(self->markdown, self->blocks);
		self->refresh_all_media();
		self->clear_selection();
		self->active_line = -1;
		self->drag_mode = 0;
		self->layout_valid = false;
		if (!self->root)
			return;
		self->ensure_layout();
		if (top_line > 0)
		{
			for (const md_line& line : self->lines)
			{
				if (line.source_line < top_line)
					continue;
				const md_block& block = self->blocks[(std::size_t)line.block];
				self->scroll = std::clamp(self->pad_top() + block.top +
					line.y, 0.0f, self->max_scroll);
				break;
			}
		}
	}

	const std::string& markdown_view::markdown() const noexcept
	{
		static const std::string empty;
		return impl_ ? impl_->markdown : empty;
	}

	bool markdown_view::truncated() const noexcept
	{
		return impl_ && impl_->truncated;
	}

	std::string markdown_view::rendered_text() const
	{
		if (!impl_)
			return {};
		std::string out;
		for (const md_block& block : impl_->blocks)
		{
			if (block.text.empty())
				continue;
			if (!out.empty())
				out.push_back('\n');
			out += block.text;
		}
		return out;
	}

	void markdown_view::set_font_size(float size)
	{
		if (!impl_)
			return;
		if (!std::isfinite(size) || size < 0.0f)
			size = 0.0f;
		impl_->font_size = size;
		if (impl_->root)
			impl_->relayout_preserving_anchor();
		else
			impl_->layout_valid = false;
	}

	float markdown_view::font_size() const noexcept
	{
		return impl_ ? impl_->font_size : 0.0f;
	}

	void markdown_view::set_highlight_theme(const code_highlight_theme& theme)
	{
		if (impl_)
			impl_->theme = theme;
	}

	const code_highlight_theme& markdown_view::highlight_theme() const noexcept
	{
		static const code_highlight_theme empty;
		return impl_ ? impl_->theme : empty;
	}

	bool markdown_view::set_highlight_preset(const std::string& name)
	{
		if (!impl_)
			return false;
		code_highlight_theme theme;
		if (!code_highlight_theme_preset(name, theme))
			return false;
		impl_->theme = theme;
		return true;
	}

	float markdown_view::content_height() const
	{
		if (!impl_)
			return 0.0f;
		impl_->ensure_layout();
		return impl_->pad_top() + impl_->layout_content_h +
			impl_->pad_bottom();
	}

	void markdown_view::set_scroll(float y)
	{
		if (!impl_)
			return;
		impl_->ensure_layout();
		impl_->scroll = std::clamp(y, 0.0f, impl_->max_scroll);
	}

	float markdown_view::scroll_y() const noexcept
	{
		return impl_ ? impl_->scroll : 0.0f;
	}

	void markdown_view::scroll_to_block(int block, bool center)
	{
		if (!impl_)
			return;
		impl* self = impl_.get();
		self->ensure_layout();
		if (self->blocks.empty())
			return;
		block = std::clamp(block, 0, (int)self->blocks.size() - 1);
		const md_block& target = self->blocks[(std::size_t)block];
		float y = target.top;
		if (center)
			y += target.height * 0.5f - self->layout_view_h * 0.5f;
		self->scroll_to_y(y, false);
	}

	void markdown_view::scroll_to_source_line(int line, bool center)
	{
		if (!impl_)
			return;
		impl* self = impl_.get();
		self->ensure_layout();
		if (self->blocks.empty())
			return;
		int index = block_for_source_line(line);
		if (index < 0)
			index = line < self->blocks.front().source_first ? 0 :
			(int)self->blocks.size() - 1;
		const md_block& target = self->blocks[(std::size_t)index];
		float y = target.top;
		for (int i = target.line_begin;
			i < target.line_begin + target.line_count; ++i)
		{
			const md_line& text_line = self->lines[(std::size_t)i];
			if (text_line.source_line >= line)
			{
				y = target.top + text_line.y;
				break;
			}
		}
		if (center)
			y += (target.line_count > 0 ? target.line_height * 0.5f :
				target.height * 0.5f) - self->layout_view_h * 0.5f;
		self->scroll_to_y(y, false);
	}

	int markdown_view::block_count() const noexcept
	{
		return impl_ ? (int)impl_->blocks.size() : 0;
	}

	int markdown_view::block_source_line(int block) const noexcept
	{
		if (!impl_ || impl_->blocks.empty())
			return 0;
		block = std::clamp(block, 0, (int)impl_->blocks.size() - 1);
		return impl_->blocks[(std::size_t)block].source_first;
	}

	int markdown_view::block_source_end_line(int block) const noexcept
	{
		if (!impl_ || impl_->blocks.empty())
			return 0;
		block = std::clamp(block, 0, (int)impl_->blocks.size() - 1);
		return impl_->blocks[(std::size_t)block].source_last;
	}

	int markdown_view::block_for_source_line(int line) const noexcept
	{
		if (!impl_)
			return -1;
		for (int i = 0; i < (int)impl_->blocks.size(); ++i)
		{
			const md_block& block = impl_->blocks[(std::size_t)i];
			if (line >= block.source_first && line <= block.source_last)
				return i;
		}
		return -1;
	}

	int markdown_view::source_line_at(float x, float y) const
	{
		if (!impl_)
			return 0;
		impl* self = impl_.get();
		const impl::position at = self->pos_at(x, y);
		if (self->blocks.empty())
			return 0;
		const md_block& block = self->blocks[(std::size_t)at.block];
		if (block.kind == md_block_kind::paragraph ||
			block.kind == md_block_kind::quote ||
			block.kind == md_block_kind::list_item)
			return source_line_for_offset(block, at.offset);
		for (int i = block.line_begin;
			i < block.line_begin + block.line_count; ++i)
		{
			const md_line& text_line = self->lines[(std::size_t)i];
			const int end = text_line.segments.empty() ? 0 :
				text_line.segments.back().end;
			if (at.offset <= end)
				return text_line.source_line;
		}
		return block.source_first;
	}

	void markdown_view::set_active_source_line(int line)
	{
		if (impl_)
			impl_->active_line = line;
	}

	int markdown_view::active_source_line() const noexcept
	{
		return impl_ ? impl_->active_line : -1;
	}

	bool markdown_view::has_selection() const noexcept
	{
		return impl_ && impl_->ordered_selection(nullptr, nullptr);
	}

	void markdown_view::select_all()
	{
		if (!impl_ || impl_->blocks.empty())
			return;
		impl_->ensure_layout();
		impl_->anchor = impl::position{ 0, 0 };
		impl_->focus = impl::position{
			(int)impl_->blocks.size() - 1,
			(int)impl_->blocks.back().text.size() };
		impl_->has_anchor = true;
		impl_->has_focus = true;
	}

	void markdown_view::clear_selection()
	{
		if (impl_)
			impl_->clear_selection();
	}

	std::string markdown_view::impl::selection_text() const
	{
		impl* self = const_cast<impl*>(this);
		self->ensure_layout();
		impl::position first;
		impl::position last;
		if (!self->ordered_selection(&first, &last))
			return {};
		std::string out;
		for (int index = first.block; index <= last.block; ++index)
		{
			const md_block& block = self->blocks[(std::size_t)index];
			const int from = index == first.block ? first.offset : 0;
			const int to = index == last.block ? last.offset :
				(int)block.text.size();
			if (from >= to)
				continue;
			std::string piece;
			if (block.line_count > 0)
			{
				int previous_end = -1;
				bool first_piece = true;
				for (int i = block.line_begin;
					i < block.line_begin + block.line_count; ++i)
				{
					const md_line& line = self->lines[(std::size_t)i];
					if (line.segments.empty())
						continue;
					const int line_start = line.segments.front().start;
					const int line_end = line.segments.back().end;
					if (line_end <= from || line_start >= to)
						continue;
					const int a = std::max(from, line_start);
					const int b = std::min(to, line_end);
					if (a >= b)
						continue;
					if (!first_piece)
					{
						if (block.kind == md_block_kind::code ||
							block.kind == md_block_kind::table ||
							block.kind == md_block_kind::media)
							piece.push_back('\n');
						else if (previous_end >= 0 &&
							line_start > previous_end)
						{
							bool spaces = true;
							for (int c = previous_end; c < line_start; ++c)
							{
								if (!is_space_char(block.text[(std::size_t)c]))
								{
									spaces = false;
									break;
								}
							}
							piece.push_back(spaces ? ' ' : '\n');
						}
					}
					piece.append(block.text, (std::size_t)a,
						(std::size_t)(b - a));
					previous_end = b;
					first_piece = false;
				}
			}
			else if (!block.text.empty())
			{
				const int a = std::clamp(from, 0, (int)block.text.size());
				const int b = std::clamp(to, a, (int)block.text.size());
				piece = block.text.substr((std::size_t)a,
					(std::size_t)(b - a));
			}
			if (piece.empty())
				continue;
			if (!out.empty() && out.back() != '\n')
				out.push_back('\n');
			out += piece;
		}
		while (!out.empty() && (out.back() == '\n' || out.back() == ' ' ||
			out.back() == '\t'))
			out.pop_back();
		return out;
	}

	std::string markdown_view::selected_text() const
	{
		return impl_ ? impl_->selection_text() : std::string();
	}

	bool markdown_view::copy()
	{
		if (!impl_)
			return false;
		const std::string text = selected_text();
		if (text.empty())
			return false;
		impl_->write_clipboard(text);
		return true;
	}

	void markdown_view::set_clipboard_setter(
		std::function<void(const std::string&)> setter)
	{
		if (impl_)
			impl_->clipboard_setter = std::move(setter);
	}

	void markdown_view::set_media_resolver(md_media_resolver resolver)
	{
		if (!impl_)
			return;
		impl_->resolver = std::move(resolver);
		impl_->refresh_all_media();
		if (impl_->root)
			impl_->relayout_preserving_anchor();
		else
			impl_->layout_valid = false;
	}

	void markdown_view::invalidate_media(const std::string& source)
	{
		if (!impl_)
			return;
		bool changed = false;
		for (md_block& block : impl_->blocks)
		{
			if (block.kind != md_block_kind::media)
				continue;
			if (!source.empty() && block.media.source != source)
				continue;
			impl_->refresh_media(block);
			changed = true;
		}
		if (!changed)
			return;
		if (impl_->root)
			impl_->relayout_preserving_anchor();
		else
			impl_->layout_valid = false;
	}

	void markdown_view::set_max_media_height(float logical_height)
	{
		if (!impl_)
			return;
		impl_->max_media_height = std::isfinite(logical_height) &&
			logical_height > 0.0f ? logical_height : 320.0f;
		if (impl_->root)
			impl_->relayout_preserving_anchor();
		else
			impl_->layout_valid = false;
	}

	float markdown_view::max_media_height() const noexcept
	{
		return impl_ ? impl_->max_media_height : 320.0f;
	}

} // namespace vgui_editor_kit

