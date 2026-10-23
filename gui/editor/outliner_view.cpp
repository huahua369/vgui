#include "outliner_view.h"

#include <cstdint>
#include <utility>

namespace vgui_editor_kit {

	void outliner_view::build(editor_context& context, vgui_widget* parent)
	{
		if (tree_ || !parent)
			return;

		context_ = &context;
		tree_ = vgui_widget_add(parent, VGUI_TREE, nullptr);
		vgui_widget_set_grow(tree_, 1.0f);
		vgui_widget_set_min_size(tree_, 120, 80);
		vgui_tree_set_selection_mode(tree_, VGUI_SELECTION_SINGLE);
		vgui_tree_on_select(tree_, &outliner_view::on_select, this);
	}

	void outliner_view::clear()
	{
		if (tree_)
			vgui_tree_clear(tree_);
		selected_ = 0;
	}

	vgui_item_id outliner_view::add_node(const char* text, vgui_item_id parent,
		object_id object)
	{
		if (!tree_)
			return 0;

		vgui_item_id item = vgui_tree_add_node(tree_, parent, text, nullptr);
		if (item == 0)
			return 0;

		const object_id stable = object != invalid_object
			? object
			: static_cast<object_id>(item);
		vgui_tree_set_item_user(tree_, item,
			reinterpret_cast<void*>(static_cast<std::uintptr_t>(stable)));
		return item;
	}

	void outliner_view::expand(vgui_item_id node)
	{
		if (tree_)
			vgui_tree_set_item_expanded(tree_, node, 1);
	}

	vgui_item_id outliner_view::selected() const noexcept
	{
		return selected_;
	}

	vgui_widget* outliner_view::widget() const noexcept
	{
		return tree_;
	}

	void outliner_view::set_selection_listener(selection_listener listener)
	{
		listener_ = std::move(listener);
	}

	void outliner_view::on_select(vgui_widget* widget, vgui_item_id item,
		int selected, void* user)
	{
		(void)widget;
		if (!selected || !user)
			return;

		auto* self = static_cast<outliner_view*>(user);
		self->selected_ = item;

		object_id object = invalid_object;
		if (self->tree_)
		{
			void* value = vgui_tree_get_item_user(self->tree_, item);
			object = static_cast<object_id>(reinterpret_cast<std::uintptr_t>(value));
		}
		if (object == invalid_object)
			object = static_cast<object_id>(item);

		if (self->context_)
			self->context_->set_selection(object);
		if (self->listener_)
			self->listener_(object);
	}

} /* namespace vgui_editor_kit */
