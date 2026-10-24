#pragma once
/*
 * vgui_editor_kit.h - umbrella header for the optional editor component layer.
 *
 * vgui_editor_kit depends on the public vgui C API.  It must not introduce
 * dependencies on a game scene, asset database or editor document format.
 */
#include "asset_browser.h"
#include "command_registry.h"
#include "command_stack.h"
#include "drag_bridge.h"
#include "editor_context.h"
#include "editor_layout.h"
#include "editor_shell.h"
#include "editor_types.h"
#include "outliner_view.h"
#include "property_grid.h"
#include "viewport_view.h"
