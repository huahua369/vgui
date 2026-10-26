#include "viewport_view.h"

#include "drag_bridge.h"

#include <algorithm>

namespace vgui_editor_kit {

	namespace {

		/* F03/A04: 视口的 drop target——接受资产 / 场景对象并转发给宿主。
		 * 悬停时在光标处画一个紧凑落点方框,宿主拒绝则整个提示消失。 */
		class viewport_drop_target final : public drop_target
		{
		public:
			explicit viewport_drop_target(viewport_view* owner) : owner_(owner) {}

			bool can_accept(const drag_payload& payload) const override
			{
				return payload.kind == drag_payload_kind::asset ||
					payload.kind == drag_payload_kind::scene_object;
			}

			void hover(const drag_payload& payload, vgui_widget* target,
				float x, float y, vgui_drop_hint* hint) override
			{
				(void)payload;
				(void)target;
				if (!owner_ || !hint)
					return;
				/* 光标提示:80x48 的目标框,钳制在视口范围内。 */
				float wx = 0, wy = 0, ww = 0, wh = 0;
				if (owner_->widget())
					vgui_widget_get_rect(owner_->widget(), &wx, &wy, &ww, &wh);
				const float w = 80.0f, h = 48.0f;
				hint->kind = VGUI_DROP_HINT_RECT;
				hint->x = x - w * 0.5f;
				hint->y = y - h * 0.5f;
				if (ww > 0.0f)
				{
					if (ww >= w)
						hint->x = std::clamp(hint->x, wx, wx + ww - w);
					else
						hint->x = wx;
				}
				if (wh > 0.0f)
				{
					if (wh >= h)
						hint->y = std::clamp(hint->y, wy, wy + wh - h);
					else
						hint->y = wy;
				}
				hint->w = w;
				hint->h = h;
			}

			bool drop(const drag_payload& payload, float x, float y) override
			{
				if (!owner_)
					return false;
				return owner_->record_drop(payload, x, y);
			}

		private:
			viewport_view* owner_;
		};

	} /* namespace */

	void viewport_view::build(vgui_widget* parent)
	{
		if (image_ || !parent)
			return;

		image_ = vgui_widget_add(parent, VGUI_IMAGE, nullptr);
		vgui_widget_set_grow(image_, 1.0f);
		vgui_widget_set_min_size(image_, 320, 200);
		vgui_widget_set_background(image_, 0xFF0E1116);
		vgui_widget_set_border(image_, 0xFF2A3038, 1);
		vgui_image_set_fit(image_, VGUI_IMAGE_FIT_CONTAIN);
	}

	void viewport_view::set_texture(vgui_texture* texture)
	{
		texture_ = texture;
		if (image_)
			vgui_image_set_texture(image_, texture);
	}

	vgui_texture* viewport_view::texture() const noexcept
	{
		return texture_;
	}

	void viewport_view::set_drop_listener(drop_listener listener)
	{
		listener_ = std::move(listener);
		if (!image_)
			return;
		if (!listener_)
		{
			set_editor_drop_target(image_, nullptr);
			return;
		}
		if (!drop_target_)
			drop_target_ = std::make_unique<viewport_drop_target>(this);
		set_editor_drop_target(image_, drop_target_.get());
	}

	const drag_payload& viewport_view::last_drop() const noexcept
	{
		return last_drop_;
	}

	float viewport_view::last_drop_x() const noexcept
	{
		return last_drop_x_;
	}

	float viewport_view::last_drop_y() const noexcept
	{
		return last_drop_y_;
	}

	bool viewport_view::record_drop(const drag_payload& payload, float x, float y)
	{
		if (!listener_ || payload.empty() || !listener_(payload, x, y))
			return false;
		last_drop_ = payload;
		last_drop_x_ = x;
		last_drop_y_ = y;
		return true;
	}

	vgui_widget* viewport_view::widget() const noexcept
	{
		return image_;
	}

} /* namespace vgui_editor_kit */
