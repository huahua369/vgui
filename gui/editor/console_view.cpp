#include "console_view.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <memory>
#include <utility>

namespace vgui_editor_kit {

	/* Context-level key handlers have no destroy callback in the C API.  Keep
	 * the registration block alive and let the weak token fence callbacks that
	 * arrive after the view is gone, matching outliner_view/drag_bridge. */
	struct console_view::context_entry
	{
		std::weak_ptr<int> alive;
		console_view* owner = nullptr;
	};

	namespace {
		constexpr vgui_color kFallbackText = 0xFFD8DEE9;
		constexpr vgui_color kFallbackMuted = 0xFF8A93A3;
		constexpr vgui_color kDefaultLevelColors[console_view::level_count] = {
			0xFF7F8A99,	/* trace */
			0xFFA9B6C8,	/* debug */
			0xFFD8DEE9,	/* info */
			0xFFE5B567,	/* warning */
			0xFFE06C75	/* error */
		};
		const char* const kLevelNames[console_view::level_count] = {
			"trace", "debug", "info", "warning", "error"
		};
		const char* const kLevelTags[console_view::level_count] = {
			"T", "D", "I", "W", "E"
		};

		vgui_theme theme_for(vgui_widget* widget)
		{
			vgui_theme theme = {};
			vgui_context* ctx = widget ? vgui_widget_get_context(widget) : nullptr;
			if (ctx)
				vgui_context_get_theme(ctx, &theme);
			if (!theme.text)
				theme.text = kFallbackText;
			return theme;
		}

		vgui_color muted_color(const vgui_theme& theme)
		{
			return theme.disabled ? theme.disabled : kFallbackMuted;
		}

		bool valid_level(console_view::level value)
		{
			const int index = static_cast<int>(value);
			return index >= 0 && index < console_view::level_count;
		}

		bool copy_to_clipboard(vgui_widget* sink, const std::string& text)
		{
			if (!sink || text.empty())
				return false;
			vgui_edit_set_text(sink, text.c_str());
			vgui_edit_select_all(sink);
			return vgui_edit_copy(sink) != 0;
		}

	}

	const char* console_view::level_name(level value) noexcept
	{
		const int index = static_cast<int>(value);
		return index >= 0 && index < level_count ? kLevelNames[index] : "";
	}

	console_view::console_view()
	{
		alive_token_ = std::make_shared<int>(0);
		for (int i = 0; i < level_count; ++i)
		{
			level_colors_[i] = kDefaultLevelColors[i];
			level_color_set_[i] = true;
		}
	}

	console_view::~console_view()
	{
		if (context_ && key_handler_token_)
			vgui_context_remove_key_handler(context_, key_handler_token_);
		context_ = nullptr;
		key_handler_token_ = 0;
		delete key_entry_;
		key_entry_ = nullptr;
		if (alive_token_)
			alive_token_.reset();
	}

	//----------------------------------------------------------------------------
	// construction
	//----------------------------------------------------------------------------

	void console_view::build(vgui_widget* parent)
	{
		if (root_ || !parent)
			return;

		if (!alive_token_)
			alive_token_ = std::make_shared<int>(0);

		context_ = vgui_widget_get_context(parent);
		if (context_)
		{
			key_entry_ = new context_entry{ alive_token_, this };
			key_handler_token_ = vgui_context_add_key_handler(context_,
				&console_view::on_key, key_entry_);
			if (key_handler_token_ <= 0)
			{
				delete key_entry_;
				key_entry_ = nullptr;
			}
		}

		root_ = vgui_widget_add(parent, VGUI_PANEL, nullptr);
		vgui_widget_set_id(root_, "console_view");
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

		header_ = vgui_widget_add(root_, VGUI_PANEL, nullptr);
		vgui_widget_set_layout(header_, VGUI_LAYOUT_ROW);
		vgui_widget_set_align_items(header_, VGUI_ALIGN_CENTER);
		vgui_widget_set_padding(header_, 0, 0, 0, 0);
		vgui_widget_set_gap(header_, 4);
		vgui_widget_set_size(header_, 0, 26);
		vgui_widget_set_min_size(header_, 0, 24);
		vgui_widget_set_background(header_, 0);
		vgui_widget_set_border(header_, 0, 0);

		for (int i = 0; i < level_count; ++i)
		{
			vgui_widget* widget = vgui_widget_add(header_, VGUI_CHECKBOX,
				kLevelTags[i]);
			vgui_widget_set_id(widget, (std::string("console_level_") +
				kLevelNames[i]).c_str());
			vgui_widget_set_size(widget, 26, 22);
			vgui_widget_set_min_size(widget, 22, 22);
			vgui_widget_set_font_size(widget, 11);
			vgui_widget_set_checked(widget, 1);
			vgui_widget_on_value(widget, &console_view::on_level_value, this);
			const std::string tip = std::string("Show ") + kLevelNames[i] +
				" messages";
			vgui_widget_set_tooltip(widget, tip.c_str());
			level_widgets_[(std::size_t)i] = widget;
		}

		search_.build(header_, "Filter log");
		vgui_widget_set_id(search_.widget(), "console_search");
		vgui_widget_set_grow(search_.widget(), 1.0f);
		vgui_widget_set_min_size(search_.widget(), 120, 24);
		search_.set_on_change([this](const std::string& text)
		{
			set_search_text(text);
		});

		auto_scroll_widget_ = vgui_widget_add(header_, VGUI_CHECKBOX,
			"Auto scroll");
		vgui_widget_set_id(auto_scroll_widget_, "console_auto_scroll");
		vgui_widget_set_size(auto_scroll_widget_, 96, 22);
		vgui_widget_set_min_size(auto_scroll_widget_, 86, 22);
		vgui_widget_set_font_size(auto_scroll_widget_, 11);
		vgui_widget_set_checked(auto_scroll_widget_, auto_scroll_ ? 1 : 0);
		vgui_widget_set_tooltip(auto_scroll_widget_,
			"Follow the newest log entry");
		vgui_widget_on_value(auto_scroll_widget_,
			&console_view::on_auto_scroll_value, this);

		copy_button_.build(header_, "Copy");
		vgui_widget_set_id(copy_button_.widget(), "console_copy");
		copy_button_.set_tooltip("Copy selected rows (or all visible rows)");
		copy_button_.set_on_click([this]() { (void)copy_selection(); });

		clear_button_.build(header_, "Clear");
		vgui_widget_set_id(clear_button_.widget(), "console_clear");
		clear_button_.set_tooltip("Clear the console");
		clear_button_.set_on_click([this]() { clear(); });

		stats_label_ = vgui_widget_add(header_, VGUI_LABEL, "");
		vgui_widget_set_id(stats_label_, "console_stats");
		vgui_widget_set_font_size(stats_label_, 11);
		vgui_widget_set_min_size(stats_label_, 0, 18);

		/* 2x2 transparent driver: drains the producer queue during draw. */
		ticker_ = vgui_widget_add(header_, VGUI_CUSTOM, nullptr);
		vgui_widget_set_size(ticker_, 2, 2);
		vgui_widget_set_min_size(ticker_, 1, 1);
		vgui_widget_set_background(ticker_, 0);
		vgui_widget_set_border(ticker_, 0, 0);
		vgui_custom_set_draw(ticker_, &console_view::on_tick_draw, this);

		tree_ = vgui_widget_add(root_, VGUI_TREE, nullptr);
		vgui_widget_set_id(tree_, "console_tree");
		vgui_widget_set_grow(tree_, 1.0f);
		vgui_widget_set_min_size(tree_, 160, 64);
		vgui_widget_set_size(tree_, 0, 0);
		vgui_widget_set_focusable(tree_, 1);
		vgui_widget_set_font_size(tree_, 12);
		vgui_tree_set_selection_mode(tree_, VGUI_SELECTION_MULTIPLE);
		vgui_tree_set_empty_text(tree_, "No messages.");
		vgui_tree_on_activate(tree_, &console_view::on_tree_activate, this);

		vgui_row_source source = {};
		source.user = this;
		source.row_count = &console_view::row_count_fn;
		source.row_info = &console_view::row_info_fn;
		source.row_id = &console_view::row_id_fn;
		source.row_id_index = &console_view::row_id_index_fn;
		source.row_depth = &console_view::row_depth_fn;
		source.row_has_children = &console_view::row_has_children_fn;
		source.row_expanded = &console_view::row_expanded_fn;
		source.row_toggle = &console_view::row_toggle_fn;
		source.row_enabled = &console_view::row_enabled_fn;
		source.row_tooltip = &console_view::row_tooltip_fn;
		source.row_text_color = &console_view::row_text_color_fn;
		vgui_tree_set_row_source(tree_, &source);

		/* Hidden read-only edit used purely to reach vgui_edit_copy(); the
		 * system clipboard is owned by the backend, not the kit. */
		copy_sink_ = vgui_widget_add(root_, VGUI_EDIT, "");
		vgui_widget_set_id(copy_sink_, "console_copy_sink");
		vgui_widget_set_visible(copy_sink_, 0);
		vgui_widget_set_focusable(copy_sink_, 0);
		vgui_edit_set_multiline(copy_sink_, 1);
		vgui_edit_set_readonly(copy_sink_, 1);

		apply_theme_colors();
		update_stats();
		vgui_tree_source_changed(tree_);
		if (auto_scroll_)
			scroll_to_latest();
	}

	bool console_view::built() const noexcept
	{
		return root_ != nullptr;
	}

	void console_view::apply_theme_colors()
	{
		if (!root_)
			return;
		const vgui_theme theme = theme_for(root_);
		vgui_widget_set_text_color(stats_label_, muted_color(theme));
		for (int i = 0; i < level_count; ++i)
		{
			vgui_widget* widget = level_widgets_[(std::size_t)i];
			if (!widget)
				continue;
			vgui_widget_set_text_color(widget,
				level_color_set_[i] ? level_colors_[i] : theme.text);
		}
	}

	//----------------------------------------------------------------------------
	// producers / draining
	//----------------------------------------------------------------------------

	vgui_item_id console_view::append(level severity, std::string message,
		std::string source_name)
	{
		entry value;
		value.severity = severity;
		value.message = std::move(message);
		value.source = std::move(source_name);
		return append(std::move(value));
	}

	vgui_item_id console_view::append(entry value)
	{
		const vgui_item_id id = static_cast<vgui_item_id>(
			next_id_.fetch_add(1, std::memory_order_relaxed));
		{
			std::lock_guard<std::mutex> lock(pending_mutex_);
			pending_.push_back(queued_entry{ id, std::move(value) });
			while (pending_.size() > max_entries_)
			{
				pending_.pop_front();
				dropped_count_.fetch_add(1, std::memory_order_relaxed);
			}
		}
		return id;
	}

	std::vector<vgui_item_id> console_view::append_batch(
		std::vector<entry> values)
	{
		std::vector<vgui_item_id> ids;
		ids.reserve(values.size());
		std::lock_guard<std::mutex> lock(pending_mutex_);
		for (entry& value : values)
		{
			const vgui_item_id id = static_cast<vgui_item_id>(
				next_id_.fetch_add(1, std::memory_order_relaxed));
			ids.push_back(id);
			pending_.push_back(queued_entry{ id, std::move(value) });
		}
		while (pending_.size() > max_entries_)
		{
			pending_.pop_front();
			dropped_count_.fetch_add(1, std::memory_order_relaxed);
		}
		return ids;
	}

	std::size_t console_view::poll()
	{
		std::size_t batch = drain_batch_;
		if (batch == 0)
			batch = 1;

		std::deque<queued_entry> drained;
		{
			std::lock_guard<std::mutex> lock(pending_mutex_);
			while (!pending_.empty() && drained.size() < batch)
			{
				drained.push_back(std::move(pending_.front()));
				pending_.pop_front();
			}
		}
		if (drained.empty())
			return 0;

		for (queued_entry& item : drained)
			append_record(std::move(item));

		publish_model_change();
		if (auto_scroll_)
			scroll_to_latest();
		return drained.size();
	}

	void console_view::tick(float delta_time)
	{
		(void)delta_time;
		(void)poll();
	}

	bool console_view::idle() const
	{
		std::lock_guard<std::mutex> lock(pending_mutex_);
		return pending_.empty();
	}

	std::size_t console_view::pending_count() const
	{
		std::lock_guard<std::mutex> lock(pending_mutex_);
		return pending_.size();
	}

	std::size_t console_view::entry_count() const noexcept
	{
		return entries_.size();
	}

	std::size_t console_view::visible_entry_count() const noexcept
	{
		return visible_entries_;
	}

	std::size_t console_view::visible_row_count() const noexcept
	{
		return rows_.size();
	}

	std::size_t console_view::dropped_count() const noexcept
	{
		return dropped_count_.load(std::memory_order_relaxed);
	}

	vgui_item_id console_view::last_entry_id() const noexcept
	{
		return entries_.empty() ? 0 : entries_.back().id;
	}

	bool console_view::entry_at(vgui_item_id id, entry* out) const
	{
		const record* found = find_record(id);
		if (!found)
			return false;
		if (out)
			*out = found->value;
		return true;
	}

	void console_view::clear()
	{
		{
			std::lock_guard<std::mutex> lock(pending_mutex_);
			pending_.clear();
		}
		entries_.clear();
		entry_index_.clear();
		rows_.clear();
		row_index_.clear();
		row_base_ = 0;
		visible_entries_ = 0;
		expanded_.clear();
		dropped_count_.store(0, std::memory_order_relaxed);
		if (tree_)
			vgui_tree_clear_selection(tree_);
		publish_model_change();
		if (auto_scroll_)
			scroll_to_latest();
	}

	void console_view::set_max_entries(std::size_t max_entries)
	{
		if (max_entries == 0)
			max_entries = 1;
		{
			std::lock_guard<std::mutex> lock(pending_mutex_);
			max_entries_ = max_entries;
			while (pending_.size() > max_entries_)
			{
				pending_.pop_front();
				dropped_count_.fetch_add(1, std::memory_order_relaxed);
			}
		}
		if (trim_entries(max_entries))
			publish_model_change();
		else
			update_stats();
	}

	std::size_t console_view::max_entries() const
	{
		std::lock_guard<std::mutex> lock(pending_mutex_);
		return max_entries_;
	}

	void console_view::set_drain_batch(std::size_t count)
	{
		drain_batch_ = count == 0 ? 1 : count;
	}

	std::size_t console_view::drain_batch() const noexcept
	{
		return drain_batch_;
	}

	//----------------------------------------------------------------------------
	// filtering / expansion
	//----------------------------------------------------------------------------

	void console_view::set_level_enabled(level value, bool enabled)
	{
		const int index = static_cast<int>(value);
		if (index < 0 || index >= level_count ||
			level_enabled_[(std::size_t)index] == enabled)
			return;
		level_enabled_[(std::size_t)index] = enabled;
		if (vgui_widget* widget = level_widgets_[(std::size_t)index])
			vgui_widget_set_checked(widget, enabled ? 1 : 0);
		rebuild_rows();
		publish_model_change();
	}

	bool console_view::level_enabled(level value) const noexcept
	{
		const int index = static_cast<int>(value);
		return index >= 0 && index < level_count &&
			level_enabled_[(std::size_t)index];
	}

	void console_view::set_search_text(std::string text)
	{
		if (search_text_ == text)
			return;
		search_text_ = std::move(text);
		search_lower_ = lower_ascii(search_text_);
		if (search_.widget() && search_.text() != search_text_)
			search_.set_text(search_text_);
		rebuild_rows();
		publish_model_change();
	}

	const std::string& console_view::search_text() const noexcept
	{
		return search_text_;
	}

	bool console_view::filter_active() const noexcept
	{
		if (!search_text_.empty())
			return true;
		for (bool enabled : level_enabled_)
		{
			if (!enabled)
				return true;
		}
		return false;
	}

	void console_view::set_auto_scroll(bool enabled)
	{
		if (auto_scroll_ == enabled)
			return;
		auto_scroll_ = enabled;
		if (auto_scroll_widget_)
			vgui_widget_set_checked(auto_scroll_widget_, enabled ? 1 : 0);
		if (enabled)
			scroll_to_latest();
	}

	bool console_view::auto_scroll() const noexcept
	{
		return auto_scroll_;
	}

	void console_view::scroll_to_latest()
	{
		if (!tree_ || rows_.empty())
			return;
		vgui_tree_scroll_to_node(tree_, rows_.back().id, VGUI_SCROLL_ALIGN_BOTTOM);
	}

	bool console_view::stack_expanded(vgui_item_id entry_id) const noexcept
	{
		return expanded_.find(entry_id) != expanded_.end();
	}

	bool console_view::set_stack_expanded(vgui_item_id entry_id, bool expanded)
	{
		const record* found = find_record(entry_id);
		if (!found || found->value.stack.empty())
			return false;
		const bool was = stack_expanded(entry_id);
		if (was == expanded)
			return true;
		if (expanded)
			expanded_.insert(entry_id);
		else
			expanded_.erase(entry_id);
		rebuild_rows();
		publish_model_change();
		return true;
	}

	bool console_view::toggle_stack(vgui_item_id entry_id)
	{
		const record* found = find_record(entry_id);
		if (!found || found->value.stack.empty())
			return false;
		if (expanded_.erase(entry_id) == 0)
			expanded_.insert(entry_id);
		rebuild_rows();
		publish_model_change();
		return true;
	}

	//----------------------------------------------------------------------------
	// copy / jump
	//----------------------------------------------------------------------------

	std::string console_view::selected_text() const
	{
		if (!tree_)
			return {};
		std::string out;
		bool first = true;
		for (const row_ref& row : rows_)
		{
			if (!vgui_tree_is_selected(tree_, row.id))
				continue;
			const std::string line = row_text(row);
			if (line.empty())
				continue;
			if (!first)
				out.push_back('\n');
			out += line;
			first = false;
		}
		return out;
	}

	std::string console_view::visible_text() const
	{
		std::string out;
		bool first = true;
		for (const row_ref& row : rows_)
		{
			const std::string line = row_text(row);
			if (line.empty())
				continue;
			if (!first)
				out.push_back('\n');
			out += line;
			first = false;
		}
		return out;
	}

	bool console_view::copy_selection()
	{
		std::string text = selected_text();
		if (text.empty())
			text = visible_text();
		return copy_to_clipboard(copy_sink_, text);
	}

	bool console_view::activate_row(vgui_item_id row_id)
	{
		const row_ref* row = find_row(row_id);
		if (!row)
			return false;
		const record* found = find_record(row->entry_id);
		if (!found)
			return false;

		location target;
		bool try_jump = false;
		if (row->frame >= 0)
		{
			if ((std::size_t)row->frame < found->value.stack.size())
			{
				target = found->value.stack[(std::size_t)row->frame].where;
				try_jump = target.valid();
			}
			if (!try_jump && found->value.where.valid())
			{
				target = found->value.where;
				try_jump = true;
			}
		}
		else
		{
			if (found->value.where.valid())
			{
				target = found->value.where;
				try_jump = true;
			}
			if (!try_jump)
			{
				for (const stack_frame& frame : found->value.stack)
				{
					if (frame.where.valid())
					{
						target = frame.where;
						try_jump = true;
						break;
					}
				}
			}
		}

		if (try_jump && jump_handler_)
			return jump_handler_(target);
		if (row->frame < 0 && !found->value.stack.empty())
			return toggle_stack(row->entry_id);
		return false;
	}

	bool console_view::activate_selected()
	{
		if (!tree_)
			return false;
		for (const row_ref& row : rows_)
		{
			if (vgui_tree_is_selected(tree_, row.id))
				return activate_row(row.id);
		}
		const vgui_item_id cursor = vgui_tree_get_cursor(tree_);
		return cursor ? activate_row(cursor) : false;
	}

	void console_view::set_jump_handler(jump_handler handler)
	{
		jump_handler_ = std::move(handler);
	}

	void console_view::set_level_color(level value, vgui_color color)
	{
		const int index = static_cast<int>(value);
		if (index < 0 || index >= level_count)
			return;
		level_colors_[(std::size_t)index] = color;
		level_color_set_[(std::size_t)index] = color != 0;
		if (vgui_widget* widget = level_widgets_[(std::size_t)index])
		{
			const vgui_theme theme = theme_for(root_);
			vgui_widget_set_text_color(widget,
				color ? color : theme.text);
		}
		if (tree_)
			vgui_tree_source_rows_changed(tree_, 0,
				static_cast<int>(rows_.size()));
	}

	vgui_color console_view::level_color(level value) const noexcept
	{
		const int index = static_cast<int>(value);
		return index >= 0 && index < level_count &&
			level_color_set_[(std::size_t)index]
			? level_colors_[(std::size_t)index] : 0;
	}

	//----------------------------------------------------------------------------
	// indexes / accessors
	//----------------------------------------------------------------------------

	vgui_item_id console_view::row_id_at(int visible_index) const
	{
		if (visible_index < 0 || (std::size_t)visible_index >= rows_.size())
			return 0;
		return rows_[(std::size_t)visible_index].id;
	}

	bool console_view::row_entry(vgui_item_id row_id, vgui_item_id* entry_id,
		int* frame) const
	{
		const row_ref* row = find_row(row_id);
		if (!row)
			return false;
		if (entry_id)
			*entry_id = row->entry_id;
		if (frame)
			*frame = row->frame;
		return true;
	}

	vgui_widget* console_view::widget() const noexcept
	{
		return root_;
	}

	vgui_widget* console_view::tree() const noexcept
	{
		return tree_;
	}

	vgui_widget* console_view::ticker() const noexcept
	{
		return ticker_;
	}

	search_box& console_view::search() noexcept
	{
		return search_;
	}

	const search_box& console_view::search() const noexcept
	{
		return search_;
	}

	vgui_widget* console_view::auto_scroll_widget() const noexcept
	{
		return auto_scroll_widget_;
	}

	vgui_widget* console_view::level_widget(level value) const noexcept
	{
		const int index = static_cast<int>(value);
		return index >= 0 && index < level_count
			? level_widgets_[(std::size_t)index] : nullptr;
	}

	vgui_widget* console_view::copy_widget() const noexcept
	{
		return copy_button_.widget();
	}

	vgui_widget* console_view::clear_widget() const noexcept
	{
		return clear_button_.widget();
	}

	//----------------------------------------------------------------------------
	// model helpers
	//----------------------------------------------------------------------------

	std::uint64_t console_view::encode_row(vgui_item_id entry_id,
		int frame) noexcept
	{
		const std::uint64_t tag = frame >= 0
			? static_cast<std::uint64_t>(frame) + 1u : 0u;
		return (entry_id << 16) | (tag & 0xFFFFu);
	}

	bool console_view::decode_row(vgui_item_id row_id, vgui_item_id* entry_id,
		int* frame) noexcept
	{
		const vgui_item_id id = row_id >> 16;
		if (id == 0)
			return false;
		const std::uint64_t tag = row_id & 0xFFFFu;
		if (entry_id)
			*entry_id = id;
		if (frame)
			*frame = tag ? static_cast<int>(tag - 1u) : -1;
		return true;
	}

	void console_view::append_record(queued_entry value)
	{
		record next;
		next.id = value.id;
		next.value = std::move(value.value);
		next.display = format_entry(next.value);
		next.frame_display.reserve(next.value.stack.size());
		for (const stack_frame& frame : next.value.stack)
			next.frame_display.push_back(format_frame(frame));
		next.visible = matches(next.value);

		entries_.push_back(std::move(next));
		record& stored = entries_.back();
		entry_index_[stored.id] = &stored;
		if (stored.visible)
		{
			++visible_entries_;
			append_rows_for(stored);
		}
		(void)trim_entries(max_entries());
	}

	bool console_view::trim_entries(std::size_t limit)
	{
		bool changed = false;
		while (entries_.size() > limit)
		{
			record& front = entries_.front();
			if (front.visible)
				--visible_entries_;
			while (!rows_.empty() && rows_.front().entry_id == front.id)
			{
				const vgui_item_id row_id = rows_.front().id;
				if (tree_ && vgui_tree_is_selected(tree_, row_id))
					vgui_tree_select(tree_, row_id, 0);
				row_index_.erase(row_id);
				rows_.pop_front();
				++row_base_;
			}
			expanded_.erase(front.id);
			entry_index_.erase(front.id);
			entries_.pop_front();
			dropped_count_.fetch_add(1, std::memory_order_relaxed);
			changed = true;
		}
		return changed;
	}

	void console_view::rebuild_rows()
	{
		rows_.clear();
		row_index_.clear();
		row_base_ = 0;
		visible_entries_ = 0;
		for (record& item : entries_)
		{
			item.visible = matches(item.value);
			if (!item.visible)
				continue;
			++visible_entries_;
			append_rows_for(item);
		}
	}

	void console_view::append_rows_for(const record& value)
	{
		add_row(value, -1);
		if (expanded_.find(value.id) == expanded_.end())
			return;
		for (std::size_t i = 0; i < value.value.stack.size(); ++i)
			add_row(value, static_cast<int>(i));
	}

	void console_view::add_row(const record& value, int frame)
	{
		row_ref row;
		row.id = encode_row(value.id, frame);
		row.entry_id = value.id;
		row.frame = frame;
		row_index_[row.id] = row_base_ + rows_.size();
		rows_.push_back(row);
	}

	bool console_view::matches(const entry& value) const
	{
		const int index = static_cast<int>(value.severity);
		if (index < 0 || index >= level_count ||
			!level_enabled_[(std::size_t)index])
			return false;
		if (search_lower_.empty())
			return true;
		if (contains_ci(value.message, search_lower_))
			return true;
		if (contains_ci(value.source, search_lower_))
			return true;
		if (contains_ci(value.timestamp, search_lower_))
			return true;
		if (contains_ci(level_name(value.severity), search_lower_))
			return true;
		if (contains_ci(value.where.path, search_lower_))
			return true;
		for (const stack_frame& frame : value.stack)
		{
			if (contains_ci(frame.function, search_lower_) ||
				contains_ci(frame.where.path, search_lower_))
				return true;
		}
		return false;
	}

	const console_view::record* console_view::find_record(
		vgui_item_id entry_id) const
	{
		const auto it = entry_index_.find(entry_id);
		if (it == entry_index_.end() || !it->second)
			return nullptr;
		return it->second;
	}

	const console_view::row_ref* console_view::find_row(
		vgui_item_id row_id) const
	{
		const auto it = row_index_.find(row_id);
		if (it == row_index_.end() || it->second < row_base_)
			return nullptr;
		const std::size_t index = it->second - row_base_;
		if (index >= rows_.size() || rows_[index].id != row_id)
			return nullptr;
		return &rows_[index];
	}

	std::string console_view::row_text(const row_ref& row) const
	{
		const record* found = find_record(row.entry_id);
		if (!found)
			return {};
		if (row.frame >= 0)
		{
			if ((std::size_t)row.frame >= found->frame_display.size())
				return {};
			return found->frame_display[(std::size_t)row.frame];
		}
		return found->display;
	}

	void console_view::publish_model_change()
	{
		if (tree_)
			vgui_tree_source_changed(tree_);
		update_stats();
	}

	void console_view::update_stats()
	{
		if (!stats_label_)
			return;
		std::string text = std::to_string(visible_entries_) + " shown / " +
			std::to_string(entries_.size());
		const std::size_t dropped = dropped_count();
		if (dropped > 0)
			text += "  ·  " + std::to_string(dropped) + " dropped";
		vgui_widget_set_text(stats_label_, text.c_str());
	}

	//----------------------------------------------------------------------------
	// text formatting
	//----------------------------------------------------------------------------

	bool console_view::contains_ci(const std::string& text,
		const std::string& lower_needle)
	{
		if (lower_needle.empty())
			return true;
		return lower_ascii(text).find(lower_needle) != std::string::npos;
	}

	std::string console_view::lower_ascii(std::string text)
	{
		for (char& ch : text)
		{
			const unsigned char value = static_cast<unsigned char>(ch);
			if (value < 128)
				ch = static_cast<char>(std::tolower(value));
		}
		return text;
	}

	std::string console_view::single_line(std::string text)
	{
		for (char& ch : text)
		{
			if (ch == '\r' || ch == '\n' || ch == '\t')
				ch = ' ';
		}
		while (!text.empty() &&
			(text.back() == ' ' || text.back() == '\r' || text.back() == '\n'))
			text.pop_back();
		return text;
	}

	std::string console_view::format_entry(const entry& value)
	{
		std::string out;
		if (!value.timestamp.empty())
		{
			out += value.timestamp;
			out.push_back(' ');
		}
		if (!value.source.empty())
		{
			out.push_back('[');
			out += value.source;
			out += "] ";
		}
		out += single_line(value.message);
		return out;
	}

	std::string console_view::format_frame(const stack_frame& value)
	{
		std::string out = value.function.empty() ? "<anonymous>"
			: value.function;
		if (!value.where.path.empty())
		{
			out += "  ";
			out += value.where.path;
			if (value.where.line > 0)
			{
				out.push_back(':');
				out += std::to_string(value.where.line);
				if (value.where.column > 0)
				{
					out.push_back(':');
					out += std::to_string(value.where.column);
				}
			}
		}
		else if (value.where.asset_id != 0)
		{
			out += "  asset #";
			out += std::to_string(value.where.asset_id);
		}
		return single_line(std::move(out));
	}

	//----------------------------------------------------------------------------
	// virtual row source callbacks
	//----------------------------------------------------------------------------

	int console_view::row_count_fn(void* user)
	{
		const auto* self = static_cast<const console_view*>(user);
		if (!self)
			return 0;
		return static_cast<int>(std::min<std::size_t>(self->rows_.size(),
			static_cast<std::size_t>(std::numeric_limits<int>::max())));
	}

	int console_view::row_info_fn(void* user, int index, const char** text,
		const char** detail, void** item_user)
	{
		const auto* self = static_cast<const console_view*>(user);
		if (!self || index < 0 || (std::size_t)index >= self->rows_.size())
			return 0;
		const row_ref& row = self->rows_[(std::size_t)index];
		const record* found = self->find_record(row.entry_id);
		if (!found)
			return 0;
		if (text)
		{
			if (row.frame >= 0)
			{
				*text = (std::size_t)row.frame < found->frame_display.size()
					? found->frame_display[(std::size_t)row.frame].c_str() : "";
			}
			else
				*text = found->display.c_str();
		}
		if (detail)
			*detail = nullptr;
		if (item_user)
			*item_user = reinterpret_cast<void*>(
				static_cast<std::uintptr_t>(row.id));
		return 1;
	}

	vgui_item_id console_view::row_id_fn(void* user, int index)
	{
		const auto* self = static_cast<const console_view*>(user);
		if (!self || index < 0 || (std::size_t)index >= self->rows_.size())
			return 0;
		return self->rows_[(std::size_t)index].id;
	}

	int console_view::row_id_index_fn(void* user, vgui_item_id item)
	{
		const auto* self = static_cast<const console_view*>(user);
		if (!self)
			return -1;
		const auto it = self->row_index_.find(item);
		if (it == self->row_index_.end() || it->second < self->row_base_)
			return -1;
		const std::size_t index = it->second - self->row_base_;
		if (index >= self->rows_.size() || self->rows_[index].id != item)
			return -1;
		return static_cast<int>(index);
	}

	int console_view::row_depth_fn(void* user, int index)
	{
		const auto* self = static_cast<const console_view*>(user);
		if (!self || index < 0 || (std::size_t)index >= self->rows_.size())
			return 0;
		return self->rows_[(std::size_t)index].frame >= 0 ? 1 : 0;
	}

	int console_view::row_has_children_fn(void* user, int index)
	{
		const auto* self = static_cast<const console_view*>(user);
		if (!self || index < 0 || (std::size_t)index >= self->rows_.size())
			return 0;
		const row_ref& row = self->rows_[(std::size_t)index];
		if (row.frame >= 0)
			return 0;
		const record* found = self->find_record(row.entry_id);
		return found && !found->value.stack.empty() ? 1 : 0;
	}

	int console_view::row_expanded_fn(void* user, int index)
	{
		const auto* self = static_cast<const console_view*>(user);
		if (!self || index < 0 || (std::size_t)index >= self->rows_.size())
			return 0;
		return self->stack_expanded(self->rows_[(std::size_t)index].entry_id)
			? 1 : 0;
	}

	void console_view::row_toggle_fn(void* user, int index)
	{
		auto* self = static_cast<console_view*>(user);
		if (!self || index < 0 || (std::size_t)index >= self->rows_.size())
			return;
		(void)self->toggle_stack(self->rows_[(std::size_t)index].entry_id);
	}

	int console_view::row_enabled_fn(void* user, int index)
	{
		const auto* self = static_cast<const console_view*>(user);
		if (!self)
			return 0;
		return index >= 0 && (std::size_t)index < self->rows_.size() ? 1 : 0;
	}

	const char* console_view::row_tooltip_fn(void* user, int index)
	{
		const auto* self = static_cast<const console_view*>(user);
		if (!self || index < 0 || (std::size_t)index >= self->rows_.size())
			return nullptr;
		const row_ref& row = self->rows_[(std::size_t)index];
		const record* found = self->find_record(row.entry_id);
		if (!found)
			return nullptr;
		if (row.frame >= 0)
		{
			return (std::size_t)row.frame < found->frame_display.size()
				? found->frame_display[(std::size_t)row.frame].c_str() : nullptr;
		}
		return found->display.c_str();
	}

	vgui_color console_view::row_text_color_fn(void* user, int index)
	{
		const auto* self = static_cast<const console_view*>(user);
		if (!self || index < 0 || (std::size_t)index >= self->rows_.size())
			return 0;
		const row_ref& row = self->rows_[(std::size_t)index];
		if (row.frame >= 0)
			return self->frame_color_;
		const record* found = self->find_record(row.entry_id);
		if (!found)
			return 0;
		const int level_index = static_cast<int>(found->value.severity);
		if (level_index < 0 || level_index >= level_count ||
			!self->level_color_set_[(std::size_t)level_index])
			return 0;
		return self->level_colors_[(std::size_t)level_index];
	}

	//----------------------------------------------------------------------------
	// widget callbacks
	//----------------------------------------------------------------------------

	void console_view::on_tick_draw(vgui_widget* widget, ovg_ctx_cb* cb,
		rvg_t* vg, float abs_x, float abs_y, float abs_w, float abs_h,
		float delta_time, void* user)
	{
		(void)widget;
		(void)cb;
		(void)vg;
		(void)abs_x;
		(void)abs_y;
		(void)abs_w;
		(void)abs_h;
		if (user)
			static_cast<console_view*>(user)->tick(delta_time);
	}

	void console_view::on_tree_activate(vgui_widget* widget, vgui_item_id item,
		void* user)
	{
		(void)widget;
		if (user)
			(void)static_cast<console_view*>(user)->activate_row(item);
	}

	void console_view::on_level_value(vgui_widget* widget, float value,
		void* user)
	{
		(void)value;
		auto* self = static_cast<console_view*>(user);
		if (!self || !widget)
			return;
		for (int i = 0; i < level_count; ++i)
		{
			if (self->level_widgets_[(std::size_t)i] != widget)
				continue;
			const bool enabled = vgui_widget_get_checked(widget) != 0;
			if (self->level_enabled_[(std::size_t)i] == enabled)
				return;
			self->level_enabled_[(std::size_t)i] = enabled;
			self->rebuild_rows();
			self->publish_model_change();
			return;
		}
	}

	void console_view::on_auto_scroll_value(vgui_widget* widget, float value,
		void* user)
	{
		(void)value;
		auto* self = static_cast<console_view*>(user);
		if (!self || !widget)
			return;
		self->auto_scroll_ = vgui_widget_get_checked(widget) != 0;
		if (self->auto_scroll_)
			self->scroll_to_latest();
	}

	int console_view::on_key(vgui_context* ctx, int key, int down, int mods,
		void* user)
	{
		auto* entry = static_cast<context_entry*>(user);
		if (!entry || entry->alive.expired() || !ctx || !down)
			return 0;
		console_view* self = entry->owner;
		if (!self || !self->tree_ || vgui_get_focus(ctx) != self->tree_)
			return 0;
		if (key == 'C' && (mods & VGUI_MOD_CTRL))
		{
			(void)self->copy_selection();
			return 1;
		}
		if (key == 'A' && (mods & VGUI_MOD_CTRL))
		{
			vgui_tree_clear_selection(self->tree_);
			for (const row_ref& row : self->rows_)
				vgui_tree_select(self->tree_, row.id, 1);
			return 1;
		}
		return 0;
	}

} /* namespace vgui_editor_kit */
