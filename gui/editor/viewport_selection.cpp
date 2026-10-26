#include "viewport_selection.h"

#include "pch.h"
#include "tools/tools.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace vgui_editor_kit {

	namespace {

		/* vgui 0xAARRGGBB -> OVG 0xAABBGGRR. */
		uint32_t to_ovg(vgui_color color)
		{
			return (color & 0xFF00FF00u) |
				((color & 0x000000FFu) << 16) |
				((color >> 16) & 0x000000FFu);
		}

		bool contains(const std::vector<object_id>& ids, object_id id)
		{
			return std::find(ids.begin(), ids.end(), id) != ids.end();
		}

	} /* namespace */

	viewport_selection::~viewport_selection()
	{
		detach();
	}

	void viewport_selection::attach(viewport_view& viewport, editor_context& context)
	{
		detach();
		viewport_ = &viewport;
		context_ = &context;
		listener_token_ = viewport.add_event_listener(
			[this](const vgui_event* event) { return on_input(event); });
		if (!listener_token_)
		{
			viewport_ = nullptr;
			context_ = nullptr;
			return;
		}
		viewport.set_selection_overlay(
			[this](ovg_ctx_cb* cb, rvg_t* vg, float x, float y,
				float width, float height, float delta_time)
			{
				draw_selection(cb, vg, x, y, width, height, delta_time);
			});
		viewport.set_interaction_overlay(
			[this](ovg_ctx_cb* cb, rvg_t* vg, float x, float y,
				float width, float height, float delta_time)
			{
				draw_interaction(cb, vg, x, y, width, height, delta_time);
			});
	}

	void viewport_selection::detach()
	{
		if (viewport_ && listener_token_)
			viewport_->remove_event_listener(listener_token_);
		if (viewport_)
		{
			viewport_->set_selection_overlay(nullptr);
			viewport_->set_interaction_overlay(nullptr);
		}
		viewport_ = nullptr;
		context_ = nullptr;
		listener_token_ = 0;
		phase_ = phase::idle;
	}

	bool viewport_selection::attached() const noexcept
	{
		return viewport_ != nullptr;
	}

	void viewport_selection::set_point_picker(point_picker picker)
	{
		point_picker_ = std::move(picker);
	}

	void viewport_selection::set_box_picker(box_picker picker)
	{
		box_picker_ = std::move(picker);
	}

	void viewport_selection::set_highlight_provider(highlight_provider provider)
	{
		highlight_provider_ = std::move(provider);
	}

	void viewport_selection::set_enabled(bool enabled)
	{
		enabled_ = enabled;
		if (!enabled_)
			cancel_selection();
	}

	bool viewport_selection::enabled() const noexcept
	{
		return enabled_;
	}

	void viewport_selection::set_multi_select(bool enabled)
	{
		multi_select_ = enabled;
	}

	bool viewport_selection::multi_select() const noexcept
	{
		return multi_select_;
	}

	void viewport_selection::set_drag_threshold(float pixels)
	{
		drag_threshold_ = pixels;
	}

	bool viewport_selection::pending() const noexcept
	{
		return phase_ == phase::pending;
	}

	bool viewport_selection::selecting() const noexcept
	{
		return phase_ == phase::box;
	}

	void viewport_selection::cancel_selection()
	{
		phase_ = phase::idle;
	}

	const std::vector<object_id>& viewport_selection::selected_ids() const
	{
		static const std::vector<object_id> empty;
		return context_ ? context_->selection_ids() : empty;
	}

	float viewport_selection::effective_threshold() const
	{
		if (drag_threshold_ >= 0.0f)
			return drag_threshold_;
		if (context_ && context_->ui())
		{
			vgui_style style = {};
			vgui_context_get_style(context_->ui(), &style);
			return std::max(1.0f, style.dock.drag_threshold);
		}
		return 4.0f;
	}

	void viewport_selection::content_geometry(float* offset_x, float* offset_y,
		int* width, int* height) const
	{
		float ox = 1.0f, oy = 1.0f, cw = 0.0f, ch = 0.0f;
		if (viewport_)
			viewport_->get_content_rect(&ox, &oy, &cw, &ch);
		if (offset_x)
			*offset_x = ox;
		if (offset_y)
			*offset_y = oy;
		if (width)
			*width = std::max(1, (int)std::lround(cw));
		if (height)
			*height = std::max(1, (int)std::lround(ch));
	}

	int viewport_selection::on_input(const vgui_event* event)
	{
		if (!event || !viewport_ || !context_ || !enabled_)
			return 0;

		float ox = 0.0f, oy = 0.0f;
		int cw = 0, ch = 0;
		content_geometry(&ox, &oy, &cw, &ch);

		switch (event->type)
		{
		case VGUI_EVENT_MOUSE_DOWN:
			if (event->button != 1 || phase_ != phase::idle)
				return 0;
			phase_ = phase::pending;
			start_x_ = event->x - ox;
			start_y_ = event->y - oy;
			current_x_ = start_x_;
			current_y_ = start_y_;
			width_ = cw;
			height_ = ch;
			return 0;

		case VGUI_EVENT_MOUSE_MOVE:
			if (phase_ == phase::idle)
				return 0;
			current_x_ = event->x - ox;
			current_y_ = event->y - oy;
			if (phase_ == phase::pending)
			{
				const float threshold = effective_threshold();
				if (std::fabs(current_x_ - start_x_) > threshold ||
					std::fabs(current_y_ - start_y_) > threshold)
					phase_ = phase::box;
			}
			return phase_ == phase::box ? 1 : 0;

		case VGUI_EVENT_MOUSE_UP:
			if (event->button != 1 || phase_ == phase::idle)
				return 0;
			current_x_ = event->x - ox;
			current_y_ = event->y - oy;
			width_ = cw;
			height_ = ch;
			{
				const int mods = vgui_context_get_mods(context_->ui());
				if (phase_ == phase::box)
				{
					if (box_picker_)
					{
						selection_box_query query;
						query.x0 = std::min(start_x_, current_x_);
						query.y0 = std::min(start_y_, current_y_);
						query.x1 = std::max(start_x_, current_x_);
						query.y1 = std::max(start_y_, current_y_);
						query.width = cw;
						query.height = ch;
						query.mods = mods;
						apply_selection(box_picker_(query), mods);
					}
				}
				else if (point_picker_)
				{
					selection_point_query query;
					query.x = current_x_;
					query.y = current_y_;
					query.width = cw;
					query.height = ch;
					query.mods = mods;
					const object_id hit = point_picker_(query);
					std::vector<object_id> picked;
					if (hit != invalid_object)
						picked.push_back(hit);
					apply_selection(picked, mods);
				}
			}
			phase_ = phase::idle;
			return 1;

		case VGUI_EVENT_CANCEL:
			if (phase_ == phase::idle)
				return 0;
			phase_ = phase::idle;
			return 1;

		case VGUI_EVENT_KEY_DOWN:
			if (event->key == VGUI_KEY_ESCAPE && phase_ != phase::idle)
			{
				phase_ = phase::idle;
				return 1;
			}
			return 0;

		default:
			return 0;
		}
	}

	void viewport_selection::apply_selection(const std::vector<object_id>& picked,
		int mods)
	{
		if (!context_)
			return;

		const bool ctrl = multi_select_ && (mods & VGUI_MOD_CTRL) != 0;
		const bool shift = multi_select_ && (mods & VGUI_MOD_SHIFT) != 0;
		if (picked.empty() && (ctrl || shift))
			return;	/* empty modifier click keeps the current selection */

		std::vector<object_id> next;
		object_id primary = invalid_object;
		if (ctrl)
		{
			next = context_->selection_ids();
			for (object_id id : picked)
			{
				if (id == invalid_object)
					continue;
				auto it = std::find(next.begin(), next.end(), id);
				if (it != next.end())
					next.erase(it);
				else
					next.push_back(id);
			}
			primary = context_->selection();
		}
		else if (shift)
		{
			next = context_->selection_ids();
			for (object_id id : picked)
			{
				if (id != invalid_object && !contains(next, id))
					next.push_back(id);
			}
			primary = context_->selection();
		}
		else
		{
			next = picked;
			if (!next.empty())
				primary = next.front();
		}

		if (primary == invalid_object || !contains(next, primary))
			primary = next.empty() ? invalid_object : next.front();
		context_->set_selection(next, primary);
	}

	void viewport_selection::draw_selection(ovg_ctx_cb* cb, rvg_t* vg,
		float x, float y, float width, float height, float delta_time)
	{
		(void)delta_time;
		(void)width;
		(void)height;
		if (!context_ || !cb || !vg || !highlight_provider_)
			return;
		const std::vector<object_id> ids = context_->selection_ids();
		if (ids.empty())
			return;
		const std::vector<selection_box> boxes = highlight_provider_(ids);
		if (boxes.empty())
			return;

		vgui_color accent_color = 0xFF409EFF;
		vgui_color hover_color = 0xFF74B6FF;
		if (context_->ui())
		{
			vgui_theme theme = {};
			vgui_context_get_theme(context_->ui(), &theme);
			if (theme.accent)
				accent_color = theme.accent;
			if (theme.accent_hover)
				hover_color = theme.accent_hover;
		}
		const uint32_t accent = to_ovg(accent_color);
		const uint32_t hover = to_ovg(hover_color);

		for (const selection_box& box : boxes)
		{
			if (box.width <= 0.0f || box.height <= 0.0f)
				continue;
			const float bx = x + box.x;
			const float by = y + box.y;
			ovg_tools::fill_rect(cb, vg, bx, by, box.width, box.height,
				ovg_tools::with_alpha(accent, 28));
			const bool primary = box.id == context_->selection();
			ovg_tools::stroke_rect(cb, vg, bx, by, box.width, box.height,
				primary ? hover : accent, primary ? 1.5f : 1.0f);
		}
	}

	void viewport_selection::draw_interaction(ovg_ctx_cb* cb, rvg_t* vg,
		float x, float y, float width, float height, float delta_time)
	{
		(void)width;
		(void)height;
		if (!context_ || !cb || !vg || phase_ != phase::box)
			return;

		vgui_color accent_color = 0xFF409EFF;
		if (context_->ui())
		{
			vgui_theme theme = {};
			vgui_context_get_theme(context_->ui(), &theme);
			if (theme.accent)
				accent_color = theme.accent;
		}
		const uint32_t accent = to_ovg(accent_color);
		marquee_offset_ += std::max(0.0f, delta_time) * 24.0f;

		const float ax = x + start_x_;
		const float ay = y + start_y_;
		const float bx = x + current_x_;
		const float by = y + current_y_;
		const float rx = std::min(ax, bx);
		const float ry = std::min(ay, by);
		const float rw = std::fabs(bx - ax);
		const float rh = std::fabs(by - ay);
		ovg_tools::fill_rect(cb, vg, rx, ry, rw, rh,
			ovg_tools::with_alpha(accent, 20));
		ovg_tools::marquee(cb, vg, ax, ay, bx, by, accent, 1.0f,
			marquee_offset_);
	}

} /* namespace vgui_editor_kit */
