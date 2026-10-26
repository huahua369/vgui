#pragma once
/*
 * outliner_view.h - scene hierarchy component backed by VGUI_TREE.
 *
 * The scene model stays in the host application; this class owns the tree
 * widget and maps rows to stable editor object ids.  Two data modes:
 *
 *  - full model: the host mirrors its scene via clear()/add_node();
 *  - virtual (O05): the host registers a scene_source adapter and this class
 *    owns the visible-row projection and the expansion state, pulling rows
 *    lazily so huge scenes only query visible rows.
 *
 * Editor interactions on top of the rows (M3):
 *  - O01 row extensions: type icons, text colors and inline visibility/lock
 *    buttons whose clicks never change the selection;
 *  - O02 inline rename: F2 / double-click starts an edit overlay, Enter
 *    commits, Escape cancels, blur commits; renames run through the rename
 *    handler and the command stack;
 *  - O03 drag & drop re-parenting: insertion line before/after a row, drop
 *    onto a row to make it a child, multi-row drags in one undoable
 *    transaction, cycle rejection without a hint;
 *  - O04 name filtering with match highlight plus two-way selection sync with
 *    editor_context (foreign selections expand and scroll into view);
 *  - O06 context menu and Delete/F2/Ctrl+D shortcuts dispatching through one
 *    action handler.
 */
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "command_stack.h"
#include "editor_context.h"
#include "editor_types.h"
#include "vgui.h"

namespace vgui_editor_kit {

	class outliner_view
	{
	public:
		using selection_listener = std::function<void(object_id)>;
		/* Applies an absolute move of node under parent at index.  Return false
		 * to reject the move; rejected moves are not recorded. */
		using move_handler = std::function<bool(object_id node, object_id parent,
			std::size_t index)>;
		/* Validates/applies a rename in the host model.  Return false (empty
		 * name, illegal characters, duplicate) to reject; rejected renames are
		 * not recorded. */
		using rename_handler = std::function<bool(object_id node,
			const std::string& name)>;
		using flag_listener = std::function<void(object_id node, bool state)>;
		/* Extra predicate combined with the name filter (e.g. type filter). */
		using filter_match_handler = std::function<bool(object_id node,
			const char* name)>;

		/* O05 virtual data adapter.  root walking and names are required; the
		 * rest falls back to neutral defaults. */
		struct scene_source
		{
			std::function<std::size_t()> root_count;
			std::function<object_id(std::size_t index)> root_at;
			std::function<std::size_t(object_id)> child_count;
			std::function<object_id(object_id, std::size_t)> child_at;
			std::function<std::string(object_id)> name;
			std::function<object_id(object_id)> parent_of;		// optional
			std::function<vgui_texture*(object_id)> icon;		// optional
			std::function<int(object_id)> enabled;			// optional
			std::function<const char*(object_id)> tooltip;		// optional
			std::function<vgui_color(object_id)> text_color;		// optional
			std::function<void(object_id, ovg_ctx_cb*, rvg_t*, float, float,
				float, float)> row_draw;				// optional (virtual only)
		};

		/* O06 menu/shortcut actions.  rename and the flag toggles are handled
		 * internally; the host implements the rest against its model. */
		enum class action
		{
			new_child,
			duplicate,
			rename,
			remove,
			focus,
			toggle_visibility,
			toggle_lock
		};
		using action_handler = std::function<bool(action kind,
			const std::vector<object_id>& nodes)>;

		/* O03 drop semantics relative to the row under the pointer. */
		enum class drop_kind { none, before, after, onto, roots };
		struct drop_location
		{
			drop_kind kind = drop_kind::none;
			object_id parent = invalid_object;	// resolved target parent
			std::size_t index = 0;			// sibling index under parent
			vgui_item_id row = 0;			// hovered row item (0 for roots)
		};

		~outliner_view();

		void build(editor_context& context, vgui_widget* parent);

		/* Full-model mirroring (ignored in virtual mode). */
		void clear();
		vgui_item_id add_node(const char* text, vgui_item_id parent = 0,
			object_id object = invalid_object);
		void expand(vgui_item_id node);

		vgui_item_id selected() const noexcept;
		vgui_widget* widget() const noexcept;

		void set_selection_listener(selection_listener listener);

		void set_command_stack(command_stack* stack);
		void set_move_handler(move_handler handler);
		/* Moves node under new_parent at new_index through the move handler and
		 * records an undoable "Move node" command.  Returns false when the move
		 * is rejected or no handler is set. */
		bool move_node(object_id node, object_id new_parent, std::size_t new_index);

		/* ---- O01 row extensions ------------------------------------------- */
		void set_node_icon(vgui_item_id node, vgui_texture* icon);
		void set_node_text_color(vgui_item_id node, vgui_color color);
		/* Installs the inline visibility (button 0) and lock (button 1)
		 * buttons on every row.  Button clicks never change the selection. */
		void enable_row_buttons();
		void set_row_button_icons(vgui_texture* visible_on, vgui_texture* visible_off,
			vgui_texture* locked_on, vgui_texture* locked_off);
		void set_object_visibility(object_id object, bool visible);
		void set_object_locked(object_id object, bool locked);
		void toggle_object_visibility(object_id object);
		void toggle_object_lock(object_id object);
		bool object_visibility(object_id object) const;
		bool object_locked(object_id object) const;
		void set_visibility_listener(flag_listener listener);
		void set_lock_listener(flag_listener listener);

		/* ---- O02 inline rename --------------------------------------------- */
		void set_rename_handler(rename_handler handler);
		/* Validates/applies a rename and records an undoable "Rename node"
		 * command; empty names are rejected without touching the handler. */
		bool rename_node(object_id node, const std::string& name);
		/* F2 (tree focused) and double-click start the inline editor. */
		void enable_inline_rename();
		void begin_rename(vgui_item_id node);
		bool renaming_active() const noexcept { return renaming_; }

		/* ---- O03 drag & drop ------------------------------------------------ */
		/* Rows drag as scene_object payloads (stable object id, decimal text;
		 * multi-selection drags a comma-joined id list); dropping on the tree
		 * routes through move_node() with insertion semantics. */
		void enable_drag_drop();
		void set_multi_select(bool enabled);
		/* A04: asset payloads dropped on the tree create a child/sibling via
		 * the host handler, which receives the asset id and resolved
		 * (parent, index).  Return false to reject the drop. */
		using asset_drop_handler = std::function<bool(const std::string& asset,
			object_id parent, std::size_t index)>;
		void set_asset_drop_handler(asset_drop_handler handler);
		bool asset_drop_enabled() const noexcept { return asset_drop_ != nullptr; }
		/* Applies an asset payload at the drop point; exposes the same path
		 * used by the internal target for tests and programmatic drops. */
		bool apply_asset_drop(const std::string& asset, float x, float y);
		/* Resolves a drop point (absolute window coordinates) to its position
		 * kind and final (parent, index). */
		drop_location drop_location_at(float x, float y) const;
		/* Legacy bool form: resolved parent/index of the drop point. */
		bool row_drop_location(float x, float y, object_id* out_parent,
			std::size_t* out_index) const;
		/* Applies a scene_object payload (comma-joined object ids) at the drop
		 * point; rejects cycles.  Exposed for tests and programmatic drops. */
		bool apply_drop_by_payload(const std::string& payload_id, float x, float y);
		/* Drop indicator for a resolved location (absolute coordinates); kind
		 * NONE signals a rejected position. */
		void emit_hover_hint(const drop_location& location,
			vgui_drop_hint* hint) const;

		/* ---- O04 filter & selection sync ----------------------------------- */
		/* Case-insensitive name substring; matched text is highlighted.
		 * Passing nullptr/empty clears the text filter (a filter match handler
		 * alone keeps filtering). */
		void set_filter(const char* text);
		void set_filter_match_handler(filter_match_handler handler);
		/* Registers a context selection listener so foreign selections
		 * (viewport, property grid) select, expand and scroll the row. */
		void enable_selection_sync();

		/* ---- O05 virtual mode ----------------------------------------------- */
		/* Switches the tree to a virtual row source backed by the adapter.
		 * Full-model mirroring calls become no-ops; expansion lives here. */
		void set_scene_source(scene_source source);
		void scene_changed();				// structural host change
		void scene_row_changed(object_id object);	// single row content
		bool is_virtual() const noexcept { return virtual_; }
		/* P08:当前选中的对象集合(视觉顺序,全量/虚拟模式通用)。宿主
		 * 把它交给 property_grid::set_multi_targets 做多对象编辑。 */
		std::vector<object_id> selection_objects() const;
		/* Test/debug helper: number of currently projected visible rows. */
		std::size_t projected_row_count() const;
		/* Test helpers exposing the internal projection. */
		void ensure_vrows_for_test() const { ensure_vrows(); }
		object_id vrow_at_for_test(std::size_t index) const
		{
			return index < vrows_.size() ? vrows_[index].object : invalid_object;
		}

		/* ---- O06 context menu & shortcuts ---------------------------------- */
		/* Right-click menu (New Child / Duplicate / Rename / Delete / Focus /
		 * visibility / lock) plus Delete, F2 and Ctrl+D shortcuts, all routed
		 * through the same dispatch as the menu entries. */
		void enable_context_menu();
		void set_action_handler(action_handler handler);

	private:
		class move_command;
		class rename_command;

		struct node_location
		{
			object_id parent = invalid_object;
			std::size_t index = 0;
			bool valid = false;
		};

		/* Virtual-mode projected row. */
		struct vrow
		{
			object_id object = invalid_object;
			object_id parent = invalid_object;
			int depth = 0;
			std::size_t index_in_parent = 0;
		};

		object_id object_for_item(vgui_item_id item) const;
		vgui_item_id item_for_object(object_id object) const;
		node_location location_of(vgui_item_id item) const;
		bool apply_move(object_id node, object_id parent, std::size_t index);
		bool apply_rename(object_id node, const std::string& name);

		/* Row text for renames/filters in either mode. */
		std::string node_text(vgui_item_id item, object_id object) const;
		std::size_t child_count_of(object_id object, vgui_item_id item) const;

		/* Buttons / flags. */
		void refresh_row_buttons(vgui_item_id item, object_id object);
		void flag_changed(object_id object, int button);

		/* Inline rename. */
		void ensure_edit();
		void finish_editor(bool restore_focus);
		void commit_rename();

		/* Drag & drop helpers. */
		std::vector<object_id> selected_objects_in_order() const;
		std::vector<object_id> parse_payload_ids(const std::string& id) const;
		bool would_cycle(const std::vector<object_id>& nodes,
			object_id target_parent) const;
		std::vector<object_id> ancestor_chain(object_id object) const;
		bool apply_drop(const std::vector<object_id>& nodes,
			const drop_location& location);

		/* Filtering. */
		bool match_object(object_id object, const char* name) const;
		bool filter_active() const;

		/* Virtual projection. */
		void ensure_vrows() const;
		void invalidate_vrows();
		void collect_virtual(object_id object, object_id parent, int depth,
			std::size_t index, const std::unordered_set<object_id>* only,
			std::vector<vrow>& out) const;
		bool find_virtual_path(object_id target, std::vector<object_id>& path) const;

		/* O06 dispatch shared by menu and shortcuts. */
		void dispatch_action(action kind);
		void open_context_menu(float abs_x, float abs_y);
		void ensure_key_handler();
		void ensure_tree_event_handler();
		void on_tree_event(const vgui_event* event);
		void select_solo(vgui_item_id item, object_id object);

		/* Core C trampolines. */
		static void on_select(vgui_widget* widget, vgui_item_id item, int selected,
			void* user);
		static void on_item_button(vgui_widget* widget, vgui_item_id item,
			int button, void* user);
		static void on_tree_event_thunk(vgui_widget* widget, const vgui_event* event,
			void* user);
		static void on_edit_event_thunk(vgui_widget* widget, const vgui_event* event,
			void* user);
		static void on_focus_change_thunk(vgui_context* ctx, vgui_widget* old_focus,
			vgui_widget* new_focus, void* user);
		static int on_key_thunk(vgui_context* ctx, int key, int down, int mods,
			void* user);
		static void on_menu_activate_thunk(vgui_widget* menu, vgui_item_id item,
			void* user);
		static int filter_thunk(vgui_widget* tree, vgui_item_id node, void* user);
		void on_context_selection(object_id object);

		/* vgui_row_source trampolines (virtual mode). */
		static int vs_row_count(void* user);
		static int vs_row_info(void* user, int index, const char** text,
			const char** detail, void** item_user);
		static vgui_item_id vs_row_id(void* user, int index);
		static int vs_row_id_index(void* user, vgui_item_id item);
		static int vs_row_depth(void* user, int index);
		static int vs_row_has_children(void* user, int index);
		static int vs_row_expanded(void* user, int index);
		static void vs_row_toggle(void* user, int index);
		static int vs_row_enabled(void* user, int index);
		static const char* vs_row_tooltip(void* user, int index);
		static int vs_row_icon(void* user, int index, vgui_texture** out_texture);
		static vgui_color vs_row_text_color(void* user, int index);
		static int vs_row_buttons(void* user, int index);
		static int vs_row_button_icon(void* user, int index, int button,
			vgui_texture** out_texture);
		static void vs_row_button_click(void* user, int index, int button);
		static void vs_row_draw(void* user, int index, ovg_ctx_cb* cb, rvg_t* vg,
			float x, float y, float w, float h);

		editor_context* context_ = nullptr;
		vgui_widget* parent_ = nullptr;		// tree's parent (edit overlay lives here)
		vgui_widget* tree_ = nullptr;
		vgui_item_id selected_ = 0;
		selection_listener listener_;
		command_stack* command_stack_ = nullptr;
		move_handler move_handler_;
		rename_handler rename_handler_;
		std::shared_ptr<int> alive_token_ = std::make_shared<int>(0);
		std::unique_ptr<drop_target> drop_target_;

		/* O01 flags: object -> {visible, locked}. */
		bool row_buttons_ = false;
		std::unordered_map<object_id, std::pair<bool, bool>> flags_;
		flag_listener visibility_listener_;
		flag_listener lock_listener_;
		vgui_texture* vis_icon_on_ = nullptr;
		vgui_texture* vis_icon_off_ = nullptr;
		vgui_texture* lock_icon_on_ = nullptr;
		vgui_texture* lock_icon_off_ = nullptr;

		/* O02 inline rename. */
		bool inline_rename_ = false;
		bool renaming_ = false;
		vgui_widget* edit_ = nullptr;
		vgui_item_id rename_item_ = 0;
		object_id rename_object_ = invalid_object;

		/* O03. */
		bool multi_select_ = false;
		asset_drop_handler asset_drop_;

		/* O04. */
		std::string filter_text_;
		filter_match_handler filter_match_;
		editor_context::selection_listener_id selection_sync_id_ = 0;
		bool syncing_selection_ = false;

		/* O05 virtual mode. */
		bool virtual_ = false;
		scene_source source_;
		std::unordered_set<object_id> expanded_;
		mutable std::vector<vrow> vrows_;
		mutable std::unordered_map<object_id, int> vrow_index_;
		mutable bool vrows_dirty_ = true;
		mutable std::string vs_scratch_text_;

		/* O06. */
		bool context_menu_ = false;
		action_handler action_handler_;
		vgui_widget* menu_ = nullptr;
		/* context 级注册块(泄漏式,同 drag_bridge):thunk 经 weak 存活令牌
		 * 校验,析构不回触 vgui_context——后者可能已被销毁(测试顺序)。 */
		void* key_entry_ = nullptr;
		void* focus_entry_ = nullptr;
		bool tree_event_installed_ = false;
	};

} /* namespace vgui_editor_kit */
