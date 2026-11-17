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
	VGUI_TEXT_DECOR_STRIKETHROUGH = 1u << 1
};

/* One styled byte range inside the UTF-8 text passed to the API below.
 * start/length are byte offsets; a zero color or font_size inherits the
 * base value supplied to the draw/measure call. */
typedef struct vgui_text_span
{
	int start;
	int length;
	vgui_color color;
	float font_size;
	unsigned decor;
} vgui_text_span;

/* Draw spanned text inside a VGUI_CUSTOM draw callback.
 *
 * - Spans may be unsorted or overlap; later spans win on overlap.  Bytes not
 *   covered by any span are drawn with base_color / base_font_size.
 * - All runs share one line box sized to the tallest run and are vertically
 *   centred inside it.  align_x/align_y are physical (0..1) and are NOT
 *   mirrored in RTL, matching vgui_custom_draw_text_sized().
 * - Text is clipped to the supplied rectangle; decoration lines are clamped
 *   to the same rectangle.  Coordinates are absolute window coordinates.
 * - Passing no spans (or NULL) is equivalent to a single base-style run. */
VGUI_API void vgui_custom_draw_text_spans(vgui_widget* widget,
	struct ovg_ctx_cb* cb, struct rvg_t* vg, const char* text,
	const vgui_text_span* spans, int span_count,
	float x, float y, float width, float height,
	vgui_color base_color, float align_x, float align_y, float base_font_size);

/* Total advance width of the same run sequence the draw call would produce. */
VGUI_API float vgui_widget_measure_text_spans(vgui_widget* widget,
	const char* text, const vgui_text_span* spans, int span_count,
	float base_font_size);

/* Height of the shared line box (the tallest run's line height). */
VGUI_API float vgui_widget_get_line_height_spans(vgui_widget* widget,
	const vgui_text_span* spans, int span_count, float base_font_size);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* VGUI_RICHTEXT_H */
