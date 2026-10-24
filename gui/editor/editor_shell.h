#pragma once
/*
 * editor_shell.h - MVP shell for vgui_editor_kit.
 *
 * The shell is assembled from core Splitter and TabView controls: an outer
 * vertical splitter separates the workspace from the bottom panel, and nested
 * horizontal splitters form the Outliner / Viewport / Properties columns.
 */
#include <string>

#include "asset_browser.h"
#include "editor_context.h"
#include "editor_layout.h"
#include "outliner_view.h"
#include "property_grid.h"
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

		void build();

		bool built() const noexcept;
		vgui_widget* widget() const noexcept;

		/* Layout handles are exposed for host-side layout persistence and for
		 * tests; components below remain the preferred editing API. */
		vgui_widget* body_splitter() const noexcept;
		vgui_widget* workspace_splitter() const noexcept;
		vgui_widget* center_splitter() const noexcept;
		vgui_widget* bottom_tabs() const noexcept;

		void set_panel_visible(editor_panel panel, bool visible);
		bool panel_visible(editor_panel panel) const;

		/* Layout persistence.  capture_layout() records the current splitter
		 * ratios, panel visibility and bottom-tab order; apply_layout() restores
		 * it.  Loading a damaged or version-mismatched file falls back to the
		 * layout captured immediately after build(). */
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
		static void on_undo(vgui_widget* widget, const vgui_event* event, void* user);
		static void on_redo(vgui_widget* widget, const vgui_event* event, void* user);

		vgui_widget* panel_widget(editor_panel panel) const noexcept;

		editor_context& context_;
		vgui_widget* root_ = nullptr;
		vgui_widget* status_ = nullptr;
		vgui_widget* body_splitter_ = nullptr;
		vgui_widget* workspace_splitter_ = nullptr;
		vgui_widget* center_splitter_ = nullptr;
		vgui_widget* bottom_tabs_ = nullptr;
		vgui_widget* outliner_panel_ = nullptr;
		vgui_widget* viewport_panel_ = nullptr;
		vgui_widget* properties_panel_ = nullptr;
		vgui_widget* bottom_panel_ = nullptr;
		editor_layout default_layout_;

		outliner_view scene_;
		property_grid properties_;
		asset_browser assets_;
		viewport_view viewport_;
	};

} /* namespace vgui_editor_kit */
