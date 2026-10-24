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
		constexpr vgui_color kMutedText = 0xFF7F8B9C;
		constexpr vgui_color kStatusText = 0xFF9CC7A4;

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
	}

	editor_shell::editor_shell(editor_context& context)
		: context_(context)
	{
		register_default_commands();
	}

	editor_shell::~editor_shell()
	{
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

	bool editor_shell::run_command(const char* id)
	{
		bool ran = commands_.invoke(id);
		refresh_commands();
		return ran;
	}

	void editor_shell::refresh_commands()
	{
		if (undo_button_)
			vgui_widget_set_enabled(undo_button_, commands_.is_enabled("edit.undo") ? 1 : 0);
		if (redo_button_)
			vgui_widget_set_enabled(redo_button_, commands_.is_enabled("edit.redo") ? 1 : 0);

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

		vgui_widget* header = vgui_widget_add(root_, VGUI_PANEL, nullptr);
		vgui_widget_set_size(header, 0, 34);
		vgui_widget_set_align_self(header, VGUI_ALIGN_STRETCH);
		vgui_widget_set_layout(header, VGUI_LAYOUT_ROW);
		vgui_widget_set_padding(header, 8, 0, 8, 0);
		vgui_widget_set_gap(header, 8);
		vgui_widget_set_background(header, kHeaderBg);
		vgui_widget_set_border(header, kBorder, 1);

		vgui_widget* title = vgui_widget_add(header, VGUI_LABEL,
			"vgui_editor_kit  ·  MVP skeleton");
		vgui_widget_set_grow(title, 1.0f);
		vgui_widget_set_size(title, 0, 24);
		vgui_widget_set_font_size(title, 14);
		vgui_widget_set_text_color(title, kTitleText);
		vgui_widget_set_text_align(title, 0.0f, 0.5f);

		undo_button_ = vgui_widget_add(header, VGUI_BUTTON, "Undo");
		vgui_widget_set_size(undo_button_, 58, 24);
		vgui_widget_set_font_size(undo_button_, 12);
		vgui_widget_on_event(undo_button_, &editor_shell::on_undo, this);

		redo_button_ = vgui_widget_add(header, VGUI_BUTTON, "Redo");
		vgui_widget_set_size(redo_button_, 58, 24);
		vgui_widget_set_font_size(redo_button_, 12);
		vgui_widget_on_event(redo_button_, &editor_shell::on_redo, this);

		status_ = vgui_widget_add(header, VGUI_LABEL, "ready");
		vgui_widget_set_size(status_, 220, 24);
		vgui_widget_set_font_size(status_, 12);
		vgui_widget_set_text_color(status_, kStatusText);
		vgui_widget_set_text_align(status_, 1.0f, 0.5f);

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
		desc.id = "viewport";
		desc.title = "VIEWPORT";
		desc.panel = viewport_panel_;
		desc.zone = VGUI_DOCK_CENTER;
		viewport_item_ = vgui_dock_add(dock_, &desc);

		properties_panel_ = make_dock_page(dock_);
		properties_.build(properties_panel_);
		properties_.set_command_stack(&context_.commands());
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
		vgui_widget_set_padding(console_page, 8, 8, 8, 8);
		vgui_widget* console_text = vgui_widget_add(console_page, VGUI_LABEL, "No messages.");
		vgui_widget_set_font_size(console_text, 12);
		vgui_widget_set_text_color(console_text, kMutedText);
		vgui_item_id console_tab = vgui_tab_add(bottom_tabs_, "CONSOLE", console_page);
		vgui_tab_set_closeable(bottom_tabs_, console_tab, 0);

		desc.id = "bottom";
		desc.title = "PANELS";
		desc.panel = bottom_panel_;
		desc.zone = VGUI_DOCK_BOTTOM;
		bottom_item_ = vgui_dock_add(dock_, &desc);

		build_menus();
		if (context_.ui())
			key_handler_token_ = vgui_context_add_key_handler(context_.ui(),
				&editor_shell::on_key, this);
		vgui_dock_on_change(dock_, &editor_shell::on_dock_change, this);
		refresh_commands();

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

		layout.dock_left_size = vgui_dock_get_zone_size(dock_, VGUI_DOCK_LEFT);
		layout.dock_right_size = vgui_dock_get_zone_size(dock_, VGUI_DOCK_RIGHT);
		layout.dock_bottom_size = vgui_dock_get_zone_size(dock_, VGUI_DOCK_BOTTOM);
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
				panel.x = x;
				panel.y = y;
				panel.width = width;
				panel.height = height;
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

		/* Keep the legacy splitter ratios meaningful for hosts that read them
		 * directly.  They are derived from the Dock zone sizes. */
		float dx = 0.0f, dy = 0.0f, dw = 0.0f, dh = 0.0f;
		vgui_widget_get_rect(dock_, &dx, &dy, &dw, &dh);
		(void)dx;
		(void)dy;
		if (dw > 0.0f)
		{
			const float left = std::max(0.0f, layout.dock_left_size);
			const float right = std::max(0.0f, layout.dock_right_size);
			const float center = std::max(0.0f, dw - left - right);
			layout.workspace_ratio = std::clamp(left / dw, 0.0f, 1.0f);
			if (center + right > 0.0f)
				layout.center_ratio = std::clamp(center / (center + right), 0.0f, 1.0f);
		}
		if (dh > 0.0f)
		{
			const float bottom = std::max(0.0f, layout.dock_bottom_size);
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

		if (dw > 0.0f)
		{
			const float left = std::clamp(layout.workspace_ratio, 0.05f, 0.60f) * dw;
			const float right = (1.0f - std::clamp(layout.center_ratio, 0.10f, 0.90f))
				* std::max(0.0f, dw - left);
			vgui_dock_set_zone_size(dock_, VGUI_DOCK_LEFT, left);
			vgui_dock_set_zone_size(dock_, VGUI_DOCK_RIGHT, right);
		}
		if (dh > 0.0f)
		{
			const float bottom = (1.0f - std::clamp(layout.body_ratio, 0.20f, 0.90f)) * dh;
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

	viewport_view& editor_shell::viewport() noexcept
	{
		return viewport_;
	}

	void editor_shell::set_status(std::string text)
	{
		if (status_)
			vgui_widget_set_text(status_, text.c_str());
	}

	void editor_shell::on_undo(vgui_widget* widget, const vgui_event* event, void* user)
	{
		(void)widget;
		if (event && event->type == VGUI_EVENT_CLICK)
		{
			auto* self = static_cast<editor_shell*>(user);
			if (!self->run_command("edit.undo"))
				self->set_status("nothing to undo");
		}
	}

	void editor_shell::on_redo(vgui_widget* widget, const vgui_event* event, void* user)
	{
		(void)widget;
		if (event && event->type == VGUI_EVENT_CLICK)
		{
			auto* self = static_cast<editor_shell*>(user);
			if (!self->run_command("edit.redo"))
				self->set_status("nothing to redo");
		}
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
