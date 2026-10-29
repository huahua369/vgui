#include <vgui/editor/combo_box.h>

#include <utility>

namespace vgui_editor_kit {

	namespace {
		const std::string& empty_string()
		{
			static const std::string value;
			return value;
		}
	}

	combo_box::~combo_box()
	{
		if (!menu_)
			return;
		vgui_widget* menu = menu_;
		menu_ = nullptr;
		vgui_context* ctx = vgui_widget_get_context(menu);
		if (!ctx)
			return;
		if (vgui_menu_is_open(menu))
			vgui_menu_close(menu);
		vgui_widget_destroy(menu);
	}

	void combo_box::build(vgui_widget* parent)
	{
		if (button_ || !parent)
			return;

		button_ = vgui_widget_add(parent, VGUI_BUTTON, nullptr);
		if (!button_)
			return;
		vgui_widget_set_a11y_role(button_, VGUI_A11Y_ROLE_COMBOBOX);
		vgui_widget_set_size(button_, 180, 24);
		vgui_widget_set_min_size(button_, 80, 24);
		vgui_widget_set_font_size(button_, 13);
		vgui_widget_set_text_align(button_, 0.0f, 0.5f);
		vgui_widget_on_event(button_, &combo_box::on_button_event, this);

		apply_ids();
		sync_display();
		sync_enabled();
	}

	void combo_box::set_id(std::string id)
	{
		if (id.empty())
			return;
		id_ = std::move(id);
		apply_ids();
	}

	const std::string& combo_box::id() const noexcept
	{
		return id_;
	}

	void combo_box::apply_ids()
	{
		if (button_)
			vgui_widget_set_id(button_, id_.c_str());
		if (menu_)
			vgui_widget_set_id(menu_, (id_ + "_menu").c_str());
	}

	void combo_box::set_items(std::vector<item> items)
	{
		const bool had_selection =
			selected_ >= 0 && selected_ < (int)items_.size();
		const std::string old_value = had_selection
			? items_[(std::size_t)selected_].value : std::string();

		items_ = std::move(items);
		if (had_selection)
			selected_ = index_of_value(old_value);
		else
			selected_ = -1;

		if (menu_)
			rebuild_menu();
		sync_display();
	}

	const std::vector<combo_box::item>& combo_box::items() const noexcept
	{
		return items_;
	}

	std::size_t combo_box::item_count() const noexcept
	{
		return items_.size();
	}

	const combo_box::item* combo_box::item_at(int index) const noexcept
	{
		if (index < 0 || index >= (int)items_.size())
			return nullptr;
		return &items_[(std::size_t)index];
	}

	int combo_box::index_of_value(const std::string& value) const noexcept
	{
		for (std::size_t i = 0; i < items_.size(); ++i)
		{
			if (items_[i].value == value)
				return (int)i;
		}
		return -1;
	}

	bool combo_box::set_item_enabled(int index, bool enabled)
	{
		if (index < 0 || index >= (int)items_.size())
			return false;
		items_[(std::size_t)index].enabled = enabled;
		if (menu_)
		{
			const vgui_item_id id = menu_item_for_index(index);
			if (id)
				vgui_menu_set_item_enabled(menu_, id, enabled ? 1 : 0);
		}
		return true;
	}

	void combo_box::set_placeholder(std::string text)
	{
		placeholder_ = std::move(text);
		if (menu_)
		{
			for (const auto& entry : menu_items_)
			{
				const int index = entry.second;
				if (index < 0 || index >= (int)items_.size())
					continue;
				if (!items_[(std::size_t)index].label.empty())
					continue;
				vgui_menu_set_item_text(menu_, entry.first,
					placeholder_.c_str());
			}
		}
		sync_display();
	}

	const std::string& combo_box::placeholder() const noexcept
	{
		return placeholder_;
	}

	std::string combo_box::display_text() const
	{
		const item* selected = selected_item();
		if (selected && !selected->label.empty())
			return selected->label;
		return placeholder_;
	}

	bool combo_box::set_selected_index(int index)
	{
		if (index < -1 || index >= (int)items_.size())
			return false;
		if (index == selected_)
			return true;
		apply_selection(index, false);
		return true;
	}

	bool combo_box::set_value(const std::string& value)
	{
		const int index = index_of_value(value);
		if (index < 0)
			return false;
		return set_selected_index(index);
	}

	void combo_box::clear_selection()
	{
		selected_ = -1;
		sync_display();
		sync_menu_checks();
	}

	bool combo_box::select_index(int index)
	{
		if (!enabled_ || read_only_)
			return false;
		if (index < 0 || index >= (int)items_.size())
			return false;
		if (!items_[(std::size_t)index].enabled)
			return false;
		if (index == selected_)
			return true;
		apply_selection(index, true);
		return true;
	}

	bool combo_box::select_value(const std::string& value)
	{
		const int index = index_of_value(value);
		if (index < 0)
			return false;
		return select_index(index);
	}

	bool combo_box::select_none()
	{
		if (!enabled_ || read_only_)
			return false;
		if (selected_ == -1)
			return true;
		apply_selection(-1, true);
		return true;
	}

	int combo_box::selected_index() const noexcept
	{
		return selected_;
	}

	const std::string& combo_box::value() const noexcept
	{
		const item* selected = selected_item();
		return selected ? selected->value : empty_string();
	}

	const combo_box::item* combo_box::selected_item() const noexcept
	{
		if (selected_ < 0 || selected_ >= (int)items_.size())
			return nullptr;
		return &items_[(std::size_t)selected_];
	}

	bool combo_box::open()
	{
		if (!button_ || !enabled_ || read_only_ || items_.empty())
			return false;
		ensure_menu();
		if (!menu_)
			return false;

		sync_menu_checks();
		vgui_menu_open_below(menu_, button_);
		if (vgui_menu_is_open(menu_) && selected_ >= 0)
		{
			const vgui_item_id id = menu_item_for_index(selected_);
			if (id)
				vgui_menu_set_cursor(menu_, id);
		}
		return vgui_menu_is_open(menu_) != 0;
	}

	void combo_box::close()
	{
		if (menu_ && vgui_menu_is_open(menu_))
			vgui_menu_close(menu_);
	}

	bool combo_box::is_open() const
	{
		return menu_ && vgui_menu_is_open(menu_) != 0;
	}

	void combo_box::toggle()
	{
		if (is_open())
			close();
		else
			open();
	}

	void combo_box::set_enabled(bool enabled)
	{
		if (enabled_ == enabled)
			return;
		enabled_ = enabled;
		sync_enabled();
	}

	bool combo_box::enabled() const noexcept
	{
		return enabled_;
	}

	void combo_box::set_read_only(bool read_only)
	{
		if (read_only_ == read_only)
			return;
		read_only_ = read_only;
		sync_enabled();
	}

	bool combo_box::read_only() const noexcept
	{
		return read_only_;
	}

	void combo_box::focus()
	{
		if (!button_ || !enabled_ || read_only_)
			return;
		vgui_context* ctx = vgui_widget_get_context(button_);
		if (ctx)
			vgui_set_focus(ctx, button_);
	}

	void combo_box::set_on_change(selection_listener listener)
	{
		change_listener_ = std::move(listener);
	}

	void combo_box::set_on_commit(selection_listener listener)
	{
		commit_listener_ = std::move(listener);
	}

	vgui_widget* combo_box::widget() const noexcept
	{
		return button_;
	}

	void combo_box::sync_display()
	{
		if (!button_)
			return;
		const std::string text = display_text();
		const char* current = vgui_widget_get_text(button_);
		if (!current || text != current)
			vgui_widget_set_text(button_, text.c_str());
	}

	void combo_box::sync_enabled()
	{
		if (button_)
			vgui_widget_set_enabled(button_,
				(enabled_ && !read_only_) ? 1 : 0);
		if (!enabled_ || read_only_)
			close();
	}

	void combo_box::ensure_menu()
	{
		if (menu_ || !button_)
			return;
		vgui_context* ctx = vgui_widget_get_context(button_);
		if (!ctx)
			return;
		menu_ = vgui_widget_create(ctx, VGUI_MENU, nullptr);
		if (!menu_)
			return;
		vgui_widget_set_id(menu_, (id_ + "_menu").c_str());
		vgui_menu_on_activate(menu_, &combo_box::on_menu_activate, this);
		vgui_menu_set_close_on_activate(menu_, 1);
		rebuild_menu();
	}

	void combo_box::rebuild_menu()
	{
		if (!menu_)
			return;
		if (vgui_menu_is_open(menu_))
			vgui_menu_close(menu_);
		vgui_menu_clear(menu_);
		menu_items_.clear();

		for (std::size_t i = 0; i < items_.size(); ++i)
		{
			const item& entry = items_[i];
			const char* label = entry.label.empty()
				? placeholder_.c_str() : entry.label.c_str();
			const vgui_item_id id = vgui_menu_add_item(menu_, label);
			if (!id)
				continue;
			menu_items_.push_back({ id, (int)i });
			if (!entry.enabled)
				vgui_menu_set_item_enabled(menu_, id, 0);
			if ((int)i == selected_)
				vgui_menu_set_item_checked(menu_, id, 1);
		}
	}

	void combo_box::sync_menu_checks()
	{
		if (!menu_)
			return;
		for (const auto& entry : menu_items_)
			vgui_menu_set_item_checked(menu_, entry.first,
				entry.second == selected_ ? 1 : 0);
	}

	bool combo_box::apply_selection(int index, bool notify)
	{
		if (index < -1 || index >= (int)items_.size())
			return false;
		if (index == selected_)
			return false;

		selected_ = index;
		sync_display();
		sync_menu_checks();
		if (notify)
		{
			notify_change();
			notify_commit();
		}
		return true;
	}

	vgui_item_id combo_box::menu_item_for_index(int index) const
	{
		for (const auto& entry : menu_items_)
		{
			if (entry.second == index)
				return entry.first;
		}
		return 0;
	}

	int combo_box::next_enabled_index(int from, int direction) const
	{
		const int count = (int)items_.size();
		if (count == 0 || direction == 0)
			return -1;
		int current = from;
		for (int step = 0; step < count; ++step)
		{
			current += direction > 0 ? 1 : -1;
			if (current < 0)
				current = count - 1;
			else if (current >= count)
				current = 0;
			if (items_[(std::size_t)current].enabled)
				return current;
		}
		return -1;
	}

	int combo_box::first_enabled_index() const
	{
		for (std::size_t i = 0; i < items_.size(); ++i)
		{
			if (items_[i].enabled)
				return (int)i;
		}
		return -1;
	}

	int combo_box::last_enabled_index() const
	{
		for (int i = (int)items_.size() - 1; i >= 0; --i)
		{
			if (items_[(std::size_t)i].enabled)
				return i;
		}
		return -1;
	}

	void combo_box::notify_change()
	{
		if (change_listener_)
			change_listener_(selected_, value());
	}

	void combo_box::notify_commit()
	{
		if (commit_listener_)
			commit_listener_(selected_, value());
	}

	void combo_box::on_button_event(vgui_widget* widget, const vgui_event* event,
		void* user)
	{
		auto* self = static_cast<combo_box*>(user);
		if (!self || !event || widget != self->button_)
			return;

		if (event->type == VGUI_EVENT_CLICK)
		{
			self->toggle();
			return;
		}
		if (event->type != VGUI_EVENT_KEY_DOWN || self->is_open())
			return;

		switch (event->key)
		{
		case VGUI_KEY_UP:
			if (event->mods & VGUI_MOD_ALT)
				self->open();
			else
				self->select_index(
					self->next_enabled_index(self->selected_, -1));
			break;
		case VGUI_KEY_DOWN:
			if (event->mods & VGUI_MOD_ALT)
				self->open();
			else
				self->select_index(
					self->next_enabled_index(self->selected_, 1));
			break;
		case VGUI_KEY_HOME:
			self->select_index(self->first_enabled_index());
			break;
		case VGUI_KEY_END:
			self->select_index(self->last_enabled_index());
			break;
		case VGUI_KEY_ESCAPE:
			self->close();
			break;
		default:
			break;
		}
	}

	void combo_box::on_menu_activate(vgui_widget* menu, vgui_item_id item,
		void* user)
	{
		auto* self = static_cast<combo_box*>(user);
		if (!self || menu != self->menu_ || !item)
			return;
		for (const auto& entry : self->menu_items_)
		{
			if (entry.first != item)
				continue;
			const int index = entry.second;
			if (index < 0 || index >= (int)self->items_.size())
				return;
			if (!self->items_[(std::size_t)index].enabled)
				return;
			if (index == self->selected_)
				return;
			self->apply_selection(index, true);
			return;
		}
	}

} /* namespace vgui_editor_kit */
