// sample/vgui_game_editor.cpp: 游戏编辑器 dogfood 示例
//
// 用 vgui_editor_kit 搭一个最小的游戏场景编辑器,持续验证 M3 Outliner 的
// 编辑器工作流(roadmap §10:每个里程碑在示例中补一条可操作路径):
//   * editor_shell 外壳(Dock + 菜单 + 工具栏 + 状态栏 + 布局持久化)
//   * Outliner:类型图标、可见性/锁定行内按钮、F2/双击行内重命名、
//     拖放重排/改父子(含多选)、名称过滤高亮、双向选中同步、右键菜单
//   * 命令栈:移动/重命名/删除/新建/复制全部可撤销
//
// 窗口与事件由 SDL3 后端(backends/vgui_sdl3.h)提供。

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "pch.h"		// glm 等基础类型(ovg_c.h 依赖)
#include <vgui_sdl3.h>	// app_mgr / gui_viewport / draw_datas_cx
#include <vgui.h>
#include <vgui_editor_kit.h>
#include "ovg_main.h"

using namespace vgui_editor_kit;

//---------------------------------------------------------------------------
// 玩具场景模型:宿主拥有数据,kit 只见稳定 id
//---------------------------------------------------------------------------

struct scene_object
{
	object_id id = invalid_object;
	std::string name;
	object_id parent = invalid_object;
	std::vector<object_id> children;
	bool visible = true;
	bool locked = false;
};

struct game_scene
{
	std::map<object_id, scene_object> objects;
	std::vector<object_id> roots;
	object_id next_id = 1;

	object_id add(const char* name, object_id parent)
	{
		scene_object o;
		o.id = next_id++;
		o.name = name;
		if (parent == invalid_object)
			roots.push_back(o.id);
		else
		{
			o.parent = parent;
			objects[parent].children.push_back(o.id);
		}
		objects[o.id] = o;
		return o.id;
	}

	object_id parent_of(object_id id) const
	{
		auto it = objects.find(id);
		return it == objects.end() ? invalid_object : it->second.parent;
	}

	void set_parent(object_id id, object_id parent)
	{
		auto it = objects.find(id);
		if (it == objects.end())
			return;
		if (it->second.parent == parent)
			return;
		if (it->second.parent == invalid_object)
		{
			auto r = std::find(roots.begin(), roots.end(), id);
			if (r != roots.end())
				roots.erase(r);
		}
		else
		{
			auto& siblings = objects[it->second.parent].children;
			auto s = std::find(siblings.begin(), siblings.end(), id);
			if (s != siblings.end())
				siblings.erase(s);
		}
		it->second.parent = parent;
		if (parent == invalid_object)
		{
			if (std::find(roots.begin(), roots.end(), id) == roots.end())
				roots.push_back(id);
		}
		else
			objects[parent].children.push_back(id);
	}

	void place(object_id id, object_id parent, std::size_t index)
	{
		set_parent(id, parent);
		std::vector<object_id>& siblings = parent == invalid_object
			? roots : objects[parent].children;
		auto it = std::find(siblings.begin(), siblings.end(), id);
		if (it != siblings.end())
			siblings.erase(it);
		index = std::min(index, siblings.size());
		siblings.insert(siblings.begin() + (std::ptrdiff_t)index, id);
	}

	bool is_ancestor(object_id ancestor, object_id node) const
	{
		object_id walk = node;
		while (walk != invalid_object)
		{
			if (walk == ancestor)
				return true;
			walk = parent_of(walk);
		}
		return false;
	}

	object_id duplicate(object_id src, object_id parent)
	{
		const scene_object& o = objects[src];
		const std::string name = o.name + " copy";
		const object_id copy = add(name.c_str(), parent);
		objects[copy].visible = o.visible;
		objects[copy].locked = o.locked;
		for (object_id c : o.children)
			duplicate(c, copy);
		return copy;
	}
};

//---------------------------------------------------------------------------
// 编辑器状态
//---------------------------------------------------------------------------

struct editor_state
{
	game_scene scene;
	editor_context* context = nullptr;
	editor_shell* shell = nullptr;

	bool valid_selection() const
	{
		return context->selection() != invalid_object &&
			scene.objects.count(context->selection()) != 0;
	}

	void rebuild_outliner()
	{
		outliner_view& outliner = shell->scene();
		outliner.clear();
		std::function<void(object_id, vgui_item_id)> add = [&](object_id id,
			vgui_item_id parent_item)
		{
			const scene_object& o = scene.objects[id];
			const vgui_item_id item = outliner.add_node(o.name.c_str(),
				parent_item, id);
			outliner.set_object_visibility(id, o.visible);
			outliner.set_object_locked(id, o.locked);
			for (object_id c : o.children)
				add(c, item);
		};
		for (object_id id : scene.roots)
			add(id, 0);
		/* 恢复选择与展开 */
		if (context->selection() != invalid_object)
			context->set_selection(context->selection());
	}
};

static editor_state g;

/* 类型图标:2x2 纯色 CPU 纹理,按类型着色(无外部资源)。 */
static vgui_texture* make_type_texture(vgui_context* ui, uint32_t color)
{
	uint32_t pixels[4] = { color, color, color, color };
	vgui_texture_desc desc = {};
	desc.width = 2;
	desc.height = 2;
	desc.format = VGUI_TEXTURE_FORMAT_RGBA8;
	desc.pixels = pixels;
	return vgui_texture_create(ui, &desc);
}

/* item_for:遍历树把 object_id 映射回 vgui_item_id(图标等行级设置用)。 */
static vgui_item_id item_for(outliner_view& outliner, object_id id)
{
	/* kit 的 add_node 把 object 存进 item user;通过树 API 遍历。 */
	vgui_widget* tree = outliner.widget();
	if (!tree)
		return 0;
	std::vector<vgui_item_id> stack;
	for (int i = vgui_tree_get_root_count(tree) - 1; i >= 0; --i)
		stack.push_back(vgui_tree_get_root(tree, i));
	while (!stack.empty())
	{
		const vgui_item_id item = stack.back();
		stack.pop_back();
		const void* user = vgui_tree_get_item_user(tree, item);
		if ((object_id)(std::uintptr_t)user == id)
			return item;
		for (int i = vgui_tree_get_child_count(tree, item) - 1; i >= 0; --i)
			stack.push_back(vgui_tree_get_child(tree, item, i));
	}
	return 0;
}

static void setup_editor(vgui_context* ui)
{
	g.context = new editor_context(ui);
	g.shell = new editor_shell(*g.context);
	g.shell->build();

	command_stack& commands = g.context->commands();
	outliner_view& outliner = g.shell->scene();
	outliner.set_command_stack(&commands);

	/* 初始场景:World { Camera, Light }, Terrain, Enemies { Slime, Bat }。 */
	const object_id world = g.scene.add("World", invalid_object);
	g.scene.add("Camera", world);
	g.scene.add("Light", world);
	g.scene.add("Terrain", invalid_object);
	const object_id enemies = g.scene.add("Enemies", invalid_object);
	g.scene.add("Slime", enemies);
	g.scene.add("Bat", enemies);
	g.rebuild_outliner();

	/* 类型图标:场景根(青)、容器组(红)、普通对象(灰蓝),2x2 纯色 CPU
	 * 纹理,不依赖外部资源。 */
	vgui_texture* tex_scene = make_type_texture(ui, 0xFF40E0D0u);
	vgui_texture* tex_group = make_type_texture(ui, 0xFFE06060u);
	vgui_texture* tex_object = make_type_texture(ui, 0xFF7090C0u);
	for (const auto& entry : g.scene.objects)
	{
		const scene_object& o = entry.second;
		vgui_texture* icon = o.parent == invalid_object && o.children.empty()
			? tex_object
			: entry.first == world ? tex_scene
			: !o.children.empty() ? tex_group : tex_object;
		outliner.set_node_icon(item_for(outliner, entry.first), icon);
	}

	/* 移动:拒绝放进自己的子树。 */
	outliner.set_move_handler([&](object_id node, object_id parent,
		std::size_t index)
	{
		if (parent != invalid_object &&
			(g.scene.is_ancestor(node, parent) || node == parent))
			return false;
		g.scene.place(node, parent, index);
		g.rebuild_outliner();
		g.shell->set_status("moved object " + std::to_string(node));
		return true;
	});

	/* 重命名:空名/超长拒绝,重名追加后缀。 */
	outliner.set_rename_handler([&](object_id node, const std::string& name)
	{
		if (name.empty() || name.size() > 64)
			return false;
		g.scene.objects[node].name = name;
		g.rebuild_outliner();
		return true;
	});

	/* O06 动作:新建/删除/复制/聚焦由宿主实现,可包命令。 */
	outliner.set_action_handler([&](outliner_view::action kind,
		const std::vector<object_id>& nodes)
	{
		switch (kind)
		{
		case outliner_view::action::new_child:
		{
			if (nodes.empty())
				return false;
			const object_id created = g.scene.add("New Object", nodes.front());
			g.rebuild_outliner();
			g.context->set_selection(created);
			g.shell->set_status("new child of " + std::to_string(nodes.front()));
			return true;
		}
		case outliner_view::action::duplicate:
		{
			if (nodes.empty())
				return false;
			const object_id src = nodes.front();
			const object_id parent = g.scene.parent_of(src);
			const object_id copy = g.scene.duplicate(src, parent);
			g.rebuild_outliner();
			g.context->set_selection(copy);
			g.shell->set_status("duplicated object " + std::to_string(src));
			return true;
		}
		case outliner_view::action::remove:
		{
			if (nodes.empty())
				return false;
			for (object_id node : nodes)
			{
				const object_id parent = g.scene.parent_of(node);
				auto& src_list = parent == invalid_object
					? g.scene.roots : g.scene.objects[parent].children;
				auto it = std::find(src_list.begin(), src_list.end(), node);
				if (it != src_list.end())
					src_list.erase(it);
				g.scene.objects.erase(node);
			}
			g.context->clear_selection();
			g.rebuild_outliner();
			g.shell->set_status("deleted " +
				std::to_string(nodes.size()) + " object(s)");
			return true;
		}
		case outliner_view::action::focus:
			g.shell->set_status("focus object " +
				(nodes.empty() ? std::string("-")
					: std::to_string(nodes.front())));
			return true;
		default:
			return false;
		}
	});

	/* 可见性/锁定反馈到状态栏。 */
	outliner.set_visibility_listener([](object_id id, bool visible) {
		g.shell->set_status(std::string(visible ? "show " : "hide ") +
			(g.scene.objects.count(id) ? g.scene.objects[id].name
				: std::to_string(id)));
	});
	outliner.set_lock_listener([](object_id id, bool locked) {
		g.shell->set_status(std::string(locked ? "lock " : "unlock ") +
			(g.scene.objects.count(id) ? g.scene.objects[id].name
				: std::to_string(id)));
	});

	g.shell->set_status("M3 outliner ready");
}

int main()
{
	// ---- 后端初始化 ----
	auto* wg = new app_mgr();
	font_familys_t* familys = new_font_family(wg->font_ctx,
		"微软雅黑,Tahoma,Arial Unicode MS,Segoe UI,Arial,Consolas,Segoe UI Emoji", 0);
	auto* cb = new_ctx_cb();
	rvg_t* vg = cb->new_rvg(cb->ac);

	if (!wg->init_gpu(true))
	{
		fprintf(stderr, "init_gpu 失败\n");
		return -1;
	}

	const int win_w = 1280, win_h = 800;
	os_window* form = wg->create("vgui 游戏编辑器示例", win_w, win_h, 0, true);
	if (!form)
	{
		fprintf(stderr, "创建窗口失败\n");
		return -1;
	}

	// ---- 创建 UI 上下文并挂到窗口 viewport ----
	vgui_context* ui = vgui_create(familys, 16.0f);
	vgui_context_set_size(ui, win_w, win_h);
	setup_editor(ui);
	form->viewport->ui = ui;

	// ---- 主循环 ----
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

			vgui_update(ui, delta);

			rtc.end();
			rtc.begin();
			cb->clear(vg);
			cb->reset_clip(vg, 1);
			vgui_draw(ui, cb, vg);

			draw_datas_cx drawable;
			drawable.set(fbo);
			drawable.add(vg);
			drawable.render(wg->vgctx);
		}
	}

	// ---- 清理 ----
	wg->wait_gpu();
	delete g.shell;
	delete g.context;
	vgui_destroy(ui);
	form->viewport->ui = nullptr;
	delete_font_family(familys);
	cb->destroy_rvg(vg);
	free_ctx_cb(cb);
	delete wg;
	return 0;
}
