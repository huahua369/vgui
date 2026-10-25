// sample/vgui_editor_kit_test.cpp
//
// Headless smoke test for the first vgui_editor_kit skeleton:
// command stack, context selection, shell panes, outliner/property/asset
// components and the viewport widget boundary.
//
// The test deliberately creates a vgui context without a font or GPU.  That
// keeps it runnable in CI while still exercising widget construction, layout
// and model callbacks.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
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

	vgui_widget* find_widget(vgui_widget* root, const char* id, int kind)
	{
		if (!root)
			return nullptr;
		const char* current = vgui_widget_get_id(root);
		if (current && std::string(current) == id &&
			(int)vgui_widget_get_kind(root) == kind)
			return root;
		const std::size_t count = vgui_widget_get_child_count(root);
		for (std::size_t i = 0; i < count; ++i)
		{
			if (vgui_widget* found = find_widget(vgui_widget_get_child(root, i),
				id, kind))
				return found;
		}
		return nullptr;
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

	/* Mergeable test command: moves *value from from_ to to_ and absorbs the
	 * next step when the chain is unbroken. */
	class step_command final : public vgui_editor_kit::command
	{
	public:
		step_command(int* value, int from, int to, bool* fail_flag = nullptr)
			: value_(value), from_(from), to_(to), fail_(fail_flag)
		{
		}

		const char* name() const noexcept override
		{
			return "step";
		}

		bool execute() override
		{
			if (fail_ && *fail_)
				return false;
			*value_ = to_;
			return true;
		}

		void undo() override
		{
			*value_ = from_;
		}

		bool merge_with(vgui_editor_kit::command& next) override
		{
			auto* other = dynamic_cast<step_command*>(&next);
			if (!other || other->from_ != to_)
				return false;
			to_ = other->to_;
			return true;
		}

	private:
		int* value_;
		int from_;
		int to_;
		bool* fail_;
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

		stack.clear();
		int pushed = 0;
		stack.push(std::make_unique<function_command>(
			"push applied value",
			[&pushed]() { pushed = 2; },
			[&pushed]() { pushed = 1; }));
		if (pushed != 0 || !stack.can_undo() || stack.undo_count() != 1)
			return fail("command push must not execute");
		if (!stack.undo() || pushed != 1 || !stack.can_redo())
			return fail("command push undo");
		if (!stack.redo() || pushed != 2)
			return fail("command push redo");

		stack.clear();
		stack.begin_transaction("push transaction");
		stack.push(std::make_unique<function_command>(
			"pushed child",
			[&pushed]() { pushed = 3; },
			[&pushed]() { pushed = 2; }));
		if (!stack.commit_transaction() || stack.undo_count() != 1)
			return fail("command push transaction");
		if (!stack.undo() || pushed != 2)
			return fail("command push transaction undo");

		stack.clear();
		pushed = 6;
		stack.begin_transaction("push rollback");
		stack.push(std::make_unique<function_command>(
			"pushed rollback",
			[&pushed]() { pushed = 6; },
			[&pushed]() { pushed = 5; }));
		stack.rollback_transaction();
		if (pushed != 5 || stack.can_undo())
			return fail("command push transaction rollback");

		return 0;
	}

	int test_command_merge_and_failures()
	{
		using namespace vgui_editor_kit;

		int value = 0;
		command_stack stack;

		/* Consecutive mergeable steps collapse into one undo record. */
		stack.execute(std::make_unique<step_command>(&value, 0, 1));
		stack.execute(std::make_unique<step_command>(&value, 1, 2));
		stack.execute(std::make_unique<step_command>(&value, 2, 3));
		if (value != 3 || stack.undo_count() != 1)
			return fail("command merge single record");
		if (!stack.undo() || value != 0)
			return fail("command merge undo restores start");
		if (!stack.redo() || value != 3)
			return fail("command merge redo");

		/* Undo breaks the merge chain: after redo, the next command records
		 * separately instead of folding into the redone record. */
		if (!stack.undo() || value != 0)
			return fail("command merge undo again");
		if (!stack.redo() || value != 3)
			return fail("command merge redo chain");
		stack.execute(std::make_unique<step_command>(&value, 3, 7));
		if (value != 7 || stack.undo_count() != 2)
			return fail("command merge reset after undo");
		if (!stack.undo() || value != 3)
			return fail("command post-merge undo");
		if (!stack.undo() || value != 0)
			return fail("command merged undo chain");

		stack.clear();

		/* A failed execute() leaves both stacks untouched. */
		bool fail_flag = true;
		stack.execute(std::make_unique<step_command>(&value, 0, 1));
		if (!stack.undo() || value != 0 || !stack.can_redo())
			return fail("command failure prelude");
		const std::size_t undo_before_fail = stack.undo_count();
		const std::size_t redo_before_fail = stack.redo_count();
		if (stack.execute(std::make_unique<step_command>(&value, 0, 9, &fail_flag)))
			return fail("failed command reports failure");
		if (value != 0 || stack.undo_count() != undo_before_fail ||
			stack.redo_count() != redo_before_fail)
			return fail("failed command keeps stacks");

		stack.clear();

		/* Nested transactions collapse into a single top-level record. */
		stack.begin_transaction("outer");
		stack.execute(std::make_unique<step_command>(&value, 0, 1));
		stack.begin_transaction("inner");
		stack.execute(std::make_unique<step_command>(&value, 1, 2));
		if (!stack.commit_transaction())
			return fail("inner transaction commit");
		stack.execute(std::make_unique<step_command>(&value, 2, 3));
		if (!stack.commit_transaction())
			return fail("outer transaction commit");
		if (value != 3 || stack.undo_count() != 1)
			return fail("nested transaction single record");
		if (!stack.undo() || value != 0)
			return fail("nested transaction undo");
		if (!stack.redo() || value != 3)
			return fail("nested transaction redo");

		stack.clear();
		return 0;
	}

	bool near_px(float a, float b)
	{
		return std::fabs(a - b) <= 0.5f;
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
			vgui_context_set_size(ui, 1280, 800);
			{
				editor_context context(ui);
				editor_shell shell(context);
				shell.build();
				vgui_widget* dock = shell.dock();
				if (!shell.built() || !dock ||
					vgui_widget_get_kind(dock) != VGUI_DOCK)
					return fail("layout shell dock build");

				editor_layout initial = shell.capture_layout();
				if (initial.version != editor_layout::current_version ||
					!near_px(initial.dock_left_size, 230.0f) ||
					!near_px(initial.dock_right_size, 300.0f) ||
					!near_px(initial.dock_bottom_size, 180.0f) ||
					initial.dock_panels.size() != 4 ||
					!initial.outliner_visible || !initial.viewport_visible ||
					!initial.properties_visible || !initial.bottom_visible ||
					initial.bottom_tab_order.size() != 2 ||
					initial.bottom_tab_order[0] != "ASSETS" ||
					initial.bottom_tab_order[1] != "CONSOLE" ||
					initial.bottom_active_tab != "ASSETS")
					return fail("layout default capture");

				vgui_item_id outliner = vgui_dock_find(dock, "outliner");
				vgui_item_id viewport = vgui_dock_find(dock, "viewport");
				vgui_item_id properties = vgui_dock_find(dock, "properties");
				vgui_item_id bottom = vgui_dock_find(dock, "bottom");
				if (!outliner || !viewport || !properties || !bottom)
					return fail("layout dock panel ids");

				vgui_dock_set_zone_size(dock, VGUI_DOCK_LEFT, 260.0f);
				vgui_dock_set_zone_size(dock, VGUI_DOCK_RIGHT, 340.0f);
				vgui_dock_set_zone_size(dock, VGUI_DOCK_BOTTOM, 150.0f);

				// Merge Properties into the Outliner tab group, float the bottom
				// panel and close the viewport.
				if (!vgui_dock_set_zone(dock, properties, VGUI_DOCK_LEFT, 1))
					return fail("layout dock merge properties");
				if (!vgui_dock_float(dock, bottom, 120.0f, 90.0f, 420.0f, 260.0f))
					return fail("layout dock float bottom");
				if (!vgui_dock_close(dock, viewport))
					return fail("layout dock close viewport");

				vgui_item_id console = vgui_tab_get_at(shell.bottom_tabs(), 1);
				if (!console || !vgui_tab_move(shell.bottom_tabs(), console, 0) ||
					!vgui_tab_set_active(shell.bottom_tabs(), console))
					return fail("layout tab setup");

				std::string error;
				if (!shell.save_layout(layout_path.string(), &error))
					return fail("layout save");

				editor_layout saved = shell.capture_layout();
				if (!near_px(saved.dock_left_size, 260.0f) ||
					!near_px(saved.dock_right_size, 340.0f) ||
					!near_px(saved.dock_bottom_size, 150.0f) ||
					saved.dock_panels.size() != 4 ||
					!saved.outliner_visible || saved.viewport_visible ||
					!saved.properties_visible || !saved.bottom_visible ||
					saved.bottom_tab_order.size() != 2 ||
					saved.bottom_tab_order[0] != "CONSOLE" ||
					saved.bottom_active_tab != "CONSOLE")
					return fail("layout capture after edit");

				auto find_panel = [&saved](const char* id)
					-> const editor_dock_panel_layout* {
					for (const editor_dock_panel_layout& panel : saved.dock_panels)
						if (panel.id == id)
							return &panel;
					return nullptr;
				};
				const editor_dock_panel_layout* outliner_state = find_panel("outliner");
				const editor_dock_panel_layout* viewport_state = find_panel("viewport");
				const editor_dock_panel_layout* properties_state = find_panel("properties");
				const editor_dock_panel_layout* bottom_state = find_panel("bottom");
				if (!outliner_state || !viewport_state || !properties_state ||
					!bottom_state)
					return fail("layout captured panel states");
				if (outliner_state->zone != VGUI_DOCK_LEFT || !outliner_state->open ||
					properties_state->zone != VGUI_DOCK_LEFT || !properties_state->open ||
					viewport_state->zone != VGUI_DOCK_CENTER || viewport_state->open ||
					bottom_state->zone != VGUI_DOCK_FLOATING || !bottom_state->open ||
					!bottom_state->has_floating_rect ||
					!near_px(bottom_state->width, 420.0f) ||
					!near_px(bottom_state->height, 260.0f))
					return fail("layout captured dock state");
				if (saved.dock_active[VGUI_DOCK_LEFT] != "properties")
					return fail("layout captured active tab");

				editor_layout parsed;
				if (!editor_layout_from_json(editor_layout_to_json(saved), parsed, &error) ||
					!near_px(parsed.dock_left_size, 260.0f) ||
					!near_px(parsed.dock_right_size, 340.0f) ||
					!near_px(parsed.dock_bottom_size, 150.0f) ||
					parsed.dock_panels.size() != saved.dock_panels.size() ||
					parsed.dock_active != saved.dock_active ||
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
			vgui_context_set_size(ui, 1280, 800);
			{
				editor_context context(ui);
				editor_shell shell(context);
				shell.build();
				vgui_widget* dock = shell.dock();
				if (!shell.built() || !dock)
					return fail("layout restored shell build");

				std::string error;
				if (!shell.load_layout(layout_path.string(), &error))
					return fail("layout load");
				if (!near_px(vgui_dock_get_zone_size(dock, VGUI_DOCK_LEFT), 260.0f) ||
					!near_px(vgui_dock_get_zone_size(dock, VGUI_DOCK_RIGHT), 340.0f) ||
					!near_px(vgui_dock_get_zone_size(dock, VGUI_DOCK_BOTTOM), 150.0f))
					return fail("layout restored dock sizes");

				vgui_item_id outliner = vgui_dock_find(dock, "outliner");
				vgui_item_id viewport = vgui_dock_find(dock, "viewport");
				vgui_item_id properties = vgui_dock_find(dock, "properties");
				vgui_item_id bottom = vgui_dock_find(dock, "bottom");
				if (!outliner || !viewport || !properties || !bottom)
					return fail("layout restored dock ids");
				if (vgui_dock_get_zone(dock, outliner) != VGUI_DOCK_LEFT ||
					vgui_dock_get_zone(dock, properties) != VGUI_DOCK_LEFT ||
					vgui_dock_get_zone(dock, viewport) != VGUI_DOCK_CENTER ||
					vgui_dock_get_zone(dock, bottom) != VGUI_DOCK_FLOATING ||
					!vgui_dock_is_open(dock, outliner) ||
					vgui_dock_is_open(dock, viewport) ||
					!vgui_dock_is_open(dock, properties) ||
					!vgui_dock_is_open(dock, bottom))
					return fail("layout restored dock state");
				if (vgui_dock_get_active(dock, VGUI_DOCK_LEFT) != properties)
					return fail("layout restored active tab");
				if (vgui_dock_get_zone_count(dock, VGUI_DOCK_LEFT) != 2 ||
					vgui_dock_get_zone_at(dock, VGUI_DOCK_LEFT, 0) != outliner ||
					vgui_dock_get_zone_at(dock, VGUI_DOCK_LEFT, 1) != properties)
					return fail("layout restored tab order");
				float fx = 0, fy = 0, fw = 0, fh = 0;
				if (!vgui_dock_get_floating_rect(dock, bottom, &fx, &fy, &fw, &fh) ||
					!near_px(fx, 120.0f) || !near_px(fy, 90.0f) ||
					!near_px(fw, 420.0f) || !near_px(fh, 260.0f))
					return fail("layout restored floating rect");
				if (!shell.panel_visible(editor_panel::outliner) ||
					shell.panel_visible(editor_panel::viewport) ||
					!shell.panel_visible(editor_panel::properties) ||
					!shell.panel_visible(editor_panel::bottom))
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
				if (!near_px(vgui_dock_get_zone_size(dock, VGUI_DOCK_LEFT), 230.0f) ||
					!near_px(vgui_dock_get_zone_size(dock, VGUI_DOCK_RIGHT), 300.0f) ||
					!near_px(vgui_dock_get_zone_size(dock, VGUI_DOCK_BOTTOM), 180.0f) ||
					vgui_dock_get_zone(dock, properties) != VGUI_DOCK_RIGHT ||
					vgui_dock_get_zone(dock, bottom) != VGUI_DOCK_BOTTOM ||
					!vgui_dock_is_open(dock, outliner) ||
					!vgui_dock_is_open(dock, viewport) ||
					!vgui_dock_is_open(dock, properties) ||
					!vgui_dock_is_open(dock, bottom))
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
				if (!near_px(vgui_dock_get_zone_size(dock, VGUI_DOCK_LEFT), 230.0f))
					return fail("layout version fallback");

				// Version-1 files still map their splitter ratios onto the Dock.
				if (!write_text(bad_path,
					"{\"version\": 1,"
					"\"splitters\": {\"body\": 0.60, \"workspace\": 0.30, \"center\": 0.50},"
					"\"panels\": {\"outliner\": true, \"viewport\": true,"
					"\"properties\": false, \"bottom\": true},"
					"\"bottom_tabs\": {\"order\": [\"CONSOLE\", \"ASSETS\"],"
					"\"active\": \"CONSOLE\"}}"))
					return fail("layout write legacy file");
				if (!shell.load_layout(bad_path.string(), &error))
					return fail("layout legacy load");
				float dw = 0, dh = 0;
				vgui_widget_get_rect(dock, nullptr, nullptr, &dw, &dh);
				const float expected_left = 0.30f * dw;
				const float expected_right = 0.50f * (dw - expected_left);
				const float expected_bottom = 0.40f * dh;
				if (!near_px(vgui_dock_get_zone_size(dock, VGUI_DOCK_LEFT), expected_left) ||
					!near_px(vgui_dock_get_zone_size(dock, VGUI_DOCK_RIGHT), expected_right) ||
					!near_px(vgui_dock_get_zone_size(dock, VGUI_DOCK_BOTTOM), expected_bottom) ||
					vgui_dock_is_open(dock, properties))
					return fail("layout legacy mapping");
				vgui_item_id legacy_first = vgui_tab_get_at(shell.bottom_tabs(), 0);
				vgui_item_id legacy_active = vgui_tab_get_active(shell.bottom_tabs());
				if (std::string(vgui_tab_get_title(shell.bottom_tabs(), legacy_first)) != "CONSOLE" ||
					std::string(vgui_tab_get_title(shell.bottom_tabs(), legacy_active)) != "CONSOLE")
					return fail("layout legacy bottom tabs");
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

	int test_selection_subscriptions()
	{
		using namespace vgui_editor_kit;

		vgui_context* ui = vgui_create(nullptr, 16.0f);
		if (!ui)
			return fail("selection subscription context");

		{
			editor_context context(ui);
			int first = 0;
			int second = 0;
			const auto first_id = context.add_selection_listener(
				[&first](object_id) { ++first; });
			const auto second_id = context.add_selection_listener(
				[&second](object_id) { ++second; });
			if (!first_id || !second_id || first_id == second_id)
				return fail("selection listener tokens");

			context.set_selection(1);
			if (first != 1 || second != 1)
				return fail("selection listeners notified");
			if (!context.remove_selection_listener(first_id))
				return fail("selection listener removal");
			context.set_selection(2);
			if (first != 1 || second != 2)
				return fail("removed selection listener stays quiet");
			if (context.remove_selection_listener(first_id))
				return fail("selection listener double removal");
			if (!context.remove_selection_listener(second_id))
				return fail("selection listener final removal");

			int self_calls = 0;
			editor_context::selection_listener_id self_id = 0;
			self_id = context.add_selection_listener([&](object_id) {
				++self_calls;
				context.remove_selection_listener(self_id);
			});
			context.set_selection(3);
			context.set_selection(4);
			if (self_calls != 1)
				return fail("selection listener self removal");
		}

		vgui_destroy(ui);
		return 0;
	}

	int test_property_grid()
	{
		using namespace vgui_editor_kit;

		vgui_context* ui = vgui_create(nullptr, 16.0f);
		if (!ui)
			return fail("property grid context");
		vgui_context_set_size(ui, 900, 700);

		{
			property_grid grid;
			command_stack commands;
			grid.build(vgui_root(ui));
			grid.set_command_stack(&commands);

			property_desc name;
			name.id = "name";
			name.label = "Name";
			name.type = property_type::text;
			name.value = "Cube";
			name.category = "General";
			name.tooltip = "Object name";
			if (!grid.add_property(name))
				return fail("add text property");

			property_desc count;
			count.id = "count";
			count.label = "Count";
			count.type = property_type::integer;
			count.value = "5";
			count.category = "Transform";
			count.tooltip = "Integer count";
			count.has_range = true;
			count.min_value = 0.0;
			count.max_value = 10.0;
			count.has_default = true;
			count.default_value = "5";
			if (!grid.add_property(count))
				return fail("add integer property");

			property_desc scale;
			scale.id = "scale";
			scale.label = "Scale";
			scale.type = property_type::number;
			scale.value = "0.5";
			scale.category = "Transform";
			scale.has_range = true;
			scale.min_value = -1.0;
			scale.max_value = 1.0;
			scale.step = 0.1;
			scale.has_default = true;
			scale.default_value = "0.5";
			if (!grid.add_property(scale))
				return fail("add number property");

			property_desc visible;
			visible.id = "visible";
			visible.label = "Visible";
			visible.type = property_type::boolean;
			visible.value = "false";
			visible.category = "General";
			if (!grid.add_property(visible))
				return fail("add boolean property");

			property_desc mode;
			mode.id = "mode";
			mode.label = "Mode";
			mode.type = property_type::enumeration;
			mode.value = "medium";
			mode.category = "Rendering";
			mode.choices = { "Low", "Medium", "High" };
			mode.choice_values = { "low", "medium", "high" };
			mode.choice_enabled = { 1, 0, 1 };
			mode.has_default = true;
			mode.default_value = "medium";
			if (!grid.add_property(mode))
				return fail("add enum property");

			property_desc tint;
			tint.id = "tint";
			tint.label = "Tint";
			tint.type = property_type::color;
			tint.value = "#FF0000";
			tint.category = "Rendering";
			if (!grid.add_property(tint))
				return fail("add color property");

			property_desc locked;
			locked.id = "locked";
			locked.label = "Locked";
			locked.type = property_type::text;
			locked.value = "yes";
			locked.category = "General";
			locked.read_only = true;
			if (!grid.add_property(locked))
				return fail("add read-only property");

			int bound_value = 3;
			property_desc bound;
			bound.id = "bound";
			bound.label = "Bound";
			bound.type = property_type::integer;
			bound.value = "3";
			bound.category = "General";
			bound.get_value = [&bound_value]() { return std::to_string(bound_value); };
			bound.set_value = [&bound_value](const std::string& value) {
				bound_value = std::stoi(value);
			};
			if (!grid.add_property(bound))
				return fail("add bound property");

			if (grid.row_count() != 8 || grid.visible_row_count() != 8)
				return fail("property row count");
			if (grid.property_value("count") != "5" ||
				grid.property_value("tint") != "#FF0000")
				return fail("property initial values");
			if (std::string(vgui_widget_get_tooltip(grid.property_editor("name"))) !=
				"Object name")
				return fail("property tooltip");
			if (!grid.property_editor("visible") ||
				!grid.property_editor("mode") ||
				!grid.property_editor("tint"))
				return fail("typed property editors");

			std::string last_id;
			std::string last_value;
			int change_count = 0;
			grid.set_change_listener([&](const std::string& id,
				const std::string& value) {
				++change_count;
				last_id = id;
				last_value = value;
			});

			const std::size_t undo_start = commands.undo_count();
			if (!grid.commit_property_value("count", "12") ||
				grid.property_value("count") != "10")
				return fail("integer clamping");
			if (commands.undo_count() != undo_start + 1 ||
				change_count == 0 || last_id != "count" || last_value != "10")
				return fail("property command record");
			if (!grid.property_error("count").empty())
				return fail("property clamp error state");

			if (grid.commit_property_value("count", "abc"))
				return fail("invalid integer accepted");
			if (grid.property_value("count") != "10" ||
				grid.property_error("count").empty())
				return fail("invalid integer error");

			if (!grid.commit_property_value("count", "3") ||
				!grid.property_error("count").empty())
				return fail("integer error recovery");
			const std::size_t undo_before_display = commands.undo_count();
			if (!grid.set_property_value("count", "4") ||
				commands.undo_count() != undo_before_display)
				return fail("display-only property update");
			if (!grid.commit_property_value("count", "7") ||
				commands.undo_count() != undo_before_display + 1)
				return fail("integer command record");
			if (!commands.undo() || grid.property_value("count") != "4")
				return fail("property command undo");
			if (!commands.redo() || grid.property_value("count") != "7")
				return fail("property command redo");
			if (!grid.reset_property("count") ||
				grid.property_value("count") != "5")
				return fail("property reset");
			if (!commands.undo() || grid.property_value("count") != "7")
				return fail("property reset undo");
			if (!commands.redo() || grid.property_value("count") != "5")
				return fail("property reset redo");

			vgui_widget* count_editor = grid.property_editor("count");
			if (!count_editor)
				return fail("count editor");
			vgui_edit_set_text(count_editor, "abc");
			vgui_set_focus(ui, count_editor);
			const std::size_t undo_before_text = commands.undo_count();
			vgui_input_key(ui, VGUI_KEY_ENTER, 1, 0);
			vgui_input_key(ui, VGUI_KEY_ENTER, 0, 0);
			if (std::string(vgui_edit_get_text(count_editor)) != "abc" ||
				grid.property_value("count") != "5" ||
				grid.property_error("count").empty() ||
				commands.undo_count() != undo_before_text)
				return fail("invalid editor text preserved");

			vgui_edit_set_text(count_editor, "6");
			vgui_input_key(ui, VGUI_KEY_ENTER, 1, 0);
			vgui_input_key(ui, VGUI_KEY_ENTER, 0, 0);
			if (grid.property_value("count") != "6" ||
				!grid.property_error("count").empty() ||
				commands.undo_count() != undo_before_text + 1)
				return fail("editor submit commits");
			if (!commands.undo() || grid.property_value("count") != "5")
				return fail("editor submit undo");

			if (!grid.commit_property_value("scale", "2") ||
				grid.property_value("scale") != "1")
				return fail("number upper clamp");
			if (!grid.commit_property_value("scale", "-3") ||
				grid.property_value("scale") != "-1")
				return fail("number lower clamp");
			if (!grid.commit_property_value("scale", "0.25") ||
				grid.property_value("scale") != "0.25")
				return fail("number input");

			if (!grid.commit_property_value("visible", "yes") ||
				grid.property_value("visible") != "true" ||
				vgui_widget_get_checked(grid.property_editor("visible")) != 1)
				return fail("boolean commit");
			if (!grid.commit_property_value("visible", "off") ||
				grid.property_value("visible") != "false" ||
				vgui_widget_get_checked(grid.property_editor("visible")) != 0)
				return fail("boolean false commit");

			if (!grid.commit_property_value("mode", "High") ||
				grid.property_value("mode") != "high" ||
				std::string(vgui_widget_get_text(grid.property_editor("mode"))) !=
					"High")
				return fail("enum label commit");
			if (grid.commit_property_value("mode", "Medium"))
				return fail("disabled enum choice accepted");
			if (!grid.commit_property_value("mode", "Low") ||
				grid.property_value("mode") != "low")
				return fail("enum recovery");

			if (!grid.commit_property_value("tint", "ff0000") ||
				grid.property_value("tint") != "#FF0000")
				return fail("color hex commit");
			if (!grid.commit_property_value("tint", "#00ff0080") ||
				grid.property_value("tint") != "#00FF0080")
				return fail("color alpha commit");
			if (grid.commit_property_value("tint", "#GG0000") ||
				grid.property_error("tint").empty())
				return fail("invalid color error");

			if (grid.commit_property_value("locked", "no"))
				return fail("read-only property commit");
			if (grid.property_error("locked").empty())
				return fail("read-only property error");
			if (!grid.set_property_read_only("locked", false) ||
				grid.property_read_only("locked"))
				return fail("property read-only toggle");
			if (!grid.commit_property_value("locked", "no") ||
				grid.property_value("locked") != "no")
				return fail("property editable after toggle");

			grid.set_read_only(true);
			if (!grid.property_read_only("name"))
				return fail("global read-only state");
			if (grid.commit_property_value("name", "Box"))
				return fail("global read-only commit");
			grid.set_read_only(false);
			if (grid.property_read_only("name") ||
				!grid.commit_property_value("name", "Box") ||
				grid.property_value("name") != "Box")
				return fail("global read-only recovery");

			bound_value = 9;
			grid.refresh();
			if (grid.property_value("bound") != "9")
				return fail("descriptor getter refresh");
			if (!grid.commit_property_value("bound", "4") || bound_value != 4)
				return fail("descriptor setter commit");
			if (!commands.undo() || bound_value != 9)
				return fail("descriptor setter undo");
			if (!commands.redo() || bound_value != 4)
				return fail("descriptor setter redo");

			std::string source_name = "Sphere";
			grid.set_value_source([&source_name](const std::string& id,
				std::string& out) {
				if (id != "name")
					return false;
				out = source_name;
				return true;
			});
			grid.refresh();
			if (grid.property_value("name") != "Sphere")
				return fail("value source refresh");
			source_name = "Cylinder";
			grid.refresh();
			if (grid.property_value("name") != "Cylinder")
				return fail("value source update");
			grid.set_value_source({});

			grid.set_filter("count");
			if (grid.visible_row_count() != 1 ||
				!grid.property_visible("count") ||
				grid.property_visible("name"))
				return fail("property filter");
			grid.set_filter("");
			if (grid.visible_row_count() != 8)
				return fail("property filter clear");

			if (!grid.set_category_collapsed("Transform", true) ||
				!grid.category_collapsed("Transform") ||
				grid.property_visible("count") ||
				grid.visible_row_count() != 6)
				return fail("category collapse");
			if (!grid.set_category_collapsed("Transform", false) ||
				grid.category_collapsed("Transform") ||
				!grid.property_visible("count"))
				return fail("category expand");

			vgui_layout(ui);
			float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
			vgui_widget_get_rect(grid.widget(), &x, &y, &w, &h);
			if (w <= 0.0f || h <= 0.0f)
				return fail("property grid layout size");

			vgui_widget* mode_label = find_widget(grid.widget(), "property:mode",
				VGUI_LABEL);
			if (!mode_label)
				return fail("enum label widget");
			if (!grid.set_property_read_only("mode", true))
				return fail("enum read-only set");
			float lx = 0.0f, ly = 0.0f, lw = 0.0f, lh = 0.0f;
			vgui_widget_get_rect(mode_label, &lx, &ly, &lw, &lh);
			if (lw <= 0.0f || lh <= 0.0f)
				return fail("enum label layout size");
			const float mx = lx + lw * 0.5f;
			const float my = ly + lh * 0.5f;
			if (vgui_hit_test(ui, mx, my) != mode_label)
				return fail("enum label hit test");
			vgui_input_mouse_move(ui, mx, my);
			vgui_input_mouse_down(ui, 1, mx, my, 1);
			vgui_input_mouse_up(ui, 1, mx, my, 1);
			if (vgui_context_get_open_menu(ui))
				return fail("read-only enum menu stays closed");
			if (!grid.set_property_read_only("mode", false))
				return fail("enum read-only clear");
			vgui_input_mouse_move(ui, mx, my);
			vgui_input_mouse_down(ui, 1, mx, my, 1);
			vgui_input_mouse_up(ui, 1, mx, my, 1);
			vgui_widget* open_menu = vgui_context_get_open_menu(ui);
			if (!open_menu)
				return fail("enum menu opens");
			vgui_menu_close(open_menu);
			if (vgui_context_get_open_menu(ui))
				return fail("enum menu closes");

			grid.set_filter("count");
			grid.set_category_collapsed("Transform", true);
			grid.clear();
			if (grid.row_count() != 0 || grid.visible_row_count() != 0)
				return fail("property grid clear");
			if (!grid.category_collapsed("Transform"))
				return fail("category state survives clear");
			property_desc count_again = count;
			if (!grid.add_property(count_again) ||
				grid.visible_row_count() != 0)
				return fail("filter state survives clear");
			grid.set_category_collapsed("Transform", false);
			if (grid.visible_row_count() != 1 ||
				!grid.property_visible("count"))
				return fail("filtered row after rebuild");
			grid.set_filter("");

			grid.clear();
			property_desc token;
			token.id = "token";
			token.label = "Token";
			token.type = property_type::integer;
			token.value = "1";
			if (!grid.add_property(token) ||
				!grid.commit_property_value("token", "2") ||
				grid.property_value("token") != "2")
				return fail("stale command setup");
			grid.clear();
			if (!grid.add_property(token) ||
				grid.property_value("token") != "1")
				return fail("stale command rebuild");
			if (!commands.undo())
				return fail("stale command undo");
			if (grid.property_value("token") != "1")
				return fail("stale command touched new row");
		}

		vgui_destroy(ui);
		return 0;
	}

	int test_property_grid_virtualization()
	{
		using namespace vgui_editor_kit;

		vgui_context* ui = vgui_create(nullptr, 16.0f);
		if (!ui)
			return fail("property grid virtualization context");
		vgui_context_set_size(ui, 900, 500);

		{
			property_grid grid;
			grid.build(vgui_root(ui));
			vgui_widget_set_size(grid.widget(), 900.0f, 500.0f);
			for (int i = 0; i < 1000; ++i)
			{
				property_desc desc;
				desc.id = "p" + std::to_string(i);
				desc.label = "Property " + std::to_string(i);
				desc.type = property_type::integer;
				desc.value = std::to_string(i);
				desc.category = "Group " + std::to_string(i / 100);
				if (!grid.add_property(std::move(desc)))
					return fail("add virtualized property");
			}

			vgui_layout(ui);
			if (grid.row_count() != 1000 || grid.visible_row_count() != 1000)
				return fail("virtualized row count");
			const std::size_t top_window = grid.materialized_row_count();
			if (top_window == 0 || top_window >= 100)
				return fail("virtualized top window is bounded");
			if (!grid.property_editor("p0") || grid.property_editor("p999"))
				return fail("virtualized top window contents");

			float content_w = 0.0f, content_h = 0.0f;
			vgui_panel_get_content_size(grid.widget(), &content_w, &content_h);
			if (content_h <= 500.0f)
				return fail("virtualized content height");

			vgui_panel_set_scroll(grid.widget(), 0.0f, content_h);
			vgui_layout(ui);
			const std::size_t bottom_window = grid.materialized_row_count();
			if (bottom_window == 0 || bottom_window >= 100)
				return fail("virtualized bottom window is bounded");
			if (grid.property_editor("p0") || !grid.property_editor("p999"))
				return fail("virtualized bottom window contents");

			vgui_panel_set_scroll(grid.widget(), 0.0f, 0.0f);
			vgui_layout(ui);
			if (!grid.property_editor("p0") || grid.property_editor("p999"))
				return fail("virtualized window scrolls back");

			/* Filtering from the bottom must clamp the scroll offset and
			 * rebuild the window for the shorter content. */
			vgui_panel_set_scroll(grid.widget(), 0.0f, content_h);
			vgui_layout(ui);
			grid.set_filter("Property 999");
			vgui_layout(ui);
			if (grid.visible_row_count() != 1 ||
				grid.materialized_row_count() != 1)
				return fail("virtualized filter window");
			if (!grid.property_editor("p999") || grid.property_editor("p0"))
				return fail("virtualized filter contents");
			float scroll_x = 0.0f, scroll_y = 0.0f;
			vgui_panel_get_scroll(grid.widget(), &scroll_x, &scroll_y);
			if (scroll_y > 0.5f)
				return fail("virtualized filter resets scroll");
		}

		vgui_destroy(ui);
		return 0;
	}

	int test_selection_binding()
	{
		using namespace vgui_editor_kit;

		vgui_context* ui = vgui_create(nullptr, 16.0f);
		if (!ui)
			return fail("selection binding context");

		{
			editor_context context(ui);
			property_grid grid;
			grid.build(vgui_root(ui));

			int bind_calls = 0;
			grid.bind_selection(context, [&bind_calls](property_grid& target,
				object_id id) {
				++bind_calls;
				target.clear();
				if (id == 1)
					target.add_row("Name", "One");
				else if (id == 2)
				{
					target.add_row("Name", "Two");
					target.add_row("Kind", "Mesh");
				}
			});
			if (bind_calls != 1 || grid.row_count() != 0)
				return fail("selection binding initial callback");

			context.set_selection(1);
			if (bind_calls != 2 || grid.row_count() != 1 ||
				grid.row_value(0) != "One")
				return fail("selection binding first object");

			context.set_selection(2);
			if (bind_calls != 3 || grid.row_count() != 2 ||
				grid.row_value(0) != "Two" || grid.row_value(1) != "Mesh")
				return fail("selection binding second object");

			grid.unbind_selection();
			context.set_selection(3);
			if (bind_calls != 3 || grid.row_count() != 2)
				return fail("selection binding unsubscribe");
		}

		vgui_destroy(ui);
		return 0;
	}

	int test_property_grid_drag()
	{
		using namespace vgui_editor_kit;

		vgui_context* ui = vgui_create(nullptr, 16.0f);
		if (!ui)
			return fail("property grid drag context");
		vgui_context_set_size(ui, 900, 700);

		{
			property_grid grid;
			command_stack commands;
			grid.build(vgui_root(ui));
			grid.set_command_stack(&commands);

			property_desc a;
			a.id = "a";
			a.label = "A";
			a.type = property_type::integer;
			a.value = "5";
			a.category = "Drag";
			a.has_range = true;
			a.min_value = 0.0;
			a.max_value = 10.0;
			if (!grid.add_property(a))
				return fail("add drag property a");

			property_desc b;
			b.id = "b";
			b.label = "B";
			b.type = property_type::integer;
			b.value = "50";
			b.category = "Drag";
			b.has_range = true;
			b.min_value = 0.0;
			b.max_value = 100.0;
			if (!grid.add_property(b))
				return fail("add drag property b");

			vgui_update(ui, 1.0f / 60.0f);
			vgui_layout(ui);

			vgui_widget* label_a = find_widget(grid.widget(), "property:a",
				VGUI_LABEL);
			vgui_widget* label_b = find_widget(grid.widget(), "property:b",
				VGUI_LABEL);
			if (!label_a || !label_b)
				return fail("property drag labels");

			float ax = 0.0f, ay = 0.0f, aw = 0.0f, ah = 0.0f;
			float bx = 0.0f, by = 0.0f, bw = 0.0f, bh = 0.0f;
			vgui_widget_get_rect(label_a, &ax, &ay, &aw, &ah);
			vgui_widget_get_rect(label_b, &bx, &by, &bw, &bh);
			(void)bx;
			if (aw <= 0.0f || ah <= 0.0f || bw <= 0.0f || bh <= 0.0f)
				return fail("property drag label layout");

			const std::size_t undo_before = commands.undo_count();
			const float start_x = ax + aw * 0.5f;
			const float start_y = ay + ah * 0.5f;
			const float across_y = by + bh * 0.5f;

			vgui_input_mouse_move(ui, start_x, start_y);
			vgui_input_mouse_down(ui, 1, start_x, start_y, 1);
			vgui_input_mouse_move(ui, start_x + 3.0f, across_y);
			if (!grid.property_editor("a") ||
				std::string(vgui_edit_get_text(grid.property_editor("a"))) != "8")
				return fail("property drag crosses rows");
			if (grid.property_value("b") != "50")
				return fail("property drag leaves other row unchanged");
			if (commands.undo_count() != undo_before)
				return fail("property drag commits only on release");

			vgui_input_mouse_move(ui, start_x + 25.0f, across_y);
			if (std::string(vgui_edit_get_text(grid.property_editor("a"))) != "10")
				return fail("property drag clamps");

			vgui_input_mouse_up(ui, 1, start_x + 25.0f, across_y, 1);
			if (grid.property_value("a") != "10" ||
				grid.property_value("b") != "50")
				return fail("property drag release commits");
			if (commands.undo_count() != undo_before + 1)
				return fail("property drag records one command");
			if (!commands.undo() || grid.property_value("a") != "5")
				return fail("property drag undo");
		}

		vgui_destroy(ui);
		return 0;
	}

	int test_spinbox()
	{
		using namespace vgui_editor_kit;

		vgui_context* ui = vgui_create(nullptr, 16.0f);
		if (!ui)
			return fail("spinbox context");
		vgui_context_set_size(ui, 900, 700);

		{
			vgui_widget* host = vgui_widget_add(vgui_root(ui), VGUI_PANEL,
				nullptr);
			vgui_widget_set_size(host, 420, 260);
			vgui_widget_set_layout(host, VGUI_LAYOUT_COLUMN);
			vgui_widget_set_padding(host, 12, 12, 12, 12);
			vgui_widget_set_gap(host, 10);

			spinbox count;
			count.set_id("spin_count");
			count.set_value_type(spinbox::value_type::integer);
			count.set_range(0.0, 10.0);
			count.set_step(1.0);
			count.build(host, "Count");
			count.set_value(5.0);

			spinbox scale;
			scale.set_id("spin_scale");
			scale.set_value_type(spinbox::value_type::number);
			scale.set_range(-1.0, 1.0);
			scale.set_step(0.1);
			scale.build(host, "Scale");
			scale.set_value(0.5);

			if (!count.widget() || !count.edit_widget() ||
				!count.label_widget() || !count.decrement_widget() ||
				!count.increment_widget() || !scale.widget())
				return fail("spinbox widgets");
			if (count.id() != "spin_count" ||
				find_widget(host, "spin_count_edit", VGUI_EDIT) !=
					count.edit_widget() ||
				find_widget(host, "spin_count_label", VGUI_LABEL) !=
					count.label_widget() ||
				find_widget(host, "spin_count_decrement", VGUI_BUTTON) !=
					count.decrement_widget())
				return fail("spinbox widget ids");
			if (std::string(vgui_widget_get_text(count.edit_widget())) != "5" ||
				std::string(vgui_widget_get_text(count.label_widget())) !=
					"Count")
				return fail("spinbox initial text");
			if (count.step() != 1.0 ||
				std::fabs(scale.step() - 0.1) > 1e-9)
				return fail("spinbox effective step");

			int count_changes = 0;
			int count_commits = 0;
			double last_count = 0.0;
			count.set_on_change([&](double value) {
				++count_changes;
				last_count = value;
			});
			count.set_on_commit([&](double value) {
				++count_commits;
				last_count = value;
			});

			count.set_value(12.0);
			if (count.value() != 10.0 || count_changes != 0 ||
				count_commits != 0 ||
				std::string(vgui_edit_get_text(count.edit_widget())) != "10")
				return fail("spinbox silent upper clamp");
			count.set_value(-3.0);
			if (count.value() != 0.0 || count_changes != 0)
				return fail("spinbox silent lower clamp");
			count.set_value(4.0);

			if (!count.commit_text("7") || count.value() != 7.0 ||
				count_changes != 1 || count_commits != 1 || last_count != 7.0)
				return fail("spinbox text commit");
			vgui_edit_set_text(count.edit_widget(), "abc");
			if (count.commit_text("abc") || count.value() != 7.0 ||
				count.error().empty())
				return fail("spinbox invalid text");
			if (std::string(vgui_edit_get_text(count.edit_widget())) != "abc")
				return fail("spinbox invalid text preserved");
			if (!count.commit_text("9") || !count.error().empty() ||
				count.value() != 9.0)
				return fail("spinbox error recovery");

			const int changes_before_unchanged = count_changes;
			const int commits_before_unchanged = count_commits;
			if (!count.commit_text("9") ||
				count_changes != changes_before_unchanged ||
				count_commits != commits_before_unchanged)
				return fail("spinbox unchanged commit is silent");

			count.set_value(10.0);
			const int changes_at_max = count_changes;
			const int commits_at_max = count_commits;
			count.step_by(1);
			if (count.value() != 10.0 || count_changes != changes_at_max ||
				count_commits != commits_at_max)
				return fail("spinbox step clamps silently");
			count.step_by(-1);
			if (count.value() != 9.0 || count_changes != changes_at_max + 1 ||
				count_commits != commits_at_max + 1)
				return fail("spinbox step commit");

			count.set_read_only(true);
			if (vgui_widget_get_enabled(count.decrement_widget()) != 0 ||
				vgui_widget_get_enabled(count.increment_widget()) != 0)
				return fail("spinbox read-only buttons");
			if (count.commit_text("6") || count.error().empty() ||
				count.value() != 9.0)
				return fail("spinbox read-only commit");
			count.set_read_only(false);
			if (vgui_widget_get_enabled(count.decrement_widget()) == 0)
				return fail("spinbox read-only recovery");

			count.set_enabled(false);
			if (count.enabled() || count.commit_text("6") ||
				count.error() != "value is disabled")
				return fail("spinbox disabled commit");
			count.set_enabled(true);
			if (!count.enabled())
				return fail("spinbox enabled recovery");

			count.set_label("Amount");
			if (count.label() != "Amount" ||
				std::string(vgui_widget_get_text(count.label_widget())) !=
					"Amount")
				return fail("spinbox label update");

			count.set_label_width(40.0f);
			if (count.label_width() != 40.0f)
				return fail("spinbox label width");
			count.set_buttons_visible(false);
			if (count.buttons_visible() ||
				vgui_widget_get_visible(count.decrement_widget()) != 0 ||
				vgui_widget_get_visible(count.increment_widget()) != 0)
				return fail("spinbox hidden buttons");
			count.set_buttons_visible(true);
			if (!count.buttons_visible() ||
				vgui_widget_get_visible(count.increment_widget()) == 0)
				return fail("spinbox shown buttons");
			count.set_label_width(72.0f);

			int scale_changes = 0;
			int scale_commits = 0;
			scale.set_on_change([&](double) { ++scale_changes; });
			scale.set_on_commit([&](double) { ++scale_commits; });

			scale.set_value(2.0);
			if (scale.value() != 1.0 || scale_changes != 0)
				return fail("spinbox float upper clamp");
			scale.set_value(-2.0);
			if (scale.value() != -1.0 || scale_changes != 0)
				return fail("spinbox float lower clamp");
			scale.set_value(0.5);
			if (!scale.commit_text("0.25") || scale.value() != 0.25)
				return fail("spinbox float text commit");
			scale_changes = 0;
			scale_commits = 0;
			scale.step_by(1);
			if (scale.value_text() != "0.35" || scale_commits != 1)
				return fail("spinbox float step");
			scale.step_by(-1);
			if (scale.value_text() != "0.25")
				return fail("spinbox float step back");
			if (!scale.commit_text("1.5") || scale.value() != 1.0)
				return fail("spinbox float clamp commit");

			count.set_value(5.0);
			count.set_error("");
			scale.set_value(0.5);
			scale.set_error("");
			count_changes = 0;
			count_commits = 0;
			scale_changes = 0;
			scale_commits = 0;

			vgui_update(ui, 1.0f / 60.0f);
			vgui_layout(ui);

			float clx = 0.0f, cly = 0.0f, clw = 0.0f, clh = 0.0f;
			float slx = 0.0f, sly = 0.0f, slw = 0.0f, slh = 0.0f;
			vgui_widget_get_rect(count.label_widget(), &clx, &cly, &clw, &clh);
			vgui_widget_get_rect(scale.label_widget(), &slx, &sly, &slw, &slh);
			if (clw <= 0.0f || clh <= 0.0f || slw <= 0.0f || slh <= 0.0f)
				return fail("spinbox drag label layout");

			const float count_x = clx + clw * 0.5f;
			const float count_y = cly + clh * 0.5f;
			const float scale_y = sly + slh * 0.5f;

			vgui_input_mouse_move(ui, count_x, count_y);
			vgui_input_mouse_down(ui, 1, count_x, count_y, 1);
			if (count.value() != 5.0)
				return fail("spinbox drag press is silent");
			vgui_input_mouse_move(ui, count_x + 3.0f, scale_y);
			if (count.value() != 8.0 || count_changes == 0 ||
				count_commits != 0)
				return fail("spinbox drag crosses rows");
			if (scale.value() != 0.5 || scale_changes != 0)
				return fail("spinbox drag leaves other control unchanged");
			vgui_input_mouse_move(ui, count_x + 6.0f, scale_y + 300.0f);
			if (count.value() != 10.0)
				return fail("spinbox drag updates outside control");
			vgui_input_mouse_up(ui, 1, count_x + 6.0f, scale_y + 300.0f, 1);
			if (count_commits != 1 || count.value() != 10.0)
				return fail("spinbox drag release commits once");

			count.set_value(5.0);
			count_changes = 0;
			count_commits = 0;
			vgui_input_mouse_move(ui, count_x, count_y);
			vgui_input_mouse_down(ui, 1, count_x, count_y, 1);
			vgui_input_mouse_move(ui, count_x + 3.0f, scale_y);
			vgui_input_mouse_up(ui, 1, count_x + 3.0f, scale_y, 1);
			if (count.value() != 8.0 || count_commits != 1 ||
				scale.value() != 0.5 || scale_changes != 0)
				return fail("spinbox cross-row release");

			scale.set_value(0.5);
			scale_changes = 0;
			scale_commits = 0;
			vgui_input_mouse_move(ui, slx + slw * 0.5f, scale_y);
			vgui_input_mouse_down(ui, 1, slx + slw * 0.5f, scale_y, 1);
			vgui_input_key(ui, 0, 0, VGUI_MOD_SHIFT);
			vgui_input_mouse_move(ui, slx + slw * 0.5f + 20.0f, scale_y + 4.0f);
			if (std::fabs(scale.value() - 0.7) > 1e-9 ||
				scale_commits != 0)
				return fail("spinbox shift drag precision");
			vgui_input_key(ui, 0, 0, 0);
			vgui_input_mouse_up(ui, 1, slx + slw * 0.5f + 20.0f,
				scale_y + 4.0f, 1);
			if (scale_commits != 1 || std::fabs(scale.value() - 0.7) > 1e-9)
				return fail("spinbox shift drag commit");

			scale.set_value(0.0);
			scale_changes = 0;
			scale_commits = 0;
			vgui_input_mouse_move(ui, slx + slw * 0.5f, scale_y);
			vgui_input_mouse_down(ui, 1, slx + slw * 0.5f, scale_y, 1);
			vgui_input_key(ui, 0, 0, VGUI_MOD_ALT);
			vgui_input_mouse_move(ui, slx + slw * 0.5f + 2.0f, scale_y);
			if (std::fabs(scale.value() - 1.0) > 1e-9)
				return fail("spinbox alt drag coarse");
			vgui_input_key(ui, 0, 0, 0);
			vgui_input_mouse_up(ui, 1, slx + slw * 0.5f + 2.0f, scale_y, 1);
			if (scale_commits != 1 || std::fabs(scale.value() - 1.0) > 1e-9)
				return fail("spinbox alt drag commit");

			scale.set_value(0.5);
			scale_changes = 0;
			scale_commits = 0;
			float bx = 0.0f, by = 0.0f, bw = 0.0f, bh = 0.0f;
			vgui_widget_get_rect(scale.increment_widget(), &bx, &by, &bw, &bh);
			const float button_x = bx + bw * 0.5f;
			const float button_y = by + bh * 0.5f;
			vgui_input_mouse_move(ui, button_x, button_y);
			vgui_input_mouse_down(ui, 1, button_x, button_y, 1);
			vgui_input_mouse_up(ui, 1, button_x, button_y, 1);
			if (std::fabs(scale.value() - 0.6) > 1e-9 ||
				scale_commits != 1)
				return fail("spinbox step button");

			scale.set_read_only(true);
			const double before_readonly = scale.value();
			const int commits_before_readonly = scale_commits;
			vgui_input_mouse_move(ui, button_x, button_y);
			vgui_input_mouse_down(ui, 1, button_x, button_y, 1);
			vgui_input_mouse_up(ui, 1, button_x, button_y, 1);
			if (scale.value() != before_readonly ||
				scale_commits != commits_before_readonly)
				return fail("spinbox read-only step button");
			scale.set_read_only(false);

			scale.set_value(0.4);
			scale_changes = 0;
			scale_commits = 0;
			scale.focus();
			vgui_edit_set_text(scale.edit_widget(), "0.6");
			vgui_input_key(ui, VGUI_KEY_ENTER, 1, 0);
			vgui_input_key(ui, VGUI_KEY_ENTER, 0, 0);
			if (std::fabs(scale.value() - 0.6) > 1e-9 ||
				scale_commits != 1)
				return fail("spinbox enter commit");

			vgui_edit_set_text(scale.edit_widget(), "oops");
			vgui_input_key(ui, VGUI_KEY_ENTER, 1, 0);
			vgui_input_key(ui, VGUI_KEY_ENTER, 0, 0);
			if (scale.error().empty() ||
				std::fabs(scale.value() - 0.6) > 1e-9 ||
				std::string(vgui_edit_get_text(scale.edit_widget())) != "oops")
				return fail("spinbox enter validation");

			vgui_input_key(ui, VGUI_KEY_ESCAPE, 1, 0);
			vgui_input_key(ui, VGUI_KEY_ESCAPE, 0, 0);
			if (!scale.error().empty() ||
				std::string(vgui_edit_get_text(scale.edit_widget())) != "0.6")
				return fail("spinbox escape restores text");

			scale.set_value(0.4);
			scale_changes = 0;
			scale_commits = 0;
			scale.focus();
			vgui_input_key(ui, VGUI_KEY_UP, 1, 0);
			vgui_input_key(ui, VGUI_KEY_UP, 0, 0);
			if (std::fabs(scale.value() - 0.5) > 1e-9 ||
				scale_commits != 1)
				return fail("spinbox keyboard up");
			vgui_input_key(ui, VGUI_KEY_DOWN, 1, 0);
			vgui_input_key(ui, VGUI_KEY_DOWN, 0, 0);
			if (std::fabs(scale.value() - 0.4) > 1e-9 ||
				scale_commits != 2)
				return fail("spinbox keyboard down");

			count.clear_range();
			count.set_value(100.0);
			if (count.has_range() || count.value() != 100.0)
				return fail("spinbox clear range");
			count.set_range(10.0, 0.0);
			if (!count.has_range() || count.min_value() != 0.0 ||
				count.max_value() != 10.0 || count.value() != 10.0)
				return fail("spinbox range swap and clamp");
		}

		vgui_destroy(ui);
		return 0;
	}

	int test_vector_editor()
	{
		using namespace vgui_editor_kit;

		vgui_context* ui = vgui_create(nullptr, 16.0f);
		if (!ui)
			return fail("vector editor context");
		vgui_context_set_size(ui, 900, 700);

		{
			vgui_widget* host = vgui_widget_add(vgui_root(ui), VGUI_PANEL,
				nullptr);
			vgui_widget_set_size(host, 480, 260);
			vgui_widget_set_layout(host, VGUI_LAYOUT_COLUMN);
			vgui_widget_set_padding(host, 12, 12, 12, 12);
			vgui_widget_set_gap(host, 8);

			vector_editor position;
			position.set_id("vec_position");
			position.set_semantic(vector_semantic::position);
			position.set_range(-10.0, 10.0);
			position.set_step(1.0);
			position.build(host, 3);
			position.set_value({ 1.0, 2.0, 3.0, 0.0 });

			if (!position.widget() || position.dimension() != 3 ||
				position.value_type() != spinbox::value_type::number ||
				position.id() != "vec_position")
				return fail("vector editor build");
			if (find_widget(host, "vec_position", VGUI_PANEL) !=
					position.widget() ||
				!position.component_edit_widget(0) ||
				!position.component_label_widget(1) ||
				find_widget(host, "vec_position_0_edit", VGUI_EDIT) !=
					position.component_edit_widget(0) ||
				find_widget(host, "vec_position_1_label", VGUI_LABEL) !=
					position.component_label_widget(1))
				return fail("vector editor widget ids");
			if (position.value_text() != "1, 2, 3" ||
				position.axis_labels()[0] != "X" ||
				position.axis_labels()[1] != "Y" ||
				position.axis_labels()[2] != "Z" ||
				std::string(vgui_widget_get_text(
					position.component_label_widget(1))) != "Y")
				return fail("vector editor position semantic");
			if (position.axis_colors()[0] == 0 ||
				position.axis_colors()[0] !=
					vector_editor::default_axis_color(0,
						vector_semantic::position))
				return fail("vector editor axis colors");
			vgui_widget* decrement = find_widget(host,
				"vec_position_0_decrement", VGUI_BUTTON);
			vgui_widget* increment = find_widget(host,
				"vec_position_0_increment", VGUI_BUTTON);
			if (!decrement || !increment ||
				vgui_widget_get_visible(decrement) != 0 ||
				vgui_widget_get_visible(increment) != 0)
				return fail("vector editor compact buttons");

			position.set_semantic(vector_semantic::color);
			if (position.axis_labels()[0] != "R" ||
				position.axis_labels()[3] != "A" ||
				position.axis_colors()[0] !=
					vector_editor::default_axis_color(0,
						vector_semantic::color) ||
				position.value_text() != "1, 2, 3")
				return fail("vector editor color semantic");

			position.set_axis_labels({ "Right", "Up", "Forward" });
			position.set_axis_label(2, "Depth");
			position.set_axis_color(1, 0xFF123456);
			if (position.axis_labels()[0] != "Right" ||
				position.axis_labels()[2] != "Depth" ||
				position.axis_colors()[1] != 0xFF123456 ||
				std::string(vgui_widget_get_text(
					position.component_label_widget(0))) != "Right")
				return fail("vector editor custom axes");

			int changes = 0;
			int commits = 0;
			vector_editor::vector_value last{};
			position.set_on_change(
				[&](const vector_editor::vector_value& value) {
					++changes;
					last = value;
				});
			position.set_on_commit(
				[&](const vector_editor::vector_value& value) {
					++commits;
					last = value;
				});

			position.set_value({ 4.0, 5.0, 6.0, 0.0 });
			position.set_component_value(1, 8.0);
			position.set_component_value(0, 50.0);
			if (changes != 0 || commits != 0 ||
				position.value_text() != "10, 8, 6" ||
				position.value()[1] != 8.0)
				return fail("vector editor silent values");

			position.set_value({ 1.0, 2.0, 3.0, 0.0 });
			if (!position.commit_component_text(1, "7") ||
				changes != 1 || commits != 1 || last[1] != 7.0 ||
				position.value_text() != "1, 7, 3")
				return fail("vector editor component commit");
			if (position.commit_component_text(0, "oops") ||
				position.component_error(0).empty() ||
				changes != 1 || commits != 1 ||
				position.value_text() != "1, 7, 3")
				return fail("vector editor invalid component");
			if (!position.commit_component_text(0, "9") ||
				!position.component_error(0).empty() ||
				changes != 2 || commits != 2 ||
				position.value_text() != "9, 7, 3")
				return fail("vector editor error recovery");
			if (!position.commit_component_text(0, "9") ||
				changes != 2 || commits != 2)
				return fail("vector editor unchanged commit");

			position.set_read_only(true);
			if (!position.read_only() ||
				position.commit_component_value(0, 2.0) ||
				position.component_error(0) != "value is read-only" ||
				changes != 2 || commits != 2 ||
				position.value_text() != "9, 7, 3")
				return fail("vector editor read-only");
			position.set_read_only(false);
			position.clear_component_errors();
			if (!position.component_error(0).empty())
				return fail("vector editor clear errors");

			position.set_enabled(false);
			if (position.enabled() ||
				position.commit_component_value(0, 2.0) ||
				position.component_error(0) != "value is disabled")
				return fail("vector editor disabled");
			position.set_enabled(true);
			position.clear_component_errors();

			vector_editor::vector_value gizmo{ 9.0, 7.0, 3.0, 0.0 };
			position.set_value_source(
				[&](vector_editor::vector_value& out) {
					out = gizmo;
					return true;
				});
			gizmo = { 4.0, 5.0, 6.0, 0.0 };
			const int changes_before_refresh = changes;
			position.refresh();
			if (changes != changes_before_refresh || commits != 2 ||
				position.value_text() != "4, 5, 6")
				return fail("vector editor value source refresh");

			position.set_on_change(
				[&](const vector_editor::vector_value& value) {
					++changes;
					gizmo = value;
				});
			if (!position.commit_component_text(0, "7") ||
				gizmo[0] != 7.0 ||
				position.value_text() != "7, 5, 6")
				return fail("vector editor two-way binding");

			if (position.set_dimension(4) || !position.set_dimension(3))
				return fail("vector editor locked dimension");

			vector_editor uv;
			uv.set_id("vec_uv");
			if (!uv.set_dimension(2) || uv.set_dimension(1) ||
				uv.set_dimension(5))
				return fail("vector editor dimension bounds");
			uv.build(host, 0);
			uv.set_value({ 0.5, 0.25, 0.0, 0.0 });
			if (uv.dimension() != 2 ||
				uv.value_text() != "0.5, 0.25" ||
				uv.axis_labels()[0] != "X" ||
				uv.axis_labels()[1] != "Y")
				return fail("vector editor vector2");

			vector_editor tint;
			tint.set_id("vec_tint");
			tint.set_semantic(vector_semantic::color);
			tint.build(host, 4);
			tint.set_value({ 1.0, 0.5, 0.25, 1.0 });
			if (tint.dimension() != 4 ||
				tint.value_text() != "1, 0.5, 0.25, 1" ||
				tint.axis_labels()[3] != "A")
				return fail("vector editor vector4");

			vector_editor::vector_value parsed{};
			std::string error;
			if (!vector_editor::parse_value("1 2 3", 3, parsed, &error) ||
				parsed[2] != 3.0)
				return fail("vector editor space parse");
			if (!vector_editor::parse_value("[1, 2, 3]", 3, parsed,
				&error) || parsed[0] != 1.0)
				return fail("vector editor bracket parse");
			if (!vector_editor::parse_value("(4; 5; 6)", 3, parsed,
				&error) || parsed[1] != 5.0)
				return fail("vector editor semicolon parse");
			if (vector_editor::parse_value("1, 2", 3, parsed, &error) ||
				error.empty())
				return fail("vector editor parse count");
			if (vector_editor::parse_value("1, x, 3", 3, parsed, &error))
				return fail("vector editor parse number");
			if (vector_editor::format_value({ 1.0, 2.0, 3.0, 0.0 }, 3) !=
				"1, 2, 3")
				return fail("vector editor format");

			position.set_value({ 1.0, 2.0, 3.0, 0.0 });
			position.set_step(0.0);
			changes = 0;
			commits = 0;
			vgui_update(ui, 1.0f / 60.0f);
			vgui_layout(ui);

			vgui_widget* drag_label = position.component_label_widget(0);
			if (!drag_label)
				return fail("vector editor drag label");
			float lx = 0.0f, ly = 0.0f, lw = 0.0f, lh = 0.0f;
			vgui_widget_get_rect(drag_label, &lx, &ly, &lw, &lh);
			if (lw <= 0.0f || lh <= 0.0f || lw >= 40.0f)
				return fail("vector editor compact label layout");

			const float cx = lx + lw * 0.5f;
			const float cy = ly + lh * 0.5f;
			vgui_input_mouse_move(ui, cx, cy);
			vgui_input_mouse_down(ui, 1, cx, cy, 1);
			vgui_input_mouse_move(ui, cx + 3.0f, cy);
			if (std::fabs(position.value()[0] - 1.3) > 1e-9 ||
				position.value()[1] != 2.0 || changes == 0 ||
				commits != 0)
				return fail("vector editor component drag");
			vgui_input_mouse_up(ui, 1, cx + 3.0f, cy, 1);
			if (commits != 1 ||
				std::fabs(position.value()[0] - 1.3) > 1e-9)
				return fail("vector editor drag commit");
		}

		vgui_destroy(ui);
		return 0;
	}

	int test_combo_box()
	{
		using namespace vgui_editor_kit;

		vgui_context* ui = vgui_create(nullptr, 16.0f);
		if (!ui)
			return fail("combo box context");
		vgui_context_set_size(ui, 900, 700);

		{
			vgui_widget* host = vgui_widget_add(vgui_root(ui), VGUI_PANEL,
				nullptr);
			vgui_widget_set_size(host, 360, 220);
			vgui_widget_set_layout(host, VGUI_LAYOUT_COLUMN);
			vgui_widget_set_padding(host, 12, 12, 12, 12);
			vgui_widget_set_gap(host, 8);

			combo_box combo;
			combo.set_id("combo_mode");
			combo.set_placeholder("Choose...");
			combo.set_items({
				{ "Low", "low", true },
				{ "Medium", "medium", false },
				{ "High", "high", true },
				{ "", "unlabeled", true },
			});
			combo.build(host);

			if (!combo.widget() ||
				find_widget(host, "combo_mode", VGUI_BUTTON) != combo.widget())
				return fail("combo box widget id");
			if (combo.item_count() != 4 || combo.item_at(1) == nullptr ||
				combo.item_at(1)->value != "medium" ||
				combo.item_at(1)->enabled || combo.item_at(4) != nullptr)
				return fail("combo box items");
			if (combo.selected_index() != -1 || !combo.value().empty() ||
				combo.display_text() != "Choose..." ||
				std::string(vgui_widget_get_text(combo.widget())) !=
					"Choose...")
				return fail("combo box empty selection");

			int changes = 0;
			int commits = 0;
			int last_index = -2;
			std::string last_value;
			combo.set_on_change([&](int index, const std::string& value) {
				++changes;
				last_index = index;
				last_value = value;
			});
			combo.set_on_commit([&](int index, const std::string& value) {
				++commits;
				last_index = index;
				last_value = value;
			});

			if (!combo.set_selected_index(2) ||
				combo.selected_index() != 2 || combo.value() != "high" ||
				combo.display_text() != "High" || changes != 0 ||
				commits != 0)
				return fail("combo box silent index");
			if (!combo.set_value("low") || combo.selected_index() != 0 ||
				changes != 0 || commits != 0)
				return fail("combo box silent value");
			if (combo.set_value("missing") || combo.selected_index() != 0)
				return fail("combo box unknown value");
			combo.clear_selection();
			if (combo.selected_index() != -1 || !combo.value().empty() ||
				combo.display_text() != "Choose..." || changes != 0)
				return fail("combo box silent clear");
			if (!combo.set_selected_index(-1) ||
				!combo.set_selected_index(0) ||
				combo.selected_index() != 0)
				return fail("combo box index bounds");
			if (combo.set_selected_index(4))
				return fail("combo box index out of range");

			if (combo.select_index(1) || combo.selected_index() != 0 ||
				changes != 0)
				return fail("combo box disabled select");
			if (!combo.set_item_enabled(1, true) || !combo.select_index(1) ||
				combo.selected_index() != 1 || combo.value() != "medium" ||
				changes != 1 || commits != 1 || last_index != 1 ||
				last_value != "medium")
				return fail("combo box user select");
			if (!combo.set_item_enabled(1, false) ||
				combo.set_item_enabled(99, false))
				return fail("combo box item enable bounds");
			if (combo.select_index(1) || changes != 1)
				return fail("combo box disabled after enable");

			combo.set_selected_index(2);
			combo.set_items({
				{ "Low", "low", true },
				{ "Medium", "medium", false },
				{ "High!", "high", true },
				{ "", "unlabeled", true },
			});
			if (combo.selected_index() != 2 || combo.value() != "high" ||
				combo.display_text() != "High!" || changes != 1)
				return fail("combo box items preserve value");
			if (!combo.set_selected_index(3) ||
				combo.display_text() != "Choose..." ||
				combo.value() != "unlabeled")
				return fail("combo box empty label placeholder");
			combo.set_placeholder("Pick one");
			if (combo.display_text() != "Pick one")
				return fail("combo box placeholder update");

			combo.set_selected_index(0);
			if (!combo.select_none() || combo.selected_index() != -1 ||
				!combo.value().empty() ||
				combo.display_text() != "Pick one" || changes != 2 ||
				commits != 2 || last_index != -1 || !last_value.empty())
				return fail("combo box select none");

			combo.set_selected_index(2);
			combo.set_read_only(true);
			if (vgui_widget_get_enabled(combo.widget()) != 0 ||
				combo.open() || combo.select_index(0) ||
				combo.select_none())
				return fail("combo box read-only");
			combo.set_read_only(false);
			if (vgui_widget_get_enabled(combo.widget()) == 0)
				return fail("combo box read-only recovery");
			combo.set_enabled(false);
			if (combo.enabled() ||
				vgui_widget_get_enabled(combo.widget()) != 0 ||
				combo.open() || combo.select_index(0))
				return fail("combo box disabled");
			combo.set_enabled(true);
			if (!combo.enabled())
				return fail("combo box enabled recovery");

			combo.set_selected_index(0);
			changes = 0;
			commits = 0;
			combo.focus();
			if (vgui_get_focus(ui) != combo.widget())
				return fail("combo box focus");
			if (!combo.open() || !combo.is_open())
				return fail("combo box open");
			vgui_widget* menu = vgui_context_get_open_menu(ui);
			if (!menu ||
				std::string(vgui_widget_get_id(menu)) != "combo_mode_menu" ||
				vgui_menu_get_count(menu) != 4 ||
				find_widget(host, "combo_mode_menu", VGUI_MENU) != nullptr)
				return fail("combo box menu contents");
			if (vgui_get_focus(ui) != combo.widget())
				return fail("combo box open keeps focus");
			vgui_item_id low_item = vgui_menu_get_item_at(menu, 0);
			vgui_item_id medium_item = vgui_menu_get_item_at(menu, 1);
			vgui_item_id high_item = vgui_menu_get_item_at(menu, 2);
			vgui_item_id unlabeled_item = vgui_menu_get_item_at(menu, 3);
			if (!low_item || !medium_item || !high_item || !unlabeled_item)
				return fail("combo box menu items");
			if (vgui_menu_get_cursor(menu) != low_item)
				return fail("combo box menu cursor starts on selection");
			if (vgui_menu_get_item_enabled(menu, medium_item) != 0 ||
				std::string(vgui_menu_get_item_text(menu, unlabeled_item)) !=
					"Pick one")
				return fail("combo box menu item state");
			if (vgui_menu_get_item_checked(menu, low_item) != 1)
				return fail("combo box menu checked selection");
			int handled = vgui_input_key(ui, VGUI_KEY_DOWN, 1, 0);
			vgui_input_key(ui, VGUI_KEY_DOWN, 0, 0);
			if (!handled || vgui_menu_get_cursor(menu) != high_item)
				return fail("combo box menu skips disabled");
			handled = vgui_input_key(ui, VGUI_KEY_ENTER, 1, 0);
			vgui_input_key(ui, VGUI_KEY_ENTER, 0, 0);
			if (!handled || combo.is_open() ||
				combo.selected_index() != 2 || combo.value() != "high" ||
				combo.display_text() != "High!" || changes != 1 ||
				commits != 1 ||
				vgui_get_focus(ui) != combo.widget())
				return fail("combo box menu selection");

			if (!combo.open())
				return fail("combo box reopen");
			menu = vgui_context_get_open_menu(ui);
			if (!menu || vgui_menu_get_cursor(menu) != high_item)
				return fail("combo box menu cursor tracks selection");
			vgui_input_key(ui, VGUI_KEY_ESCAPE, 1, 0);
			vgui_input_key(ui, VGUI_KEY_ESCAPE, 0, 0);
			if (combo.is_open() || combo.value() != "high" ||
				changes != 1 || commits != 1 ||
				vgui_get_focus(ui) != combo.widget())
				return fail("combo box escape keeps value and focus");

			vgui_input_key(ui, VGUI_KEY_DOWN, 1, VGUI_MOD_ALT);
			vgui_input_key(ui, VGUI_KEY_DOWN, 0, VGUI_MOD_ALT);
			if (!combo.is_open() ||
				vgui_context_get_open_menu(ui) != menu)
				return fail("combo box alt down opens");
			vgui_input_key(ui, VGUI_KEY_ESCAPE, 1, 0);
			vgui_input_key(ui, VGUI_KEY_ESCAPE, 0, 0);

			combo.focus();
			changes = 0;
			commits = 0;
			vgui_input_key(ui, VGUI_KEY_DOWN, 1, 0);
			vgui_input_key(ui, VGUI_KEY_DOWN, 0, 0);
			if (combo.selected_index() != 3 ||
				combo.value() != "unlabeled" ||
				combo.display_text() != "Pick one" || changes != 1 ||
				commits != 1)
				return fail("combo box closed keyboard down");
			vgui_input_key(ui, VGUI_KEY_UP, 1, 0);
			vgui_input_key(ui, VGUI_KEY_UP, 0, 0);
			if (combo.selected_index() != 2 || combo.value() != "high" ||
				changes != 2 || commits != 2)
				return fail("combo box closed keyboard up");
			vgui_input_key(ui, VGUI_KEY_HOME, 1, 0);
			vgui_input_key(ui, VGUI_KEY_HOME, 0, 0);
			if (combo.selected_index() != 0 || changes != 3)
				return fail("combo box keyboard home");
			vgui_input_key(ui, VGUI_KEY_END, 1, 0);
			vgui_input_key(ui, VGUI_KEY_END, 0, 0);
			if (combo.selected_index() != 3 || changes != 4)
				return fail("combo box keyboard end");
			combo.set_placeholder("Updated");
			if (combo.display_text() != "Updated" ||
				std::string(vgui_menu_get_item_text(menu, unlabeled_item)) !=
					"Updated")
				return fail("combo box placeholder updates menu");

			combo.set_selected_index(0);
			if (!combo.open())
				return fail("combo box destructor open");
		}
		if (vgui_context_get_open_menu(ui))
			return fail("combo box destructor closes menu");

		vgui_destroy(ui);
		return 0;
	}

	int test_property_grid_enum()
	{
		using namespace vgui_editor_kit;

		vgui_context* ui = vgui_create(nullptr, 16.0f);
		if (!ui)
			return fail("property grid enum context");
		vgui_context_set_size(ui, 900, 700);

		{
			property_grid grid;
			command_stack commands;
			grid.build(vgui_root(ui));
			grid.set_command_stack(&commands);

			int changes = 0;
			std::string changed_id;
			std::string changed_value;
			grid.set_change_listener([&](const std::string& id,
				const std::string& value) {
				++changes;
				changed_id = id;
				changed_value = value;
			});

			property_desc mode;
			mode.id = "mode";
			mode.label = "Mode";
			mode.type = property_type::enumeration;
			mode.value = "";
			mode.category = "Rendering";
			mode.placeholder = "None";
			mode.allow_empty = true;
			mode.choices = { "Low", "Medium", "High" };
			mode.choice_values = { "low", "medium", "high" };
			mode.choice_enabled = { 1, 0, 1 };
			mode.has_default = true;
			mode.default_value = "";
			if (!grid.add_property(mode))
				return fail("enum empty property");

			property_desc plain = mode;
			plain.id = "plain";
			plain.label = "Plain";
			plain.placeholder.clear();
			if (!grid.add_property(plain))
				return fail("enum default placeholder property");

			property_desc strict = mode;
			strict.id = "strict";
			strict.label = "Strict";
			strict.allow_empty = false;
			strict.value = "low";
			strict.default_value = "low";
			if (!grid.add_property(strict))
				return fail("enum strict property");

			if (!grid.property_value("mode").empty() ||
				!grid.property_value("plain").empty() ||
				grid.property_value("strict") != "low")
				return fail("enum empty values");
			vgui_widget* editor = grid.property_editor("mode");
			vgui_widget* plain_editor = grid.property_editor("plain");
			if (!editor || vgui_widget_get_kind(editor) != VGUI_BUTTON ||
				std::string(vgui_widget_get_text(editor)) != "None" ||
				!plain_editor ||
				std::string(vgui_widget_get_text(plain_editor)) != "(None)")
				return fail("enum empty display");
			if (!grid.set_property_value("mode", "high") ||
				grid.property_value("mode") != "high" ||
				std::string(vgui_widget_get_text(editor)) != "High" ||
				changes != 0)
				return fail("enum silent value");
			if (!grid.set_property_value("mode", "") ||
				!grid.property_value("mode").empty() ||
				std::string(vgui_widget_get_text(editor)) != "None" ||
				changes != 0)
				return fail("enum silent empty");
			if (!grid.commit_property_value("mode", "None") ||
				!grid.property_value("mode").empty() || changes != 0 ||
				commands.can_undo())
				return fail("enum placeholder commit");
			if (!grid.set_property_value("mode", "high") ||
				!grid.reset_property("mode") ||
				!grid.property_value("mode").empty() ||
				std::string(vgui_widget_get_text(editor)) != "None" ||
				changes != 1 || !commands.can_undo())
				return fail("enum empty reset");
			if (!commands.undo() || grid.property_value("mode") != "high")
				return fail("enum reset undo");
			if (!commands.redo() || !grid.property_value("mode").empty())
				return fail("enum reset redo");
			commands.clear();
			changes = 0;

			if (grid.commit_property_value("strict", "") ||
				grid.property_error("strict").empty() ||
				grid.property_value("strict") != "low")
				return fail("enum strict empty rejected");
			if (grid.commit_property_value("strict", "None") ||
				grid.property_value("strict") != "low")
				return fail("enum strict placeholder rejected");
			if (!grid.commit_property_value("strict", "High") ||
				grid.property_value("strict") != "high" ||
				!grid.property_error("strict").empty())
				return fail("enum strict label commit");
			commands.clear();
			changes = 0;

			vgui_layout(ui);
			vgui_widget* label = find_widget(grid.widget(), "property:mode",
				VGUI_LABEL);
			if (!label)
				return fail("enum label widget");
			float lx = 0.0f, ly = 0.0f, lw = 0.0f, lh = 0.0f;
			vgui_widget_get_rect(label, &lx, &ly, &lw, &lh);
			if (lw <= 0.0f || lh <= 0.0f)
				return fail("enum label layout size");
			const float mx = lx + lw * 0.5f;
			const float my = ly + lh * 0.5f;
			vgui_set_focus(ui, nullptr);
			vgui_input_mouse_move(ui, mx, my);
			vgui_input_mouse_down(ui, 1, mx, my, 1);
			vgui_input_mouse_up(ui, 1, mx, my, 1);
			vgui_widget* menu = vgui_context_get_open_menu(ui);
			if (!menu || vgui_get_focus(ui) != editor)
				return fail("enum label click opens and keeps focus");
			if (vgui_menu_get_count(menu) != 4)
				return fail("enum empty menu count");
			vgui_item_id none_item = vgui_menu_get_item_at(menu, 0);
			vgui_item_id low_item = vgui_menu_get_item_at(menu, 1);
			vgui_item_id medium_item = vgui_menu_get_item_at(menu, 2);
			vgui_item_id high_item = vgui_menu_get_item_at(menu, 3);
			if (!none_item || !low_item || !medium_item || !high_item)
				return fail("enum empty menu items");
			if (std::string(vgui_menu_get_item_text(menu, none_item)) !=
					"None" ||
				vgui_menu_get_cursor(menu) != none_item ||
				vgui_menu_get_item_checked(menu, none_item) != 1 ||
				vgui_menu_get_item_enabled(menu, medium_item) != 0)
				return fail("enum empty menu state");
			vgui_input_key(ui, VGUI_KEY_ESCAPE, 1, 0);
			vgui_input_key(ui, VGUI_KEY_ESCAPE, 0, 0);
			if (vgui_context_get_open_menu(ui) ||
				!grid.property_value("mode").empty() || changes != 0 ||
				vgui_get_focus(ui) != editor)
				return fail("enum escape keeps value and focus");

			float ex = 0.0f, ey = 0.0f, ew = 0.0f, eh = 0.0f;
			vgui_widget_get_rect(editor, &ex, &ey, &ew, &eh);
			if (ew <= 0.0f || eh <= 0.0f)
				return fail("enum editor layout size");
			const float ecx = ex + ew * 0.5f;
			const float ecy = ey + eh * 0.5f;
			vgui_input_mouse_move(ui, ecx, ecy);
			vgui_input_mouse_down(ui, 1, ecx, ecy, 1);
			vgui_input_mouse_up(ui, 1, ecx, ecy, 1);
			menu = vgui_context_get_open_menu(ui);
			if (!menu || vgui_menu_get_cursor(menu) != none_item)
				return fail("enum editor opens on selection");
			int handled = vgui_input_key(ui, VGUI_KEY_DOWN, 1, 0);
			vgui_input_key(ui, VGUI_KEY_DOWN, 0, 0);
			if (!handled || vgui_menu_get_cursor(menu) != low_item)
				return fail("enum menu keyboard down");
			handled = vgui_input_key(ui, VGUI_KEY_DOWN, 1, 0);
			vgui_input_key(ui, VGUI_KEY_DOWN, 0, 0);
			if (!handled || vgui_menu_get_cursor(menu) != high_item)
				return fail("enum menu skips disabled choice");
			handled = vgui_input_key(ui, VGUI_KEY_ENTER, 1, 0);
			vgui_input_key(ui, VGUI_KEY_ENTER, 0, 0);
			if (!handled || vgui_context_get_open_menu(ui) ||
				grid.property_value("mode") != "high" || changes != 1 ||
				changed_id != "mode" || changed_value != "high" ||
				std::string(vgui_widget_get_text(editor)) != "High" ||
				vgui_get_focus(ui) != editor || !commands.can_undo())
				return fail("enum menu selection");
			if (!commands.undo() || !grid.property_value("mode").empty() ||
				std::string(vgui_widget_get_text(editor)) != "None")
				return fail("enum menu undo");
			if (!commands.redo() || grid.property_value("mode") != "high")
				return fail("enum menu redo");

			vgui_input_mouse_move(ui, ecx, ecy);
			vgui_input_mouse_down(ui, 1, ecx, ecy, 1);
			vgui_input_mouse_up(ui, 1, ecx, ecy, 1);
			menu = vgui_context_get_open_menu(ui);
			if (!menu || vgui_menu_get_cursor(menu) != high_item)
				return fail("enum menu cursor tracks value");
			vgui_input_key(ui, VGUI_KEY_HOME, 1, 0);
			vgui_input_key(ui, VGUI_KEY_HOME, 0, 0);
			if (vgui_menu_get_cursor(menu) != none_item)
				return fail("enum menu home to empty");
			vgui_input_key(ui, VGUI_KEY_ENTER, 1, 0);
			vgui_input_key(ui, VGUI_KEY_ENTER, 0, 0);
			if (vgui_context_get_open_menu(ui) ||
				!grid.property_value("mode").empty() ||
				std::string(vgui_widget_get_text(editor)) != "None" ||
				changes != 4 || vgui_get_focus(ui) != editor)
				return fail("enum menu selects empty");
			if (!commands.undo() || grid.property_value("mode") != "high")
				return fail("enum empty undo");
			if (!commands.redo() || !grid.property_value("mode").empty())
				return fail("enum empty redo");
		}

		vgui_destroy(ui);
		return 0;
	}

	int test_property_grid_vector()
	{
		using namespace vgui_editor_kit;

		vgui_context* ui = vgui_create(nullptr, 16.0f);
		if (!ui)
			return fail("property grid vector context");
		vgui_context_set_size(ui, 900, 700);

		{
			property_grid grid;
			command_stack commands;
			grid.build(vgui_root(ui));
			grid.set_command_stack(&commands);

			int changes = 0;
			std::string changed_id;
			std::string changed_value;
			grid.set_change_listener([&](const std::string& id,
				const std::string& value) {
				++changes;
				changed_id = id;
				changed_value = value;
			});

			property_desc position;
			position.id = "position";
			position.label = "Position";
			position.type = property_type::vector3;
			position.value = "1,2,3";
			position.category = "Transform";
			position.semantic = vector_semantic::position;
			position.step = 1.0;
			position.has_range = true;
			position.min_value = -10.0;
			position.max_value = 10.0;
			position.has_default = true;
			position.default_value = "0, 0, 0";
			position.tooltip = "Object position";
			if (!grid.add_property(position))
				return fail("add vector3 property");

			property_desc uv;
			uv.id = "uv";
			uv.label = "UV";
			uv.type = property_type::vector2;
			uv.value = "0.5; 0.25";
			uv.category = "Rendering";
			uv.component_labels = { "U", "V" };
			uv.component_colors = { 0xFF010203, 0xFF040506 };
			if (!grid.add_property(uv))
				return fail("add vector2 property");

			property_desc tint;
			tint.id = "tint";
			tint.label = "Tint";
			tint.type = property_type::vector4;
			tint.value = "1, 0, 0, 1";
			tint.category = "Rendering";
			tint.semantic = vector_semantic::color;
			if (!grid.add_property(tint))
				return fail("add vector4 property");

			property_desc clamped;
			clamped.id = "clamped";
			clamped.label = "Clamped";
			clamped.type = property_type::vector2;
			clamped.value = "5, -5";
			clamped.category = "Transform";
			clamped.has_range = true;
			clamped.min_value = 0.0;
			clamped.max_value = 1.0;
			if (!grid.add_property(clamped))
				return fail("add clamped vector property");

			if (grid.property_value("position") != "1, 2, 3" ||
				grid.property_value("uv") != "0.5, 0.25" ||
				grid.property_value("tint") != "1, 0, 0, 1" ||
				grid.property_value("clamped") != "1, 0")
				return fail("vector property normalization");

			vgui_update(ui, 1.0f / 60.0f);
			vgui_layout(ui);

			vector_editor* pos = grid.property_vector_editor("position");
			vector_editor* uv_editor = grid.property_vector_editor("uv");
			vector_editor* tint_editor = grid.property_vector_editor("tint");
			if (!pos || !uv_editor || !tint_editor)
				return fail("vector property editors");
			if (grid.property_editor("position") != pos->widget() ||
				pos->dimension() != 3 ||
				pos->value_text() != "1, 2, 3" ||
				pos->axis_labels()[0] != "X" ||
				uv_editor->dimension() != 2 ||
				uv_editor->axis_labels()[0] != "U" ||
				uv_editor->axis_labels()[1] != "V" ||
				uv_editor->axis_colors()[0] != 0xFF010203 ||
				tint_editor->dimension() != 4 ||
				tint_editor->axis_labels()[3] != "A")
				return fail("vector property editor setup");
			if (!pos->component_edit_widget(0) ||
				std::string(vgui_widget_get_tooltip(
					pos->component_edit_widget(0))) != "Object position")
				return fail("vector property component tooltip");

			const std::size_t undo_before = commands.undo_count();
			changes = 0;
			if (!pos->commit_component_text(1, "7") ||
				grid.property_value("position") != "1, 7, 3" ||
				pos->value_text() != "1, 7, 3" ||
				changes != 1 || changed_id != "position" ||
				changed_value != "1, 7, 3" ||
				commands.undo_count() != undo_before + 1)
				return fail("vector property component commit");
			if (!commands.undo() ||
				grid.property_value("position") != "1, 2, 3" ||
				pos->value_text() != "1, 2, 3")
				return fail("vector property undo");
			if (!commands.redo() ||
				grid.property_value("position") != "1, 7, 3" ||
				pos->value_text() != "1, 7, 3")
				return fail("vector property redo");

			changes = 0;
			const std::size_t undo_before_enter = commands.undo_count();
			pos->focus_component(2);
			vgui_edit_set_text(pos->component_edit_widget(2), "9");
			vgui_input_key(ui, VGUI_KEY_ENTER, 1, 0);
			vgui_input_key(ui, VGUI_KEY_ENTER, 0, 0);
			if (grid.property_value("position") != "1, 7, 9" ||
				changes != 1 || changed_value != "1, 7, 9" ||
				commands.undo_count() != undo_before_enter + 1)
				return fail("vector property enter edit");
			if (!commands.undo() ||
				grid.property_value("position") != "1, 7, 3")
				return fail("vector property enter undo");

			vgui_widget* label = find_widget(grid.widget(), "property:position",
				VGUI_LABEL);
			if (!label)
				return fail("vector property label");
			float lx = 0.0f, ly = 0.0f, lw = 0.0f, lh = 0.0f;
			vgui_widget_get_rect(label, &lx, &ly, &lw, &lh);
			if (lw <= 0.0f || lh <= 0.0f)
				return fail("vector property label layout");
			const float start_x = lx + lw * 0.5f;
			const float start_y = ly + lh * 0.5f;
			const std::size_t undo_before_drag = commands.undo_count();
			vgui_input_mouse_move(ui, start_x, start_y);
			vgui_input_mouse_down(ui, 1, start_x, start_y, 1);
			vgui_input_mouse_move(ui, start_x + 2.0f, start_y);
			if (grid.property_value("position") != "1, 7, 3" ||
				pos->value_text() != "3, 9, 5" ||
				commands.undo_count() != undo_before_drag)
				return fail("vector property drag is live");
			vgui_input_mouse_move(ui, start_x + 20.0f, start_y);
			if (pos->value_text() != "10, 10, 10")
				return fail("vector property drag clamps");
			vgui_input_mouse_up(ui, 1, start_x + 20.0f, start_y, 1);
			if (grid.property_value("position") != "10, 10, 10" ||
				commands.undo_count() != undo_before_drag + 1)
				return fail("vector property drag commit");
			if (!commands.undo() ||
				grid.property_value("position") != "1, 7, 3" ||
				pos->value_text() != "1, 7, 3")
				return fail("vector property drag undo");
			if (!commands.redo() ||
				grid.property_value("position") != "10, 10, 10")
				return fail("vector property drag redo");

			changes = 0;
			if (!grid.set_property_read_only("position", true) ||
				!pos->read_only() ||
				vgui_widget_get_enabled(grid.property_editor("position")) != 0)
				return fail("vector property read-only");
			if (pos->commit_component_text(0, "2") ||
				pos->component_error(0) != "value is disabled" ||
				grid.property_value("position") != "10, 10, 10" ||
				changes != 0)
				return fail("vector property read-only edit");
			grid.set_property_read_only("position", false);
			pos->clear_component_errors();
			if (pos->read_only() ||
				vgui_widget_get_enabled(grid.property_editor("position")) == 0)
				return fail("vector property read-only recovery");

			changes = 0;
			const std::size_t undo_before_invalid = commands.undo_count();
			if (pos->commit_component_text(0, "oops") ||
				pos->component_error(0).empty() ||
				grid.property_value("position") != "10, 10, 10" ||
				changes != 0 ||
				commands.undo_count() != undo_before_invalid)
				return fail("vector property invalid component");
			if (!pos->commit_component_text(0, "2") ||
				grid.property_value("position") != "2, 10, 10" ||
				changes != 1 ||
				commands.undo_count() != undo_before_invalid + 1)
				return fail("vector property invalid recovery");
			if (!commands.undo() ||
				grid.property_value("position") != "10, 10, 10")
				return fail("vector property invalid recovery undo");

			const std::size_t undo_before_reset = commands.undo_count();
			if (!grid.reset_property("position") ||
				grid.property_value("position") != "0, 0, 0" ||
				pos->value_text() != "0, 0, 0" ||
				commands.undo_count() != undo_before_reset + 1)
				return fail("vector property reset");
			if (!commands.undo() ||
				grid.property_value("position") != "10, 10, 10")
				return fail("vector property reset undo");

			changes = 0;
			const std::size_t undo_before_silent = commands.undo_count();
			if (!grid.set_property_value("position", "[4, 5, 6]") ||
				grid.property_value("position") != "4, 5, 6" ||
				pos->value_text() != "4, 5, 6" ||
				changes != 0 ||
				commands.undo_count() != undo_before_silent)
				return fail("vector property silent update");

			vector_editor::vector_value gizmo{ 4.0, 5.0, 6.0, 0.0 };
			grid.set_change_listener([&](const std::string& id,
				const std::string& value) {
				++changes;
				changed_id = id;
				changed_value = value;
				if (id == "position")
				{
					vector_editor::vector_value parsed{};
					if (vector_editor::parse_value(value, 3, parsed))
						gizmo = parsed;
				}
			});
			pos->set_value_source(
				[&](vector_editor::vector_value& out) {
					out = gizmo;
					return true;
				});
			gizmo = { 7.0, 8.0, 9.0, 0.0 };
			changes = 0;
			pos->refresh();
			if (changes != 0 || pos->value_text() != "7, 8, 9")
				return fail("vector property host refresh");
			if (!grid.set_property_value("position", pos->value_text()) ||
				grid.property_value("position") != "7, 8, 9")
				return fail("vector property host refresh sync");
			if (!pos->commit_component_text(0, "6") ||
				gizmo[0] != 6.0 ||
				grid.property_value("position") != "6, 8, 9" ||
				changed_value != "6, 8, 9")
				return fail("vector property gizmo binding");

			grid.set_filter("uv");
			if (grid.property_vector_editor("position") != nullptr ||
				!grid.property_vector_editor("uv"))
				return fail("vector property filter release");
			grid.set_filter("");
			pos = grid.property_vector_editor("position");
			if (!pos || pos->value_text() != "6, 8, 9")
				return fail("vector property filter rematerialize");
		}

		vgui_destroy(ui);
		return 0;
	}

} // namespace

	int test_scene_asset_commands()
	{
		using namespace vgui_editor_kit;

		vgui_context* ui = vgui_create(nullptr, 16.0f);
		if (!ui)
			return fail("scene asset commands context");
		vgui_context_set_size(ui, 900, 700);
		vgui_style test_style;
		vgui_style_default(&test_style);
		test_style.list.row_height = 24.0f;
		test_style.tree.row_height = 24.0f;
		vgui_context_set_style(ui, &test_style);

		editor_context context(ui);
		command_stack commands;

		/* Outliner move_node: host model, undo/redo, cycle rejection. */
		{
			outliner_view scene;
			scene.build(context, vgui_root(ui));
			scene.set_command_stack(&commands);

			std::map<object_id, object_id> parent_of;
			std::map<object_id, std::vector<object_id>> children;
			std::vector<object_id> roots;
			std::map<object_id, vgui_item_id> item_ids;
			auto set_parent = [&](object_id node, object_id parent) {
				object_id& old = parent_of[node];
				auto& siblings = old == invalid_object ? roots : children[old];
				siblings.erase(std::remove(siblings.begin(), siblings.end(), node),
					siblings.end());
				old = parent;
				auto& target = parent == invalid_object ? roots : children[parent];
				target.push_back(node);
			};

			auto rebuild = [&](std::vector<object_id>& nodes, vgui_item_id parent_item,
				auto&& self) -> void {
				for (object_id node : nodes)
				{
					vgui_item_id item = scene.add_node(
						("node " + std::to_string(node)).c_str(), parent_item, node);
					if (item != 0)
					{
						item_ids[node] = item;
						self(children[node], item, self);
					}
				}
			};

			outliner_view::move_handler handler =
				[&](object_id node, object_id parent, std::size_t index) -> bool {
				/* Reject moves into the node's own subtree. */
				object_id walk = parent;
				while (walk != invalid_object)
				{
					if (walk == node)
						return false;
					walk = parent_of[walk];
				}
				set_parent(node, parent);
				auto& siblings =
					parent == invalid_object ? roots : children[parent];
				if (index < siblings.size() && siblings[index] != node)
				{
					for (std::size_t i = siblings.size() - 1; i > index; --i)
						std::swap(siblings[i], siblings[i - 1]);
				}
				/* Mirror the move into the tree widget. */
				scene.clear();
				rebuild(roots, 0, rebuild);
				return true;
			};
			scene.set_move_handler(handler);

			set_parent(1, invalid_object);
			set_parent(2, 1);
			set_parent(3, 1);
			set_parent(4, 2);
			rebuild(roots, 0, rebuild);

			/* F03: 通过通用拖放重排——行 node 3 拖到行 node 4 上，move_node
			 * 进命令栈，undo 恢复。可见行序：1, 2, 4, 3。 */
			scene.enable_drag_drop();
			vgui_widget_set_pos(scene.widget(), 10.0f, 10.0f);
			vgui_widget_set_size(scene.widget(), 240.0f, 200.0f);
			scene.expand(item_ids[1]);
			scene.expand(item_ids[2]);
			vgui_update(ui, 0.016f);
			{
				float tx = 0.0f, ty = 0.0f, tw = 0.0f, th = 0.0f;
				vgui_widget_get_rect(scene.widget(), &tx, &ty, &tw, &th);
				(void)tw;
				(void)th;
				const float row_h = test_style.tree.row_height;
				const float pad_y = test_style.tree.padding_y;
				const float drag_y = ty + pad_y + row_h * 3.0f + row_h * 0.5f;
				const float drop_y = ty + pad_y + row_h * 2.0f + row_h * 0.5f;
				const std::size_t undo_before_drag = commands.undo_count();
				vgui_input_mouse_move(ui, tx + 30.0f, drag_y);
				vgui_input_mouse_down(ui, 1, tx + 30.0f, drag_y, 1);
				vgui_input_mouse_move(ui, tx + 30.0f, drop_y);
				if (!vgui_drag_active(ui))
					return fail("outliner drag begins");
				vgui_input_mouse_up(ui, 1, tx + 30.0f, drop_y, 1);
				if (parent_of[3] != 2 || children[2].size() != 2 ||
					children[2][0] != 3 ||
					commands.undo_count() != undo_before_drag + 1)
					return fail("outliner drag drop reorders");
				if (!commands.undo() || parent_of[3] != 1)
					return fail("outliner drag drop undo");
			}

			const std::size_t undo_base = commands.undo_count();
			if (!scene.move_node(3, 2, 0) || parent_of[3] != 2 ||
				commands.undo_count() != undo_base + 1)
				return fail("outliner move records command");
			if (!commands.undo() || parent_of[3] != 1)
				return fail("outliner move undo");
			if (!commands.redo() || parent_of[3] != 2)
				return fail("outliner move redo");

			if (!scene.move_node(3, 2, 0) || commands.undo_count() != undo_base + 1)
				return fail("outliner no-op move");

			const std::size_t undo_before_reject = commands.undo_count();
			if (scene.move_node(1, 2, 0))
				return fail("outliner cycle move rejected");
			if (commands.undo_count() != undo_before_reject)
				return fail("outliner rejected move recorded");

			if (!scene.move_node(3, invalid_object, 0) || parent_of[3] != invalid_object)
				return fail("outliner move to root");
			if (!commands.undo() || parent_of[3] != 2)
				return fail("outliner move to root undo");
		}

		/* Asset rename: handler rejection and undo/redo. */
		{
			asset_browser assets;
			assets.build(vgui_root(ui));
			assets.set_command_stack(&commands);

			std::map<vgui_item_id, std::string> names;
			assets.set_rename_handler(
				[&](vgui_item_id item, const std::string& name) -> bool {
					if (name.empty())
						return false;
					names[item] = name;
					return true;
				});

			const vgui_item_id asset = assets.add_item("Cube.mesh", "Mesh");
			if (asset == 0)
				return fail("asset rename item");
			names[asset] = "Cube.mesh";

			const std::size_t undo_before_rename = commands.undo_count();
			if (!assets.rename_item(asset, "Sphere.mesh") ||
				names[asset] != "Sphere.mesh" ||
				std::string(vgui_list_get_item_text(assets.widget(), asset)) !=
					"Sphere.mesh" ||
				commands.undo_count() != undo_before_rename + 1)
				return fail("asset rename records command");
			if (!commands.undo() || names[asset] != "Cube.mesh" ||
				std::string(vgui_list_get_item_text(assets.widget(), asset)) !=
					"Cube.mesh")
				return fail("asset rename undo");
			if (!commands.redo() || names[asset] != "Sphere.mesh")
				return fail("asset rename redo");

			if (assets.rename_item(asset, "") ||
				commands.undo_count() != undo_before_rename + 1 ||
				names[asset] != "Sphere.mesh")
				return fail("asset rejected rename recorded");
		}

		/* Property grid: consecutive commits on one row merge into one record;
		 * a display-only update in between breaks the merge chain. */
		{
			property_grid grid;
			grid.build(vgui_root(ui));
			grid.set_command_stack(&commands);

			property_desc hp;
			hp.id = "hp";
			hp.label = "HP";
			hp.type = property_type::integer;
			hp.value = "1";
			hp.category = "General";
			if (!grid.add_property(hp))
				return fail("merge property add");

			const std::size_t undo_before = commands.undo_count();
			if (!grid.commit_property_value("hp", "2") ||
				!grid.commit_property_value("hp", "3"))
				return fail("merge property commits");
			if (grid.property_value("hp") != "3" ||
				commands.undo_count() != undo_before + 1)
				return fail("property merge single record");
			if (!commands.undo() || grid.property_value("hp") != "1")
				return fail("property merge undo to start");

			if (!grid.commit_property_value("hp", "5"))
				return fail("merge restart commit");
			if (!grid.set_property_value("hp", "6"))
				return fail("display-only update");
			if (!grid.commit_property_value("hp", "7") ||
				commands.undo_count() != undo_before + 2)
				return fail("display-only update breaks merge");
			if (!commands.undo() || grid.property_value("hp") != "6")
				return fail("separated record undo");
		}

		vgui_destroy(ui);
		return 0;
	}

	int test_toolbar_and_status_components()
	{
		using namespace vgui_editor_kit;

		vgui_context* ui = vgui_create(nullptr, 16.0f);
		if (!ui)
			return fail("toolbar component context");
		vgui_context_set_size(ui, 1280, 800);

		{
			vgui_widget* root = vgui_root(ui);

			int clicks = 0;
			icon_button icon;
			icon.build(root, "Run");
			icon.set_tooltip("Run command");
			icon.set_on_click([&clicks]() { ++clicks; });

			vgui_texture_desc icon_desc = {};
			icon_desc.width = 4;
			icon_desc.height = 4;
			icon_desc.format = VGUI_TEXTURE_FORMAT_RGBA8;
			unsigned char icon_pixels[64] = {};
			icon_desc.pixels = icon_pixels;
			vgui_texture* icon_texture = vgui_texture_create(ui, &icon_desc);
			if (!icon_texture)
				return fail("icon button texture");
			icon.set_icon(icon_texture, 12, 12);

			if (!icon.widget() ||
				vgui_widget_get_kind(icon.widget()) != VGUI_BUTTON ||
				std::string(vgui_widget_get_text(icon.widget())) != "Run")
				return fail("icon button build");
			icon.click();
			if (clicks != 1)
				return fail("icon button click");
			icon.set_checked(true);
			if (!icon.checked() || vgui_widget_get_checked(icon.widget()) != 1)
				return fail("icon button checked");
			icon.set_enabled(false);
			icon.click();
			if (icon.enabled() || clicks != 1)
				return fail("icon button disabled");

			toggle_button toggle;
			toggle.build(root, "Snap");
			int toggles = 0;
			bool last_toggle = false;
			toggle.set_on_change([&](bool value) {
				++toggles;
				last_toggle = value;
			});
			toggle.toggle();
			if (!toggle.checked() || toggles != 1 || !last_toggle)
				return fail("toggle button on");
			toggle.set_checked(false);
			if (toggle.checked() || toggles != 1)
				return fail("toggle button silent set");
			toggle.set_enabled(false);
			toggle.toggle();
			if (toggle.checked() || toggles != 1)
				return fail("toggle button disabled");

			segmented_control segments;
			segments.build(root);
			if (segments.add_segment("One") != 0 ||
				segments.add_segment("Two") != 1 ||
				segments.add_segment("Three") != 2 ||
				segments.count() != 3 || !segments.has_selection() ||
				segments.selected() != 0)
				return fail("segmented control add");
			int selections = 0;
			std::size_t last_selection = 99;
			segments.set_on_change([&](std::size_t index) {
				++selections;
				last_selection = index;
			});
			if (!segments.select(1) || segments.selected() != 1 ||
				selections != 1 || last_selection != 1)
				return fail("segmented control select");
			if (!segments.select(1) || selections != 1)
				return fail("segmented control same selection");
			if (segments.select(7))
				return fail("segmented control invalid selection");
			segments.set_segment_enabled(2, false);
			if (segments.segment_enabled(2) || segments.select(2) ||
				selections != 1)
				return fail("segmented control disabled segment");
			segments.set_segment_enabled(2, true);
			if (!segments.select(2) || selections != 2)
				return fail("segmented control re-enabled segment");
			segments.set_selected(0);
			if (segments.selected() != 0 || selections != 2 ||
				vgui_widget_get_checked(segments.segment_widget(0)) != 1 ||
				vgui_widget_get_checked(segments.segment_widget(2)) != 0)
				return fail("segmented control silent selection");

			command_registry commands;
			int runs = 0;
			bool command_enabled = true;
			commands.add("test.run", "Run", [&runs]() { ++runs; });
			commands.set_enabled("test.run",
				[&command_enabled]() { return command_enabled; });
			commands.set_checked("test.run", []() { return true; });

			toolbar_view toolbar;
			toolbar.build(root);
			icon_button* command = toolbar.add_command_button(commands, "test.run");
			if (!toolbar.widget() || !command || !command->enabled() ||
				!command->checked() ||
				std::string(vgui_widget_get_text(command->widget())) != "Run")
				return fail("toolbar command button");
			command->click();
			if (runs != 1)
				return fail("toolbar command invoke");
			command_enabled = false;
			toolbar.refresh_commands();
			if (command->enabled())
				return fail("toolbar command refresh");
			command->click();
			if (runs != 1)
				return fail("toolbar disabled command");
			commands.add("test.run", "Run updated", [&runs]() { ++runs; });
			toolbar.refresh_commands();
			if (std::string(vgui_widget_get_text(command->widget())) !=
				"Run updated")
				return fail("toolbar command title refresh");
			if (toolbar.add_command_button(commands, "missing"))
				return fail("toolbar unknown command");

			toggle_button& toolbar_toggle = toolbar.add_toggle_button("Grid", true);
			segmented_control& toolbar_segments = toolbar.add_segmented_control();
			toolbar_segments.add_segment("Persp");
			toolbar_segments.add_segment("Top");
			if (!toolbar_toggle.checked() || toolbar_segments.count() != 2 ||
				!toolbar.add_label("Status") || !toolbar.add_spacer() ||
				!toolbar.add_separator())
				return fail("toolbar extra controls");

			status_bar status;
			status.set_message("before build");
			status.set_selected_object(12);
			status.set_coordinates(1.25f, -2.5f);
			status.set_error("broken");
			status.build(root);
			if (!status.widget() || status.message() != "before build" ||
				status.selection_text() != "Selected: 12" ||
				status.coordinate_text() != "x 1.25  y -2.50" ||
				status.error() != "broken")
				return fail("status bar fields");
			if (std::string(vgui_widget_get_text(status.message_widget())) !=
				"before build" ||
				std::string(vgui_widget_get_text(status.selection_widget())) !=
				"Selected: 12" ||
				std::string(vgui_widget_get_text(status.coordinate_widget())) !=
				"x 1.25  y -2.50" ||
				std::string(vgui_widget_get_text(status.error_widget())) != "broken")
				return fail("status bar labels");
			status.clear_error();
			status.set_selected_object(invalid_object);
			if (!status.error().empty() || !status.selection_text().empty() ||
				!std::string(vgui_widget_get_text(status.error_widget())).empty())
				return fail("status bar clear");

			search_box search;
			search.build(root, "Find");
			int search_changes = 0;
			int submits = 0;
			std::string changed;
			std::string submitted;
			search.set_on_change([&](const std::string& text) {
				++search_changes;
				changed = text;
			});
			search.set_on_submit([&](const std::string& text) {
				++submits;
				submitted = text;
			});
			search.set_text("cube");
			if (search.text() != "cube" || search_changes != 0 ||
				vgui_widget_get_visible(search.clear_widget()) != 1)
				return fail("search box set text");
			search.submit();
			if (submits != 1 || submitted != "cube")
				return fail("search box submit");
			search.clear();
			if (!search.text().empty() || search_changes != 1 ||
				!changed.empty() ||
				vgui_widget_get_visible(search.clear_widget()) != 0)
				return fail("search box clear");

			breadcrumb path;
			path.build(root);
			int navigations = 0;
			std::size_t navigated_index = 99;
			std::string navigated_id;
			path.set_on_navigate([&](std::size_t index, const std::string& id) {
				++navigations;
				navigated_index = index;
				navigated_id = id;
			});
			path.set_items({
				{ "Scene", "scene" },
				{ "World", "world" },
				{ "Cube", "cube" } });
			if (path.count() != 3 || path.item(1).label != "World" ||
				path.item(2).id != "cube" || path.active() != 2 ||
				vgui_widget_get_checked(path.item_widget(2)) != 1)
				return fail("breadcrumb items");
			if (!path.navigate(1) || navigations != 1 ||
				navigated_index != 1 || navigated_id != "world")
				return fail("breadcrumb navigate");
			if (path.navigate(3))
				return fail("breadcrumb invalid navigate");
			path.set_active(0);
			if (path.active() != 0 ||
				vgui_widget_get_checked(path.item_widget(0)) != 1 ||
				vgui_widget_get_checked(path.item_widget(2)) != 0)
				return fail("breadcrumb active");
			path.add_item("Extra", "extra");
			if (path.count() != 4 || path.active() != 3)
				return fail("breadcrumb add item");
			path.clear();
			if (path.count() != 0 || !path.item(0).label.empty())
				return fail("breadcrumb clear");

			vgui_update(ui, 1.0f / 60.0f);
			vgui_layout(ui);
			search.focus();
			if (vgui_get_focus(ui) != search.edit_widget())
				return fail("search box focus");
			vgui_input_text(ui, "ab");
			if (search.text() != "ab" || search_changes != 2 ||
				changed != "ab")
				return fail("search box typing");
			vgui_input_key(ui, VGUI_KEY_ENTER, 1, 0);
			vgui_input_key(ui, VGUI_KEY_ENTER, 0, 0);
			if (submits != 2 || submitted != "ab")
				return fail("search box enter");
		}

		vgui_destroy(ui);
		return 0;
	}

int main()
{
	using namespace vgui_editor_kit;

	if (int rc = test_command_stack())
		return rc;
	if (int rc = test_command_merge_and_failures())
		return rc;
	if (int rc = test_layout_persistence())
		return rc;
	if (int rc = test_commands_and_menus())
		return rc;
	if (int rc = test_selection_subscriptions())
		return rc;
	if (int rc = test_property_grid())
		return rc;
	if (int rc = test_property_grid_virtualization())
		return rc;
	if (int rc = test_property_grid_drag())
		return rc;
	if (int rc = test_spinbox())
		return rc;
	if (int rc = test_vector_editor())
		return rc;
	if (int rc = test_combo_box())
		return rc;
	if (int rc = test_property_grid_enum())
		return rc;
	if (int rc = test_property_grid_vector())
		return rc;
	if (int rc = test_selection_binding())
		return rc;
	if (int rc = test_scene_asset_commands())
		return rc;
	if (int rc = test_toolbar_and_status_components())
		return rc;

	vgui_context* ui = vgui_create(nullptr, 16.0f);
	if (!ui)
		return fail("vgui_create failed");

	vgui_context_set_size(ui, 1280, 800);
	vgui_style shell_style;
	vgui_style_default(&shell_style);
	shell_style.list.row_height = 24.0f;
	shell_style.tree.row_height = 24.0f;
	vgui_context_set_style(ui, &shell_style);

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

		if (!shell.toolbar().widget() || !shell.statusbar().widget() ||
			!shell.search().widget() || !shell.path().widget())
			return fail("shell toolbar and status widgets");
		if (shell.statusbar().message() != "ready" ||
			shell.statusbar().selection_text() != "Selected: 7")
			return fail("shell status selection");
		if (shell.path().count() != 2 || shell.path().item(1).id != "object:7")
			return fail("shell breadcrumb selection");

		float x = 0, y = 0, w = 0, h = 0;
		vgui_widget_get_rect(shell.widget(), &x, &y, &w, &h);
		if (w <= 0.0f || h <= 0.0f)
			return fail("shell layout size");

		vgui_widget* dock = shell.dock();
		if (!dock || vgui_widget_get_kind(dock) != VGUI_DOCK)
			return fail("shell dock");
		if (vgui_dock_get_count(dock) != 4)
			return fail("shell dock panel count");
		vgui_item_id outliner_panel = vgui_dock_find(dock, "outliner");
		vgui_item_id viewport_panel = vgui_dock_find(dock, "viewport");
		vgui_item_id properties_panel = vgui_dock_find(dock, "properties");
		vgui_item_id bottom_panel = vgui_dock_find(dock, "bottom");
		if (!outliner_panel || !viewport_panel || !properties_panel || !bottom_panel)
			return fail("shell dock panel ids");
		if (vgui_dock_get_zone(dock, outliner_panel) != VGUI_DOCK_LEFT ||
			vgui_dock_get_zone(dock, viewport_panel) != VGUI_DOCK_CENTER ||
			vgui_dock_get_zone(dock, properties_panel) != VGUI_DOCK_RIGHT ||
			vgui_dock_get_zone(dock, bottom_panel) != VGUI_DOCK_BOTTOM)
			return fail("shell dock zones");
		if (shell.body_splitter() || shell.workspace_splitter() ||
			shell.center_splitter())
			return fail("shell legacy splitter handles");
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
		if (shell.statusbar().selection_text() != "Selected: 42" ||
			shell.path().count() != 2 ||
			shell.path().item(1).label != "Object 42")
			return fail("shell status follows selection");

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

		if (shell.properties().command_stack_ptr() != &context.commands())
			return fail("shell property grid command stack");
		property_desc health;
		health.id = "shell.health";
		health.label = "Health";
		health.type = property_type::integer;
		health.value = "10";
		health.category = "Gameplay";
		if (!shell.properties().add_property(health))
			return fail("shell typed property add");
		const std::size_t shell_undo = context.commands().undo_count();
		if (!shell.properties().commit_property_value("shell.health", "25") ||
			shell.properties().property_value("shell.health") != "25")
			return fail("shell typed property commit");
		if (context.commands().undo_count() != shell_undo + 1)
			return fail("shell typed property command record");
		if (!context.commands().undo() ||
			shell.properties().property_value("shell.health") != "10")
			return fail("shell typed property undo");
		if (!context.commands().redo() ||
			shell.properties().property_value("shell.health") != "25")
			return fail("shell typed property redo");

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

		/* F03: kit payload 与核心载荷往返；资产行拖到视口 / 属性栏。 */
		{
			vgui_drag_payload core_payload = {};
			core_payload.kind = VGUI_DRAG_CUSTOM;
			core_payload.text = "42";
			core_payload.data = reinterpret_cast<const void*>(
				static_cast<std::uintptr_t>(drag_payload_kind::scene_object));
			const drag_payload back = to_editor_payload(core_payload);
			if (back.kind != drag_payload_kind::scene_object || back.id != "42")
				return fail("payload round trip");

			const char* file_list[] = { "assets/a.png" };
			core_payload.kind = VGUI_DRAG_FILES;
			core_payload.text = nullptr;
			core_payload.files = file_list;
			core_payload.file_count = 1;
			const drag_payload file_back = to_editor_payload(core_payload);
			if (file_back.kind != drag_payload_kind::files ||
				file_back.files.size() != 1 || file_back.files[0] != "assets/a.png")
				return fail("file payload round trip");
		}

		{
			vgui_update(ui, 1.0f / 60.0f);
			vgui_layout(ui);
			float arx = 0, ary = 0, arw = 0, arh = 0;
			vgui_widget_get_rect(shell.assets().widget(), &arx, &ary, &arw, &arh);
			float vpx = 0, vpy = 0, vpw = 0, vph = 0;
			vgui_widget_get_rect(shell.viewport().widget(), &vpx, &vpy, &vpw, &vph);
			float ppx = 0, ppy = 0, ppw = 0, pph = 0;
			vgui_widget_get_rect(shell.properties().widget(), &ppx, &ppy, &ppw, &pph);
			const float arow_y = ary + shell_style.list.padding_y +
				shell_style.list.row_height * 0.5f;
			const float viewport_cx = vpx + vpw * 0.5f;
			const float viewport_cy = vpy + vph * 0.5f;

			vgui_input_mouse_move(ui, arx + 30.0f, arow_y);
			vgui_input_mouse_down(ui, 1, arx + 30.0f, arow_y, 1);
			vgui_input_mouse_move(ui, viewport_cx, viewport_cy);
			if (!vgui_drag_active(ui))
				return fail("asset drag begins");
			vgui_input_mouse_up(ui, 1, viewport_cx, viewport_cy, 1);
			if (shell.viewport().last_drop().kind != drag_payload_kind::asset ||
				shell.viewport().last_drop().id != "Cube.mesh")
				return fail("asset drop reaches viewport");

			std::string assigned;
			shell.properties().set_asset_drop_handler(
				[&assigned](const std::string& asset) { assigned = asset; });
			vgui_input_mouse_move(ui, arx + 30.0f, arow_y);
			vgui_input_mouse_down(ui, 1, arx + 30.0f, arow_y, 1);
			vgui_input_mouse_move(ui, ppx + ppw * 0.5f, ppy + pph * 0.5f);
			if (!vgui_drag_active(ui))
				return fail("asset drag to properties begins");
			vgui_input_mouse_up(ui, 1, ppx + ppw * 0.5f, ppy + pph * 0.5f, 1);
			if (assigned != "Cube.mesh")
				return fail("asset drop reaches property grid");
		}

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
