#pragma once
/*
 * vgui_multiwindow.h - host helper for running several OS windows in one
 * process (goal A of docs/dock_multiwindow_assessment.md).
 *
 * libvgui core has no windowing code: each OS window owns its own
 * gui_viewport / vgui_context, and the SDL3 backend routes events by
 * SDL_WindowID.  The host's only non-obvious duty is the frame loop:
 *
 *   1. Each window needs its OWN rvg_t.  get_draw_list() returns an
 *      ovg_draw_data_t full of raw pointers into the rvg's internal vectors,
 *      so recording two windows into one rvg (clear -> draw -> clear -> draw)
 *      would make every draw data entry point at the same buffer.
 *   2. Do NOT call vgpu_acquire_swapchain() once per window.  It stores a
 *      single ctx->current_cmd and the next call submits the previous one
 *      before it has been drawn.  Instead collect every window's draw data,
 *      each tagged with its own fbo, and submit them all in one
 *      vgpu_render_frame() call -- it encodes one render pass per FBO inside
 *      a single command buffer.
 *
 * This header-only helper implements exactly that.  The host still owns the
 * vgui_context lifetime; gui_viewport::~gui_viewport() destroys the context
 * assigned to viewport->ui unless the host clears that pointer first (see
 * vgui_demo.cpp for the manual-destroy pattern).
 */
#include <vector>

#include <SDL3/SDL.h>
#include <vgui/backend/vgui_sdl3.h>

struct multiwindow_slot
{
	os_window* window = nullptr;
	vgui_context* ui = nullptr;
	rvg_t* canvas = nullptr;
	int width = 0;
	int height = 0;
};

class multiwindow_host
{
public:
	multiwindow_host(app_mgr* mgr, ovg_ctx_cb* cb) : mgr_(mgr), cb_(cb) {}
	~multiwindow_host()
	{
		for (multiwindow_slot& s : slots_)
			if (s.canvas && cb_) cb_->destroy_rvg(s.canvas);
	}
	multiwindow_host(const multiwindow_host&) = delete;
	multiwindow_host& operator=(const multiwindow_host&) = delete;

	// Binds a UI tree to a window and allocates its private recording canvas.
	// Returns false for null arguments, a duplicate window, or a failed rvg.
	bool attach(os_window* window, vgui_context* ui)
	{
		if (!window || !ui || !cb_ || !window->viewport) return false;
		// Refuse to silently orphan a context already bound to this viewport.
		if (window->viewport->ui && window->viewport->ui != ui) return false;
		for (const multiwindow_slot& s : slots_)
			if (s.window == window) return false;
		rvg_t* canvas = cb_->new_rvg(cb_->ac);
		if (!canvas) return false;
		window->viewport->ui = ui;
		multiwindow_slot s;
		s.window = window;
		s.ui = ui;
		s.canvas = canvas;
		slots_.push_back(s);
		return true;
	}

	// Releases the recording canvas only.  The window and its UI tree belong
	// to the host / app_mgr.
	void detach(os_window* window)
	{
		for (auto it = slots_.begin(); it != slots_.end(); ++it)
		{
			if (it->window != window) continue;
			if (it->canvas && cb_) cb_->destroy_rvg(it->canvas);
			slots_.erase(it);
			return;
		}
	}

	const std::vector<multiwindow_slot>& slots() const { return slots_; }
	size_t size() const { return slots_.size(); }

	// Drops slots whose window has been destroyed by app_mgr.  Call this
	// after processing WINDOW_CLOSE_REQUESTED / destroy().
	void prune()
	{
		for (auto it = slots_.begin(); it != slots_.end();)
		{
			if (alive(it->window)) { ++it; continue; }
			if (it->canvas && cb_) cb_->destroy_rvg(it->canvas);
			it = slots_.erase(it);
		}
	}

	// Start of frame: sync each window's pixel size, clear its canvas and
	// advance the UI tree.  The host then calls
	//   vgui_draw(s.ui, cb, s.canvas);
	// for every slot (see draw_all below for the common case).
	void begin_frame(float delta)
	{
		if (!cb_) return;
		for (multiwindow_slot& s : slots_)
		{
			if (!alive(s.window)) continue;
			int w = 0, h = 0;
			if (SDL_GetWindowSizeInPixels(s.window->window, &w, &h) && w > 0 && h > 0)
			{
				s.width = w;
				s.height = h;
				vgui_context_set_size(s.ui, w, h);
				s.canvas->width = w;
				s.canvas->height = h;
			}
			cb_->clear(s.canvas);
			cb_->reset_clip(s.canvas, 1);
			vgui_update(s.ui, delta);
		}
	}

	// Convenience: record every attached UI tree into its own canvas.
	// A host that needs custom per-window drawing can skip this and loop
	// over slots() itself.
	void draw_all()
	{
		if (!cb_) return;
		for (const multiwindow_slot& s : slots_)
			if (alive(s.window)) vgui_draw(s.ui, cb_, s.canvas);
	}

	// End of frame: collect one draw data entry per window (each tagged with
	// that window's fbo) and submit them all in a single command buffer.
	// Returns the number of windows submitted.
	int end_frame()
	{
		if (!mgr_ || !mgr_->vgctx) return 0;
		draw_datas_cx drawable;
		int count = 0;
		for (const multiwindow_slot& s : slots_)
		{
			if (!alive(s.window)) continue;
			drawable.set(&s.window->fbo);
			drawable.add(s.canvas);
			++count;
		}
		if (count == 0)
		{
			// Still flush a dangling current_cmd (e.g. the host called
			// vgpu_acquire_swapchain earlier this frame).
			vgpu_render_frame(mgr_->vgctx, nullptr, 0);
			return 0;
		}
		drawable.render(mgr_->vgctx);
		return count;
	}

private:
	bool alive(os_window* window) const
	{
		if (!window || !mgr_) return false;
		for (const auto& w : mgr_->windows())
			if (w.get() == window && !w->pending_close) return true;
		return false;
	}

	app_mgr* mgr_ = nullptr;
	ovg_ctx_cb* cb_ = nullptr;
	std::vector<multiwindow_slot> slots_;
};
