/* gui/editor/editor_c_api_properties.cpp - C facade for the property inspector
 * declared in <vgui/vgui_editor.h>.
 *
 * Rows cross the boundary as vgui_editor_property_desc records: every string is
 * copied at add time, host value bindings travel as a small callback struct
 * whose user pointer is released once the grid drops the row. */
#include "editor_c_api.h"
#include "editor_c_api_borrow.h"

#include <new>
#include <utility>
#include <vector>

#include <vgui/editor/property_grid.h>
#include <vgui/editor/vector_editor.h>

using namespace vgui_editor_kit;

/* -------------------------------------------------------------------------- */
/* Handle storage                                                            */
/* -------------------------------------------------------------------------- */

struct vgui_editor_property_grid : vgeditor::c_object<property_grid> {
	using c_object::c_object;

	/* Backs the borrowed string getters (row values, errors, favorite ids). */
	std::string scratch;

	vgui_editor_property_change_fn change_fn = nullptr;
	void* change_user = nullptr;
	vgui_editor_property_source_fn source_fn = nullptr;
	void* source_user = nullptr;
	vgui_editor_asset_drop_fn drop_fn = nullptr;
	void* drop_user = nullptr;
	vgui_editor_asset_query_fn query_fn = nullptr;
	void* query_user = nullptr;
	vgui_editor_asset_browse_fn browse_fn = nullptr;
	void* browse_user = nullptr;
	vgui_editor_object_activate_fn activate_fn = nullptr;
	void* activate_user = nullptr;
	vgui_editor_asset_filter_fn filter_fn = nullptr;
	void* filter_user = nullptr;
	vgui_editor_missing_asset_fn missing_fn = nullptr;
	void* missing_user = nullptr;
	vgui_editor_multi_get_fn multi_get_fn = nullptr;
	void* multi_get_user = nullptr;
	vgui_editor_multi_set_fn multi_set_fn = nullptr;
	void* multi_set_user = nullptr;
	vgui_editor_property_bind_fn bind_fn = nullptr;
	void* bind_user = nullptr;
};

namespace {

	/* The C enum mirrors the kit's ordering; these asserts turn a future reorder
	 * in either header into a build error. */
	static_assert((int)VGUI_EDITOR_PROPERTY_TEXT == (int)property_type::text,
		"property type drift");
	static_assert((int)VGUI_EDITOR_PROPERTY_COLOR == (int)property_type::color,
		"property type drift");
	static_assert((int)VGUI_EDITOR_PROPERTY_GRADIENT ==
		(int)property_type::gradient, "property type drift");

	property_type to_type(vgui_editor_property_type type)
	{
		if (type < VGUI_EDITOR_PROPERTY_TEXT || type > VGUI_EDITOR_PROPERTY_GRADIENT)
			return property_type::text;
		return static_cast<property_type>(type);
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

	/* Copies one descriptor into the kit's value type.  `binding` is only valid
	 * during the call, so its function pointers and a shared owner for its user
	 * pointer are captured by value instead. */
	property_desc to_desc(const vgui_editor_property_desc* src)
	{
		property_desc out;
		if (!src)
			return out;
		const vgui_editor_property_desc d = vgeditor::take_desc(src);

		out.id = vgeditor::to_text(d.id);
		out.label = vgeditor::to_text(d.label);
		out.type = to_type(d.type);
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
		switch (d.semantic) {
		case VGUI_EDITOR_VECTOR_POSITION:
			out.semantic = vector_semantic::position;
			break;
		case VGUI_EDITOR_VECTOR_COLOR:
			out.semantic = vector_semantic::color;
			break;
		default:
			out.semantic = vector_semantic::generic;
			break;
		}
		out.component_labels = vgeditor::to_text_list(d.component_labels,
			d.component_label_count);
		if (d.component_colors)
			out.component_colors.assign(d.component_colors,
				d.component_colors + d.component_color_count);
		out.asset_filter = vgeditor::to_text(d.asset_filter);

		if (!d.binding)
			return out;
		const vgui_editor_property_binding b = vgeditor::take_desc(d.binding);
		vgeditor::user_owner_ptr owner = vgeditor::make_owner(b.user,
			b.release_user);
		if (b.get_value) {
			/* The kit validates desc.value at add time, so a bound row with no
			 * explicit value takes its starting value from the host. */
			if (out.value.empty())
				out.value = vgeditor::read_text([b, owner](char* text, size_t cap) {
					return b.get_value(owner->user, text, cap);
				}).value;
			out.get_value = [b, owner]() {
				return vgeditor::read_text([b, owner](char* text, size_t cap) {
					return b.get_value(owner->user, text, cap);
				}).value;
			};
		}
		if (b.set_value) {
			out.set_value = [b, owner](const std::string& value) {
				b.set_value(owner->user, value.c_str());
			};
		}
		return out;
	}

	std::vector<property_desc> to_desc_list(const vgui_editor_property_desc* descs,
		size_t count)
	{
		std::vector<property_desc> out;
		if (!descs)
			return out;
		out.reserve(count);
		for (size_t i = 0; i < count; ++i)
			out.push_back(to_desc(descs + i));
		return out;
	}

	std::vector<object_id> to_targets(const vgui_editor_object_id* targets,
		size_t count)
	{
		std::vector<object_id> out;
		if (!targets)
			return out;
		out.reserve(count);
		for (size_t i = 0; i < count; ++i)
			out.push_back((object_id)targets[i]);
		return out;
	}

	/* Wraps the C out struct so the grid only ever sees the kit's own type. */
	bool query_asset(vgui_editor_asset_query_fn fn, void* user,
		const std::string& id, const std::string& filter,
		property_grid::asset_info* out)
	{
		if (!fn)
			return false;
		vgui_editor_asset_info info;
		std::memset(&info, 0, sizeof(info));
		info.struct_size = (uint32_t)sizeof(info);
		if (!fn(user, id.c_str(), filter.c_str(), &info))
			return false;
		if (out) {
			out->name = vgeditor::to_text(info.name);
			out->kind = vgeditor::to_text(info.kind);
			out->thumbnail = info.thumbnail;
			out->missing = info.missing != 0;
		}
		return true;
	}

} /* namespace */

extern "C" {

	vgui_editor_property_grid* vgui_editor_property_grid_create(vgui_widget* parent)
	{
		if (!parent)
			return nullptr;
		std::unique_ptr<vgui_editor_property_grid> grid(
			vgeditor::own_object<vgui_editor_property_grid>(
			new (std::nothrow) property_grid()));
		if (!grid)
			return nullptr;
		grid->impl.build(parent);
		if (!grid->impl.widget())
			return nullptr;
		return grid.release();
	}

	void vgui_editor_property_grid_destroy(vgui_editor_property_grid* grid)
	{
		if (!grid || !grid->owns_object())
			return;
		grid->impl.unbind_selection();
		grid->impl.set_change_listener(nullptr);
		grid->impl.set_value_source(nullptr);
		grid->impl.set_multi_getter(nullptr);
		grid->impl.set_multi_setter(nullptr);
		grid->impl.set_asset_query(nullptr);
		grid->impl.set_asset_browse_handler(nullptr);
		grid->impl.set_object_activated_handler(nullptr);
		grid->impl.set_asset_filter_check(nullptr);
		grid->impl.set_missing_asset_handler(nullptr);
		grid->impl.set_asset_drop_handler(nullptr);
		/* Rows own the binding owners, so dropping them releases host users. */
		grid->impl.clear();
		/* ~property_grid() tears down its panel, so the widget must not be
		 * destroyed a second time through the handle. */
		delete grid;
	}

	vgui_widget* vgui_editor_property_grid_widget(vgui_editor_property_grid* grid)
	{
		return grid ? grid->impl.widget() : nullptr;
	}

	vgui_widget* vgui_editor_property_grid_editor(vgui_editor_property_grid* grid,
		const char* id)
	{
		if (!grid)
			return nullptr;
		return grid->impl.property_editor(vgeditor::to_text(id));
	}

	void vgui_editor_property_grid_clear(vgui_editor_property_grid* grid)
	{
		if (!grid)
			return;
		grid->impl.clear();
	}

	int vgui_editor_property_grid_add_property(vgui_editor_property_grid* grid,
		const vgui_editor_property_desc* desc)
	{
		if (!grid || !desc)
			return 0;
		return grid->impl.add_property(to_desc(desc)) ? 1 : 0;
	}

	size_t vgui_editor_property_grid_set_properties(
		vgui_editor_property_grid* grid, const vgui_editor_property_desc* descs,
		size_t count)
	{
		if (!grid)
			return 0;
		return grid->impl.set_properties(to_desc_list(descs, count));
	}

	int vgui_editor_property_grid_remove_property(vgui_editor_property_grid* grid,
		const char* id)
	{
		if (!grid)
			return 0;
		return grid->impl.remove_property(vgeditor::to_text(id)) ? 1 : 0;
	}

	int vgui_editor_property_grid_has_property(vgui_editor_property_grid* grid,
		const char* id)
	{
		if (!grid)
			return 0;
		return grid->impl.has_property(vgeditor::to_text(id)) ? 1 : 0;
	}

	void vgui_editor_property_grid_add_row(vgui_editor_property_grid* grid,
		const char* label, const char* value)
	{
		if (!grid)
			return;
		grid->impl.add_row(vgeditor::to_text(label), vgeditor::to_text(value));
	}

	int vgui_editor_property_grid_set_row_value(vgui_editor_property_grid* grid,
		size_t index, const char* value)
	{
		if (!grid)
			return 0;
		return grid->impl.set_row_value(index, vgeditor::to_text(value)) ? 1 : 0;
	}

	size_t vgui_editor_property_grid_row_count(vgui_editor_property_grid* grid)
	{
		return grid ? grid->impl.row_count() : 0;
	}

	const char* vgui_editor_property_grid_row_value(vgui_editor_property_grid* grid,
		size_t index)
	{
		if (!grid)
			return nullptr;
		if (index >= grid->impl.row_count())
			return nullptr;
		grid->scratch = grid->impl.row_value(index);
		return grid->scratch.c_str();
	}

	const char* vgui_editor_property_grid_property_value(
		vgui_editor_property_grid* grid, const char* id)
	{
		if (!grid)
			return nullptr;
		grid->scratch = grid->impl.property_value(vgeditor::to_text(id));
		return grid->scratch.c_str();
	}

	int vgui_editor_property_grid_set_property_value(
		vgui_editor_property_grid* grid, const char* id, const char* value)
	{
		if (!grid)
			return 0;
		return grid->impl.set_property_value(vgeditor::to_text(id),
			vgeditor::to_text(value)) ? 1 : 0;
	}

	int vgui_editor_property_grid_commit_property_value(
		vgui_editor_property_grid* grid, const char* id, const char* value)
	{
		if (!grid)
			return 0;
		return grid->impl.commit_property_value(vgeditor::to_text(id),
			vgeditor::to_text(value)) ? 1 : 0;
	}

	int vgui_editor_property_grid_reset_property(vgui_editor_property_grid* grid,
		const char* id)
	{
		if (!grid)
			return 0;
		return grid->impl.reset_property(vgeditor::to_text(id)) ? 1 : 0;
	}

	void vgui_editor_property_grid_reset_all(vgui_editor_property_grid* grid)
	{
		if (!grid)
			return;
		grid->impl.reset_all();
	}

	const char* vgui_editor_property_grid_property_error(
		vgui_editor_property_grid* grid, const char* id)
	{
		if (!grid)
			return nullptr;
		grid->scratch = grid->impl.property_error(vgeditor::to_text(id));
		return grid->scratch.c_str();
	}

	int vgui_editor_property_grid_set_property_error(
		vgui_editor_property_grid* grid, const char* id, const char* error)
	{
		if (!grid)
			return 0;
		return grid->impl.set_property_error(vgeditor::to_text(id),
			vgeditor::to_text(error)) ? 1 : 0;
	}

	int vgui_editor_property_grid_property_read_only(
		vgui_editor_property_grid* grid, const char* id)
	{
		if (!grid)
			return 0;
		return grid->impl.property_read_only(vgeditor::to_text(id)) ? 1 : 0;
	}

	int vgui_editor_property_grid_set_property_read_only(
		vgui_editor_property_grid* grid, const char* id, int read_only)
	{
		if (!grid)
			return 0;
		return grid->impl.set_property_read_only(vgeditor::to_text(id),
			read_only != 0) ? 1 : 0;
	}

	void vgui_editor_property_grid_set_read_only(vgui_editor_property_grid* grid,
		int read_only)
	{
		if (!grid)
			return;
		grid->impl.set_read_only(read_only != 0);
	}

	int vgui_editor_property_grid_read_only(vgui_editor_property_grid* grid)
	{
		return grid && grid->impl.read_only() ? 1 : 0;
	}

	void vgui_editor_property_grid_set_filter(vgui_editor_property_grid* grid,
		const char* text)
	{
		if (!grid)
			return;
		grid->impl.set_filter(vgeditor::to_text(text));
	}

	const char* vgui_editor_property_grid_filter(vgui_editor_property_grid* grid)
	{
		return grid ? grid->impl.filter().c_str() : "";
	}

	size_t vgui_editor_property_grid_visible_row_count(
		vgui_editor_property_grid* grid)
	{
		return grid ? grid->impl.visible_row_count() : 0;
	}

	int vgui_editor_property_grid_property_visible(vgui_editor_property_grid* grid,
		const char* id)
	{
		if (!grid)
			return 0;
		return grid->impl.property_visible(vgeditor::to_text(id)) ? 1 : 0;
	}

	size_t vgui_editor_property_grid_materialized_row_count(
		vgui_editor_property_grid* grid)
	{
		return grid ? grid->impl.materialized_row_count() : 0;
	}

	int vgui_editor_property_grid_set_category_collapsed(
		vgui_editor_property_grid* grid, const char* category, int collapsed)
	{
		if (!grid)
			return 0;
		return grid->impl.set_category_collapsed(vgeditor::to_text(category),
			collapsed != 0) ? 1 : 0;
	}

	int vgui_editor_property_grid_category_collapsed(
		vgui_editor_property_grid* grid, const char* category)
	{
		if (!grid)
			return 0;
		return grid->impl.category_collapsed(vgeditor::to_text(category)) ? 1 : 0;
	}

	void vgui_editor_property_grid_set_command_stack(
		vgui_editor_property_grid* grid, vgui_editor_command_stack* stack)
	{
		if (!grid)
			return;
		grid->impl.set_command_stack(vgeditor::stack_impl(stack));
	}

	void vgui_editor_property_grid_set_asset_drop_handler(
		vgui_editor_property_grid* grid, vgui_editor_asset_drop_fn fn, void* user)
	{
		if (!grid)
			return;
		grid->drop_fn = fn;
		grid->drop_user = user;
		grid->impl.set_asset_drop_handler([grid](const std::string& asset) {
			if (!grid->drop_fn)
				return false;
			return grid->drop_fn(grid->drop_user, asset.c_str()) != 0;
		});
	}

	void vgui_editor_property_grid_set_asset_query(vgui_editor_property_grid* grid,
		vgui_editor_asset_query_fn fn, void* user)
	{
		if (!grid)
			return;
		grid->query_fn = fn;
		grid->query_user = user;
		grid->impl.set_asset_query([grid](const std::string& id,
			const std::string& filter, property_grid::asset_info* out) {
			return query_asset(grid->query_fn, grid->query_user, id, filter, out);
		});
	}

	void vgui_editor_property_grid_set_asset_browse_handler(
		vgui_editor_property_grid* grid, vgui_editor_asset_browse_fn fn, void* user)
	{
		if (!grid)
			return;
		grid->browse_fn = fn;
		grid->browse_user = user;
		grid->impl.set_asset_browse_handler([grid](const std::string& id,
			const std::string& filter) {
			if (grid->browse_fn)
				grid->browse_fn(grid->browse_user, id.c_str(), filter.c_str());
		});
	}

	void vgui_editor_property_grid_set_object_activated_handler(
		vgui_editor_property_grid* grid, vgui_editor_object_activate_fn fn,
		void* user)
	{
		if (!grid)
			return;
		grid->activate_fn = fn;
		grid->activate_user = user;
		grid->impl.set_object_activated_handler([grid](object_id id) {
			if (grid->activate_fn)
				grid->activate_fn(grid->activate_user, (vgui_editor_object_id)id);
		});
	}

	void vgui_editor_property_grid_set_asset_filter_check(
		vgui_editor_property_grid* grid, vgui_editor_asset_filter_fn fn, void* user)
	{
		if (!grid)
			return;
		grid->filter_fn = fn;
		grid->filter_user = user;
		grid->impl.set_asset_filter_check([grid](const std::string& id,
			const std::string& filter) {
			if (!grid->filter_fn)
				return true;
			return grid->filter_fn(grid->filter_user, id.c_str(),
				filter.c_str()) != 0;
		});
	}

	void vgui_editor_property_grid_set_missing_asset_handler(
		vgui_editor_property_grid* grid, vgui_editor_missing_asset_fn fn,
		void* user)
	{
		if (!grid)
			return;
		grid->missing_fn = fn;
		grid->missing_user = user;
		grid->impl.set_missing_asset_handler([grid](const std::string& id,
			const std::string& filter) {
			if (grid->missing_fn)
				grid->missing_fn(grid->missing_user, id.c_str(), filter.c_str());
		});
	}

	int vgui_editor_property_grid_begin_batch(vgui_editor_property_grid* grid,
		const char* name)
	{
		if (!grid)
			return 0;
		return grid->impl.begin_batch(vgeditor::to_text(name)) ? 1 : 0;
	}

	int vgui_editor_property_grid_end_batch(vgui_editor_property_grid* grid)
	{
		if (!grid)
			return 0;
		return grid->impl.end_batch() ? 1 : 0;
	}

	size_t vgui_editor_property_grid_batch_depth(vgui_editor_property_grid* grid)
	{
		return grid ? grid->impl.batch_depth() : 0;
	}

	void vgui_editor_property_grid_set_multi_targets(
		vgui_editor_property_grid* grid, const vgui_editor_object_id* targets,
		size_t count)
	{
		if (!grid)
			return;
		grid->impl.set_multi_targets(to_targets(targets, count));
	}

	size_t vgui_editor_property_grid_multi_targets(
		vgui_editor_property_grid* grid, vgui_editor_object_id* out, size_t cap)
	{
		if (!grid)
			return 0;
		const std::vector<object_id>& targets = grid->impl.multi_targets();
		const size_t written = targets.size() < cap ? targets.size() : cap;
		for (size_t i = 0; i < written; ++i)
			out[i] = (vgui_editor_object_id)targets[i];
		return targets.size();
	}

	void vgui_editor_property_grid_set_multi_getter(
		vgui_editor_property_grid* grid, vgui_editor_multi_get_fn fn, void* user)
	{
		if (!grid)
			return;
		grid->multi_get_fn = fn;
		grid->multi_get_user = user;
		grid->impl.set_multi_getter([grid](const std::string& id,
			object_id target) {
			if (!grid->multi_get_fn)
				return std::string();
			const char* key = id.c_str();
			const vgui_editor_object_id slot = (vgui_editor_object_id)target;
			return vgeditor::read_text([grid, key, slot](char* text, size_t cap) {
				return grid->multi_get_fn(grid->multi_get_user, key, slot,
					text, cap);
			}).value;
		});
	}

	void vgui_editor_property_grid_set_multi_setter(
		vgui_editor_property_grid* grid, vgui_editor_multi_set_fn fn, void* user)
	{
		if (!grid)
			return;
		grid->multi_set_fn = fn;
		grid->multi_set_user = user;
		grid->impl.set_multi_setter([grid](const std::string& id, object_id target,
			const std::string& value) {
			if (grid->multi_set_fn)
				grid->multi_set_fn(grid->multi_set_user, id.c_str(),
					(vgui_editor_object_id)target, value.c_str());
		});
	}

	int vgui_editor_property_grid_property_mixed(vgui_editor_property_grid* grid,
		const char* id)
	{
		if (!grid)
			return 0;
		return grid->impl.property_mixed(vgeditor::to_text(id)) ? 1 : 0;
	}

	size_t vgui_editor_property_grid_copy_properties(vgui_editor_property_grid* grid)
	{
		return grid ? grid->impl.copy_properties() : 0;
	}

	size_t vgui_editor_property_grid_paste_properties(
		vgui_editor_property_grid* grid)
	{
		return grid ? grid->impl.paste_properties() : 0;
	}

	size_t vgui_editor_property_grid_clipboard_count(void)
	{
		return property_grid::clipboard_property_count();
	}

	void vgui_editor_property_grid_set_property_favorite(
		vgui_editor_property_grid* grid, const char* id, int favorite)
	{
		if (!grid)
			return;
		grid->impl.set_property_favorite(vgeditor::to_text(id), favorite != 0);
	}

	int vgui_editor_property_grid_property_favorite(vgui_editor_property_grid* grid,
		const char* id)
	{
		if (!grid)
			return 0;
		return grid->impl.property_favorite(vgeditor::to_text(id)) ? 1 : 0;
	}

	size_t vgui_editor_property_grid_favorite_count(vgui_editor_property_grid* grid)
	{
		if (!grid)
			return 0;
		return grid->impl.favorite_ids().size();
	}

	const char* vgui_editor_property_grid_favorite_id_at(
		vgui_editor_property_grid* grid, size_t index)
	{
		if (!grid)
			return nullptr;
		const std::vector<std::string> ids = grid->impl.favorite_ids();
		if (index >= ids.size())
			return nullptr;
		grid->scratch = ids[index];
		return grid->scratch.c_str();
	}

	void vgui_editor_property_grid_set_favorites_only(
		vgui_editor_property_grid* grid, int only)
	{
		if (!grid)
			return;
		grid->impl.set_favorites_only(only != 0);
	}

	void vgui_editor_property_grid_set_change_listener(
		vgui_editor_property_grid* grid, vgui_editor_property_change_fn fn,
		void* user)
	{
		if (!grid)
			return;
		grid->change_fn = fn;
		grid->change_user = user;
		grid->impl.set_change_listener([grid](const std::string& id,
			const std::string& value) {
			if (grid->change_fn)
				grid->change_fn(grid->change_user, id.c_str(), value.c_str());
		});
	}

	void vgui_editor_property_grid_set_value_source(
		vgui_editor_property_grid* grid, vgui_editor_property_source_fn fn,
		void* user)
	{
		if (!grid)
			return;
		grid->source_fn = fn;
		grid->source_user = user;
		grid->impl.set_value_source([grid](const std::string& id,
			std::string& out_value) {
			if (!grid->source_fn)
				return false;
			const char* key = id.c_str();
			const vgeditor::text_result result = vgeditor::read_text(
				[grid, key](char* text, size_t cap) {
					return grid->source_fn(grid->source_user, key, text, cap);
				});
			if (!result.ok)
				return false;
			out_value = std::move(result.value);
			return true;
		});
	}

	void vgui_editor_property_grid_refresh(vgui_editor_property_grid* grid)
	{
		if (!grid)
			return;
		grid->impl.refresh();
	}

	void vgui_editor_property_grid_bind_selection(vgui_editor_property_grid* grid,
		vgui_editor_context* context, vgui_editor_property_bind_fn fn, void* user)
	{
		if (!grid)
			return;
		grid->bind_fn = fn;
		grid->bind_user = user;
		if (!context) {
			grid->impl.unbind_selection();
			return;
		}
		editor_context* target = vgeditor::context_impl(context);
		if (!target)
			return;
		grid->impl.bind_selection(*target, [grid](property_grid&, object_id id) {
			if (grid->bind_fn)
				grid->bind_fn(grid->bind_user, grid, (vgui_editor_object_id)id);
		});
	}

	void vgui_editor_property_grid_unbind_selection(vgui_editor_property_grid* grid)
	{
		if (!grid)
			return;
		grid->bind_fn = nullptr;
		grid->bind_user = nullptr;
		grid->impl.unbind_selection();
	}

	size_t vgui_editor_property_normalize_value(const vgui_editor_property_desc* desc,
		const char* text, char* out, size_t cap)
	{
		const property_value_result result = property_normalize_value(to_desc(desc),
			vgeditor::to_text(text));
		if (!result.ok) {
			vgeditor::set_error(result.error.empty() ?
				std::string("property: invalid value") : result.error);
			return VGUI_EDITOR_NO_VALUE;
		}
		vgeditor::set_error(std::string());
		return vgeditor::copy_string(out, cap, result.value.c_str());
	}

} /* extern "C" */

namespace vgeditor {

	vgui_editor_property_grid* borrow_property_grid(property_grid& grid)
	{
		return borrow_object<vgui_editor_property_grid>(grid);
	}

	void release_borrowed_property_grid(vgui_editor_property_grid* handle)
	{
		/* Wrapper only: a borrowed handle's C++ object belongs to its container. */
		if (handle && !handle->owns_object())
			delete handle;
	}


} /* namespace vgeditor */
