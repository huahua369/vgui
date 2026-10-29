#pragma once
/*
 * editor_shell.h - MVP shell for vgui_editor_kit.
 *
 * The shell is assembled from a core Dock: Outliner, Viewport, Properties and
 * the bottom panel are dock panels with stable ids, so hosts and users can
 * move them between the left/right/bottom/centre zones, merge them into tab
 * groups, float them and restore the arrangement after a restart.  The bottom
 * panel keeps its ASSETS / CONSOLE / CANVAS / TABLE / TIMELINE / NODEGRAPH /
 * CODE TabView.
 */
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "asset_browser.h"
#include "asset_preview.h"
#include "breadcrumb.h"
#include "canvas_view.h"
#include "code_editor.h"
#include "command_registry.h"
#include "console_view.h"
#include "editor_context.h"
#include "editor_layout.h"
#include "node_graph_view.h"
#include "outliner_view.h"
#include "property_grid.h"
#include "search_box.h"
#include "status_bar.h"
#include "table_view.h"
#include "timeline_view.h"
#include "toolbar.h"
#include "viewport_camera.h"
#include "viewport_gizmo.h"
#include "viewport_grid.h"
#include "viewport_selection.h"
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

	/* V09: per-view render request mirrored by the shell.  The shell does not
	 * own a GPU device; the host applies these values to its renderer each
	 * frame and can also change them from its own UI. */
	struct viewport_render_settings
	{
		bool hdr = false;
		/* 1 or 4; the host may clamp to what the device supports. */
		int msaa_samples = 1;
		bool depth_preview = true;
	};

	/* S08: document lifecycle hooks.  The File menu and shortcuts dispatch
	 * through these callbacks, so a host can connect the shell to its own
	 * document model without replacing individual commands.  Unset
	 * callbacks keep the shell's status-only fallback, which also preserves
	 * hosts that only override command ids through commands().add(). */
	struct document_handlers
	{
		std::function<void()> new_document;
		std::function<void()> open_document;
		std::function<void()> save_document;
		std::function<void()> save_document_as;
		std::function<void()> exit_editor;
		/* Return true when the host deleted (or otherwise handled) the
		 * selection.  Returning false lets the shell fall back to clearing the
		 * selection, which keeps the default edit.delete behaviour. */
		std::function<bool(const std::vector<object_id>&)> delete_selection;
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

		/* S08: install/replace document lifecycle callbacks.  Passing an empty
		 * struct restores the status-only defaults. */
		void set_document_handlers(document_handlers handlers);
		const document_handlers& document_handler_state() const noexcept;

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

		/* The ASSETS / CONSOLE / CANVAS / TABLE / TIMELINE / NODEGRAPH TabView
		 * inside the bottom dock panel. */
		vgui_widget* bottom_tabs() const noexcept;

		/* A07: the log console hosted by the CONSOLE tab.  Hosts append from
		 * any thread, install a jump handler for error locations, and call
		 * tick()/poll() if the console is not being drawn. */
		console_view& console() noexcept;
		const console_view& console() const noexcept;

		/* V08: standalone 2D canvas hosted by the CANVAS tab.  The shell wires
		 * its snapping policy and command stack; hosts own the item model. */
		canvas_view& canvas() noexcept;
		const canvas_view& canvas() const noexcept;

		/* T01: virtualized, editable table hosted by the TABLE tab.  The shell
		 * wires its command stack and persists widths/sort through
		 * editor_layout; hosts install the data source and columns. */
		table_view& table() noexcept;
		const table_view& table() const noexcept;

		/* T02: animation timeline hosted by the TIMELINE tab.  The shell wires
		 * its command stack and persists its playhead/view settings through
		 * editor_layout; hosts populate tracks and drive playback with tick(). */
		timeline_view& timeline() noexcept;
		const timeline_view& timeline() const noexcept;

		/* T03: node graph editor hosted by the NODEGRAPH tab.  The shell wires
		 * its command stack and persists pan/zoom/grid/selection through
		 * editor_layout; hosts populate the graph and may install a create-node
		 * handler for context-menu Add Node. */
		node_graph_view& node_graph() noexcept;
		const node_graph_view& node_graph() const noexcept;

		/* T04: syntax-highlighted code editor / rich log hosted by the CODE
		 * tab.  The shell wires its command stack and persists language,
		 * caret, scroll and presentation through editor_layout; hosts supply
		 * the document with set_text()/open_file(). */
		code_editor& code() noexcept;
		const code_editor& code() const noexcept;

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

		/* The shell enables the outliner row buttons and installs default
		 * visibility/lock icons; call scene().set_row_button_icons() to
		 * replace them. */
		outliner_view& scene() noexcept;
		property_grid& properties() noexcept;
		asset_browser& assets() noexcept;
		asset_preview& preview() noexcept;
		viewport_view& viewport() noexcept;
		/* V09: a second camera surface and a depth-preview surface hosted under
		 * the main viewport.  They are independent viewport_view instances with
		 * their own textures; the host renders a second camera into one and a
		 * depth visualization into the other. */
		viewport_view& preview_viewport() noexcept;
		viewport_view& depth_viewport() noexcept;
		/* V02: the shell attaches an orbit camera to the viewport by default.
		 * Hosts can disable navigation, re-map the buttons or detach it.  The
		 * preview viewport gets its own camera so both views navigate
		 * independently. */
		viewport_camera& camera() noexcept;
		const viewport_camera& camera() const noexcept;
		viewport_camera& preview_camera() noexcept;
		const viewport_camera& preview_camera() const noexcept;
		/* V09: HDR/MSAA/depth-preview requests shared with the host through
		 * view.hdr, view.msaa and view.depth. */
		viewport_render_settings& viewport_settings() noexcept;
		const viewport_render_settings& viewport_settings() const noexcept;
		void set_viewport_settings(const viewport_render_settings& settings);
		/* V03: left-button picking/box selection.  Hosts install the point/box
		 * pickers and the highlight provider; selection state lives in
		 * editor_context and is mirrored by the Outliner. */
		viewport_selection& picking() noexcept;
		const viewport_selection& picking() const noexcept;
		/* V05/V07: translate/rotate/scale gizmo.  The shell attaches it before
		 * picking so its left-button hit test wins; hosts install the
		 * frame/projection/ray providers and the transform handler. */
		viewport_gizmo& gizmo() noexcept;
		const viewport_gizmo& gizmo() const noexcept;
		/* V04: world grid settings/geometry and the snapping policy shared with
		 * future gizmos.  The host renders grid().build() itself. */
		viewport_grid& grid() noexcept;
		const viewport_grid& grid() const noexcept;
		snap_settings& snap() noexcept;
		const snap_settings& snap() const noexcept;

		/* E05: shell-wide high-contrast palette.  Enabling stores the current
		 * context theme, installs vgui_theme_high_contrast(), and moves the
		 * shell-owned chrome (root/menubar/panels) to black/white/yellow.
		 * Disabling restores the captured theme. */
		bool high_contrast() const noexcept;
		void set_high_contrast(bool enabled);

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
		void update_selection_summary(const std::vector<object_id>& ids);

		/* E05 keyboard navigation: F6 / Shift+F6 cycles the four dock panels.
		 * Panels that are closed are revealed first; empty panels are skipped. */
		void focus_next_panel(int direction);
		void apply_shell_palette();
		void install_accessibility_names();
		/* O01: default 16x16 eye/lock textures for the outliner row buttons. */
		void build_row_button_icons();
		void release_row_button_icons();

		vgui_item_id panel_item(editor_panel panel) const noexcept;

		void capture_bottom_tabs(editor_layout& layout) const;
		void capture_dock_state(editor_layout& layout) const;
		void apply_bottom_tabs(const editor_layout& layout);
		void apply_dock_sizes(const editor_layout& layout);
		void apply_legacy_ratios(const editor_layout& layout);
		void apply_dock_panels(const editor_layout& layout);

		editor_context& context_;
		command_registry commands_;
		document_handlers document_handlers_;
		std::vector<std::unique_ptr<menu_binding>> menu_bindings_;
		vgui_widget* menubar_ = nullptr;
		editor_context::selection_listener_id selection_listener_ = 0;
		editor_context::selection_set_listener_id selection_set_listener_ = 0;
		asset_browser::selection_listener_id asset_selection_listener_ = 0;
		int key_handler_token_ = 0;
		vgui_widget* root_ = nullptr;
		vgui_widget* dock_ = nullptr;
		vgui_widget* title_label_ = nullptr;
		vgui_widget* view_strip_ = nullptr;
		bool high_contrast_ = false;
		bool has_saved_theme_ = false;
		vgui_theme saved_theme_ = {};
		vgui_item_id outliner_item_ = 0;
		vgui_item_id viewport_item_ = 0;
		vgui_item_id properties_item_ = 0;
		vgui_item_id bottom_item_ = 0;
		vgui_widget* bottom_tabs_ = nullptr;
		vgui_item_id console_tab_ = 0;
		vgui_item_id canvas_tab_ = 0;
		vgui_item_id table_tab_ = 0;
		vgui_item_id timeline_tab_ = 0;
		vgui_item_id node_graph_tab_ = 0;
		vgui_item_id code_tab_ = 0;
		vgui_widget* outliner_panel_ = nullptr;
		vgui_widget* viewport_panel_ = nullptr;
		vgui_widget* properties_panel_ = nullptr;
		vgui_widget* bottom_panel_ = nullptr;
		bool applying_layout_ = false;
		editor_layout default_layout_;

		outliner_view scene_;
		vgui_texture* row_icon_visible_on_ = nullptr;
		vgui_texture* row_icon_visible_off_ = nullptr;
		vgui_texture* row_icon_locked_on_ = nullptr;
		vgui_texture* row_icon_locked_off_ = nullptr;
		property_grid properties_;
		asset_browser assets_;
		asset_preview preview_;
		viewport_view viewport_;
		viewport_view preview_viewport_;
		viewport_view depth_viewport_;
		/* Declared after the viewports so they detach before the surfaces die. */
		viewport_camera camera_;
		viewport_camera preview_camera_;
		viewport_gizmo gizmo_;
		viewport_selection picking_;
		viewport_grid grid_;
		snap_settings snap_;
		viewport_render_settings viewport_settings_;
		toolbar_view toolbar_;
		status_bar statusbar_;
		search_box search_;
		breadcrumb path_;
		console_view console_;
		canvas_view canvas_;
		table_view table_;
		timeline_view timeline_;
		node_graph_view node_graph_;
		code_editor code_;
	};

} /* namespace vgui_editor_kit */
