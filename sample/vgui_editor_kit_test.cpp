// sample/vgui_editor_kit_test.cpp
//
// Headless smoke test for the first vgui_editor_kit skeleton:
// command stack, context selection, shell panes, outliner/property/asset
// components and the viewport widget boundary.
//
// The test deliberately creates a vgui context without a font or GPU.  That
// keeps it runnable in CI while still exercising widget construction, layout
// and model callbacks.

#include <cstdio>
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

} // namespace

int main()
{
	using namespace vgui_editor_kit;

	if (int rc = test_command_stack())
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
