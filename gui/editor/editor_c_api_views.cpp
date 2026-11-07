/* gui/editor/editor_c_api_views.cpp - C facade for the outliner and table views
 * declared in <vgui/vgui_editor.h>.
 *
 * Host data sources (scene_source, table_source, table_column) cross the boundary
 * as a prefix-copied struct plus function pointers; each callback captures the
 * pointers and a shared owner by value - never the handle - so the borrowed
 * handle may die before the view releases the host user pointer.
 * The kit has no public filter()/multi_select()/object<->item accessors, so the
 * handle mirrors the filter and multi-select the host sets and rebuilds the id
 * mapping through the tree widget and the virtual projection helpers. */
#include "editor_c_api.h"
#include "editor_c_api_borrow.h"

#include <cstdint>
#include <cstring>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include <vgui/editor/outliner_view.h>
#include <vgui/editor/table_view.h>

using namespace vgui_editor_kit;

/* -------------------------------------------------------------------------- */
/* Handle storage                                                            */
/* -------------------------------------------------------------------------- */

struct vgui_editor_outliner : vgeditor::c_object<outliner_view> {
	using c_object::c_object;

	/* Borrowed getter for filter(); the kit keeps the text private, so the
	 * handle mirrors whatever the host last set. */
	std::string scratch;
	std::string filter_text;
	int multi_select = 0;

	vgeditor::callback_slot<vgui_editor_object_fn> on_selection;
	vgeditor::callback_slot<vgui_editor_move_fn> on_move;
	vgeditor::callback_slot<vgui_editor_rename_fn> on_rename;
	vgeditor::callback_slot<vgui_editor_flag_fn> on_visibility;
	vgeditor::callback_slot<vgui_editor_flag_fn> on_lock;
	vgeditor::callback_slot<vgui_editor_filter_match_fn> on_filter_match;
	vgeditor::callback_slot<vgui_editor_outliner_action_fn> on_action;
	vgeditor::callback_slot<vgui_editor_asset_drop_node_fn> on_asset_drop;
};

struct vgui_editor_table : vgeditor::c_object<table_view> {
	using c_object::c_object;

	/* Borrowed column_at() text; separate members per returned field keep an
	 * earlier c_str() valid while later fields are filled. */
	std::string col_id;
	std::string col_label;
	std::string ed_id;
	std::string ed_label;
	std::string ed_value;
	std::string ed_category;
	std::string ed_tooltip;
	std::string ed_placeholder;
	std::string ed_default;
	std::string ed_asset_filter;
	vgui_editor_property_desc col_editor;

	vgeditor::callback_slot<vgui_editor_table_select_fn> on_selection;
	vgeditor::callback_slot<vgui_editor_table_edit_fn> edit_handler;
	vgeditor::callback_slot<vgui_editor_table_edit_fn> on_edit;
};

namespace {

	/* The C enums mirror the kit's ordering; these asserts turn a future reorder
	 * in either header into a build error. */
	static_assert((int)VGUI_EDITOR_OUTLINER_NEW_CHILD ==
		(int)outliner_view::action::new_child, "outliner action drift");
	static_assert((int)VGUI_EDITOR_OUTLINER_DUPLICATE ==
		(int)outliner_view::action::duplicate, "outliner action drift");
	static_assert((int)VGUI_EDITOR_OUTLINER_RENAME ==
		(int)outliner_view::action::rename, "outliner action drift");
	static_assert((int)VGUI_EDITOR_OUTLINER_REMOVE ==
		(int)outliner_view::action::remove, "outliner action drift");
	static_assert((int)VGUI_EDITOR_OUTLINER_FOCUS ==
		(int)outliner_view::action::focus, "outliner action drift");
	static_assert((int)VGUI_EDITOR_OUTLINER_TOGGLE_VISIBILITY ==
		(int)outliner_view::action::toggle_visibility, "outliner action drift");
	static_assert((int)VGUI_EDITOR_OUTLINER_TOGGLE_LOCK ==
		(int)outliner_view::action::toggle_lock, "outliner action drift");

	static_assert((int)VGUI_EDITOR_DROP_NONE == (int)outliner_view::drop_kind::none,
		"drop kind drift");
	static_assert((int)VGUI_EDITOR_DROP_BEFORE ==
		(int)outliner_view::drop_kind::before, "drop kind drift");
	static_assert((int)VGUI_EDITOR_DROP_AFTER == (int)outliner_view::drop_kind::after,
		"drop kind drift");
	static_assert((int)VGUI_EDITOR_DROP_ONTO == (int)outliner_view::drop_kind::onto,
		"drop kind drift");
	static_assert((int)VGUI_EDITOR_DROP_ROOTS == (int)outliner_view::drop_kind::roots,
		"drop kind drift");

	static_assert((int)VGUI_EDITOR_TABLE_ALIGN_LEFT == (int)table_alignment::left,
		"table alignment drift");
	static_assert((int)VGUI_EDITOR_TABLE_ALIGN_CENTER == (int)table_alignment::center,
		"table alignment drift");
	static_assert((int)VGUI_EDITOR_TABLE_ALIGN_RIGHT == (int)table_alignment::right,
		"table alignment drift");

	/* The table column reuses the property descriptor, so it shares the grid's
	 * type/semantic ordering. */
	static_assert((int)VGUI_EDITOR_PROPERTY_TEXT == (int)property_type::text,
		"property type drift");
	static_assert((int)VGUI_EDITOR_PROPERTY_GRADIENT == (int)property_type::gradient,
		"property type drift");
	static_assert((int)VGUI_EDITOR_VECTOR_GENERIC == (int)vector_semantic::generic,
		"vector semantic drift");
	static_assert((int)VGUI_EDITOR_VECTOR_COLOR == (int)vector_semantic::color,
		"vector semantic drift");

	vgui_editor_outliner_action from_action(outliner_view::action kind)
	{
		return (vgui_editor_outliner_action)(int)kind;
	}

	vgui_editor_drop_kind from_drop_kind(outliner_view::drop_kind kind)
	{
		if (kind < outliner_view::drop_kind::none ||
			kind > outliner_view::drop_kind::roots)
			return VGUI_EDITOR_DROP_NONE;
		return (vgui_editor_drop_kind)(int)kind;
	}

	table_alignment to_alignment(vgui_editor_table_alignment alignment)
	{
		if (alignment < VGUI_EDITOR_TABLE_ALIGN_LEFT ||
			alignment > VGUI_EDITOR_TABLE_ALIGN_RIGHT)
			return table_alignment::left;
		return (table_alignment)alignment;
	}

	vgui_editor_table_alignment from_alignment(table_alignment alignment)
	{
		if (alignment < table_alignment::left || alignment > table_alignment::right)
			return VGUI_EDITOR_TABLE_ALIGN_LEFT;
		return (vgui_editor_table_alignment)(int)alignment;
	}

	property_type to_property_type(vgui_editor_property_type type)
	{
		if (type < VGUI_EDITOR_PROPERTY_TEXT || type > VGUI_EDITOR_PROPERTY_GRADIENT)
			return property_type::text;
		return (property_type)type;
	}

	vector_semantic to_semantic(vgui_editor_vector_semantic semantic)
	{
		if (semantic < VGUI_EDITOR_VECTOR_GENERIC || semantic > VGUI_EDITOR_VECTOR_COLOR)
			return vector_semantic::generic;
		return (vector_semantic)semantic;
	}

	std::vector<char> to_flags(const unsigned char* flags, size_t count)
	{
		std::vector<char> out;
		if (!flags)
			return out;
		out.reserve(count);
		for (size_t i = 0; i < count; ++i)
			out.push_back(flags[i] ? 1 : 0);
		return out;
	}

	/* Copies the descriptor that backs a column's cell editor.  The per-row value
	 * binding is ignored: the table pulls cell text from its source instead. */
	property_desc to_editor_desc(const vgui_editor_property_desc* src)
	{
		property_desc out;
		if (!src)
			return out;
		const vgui_editor_property_desc d = vgeditor::take_desc(src);
		out.id = vgeditor::to_text(d.id);
		out.label = vgeditor::to_text(d.label);
		out.type = to_property_type(d.type);
		out.value = vgeditor::to_text(d.value);
		if (d.category)
			out.category = d.category;
		out.tooltip = vgeditor::to_text(d.tooltip);
		out.placeholder = vgeditor::to_text(d.placeholder);
		out.default_value = vgeditor::to_text(d.default_value);
		out.has_default = d.has_default != 0;
		out.read_only = d.read_only != 0;
		out.has_range = d.has_range != 0;
		out.min_value = d.min_value;
		out.max_value = d.max_value;
		out.step = d.step;
		out.choices = vgeditor::to_text_list(d.choices, d.choice_count);
		if (d.choice_values)
			out.choice_values = vgeditor::to_text_list(d.choice_values,
				d.choice_value_count);
		out.choice_enabled = to_flags(d.choice_enabled, d.choice_count);
		out.allow_empty = d.allow_empty != 0;
		out.semantic = to_semantic(d.semantic);
		out.component_labels = vgeditor::to_text_list(d.component_labels,
			d.component_label_count);
		if (d.component_colors)
			out.component_colors.assign(d.component_colors,
				d.component_colors + d.component_color_count);
		out.asset_filter = vgeditor::to_text(d.asset_filter);
		return out;
	}

	/* C column turned into the kit's value type.  The user pointer travels with a
	 * shared owner so the host data stays alive until the table drops the column.
	 */
	table_column to_column(const vgui_editor_table_column* src)
	{
		table_column out;
		if (!src)
			return out;
		const vgui_editor_table_column d = vgeditor::take_desc(src);
		out.id = vgeditor::to_text(d.id);
		out.label = vgeditor::to_text(d.label);
		out.width = d.width;
		out.min_width = d.min_width;
		out.alignment = to_alignment(d.alignment);
		out.sortable = d.sortable != 0;
		out.editable = d.editable != 0;
		if (d.editor)
			out.editor = to_editor_desc(d.editor);
		vgeditor::user_owner_ptr owner = vgeditor::make_owner(d.user, d.release_user);
		if (d.compare) {
			out.compare = [d, owner](const std::string& a, const std::string& b) {
				return d.compare(owner->user, a.c_str(), b.c_str());
			};
		}
		if (d.set_value) {
			out.set_value = [d, owner](size_t row, const std::string& value) {
				return d.set_value(owner->user, row, value.c_str()) != 0;
			};
		}
		return out;
	}

	std::vector<table_column> to_columns(const vgui_editor_table_column* columns,
		size_t count)
	{
		std::vector<table_column> out;
		if (!columns)
			return out;
		out.reserve(count);
		for (size_t i = 0; i < count; ++i)
			out.push_back(to_column(columns + i));
		return out;
	}

	void fill_cell(const table_cell& cell, vgui_editor_table_cell* out)
	{
		if (!out)
			return;
		std::memset(out, 0, sizeof(*out));
		out->struct_size = (uint32_t)sizeof(*out);
		out->row = cell.row;
		out->column = cell.column;
	}

	/* Pull model conversion.  row_count and cell_text are required (a missing
	 * required callback degrades to an empty model); row_id and cell_style fall
	 * back to the kit defaults when the host leaves them NULL. */
	table_source to_table_source(const vgui_editor_table_source* src)
	{
		table_source out;
		if (!src)
			return out;
		const vgui_editor_table_source d = vgeditor::take_desc(src);
		vgeditor::user_owner_ptr owner = vgeditor::make_owner(d.user, d.release_user);
		out.row_count = [d, owner]() -> size_t {
			return d.row_count ? d.row_count(owner->user) : 0;
		};
		if (d.cell_text) {
			out.cell_text = [d, owner](size_t row, const std::string& column) {
				const char* key = column.c_str();
				return vgeditor::read_text([d, owner, row, key](char* text,
					size_t cap) {
					return d.cell_text(owner->user, row, key, text, cap);
				}).value;
			};
		}
		if (d.row_id) {
			out.row_id = [d, owner](size_t row) -> object_id {
				return (object_id)d.row_id(owner->user, row);
			};
		}
		if (d.cell_style) {
			out.cell_style = [d, owner](size_t row,
				const std::string& column) -> table_cell_style {
				table_cell_style style;
				vgui_editor_table_cell_style view;
				std::memset(&view, 0, sizeof(view));
				view.struct_size = (uint32_t)sizeof(view);
				d.cell_style(owner->user, row, column.c_str(), &view);
				style.text_color = view.text_color;
				style.background = view.background;
				return style;
			};
		}
		return out;
	}

	/* Outliner virtual adapter.  Names cross as borrowed host strings copied by
	 * to_text; the tooltip keeps the host pointer because the kit only borrows it
	 * for the duration of the call, matching its own contract. */
	outliner_view::scene_source to_scene_source(const vgui_editor_scene_source* src)
	{
		outliner_view::scene_source out;
		if (!src)
			return out;
		const vgui_editor_scene_source d = vgeditor::take_desc(src);
		vgeditor::user_owner_ptr owner = vgeditor::make_owner(d.user, d.release_user);

		out.root_count = [d, owner]() -> size_t {
			return d.root_count ? d.root_count(owner->user) : 0;
		};
		out.root_at = [d, owner](size_t index) -> object_id {
			return d.root_at ? (object_id)d.root_at(owner->user, index) :
				invalid_object;
		};
		out.child_count = [d, owner](object_id node) -> size_t {
			return d.child_count ? d.child_count(owner->user, (vgui_editor_object_id)node)
				: 0;
		};
		out.child_at = [d, owner](object_id parent, size_t index) -> object_id {
			return d.child_at ? (object_id)d.child_at(owner->user,
				(vgui_editor_object_id)parent, index) : invalid_object;
		};
		out.name = [d, owner](object_id node) -> std::string {
			return d.name ? vgeditor::to_text(d.name(owner->user,
				(vgui_editor_object_id)node)) : std::string();
		};
		if (d.parent_of) {
			out.parent_of = [d, owner](object_id node) -> object_id {
				return (object_id)d.parent_of(owner->user, (vgui_editor_object_id)node);
			};
		}
		if (d.icon) {
			out.icon = [d, owner](object_id node) -> vgui_texture* {
				return d.icon(owner->user, (vgui_editor_object_id)node);
			};
		}
		if (d.enabled) {
			out.enabled = [d, owner](object_id node) -> int {
				return d.enabled(owner->user, (vgui_editor_object_id)node);
			};
		}
		if (d.tooltip) {
			out.tooltip = [d, owner](object_id node) -> const char* {
				return d.tooltip(owner->user, (vgui_editor_object_id)node);
			};
		}
		if (d.text_color) {
			out.text_color = [d, owner](object_id node) -> vgui_color {
				return d.text_color(owner->user, (vgui_editor_object_id)node);
			};
		}
		return out;
	}

	/* object_for_item()/item_for_object() are private in the kit, so the facade
	 * reproduces their mapping from the public tree widget and projection. */
	vgui_editor_object_id object_for_item(outliner_view& view, vgui_item_id item)
	{
		if (view.is_virtual())
			return item != 0 ? (vgui_editor_object_id)item : VGUI_EDITOR_INVALID_OBJECT;
		vgui_widget* tree = view.widget();
		if (!tree || item == 0)
			return VGUI_EDITOR_INVALID_OBJECT;
		void* value = vgui_tree_get_item_user(tree, item);
		vgui_editor_object_id object =
			(vgui_editor_object_id)(std::uintptr_t)value;
		if (object == VGUI_EDITOR_INVALID_OBJECT)
			object = (vgui_editor_object_id)item;
		return object;
	}

	vgui_item_id item_for_object(outliner_view& view, vgui_editor_object_id object)
	{
		vgui_widget* tree = view.widget();
		if (!tree || object == VGUI_EDITOR_INVALID_OBJECT)
			return 0;
		if (view.is_virtual()) {
			const size_t rows = view.projected_row_count();
			for (size_t i = 0; i < rows; ++i) {
				if (view.vrow_at_for_test(i) == (object_id)object)
					return (vgui_item_id)object;
			}
			return 0;
		}

		std::vector<vgui_item_id> stack;
		for (int i = vgui_tree_get_root_count(tree) - 1; i >= 0; --i)
			stack.push_back(vgui_tree_get_root(tree, i));
		while (!stack.empty()) {
			const vgui_item_id item = stack.back();
			stack.pop_back();
			if (object_for_item(view, item) == object)
				return item;
			for (int i = vgui_tree_get_child_count(tree, item) - 1; i >= 0; --i)
				stack.push_back(vgui_tree_get_child(tree, item, i));
		}
		return 0;
	}

} /* namespace */

extern "C" {

	/* ------------------------------------------------------------------------- */
	/* Outliner                                                                  */
	/* ------------------------------------------------------------------------- */

	void vgui_editor_drop_location_default(vgui_editor_drop_location* location)
	{
		if (!location)
			return;
		std::memset(location, 0, sizeof(*location));
		location->struct_size = (uint32_t)sizeof(*location);
		location->kind = VGUI_EDITOR_DROP_NONE;
		location->parent = VGUI_EDITOR_INVALID_OBJECT;
		location->index = 0;
		location->row = 0;
	}

	void vgui_editor_scene_source_default(vgui_editor_scene_source* source)
	{
		if (!source)
			return;
		std::memset(source, 0, sizeof(*source));
		source->struct_size = (uint32_t)sizeof(*source);
	}

	vgui_editor_outliner* vgui_editor_outliner_create(vgui_editor_context* context,
		vgui_widget* parent)
	{
		if (!context || !parent)
			return nullptr;
		editor_context* target = vgeditor::context_impl(context);
		if (!target)
			return nullptr;
		std::unique_ptr<vgui_editor_outliner> outliner(
			vgeditor::own_object<vgui_editor_outliner>(
				new (std::nothrow) outliner_view()));
		if (!outliner)
			return nullptr;
		outliner->impl.build(*target, parent);
		if (!outliner->impl.widget())
			return nullptr;
		return outliner.release();
	}

	void vgui_editor_outliner_destroy(vgui_editor_outliner* outliner)
	{
		if (!outliner || !outliner->owns_object())
			return;
		outliner->impl.set_selection_listener(nullptr);
		outliner->impl.set_move_handler(nullptr);
		outliner->impl.set_rename_handler(nullptr);
		outliner->impl.set_visibility_listener(nullptr);
		outliner->impl.set_lock_listener(nullptr);
		outliner->impl.set_asset_drop_handler(nullptr);
		outliner->impl.set_filter_match_handler(nullptr);
		outliner->impl.set_action_handler(nullptr);
		outliner->impl.set_command_stack(nullptr);
		outliner->impl.set_scene_source(outliner_view::scene_source());
		vgui_widget* widget = outliner->impl.widget();
		delete outliner;
		if (widget)
			vgui_widget_destroy(widget);
	}

	vgui_widget* vgui_editor_outliner_widget(vgui_editor_outliner* outliner)
	{
		return outliner ? outliner->impl.widget() : nullptr;
	}

	void vgui_editor_outliner_clear(vgui_editor_outliner* outliner)
	{
		if (!outliner)
			return;
		outliner->impl.clear();
	}

	vgui_item_id vgui_editor_outliner_add_node(vgui_editor_outliner* outliner,
		const char* text, vgui_item_id parent, vgui_editor_object_id object)
	{
		if (!outliner)
			return 0;
		return outliner->impl.add_node(text ? text : "", parent, (object_id)object);
	}

	void vgui_editor_outliner_expand(vgui_editor_outliner* outliner,
		vgui_item_id node)
	{
		if (!outliner)
			return;
		outliner->impl.expand(node);
	}

	vgui_item_id vgui_editor_outliner_selected(vgui_editor_outliner* outliner)
	{
		return outliner ? outliner->impl.selected() : 0;
	}

	vgui_editor_object_id vgui_editor_outliner_object_for_item(
		vgui_editor_outliner* outliner, vgui_item_id item)
	{
		if (!outliner)
			return VGUI_EDITOR_INVALID_OBJECT;
		return object_for_item(outliner->impl, item);
	}

	vgui_item_id vgui_editor_outliner_item_for_object(
		vgui_editor_outliner* outliner, vgui_editor_object_id object)
	{
		if (!outliner)
			return 0;
		return item_for_object(outliner->impl, object);
	}

	void vgui_editor_outliner_set_on_selection(vgui_editor_outliner* outliner,
		vgui_editor_object_fn fn, void* user)
	{
		if (!outliner)
			return;
		if (!fn) {
			outliner->on_selection.clear();
			outliner->impl.set_selection_listener(nullptr);
			return;
		}
		outliner->on_selection.set(fn, user, nullptr);
		auto fn_ = outliner->on_selection.fn;
		auto user_ = outliner->on_selection.user;
		auto owner_ = outliner->on_selection.owner;
		outliner->impl.set_selection_listener([fn_, user_, owner_](object_id id) {
			if (fn_)
				fn_(user_, (vgui_editor_object_id)id);
		});
	}

	void vgui_editor_outliner_set_command_stack(vgui_editor_outliner* outliner,
		vgui_editor_command_stack* stack)
	{
		if (!outliner)
			return;
		outliner->impl.set_command_stack(vgeditor::stack_impl(stack));
	}

	void vgui_editor_outliner_set_move_handler(vgui_editor_outliner* outliner,
		vgui_editor_move_fn fn, void* user)
	{
		if (!outliner)
			return;
		if (!fn) {
			outliner->on_move.clear();
			outliner->impl.set_move_handler(nullptr);
			return;
		}
		outliner->on_move.set(fn, user, nullptr);
		auto fn_ = outliner->on_move.fn;
		auto user_ = outliner->on_move.user;
		auto owner_ = outliner->on_move.owner;
		outliner->impl.set_move_handler([fn_, user_, owner_](object_id node,
			object_id parent, size_t index) {
			if (!fn_)
				return false;
			return fn_(user_, (vgui_editor_object_id)node,
				(vgui_editor_object_id)parent, index) != 0;
		});
	}

	int vgui_editor_outliner_move_node(vgui_editor_outliner* outliner,
		vgui_editor_object_id node, vgui_editor_object_id new_parent, size_t index)
	{
		if (!outliner)
			return 0;
		return outliner->impl.move_node((object_id)node, (object_id)new_parent,
			index) ? 1 : 0;
	}

	void vgui_editor_outliner_set_node_icon(vgui_editor_outliner* outliner,
		vgui_item_id node, vgui_texture* icon)
	{
		if (!outliner)
			return;
		outliner->impl.set_node_icon(node, icon);
	}

	void vgui_editor_outliner_set_node_text_color(vgui_editor_outliner* outliner,
		vgui_item_id node, vgui_color color)
	{
		if (!outliner)
			return;
		outliner->impl.set_node_text_color(node, color);
	}

	void vgui_editor_outliner_enable_row_buttons(vgui_editor_outliner* outliner)
	{
		if (!outliner)
			return;
		outliner->impl.enable_row_buttons();
	}

	void vgui_editor_outliner_set_row_button_icons(vgui_editor_outliner* outliner,
		vgui_texture* visible_on, vgui_texture* visible_off, vgui_texture* locked_on,
		vgui_texture* locked_off)
	{
		if (!outliner)
			return;
		outliner->impl.set_row_button_icons(visible_on, visible_off, locked_on,
			locked_off);
	}

	void vgui_editor_outliner_set_object_visibility(vgui_editor_outliner* outliner,
		vgui_editor_object_id object, int visible)
	{
		if (!outliner)
			return;
		outliner->impl.set_object_visibility((object_id)object, visible != 0);
	}

	void vgui_editor_outliner_set_object_locked(vgui_editor_outliner* outliner,
		vgui_editor_object_id object, int locked)
	{
		if (!outliner)
			return;
		outliner->impl.set_object_locked((object_id)object, locked != 0);
	}

	void vgui_editor_outliner_toggle_object_visibility(
		vgui_editor_outliner* outliner, vgui_editor_object_id object)
	{
		if (!outliner)
			return;
		outliner->impl.toggle_object_visibility((object_id)object);
	}

	void vgui_editor_outliner_toggle_object_lock(vgui_editor_outliner* outliner,
		vgui_editor_object_id object)
	{
		if (!outliner)
			return;
		outliner->impl.toggle_object_lock((object_id)object);
	}

	int vgui_editor_outliner_object_visibility(vgui_editor_outliner* outliner,
		vgui_editor_object_id object)
	{
		if (!outliner)
			return 0;
		return outliner->impl.object_visibility((object_id)object) ? 1 : 0;
	}

	int vgui_editor_outliner_object_locked(vgui_editor_outliner* outliner,
		vgui_editor_object_id object)
	{
		if (!outliner)
			return 0;
		return outliner->impl.object_locked((object_id)object) ? 1 : 0;
	}

	void vgui_editor_outliner_set_on_visibility(vgui_editor_outliner* outliner,
		vgui_editor_flag_fn fn, void* user)
	{
		if (!outliner)
			return;
		if (!fn) {
			outliner->on_visibility.clear();
			outliner->impl.set_visibility_listener(nullptr);
			return;
		}
		outliner->on_visibility.set(fn, user, nullptr);
		auto fn_ = outliner->on_visibility.fn;
		auto user_ = outliner->on_visibility.user;
		auto owner_ = outliner->on_visibility.owner;
		outliner->impl.set_visibility_listener([fn_, user_, owner_](object_id node,
			bool state) {
			if (fn_)
				fn_(user_, (vgui_editor_object_id)node, state ? 1 : 0);
		});
	}

	void vgui_editor_outliner_set_on_lock(vgui_editor_outliner* outliner,
		vgui_editor_flag_fn fn, void* user)
	{
		if (!outliner)
			return;
		if (!fn) {
			outliner->on_lock.clear();
			outliner->impl.set_lock_listener(nullptr);
			return;
		}
		outliner->on_lock.set(fn, user, nullptr);
		auto fn_ = outliner->on_lock.fn;
		auto user_ = outliner->on_lock.user;
		auto owner_ = outliner->on_lock.owner;
		outliner->impl.set_lock_listener([fn_, user_, owner_](object_id node,
			bool state) {
			if (fn_)
				fn_(user_, (vgui_editor_object_id)node, state ? 1 : 0);
		});
	}

	void vgui_editor_outliner_set_rename_handler(vgui_editor_outliner* outliner,
		vgui_editor_rename_fn fn, void* user)
	{
		if (!outliner)
			return;
		if (!fn) {
			outliner->on_rename.clear();
			outliner->impl.set_rename_handler(nullptr);
			return;
		}
		outliner->on_rename.set(fn, user, nullptr);
		auto fn_ = outliner->on_rename.fn;
		auto user_ = outliner->on_rename.user;
		auto owner_ = outliner->on_rename.owner;
		outliner->impl.set_rename_handler([fn_, user_, owner_](object_id node,
			const std::string& name) {
			if (!fn_)
				return false;
			return fn_(user_, (vgui_editor_object_id)node, name.c_str()) != 0;
		});
	}

	int vgui_editor_outliner_rename_node(vgui_editor_outliner* outliner,
		vgui_editor_object_id node, const char* name)
	{
		if (!outliner)
			return 0;
		return outliner->impl.rename_node((object_id)node,
			vgeditor::to_text(name)) ? 1 : 0;
	}

	void vgui_editor_outliner_enable_inline_rename(vgui_editor_outliner* outliner)
	{
		if (!outliner)
			return;
		outliner->impl.enable_inline_rename();
	}

	void vgui_editor_outliner_begin_rename(vgui_editor_outliner* outliner,
		vgui_item_id node)
	{
		if (!outliner)
			return;
		outliner->impl.begin_rename(node);
	}

	int vgui_editor_outliner_renaming(vgui_editor_outliner* outliner)
	{
		return outliner && outliner->impl.renaming_active() ? 1 : 0;
	}

	void vgui_editor_outliner_enable_drag_drop(vgui_editor_outliner* outliner)
	{
		if (!outliner)
			return;
		outliner->impl.enable_drag_drop();
	}

	void vgui_editor_outliner_set_multi_select(vgui_editor_outliner* outliner,
		int enabled)
	{
		if (!outliner)
			return;
		outliner->multi_select = enabled ? 1 : 0;
		outliner->impl.set_multi_select(enabled != 0);
	}

	int vgui_editor_outliner_multi_select(vgui_editor_outliner* outliner)
	{
		return outliner ? outliner->multi_select : 0;
	}

	void vgui_editor_outliner_set_asset_drop_handler(
		vgui_editor_outliner* outliner, vgui_editor_asset_drop_node_fn fn,
		void* user)
	{
		if (!outliner)
			return;
		if (!fn) {
			outliner->on_asset_drop.clear();
			outliner->impl.set_asset_drop_handler(nullptr);
			return;
		}
		outliner->on_asset_drop.set(fn, user, nullptr);
		auto fn_ = outliner->on_asset_drop.fn;
		auto user_ = outliner->on_asset_drop.user;
		auto owner_ = outliner->on_asset_drop.owner;
		outliner->impl.set_asset_drop_handler([fn_, user_, owner_](
			const std::string& asset, object_id parent, size_t index) {
			if (!fn_)
				return false;
			return fn_(user_, asset.c_str(), (vgui_editor_object_id)parent,
				index) != 0;
		});
	}

	int vgui_editor_outliner_asset_drop_enabled(vgui_editor_outliner* outliner)
	{
		return outliner && outliner->impl.asset_drop_enabled() ? 1 : 0;
	}

	int vgui_editor_outliner_apply_asset_drop(vgui_editor_outliner* outliner,
		const char* asset, float x, float y)
	{
		if (!outliner)
			return 0;
		return outliner->impl.apply_asset_drop(vgeditor::to_text(asset), x, y) ? 1 : 0;
	}

	int vgui_editor_outliner_apply_drop_payload(vgui_editor_outliner* outliner,
		const char* payload_id, float x, float y)
	{
		if (!outliner)
			return 0;
		return outliner->impl.apply_drop_by_payload(vgeditor::to_text(payload_id),
			x, y) ? 1 : 0;
	}

	void vgui_editor_outliner_drop_location_at(vgui_editor_outliner* outliner,
		float x, float y, vgui_editor_drop_location* out)
	{
		if (!outliner || !out)
			return;
		const outliner_view::drop_location loc = outliner->impl.drop_location_at(x, y);
		std::memset(out, 0, sizeof(*out));
		out->struct_size = (uint32_t)sizeof(*out);
		out->kind = from_drop_kind(loc.kind);
		out->parent = (vgui_editor_object_id)loc.parent;
		out->index = loc.index;
		out->row = loc.row;
	}

	int vgui_editor_outliner_row_drop_location(vgui_editor_outliner* outliner,
		float x, float y, vgui_editor_object_id* out_parent, size_t* out_index)
	{
		if (!outliner)
			return 0;
		return outliner->impl.row_drop_location(x, y, out_parent, out_index) ? 1 : 0;
	}

	void vgui_editor_outliner_set_filter(vgui_editor_outliner* outliner,
		const char* text)
	{
		if (!outliner)
			return;
		outliner->filter_text = vgeditor::to_text(text);
		outliner->impl.set_filter(text);
	}

	const char* vgui_editor_outliner_filter(vgui_editor_outliner* outliner)
	{
		return outliner ? outliner->filter_text.c_str() : "";
	}

	void vgui_editor_outliner_set_filter_match_handler(
		vgui_editor_outliner* outliner, vgui_editor_filter_match_fn fn, void* user)
	{
		if (!outliner)
			return;
		if (!fn) {
			outliner->on_filter_match.clear();
			outliner->impl.set_filter_match_handler(nullptr);
			return;
		}
		outliner->on_filter_match.set(fn, user, nullptr);
		auto fn_ = outliner->on_filter_match.fn;
		auto user_ = outliner->on_filter_match.user;
		auto owner_ = outliner->on_filter_match.owner;
		outliner->impl.set_filter_match_handler([fn_, user_, owner_](object_id node,
			const char* name) {
			if (!fn_)
				return true;
			return fn_(user_, (vgui_editor_object_id)node, name) != 0;
		});
	}

	void vgui_editor_outliner_enable_selection_sync(vgui_editor_outliner* outliner)
	{
		if (!outliner)
			return;
		outliner->impl.enable_selection_sync();
	}

	void vgui_editor_outliner_set_scene_source(vgui_editor_outliner* outliner,
		const vgui_editor_scene_source* source)
	{
		if (!outliner)
			return;
		outliner->impl.set_scene_source(source ? to_scene_source(source) :
			outliner_view::scene_source());
		/* The kit only installs the tree row source; clearing the source leaves the
		 * tree virtual, so the facade has to detach it to restore the model. */
		if (!outliner->impl.is_virtual()) {
			vgui_widget* tree = outliner->impl.widget();
			if (tree && vgui_tree_is_virtual(tree))
				vgui_tree_set_row_source(tree, nullptr);
		}
	}

	int vgui_editor_outliner_is_virtual(vgui_editor_outliner* outliner)
	{
		return outliner && outliner->impl.is_virtual() ? 1 : 0;
	}

	void vgui_editor_outliner_scene_changed(vgui_editor_outliner* outliner)
	{
		if (!outliner)
			return;
		outliner->impl.scene_changed();
	}

	void vgui_editor_outliner_scene_row_changed(vgui_editor_outliner* outliner,
		vgui_editor_object_id object)
	{
		if (!outliner)
			return;
		outliner->impl.scene_row_changed((object_id)object);
	}

	size_t vgui_editor_outliner_projected_rows(vgui_editor_outliner* outliner)
	{
		return outliner ? outliner->impl.projected_row_count() : 0;
	}

	size_t vgui_editor_outliner_selection_ids(vgui_editor_outliner* outliner,
		vgui_editor_object_id* out, size_t cap)
	{
		if (!outliner)
			return 0;
		const std::vector<object_id> ids = outliner->impl.selection_objects();
		const size_t written = out ? (ids.size() < cap ? ids.size() : cap) : 0;
		for (size_t i = 0; i < written; ++i)
			out[i] = (vgui_editor_object_id)ids[i];
		return ids.size();
	}

	void vgui_editor_outliner_enable_context_menu(vgui_editor_outliner* outliner)
	{
		if (!outliner)
			return;
		outliner->impl.enable_context_menu();
	}

	void vgui_editor_outliner_set_action_handler(vgui_editor_outliner* outliner,
		vgui_editor_outliner_action_fn fn, void* user)
	{
		if (!outliner)
			return;
		if (!fn) {
			outliner->on_action.clear();
			outliner->impl.set_action_handler(nullptr);
			return;
		}
		outliner->on_action.set(fn, user, nullptr);
		auto fn_ = outliner->on_action.fn;
		auto user_ = outliner->on_action.user;
		auto owner_ = outliner->on_action.owner;
		outliner->impl.set_action_handler([fn_, user_, owner_](
			outliner_view::action kind, const std::vector<object_id>& nodes) {
			if (!fn_)
				return false;
			return fn_(user_, from_action(kind), nodes.data(), nodes.size()) != 0;
		});
	}

	/* ------------------------------------------------------------------------- */
	/* Table view                                                                */
	/* ------------------------------------------------------------------------- */

	void vgui_editor_table_cell_default(vgui_editor_table_cell* cell)
	{
		if (!cell)
			return;
		std::memset(cell, 0, sizeof(*cell));
		cell->struct_size = (uint32_t)sizeof(*cell);
		cell->row = 0;
		cell->column = VGUI_EDITOR_TABLE_NO_COLUMN;
	}

	void vgui_editor_table_cell_style_default(vgui_editor_table_cell_style* style)
	{
		if (!style)
			return;
		std::memset(style, 0, sizeof(*style));
		style->struct_size = (uint32_t)sizeof(*style);
	}

	void vgui_editor_table_column_default(vgui_editor_table_column* column)
	{
		if (!column)
			return;
		std::memset(column, 0, sizeof(*column));
		column->struct_size = (uint32_t)sizeof(*column);
		column->id = nullptr;
		column->label = nullptr;
		column->width = 120.0f;
		column->min_width = 40.0f;
		column->alignment = VGUI_EDITOR_TABLE_ALIGN_LEFT;
		column->sortable = 1;
		column->editable = 1;
		column->editor = nullptr;
		column->compare = nullptr;
		column->set_value = nullptr;
		column->user = nullptr;
		column->release_user = nullptr;
	}

	void vgui_editor_table_source_default(vgui_editor_table_source* source)
	{
		if (!source)
			return;
		std::memset(source, 0, sizeof(*source));
		source->struct_size = (uint32_t)sizeof(*source);
	}

	vgui_editor_table* vgui_editor_table_create(vgui_widget* parent)
	{
		if (!parent)
			return nullptr;
		std::unique_ptr<vgui_editor_table> table(
			vgeditor::own_object<vgui_editor_table>(new (std::nothrow) table_view()));
		if (!table)
			return nullptr;
		table->impl.build(parent);
		if (!table->impl.widget())
			return nullptr;
		return table.release();
	}

	void vgui_editor_table_destroy(vgui_editor_table* table)
	{
		if (!table || !table->owns_object())
			return;
		table->impl.set_selection_listener(nullptr);
		table->impl.set_edit_handler(nullptr);
		table->impl.set_edit_listener(nullptr);
		table->impl.set_command_stack(nullptr);
		table->impl.set_source(table_source());
		vgui_widget* widget = table->impl.widget();
		delete table;
		if (widget)
			vgui_widget_destroy(widget);
	}

	vgui_widget* vgui_editor_table_widget(vgui_editor_table* table)
	{
		return table ? table->impl.widget() : nullptr;
	}

	void vgui_editor_table_focus(vgui_editor_table* table)
	{
		if (!table)
			return;
		table->impl.focus();
	}

	int vgui_editor_table_built(vgui_editor_table* table)
	{
		return table && table->impl.built() ? 1 : 0;
	}

	void vgui_editor_table_set_source(vgui_editor_table* table,
		const vgui_editor_table_source* source)
	{
		if (!table)
			return;
		table->impl.set_source(source ? to_table_source(source) : table_source());
	}

	int vgui_editor_table_add_column(vgui_editor_table* table,
		const vgui_editor_table_column* column)
	{
		if (!table || !column)
			return 0;
		return table->impl.add_column(to_column(column)) ? 1 : 0;
	}

	void vgui_editor_table_set_columns(vgui_editor_table* table,
		const vgui_editor_table_column* columns, size_t count)
	{
		if (!table)
			return;
		table->impl.set_columns(to_columns(columns, count));
	}

	void vgui_editor_table_clear_columns(vgui_editor_table* table)
	{
		if (!table)
			return;
		table->impl.clear_columns();
	}

	size_t vgui_editor_table_column_count(vgui_editor_table* table)
	{
		return table ? table->impl.column_count() : 0;
	}

	int vgui_editor_table_column_at(vgui_editor_table* table, size_t index,
		vgui_editor_table_column* out)
	{
		if (!table || !out)
			return 0;
		const std::vector<table_column>& columns = table->impl.columns();
		if (index >= columns.size())
			return 0;
		const table_column& column = columns[index];

		/* Borrowed text lives in handle scratch so an earlier c_str() stays valid
		 * while later fields are filled. */
		table->col_id = column.id;
		table->col_label = column.label;

		std::memset(out, 0, sizeof(*out));
		out->struct_size = (uint32_t)sizeof(*out);
		out->id = table->col_id.c_str();
		out->label = table->col_label.c_str();
		out->width = column.width;
		out->min_width = column.min_width;
		out->alignment = from_alignment(column.alignment);
		out->sortable = column.sortable ? 1 : 0;
		out->editable = column.editable ? 1 : 0;

		const property_desc& ed = column.editor;
		table->ed_id = ed.id;
		table->ed_label = ed.label;
		table->ed_value = ed.value;
		table->ed_category = ed.category;
		table->ed_tooltip = ed.tooltip;
		table->ed_placeholder = ed.placeholder;
		table->ed_default = ed.default_value;
		table->ed_asset_filter = ed.asset_filter;

		std::memset(&table->col_editor, 0, sizeof(table->col_editor));
		table->col_editor.struct_size = (uint32_t)sizeof(table->col_editor);
		table->col_editor.id = table->ed_id.c_str();
		table->col_editor.label = table->ed_label.c_str();
		table->col_editor.type = (vgui_editor_property_type)ed.type;
		table->col_editor.value = table->ed_value.c_str();
		table->col_editor.category = table->ed_category.c_str();
		table->col_editor.tooltip = table->ed_tooltip.c_str();
		table->col_editor.placeholder = table->ed_placeholder.c_str();
		table->col_editor.default_value = table->ed_default.c_str();
		table->col_editor.has_default = ed.has_default ? 1 : 0;
		table->col_editor.read_only = ed.read_only ? 1 : 0;
		table->col_editor.has_range = ed.has_range ? 1 : 0;
		table->col_editor.min_value = ed.min_value;
		table->col_editor.max_value = ed.max_value;
		table->col_editor.step = ed.step;
		table->col_editor.allow_empty = ed.allow_empty ? 1 : 0;
		table->col_editor.semantic = (vgui_editor_vector_semantic)ed.semantic;
		table->col_editor.asset_filter = table->ed_asset_filter.c_str();
		out->editor = &table->col_editor;

		/* Host callbacks cannot be reconstructed from the kit column; the caller
		 * keeps its own comparator/setter if it needs them back. */
		out->compare = nullptr;
		out->set_value = nullptr;
		out->user = nullptr;
		out->release_user = nullptr;
		return 1;
	}

	int vgui_editor_table_set_column_width(vgui_editor_table* table, size_t index,
		float width)
	{
		if (!table)
			return 0;
		return table->impl.set_column_width(index, width) ? 1 : 0;
	}

	float vgui_editor_table_column_width(vgui_editor_table* table, size_t index)
	{
		return table ? table->impl.column_width(index) : 0.0f;
	}

	void vgui_editor_table_refresh(vgui_editor_table* table)
	{
		if (!table)
			return;
		table->impl.refresh();
	}

	void vgui_editor_table_refresh_row(vgui_editor_table* table, size_t source_row)
	{
		if (!table)
			return;
		table->impl.refresh_row(source_row);
	}

	size_t vgui_editor_table_row_count(vgui_editor_table* table)
	{
		return table ? table->impl.row_count() : 0;
	}

	size_t vgui_editor_table_display_row_count(vgui_editor_table* table)
	{
		return table ? table->impl.display_row_count() : 0;
	}

	size_t vgui_editor_table_source_row_at(vgui_editor_table* table,
		size_t display_row)
	{
		return table ? table->impl.source_row_at(display_row) : 0;
	}

	int vgui_editor_table_display_index_of(vgui_editor_table* table,
		size_t source_row)
	{
		return table ? table->impl.display_index_of(source_row) : -1;
	}

	void vgui_editor_table_visible_row_range(vgui_editor_table* table, size_t* first,
		size_t* count)
	{
		if (!table)
			return;
		const table_row_range range = table->impl.visible_row_range();
		if (first)
			*first = range.first;
		if (count)
			*count = range.count;
	}

	int vgui_editor_table_sort_column(vgui_editor_table* table)
	{
		return table ? table->impl.sort_column() : -1;
	}

	int vgui_editor_table_sort_ascending(vgui_editor_table* table)
	{
		return table && table->impl.sort_ascending() ? 1 : 0;
	}

	int vgui_editor_table_set_sort(vgui_editor_table* table, int column,
		int ascending)
	{
		if (!table)
			return 0;
		return table->impl.set_sort(column, ascending != 0) ? 1 : 0;
	}

	void vgui_editor_table_clear_sort(vgui_editor_table* table)
	{
		if (!table)
			return;
		table->impl.clear_sort();
	}

	void vgui_editor_table_selected_cell(vgui_editor_table* table,
		vgui_editor_table_cell* out)
	{
		if (!table || !out)
			return;
		fill_cell(table->impl.selected_cell(), out);
	}

	int vgui_editor_table_select_cell(vgui_editor_table* table, size_t source_row,
		int column)
	{
		if (!table)
			return 0;
		return table->impl.select_cell(source_row, column) ? 1 : 0;
	}

	void vgui_editor_table_clear_selection(vgui_editor_table* table)
	{
		if (!table)
			return;
		table->impl.clear_selection();
	}

	void vgui_editor_table_hit_test(vgui_editor_table* table, float content_x,
		float content_y, vgui_editor_table_cell* out)
	{
		if (!table)
			return;
		fill_cell(table->impl.hit_test(content_x, content_y), out);
	}

	void vgui_editor_table_set_on_selection(vgui_editor_table* table,
		vgui_editor_table_select_fn fn, void* user)
	{
		if (!table)
			return;
		if (!fn) {
			table->on_selection.clear();
			table->impl.set_selection_listener(nullptr);
			return;
		}
		table->on_selection.set(fn, user, nullptr);
		auto fn_ = table->on_selection.fn;
		auto user_ = table->on_selection.user;
		auto owner_ = table->on_selection.owner;
		table->impl.set_selection_listener([fn_, user_, owner_](
			const table_cell& cell) {
			if (!fn_)
				return;
			vgui_editor_table_cell view;
			fill_cell(cell, &view);
			fn_(user_, &view);
		});
	}

	int vgui_editor_table_cell_rect(vgui_editor_table* table, size_t source_row,
		int column, float* x, float* y, float* width, float* height)
	{
		if (!table)
			return 0;
		return table->impl.cell_rect(source_row, column, x, y, width, height) ? 1 : 0;
	}

	int vgui_editor_table_header_rect(vgui_editor_table* table, int column, float* x,
		float* y, float* width, float* height)
	{
		if (!table)
			return 0;
		return table->impl.header_rect(column, x, y, width, height) ? 1 : 0;
	}

	float vgui_editor_table_scroll_x(vgui_editor_table* table)
	{
		return table ? table->impl.scroll_x() : 0.0f;
	}

	float vgui_editor_table_scroll_y(vgui_editor_table* table)
	{
		return table ? table->impl.scroll_y() : 0.0f;
	}

	void vgui_editor_table_set_scroll(vgui_editor_table* table, float x, float y)
	{
		if (!table)
			return;
		table->impl.set_scroll(x, y);
	}

	float vgui_editor_table_max_scroll_x(vgui_editor_table* table)
	{
		return table ? table->impl.max_scroll_x() : 0.0f;
	}

	float vgui_editor_table_max_scroll_y(vgui_editor_table* table)
	{
		return table ? table->impl.max_scroll_y() : 0.0f;
	}

	void vgui_editor_table_scroll_to_row(vgui_editor_table* table, size_t source_row)
	{
		if (!table)
			return;
		table->impl.scroll_to_row(source_row);
	}

	void vgui_editor_table_set_row_height(vgui_editor_table* table, float height)
	{
		if (!table)
			return;
		table->impl.set_row_height(height);
	}

	float vgui_editor_table_row_height(vgui_editor_table* table)
	{
		return table ? table->impl.row_height() : 0.0f;
	}

	float vgui_editor_table_header_height(vgui_editor_table* table)
	{
		return table ? table->impl.header_height() : 0.0f;
	}

	int vgui_editor_table_begin_edit(vgui_editor_table* table)
	{
		if (!table)
			return 0;
		return table->impl.begin_edit() ? 1 : 0;
	}

	int vgui_editor_table_begin_edit_cell(vgui_editor_table* table, size_t source_row,
		int column)
	{
		if (!table)
			return 0;
		return table->impl.begin_edit(source_row, column) ? 1 : 0;
	}

	int vgui_editor_table_cancel_edit(vgui_editor_table* table)
	{
		if (!table)
			return 0;
		return table->impl.cancel_edit() ? 1 : 0;
	}

	int vgui_editor_table_editing(vgui_editor_table* table)
	{
		return table && table->impl.editing() ? 1 : 0;
	}

	void vgui_editor_table_editing_cell(vgui_editor_table* table,
		vgui_editor_table_cell* out)
	{
		if (!table || !out)
			return;
		fill_cell(table->impl.editing_cell(), out);
	}

	int vgui_editor_table_commit_edit(vgui_editor_table* table, const char* value)
	{
		if (!table)
			return 0;
		return table->impl.commit_edit(vgeditor::to_text(value)) ? 1 : 0;
	}

	void vgui_editor_table_set_edit_handler(vgui_editor_table* table,
		vgui_editor_table_edit_fn fn, void* user)
	{
		if (!table)
			return;
		if (!fn) {
			table->edit_handler.clear();
			table->impl.set_edit_handler(nullptr);
			return;
		}
		table->edit_handler.set(fn, user, nullptr);
		auto fn_ = table->edit_handler.fn;
		auto user_ = table->edit_handler.user;
		auto owner_ = table->edit_handler.owner;
		table->impl.set_edit_handler([fn_, user_, owner_](const table_cell& cell,
			const std::string& value) {
			if (!fn_)
				return false;
			vgui_editor_table_cell view;
			fill_cell(cell, &view);
			return fn_(user_, &view, value.c_str()) != 0;
		});
	}

	void vgui_editor_table_set_on_edit(vgui_editor_table* table,
		vgui_editor_table_edit_fn fn, void* user)
	{
		if (!table)
			return;
		if (!fn) {
			table->on_edit.clear();
			table->impl.set_edit_listener(nullptr);
			return;
		}
		table->on_edit.set(fn, user, nullptr);
		auto fn_ = table->on_edit.fn;
		auto user_ = table->on_edit.user;
		auto owner_ = table->on_edit.owner;
		table->impl.set_edit_listener([fn_, user_, owner_](const table_cell& cell,
			const std::string& value) {
			if (!fn_)
				return;
			vgui_editor_table_cell view;
			fill_cell(cell, &view);
			fn_(user_, &view, value.c_str());
		});
	}

	void vgui_editor_table_set_command_stack(vgui_editor_table* table,
		vgui_editor_command_stack* stack)
	{
		if (!table)
			return;
		table->impl.set_command_stack(vgeditor::stack_impl(stack));
	}

	size_t vgui_editor_table_capture_state_json(vgui_editor_table* table, char* out,
		size_t cap)
	{
		if (!table)
			return 0;
		const std::string json = table_state_to_json(table->impl.capture_state());
		return vgeditor::copy_string(out, cap, json.c_str());
	}

	int vgui_editor_table_apply_state_json(vgui_editor_table* table, const char* json)
	{
		if (!table || !json)
			return 0;
		table_state state;
		std::string error;
		if (!table_state_from_json(vgeditor::to_text(json), state, &error))
			return 0;
		std::string apply_error;
		return table->impl.apply_state(state, &apply_error) ? 1 : 0;
	}

	size_t vgui_editor_table_last_render_cell_queries(vgui_editor_table* table)
	{
		return table ? table->impl.last_render_cell_queries() : 0;
	}

} /* extern "C" */

namespace vgeditor {

	vgui_editor_outliner* borrow_outliner(vek::outliner_view& view)
	{
		return borrow_object<vgui_editor_outliner>(view);
	}

	vgui_editor_table* borrow_table(vek::table_view& table)
	{
		return borrow_object<vgui_editor_table>(table);
	}

	void release_borrowed_outliner(vgui_editor_outliner* handle)
	{
		/* Wrapper only: a borrowed handle's C++ object belongs to its container. */
		if (handle && !handle->owns_object())
			delete handle;
	}


	void release_borrowed_table(vgui_editor_table* handle)
	{
		/* Wrapper only: a borrowed handle's C++ object belongs to its container. */
		if (handle && !handle->owns_object())
			delete handle;
	}


} /* namespace vgeditor */
