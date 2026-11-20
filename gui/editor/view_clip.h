#pragma once

#include <glm/glm.hpp>
#include "ovg_c.h"

#include <algorithm>
#include <cmath>

namespace vgui_editor_kit {

	/* OVG save/restore does not track the scissor. Callers must skip paint
	 * when visible() is false: a zero-area scissor is not a portable way
	 * to suppress drawing (SDL ignores it and CPU images treat it as unset). */
	class scoped_view_clip
	{
	public:
		scoped_view_clip(ovg_ctx_cb* cb, rvg_t* vg, float x, float y,
			float w, float h) : cb_(cb), vg_(vg)
		{
			if (!(w > 0.0f && h > 0.0f))
				return;
			visible_ = true;
			// Without a getter we cannot safely replace an inherited scissor.
			if (!cb_ || !vg_ || !cb_->get_clip_rect)
				return;
			cb_->get_clip_rect(vg_, &saved_);
			int x0 = (int)std::floor(x);
			int y0 = (int)std::floor(y);
			int x1 = (int)std::ceil(x + w);
			int y1 = (int)std::ceil(y + h);
			// OVG reports zero extents at reset and negative extents for
			// explicitly disabled clipping; neither describes a parent clip.
			if (saved_.z > 0 && saved_.w > 0)
			{
				x0 = std::max(x0, saved_.x);
				y0 = std::max(y0, saved_.y);
				x1 = std::min(x1, saved_.x + saved_.z);
				y1 = std::min(y1, saved_.y + saved_.w);
			}
			visible_ = x1 > x0 && y1 > y0;
			if (!visible_ || !cb_->clip_rect)
				return;
			if (saved_ == glm::ivec4(x0, y0, x1 - x0, y1 - y0))
				return;
			cb_->clip_rect(vg_, x0, y0, x1 - x0, y1 - y0);
			changed_ = true;
		}

		~scoped_view_clip()
		{
			if (!changed_)
				return;
			// Reapply a real scissor verbatim. If OVG was reset, send the
			// explicit disabled sentinel so SDL also drops our scissor.
			if (saved_.z == 0 || saved_.w == 0)
				cb_->clip_rect(vg_, 0, 0, -1, -1);
			else if (cb_->set_clip_rect)
				cb_->set_clip_rect(vg_, &saved_);
			else
				cb_->clip_rect(vg_, saved_.x, saved_.y, saved_.z, saved_.w);
		}

		scoped_view_clip(const scoped_view_clip&) = delete;
		scoped_view_clip& operator=(const scoped_view_clip&) = delete;

		bool visible() const { return visible_; }

	private:
		ovg_ctx_cb* cb_;
		rvg_t* vg_;
		glm::ivec4 saved_{};
		bool visible_ = false;
		bool changed_ = false;
	};

} // namespace vgui_editor_kit
