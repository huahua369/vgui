#include "outliner_view.h"

#include "drag_bridge.h"

#include <cstdint>
#include <string>
#include <utility>

namespace vgui_editor_kit {

	namespace {

		/* F03: 树上的 drop target——接受 scene_object 载荷，落到行上重排。 */
		class outliner_drop_target final : public drop_target
		{
		public:
			explicit outliner_drop_target(outliner_view* owner) : owner_(owner) {}

			bool can_accept(const drag_payload& payload) const override
			{
				return payload.kind == drag_payload_kind::scene_object;
			}

			bool drop(const drag_payload& payload, float x, float y) override
			{
				if (!owner_)
					return false;
				object_id parent = invalid_object;
				std::size_t index = 0;
				if (!owner_->row_drop_location(x, y, &parent, &index))
					return false;
				object_id node = invalid_object;
				try
				{
					node = static_cast<object_id>(std::stoull(payload.id));
				}
				catch (...)
				{
					return false;
				}
				return owner_->move_node(node, parent, index);
			}

		private:
			outliner_view* owner_;
		};

	} /* namespace */

	/* Undo record for one outliner move.  Commands capture the owner through the
	 * alive token so stale records left on a shared stack become no-ops. */
	class outliner_view::move_command final : public command
	{
	public:
		move_command(outliner_view* owner, std::weak_ptr<int> alive, object_id node,
			node_location old_location, object_id new_parent, std::size_t new_index)
			: owner_(owner), alive_(std::move(alive)), node_(node),
			old_(old_location), new_parent_(new_parent), new_index_(new_index)
		{
		}

		const char* name() const noexcept override
		{
			return "Move node";
		}

		bool execute() override
		{
			if (alive_.expired())
				return true;
			return owner_->apply_move(node_, new_parent_, new_index_);
		}

		void undo() override
		{
			if (!alive_.expired() && old_.valid)
				(void)owner_->apply_move(node_, old_.parent, old_.index);
		}

	private:
		outliner_view* owner_;
		std::weak_ptr<int> alive_;
		object_id node_;
		node_location old_;
		object_id new_parent_;
		std::size_t new_index_;
	};

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

	void outliner_view::set_command_stack(command_stack* stack)
	{
		command_stack_ = stack;
	}

	void outliner_view::set_move_handler(move_handler handler)
	{
		move_handler_ = std::move(handler);
	}

	bool outliner_view::move_node(object_id node, object_id new_parent,
		std::size_t new_index)
	{
		if (!tree_ || !move_handler_)
			return false;

		const vgui_item_id item = item_for_object(node);
		if (item == 0)
			return false;

		const node_location old = location_of(item);
		if (old.valid && old.parent == new_parent && old.index == new_index)
			return true;

		if (!move_handler_(node, new_parent, new_index))
			return false;

		if (command_stack_)
		{
			command_stack_->push(std::make_unique<move_command>(
				this, alive_token_, node, old, new_parent, new_index));
		}
		return true;
	}

	object_id outliner_view::object_for_item(vgui_item_id item) const
	{
		if (!tree_ || item == 0)
			return invalid_object;
		void* value = vgui_tree_get_item_user(tree_, item);
		object_id object =
			static_cast<object_id>(reinterpret_cast<std::uintptr_t>(value));
		if (object == invalid_object)
			object = static_cast<object_id>(item);
		return object;
	}

	vgui_item_id outliner_view::item_for_object(object_id object) const
	{
		if (!tree_ || object == invalid_object)
			return 0;

		std::vector<vgui_item_id> stack;
		for (int i = vgui_tree_get_root_count(tree_) - 1; i >= 0; --i)
			stack.push_back(vgui_tree_get_root(tree_, i));

		while (!stack.empty())
		{
			const vgui_item_id item = stack.back();
			stack.pop_back();
			if (object_for_item(item) == object)
				return item;
			for (int i = vgui_tree_get_child_count(tree_, item) - 1; i >= 0; --i)
				stack.push_back(vgui_tree_get_child(tree_, item, i));
		}
		return 0;
	}

	outliner_view::node_location outliner_view::location_of(vgui_item_id item) const
	{
		node_location location;
		if (!tree_ || item == 0)
			return location;

		const vgui_item_id parent_item = vgui_tree_get_parent(tree_, item);
		if (parent_item != 0)
			location.parent = object_for_item(parent_item);

		const int count = parent_item != 0
			? vgui_tree_get_child_count(tree_, parent_item)
			: vgui_tree_get_root_count(tree_);
		for (int i = 0; i < count; ++i)
		{
			const vgui_item_id sibling = parent_item != 0
				? vgui_tree_get_child(tree_, parent_item, i)
				: vgui_tree_get_root(tree_, i);
			if (sibling == item)
			{
				location.index = static_cast<std::size_t>(i);
				location.valid = true;
				break;
			}
		}
		return location;
	}

	bool outliner_view::apply_move(object_id node, object_id parent,
		std::size_t index)
	{
		if (!move_handler_)
			return false;
		return move_handler_(node, parent, index);
	}

	void outliner_view::enable_drag_drop()
	{
		if (!tree_)
			return;

		set_editor_row_drag(tree_, true,
			[this](vgui_item_id item, int index, drag_payload& out) -> bool {
				(void)index;
				if (!tree_ || item == 0)
					return false;
				const object_id object = object_for_item(item);
				if (object == invalid_object)
					return false;
				out.kind = drag_payload_kind::scene_object;
				out.id = std::to_string(object);
				return true;
			});
		if (!drop_target_)
			drop_target_ = std::make_unique<outliner_drop_target>(this);
		set_editor_drop_target(tree_, drop_target_.get());
	}

	bool outliner_view::row_drop_location(float x, float y, object_id* out_parent,
		std::size_t* out_index) const
	{
		if (!tree_ || !out_parent || !out_index)
			return false;
		/* vgui_tree_item_at_point 接收控件局部坐标。 */
		float wx = 0.0f, wy = 0.0f, ww = 0.0f, wh = 0.0f;
		vgui_widget_get_rect(tree_, &wx, &wy, &ww, &wh);
		const vgui_item_id item = vgui_tree_item_at_point(tree_,
			x - wx, y - wy);
		if (item == 0)
		{
			/* 空白区：追加到根。 */
			*out_parent = invalid_object;
			*out_index = static_cast<std::size_t>(
				vgui_tree_get_root_count(tree_));
			return true;
		}
		const node_location location = location_of(item);
		if (!location.valid)
			return false;
		*out_parent = location.parent;
		*out_index = location.index;
		return true;
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
			object = self->object_for_item(item);

		if (self->context_)
			self->context_->set_selection(object);
		if (self->listener_)
			self->listener_(object);
	}

} /* namespace vgui_editor_kit */
