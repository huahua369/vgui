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

typedef enum grid_align {
	ALIGN_AUTO = 0,
	ALIGN_STRETCH,
	ALIGN_CENTER,
	ALIGN_START,
	ALIGN_END,
	ALIGN_SPACE_BETWEEN,
	ALIGN_SPACE_AROUND,
	ALIGN_SPACE_EVENLY,
	ALIGN_BASELINE
} grid_align;

/* ---- Grid 容器参数 ---- */
typedef struct grid_data {
	int cols;              /* 列数 */
	int rows;              /* 行数 */

	float* col_widths;     /* 每列宽度（px 或 FR） */
	float* row_heights;    /* 每行高度（px 或 FR） */

	float gap_x;           /* 列间距 */
	float gap_y;           /* 行间距 */

	grid_align justify_items; /* 单元格内水平对齐 */
	grid_align align_items;   /* 单元格内垂直对齐 */

	int auto_flow;         /* 0 = row, 1 = column */
} grid_data;

/* ---- Grid 子元素参数 ---- */
typedef struct grid_child {
	int col;          /* 起始列（0-based） */
	int row;          /* 起始行（0-based） */
	int col_span;     /* 跨列数（默认 1） */
	int row_span;     /* 跨行数（默认 1） */

	grid_align justify_self; /* 覆盖容器对齐 */
	grid_align align_self;
} grid_child;

/* ---- 创建 / 销毁 ---- */
grid_data* grid_create(int cols, int rows);
void       grid_destroy(grid_data* g);

/* ---- 设置轨道尺寸 ---- */
void grid_set_col_widths(grid_data* g, const float* widths);
void grid_set_row_heights(grid_data* g, const float* heights);

/* ---- 设置间距 ---- */
void grid_set_gap(grid_data* g, float gap_x, float gap_y);

/* ---- 布局计算 ---- */
void grid_layout(
	grid_data* g,
	float container_width,
	float container_height,
	node_dt* children,
	size_t count
);



#if 1
#ifndef MALLOC
#define MALLOC(sz)    malloc(sz)
#define FREE(p)       free(p)
#endif
/* ---- 创建 / 销毁 ---- */
grid_data* grid_create(int cols, int rows) {
	if (cols <= 0 || rows <= 0) return NULL;

	grid_data* g = (grid_data*)MALLOC(sizeof(grid_data));
	if (!g) return NULL;

	memset(g, 0, sizeof(grid_data));
	g->cols = cols;
	g->rows = rows;

	g->col_widths = (float*)MALLOC(sizeof(float) * cols);
	g->row_heights = (float*)MALLOC(sizeof(float) * rows);

	if (!g->col_widths || !g->row_heights) {
		FREE(g->col_widths);
		FREE(g->row_heights);
		FREE(g);
		return NULL;
	}

	for (int i = 0; i < cols; i++) g->col_widths[i] = 1.0f;
	for (int i = 0; i < rows; i++) g->row_heights[i] = 1.0f;

	g->justify_items = ALIGN_STRETCH;
	g->align_items = ALIGN_STRETCH;
	g->auto_flow = 0;

	return g;
}

void grid_destroy(grid_data* g) {
	if (!g) return;
	FREE(g->col_widths);
	FREE(g->row_heights);
	FREE(g);
}

/* ---- 设置轨道尺寸 ---- */
void grid_set_col_widths(grid_data* g, const float* widths) {
	if (!g || !widths) return;
	for (int i = 0; i < g->cols; i++)
		g->col_widths[i] = widths[i];
}

void grid_set_row_heights(grid_data* g, const float* heights) {
	if (!g || !heights) return;
	for (int i = 0; i < g->rows; i++)
		g->row_heights[i] = heights[i];
}

/* ---- 设置间距 ---- */
void grid_set_gap(grid_data* g, float gap_x, float gap_y) {
	if (!g) return;
	g->gap_x = gap_x;
	g->gap_y = gap_y;
}

/* ---- 计算轨道尺寸（FR 分配） ---- */
static void grid_calc_tracks(
	grid_data* g,
	float container_width,
	float container_height
) {
	float total_gap_x = g->gap_x * (g->cols - 1);
	float total_gap_y = g->gap_y * (g->rows - 1);

	float avail_w = container_width - total_gap_x;
	float avail_h = container_height - total_gap_y;

	if (avail_w < 0) avail_w = 0;
	if (avail_h < 0) avail_h = 0;

	float total_fr_w = 0.0f;
	float total_fr_h = 0.0f;

	for (int i = 0; i < g->cols; i++)
		if (g->col_widths[i] > 0) total_fr_w += g->col_widths[i];

	for (int i = 0; i < g->rows; i++)
		if (g->row_heights[i] > 0) total_fr_h += g->row_heights[i];

	for (int i = 0; i < g->cols; i++) {
		if (g->col_widths[i] > 0)
			g->col_widths[i] = avail_w * (g->col_widths[i] / total_fr_w);
	}

	for (int i = 0; i < g->rows; i++) {
		if (g->row_heights[i] > 0)
			g->row_heights[i] = avail_h * (g->row_heights[i] / total_fr_h);
	}
}

/* ---- 获取单元格对齐偏移 ---- */
static void grid_align_in_cell(
	grid_data* g,
	grid_child* gc,
	float cell_w,
	float cell_h,
	float item_w,
	float item_h,
	float* ox,
	float* oy
) {
	grid_align j = (gc && gc->justify_self != ALIGN_AUTO)
		? gc->justify_self
		: g->justify_items;

	grid_align a = (gc && gc->align_self != ALIGN_AUTO)
		? gc->align_self
		: g->align_items;

	*ox = 0;
	*oy = 0;

	if (j == ALIGN_CENTER)
		*ox = (cell_w - item_w) * 0.5f;
	else if (j == ALIGN_END)
		*ox = cell_w - item_w;

	if (a == ALIGN_CENTER)
		*oy = (cell_h - item_h) * 0.5f;
	else if (a == ALIGN_END)
		*oy = cell_h - item_h;
}

/* ---- 主布局函数 ---- */
void grid_layout(
	grid_data* g,
	float container_width,
	float container_height,
	node_dt* children,
	size_t count
) {
	if (!g || !children || count == 0) return;

	grid_calc_tracks(g, container_width, container_height);

	int* cell_occupied = (int*)MALLOC(sizeof(int) * g->cols * g->rows);
	memset(cell_occupied, 0, sizeof(int) * g->cols * g->rows);

	for (size_t i = 0; i < count; i++) {
		grid_child* gc = NULL;
		if (children[i].user)
			gc = (grid_child*)children[i].user;

		int col = 0, row = 0;
		int cspan = 1, rspan = 1;

		if (gc) {
			col = gc->col;
			row = gc->row;
			cspan = gc->col_span > 0 ? gc->col_span : 1;
			rspan = gc->row_span > 0 ? gc->row_span : 1;
		}
		else {
			/* 自动流布局 */
			if (g->auto_flow == 0) {
				col = (int)(i % g->cols);
				row = (int)(i / g->cols);
			}
			else {
				row = (int)(i % g->rows);
				col = (int)(i / g->rows);
			}
		}

		/* 边界保护 */
		if (col < 0) col = 0;
		if (row < 0) row = 0;
		if (col + cspan > g->cols) cspan = g->cols - col;
		if (row + rspan > g->rows) rspan = g->rows - row;

		/* 计算单元格位置 */
		float x = 0, y = 0;
		for (int c = 0; c < col; c++)
			x += g->col_widths[c] + g->gap_x;

		for (int r = 0; r < row; r++)
			y += g->row_heights[r] + g->gap_y;

		float cell_w = 0, cell_h = 0;
		for (int c = 0; c < cspan; c++)
			cell_w += g->col_widths[col + c] + (c ? g->gap_x : 0);

		for (int r = 0; r < rspan; r++)
			cell_h += g->row_heights[row + r] + (r ? g->gap_y : 0);

		/* 子元素尺寸 */
		float item_w = children[i].size.x;
		float item_h = children[i].size.y;

		float ox = 0, oy = 0;
		grid_align_in_cell(g, gc, cell_w, cell_h, item_w, item_h, &ox, &oy);

		children[i].frame.x = x + ox;
		children[i].frame.y = y + oy;
		children[i].frame.z = item_w;
		children[i].frame.w = item_h;
	}

	FREE(cell_occupied);
}
#endif // 1



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
	LoadLibraryA(R"(E:\Program Files\RenderDoc_1.37_64\renderdoc.dll)");
	cout << "Hello ovg." << endl;
	glm::ivec2 surfsize = { 1024,800 };
	font_cache_cx* font_ctx = new_font_cache();
	font_familys_t* familys = new_font_family(font_ctx, (char*)u8"微软雅黑,Segoe UI Emoji,Consolas,Times New Roman,Tahoma,Calibri,Noto Serif Devanagari", 0);

	auto cb = new_ctx_cb();
	auto vg = cb->new_rvg(cb->ac);
	uint32_t f = SDL_INIT_AUDIO | SDL_INIT_VIDEO | SDL_INIT_EVENTS;
#ifdef __ANDROID__
	f |= SDL_INIT_HAPTIC;
#endif
	int kr = SDL_Init(f);
	auto wg = new app_mgr();
	if (!wg->init_gpu(true))return -1;
	//auto vp = new gui_viewport();
	auto form1 = wg->create("SDL3 GPU Vector Graphics", surfsize.x, surfsize.y, 0);
	//form1->viewport = vp;
	//vp->set_viewport({ 0,0,surfsize.x, surfsize.y });
	//auto div0 = new div_cx0({ 100,100,50,50 });
	//vp->add_div(div0);
	//if (!vg_sdl3_init(g, surfsize.x, surfsize.y, true)) {
	//	SDL_Log("Init failed: %s", SDL_GetError());
	//	return 1;
	//}
	auto dev = new_sdl3gpu_device(wg->device);
	assert(dev);
	auto format = SDL_GetGPUSwapchainTextureFormat(wg->device, form1->window);
	ovg_ctx_t* ctx = new_ovgctx_sdl3(dev, format ? format : SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT, SDL_GPU_SAMPLECOUNT_4);
	assert(ctx);

	vg_fbo_t fbo = new_vgfbo_sdl3(ctx, surfsize.x, surfsize.y, form1->window);
	bool running = true;
	runtime_cx rtc = {};
	FrameProfiler fps;
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
	gradient_btn_init(&btn1, 0x5ffc6122, -1);
	btn.rounding = 4;
	btn1.rounding = 4;
	gradient_btn_update(&btn1, 0);
	while (running) {
		fps.beginFrame();
		if (wg->get_event() < 0)
		{
			running = false;
		}
		if (ovg_get_window_swapchain(ctx, &fbo))
		{
			rtc.begin();
			cb->clear(vg);
			cb->set_fill_rule(vg, VG_FILL_RULE_NON_ZERO);
			glm::vec2 sf = fbo.display_size;
			draw_grid_fill(vg, sf, glm::ivec2(-1, 0xffdfdfdf), 20);
			cb->reset_clip(vg, 1);

			vg->width = fbo.display_size.x; vg->height = fbo.display_size.y;

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

			text4.pos = { 10.0f, 200.0f };
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
			rimg.dst = { 108,108,img->width * 2.8,img->height * 1.5 };
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
			gradient_btn_draw(cb, vg, &btn1, { 400,420 }, { 200,30 });
			//timeline_draw_system(tl, tk, tkcount, cb, vg, familys);
			vgms = rtc.end();
			//if (ms > 0)
			//	printf("draw build ms: %d\n", ms);
			ovg_draw_data_t dlist[] = { get_draw_list(vg)/*, get_draw_list(rwvg)*/ };
			rtc.begin();
			ovg_render_frame(ctx, &fbo, dlist, sizeof(dlist) / sizeof(ovg_draw_data_t));// 提交渲染 
			fms = rtc.end();
			//if (ms > 0)
			//	printf("submit draw ms: %d\n", ms);
		}
		SDL_Delay(16);  /* ~60 FPS */
		fps.endFrame();
		if (++scount > 20) {
			scount = 0;
			showstr = fps.c_str();
			showstr += " vgms: " + std::to_string(vgms);
			showstr += " vg_render: " + std::to_string(fms);
		}
	}

	SDL_WaitForGPUIdle(wg->device);
	/* Cleanup */

	free_vgfbo_sdl3(&fbo);
	free_ovgctx_sdl3(ctx);
	free_sdl3gpu_device(dev);

	delete_font_family(familys);
	free_font_cache(font_ctx);
	// 删除vg对象
	cb->destroy_rvg(vg);
	if (cb)free_ctx_cb(cb);
	delete wg;
	SDL_Quit();

	return 0;
}
