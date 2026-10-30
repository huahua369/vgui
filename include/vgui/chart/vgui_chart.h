#pragma once
/*
 * vgui_chart.h - optional chart / gauge / dashboard module for vgui.
 *
 * The public interface is plain C99/C11.  The implementation is C++17 but no
 * C++ type, template, exception or std:: type crosses this header.  All
 * handles are opaque, every extensible struct starts with struct_size and has
 * a *_default() helper.
 *
 * Design rules:
 *  - The module owns chart data.  Callers push data with set_points() or
 *    append_points(); no host callback is invoked during drawing.
 *  - x values must be finite and non-decreasing.  append_points() rejects a
 *    backwards timestamp instead of silently corrupting the ordering.
 *  - set_ring_capacity() enables a fixed-capacity ring buffer: once full, the
 *    oldest point is overwritten.  reserve() only preallocates.
 *  - Strings are UTF-8 and copied by the module.  Returned strings stay valid
 *    until the next call that mutates the object, or until destruction.
 *  - All objects are single-threaded and belong to the UI thread.
 *  - Destroy charts/gauges/dashboards before their parent widget or context.
 *
 * P0 scope: line/area/bar charts, radial gauge, JSON dashboard configuration,
 * JSON persistence and CSV data export.
 */
#include <stddef.h>
#include <stdint.h>

#include <vgui/vgui.h>

#ifdef __cplusplus
extern "C" {
#endif

	typedef struct vgui_chart vgui_chart;
	typedef struct vgui_gauge vgui_gauge;
	typedef struct vgui_dashboard vgui_dashboard;

	/* Stable within one chart/dashboard session; 0 is invalid.  Persisted
	 * configuration uses series keys, never numeric ids. */
	typedef uint64_t vgui_series_id;

	typedef struct vgui_chart_point {
		double x;
		double y;
	} vgui_chart_point;

	typedef enum vgui_chart_kind {
		VGUI_CHART_LINE = 0,
		VGUI_CHART_BAR,
		VGUI_CHART_AREA
	} vgui_chart_kind;

	typedef enum vgui_chart_scale {
		VGUI_CHART_SCALE_LINEAR = 0,
		/* x is elapsed seconds; labels are durations ("30s", "1m30s"). */
		VGUI_CHART_SCALE_TIME
	} vgui_chart_scale;

	typedef enum vgui_chart_decimation {
		VGUI_CHART_DECIMATION_NONE = 0,
		/* One min/max pair per pixel column.  Default; keeps spikes. */
		VGUI_CHART_DECIMATION_MIN_MAX
	} vgui_chart_decimation;

	typedef enum vgui_chart_status {
		VGUI_CHART_STATUS_NORMAL = 0,
		VGUI_CHART_STATUS_INFO,
		VGUI_CHART_STATUS_WARNING,
		VGUI_CHART_STATUS_CRITICAL,
		VGUI_CHART_STATUS_STALE
	} vgui_chart_status;

	/* Streamed output.  data is not NUL terminated.  Return non-zero on
	 * success.  A save/export may invoke the callback more than once. */
	typedef int (*vgui_chart_write_fn)(void* user, const char* data, size_t size);

	/* Formats one numeric value.  Return 1 when out contains a NUL terminated
	 * string.  Used for axis labels, gauge values and tooltips. */
	typedef int (*vgui_chart_format_fn)(void* user, double value,
		char* out, size_t cap);

	/* ---------------------------------------------------------------------- */
	/* Chart                                                                   */
	/* ---------------------------------------------------------------------- */

	typedef struct vgui_chart_series_desc {
		uint32_t struct_size;
		const char* key;		/* stable string key; unique in the chart */
		const char* name;		/* display name; may be NULL -> key */
		int kind;			/* vgui_chart_kind */
		int axis;			/* reserved: 0 = left Y axis */
		vgui_color color;		/* 0 = palette */
		float line_width;		/* <= 0 -> 2 logical pixels */
		float fill_alpha;		/* area fill alpha 0..1; < 0 -> 0.15 */
		int visible;
		int stacked;			/* bar/area stacking */
		const char* stack_group;	/* NULL/"" = default group */
		const char* unit;		/* used by tooltips/legend */
	} vgui_chart_series_desc;

	typedef struct vgui_chart_axis_desc {
		uint32_t struct_size;
		int scale;			/* vgui_chart_scale */
		double min;			/* min >= max selects auto range */
		double max;
		int tick_count;			/* <= 0 -> 5 */
		int include_zero;		/* bars default to 1 */
		int grid;
		const char* label;
		const char* unit;
		/* Optional printf-style pattern with one double conversion, used when
		 * format is NULL.  Example "%.1f" or "%g ms". */
		const char* format_pattern;
		vgui_chart_format_fn format;
		void* format_user;
	} vgui_chart_axis_desc;

	typedef struct vgui_chart_options {
		uint32_t struct_size;
		int legend;
		int tooltip;
		int crosshair;
		/* Follow the newest data.  When window > 0 the view becomes
		 * [newest_x - window, newest_x]; otherwise it spans all data. */
		int follow;
		double window;
		int decimation;			/* vgui_chart_decimation */
	} vgui_chart_options;

	typedef struct vgui_chart_theme {
		uint32_t struct_size;
		vgui_color plot_bg;
		vgui_color grid;
		vgui_color axis;
		vgui_color text;
		vgui_color muted_text;
		vgui_color tooltip_bg;
		vgui_color tooltip_border;
		vgui_color tooltip_text;
		vgui_color crosshair;
		vgui_color selection;
		vgui_color series[10];
		vgui_color positive;
		vgui_color negative;
		vgui_color warning;
		vgui_color critical;
	} vgui_chart_theme;

	typedef struct vgui_chart_stats {
		uint32_t struct_size;
		size_t raw_points;		/* sum of all series point counts */
		size_t visible_points;		/* points inside the current x view */
		size_t drawn_points;		/* vertices produced after decimation */
		size_t series_drawn;
		int cache_hit;
	} vgui_chart_stats;

	typedef enum vgui_chart_export_flags {
		/* Restrict rows to the current x view instead of exporting all data. */
		VGUI_CHART_EXPORT_VISIBLE_ONLY = 1u << 0,
		/* Export the min/max decimated view instead of raw points. */
		VGUI_CHART_EXPORT_DECIMATED = 1u << 1,
		/* Include series whose visible flag is off.  Hidden series are skipped
		 * by default. */
		VGUI_CHART_EXPORT_INCLUDE_HIDDEN = 1u << 2,
		/* Write the CSV header line. */
		VGUI_CHART_EXPORT_HEADER = 1u << 3
	} vgui_chart_export_flags;

	VGUI_API void vgui_chart_options_default(vgui_chart_options* options);
	VGUI_API void vgui_chart_axis_desc_default(vgui_chart_axis_desc* desc);
	VGUI_API void vgui_chart_series_desc_default(vgui_chart_series_desc* desc);

	/* Fills a chart theme derived from the current vgui theme. */
	VGUI_API void vgui_chart_theme_from_theme(const vgui_theme* theme,
		vgui_chart_theme* out);
	VGUI_API void vgui_chart_theme_high_contrast(vgui_chart_theme* out);

	/* Creates a chart inside parent.  Returns NULL on invalid input. */
	VGUI_API vgui_chart* vgui_chart_create(vgui_widget* parent);
	/* Destroys the chart and its widget.  Destroy before the parent widget. */
	VGUI_API void vgui_chart_destroy(vgui_chart* chart);
	VGUI_API vgui_widget* vgui_chart_widget(vgui_chart* chart);
	/* Sets a title drawn in the chart header and used as the a11y name. */
	VGUI_API void vgui_chart_set_title(vgui_chart* chart, const char* title);

	VGUI_API vgui_series_id vgui_chart_add_series(vgui_chart* chart,
		const vgui_chart_series_desc* desc);
	VGUI_API vgui_series_id vgui_chart_find_series(vgui_chart* chart,
		const char* key);
	VGUI_API int vgui_chart_remove_series(vgui_chart* chart,
		vgui_series_id series);
	/* Updates mutable presentation fields of an existing series.  desc.key may
	 * be NULL; kind cannot change after points exist. */
	VGUI_API int vgui_chart_update_series(vgui_chart* chart,
		vgui_series_id series, const vgui_chart_series_desc* desc);

	/* Preallocates point storage. */
	VGUI_API int vgui_chart_reserve(vgui_chart* chart, vgui_series_id series,
		size_t capacity);
	/* Fixed-capacity ring buffer; 0 disables the ring.  Shrinking keeps the
	 * newest points. */
	VGUI_API int vgui_chart_set_ring_capacity(vgui_chart* chart,
		vgui_series_id series, size_t capacity);

	/* Replaces all points.  Copies and sorts the input by x. */
	VGUI_API int vgui_chart_set_points(vgui_chart* chart,
		vgui_series_id series, const vgui_chart_point* points, size_t count);
	/* Batch append.  Rejects x regression and non-finite x values. */
	VGUI_API int vgui_chart_append_points(vgui_chart* chart,
		vgui_series_id series, const vgui_chart_point* points, size_t count);
	VGUI_API int vgui_chart_clear_points(vgui_chart* chart,
		vgui_series_id series);
	VGUI_API size_t vgui_chart_point_count(vgui_chart* chart,
		vgui_series_id series);

	/* Explicit x view.  Disables follow. */
	VGUI_API int vgui_chart_set_view(vgui_chart* chart, double x0, double x1);
	/* Returns 0 when the chart uses automatic/follow range. */
	VGUI_API int vgui_chart_get_view(vgui_chart* chart,
		double* x0, double* x1);
	/* Resets to follow when options.follow is set, otherwise full data. */
	VGUI_API int vgui_chart_reset_view(vgui_chart* chart);
	VGUI_API int vgui_chart_set_follow(vgui_chart* chart, int follow);
	VGUI_API int vgui_chart_get_follow(vgui_chart* chart);

	VGUI_API int vgui_chart_set_options(vgui_chart* chart,
		const vgui_chart_options* options);
	/* axis: 0 = x, 1 = y.  Only axis 0 is meaningful in P0; axis 1 sets the
	 * left Y axis range, tick count and formatting. */
	VGUI_API int vgui_chart_set_axis(vgui_chart* chart, int axis,
		const vgui_chart_axis_desc* desc);
	VGUI_API int vgui_chart_set_axis_format(vgui_chart* chart, int axis,
		vgui_chart_format_fn format, void* user);
	/* Overrides the derived palette.  count is clamped to 10. */
	VGUI_API int vgui_chart_set_palette(vgui_chart* chart,
		const vgui_color* colors, size_t count);
	VGUI_API int vgui_chart_set_theme(vgui_chart* chart,
		const vgui_chart_theme* theme);

	/* Render statistics from the last draw. */
	VGUI_API int vgui_chart_get_stats(vgui_chart* chart,
		vgui_chart_stats* stats);

	/* CSV long format: series,x,y.  Streamed through write. */
	VGUI_API int vgui_chart_export_csv(vgui_chart* chart, uint32_t flags,
		vgui_chart_write_fn write, void* user);

	VGUI_API const char* vgui_chart_last_error(vgui_chart* chart);

	/* ---------------------------------------------------------------------- */
	/* Gauge                                                                   */
	/* ---------------------------------------------------------------------- */

	typedef struct vgui_gauge_zone {
		double from;
		double to;
		vgui_color color;		/* 0 = derive from chart theme */
		const char* label;
	} vgui_gauge_zone;

	typedef struct vgui_gauge_options {
		uint32_t struct_size;
		double min;
		double max;
		double start_angle;		/* degrees; default 135 */
		double sweep;			/* degrees; default 270 */
		int show_ticks;
		int show_value;
		const char* label;
		const char* unit;
		const char* format_pattern;
		vgui_chart_format_fn format;
		void* format_user;
		const vgui_gauge_zone* zones;
		size_t zone_count;
	} vgui_gauge_options;

	VGUI_API void vgui_gauge_options_default(vgui_gauge_options* options);

	VGUI_API vgui_gauge* vgui_gauge_create(vgui_widget* parent);
	VGUI_API void vgui_gauge_destroy(vgui_gauge* gauge);
	VGUI_API vgui_widget* vgui_gauge_widget(vgui_gauge* gauge);
	VGUI_API int vgui_gauge_set_options(vgui_gauge* gauge,
		const vgui_gauge_options* options);
	VGUI_API int vgui_gauge_set_value(vgui_gauge* gauge, double value);
	VGUI_API double vgui_gauge_get_value(vgui_gauge* gauge);
	VGUI_API int vgui_gauge_set_theme(vgui_gauge* gauge,
		const vgui_chart_theme* theme);
	VGUI_API const char* vgui_gauge_last_error(vgui_gauge* gauge);

	/* ---------------------------------------------------------------------- */
	/* JSON dashboard                                                          */
	/* ---------------------------------------------------------------------- */

	VGUI_API vgui_dashboard* vgui_dashboard_create(vgui_widget* parent);
	VGUI_API void vgui_dashboard_destroy(vgui_dashboard* dashboard);
	VGUI_API vgui_widget* vgui_dashboard_widget(vgui_dashboard* dashboard);

	/* Parses schema version 1 and rebuilds the dashboard.  On failure the
	 * previous dashboard is kept intact and error receives a path-qualified
	 * message.  error may be NULL. */
	VGUI_API int vgui_dashboard_load_json(vgui_dashboard* dashboard,
		const char* json, size_t len, char* error, size_t error_cap);
	/* Serialises the current configuration and view state. */
	VGUI_API int vgui_dashboard_save_json(vgui_dashboard* dashboard,
		vgui_chart_write_fn write, void* user);

	/* Global runtime data keys.  set_number updates KPI values and gauges whose
	 * value_key matches.  set_text updates KPI delta/subtitle text.  status
	 * controls the status label colour and text. */
	VGUI_API int vgui_dashboard_set_number(vgui_dashboard* dashboard,
		const char* key, double value);
	VGUI_API int vgui_dashboard_set_text(vgui_dashboard* dashboard,
		const char* key, const char* text);
	VGUI_API int vgui_dashboard_set_status(vgui_dashboard* dashboard,
		const char* key, int status, const char* text);

	/* Chart data by card id and series key.  The chart handle stays owned by
	 * the dashboard; do not destroy it separately. */
	VGUI_API int vgui_dashboard_append_points(vgui_dashboard* dashboard,
		const char* card, const char* series,
		const vgui_chart_point* points, size_t count);
	VGUI_API vgui_chart* vgui_dashboard_chart(vgui_dashboard* dashboard,
		const char* card);
	VGUI_API vgui_gauge* vgui_dashboard_gauge(vgui_dashboard* dashboard,
		const char* card);
	/* Forces layout of the dashboard tree. */
	VGUI_API void vgui_dashboard_layout(vgui_dashboard* dashboard);

	/* CSV long format: card,series,x,y. */
	VGUI_API int vgui_dashboard_export_csv(vgui_dashboard* dashboard,
		uint32_t flags, vgui_chart_write_fn write, void* user);

	VGUI_API const char* vgui_dashboard_last_error(vgui_dashboard* dashboard);

#ifdef __cplusplus
} /* extern "C" */
#endif
