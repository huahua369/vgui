#pragma once
/*
 * vgui_dock_coords.h - shared screen/window coordinate helpers for the
 * dock multiwindow host components (goal C of
 * docs/dock_multiwindow_assessment.md).
 *
 * libvgui expresses all widget geometry -- vgui_widget_get_rect(),
 * vgui_dock_get_zone_rect(), vgui_dock_get_tab_rect() -- in window-local
 * RENDER PIXELS, while SDL window positions/sizes and global mouse state are
 * in WINDOW POINTS.  pixel_ratio() = render pixels / window points bridges
 * the two.  Every host-side component that crosses the window boundary must
 * use the SAME conversion or the hit-test and the tear-off will silently
 * disagree (assessment §9 "coordinate space" risk).
 *
 * These free functions are the single source of truth.  C1's
 * crosswindow_dock_host uses them; the B component (vgui_dock_windows.h)
 * keeps its inlined conversions for now because that path is already
 * validated -- folding it over to these helpers is tracked as part of C2,
 * where the merge path is deliberately touched anyway.
 *
 * No core types are changed; this header only depends on the SDL3 backend.
 */
#include <SDL3/SDL.h>
#include <vgui/backend/vgui_sdl3.h>

namespace vgui_dock_coords {

inline float pixel_ratio(const os_window* w)
{
	return (w && w->pixel_ratio() > 0.0f) ? w->pixel_ratio() : 1.0f;
}

inline bool window_alive(app_mgr* mgr, const os_window* w)
{
	if (!mgr || !w) return false;
	for (const auto& win : mgr->windows())
		if (win.get() == w && !w->pending_close) return true;
	return false;
}

// Screen points (SDL global) -> window-local render pixels.
inline void screen_to_render(const os_window* w, float sx, float sy,
	float& px, float& py)
{
	int wx = 0, wy = 0;
	if (w && w->window) SDL_GetWindowPosition(w->window, &wx, &wy);
	const float pr = pixel_ratio(w);
	px = (sx - (float)wx) * pr;
	py = (sy - (float)wy) * pr;
}

// Window-local render pixels -> screen points (inverse of screen_to_render).
inline void render_to_screen(const os_window* w, float px, float py,
	float& sx, float& sy)
{
	int wx = 0, wy = 0;
	if (w && w->window) SDL_GetWindowPosition(w->window, &wx, &wy);
	const float pr = pixel_ratio(w);
	sx = (float)wx + px / pr;
	sy = (float)wy + py / pr;
}

// Window outer rectangle in screen points.
inline void window_rect(const os_window* w, float& x, float& y,
	float& w_out, float& h_out)
{
	int wx = 0, wy = 0, ww = 0, wh = 0;
	if (w && w->window)
	{
		SDL_GetWindowPosition(w->window, &wx, &wy);
		SDL_GetWindowSize(w->window, &ww, &wh);
	}
	x = (float)wx;
	y = (float)wy;
	w_out = (float)ww;
	h_out = (float)wh;
}

inline bool contains(float x, float y, float w, float h,
	float px, float py)
{
	return w > 0.0f && h > 0.0f &&
		px >= x && px <= x + w && py >= y && py <= y + h;
}

} // namespace vgui_dock_coords
