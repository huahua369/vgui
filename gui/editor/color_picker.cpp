#include <vgui/editor/color_picker.h>

#include "editor_metrics.h"

#include "pch.h"
#include "tools/tools.hpp"
#include "ui_scale.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace vgui_editor_kit {

	namespace {

		/* vgui 0xAARRGGBB <-> ovg tools 0xAABBGGRR:R/B 换位。 */
		uint32_t vgui_to_ovg(uint32_t c)
		{
			const uint32_t a = c & 0xFF000000u;
			const uint32_t r = (c >> 16) & 0xFFu;
			const uint32_t g = (c >> 8) & 0xFFu;
			const uint32_t b = c & 0xFFu;
			return a | (b << 16) | (g << 8) | r;
		}

		uint32_t ovg_to_vgui(uint32_t c)
		{
			return vgui_to_ovg(c);	/* 交换是对合的 */
		}

		int hex_digit(char c)
		{
			if (c >= '0' && c <= '9') return c - '0';
			if (c >= 'a' && c <= 'f') return c - 'a' + 10;
			if (c >= 'A' && c <= 'F') return c - 'A' + 10;
			return -1;
		}

		/* #RRGGBB / #RRGGBBAA(可带 # 或 0x 前缀、3/4 位短写) -> vgui ARGB。 */
		bool parse_hex_color(const std::string& text, uint32_t* out)
		{
			std::string s = text;
			if (s.rfind("0x", 0) == 0 || s.rfind("0X", 0) == 0)
				s = s.substr(2);
			else if (!s.empty() && s[0] == '#')
				s = s.substr(1);
			if (s.empty() || s.size() > 8)
				return false;
			int values[8] = {};
			for (char& c : s)
			{
				const int d = hex_digit(c);
				if (d < 0)
					return false;
				(&values[0])[&c - &s[0]] = d;
			}
			uint32_t r = 0, g = 0, b = 0, a = 255;
			switch (s.size())
			{
			case 3:
				r = (uint32_t)(values[0] * 17);
				g = (uint32_t)(values[1] * 17);
				b = (uint32_t)(values[2] * 17);
				break;
			case 4:
				r = (uint32_t)(values[0] * 17);
				g = (uint32_t)(values[1] * 17);
				b = (uint32_t)(values[2] * 17);
				a = (uint32_t)(values[3] * 17);
				break;
			case 6:
				r = (uint32_t)(values[0] * 16 + values[1]);
				g = (uint32_t)(values[2] * 16 + values[3]);
				b = (uint32_t)(values[4] * 16 + values[5]);
				break;
			case 8:
				r = (uint32_t)(values[0] * 16 + values[1]);
				g = (uint32_t)(values[2] * 16 + values[3]);
				b = (uint32_t)(values[4] * 16 + values[5]);
				a = (uint32_t)(values[6] * 16 + values[7]);
				break;
			default:
				return false;
			}
			if (out)
				*out = (a << 24) | (r << 16) | (g << 8) | b;
			return true;
		}

		std::string format_hex_color(uint32_t argb)
		{
			char buffer[16];
			const uint32_t a = (argb >> 24) & 0xFFu;
			if (a == 255)
			{
				std::snprintf(buffer, sizeof(buffer), "#%02X%02X%02X",
					(unsigned)((argb >> 16) & 0xFFu),
					(unsigned)((argb >> 8) & 0xFFu),
					(unsigned)(argb & 0xFFu));
			}
			else
			{
				std::snprintf(buffer, sizeof(buffer), "#%02X%02X%02X%02X",
					(unsigned)((argb >> 16) & 0xFFu),
					(unsigned)((argb >> 8) & 0xFFu),
					(unsigned)(argb & 0xFFu),
					(unsigned)a);
			}
			return buffer;
		}

		picker_rgb hsv_to_rgb(float h, float s, float v, float a)
		{
			const uint32_t c = ovg_to_vgui(ovg_tools::hsva(h, s, v, a));
			picker_rgb out;
			out.r = (float)((c >> 16) & 0xFFu) / 255.0f;
			out.g = (float)((c >> 8) & 0xFFu) / 255.0f;
			out.b = (float)(c & 0xFFu) / 255.0f;
			out.a = a;
			return out;
		}

		std::string rgb_to_hex(const picker_rgb& rgb)
		{
			const auto u8 = [](float v) -> uint32_t {
				const float x = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
				return (uint32_t)std::lround(x * 255.0f);
			};
			const uint32_t argb = (u8(rgb.a) << 24) | (u8(rgb.r) << 16) |
				(u8(rgb.g) << 8) | u8(rgb.b);
			return format_hex_color(argb);
		}

	} /* namespace */

	color_picker::~color_picker()
	{
		/* detached 子树随 context 销毁;这里无需手工清理。 */
	}

	void color_picker::build(vgui_context* ctx)
	{
		if (root_ || !ctx)
			return;
		ctx_ = ctx;
		root_ = vgui_widget_create(ctx, VGUI_PANEL, nullptr);
		if (!id_.empty())
			vgui_widget_set_id(root_, id_.c_str());
		vgui_widget_set_layout(root_, VGUI_LAYOUT_COLUMN);
		vgui_widget_set_padding(root_, 8, 8, 8, 8);
		vgui_widget_set_gap(root_, 6.0f);
		vgui_widget_set_background(root_, 0xFF22262E);
		vgui_widget_set_border(root_, 0xFF3A424E, 1.0f);
		vgui_widget_set_min_size(root_, 210, 0);

		/* 预览行:new / current / hex。 */
		vgui_widget* preview = vgui_widget_add(root_, VGUI_PANEL, nullptr);
		vgui_widget_set_layout(preview, VGUI_LAYOUT_ROW);
		vgui_widget_set_padding(preview, 0, 0, 0, 0);
		vgui_widget_set_gap(preview, 6.0f);
		new_swatch_ = vgui_widget_add(preview, VGUI_PANEL, nullptr);
		vgui_widget_set_id(new_swatch_, (id_ + ".new").c_str());
		const float row_h = editor_row_height(ctx);
		vgui_widget_set_size(new_swatch_, 44.0f, row_h);
		current_swatch_ = vgui_widget_add(preview, VGUI_PANEL, nullptr);
		vgui_widget_set_id(current_swatch_, (id_ + ".current").c_str());
		vgui_widget_set_size(current_swatch_, row_h, row_h);
		hex_ = vgui_widget_add(preview, VGUI_EDIT, nullptr);
		vgui_widget_set_id(hex_, (id_ + ".hex").c_str());
		/* Match the property-row type used by the row editors and popups. */
		vgui_widget_set_font_size(hex_, 13.0f);
		vgui_widget_set_grow(hex_, 1.0f);
		/* 宽由 grow 决定,高度跟随行高。 */
		vgui_widget_set_size(hex_, 0.0f, row_h);
		vgui_widget_set_min_size(hex_, 80, row_h);
		vgui_widget_on_event(hex_, &color_picker::on_hex_event, this);

		sv_ = vgui_widget_add(root_, VGUI_CUSTOM, nullptr);
		vgui_widget_set_id(sv_, (id_ + ".sv").c_str());
		vgui_widget_set_size(sv_, 0.0f, 128.0f);
		vgui_widget_set_align_self(sv_, VGUI_ALIGN_STRETCH);
		vgui_custom_set_draw(sv_, &color_picker::draw_sv, this);
		vgui_custom_set_event(sv_, &color_picker::on_sv_event, this);

		hue_ = vgui_widget_add(root_, VGUI_CUSTOM, nullptr);
		vgui_widget_set_id(hue_, (id_ + ".hue").c_str());
		vgui_widget_set_size(hue_, 0.0f, editor_row_inner(ctx));
		vgui_widget_set_align_self(hue_, VGUI_ALIGN_STRETCH);
		vgui_custom_set_draw(hue_, &color_picker::draw_hue, this);
		vgui_custom_set_event(hue_, &color_picker::on_hue_event, this);

		alpha_ = vgui_widget_add(root_, VGUI_CUSTOM, nullptr);
		vgui_widget_set_id(alpha_, (id_ + ".alpha").c_str());
		vgui_widget_set_size(alpha_, 0.0f, editor_row_inner(ctx));
		vgui_widget_set_align_self(alpha_, VGUI_ALIGN_STRETCH);
		vgui_custom_set_draw(alpha_, &color_picker::draw_alpha, this);
		vgui_custom_set_event(alpha_, &color_picker::on_alpha_event, this);

		swatch_row_ = vgui_widget_add(root_, VGUI_PANEL, nullptr);
		vgui_widget_set_id(swatch_row_, (id_ + ".swatches").c_str());
		vgui_widget_set_layout(swatch_row_, VGUI_LAYOUT_WRAP);
		vgui_widget_set_padding(swatch_row_, 0, 0, 0, 0);
		vgui_widget_set_gap(swatch_row_, 4.0f);
		build_swatch_row();
	}

	void color_picker::set_id(std::string id)
	{
		id_ = std::move(id);
	}

	vgui_widget* color_picker::widget() const noexcept
	{
		return root_;
	}

	void color_picker::build_swatch_row()
	{
		if (!swatch_row_)
			return;
		swatch_hexes_.clear();
		/* destroy 是延迟销毁,child_count 不会立即变;用 clear 一次性清空。 */
		vgui_widget_clear(swatch_row_);
		const auto add_swatch = [&](const std::string& hex)
		{
			uint32_t argb = 0;
			if (!parse_hex_color(hex, &argb))
				return;
			const int index = (int)swatch_hexes_.size();
			swatch_hexes_.push_back(hex);
			vgui_widget* swatch = vgui_widget_add(swatch_row_, VGUI_PANEL, nullptr);
			char widget_id[32];
			std::snprintf(widget_id, sizeof(widget_id), "%s.swatch.%d",
				id_.c_str(), index);
			vgui_widget_set_id(swatch, widget_id);
			const float sw = editor_row_inner(vgui_widget_get_context(swatch_row_));
			vgui_widget_set_size(swatch, sw, sw);
			vgui_widget_set_background(swatch, argb);
			vgui_widget_set_tooltip(swatch, hex.c_str());
			vgui_widget_on_event(swatch, &color_picker::on_swatch_click, this);
		};
		for (const std::string& hex : presets_)
			add_swatch(hex);
		if (!presets_.empty() && !recent_.empty())
		{
			vgui_widget* sep = vgui_widget_add(swatch_row_, VGUI_PANEL, nullptr);
			vgui_widget_set_size(sep, 2.0f, editor_row_inner(vgui_widget_get_context(swatch_row_)));
			vgui_widget_set_background(sep, 0xFF3A424E);
		}
		for (const std::string& hex : recent_)
			add_swatch(hex);
	}

	void color_picker::set_swatch_background(vgui_widget* swatch,
		const picker_rgb& rgb)
	{
		if (!swatch)
			return;
		vgui_widget_set_background(swatch, vgui_color(0) |
			((uint32_t)std::lround(std::clamp(rgb.a, 0.0f, 1.0f) * 255.0f) << 24) |
			((uint32_t)std::lround(std::clamp(rgb.r, 0.0f, 1.0f) * 255.0f) << 16) |
			((uint32_t)std::lround(std::clamp(rgb.g, 0.0f, 1.0f) * 255.0f) << 8) |
			(uint32_t)std::lround(std::clamp(rgb.b, 0.0f, 1.0f) * 255.0f));
	}

	void color_picker::sync_ui()
	{
		if (!root_)
			return;
		set_swatch_background(new_swatch_, edit_rgb_);
		uint32_t argb = 0;
		if (parse_hex_color(current_, &argb))
			vgui_widget_set_background(current_swatch_, argb);
		if (vgui_get_focus(ctx_) != hex_)
			vgui_edit_set_text(hex_, rgb_to_hex(edit_rgb_).c_str());
		/* 自绘控件每帧重绘,面板背景由 set_background 自行失效 */
	}

	void color_picker::set_value(const std::string& hex)
	{
		const std::string edit_hex = to_edit_ ? to_edit_(hex) : hex;
		uint32_t argb = 0;
		if (!parse_hex_color(edit_hex, &argb))
			return;
		const float r = (float)((argb >> 16) & 0xFFu) / 255.0f;
		const float g = (float)((argb >> 8) & 0xFFu) / 255.0f;
		const float b = (float)(argb & 0xFFu) / 255.0f;
		const uint32_t ovg = vgui_to_ovg(argb);
		ovg_tools::rgb_to_hsv(ovg, h_deg_, s_, v_);
		a_ = (float)((argb >> 24) & 0xFFu) / 255.0f;
		edit_rgb_ = hsv_to_rgb(h_deg_, s_, v_, a_);
		value_ = hex;
		sync_ui();
	}

	const std::string& color_picker::value() const noexcept
	{
		return value_;
	}

	void color_picker::set_current(const std::string& hex)
	{
		current_ = hex;
		sync_ui();
	}

	void color_picker::set_on_change(color_listener listener)
	{
		change_listener_ = std::move(listener);
	}

	void color_picker::set_on_press(color_listener listener)
	{
		press_listener_ = std::move(listener);
	}

	void color_picker::set_on_commit(color_listener listener)
	{
		commit_listener_ = std::move(listener);
	}

	void color_picker::set_presets(std::vector<std::string> presets)
	{
		presets_ = std::move(presets);
		build_swatch_row();
		sync_ui();
	}

	void color_picker::set_recent(std::vector<std::string> recent)
	{
		recent_ = std::move(recent);
		build_swatch_row();
		sync_ui();
	}

	void color_picker::set_color_transform(transform_fn to_edit,
		transform_fn from_edit)
	{
		to_edit_ = std::move(to_edit);
		from_edit_ = std::move(from_edit);
	}

	bool color_picker::valid() const noexcept
	{
		uint32_t argb = 0;
		return parse_hex_color(to_edit_ ? to_edit_(value_) : value_, &argb);
	}

	float color_picker::hue_degrees() const noexcept
	{
		return h_deg_;
	}

	void color_picker::apply_color(const picker_rgb& rgb, bool commit, bool sync_hsv)
	{
		edit_rgb_ = rgb;
		if (sync_hsv)
		{
			/* swatch/hex 路径从 RGB 回填 HSV。
			 * 组装直接用 ovg 序 0xAABBGGRR,rgb_to_hsv 按此读取。 */
			const uint32_t ovg =
				((uint32_t)std::lround(std::clamp(rgb.a, 0.0f, 1.0f) * 255.0f) << 24) |
				((uint32_t)std::lround(std::clamp(rgb.b, 0.0f, 1.0f) * 255.0f) << 16) |
				((uint32_t)std::lround(std::clamp(rgb.g, 0.0f, 1.0f) * 255.0f) << 8) |
				(uint32_t)std::lround(std::clamp(rgb.r, 0.0f, 1.0f) * 255.0f);
			ovg_tools::rgb_to_hsv(ovg, h_deg_, s_, v_);
		}
		a_ = rgb.a;

		const std::string edit_hex = rgb_to_hex(rgb);
		value_ = from_edit_ ? from_edit_(edit_hex) : edit_hex;
		sync_ui();
		if (change_listener_)
			change_listener_(value_);
		if (commit && commit_listener_)
			commit_listener_(value_);
	}

	void color_picker::commit_edit_text()
	{
		if (!hex_)
			return;
		const char* text = vgui_edit_get_text(hex_);
		uint32_t argb = 0;
		if (!text || !parse_hex_color(text, &argb))
			return;	/* 非法文本保留在编辑框,不改值 */
		const float r = (float)((argb >> 16) & 0xFFu) / 255.0f;
		const float g = (float)((argb >> 8) & 0xFFu) / 255.0f;
		const float b = (float)(argb & 0xFFu) / 255.0f;
		apply_color({ r, g, b, (float)((argb >> 24) & 0xFFu) / 255.0f }, true);
	}

	void color_picker::on_swatch_click(vgui_widget* widget,
		const vgui_event* event, void* user)
	{
		auto* self = static_cast<color_picker*>(user);
		if (!self || event->type != VGUI_EVENT_CLICK || event->button != 1)
			return;
		/* widget id 形如 "<id>.swatch.N",N 是 swatch_hexes_ 的下标。 */
		const char* widget_id = vgui_widget_get_id(widget);
		if (!widget_id)
			return;
		const char* dot = std::strrchr(widget_id, '.');
		if (!dot || std::strncmp(dot, ".swatch.", 8) != 0)
			return;
		const int index = std::atoi(dot + 8);
		if (index < 0 || (std::size_t)index >= self->swatch_hexes_.size())
			return;
		uint32_t argb = 0;
		const std::string& hex = self->swatch_hexes_[(std::size_t)index];
		if (!parse_hex_color(hex, &argb))
			return;
		const float r = (float)((argb >> 16) & 0xFFu) / 255.0f;
		const float g = (float)((argb >> 8) & 0xFFu) / 255.0f;
		const float b = (float)(argb & 0xFFu) / 255.0f;
		self->apply_color({ r, g, b, (float)((argb >> 24) & 0xFFu) / 255.0f }, true);
	}

	void color_picker::on_hex_event(vgui_widget* widget, const vgui_event* event,
		void* user)
	{
		(void)widget;
		auto* self = static_cast<color_picker*>(user);
		if (!self || !event)
			return;
		if (event->type == VGUI_EVENT_SUBMIT)
			self->commit_edit_text();
		else if (event->type == VGUI_EVENT_CANCEL)
		{
			/* 恢复显示当前 edit 色。 */
			if (self->hex_)
				vgui_edit_set_text(self->hex_,
					rgb_to_hex(self->edit_rgb_).c_str());
		}
	}

	bool color_picker::accept_drag_event(vgui_widget* widget,
		const vgui_event* event)
	{
		if (!event)
			return false;
		switch (event->type)
		{
		case VGUI_EVENT_MOUSE_DOWN:
			if (event->button != 1)
				return false;
			drag_widget_ = widget;
			dragging_ = true;
			if (press_listener_)
				press_listener_(value_);
			return true;
		case VGUI_EVENT_MOUSE_MOVE:
			/* 鼠标移动事件在 vgui 0.3 里 button 为 0,必须靠按下时记录的
			 * 控件判断归属;否则拖过相邻色条会同时改动另一个通道。 */
			return dragging_ && drag_widget_ == widget;
		case VGUI_EVENT_MOUSE_UP:
			if (event->button != 1 || !dragging_ || drag_widget_ != widget)
				return false;
			dragging_ = false;
			drag_widget_ = nullptr;
			return true;
		case VGUI_EVENT_CANCEL:
			dragging_ = false;
			drag_widget_ = nullptr;
			return false;
		default:
			return false;
		}
	}

	int color_picker::on_sv_event(vgui_widget* widget, const vgui_event* event,
		void* user)
	{
		auto* self = static_cast<color_picker*>(user);
		if (!self || !event || !self->accept_drag_event(widget, event))
			return 0;

		float wx = 0, wy = 0, ww = 0, wh = 0;
		vgui_widget_get_rect(widget, &wx, &wy, &ww, &wh);
		(void)wx;
		(void)wy;
		self->s_ = std::clamp(event->x / std::max(1.0f, ww), 0.0f, 1.0f);
		self->v_ = std::clamp(1.0f - event->y / std::max(1.0f, wh), 0.0f, 1.0f);
		self->apply_color(hsv_to_rgb(self->h_deg_, self->s_, self->v_, self->a_),
			event->type == VGUI_EVENT_MOUSE_UP, false);
		return 1;
	}

	int color_picker::on_hue_event(vgui_widget* widget, const vgui_event* event,
		void* user)
	{
		auto* self = static_cast<color_picker*>(user);
		if (!self || !event || !self->accept_drag_event(widget, event))
			return 0;

		float wx = 0, wy = 0, ww = 0, wh = 0;
		vgui_widget_get_rect(widget, &wx, &wy, &ww, &wh);
		(void)wx;
		(void)wy;
		(void)wh;
		self->h_deg_ = std::clamp(event->x / std::max(1.0f, ww), 0.0f, 1.0f) * 360.0f;
		self->apply_color(hsv_to_rgb(self->h_deg_, self->s_, self->v_, self->a_),
			event->type == VGUI_EVENT_MOUSE_UP, false);
		return 1;
	}

	int color_picker::on_alpha_event(vgui_widget* widget, const vgui_event* event,
		void* user)
	{
		auto* self = static_cast<color_picker*>(user);
		if (!self || !event || !self->accept_drag_event(widget, event))
			return 0;

		float wx = 0, wy = 0, ww = 0, wh = 0;
		vgui_widget_get_rect(widget, &wx, &wy, &ww, &wh);
		(void)wx;
		(void)wy;
		(void)wh;
		self->a_ = std::clamp(event->x / std::max(1.0f, ww), 0.0f, 1.0f);
		self->apply_color(hsv_to_rgb(self->h_deg_, self->s_, self->v_, self->a_),
			event->type == VGUI_EVENT_MOUSE_UP, false);
		return 1;
	}

	void color_picker::draw_sv(vgui_widget* widget, ovg_ctx_cb* cb, rvg_t* vg,
		float x, float y, float w, float h, float dt, void* user)
	{
		(void)dt;
		auto* self = static_cast<color_picker*>(user);
		if (!self || !cb || !vg)
			return;
		ovg_tools::sv_box(cb, vg, x, y, w, h, self->h_deg_);
		const float cx = x + self->s_ * w;
		const float cy = y + (1.0f - self->v_) * h;
		ovg_tools::sv_cursor(cb, vg, cx, cy, 5.0f * ui_scale_of(widget));
	}

	void color_picker::draw_hue(vgui_widget* widget, ovg_ctx_cb* cb, rvg_t* vg,
		float x, float y, float w, float h, float dt, void* user)
	{
		(void)widget; (void)dt;
		auto* self = static_cast<color_picker*>(user);
		if (!self || !cb || !vg)
			return;
		ovg_tools::hue_bar(cb, vg, x, y, w, h);
		ovg_tools::bar_thumb(cb, vg, x + self->h_deg_ / 360.0f * w,
			y + h * 0.5f, 6.0f * ui_scale_of(widget));
	}

	void color_picker::draw_alpha(vgui_widget* widget, ovg_ctx_cb* cb, rvg_t* vg,
		float x, float y, float w, float h, float dt, void* user)
	{
		(void)widget; (void)dt;
		auto* self = static_cast<color_picker*>(user);
		if (!self || !cb || !vg)
			return;
		const uint32_t opaque = vgui_to_ovg(
			0xFF000000u |
			((uint32_t)std::lround(std::clamp(self->edit_rgb_.r, 0.0f, 1.0f) * 255.0f) << 16) |
			((uint32_t)std::lround(std::clamp(self->edit_rgb_.g, 0.0f, 1.0f) * 255.0f) << 8) |
			(uint32_t)std::lround(std::clamp(self->edit_rgb_.b, 0.0f, 1.0f) * 255.0f));
		ovg_tools::alpha_bar(cb, vg, x, y, w, h, opaque);
		ovg_tools::bar_thumb(cb, vg, x + self->a_ * w, y + h * 0.5f,
			6.0f * ui_scale_of(widget));
	}

} /* namespace vgui_editor_kit */
