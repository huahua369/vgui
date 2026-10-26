#pragma once
/*
 * asset_browser.h - asset list/grid browser.
 *
 * The component owns a canonical selected set shared by both views.  Lists use
 * the core VGUI_LIST row model; grids draw only the visible cells of a custom
 * child inside a scroll panel.  Large directories can attach an asset_source
 * so metadata is queried lazily for the visible index range only.
 *
 * A02 adds a breadcrumb navigation strip with back/forward history and a
 * debounced search + type filter.  The host supplies directory contents
 * through set_navigate_handler(); every navigation snapshots the previous
 * path's selection and scroll offset so going back restores what the user saw.
 *
 * rename_item() routes through the rename handler and records an undoable
 * command when a command stack is attached.
 *
 * A05 adds an owned thumbnail_cache.  Host thumbnails supplied through
 * set_item_thumbnail() still win; otherwise the browser requests missing
 * thumbnails for the visible range from a host loader, shows queued/failed
 * placeholders and cancels requests when items scroll away.  Configure it via
 * thumbnails().set_load_handler()/set_options()/set_disk_cache().
 */
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "command_stack.h"
#include "editor_types.h"
#include "thumbnail_cache.h"
#include "vgui.h"

namespace vgui_editor_kit {

	class breadcrumb;
	class combo_box;
	class icon_button;
	class search_box;
	class segmented_control;

	struct asset_info
	{
		std::string name;
		std::string type;
		vgui_texture* thumbnail = nullptr;
		vgui_item_id id = 0;
	};

	/* One breadcrumb segment.  The id is passed back to the navigate handler
	 * when the user clicks the segment. */
	struct asset_path_item
	{
		std::string label;
		std::string id;
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

		/* Contents of one directory.  Provide either a virtual source or a
		 * plain item vector; source wins when both are valid.  path becomes the
		 * breadcrumb strip (the last segment is the current location) and
		 * empty_text customises the zero-item placeholder. */
		struct asset_directory
		{
			std::vector<asset_path_item> path;
			std::vector<asset_info> items;
			asset_source source;
			std::string empty_text;
		};

		using selection_listener = std::function<void(vgui_item_id)>;
		using selection_listener_id = std::size_t;
		using activate_listener = std::function<void(vgui_item_id)>;
		using path_listener = std::function<void(const std::string& path_id)>;
		/* Applies the new name to the host asset model.  Return false to reject
		 * the rename; rejected renames are not recorded. */
		using rename_handler = std::function<bool(vgui_item_id item, const std::string& name)>;
		/* Resolves a path id into breadcrumbs plus directory contents.  Return
		 * false to reject the navigation and leave the current path untouched. */
		using navigate_handler = std::function<bool(const std::string& path_id, asset_directory* out)>;

		asset_browser();
		~asset_browser();
		asset_browser(const asset_browser&) = delete;
		asset_browser& operator=(const asset_browser&) = delete;

		void build(vgui_widget* parent);

		void clear();
		vgui_item_id add_item(std::string text, std::string detail = {});
		bool set_item_thumbnail(vgui_item_id item, vgui_texture* texture);
		vgui_texture* item_thumbnail(vgui_item_id item) const;

		/* A05: asynchronous thumbnails.  The browser drives requests from
		 * visible_first()/visible_last() inside tick() and cancels queued or
		 * in-flight work for items that leave the visible range.  Host
		 * thumbnails set with set_item_thumbnail() always win. */
		thumbnail_cache& thumbnails() noexcept;
		const thumbnail_cache& thumbnails() const noexcept;
		/* Stable cache key for this item under the current path.  Passed to
		 * thumbnail_cache::enqueue and visible through the cache handlers. */
		std::string thumbnail_key(vgui_item_id item) const;
		thumbnail_cache::state thumbnail_state(vgui_item_id item) const;
		/* Restarts a failed automatic load.  Returns false when the item does
		 * not exist or already has a texture. */
		bool retry_thumbnail(vgui_item_id item);

		/* Replaces the current model with a virtual source.  The source struct
		 * is copied; call refresh() after the backing data changes.  Passing
		 * nullptr detaches the source and leaves an empty full model. */
		void set_virtual_source(const asset_source* source);
		bool is_virtual() const noexcept;
		int item_count() const noexcept;
		bool item_at(int index, asset_info* out) const;
		vgui_item_id id_at(int index) const noexcept;
		int index_of(vgui_item_id item) const noexcept;
		/* Looks up an item by id even when the current search/type filter
		 * hides it.  Used by selection-driven panels such as asset_preview. */
		bool item_info(vgui_item_id item, asset_info* out) const;

		view_mode view() const noexcept;
		void set_view(view_mode mode);
		void set_view_switch_visible(bool visible);
		bool view_switch_visible() const noexcept;

		/* ---- A02: path navigation, search and type filtering ---- */
		void set_navigate_handler(navigate_handler handler);
		void set_path_listener(path_listener listener);
		/* Resolves path_id through the navigate handler and clears the forward
		 * stack.  The previous location (selection, cursor and both scroll
		 * offsets) is snapshotted for go_back(). */
		bool navigate_to(const std::string& path_id);
		bool can_go_back() const noexcept;
		bool can_go_forward() const noexcept;
		bool go_back();
		bool go_forward();
		/* Re-resolves the current path without touching the history stacks.
		 * Selection and scroll offsets survive when their items still exist. */
		bool reload();
		const std::string& current_path() const noexcept;
		void set_navigation_visible(bool visible);
		bool navigation_visible() const noexcept;

		/* Debounced search.  Search text reaches the filter after the debounce
		 * interval; submitting the search box or set_search_text(..., true)
		 * applies immediately.  The component drives its own timer from a 2x2
		 * ticker widget, while headless hosts/tests can call tick() directly. */
		void set_search_debounce(float seconds);
		float search_debounce() const noexcept;
		void set_search_text(std::string text, bool immediate = false);
		const std::string& search_text() const noexcept;
		bool search_pending() const noexcept;
		void apply_search();
		bool filter_active() const noexcept;
		/* "No assets" for an empty directory, "No matches" when filters hide
		 * every item.  Both strings are configurable; a directory's own
		 * empty_text still wins over the default. */
		void set_empty_texts(std::string empty, std::string no_matches);
		const std::string& empty_state_text() const noexcept;
		void tick(float delta_time);

		void set_type_options(std::vector<std::string> types);
		const std::vector<std::string>& type_options() const noexcept;
		void set_type_filter(std::string type);
		const std::string& type_filter() const noexcept;
		void set_type_filter_visible(bool visible);
		bool type_filter_visible() const noexcept;
		int total_item_count() const noexcept;

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
		/* Legacy single listener.  Prefer add_selection_listener() when more
		 * than one component tracks the selection. */
		void set_selection_listener(selection_listener listener);
		selection_listener_id add_selection_listener(selection_listener listener);
		bool remove_selection_listener(selection_listener_id id);

		void set_command_stack(command_stack* stack);
		void set_rename_handler(rename_handler handler);
		/* Renames the asset through the rename handler and records an undoable
		 * "Rename asset" command.  Returns false when the rename is rejected. */
		bool rename_item(vgui_item_id item, const std::string& name);

		/* Double-click/Enter activation (for example to enter a folder). */
		void set_activate_listener(activate_listener listener);
		void activate_selected();

		/* F03: rows drag as asset payloads carrying the asset name in
		 * drag_payload::id.  The same provider is used by the grid cells. */
		void enable_row_drag();

		/* A04: OS files/asset imports.  The root of the browser accepts
		 * VGUI_DRAG_FILES and routes them through the same import entry as
		 * import_files(), with the current path id.  Return false to reject
		 * the whole batch (no assets are imported). */
		using import_handler = std::function<bool(const std::vector<std::string>& files,
			const std::string& path_id)>;
		void set_import_handler(import_handler handler);
		bool import_files(const std::vector<std::string>& files);

		vgui_widget* widget() const noexcept;
		vgui_widget* root() const noexcept;
		vgui_widget* grid() const noexcept;
		vgui_widget* scroll() const noexcept;
		segmented_control* view_switch() const noexcept;
		breadcrumb* path() const noexcept;
		search_box* search() const noexcept;
		combo_box* type_combo() const noexcept;

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

		struct nav_state
		{
			std::string path_id;
			std::vector<vgui_item_id> selection;
			vgui_item_id primary = 0;
			vgui_item_id cursor = 0;
			int anchor = -1;
			float list_scroll = 0.0f;
			float grid_scroll = 0.0f;
		};

		bool source_valid(const asset_source& source) const noexcept;
		bool apply_rename(vgui_item_id item, const std::string& name);
		void apply_virtual_source();
		void reset_model(bool detach_source, bool notify = true);
		void clear_model_data();
		void apply_directory(const std::string& path_id, const asset_directory& directory);
		bool install_directory(const asset_directory& directory);
		nav_state capture_navigation_state() const;
		void restore_navigation_state(const nav_state& state, bool notify);
		void update_history_buttons();
		void update_breadcrumb();
		void update_type_combo();
		void update_empty_text();
		void update_filter_visibility();
		void rebuild_view_indices();
		int base_item_count() const noexcept;
		bool base_item_at(int index, asset_info* out) const;
		vgui_item_id base_id_at(int index) const noexcept;
		int base_index_of(vgui_item_id item) const noexcept;
		int view_index_to_base(int index) const noexcept;
		int base_index_to_view(int index) const noexcept;
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
		void update_import_target();
		void sync_thumbnail_requests();
		void cancel_thumbnail_requests();
		vgui_texture* cached_thumbnail(vgui_item_id item) const;

		static void on_select(vgui_widget* widget, vgui_item_id item, int selected, void* user);
		static void on_activate(vgui_widget* widget, vgui_item_id item, void* user);
		static void on_draw(vgui_widget* widget, ovg_ctx_cb* cb, rvg_t* vg, float abs_x, float abs_y, float abs_w, float abs_h, float delta_time, void* user);
		static void on_tick_draw(vgui_widget* widget, ovg_ctx_cb* cb, rvg_t* vg, float abs_x, float abs_y, float abs_w, float abs_h, float delta_time, void* user);
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
		vgui_widget* filter_row_ = nullptr;
		vgui_widget* ticker_ = nullptr;
		vgui_widget* list_ = nullptr;
		vgui_widget* scroll_ = nullptr;
		vgui_widget* grid_ = nullptr;
		std::unique_ptr<segmented_control> view_switch_;
		std::unique_ptr<icon_button> back_button_;
		std::unique_ptr<icon_button> forward_button_;
		std::unique_ptr<icon_button> reload_button_;
		std::unique_ptr<breadcrumb> breadcrumb_;
		std::unique_ptr<search_box> search_;
		std::unique_ptr<combo_box> type_combo_;

		view_mode view_ = view_mode::list;
		bool switch_visible_ = true;
		bool navigation_visible_ = true;

		std::vector<item_entry> items_;
		std::unordered_map<vgui_item_id, int> item_index_;
		vgui_item_id next_item_id_ = 1;
		std::unordered_map<vgui_item_id, vgui_texture*> virtual_thumbs_;

		asset_source source_;
		asset_source pending_source_;
		bool has_pending_source_ = false;
		bool virtual_mode_ = false;

		/* Filtered view maps public indices onto the backing model.  Empty and
		 * filtered_ == false means the view is the identity. */
		std::vector<int> view_indices_;
		bool filtered_ = false;
		std::string search_query_;
		std::string applied_search_;
		float search_debounce_ = 0.25f;
		float search_elapsed_ = 0.0f;
		bool search_pending_ = false;
		std::string type_filter_;
		std::vector<std::string> type_options_;
		bool type_filter_visible_ = true;
		std::string default_empty_text_ = "No assets";
		std::string directory_empty_text_ = "No assets";
		std::string no_matches_text_ = "No matches";
		std::string empty_state_ = "No assets";

		navigate_handler navigate_handler_;
		path_listener path_callback_;
		std::string current_path_id_;
		std::vector<asset_path_item> path_items_;
		bool has_location_ = false;
		std::vector<nav_state> back_stack_;
		std::vector<nav_state> forward_stack_;

		std::vector<vgui_item_id> selection_;
		std::unordered_set<vgui_item_id> selection_lookup_;
		vgui_item_id primary_ = 0;
		vgui_item_id cursor_ = 0;
		int anchor_index_ = -1;
		bool syncing_ = false;

		struct selection_listener_entry
		{
			selection_listener_id id = 0;
			selection_listener fn;
		};

		selection_listener listener_;
		std::vector<selection_listener_entry> selection_listeners_;
		selection_listener_id next_selection_listener_ = 1;
		activate_listener activate_listener_;
		command_stack* command_stack_ = nullptr;
		rename_handler rename_handler_;

		bool row_drag_enabled_ = false;
		bool drag_pending_ = false;
		bool drag_started_ = false;
		float drag_press_x_ = 0.0f;
		float drag_press_y_ = 0.0f;
		vgui_item_id drag_item_ = 0;
		std::string drag_name_;

		/* A04 import drop target (root widget, VGUI_DRAG_FILES). */
		import_handler import_handler_;
		std::unique_ptr<drop_target> import_target_;

		/* A05 asynchronous visible-range thumbnails. */
		std::unique_ptr<thumbnail_cache> thumbnails_;
		std::unordered_set<std::string> thumbnail_requests_;
		float thumbnail_phase_ = 0.0f;

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
