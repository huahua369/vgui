#ifndef VGUI_RICHTEXT_H
#define VGUI_RICHTEXT_H

/* Core rich-text primitives (CORE_ONLY safe).
 *
 * These helpers turn a plain UTF-8 string plus an array of byte-ranged spans
 * into a sequence of single-style text runs.  They are the small missing
 * primitive that lets hosts draw highlighted / mixed-font text without
 * owning their own text-layout code.  They do not parse markup and do not
 * introduce a control: hosts keep their own document model.
 */

#include <vgui/vgui.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Decoration bits for vgui_text_span::decor. */
enum vgui_text_decor
{
	VGUI_TEXT_DECOR_UNDERLINE = 1u << 0,
	VGUI_TEXT_DECOR_STRIKETHROUGH = 1u << 1,
	VGUI_TEXT_DECOR_OVERLINE = 1u << 2,
	VGUI_TEXT_DECOR_ALL = VGUI_TEXT_DECOR_UNDERLINE | VGUI_TEXT_DECOR_STRIKETHROUGH | VGUI_TEXT_DECOR_OVERLINE
};

enum vgui_text_decor_style
{
	VGUI_TEXT_DECOR_STYLE_SOLID = 0,
	VGUI_TEXT_DECOR_STYLE_DOUBLE,
	VGUI_TEXT_DECOR_STYLE_DOTTED,
	VGUI_TEXT_DECOR_STYLE_DASHED,
	VGUI_TEXT_DECOR_STYLE_WAVY
};

enum vgui_text_decor_flags
{
	/* Opt in to gaps around intersecting glyph ink (underline/overline only). */
	VGUI_TEXT_DECOR_FLAG_SKIP_INK = 1u << 0
};

/* Fields to take from a per-line override instead of the span's decor_* fields. */
enum vgui_text_decor_presence
{
	VGUI_TEXT_DECOR_PRESENT_COLOR = 1u << 0,
	VGUI_TEXT_DECOR_PRESENT_THICKNESS = 1u << 1,
	VGUI_TEXT_DECOR_PRESENT_OFFSET = 1u << 2,
	VGUI_TEXT_DECOR_PRESENT_STYLE = 1u << 3,
	VGUI_TEXT_DECOR_PRESENT_FLAGS = 1u << 4
};

typedef struct vgui_text_decor_override
{
	unsigned present;      /* mask of VGUI_TEXT_DECOR_PRESENT_*; 0 uses legacy fields */
	vgui_color color;      /* literal color, including transparent 0 */
	float thickness;      /* logical units; 0 = font metric */
	float offset;         /* logical units; positive moves away from the baseline */
	unsigned style;        /* VGUI_TEXT_DECOR_STYLE_*; 0 = solid */
	unsigned flags;        /* VGUI_TEXT_DECOR_FLAG_*; 0 = no flags */
} vgui_text_decor_override;

/* One styled UTF-8 byte range. Fields after decor may be zero-initialized. */
typedef struct vgui_text_span
{
	int start;
	int length;
	vgui_color color;        /* 0 = base_color */
	float font_size;         /* logical size; 0 = base_font_size */
	unsigned decor;         /* adds decoration bits; 0 does not clear them */
	int font_weight;        /* VGUI_FONT_WEIGHT_*; 0 = widget value */
	int font_slant;         /* VGUI_FONT_SLANT_*; 0 = widget value */
	vgui_color decor_color; /* 0 = this span's text color, or base_color */
	float decor_thickness;  /* logical units; 0 = font metric */
	float decor_offset;     /* logical units added to the font offset; positive moves away from the baseline */
	unsigned decor_style;
	unsigned decor_flags;
	vgui_text_glow glow;    /* nonzero alpha overrides; with zero alpha, OFF clears, otherwise inherits */
	/* Overrides apply only to lines enabled by the corresponding decor bit. */
	vgui_text_decor_override underline;
	vgui_text_decor_override strikethrough;
	vgui_text_decor_override overline;
} vgui_text_span;

/* Draw spanned text inside a VGUI_CUSTOM draw callback.
 *
 * - Byte ranges are clamped and expanded to UTF-8 character boundaries.
 *   For text color/size/weight/slant, the last covering span wins; unset
 *   fields use the base, not the value of an outer span.
 * - Each decoration bit takes its style and metrics from the last span that
 *   sets it. Glow inherits from earlier covering spans or the widget unless
 *   explicitly overridden or cleared.
 * - LF and CRLF break lines; there is no automatic wrapping. Runs share a
 *   baseline on each line. align_x positions each line independently; align_y
 *   positions the whole block. Both are physical (0..1) and are NOT mirrored
 *   in RTL, matching vgui_custom_draw_text_sized().
 * - Coordinates are absolute window coordinates. Text and decoration are
 *   clipped to the supplied rectangle. Glow can extend beyond it by its
 *   blur padding, subject to the existing parent/widget clip.
 * - No spans (or NULL) means base styling on each line. base_color == 0 is a
 *   no-op; for glow-only text use an alpha-zero nonzero color (e.g. 0x00FFFFFFu). */
VGUI_API void vgui_custom_draw_text_spans(vgui_widget* widget,
	struct ovg_ctx_cb* cb, struct rvg_t* vg, const char* text,
	const vgui_text_span* spans, int span_count,
	float x, float y, float width, float height,
	vgui_color base_color, float align_x, float align_y, float base_font_size);

/* Largest advance width of any explicit line in the same layout as drawing. */
VGUI_API float vgui_widget_measure_text_spans(vgui_widget* widget,
	const char* text, const vgui_text_span* spans, int span_count,
	float base_font_size);

/* Shared-baseline line height for the span styles and base style. As this
 * helper has no text argument, it cannot exclude styles past the string end
 * or tell whether the base style will actually be used. */
VGUI_API float vgui_widget_get_line_height_spans(vgui_widget* widget,
	const vgui_text_span* spans, int span_count, float base_font_size);

/* Total layout height for the actual string and spans, in device pixels.
 * Empty lines (including a trailing line) use base metrics; NULL or empty
 * text has height zero. Decorations and glow do not affect the result. */
VGUI_API float vgui_widget_get_text_height_spans(vgui_widget* widget,
	const char* text, const vgui_text_span* spans, int span_count,
	float base_font_size);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* VGUI_RICHTEXT_H */
