// vgui_spreadsheet_demo.cpp - a real spreadsheet-style demo built on the
// S0+S1 extended table_view (multi-column sort, rectangular selection, TSV
// clipboard, transactional undo).  The host owns the data; the table is a
// pull-based, virtualized view that only queries visible rows on paint.
//
// Interactive:  vgui_spreadsheet_demo.exe
//   - click a header:  ascending -> descending -> unsorted (three-state)
//   - Shift+click a header: stack/remove a secondary sort key
//   - click / Shift+click / Shift+arrow / Ctrl+A: rectangular selection
//   - Ctrl+C / Ctrl+X / Ctrl+V: TSV copy / cut / paste (system clipboard)
//   - toolbar: undo/redo, sort presets, select row/column, add/delete row
// Bounded probe: VGUI_SPREADSHEET_SIM=1 vgui_spreadsheet_demo.exe
//   injects API-level assertions + a real header click, exits 0/1.
//   Returns 77 when a GPU/window cannot be created (ctest "skip").

#include "pch.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <ovg_c.h>
#include <vgui/backend/vgui_multiwindow.h>
#include <vgui/backend/vgui_sdl3.h>
#include <vgui/editor/table_view.h>
#include <vgui/vgui.h>

using namespace vgui_editor_kit;

static int g_fail = 0;
static int g_pass = 0;

static void check(bool ok, const char* what)
{
	if (ok) { ++g_pass; printf("PASS %s\n", what); }
	else { ++g_fail; printf("FAIL %s\n", what); }
}

/* ------------------------------------------------------------------------- */
/* Host-owned data model                                                     */
/* ------------------------------------------------------------------------- */

struct record
{
	std::string id;
	std::string name;
	std::string dept;		/* enumeration: sales / eng / ops / hr */
	int units = 0;
	double revenue = 0.0;
	bool active = true;
	std::string color = "#FF9FB0C4";
	std::string pos = "0, 0, 0";
};

static const char* k_depts[] = { "sales", "eng", "ops", "hr" };

static std::string make_color(int i)
{
	char buf[16];
	snprintf(buf, sizeof(buf), "#FF%02X%02X%02X",
		(unsigned char)(40 + (i * 53) % 200),
		(unsigned char)(40 + (i * 97) % 200),
		(unsigned char)(40 + (i * 31) % 200));
	return buf;
}

static std::string make_pos(int i)
{
	char buf[48];
	snprintf(buf, sizeof(buf), "%.1f, %.1f, %.1f",
		(i % 97) * 1.5f, (i % 53) * 2.25f, (i % 31) * 0.75f);
	return buf;
}

static std::vector<record> make_rows(std::size_t count)
{
	std::vector<record> rows;
	rows.reserve(count);
	for (std::size_t i = 0; i < count; ++i)
	{
		record r;
		char id[32];
		snprintf(id, sizeof(id), "R%05d", (int)i);
		r.id = id;
		r.name = std::string("Item ") + id;
		r.dept = k_depts[i % 4];
		r.units = (int)((i * 37) % 1000);
		r.revenue = (double)r.units * (1.5 + (i % 17) * 0.35);
		r.active = (i % 5) != 0;
		r.color = make_color((int)i);
		r.pos = make_pos((int)i);
		rows.push_back(std::move(r));
	}
	return rows;
}

/* ------------------------------------------------------------------------- */
/* Demo state + toolbar callbacks                                            */
/* ------------------------------------------------------------------------- */

struct app_state
{
	std::vector<record> rows;
	command_stack commands;
	table_view table;
	vgui_context* ui = nullptr;
	vgui_widget* status = nullptr;
	vgui_widget* sort_info = nullptr;
	char note[160] = "ready";

	record* row_at(std::size_t row)
	{
		return row < rows.size() ? &rows[row] : nullptr;
	}
	void set_note(const char* text)
	{
		snprintf(note, sizeof(note), "%s", text);
	}
	void refresh_status();
};

/* Heap-allocated so the table_view can be destroyed BEFORE the UI context:
 * its destructor touches widgets/context, which are gone by static teardown. */
static app_state* g = nullptr;

void app_state::refresh_status()
{
	const table_selection& sel = g->table.selection();
	char buf[320];
	if (!sel.valid())
		snprintf(buf, sizeof(buf),
			"rows=%zu  visible=%zu  cell_queries(paint)=%zu  selection: none",
			g->table.row_count(), g->table.display_row_count(),
			g->table.last_render_cell_queries());
	else
		snprintf(buf, sizeof(buf),
			"rows=%zu  visible=%zu  cell_queries(paint)=%zu  selection: r%zu-r%zu c%d-c%d (%zux%zu)",
			g->table.row_count(), g->table.display_row_count(),
			g->table.last_render_cell_queries(),
			sel.first_row() + 1, sel.last_row() + 1,
			sel.first_column() + 1, sel.last_column() + 1,
			sel.row_span(), sel.column_span());
	if (g->status) vgui_widget_set_text(g->status, buf);

	const std::vector<table_sort_spec>& specs = g->table.sort_specs();
	std::string s = "sort: ";
	if (specs.empty())
		s += "none";
	else
		for (std::size_t i = 0; i < specs.size(); ++i)
		{
			if (i) s += "  |  ";
			s += std::to_string(i + 1) + "." +
				g->table.columns()[(std::size_t)specs[i].column].label +
				(specs[i].ascending ? " ^" : " v");
		}
	snprintf(buf, sizeof(buf), "%s  |  undo=%zu redo=%zu  |  %s",
		s.c_str(), g->commands.undo_count(), g->commands.redo_count(), g->note);
	if (g->sort_info) vgui_widget_set_text(g->sort_info, buf);
}

static void on_button(vgui_widget* w, const vgui_event* e, void* user)
{
	(void)w;
	if (e->type != VGUI_EVENT_CLICK) return;
	const char* id = (const char*)user;

	if (std::strcmp(id, "undo") == 0)
	{
		g->commands.undo();
		g->table.refresh();
		g->set_note("undo");
	}
	else if (std::strcmp(id, "redo") == 0)
	{
		g->commands.redo();
		g->table.refresh();
		g->set_note("redo");
	}
	else if (std::strcmp(id, "sort_name") == 0)
	{
		g->table.set_sort(1, true);
		g->set_note("sort Name asc");
	}
	else if (std::strcmp(id, "sort_revenue") == 0)
	{
		g->table.set_sort(4, false);
		g->set_note("sort Revenue desc");
	}
	else if (std::strcmp(id, "sort_multi") == 0)
	{
		/* Primary Dept asc, secondary Revenue desc. */
		g->table.set_sort_specs({ { 2, true }, { 4, false } });
		g->set_note("multi-sort Dept ^ / Revenue v");
	}
	else if (std::strcmp(id, "sort_clear") == 0)
	{
		g->table.clear_sort();
		g->set_note("sort cleared");
	}
	else if (std::strcmp(id, "sel_all") == 0)
	{
		g->table.select_all();
		g->set_note("select all");
	}
	else if (std::strcmp(id, "sel_row") == 0)
	{
		if (g->table.selected_cell().valid())
			g->table.select_row(g->table.selected_cell().row);
		g->set_note("select row");
	}
	else if (std::strcmp(id, "sel_col") == 0)
	{
		if (g->table.selected_cell().valid())
			g->table.select_column(g->table.selected_cell().column);
		g->set_note("select column");
	}
	else if (std::strcmp(id, "sel_clear") == 0)
	{
		g->table.clear_selection();
		g->set_note("selection cleared");
	}
	else if (std::strcmp(id, "copy") == 0)
	{
		const std::string tsv = g->table.copy_selection_to_clipboard();
		g->set_note(tsv.empty() ? "copy: no selection" : "copy: TSV to clipboard");
	}
	else if (std::strcmp(id, "cut") == 0)
	{
		g->set_note(g->table.cut_selection()
			? "cut: TSV + clear (one undo)" : "cut: no selection");
	}
	else if (std::strcmp(id, "paste") == 0)
	{
		std::string err;
		const bool ok = g->table.paste_text(get_clipboard(), &err);
		g->set_note(ok ? "paste: TSV applied (one undo)"
			: (err.empty() ? "paste: nothing to paste" : err.c_str()));
	}
	else if (std::strcmp(id, "add_row") == 0)
	{
		g->rows.push_back(make_rows(g->rows.size() + 1).back());
		g->rows.back().id = "N" + std::to_string(g->rows.size());
		g->rows.back().name = "New " + g->rows.back().id;
		g->table.refresh();
		g->set_note("row added");
	}
	else if (std::strcmp(id, "del_row") == 0)
	{
		const table_cell cell = g->table.selected_cell();
		if (cell.valid() && cell.row < g->rows.size())
		{
			const std::size_t index = cell.row;
			const record removed = g->rows[index];
			g->rows.erase(g->rows.begin() + (std::ptrdiff_t)index);
			g->commands.push(std::make_unique<function_command>(
				"Delete Row", []() {},
				[index, removed]() {
					g->rows.insert(g->rows.begin() + (std::ptrdiff_t)index, removed);
					g->table.refresh();
				}));
			g->table.refresh();
			g->set_note("row deleted (undo restores)");
		}
		else
			g->set_note("delete: select a row first");
	}
	g->refresh_status();
}

static vgui_widget* make_button(vgui_widget* parent, const char* text,
	const char* action)
{
	vgui_widget* b = vgui_widget_add(parent, VGUI_BUTTON, text);
	vgui_widget_on_event(b, on_button, (void*)action);
	return b;
}

static vgui_widget* make_toolbar(vgui_context* ui)
{
	vgui_widget* bar = vgui_widget_add(vgui_root(ui), VGUI_PANEL, nullptr);
	vgui_widget_set_layout(bar, VGUI_LAYOUT_ROW);
	vgui_widget_set_padding(bar, 8, 6, 8, 6);
	vgui_widget_set_gap(bar, 6);
	vgui_widget_set_align_items(bar, VGUI_ALIGN_STRETCH);
	vgui_widget_set_size(bar, 0, 38);
	vgui_widget_set_align_self(bar, VGUI_ALIGN_STRETCH);
	vgui_widget_set_background(bar, 0xFF1C2530);

	make_button(bar, "Undo", "undo");
	make_button(bar, "Redo", "redo");
	vgui_widget_add(bar, VGUI_SEPARATOR, nullptr);
	make_button(bar, "Name ^", "sort_name");
	make_button(bar, "Revenue v", "sort_revenue");
	make_button(bar, "Multi-sort", "sort_multi");
	make_button(bar, "Clear sort", "sort_clear");
	vgui_widget_add(bar, VGUI_SEPARATOR, nullptr);
	make_button(bar, "All", "sel_all");
	make_button(bar, "Row", "sel_row");
	make_button(bar, "Col", "sel_col");
	make_button(bar, "None", "sel_clear");
	vgui_widget_add(bar, VGUI_SEPARATOR, nullptr);
	make_button(bar, "Copy", "copy");
	make_button(bar, "Cut", "cut");
	make_button(bar, "Paste", "paste");
	vgui_widget_add(bar, VGUI_SEPARATOR, nullptr);
	make_button(bar, "+ Row", "add_row");
	make_button(bar, "- Row", "del_row");
	return bar;
}

/* ------------------------------------------------------------------------- */
/* Table wiring                                                              */
/* ------------------------------------------------------------------------- */

static void configure_table(vgui_context* ui)
{
	g->table.build(vgui_root(ui));
	g->table.set_command_stack(&g->commands);
	vgui_widget_set_size(g->table.widget(), 0, 0);
	vgui_widget_set_grow(g->table.widget(), 1.0f);
	vgui_widget_set_align_self(g->table.widget(), VGUI_ALIGN_STRETCH);

	table_source source;
	source.row_count = []() { return g->rows.size(); };
	source.row_id = [](std::size_t row) {
		return (object_id)(row + 1);
	};
	source.cell_text = [](std::size_t row,
		const std::string& column) -> std::string {
		const record* r = g->row_at(row);
		if (!r) return {};
		if (column == "id") return r->id;
		if (column == "name") return r->name;
		if (column == "dept") return r->dept;
		if (column == "units") return std::to_string(r->units);
		if (column == "revenue")
		{
			char buf[48];
			snprintf(buf, sizeof(buf), "%.2f", r->revenue);
			return buf;
		}
		if (column == "active") return r->active ? "true" : "false";
		if (column == "color") return r->color;
		if (column == "pos") return r->pos;
		return {};
	};
	source.cell_style = [](std::size_t row,
		const std::string& column) -> table_cell_style {
		(void)column;
		table_cell_style style;
		const record* r = g->row_at(row);
		if (r && !r->active)
			style.text_color = 0xFFC97A7A;	/* inactive rows: soft red */
		return style;
	};
	g->table.set_source(source);

	g->table.set_clipboard_handlers(
		[]() -> std::string { return get_clipboard(); },
		[](const std::string& text) { set_clipboard(text.c_str()); });

	std::vector<table_column> columns(8);
	columns[0].id = "id";
	columns[0].label = "ID";
	columns[0].width = 90.0f;
	columns[0].editable = false;

	columns[1].id = "name";
	columns[1].label = "Name";
	columns[1].width = 150.0f;
	columns[1].set_value = [](std::size_t row, const std::string& value) {
		record* r = g->row_at(row);
		if (!r || value.empty()) return false;
		r->name = value;
		return true;
	};

	columns[2].id = "dept";
	columns[2].label = "Dept";
	columns[2].width = 100.0f;
	columns[2].editor.type = property_type::enumeration;
	columns[2].editor.choices = { "Sales", "Eng", "Ops", "HR" };
	columns[2].editor.choice_values = { "sales", "eng", "ops", "hr" };
	columns[2].set_value = [](std::size_t row, const std::string& value) {
		record* r = g->row_at(row);
		if (!r) return false;
		r->dept = value;
		return true;
	};

	columns[3].id = "units";
	columns[3].label = "Units";
	columns[3].width = 90.0f;
	columns[3].alignment = table_alignment::right;
	columns[3].editor.type = property_type::integer;
	columns[3].editor.has_range = true;
	columns[3].editor.min_value = 0.0;
	columns[3].editor.max_value = 9999.0;
	columns[3].set_value = [](std::size_t row, const std::string& value) {
		record* r = g->row_at(row);
		if (!r || value.empty()) return false;
		r->units = std::atoi(value.c_str());
		r->revenue = (double)r->units * (1.5 + (row % 17) * 0.35);
		return true;
	};

	columns[4].id = "revenue";
	columns[4].label = "Revenue";
	columns[4].width = 120.0f;
	columns[4].alignment = table_alignment::right;
	columns[4].editor.type = property_type::number;
	columns[4].set_value = [](std::size_t row, const std::string& value) {
		record* r = g->row_at(row);
		if (!r || value.empty()) return false;
		r->revenue = std::atof(value.c_str());
		return true;
	};

	columns[5].id = "active";
	columns[5].label = "Active";
	columns[5].width = 80.0f;
	columns[5].editor.type = property_type::boolean;
	columns[5].set_value = [](std::size_t row, const std::string& value) {
		record* r = g->row_at(row);
		if (!r) return false;
		r->active = value == "true";
		return true;
	};

	columns[6].id = "color";
	columns[6].label = "Color";
	columns[6].width = 120.0f;
	columns[6].editor.type = property_type::color;
	columns[6].set_value = [](std::size_t row, const std::string& value) {
		record* r = g->row_at(row);
		if (!r) return false;
		r->color = value;
		return true;
	};

	columns[7].id = "pos";
	columns[7].label = "Position";
	columns[7].width = 170.0f;
	columns[7].editor.type = property_type::vector3;
	columns[7].set_value = [](std::size_t row, const std::string& value) {
		record* r = g->row_at(row);
		if (!r) return false;
		r->pos = value;
		return true;
	};
	g->table.set_columns(std::move(columns));

	g->table.set_edit_listener([](const table_cell& cell,
		const std::string& value) {
		char buf[128];
		snprintf(buf, sizeof(buf), "edited r%zu c%d = %s",
			cell.row + 1, cell.column + 1, value.c_str());
		g->set_note(buf);
	});
	g->table.set_selection_range_listener([](const table_selection&) {
		g->refresh_status();
	});
	g->table.refresh();
}

/* ------------------------------------------------------------------------- */
/* Probe                                                                     */
/* ------------------------------------------------------------------------- */

static void run_probe()
{
	check(g->rows.size() == 5000, "host model has 5000 rows");
	check(g->table.row_count() == 5000, "table sees 5000 rows");
	check(g->table.column_count() == 8, "table has 8 typed columns");

	/* S0: three-state plain sort via public API semantics. */
	check(g->table.set_sort(1, true) && g->table.sort_column() == 1 &&
		g->table.sort_ascending(), "set_sort Name ascending");
	check(g->table.set_sort(1, false) && g->table.sort_ascending() == false,
		"set_sort Name descending");
	g->table.clear_sort();
	check(g->table.sort_specs().empty(), "clear_sort empties specs");

	/* S0: multi-column sort + v2 state round-trip. */
	check(g->table.set_sort_specs({ { 2, true }, { 4, false } }) &&
		g->table.sort_specs().size() == 2, "multi-column sort accepted");
	check(!g->table.set_sort_specs({ { 2, true }, { 2, false } }),
		"duplicate sort column rejected");
	const table_state saved = g->table.capture_state();
	check(saved.version == 2 && saved.sorts.size() == 2,
		"capture_state writes v2 with 2 keys");
	std::string json = table_state_to_json(saved);
	table_state loaded;
	std::string jerr;
	check(table_state_from_json(json, loaded, &jerr) &&
		loaded.sorts.size() == 2, "table_state JSON round-trip");
	g->table.apply_state(loaded);
	check(g->table.sort_specs().size() == 2, "apply_state restores multi-sort");
	g->table.clear_sort();

	/* S1: rectangular selection + TSV copy. */
	check(g->table.select_range({ 0, 1 }, { 2, 3 }), "select 3x3 range");
	const table_selection& sel = g->table.selection();
	check(sel.row_span() == 3 && sel.column_span() == 3,
		"selection normalized to 3x3");
	const std::string tsv = g->table.copy_selection();
	int tabs = 0, newlines = 0;
	for (char c : tsv) { if (c == '\t') ++tabs; if (c == '\n') ++newlines; }
	check(tabs >= 6 && newlines >= 2, "copy_selection emits TSV grid");

	/* S1: paste as one undo record, undo restores. */
	const std::string old_name = g->rows[0].name;
	std::string perr;
	check(g->table.paste_text("P0\tP1\tP2\nP3\tP4\tP5\nP6\tP7\tP8", &perr),
		"paste_text applies TSV");
	check(g->rows[0].name == "P0" && g->rows[2].name == "P6",
		"paste wrote first and last cell");
	check(g->commands.undo_count() == 1, "paste is a single undo record");
	check(g->commands.undo(), "undo succeeds");
	check(g->rows[0].name == old_name, "undo restored the original value");

	/* Virtualization: painted rows are far fewer than total. */
	check(g->table.last_render_cell_queries() < 500,
		"paint queries only visible cells (virtualized)");
}

/* ------------------------------------------------------------------------- */
/* Main                                                                      */
/* ------------------------------------------------------------------------- */

int main()
{
	setvbuf(stdout, nullptr, _IONBF, 0);

	const char* sim_env = std::getenv("VGUI_SPREADSHEET_SIM");
	const bool self_test = sim_env && std::strcmp(sim_env, "1") == 0;
	const int max_frames = self_test ? 90 : 0;

	auto* wg = new app_mgr();
	wg->set_msaa_samples(4);
	font_familys_t* familys = new_font_family(wg->font_ctx,
		"微软雅黑,Segoe UI,Consolas", 0);
	auto* cb = new_ctx_cb();
	if (!wg->init_gpu(true))
	{
		printf("SKIP init_gpu failed\n");
		delete_font_family(familys);
		free_ctx_cb(cb);
		delete wg;
		return 77;
	}

	const int win_w = 1180, win_h = 760;
	os_window* main_win = wg->create("vgui spreadsheet demo - S0/S1 extended table_view",
		win_w, win_h, 0, true);
	if (!main_win)
	{
		printf("SKIP window creation failed\n");
		delete_font_family(familys);
		free_ctx_cb(cb);
		delete wg;
		return 77;
	}

	g = new app_state();
	g->rows = make_rows(5000);
	g->ui = vgui_create(familys, 16.0f);
	vgui_context_set_size(g->ui, win_w, win_h);
	{
		vgui_widget* root = vgui_root(g->ui);
		vgui_widget_set_layout(root, VGUI_LAYOUT_COLUMN);
		vgui_widget_set_padding(root, 0, 0, 0, 0);
		vgui_widget_set_gap(root, 0);
		vgui_widget_set_align_items(root, VGUI_ALIGN_STRETCH);
	}

	vgui_widget* bar = make_toolbar(g->ui);
	(void)bar;
	configure_table(g->ui);

	vgui_widget* footer = vgui_widget_add(vgui_root(g->ui), VGUI_PANEL, nullptr);
	vgui_widget_set_layout(footer, VGUI_LAYOUT_COLUMN);
	vgui_widget_set_padding(footer, 8, 4, 8, 4);
	vgui_widget_set_gap(footer, 2);
	vgui_widget_set_size(footer, 0, 46);
	vgui_widget_set_align_self(footer, VGUI_ALIGN_STRETCH);
	g->status = vgui_widget_add(footer, VGUI_LABEL, "");
	g->sort_info = vgui_widget_add(footer, VGUI_LABEL, "");
	vgui_widget_set_font_size(g->sort_info, 13);
	g->refresh_status();

	{
		multiwindow_host host(wg, cb);
		if (!host.attach(main_win, g->ui))
		{
			printf("SKIP host attach failed\n");
			vgui_context* dying_ui = g->ui;
			delete g; g = nullptr;	/* table_view dies while context alive */
			vgui_destroy(dying_ui);
			main_win->viewport->ui = nullptr;
			wg->wait_gpu();
			delete_font_family(familys);
			free_ctx_cb(cb);
			delete wg;
			return 77;
		}

		FpsCounter fps;
		int frame = 0;
		bool running = true;
		bool probe_done = false;
		bool header_click_done = false;

		while (running)
		{
			if (wg->get_event() < 0)
				running = false;
			const float delta = fps.tick();

			if (self_test && frame == 3 && !probe_done)
			{
				probe_done = true;
				run_probe();
			}

			if (self_test && frame == 8 && !header_click_done)
			{
				/* Real input: click the "Name" header to flip sort state. */
				header_click_done = true;
				float hx = 0, hy = 0, hw = 0, hh = 0;
				if (g->table.header_rect(1, &hx, &hy, &hw, &hh) && hw > 0)
				{
					const float cx = hx + hw * 0.5f;
					const float cy = hy + hh * 0.5f;
					vgui_input_mouse_move(g->ui, cx, cy);
					vgui_input_mouse_down(g->ui, 1, cx, cy, 1);
					vgui_input_mouse_up(g->ui, 1, cx, cy, 1);
					check(g->table.sort_column() == 1,
						"real header click sorts Name");
				}
				else
					check(false, "real header click sorts Name (no rect)");
			}

			if (!self_test && (frame % 30) == 0)
				g->refresh_status();

			host.begin_frame(delta);
			host.draw_all();
			host.end_frame();

			++frame;
			if (max_frames > 0 && frame >= max_frames)
				running = false;
		}

		if (self_test)
			printf("SELFTEST %s (%d pass, %d fail)\n",
				g_fail ? "FAIL" : "PASS", g_pass, g_fail);
		else
			printf("interactive mode: click headers, Shift+click to stack sorts, "
				"Ctrl+C/X/V to copy/cut/paste.\n");

		/* Destroy the table_view (heap app_state) while the context is still
		 * alive: its destructor detaches custom draw/event callbacks and
		 * destroys popup widgets.  Then tear down the UI context. */
		const vgui_context* main_ui = g->ui;
		delete g; g = nullptr;
		for (const multiwindow_slot& s : host.slots())
			if (s.ui == main_ui)
			{
				vgui_destroy(s.ui);
				s.window->viewport->ui = nullptr;
			}
	}

	wg->wait_gpu();
	delete_font_family(familys);
	free_ctx_cb(cb);
	delete wg;

	return self_test ? (g_fail ? 1 : 0) : 0;
}
