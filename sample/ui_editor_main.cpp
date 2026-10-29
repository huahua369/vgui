// sample/ui_editor_main.cpp: vgui UI 编辑器宿主程序。
//
//   vgui_editor            打开编辑器窗口（SDL3 后端）
//   vgui_editor --selftest 无窗口自测：JSON 往返 + 代码生成 + 无 GPU 构建
//
// 编辑器本身（文档模型 / JSON / 代码生成 / 界面）位于 gui/ui_editor.{h,cpp}，
// 全部基于 gui/vgui.h 的公开 C API 实现。

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>

#include "pch.h"          // glm 等基础类型（ovg_c.h 依赖）
#include <vgui/backend/vgui_sdl3.h>    // app_mgr / gui_viewport / draw_datas_cx
#include <vgui/vgui.h>         // 公共 C API
#include "ovg_main.h"     // runtime_cx / FpsCounter
#include "ui_editor.h"

//---------------------------------------------------------------------------
// 无窗口自测
//---------------------------------------------------------------------------

static int selftest_fail(const char* message)
{
	fprintf(stderr, "FAIL: %s\n", message);
	return 1;
}

static int run_selftest()
{
	using namespace vgui_editor;

	ui_document doc;
	doc.root.children.clear();
	auto* label = doc.root.add_child(VGUI_LABEL);
	label->id = "hello";
	label->text = "Hello";
	label->x = 12.0f;
	label->y = 34.0f;
	auto* button = doc.root.add_child(VGUI_BUTTON);
	button->id = "ok";
	button->text = "OK";
	button->bg = 0xFF336699;

	std::string text = doc.to_json_string(2);
	if (text.find("\"format\": \"vgui-ui-editor\"") == std::string::npos)
		return selftest_fail("JSON 缺少 format 字段");

	ui_document copy;
	std::string error;
	if (!copy.from_json_string(text, &error))
		return selftest_fail(error.c_str());

	if (copy.root.children.size() != doc.root.children.size())
		return selftest_fail("JSON 往返后子节点数量不一致");
	if (copy.root.children[0]->id != "hello" || copy.root.children[0]->text != "Hello")
		return selftest_fail("JSON 往返后 label 内容不一致");
	if (copy.root.children[0]->x != 12.0f || copy.root.children[1]->bg != 0xFF336699)
		return selftest_fail("JSON 往返后属性不一致");
	if (copy.root.children[1]->parent != &copy.root)
		return selftest_fail("JSON 往返后 parent 指针未修复");

	std::string code = ui_generate_cpp(copy);
	if (code.find("#include \"vgui.h\"") == std::string::npos)
		return selftest_fail("生成代码缺少 vgui.h");
	if (code.find("vgui_widget_add(root, VGUI_LABEL") == std::string::npos)
		return selftest_fail("生成代码缺少 label 创建");
	if (code.find("vgui_widget_set_background") == std::string::npos)
		return selftest_fail("生成代码缺少颜色设置");

	/* 无字体、无 GPU 环境下也能构建编辑器界面。  The context holder is
	 * declared before the editor so the editor (and its PropertyGrid) is
	 * destroyed first, even on the early-return paths below. */
	std::unique_ptr<vgui_context, decltype(&vgui_destroy)> ctx_holder(
		vgui_create(nullptr, 16.0f), &vgui_destroy);
	vgui_context* ctx = ctx_holder.get();
	if (!ctx) return selftest_fail("vgui_create 失败");
	vgui_context_set_size(ctx, 1400, 900);

	ui_editor editor;
	editor.build(ctx);
	vgui_update(ctx, 0.016f);
	vgui_layout(ctx);

	if (vgui_widget_get_child_count(vgui_root(ctx)) < 4)
		return selftest_fail("编辑器界面未创建");
	if (editor.selected() == nullptr)
		return selftest_fail("编辑器默认未选中根节点");
	if (editor.property_count() == 0)
		return selftest_fail("根节点属性未显示");

	editor.add_widget(VGUI_SLIDER);
	if (editor.selected() == nullptr || editor.selected()->kind != VGUI_SLIDER)
		return selftest_fail("add_widget 失败");
	if (editor.property_count() == 0)
		return selftest_fail("滑杆属性未显示");

	const std::string tmp_json = "ui_editor_selftest.json";
	const std::string tmp_cpp = "ui_editor_selftest.cpp";

	error.clear();
	if (!editor.save_json(tmp_json, &error) || !editor.load_json(tmp_json, &error))
		return selftest_fail(error.c_str());
	if (!editor.export_cpp(tmp_cpp, &error))
		return selftest_fail(error.c_str());

	std::error_code ec;
	std::filesystem::remove(tmp_json, ec);
	std::filesystem::remove(tmp_cpp, ec);

	printf("default json: %s\n", default_json_path().c_str());
	printf("default cpp : %s\n", default_cpp_path().c_str());
	printf("vgui_editor selftest PASS\n");
	return 0;
}

//---------------------------------------------------------------------------
// 窗口模式
//---------------------------------------------------------------------------

int main(int argc, char** argv)
{
	for (int i = 1; i < argc; ++i)
	{
		if (strcmp(argv[i], "--selftest") == 0)
			return run_selftest();
		if (strcmp(argv[i], "--dump-code") == 0)
		{
			vgui_editor::ui_document doc;
			std::string code = vgui_editor::ui_generate_cpp(doc);
			fwrite(code.data(), 1, code.size(), stdout);
			return 0;
		}
	}

	auto* wg = new app_mgr();
	font_familys_t* familys = new_font_family(wg->font_ctx, "微软雅黑,Consolas,Segoe UI Emoji", 0);
	auto* cb = new_ctx_cb();
	rvg_t* vg = cb->new_rvg(cb->ac);

	if (!wg->init_gpu(true))
	{
		fprintf(stderr, "init_gpu 失败\n");
		return -1;
	}

	const int win_w = 1400, win_h = 900;
	os_window* form = wg->create("vgui UI 编辑器", win_w, win_h, 0, true);
	if (!form)
	{
		fprintf(stderr, "创建窗口失败\n");
		return -1;
	}

	vgui_context* ui = vgui_create(familys, 16.0f);
	vgui_context_set_size(ui, win_w, win_h);

	vgui_editor::ui_editor* editor = new vgui_editor::ui_editor();
	editor->build(ui);
	form->viewport->ui = ui;

	vg_fbo_t* fbo = &form->fbo;
	FpsCounter fps;
	runtime_cx rtc = {};
	bool running = true;
	int last_w = win_w, last_h = win_h;

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
				vgui_context_set_size(ui, cw, ch);
			}
			if (fbo->upsize)
			{
				fbo->upsize = false;
				vg->width = fbo->display_size.x;
				vg->height = fbo->display_size.y;
			}

			editor->tick();
			vgui_update(ui, delta);

			rtc.end();
			rtc.begin();
			cb->clear(vg);
			cb->reset_clip(vg, 1);
			vgui_draw(ui, cb, vg);

			rtc.end();
			rtc.begin();
			draw_datas_cx drawable;
			drawable.set(fbo);
			drawable.add(vg);
			drawable.render(wg->vgctx);
			rtc.end();
		}
	}

	wg->wait_gpu();
	delete editor;
	form->viewport->ui = nullptr;
	vgui_destroy(ui);
	delete_font_family(familys);
	cb->destroy_rvg(vg);
	free_ctx_cb(cb);
	delete wg;
	return 0;
}
