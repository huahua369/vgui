#include "asset_browser.h"

#include "drag_bridge.h"

#include <utility>

namespace vgui_editor_kit {

	/* Undo record for one asset rename.  Commands capture the owner through the
	 * alive token so stale records left on a shared stack become no-ops. */
	class asset_browser::rename_command final : public command
	{
	public:
		rename_command(asset_browser* owner, std::weak_ptr<int> alive,
			vgui_item_id item, std::string old_name, std::string new_name)
			: owner_(owner), alive_(std::move(alive)), item_(item),
			old_name_(std::move(old_name)), new_name_(std::move(new_name))
		{
		}

		const char* name() const noexcept override
		{
			return "Rename asset";
		}

		bool execute() override
		{
			if (alive_.expired())
				return true;
			return owner_->apply_rename(item_, new_name_);
		}

		void undo() override
		{
			if (!alive_.expired())
				(void)owner_->apply_rename(item_, old_name_);
		}

	private:
		asset_browser* owner_;
		std::weak_ptr<int> alive_;
		vgui_item_id item_;
		std::string old_name_;
		std::string new_name_;
	};

	void asset_browser::build(vgui_widget* parent)
	{
		if (list_ || !parent)
			return;

		list_ = vgui_widget_add(parent, VGUI_LIST, nullptr);
		vgui_widget_set_grow(list_, 1.0f);
		vgui_widget_set_min_size(list_, 160, 80);
		vgui_list_set_selection_mode(list_, VGUI_SELECTION_SINGLE);
		vgui_list_on_select(list_, &asset_browser::on_select, this);
	}

	void asset_browser::clear()
	{
		if (list_)
			vgui_list_clear(list_);
		selected_ = 0;
	}

	vgui_item_id asset_browser::add_item(std::string text, std::string detail)
	{
		if (!list_)
			return 0;

		vgui_item_id item = vgui_list_add_item(list_, text.c_str(), nullptr);
		if (item && !detail.empty())
			vgui_list_set_item_detail(list_, item, detail.c_str());
		return item;
	}

	vgui_item_id asset_browser::selected() const noexcept
	{
		return selected_;
	}

	vgui_widget* asset_browser::widget() const noexcept
	{
		return list_;
	}

	void asset_browser::set_selection_listener(selection_listener listener)
	{
		listener_ = std::move(listener);
	}

	void asset_browser::set_command_stack(command_stack* stack)
	{
		command_stack_ = stack;
	}

	void asset_browser::set_rename_handler(rename_handler handler)
	{
		rename_handler_ = std::move(handler);
	}

	bool asset_browser::rename_item(vgui_item_id item, const std::string& name)
	{
		if (!list_ || item == 0)
			return false;

		const char* current = vgui_list_get_item_text(list_, item);
		const std::string old_name = current ? current : "";
		if (old_name == name)
			return true;

		if (!apply_rename(item, name))
			return false;

		if (command_stack_)
		{
			command_stack_->push(std::make_unique<rename_command>(
				this, alive_token_, item, old_name, name));
		}
		return true;
	}

	bool asset_browser::apply_rename(vgui_item_id item, const std::string& name)
	{
		if (!list_ || !rename_handler_)
			return false;
		if (!rename_handler_(item, name))
			return false;
		vgui_list_set_item_text(list_, item, name.c_str());
		return true;
	}

	void asset_browser::enable_row_drag()
	{
		if (!list_)
			return;
		set_editor_row_drag(list_, false,
			[this](vgui_item_id item, int index, drag_payload& out) -> bool {
				(void)index;
				if (!list_ || item == 0)
					return false;
				const char* text = vgui_list_get_item_text(list_, item);
				if (!text || !*text)
					return false;
				out.kind = drag_payload_kind::asset;
				out.id = text;
				return true;
			});
	}

	void asset_browser::on_select(vgui_widget* widget, vgui_item_id item,
		int selected, void* user)
	{
		(void)widget;
		if (!selected || !user)
			return;

		auto* self = static_cast<asset_browser*>(user);
		self->selected_ = item;
		if (self->listener_)
			self->listener_(item);
	}

} /* namespace vgui_editor_kit */
