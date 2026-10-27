#include "asset_browser.h"

#include "breadcrumb.h"
#include "combo_box.h"
#include "drag_bridge.h"
#include "search_box.h"
#include "toolbar.h"
#include "ui_scale.h"

#include "pch.h"
#include "tools/tools.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
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

		std::string lower_ascii(std::string text)
		{
			for (char& ch : text)
			{
				const unsigned char value = (unsigned char)ch;
				if (value < 128)
					ch = (char)std::tolower(value);
			}
			return text;
		}

		bool contains_ci(const std::string& haystack, const std::string& needle)
		{
			if (needle.empty())
				return true;
			return lower_ascii(haystack).find(lower_ascii(needle)) != std::string::npos;
		}

		bool equals_ci(const std::string& lhs, const std::string& rhs)
		{
			return lower_ascii(lhs) == lower_ascii(rhs);
		}

		std::string trim_ascii(std::string text)
		{
			std::size_t begin = 0;
			while (begin < text.size() && std::isspace((unsigned char)text[begin]))
				++begin;
			std::size_t end = text.size();
			while (end > begin && std::isspace((unsigned char)text[end - 1]))
				--end;
			return text.substr(begin, end - begin);
		}

		const char* operation_verb(asset_browser::operation kind)
		{
			switch (kind)
			{
			case asset_browser::operation::rename: return "Rename";
			case asset_browser::operation::remove: return "Delete";
			case asset_browser::operation::duplicate: return "Duplicate";
			case asset_browser::operation::move: return "Move";
			case asset_browser::operation::reimport: return "Reimport";
			}
			return "Asset operation";
		}

		std::string operation_command_name(asset_browser::operation kind)
		{
			switch (kind)
			{
			case asset_browser::operation::rename: return "Rename asset";
			case asset_browser::operation::remove: return "Delete assets";
			case asset_browser::operation::duplicate: return "Duplicate assets";
			case asset_browser::operation::move: return "Move assets";
			case asset_browser::operation::reimport: return "Reimport assets";
			}
			return "Asset operation";
		}

		const char* operation_error_text(asset_browser::operation_error error)
		{
			switch (error)
			{
			case asset_browser::operation_error::none: return "unknown error";
			case asset_browser::operation_error::no_selection: return "nothing selected";
			case asset_browser::operation_error::invalid_name: return "invalid name";
			case asset_browser::operation_error::invalid_target: return "invalid destination";
			case asset_browser::operation_error::not_found: return "asset not found";
			case asset_browser::operation_error::conflict: return "name conflict";
			case asset_browser::operation_error::read_only: return "asset is read-only";
			case asset_browser::operation_error::in_use: return "asset is in use";
			case asset_browser::operation_error::unsupported: return "operation not supported";
			case asset_browser::operation_error::io_error: return "I/O error";
			}
			return "unknown error";
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

	/* A06: one host-backed batch.  The host already applied the operation when
	 * this is pushed; execute() re-applies it for redo and undo() reverses it.
	 * Both refresh the current directory so the browser and the database stay
	 * in sync even when undo/redo comes from the shell menu. */
	class asset_browser::operation_command final : public command
	{
	public:
		operation_command(asset_browser* owner, std::weak_ptr<int> alive,
			std::string name, std::function<bool()> redo,
			std::function<bool()> undo)
			: owner_(owner), alive_(std::move(alive)), name_(std::move(name)),
			redo_(std::move(redo)), undo_(std::move(undo))
		{
		}

		const char* name() const noexcept override
		{
			return name_.c_str();
		}

		bool execute() override
		{
			if (alive_.expired() || !redo_)
				return true;
			const bool ok = redo_();
			owner_->refresh_after_command();
			return ok;
		}

		void undo() override
		{
			if (alive_.expired())
				return;
			if (undo_)
				(void)undo_();
			owner_->refresh_after_command();
		}

	private:
		asset_browser* owner_;
		std::weak_ptr<int> alive_;
		std::string name_;
		std::function<bool()> redo_;
		std::function<bool()> undo_;
	};

	namespace {

		/* Context-level handlers have no destroy callback, so the registration
		 * block is intentionally leaked (same pattern as outliner_view and
		 * drag_bridge).  The weak token makes stale entries harmless. */
		struct context_entry
		{
			std::weak_ptr<int> alive;
			asset_browser* owner = nullptr;
		};

		/* Menu item ids are local to one menu, so the action/target is stored
		 * in the item's user pointer.  Small tagged integers dodge the id
		 * collisions between the top-level menu and the Move To submenu. */
		constexpr std::uintptr_t kMenuActionBase = 1;
		constexpr std::uintptr_t kMenuMoveBase = 1000;

		void* menu_tag(std::uintptr_t value)
		{
			return reinterpret_cast<void*>(value);
		}

		std::uintptr_t menu_tag_value(void* user)
		{
			return reinterpret_cast<std::uintptr_t>(user);
		}

		struct rename_prompt_state
		{
			std::weak_ptr<int> alive;
			asset_browser* owner = nullptr;
			vgui_item_id item = 0;
		};

		void on_rename_prompt_result(vgui_context* ctx, int result,
			const char* text, void* user)
		{
			(void)ctx;
			std::unique_ptr<rename_prompt_state> state(
				static_cast<rename_prompt_state*>(user));
			if (!state || state->alive.expired() || !state->owner)
				return;
			if (result != VGUI_MODAL_RESULT_CONFIRM || !text)
				return;
			(void)state->owner->rename_item(state->item, text);
		}

		struct remove_confirm_state
		{
			std::weak_ptr<int> alive;
			asset_browser* owner = nullptr;
			std::vector<vgui_item_id> items;
		};

		void on_remove_confirm_result(vgui_context* ctx, int result,
			const char* text, void* user)
		{
			(void)ctx;
			(void)text;
			std::unique_ptr<remove_confirm_state> state(
				static_cast<remove_confirm_state*>(user));
			if (!state || state->alive.expired() || !state->owner)
				return;
			if (result != VGUI_MODAL_RESULT_CONFIRM)
				return;
			(void)state->owner->remove_items(state->items);
		}

	} /* namespace */

	asset_browser::asset_browser()
		: thumbnails_(std::make_unique<thumbnail_cache>())
	{
	}

	asset_browser::~asset_browser()
	{
		/* Join the thumbnail worker before the host-scoped loader data and the
		 * vgui context disappear. */
		cancel_thumbnail_requests();
		thumbnails_.reset();
	}

	void asset_browser::build(vgui_widget* parent)
	{
		if (root_ || !parent)
			return;

		if (thumbnails_)
			thumbnails_->set_context(vgui_widget_get_context(parent));

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
		vgui_widget_set_gap(header_, 2);
		vgui_widget_set_background(header_, 0);
		vgui_widget_set_border(header_, 0, 0);

		back_button_ = std::make_unique<icon_button>();
		back_button_->build(header_, "<");
		back_button_->set_tooltip("Back");
		back_button_->set_on_click([this]() { (void)go_back(); });
		forward_button_ = std::make_unique<icon_button>();
		forward_button_->build(header_, ">");
		forward_button_->set_tooltip("Forward");
		forward_button_->set_on_click([this]() { (void)go_forward(); });
		reload_button_ = std::make_unique<icon_button>();
		reload_button_->build(header_, "R");
		reload_button_->set_tooltip("Refresh");
		reload_button_->set_on_click([this]() { (void)reload(); });

		breadcrumb_ = std::make_unique<breadcrumb>();
		breadcrumb_->build(header_);
		vgui_widget* crumbs = breadcrumb_->widget();
		vgui_widget_set_grow(crumbs, 1.0f);
		vgui_widget_set_min_size(crumbs, 60, 24);
		breadcrumb_->set_on_navigate([this](std::size_t index, const std::string& id) {
			if (!breadcrumb_ || index + 1 >= breadcrumb_->count())
				return;
			(void)navigate_to(id);
		});

		view_switch_ = std::make_unique<segmented_control>();
		view_switch_->build(header_);
		(void)view_switch_->add_segment("List");
		(void)view_switch_->add_segment("Grid");
		view_switch_->set_on_change([this](std::size_t index) { set_view(index == 1 ? view_mode::grid : view_mode::list); });
		view_switch_->set_selected(view_ == view_mode::list ? 0 : 1);
		vgui_widget_set_visible(view_switch_->widget(), switch_visible_ ? 1 : 0);
		vgui_widget_set_visible(header_, navigation_visible_ ? 1 : 0);

		filter_row_ = vgui_widget_add(root_, VGUI_PANEL, nullptr);
		vgui_widget_set_layout(filter_row_, VGUI_LAYOUT_ROW);
		vgui_widget_set_align_items(filter_row_, VGUI_ALIGN_CENTER);
		vgui_widget_set_padding(filter_row_, 0, 0, 0, 0);
		vgui_widget_set_gap(filter_row_, 4);
		vgui_widget_set_background(filter_row_, 0);
		vgui_widget_set_border(filter_row_, 0, 0);

		search_ = std::make_unique<search_box>();
		search_->build(filter_row_, "Search assets");
		vgui_widget_set_grow(search_->widget(), 1.0f);
		vgui_widget_set_min_size(search_->widget(), 100, 24);
		search_->set_on_change([this](const std::string& text) {
			if (search_query_ == text)
				return;
			search_query_ = text;
			search_pending_ = true;
			search_elapsed_ = 0.0f;
			if (search_debounce_ <= 0.0f)
				apply_search();
		});
		search_->set_on_submit([this](const std::string& text) {
			search_query_ = text;
			apply_search();
		});
		if (!search_query_.empty())
			search_->set_text(search_query_);

		type_combo_ = std::make_unique<combo_box>();
		type_combo_->set_id("asset_type_filter");
		type_combo_->build(filter_row_);
		vgui_widget_set_size(type_combo_->widget(), 104, 24);
		vgui_widget_set_min_size(type_combo_->widget(), 84, 24);
		type_combo_->set_placeholder("All types");
		type_combo_->set_on_change([this](int, const std::string& value) { set_type_filter(value); });

		/* A zero-size custom widget would be culled before draw, so the
		 * debounce driver is a 2x2 transparent ticker in the filter row. */
		ticker_ = vgui_widget_add(filter_row_, VGUI_CUSTOM, nullptr);
		vgui_widget_set_size(ticker_, 2, 2);
		vgui_widget_set_min_size(ticker_, 1, 1);
		vgui_widget_set_background(ticker_, 0);
		vgui_widget_set_border(ticker_, 0, 0);
		vgui_custom_set_draw(ticker_, &asset_browser::on_tick_draw, this);

		list_ = vgui_widget_add(root_, VGUI_LIST, nullptr);
		vgui_widget_set_grow(list_, 1.0f);
		vgui_widget_set_min_size(list_, 160, 80);
		vgui_widget_set_size(list_, 0, 0);
		vgui_list_set_selection_mode(list_, VGUI_SELECTION_MULTIPLE);
		vgui_list_set_empty_text(list_, empty_state_.c_str());
		vgui_list_on_select(list_, &asset_browser::on_select, this);
		vgui_list_on_activate(list_, &asset_browser::on_activate, this);

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

		update_import_target();

		update_breadcrumb();
		update_type_combo();
		update_filter_visibility();
		refresh();
		sync_list_selection();
		update_history_buttons();
		if (context_menu_)
			enable_context_menu();

		/* E05 semantic names for host screen-reader bridges. */
		vgui_widget_set_a11y_name(root_, "Assets");
		vgui_widget_set_a11y_name(breadcrumb_->widget(), "Asset path");
		vgui_widget_set_a11y_name(view_switch_->widget(), "Asset view");
		vgui_widget_set_a11y_name(search_->widget(), "Search assets");
		vgui_widget_set_a11y_name(type_combo_->widget(), "Asset type filter");
		vgui_widget_set_a11y_role(type_combo_->widget(), VGUI_A11Y_ROLE_COMBOBOX);
		vgui_widget_set_a11y_name(list_, "Asset list");
		vgui_widget_set_a11y_name(grid_, "Asset grid");
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
		const int index = base_index_of(item);
		if (index < 0)
			return false;
		/* A host texture supersedes (or clears) any asynchronous result for
		 * this item. */
		if (thumbnails_)
			thumbnails_->erase(thumbnail_key(item));
		thumbnail_requests_.erase(thumbnail_key(item));
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
		const int index = base_index_of(item);
		if (index < 0)
			return nullptr;
		if (!virtual_mode_)
		{
			vgui_texture* host = items_[(std::size_t)index].thumbnail;
			return host ? host : cached_thumbnail(item);
		}

		auto found = virtual_thumbs_.find(item);
		if (found != virtual_thumbs_.end())
			return found->second;
		asset_info info;
		if (!base_item_at(index, &info))
			return nullptr;
		return info.thumbnail;
	}

	thumbnail_cache& asset_browser::thumbnails() noexcept
	{
		return *thumbnails_;
	}

	const thumbnail_cache& asset_browser::thumbnails() const noexcept
	{
		return *thumbnails_;
	}

	std::string asset_browser::thumbnail_key(vgui_item_id item) const
	{
		if (item == 0)
			return {};
		std::string key = current_path_id_;
		key.push_back('\x1F');
		key += std::to_string((unsigned long long)item);
		return key;
	}

	thumbnail_cache::state asset_browser::thumbnail_state(vgui_item_id item) const
	{
		if (!thumbnails_ || item == 0)
			return thumbnail_cache::state::missing;
		if (item_thumbnail(item))
			return thumbnail_cache::state::ready;
		return thumbnails_->state_of(thumbnail_key(item));
	}

	bool asset_browser::retry_thumbnail(vgui_item_id item)
	{
		if (!thumbnails_ || item == 0 || item_thumbnail(item))
			return false;
		const std::string key = thumbnail_key(item);
		if (thumbnails_->state_of(key) != thumbnail_cache::state::failed)
			return false;
		thumbnails_->retry(key);
		return true;
	}

	vgui_texture* asset_browser::cached_thumbnail(vgui_item_id item) const
	{
		if (!thumbnails_ || item == 0)
			return nullptr;
		return thumbnails_->lookup(thumbnail_key(item));
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

	void asset_browser::clear_model_data()
	{
		/* Items are about to change identity (or disappear), so no pending
		 * thumbnail request can still target them.  Already cached textures
		 * stay in the LRU for the next visit to this path. */
		cancel_thumbnail_requests();
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
	}

	void asset_browser::reset_model(bool detach_source, bool notify)
	{
		if (detach_source)
		{
			source_ = {};
			pending_source_ = {};
			has_pending_source_ = false;
			virtual_mode_ = false;
		}

		const bool had_selection = !selection_.empty();
		clear_model_data();
		refresh();
		sync_list_selection();
		if (had_selection && notify)
			notify_selection();
	}

	bool asset_browser::install_directory(const asset_directory& directory)
	{
		if (source_valid(directory.source))
		{
			source_ = directory.source;
			virtual_mode_ = true;
			return true;
		}

		for (const asset_info& info : directory.items)
		{
			item_entry entry;
			entry.id = info.id;
			if (entry.id == 0 || item_index_.find(entry.id) != item_index_.end())
			{
				do
				{
					entry.id = next_item_id_++;
				} while (entry.id == 0 || item_index_.find(entry.id) != item_index_.end());
			}
			else if (entry.id >= next_item_id_)
			{
				next_item_id_ = entry.id + 1;
				if (next_item_id_ == 0)
					next_item_id_ = 1;
			}
			entry.name = info.name;
			entry.type = info.type;
			entry.thumbnail = info.thumbnail;
			item_index_[entry.id] = (int)items_.size();
			items_.push_back(std::move(entry));
		}
		return false;
	}

	void asset_browser::apply_directory(const std::string& path_id, const asset_directory& directory)
	{
		clear_model_data();
		source_ = {};
		pending_source_ = {};
		has_pending_source_ = false;
		virtual_mode_ = false;
		directory_empty_text_ = directory.empty_text.empty()
			? default_empty_text_ : directory.empty_text;

		(void)install_directory(directory);

		current_path_id_ = path_id;
		has_location_ = true;
		path_items_ = directory.path;
		if (path_items_.empty() && !path_id.empty())
			path_items_.push_back({ path_id, path_id });
		update_breadcrumb();

		refresh();
		sync_list_selection();
		if (list_)
			vgui_panel_set_scroll(list_, 0.0f, 0.0f);
		if (scroll_)
			vgui_panel_set_scroll(scroll_, 0.0f, 0.0f);
		update_history_buttons();
		if (path_callback_)
			path_callback_(current_path_id_);
	}

	bool asset_browser::is_virtual() const noexcept
	{
		return virtual_mode_;
	}

	int asset_browser::base_item_count() const noexcept
	{
		if (!virtual_mode_)
			return (int)items_.size();
		if (!source_.count)
			return 0;
		return std::max(0, source_.count(source_.user));
	}

	bool asset_browser::base_item_at(int index, asset_info* out) const
	{
		if (!out || index < 0 || index >= base_item_count())
			return false;
		if (!virtual_mode_)
		{
			const item_entry& entry = items_[(std::size_t)index];
			out->name = entry.name;
			out->type = entry.type;
			out->thumbnail = entry.thumbnail;
			out->id = entry.id;
			if (!out->thumbnail)
				out->thumbnail = cached_thumbnail(out->id);
			return true;
		}

		if (!source_.item_at || !source_.item_at(source_.user, index, out))
			return false;
		if (out->id == 0)
			out->id = base_id_at(index);
		auto thumb = virtual_thumbs_.end();
		if (out->id)
			thumb = virtual_thumbs_.find(out->id);
		if (thumb != virtual_thumbs_.end())
			out->thumbnail = thumb->second;
		if (!out->thumbnail)
			out->thumbnail = cached_thumbnail(out->id);
		return true;
	}

	vgui_item_id asset_browser::base_id_at(int index) const noexcept
	{
		if (index < 0 || index >= base_item_count())
			return 0;
		if (!virtual_mode_)
			return items_[(std::size_t)index].id;
		if (source_.id_at)
			return source_.id_at(source_.user, index);
		return (vgui_item_id)index + 1;
	}

	int asset_browser::base_index_of(vgui_item_id item) const noexcept
	{
		if (item == 0)
			return -1;
		if (!virtual_mode_)
		{
			auto found = item_index_.find(item);
			return found == item_index_.end() ? -1 : found->second;
		}
		const int count = base_item_count();
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

	int asset_browser::view_index_to_base(int index) const noexcept
	{
		if (index < 0)
			return -1;
		if (!filtered_)
			return index < base_item_count() ? index : -1;
		if (index >= (int)view_indices_.size())
			return -1;
		return view_indices_[(std::size_t)index];
	}

	int asset_browser::base_index_to_view(int index) const noexcept
	{
		if (index < 0)
			return -1;
		if (!filtered_)
			return index < base_item_count() ? index : -1;
		auto found = std::lower_bound(view_indices_.begin(), view_indices_.end(), index);
		if (found == view_indices_.end() || *found != index)
			return -1;
		return (int)(found - view_indices_.begin());
	}

	void asset_browser::rebuild_view_indices()
	{
		view_indices_.clear();
		const bool has_text = !applied_search_.empty();
		const bool has_type = !type_filter_.empty();
		filtered_ = has_text || has_type;
		if (!filtered_)
			return;

		const int count = base_item_count();
		view_indices_.reserve((std::size_t)count);
		for (int i = 0; i < count; ++i)
		{
			asset_info info;
			if (!base_item_at(i, &info))
				continue;
			if (has_type && !equals_ci(info.type, type_filter_))
				continue;
			if (has_text && !contains_ci(info.name, applied_search_) &&
				!contains_ci(info.type, applied_search_))
				continue;
			view_indices_.push_back(i);
		}
	}

	int asset_browser::item_count() const noexcept
	{
		if (!filtered_)
			return base_item_count();
		return (int)view_indices_.size();
	}

	int asset_browser::total_item_count() const noexcept
	{
		return base_item_count();
	}

	bool asset_browser::item_at(int index, asset_info* out) const
	{
		const int base = view_index_to_base(index);
		return base >= 0 && base_item_at(base, out);
	}

	vgui_item_id asset_browser::id_at(int index) const noexcept
	{
		const int base = view_index_to_base(index);
		return base >= 0 ? base_id_at(base) : 0;
	}

	int asset_browser::index_of(vgui_item_id item) const noexcept
	{
		return base_index_to_view(base_index_of(item));
	}

	bool asset_browser::item_info(vgui_item_id item, asset_info* out) const
	{
		const int index = base_index_of(item);
		return index >= 0 && base_item_at(index, out);
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
		if (view_switch_)
			vgui_widget_set_visible(view_switch_->widget(), visible ? 1 : 0);
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
		rebuild_view_indices();
		update_empty_text();
		if (!root_)
			return;
		update_grid_metrics();
		if (grid_)
		{
			const float w = std::max(1.0f, last_grid_width_);
			const float h = std::max(1.0f, grid_content_h_);
			vgui_widget_set_size(grid_, to_logical(grid_, w), to_logical(grid_, h));
		}
		if (list_)
			vgui_list_source_changed(list_);
	}

	void asset_browser::set_navigate_handler(navigate_handler handler)
	{
		navigate_handler_ = std::move(handler);
		update_history_buttons();
	}

	void asset_browser::set_path_listener(path_listener listener)
	{
		path_callback_ = std::move(listener);
	}

	asset_browser::nav_state asset_browser::capture_navigation_state() const
	{
		nav_state state;
		state.path_id = current_path_id_;
		state.selection = selection_;
		state.primary = primary_;
		state.cursor = cursor();
		state.anchor = anchor_index_;
		if (list_)
			vgui_panel_get_scroll(list_, nullptr, &state.list_scroll);
		if (scroll_)
			vgui_panel_get_scroll(scroll_, nullptr, &state.grid_scroll);
		return state;
	}

	void asset_browser::restore_navigation_state(const nav_state& state, bool notify)
	{
		std::vector<vgui_item_id> items;
		items.reserve(state.selection.size());
		for (vgui_item_id item : state.selection)
		{
			if (base_index_of(item) >= 0)
				items.push_back(item);
		}

		vgui_item_id primary = state.primary;
		if (base_index_of(primary) < 0)
			primary = items.empty() ? 0 : items.front();
		vgui_item_id cursor = state.cursor;
		if (base_index_of(cursor) < 0)
			cursor = primary;
		int anchor = primary ? index_of(primary) : -1;
		if (state.anchor >= 0 && state.anchor < item_count())
			anchor = state.anchor;

		apply_selection(std::move(items), primary, anchor, false);
		cursor_ = cursor;
		if (list_ && cursor)
			vgui_list_set_cursor(list_, cursor);
		if (list_)
			vgui_panel_set_scroll(list_, 0.0f, state.list_scroll);
		if (scroll_)
			vgui_panel_set_scroll(scroll_, 0.0f, state.grid_scroll);
		if (notify)
			notify_selection();
	}

	void asset_browser::update_breadcrumb()
	{
		if (!breadcrumb_)
			return;
		std::vector<breadcrumb_item> items;
		items.reserve(path_items_.size());
		for (const asset_path_item& segment : path_items_)
			items.push_back({ segment.label, segment.id });
		breadcrumb_->set_items(std::move(items));
	}

	void asset_browser::update_history_buttons()
	{
		if (back_button_)
			back_button_->set_enabled(navigate_handler_ && !back_stack_.empty());
		if (forward_button_)
			forward_button_->set_enabled(navigate_handler_ && !forward_stack_.empty());
		if (reload_button_)
			reload_button_->set_enabled(has_location_ && navigate_handler_ != nullptr);
	}

	bool asset_browser::navigate_to(const std::string& path_id)
	{
		if (!navigate_handler_)
			return false;
		/* The caller may pass a breadcrumb's internal id; keep it alive because
		 * apply_directory() rebuilds the strip during this call. */
		const std::string target = path_id;
		if (has_location_ && current_path_id_ == target)
			return true;

		asset_directory directory;
		if (!navigate_handler_(target, &directory))
			return false;

		const bool had_location = has_location_;
		const bool had_selection = !selection_.empty();
		nav_state previous = capture_navigation_state();
		apply_directory(target, directory);
		if (had_location)
			back_stack_.push_back(std::move(previous));
		forward_stack_.clear();
		update_history_buttons();
		if (had_selection)
			notify_selection();
		return true;
	}

	bool asset_browser::can_go_back() const noexcept
	{
		return navigate_handler_ != nullptr && !back_stack_.empty();
	}

	bool asset_browser::can_go_forward() const noexcept
	{
		return navigate_handler_ != nullptr && !forward_stack_.empty();
	}

	bool asset_browser::go_back()
	{
		if (!navigate_handler_ || back_stack_.empty())
			return false;
		const nav_state target = back_stack_.back();
		asset_directory directory;
		if (!navigate_handler_(target.path_id, &directory))
			return false;

		nav_state current = capture_navigation_state();
		back_stack_.pop_back();
		forward_stack_.push_back(std::move(current));
		apply_directory(target.path_id, directory);
		restore_navigation_state(target, true);
		update_history_buttons();
		return true;
	}

	bool asset_browser::go_forward()
	{
		if (!navigate_handler_ || forward_stack_.empty())
			return false;
		const nav_state target = forward_stack_.back();
		asset_directory directory;
		if (!navigate_handler_(target.path_id, &directory))
			return false;

		nav_state current = capture_navigation_state();
		forward_stack_.pop_back();
		back_stack_.push_back(std::move(current));
		apply_directory(target.path_id, directory);
		restore_navigation_state(target, true);
		update_history_buttons();
		return true;
	}

	bool asset_browser::reload()
	{
		if (!navigate_handler_ || !has_location_)
			return false;
		asset_directory directory;
		if (!navigate_handler_(current_path_id_, &directory))
			return false;

		const nav_state state = capture_navigation_state();
		apply_directory(current_path_id_, directory);
		restore_navigation_state(state, true);
		update_history_buttons();
		return true;
	}

	const std::string& asset_browser::current_path() const noexcept
	{
		return current_path_id_;
	}

	void asset_browser::set_navigation_visible(bool visible)
	{
		navigation_visible_ = visible;
		if (header_)
			vgui_widget_set_visible(header_, visible ? 1 : 0);
	}

	bool asset_browser::navigation_visible() const noexcept
	{
		return navigation_visible_;
	}

	void asset_browser::set_search_debounce(float seconds)
	{
		search_debounce_ = std::max(0.0f, seconds);
		if (search_debounce_ <= 0.0f && search_pending_)
			apply_search();
	}

	float asset_browser::search_debounce() const noexcept
	{
		return search_debounce_;
	}

	void asset_browser::set_search_text(std::string text, bool immediate)
	{
		search_query_ = std::move(text);
		if (search_)
			search_->set_text(search_query_);
		if (immediate || search_debounce_ <= 0.0f)
		{
			apply_search();
			return;
		}
		search_pending_ = true;
		search_elapsed_ = 0.0f;
	}

	const std::string& asset_browser::search_text() const noexcept
	{
		return search_query_;
	}

	bool asset_browser::search_pending() const noexcept
	{
		return search_pending_;
	}

	void asset_browser::apply_search()
	{
		search_pending_ = false;
		search_elapsed_ = 0.0f;
		const std::string query = trim_ascii(search_query_);
		if (query == applied_search_)
			return;

		applied_search_ = query;
		refresh();
		sync_list_selection();
		if (list_)
			vgui_panel_set_scroll(list_, 0.0f, 0.0f);
		if (scroll_)
			vgui_panel_set_scroll(scroll_, 0.0f, 0.0f);
	}

	bool asset_browser::filter_active() const noexcept
	{
		return !applied_search_.empty() || !type_filter_.empty();
	}

	void asset_browser::set_empty_texts(std::string empty, std::string no_matches)
	{
		default_empty_text_ = empty.empty() ? "No assets" : std::move(empty);
		directory_empty_text_ = default_empty_text_;
		no_matches_text_ = no_matches.empty() ? "No matches" : std::move(no_matches);
		update_empty_text();
	}

	const std::string& asset_browser::empty_state_text() const noexcept
	{
		return empty_state_;
	}

	void asset_browser::cancel_thumbnail_requests()
	{
		if (!thumbnails_)
		{
			thumbnail_requests_.clear();
			return;
		}
		for (const std::string& key : thumbnail_requests_)
			thumbnails_->cancel(key);
		thumbnail_requests_.clear();
	}

	void asset_browser::sync_thumbnail_requests()
	{
		if (!thumbnails_)
			return;
		if (!thumbnails_->configured())
		{
			cancel_thumbnail_requests();
			return;
		}

		std::unordered_set<std::string> wanted;
		const int first = visible_first();
		const int last = visible_last();
		for (int i = first; i < last; ++i)
		{
			asset_info info;
			if (!item_at(i, &info) || info.id == 0)
				continue;
			if (info.thumbnail)
				continue;	/* host or already uploaded */

			const std::string key = thumbnail_key(info.id);
			const thumbnail_cache::state state = thumbnails_->state_of(key);
			if (state == thumbnail_cache::state::ready)
				continue;
			if (state == thumbnail_cache::state::failed)
			{
				/* Keep a visible failure tracked so it is not cancelled and
				 * immediately retried every tick.  retry_thumbnail() restarts
				 * it explicitly. */
				wanted.insert(key);
				continue;
			}

			thumbnail_cache::request req;
			req.key = key;
			req.name = info.name;
			req.type = info.type;
			thumbnails_->enqueue(req, thumbnail_cache::priority::high);
			wanted.insert(key);
		}

		for (const std::string& key : thumbnail_requests_)
		{
			if (wanted.find(key) == wanted.end())
				thumbnails_->cancel(key);
		}
		thumbnail_requests_ = std::move(wanted);
	}

	void asset_browser::tick(float delta_time)
	{
		thumbnail_phase_ += std::max(0.0f, delta_time);
		if (thumbnail_phase_ > 1000.0f)
			thumbnail_phase_ = std::fmod(thumbnail_phase_, 1000.0f);

		sync_thumbnail_requests();
		if (thumbnails_)
		{
			const bool changed = thumbnails_->tick(delta_time);
			/* Uploads change row icons without changing the row count. */
			if (changed && list_)
			{
				const int count = item_count();
				if (count > 0)
					vgui_list_source_rows_changed(list_, 0, count);
			}
		}

		if (!search_pending_)
			return;
		if (search_debounce_ <= 0.0f)
		{
			apply_search();
			return;
		}
		search_elapsed_ += std::max(0.0f, delta_time);
		if (search_elapsed_ >= search_debounce_)
			apply_search();
	}

	void asset_browser::set_type_options(std::vector<std::string> types)
	{
		type_options_ = std::move(types);
		bool found = type_filter_.empty();
		for (const std::string& type : type_options_)
		{
			if (equals_ci(type, type_filter_))
			{
				type_filter_ = type;
				found = true;
				break;
			}
		}
		if (!found)
			type_filter_.clear();
		update_type_combo();
		if (!found)
		{
			refresh();
			sync_list_selection();
			if (list_)
				vgui_panel_set_scroll(list_, 0.0f, 0.0f);
			if (scroll_)
				vgui_panel_set_scroll(scroll_, 0.0f, 0.0f);
		}
	}

	const std::vector<std::string>& asset_browser::type_options() const noexcept
	{
		return type_options_;
	}

	void asset_browser::set_type_filter(std::string type)
	{
		type = trim_ascii(std::move(type));
		if (!type.empty())
		{
			for (const std::string& option : type_options_)
			{
				if (equals_ci(option, type))
				{
					type = option;
					break;
				}
			}
		}
		if (type == type_filter_)
		{
			if (type_combo_)
				type_combo_->set_value(type);
			return;
		}
		type_filter_ = std::move(type);
		if (type_combo_)
			type_combo_->set_value(type_filter_);
		refresh();
		sync_list_selection();
		if (list_)
			vgui_panel_set_scroll(list_, 0.0f, 0.0f);
		if (scroll_)
			vgui_panel_set_scroll(scroll_, 0.0f, 0.0f);
	}

	const std::string& asset_browser::type_filter() const noexcept
	{
		return type_filter_;
	}

	void asset_browser::update_type_combo()
	{
		if (!type_combo_)
		{
			update_filter_visibility();
			return;
		}
		std::vector<combo_box::item> items;
		items.reserve(type_options_.size() + 1);
		items.push_back({ "All types", std::string(), true });
		for (const std::string& type : type_options_)
			items.push_back({ type, type, true });
		type_combo_->set_items(std::move(items));
		if (!type_combo_->set_value(type_filter_) && !type_filter_.empty())
			type_combo_->clear_selection();
		update_filter_visibility();
	}

	void asset_browser::update_filter_visibility()
	{
		if (type_combo_)
		{
			vgui_widget* widget = type_combo_->widget();
			const bool visible = type_filter_visible_ && !type_options_.empty();
			vgui_widget_set_visible(widget, visible ? 1 : 0);
		}
	}

	void asset_browser::update_empty_text()
	{
		if (item_count() == 0 && base_item_count() > 0 && filtered_)
			empty_state_ = no_matches_text_;
		else
			empty_state_ = directory_empty_text_;
		if (list_)
			vgui_list_set_empty_text(list_, empty_state_.c_str());
	}

	void asset_browser::set_type_filter_visible(bool visible)
	{
		type_filter_visible_ = visible;
		update_filter_visibility();
	}

	bool asset_browser::type_filter_visible() const noexcept
	{
		return type_filter_visible_;
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

	asset_browser::selection_listener_id asset_browser::add_selection_listener(
		selection_listener listener)
	{
		if (!listener)
			return 0;
		selection_listener_entry entry;
		entry.id = next_selection_listener_++;
		entry.fn = std::move(listener);
		const selection_listener_id id = entry.id;
		selection_listeners_.push_back(std::move(entry));
		return id;
	}

	bool asset_browser::remove_selection_listener(selection_listener_id id)
	{
		if (!id)
			return false;
		for (auto it = selection_listeners_.begin(); it != selection_listeners_.end(); ++it)
		{
			if (it->id != id)
				continue;
			selection_listeners_.erase(it);
			return true;
		}
		return false;
	}

	void asset_browser::set_command_stack(command_stack* stack)
	{
		command_stack_ = stack;
	}

	void asset_browser::set_rename_handler(rename_handler handler)
	{
		rename_handler_ = std::move(handler);
	}

	void asset_browser::set_activate_listener(activate_listener listener)
	{
		activate_listener_ = std::move(listener);
	}

	void asset_browser::activate_selected()
	{
		const vgui_item_id item = selected();
		if (item && activate_listener_)
			activate_listener_(item);
	}

	bool asset_browser::rename_item(vgui_item_id item, const std::string& name)
	{
		if (!list_ || item == 0)
		{
			report_failure("Rename", {}, operation_error::no_selection, {});
			return false;
		}
		const int index = index_of(item);
		asset_info info;
		if (index < 0 || !item_at(index, &info))
		{
			report_failure("Rename", {}, operation_error::not_found, {});
			return false;
		}

		const std::string clean = trim_ascii(name);
		if (clean.empty())
		{
			report_failure("Rename", "'" + info.name + "'",
				operation_error::invalid_name, "name cannot be empty");
			return false;
		}
		if (clean == info.name)
		{
			last_error_ = operation_error::none;
			last_undoable_ = true;
			publish_notice("Rename '" + info.name + "': unchanged",
				operation_error::none, false);
			return true;
		}

		operation_request request;
		request.kind = operation::rename;
		request.items.push_back(item);
		request.name = clean;
		request.path_id = current_path_id_;
		return perform_operation(std::move(request));
	}

	void asset_browser::set_operation_handler(operation_handler handler)
	{
		operation_handler_ = std::move(handler);
	}

	void asset_browser::set_notice_listener(notice_listener listener)
	{
		notice_listener_ = std::move(listener);
	}

	void asset_browser::set_move_targets_handler(move_targets_handler handler)
	{
		move_targets_handler_ = std::move(handler);
	}

	void asset_browser::set_dependency_handler(dependency_handler handler)
	{
		dependency_handler_ = std::move(handler);
	}

	void asset_browser::set_import_prompt_handler(import_prompt_handler handler)
	{
		import_prompt_handler_ = std::move(handler);
	}

	bool asset_browser::operation_available(operation kind) const noexcept
	{
		switch (kind)
		{
		case operation::rename:
			return operation_handler_ != nullptr || rename_handler_ != nullptr;
		case operation::remove:
		case operation::duplicate:
		case operation::move:
		case operation::reimport:
			return operation_handler_ != nullptr;
		}
		return false;
	}

	std::vector<vgui_item_id> asset_browser::selected_items_in_order() const
	{
		std::vector<vgui_item_id> ordered;
		ordered.reserve(selection_.size());
		std::unordered_set<vgui_item_id> seen;
		if (primary_ != 0 && selection_lookup_.count(primary_) && index_of(primary_) >= 0)
		{
			ordered.push_back(primary_);
			seen.insert(primary_);
		}
		for (vgui_item_id item : selection_)
		{
			if (item == 0 || !seen.insert(item).second || index_of(item) < 0)
				continue;
			ordered.push_back(item);
		}
		return ordered;
	}

	std::vector<vgui_item_id> asset_browser::valid_operation_items(
		const std::vector<vgui_item_id>& items) const
	{
		std::vector<vgui_item_id> valid;
		valid.reserve(items.size());
		std::unordered_set<vgui_item_id> seen;
		for (vgui_item_id item : items)
		{
			if (item == 0 || !seen.insert(item).second || index_of(item) < 0)
				continue;
			valid.push_back(item);
		}
		return valid;
	}

	std::string asset_browser::operation_target_text(const operation_request& request) const
	{
		if (request.items.empty())
			return {};
		if (request.items.size() == 1)
		{
			const int index = index_of(request.items.front());
			asset_info info;
			if (index >= 0 && item_at(index, &info) && !info.name.empty())
				return "'" + info.name + "'";
		}
		return std::to_string(request.items.size()) + " assets";
	}

	void asset_browser::show_toast(const std::string& message, int level)
	{
		vgui_context* ctx = list_ ? vgui_widget_get_context(list_)
			: (grid_ ? vgui_widget_get_context(grid_) : nullptr);
		if (!ctx || message.empty())
			return;
		vgui_toast_options options;
		vgui_toast_options_default(&options);
		options.level = (vgui_toast_level)level;
		(void)vgui_toast_show(ctx, message.c_str(), &options);
	}

	void asset_browser::publish_notice(std::string message,
		operation_error error, bool warning_toast)
	{
		last_message_ = std::move(message);
		last_error_ = error;
		if (notice_listener_)
			notice_listener_(last_message_, last_error_);
		if (last_error_ != operation_error::none)
			show_toast(last_message_, VGUI_TOAST_ERROR);
		else if (warning_toast)
			show_toast(last_message_, VGUI_TOAST_WARNING);
	}

	void asset_browser::report_failure(const std::string& action_text,
		const std::string& target_text, operation_error error,
		const std::string& detail)
	{
		last_undoable_ = false;
		std::string message = action_text.empty() ? "Asset operation" : action_text;
		if (!target_text.empty())
			message += " " + target_text;
		message += " failed: ";
		message += operation_error_text(error);
		if (!detail.empty())
			message += " (" + detail + ")";
		publish_notice(std::move(message), error, false);
	}

	void asset_browser::report_operation_failure(operation kind,
		const std::vector<vgui_item_id>& items, operation_error error,
		const std::string& detail)
	{
		operation_request request;
		request.kind = kind;
		request.items = items;
		report_failure(operation_verb(kind), operation_target_text(request),
			error, detail);
	}

	void asset_browser::remove_mirror_items(const std::vector<vgui_item_id>& items)
	{
		if (virtual_mode_)
		{
			for (vgui_item_id item : items)
				virtual_thumbs_.erase(item);
			return;
		}
		if (items.empty())
			return;
		std::unordered_set<vgui_item_id> removed(items.begin(), items.end());
		items_.erase(std::remove_if(items_.begin(), items_.end(),
			[&](const item_entry& entry) { return removed.count(entry.id) != 0; }),
			items_.end());
		item_index_.clear();
		for (std::size_t i = 0; i < items_.size(); ++i)
			item_index_[items_[(std::size_t)i].id] = (int)i;
	}

	void asset_browser::refresh_after_operation()
	{
		if (navigate_handler_ && has_location_)
		{
			(void)reload();
			return;
		}
		refresh();
		sync_list_selection();
	}

	void asset_browser::refresh_after_command()
	{
		if (navigate_handler_ && has_location_)
		{
			(void)reload();
			return;
		}
		refresh();
		sync_list_selection();
	}

	bool asset_browser::perform_operation(operation_request request)
	{
		request.items = valid_operation_items(request.items);
		if (request.items.empty())
		{
			report_operation_failure(request.kind, {}, operation_error::no_selection,
				{});
			return false;
		}

		const std::string target = operation_target_text(request);
		std::string old_name;
		if (request.kind == operation::rename)
		{
			asset_info info;
			const int index = index_of(request.items.front());
			if (index >= 0)
				(void)item_at(index, &info);
			old_name = info.name;
		}

		/* Legacy hosts that only installed rename_handler keep the A01/A02
		 * path.  It records rename_command and remains fully undoable. */
		if (!operation_handler_)
		{
			if (request.kind == operation::rename && rename_handler_)
			{
				if (!apply_rename(request.items.front(), request.name))
				{
					report_failure("Rename", target, operation_error::io_error,
						"host rejected the rename");
					return false;
				}
				if (command_stack_)
				{
					command_stack_->push(std::make_unique<rename_command>(this,
						alive_token_, request.items.front(), old_name,
						request.name));
					last_undoable_ = true;
				}
				else
				{
					last_undoable_ = false;
				}
				std::string message = "Renamed '" + old_name + "' to '" +
					request.name + "'";
				if (!last_undoable_)
					message += " (not undoable)";
				publish_notice(std::move(message), operation_error::none,
					!last_undoable_);
				refresh_after_operation();
				return true;
			}
			report_operation_failure(request.kind, request.items,
				operation_error::unsupported, "no operation handler installed");
			return false;
		}

		operation_result result;
		if (!operation_handler_(request, &result))
		{
			const operation_error error = result.error == operation_error::none
				? operation_error::io_error : result.error;
			report_operation_failure(request.kind, request.items, error,
				result.message);
			return false;
		}

		const bool undoable = result.undoable && result.undo && result.redo;
		last_undoable_ = undoable && command_stack_ != nullptr;
		if (last_undoable_)
		{
			command_stack_->push(std::make_unique<operation_command>(this,
				alive_token_, operation_command_name(request.kind),
				std::move(result.redo), std::move(result.undo)));
		}

		/* Keep local models usable when no navigate handler is installed.  A
		 * full-model host without navigation rebuilds the directory itself. */
		if (!(navigate_handler_ && has_location_))
		{
			switch (request.kind)
			{
			case operation::rename:
				if (!virtual_mode_)
				{
					const int index = base_index_of(request.items.front());
					if (index >= 0)
						items_[(std::size_t)index].name = request.name;
				}
				break;
			case operation::remove:
			case operation::move:
				remove_mirror_items(request.items);
				break;
			case operation::reimport:
				if (thumbnails_)
				{
					for (vgui_item_id item : request.items)
						thumbnails_->erase(thumbnail_key(item));
				}
				break;
			case operation::duplicate:
				break;
			}
		}

		refresh_after_operation();

		std::vector<vgui_item_id> wanted = selection_;
		if (!result.select.empty())
			wanted = result.select;
		else if (request.kind == operation::duplicate && !result.created.empty())
			wanted = result.created;
		else if (request.kind == operation::remove ||
			request.kind == operation::move)
		{
			std::unordered_set<vgui_item_id> gone(request.items.begin(),
				request.items.end());
			wanted.erase(std::remove_if(wanted.begin(), wanted.end(),
				[&](vgui_item_id item) { return gone.count(item) != 0; }),
				wanted.end());
		}

		std::vector<vgui_item_id> filtered;
		std::unordered_set<vgui_item_id> seen;
		for (vgui_item_id item : wanted)
		{
			if (item == 0 || !seen.insert(item).second || index_of(item) < 0)
				continue;
			filtered.push_back(item);
		}
		const vgui_item_id primary = filtered.empty() ? 0 : filtered.front();
		const int anchor = primary ? index_of(primary) : -1;
		apply_selection(std::move(filtered), primary, anchor, true);

		std::string message;
		switch (request.kind)
		{
		case operation::rename:
			message = "Renamed '" + old_name + "' to '" + request.name + "'";
			break;
		case operation::remove:
			message = "Deleted " + std::to_string(request.items.size()) +
				(request.items.size() == 1 ? " asset" : " assets");
			break;
		case operation::duplicate:
			message = "Duplicated " + std::to_string(request.items.size()) +
				(request.items.size() == 1 ? " asset" : " assets");
			break;
		case operation::move:
			message = "Moved " + std::to_string(request.items.size()) +
				(request.items.size() == 1 ? " asset" : " assets") +
				" to '" + request.target_path_id + "'";
			break;
		case operation::reimport:
			message = "Reimported " + std::to_string(request.items.size()) +
				(request.items.size() == 1 ? " asset" : " assets");
			break;
		}
		if (!result.message.empty())
			message += " (" + result.message + ")";
		if (!last_undoable_)
			message += " (not undoable)";
		publish_notice(std::move(message), operation_error::none,
			!last_undoable_);
		return true;
	}

	bool asset_browser::remove_items(const std::vector<vgui_item_id>& items)
	{
		const std::vector<vgui_item_id> valid = valid_operation_items(items);
		if (valid.empty())
		{
			report_operation_failure(operation::remove, items,
				items.empty() ? operation_error::no_selection
					: operation_error::not_found, {});
			return false;
		}
		operation_request request;
		request.kind = operation::remove;
		request.items = valid;
		request.path_id = current_path_id_;
		return perform_operation(std::move(request));
	}

	bool asset_browser::remove_selected()
	{
		return remove_items(selected_items_in_order());
	}

	bool asset_browser::duplicate_items(const std::vector<vgui_item_id>& items)
	{
		const std::vector<vgui_item_id> valid = valid_operation_items(items);
		if (valid.empty())
		{
			report_operation_failure(operation::duplicate, items,
				items.empty() ? operation_error::no_selection
					: operation_error::not_found, {});
			return false;
		}
		operation_request request;
		request.kind = operation::duplicate;
		request.items = valid;
		request.path_id = current_path_id_;
		return perform_operation(std::move(request));
	}

	bool asset_browser::duplicate_selected()
	{
		return duplicate_items(selected_items_in_order());
	}

	bool asset_browser::move_items(const std::vector<vgui_item_id>& items,
		const std::string& target_path_id)
	{
		if (target_path_id.empty())
		{
			report_operation_failure(operation::move, items,
				operation_error::invalid_target, "no destination given");
			return false;
		}
		if (target_path_id == current_path_id_ && has_location_)
		{
			report_operation_failure(operation::move, items,
				operation_error::invalid_target, "already in this folder");
			return false;
		}
		const std::vector<vgui_item_id> valid = valid_operation_items(items);
		if (valid.empty())
		{
			report_operation_failure(operation::move, items,
				items.empty() ? operation_error::no_selection
					: operation_error::not_found, {});
			return false;
		}
		operation_request request;
		request.kind = operation::move;
		request.items = valid;
		request.path_id = current_path_id_;
		request.target_path_id = target_path_id;
		return perform_operation(std::move(request));
	}

	bool asset_browser::move_selected(const std::string& target_path_id)
	{
		return move_items(selected_items_in_order(), target_path_id);
	}

	bool asset_browser::reimport_items(const std::vector<vgui_item_id>& items)
	{
		const std::vector<vgui_item_id> valid = valid_operation_items(items);
		if (valid.empty())
		{
			report_operation_failure(operation::reimport, items,
				items.empty() ? operation_error::no_selection
					: operation_error::not_found, {});
			return false;
		}
		operation_request request;
		request.kind = operation::reimport;
		request.items = valid;
		request.path_id = current_path_id_;
		return perform_operation(std::move(request));
	}

	bool asset_browser::reimport_selected()
	{
		return reimport_items(selected_items_in_order());
	}

	bool asset_browser::context_menu_enabled() const noexcept
	{
		return context_menu_;
	}

	bool asset_browser::perform_action(action kind, const std::string& argument)
	{
		switch (kind)
		{
		case action::rename:
			return begin_rename();
		case action::duplicate:
			return duplicate_selected();
		case action::remove:
			return confirm_remove_selected();
		case action::move:
			return move_selected(argument);
		case action::reimport:
			return reimport_selected();
		case action::import:
			return request_import();
		case action::dependencies:
			return show_dependencies();
		}
		return false;
	}

	bool asset_browser::begin_rename(vgui_item_id item)
	{
		const vgui_item_id target = item != 0 ? item : primary_;
		if (target == 0)
		{
			report_failure("Rename", {}, operation_error::no_selection, {});
			return false;
		}
		const int index = index_of(target);
		asset_info info;
		if (index < 0 || !item_at(index, &info))
		{
			report_failure("Rename", {}, operation_error::not_found, {});
			return false;
		}
		vgui_context* ctx = list_ ? vgui_widget_get_context(list_)
			: (grid_ ? vgui_widget_get_context(grid_) : nullptr);
		if (!ctx)
			return false;
		auto* state = new rename_prompt_state{ alive_token_, this, target };
		vgui_widget* dialog = vgui_dialog_prompt(ctx, "Rename Asset",
			"Enter a new asset name:", info.name.c_str(),
			&on_rename_prompt_result, state);
		if (!dialog)
		{
			delete state;
			report_failure("Rename", "'" + info.name + "'",
				operation_error::unsupported, "another dialog is open");
			return false;
		}
		return true;
	}

	bool asset_browser::confirm_remove(const std::vector<vgui_item_id>& items)
	{
		const std::vector<vgui_item_id> valid = valid_operation_items(items);
		if (valid.empty())
		{
			report_operation_failure(operation::remove, items,
				items.empty() ? operation_error::no_selection
					: operation_error::not_found, {});
			return false;
		}
		if (!operation_handler_)
		{
			report_operation_failure(operation::remove, valid,
				operation_error::unsupported, "no operation handler installed");
			return false;
		}
		vgui_context* ctx = list_ ? vgui_widget_get_context(list_)
			: (grid_ ? vgui_widget_get_context(grid_) : nullptr);
		if (!ctx)
			return false;

		std::string message;
		if (valid.size() == 1)
		{
			asset_info info;
			const int index = index_of(valid.front());
			if (index >= 0)
				(void)item_at(index, &info);
			message = "Delete '" + info.name + "'?";
		}
		else
			message = "Delete " + std::to_string(valid.size()) + " assets?";
		message += "\nThe asset files are removed from the project. Hosts that "
			"supply undo callbacks can reverse this.";
		auto* state = new remove_confirm_state{ alive_token_, this, valid };
		vgui_widget* dialog = vgui_dialog_confirm(ctx, "Delete Assets",
			message.c_str(), 1, &on_remove_confirm_result, state);
		if (!dialog)
		{
			delete state;
			report_operation_failure(operation::remove, valid,
				operation_error::unsupported, "another dialog is open");
			return false;
		}
		return true;
	}

	bool asset_browser::confirm_remove_selected()
	{
		return confirm_remove(selected_items_in_order());
	}

	bool asset_browser::request_import()
	{
		if (!import_prompt_handler_)
		{
			report_failure("Import", {}, operation_error::unsupported,
				"no import dialog handler installed");
			return false;
		}
		import_prompt_handler_();
		last_error_ = operation_error::none;
		last_undoable_ = false;
		last_message_ = "Import dialog requested";
		return true;
	}

	std::vector<asset_browser::asset_dependency> asset_browser::dependencies(
		vgui_item_id item) const
	{
		const vgui_item_id target = item != 0 ? item : primary_;
		if (!dependency_handler_ || target == 0 || index_of(target) < 0)
			return {};
		return dependency_handler_(target);
	}

	bool asset_browser::show_dependencies(vgui_item_id item)
	{
		const vgui_item_id target = item != 0 ? item : primary_;
		if (target == 0 || index_of(target) < 0)
		{
			report_failure("Show Dependencies", {},
				operation_error::no_selection, {});
			return false;
		}
		if (!dependency_handler_)
		{
			report_failure("Show Dependencies", {},
				operation_error::unsupported, "no dependency handler installed");
			return false;
		}
		asset_info info;
		const int index = index_of(target);
		if (index >= 0)
			(void)item_at(index, &info);

		const std::vector<asset_dependency> list = dependency_handler_(target);
		std::string message;
		if (list.empty())
			message = "No assets depend on '" + info.name + "'.";
		else
		{
			message = "'" + info.name + "' is used by " +
				std::to_string(list.size()) +
				(list.size() == 1 ? " asset:" : " assets:");
			for (const asset_dependency& dep : list)
			{
				message += "\n  ";
				message += dep.name.empty() ? "(unnamed)" : dep.name;
				if (!dep.type.empty())
					message += " [" + dep.type + "]";
				if (!dep.path_id.empty())
					message += "  " + dep.path_id;
			}
		}
		last_error_ = operation_error::none;
		last_undoable_ = false;
		last_message_ = message;

		vgui_context* ctx = list_ ? vgui_widget_get_context(list_)
			: (grid_ ? vgui_widget_get_context(grid_) : nullptr);
		if (!ctx)
			return false;
		return vgui_dialog_alert(ctx, "Dependencies", message.c_str(),
			nullptr, nullptr) != nullptr;
	}

	asset_browser::operation_error asset_browser::last_error() const noexcept
	{
		return last_error_;
	}

	const std::string& asset_browser::last_message() const noexcept
	{
		return last_message_;
	}

	bool asset_browser::last_undoable() const noexcept
	{
		return last_undoable_;
	}

	void asset_browser::enable_context_menu()
	{
		context_menu_ = true;
		vgui_context* ctx = root_ ? vgui_widget_get_context(root_)
			: (list_ ? vgui_widget_get_context(list_) : nullptr);
		if (!ctx)
			return;
		if (!menu_)
		{
			menu_ = vgui_widget_create(ctx, VGUI_MENU, nullptr);
			if (menu_)
			{
				auto* entry = new context_entry{ alive_token_, this };
				vgui_menu_on_activate(menu_, &asset_browser::on_menu_activate_thunk,
					entry);
			}
		}
		if (list_ && !list_event_installed_)
		{
			vgui_widget_on_event(list_, &asset_browser::on_list_event_thunk, this);
			list_event_installed_ = true;
		}
		if (!key_entry_)
		{
			key_entry_ = new context_entry{ alive_token_, this };
			(void)vgui_context_add_key_handler(ctx, &asset_browser::on_key_thunk,
				key_entry_);
		}
	}

	bool asset_browser::open_context_menu(float abs_x, float abs_y)
	{
		if (!menu_)
			return false;
		const std::vector<vgui_item_id> items = selected_items_in_order();
		if (items.empty())
			return false;

		vgui_menu_clear(menu_);
		const bool rename_ok = operation_available(operation::rename) &&
			items.size() == 1;
		const bool ops_ok = operation_handler_ != nullptr;
		const auto add_plain = [this](const char* text, action kind,
			const char* shortcut, bool enabled)
		{
			const vgui_item_id id = vgui_menu_add_item(menu_, text);
			if (id == 0)
				return;
			vgui_menu_set_item_user(menu_, id, menu_tag(kMenuActionBase +
				(std::uintptr_t)kind));
			if (shortcut)
				vgui_menu_set_item_shortcut(menu_, id, shortcut);
			if (!enabled)
				vgui_menu_set_item_enabled(menu_, id, 0);
		};

		add_plain("Rename", action::rename, "F2", rename_ok);
		add_plain("Duplicate", action::duplicate, "Ctrl+D", ops_ok);
		add_plain("Delete", action::remove, "Del", ops_ok);

		move_menu_targets_.clear();
		if (move_targets_handler_)
		{
			const std::vector<asset_move_target> all = move_targets_handler_();
			std::unordered_set<std::string> seen;
			for (const asset_move_target& target : all)
			{
				if (target.path_id.empty() || !seen.insert(target.path_id).second)
					continue;
				if (has_location_ && target.path_id == current_path_id_)
					continue;
				move_menu_targets_.push_back(target);
			}
		}

		vgui_widget* move_menu = nullptr;
		const vgui_item_id move_item = vgui_menu_add_submenu(menu_, "Move To",
			&move_menu);
		const bool move_ok = ops_ok && !move_menu_targets_.empty();
		if (move_item && !move_ok)
			vgui_menu_set_item_enabled(menu_, move_item, 0);
		if (move_menu)
		{
			if (!move_entry_)
				move_entry_ = new context_entry{ alive_token_, this };
			vgui_menu_on_activate(move_menu,
				&asset_browser::on_menu_activate_thunk, move_entry_);
			for (std::size_t i = 0; i < move_menu_targets_.size(); ++i)
			{
				const asset_move_target& target = move_menu_targets_[i];
				const vgui_item_id id = vgui_menu_add_item(move_menu,
					target.label.empty() ? target.path_id.c_str()
						: target.label.c_str());
				if (id)
					vgui_menu_set_item_user(move_menu, id,
						menu_tag(kMenuMoveBase + i));
			}
			if (!move_ok)
			{
				const vgui_item_id id = vgui_menu_add_item(move_menu,
					"(No destinations)");
				if (id)
					vgui_menu_set_item_enabled(move_menu, id, 0);
			}
		}

		vgui_menu_add_separator(menu_);
		add_plain("Import...", action::import, "Ctrl+I",
			import_prompt_handler_ != nullptr);
		add_plain("Reimport", action::reimport, "F5",
			operation_available(operation::reimport));
		vgui_menu_add_separator(menu_);
		add_plain("Show Dependencies", action::dependencies, nullptr,
			dependency_handler_ != nullptr);

		vgui_menu_open(menu_, abs_x, abs_y);
		return true;
	}

	void asset_browser::on_list_event(const vgui_event* event)
	{
		if (!context_menu_ || !event || !list_)
			return;
		if (event->type != VGUI_EVENT_MOUSE_DOWN || event->button != 3 ||
			event->item == 0)
			return;
		if (!is_selected(event->item))
		{
			const int index = index_of(event->item);
			apply_selection({ event->item }, event->item, index, true);
		}
		float x = 0, y = 0, w = 0, h = 0;
		vgui_widget_get_rect(list_, &x, &y, &w, &h);
		(void)w;
		(void)h;
		(void)open_context_menu(x + event->x, y + event->y);
	}

	void asset_browser::on_list_event_thunk(vgui_widget* widget,
		const vgui_event* event, void* user)
	{
		(void)widget;
		if (auto* self = static_cast<asset_browser*>(user))
			self->on_list_event(event);
	}

	void asset_browser::on_menu_activate_thunk(vgui_widget* menu,
		vgui_item_id item, void* user)
	{
		auto* entry = static_cast<context_entry*>(user);
		if (!entry || entry->alive.expired() || !menu || item == 0)
			return;
		asset_browser* self = entry->owner;
		const std::uintptr_t tag = menu_tag_value(
			vgui_menu_get_item_user(menu, item));
		if (tag >= kMenuMoveBase)
		{
			const std::size_t index = (std::size_t)(tag - kMenuMoveBase);
			if (index < self->move_menu_targets_.size())
			{
				(void)self->perform_action(action::move,
					self->move_menu_targets_[index].path_id);
			}
			return;
		}
		if (tag < kMenuActionBase)
			return;
		const std::uintptr_t value = tag - kMenuActionBase;
		if (value > (std::uintptr_t)action::dependencies)
			return;
		(void)self->perform_action((action)value);
	}

	int asset_browser::on_key_thunk(vgui_context* ctx, int key, int down,
		int mods, void* user)
	{
		auto* entry = static_cast<context_entry*>(user);
		if (!entry || entry->alive.expired() || !ctx || !down)
			return 0;
		asset_browser* self = entry->owner;
		if (!self->context_menu_)
			return 0;
		/* Only react while the asset view owns focus. */
		vgui_widget* focus = vgui_get_focus(ctx);
		if (focus != self->list_ && focus != self->grid_)
			return 0;
		if (key == VGUI_KEY_F2)
		{
			(void)self->perform_action(action::rename);
			return 1;
		}
		if (key == VGUI_KEY_DELETE)
		{
			(void)self->perform_action(action::remove);
			return 1;
		}
		if (key == 'D' && (mods & VGUI_MOD_CTRL))
		{
			(void)self->perform_action(action::duplicate);
			return 1;
		}
		if (key == VGUI_KEY_F5)
		{
			(void)self->perform_action(action::reimport);
			return 1;
		}
		if (key == 'I' && (mods & VGUI_MOD_CTRL))
		{
			(void)self->perform_action(action::import);
			return 1;
		}
		return 0;
	}

	bool asset_browser::apply_rename(vgui_item_id item, const std::string& name)
	{
		if (!rename_handler_ || item == 0)
			return false;
		const int index = base_index_of(item);
		if (index < 0)
			return false;
		if (!rename_handler_(item, name))
			return false;
		if (!virtual_mode_)
			items_[(std::size_t)index].name = name;
		refresh();
		sync_list_selection();
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

	void asset_browser::set_import_handler(import_handler handler)
	{
		import_handler_ = std::move(handler);
		update_import_target();
	}

	bool asset_browser::import_files(const std::vector<std::string>& files)
	{
		if (files.empty())
		{
			report_failure("Import", {}, operation_error::no_selection,
				"no files given");
			return false;
		}
		if (!import_handler_)
		{
			report_failure("Import", {}, operation_error::unsupported,
				"no import handler installed");
			return false;
		}
		if (!import_handler_(files, current_path_id_))
		{
			report_failure("Import",
				std::to_string(files.size()) +
					(files.size() == 1 ? " file" : " files"),
				operation_error::io_error, "host rejected the import");
			return false;
		}
		last_error_ = operation_error::none;
		last_undoable_ = false;
		publish_notice("Imported " + std::to_string(files.size()) +
			(files.size() == 1 ? " file" : " files"),
			operation_error::none, false);
		return true;
	}

	void asset_browser::update_import_target()
	{
		if (!root_)
			return;
		if (!import_handler_)
		{
			if (import_target_)
			{
				set_editor_drop_target(root_, nullptr);
				import_target_.reset();
			}
			return;
		}
		if (!import_target_)
		{
			/* A04: OS 文件拖入整个浏览器根;hover 高亮根区域。 */
			class files_drop_target final : public drop_target
			{
			public:
				explicit files_drop_target(asset_browser* owner) : owner_(owner) {}

				bool can_accept(const drag_payload& payload) const override
				{
					return payload.kind == drag_payload_kind::files &&
						!payload.files.empty();
				}

				void hover(const drag_payload& payload, vgui_widget* target,
					float x, float y, vgui_drop_hint* hint) override
				{
					(void)payload;
					(void)x;
					(void)y;
					if (!hint || !target)
						return;
					float rx = 0, ry = 0, rw = 0, rh = 0;
					vgui_widget_get_rect(target, &rx, &ry, &rw, &rh);
					if (rw <= 0.0f || rh <= 0.0f)
						return;
					hint->kind = VGUI_DROP_HINT_RECT;
					hint->x = rx + 1.0f;
					hint->y = ry + 1.0f;
					hint->w = std::max(0.0f, rw - 2.0f);
					hint->h = std::max(0.0f, rh - 2.0f);
				}

				bool drop(const drag_payload& payload, float x, float y) override
				{
					(void)x;
					(void)y;
					if (!owner_)
						return false;
					return owner_->import_files(payload.files);
				}

			private:
				asset_browser* owner_;
			};
			import_target_ = std::make_unique<files_drop_target>(this);
		}
		set_editor_drop_target(root_, import_target_.get());
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

	breadcrumb* asset_browser::path() const noexcept
	{
		return breadcrumb_.get();
	}

	search_box* asset_browser::search() const noexcept
	{
		return search_.get();
	}

	combo_box* asset_browser::type_combo() const noexcept
	{
		return type_combo_.get();
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
			: available_height();
		const bool width_changed = std::fabs(width - last_grid_width_) > 0.5f;
		const bool height_changed = std::fabs(content_h - grid_content_h_) > 0.5f;
		last_grid_width_ = width;
		grid_content_h_ = content_h;
		if (width_changed || height_changed)
			vgui_widget_set_size(grid_, to_logical(grid_, width),
				to_logical(grid_, std::max(1.0f, content_h)));
	}

	asset_browser::grid_metrics asset_browser::metrics_for_width(float width) const
	{
		grid_metrics metrics;
		const float scale = ui_scale_of(scroll_ ? scroll_ : grid_);
		metrics.pad = 8.0f * scale;
		metrics.gap = 8.0f * scale;
		const float avail = std::max(1.0f, width - metrics.pad * 2.0f);
		const float target = 112.0f * scale;
		metrics.columns = std::max(1, (int)std::floor((avail + metrics.gap) / (target + metrics.gap)));
		metrics.cell_w = std::max(1.0f, (avail - (float)(metrics.columns - 1) * metrics.gap) / (float)metrics.columns);
		metrics.cell_h = clampf(metrics.cell_w * 1.16f, 112.0f * scale, 168.0f * scale);
		return metrics;
	}

	float asset_browser::available_width() const
	{
		if (!scroll_)
			return 320.0f * ui_scale_of(grid_);
		float w = 0.0f, h = 0.0f;
		vgui_widget_get_rect(scroll_, nullptr, nullptr, &w, &h);
		(void)h;
		if (w <= 0.0f)
			return 320.0f * ui_scale_of(scroll_);
		const vgui_style style = style_for(scroll_);
		w -= style.panel.scrollbar_width + style.panel.scrollbar_inset;
		return std::max(32.0f * ui_scale_of(scroll_), w);
	}

	float asset_browser::available_height() const
	{
		if (!scroll_)
			return 180.0f * ui_scale_of(grid_);
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
			vgui_custom_draw_text(grid_, cb, vg, empty_state_.c_str(), abs_x, abs_y, abs_w, std::min(abs_h, available_height()), theme.disabled ? theme.disabled : theme.text, 0.5f, 0.5f);
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
		const float scale = ui_scale_of(grid_);
		float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
		cell_rect(metrics, index, &x, &y, &w, &h);
		if (w <= 1.0f || h <= 1.0f)
			return;
		(void)abs_x;
		(void)abs_y;

		const bool selected = is_selected(info.id);
		const bool hovered = hover_index_ == index;
		if (selected)
			ovg_tools::fill_round_rect(cb, vg, x, y, w, h, 5.0f * scale, to_ovg(theme.row_selected ? theme.row_selected : 0x55409EFF));
		else if (hovered)
			ovg_tools::fill_round_rect(cb, vg, x, y, w, h, 5.0f * scale, to_ovg(theme.row_hover ? theme.row_hover : 0x334A4A4A));

		const float thumb_x = x + 5.0f * scale;
		const float thumb_y = y + 5.0f * scale;
		const float thumb_w = std::max(1.0f, w - 10.0f * scale);
		const float thumb_h = std::max(20.0f * scale, h - 36.0f * scale);
		const vgui_color thumb_bg = theme.edit_bg ? theme.edit_bg : theme.track;
		if (thumb_bg)
			ovg_tools::fill_round_rect(cb, vg, thumb_x, thumb_y, thumb_w, thumb_h, 4.0f * scale, to_ovg(thumb_bg));
		if (info.thumbnail)
			vgui_custom_draw_texture(grid_, cb, vg, info.thumbnail, thumb_x, thumb_y, thumb_w, thumb_h, 0xFFFFFFFF);
		else
		{
			/* A05: an asynchronous thumbnail shows its progress instead of the
			 * type fallback.  Failures stay visible until the host retries. */
			const thumbnail_cache::state state = (thumbnails_ && info.id)
				? thumbnails_->state_of(thumbnail_key(info.id))
				: thumbnail_cache::state::missing;
			if (state == thumbnail_cache::state::failed)
			{
				const vgui_color danger = theme.modal_danger
					? theme.modal_danger : 0xFFE06C75;
				vgui_custom_draw_text(grid_, cb, vg, "!",
					thumb_x, thumb_y, thumb_w, thumb_h, danger, 0.5f, 0.5f);
			}
			else if (state == thumbnail_cache::state::queued ||
				state == thumbnail_cache::state::loading)
			{
				const int dots = ((int)(thumbnail_phase_ * 4.0f)) % 4;
				std::string label = state == thumbnail_cache::state::loading
					? "loading" : "queued";
				label.append((std::size_t)dots, '.');
				vgui_custom_draw_text(grid_, cb, vg, label.c_str(),
					thumb_x, thumb_y, thumb_w, thumb_h,
					theme.disabled ? theme.disabled : theme.text, 0.5f, 0.5f);
			}
			else
			{
				vgui_custom_draw_text(grid_, cb, vg, info.type.empty() ? "?" : info.type.c_str(), thumb_x, thumb_y, thumb_w, thumb_h, theme.disabled ? theme.disabled : theme.text, 0.5f, 0.5f);
			}
		}

		const vgui_color text_color = selected ? selected_text_color_of(theme) : text_color_of(theme);
		vgui_custom_draw_text(grid_, cb, vg, info.name.c_str(), x + 6.0f * scale, y + h - 30.0f * scale, std::max(1.0f, w - 12.0f * scale), 16.0f * scale, text_color, 0.5f, 0.5f);
		if (!info.type.empty())
			vgui_custom_draw_text(grid_, cb, vg, info.type.c_str(), x + 6.0f * scale, y + h - 15.0f * scale, std::max(1.0f, w - 12.0f * scale), 12.0f * scale, theme.disabled ? theme.disabled : text_color_of(theme), 0.5f, 0.5f);

		if (selected)
			ovg_tools::stroke_round_rect(cb, vg, x, y, w, h, 5.0f * scale, to_ovg(theme.accent ? theme.accent : 0xFF409EFF), 1.5f);
		else if (hovered)
			ovg_tools::stroke_round_rect(cb, vg, x, y, w, h, 5.0f * scale, to_ovg(theme.panel_border), 1.0f);
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
				if (!info.thumbnail && thumbnails_ && info.id)
				{
					const std::string key = thumbnail_key(info.id);
					const thumbnail_cache::state state = thumbnails_->state_of(key);
					if (state == thumbnail_cache::state::queued)
						tooltip_ += " - thumbnail queued";
					else if (state == thumbnail_cache::state::loading)
						tooltip_ += " - loading thumbnail";
					else if (state == thumbnail_cache::state::failed)
					{
						tooltip_ += " - thumbnail failed";
						const std::string& error = thumbnails_->error(key);
						if (!error.empty())
						{
							tooltip_ += ": ";
							tooltip_ += error;
						}
					}
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
		{
			// A cell taller than the viewport can never be fully visible;
			// aligning to its bottom would hide the top edge, so pin it to
			// the top instead (also avoids scrolling on initial selection).
			scroll = metrics.cell_h >= view_h ? top : bottom - view_h;
		}
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
		vgui_list_set_cursor(list_, cursor_ && index_of(cursor_) >= 0 ? cursor_ : 0);
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
		if (selection_listeners_.empty())
			return;
		/* Copy the callbacks so listeners may add or remove listeners while
		 * being notified without invalidating this iteration. */
		std::vector<selection_listener> listeners;
		listeners.reserve(selection_listeners_.size());
		for (const selection_listener_entry& entry : selection_listeners_)
			listeners.push_back(entry.fn);
		for (const selection_listener& listener : listeners)
		{
			if (listener)
				listener(primary_);
		}
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

	void asset_browser::on_activate(vgui_widget* widget, vgui_item_id item, void* user)
	{
		(void)widget;
		if (!user || !item)
			return;
		auto* self = static_cast<asset_browser*>(user);
		if (self->activate_listener_)
			self->activate_listener_(item);
	}

	void asset_browser::on_tick_draw(vgui_widget* widget, ovg_ctx_cb* cb, rvg_t* vg, float abs_x, float abs_y, float abs_w, float abs_h, float delta_time, void* user)
	{
		(void)widget;
		(void)cb;
		(void)vg;
		(void)abs_x;
		(void)abs_y;
		(void)abs_w;
		(void)abs_h;
		if (user)
			static_cast<asset_browser*>(user)->tick(delta_time);
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
			if (event->button == 3)
			{
				const int index = self->grid_index_at(event->x, event->y);
				if (index < 0 || !self->context_menu_)
					return 0;
				const vgui_item_id item = self->id_at(index);
				if (item == 0)
					return 0;
				if (!self->is_selected(item))
					self->apply_selection({ item }, item, index, true);
				float x = 0, y = 0, w = 0, h = 0;
				vgui_widget_get_rect(widget, &x, &y, &w, &h);
				(void)w;
				(void)h;
				(void)self->open_context_menu(x + event->x, y + event->y);
				return 1;
			}
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
			if (event->clicks >= 2)
			{
				self->finish_grid_drag();
				if (self->activate_listener_)
					self->activate_listener_(info.id);
				return 1;
			}
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
			if (event->key == VGUI_KEY_ENTER)
			{
				self->activate_selected();
				return 1;
			}
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
		if (!self->scratch_info_.thumbnail && self->thumbnails_ &&
			self->scratch_info_.id)
		{
			const std::string key = self->thumbnail_key(self->scratch_info_.id);
			const thumbnail_cache::state state = self->thumbnails_->state_of(key);
			if (state == thumbnail_cache::state::queued)
				self->scratch_tooltip_ += " - thumbnail queued";
			else if (state == thumbnail_cache::state::loading)
				self->scratch_tooltip_ += " - loading thumbnail";
			else if (state == thumbnail_cache::state::failed)
			{
				self->scratch_tooltip_ += " - thumbnail failed";
				const std::string& error = self->thumbnails_->error(key);
				if (!error.empty())
				{
					self->scratch_tooltip_ += ": ";
					self->scratch_tooltip_ += error;
				}
			}
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
