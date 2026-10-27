#pragma once
/*
 * editor_layout.h - persistent editor shell layout.
 *
 * Layout is deliberately separate from scene/document data: the host can
 * store it next to user preferences and restoring a scene never overwrites the
 * user's panel arrangement.  The JSON format carries a version so incompatible
 * files can be rejected cleanly in favour of the built-in default layout.
 *
 * Version 1 stored splitter ratios, panel visibility and bottom-tab order.
 * Version 2 adds the Dock state (zone sizes, per-zone order, active tabs,
 * open state and floating rectangles).  Version 3 adds the editor table state
 * (column widths and sort) as an opaque JSON string so the layout format does
 * not need to track the table schema.  Version 4 adds the timeline state
 * (view range, playhead, duration, snapping and looping) as another opaque
 * JSON string.  Version 5 adds the node graph state (pan, zoom, grid, minimap,
 * snapping and selection) as an opaque JSON string.  Version 6 adds the code
 * editor state (language, caret, scroll and presentation) as another opaque
 * JSON string.  Older files are still accepted and mapped onto the current
 * format so existing user layouts keep working.
 */
#include <array>
#include <string>
#include <vector>

#include "vgui.h"

namespace vgui_editor_kit {

	struct editor_dock_panel_layout
	{
		/* Stable string id passed to vgui_dock_panel_desc::id. */
		std::string id;
		vgui_dock_zone zone = VGUI_DOCK_CENTER;
		bool open = true;

		/* Floating rectangle.  Coordinates are absolute pixels; the rectangle
		 * is only used when zone is VGUI_DOCK_FLOATING and has_floating_rect
		 * is true.  Otherwise the dock style defaults apply. */
		bool has_floating_rect = false;
		float x = 0.0f;
		float y = 0.0f;
		float width = 0.0f;
		float height = 0.0f;
	};

	struct editor_layout
	{
		static constexpr int current_version = 6;

		int version = current_version;

		/* Legacy splitter ratios.  Version-2 captures still write them for
		 * hosts that read the fields directly, while version-1 files use them
		 * to derive the initial Dock zone sizes.  They are stored as fractions
		 * of the main axis and clamped to 0..1 during restore. */
		float body_ratio = 0.75f;
		float workspace_ratio = 0.20f;
		float center_ratio = 0.70f;

		bool outliner_visible = true;
		bool viewport_visible = true;
		bool properties_visible = true;
		bool bottom_visible = true;

		/* Titles are used as stable panel identities for the shell's fixed
		 * bottom tabs.  Unknown titles are ignored during restore. */
		std::vector<std::string> bottom_tab_order;
		std::string bottom_active_tab;

		/* Dock state (version 2).  A negative zone size means "use the style
		 * default"; captured layouts always store concrete pixel sizes. */
		float dock_left_size = -1.0f;
		float dock_right_size = -1.0f;
		float dock_bottom_size = -1.0f;

		/* Panels are stored in restore order: zones first (left, right,
		 * bottom, centre), then floating frames from bottom to top.  The order
		 * inside each zone is the tab order, and closed panels are included so
		 * reopening them restores their position. */
		std::vector<editor_dock_panel_layout> dock_panels;

		/* Active panel ids for the four dock zones, indexed by
		 * vgui_dock_zone (left, right, bottom, centre).  Empty entries mean no
		 * active panel or a collapsed zone. */
		std::array<std::string, 4> dock_active{};

		/* Version 3: opaque table_state_to_json() payload owned by the TABLE
		 * bottom tab.  An empty string leaves the current table state. */
		std::string table_state;

		/* Version 4: opaque timeline_state_to_json() payload owned by the
		 * TIMELINE bottom tab.  An empty string leaves the current timeline
		 * state. */
		std::string timeline_state;

		/* Version 5: opaque node_graph_state_to_json() payload owned by the
		 * NODEGRAPH bottom tab.  An empty string leaves the current graph state. */
		std::string node_graph_state;

		/* Version 6: opaque code_editor_state_to_json() payload owned by the
		 * CODE bottom tab.  An empty string leaves the current editor state. */
		std::string code_editor_state;
	};

	std::string editor_layout_to_json(const editor_layout& layout);
	bool editor_layout_from_json(const std::string& text, editor_layout& out,
		std::string* error = nullptr);

	bool editor_layout_load_file(const std::string& path, editor_layout& out,
		std::string* error = nullptr);
	bool editor_layout_save_file(const std::string& path, const editor_layout& layout,
		std::string* error = nullptr);

} /* namespace vgui_editor_kit */
