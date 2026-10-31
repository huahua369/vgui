/* sample/vgui_chart_test.c: pure C consumer smoke test for vgui_chart.
 *
 * The file is compiled as C99 so it catches any accidental C++ dependency in
 * the public chart header.  It runs headless: data management, JSON
 * configuration, persistence and CSV export do not need a renderer.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vgui/chart/vgui_chart.h>

static int g_pass;
static int g_fail;

static void check(int ok, const char* what)
{
	if (ok)
	{
		++g_pass;
		printf("PASS %s\n", what);
	}
	else
	{
		++g_fail;
		printf("FAIL %s\n", what);
	}
}

typedef struct text_buffer {
	char* data;
	size_t size;
	size_t capacity;
} text_buffer;

static int buffer_write(void* user, const char* data, size_t size)
{
	text_buffer* buffer = (text_buffer*)user;
	if (!buffer || (size > 0 && !data))
		return 0;
	if (buffer->size + size + 1 > buffer->capacity)
	{
		size_t capacity = buffer->capacity ? buffer->capacity : 4096;
		while (capacity < buffer->size + size + 1)
			capacity *= 2;
		char* memory = (char*)realloc(buffer->data, capacity);
		if (!memory)
			return 0;
		buffer->data = memory;
		buffer->capacity = capacity;
	}
	if (size > 0)
		memcpy(buffer->data + buffer->size, data, size);
	buffer->size += size;
	buffer->data[buffer->size] = '\0';
	return 1;
}

static void buffer_reset(text_buffer* buffer)
{
	free(buffer->data);
	buffer->data = NULL;
	buffer->size = 0;
	buffer->capacity = 0;
}

static size_t count_byte(const char* data, size_t size, char value)
{
	size_t count = 0;
	size_t i = 0;
	for (i = 0; i < size; ++i)
	{
		if (data[i] == value)
			++count;
	}
	return count;
}

static void set_visible(vgui_chart* chart, vgui_series_id series, int visible)
{
	vgui_chart_series_desc desc;
	vgui_chart_series_desc_default(&desc);
	desc.key = NULL;
	desc.name = NULL;
	desc.unit = NULL;
	desc.stack_group = NULL;
	desc.visible = visible;
	vgui_chart_update_series(chart, series, &desc);
}

static void test_chart_data(vgui_widget* root)
{
	vgui_chart* chart = vgui_chart_create(root);
	check(chart != NULL, "chart: create");
	if (!chart)
		return;

	vgui_chart_series_desc desc;
	vgui_chart_series_desc_default(&desc);
	desc.key = "latency";
	desc.name = "Latency";
	desc.kind = VGUI_CHART_LINE;
	desc.unit = "ms";
	vgui_series_id line = vgui_chart_add_series(chart, &desc);
	check(line != 0, "chart: add line series");

	desc.key = "ring";
	desc.name = "Ring";
	desc.kind = VGUI_CHART_BAR;
	vgui_series_id ring = vgui_chart_add_series(chart, &desc);
	check(ring != 0, "chart: add ring series");

	check(vgui_chart_reserve(chart, line, 100000) == 1,
		"chart: reserve 100000 points");

	vgui_chart_point* points = (vgui_chart_point*)malloc(
		sizeof(vgui_chart_point) * 100000);
	check(points != NULL, "chart: allocate 100000 points");
	if (!points)
	{
		vgui_chart_destroy(chart);
		return;
	}
	{
		size_t i = 0;
		for (i = 0; i < 100000; ++i)
		{
			points[i].x = (double)i * 0.1;
			points[i].y = sin((double)i * 0.01) * 50.0 + 50.0;
		}
	}
	check(vgui_chart_append_points(chart, line, points, 100000) == 1,
		"chart: append 100000 points");
	check(vgui_chart_point_count(chart, line) == 100000,
		"chart: stores 100000 points");

	{
		vgui_chart_point bad;
		bad.x = -1.0;
		bad.y = 1.0;
		check(vgui_chart_append_points(chart, line, &bad, 1) == 0,
			"chart: rejects x regression");
		check(vgui_chart_point_count(chart, line) == 100000,
			"chart: rejected append kept point count");
	}

	{
		text_buffer csv;
		memset(&csv, 0, sizeof(csv));
		check(vgui_chart_export_csv(chart, VGUI_CHART_EXPORT_HEADER,
			buffer_write, &csv) == 1, "chart: export 100k CSV");
		check(count_byte(csv.data, csv.size, '\n') == 100001,
			"chart: CSV contains all 100000 rows");
		buffer_reset(&csv);
	}

	{
		vgui_chart_point ring_points[10];
		vgui_chart_point extra[2];
		size_t i = 0;
		for (i = 0; i < 10; ++i)
		{
			ring_points[i].x = (double)i;
			ring_points[i].y = (double)(i * 2);
		}
		check(vgui_chart_set_ring_capacity(chart, ring, 5) == 1,
			"chart: set ring capacity");
		check(vgui_chart_set_points(chart, ring, ring_points, 10) == 1,
			"chart: ring set_points");
		check(vgui_chart_point_count(chart, ring) == 5,
			"chart: ring trims oldest points");
		extra[0].x = 10.0;
		extra[0].y = 20.0;
		extra[1].x = 11.0;
		extra[1].y = 22.0;
		check(vgui_chart_append_points(chart, ring, extra, 2) == 1,
			"chart: ring append");
		check(vgui_chart_point_count(chart, ring) == 5,
			"chart: ring keeps capacity");

		set_visible(chart, line, 0);
		{
			text_buffer csv;
			memset(&csv, 0, sizeof(csv));
			check(vgui_chart_export_csv(chart, VGUI_CHART_EXPORT_HEADER,
				buffer_write, &csv) == 1, "chart: ring CSV export");
			check(count_byte(csv.data, csv.size, '\n') == 6,
				"chart: ring CSV has 5 rows");
			check(strstr(csv.data, "\"ring\",7,14") != NULL,
				"chart: ring CSV starts at newest window");
			check(strstr(csv.data, "\"ring\",11,22") != NULL,
				"chart: ring CSV ends with newest point");
			buffer_reset(&csv);
		}
	}

	{
		text_buffer csv;
		memset(&csv, 0, sizeof(csv));
		set_visible(chart, line, 1);
		set_visible(chart, ring, 0);
		check(vgui_chart_export_csv(chart,
			VGUI_CHART_EXPORT_DECIMATED | VGUI_CHART_EXPORT_VISIBLE_ONLY,
			buffer_write, &csv) == 1, "chart: decimated export");
		check(count_byte(csv.data, csv.size, '\n') < 100000,
			"chart: decimation reduces rows");
		check(count_byte(csv.data, csv.size, '\n') > 100,
			"chart: decimated export keeps spikes");
		buffer_reset(&csv);
	}

	check(vgui_chart_set_view(chart, 0.0, 500.0) == 1,
		"chart: set explicit view");
	check(vgui_chart_get_follow(chart) == 0,
		"chart: explicit view disables follow");
	check(vgui_chart_reset_view(chart) == 1, "chart: reset view");

	vgui_chart_stats stats;
	memset(&stats, 0, sizeof(stats));
	stats.struct_size = (uint32_t)sizeof(stats);
	check(vgui_chart_get_stats(chart, &stats) == 1,
		"chart: stats query");

	free(points);
	vgui_chart_destroy(chart);
}

/* Export range edge cases: DECIMATED without VISIBLE_ONLY must still cover the
 * whole series, and bars must stay at raw sample resolution. */
static void test_chart_export_ranges(vgui_widget* root)
{
	vgui_chart* chart = vgui_chart_create(root);
	check(chart != NULL, "chart ranges: create");
	if (!chart)
		return;

	vgui_chart_series_desc desc;
	vgui_chart_series_desc_default(&desc);
	desc.key = "line-range";
	desc.kind = VGUI_CHART_LINE;
	vgui_series_id line = vgui_chart_add_series(chart, &desc);
	check(line != 0, "chart ranges: add line");

	vgui_chart_point points[100];
	size_t i = 0;
	for (i = 0; i < 100; ++i)
	{
		points[i].x = (double)i;
		points[i].y = (double)i;
	}
	check(vgui_chart_set_points(chart, line, points, 100) == 1,
		"chart ranges: line points");
	check(vgui_chart_set_view(chart, 0.0, 9.0) == 1,
		"chart ranges: line view");

	{
		text_buffer csv;
		memset(&csv, 0, sizeof(csv));
		check(vgui_chart_export_csv(chart, VGUI_CHART_EXPORT_DECIMATED,
			buffer_write, &csv) == 1,
			"chart ranges: decimated export without visible-only");
		check(count_byte(csv.data, csv.size, '\n') == 100,
			"chart ranges: decimated export spans all data");
		buffer_reset(&csv);

		memset(&csv, 0, sizeof(csv));
		check(vgui_chart_export_csv(chart,
			VGUI_CHART_EXPORT_DECIMATED | VGUI_CHART_EXPORT_VISIBLE_ONLY,
			buffer_write, &csv) == 1,
			"chart ranges: decimated visible-only export");
		check(count_byte(csv.data, csv.size, '\n') == 10,
			"chart ranges: visible-only export clamps to view");
		buffer_reset(&csv);
	}

	desc.key = "bar-range";
	desc.kind = VGUI_CHART_BAR;
	vgui_series_id bars = vgui_chart_add_series(chart, &desc);
	check(bars != 0, "chart ranges: add bars");

	{
		vgui_chart_point* bar_points = (vgui_chart_point*)malloc(
			sizeof(vgui_chart_point) * 5000);
		check(bar_points != NULL, "chart ranges: allocate bars");
		if (bar_points)
		{
			for (i = 0; i < 5000; ++i)
			{
				bar_points[i].x = (double)i;
				bar_points[i].y = (double)(i % 7);
			}
			check(vgui_chart_set_points(chart, bars, bar_points, 5000) == 1,
				"chart ranges: bar points");
			set_visible(chart, line, 0);
			{
				text_buffer csv;
				memset(&csv, 0, sizeof(csv));
				check(vgui_chart_export_csv(chart,
					VGUI_CHART_EXPORT_HEADER | VGUI_CHART_EXPORT_DECIMATED,
					buffer_write, &csv) == 1,
					"chart ranges: bar decimated export");
				check(count_byte(csv.data, csv.size, '\n') == 5001,
					"chart ranges: bars stay at raw resolution");
				buffer_reset(&csv);
			}
			free(bar_points);
		}
	}

	vgui_chart_destroy(chart);
}

static const char* k_dashboard_json =
	"{"
	"\"version\":1,"
	"\"title\":\"Operations\","
	"\"layout\":{\"mode\":\"stack\",\"gap\":8,\"padding\":10},"
	"\"cards\":["
	"{\"type\":\"kpi\",\"id\":\"cpu\",\"title\":\"CPU\","
	"\"value_key\":\"cpu.value\",\"delta_key\":\"cpu.delta\","
	"\"text_key\":\"cpu.text\",\"status_key\":\"cpu.status\","
	"\"format\":\"%.1f\",\"unit\":\"%\"},"
	"{\"type\":\"chart\",\"id\":\"load\",\"title\":\"Load\","
	"\"options\":{\"legend\":true,\"follow\":true,\"window\":60},"
	"\"axes\":{\"x\":{\"scale\":\"time\"},\"y\":{\"min\":0,\"max\":100}},"
	"\"series\":[{\"key\":\"rps\",\"name\":\"Requests\",\"kind\":\"line\"},"
	"{\"key\":\"errors\",\"kind\":\"bar\",\"stacked\":false}]},"
	"{\"type\":\"gauge\",\"id\":\"disk\",\"title\":\"Disk\","
	"\"value_key\":\"disk.value\","
	"\"gauge\":{\"min\":0,\"max\":100,"
	"\"zones\":[{\"from\":0,\"to\":70,\"color\":\"#3FB950\"},"
	"{\"from\":70,\"to\":100,\"color\":\"#F85149\"}]}}"
	"]"
	"}";

static void test_dashboard(vgui_widget* root)
{
	vgui_dashboard* dashboard = vgui_dashboard_create(root);
	check(dashboard != NULL, "dashboard: create");
	if (!dashboard)
		return;

	vgui_widget_set_pos(vgui_dashboard_widget(dashboard), 5, 7);
	vgui_widget_set_size(vgui_dashboard_widget(dashboard), 820, 560);

	char error[256];
	memset(error, 0, sizeof(error));
	check(vgui_dashboard_load_json(dashboard, k_dashboard_json,
		strlen(k_dashboard_json), error, sizeof(error)) == 1,
		"dashboard: load schema v1");
	check(error[0] == '\0', "dashboard: no load error");

	{
		float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
		vgui_widget_get_rect(vgui_dashboard_widget(dashboard), &x, &y, &w, &h);
		check(x == 5.0f && y == 7.0f && w == 820.0f && h == 560.0f,
			"dashboard: load preserves widget geometry");
	}

	vgui_chart* chart = vgui_dashboard_chart(dashboard, "load");
	vgui_gauge* gauge = vgui_dashboard_gauge(dashboard, "disk");
	check(chart != NULL, "dashboard: chart card lookup");
	check(gauge != NULL, "dashboard: gauge card lookup");
	if (!chart || !gauge)
	{
		vgui_dashboard_destroy(dashboard);
		return;
	}

	check(vgui_dashboard_set_number(dashboard, "cpu.value", 42.5) == 1,
		"dashboard: KPI number binding");
	check(vgui_dashboard_set_number(dashboard, "disk.value", 80.0) == 1,
		"dashboard: gauge number binding");
	check(fabs(vgui_gauge_get_value(gauge) - 80.0) < 1e-9,
		"dashboard: gauge value updated");
	check(vgui_dashboard_set_text(dashboard, "cpu.delta", "+1.2%") == 1,
		"dashboard: KPI delta binding");
	check(vgui_dashboard_set_text(dashboard, "cpu.text", "8 cores") == 1,
		"dashboard: KPI subtitle binding");
	check(vgui_dashboard_set_status(dashboard, "cpu.status",
		VGUI_CHART_STATUS_WARNING, "warm") == 1,
		"dashboard: KPI status binding");

	{
		vgui_chart_point points[16];
		size_t i = 0;
		for (i = 0; i < 16; ++i)
		{
			points[i].x = (double)i;
			points[i].y = 20.0 + (double)i;
		}
		check(vgui_dashboard_append_points(dashboard, "load", "rps",
			points, 16) == 1, "dashboard: append chart points");
		check(vgui_dashboard_append_points(dashboard, "load", "errors",
			points, 16) == 1, "dashboard: append second series");
	}

	{
		text_buffer csv;
		memset(&csv, 0, sizeof(csv));
		check(vgui_dashboard_export_csv(dashboard, VGUI_CHART_EXPORT_HEADER,
			buffer_write, &csv) == 1, "dashboard: CSV export");
		check(strncmp(csv.data, "card,series,x,y\n", 16) == 0,
			"dashboard: CSV header has card column");
		check(strstr(csv.data, "\"load\",\"rps\",") != NULL,
			"dashboard: CSV contains card and series");
		buffer_reset(&csv);
	}

	{
		text_buffer json;
		memset(&json, 0, sizeof(json));
		check(vgui_dashboard_save_json(dashboard, buffer_write, &json) == 1,
			"dashboard: save JSON");
		check(strstr(json.data, "\"version\": 1") != NULL,
			"dashboard: saved schema version");
		check(strstr(json.data, "\"load\"") != NULL,
			"dashboard: saved card id");
		check(vgui_dashboard_load_json(dashboard, json.data, json.size,
			error, sizeof(error)) == 1,
			"dashboard: reload saved JSON");
		check(vgui_dashboard_chart(dashboard, "load") != NULL,
			"dashboard: chart survives reload");
		{
			float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
			vgui_widget_get_rect(vgui_dashboard_widget(dashboard),
				&x, &y, &w, &h);
			check(x == 5.0f && y == 7.0f && w == 820.0f && h == 560.0f,
				"dashboard: reload preserves widget geometry");
		}
		buffer_reset(&json);
	}

	{
		const char* invalid =
			"{\"version\":1,\"layout\":{\"mode\":\"freeform\"},\"cards\":[]}";
		vgui_chart* before = vgui_dashboard_chart(dashboard, "load");
		memset(error, 0, sizeof(error));
		check(vgui_dashboard_load_json(dashboard, invalid, strlen(invalid),
			error, sizeof(error)) == 0,
			"dashboard: rejects unsupported layout");
		check(strstr(error, "layout.mode") != NULL,
			"dashboard: path-qualified layout error");
		check(vgui_dashboard_chart(dashboard, "load") == before,
			"dashboard: failed load keeps previous tree");
	}

	vgui_dashboard_destroy(dashboard);
}

/* P1: wrap/grid/columns layouts, card span, snapshot data and runtime
 * sections, PNG export through the host renderer hook. */

static void test_dashboard_layouts(vgui_widget* root)
{
	const char* grid_json =
		"{"
		"\"version\":1,"
		"\"layout\":{\"mode\":\"grid\",\"columns\":3,\"gap\":6,\"padding\":8},"
		"\"cards\":["
		"{\"type\":\"kpi\",\"id\":\"a\",\"title\":\"A\"},"
		"{\"type\":\"kpi\",\"id\":\"b\",\"title\":\"B\"},"
		"{\"type\":\"kpi\",\"id\":\"c\",\"title\":\"C\"},"
		"{\"type\":\"chart\",\"id\":\"g\",\"title\":\"G\","
		"\"series\":[{\"key\":\"v\",\"kind\":\"line\"}],\"span\":2},"
		"{\"type\":\"gauge\",\"id\":\"d\",\"title\":\"D\"}"
		"]"
		"}";

	vgui_dashboard* dashboard = vgui_dashboard_create(root);
	check(dashboard != NULL, "grid: create");
	if (!dashboard)
		return;

	vgui_widget_set_pos(vgui_dashboard_widget(dashboard), 4, 4);
	vgui_widget_set_size(vgui_dashboard_widget(dashboard), 900, 600);

	char error[256];
	memset(error, 0, sizeof(error));
	check(vgui_dashboard_load_json(dashboard, grid_json, strlen(grid_json),
		error, sizeof(error)) == 1, "grid: load layout");
	check(error[0] == '\0', "grid: no load error");
	check(vgui_dashboard_chart(dashboard, "g") != NULL,
		"grid: chart card lookup");
	check(vgui_dashboard_gauge(dashboard, "d") != NULL,
		"grid: gauge card lookup");

	{
		text_buffer json;
		memset(&json, 0, sizeof(json));
		check(vgui_dashboard_save_json(dashboard, buffer_write, &json) == 1,
			"grid: save");
		check(strstr(json.data, "\"mode\": \"grid\"") != NULL,
			"grid: saved layout mode");
		check(strstr(json.data, "\"columns\": 3") != NULL,
			"grid: saved column count");
		check(strstr(json.data, "\"span\": 2") != NULL,
			"grid: saved card span");
		check(vgui_dashboard_load_json(dashboard, json.data, json.size,
			error, sizeof(error)) == 1, "grid: reload saved JSON");
		check(error[0] == '\0', "grid: reload no error");
		buffer_reset(&json);
	}

	{
		/* span wider than the grid is clamped to the column count */
		const char* wide_span =
			"{\"version\":1,\"layout\":{\"mode\":\"grid\",\"columns\":2},"
			"\"cards\":[{\"type\":\"chart\",\"id\":\"x\","
			"\"series\":[{\"key\":\"v\"}],\"span\":9}]}";
		memset(error, 0, sizeof(error));
		check(vgui_dashboard_load_json(dashboard, wide_span,
			strlen(wide_span), error, sizeof(error)) == 1,
			"grid: clamps oversized span");
		{
			text_buffer json;
			memset(&json, 0, sizeof(json));
			check(vgui_dashboard_save_json(dashboard, buffer_write, &json)
				== 1, "grid: save clamped span");
			check(strstr(json.data, "\"span\": 2") != NULL,
				"grid: saved span clamped to columns");
			buffer_reset(&json);
		}
	}
	vgui_dashboard_destroy(dashboard);

	{
		const char* wrap_json =
			"{\"version\":1,\"layout\":{\"mode\":\"wrap\",\"gap\":4},"
			"\"cards\":[{\"type\":\"kpi\",\"id\":\"w1\"},"
			"{\"type\":\"kpi\",\"id\":\"w2\"},{\"type\":\"kpi\",\"id\":\"w3\"}]}";
		vgui_dashboard* wrap = vgui_dashboard_create(root);
		check(wrap != NULL, "wrap: create");
		if (wrap)
		{
			check(vgui_dashboard_load_json(wrap, wrap_json,
				strlen(wrap_json), error, sizeof(error)) == 1,
				"wrap: load layout");
			check(vgui_dashboard_load_json(wrap, grid_json, strlen(grid_json),
				error, sizeof(error)) == 1, "wrap: reload grid over wrap");
			vgui_dashboard_destroy(wrap);
		}

		const char* columns_json =
			"{\"version\":1,\"layout\":{\"mode\":\"columns\",\"columns\":2},"
			"\"cards\":[{\"type\":\"kpi\",\"id\":\"c1\"},"
			"{\"type\":\"chart\",\"id\":\"c2\",\"series\":[{\"key\":\"v\"}]},"
			"{\"type\":\"gauge\",\"id\":\"c3\"}]}";
		vgui_dashboard* columns = vgui_dashboard_create(root);
		check(columns != NULL, "columns: create");
		if (columns)
		{
			check(vgui_dashboard_load_json(columns, columns_json,
				strlen(columns_json), error, sizeof(error)) == 1,
				"columns: load layout");
			{
				text_buffer json;
				memset(&json, 0, sizeof(json));
				check(vgui_dashboard_save_json(columns, buffer_write, &json)
					== 1, "columns: save");
				check(strstr(json.data, "\"mode\": \"columns\"") != NULL,
					"columns: saved layout mode");
				check(strstr(json.data, "\"columns\": 2") != NULL,
					"columns: saved column count");
				buffer_reset(&json);
			}
			vgui_dashboard_destroy(columns);
		}
	}

	{
		vgui_dashboard* keep = vgui_dashboard_create(root);
		if (keep)
		{
			vgui_chart* before = NULL;
			vgui_chart* after = NULL;
			check(vgui_dashboard_load_json(keep, grid_json, strlen(grid_json),
				error, sizeof(error)) == 1, "keep: load grid");
			before = vgui_dashboard_chart(keep, "g");
			const char* bad_mode =
				"{\"version\":1,\"layout\":{\"mode\":\"masonry\"},\"cards\":[]}";
			memset(error, 0, sizeof(error));
			check(vgui_dashboard_load_json(keep, bad_mode, strlen(bad_mode),
				error, sizeof(error)) == 0, "keep: rejects unknown mode");
			check(strstr(error, "layout.mode") != NULL,
				"keep: path-qualified mode error");
			after = vgui_dashboard_chart(keep, "g");
			check(before != NULL && after == before,
				"keep: failed load keeps previous tree");
			vgui_dashboard_destroy(keep);
		}
	}
}

/* 2026-03-17 00:00:00 UTC, expressed without OS time functions. */
#define K_EPOCH_2026_03_17 1773705600.0

static void test_datetime_axis(vgui_widget* root)
{
	const char* datetime_json =
		"{"
		"\"version\":1,"
		"\"cards\":[{"
		"\"type\":\"chart\",\"id\":\"t\",\"title\":\"Timeline\","
		"\"axes\":{\"x\":{\"scale\":\"datetime\",\"timezone\":\"utc\","
		"\"ticks\":6},\"y\":{\"min\":0,\"max\":10}},"
		"\"series\":[{\"key\":\"s\",\"kind\":\"line\"}]"
		"}]}";

	vgui_chart* chart = vgui_chart_create(root);
	check(chart != NULL, "datetime: create chart");
	if (!chart)
		return;
	/* No draw has happened yet: the tick query reports nothing. */
	check(vgui_chart_get_axis_ticks(chart, 0, NULL, 0) == 0,
		"datetime: no ticks before first draw");
	vgui_chart_destroy(chart);

	vgui_dashboard* dashboard = vgui_dashboard_create(root);
	check(dashboard != NULL, "datetime: create dashboard");
	if (!dashboard)
		return;

	char error[256];
	memset(error, 0, sizeof(error));
	check(vgui_dashboard_load_json(dashboard, datetime_json,
		strlen(datetime_json), error, sizeof(error)) == 1,
		"datetime: load dashboard");
	check(error[0] == '\0', "datetime: no load error");

	vgui_chart* timed = vgui_dashboard_chart(dashboard, "t");
	check(timed != NULL, "datetime: chart card lookup");
	if (timed)
	{
		vgui_series_id series = vgui_chart_find_series(timed, "s");
		vgui_chart_point points[8];
		size_t i = 0;
		for (i = 0; i < 8; ++i)
		{
			points[i].x = K_EPOCH_2026_03_17 + (double)i * 43200.0;
			points[i].y = (double)i;
		}
		check(vgui_chart_append_points(timed, series, points, 8) == 1,
			"datetime: append epoch points");
	}

	{
		text_buffer json;
		memset(&json, 0, sizeof(json));
		check(vgui_dashboard_save_json(dashboard, buffer_write, &json) == 1,
			"datetime: save");
		check(strstr(json.data, "\"scale\": \"datetime\"") != NULL,
			"datetime: saved scale");
		check(strstr(json.data, "\"timezone\": \"local\"") == NULL,
			"datetime: utc zone omitted");
		check(vgui_dashboard_load_json(dashboard, json.data, json.size,
			error, sizeof(error)) == 1, "datetime: reload saved JSON");
		buffer_reset(&json);
	}

	{
		const char* local_json =
			"{\"version\":1,\"cards\":[{\"type\":\"chart\",\"id\":\"l\","
			"\"axes\":{\"x\":{\"scale\":\"datetime\","
			"\"timezone\":\"local\"}},\"series\":[{\"key\":\"s\"}]}]}";
		memset(error, 0, sizeof(error));
		check(vgui_dashboard_load_json(dashboard, local_json,
			strlen(local_json), error, sizeof(error)) == 1,
			"datetime: load local zone");
		{
			text_buffer json;
			memset(&json, 0, sizeof(json));
			check(vgui_dashboard_save_json(dashboard, buffer_write, &json)
				== 1, "datetime: save local zone");
			check(strstr(json.data, "\"timezone\": \"local\"") != NULL,
				"datetime: saved local zone");
			buffer_reset(&json);
		}
	}

	{
		const char* bad_zone =
			"{\"version\":1,\"cards\":[{\"type\":\"chart\",\"id\":\"l\","
			"\"axes\":{\"x\":{\"scale\":\"datetime\","
			"\"timezone\":\"mars\"}},\"series\":[{\"key\":\"s\"}]}]}";
		memset(error, 0, sizeof(error));
		check(vgui_dashboard_load_json(dashboard, bad_zone, strlen(bad_zone),
			error, sizeof(error)) == 0, "datetime: rejects unknown zone");
		check(strstr(error, "timezone") != NULL,
			"datetime: path-qualified zone error");
	}

	vgui_dashboard_destroy(dashboard);
}

static void test_snapshot(vgui_widget* root)
{
	vgui_dashboard* dashboard = vgui_dashboard_create(root);
	check(dashboard != NULL, "snapshot: create");
	if (!dashboard)
		return;
	vgui_widget_set_pos(vgui_dashboard_widget(dashboard), 6, 6);
	vgui_widget_set_size(vgui_dashboard_widget(dashboard), 820, 560);

	char error[256];
	memset(error, 0, sizeof(error));
	check(vgui_dashboard_load_json(dashboard, k_dashboard_json,
		strlen(k_dashboard_json), error, sizeof(error)) == 1,
		"snapshot: load base dashboard");

	vgui_chart* chart = vgui_dashboard_chart(dashboard, "load");
	check(chart != NULL, "snapshot: chart lookup");
	if (chart)
	{
		vgui_series_id rps = vgui_chart_find_series(chart, "rps");
		vgui_series_id errors = vgui_chart_find_series(chart, "errors");
		vgui_chart_point points[12];
		size_t i = 0;
		for (i = 0; i < 12; ++i)
		{
			points[i].x = (double)i * 0.5;
			points[i].y = (i == 3) ? NAN
				: (i == 5) ? INFINITY
				: (i == 7) ? -INFINITY
				: (double)i * 1.5;
		}
		check(vgui_chart_append_points(chart, rps, points, 12) == 1,
			"snapshot: append rps with non-finite y");

		check(vgui_chart_set_ring_capacity(chart, errors, 5) == 1,
			"snapshot: enable ring on errors");
		{
			vgui_chart_point ring[8];
			for (i = 0; i < 8; ++i)
			{
				ring[i].x = (double)i;
				ring[i].y = (double)(i * 10);
			}
			check(vgui_chart_append_points(chart, errors, ring, 8) == 1,
				"snapshot: fill ring series");
			check(vgui_chart_point_count(chart, errors) == 5,
				"snapshot: ring keeps newest 5");
		}
	}

	check(vgui_dashboard_set_number(dashboard, "cpu.value", 42.5) == 1,
		"snapshot: KPI value");
	check(vgui_dashboard_set_text(dashboard, "cpu.delta", "+1.2%") == 1,
		"snapshot: KPI delta");
	check(vgui_dashboard_set_text(dashboard, "cpu.text", "8 cores") == 1,
		"snapshot: KPI subtitle");
	check(vgui_dashboard_set_status(dashboard, "cpu.status",
		VGUI_CHART_STATUS_WARNING, "warm") == 1, "snapshot: KPI status");
	check(vgui_dashboard_set_number(dashboard, "disk.value", 78.0) == 1,
		"snapshot: gauge value");

	{
		text_buffer json;
		memset(&json, 0, sizeof(json));
		check(vgui_dashboard_save_json_ex(dashboard,
			VGUI_CHART_SAVE_DATA | VGUI_CHART_SAVE_RUNTIME, buffer_write,
			&json) == 1, "snapshot: save with data");
		check(strstr(json.data, "\"data\"") != NULL,
			"snapshot: JSON contains data section");
		check(strstr(json.data, "\"runtime\"") != NULL,
			"snapshot: JSON contains runtime section");
		check(strstr(json.data, "\"cpu.value\": 42.5") != NULL,
			"snapshot: runtime keeps KPI value");
		check(strstr(json.data, "\"disk.value\": 78") != NULL,
			"snapshot: runtime keeps gauge value");

		check(vgui_dashboard_load_json(dashboard, json.data, json.size,
			error, sizeof(error)) == 1, "snapshot: reload with data");
		check(error[0] == '\0', "snapshot: reload no error");
		buffer_reset(&json);
	}

	vgui_chart* reloaded = vgui_dashboard_chart(dashboard, "load");
	check(reloaded != NULL, "snapshot: reloaded chart");
	if (reloaded)
	{
		vgui_series_id rps = vgui_chart_find_series(reloaded, "rps");
		vgui_series_id errors = vgui_chart_find_series(reloaded, "errors");
		check(vgui_chart_point_count(reloaded, rps) == 12,
			"snapshot: rps points restored");
		check(vgui_chart_point_count(reloaded, errors) == 5,
			"snapshot: ring trimmed to capacity again");
		{
			text_buffer csv;
			memset(&csv, 0, sizeof(csv));
			check(vgui_dashboard_export_csv(dashboard,
				VGUI_CHART_EXPORT_HEADER, buffer_write, &csv) == 1,
				"snapshot: CSV of restored data");
			check(strstr(csv.data, "\"load\",\"rps\",1.5,") != NULL,
				"snapshot: CSV has restored rps row");
			check(strstr(csv.data, ",nan\n") != NULL,
				"snapshot: NaN survived the round trip");
			check(strstr(csv.data, ",inf\n") != NULL,
				"snapshot: +inf survived the round trip");
			check(strstr(csv.data, ",-inf\n") != NULL,
				"snapshot: -inf survived the round trip");
			check(strstr(csv.data, "\"load\",\"errors\",3,") != NULL,
				"snapshot: ring restored in old-to-new order");
			check(strstr(csv.data, "\"load\",\"errors\",2,") == NULL,
				"snapshot: dropped oldest ring points stay dropped");
			buffer_reset(&csv);
		}
	}

	{
		vgui_gauge* gauge = vgui_dashboard_gauge(dashboard, "disk");
		check(gauge != NULL, "snapshot: reloaded gauge");
		if (gauge)
			check(fabs(vgui_gauge_get_value(gauge) - 78.0) < 1e-9,
				"snapshot: gauge value restored from runtime");
	}

	{
		text_buffer json;
		memset(&json, 0, sizeof(json));
		check(vgui_dashboard_save_json_ex(dashboard,
			VGUI_CHART_SAVE_DATA | VGUI_CHART_SAVE_RUNTIME, buffer_write,
			&json) == 1, "snapshot: re-save reloaded dashboard");
		check(strstr(json.data, "\"cpu.value\": 42.5") != NULL,
			"snapshot: runtime survived reload");
		check(strstr(json.data, "\"cpu.delta\": \"+1.2%\"") != NULL,
			"snapshot: runtime text survived reload");
		check(strstr(json.data, "\"cpu.status\"") != NULL
			&& strstr(json.data, "\"warm\"") != NULL,
			"snapshot: runtime status survived reload");
		buffer_reset(&json);
	}

	{
		const char* bad_data =
			"{\"version\":1,\"cards\":[{\"type\":\"chart\",\"id\":\"c\","
			"\"series\":[{\"key\":\"v\"}]}],"
			"\"data\":{\"c\":{\"v\":[[1,2],[0,3]]}}}";
		vgui_chart* before = vgui_dashboard_chart(dashboard, "load");
		memset(error, 0, sizeof(error));
		check(vgui_dashboard_load_json(dashboard, bad_data, strlen(bad_data),
			error, sizeof(error)) == 0,
			"snapshot: rejects non-monotonic data");
		check(strstr(error, "data.c.v") != NULL,
			"snapshot: path-qualified data error");
		check(vgui_dashboard_chart(dashboard, "load") == before,
			"snapshot: failed data load keeps previous tree");
	}

	vgui_dashboard_destroy(dashboard);
}

static uint8_t* g_png_pixels;
static int g_png_size;

static int fake_png_render(void* user, vgui_widget* widget,
	int width, int height, vgui_color background,
	const uint8_t** out_rgba, int* out_stride)
{
	(void)user;
	(void)widget;
	uint8_t* pixels = (uint8_t*)malloc((size_t)width * (size_t)height * 4);
	if (!pixels)
		return 0;
	const uint8_t bg_r = (uint8_t)((background >> 16) & 0xFFu);
	const uint8_t bg_g = (uint8_t)((background >> 8) & 0xFFu);
	const uint8_t bg_b = (uint8_t)(background & 0xFFu);
	for (int y = 0; y < height; ++y)
	{
		for (int x = 0; x < width; ++x)
		{
			uint8_t* px = pixels + ((size_t)y * (size_t)width + (size_t)x) * 4;
			px[0] = (uint8_t)(bg_r ^ (uint8_t)(x * 7));
			px[1] = (uint8_t)(bg_g ^ (uint8_t)(y * 5));
			px[2] = (uint8_t)(bg_b ^ (uint8_t)((x + y) * 3));
			px[3] = 255;
		}
	}
	free(g_png_pixels);
	g_png_pixels = pixels;
	g_png_size = width * height * 4;
	*out_rgba = pixels;
	*out_stride = width * 4;
	return 1;
}

static void test_png_export(vgui_widget* root)
{
	vgui_chart* chart = vgui_chart_create(root);
	check(chart != NULL, "png: create chart");
	if (!chart)
		return;

	vgui_chart_png_desc desc;
	vgui_chart_png_desc_default(&desc);

	{
		text_buffer png;
		memset(&png, 0, sizeof(png));
		check(vgui_chart_export_png(chart, &desc, buffer_write, &png) == 0,
			"png: export without renderer fails");
		check(strstr(vgui_chart_last_error(chart), "renderer") != NULL,
			"png: explains missing renderer");
		check(png.data == NULL, "png: nothing written without renderer");
	}

	check(vgui_chart_set_png_renderer(fake_png_render, NULL) == 1,
		"png: install renderer hook");

	{
		vgui_series_id series;
		vgui_chart_series_desc sd;
		vgui_chart_series_desc_default(&sd);
		sd.key = "s";
		vgui_chart_point points[4];
		size_t i = 0;
		series = vgui_chart_add_series(chart, &sd);
		for (i = 0; i < 4; ++i)
		{
			points[i].x = (double)i;
			points[i].y = (double)(i * i);
		}
		vgui_chart_append_points(chart, series, points, 4);
	}

	desc.width = 320;
	desc.height = 200;
	desc.background = 0xFF202830;
	{
		text_buffer png;
		memset(&png, 0, sizeof(png));
		check(vgui_chart_export_png(chart, &desc, buffer_write, &png) == 1,
			"png: export chart");
		check(png.size > 100, "png: output has content");
		check(png.size >= 8 && memcmp(png.data,
			"\x89PNG\r\n\x1a\n", 8) == 0, "png: signature");
		{
			/* IHDR chunk: 00 00 00 0D "IHDR" then big-endian width/height */
			const unsigned char expect_header[16] = {
				0, 0, 0, 13, 'I', 'H', 'D', 'R',
				0, 0, 1, 64,	/* 320 */
				0, 0, 0, 200	/* 200 */
			};
			check(png.size >= 24 && memcmp(png.data + 8, expect_header, 16)
				== 0, "png: IHDR dimensions");
		}
		check(png.size >= 12 && memcmp(png.data + png.size - 8, "IEND", 4)
			== 0, "png: IEND trailer");
		buffer_reset(&png);
	}

	{
		text_buffer png;
		memset(&png, 0, sizeof(png));
		check(vgui_chart_export_png(chart, &desc, NULL, NULL) == 0,
			"png: rejects null writer");
		buffer_reset(&png);
	}

	vgui_chart_destroy(chart);

	{
		vgui_dashboard* dashboard = vgui_dashboard_create(root);
		check(dashboard != NULL, "png: create dashboard");
		if (dashboard)
		{
			char error[256];
			memset(error, 0, sizeof(error));
			check(vgui_dashboard_load_json(dashboard, k_dashboard_json,
				strlen(k_dashboard_json), error, sizeof(error)) == 1,
				"png: load dashboard");
			desc.width = 480;
			desc.height = 360;
			{
				text_buffer png;
				memset(&png, 0, sizeof(png));
				check(vgui_dashboard_export_png(dashboard, &desc,
					buffer_write, &png) == 1, "png: export dashboard");
				check(png.size >= 8 && memcmp(png.data,
					"\x89PNG\r\n\x1a\n", 8) == 0, "png: dashboard signature");
				buffer_reset(&png);
			}
			vgui_dashboard_destroy(dashboard);
		}
	}

	check(vgui_chart_set_png_renderer(NULL, NULL) == 1,
		"png: uninstall renderer hook");
}

int main(void)
{
	check(&vgui_chart_theme_from_theme != NULL, "chart: C ABI symbols");
	{
		vgui_chart_theme theme;
		memset(&theme, 0, sizeof(theme));
		vgui_chart_theme_high_contrast(&theme);
		check(theme.series[0] != 0, "chart: high contrast theme");
	}

	vgui_context* ui = vgui_create(NULL, 14.0f);
	check(ui != NULL, "chart: context create");
	if (!ui)
		return 1;
	vgui_context_set_size(ui, 1280, 720);
	vgui_widget* root = vgui_root(ui);
	check(root != NULL, "chart: root widget");
	if (!root)
	{
		vgui_destroy(ui);
		return 1;
	}

	test_chart_data(root);
	test_chart_export_ranges(root);
	test_dashboard(root);
	test_dashboard_layouts(root);
	test_datetime_axis(root);
	test_snapshot(root);
	test_png_export(root);

	vgui_destroy(ui);
	printf("%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
