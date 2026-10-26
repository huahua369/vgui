// sample/vgui_game_editor.cpp: 游戏编辑器 dogfood 示例
//
// 用 vgui_editor_kit 搭一个最小的游戏场景编辑器,持续验证 M3 Outliner 的
// 编辑器工作流(roadmap §10:每个里程碑在示例中补一条可操作路径):
//   * editor_shell 外壳(Dock + 菜单 + 工具栏 + 状态栏 + 布局持久化)
//   * Outliner:类型图标、可见性/锁定行内按钮、F2/双击行内重命名、
//     拖放重排/改父子(含多选)、名称过滤高亮、双向选中同步、右键菜单
//   * Assets:Breadcrumb/前进后退/刷新、宿主目录、双击进入文件夹、
//     防抖搜索与类型过滤
//   * 命令栈:移动/重命名/删除/新建/复制全部可撤销
//
// 窗口与事件由 SDL3 后端(backends/vgui_sdl3.h)提供。

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
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
	/* 属性面板演示值(P08 多选编辑的批量目标)。 */
	std::map<std::string, std::string> props;
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

static bool load_demo_thumbnail(const thumbnail_cache::request& req,
	thumbnail_cache::image* out);
static bool read_demo_thumbnail_disk(const thumbnail_cache::request& req,
	thumbnail_cache::image* out);
static bool write_demo_thumbnail_disk(const thumbnail_cache::request& req,
	const thumbnail_cache::image& img);

struct editor_state
{
	game_scene scene;
	editor_context* context = nullptr;
	editor_shell* shell = nullptr;
	vgui_context* ui = nullptr;
	/* A02 资产目录由宿主提供,浏览器只按 path id 取快照。 */
	std::map<std::string, asset_browser::asset_directory> asset_directories;
	/* A04 资产索引:按名称解析属性引用/拖放载荷,目录与预览共用。 */
	std::map<std::string, asset_info> assets;
	vgui_item_id next_asset = 100;
	/* Browse 菜单项 → 资产名(菜单回调只带 item id)。 */
	std::map<vgui_item_id, std::string> browse_assets;

	bool valid_selection() const
	{
		return context->selection() != invalid_object &&
			scene.objects.count(context->selection()) != 0;
	}

	const asset_info* find_asset(const std::string& name) const
	{
		auto it = assets.find(name);
		return it == assets.end() ? nullptr : &it->second;
	}

	void add_asset(const std::string& path_id, const asset_info& info)
	{
		auto directory = asset_directories.find(path_id);
		if (directory != asset_directories.end())
		{
			auto& items = directory->second.items;
			auto it = std::find_if(items.begin(), items.end(),
				[&](const asset_info& item) { return item.name == info.name; });
			if (it == items.end())
				items.push_back(info);
			else
				*it = info;
		}
		assets[info.name] = info;
	}

	void remove_asset(const std::string& path_id, const std::string& name)
	{
		auto directory = asset_directories.find(path_id);
		if (directory != asset_directories.end())
		{
			auto& items = directory->second.items;
			items.erase(std::remove_if(items.begin(), items.end(),
				[&](const asset_info& item) { return item.name == name; }),
				items.end());
		}
		assets.erase(name);
	}

	std::string unique_asset_name(const std::string& base) const
	{
		if (!assets.count(base) && !base.empty())
			return base;
		std::string stem = base.empty() ? "Imported" : base;
		std::string extension;
		const std::size_t dot = stem.find_last_of('.');
		if (dot != std::string::npos && dot > 0)
		{
			extension = stem.substr(dot);
			stem = stem.substr(0, dot);
		}
		for (int suffix = 2; ; ++suffix)
		{
			const std::string name = stem + " (" + std::to_string(suffix) +
				")" + extension;
			if (!assets.count(name))
				return name;
		}
	}

	/* A04: 重做/创建命令用同一稳定 id 重新插入对象。 */
	void insert_object(const scene_object& snapshot, std::size_t index)
	{
		if (scene.objects.count(snapshot.id))
			return;
		scene.objects[snapshot.id] = snapshot;
		std::vector<object_id>* siblings = nullptr;
		if (snapshot.parent == invalid_object)
			siblings = &scene.roots;
		else
		{
			auto parent = scene.objects.find(snapshot.parent);
			if (parent != scene.objects.end())
				siblings = &parent->second.children;
			else
				siblings = &scene.roots;
		}
		index = std::min(index, siblings->size());
		siblings->insert(siblings->begin() + (std::ptrdiff_t)index,
			snapshot.id);
	}

	void erase_object(object_id id)
	{
		auto it = scene.objects.find(id);
		if (it == scene.objects.end())
			return;
		std::vector<object_id>& siblings = it->second.parent == invalid_object
			? scene.roots : scene.objects[it->second.parent].children;
		auto found = std::find(siblings.begin(), siblings.end(), id);
		if (found != siblings.end())
			siblings.erase(found);
		scene.objects.erase(it);
	}

	/* A04: 资产拖入场景 -> 新建叶子对象,返回快照供命令重做。 */
	object_id add_asset_object(const std::string& asset, object_id parent,
		std::size_t index, scene_object* out_snapshot)
	{
		std::string name = asset;
		const std::size_t dot = name.find_last_of('.');
		if (dot != std::string::npos && dot > 0)
			name = name.substr(0, dot);
		if (name.empty())
			name = "Asset";
		const object_id id = scene.add(name.c_str(), parent);
		scene.place(id, parent, index);
		const asset_info* info = find_asset(asset);
		scene.objects[id].props["tint"] = "#FF80C0FF";
		if (info && info->type == "Mesh")
			scene.objects[id].props["mesh"] = asset;
		else
			scene.objects[id].props["asset"] = asset;
		if (out_snapshot)
			*out_snapshot = scene.objects[id];
		return id;
	}

	void push_create_command(const scene_object& snapshot, std::size_t index);

	/* A04: OS 文件导入与宿主导入入口共用此路径;整批一个可撤销命令。 */
	bool import_assets(const std::vector<std::string>& files,
		const std::string& path_id);

	/* A06: 宿主资产数据库;浏览器只发结构化请求,这里负责冲突/只读/占用
	 * 检查、整批应用以及可撤销快照。 */
	bool asset_operation(const asset_browser::operation_request& request,
		asset_browser::operation_result* result);
	std::vector<asset_browser::asset_dependency> asset_dependencies(
		vgui_item_id item) const;
	std::vector<asset_browser::asset_move_target> asset_move_targets() const;

	bool locate_asset(vgui_item_id id, std::string* path_id,
		asset_info* out) const
	{
		for (const auto& directory : asset_directories)
		{
			for (const asset_info& item : directory.second.items)
			{
				if (item.id != id)
					continue;
				if (path_id)
					*path_id = directory.first;
				if (out)
					*out = item;
				return true;
			}
		}
		return false;
	}

	std::string asset_user_list(const std::string& name) const
	{
		std::vector<std::string> users;
		for (const auto& entry : scene.objects)
		{
			for (const auto& prop : entry.second.props)
			{
				if (prop.second != name)
					continue;
				if (std::find(users.begin(), users.end(), entry.second.name) ==
					users.end())
					users.push_back(entry.second.name);
			}
		}
		std::string text;
		for (std::size_t i = 0; i < users.size(); ++i)
		{
			if (i)
				text += ", ";
			text += users[i];
		}
		return text;
	}

	void rename_asset_references(const std::string& old_name,
		const std::string& new_name)
	{
		for (auto& entry : scene.objects)
		{
			for (auto& prop : entry.second.props)
			{
				if (prop.second == old_name)
					prop.second = new_name;
			}
		}
	}

	static std::string file_basename(const std::string& path);
	static std::string asset_type_for_path(const std::string& path);
	static vgui_color asset_base_color(const std::string& type);
	static vgui_color asset_accent_color(const std::string& type);

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

/* A06 Import...: 宿主负责文件对话框;这里用一个路径输入框模拟,确认后走
 * 与 OS 拖入完全相同的 import_files() 入口。 */
static void on_demo_import_prompt(vgui_context* ctx, int result,
	const char* text, void* user)
{
	(void)ctx;
	(void)user;
	if (result != VGUI_MODAL_RESULT_CONFIRM || !text || !*text || !g.shell)
		return;
	(void)g.shell->assets().import_files({ std::string(text) });
}

std::string editor_state::file_basename(const std::string& path)
{
	const std::size_t slash = path.find_last_of("/\\");
	return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string editor_state::asset_type_for_path(const std::string& path)
{
	std::string extension = file_basename(path);
	const std::size_t dot = extension.find_last_of('.');
	extension = dot == std::string::npos ? "" : extension.substr(dot + 1);
	for (char& c : extension)
		c = (char)std::tolower((unsigned char)c);
	if (extension == "mesh" || extension == "glb" || extension == "gltf" ||
		extension == "obj" || extension == "fbx")
		return "Mesh";
	if (extension == "png" || extension == "jpg" || extension == "jpeg" ||
		extension == "bmp" || extension == "tga" || extension == "dds")
		return "Texture";
	if (extension == "mat" || extension == "material")
		return "Material";
	return "File";
}

vgui_color editor_state::asset_base_color(const std::string& type)
{
	if (type == "Mesh") return 0xFF6E9ED8u;
	if (type == "Texture") return 0xFF6CC878u;
	if (type == "Material") return 0xFFC8A050u;
	return 0xFF8FA8C8u;
}

vgui_color editor_state::asset_accent_color(const std::string& type)
{
	if (type == "Mesh") return 0xFF355C8Cu;
	if (type == "Texture") return 0xFF327E42u;
	if (type == "Material") return 0xFF705820u;
	return 0xFF3A4658u;
}

void editor_state::push_create_command(const scene_object& snapshot,
	std::size_t index)
{
	if (!context)
		return;
	context->commands().push(std::make_unique<function_command>(
		"Create " + snapshot.name,
		[snapshot, index]() {
			g.insert_object(snapshot, index);
			if (g.shell)
				g.rebuild_outliner();
			if (g.context)
				g.context->set_selection(snapshot.id);
		},
		[snapshot]() {
			if (g.context && g.context->selection() == snapshot.id)
				g.context->clear_selection();
			g.erase_object(snapshot.id);
			if (g.shell)
				g.rebuild_outliner();
		}));
}

bool editor_state::import_assets(const std::vector<std::string>& files,
	const std::string& path_id)
{
	if (!context || !shell || files.empty() ||
		!asset_directories.count(path_id))
		return false;

	std::vector<asset_info> imported;
	for (const std::string& path : files)
	{
		const std::string base = file_basename(path);
		asset_info info;
		info.name = unique_asset_name(base);
		info.type = asset_type_for_path(path);
		info.id = next_asset++;
		add_asset(path_id, info);
		imported.push_back(info);
	}
	if (imported.empty())
		return false;

	asset_browser& browser = shell->assets();
	browser.reload();
	browser.set_selection({ imported.back().id });
	shell->set_status("imported " + std::to_string(imported.size()) +
		" asset(s) into " + path_id);

	context->commands().push(std::make_unique<function_command>(
		"Import assets",
		[path_id, imported]() {
			for (const asset_info& info : imported)
				g.add_asset(path_id, info);
			g.shell->assets().reload();
			g.shell->set_status("reimported " +
				std::to_string(imported.size()) + " asset(s)");
		},
		[path_id, imported]() {
			for (const asset_info& info : imported)
				g.remove_asset(path_id, info.name);
			g.shell->assets().reload();
			g.shell->set_status("undid import");
		}));
	return true;
}

bool editor_state::asset_operation(
	const asset_browser::operation_request& request,
	asset_browser::operation_result* result)
{
	if (!result)
		return false;
	result->error = asset_browser::operation_error::none;
	result->message.clear();
	result->undoable = false;
	result->undo = {};
	result->redo = {};
	result->select.clear();
	result->created.clear();

	const auto fail = [result](asset_browser::operation_error error,
		std::string message) {
		result->error = error;
		result->message = std::move(message);
		return false;
	};

	struct entry
	{
		std::string path_id;
		asset_info info;
	};
	std::vector<entry> entries;
	entries.reserve(request.items.size());
	for (vgui_item_id id : request.items)
	{
		entry found;
		if (!locate_asset(id, &found.path_id, &found.info) ||
			found.path_id.empty())
			return fail(asset_browser::operation_error::not_found,
				"asset " + std::to_string(id) + " no longer exists");
		entries.push_back(std::move(found));
	}
	if (entries.empty())
		return fail(asset_browser::operation_error::no_selection,
			"no assets selected");

	/* Ghost.mesh 演示只读资产:所有破坏性操作都必须给出同一类错误。 */
	for (const entry& item : entries)
	{
		if (item.info.type == "Folder")
			return fail(asset_browser::operation_error::unsupported,
				"folders are managed by the host database");
		if (item.info.name == "Ghost.mesh")
			return fail(asset_browser::operation_error::read_only,
				"Ghost.mesh is marked read-only");
	}

	const auto before_dirs = asset_directories;
	const auto before_assets = assets;

	auto capture_command = [this, &before_dirs, &before_assets, result](
		const std::map<object_id, std::map<std::string, std::string>>*
			props_before = nullptr,
		const std::map<object_id, std::map<std::string, std::string>>*
			props_after = nullptr)
	{
		const auto after_dirs = asset_directories;
		const auto after_assets = assets;
		result->undoable = true;
		result->undo = [this, before_dirs, before_assets, props_before]() {
			asset_directories = before_dirs;
			assets = before_assets;
			if (props_before)
			{
				for (const auto& entry : *props_before)
				{
					auto object = scene.objects.find(entry.first);
					if (object != scene.objects.end())
						object->second.props = entry.second;
				}
			}
			if (shell)
				shell->set_status("undid asset operation");
			return true;
		};
		result->redo = [this, after_dirs, after_assets, props_after]() {
			asset_directories = after_dirs;
			assets = after_assets;
			if (props_after)
			{
				for (const auto& entry : *props_after)
				{
					auto object = scene.objects.find(entry.first);
					if (object != scene.objects.end())
						object->second.props = entry.second;
				}
			}
			if (shell)
				shell->set_status("redid asset operation");
			return true;
		};
	};

	switch (request.kind)
	{
	case asset_browser::operation::rename:
	{
		if (entries.size() != 1)
			return fail(asset_browser::operation_error::invalid_name,
				"rename expects exactly one asset");
		const std::string clean = request.name;
		if (clean.empty() || clean.find_first_of("/\\") != std::string::npos)
			return fail(asset_browser::operation_error::invalid_name,
				"name cannot be empty or contain path separators");
		const entry& item = entries.front();
		if (clean == item.info.name)
			return true;
		auto existing = assets.find(clean);
		if (existing != assets.end() && existing->second.name != item.info.name)
			return fail(asset_browser::operation_error::conflict,
				"an asset named '" + clean + "' already exists");
		std::map<object_id, std::map<std::string, std::string>> props_before_map;
		for (const auto& object : scene.objects)
			props_before_map[object.first] = object.second.props;

		auto directory = asset_directories.find(item.path_id);
		if (directory == asset_directories.end())
			return fail(asset_browser::operation_error::not_found,
				"directory " + item.path_id + " no longer exists");
		auto slot = std::find_if(directory->second.items.begin(),
			directory->second.items.end(),
			[&](const asset_info& info) { return info.id == item.info.id; });
		if (slot == directory->second.items.end())
			return fail(asset_browser::operation_error::not_found,
				"asset disappeared from its directory");
		assets.erase(item.info.name);
		slot->name = clean;
		assets[clean] = *slot;
		rename_asset_references(item.info.name, clean);
		result->select.push_back(item.info.id);

		std::map<object_id, std::map<std::string, std::string>> props_after_map;
		for (const auto& object : scene.objects)
			props_after_map[object.first] = object.second.props;
		capture_command(&props_before_map, &props_after_map);
		return true;
	}
	case asset_browser::operation::remove:
	{
		for (const entry& item : entries)
		{
			const std::string users = asset_user_list(item.info.name);
			if (!users.empty())
				return fail(asset_browser::operation_error::in_use,
					"'" + item.info.name + "' is used by " + users);
		}
		for (const entry& item : entries)
			remove_asset(item.path_id, item.info.name);
		capture_command();
		return true;
	}
	case asset_browser::operation::duplicate:
	{
		for (const entry& item : entries)
		{
			asset_info copy = item.info;
			copy.name = unique_asset_name(item.info.name);
			copy.id = next_asset++;
			add_asset(item.path_id, copy);
			result->created.push_back(copy.id);
			result->select.push_back(copy.id);
		}
		capture_command();
		return true;
	}
	case asset_browser::operation::move:
	{
		if (request.target_path_id.empty() ||
			request.target_path_id == request.path_id)
			return fail(asset_browser::operation_error::invalid_target,
				"choose a different destination folder");
		auto target = asset_directories.find(request.target_path_id);
		if (target == asset_directories.end())
			return fail(asset_browser::operation_error::invalid_target,
				"destination folder no longer exists");
		for (const entry& item : entries)
		{
			for (const asset_info& existing : target->second.items)
			{
				if (existing.name == item.info.name)
					return fail(asset_browser::operation_error::conflict,
						"'" + item.info.name +
						"' already exists in the destination folder");
			}
		}
		for (const entry& item : entries)
		{
			remove_asset(item.path_id, item.info.name);
			add_asset(request.target_path_id, item.info);
		}
		capture_command();
		return true;
	}
	case asset_browser::operation::reimport:
	{
		if (entries.size() == 1 && entries.front().info.name == "Broken.mesh")
			return fail(asset_browser::operation_error::io_error,
				"Broken.mesh failed to decode; the source file is corrupt");
		/* Reimport only refreshes derived data and intentionally serves as the
		 * demo's non-undoable operation. */
		result->select = request.items;
		result->message = "refreshed from disk; no undo snapshot";
		return true;
	}
	}
	return fail(asset_browser::operation_error::unsupported,
		"unhandled asset operation");
}

std::vector<asset_browser::asset_dependency> editor_state::asset_dependencies(
	vgui_item_id item) const
{
	std::vector<asset_browser::asset_dependency> deps;
	std::string path_id;
	asset_info info;
	if (!locate_asset(item, &path_id, &info))
		return deps;
	for (const auto& entry : scene.objects)
	{
		bool used = false;
		for (const auto& prop : entry.second.props)
		{
			if (prop.second == info.name)
			{
				used = true;
				break;
			}
		}
		if (!used)
			continue;
		asset_browser::asset_dependency dep;
		dep.name = entry.second.name;
		dep.type = "Scene Object";
		dep.path_id = "scene";
		deps.push_back(std::move(dep));
	}
	return deps;
}

std::vector<asset_browser::asset_move_target> editor_state::asset_move_targets() const
{
	std::vector<asset_browser::asset_move_target> targets;
	for (const auto& directory : asset_directories)
	{
		asset_browser::asset_move_target target;
		for (std::size_t i = 0; i < directory.second.path.size(); ++i)
		{
			if (i)
				target.label += " / ";
			target.label += directory.second.path[i].label;
		}
		if (target.label.empty())
			target.label = directory.first;
		target.path_id = directory.first;
		targets.push_back(std::move(target));
	}
	return targets;
}

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

/* A05 缩略图加载器:在后台线程生成 16x16 棋盘格 CPU 像素。
 * "Broken.mesh" 前两次故意失败,第三次成功,用来演示自动重试。 */
static bool demo_thumbnail_failure(const std::string& name)
{
	if (name != "Broken.mesh")
		return false;
	static std::mutex mutex;
	static std::map<std::string, int> attempts;
	std::lock_guard<std::mutex> lock(mutex);
	return ++attempts[name] <= 2;
}

static std::filesystem::path demo_thumbnail_file(const std::string& key)
{
	const std::size_t hash = std::hash<std::string>{}(key);
	return std::filesystem::temp_directory_path() /
		"vgui_game_editor_thumbs" / (std::to_string(hash) + ".thumb");
}

static void render_demo_thumbnail(const thumbnail_cache::request& req,
	thumbnail_cache::image* out)
{
	const int size = 16;
	out->width = size;
	out->height = size;
	out->stride = size * 4;
	out->format = VGUI_TEXTURE_FORMAT_RGBA8;
	out->pixels.resize((std::size_t)size * size * 4u);

	uint32_t hash = 2166136261u;
	for (unsigned char ch : req.name)
		hash = (hash ^ ch) * 16777619u;
	const int cell = 2 + (int)(hash % 4);
	const int shine = 18 + (int)(hash % 13);
	const vgui_color base = editor_state::asset_base_color(req.type);
	const vgui_color accent = editor_state::asset_accent_color(req.type);

	for (int y = 0; y < size; ++y)
	{
		for (int x = 0; x < size; ++x)
		{
			const bool checker = ((x / cell) + (y / cell)) % 2 == 0;
			const bool highlight = x + y >= shine && x + y <= shine + 4;
			const vgui_color color = highlight ? 0xFFFFFFFFu
				: (checker ? base : accent);
			unsigned char* p = out->pixels.data() +
				((std::size_t)y * size + x) * 4u;
			p[0] = (unsigned char)((color >> 16) & 0xFF);
			p[1] = (unsigned char)((color >> 8) & 0xFF);
			p[2] = (unsigned char)(color & 0xFF);
			p[3] = (unsigned char)((color >> 24) & 0xFF);
		}
	}
}

static bool load_demo_thumbnail(const thumbnail_cache::request& req,
	thumbnail_cache::image* out)
{
	/* 模拟真实解码/采样耗时,让占位与取消在演示里可见。 */
	std::this_thread::sleep_for(std::chrono::milliseconds(24));
	if (demo_thumbnail_failure(req.name))
	{
		out->error = "simulated corrupt thumbnail";
		return false;
	}
	render_demo_thumbnail(req, out);
	return true;
}

static bool read_demo_thumbnail_disk(const thumbnail_cache::request& req,
	thumbnail_cache::image* out)
{
	if (req.name == "Broken.mesh")
		return false;	/* 每次运行都演示一次失败重试 */
	try
	{
		std::ifstream in(demo_thumbnail_file(req.key), std::ios::binary);
		if (!in)
			return false;
		std::int32_t header[4] = {};
		in.read(reinterpret_cast<char*>(header), sizeof(header));
		if (!in || header[0] <= 0 || header[1] <= 0 || header[2] <= 0 ||
			header[0] > 4096 || header[1] > 4096 ||
			header[3] < 0 || header[3] > (std::int32_t)VGUI_TEXTURE_FORMAT_BGRA8_SRGB)
			return false;
		const std::size_t bytes = (std::size_t)header[2] *
			(std::size_t)header[1];
		if (bytes == 0 || bytes > 4u * 1024u * 1024u)
			return false;
		out->width = header[0];
		out->height = header[1];
		out->stride = header[2];
		out->format = (vgui_texture_format)header[3];
		out->pixels.resize(bytes);
		in.read(reinterpret_cast<char*>(out->pixels.data()),
			(std::streamsize)bytes);
		return (bool)in;
	}
	catch (...)
	{
		return false;
	}
}

static bool write_demo_thumbnail_disk(const thumbnail_cache::request& req,
	const thumbnail_cache::image& img)
{
	if (req.name == "Broken.mesh")
		return false;
	try
	{
		const std::filesystem::path file = demo_thumbnail_file(req.key);
		std::filesystem::create_directories(file.parent_path());
		std::ofstream out(file, std::ios::binary | std::ios::trunc);
		if (!out)
			return false;
		const std::int32_t header[4] = { img.width, img.height, img.stride,
			(std::int32_t)img.format };
		out.write(reinterpret_cast<const char*>(header), sizeof(header));
		out.write(reinterpret_cast<const char*>(img.pixels.data()),
			(std::streamsize)img.pixels.size());
		return (bool)out;
	}
	catch (...)
	{
		return false;
	}
}

/* A03 预览加载器在后台线程运行:只读请求与静态数据,绝不调用 vgui。 */
static asset_preview::image_data make_preview_image(int size, vgui_color base,
	vgui_color accent, bool checker)
{
	asset_preview::image_data image;
	image.width = size;
	image.height = size;
	image.stride = size * 4;
	image.format = VGUI_TEXTURE_FORMAT_RGBA8;
	image.pixels.resize((std::size_t)size * (std::size_t)size * 4u);
	const unsigned br = (base >> 16) & 0xFF, bg = (base >> 8) & 0xFF, bb = base & 0xFF;
	const unsigned ar = (accent >> 16) & 0xFF, ag = (accent >> 8) & 0xFF, ab = accent & 0xFF;
	for (int y = 0; y < size; ++y)
	{
		for (int x = 0; x < size; ++x)
		{
			float t = (float)(x + y) / (float)(size * 2);
			unsigned r = (unsigned)(br + (ar - br) * t);
			unsigned g = (unsigned)(bg + (ag - bg) * t);
			unsigned b = (unsigned)(bb + (ab - bb) * t);
			if (checker && (((x / 8) + (y / 8)) & 1))
			{
				r = (r + 235u) / 2u;
				g = (g + 235u) / 2u;
				b = (b + 235u) / 2u;
			}
			const float dx = (float)x - (float)size * 0.5f;
			const float dy = (float)y - (float)size * 0.5f;
			const float dist = std::sqrt(dx * dx + dy * dy);
			if (dist < (float)size * 0.27f)
			{
				r = (r + 255u) / 2u;
				g = (g + 255u) / 2u;
				b = (b + 255u) / 2u;
			}
			const std::size_t p = ((std::size_t)y * (std::size_t)size + (std::size_t)x) * 4u;
			image.pixels[p] = (unsigned char)std::min(255u, r);
			image.pixels[p + 1] = (unsigned char)std::min(255u, g);
			image.pixels[p + 2] = (unsigned char)std::min(255u, b);
			image.pixels[p + 3] = 255;
		}
	}
	return image;
}

static bool load_demo_preview(const asset_preview::request& req,
	asset_preview::data* out)
{
	if (!out)
		return false;
	using kind = asset_preview::preview_kind;
	out->name = req.name;
	out->type = req.type;

	if (req.type == "Folder")
	{
		out->kind = kind::none;
		out->detail = "Folder";
		out->fields.push_back({ "Type", "Folder" });
		out->fields.push_back({ "Open", "Double-click" });
		return true;
	}

	if (req.name == "Broken.mesh")
	{
		out->kind = kind::model;
		out->error = "Corrupt mesh header (0x0BADC0DE)";
		out->fields.push_back({ "Error", out->error });
		out->fields.push_back({ "Hint", "Re-import or repair the file" });
		return false;
	}

	if (req.type == "Texture")
	{
		/* Deliberately slow enough to show the loading state without blocking
		 * the editor thread. */
		std::this_thread::sleep_for(std::chrono::milliseconds(90));
		const bool checker = req.name == "checker.png";
		out->kind = kind::image;
		out->detail = "RGBA8 texture";
		out->accent = checker ? 0xFFC8A050u : 0xFF6CC878u;
		out->secondary = checker ? 0xFF705820u : 0xFF327E42u;
		out->image = make_preview_image(96,
			checker ? 0xFFC8A050u : 0xFF6CC878u,
			checker ? 0xFF705820u : 0xFF327E42u, checker);
		out->fields.push_back({ "Size", "96 x 96" });
		out->fields.push_back({ "Format", "RGBA8" });
		out->fields.push_back({ "Filter", "Linear / repeat" });
		return true;
	}

	if (req.type == "Mesh")
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(60));
		out->kind = kind::model;
		const bool sphere = req.name == "Sphere.mesh";
		out->detail = sphere ? "Sphere · 482 tris" : "Static mesh · 12 tris";
		out->accent = sphere ? 0xFF6E9ED8u : 0xFFC08A4Eu;
		out->secondary = sphere ? 0xFF355C8Cu : 0xFF7A5428u;
		out->fields.push_back({ "Vertices", sphere ? "242" : "8" });
		out->fields.push_back({ "Triangles", sphere ? "482" : "12" });
		out->fields.push_back({ "Bounds", sphere ? "1.0 x 1.0 x 1.0" : "1.0 x 1.0 x 1.0" });
		out->fields.push_back({ "Material", "Steel.mat" });
		return true;
	}

	if (req.type == "Material")
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(70));
		out->kind = kind::material;
		out->detail = "PBR material";
		out->accent = 0xFF8FA8C8u;
		out->secondary = 0xFF3A4658u;
		out->swatches = { 0xFF8FA8C8u, 0xFF3A4658u, 0xFFE0B040u, 0xFF202830u };
		out->fields.push_back({ "Shader", "Standard PBR" });
		out->fields.push_back({ "Metallic", "0.85" });
		out->fields.push_back({ "Roughness", "0.28" });
		out->fields.push_back({ "Textures", "albedo, normal, orm" });
		return true;
	}

	out->kind = kind::none;
	out->detail = "No preview available";
	out->fields.push_back({ "Type", req.type.empty() ? std::string("-") : req.type });
	return true;
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
	g.ui = ui;
	g.context = new editor_context(ui);
	g.shell = new editor_shell(*g.context);
	g.shell->build();
	g.shell->preview().set_load_handler(&load_demo_preview);

	command_stack& commands = g.context->commands();
	outliner_view& outliner = g.shell->scene();
	outliner.set_command_stack(&commands);

	/* 初始场景:World { Camera, Light }, Terrain, Enemies { Slime, Bat }。 */
	const object_id world = g.scene.add("World", invalid_object);
	const object_id camera = g.scene.add("Camera", world);
	const object_id light = g.scene.add("Light", world);
	g.scene.add("Terrain", invalid_object);
	const object_id enemies = g.scene.add("Enemies", invalid_object);
	const object_id slime = g.scene.add("Slime", enemies);
	const object_id bat = g.scene.add("Bat", enemies);
	/* P08 演示:对象间值不同 → 多选时属性行进入混合态。 */
	g.scene.objects[camera].props["tint"] = "#FFFFFFFF";
	g.scene.objects[camera].props["mesh"] = "Cube.mesh";
	/* A06 依赖/占用演示:材质被相机使用,删除 Steel.mat 会被宿主拒绝。 */
	g.scene.objects[camera].props["material"] = "Steel.mat";
	g.scene.objects[light].props["tint"] = "#FFE0B040";
	g.scene.objects[slime].props["tint"] = "#FF60E060";
	g.scene.objects[slime].props["mesh"] = "Sphere.mesh";
	g.scene.objects[bat].props["tint"] = "#FF8060C0";
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

	/* ---- M2 属性编辑 dogfood:选中对象 → 类型化属性行 ---- */
	property_grid& props = g.shell->properties();
	props.set_command_stack(&commands);

	/* 资产表:query 解析;browse 用菜单做简易选择列表(popup 由核心
	 * 菜单承载,足够 MVP)。 */
	props.set_asset_query([](const std::string& id, const std::string&,
		property_grid::asset_info* out)
	{
		const asset_info* asset = g.find_asset(id);
		if (!asset)
			return false;
		out->name = asset->name;
		out->kind = asset->type;
		out->thumbnail = asset->thumbnail;
		return true;
	});
	/* A04:mesh 行只接受 Mesh 资产,悬停/落点反馈会明确拒绝其它类型。 */
	props.set_asset_filter_check([](const std::string& id,
		const std::string& filter)
	{
		const asset_info* asset = g.find_asset(id);
		if (!asset)
			return false;
		if (filter == "mesh")
			return asset->type == "Mesh";
		if (filter == "material")
			return asset->type == "Material";
		return true;
	});
	props.set_asset_browse_handler([ui](const std::string& current,
		const std::string& filter)
	{
		vgui_widget* menu = vgui_widget_create(ui, VGUI_MENU, nullptr);
		g.browse_assets.clear();
		const vgui_item_id none = vgui_menu_add_item(menu, "(None)");
		g.browse_assets[none] = "";
		for (const auto& entry : g.assets)
		{
			if (filter == "mesh" && entry.second.type != "Mesh")
				continue;
			if (filter == "material" && entry.second.type != "Material")
				continue;
			const vgui_item_id item = vgui_menu_add_item(menu,
				entry.first.c_str());
			g.browse_assets[item] = entry.first;
		}
		vgui_menu_on_activate(menu, [](vgui_widget*, vgui_item_id item, void*) {
			auto found = g.browse_assets.find(item);
			if (found == g.browse_assets.end())
				return;
			g.shell->properties().commit_property_value("mesh", found->second);
		}, nullptr);
		(void)current;
		vgui_menu_open(menu, 400.0f, 300.0f);
	});
	props.set_object_activated_handler([](object_id id) {
		g.context->set_selection(id);	/* 点击引用名跳转选择 */
	});
	props.set_missing_asset_handler([](const std::string& id,
		const std::string&)
	{
		g.shell->set_status("missing asset on " + id);
	});

	/* 选中变化 → 重建属性行(名称/颜色/网格/目标)。 */
	/* 选中变化 → 重建属性行;Outliner 多选(P08)时进入多目标模式,
	 * 值不同的行显示混合态,提交批量写入所有选中对象。 */
	g.context->add_selection_listener([](object_id id) {
		property_grid& grid = g.shell->properties();
		grid.clear();

		/* 多选集来自 Outliner(视觉顺序);单选退化为普通模式。 */
		std::vector<object_id> targets =
			g.shell->scene().selection_objects();
		if (targets.size() <= 1 || !g.scene.objects.count(id))
			targets.clear();
		grid.set_multi_targets(targets);

		if (id == invalid_object || !g.scene.objects.count(id))
			return;
		const scene_object& o = g.scene.objects[id];

		auto prop_of = [&](const std::string& pid,
			const std::string& fallback) -> std::string
		{
			auto it = g.scene.objects[id].props.find(pid);
			return it == g.scene.objects[id].props.end() ? fallback
				: it->second;
		};

		property_desc name;
		name.id = "name";
		name.label = "Name";
		name.type = property_type::text;
		name.value = o.name;
		name.category = "Object";
		name.default_value = o.name;
		name.has_default = true;
		grid.add_property(name);

		property_desc color;
		color.id = "tint";
		color.label = "Tint";
		color.type = property_type::color;
		color.value = prop_of("tint", "#FFFFFFFF");
		color.category = "Rendering";
		color.tooltip = "Light tint (HSV picker)";
		grid.add_property(color);

		property_desc mesh;
		mesh.id = "mesh";
		mesh.label = "Mesh";
		mesh.type = property_type::asset;
		mesh.value = prop_of("mesh", "");
		mesh.asset_filter = "mesh";
		mesh.category = "Rendering";
		mesh.tooltip = "Drag from ASSETS or use ...";
		/* A04:拖放/浏览提交写回宿主模型;undo/redo 经同一 setter 同步。 */
		mesh.set_value = [oid = id](const std::string& value) {
			auto it = g.scene.objects.find(oid);
			if (it != g.scene.objects.end())
				it->second.props["mesh"] = value;
		};
		grid.add_property(mesh);

		property_desc target;
		target.id = "target";
		target.label = "Target";
		target.type = property_type::object_ref;
		target.value = prop_of("target", "");
		target.category = "AI";
		target.tooltip = "Click the name to select";
		grid.add_property(target);

		/* P09:曲线 / 渐变自定义编辑器。行内显示紧凑预览,点击打开
		 * popup;预览与 popup 共用同一撤销记录。 */
		property_desc idle_curve;
		idle_curve.id = "idle_curve";
		idle_curve.label = "Idle Curve";
		idle_curve.type = property_type::curve;
		idle_curve.value = prop_of("idle_curve",
			"cubic;0,0,0,0;0.5,1,0,0;1,0,0,0");
		idle_curve.category = "Animation";
		idle_curve.tooltip = "Double-click the graph to add keys; edit tangents";
		idle_curve.set_value = [oid = id](const std::string& value) {
			auto it = g.scene.objects.find(oid);
			if (it != g.scene.objects.end())
				it->second.props["idle_curve"] = value;
		};
		grid.add_property(idle_curve);

		property_desc color_ramp;
		color_ramp.id = "color_ramp";
		color_ramp.label = "Color Ramp";
		color_ramp.type = property_type::gradient;
		color_ramp.value = prop_of("color_ramp",
			"0,#FF6A00;0.5,#FFD000;1,#FFFFFF");
		color_ramp.category = "Rendering";
		color_ramp.tooltip = "Drag stops, double-click to add, apply presets";
		color_ramp.set_value = [oid = id](const std::string& value) {
			auto it = g.scene.objects.find(oid);
			if (it != g.scene.objects.end())
				it->second.props["color_ramp"] = value;
		};
		grid.add_property(color_ramp);

		/* 多目标 getter/setter:值存在对象 props 里(name 特殊)。 */
		grid.set_multi_getter([](const std::string& pid, object_id obj)
			-> std::string
		{
			auto it = g.scene.objects.find(obj);
			if (it == g.scene.objects.end())
				return "";
			if (pid == "name")
				return it->second.name;
			auto pit = it->second.props.find(pid);
			return pit == it->second.props.end() ? "" : pit->second;
		});
		grid.set_multi_setter([](const std::string& pid, object_id obj,
			const std::string& value)
		{
			auto it = g.scene.objects.find(obj);
			if (it == g.scene.objects.end())
				return;
			if (pid == "name")
			{
				it->second.name = value;
				g.rebuild_outliner();
			}
			else
			{
				it->second.props[pid] = value;
			}
		});
		/* 多目标模式下重建后立即检测混合态。 */
		grid.refresh();

		grid.set_change_listener([](const std::string& pid,
			const std::string& value)
		{
			/* 多目标模式写入走 multi_setter;这里只做状态反馈与
			 * 单选时的模型写回。 */
			object_id sel = g.context->selection();
			if (sel == invalid_object || !g.scene.objects.count(sel))
				return;
			if (pid == "mesh" || pid == "target" || pid == "tint" ||
				pid == "idle_curve" || pid == "color_ramp")
			{
				g.shell->set_status(pid + " = " + (value.empty()
					? std::string("(none)") : value));
			}
		});
	});

	/* A02 资产浏览器:宿主按 path id 提供面包屑与目录内容,双击文件夹进入;
	 * 返回/前进恢复该目录的选择与滚动,搜索/类型过滤在本地视图上生效。 */
	asset_browser& assets = g.shell->assets();
	static const struct
	{
		const char* name;
		const char* type;
		const char* folder;
	} kDemoAssets[] = {
		{ "Cube.mesh", "Mesh", "models" },
		{ "Sphere.mesh", "Mesh", "models" },
		{ "Ghost.mesh", "Mesh", "models" },
		{ "Broken.mesh", "Mesh", "models" },
		{ "icon.png", "Texture", "textures" },
		{ "checker.png", "Texture", "textures" },
		{ "Steel.mat", "Material", "materials" },
	};
	g.asset_directories.clear();

	asset_browser::asset_directory root;
	root.path.push_back({ "Root", "root" });
	root.items.push_back({ "Models", "Folder", nullptr, 1 });
	root.items.push_back({ "Textures", "Folder", nullptr, 2 });
	root.items.push_back({ "Empty", "Folder", nullptr, 3 });
	root.items.push_back({ "Materials", "Folder", nullptr, 4 });
	g.asset_directories["root"] = root;

	asset_browser::asset_directory models;
	models.path.push_back({ "Root", "root" });
	models.path.push_back({ "Models", "models" });
	g.asset_directories["models"] = models;

	asset_browser::asset_directory textures;
	textures.path.push_back({ "Root", "root" });
	textures.path.push_back({ "Textures", "textures" });
	g.asset_directories["textures"] = textures;

	asset_browser::asset_directory empty;
	empty.path.push_back({ "Root", "root" });
	empty.path.push_back({ "Empty", "empty" });
	empty.empty_text = "No assets in this folder";
	g.asset_directories["empty"] = empty;

	asset_browser::asset_directory materials;
	materials.path.push_back({ "Root", "root" });
	materials.path.push_back({ "Materials", "materials" });
	g.asset_directories["materials"] = materials;

	vgui_item_id first_asset = 0;
	for (const auto& item : kDemoAssets)
	{
		asset_info info;
		info.name = item.name;
		info.type = item.type;
		info.id = g.next_asset++;
		g.asset_directories[item.folder].items.push_back(info);
		g.assets[info.name] = info;
		if (!first_asset)
			first_asset = info.id;
	}

	assets.set_type_options({ "Folder", "Mesh", "Texture", "Material" });

	/* A05 异步缩略图:worker 线程生成/读盘,主线程只上传纹理。
	 * 可见范围变化时取消过期请求;LRU 限制内存中的纹理数量。 */
	thumbnail_cache& thumbnails = assets.thumbnails();
	thumbnail_cache::options cache_options;
	cache_options.memory_limit = 96;
	cache_options.max_attempts = 3;
	cache_options.retry_delay = 0.45f;
	cache_options.disk_cache = true;
	thumbnails.set_options(cache_options);
	thumbnails.set_load_handler(&load_demo_thumbnail);
	thumbnails.set_disk_cache(&read_demo_thumbnail_disk,
		&write_demo_thumbnail_disk);
	assets.set_navigate_handler([](const std::string& path_id,
		asset_browser::asset_directory* out) {
		auto found = g.asset_directories.find(path_id);
		if (found == g.asset_directories.end() || !out)
			return false;
		*out = found->second;
		return true;
	});
	assets.set_path_listener([](const std::string& path_id) {
		g.shell->set_status("assets: " + path_id);
	});
	assets.set_activate_listener([](vgui_item_id item) {
		if (item == 1)
		{
			(void)g.shell->assets().navigate_to("models");
			return;
		}
		if (item == 2)
		{
			(void)g.shell->assets().navigate_to("textures");
			return;
		}
		if (item == 3)
		{
			(void)g.shell->assets().navigate_to("empty");
			return;
		}
		if (item == 4)
		{
			(void)g.shell->assets().navigate_to("materials");
			return;
		}
		asset_browser& browser = g.shell->assets();
		const int index = browser.index_of(item);
		asset_info info;
		if (index >= 0 && browser.item_at(index, &info))
			g.shell->set_status("open asset: " + info.name);
	});
	(void)assets.navigate_to("models");
	if (first_asset)
		assets.set_selection({ first_asset });
	assets.set_view(asset_browser::view_mode::grid);

	/* ---- A04 三条资产拖放路径 + OS 文件导入 ----------------------------- */

	/* 1) Viewport:落点处新建对象;新建走命令栈,undo 删除、redo 原 id 重建。 */
	g.shell->viewport().set_drop_listener([](const drag_payload& payload,
		float, float) -> bool
	{
		if (payload.kind != drag_payload_kind::asset || !g.find_asset(payload.id))
			return false;
		object_id parent = g.context->selection();
		if (!g.scene.objects.count(parent))
			parent = invalid_object;
		const std::size_t index = parent == invalid_object
			? g.scene.roots.size() : g.scene.objects[parent].children.size();
		scene_object snapshot;
		const object_id created = g.add_asset_object(payload.id, parent, index,
			&snapshot);
		g.rebuild_outliner();
		g.context->set_selection(created);
		g.push_create_command(snapshot, index);
		g.shell->set_status("created " + snapshot.name + " from " + payload.id);
		return true;
	});

	/* 2) Outliner:落点解析为 parent/index,建立父子/兄弟关系。 */
	outliner.set_asset_drop_handler([](const std::string& asset,
		object_id parent, std::size_t index) -> bool
	{
		if (!g.find_asset(asset))
			return false;
		scene_object snapshot;
		const object_id created = g.add_asset_object(asset, parent, index,
			&snapshot);
		g.rebuild_outliner();
		g.context->set_selection(created);
		g.push_create_command(snapshot, index);
		g.shell->set_status("parented " + snapshot.name + " under " +
			(parent == invalid_object ? std::string("root")
				: std::to_string(parent)));
		return true;
	});

	/* 3) PropertyGrid:行级 drop 由 kit 处理;落点不在行上时仍按 Mesh 赋值。 */
	props.set_asset_drop_handler([](const std::string& asset) -> bool
	{
		const asset_info* info = g.find_asset(asset);
		if (!info || info->type != "Mesh" || !g.valid_selection())
			return false;
		if (!g.shell->properties().commit_property_value("mesh", asset))
			return false;
		g.shell->set_status("assigned " + asset + " to mesh");
		return true;
	});

	/* 4) OS 文件拖入与导入入口一致:整批可撤销,写入当前目录。 */
	assets.set_import_handler([](const std::vector<std::string>& files,
		const std::string& path_id) -> bool
	{
		return g.import_assets(files, path_id);
	});

	/* ---- A06 资产操作:右键菜单/快捷键 + 宿主数据库事务 ------------------ */
	assets.set_command_stack(&commands);
	assets.set_operation_handler([](const asset_browser::operation_request& request,
		asset_browser::operation_result* result) -> bool
	{
		return g.asset_operation(request, result);
	});
	assets.set_notice_listener([](const std::string& message,
		asset_browser::operation_error error)
	{
		if (!g.shell)
			return;
		const char* prefix = error == asset_browser::operation_error::none
			? "assets: " : "asset error: ";
		g.shell->set_status(prefix + message);
	});
	assets.set_move_targets_handler([]() {
		return g.asset_move_targets();
	});
	assets.set_dependency_handler([](vgui_item_id item) {
		return g.asset_dependencies(item);
	});
	assets.set_import_prompt_handler([]() {
		vgui_dialog_prompt(g.ui, "Import Asset",
			"File path (simulated OS dialog):", "NewAsset.png",
			&on_demo_import_prompt, nullptr);
	});
	/* F2 / Delete / Ctrl+D / F5 / Ctrl+I 与右键菜单共用同一 dispatch。 */
	assets.enable_context_menu();

	g.shell->set_status("A06: F2 rename, Del delete, Ctrl+D duplicate, F5 reimport, Ctrl+I import");
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
