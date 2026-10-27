#pragma once
/*
 * vgui_editor_kit.h - umbrella header for the optional editor component layer.
 *
 * vgui_editor_kit depends on the public vgui C API.  It must not introduce
 * dependencies on a game scene, asset database or editor document format.
 */
#include "asset_browser.h"
#include "asset_preview.h"
#include "breadcrumb.h"
#include "canvas_view.h"
#include "command_registry.h"
#include "command_stack.h"
#include "combo_box.h"
#include "console_view.h"
#include "curve_editor.h"
#include "drag_bridge.h"
#include "editor_context.h"
#include "editor_layout.h"
#include "editor_shell.h"
#include "editor_types.h"
#include "gradient_editor.h"
#include "node_graph_view.h"
#include "outliner_view.h"
#include "property_grid.h"
#include "search_box.h"
#include "spinbox.h"
#include "status_bar.h"
#include "table_view.h"
#include "thumbnail_cache.h"
#include "timeline_view.h"
#include "toolbar.h"
#include "vector_editor.h"
#include "viewport_camera.h"
#include "viewport_gizmo.h"
#include "viewport_grid.h"
#include "viewport_selection.h"
#include "viewport_view.h"
