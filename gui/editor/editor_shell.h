#pragma once
/*
 * editor_shell.h - fixed-pane MVP shell for vgui_editor_kit.
 *
 * This is the starting point for the first development batch.  It uses the
 * existing vgui layout primitives as a temporary stand-in for Splitter/TabView
 * and should keep the component boundaries stable when those controls arrive.
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

		outliner_view scene_;
		property_grid properties_;
		asset_browser assets_;
		viewport_view viewport_;
	};

} /* namespace vgui_editor_kit */
