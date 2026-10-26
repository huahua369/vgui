#pragma once
/*
 * asset_browser.h - asset list/grid browser.
 *
 * The component owns a canonical selected set shared by both views.  Lists use
 * the core VGUI_LIST row model; grids draw only the visible cells of a custom
 * child inside a scroll panel.  Large directories can attach an asset_source
 * so metadata is queried lazily for the visible index range only.
 *
 * rename_item() routes through the rename handler and records an undoable
 * command when a command stack is attached.  Thumbnail textures are borrowed;
 * the host keeps them alive for as long as the browser uses them.
 */
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "command_stack.h"
#include "vgui.h"

namespace vgui_editor_kit {

	class segmented_control;

	struct asset_info
	{
		std::string name;
		std::string type;
		vgui_texture* thumbnail = nullptr;
		vgui_item_id id = 0;
	};

	class asset_browser
	{
	public:
		enum class view_mode
		{
			list,
			grid
		};

		/* Optional read-only virtual model.  count and item_at are required.
		 * item_at() is called only for indices in the visible range.  When
		 * id_at is null, ids default to index + 1.  Provide index_of for O(1)
		 * id lookup on huge directories; otherwise the browser scans id_at. */
		struct asset_source
		{
			int (*count)(void* user) = nullptr;
			int (*item_at)(void* user, int index, asset_info* out) = nullptr;
			vgui_item_id (*id_at)(void* user, int index) = nullptr;
			int (*index_of)(void* user, vgui_item_id item) = nullptr;
			void* user = nullptr;
		};

		using selection_listener = std::function<void(vgui_item_id)>;
		/* Applies the new name to the host asset model.  Return false to reject
		 * the rename; rejected renames are not recorded. */
		using rename_handler = std::function<bool(vgui_item_id item, const std::string& name)>;

		asset_browser();
		~asset_browser();
		asset_browser(const asset_browser&) = delete;
		asset_browser& operator=(const asset_browser&) = delete;

		void build(vgui_widget* parent);

		void clear();
		vgui_item_id add_item(std::string text, std::string detail = {});
		bool set_item_thumbnail(vgui_item_id item, vgui_texture* texture);
		vgui_texture* item_thumbnail(vgui_item_id item) const;

		/* Replaces the current model with a virtual source.  The source struct
		 * is copied; call refresh() after the backing data changes.  Passing
		 * nullptr detaches the source and leaves an empty full model. */
		void set_virtual_source(const asset_source* source);
		bool is_virtual() const noexcept;
		int item_count() const noexcept;
		bool item_at(int index, asset_info* out) const;
		vgui_item_id id_at(int index) const noexcept;
		int index_of(vgui_item_id item) const noexcept;

		view_mode view() const noexcept;
		void set_view(view_mode mode);
		void set_view_switch_visible(bool visible);
		bool view_switch_visible() const noexcept;

		/* Visible index range; last is exclusive.  These helpers run the same
		 * range math as the renderer so headless hosts and tests can verify
		 * virtualization without a draw pass. */
		int visible_first() const noexcept;
		int visible_last() const noexcept;
		int visible_count() const noexcept;
		std::vector<vgui_item_id> visible_items() const;
		int grid_columns() const noexcept;
		/* Item rect in absolute window coordinates.  In list mode this is the
		 * row rect; in grid mode it is the cell rect. */
		bool item_rect(int index, float* x, float* y, float* width, float* height) const;

		/* Recomputes grid metrics and invalidates virtual row data. */
		void refresh();

		vgui_item_id selected() const noexcept;
		vgui_item_id cursor() const noexcept;
		std::vector<vgui_item_id> selection() const;
		bool is_selected(vgui_item_id item) const noexcept;
		void set_selection(const std::vector<vgui_item_id>& items);
		void clear_selection();
		void set_selection_listener(selection_listener listener);

		void set_command_stack(command_stack* stack);
		void set_rename_handler(rename_handler handler);
		/* Renames the asset through the rename handler and records an undoable
		 * "Rename asset" command.  Returns false when the rename is rejected. */
		bool rename_item(vgui_item_id item, const std::string& name);

		/* F03: rows drag as asset payloads carrying the asset name in
		 * drag_payload::id.  The same provider is used by the grid cells. */
		void enable_row_drag();

		vgui_widget* widget() const noexcept;
		vgui_widget* root() const noexcept;
		vgui_widget* grid() const noexcept;
		vgui_widget* scroll() const noexcept;
		segmented_control* view_switch() const noexcept;

	private:
		class rename_command;

		struct item_entry
		{
			vgui_item_id id = 0;
			std::string name;
			std::string type;
			vgui_texture* thumbnail = nullptr;
		};

		struct grid_metrics
		{
			int columns = 1;
			float cell_w = 0.0f;
			float cell_h = 0.0f;
			float pad = 8.0f;
			float gap = 8.0f;
		};

		bool source_valid(const asset_source& source) const noexcept;
		bool apply_rename(vgui_item_id item, const std::string& name);
		void apply_virtual_source();
		void reset_model(bool detach_source);
		void update_grid_metrics();
		grid_metrics metrics_for_width(float width) const;
		float available_width() const;
		float available_height() const;
		float grid_scroll_y() const;
		int grid_index_at(float local_x, float local_y) const;
		void cell_rect(const grid_metrics& metrics, int index, float* x, float* y, float* width, float* height) const;
		void draw_grid(ovg_ctx_cb* cb, rvg_t* vg, float abs_x, float abs_y, float abs_w, float abs_h, float delta_time);
		void draw_cell(ovg_ctx_cb* cb, rvg_t* vg, const grid_metrics& metrics, int index, const asset_info& info, float abs_x, float abs_y);
		void update_hover(float local_x, float local_y);
		void update_marquee(float local_x, float local_y);
		void begin_marquee(float local_x, float local_y, bool additive);
		void end_marquee();
		void move_cursor(int target_index, bool extend, bool keep_selection);
		void ensure_cursor_visible();
		void select_range(int from, int to, bool additive);
		void apply_selection(std::vector<vgui_item_id> items, vgui_item_id primary, int anchor, bool notify);
		void sync_list_selection();
		void rebuild_selection_lookup();
		void notify_selection();
		void update_primary_from_list(vgui_item_id item, int selected);
		bool begin_grid_drag(float local_x, float local_y);
		void finish_grid_drag();

		static void on_select(vgui_widget* widget, vgui_item_id item, int selected, void* user);
		static void on_draw(vgui_widget* widget, ovg_ctx_cb* cb, rvg_t* vg, float abs_x, float abs_y, float abs_w, float abs_h, float delta_time, void* user);
		static int on_event(vgui_widget* widget, const vgui_event* event, void* user);
		static const char* on_grid_tooltip(vgui_widget* widget, void* user);

		static int source_row_count(void* user);
		static int source_row_info(void* user, int index, const char** text, const char** detail, void** item_user);
		static vgui_item_id source_row_id(void* user, int index);
		static int source_row_id_index(void* user, vgui_item_id item);
		static const char* source_row_tooltip(void* user, int index);
		static int source_row_icon(void* user, int index, vgui_texture** out_texture);

		vgui_widget* root_ = nullptr;
		vgui_widget* header_ = nullptr;
		vgui_widget* list_ = nullptr;
		vgui_widget* scroll_ = nullptr;
		vgui_widget* grid_ = nullptr;
		std::unique_ptr<segmented_control> view_switch_;

		view_mode view_ = view_mode::list;
		bool switch_visible_ = true;

		std::vector<item_entry> items_;
		std::unordered_map<vgui_item_id, int> item_index_;
		vgui_item_id next_item_id_ = 1;
		std::unordered_map<vgui_item_id, vgui_texture*> virtual_thumbs_;

		asset_source source_;
		asset_source pending_source_;
		bool has_pending_source_ = false;
		bool virtual_mode_ = false;

		std::vector<vgui_item_id> selection_;
		std::unordered_set<vgui_item_id> selection_lookup_;
		vgui_item_id primary_ = 0;
		vgui_item_id cursor_ = 0;
		int anchor_index_ = -1;
		bool syncing_ = false;

		selection_listener listener_;
		command_stack* command_stack_ = nullptr;
		rename_handler rename_handler_;

		bool row_drag_enabled_ = false;
		bool drag_pending_ = false;
		bool drag_started_ = false;
		float drag_press_x_ = 0.0f;
		float drag_press_y_ = 0.0f;
		vgui_item_id drag_item_ = 0;
		std::string drag_name_;

		bool marquee_active_ = false;
		float marquee_x0_ = 0.0f;
		float marquee_y0_ = 0.0f;
		float marquee_x1_ = 0.0f;
		float marquee_y1_ = 0.0f;
		float marquee_phase_ = 0.0f;
		std::vector<vgui_item_id> marquee_base_;

		int hover_index_ = -1;
		std::string tooltip_;

		float grid_content_h_ = 0.0f;
		float last_grid_width_ = 0.0f;

		mutable asset_info scratch_info_;
		mutable int scratch_index_ = -1;
		mutable std::string scratch_tooltip_;

		std::shared_ptr<int> alive_token_ = std::make_shared<int>(0);
	};

} /* namespace vgui_editor_kit */
