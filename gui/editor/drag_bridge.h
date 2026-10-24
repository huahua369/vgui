#pragma once
/*
 * drag_bridge.h - bridges the kit's drag_payload / drop_target types to the
 * core C drag & drop API (vgui_begin_drag, vgui_widget_set_drop_target,
 * vgui_list/tree_set_row_drag).
 *
 * The kit kind travels through the core payload's data pointer so asset /
 * scene_object semantics survive the round trip; strings are copied by
 * vgui_begin_drag, so providers may return temporary kit payloads.
 */
#include <functional>

#include "editor_types.h"
#include "vgui.h"

namespace vgui_editor_kit {

	/* Converts a core payload into the kit's value type (strings copied). */
	drag_payload to_editor_payload(const vgui_drag_payload& payload);

	/* Starts a core drag from an editor payload.  Returns false when another
	 * drag is already in flight. */
	bool begin_editor_drag(vgui_context* ctx, vgui_widget* source,
		const drag_payload& payload);

	/* Attaches a kit drop_target to a widget.  The adapter stores the widget
	 * pointer and synthesizes an insertion-line hover hint; the kit target must
	 * outlive the attachment.  Pass nullptr to detach. */
	void set_editor_drop_target(vgui_widget* widget, drop_target* target);

	/* Row drag source: provider fills the kit payload for a pressed row; a
	 * false return refuses the drag.  kind selects list or tree. */
	using row_drag_provider = std::function<bool(vgui_item_id item, int index,
		drag_payload& out)>;
	void set_editor_row_drag(vgui_widget* widget, bool tree, row_drag_provider provider);

} /* namespace vgui_editor_kit */
