// ovg.cpp: 定义应用程序的入口点。
//

#include "ovg_main.h" 
#include "ovg_renderer_sdl3.h"
#include <Windows.h>
#include <cmath>
#include <unordered_map>

#ifndef fseeki64
#ifdef _WIN32
#define fseeki64 _fseeki64
#define ftelli64 _ftelli64
#else			
#define fseeki64 fseeko64
#define ftelli64 ftello64
#endif // _WIN32
#endif

using namespace std;
#include "ovg_fonts.h"
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#include <vgui_sdl3.h>
#define TECS_IMPLEMENTATION
#include <tiny_ecs.h>
#include <entt/entt.hpp>
#if defined(__cpp_pp_embed) && __cpp_pp_embed >= 202502L
const unsigned char image_data[] = {
	#embed "button.png"
};
#endif

#include "ovg_scene_json.h"



typedef struct node_dt node_dt;





rvg_t* test_vgrw(ovg_ctx_cb* ovg)
{
	/* ========= 2. 构建场景 ========= */
	ovg::Scene scene;
	scene.name = "demo_scene";
	scene.width = 800.0f; scene.height = 600.0f;


	/* ========= 3. 保存为 JSON ========= */
	std::string jsonPath = "res/example.ovgs";
	//if (!ovg::JsonIO::save(jsonPath, scene)) {
	//	std::cerr << "Failed to save scene\n";
	//}
	//else {
	//	std::cout << "Scene saved to " << jsonPath << "\n";
	//}
	/* ========= 4. 从 JSON 加载 ========= */
	ovg::Scene loadedScene;
	if (!ovg::JsonIO::load(jsonPath, loadedScene)) {
		std::cerr << "Failed to load scene\n";
		return 0;
	}
	std::cout << "Loaded scene: " << loadedScene.name
		<< " (" << loadedScene.width << "x"
		<< loadedScene.height << ")\n";

	/* ========= 5. 渲染 ========= */
	rvg_t* rvg = ovg->new_rvg(ovg->ac);
	if (!rvg) {
		std::cerr << "Failed to create rvg\n";
		//free_ctx_cb(ovg);
		return 0;
	}
	ovg->clear(rvg); ovg->reset_clip(rvg, 1);
	ovg::SceneRenderer renderer(ovg);
	renderer.render(rvg, loadedScene);

	/* 这里你可以： */
	/* - 提交 draw list */
	/* - 或继续录制/编辑 */

	ovg_draw_data_t drawData = get_draw_list(rvg);
	std::cout << "Draw commands: " << drawData.count << "\n";
	std::cout << "VG vertices:   " << drawData.v_count << "\n";

	/* 清理 */
	//ovg->destroy_rvg(rvg);
	return rvg;
}

int main()
{
	//LoadLibraryA(R"(E:\Program Files\RenderDoc_1.37_64\renderdoc.dll)"); 
	glm::ivec2 surfsize = { 1024,800 };
	auto wg = new app_mgr();
	font_cache_cx* font_ctx = wg->font_ctx;
	font_familys_t* familys = new_font_family(font_ctx, (char*)u8"微软雅黑,Segoe UI Emoji,Consolas,Times New Roman,Tahoma,Calibri,Noto Serif Devanagari", 0);

	auto cb = new_ctx_cb();
	auto vg = cb->new_rvg(cb->ac);
	auto vg1 = cb->new_rvg(cb->ac);

	if (!wg->init_gpu(true))return -1;
	auto form1 = wg->create("SDL3GPU render", surfsize.x, surfsize.y, 0, true);

	draw_datas_cx drawable;
	vg_fbo_t* fbo = &form1->fbo;
	bool running = true;
	runtime_cx rtc = {};
	runtime_cx rtc0 = {};
	FpsCounter fps;
	// 渲染 
	auto fp = fopen("E:\\1.txt", "r");
	std::string buff;
	if (fp) {
		fseeki64(fp, 0L, SEEK_END);
		auto size = ftelli64(fp);
		fseeki64(fp, 0L, SEEK_SET);
		buff.resize(size);
		auto retval = fread(buff.data(), size, 1, fp);
		assert(retval == 1);
		fclose(fp);
	}
	bool testvg = 0;
	ovg_image_data img[1] = {};
	int channels = 0;
	img->data = (uint32_t*)stbi_load("res/button.png", &img->width, &img->height, &channels, 4);
	img->valid = true;
	auto rwvg = test_vgrw(cb);
	std::string showstr;
	int vgms = 0, fms = 0;
	int scount = 0;
	color_style btn = {};
	gradient_style btn1 = {};
	set_btn_color_idx(&btn, 0);//0-7
	update_color_btn(&btn, 0);

	uint32_t colors[6] = { 0x905050fc,0x9050fc50,0x90fc5050,0x90ffffff,0x90282828,0x90f82828 };
	//x=默认，y=鼠标进入，z=按下
	glm::uvec3 gradTop = { 0xff4a4a4a,0x80404040,0xff292929 }, gradBot = { 0xff3a3a3a,0x80303030,0xff1d1d1d };
	glm::uvec3 gradTop1 = { 0xff8a8a8a,0x80bebebe,0xff303030 }, gradBot1 = { 0xff5a5a5a,0x80303030,0xff1d1d1d };
	glm::uvec2 blackb = { 0x805c5c5c , 0x801d1d1d };

	gradient_btn_init(&btn1, 0x80ff8222, -1);
	{
		auto p = &btn1;
		p->back_color = colors[2];
		p->borderLight = blackb.x;
		p->borderDark = blackb.y;
		p->gradTop = gradTop;
		p->gradBot = gradBot;
		p->gradTop = gradTop1;
		p->gradBot = gradBot1;
	}
	btn.rounding = 4;
	btn1.rounding = 4;
	gradient_btn_update(&btn1, 0);
	node_dt nodes[6] = { };
	{
		grid_data* g = grid_create(3, 2);
		float cw[] = { 1, 1, 1 };
		float rh[] = { 40, 40 };
		grid_set_col_widths(g, cw, 3);
		grid_set_row_heights(g, rh, 2);
		grid_set_gap(g, 8, 8);
		for (int i = 0; i < 6; i++) {
			nodes[i].size.x = 160;
			nodes[i].size.y = 38;
		}
		grid_layout(g, 600, 100, nodes, 6);
		grid_destroy(g);
	}
	switch_info_t sw[1] = {};
	sw->dcol = sw->color.x;
	sw->cpos = 1.0;
	double js = 0.0;
	bool is_draw = true;
	while (running) {
		//fps.beginFrame();
		auto delta = fps.tick();
		js += delta;
		rtc0.begin();
		if (wg->get_event() < 0)
		{
			running = false;
		}
		do {
			if (!is_draw)break;
			if (ovg_get_window_swapchain(wg->vgctx, fbo, 1, true))
			{
				rtc.begin();
				cb->clear(vg);
				cb->set_fill_rule(vg, VG_FILL_RULE_NON_ZERO);

				glm::vec2 sf = fbo->display_size;
				if (fbo->upsize)
				{
					fbo->upsize = false;
					vg->width = fbo->display_size.x;
					vg->height = fbo->display_size.y;
					vg1->width = fbo->display_size.x;
					vg1->height = fbo->display_size.y;
					cb->clear(vg1);
					rtc.begin();
					cb->set_enable_tess2(vg1, true);
					draw_grid_fill(vg1, sf, glm::ivec2(-1, 0xffdfdfdf), 20);
					auto ms = rtc.end();
					printf("draw_grid_fill ms: %d enable_tess2\n", ms);
					rtc.begin();
					cb->set_enable_tess2(vg1, false);
					draw_grid_fill(vg1, sf, glm::ivec2(-1, 0xffdfdfdf), 20);
					ms = rtc.end();
					printf("draw_grid_fill ms: %d\n", ms);
				}
				cb->reset_clip(vg, 1);
				//draw_test3d(&fbo, cb, vg);
				text_style_t style4 = {};
				style4.family = familys;
				style4.fontsize = 18;
				style4.color = 0xff0080f0;
				style4.color_stroke = 0xFF0000f0;
				style4.min_subpixel = 0;
				//style4.stroke = 1;
				//style4.color_shadow = 0xa6000000;
				style4.shadow_pos = { 2.0f, 2.0f };

				text_st_t text4 = {};
				text4.text = (char*)u8"🍕➗☂️-abgyh彩色渐变字体";
				//text4.text = (char*)buff.c_str();
				text4.text_len = -1;

				text4.pos = { 10.0f, 500.0f };
				cb->move_to(vg, 0, text4.pos.y);
				cb->rel_line_to(vg, 1800, 0);
				cb->set_source_color(vg, 0xff00ff00);
				cb->set_line_width(vg, 2);
				cb->stroke(vg);
				cb->add_text(vg, &text4, &style4, nullptr);

				style4.min_subpixel = 0;
				text4.text = (char*)u8"-+abg➗🍕☂️灰度+彩色渐变字体\n右起";
				//style4.stroke = -1;
				text4.pos = { 10.0f, 120 + 200.0f };

				cb->move_to(vg, 0, text4.pos.y + 0.5);
				cb->rel_line_to(vg, 1800, 0);
				cb->set_source_color(vg, 0xff00ff00);
				cb->set_line_width(vg, 1);
				cb->stroke(vg);
				cb->rectangle(vg, 0, text4.pos.y - 20, 200, 200);
				cb->set_source_color(vg, 0xff000000);
				//cb->set_source_color(vg, -1);
				cb->fill(vg);
				cb->add_text(vg, &text4, &style4, nullptr);
				text4.text = showstr.c_str();// (char*)u8"./+*@#!@#$%^&*()_+[];'/.,";
				text4.pos.y += 260;
				cb->add_text(vg, &text4, &style4, nullptr);
				ovg_image_r rimg = {};
				rimg.img = img;
				rimg.dst = { 108,508,img->width * 2.8,img->height * 1.5 };
				rimg.rc = { 0,0,img->width,img->height };
				rimg.sliced = { 4,4,4,4 };
				rimg.color = -1;
				cb->add_image(vg, &rimg);
				if (img->valid)
				{
					vg_image_desc_t desc = {};
					desc.width = img->width;
					desc.height = img->height;
					desc.format = VG_FORMAT_RGBA8;
					desc.stride = desc.width * sizeof(int);
					desc.pixels = img->data;
					desc.x = 0, desc.y = 0, desc.w = img->width, desc.h = img->height;		// 更新矩形区域
					desc.is_copy = true;
					img->valid = false;
					cb->image_update(vg, img, &desc);
				}
				draw_color_btn(cb, vg, &btn, { 400,380 }, { 200,30 });
				cb->translate(vg, 10, 10);
				for (int i = 0; i < 6; i++) {
					auto it = &nodes[i];
					btn1.back_color = colors[i];
					gradient_btn_draw(cb, vg, &btn1, { it->frame.x,it->frame.y }, { it->frame.z,it->frame.w });
				}
				cb->translate(vg, 0, 100);
				btn1.cubic_gradient = false;
				for (int i = 0; i < 6; i++) {
					auto it = &nodes[i];
					btn1.back_color = colors[i];
					if (i < 5)
						gradient_btn_draw(cb, vg, &btn1, { it->frame.x,it->frame.y }, { it->frame.z,it->frame.w });
					else
						switch_draw(cb, vg, sw, { it->frame.x,it->frame.y }, { it->frame.z,it->frame.w });
				}
				//timeline_draw_system(tl, tk, tkcount, cb, vg, familys);
				vgms = rtc.end();
				//if (ms > 0)
				//	printf("draw build ms: %d\n", ms);
				drawable.clear();
				drawable.set(fbo);
				drawable.add(vg1);
				drawable.add(vg);

				rtc.begin();
				drawable.render(wg->vgctx);// 提交渲染 
				fms = rtc.end();
				//if (ms > 0)
				//	printf("submit draw ms: %d\n", ms);
			}
		} while (0);
		int ams = rtc0.end();
		//SDL_Delay(32);
		//fps.endFrame();
		if (js > 0.5) {
			js = 0.0;
			scount = 0;
			showstr = "fps: "; showstr += fps.c_str();
			showstr += "\ncpu ms: " + std::to_string(ams);
			showstr += "\nvgms: " + std::to_string(vgms);
			showstr += " vg_render: " + std::to_string(fms);
			int rfps = (fms + vgms);
			rfps = (rfps > 0) ? 1000.0 / rfps : 0;
			showstr += "\nfps render: " + std::to_string(rfps);
		}
	}
	wg->wait_gpu();


	delete_font_family(familys);
	// 删除vg对象
	cb->destroy_rvg(vg);
	cb->destroy_rvg(vg1);
	if (cb)free_ctx_cb(cb);
	delete wg;

	return 0;
}
