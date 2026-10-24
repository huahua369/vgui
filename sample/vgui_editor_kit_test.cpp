// sample/vgui_editor_kit_test.cpp
//
// Headless smoke test for the first vgui_editor_kit skeleton:
// command stack, context selection, shell panes, outliner/property/asset
// components and the viewport widget boundary.
//
// The test deliberately creates a vgui context without a font or GPU.  That
// keeps it runnable in CI while still exercising widget construction, layout
// and model callbacks.

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "vgui.h"
#include "vgui_editor_kit.h"

namespace {

	int fail(const char* message)
	{
		std::fprintf(stderr, "FAIL: %s\n", message);
		return 1;
	}

	class test_row_model final : public vgui_editor_kit::row_model
	{
	public:
		std::vector<vgui_editor_kit::row_info> rows;

		std::size_t row_count() const override
		{
			return rows.size();
		}

		vgui_editor_kit::row_info row_at(std::size_t index) const override
		{
			return index < rows.size() ? rows[index] : vgui_editor_kit::row_info();
		}
	};

	int test_command_stack()
	{
		using namespace vgui_editor_kit;

		int value = 0;
		command_stack stack;

		stack.execute(std::make_unique<function_command>(
			"set one",
			[&value]() { value = 1; },
			[&value]() { value = 0; }));
		if (value != 1 || !stack.can_undo() || stack.undo_count() != 1)
			return fail("command execute/undo bookkeeping");

		if (!stack.undo() || value != 0 || !stack.can_redo())
			return fail("command undo");
		if (!stack.redo() || value != 1 || stack.undo_count() != 1)
			return fail("command redo");

		stack.begin_transaction("set two and three");
		stack.execute(std::make_unique<function_command>(
			"set two",
			[&value]() { value = 2; },
			[&value]() { value = 1; }));
		stack.execute(std::make_unique<function_command>(
			"set three",
			[&value]() { value = 3; },
			[&value]() { value = 2; }));
		if (!stack.commit_transaction() || value != 3)
			return fail("transaction commit");

		if (!stack.undo() || value != 1)
			return fail("transaction undo order");
		if (!stack.redo() || value != 3)
			return fail("transaction redo order");

		stack.begin_transaction("rollback");
		stack.execute(std::make_unique<function_command>(
			"set four",
			[&value]() { value = 4; },
			[&value]() { value = 3; }));
		stack.rollback_transaction();
		if (value != 3)
			return fail("transaction rollback");

		return 0;
	}

	bool near(float a, float b)
	{
		return std::fabs(a - b) <= 0.0005f;
	}

	bool write_text(const std::filesystem::path& path, const char* text)
	{
		std::ofstream file(path, std::ios::binary | std::ios::trunc);
		if (!file)
			return false;
		file << text;
		return file.good();
	}

	int test_layout_persistence()
	{
		using namespace vgui_editor_kit;

		std::error_code ec;
		std::filesystem::path directory = std::filesystem::temp_directory_path(ec);
		if (ec)
			directory = std::filesystem::current_path(ec);
		if (ec)
			return fail("layout temporary directory");

		const std::filesystem::path layout_path =
			directory / "vgui_editor_kit_layout.json";
		const std::filesystem::path bad_path =
			directory / "vgui_editor_kit_layout_bad.json";
		std::filesystem::remove(layout_path, ec);
		std::filesystem::remove(bad_path, ec);

		{
			vgui_context* ui = vgui_create(nullptr, 16.0f);
			if (!ui)
				return fail("layout source context");
			{
				editor_context context(ui);
				editor_shell shell(context);
				shell.build();
				if (!shell.built() || !shell.center_splitter())
					return fail("layout shell build");

				editor_layout initial = shell.capture_layout();
				if (initial.version != editor_layout::current_version ||
					!near(initial.body_ratio, 0.75f) ||
					!near(initial.workspace_ratio, 0.20f) ||
					!near(initial.center_ratio, 0.70f) ||
					!initial.outliner_visible || !initial.viewport_visible ||
					!initial.properties_visible || !initial.bottom_visible ||
					initial.bottom_tab_order.size() != 2 ||
					initial.bottom_tab_order[0] != "ASSETS" ||
					initial.bottom_tab_order[1] != "CONSOLE" ||
					initial.bottom_active_tab != "ASSETS")
					return fail("layout default capture");

				vgui_splitter_set_ratio(shell.body_splitter(), 0.62f);
				vgui_splitter_set_ratio(shell.workspace_splitter(), 0.31f);
				vgui_splitter_set_ratio(shell.center_splitter(), 0.55f);
				shell.set_panel_visible(editor_panel::properties, false);
				shell.set_panel_visible(editor_panel::bottom, false);

				vgui_item_id console = vgui_tab_get_at(shell.bottom_tabs(), 1);
				if (!console || !vgui_tab_move(shell.bottom_tabs(), console, 0) ||
					!vgui_tab_set_active(shell.bottom_tabs(), console))
					return fail("layout tab setup");

				std::string error;
				if (!shell.save_layout(layout_path.string(), &error))
					return fail("layout save");

				editor_layout saved = shell.capture_layout();
				if (!near(saved.body_ratio, 0.62f) ||
					!near(saved.workspace_ratio, 0.31f) ||
					!near(saved.center_ratio, 0.55f) ||
					!saved.outliner_visible || !saved.viewport_visible ||
					saved.properties_visible || saved.bottom_visible ||
					saved.bottom_tab_order.size() != 2 ||
					saved.bottom_tab_order[0] != "CONSOLE" ||
					saved.bottom_active_tab != "CONSOLE")
					return fail("layout capture after edit");

				editor_layout parsed;
				if (!editor_layout_from_json(editor_layout_to_json(saved), parsed, &error) ||
					!near(parsed.body_ratio, 0.62f) ||
					!near(parsed.workspace_ratio, 0.31f) ||
					!near(parsed.center_ratio, 0.55f) ||
					parsed.bottom_tab_order != saved.bottom_tab_order ||
					parsed.bottom_active_tab != saved.bottom_active_tab)
					return fail("layout json round trip");
			}
			vgui_destroy(ui);
		}

		{
			vgui_context* ui = vgui_create(nullptr, 16.0f);
			if (!ui)
				return fail("layout restored context");
			{
				editor_context context(ui);
				editor_shell shell(context);
				shell.build();
				if (!shell.built())
					return fail("layout restored shell build");

				std::string error;
				if (!shell.load_layout(layout_path.string(), &error))
					return fail("layout load");
				if (!near(vgui_splitter_get_ratio(shell.body_splitter()), 0.62f) ||
					!near(vgui_splitter_get_ratio(shell.workspace_splitter()), 0.31f) ||
					!near(vgui_splitter_get_ratio(shell.center_splitter()), 0.55f))
					return fail("layout restored ratios");
				if (!shell.panel_visible(editor_panel::outliner) ||
					!shell.panel_visible(editor_panel::viewport) ||
					shell.panel_visible(editor_panel::properties) ||
					shell.panel_visible(editor_panel::bottom))
					return fail("layout restored visibility");
				if (vgui_tab_get_count(shell.bottom_tabs()) != 2)
					return fail("layout restored tab count");
				vgui_item_id first = vgui_tab_get_at(shell.bottom_tabs(), 0);
				vgui_item_id active = vgui_tab_get_active(shell.bottom_tabs());
				if (std::string(vgui_tab_get_title(shell.bottom_tabs(), first)) != "CONSOLE" ||
					std::string(vgui_tab_get_title(shell.bottom_tabs(), active)) != "CONSOLE")
					return fail("layout restored tab order and active page");

				if (!write_text(bad_path, "{ this is not layout json"))
					return fail("layout write malformed file");
				if (shell.load_layout(bad_path.string(), &error))
					return fail("layout malformed file accepted");
				if (!near(vgui_splitter_get_ratio(shell.body_splitter()), 0.75f) ||
					!near(vgui_splitter_get_ratio(shell.workspace_splitter()), 0.20f) ||
					!near(vgui_splitter_get_ratio(shell.center_splitter()), 0.70f) ||
					!shell.panel_visible(editor_panel::outliner) ||
					!shell.panel_visible(editor_panel::viewport) ||
					!shell.panel_visible(editor_panel::properties) ||
					!shell.panel_visible(editor_panel::bottom))
					return fail("layout malformed fallback");
				vgui_item_id fallback_first = vgui_tab_get_at(shell.bottom_tabs(), 0);
				vgui_item_id fallback_active = vgui_tab_get_active(shell.bottom_tabs());
				if (std::string(vgui_tab_get_title(shell.bottom_tabs(), fallback_first)) != "ASSETS" ||
					std::string(vgui_tab_get_title(shell.bottom_tabs(), fallback_active)) != "ASSETS")
					return fail("layout malformed tab fallback");

				if (!write_text(bad_path, "{\"version\": 999}"))
					return fail("layout write version file");
				if (shell.load_layout(bad_path.string(), &error))
					return fail("layout version mismatch accepted");
				if (!near(vgui_splitter_get_ratio(shell.body_splitter()), 0.75f))
					return fail("layout version fallback");
			}
			vgui_destroy(ui);
		}

		std::filesystem::remove(layout_path, ec);
		std::filesystem::remove(bad_path, ec);
		return 0;
	}

	int test_commands_and_menus()
	{
		using namespace vgui_editor_kit;

		command_shortcut parsed = command_shortcut::parse("Ctrl+Shift+S");
		if (!parsed.valid() || parsed.key != 'S' ||
			parsed.mods != (VGUI_MOD_CTRL | VGUI_MOD_SHIFT) ||
			parsed.to_string() != "Ctrl+Shift+S")
			return fail("shortcut parse and format");
		if (!parsed.matches('s', VGUI_MOD_CTRL | VGUI_MOD_SHIFT))
			return fail("shortcut matches letter case");
		if (parsed.matches('S', VGUI_MOD_CTRL))
			return fail("shortcut requires exact modifiers");
		if (command_shortcut::parse("").valid() ||
			command_shortcut::parse("Ctrl+").valid() ||
			command_shortcut::parse("Bogus").valid())
			return fail("invalid shortcut rejected");
		if (!command_shortcut::parse("delete").matches(VGUI_KEY_DELETE, 0) ||
			command_shortcut::parse("Ctrl+Z").to_string() != "Ctrl+Z")
			return fail("shortcut key names");

		vgui_context* ui = vgui_create(nullptr, 16.0f);
		if (!ui)
			return fail("commands context");
		vgui_context_set_size(ui, 1280, 800);

		{
			editor_context context(ui);
			editor_shell shell(context);
			shell.build();
			if (!shell.built() || !shell.menubar())
				return fail("commands shell build");
			vgui_update(ui, 1.0f / 60.0f);
			vgui_layout(ui);

			vgui_widget* bar = shell.menubar();
			if (vgui_widget_get_kind(bar) != VGUI_MENUBAR ||
				vgui_menubar_get_count(bar) != 4)
				return fail("menu bar built");
			const char* titles[] = { "File", "Edit", "View", "Help" };
			for (int i = 0; i < 4; ++i)
			{
				vgui_item_id item = vgui_menubar_get_item_at(bar, i);
				if (!item || std::string(vgui_menubar_get_item_text(bar, item)) != titles[i])
					return fail("menu bar titles");
			}

			const command_entry* save = shell.commands().find("file.save");
			if (!save || save->shortcut.to_string() != "Ctrl+S" ||
				!shell.commands().has("edit.undo") ||
				!shell.commands().has("view.outliner") ||
				!shell.commands().has("help.about"))
				return fail("default commands registered");

			// Hosts replace the action; the shortcut survives.
			int saves = 0;
			shell.commands().add("file.save", "Save", [&saves]() { saves++; });
			if (vgui_input_key(ui, 'S', 1, VGUI_MOD_CTRL) != 1 || saves != 1)
				return fail("shortcut invokes command");
			vgui_input_key(ui, 'S', 0, VGUI_MOD_CTRL);
			if (vgui_input_key(ui, 'S', 1, 0) != 0 || saves != 1)
				return fail("shortcut needs modifiers");

			int value = 0;
			context.commands().execute(std::make_unique<function_command>(
				"set value",
				[&value]() { value = 1; },
				[&value]() { value = 0; }));
			shell.refresh_commands();
			if (!shell.commands().is_enabled("edit.undo"))
				return fail("undo enabled after command");
			if (vgui_input_key(ui, 'Z', 1, VGUI_MOD_CTRL) != 1 || value != 0)
				return fail("undo shortcut");
			vgui_input_key(ui, 'Z', 0, VGUI_MOD_CTRL);
			if (!shell.commands().is_enabled("edit.redo"))
				return fail("redo enabled after undo");
			if (vgui_input_key(ui, 'Y', 1, VGUI_MOD_CTRL) != 1 || value != 1)
				return fail("redo shortcut");
			vgui_input_key(ui, 'Y', 0, VGUI_MOD_CTRL);

			// Delete is enabled only while something is selected.
			context.clear_selection();
			shell.refresh_commands();
			if (shell.commands().is_enabled("edit.delete"))
				return fail("delete disabled without selection");
			context.set_selection(9);
			shell.refresh_commands();
			if (!shell.commands().is_enabled("edit.delete"))
				return fail("delete enabled with selection");
			if (vgui_input_key(ui, VGUI_KEY_DELETE, 1, 0) != 1 ||
				context.selection() != invalid_object)
				return fail("delete shortcut clears selection");
			vgui_input_key(ui, VGUI_KEY_DELETE, 0, 0);

			// View toggles are checkable and menu activation changes the panel.
			vgui_item_id view_id = vgui_menubar_get_item_at(bar, 2);
			vgui_widget* view_menu = vgui_menubar_get_menu(bar, view_id);
			vgui_item_id outliner_item = view_menu ? vgui_menu_get_item_at(view_menu, 0) : 0;
			if (!view_menu || !outliner_item ||
				vgui_menu_get_item_checked(view_menu, outliner_item) != 1)
				return fail("view menu checkable item");

			float vx = 0, vy = 0, vw = 0, vh = 0;
			if (!vgui_menubar_get_item_rect(bar, view_id, &vx, &vy, &vw, &vh))
				return fail("view title rect");
			vgui_input_mouse_move(ui, vx + vw * 0.5f, vy + vh * 0.5f);
			vgui_input_mouse_down(ui, 1, vx + vw * 0.5f, vy + vh * 0.5f, 1);
			vgui_input_mouse_up(ui, 1, vx + vw * 0.5f, vy + vh * 0.5f, 1);
			if (!vgui_menu_is_open(view_menu) ||
				vgui_menubar_get_active(bar) != view_id)
				return fail("view menu opens from title");

			vgui_style style;
			vgui_context_get_style(ui, &style);
			float mx = 0, my = 0, mw = 0, mh = 0;
			vgui_widget_get_rect(view_menu, &mx, &my, &mw, &mh);
			float row_y = my + style.menu.padding_y + style.menu.item_height * 0.5f;
			vgui_input_mouse_move(ui, mx + 20.0f, row_y);
			vgui_input_mouse_down(ui, 1, mx + 20.0f, row_y, 1);
			vgui_input_mouse_up(ui, 1, mx + 20.0f, row_y, 1);
			if (vgui_menu_is_open(view_menu))
				return fail("view menu closes after activation");
			if (shell.panel_visible(editor_panel::outliner))
				return fail("view menu hides outliner");
			if (vgui_menu_get_item_checked(view_menu, outliner_item) != 0 ||
				shell.commands().is_checked("view.outliner"))
				return fail("view menu checked state follows panel");
			if (vgui_menubar_get_active(bar) != 0)
				return fail("view activation clears active title");

			// Reopen and restore through the menu.
			if (!vgui_menubar_open_item(bar, view_id))
				return fail("view menu reopens");
			if (vgui_menu_get_item_checked(view_menu, outliner_item) != 0)
				return fail("reopened menu refreshes check state");
			vgui_input_mouse_move(ui, mx + 20.0f, row_y);
			vgui_input_mouse_down(ui, 1, mx + 20.0f, row_y, 1);
			vgui_input_mouse_up(ui, 1, mx + 20.0f, row_y, 1);
			if (!shell.panel_visible(editor_panel::outliner) ||
				vgui_menu_get_item_checked(view_menu, outliner_item) != 1)
				return fail("view menu restores outliner");

			// Edit menu picks up enabled state when it opens and after commands.
			vgui_item_id edit_id = vgui_menubar_get_item_at(bar, 1);
			vgui_widget* edit_menu = vgui_menubar_get_menu(bar, edit_id);
			vgui_item_id undo_item = edit_menu ? vgui_menu_get_item_at(edit_menu, 0) : 0;
			vgui_item_id redo_item = edit_menu ? vgui_menu_get_item_at(edit_menu, 1) : 0;
			if (!edit_menu || !undo_item || !redo_item)
				return fail("edit menu items");
			if (!context.commands().can_undo())
				return fail("undo record before edit menu");
			// State changes made without refresh_commands() are picked up when
			// the menu opens.
			context.set_selection(17);
			if (!vgui_menubar_open_item(bar, edit_id))
				return fail("edit menu opens");
			if (vgui_menu_get_item_enabled(edit_menu, undo_item) != 1)
				return fail("edit menu undo enabled");
			vgui_item_id delete_item = vgui_menu_get_item_at(edit_menu, 3);
			if (!delete_item || vgui_menu_get_item_enabled(edit_menu, delete_item) != 1)
				return fail("edit menu open refreshes state");

			vgui_widget_get_rect(edit_menu, &mx, &my, &mw, &mh);
			float undo_y = my + style.menu.padding_y + style.menu.item_height * 0.5f;
			vgui_input_mouse_move(ui, mx + 20.0f, undo_y);
			vgui_input_mouse_down(ui, 1, mx + 20.0f, undo_y, 1);
			vgui_input_mouse_up(ui, 1, mx + 20.0f, undo_y, 1);
			if (value != 0 || vgui_menu_is_open(edit_menu))
				return fail("edit menu runs undo");
			if (vgui_menu_get_item_enabled(edit_menu, undo_item) != 0 ||
				vgui_menu_get_item_enabled(edit_menu, redo_item) != 1)
				return fail("edit menu enabled state refreshed");

			// Redo through the menu.
			if (!vgui_menubar_open_item(bar, edit_id))
				return fail("edit menu reopens");
			vgui_widget_get_rect(edit_menu, &mx, &my, &mw, &mh);
			float redo_y = my + style.menu.padding_y + style.menu.item_height * 1.5f;
			vgui_input_mouse_move(ui, mx + 20.0f, redo_y);
			vgui_input_mouse_down(ui, 1, mx + 20.0f, redo_y, 1);
			vgui_input_mouse_up(ui, 1, mx + 20.0f, redo_y, 1);
			if (value != 1 || context.commands().can_redo())
				return fail("edit menu runs redo");

			vgui_input_key(ui, VGUI_KEY_ESCAPE, 1, 0);
			vgui_input_key(ui, VGUI_KEY_ESCAPE, 0, 0);
			(void)mw;
			(void)mh;
		}

		vgui_destroy(ui);
		return 0;
	}

} // namespace

int main()
{
	using namespace vgui_editor_kit;

	if (int rc = test_command_stack())
		return rc;
	if (int rc = test_layout_persistence())
		return rc;
	if (int rc = test_commands_and_menus())
		return rc;

	vgui_context* ui = vgui_create(nullptr, 16.0f);
	if (!ui)
		return fail("vgui_create failed");

	vgui_context_set_size(ui, 1280, 800);

	{
		editor_context context(ui);
		object_id selected = invalid_object;
		context.add_selection_listener([&selected](object_id id) {
			selected = id;
		});

		context.set_selection(7);
		if (context.selection() != 7 || selected != 7)
			return fail("selection listener");

		editor_shell shell(context);
		shell.build();
		if (!shell.built() || !shell.widget())
			return fail("shell build");

		vgui_update(ui, 1.0f / 60.0f);
		vgui_layout(ui);

		float x = 0, y = 0, w = 0, h = 0;
		vgui_widget_get_rect(shell.widget(), &x, &y, &w, &h);
		if (w <= 0.0f || h <= 0.0f)
			return fail("shell layout size");

		if (!shell.body_splitter() || !shell.workspace_splitter() || !shell.bottom_tabs())
			return fail("shell layout handles");
		if (vgui_widget_get_kind(shell.body_splitter()) != VGUI_SPLITTER ||
			vgui_widget_get_kind(shell.workspace_splitter()) != VGUI_SPLITTER)
			return fail("shell splitter kinds");
		if (!vgui_splitter_get_vertical(shell.body_splitter()) ||
			vgui_splitter_get_vertical(shell.workspace_splitter()))
			return fail("shell splitter orientation");
		if (vgui_tab_get_count(shell.bottom_tabs()) != 2)
			return fail("shell bottom tab count");
		vgui_item_id assets_tab = vgui_tab_get_at(shell.bottom_tabs(), 0);
		vgui_item_id console_tab = vgui_tab_get_at(shell.bottom_tabs(), 1);
		if (std::string(vgui_tab_get_title(shell.bottom_tabs(), assets_tab)) != "ASSETS" ||
			std::string(vgui_tab_get_title(shell.bottom_tabs(), console_tab)) != "CONSOLE")
			return fail("shell bottom tab titles");
		if (!vgui_tab_set_active(shell.bottom_tabs(), console_tab) ||
			vgui_tab_get_active(shell.bottom_tabs()) != console_tab)
			return fail("shell console tab activation");
		if (!vgui_tab_set_active(shell.bottom_tabs(), assets_tab))
			return fail("shell assets tab restore");

		vgui_item_id scene_root = shell.scene().add_node("Scene");
		vgui_item_id cube = shell.scene().add_node("Cube", scene_root, 42);
		if (scene_root == 0 || cube == 0)
			return fail("outliner nodes");

		shell.scene().expand(scene_root);
		vgui_tree_select(shell.scene().widget(), cube, 1);
		if (context.selection() != 42 || selected != 42)
			return fail("outliner selection mapping");

		shell.properties().add_row("Name", "Cube");
		shell.properties().add_row("Position", "0, 0, 0");
		if (shell.properties().row_count() != 2)
			return fail("property rows");

		const std::string before = shell.properties().row_value(0);
		context.commands().execute(std::make_unique<function_command>(
			"rename object",
			[&shell]() { shell.properties().set_row_value(0, "Box"); },
			[&shell, before]() { shell.properties().set_row_value(0, before); }));
		if (shell.properties().row_value(0) != "Box")
			return fail("property command execute");
		if (!context.commands().undo() || shell.properties().row_value(0) != before)
			return fail("property command undo");
		if (!context.commands().redo() || shell.properties().row_value(0) != "Box")
			return fail("property command redo");

		vgui_item_id mesh = shell.assets().add_item("Cube.mesh", "Mesh");
		if (mesh == 0)
			return fail("asset item");
		vgui_list_select(shell.assets().widget(), mesh, 1);
		if (shell.assets().selected() != mesh)
			return fail("asset selection");

		if (!shell.viewport().widget())
			return fail("viewport widget");
		shell.viewport().set_texture(nullptr);

		drag_payload payload;
		payload.kind = drag_payload_kind::asset;
		payload.id = "Cube.mesh";
		if (payload.empty() || payload.kind != drag_payload_kind::asset)
			return fail("drag payload");

		test_row_model model;
		model.rows.push_back({ 1, "A", "first", true });
		model.rows.push_back({ 2, "B", "second", false });
		if (model.row_count() != 2 || model.row_at(1).text != "B")
			return fail("row model interface");

		shell.set_status("test complete");
		vgui_update(ui, 1.0f / 60.0f);
		vgui_layout(ui);

		auto has_area = [](vgui_widget* widget) {
			if (!widget)
				return false;
			float wx = 0, wy = 0, ww = 0, wh = 0;
			vgui_widget_get_rect(widget, &wx, &wy, &ww, &wh);
			return ww > 0.0f && wh > 0.0f;
		};
		if (!has_area(shell.scene().widget()))
			return fail("outliner layout size");
		if (!has_area(shell.viewport().widget()))
			return fail("viewport layout size");
		if (!has_area(shell.properties().widget()))
			return fail("property grid layout size");
		if (!has_area(shell.assets().widget()))
			return fail("asset browser layout size");
	}

	vgui_destroy(ui);
	std::printf("vgui_editor_kit_test PASS\n");
	return 0;
}
