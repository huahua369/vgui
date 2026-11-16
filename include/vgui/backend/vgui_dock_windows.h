#pragma once
/*
 * vgui_dock_windows.h - dock panel tear-off host (goal B, step 1 of
 * docs/dock_multiwindow_assessment.md): turn a docked panel into a real
 * borderless OS window that follows the cursor, and merge it back into the
 * main dock when it returns.
 *
 * This is a host-side component, NOT a core capability: libvgui owns no
 * windowing code (vgui.h "libvgui owns no windowing code"), so the SDL3
 * backend layer provides the window machinery and the core keeps its
 * tear-off contract (vgui_dock_on_tear_off / on_drag_out / dragging /
 * end_drag).  Widgets cannot move between vgui_contexts, so a torn-off
 * panel's real page STAYS in the main context (vgui_dock_detach returns it)
 * and the floating window shows a page built by the host hook (default: a
 * placeholder); on merge the orphan page is re-added to the main dock.
 *
 * This generalizes the proven prototype in sample/vgui_game_editor.cpp
 * (tear_off_to_floating / merge_floating_back, 2026-11).  It deliberately
 * bypasses all five "core hard blockers" of the assessment: no cross-context
 * page moves (detach + rebuild), ids stay per-dock (the host maps panel id ->
 * floater), floats are real OS windows (no vgui clamp applies), and drag
 * state stays inside the source context (the host polls global mouse state).
 *
 * Integration with the frame loop (one vgpu_render_frame() per frame for ALL
 * windows -- see vgui_multiwindow.h for why):
 *
 *     wg->get_event();
 *     floater.tick();          // create/merge/close floaters, BEFORE begin_frame
 *     host.begin_frame(delta); // host is the multiwindow_host; floaters are
 *     host.draw_all();         // attached to the same host, so one submit
 *     host.end_frame();        // covers main window + all floaters
 *
 * The component registers its own vgui_dock_on_tear_off / on_drag_out
 * handlers on the main dock; a host that needs its own handlers on the same
 * dock should chain them (only one handler per dock is possible).
 *
 * Merge rules (identical to the prototype):
 *   - while live-dragging: the floater follows the global cursor; it merges
 *     as soon as its content rect would satisfy the merge test;
 *   - after a system-modal title-bar drag (Windows swallows the frames), the
 *     merge is detected from "window position changed by > 6px while the
 *     button was up" (settle frames skip creation jitter);
 *   - a WINDOW_CLOSE_REQUESTED on a floater merges it back instead of
 *     destroying the panel;
 *   - merge test: content rect (window minus title/tab chrome) fully inside
 *     the main window, or its center inside (panels larger than the main
 *     window can still come back).
 */
#include <cmath>
#include <cstddef>
#include <deque>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <SDL3/SDL.h>
#include <vgui/backend/vgui_multiwindow.h>
#include <vgui/backend/vgui_sdl3.h>

struct dock_floater_opts
{
	int min_page_w = 160;		/* smaller sampled pages fall back to the defaults */
	int min_page_h = 120;
	int fallback_page_w = 520;	/* used when the panel has no layout yet */
	int fallback_page_h = 360;
	float merge_tolerance = 2.0f;	/* px slack for the containment test */
	float title_bar_px = 30.0f;	/* vgui layout px (see build_title_bar) */
	int hit_test_px = 30;		/* SDL points reserved for the drag hit-test */
	int settle_frames = 8;		/* creation-jitter guard for the move detector */
	const char* window_title_prefix = "vgui floater · ";
	const char* window_title_suffix = "";	/* appended after the panel title */
};

struct dock_floater_info
{
	const char* id = nullptr;		/* panel string id, "" when the panel had none */
	const char* title = nullptr;
	vgui_dock_zone origin_zone = VGUI_DOCK_CENTER;
	/* Filled once the floating window exists (build_page and later hooks);
	 * panel is the main-dock item id the floater was created from. */
	os_window* window = nullptr;
	vgui_item_id panel = 0;
};

/* Snapshot of one live floater, for layout persistence (editor layout v9).
 * id is copied out of the host (the panel's stable string id, "" when it had
 * none).  x/y/width/height are the floating OS window's screen rectangle in
 * points, including the title/tab chrome -- exactly the shape accepted by
 * request_tear_off(panel, x, y, width, height). */
struct dock_floater_rect
{
	vgui_item_id panel = 0;
	std::string id;
	int x = 0;
	int y = 0;
	int width = 0;
	int height = 0;
};

/* Builds the page shown inside the floating window's dock.  Return nullptr
 * to use the built-in placeholder.  The default hook is enough for simple
 * panels; content-heavy panels (3D viewport, document editors) rebuild their
 * real page here and release/rebuild native textures around the transition
 * (see vgui_game_editor.cpp GAME VIEW for the full pattern). */
typedef vgui_widget* (*dock_floater_page_fn)(vgui_context* floater_ui,
	vgui_widget* floater_dock, const dock_floater_info& info, void* user);

/* Lifecycle hooks (all optional) around the panel move: */
/* Called BEFORE the panel is detached (or removed) from the main dock --
 * release per-panel native resources that were registered against the main
 * context here. */
typedef void (*dock_floater_teardown_fn)(const dock_floater_info& info,
	void* user);
/* Return true to REMOVE the panel (vgui_dock_remove destroys its page)
 * instead of detaching it -- for panels whose page cannot survive the
 * context switch (e.g. a page bound to a native texture that was just
 * released).  The restored page then comes from restore_page. */
typedef bool (*dock_floater_remove_fn)(const dock_floater_info& info,
	void* user);
/* Called on merge to obtain the page for the restored panel.  Return
 * nullptr to fall back to the orphan page (or to an empty panel when the
 * panel was in remove mode). */
typedef vgui_widget* (*dock_floater_restore_fn)(vgui_widget* main_dock,
	const dock_floater_info& info, vgui_widget* orphan_page, void* user);
/* Called just before the floating window's context is destroyed -- release
 * per-panel resources registered against THAT context here. */
typedef void (*dock_floater_closing_fn)(const dock_floater_info& info,
	void* user);
/* Fired after the panel has been merged back into the main dock.  panel is
 * the new main-dock item id (0 on failure) and page the restored widget. */
typedef void (*dock_floater_merged_fn)(const dock_floater_info& info,
	vgui_item_id panel, vgui_widget* page, void* user);

class dock_floater_host
{
public:
	/* Optional host hooks.  build_page: build the page shown inside the
	 * floater (return nullptr for the built-in placeholder).  merged: fired
	 * after the panel is back in the main dock.  teardown_page/remove_page/
	 * restore_page/floater_closing cover the per-panel resource lifecycle
	 * around the context switch -- see the typedef comments. */
	struct hooks
	{
		dock_floater_page_fn build_page = nullptr;
		void* build_user = nullptr;
		dock_floater_teardown_fn teardown_page = nullptr;
		dock_floater_remove_fn remove_page = nullptr;
		dock_floater_restore_fn restore_page = nullptr;
		dock_floater_closing_fn floater_closing = nullptr;
		void* lifecycle_user = nullptr;
		dock_floater_merged_fn merged = nullptr;
		void* merged_user = nullptr;
	};

	dock_floater_host() = default;
	~dock_floater_host()
	{
		/* Floaters still alive here mean the host tore the app down without
		 * merging; destroy their windows/contexts the same way merge does to
		 * avoid leaking GPU validation errors at device destroy. */
		for (auto& [panel, fw] : floaters_)
			close_floater_window(fw);
	}
	dock_floater_host(const dock_floater_host&) = delete;
	dock_floater_host& operator=(const dock_floater_host&) = delete;

	bool attach(app_mgr* mgr, multiwindow_host* host, vgui_widget* main_dock,
		os_window* main_window, font_familys_t* familys,
		float font_size = 16.0f, const dock_floater_opts& opts = {},
		const hooks& h = {})
	{
		if (!mgr || !host || !main_dock || !main_window || !familys)
			return false;
		mgr_ = mgr;
		host_ = host;
		main_dock_ = main_dock;
		main_window_ = main_window;
		familys_ = familys;
		font_size_ = font_size;
		opts_ = opts;
		hooks_ = h;
		vgui_dock_on_tear_off(main_dock_, &dock_floater_host::on_tear_off_thunk, this);
		vgui_dock_on_drag_out(main_dock_, &dock_floater_host::on_drag_out_thunk, this);
		return true;
	}

	/* Frame-end work: process queued tear-offs, follow live drags, merge
	 * floaters back.  Call AFTER input processing and BEFORE the host's
	 * begin_frame() so created windows get updated this frame. */
	void tick()
	{
		if (!mgr_ || !main_dock_)
			return;
		if (!pending_tear_offs_.empty())
		{
			const tear_off_request req = pending_tear_offs_.front();
			pending_tear_offs_.pop_front();
			tear_off_to_floating(req.panel, req.has_rect,
				req.x, req.y, req.width, req.height);
		}
		poll_dock_drag();
		update_live_drag();
		update_floaters();
	}

	size_t floater_count() const { return floaters_.size(); }
	bool floating(vgui_item_id panel) const
	{
		return floaters_.find(panel) != floaters_.end();
	}
	/* Destroys every floating window WITHOUT merging the panels back, firing
	 * floater_closing for each.  Call this before the main dock/context, the
	 * multiwindow_host and the window manager are torn down (the destructor
	 * is then an empty fallback): a static host outliving app_mgr would
	 * otherwise touch destroyed windows.  Pending tear-off requests are
	 * dropped. */
	void close_all()
	{
		pending_tear_offs_.clear();
		for (auto& [panel, fw] : floaters_)
			close_floater_window(fw);
		floaters_.clear();
	}
	vgui_item_id pending_tear_off() const
	{
		return pending_tear_offs_.empty() ? 0 : pending_tear_offs_.front().panel;
	}
	/* Queues a tear-off through the same path the callbacks use (probes and
	 * tests): one request is processed per tick(). */
	void request_tear_off(vgui_item_id panel)
	{
		tear_off_request req;
		req.panel = panel;
		pending_tear_offs_.push_back(req);
	}
	/* Queues a tear-off at an explicit screen rectangle: x/y is the outer
	 * window top-left and width/height its outer size, in points -- the
	 * shape capture_floaters() returns.  Use it to restore layout v9
	 * floater positions after mapping the saved panel id with
	 * vgui_dock_find().  One request is processed per tick(); dropped if the
	 * panel is not in the main dock. */
	void request_tear_off(vgui_item_id panel, int x, int y, int width,
		int height)
	{
		tear_off_request req;
		req.panel = panel;
		req.has_rect = true;
		req.x = x;
		req.y = y;
		req.width = width;
		req.height = height;
		pending_tear_offs_.push_back(req);
	}
	/* Live floaters for layout persistence, in panel-id (std::map) order.
	 * The rectangles are sampled from SDL on every call, so a host can save
	 * them at any point in the frame; floaters whose window already died
	 * outside our control are skipped. */
	std::vector<dock_floater_rect> capture_floaters() const
	{
		std::vector<dock_floater_rect> out;
		out.reserve(floaters_.size());
		for (const auto& [panel, fw] : floaters_)
		{
			if (!fw.window || !window_alive(fw.window))
				continue;
			dock_floater_rect rect;
			rect.panel = panel;
			rect.id = fw.id;
			SDL_GetWindowPosition(fw.window->window, &rect.x, &rect.y);
			SDL_GetWindowSize(fw.window->window, &rect.width, &rect.height);
			out.push_back(std::move(rect));
		}
		return out;
	}
	/* Test/demo accessors for the floater state. */
	os_window* floater_window(vgui_item_id panel) const
	{
		auto it = floaters_.find(panel);
		return it != floaters_.end() ? it->second.window : nullptr;
	}
	vgui_context* floater_ui(vgui_item_id panel) const
	{
		auto it = floaters_.find(panel);
		return it != floaters_.end() ? it->second.ui : nullptr;
	}

private:
	struct tear_off_request
	{
		vgui_item_id panel = 0;
		bool has_rect = false;
		int x = 0;
		int y = 0;
		int width = 0;
		int height = 0;
	};

	struct floater
	{
		os_window* window = nullptr;
		vgui_context* ui = nullptr;
		vgui_widget* dock = nullptr;		/* the dock inside the floater */
		vgui_item_id source_panel = 0;		/* main-dock panel id (map key) */
		std::string id;				/* copied BEFORE detach (c_str dangles) */
		std::string title;
		vgui_dock_zone origin_zone = VGUI_DOCK_CENTER;
		vgui_widget* orphan_page = nullptr;	/* real page kept in the main ctx */
		bool removed_page = false;		/* remove mode: no orphan page exists */
		int chrome_h = 0;			/* title bar + tab bar, window points */
		int last_x = 0, last_y = 0;
		int settle = 0;
		int page_w = 0, page_h = 0;		/* wanted page size, window points */
		bool hit_test_set = false;
		bool from_live_drag = false;
	};

	// -- callbacks (core contract: queue only, act after the frame) --------

	static void on_tear_off_thunk(vgui_widget* dock, vgui_item_id panel,
		float lx, float ly, void* user)
	{
		auto* self = static_cast<dock_floater_host*>(user);
		(void)dock;
		if (!self || !self->main_window_)
			return;
		int ax = 0, ay = 0;
		SDL_GetWindowPosition(self->main_window_->window, &ax, &ay);
		/* lx/ly are dock-local render px; window position is screen points. */
		const float pr = self->main_window_->pixel_ratio() > 0.0f
			? self->main_window_->pixel_ratio() : 1.0f;
		self->tear_off_sx_ = (float)ax + lx / pr;
		self->tear_off_sy_ = (float)ay + ly / pr;
		tear_off_request req;
		req.panel = panel;
		self->pending_tear_offs_.push_back(req);
	}

	static void on_drag_out_thunk(vgui_widget* dock, vgui_item_id panel,
		float lx, float ly, void* user)
	{
		auto* self = static_cast<dock_floater_host*>(user);
		(void)dock;
		(void)lx;
		(void)ly;
		if (!self)
			return;
		float mx = 0, my = 0;
		SDL_GetGlobalMouseState(&mx, &my);
		self->tear_off_sx_ = mx;
		self->tear_off_sy_ = my;
		tear_off_request req;
		req.panel = panel;
		self->pending_tear_offs_.push_back(req);
	}

	// -- helpers -------------------------------------------------------------

	bool main_window_alive() const
	{
		if (!mgr_ || !main_window_)
			return false;
		for (const auto& w : mgr_->windows())
			if (w.get() == main_window_ && !w->pending_close)
				return true;
		return false;
	}

	bool window_alive(os_window* window) const
	{
		if (!mgr_ || !window)
			return false;
		for (const auto& w : mgr_->windows())
			if (w.get() == window && !w->pending_close)
				return true;
		return false;
	}

	bool page_rect_screen(vgui_widget* dock, vgui_item_id panel,
		float* x, float* y, float* w, float* h) const
	{
		if (!dock || !main_window_)
			return false;
		vgui_widget* page = vgui_dock_get_panel(dock, panel);
		if (!page)
			return false;
		float px = 0, py = 0, pw = 0, ph = 0;
		vgui_widget_get_rect(page, &px, &py, &pw, &ph);
		if (pw <= 1.0f || ph <= 1.0f)
			return false;
		int wx = 0, wy = 0;
		SDL_GetWindowPosition(main_window_->window, &wx, &wy);
		const float pr = main_window_->pixel_ratio() > 0.0f
			? main_window_->pixel_ratio() : 1.0f;
		*x = (float)wx + px / pr;
		*y = (float)wy + py / pr;
		*w = pw / pr;
		*h = ph / pr;
		return *w > 1.0f && *h > 1.0f;
	}

	bool rect_fully_inside_main(float x, float y, float w, float h) const
	{
		if (!main_window_alive())
			return false;
		int mx = 0, my = 0, mw = 0, mh = 0;
		SDL_GetWindowPosition(main_window_->window, &mx, &my);
		SDL_GetWindowSize(main_window_->window, &mw, &mh);
		return x >= (float)mx && y >= (float)my &&
			x + w <= (float)(mx + mw) && y + h <= (float)(my + mh);
	}

	/* Merge test on the content rect (window minus chrome).  Rule 1: fully
	 * inside (an axis bigger than the main window degenerates to "covers it").
	 * Rule 2: content center inside -- a panel larger than the main window
	 * can never satisfy rule 1.  Neither fires right after a tear-off: the
	 * cursor is outside by definition. */
	bool should_merge(const floater& fw) const
	{
		if (!fw.window || !main_window_alive())
			return false;
		int fx = 0, fy = 0, ww = 0, wh = 0;
		SDL_GetWindowPosition(fw.window->window, &fx, &fy);
		SDL_GetWindowSize(fw.window->window, &ww, &wh);
		int mx = 0, my = 0, mw = 0, mh = 0;
		SDL_GetWindowPosition(main_window_->window, &mx, &my);
		SDL_GetWindowSize(main_window_->window, &mw, &mh);

		const float t = opts_.merge_tolerance;
		const float cx = (float)fx;
		const float cy = (float)fy + (float)fw.chrome_h;
		const float cw = (float)ww;
		const float ch = (float)(wh - fw.chrome_h);
		if (cw <= 0.0f || ch <= 0.0f)
			return false;

		const bool x_ok = cw >= (float)mw - t
			? cx <= (float)mx + t
			: (cx >= (float)mx - t && cx + cw <= (float)(mx + mw) + t);
		const bool y_ok = ch >= (float)mh - t
			? cy <= (float)my + t
			: (cy >= (float)my - t && cy + ch <= (float)(my + mh) + t);
		if (x_ok && y_ok)
			return true;

		const float ccx = cx + cw * 0.5f;
		const float ccy = cy + ch * 0.5f;
		return ccx >= (float)mx && ccx <= (float)(mx + mw) &&
			ccy >= (float)my && ccy <= (float)(my + mh);
	}

	static vgui_widget* build_title_bar(vgui_context* ui, vgui_widget* root,
		const char* title)
	{
		(void)ui;
		vgui_widget* bar = vgui_widget_add(root, VGUI_PANEL, nullptr);
		vgui_widget_set_layout(bar, VGUI_LAYOUT_ROW);
		vgui_widget_set_padding(bar, 12, 0, 12, 0);
		vgui_widget_set_size(bar, 0, 30);
		vgui_widget_set_align_self(bar, VGUI_ALIGN_STRETCH);
		vgui_widget_set_background(bar, 0xFF1B2230);
		vgui_widget* label = vgui_widget_add(bar, VGUI_LABEL,
			title && title[0] ? title : "PANEL");
		vgui_widget_set_size(label, 0, 20);
		vgui_widget_set_align_self(label, VGUI_ALIGN_STRETCH);
		vgui_widget_set_text_color(label, 0xFFE6EDF5);
		return bar;
	}

	static vgui_widget* build_placeholder_page(vgui_context* ui,
		vgui_widget* dock, const char* title)
	{
		(void)ui;
		vgui_widget* page = vgui_widget_add(dock, VGUI_PANEL, nullptr);
		vgui_widget_set_layout(page, VGUI_LAYOUT_COLUMN);
		vgui_widget_set_padding(page, 16, 16, 16, 16);
		vgui_widget_set_gap(page, 10);
		vgui_widget_set_background(page, 0xFF10141A);
		vgui_widget* head = vgui_widget_add(page, VGUI_LABEL,
			title && title[0] ? title : "PANEL");
		vgui_widget_set_size(head, 0, 24);
		vgui_widget_set_align_self(head, VGUI_ALIGN_STRETCH);
		vgui_widget_set_text_color(head, 0xFFD6DEE8);
		vgui_widget* hint = vgui_widget_add(page, VGUI_LABEL,
			"This panel is torn off into its own window. Drag the window back "
			"onto the main window to merge it.");
		vgui_widget_set_size(hint, 0, 20);
		vgui_widget_set_align_self(hint, VGUI_ALIGN_STRETCH);
		vgui_widget_set_text_color(hint, 0xFF8A98A8);
		return page;
	}

	// -- tear-off ------------------------------------------------------------

	void tear_off_to_floating(vgui_item_id panel, bool has_rect,
		int rect_x, int rect_y, int rect_w, int rect_h)
	{
		if (!main_dock_ || !main_window_alive() || !host_ || !familys_)
			return;
		/* Idempotency: live drag-out + mouse-up tear-off can queue the same
		 * panel twice; a panel already floating (or gone from the main dock)
		 * is ignored to avoid double windows / lost pages. */
		if (floaters_.find(panel) != floaters_.end())
			return;
		if (vgui_dock_get_index(main_dock_, panel) < 0)
			return;

		/* Title / string id must be copied BEFORE detach:
		 * vgui_dock_get_title() hands out a c_str() into the dock's panel
		 * vector and detach() erases that element. */
		dock_floater_info info;
		{
			const char* title_raw = vgui_dock_get_title(main_dock_, panel);
			std::string title_str = title_raw ? title_raw : "PANEL";
			const char* id_raw = vgui_dock_get_id(main_dock_, panel);
			std::string id_str = id_raw ? id_raw : "";
			info.title = title_str.c_str();
			info.id = id_str.c_str();
			info.origin_zone = vgui_dock_get_zone(main_dock_, panel);

			/* Sample the docked page rect before detach: after detach the
			 * page is out of the dock and has no display rect. */
			float src_x = 0, src_y = 0, src_w = 0, src_h = 0;
			const bool have_src = page_rect_screen(main_dock_, panel,
				&src_x, &src_y, &src_w, &src_h);

			vgui_dock_zone zone = info.origin_zone;
			if (zone == VGUI_DOCK_FLOATING)
				zone = VGUI_DOCK_CENTER;
			/* Release per-panel resources first, then either remove the panel
			 * (vgui_dock_remove destroys its page -- for pages that cannot
			 * survive the context switch; the floater rebuilds via the
			 * build_page hook) or detach it (the real page stays in the main
			 * context and comes back on merge). */
			if (hooks_.teardown_page)
				hooks_.teardown_page(info, hooks_.lifecycle_user);
			const bool remove = hooks_.remove_page &&
				hooks_.remove_page(info, hooks_.lifecycle_user);
			vgui_widget* orphan_page = remove
				? (vgui_dock_remove(main_dock_, panel), nullptr)
				: vgui_dock_detach(main_dock_, panel);

			int width = (int)std::lround(src_w);
			int height = (int)std::lround(src_h);
			if (!have_src || width < opts_.min_page_w ||
				height < opts_.min_page_h)
			{
				width = opts_.fallback_page_w;
				height = opts_.fallback_page_h;
			}

			/* Chrome (title bar + dock tab bar) height depends on DPI /
			 * ui_scale and cannot be computed statically: measure once after
			 * the first layout, then correct the window so the PAGE size
			 * equals the docked size.  The initial hint is close enough for
			 * the first frame. */
			const float ratio_hint = main_window_->pixel_ratio() > 0.0f
				? main_window_->pixel_ratio() : 1.0f;
			const float chrome_hint =
				(opts_.title_bar_px + 30.0f) / ratio_hint;

			/* Layout restore: the saved rectangle describes the whole OS
			 * window (page + chrome), so the page gets the inner size.  The
			 * caller-provided top-left replaces the sampled page position.
			 * The post-layout chrome measurement below then converges the
			 * window onto the saved outer rectangle. */
			if (has_rect)
			{
				width = rect_w > opts_.min_page_w
					? rect_w : opts_.min_page_w;
				const int inner_h =
					rect_h - (int)std::lround(chrome_hint);
				height = inner_h > opts_.min_page_h
					? inner_h : opts_.min_page_h;
			}

			/* The window shows up at the page's screen position; live drag
			 * then adds the cursor delta so the panel looks "pulled out". */
			float page_x = has_rect ? (float)rect_x
				: (have_src ? src_x : tear_off_sx_);
			float page_y = has_rect ? (float)rect_y + chrome_hint
				: (have_src ? src_y : tear_off_sy_);
			float mx = 0, my = 0;
			const bool mouse_down =
				(SDL_GetGlobalMouseState(&mx, &my) & SDL_BUTTON_LMASK) != 0;
			if (mouse_down && !has_rect && have_src &&
				drag_panel_seen_ == panel)
			{
				page_x += mx - drag_start_mx_;
				page_y += my - drag_start_my_;
			}
			const float grab_x = mx - page_x;
			const float grab_y = my - page_y;

			/* Initial position accounts for the chrome (page below title bar
			 * + tab bar); measured and corrected once after first layout. */
			int px = (int)std::lround(page_x);
			int py = (int)std::lround(page_y - chrome_hint);

			std::string win_title = opts_.window_title_prefix
				? opts_.window_title_prefix : "";
			win_title += title_str;
			win_title += opts_.window_title_suffix ? opts_.window_title_suffix : "";
			os_window* win = mgr_->create2(win_title.c_str(), px, py,
				width, height + (int)std::lround(chrome_hint),
				ef_borderless | ef_resizable, nullptr);
			if (!win)
			{
				roll_back(info, orphan_page, remove, zone, title_str);
				return;
			}
			info.window = win;

			vgui_context* fui = vgui_create(familys_, font_size_);
			{
				int win_px_w = 0, win_px_h = 0;
				SDL_GetWindowSizeInPixels(win->window, &win_px_w, &win_px_h);
				vgui_context_set_size(fui, win_px_w, win_px_h);
			}
			/* Root must be COLUMN + cross-axis stretch or the dock will not
			 * follow the window size. */
			{
				vgui_widget* root = vgui_root(fui);
				vgui_widget_set_layout(root, VGUI_LAYOUT_COLUMN);
				vgui_widget_set_padding(root, 0, 0, 0, 0);
				vgui_widget_set_gap(root, 0);
				vgui_widget_set_align_items(root, VGUI_ALIGN_STRETCH);
			}
			build_title_bar(fui, vgui_root(fui), title_str.c_str());
			vgui_widget* dock = vgui_widget_add(vgui_root(fui), VGUI_DOCK,
				nullptr);
			vgui_widget_set_grow(dock, 1.0f);
			vgui_widget_set_align_self(dock, VGUI_ALIGN_STRETCH);
			vgui_dock_set_zone_size(dock, VGUI_DOCK_BOTTOM, 104.0f);

			dock_floater_info build_info = info;
			vgui_widget* page = hooks_.build_page
				? hooks_.build_page(fui, dock, build_info, hooks_.build_user)
				: nullptr;
			if (!page)
				page = build_placeholder_page(fui, dock, title_str.c_str());
			vgui_dock_panel_desc desc;
			vgui_dock_panel_desc_default(&desc);
			desc.title = title_str.c_str();
			desc.panel = page;
			desc.zone = VGUI_DOCK_CENTER;
			vgui_dock_add(dock, &desc);

			if (!host_->attach(win, fui))
			{
				/* Clean up in the merge order: destroy context, clear the
				 * viewport pointer (avoids double destroy), destroy window. */
				vgui_destroy(fui);
				if (win->viewport)
					win->viewport->ui = nullptr;
				mgr_->destroy(win->id);
				roll_back(info, orphan_page, remove, zone, title_str);
				return;
			}
			win->sync_ui_scale(true);

			/* Chrome (title bar + dock tab bar) height depends on DPI /
			 * ui_scale and cannot be computed statically: measure once after
			 * the first layout, then correct the window so the PAGE size
			 * equals the docked size. */
			int chrome_pt_h = 0;
			{
				const float fr = win->pixel_ratio() > 0.0f
					? win->pixel_ratio() : 1.0f;
				int wpx = 0, hpx = 0;
				SDL_GetWindowSizeInPixels(win->window, &wpx, &hpx);
				float pw = 0, ph = 0;
				vgui_widget_get_rect(page, nullptr, nullptr, &pw, &ph);
				int chrome_w = wpx - (int)std::lround(pw);
				int chrome_px_h = hpx - (int)std::lround(ph);
				if (chrome_w < 0)
					chrome_w = 0;
				if (chrome_px_h < 0)
					chrome_px_h = 0;
				const int want_w =
					(int)std::lround((float)width * fr) + chrome_w;
				const int want_h =
					(int)std::lround((float)height * fr) + chrome_px_h;
				width = (int)std::lround((float)want_w / fr);
				height = (int)std::lround((float)want_h / fr);
				chrome_pt_h = (int)std::lround((float)chrome_px_h / fr);
				SDL_SetWindowSize(win->window, width, height);
				vgui_context_set_size(fui, want_w, want_h);
				px = (int)std::lround(page_x);
				py = (int)std::lround(page_y - (float)chrome_px_h / fr);
				SDL_SetWindowPosition(win->window, px, py);
			}

			floater fw;
			fw.window = win;
			fw.ui = fui;
			fw.dock = dock;
			fw.source_panel = panel;
			fw.id = id_str;
			fw.title = title_str;
			fw.origin_zone = info.origin_zone;
			fw.orphan_page = orphan_page;
			fw.removed_page = remove;
			fw.chrome_h = chrome_pt_h;
			fw.last_x = px;
			fw.last_y = py;
			fw.settle = opts_.settle_frames;
			fw.page_w = width;
			fw.page_h = height;
			fw.from_live_drag = mouse_down && !has_rect;
			floaters_[panel] = fw;

			/* Live tear-off: the button is still down -- record the grab
			 * offset and cancel the core drag state, otherwise the mouse-up
			 * tear-off callback would fire again for the detached panel. */
			if (mouse_down && !has_rect)
			{
				live_drag_ = true;
				live_panel_ = panel;
				live_offx_ = grab_x;
				live_offy_ = grab_y;
				vgui_dock_end_drag(main_dock_);
			}
			drag_rect_valid_ = false;
			drag_panel_seen_ = 0;
			return;
		}
	}

	/* Failure paths: put the panel back into the main dock.  The restore hook
	 * runs here too -- in remove mode the original page is gone and the host
	 * rebuilds it. */
	void roll_back(const dock_floater_info& info, vgui_widget* orphan_page,
		bool removed, vgui_dock_zone zone, const std::string& title)
	{
		(void)removed;
		vgui_widget* page = orphan_page;
		if (hooks_.restore_page)
			page = hooks_.restore_page(main_dock_, info, orphan_page,
				hooks_.lifecycle_user);
		vgui_dock_panel_desc desc;
		vgui_dock_panel_desc_default(&desc);
		desc.id = !info.id || info.id[0] ? info.id : nullptr;
		desc.title = title.c_str();
		desc.panel = page;
		desc.zone = zone == VGUI_DOCK_FLOATING ? VGUI_DOCK_CENTER : zone;
		vgui_dock_add(main_dock_, &desc);
		vgui_dock_end_drag(main_dock_);
	}

	dock_floater_info info_of(const floater& fw) const
	{
		dock_floater_info info;
		info.id = fw.id.c_str();
		info.title = fw.title.c_str();
		info.origin_zone = fw.origin_zone;
		info.window = fw.window;
		info.panel = fw.source_panel;
		return info;
	}

	void close_floater_window(floater& fw)
	{
		if (fw.window)
		{
			/* The floater context dies below: give the host a chance to
			 * release resources registered against it (native textures). */
			if (hooks_.floater_closing)
			{
				dock_floater_info info = info_of(fw);
				hooks_.floater_closing(info, hooks_.lifecycle_user);
			}
			if (host_)
				host_->detach(fw.window);
			/* Order matters (see the prototype): destroy the context first,
			 * then clear viewport->ui (avoids a double destroy from
			 * ~gui_viewport), then destroy the OS window. */
			if (fw.ui)
			{
				vgui_destroy(fw.ui);
				if (fw.window->viewport)
					fw.window->viewport->ui = nullptr;
				fw.ui = nullptr;
			}
			mgr_->destroy(fw.window->id);
			fw.window = nullptr;
		}
	}

	void merge_back(std::map<vgui_item_id, floater>::iterator it)
	{
		if (it == floaters_.end())
			return;
		floater fw = it->second;	/* copy: erase invalidates the iterator */
		const dock_floater_info info = info_of(fw);
		close_floater_window(fw);

		/* Page acquisition: the restore hook may rebuild (remove mode) or
		 * fall back to the orphan page kept in the main context. */
		vgui_widget* page = fw.orphan_page;
		if (hooks_.restore_page)
			page = hooks_.restore_page(main_dock_, info, fw.orphan_page,
				hooks_.lifecycle_user);

		vgui_dock_panel_desc desc;
		vgui_dock_panel_desc_default(&desc);
		desc.id = !fw.id.empty() ? fw.id.c_str() : nullptr;
		desc.title = fw.title.c_str();
		desc.panel = page;	/* the real page kept in the main ctx / rebuilt */
		desc.zone = fw.origin_zone == VGUI_DOCK_FLOATING
			? VGUI_DOCK_CENTER : fw.origin_zone;
		const vgui_item_id restored = vgui_dock_add(main_dock_, &desc);

		if (hooks_.merged)
			hooks_.merged(info, restored,
				restored ? vgui_dock_get_panel(main_dock_, restored) : nullptr,
				hooks_.merged_user);
		floaters_.erase(it);
	}

	// -- per-frame updates ---------------------------------------------------

	/* While the left button is down on a dock tab drag, sample the panel's
	 * docked rect once and, as the cursor moves, queue a tear-off the moment
	 * the translated rect leaves the main window -- no need to wait for the
	 * cursor itself to leave, or for the release. */
	void poll_dock_drag()
	{
		if (live_drag_)
			return;
		float mx = 0, my = 0;
		const Uint32 bs = SDL_GetGlobalMouseState(&mx, &my);
		const vgui_item_id dragging = (bs & SDL_BUTTON_LMASK)
			? vgui_dock_dragging(main_dock_) : 0;
		if (!dragging)
		{
			drag_rect_valid_ = false;
			drag_panel_seen_ = 0;
			return;
		}
		if (drag_panel_seen_ != dragging)
		{
			drag_panel_seen_ = dragging;
			drag_start_mx_ = mx;
			drag_start_my_ = my;
			drag_rect_valid_ = page_rect_screen(main_dock_, dragging,
				&drag_rect_x_, &drag_rect_y_, &drag_rect_w_, &drag_rect_h_);
		}
		if (pending_tear_offs_.empty() && drag_rect_valid_ &&
			!rect_fully_inside_main(
				drag_rect_x_ + (mx - drag_start_mx_) + 2.0f,
				drag_rect_y_ + (my - drag_start_my_) + 2.0f,
				drag_rect_w_ - 4.0f, drag_rect_h_ - 4.0f))
		{
			tear_off_sx_ = mx;
			tear_off_sy_ = my;
			tear_off_request req;
			req.panel = dragging;
			pending_tear_offs_.push_back(req);
		}
	}

	void update_live_drag()
	{
		if (!live_drag_)
			return;
		float mx = 0, my = 0;
		const Uint32 bs = SDL_GetGlobalMouseState(&mx, &my);
		auto it = floaters_.find(live_panel_);
		if (it == floaters_.end() || !it->second.window ||
			!window_alive(it->second.window))
		{
			live_drag_ = false;
			return;
		}
		const bool down_now = (bs & SDL_BUTTON_LMASK) != 0;
		if (down_now)
		{
			SDL_SetWindowPosition(it->second.window->window,
				(int)std::lround(mx - live_offx_),
				(int)std::lround(my - live_offy_));
			if (should_merge(it->second))
			{
				live_drag_ = false;
				merge_back(it);
			}
		}
		else
		{
			live_drag_ = false;
			if (should_merge(it->second))
				merge_back(it);
		}
	}

	void update_floaters()
	{
		if (floaters_.empty())
			return;
		float mx_unused = 0, my_unused = 0;
		(void)mx_unused;
		(void)my_unused;
		const Uint32 bs = SDL_GetGlobalMouseState(nullptr, nullptr);
		const bool down = (bs & SDL_BUTTON_LMASK) != 0;
		const bool released = float_drag_was_down_ && !down;
		for (auto it = floaters_.begin(); it != floaters_.end();)
		{
			floater& fw = it->second;
			if (!fw.window || !window_alive(fw.window))
			{
				/* The window died outside our control (host pruned it):
				 * restore the panel, drop the floater. */
				merge_back(it++);
				continue;
			}
			if (fw.window->pending_close)
			{
				merge_back(it++);
				continue;
			}
			if (!fw.hit_test_set)
			{
				const float pr = fw.window->pixel_ratio() > 0.0f
					? fw.window->pixel_ratio() : 1.0f;
				fw.window->set_hit_test(
					(int)(opts_.hit_test_px / pr + 0.5f));
				fw.hit_test_set = true;
			}
			if (released && should_merge(fw))
			{
				merge_back(it++);
				continue;
			}
			/* System-modal title-bar drags swallow the frames on Windows; the
			 * only visible signal afterwards is "the window position changed
			 * while our button was up".  settle skips creation jitter. */
			if (!down && !released)
			{
				int fx = 0, fy = 0;
				SDL_GetWindowPosition(fw.window->window, &fx, &fy);
				const int moved = std::abs(fx - fw.last_x) +
					std::abs(fy - fw.last_y);
				fw.last_x = fx;
				fw.last_y = fy;
				if (fw.settle <= 0 && moved > 6 && should_merge(fw))
				{
					merge_back(it++);
					continue;
				}
			}
			else
			{
				SDL_GetWindowPosition(fw.window->window,
					&fw.last_x, &fw.last_y);
			}
			if (fw.settle > 0)
				--fw.settle;
			++it;
		}
		float_drag_was_down_ = down;
	}

	app_mgr* mgr_ = nullptr;
	multiwindow_host* host_ = nullptr;
	vgui_widget* main_dock_ = nullptr;
	os_window* main_window_ = nullptr;
	font_familys_t* familys_ = nullptr;
	float font_size_ = 16.0f;
	dock_floater_opts opts_;
	hooks hooks_;

	std::map<vgui_item_id, floater> floaters_;
	std::deque<tear_off_request> pending_tear_offs_;
	float tear_off_sx_ = 0.0f, tear_off_sy_ = 0.0f;

	bool live_drag_ = false;
	vgui_item_id live_panel_ = 0;
	float live_offx_ = 0.0f, live_offy_ = 0.0f;

	/* Dock-tab drag polling state (rect sampled at drag start). */
	bool drag_rect_valid_ = false;
	vgui_item_id drag_panel_seen_ = 0;
	float drag_start_mx_ = 0.0f, drag_start_my_ = 0.0f;
	float drag_rect_x_ = 0.0f, drag_rect_y_ = 0.0f;
	float drag_rect_w_ = 0.0f, drag_rect_h_ = 0.0f;

	bool float_drag_was_down_ = false;
};
