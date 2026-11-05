/* gui/editor/editor_c_api_assets.cpp - C facade for the asset browser declared in <vgui/vgui_editor.h>.
 *
 * The kit's asset_source is a plain function-pointer struct, so a virtual model
 * cannot travel as a lambda like every other host callback: each installation
 * gets an id that the thunks resolve through a weak registry owned by the handle.
 * A borrowed handle that dies therefore leaves the thunks reading "no source"
 * instead of dereferencing freed memory, and the host release_user still runs
 * exactly once.  Directories, requests and results cross as copies; a host
 * undo/redo pair carries its own owner, so the C release outlives the command. */
#include "editor_c_api.h"
#include "editor_c_api_borrow.h"

#include <cstdint>
#include <cstring>
#include <new>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <vgui/editor/asset_browser.h>

using namespace vgui_editor_kit;

/* -------------------------------------------------------------------------- */
/* Handle storage                                                            */
/* -------------------------------------------------------------------------- */

/* One installed virtual source.  The registry hands out an id for it, so a thunk
 * can always tell whether the handle that installed it is still alive. */
struct source_bridge
{
	vgui_editor_asset_source desc;
	vgeditor::user_owner_ptr owner;
};

/* Shared between the handle and the navigate lambda it installs: whichever one
 * outlives the other keeps the last directory source alive (or, when both are
 * gone, releases its host user). */
struct source_slot
{
	std::shared_ptr<source_bridge> current;
};

struct vgui_editor_asset_browser : vgeditor::c_object<asset_browser> {
	using c_object::c_object;

	~vgui_editor_asset_browser();

	/* Borrowed-string scratch for the last get_item()/item_info() call. */
	std::string entry_name;
	std::string entry_type;

	/* Borrowed-string scratch for the last dependencies() call.  Each vector is
	 * reserved before it is filled, so an earlier c_str() stays valid. */
	std::vector<std::string> dep_names;
	std::vector<std::string> dep_types;
	std::vector<std::string> dep_paths;

	std::shared_ptr<source_slot> sources = std::make_shared<source_slot>();

	vgeditor::callback_slot<vgui_editor_asset_navigate_fn> navigate;
	vgeditor::callback_slot<vgui_editor_asset_path_fn> on_path;
	vgeditor::callback_slot<vgui_editor_asset_select_fn> on_selection;
	vgeditor::callback_slot<vgui_editor_asset_select_fn> on_activate;
	vgeditor::callback_slot<vgui_editor_asset_rename_fn> rename;
	vgeditor::callback_slot<vgui_editor_asset_import_fn> on_import;
	vgeditor::callback_slot<vgui_editor_asset_operation_fn> operation;
	vgeditor::callback_slot<vgui_editor_asset_notice_fn> on_notice;
	vgeditor::callback_slot<vgui_editor_asset_move_targets_fn> move_targets;
	vgeditor::callback_slot<vgui_editor_asset_dependency_fn> dependency;
	vgeditor::callback_slot<vgui_editor_asset_void_fn> import_prompt;

	/* The browser owns its thumbnail cache; the C wrapper we hand out is
	 * borrowed and cached so repeated calls stay stable. */
	vgui_editor_thumbnails* thumbnails = nullptr;
};

vgui_editor_asset_browser::~vgui_editor_asset_browser()
{
	/* The wrapper dies before the kit cache it points into. */
	vgeditor::release_borrowed_thumbnails(thumbnails);
	thumbnails = nullptr;
}

namespace {

	static_assert((int)VGUI_EDITOR_ASSET_LIST == (int)asset_browser::view_mode::list,
		"view mode drift");
	static_assert((int)VGUI_EDITOR_ASSET_GRID == (int)asset_browser::view_mode::grid,
		"view mode drift");

	static_assert((int)VGUI_EDITOR_ASSET_OP_RENAME == (int)asset_browser::operation::rename,
		"operation drift");
	static_assert((int)VGUI_EDITOR_ASSET_OP_REMOVE == (int)asset_browser::operation::remove,
		"operation drift");
	static_assert((int)VGUI_EDITOR_ASSET_OP_DUPLICATE ==
		(int)asset_browser::operation::duplicate, "operation drift");
	static_assert((int)VGUI_EDITOR_ASSET_OP_MOVE == (int)asset_browser::operation::move,
		"operation drift");
	static_assert((int)VGUI_EDITOR_ASSET_OP_REIMPORT ==
		(int)asset_browser::operation::reimport, "operation drift");

	static_assert((int)VGUI_EDITOR_ASSET_ERR_NONE ==
		(int)asset_browser::operation_error::none, "operation error drift");
	static_assert((int)VGUI_EDITOR_ASSET_ERR_NO_SELECTION ==
		(int)asset_browser::operation_error::no_selection, "operation error drift");
	static_assert((int)VGUI_EDITOR_ASSET_ERR_INVALID_NAME ==
		(int)asset_browser::operation_error::invalid_name, "operation error drift");
	static_assert((int)VGUI_EDITOR_ASSET_ERR_INVALID_TARGET ==
		(int)asset_browser::operation_error::invalid_target, "operation error drift");
	static_assert((int)VGUI_EDITOR_ASSET_ERR_NOT_FOUND ==
		(int)asset_browser::operation_error::not_found, "operation error drift");
	static_assert((int)VGUI_EDITOR_ASSET_ERR_CONFLICT ==
		(int)asset_browser::operation_error::conflict, "operation error drift");
	static_assert((int)VGUI_EDITOR_ASSET_ERR_READ_ONLY ==
		(int)asset_browser::operation_error::read_only, "operation error drift");
	static_assert((int)VGUI_EDITOR_ASSET_ERR_IN_USE ==
		(int)asset_browser::operation_error::in_use, "operation error drift");
	static_assert((int)VGUI_EDITOR_ASSET_ERR_UNSUPPORTED ==
		(int)asset_browser::operation_error::unsupported, "operation error drift");
	static_assert((int)VGUI_EDITOR_ASSET_ERR_IO_ERROR ==
		(int)asset_browser::operation_error::io_error, "operation error drift");

	static_assert((int)VGUI_EDITOR_ASSET_ACTION_RENAME == (int)asset_browser::action::rename,
		"action drift");
	static_assert((int)VGUI_EDITOR_ASSET_ACTION_DUPLICATE ==
		(int)asset_browser::action::duplicate, "action drift");
	static_assert((int)VGUI_EDITOR_ASSET_ACTION_REMOVE ==
		(int)asset_browser::action::remove, "action drift");
	static_assert((int)VGUI_EDITOR_ASSET_ACTION_MOVE == (int)asset_browser::action::move,
		"action drift");
	static_assert((int)VGUI_EDITOR_ASSET_ACTION_REIMPORT ==
		(int)asset_browser::action::reimport, "action drift");
	static_assert((int)VGUI_EDITOR_ASSET_ACTION_IMPORT ==
		(int)asset_browser::action::import, "action drift");
	static_assert((int)VGUI_EDITOR_ASSET_ACTION_DEPENDENCIES ==
		(int)asset_browser::action::dependencies, "action drift");

	static_assert((int)VGUI_EDITOR_THUMB_MISSING == (int)thumbnail_cache::state::missing,
		"thumbnail state drift");
	static_assert((int)VGUI_EDITOR_THUMB_QUEUED == (int)thumbnail_cache::state::queued,
		"thumbnail state drift");
	static_assert((int)VGUI_EDITOR_THUMB_LOADING == (int)thumbnail_cache::state::loading,
		"thumbnail state drift");
	static_assert((int)VGUI_EDITOR_THUMB_READY == (int)thumbnail_cache::state::ready,
		"thumbnail state drift");
	static_assert((int)VGUI_EDITOR_THUMB_FAILED == (int)thumbnail_cache::state::failed,
		"thumbnail state drift");

	asset_browser::view_mode to_view(vgui_editor_asset_view_mode mode)
	{
		return mode == VGUI_EDITOR_ASSET_GRID ? asset_browser::view_mode::grid :
			asset_browser::view_mode::list;
	}

	vgui_editor_asset_view_mode from_view(asset_browser::view_mode mode)
	{
		return mode == asset_browser::view_mode::grid ? VGUI_EDITOR_ASSET_GRID :
			VGUI_EDITOR_ASSET_LIST;
	}

	vgui_editor_asset_operation from_operation(asset_browser::operation kind)
	{
		switch (kind) {
		case asset_browser::operation::remove:
			return VGUI_EDITOR_ASSET_OP_REMOVE;
		case asset_browser::operation::duplicate:
			return VGUI_EDITOR_ASSET_OP_DUPLICATE;
		case asset_browser::operation::move:
			return VGUI_EDITOR_ASSET_OP_MOVE;
		case asset_browser::operation::reimport:
			return VGUI_EDITOR_ASSET_OP_REIMPORT;
		case asset_browser::operation::rename:
		default:
			return VGUI_EDITOR_ASSET_OP_RENAME;
		}
	}

	asset_browser::operation_error to_error(vgui_editor_asset_operation_error error)
	{
		switch (error) {
		case VGUI_EDITOR_ASSET_ERR_NO_SELECTION:
			return asset_browser::operation_error::no_selection;
		case VGUI_EDITOR_ASSET_ERR_INVALID_NAME:
			return asset_browser::operation_error::invalid_name;
		case VGUI_EDITOR_ASSET_ERR_INVALID_TARGET:
			return asset_browser::operation_error::invalid_target;
		case VGUI_EDITOR_ASSET_ERR_NOT_FOUND:
			return asset_browser::operation_error::not_found;
		case VGUI_EDITOR_ASSET_ERR_CONFLICT:
			return asset_browser::operation_error::conflict;
		case VGUI_EDITOR_ASSET_ERR_READ_ONLY:
			return asset_browser::operation_error::read_only;
		case VGUI_EDITOR_ASSET_ERR_IN_USE:
			return asset_browser::operation_error::in_use;
		case VGUI_EDITOR_ASSET_ERR_UNSUPPORTED:
			return asset_browser::operation_error::unsupported;
		case VGUI_EDITOR_ASSET_ERR_IO_ERROR:
			return asset_browser::operation_error::io_error;
		case VGUI_EDITOR_ASSET_ERR_NONE:
		default:
			return asset_browser::operation_error::none;
		}
	}

	vgui_editor_asset_operation_error from_error(asset_browser::operation_error error)
	{
		switch (error) {
		case asset_browser::operation_error::no_selection:
			return VGUI_EDITOR_ASSET_ERR_NO_SELECTION;
		case asset_browser::operation_error::invalid_name:
			return VGUI_EDITOR_ASSET_ERR_INVALID_NAME;
		case asset_browser::operation_error::invalid_target:
			return VGUI_EDITOR_ASSET_ERR_INVALID_TARGET;
		case asset_browser::operation_error::not_found:
			return VGUI_EDITOR_ASSET_ERR_NOT_FOUND;
		case asset_browser::operation_error::conflict:
			return VGUI_EDITOR_ASSET_ERR_CONFLICT;
		case asset_browser::operation_error::read_only:
			return VGUI_EDITOR_ASSET_ERR_READ_ONLY;
		case asset_browser::operation_error::in_use:
			return VGUI_EDITOR_ASSET_ERR_IN_USE;
		case asset_browser::operation_error::unsupported:
			return VGUI_EDITOR_ASSET_ERR_UNSUPPORTED;
		case asset_browser::operation_error::io_error:
			return VGUI_EDITOR_ASSET_ERR_IO_ERROR;
		case asset_browser::operation_error::none:
		default:
			return VGUI_EDITOR_ASSET_ERR_NONE;
		}
	}

	/* The core enum has no "none" verb, so an out-of-range C value is rejected
	 * here instead of silently being read as a rename. */
	bool to_action(vgui_editor_asset_action kind, asset_browser::action* out)
	{
		if (!out)
			return false;
		switch (kind) {
		case VGUI_EDITOR_ASSET_ACTION_DUPLICATE:
			*out = asset_browser::action::duplicate;
			return true;
		case VGUI_EDITOR_ASSET_ACTION_REMOVE:
			*out = asset_browser::action::remove;
			return true;
		case VGUI_EDITOR_ASSET_ACTION_MOVE:
			*out = asset_browser::action::move;
			return true;
		case VGUI_EDITOR_ASSET_ACTION_REIMPORT:
			*out = asset_browser::action::reimport;
			return true;
		case VGUI_EDITOR_ASSET_ACTION_IMPORT:
			*out = asset_browser::action::import;
			return true;
		case VGUI_EDITOR_ASSET_ACTION_DEPENDENCIES:
			*out = asset_browser::action::dependencies;
			return true;
		case VGUI_EDITOR_ASSET_ACTION_RENAME:
			*out = asset_browser::action::rename;
			return true;
		}
		return false;
	}

	vgui_editor_thumbnail_state from_state(thumbnail_cache::state state)
	{
		switch (state) {
		case thumbnail_cache::state::queued:
			return VGUI_EDITOR_THUMB_QUEUED;
		case thumbnail_cache::state::loading:
			return VGUI_EDITOR_THUMB_LOADING;
		case thumbnail_cache::state::ready:
			return VGUI_EDITOR_THUMB_READY;
		case thumbnail_cache::state::failed:
			return VGUI_EDITOR_THUMB_FAILED;
		case thumbnail_cache::state::missing:
		default:
			return VGUI_EDITOR_THUMB_MISSING;
		}
	}

	std::vector<vgui_item_id> to_ids(const vgui_item_id* items, size_t count)
	{
		std::vector<vgui_item_id> out;
		if (items && count)
			out.assign(items, items + count);
		return out;
	}

	/* ------------------------------------------------------------------ */
	/* Virtual source registry                                            */
	/* ------------------------------------------------------------------ */

	std::unordered_map<uint64_t, std::weak_ptr<source_bridge>>& bridge_registry()
	{
		static std::unordered_map<uint64_t, std::weak_ptr<source_bridge>> registry;
		return registry;
	}

	uint64_t register_bridge(const std::shared_ptr<source_bridge>& bridge)
	{
		static uint64_t next_id = 1;
		const uint64_t id = next_id++;
		bridge_registry()[id] = bridge;
		return id;
	}

	/* Resolves the id the kit carries in asset_source::user.  An entry whose
	 * handle is gone is dropped here, so the registry cannot grow without bound. */
	std::shared_ptr<source_bridge> find_bridge(void* user)
	{
		std::unordered_map<uint64_t, std::weak_ptr<source_bridge>>& registry =
			bridge_registry();
		const auto entry = registry.find((uint64_t)(uintptr_t)user);
		if (entry == registry.end())
			return std::shared_ptr<source_bridge>();
		std::shared_ptr<source_bridge> bridge = entry->second.lock();
		if (!bridge)
			registry.erase(entry);
		return bridge;
	}

	int bridge_count(void* user)
	{
		const std::shared_ptr<source_bridge> bridge = find_bridge(user);
		if (!bridge || !bridge->desc.count)
			return 0;
		return bridge->desc.count(bridge->owner->user);
	}

	/* The host fills borrowed strings; the kit owns std::string fields, so both
	 * are copied before this returns. */
	int bridge_item_at(void* user, int index, asset_info* out)
	{
		if (!out)
			return 0;
		const std::shared_ptr<source_bridge> bridge = find_bridge(user);
		if (!bridge || !bridge->desc.item_at)
			return 0;
		vgui_editor_asset_entry view;
		std::memset(&view, 0, sizeof(view));
		view.struct_size = (uint32_t)sizeof(view);
		if (!bridge->desc.item_at(bridge->owner->user, index, &view))
			return 0;
		out->name = vgeditor::to_text(view.name);
		out->type = vgeditor::to_text(view.type);
		out->thumbnail = view.thumbnail;
		out->id = view.id;
		return 1;
	}

	vgui_item_id bridge_id_at(void* user, int index)
	{
		const std::shared_ptr<source_bridge> bridge = find_bridge(user);
		if (!bridge || !bridge->desc.id_at)
			return 0;
		return bridge->desc.id_at(bridge->owner->user, index);
	}

	int bridge_index_of(void* user, vgui_item_id item)
	{
		const std::shared_ptr<source_bridge> bridge = find_bridge(user);
		if (!bridge || !bridge->desc.index_of)
			return -1;
		return bridge->desc.index_of(bridge->owner->user, item);
	}

	/* count and item_at are required; without them nothing is installed and the
	 * host keeps its user pointer (no release runs).  Optional callbacks stay NULL
	 * so the kit applies its own defaults (id = index + 1, linear id search). */
	bool install_source(const vgui_editor_asset_source* src,
		asset_browser::asset_source& out, const std::shared_ptr<source_slot>& slot)
	{
		out = asset_browser::asset_source();
		if (slot)
			slot->current.reset();
		if (!src)
			return false;
		std::shared_ptr<source_bridge> bridge = std::make_shared<source_bridge>();
		bridge->desc = vgeditor::take_desc(src);
		if (!bridge->desc.count || !bridge->desc.item_at)
			return false;
		bridge->owner = vgeditor::make_owner(bridge->desc.user,
			bridge->desc.release_user);
		out.count = &bridge_count;
		out.item_at = &bridge_item_at;
		if (bridge->desc.id_at)
			out.id_at = &bridge_id_at;
		if (bridge->desc.index_of)
			out.index_of = &bridge_index_of;
		out.user = (void*)(uintptr_t)register_bridge(bridge);
		if (slot)
			slot->current = bridge;
		return true;
	}

	/* ------------------------------------------------------------------ */
	/* Value conversions                                                  */
	/* ------------------------------------------------------------------ */

	/* Points a C entry at the handle scratch, so the borrowed strings survive
	 * until the next mutation of the same handle. */
	void fill_entry(vgui_editor_asset_browser* browser, const asset_info& info,
		vgui_editor_asset_entry* out)
	{
		std::memset(out, 0, sizeof(*out));
		out->struct_size = (uint32_t)sizeof(*out);
		browser->entry_name = info.name;
		browser->entry_type = info.type;
		out->name = browser->entry_name.c_str();
		out->type = browser->entry_type.c_str();
		out->thumbnail = info.thumbnail;
		out->id = info.id;
	}

	/* Copies a directory the host just filled into the kit's value types. */
	void to_directory(const vgui_editor_asset_directory& desc,
		asset_browser::asset_directory& out, const std::shared_ptr<source_slot>& slot)
	{
		out = asset_browser::asset_directory();
		if (desc.path && desc.path_count) {
			out.path.reserve(desc.path_count);
			for (size_t i = 0; i < desc.path_count; ++i) {
				const vgui_editor_asset_path_item item = vgeditor::take_desc(
					desc.path + i);
				asset_path_item segment;
				segment.label = vgeditor::to_text(item.label);
				segment.id = vgeditor::to_text(item.id);
				out.path.push_back(std::move(segment));
			}
		}
		if (desc.items && desc.item_count) {
			out.items.reserve(desc.item_count);
			for (size_t i = 0; i < desc.item_count; ++i) {
				const vgui_editor_asset_entry entry = vgeditor::take_desc(
					desc.items + i);
				asset_info info;
				info.name = vgeditor::to_text(entry.name);
				info.type = vgeditor::to_text(entry.type);
				info.thumbnail = entry.thumbnail;
				info.id = entry.id;
				out.items.push_back(std::move(info));
			}
		}
		out.empty_text = vgeditor::to_text(desc.empty_text);
		(void)install_source(desc.source, out.source, slot);
	}

} /* namespace */

/* -------------------------------------------------------------------------- */
/* Facade                                                                     */
/* -------------------------------------------------------------------------- */

extern "C" {

	void vgui_editor_asset_entry_default(vgui_editor_asset_entry* info)
	{
		if (!info)
			return;
		std::memset(info, 0, sizeof(*info));
		info->struct_size = (uint32_t)sizeof(*info);
		info->name = nullptr;
		info->type = nullptr;
		info->thumbnail = nullptr;
		info->id = 0;
	}

	void vgui_editor_asset_source_default(vgui_editor_asset_source* source)
	{
		if (!source)
			return;
		std::memset(source, 0, sizeof(*source));
		source->struct_size = (uint32_t)sizeof(*source);
	}

	void vgui_editor_asset_directory_default(
		vgui_editor_asset_directory* directory)
	{
		if (!directory)
			return;
		std::memset(directory, 0, sizeof(*directory));
		directory->struct_size = (uint32_t)sizeof(*directory);
	}

	void vgui_editor_asset_path_item_default(vgui_editor_asset_path_item* item)
	{
		if (!item)
			return;
		std::memset(item, 0, sizeof(*item));
		item->struct_size = (uint32_t)sizeof(*item);
	}

	void vgui_editor_asset_move_target_default(
		vgui_editor_asset_move_target* target)
	{
		if (!target)
			return;
		std::memset(target, 0, sizeof(*target));
		target->struct_size = (uint32_t)sizeof(*target);
	}

	void vgui_editor_asset_dependency_default(
		vgui_editor_asset_dependency* dependency)
	{
		if (!dependency)
			return;
		std::memset(dependency, 0, sizeof(*dependency));
		dependency->struct_size = (uint32_t)sizeof(*dependency);
	}

	void vgui_editor_asset_operation_result_default(
		vgui_editor_asset_operation_result* result)
	{
		if (!result)
			return;
		std::memset(result, 0, sizeof(*result));
		result->struct_size = (uint32_t)sizeof(*result);
		result->error = VGUI_EDITOR_ASSET_ERR_NONE;
	}

	vgui_editor_asset_browser* vgui_editor_asset_browser_create(vgui_widget* parent)
	{
		if (!parent)
			return nullptr;
		std::unique_ptr<vgui_editor_asset_browser> browser(
			vgeditor::own_object<vgui_editor_asset_browser>(
				new (std::nothrow) asset_browser()));
		if (!browser)
			return nullptr;
		browser->impl.build(parent);
		if (!browser->impl.widget())
			return nullptr;
		return browser.release();
	}

	void vgui_editor_asset_browser_destroy(vgui_editor_asset_browser* browser)
	{
		/* A shell-borrowed handle must not tear down the shell's browser. */
		if (!browser || !browser->owns_object())
			return;
		browser->impl.set_navigate_handler(nullptr);
		browser->impl.set_path_listener(nullptr);
		browser->impl.set_selection_listener(nullptr);
		browser->impl.set_activate_listener(nullptr);
		browser->impl.set_rename_handler(nullptr);
		browser->impl.set_import_handler(nullptr);
		browser->impl.set_operation_handler(nullptr);
		browser->impl.set_notice_listener(nullptr);
		browser->impl.set_move_targets_handler(nullptr);
		browser->impl.set_dependency_handler(nullptr);
		browser->impl.set_import_prompt_handler(nullptr);
		browser->impl.set_command_stack(nullptr);
		/* Detach first so no thunk can run against the bridge being released. */
		browser->impl.set_virtual_source(nullptr);
		browser->sources->current.reset();
		vgui_widget* widget = browser->impl.widget();
		delete browser;
		if (widget)
			vgui_widget_destroy(widget);
	}

	vgui_widget* vgui_editor_asset_browser_widget(vgui_editor_asset_browser* browser)
	{
		return browser ? browser->impl.widget() : nullptr;
	}

	void vgui_editor_asset_browser_clear(vgui_editor_asset_browser* browser)
	{
		if (!browser)
			return;
		browser->impl.clear();
	}

	vgui_item_id vgui_editor_asset_browser_add_item(vgui_editor_asset_browser* browser,
		const char* text, const char* detail)
	{
		if (!browser)
			return 0;
		return browser->impl.add_item(vgeditor::to_text(text), vgeditor::to_text(detail));
	}

	int vgui_editor_asset_browser_set_item_thumbnail(
		vgui_editor_asset_browser* browser, vgui_item_id item, vgui_texture* texture)
	{
		if (!browser)
			return 0;
		return browser->impl.set_item_thumbnail(item, texture) ? 1 : 0;
	}

	vgui_texture* vgui_editor_asset_browser_item_thumbnail(
		vgui_editor_asset_browser* browser, vgui_item_id item)
	{
		return browser ? browser->impl.item_thumbnail(item) : nullptr;
	}

	/* The browser owns this cache, so the handle is always borrowed. */
	vgui_editor_thumbnails* vgui_editor_asset_browser_thumbnails(
		vgui_editor_asset_browser* browser)
	{
		if (!browser)
			return nullptr;
		if (!browser->thumbnails)
			browser->thumbnails = vgeditor::borrow_thumbnails(
				browser->impl.thumbnails());
		return browser->thumbnails;
	}

	size_t vgui_editor_asset_browser_thumbnail_key(
		vgui_editor_asset_browser* browser, vgui_item_id item, char* out, size_t cap)
	{
		if (!browser)
			return 0;
		const std::string key = browser->impl.thumbnail_key(item);
		return vgeditor::copy_string(out, cap, key.c_str());
	}

	vgui_editor_thumbnail_state vgui_editor_asset_browser_thumbnail_state(
		vgui_editor_asset_browser* browser, vgui_item_id item)
	{
		if (!browser)
			return VGUI_EDITOR_THUMB_MISSING;
		return from_state(browser->impl.thumbnail_state(item));
	}

	int vgui_editor_asset_browser_retry_thumbnail(vgui_editor_asset_browser* browser,
		vgui_item_id item)
	{
		if (!browser)
			return 0;
		return browser->impl.retry_thumbnail(item) ? 1 : 0;
	}

	void vgui_editor_asset_browser_set_source(vgui_editor_asset_browser* browser,
		const vgui_editor_asset_source* source)
	{
		if (!browser)
			return;
		asset_browser::asset_source installed;
		const bool usable = install_source(source, installed, browser->sources);
		browser->impl.set_virtual_source(usable ? &installed : nullptr);
	}

	int vgui_editor_asset_browser_is_virtual(vgui_editor_asset_browser* browser)
	{
		return browser && browser->impl.is_virtual() ? 1 : 0;
	}

	int vgui_editor_asset_browser_item_count(vgui_editor_asset_browser* browser)
	{
		return browser ? browser->impl.item_count() : 0;
	}

	int vgui_editor_asset_browser_get_item(vgui_editor_asset_browser* browser,
		int index, vgui_editor_asset_entry* out)
	{
		if (!browser || !out)
			return 0;
		asset_info info;
		if (!browser->impl.item_at(index, &info))
			return 0;
		fill_entry(browser, info, out);
		return 1;
	}

	vgui_item_id vgui_editor_asset_browser_id_at(vgui_editor_asset_browser* browser,
		int index)
	{
		if (!browser)
			return 0;
		return browser->impl.id_at(index);
	}

	int vgui_editor_asset_browser_index_of(vgui_editor_asset_browser* browser,
		vgui_item_id item)
	{
		if (!browser)
			return -1;
		return browser->impl.index_of(item);
	}

	int vgui_editor_asset_browser_item_info(vgui_editor_asset_browser* browser,
		vgui_item_id item, vgui_editor_asset_entry* out)
	{
		if (!browser || !out)
			return 0;
		asset_info info;
		if (!browser->impl.item_info(item, &info))
			return 0;
		fill_entry(browser, info, out);
		return 1;
	}

	vgui_editor_asset_view_mode vgui_editor_asset_browser_view(
		vgui_editor_asset_browser* browser)
	{
		if (!browser)
			return VGUI_EDITOR_ASSET_LIST;
		return from_view(browser->impl.view());
	}

	void vgui_editor_asset_browser_set_view(vgui_editor_asset_browser* browser,
		vgui_editor_asset_view_mode mode)
	{
		if (!browser)
			return;
		browser->impl.set_view(to_view(mode));
	}

	void vgui_editor_asset_browser_set_view_switch_visible(
		vgui_editor_asset_browser* browser, int visible)
	{
		if (!browser)
			return;
		browser->impl.set_view_switch_visible(visible != 0);
	}

	int vgui_editor_asset_browser_view_switch_visible(
		vgui_editor_asset_browser* browser)
	{
		return browser && browser->impl.view_switch_visible() ? 1 : 0;
	}

	void vgui_editor_asset_browser_set_navigate_handler(
		vgui_editor_asset_browser* browser, vgui_editor_asset_navigate_fn fn,
		void* user, vgui_editor_release_fn release_user)
	{
		if (!browser)
			return;
		if (!fn) {
			browser->navigate.clear();
			browser->impl.set_navigate_handler(nullptr);
			return;
		}
		browser->navigate.set(fn, user, release_user);
		const vgui_editor_asset_navigate_fn slot_fn = browser->navigate.fn;
		std::shared_ptr<source_slot> slot = browser->sources;
		vgeditor::user_owner_ptr owner = browser->navigate.owner;
		browser->impl.set_navigate_handler(
			[slot_fn, owner, slot](const std::string& path_id,
				asset_browser::asset_directory* out) -> bool {
				if (!out)
					return false;
				/* The host's directory borrows its own memory, so every field is
				 * copied before this callback returns. */
				vgui_editor_asset_directory view;
				std::memset(&view, 0, sizeof(view));
				view.struct_size = (uint32_t)sizeof(view);
				if (!slot_fn(owner->user, path_id.c_str(), &view))
					return false;
				to_directory(vgeditor::take_desc(&view), *out, slot);
				return true;
			});
	}

	void vgui_editor_asset_browser_set_on_path(vgui_editor_asset_browser* browser,
		vgui_editor_asset_path_fn fn, void* user)
	{
		if (!browser)
			return;
		if (!fn) {
			browser->on_path.clear();
			browser->impl.set_path_listener(nullptr);
			return;
		}
		browser->on_path.set(fn, user, nullptr);
		const vgui_editor_asset_path_fn slot_fn = browser->on_path.fn;
		void* slot_user = browser->on_path.user;
		vgeditor::user_owner_ptr owner = browser->on_path.owner;
		browser->impl.set_path_listener([slot_fn, slot_user, owner](
			const std::string& path_id) {
			slot_fn(slot_user, path_id.c_str());
		});
	}

	int vgui_editor_asset_browser_navigate_to(vgui_editor_asset_browser* browser,
		const char* path_id)
	{
		if (!browser)
			return 0;
		return browser->impl.navigate_to(vgeditor::to_text(path_id)) ? 1 : 0;
	}

	int vgui_editor_asset_browser_can_go_back(vgui_editor_asset_browser* browser)
	{
		return browser && browser->impl.can_go_back() ? 1 : 0;
	}

	int vgui_editor_asset_browser_can_go_forward(vgui_editor_asset_browser* browser)
	{
		return browser && browser->impl.can_go_forward() ? 1 : 0;
	}

	int vgui_editor_asset_browser_go_back(vgui_editor_asset_browser* browser)
	{
		if (!browser)
			return 0;
		return browser->impl.go_back() ? 1 : 0;
	}

	int vgui_editor_asset_browser_go_forward(vgui_editor_asset_browser* browser)
	{
		if (!browser)
			return 0;
		return browser->impl.go_forward() ? 1 : 0;
	}

	int vgui_editor_asset_browser_reload(vgui_editor_asset_browser* browser)
	{
		if (!browser)
			return 0;
		return browser->impl.reload() ? 1 : 0;
	}

	size_t vgui_editor_asset_browser_current_path(vgui_editor_asset_browser* browser,
		char* out, size_t cap)
	{
		if (!browser)
			return 0;
		return vgeditor::copy_string(out, cap, browser->impl.current_path().c_str());
	}

	void vgui_editor_asset_browser_set_navigation_visible(
		vgui_editor_asset_browser* browser, int visible)
	{
		if (!browser)
			return;
		browser->impl.set_navigation_visible(visible != 0);
	}

	int vgui_editor_asset_browser_navigation_visible(
		vgui_editor_asset_browser* browser)
	{
		return browser && browser->impl.navigation_visible() ? 1 : 0;
	}

	void vgui_editor_asset_browser_set_search_debounce(
		vgui_editor_asset_browser* browser, float seconds)
	{
		if (!browser)
			return;
		browser->impl.set_search_debounce(seconds);
	}

	float vgui_editor_asset_browser_search_debounce(
		vgui_editor_asset_browser* browser)
	{
		return browser ? browser->impl.search_debounce() : 0.0f;
	}

	void vgui_editor_asset_browser_set_search(vgui_editor_asset_browser* browser,
		const char* text, int immediate)
	{
		if (!browser)
			return;
		browser->impl.set_search_text(vgeditor::to_text(text), immediate != 0);
	}

	size_t vgui_editor_asset_browser_search(vgui_editor_asset_browser* browser,
		char* out, size_t cap)
	{
		if (!browser)
			return 0;
		return vgeditor::copy_string(out, cap, browser->impl.search_text().c_str());
	}

	int vgui_editor_asset_browser_search_pending(vgui_editor_asset_browser* browser)
	{
		return browser && browser->impl.search_pending() ? 1 : 0;
	}

	void vgui_editor_asset_browser_apply_search(vgui_editor_asset_browser* browser)
	{
		if (!browser)
			return;
		browser->impl.apply_search();
	}

	int vgui_editor_asset_browser_filter_active(vgui_editor_asset_browser* browser)
	{
		return browser && browser->impl.filter_active() ? 1 : 0;
	}

	void vgui_editor_asset_browser_set_empty_texts(
		vgui_editor_asset_browser* browser, const char* empty, const char* no_matches)
	{
		if (!browser)
			return;
		/* NULL becomes an empty string, which the kit reads as "restore the
		 * default wording". */
		browser->impl.set_empty_texts(vgeditor::to_text(empty),
			vgeditor::to_text(no_matches));
	}

	size_t vgui_editor_asset_browser_empty_state_text(
		vgui_editor_asset_browser* browser, char* out, size_t cap)
	{
		if (!browser)
			return 0;
		return vgeditor::copy_string(out, cap,
			browser->impl.empty_state_text().c_str());
	}

	void vgui_editor_asset_browser_tick(vgui_editor_asset_browser* browser,
		float delta_time)
	{
		if (!browser)
			return;
		browser->impl.tick(delta_time);
	}

	void vgui_editor_asset_browser_set_type_options(
		vgui_editor_asset_browser* browser, const char* const* types, size_t count)
	{
		if (!browser)
			return;
		browser->impl.set_type_options(vgeditor::to_text_list(types, count));
	}

	size_t vgui_editor_asset_browser_type_option_count(
		vgui_editor_asset_browser* browser)
	{
		if (!browser)
			return 0;
		return browser->impl.type_options().size();
	}

	size_t vgui_editor_asset_browser_type_option_at(
		vgui_editor_asset_browser* browser, size_t index, char* out, size_t cap)
	{
		if (!browser)
			return 0;
		const std::vector<std::string>& types = browser->impl.type_options();
		if (index >= types.size())
			return vgeditor::copy_string(out, cap, "");
		return vgeditor::copy_string(out, cap, types[index].c_str());
	}

	void vgui_editor_asset_browser_set_type_filter(vgui_editor_asset_browser* browser,
		const char* type)
	{
		if (!browser)
			return;
		browser->impl.set_type_filter(vgeditor::to_text(type));
	}

	size_t vgui_editor_asset_browser_type_filter(vgui_editor_asset_browser* browser,
		char* out, size_t cap)
	{
		if (!browser)
			return 0;
		return vgeditor::copy_string(out, cap, browser->impl.type_filter().c_str());
	}

	void vgui_editor_asset_browser_set_type_filter_visible(
		vgui_editor_asset_browser* browser, int visible)
	{
		if (!browser)
			return;
		browser->impl.set_type_filter_visible(visible != 0);
	}

	int vgui_editor_asset_browser_type_filter_visible(
		vgui_editor_asset_browser* browser)
	{
		return browser && browser->impl.type_filter_visible() ? 1 : 0;
	}

	int vgui_editor_asset_browser_total_item_count(vgui_editor_asset_browser* browser)
	{
		return browser ? browser->impl.total_item_count() : 0;
	}

	int vgui_editor_asset_browser_visible_first(vgui_editor_asset_browser* browser)
	{
		return browser ? browser->impl.visible_first() : 0;
	}

	int vgui_editor_asset_browser_visible_last(vgui_editor_asset_browser* browser)
	{
		return browser ? browser->impl.visible_last() : 0;
	}

	int vgui_editor_asset_browser_visible_count(vgui_editor_asset_browser* browser)
	{
		return browser ? browser->impl.visible_count() : 0;
	}

	size_t vgui_editor_asset_browser_visible_items(
		vgui_editor_asset_browser* browser, vgui_item_id* out, size_t cap)
	{
		if (!browser)
			return 0;
		const std::vector<vgui_item_id> ids = browser->impl.visible_items();
		const size_t written = out ? (ids.size() < cap ? ids.size() : cap) : 0;
		for (size_t i = 0; i < written; ++i)
			out[i] = ids[i];
		return ids.size();
	}

	int vgui_editor_asset_browser_grid_columns(vgui_editor_asset_browser* browser)
	{
		return browser ? browser->impl.grid_columns() : 0;
	}

	int vgui_editor_asset_browser_item_rect(vgui_editor_asset_browser* browser,
		int index, float* x, float* y, float* width, float* height)
	{
		if (!browser)
			return 0;
		return browser->impl.item_rect(index, x, y, width, height) ? 1 : 0;
	}

	void vgui_editor_asset_browser_refresh(vgui_editor_asset_browser* browser)
	{
		if (!browser)
			return;
		browser->impl.refresh();
	}

	vgui_item_id vgui_editor_asset_browser_selected(vgui_editor_asset_browser* browser)
	{
		return browser ? browser->impl.selected() : 0;
	}

	vgui_item_id vgui_editor_asset_browser_cursor(vgui_editor_asset_browser* browser)
	{
		return browser ? browser->impl.cursor() : 0;
	}

	size_t vgui_editor_asset_browser_selection(vgui_editor_asset_browser* browser,
		vgui_item_id* out, size_t cap)
	{
		if (!browser)
			return 0;
		const std::vector<vgui_item_id> ids = browser->impl.selection();
		const size_t written = out ? (ids.size() < cap ? ids.size() : cap) : 0;
		for (size_t i = 0; i < written; ++i)
			out[i] = ids[i];
		return ids.size();
	}

	void vgui_editor_asset_browser_set_selection(vgui_editor_asset_browser* browser,
		const vgui_item_id* items, size_t count)
	{
		if (!browser)
			return;
		browser->impl.set_selection(to_ids(items, count));
	}

	void vgui_editor_asset_browser_clear_selection(vgui_editor_asset_browser* browser)
	{
		if (!browser)
			return;
		browser->impl.clear_selection();
	}

	int vgui_editor_asset_browser_is_selected(vgui_editor_asset_browser* browser,
		vgui_item_id item)
	{
		if (!browser)
			return 0;
		return browser->impl.is_selected(item) ? 1 : 0;
	}

	void vgui_editor_asset_browser_set_on_selection(
		vgui_editor_asset_browser* browser, vgui_editor_asset_select_fn fn, void* user)
	{
		if (!browser)
			return;
		if (!fn) {
			browser->on_selection.clear();
			browser->impl.set_selection_listener(nullptr);
			return;
		}
		browser->on_selection.set(fn, user, nullptr);
		const vgui_editor_asset_select_fn slot_fn = browser->on_selection.fn;
		void* slot_user = browser->on_selection.user;
		vgeditor::user_owner_ptr owner = browser->on_selection.owner;
		browser->impl.set_selection_listener([slot_fn, slot_user, owner](
			vgui_item_id item) {
			slot_fn(slot_user, item);
		});
	}

	size_t vgui_editor_asset_browser_add_selection_listener(
		vgui_editor_asset_browser* browser, vgui_editor_asset_select_fn fn, void* user)
	{
		if (!browser || !fn)
			return 0;
		/* Multi-listeners live in the browser, so a shell-borrowed handle that
		 * disappears cannot remove them; the browser drops them at the end. */
		return browser->impl.add_selection_listener([fn, user](vgui_item_id item) {
			fn(user, item);
		});
	}

	int vgui_editor_asset_browser_remove_selection_listener(
		vgui_editor_asset_browser* browser, size_t token)
	{
		if (!browser)
			return 0;
		return browser->impl.remove_selection_listener(token) ? 1 : 0;
	}

	void vgui_editor_asset_browser_set_rename_handler(
		vgui_editor_asset_browser* browser, vgui_editor_asset_rename_fn fn, void* user)
	{
		if (!browser)
			return;
		if (!fn) {
			browser->rename.clear();
			browser->impl.set_rename_handler(nullptr);
			return;
		}
		browser->rename.set(fn, user, nullptr);
		const vgui_editor_asset_rename_fn slot_fn = browser->rename.fn;
		void* slot_user = browser->rename.user;
		vgeditor::user_owner_ptr owner = browser->rename.owner;
		browser->impl.set_rename_handler([slot_fn, slot_user, owner](vgui_item_id item,
			const std::string& name) -> bool {
			return slot_fn(slot_user, item, name.c_str()) != 0;
		});
	}

	void vgui_editor_asset_browser_set_command_stack(
		vgui_editor_asset_browser* browser, vgui_editor_command_stack* stack)
	{
		if (!browser)
			return;
		browser->impl.set_command_stack(vgeditor::stack_impl(stack));
	}

	int vgui_editor_asset_browser_rename_item(vgui_editor_asset_browser* browser,
		vgui_item_id item, const char* name)
	{
		if (!browser)
			return 0;
		return browser->impl.rename_item(item, vgeditor::to_text(name)) ? 1 : 0;
	}

	void vgui_editor_asset_browser_set_on_activate(
		vgui_editor_asset_browser* browser, vgui_editor_asset_select_fn fn, void* user)
	{
		if (!browser)
			return;
		if (!fn) {
			browser->on_activate.clear();
			browser->impl.set_activate_listener(nullptr);
			return;
		}
		browser->on_activate.set(fn, user, nullptr);
		const vgui_editor_asset_select_fn slot_fn = browser->on_activate.fn;
		void* slot_user = browser->on_activate.user;
		vgeditor::user_owner_ptr owner = browser->on_activate.owner;
		browser->impl.set_activate_listener([slot_fn, slot_user, owner](
			vgui_item_id item) {
			slot_fn(slot_user, item);
		});
	}

	void vgui_editor_asset_browser_activate_selected(vgui_editor_asset_browser* browser)
	{
		if (!browser)
			return;
		browser->impl.activate_selected();
	}

	void vgui_editor_asset_browser_enable_row_drag(vgui_editor_asset_browser* browser)
	{
		if (!browser)
			return;
		browser->impl.enable_row_drag();
	}

	void vgui_editor_asset_browser_set_import_handler(
		vgui_editor_asset_browser* browser, vgui_editor_asset_import_fn fn, void* user)
	{
		if (!browser)
			return;
		if (!fn) {
			browser->on_import.clear();
			browser->impl.set_import_handler(nullptr);
			return;
		}
		browser->on_import.set(fn, user, nullptr);
		const vgui_editor_asset_import_fn slot_fn = browser->on_import.fn;
		void* slot_user = browser->on_import.user;
		vgeditor::user_owner_ptr owner = browser->on_import.owner;
		browser->impl.set_import_handler([slot_fn, slot_user, owner](
			const std::vector<std::string>& files,
			const std::string& path_id) -> bool {
			std::vector<const char*> ptrs;
			ptrs.reserve(files.size());
			for (const std::string& file : files)
				ptrs.push_back(file.c_str());
			return slot_fn(slot_user, ptrs.empty() ? nullptr : ptrs.data(),
				ptrs.size(), path_id.c_str()) != 0;
		});
	}

	int vgui_editor_asset_browser_import_files(vgui_editor_asset_browser* browser,
		const char* const* files, size_t file_count)
	{
		if (!browser)
			return 0;
		return browser->impl.import_files(vgeditor::to_text_list(files, file_count)) ?
			1 : 0;
	}

	int vgui_editor_asset_browser_request_import(vgui_editor_asset_browser* browser)
	{
		if (!browser)
			return 0;
		return browser->impl.request_import() ? 1 : 0;
	}

	void vgui_editor_asset_browser_set_operation_handler(
		vgui_editor_asset_browser* browser, vgui_editor_asset_operation_fn fn,
		void* user, vgui_editor_release_fn release_user)
	{
		if (!browser)
			return;
		if (!fn) {
			browser->operation.clear();
			browser->impl.set_operation_handler(nullptr);
			return;
		}
		browser->operation.set(fn, user, release_user);
		const vgui_editor_asset_operation_fn slot_fn = browser->operation.fn;
		vgeditor::user_owner_ptr owner = browser->operation.owner;
		browser->impl.set_operation_handler(
			[slot_fn, owner](const asset_browser::operation_request& request,
				asset_browser::operation_result* result) -> bool {
				if (!result)
					return false;
				/* The request borrows the kit's strings and id vector only for the
				 * duration of the call; the result is copied out immediately. */
				vgui_editor_asset_operation_request view;
				std::memset(&view, 0, sizeof(view));
				view.struct_size = (uint32_t)sizeof(view);
				view.kind = from_operation(request.kind);
				view.items = request.items.empty() ? nullptr : request.items.data();
				view.item_count = request.items.size();
				view.name = request.name.c_str();
				view.path_id = request.path_id.c_str();
				view.target_path_id = request.target_path_id.c_str();

				vgui_editor_asset_operation_result filled;
				std::memset(&filled, 0, sizeof(filled));
				filled.struct_size = (uint32_t)sizeof(filled);
				const bool ok = slot_fn(owner->user, &view, &filled) != 0;
				const vgui_editor_asset_operation_result out = vgeditor::take_desc(
					&filled);

				result->error = to_error(out.error);
				result->message = vgeditor::to_text(out.message);
				result->undoable = out.undoable != 0;
				/* The undo pair owns the host user on its own, so release_user runs
				 * exactly once: when the command record leaves the stack, or right
				 * here when the result never became one. */
				vgeditor::user_owner_ptr cmd = vgeditor::make_owner(out.user,
					out.release_user);
				if (out.undo) {
					const vgui_editor_asset_undo_fn undo_fn = out.undo;
					result->undo = [undo_fn, cmd]() {
						return undo_fn(cmd->user) != 0;
					};
				}
				if (out.redo) {
					const vgui_editor_asset_undo_fn redo_fn = out.redo;
					result->redo = [redo_fn, cmd]() {
						return redo_fn(cmd->user) != 0;
					};
				}
				if (out.select) {
					result->select.assign(out.select, out.select + out.select_count);
				}
				if (out.created) {
					result->created.assign(out.created,
						out.created + out.created_count);
				}
				return ok;
			});
	}

	void vgui_editor_asset_browser_set_on_notice(vgui_editor_asset_browser* browser,
		vgui_editor_asset_notice_fn fn, void* user)
	{
		if (!browser)
			return;
		if (!fn) {
			browser->on_notice.clear();
			browser->impl.set_notice_listener(nullptr);
			return;
		}
		browser->on_notice.set(fn, user, nullptr);
		const vgui_editor_asset_notice_fn slot_fn = browser->on_notice.fn;
		void* slot_user = browser->on_notice.user;
		vgeditor::user_owner_ptr owner = browser->on_notice.owner;
		browser->impl.set_notice_listener([slot_fn, slot_user, owner](
			const std::string& message, asset_browser::operation_error error) {
			slot_fn(slot_user, message.c_str(), from_error(error));
		});
	}

	void vgui_editor_asset_browser_set_move_targets(vgui_editor_asset_browser* browser,
		vgui_editor_asset_move_targets_fn fn, void* user)
	{
		if (!browser)
			return;
		if (!fn) {
			browser->move_targets.clear();
			browser->impl.set_move_targets_handler(nullptr);
			return;
		}
		browser->move_targets.set(fn, user, nullptr);
		const vgui_editor_asset_move_targets_fn slot_fn = browser->move_targets.fn;
		void* slot_user = browser->move_targets.user;
		vgeditor::user_owner_ptr owner = browser->move_targets.owner;
		browser->impl.set_move_targets_handler(
			[slot_fn, slot_user, owner]() -> std::vector<asset_browser::asset_move_target> {
				std::vector<asset_browser::asset_move_target> targets;
				const size_t total = slot_fn(slot_user, nullptr, 0);
				if (total == 0)
					return targets;
				std::vector<vgui_editor_asset_move_target> view(total);
				for (size_t i = 0; i < total; ++i) {
					std::memset(&view[i], 0, sizeof(view[i]));
					view[i].struct_size = (uint32_t)sizeof(view[i]);
				}
				const size_t filled = slot_fn(slot_user, view.data(), total);
				const size_t count = filled < total ? filled : total;
				targets.reserve(count);
				for (size_t i = 0; i < count; ++i) {
					asset_browser::asset_move_target target;
					target.label = vgeditor::to_text(view[i].label);
					target.path_id = vgeditor::to_text(view[i].path_id);
					targets.push_back(std::move(target));
				}
				return targets;
			});
	}

	void vgui_editor_asset_browser_set_dependency_handler(
		vgui_editor_asset_browser* browser, vgui_editor_asset_dependency_fn fn,
		void* user)
	{
		if (!browser)
			return;
		if (!fn) {
			browser->dependency.clear();
			browser->impl.set_dependency_handler(nullptr);
			return;
		}
		browser->dependency.set(fn, user, nullptr);
		const vgui_editor_asset_dependency_fn slot_fn = browser->dependency.fn;
		void* slot_user = browser->dependency.user;
		vgeditor::user_owner_ptr owner = browser->dependency.owner;
		browser->impl.set_dependency_handler(
			[slot_fn, slot_user, owner](vgui_item_id item)
				-> std::vector<asset_browser::asset_dependency> {
				std::vector<asset_browser::asset_dependency> list;
				const size_t total = slot_fn(slot_user, item, nullptr, 0);
				if (total == 0)
					return list;
				std::vector<vgui_editor_asset_dependency> view(total);
				for (size_t i = 0; i < total; ++i) {
					std::memset(&view[i], 0, sizeof(view[i]));
					view[i].struct_size = (uint32_t)sizeof(view[i]);
				}
				const size_t filled = slot_fn(slot_user, item, view.data(), total);
				const size_t count = filled < total ? filled : total;
				list.reserve(count);
				for (size_t i = 0; i < count; ++i) {
					asset_browser::asset_dependency dep;
					dep.name = vgeditor::to_text(view[i].name);
					dep.type = vgeditor::to_text(view[i].type);
					dep.path_id = vgeditor::to_text(view[i].path_id);
					dep.id = view[i].id;
					list.push_back(std::move(dep));
				}
				return list;
			});
	}

	void vgui_editor_asset_browser_set_import_prompt(
		vgui_editor_asset_browser* browser, vgui_editor_asset_void_fn fn, void* user)
	{
		if (!browser)
			return;
		if (!fn) {
			browser->import_prompt.clear();
			browser->impl.set_import_prompt_handler(nullptr);
			return;
		}
		browser->import_prompt.set(fn, user, nullptr);
		const vgui_editor_asset_void_fn slot_fn = browser->import_prompt.fn;
		void* slot_user = browser->import_prompt.user;
		vgeditor::user_owner_ptr owner = browser->import_prompt.owner;
		browser->impl.set_import_prompt_handler([slot_fn, slot_user, owner]() {
			slot_fn(slot_user);
		});
	}

	int vgui_editor_asset_browser_remove_items(vgui_editor_asset_browser* browser,
		const vgui_item_id* items, size_t count)
	{
		if (!browser)
			return 0;
		return browser->impl.remove_items(to_ids(items, count)) ? 1 : 0;
	}

	int vgui_editor_asset_browser_remove_selected(vgui_editor_asset_browser* browser)
	{
		if (!browser)
			return 0;
		return browser->impl.remove_selected() ? 1 : 0;
	}

	int vgui_editor_asset_browser_duplicate_items(vgui_editor_asset_browser* browser,
		const vgui_item_id* items, size_t count)
	{
		if (!browser)
			return 0;
		return browser->impl.duplicate_items(to_ids(items, count)) ? 1 : 0;
	}

	int vgui_editor_asset_browser_duplicate_selected(
		vgui_editor_asset_browser* browser)
	{
		if (!browser)
			return 0;
		return browser->impl.duplicate_selected() ? 1 : 0;
	}

	int vgui_editor_asset_browser_move_items(vgui_editor_asset_browser* browser,
		const vgui_item_id* items, size_t count, const char* target_path_id)
	{
		if (!browser)
			return 0;
		return browser->impl.move_items(to_ids(items, count),
			vgeditor::to_text(target_path_id)) ? 1 : 0;
	}

	int vgui_editor_asset_browser_move_selected(vgui_editor_asset_browser* browser,
		const char* target_path_id)
	{
		if (!browser)
			return 0;
		return browser->impl.move_selected(vgeditor::to_text(target_path_id)) ? 1 : 0;
	}

	int vgui_editor_asset_browser_reimport_items(vgui_editor_asset_browser* browser,
		const vgui_item_id* items, size_t count)
	{
		if (!browser)
			return 0;
		return browser->impl.reimport_items(to_ids(items, count)) ? 1 : 0;
	}

	int vgui_editor_asset_browser_reimport_selected(
		vgui_editor_asset_browser* browser)
	{
		if (!browser)
			return 0;
		return browser->impl.reimport_selected() ? 1 : 0;
	}

	void vgui_editor_asset_browser_enable_context_menu(
		vgui_editor_asset_browser* browser)
	{
		if (!browser)
			return;
		browser->impl.enable_context_menu();
	}

	int vgui_editor_asset_browser_context_menu_enabled(
		vgui_editor_asset_browser* browser)
	{
		return browser && browser->impl.context_menu_enabled() ? 1 : 0;
	}

	int vgui_editor_asset_browser_perform_action(vgui_editor_asset_browser* browser,
		vgui_editor_asset_action kind, const char* argument)
	{
		if (!browser)
			return 0;
		asset_browser::action mapped;
		if (!to_action(kind, &mapped))
			return 0;
		return browser->impl.perform_action(mapped,
			vgeditor::to_text(argument)) ? 1 : 0;
	}

	int vgui_editor_asset_browser_begin_rename(vgui_editor_asset_browser* browser,
		vgui_item_id item)
	{
		if (!browser)
			return 0;
		return browser->impl.begin_rename(item) ? 1 : 0;
	}

	int vgui_editor_asset_browser_confirm_remove(vgui_editor_asset_browser* browser,
		const vgui_item_id* items, size_t count)
	{
		if (!browser)
			return 0;
		return browser->impl.confirm_remove(to_ids(items, count)) ? 1 : 0;
	}

	int vgui_editor_asset_browser_confirm_remove_selected(
		vgui_editor_asset_browser* browser)
	{
		if (!browser)
			return 0;
		return browser->impl.confirm_remove_selected() ? 1 : 0;
	}

	size_t vgui_editor_asset_browser_dependencies(vgui_editor_asset_browser* browser,
		vgui_item_id item, vgui_editor_asset_dependency* out, size_t out_count)
	{
		if (!browser)
			return 0;
		const std::vector<asset_browser::asset_dependency> list =
			browser->impl.dependencies(item);
		const size_t total = list.size();

		/* Reserved up front: the caller borrows these strings until the next
		 * mutation of this handle. */
		browser->dep_names.clear();
		browser->dep_names.reserve(total);
		browser->dep_types.clear();
		browser->dep_types.reserve(total);
		browser->dep_paths.clear();
		browser->dep_paths.reserve(total);
		for (const asset_browser::asset_dependency& dep : list) {
			browser->dep_names.push_back(dep.name);
			browser->dep_types.push_back(dep.type);
			browser->dep_paths.push_back(dep.path_id);
		}

		const size_t written = out ? (total < out_count ? total : out_count) : 0;
		for (size_t i = 0; i < written; ++i) {
			std::memset(&out[i], 0, sizeof(out[i]));
			out[i].struct_size = (uint32_t)sizeof(out[i]);
			out[i].name = browser->dep_names[i].c_str();
			out[i].type = browser->dep_types[i].c_str();
			out[i].path_id = browser->dep_paths[i].c_str();
			out[i].id = list[i].id;
		}
		return total;
	}

	int vgui_editor_asset_browser_show_dependencies(vgui_editor_asset_browser* browser,
		vgui_item_id item)
	{
		if (!browser)
			return 0;
		return browser->impl.show_dependencies(item) ? 1 : 0;
	}

	vgui_editor_asset_operation_error vgui_editor_asset_browser_last_error(
		vgui_editor_asset_browser* browser)
	{
		if (!browser)
			return VGUI_EDITOR_ASSET_ERR_NONE;
		return from_error(browser->impl.last_error());
	}

	size_t vgui_editor_asset_browser_last_message(
		vgui_editor_asset_browser* browser, char* out, size_t cap)
	{
		if (!browser)
			return 0;
		return vgeditor::copy_string(out, cap, browser->impl.last_message().c_str());
	}

	int vgui_editor_asset_browser_last_undoable(vgui_editor_asset_browser* browser)
	{
		return browser && browser->impl.last_undoable() ? 1 : 0;
	}

	vgui_widget* vgui_editor_asset_browser_root(vgui_editor_asset_browser* browser)
	{
		return browser ? browser->impl.root() : nullptr;
	}

	vgui_widget* vgui_editor_asset_browser_grid(vgui_editor_asset_browser* browser)
	{
		return browser ? browser->impl.grid() : nullptr;
	}

	vgui_widget* vgui_editor_asset_browser_scroll(vgui_editor_asset_browser* browser)
	{
		return browser ? browser->impl.scroll() : nullptr;
	}

	vgui_widget* vgui_editor_asset_browser_view_switch_widget(
		vgui_editor_asset_browser* browser)
	{
		if (!browser)
			return nullptr;
		segmented_control* switch_ = browser->impl.view_switch();
		return switch_ ? switch_->widget() : nullptr;
	}

	vgui_widget* vgui_editor_asset_browser_path_widget(
		vgui_editor_asset_browser* browser)
	{
		if (!browser)
			return nullptr;
		breadcrumb* path = browser->impl.path();
		return path ? path->widget() : nullptr;
	}

	vgui_widget* vgui_editor_asset_browser_search_widget(
		vgui_editor_asset_browser* browser)
	{
		if (!browser)
			return nullptr;
		search_box* search = browser->impl.search();
		return search ? search->widget() : nullptr;
	}

	vgui_widget* vgui_editor_asset_browser_type_combo_widget(
		vgui_editor_asset_browser* browser)
	{
		if (!browser)
			return nullptr;
		combo_box* combo = browser->impl.type_combo();
		return combo ? combo->widget() : nullptr;
	}

} /* extern "C" */

namespace vgeditor {

	vgui_editor_asset_browser* borrow_asset_browser(vek::asset_browser& browser)
	{
		return borrow_object<vgui_editor_asset_browser>(browser);
	}

	void release_borrowed_asset_browser(vgui_editor_asset_browser* handle)
	{
		/* Wrapper only: a borrowed handle's C++ object belongs to its container. */
		if (handle && !handle->owns_object())
			delete handle;
	}


} /* namespace vgeditor */
