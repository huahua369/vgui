/*
legacy C++ 绘图辅助实现

这些函数在 gui/vgui.cpp 重构为 C API 时被移出，但 sample/main.cpp（旧示例）
仍在使用，因此在这里按原实现恢复。

创建日期：2026-10-20
*/

#include "pch.h"
#include "vgui_internal.h"

#include <ovg_c.h>

#include <algorithm>
#include <cassert>
#include <cmath>

//----------------------------------------------------------------------------
// 颜色辅助（颜色按 ovg 的 BGRA 内存顺序存放）
//----------------------------------------------------------------------------

namespace {

union u_col
{
	uint32_t uc;
	unsigned char u[4];
	struct urgba
	{
		unsigned char r, g, b, a;
	} c;
};

inline uint32_t swap_rb(uint32_t c)
{
	return (c & 0xFF00FF00u) | ((c & 0x00FF0000u) >> 16) | ((c & 0x000000FFu) << 16);
}

inline uint32_t set_alpha_xf(uint32_t c, double af)
{
	if (af < 0) af = 0;
	u_col* t = (u_col*)&c;
	double a = t->c.a * af + 0.5;
	if (a > 255) t->c.a = 255;
	else t->c.a = (unsigned char)a;
	return c;
}

inline uint32_t set_alpha_xf2(uint32_t c, double af)
{
	if (af < 0) af = 0;
	u_col* t = (u_col*)&c;
	t->c.r = (unsigned char)(t->c.r * af);
	t->c.g = (unsigned char)(t->c.g * af);
	t->c.b = (unsigned char)(t->c.b * af);
	return c;
}

inline uint32_t set_alpha_x(uint32_t c, uint32_t a0)
{
	u_col* t = (u_col*)&c;
	double af = a0 / 255.0;
	double a = t->c.a * af + 0.5;
	if (a > 255) t->c.a = 255;
	else t->c.a = (unsigned char)a;
	return c;
}

inline uint32_t set_alpha_f(uint32_t c, double af)
{
	if (af > 1) af = 1;
	if (af < 0) af = 0;
	u_col* t = (u_col*)&c;
	t->c.a = (unsigned char)(af * 255.0 + 0.5);
	return c;
}

inline glm::vec4 to_c4(uint32_t c)
{
	u_col* t = (u_col*)&c;
	glm::vec4 fc;
	float* f = &fc.x;
	for (int i = 0; i < 4; i++)
		*f++ = t->u[i] / 255.0f;
	return fc;
}

inline bool is_alpha(uint32_t c)
{
	return (c & 0xFF000000) != 0;
}

inline double get_alpha_f(uint32_t c)
{
	u_col* t = (u_col*)&c;
	return t->c.a / 255.0;
}

inline glm::dvec4 mix_colors0(glm::vec4 a, glm::vec4 b, float ratio)
{
	auto lerp = [](double v0, double v1, double t) { return (1.0 - t) * v0 + t * v1; };
	glm::dvec4 result = {};
	result.x = lerp(a.x, b.x, ratio);
	result.y = lerp(a.y, b.y, ratio);
	result.z = lerp(a.z, b.z, ratio);
	result.w = lerp(a.w, b.w, ratio);
	return result;
}

//----------------------------------------------------------------------------
// 渐变辅助
//----------------------------------------------------------------------------

vg_pattern_t* new_cubic_gradient(ovg_ctx_cb* cb, rvg_t* vg, glm::vec4 rect,
	glm::vec4 from, glm::vec4 to, glm::vec2 ctrl1, glm::vec2 ctrl2,
	glm::vec2 p0 = {}, glm::vec2 p1 = { 1, 1 }, int steps = 8)
{
	for (auto&& pt : { p0, ctrl1, ctrl2, p1 })
	{
		if (pt.x < 0 || pt.x > 1 || pt.y < 0 || pt.y > 1)
		{
			assert(0);
			return 0;
		}
	}
	if (steps < 2 || steps > 32)
	{
		assert(0);
		return 0;
	}
	if (rect.x > rect.z) std::swap(rect.x, rect.z);
	if (rect.y > rect.w) std::swap(rect.y, rect.w);
	vg_pattern_t* g = cb->new_pattern_linear(vg, rect.x, rect.y, rect.z, rect.w);
	if (!g) return 0;
	--steps;
	for (int step = 0; step <= steps; ++step)
	{
		auto t = 1.0 * step / steps;
		auto s = 1.0 - t;
		auto p0t = p0 * (float)(t * t * t);
		auto c1 = ctrl1 * (float)(3 * t * t * s);
		auto c2 = ctrl2 * (float)(3 * t * s * s);
		auto p1t = p1 * (float)(s * s * s);
		auto p = p0t + c1 + c2 + p1t;
		auto color = mix_colors0(from, to, (float)p.y);
		cb->pattern_add_color_stop(g, p.x, (float)color.x, (float)color.y, (float)color.z, (float)color.w);
	}
	return g;
}

void paint_linear(ovg_ctx_cb* cb, rvg_t* vg, const glm::vec2& gsize, const glm::vec2& box,
	const glm::vec4& c, const glm::vec4& c1, int rounding)
{
	auto pat = cb->new_pattern_linear(vg, 0, 0, gsize.x, gsize.y);
	if (pat)
	{
		cb->pattern_add_color_stop(pat, 0, c.x, c.y, c.z, c.w);
		cb->pattern_add_color_stop(pat, 1, c1.x, c1.y, c1.z, c1.w);
		cb->set_source(vg, pat);
		cb->rounded_rectangle(vg, 0, 0, box.x, box.y, (float)rounding);
		cb->fill(vg);
	}
}

void paint_shadow_v(ovg_ctx_cb* cb, rvg_t* vg, double size_x, double size_y,
	double width, double height, const glm::vec4& shadow, bool rev, float r)
{
	auto trans = shadow;
	trans.w = 0;
	auto gr = rev ?
		new_cubic_gradient(cb, vg, glm::vec4(0, 0, (float)size_x, (float)size_y), trans, shadow,
			glm::vec2(0.5f, 0.0f), glm::vec2(1.0f, 0.5f)) :
		new_cubic_gradient(cb, vg, glm::vec4(0, 0, (float)size_x, (float)size_y), shadow, trans,
			glm::vec2(0.0f, 0.5f), glm::vec2(0.5f, 1.0f));
	if (!gr) return;
	cb->rounded_rectangle(vg, 0, 0, (float)width, (float)height, r);
	cb->set_source(vg, gr);
	cb->fill(vg);
}

void paint_shadow_v(ovg_ctx_cb* cb, rvg_t* vg, double size_x, double size_y,
	double width, double height, const glm::vec4& shadow, const glm::vec4& color_to,
	bool rev, float r)
{
	auto gr = rev ?
		new_cubic_gradient(cb, vg, glm::vec4(0, 0, (float)size_x, (float)size_y), color_to, shadow,
			glm::vec2(0.5f, 0.0f), glm::vec2(1.0f, 0.5f)) :
		new_cubic_gradient(cb, vg, glm::vec4(0, 0, (float)size_x, (float)size_y), shadow, color_to,
			glm::vec2(0.0f, 0.5f), glm::vec2(0.5f, 1.0f));
	if (!gr) return;
	cb->rounded_rectangle(vg, 0, 0, (float)width, (float)height, r);
	cb->set_source(vg, gr);
	cb->fill(vg);
}

} // namespace

//----------------------------------------------------------------------------
// 纯色按钮
//----------------------------------------------------------------------------

void set_btn_color_idx(color_style* cs, int idx)
{
	// 与旧实现相同的 5 组主题色（蓝 / 绿 / 灰 / 橙 / 红）
	static const uint32_t palette[5][8] =
	{
		{ 0xFFffffff, 0xFF409eff, 0xFF409eff, 0xFF66b1ff, 0xFFe6e6e6, 0xFF0d84ff, 0xFF0d84ff, 0 },
		{ 0xFFffffff, 0xFF67c23a, 0xFF67c23a, 0xFF85ce61, 0xFFe6e6e6, 0xFF529b2e, 0xFF529b2e, 0 },
		{ 0xFFffffff, 0xFF909399, 0xFF909399, 0xFFa6a9ad, 0xFFe6e6e6, 0xFF767980, 0xFF767980, 0 },
		{ 0xFFffffff, 0xFFe6a23c, 0xFFe6a23c, 0xFFebb563, 0xFFe6e6e6, 0xFFd48a1b, 0xFFd48a1b, 0 },
		{ 0xFFffffff, 0xFFf56c6c, 0xFFf56c6c, 0xFFf78989, 0xFFe6e6e6, 0xFFf23c3c, 0xFFf23c3c, 0 },
	};
	if (!cs || idx < 0 || idx >= 5) return;
	const uint32_t* p = palette[idx];
	cs->pdc.font_color = swap_rb(p[0]);
	cs->pdc.background_color = swap_rb(p[1]);
	cs->pdc.border_color = swap_rb(p[2]);
	cs->pdc.hover_color = swap_rb(p[3]);
	cs->pdc.active_font_color = swap_rb(p[4]);
	cs->pdc.active_background_color = swap_rb(p[5]);
	cs->pdc.active_border_color = swap_rb(p[6]);
	cs->pdc.hover_border_color = swap_rb(p[7]);
}

bool update_color_btn(color_style* p, float delta)
{
	(void)delta;
	if (!p) return false;
	if (p->_bst == p->_old_bst) return false;
	p->_old_bst = p->_bst;
	btn_cols_t* pdc = &p->pdc;
	p->_disabled = (p->_bst & (int)BTN_STATE::STATE_DISABLE) != 0;
	auto text_color = p->text.ptext_style ? p->text.ptext_style->color : 0;
	if (p->_disabled)
	{
		p->hover = false;
		p->dfill = pdc->background_color;
		p->dcol = pdc->border_color;
		if (p->effect == uTheme::dark)
		{
			p->dcol = 0;
			p->dtext_color = (text_color) ? text_color : pdc->font_color;
		}
		if (p->effect == uTheme::light)
		{
			p->dcol = set_alpha_xf(p->dcol, p->light * 3);
			p->dfill = set_alpha_xf(p->dfill, p->light);
			p->dtext_color = (text_color) ? text_color : pdc->border_color;
		}
		if (p->effect == uTheme::plain)
		{
			p->dfill = 0;
			p->dtext_color = (text_color) ? text_color : pdc->border_color;
		}
		p->dcol = set_alpha_x(p->dcol, p->disabled_alpha);
		p->dfill = set_alpha_x(p->dfill, p->disabled_alpha);
		p->dtext_color = set_alpha_x(p->dtext_color, p->disabled_alpha);
	}
	else
	{
		bool isdown = p->mPushed = (p->_bst & (int)BTN_STATE::STATE_ACTIVE) != 0;
		p->hover = (p->_bst & (int)BTN_STATE::STATE_HOVER) != 0;
		if (isdown)
		{
			p->dfill = pdc->active_background_color;
			p->dcol = pdc->active_border_color;
			if (p->effect == uTheme::plain)
			{
				p->dfill = set_alpha_xf(p->dcol, p->light);
			}
			else
			{
				p->dtext_color = (text_color) ? text_color : pdc->active_font_color;
			}
			if (p->effect == uTheme::light)
			{
				p->dfill = set_alpha_f(p->dfill, p->light * 5);
				p->dcol = set_alpha_f(p->dcol, p->light * 6);
			}
			if (p->effect == uTheme::dark)
			{
				p->dcol = 0;
			}
		}
		else if (p->hover)
		{
			p->dfill = pdc->hover_color;
			if (pdc->hover_border_color)
				p->dcol = pdc->hover_border_color;
			if (p->effect == uTheme::plain)
			{
				p->dcol = pdc->border_color;
				p->dfill = set_alpha_xf(p->dcol, p->light);
			}
			if (p->effect == uTheme::light)
			{
				p->dfill = set_alpha_f(p->dfill, p->light * 5);
				p->dtext_color = (text_color) ? text_color : pdc->font_color;
			}
		}
		else
		{
			p->dfill = pdc->background_color;
			p->dcol = pdc->border_color;
			if (p->effect == uTheme::dark)
			{
				p->dcol = 0;
				p->dtext_color = (text_color) ? text_color : pdc->font_color;
			}
			if (p->effect == uTheme::light)
			{
				p->dcol = set_alpha_xf(p->dcol, p->light * 3);
				p->dfill = set_alpha_xf(p->dfill, p->light);
				p->dtext_color = (text_color) ? text_color : pdc->border_color;
			}
			if (p->effect == uTheme::plain)
			{
				p->dfill = 0;
				p->dtext_color = (text_color) ? text_color : pdc->border_color;
			}
		}
	}
	return true;
}

void draw_color_btn(ovg_ctx_cb* cb, rvg_t* vg, color_style* t, const glm::ivec2& pos, const glm::ivec2& size)
{
	if (!cb || !vg || !t) return;
	auto ns = size;
	auto ss = size;
	int thickness = t->thickness;
	auto psv = pos;
	cb->save(vg);
	cb->translate(vg, (float)psv.x, (float)psv.y);
	if (t->dfill)
	{
		if (t->circle)
		{
			int r = (int)std::lround(ss.y * 0.5);
			cb->circle(vg, (float)r, (float)r, (float)r);
		}
		else
		{
			cb->rounded_rectangle(vg, 0, 0, (float)ss.x, (float)ss.y, (float)t->rounding);
		}
		cb->set_source_color(vg, t->dfill);
		cb->fill(vg);
	}
	glm::vec2 ps = { (float)(thickness * 2), (float)(thickness * 2) };
	if (t->mPushed) ps += t->pushedps;
	ns -= thickness * 4;
	float padding = thickness == 1 ? 0.5f : 0.0f;
	if (t->dcol)
	{
		if (t->circle)
		{
			int r = (int)std::lround(ss.y * 0.5);
			cb->circle(vg, (float)r, (float)r, (float)r);
		}
		else
		{
			cb->rounded_rectangle(vg, padding, padding, (float)ss.x, (float)ss.y, (float)t->rounding);
		}
		cb->set_line_width(vg, (float)thickness);
		cb->set_source_color(vg, t->dcol);
		cb->stroke(vg);
	}
	text_st_t dt = {};
	dt.text = (char*)t->text.str;
	dt.text_len = t->text.str_len;
	dt.pos = ps;
	dt.size = ns;
	cb->add_text(vg, &dt, t->text.ptext_style, nullptr);
	cb->restore(vg);
}

//----------------------------------------------------------------------------
// 渐变按钮
//----------------------------------------------------------------------------

void gradient_btn_init(gradient_style* p, uint32_t back_color, uint32_t text_color)
{
	if (!p) return;
	p->back_color = back_color;
	if (p->text.ptext_style)
		p->text.ptext_style->color = text_color;
	p->opacity = 1;
	p->borderLight = 0xff5c5c5c;
	p->borderDark = 0xff1d1d1d;
}

bool gradient_btn_update(gradient_style* p, float delta)
{
	(void)delta;
	if (!p) return false;
	if (p->_bst == p->_old_bst) return false;
	p->_old_bst = p->_bst;

	uint32_t gradTop = p->gradTop.x;
	uint32_t gradBot = p->gradBot.x;

	p->mPushed = (p->_bst & (int)BTN_STATE::STATE_ACTIVE) != 0;
	p->mMouseFocus = (p->_bst & (int)BTN_STATE::STATE_HOVER) != 0;
	if (p->_bst & (int)BTN_STATE::STATE_DISABLE)
		p->mEnabled = false;
	if (p->mPushed)
	{
		gradTop = p->gradTop.z;
		gradBot = p->gradBot.z;
	}
	else if (p->mMouseFocus && p->mEnabled)
	{
		gradTop = p->gradTop.y;
		gradBot = p->gradBot.y;
	}
	p->_gradTop = gradTop;
	p->_gradBot = gradBot;
	return true;
}

void gradient_btn_draw(ovg_ctx_cb* cb, rvg_t* vg, gradient_style* p, const glm::ivec2& pos, const glm::ivec2& size)
{
	if (!cb || !vg || !p) return;
	float x = (float)pos.x, y = (float)pos.y, w = (float)size.x, h = (float)size.y;
	uint32_t gradTop = p->_gradTop;
	uint32_t gradBot = p->_gradBot;
	uint32_t borderDark = p->borderDark;
	uint32_t borderLight = p->borderLight;
	double oa = p->opacity;

	auto bc = p->effect == uTheme::light ? p->back_color : set_alpha_xf2(p->back_color, get_alpha_f(p->back_color));
	double rounding = p->rounding;
	auto nr = (int)std::min(w * 0.5f, h * 0.5f);
	if (rounding > nr) rounding = nr;
	auto thickness = (float)p->thickness;
	glm::vec2 tps = { 0.5f, 0.5f };
	if (thickness > 1) tps *= 0.0f;

	cb->save(vg);
	cb->translate(vg, x, y);
	if (is_alpha(bc))
	{
		bc = set_alpha_f(bc, oa);
		cb->rounded_rectangle(vg, thickness, thickness, w - thickness * 2, h - thickness * 2, (float)rounding);
		cb->set_source_color(vg, bc);
		cb->fill(vg);
	}
	if (p->mPushed)
	{
		gradTop = set_alpha_f(gradTop, 0.8f);
		gradBot = set_alpha_f(gradBot, 0.8f);
	}
	else
	{
		double v = 1.0 - get_alpha_f(p->back_color);
		auto gv = p->mEnabled ? v : v * 0.5 + 0.5;
		gradTop = set_alpha_xf(gradTop, gv);
		gradBot = set_alpha_xf(gradBot, gv);
	}
	auto gtop = to_c4(gradTop);
	auto gbot = to_c4(gradBot);
	gradTop = set_alpha_xf(gradTop, oa);
	gradBot = set_alpha_xf(gradBot, oa);
	borderLight = set_alpha_xf(borderLight, oa);
	borderDark = set_alpha_xf(borderDark, oa);

	glm::vec2 rct = { w - thickness, h - thickness * 2.0f };
	glm::vec4 gtop2 = to_c4(gradTop);
	glm::vec4 gbot2 = to_c4(gradBot);

	if (p->effect == uTheme::dark || p->mPushed)
		cb->translate(vg, thickness, thickness);
	if (p->cubic_gradient)
		paint_shadow_v(cb, vg, 0, rct.y, rct.x, rct.y, gtop, gbot, 0, (float)rounding);
	else
		paint_linear(cb, vg, glm::vec2(0, rct.y), { rct.x, rct.y }, gtop2, gbot2, (int)rounding);
	if (p->effect == uTheme::dark || p->mPushed)
		cb->translate(vg, -thickness, -thickness);

	glm::vec2 ps = { thickness * 2, thickness * 2 };
	if (p->mPushed) ps += thickness;
	auto ns = size;
	ns -= (int)(thickness * 4);
	w -= 1;
	h -= 1;
	cb->set_line_width(vg, thickness);
	cb->set_source_color(vg, borderLight);
	cb->rounded_rectangle(vg, tps.x, tps.y + (p->mPushed ? 0.0f : 1.0f), w, h - (p->mPushed ? 0.0f : 1.0f), (float)rounding);
	cb->stroke(vg);
	cb->set_source_color(vg, borderDark);
	cb->rounded_rectangle(vg, tps.x, tps.y, w, h, (float)rounding);
	cb->stroke(vg);

	text_st_t dt = {};
	dt.text = (char*)p->text.str;
	dt.text_len = p->text.str_len;
	dt.pos = { 0, thickness * 0.5f };
	dt.size = { (float)ns.x, (float)ns.y };
	cb->add_text(vg, &dt, p->text.ptext_style, nullptr);
	cb->restore(vg);
}

//----------------------------------------------------------------------------
// 开关
//----------------------------------------------------------------------------

void switch_draw(ovg_ctx_cb* cb, rvg_t* vg, switch_info_t* p, const glm::ivec2& pos, const glm::ivec2& size0)
{
	if (!cb || !vg || !p) return;
	cb->save(vg);
	glm::ivec2 poss = pos;
	auto h = p->height;
	auto size = size0;
	auto fc = h * p->cv * 0.5f;
	auto ss = h * p->wf;
	if (size.x <= 0) size.x = (int)ss;
	if (size.y <= 0) size.y = (int)h;
	poss.x += (int)((size.x - ss) * 0.5f);
	poss.y += (int)((size.y - p->height) * 0.5f);
	cb->translate(vg, (float)poss.x, (float)poss.y);
	cb->rounded_rectangle(vg, 0, 0, ss, h, h * 0.5f);
	cb->set_source_color(vg, p->dcol);
	cb->fill(vg);
	{
		float cp_x = (ss - h) * p->cpos + h * 0.5f;
		float cp_y = h * 0.5f;
		cb->circle(vg, cp_x, cp_y, fc);
		cb->set_source_color(vg, p->color.z);
		cb->fill(vg);
	}
	cb->stroke(vg);
	cb->restore(vg);
}
