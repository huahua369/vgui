/* gui/editor/editor_c_api_util.cpp - shared plumbing for the editor C facade. */
#include "editor_c_api.h"

#include <utility>

namespace vgeditor {

	namespace {

		thread_local std::string g_last_error;

	} /* namespace */

	void set_error(const std::string& text)
	{
		g_last_error = text;
	}

	/* Anchors the handle base vtable in the library. */
	c_object_base::~c_object_base() = default;

	std::string& error_storage()
	{
		return g_last_error;
	}

	size_t copy_string(char* out, size_t cap, const char* text)
	{
		if (!text)
			text = "";
		const size_t length = std::strlen(text);
		if (out && cap > 0)
		{
			const size_t bytes = length < cap - 1 ? length : cap - 1;
			std::memcpy(out, text, bytes);
			out[bytes] = '\0';
		}
		return length;
	}

	std::string to_text(const char* text)
	{
		return text ? std::string(text) : std::string();
	}

	std::vector<std::string> to_text_list(const char* const* items, size_t count)
	{
		std::vector<std::string> out;
		if (!items)
			return out;
		out.reserve(count);
		for (size_t i = 0; i < count; ++i)
			out.push_back(to_text(items[i]));
		return out;
	}

	user_owner::~user_owner()
	{
		if (release && user)
			release(user);
	}

	user_owner_ptr make_owner(void* user, vgui_editor_release_fn release)
	{
		user_owner_ptr owner = std::make_shared<user_owner>();
		owner->user = user;
		owner->release = release;
		return owner;
	}

	c_command::c_command(std::string name, vgui_editor_execute_fn execute,
		vgui_editor_undo_fn undo, user_owner_ptr owner)
		: name_(std::move(name)), execute_(execute), undo_(undo), owner_(std::move(owner))
	{
	}

	const char* c_command::name() const noexcept
	{
		return name_.c_str();
	}

	bool c_command::execute()
	{
		if (!execute_)
			return true;
		return execute_(owner_->user) != 0;
	}

	void c_command::undo()
	{
		if (undo_)
			undo_(owner_->user);
	}

	void fill_c_payload(vgui_editor_drag_payload* out,
		const vgui_editor_kit::drag_payload& payload,
		std::vector<const char*>& file_ptrs)
	{
		if (!out)
			return;
		std::memset(out, 0, sizeof(*out));
		out->struct_size = (uint32_t)sizeof(*out);
		file_ptrs.clear();
		for (const std::string& file : payload.files)
			file_ptrs.push_back(file.c_str());
		switch (payload.kind)
		{
		case vgui_editor_kit::drag_payload_kind::text:
			out->kind = VGUI_EDITOR_DRAG_TEXT;
			break;
		case vgui_editor_kit::drag_payload_kind::files:
			out->kind = VGUI_EDITOR_DRAG_FILES;
			break;
		case vgui_editor_kit::drag_payload_kind::asset:
			out->kind = VGUI_EDITOR_DRAG_ASSET;
			break;
		case vgui_editor_kit::drag_payload_kind::scene_object:
			out->kind = VGUI_EDITOR_DRAG_SCENE_OBJECT;
			break;
		case vgui_editor_kit::drag_payload_kind::custom:
			out->kind = VGUI_EDITOR_DRAG_CUSTOM;
			break;
		case vgui_editor_kit::drag_payload_kind::none:
		default:
			out->kind = VGUI_EDITOR_DRAG_NONE;
			break;
		}
		out->text = payload.text.c_str();
		out->id = payload.id.c_str();
		out->files = file_ptrs.empty() ? nullptr : file_ptrs.data();
		out->file_count = (int)file_ptrs.size();
		out->data = payload.data;
	}

	vgui_editor_kit::drag_payload from_c_payload(const vgui_editor_drag_payload* payload)
	{
		vgui_editor_kit::drag_payload out;
		if (!payload)
			return out;
		const vgui_editor_drag_payload desc = take_desc(payload);
		switch (desc.kind)
		{
		case VGUI_EDITOR_DRAG_TEXT:
			out.kind = vgui_editor_kit::drag_payload_kind::text;
			break;
		case VGUI_EDITOR_DRAG_FILES:
			out.kind = vgui_editor_kit::drag_payload_kind::files;
			break;
		case VGUI_EDITOR_DRAG_ASSET:
			out.kind = vgui_editor_kit::drag_payload_kind::asset;
			break;
		case VGUI_EDITOR_DRAG_SCENE_OBJECT:
			out.kind = vgui_editor_kit::drag_payload_kind::scene_object;
			break;
		case VGUI_EDITOR_DRAG_CUSTOM:
			out.kind = vgui_editor_kit::drag_payload_kind::custom;
			break;
		default:
			out.kind = vgui_editor_kit::drag_payload_kind::none;
			break;
		}
		out.text = to_text(desc.text);
		out.id = to_text(desc.id);
		out.data = desc.data;
		if (desc.files)
		{
			const int count = desc.file_count < 0 ? 0 : desc.file_count;
			for (int i = 0; i < count; ++i)
			{
				if (desc.files[i])
					out.files.emplace_back(desc.files[i]);
			}
		}
		return out;
	}

	bool c_drop_target::can_accept(const vgui_editor_kit::drag_payload& payload) const
	{
		if (!desc.can_accept)
			return !payload.empty();
		std::vector<const char*> file_ptrs;
		vgui_editor_drag_payload view;
		fill_c_payload(&view, payload, file_ptrs);
		return desc.can_accept(user, &view) != 0;
	}

	void c_drop_target::hover(const vgui_editor_kit::drag_payload& payload,
		vgui_widget* target, float x, float y, vgui_drop_hint* hint)
	{
		if (!desc.hover)
		{
			vgui_editor_kit::drop_target::hover(payload, target, x, y, hint);
			return;
		}
		std::vector<const char*> file_ptrs;
		vgui_editor_drag_payload view;
		fill_c_payload(&view, payload, file_ptrs);
		desc.hover(user, &view, target, x, y, hint);
	}

	bool c_drop_target::drop(const vgui_editor_kit::drag_payload& payload, float x,
		float y)
	{
		if (!desc.drop)
			return false;
		std::vector<const char*> file_ptrs;
		vgui_editor_drag_payload view;
		fill_c_payload(&view, payload, file_ptrs);
		return desc.drop(user, &view, x, y) != 0;
	}

	std::string shortcut_text(const vgui_editor_kit::command_shortcut& shortcut)
	{
		if (!shortcut.valid())
			return std::string();
		return shortcut.to_string();
	}

	vgui_editor_kit::command_shortcut parse_shortcut(const char* text)
	{
		if (!text || !*text)
		{
			vgui_editor_kit::command_shortcut empty;
			return empty;
		}
		return vgui_editor_kit::command_shortcut::parse(text);
	}

} /* namespace vgeditor */

extern "C" {

	size_t vgui_editor_copy_string(char* out, size_t cap, const char* text)
	{
		return vgeditor::copy_string(out, cap, text);
	}

	const char* vgui_editor_last_error(void)
	{
		return vgeditor::error_storage().c_str();
	}

	void vgui_editor_drag_payload_default(vgui_editor_drag_payload* payload)
	{
		if (!payload)
			return;
		std::memset(payload, 0, sizeof(*payload));
		payload->struct_size = (uint32_t)sizeof(*payload);
		payload->kind = VGUI_EDITOR_DRAG_NONE;
	}

	void vgui_editor_drop_target_desc_default(vgui_editor_drop_target_desc* desc)
	{
		if (!desc)
			return;
		std::memset(desc, 0, sizeof(*desc));
		desc->struct_size = (uint32_t)sizeof(*desc);
	}

	void vgui_editor_command_info_default(vgui_editor_command_info* info)
	{
		if (!info)
			return;
		std::memset(info, 0, sizeof(*info));
		info->struct_size = (uint32_t)sizeof(*info);
		info->enabled = 1;
	}

	void vgui_editor_combo_item_default(vgui_editor_combo_item* item)
	{
		if (!item)
			return;
		std::memset(item, 0, sizeof(*item));
		item->struct_size = (uint32_t)sizeof(*item);
		item->enabled = 1;
	}

	void vgui_editor_breadcrumb_item_default(vgui_editor_breadcrumb_item* item)
	{
		if (!item)
			return;
		std::memset(item, 0, sizeof(*item));
		item->struct_size = (uint32_t)sizeof(*item);
	}

	void vgui_editor_curve_key_default(vgui_editor_curve_key* key)
	{
		if (!key)
			return;
		std::memset(key, 0, sizeof(*key));
		key->struct_size = (uint32_t)sizeof(*key);
	}

	void vgui_editor_gradient_stop_default(vgui_editor_gradient_stop* stop)
	{
		if (!stop)
			return;
		std::memset(stop, 0, sizeof(*stop));
		stop->struct_size = (uint32_t)sizeof(*stop);
	}

	void vgui_editor_property_binding_default(
		vgui_editor_property_binding* binding)
	{
		if (!binding)
			return;
		std::memset(binding, 0, sizeof(*binding));
		binding->struct_size = (uint32_t)sizeof(*binding);
	}

	/* NULL strings select the kit defaults: empty value/tooltip, "General"
	 * category, automatic step. */
	void vgui_editor_property_desc_default(vgui_editor_property_desc* desc)
	{
		if (!desc)
			return;
		std::memset(desc, 0, sizeof(*desc));
		desc->struct_size = (uint32_t)sizeof(*desc);
		desc->type = VGUI_EDITOR_PROPERTY_TEXT;
		desc->semantic = VGUI_EDITOR_VECTOR_GENERIC;
	}

	void vgui_editor_asset_info_default(vgui_editor_asset_info* info)
	{
		if (!info)
			return;
		std::memset(info, 0, sizeof(*info));
		info->struct_size = (uint32_t)sizeof(*info);
	}

	void vgui_editor_log_location_default(vgui_editor_log_location* where)
	{
		if (!where)
			return;
		std::memset(where, 0, sizeof(*where));
		where->struct_size = (uint32_t)sizeof(*where);
		where->asset_id = VGUI_EDITOR_INVALID_OBJECT;
	}

	void vgui_editor_log_frame_default(vgui_editor_log_frame* frame)
	{
		if (!frame)
			return;
		std::memset(frame, 0, sizeof(*frame));
		frame->struct_size = (uint32_t)sizeof(*frame);
		vgui_editor_log_location_default(&frame->where);
	}

	void vgui_editor_log_entry_default(vgui_editor_log_entry* entry)
	{
		if (!entry)
			return;
		std::memset(entry, 0, sizeof(*entry));
		entry->struct_size = (uint32_t)sizeof(*entry);
		entry->level = VGUI_EDITOR_LOG_INFO;
		vgui_editor_log_location_default(&entry->where);
	}

} /* extern "C" */
