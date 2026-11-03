#pragma once
/*
 * vgui SDL3 backend: private hook for the native file dialog provider.
 *
 * The implementation lives in vgui_sdl3_dialog.cpp.  That translation unit has
 * to be pulled into the link even though nothing calls it directly (dialogs are
 * invoked through the core provider table), so app_mgr's constructor calls this
 * installer once.  Public headers never reference it.
 *
 * Created: 2026-11-03
 */

// Idempotent: installs the SDL3 dialog provider into the core bridge.
void vgui_sdl3_install_file_dialog_provider();
