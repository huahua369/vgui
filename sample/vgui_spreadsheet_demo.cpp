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
//   injects API-level assertions + a real header click + on-demand render
//   assertions (idle frames submit nothing, changes repaint), exits 0/1.
//   Returns 77 when a GPU/window cannot be created (ctest "skip").
// Rendering: on-demand mode is ON.  Idle frames skip clear/update/draw/submit;
//   input, sort/selection/clipboard changes and status text updates mark the
//   window dirty and trigger exactly one repaint (plus settle frames).

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

/* Programmatic 32x20 RGBA badge used by the in-cell image column.  The host
 * owns the texture; table_view only borrows it during paint. */
static vgui_texture* make_badge_texture(vgui_context* ui)
{
	constexpr int w = 32, h = 20;
	std::vector<uint32_t> pixels((std::size_t)w * h);
	for (int y = 0; y < h; ++y)
	{
		for (int x = 0; x < w; ++x)
		{
			const uint8_t r = (uint8_t)(40 + x * 6);
			const uint8_t g = (uint8_t)(90 + y * 7);
			const uint8_t b = 200;
			const uint8_t a = (x == 0 || y == 0 || x == w - 1 || y == h - 1)
				? 255 : 220;
			pixels[(std::size_t)y * w + x] =
				(uint32_t)a << 24 | (uint32_t)b << 16 | (uint32_t)g << 8 | r;
		}
	}
	vgui_texture_desc desc = {};
	desc.width = w;
	desc.height = h;
	desc.stride = w * 4;
	desc.format = VGUI_TEXTURE_FORMAT_RGBA8;
	desc.pixels = pixels.data();
	return vgui_texture_create(ui, &desc);
}

/* Programmatic 96x64 texture used by the floating drawing overlay.  A simple
 * framed gradient makes the anchor rect and fit mode easy to see. */
static vgui_texture* make_float_texture(vgui_context* ui)
{
	constexpr int w = 96, h = 64;
	std::vector<uint32_t> pixels((std::size_t)w * h);
	for (int y = 0; y < h; ++y)
	{
		for (int x = 0; x < w; ++x)
		{
			const uint8_t r = (uint8_t)(30 + x * 2);
			const uint8_t g = (uint8_t)(180 - y);
			const uint8_t b = (uint8_t)(120 + y * 2);
			const uint8_t a = (x < 3 || y < 3 || x >= w - 3 || y >= h - 3)
				? 255 : 210;
			pixels[(std::size_t)y * w + x] =
				(uint32_t)a << 24 | (uint32_t)b << 16 | (uint32_t)g << 8 | r;
		}
	}
	vgui_texture_desc desc = {};
	desc.width = w;
	desc.height = h;
	desc.stride = w * 4;
	desc.format = VGUI_TEXTURE_FORMAT_RGBA8;
	desc.pixels = pixels.data();
	return vgui_texture_create(ui, &desc);
}

struct app_state
{
	std::vector<record> rows;
	command_stack commands;
	table_view table;
	vgui_context* ui = nullptr;
	vgui_widget* status = nullptr;
	vgui_widget* sort_info = nullptr;
	vgui_texture* badge_texture = nullptr;
	vgui_texture* float_texture = nullptr;
	std::vector<table_drawing> drawings;
	int badge_image_queries = 0;
	int drawing_queries = 0;
	char drawing_selected[64] = "none";
	char note[160] = "ready";

	~app_state()
	{
		/* Textures belong to the UI context, which is still alive when the
		 * heap app_state is deleted in main(). */
		if (badge_texture)
		{
			vgui_texture_destroy(badge_texture);
			badge_texture = nullptr;
		}
		if (float_texture)
		{
			vgui_texture_destroy(float_texture);
			float_texture = nullptr;
		}
	}

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
	g->badge_texture = make_badge_texture(ui);
	g->float_texture = make_float_texture(ui);
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
		if (column == "badge")
		{
			char buf[32];
			snprintf(buf, sizeof(buf), "badge://%zu", row);
			return buf;
		}
		return {};
	};
	source.cell_image = [](std::size_t row,
		const std::string& column) -> table_cell_image {
		table_cell_image image;
		if (column != "badge" || !g->badge_texture)
			return image;
		++g->badge_image_queries;
		image.texture = g->badge_texture;
		/* Rotate through the four fits so the demo shows all of them. */
		switch (row % 4)
		{
		case 0: image.fit = table_image_fit::contain; break;
		case 1: image.fit = table_image_fit::cover; break;
		case 2: image.fit = table_image_fit::stretch; break;
		default: image.fit = table_image_fit::none; break;
		}
		return image;
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

	/* Floating drawing overlay.  Three examples cover all anchor policies:
	 * a resized picture across a cell range, a move-only badge and a fixed
	 * content-space stamp.  Geometry is recomputed from anchors each paint. */
	g->drawings.clear();
	{
		table_drawing d;
		d.id = "float-resize";
		d.key = "float://gradient";
		d.texture = g->float_texture;
		d.anchor_from = { 2, 1 };	/* source row 2, Name column */
		d.anchor_to = { 5, 3 };		/* source row 5, Units column */
		d.from_dx = 4.0f; d.from_dy = 3.0f;
		d.to_dx = -4.0f; d.to_dy = -3.0f;
		d.sizing = table_drawing_sizing::move_and_size;
		d.fit = table_image_fit::stretch;
		d.opacity = 0.92f;
		d.z = 1;
		d.alt_text = "resize-with-cells illustration";
		g->drawings.push_back(d);
	}
	{
		table_drawing d;
		d.id = "float-move";
		d.key = "float://badge";
		d.texture = g->badge_texture;
		d.anchor_from = { 8, 0 };
		d.anchor_to = { 8, 0 };
		d.sizing = table_drawing_sizing::move_only;
		d.width = 64.0f; d.height = 40.0f;
		d.fit = table_image_fit::contain;
		d.z = 2;
		d.alt_text = "move-only badge";
		g->drawings.push_back(d);
	}
	{
		table_drawing d;
		d.id = "float-fixed";
		d.key = "float://stamp";
		d.texture = g->float_texture;
		d.from_dx = 680.0f; d.from_dy = 8.0f;
		d.sizing = table_drawing_sizing::fixed;
		d.width = 72.0f; d.height = 48.0f;
		d.fit = table_image_fit::cover;
		d.opacity = 0.75f;
		d.z = 3;
		d.alt_text = "fixed stamp";
		g->drawings.push_back(d);
	}
	source.drawings = []() {
		++g->drawing_queries;
		return g->drawings;
	};
	g->table.set_drawing_selection_listener([](const std::string& id) {
		snprintf(g->drawing_selected, sizeof(g->drawing_selected), "%s",
			id.empty() ? "none" : id.c_str());
		g->refresh_status();
	});
	g->table.set_source(source);

	g->table.set_clipboard_handlers(
		[]() -> std::string { return get_clipboard(); },
		[](const std::string& text) { set_clipboard(text.c_str()); });

	std::vector<table_column> columns(9);
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

	/* In-cell image column.  Content is a borrowed texture supplied by the
	 * host through source.cell_image; the text value is the image key used by
	 * copy/paste and editing. */
	columns[8].id = "badge";
	columns[8].label = "Badge";
	columns[8].width = 80.0f;
	columns[8].alignment = table_alignment::center;
	columns[8].editor.type = property_type::text;
	columns[8].set_value = [](std::size_t row, const std::string& value) {
		record* r = g->row_at(row);
		if (!r || value.rfind("badge://", 0) != 0) return false;
		return true;	/* key accepted; texture stays host-owned */
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
	check(g->table.column_count() == 9, "table has 9 typed columns");
	check(g->badge_texture != nullptr, "host created programmatic badge texture");
	check(g->badge_image_queries > 0, "in-cell image queries happened during paint");

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
	check(g->table.last_render_cell_image_queries() < 500 &&
		g->table.last_render_cell_image_queries() > 0,
		"in-cell image queries are virtualized to visible cells");
	check(g->table.last_render_cell_image_queries() <=
		g->table.last_render_cell_queries(),
		"image queries never exceed cell queries in one paint");

	/* S2: floating drawing layer. */
	check(g->float_texture != nullptr, "host created programmatic float texture");
	check(g->drawing_queries > 0, "drawings pulled during paint");
	check(g->table.last_render_drawing_count() == g->drawings.size(),
		"last_render_drawing_count matches host drawings");
	{
		float rx = 0, ry = 0, rw = 0, rh = 0;
		check(g->table.drawing_rect("float-resize", &rx, &ry, &rw, &rh) &&
			rw > 0 && rh > 0, "anchored drawing resolves to a positive rect");

		/* move_and_size: widening anchor_from column moves right edge and
		 * grows the rectangle (anchor_to col boundary shifts too). */
		const float w1 = g->table.column_width(1);
		g->table.set_column_width(1, w1 + 40.0f);
		float rx2 = 0, ry2 = 0, rw2 = 0, rh2 = 0;
		g->table.drawing_rect("float-resize", &rx2, &ry2, &rw2, &rh2);
		check(rw2 > rw + 30.0f,
			"move_and_size drawing grows when an anchor column widens");

		/* move_only keeps its fixed pixel size across the same resize. */
		float mx = 0, my = 0, mw = 0, mh = 0;
		g->table.drawing_rect("float-move", &mx, &my, &mw, &mh);
		const float mw_before = mw;
		g->table.set_column_width(1, w1);	/* restore */
		g->table.drawing_rect("float-move", &mx, &my, &mw, &mh);
		check(std::fabs(mw - mw_before) < 0.5f,
			"move_only drawing keeps a fixed size across column resize");

		/* fixed: content-space position is independent of column widths. */
		float fx = 0, fy = 0, fw = 0, fh = 0;
		g->table.drawing_rect("float-fixed", &fx, &fy, &fw, &fh);
		const float fx_before = fx;
		const float w2 = g->table.column_width(2);
		g->table.set_column_width(2, w2 + 30.0f);
		g->table.drawing_rect("float-fixed", &fx, &fy, &fw, &fh);
		check(std::fabs(fx - fx_before) < 0.5f,
			"fixed drawing ignores column resize");
		g->table.set_column_width(2, w2);	/* restore */
	}

	/* Hit testing + mutually-exclusive selection via real pointer input. */
	{
		float dx = 0, dy = 0, dw = 0, dh = 0;
		if (g->table.drawing_rect("float-resize", &dx, &dy, &dw, &dh) &&
			dw > 8 && dh > 8)
		{
			const float px = dx + dw * 0.5f;
			const float py = dy + dh * 0.5f;
			g->table.select_cell(0, 0);
			check(g->table.selected_cell().valid(), "cell selected before drawing click");
			vgui_input_mouse_move(g->ui, px, py);
			vgui_input_mouse_down(g->ui, 1, px, py, 1);
			vgui_input_mouse_up(g->ui, 1, px, py, 1);
			check(g->table.selected_drawing() == "float-resize" &&
				!g->table.selected_cell().valid(),
				"clicking a drawing selects it and clears cell selection");

			g->table.select_drawing("float-fixed");
			check(g->table.selected_drawing() == "float-fixed",
				"select_drawing API selects a different drawing");
			g->table.clear_drawing_selection();
			check(g->table.selected_drawing().empty(),
				"clear_drawing_selection empties the selection");
		}
		else
			check(false, "drawing hit-test setup (no rect)");
	}

	/* Right-gap hit testing: the table body is wider than the sum of all
	 * columns (920px of columns in a ~1180px window), leaving empty space to
	 * the right of the last column.  A click there must still select the row
	 * under the cursor instead of being swallowed by hit_test returning none. */
	{
		float lx = 0, ly = 0, lw = 0, lh = 0;
		const int last_col = (int)g->table.column_count() - 1;
		if (g->table.cell_rect(0, last_col, &lx, &ly, &lw, &lh) && lw > 0)
		{
			const float cx = lx + lw + 40.0f;  /* right of last col, still in body */
			const float cy = ly + lh * 0.5f;   /* centre of row 0 */
			g->table.clear_selection();
			vgui_input_mouse_move(g->ui, cx, cy);
			vgui_input_mouse_down(g->ui, 1, cx, cy, 1);
			vgui_input_mouse_up(g->ui, 1, cx, cy, 1);
			const table_cell sel = g->table.selected_cell();
			check(sel.valid() && sel.row == 0 && sel.column == last_col,
				"right-gap click selects the row under cursor");
		}
		else
			check(false, "right-gap click selects the row under cursor (no rect)");
	}
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

		/* ---- On-demand rendering -----------------------------------------
		 * Idle frames submit nothing; input / content changes repaint.  This
		 * demo has a single window, so it exercises the per-window dirty path
		 * end to end through the real frame loop. */
		host.set_on_demand_render(true);
		if (self_test)
		{
			check(wg->get_render_on_change(),
				"on-demand rendering enabled at runtime");

			/* Drain the repaint forced by set_on_demand_render(true) plus its
			 * settle frames; after that an untouched window goes idle. */
			for (int i = 0; i < 16; ++i)
			{
				host.begin_frame(0.016f);
				host.draw_all();
				host.end_frame();
			}
			check(host.end_frame() == 0,
				"idle on-demand frame submits no window");

			/* Host-side data change + status text -> one repaint, then idle. */
			g->rows[0].name = "ondemand";
			g->table.refresh();
			g->refresh_status();
			check(wg->should_render(main_win),
				"content change marks window dirty");
			check(host.end_frame() == 1,
				"content change repaints the window once");
			for (int i = 0; i < 16; ++i)
			{
				host.begin_frame(0.016f);
				host.end_frame();
			}

			/* Explicit request_render marks a single window dirty. */
			wg->request_render(main_win);
			check(host.end_frame() == 1,
				"request_render repaints exactly the requested window");

			/* Footer labels are visually inert: moving within one must not
			 * advance the paint revision or keep hover easing alive. */
			for (int i = 0; i < 32; ++i)
			{
				host.begin_frame(0.016f);
				host.end_frame();
			}
			float sx = 0, sy = 0, sw = 0, sh = 0;
			vgui_widget_get_rect(g->sort_info, &sx, &sy, &sw, &sh);
			if (sw > 0 && sh > 0)
			{
				const float x = sx + sw * 0.5f;
				const float y = sy + sh * 0.5f;
				vgui_input_mouse_move(g->ui, x, y);
				for (int i = 0; i < 60; ++i)
				{
					host.begin_frame(0.016f);
					host.end_frame();
				}
				vgui_input_mouse_move(g->ui, x + 3.0f, y);
				check(!wg->should_render(main_win),
					"no-op motion over footer label does not dirty");
				check(host.end_frame() == 0,
					"no-op motion over footer label submits no frame");
			}
			else
				check(false, "no-op motion over footer label (no rect)");

			/* Same rule inside one table cell: hover changes only when the
			 * hovered row/column actually changes. */
			float cx = 0, cy = 0, cw = 0, ch = 0;
			if (g->table.cell_rect(0, 1, &cx, &cy, &cw, &ch) && cw > 8)
			{
				const float x = cx + cw * 0.5f;
				const float y = cy + ch * 0.5f;
				vgui_input_mouse_move(g->ui, x, y);
				for (int i = 0; i < 60; ++i)
				{
					host.begin_frame(0.016f);
					host.end_frame();
				}
				vgui_input_mouse_move(g->ui, x + 2.0f, y);
				check(!wg->should_render(main_win),
					"no-op motion inside one table cell does not dirty");
				check(host.end_frame() == 0,
					"no-op motion inside one table cell submits no frame");
			}
			else
				check(false, "no-op motion inside one table cell (no rect)");
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
