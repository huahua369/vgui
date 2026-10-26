#include "viewport_view.h"

#include "drag_bridge.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace vgui_editor_kit {

	namespace {

		/* 与 build() 里的边框宽度保持一致：纹理绘制在边框内侧，核心画的焦点
		 * 环留在内容之外仍然可见。 */
		constexpr float kBorderInset = 1.0f;

		/* F03/A04: 视口的 drop target——接受资产 / 场景对象并转发给宿主。
		 * 悬停时在光标处画一个紧凑落点方框,宿主拒绝则整个提示消失。 */
		class viewport_drop_target final : public drop_target
		{
		public:
			explicit viewport_drop_target(viewport_view* owner) : owner_(owner) {}

			bool can_accept(const drag_payload& payload) const override
			{
				return payload.kind == drag_payload_kind::asset ||
					payload.kind == drag_payload_kind::scene_object;
			}

			void hover(const drag_payload& payload, vgui_widget* target,
				float x, float y, vgui_drop_hint* hint) override
			{
				(void)payload;
				(void)target;
				if (!owner_ || !hint)
					return;
				/* 光标提示:80x48 的目标框,钳制在视口范围内。 */
				float wx = 0, wy = 0, ww = 0, wh = 0;
				if (owner_->widget())
					vgui_widget_get_rect(owner_->widget(), &wx, &wy, &ww, &wh);
				const float w = 80.0f, h = 48.0f;
				hint->kind = VGUI_DROP_HINT_RECT;
				hint->x = x - w * 0.5f;
				hint->y = y - h * 0.5f;
				if (ww > 0.0f)
				{
					if (ww >= w)
						hint->x = std::clamp(hint->x, wx, wx + ww - w);
					else
						hint->x = wx;
				}
				if (wh > 0.0f)
				{
					if (wh >= h)
						hint->y = std::clamp(hint->y, wy, wy + wh - h);
					else
						hint->y = wy;
				}
				hint->w = w;
				hint->h = h;
			}

			bool drop(const drag_payload& payload, float x, float y) override
			{
				if (!owner_)
					return false;
				return owner_->record_drop(payload, x, y);
			}

		private:
			viewport_view* owner_;
		};

	} /* namespace */

	viewport_view::~viewport_view()
	{
		if (context_ && focus_token_)
			vgui_context_remove_focus_listener(context_, focus_token_);
		focus_token_ = 0;
		if (surface_)
		{
			/* 组件对象可能先于控件树销毁：先摘掉回调，避免残留悬空 user。 */
			vgui_custom_set_draw(surface_, nullptr, nullptr);
			vgui_custom_set_event(surface_, nullptr, nullptr);
			set_editor_drop_target(surface_, nullptr);
		}
	}

	void viewport_view::build(vgui_widget* parent)
	{
		if (surface_ || !parent)
			return;

		context_ = vgui_widget_get_context(parent);
		surface_ = vgui_widget_add(parent, VGUI_CUSTOM, nullptr);
		vgui_widget_set_grow(surface_, 1.0f);
		vgui_widget_set_min_size(surface_, 320, 200);
		vgui_widget_set_background(surface_, 0xFF0E1116);
		vgui_widget_set_border(surface_, 0xFF2A3038, 1);
		vgui_widget_set_focusable(surface_, 1);
		vgui_custom_set_draw(surface_, &viewport_view::on_draw, this);
		vgui_custom_set_event(surface_, &viewport_view::on_event, this);
		if (context_)
			focus_token_ = vgui_context_add_focus_listener(context_,
				&viewport_view::on_focus_change, this);
	}

	void viewport_view::set_texture(vgui_texture* texture)
	{
		texture_ = texture;
	}

	vgui_texture* viewport_view::texture() const noexcept
	{
		return texture_;
	}

	bool viewport_view::poll_resize()
	{
		if (!surface_)
			return false;
		float x = 0, y = 0, w = 0, h = 0;
		vgui_widget_get_rect(surface_, &x, &y, &w, &h);
		const float inner_w = w - kBorderInset * 2.0f;
		const float inner_h = h - kBorderInset * 2.0f;
		if (inner_w <= 0.0f || inner_h <= 0.0f)
			return false;
		const int width = (int)std::lround(inner_w);
		const int height = (int)std::lround(inner_h);
		if (width == render_w_ && height == render_h_)
			return false;
		render_w_ = width;
		render_h_ = height;
		if (resize_listener_)
			resize_listener_(width, height);
		return true;
	}

	int viewport_view::render_width() const noexcept
	{
		return render_w_;
	}

	int viewport_view::render_height() const noexcept
	{
		return render_h_;
	}

	void viewport_view::get_render_size(int* width, int* height) const noexcept
	{
		if (width)
			*width = render_w_;
		if (height)
			*height = render_h_;
	}

	void viewport_view::set_resize_listener(resize_listener listener)
	{
		resize_listener_ = std::move(listener);
	}

	void viewport_view::set_event_listener(event_listener listener)
	{
		event_listener_ = std::move(listener);
	}

	int viewport_view::add_event_listener(event_listener listener)
	{
		if (!listener)
			return 0;
		event_listener_entry entry;
		entry.token = next_event_listener_++;
		entry.fn = std::move(listener);
		event_listeners_.push_back(std::move(entry));
		return event_listeners_.back().token;
	}

	void viewport_view::remove_event_listener(int token)
	{
		if (token <= 0)
			return;
		for (auto it = event_listeners_.begin(); it != event_listeners_.end(); ++it)
		{
			if (it->token != token)
				continue;
			event_listeners_.erase(it);
			return;
		}
	}

	void viewport_view::set_focus_listener(focus_listener listener)
	{
		focus_listener_ = std::move(listener);
	}

	bool viewport_view::dragging() const noexcept
	{
		return dragging_;
	}

	int viewport_view::drag_button() const noexcept
	{
		return drag_button_;
	}

	void viewport_view::cancel_interaction()
	{
		end_gesture(true);
	}

	bool viewport_view::focused() const
	{
		return context_ && surface_ && vgui_get_focus(context_) == surface_;
	}

	void viewport_view::request_focus()
	{
		if (context_ && surface_)
			vgui_set_focus(context_, surface_);
	}

	void viewport_view::set_drop_listener(drop_listener listener)
	{
		listener_ = std::move(listener);
		if (!surface_)
			return;
		if (!listener_)
		{
			set_editor_drop_target(surface_, nullptr);
			return;
		}
		if (!drop_target_)
			drop_target_ = std::make_unique<viewport_drop_target>(this);
		set_editor_drop_target(surface_, drop_target_.get());
	}

	const drag_payload& viewport_view::last_drop() const noexcept
	{
		return last_drop_;
	}

	float viewport_view::last_drop_x() const noexcept
	{
		return last_drop_x_;
	}

	float viewport_view::last_drop_y() const noexcept
	{
		return last_drop_y_;
	}

	bool viewport_view::record_drop(const drag_payload& payload, float x, float y)
	{
		if (!listener_ || payload.empty() || !listener_(payload, x, y))
			return false;
		last_drop_ = payload;
		last_drop_x_ = x;
		last_drop_y_ = y;
		return true;
	}

	vgui_widget* viewport_view::widget() const noexcept
	{
		return surface_;
	}

	void viewport_view::on_draw(vgui_widget* widget, ovg_ctx_cb* cb,
		rvg_t* vg, float abs_x, float abs_y, float abs_w, float abs_h,
		float delta_time, void* user)
	{
		(void)delta_time;
		auto* self = static_cast<viewport_view*>(user);
		if (!self || !widget || !cb || !vg)
			return;

		const float x = abs_x + kBorderInset;
		const float y = abs_y + kBorderInset;
		const float width = abs_w - kBorderInset * 2.0f;
		const float height = abs_h - kBorderInset * 2.0f;
		if (width <= 0.0f || height <= 0.0f)
			return;

		/* 核心绘制 VGUI_CUSTOM 时已在外层包 save/restore；这里只做只读采样，
		 * 不直接改 OVG 状态。 */
		if (self->texture_)
		{
			vgui_custom_draw_texture(widget, cb, vg, self->texture_,
				x, y, width, height, 0xFFFFFFFF);
			return;
		}

		vgui_theme theme = {};
		if (vgui_context* ctx = vgui_widget_get_context(widget))
			vgui_context_get_theme(ctx, &theme);
		vgui_custom_draw_text(widget, cb, vg, "No viewport texture",
			x, y, width, height, theme.disabled, 0.5f, 0.5f);
	}

	int viewport_view::on_event(vgui_widget* widget, const vgui_event* event,
		void* user)
	{
		(void)widget;
		auto* self = static_cast<viewport_view*>(user);
		if (!self || !event)
			return 0;

		switch (event->type)
		{
		case VGUI_EVENT_MOUSE_MOVE:
		case VGUI_EVENT_MOUSE_DOWN:
		case VGUI_EVENT_MOUSE_UP:
			self->last_x_ = event->x;
			self->last_y_ = event->y;
			break;
		default:
			break;
		}

		switch (event->type)
		{
		case VGUI_EVENT_MOUSE_DOWN:
			if (!self->dragging_ && event->button >= 1)
			{
				self->begin_gesture(*event);
				self->request_focus();
			}
			break;
		case VGUI_EVENT_MOUSE_UP:
			if (self->dragging_ && event->button == self->drag_button_)
			{
				/* 先转发抬起事件（坐标可能已在视口外），再结束手势。 */
				const bool consumed = self->forward_event(event);
				self->end_gesture(false);
				return consumed ? 1 : 0;
			}
			break;
		case VGUI_EVENT_CANCEL:
			if (self->dragging_)
			{
				/* 核心（vgui_input_cancel / 焦点旁落）已经取消交互，宿主只需要
				 * 收回自己的 Gizmo/相机操作。 */
				self->end_gesture(true);
				return 1;
			}
			break;
		default:
			break;
		}

		return self->forward_event(event) ? 1 : 0;
	}

	void viewport_view::on_focus_change(vgui_context* ctx, vgui_widget* old_focus,
		vgui_widget* new_focus, void* user)
	{
		(void)ctx;
		auto* self = static_cast<viewport_view*>(user);
		if (!self || !self->surface_)
			return;
		const bool was = old_focus == self->surface_;
		const bool now = new_focus == self->surface_;
		if (was == now)
			return;
		if (was && self->dragging_)
			self->end_gesture(true);
		if (self->focus_listener_)
			self->focus_listener_(now);
	}

	void viewport_view::begin_gesture(const vgui_event& event)
	{
		dragging_ = true;
		drag_button_ = event.button;
		last_x_ = event.x;
		last_y_ = event.y;
	}

	void viewport_view::end_gesture(bool cancelled)
	{
		if (!dragging_)
			return;
		const int button = drag_button_;
		dragging_ = false;
		drag_button_ = 0;
		if (!cancelled)
			return;

		vgui_event cancel = {};
		cancel.type = VGUI_EVENT_CANCEL;
		cancel.x = last_x_;
		cancel.y = last_y_;
		cancel.button = button;
		if (context_)
			cancel.mods = vgui_context_get_mods(context_);
		/* 合成的取消事件也必须经过附加监听器（相机/Gizmo 依赖它收尾）。 */
		forward_event(&cancel);
	}

	bool viewport_view::forward_event(const vgui_event* event)
	{
		if (!event)
			return false;
		bool consumed = false;
		if (event_listener_)
			consumed = event_listener_(event);
		if (event_listeners_.empty())
			return consumed;

		/* 监听器可能在回调里移除自己；先拍快照，调用前确认 token 仍然有效。 */
		const std::vector<event_listener_entry> snapshot = event_listeners_;
		for (const event_listener_entry& entry : snapshot)
		{
			bool alive = false;
			for (const event_listener_entry& live : event_listeners_)
			{
				if (live.token == entry.token)
				{
					alive = true;
					break;
				}
			}
			if (alive && entry.fn && entry.fn(event))
				consumed = true;
		}
		return consumed;
	}

} /* namespace vgui_editor_kit */
