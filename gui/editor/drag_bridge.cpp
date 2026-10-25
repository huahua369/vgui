#include "drag_bridge.h"

#include <cstddef>
#include <cstdint>

#include <utility>

namespace vgui_editor_kit {

	/* drop_target 默认 hover：悬停控件内的横向插入线。 */
	void drop_target::hover(const drag_payload& payload, vgui_widget* target,
		float x, float y, vgui_drop_hint* hint)
	{
		(void)payload;
		if (!hint) return;
		float wx = 0, wy = 0, ww = 0, wh = 0;
		if (target)
			vgui_widget_get_rect(target, &wx, &wy, &ww, &wh);
		hint->kind = VGUI_DROP_HINT_LINE;
		hint->x = wx;
		hint->y = y - 1.0f;
		hint->w = ww;
		hint->h = 2.0f;
	}

	drag_payload to_editor_payload(const vgui_drag_payload& payload)
	{
		drag_payload out;
		switch (payload.kind)
		{
		case VGUI_DRAG_TEXT:
			out.kind = drag_payload_kind::text;
			out.text = payload.text ? payload.text : "";
			break;
		case VGUI_DRAG_FILES:
			out.kind = drag_payload_kind::files;
			for (int i = 0; i < payload.file_count; ++i)
			{
				if (payload.files && payload.files[i])
					out.files.push_back(payload.files[i]);
			}
			break;
		case VGUI_DRAG_CUSTOM:
		{
			/* The kit kind rides in data; unknown markers fall back to custom. */
			const auto marker = reinterpret_cast<std::uintptr_t>(payload.data);
			out.kind = marker == static_cast<std::uintptr_t>(drag_payload_kind::asset)
				? drag_payload_kind::asset
				: marker == static_cast<std::uintptr_t>(drag_payload_kind::scene_object)
					? drag_payload_kind::scene_object
					: drag_payload_kind::custom;
			out.id = payload.text ? payload.text : "";
			out.data = payload.data;
			break;
		}
		default:
			break;
		}
		return out;
	}

	namespace {

		/* files 的 std::vector<std::string> 不是 char* 数组：展平成临时指针表，
		 * vgui_begin_drag 会立刻深拷贝，所以临时表的生命周期到调用结束即可。 */
		struct core_payload_scratch
		{
			std::vector<const char*> ptrs;
			vgui_drag_payload payload = {};

			explicit core_payload_scratch(const drag_payload& src)
			{
				switch (src.kind)
				{
				case drag_payload_kind::text:
					payload.kind = VGUI_DRAG_TEXT;
					payload.text = src.text.c_str();
					break;
				case drag_payload_kind::files:
					ptrs.reserve(src.files.size());
					for (const std::string& f : src.files)
						ptrs.push_back(f.c_str());
					payload.kind = VGUI_DRAG_FILES;
					payload.files = ptrs.data();
					payload.file_count = (int)ptrs.size();
					break;
				default:
					payload.kind = VGUI_DRAG_CUSTOM;
					payload.text = src.id.c_str();
					payload.data = reinterpret_cast<const void*>(
						static_cast<std::uintptr_t>(src.kind));
					break;
				}
			}
		};

		/* 每次挂载分配一块；控件析构或重新挂载时旧的块保留到进程结束
		 * （核心没有 destroy 回调，控件的堆地址可能被复用，按指针清理
		 * 反而会 use-after-free）。分配量以挂载次数为界，MVP 阶段接受。 */
		struct drop_adapter
		{
			vgui_drop_target core = {};
			vgui_widget* widget = nullptr;
			drop_target* impl = nullptr;
		};

		int drop_adapter_can_accept(void* user, const vgui_drag_payload* payload)
		{
			auto* adapter = static_cast<drop_adapter*>(user);
			if (!adapter || !adapter->impl || !payload) return 0;
			return adapter->impl->can_accept(to_editor_payload(*payload)) ? 1 : 0;
		}

		void drop_adapter_hover(void* user, const vgui_drag_payload* payload,
			float x, float y, vgui_drop_hint* hint)
		{
			auto* adapter = static_cast<drop_adapter*>(user);
			if (!adapter || !adapter->impl || !payload || !hint) return;
			adapter->impl->hover(to_editor_payload(*payload), adapter->widget,
				x, y, hint);
		}

	int drop_adapter_drop(void* user, const vgui_drag_payload* payload,
		float x, float y)
	{
		auto* adapter = static_cast<drop_adapter*>(user);
		if (!adapter || !adapter->impl || !payload) return 0;
		return adapter->impl->drop(to_editor_payload(*payload), x, y) ? 1 : 0;
	}

		/* 行拖拽存储块：provider 之外再持有一份 kit 载荷（keeper）。vgui 的
		 * row_drag_fn 返回后才开始拷贝字符串，若直接引用 thunk 局部变量，
		 * 拷贝发生时字符串已经销毁。keeper 保证拷贝窗口内有效。 */
		struct row_drag_entry
		{
			row_drag_provider provider;
			drag_payload keeper;
		};

		int row_drag_thunk(void* user, vgui_item_id item, int index,
			vgui_drag_payload* out_payload)
		{
			auto* entry = static_cast<row_drag_entry*>(user);
			if (!entry || !out_payload) return 0;
			entry->keeper = drag_payload();
			if (!entry->provider(item, index, entry->keeper) ||
				entry->keeper.kind == drag_payload_kind::none)
				return 0;
			core_payload_scratch scratch(entry->keeper);
			*out_payload = scratch.payload;
			return 1;
		}

	} /* namespace */

	bool begin_editor_drag(vgui_context* ctx, vgui_widget* source,
		const drag_payload& payload)
	{
		if (!ctx || payload.kind == drag_payload_kind::none)
			return false;
		core_payload_scratch scratch(payload);
		return vgui_begin_drag(ctx, source, &scratch.payload) != 0;
	}

	void set_editor_drop_target(vgui_widget* widget, drop_target* target)
	{
		if (!widget)
			return;
		if (!target)
		{
			vgui_widget_set_drop_target(widget, nullptr);
			return;
		}

		auto* adapter = new drop_adapter();
		adapter->widget = widget;
		adapter->impl = target;
		adapter->core.can_accept = &drop_adapter_can_accept;
		adapter->core.hover = &drop_adapter_hover;
		adapter->core.drop = &drop_adapter_drop;
		adapter->core.user = adapter;
		vgui_widget_set_drop_target(widget, &adapter->core);
	}

	void set_editor_row_drag(vgui_widget* widget, bool tree, row_drag_provider provider)
	{
		if (!widget)
			return;
		if (!provider)
		{
			if (tree)
				vgui_tree_set_row_drag(widget, nullptr, nullptr);
			else
				vgui_list_set_row_drag(widget, nullptr, nullptr);
			return;
		}

		auto* stored = new row_drag_entry{ row_drag_provider(std::move(provider)), {} };
		if (tree)
			vgui_tree_set_row_drag(widget, &row_drag_thunk, stored);
		else
			vgui_list_set_row_drag(widget, &row_drag_thunk, stored);
	}

} /* namespace vgui_editor_kit */
