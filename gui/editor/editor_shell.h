#pragma once
/*
 * editor_shell.h - MVP shell for vgui_editor_kit.
 *
 * The shell is assembled from a core Dock: Outliner, Viewport, Properties and
 * the bottom panel are dock panels with stable ids, so hosts and users can
 * move them between the left/right/bottom/centre zones, merge them into tab
 * groups, float them and restore the arrangement after a restart.  The bottom
 * panel keeps its ASSETS / CONSOLE TabView.
 */
#include <memory>
#include <string>
#include <vector>

#include "asset_browser.h"
#include "breadcrumb.h"
#include "command_registry.h"
#include "editor_context.h"
#include "editor_layout.h"
#include "outliner_view.h"
#include "property_grid.h"
#include "search_box.h"
#include "status_bar.h"
#include "toolbar.h"
#include "viewport_view.h"
#include "vgui.h"

namespace vgui_editor_kit {

	enum class editor_panel
	{
		outliner,
		viewport,
		properties,
		bottom
	};

	class editor_shell
	{
	public:
		explicit editor_shell(editor_context& context);
		/* The shell must be destroyed before the vgui context it was built on. */
		~editor_shell();

		editor_shell(const editor_shell&) = delete;
		editor_shell& operator=(const editor_shell&) = delete;

		void build();

		bool built() const noexcept;
		vgui_widget* widget() const noexcept;

		/* Menu bar and command registry.  Hosts may override a default command
		 * by re-adding its id, then call refresh_commands() so menus pick up the
		 * new title/state.  run_command() also refreshes menu state. */
		vgui_widget* menubar() const noexcept;
		command_registry& commands() noexcept;
		const command_registry& commands() const noexcept;
		bool run_command(const char* id);
		void refresh_commands();

		/* Toolbar and status strip.  The toolbar is bound to the shell's
		 * command registry; the status bar mirrors set_status() and the current
		 * editor selection. */
		toolbar_view& toolbar() noexcept;
		status_bar& statusbar() noexcept;
		search_box& search() noexcept;
		breadcrumb& path() noexcept;

		/* The shell's Dock.  Panel ids are "outliner", "viewport",
		 * "properties" and "bottom".  Use the vgui_dock_* API to rearrange,
		 * float or query them. */
		vgui_widget* dock() const noexcept;

		/* Legacy Splitter handles.  The shell now uses a Dock, so these return
		 * nullptr; they remain for source compatibility.  Use dock() instead. */
		vgui_widget* body_splitter() const noexcept;
		vgui_widget* workspace_splitter() const noexcept;
		vgui_widget* center_splitter() const noexcept;

		/* The ASSETS / CONSOLE TabView inside the bottom dock panel. */
		vgui_widget* bottom_tabs() const noexcept;

		void set_panel_visible(editor_panel panel, bool visible);
		bool panel_visible(editor_panel panel) const;

		/* Layout persistence.  capture_layout() records the Dock zone sizes,
		 * panel order/open state/active tabs/floating rectangles, panel
		 * visibility and bottom-tab order; apply_layout() restores it.  Loading
		 * a damaged or version-mismatched file falls back to the layout
		 * captured immediately after build().  Version-1 splitter layouts are
		 * still accepted and mapped onto the Dock. */
		editor_layout capture_layout() const;
		bool apply_layout(const editor_layout& layout, std::string* error = nullptr);
		bool save_layout(const std::string& path, std::string* error = nullptr) const;
		bool load_layout(const std::string& path, std::string* error = nullptr);
		bool reset_layout();

		outliner_view& scene() noexcept;
		property_grid& properties() noexcept;
		asset_browser& assets() noexcept;
		viewport_view& viewport() noexcept;

		void set_status(std::string text);

	private:
		struct menu_binding
		{
			vgui_widget* menu = nullptr;
			vgui_item_id item = 0;
			std::string command;
		};

		static void on_menu_activate(vgui_widget* menu, vgui_item_id item, void* user);
		static void on_menu_open(vgui_widget* menu, void* user);
		static void on_dock_change(vgui_widget* dock, vgui_item_id item, void* user);
		static int on_key(vgui_context* ctx, int key, int down, int mods, void* user);

		void register_default_commands();
		void build_menus();
		void bind_menu_command(vgui_widget* menu, vgui_item_id item, const char* command);
		vgui_item_id add_menu_command(vgui_widget* menu, const char* command, bool checkable);
		void update_selection_ui(object_id id);

		vgui_item_id panel_item(editor_panel panel) const noexcept;

		void capture_bottom_tabs(editor_layout& layout) const;
		void capture_dock_state(editor_layout& layout) const;
		void apply_bottom_tabs(const editor_layout& layout);
		void apply_dock_sizes(const editor_layout& layout);
		void apply_legacy_ratios(const editor_layout& layout);
		void apply_dock_panels(const editor_layout& layout);

		editor_context& context_;
		command_registry commands_;
		std::vector<std::unique_ptr<menu_binding>> menu_bindings_;
		vgui_widget* menubar_ = nullptr;
		editor_context::selection_listener_id selection_listener_ = 0;
		int key_handler_token_ = 0;
		vgui_widget* root_ = nullptr;
		vgui_widget* dock_ = nullptr;
		vgui_item_id outliner_item_ = 0;
		vgui_item_id viewport_item_ = 0;
		vgui_item_id properties_item_ = 0;
		vgui_item_id bottom_item_ = 0;
		vgui_widget* bottom_tabs_ = nullptr;
		vgui_widget* outliner_panel_ = nullptr;
		vgui_widget* viewport_panel_ = nullptr;
		vgui_widget* properties_panel_ = nullptr;
		vgui_widget* bottom_panel_ = nullptr;
		bool applying_layout_ = false;
		editor_layout default_layout_;

		outliner_view scene_;
		property_grid properties_;
		asset_browser assets_;
		viewport_view viewport_;
		toolbar_view toolbar_;
		status_bar statusbar_;
		search_box search_;
		breadcrumb path_;
	};

} /* namespace vgui_editor_kit */
