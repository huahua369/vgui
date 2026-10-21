// sample/vgui_demo.cpp: vgui C API 控件演示程序
//
// 演示 gui/vgui.h 公开的纯 C API（注意：本文件除窗口/渲染后端外，
// 全部通过 C 接口操作 UI，没有任何 C++ 控件类参与）：
//   面板 / 标签 / 按钮 / 复选框 / 单选 / 开关 / 滑杆 / 进度条 / 编辑框 / 分隔线
// 以及：行列布局、对齐、间距、grow、justify、滚动面板、主题切换、焦点、
//       文本编辑辅助、动态增删控件、可见性与禁用状态。
//
// 窗口与事件由 SDL3 后端（backends/vgui_sdl3.h）提供。

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

#include "pch.h"		// glm 等基础类型（ovg_c.h 依赖）
#include <vgui_sdl3.h>	// app_mgr / gui_viewport / draw_datas_cx
#include <vgui.h>		// 公共 C API
#include "ovg_main.h"
//---------------------------------------------------------------------------
// 主题预设
//---------------------------------------------------------------------------

// 深色主题（与 vgui 默认主题一致）
static vgui_theme theme_dark()
{
	vgui_theme t = {};
	t.text = 0xFFE6E6E6;
	t.panel_bg = 0xFF1E1E1E;
	t.panel_border = 0xFF3C3C3C;
	t.button_bg = 0xFF3A3A3A;
	t.button_hover = 0xFF4A4A4A;
	t.button_active = 0xFF2A2A2A;
	t.button_border = 0xFF555555;
	t.accent = 0xFF409EFF;
	t.accent_hover = 0xFF66B1FF;
	t.track = 0xFF303030;
	t.edit_bg = 0xFF252525;
	t.edit_selection = 0x80409EFF;
	t.scrollbar = 0xFF555555;
	t.disabled = 0xFF808080;
	return t;
}

// 浅色主题
static vgui_theme theme_light()
{
	vgui_theme t = {};
	t.text = 0xFF303030;
	t.panel_bg = 0xFFF3F3F3;
	t.panel_border = 0xFFD2D2D2;
	t.button_bg = 0xFFE4E4E4;
	t.button_hover = 0xFFF2F2F2;
	t.button_active = 0xFFCFCFCF;
	t.button_border = 0xFFBFBFBF;
	t.accent = 0xFF2D7FF9;
	t.accent_hover = 0xFF5A9DF9;
	t.track = 0xFFDCDCDC;
	t.edit_bg = 0xFFFFFFFF;
	t.edit_selection = 0x802D7FF9;
	t.scrollbar = 0xFF9A9A9A;
	t.disabled = 0xFFAAAAAA;
	return t;
}

// 护眼主题
static vgui_theme theme_green()
{
	vgui_theme t = {};
	t.text = 0xFF1E3320;
	t.panel_bg = 0xFFC7E8CD;
	t.panel_border = 0xFF9CC7A4;
	t.button_bg = 0xFFB4DDBB;
	t.button_hover = 0xFFC6E8CB;
	t.button_active = 0xFF9DCFA6;
	t.button_border = 0xFF8AB894;
	t.accent = 0xFF2E8B57;
	t.accent_hover = 0xFF3FA96C;
	t.track = 0xFFA9CFAF;
	t.edit_bg = 0xFFE3F5E6;
	t.edit_selection = 0x802E8B57;
	t.scrollbar = 0xFF6FA57B;
	t.disabled = 0xFF8FA894;
	return t;
}

//---------------------------------------------------------------------------
// 演示状态
//---------------------------------------------------------------------------

struct demo_state
{
	vgui_context* ui = nullptr;

	// 状态栏 / 读数
	vgui_widget* status = nullptr;
	vgui_widget* focus_label = nullptr;
	vgui_widget* fps_label = nullptr;
	vgui_widget* edit_mirror = nullptr;
	vgui_widget* slider_h_readout = nullptr;
	vgui_widget* slider_c_readout = nullptr;
	vgui_widget* slider_v_readout = nullptr;

	// 回调中需要访问的控件
	vgui_widget* slider_h = nullptr;
	vgui_widget* slider_c = nullptr;
	vgui_widget* slider_v = nullptr;
	vgui_widget* anim_toggle = nullptr;
	vgui_widget* progress_auto = nullptr;
	vgui_widget* progress_manual = nullptr;
	vgui_widget* edit_single = nullptr;
	vgui_widget* edit_password = nullptr;
	vgui_widget* edit_multi = nullptr;
	vgui_widget* edit_readonly = nullptr;
	vgui_widget* sel_rounding = nullptr;
	vgui_widget* sel_rounding_readout = nullptr;
	vgui_widget* focus_alpha = nullptr;
	vgui_widget* focus_alpha_readout = nullptr;
	int sel_color_index = 0;
	vgui_widget* scroll_panel = nullptr;
	vgui_widget* scroll_info = nullptr;
	vgui_widget* bind_box = nullptr;
	vgui_widget* hidden_panel = nullptr;
	vgui_widget* hide_toggle = nullptr;
	vgui_widget* enable_toggle = nullptr;
	vgui_widget* enable_targets[4] = {};
	int enable_target_count = 0;
	vgui_widget* radios[3] = {};
	vgui_widget* radio_swatch = nullptr;
	const char* radio_names[3] = { "红", "绿", "蓝" };
	vgui_color radio_colors[3] = { 0xFFE05252, 0xFF52C05A, 0xFF4A7FE0 };

	// 数值状态
	int clicks = 0;
	int added = 0;
	float anim = 0.0f;
	int anim_dir = 1;
	bool anim_on = true;
};

//---------------------------------------------------------------------------
// 通用辅助
//---------------------------------------------------------------------------

static const char* kind_name(vgui_widget_kind k)
{
	switch (k)
	{
	case VGUI_PANEL: return "panel";
	case VGUI_LABEL: return "label";
	case VGUI_BUTTON: return "button";
	case VGUI_CHECKBOX: return "checkbox";
	case VGUI_RADIO: return "radio";
	case VGUI_SWITCH: return "switch";
	case VGUI_SLIDER: return "slider";
	case VGUI_PROGRESS: return "progress";
	case VGUI_EDIT: return "edit";
	case VGUI_SEPARATOR: return "separator";
	}
	return "widget";
}

static const char* id_of(vgui_widget* w)
{
	const char* id = vgui_widget_get_id(w);
	return (id && *id) ? id : kind_name(vgui_widget_get_kind(w));
}

// 状态栏文本
static void statusf(demo_state* st, const char* fmt, ...)
{
	if (!st || !st->status) return;
	char buf[512] = {};
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	vgui_widget_set_text(st->status, buf);
}

//---------------------------------------------------------------------------
// 布局辅助
//---------------------------------------------------------------------------

// 透明分隔容器（不含背景/边框/内边距）
static vgui_widget* plain(vgui_widget* parent, vgui_layout_kind layout, float gap)
{
	vgui_widget* p = vgui_widget_add(parent, VGUI_PANEL, nullptr);
	vgui_widget_set_layout(p, layout);
	vgui_widget_set_background(p, 0);
	vgui_widget_set_border(p, 0, 0);
	vgui_widget_set_padding(p, 0, 0, 0, 0);
	vgui_widget_set_gap(p, gap);
	return p;
}

static vgui_widget* row(vgui_widget* parent, float gap = 8)
{
	vgui_widget* p = plain(parent, VGUI_LAYOUT_ROW, gap);
	vgui_widget_set_align_items(p, VGUI_ALIGN_CENTER);
	return p;
}

static vgui_widget* col(vgui_widget* parent, float gap = 6)
{
	return plain(parent, VGUI_LAYOUT_COLUMN, gap);
}

// 分组卡片：列布局 + 内边距 + 标题
static vgui_widget* card(vgui_widget* parent, const char* title)
{
	vgui_widget* c = vgui_widget_add(parent, VGUI_PANEL, nullptr);
	vgui_widget_set_layout(c, VGUI_LAYOUT_COLUMN);
	vgui_widget_set_padding(c, 12, 10, 12, 12);
	vgui_widget_set_gap(c, 8);
	vgui_widget_set_align_items(c, VGUI_ALIGN_STRETCH);
	vgui_widget_set_id(c, title);
	if (title)
	{
		vgui_widget* t = vgui_widget_add(c, VGUI_LABEL, title);
		vgui_widget_set_text_color(t, 0xFF7FB6FF);
		vgui_widget_set_id(t, title);
	}
	return c;
}

static vgui_widget* label(vgui_widget* parent, const char* text, float size = 0, vgui_color color = 0)
{
	vgui_widget* l = vgui_widget_add(parent, VGUI_LABEL, text);
	if (size > 0) vgui_widget_set_font_size(l, size);
	if (color) vgui_widget_set_text_color(l, color);
	return l;
}

static vgui_widget* button(vgui_widget* parent, const char* text, vgui_event_fn fn, void* user)
{
	vgui_widget* b = vgui_widget_add(parent, VGUI_BUTTON, text);
	vgui_widget_set_id(b, text);
	if (fn) vgui_widget_on_event(b, fn, user);
	return b;
}

static void separator(vgui_widget* parent)
{
	vgui_widget_add(parent, VGUI_SEPARATOR, nullptr);
}

//---------------------------------------------------------------------------
// 回调
//---------------------------------------------------------------------------

// 通用事件回调：把事件写到状态栏
static void on_widget_event(vgui_widget* w, const vgui_event* e, void* user)
{
	demo_state* st = (demo_state*)user;
	const char* id = id_of(w);
	switch (e->type)
	{
	case VGUI_EVENT_CLICK:        statusf(st, "[%s] 单击", id); break;
	case VGUI_EVENT_DOUBLE_CLICK: statusf(st, "[%s] 双击", id); break;
	case VGUI_EVENT_TRIPLE_CLICK: statusf(st, "[%s] 三击", id); break;
	case VGUI_EVENT_ENTER:        statusf(st, "[%s] 鼠标进入 (%.0f, %.0f)", id, e->x, e->y); break;
	case VGUI_EVENT_LEAVE:        statusf(st, "[%s] 鼠标离开", id); break;
	case VGUI_EVENT_KEY_DOWN:     statusf(st, "[%s] 按键 key=%d mods=%d", id, e->key, e->mods); break;
	case VGUI_EVENT_SUBMIT:       statusf(st, "[%s] 回车提交: %s", id, vgui_widget_get_text(w)); break;
	case VGUI_EVENT_TEXT:         break;	// 由 on_edit_single_event 处理
	default:                      statusf(st, "[%s] 事件 %d", id, e->type); break;
	}
}

// 计数按钮：单击/双击/三击
static void on_count_click(vgui_widget* w, const vgui_event* e, void* user)
{
	(void)w;
	demo_state* st = (demo_state*)user;
	if (e->type == VGUI_EVENT_CLICK)
	{
		st->clicks++;
		statusf(st, "按钮点击 %d 次（试试双击/三击）", st->clicks);
	}
	else if (e->type == VGUI_EVENT_DOUBLE_CLICK)
		statusf(st, "双击触发！累计单击 %d 次", st->clicks);
	else if (e->type == VGUI_EVENT_TRIPLE_CLICK)
		statusf(st, "三击触发！累计单击 %d 次", st->clicks);
}

// 值改变（复选框/单选/开关/滑杆）
static void on_value(vgui_widget* w, float v, void* user)
{
	demo_state* st = (demo_state*)user;
	char buf[96];

	if (w == st->slider_h)
	{
		snprintf(buf, sizeof(buf), "值: %.0f", v);
		vgui_widget_set_text(st->slider_h_readout, buf);
		statusf(st, "[滑杆 0~100] = %.0f", v);
		return;
	}
	if (w == st->slider_c)
	{
		snprintf(buf, sizeof(buf), "值: %.2f", v);
		vgui_widget_set_text(st->slider_c_readout, buf);
		statusf(st, "[滑杆 -1~1] = %.2f", v);
		return;
	}
	if (w == st->slider_v)
	{
		snprintf(buf, sizeof(buf), "值: %.0f", v);
		vgui_widget_set_text(st->slider_v_readout, buf);
		statusf(st, "[垂直滑杆] = %.0f", v);
		return;
	}
	if (w == st->anim_toggle)
	{
		st->anim_on = v >= 0.5f;
		statusf(st, "自动动画: %s", st->anim_on ? "开" : "关");
		return;
	}
	if (w == st->hide_toggle)
	{
		int show = v >= 0.5f;
		vgui_widget_set_visible(st->hidden_panel, show);
		statusf(st, "附加面板: %s", show ? "显示" : "隐藏");
		return;
	}
	if (w == st->enable_toggle)
	{
		int on = v >= 0.5f;
		for (int i = 0; i < st->enable_target_count; i++)
			vgui_widget_set_enabled(st->enable_targets[i], on);
		statusf(st, "控件组: %s", on ? "启用" : "禁用");
		return;
	}
	for (int i = 0; i < 3; i++)
	{
		if (w == st->radios[i] && v >= 0.5f)
		{
			vgui_widget_set_background(st->radio_swatch, st->radio_colors[i]);
			statusf(st, "单选组 -> %s", st->radio_names[i]);
			return;
		}
	}
	statusf(st, "[%s] = %.2f", id_of(w), v);
}

// 更新单行编辑框的镜像标签（按 UTF-8 边界截断）
static void sync_edit_mirror(demo_state* st)
{
	std::string s = "镜像: ";
	s += vgui_edit_get_text(st->edit_single);
	if (s.size() > 64)
	{
		size_t n = 64;
		while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
		s.resize(n);
		s += "...";
	}
	vgui_widget_set_text(st->edit_mirror, s.c_str());
}

// 单行编辑框：镜像文本 + 回车提交
static void on_edit_single_event(vgui_widget* w, const vgui_event* e, void* user)
{
	demo_state* st = (demo_state*)user;
	if (e->type == VGUI_EVENT_TEXT)
	{
		sync_edit_mirror(st);
		statusf(st, "[单行编辑框] 输入: %s", vgui_edit_get_text(st->edit_single));
		return;
	}
	if (e->type == VGUI_EVENT_KEY_DOWN)
		sync_edit_mirror(st);	// 剪贴板/撤销等快捷键也在这里生效
	on_widget_event(w, e, user);
}

// 编辑框辅助按钮
static void on_edit_select_all(vgui_widget* w, const vgui_event* e, void* user)
{
	(void)w;
	if (e->type != VGUI_EVENT_CLICK) return;
	demo_state* st = (demo_state*)user;
	vgui_edit_select_all(st->edit_single);
	statusf(st, "编辑框: 已全选 (has_selection=%d)", vgui_edit_has_selection(st->edit_single));
}

static void on_edit_clear(vgui_widget* w, const vgui_event* e, void* user)
{
	(void)w;
	if (e->type != VGUI_EVENT_CLICK) return;
	demo_state* st = (demo_state*)user;
	vgui_edit_set_text(st->edit_single, "");
	vgui_widget_set_text(st->edit_mirror, "镜像: ");
	statusf(st, "编辑框: 已清空");
}

static void on_edit_append(vgui_widget* w, const vgui_event* e, void* user)
{
	(void)w;
	if (e->type != VGUI_EVENT_CLICK) return;
	demo_state* st = (demo_state*)user;
	std::string s = vgui_edit_get_text(st->edit_single);
	s += " [追加]";
	vgui_edit_set_text(st->edit_single, s.c_str());
	vgui_edit_set_cursor(st->edit_single, (int)s.size());
	vgui_widget_set_text(st->edit_mirror, ("镜像: " + s).c_str());
	statusf(st, "编辑框: 追加文本，光标置于末尾 (cursor=%d)", vgui_edit_get_cursor(st->edit_single));
}

// 当前操作的编辑框：优先使用获得焦点的编辑框，否则用单行编辑框
static vgui_widget* edit_target(demo_state* st)
{
	vgui_widget* f = vgui_get_focus(st->ui);
	if (f && vgui_widget_get_kind(f) == VGUI_EDIT) return f;
	return st->edit_single;
}

static void on_edit_copy(vgui_widget* w, const vgui_event* e, void* user)
{
	(void)w;
	if (e->type != VGUI_EVENT_CLICK) return;
	demo_state* st = (demo_state*)user;
	vgui_widget* t = edit_target(st);
	int ok = vgui_edit_copy(t);
	statusf(st, "复制: %s（目标: %s，选中=%d）", ok ? "成功" : "无选中文本", id_of(t), vgui_edit_has_selection(t));
}

static void on_edit_cut(vgui_widget* w, const vgui_event* e, void* user)
{
	(void)w;
	if (e->type != VGUI_EVENT_CLICK) return;
	demo_state* st = (demo_state*)user;
	vgui_widget* t = edit_target(st);
	int ok = vgui_edit_cut(t);
	statusf(st, "剪切: %s（目标: %s）", ok ? "成功" : "无选中文本", id_of(t));
}

static void on_edit_paste(vgui_widget* w, const vgui_event* e, void* user)
{
	(void)w;
	if (e->type != VGUI_EVENT_CLICK) return;
	demo_state* st = (demo_state*)user;
	vgui_widget* t = edit_target(st);
	int ok = vgui_edit_paste(t);
	statusf(st, "粘贴: %s（目标: %s，长度=%d）", ok ? "成功" : "剪贴板为空或只读",
		id_of(t), (int)strlen(vgui_edit_get_text(t)));
}

static void on_edit_undo(vgui_widget* w, const vgui_event* e, void* user)
{
	(void)w;
	if (e->type != VGUI_EVENT_CLICK) return;
	demo_state* st = (demo_state*)user;
	vgui_widget* t = edit_target(st);
	int ok = vgui_edit_undo(t);
	statusf(st, "撤销: %s（目标: %s，剩余 %s）", ok ? "成功" : "没有可撤销的操作",
		id_of(t), ok ? "可继续撤销" : "无");
}

static void on_edit_redo(vgui_widget* w, const vgui_event* e, void* user)
{
	(void)w;
	if (e->type != VGUI_EVENT_CLICK) return;
	demo_state* st = (demo_state*)user;
	vgui_widget* t = edit_target(st);
	int ok = vgui_edit_redo(t);
	statusf(st, "重做: %s（目标: %s）", ok ? "成功" : "没有可重做的操作", id_of(t));
}

// 循环切换选中高亮的颜色
static const vgui_color edit_sel_colors[4] =
{
	0x80409EFF,	// 蓝
	0x80B060FF,	// 紫
	0x80FF8A3D,	// 橙
	0x8055C86A	// 绿
};
static const char* edit_sel_color_names[4] = { "蓝", "紫", "橙", "绿" };

static void apply_edit_style(demo_state* st)
{
	vgui_widget* list[4] = { st->edit_single, st->edit_password, st->edit_multi, st->edit_readonly };
	vgui_color c = edit_sel_colors[st->sel_color_index & 3];
	for (int i = 0; i < 4; i++)
		if (list[i]) vgui_edit_set_selection_color(list[i], c);
}

static void on_edit_sel_color(vgui_widget* w, const vgui_event* e, void* user)
{
	(void)w;
	if (e->type != VGUI_EVENT_CLICK) return;
	demo_state* st = (demo_state*)user;
	st->sel_color_index = (st->sel_color_index + 1) & 3;
	apply_edit_style(st);
	statusf(st, "选中高亮颜色: %s", edit_sel_color_names[st->sel_color_index]);
}

// 选中圆角滑杆
static void on_sel_rounding(vgui_widget* w, float v, void* user)
{
	(void)w;
	demo_state* st = (demo_state*)user;
	char buf[32] = {};
	snprintf(buf, sizeof(buf), "%.0f px", v);
	if (st->sel_rounding_readout) vgui_widget_set_text(st->sel_rounding_readout, buf);
	vgui_widget* list[4] = { st->edit_single, st->edit_password, st->edit_multi, st->edit_readonly };
	for (int i = 0; i < 4; i++)
		if (list[i]) vgui_edit_set_selection_rounding(list[i], v);
	statusf(st, "选中高亮圆角: %.0f px（0 为方角；拖拽选择 / 双击选词 / 三击选行）", v);
}

// 焦点环透明度（0% 时不渲染焦点）
static void on_focus_alpha(vgui_widget* w, float v, void* user)
{
	(void)w;
	demo_state* st = (demo_state*)user;
	char buf[32] = {};
	snprintf(buf, sizeof(buf), "%.0f%%", v * 100.0f);
	if (st->focus_alpha_readout) vgui_widget_set_text(st->focus_alpha_readout, buf);
	if (st->ui) vgui_context_set_focus_alpha(st->ui, v);
	statusf(st, "焦点环透明度: %.0f%%（0%% 时不渲染焦点）", v * 100.0f);
}

// 动态增删
static void on_dynamic_click(vgui_widget* w, const vgui_event* e, void* user)
{
	if (e->type != VGUI_EVENT_CLICK) return;
	demo_state* st = (demo_state*)user;
	statusf(st, "[%s] 被点击（由 C API 动态创建）", id_of(w));
}

static void on_item_add(vgui_widget* w, const vgui_event* e, void* user)
{
	(void)w;
	if (e->type != VGUI_EVENT_CLICK) return;
	demo_state* st = (demo_state*)user;
	if (vgui_widget_get_child_count(st->bind_box) >= 6)
		vgui_widget_destroy(vgui_widget_get_child(st->bind_box, 0));	// 超过 6 个移除最早的
	char txt[64] = {};
	snprintf(txt, sizeof(txt), "动态按钮 #%d", ++st->added);
	vgui_widget* b = button(st->bind_box, txt, on_dynamic_click, st);
	vgui_widget_set_id(b, txt);
	statusf(st, "新增控件: %s（当前 %d 个）", txt, (int)vgui_widget_get_child_count(st->bind_box));
}

static void on_item_remove(vgui_widget* w, const vgui_event* e, void* user)
{
	(void)w;
	if (e->type != VGUI_EVENT_CLICK) return;
	demo_state* st = (demo_state*)user;
	size_t n = vgui_widget_get_child_count(st->bind_box);
	if (n > 0)
	{
		vgui_widget* first = vgui_widget_get_child(st->bind_box, 0);
		const char* id = id_of(first);
		std::string tmp = id ? id : "";
		vgui_widget_destroy(first);
		statusf(st, "已销毁: %s（剩余 %d 个）", tmp.c_str(), (int)vgui_widget_get_child_count(st->bind_box));
	}
	else
		statusf(st, "没有可销毁的子控件");
}

static void on_item_clear(vgui_widget* w, const vgui_event* e, void* user)
{
	(void)w;
	if (e->type != VGUI_EVENT_CLICK) return;
	demo_state* st = (demo_state*)user;
	vgui_widget_clear(st->bind_box);
	statusf(st, "已清空动态区域");
}

// 滚动面板
static void update_scroll_info(demo_state* st)
{
	float cw = 0, ch = 0, sx = 0, sy = 0;
	vgui_panel_get_content_size(st->scroll_panel, &cw, &ch);
	vgui_panel_get_scroll(st->scroll_panel, &sx, &sy);
	char buf[160] = {};
	snprintf(buf, sizeof(buf), "内容 %.0f x %.0f   偏移 (%.0f, %.0f)", cw, ch, sx, sy);
	vgui_widget_set_text(st->scroll_info, buf);
}

static void on_scroll_top(vgui_widget* w, const vgui_event* e, void* user)
{
	(void)w;
	if (e->type != VGUI_EVENT_CLICK) return;
	demo_state* st = (demo_state*)user;
	vgui_panel_set_scroll(st->scroll_panel, 0, 0);
	update_scroll_info(st);
}

static void on_scroll_bottom(vgui_widget* w, const vgui_event* e, void* user)
{
	(void)w;
	if (e->type != VGUI_EVENT_CLICK) return;
	demo_state* st = (demo_state*)user;
	float cw = 0, ch = 0;
	vgui_panel_get_content_size(st->scroll_panel, &cw, &ch);
	vgui_panel_set_scroll(st->scroll_panel, 0, ch);	// 越界值会在布局时被钳制
	update_scroll_info(st);
}

static void on_scroll_down(vgui_widget* w, const vgui_event* e, void* user)
{
	(void)w;
	if (e->type != VGUI_EVENT_CLICK) return;
	demo_state* st = (demo_state*)user;
	vgui_panel_scroll_by(st->scroll_panel, 0, 80);
	update_scroll_info(st);
}

static void on_scroll_up(vgui_widget* w, const vgui_event* e, void* user)
{
	(void)w;
	if (e->type != VGUI_EVENT_CLICK) return;
	demo_state* st = (demo_state*)user;
	vgui_panel_scroll_by(st->scroll_panel, 0, -80);
	update_scroll_info(st);
}

static void on_scroll_right(vgui_widget* w, const vgui_event* e, void* user)
{
	(void)w;
	if (e->type != VGUI_EVENT_CLICK) return;
	demo_state* st = (demo_state*)user;
	vgui_panel_scroll_by(st->scroll_panel, 80, 0);
	update_scroll_info(st);
}

static void on_scroll_left(vgui_widget* w, const vgui_event* e, void* user)
{
	(void)w;
	if (e->type != VGUI_EVENT_CLICK) return;
	demo_state* st = (demo_state*)user;
	vgui_panel_scroll_by(st->scroll_panel, -80, 0);
	update_scroll_info(st);
}

// 进度条手动增减
static void on_progress_inc(vgui_widget* w, const vgui_event* e, void* user)
{
	(void)w;
	if (e->type != VGUI_EVENT_CLICK) return;
	demo_state* st = (demo_state*)user;
	vgui_widget_set_value(st->progress_manual, vgui_widget_get_value(st->progress_manual) + 10);
	statusf(st, "手动进度 -> %.0f", vgui_widget_get_value(st->progress_manual));
}

static void on_progress_dec(vgui_widget* w, const vgui_event* e, void* user)
{
	(void)w;
	if (e->type != VGUI_EVENT_CLICK) return;
	demo_state* st = (demo_state*)user;
	vgui_widget_set_value(st->progress_manual, vgui_widget_get_value(st->progress_manual) - 10);
	statusf(st, "手动进度 -> %.0f", vgui_widget_get_value(st->progress_manual));
}

// 焦点
static void on_focus_next(vgui_widget* w, const vgui_event* e, void* user)
{
	(void)w;
	if (e->type != VGUI_EVENT_CLICK) return;
	demo_state* st = (demo_state*)user;
	vgui_input_focus_next(st->ui, 0);
	statusf(st, "焦点 -> 下一个（等价 Tab）");
}

static void on_focus_prev(vgui_widget* w, const vgui_event* e, void* user)
{
	(void)w;
	if (e->type != VGUI_EVENT_CLICK) return;
	demo_state* st = (demo_state*)user;
	vgui_input_focus_next(st->ui, 1);
	statusf(st, "焦点 -> 上一个（等价 Shift+Tab）");
}

static void on_focus_edit(vgui_widget* w, const vgui_event* e, void* user)
{
	(void)w;
	if (e->type != VGUI_EVENT_CLICK) return;
	demo_state* st = (demo_state*)user;
	vgui_set_focus(st->ui, st->edit_single);
	statusf(st, "vgui_set_focus -> 单行编辑框");
}

// 主题切换
static void on_theme_dark(vgui_widget* w, const vgui_event* e, void* user)
{
	(void)w;
	if (e->type != VGUI_EVENT_CLICK) return;
	demo_state* st = (demo_state*)user;
	vgui_theme t = theme_dark();
	vgui_context_set_theme(st->ui, &t);
	statusf(st, "主题: 深色");
}

static void on_theme_light(vgui_widget* w, const vgui_event* e, void* user)
{
	(void)w;
	if (e->type != VGUI_EVENT_CLICK) return;
	demo_state* st = (demo_state*)user;
	vgui_theme t = theme_light();
	vgui_context_set_theme(st->ui, &t);
	statusf(st, "主题: 浅色");
}

static void on_theme_green(vgui_widget* w, const vgui_event* e, void* user)
{
	(void)w;
	if (e->type != VGUI_EVENT_CLICK) return;
	demo_state* st = (demo_state*)user;
	vgui_theme t = theme_green();
	vgui_context_set_theme(st->ui, &t);
	statusf(st, "主题: 护眼");
}

//---------------------------------------------------------------------------
// 构建界面
//---------------------------------------------------------------------------

static void build_header(demo_state* st, vgui_widget* root)
{
	vgui_widget* head = row(root, 12);
	vgui_widget_set_id(head, "header");

	vgui_widget* left = col(head, 2);
	vgui_widget* title = label(left, "vgui C API 控件演示", 20, 0);
	vgui_widget_set_id(title, "title");
	vgui_widget* hint = label(left, "Tab 切换焦点 · 滚轮滚动 · 回车/空格 激活 · 单击/双击/三击都有事件", 13, 0xFF9A9A9A);
	vgui_widget_set_id(hint, "hint");

	vgui_widget* right = col(head, 2);
	vgui_widget_set_align_self(right, VGUI_ALIGN_END);
	char ver[64] = {};
	snprintf(ver, sizeof(ver), "版本: %s", vgui_version());
	vgui_widget* vl = label(right, ver, 13, 0xFF9A9A9A);
	vgui_widget_set_text_align(vl, 1.0f, 0.5f);
	vgui_widget_set_min_size(vl, 140, 0);
	st->fps_label = label(right, "fps: -", 13, 0xFF9A9A9A);
	vgui_widget_set_text_align(st->fps_label, 1.0f, 0.5f);
	vgui_widget_set_min_size(st->fps_label, 140, 0);
	vgui_widget_set_max_size(st->fps_label, 300, 0);
	separator(root);
}

static void build_buttons(demo_state* st, vgui_widget* root)
{
	vgui_widget* c = card(root, "按钮 Button");

	vgui_widget* r1 = row(c, 8);
	button(r1, "计数按钮", on_count_click, st);
	vgui_widget* dis = button(r1, "禁用按钮", on_widget_event, st);
	vgui_widget_set_enabled(dis, 0);

	vgui_widget* styled = button(r1, "自定义配色", on_widget_event, st);
	vgui_widget_set_background(styled, 0xFF7A2E2E);
	vgui_widget_set_border(styled, 0xFFE06A6A, 2);
	vgui_widget_set_rounding(styled, 10);
	vgui_widget_set_text_color(styled, 0xFFFFE0E0);

	vgui_widget* half = button(r1, "半透明 opacity=0.5", on_widget_event, st);
	vgui_widget_set_opacity(half, 0.5f);

	vgui_widget* cursor_btn = button(r1, "等待光标", on_widget_event, st);
	vgui_widget_set_cursor(cursor_btn, VGUI_CURSOR_WAIT);

	vgui_widget* r2 = row(c, 8);
	vgui_widget_set_justify(r2, VGUI_JUSTIFY_SPACE_BETWEEN);
	vgui_widget* jl = label(r2, "justify=SPACE_BETWEEN + grow：", 14, 0xFFB0B0B0);
	vgui_widget_set_grow(jl, 1);
	button(r2, "靠右按钮", on_widget_event, st);

	// 绝对定位面板：子控件通过 set_pos 摆放
	vgui_widget* r3 = row(c, 8);
	vgui_widget* abs_panel = vgui_widget_add(r3, VGUI_PANEL, nullptr);
	vgui_widget_set_id(abs_panel, "absolute-panel");
	vgui_widget_set_layout(abs_panel, VGUI_LAYOUT_ABSOLUTE);
	vgui_widget_set_size(abs_panel, 380, 66);
	vgui_widget_set_padding(abs_panel, 6, 6, 6, 6);
	vgui_widget* a1 = button(abs_panel, "绝对定位 (10, 8)", on_widget_event, st);
	vgui_widget_set_pos(a1, 10, 8);
	vgui_widget* a2 = button(abs_panel, "绝对定位 (190, 30)", on_widget_event, st);
	vgui_widget_set_pos(a2, 190, 30);
	label(r3, "← VGUI_LAYOUT_ABSOLUTE + set_pos", 12, 0xFF9A9A9A);
}

static void build_toggles(demo_state* st, vgui_widget* root)
{
	vgui_widget* c = card(root, "选择控件 Checkbox / Radio / Switch");

	vgui_widget* r1 = row(c, 18);
	vgui_widget* chk = vgui_widget_add(r1, VGUI_CHECKBOX, "普通复选框");
	vgui_widget_set_id(chk, "checkbox-normal");
	vgui_widget_on_value(chk, on_value, st);
	vgui_widget_set_checked(chk, 1);

	vgui_widget* chk2 = vgui_widget_add(r1, VGUI_CHECKBOX, "禁用复选框");
	vgui_widget_set_id(chk2, "checkbox-disabled");
	vgui_widget_set_enabled(chk2, 0);

	vgui_widget* sw = vgui_widget_add(r1, VGUI_SWITCH, "开关");
	vgui_widget_set_id(sw, "switch");
	vgui_widget_on_value(sw, on_value, st);
	vgui_widget_set_checked(sw, 1);

	// 单选组：同父节点的单选控件自动互斥
	vgui_widget* r2 = row(c, 10);
	label(r2, "单选组（同父节点自动互斥）:", 14, 0xFFB0B0B0);
	vgui_widget* group = plain(r2, VGUI_LAYOUT_ROW, 14);
	for (int i = 0; i < 3; i++)
	{
		char buf[32] = {};
		snprintf(buf, sizeof(buf), "radio-%s", st->radio_names[i]);
		st->radios[i] = vgui_widget_add(group, VGUI_RADIO, st->radio_names[i]);
		vgui_widget_set_id(st->radios[i], buf);
		vgui_widget_on_value(st->radios[i], on_value, st);
	}
	vgui_widget_set_checked(st->radios[0], 1);
	vgui_widget_set_id(group, "radio-group");

	label(r2, "当前颜色:", 14, 0xFFB0B0B0);
	st->radio_swatch = vgui_widget_add(r2, VGUI_PANEL, nullptr);
	vgui_widget_set_id(st->radio_swatch, "radio-swatch");
	vgui_widget_set_size(st->radio_swatch, 46, 18);
	vgui_widget_set_background(st->radio_swatch, st->radio_colors[0]);
	vgui_widget_set_border(st->radio_swatch, 0xFF000000, 1);
	vgui_widget_set_rounding(st->radio_swatch, 4);
}

static void build_sliders(demo_state* st, vgui_widget* root)
{
	vgui_widget* c = card(root, "滑杆 Slider");

	// 水平滑杆 0~100，步进 1
	vgui_widget* r1 = row(c, 10);
	label(r1, "0 ~ 100, step 1:", 14, 0xFFB0B0B0);
	st->slider_h = vgui_widget_add(r1, VGUI_SLIDER, nullptr);
	vgui_widget_set_id(st->slider_h, "slider-0-100");
	vgui_widget_set_range(st->slider_h, 0, 100);
	vgui_widget_set_step(st->slider_h, 1);
	vgui_widget_set_grow(st->slider_h, 1);
	vgui_widget_on_value(st->slider_h, on_value, st);
	vgui_widget_set_value(st->slider_h, 25);
	st->slider_h_readout = label(r1, "值: 25", 14, 0xFF7FB6FF);
	vgui_widget_set_min_size(st->slider_h_readout, 90, 0);
	vgui_widget_set_text_align(st->slider_h_readout, 1.0f, 0.5f);

	// 连续滑杆 -1~1
	vgui_widget* r2 = row(c, 10);
	label(r2, "-1 ~ 1, 连续:", 14, 0xFFB0B0B0);
	st->slider_c = vgui_widget_add(r2, VGUI_SLIDER, nullptr);
	vgui_widget_set_id(st->slider_c, "slider-1-1");
	vgui_widget_set_range(st->slider_c, -1, 1);
	vgui_widget_set_grow(st->slider_c, 1);
	vgui_widget_on_value(st->slider_c, on_value, st);
	vgui_widget_set_value(st->slider_c, 0);
	st->slider_c_readout = label(r2, "值: 0.00", 14, 0xFF7FB6FF);
	vgui_widget_set_min_size(st->slider_c_readout, 90, 0);
	vgui_widget_set_text_align(st->slider_c_readout, 1.0f, 0.5f);

	// 垂直滑杆
	vgui_widget* r3 = row(c, 12);
	st->slider_v = vgui_widget_add(r3, VGUI_SLIDER, nullptr);
	vgui_widget_set_id(st->slider_v, "slider-vertical");
	vgui_widget_set_vertical(st->slider_v, 1);
	vgui_widget_set_range(st->slider_v, 0, 100);
	vgui_widget_set_size(st->slider_v, 26, 110);
	vgui_widget_on_value(st->slider_v, on_value, st);
	vgui_widget_set_value(st->slider_v, 60);
	vgui_widget* vc = col(r3, 2);
	st->slider_v_readout = label(vc, "值: 60", 14, 0xFF7FB6FF);
	label(vc, "垂直方向: 上下箭头或拖动", 12, 0xFF9A9A9A);
}

static void build_progress(demo_state* st, vgui_widget* root)
{
	vgui_widget* c = card(root, "进度条 Progress");

	vgui_widget* r1 = row(c, 10);
	st->anim_toggle = vgui_widget_add(r1, VGUI_CHECKBOX, "自动动画");
	vgui_widget_set_id(st->anim_toggle, "progress-anim");
	vgui_widget_on_value(st->anim_toggle, on_value, st);
	vgui_widget_set_checked(st->anim_toggle, 1);
	label(r1, "自动:", 14, 0xFFB0B0B0);
	st->progress_auto = vgui_widget_add(r1, VGUI_PROGRESS, nullptr);
	vgui_widget_set_id(st->progress_auto, "progress-auto");
	vgui_widget_set_range(st->progress_auto, 0, 100);
	vgui_widget_set_grow(st->progress_auto, 1);

	vgui_widget* r2 = row(c, 10);
	button(r2, "-10", on_progress_dec, st);
	button(r2, "+10", on_progress_inc, st);
	label(r2, "手动:", 14, 0xFFB0B0B0);
	st->progress_manual = vgui_widget_add(r2, VGUI_PROGRESS, nullptr);
	vgui_widget_set_id(st->progress_manual, "progress-manual");
	vgui_widget_set_range(st->progress_manual, 0, 100);
	vgui_widget_set_grow(st->progress_manual, 1);
	vgui_widget_set_value(st->progress_manual, 40);
}

static void build_edits(demo_state* st, vgui_widget* root)
{
	vgui_widget* c = card(root, "编辑框 Edit");

	// 单行编辑框 + 镜像标签
	vgui_widget* r1 = row(c, 10);
	label(r1, "单行:", 14, 0xFFB0B0B0);
	st->edit_single = vgui_widget_add(r1, VGUI_EDIT, nullptr);
	vgui_widget_set_id(st->edit_single, "edit-single");
	vgui_edit_set_placeholder(st->edit_single, "请输入文本，回车可提交…");
	vgui_edit_set_text(st->edit_single, "你好 vgui，试试拖拽选中");
	vgui_widget_set_grow(st->edit_single, 1);
	vgui_widget_on_event(st->edit_single, on_edit_single_event, st);

	vgui_widget* r2 = row(c, 10);
	label(r2, "密码:", 14, 0xFFB0B0B0);
	st->edit_password = vgui_widget_add(r2, VGUI_EDIT, nullptr);
	vgui_widget_set_id(st->edit_password, "edit-password");
	vgui_edit_set_password(st->edit_password, '*');
	vgui_edit_set_text(st->edit_password, "secret123");
	vgui_widget_set_grow(st->edit_password, 1);
	vgui_widget_on_event(st->edit_password, on_widget_event, st);

	vgui_widget* r3 = row(c, 10);
	label(r3, "多行:", 14, 0xFFB0B0B0);
	st->edit_multi = vgui_widget_add(r3, VGUI_EDIT, nullptr);
	vgui_widget_set_id(st->edit_multi, "edit-multiline");
	vgui_edit_set_multiline(st->edit_multi, 1);
	vgui_edit_set_text(st->edit_multi,
		"第一行：支持多行编辑，滚轮或右侧滚动条可上下滚动\n"
		"第二行：鼠标按住拖拽选中文本\n"
		"第三行：双击选词，三击选整行\n"
		"第四行：Ctrl+A 全选，Ctrl+C/X/V 复制/剪切/粘贴\n"
		"第五行：Ctrl+Z 撤销，Ctrl+Y 重做\n"
		"第六行：选中高亮圆角可调（见下方滑杆）\n"
		"第七行：跨行选中时每个拐角都有圆角\n"
		"第八行：最后一行");
	vgui_widget_set_size(st->edit_multi, 0, 96);
	vgui_widget_set_grow(st->edit_multi, 1);
	vgui_widget_on_event(st->edit_multi, on_widget_event, st);

	vgui_widget* r4 = row(c, 10);
	label(r4, "只读:", 14, 0xFFB0B0B0);
	st->edit_readonly = vgui_widget_add(r4, VGUI_EDIT, nullptr);
	vgui_widget_set_id(st->edit_readonly, "edit-readonly");
	vgui_edit_set_readonly(st->edit_readonly, 1);
	vgui_edit_set_text(st->edit_readonly, "只读文本，可选中/复制，不可编辑（readonly=1）");
	vgui_widget_set_grow(st->edit_readonly, 1);
	vgui_widget_on_event(st->edit_readonly, on_widget_event, st);

	// 选中高亮圆角
	vgui_widget* r5 = row(c, 10);
	label(r5, "选中圆角:", 14, 0xFFB0B0B0);
	st->sel_rounding = vgui_widget_add(r5, VGUI_SLIDER, nullptr);
	vgui_widget_set_id(st->sel_rounding, "edit-sel-rounding");
	vgui_widget_set_range(st->sel_rounding, 0, 10);
	vgui_widget_set_step(st->sel_rounding, 1);
	vgui_widget_set_grow(st->sel_rounding, 1);
	vgui_widget_on_value(st->sel_rounding, on_sel_rounding, st);
	vgui_widget_set_value(st->sel_rounding, 3);
	st->sel_rounding_readout = label(r5, "3 px", 14, 0xFF7FB6FF);
	vgui_widget_set_min_size(st->sel_rounding_readout, 56, 0);
	vgui_widget_set_text_align(st->sel_rounding_readout, 1.0f, 0.5f);

	// 焦点环透明度
	vgui_widget* r5b = row(c, 10);
	label(r5b, "焦点透明度:", 14, 0xFFB0B0B0);
	st->focus_alpha = vgui_widget_add(r5b, VGUI_SLIDER, nullptr);
	vgui_widget_set_id(st->focus_alpha, "edit-focus-alpha");
	vgui_widget_set_range(st->focus_alpha, 0, 1);
	vgui_widget_set_step(st->focus_alpha, 0.05f);
	vgui_widget_set_grow(st->focus_alpha, 1);
	vgui_widget_on_value(st->focus_alpha, on_focus_alpha, st);
	vgui_widget_set_value(st->focus_alpha, 1);
	st->focus_alpha_readout = label(r5b, "100%", 14, 0xFF7FB6FF);
	vgui_widget_set_min_size(st->focus_alpha_readout, 56, 0);
	vgui_widget_set_text_align(st->focus_alpha_readout, 1.0f, 0.5f);

	// 编辑辅助：剪贴板 / 撤销重做 / 选中颜色
	vgui_widget* r6 = row(c, 8);
	button(r6, "全选", on_edit_select_all, st);
	button(r6, "复制", on_edit_copy, st);
	button(r6, "剪切", on_edit_cut, st);
	button(r6, "粘贴", on_edit_paste, st);
	button(r6, "撤销", on_edit_undo, st);
	button(r6, "重做", on_edit_redo, st);
	button(r6, "选中色", on_edit_sel_color, st);
	button(r6, "清空", on_edit_clear, st);
	button(r6, "追加文本", on_edit_append, st);
	label(r6, "← 与 Ctrl+C/X/V/Z/Y 等价", 12, 0xFF9A9A9A);

	st->edit_mirror = label(c, "镜像: 你好 vgui，试试拖拽选中", 13, 0xFF9A9A9A);
	vgui_widget_set_id(st->edit_mirror, "edit-mirror");

	// 初始样式与滑杆/颜色状态保持一致
	on_sel_rounding(st->sel_rounding, 3, st);
	on_focus_alpha(st->focus_alpha, 1, st);
	apply_edit_style(st);
}

static void build_scroll(demo_state* st, vgui_widget* root)
{
	vgui_widget* c = card(root, "滚动面板 Panel + Scroll");

	vgui_widget* r1 = row(c, 8);
	button(r1, "顶部", on_scroll_top, st);
	button(r1, "↑ -80", on_scroll_up, st);
	button(r1, "↓ +80", on_scroll_down, st);
	button(r1, "底部", on_scroll_bottom, st);
	button(r1, "← -80", on_scroll_left, st);
	button(r1, "+80 →", on_scroll_right, st);
	st->scroll_info = label(r1, "内容: -", 13, 0xFF9A9A9A);

	// 可双向滚动的面板：内容超出时自动显示滚动条
	vgui_widget* sp = vgui_widget_add(c, VGUI_PANEL, nullptr);
	vgui_widget_set_id(sp, "scroll-panel");
	vgui_widget_set_layout(sp, VGUI_LAYOUT_COLUMN);
	vgui_widget_set_gap(sp, 4);
	vgui_widget_set_padding(sp, 6, 6, 6, 6);
	vgui_widget_set_size(sp, 0, 170);
	vgui_panel_set_scrollable(sp, 1, 1);
	vgui_widget_set_align_items(sp, VGUI_ALIGN_START);
	st->scroll_panel = sp;

	for (int i = 1; i <= 30; i++)
	{
		if (i % 6 == 0)
		{
			char b[64] = {};
			snprintf(b, sizeof(b), "内嵌按钮 #%d（滚动后仍可点击）", i);
			vgui_widget* btn = button(sp, b, on_widget_event, st);
			vgui_widget_set_id(btn, b);
		}
		else
		{
			char b[160] = {};
			snprintf(b, sizeof(b), "行 %02d —— 内容很长很长很长很长很长很长很长很长，用于演示横向滚动", i);
			vgui_widget* l = label(sp, b, 13, i % 2 ? 0xFFC8C8C8 : 0xFF9A9A9A);
			vgui_widget_set_size(l, 1400, 0);	// 固定宽度，超过面板宽度即可横向滚动
			vgui_widget_set_id(l, "scroll-row");
		}
	}
}

static void build_dynamic(demo_state* st, vgui_widget* root)
{
	vgui_widget* c = card(root, "动态增删 Widget tree");

	vgui_widget* r = row(c, 8);
	button(r, "添加按钮", on_item_add, st);
	button(r, "删除第一个", on_item_remove, st);
	button(r, "清空", on_item_clear, st);
	label(r, "← vgui_widget_add / destroy / clear", 12, 0xFF9A9A9A);

	vgui_widget* box = plain(c, VGUI_LAYOUT_COLUMN, 4);
	vgui_widget_set_id(box, "dynamic-box");
	vgui_widget_set_min_size(box, 0, 28);
	vgui_widget_set_align_items(box, VGUI_ALIGN_START);
	st->bind_box = box;
}

static void build_visibility(demo_state* st, vgui_widget* root)
{
	vgui_widget* c = card(root, "可见性与禁用 Visibility / Enabled");

	vgui_widget* r1 = row(c, 18);
	st->hide_toggle = vgui_widget_add(r1, VGUI_CHECKBOX, "显示附加面板");
	vgui_widget_set_id(st->hide_toggle, "toggle-visible");
	vgui_widget_on_value(st->hide_toggle, on_value, st);
	vgui_widget_set_checked(st->hide_toggle, 1);

	st->enable_toggle = vgui_widget_add(r1, VGUI_CHECKBOX, "启用下方控件组");
	vgui_widget_set_id(st->enable_toggle, "toggle-enabled");
	vgui_widget_on_value(st->enable_toggle, on_value, st);
	vgui_widget_set_checked(st->enable_toggle, 1);

	// 显示 / 隐藏目标
	vgui_widget* hp = row(c, 10);
	vgui_widget_set_id(hp, "hidden-panel");
	st->hidden_panel = hp;
	label(hp, "附加面板:", 14, 0xFF7FB6FF);
	vgui_widget* hb = button(hp, "面板内按钮", on_widget_event, st);
	vgui_widget_set_id(hb, "hidden-button");
	vgui_widget* hs = vgui_widget_add(hp, VGUI_SWITCH, "面板内开关");
	vgui_widget_set_id(hs, "hidden-switch");
	vgui_widget_on_value(hs, on_value, st);
	vgui_widget_set_checked(hs, 1);

	// 启用 / 禁用目标组
	vgui_widget* ep = row(c, 10);
	vgui_widget_set_id(ep, "enable-group");
	vgui_widget* eb = button(ep, "组内按钮", on_widget_event, st);
	vgui_widget_set_id(eb, "enable-button");
	vgui_widget* es = vgui_widget_add(ep, VGUI_SLIDER, nullptr);
	vgui_widget_set_id(es, "enable-slider");
	vgui_widget_set_range(es, 0, 100);
	vgui_widget_set_value(es, 30);
	vgui_widget_set_size(es, 160, 24);
	vgui_widget_on_value(es, on_value, st);
	vgui_widget* ec = vgui_widget_add(ep, VGUI_CHECKBOX, "组内复选框");
	vgui_widget_set_id(ec, "enable-check");

	st->enable_targets[st->enable_target_count++] = eb;
	st->enable_targets[st->enable_target_count++] = es;
	st->enable_targets[st->enable_target_count++] = ec;
}

static void build_focus_theme(demo_state* st, vgui_widget* root)
{
	vgui_widget* c = card(root, "焦点与主题 Focus / Theme");

	vgui_widget* r1 = row(c, 8);
	label(r1, "当前焦点:", 14, 0xFFB0B0B0);
	st->focus_label = label(r1, "-", 14, 0xFF7FB6FF);
	vgui_widget_set_id(st->focus_label, "focus-label");
	vgui_widget_set_min_size(st->focus_label, 180, 24);
	vgui_widget_set_max_size(st->focus_label, 260, 24);

	button(r1, "下一个焦点", on_focus_next, st);
	button(r1, "上一个焦点", on_focus_prev, st);
	button(r1, "聚焦到编辑框", on_focus_edit, st);

	vgui_widget* r2 = row(c, 8);
	label(r2, "主题:", 14, 0xFFB0B0B0);
	button(r2, "深色", on_theme_dark, st);
	button(r2, "浅色", on_theme_light, st);
	button(r2, "护眼", on_theme_green, st);
	label(r2, "← vgui_context_set_theme", 12, 0xFF9A9A9A);
}

static void build_status(demo_state* st, vgui_widget* root)
{
	separator(root);
	vgui_widget* c = row(root, 8);
	vgui_widget_set_id(c, "status-bar");
	label(c, "状态:", 14, 0xFF7FB6FF);
	st->status = label(c, "就绪。点击任意控件查看事件。", 14, 0);
	vgui_widget_set_id(st->status, "status");
	vgui_widget_set_grow(st->status, 1);
}

static void build_ui(demo_state* st)
{
	vgui_context* ui = st->ui;
	vgui_widget* root = vgui_root(ui);
	vgui_widget_set_layout(root, VGUI_LAYOUT_COLUMN);
	vgui_widget_set_padding(root, 14, 14, 14, 14);
	vgui_widget_set_gap(root, 10);
	vgui_widget_set_align_items(root, VGUI_ALIGN_STRETCH);
	vgui_panel_set_scrollable(root, 0, 1);	// 窗口太小时整页可滚动
	vgui_widget_set_id(root, "root");

	// Give the root an opaque background so the window margins follow the
	// theme too.  The color matches the current theme's panel color, so
	// vgui_context_set_theme() updates it on every preset switch.
	vgui_theme theme = {};
	vgui_context_get_theme(ui, &theme);
	vgui_widget_set_background(root, theme.panel_bg);

	build_header(st, root);
	build_buttons(st, root);
	build_toggles(st, root);
	build_sliders(st, root);
	build_progress(st, root);
	build_edits(st, root);
	build_scroll(st, root);
	build_dynamic(st, root);
	build_visibility(st, root);
	build_focus_theme(st, root);
	build_status(st, root);
}

//---------------------------------------------------------------------------
// 每帧更新
//---------------------------------------------------------------------------

static void tick(demo_state* st, float delta)
{
	// 焦点名称
	vgui_widget* f = vgui_get_focus(st->ui);
	const char* fid = f ? id_of(f) : "-";
	if (strcmp(vgui_widget_get_text(st->focus_label), fid) != 0)
		vgui_widget_set_text(st->focus_label, fid);

	// 自动进度
	if (st->anim_on)
	{
		st->anim += st->anim_dir * delta * 0.35f;
		if (st->anim >= 1.0f)
		{
			st->anim = 1.0f;
			st->anim_dir = -1;
		}
		else if (st->anim <= 0.0f)
		{
			st->anim = 0.0f;
			st->anim_dir = 1;
		}
		vgui_widget_set_value(st->progress_auto, st->anim * 100.0f);
	}
}

//---------------------------------------------------------------------------
// main
//---------------------------------------------------------------------------

int main()
{
	// ---- 后端初始化 ----
	auto* wg = new app_mgr();
	font_familys_t* familys = new_font_family(wg->font_ctx, "微软雅黑,Segoe UI,Segoe UI Emoji,Consolas", 0);
	auto* cb = new_ctx_cb();
	rvg_t* vg = cb->new_rvg(cb->ac);

	if (!wg->init_gpu(true))
	{
		fprintf(stderr, "init_gpu 失败\n");
		return -1;
	}

	const int win_w = 1080, win_h = 860;
	os_window* form = wg->create("vgui C API 控件演示", win_w, win_h, 0, true);
	if (!form)
	{
		fprintf(stderr, "创建窗口失败\n");
		return -1;
	}

	// ---- 创建 UI 上下文并挂到窗口 viewport（后端会把输入转发进来）----
	demo_state st;
	st.ui = vgui_create(familys, 16.0f);
	vgui_context_set_size(st.ui, win_w, win_h);
	build_ui(&st);
	form->viewport->ui = st.ui;

	// ---- 主循环 ----
	vg_fbo_t* fbo = &form->fbo;
	FpsCounter fps;
	runtime_cx rtc = {};
	bool running = true;
	float stat_timer = 0.0f;
	int last_w = win_w, last_h = win_h;

	int upms = 0;
	int drawms = 0;
	int submitms = 0;
	while (running)
	{
		float delta = fps.tick();
		if (wg->get_event() < 0)
			running = false;

		if (vgpu_acquire_swapchain(wg->vgctx, fbo, 1, true))
		{
			rtc.begin();
			int cw = (int)fbo->display_size.x;
			int ch = (int)fbo->display_size.y;
			if (cw != last_w || ch != last_h)
			{
				last_w = cw;
				last_h = ch;
				vgui_context_set_size(st.ui, cw, ch);
			}
			if (fbo->upsize)
			{
				fbo->upsize = false;
				vg->width = fbo->display_size.x;
				vg->height = fbo->display_size.y;
			}

			tick(&st, delta);
			vgui_update(st.ui, delta);

			stat_timer += delta;
			if (stat_timer > 0.5f)
			{
				stat_timer = 0.0f;
				char buf[64] = {};
				snprintf(buf, sizeof(buf), "fps: %d\tupdate: %dms\tdraw: %dms\tsubmit: %dms", fps.get_fps(), upms, drawms, submitms);
				vgui_widget_set_text(st.fps_label, buf);
			}
			upms = rtc.end();
			rtc.begin();
			cb->clear(vg);
			cb->reset_clip(vg, 1);
			vgui_draw(st.ui, cb, vg);

			drawms = rtc.end();
			rtc.begin();
			draw_datas_cx drawable;
			drawable.set(fbo);
			drawable.add(vg);
			drawable.render(wg->vgctx);
			submitms = rtc.end();
		}
		//SDL_Delay(1);
	}

	// ---- 清理 ----
	wg->wait_gpu();
	vgui_destroy(st.ui);
	form->viewport->ui = nullptr;	// 已经手动销毁，避免 viewport 析构时重复释放
	delete_font_family(familys);
	cb->destroy_rvg(vg);
	free_ctx_cb(cb);
	delete wg;
	return 0;
}
