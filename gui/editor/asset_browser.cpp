#include "asset_browser.h"

#include "drag_bridge.h"
#include "toolbar.h"

#include "pch.h"
#include "tools/tools.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace vgui_editor_kit {

	namespace {

		constexpr vgui_color kFallbackPanel = 0xFF1E232B;
		constexpr vgui_color kFallbackBorder = 0xFF2E3440;
		constexpr vgui_color kFallbackText = 0xFFD8DEE9;

		vgui_theme theme_for(vgui_widget* widget)
		{
			vgui_theme theme = {};
			vgui_context* ctx = widget ? vgui_widget_get_context(widget) : nullptr;
			if (ctx)
				vgui_context_get_theme(ctx, &theme);
			if (!theme.panel_bg) theme.panel_bg = kFallbackPanel;
			if (!theme.panel_border) theme.panel_border = kFallbackBorder;
			if (!theme.text) theme.text = kFallbackText;
			return theme;
		}

		vgui_style style_for(vgui_widget* widget)
		{
			vgui_style style = {};
			vgui_context* ctx = widget ? vgui_widget_get_context(widget) : nullptr;
			if (ctx)
				vgui_context_get_style(ctx, &style);
			return style;
		}

		uint32_t to_ovg(vgui_color color)
		{
			return ((color & 0x00FF0000u) >> 16) |
				(color & 0x0000FF00u) |
				((color & 0x000000FFu) << 16) |
				(color & 0xFF000000u);
		}

		vgui_color text_color_of(const vgui_theme& theme)
		{
			return theme.row_text ? theme.row_text : theme.text;
		}

		vgui_color selected_text_color_of(const vgui_theme& theme)
		{
			return theme.row_selected_text ? theme.row_selected_text : text_color_of(theme);
		}

		float clampf(float value, float lo, float hi)
		{
			return value < lo ? lo : (value > hi ? hi : value);
		}

		bool rects_intersect(float ax, float ay, float aw, float ah, float bx, float by, float bw, float bh)
		{
			return ax < bx + bw && ax + aw > bx && ay < by + bh && ay + ah > by;
		}

	} /* namespace */

	/* Undo record for one asset rename.  Commands capture the owner through the
	 * alive token so stale records left on a shared stack become no-ops. */
	class asset_browser::rename_command final : public command
	{
	public:
		rename_command(asset_browser* owner, std::weak_ptr<int> alive, vgui_item_id item, std::string old_name, std::string new_name)
			: owner_(owner), alive_(std::move(alive)), item_(item), old_name_(std::move(old_name)), new_name_(std::move(new_name))
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

	asset_browser::asset_browser() = default;

	asset_browser::~asset_browser() = default;

	void asset_browser::build(vgui_widget* parent)
	{
		if (root_ || !parent)
			return;

		const vgui_theme theme = theme_for(parent);
		const vgui_style style = style_for(parent);

		root_ = vgui_widget_add(parent, VGUI_PANEL, nullptr);
		vgui_widget_set_grow(root_, 1.0f);
		vgui_widget_set_min_size(root_, 160, 80);
		vgui_widget_set_size(root_, 0, 0);
		vgui_widget_set_align_self(root_, VGUI_ALIGN_STRETCH);
		vgui_widget_set_layout(root_, VGUI_LAYOUT_COLUMN);
		vgui_widget_set_align_items(root_, VGUI_ALIGN_STRETCH);
		vgui_widget_set_padding(root_, 0, 0, 0, 0);
		vgui_widget_set_gap(root_, 4);
		vgui_widget_set_background(root_, 0);
		vgui_widget_set_border(root_, 0, 0);
		vgui_widget_set_rounding(root_, 0);

		header_ = vgui_widget_add(root_, VGUI_PANEL, nullptr);
		vgui_widget_set_layout(header_, VGUI_LAYOUT_ROW);
		vgui_widget_set_align_items(header_, VGUI_ALIGN_CENTER);
		vgui_widget_set_padding(header_, 0, 0, 0, 0);
		vgui_widget_set_gap(header_, 0);
		vgui_widget_set_background(header_, 0);
		vgui_widget_set_border(header_, 0, 0);

		vgui_widget* spacer = vgui_widget_add(header_, VGUI_PANEL, nullptr);
		vgui_widget_set_grow(spacer, 1.0f);
		vgui_widget_set_min_size(spacer, 1, 1);
		vgui_widget_set_background(spacer, 0);
		vgui_widget_set_border(spacer, 0, 0);

		view_switch_ = std::make_unique<segmented_control>();
		view_switch_->build(header_);
		(void)view_switch_->add_segment("List");
		(void)view_switch_->add_segment("Grid");
		view_switch_->set_on_change([this](std::size_t index) { set_view(index == 1 ? view_mode::grid : view_mode::list); });
		view_switch_->set_selected(view_ == view_mode::list ? 0 : 1);
		vgui_widget_set_visible(header_, switch_visible_ ? 1 : 0);

		list_ = vgui_widget_add(root_, VGUI_LIST, nullptr);
		vgui_widget_set_grow(list_, 1.0f);
		vgui_widget_set_min_size(list_, 160, 80);
		vgui_widget_set_size(list_, 0, 0);
		vgui_list_set_selection_mode(list_, VGUI_SELECTION_MULTIPLE);
		vgui_list_set_empty_text(list_, "No assets");
		vgui_list_on_select(list_, &asset_browser::on_select, this);

		vgui_row_source row_source = {};
		row_source.row_count = &asset_browser::source_row_count;
		row_source.row_info = &asset_browser::source_row_info;
		row_source.row_id = &asset_browser::source_row_id;
		row_source.row_id_index = &asset_browser::source_row_id_index;
		row_source.row_tooltip = &asset_browser::source_row_tooltip;
		row_source.row_icon = &asset_browser::source_row_icon;
		row_source.user = this;
		vgui_list_set_row_source(list_, &row_source);

		scroll_ = vgui_widget_add(root_, VGUI_PANEL, nullptr);
		vgui_widget_set_grow(scroll_, 1.0f);
		vgui_widget_set_min_size(scroll_, 160, 80);
		vgui_widget_set_size(scroll_, 0, 0);
		vgui_widget_set_layout(scroll_, VGUI_LAYOUT_ABSOLUTE);
		vgui_widget_set_padding(scroll_, 0, 0, 0, 0);
		vgui_widget_set_background(scroll_, theme.list_bg ? theme.list_bg : theme.panel_bg);
		vgui_widget_set_border(scroll_, theme.list_border ? theme.list_border : theme.panel_border, style.panel.border_width);
		vgui_widget_set_rounding(scroll_, style.list.rounding);
		vgui_panel_set_scrollable(scroll_, 0, 1);

		grid_ = vgui_widget_add(scroll_, VGUI_CUSTOM, nullptr);
		vgui_widget_set_pos(grid_, 0, 0);
		vgui_widget_set_size(grid_, 320, 1);
		vgui_widget_set_focusable(grid_, 1);
		vgui_widget_set_cursor(grid_, VGUI_CURSOR_HAND);
		vgui_widget_set_padding(grid_, 0, 0, 0, 0);
		vgui_widget_set_background(grid_, 0);
		vgui_widget_set_border(grid_, 0, 0);
		vgui_widget_set_rounding(grid_, 0);
		vgui_widget_set_font_size(grid_, 12);
		vgui_custom_set_clip_content(grid_, 1);
		vgui_custom_set_draw(grid_, &asset_browser::on_draw, this);
		vgui_custom_set_event(grid_, &asset_browser::on_event, this);
		vgui_widget_set_tooltip_provider(grid_, &asset_browser::on_grid_tooltip, this);

		vgui_widget_set_visible(list_, view_ == view_mode::list ? 1 : 0);
		vgui_widget_set_visible(scroll_, view_ == view_mode::grid ? 1 : 0);

		if (row_drag_enabled_)
			enable_row_drag();

		refresh();
		sync_list_selection();
	}

	void asset_browser::clear()
	{
		reset_model(true);
	}

	vgui_item_id asset_browser::add_item(std::string text, std::string detail)
	{
		if (!list_ || virtual_mode_)
			return 0;

		item_entry entry;
		entry.id = next_item_id_++;
		if (entry.id == 0)
			entry.id = next_item_id_++;
		entry.name = std::move(text);
		entry.type = std::move(detail);
		const vgui_item_id id = entry.id;
		items_.push_back(std::move(entry));
		item_index_[id] = (int)items_.size() - 1;
		refresh();
		return id;
	}

	bool asset_browser::set_item_thumbnail(vgui_item_id item, vgui_texture* texture)
	{
		const int index = index_of(item);
		if (index < 0)
			return false;
		if (virtual_mode_)
		{
			if (texture)
				virtual_thumbs_[item] = texture;
			else
				virtual_thumbs_.erase(item);
		}
		else
		{
			items_[(std::size_t)index].thumbnail = texture;
		}
		refresh();
		return true;
	}

	vgui_texture* asset_browser::item_thumbnail(vgui_item_id item) const
	{
		const int index = index_of(item);
		if (index < 0)
			return nullptr;
		if (!virtual_mode_)
			return items_[(std::size_t)index].thumbnail;

		auto found = virtual_thumbs_.find(item);
		if (found != virtual_thumbs_.end())
			return found->second;
		asset_info info;
		if (!item_at(index, &info))
			return nullptr;
		return info.thumbnail;
	}

	bool asset_browser::source_valid(const asset_source& source) const noexcept
	{
		return source.count && source.item_at;
	}

	void asset_browser::set_virtual_source(const asset_source* source)
	{
		pending_source_ = source ? *source : asset_source{};
		has_pending_source_ = true;
		apply_virtual_source();
	}

	void asset_browser::apply_virtual_source()
	{
		if (!has_pending_source_)
			return;
		source_ = pending_source_;
		pending_source_ = {};
		has_pending_source_ = false;
		virtual_mode_ = source_valid(source_);
		reset_model(false);
	}

	void asset_browser::reset_model(bool detach_source)
	{
		if (detach_source)
		{
			source_ = {};
			pending_source_ = {};
			has_pending_source_ = false;
			virtual_mode_ = false;
		}

		const bool had_selection = !selection_.empty();
		items_.clear();
		item_index_.clear();
		next_item_id_ = 1;
		virtual_thumbs_.clear();
		selection_.clear();
		selection_lookup_.clear();
		primary_ = 0;
		cursor_ = 0;
		anchor_index_ = -1;
		hover_index_ = -1;
		tooltip_.clear();
		drag_pending_ = false;
		drag_started_ = false;
		drag_item_ = 0;
		drag_name_.clear();
		marquee_active_ = false;
		marquee_base_.clear();

		if (list_)
		{
			vgui_list_source_changed(list_);
			sync_list_selection();
		}
		refresh();
		if (had_selection)
			notify_selection();
	}

	bool asset_browser::is_virtual() const noexcept
	{
		return virtual_mode_;
	}

	int asset_browser::item_count() const noexcept
	{
		if (!virtual_mode_)
			return (int)items_.size();
		if (!source_.count)
			return 0;
		return std::max(0, source_.count(source_.user));
	}

	bool asset_browser::item_at(int index, asset_info* out) const
	{
		if (!out || index < 0 || index >= item_count())
			return false;
		if (!virtual_mode_)
		{
			const item_entry& entry = items_[(std::size_t)index];
			out->name = entry.name;
			out->type = entry.type;
			out->thumbnail = entry.thumbnail;
			out->id = entry.id;
			return true;
		}

		if (!source_.item_at || !source_.item_at(source_.user, index, out))
			return false;
		if (out->id == 0)
			out->id = id_at(index);
		auto thumb = virtual_thumbs_.end();
		if (out->id)
			thumb = virtual_thumbs_.find(out->id);
		if (thumb != virtual_thumbs_.end())
			out->thumbnail = thumb->second;
		return true;
	}

	vgui_item_id asset_browser::id_at(int index) const noexcept
	{
		if (index < 0 || index >= item_count())
			return 0;
		if (!virtual_mode_)
			return items_[(std::size_t)index].id;
		if (source_.id_at)
			return source_.id_at(source_.user, index);
		return (vgui_item_id)index + 1;
	}

	int asset_browser::index_of(vgui_item_id item) const noexcept
	{
		if (item == 0)
			return -1;
		if (!virtual_mode_)
		{
			auto found = item_index_.find(item);
			return found == item_index_.end() ? -1 : found->second;
		}
		const int count = item_count();
		if (source_.index_of)
			return source_.index_of(source_.user, item);
		if (!source_.id_at)
		{
			if (item >= 1 && item <= (vgui_item_id)count)
				return (int)item - 1;
			return -1;
		}
		for (int i = 0; i < count; ++i)
		{
			if (source_.id_at(source_.user, i) == item)
				return i;
		}
		return -1;
	}

	asset_browser::view_mode asset_browser::view() const noexcept
	{
		return view_;
	}

	void asset_browser::set_view(view_mode mode)
	{
		if (view_ == mode)
		{
			if (view_switch_)
				view_switch_->set_selected(mode == view_mode::list ? 0 : 1);
			return;
		}

		if (view_ == view_mode::list && list_)
		{
			const vgui_item_id list_cursor = vgui_list_get_cursor(list_);
			if (list_cursor)
				cursor_ = list_cursor;
		}

		view_ = mode;
		if (view_switch_)
			view_switch_->set_selected(mode == view_mode::list ? 0 : 1);
		if (!root_ || !list_ || !scroll_)
			return;

		const bool list_mode = mode == view_mode::list;
		vgui_widget_set_visible(list_, list_mode ? 1 : 0);
		vgui_widget_set_visible(scroll_, list_mode ? 0 : 1);

		vgui_context* ctx = vgui_widget_get_context(root_);
		if (list_mode)
		{
			sync_list_selection();
			if (cursor_)
				vgui_list_scroll_to_item(list_, cursor_, VGUI_SCROLL_ALIGN_NEAREST);
			if (ctx)
				vgui_set_focus(ctx, list_);
		}
		else
		{
			refresh();
			ensure_cursor_visible();
			if (ctx)
				vgui_set_focus(ctx, grid_);
		}
	}

	void asset_browser::set_view_switch_visible(bool visible)
	{
		switch_visible_ = visible;
		if (header_)
			vgui_widget_set_visible(header_, visible ? 1 : 0);
	}

	bool asset_browser::view_switch_visible() const noexcept
	{
		return switch_visible_;
	}

	int asset_browser::visible_first() const noexcept
	{
		const int count = item_count();
		if (count <= 0)
			return 0;
		if (view_ == view_mode::list)
		{
			if (!list_)
				return 0;
			float list_w = 0.0f, list_h = 0.0f;
			float content_w = 0.0f, content_h = 0.0f;
			vgui_widget_get_rect(list_, nullptr, nullptr, &list_w, &list_h);
			vgui_panel_get_content_size(list_, &content_w, &content_h);
			(void)list_w;
			const vgui_style style = style_for(list_);
			const float row_h = content_h > 0.0f ? content_h / (float)count : std::max(1.0f, style.list.row_height);
			const float view_h = std::max(1.0f, list_h - style.list.padding_y * 2.0f);
			float scroll = 0.0f;
			vgui_panel_get_scroll(list_, nullptr, &scroll);
			(void)view_h;
			return std::min(count - 1, std::max(0, (int)std::floor(std::max(0.0f, scroll) / row_h)));
		}

		if (!scroll_)
			return 0;
		const grid_metrics metrics = metrics_for_width(available_width());
		const float row_h = metrics.cell_h + metrics.gap;
		const float scroll = grid_scroll_y();
		const int row = std::max(0, (int)std::floor(scroll / std::max(1.0f, row_h)));
		return std::min(count - 1, row * std::max(1, metrics.columns));
	}

	int asset_browser::visible_last() const noexcept
	{
		const int count = item_count();
		if (count <= 0)
			return 0;
		const int first = visible_first();
		if (view_ == view_mode::list)
		{
			if (!list_)
				return count;
			float list_w = 0.0f, list_h = 0.0f;
			float content_w = 0.0f, content_h = 0.0f;
			vgui_widget_get_rect(list_, nullptr, nullptr, &list_w, &list_h);
			vgui_panel_get_content_size(list_, &content_w, &content_h);
			(void)list_w;
			const vgui_style style = style_for(list_);
			const float row_h = content_h > 0.0f ? content_h / (float)count : std::max(1.0f, style.list.row_height);
			const float view_h = std::max(1.0f, list_h - style.list.padding_y * 2.0f);
			float scroll = 0.0f;
			vgui_panel_get_scroll(list_, nullptr, &scroll);
			const int last = (int)std::ceil((std::max(0.0f, scroll) + view_h) / row_h);
			return std::min(count, std::max(first + 1, last));
		}

		if (!scroll_)
			return count;
		const grid_metrics metrics = metrics_for_width(available_width());
		const float row_h = metrics.cell_h + metrics.gap;
		const float scroll = grid_scroll_y();
		const float view_h = available_height();
		int last_row = (int)std::ceil((scroll + view_h) / std::max(1.0f, row_h));
		last_row = std::max(last_row, first / std::max(1, metrics.columns) + 1);
		const int last = last_row * std::max(1, metrics.columns);
		return std::min(count, std::max(first + 1, last));
	}

	int asset_browser::visible_count() const noexcept
	{
		return std::max(0, visible_last() - visible_first());
	}

	std::vector<vgui_item_id> asset_browser::visible_items() const
	{
		std::vector<vgui_item_id> out;
		const int first = visible_first();
		const int last = visible_last();
		if (last <= first)
			return out;
		out.reserve((std::size_t)(last - first));
		for (int i = first; i < last; ++i)
		{
			const vgui_item_id id = id_at(i);
			if (id)
				out.push_back(id);
		}
		return out;
	}

	int asset_browser::grid_columns() const noexcept
	{
		if (view_ != view_mode::grid || !scroll_)
			return 1;
		return std::max(1, metrics_for_width(available_width()).columns);
	}

	bool asset_browser::item_rect(int index, float* x, float* y, float* width, float* height) const
	{
		if (index < 0 || index >= item_count())
			return false;
		if (view_ == view_mode::list)
		{
			if (!list_)
				return false;
			float list_w = 0.0f, list_h = 0.0f;
			float content_w = 0.0f, content_h = 0.0f;
			vgui_widget_get_rect(list_, nullptr, nullptr, &list_w, &list_h);
			(void)list_h;
			vgui_panel_get_content_size(list_, &content_w, &content_h);
			const vgui_style style = style_for(list_);
			const float row_h = content_h > 0.0f ? content_h / (float)std::max(1, item_count()) : std::max(1.0f, style.list.row_height);
			float scroll = 0.0f;
			vgui_panel_get_scroll(list_, nullptr, &scroll);
			float lx = 0.0f, ly = 0.0f;
			vgui_widget_get_rect(list_, &lx, &ly, nullptr, nullptr);
			const float cx = lx + style.list.padding_x;
			const float cy = ly + style.list.padding_y - scroll + (float)index * row_h;
			const float cw = std::max(0.0f, list_w - style.list.padding_x * 2.0f);
			if (x) *x = cx;
			if (y) *y = cy;
			if (width) *width = cw;
			if (height) *height = row_h;
			return true;
		}

		if (!scroll_)
			return false;
		const grid_metrics metrics = metrics_for_width(available_width());
		cell_rect(metrics, index, x, y, width, height);
		return true;
	}

	void asset_browser::refresh()
	{
		if (!root_)
			return;
		update_grid_metrics();
		if (grid_)
		{
			const float w = std::max(1.0f, last_grid_width_);
			const float h = std::max(1.0f, grid_content_h_);
			vgui_widget_set_size(grid_, w, h);
		}
		if (list_)
			vgui_list_source_changed(list_);
	}

	vgui_item_id asset_browser::selected() const noexcept
	{
		if (primary_)
			return primary_;
		return selection_.empty() ? 0 : selection_.front();
	}

	vgui_item_id asset_browser::cursor() const noexcept
	{
		if (view_ == view_mode::list && list_ && vgui_widget_get_visible(list_))
		{
			const vgui_item_id list_cursor = vgui_list_get_cursor(list_);
			if (list_cursor)
				return list_cursor;
		}
		return cursor_;
	}

	std::vector<vgui_item_id> asset_browser::selection() const
	{
		return selection_;
	}

	bool asset_browser::is_selected(vgui_item_id item) const noexcept
	{
		return item != 0 && selection_lookup_.find(item) != selection_lookup_.end();
	}

	void asset_browser::set_selection(const std::vector<vgui_item_id>& items)
	{
		std::vector<vgui_item_id> filtered;
		std::unordered_set<vgui_item_id> seen;
		filtered.reserve(items.size());
		for (vgui_item_id item : items)
		{
			if (item == 0 || seen.count(item) || index_of(item) < 0)
				continue;
			seen.insert(item);
			filtered.push_back(item);
		}
		const vgui_item_id primary = filtered.empty() ? 0 : filtered.front();
		const int anchor = primary ? index_of(primary) : -1;
		apply_selection(std::move(filtered), primary, anchor, true);
	}

	void asset_browser::clear_selection()
	{
		apply_selection({}, 0, -1, true);
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
		const int index = index_of(item);
		if (index < 0)
			return false;
		asset_info info;
		if (!item_at(index, &info))
			return false;
		const std::string old_name = info.name;
		if (old_name == name)
			return true;
		if (!apply_rename(item, name))
			return false;
		if (command_stack_)
		{
			command_stack_->push(std::make_unique<rename_command>(this, alive_token_, item, old_name, name));
		}
		return true;
	}

	bool asset_browser::apply_rename(vgui_item_id item, const std::string& name)
	{
		if (!rename_handler_ || item == 0)
			return false;
		const int index = index_of(item);
		if (index < 0)
			return false;
		if (!rename_handler_(item, name))
			return false;
		if (!virtual_mode_)
			items_[(std::size_t)index].name = name;
		refresh();
		return true;
	}

	void asset_browser::enable_row_drag()
	{
		row_drag_enabled_ = true;
		if (!list_)
			return;
		set_editor_row_drag(list_, false, [this](vgui_item_id item, int index, drag_payload& out) -> bool {
			if (!list_ || item == 0)
				return false;
			if (index < 0)
				index = index_of(item);
			asset_info info;
			if (index < 0 || !item_at(index, &info) || info.name.empty())
				return false;
			out.kind = drag_payload_kind::asset;
			out.id = info.name;
			return true;
		});
	}

	vgui_widget* asset_browser::widget() const noexcept
	{
		return list_;
	}

	vgui_widget* asset_browser::root() const noexcept
	{
		return root_;
	}

	vgui_widget* asset_browser::grid() const noexcept
	{
		return grid_;
	}

	vgui_widget* asset_browser::scroll() const noexcept
	{
		return scroll_;
	}

	segmented_control* asset_browser::view_switch() const noexcept
	{
		return view_switch_.get();
	}

	void asset_browser::update_grid_metrics()
	{
		if (!grid_)
			return;
		const float width = std::max(1.0f, available_width());
		const grid_metrics metrics = metrics_for_width(width);
		const int count = item_count();
		const int rows = count > 0 ? (count + metrics.columns - 1) / metrics.columns : 0;
		const float content_h = rows > 0
			? metrics.pad * 2.0f + (float)rows * metrics.cell_h + (float)(rows - 1) * metrics.gap
			: 0.0f;
		const bool width_changed = std::fabs(width - last_grid_width_) > 0.5f;
		const bool height_changed = std::fabs(content_h - grid_content_h_) > 0.5f;
		last_grid_width_ = width;
		grid_content_h_ = content_h;
		if (width_changed || height_changed)
			vgui_widget_set_size(grid_, width, std::max(1.0f, content_h));
	}

	asset_browser::grid_metrics asset_browser::metrics_for_width(float width) const
	{
		grid_metrics metrics;
		metrics.pad = 8.0f;
		metrics.gap = 8.0f;
		const float avail = std::max(1.0f, width - metrics.pad * 2.0f);
		const float target = 112.0f;
		metrics.columns = std::max(1, (int)std::floor((avail + metrics.gap) / (target + metrics.gap)));
		metrics.cell_w = std::max(1.0f, (avail - (float)(metrics.columns - 1) * metrics.gap) / (float)metrics.columns);
		metrics.cell_h = clampf(metrics.cell_w * 1.16f, 112.0f, 168.0f);
		return metrics;
	}

	float asset_browser::available_width() const
	{
		if (!scroll_)
			return 320.0f;
		float w = 0.0f, h = 0.0f;
		vgui_widget_get_rect(scroll_, nullptr, nullptr, &w, &h);
		(void)h;
		if (w <= 0.0f)
			return 320.0f;
		const vgui_style style = style_for(scroll_);
		w -= style.panel.scrollbar_width + style.panel.scrollbar_inset;
		return std::max(32.0f, w);
	}

	float asset_browser::available_height() const
	{
		if (!scroll_)
			return 180.0f;
		float w = 0.0f, h = 0.0f;
		vgui_widget_get_rect(scroll_, nullptr, nullptr, &w, &h);
		(void)w;
		return std::max(1.0f, h);
	}

	float asset_browser::grid_scroll_y() const
	{
		if (!scroll_)
			return 0.0f;
		float y = 0.0f;
		vgui_panel_get_scroll(scroll_, nullptr, &y);
		return std::max(0.0f, y);
	}

	int asset_browser::grid_index_at(float local_x, float local_y) const
	{
		const int count = item_count();
		if (count <= 0 || local_x < 0.0f || local_y < 0.0f)
			return -1;
		const grid_metrics metrics = metrics_for_width(available_width());
		const float stride_x = metrics.cell_w + metrics.gap;
		const float stride_y = metrics.cell_h + metrics.gap;
		if (local_x < metrics.pad || local_y < metrics.pad)
			return -1;
		const int col = (int)std::floor((local_x - metrics.pad) / std::max(1.0f, stride_x));
		const int row = (int)std::floor((local_y - metrics.pad) / std::max(1.0f, stride_y));
		if (col < 0 || col >= metrics.columns || row < 0)
			return -1;
		const float cell_x = metrics.pad + (float)col * stride_x;
		const float cell_y = metrics.pad + (float)row * stride_y;
		if (local_x > cell_x + metrics.cell_w || local_y > cell_y + metrics.cell_h)
			return -1;
		const int index = row * metrics.columns + col;
		return index >= 0 && index < count ? index : -1;
	}

	void asset_browser::cell_rect(const grid_metrics& metrics, int index, float* x, float* y, float* width, float* height) const
	{
		const int columns = std::max(1, metrics.columns);
		const int row = index / columns;
		const int col = index % columns;
		float gx = 0.0f, gy = 0.0f;
		if (grid_)
			vgui_widget_get_rect(grid_, &gx, &gy, nullptr, nullptr);
		if (x) *x = gx + metrics.pad + (float)col * (metrics.cell_w + metrics.gap);
		if (y) *y = gy + metrics.pad + (float)row * (metrics.cell_h + metrics.gap);
		if (width) *width = metrics.cell_w;
		if (height) *height = metrics.cell_h;
	}

	void asset_browser::draw_grid(ovg_ctx_cb* cb, rvg_t* vg, float abs_x, float abs_y, float abs_w, float abs_h, float delta_time)
	{
		if (!cb || !vg || !grid_)
			return;
		const vgui_theme theme = theme_for(grid_);
		const int count = item_count();
		if (count <= 0)
		{
			vgui_custom_draw_text(grid_, cb, vg, "No assets", abs_x, abs_y, abs_w, std::min(abs_h, available_height()), theme.disabled ? theme.disabled : theme.text, 0.5f, 0.5f);
			return;
		}

		const grid_metrics metrics = metrics_for_width(available_width());
		const int first = visible_first();
		const int last = visible_last();
		for (int i = first; i < last; ++i)
		{
			asset_info info;
			if (!item_at(i, &info))
				continue;
			draw_cell(cb, vg, metrics, i, info, abs_x, abs_y);
		}

		if (marquee_active_)
		{
			marquee_phase_ += delta_time * 18.0f;
			if (marquee_phase_ > 8.0f)
				marquee_phase_ = std::fmod(marquee_phase_, 8.0f);
			const float x0 = abs_x + std::min(marquee_x0_, marquee_x1_);
			const float y0 = abs_y + std::min(marquee_y0_, marquee_y1_);
			const float x1 = abs_x + std::max(marquee_x0_, marquee_x1_);
			const float y1 = abs_y + std::max(marquee_y0_, marquee_y1_);
			ovg_tools::marquee(cb, vg, x0, y0, x1, y1, to_ovg(theme.accent ? theme.accent : 0xFF409EFF), 1.0f, marquee_phase_);
		}
	}

	void asset_browser::draw_cell(ovg_ctx_cb* cb, rvg_t* vg, const grid_metrics& metrics, int index, const asset_info& info, float abs_x, float abs_y)
	{
		const vgui_theme theme = theme_for(grid_);
		float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
		cell_rect(metrics, index, &x, &y, &w, &h);
		if (w <= 1.0f || h <= 1.0f)
			return;
		(void)abs_x;
		(void)abs_y;

		const bool selected = is_selected(info.id);
		const bool hovered = hover_index_ == index;
		if (selected)
			ovg_tools::fill_round_rect(cb, vg, x, y, w, h, 5.0f, to_ovg(theme.row_selected ? theme.row_selected : 0x55409EFF));
		else if (hovered)
			ovg_tools::fill_round_rect(cb, vg, x, y, w, h, 5.0f, to_ovg(theme.row_hover ? theme.row_hover : 0x334A4A4A));

		const float thumb_x = x + 5.0f;
		const float thumb_y = y + 5.0f;
		const float thumb_w = std::max(1.0f, w - 10.0f);
		const float thumb_h = std::max(20.0f, h - 36.0f);
		const vgui_color thumb_bg = theme.edit_bg ? theme.edit_bg : theme.track;
		if (thumb_bg)
			ovg_tools::fill_round_rect(cb, vg, thumb_x, thumb_y, thumb_w, thumb_h, 4.0f, to_ovg(thumb_bg));
		if (info.thumbnail)
			vgui_custom_draw_texture(grid_, cb, vg, info.thumbnail, thumb_x, thumb_y, thumb_w, thumb_h, 0xFFFFFFFF);
		else
			vgui_custom_draw_text(grid_, cb, vg, info.type.empty() ? "?" : info.type.c_str(), thumb_x, thumb_y, thumb_w, thumb_h, theme.disabled ? theme.disabled : theme.text, 0.5f, 0.5f);

		const vgui_color text_color = selected ? selected_text_color_of(theme) : text_color_of(theme);
		vgui_custom_draw_text(grid_, cb, vg, info.name.c_str(), x + 6.0f, y + h - 30.0f, std::max(1.0f, w - 12.0f), 16.0f, text_color, 0.5f, 0.5f);
		if (!info.type.empty())
			vgui_custom_draw_text(grid_, cb, vg, info.type.c_str(), x + 6.0f, y + h - 15.0f, std::max(1.0f, w - 12.0f), 12.0f, theme.disabled ? theme.disabled : text_color_of(theme), 0.5f, 0.5f);

		if (selected)
			ovg_tools::stroke_round_rect(cb, vg, x, y, w, h, 5.0f, to_ovg(theme.accent ? theme.accent : 0xFF409EFF), 1.5f);
		else if (hovered)
			ovg_tools::stroke_round_rect(cb, vg, x, y, w, h, 5.0f, to_ovg(theme.panel_border), 1.0f);
	}

	void asset_browser::update_hover(float local_x, float local_y)
	{
		const int index = grid_index_at(local_x, local_y);
		hover_index_ = index;
		tooltip_.clear();
		if (index >= 0)
		{
			asset_info info;
			if (item_at(index, &info))
			{
				tooltip_ = info.name;
				if (!info.type.empty())
				{
					tooltip_ += " (";
					tooltip_ += info.type;
					tooltip_ += ")";
				}
			}
		}
	}

	void asset_browser::update_marquee(float local_x, float local_y)
	{
		if (!marquee_active_ || !grid_)
			return;
		marquee_x1_ = clampf(local_x, 0.0f, available_width());
		marquee_y1_ = clampf(local_y, 0.0f, available_height());

		const grid_metrics metrics = metrics_for_width(available_width());
		float gx = 0.0f, gy = 0.0f;
		vgui_widget_get_rect(grid_, &gx, &gy, nullptr, nullptr);
		const float mx0 = gx + std::min(marquee_x0_, marquee_x1_);
		const float my0 = gy + std::min(marquee_y0_, marquee_y1_);
		const float mw = std::fabs(marquee_x1_ - marquee_x0_);
		const float mh = std::fabs(marquee_y1_ - marquee_y0_);

		std::vector<vgui_item_id> items = marquee_base_;
		std::unordered_set<vgui_item_id> seen(items.begin(), items.end());
		vgui_item_id primary = 0;
		const int first = visible_first();
		const int last = visible_last();
		for (int i = first; i < last; ++i)
		{
			float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
			cell_rect(metrics, i, &x, &y, &w, &h);
			if (!rects_intersect(mx0, my0, mw, mh, x, y, w, h))
				continue;
			const vgui_item_id id = id_at(i);
			if (id == 0 || !seen.insert(id).second)
				continue;
			items.push_back(id);
			primary = id;
		}
		if (primary == 0 && !items.empty())
			primary = is_selected(primary_) ? primary_ : items.front();
		cursor_ = primary;
		hover_index_ = grid_index_at(local_x, local_y);
		apply_selection(std::move(items), primary, anchor_index_, true);
	}

	void asset_browser::begin_marquee(float local_x, float local_y, bool additive)
	{
		marquee_active_ = true;
		marquee_x0_ = marquee_x1_ = clampf(local_x, 0.0f, available_width());
		marquee_y0_ = marquee_y1_ = clampf(local_y, 0.0f, available_height());
		marquee_phase_ = 0.0f;
		if (!additive && !selection_.empty())
			apply_selection({}, 0, -1, true);
		marquee_base_ = additive ? selection_ : std::vector<vgui_item_id>();
		anchor_index_ = additive ? anchor_index_ : -1;
	}

	void asset_browser::end_marquee()
	{
		marquee_active_ = false;
		marquee_phase_ = 0.0f;
		marquee_base_.clear();
	}

	void asset_browser::move_cursor(int target_index, bool extend, bool keep_selection)
	{
		const int count = item_count();
		if (count <= 0)
			return;
		target_index = std::max(0, std::min(count - 1, target_index));
		const vgui_item_id id = id_at(target_index);
		if (id == 0)
			return;

		if (extend)
		{
			int anchor = anchor_index_;
			if (anchor < 0)
				anchor = index_of(cursor_);
			if (anchor < 0)
				anchor = target_index;
			select_range(anchor, target_index, keep_selection);
		}
		else if (keep_selection)
		{
			cursor_ = id;
			anchor_index_ = target_index;
		}
		else
		{
			apply_selection({ id }, id, target_index, true);
		}
		cursor_ = id;
		ensure_cursor_visible();
	}

	void asset_browser::ensure_cursor_visible()
	{
		if (!cursor_)
			return;
		if (view_ == view_mode::list)
		{
			if (list_)
				vgui_list_scroll_to_item(list_, cursor_, VGUI_SCROLL_ALIGN_NEAREST);
			return;
		}
		if (!scroll_)
			return;
		const int index = index_of(cursor_);
		if (index < 0)
			return;
		const grid_metrics metrics = metrics_for_width(available_width());
		const int columns = std::max(1, metrics.columns);
		const int row = index / columns;
		const float row_h = metrics.cell_h + metrics.gap;
		const float top = metrics.pad + (float)row * row_h;
		const float bottom = top + metrics.cell_h;
		const float view_h = available_height();
		float scroll = grid_scroll_y();
		if (top < scroll)
			scroll = top;
		else if (bottom > scroll + view_h)
			scroll = bottom - view_h;
		scroll = std::max(0.0f, scroll);
		vgui_panel_set_scroll(scroll_, 0.0f, scroll);
	}

	void asset_browser::select_range(int from, int to, bool additive)
	{
		const int count = item_count();
		if (count <= 0)
			return;
		from = std::max(0, std::min(count - 1, from));
		to = std::max(0, std::min(count - 1, to));
		const int lo = std::min(from, to);
		const int hi = std::max(from, to);
		std::vector<vgui_item_id> items = additive ? selection_ : std::vector<vgui_item_id>();
		std::unordered_set<vgui_item_id> seen(items.begin(), items.end());
		for (int i = lo; i <= hi; ++i)
		{
			const vgui_item_id id = id_at(i);
			if (id && seen.insert(id).second)
				items.push_back(id);
		}
		const vgui_item_id primary = id_at(to);
		apply_selection(std::move(items), primary, anchor_index_ >= 0 ? anchor_index_ : from, true);
	}

	void asset_browser::apply_selection(std::vector<vgui_item_id> items, vgui_item_id primary, int anchor, bool notify)
	{
		std::vector<vgui_item_id> filtered;
		std::unordered_set<vgui_item_id> lookup;
		filtered.reserve(items.size());
		lookup.reserve(items.size() * 2 + 1);
		for (vgui_item_id item : items)
		{
			if (item == 0 || !lookup.insert(item).second)
				continue;
			filtered.push_back(item);
		}
		if (primary != 0 && !lookup.count(primary))
			primary = 0;
		if (primary == 0 && !filtered.empty())
			primary = filtered.front();
		selection_ = std::move(filtered);
		selection_lookup_ = std::move(lookup);
		primary_ = primary;
		cursor_ = primary;
		anchor_index_ = anchor;
		sync_list_selection();
		if (notify)
			notify_selection();
	}

	void asset_browser::sync_list_selection()
	{
		if (!list_)
			return;
		syncing_ = true;
		vgui_list_clear_selection(list_);
		for (vgui_item_id item : selection_)
		{
			if (index_of(item) >= 0)
				vgui_list_select(list_, item, 1);
		}
		if (cursor_ && index_of(cursor_) >= 0)
			vgui_list_set_cursor(list_, cursor_);
		syncing_ = false;
	}

	void asset_browser::rebuild_selection_lookup()
	{
		selection_lookup_.clear();
		selection_lookup_.reserve(selection_.size() * 2 + 1);
		for (vgui_item_id item : selection_)
			selection_lookup_.insert(item);
	}

	void asset_browser::notify_selection()
	{
		if (listener_)
			listener_(primary_);
	}

	void asset_browser::update_primary_from_list(vgui_item_id item, int selected)
	{
		if (!list_)
			return;
		std::vector<vgui_item_id> items;
		const int count = vgui_list_get_selected_count(list_);
		items.reserve((std::size_t)std::max(0, count));
		for (int i = 0; i < count; ++i)
		{
			const vgui_item_id id = vgui_list_get_selected(list_, i);
			if (id)
				items.push_back(id);
		}
		selection_ = std::move(items);
		rebuild_selection_lookup();
		cursor_ = vgui_list_get_cursor(list_);
		if (selected && item)
			primary_ = item;
		else if (!is_selected(primary_))
			primary_ = selection_.empty() ? 0 : selection_.front();
		if (cursor_ == 0)
			cursor_ = primary_;
		anchor_index_ = primary_ ? index_of(primary_) : -1;
		notify_selection();
	}

	bool asset_browser::begin_grid_drag(float local_x, float local_y)
	{
		(void)local_x;
		(void)local_y;
		if (!row_drag_enabled_ || drag_item_ == 0 || drag_name_.empty() || !grid_)
			return false;
		drag_payload payload;
		payload.kind = drag_payload_kind::asset;
		payload.id = drag_name_;
		if (!begin_editor_drag(vgui_widget_get_context(grid_), grid_, payload))
			return false;
		drag_started_ = true;
		drag_pending_ = false;
		return true;
	}

	void asset_browser::finish_grid_drag()
	{
		drag_pending_ = false;
		drag_started_ = false;
		drag_item_ = 0;
		drag_name_.clear();
	}

	void asset_browser::on_select(vgui_widget* widget, vgui_item_id item, int selected, void* user)
	{
		(void)widget;
		if (!user || !item)
			return;
		auto* self = static_cast<asset_browser*>(user);
		if (self->syncing_)
			return;
		self->update_primary_from_list(item, selected);
	}

	void asset_browser::on_draw(vgui_widget* widget, ovg_ctx_cb* cb, rvg_t* vg, float abs_x, float abs_y, float abs_w, float abs_h, float delta_time, void* user)
	{
		(void)widget;
		if (!user)
			return;
		auto* self = static_cast<asset_browser*>(user);
		if (self->grid_)
		{
			const float width = std::max(1.0f, self->available_width());
			if (std::fabs(width - self->last_grid_width_) > 0.5f)
				self->update_grid_metrics();
		}
		self->draw_grid(cb, vg, abs_x, abs_y, abs_w, abs_h, delta_time);
	}

	int asset_browser::on_event(vgui_widget* widget, const vgui_event* event, void* user)
	{
		if (!event || !user || !widget)
			return 0;
		auto* self = static_cast<asset_browser*>(user);

		switch (event->type)
		{
		case VGUI_EVENT_MOUSE_MOVE:
			self->update_hover(event->x, event->y);
			if (self->marquee_active_)
			{
				self->update_marquee(event->x, event->y);
				return 1;
			}
			if (self->drag_pending_ && !self->drag_started_)
			{
				const float dx = event->x - self->drag_press_x_;
				const float dy = event->y - self->drag_press_y_;
				if (dx * dx + dy * dy >= 9.0f)
				{
					if (!self->begin_grid_drag(event->x, event->y))
						self->drag_pending_ = false;
				}
				return 1;
			}
			return 1;

		case VGUI_EVENT_MOUSE_DOWN:
		{
			if (event->button != 1)
				return 0;
			self->update_hover(event->x, event->y);
			const int index = self->grid_index_at(event->x, event->y);
			if (index < 0)
			{
				self->finish_grid_drag();
				self->begin_marquee(event->x, event->y, (event->mods & (VGUI_MOD_CTRL | VGUI_MOD_SHIFT)) != 0);
				return 1;
			}

			asset_info info;
			if (!self->item_at(index, &info))
				return 1;
			const bool shift = (event->mods & VGUI_MOD_SHIFT) != 0;
			const bool ctrl = (event->mods & VGUI_MOD_CTRL) != 0;
			if (shift)
			{
				int anchor = self->anchor_index_;
				if (anchor < 0)
					anchor = self->index_of(self->cursor_);
				if (anchor < 0)
					anchor = index;
				self->select_range(anchor, index, ctrl);
			}
			else if (ctrl)
			{
				std::vector<vgui_item_id> items = self->selection_;
				auto found = std::find(items.begin(), items.end(), info.id);
				vgui_item_id primary = info.id;
				int anchor = index;
				if (found != items.end())
				{
					items.erase(found);
					primary = items.empty() ? 0 : items.front();
					anchor = self->anchor_index_ >= 0 ? self->anchor_index_ : index;
				}
				else
				{
					items.push_back(info.id);
				}
				self->apply_selection(std::move(items), primary, anchor, true);
			}
			else
			{
				self->apply_selection({ info.id }, info.id, index, true);
			}

			self->cursor_ = info.id;
			self->hover_index_ = index;
			self->drag_pending_ = self->row_drag_enabled_ && !info.name.empty();
			self->drag_started_ = false;
			self->drag_item_ = info.id;
			self->drag_name_ = info.name;
			self->drag_press_x_ = event->x;
			self->drag_press_y_ = event->y;
			return 1;
		}

		case VGUI_EVENT_MOUSE_UP:
			if (event->button != 1)
				return 0;
			if (self->marquee_active_)
				self->end_marquee();
			self->finish_grid_drag();
			self->update_hover(event->x, event->y);
			return 1;

		case VGUI_EVENT_LEAVE:
			self->hover_index_ = -1;
			self->tooltip_.clear();
			return 0;

		case VGUI_EVENT_KEY_DOWN:
		{
			const bool shift = (event->mods & VGUI_MOD_SHIFT) != 0;
			const bool ctrl = (event->mods & VGUI_MOD_CTRL) != 0;
			const int count = self->item_count();
			if (ctrl && (event->key == 'A' || event->key == 'a'))
			{
				if (count <= 0)
					return 1;
				std::vector<vgui_item_id> items;
				items.reserve((std::size_t)count);
				for (int i = 0; i < count; ++i)
				{
					const vgui_item_id id = self->id_at(i);
					if (id)
						items.push_back(id);
				}
				const vgui_item_id primary = self->cursor_ ? self->cursor_ : (items.empty() ? 0 : items.front());
				const int anchor = items.empty() ? -1 : self->index_of(items.front());
				self->apply_selection(std::move(items), primary, anchor, true);
				self->ensure_cursor_visible();
				return 1;
			}

			if (event->key == VGUI_KEY_UP || event->key == VGUI_KEY_DOWN || event->key == VGUI_KEY_LEFT || event->key == VGUI_KEY_RIGHT || event->key == VGUI_KEY_HOME || event->key == VGUI_KEY_END || event->key == VGUI_KEY_PAGE_UP || event->key == VGUI_KEY_PAGE_DOWN)
			{
				if (count <= 0)
					return 1;
				const grid_metrics metrics = self->metrics_for_width(self->available_width());
				const int columns = std::max(1, metrics.columns);
				int index = self->index_of(self->cursor_);
				if (index < 0)
					index = 0;
				int target = index;
				switch (event->key)
				{
				case VGUI_KEY_UP: target = index - columns; break;
				case VGUI_KEY_DOWN: target = index + columns; break;
				case VGUI_KEY_LEFT: target = index - 1; break;
				case VGUI_KEY_RIGHT: target = index + 1; break;
				case VGUI_KEY_HOME: target = 0; break;
				case VGUI_KEY_END: target = count - 1; break;
				case VGUI_KEY_PAGE_UP:
				{
					const int rows = std::max(1, (int)(self->available_height() / std::max(1.0f, metrics.cell_h + metrics.gap)));
					target = index - columns * rows;
					break;
				}
				case VGUI_KEY_PAGE_DOWN:
				{
					const int rows = std::max(1, (int)(self->available_height() / std::max(1.0f, metrics.cell_h + metrics.gap)));
					target = index + columns * rows;
					break;
				}
				default: break;
				}
				target = std::max(0, std::min(count - 1, target));
				self->move_cursor(target, shift, ctrl);
				return 1;
			}

			if (event->key == ' ')
			{
				const int index = self->index_of(self->cursor_);
				if (index >= 0)
				{
					const vgui_item_id id = self->id_at(index);
					if (id)
					{
						std::vector<vgui_item_id> items = self->selection_;
						auto found = std::find(items.begin(), items.end(), id);
						vgui_item_id primary = id;
						if (found != items.end())
						{
							items.erase(found);
							primary = items.empty() ? 0 : items.front();
						}
						else
						{
							items.push_back(id);
						}
						self->apply_selection(std::move(items), primary, self->anchor_index_, true);
						self->ensure_cursor_visible();
					}
				}
				return 1;
			}
			return 0;
		}

		default:
			return 0;
		}
	}

	const char* asset_browser::on_grid_tooltip(vgui_widget* widget, void* user)
	{
		(void)widget;
		if (!user)
			return nullptr;
		auto* self = static_cast<asset_browser*>(user);
		return self->tooltip_.empty() ? nullptr : self->tooltip_.c_str();
	}

	int asset_browser::source_row_count(void* user)
	{
		auto* self = static_cast<asset_browser*>(user);
		return self ? self->item_count() : 0;
	}

	int asset_browser::source_row_info(void* user, int index, const char** text, const char** detail, void** item_user)
	{
		auto* self = static_cast<asset_browser*>(user);
		if (!self)
			return 0;
		if (!self->item_at(index, &self->scratch_info_))
			return 0;
		if (text) *text = self->scratch_info_.name.c_str();
		if (detail) *detail = self->scratch_info_.type.c_str();
		if (item_user) *item_user = nullptr;
		return 1;
	}

	vgui_item_id asset_browser::source_row_id(void* user, int index)
	{
		auto* self = static_cast<asset_browser*>(user);
		return self ? self->id_at(index) : 0;
	}

	int asset_browser::source_row_id_index(void* user, vgui_item_id item)
	{
		auto* self = static_cast<asset_browser*>(user);
		return self ? self->index_of(item) : -1;
	}

	const char* asset_browser::source_row_tooltip(void* user, int index)
	{
		auto* self = static_cast<asset_browser*>(user);
		if (!self)
			return nullptr;
		if (!self->item_at(index, &self->scratch_info_))
			return nullptr;
		self->scratch_tooltip_ = self->scratch_info_.name;
		if (!self->scratch_info_.type.empty())
		{
			self->scratch_tooltip_ += " (";
			self->scratch_tooltip_ += self->scratch_info_.type;
			self->scratch_tooltip_ += ")";
		}
		return self->scratch_tooltip_.c_str();
	}

	int asset_browser::source_row_icon(void* user, int index, vgui_texture** out_texture)
	{
		auto* self = static_cast<asset_browser*>(user);
		if (!self || !out_texture)
			return 0;
		if (!self->item_at(index, &self->scratch_info_) || !self->scratch_info_.thumbnail)
			return 0;
		*out_texture = self->scratch_info_.thumbnail;
		return 1;
	}

} /* namespace vgui_editor_kit */
