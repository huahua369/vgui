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
#include "command_registry.h"
#include "command_stack.h"
#include "combo_box.h"
#include "curve_editor.h"
#include "drag_bridge.h"
#include "editor_context.h"
#include "editor_layout.h"
#include "editor_shell.h"
#include "editor_types.h"
#include "gradient_editor.h"
#include "outliner_view.h"
#include "property_grid.h"
#include "search_box.h"
#include "spinbox.h"
#include "status_bar.h"
#include "thumbnail_cache.h"
#include "toolbar.h"
#include "vector_editor.h"
#include "viewport_view.h"
