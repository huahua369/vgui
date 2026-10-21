// sample/vgui_edit_test.cpp: 编辑框行为自测（无需窗口/GPU）
//
// 通过公开 C API 直接驱动输入事件，验证：
//   * 鼠标拖拽选择、双击选词、三击选行
//   * 复制/剪切/粘贴（含单行编辑框粘贴多行文本的净化）
//   * 撤销/重做（含快捷键 Ctrl+Z / Ctrl+Y / Ctrl+Insert 等）
//   * 多行编辑框滚轮滚动 / 内部滚动条拖动、只读编辑框限制
//   * 面板滚动条拖动、焦点环透明度 API
//
// 运行方式：直接执行 vgui_edit_test.exe，全部 PASS 时返回 0。

#include <cstdio>
#include <cstring>
#include <string>

#include "pch.h"		// glm 等基础类型（ovg_c.h 依赖）
#include <ovg_c.h>		// font_familys_t
#include <vgui_sdl3.h>	// app_mgr / new_font_family / set_clipboard / get_clipboard
#include <vgui.h>		// 公共 C API

static int g_fail = 0;

// Windows 剪贴板会把 LF 规范化为 CRLF，比较前统一去掉 '\r'
static std::string norm(const std::string& s)
{
	std::string o;
	o.reserve(s.size());
	for (char c : s)
		if (c != '\r') o.push_back(c);
	return o;
}

// SDL 在 Windows 上使用延迟渲染，需要消息循环处理 WM_RENDERFORMAT。
// 自测程序没有主循环，这里手动 pump 一下，避免偶发读到空剪贴板。
static std::string clip()
{
	for (int i = 0; i < 10; i++)
	{
		SDL_PumpEvents();
		std::string s = get_clipboard();
		if (!s.empty()) return s;
		SDL_Delay(2);
	}
	return {};
}

static void seed_clip(const char* s)
{
	set_clipboard(s);
	SDL_PumpEvents();
}

// 用“剪切 + 撤销”取出当前选中的文本：不依赖系统剪贴板的延迟渲染。
// 剪切前记下光标位置：正向选择时光标在选区末尾，反向选择时光标在开头，
// 再结合文本长度差即可精确定位被剪掉的区间（重复文本也不会误判）。
static std::string cut_sel(vgui_widget* w)
{
	int cur = vgui_edit_get_cursor(w);
	std::string before = vgui_edit_get_text(w);
	if (!vgui_edit_cut(w)) return {};
	std::string after = vgui_edit_get_text(w);
	if (before.size() <= after.size())
	{
		vgui_edit_undo(w);
		return {};
	}
	size_t L = before.size() - after.size();
	size_t c = (size_t)cur;
	if (c >= L && before.compare(c, std::string::npos, after, c - L, std::string::npos) == 0)
	{
		std::string removed = before.substr(c - L, L);
		vgui_edit_undo(w);
		return removed;
	}
	if (before.compare(c + L, std::string::npos, after, c, std::string::npos) == 0)
	{
		std::string removed = before.substr(c, L);
		vgui_edit_undo(w);
		return removed;
	}
	vgui_edit_undo(w);
	return {};
}

static void check(bool ok, const char* what)
{
	printf("%s  %s\n", ok ? "[PASS]" : "[FAIL]", what);
	if (!ok) g_fail++;
}

static void check_str(const char* got, const char* want, const char* what)
{
	bool ok = (got && want && strcmp(got, want) == 0);
	printf("%s  %s\n", ok ? "[PASS]" : "[FAIL]", what);
	if (!ok)
	{
		printf("        got : \"%s\"\n", got ? got : "(null)");
		printf("        want: \"%s\"\n", want ? want : "(null)");
		g_fail++;
	}
}

static void check_clip(const char* want, const char* what)
{
	check_str(norm(clip()).c_str(), want, what);
}

// 键盘辅助
static void key(vgui_context* ui, int k, int mods = 0)
{
	vgui_input_key(ui, k, 1, mods);
	vgui_input_key(ui, k, 0, mods);
}

int main()
{
	setvbuf(stdout, nullptr, _IONBF, 0);
	auto* wg = new app_mgr();
	font_familys_t* familys = new_font_family(wg->font_ctx, "Consolas,微软雅黑", 0);
	vgui_context* ui = vgui_create(familys, 16.0f);
	vgui_context_set_size(ui, 900, 700);
	vgui_widget_set_layout(vgui_root(ui), VGUI_LAYOUT_COLUMN);

	// 多行编辑框（等宽字体，便于按字符定位）
	vgui_widget* ed = vgui_widget_add(vgui_root(ui), VGUI_EDIT, nullptr);
	vgui_widget_set_id(ed, "test-multi");
	vgui_edit_set_multiline(ed, 1);
	vgui_widget_set_size(ed, 420, 92);
	vgui_edit_set_text(ed, "hello world\nsecond line\nthird line");

	// 单行编辑框（粘贴净化/只读）
	vgui_widget* ed1 = vgui_widget_add(vgui_root(ui), VGUI_EDIT, nullptr);
	vgui_widget_set_id(ed1, "test-single");
	vgui_widget_set_size(ed1, 420, 0);

	// 只读编辑框
	vgui_widget* ro = vgui_widget_add(vgui_root(ui), VGUI_EDIT, nullptr);
	vgui_widget_set_id(ro, "test-readonly");
	vgui_edit_set_readonly(ro, 1);
	vgui_edit_set_text(ro, "readonly");

	vgui_update(ui, 0.016f);
	vgui_set_focus(ui, ed);		// 键盘快捷键测试需要编辑框获得焦点

	float x = 0, y = 0, w = 0, h = 0;
	vgui_widget_get_rect(ed, &x, &y, &w, &h);
	printf("多行编辑框 rect = (%.0f, %.0f, %.0f, %.0f)\n", x, y, w, h);
	check(w > 100 && h > 40, "布局：多行编辑框已测量");

	//---- 选择 / 剪贴板 ----
	vgui_edit_set_text(ed, "hello world\nsecond line\nthird line");
	vgui_update(ui, 0.016f);
	check(vgui_edit_has_selection(ed) == 0, "初始无选中");
	check(vgui_edit_copy(ed) == 0, "无选中时复制返回 0");

	vgui_edit_select(ed, 0, 5);
	check(vgui_edit_has_selection(ed) == 1, "vgui_edit_select 生效");
	check(vgui_edit_copy(ed) == 1, "有选中时复制返回 1");
	check_clip("hello", "复制内容 = hello");

	// 快捷键：Ctrl+A 全选 / Ctrl+C 复制
	key(ui, 'A', VGUI_MOD_CTRL);
	check(vgui_edit_has_selection(ed) == 1, "Ctrl+A 全选");
	key(ui, 'C', VGUI_MOD_CTRL);
	check_clip("hello world\nsecond line\nthird line", "Ctrl+C 复制全部");

	// Ctrl+X 剪切 + Ctrl+Z 撤销 + Ctrl+Y 重做
	key(ui, 'X', VGUI_MOD_CTRL);
	check_str(vgui_edit_get_text(ed), "", "Ctrl+X 剪切全部");
	key(ui, 'Z', VGUI_MOD_CTRL);
	check_str(vgui_edit_get_text(ed), "hello world\nsecond line\nthird line", "Ctrl+Z 撤销剪切");
	key(ui, 'Y', VGUI_MOD_CTRL);
	check_str(vgui_edit_get_text(ed), "", "Ctrl+Y 重做剪切");
	key(ui, 'Z', VGUI_MOD_CTRL);
	check_str(vgui_edit_get_text(ed), "hello world\nsecond line\nthird line", "再次 Ctrl+Z 恢复");

	// Ctrl+V 粘贴（全选后整体替换）
	vgui_edit_select_all(ed);
	key(ui, 'V', VGUI_MOD_CTRL);
	check_str(vgui_edit_get_text(ed), "hello world\nsecond line\nthird line",
		"Ctrl+V 粘贴剪贴板内容（覆盖选中）");

	// 撤销粘贴恢复原文（回归：撤销后不能因遗留滚动而看不见文本）
	vgui_edit_set_text(ed, "orig");
	vgui_update(ui, 0.016f);
	vgui_edit_select_all(ed);
	seed_clip("0123456789abcdefghijklmnopqrstuvwxyz-0123456789abcdefghijklmnopqrstuvwxyz");
	key(ui, 'V', VGUI_MOD_CTRL);
	key(ui, 'Z', VGUI_MOD_CTRL);
	check_str(vgui_edit_get_text(ed), "orig", "Ctrl+Z 撤销粘贴恢复原文");

	// Windows 习惯快捷键：Ctrl+Insert / Shift+Insert / Shift+Delete
	vgui_edit_set_text(ed, "abc def");
	vgui_update(ui, 0.016f);
	vgui_edit_select(ed, 0, 3);
	key(ui, VGUI_KEY_INSERT, VGUI_MOD_CTRL);
	check_clip("abc", "Ctrl+Insert 复制");
	vgui_edit_set_cursor(ed, 7);
	key(ui, VGUI_KEY_INSERT, VGUI_MOD_SHIFT);
	check_str(vgui_edit_get_text(ed), "abc defabc", "Shift+Insert 粘贴");
	vgui_edit_select(ed, 0, 3);
	key(ui, VGUI_KEY_DELETE, VGUI_MOD_SHIFT);
	check_str(vgui_edit_get_text(ed), " defabc", "Shift+Delete 剪切");
	check_clip("abc", "Shift+Delete 写入剪贴板");

	// 撤销/重做栈状态
	vgui_edit_set_text(ed, "abc");
	vgui_update(ui, 0.016f);
	check(vgui_edit_can_undo(ed) == 0, "set_text 后历史被清空");
	vgui_input_text(ui, "d");
	check_str(vgui_edit_get_text(ed), "abcd", "输入文本");
	check(vgui_edit_can_undo(ed) == 1, "输入后可撤销");
	vgui_edit_undo(ed);
	check(vgui_edit_get_text(ed) == std::string("abc"), "undo 恢复输入前");
	check(vgui_edit_can_redo(ed) == 1, "undo 后可重做");
	vgui_edit_redo(ed);
	check_str(vgui_edit_get_text(ed), "abcd", "redo 恢复输入");

	// 单行编辑框粘贴净化：换行/制表符 -> 空格
	seed_clip("a\nb\tc");
	vgui_edit_set_text(ed1, "");
	vgui_edit_paste(ed1);
	check_str(vgui_edit_get_text(ed1), "a b c", "单行编辑框粘贴净化换行");
	check(vgui_edit_get_text(ed1) != std::string("a\nb\tc"), "单行编辑框不含换行");

	// 只读编辑框
	seed_clip("zzz");
	vgui_set_focus(ui, ro);
	vgui_edit_select(ro, 0, 8);
	check(vgui_edit_cut(ro) == 0, "只读编辑框不可剪切");
	check(vgui_edit_paste(ro) == 0, "只读编辑框不可粘贴");
	vgui_edit_set_cursor(ro, 8);
	vgui_input_text(ui, "q");
	check_str(vgui_edit_get_text(ro), "readonly", "只读编辑框不接受输入");

	//---- 鼠标：拖拽 / 双击 / 三击 ----
	vgui_edit_set_text(ed, "hello world\nsecond line\nthird line");
	vgui_update(ui, 0.016f);
	vgui_widget_get_rect(ed, &x, &y, &w, &h);
	float x0 = x + 2.0f;					// 最左
	float x1 = x + w - 2.0f;				// 最右

	// 点击每一行（三击）应选中对应整行，同时得到各行的点击 y
	const char* line_text[3] = { "hello world\n", "second line\n", "third line" };
	float ly[3] = { -1, -1, -1 };
	for (float yy = y + 1.0f; yy < y + h - 1.0f; yy += 1.0f)
	{
		vgui_input_mouse_down(ui, 1, x0, yy, 3);
		vgui_input_mouse_up(ui, 1, x0, yy, 3);
		std::string c = cut_sel(ed);
		for (int i = 0; i < 3; i++)
			if (ly[i] < 0 && c == line_text[i]) ly[i] = yy;
	}
	check(ly[0] > 0 && ly[1] > ly[0] && ly[2] > ly[1], "三击逐行选中（行映射正确）");
	printf("        行点击 y: %.0f / %.0f / %.0f（编辑框 y=%.0f h=%.0f）\n", ly[0], ly[1], ly[2], y, h);
	// 扫描结束时停留在最后一行，这里再点一次第二行
	vgui_input_mouse_down(ui, 1, x0, ly[1], 3);
	vgui_input_mouse_up(ui, 1, x0, ly[1], 3);
	check_str(cut_sel(ed).c_str(), "second line\n", "三击选中第二行");

	// 双击选词（行首 = 第一个词）
	vgui_input_mouse_down(ui, 1, x0, ly[0], 2);
	vgui_input_mouse_up(ui, 1, x0, ly[0], 2);
	check_str(cut_sel(ed).c_str(), "hello", "双击行首选词");

	// 双击选词（行尾 = 最后一个词）
	vgui_input_mouse_down(ui, 1, x1, ly[2], 2);
	vgui_input_mouse_up(ui, 1, x1, ly[2], 2);
	check_str(cut_sel(ed).c_str(), "line", "双击行尾选词");

	// 拖拽选择：行首 -> 第 2 行行尾
	vgui_input_mouse_down(ui, 1, x0, ly[0], 1);
	vgui_input_mouse_move(ui, x1, ly[1]);
	vgui_input_mouse_move(ui, x1 + 50, ly[1]);
	vgui_input_mouse_up(ui, 1, x1 + 50, ly[1], 1);
	check_str(cut_sel(ed).c_str(), "hello world\nsecond line", "拖拽选择（向下跨行）");

	// 拖拽选择：文本结尾 -> 行首（反向）
	vgui_edit_set_text(ed, "hello world\nsecond line\nthird line");
	vgui_update(ui, 0.016f);
	vgui_input_mouse_down(ui, 1, x1, ly[2], 1);
	vgui_input_mouse_move(ui, x0, ly[0]);
	vgui_input_mouse_up(ui, 1, x0, ly[0], 1);
	check_str(cut_sel(ed).c_str(), "hello world\nsecond line\nthird line", "拖拽选择（反向全选）");

	// 拖拽中把光标拖出控件范围（越界保护）
	vgui_input_mouse_down(ui, 1, x0, ly[0], 1);
	vgui_input_mouse_move(ui, x1 + 400, ly[2] + 200);
	vgui_input_mouse_move(ui, x - 300, y - 300);
	vgui_input_mouse_up(ui, 1, x0, ly[0], 1);
	check(vgui_edit_get_cursor(ed) >= 0, "越界拖拽不崩溃");

	//---- 多行滚轮滚动 ----
	{
		std::string lines;
		for (int i = 0; i < 16; i++)
		{
			lines += "line";
			lines += (char)('0' + i / 10);
			lines += (char)('0' + i % 10);
			if (i < 15) lines += "\n";
		}
		// 失焦状态下 set_text 会回到文本开头
		vgui_set_focus(ui, ed1);
		vgui_edit_set_text(ed, lines.c_str());
		vgui_update(ui, 0.016f);
		vgui_widget_get_rect(ed, &x, &y, &w, &h);
		float top_y = y + 2.0f;

		// 未滚动时点击顶部 = line00
		vgui_input_mouse_down(ui, 1, x0, top_y, 3);
		vgui_input_mouse_up(ui, 1, x0, top_y, 3);
		check_str(cut_sel(ed).c_str(), "line00\n", "滚轮前：顶部行为 line00");

		// 滚动后点击顶部应命中后面的行
		for (int i = 0; i < 3; i++)
			vgui_input_mouse_wheel(ui, 0, -1);
		vgui_widget_get_rect(ed, &x, &y, &w, &h);
		vgui_input_mouse_down(ui, 1, x0, top_y, 3);
		vgui_input_mouse_up(ui, 1, x0, top_y, 3);
		std::string top = cut_sel(ed);
		check(top.size() >= 6 && top.compare(0, 4, "line") == 0 && top != "line00\n",
			"滚轮后：顶部行已滚动");
		printf("        滚轮后顶部行 = \"%s\"\n", top.c_str());
	}

	//---- 多行编辑框滚动条拖动 ----
	{
		std::string lines;
		for (int i = 0; i < 16; i++)
		{
			lines += "line";
			lines += (char)('0' + i / 10);
			lines += (char)('0' + i % 10);
			if (i < 15) lines += "\n";
		}
		// 失焦状态下 set_text 会回到文本开头
		vgui_set_focus(ui, ed1);
		vgui_edit_set_text(ed, lines.c_str());
		vgui_update(ui, 0.016f);
		vgui_widget_get_rect(ed, &x, &y, &w, &h);
		float top_y = y + 2.0f;
		vgui_input_mouse_down(ui, 1, x0, top_y, 1);
		vgui_input_mouse_up(ui, 1, x0, top_y, 1);
		check(vgui_edit_get_cursor(ed) == 0, "编辑框滚动条：拖动前顶部为第 0 字符");

		// 编辑框右侧内部滚动条：从上往下拖到底部
		float bx = x + w - 4.0f;
		vgui_input_mouse_move(ui, bx, y + 10.0f);
		check(vgui_context_get_cursor(ui) == VGUI_CURSOR_ARROW,
			"编辑框滚动条悬停时鼠标为普通箭头");
		vgui_input_mouse_down(ui, 1, bx, y + 10.0f, 1);
		check(vgui_context_get_cursor(ui) == VGUI_CURSOR_ARROW,
			"按下编辑框滚动条时鼠标保持普通箭头");
		vgui_input_mouse_move(ui, bx, y + h - 10.0f);
		check(vgui_context_get_cursor(ui) == VGUI_CURSOR_ARROW,
			"拖动编辑框滚动条时鼠标保持普通箭头");
		vgui_input_mouse_up(ui, 1, bx, y + h - 10.0f, 1);

		vgui_input_mouse_down(ui, 1, x0, top_y, 1);
		vgui_input_mouse_up(ui, 1, x0, top_y, 1);
		int cur = vgui_edit_get_cursor(ed);
		check(cur > 0, "编辑框滚动条拖动后顶部行已滚动");
		printf("        拖动后顶部光标 = %d\n", cur);

		vgui_input_mouse_move(ui, x0, top_y);
		check(vgui_context_get_cursor(ui) == VGUI_CURSOR_IBEAM,
			"编辑框文本区域鼠标仍为 I 形");
	}

	//---- multi-line edit horizontal scrollbar ----
	{
		std::string lines;
		std::string wide = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
		for (int i = 0; i < 16; i++)
		{
			lines += wide;
			if (i < 15) lines += "\n";
		}
		vgui_set_focus(ui, ed1);
		vgui_edit_set_text(ed, lines.c_str());
		vgui_update(ui, 0.016f);
		vgui_widget_get_rect(ed, &x, &y, &w, &h);
		float x_left = x + 2.0f;
		float top_y = y + 2.0f;
		vgui_input_mouse_down(ui, 1, x_left, top_y, 1);
		vgui_input_mouse_up(ui, 1, x_left, top_y, 1);
		check(vgui_edit_get_cursor(ed) == 0, "horizontal scroll: initial cursor at 0");

		float bar_y = y + h - 4.0f;
		vgui_input_mouse_move(ui, x + 40.0f, bar_y);
		check(vgui_context_get_cursor(ui) == VGUI_CURSOR_ARROW,
			"horizontal scrollbar hover uses arrow cursor");
		vgui_input_mouse_down(ui, 1, x + 40.0f, bar_y, 1);
		vgui_input_mouse_move(ui, x + w - 30.0f, bar_y);
		vgui_input_mouse_up(ui, 1, x + w - 30.0f, bar_y, 1);

		vgui_input_mouse_down(ui, 1, x_left, top_y, 1);
		vgui_input_mouse_up(ui, 1, x_left, top_y, 1);
		int cur = vgui_edit_get_cursor(ed);
		check(cur > 0, "horizontal scrollbar drag scrolls content");
		printf("        horizontal drag cursor = %d\n", cur);
	}

	//---- grow edit must not widen its row for long text ----
	{
		vgui_context* ui3 = vgui_create(familys, 16.0f);
		vgui_context_set_size(ui3, 500, 300);
		vgui_widget_set_layout(vgui_root(ui3), VGUI_LAYOUT_COLUMN);
		vgui_widget_set_padding(vgui_root(ui3), 10, 10, 10, 10);

		vgui_widget* row3 = vgui_widget_add(vgui_root(ui3), VGUI_PANEL, nullptr);
		vgui_widget_set_layout(row3, VGUI_LAYOUT_ROW);
		vgui_widget_set_size(row3, 400, 96);
		vgui_widget_set_gap(row3, 8);
		vgui_widget_add(row3, VGUI_LABEL, "Long:");

		vgui_widget* wide = vgui_widget_add(row3, VGUI_EDIT, nullptr);
		vgui_edit_set_multiline(wide, 1);
		vgui_widget_set_size(wide, 0, 80);
		vgui_widget_set_grow(wide, 1);
		std::string long_line(300, 'a');
		vgui_edit_set_text(wide, long_line.c_str());
		vgui_update(ui3, 0.016f);

		float rx = 0, ry = 0, rw = 0, rh = 0;
		float ex = 0, ey = 0, ew = 0, eh = 0;
		vgui_widget_get_rect(row3, &rx, &ry, &rw, &rh);
		vgui_widget_get_rect(wide, &ex, &ey, &ew, &eh);
		check(ew <= rw + 0.5f, "grow multiline edit shrinks to its row width");
		check(ew > 0.0f, "grow multiline edit keeps a usable width");
		printf("        row width = %.0f, edit width = %.0f\n", rw, ew);

		// With the row width fixed, focusing and setting the long line must
		// scroll horizontally instead of widening the edit.  A click near the
		// left edge then maps to a non-zero byte.
		vgui_set_focus(ui3, wide);
		vgui_edit_set_text(wide, long_line.c_str());
		vgui_update(ui3, 0.016f);
		vgui_widget_get_rect(wide, &ex, &ey, &ew, &eh);
		vgui_input_mouse_down(ui3, 1, ex + 2.0f, ey + 2.0f, 1);
		vgui_input_mouse_up(ui3, 1, ex + 2.0f, ey + 2.0f, 1);
		check(vgui_edit_get_cursor(wide) > 0,
			"grow multiline edit scrolls instead of widening for long text");

		vgui_destroy(ui3);
	}

	//---- 面板滚动条拖动 ----
	{
		vgui_context* ui2 = vgui_create(familys, 16.0f);
		vgui_context_set_size(ui2, 400, 300);
		vgui_widget* p = vgui_widget_add(vgui_root(ui2), VGUI_PANEL, nullptr);
		vgui_widget_set_pos(p, 20, 20);
		vgui_widget_set_size(p, 300, 160);
		vgui_panel_set_scrollable(p, 1, 1);
		vgui_widget* big = vgui_widget_add(p, VGUI_PANEL, nullptr);
		vgui_widget_set_pos(big, 0, 0);
		vgui_widget_set_size(big, 600, 600);
		vgui_update(ui2, 0.016f);

		float px = 0, py = 0, pw = 0, ph = 0, sx = 0, sy = 0;
		vgui_widget_get_rect(p, &px, &py, &pw, &ph);
		vgui_panel_get_scroll(p, &sx, &sy);
		check(sx == 0 && sy == 0, "面板滚动条：初始 scroll = 0");

		// 切换主题只影响颜色，不应改变内容尺寸（滚动条长度由内容尺寸决定）。
		float cw0 = 0, ch0 = 0, cw1 = 0, ch1 = 0;
		vgui_panel_get_content_size(p, &cw0, &ch0);
		vgui_theme light = {};
		vgui_context_get_theme(ui2, &light);
		light.track = 0xFFDCDCDC;
		light.scrollbar = 0xFF9A9A9A;
		vgui_context_set_theme(ui2, &light);
		vgui_update(ui2, 0.016f);
		vgui_panel_get_content_size(p, &cw1, &ch1);
		check(cw0 == cw1 && ch0 == ch1, "主题切换不改变滚动内容尺寸（长度计算独立于颜色）");

		// 垂直：按住滑块向下拖到底
		float bx = px + pw - 4.0f;
		vgui_input_mouse_down(ui2, 1, bx, py + 10.0f, 1);
		vgui_input_mouse_move(ui2, bx, py + ph - 10.0f);
		vgui_input_mouse_up(ui2, 1, bx, py + ph - 10.0f, 1);
		vgui_panel_get_scroll(p, &sx, &sy);
		check(sy > 100.0f, "垂直滚动条拖动改变 scroll_y");

		// 点击滑块外的轨道：直接跳转（不需要拖动）
		vgui_panel_set_scroll(p, 0, 0);
		vgui_update(ui2, 0.016f);
		vgui_input_mouse_down(ui2, 1, bx, py + ph - 20.0f, 1);
		vgui_input_mouse_up(ui2, 1, bx, py + ph - 20.0f, 1);
		vgui_panel_get_scroll(p, &sx, &sy);
		check(sy > 100.0f, "点击滚动条轨道跳转");

		// 水平：按住滑块向右拖
		vgui_panel_set_scroll(p, 0, 0);
		vgui_update(ui2, 0.016f);
		float by = py + ph - 4.0f;
		vgui_input_mouse_down(ui2, 1, px + 20.0f, by, 1);
		vgui_input_mouse_move(ui2, px + 260.0f, by);
		vgui_input_mouse_up(ui2, 1, px + 260.0f, by, 1);
		vgui_panel_get_scroll(p, &sx, &sy);
		check(sx > 100.0f, "水平滚动条拖动改变 scroll_x");
		printf("        拖动后 scroll = %.0f / %.0f\n", sx, sy);
		vgui_destroy(ui2);
	}

	//---- 焦点透明度 API ----
	vgui_context_set_focus_alpha(ui, 0.0f);
	check(vgui_context_get_focus_alpha(ui) == 0.0f, "上下文焦点透明度 = 0（不渲染焦点）");
	check(vgui_widget_get_focus_alpha(ed) == 0.0f, "控件继承上下文焦点透明度");
	vgui_widget_set_focus_alpha(ed, 0.5f);
	check(vgui_widget_get_focus_alpha(ed) == 0.5f, "控件焦点透明度覆盖生效");
	vgui_widget_set_focus_alpha(ed, -1.0f);
	check(vgui_widget_get_focus_alpha(ed) == 0.0f, "负值恢复继承");
	vgui_context_set_focus_alpha(ui, 1.0f);
	check(vgui_widget_get_focus_alpha(ed) == 1.0f, "上下文焦点透明度恢复 1");

	//---- 选中样式 API ----
	vgui_edit_set_selection_rounding(ed, 0.0f);
	vgui_edit_set_selection_rounding(ed, 12.0f);
	vgui_edit_set_selection_color(ed, 0x80409EFFu);
	vgui_edit_set_selection_color(ed, 0);
	vgui_edit_set_caret_width(ed, 2.0f);
	vgui_edit_set_caret_width(ed, 1.0f);
	check(true, "选中圆角/颜色与光标宽度 API 可调用");
	check(vgui_widget_get_kind(ed) == VGUI_EDIT, "vgui_widget_get_kind");

	// 密码框（双击选词同样可用）
	vgui_widget* pw = vgui_widget_add(vgui_root(ui), VGUI_EDIT, nullptr);
	vgui_edit_set_password(pw, '*');
	vgui_edit_set_text(pw, "secret");
	vgui_update(ui, 0.016f);
	vgui_edit_select_all(pw);
	check(vgui_edit_copy(pw) == 1, "密码框可选中复制");

	//---- 样式 API / 主题新增颜色 ----
	{
		vgui_style def = {};
		vgui_style_default(&def);
		check(def.panel.padding == 8.0f && def.button.rounding == 4.0f &&
			def.edit.scrollbar_width == 4.0f && def.edit.caret_width == 1.0f,
			"vgui_style_default 填充当前默认值");

		vgui_style before = {};
		vgui_context_get_style(ui, &before);
		check(before.panel.scrollbar_width == 5.0f && before.edit.scrollbar_width == 4.0f,
			"vgui_context_get_style 返回上下文样式");

		// 已存在的编辑框仍使用旧默认宽度时，改样式应迁移其请求尺寸。
		vgui_widget* se = vgui_widget_add(vgui_root(ui), VGUI_EDIT, nullptr);
		vgui_update(ui, 0.016f);
		float sx0 = 0, sy0 = 0, sw0 = 0, sh0 = 0;
		vgui_widget_get_rect(se, &sx0, &sy0, &sw0, &sh0);
		check(sw0 == def.edit.req_width, "新建编辑框使用 style.edit.req_width");

		vgui_style wide = before;
		wide.edit.req_width = 233.0f;
		wide.button.min_height = 41.0f;
		vgui_context_set_style(ui, &wide);
		vgui_update(ui, 0.016f);
		float sx1 = 0, sy1 = 0, sw1 = 0, sh1 = 0;
		vgui_widget_get_rect(se, &sx1, &sy1, &sw1, &sh1);
		check(sw1 == 233.0f, "vgui_context_set_style 迁移已有控件的默认尺寸");

		vgui_widget* sb = vgui_widget_add(vgui_root(ui), VGUI_BUTTON, "style");
		vgui_update(ui, 0.016f);
		float bx = 0, by = 0, bw = 0, bh = 0;
		vgui_widget_get_rect(sb, &bx, &by, &bw, &bh);
		check(bh >= 41.0f, "按钮最小高度来自 style");

		vgui_style got = {};
		vgui_context_get_style(ui, &got);
		check(got.button.min_height == 41.0f && got.edit.req_width == 233.0f,
			"vgui_context_get_style 可读回修改");
		vgui_context_set_style(ui, &def);
		vgui_update(ui, 0.016f);

		vgui_theme th = {};
		vgui_context_get_theme(ui, &th);
		check(th.scrollbar_hover != 0 && th.checkmark != 0 && th.switch_thumb != 0,
			"默认主题包含滚动条悬停/勾选/开关颜色");
		th.scrollbar_hover = 0;
		th.checkmark = 0;
		th.switch_thumb = 0;
		vgui_theme fallback_scroll = th;
		vgui_context_set_theme(ui, &th);
		vgui_context_get_theme(ui, &th);
		check(th.scrollbar_hover == fallback_scroll.scrollbar,
			"旧主题缺少 scrollbar_hover 时回退到 scrollbar 色");
		check(th.checkmark == 0xFFFFFFFFu && th.switch_thumb == 0xFFFFFFFFu,
			"旧主题缺少勾选/开关颜色时回退到白色");
	}

	printf("\n%s（%d 个失败）\n", g_fail ? "存在失败项" : "全部通过", g_fail);

	//---- dash state save/restore regression ----
	// A saved state owns its dash array. Drawing a focused edit leaves a dash
	// pattern on the context, and the edit's save/restore path must not free
	// the source state's dash array (which used to double-free here).
	{
		ovg_ctx_cb* cb = new_ctx_cb();
		rvg_t* vg = cb ? cb->new_rvg(cb->ac) : nullptr;
		vgui_context* dui = vgui_create(familys, 16.0f);
		vgui_widget* da = nullptr;
		vgui_widget* db = nullptr;
		if (dui)
		{
			vgui_context_set_size(dui, 280, 140);
			vgui_widget_set_layout(vgui_root(dui), VGUI_LAYOUT_COLUMN);
			da = vgui_widget_add(vgui_root(dui), VGUI_EDIT, nullptr);
			db = vgui_widget_add(vgui_root(dui), VGUI_EDIT, nullptr);
			vgui_edit_set_text(da, "alpha");
			vgui_edit_set_text(db, "beta");
			vgui_widget_set_size(da, 220, 30);
			vgui_widget_set_size(db, 220, 30);
			vgui_update(dui, 0.016f);
		}
		check(cb && vg && dui, "dash state: create rendering context");
		if (cb && vg && dui)
		{
			float dashes[] = { 4.0f, 4.0f };
			cb->set_dash(vg, dashes, 2, 0.0f);
			for (int i = 0; i < 16; i++)
			{
				cb->save(vg);
				cb->restore(vg);
			}
			printf("dash: save/restore ok\n");

			for (int i = 0; i < 6; i++)
			{
				printf("dash: draw %d focus\n", i);
				vgui_set_focus(dui, (i & 1) ? db : da);
				vgui_update(dui, 0.016f);
				printf("dash: draw %d render\n", i);
				cb->clear(vg);
				cb->reset_clip(vg, 1);
				vgui_draw(dui, cb, vg);
				printf("dash: draw %d done\n", i);
			}
			check(true, "dash state: repeated focus draw/save/restore");
		}
		printf("dash: destroy ui\n");
		vgui_destroy(dui);
		printf("dash: destroy rvg\n");
		if (vg) cb->destroy_rvg(vg);
		printf("dash: free cb\n");
		if (cb) free_ctx_cb(cb);
		printf("dash: cleanup done\n");
	}

	vgui_destroy(ui);
	delete_font_family(familys);
	delete wg;
	return g_fail ? 1 : 0;
}
