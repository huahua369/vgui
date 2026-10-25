#include "outliner_view.h"

#include "drag_bridge.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>

namespace vgui_editor_kit {

	namespace {

		char ascii_lower(char c)
		{
			return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
		}

		bool contains_ci(const std::string& hay, const std::string& needle)
		{
			if (needle.empty() || needle.size() > hay.size())
				return false;
			for (std::size_t i = 0; i + needle.size() <= hay.size(); ++i)
			{
				std::size_t j = 0;
				while (j < needle.size() &&
					ascii_lower(hay[i + j]) == ascii_lower(needle[j]))
					++j;
				if (j == needle.size())
					return true;
			}
			return false;
		}

		std::vector<std::string> split_ids(const std::string& text)
		{
			std::vector<std::string> out;
			std::size_t start = 0;
			while (start <= text.size())
			{
				const std::size_t comma = text.find(',', start);
				const std::string part = text.substr(start,
					comma == std::string::npos ? std::string::npos : comma - start);
				if (!part.empty())
					out.push_back(part);
				if (comma == std::string::npos)
					break;
				start = comma + 1;
			}
			return out;
		}

	/* context 级注册块:核心没有 destroy 回调,按 drag_bridge 的既定模式
	 * 泄漏持有,thunk 用 weak 存活令牌挡住已销毁视图。 */
	struct context_entry
	{
		std::weak_ptr<int> alive;
		outliner_view* owner = nullptr;
	};

	/* F03: 树上的 drop target——接受 scene_object 载荷;hover 依 O03 插入语义
	 * 给出插入线/高亮提示,非法位置不给提示(拒绝反馈)。 */
		class outliner_drop_target final : public drop_target
		{
		public:
			explicit outliner_drop_target(outliner_view* owner) : owner_(owner) {}

			bool can_accept(const drag_payload& payload) const override
			{
				return payload.kind == drag_payload_kind::scene_object;
			}

			void hover(const drag_payload& payload, vgui_widget* target, float x,
				float y, vgui_drop_hint* hint) override
			{
				(void)payload;
				(void)target;
				if (!owner_ || !hint)
					return;
				const auto location = owner_->drop_location_at(x, y);
				owner_->emit_hover_hint(location, hint);
			}

			bool drop(const drag_payload& payload, float x, float y) override
			{
				if (!owner_)
					return false;
				return owner_->apply_drop_by_payload(payload.id, x, y);
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

	/* Undo record for one outliner rename (O02), mirroring the asset browser. */
	class outliner_view::rename_command final : public command
	{
	public:
		rename_command(outliner_view* owner, std::weak_ptr<int> alive,
			object_id node, std::string old_name, std::string new_name)
			: owner_(owner), alive_(std::move(alive)), node_(node),
			old_name_(std::move(old_name)), new_name_(std::move(new_name))
		{
		}

		const char* name() const noexcept override
		{
			return "Rename node";
		}

		bool execute() override
		{
			if (alive_.expired())
				return true;
			return owner_->apply_rename(node_, new_name_);
		}

		void undo() override
		{
			if (!alive_.expired())
				(void)owner_->apply_rename(node_, old_name_);
		}

	private:
		outliner_view* owner_;
		std::weak_ptr<int> alive_;
		object_id node_;
		std::string old_name_;
		std::string new_name_;
	};

	//----------------------------------------------------------------------------
	// build / basic mirroring
	//----------------------------------------------------------------------------

	outliner_view::~outliner_view()
	{
		/* 只回触 editor_context(其生命周期必然覆盖本视图:build 需要它)。
		 * vgui_context 级注册不注销——后者可能已先被销毁;thunk 靠存活
		 * 令牌自守。 */
		if (context_ && selection_sync_id_)
			context_->remove_selection_listener(selection_sync_id_);
	}

	void outliner_view::build(editor_context& context, vgui_widget* parent)
	{
		if (tree_ || !parent)
			return;

		context_ = &context;
		parent_ = parent;
		tree_ = vgui_widget_add(parent, VGUI_TREE, nullptr);
		vgui_widget_set_grow(tree_, 1.0f);
		vgui_widget_set_min_size(tree_, 120, 80);
		vgui_widget_set_focusable(tree_, 1);
		vgui_tree_set_selection_mode(tree_, VGUI_SELECTION_SINGLE);
		vgui_tree_on_select(tree_, &outliner_view::on_select, this);
	}

	void outliner_view::clear()
	{
		if (!tree_)
			return;
		if (virtual_)
		{
			expanded_.clear();
			invalidate_vrows();
			vgui_tree_source_changed(tree_);
		}
		else
		{
			vgui_tree_clear(tree_);
		}
		selected_ = 0;
	}

	vgui_item_id outliner_view::add_node(const char* text, vgui_item_id parent,
		object_id object)
	{
		if (!tree_ || virtual_)
			return 0;

		vgui_item_id item = vgui_tree_add_node(tree_, parent, text, nullptr);
		if (item == 0)
			return 0;

		const object_id stable = object != invalid_object
			? object
			: static_cast<object_id>(item);
		vgui_tree_set_item_user(tree_, item,
			reinterpret_cast<void*>(static_cast<std::uintptr_t>(stable)));
		if (row_buttons_)
		{
			vgui_tree_set_item_buttons(tree_, item, 2);
			refresh_row_buttons(item, stable);
		}
		return item;
	}

	void outliner_view::expand(vgui_item_id node)
	{
		if (!tree_)
			return;
		if (virtual_)
		{
			const object_id object = object_for_item(node);
			if (object != invalid_object && expanded_.insert(object).second)
			{
				invalidate_vrows();
				vgui_tree_source_changed(tree_);
			}
			return;
		}
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

	//----------------------------------------------------------------------------
	// moves (F02/O03)
	//----------------------------------------------------------------------------

	bool outliner_view::move_node(object_id node, object_id new_parent,
		std::size_t new_index)
	{
		if (!tree_ || !move_handler_)
			return false;

		const vgui_item_id item = virtual_
			? static_cast<vgui_item_id>(node)
			: item_for_object(node);
		if (item == 0)
			return false;

		const node_location old = location_of(item);
		if (old.valid && old.parent == new_parent && old.index == new_index)
			return true;

		if (!apply_move(node, new_parent, new_index))
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
		if (virtual_)
			return item != 0 ? static_cast<object_id>(item) : invalid_object;
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
		if (virtual_)
		{
			ensure_vrows();
			const auto it = vrow_index_.find(object);
			return it != vrow_index_.end()
				? static_cast<vgui_item_id>(object) : 0;
		}

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

		if (virtual_)
		{
			ensure_vrows();
			const auto it = vrow_index_.find(static_cast<object_id>(item));
			if (it == vrow_index_.end())
				return location;
			const vrow& row = vrows_[(std::size_t)it->second];
			location.parent = row.parent;
			location.index = row.index_in_parent;
			location.valid = true;
			return location;
		}

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
		if (!move_handler_(node, parent, index))
			return false;
		if (virtual_ && tree_)
		{
			invalidate_vrows();
			vgui_tree_source_changed(tree_);
		}
		return true;
	}

	//----------------------------------------------------------------------------
	// O01 row extensions: icons, colors, visibility/lock buttons
	//----------------------------------------------------------------------------

	void outliner_view::set_node_icon(vgui_item_id node, vgui_texture* icon)
	{
		if (tree_ && !virtual_)
			vgui_tree_set_item_icon(tree_, node, icon);
	}

	void outliner_view::set_node_text_color(vgui_item_id node, vgui_color color)
	{
		if (tree_ && !virtual_)
			vgui_tree_set_item_text_color(tree_, node, color);
	}

	void outliner_view::enable_row_buttons()
	{
		if (!tree_ || row_buttons_)
			return;
		row_buttons_ = true;
		vgui_tree_on_item_button(tree_, &outliner_view::on_item_button, this);
		if (virtual_)
		{
			vgui_tree_source_rows_changed(tree_, 0,
				static_cast<int>(projected_row_count()));
			return;
		}
		/* 已有节点统一补上按钮。 */
		std::vector<vgui_item_id> stack;
		for (int i = vgui_tree_get_root_count(tree_) - 1; i >= 0; --i)
			stack.push_back(vgui_tree_get_root(tree_, i));
		while (!stack.empty())
		{
			const vgui_item_id item = stack.back();
			stack.pop_back();
			vgui_tree_set_item_buttons(tree_, item, 2);
			refresh_row_buttons(item, object_for_item(item));
			for (int i = vgui_tree_get_child_count(tree_, item) - 1; i >= 0; --i)
				stack.push_back(vgui_tree_get_child(tree_, item, i));
		}
	}

	void outliner_view::set_row_button_icons(vgui_texture* visible_on,
		vgui_texture* visible_off, vgui_texture* locked_on, vgui_texture* locked_off)
	{
		vis_icon_on_ = visible_on;
		vis_icon_off_ = visible_off;
		lock_icon_on_ = locked_on;
		lock_icon_off_ = locked_off;
		/* 刷新已有行的按钮图标。 */
		if (!tree_ || !row_buttons_)
			return;
		if (virtual_)
		{
			vgui_tree_source_rows_changed(tree_, 0,
				static_cast<int>(projected_row_count()));
			return;
		}
		std::vector<vgui_item_id> stack;
		for (int i = vgui_tree_get_root_count(tree_) - 1; i >= 0; --i)
			stack.push_back(vgui_tree_get_root(tree_, i));
		while (!stack.empty())
		{
			const vgui_item_id item = stack.back();
			stack.pop_back();
			refresh_row_buttons(item, object_for_item(item));
			for (int i = vgui_tree_get_child_count(tree_, item) - 1; i >= 0; --i)
				stack.push_back(vgui_tree_get_child(tree_, item, i));
		}
	}

	void outliner_view::refresh_row_buttons(vgui_item_id item, object_id object)
	{
		if (!tree_ || !row_buttons_ || item == 0)
			return;
		const auto it = flags_.find(object);
		const bool visible = it == flags_.end() ? true : it->second.first;
		const bool locked = it == flags_.end() ? false : it->second.second;
		vgui_tree_set_item_button_icon(tree_, item, 0,
			visible ? vis_icon_on_ : vis_icon_off_);
		vgui_tree_set_item_button_icon(tree_, item, 1,
			locked ? lock_icon_on_ : lock_icon_off_);
	}

	void outliner_view::set_object_visibility(object_id object, bool visible)
	{
		flags_[object] = { visible, object_locked(object) };
		if (!tree_ || !row_buttons_)
			return;
		if (virtual_)
		{
			ensure_vrows();
			const auto it = vrow_index_.find(object);
			if (it != vrow_index_.end())
				vgui_tree_source_rows_changed(tree_, it->second, 1);
			return;
		}
		refresh_row_buttons(item_for_object(object), object);
	}

	void outliner_view::set_object_locked(object_id object, bool locked)
	{
		flags_[object] = { object_visibility(object), locked };
		if (!tree_ || !row_buttons_)
			return;
		if (virtual_)
		{
			ensure_vrows();
			const auto it = vrow_index_.find(object);
			if (it != vrow_index_.end())
				vgui_tree_source_rows_changed(tree_, it->second, 1);
			return;
		}
		refresh_row_buttons(item_for_object(object), object);
	}

	void outliner_view::toggle_object_visibility(object_id object)
	{
		set_object_visibility(object, !object_visibility(object));
		if (visibility_listener_)
			visibility_listener_(object, object_visibility(object));
	}

	void outliner_view::toggle_object_lock(object_id object)
	{
		set_object_locked(object, !object_locked(object));
		if (lock_listener_)
			lock_listener_(object, object_locked(object));
	}

	bool outliner_view::object_visibility(object_id object) const
	{
		const auto it = flags_.find(object);
		return it == flags_.end() ? true : it->second.first;
	}

	bool outliner_view::object_locked(object_id object) const
	{
		const auto it = flags_.find(object);
		return it == flags_.end() ? false : it->second.second;
	}

	void outliner_view::set_visibility_listener(flag_listener listener)
	{
		visibility_listener_ = std::move(listener);
	}

	void outliner_view::set_lock_listener(flag_listener listener)
	{
		lock_listener_ = std::move(listener);
	}

	void outliner_view::on_item_button(vgui_widget* widget, vgui_item_id item,
		int button, void* user)
	{
		(void)widget;
		auto* self = static_cast<outliner_view*>(user);
		if (!self || item == 0)
			return;
		self->flag_changed(self->object_for_item(item), button);
	}

	void outliner_view::flag_changed(object_id object, int button)
	{
		if (object == invalid_object)
			return;
		if (button == 0)
			toggle_object_visibility(object);
		else if (button == 1)
			toggle_object_lock(object);
	}

	//----------------------------------------------------------------------------
	// O02 rename
	//----------------------------------------------------------------------------

	void outliner_view::set_rename_handler(rename_handler handler)
	{
		rename_handler_ = std::move(handler);
	}

	std::string outliner_view::node_text(vgui_item_id item, object_id object) const
	{
		if (virtual_)
			return source_.name ? source_.name(object) : std::string();
		if (!tree_ || item == 0)
			return std::string();
		const char* text = vgui_tree_get_item_text(tree_, item);
		return text ? std::string(text) : std::string();
	}

	std::size_t outliner_view::child_count_of(object_id object,
		vgui_item_id item) const
	{
		if (virtual_)
			return source_.child_count ? source_.child_count(object) : 0;
		if (!tree_ || item == 0)
			return 0;
		return static_cast<std::size_t>(vgui_tree_get_child_count(tree_, item));
	}

	bool outliner_view::rename_node(object_id node, const std::string& name)
	{
		if (!tree_ || name.empty())
			return false;
		if (name.find(',') != std::string::npos)
			return false;	/* 逗号保留给多选拖拽载荷 */

		const vgui_item_id item = virtual_
			? static_cast<vgui_item_id>(node)
			: item_for_object(node);
		if (item == 0)
			return false;

		const std::string old_name = node_text(item, node);
		if (old_name == name)
			return true;

		if (!apply_rename(node, name))
			return false;

		if (command_stack_)
		{
			command_stack_->push(std::make_unique<rename_command>(
				this, alive_token_, node, old_name, name));
		}
		return true;
	}

	bool outliner_view::apply_rename(object_id node, const std::string& name)
	{
		if (rename_handler_ && !rename_handler_(node, name))
			return false;
		if (!tree_)
			return true;
		if (virtual_)
		{
			ensure_vrows();
			const auto it = vrow_index_.find(node);
			if (it != vrow_index_.end())
				vgui_tree_source_rows_changed(tree_, it->second, 1);
			return true;
		}
		const vgui_item_id item = item_for_object(node);
		if (item != 0)
			vgui_tree_set_item_text(tree_, item, name.c_str());
		return true;
	}

	void outliner_view::ensure_edit()
	{
		if (edit_ || !tree_ || !parent_ || !context_)
			return;
		edit_ = vgui_widget_add(parent_, VGUI_EDIT, nullptr);
		vgui_widget_set_visible(edit_, 0);
		vgui_widget_on_event(edit_, &outliner_view::on_edit_event_thunk, this);
	}

	void outliner_view::enable_inline_rename()
	{
		if (!tree_ || !context_)
			return;
		inline_rename_ = true;
		ensure_edit();
		ensure_tree_event_handler();
		ensure_key_handler();
		if (!focus_entry_)
		{
			focus_entry_ = new context_entry{ alive_token_, this };
			vgui_context_set_focus_listener(context_->ui(),
				&outliner_view::on_focus_change_thunk, focus_entry_);
		}
	}

	void outliner_view::begin_rename(vgui_item_id node)
	{
		if (!tree_ || !context_ || node == 0 || renaming_)
			return;
		ensure_edit();
		if (!edit_)
			return;
		const object_id object = object_for_item(node);
		if (object == invalid_object)
			return;

		float rx = 0, ry = 0, rw = 0, rh = 0;
		if (!vgui_tree_get_item_rect(tree_, node, &rx, &ry, &rw, &rh))
			return;
		float px = 0, py = 0, pw = 0, ph = 0;
		if (parent_)
			vgui_widget_get_rect(parent_, &px, &py, &pw, &ph);
		(void)pw; (void)ph;

		rename_item_ = node;
		rename_object_ = object;
		renaming_ = true;
		vgui_edit_set_text(edit_, node_text(node, object).c_str());
		vgui_widget_set_pos(edit_, rx - px + 2.0f, ry - py + 1.0f);
		vgui_widget_set_size(edit_, std::max(40.0f, rw - 4.0f), std::max(16.0f, rh - 2.0f));
		vgui_widget_set_visible(edit_, 1);
		vgui_set_focus(context_->ui(), edit_);
		vgui_edit_select_all(edit_);
	}

	void outliner_view::finish_editor(bool restore_focus)
	{
		renaming_ = false;
		rename_item_ = 0;
		rename_object_ = invalid_object;
		if (edit_)
			vgui_widget_set_visible(edit_, 0);
		if (restore_focus && tree_ && context_ && vgui_get_focus(context_->ui()) == edit_)
			vgui_set_focus(context_->ui(), tree_);
	}

	void outliner_view::commit_rename()
	{
		if (!renaming_ || !edit_)
			return;
		const object_id object = rename_object_;
		const char* text = vgui_edit_get_text(edit_);
		const std::string name = text ? std::string(text) : std::string();
		finish_editor(true);
		if (object != invalid_object && !name.empty())
			(void)rename_node(object, name);
	}

	void outliner_view::on_edit_event_thunk(vgui_widget* widget,
		const vgui_event* event, void* user)
	{
		(void)widget;
		auto* self = static_cast<outliner_view*>(user);
		if (!self || !event)
			return;
		if (event->type == VGUI_EVENT_SUBMIT)
			self->commit_rename();
		else if (event->type == VGUI_EVENT_CANCEL)
			self->finish_editor(true);
	}

	void outliner_view::on_focus_change_thunk(vgui_context* ctx,
		vgui_widget* old_focus, vgui_widget* new_focus, void* user)
	{
		(void)new_focus;
		auto* entry = static_cast<context_entry*>(user);
		if (!entry || entry->alive.expired() || !ctx)
			return;
		outliner_view* self = entry->owner;
		/* 失焦确认:编辑框焦点被别处抢走(点击树外/其他面板)时提交。 */
		if (self->renaming_ && old_focus == self->edit_)
			self->commit_rename();
	}

	//----------------------------------------------------------------------------
	// O03 drag & drop
	//----------------------------------------------------------------------------

	void outliner_view::enable_drag_drop()
	{
		if (!tree_)
			return;

		set_editor_row_drag(tree_, true,
			[this](vgui_item_id item, int index, drag_payload& out) -> bool {
				(void)index;
				if (!tree_ || item == 0)
					return false;
				/* 多选时拖动整组选中行(视觉顺序,逗号拼接)。 */
				std::vector<object_id> objects;
				if (multi_select_ && vgui_tree_is_selected(tree_, item) &&
					vgui_tree_get_selected_count(tree_) > 1)
					objects = selected_objects_in_order();
				if (objects.empty())
				{
					const object_id object = object_for_item(item);
					if (object == invalid_object)
						return false;
					objects.push_back(object);
				}
				out.kind = drag_payload_kind::scene_object;
				std::string joined;
				for (object_id object : objects)
				{
					if (!joined.empty())
						joined += ',';
					joined += std::to_string(object);
				}
				out.id = std::move(joined);
				return true;
			});
		if (!drop_target_)
			drop_target_ = std::make_unique<outliner_drop_target>(this);
		set_editor_drop_target(tree_, drop_target_.get());
	}

	void outliner_view::set_multi_select(bool enabled)
	{
		if (!tree_)
			return;
		multi_select_ = enabled;
		vgui_tree_set_selection_mode(tree_, enabled
			? VGUI_SELECTION_MULTIPLE : VGUI_SELECTION_SINGLE);
	}

	std::vector<object_id> outliner_view::selected_objects_in_order() const
	{
		std::vector<object_id> out;
		if (!tree_)
			return out;
		if (virtual_)
		{
			ensure_vrows();
			for (const vrow& row : vrows_)
				if (vgui_tree_is_selected(tree_, static_cast<vgui_item_id>(row.object)))
					out.push_back(row.object);
			return out;
		}
		/* DFS 保证视觉顺序。 */
		std::vector<vgui_item_id> stack;
		for (int i = vgui_tree_get_root_count(tree_) - 1; i >= 0; --i)
			stack.push_back(vgui_tree_get_root(tree_, i));
		while (!stack.empty())
		{
			const vgui_item_id item = stack.back();
			stack.pop_back();
			if (vgui_tree_is_selected(tree_, item))
				out.push_back(object_for_item(item));
			for (int i = vgui_tree_get_child_count(tree_, item) - 1; i >= 0; --i)
				stack.push_back(vgui_tree_get_child(tree_, item, i));
		}
		return out;
	}

	std::vector<object_id> outliner_view::parse_payload_ids(const std::string& id) const
	{
		std::vector<object_id> out;
		for (const std::string& part : split_ids(id))
		{
			try
			{
				out.push_back(static_cast<object_id>(std::stoull(part)));
			}
			catch (...)
			{
			}
		}
		return out;
	}

	std::vector<object_id> outliner_view::ancestor_chain(object_id object) const
	{
		std::vector<object_id> chain;
		if (object == invalid_object)
			return chain;
		if (virtual_)
		{
			if (source_.parent_of)
			{
				object_id walk = source_.parent_of(object);
				while (walk != invalid_object && chain.size() < 100000)
				{
					chain.push_back(walk);
					walk = source_.parent_of(walk);
				}
				return chain;
			}
			/* 无 parent_of:DFS 找路径。 */
			std::vector<object_id> path;
			if (find_virtual_path(object, path))
				chain = path;	/* path 不含自身 */
			return chain;
		}
		if (!tree_)
			return chain;
		const vgui_item_id item = item_for_object(object);
		vgui_item_id walk = item != 0 ? vgui_tree_get_parent(tree_, item) : 0;
		while (walk != 0)
		{
			chain.push_back(object_for_item(walk));
			walk = vgui_tree_get_parent(tree_, walk);
		}
		return chain;
	}

	bool outliner_view::would_cycle(const std::vector<object_id>& nodes,
		object_id target_parent) const
	{
		if (target_parent == invalid_object)
			return false;
		for (object_id node : nodes)
		{
			if (node == target_parent)
				return true;
			for (object_id ancestor : ancestor_chain(target_parent))
				if (ancestor == node)
					return true;
		}
		return false;
	}

	outliner_view::drop_location outliner_view::drop_location_at(float x,
		float y) const
	{
		drop_location location;
		if (!tree_)
			return location;

		float wx = 0.0f, wy = 0.0f, ww = 0.0f, wh = 0.0f;
		vgui_widget_get_rect(tree_, &wx, &wy, &ww, &wh);
		const vgui_item_id item = vgui_tree_item_at_point(tree_, x - wx, y - wy);
		if (item == 0)
		{
			location.kind = drop_kind::roots;
			location.parent = invalid_object;
			location.index = virtual_
				? (source_.root_count ? source_.root_count() : 0)
				: static_cast<std::size_t>(vgui_tree_get_root_count(tree_));
			return location;
		}

		float rx = 0, ry = 0, rw = 0, rh = 0;
		if (!vgui_tree_get_item_rect(tree_, item, &rx, &ry, &rw, &rh) || rh <= 0)
			return location;
		const float ratio = (y - ry) / rh;

		const node_location row_location = location_of(item);
		if (!row_location.valid)
			return location;
		location.row = item;
		if (ratio < 0.25f)
		{
			location.kind = drop_kind::before;
			location.parent = row_location.parent;
			location.index = row_location.index;
		}
		else if (ratio > 0.75f)
		{
			location.kind = drop_kind::after;
			location.parent = row_location.parent;
			location.index = row_location.index + 1;
		}
		else
		{
			location.kind = drop_kind::onto;
			location.parent = object_for_item(item);
			location.index = child_count_of(location.parent, item);
		}
		return location;
	}

	bool outliner_view::row_drop_location(float x, float y,
		object_id* out_parent, std::size_t* out_index) const
	{
		if (!out_parent || !out_index)
			return false;
		const drop_location location = drop_location_at(x, y);
		if (location.kind == drop_kind::none)
			return false;
		*out_parent = location.parent;
		*out_index = location.index;
		return true;
	}

	bool outliner_view::apply_drop_by_payload(const std::string& payload_id,
		float x, float y)
	{
		const std::vector<object_id> nodes = parse_payload_ids(payload_id);
		if (nodes.empty())
			return false;
		const drop_location location = drop_location_at(x, y);
		if (location.kind == drop_kind::none)
			return false;
		return apply_drop(nodes, location);
	}

	bool outliner_view::apply_drop(const std::vector<object_id>& nodes,
		const drop_location& location)
	{
		if (!tree_ || !move_handler_ || nodes.empty())
			return false;
		if (would_cycle(nodes, location.parent))
			return false;

		/* 旧位置按拖动前状态捕获,多选顺序应用并按公式修正索引:
		 * base_k = final + k - #{j<=k: 旧父==目标父 && 旧序 < final}。 */
		std::vector<node_location> old_locations;
		old_locations.reserve(nodes.size());
		for (object_id node : nodes)
		{
			const vgui_item_id item = virtual_
				? static_cast<vgui_item_id>(node)
				: item_for_object(node);
			old_locations.push_back(location_of(item));
		}

		const bool transaction = nodes.size() > 1 && command_stack_ &&
			!command_stack_->in_transaction();
		if (transaction)
			command_stack_->begin_transaction("Move nodes");

		bool any = false;
		for (std::size_t k = 0; k < nodes.size(); ++k)
		{
			/* base_k = final + k - #{j<=k: 旧父==目标父 && 旧序 < final} */
			std::size_t removed = 0;
			for (std::size_t j = 0; j <= k; ++j)
			{
				if (old_locations[j].valid &&
					old_locations[j].parent == location.parent &&
					old_locations[j].index < location.index)
					++removed;
			}
			const std::size_t adjusted = location.index + k
				- std::min(removed, location.index + k);
			any = move_node(nodes[k], location.parent, adjusted) || any;
		}

		if (transaction)
			command_stack_->commit_transaction();
		return any;
	}

	void outliner_view::emit_hover_hint(const drop_location& location,
		vgui_drop_hint* hint) const
	{
		if (!hint || !tree_)
			return;
		hint->kind = VGUI_DROP_HINT_NONE;
		if (location.kind == drop_kind::none)
			return;

		float rx = 0, ry = 0, rw = 0, rh = 0;
		if (location.row != 0 &&
			vgui_tree_get_item_rect(tree_, location.row, &rx, &ry, &rw, &rh))
		{
			if (location.kind == drop_kind::before)
			{
				hint->kind = VGUI_DROP_HINT_LINE;
				hint->x = rx;
				hint->y = ry - 1.0f;
				hint->w = rw;
				hint->h = 2.0f;
			}
			else if (location.kind == drop_kind::after)
			{
				hint->kind = VGUI_DROP_HINT_LINE;
				hint->x = rx;
				hint->y = ry + rh - 1.0f;
				hint->w = rw;
				hint->h = 2.0f;
			}
			else
			{
				hint->kind = VGUI_DROP_HINT_RECT;
				hint->x = rx;
				hint->y = ry;
				hint->w = rw;
				hint->h = rh;
			}
			return;
		}
		if (location.kind == drop_kind::roots)
		{
			/* 追加到根:内容底部画插入线。 */
			float wx = 0, wy = 0, ww = 0, wh = 0;
			vgui_widget_get_rect(tree_, &wx, &wy, &ww, &wh);
			hint->kind = VGUI_DROP_HINT_LINE;
			hint->x = wx;
			hint->w = ww;
			hint->h = 2.0f;
			hint->y = wy + wh - 2.0f;
		}
	}

	//----------------------------------------------------------------------------
	// O04 filter & selection sync
	//----------------------------------------------------------------------------

	bool outliner_view::filter_active() const
	{
		return !filter_text_.empty() || filter_match_ != nullptr;
	}

	bool outliner_view::match_object(object_id object, const char* name) const
	{
		if (!filter_text_.empty() && !contains_ci(name ? name : "", filter_text_))
			return false;
		if (filter_match_)
			return filter_match_(object, name ? name : "");
		return true;
	}

	int outliner_view::filter_thunk(vgui_widget* tree, vgui_item_id node,
		void* user)
	{
		auto* self = static_cast<outliner_view*>(user);
		if (!self || !tree)
			return 0;
		const char* text = vgui_tree_get_item_text(tree, node);
		return self->match_object(self->object_for_item(node), text) ? 1 : 0;
	}

	void outliner_view::set_filter(const char* text)
	{
		filter_text_ = text ? text : "";
		if (!tree_)
			return;
		if (virtual_)
		{
			invalidate_vrows();
			vgui_tree_source_changed(tree_);
			return;
		}
		vgui_tree_set_match_text(tree_, filter_text_.empty() ? nullptr
			: filter_text_.c_str());
		vgui_tree_set_filter(tree_, filter_active()
			? &outliner_view::filter_thunk : nullptr, this);
	}

	void outliner_view::set_filter_match_handler(filter_match_handler handler)
	{
		filter_match_ = std::move(handler);
		if (!tree_ || virtual_)
		{
			if (tree_ && virtual_)
			{
				invalidate_vrows();
				vgui_tree_source_changed(tree_);
			}
			return;
		}
		/* 纯 handler 过滤(空文本)也要挂过滤回调。 */
		vgui_tree_set_filter(tree_, filter_active()
			? &outliner_view::filter_thunk : nullptr, this);
	}

	void outliner_view::enable_selection_sync()
	{
		if (!context_ || selection_sync_id_)
			return;
		selection_sync_id_ = context_->add_selection_listener(
			[this](object_id object) { on_context_selection(object); });
	}

	void outliner_view::on_context_selection(object_id object)
	{
		if (!tree_ || syncing_selection_)
			return;
		if (object == invalid_object)
		{
			vgui_tree_clear_selection(tree_);
			return;
		}
		vgui_item_id item = 0;
		if (virtual_)
		{
			ensure_vrows();
			if (vrow_index_.find(object) == vrow_index_.end())
			{
				/* 折叠/过滤掉了:展开祖先链再投影。 */
				std::vector<object_id> path;
				if (!find_virtual_path(object, path))
					return;
				for (object_id ancestor : path)
					expanded_.insert(ancestor);
				invalidate_vrows();
				ensure_vrows();
				vgui_tree_source_changed(tree_);
			}
			if (vrow_index_.find(object) == vrow_index_.end())
				return;	/* 被过滤隐藏 */
			item = static_cast<vgui_item_id>(object);
		}
		else
		{
			item = item_for_object(object);
			if (item == 0)
				return;
			vgui_tree_expand_to(tree_, item);
		}
		vgui_tree_clear_selection(tree_);
		vgui_tree_select(tree_, item, 1);
		vgui_tree_set_cursor(tree_, item);
		vgui_tree_scroll_to_node(tree_, item, VGUI_SCROLL_ALIGN_NEAREST);
	}

	//----------------------------------------------------------------------------
	// O05 virtual mode
	//----------------------------------------------------------------------------

	void outliner_view::invalidate_vrows()
	{
		vrows_dirty_ = true;
	}

	bool outliner_view::find_virtual_path(object_id target,
		std::vector<object_id>& path) const
	{
		/* DFS 收集 root→target 路径(不含 target)。 */
		std::vector<std::pair<object_id, std::vector<object_id>>> stack;
		if (!source_.root_count || !source_.root_at || !source_.child_count ||
			!source_.child_at)
			return false;
		const std::size_t roots = source_.root_count();
		for (std::size_t i = roots; i > 0; --i)
		{
			const object_id root = source_.root_at(i - 1);
			if (root == invalid_object)
				continue;
			stack.push_back({ root, {} });
		}
		while (!stack.empty())
		{
			auto entry = stack.back();
			stack.pop_back();
			if (entry.first == target)
			{
				path = entry.second;
				return true;
			}
			const std::size_t count = source_.child_count(entry.first);
			for (std::size_t i = count; i > 0; --i)
			{
				const object_id child = source_.child_at(entry.first, i - 1);
				if (child == invalid_object)
					continue;
				if (entry.second.size() > 128)
					continue;	/* 深度保险 */
				std::vector<object_id> child_path = entry.second;
				child_path.push_back(entry.first);
				stack.push_back({ child, std::move(child_path) });
			}
		}
		return false;
	}

	void outliner_view::collect_virtual(object_id object, object_id parent,
		int depth, std::size_t index,
		const std::unordered_set<object_id>* only, std::vector<vrow>& out) const
	{
		if (object == invalid_object)
			return;
		if (only && only->find(object) == only->end())
			return;
		out.push_back({ object, parent, depth, index });
		/* 过滤模式(only 非空):闭包之外的都不发射,命中后代自然在集合里,
		 * 深度照常递增;非过滤模式按展开状态走。 */
		if (!only && expanded_.find(object) == expanded_.end())
			return;
		const std::size_t count = source_.child_count
			? source_.child_count(object) : 0;
		for (std::size_t i = 0; i < count; ++i)
			collect_virtual(source_.child_at(object, i), object, depth + 1, i,
				only, out);
	}

	void outliner_view::ensure_vrows() const
	{
		if (!vrows_dirty_)
			return;
		vrows_.clear();
		vrow_index_.clear();
		vrows_dirty_ = false;
		if (!source_.root_count || !source_.root_at)
			return;

		if (filter_active())
		{
			/* 两遍:先收集命中闭包(命中 + 祖先),再先序发射。 */
			std::unordered_set<object_id> matched;
			std::function<bool(object_id)> mark = [&](object_id object) -> bool
			{
				if (object == invalid_object)
					return false;
				const std::string name = source_.name ? source_.name(object)
					: std::string();
				bool any = match_object(object, name.c_str());
				const std::size_t count = source_.child_count
					? source_.child_count(object) : 0;
				for (std::size_t i = 0; i < count; ++i)
				{
					if (mark(source_.child_at(object, i)))
						any = true;
				}
				if (any)
					matched.insert(object);
				return any;
			};
			const std::size_t roots = source_.root_count();
			for (std::size_t i = 0; i < roots; ++i)
				mark(source_.root_at(i));
			for (std::size_t i = 0; i < roots; ++i)
				collect_virtual(source_.root_at(i), invalid_object, 0, i, &matched,
					vrows_);
		}
		else
		{
			const std::size_t roots = source_.root_count();
			for (std::size_t i = 0; i < roots; ++i)
				collect_virtual(source_.root_at(i), invalid_object, 0, i, nullptr,
					vrows_);
		}
		for (std::size_t i = 0; i < vrows_.size(); ++i)
			vrow_index_[vrows_[i].object] = static_cast<int>(i);
	}

	std::size_t outliner_view::projected_row_count() const
	{
		if (!virtual_)
			return 0;
		ensure_vrows();
		return vrows_.size();
	}

	void outliner_view::set_scene_source(scene_source source)
	{
		if (!tree_)
			return;
		source_ = std::move(source);
		virtual_ = source_.root_count && source_.root_at && source_.child_count &&
			source_.child_at && source_.name;
		if (!virtual_)
			return;

		vgui_row_source row_source = {};
		row_source.user = this;
		row_source.row_count = &outliner_view::vs_row_count;
		row_source.row_info = &outliner_view::vs_row_info;
		row_source.row_id = &outliner_view::vs_row_id;
		row_source.row_id_index = &outliner_view::vs_row_id_index;
		row_source.row_depth = &outliner_view::vs_row_depth;
		row_source.row_has_children = &outliner_view::vs_row_has_children;
		row_source.row_expanded = &outliner_view::vs_row_expanded;
		row_source.row_toggle = &outliner_view::vs_row_toggle;
		row_source.row_enabled = &outliner_view::vs_row_enabled;
		row_source.row_tooltip = &outliner_view::vs_row_tooltip;
		row_source.row_icon = &outliner_view::vs_row_icon;
		row_source.row_text_color = &outliner_view::vs_row_text_color;
		row_source.row_buttons = &outliner_view::vs_row_buttons;
		row_source.row_button_icon = &outliner_view::vs_row_button_icon;
		row_source.row_button_click = &outliner_view::vs_row_button_click;
		row_source.row_draw = &outliner_view::vs_row_draw;
		vgui_tree_set_row_source(tree_, &row_source);
		selected_ = 0;
		invalidate_vrows();
		vgui_tree_source_changed(tree_);
	}

	void outliner_view::scene_changed()
	{
		if (!tree_ || !virtual_)
			return;
		invalidate_vrows();
		vgui_tree_source_changed(tree_);
	}

	void outliner_view::scene_row_changed(object_id object)
	{
		if (!tree_ || !virtual_ || object == invalid_object)
			return;
		ensure_vrows();
		const auto it = vrow_index_.find(object);
		if (it != vrow_index_.end())
			vgui_tree_source_rows_changed(tree_, it->second, 1);
	}

	int outliner_view::vs_row_count(void* user)
	{
		auto* self = static_cast<outliner_view*>(user);
		if (!self)
			return 0;
		self->ensure_vrows();
		return static_cast<int>(self->vrows_.size());
	}

	int outliner_view::vs_row_info(void* user, int index, const char** text,
		const char** detail, void** item_user)
	{
		auto* self = static_cast<outliner_view*>(user);
		if (!self || index < 0 || (std::size_t)index >= self->vrows_.size())
			return 0;
		self->ensure_vrows();
		const vrow& row = self->vrows_[(std::size_t)index];
		self->vs_scratch_text_ = self->source_.name
			? self->source_.name(row.object) : std::string();
		if (text)
			*text = self->vs_scratch_text_.c_str();
		if (detail)
			*detail = nullptr;
		if (item_user)
			*item_user = reinterpret_cast<void*>(
				static_cast<std::uintptr_t>(row.object));
		return 1;
	}

	vgui_item_id outliner_view::vs_row_id(void* user, int index)
	{
		auto* self = static_cast<outliner_view*>(user);
		if (!self || index < 0 || (std::size_t)index >= self->vrows_.size())
			return 0;
		self->ensure_vrows();
		return static_cast<vgui_item_id>(self->vrows_[(std::size_t)index].object);
	}

	int outliner_view::vs_row_id_index(void* user, vgui_item_id item)
	{
		auto* self = static_cast<outliner_view*>(user);
		if (!self)
			return -1;
		self->ensure_vrows();
		const auto it = self->vrow_index_.find(static_cast<object_id>(item));
		return it == self->vrow_index_.end() ? -1 : it->second;
	}

	int outliner_view::vs_row_depth(void* user, int index)
	{
		auto* self = static_cast<outliner_view*>(user);
		if (!self || index < 0 || (std::size_t)index >= self->vrows_.size())
			return 0;
		self->ensure_vrows();
		return self->vrows_[(std::size_t)index].depth;
	}

	int outliner_view::vs_row_has_children(void* user, int index)
	{
		auto* self = static_cast<outliner_view*>(user);
		if (!self || index < 0 || (std::size_t)index >= self->vrows_.size())
			return 0;
		self->ensure_vrows();
		const object_id object = self->vrows_[(std::size_t)index].object;
		return self->source_.child_count &&
			self->source_.child_count(object) > 0 ? 1 : 0;
	}

	int outliner_view::vs_row_expanded(void* user, int index)
	{
		auto* self = static_cast<outliner_view*>(user);
		if (!self || index < 0 || (std::size_t)index >= self->vrows_.size())
			return 0;
		self->ensure_vrows();
		return self->expanded_.find(self->vrows_[(std::size_t)index].object)
			!= self->expanded_.end() ? 1 : 0;
	}

	void outliner_view::vs_row_toggle(void* user, int index)
	{
		auto* self = static_cast<outliner_view*>(user);
		if (!self || index < 0 || (std::size_t)index >= self->vrows_.size())
			return;
		self->ensure_vrows();
		const object_id object = self->vrows_[(std::size_t)index].object;
		if (self->expanded_.find(object) != self->expanded_.end())
			self->expanded_.erase(object);
		else
			self->expanded_.insert(object);
		self->invalidate_vrows();
		vgui_tree_source_changed(self->tree_);
	}

	int outliner_view::vs_row_enabled(void* user, int index)
	{
		auto* self = static_cast<outliner_view*>(user);
		if (!self || index < 0 || (std::size_t)index >= self->vrows_.size())
			return 1;
		self->ensure_vrows();
		return self->source_.enabled
			? (self->source_.enabled(self->vrows_[(std::size_t)index].object)
				? 1 : 0) : 1;
	}

	const char* outliner_view::vs_row_tooltip(void* user, int index)
	{
		auto* self = static_cast<outliner_view*>(user);
		if (!self || index < 0 || (std::size_t)index >= self->vrows_.size())
			return nullptr;
		self->ensure_vrows();
		return self->source_.tooltip
			? self->source_.tooltip(self->vrows_[(std::size_t)index].object)
			: nullptr;
	}

	int outliner_view::vs_row_icon(void* user, int index, vgui_texture** out_texture)
	{
		auto* self = static_cast<outliner_view*>(user);
		if (!self || !out_texture || index < 0 ||
			(std::size_t)index >= self->vrows_.size())
			return 0;
		self->ensure_vrows();
		if (!self->source_.icon)
			return 0;
		*out_texture = self->source_.icon(self->vrows_[(std::size_t)index].object);
		return *out_texture != nullptr ? 1 : 0;
	}

	vgui_color outliner_view::vs_row_text_color(void* user, int index)
	{
		auto* self = static_cast<outliner_view*>(user);
		if (!self || !self->source_.text_color || index < 0 ||
			(std::size_t)index >= self->vrows_.size())
			return 0;
		self->ensure_vrows();
		return self->source_.text_color(self->vrows_[(std::size_t)index].object);
	}

	int outliner_view::vs_row_buttons(void* user, int index)
	{
		auto* self = static_cast<outliner_view*>(user);
		(void)index;
		return self && self->row_buttons_ ? 2 : 0;
	}

	int outliner_view::vs_row_button_icon(void* user, int index, int button,
		vgui_texture** out_texture)
	{
		auto* self = static_cast<outliner_view*>(user);
		if (!self || !out_texture || index < 0 ||
			(std::size_t)index >= self->vrows_.size())
			return 0;
		self->ensure_vrows();
		const object_id object = self->vrows_[(std::size_t)index].object;
		if (button == 0)
			*out_texture = self->object_visibility(object)
				? self->vis_icon_on_ : self->vis_icon_off_;
		else if (button == 1)
			*out_texture = self->object_locked(object)
				? self->lock_icon_on_ : self->lock_icon_off_;
		else
			return 0;
		return *out_texture != nullptr ? 1 : 0;
	}

	void outliner_view::vs_row_button_click(void* user, int index, int button)
	{
		auto* self = static_cast<outliner_view*>(user);
		if (!self || index < 0 || (std::size_t)index >= self->vrows_.size())
			return;
		self->ensure_vrows();
		self->flag_changed(self->vrows_[(std::size_t)index].object, button);
	}

	void outliner_view::vs_row_draw(void* user, int index, ovg_ctx_cb* cb,
		rvg_t* vg, float x, float y, float w, float h)
	{
		auto* self = static_cast<outliner_view*>(user);
		if (!self || !self->source_.row_draw || index < 0 ||
			(std::size_t)index >= self->vrows_.size())
			return;
		self->ensure_vrows();
		self->source_.row_draw(self->vrows_[(std::size_t)index].object, cb, vg,
			x, y, w, h);
	}

	//----------------------------------------------------------------------------
	// O06 context menu & shortcuts
	//----------------------------------------------------------------------------

	void outliner_view::set_action_handler(action_handler handler)
	{
		action_handler_ = std::move(handler);
	}

	void outliner_view::ensure_key_handler()
	{
		if (!context_ || key_entry_)
			return;
		key_entry_ = new context_entry{ alive_token_, this };
		(void)vgui_context_add_key_handler(context_->ui(),
			&outliner_view::on_key_thunk, key_entry_);
	}

	void outliner_view::ensure_tree_event_handler()
	{
		if (!tree_ || tree_event_installed_)
			return;
		tree_event_installed_ = true;
		vgui_widget_on_event(tree_, &outliner_view::on_tree_event_thunk, this);
	}

	void outliner_view::enable_context_menu()
	{
		if (!tree_ || !context_)
			return;
		context_menu_ = true;
		if (!menu_)
		{
			menu_ = vgui_widget_create(context_->ui(), VGUI_MENU, nullptr);
			vgui_menu_on_activate(menu_, &outliner_view::on_menu_activate_thunk, this);
		}
		ensure_tree_event_handler();
		ensure_key_handler();
	}

	void outliner_view::on_tree_event_thunk(vgui_widget* widget,
		const vgui_event* event, void* user)
	{
		(void)widget;
		auto* self = static_cast<outliner_view*>(user);
		if (!self || !event)
			return;
		self->on_tree_event(event);
	}

	void outliner_view::on_tree_event(const vgui_event* event)
	{
		if (event->type == VGUI_EVENT_DOUBLE_CLICK && inline_rename_ &&
			event->button == 1 && event->item != 0 && !renaming_)
		{
			begin_rename(event->item);
			return;
		}
		if (event->type == VGUI_EVENT_MOUSE_DOWN && event->button == 3 &&
			context_menu_ && event->item != 0)
		{
			/* 右键行:不在选区内则先独占选中,菜单作用于选中项。 */
			if (!vgui_tree_is_selected(tree_, event->item))
				select_solo(event->item, object_for_item(event->item));
			float wx = 0, wy = 0, ww = 0, wh = 0;
			vgui_widget_get_rect(tree_, &wx, &wy, &ww, &wh);
			(void)ww; (void)wh;
			open_context_menu(wx + event->x, wy + event->y);
		}
	}

	void outliner_view::select_solo(vgui_item_id item, object_id object)
	{
		if (!tree_ || item == 0)
			return;
		(void)object;
		vgui_tree_clear_selection(tree_);
		vgui_tree_select(tree_, item, 1);
		vgui_tree_set_cursor(tree_, item);
	}

	void outliner_view::open_context_menu(float abs_x, float abs_y)
	{
		if (!menu_ || !tree_)
			return;
		const std::vector<object_id> nodes = selected_objects_in_order();
		if (nodes.empty())
			return;
		const object_id primary = nodes.front();

		const auto add_plain = [this](const char* text, action kind,
			const char* shortcut)
		{
			const vgui_item_id item = vgui_menu_add_item(menu_, text);
			if (item != 0)
			{
				vgui_menu_set_item_user(menu_, item,
					reinterpret_cast<void*>(static_cast<std::uintptr_t>((int)kind)));
				if (shortcut)
					vgui_menu_set_item_shortcut(menu_, item, shortcut);
			}
		};
		const auto add_check = [this](const char* text, action kind, int checked)
		{
			const vgui_item_id item = vgui_menu_add_check_item(menu_, text, checked);
			if (item != 0)
				vgui_menu_set_item_user(menu_, item,
					reinterpret_cast<void*>(static_cast<std::uintptr_t>((int)kind)));
		};

		vgui_menu_clear(menu_);
		add_plain("New Child", action::new_child, nullptr);
		vgui_menu_add_separator(menu_);
		add_plain("Rename", action::rename, "F2");
		add_plain("Duplicate", action::duplicate, "Ctrl+D");
		add_plain("Delete", action::remove, "Del");
		vgui_menu_add_separator(menu_);
		add_plain("Focus", action::focus, nullptr);
		add_check("Visible", action::toggle_visibility,
			object_visibility(primary) ? 1 : 0);
		add_check("Locked", action::toggle_lock,
			object_locked(primary) ? 1 : 0);
		vgui_menu_open(menu_, abs_x, abs_y);
	}

	void outliner_view::dispatch_action(action kind)
	{
		const std::vector<object_id> nodes = selected_objects_in_order();
		if (nodes.empty())
			return;
		switch (kind)
		{
		case action::rename:
			if (inline_rename_)
			{
				const vgui_item_id item = virtual_
					? static_cast<vgui_item_id>(nodes.front())
					: item_for_object(nodes.front());
				begin_rename(item);
			}
			break;
		case action::toggle_visibility:
			for (object_id node : nodes)
				toggle_object_visibility(node);
			break;
		case action::toggle_lock:
			for (object_id node : nodes)
				toggle_object_lock(node);
			break;
		default:
			if (action_handler_)
				(void)action_handler_(kind, nodes);
			break;
		}
	}

	void outliner_view::on_menu_activate_thunk(vgui_widget* menu,
		vgui_item_id item, void* user)
	{
		auto* self = static_cast<outliner_view*>(user);
		if (!self || !menu || item == 0)
			return;
		const auto action_value = reinterpret_cast<std::uintptr_t>(
			vgui_menu_get_item_user(menu, item));
		self->dispatch_action(static_cast<action>(action_value));
	}

	int outliner_view::on_key_thunk(vgui_context* ctx, int key, int down,
		int mods, void* user)
	{
		auto* entry = static_cast<context_entry*>(user);
		if (!entry || entry->alive.expired() || !ctx)
			return 0;
		outliner_view* self = entry->owner;
		if (!down || !self->tree_)
			return 0;
		/* 树持有焦点时才响应,避免抢占其他面板的快捷键。 */
		if (vgui_get_focus(ctx) != self->tree_)
			return 0;
		if (key == VGUI_KEY_F2 && self->inline_rename_)
		{
			self->dispatch_action(action::rename);
			return 1;
		}
		if (key == VGUI_KEY_DELETE)
		{
			self->dispatch_action(action::remove);
			return 1;
		}
		if (key == 'D' && (mods & VGUI_MOD_CTRL))
		{
			self->dispatch_action(action::duplicate);
			return 1;
		}
		return 0;
	}

	//----------------------------------------------------------------------------
	// selection plumbing
	//----------------------------------------------------------------------------

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
		{
			/* 自己点击推给 context;同步监听里不再回滚滚动。 */
			self->syncing_selection_ = true;
			self->context_->set_selection(object);
			self->syncing_selection_ = false;
		}
		if (self->listener_)
			self->listener_(object);
	}

} /* namespace vgui_editor_kit */
