#pragma once
/*
 * vgui_window_dock.h - host helper that docks OS windows to each other.
 *
 * This is *window-level* docking (goal "OS 窗口可互相拼合" in
 * docs/dock_multiwindow_assessment.md): free-floating SDL windows can be
 * snapped flush to another window's edge so they sit together like a tiled
 * IDE, move as a unit when the anchor moves, and can be pulled apart again by
 * dragging.  It is deliberately independent of libvgui's per-context Dock
 * widget -- each OS window still owns its own vgui_context / FBO / rvg, and
 * this helper only moves and (optionally) resizes the SDL windows.
 *
 * Usage:
 *   window_dock_manager mgr;
 *   mgr.add(main_window);
 *   mgr.add(secondary_window);
 *   mgr.dock(secondary_window, main_window, window_dock_manager::RIGHT);
 *   // each frame, after input but before drawing:
 *   mgr.update();
 *
 * How it works:
 *   * A window is either "free" or "docked" (bound to an anchor + edge).
 *   * update() reads every managed window's rectangle, then:
 *       - repositions each docked window flush to its anchor (so it follows
 *         the anchor when the user moves/resizes it);
 *       - a docked window whose actual position has diverged from its docked
 *         target (the user grabbed its title bar and dragged away) is undocked;
 *       - a free window whose edges landed within `threshold` of another
 *         *free* window's edge is snapped and docked there.  (Docked windows
 *         are never used as anchors, which keeps the graph acyclic.)
 *   * Docking is detected purely from window rectangles, so there is no
 *     dependence on SDL_WINDOW_MOVED event routing -- the manager tolerates
 *     whatever the windowing system reports.
 *
 * The cross-axis match (a right/left docked window adopts the anchor's
 * height, a top/bottom docked window adopts its width) makes the join look
 * like a clean split.  Set set_match_cross_axis(false) to keep each window's
 * own size.
 */
#include <SDL3/SDL.h>

#include <map>
#include <set>
#include <vector>

#include <vgui/backend/vgui_sdl3.h>

class window_dock_manager
{
public:
	enum edge : int
	{
		NONE = 0,
		LEFT = 1,
		RIGHT = 2,
		TOP = 3,
		BOTTOM = 4
	};
	// Bit mask for set_allowed_edges(): (1 << LEFT) | (1 << RIGHT) | ...
	static constexpr int ALL_EDGES = (1 << LEFT) | (1 << RIGHT) |
		(1 << TOP) | (1 << BOTTOM);

	struct rect
	{
		int x = 0, y = 0, w = 0, h = 0;
	};

	// Registers / forgets a window.  A window removed while docked is undocked
	// first.  Adding a window already present is a no-op.
	void add(os_window* win)
	{
		if (!win || !win->window) return;
		for (const entry& e : wins_)
			if (e.win == win) return;
		entry e;
		e.win = win;
		SDL_GetWindowPosition(win->window, &e.last_x, &e.last_y);
		wins_.push_back(e);
	}
	void remove(os_window* win)
	{
		undock(win);
		for (auto it = wins_.begin(); it != wins_.end(); ++it)
		{
			if (it->win == win) { wins_.erase(it); return; }
		}
	}

	void set_threshold(int px) { threshold_ = px > 0 ? px : 0; }
	void set_match_cross_axis(bool on) { match_cross_axis_ = on; }
	void set_allowed_edges(int mask) { allowed_ = mask; }

	// Manual docking.  Positions (and, with cross-axis matching, sizes) the
	// window flush against the anchor's chosen edge and records the link.
	// Returns false for null/foreign anchors or when the window is not managed.
	bool dock(os_window* win, os_window* anchor, int edge)
	{
		if (!win || !anchor || !win->window || !anchor->window) return false;
		if (edge != LEFT && edge != RIGHT && edge != TOP && edge != BOTTOM)
			return false;
		bool known = false;
		for (const entry& e : wins_)
			if (e.win == win) { known = true; break; }
		if (!known) return false;
		rect a = read(anchor);
		rect f = read(win);
		rect target = docked_rect(f, a, edge);
		clamp_to_display(anchor, target);
		apply(win, target);
		moved_by_us_.insert(win);
		docked_[win] = link{ anchor, edge };
		mark_last(win, target);
		return true;
	}

	void undock(os_window* win) { docked_.erase(win); }
	bool is_docked(os_window* win) const { return docked_.count(win) != 0; }
	os_window* anchor_of(os_window* win) const
	{
		auto it = docked_.find(win);
		return it == docked_.end() ? nullptr : it->second.anchor;
	}
	int edge_of(os_window* win) const
	{
		auto it = docked_.find(win);
		return it == docked_.end() ? NONE : it->second.edge;
	}

	// Pure helper: given a free rectangle and a candidate anchor rectangle,
	// returns the best edge whose gap is within `threshold` (and whose
	// cross-axis spans overlap), or NONE.  `out` receives the snapped rectangle
	// (flush, cross-axis aligned); `gap_out` receives |gap| for that edge.
	// No SDL window is required, so this is the unit-testable core.
	static int probe_edge(const rect& free, const rect& anchor, int threshold,
		int allowed, int* gap_out, rect* out)
	{
		if (threshold < 0) threshold = 0;
		int best = NONE, best_gap = threshold + 1;
		if (out) *out = free;
		if (gap_out) *gap_out = best_gap;
		auto consider = [&](int e, int gap, int overlap)
		{
			if (!(allowed & (1 << e))) return;
			if (overlap < 24) return;            // must share enough cross span
			if (gap < 0) gap = -gap;             // tolerant of slight overlap
			if (gap <= threshold && gap < best_gap)
			{
				best_gap = gap;
				best = e;
				if (out)
				{
					out->w = free.w;
					out->h = free.h;
					switch (e)
					{
					case RIGHT:  out->x = anchor.x + anchor.w; out->y = anchor.y; break;
					case LEFT:   out->x = anchor.x - free.w;   out->y = anchor.y; break;
					case TOP:    out->x = anchor.x; out->y = anchor.y - free.h; break;
					case BOTTOM: out->x = anchor.x; out->y = anchor.y + anchor.h; break;
					}
				}
			}
		};
		const int v_overlap = overlap_len(free.y, free.h, anchor.y, anchor.h);
		const int h_overlap = overlap_len(free.x, free.w, anchor.x, anchor.w);
		consider(RIGHT, free.x - (anchor.x + anchor.w), v_overlap);
		consider(LEFT, anchor.x - (free.x + free.w), v_overlap);
		consider(TOP, anchor.y - (free.y + free.h), h_overlap);
		consider(BOTTOM, free.y - (anchor.y + anchor.h), h_overlap);
		if (gap_out) *gap_out = best_gap;
		return best;
	}

	// One frame of docking bookkeeping.  Cheap to call every frame; it only
	// touches SDL when a window actually needs to move/resize.
	void update()
	{
		if (wins_.empty()) return;
		// 1. Snapshot current rectangles (points, window coordinates).
		std::map<os_window*, rect> cur;
		for (const entry& e : wins_)
			cur[e.win] = read(e.win);

		moved_by_us_.clear();

		// 2. Reposition docked windows from their anchors.  A docked window
		//    whose real position diverged from its target (user dragged it
		//    loose) is undocked instead of being yanked back.
		for (auto it = docked_.begin(); it != docked_.end();)
		{
			os_window* win = it->first;
			const link& lk = it->second;
			auto ait = cur.find(lk.anchor);
			if (ait == cur.end())                 // anchor vanished
			{
				it = docked_.erase(it);
				continue;
			}
			rect f = cur[win];
			rect target = docked_rect(f, ait->second, lk.edge);
			clamp_to_display(lk.anchor, target);
			const bool ours = moved_by_us_.count(win) != 0;
			if (!ours && (std::abs(f.x - target.x) > threshold_ ||
				std::abs(f.y - target.y) > threshold_))
			{
				it = docked_.erase(it);           // user pulled it loose -> undock
				mark_last(win, f);
				continue;
			}
			if (f.x != target.x || f.y != target.y ||
				(match_cross_axis_ && (f.w != target.w || f.h != target.h)))
			{
				apply(win, target);
				moved_by_us_.insert(win);
			}
			cur[win] = target;
			mark_last(win, target);
			++it;
		}

		// 3. Free windows: detect a user drag and snap-dock to a nearby anchor.
		for (entry& e : wins_)
		{
			if (docked_.count(e.win)) continue;
			rect f = cur[e.win];
			if (f.x == e.last_x && f.y == e.last_y) continue;   // no move
			os_window* best_anchor = nullptr;
			int best_edge = NONE, best_gap = threshold_ + 1;
			rect best_out = f;
			for (const entry& o : wins_)
			{
				if (o.win == e.win) continue;
				if (docked_.count(o.win)) continue;   // anchors stay free
				rect a = cur[o.win];
				rect out;
				int gap = 0;
				const int edge = probe_edge(f, a, threshold_, allowed_, &gap, &out);
				if (edge != NONE && gap < best_gap)
				{
					best_gap = gap;
					best_edge = edge;
					best_anchor = o.win;
					best_out = out;
				}
			}
			if (best_edge != NONE && best_anchor)
			{
				rect target = docked_rect(f, cur[best_anchor], best_edge);
				clamp_to_display(best_anchor, target);
				apply(e.win, target);
				moved_by_us_.insert(e.win);
				docked_[e.win] = link{ best_anchor, best_edge };
				cur[e.win] = target;
				mark_last(e.win, target);
			}
			else
			{
				e.last_x = f.x;
				e.last_y = f.y;
			}
		}
	}

private:
	struct entry
	{
		os_window* win = nullptr;
		int last_x = 0, last_y = 0;
	};
	struct link
	{
		os_window* anchor = nullptr;
		int edge = NONE;
	};

	static int overlap_len(int a, int alen, int b, int blen)
	{
		const int lo = a > b ? a : b;
		const int hi = (a + alen) < (b + blen) ? (a + alen) : (b + blen);
		const int o = hi - lo;
		return o > 0 ? o : 0;
	}

	static rect read(os_window* win)
	{
		rect r;
		SDL_GetWindowPosition(win->window, &r.x, &r.y);
		SDL_GetWindowSize(win->window, &r.w, &r.h);
		return r;
	}

	static void apply(os_window* win, const rect& r)
	{
		SDL_SetWindowPosition(win->window, r.x, r.y);
		if (r.w > 0 && r.h > 0)
			SDL_SetWindowSize(win->window, r.w, r.h);
	}

	rect docked_rect(const rect& free, const rect& anchor, int edge) const
	{
		rect t = free;
		switch (edge)
		{
		case RIGHT:
			t.x = anchor.x + anchor.w;
			t.y = anchor.y;
			if (match_cross_axis_) t.h = anchor.h;
			break;
		case LEFT:
			t.x = anchor.x - free.w;
			t.y = anchor.y;
			if (match_cross_axis_) t.h = anchor.h;
			break;
		case TOP:
			t.x = anchor.x;
			t.y = anchor.y - free.h;
			if (match_cross_axis_) t.w = anchor.w;
			break;
		case BOTTOM:
			t.x = anchor.x;
			t.y = anchor.y + anchor.h;
			if (match_cross_axis_) t.w = anchor.w;
			break;
		default: break;
		}
		return t;
	}

	void clamp_to_display(os_window* ref, rect& r) const
	{
		SDL_Rect usable = { 0, 0, 0, 0 };
		if (!SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(ref->window),
			&usable) || usable.w <= 0 || usable.h <= 0)
			return;
		if (r.w <= 0) SDL_GetWindowSize(ref->window, &r.w, &r.h);
		if (r.h <= 0) SDL_GetWindowSize(ref->window, &r.h, &r.w);
		if (r.x < usable.x) r.x = usable.x;
		if (r.y < usable.y) r.y = usable.y;
		if (r.x + r.w > usable.x + usable.w)
			r.x = usable.x + usable.w - r.w;
		if (r.y + r.h > usable.y + usable.h)
			r.y = usable.y + usable.h - r.h;
		if (r.x < usable.x) r.x = usable.x;
		if (r.y < usable.y) r.y = usable.y;
	}

	void mark_last(os_window* win, const rect& r)
	{
		for (entry& e : wins_)
			if (e.win == win) { e.last_x = r.x; e.last_y = r.y; return; }
	}

	int threshold_ = 28;
	bool match_cross_axis_ = true;
	int allowed_ = ALL_EDGES;
	std::vector<entry> wins_;
	std::map<os_window*, link> docked_;
	std::set<os_window*> moved_by_us_;
};
