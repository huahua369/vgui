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
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#include <crtdbg.h>

#include "vgui.h"
#include "vgui_editor_kit.h"

namespace {

/* 崩溃时打印符号化调用栈,便于定位无窗口测试的段错误。 */
LONG WINAPI crash_dump(EXCEPTION_POINTERS* info)
{
	SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
	SymInitialize(GetCurrentProcess(), nullptr, TRUE);
	void* frames[32] = {};
	const WORD count = CaptureStackBackTrace(0, 32, frames, nullptr);
	std::fprintf(stderr, "CRASH exception 0x%08lX at %p\n",
		info->ExceptionRecord->ExceptionCode,
		info->ExceptionRecord->ExceptionAddress);
	char symbol[sizeof(SYMBOL_INFO) + 256] = {};
	for (WORD i = 0; i < count; ++i)
	{
		DWORD64 offset = 0;
		reinterpret_cast<SYMBOL_INFO*>(symbol)->SizeOfStruct = sizeof(SYMBOL_INFO);
		reinterpret_cast<SYMBOL_INFO*>(symbol)->MaxNameLen = 255;
		if (SymFromAddr(GetCurrentProcess(), (DWORD64)frames[i], &offset,
			reinterpret_cast<SYMBOL_INFO*>(symbol)))
			std::fprintf(stderr, "  #%u %s+0x%llx\n", i,
				reinterpret_cast<SYMBOL_INFO*>(symbol)->Name,
				(unsigned long long)offset);
		else
			std::fprintf(stderr, "  #%u %p\n", i, frames[i]);
	}
	std::fflush(stderr);
	return EXCEPTION_EXECUTE_HANDLER;
}


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
				editor_context context(ui);		editor_shell shell(context);		shell.build();
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
			}	vgui_destroy(ui);
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
			/* P08 后 Edit 菜单:undo, redo, sep, copy, paste, sep, delete。 */
			vgui_item_id delete_item = vgui_menu_get_item_at(edit_menu, 6);
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
				/* O03 插入语义:行顶部 25% = 插到该行之前。行 2 是节点 4
				 * (parent=2,index=0),等价旧"换位"断言。 */
				const float drop_y = ty + pad_y + row_h * 2.0f + row_h * 0.1f;
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

//----------------------------------------------------------------------------
// M2 P05:color_picker 组件与颜色行 popup
//----------------------------------------------------------------------------

static vgui_widget* find_widget_by_suffix(vgui_widget* root, const char* suffix)
{
	if (!root)
		return nullptr;
	const char* id = vgui_widget_get_id(root);
	if (id && std::strlen(id) >= std::strlen(suffix) &&
		std::strcmp(id + std::strlen(id) - std::strlen(suffix), suffix) == 0)
		return root;
	for (size_t i = 0; i < vgui_widget_get_child_count(root); ++i)
	{
		vgui_widget* found = find_widget_by_suffix(
			vgui_widget_get_child(root, i), suffix);
		if (found)
			return found;
	}
	return nullptr;
}

static void click_widget_center(vgui_context* ui, vgui_widget* widget)
{
	if (!ui || !widget)
		return;
	float x = 0, y = 0, w = 0, h = 0;
	vgui_widget_get_rect(widget, &x, &y, &w, &h);
	const float cx = x + w * 0.5f;
	const float cy = y + h * 0.5f;
	vgui_input_mouse_move(ui, cx, cy);
	vgui_input_mouse_down(ui, 1, cx, cy, 1);
	vgui_input_mouse_up(ui, 1, cx, cy, 1);
}

static void double_click_at(vgui_context* ui, float x, float y)
{
	if (!ui)
		return;
	vgui_input_mouse_move(ui, x, y);
	vgui_input_mouse_down(ui, 1, x, y, 2);
	vgui_input_mouse_up(ui, 1, x, y, 2);
}

static int test_color_picker()
{
	using namespace vgui_editor_kit;
	vgui_context* ui = vgui_create(nullptr, 16.0f);
	if (!ui)
		return fail("color picker context");
	vgui_context_set_size(ui, 900, 700);
	vgui_style style;
	vgui_style_default(&style);
	vgui_context_set_style(ui, &style);

	/* ---- 组件级:值往返、alpha、策略转换 ---- */
	{
		color_picker picker;
		picker.build(ui);
		picker.set_value("#FF8040");
		if (picker.value() != "#FF8040" || !picker.valid())
			return fail("picker hex roundtrip");
		picker.set_value("#FF804080");
		if (picker.value() != "#FF804080")
			return fail("picker alpha roundtrip");
		/* 策略:value 域带 "lin:" 前缀模拟线性空间存储。 */
		picker.set_color_transform(
			[](const std::string& v) { return v.empty() ? v : v.substr(4); },
			[](const std::string& v) { return "lin:" + v; });
		picker.set_value("lin:#FF0000");
		if (picker.value() != "lin:#FF0000")
			return fail("picker policy value domain");
		/* SV 拖到右上角(纯色相满饱和满亮)→ 提交值带策略前缀。 */
		std::string committed;
		picker.set_on_commit([&committed](const std::string& hex) {
			committed = hex;
		});
		/* detached 组件经 popup 承载,几何由 popup 布局维护。 */
		if (!vgui_popup_open(ui, picker.widget(), 20.0f, 20.0f))
			return fail("picker popup opens (component)");
		vgui_update(ui, 0.016f);
		vgui_layout(ui);
		float px = 0, py = 0, pw = 0, ph = 0;
		vgui_widget_get_rect(picker.widget(), &px, &py, &pw, &ph);
		if (!near(px, 20.0f, 8.5f) || !near(py, 20.0f, 8.5f))
			return fail("picker popup positioned");
		(void)pw; (void)ph;
		vgui_widget* sv_widget = find_widget_by_suffix(picker.widget(), ".sv");
		if (!sv_widget)
			return fail("picker sv widget found");
		float svr_x = 0, svr_y = 0, svr_w = 0, svr_h = 0;
		vgui_widget_get_rect(sv_widget, &svr_x, &svr_y, &svr_w, &svr_h);
		const float sv_x = svr_x;
		const float sv_y = svr_y;
		vgui_input_mouse_move(ui, sv_x + svr_w - 4.0f, sv_y + 2.0f);
		vgui_input_mouse_down(ui, 1, sv_x + svr_w - 4.0f, sv_y + 2.0f, 1);
		vgui_input_mouse_up(ui, 1, sv_x + svr_w - 4.0f, sv_y + 2.0f, 1);
		if (committed.empty() || committed.rfind("lin:", 0) != 0)
			return fail("picker commit applies policy");
		/* 提交的 edit 域 hex 应接近纯色相(红):R 高,G/B 低。 */
		uint32_t argb = 0;
		const std::string edit_hex = committed.substr(4);
		const char* scan = edit_hex.c_str() + 1;
		auto digit = [](char c) -> int {
			if (c >= '0' && c <= '9') return c - '0';
			return std::toupper((unsigned char)c) - 'A' + 10;
		};
		const int rr = digit(scan[0]) * 16 + digit(scan[1]);
		const int gg = digit(scan[2]) * 16 + digit(scan[3]);
		const int bb = digit(scan[4]) * 16 + digit(scan[5]);
		if (rr < 200 || gg > 120 || bb > 120)
			return fail("picker sv drag reaches hue");
		vgui_popup_close(ui);
	}

	/* ---- popup 集成:预览/提交合并/Esc 回滚 ---- */
	{
		editor_context context(ui);
		command_stack commands;
		property_grid grid;
		grid.build(vgui_root(ui));
		grid.set_command_stack(&commands);
		vgui_widget_set_pos(grid.widget(), 10.0f, 10.0f);
		vgui_widget_set_size(grid.widget(), 260.0f, 300.0f);

		property_desc desc;
		desc.id = "light.color";
		desc.label = "Color";
		desc.type = property_type::color;
		desc.value = "#409EFF";
		if (!grid.add_property(desc))
			return fail("picker color row added");

		vgui_update(ui, 0.016f);
		vgui_layout(ui);

		/* 点色块打开 popup:递归找 "property:light.color" 的第二个命中
		 * (第一个是 label,第二个是行尾 swatch)。 */
		float sx = 0, sy = 0;
		{
			std::vector<vgui_widget*> matches;
			std::function<void(vgui_widget*)> collect = [&](vgui_widget* w)
			{
				const char* id = vgui_widget_get_id(w);
				if (id && std::strcmp(id, "property:light.color") == 0)
					matches.push_back(w);
				for (size_t i = 0; i < vgui_widget_get_child_count(w); ++i)
					collect(vgui_widget_get_child(w, i));
			};
			collect(grid.widget());
			if (matches.size() < 2)
				return fail("picker swatch widget found");
			float gx = 0, gy = 0, gw = 0, gh = 0;
			vgui_widget_get_rect(matches[1], &gx, &gy, &gw, &gh);
			sx = gx + gw * 0.5f;
			sy = gy + gh * 0.5f;
		}
		const std::size_t undo_base = commands.undo_count();
		vgui_input_mouse_move(ui, sx, sy);
		vgui_input_mouse_down(ui, 1, sx, sy, 1);
		vgui_input_mouse_up(ui, 1, sx, sy, 1);
		if (!vgui_popup_active(ui))
			return fail("picker popup opens from swatch");
		vgui_widget* content = vgui_popup_content(ui);
		if (!content)
			return fail("picker popup content");
		float px = 0, py = 0, pw = 0, ph = 0;
		vgui_widget_get_rect(content, &px, &py, &pw, &ph);
		if (py < sy || py > sy + 40.0f)
			return fail("picker popup below swatch");
		(void)pw; (void)ph; (void)px;

		/* SV 盒拖一次(不释放):预览更新,无命令。 */
		vgui_widget* sv_widget = find_widget_by_suffix(content, ".sv");
		if (!sv_widget)
			return fail("picker popup sv found");
		float svr_x = 0, svr_y = 0, svr_w = 0, svr_h = 0;
		vgui_widget_get_rect(sv_widget, &svr_x, &svr_y, &svr_w, &svr_h);
		const float sv_x = svr_x;
		const float sv_y = svr_y;
		vgui_input_mouse_move(ui, sv_x + 100.0f, sv_y + 64.0f);
		vgui_input_mouse_down(ui, 1, sv_x + 100.0f, sv_y + 64.0f, 1);
		if (grid.property_value("light.color") == "#409EFF")
			return fail("picker change previews");
		if (commands.undo_count() != undo_base)
			return fail("picker preview records no command");
		/* 移动再释放:一条命令(整个手势)。 */
		vgui_input_mouse_move(ui, sv_x + 150.0f, sv_y + 30.0f);
		vgui_input_mouse_up(ui, 1, sv_x + 150.0f, sv_y + 30.0f, 1);
		if (commands.undo_count() != undo_base + 1)
			return fail("picker gesture commits once");
		const std::string after_drag = grid.property_value("light.color");

		/* 第二个手势:merge 链保持一条 undo 记录。 */
		vgui_input_mouse_move(ui, sv_x + 60.0f, sv_y + 100.0f);
		vgui_input_mouse_down(ui, 1, sv_x + 60.0f, sv_y + 100.0f, 1);
		vgui_input_mouse_up(ui, 1, sv_x + 60.0f, sv_y + 100.0f, 1);
		if (commands.undo_count() != undo_base + 1)
			return fail("picker session merges commands");

		/* Esc:回滚到会话起点,popup 关闭;undo 一次恢复原值。 */
		vgui_input_key(ui, VGUI_KEY_ESCAPE, 1, 0);
		vgui_input_key(ui, VGUI_KEY_ESCAPE, 0, 0);
		if (vgui_popup_active(ui))
			return fail("picker escape closes popup");
		if (grid.property_value("light.color") != "#409EFF")
			return fail("picker escape reverts");
		if (commands.undo_count() != undo_base + 1)
			return fail("picker revert keeps one record");
		if (!commands.undo() ||
			grid.property_value("light.color") != "#409EFF")
			return fail("picker undo after revert");
		if (grid.property_value("light.color") != after_drag &&
			after_drag.empty())
			return fail("picker after_drag sanity");
		commands.redo();
		/* redo 后值=最后手势值,再撤销一次回到 #409EFF(链顶 old=起点)。 */
		if (!commands.undo() ||
			grid.property_value("light.color") != "#409EFF")
			return fail("picker merged undo reaches start");
	}

	vgui_destroy(ui);
	return 0;
}

/* P05 回归:SV/色相/透明条必须能拖动。vgui 0.3 的 MOUSE_MOVE button 为 0,
 * 旧实现按 button==1 过滤导致拖动完全无效;这里同时验证拖动归属，拖过相邻
 * 色条时不能误改另一个通道。 */
static int test_color_picker_drag()
{
	using namespace vgui_editor_kit;
	vgui_context* ui = vgui_create(nullptr, 16.0f);
	if (!ui)
		return fail("picker drag context");
	vgui_context_set_size(ui, 800, 600);
	{
		color_picker picker;
		picker.build(ui);
		picker.set_value("#FF8040");
		int changes = 0;
		int commits = 0;
		std::string last;
		picker.set_on_change([&](const std::string& hex) {
			++changes;
			last = hex;
		});
		picker.set_on_commit([&](const std::string& hex) {
			++commits;
			last = hex;
		});
		if (!vgui_popup_open(ui, picker.widget(), 20.0f, 20.0f))
			return fail("picker drag popup");
		vgui_update(ui, 1.0f / 60.0f);
		vgui_layout(ui);

		vgui_widget* sv = find_widget_by_suffix(picker.widget(), ".sv");
		vgui_widget* hue = find_widget_by_suffix(picker.widget(), ".hue");
		vgui_widget* alpha = find_widget_by_suffix(picker.widget(), ".alpha");
		if (!sv || !hue || !alpha)
			return fail("picker drag widgets");
		float sx = 0, sy = 0, sw = 0, sh = 0;
		float hx = 0, hy = 0, hw = 0, hh = 0;
		float ax = 0, ay = 0, aw = 0, ah = 0;
		vgui_widget_get_rect(sv, &sx, &sy, &sw, &sh);
		vgui_widget_get_rect(hue, &hx, &hy, &hw, &hh);
		vgui_widget_get_rect(alpha, &ax, &ay, &aw, &ah);
		(void)hh;
		(void)ah;

		auto hex_byte = [](const std::string& hex, int index) {
			auto digit = [](char c) {
				if (c >= '0' && c <= '9') return c - '0';
				return std::toupper((unsigned char)c) - 'A' + 10;
			};
			return digit(hex[(std::size_t)index]) * 16 +
				digit(hex[(std::size_t)index + 1]);
		};

		/* 1) SV 拖动:左下(黑)→右上(亮),过程中持续 change,抬起只提交一次。 */
		const int changes_before_sv = changes;
		vgui_input_mouse_move(ui, sx + 2.0f, sy + sh - 2.0f);
		vgui_input_mouse_down(ui, 1, sx + 2.0f, sy + sh - 2.0f, 1);
		vgui_input_mouse_move(ui, sx + sw - 2.0f, sy + 2.0f);
		if (changes <= changes_before_sv)
			return fail("picker sv drag updates");
		vgui_input_mouse_up(ui, 1, sx + sw - 2.0f, sy + 2.0f, 1);
		if (commits != 1 || last.size() < 7)
			return fail("picker sv drag commits once");
		if (hex_byte(last, 1) + hex_byte(last, 3) + hex_byte(last, 5) < 200)
			return fail("picker sv drag reaches bright color");

		/* 2) 透明条:右侧(a≈1)拖到中点(a≈0.5),提交值带 alpha。 */
		picker.set_value("#FF0000");
		const int commits_before_alpha = commits;
		vgui_input_mouse_move(ui, ax + aw - 2.0f, ay + 4.0f);
		vgui_input_mouse_down(ui, 1, ax + aw - 2.0f, ay + 4.0f, 1);
		vgui_input_mouse_move(ui, ax + aw * 0.5f, ay + 4.0f);
		vgui_input_mouse_up(ui, 1, ax + aw * 0.5f, ay + 4.0f, 1);
		if (commits != commits_before_alpha + 1 || last.size() != 9)
			return fail("picker alpha drag commits");
		{
			const int a = hex_byte(last, 7);
			if (a < 100 || a > 160)
				return fail("picker alpha drag value");
		}

		/* 3) 色相拖动;拖过 SV 盒时悬停也会收到 MOUSE_MOVE，不能改 s/v。 */
		picker.set_value("#FF0000");
		const int changes_before_hue = changes;
		vgui_input_mouse_move(ui, hx + 2.0f, hy + 4.0f);
		vgui_input_mouse_down(ui, 1, hx + 2.0f, hy + 4.0f, 1);
		vgui_input_mouse_move(ui, hx + hw * 0.5f, hy + 4.0f);
		if (changes <= changes_before_hue)
			return fail("picker hue drag updates");
		vgui_input_mouse_move(ui, sx + sw * 0.5f, sy + sh * 0.5f);
		/* 色相约 180°,s/v 仍为 1 → 亮青;若 SV 被误改则 G/B 会降到 ~128。 */
		if (hex_byte(last, 3) < 200 || hex_byte(last, 5) < 200 ||
			hex_byte(last, 1) > 40)
			return fail("picker drag does not cross channels");
		vgui_input_mouse_up(ui, 1, sx + sw * 0.5f, sy + sh * 0.5f, 1);
		if (commits != commits_before_alpha + 2)
			return fail("picker hue drag commits");

		/* 3b) 色相条拖到最右端(≈360°)时,RGB 回绕不能让色相位置跳回 0°。 */
		picker.set_value("#00FF00");
		if (picker.hue_degrees() < 100.0f || picker.hue_degrees() > 140.0f)
			return fail("picker hue starts green");
		const int commits_before_edge = commits;
		vgui_input_mouse_move(ui, hx + hw - 1.0f, hy + 4.0f);
		vgui_input_mouse_down(ui, 1, hx + hw - 1.0f, hy + 4.0f, 1);
		vgui_input_mouse_up(ui, 1, hx + hw - 1.0f, hy + 4.0f, 1);
		if (picker.hue_degrees() < 350.0f)
			return fail("picker hue keeps right-edge position");
		if (commits != commits_before_edge + 1)
			return fail("picker hue edge commits");
		/* 后续 SV 拖动仍使用右端色相,不回到 0°。 */
		vgui_input_mouse_move(ui, sx + 2.0f, sy + 2.0f);
		vgui_input_mouse_down(ui, 1, sx + 2.0f, sy + 2.0f, 1);
		vgui_input_mouse_move(ui, sx + sw * 0.5f, sy + sh * 0.5f);
		vgui_input_mouse_up(ui, 1, sx + sw * 0.5f, sy + sh * 0.5f, 1);
		if (picker.hue_degrees() < 350.0f)
			return fail("picker sv drag keeps edge hue");

		/* 4) CANCEL 结束拖动:之后移动不应继续改色或提交。 */
		vgui_input_mouse_move(ui, sx + 2.0f, sy + sh - 2.0f);
		vgui_input_mouse_down(ui, 1, sx + 2.0f, sy + sh - 2.0f, 1);
		const int changes_before_cancel = changes;
		const int commits_before_cancel = commits;
		vgui_input_cancel(ui);
		vgui_input_mouse_move(ui, sx + sw * 0.5f, sy + sh * 0.5f);
		vgui_input_mouse_up(ui, 1, sx + sw * 0.5f, sy + sh * 0.5f, 1);
		if (changes != changes_before_cancel ||
			commits != commits_before_cancel)
			return fail("picker cancel stops drag");

		vgui_popup_close(ui);
	}
	vgui_destroy(ui);
	return 0;
}

//----------------------------------------------------------------------------
// M2 P09:曲线 / 渐变编辑器
//----------------------------------------------------------------------------

static int test_curve_editor()
{
	using namespace vgui_editor_kit;
	vgui_context* ui = vgui_create(nullptr, 16.0f);
	if (!ui)
		return fail("curve editor context");
	vgui_context_set_size(ui, 900, 700);
	vgui_style style;
	vgui_style_default(&style);
	vgui_context_set_style(ui, &style);

	/* ---- 文本格式与规范化 ---- */
	curve_value parsed;
	std::string error;
	if (!curve_editor::parse_value("bezier; 0,0; 0.5, 1, 0, 0; 1,1",
		parsed, &error))
		return fail("curve parse aliases");
	if (parsed.interpolation != curve_interpolation::cubic ||
		parsed.keys.size() != 3)
		return fail("curve parse mode and keys");
	if (curve_editor::format_value(parsed) !=
		"cubic;0,0,0,0;0.5,1,0,0;1,1,0,0")
		return fail("curve format canonical");
	if (curve_editor::parse_value("linear;0,0", parsed, &error) ||
		error.empty())
		return fail("curve rejects one key");
	if (curve_editor::parse_value("cubic;0,0,0,0;0,1,0,0", parsed, &error))
		return fail("curve rejects duplicate times");
	if (!curve_editor::parse_value("linear;0,-2;2,3", parsed, &error))
		return fail("curve clamps out-of-range keys");
	if (!near_px(parsed.keys[1].t, 1.0f) ||
		!near_px(parsed.keys[1].value, 1.0f))
		return fail("curve clamp values");

	/* ---- 组件:键操作 / 模式 / 手势通知 ---- */
	curve_editor editor;
	editor.set_id("test.curve");
	editor.build(vgui_root(ui));
	if (!editor.widget() || !editor.graph_widget() || !editor.add_widget() ||
		!editor.remove_widget() || !editor.time_widget() ||
		!editor.value_widget() || !editor.in_tangent_widget() ||
		!editor.out_tangent_widget())
		return fail("curve editor widgets");
	vgui_widget_set_pos(editor.widget(), 20.0f, 20.0f);
	vgui_widget_set_size(editor.widget(), 380.0f, 340.0f);
	vgui_update(ui, 0.016f);
	vgui_layout(ui);

	int presses = 0;
	int changes = 0;
	int commits = 0;
	editor.set_on_press([&](const curve_value&) { ++presses; });
	editor.set_on_change([&](const curve_value&) { ++changes; });
	editor.set_on_commit([&](const curve_value&) { ++commits; });

	curve_value linear;
	linear.interpolation = curve_interpolation::linear;
	linear.keys = { { 0.0f, 0.0f, 0.0f, 0.0f },
		{ 1.0f, 1.0f, 0.0f, 0.0f } };
	if (!editor.set_value(linear) || presses != 0 || changes != 0 ||
		commits != 0)
		return fail("curve silent set_value");
	if (editor.value_text() != "linear;0,0,0,0;1,1,0,0")
		return fail("curve editor value text");

	/* 模式按钮:改 cubic 是一次可提交手势。 */
	vgui_widget* cubic_button = find_widget(editor.widget(),
		"test.curve.mode.cubic", VGUI_BUTTON);
	if (!cubic_button)
		return fail("curve cubic mode button");
	click_widget_center(ui, cubic_button);
	if (editor.interpolation() != curve_interpolation::cubic ||
		presses != 1 || changes != 1 || commits != 1)
		return fail("curve mode click commits");

	/* 双击图内空白处添加关键帧。 */
	float gx = 0, gy = 0, gw = 0, gh = 0;
	vgui_widget_get_rect(editor.graph_widget(), &gx, &gy, &gw, &gh);
	const float margin = 10.0f;
	const float area_w = gw - margin * 2.0f;
	const float area_h = gh - margin * 2.0f;
	const float center_x = gx + margin + area_w * 0.5f;
	const float center_y = gy + margin + area_h * 0.5f;
	double_click_at(ui, center_x, center_y);
	if (editor.value().keys.size() != 3 ||
		editor.selected_key() != 1 ||
		presses != 2 || changes != 2 || commits != 2)
		return fail("curve double-click adds key");
	if (!near_px(editor.value().keys[1].t, 0.5f) ||
		!near_px(editor.value().keys[1].value, 0.5f))
		return fail("curve new key position");

	/* 拖拽选中的关键帧:预览走 change,释放只提交一次。 */
	const float key_x = gx + margin + editor.value().keys[1].t * area_w;
	const float key_y = gy + margin +
		(1.0f - editor.value().keys[1].value) * area_h;
	vgui_input_mouse_move(ui, key_x, key_y);
	vgui_input_mouse_down(ui, 1, key_x, key_y, 1);
	vgui_input_mouse_move(ui, key_x + area_w * 0.15f,
		key_y - area_h * 0.2f);
	if (changes != 3 || commits != 2)
		return fail("curve key drag previews without commit");
	if (!near_px(editor.value().keys[1].t, 0.65f) ||
		!near_px(editor.value().keys[1].value, 0.7f))
		return fail("curve key drag updates value");
	vgui_input_mouse_up(ui, 1, key_x + area_w * 0.15f,
		key_y - area_h * 0.2f, 1);
	if (commits != 3)
		return fail("curve key drag commits once");

	/* 拖拽 cubic 出切线手柄。 */
	const curve_key key = editor.value().keys[1];
	const curve_key next = editor.value().keys[2];
	const float dt = next.t - key.t;
	const float tangent_x = key_x + area_w * 0.15f + dt * area_w / 3.0f;
	const float tangent_y = key_y - area_h * 0.2f;
	vgui_input_mouse_move(ui, tangent_x, tangent_y);
	vgui_input_mouse_down(ui, 1, tangent_x, tangent_y, 1);
	vgui_input_mouse_move(ui, tangent_x, tangent_y - 24.0f);
	if (editor.value().keys[1].out_tangent <= 0.2f)
		return fail("curve tangent drag updates slope");
	vgui_input_mouse_up(ui, 1, tangent_x, tangent_y - 24.0f, 1);
	if (changes != 4 || commits != 4)
		return fail("curve tangent drag commits");
	if (!near_px(editor.value().keys[1].in_tangent, 0.0f))
		return fail("curve tangent drag keeps incoming slope");

	/* 删除与只读保护。 */
	if (!editor.remove_selected_key() || editor.value().keys.size() != 2)
		return fail("curve removes selected key");
	editor.set_read_only(true);
	if (editor.add_key(0.8f, 0.5f) || editor.value().keys.size() != 2)
		return fail("curve read-only blocks changes");
	editor.set_read_only(false);
	if (!editor.select_key(0))
		return fail("curve select key");

	vgui_destroy(ui);
	return 0;
}

static int test_gradient_editor()
{
	using namespace vgui_editor_kit;
	vgui_context* ui = vgui_create(nullptr, 16.0f);
	if (!ui)
		return fail("gradient editor context");
	vgui_context_set_size(ui, 900, 700);
	vgui_style style;
	vgui_style_default(&style);
	vgui_context_set_style(ui, &style);

	/* ---- 文本格式:短写 / alpha / 0x 前缀归一 ---- */
	gradient_value parsed;
	std::string error;
	if (!gradient_editor::parse_value(
		" 0, #f00 ; .5, #00FF0080 ; 1, 0x0000FF ", parsed, &error))
		return fail("gradient parse aliases");
	if (gradient_editor::format_value(parsed) !=
		"0,#FF0000;0.5,#00FF0080;1,#0000FF")
		return fail("gradient format canonical");
	if (gradient_editor::parse_value("0,#FF0000", parsed, &error) ||
		error.empty())
		return fail("gradient rejects one stop");
	if (gradient_editor::parse_value(
		"0,#FF0000;0.0,#00FF00;1,#0000FF", parsed, &error))
		return fail("gradient rejects duplicate times");
	if (gradient_editor::parse_value("0,red;1,#0000FF", parsed, &error))
		return fail("gradient rejects bad color");

	/* ---- 组件:停靠点 / 取色 / 预设 / 通知 ---- */
	gradient_editor editor;
	editor.set_id("test.gradient");
	editor.build(vgui_root(ui));
	if (!editor.widget() || !editor.bar_widget() || !editor.add_widget() ||
		!editor.remove_widget() || !editor.time_widget() ||
		!editor.picker() || !editor.preset_widget(0))
		return fail("gradient editor widgets");
	vgui_widget_set_pos(editor.widget(), 20.0f, 20.0f);
	vgui_widget_set_size(editor.widget(), 380.0f, 360.0f);
	vgui_update(ui, 0.016f);
	vgui_layout(ui);

	int presses = 0;
	int changes = 0;
	int commits = 0;
	editor.set_on_press([&](const gradient_value&) { ++presses; });
	editor.set_on_change([&](const gradient_value&) { ++changes; });
	editor.set_on_commit([&](const gradient_value&) { ++commits; });

	gradient_value mono;
	mono.stops = { { 0.0f, "#000000" }, { 1.0f, "#FFFFFF" } };
	if (!editor.set_value(mono) || presses != 0 || changes != 0 ||
		commits != 0)
		return fail("gradient silent set_value");

	/* 中间停靠点采样灰度 50%。 */
	if (!editor.add_stop(0.5f) || editor.value().stops.size() != 3 ||
		editor.selected_stop() != 1 ||
		editor.value().stops[1].color != "#808080")
		return fail("gradient add stop samples color");
	if (presses != 1 || changes != 1 || commits != 1)
		return fail("gradient add stop commits");

	if (!editor.set_selected_color("#FF0000") ||
		editor.value().stops[1].color != "#FF0000" ||
		presses != 2 || changes != 2 || commits != 2)
		return fail("gradient selected color commit");

	if (!editor.commit_preset(1) ||
		editor.value().stops.size() != 4 ||
		editor.value().stops[0].color != "#000000" ||
		editor.value().stops[3].color != "#FFFF66")
		return fail("gradient preset commit");

	editor.set_read_only(true);
	if (editor.add_stop(0.25f) || editor.commit_preset(0))
		return fail("gradient read-only blocks changes");
	editor.set_read_only(false);

	/* 双击渐变条插入停靠点,再拖动新点。 */
	if (!editor.set_value(mono))
		return fail("gradient reset value");
	float bx = 0, by = 0, bw = 0, bh = 0;
	vgui_widget_get_rect(editor.bar_widget(), &bx, &by, &bw, &bh);
	(void)bh;
	double_click_at(ui, bx + bw * 0.5f, by + 10.0f);
	if (editor.value().stops.size() != 3 ||
		!near_px(editor.value().stops[1].t, 0.5f))
		return fail("gradient bar double-click adds stop");

	const float stop_x = bx + bw * 0.5f;
	vgui_input_mouse_move(ui, stop_x, by + 10.0f);
	vgui_input_mouse_down(ui, 1, stop_x, by + 10.0f, 1);
	vgui_input_mouse_move(ui, bx + 6.0f + (bw - 12.0f) * 0.75f,
		by + 10.0f);
	if (!near_px(editor.value().stops[1].t, 0.75f))
		return fail("gradient bar drag moves stop");
	vgui_input_mouse_up(ui, 1, bx + 6.0f + (bw - 12.0f) * 0.75f,
		by + 10.0f, 1);
	if (editor.value().stops[1].t > 0.9f)
		return fail("gradient bar drag keeps order");

	vgui_destroy(ui);
	return 0;
}

static int test_property_grid_curve_gradient()
{
	using namespace vgui_editor_kit;
	vgui_context* ui = vgui_create(nullptr, 16.0f);
	if (!ui)
		return fail("curve/gradient grid context");
	vgui_context_set_size(ui, 900, 700);
	vgui_style style;
	vgui_style_default(&style);
	vgui_context_set_style(ui, &style);

	/* ---- 单目标:预览、popup、实时预览、合并 undo、Esc 回滚 ---- */
	{
		editor_context context(ui);
		command_stack commands;
		property_grid grid;
		grid.build(vgui_root(ui));
		grid.set_command_stack(&commands);
		vgui_widget_set_pos(grid.widget(), 10.0f, 10.0f);
		vgui_widget_set_size(grid.widget(), 300.0f, 360.0f);

		property_desc curve_desc;
		curve_desc.id = "anim.curve";
		curve_desc.label = "Curve";
		curve_desc.type = property_type::curve;
		curve_desc.value = "linear;0,0;1,1";
		property_desc gradient_desc;
		gradient_desc.id = "fx.gradient";
		gradient_desc.label = "Gradient";
		gradient_desc.type = property_type::gradient;
		gradient_desc.value = "0,#FF0000;1,#0000FF";
		if (!grid.add_property(curve_desc) ||
			!grid.add_property(gradient_desc))
			return fail("curve/gradient rows added");

		const std::string curve_start = grid.property_value("anim.curve");
		const std::string gradient_start =
			grid.property_value("fx.gradient");
		if (curve_start != "linear;0,0,0,0;1,1,0,0" ||
			gradient_start != "0,#FF0000;1,#0000FF")
			return fail("curve/gradient rows normalized");

		vgui_update(ui, 0.016f);
		vgui_layout(ui);

		vgui_widget* curve_preview = find_widget(grid.widget(),
			"property:anim.curve", VGUI_CUSTOM);
		vgui_widget* gradient_preview = find_widget(grid.widget(),
			"property:fx.gradient", VGUI_CUSTOM);
		if (!curve_preview || !gradient_preview)
			return fail("curve/gradient preview widgets");

		/* 曲线行点击打开 popup 并保持值不变。 */
		const std::size_t curve_base = commands.undo_count();
		click_widget_center(ui, curve_preview);
		if (!vgui_popup_active(ui))
			return fail("curve popup opens from preview");
		vgui_widget* content = vgui_popup_content(ui);
		if (!content || std::string(vgui_widget_get_id(content)) !=
			"property-grid.curve-editor")
			return fail("curve popup content");
		if (grid.property_value("anim.curve") != curve_start)
			return fail("curve popup open keeps value");
		float curve_px = 0, curve_py = 0, curve_pw = 0, curve_ph = 0;
		vgui_widget_get_rect(curve_preview, nullptr, nullptr,
			&curve_pw, &curve_ph);
		vgui_widget_get_rect(content, &curve_px, &curve_py, nullptr,
			nullptr);
		if (curve_py < 10.0f)
			return fail("curve popup positioned");
		(void)curve_py;
		(void)curve_pw;
		(void)curve_ph;
		(void)curve_px;

		/* 双击图中点:实时预览 + 提交一条命令。 */
		vgui_widget* graph = find_widget(content,
			"property-grid.curve-editor.graph", VGUI_CUSTOM);
		if (!graph)
			return fail("curve popup graph");
		float gx = 0, gy = 0, gw = 0, gh = 0;
		vgui_widget_get_rect(graph, &gx, &gy, &gw, &gh);
		const float margin = 10.0f;
		double_click_at(ui, gx + margin + (gw - margin * 2.0f) * 0.5f,
			gy + margin + (gh - margin * 2.0f) * 0.5f);
		curve_value after_add;
		if (!curve_editor::parse_value(grid.property_value("anim.curve"),
			after_add, nullptr) || after_add.keys.size() != 3)
			return fail("curve popup previews new key");
		if (!near_px(after_add.keys[1].t, 0.5f) ||
			!near_px(after_add.keys[1].value, 0.5f))
			return fail("curve popup key position");
		if (commands.undo_count() != curve_base + 1)
			return fail("curve popup records one command");

		/* 切换 interpolation:第二个手势合并进同一 undo 记录。 */
		vgui_widget* cubic = find_widget(content,
			"property-grid.curve-editor.mode.cubic", VGUI_BUTTON);
		if (!cubic)
			return fail("curve popup cubic button");
		click_widget_center(ui, cubic);
		curve_value after_mode;
		if (!curve_editor::parse_value(grid.property_value("anim.curve"),
			after_mode, nullptr) ||
			after_mode.interpolation != curve_interpolation::cubic)
			return fail("curve popup mode preview");
		if (commands.undo_count() != curve_base + 1)
			return fail("curve popup merges gestures");

		/* Esc:显示回滚到会话起点,popup 关闭。 */
		vgui_input_key(ui, VGUI_KEY_ESCAPE, 1, 0);
		vgui_input_key(ui, VGUI_KEY_ESCAPE, 0, 0);
		if (vgui_popup_active(ui))
			return fail("curve escape closes popup");
		if (grid.property_value("anim.curve") != curve_start)
			return fail("curve escape reverts display");
		if (commands.undo_count() != curve_base + 1)
			return fail("curve escape keeps one record");
		if (!commands.undo() ||
			grid.property_value("anim.curve") != curve_start)
			return fail("curve undo after revert");
		if (!commands.redo())
			return fail("curve redo after revert");
		curve_value redone;
		if (!curve_editor::parse_value(grid.property_value("anim.curve"),
			redone, nullptr) || redone.keys.size() != 3 ||
			redone.interpolation != curve_interpolation::cubic)
			return fail("curve redo restores merged value");

		/* 渐变行:双击条新增停靠点,预设合并,显式关闭保留已提交值。 */
		const std::size_t gradient_base = commands.undo_count();
		click_widget_center(ui, gradient_preview);
		if (!vgui_popup_active(ui))
			return fail("gradient popup opens from preview");
		content = vgui_popup_content(ui);
		if (!content || std::string(vgui_widget_get_id(content)) !=
			"property-grid.gradient-editor")
			return fail("gradient popup content");
		vgui_widget* bar = find_widget(content,
			"property-grid.gradient-editor.bar", VGUI_CUSTOM);
		if (!bar)
			return fail("gradient popup bar");
		float bx = 0, by = 0, bw = 0, bh = 0;
		vgui_widget_get_rect(bar, &bx, &by, &bw, &bh);
		(void)bh;
		double_click_at(ui, bx + bw * 0.5f, by + 10.0f);
		gradient_value after_stop;
		if (!gradient_editor::parse_value(
			grid.property_value("fx.gradient"), after_stop, nullptr) ||
			after_stop.stops.size() != 3 ||
			!near_px(after_stop.stops[1].t, 0.5f))
			return fail("gradient popup previews new stop");
		if (commands.undo_count() != gradient_base + 1)
			return fail("gradient popup records one command");

		vgui_widget* fire = find_widget(content,
			"property-grid.gradient-editor.preset.1", VGUI_BUTTON);
		if (!fire)
			return fail("gradient popup preset button");
		click_widget_center(ui, fire);
		gradient_value after_preset;
		if (!gradient_editor::parse_value(
			grid.property_value("fx.gradient"), after_preset, nullptr) ||
			after_preset.stops.size() != 4 ||
			after_preset.stops[0].color != "#000000" ||
			after_preset.stops[3].color != "#FFFF66")
			return fail("gradient popup preset preview");
		if (commands.undo_count() != gradient_base + 1)
			return fail("gradient popup merges preset");

		vgui_popup_close(ui);
		if (vgui_popup_active(ui))
			return fail("gradient explicit close");
		gradient_value kept;
		if (!gradient_editor::parse_value(
			grid.property_value("fx.gradient"), kept, nullptr) ||
			kept.stops.size() != 4)
			return fail("gradient close keeps committed value");
		if (!commands.undo() ||
			grid.property_value("fx.gradient") != gradient_start)
			return fail("gradient undo reaches session start");
		if (!commands.redo())
			return fail("gradient redo");
		gradient_value redone_gradient;
		if (!gradient_editor::parse_value(
			grid.property_value("fx.gradient"), redone_gradient, nullptr) ||
			redone_gradient.stops.size() != 4)
			return fail("gradient redo restores preset");
	}

	/* ---- 多目标:弹窗编辑写入所有 target,一条事务 undo ---- */
	{
		editor_context context(ui);
		command_stack commands;
		property_grid grid;
		grid.build(vgui_root(ui));
		grid.set_command_stack(&commands);
		vgui_widget_set_pos(grid.widget(), 10.0f, 10.0f);
		vgui_widget_set_size(grid.widget(), 300.0f, 300.0f);

		const std::string start = "linear;0,0,0,0;1,1,0,0";
		std::map<object_id, std::map<std::string, std::string>> store = {
			{ 1, { { "fx.curve", start } } },
			{ 2, { { "fx.curve", start } } }
		};
		grid.set_multi_getter([&](const std::string& id, object_id target) {
			return store.count(target) && store[target].count(id)
				? store[target][id] : std::string();
		});
		grid.set_multi_setter([&](const std::string& id, object_id target,
			const std::string& value)
		{
			store[target][id] = value;
		});

		property_desc desc;
		desc.id = "fx.curve";
		desc.label = "Curve";
		desc.type = property_type::curve;
		desc.value = start;
		if (!grid.add_property(desc))
			return fail("multi curve row added");
		grid.set_multi_targets({ 1, 2 });
		vgui_update(ui, 0.016f);
		vgui_layout(ui);
		if (grid.property_mixed("fx.curve"))
			return fail("multi curve same value not mixed");

		vgui_widget* preview = find_widget(grid.widget(),
			"property:fx.curve", VGUI_CUSTOM);
		if (!preview)
			return fail("multi curve preview");
		const std::size_t base = commands.undo_count();
		click_widget_center(ui, preview);
		if (!vgui_popup_active(ui))
			return fail("multi curve popup opens");
		vgui_widget* content = vgui_popup_content(ui);
		vgui_widget* graph = content ? find_widget(content,
			"property-grid.curve-editor.graph", VGUI_CUSTOM) : nullptr;
		if (!graph)
			return fail("multi curve graph");
		float gx = 0, gy = 0, gw = 0, gh = 0;
		vgui_widget_get_rect(graph, &gx, &gy, &gw, &gh);
		const float margin = 10.0f;
		double_click_at(ui, gx + margin + (gw - margin * 2.0f) * 0.5f,
			gy + margin + (gh - margin * 2.0f) * 0.5f);
		if (store[1]["fx.curve"] != store[2]["fx.curve"] ||
			store[1]["fx.curve"] == start)
			return fail("multi curve commit writes all targets");
		curve_value multi_value;
		if (!curve_editor::parse_value(store[1]["fx.curve"],
			multi_value, nullptr) || multi_value.keys.size() != 3)
			return fail("multi curve committed value");
		if (commands.undo_count() != base + 1)
			return fail("multi curve single transaction");
		if (!commands.undo() ||
			store[1]["fx.curve"] != start || store[2]["fx.curve"] != start)
			return fail("multi curve undo restores targets");
		if (!commands.redo() ||
			store[1]["fx.curve"] == start || store[2]["fx.curve"] == start)
			return fail("multi curve redo");
		vgui_popup_close(ui);
	}

	vgui_destroy(ui);
	return 0;
}

//----------------------------------------------------------------------------
// M2 P08:多选混合值 / 属性剪贴板 / 收藏
//----------------------------------------------------------------------------

static int test_property_multi()
{
	using namespace vgui_editor_kit;
	vgui_context* ui = vgui_create(nullptr, 16.0f);
	if (!ui)
		return fail("multi context");
	vgui_context_set_size(ui, 900, 700);
	vgui_style style;
	vgui_style_default(&style);
	vgui_context_set_style(ui, &style);

	/* kit 契约:grid 先于 vgui_context 析构。 */
	{
	editor_context context(ui);
	command_stack commands;
	property_grid grid;
	grid.build(vgui_root(ui));
	grid.set_command_stack(&commands);

	/* 宿主对象存储:两对象同名属性不同值。 */
	std::map<object_id, std::map<std::string, std::string>> store = {
		{ 1, { { "hp", "10" }, { "name", "A" }, { "enabled", "true" } } },
		{ 2, { { "hp", "20" }, { "name", "A" }, { "enabled", "false" } } },
	};
	grid.set_multi_getter([&](const std::string& id, object_id target) {
		return store.count(target) && store[target].count(id)
			? store[target][id] : std::string();
	});
	grid.set_multi_setter([&](const std::string& id, object_id target,
		const std::string& value)
	{
		store[target][id] = value;
	});

	property_desc hp;
	hp.id = "hp";
	hp.label = "HP";
	hp.type = property_type::integer;
	hp.value = "10";
	property_desc name;
	name.id = "name";
	name.label = "Name";
	name.type = property_type::text;
	name.value = "A";
	if (!grid.add_property(hp) || !grid.add_property(name))
		return fail("multi rows added");

	/* 同值 → 不混合;不同值 → 混合。 */
	grid.set_multi_targets({ 1, 2 });
	if (grid.multi_targets().size() != 2)
		return fail("multi targets stored");
	if (!grid.property_mixed("hp") || grid.property_mixed("name"))
		return fail("mixed detection");
	if (grid.property_value("name") != "A")
		return fail("same value displayed");

	/* 批量提交:两对象同写,一条 undo;undo 回各自旧值并回到混合。 */
	const std::size_t base = commands.undo_count();
	if (!grid.commit_property_value("hp", "15"))
		return fail("multi commit");
	if (store[1]["hp"] != "15" || store[2]["hp"] != "15")
		return fail("multi commit writes all targets");
	if (commands.undo_count() != base + 1)
		return fail("multi single undo record");
	if (grid.property_mixed("hp"))
		return fail("mixed cleared after commit");
	if (!commands.undo())
		return fail("multi undo");
	if (store[1]["hp"] != "10" || store[2]["hp"] != "20")
		return fail("multi undo restores per-target values");
	if (!grid.property_mixed("hp"))
		return fail("mixed restored after undo");
	if (!commands.redo() || store[1]["hp"] != "15" ||
		store[2]["hp"] != "15" || grid.property_mixed("hp"))
		return fail("multi redo");

	/* 空目标列表退回单选路径(desc.set_value 被调)。 */
	bool single_path_used = false;
	{
		property_desc single;
		single.id = "hp";
		single.label = "HP";
		single.type = property_type::integer;
		single.value = "15";
		single.set_value = [&](const std::string&) { single_path_used = true; };
		property_grid single_grid;
		single_grid.build(vgui_root(ui));
		single_grid.set_command_stack(&commands);
		single_grid.add_property(single);
		single_grid.set_multi_targets({ 1, 2 });
		single_grid.set_multi_targets({});
		if (!single_grid.commit_property_value("hp", "30"))
			return fail("single commit after clearing targets");
		if (!single_path_used)
			return fail("single path desc.set_value used");
	}
	}	/* kit 契约块:grid 先析构 */

	vgui_destroy(ui);
	return 0;
}

static int test_property_clipboard()
{
	using namespace vgui_editor_kit;
	vgui_context* ui = vgui_create(nullptr, 16.0f);
	if (!ui)
		return fail("clipboard context");
	vgui_context_set_size(ui, 900, 700);
	vgui_style style;
	vgui_style_default(&style);
	vgui_context_set_style(ui, &style);

	editor_context context(ui);
	command_stack commands;
	{
		property_grid source;
		source.build(vgui_root(ui));
		property_desc hp;
		hp.id = "hp";
		hp.type = property_type::integer;
		hp.value = "10";
		property_desc tint;
		tint.id = "tint";
		tint.type = property_type::color;
		tint.value = "#FF0000";
		property_desc mesh;
		mesh.id = "mesh";
		mesh.type = property_type::asset;
		mesh.value = "Cube.mesh";
		property_desc extra;
		extra.id = "extra";
		extra.type = property_type::text;
		extra.value = "keep";
		if (!source.add_property(hp) || !source.add_property(tint) ||
			!source.add_property(mesh) || !source.add_property(extra))
			return fail("clipboard source rows");
		if (source.copy_properties() != 4 ||
			property_grid::clipboard_property_count() != 4)
			return fail("clipboard copy count");
	}

	/* 目标 grid:同 id 同型 3 行 + 同 id 异型 1 行(text→integer)+ 无关行。 */
	{
		property_grid target;
		target.build(vgui_root(ui));
		target.set_command_stack(&commands);
		property_desc hp;
		hp.id = "hp";
		hp.type = property_type::integer;
		hp.value = "30";
		property_desc tint;
		tint.id = "tint";
		tint.type = property_type::color;
		tint.value = "#00FF00";
		property_desc mesh;
		mesh.id = "mesh";
		mesh.type = property_type::asset;
		mesh.value = "";
		property_desc extra;
		extra.id = "extra";
		extra.type = property_type::integer;
		extra.value = "7";
		property_desc note;
		note.id = "note";
		note.type = property_type::text;
		note.value = "untouched";
		if (!target.add_property(hp) || !target.add_property(tint) ||
			!target.add_property(mesh) || !target.add_property(extra) ||
			!target.add_property(note))
			return fail("clipboard target rows");

		const std::size_t base = commands.undo_count();
		if (target.paste_properties() != 3)
			return fail("clipboard pastes matching typed rows");
		if (target.property_value("hp") != "10" ||
			target.property_value("tint") != "#FF0000" ||
			target.property_value("mesh") != "Cube.mesh")
			return fail("clipboard values applied");
		/* 类型不匹配与缺行跳过。 */
		if (target.property_value("extra") != "7" ||
			target.property_value("note") != "untouched")
			return fail("clipboard skips mismatched rows");
		if (commands.undo_count() != base + 1)
			return fail("clipboard single undo record");
		if (!commands.undo() || target.property_value("hp") != "30" ||
			target.property_value("mesh") != "")
			return fail("clipboard undo restores");
	}

	vgui_destroy(ui);
	return 0;
}

static int test_property_favorites()
{
	using namespace vgui_editor_kit;
	vgui_context* ui = vgui_create(nullptr, 16.0f);
	if (!ui)
		return fail("favorites context");
	vgui_context_set_size(ui, 900, 700);
	vgui_style style;
	vgui_style_default(&style);
	vgui_context_set_style(ui, &style);

	/* kit 契约:grid 先于 vgui_context 析构。 */
	{
	editor_context context(ui);
	property_grid grid;
	grid.build(vgui_root(ui));

	auto add = [&](const char* id) {
		property_desc desc;
		desc.id = id;
		desc.label = id;
		desc.type = property_type::text;
		desc.value = id;
		return grid.add_property(desc);
	};
	if (!add("alpha") || !add("beta") || !add("gamma"))
		return fail("favorites rows");
	vgui_update(ui, 0.016f);

	grid.set_property_favorite("alpha", true);
	if (!grid.property_favorite("alpha") || grid.property_favorite("beta"))
		return fail("favorite toggle");
	{
		const std::vector<std::string> ids = grid.favorite_ids();
		if (ids.size() != 1 || ids[0] != "alpha")
			return fail("favorite ids export");
	}

	/* 收藏过滤与可见行数。 */
	grid.set_favorites_only(true);
	if (grid.visible_row_count() != 1)
		return fail("favorites only filters rows");

	/* 与搜索叠加:beta 不收藏 → 0 行。 */
	grid.set_filter("beta");
	if (grid.visible_row_count() != 0)
		return fail("favorites plus search filters");
	grid.set_filter("");
	if (grid.visible_row_count() != 1)
		return fail("search clear restores favorites view");
	grid.set_favorites_only(false);
	if (grid.visible_row_count() != 3)
		return fail("favorites off shows all");

	/* 重建(clear + add)后收藏保持。 */
	grid.clear();
	if (!add("alpha") || !add("beta"))
		return fail("favorites rebuild rows");
	if (!grid.property_favorite("alpha"))
		return fail("favorite survives rebuild");
	grid.set_favorites_only(true);
	if (grid.visible_row_count() != 1)
		return fail("favorite filter after rebuild");

	/* 导出到另一个 grid。 */
	property_grid other;
	other.build(vgui_root(ui));
	property_desc desc;
	desc.id = "alpha";
	desc.type = property_type::text;
	other.add_property(desc);
	for (const std::string& id : grid.favorite_ids())
		other.set_property_favorite(id, true);
	if (!other.property_favorite("alpha"))
		return fail("favorite import to other grid");
	}	/* kit 契约块:grid 先析构 */

	vgui_destroy(ui);
	return 0;
}

//----------------------------------------------------------------------------
// M2 P06:asset / object 引用行
//----------------------------------------------------------------------------

static int test_property_reference()
{
	using namespace vgui_editor_kit;
	vgui_context* ui = vgui_create(nullptr, 16.0f);
	if (!ui)
		return fail("reference context");
	vgui_context_set_size(ui, 900, 700);
	vgui_style style;
	vgui_style_default(&style);
	vgui_context_set_style(ui, &style);

	/* grid/context 必须先于 vgui_context 析构(kit 契约),块作用域保证。 */
	/* grid/context 必须先于 vgui_context 析构(kit 契约),块作用域保证。 */
	{
	editor_context context(ui);
	command_stack commands;
	property_grid grid;
	grid.build(vgui_root(ui));
	grid.set_command_stack(&commands);
	vgui_widget_set_pos(grid.widget(), 10.0f, 10.0f);
	vgui_widget_set_size(grid.widget(), 280.0f, 320.0f);

	property_desc mesh;
	mesh.id = "render.mesh";
	mesh.label = "Mesh";
	mesh.type = property_type::asset;
	mesh.value = "Cube.mesh";
	mesh.asset_filter = "mesh";
	if (!grid.add_property(mesh))
		return fail("reference mesh row");

	property_desc target;
	target.id = "ai.target";
	target.label = "Target";
	target.type = property_type::object_ref;
	target.value = "";
	if (!grid.add_property(target))
		return fail("reference object row");

	int missing_calls = 0;
	std::string missing_id;
	grid.set_missing_asset_handler([&](const std::string& id, const std::string&) {
		++missing_calls;
		missing_id = id;
	});
	grid.set_asset_query([&](const std::string& id, const std::string& filter,
		property_grid::asset_info* out)
	{
		if (filter == "mesh" && id == "Cube.mesh")
		{
			out->name = "Cube";
			out->kind = "mesh";
			return true;
		}
		if (filter == "mesh" && id == "Ghost.mesh")
			return true;	/* 宿主也解析不到 → missing */
		return false;
	});

	vgui_update(ui, 0.016f);
	vgui_layout(ui);

	/* 解析:mesh 行显示宿主名;object 行空值显示 (None)。 */
	{
		vgui_widget* mesh_name = find_widget_by_suffix(grid.widget(),
			"property:render.mesh");
		/* find 命中的是同 id 的第一个(label);改按文本断言:直接查
		 * editor 的显示需要 editor 指针,这里用 grid 级断言替代。 */
		(void)mesh_name;
		if (grid.property_value("render.mesh") != "Cube.mesh")
			return fail("reference value preserved");
	}

	/* 显示名通过 refresh 查询 —— 用一次 missing 查询验证通路:
	 * 把 mesh 值改成 Ghost.mesh(宿主查询返回 true 但 name 为空 →
	 * 显示原始 id;不触发 missing)。 */
	if (!grid.commit_property_value("render.mesh", "Ghost.mesh"))
		return fail("reference commit ghost");
	if (grid.property_value("render.mesh") != "Ghost.mesh")
		return fail("reference ghost value");
	if (missing_calls != 0)
		return fail("reference resolvable not missing");
	/* 改成宿主解析失败的 id → missing 态 + handler 触发。 */
	if (!grid.commit_property_value("render.mesh", "Unknown.mesh"))
		return fail("reference commit unknown");
	if (missing_calls != 1 || missing_id != "render.mesh")
		return fail("reference missing handler fires");
	if (!grid.commit_property_value("render.mesh", "Cube.mesh"))
		return fail("reference commit cube");
	if (missing_calls != 1)
		return fail("reference missing clears without refire");


	/* 行级 drop:asset 载荷拖到 mesh 行 thumb。 */
	vgui_widget* thumb = nullptr;
	{
		std::vector<vgui_widget*> matches;
		std::function<void(vgui_widget*)> collect = [&](vgui_widget* w)
		{
			const char* wid = vgui_widget_get_id(w);
			if (wid && std::strcmp(wid, "property:render.mesh") == 0 &&
				vgui_widget_get_kind(w) == VGUI_PANEL)
				matches.push_back(w);
			for (size_t i = 0; i < vgui_widget_get_child_count(w); ++i)
				collect(vgui_widget_get_child(w, i));
		};
		collect(grid.widget());
		if (matches.size() != 1)
			return fail("reference thumb found");
		thumb = matches[0];
	}
	float gx = 0, gy = 0, gw = 0, gh = 0;
	vgui_widget_get_rect(thumb, &gx, &gy, &gw, &gh);
	const std::size_t undo_base = commands.undo_count();
	{
		vgui_drag_payload payload = {};
		payload.kind = VGUI_DRAG_CUSTOM;
		payload.text = "Sphere.mesh";
		payload.data = reinterpret_cast<const void*>(
			static_cast<std::uintptr_t>(drag_payload_kind::asset));
		if (!vgui_begin_drag(ui, grid.widget(), &payload))
			return fail("reference drag begins");
		vgui_input_mouse_move(ui, gx + gw * 0.5f, gy + gh * 0.5f);
		vgui_input_mouse_up(ui, 1, gx + gw * 0.5f, gy + gh * 0.5f, 1);
		/* 同属性连续提交被 merge 链吸收(此前 3 次 commit 已合成一条),
		 * 撤销数不变;用 undo/redo 往返验证 drop 确实入栈。 */
		if (grid.property_value("render.mesh") != "Sphere.mesh" ||
			commands.undo_count() != undo_base)
			return fail("reference drop assigns");
		if (!commands.undo() ||
			grid.property_value("render.mesh") != "Cube.mesh")
			return fail("reference drop undo");
		if (!commands.redo() ||
			grid.property_value("render.mesh") != "Sphere.mesh")
			return fail("reference drop redo");
		if (!commands.undo() ||
			grid.property_value("render.mesh") != "Cube.mesh")
			return fail("reference drop back");

	}

	/* filter 拒绝:check 返回 false 时 drop 不生效。 */
	grid.set_asset_filter_check([](const std::string&, const std::string&) {
		return false;
	});
	{
		const std::size_t before = commands.undo_count();
		vgui_drag_payload payload = {};
		payload.kind = VGUI_DRAG_CUSTOM;
		payload.text = "Sphere.mesh";
		payload.data = reinterpret_cast<const void*>(
			static_cast<std::uintptr_t>(drag_payload_kind::asset));
		vgui_begin_drag(ui, grid.widget(), &payload);
		vgui_input_mouse_move(ui, gx + gw * 0.5f, gy + gh * 0.5f);
		vgui_input_mouse_up(ui, 1, gx + gw * 0.5f, gy + gh * 0.5f, 1);
		if (grid.property_value("render.mesh") != "Cube.mesh" ||
			commands.undo_count() != before)
			return fail("reference filter rejects drop");
	}
	grid.set_asset_filter_check(nullptr);


	/* 清空 + 撤销。 */
	{
		vgui_widget* clear_btn = nullptr;
		std::function<void(vgui_widget*)> collect = [&](vgui_widget* w)
		{
			/* clear 按钮与行同 id、kind BUTTON;取第一个文本为 "x" 的。 */
			const char* wid = vgui_widget_get_id(w);
			if (wid && std::strcmp(wid, "property:render.mesh") == 0 &&
				vgui_widget_get_kind(w) == VGUI_BUTTON &&
				std::strcmp(vgui_widget_get_text(w), "x") == 0)
			{
				clear_btn = w;
				return;
			}
			for (size_t i = 0; i < vgui_widget_get_child_count(w) && !clear_btn; ++i)
				collect(vgui_widget_get_child(w, i));
		};
		collect(grid.widget());
		if (!clear_btn)
			return fail("reference clear button found");
		float cx = 0, cy = 0, cw = 0, ch = 0;
		vgui_widget_get_rect(clear_btn, &cx, &cy, &cw, &ch);
		const std::size_t before = commands.undo_count();
		vgui_input_mouse_move(ui, cx + cw * 0.5f, cy + ch * 0.5f);
		vgui_input_mouse_down(ui, 1, cx + cw * 0.5f, cy + ch * 0.5f, 1);
		vgui_input_mouse_up(ui, 1, cx + cw * 0.5f, cy + ch * 0.5f, 1);
		if (grid.property_value("render.mesh") != "" ||
			commands.undo_count() != before + 1)
			return fail("reference clear assigns empty");
		if (!commands.undo() ||
			grid.property_value("render.mesh") != "Cube.mesh")
			return fail("reference clear undo");

	}

	/* browse 与 object 激活。 */
	{
		std::string browsed_value, browsed_filter;
		grid.set_asset_browse_handler([&](const std::string& value,
			const std::string& filter)
		{
			browsed_value = value;
			browsed_filter = filter;
		});
		vgui_widget* browse_btn = nullptr;
		std::function<void(vgui_widget*)> collect = [&](vgui_widget* w)
		{
			const char* wid = vgui_widget_get_id(w);
			if (wid && std::strcmp(wid, "property:render.mesh") == 0 &&
				vgui_widget_get_kind(w) == VGUI_BUTTON &&
				std::strcmp(vgui_widget_get_text(w), "...") == 0)
			{
				browse_btn = w;
				return;
			}
			for (size_t i = 0; i < vgui_widget_get_child_count(w) && !browse_btn; ++i)
				collect(vgui_widget_get_child(w, i));
		};
		collect(grid.widget());
		if (!browse_btn)
			return fail("reference browse button found");
		float bx = 0, by = 0, bw = 0, bh = 0;
		vgui_widget_get_rect(browse_btn, &bx, &by, &bw, &bh);
		vgui_input_mouse_move(ui, bx + bw * 0.5f, by + bh * 0.5f);
		vgui_input_mouse_down(ui, 1, bx + bw * 0.5f, by + bh * 0.5f, 1);
		vgui_input_mouse_up(ui, 1, bx + bw * 0.5f, by + bh * 0.5f, 1);
		if (browsed_value != "Cube.mesh" || browsed_filter != "mesh")
			return fail("reference browse handler");

	}

	/* object_ref:赋值 + 名称点击激活。 */
	{
		object_id activated = invalid_object;
		grid.set_object_activated_handler([&](object_id id) {
			activated = id;
		});
		if (!grid.commit_property_value("ai.target", "42"))
			return fail("reference object commit");
		/* 名称控件 = "property:ai.target" 的第二个 LABEL(第一个是行首
		 * 属性行名;editor 槽的名称 label 在 thumb 之后创建)。 */
		vgui_widget* name = nullptr;
		{
			std::vector<vgui_widget*> labels;
			std::function<void(vgui_widget*)> collect_labels =
				[&](vgui_widget* w)
			{
				const char* wid = vgui_widget_get_id(w);
				if (wid && std::strcmp(wid, "property:ai.target") == 0 &&
					vgui_widget_get_kind(w) == VGUI_LABEL)
					labels.push_back(w);
				for (size_t i = 0; i < vgui_widget_get_child_count(w); ++i)
					collect_labels(vgui_widget_get_child(w, i));
			};
			collect_labels(grid.widget());
			if (labels.size() < 2)
				return fail("reference object name found");
			name = labels[1];
		}
		float nx = 0, ny = 0, nw = 0, nh = 0;
		vgui_widget_get_rect(name, &nx, &ny, &nw, &nh);
		vgui_input_mouse_move(ui, nx + 10.0f, ny + nh * 0.5f);
		vgui_input_mouse_down(ui, 1, nx + 10.0f, ny + nh * 0.5f, 1);
		vgui_input_mouse_up(ui, 1, nx + 10.0f, ny + nh * 0.5f, 1);
		if (activated != 42)
			return fail("reference object activated");
		/* scene_object 载荷拖到 object 行。 */
		vgui_widget* obj_thumb = nullptr;
		std::function<void(vgui_widget*)> collect2 = [&](vgui_widget* w)
		{
			const char* wid = vgui_widget_get_id(w);
			if (wid && std::strcmp(wid, "property:ai.target") == 0 &&
				vgui_widget_get_kind(w) == VGUI_PANEL)
				obj_thumb = w;
			for (size_t i = 0; i < vgui_widget_get_child_count(w) && !obj_thumb; ++i)
				collect2(vgui_widget_get_child(w, i));
		};
		collect2(grid.widget());
		if (!obj_thumb)
			return fail("reference object thumb found");
		float ox = 0, oy = 0, ow = 0, oh = 0;
		vgui_widget_get_rect(obj_thumb, &ox, &oy, &ow, &oh);
		vgui_drag_payload payload = {};
		payload.kind = VGUI_DRAG_CUSTOM;
		payload.text = "7";
		payload.data = reinterpret_cast<const void*>(
			static_cast<std::uintptr_t>(drag_payload_kind::scene_object));
		vgui_begin_drag(ui, grid.widget(), &payload);
		vgui_input_mouse_move(ui, ox + ow * 0.5f, oy + oh * 0.5f);
		vgui_input_mouse_up(ui, 1, ox + ow * 0.5f, oy + oh * 0.5f, 1);
		/* 与 mesh 行同理:同属性 drop 被 merge 链吸收(commit 42 与 drop 7
		 * 合成一条),undo 直接回到本行编辑串的起点值。 */
		if (grid.property_value("ai.target") != "7")
			return fail("reference object drop assigns");
		/* commit 42 与 drop 7 合成一条 (old="", new="7");undo 回空。 */
		if (!commands.undo() || grid.property_value("ai.target") != "")
			return fail("reference object drop undo");
		if (!commands.redo() || grid.property_value("ai.target") != "7")
			return fail("reference object drop redo");

	}

	/* P07:批量编辑事务——跨属性多次提交并入一条 undo。 */
	{
		property_grid batch_grid;
		batch_grid.build(vgui_root(ui));
		batch_grid.set_command_stack(&commands);
		property_desc a;
		a.id = "batch.a";
		a.label = "A";
		a.type = property_type::integer;
		a.value = "1";
		property_desc b;
		b.id = "batch.b";
		b.label = "B";
		b.type = property_type::integer;
		b.value = "10";
		b.default_value = "10";
		b.has_default = true;
		if (!batch_grid.add_property(a) || !batch_grid.add_property(b))
			return fail("batch rows added");
		vgui_update(ui, 0.016f);

		/* 无栈/不平衡防护。 */
		{
			property_grid bare;
			bare.build(vgui_root(ui));
			if (bare.begin_batch("x"))
				return fail("batch without stack rejected");
			if (bare.end_batch())
				return fail("batch unbalanced end rejected");
		}

		if (!batch_grid.begin_batch("Batch edit"))
			return fail("batch begin");
		if (batch_grid.batch_depth() != 1)
			return fail("batch depth 1");
		/* 嵌套:并入外层。 */
		if (!batch_grid.begin_batch("Inner"))
			return fail("batch nested begin");
		if (batch_grid.batch_depth() != 2)
			return fail("batch depth 2");
		if (!batch_grid.commit_property_value("batch.a", "5"))
			return fail("batch commit a");
		if (!batch_grid.end_batch())
			return fail("batch nested end");
		if (!batch_grid.commit_property_value("batch.b", "50"))
			return fail("batch commit b");
		if (!batch_grid.end_batch())
			return fail("batch end");
		if (batch_grid.batch_depth() != 0)
			return fail("batch depth back to 0");
		if (batch_grid.property_value("batch.a") != "5" ||
			batch_grid.property_value("batch.b") != "50")
			return fail("batch applies immediately");

		/* 事务后一条 undo 记录,undo 一次全部回滚。 */
		const std::size_t records = commands.undo_count();
		if (!commands.undo() ||
			batch_grid.property_value("batch.a") != "1" ||
			batch_grid.property_value("batch.b") != "10")
			return fail("batch undo restores all");
		if (commands.undo_count() != records - 1)
			return fail("batch single undo record");
		if (!commands.redo() ||
			batch_grid.property_value("batch.a") != "5" ||
			batch_grid.property_value("batch.b") != "50")
			return fail("batch redo reapplies");

		/* reset_all 同样一条 undo(内部走 batch;batch.b 有默认值)。 */
		if (batch_grid.property_value("batch.b") != "50")
			return fail("reset all precondition");
		const std::size_t before_reset = commands.undo_count();
		batch_grid.reset_all();
		if (batch_grid.property_value("batch.b") != "10" ||
			commands.undo_count() != before_reset + 1)
			return fail("reset all single record");
		if (!commands.undo() ||
			batch_grid.property_value("batch.b") != "50")
			return fail("reset all undo");
	}
	}	/* grid/context 块:先于 vgui_destroy 析构 */

	vgui_destroy(ui);
	return 0;
}

//----------------------------------------------------------------------------
// M3 Outliner:O01~O06
//----------------------------------------------------------------------------

static int test_outliner_row_extensions()
{
	using namespace vgui_editor_kit;
	vgui_context* ui = vgui_create(nullptr, 16.0f);
	if (!ui)
		return fail("row ext context");
	vgui_context_set_size(ui, 900, 700);
	vgui_style style;
	vgui_style_default(&style);
	style.tree.row_height = 24.0f;
	vgui_context_set_style(ui, &style);

	editor_context context(ui);
	outliner_view scene;
	scene.build(context, vgui_root(ui));
	vgui_widget_set_pos(scene.widget(), 10.0f, 10.0f);
	vgui_widget_set_size(scene.widget(), 240.0f, 200.0f);

	vgui_item_id root = scene.add_node("Root", 0, 1);
	vgui_item_id child = scene.add_node("Child", root, 2);
	scene.expand(root);
	if (!root || !child)
		return fail("row ext nodes");

	/* O01 透传:图标与文字颜色。 */
	scene.set_node_icon(root, nullptr);
	scene.set_node_text_color(child, 0xFF80FF80u);
	if (vgui_tree_get_item_text_color(scene.widget(), child) != 0xFF80FF80u)
		return fail("row ext text color passthrough");

	scene.enable_row_buttons();
	vgui_update(ui, 0.016f);
	vgui_layout(ui);

	object_id toggled = invalid_object;
	bool toggled_state = false;
	int toggles = 0;
	scene.set_visibility_listener([&](object_id id, bool state) {
		toggled = id; toggled_state = state; ++toggles;
	});
	object_id locked_id = invalid_object;
	scene.set_lock_listener([&](object_id id, bool state) {
		locked_id = id; (void)state;
	});

	scene.set_object_visibility(2, false);
	if (scene.object_visibility(2) || !scene.object_visibility(1))
		return fail("row ext visibility state");
	scene.set_object_locked(1, true);
	if (!scene.object_locked(1) || scene.object_locked(2))
		return fail("row ext lock state");

	/* 点可见性按钮(行尾左侧第一个):不改选中、不改展开。 */
	float tx = 0, ty = 0, tw = 0, th = 0;
	vgui_widget_get_rect(scene.widget(), &tx, &ty, &tw, &th);
	(void)th;
	const float row_h = style.tree.row_height;
	const float bsize = std::max(10.0f, std::min(18.0f, row_h - 8.0f));
	const float row0_y = ty + style.tree.padding_y + row_h * 0.5f;
	const float content_right = tx + tw - style.tree.padding_x;
	const float btn0_x = content_right - 4.0f - bsize - (bsize + 4.0f);
	const float btn1_x = content_right - 4.0f - bsize;

	vgui_input_mouse_move(ui, btn0_x, row0_y);
	vgui_input_mouse_down(ui, 1, btn0_x, row0_y, 1);
	vgui_input_mouse_up(ui, 1, btn0_x, row0_y, 1);
	if (toggles != 1 || toggled != 1 || toggled_state)
		return fail("row ext visibility button toggles");
	if (vgui_tree_get_selected_count(scene.widget()) != 0)
		return fail("row ext visibility button does not select");
	if (!vgui_tree_get_item_expanded(scene.widget(), root))
		return fail("row ext visibility button keeps expansion");

	vgui_input_mouse_down(ui, 1, btn1_x, row0_y, 1);
	vgui_input_mouse_up(ui, 1, btn1_x, row0_y, 1);
	if (locked_id != 1 || scene.object_locked(1))
		return fail("row ext lock button toggles");

	/* 正文区仍可选中。 */
	vgui_input_mouse_down(ui, 1, tx + 30.0f, row0_y, 1);
	vgui_input_mouse_up(ui, 1, tx + 30.0f, row0_y, 1);
	if (vgui_tree_get_selected_count(scene.widget()) != 1)
		return fail("row ext text area still selects");

	vgui_destroy(ui);
	return 0;
}

static int test_outliner_inline_rename()
{
	using namespace vgui_editor_kit;
	vgui_context* ui = vgui_create(nullptr, 16.0f);
	if (!ui)
		return fail("rename context");
	vgui_context_set_size(ui, 900, 700);
	vgui_style style;
	vgui_style_default(&style);
	style.tree.row_height = 24.0f;
	vgui_context_set_style(ui, &style);

	editor_context context(ui);
	command_stack commands;
	outliner_view scene;
	scene.build(context, vgui_root(ui));
	scene.set_command_stack(&commands);
	vgui_widget_set_pos(scene.widget(), 10.0f, 10.0f);
	vgui_widget_set_size(scene.widget(), 240.0f, 200.0f);

	vgui_item_id root = scene.add_node("Cube", 0, 42);
	vgui_item_id other = scene.add_node("Cube2", 0, 43);
	if (!root || !other)
		return fail("rename nodes");
	std::map<object_id, std::string> node_names = { { 42, "Cube" },
		{ 43, "Cube2" } };
	scene.set_rename_handler([&](object_id id, const std::string& name) {
		/* 重名检测由宿主负责:不许与其他节点同名。 */
		if (name.empty())
			return false;
		for (const auto& entry : node_names)
			if (entry.first != id && entry.second == name)
				return false;
		node_names[id] = name;
		return true;
	});
	scene.enable_inline_rename();
	vgui_update(ui, 0.016f);
	vgui_layout(ui);

	/* 直接程序化改名:命令栈 + 撤销。 */
	const std::size_t base = commands.undo_count();
	if (!scene.rename_node(42, "Box") ||
		std::string(vgui_tree_get_item_text(scene.widget(), root)) != "Box" ||
		commands.undo_count() != base + 1)
		return fail("rename node records command");
	if (!commands.undo() ||
		std::string(vgui_tree_get_item_text(scene.widget(), root)) != "Cube")
		return fail("rename undo");
	if (!commands.redo() ||
		std::string(vgui_tree_get_item_text(scene.widget(), root)) != "Box")
		return fail("rename redo");

	/* 空名拒绝、非法(逗号)拒绝、重名拒绝。 */
	if (scene.rename_node(42, "") || commands.undo_count() != base + 1)
		return fail("rename empty rejected");
	if (scene.rename_node(42, "a,b") || commands.undo_count() != base + 1)
		return fail("rename comma rejected");
	if (scene.rename_node(42, "Cube2") || commands.undo_count() != base + 1)
		return fail("rename duplicate rejected");

	/* 行内编辑:Enter 提交入栈。 */
	commands.undo();
	if (std::string(vgui_tree_get_item_text(scene.widget(), root)) != "Cube")
		return fail("rename back to cube");
	scene.begin_rename(root);
	if (!scene.renaming_active())
		return fail("rename editor opens");
	vgui_input_text(ui, "Sphere");
	vgui_input_key(ui, VGUI_KEY_ENTER, 1, 0);
	vgui_input_key(ui, VGUI_KEY_ENTER, 0, 0);
	if (scene.renaming_active())
		return fail("rename editor closes on enter");
	if (std::string(vgui_tree_get_item_text(scene.widget(), root)) != "Sphere"
		|| commands.undo_count() != base + 1)
		return fail("rename enter commits");

	/* Esc 取消:恢复原名。 */
	scene.begin_rename(root);
	vgui_input_text(ui, "zzz");
	vgui_input_key(ui, VGUI_KEY_ESCAPE, 1, 0);
	vgui_input_key(ui, VGUI_KEY_ESCAPE, 0, 0);
	if (std::string(vgui_tree_get_item_text(scene.widget(), root)) != "Sphere"
		|| commands.undo_count() != base + 1)
		return fail("rename escape cancels");

	/* 失焦提交:焦点被抢走时提交当前文本。 */
	scene.begin_rename(root);
	vgui_input_text(ui, "Torus");
	vgui_set_focus(ui, nullptr);
	if (scene.renaming_active() ||
		std::string(vgui_tree_get_item_text(scene.widget(), root)) != "Torus"
		|| commands.undo_count() != base + 2)
		return fail("rename blur commits");

	/* F2:树持有焦点且有选中时进入编辑。 */
	vgui_tree_select(scene.widget(), root, 1);
	vgui_set_focus(ui, scene.widget());
	vgui_input_key(ui, VGUI_KEY_F2, 1, 0);
	vgui_input_key(ui, VGUI_KEY_F2, 0, 0);
	if (!scene.renaming_active())
		return fail("rename F2 starts");
	vgui_input_key(ui, VGUI_KEY_ESCAPE, 1, 0);
	vgui_input_key(ui, VGUI_KEY_ESCAPE, 0, 0);

	/* 双击进入编辑。 */
	float tx = 0, ty = 0, tw = 0, th = 0;
	vgui_widget_get_rect(scene.widget(), &tx, &ty, &tw, &th);
	(void)tw; (void)th;
	const float row0_y = ty + style.tree.padding_y + style.tree.row_height * 0.5f;
	vgui_input_mouse_move(ui, tx + 30.0f, row0_y);
	vgui_input_mouse_down(ui, 1, tx + 30.0f, row0_y, 1);
	vgui_input_mouse_up(ui, 1, tx + 30.0f, row0_y, 1);
	vgui_input_mouse_down(ui, 1, tx + 30.0f, row0_y, 2);
	vgui_input_mouse_up(ui, 1, tx + 30.0f, row0_y, 2);
	if (!scene.renaming_active())
		return fail("rename double click starts");
	vgui_input_key(ui, VGUI_KEY_ESCAPE, 1, 0);
	vgui_input_key(ui, VGUI_KEY_ESCAPE, 0, 0);

	vgui_destroy(ui);
	return 0;
}

/* O03 用宿主场景模型:与 test_scene_asset_commands 相同模式。 */
using vgui_editor_kit::object_id;
using vgui_editor_kit::invalid_object;
struct outliner_host_scene
{
	std::map<object_id, object_id> parent_of;
	std::map<object_id, std::vector<object_id>> children;
	std::vector<object_id> roots;

	void set_parent(object_id node, object_id parent)
	{
		const bool had = parent_of.count(node) != 0;
		object_id old = had ? parent_of[node] : invalid_object;
		if (had && old == parent)
			return;
		if (had && old == invalid_object)
		{
			auto it = std::find(roots.begin(), roots.end(), node);
			if (it != roots.end())
				roots.erase(it);
		}
		else if (had)
		{
			auto& siblings = children[old];
			auto it = std::find(siblings.begin(), siblings.end(), node);
			if (it != siblings.end())
				siblings.erase(it);
		}
		if (parent == invalid_object)
		{
			parent_of.erase(node);
			if (std::find(roots.begin(), roots.end(), node) == roots.end())
				roots.push_back(node);
		}
		else
		{
			parent_of[node] = parent;
			children[parent].push_back(node);
		}
	}

	void place(object_id node, object_id parent, std::size_t index)
	{
		set_parent(node, parent);
		std::vector<object_id>& siblings = parent == invalid_object
			? roots : children[parent];
		auto it = std::find(siblings.begin(), siblings.end(), node);
		if (it != siblings.end())
			siblings.erase(it);
		index = std::min(index, siblings.size());
		siblings.insert(siblings.begin() + (std::ptrdiff_t)index, node);
	}
};

static int test_outliner_drag_semantics()
{
	using namespace vgui_editor_kit;
	vgui_context* ui = vgui_create(nullptr, 16.0f);
	if (!ui)
		return fail("drag context");
	vgui_context_set_size(ui, 900, 700);
	vgui_style style;
	vgui_style_default(&style);
	style.tree.row_height = 24.0f;
	vgui_context_set_style(ui, &style);

	editor_context context(ui);
	command_stack commands;
	outliner_view scene;
	scene.build(context, vgui_root(ui));
	scene.set_command_stack(&commands);
	vgui_widget_set_pos(scene.widget(), 10.0f, 10.0f);
	vgui_widget_set_size(scene.widget(), 260.0f, 300.0f);

	outliner_host_scene model;
	std::map<object_id, vgui_item_id> item_ids;
	std::function<void(const std::vector<object_id>&)> rebuild =
		[&](const std::vector<object_id>& nodes)
	{
		scene.clear();
		item_ids.clear();
		std::function<void(object_id, vgui_item_id)> add = [&](object_id node,
			vgui_item_id parent_item)
		{
			const vgui_item_id item = scene.add_node(("Node " +
				std::to_string(node)).c_str(), parent_item, node);
			item_ids[node] = item;
			auto it = model.children.find(node);
			if (it != model.children.end())
				for (object_id c : it->second)
					add(c, item);
		};
		for (object_id node : nodes)
			add(node, 0);
	};

	scene.set_move_handler([&](object_id node, object_id parent,
		std::size_t index)
	{
		object_id walk = parent;
		while (walk != invalid_object)
		{
			if (walk == node)
				return false;
			walk = model.parent_of.count(walk) ? model.parent_of[walk]
				: invalid_object;
		}
		model.place(node, parent, index);
		rebuild(model.roots);
		return true;
	});

	/* 1 根,子 2、3;2 的子 4、5。 */
	model.set_parent(1, invalid_object);
	model.set_parent(2, 1);
	model.set_parent(3, 1);
	model.set_parent(4, 2);
	model.set_parent(5, 2);
	rebuild(model.roots);
	scene.expand(item_ids[1]);
	scene.expand(item_ids[2]);
	scene.enable_drag_drop();
	vgui_update(ui, 0.016f);
	vgui_layout(ui);

	float tx = 0, ty = 0, tw = 0, th = 0;
	vgui_widget_get_rect(scene.widget(), &tx, &ty, &tw, &th);
	(void)tw; (void)th;
	const float row_h = style.tree.row_height;
	const auto row_y = [&](int row) {
		return ty + style.tree.padding_y + row_h * (float)row + row_h * 0.5f;
	};
	/* 可见行序:1, 2, 4, 5, 3。 */

	/* 位置解析:上=before,中=onto,下=after,空白=roots。行 2 = 节点 4。 */
	{
		auto loc = scene.drop_location_at(tx + 30.0f,
			ty + style.tree.padding_y + row_h * 2.0f + row_h * 0.1f);
		if (loc.kind != outliner_view::drop_kind::before || loc.parent != 2 ||
			loc.index != 0)
			return fail("drop resolve before");
		loc = scene.drop_location_at(tx + 30.0f,
			ty + style.tree.padding_y + row_h * 2.0f + row_h * 0.9f);
		if (loc.kind != outliner_view::drop_kind::after || loc.parent != 2 ||
			loc.index != 1)
			return fail("drop resolve after");
		loc = scene.drop_location_at(tx + 30.0f,
			ty + style.tree.padding_y + row_h * 2.0f + row_h * 0.5f);
		if (loc.kind != outliner_view::drop_kind::onto || loc.parent != 4 ||
			loc.index != 0)
			return fail("drop resolve onto");
		loc = scene.drop_location_at(tx + 30.0f,
			ty + style.tree.padding_y + row_h * 5.0f + row_h * 2.0f);
		if (loc.kind != outliner_view::drop_kind::roots ||
			loc.index != model.roots.size())
			return fail("drop resolve roots");
		/* 提示:onto 给 RECT,before 给 LINE。 */
		vgui_drop_hint hint = {};
		auto onto_loc = scene.drop_location_at(tx + 30.0f,
			ty + style.tree.padding_y + row_h * 2.0f + row_h * 0.5f);
		scene.emit_hover_hint(onto_loc, &hint);
		if (hint.kind != VGUI_DROP_HINT_RECT || hint.h < row_h * 0.5f)
			return fail("drop hint onto rect");
		auto before_loc = scene.drop_location_at(tx + 30.0f,
			ty + style.tree.padding_y + row_h * 2.0f + row_h * 0.1f);
		scene.emit_hover_hint(before_loc, &hint);
		if (hint.kind != VGUI_DROP_HINT_LINE)
			return fail("drop hint before line");
	}

	/* 拖到行中间成为子节点,可撤销。 */
	{
		const std::size_t undo_base = commands.undo_count();
		const float from_y = row_y(4);	/* 节点 3 */
		const float to_y = ty + style.tree.padding_y + row_h * 1.0f
			+ row_h * 0.5f;		/* 节点 2 中部 */
		vgui_input_mouse_move(ui, tx + 30.0f, from_y);
		vgui_input_mouse_down(ui, 1, tx + 30.0f, from_y, 1);
		vgui_input_mouse_move(ui, tx + 30.0f, to_y);
		if (!vgui_drag_active(ui))
			return fail("onto drag begins");
		vgui_input_mouse_up(ui, 1, tx + 30.0f, to_y, 1);
		auto& kids = model.children[2];
		if (model.parent_of[3] != 2 ||
			std::find(kids.begin(), kids.end(), 3) == kids.end() ||
			commands.undo_count() != undo_base + 1)
			return fail("drop onto makes child");
		if (!commands.undo() || model.parent_of[3] != 1)
			return fail("drop onto undo");
		if (!commands.redo())
			return fail("drop onto redo");
	}

	/* 拖父节点到自己后代上:拒绝且不入栈。 */
	{
		const std::size_t undo_base = commands.undo_count();
		/* 重建后 item id 变了,重新展开。 */
		scene.expand(item_ids[1]);
		scene.expand(item_ids[2]);
		vgui_update(ui, 0.016f);
		const float from_y = row_y(0);	/* 节点 1 */
		const float to_y = row_y(2);	/* 节点 4(1 的后代)中部 */
		vgui_input_mouse_move(ui, tx + 30.0f, from_y);
		vgui_input_mouse_down(ui, 1, tx + 30.0f, from_y, 1);
		vgui_input_mouse_move(ui, tx + 30.0f, to_y);
		if (!vgui_drag_active(ui))
			return fail("cycle drag begins");
		vgui_input_mouse_up(ui, 1, tx + 30.0f, to_y, 1);
		if (model.parent_of[1] != invalid_object ||
			commands.undo_count() != undo_base)
			return fail("cycle drop rejected");
	}

	/* 多选拖动:两条移动并入一个事务,一次撤销全部还原。 */
	{
		scene.set_multi_select(true);
		/* 重置为 1 {2 {4,5}, 3}。 */
		commands.undo();
		commands.undo();
		if (model.parent_of[3] != 1)
			return fail("multi reset state");
		rebuild(model.roots);
		scene.expand(item_ids[1]);
		scene.expand(item_ids[2]);
		vgui_update(ui, 0.016f);

		/* 选节点 4、5(行 2、3):多选用程序化选择。 */
		vgui_tree_select(scene.widget(), item_ids[4], 1);
		vgui_tree_select(scene.widget(), item_ids[5], 1);
		if (vgui_tree_get_selected_count(scene.widget()) != 2)
			return fail("multi select rows");

		const std::size_t undo_base = commands.undo_count();
		const float to_y = ty + style.tree.padding_y + row_h * 4.0f
			+ row_h * 0.1f;	/* 节点 3 顶部 = before */
		vgui_input_mouse_move(ui, tx + 30.0f, row_y(2));
		vgui_input_mouse_down(ui, 1, tx + 30.0f, row_y(2), 1);
		vgui_input_mouse_move(ui, tx + 30.0f, to_y);
		if (!vgui_drag_active(ui))
			return fail("multi drag begins");
		vgui_input_mouse_up(ui, 1, tx + 30.0f, to_y, 1);
		if (model.parent_of[4] != 1 || model.parent_of[5] != 1)
			return fail("multi drop moved both");
		{
			auto& kids = model.children[1];
			if (kids.size() != 4 || kids[1] != 4 || kids[2] != 5)
				return fail("multi drop order");
		}
		if (commands.undo_count() != undo_base + 1)
			return fail("multi drop single undo record");
		if (!commands.undo() || model.parent_of[4] != 2 ||
			model.parent_of[5] != 2)
			return fail("multi drop undo restores both");
	}

	vgui_destroy(ui);
	return 0;
}

static int test_outliner_filter_sync()
{
	using namespace vgui_editor_kit;
	vgui_context* ui = vgui_create(nullptr, 16.0f);
	if (!ui)
		return fail("filter context");
	vgui_context_set_size(ui, 900, 700);
	vgui_style style;
	vgui_style_default(&style);
	style.tree.row_height = 24.0f;
	vgui_context_set_style(ui, &style);

	editor_context context(ui);
	outliner_view scene;
	scene.build(context, vgui_root(ui));
	vgui_widget_set_pos(scene.widget(), 10.0f, 10.0f);
	vgui_widget_set_size(scene.widget(), 240.0f, 200.0f);

	vgui_item_id root = scene.add_node("World", 0, 1);
	vgui_item_id alpha = scene.add_node("alpha_light", root, 2);
	vgui_item_id beta = scene.add_node("beta_dark", root, 3);
	vgui_item_id gamma = scene.add_node("gamma_light", beta, 4);
	if (!root || !alpha || !beta || !gamma)
		return fail("filter nodes");
	scene.expand(root);
	scene.expand(beta);
	vgui_update(ui, 0.016f);
	vgui_layout(ui);

	vgui_widget* tree = scene.widget();
	const auto row_item = [&](int row) {
		float tx = 0, ty = 0, tw = 0, th = 0;
		vgui_widget_get_rect(tree, &tx, &ty, &tw, &th);
		(void)tw; (void)th;
		return vgui_tree_item_at_point(tree, 30.0f,
			style.tree.padding_y + style.tree.row_height * (float)row
			+ style.tree.row_height * 0.5f);
	};

	/* 名称过滤:命中 + 祖先可见。行:World, alpha_light,
	 * beta(gamma 的祖先), gamma_light。 */
	scene.set_filter("light");
	vgui_update(ui, 0.016f);
	if (row_item(0) != root || row_item(1) != alpha ||
		row_item(2) != beta || row_item(3) != gamma)
		return fail("filter keeps matches and ancestors");
	if (row_item(4) != 0)
		return fail("filter hides non-matching");
	/* beta 行可见(它是命中行 gamma 的祖先),但选择保留。 */
	vgui_tree_select(tree, beta, 1);
	if (!vgui_tree_is_selected(tree, beta))
		return fail("filter keeps selection");
	scene.set_filter(nullptr);
	vgui_update(ui, 0.016f);
	if (row_item(0) != root || row_item(1) != alpha || row_item(2) != beta)
		return fail("filter clear restores rows");
	if (!vgui_tree_is_selected(tree, beta) ||
		!vgui_tree_get_item_expanded(tree, beta))
		return fail("filter clear keeps selection and expansion");

	/* 类型过滤(handler)叠加文本过滤。 */
	int type_calls = 0;
	scene.set_filter_match_handler([&](object_id, const char*) {
		++type_calls;
		return true;
	});
	scene.set_filter("light");
	vgui_update(ui, 0.016f);
	if (type_calls == 0 || row_item(1) != alpha)
		return fail("filter match handler consulted");
	scene.set_filter_match_handler(nullptr);
	scene.set_filter(nullptr);
	vgui_update(ui, 0.016f);

	/* 选择同步:外来选中自动展开并选中对应行。 */
	scene.enable_selection_sync();
	vgui_tree_set_item_expanded(tree, root, 0);
	vgui_update(ui, 0.016f);
	object_id observed = invalid_object;
	scene.set_selection_listener([&](object_id id) { observed = id; });
	context.set_selection(4);	/* gamma,藏于折叠的 root 下 */
	if (!vgui_tree_is_selected(tree, gamma) ||
		!vgui_tree_get_item_expanded(tree, root))
		return fail("sync expands and selects");
	if (observed != 4)
		return fail("sync notifies listeners");
	if (context.selection() != 4)
		return fail("sync keeps context selection");

	/* 自己点击不再引发滚动/重入:选中正常传播。 */
	vgui_input_mouse_move(ui, 40.0f, 40.0f);
	context.set_selection(1);
	if (context.selection() != 1)
		return fail("sync stable after foreign selection");

	vgui_destroy(ui);
	return 0;
}

static int test_virtual_outliner()
{
	using namespace vgui_editor_kit;
	vgui_context* ui = vgui_create(nullptr, 16.0f);
	if (!ui)
		return fail("virtual context");
	vgui_context_set_size(ui, 900, 700);
	vgui_style style;
	vgui_style_default(&style);
	style.tree.row_height = 24.0f;
	vgui_context_set_style(ui, &style);

	editor_context context(ui);
	command_stack commands;
	outliner_view scene;
	scene.build(context, vgui_root(ui));
	vgui_widget_set_pos(scene.widget(), 10.0f, 10.0f);
	vgui_widget_set_size(scene.widget(), 240.0f, 200.0f);

	/* 万级场景:100 根 × 100 子。带调用计数的适配器。 */
	struct counting_scene
	{
		std::vector<object_id> roots;
		std::map<object_id, std::vector<object_id>> children;
		std::map<object_id, object_id> parent_of;
		std::map<object_id, std::string> names;
		int name_calls = 0;
		int child_count_calls = 0;
	};
	counting_scene vs;
	for (object_id r = 1; r <= 100; ++r)
	{
		vs.roots.push_back(r);
		vs.names[r] = "Root " + std::to_string(r);
		for (object_id c = 1000 + r * 100; c < 1000 + r * 100 + 100; ++c)
		{
			vs.children[r].push_back(c);
			vs.parent_of[c] = r;
			vs.names[c] = "Child " + std::to_string(c);
		}
	}

	outliner_view::scene_source source;
	source.root_count = [&vs]() { return vs.roots.size(); };
	source.root_at = [&vs](std::size_t i) { return vs.roots[i]; };
	source.child_count = [&vs](object_id o) -> std::size_t {
		auto it = vs.children.find(o);
		return it == vs.children.end() ? 0 : it->second.size();
	};
	source.child_at = [&vs](object_id o, std::size_t i) {
		return vs.children[o][i];
	};
	source.name = [&vs](object_id o) -> std::string {
		++vs.name_calls;
		auto it = vs.names.find(o);
		return it == vs.names.end() ? std::string("?") : it->second;
	};
	source.parent_of = [&vs](object_id o) -> object_id {
		auto it = vs.parent_of.find(o);
		return it == vs.parent_of.end() ? invalid_object : it->second;
	};
	scene.set_scene_source(source);
	scene.set_command_stack(&commands);
	if (!scene.is_virtual())
		return fail("virtual mode entered");
	vgui_update(ui, 0.016f);
	vgui_layout(ui);

	vgui_widget* tree = scene.widget();
	if (scene.projected_row_count() != 100)
		return fail("virtual collapsed projection");
	/* 万级节点:折叠子树的名字从不被查询。 */
	vs.name_calls = 0;
	vgui_tree_select(tree, 7, 1);
	vgui_tree_set_cursor(tree, 7);
	vgui_update(ui, 0.016f);
	if (vs.name_calls > 300)
		return fail("virtual only queries visible rows");

	/* 展开:投影增长但结构稳定,选择保持。 */
	scene.expand(3);
	vgui_update(ui, 0.016f);
	if (scene.projected_row_count() != 100 + 100)
		return fail("virtual expand projection");
	if (!vgui_tree_is_selected(tree, 7))
		return fail("virtual selection stable across expand");
	vs.name_calls = 0;
	vgui_update(ui, 0.016f);
	if (vs.name_calls > 400)
		return fail("virtual expanded queries bounded");

	/* 行内容增量刷新:内容宽度只按投影重测(折叠子树不参与)。 */
	const float row_h = style.tree.row_height;
	float tx = 0, ty = 0, tw = 0, th = 0;
	vgui_widget_get_rect(tree, &tx, &ty, &tw, &th);
	(void)tw; (void)th;
	vs.name_calls = 0;
	vs.names[3] = "Renamed Root";
	scene.scene_row_changed(3);
	vgui_update(ui, 0.016f);
	if (std::string(vgui_tree_get_item_text(tree, 3)) != "Renamed Root")
		return fail("virtual row changed updates");
	if (vs.name_calls > 300)
		return fail("virtual row refresh bounded");

	/* 结构变化:重建投影但选择保持(稳定 id)。 */
	scene.scene_changed();
	vgui_update(ui, 0.016f);
	if (!vgui_tree_is_selected(tree, 7) || scene.projected_row_count() != 200)
		return fail("virtual structural change keeps selection");

	/* 重命名走 handler + 命令栈(handler 负责写回宿主模型)。 */
	const std::size_t undo_base = commands.undo_count();
	std::string renamed_to;
	scene.set_rename_handler([&](object_id node, const std::string& name) {
		renamed_to = name;
		vs.names[node] = name;
		return true;
	});
	if (!scene.rename_node(7, "Lucky") || renamed_to != "Lucky" ||
		commands.undo_count() != undo_base + 1)
		return fail("virtual rename records command");
	if (std::string(vgui_tree_get_item_text(tree, 7)) != "Lucky")
		return fail("virtual rename visible");
	if (!commands.undo() || vs.names[7] != "Root 7")
		return fail("virtual rename undo restores model");
	if (!commands.redo() || vs.names[7] != "Lucky")
		return fail("virtual rename redo");

	/* 拖放:根 2 拖到根 1 中部成为子节点(可撤销)。 */
	scene.set_move_handler([&](object_id node, object_id parent,
		std::size_t index)
	{
		object_id walk = parent;
		while (walk != invalid_object)
		{
			if (walk == node)
				return false;
			auto it = vs.parent_of.find(walk);
			walk = it == vs.parent_of.end() ? invalid_object : it->second;
		}
		std::vector<object_id>& old = vs.parent_of.count(node) &&
			vs.parent_of[node] != invalid_object
			? vs.children[vs.parent_of[node]] : vs.roots;
		auto it = std::find(old.begin(), old.end(), node);
		if (it != old.end())
			old.erase(it);
		if (parent == invalid_object)
		{
			vs.parent_of.erase(node);
			vs.roots.push_back(node);
			return true;
		}
		vs.parent_of[node] = parent;
		std::vector<object_id>& kids = vs.children[parent];
		index = std::min(index, kids.size());
		kids.insert(kids.begin() + (std::ptrdiff_t)index, node);
		return true;
	});
	scene.enable_drag_drop();
	vgui_update(ui, 0.016f);
	vgui_layout(ui);

	const auto row_y = [&](int row) {
		return ty + style.tree.padding_y + row_h * (float)row + row_h * 0.5f;
	};

	/* 过滤:虚拟投影只保留命中行。"Lucky 7" 唯一命中
	 * ("Root N" 两位数不会被 "Lucky" 误伤)。 */
	vs.names[7] = "Lucky 7";
	scene.set_filter("Lucky");
	vgui_update(ui, 0.016f);
	if (scene.projected_row_count() != 1)
		return fail("virtual filter projection");
	scene.set_filter(nullptr);
	vgui_update(ui, 0.016f);
	if (scene.projected_row_count() != 200)
		return fail("virtual filter clear restores");

	/* 点击行 2(根 3)箭头折叠:展开状态由 kit 持有,走 row_toggle。 */
	const float arrow_x = tx + style.tree.padding_x + style.tree.arrow_gap
		+ style.tree.arrow_size * 0.5f;
	vgui_input_mouse_move(ui, arrow_x, row_y(2));
	vgui_input_mouse_down(ui, 1, arrow_x, row_y(2), 1);
	vgui_input_mouse_up(ui, 1, arrow_x, row_y(2), 1);
	vgui_update(ui, 0.016f);
	if (scene.projected_row_count() != 100)
		return fail("virtual collapse via arrow toggle");
	if (!vgui_tree_is_selected(tree, 7))
		return fail("virtual selection stable across collapse");

	/* 拖根 2(行 1)到根 1(行 0)中部:成为子节点,undo 还原。 */
	{
		const std::size_t before = commands.undo_count();
		vgui_input_mouse_move(ui, tx + 30.0f, row_y(1));
		vgui_input_mouse_down(ui, 1, tx + 30.0f, row_y(1), 1);
		vgui_input_mouse_move(ui, tx + 30.0f,
			ty + style.tree.padding_y + row_h * 0.5f);
		if (!vgui_drag_active(ui))
			return fail("virtual drag begins");
		vgui_input_mouse_up(ui, 1, tx + 30.0f,
			ty + style.tree.padding_y + row_h * 0.5f, 1);
		auto it = vs.parent_of.find(2);
		if (it == vs.parent_of.end() || it->second != 1 ||
			scene.projected_row_count() != 99 ||
			commands.undo_count() != before + 1)
			return fail("virtual drag drop reparents");
		if (!commands.undo() || vs.parent_of.count(2))
			return fail("virtual drag undo");
	}

	vgui_destroy(ui);
	return 0;
}

static int test_outliner_context_menu()
{
	using namespace vgui_editor_kit;
	vgui_context* ui = vgui_create(nullptr, 16.0f);
	if (!ui)
		return fail("menu context");
	vgui_context_set_size(ui, 900, 700);
	vgui_style style;
	vgui_style_default(&style);
	style.tree.row_height = 24.0f;
	vgui_context_set_style(ui, &style);

	editor_context context(ui);
	outliner_view scene;
	scene.build(context, vgui_root(ui));
	vgui_widget_set_pos(scene.widget(), 10.0f, 10.0f);
	vgui_widget_set_size(scene.widget(), 240.0f, 200.0f);

	vgui_item_id root = scene.add_node("Root", 0, 1);
	vgui_item_id child = scene.add_node("Child", root, 2);
	if (!root || !child)
		return fail("menu nodes");
	scene.expand(root);

	std::vector<outliner_view::action> actions;
	std::vector<std::vector<object_id>> action_nodes;
	scene.set_action_handler([&](outliner_view::action kind,
		const std::vector<object_id>& nodes)
	{
		actions.push_back(kind);
		action_nodes.push_back(nodes);
		return true;
	});
	scene.enable_context_menu();
	scene.enable_inline_rename();
	vgui_update(ui, 0.016f);
	vgui_layout(ui);

	vgui_widget* tree = scene.widget();
	float tx = 0, ty = 0, tw = 0, th = 0;
	vgui_widget_get_rect(tree, &tx, &ty, &tw, &th);
	(void)tw; (void)th;
	const float row_h = style.tree.row_height;
	const float row0_y = ty + style.tree.padding_y + row_h * 0.5f;

	/* 右键行 0:选中并弹出菜单。 */
	vgui_input_mouse_move(ui, tx + 30.0f, row0_y);
	vgui_input_mouse_down(ui, 3, tx + 30.0f, row0_y, 1);
	vgui_input_mouse_up(ui, 3, tx + 30.0f, row0_y, 1);
	vgui_widget* menu = vgui_context_get_open_menu(ui);
	if (!menu || !vgui_menu_is_open(menu))
		return fail("context menu opens");
	if (!vgui_tree_is_selected(tree, root))
		return fail("context menu selects row");
	if (vgui_menu_get_count(menu) < 8)
		return fail("context menu entries");

	/* 找到 Delete 项并用键盘激活:菜单与命令共用 dispatch。 */
	vgui_item_id delete_item = 0;
	for (int i = 0; i < vgui_menu_get_count(menu); ++i)
	{
		vgui_item_id item = vgui_menu_get_item_at(menu, i);
		if (std::string(vgui_menu_get_item_text(menu, item)) == "Delete")
		{
			delete_item = item;
			break;
		}
	}
	if (!delete_item)
		return fail("context menu has delete");
	vgui_menu_set_cursor(menu, delete_item);
	vgui_input_key(ui, VGUI_KEY_ENTER, 1, 0);
	vgui_input_key(ui, VGUI_KEY_ENTER, 0, 0);
	if (actions.size() != 1 || actions[0] != outliner_view::action::remove ||
		action_nodes[0].size() != 1 || action_nodes[0][0] != 1)
		return fail("context menu delete dispatches to selection");

	/* 快捷键与菜单共用实现:Delete / Ctrl+D / F2。 */
	vgui_tree_select(tree, child, 1);
	vgui_set_focus(ui, tree);
	vgui_input_key(ui, VGUI_KEY_DELETE, 1, 0);
	vgui_input_key(ui, VGUI_KEY_DELETE, 0, 0);
	if (actions.size() != 2 || actions[1] != outliner_view::action::remove ||
		action_nodes[1][0] != 2)
		return fail("delete key dispatches");
	vgui_input_key(ui, 'D', 1, VGUI_MOD_CTRL);
	vgui_input_key(ui, 'D', 0, VGUI_MOD_CTRL);
	if (actions.size() != 3 || actions[2] != outliner_view::action::duplicate)
		return fail("ctrl+d dispatches");
	vgui_input_key(ui, VGUI_KEY_F2, 1, 0);
	vgui_input_key(ui, VGUI_KEY_F2, 0, 0);
	if (!scene.renaming_active())
		return fail("f2 starts rename via shared dispatch");
	vgui_input_key(ui, VGUI_KEY_ESCAPE, 1, 0);
	vgui_input_key(ui, VGUI_KEY_ESCAPE, 0, 0);

	/* 菜单里的可见性开关走 kit 内部实现。 */
	vgui_input_mouse_down(ui, 3, tx + 30.0f, row0_y, 1);
	vgui_input_mouse_up(ui, 3, tx + 30.0f, row0_y, 1);
	menu = vgui_context_get_open_menu(ui);
	if (!menu)
		return fail("context menu reopens");
	vgui_item_id visible_item = 0;
	for (int i = 0; i < vgui_menu_get_count(menu); ++i)
	{
		vgui_item_id item = vgui_menu_get_item_at(menu, i);
		if (std::string(vgui_menu_get_item_text(menu, item)) == "Visible")
		{
			visible_item = item;
			break;
		}
	}
	if (!visible_item || !vgui_menu_get_item_checked(menu, visible_item))
		return fail("context menu visibility checked state");
	vgui_menu_set_cursor(menu, visible_item);
	vgui_input_key(ui, VGUI_KEY_ENTER, 1, 0);
	vgui_input_key(ui, VGUI_KEY_ENTER, 0, 0);
	if (scene.object_visibility(1))
		return fail("context menu toggles visibility");

	vgui_destroy(ui);
	return 0;
}

/* A01: 列表/网格双视图共享选择、键盘/框选、100k 虚拟目录只取可见项。 */
static int test_asset_browser_views()
{
	using namespace vgui_editor_kit;
	vgui_context* ui = vgui_create(nullptr, 16.0f);
	if (!ui)
		return fail("asset views context");
	vgui_context_set_size(ui, 1000, 700);
	vgui_style style;
	vgui_style_default(&style);
	style.list.row_height = 24.0f;
	vgui_context_set_style(ui, &style);

	vgui_texture_desc thumb_desc = {};
	thumb_desc.width = 2;
	thumb_desc.height = 2;
	thumb_desc.format = VGUI_TEXTURE_FORMAT_RGBA8;
	unsigned char thumb_pixels[16] = { 255, 80, 40, 255, 40, 160, 255, 255,
		40, 160, 255, 255, 255, 80, 40, 255 };
	thumb_desc.pixels = thumb_pixels;
	vgui_texture* thumb = vgui_texture_create(ui, &thumb_desc);
	if (!thumb)
		return fail("asset thumbnail texture");

	{
		asset_browser assets;
		assets.build(vgui_root(ui));
		assets.enable_row_drag();
		vgui_widget_set_pos(assets.root(), 20.0f, 20.0f);
		vgui_widget_set_size(assets.root(), 420.0f, 320.0f);
		vgui_update(ui, 0.016f);
		vgui_layout(ui);

		const vgui_item_id a = assets.add_item("Cube.mesh", "Mesh");
		const vgui_item_id b = assets.add_item("Sphere.mesh", "Mesh");
		const vgui_item_id c = assets.add_item("Ghost.mesh", "Mesh");
		const vgui_item_id d = assets.add_item("icon.png", "Texture");
		if (!a || !b || !c || !d || a == b || b == c || c == d)
			return fail("asset view items");
		if (!assets.set_item_thumbnail(b, thumb) ||
			assets.item_thumbnail(b) != thumb)
			return fail("asset thumbnail set");

		asset_info info;
		if (!assets.item_at(1, &info) || info.name != "Sphere.mesh" ||
			info.type != "Mesh" || info.thumbnail != thumb ||
			info.id != b)
			return fail("asset metadata");
		if (vgui_list_get_item_icon(assets.widget(), b) != thumb)
			return fail("asset list row thumbnail");

		/* 视图自动开关存在;默认列表,切到网格后选择不变。 */
		if (assets.view() != asset_browser::view_mode::list ||
			!assets.view_switch() || !assets.view_switch_visible() ||
			assets.view_switch()->selected() != 0)
			return fail("asset default list view");
		assets.set_selection({ a, b });
		if (assets.selected() != a || assets.selection().size() != 2)
			return fail("asset initial selection");
		assets.set_view(asset_browser::view_mode::grid);
		vgui_update(ui, 0.016f);
		vgui_layout(ui);
		if (assets.view() != asset_browser::view_mode::grid ||
			assets.view_switch()->selected() != 1 ||
			assets.selection().size() != 2 || !assets.is_selected(b) ||
			!vgui_widget_get_visible(assets.grid()) ||
			vgui_widget_get_visible(assets.widget()))
			return fail("asset switch to grid");
		if (assets.grid_columns() < 1 || assets.visible_count() != 4)
			return fail("asset grid metrics");

		assets.set_view(asset_browser::view_mode::list);
		if (assets.selection().size() != 2 || !assets.is_selected(b) ||
			!vgui_widget_get_visible(assets.widget()))
			return fail("asset switch back keeps selection");
		assets.set_view_switch_visible(false);
		if (assets.view_switch_visible())
			return fail("asset view switch hide");
		assets.set_view_switch_visible(true);
		if (!assets.view_switch_visible())
			return fail("asset view switch show");
		assets.set_view(asset_browser::view_mode::grid);
		vgui_update(ui, 0.016f);
		vgui_layout(ui);

		/* 键盘:方向键/I 光标、Shift 扩展、Ctrl+Space 反选、Home/End、Ctrl+A。 */
		assets.set_selection({ a });
		vgui_set_focus(ui, assets.grid());
		if (vgui_get_focus(ui) != assets.grid())
			return fail("asset grid focus");
		vgui_input_key(ui, VGUI_KEY_RIGHT, 1, 0);
		vgui_input_key(ui, VGUI_KEY_RIGHT, 0, 0);
		if (assets.cursor() != b || assets.selected() != b ||
			assets.selection().size() != 1)
			return fail("asset grid arrow selection");
		vgui_input_key(ui, VGUI_KEY_RIGHT, 1, VGUI_MOD_SHIFT);
		vgui_input_key(ui, VGUI_KEY_RIGHT, 0, VGUI_MOD_SHIFT);
		if (assets.selection().size() != 2 || !assets.is_selected(b) ||
			!assets.is_selected(c) || assets.selected() != c)
			return fail("asset grid shift range");
		vgui_input_key(ui, ' ', 1, VGUI_MOD_CTRL);
		vgui_input_key(ui, ' ', 0, VGUI_MOD_CTRL);
		if (assets.selection().size() != 1 || !assets.is_selected(b) ||
			assets.selected() != b)
			return fail("asset grid ctrl toggle");
		vgui_input_key(ui, VGUI_KEY_END, 1, 0);
		vgui_input_key(ui, VGUI_KEY_END, 0, 0);
		if (assets.cursor() != d || assets.selected() != d)
			return fail("asset grid end key");
		vgui_input_key(ui, 'A', 1, VGUI_MOD_CTRL);
		vgui_input_key(ui, 'A', 0, VGUI_MOD_CTRL);
		if (assets.selection().size() != 4 || !assets.is_selected(a) ||
			!assets.is_selected(d))
			return fail("asset grid select all");
		assets.set_view(asset_browser::view_mode::list);
		if (!vgui_list_is_selected(assets.widget(), d) ||
			assets.selection().size() != 4)
			return fail("asset list mirrors grid selection");
		assets.set_view(asset_browser::view_mode::grid);
		vgui_update(ui, 0.016f);
		vgui_layout(ui);
		vgui_panel_set_scroll(assets.scroll(), 0.0f, 0.0f);
		vgui_update(ui, 0.016f);
		vgui_layout(ui);

		/* 框选:从留白拖到第 3 个单元格,只选中可见的 3 项。 */
		assets.clear_selection();
		float gx = 0, gy = 0, gw = 0, gh = 0;
		vgui_widget_get_rect(assets.grid(), &gx, &gy, &gw, &gh);
		(void)gw; (void)gh;
		float c0x = 0, c0y = 0, c0w = 0, c0h = 0;
		float c2x = 0, c2y = 0, c2w = 0, c2h = 0;
		if (!assets.item_rect(0, &c0x, &c0y, &c0w, &c0h) ||
			!assets.item_rect(2, &c2x, &c2y, &c2w, &c2h))
			return fail("asset grid cell rects");
		(void)c0x; (void)c0y; (void)c0w; (void)c0h;
		vgui_input_mouse_move(ui, gx + 2.0f, gy + 2.0f);
		vgui_input_mouse_down(ui, 1, gx + 2.0f, gy + 2.0f, 1);
		vgui_input_mouse_move(ui, c2x + c2w * 0.5f, c2y + c2h * 0.5f);
		vgui_input_mouse_up(ui, 1, c2x + c2w * 0.5f, c2y + c2h * 0.5f, 1);
		if (assets.selection().size() != 3 || !assets.is_selected(a) ||
			!assets.is_selected(b) || !assets.is_selected(c))
			return fail("asset grid marquee");

		/* 单元格拖放与列表共用 asset payload。 */
		if (!assets.item_rect(0, &c0x, &c0y, &c0w, &c0h))
			return fail("asset drag cell rect");
		const float press_x = c0x + c0w * 0.5f;
		const float press_y = c0y + c0h * 0.5f;
		vgui_input_mouse_move(ui, press_x, press_y);
		vgui_input_mouse_down(ui, 1, press_x, press_y, 1);
		vgui_input_mouse_move(ui, press_x + 20.0f, press_y + 10.0f);
		if (!vgui_drag_active(ui))
			return fail("asset grid drag begins");
		vgui_input_mouse_up(ui, 1, press_x + 20.0f, press_y + 10.0f, 1);

		/* 100k 虚拟目录:滚动到中段后仅可见范围取元数据。 */
		struct virtual_assets
		{
			int count = 100000;
			int item_calls = 0;
		};
		virtual_assets va;
		asset_browser::asset_source source;
		source.count = [](void* user) {
			return static_cast<virtual_assets*>(user)->count;
		};
		source.item_at = [](void* user, int index, asset_info* out) -> int {
			auto* model = static_cast<virtual_assets*>(user);
			++model->item_calls;
			if (!out || index < 0 || index >= model->count)
				return 0;
			out->name = "Asset " + std::to_string(index);
			out->type = (index & 1) ? "Texture" : "Mesh";
			out->thumbnail = nullptr;
			out->id = 0;
			return 1;
		};
		source.user = &va;
		assets.set_virtual_source(&source);
		if (!assets.is_virtual() || assets.item_count() != 100000 ||
			assets.index_of(90000) != 89999)
			return fail("asset virtual source");
		assets.set_selection({ 90000 });
		if (assets.selected() != 90000 || !assets.is_selected(90000))
			return fail("asset virtual selection");
		if (!assets.set_item_thumbnail(90000, thumb) ||
			assets.item_thumbnail(90000) != thumb)
			return fail("asset virtual thumbnail");
		assets.set_view(asset_browser::view_mode::list);
		vgui_update(ui, 0.016f);
		if (!vgui_list_is_selected(assets.widget(), 90000))
			return fail("asset virtual list selection");
		assets.set_view(asset_browser::view_mode::grid);
		vgui_update(ui, 0.016f);
		vgui_layout(ui);
		if (assets.visible_first() < 80000 || assets.visible_last() > 100000 ||
			assets.visible_count() <= 0 || assets.visible_count() > 200)
			return fail("asset virtual visible range");

		va.item_calls = 0;
		const std::vector<vgui_item_id> visible = assets.visible_items();
		const int before_loop = va.item_calls;
		if ((int)visible.size() != assets.visible_count())
			return fail("asset virtual visible items");
		std::size_t matched = 0;
		for (vgui_item_id id : visible)
		{
			const int index = assets.index_of(id);
			asset_info item;
			if (index >= 0 && assets.item_at(index, &item) &&
				item.id == id && item.name == "Asset " + std::to_string(index))
				++matched;
		}
		if (matched != visible.size() ||
			va.item_calls - before_loop != (int)visible.size() ||
			va.item_calls > 256)
			return fail("asset virtual lazy metadata");
		asset_info near_end;
		if (!assets.item_at(89999, &near_end) ||
			near_end.name != "Asset 89999")
			return fail("asset virtual item lookup");
		if (assets.item_at(-1, &near_end) || assets.item_at(100000, &near_end))
			return fail("asset virtual bounds");

		assets.clear();
		if (assets.is_virtual() || assets.item_count() != 0 ||
			!assets.selection().empty())
			return fail("asset virtual detach");

		/* 自定义 id_at/index_of：宿主稳定 ID 不依赖行号,列表与网格仍同步。 */
		struct custom_assets
		{
			int count = 1000;
		};
		custom_assets ca;
		asset_browser::asset_source custom;
		custom.count = [](void* user) {
			return static_cast<custom_assets*>(user)->count;
		};
		custom.item_at = [](void* user, int index, asset_info* out) -> int {
			auto* model = static_cast<custom_assets*>(user);
			if (!out || index < 0 || index >= model->count)
				return 0;
			out->name = "Custom " + std::to_string(index);
			out->type = "Mesh";
			out->thumbnail = nullptr;
			out->id = 0;
			return 1;
		};
		custom.id_at = [](void*, int index) -> vgui_item_id {
			return (vgui_item_id)(10000 + index);
		};
		custom.index_of = [](void*, vgui_item_id item) -> int {
			return item >= 10000 && item < 11000 ? (int)(item - 10000) : -1;
		};
		custom.user = &ca;
		assets.set_virtual_source(&custom);
		asset_info custom_info;
		if (!assets.is_virtual() || assets.id_at(5) != 10005 ||
			assets.index_of(10005) != 5 || assets.index_of(9999) != -1 ||
			!assets.item_at(5, &custom_info) || custom_info.id != 10005 ||
			custom_info.name != "Custom 5")
			return fail("asset virtual custom ids");
		assets.set_selection({ 10005 });
		if (assets.selected() != 10005 || !assets.is_selected(10005))
			return fail("asset virtual custom selection");
		assets.set_view(asset_browser::view_mode::list);
		if (!vgui_list_is_selected(assets.widget(), 10005))
			return fail("asset virtual custom list selection");
		assets.set_view(asset_browser::view_mode::grid);
		vgui_update(ui, 0.016f);
		vgui_layout(ui);
		if (assets.selection().size() != 1 || !assets.is_selected(10005))
			return fail("asset virtual custom grid selection");
	}

	vgui_texture_destroy(thumb);
	vgui_destroy(ui);
	return 0;
}

/* A02: breadcrumb 路径/前进后退、回到目录后的选择与滚动恢复、
 * 防抖搜索/类型过滤/空状态,以及宿主目录提供者。 */
static int test_asset_browser_navigation()
{
	using namespace vgui_editor_kit;
	vgui_context* ui = vgui_create(nullptr, 16.0f);
	if (!ui)
		return fail("asset navigation context");
	vgui_context_set_size(ui, 1000, 700);
	vgui_style style;
	vgui_style_default(&style);
	style.list.row_height = 24.0f;
	vgui_context_set_style(ui, &style);

	std::map<std::string, asset_browser::asset_directory> directories;
	{
		asset_browser::asset_directory root;
		root.path.push_back({ "Root", "root" });
		root.items.push_back({ "Models", "Folder", nullptr, 101 });
		root.items.push_back({ "Textures", "Folder", nullptr, 102 });
		root.items.push_back({ "Empty", "Folder", nullptr, 103 });
		directories["root"] = root;

		asset_browser::asset_directory models;
		models.path.push_back({ "Root", "root" });
		models.path.push_back({ "Models", "models" });
		for (int i = 0; i < 40; ++i)
		{
			asset_info info;
			info.name = "Model " + std::to_string(i);
			info.type = i < 32 ? "Mesh" : "Texture";
			info.id = (vgui_item_id)(2000 + i);
			models.items.push_back(info);
		}
		directories["models"] = models;

		asset_browser::asset_directory textures;
		textures.path.push_back({ "Root", "root" });
		textures.path.push_back({ "Textures", "textures" });
		for (int i = 0; i < 3; ++i)
		{
			asset_info info;
			info.name = "Tex " + std::to_string(i) + ".png";
			info.type = "Texture";
			info.id = (vgui_item_id)(3000 + i);
			textures.items.push_back(info);
		}
		directories["textures"] = textures;

		asset_browser::asset_directory empty;
		empty.path.push_back({ "Root", "root" });
		empty.path.push_back({ "Empty", "empty" });
		empty.empty_text = "Nothing here";
		directories["empty"] = empty;

		asset_browser::asset_directory bare;
		bare.path.push_back({ "Root", "root" });
		bare.path.push_back({ "Bare", "bare" });
		directories["bare"] = bare;
	}

	{
		asset_browser assets;
		assets.build(vgui_root(ui));
		vgui_widget_set_pos(assets.root(), 20.0f, 20.0f);
		vgui_widget_set_size(assets.root(), 460.0f, 360.0f);
		vgui_update(ui, 0.016f);
		vgui_layout(ui);

		std::vector<std::string> resolved;
		assets.set_navigate_handler([&](const std::string& path_id,
			asset_browser::asset_directory* out) {
			resolved.push_back(path_id);
			auto found = directories.find(path_id);
			if (found == directories.end() || !out)
				return false;
			*out = found->second;
			return true;
		});
		std::vector<std::string> visited;
		assets.set_path_listener([&](const std::string& path_id) {
			visited.push_back(path_id);
		});
		assets.set_type_options({ "Mesh", "Texture" });
		assets.set_search_debounce(0.25f);
		if (!assets.navigation_visible() || !assets.type_filter_visible() ||
			assets.type_options().size() != 2 || !assets.path() ||
			!assets.search() || !assets.type_combo() ||
			assets.search_debounce() != 0.25f)
			return fail("asset navigation widgets");

		if (!assets.navigate_to("root") || assets.current_path() != "root" ||
			assets.item_count() != 3 || assets.total_item_count() != 3 ||
			!assets.path() || assets.path()->count() != 1 ||
			assets.path()->item(0).label != "Root" ||
			assets.path()->item(0).id != "root" || visited.size() != 1 ||
			visited[0] != "root" || assets.can_go_back() ||
			assets.can_go_forward() || assets.selected() != 0)
			return fail("asset navigate initial directory");
		if (assets.navigate_to("missing") ||
			assets.current_path() != "root" || assets.item_count() != 3)
			return fail("asset navigate rejection");
		if (assets.type_combo()->value() != "" ||
			assets.type_combo()->display_text() != "All types")
			return fail("asset type combo default");

		if (!assets.navigate_to("models") ||
			assets.current_path() != "models" || assets.item_count() != 40 ||
			assets.path()->count() != 2 ||
			assets.path()->item(1).label != "Models" ||
			assets.path()->active() != 1 ||
			!assets.can_go_back() || assets.can_go_forward())
			return fail("asset navigate models");

		/* 目录内选择 + 列表滚动应成为 models 的历史快照。 */
		assets.set_selection({ 2003, 2004 });
		vgui_panel_set_scroll(assets.widget(), 0.0f, 96.0f);
		if (assets.selected() != 2003 || assets.selection().size() != 2)
			return fail("asset models selection");
		if (!assets.navigate_to("textures") ||
			assets.current_path() != "textures" || assets.item_count() != 3 ||
			!assets.selection().empty() || !assets.can_go_back() ||
			assets.path()->count() != 2)
			return fail("asset navigate textures");
		if (!assets.go_back() || assets.current_path() != "models" ||
			assets.selection().size() != 2 || !assets.is_selected(2003) ||
			!assets.is_selected(2004) || assets.selected() != 2003 ||
			assets.cursor() != 2003 || !assets.can_go_forward())
			return fail("asset back restores models selection");
		float scroll_x = 0.0f, scroll_y = 0.0f;
		vgui_panel_get_scroll(assets.widget(), &scroll_x, &scroll_y);
		if (scroll_y < 95.0f || scroll_y > 97.0f)
			return fail("asset back restores list scroll");

		/* 前进后返回仍应恢复同一快照;面包屑点击也走同一导航路径。 */
		if (!assets.go_forward() || assets.current_path() != "textures" ||
			assets.can_go_forward() || assets.selection().size() != 0)
			return fail("asset forward textures");
		if (!assets.go_back() || assets.current_path() != "models" ||
			assets.selection().size() != 2)
			return fail("asset back after forward");
		vgui_widget* root_crumb = assets.path()->item_widget(0);
		float crumb_x = 0.0f, crumb_y = 0.0f;
		float crumb_w = 0.0f, crumb_h = 0.0f;
		if (!root_crumb)
			return fail("asset breadcrumb root widget");
		vgui_widget_get_rect(root_crumb, &crumb_x, &crumb_y,
			&crumb_w, &crumb_h);
		if (crumb_w <= 0.0f || crumb_h <= 0.0f)
			return fail("asset breadcrumb root rect");
		const float crumb_press_x = crumb_x + crumb_w * 0.5f;
		const float crumb_press_y = crumb_y + crumb_h * 0.5f;
		vgui_input_mouse_move(ui, crumb_press_x, crumb_press_y);
		vgui_input_mouse_down(ui, 1, crumb_press_x, crumb_press_y, 1);
		vgui_input_mouse_up(ui, 1, crumb_press_x, crumb_press_y, 1);
		if (assets.current_path() != "root" || assets.item_count() != 3)
			return fail("asset breadcrumb navigate root");
		if (!assets.go_back() || assets.current_path() != "models" ||
			assets.selection().size() != 2 ||
			assets.selected() != 2003)
			return fail("asset breadcrumb history");
		vgui_panel_get_scroll(assets.widget(), &scroll_x, &scroll_y);
		if (scroll_y < 95.0f || scroll_y > 97.0f)
			return fail("asset breadcrumb back restores scroll");

		/* 网格滚动同样随历史快照恢复。 */
		assets.set_view(asset_browser::view_mode::grid);
		vgui_update(ui, 0.016f);
		vgui_layout(ui);
		vgui_panel_set_scroll(assets.scroll(), 0.0f, 420.0f);
		if (!assets.navigate_to("textures") || !assets.go_back() ||
			assets.current_path() != "models" ||
			assets.selection().size() != 2)
			return fail("asset grid history navigation");
		vgui_panel_get_scroll(assets.scroll(), &scroll_x, &scroll_y);
		if (scroll_y < 419.0f || scroll_y > 421.0f)
			return fail("asset back restores grid scroll");
		assets.set_view(asset_browser::view_mode::list);
		if (!vgui_widget_get_visible(assets.widget()) ||
			assets.selection().size() != 2 || assets.selected() != 2003)
			return fail("asset list selection after history");

		/* 激活回调供宿主打开文件夹/预览。 */
		vgui_item_id activated = 0;
		assets.set_activate_listener([&](vgui_item_id item) {
			activated = item;
		});
		assets.activate_selected();
		if (activated != 2003)
			return fail("asset activate selection");

		/* 防抖:输入后先保留旧结果,tick 到阈值才过滤。 */
		assets.set_search_text("model 1");
		if (!assets.search_pending() || assets.search_text() != "model 1" ||
			assets.item_count() != 40 || assets.filter_active())
			return fail("asset search pending");
		assets.tick(0.1f);
		if (!assets.search_pending() || assets.item_count() != 40)
			return fail("asset search debounce early");
		assets.tick(0.2f);
		if (assets.search_pending() || !assets.filter_active() ||
			assets.item_count() != 11 || assets.total_item_count() != 40)
			return fail("asset search debounce apply");
		for (int i = 0; i < assets.item_count(); ++i)
		{
			asset_info info;
			if (!assets.item_at(i, &info) ||
				info.name.find("Model 1") == std::string::npos)
				return fail("asset search filtered names");
		}
		assets.set_search_text("MODEL 1", true);
		if (assets.item_count() != 11 || assets.search_pending())
			return fail("asset search immediate case insensitive");
		assets.set_search_text("", true);
		if (assets.item_count() != 40 || assets.filter_active())
			return fail("asset search clear");

		/* 类型过滤与搜索叠加;全隐藏时显示 No matches。 */
		assets.set_type_filter("Mesh");
		if (assets.type_filter() != "Mesh" || assets.item_count() != 32 ||
			assets.type_combo()->value() != "Mesh")
			return fail("asset type filter mesh");
		assets.set_search_text("zzz", true);
		if (assets.item_count() != 0 ||
			assets.empty_state_text() != "No matches")
			return fail("asset filter empty state");
		assets.set_search_text("", true);
		if (assets.item_count() != 32)
			return fail("asset search clears under type filter");
		assets.set_type_filter("Texture");
		if (assets.item_count() != 8 || assets.type_combo()->value() != "Texture")
			return fail("asset type filter texture");
		assets.set_type_filter("");
		if (assets.item_count() != 40 || assets.filter_active())
			return fail("asset type filter clear");
		assets.set_type_filter_visible(false);
		if (assets.type_filter_visible() ||
			vgui_widget_get_visible(assets.type_combo()->widget()))
			return fail("asset type filter hide");
		assets.set_type_filter_visible(true);
		assets.set_type_options({ "Texture", "Mesh" });
		if (!vgui_widget_get_visible(assets.type_combo()->widget()) ||
			assets.type_options().size() != 2)
			return fail("asset type filter show");

		/* reload 不影响历史栈,并尽量保留选择/滚动。 */
		assets.set_selection({ 2007 });
		vgui_panel_set_scroll(assets.widget(), 0.0f, 48.0f);
		const bool reload_back = assets.can_go_back();
		const bool reload_forward = assets.can_go_forward();
		if (!assets.reload() || assets.current_path() != "models" ||
			assets.selection().size() != 1 || !assets.is_selected(2007) ||
			assets.can_go_back() != reload_back ||
			assets.can_go_forward() != reload_forward)
			return fail("asset reload restores state");
		vgui_panel_get_scroll(assets.widget(), &scroll_x, &scroll_y);
		if (scroll_y < 47.0f || scroll_y > 49.0f)
			return fail("asset reload restores list scroll");

		/* 空目录:目录专属文案优先,未提供时使用全局默认。 */
		assets.set_empty_texts("Global empty", "Global no match");
		if (!assets.navigate_to("bare") || assets.current_path() != "bare" ||
			assets.item_count() != 0 || assets.total_item_count() != 0 ||
			assets.empty_state_text() != "Global empty" ||
			!assets.selection().empty() ||
			assets.path()->item(1).id != "bare")
			return fail("asset empty default text");
		if (!assets.navigate_to("empty") || assets.current_path() != "empty" ||
			assets.item_count() != 0 ||
			assets.empty_state_text() != "Nothing here")
			return fail("asset empty directory");
	}

	vgui_destroy(ui);
	return 0;
}

/* A03: 选中资产预览的异步加载、失败占位、陈旧结果丢弃与 shell 自动同步。 */
static int test_asset_preview()
{
	using namespace vgui_editor_kit;
	vgui_context* ui = vgui_create(nullptr, 16.0f);
	if (!ui)
		return fail("a03 vgui_create failed");
	vgui_context_set_size(ui, 480, 600);

	{
		vgui_widget* root = vgui_root(ui);
		vgui_widget_set_layout(root, VGUI_LAYOUT_COLUMN);
		vgui_widget_set_align_items(root, VGUI_ALIGN_STRETCH);

		asset_preview preview;
		preview.build(root);
		if (!preview.widget() || !preview.stage())
			return fail("a03 preview build");
		vgui_update(ui, 1.0f / 60.0f);
		vgui_layout(ui);

		float stage_w = 0, stage_h = 0;
		vgui_widget_get_rect(preview.stage(), nullptr, nullptr, &stage_w, &stage_h);
		if (stage_w <= 0.0f || stage_h <= 0.0f)
			return fail("a03 preview layout");
		if (preview.item() != 0 || preview.loading() ||
			preview.status_text() != "Select an asset to preview")
			return fail("a03 preview initial state");

		std::atomic<int> loads{ 0 };
		preview.set_load_handler([&](const asset_preview::request& req,
			asset_preview::data* out)
		{
			loads.fetch_add(1);
			if (req.id == 1 || req.id == 4)
			{
				/* Simulate a large file decode. */
				std::this_thread::sleep_for(std::chrono::milliseconds(200));
				out->kind = asset_preview::preview_kind::image;
				out->name = req.name;
				out->type = req.type;
				out->detail = "RGBA8 texture";
				out->image.width = 8;
				out->image.height = 6;
				out->image.stride = 0;
				out->image.format = VGUI_TEXTURE_FORMAT_RGBA8;
				out->image.pixels.assign(8u * 6u * 4u, 0xC0u);
				out->fields.push_back({ "Size", "8 x 6" });
				return true;
			}
			if (req.id == 2)
			{
				out->name = req.name;
				out->type = req.type;
				out->error = "missing file";
				return false;
			}
			if (req.id == 3)
			{
				out->kind = asset_preview::preview_kind::text;
				out->name = req.name;
				out->type = req.type;
				out->text = "slow result";
				return true;
			}
			return true;
		});

		const auto start = std::chrono::steady_clock::now();
		preview.set_item(1, "hero.png", "Texture");
		const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - start).count();
		if (elapsed > 100)
			return fail("a03 set_item blocks on loader");
		if (!preview.loading() || preview.item() != 1)
			return fail("a03 preview loading state");

		/* Switch immediately: the older 200 ms job must not win. */
		preview.set_item(3, "notes.txt", "Text");
		if (preview.item() != 3 || !preview.loading())
			return fail("a03 rapid selection");

		auto wait_applied = [&preview](int timeout_ms) {
			const auto deadline = std::chrono::steady_clock::now() +
				std::chrono::milliseconds(timeout_ms);
			while (std::chrono::steady_clock::now() < deadline)
			{
				if (preview.poll())
					return true;
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			return preview.poll();
		};

		if (!wait_applied(2500))
			return fail("a03 async result poll");
		if (preview.loading() || preview.item() != 3 ||
			preview.current_data().kind != asset_preview::preview_kind::text ||
			preview.current_data().name != "notes.txt" ||
			preview.current_data().text != "slow result")
			return fail("a03 stale result discarded");

		preview.set_item(2, "Broken.mesh", "Mesh");
		if (!wait_applied(1500))
			return fail("a03 failure result poll");
		if (!preview.has_error() || preview.error() != "missing file" ||
			preview.current_data().error != "missing file" ||
			preview.texture() != nullptr ||
			preview.status_text().find("Preview failed") == std::string::npos)
			return fail("a03 failure placeholder state");

		preview.set_item(4, "icon.png", "Texture");
		if (!wait_applied(1500))
			return fail("a03 image result poll");
		if (preview.has_error() || !preview.texture() ||
			vgui_texture_get_width(preview.texture()) != 8 ||
			vgui_texture_get_height(preview.texture()) != 6 ||
			preview.current_data().kind != asset_preview::preview_kind::image)
			return fail("a03 image texture");

		preview.clear();
		if (preview.item() != 0 || preview.loading() || preview.has_error() ||
			preview.texture() != nullptr ||
			preview.status_text() != "Select an asset to preview")
			return fail("a03 preview clear");

		preview.set_texts("Empty!", "Loading!", "Failed!");
		preview.set_load_handler({});
		preview.set_item(5, "Docs", "Folder");
		if (preview.loading() || preview.has_error() ||
			preview.item() != 5 ||
			preview.status_text() != "No preview available")
			return fail("a03 no loader placeholder");
		preview.clear();
		if (preview.status_text() != "Empty!")
			return fail("a03 custom empty text");

		/* A03 also relies on asset_browser's multi-listener API and on item
		 * lookup surviving the search/type filter. */
		vgui_widget* browser_host = vgui_widget_add(root, VGUI_PANEL, nullptr);
		vgui_widget_set_layout(browser_host, VGUI_LAYOUT_COLUMN);
		vgui_widget_set_size(browser_host, 320, 220);
		asset_browser browser;
		browser.build(browser_host);
		vgui_item_id alpha = browser.add_item("Alpha.mesh", "Mesh");
		vgui_item_id beta = browser.add_item("Beta.png", "Texture");
		asset_info info;
		if (!alpha || !beta || !browser.item_info(alpha, &info) ||
			info.name != "Alpha.mesh" || info.type != "Mesh")
			return fail("a03 asset item_info");
		int first_hits = 0;
		int second_hits = 0;
		const auto first_id = browser.add_selection_listener(
			[&first_hits](vgui_item_id) { ++first_hits; });
		const auto second_id = browser.add_selection_listener(
			[&second_hits](vgui_item_id) { ++second_hits; });
		if (!first_id || !second_id || first_id == second_id)
			return fail("a03 listener ids");
		browser.set_selection({ alpha });
		if (first_hits != 1 || second_hits != 1)
			return fail("a03 multi selection listeners");
		if (!browser.remove_selection_listener(first_id) ||
			browser.remove_selection_listener(first_id))
			return fail("a03 remove selection listener");
		browser.set_selection({ beta });
		if (first_hits != 1 || second_hits != 2)
			return fail("a03 removed listener stops");
		browser.set_search_text("no-such-asset", true);
		if (browser.index_of(alpha) != -1 ||
			!browser.item_info(alpha, &info) || info.name != "Alpha.mesh")
			return fail("a03 item_info ignores filter");
	}

	vgui_destroy(ui);

	vgui_context* shell_ui = vgui_create(nullptr, 16.0f);
	if (!shell_ui)
		return fail("a03 shell vgui_create failed");
	vgui_context_set_size(shell_ui, 1280, 800);
	{
		std::atomic<int> loads{ 0 };
		editor_context context(shell_ui);
		editor_shell shell(context);
		shell.build();
		shell.preview().set_load_handler([&](const asset_preview::request& req,
			asset_preview::data* out)
		{
			loads.fetch_add(1);
			out->kind = asset_preview::preview_kind::model;
			out->name = req.name;
			out->type = req.type;
			out->detail = "Static mesh";
			return true;
		});
		vgui_item_id item = shell.assets().add_item("Hero.mesh", "Mesh");
		if (!item || !shell.preview().widget())
			return fail("a03 shell preview widget");
		shell.assets().set_selection({ item });
		if (shell.preview().item() != item)
			return fail("a03 shell preview selection sync");

		const auto deadline = std::chrono::steady_clock::now() +
			std::chrono::milliseconds(1500);
		while (shell.preview().loading() &&
			std::chrono::steady_clock::now() < deadline)
		{
			shell.preview().poll();
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		if (shell.preview().loading() || shell.preview().has_error() ||
			shell.preview().current_data().name != "Hero.mesh" ||
			shell.preview().current_data().kind !=
				asset_preview::preview_kind::model ||
			loads.load() < 1)
			return fail("a03 shell preview async sync");

		vgui_update(shell_ui, 1.0f / 60.0f);
		vgui_layout(shell_ui);
		float pw = 0, ph = 0;
		vgui_widget_get_rect(shell.preview().widget(), nullptr, nullptr, &pw, &ph);
		if (pw <= 0.0f || ph <= 0.0f)
			return fail("a03 shell preview layout");
	}

	vgui_destroy(shell_ui);
	return 0;
}

/* A04: 资产拖放三条路径的落点反馈、拒绝语义、可撤销结果与 OS 文件导入。 */
static int test_asset_drop_paths()
{
	using namespace vgui_editor_kit;
	vgui_context* ui = vgui_create(nullptr, 16.0f);
	if (!ui)
		return fail("a04 context");
	vgui_context_set_size(ui, 900, 650);
	vgui_style style;
	vgui_style_default(&style);
	style.tree.row_height = 24.0f;
	vgui_context_set_style(ui, &style);

	{
		editor_context context(ui);
		command_stack commands;
		drag_payload asset;
		asset.kind = drag_payload_kind::asset;
		asset.id = "Cube.mesh";

		/* 1) Viewport:光标处 RECT 反馈,接受后记录落点;拒绝不覆盖上次结果。 */
		viewport_view viewport;
		viewport.build(vgui_root(ui));
		vgui_widget_set_pos(viewport.widget(), 10.0f, 10.0f);
		vgui_widget_set_size(viewport.widget(), 300.0f, 180.0f);
		int viewport_drops = 0;
		std::string viewport_asset;
		viewport.set_drop_listener([&](const drag_payload& payload, float, float)
		{
			++viewport_drops;
			viewport_asset = payload.id;
			return true;
		});
		vgui_update(ui, 0.016f);
		vgui_layout(ui);

		float vx = 0, vy = 0, vw = 0, vh = 0;
		vgui_widget_get_rect(viewport.widget(), &vx, &vy, &vw, &vh);
		const float vcx = vx + vw * 0.5f;
		const float vcy = vy + vh * 0.5f;
		if (!begin_editor_drag(ui, viewport.widget(), asset))
			return fail("a04 viewport drag begins");
		vgui_input_mouse_move(ui, vcx, vcy);
		vgui_drop_hint hint = {};
		if (!vgui_drag_hint(ui, &hint) || hint.kind != VGUI_DROP_HINT_RECT ||
			hint.w <= 0.0f || hint.h <= 0.0f)
			return fail("a04 viewport hover feedback");
		if (std::fabs(hint.x + hint.w * 0.5f - vcx) > 1.5f ||
			std::fabs(hint.y + hint.h * 0.5f - vcy) > 1.5f)
			return fail("a04 viewport hint follows cursor");
		vgui_input_mouse_up(ui, 1, vcx, vcy, 1);
		if (viewport_drops != 1 || viewport_asset != "Cube.mesh" ||
			viewport.last_drop().id != "Cube.mesh" ||
			std::fabs(viewport.last_drop_x() - vcx) > 1.0f ||
			std::fabs(viewport.last_drop_y() - vcy) > 1.0f)
			return fail("a04 viewport accepts drop");
		if (vgui_drag_hint(ui, &hint) != 0 ||
			hint.kind != VGUI_DROP_HINT_NONE)
			return fail("a04 drag hint clears after drop");

		viewport.set_drop_listener([&](const drag_payload&, float, float)
		{
			++viewport_drops;
			return false;
		});
		drag_payload rejected = asset;
		rejected.id = "Sphere.mesh";
		if (viewport.record_drop(rejected, 1.0f, 2.0f))
			return fail("a04 viewport rejects payload");
		if (viewport.last_drop().id != "Cube.mesh" || viewport_drops != 2)
			return fail("a04 viewport keeps last accepted");

		/* 2) Outliner:资产落到行中部成为子对象,落到空白成为根;hover 同步。 */
		outliner_view outliner;
		outliner.build(context, vgui_root(ui));
		vgui_widget_set_pos(outliner.widget(), 10.0f, 210.0f);
		vgui_widget_set_size(outliner.widget(), 300.0f, 240.0f);
		const vgui_item_id root = outliner.add_node("Root", 0, 1);
		outliner.add_node("Child", root, 2);
		outliner.expand(root);
		int asset_drops = 0;
		std::string outliner_asset;
		object_id outliner_parent = invalid_object;
		std::size_t outliner_index = 0;
		outliner.set_asset_drop_handler([&](const std::string& name,
			object_id parent, std::size_t index)
		{
			++asset_drops;
			outliner_asset = name;
			outliner_parent = parent;
			outliner_index = index;
			return true;
		});
		vgui_update(ui, 0.016f);
		vgui_layout(ui);

		float tx = 0, ty = 0, tw = 0, th = 0;
		vgui_widget_get_rect(outliner.widget(), &tx, &ty, &tw, &th);
		(void)tw;
		(void)th;
		const float row_h = style.tree.row_height;
		const float root_mid = ty + style.tree.padding_y + row_h * 0.5f;
		if (!begin_editor_drag(ui, outliner.widget(), asset))
			return fail("a04 outliner drag begins");
		vgui_input_mouse_move(ui, tx + 30.0f, root_mid);
		if (!vgui_drag_hint(ui, &hint) || hint.kind != VGUI_DROP_HINT_RECT)
			return fail("a04 outliner hover feedback");
		vgui_input_mouse_up(ui, 1, tx + 30.0f, root_mid, 1);
		if (asset_drops != 1 || outliner_asset != "Cube.mesh" ||
			outliner_parent != 1 || outliner_index != 1)
			return fail("a04 outliner parents asset");

		const float roots_y = ty + style.tree.padding_y + row_h * 5.0f;
		if (!begin_editor_drag(ui, outliner.widget(), asset))
			return fail("a04 outliner roots drag begins");
		vgui_input_mouse_move(ui, tx + 30.0f, roots_y);
		if (!vgui_drag_hint(ui, &hint) || hint.kind != VGUI_DROP_HINT_LINE)
			return fail("a04 outliner roots feedback");
		vgui_input_mouse_up(ui, 1, tx + 30.0f, roots_y, 1);
		if (asset_drops != 2 || outliner_parent != invalid_object ||
			outliner_index != 1)
			return fail("a04 outliner roots drop");

		/* 3) PropertyGrid:整行 RECT 高亮;drop 走命令栈,undo/redo 往返。 */
		property_grid grid;
		grid.build(vgui_root(ui));
		vgui_widget_set_pos(grid.widget(), 330.0f, 10.0f);
		vgui_widget_set_size(grid.widget(), 330.0f, 320.0f);
		grid.set_command_stack(&commands);
		grid.set_asset_query([](const std::string& id, const std::string&,
			property_grid::asset_info* out)
		{
			if (id != "Cube.mesh" && id != "Sphere.mesh")
				return false;
			out->name = id;
			out->kind = "Mesh";
			return true;
		});
		property_desc mesh;
		mesh.id = "mesh";
		mesh.label = "Mesh";
		mesh.type = property_type::asset;
		mesh.value = "Cube.mesh";
		mesh.asset_filter = "mesh";
		if (!grid.add_property(mesh))
			return fail("a04 property row");
		vgui_update(ui, 0.016f);
		vgui_layout(ui);

		vgui_widget* mesh_editor = grid.property_editor("mesh");
		if (!mesh_editor)
			return fail("a04 property editor");
		float ex = 0, ey = 0, ew = 0, eh = 0;
		vgui_widget_get_rect(mesh_editor, &ex, &ey, &ew, &eh);
		const float ecx = ex + ew * 0.5f;
		const float ecy = ey + eh * 0.5f;
		const std::size_t undo_base = commands.undo_count();
		asset.id = "Sphere.mesh";
		if (!begin_editor_drag(ui, grid.widget(), asset))
			return fail("a04 property drag begins");
		vgui_input_mouse_move(ui, ecx, ecy);
		if (!vgui_drag_hint(ui, &hint) || hint.kind != VGUI_DROP_HINT_RECT ||
			hint.h <= eh || hint.w <= ew)
			return fail("a04 property row feedback");
		vgui_input_mouse_up(ui, 1, ecx, ecy, 1);
		if (grid.property_value("mesh") != "Sphere.mesh" ||
			commands.undo_count() != undo_base + 1)
			return fail("a04 property drop assigns undoably");
		if (!commands.undo() || grid.property_value("mesh") != "Cube.mesh")
			return fail("a04 property drop undo");
		if (!commands.redo() || grid.property_value("mesh") != "Sphere.mesh")
			return fail("a04 property drop redo");
		if (!commands.undo())
			return fail("a04 property drop rewind");

		/* 4) AssetBrowser:宿主导入入口与 OS 文件拖入走同一条路径。 */
		asset_browser browser;
		browser.build(vgui_root(ui));
		vgui_widget_set_pos(browser.root(), 330.0f, 350.0f);
		vgui_widget_set_size(browser.root(), 340.0f, 250.0f);
		browser.set_navigate_handler([](const std::string& path_id,
			asset_browser::asset_directory* out)
		{
			if (path_id != "assets" || !out)
				return false;
			out->path.push_back({ "Assets", "assets" });
			out->items.push_back({ "Cube.mesh", "Mesh", nullptr, 1 });
			return true;
		});
		if (!browser.navigate_to("assets"))
			return fail("a04 browser navigate");
		int import_calls = 0;
		std::vector<std::string> last_files;
		std::string last_path;
		browser.set_import_handler([&](const std::vector<std::string>& files,
			const std::string& path_id)
		{
			++import_calls;
			last_files = files;
			last_path = path_id;
			return path_id == "assets" && !files.empty();
		});
		vgui_update(ui, 0.016f);
		vgui_layout(ui);

		if (!browser.import_files({ "C:/in/a.png", "C:/in/b.mesh" }) ||
			import_calls != 1 || last_files.size() != 2 ||
			last_path != "assets")
			return fail("a04 import entry");

		float bx = 0, by = 0, bw = 0, bh = 0;
		vgui_widget_get_rect(browser.root(), &bx, &by, &bw, &bh);
		const char* files[] = { "C:/drop/c.mat" };
		if (vgui_input_drop_files(ui, files, 1, bx + bw * 0.5f,
			by + bh * 0.3f) != 1)
			return fail("a04 os file drop");
		if (import_calls != 2 || last_files.size() != 1 ||
			last_files[0] != "C:/drop/c.mat" || last_path != "assets")
			return fail("a04 os file drop uses import entry");

		browser.set_import_handler([&](const std::vector<std::string>&,
			const std::string&)
		{
			++import_calls;
			return false;
		});
		if (vgui_input_drop_files(ui, files, 1, bx + bw * 0.5f,
			by + bh * 0.3f) != 0)
			return fail("a04 os file drop rejection");
	}

	vgui_destroy(ui);
	return 0;
}

/* A05: 异步缩略图加载、可见范围取消、失败重试与 LRU/磁盘缓存。 */
static int test_asset_thumbnails()
{
	using namespace vgui_editor_kit;
	vgui_context* ui = vgui_create(nullptr, 16.0f);
	if (!ui)
		return fail("a05 context");
	vgui_context_set_size(ui, 900, 650);
	vgui_style style;
	vgui_style_default(&style);
	style.list.row_height = 24.0f;
	vgui_context_set_style(ui, &style);

	auto fill = [](thumbnail_cache::image* img, unsigned char red,
		unsigned char green)
	{
		img->width = 2;
		img->height = 2;
		img->stride = 8;
		img->format = VGUI_TEXTURE_FORMAT_RGBA8;
		img->pixels.clear();
		img->pixels.reserve(16);
		for (int i = 0; i < 4; ++i)
		{
			img->pixels.push_back(red);
			img->pixels.push_back(green);
			img->pixels.push_back(90);
			img->pixels.push_back(255);
		}
	};

	{
		/* Declared before the cache so the worker stops before these disappear. */
		std::mutex gate_mutex;
		std::condition_variable gate_cv;
		bool gate_open = false;
		std::atomic<int> gated_started{ 0 };
		std::mutex log_mutex;
		std::vector<std::string> order;
		std::atomic<int> auto_attempts{ 0 };
		std::atomic<int> retry_attempts{ 0 };
		std::atomic<bool> retry_fail{ true };
		std::mutex disk_mutex;
		std::map<std::string, thumbnail_cache::image> disk;
		std::atomic<int> disk_reads{ 0 };
		std::atomic<int> disk_writes{ 0 };

		thumbnail_cache cache(ui);
		thumbnail_cache::options options;
		options.memory_limit = 8;
		options.max_attempts = 2;
		options.retry_delay = 0.0f;
		cache.set_options(options);
		if (cache.configured() || !cache.idle())
			return fail("a05 cache initial state");

		cache.set_load_handler([&](const thumbnail_cache::request& req,
			thumbnail_cache::image* img)
		{
			if (req.key == "blocked" || req.key == "stale")
			{
				gated_started.fetch_add(1);
				std::unique_lock<std::mutex> lock(gate_mutex);
				gate_cv.wait(lock, [&]() { return gate_open; });
			}
			{
				std::lock_guard<std::mutex> lock(log_mutex);
				order.push_back(req.key);
			}
			if (req.key == "auto-retry" &&
				auto_attempts.fetch_add(1) == 0)
			{
				img->error = "transient failure";
				return false;
			}
			if (req.key == "retry")
			{
				retry_attempts.fetch_add(1);
				if (retry_fail.load())
				{
					img->error = "permanent failure";
					return false;
				}
			}
			fill(img, 200, 60);
			return true;
		});

		/* A blocked worker must not block enqueue(), and low-priority queued
		 * work must run after high-priority work. */
		cache.enqueue({ "blocked", "Blocked", "Test" },
			thumbnail_cache::priority::low);
		auto deadline = std::chrono::steady_clock::now() +
			std::chrono::milliseconds(1500);
		while (gated_started.load() == 0 &&
			std::chrono::steady_clock::now() < deadline)
		{
			cache.poll();
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		if (gated_started.load() == 0 ||
			cache.state_of("blocked") != thumbnail_cache::state::loading)
			return fail("a05 blocked loader start");

		const auto enqueue_start = std::chrono::steady_clock::now();
		cache.enqueue({ "queued-low", "Low", "Test" },
			thumbnail_cache::priority::low);
		cache.enqueue({ "queued-high", "High", "Test" },
			thumbnail_cache::priority::high);
		cache.enqueue({ "queued-cancel", "Cancel", "Test" },
			thumbnail_cache::priority::low);
		cache.cancel("queued-cancel");
		const auto enqueue_ms = std::chrono::duration_cast<
			std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - enqueue_start).count();
		if (enqueue_ms > 250)
			return fail("a05 enqueue must not block the main thread");

		{
			std::lock_guard<std::mutex> lock(gate_mutex);
			gate_open = true;
		}
		gate_cv.notify_all();
		auto all_ready = [&]()
		{
			return cache.state_of("blocked") == thumbnail_cache::state::ready &&
				cache.state_of("queued-low") == thumbnail_cache::state::ready &&
				cache.state_of("queued-high") == thumbnail_cache::state::ready;
		};
		deadline = std::chrono::steady_clock::now() +
			std::chrono::milliseconds(2000);
		while (!all_ready() &&
			std::chrono::steady_clock::now() < deadline)
		{
			cache.poll();
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		if (!all_ready() || cache.stats().cancellations == 0)
			return fail("a05 queued jobs and cancellation");
		if (cache.state_of("queued-cancel") != thumbnail_cache::state::missing)
			return fail("a05 cancelled key state");
		{
			std::lock_guard<std::mutex> lock(log_mutex);
			const bool saw_cancelled = std::find(order.begin(), order.end(),
				"queued-cancel") != order.end();
			const auto high = std::find(order.begin(), order.end(), "queued-high");
			const auto low = std::find(order.begin(), order.end(), "queued-low");
			if (saw_cancelled || high == order.end() || low == order.end() ||
				high > low)
				return fail("a05 priority ordering");
		}

		/* A cancelled in-flight result is stale and must not upload. */
		{
			std::lock_guard<std::mutex> lock(gate_mutex);
			gate_open = false;
		}
		gated_started.store(0);
		cache.enqueue({ "stale", "Stale", "Test" },
			thumbnail_cache::priority::low);
		deadline = std::chrono::steady_clock::now() +
			std::chrono::milliseconds(1500);
		while (gated_started.load() == 0 &&
			std::chrono::steady_clock::now() < deadline)
		{
			cache.poll();
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		if (gated_started.load() == 0)
			return fail("a05 stale loader start");
		cache.cancel("stale");
		cache.enqueue({ "sentinel", "Sentinel", "Test" },
			thumbnail_cache::priority::high);
		const std::uint64_t uploads_before = cache.stats().uploads;
		{
			std::lock_guard<std::mutex> lock(gate_mutex);
			gate_open = true;
		}
		gate_cv.notify_all();
		deadline = std::chrono::steady_clock::now() +
			std::chrono::milliseconds(2000);
		while (cache.state_of("sentinel") != thumbnail_cache::state::ready &&
			std::chrono::steady_clock::now() < deadline)
		{
			cache.poll();
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		if (cache.state_of("sentinel") != thumbnail_cache::state::ready)
			return fail("a05 sentinel upload");
		if (cache.state_of("stale") != thumbnail_cache::state::missing ||
			cache.lookup("stale") != nullptr)
			return fail("a05 stale result discarded");
		if (cache.stats().uploads != uploads_before + 1)
			return fail("a05 cancelled upload count");

		/* Exhausted automatic retries end in failed; retry() restarts. */
		const std::uint64_t failures_before = cache.stats().failures;
		cache.enqueue({ "retry", "Retry", "Test" },
			thumbnail_cache::priority::high);
		deadline = std::chrono::steady_clock::now() +
			std::chrono::milliseconds(2000);
		while (cache.state_of("retry") != thumbnail_cache::state::failed &&
			std::chrono::steady_clock::now() < deadline)
		{
			cache.tick(0.05f);
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		if (cache.state_of("retry") != thumbnail_cache::state::failed ||
			retry_attempts.load() != 2 || cache.error("retry").empty() ||
			cache.stats().failures != failures_before + 1)
			return fail("a05 terminal failure state");

		retry_fail.store(false);
		cache.retry("retry");
		deadline = std::chrono::steady_clock::now() +
			std::chrono::milliseconds(1500);
		while (cache.state_of("retry") != thumbnail_cache::state::ready &&
			std::chrono::steady_clock::now() < deadline)
		{
			cache.poll();
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		if (cache.state_of("retry") != thumbnail_cache::state::ready ||
			retry_attempts.load() != 3 || cache.lookup("retry") == nullptr)
			return fail("a05 manual retry");

		/* A transient failure retries automatically. */
		const std::uint64_t retries_before = cache.stats().retries;
		auto_attempts.store(0);
		cache.enqueue({ "auto-retry", "Auto", "Test" },
			thumbnail_cache::priority::high);
		deadline = std::chrono::steady_clock::now() +
			std::chrono::milliseconds(2000);
		while (cache.state_of("auto-retry") != thumbnail_cache::state::ready &&
			std::chrono::steady_clock::now() < deadline)
		{
			cache.tick(0.05f);
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		if (cache.state_of("auto-retry") != thumbnail_cache::state::ready ||
			auto_attempts.load() != 2 ||
			cache.stats().retries != retries_before + 1)
			return fail("a05 automatic retry");

		/* Memory hits neither reload nor consume the disk cache. */
		const std::uint64_t loads_before_hit = cache.stats().loads;
		const std::uint64_t hits_before = cache.stats().memory_hits;
		cache.enqueue({ "hit", "Hit", "Test" });
		deadline = std::chrono::steady_clock::now() +
			std::chrono::milliseconds(1500);
		while (cache.state_of("hit") != thumbnail_cache::state::ready &&
			std::chrono::steady_clock::now() < deadline)
		{
			cache.poll();
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		vgui_texture* hit_texture = cache.texture("hit");
		if (!hit_texture)
			return fail("a05 memory texture");
		cache.enqueue({ "hit", "Hit", "Test" });
		if (cache.stats().loads != loads_before_hit + 1 ||
			cache.stats().memory_hits != hits_before + 1 ||
			cache.texture("hit") != hit_texture)
			return fail("a05 memory hit dedupe");

		/* LRU eviction falls back to disk; disabling the disk cache forces a
		 * fresh load. */
		options.memory_limit = 1;
		options.max_attempts = 1;
		cache.set_options(options);
		cache.set_disk_cache(
			[&](const thumbnail_cache::request& req,
				thumbnail_cache::image* out)
			{
				std::lock_guard<std::mutex> lock(disk_mutex);
				disk_reads.fetch_add(1);
				auto found = disk.find(req.key);
				if (found == disk.end())
					return false;
				*out = found->second;
				return true;
			},
			[&](const thumbnail_cache::request& req,
				const thumbnail_cache::image& img)
			{
				std::lock_guard<std::mutex> lock(disk_mutex);
				disk_writes.fetch_add(1);
				disk[req.key] = img;
				return true;
			});
		cache.enqueue({ "lru-a", "A", "Test" });
		deadline = std::chrono::steady_clock::now() +
			std::chrono::milliseconds(1500);
		while (cache.state_of("lru-a") != thumbnail_cache::state::ready &&
			std::chrono::steady_clock::now() < deadline)
		{
			cache.poll();
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		const std::uint64_t loads_after_a = cache.stats().loads;
		cache.enqueue({ "lru-b", "B", "Test" });
		deadline = std::chrono::steady_clock::now() +
			std::chrono::milliseconds(1500);
		while (cache.state_of("lru-b") != thumbnail_cache::state::ready &&
			std::chrono::steady_clock::now() < deadline)
		{
			cache.poll();
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		if (cache.lookup("lru-a") != nullptr || cache.lookup("lru-b") == nullptr)
			return fail("a05 lru eviction");

		const std::uint64_t disk_hits_before = cache.stats().disk_hits;
		const int reads_before = disk_reads.load();
		cache.enqueue({ "lru-a", "A", "Test" });
		deadline = std::chrono::steady_clock::now() +
			std::chrono::milliseconds(1500);
		while (cache.state_of("lru-a") != thumbnail_cache::state::ready &&
			std::chrono::steady_clock::now() < deadline)
		{
			cache.poll();
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		if (cache.stats().loads != loads_after_a + 1 ||
			cache.stats().disk_hits != disk_hits_before + 1 ||
			disk_reads.load() != reads_before + 1 ||
			cache.lookup("lru-a") == nullptr)
			return fail("a05 disk cache hit");

		options.disk_cache = false;
		cache.set_options(options);
		cache.enqueue({ "lru-c", "C", "Test" });
		deadline = std::chrono::steady_clock::now() +
			std::chrono::milliseconds(1500);
		while (cache.state_of("lru-c") != thumbnail_cache::state::ready &&
			std::chrono::steady_clock::now() < deadline)
		{
			cache.poll();
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		const std::uint64_t loads_before_bypass = cache.stats().loads;
		const int reads_before_bypass = disk_reads.load();
		cache.enqueue({ "lru-a", "A", "Test" });
		deadline = std::chrono::steady_clock::now() +
			std::chrono::milliseconds(1500);
		while (cache.state_of("lru-a") != thumbnail_cache::state::ready &&
			std::chrono::steady_clock::now() < deadline)
		{
			cache.poll();
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		if (cache.stats().loads != loads_before_bypass + 1 ||
			disk_reads.load() != reads_before_bypass)
			return fail("a05 disk cache bypass");
		if (disk_writes.load() < 2 || cache.stats().disk_hits == 0)
			return fail("a05 disk cache writes");

		cache.clear();
		if (cache.stats().memory_entries != 0 ||
			cache.lookup("hit") != nullptr ||
			cache.state_of("hit") != thumbnail_cache::state::missing)
			return fail("a05 cache clear");
	}

	/* Browser integration: host thumbnails win, only visible rows request
	 * asynchronously, and scrolling cancels obsolete queued work. */
	unsigned char host_pixels[4] = { 255, 210, 80, 255 };
	vgui_texture_desc host_desc = {};
	host_desc.width = 1;
	host_desc.height = 1;
	host_desc.format = VGUI_TEXTURE_FORMAT_RGBA8;
	host_desc.pixels = host_pixels;
	vgui_texture* host_texture = vgui_texture_create(ui, &host_desc);
	if (!host_texture)
		return fail("a05 host texture");

	{
		std::mutex gate_mutex;
		std::condition_variable gate_cv;
		bool gate_open = false;
		std::atomic<int> loads{ 0 };
		std::atomic<int> gated_started{ 0 };
		std::mutex log_mutex;
		std::set<std::string> loaded_keys;

		asset_browser assets;
		assets.build(vgui_root(ui));
		vgui_widget_set_size(assets.root(), 360.0f, 150.0f);
		std::vector<vgui_item_id> ids;
		for (int i = 0; i < 30; ++i)
			ids.push_back(assets.add_item(
				"Item " + std::to_string(i) + ".mesh", "Mesh"));
		if (!assets.set_item_thumbnail(ids[0], host_texture))
			return fail("a05 browser host thumbnail");
		vgui_update(ui, 0.016f);
		vgui_layout(ui);

		thumbnail_cache::options options;
		options.memory_limit = 16;
		options.max_attempts = 1;
		assets.thumbnails().set_options(options);
		assets.thumbnails().set_load_handler(
			[&](const thumbnail_cache::request& req,
				thumbnail_cache::image* img)
			{
				loads.fetch_add(1);
				{
					std::lock_guard<std::mutex> lock(log_mutex);
					loaded_keys.insert(req.key);
				}
				fill(img, 120, 180);
				return true;
			});

		const int first_visible = assets.visible_first();
		const int last_visible = assets.visible_last();
		if (last_visible - first_visible <= 0 ||
			last_visible - first_visible >= (int)ids.size())
			return fail("a05 browser visible range");

		auto pump = [&](int wait_ms)
		{
			const auto pump_deadline = std::chrono::steady_clock::now() +
				std::chrono::milliseconds(wait_ms);
			do
			{
				assets.tick(0.016f);
				if (assets.thumbnails().idle())
					break;
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			} while (std::chrono::steady_clock::now() < pump_deadline);
		};
		pump(2000);
		if (!assets.thumbnails().idle())
			return fail("a05 browser idle");
		if (assets.thumbnail_state(ids[(std::size_t)first_visible]) !=
			thumbnail_cache::state::ready)
			return fail("a05 browser async thumbnail");
		if (assets.item_thumbnail(ids[0]) != host_texture ||
			assets.thumbnail_state(ids[0]) != thumbnail_cache::state::ready)
			return fail("a05 browser host precedence");
		if (loads.load() >= (int)ids.size())
			return fail("a05 browser loads hidden items");
		{
			std::lock_guard<std::mutex> lock(log_mutex);
			if (loaded_keys.count(assets.thumbnail_key(ids[0])) != 0)
				return fail("a05 browser loads host item");
		}

		const int loads_before_hit = loads.load();
		assets.tick(0.016f);
		assets.tick(0.016f);
		if (loads.load() != loads_before_hit)
			return fail("a05 browser cache hit reload");

		if (!assets.set_item_thumbnail(ids[0], nullptr))
			return fail("a05 browser clear host thumbnail");
		pump(2000);
		if (assets.item_thumbnail(ids[0]) == host_texture ||
			assets.item_thumbnail(ids[0]) == nullptr ||
			assets.thumbnail_state(ids[0]) != thumbnail_cache::state::ready)
			return fail("a05 browser async after host clear");

		/* Replace the loader with a gated one, scroll far away, then scroll
		 * back before releasing the worker.  Queued offscreen requests are
		 * cancelled and the in-flight result is discarded. */
		{
			std::lock_guard<std::mutex> lock(gate_mutex);
			gate_open = false;
		}
		gated_started.store(0);
		loads.store(0);
		assets.thumbnails().set_load_handler(
			[&](const thumbnail_cache::request& req,
				thumbnail_cache::image* img)
			{
				gated_started.fetch_add(1);
				std::unique_lock<std::mutex> lock(gate_mutex);
				gate_cv.wait(lock, [&]() { return gate_open; });
				loads.fetch_add(1);
				{
					std::lock_guard<std::mutex> log_lock(log_mutex);
					loaded_keys.insert(req.key);
				}
				fill(img, 30, 200);
				return true;
			});

		const int top_first = assets.visible_first();
		vgui_panel_set_scroll(assets.widget(), 0.0f, 100000.0f);
		vgui_update(ui, 0.016f);
		vgui_layout(ui);
		const int bottom_first = assets.visible_first();
		if (bottom_first <= top_first)
			return fail("a05 browser scroll range");
		assets.tick(0.016f);
		auto deadline = std::chrono::steady_clock::now() +
			std::chrono::milliseconds(1500);
		while (gated_started.load() == 0 &&
			std::chrono::steady_clock::now() < deadline)
		{
			assets.tick(0.016f);
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		if (gated_started.load() == 0)
			return fail("a05 browser offscreen loader start");

		vgui_panel_set_scroll(assets.widget(), 0.0f, 0.0f);
		vgui_update(ui, 0.016f);
		vgui_layout(ui);
		assets.tick(0.016f);
		const std::uint64_t cancellations =
			assets.thumbnails().stats().cancellations;
		if (cancellations == 0)
			return fail("a05 browser offscreen cancellation");
		{
			std::lock_guard<std::mutex> lock(gate_mutex);
			gate_open = true;
		}
		gate_cv.notify_all();
		for (int i = 0; i < 100; ++i)
		{
			assets.tick(0.016f);
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		if (loads.load() > 1)
			return fail("a05 browser cancelled loads");
	}

	vgui_texture_destroy(host_texture);
	vgui_destroy(ui);
	return 0;
}

/* A06: 宿主数据库操作、整批撤销、结构化错误、右键/快捷键与依赖查看。 */
static int test_asset_operations()
{
	using namespace vgui_editor_kit;
	vgui_context* ui = vgui_create(nullptr, 16.0f);
	if (!ui)
		return fail("a06 context");
	vgui_context_set_size(ui, 1100, 760);
	vgui_style style;
	vgui_style_default(&style);
	style.list.row_height = 24.0f;
	vgui_context_set_style(ui, &style);

	/* 一个最小的宿主资产数据库:名称 + 所属目录。 */
	std::map<vgui_item_id, std::pair<std::string, std::string>> db;
	std::vector<vgui_item_id> order;
	const auto add_asset = [&](vgui_item_id id, std::string name,
		std::string folder)
	{
		db[id] = { std::move(name), std::move(folder) };
		order.push_back(id);
	};
	add_asset(201, "Cube.mesh", "models");
	add_asset(202, "Sphere.mesh", "models");
	add_asset(203, "Ghost.mesh", "models");
	add_asset(204, "Broken.mesh", "models");
	add_asset(405, "Steel.mat", "models");
	add_asset(301, "icon.png", "textures");
	add_asset(302, "checker.png", "textures");
	vgui_item_id next_id = 500;

	const auto type_of = [](const std::string& name)
	{
		const auto dot = name.find_last_of('.');
		const std::string ext = dot == std::string::npos
			? std::string() : name.substr(dot + 1);
		if (ext == "mesh")
			return std::string("Mesh");
		if (ext == "png")
			return std::string("Texture");
		if (ext == "mat")
			return std::string("Material");
		return std::string("Asset");
	};
	const auto folder_label = [](const std::string& path_id)
	{
		if (path_id == "models")
			return std::string("Models");
		if (path_id == "textures")
			return std::string("Textures");
		if (path_id == "materials")
			return std::string("Materials");
		if (path_id == "archive")
			return std::string("Archive");
		return path_id;
	};
	const auto name_exists = [&](const std::string& name)
	{
		for (const auto& entry : db)
			if (entry.second.first == name)
				return true;
		return false;
	};

	bool reimport_undoable = true;
	command_stack commands;
	{
		asset_browser assets;
		assets.build(vgui_root(ui));
		vgui_widget_set_pos(assets.root(), 20.0f, 20.0f);
		vgui_widget_set_size(assets.root(), 520.0f, 440.0f);
		assets.set_command_stack(&commands);

		assets.set_navigate_handler([&](const std::string& path_id,
			asset_browser::asset_directory* out)
		{
			if (!out)
				return false;
			if (path_id != "models" && path_id != "textures" &&
				path_id != "materials" && path_id != "archive")
				return false;
			asset_browser::asset_directory directory;
			directory.path.push_back({ "Root", "root" });
			directory.path.push_back({ folder_label(path_id), path_id });
			for (vgui_item_id id : order)
			{
				const auto found = db.find(id);
				if (found == db.end() || found->second.second != path_id)
					continue;
				asset_info info;
				info.name = found->second.first;
				info.type = type_of(info.name);
				info.id = id;
				directory.items.push_back(info);
			}
			*out = std::move(directory);
			return true;
		});

		std::vector<std::pair<std::string, asset_browser::operation_error>>
			notices;
		assets.set_notice_listener([&](const std::string& message,
			asset_browser::operation_error error)
		{
			notices.emplace_back(message, error);
		});

		assets.set_operation_handler(
			[&](const asset_browser::operation_request& request,
				asset_browser::operation_result* result) -> bool
			{
				if (!result)
					return false;
				*result = asset_browser::operation_result();
				const auto fail = [result](
					asset_browser::operation_error error,
					std::string message)
				{
					result->error = error;
					result->message = std::move(message);
					return false;
				};
				if (request.items.empty())
					return fail(asset_browser::operation_error::no_selection,
						"no assets selected");
				for (vgui_item_id id : request.items)
				{
					if (!db.count(id))
						return fail(asset_browser::operation_error::not_found,
							"asset " + std::to_string(id) +
							" no longer exists");
					if (db[id].first == "Ghost.mesh")
						return fail(asset_browser::operation_error::read_only,
							"Ghost.mesh is marked read-only");
					if (db[id].first == "Steel.mat")
						return fail(asset_browser::operation_error::in_use,
							"'Steel.mat' is used by Camera");
				}

				const auto before = db;
				bool record_undo = true;
				switch (request.kind)
				{
				case asset_browser::operation::rename:
				{
					if (request.items.size() != 1)
						return fail(asset_browser::operation_error::invalid_name,
							"rename expects one asset");
					const vgui_item_id item = request.items.front();
					if (request.name.empty() ||
						request.name.find_first_of("/\\") != std::string::npos)
						return fail(asset_browser::operation_error::invalid_name,
							"name cannot be empty or contain separators");
					for (const auto& entry : db)
					{
						if (entry.first != item &&
							entry.second.first == request.name)
							return fail(asset_browser::operation_error::conflict,
								"an asset named '" + request.name +
								"' already exists");
					}
					db[item].first = request.name;
					result->select.push_back(item);
					break;
				}
				case asset_browser::operation::remove:
					for (vgui_item_id id : request.items)
						db.erase(id);
					break;
				case asset_browser::operation::duplicate:
				{
					for (vgui_item_id id : request.items)
					{
						const auto found = db.find(id);
						if (found == db.end())
							return fail(
								asset_browser::operation_error::not_found,
								"asset disappeared during duplicate");
						const std::string base = found->second.first;
						const auto dot = base.find_last_of('.');
						const std::string stem = dot == std::string::npos
							? base : base.substr(0, dot);
						const std::string ext = dot == std::string::npos
							? std::string() : base.substr(dot);
						std::string copy_name = stem + " copy" + ext;
						int attempt = 2;
						while (name_exists(copy_name))
							copy_name = stem + " copy " +
								std::to_string(attempt++) + ext;
						const vgui_item_id copy_id = next_id++;
						db[copy_id] = { copy_name, found->second.second };
						order.push_back(copy_id);
						result->created.push_back(copy_id);
						result->select.push_back(copy_id);
					}
					break;
				}
				case asset_browser::operation::move:
				{
					const std::string& target = request.target_path_id;
					if (target.empty() || target == "missing" ||
						target == request.path_id)
						return fail(
							asset_browser::operation_error::invalid_target,
							target == "missing"
								? "destination folder no longer exists"
								: "choose a different destination folder");
					for (vgui_item_id id : request.items)
					{
						const auto found = db.find(id);
						if (found == db.end())
							return fail(
								asset_browser::operation_error::not_found,
								"asset disappeared during move");
						for (const auto& entry : db)
						{
							if (entry.first != id &&
								entry.second.second == target &&
								entry.second.first == found->second.first)
								return fail(
									asset_browser::operation_error::conflict,
									"'" + found->second.first +
									"' already exists in the destination");
						}
					}
					for (vgui_item_id id : request.items)
						db[id].second = target;
					result->select = request.items;
					break;
				}
				case asset_browser::operation::reimport:
					if (request.items.size() == 1 &&
						db.count(request.items.front()) &&
						db[request.items.front()].first == "Broken.mesh")
						return fail(asset_browser::operation_error::io_error,
							"Broken.mesh failed to decode; the source file is corrupt");
					result->select = request.items;
					result->message = "refreshed from disk";
					record_undo = reimport_undoable;
					break;
				}

				if (record_undo)
				{
					const auto after = db;
					result->undoable = true;
					result->undo = [&db, before]()
					{
						db = before;
						return true;
					};
					result->redo = [&db, after]()
					{
						db = after;
						return true;
					};
				}
				return true;
			});

		assets.set_move_targets_handler([&]()
		{
			return std::vector<asset_browser::asset_move_target>{
				{ "Models", "models" },
				{ "Textures", "textures" },
				{ "Archive", "archive" },
			};
		});
		assets.set_dependency_handler([](vgui_item_id item)
		{
			std::vector<asset_browser::asset_dependency> deps;
			if (item == 201)
			{
				deps.push_back({ "Camera", "Scene Object", "scene", 1 });
				deps.push_back({ "Light", "Scene Object", "scene", 2 });
			}
			return deps;
		});

		int import_calls = 0;
		bool import_ok = true;
		std::vector<std::string> last_files;
		std::string last_import_path;
		assets.set_import_handler([&](const std::vector<std::string>& files,
			const std::string& path_id)
		{
			++import_calls;
			last_files = files;
			last_import_path = path_id;
			return import_ok;
		});
		int import_prompt_calls = 0;
		assets.set_import_prompt_handler([&]()
		{
			++import_prompt_calls;
		});

		const auto item_name = [&](vgui_item_id id)
		{
			for (int i = 0; i < assets.item_count(); ++i)
			{
				asset_info info;
				if (assets.item_at(i, &info) && info.id == id)
					return info.name;
			}
			return std::string();
		};
		const auto item_index = [&](vgui_item_id id)
		{
			for (int i = 0; i < assets.item_count(); ++i)
			{
				asset_info info;
				if (assets.item_at(i, &info) && info.id == id)
					return i;
			}
			return -1;
		};
		const auto item_present = [&](vgui_item_id id)
		{
			return item_index(id) >= 0;
		};
		const auto menu_item = [](vgui_widget* menu, const char* text)
		{
			if (!menu)
				return (vgui_item_id)0;
			for (int i = 0; i < vgui_menu_get_count(menu); ++i)
			{
				const vgui_item_id item = vgui_menu_get_item_at(menu, i);
				const char* label = vgui_menu_get_item_text(menu, item);
				if (label && std::string(label) == text)
					return item;
			}
			return (vgui_item_id)0;
		};
		const auto press_enter = [&]()
		{
			vgui_input_key(ui, VGUI_KEY_ENTER, 1, 0);
			vgui_input_key(ui, VGUI_KEY_ENTER, 0, 0);
		};
		const auto press_escape = [&]()
		{
			vgui_input_key(ui, VGUI_KEY_ESCAPE, 1, 0);
			vgui_input_key(ui, VGUI_KEY_ESCAPE, 0, 0);
		};
		const auto open_menu_at = [&](vgui_item_id id)
		{
			const int index = item_index(id);
			float x = 0, y = 0, w = 0, h = 0;
			if (index < 0 ||
				!assets.item_rect(index, &x, &y, &w, &h) ||
				w <= 0.0f || h <= 0.0f)
				return (vgui_widget*)nullptr;
			vgui_input_mouse_move(ui, x + w * 0.5f, y + h * 0.5f);
			vgui_input_mouse_down(ui, 3, x + w * 0.5f, y + h * 0.5f, 1);
			vgui_input_mouse_up(ui, 3, x + w * 0.5f, y + h * 0.5f, 1);
			return vgui_context_get_open_menu(ui);
		};

		vgui_update(ui, 0.016f);
		vgui_layout(ui);
		if (!assets.navigate_to("models") || assets.item_count() != 5 ||
			!item_present(201) || !item_present(405))
			return fail("a06 initial directory");

		/* 成功重命名 + 可撤销。 */
		assets.set_selection({ 201 });
		std::size_t before_commands = commands.undo_count();
		if (!assets.rename_item(201, "CubeA.mesh") ||
			db[201].first != "CubeA.mesh" ||
			item_name(201) != "CubeA.mesh" ||
			assets.selected() != 201 ||
			commands.undo_count() != before_commands + 1 ||
			std::string(commands.next_undo_name()) != "Rename asset" ||
			assets.last_error() != asset_browser::operation_error::none ||
			!assets.last_undoable() ||
			assets.last_message().find("Renamed 'Cube.mesh' to 'CubeA.mesh'") ==
				std::string::npos)
			return fail("a06 rename success");
		if (notices.empty() || notices.back().second !=
			asset_browser::operation_error::none ||
			notices.back().first.find("Renamed") == std::string::npos)
			return fail("a06 rename notice");
		if (!commands.undo() || db[201].first != "Cube.mesh" ||
			item_name(201) != "Cube.mesh" || commands.can_undo())
			return fail("a06 rename undo");
		if (!commands.redo() || db[201].first != "CubeA.mesh" ||
			item_name(201) != "CubeA.mesh" || commands.can_redo())
			return fail("a06 rename redo");
		if (!commands.undo())
			return fail("a06 rename undo again");
		commands.clear();

		/* 冲突/空名称是明确的结构化错误,且不留撤销记录。 */
		before_commands = commands.undo_count();
		if (assets.rename_item(201, "Sphere.mesh") ||
			assets.last_error() != asset_browser::operation_error::conflict ||
			assets.last_message().find("name conflict") == std::string::npos ||
			assets.last_message().find("Sphere.mesh") == std::string::npos ||
			db[201].first != "Cube.mesh" ||
			item_name(201) != "Cube.mesh" ||
			commands.undo_count() != before_commands ||
			assets.last_undoable())
			return fail("a06 rename conflict");
		if (assets.rename_item(201, "   ") ||
			assets.last_error() != asset_browser::operation_error::invalid_name ||
			db[201].first != "Cube.mesh" ||
			commands.undo_count() != before_commands)
			return fail("a06 rename empty");

		/* 只读与占用错误,以及整批失败不产生部分写入。 */
		if (assets.remove_items({ 203 }) ||
			assets.last_error() != asset_browser::operation_error::read_only ||
			assets.last_message().find("read-only") == std::string::npos ||
			assets.last_message().find("Ghost.mesh") == std::string::npos ||
			!item_present(203) || commands.undo_count() != before_commands)
			return fail("a06 remove read-only");
		if (assets.remove_items({ 405 }) ||
			assets.last_error() != asset_browser::operation_error::in_use ||
			assets.last_message().find("in use") == std::string::npos ||
			assets.last_message().find("Steel.mat") == std::string::npos ||
			!item_present(405))
			return fail("a06 remove in-use");
		if (assets.remove_items({ 201, 405 }) ||
			assets.last_error() != asset_browser::operation_error::in_use ||
			db.size() != 7 || !item_present(201) || !item_present(405) ||
			commands.undo_count() != before_commands)
			return fail("a06 batch atomicity");

		/* 批量复制:一个撤销记录、结果选择新资产。 */
		commands.clear();
		assets.set_selection({ 201, 202 });
		if (!assets.duplicate_selected() ||
			db.size() != 9 || assets.item_count() != 7 ||
			assets.selection().size() != 2 ||
			!assets.is_selected(500) || !assets.is_selected(501) ||
			assets.selected() != 500 ||
			commands.undo_count() != 1 ||
			std::string(commands.next_undo_name()) != "Duplicate assets" ||
			!assets.last_undoable() ||
			assets.last_message().find("Duplicated 2 assets") == std::string::npos)
			return fail("a06 duplicate batch");
		if (!commands.undo() || db.size() != 7 || assets.item_count() != 5)
			return fail("a06 duplicate undo");
		if (!commands.redo() || db.size() != 9 || assets.item_count() != 7)
			return fail("a06 duplicate redo");
		if (!commands.undo())
			return fail("a06 duplicate undo again");
		commands.clear();

		/* 批量删除也作为一个撤销记录。 */
		assets.set_selection({ 201, 202 });
		if (!assets.remove_items({ 201, 202 }) ||
			db.count(201) || db.count(202) || assets.item_count() != 3 ||
			commands.undo_count() != 1 ||
			std::string(commands.next_undo_name()) != "Delete assets" ||
			assets.last_message().find("Deleted 2 assets") == std::string::npos)
			return fail("a06 remove batch");
		if (!commands.undo() || !item_present(201) || !item_present(202) ||
			assets.item_count() != 5)
			return fail("a06 remove batch undo");
		if (!commands.redo() || item_present(201) || item_present(202) ||
			assets.item_count() != 3)
			return fail("a06 remove batch redo");
		if (!commands.undo())
			return fail("a06 remove batch undo again");
		commands.clear();

		/* 移动:空目标、当前目录与不存在目标都拒绝;成功后可撤销。 */
		assets.set_selection({ 201 });
		if (assets.move_items({ 201 }, "") ||
			assets.last_error() != asset_browser::operation_error::invalid_target ||
			assets.last_message().find("no destination given") ==
				std::string::npos ||
			commands.undo_count() != 0)
			return fail("a06 move empty target");
		if (assets.move_items({ 201 }, "models") ||
			assets.last_error() != asset_browser::operation_error::invalid_target ||
			assets.last_message().find("already in this folder") ==
				std::string::npos ||
			commands.undo_count() != 0)
			return fail("a06 move same folder");
		if (assets.move_items({ 201 }, "missing") ||
			assets.last_error() != asset_browser::operation_error::invalid_target ||
			assets.last_message().find("destination folder no longer exists") ==
				std::string::npos ||
			commands.undo_count() != 0)
			return fail("a06 move missing target");
		if (!assets.move_items({ 201 }, "textures") ||
			db[201].second != "textures" || item_present(201) ||
			assets.item_count() != 4 ||
			commands.undo_count() != 1 ||
			std::string(commands.next_undo_name()) != "Move assets" ||
			assets.last_message().find("Moved 1 asset to 'textures'") ==
				std::string::npos ||
			!assets.last_undoable())
			return fail("a06 move success");
		if (!commands.undo() || db[201].second != "models" ||
			!item_present(201) || assets.item_count() != 5)
			return fail("a06 move undo");
		if (!commands.redo() || db[201].second != "textures" ||
			item_present(201) || assets.item_count() != 4)
			return fail("a06 move redo");
		if (!commands.undo())
			return fail("a06 move undo again");
		commands.clear();

		/* 重新导入:可撤销成功、I/O 错误、以及显式不可撤销。 */
		assets.set_selection({ 201 });
		reimport_undoable = true;
		if (!assets.reimport_items({ 201 }) ||
			commands.undo_count() != 1 ||
			std::string(commands.next_undo_name()) != "Reimport assets" ||
			!assets.last_undoable() ||
			assets.last_message().find("Reimported 1 asset") ==
				std::string::npos)
			return fail("a06 reimport success");
		if (!commands.undo() || commands.can_undo())
			return fail("a06 reimport undo");
		commands.clear();
		if (assets.reimport_items({ 204 }) ||
			assets.last_error() != asset_browser::operation_error::io_error ||
			assets.last_message().find("I/O error") == std::string::npos ||
			assets.last_message().find("Broken.mesh") == std::string::npos ||
			commands.undo_count() != 0)
			return fail("a06 reimport io error");
		vgui_toast_clear(ui);
		reimport_undoable = false;
		if (!assets.reimport_items({ 201 }) || assets.last_undoable() ||
			assets.last_message().find("(not undoable)") == std::string::npos ||
			commands.undo_count() != 0 ||
			vgui_toast_count(ui) == 0)
			return fail("a06 reimport not undoable");
		reimport_undoable = true;
		vgui_toast_clear(ui);

		/* 右键菜单:列表行打开、项启用状态、快捷键提示和依赖查看。 */
		assets.enable_context_menu();
		if (!assets.context_menu_enabled())
			return fail("a06 context menu enabled");
		assets.set_selection({ 201, 202 });
		vgui_widget* menu = open_menu_at(201);
		if (!menu || !vgui_menu_is_open(menu))
			return fail("a06 list context menu opens");
		if (menu_item(menu, "Rename") == 0 ||
			vgui_menu_get_item_enabled(menu, menu_item(menu, "Rename")) != 0)
			return fail("a06 rename disabled for multi-selection");
		press_escape();
		if (vgui_context_get_open_menu(ui))
			return fail("a06 context menu closes");

		assets.set_selection({ 201 });
		menu = open_menu_at(201);
		if (!menu || !assets.is_selected(201))
			return fail("a06 list context menu selection");
		const vgui_item_id rename_item = menu_item(menu, "Rename");
		const vgui_item_id duplicate_item = menu_item(menu, "Duplicate");
		const vgui_item_id delete_item = menu_item(menu, "Delete");
		vgui_item_id move_root = menu_item(menu, "Move To");
		const vgui_item_id import_item = menu_item(menu, "Import...");
		const vgui_item_id reimport_item = menu_item(menu, "Reimport");
		const vgui_item_id deps_item = menu_item(menu, "Show Dependencies");
		if (!rename_item || !duplicate_item || !delete_item || !move_root ||
			!import_item || !reimport_item || !deps_item)
			return fail("a06 context menu entries");
		if (vgui_menu_get_item_enabled(menu, rename_item) != 1 ||
			vgui_menu_get_item_enabled(menu, duplicate_item) != 1 ||
			vgui_menu_get_item_enabled(menu, delete_item) != 1 ||
			vgui_menu_get_item_enabled(menu, move_root) != 1 ||
			vgui_menu_get_item_enabled(menu, import_item) != 1 ||
			vgui_menu_get_item_enabled(menu, reimport_item) != 1 ||
			vgui_menu_get_item_enabled(menu, deps_item) != 1)
			return fail("a06 context menu item states");
		if (std::string(vgui_menu_get_item_shortcut(menu, rename_item)) != "F2" ||
			std::string(vgui_menu_get_item_shortcut(menu, duplicate_item)) !=
				"Ctrl+D" ||
			std::string(vgui_menu_get_item_shortcut(menu, delete_item)) != "Del" ||
			std::string(vgui_menu_get_item_shortcut(menu, import_item)) !=
				"Ctrl+I" ||
			std::string(vgui_menu_get_item_shortcut(menu, reimport_item)) != "F5")
			return fail("a06 context menu shortcuts");

		vgui_menu_set_cursor(menu, deps_item);
		press_enter();
		if (!vgui_modal_active(ui) ||
			assets.last_message().find("Camera") == std::string::npos ||
			assets.last_message().find("Light") == std::string::npos ||
			assets.last_message().find("Scene Object") == std::string::npos ||
			assets.last_message().find("scene") == std::string::npos)
			return fail("a06 menu dependencies dispatch");
		press_escape();

		/* Move To 子菜单:当前目录被排除,激活后执行移动。 */
		menu = open_menu_at(201);
		if (!menu)
			return fail("a06 move menu reopen");
		move_root = menu_item(menu, "Move To");
		vgui_widget* submenu = vgui_menu_get_submenu(menu, move_root);
		const vgui_item_id textures_item = menu_item(submenu, "Textures");
		if (!submenu || !textures_item ||
			menu_item(submenu, "Models") != 0 ||
			menu_item(submenu, "Archive") == 0)
			return fail("a06 move submenu contents");
		vgui_menu_set_cursor(menu, move_root);
		vgui_input_key(ui, VGUI_KEY_RIGHT, 1, 0);
		vgui_input_key(ui, VGUI_KEY_RIGHT, 0, 0);
		vgui_menu_set_cursor(submenu, textures_item);
		press_enter();
		if (db[201].second != "textures" || item_present(201) ||
			commands.undo_count() != 1 ||
			assets.last_message().find("Moved 1 asset to 'textures'") ==
				std::string::npos)
			return fail("a06 move submenu dispatch");
		if (!commands.undo() || db[201].second != "models" ||
			!item_present(201))
			return fail("a06 move submenu undo");
		commands.clear();

		/* 网格右键与列表共用菜单和 dispatch。 */
		assets.set_view(asset_browser::view_mode::grid);
		vgui_update(ui, 0.016f);
		vgui_layout(ui);
		assets.set_selection({ 202 });
		menu = open_menu_at(202);
		if (!menu || !assets.is_selected(202) ||
			menu_item(menu, "Duplicate") == 0)
			return fail("a06 grid context menu");
		vgui_menu_set_cursor(menu, menu_item(menu, "Duplicate"));
		press_enter();
		if (db.size() != 8 || commands.undo_count() != 1 ||
			assets.last_message().find("Duplicated 1 asset") == std::string::npos)
			return fail("a06 grid menu duplicate");
		if (!commands.undo() || db.size() != 7)
			return fail("a06 grid menu undo");
		commands.clear();
		assets.set_view(asset_browser::view_mode::list);
		vgui_update(ui, 0.016f);
		vgui_layout(ui);

		/* 快捷键与菜单共用 dispatch;焦点不在浏览器时不应抢占。 */
		assets.set_selection({ 201 });
		vgui_set_focus(ui, assets.widget());
		vgui_input_key(ui, VGUI_KEY_F2, 1, 0);
		vgui_input_key(ui, VGUI_KEY_F2, 0, 0);
		if (!vgui_modal_active(ui) || !vgui_dialog_input(ui) ||
			std::string(vgui_edit_get_text(vgui_dialog_input(ui))) !=
				"Cube.mesh")
			return fail("a06 f2 opens rename dialog");
		press_escape();
		if (db[201].first != "Cube.mesh" || commands.undo_count() != 0)
			return fail("a06 f2 cancel");

		vgui_input_key(ui, VGUI_KEY_DELETE, 1, 0);
		vgui_input_key(ui, VGUI_KEY_DELETE, 0, 0);
		if (!vgui_modal_active(ui))
			return fail("a06 delete shortcut dialog");
		press_escape();
		if (!item_present(201) || commands.undo_count() != 0)
			return fail("a06 delete shortcut cancel");

		vgui_input_key(ui, 'D', 1, VGUI_MOD_CTRL);
		vgui_input_key(ui, 'D', 0, VGUI_MOD_CTRL);
		if (db.size() != 8 || commands.undo_count() != 1 ||
			!assets.last_undoable())
			return fail("a06 ctrl+d shortcut");
		if (!commands.undo())
			return fail("a06 ctrl+d undo");
		commands.clear();

		assets.set_selection({ 201 });
		vgui_set_focus(ui, assets.widget());
		vgui_input_key(ui, VGUI_KEY_F5, 1, 0);
		vgui_input_key(ui, VGUI_KEY_F5, 0, 0);
		if (commands.undo_count() != 1 ||
			std::string(commands.next_undo_name()) != "Reimport assets")
			return fail("a06 f5 shortcut");
		if (!commands.undo())
			return fail("a06 f5 undo");
		commands.clear();

		vgui_input_key(ui, 'I', 1, VGUI_MOD_CTRL);
		vgui_input_key(ui, 'I', 0, VGUI_MOD_CTRL);
		if (import_prompt_calls != 1 ||
			assets.last_message() != "Import dialog requested" ||
			commands.undo_count() != 0)
			return fail("a06 ctrl+i import prompt");

		vgui_widget* other = vgui_widget_add(vgui_root(ui), VGUI_BUTTON,
			"Other");
		vgui_widget_set_size(other, 80.0f, 24.0f);
		vgui_set_focus(ui, other);
		vgui_input_key(ui, VGUI_KEY_F2, 1, 0);
		vgui_input_key(ui, VGUI_KEY_F2, 0, 0);
		if (vgui_modal_active(ui) || commands.undo_count() != 0)
			return fail("a06 shortcuts respect focus");
		vgui_set_focus(ui, assets.widget());

		/* 重命名对话框:确认、取消和失败路径。 */
		assets.set_selection({ 201 });
		if (!assets.begin_rename(201))
			return fail("a06 rename dialog opens");
		vgui_widget* input = vgui_dialog_input(ui);
		if (!input || std::string(vgui_edit_get_text(input)) != "Cube.mesh")
			return fail("a06 rename dialog initial text");
		vgui_edit_set_text(input, "Dialog.mesh");
		press_enter();
		if (vgui_modal_active(ui) || db[201].first != "Dialog.mesh" ||
			item_name(201) != "Dialog.mesh" ||
			commands.undo_count() != 1 ||
			assets.last_message().find("Renamed 'Cube.mesh' to 'Dialog.mesh'") ==
				std::string::npos)
			return fail("a06 rename dialog confirm");
		if (!commands.undo())
			return fail("a06 rename dialog undo");
		commands.clear();

		if (!assets.begin_rename(201))
			return fail("a06 rename dialog reopens");
		input = vgui_dialog_input(ui);
		vgui_edit_set_text(input, "Nope.mesh");
		press_escape();
		if (vgui_modal_active(ui) || db[201].first != "Cube.mesh" ||
			commands.undo_count() != 0)
			return fail("a06 rename dialog cancel");

		if (!assets.begin_rename(201))
			return fail("a06 rename dialog invalid opens");
		input = vgui_dialog_input(ui);
		vgui_edit_set_text(input, "   ");
		press_enter();
		if (db[201].first != "Cube.mesh" || commands.undo_count() != 0 ||
			assets.last_error() != asset_browser::operation_error::invalid_name)
			return fail("a06 rename dialog invalid name");

		/* 删除确认:确认后删除并可撤销,取消不写入。 */
		assets.set_selection({ 201 });
		if (!assets.confirm_remove({ 201 }) || !vgui_modal_active(ui))
			return fail("a06 delete dialog opens");
		press_enter();
		if (db.count(201) || item_present(201) || commands.undo_count() != 1 ||
			std::string(commands.next_undo_name()) != "Delete assets" ||
			assets.last_message().find("Deleted 1 asset") == std::string::npos)
			return fail("a06 delete dialog confirm");
		if (!commands.undo() || !item_present(201) || db.count(201) != 1)
			return fail("a06 delete dialog undo");
		commands.clear();

		if (!assets.confirm_remove({ 201 }) || !vgui_modal_active(ui))
			return fail("a06 delete dialog reopens");
		press_escape();
		if (!item_present(201) || commands.undo_count() != 0)
			return fail("a06 delete dialog cancel");
		if (assets.confirm_remove({}) ||
			assets.last_error() != asset_browser::operation_error::no_selection ||
			vgui_modal_active(ui))
			return fail("a06 delete empty selection");

		/* 导入入口仍走 A04 import_handler,并通过通知/错误暴露结果。 */
		vgui_toast_clear(ui);
		import_ok = true;
		const std::size_t notice_count = notices.size();
		if (!assets.import_files({ "C:/new.png" }) ||
			import_calls != 1 || last_files.size() != 1 ||
			last_files[0] != "C:/new.png" || last_import_path != "models" ||
			assets.last_error() != asset_browser::operation_error::none ||
			assets.last_undoable() ||
			assets.last_message().find("Imported 1 file") == std::string::npos ||
			notices.size() != notice_count + 1 ||
			notices.back().second != asset_browser::operation_error::none)
			return fail("a06 import success notice");
		import_ok = false;
		if (assets.import_files({ "C:/bad.png" }) || import_calls != 2 ||
			assets.last_error() != asset_browser::operation_error::io_error ||
			assets.last_message().find("host rejected the import") ==
				std::string::npos ||
			assets.last_undoable())
			return fail("a06 import failure");
		assets.set_import_handler({});
		if (assets.import_files({ "C:/x.png" }) ||
			assets.last_error() !=
				asset_browser::operation_error::unsupported ||
			assets.last_message().find("no import handler installed") ==
				std::string::npos)
			return fail("a06 import without handler");

		if (!assets.request_import() || import_prompt_calls != 2 ||
			assets.last_message() != "Import dialog requested" ||
			assets.last_undoable())
			return fail("a06 import prompt");
		assets.set_import_prompt_handler({});
		if (assets.request_import() ||
			assets.last_error() !=
				asset_browser::operation_error::unsupported ||
			assets.last_message().find("no import dialog handler installed") ==
				std::string::npos)
			return fail("a06 import prompt missing");

		/* 依赖列表:直接读取、模态展示、空结果与无处理器。 */
		if (assets.dependencies(201).size() != 2 ||
			!assets.dependencies(999).empty())
			return fail("a06 dependencies query");
		if (!assets.show_dependencies(201) || !vgui_modal_active(ui) ||
			assets.last_message().find("'Cube.mesh' is used by 2 assets") ==
				std::string::npos ||
			assets.last_message().find("Camera") == std::string::npos ||
			assets.last_message().find("Light") == std::string::npos)
			return fail("a06 dependencies modal");
		press_escape();
		if (!assets.show_dependencies(202) || !vgui_modal_active(ui) ||
			assets.last_message().find("No assets depend on 'Sphere.mesh'") ==
				std::string::npos)
			return fail("a06 dependencies empty");
		press_escape();
		assets.set_dependency_handler({});
		if (assets.show_dependencies(201) ||
			assets.last_error() !=
				asset_browser::operation_error::unsupported)
			return fail("a06 dependencies missing handler");

		assets.clear_selection();
		if (assets.show_dependencies(0) ||
			assets.last_error() != asset_browser::operation_error::no_selection)
			return fail("a06 dependencies no selection");
		if (assets.begin_rename(0) ||
			assets.last_error() != asset_browser::operation_error::no_selection)
			return fail("a06 rename no selection");
	}

	vgui_destroy(ui);
	return 0;
}

/* V01: Viewport 表面——渲染尺寸、resize 通知、跨边界鼠标捕获与焦点取消。 */
static int test_viewport_widget()
{
	using namespace vgui_editor_kit;
	vgui_context* ui = vgui_create(nullptr, 16.0f);
	if (!ui)
		return fail("v01 context");
	vgui_context_set_size(ui, 900, 650);

	{
		viewport_view viewport;
		viewport.build(vgui_root(ui));
		if (!viewport.widget() ||
			vgui_widget_get_kind(viewport.widget()) != VGUI_CUSTOM)
			return fail("v01 surface kind");
		vgui_widget_set_pos(viewport.widget(), 20.0f, 20.0f);
		vgui_widget_set_size(viewport.widget(), 400.0f, 300.0f);

		/* 焦点旁落需要一个可聚焦的对照控件。 */
		vgui_widget* other = vgui_widget_add(vgui_root(ui), VGUI_BUTTON, "other");
		vgui_widget_set_pos(other, 600.0f, 500.0f);
		vgui_widget_set_size(other, 120.0f, 30.0f);

		/* 附加监听器不能顶掉现有的单监听器接口（Outliner 行内重命名依赖它）。 */
		int legacy_focus = 0;
		vgui_context_set_focus_listener(ui,
			[](vgui_context*, vgui_widget*, vgui_widget*, void* user)
			{
				++*static_cast<int*>(user);
			}, &legacy_focus);

		vgui_update(ui, 1.0f / 60.0f);
		vgui_layout(ui);

		/* 1) 渲染尺寸 = 内容区（去掉 1px 边框），并且只在变化时通知一次。 */
		if (!viewport.poll_resize())
			return fail("v01 initial resize");
		if (viewport.render_width() != 398 || viewport.render_height() != 298)
			return fail("v01 render size");
		if (viewport.poll_resize())
			return fail("v01 resize edge triggered");

		int resize_calls = 0;
		int resize_w = 0, resize_h = 0;
		viewport.set_resize_listener([&](int w, int h)
		{
			++resize_calls;
			resize_w = w;
			resize_h = h;
		});
		vgui_widget_set_size(viewport.widget(), 520.0f, 360.0f);
		vgui_update(ui, 1.0f / 60.0f);
		vgui_layout(ui);
		if (!viewport.poll_resize() || resize_calls != 1 ||
			resize_w != 518 || resize_h != 358)
			return fail("v01 resize listener");

		/* 2) resize 期间纹理保持绑定：宿主原地更新即可，不会空一帧。 */
		uint32_t pixels[4] = { 0xFF202020u, 0xFFE0E0E0u, 0xFFE0E0E0u, 0xFF202020u };
		vgui_texture_desc desc = {};
		desc.width = 2;
		desc.height = 2;
		desc.format = VGUI_TEXTURE_FORMAT_RGBA8;
		desc.pixels = pixels;
		vgui_texture* texture = vgui_texture_create(ui, &desc);
		if (!texture)
			return fail("v01 texture");
		viewport.set_texture(texture);
		if (viewport.texture() != texture)
			return fail("v01 bind texture");
		vgui_widget_set_size(viewport.widget(), 300.0f, 200.0f);
		vgui_update(ui, 1.0f / 60.0f);
		vgui_layout(ui);
		viewport.poll_resize();
		if (viewport.texture() != texture || resize_calls != 2)
			return fail("v01 texture survives resize");
		viewport.set_texture(nullptr);
		vgui_texture_destroy(texture);

		/* 3) 按下后拖出控件边界，事件仍然送达且坐标保持本地（可为负）。 */
		int moves = 0, downs = 0, ups = 0, cancels = 0, keys = 0;
		float local_x = 0.0f, local_y = 0.0f;
		viewport.set_event_listener([&](const vgui_event* event)
		{
			if (!event)
				return false;
			switch (event->type)
			{
			case VGUI_EVENT_MOUSE_MOVE:
				++moves;
				local_x = event->x;
				local_y = event->y;
				break;
			case VGUI_EVENT_MOUSE_DOWN:
				++downs;
				break;
			case VGUI_EVENT_MOUSE_UP:
				++ups;
				break;
			case VGUI_EVENT_CANCEL:
				++cancels;
				break;
			case VGUI_EVENT_KEY_DOWN:
				++keys;
				break;
			default:
				break;
			}
			return false;
		});
		int focus_events = 0;
		bool focus_now = false;
		viewport.set_focus_listener([&](bool focused)
		{
			++focus_events;
			focus_now = focused;
		});

		float vx = 0, vy = 0, vw = 0, vh = 0;
		vgui_widget_get_rect(viewport.widget(), &vx, &vy, &vw, &vh);
		const float inside_x = vx + vw * 0.5f;
		const float inside_y = vy + vh * 0.5f;

		vgui_input_mouse_move(ui, inside_x, inside_y);
		if (moves != 1 || downs != 0)
			return fail("v01 hover move");
		vgui_input_mouse_down(ui, 1, inside_x, inside_y, 1);
		if (downs != 1 || !viewport.dragging() || viewport.drag_button() != 1)
			return fail("v01 gesture begins");
		if (!viewport.focused() || !focus_now || focus_events != 1 ||
			legacy_focus == 0)
			return fail("v01 press focuses surface");

		const int moves_before_outside = moves;
		vgui_input_mouse_move(ui, 5.0f, 5.0f);
		if (!viewport.dragging() || moves <= moves_before_outside)
			return fail("v01 drag continues outside");
		if (local_x > 0.0f || local_y > 0.0f)
			return fail("v01 outside coordinates stay local");
		vgui_input_mouse_up(ui, 1, 5.0f, 5.0f, 1);
		if (ups != 1 || viewport.dragging())
			return fail("v01 release ends gesture");

		/* 4) 键盘事件只发给获得焦点的表面。 */
		vgui_input_key(ui, 'F', 1, 0);
		vgui_input_key(ui, 'F', 0, 0);
		if (keys != 1)
			return fail("v01 keyboard reaches focused surface");

		/* 5) 焦点旁落时取消手势并向宿主转发 CANCEL。 */
		const int cancels_before_focus = cancels;
		vgui_input_mouse_move(ui, inside_x, inside_y);
		vgui_input_mouse_down(ui, 1, inside_x, inside_y, 1);
		if (!viewport.dragging())
			return fail("v01 second gesture");
		vgui_set_focus(ui, other);
		if (viewport.dragging() || cancels != cancels_before_focus + 1)
			return fail("v01 focus loss cancels");
		if (focus_now || viewport.focused())
			return fail("v01 focus listener reports loss");
		vgui_input_mouse_up(ui, 1, inside_x, inside_y, 1);

		/* 6) vgui_input_cancel（窗口失焦）同样结束手势并清空指针状态。 */
		const int cancels_before_cancel = cancels;
		vgui_input_mouse_move(ui, inside_x, inside_y);
		vgui_input_mouse_down(ui, 1, inside_x, inside_y, 1);
		if (!viewport.dragging())
			return fail("v01 cancel gesture begins");
		vgui_input_cancel(ui);
		if (viewport.dragging() || cancels != cancels_before_cancel + 1)
			return fail("v01 input cancel");
		if (vgui_context_wants_mouse(ui) != 0)
			return fail("v01 cancel clears pointer state");

		/* 7) 隐藏或折叠的页不给宿主发 1x1 的重建请求。 */
		vgui_widget_set_visible(viewport.widget(), 0);
		vgui_update(ui, 1.0f / 60.0f);
		vgui_layout(ui);
		if (viewport.poll_resize())
			return fail("v01 hidden surface keeps size");
		vgui_widget_set_visible(viewport.widget(), 1);
		vgui_update(ui, 1.0f / 60.0f);
		vgui_layout(ui);
	}

	vgui_destroy(ui);
	return 0;
}

/* V02: 轨道相机——平移/旋转/缩放/取景、左键预留、焦点取消与状态往返。 */
static int test_viewport_camera()
{
	using namespace vgui_editor_kit;
	vgui_context* ui = vgui_create(nullptr, 16.0f);
	if (!ui)
		return fail("v02 context");
	vgui_context_set_size(ui, 900, 650);

	{
		viewport_view viewport;
		viewport.build(vgui_root(ui));
		vgui_widget_set_pos(viewport.widget(), 20.0f, 20.0f);
		vgui_widget_set_size(viewport.widget(), 400.0f, 300.0f);

		vgui_widget* other = vgui_widget_add(vgui_root(ui), VGUI_BUTTON, "other");
		vgui_widget_set_pos(other, 600.0f, 500.0f);
		vgui_widget_set_size(other, 120.0f, 30.0f);

		viewport_camera camera;
		camera.attach(viewport);
		if (!camera.attached())
			return fail("v02 camera attach");

		vgui_update(ui, 1.0f / 60.0f);
		vgui_layout(ui);
		viewport.poll_resize();

		float vx = 0, vy = 0, vw = 0, vh = 0;
		vgui_widget_get_rect(viewport.widget(), &vx, &vy, &vw, &vh);
		const float cx = vx + vw * 0.5f;
		const float cy = vy + vh * 0.5f;

		/* 注意:windows.h 把 near 定义成空宏,这里不能用这个名字。 */
		auto close_to = [](float a, float b, float eps = 1e-3f) {
			return std::fabs(a - b) <= eps;
		};
		auto moved = [](const camera_vec3& a, const camera_vec3& b) {
			return std::fabs(a.x - b.x) + std::fabs(a.y - b.y) +
				std::fabs(a.z - b.z) > 1e-4f;
		};
		const camera_state initial = camera.get_state();
		const camera_vec3 position0 = camera.position();
		const float span0 = std::sqrt(
			(position0.x - initial.target.x) * (position0.x - initial.target.x) +
			(position0.y - initial.target.y) * (position0.y - initial.target.y) +
			(position0.z - initial.target.z) * (position0.z - initial.target.z));
		if (!close_to(span0, initial.distance, 1e-3f))
			return fail("v02 derived position");

		int changes = 0;
		camera.set_change_listener([&] { ++changes; });

		/* 1) 中键平移：target 改变、距离保持；拖出视口仍连续。 */
		vgui_input_mouse_move(ui, cx, cy);
		vgui_input_mouse_down(ui, 2, cx, cy, 1);
		if (!camera.panning())
			return fail("v02 middle drag starts pan");
		const camera_vec3 target_before_pan = camera.get_state().target;
		vgui_input_mouse_move(ui, cx + 40.0f, cy);
		const camera_state panned = camera.get_state();
		if (!moved(panned.target, target_before_pan) ||
			!close_to(panned.distance, initial.distance))
			return fail("v02 pan updates target");
		const camera_vec3 position1 = camera.position();
		const float span1 = std::sqrt(
			(position1.x - panned.target.x) * (position1.x - panned.target.x) +
			(position1.y - panned.target.y) * (position1.y - panned.target.y) +
			(position1.z - panned.target.z) * (position1.z - panned.target.z));
		if (!close_to(span1, initial.distance))
			return fail("v02 pan keeps distance");
		const camera_vec3 target_mid = panned.target;
		vgui_input_mouse_move(ui, 5.0f, 5.0f);
		if (!camera.panning() || !moved(camera.get_state().target, target_mid))
			return fail("v02 pan continues outside");
		vgui_input_mouse_up(ui, 2, 5.0f, 5.0f, 1);
		if (camera.navigating())
			return fail("v02 pan release ends");
		if (changes == 0)
			return fail("v02 change listener");

		/* 2) 右键旋转与 pitch 钳制。 */
		camera.reset();
		vgui_input_mouse_move(ui, cx, cy);
		vgui_input_mouse_down(ui, 3, cx, cy, 1);
		if (!camera.orbiting())
			return fail("v02 right drag starts orbit");
		const camera_state before_orbit = camera.get_state();
		vgui_input_mouse_move(ui, cx + 50.0f, cy + 20.0f);
		const camera_state orbited = camera.get_state();
		if (!(orbited.yaw > before_orbit.yaw) ||
			!(orbited.pitch > before_orbit.pitch))
			return fail("v02 orbit direction");
		vgui_input_mouse_move(ui, cx + 50.0f, cy + 10000.0f);
		if (!close_to(camera.get_state().pitch, 1.5f, 1e-4f))
			return fail("v02 orbit pitch clamp");
		vgui_input_mouse_up(ui, 3, cx + 50.0f, cy + 10000.0f, 1);
		if (camera.navigating())
			return fail("v02 orbit release ends");

		/* 3) 滚轮缩放与距离上下限。 */
		camera.reset();
		camera.set_distance_range(1.0f, 10.0f);
		vgui_input_mouse_move(ui, cx, cy);
		const float zoom_start = camera.get_state().distance;
		vgui_input_mouse_wheel(ui, 0.0f, 1.0f);
		const float zoomed_in = camera.get_state().distance;
		if (!(zoomed_in < zoom_start))
			return fail("v02 wheel zooms in");
		vgui_input_mouse_wheel(ui, 0.0f, -1.0f);
		if (!(camera.get_state().distance > zoomed_in))
			return fail("v02 wheel zooms out");
		vgui_input_mouse_wheel(ui, 0.0f, 100.0f);
		if (!close_to(camera.get_state().distance, 1.0f))
			return fail("v02 zoom min clamp");
		vgui_input_mouse_wheel(ui, 0.0f, -100.0f);
		if (!close_to(camera.get_state().distance, 10.0f))
			return fail("v02 zoom max clamp");
		camera.set_distance_range(0.1f, 1000.0f);

		/* 4) 左键完全不动相机（V03/V05 预留）。 */
		camera.reset();
		const camera_state before_left = camera.get_state();
		vgui_input_mouse_move(ui, cx, cy);
		vgui_input_mouse_down(ui, 1, cx, cy, 1);
		vgui_input_mouse_move(ui, cx + 60.0f, cy + 30.0f);
		vgui_input_mouse_up(ui, 1, cx + 60.0f, cy + 30.0f, 1);
		const camera_state after_left = camera.get_state();
		if (moved(after_left.target, before_left.target) ||
			!close_to(after_left.yaw, before_left.yaw) ||
			!close_to(after_left.pitch, before_left.pitch) ||
			!close_to(after_left.distance, before_left.distance) ||
			camera.navigating())
			return fail("v02 left button reserved");

		/* 5) 焦点旁落取消导航（复用 V01 的 CANCEL 通路）。 */
		camera.reset();
		vgui_input_mouse_move(ui, cx, cy);
		vgui_input_mouse_down(ui, 3, cx, cy, 1);
		if (!camera.orbiting())
			return fail("v02 cancel gesture starts");
		vgui_set_focus(ui, other);
		if (camera.navigating())
			return fail("v02 focus loss cancels navigation");
		vgui_input_mouse_up(ui, 3, cx, cy, 1);

		/* 6) 导航总开关。 */
		camera.reset();
		camera.set_navigation_enabled(false);
		const camera_state disabled_before = camera.get_state();
		vgui_input_mouse_move(ui, cx, cy);
		vgui_input_mouse_down(ui, 2, cx, cy, 1);
		vgui_input_mouse_move(ui, cx + 40.0f, cy);
		vgui_input_mouse_wheel(ui, 0.0f, 1.0f);
		vgui_input_mouse_up(ui, 2, cx + 40.0f, cy, 1);
		const camera_state disabled_after = camera.get_state();
		if (moved(disabled_after.target, disabled_before.target) ||
			!close_to(disabled_after.distance, disabled_before.distance))
			return fail("v02 navigation disabled");
		camera.set_navigation_enabled(true);

		/* 7) F 取景选中对象。 */
		camera.reset();
		bool have_selection = false;
		camera_focus focus_target;
		focus_target.center = { 2.0f, -1.0f, 3.0f };
		focus_target.radius = 2.0f;
		camera.set_focus_provider([&](camera_focus* out) {
			if (!have_selection)
				return false;
			*out = focus_target;
			return true;
		});
		vgui_set_focus(ui, viewport.widget());
		if (camera.focus_selection())
			return fail("v02 focus without selection");
		have_selection = true;
		vgui_input_key(ui, 'F', 1, 0);
		const camera_state framed = camera.get_state();
		if (!close_to(framed.target.x, 2.0f) || !close_to(framed.target.y, -1.0f) ||
			!close_to(framed.target.z, 3.0f))
			return fail("v02 F frames selection");
		const float expected_distance = 2.0f /
			std::sin(25.0f * 3.14159265358979323846f / 180.0f) * 1.15f;
		if (!close_to(framed.distance, expected_distance, 0.01f))
			return fail("v02 focus distance");
		vgui_input_key(ui, 'F', 0, 0);
		const camera_state before_ctrl_f = camera.get_state();
		vgui_input_key(ui, 'F', 1, VGUI_MOD_CTRL);
		const camera_state after_ctrl_f = camera.get_state();
		vgui_input_key(ui, 'F', 0, VGUI_MOD_CTRL);
		if (moved(after_ctrl_f.target, before_ctrl_f.target) ||
			!close_to(after_ctrl_f.distance, before_ctrl_f.distance))
			return fail("v02 modified F ignored");

		/* 8) 状态保存/恢复与非法值钳制。 */
		camera_state stored = camera.get_state();
		stored.target = { 1.0f, 2.0f, 3.0f };
		stored.yaw = 1.25f;
		stored.pitch = -0.5f;
		stored.distance = 4.0f;
		stored.fov_deg = 60.0f;
		camera.set_state(stored);
		const camera_state round_trip = camera.get_state();
		if (!close_to(round_trip.yaw, 1.25f) || !close_to(round_trip.pitch, -0.5f) ||
			!close_to(round_trip.distance, 4.0f) || !close_to(round_trip.fov_deg, 60.0f) ||
			!close_to(round_trip.target.y, 2.0f))
			return fail("v02 state round trip");
		camera_state wild = stored;
		wild.pitch = 99.0f;
		wild.distance = 0.0f;
		wild.fov_deg = 1000.0f;
		wild.near_plane = -1.0f;
		wild.far_plane = 0.0f;
		camera.set_state(wild);
		const camera_state clamped = camera.get_state();
		if (!close_to(clamped.pitch, 1.5f) || !close_to(clamped.distance, 0.1f) ||
			!close_to(clamped.fov_deg, 120.0f) ||
			!(clamped.far_plane > clamped.near_plane) ||
			!(clamped.near_plane > 0.0f))
			return fail("v02 state clamping");

		/* 9) 附加事件监听器与宿主主监听器并存，并可单独移除。 */
		int primary_events = 0;
		int extra_events = 0;
		viewport.set_event_listener([&](const vgui_event*) {
			++primary_events;
			return false;
		});
		const int token = viewport.add_event_listener([&](const vgui_event*) {
			++extra_events;
			return false;
		});
		if (!token)
			return fail("v02 additive listener token");
		vgui_input_mouse_move(ui, cx, cy);
		if (primary_events == 0 || extra_events == 0)
			return fail("v02 additive listener dispatch");
		viewport.remove_event_listener(token);
		const int extra_before_removal = extra_events;
		vgui_input_mouse_move(ui, cx + 1.0f, cy);
		if (extra_events != extra_before_removal)
			return fail("v02 listener removal");

		/* 10) 重新 attach 不会叠加监听。 */
		camera.attach(viewport);
		if (!camera.attached())
			return fail("v02 reattach");
	}

	vgui_destroy(ui);
	return 0;
}

/* V03: editor_context 多选集合、主选语义与集合监听器。 */
static int test_editor_context_multi_selection()
{
	using namespace vgui_editor_kit;
	vgui_context* ui = vgui_create(nullptr, 16.0f);
	if (!ui)
		return fail("v03 context");
	{
		editor_context context(ui);
		int primary_calls = 0;
		object_id last_primary = 999;
		int set_calls = 0;
		std::vector<object_id> last_set;
		const auto pid = context.add_selection_listener([&](object_id id) {
			++primary_calls;
			last_primary = id;
		});
		const auto sid = context.add_selection_set_listener(
			[&](const std::vector<object_id>& ids) {
				++set_calls;
				last_set = ids;
			});
		if (!pid || !sid)
			return fail("v03 selection listeners");
		if (context.selection() != invalid_object ||
			context.selection_count() != 0 || !context.selection_ids().empty())
			return fail("v03 selection starts empty");

		context.set_selection(5);
		if (context.selection() != 5 || context.selection_count() != 1 ||
			context.selection_ids()[0] != 5 || last_primary != 5 ||
			primary_calls != 1 || set_calls != 1 || last_set.size() != 1 ||
			last_set[0] != 5)
			return fail("v03 single selection");

		context.set_selection(5);
		if (primary_calls != 1 || set_calls != 1)
			return fail("v03 duplicate selection ignored");

		context.set_selection({ 1, 2, 3 }, 2);
		if (context.selection() != 2 || context.selection_count() != 3 ||
			context.selection_ids()[1] != 2 ||
			primary_calls != 2 || set_calls != 2)
			return fail("v03 selection set");

		/* 重复/无效 id 被过滤;primary 不在集合时回退到首个。 */
		context.set_selection({ 2, 1, 2, 0 }, 9);
		if (context.selection() != 2 || context.selection_count() != 2 ||
			context.selection_ids()[0] != 2 || context.selection_ids()[1] != 1 ||
			primary_calls != 2 || set_calls != 3)
			return fail("v03 set sanitize and primary fallback");

		/* 纯主选变化只触发单监听器;集合不变。 */
		context.set_selection({ 2, 1 }, 1);
		if (context.selection() != 1 || primary_calls != 3 || set_calls != 3)
			return fail("v03 primary-only change");

		context.add_to_selection(4);
		if (context.selection_count() != 3 || context.selection() != 1 ||
			set_calls != 4)
			return fail("v03 add to selection");
		context.add_to_selection(4);
		context.add_to_selection(0);
		if (set_calls != 4)
			return fail("v03 duplicate add ignored");

		context.remove_from_selection(1);
		if (context.selection() != 2 || context.selection_count() != 2 ||
			primary_calls != 4 || set_calls != 5)
			return fail("v03 remove primary");

		if (!context.is_selected(2) || context.is_selected(1) ||
			context.is_selected(9))
			return fail("v03 is_selected");

		context.toggle_selection(2);
		if (context.selection() != 4 || context.selection_count() != 1 ||
			primary_calls != 5 || set_calls != 6)
			return fail("v03 toggle primary off");
		context.toggle_selection(7);
		if (context.selection() != 4 || context.selection_count() != 2 ||
			primary_calls != 5 || set_calls != 7)
			return fail("v03 toggle add");

		context.clear_selection();
		if (context.selection() != invalid_object ||
			context.selection_count() != 0 ||
			primary_calls != 6 || set_calls != 8)
			return fail("v03 clear selection");
		context.clear_selection();
		if (primary_calls != 6 || set_calls != 8)
			return fail("v03 duplicate clear ignored");

		context.remove_selection_listener(pid);
		context.remove_selection_set_listener(sid);
		context.set_selection(9);
		if (primary_calls != 6 || set_calls != 8)
			return fail("v03 listener removal");
	}
	vgui_destroy(ui);
	return 0;
}

/* V03: 点击/框选手势、修饰键规则、取消与坐标换算。 */
static int test_viewport_selection()
{
	using namespace vgui_editor_kit;
	vgui_context* ui = vgui_create(nullptr, 16.0f);
	if (!ui)
		return fail("v03 viewport context");
	vgui_context_set_size(ui, 900, 650);
	vgui_style style;
	vgui_style_default(&style);
	style.dock.drag_threshold = 4.0f;
	vgui_context_set_style(ui, &style);

	{
		editor_context context(ui);
		viewport_view viewport;
		viewport.build(vgui_root(ui));
		vgui_widget_set_pos(viewport.widget(), 20.0f, 20.0f);
		vgui_widget_set_size(viewport.widget(), 400.0f, 300.0f);

		viewport_selection picking;
		picking.attach(viewport, context);
		if (!picking.attached())
			return fail("v03 picking attach");
		picking.set_drag_threshold(4.0f);

		object_id point_hit = 7;
		selection_point_query point_query = {};
		int point_calls = 0;
		picking.set_point_picker([&](const selection_point_query& query)
		{
			++point_calls;
			point_query = query;
			return point_hit;
		});

		selection_box_query box_query = {};
		int box_calls = 0;
		std::vector<object_id> box_hits;
		picking.set_box_picker([&](const selection_box_query& query)
		{
			++box_calls;
			box_query = query;
			return box_hits;
		});

		vgui_update(ui, 1.0f / 60.0f);
		vgui_layout(ui);
		viewport.poll_resize();

		float vx = 0, vy = 0, vw = 0, vh = 0;
		vgui_widget_get_rect(viewport.widget(), &vx, &vy, &vw, &vh);
		auto surface = [&](float content_x, float content_y)
		{
			return std::pair<float, float>(vx + 1.0f + content_x,
				vy + 1.0f + content_y);
		};
		auto ids_are = [&](std::initializer_list<object_id> expected)
		{
			return context.selection_ids() == std::vector<object_id>(expected);
		};

		auto click_at = [&](float content_x, float content_y, int mods)
		{
			if (mods)
				vgui_input_key(ui, 'X', 1, mods);
			const auto p = surface(content_x, content_y);
			vgui_input_mouse_move(ui, p.first, p.second);
			vgui_input_mouse_down(ui, 1, p.first, p.second, 1);
			vgui_input_mouse_up(ui, 1, p.first, p.second, 1);
			if (mods)
				vgui_input_key(ui, 'X', 0, 0);
		};

		auto drag_box = [&](float x0, float y0, float x1, float y1, int mods)
		{
			if (mods)
				vgui_input_key(ui, 'X', 1, mods);
			const auto a = surface(x0, y0);
			const auto b = surface(x1, y1);
			vgui_input_mouse_move(ui, a.first, a.second);
			vgui_input_mouse_down(ui, 1, a.first, a.second, 1);
			vgui_input_mouse_move(ui, a.first + 20.0f, a.second + 20.0f);
			vgui_input_mouse_move(ui, b.first, b.second);
			vgui_input_mouse_up(ui, 1, b.first, b.second, 1);
			if (mods)
				vgui_input_key(ui, 'X', 0, 0);
		};

		/* 1) 点击替换 + 内容区坐标/尺寸。 */
		click_at(100.0f, 100.0f, 0);
		if (point_calls != 1 || !ids_are({ 7 }))
			return fail("v03 point pick replaces");
		if (std::fabs(point_query.x - 100.0f) > 1e-3f ||
			std::fabs(point_query.y - 100.0f) > 1e-3f ||
			point_query.width != 398 || point_query.height != 298)
			return fail("v03 point query coordinates");

		/* 2) Ctrl 切换、Shift 追加、空白点击清空。 */
		click_at(100.0f, 100.0f, VGUI_MOD_CTRL);
		if (!context.selection_ids().empty())
			return fail("v03 ctrl toggles off");
		click_at(100.0f, 100.0f, VGUI_MOD_CTRL);
		if (!ids_are({ 7 }))
			return fail("v03 ctrl toggles on");
		point_hit = 8;
		click_at(100.0f, 100.0f, VGUI_MOD_SHIFT);
		if (!ids_are({ 7, 8 }) || context.selection() != 7)
			return fail("v03 shift appends");
		point_hit = 0;
		click_at(10.0f, 10.0f, 0);
		if (!context.selection_ids().empty())
			return fail("v03 empty click clears");
		context.set_selection(7);
		click_at(10.0f, 10.0f, VGUI_MOD_CTRL);
		if (!ids_are({ 7 }))
			return fail("v03 empty ctrl click keeps");
		context.clear_selection();

		/* 3) 框选替换/追加/切换/空框清空。 */
		box_hits = { 1, 2 };
		drag_box(100.0f, 100.0f, 200.0f, 200.0f, 0);
		if (box_calls != 1 || !ids_are({ 1, 2 }) || context.selection() != 1)
			return fail("v03 box pick replaces");
		if (std::fabs(box_query.x0 - 100.0f) > 1e-3f ||
			std::fabs(box_query.y0 - 100.0f) > 1e-3f ||
			std::fabs(box_query.x1 - 200.0f) > 1e-3f ||
			std::fabs(box_query.y1 - 200.0f) > 1e-3f ||
			box_query.width != 398 || box_query.height != 298)
			return fail("v03 box query normalised");

		box_hits = { 1, 3 };
		drag_box(100.0f, 100.0f, 200.0f, 200.0f, VGUI_MOD_CTRL);
		if (!ids_are({ 2, 3 }) || context.selection() != 2)
			return fail("v03 ctrl box toggles");

		box_hits = { 4 };
		drag_box(100.0f, 100.0f, 200.0f, 200.0f, VGUI_MOD_SHIFT);
		if (!ids_are({ 2, 3, 4 }) || context.selection() != 2)
			return fail("v03 shift box appends");

		box_hits.clear();
		drag_box(100.0f, 100.0f, 200.0f, 200.0f, 0);
		if (!context.selection_ids().empty())
			return fail("v03 empty box clears");

		/* 4) 小于阈值仍按点击;拖出视口仍继续框选。 */
		context.clear_selection();
		point_hit = 9;
		const int point_before_small_drag = point_calls;
		{
			const auto p = surface(100.0f, 100.0f);
			vgui_input_mouse_move(ui, p.first, p.second);
			vgui_input_mouse_down(ui, 1, p.first, p.second, 1);
			vgui_input_mouse_move(ui, p.first + 2.0f, p.second + 2.0f);
			vgui_input_mouse_up(ui, 1, p.first + 2.0f, p.second + 2.0f, 1);
		}
		if (point_calls != point_before_small_drag + 1 || !ids_are({ 9 }))
			return fail("v03 small drag stays a click");

		box_hits = { 5 };
		{
			const auto p = surface(100.0f, 100.0f);
			vgui_input_mouse_move(ui, p.first, p.second);
			vgui_input_mouse_down(ui, 1, p.first, p.second, 1);
			vgui_input_mouse_move(ui, p.first + 50.0f, p.second + 50.0f);
			if (!picking.selecting())
				return fail("v03 box drag starts");
			vgui_input_mouse_move(ui, 5.0f, 5.0f);
			vgui_input_mouse_up(ui, 1, 5.0f, 5.0f, 1);
		}
		if (!ids_are({ 5 }))
			return fail("v03 box drag outside");
		/* widget(5,5) - widget origin(20,20) - content inset(1,1) = (-16,-16) */
		if (std::fabs(box_query.x0 + 16.0f) > 1e-3f ||
			std::fabs(box_query.y0 + 16.0f) > 1e-3f ||
			std::fabs(box_query.x1 - 100.0f) > 1e-3f ||
			std::fabs(box_query.y1 - 100.0f) > 1e-3f)
			return fail("v03 outside box coordinates");

		/* 5) Esc 与 CANCEL 取消框选，不调用 picker。 */
		box_hits = { 6 };
		context.clear_selection();
		{
			const auto p = surface(100.0f, 100.0f);
			vgui_input_mouse_move(ui, p.first, p.second);
			vgui_input_mouse_down(ui, 1, p.first, p.second, 1);
			vgui_input_mouse_move(ui, p.first + 50.0f, p.second + 50.0f);
			if (!picking.selecting())
				return fail("v03 escape gesture starts");
			const int before = box_calls;
			vgui_input_key(ui, VGUI_KEY_ESCAPE, 1, 0);
			vgui_input_key(ui, VGUI_KEY_ESCAPE, 0, 0);
			if (picking.selecting() || picking.pending())
				return fail("v03 escape cancels");
			vgui_input_mouse_up(ui, 1, p.first + 50.0f, p.second + 50.0f, 1);
			if (box_calls != before || !context.selection_ids().empty())
				return fail("v03 escape does not pick");
		}
		{
			const auto p = surface(100.0f, 100.0f);
			vgui_input_mouse_move(ui, p.first, p.second);
			vgui_input_mouse_down(ui, 1, p.first, p.second, 1);
			vgui_input_mouse_move(ui, p.first + 50.0f, p.second + 50.0f);
			const int before = box_calls;
			vgui_input_cancel(ui);
			if (picking.selecting())
				return fail("v03 cancel resets gesture");
			vgui_input_mouse_up(ui, 1, p.first + 50.0f, p.second + 50.0f, 1);
			if (box_calls != before)
				return fail("v03 cancel does not pick");
		}

		/* 6) 禁用、关闭多选、缺失 picker。 */
		box_hits = { 7 };
		context.set_selection(5);
		picking.set_enabled(false);
		point_hit = 6;
		const int point_before_disable = point_calls;
		click_at(100.0f, 100.0f, 0);
		if (point_calls != point_before_disable || !ids_are({ 5 }))
			return fail("v03 disabled picking");
		picking.set_enabled(true);

		picking.set_multi_select(false);
		context.set_selection(7);
		point_hit = 8;
		click_at(100.0f, 100.0f, VGUI_MOD_CTRL);
		if (!ids_are({ 8 }))
			return fail("v03 multi-select disabled");
		picking.set_multi_select(true);

		picking.set_point_picker({});
		const std::vector<object_id> before_empty_picker = context.selection_ids();
		click_at(100.0f, 100.0f, 0);
		if (context.selection_ids() != before_empty_picker)
			return fail("v03 missing picker is a no-op");
		if (picking.selected_ids() != context.selection_ids())
			return fail("v03 selected ids mirror context");
	}

	vgui_destroy(ui);
	return 0;
}

/* V03: Outliner 多选与 editor_context 双向同步。 */
static int test_outliner_multi_selection_sync()
{
	using namespace vgui_editor_kit;
	vgui_context* ui = vgui_create(nullptr, 16.0f);
	if (!ui)
		return fail("v03 outliner context");
	vgui_context_set_size(ui, 900, 650);
	vgui_style style;
	vgui_style_default(&style);
	style.tree.row_height = 24.0f;
	vgui_context_set_style(ui, &style);

	{
		editor_context context(ui);
		outliner_view outliner;
		outliner.build(context, vgui_root(ui));
		vgui_widget_set_pos(outliner.widget(), 10.0f, 10.0f);
		vgui_widget_set_size(outliner.widget(), 300.0f, 300.0f);

		const vgui_item_id item_a = outliner.add_node("A", 0, 1);
		const vgui_item_id item_b = outliner.add_node("B", 0, 2);
		const vgui_item_id item_c = outliner.add_node("C", 0, 3);
		outliner.set_multi_select(true);
		outliner.enable_selection_sync();
		vgui_update(ui, 1.0f / 60.0f);
		vgui_layout(ui);

		/* context -> tree */
		context.set_selection({ 1, 3 }, 1);
		if (!vgui_tree_is_selected(outliner.widget(), item_a) ||
			vgui_tree_is_selected(outliner.widget(), item_b) ||
			!vgui_tree_is_selected(outliner.widget(), item_c) ||
			context.selection() != 1)
			return fail("v03 context to outliner set");

		/* tree -> context: 追加 B, 视觉顺序 A,B,C */
		vgui_tree_select(outliner.widget(), item_b, 1);
		if (context.selection_count() != 3 ||
			context.selection_ids()[0] != 1 ||
			context.selection_ids()[1] != 2 ||
			context.selection_ids()[2] != 3 ||
			context.selection() != 2)
			return fail("v03 outliner adds to context");

		/* 反选 B -> context 回到 {A,C}, 主选回到 A */
		vgui_tree_select(outliner.widget(), item_b, 0);
		if (context.selection_count() != 2 || context.selection() != 1 ||
			!context.is_selected(1) || context.is_selected(2) ||
			!context.is_selected(3))
			return fail("v03 outliner deselect updates context");

		vgui_tree_clear_selection(outliner.widget());
		if (!context.selection_ids().empty())
			return fail("v03 outliner clear selection");

		/* context -> tree 单选 */
		context.set_selection(2);
		if (!vgui_tree_is_selected(outliner.widget(), item_b) ||
			vgui_tree_is_selected(outliner.widget(), item_a) ||
			vgui_tree_is_selected(outliner.widget(), item_c))
			return fail("v03 context single selection sync");

		context.set_selection({ 1, 2, 3 }, 3);
		if (vgui_tree_get_selected_count(outliner.widget()) != 3 ||
			context.selection() != 3)
			return fail("v03 context multi selection sync");
	}

	vgui_destroy(ui);
	return 0;
}

int main()
{
	using namespace vgui_editor_kit;
	SetUnhandledExceptionFilter(crash_dump);
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
	if (int rc = test_property_multi())
		return rc;
	if (int rc = test_property_clipboard())
		return rc;
	if (int rc = test_property_favorites())
		return rc;
	if (int rc = test_property_reference())
		return rc;
	if (int rc = test_color_picker())
		return rc;
	if (int rc = test_color_picker_drag())
		return rc;
	if (int rc = test_curve_editor())
		return rc;
	if (int rc = test_gradient_editor())
		return rc;
	if (int rc = test_property_grid_curve_gradient())
		return rc;
	if (int rc = test_outliner_row_extensions())
		return rc;
	if (int rc = test_outliner_inline_rename())
		return rc;
	if (int rc = test_outliner_drag_semantics())
		return rc;
	if (int rc = test_outliner_filter_sync())
		return rc;
	if (int rc = test_virtual_outliner())
		return rc;
	if (int rc = test_outliner_context_menu())
		return rc;
	if (int rc = test_asset_browser_views())
		return rc;
	if (int rc = test_asset_browser_navigation())
		return rc;
	if (int rc = test_asset_preview())
		return rc;
	if (int rc = test_asset_drop_paths())
		return rc;
	if (int rc = test_asset_thumbnails())
		return rc;
	if (int rc = test_asset_operations())
		return rc;
	if (int rc = test_viewport_widget())
		return rc;
	if (int rc = test_viewport_camera())
		return rc;
	if (int rc = test_editor_context_multi_selection())
		return rc;
	if (int rc = test_viewport_selection())
		return rc;
	if (int rc = test_outliner_multi_selection_sync())
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
			return fail("shell build");		vgui_update(ui, 1.0f / 60.0f);
		vgui_layout(ui);

		if (!shell.toolbar().widget() || !shell.statusbar().widget() ||
			!shell.search().widget() || !shell.path().widget())
			return fail("shell toolbar and status widgets");
		if (shell.statusbar().message() != "ready" ||
			shell.statusbar().selection_text() != "Selected: 7")
			return fail("shell status selection");
		if (shell.path().count() != 2 || shell.path().item(1).id != "object:7")
			return fail("shell breadcrumb selection");
		if (!shell.picking().attached())
			return fail("shell picking attached");

		context.set_selection(std::vector<object_id>{ 7, 8 }, 7);
		if (shell.statusbar().selection_text() !=
			"Selected: 2 objects (primary 7)")
			return fail("shell multi selection summary");
		context.set_selection(7);
		if (shell.statusbar().selection_text() != "Selected: 7")
			return fail("shell selection summary restores");

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
			return fail("shell assets tab restore");		vgui_item_id scene_root = shell.scene().add_node("Scene");
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
			return fail("shell status follows selection");		shell.properties().add_row("Name", "Cube");
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
			return fail("shell typed property redo");		vgui_item_id mesh = shell.assets().add_item("Cube.mesh", "Mesh");
		if (mesh == 0)
			return fail("asset item");
		vgui_list_select(shell.assets().widget(), mesh, 1);
		if (shell.assets().selected() != mesh)
			return fail("asset selection");

		if (!shell.viewport().widget())
			return fail("viewport widget");
		if (!shell.camera().attached())
			return fail("shell camera attached");
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
		}		{
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
				[&assigned](const std::string& asset) {
					assigned = asset;
					return true;
				});
			vgui_input_mouse_move(ui, arx + 30.0f, arow_y);
			vgui_input_mouse_down(ui, 1, arx + 30.0f, arow_y, 1);
			vgui_input_mouse_move(ui, ppx + ppw * 0.5f, ppy + pph * 0.5f);
			if (!vgui_drag_active(ui))
				return fail("asset drag to properties begins");
			vgui_input_mouse_up(ui, 1, ppx + ppw * 0.5f, ppy + pph * 0.5f, 1);
			if (assigned != "Cube.mesh")
				return fail("asset drop reaches property grid");
		}		test_row_model model;
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
