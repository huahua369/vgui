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
#include "outliner_view.h"
#include "property_grid.h"
#include "viewport_view.h"
#include "vgui.h"

namespace vgui_editor_kit {

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
		vgui_widget* bottom_tabs() const noexcept;

		outliner_view& scene() noexcept;
		property_grid& properties() noexcept;
		asset_browser& assets() noexcept;
		viewport_view& viewport() noexcept;

		void set_status(std::string text);

	private:
		static void on_undo(vgui_widget* widget, const vgui_event* event, void* user);
		static void on_redo(vgui_widget* widget, const vgui_event* event, void* user);

		editor_context& context_;
		vgui_widget* root_ = nullptr;
		vgui_widget* status_ = nullptr;
		vgui_widget* body_splitter_ = nullptr;
		vgui_widget* workspace_splitter_ = nullptr;
		vgui_widget* bottom_tabs_ = nullptr;

		outliner_view scene_;
		property_grid properties_;
		asset_browser assets_;
		viewport_view viewport_;
	};

} /* namespace vgui_editor_kit */
