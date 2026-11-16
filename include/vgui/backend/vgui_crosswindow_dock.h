#pragma once
/*
 * vgui_crosswindow_dock.h - cross-window dock drop target resolution (goal C
 * of docs/dock_multiwindow_assessment.md), C1 slice.
 *
 * C1 responsibility: given a global (screen-points) cursor position, decide
 * which registered window's Dock is under it, and which zone / tab index the
 * panel would land in.  It is a PURE QUERY: no widget is moved and no core
 * drag state is touched.  Moving the panel (C2) consumes the drop_result
 * produced here.
 *
 * Design constraints (see .zcode/plans/plan-dock-c-crosswindow-docking.md):
 *  - core is unchanged.  Hit-testing reuses existing public APIs
 *    vgui_dock_get_zone_rect() / vgui_dock_get_tab_rect(), which report
 *    window-local RENDER PIXELS.
 *  - multiwindow_slot has no dock field, so this component owns the
 *    window -> dock registration (a host calls register_target once per
 *    Dock-bearing window).
 *  - all screen<->render conversion goes through vgui_dock_coords.h so the
 *    hit-test agrees with the B tear-off path.
 *
 * Integration: call resolve_drop() from the floater's tick()/liberation
 * path with SDL_GetGlobalMouseState() (or the floater's content-rect centre).
 * When the result is valid and belongs to a DIFFERENT dock than the panel's
 * source, C2 migrates into result.dock at result.zone / result.index.
 */
#include <algorithm>
#include <string>
#include <vector>

#include <SDL3/SDL.h>
#include <vgui/backend/vgui_dock_coords.h>
#include <vgui/backend/vgui_sdl3.h>
#include <vgui/vgui.h>

struct crosswindow_drop_result
{
	os_window* window = nullptr;	/* target OS window (screen-point hit) */
	vgui_widget* dock = nullptr;	/* target Dock widget */
	vgui_context* ui = nullptr;		/* target Dock's context (for page rebuild) */
	std::string window_id;			/* registration id, for layout v10 owner */
	vgui_dock_zone zone = VGUI_DOCK_CENTER;
	int index = -1;					/* tab insertion index, -1 = append */

	bool valid() const { return window != nullptr && dock != nullptr; }
	void reset()
	{
		window = nullptr;
		dock = nullptr;
		ui = nullptr;
		window_id.clear();
		zone = VGUI_DOCK_CENTER;
		index = -1;
	}
};

struct crosswindow_target
{
	os_window* window = nullptr;
	vgui_widget* dock = nullptr;
	vgui_context* ui = nullptr;
	std::string id;
};

class crosswindow_dock_host
{
public:
	explicit crosswindow_dock_host(app_mgr* mgr) : mgr_(mgr) {}

	crosswindow_dock_host(const crosswindow_dock_host&) = delete;
	crosswindow_dock_host& operator=(const crosswindow_dock_host&) = delete;

	// Registers a Dock-bearing window as a drop target.  A window may be
	// registered once; re-registering refreshes its dock/ui/id.  Null window
	// or null dock are rejected.  `ui` is the Dock's context and is required
	// by C2 to rebuild a page in the target context; when omitted it is taken
	// from window->viewport->ui.  Returns true when the registration is usable.
	bool register_target(os_window* window, vgui_widget* dock, const char* id,
		vgui_context* ui = nullptr)
	{
		if (!window || !dock) return false;
		if (!ui && window->viewport) ui = window->viewport->ui;
		for (crosswindow_target& t : targets_)
		{
			if (t.window != window) continue;
			t.dock = dock;
			t.ui = ui;
			t.id = id ? id : "";
			return true;
		}
		crosswindow_target t;
		t.window = window;
		t.dock = dock;
		t.ui = ui;
		t.id = id ? id : "";
		targets_.push_back(t);
		return true;
	}

	void unregister_target(os_window* window)
	{
		for (auto it = targets_.begin(); it != targets_.end(); ++it)
		{
			if (it->window != window) continue;
			targets_.erase(it);
			return;
		}
	}

	void clear() { targets_.clear(); }

	// Drops registrations whose OS window was destroyed / is closing.
	void prune()
	{
		for (auto it = targets_.begin(); it != targets_.end();)
		{
			if (vgui_dock_coords::window_alive(mgr_, it->window)) ++it;
			else it = targets_.erase(it);
		}
	}

	size_t target_count() const { return targets_.size(); }

	const crosswindow_target* find_target(os_window* window) const
	{
		for (const crosswindow_target& t : targets_)
			if (t.window == window) return &t;
		return nullptr;
	}

	vgui_widget* dock_for(os_window* window) const
	{
		const crosswindow_target* t = find_target(window);
		return t ? t->dock : nullptr;
	}

	// Core query: which registered Dock is under the given screen point, and
	// where in it would a panel land?  Zones are tested in reverse layout
	// order so the centre (which spans the remaining area) is considered last;
	// this mirrors the source Dock's own drop-target priority.  Tab rects give
	// the insertion index; when the point is in a zone's empty area the panel
	// appends (index -1).
	bool resolve_drop(float screen_x, float screen_y,
		crosswindow_drop_result& out) const
	{
		out.reset();
		for (const crosswindow_target& t : targets_)
		{
			if (!vgui_dock_coords::window_alive(mgr_, t.window)) continue;
			float wx = 0, wy = 0, ww = 0, wh = 0;
			vgui_dock_coords::window_rect(t.window, wx, wy, ww, wh);
			if (!vgui_dock_coords::contains(wx, wy, ww, wh, screen_x, screen_y))
				continue;

			float px = 0, py = 0;
			vgui_dock_coords::screen_to_render(t.window, screen_x, screen_y, px, py);

			for (int zi = (int)VGUI_DOCK_CENTER; zi >= (int)VGUI_DOCK_LEFT; --zi)
			{
				const vgui_dock_zone zone = (vgui_dock_zone)zi;
				float zx = 0, zy = 0, zw = 0, zh = 0;
				if (!vgui_dock_get_zone_rect(t.dock, zone, &zx, &zy, &zw, &zh))
					continue;
				if (!vgui_dock_coords::contains(zx, zy, zw, zh, px, py))
					continue;

				out.window = t.window;
				out.dock = t.dock;
				out.ui = t.ui;
				out.window_id = t.id;
				out.zone = zone;
				out.index = tab_index_at(t.dock, zone, px, py);
				return true;
			}
		}
		return false;
	}

	// Screen point for a specific (dock, zone, optional tab) -- the inverse
	// query used by the self-test to synthesise deterministic drop points.
	bool screen_point_for(os_window* window, vgui_widget* dock,
		vgui_dock_zone zone, int tab_index, float& sx, float& sy) const
	{
		if (!window || !dock) return false;
		float zx = 0, zy = 0, zw = 0, zh = 0;
		if (!vgui_dock_get_zone_rect(dock, zone, &zx, &zy, &zw, &zh))
			return false;
		float px = zx + zw * 0.5f;
		float py = zy + std::max(6.0f, zh * 0.15f);	// aim at the tab strip
		if (tab_index >= 0)
		{
			const int count = vgui_dock_get_zone_count(dock, zone);
			if (tab_index < count)
			{
				const vgui_item_id panel = vgui_dock_get_zone_at(dock, zone, tab_index);
				float tx = 0, ty = 0, tw = 0, th = 0;
				if (vgui_dock_get_tab_rect(dock, panel, &tx, &ty, &tw, &th) && tw > 0)
				{
					px = tx + tw * 0.5f;
					py = ty + th * 0.5f;
				}
			}
		}
		vgui_dock_coords::render_to_screen(window, px, py, sx, sy);
		return true;
	}

private:
	// Returns the insertion index of the tab under (px,py) in render pixels,
	// or -1 for an empty zone area (append).
	static int tab_index_at(vgui_widget* dock, vgui_dock_zone zone,
		float px, float py)
	{
		const int count = vgui_dock_get_zone_count(dock, zone);
		for (int i = 0; i < count; ++i)
		{
			const vgui_item_id panel = vgui_dock_get_zone_at(dock, zone, i);
			float tx = 0, ty = 0, tw = 0, th = 0;
			if (!vgui_dock_get_tab_rect(dock, panel, &tx, &ty, &tw, &th))
				continue;
			if (vgui_dock_coords::contains(tx, ty, tw, th, px, py))
				return i;
		}
		return -1;
	}

	app_mgr* mgr_ = nullptr;
	std::vector<crosswindow_target> targets_;
};
