#include "editor_shell.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace vgui_editor_kit {

	namespace {
		constexpr vgui_color kRootBg = 0xFF14171C;
		constexpr vgui_color kHeaderBg = 0xFF1E232B;
		constexpr vgui_color kSectionBg = 0xFF1B1E24;
		constexpr vgui_color kBorder = 0xFF2E3440;
		constexpr vgui_color kTitleText = 0xFFB8C4D6;

		vgui_widget* make_dock_page(vgui_widget* dock)
		{
			vgui_widget* page = vgui_widget_add(dock, VGUI_PANEL, nullptr);
			vgui_widget_set_layout(page, VGUI_LAYOUT_COLUMN);
			vgui_widget_set_padding(page, 0, 0, 0, 0);
			vgui_widget_set_gap(page, 0);
			vgui_widget_set_background(page, kSectionBg);
			return page;
		}

		float finite_or(float value, float fallback)
		{
			return std::isfinite(value) ? value : fallback;
		}

		// E05: first focusable, visible and enabled descendant (pre-order).
		vgui_widget* first_focusable_descendant(vgui_widget* widget)
		{
			if (!widget || !vgui_widget_get_visible(widget) ||
				!vgui_widget_get_enabled(widget))
				return nullptr;
			if (vgui_widget_get_focusable(widget))
				return widget;
			const std::size_t count = vgui_widget_get_child_count(widget);
			for (std::size_t i = 0; i < count; ++i)
			{
				if (vgui_widget* hit = first_focusable_descendant(
					vgui_widget_get_child(widget, i)))
					return hit;
			}
			return nullptr;
		}

		bool widget_within_root(vgui_widget* root, vgui_widget* widget)
		{
			if (!root || !widget)
				return false;
			for (vgui_widget* p = widget; p; p = vgui_widget_get_parent(p))
				if (p == root)
					return true;
			return false;
		}
	}

	editor_shell::editor_shell(editor_context& context)
		: context_(context)
	{
		register_default_commands();
		selection_listener_ = context_.add_selection_listener(
			[this](object_id id) { update_selection_ui(id); });
		selection_set_listener_ = context_.add_selection_set_listener(
			[this](const std::vector<object_id>& ids) {
				update_selection_summary(ids);
			});
		update_selection_ui(context_.selection());
		update_selection_summary(context_.selection_ids());
	}

	editor_shell::~editor_shell()
	{
		if (selection_listener_)
			context_.remove_selection_listener(selection_listener_);
		selection_listener_ = 0;
		if (selection_set_listener_)
			context_.remove_selection_set_listener(selection_set_listener_);
		selection_set_listener_ = 0;
		if (asset_selection_listener_)
			assets_.remove_selection_listener(asset_selection_listener_);
		asset_selection_listener_ = 0;
		if (key_handler_token_ && context_.ui())
			vgui_context_remove_key_handler(context_.ui(), key_handler_token_);
		key_handler_token_ = 0;
	}

	void editor_shell::register_default_commands()
	{
		auto shortcut = [](const char* text) {
			return command_shortcut::parse(text);
		};

		commands_.add("file.save", "Save", [this]() {
			set_status("saved");
		}).shortcut = shortcut("Ctrl+S");

		commands_.add("file.exit", "Exit", [this]() {
			set_status("exit requested");
		});

		commands_.add("edit.undo", "Undo", [this]() {
			set_status(context_.commands().undo() ? "undo" : "nothing to undo");
		}).shortcut = shortcut("Ctrl+Z");
		commands_.set_enabled("edit.undo", [this]() {
			return context_.commands().can_undo();
		});

		commands_.add("edit.redo", "Redo", [this]() {
			set_status(context_.commands().redo() ? "redo" : "nothing to redo");
		}).shortcut = shortcut("Ctrl+Y");
		commands_.set_enabled("edit.redo", [this]() {
			return context_.commands().can_redo();
		});

		commands_.add("edit.delete", "Delete", [this]() {
			context_.clear_selection();
			set_status("delete selection");
		}).shortcut = shortcut("Delete");
		commands_.set_enabled("edit.delete", [this]() {
			return context_.selection() != invalid_object;
		});

		/* P08:属性复制/粘贴(作用于属性面板当前行集)。 */
		commands_.add("edit.copy_properties", "Copy Properties", [this]() {
			const std::size_t count = properties_.copy_properties();
			set_status(count ? "copied " + std::to_string(count)
				+ " properties" : "nothing to copy");
		}).shortcut = shortcut("Ctrl+Shift+C");
		commands_.set_enabled("edit.copy_properties", [this]() {
			return properties_.row_count() > 0;
		});

		commands_.add("edit.paste_properties", "Paste Properties", [this]() {
			const std::size_t count = properties_.paste_properties();
			set_status(count ? "pasted " + std::to_string(count)
				+ " properties" : "no matching properties");
		}).shortcut = shortcut("Ctrl+Shift+V");
		commands_.set_enabled("edit.paste_properties", [this]() {
			return property_grid::clipboard_property_count() > 0;
		});

		auto add_panel_toggle = [this](const char* id, const char* title,
			editor_panel panel)
		{
			commands_.add(id, title, [this, panel]() {
				set_panel_visible(panel, !panel_visible(panel));
			});
			commands_.set_checked(id, [this, panel]() {
				return panel_visible(panel);
			});
		};
		add_panel_toggle("view.outliner", "Outliner", editor_panel::outliner);
		add_panel_toggle("view.viewport", "Viewport", editor_panel::viewport);
		add_panel_toggle("view.properties", "Properties", editor_panel::properties);
		add_panel_toggle("view.bottom", "Bottom Panel", editor_panel::bottom);

		/* E05: keyboard-only panel cycling and high-contrast presentation. */
		commands_.add("view.focus.next", "Focus Next Panel", [this]() {
			focus_next_panel(1);
		}).shortcut = shortcut("F6");
		commands_.add("view.focus.prev", "Focus Previous Panel", [this]() {
			focus_next_panel(-1);
		}).shortcut = shortcut("Shift+F6");
		commands_.add("view.high_contrast", "High Contrast", [this]() {
			set_high_contrast(!high_contrast_);
		}).shortcut = shortcut("Ctrl+Shift+H");
		commands_.set_checked("view.high_contrast", [this]() {
			return high_contrast_;
		});

		/* A07: reveal and focus the log console even when the bottom panel or
		 * another bottom tab is active. */
		commands_.add("view.console", "Console", [this]() {
			set_panel_visible(editor_panel::bottom, true);
			if (console_tab_ && bottom_tabs_)
				vgui_tab_set_active(bottom_tabs_, console_tab_);
			if (context_.ui() && console_.tree())
				vgui_set_focus(context_.ui(), console_.tree());
			set_status("console");
		});
		commands_.set_checked("view.console", [this]() {
			return console_tab_ && bottom_tabs_ &&
				vgui_tab_get_active(bottom_tabs_) == console_tab_ &&
				panel_visible(editor_panel::bottom);
		});

		/* V08: reveal and focus the standalone 2D canvas. */
		commands_.add("view.canvas", "Canvas", [this]() {
			set_panel_visible(editor_panel::bottom, true);
			if (canvas_tab_ && bottom_tabs_)
				vgui_tab_set_active(bottom_tabs_, canvas_tab_);
			if (canvas_.built())
				canvas_.request_focus();
			set_status("canvas");
		});
		commands_.set_checked("view.canvas", [this]() {
			return canvas_tab_ && bottom_tabs_ &&
				vgui_tab_get_active(bottom_tabs_) == canvas_tab_ &&
				panel_visible(editor_panel::bottom);
		});

		/* T01: reveal and focus the editable table tab. */
		commands_.add("view.table", "Table", [this]() {
			set_panel_visible(editor_panel::bottom, true);
			if (table_tab_ && bottom_tabs_)
				vgui_tab_set_active(bottom_tabs_, table_tab_);
			if (table_.built())
				table_.focus();
			set_status("table");
		});
		commands_.set_checked("view.table", [this]() {
			return table_tab_ && bottom_tabs_ &&
				vgui_tab_get_active(bottom_tabs_) == table_tab_ &&
				panel_visible(editor_panel::bottom);
		});

		/* T02: reveal and focus the animation timeline tab. */
		commands_.add("view.timeline", "Timeline", [this]() {
			set_panel_visible(editor_panel::bottom, true);
			if (timeline_tab_ && bottom_tabs_)
				vgui_tab_set_active(bottom_tabs_, timeline_tab_);
			if (timeline_.built())
				timeline_.request_focus();
			set_status("timeline");
		});
		commands_.set_checked("view.timeline", [this]() {
			return timeline_tab_ && bottom_tabs_ &&
				vgui_tab_get_active(bottom_tabs_) == timeline_tab_ &&
				panel_visible(editor_panel::bottom);
		});

		/* T03: reveal and focus the node graph tab. */
		commands_.add("view.nodegraph", "Node Graph", [this]() {
			set_panel_visible(editor_panel::bottom, true);
			if (node_graph_tab_ && bottom_tabs_)
				vgui_tab_set_active(bottom_tabs_, node_graph_tab_);
			if (node_graph_.built())
				node_graph_.request_focus();
			set_status("node graph");
		});
		commands_.set_checked("view.nodegraph", [this]() {
			return node_graph_tab_ && bottom_tabs_ &&
				vgui_tab_get_active(bottom_tabs_) == node_graph_tab_ &&
				panel_visible(editor_panel::bottom);
		});

		/* T04: reveal and focus the code editor tab. */
		commands_.add("view.code", "Code", [this]() {
			set_panel_visible(editor_panel::bottom, true);
			if (code_tab_ && bottom_tabs_)
				vgui_tab_set_active(bottom_tabs_, code_tab_);
			if (code_.built())
				code_.request_focus();
			set_status("code");
		});
		commands_.set_checked("view.code", [this]() {
			return code_tab_ && bottom_tabs_ &&
				vgui_tab_get_active(bottom_tabs_) == code_tab_ &&
				panel_visible(editor_panel::bottom);
		});

		/* V04: 网格可见性、吸附总开关与网格尺寸。 */
		commands_.add("view.grid", "Grid", [this]() {
			grid_.set_visible(!grid_.visible());
			set_status(grid_.visible() ? "grid on" : "grid off");
		});
		commands_.set_checked("view.grid", [this]() {
			return grid_.visible();
		});

		commands_.add("view.snap", "Snapping", [this]() {
			snap_.enabled = !snap_.enabled;
			set_status(snap_.enabled ? "snapping on" : "snapping off");
		});
		commands_.set_checked("view.snap", [this]() {
			return snap_.enabled;
		});

		commands_.add("view.grid.auto", "Auto", [this]() {
			grid_.set_auto_density(true);
			set_status("grid auto");
		});
		commands_.set_checked("view.grid.auto", [this]() {
			return grid_.auto_density();
		});

		auto add_grid_size = [this](const char* id, const char* title,
			const char* label, float size)
		{
			commands_.add(id, title, [this, size, label]() {
				grid_.set_auto_density(false);
				grid_.set_cell_size(size);
				snap_.translate_step = size;
				set_status(std::string("grid ") + label);
			});
			commands_.set_checked(id, [this, size]() {
				return !grid_.auto_density() &&
					std::fabs(grid_.cell_size() - size) < 1e-4f;
			});
		};
		add_grid_size("view.grid.0.5", "0.5", "0.5", 0.5f);
		add_grid_size("view.grid.1", "1", "1", 1.0f);
		add_grid_size("view.grid.2", "2", "2", 2.0f);
		add_grid_size("view.grid.5", "5", "5", 5.0f);

		/* V05/V07: Gizmo 可见性、工具模式与世界/局部/父级/屏幕空间。 */
		commands_.add("view.gizmo", "Gizmo", [this]() {
			gizmo_.set_visible(!gizmo_.visible());
			set_status(gizmo_.visible() ? "gizmo on" : "gizmo off");
		});
		commands_.set_checked("view.gizmo", [this]() {
			return gizmo_.visible();
		});

		auto add_gizmo_mode = [this](const char* id, const char* title,
			gizmo_mode mode, const char* key)
		{
			commands_.add(id, title, [this, mode, title]() {
				gizmo_.set_mode(mode);
				set_status(std::string("gizmo ") + title + " tool");
			});
			commands_.set_checked(id, [this, mode]() {
				return gizmo_.mode() == mode;
			});
			commands_.set_shortcut(id, command_shortcut::parse(key));
		};
		add_gizmo_mode("view.gizmo.translate", "Translate", gizmo_mode::translate,
			"W");
		add_gizmo_mode("view.gizmo.rotate", "Rotate", gizmo_mode::rotate, "E");
		add_gizmo_mode("view.gizmo.scale", "Scale", gizmo_mode::scale, "R");

		/* V05 compatibility: this command still toggles between world and local.
		 * The discrete space commands below expose parent/screen as well. */
		commands_.add("view.gizmo.local", "Local Space", [this]() {
			const bool local = gizmo_.space() == gizmo_space::local;
			gizmo_.set_space(local ? gizmo_space::world : gizmo_space::local);
			set_status(local ? "gizmo world space" : "gizmo local space");
		});
		commands_.set_checked("view.gizmo.local", [this]() {
			return gizmo_.space() == gizmo_space::local;
		});

		auto add_gizmo_space = [this](const char* id, const char* title,
			gizmo_space space)
		{
			commands_.add(id, title, [this, space, title]() {
				gizmo_.set_space(space);
				set_status(std::string("gizmo ") + title + " space");
			});
			commands_.set_checked(id, [this, space]() {
				return gizmo_.space() == space;
			});
		};
		add_gizmo_space("view.gizmo.space.world", "World", gizmo_space::world);
		add_gizmo_space("view.gizmo.space.local", "Local", gizmo_space::local);
		add_gizmo_space("view.gizmo.space.parent", "Parent", gizmo_space::parent);
		add_gizmo_space("view.gizmo.space.screen", "Screen", gizmo_space::screen);

		/* V09: runtime render switches.  The shell only records the request;
		 * the host applies it to its phase0 views and may clamp MSAA. */
		commands_.add("view.hdr", "HDR", [this]() {
			viewport_render_settings settings = viewport_settings_;
			settings.hdr = !settings.hdr;
			set_viewport_settings(settings);
			set_status(settings.hdr ? "hdr on" : "hdr off");
		});
		commands_.set_checked("view.hdr", [this]() {
			return viewport_settings_.hdr;
		});

		commands_.add("view.msaa", "MSAA 4x", [this]() {
			viewport_render_settings settings = viewport_settings_;
			settings.msaa_samples = settings.msaa_samples > 1 ? 1 : 4;
			set_viewport_settings(settings);
			set_status(settings.msaa_samples > 1 ? "msaa 4x on" : "msaa off");
		});
		commands_.set_checked("view.msaa", [this]() {
			return viewport_settings_.msaa_samples > 1;
		});

		commands_.add("view.depth", "Depth Preview", [this]() {
			viewport_render_settings settings = viewport_settings_;
			settings.depth_preview = !settings.depth_preview;
			set_viewport_settings(settings);
			set_status(settings.depth_preview ? "depth preview on" :
				"depth preview off");
		});
		commands_.set_checked("view.depth", [this]() {
			return viewport_settings_.depth_preview;
		});

		commands_.add("view.reset_layout", "Reset Layout", [this]() {
			reset_layout();
			set_status("layout reset");
		});

		commands_.add("help.about", "About vgui_editor_kit", [this]() {
			set_status("vgui_editor_kit");
		});
	}

	vgui_widget* editor_shell::menubar() const noexcept
	{
		return menubar_;
	}

	command_registry& editor_shell::commands() noexcept
	{
		return commands_;
	}

	const command_registry& editor_shell::commands() const noexcept
	{
		return commands_;
	}

	toolbar_view& editor_shell::toolbar() noexcept
	{
		return toolbar_;
	}

	status_bar& editor_shell::statusbar() noexcept
	{
		return statusbar_;
	}

	search_box& editor_shell::search() noexcept
	{
		return search_;
	}

	breadcrumb& editor_shell::path() noexcept
	{
		return path_;
	}

	bool editor_shell::run_command(const char* id)
	{
		bool ran = commands_.invoke(id);
		refresh_commands();
		return ran;
	}

	void editor_shell::refresh_commands()
	{
		toolbar_.refresh_commands();

		for (const std::unique_ptr<menu_binding>& binding : menu_bindings_)
		{
			if (!binding || !binding->menu || !binding->item)
				continue;
			vgui_menu_set_item_enabled(binding->menu, binding->item,
				commands_.is_enabled(binding->command.c_str()) ? 1 : 0);
			vgui_menu_set_item_checked(binding->menu, binding->item,
				commands_.is_checked(binding->command.c_str()) ? 1 : 0);
		}
	}

	void editor_shell::bind_menu_command(vgui_widget* menu, vgui_item_id item,
		const char* command)
	{
		if (!menu || !item || !command || !*command)
			return;
		auto binding = std::make_unique<menu_binding>();
		binding->menu = menu;
		binding->item = item;
		binding->command = command;
		vgui_menu_set_item_user(menu, item, binding.get());
		menu_bindings_.push_back(std::move(binding));
	}

	vgui_item_id editor_shell::add_menu_command(vgui_widget* menu, const char* command,
		bool checkable)
	{
		const command_entry* entry = commands_.find(command);
		if (!menu || !entry)
			return 0;

		vgui_item_id item = checkable
			? vgui_menu_add_check_item(menu, entry->title.c_str(),
				commands_.is_checked(command) ? 1 : 0)
			: vgui_menu_add_item(menu, entry->title.c_str());
		if (!item)
			return 0;

		std::string shortcut = entry->shortcut.to_string();
		if (!shortcut.empty())
			vgui_menu_set_item_shortcut(menu, item, shortcut.c_str());
		bind_menu_command(menu, item, command);
		return item;
	}

	void editor_shell::build_menus()
	{
		if (!menubar_ || !context_.ui())
			return;
		menu_bindings_.clear();

		auto wire_menu = [this](vgui_widget* menu) {
			vgui_menu_on_activate(menu, &editor_shell::on_menu_activate, this);
			vgui_menu_on_open(menu, &editor_shell::on_menu_open, this);
		};

		vgui_widget* file_menu = vgui_widget_create(context_.ui(), VGUI_MENU, nullptr);
		if (file_menu)
		{
			wire_menu(file_menu);
			add_menu_command(file_menu, "file.save", false);
			vgui_menu_add_separator(file_menu);
			add_menu_command(file_menu, "file.exit", false);
			vgui_menubar_add_menu(menubar_, "File", file_menu);
		}

		vgui_widget* edit_menu = vgui_widget_create(context_.ui(), VGUI_MENU, nullptr);
		if (edit_menu)
		{
			wire_menu(edit_menu);
			add_menu_command(edit_menu, "edit.undo", false);
			add_menu_command(edit_menu, "edit.redo", false);
			vgui_menu_add_separator(edit_menu);
			add_menu_command(edit_menu, "edit.copy_properties", false);
			add_menu_command(edit_menu, "edit.paste_properties", false);
			vgui_menu_add_separator(edit_menu);
			add_menu_command(edit_menu, "edit.delete", false);
			vgui_menubar_add_menu(menubar_, "Edit", edit_menu);
		}

		vgui_widget* view_menu = vgui_widget_create(context_.ui(), VGUI_MENU, nullptr);
		if (view_menu)
		{
			wire_menu(view_menu);
			add_menu_command(view_menu, "view.outliner", true);
			add_menu_command(view_menu, "view.viewport", true);
			add_menu_command(view_menu, "view.properties", true);
			add_menu_command(view_menu, "view.bottom", true);
			add_menu_command(view_menu, "view.console", true);
			add_menu_command(view_menu, "view.canvas", true);
			add_menu_command(view_menu, "view.table", true);
			add_menu_command(view_menu, "view.timeline", true);
			add_menu_command(view_menu, "view.nodegraph", true);
			add_menu_command(view_menu, "view.code", true);
			vgui_menu_add_separator(view_menu);
			add_menu_command(view_menu, "view.focus.next", false);
			add_menu_command(view_menu, "view.focus.prev", false);
			vgui_menu_add_separator(view_menu);
			add_menu_command(view_menu, "view.grid", true);
			add_menu_command(view_menu, "view.snap", true);
			add_menu_command(view_menu, "view.gizmo", true);
			add_menu_command(view_menu, "view.gizmo.translate", true);
			add_menu_command(view_menu, "view.gizmo.rotate", true);
			add_menu_command(view_menu, "view.gizmo.scale", true);
			vgui_widget* gizmo_space_menu = nullptr;
			if (vgui_menu_add_submenu(view_menu, "Gizmo Space",
				&gizmo_space_menu) && gizmo_space_menu)
			{
				wire_menu(gizmo_space_menu);
				add_menu_command(gizmo_space_menu,
					"view.gizmo.space.world", true);
				add_menu_command(gizmo_space_menu,
					"view.gizmo.space.local", true);
				add_menu_command(gizmo_space_menu,
					"view.gizmo.space.parent", true);
				add_menu_command(gizmo_space_menu,
					"view.gizmo.space.screen", true);
			}
			vgui_widget* grid_size_menu = nullptr;
			if (vgui_menu_add_submenu(view_menu, "Grid Size", &grid_size_menu) &&
				grid_size_menu)
			{
				wire_menu(grid_size_menu);
				add_menu_command(grid_size_menu, "view.grid.auto", true);
				vgui_menu_add_separator(grid_size_menu);
				add_menu_command(grid_size_menu, "view.grid.0.5", true);
				add_menu_command(grid_size_menu, "view.grid.1", true);
				add_menu_command(grid_size_menu, "view.grid.2", true);
				add_menu_command(grid_size_menu, "view.grid.5", true);
			}
			vgui_menu_add_separator(view_menu);
			/* V09 */
			add_menu_command(view_menu, "view.hdr", true);
			add_menu_command(view_menu, "view.msaa", true);
			add_menu_command(view_menu, "view.depth", true);
			vgui_menu_add_separator(view_menu);
			add_menu_command(view_menu, "view.high_contrast", true);
			vgui_menu_add_separator(view_menu);
			add_menu_command(view_menu, "view.reset_layout", false);
			vgui_menubar_add_menu(menubar_, "View", view_menu);
		}

		vgui_widget* help_menu = vgui_widget_create(context_.ui(), VGUI_MENU, nullptr);
		if (help_menu)
		{
			wire_menu(help_menu);
			add_menu_command(help_menu, "help.about", false);
			vgui_menubar_add_menu(menubar_, "Help", help_menu);
		}

		refresh_commands();
	}

	void editor_shell::build()
	{
		if (root_)
			return;

		vgui_widget* host = context_.root();
		if (!host)
			return;

		vgui_widget_set_layout(host, VGUI_LAYOUT_COLUMN);
		vgui_widget_set_padding(host, 0, 0, 0, 0);
		vgui_widget_set_gap(host, 0);
		vgui_widget_set_align_items(host, VGUI_ALIGN_STRETCH);

		root_ = vgui_widget_add(host, VGUI_PANEL, nullptr);
		vgui_widget_set_grow(root_, 1.0f);
		vgui_widget_set_layout(root_, VGUI_LAYOUT_COLUMN);
		vgui_widget_set_padding(root_, 4, 4, 4, 4);
		vgui_widget_set_gap(root_, 4);
		vgui_widget_set_background(root_, kRootBg);

		menubar_ = vgui_widget_add(root_, VGUI_MENUBAR, nullptr);
		vgui_widget_set_size(menubar_, 0, 26);
		vgui_widget_set_align_self(menubar_, VGUI_ALIGN_STRETCH);
		vgui_widget_set_background(menubar_, kHeaderBg);
		vgui_widget_set_border(menubar_, kBorder, 1);

		toolbar_.build(root_);
		title_label_ = toolbar_.add_label(
			"vgui_editor_kit  ·  MVP skeleton", true);
		vgui_widget_set_font_size(title_label_, 14);
		vgui_widget_set_text_color(title_label_, kTitleText);

		search_.build(toolbar_.widget(), "Search scene");
		search_.set_on_change([this](const std::string& text) {
			scene_.set_filter(text.c_str());
			set_status(text.empty() ? "ready" : "search: " + text);
		});
		search_.set_on_submit([this](const std::string& text) {
			scene_.set_filter(text.c_str());
			set_status(text.empty() ? "search cleared" : "search: " + text);
		});

		toolbar_.add_command_button(commands_, "edit.undo");
		toolbar_.add_command_button(commands_, "edit.redo");
		toolbar_.add_separator();

		/* V04: 网格/吸附开关与菜单共用同一命令状态。 */
		toolbar_.add_command_button(commands_, "view.grid");
		toolbar_.add_command_button(commands_, "view.snap");

		segmented_control& view_mode = toolbar_.add_segmented_control();
		view_mode.add_segment("Persp");
		view_mode.add_segment("Top");
		view_mode.add_segment("Front");
		view_mode.set_on_change([this](std::size_t index) {
			static const char* names[] = { "perspective", "top", "front" };
			if (index < 3)
				set_status(std::string("view ") + names[index]);
		});

		dock_ = vgui_widget_add(root_, VGUI_DOCK, nullptr);
		vgui_widget_set_grow(dock_, 1.0f);
		vgui_widget_set_align_self(dock_, VGUI_ALIGN_STRETCH);
		vgui_dock_set_zone_size(dock_, VGUI_DOCK_LEFT, 230.0f);
		vgui_dock_set_zone_size(dock_, VGUI_DOCK_RIGHT, 300.0f);
		vgui_dock_set_zone_size(dock_, VGUI_DOCK_BOTTOM, 180.0f);

		vgui_dock_panel_desc desc;
		vgui_dock_panel_desc_default(&desc);
		desc.closeable = 1;

		outliner_panel_ = make_dock_page(dock_);
		vgui_widget_set_padding(outliner_panel_, 4, 4, 4, 4);
		scene_.build(context_, outliner_panel_);
		desc.id = "outliner";
		desc.title = "OUTLINER";
		desc.panel = outliner_panel_;
		desc.zone = VGUI_DOCK_LEFT;
		outliner_item_ = vgui_dock_add(dock_, &desc);

		viewport_panel_ = make_dock_page(dock_);
		viewport_.build(viewport_panel_);
		/* 面板是 COLUMN 布局：视口需要显式拉伸横向，否则只会占 min-size 宽。 */
		vgui_widget_set_align_self(viewport_.widget(), VGUI_ALIGN_STRETCH);
		/* V02: 视口默认带轨道相机——中/右键导航、滚轮缩放、F 聚焦。 */
		camera_.attach(viewport_);
		/* V05: Gizmo 先于拾取挂载，左键命中手柄时先消费事件。 */
		gizmo_.attach(viewport_);
		gizmo_.set_snap_settings(&snap_);
		/* V03: 左键拾取/框选，选择集写入 editor_context 并与 Outliner 同步。 */
		picking_.attach(viewport_, context_);
		/* V09: 主视口下方并排一个第二相机视口和一个深度预览视口。两者都是
		 * 独立 viewport_view，各自绑定宿主提供的纹理；深度预览关闭时只保留
		 * 第二相机视口。 */
		vgui_widget_set_gap(viewport_panel_, 4);
		vgui_widget* view_strip = vgui_widget_add(viewport_panel_, VGUI_PANEL,
			nullptr);
		view_strip_ = view_strip;
		vgui_widget_set_layout(view_strip, VGUI_LAYOUT_ROW);
		vgui_widget_set_padding(view_strip, 0, 0, 0, 0);
		vgui_widget_set_gap(view_strip, 4);
		vgui_widget_set_size(view_strip, 0, 150);
		vgui_widget_set_align_self(view_strip, VGUI_ALIGN_STRETCH);
		vgui_widget_set_background(view_strip, kSectionBg);

		preview_viewport_.build(view_strip);
		vgui_widget_set_min_size(preview_viewport_.widget(), 120, 80);
		vgui_widget_set_align_self(preview_viewport_.widget(),
			VGUI_ALIGN_STRETCH);
		/* 第二个相机可独立导航，不与主视口共享状态。 */
		preview_camera_.attach(preview_viewport_);

		depth_viewport_.build(view_strip);
		vgui_widget_set_min_size(depth_viewport_.widget(), 120, 80);
		vgui_widget_set_align_self(depth_viewport_.widget(),
			VGUI_ALIGN_STRETCH);

		desc.id = "viewport";
		desc.title = "VIEWPORT";
		desc.panel = viewport_panel_;
		desc.zone = VGUI_DOCK_CENTER;
		viewport_item_ = vgui_dock_add(dock_, &desc);

		properties_panel_ = make_dock_page(dock_);
		path_.build(properties_panel_);
		vgui_widget_set_padding(path_.widget(), 6, 0, 6, 0);
		properties_.build(properties_panel_);
		properties_.set_command_stack(&context_.commands());
		/* A03: the selected-asset preview sits below the property grid so the
		 * four dock panels (and their persisted layout) stay unchanged. */
		preview_.build(properties_panel_);
		desc.id = "properties";
		desc.title = "PROPERTIES";
		desc.panel = properties_panel_;
		desc.zone = VGUI_DOCK_RIGHT;
		properties_item_ = vgui_dock_add(dock_, &desc);

		bottom_panel_ = make_dock_page(dock_);
		bottom_tabs_ = vgui_widget_add(bottom_panel_, VGUI_TABVIEW, nullptr);
		vgui_widget_set_grow(bottom_tabs_, 1.0f);
		vgui_widget_set_min_size(bottom_tabs_, 0, 80);
		vgui_tab_set_tab_height(bottom_tabs_, 26.0f);

		vgui_widget* assets_page = vgui_widget_add(bottom_tabs_, VGUI_PANEL, nullptr);
		vgui_widget_set_layout(assets_page, VGUI_LAYOUT_COLUMN);
		vgui_widget_set_padding(assets_page, 6, 6, 6, 6);
		assets_.build(assets_page);
		vgui_item_id assets_tab = vgui_tab_add(bottom_tabs_, "ASSETS", assets_page);
		vgui_tab_set_closeable(bottom_tabs_, assets_tab, 0);

		vgui_widget* console_page = vgui_widget_add(bottom_tabs_, VGUI_PANEL, nullptr);
		vgui_widget_set_layout(console_page, VGUI_LAYOUT_COLUMN);
		vgui_widget_set_padding(console_page, 6, 4, 6, 4);
		console_.build(console_page);
		console_tab_ = vgui_tab_add(bottom_tabs_, "CONSOLE", console_page);
		vgui_tab_set_closeable(bottom_tabs_, console_tab_, 0);

		vgui_widget* canvas_page = vgui_widget_add(bottom_tabs_, VGUI_PANEL, nullptr);
		vgui_widget_set_layout(canvas_page, VGUI_LAYOUT_COLUMN);
		vgui_widget_set_padding(canvas_page, 0, 0, 0, 0);
		canvas_.build(canvas_page);
		vgui_widget_set_align_self(canvas_.widget(), VGUI_ALIGN_STRETCH);
		canvas_.set_snap_settings(&snap_);
		canvas_.set_command_stack(&context_.commands());
		canvas_tab_ = vgui_tab_add(bottom_tabs_, "CANVAS", canvas_page);
		vgui_tab_set_closeable(bottom_tabs_, canvas_tab_, 0);

		vgui_widget* table_page = vgui_widget_add(bottom_tabs_, VGUI_PANEL, nullptr);
		vgui_widget_set_layout(table_page, VGUI_LAYOUT_COLUMN);
		vgui_widget_set_padding(table_page, 6, 4, 6, 4);
		table_.build(table_page);
		vgui_widget_set_align_self(table_.widget(), VGUI_ALIGN_STRETCH);
		table_.set_command_stack(&context_.commands());
		table_tab_ = vgui_tab_add(bottom_tabs_, "TABLE", table_page);
		vgui_tab_set_closeable(bottom_tabs_, table_tab_, 0);

		vgui_widget* timeline_page = vgui_widget_add(bottom_tabs_, VGUI_PANEL, nullptr);
		vgui_widget_set_layout(timeline_page, VGUI_LAYOUT_COLUMN);
		vgui_widget_set_padding(timeline_page, 0, 0, 0, 0);
		timeline_.build(timeline_page);
		vgui_widget_set_align_self(timeline_.widget(), VGUI_ALIGN_STRETCH);
		timeline_.set_command_stack(&context_.commands());
		timeline_tab_ = vgui_tab_add(bottom_tabs_, "TIMELINE", timeline_page);
		vgui_tab_set_closeable(bottom_tabs_, timeline_tab_, 0);

		vgui_widget* node_graph_page = vgui_widget_add(bottom_tabs_,
			VGUI_PANEL, nullptr);
		vgui_widget_set_layout(node_graph_page, VGUI_LAYOUT_COLUMN);
		vgui_widget_set_padding(node_graph_page, 0, 0, 0, 0);
		node_graph_.build(node_graph_page);
		vgui_widget_set_align_self(node_graph_.widget(), VGUI_ALIGN_STRETCH);
		node_graph_.set_command_stack(&context_.commands());
		node_graph_tab_ = vgui_tab_add(bottom_tabs_, "NODEGRAPH",
			node_graph_page);
		vgui_tab_set_closeable(bottom_tabs_, node_graph_tab_, 0);

		vgui_widget* code_page = vgui_widget_add(bottom_tabs_, VGUI_PANEL,
			nullptr);
		vgui_widget_set_layout(code_page, VGUI_LAYOUT_COLUMN);
		vgui_widget_set_padding(code_page, 0, 0, 0, 0);
		code_.build(code_page);
		vgui_widget_set_align_self(code_.widget(), VGUI_ALIGN_STRETCH);
		code_.set_command_stack(&context_.commands());
		code_tab_ = vgui_tab_add(bottom_tabs_, "CODE", code_page);
		vgui_tab_set_closeable(bottom_tabs_, code_tab_, 0);

		desc.id = "bottom";
		desc.title = "PANELS";
		desc.panel = bottom_panel_;
		desc.zone = VGUI_DOCK_BOTTOM;
		bottom_item_ = vgui_dock_add(dock_, &desc);

		statusbar_.build(root_);
		update_selection_ui(context_.selection());

		build_menus();
		if (context_.ui())
			key_handler_token_ = vgui_context_add_key_handler(context_.ui(),
				&editor_shell::on_key, this);
		vgui_dock_on_change(dock_, &editor_shell::on_dock_change, this);
		refresh_commands();

		// F03: 三条拖放路径的默认接线——Outliner 行拖拽重排（走命令栈）、
		// 资产行拖拽、资产拖到属性栏 / 视口的状态反馈。
		scene_.enable_drag_drop();
		// M3: Outliner 双向选中同步 + 行内编辑/右键菜单默认可用;宿主可
		// 通过 scene() 进一步设置 move/rename/action handler。
		/* V03: 视口多选需要树保持多选模式,Ctrl/Shift 才能在两侧一致。 */
		scene_.set_multi_select(true);
		scene_.enable_selection_sync();
		scene_.enable_inline_rename();
		scene_.enable_context_menu();
		scene_.enable_row_buttons();
		assets_.enable_row_drag();
		/* A03: keep the preview in sync with the asset selection.  A separate
		 * listener leaves the host's legacy selection callback untouched. */
		asset_selection_listener_ = assets_.add_selection_listener(
			[this](vgui_item_id item)
			{
				if (!item)
				{
					preview_.clear();
					return;
				}
				asset_info info;
				if (assets_.item_info(item, &info))
					preview_.set_item(item, info.name, info.type);
				else
					preview_.set_item(item);
			});
		viewport_.set_drop_listener([this](const drag_payload& payload, float, float) -> bool {
			if (payload.id.empty())
				return false;
			set_status("drop " + payload.id);
			return true;
		});
		properties_.set_asset_drop_handler([this](const std::string& asset) -> bool {
			set_status("assign " + asset);
			return true;
		});

		/* Reflect the initial V09 settings (depth preview on) in the auxiliary
		 * surface visibility and menu checks. */
		set_viewport_settings(viewport_settings_);
		install_accessibility_names();

		default_layout_ = capture_layout();
	}

	bool editor_shell::built() const noexcept
	{
		return root_ != nullptr;
	}

	vgui_widget* editor_shell::widget() const noexcept
	{
		return root_;
	}

	vgui_widget* editor_shell::dock() const noexcept
	{
		return dock_;
	}

	vgui_widget* editor_shell::body_splitter() const noexcept
	{
		return nullptr;
	}

	vgui_widget* editor_shell::workspace_splitter() const noexcept
	{
		return nullptr;
	}

	vgui_widget* editor_shell::center_splitter() const noexcept
	{
		return nullptr;
	}

	vgui_widget* editor_shell::bottom_tabs() const noexcept
	{
		return bottom_tabs_;
	}

	console_view& editor_shell::console() noexcept
	{
		return console_;
	}

	const console_view& editor_shell::console() const noexcept
	{
		return console_;
	}

	canvas_view& editor_shell::canvas() noexcept
	{
		return canvas_;
	}

	const canvas_view& editor_shell::canvas() const noexcept
	{
		return canvas_;
	}

	table_view& editor_shell::table() noexcept
	{
		return table_;
	}

	const table_view& editor_shell::table() const noexcept
	{
		return table_;
	}

	timeline_view& editor_shell::timeline() noexcept
	{
		return timeline_;
	}

	const timeline_view& editor_shell::timeline() const noexcept
	{
		return timeline_;
	}

	node_graph_view& editor_shell::node_graph() noexcept
	{
		return node_graph_;
	}

	const node_graph_view& editor_shell::node_graph() const noexcept
	{
		return node_graph_;
	}

	code_editor& editor_shell::code() noexcept
	{
		return code_;
	}

	const code_editor& editor_shell::code() const noexcept
	{
		return code_;
	}

	vgui_item_id editor_shell::panel_item(editor_panel panel) const noexcept
	{
		switch (panel)
		{
		case editor_panel::outliner: return outliner_item_;
		case editor_panel::viewport: return viewport_item_;
		case editor_panel::properties: return properties_item_;
		case editor_panel::bottom: return bottom_item_;
		}
		return 0;
	}

	void editor_shell::set_panel_visible(editor_panel panel, bool visible)
	{
		if (!dock_)
			return;
		vgui_item_id item = panel_item(panel);
		if (!item)
			return;
		const bool open = vgui_dock_is_open(dock_, item) != 0;
		if (open == visible)
		{
			refresh_commands();
			return;
		}
		if (visible)
			vgui_dock_restore(dock_, item);
		else
			vgui_dock_close(dock_, item);
		refresh_commands();
	}

	bool editor_shell::panel_visible(editor_panel panel) const
	{
		if (!dock_)
			return false;
		vgui_item_id item = panel_item(panel);
		return item != 0 && vgui_dock_is_open(dock_, item) != 0;
	}

	void editor_shell::capture_bottom_tabs(editor_layout& layout) const
	{
		if (!bottom_tabs_)
			return;
		int count = vgui_tab_get_count(bottom_tabs_);
		for (int i = 0; i < count; ++i)
		{
			vgui_item_id item = vgui_tab_get_at(bottom_tabs_, i);
			layout.bottom_tab_order.emplace_back(vgui_tab_get_title(bottom_tabs_, item));
		}
		vgui_item_id active = vgui_tab_get_active(bottom_tabs_);
		if (active)
			layout.bottom_active_tab = vgui_tab_get_title(bottom_tabs_, active);
	}

	void editor_shell::capture_dock_state(editor_layout& layout) const
	{
		if (!dock_)
			return;

		/* E01: layout files are DPI independent.  Dock getters report effective
		 * pixels, so store logical values and let set_zone_size() scale them on
		 * restore. */
		const float scale = context_.ui()
			? std::max(0.01f, vgui_context_get_ui_scale(context_.ui())) : 1.0f;
		layout.dock_left_size =
			vgui_dock_get_zone_size(dock_, VGUI_DOCK_LEFT) / scale;
		layout.dock_right_size =
			vgui_dock_get_zone_size(dock_, VGUI_DOCK_RIGHT) / scale;
		layout.dock_bottom_size =
			vgui_dock_get_zone_size(dock_, VGUI_DOCK_BOTTOM) / scale;
		layout.dock_panels.clear();
		layout.dock_active = {};

		static const vgui_dock_zone zones[4] = {
			VGUI_DOCK_LEFT,
			VGUI_DOCK_RIGHT,
			VGUI_DOCK_BOTTOM,
			VGUI_DOCK_CENTER
		};
		for (int z = 0; z < 4; ++z)
		{
			const int count = vgui_dock_get_zone_count(dock_, zones[z]);
			for (int i = 0; i < count; ++i)
			{
				vgui_item_id item = vgui_dock_get_zone_at(dock_, zones[z], i);
				const char* id = item ? vgui_dock_get_id(dock_, item) : nullptr;
				if (!id || !*id)
					continue;
				editor_dock_panel_layout panel;
				panel.id = id;
				panel.zone = vgui_dock_get_zone(dock_, item);
				panel.open = vgui_dock_is_open(dock_, item) != 0;
				layout.dock_panels.push_back(std::move(panel));
			}
			vgui_item_id active = vgui_dock_get_active(dock_, zones[z]);
			const char* active_id = active ? vgui_dock_get_id(dock_, active) : nullptr;
			if (active_id && *active_id)
				layout.dock_active[(size_t)z] = active_id;
		}

		const int floating = vgui_dock_get_zone_count(dock_, VGUI_DOCK_FLOATING);
		for (int i = 0; i < floating; ++i)
		{
			vgui_item_id item = vgui_dock_get_zone_at(dock_, VGUI_DOCK_FLOATING, i);
			const char* id = item ? vgui_dock_get_id(dock_, item) : nullptr;
			if (!id || !*id)
				continue;
			editor_dock_panel_layout panel;
			panel.id = id;
			panel.zone = VGUI_DOCK_FLOATING;
			panel.open = vgui_dock_is_open(dock_, item) != 0;
			float x = 0.0f, y = 0.0f, width = 0.0f, height = 0.0f;
			if (vgui_dock_get_floating_rect(dock_, item, &x, &y, &width, &height))
			{
				panel.has_floating_rect = width > 0.0f && height > 0.0f;
				panel.x = x / scale;
				panel.y = y / scale;
				panel.width = width / scale;
				panel.height = height / scale;
			}
			layout.dock_panels.push_back(std::move(panel));
		}
	}

	editor_layout editor_shell::capture_layout() const
	{
		editor_layout layout;
		if (!built())
			return layout;

		capture_dock_state(layout);
		layout.outliner_visible = panel_visible(editor_panel::outliner);
		layout.viewport_visible = panel_visible(editor_panel::viewport);
		layout.properties_visible = panel_visible(editor_panel::properties);
		layout.bottom_visible = panel_visible(editor_panel::bottom);
		capture_bottom_tabs(layout);
		if (table_.built())
			layout.table_state = table_state_to_json(table_.capture_state());
		if (timeline_.built())
			layout.timeline_state = timeline_state_to_json(
				timeline_.capture_state());
		if (node_graph_.built())
			layout.node_graph_state = node_graph_state_to_json(
				node_graph_.capture_state());
		if (code_.built())
			layout.code_editor_state = code_editor_state_to_json(
				code_.capture_state());

		/* Keep the legacy splitter ratios meaningful for hosts that read them
		 * directly.  They are derived from the Dock zone sizes. */
		float dx = 0.0f, dy = 0.0f, dw = 0.0f, dh = 0.0f;
		vgui_widget_get_rect(dock_, &dx, &dy, &dw, &dh);
		(void)dx;
		(void)dy;
		const float layout_scale = context_.ui()
			? std::max(0.01f, vgui_context_get_ui_scale(context_.ui())) : 1.0f;
		if (dw > 0.0f)
		{
			const float left = std::max(0.0f, layout.dock_left_size) * layout_scale;
			const float right = std::max(0.0f, layout.dock_right_size) * layout_scale;
			const float center = std::max(0.0f, dw - left - right);
			layout.workspace_ratio = std::clamp(left / dw, 0.0f, 1.0f);
			if (center + right > 0.0f)
				layout.center_ratio = std::clamp(center / (center + right), 0.0f, 1.0f);
		}
		if (dh > 0.0f)
		{
			const float bottom = std::max(0.0f, layout.dock_bottom_size) * layout_scale;
			layout.body_ratio = std::clamp((dh - bottom) / dh, 0.0f, 1.0f);
		}
		return layout;
	}

	void editor_shell::apply_bottom_tabs(const editor_layout& layout)
	{
		if (!bottom_tabs_)
			return;

		int count = vgui_tab_get_count(bottom_tabs_);
		std::vector<vgui_item_id> items((size_t)count, 0);
		std::vector<char> used((size_t)count, 0);
		for (int i = 0; i < count; ++i)
			items[(size_t)i] = vgui_tab_get_at(bottom_tabs_, i);

		std::vector<vgui_item_id> desired;
		desired.reserve((size_t)count);
		for (const std::string& title : layout.bottom_tab_order)
		{
			for (int i = 0; i < count; ++i)
			{
				if (used[(size_t)i]) continue;
				if (title != vgui_tab_get_title(bottom_tabs_, items[(size_t)i])) continue;
				desired.push_back(items[(size_t)i]);
				used[(size_t)i] = 1;
				break;
			}
		}
		for (size_t i = 0; i < desired.size(); ++i)
			vgui_tab_move(bottom_tabs_, desired[i], (int)i);

		if (!layout.bottom_active_tab.empty())
		{
			count = vgui_tab_get_count(bottom_tabs_);
			for (int i = 0; i < count; ++i)
			{
				vgui_item_id item = vgui_tab_get_at(bottom_tabs_, i);
				if (layout.bottom_active_tab != vgui_tab_get_title(bottom_tabs_, item))
					continue;
				vgui_tab_set_active(bottom_tabs_, item);
				break;
			}
		}
	}

	void editor_shell::apply_dock_sizes(const editor_layout& layout)
	{
		if (!dock_)
			return;
		auto size = [this](float value, float fallback) {
			if (std::isfinite(value) && value >= 0.0f)
				return value;
			return fallback;
		};
		vgui_dock_set_zone_size(dock_, VGUI_DOCK_LEFT,
			size(layout.dock_left_size, default_layout_.dock_left_size));
		vgui_dock_set_zone_size(dock_, VGUI_DOCK_RIGHT,
			size(layout.dock_right_size, default_layout_.dock_right_size));
		vgui_dock_set_zone_size(dock_, VGUI_DOCK_BOTTOM,
			size(layout.dock_bottom_size, default_layout_.dock_bottom_size));
	}

	void editor_shell::apply_legacy_ratios(const editor_layout& layout)
	{
		if (!dock_)
			return;

		float dx = 0.0f, dy = 0.0f, dw = 0.0f, dh = 0.0f;
		vgui_widget_get_rect(dock_, &dx, &dy, &dw, &dh);
		(void)dx;
		(void)dy;
		if ((dw <= 0.0f || dh <= 0.0f) && context_.ui())
		{
			int cw = 0, ch = 0;
			vgui_context_get_size(context_.ui(), &cw, &ch);
			if (dw <= 0.0f) dw = (float)cw;
			if (dh <= 0.0f) dh = (float)ch;
		}

		/* Ratios are applied in logical units so legacy layouts keep the same
		 * proportions on every DPI. */
		const float scale = context_.ui()
			? std::max(0.01f, vgui_context_get_ui_scale(context_.ui())) : 1.0f;
		const float logical_w = dw / scale;
		const float logical_h = dh / scale;
		if (logical_w > 0.0f)
		{
			const float left = std::clamp(layout.workspace_ratio, 0.05f, 0.60f) * logical_w;
			const float right = (1.0f - std::clamp(layout.center_ratio, 0.10f, 0.90f))
				* std::max(0.0f, logical_w - left);
			vgui_dock_set_zone_size(dock_, VGUI_DOCK_LEFT, left);
			vgui_dock_set_zone_size(dock_, VGUI_DOCK_RIGHT, right);
		}
		if (logical_h > 0.0f)
		{
			const float bottom = (1.0f - std::clamp(layout.body_ratio, 0.20f, 0.90f)) * logical_h;
			vgui_dock_set_zone_size(dock_, VGUI_DOCK_BOTTOM, bottom);
		}
	}

	void editor_shell::apply_dock_panels(const editor_layout& layout)
	{
		if (!dock_)
			return;

		int next_index[4] = { 0, 0, 0, 0 };
		for (const editor_dock_panel_layout& panel : layout.dock_panels)
		{
			if (panel.id.empty() || panel.zone == VGUI_DOCK_FLOATING)
				continue;
			const int zone = (int)panel.zone;
			if (zone < 0 || zone >= 4)
				continue;
			vgui_item_id item = vgui_dock_find(dock_, panel.id.c_str());
			if (!item)
				continue;
			if (!vgui_dock_set_zone(dock_, item, panel.zone, next_index[zone]))
				continue;
			++next_index[zone];
			if (!panel.open)
				vgui_dock_close(dock_, item);
		}

		for (const editor_dock_panel_layout& panel : layout.dock_panels)
		{
			if (panel.id.empty() || panel.zone != VGUI_DOCK_FLOATING)
				continue;
			vgui_item_id item = vgui_dock_find(dock_, panel.id.c_str());
			if (!item)
				continue;
			const float x = panel.has_floating_rect ? finite_or(panel.x, 0.0f) : 0.0f;
			const float y = panel.has_floating_rect ? finite_or(panel.y, 0.0f) : 0.0f;
			const float width = panel.has_floating_rect ? finite_or(panel.width, 0.0f) : 0.0f;
			const float height = panel.has_floating_rect ? finite_or(panel.height, 0.0f) : 0.0f;
			if (!vgui_dock_float(dock_, item, x, y, width, height))
				continue;
			if (!panel.open)
				vgui_dock_close(dock_, item);
		}

		for (int z = 0; z < 4; ++z)
		{
			const std::string& id = layout.dock_active[(size_t)z];
			if (id.empty())
				continue;
			vgui_item_id item = vgui_dock_find(dock_, id.c_str());
			if (!item || !vgui_dock_is_open(dock_, item))
				continue;
			vgui_dock_activate(dock_, item);
		}
	}

	bool editor_shell::apply_layout(const editor_layout& layout, std::string* error)
	{
		if (!built())
		{
			if (error) *error = "editor shell is not built";
			return false;
		}
		if (layout.version < 1 || layout.version > editor_layout::current_version)
		{
			if (error) *error = "unsupported layout version";
			return false;
		}

		/* Validate the opaque table payload before changing any dock state so a
		 * malformed table blob cannot leave a partially applied layout. */
		table_state loaded_table_state;
		if (!layout.table_state.empty())
		{
			std::string reason;
			if (!table_state_from_json(layout.table_state, loaded_table_state,
				&reason))
			{
				if (error) *error = reason;
				return false;
			}
		}

		/* T02: validate the timeline payload the same way before touching any
		 * dock state. */
		timeline_state loaded_timeline_state;
		if (!layout.timeline_state.empty())
		{
			std::string reason;
			if (!timeline_state_from_json(layout.timeline_state,
				loaded_timeline_state, &reason))
			{
				if (error) *error = reason;
				return false;
			}
		}

		/* T03: validate the node graph payload before changing dock state. */
		node_graph_state loaded_node_graph_state;
		if (!layout.node_graph_state.empty())
		{
			std::string reason;
			if (!node_graph_state_from_json(layout.node_graph_state,
				loaded_node_graph_state, &reason))
			{
				if (error) *error = reason;
				return false;
			}
		}

		/* T04: validate the code editor payload before changing dock state. */
		code_editor_state loaded_code_editor_state;
		if (!layout.code_editor_state.empty())
		{
			std::string reason;
			if (!code_editor_state_from_json(layout.code_editor_state,
				loaded_code_editor_state, &reason))
			{
				if (error) *error = reason;
				return false;
			}
		}

		const bool was_applying = applying_layout_;
		applying_layout_ = true;

		if (context_.ui())
			vgui_layout(context_.ui());

		const bool has_dock_sizes = layout.dock_left_size >= 0.0f
			|| layout.dock_right_size >= 0.0f
			|| layout.dock_bottom_size >= 0.0f;
		if (has_dock_sizes || layout.version >= 2)
			apply_dock_sizes(layout);
		else
			apply_legacy_ratios(layout);

		/* Legacy visibility acts as a fallback for panels that are not present
		 * in the version-2 dock list.  The dock list then overrides it. */
		set_panel_visible(editor_panel::outliner, layout.outliner_visible);
		set_panel_visible(editor_panel::viewport, layout.viewport_visible);
		set_panel_visible(editor_panel::properties, layout.properties_visible);
		set_panel_visible(editor_panel::bottom, layout.bottom_visible);

		if (!layout.dock_panels.empty())
			apply_dock_panels(layout);
		apply_bottom_tabs(layout);

		if (!layout.table_state.empty())
		{
			std::string reason;
			if (!table_.apply_state(loaded_table_state, &reason))
			{
				applying_layout_ = was_applying;
				if (error) *error = reason;
				refresh_commands();
				return false;
			}
		}

		if (!layout.timeline_state.empty())
		{
			std::string reason;
			if (!timeline_.apply_state(loaded_timeline_state, &reason))
			{
				applying_layout_ = was_applying;
				if (error) *error = reason;
				refresh_commands();
				return false;
			}
		}

		if (!layout.node_graph_state.empty())
		{
			std::string reason;
			if (!node_graph_.apply_state(loaded_node_graph_state, &reason))
			{
				applying_layout_ = was_applying;
				if (error) *error = reason;
				refresh_commands();
				return false;
			}
		}

		if (!layout.code_editor_state.empty())
		{
			std::string reason;
			if (!code_.apply_state(loaded_code_editor_state, &reason))
			{
				applying_layout_ = was_applying;
				if (error) *error = reason;
				refresh_commands();
				return false;
			}
		}

		applying_layout_ = was_applying;
		refresh_commands();
		if (context_.ui())
			vgui_layout(context_.ui());
		return true;
	}

	bool editor_shell::save_layout(const std::string& path, std::string* error) const
	{
		if (!built())
		{
			if (error) *error = "editor shell is not built";
			return false;
		}
		return editor_layout_save_file(path, capture_layout(), error);
	}

	bool editor_shell::load_layout(const std::string& path, std::string* error)
	{
		if (!built())
		{
			if (error) *error = "editor shell is not built";
			return false;
		}

		editor_layout layout;
		std::string reason;
		if (!editor_layout_load_file(path, layout, &reason))
		{
			reset_layout();
			if (error) *error = reason;
			return false;
		}
		if (!apply_layout(layout, &reason))
		{
			reset_layout();
			if (error) *error = reason;
			return false;
		}
		if (error) error->clear();
		return true;
	}

	bool editor_shell::reset_layout()
	{
		if (!built())
			return false;
		std::string error;
		return apply_layout(default_layout_, &error);
	}

	outliner_view& editor_shell::scene() noexcept
	{
		return scene_;
	}

	property_grid& editor_shell::properties() noexcept
	{
		return properties_;
	}

	asset_browser& editor_shell::assets() noexcept
	{
		return assets_;
	}

	asset_preview& editor_shell::preview() noexcept
	{
		return preview_;
	}

	viewport_view& editor_shell::viewport() noexcept
	{
		return viewport_;
	}

	viewport_view& editor_shell::preview_viewport() noexcept
	{
		return preview_viewport_;
	}

	viewport_view& editor_shell::depth_viewport() noexcept
	{
		return depth_viewport_;
	}

	viewport_camera& editor_shell::camera() noexcept
	{
		return camera_;
	}

	const viewport_camera& editor_shell::camera() const noexcept
	{
		return camera_;
	}

	viewport_camera& editor_shell::preview_camera() noexcept
	{
		return preview_camera_;
	}

	const viewport_camera& editor_shell::preview_camera() const noexcept
	{
		return preview_camera_;
	}

	viewport_render_settings& editor_shell::viewport_settings() noexcept
	{
		return viewport_settings_;
	}

	const viewport_render_settings& editor_shell::viewport_settings() const noexcept
	{
		return viewport_settings_;
	}

	void editor_shell::set_viewport_settings(
		const viewport_render_settings& settings)
	{
		viewport_settings_ = settings;
		if (viewport_settings_.msaa_samples < 1)
			viewport_settings_.msaa_samples = 1;
		if (depth_viewport_.widget())
		{
			vgui_widget_set_visible(depth_viewport_.widget(),
				viewport_settings_.depth_preview ? 1 : 0);
		}
		refresh_commands();
	}

	viewport_selection& editor_shell::picking() noexcept
	{
		return picking_;
	}

	const viewport_selection& editor_shell::picking() const noexcept
	{
		return picking_;
	}

	viewport_gizmo& editor_shell::gizmo() noexcept
	{
		return gizmo_;
	}

	const viewport_gizmo& editor_shell::gizmo() const noexcept
	{
		return gizmo_;
	}

	viewport_grid& editor_shell::grid() noexcept
	{
		return grid_;
	}

	const viewport_grid& editor_shell::grid() const noexcept
	{
		return grid_;
	}

	snap_settings& editor_shell::snap() noexcept
	{
		return snap_;
	}

	const snap_settings& editor_shell::snap() const noexcept
	{
		return snap_;
	}

	void editor_shell::set_status(std::string text)
	{
		statusbar_.set_message(std::move(text));
	}

	bool editor_shell::high_contrast() const noexcept
	{
		return high_contrast_;
	}

	void editor_shell::set_high_contrast(bool enabled)
	{
		if (enabled == high_contrast_)
			return;
		vgui_context* ui = context_.ui();
		if (!ui)
			return;
		if (enabled)
		{
			vgui_context_get_theme(ui, &saved_theme_);
			has_saved_theme_ = true;
			vgui_theme contrast = {};
			vgui_theme_high_contrast(&contrast);
			vgui_context_set_theme(ui, &contrast);
		}
		else if (has_saved_theme_)
		{
			vgui_context_set_theme(ui, &saved_theme_);
		}
		high_contrast_ = enabled;
		apply_shell_palette();
		refresh_commands();
	}

	void editor_shell::apply_shell_palette()
	{
		const vgui_color root_bg = high_contrast_ ? 0xFF000000 : kRootBg;
		const vgui_color header_bg = high_contrast_ ? 0xFF000000 : kHeaderBg;
		const vgui_color section_bg = high_contrast_ ? 0xFF000000 : kSectionBg;
		const vgui_color border = high_contrast_ ? 0xFFFFFFFF : kBorder;
		const vgui_color title_text = high_contrast_ ? 0xFFFFFFFF : kTitleText;
		if (root_) vgui_widget_set_background(root_, root_bg);
		if (menubar_)
		{
			vgui_widget_set_background(menubar_, header_bg);
			vgui_widget_set_border(menubar_, border, 1);
		}
		vgui_widget* pages[] = { outliner_panel_, viewport_panel_,
			properties_panel_, bottom_panel_ };
		for (vgui_widget* page : pages)
			if (page) vgui_widget_set_background(page, section_bg);
		if (bottom_tabs_)
		{
			/* Tab pages are plain panels created with the shell section color;
			 * theme migration cannot see those hard-coded values. */
			const std::size_t count = vgui_widget_get_child_count(bottom_tabs_);
			for (std::size_t i = 0; i < count; ++i)
			{
				vgui_widget* child = vgui_widget_get_child(bottom_tabs_, i);
				if (child) vgui_widget_set_background(child, section_bg);
			}
		}
		if (view_strip_) vgui_widget_set_background(view_strip_, section_bg);
		if (title_label_) vgui_widget_set_text_color(title_label_, title_text);
	}

	void editor_shell::focus_next_panel(int direction)
	{
		vgui_context* ui = context_.ui();
		if (!ui || !dock_)
			return;
		auto panel_root = [this](editor_panel panel) -> vgui_widget*
		{
			switch (panel)
			{
			case editor_panel::outliner: return scene_.widget();
			case editor_panel::viewport: return viewport_.widget();
			case editor_panel::properties: return properties_.widget();
			case editor_panel::bottom:
			{
				if (bottom_tabs_)
				{
					vgui_item_id active = vgui_tab_get_active(bottom_tabs_);
					if (!active)
					{
						active = vgui_tab_get_at(bottom_tabs_, 0);
						if (active) vgui_tab_set_active(bottom_tabs_, active);
					}
					if (active)
						if (vgui_widget* page = vgui_tab_get_page(bottom_tabs_, active))
							return page;
				}
				return bottom_tabs_;
			}
			default: return nullptr;
			}
		};

		const editor_panel panels[4] = {
			editor_panel::outliner,
			editor_panel::viewport,
			editor_panel::properties,
			editor_panel::bottom
		};

		vgui_widget* focus = vgui_get_focus(ui);
		int current = -1;
		for (int i = 0; i < 4; ++i)
			if (widget_within_root(panel_root(panels[i]), focus))
				current = i;

		int index = current;
		for (int step = 0; step < 4; ++step)
		{
			if (index < 0)
				index = direction > 0 ? 0 : 3;
			else
				index = (index + (direction > 0 ? 1 : 3)) % 4;

			set_panel_visible(panels[index], true);
			const vgui_item_id item = panel_item(panels[index]);
			if (item)
				vgui_dock_activate(dock_, item);
			vgui_layout(ui);
			if (vgui_widget* target = first_focusable_descendant(panel_root(panels[index])))
			{
				vgui_set_focus(ui, target);
				return;
			}
		}
	}

	void editor_shell::install_accessibility_names()
	{
		auto name = [](vgui_widget* widget, const char* text) {
			if (widget) vgui_widget_set_a11y_name(widget, text);
		};
		name(root_, "Editor shell");
		name(menubar_, "Main menu");
		name(toolbar_.widget(), "Toolbar");
		name(statusbar_.widget(), "Status bar");
		name(search_.widget(), "Search scene");
		name(path_.widget(), "Selection path");
		name(outliner_panel_, "Outliner panel");
		name(viewport_panel_, "Viewport panel");
		name(properties_panel_, "Properties panel");
		name(bottom_panel_, "Bottom panel");
		name(bottom_tabs_, "Bottom tabs");
		name(scene_.widget(), "Scene outliner");
		name(viewport_.widget(), "3D viewport");
		name(preview_viewport_.widget(), "Preview viewport");
		name(depth_viewport_.widget(), "Depth preview");
		name(properties_.widget(), "Properties");
		name(assets_.widget(), "Assets");
		name(preview_.widget(), "Asset preview");
		name(console_.tree(), "Console log");
		name(canvas_.widget(), "Canvas");
		name(table_.widget(), "Table");
		name(timeline_.widget(), "Timeline");
		name(node_graph_.widget(), "Node graph");
		name(code_.widget(), "Code editor");
	}

	void editor_shell::update_selection_ui(object_id id)
	{
		statusbar_.set_selected_object(id);

		if (!path_.widget())
			return;
		std::vector<breadcrumb_item> path;
		path.push_back({ "Scene", "scene" });
		if (id != invalid_object)
		{
			const std::string label = "Object " + std::to_string(id);
			path.push_back({ label, "object:" + std::to_string(id) });
		}
		path_.set_items(std::move(path));
	}

	void editor_shell::update_selection_summary(const std::vector<object_id>& ids)
	{
		if (ids.size() <= 1)
		{
			/* 单选保持原有 "Selected: <id>" 文案;0 清空。 */
			statusbar_.set_selected_object(context_.selection());
			return;
		}
		statusbar_.set_selection_text("Selected: " + std::to_string(ids.size()) +
			" objects (primary " + std::to_string(context_.selection()) + ")");
	}

	void editor_shell::on_menu_activate(vgui_widget* menu, vgui_item_id item, void* user)
	{
		auto* self = static_cast<editor_shell*>(user);
		if (!self || !menu || !item)
			return;
		auto* binding = static_cast<menu_binding*>(vgui_menu_get_item_user(menu, item));
		if (!binding || binding->command.empty())
			return;
		self->commands_.invoke(binding->command.c_str());
		self->refresh_commands();
	}

	void editor_shell::on_menu_open(vgui_widget* menu, void* user)
	{
		(void)menu;
		auto* self = static_cast<editor_shell*>(user);
		if (self)
			self->refresh_commands();
	}

	void editor_shell::on_dock_change(vgui_widget* dock, vgui_item_id item, void* user)
	{
		(void)dock;
		(void)item;
		auto* self = static_cast<editor_shell*>(user);
		if (self && !self->applying_layout_)
			self->refresh_commands();
	}

	int editor_shell::on_key(vgui_context* ctx, int key, int down, int mods, void* user)
	{
		(void)ctx;
		if (!user || !down)
			return 0;
		auto* self = static_cast<editor_shell*>(user);
		if (!self->commands_.handle_key(key, down, mods))
			return 0;
		self->refresh_commands();
		return 1;
	}

} /* namespace vgui_editor_kit */
