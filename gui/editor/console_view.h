#pragma once
/*
 * console_view.h - editor log console.
 *
 * Logs are produced on any thread and queued; the UI thread drains a bounded
 * batch from the ticker so a large producer cannot stall layout or drawing.
 * The visible log is a virtual tree projection: only the rows in view are
 * formatted/queried, and stack traces stay collapsed until expanded.
 *
 * The component owns no game or asset data.  Error rows carry an optional
 * location (asset id or script path + line); double-clicking/Enter invokes
 * the host jump handler so the editor can focus the related asset or script.
 */
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "search_box.h"
#include "toolbar.h"
#include "vgui.h"

namespace vgui_editor_kit {

	class console_view
	{
	public:
		enum class level
		{
			trace,
			debug,
			info,
			warning,
			error
		};

		static constexpr int level_count = 5;
		static const char* level_name(level value) noexcept;

		/* A jump target.  asset_id and path are alternatives; line/column are
		 * optional and mainly make sense for scripts. */
		struct location
		{
			vgui_item_id asset_id = 0;
			std::string path;
			int line = 0;
			int column = 0;

			bool valid() const noexcept
			{
				return asset_id != 0 || !path.empty();
			}
		};

		struct stack_frame
		{
			std::string function;
			location where;
		};

		struct entry
		{
			level severity = level::info;
			std::string message;
			std::string source;
			std::string timestamp;
			location where;
			std::vector<stack_frame> stack;
		};

		using jump_handler = std::function<bool(const location& where)>;

		console_view();
		~console_view();

		console_view(const console_view&) = delete;
		console_view& operator=(const console_view&) = delete;

		void build(vgui_widget* parent);
		bool built() const noexcept;

		/* Thread-safe producers: each call assigns a stable entry id and queues
		 * the record.  poll()/tick() on the UI thread drains up to
		 * drain_batch() records per call.  Only the last max_entries() records
		 * are retained; older records are dropped and counted. */
		vgui_item_id append(level severity, std::string message,
			std::string source = {});
		vgui_item_id append(entry value);
		std::vector<vgui_item_id> append_batch(std::vector<entry> values);
		std::size_t poll();
		void tick(float delta_time);
		bool idle() const;
		std::size_t pending_count() const;
		std::size_t entry_count() const noexcept;
		std::size_t visible_entry_count() const noexcept;
		std::size_t visible_row_count() const noexcept;
		std::size_t dropped_count() const noexcept;
		vgui_item_id last_entry_id() const noexcept;
		bool entry_at(vgui_item_id id, entry* out) const;

		void clear();
		void set_max_entries(std::size_t max_entries);
		std::size_t max_entries() const;
		void set_drain_batch(std::size_t count);
		std::size_t drain_batch() const noexcept;

		/* Level and text filters are independent.  The first disabled level or
		 * non-empty search term makes filter_active() true. */
		void set_level_enabled(level value, bool enabled);
		bool level_enabled(level value) const noexcept;
		void set_search_text(std::string text);
		const std::string& search_text() const noexcept;
		bool filter_active() const noexcept;

		void set_auto_scroll(bool enabled);
		bool auto_scroll() const noexcept;
		void scroll_to_latest();

		/* Stack traces are collapsed by default.  Expansion is per entry so
		 * opening one error does not expand every trace in the log. */
		bool stack_expanded(vgui_item_id entry_id) const noexcept;
		bool set_stack_expanded(vgui_item_id entry_id, bool expanded);
		bool toggle_stack(vgui_item_id entry_id);

		/* Copy support.  selected_text()/visible_text() are useful for tests
		 * and hosts; copy_selection() puts selected rows (or all visible rows
		 * when nothing is selected) on the system clipboard. */
		std::string selected_text() const;
		std::string visible_text() const;
		bool copy_selection();
		bool activate_row(vgui_item_id row_id);
		bool activate_selected();
		void set_jump_handler(jump_handler handler);

		/* Per-level colours; unspecified levels follow the current theme. */
		void set_level_color(level value, vgui_color color);
		vgui_color level_color(level value) const noexcept;

		/* Index helpers for hosts/tests.  row ids stay stable while their log
		 * entry is retained, including when filters change. */
		vgui_item_id row_id_at(int visible_index) const;
		bool row_entry(vgui_item_id row_id, vgui_item_id* entry_id,
			int* frame) const;

		vgui_widget* widget() const noexcept;
		vgui_widget* tree() const noexcept;
		vgui_widget* ticker() const noexcept;
		search_box& search() noexcept;
		const search_box& search() const noexcept;
		vgui_widget* auto_scroll_widget() const noexcept;
		vgui_widget* level_widget(level value) const noexcept;
		vgui_widget* copy_widget() const noexcept;
		vgui_widget* clear_widget() const noexcept;

	private:
		struct record
		{
			vgui_item_id id = 0;
			entry value;
			std::string display;
			std::vector<std::string> frame_display;
			bool visible = false;
		};

		struct row_ref
		{
			vgui_item_id id = 0;
			vgui_item_id entry_id = 0;
			int frame = -1;
		};

		struct queued_entry
		{
			vgui_item_id id = 0;
			entry value;
		};

		struct context_entry;

		static std::uint64_t encode_row(vgui_item_id entry_id,
			int frame) noexcept;
		static bool decode_row(vgui_item_id row_id, vgui_item_id* entry_id,
			int* frame) noexcept;

		void apply_theme_colors();
		void append_record(queued_entry value);
		bool trim_entries(std::size_t limit);
		void rebuild_rows();
		void append_rows_for(const record& value);
		void add_row(const record& value, int frame);
		bool matches(const entry& value) const;
		const record* find_record(vgui_item_id entry_id) const;
		const row_ref* find_row(vgui_item_id row_id) const;
		std::string row_text(const row_ref& row) const;
		bool activate_selection(const std::vector<vgui_item_id>& rows);
		void publish_model_change();
		void update_stats();

		static bool contains_ci(const std::string& text,
			const std::string& lower_needle);
		static std::string lower_ascii(std::string text);
		static std::string single_line(std::string text);
		static std::string format_entry(const entry& value);
		static std::string format_frame(const stack_frame& value);

		static int row_count_fn(void* user);
		static int row_info_fn(void* user, int index, const char** text,
			const char** detail, void** item_user);
		static vgui_item_id row_id_fn(void* user, int index);
		static int row_id_index_fn(void* user, vgui_item_id item);
		static int row_depth_fn(void* user, int index);
		static int row_has_children_fn(void* user, int index);
		static int row_expanded_fn(void* user, int index);
		static void row_toggle_fn(void* user, int index);
		static int row_enabled_fn(void* user, int index);
		static const char* row_tooltip_fn(void* user, int index);
		static vgui_color row_text_color_fn(void* user, int index);

		static void on_tick_draw(vgui_widget* widget, ovg_ctx_cb* cb,
			rvg_t* vg, float abs_x, float abs_y, float abs_w, float abs_h,
			float delta_time, void* user);
		static void on_tree_activate(vgui_widget* widget, vgui_item_id item,
			void* user);
		static void on_level_value(vgui_widget* widget, float value, void* user);
		static void on_auto_scroll_value(vgui_widget* widget, float value,
			void* user);
		static int on_key(vgui_context* ctx, int key, int down, int mods,
			void* user);

		vgui_widget* root_ = nullptr;
		vgui_widget* header_ = nullptr;
		vgui_widget* filter_row_ = nullptr;
		vgui_widget* tree_ = nullptr;
		vgui_widget* ticker_ = nullptr;
		search_box search_;
		icon_button copy_button_;
		icon_button clear_button_;
		vgui_widget* auto_scroll_widget_ = nullptr;
		std::array<vgui_widget*, level_count> level_widgets_ = {};
		vgui_widget* stats_label_ = nullptr;
		vgui_widget* copy_sink_ = nullptr;
		vgui_context* context_ = nullptr;
		int key_handler_token_ = 0;
		context_entry* key_entry_ = nullptr;

		std::deque<record> entries_;
		std::unordered_map<vgui_item_id, record*> entry_index_;
		std::deque<row_ref> rows_;
		std::unordered_map<vgui_item_id, std::size_t> row_index_;
		std::size_t row_base_ = 0;
		std::size_t visible_entries_ = 0;
		std::unordered_set<vgui_item_id> expanded_;
		std::string search_text_;
		std::string search_lower_;
		bool level_enabled_[level_count] = { true, true, true, true, true };
		bool level_color_set_[level_count] = { false, false, false, false, false };
		vgui_color level_colors_[level_count] = {};
		vgui_color frame_color_ = 0xFF8A93A3;
		bool auto_scroll_ = true;
		jump_handler jump_handler_;

		mutable std::mutex pending_mutex_;
		std::deque<queued_entry> pending_;
		std::size_t max_entries_ = 20000;
		std::size_t drain_batch_ = 256;
		std::atomic<std::uint64_t> next_id_ = 1;
		std::atomic<std::size_t> dropped_count_ = 0;

		std::shared_ptr<int> alive_token_;
	};

} /* namespace vgui_editor_kit */
