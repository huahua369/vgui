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
			"{\"version\":1,\"layout\":{\"mode\":\"grid\"},\"cards\":[]}";
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

	vgui_destroy(ui);
	printf("%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}
