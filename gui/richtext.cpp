/* Core rich-text primitives: turn UTF-8 text + byte spans into styled runs. */
#include "pch.h"
#include <vgui/richtext.h>
#include "vgui_text_draw.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <ovg_c.h>
#include <unicode/ubrk.h>
#include <unicode/utext.h>
#include "tools/tools.hpp"

namespace
{
	constexpr unsigned decor_bits[] = {
		VGUI_TEXT_DECOR_UNDERLINE, VGUI_TEXT_DECOR_STRIKETHROUGH, VGUI_TEXT_DECOR_OVERLINE
	};
	enum { paint_channel = 0, underline_channel, strike_channel, overline_channel, glow_channel, channel_count };

	bool has_alpha(vgui_color color) { return (color >> 24) != 0; }

	using span_sources = std::array<const vgui_text_span*, channel_count>;

	struct span_event
	{
		int64_t pos;
		int index;
		bool enter;
	};

	bool is_utf8_continuation(unsigned char c) { return (c & 0xC0) == 0x80; }

	// One sweep for paint, independent decoration bits, and glow. Keeping the
	// active spans ordered by original index makes priority independent of start.
	template<class Emit>
	void walk_intervals(const char* text, int64_t len, const vgui_text_span* spans,
		int span_count, bool for_draw, Emit emit)
	{
		std::vector<span_event> events;
		for (int i = 0; spans && i < span_count; ++i)
		{
			const auto& span = spans[i];
			if (span.length <= 0) continue;
			int64_t start = std::max<int64_t>(0, span.start);
			int64_t end = std::min(len, (int64_t)span.start + span.length);
			if (end <= start) continue;
			if (text)
			{
				while (start > 0 && is_utf8_continuation((unsigned char)text[start])) --start;
				while (end < len && is_utf8_continuation((unsigned char)text[end])) ++end;
			}
			events.push_back({start, i, true});
			events.push_back({end, i, false});
		}
		span_sources sources = {};
		if (events.empty())
		{
			if (len > 0) emit(0, len, sources);
			return;
		}

		std::sort(events.begin(), events.end(), [](const auto& a, const auto& b) { return a.pos < b.pos; });
		std::array<std::set<int>, channel_count> active;
		int64_t start = 0;
		for (size_t i = 0; i < events.size();)
		{
			const int64_t end = events[i].pos;
			if (end > start) emit(start, end, sources);
			do
			{
				const auto& e = events[i++];
				const auto& span = spans[e.index];
				auto update = [&](int channel) {
					auto& indices = active[channel];
					if (e.enter) indices.insert(e.index);
					else indices.erase(e.index);
					sources[channel] = indices.empty() ? nullptr : spans + *indices.rbegin();
				};
				update(paint_channel);
				if (for_draw)
				{
					for (int k = 0; k < 3; ++k)
						if (span.decor & decor_bits[k]) update(underline_channel + k);
					if (has_alpha(span.glow.color) || (span.glow.flags & VGUI_TEXT_GLOW_OFF)) update(glow_channel);
				}
			} while (i < events.size() && events[i].pos == end);
			start = end;
		}
		if (start < len) emit(start, len, sources);
	}

	// Split resolved spans at line breaks without changing their byte indices.
	// Keep CRLF together even if a span boundary falls between its bytes.
	template<class EmitRun, class FinishLine>
	void walk_lines(const char* text, const vgui_text_span* spans, int span_count,
		bool for_draw, EmitRun emit_run, FinishLine finish_line)
	{
		const char* newline = std::strchr(text, '\n');
		auto content_end = [&] { return newline - (newline > text && newline[-1] == '\r'); };
		walk_intervals(text, std::strlen(text), spans, span_count, for_draw,
			[&](int64_t start, int64_t end, const auto& sources) {
				const char* first = text + start;
				const char* last = text + end;
				while (newline && newline < last)
				{
					const char* stop = content_end();
					if (first < stop) emit_run(first, stop, sources);
					finish_line();
					first = newline + 1;
					newline = std::strchr(first, '\n');
				}
				if (newline) last = std::min(last, content_end());
				if (first < last) emit_run(first, last, sources);
			});
		finish_line();
	}

	struct font_style
	{
		float size = 0;
		int weight = 0;
		int slant = 0;
	};

	font_style font_for(const vgui_text_span* span, float base_size)
	{
		if (!span) return {base_size, 0, 0};
		return {span->font_size > 0.0f ? span->font_size : base_size,
			span->font_weight, span->font_slant};
	}

	struct styled_range
	{
		const char* first;
		const char* last;
		span_sources sources;
		font_style font;
	};

	// ICU operates on the original UTF-8 paragraph so neither a change of
	// style nor a byte offset inside an extended grapheme is a line break.
	template<class EmitLine>
	void wrap_paragraph(vgui_widget* widget, const std::vector<styled_range>& ranges,
		float max_width, EmitLine emit_line)
	{
		const char* first = ranges.front().first;
		const char* last = ranges.back().last;
		std::string scratch;
		auto advance = [&](const char* a, const char* b, font_style font) {
			scratch.assign(a, b);
			return vgui_widget_measure_text_styled(widget, scratch.c_str(), font.size, font.weight, font.slant);
		};
		double width = 0;
		for (const auto& r : ranges) width += advance(r.first, r.last, r.font);
		if (width <= max_width || last - first > std::numeric_limits<int32_t>::max())
		{
			emit_line(first, last);
			return;
		}

		UErrorCode status = U_ZERO_ERROR;
		std::unique_ptr<UText, decltype(&utext_close)> input(
			utext_openUTF8(nullptr, first, last - first, &status), utext_close);
		auto breaker = [&](UBreakIteratorType kind) {
			std::unique_ptr<UBreakIterator, decltype(&ubrk_close)> it(
				ubrk_open(kind, "root", nullptr, 0, &status), ubrk_close);
			if (U_SUCCESS(status)) ubrk_setUText(it.get(), input.get(), &status);
			return it;
		};
		auto line_breaks = breaker(UBRK_LINE);
		auto graphemes = breaker(UBRK_CHARACTER);
		if (U_FAILURE(status))
		{
			emit_line(first, last); // Keep the explicit line if ICU cannot be initialized.
			return;
		}

		struct edge { const char* pos; double width; };
		struct cut { size_t end, next; };
		std::vector<edge> edges{{first, 0}};
		std::vector<cut> cuts;
		size_t style = 0, trimmed = 0;
		ubrk_first(line_breaks.get());
		int32_t soft = ubrk_next(line_breaks.get());
		ubrk_first(graphemes.get());
		for (int32_t end = ubrk_next(graphemes.get()); end != UBRK_DONE; end = ubrk_next(graphemes.get()))
		{
			const char* a = edges.back().pos;
			const char* b = first + end;
			double pen = edges.back().width;
			// The OVG width API sums nominal glyph advances. Walk the styled
			// slices once, rather than remeasuring prefixes of a long word.
			for (const char* p = a; p < b;)
			{
				const auto& r = ranges[style];
				const char* q = std::min(r.last, b);
				pen += advance(p, q, r.font);
				p = q;
				if (p == r.last) ++style;
			}
			const size_t index = edges.size();
			edges.push_back({b, pen});
			if (!std::all_of(a, b, [](char c) { return c == ' ' || c == '\t'; })) trimmed = index;
			while (soft != UBRK_DONE && soft < end) soft = ubrk_next(line_breaks.get());
			if (soft == end) cuts.push_back({trimmed, index});
		}
		if (edges.back().pos != last)
		{
			emit_line(first, last);
			return;
		}

		for (size_t start = 0; start + 1 < edges.size();)
		{
			const double limit = edges[start].width + max_width;
			const auto fit = std::upper_bound(edges.begin() + start + 1, edges.end(), limit,
				[](double w, const edge& e) { return w < e.width; });
			size_t end = static_cast<size_t>(fit - edges.begin() - 1), next = end;
			if (fit != edges.end())
			{
				const auto preferred = std::upper_bound(cuts.begin(), cuts.end(), limit,
					[&](double w, const cut& c) { return w < edges[c.end].width; });
				if (preferred != cuts.begin() && (preferred - 1)->end > start)
				{
					end = (preferred - 1)->end;
					next = (preferred - 1)->next;
				}
				if (end == start) end = next = start + 1; // Keep an oversized grapheme intact.
			}
			emit_line(edges[start].pos, edges[end].pos);
			start = next;
		}
	}

	template<class EmitRun, class FinishLine>
	void walk_wrapped_lines(vgui_widget* widget, const char* text, const vgui_text_span* spans,
		int span_count, bool for_draw, float base_size, float max_width, EmitRun emit_run, FinishLine finish_line)
	{
		std::vector<styled_range> ranges;
		walk_lines(text, spans, span_count, for_draw,
			[&](const char* first, const char* last, const span_sources& sources) {
				ranges.push_back({first, last, sources, font_for(sources[paint_channel], base_size)});
			}, [&] {
				if (ranges.empty()) { finish_line(); return; }
				size_t i = 0;
				wrap_paragraph(widget, ranges, max_width, [&](const char* first, const char* last) {
					while (i < ranges.size() && ranges[i].last <= first) ++i;
					while (i < ranges.size() && ranges[i].first < last)
					{
						const auto& r = ranges[i];
						emit_run(std::max(first, r.first), std::min(last, r.last), r.sources);
						if (r.last > last) break;
						++i;
					}
					finish_line();
				});
				ranges.clear();
			});
	}

	vgui_text_metrics metrics_for(vgui_widget* widget, font_style font)
	{
		vgui_text_metrics m = {};
		vgui_widget_get_text_metrics_styled(widget, font.size, font.weight, font.slant, &m);
		return m;
	}

	// OVG's text_box_rt uses an integer baseline offset inside the layout box.
	float layout_ascent(const vgui_text_metrics& m) { return std::trunc(m.ascent); }

	struct line_box
	{
		float ascent = 0, descent = 0, max_height = 0;
		void add(const vgui_text_metrics& m)
		{
			const float a = layout_ascent(m);
			ascent = std::max(ascent, a);
			descent = std::max(descent, m.line_height - a);
			max_height = std::max(max_height, m.line_height);
		}
		float height() const { return std::max(max_height, ascent + descent); }
	};

	struct text_run
	{
		std::string text;
		font_style font;
		vgui_color color = 0;
		vgui_text_glow glow = {};
		std::array<const vgui_text_span*, 3> decor = {};
		float width = 0, ascent = 0, line_height = 0;
		float x = 0, y = 0;
	};

	struct text_line
	{
		std::vector<text_run> runs;
		line_box box;
		float width = 0, top = 0, baseline = 0;
		unsigned decorated = 0;
	};

	struct text_layout
	{
		std::vector<text_line> lines;
		float width = 0, height = 0;
	};

	text_layout layout_text(vgui_widget* widget, const char* text, const vgui_text_span* spans,
		int span_count, float base_size, float wrap_width, vgui_color base_color)
	{
		text_layout layout;
		text_line row;
		const bool for_draw = base_color != 0;
		const auto base_glow = for_draw ? vgui_widget_get_text_glow(widget) : vgui_text_glow{};
		auto append_run = [&](const char* first, const char* last, const span_sources& sources) {
			const auto* paint = sources[paint_channel];
			auto& run = row.runs.emplace_back();
			run.text.assign(first, last);
			run.font = font_for(paint, base_size);
			if (for_draw)
			{
				run.color = vgui_text_widget_color(widget, paint && paint->color ? paint->color : base_color);
				const auto* glow = sources[glow_channel];
				run.glow = glow ? glow->glow : base_glow;
				for (int k = 0; k < 3; ++k)
				{
					run.decor[k] = sources[underline_channel + k];
					if (run.decor[k]) row.decorated |= decor_bits[k];
				}
			}
			const auto m = metrics_for(widget, run.font);
			run.ascent = layout_ascent(m);
			run.line_height = m.line_height;
			row.box.add(m);
			run.width = vgui_widget_measure_text_styled(widget, run.text.c_str(),
				run.font.size, run.font.weight, run.font.slant);
			row.width += run.width;
		};
		auto finish_line = [&] {
			if (row.runs.empty()) row.box.add(metrics_for(widget, font_for(nullptr, base_size)));
			layout.width = std::max(layout.width, row.width);
			layout.height += row.box.height();
			if (for_draw) layout.lines.push_back(std::move(row));
			row = {};
		};
		if (wrap_width > 0.0f)
			walk_wrapped_lines(widget, text, spans, span_count, for_draw, base_size, wrap_width, append_run, finish_line);
		else
			walk_lines(text, spans, span_count, for_draw, append_run, finish_line);
		return layout;
	}

	struct rect
	{
		float x0, y0, x1, y1;
	};

	// Intersect with the existing scissor, not replace it. An empty intersection
	// skips submission rather than setting a zero-size (disabled) scissor.
	struct scoped_clip
	{
		ovg_ctx_cb* cb;
		rvg_t* vg;
		glm::ivec4 saved = {};
		bool changed = false;
		bool visible = true;

		scoped_clip(ovg_ctx_cb* cb, rvg_t* vg, rect r) : cb(cb), vg(vg)
		{
			if (!cb->get_clip_rect || !cb->set_clip_rect) return;
			cb->get_clip_rect(vg, &saved);
			int x0 = (int)std::floor(r.x0), y0 = (int)std::floor(r.y0);
			int x1 = (int)std::ceil(r.x1), y1 = (int)std::ceil(r.y1);
			if (saved.z > 0 && saved.w > 0)
			{
				x0 = std::max(x0, saved.x); y0 = std::max(y0, saved.y);
				x1 = std::min(x1, saved.x + saved.z); y1 = std::min(y1, saved.y + saved.w);
			}
			visible = x0 < x1 && y0 < y1;
			if (!visible) return;
			glm::ivec4 clip = {x0, y0, x1 - x0, y1 - y0};
			changed = clip != saved;
			if (changed) cb->set_clip_rect(vg, &clip);
		}
		~scoped_clip() { if (changed) cb->set_clip_rect(vg, &saved); }
	};

	struct decor_line
	{
		font_style font;
		vgui_color color = 0;
		float thickness = 0, offset = 0;
		unsigned style = 0, flags = 0;

		bool operator==(const decor_line& rhs) const
		{
			return color == rhs.color && thickness == rhs.thickness && offset == rhs.offset &&
				style == rhs.style && flags == rhs.flags && font.size == rhs.font.size &&
				font.weight == rhs.font.weight && font.slant == rhs.font.slant;
		}
	};

	decor_line decor_for(const vgui_text_span& span, int bit, float base_size, vgui_color base_color)
	{
		const auto& o = bit == 0 ? span.underline : bit == 1 ? span.strikethrough : span.overline;
		const auto legacy_color = span.decor_color ? span.decor_color : (span.color ? span.color : base_color);
		const unsigned style = o.present & VGUI_TEXT_DECOR_PRESENT_STYLE ? o.style : span.decor_style;
		const unsigned flags = o.present & VGUI_TEXT_DECOR_PRESENT_FLAGS ? o.flags : span.decor_flags;
		return { font_for(&span, base_size), o.present & VGUI_TEXT_DECOR_PRESENT_COLOR ? o.color : legacy_color,
			o.present & VGUI_TEXT_DECOR_PRESENT_THICKNESS ? o.thickness : span.decor_thickness,
			o.present & VGUI_TEXT_DECOR_PRESENT_OFFSET ? o.offset : span.decor_offset,
			style <= VGUI_TEXT_DECOR_STYLE_WAVY ? style : (unsigned)VGUI_TEXT_DECOR_STYLE_SOLID,
			bit != 1 ? flags & VGUI_TEXT_DECOR_FLAG_SKIP_INK : 0u };
	}

	using ink_interval = std::pair<float, float>;

	// At a shared strip height, every decoration on this line can reuse the
	// same ink. Query paint runs rather than decoration sources; a glyph can
	// overhang its advance and enter a neighboring decoration segment.
	struct line_ink
	{
		struct strip
		{
			float top, bottom;
			std::vector<ink_interval> intervals;
		};
		vgui_widget* widget;
		const std::vector<text_run>& runs;
		float right;
		std::vector<strip> strips;

		const std::vector<ink_interval>& get(float top, float bottom)
		{
			for (const auto& s : strips)
				if (s.top == top && s.bottom == bottom) return s.intervals;
			auto& intervals = strips.emplace_back(strip{top, bottom, {}}).intervals;
			std::vector<ink_interval> scratch;
			for (const auto& run : runs)
			{
				if (run.x >= right) break;
				if (!has_alpha(run.color) || !(run.line_height > 0)) continue;
				scratch.clear();
				vgui_text_ink_intervals(widget, run.text.c_str(), run.x, run.y,
					run.font.size, run.font.weight, run.font.slant, top, bottom, scratch);
				for (auto i : scratch)
					if (std::isfinite(i.first) && std::isfinite(i.second) && i.first < i.second)
						intervals.push_back(i);
			}
			std::sort(intervals.begin(), intervals.end());
			size_t count = 0;
			for (auto i : intervals)
			{
				if (count && i.first <= intervals[count - 1].second)
					intervals[count - 1].second = std::max(intervals[count - 1].second, i.second);
				else intervals[count++] = i;
			}
			intervals.resize(count);
			return intervals;
		}
	};

	struct point { float x, y; };

	// All decoration geometry is emitted here, after clipping and skip-ink.
	struct decor_painter
	{
		ovg_ctx_cb* cb;
		rvg_t* vg;
		uint32_t color;
		std::vector<vgui_glow_rect>* glow_mask;

		void fill_rect(rect r) const
		{
			if (glow_mask) {
				glow_mask->push_back({r.x0, r.y0, r.x1 - r.x0, r.y1 - r.y0});
				return;
			}
			ovg_tools::fill_round_rect(cb, vg, r.x0, r.y0, r.x1 - r.x0, r.y1 - r.y0, 0.0f, color);
		}
		bool supports_polygon() const { return glow_mask || (cb->move_to && cb->line_to && cb->close_path); }
		void fill_polygon(const std::vector<point>& points) const
		{
			if (points.size() < 3) return;
			if (glow_mask) {
				// The sine ribbon emits paired upper/lower vertices. Approximate
				// its blurred mask with narrow strips, not its full bounds.
				for (size_t i = 1, n = points.size() / 2; i < n; ++i) {
					const auto& a = points[i - 1];
					const auto& b = points[i];
					const float top = (a.y + b.y) * 0.5f;
					const float bottom = (points[points.size() - i].y + points[points.size() - i - 1].y) * 0.5f;
					if (b.x > a.x && bottom > top) fill_rect({a.x, top, b.x, bottom});
				}
				return;
			}
			cb->move_to(vg, points.front().x, points.front().y);
			for (size_t i = 1; i < points.size(); ++i) cb->line_to(vg, points[i].x, points[i].y);
			cb->close_path(vg);
			cb->set_source_color(vg, color);
			cb->fill(vg);
		}
	};

	// Draw a continuous sine ribbon with eight samples per period. Clip the
	// top and bottom edges at their intersections with the vertical clip;
	// disconnected visible lobes are emitted as separate simple polygons.
	void draw_wave(const decor_painter& paint, double anchor, float left, float right,
		float top, float t, float clip_top, float clip_bottom)
	{
		static constexpr double wave[] = {0, .70710678, 1, .70710678, 0, -.70710678, -1, -.70710678, 0};
		const double step = 0.5 * t;
		const double amplitude = 1.5 * t;
		std::vector<point> upper, lower;
		auto flush = [&] {
			if (upper.size() > 1)
			{
				upper.insert(upper.end(), lower.rbegin(), lower.rend());
				paint.fill_polygon(upper);
			}
			upper.clear();
			lower.clear();
		};
		double sample = std::floor((left - anchor) / step);
		for (double a = anchor + sample * step; a < right; a = anchor + ++sample * step)
		{
			const double b = anchor + (sample + 1) * step;
			if (!(b > a)) break;
			const double x = std::max<double>(left, a);
			const double end = std::min<double>(right, b);
			if (!(end > x)) continue;
			const int k = (int)std::fmod(sample, 8);
			const double slope = (wave[k + 1] - wave[k]) * amplitude / (b - a);
			const double y = top + amplitude * wave[k] + (x - a) * slope;
			std::array<double, 6> stops = {x, end};
			size_t n = 2;
			if (slope != 0)
				for (double edge : {clip_top - (double)t, (double)clip_top, clip_bottom - (double)t, (double)clip_bottom})
				{
					const double u = x + (edge - y) / slope;
					if (u > x && u < end) stops[n++] = u;
				}
			std::sort(stops.begin(), stops.begin() + n);
			auto point_at = [&](double u) {
				const double v = y + (u - x) * slope;
				upper.push_back({(float)u, (float)std::clamp(v, (double)clip_top, (double)clip_bottom)});
				lower.push_back({(float)u, (float)std::clamp(v + t, (double)clip_top, (double)clip_bottom)});
			};
			for (size_t j = 1; j < n; ++j)
			{
				if (stops[j] <= stops[j - 1]) continue;
				const double mid_y = y + ((stops[j - 1] + stops[j]) * .5 - x) * slope;
				if (mid_y < clip_bottom && mid_y + t > clip_top)
				{
					if (upper.empty()) point_at(stops[j - 1]);
					point_at(stops[j]);
				}
				else flush();
			}
		}
		flush();
	}

	void draw_decor(vgui_widget* widget, ovg_ctx_cb* cb, rvg_t* vg, int bit, const decor_line& line,
		float x0, float x1, float baseline, rect clip, float scale, line_ink* ink,
		std::vector<vgui_glow_rect>* glow_mask)
	{
		const float anchor = x0;
		x0 = std::max(x0, clip.x0);
		x1 = std::min(x1, clip.x1);
		const vgui_color color = vgui_text_widget_color(widget, line.color);
		if (!(x1 > x0) || !has_alpha(color)) return;

		const auto m = metrics_for(widget, line.font);
		const float offset = line.offset * scale;
		float top = baseline, thickness = m.underline_thickness;
		if (bit == 0) top += m.underline_offset + offset;
		else if (bit == 1)
		{
			top += m.strikeout_offset - offset;
			thickness = m.strikeout_thickness;
		}
		else top -= m.ascent + offset;
		if (line.thickness > 0.0f) thickness = std::max(1.0f, line.thickness * scale);
		top = std::round(top);
		if (!(thickness > 0) || !std::isfinite(thickness) || !std::isfinite(top)) return;
		// vgui packs AARRGGBB; OVG consumes AABBGGRR.
		const uint32_t ovg = (color & 0xFF00FF00u) | ((color & 0x00FF0000u) >> 16) | ((color & 0xFFu) << 16);
		const decor_painter paint{cb, vg, ovg, glow_mask};

		// Apply ink gaps to each strip independently, without resetting the phase
		// of a dash or wave after a gap or at the clipping edge.
		auto strip = [&](float y0, float y1, const auto& emit) {
			y0 = std::max(y0, clip.y0);
			y1 = std::min(y1, clip.y1);
			if (!(y1 > y0)) return;
			float left = x0;
			if (ink && (line.flags & VGUI_TEXT_DECOR_FLAG_SKIP_INK))
			{
				const float gap = std::max(1.0f, thickness * .5f);
				const auto& intervals = ink->get(y0, y1);
				auto i = std::lower_bound(intervals.begin(), intervals.end(), x0 - gap,
					[](const auto& interval, float x) { return interval.second < x; });
				for (; i != intervals.end() && i->first - gap < x1; ++i)
				{
					const float next = i->first - gap;
					if (next > left) emit(left, next, y0, y1);
					left = std::max(left, i->second + gap);
					if (left >= x1) return;
				}
			}
			if (left < x1) emit(left, x1, y0, y1);
		};
		auto solid = [&](float left, float right, float y0, float y1) { paint.fill_rect({left, y0, right, y1}); };
		switch (line.style)
		{
		case VGUI_TEXT_DECOR_STYLE_DOUBLE:
			strip(top, top + thickness * .5f, solid);
			strip(top + thickness * 1.5f, top + thickness * 2.0f, solid);
			break;
		case VGUI_TEXT_DECOR_STYLE_DOTTED:
		case VGUI_TEXT_DECOR_STYLE_DASHED:
			strip(top, top + thickness, [&](float left, float right, float y0, float y1) {
				const double on = (line.style == VGUI_TEXT_DECOR_STYLE_DOTTED ? 1.0 : 3.0) * thickness;
				const double period = on + 2.0 * thickness;
				double x = left - std::fmod((double)left - anchor, period);
				for (; x < right; x += period)
				{
					const float a = (float)std::max<double>(left, x);
					const float b = (float)std::min<double>(right, x + on);
					if (b > a) solid(a, b, y0, y1);
					if (!(x + period > x)) break;
				}
			});
			break;
		case VGUI_TEXT_DECOR_STYLE_WAVY:
			if (paint.supports_polygon())
			{
				strip(top - 1.5f * thickness, top + 2.5f * thickness,
					[&](float left, float right, float y0, float y1) {
						draw_wave(paint, anchor, left, right, top, thickness, y0, y1);
					});
				break;
			}
			[[fallthrough]];
		default:
			strip(top, top + thickness, solid);
			break;
		}
	}

	void draw_decor_layer(vgui_widget* widget, ovg_ctx_cb* cb, rvg_t* vg,
		const text_line& row, int bit, rect clip, float scale,
		float base_size, vgui_color base_color, line_ink* ink = nullptr,
		std::vector<vgui_glow_rect>* glow_mask = nullptr)
	{
		if (!(row.decorated & decor_bits[bit])) return;
		// Merge each bit independently: a new strike style must not split a
		// continuous, potentially translucent underline at the same boundary.
		bool open = false;
		float x0 = 0, x1 = 0;
		decor_line line;
		auto flush = [&] {
			if (open) draw_decor(widget, cb, vg, bit, line, x0, x1, row.baseline, clip, scale, ink, glow_mask);
			open = false;
		};
		for (const auto& run : row.runs)
		{
			if (run.x >= clip.x1) break;
			const auto* source = run.decor[bit];
			if (!source) { flush(); continue; }
			const decor_line next = decor_for(*source, bit, base_size, base_color);
			if (!open || !(line == next))
			{
				flush();
				open = true;
				x0 = run.x;
				line = next;
			}
			x1 = run.x + run.width;
		}
		flush();
	}
}

static void draw_text_spans(vgui_widget* widget,
	ovg_ctx_cb* cb, rvg_t* vg, const char* text,
	const vgui_text_span* spans, int span_count,
	float x, float y, float width, float height,
	vgui_color base_color, float align_x, float align_y, float base_font_size, bool wrap)
{
	if (!widget || !cb || !vg || !text || !*text || !base_color || !(width > 0 && height > 0)) return;
	if (wrap && !std::isfinite(width)) return;
	const rect clip = { x, y, x + width, y + height };
	const float scale = vgui_context_get_ui_scale(vgui_widget_get_context(widget));
	auto layout = layout_text(widget, text, spans, span_count, base_font_size, wrap ? width : 0, base_color);
	auto& lines = layout.lines;
	if (!(layout.height > 0.0f)) return;

	const float ax = std::clamp(align_x, 0.0f, 1.0f);
	const float pen_y = y + std::max(0.0f, (height - layout.height) * std::clamp(align_y, 0.0f, 1.0f));
	float y_offset = 0;
	for (auto& line : lines)
	{
		line.top = std::trunc(pen_y + y_offset);
		line.baseline = line.top + line.box.ascent;
		y_offset += line.box.height();
		float pen_x = x + std::max(0.0f, (width - line.width) * ax);
		for (auto& run : line.runs)
		{
			run.x = pen_x;
			run.y = line.baseline - run.ascent;
			pen_x += run.width;
		}
	}
	auto submit = [&](const text_run& run, unsigned pass) {
		vgui_draw_text_passes(widget, cb, vg, run.text.c_str(), run.x, run.y,
			clip.x1 - run.x, run.line_height, run.color, 0.0f, 0.0f, run.font.size,
			&run.glow, pass, run.font.weight, run.font.slant);
	};

	const auto same_glow = [](const vgui_text_glow& a, const vgui_text_glow& b) {
		return a.color == b.color && a.sigma == b.sigma && a.spread == b.spread &&
			a.opacity == b.opacity && a.mode == b.mode && a.blend == b.blend &&
			((a.flags ^ b.flags) & VGUI_TEXT_GLOW_DECORATION) == 0;
	};
	const auto draw_glow = [&](bool inner) {
		std::vector<vgui_glow_run> batch;
		std::vector<vgui_glow_rect> mask;
		for (const auto& line : lines) {
			line_ink ink{widget, line.runs, clip.x1, {}};
			for (size_t i = 0; i < line.runs.size();) {
				const auto& run = line.runs[i];
				if (run.x >= clip.x1) break;
				const auto& glow = run.glow;
				if (!has_alpha(glow.color) || !(run.line_height > 0) ||
					(glow.mode == VGUI_TEXT_GLOW_INNER) != inner) { ++i; continue; }
				const bool decor_glow = cb->add_text_glow_batch && (glow.flags & VGUI_TEXT_GLOW_DECORATION);
				size_t end = i + 1;
				if (cb->add_text_glow_batch) {
					// Keep the ROI local and the batch bounded. Text remains shaped
					// per original run, including differently styled glyphs.
					while (end < line.runs.size() && end - i < 64 && line.runs[end].x < clip.x1 &&
						line.runs[end].x + line.runs[end].width - run.x <= 1024.0f &&
						same_glow(glow, line.runs[end].glow)) ++end;
				}
				const float sigma = glow.sigma * scale;
				const float pad = inner ? 0.0f : std::ceil(3.0f *
					(std::isfinite(sigma) ? std::clamp(sigma, 0.0f, 256.0f) : 0.0f) + 2.0f);
				scoped_clip scissor(cb, vg, {clip.x0 - pad, clip.y0 - pad, clip.x1 + pad, clip.y1 + pad});
				if (scissor.visible) {
					if (end == i + 1 && !decor_glow) {
						submit(run, inner ? (VGUI_TEXT_PASS_INNER_GLOW | VGUI_TEXT_PASS_CLIP_TEXT) : VGUI_TEXT_PASS_GLOW);
					} else {
						batch.clear();
						mask.clear();
						for (size_t j = i; j < end; ++j) {
							const auto& part = line.runs[j];
							batch.push_back({part.text.c_str(), part.x, part.y, clip.x1 - part.x,
								part.line_height, part.font.size, part.font.weight, part.font.slant});
						}
						if (decor_glow) {
							const auto& last = line.runs[end - 1];
							rect region = clip;
							region.x0 = std::max(region.x0, run.x);
							region.x1 = std::min(region.x1, last.x + last.width);
							for (int bit = 0; bit < 3; ++bit)
								draw_decor_layer(widget, cb, vg, line, bit, region, scale, base_font_size,
									base_color, bit == 1 ? nullptr : &ink, &mask);
						}
						vgui_text_glow_batch(widget, cb, vg, glow, batch.data(), batch.size(), mask.data(), mask.size());
					}
				}
				i = end;
			}
		}
	};
	draw_glow(false); // Outer halos are below every run's body.
	for (const auto& line : lines)
	{
		line_ink ink{widget, line.runs, clip.x1, {}};
		draw_decor_layer(widget, cb, vg, line, 0, clip, scale, base_font_size, base_color, &ink);
		draw_decor_layer(widget, cb, vg, line, 2, clip, scale, base_font_size, base_color, &ink);
	}
	auto draw_text_runs = [&] {
		for (const auto& line : lines)
		{
			if (line.top >= std::ceil(clip.y1)) break;
			for (const auto& run : line.runs)
			{
				if (run.x >= clip.x1) break;
				if (has_alpha(run.color) && run.line_height > 0)
					submit(run, VGUI_TEXT_PASS_TEXT | VGUI_TEXT_PASS_CLIP_TEXT);
			}
		}
	};
	if (lines.front().top < std::floor(clip.y0) ||
		std::ceil(lines.back().top + lines.back().box.height()) > std::ceil(clip.y1))
	{
		// Clip the whole block without resizing the layouts shared by both passes.
		scoped_clip text_clip(cb, vg, clip);
		if (text_clip.visible) draw_text_runs();
	}
	else draw_text_runs();
	for (const auto& line : lines)
		draw_decor_layer(widget, cb, vg, line, 1, clip, scale, base_font_size, base_color);
	draw_glow(true); // Inner light is confined to the mask and overlays the body.
}

void vgui_custom_draw_text_spans(vgui_widget* widget,
	ovg_ctx_cb* cb, rvg_t* vg, const char* text,
	const vgui_text_span* spans, int span_count,
	float x, float y, float width, float height,
	vgui_color base_color, float align_x, float align_y, float base_font_size)
{
	draw_text_spans(widget, cb, vg, text, spans, span_count, x, y, width, height,
		base_color, align_x, align_y, base_font_size, false);
}

void vgui_custom_draw_text_spans_wrapped(vgui_widget* widget,
	ovg_ctx_cb* cb, rvg_t* vg, const char* text,
	const vgui_text_span* spans, int span_count,
	float x, float y, float width, float height,
	vgui_color base_color, float align_x, float align_y, float base_font_size)
{
	draw_text_spans(widget, cb, vg, text, spans, span_count, x, y, width, height,
		base_color, align_x, align_y, base_font_size, true);
}

void vgui_widget_measure_text_spans_wrapped(vgui_widget* widget,
	const char* text, const vgui_text_span* spans, int span_count,
	float max_width, float base_font_size, float* out_width, float* out_height)
{
	if (!out_width && !out_height) return;
	float width = 0, height = 0;
	if (widget && vgui_widget_get_context(widget) && text && *text && max_width > 0 && std::isfinite(max_width))
	{
		const auto layout = layout_text(widget, text, spans, span_count, base_font_size, max_width, 0);
		width = layout.width;
		height = layout.height;
	}
	if (out_width) *out_width = width;
	if (out_height) *out_height = height;
}

float vgui_widget_measure_text_spans(vgui_widget* widget,
	const char* text, const vgui_text_span* spans, int span_count,
	float base_font_size)
{
	if (!widget || !text || !*text) return 0.0f;
	float widest = 0, total = 0;
	std::string run;
	walk_lines(text, spans, span_count, false,
		[&](const char* start, const char* end, const auto& sources) {
			const auto font = font_for(sources[paint_channel], base_font_size);
			run.assign(start, end);
			total += vgui_widget_measure_text_styled(widget, run.c_str(), font.size, font.weight, font.slant);
		}, [&] {
			widest = std::max(widest, total);
			total = 0;
		});
	return widest;
}

float vgui_widget_get_line_height_spans(vgui_widget* widget,
	const vgui_text_span* spans, int span_count, float base_font_size)
{
	if (!widget || !vgui_widget_get_context(widget)) return 0.0f;
	if (!spans || span_count <= 0)
		return vgui_widget_get_line_height_styled(widget, base_font_size, 0, 0);
	// No text length is available: keep base metrics for potentially uncovered
	// text, and sweep the positive byte positions to ignore shadowed styles.
	line_box box;
	walk_intervals(nullptr, std::numeric_limits<int64_t>::max(), spans, span_count, false,
		[&](int64_t, int64_t, const auto& sources) {
			box.add(metrics_for(widget, font_for(sources[paint_channel], base_font_size)));
		});
	return box.height();
}

float vgui_widget_get_text_height_spans(vgui_widget* widget, const char* text,
	const vgui_text_span* spans, int span_count, float base_font_size)
{
	if (!widget || !vgui_widget_get_context(widget) || !text || !*text) return 0.0f;
	float height = 0;
	line_box box;
	bool has_runs = false;
	walk_lines(text, spans, span_count, false,
		[&](const char*, const char*, const auto& sources) {
			box.add(metrics_for(widget, font_for(sources[paint_channel], base_font_size)));
			has_runs = true;
		}, [&] {
			if (!has_runs) box.add(metrics_for(widget, font_for(nullptr, base_font_size)));
			height += box.height();
			box = {};
			has_runs = false;
		});
	return height;
}
