#include "viewport_view.h"

namespace vgui_editor_kit {

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

	vgui_widget* viewport_view::widget() const noexcept
	{
		return image_;
	}

} /* namespace vgui_editor_kit */
