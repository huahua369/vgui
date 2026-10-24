#include "viewport_view.h"

#include "drag_bridge.h"

namespace vgui_editor_kit {

	namespace {

		/* F03: 视口的 drop target——接受资产 / 场景对象并转发给宿主。 */
		class viewport_drop_target final : public drop_target
		{
		public:
			explicit viewport_drop_target(viewport_view* owner) : owner_(owner) {}

			bool can_accept(const drag_payload& payload) const override
			{
				return payload.kind == drag_payload_kind::asset ||
					payload.kind == drag_payload_kind::scene_object;
			}

			bool drop(const drag_payload& payload, float x, float y) override
			{
				if (!owner_)
					return false;
				owner_->record_drop(payload, x, y);
				return true;
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

	void viewport_view::record_drop(const drag_payload& payload, float x, float y)
	{
		last_drop_ = payload;
		if (listener_)
			listener_(payload, x, y);
	}

	vgui_widget* viewport_view::widget() const noexcept
	{
		return image_;
	}

} /* namespace vgui_editor_kit */
