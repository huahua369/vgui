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
#include "phase0_renderer.h"
#include "editor_transform.h"

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
	/* V05/V07: world transform used by the viewport demo (gizmo + renderer). */
	camera_vec3 position{};
	glm::quat rotation{ 1.0f, 0.0f, 0.0f, 0.0f };
	camera_vec3 scale{ 1.0f, 1.0f, 1.0f };
	/* V06: viewport-dropped objects (and the demo cube) are drawn/pickable. */
	bool renderable = false;
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
		objects[copy].position = o.position;
		objects[copy].rotation = o.rotation;
		objects[copy].scale = o.scale;
		objects[copy].renderable = o.renderable;
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

/* V09: borrows a phase0 attachment as a vgui native texture.  The UI texture is
 * only updated when the GPU handle or the attachment size changes, so a
 * stable frame does not dirty the UI layout. */
struct native_view_texture
{
	vgui_texture* texture = nullptr;
	SDL_GPUTexture* source = nullptr;
	int width = 0;
	int height = 0;

	bool sync(vgui_context* ui, vgpu_ctx_t* gpu_ctx, viewport_view& surface,
		SDL_GPUTexture* next_source, int next_width, int next_height)
	{
		if (!ui || !gpu_ctx || !next_source || next_width <= 0 ||
			next_height <= 0)
			return false;
		if (texture && source == next_source && width == next_width &&
			height == next_height)
			return false;

		if (!texture)
		{
			texture = vgui_sdl3_texture_create_native(ui, gpu_ctx,
				next_source, next_width, next_height,
				VGUI_TEXTURE_FORMAT_RGBA8);
			if (!texture)
				return false;
			surface.set_texture(texture);
		}
		else if (!vgui_sdl3_texture_update_native(texture, gpu_ctx,
			next_source, next_width, next_height, VGUI_TEXTURE_FORMAT_RGBA8))
		{
			return false;
		}

		source = next_source;
		width = next_width;
		height = next_height;
		return true;
	}
};

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
	/* T01: stable object ids backing the TABLE tab rows. */
	std::vector<object_id> table_rows;
	/* V01/V09: 共享 3D 渲染器、每个相机视口独立的 view 与 UI 原生纹理。 */
	phase0_renderer* renderer = nullptr;
	phase0_view* main_view = nullptr;
	phase0_view* preview_view = nullptr;
	native_view_texture viewport_texture;
	native_view_texture preview_texture;
	native_view_texture depth_texture;
	viewport_render_settings render_settings{};
	bool render_settings_initialized = false;
	/* V03: 视口里唯一可见的立方体对应的场景对象。 */
	/* V05/V07: gizmo drag snapshots (before = pre-drag, start = preview base). */
	std::map<object_id, editor_transform::transform> gizmo_before_transforms;
	std::map<object_id, editor_transform::transform> gizmo_start_transforms;
	/* V06: host-side drop preview resolved from the viewport hover. */
	bool drop_preview_active = false;
	camera_vec3 drop_preview_position{};
	std::string drop_preview_asset;
	std::string drop_preview_type;
	/* V08: the canvas uses a fixed 20-unit grid, independent of the 3D
	 * viewport's camera-dependent snap cell. */
	snap_settings canvas_snap;

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

	/* A04: 资产拖入场景 -> 新建叶子对象,返回快照供命令重做。
	 * V06: 视口落点会传入世界位置,并让对象进入渲染/拾取集合。 */
	object_id add_asset_object(const std::string& asset, object_id parent,
		std::size_t index, scene_object* out_snapshot,
		const camera_vec3* position = nullptr)
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
		if (position)
		{
			scene.objects[id].position = *position;
			scene.objects[id].renderable = true;
		}
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
		refresh_table_rows();
	}

	void refresh_table_rows()
	{
		table_rows.clear();
		table_rows.reserve(scene.objects.size());
		for (const auto& entry : scene.objects)
			table_rows.push_back(entry.first);
		if (shell)
			shell->table().refresh();
	}

	scene_object* table_object(std::size_t row)
	{
		if (row >= table_rows.size())
			return nullptr;
		auto found = scene.objects.find(table_rows[row]);
		return found == scene.objects.end() ? nullptr : &found->second;
	}

	/* T01: the object list doubles as the demo DataGrid model.  Values are
	 * pulled lazily per visible row and edits go through typed cell editors
	 * shared with PropertyGrid. */
	void configure_table()
	{
		if (!shell)
			return;
		table_view& table = shell->table();
		table_source source;
		source.row_count = [this]() { return table_rows.size(); };
		source.row_id = [this](std::size_t row) {
			return row < table_rows.size() ? table_rows[row] : invalid_object;
		};
		source.cell_text = [this](std::size_t row,
			const std::string& column) -> std::string {
			scene_object* object = table_object(row);
			if (!object)
				return {};
			if (column == "name")
				return object->name;
			if (column == "parent")
			{
				auto parent = scene.objects.find(object->parent);
				return object->parent == invalid_object || parent == scene.objects.end()
					? std::string("-") : parent->second.name;
			}
			if (column == "visible")
				return object->visible ? "true" : "false";
			if (column == "locked")
				return object->locked ? "true" : "false";
			if (column == "position")
			{
				vector_editor::vector_value value{};
				value[0] = object->position.x;
				value[1] = object->position.y;
				value[2] = object->position.z;
				return vector_editor::format_value(value, 3);
			}
			if (column == "tint")
			{
				auto tint = object->props.find("tint");
				return tint == object->props.end() ? std::string("#9FB0C4")
					: tint->second;
			}
			if (column == "mesh")
			{
				auto mesh = object->props.find("mesh");
				if (mesh != object->props.end())
					return mesh->second;
				auto asset = object->props.find("asset");
				return asset == object->props.end() ? std::string()
					: asset->second;
			}
			return {};
		};
		table.set_source(source);

		std::vector<table_column> columns(7);
		columns[0].id = "name";
		columns[0].label = "Name";
		columns[0].width = 160.0f;
		columns[0].editor.type = property_type::text;
		columns[0].set_value = [this](std::size_t row,
			const std::string& value) {
			scene_object* object = table_object(row);
			if (!object || value.empty())
				return false;
			object->name = value;
			rebuild_outliner();
			return true;
		};

		columns[1].id = "parent";
		columns[1].label = "Parent";
		columns[1].width = 120.0f;
		columns[1].editable = false;
		columns[1].editor.type = property_type::text;

		columns[2].id = "visible";
		columns[2].label = "Visible";
		columns[2].width = 80.0f;
		columns[2].editor.type = property_type::boolean;
		columns[2].set_value = [this](std::size_t row,
			const std::string& value) {
			scene_object* object = table_object(row);
			if (!object)
				return false;
			object->visible = value == "true";
			rebuild_outliner();
			return true;
		};

		columns[3].id = "locked";
		columns[3].label = "Locked";
		columns[3].width = 80.0f;
		columns[3].editor.type = property_type::boolean;
		columns[3].set_value = [this](std::size_t row,
			const std::string& value) {
			scene_object* object = table_object(row);
			if (!object)
				return false;
			object->locked = value == "true";
			rebuild_outliner();
			return true;
		};

		columns[4].id = "position";
		columns[4].label = "Position";
		columns[4].width = 180.0f;
		columns[4].editor.type = property_type::vector3;
		columns[4].editor.semantic = vector_semantic::position;
		columns[4].set_value = [this](std::size_t row,
			const std::string& value) {
			scene_object* object = table_object(row);
			vector_editor::vector_value parsed{};
			if (!object ||
				!vector_editor::parse_value(value, 3, parsed))
				return false;
			object->position = { (float)parsed[0], (float)parsed[1],
				(float)parsed[2] };
			return true;
		};

		columns[5].id = "tint";
		columns[5].label = "Tint";
		columns[5].width = 110.0f;
		columns[5].editor.type = property_type::color;
		columns[5].set_value = [this](std::size_t row,
			const std::string& value) {
			scene_object* object = table_object(row);
			if (!object)
				return false;
			object->props["tint"] = value;
			return true;
		};

		columns[6].id = "mesh";
		columns[6].label = "Mesh / Asset";
		columns[6].width = 170.0f;
		columns[6].editor.type = property_type::text;
		columns[6].set_value = [this](std::size_t row,
			const std::string& value) {
			scene_object* object = table_object(row);
			if (!object)
				return false;
			object->props["mesh"] = value;
			return true;
		};

		table.set_columns(std::move(columns));
		table.set_edit_listener([this](const table_cell& cell,
			const std::string& value) {
			if (shell)
				shell->set_status("table row " + std::to_string(cell.row + 1) +
					" = " + value);
		});
	}

	/* T02: animation timeline demo.  The timeline owns tracks/keyframes while
	 * the host samples the playhead each frame and writes the animated transform
	 * back into the scene.  Playback is driven by the main loop tick(), which
	 * also proves playhead synchronization with the external clock. */
	void configure_timeline(object_id cube)
	{
		if (!shell)
			return;
		timeline_view& timeline = shell->timeline();

		timeline_track height;
		height.name = "Cube Height";
		height.color = 0xFF5FA8FF;
		height.keyframes = {
			{ 0, 0.0, 1.0, timeline_interpolation::smooth },
			{ 0, 1.0, 2.5, timeline_interpolation::smooth },
			{ 0, 2.0, 1.0, timeline_interpolation::smooth },
			{ 0, 3.0, 2.5, timeline_interpolation::smooth }
		};
		const object_id height_track = timeline.add_track(height);

		timeline_track yaw;
		yaw.name = "Cube Yaw";
		yaw.color = 0xFFD9A441;
		yaw.keyframes = {
			{ 0, 0.0, 0.55, timeline_interpolation::linear },
			{ 0, 3.0, 6.833185, timeline_interpolation::linear }
		};
		const object_id yaw_track = timeline.add_track(yaw);

		timeline.set_duration(3.0);
		timeline.set_frame_rate(30.0);
		timeline.set_loop(true);
		timeline.set_playhead(0.0);
		timeline.set_selected_track(height_track);
		timeline.set_selection_listener([this](const std::vector<object_id>& ids) {
			if (!shell)
				return;
			shell->set_status(ids.empty()
				? "timeline: click or marquee keyframes"
				: "timeline: " + std::to_string(ids.size()) +
					" keyframe(s) selected");
		});
		timeline.set_playhead_listener([this, cube, height_track,
			yaw_track](double time) {
			auto found = scene.objects.find(cube);
			if (found == scene.objects.end() || !shell)
				return;
			timeline_view& view = shell->timeline();
			found->second.position.y =
				(float)view.evaluate(height_track, time);
			found->second.rotation = glm::angleAxis(
				(float)view.evaluate(yaw_track, time),
				glm::vec3(0.0f, 1.0f, 0.0f));
		});
		timeline.play();
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
	if (shell)
		shell->refresh_commands();
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
	shell->console().append(console_view::level::info,
		"imported " + std::to_string(imported.size()) + " asset(s) into " +
			path_id, "assets");

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

//---------------------------------------------------------------------------
// V03: 示例级拾取——射线/AABB/投影数学留在宿主，kit 只负责手势与选择同步
//---------------------------------------------------------------------------

static glm::vec3 to_glm(const camera_vec3& value)
{
	return glm::vec3(value.x, value.y, value.z);
}

static camera_vec3 from_glm(const glm::vec3& value)
{
	return { value.x, value.y, value.z };
}

static const scene_object* find_scene_object(object_id id)
{
	auto it = g.scene.objects.find(id);
	return it == g.scene.objects.end() ? nullptr : &it->second;
}

static editor_transform::transform object_transform(const scene_object& object)
{
	editor_transform::transform transform;
	transform.position = object.position;
	transform.rotation = object.rotation;
	transform.scale = object.scale;
	return transform;
}

static void write_object_transform(scene_object& object,
	const editor_transform::transform& transform)
{
	object.position = transform.position;
	object.rotation = transform.rotation;
	object.scale = transform.scale;
}

static void content_size(int* width, int* height)
{
	int w = 1, h = 1;
	if (g.shell)
	{
		float cx = 0, cy = 0, cw = 0, ch = 0;
		g.shell->viewport().get_content_rect(&cx, &cy, &cw, &ch);
		w = std::max(1, (int)std::lround(cw));
		h = std::max(1, (int)std::lround(ch));
	}
	if (width)
		*width = w;
	if (height)
		*height = h;
}

/* Content-local screen point -> world ray.  V03/V05 share the same camera
 * math; only the input structs differ. */
static bool screen_ray(float x, float y, int width, int height,
	glm::vec3* origin, glm::vec3* direction)
{
	if (!g.shell || !origin || !direction || width <= 0 || height <= 0)
		return false;
	const viewport_camera& camera = g.shell->camera();
	const camera_state state = camera.get_state();
	const float nx = (x / (float)width) * 2.0f - 1.0f;
	const float ny = 1.0f - (y / (float)height) * 2.0f;
	const float tan_half = std::tan(glm::radians(state.fov_deg) * 0.5f);
	const float aspect = (float)width / (float)height;
	*origin = to_glm(camera.position());
	*direction = glm::normalize(to_glm(camera.forward()) +
		to_glm(camera.right()) * (nx * tan_half * aspect) +
		to_glm(camera.up()) * (ny * tan_half));
	return true;
}

static bool viewport_ray(const selection_point_query& query, glm::vec3* origin,
	glm::vec3* direction)
{
	return screen_ray(query.x, query.y, query.width, query.height, origin,
		direction);
}

/* Ray/box in the object's local space, so rotated/scaled cubes still pick
 * accurately.  Returns the entry distance for nearest-hit ordering. */
static bool ray_box_distance(const glm::vec3& origin,
	const glm::vec3& direction, const editor_transform::transform& transform,
	float* out_distance)
{
	constexpr float kHalfExtent = 1.0f;
	const glm::mat4 inverse = glm::inverse(
		editor_transform::matrix(transform));
	const glm::vec3 local_origin = glm::vec3(inverse *
		glm::vec4(origin, 1.0f));
	const glm::vec3 local_direction = glm::vec3(inverse *
		glm::vec4(direction, 0.0f));

	float t_min = -1e30f;
	float t_max = 1e30f;
	for (int axis = 0; axis < 3; ++axis)
	{
		const float o = local_origin[axis];
		const float d = local_direction[axis];
		if (std::fabs(d) < 1e-8f)
		{
			if (o < -kHalfExtent || o > kHalfExtent)
				return false;
			continue;
		}
		float t1 = (-kHalfExtent - o) / d;
		float t2 = (kHalfExtent - o) / d;
		if (t1 > t2)
			std::swap(t1, t2);
		t_min = std::max(t_min, t1);
		t_max = std::min(t_max, t2);
		if (t_min > t_max)
			return false;
	}
	if (t_max < 0.0f)
		return false;
	if (out_distance)
		*out_distance = t_min > 0.0f ? t_min : 0.0f;
	return true;
}

static bool camera_matrices(const camera_state& state, int width, int height,
	glm::mat4* view, glm::mat4* projection)
{
	if (!g.shell || !view || !projection || width <= 0 || height <= 0)
		return false;
	const viewport_camera& camera = g.shell->camera();
	*view = glm::lookAt(to_glm(camera.position()), to_glm(state.target),
		glm::vec3(0.0f, 1.0f, 0.0f));
	*projection = glm::perspectiveRH_ZO(glm::radians(state.fov_deg),
		(float)width / (float)height, 0.1f, 100.0f);
	return true;
}

static bool project_screen(const glm::mat4& mvp, int width, int height,
	const glm::vec3& world, float* sx, float* sy)
{
	if (!sx || !sy || width <= 0 || height <= 0)
		return false;
	const glm::vec4 clip = mvp * glm::vec4(world, 1.0f);
	if (clip.w <= 1e-4f)
		return false;
	*sx = ((clip.x / clip.w) * 0.5f + 0.5f) * (float)width;
	*sy = (1.0f - ((clip.y / clip.w) * 0.5f + 0.5f)) * (float)height;
	return std::isfinite(*sx) && std::isfinite(*sy);
}

static bool project_box(const camera_state& state, int width, int height,
	const editor_transform::transform& transform, glm::vec4* out)
{
	if (!out)
		return false;
	glm::mat4 view(1.0f), projection(1.0f);
	if (!camera_matrices(state, width, height, &view, &projection))
		return false;
	const glm::mat4 mvp = projection * view;
	const glm::mat4 model = editor_transform::matrix(transform);
	constexpr float kHalfExtent = 1.0f;
	float min_x = 1e30f, min_y = 1e30f;
	float max_x = -1e30f, max_y = -1e30f;
	for (int i = 0; i < 8; ++i)
	{
		const glm::vec3 corner((i & 1) ? kHalfExtent : -kHalfExtent,
			(i & 2) ? kHalfExtent : -kHalfExtent,
			(i & 4) ? kHalfExtent : -kHalfExtent);
		const glm::vec3 world = glm::vec3(model * glm::vec4(corner, 1.0f));
		float sx = 0.0f, sy = 0.0f;
		if (!project_screen(mvp, width, height, world, &sx, &sy))
			return false;
		min_x = std::min(min_x, sx);
		max_x = std::max(max_x, sx);
		min_y = std::min(min_y, sy);
		max_y = std::max(max_y, sy);
	}
	*out = glm::vec4(min_x, min_y, max_x, max_y);
	return true;
}

static bool project_world(const camera_vec3& world, float* x, float* y)
{
	if (!g.shell)
		return false;
	int width = 0, height = 0;
	content_size(&width, &height);
	const camera_state state = g.shell->camera().get_state();
	glm::mat4 view(1.0f), projection(1.0f);
	if (!camera_matrices(state, width, height, &view, &projection))
		return false;
	return project_screen(projection * view, width, height, to_glm(world),
		x, y);
}

static bool gizmo_ray_at(float x, float y, gizmo_ray* out)
{
	if (!out)
		return false;
	int width = 0, height = 0;
	content_size(&width, &height);
	glm::vec3 origin(0.0f), direction(0.0f);
	if (!screen_ray(x, y, width, height, &origin, &direction))
		return false;
	out->origin = from_glm(origin);
	out->direction = from_glm(direction);
	return true;
}

/* V06: absolute cursor point -> ground-plane drop position.  The returned
 * point is the object centre one unit above the grid plane, so the cube proxy
 * sits on the ground. */
static bool viewport_drop_point(float x, float y, camera_vec3* out)
{
	if (!g.shell || !out)
		return false;
	float wx = 0, wy = 0, ww = 0, wh = 0;
	vgui_widget_get_rect(g.shell->viewport().widget(), &wx, &wy, &ww, &wh);
	(void)ww;
	(void)wh;
	float ox = 1.0f, oy = 1.0f, cw = 0.0f, ch = 0.0f;
	g.shell->viewport().get_content_rect(&ox, &oy, &cw, &ch);
	const int width = std::max(1, (int)std::lround(cw));
	const int height = std::max(1, (int)std::lround(ch));
	glm::vec3 origin(0.0f), direction(0.0f);
	if (!screen_ray(x - wx - ox, y - wy - oy, width, height, &origin,
		&direction))
		return false;
	const float plane_y = g.shell->grid().plane_height();
	if (!(direction.y < -1e-4f))
		return false;
	const float t = (plane_y - origin.y) / direction.y;
	if (!(t > 0.0f) || !std::isfinite(t))
		return false;
	const glm::vec3 hit = origin + direction * t;
	if (!std::isfinite(hit.x) || !std::isfinite(hit.z))
		return false;
	*out = { hit.x, plane_y + 1.0f, hit.z };
	return true;
}

static void update_drop_preview()
{
	const std::string previous_asset = g.drop_preview_asset;
	g.drop_preview_active = false;
	g.drop_preview_asset.clear();
	g.drop_preview_type.clear();
	if (!g.shell)
		return;
	float x = 0.0f, y = 0.0f;
	drag_payload payload;
	if (!g.shell->viewport().drop_hover(&x, &y, &payload) ||
		payload.kind != drag_payload_kind::asset)
		return;
	const asset_info* info = g.find_asset(payload.id);
	camera_vec3 point;
	if (!info || !viewport_drop_point(x, y, &point))
		return;
	if (previous_asset != payload.id)
		g.shell->set_status("drop " + payload.id + " (" + info->type + ")");
	g.drop_preview_active = true;
	g.drop_preview_position = point;
	g.drop_preview_asset = payload.id;
	g.drop_preview_type = info->type;
}

static object_id pick_object_at(const selection_point_query& query)
{
	if (!g.shell)
		return invalid_object;
	glm::vec3 origin(0.0f);
	glm::vec3 direction(0.0f);
	if (!viewport_ray(query, &origin, &direction))
		return invalid_object;

	object_id best = invalid_object;
	float best_distance = 1e30f;
	for (const auto& entry : g.scene.objects)
	{
		const scene_object& object = entry.second;
		if (!object.renderable || !object.visible)
			continue;
		float distance = 0.0f;
		if (ray_box_distance(origin, direction, object_transform(object),
			&distance) && distance < best_distance)
		{
			best = entry.first;
			best_distance = distance;
		}
	}
	if (best != invalid_object)
		g.shell->set_status("picked " + g.scene.objects[best].name);
	return best;
}

static std::vector<object_id> pick_objects_in_box(const selection_box_query& query)
{
	std::vector<object_id> out;
	if (!g.shell)
		return out;
	const camera_state state = g.shell->camera().get_state();
	for (const auto& entry : g.scene.objects)
	{
		const scene_object& object = entry.second;
		if (!object.renderable || !object.visible)
			continue;
		glm::vec4 box(0.0f);
		if (!project_box(state, query.width, query.height,
			object_transform(object), &box))
			continue;
		if (box.x >= query.x1 || box.z <= query.x0 ||
			box.y >= query.y1 || box.w <= query.y0)
			continue;
		out.push_back(entry.first);
	}
	if (!out.empty())
		g.shell->set_status("box selected " + std::to_string(out.size()) +
			" object(s)");
	return out;
}

static std::vector<selection_box> highlight_selection(
	const std::vector<object_id>& ids)
{
	std::vector<selection_box> out;
	if (!g.shell)
		return out;
	int width = 0, height = 0;
	content_size(&width, &height);
	const camera_state state = g.shell->camera().get_state();
	for (object_id id : ids)
	{
		const scene_object* object = find_scene_object(id);
		if (!object || !object->renderable || !object->visible)
			continue;
		glm::vec4 box(0.0f);
		if (!project_box(state, width, height, object_transform(*object),
			&box))
			continue;
		selection_box item;
		item.id = id;
		item.x = box.x;
		item.y = box.y;
		item.width = box.z - box.x;
		item.height = box.w - box.y;
		out.push_back(item);
	}
	return out;
}

//---------------------------------------------------------------------------
// V05/V07: 平移/旋转/缩放 Gizmo——kit 负责手势/绘制，宿主提供帧、投影、
// 射线与命令栈；所有变换结果写回同一份场景数据，属性面板只读取该数据。
//---------------------------------------------------------------------------

static gizmo_frame gizmo_frame_for_selection()
{
	gizmo_frame frame;
	if (!g.shell || !g.context)
		return frame;
	const std::vector<object_id>& ids = g.context->selection_ids();
	if (ids.empty())
		return frame;

	camera_vec3 center{};
	int count = 0;
	for (object_id id : ids)
	{
		const scene_object* object = find_scene_object(id);
		if (!object)
			continue;
		center.x += object->position.x;
		center.y += object->position.y;
		center.z += object->position.z;
		++count;
	}
	if (!count)
		return frame;
	center.x /= (float)count;
	center.y /= (float)count;
	center.z /= (float)count;

	object_id primary = g.context->selection() != invalid_object
		? g.context->selection() : ids.front();
	const scene_object* primary_object = find_scene_object(primary);
	const gizmo_space space = g.shell->gizmo().space();

	frame.valid = true;
	frame.origin = center;
	frame.screen_length = 80.0f;

	if (space == gizmo_space::screen)
	{
		/* Screen space: the rings face the camera; axis[2] is the view
		 * direction (back), keeping right/up/back right-handed. */
		const viewport_camera& camera = g.shell->camera();
		frame.axis[0] = camera.right();
		frame.axis[1] = camera.up();
		frame.axis[2] = from_glm(-to_glm(camera.forward()));
		return frame;
	}

	glm::quat orientation(1.0f, 0.0f, 0.0f, 0.0f);
	if (space == gizmo_space::local && primary_object)
		orientation = primary_object->rotation;
	else if (space == gizmo_space::parent && primary_object &&
		primary_object->parent != invalid_object)
	{
		if (const scene_object* parent =
			find_scene_object(primary_object->parent))
			orientation = parent->rotation;
	}
	orientation = glm::normalize(orientation);
	frame.axis[0] = from_glm(orientation * glm::vec3(1.0f, 0.0f, 0.0f));
	frame.axis[1] = from_glm(orientation * glm::vec3(0.0f, 1.0f, 0.0f));
	frame.axis[2] = from_glm(orientation * glm::vec3(0.0f, 0.0f, 1.0f));
	return frame;
}

static std::map<object_id, editor_transform::transform>
capture_selected_transforms()
{
	std::map<object_id, editor_transform::transform> out;
	if (!g.context)
		return out;
	for (object_id id : g.context->selection_ids())
	{
		const scene_object* object = find_scene_object(id);
		if (object)
			out[id] = object_transform(*object);
	}
	return out;
}

static void apply_transform_map(
	const std::map<object_id, editor_transform::transform>& transforms)
{
	for (const auto& entry : transforms)
	{
		auto it = g.scene.objects.find(entry.first);
		if (it != g.scene.objects.end())
			write_object_transform(it->second, entry.second);
	}
	if (g.shell)
		g.shell->properties().refresh();
}

static void apply_gizmo_delta(
	const std::map<object_id, editor_transform::transform>& base,
	const gizmo_transform& delta)
{
	for (const auto& entry : base)
	{
		auto it = g.scene.objects.find(entry.first);
		if (it == g.scene.objects.end())
			continue;
		editor_transform::transform transform = entry.second;
		editor_transform::apply(delta, &transform);
		write_object_transform(it->second, transform);
	}
	if (g.shell)
		g.shell->properties().refresh();
}

static const char* gizmo_axis_name(gizmo_handle handle)
{
	switch (handle)
	{
	case gizmo_handle::rotate_x:
	case gizmo_handle::scale_x:
	case gizmo_handle::axis_x:
	case gizmo_handle::plane_yz:
		return "X";
	case gizmo_handle::rotate_y:
	case gizmo_handle::scale_y:
	case gizmo_handle::axis_y:
	case gizmo_handle::plane_zx:
		return "Y";
	case gizmo_handle::rotate_z:
	case gizmo_handle::scale_z:
	case gizmo_handle::axis_z:
	case gizmo_handle::plane_xy:
		return "Z";
	default:
		return "uniform";
	}
}

static void report_gizmo_commit(const gizmo_transform& delta)
{
	if (!g.shell)
		return;
	char text[160] = {};
	switch (delta.mode)
	{
	case gizmo_mode::translate:
		std::snprintf(text, sizeof(text),
			"translated %.2f %.2f %.2f", delta.translation.x,
			delta.translation.y, delta.translation.z);
		break;
	case gizmo_mode::rotate:
	{
		const float angle = std::sqrt(delta.rotation.x * delta.rotation.x +
			delta.rotation.y * delta.rotation.y +
			delta.rotation.z * delta.rotation.z) * 57.2957795f;
		std::snprintf(text, sizeof(text), "rotated %.1f deg around %s",
			angle, gizmo_axis_name(delta.handle));
		break;
	}
	case gizmo_mode::scale:
		if (delta.handle == gizmo_handle::scale_uniform)
			std::snprintf(text, sizeof(text), "scaled uniformly %.3f",
				delta.scale.x);
		else
			std::snprintf(text, sizeof(text), "scaled %s %.3f",
				gizmo_axis_name(delta.handle),
				delta.handle == gizmo_handle::scale_x ? delta.scale.x
					: delta.handle == gizmo_handle::scale_y ? delta.scale.y
					: delta.scale.z);
		break;
	}
	g.shell->set_status(text);
}

static void setup_gizmo()
{
	if (!g.shell || !g.context)
		return;
	viewport_gizmo& gizmo = g.shell->gizmo();
	gizmo.set_frame_provider(&gizmo_frame_for_selection);
	gizmo.set_projection(&project_world);
	gizmo.set_ray_provider(&gizmo_ray_at);
	gizmo.set_transform_handler([](gizmo_phase phase,
		const gizmo_transform& delta)
	{
		if (!g.shell || !g.context)
			return;
		switch (phase)
		{
		case gizmo_phase::begin:
			g.gizmo_before_transforms = capture_selected_transforms();
			g.gizmo_start_transforms = g.gizmo_before_transforms;
			break;
		case gizmo_phase::preview:
			apply_gizmo_delta(g.gizmo_start_transforms, delta);
			break;
		case gizmo_phase::commit:
		{
			apply_gizmo_delta(g.gizmo_start_transforms, delta);
			if (editor_transform::changed(delta) &&
				!g.gizmo_before_transforms.empty())
			{
				std::map<object_id, editor_transform::transform> before =
					g.gizmo_before_transforms;
				std::map<object_id, editor_transform::transform> after =
					capture_selected_transforms();
				const char* name = delta.mode == gizmo_mode::rotate
					? "Rotate"
					: delta.mode == gizmo_mode::scale ? "Scale"
					: "Translate";
				g.context->commands().push(
					std::make_unique<function_command>(name,
						[after]() { apply_transform_map(after); },
						[before]() { apply_transform_map(before); }));
			}
			report_gizmo_commit(delta);
			/* The command was pushed directly, so refresh Undo/Redo enabled
			 * state in the toolbar and menus. */
			g.shell->refresh_commands();
			break;
		}
		case gizmo_phase::cancel:
			apply_transform_map(g.gizmo_before_transforms);
			g.shell->set_status("gizmo cancelled");
			break;
		}
	});
}

static void setup_editor(vgui_context* ui)
{
	g.ui = ui;
	g.context = new editor_context(ui);
	g.shell = new editor_shell(*g.context);
	g.shell->build();
	g.shell->preview().set_load_handler(&load_demo_preview);

	/* V01: 视口表面——尺寸变化与跨边界拖拽/失焦取消的状态反馈。真实 3D FBO
	 * 由宿主按 resize 回调重建（见 phase0 示例），这里先验证控件的交互边界。 */
	g.shell->viewport().set_resize_listener([](int w, int h) {
		if (g.shell)
			g.shell->set_status("viewport surface " + std::to_string(w) +
				"x" + std::to_string(h));
	});
	g.shell->viewport().set_event_listener([](const vgui_event* event) -> bool {
		if (!g.shell || !event)
			return false;
		if (event->type == VGUI_EVENT_MOUSE_DOWN)
			g.shell->set_status("viewport: MMB pan / RMB orbit / wheel zoom / F frame");
		else if (event->type == VGUI_EVENT_CANCEL)
			g.shell->set_status("viewport: gesture cancelled (focus lost)");
		return false;
	});

	/* V02: 相机导航——F 取景当前选中对象；状态栏跟随相机状态。 */
	g.shell->camera().set_focus_provider([](camera_focus* out) -> bool {
		if (!out || !g.valid_selection())
			return false;
		const scene_object* object =
			find_scene_object(g.context->selection());
		if (!object)
			return false;
		out->center = object->position;
		const float extent = std::max({ object->scale.x, object->scale.y,
			object->scale.z, 0.25f });
		out->radius = 2.5f * extent;
		g.shell->set_status("camera: frame " + object->name);
		return true;
	});
	g.shell->camera().set_change_listener([] {
		if (!g.shell)
			return;
		const camera_state state = g.shell->camera().get_state();
		char text[128] = {};
		std::snprintf(text, sizeof(text),
			"camera yaw %.2f pitch %.2f dist %.2f",
			state.yaw, state.pitch, state.distance);
		g.shell->set_status(text);
	});

	/* V09: 第二视口从俯视角度观察同一场景，并拥有独立的轨道相机。 */
	{
		camera_state state = g.shell->preview_camera().get_state();
		state.yaw = 0.0f;
		state.pitch = 1.45f;
		state.distance = 9.0f;
		g.shell->preview_camera().set_state(state);
	}
	g.shell->preview_camera().set_change_listener([] {
		if (!g.shell)
			return;
		const camera_state state = g.shell->preview_camera().get_state();
		char text[128] = {};
		std::snprintf(text, sizeof(text),
			"preview camera yaw %.2f pitch %.2f dist %.2f",
			state.yaw, state.pitch, state.distance);
		g.shell->set_status(text);
	});

	/* V03: 左键拾取/框选与屏幕空间选中高亮；几何求交留在宿主。 */
	g.shell->picking().set_point_picker(&pick_object_at);
	g.shell->picking().set_box_picker(&pick_objects_in_box);
	g.shell->picking().set_highlight_provider(&highlight_selection);
	/* V05: 平移 Gizmo 的帧/投影/射线与命令栈接线。 */
	setup_gizmo();

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
	/* V03: 视口里唯一可见的立方体对应一个可拾取对象。 */
	const object_id cube = g.scene.add("Cube", invalid_object);
	g.scene.objects[cube].props["mesh"] = "Cube.mesh";
	/* V05/V07: 立方体落在网格上，并带一点偏航角以区分局部/世界空间。 */
	g.scene.objects[cube].position = { 0.0f, 1.0f, 0.0f };
	g.scene.objects[cube].rotation = glm::angleAxis(0.55f,
		glm::vec3(0.0f, 1.0f, 0.0f));
	/* V06: 渲染/拾取集合由 renderable 标记驱动。 */
	g.scene.objects[cube].renderable = true;
	g.configure_table();
	g.configure_timeline(cube);
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
	/* V03: 监听完整选择集——视口框选/Outliner 多选都走同一条路径。 */
	g.context->add_selection_set_listener([](const std::vector<object_id>& selection)
	{
		property_grid& grid = g.shell->properties();
		grid.clear();
		const object_id id = g.context->selection();

		std::vector<object_id> targets = selection;
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

		/* V07: Transform rows read the same scene state the gizmo writes, so a
		 * gizmo drag and a numeric edit converge on identical values. */
		property_desc position;
		position.id = "position";
		position.label = "Position";
		position.type = property_type::vector3;
		position.semantic = vector_semantic::position;
		position.value = editor_transform::vec3_text(o.position);
		position.category = "Transform";
		position.get_value = [oid = id]() -> std::string {
			auto it = g.scene.objects.find(oid);
			return it == g.scene.objects.end() ? std::string()
				: editor_transform::vec3_text(it->second.position);
		};
		position.set_value = [oid = id](const std::string& value) {
			camera_vec3 parsed;
			auto it = g.scene.objects.find(oid);
			if (it != g.scene.objects.end() &&
				editor_transform::parse_vec3(value, &parsed))
				it->second.position = parsed;
		};
		grid.add_property(position);

		property_desc rotation;
		rotation.id = "rotation";
		rotation.label = "Rotation";
		rotation.type = property_type::vector3;
		rotation.value = editor_transform::euler_text(o.rotation);
		rotation.category = "Transform";
		rotation.tooltip = "Euler degrees (XYZ)";
		rotation.get_value = [oid = id]() -> std::string {
			auto it = g.scene.objects.find(oid);
			return it == g.scene.objects.end() ? std::string()
				: editor_transform::euler_text(it->second.rotation);
		};
		rotation.set_value = [oid = id](const std::string& value) {
			camera_vec3 parsed;
			auto it = g.scene.objects.find(oid);
			if (it != g.scene.objects.end() &&
				editor_transform::parse_vec3(value, &parsed))
				it->second.rotation =
					editor_transform::from_euler_degrees(parsed);
		};
		grid.add_property(rotation);

		property_desc scale;
		scale.id = "scale";
		scale.label = "Scale";
		scale.type = property_type::vector3;
		scale.value = editor_transform::vec3_text(o.scale);
		scale.category = "Transform";
		scale.tooltip = "Per-axis scale";
		scale.get_value = [oid = id]() -> std::string {
			auto it = g.scene.objects.find(oid);
			return it == g.scene.objects.end() ? std::string()
				: editor_transform::vec3_text(it->second.scale);
		};
		scale.set_value = [oid = id](const std::string& value) {
			camera_vec3 parsed;
			auto it = g.scene.objects.find(oid);
			if (it != g.scene.objects.end() &&
				editor_transform::parse_vec3(value, &parsed))
			{
				it->second.scale = { editor_transform::clamp_scale(parsed.x),
					editor_transform::clamp_scale(parsed.y),
					editor_transform::clamp_scale(parsed.z) };
			}
		};
		grid.add_property(scale);

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

		/* 多目标 getter/setter:Transform 读场景字段,其余值存在对象 props
		 * 里(name 特殊)。 */
		grid.set_multi_getter([](const std::string& pid, object_id obj)
			-> std::string
		{
			auto it = g.scene.objects.find(obj);
			if (it == g.scene.objects.end())
				return "";
			if (pid == "name")
				return it->second.name;
			if (pid == "position")
				return editor_transform::vec3_text(it->second.position);
			if (pid == "rotation")
				return editor_transform::euler_text(it->second.rotation);
			if (pid == "scale")
				return editor_transform::vec3_text(it->second.scale);
			auto pit = it->second.props.find(pid);
			return pit == it->second.props.end() ? "" : pit->second;
		});
		grid.set_multi_setter([](const std::string& pid, object_id obj,
			const std::string& value)
		{
			auto it = g.scene.objects.find(obj);
			if (it == g.scene.objects.end())
				return;
			if (pid == "position" || pid == "rotation" || pid == "scale")
			{
				camera_vec3 parsed;
				if (!editor_transform::parse_vec3(value, &parsed))
					return;
				if (pid == "position")
					it->second.position = parsed;
				else if (pid == "rotation")
					it->second.rotation =
						editor_transform::from_euler_degrees(parsed);
				else
					it->second.scale = {
						editor_transform::clamp_scale(parsed.x),
						editor_transform::clamp_scale(parsed.y),
						editor_transform::clamp_scale(parsed.z) };
				return;
			}
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
			if (pid == "position" || pid == "rotation" || pid == "scale" ||
				pid == "mesh" || pid == "target" || pid == "tint" ||
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
		float x, float y) -> bool
	{
		const asset_info* info = g.find_asset(payload.id);
		if (payload.kind != drag_payload_kind::asset || !info)
			return false;
		camera_vec3 point;
		if (!viewport_drop_point(x, y, &point))
			return false;
		object_id parent = g.context->selection();
		if (!g.scene.objects.count(parent))
			parent = invalid_object;
		const std::size_t index = parent == invalid_object
			? g.scene.roots.size() : g.scene.objects[parent].children.size();
		scene_object snapshot;
		const object_id created = g.add_asset_object(payload.id, parent, index,
			&snapshot, &point);
		g.rebuild_outliner();
		g.context->set_selection(created);
		g.push_create_command(snapshot, index);
		g.drop_preview_active = false;
		char text[192] = {};
		std::snprintf(text, sizeof(text),
			"created %s (%s) at %.2f %.2f %.2f", snapshot.name.c_str(),
			info->type.c_str(), point.x, point.y, point.z);
		g.shell->set_status(text);
		g.shell->console().append(console_view::level::info, text,
			"viewport");
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
		g.shell->console().append(
			error == asset_browser::operation_error::none
				? console_view::level::info : console_view::level::warning,
			message, "assets");
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

	/* ---- A07 日志控制台:分级日志、堆栈折叠与错误跳转 ------------------- */
	console_view& console = g.shell->console();
	console.set_jump_handler([](const console_view::location& where) -> bool {
		if (where.asset_id != 0)
		{
			std::string path_id;
			asset_info info;
			if (g.locate_asset(where.asset_id, &path_id, &info))
			{
				if (!path_id.empty())
					(void)g.shell->assets().navigate_to(path_id);
				g.shell->assets().set_selection({ where.asset_id });
				g.shell->set_panel_visible(editor_panel::bottom, true);
				g.shell->set_status("console jump: " + info.name);
				return true;
			}
		}
		if (!where.path.empty())
		{
			std::string text = where.path;
			if (where.line > 0)
			{
				text += ":" + std::to_string(where.line);
				if (where.column > 0)
					text += ":" + std::to_string(where.column);
			}
			g.shell->set_status("console jump: " + text);
			return true;
		}
		return false;
	});
	console.append(console_view::level::trace, "console initialized",
		"console");
	console.append(console_view::level::debug,
		"producers queue on any thread; tick drains a bounded batch",
		"console");
	console.append(console_view::level::info,
		"editor ready: 7 demo assets, 4 asset folders", "app");
	console.append(console_view::level::warning,
		"Broken.mesh is intentionally corrupt for the A06 error demo",
		"assets");
	console_view::entry failure;
	failure.severity = console_view::level::error;
	failure.message = "Failed to decode Broken.mesh";
	failure.source = "renderer";
	if (const asset_info* broken = g.find_asset("Broken.mesh"))
		failure.where.asset_id = broken->id;
	failure.stack.push_back({ "decode_texture",
		{ 0, "assets/codec/png.cpp", 128, 9 } });
	failure.stack.push_back({ "load_asset",
		{ 0, "assets/asset_database.cpp", 87, 5 } });
	console.append(std::move(failure));
	console.poll();

	/* ---- V08: 独立 2D Canvas:UI 场景示例 ---------------------------------
	 * 画布拥有自己的轻量 item 模型;宿主只提供展示数据并把命令栈交给 shell。
	 * 拖动/缩放/旋转各提交一条命令,可直接用 Edit > Undo/Redo 验证。 */
	canvas_view& canvas = g.shell->canvas();
	g.canvas_snap.translate_step = 20.0f;
	g.canvas_snap.rotate_step_deg = 15.0f;
	canvas.set_snap_settings(&g.canvas_snap);
	canvas.set_grid_step(20.0f);
	canvas.set_zoom_limits(0.25f, 6.0f);
	canvas.reset_view();
	auto add_canvas_item = [&canvas](float x, float y, float w, float h,
		canvas_shape shape, vgui_color fill, vgui_color border,
		const char* label, float radius) {
		canvas_item item;
		item.bounds = { x, y, w, h };
		item.shape = shape;
		item.fill = fill;
		item.border = border;
		item.label = label;
		item.corner_radius = radius;
		return canvas.add_item(std::move(item));
	};
	const object_id canvas_backdrop = add_canvas_item(10.0f, 10.0f, 400.0f,
		290.0f, canvas_shape::rectangle, 0xFF141C26, 0xFF2E4054, "", 6.0f);
	canvas_item backdrop = *canvas.find_item(canvas_backdrop);
	backdrop.locked = true;
	canvas.update_item(backdrop);
	const object_id canvas_panel = add_canvas_item(40.0f, 40.0f, 220.0f,
		130.0f, canvas_shape::rectangle, 0xFF1E2A3A, 0xFF4A6A92,
		"Scene Panel", 10.0f);
	add_canvas_item(70.0f, 200.0f, 140.0f, 44.0f, canvas_shape::rectangle,
		0xFF2D6CDF, 0xFF6EA8FF, "Play", 8.0f);
	add_canvas_item(320.0f, 60.0f, 90.0f, 90.0f, canvas_shape::ellipse,
		0xFF3A2E5A, 0xFF9A7BE0, "Avatar", 0.0f);
	add_canvas_item(330.0f, 210.0f, 90.0f, 70.0f, canvas_shape::diamond,
		0xFF4A3A20, 0xFFD9A441, "Badge", 0.0f);

	canvas.set_selection_listener([](const std::vector<object_id>& ids) {
		if (!g.shell)
			return;
		if (ids.empty())
			g.shell->set_status("canvas: click an item or drag a marquee");
		else if (ids.size() == 1)
			g.shell->set_status("canvas: drag to move / handles resize / circle rotates");
		else
			g.shell->set_status("canvas: " + std::to_string(ids.size()) +
				" items selected");
	});
	canvas.set_selection({ canvas_panel });

	g.shell->set_status("V08: CANVAS tab drag/resize/rotate; V03 viewport: LMB pick / box / Esc");
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

	/* V01/V09: 视口离屏渲染器（失败时视口显示占位文本，编辑器仍可用）。
	 * 默认 view 给主相机，额外 view 给第二相机；两者共享着色器/管线/立方体
	 * 缓冲，但各自拥有相机、实例、网格与颜色/深度附件。 */
	g.renderer = phase0_renderer_create(wg->device);
	if (!g.renderer)
		fprintf(stderr, "3D 渲染器不可用，视口将显示占位内容\n");
	else
	{
		g.main_view = phase0_renderer_default_view(g.renderer);
		g.preview_view = phase0_renderer_create_view(g.renderer);
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
			/* A07: drain the console every frame even when its tab is hidden,
			 * so producers never accumulate an unbounded queue. */
			if (g.shell)
			{
				g.shell->console().tick(delta);
				/* T02: advance the timeline from the external frame clock; the
				 * playhead listener writes the sampled pose into the scene. */
				g.shell->timeline().tick(delta);
			}
			/* V01/V09: 布局完成后取每个相机视口的渲染尺寸，宿主在此原地重建
			 * 3D 附件并更新原生纹理；旧纹理在新纹理就绪前一直保持绑定，不会
			 * 闪空白。深度预览视口直接复用主视口的深度可视化纹理。 */
			if (g.shell)
			{
				g.shell->viewport().poll_resize();
				g.shell->preview_viewport().poll_resize();
			}
			/* V06: resolve the current asset hover into a world-space drop
			 * preview before building the frame. */
			update_drop_preview();
			if (g.renderer && g.shell)
			{
				int rw = 0, rh = 0;
				g.shell->viewport().get_render_size(&rw, &rh);
				int pw = 0, ph = 0;
				g.shell->preview_viewport().get_render_size(&pw, &ph);
				if (g.main_view && rw > 0 && rh > 0)
					phase0_view_resize(g.main_view, rw, rh);
				if (g.preview_view && pw > 0 && ph > 0)
					phase0_view_resize(g.preview_view, pw, ph);

				/* V09: runtime HDR / MSAA / depth preview.  Both camera views
				 * share pipelines for the same format/sample count, but keep
				 * independent attachments and camera state. */
				const viewport_render_settings requested =
					g.shell->viewport_settings();
				if (!g.render_settings_initialized ||
					requested.hdr != g.render_settings.hdr ||
					requested.msaa_samples != g.render_settings.msaa_samples ||
					requested.depth_preview != g.render_settings.depth_preview)
				{
					g.render_settings = requested;
					g.render_settings_initialized = true;

					phase0_view_options options =
						phase0_view_get_options(g.main_view);
					options.hdr = requested.hdr;
					options.msaa_samples = requested.msaa_samples;
					options.depth_preview = requested.depth_preview;
					phase0_view_set_options(g.main_view, &options);

					options = phase0_view_get_options(g.preview_view);
					options.hdr = requested.hdr;
					options.msaa_samples = requested.msaa_samples;
					options.depth_preview = false;
					phase0_view_set_options(g.preview_view, &options);

					/* The device may clamp MSAA; reflect the effective value so
					 * the menu check does not claim an unsupported mode. */
					const int effective_msaa =
						phase0_view_get_options(g.main_view).msaa_samples;
					if (effective_msaa != requested.msaa_samples)
					{
						viewport_render_settings actual =
							g.shell->viewport_settings();
						actual.msaa_samples = effective_msaa;
						g.shell->set_viewport_settings(actual);
					}
				}

				auto make_camera = [](const viewport_camera& source)
				{
					const camera_vec3 eye = source.position();
					const camera_state state = source.get_state();
					phase0_camera camera = {};
					camera.eye[0] = eye.x;
					camera.eye[1] = eye.y;
					camera.eye[2] = eye.z;
					camera.target[0] = state.target.x;
					camera.target[1] = state.target.y;
					camera.target[2] = state.target.z;
					camera.fov_deg = state.fov_deg;
					return camera;
				};
				const camera_state camera_view =
					g.shell->camera().get_state();
				const camera_state preview_camera_view =
					g.shell->preview_camera().get_state();
				const phase0_camera main_camera =
					make_camera(g.shell->camera());
				const phase0_camera preview_camera =
					make_camera(g.shell->preview_camera());
				phase0_view_set_camera(g.main_view, &main_camera);
				phase0_view_set_camera(g.preview_view, &preview_camera);

				/* V06: all renderable scene objects become cube proxies; the
				 * drop preview adds one translucent ghost instance.  Both
				 * cameras share the instance list but render it into their own
				 * attachments. */
				std::vector<phase0_instance> instances;
				for (const auto& entry : g.scene.objects)
				{
					const scene_object& object = entry.second;
					if (!object.renderable || !object.visible)
						continue;
					phase0_instance instance = {};
					instance.position[0] = object.position.x;
					instance.position[1] = object.position.y;
					instance.position[2] = object.position.z;
					const glm::quat rotation = glm::normalize(
						object.rotation);
					instance.rotation[0] = rotation.x;
					instance.rotation[1] = rotation.y;
					instance.rotation[2] = rotation.z;
					instance.rotation[3] = rotation.w;
					instance.scale[0] =
						editor_transform::clamp_scale(object.scale.x);
					instance.scale[1] =
						editor_transform::clamp_scale(object.scale.y);
					instance.scale[2] =
						editor_transform::clamp_scale(object.scale.z);
					instance.ghost = false;
					instances.push_back(instance);
				}
				if (g.drop_preview_active)
				{
					phase0_instance ghost = {};
					ghost.position[0] = g.drop_preview_position.x;
					ghost.position[1] = g.drop_preview_position.y;
					ghost.position[2] = g.drop_preview_position.z;
					ghost.rotation[3] = 1.0f;
					ghost.scale[0] = 1.0f;
					ghost.scale[1] = 1.0f;
					ghost.scale[2] = 1.0f;
					ghost.ghost = true;
					instances.push_back(ghost);
				}
				phase0_view_set_instances(g.main_view,
					instances.empty() ? nullptr : instances.data(),
					(int)instances.size());
				phase0_view_set_instances(g.preview_view,
					instances.empty() ? nullptr : instances.data(),
					(int)instances.size());

				/* V04: grid geometry (kit) -> renderer line quads (host).
				 * V06 appends a ground marker under the drop preview.  Each
				 * view receives its own grid buffer built for its camera. */
				auto append_drop_marker = [&](std::vector<viewport_grid_line>& lines)
				{
					if (!g.drop_preview_active)
						return;
					const float px = g.drop_preview_position.x;
					const float pz = g.drop_preview_position.z;
					const float y = g.shell->grid().plane_height() + 0.02f;
					const float half = 1.0f;
					const vgui_color color =
						editor_state::asset_accent_color(g.drop_preview_type);
					auto marker = [&](float ax, float az, float bx, float bz)
					{
						viewport_grid_line line;
						line.a = { ax, y, az };
						line.b = { bx, y, bz };
						line.color = color;
						line.width = 0.06f;
						lines.push_back(line);
					};
					marker(px - half, pz - half, px + half, pz - half);
					marker(px + half, pz - half, px + half, pz + half);
					marker(px + half, pz + half, px - half, pz + half);
					marker(px - half, pz + half, px - half, pz - half);
				};
				auto upload_grid = [](phase0_view* view,
					const std::vector<viewport_grid_line>& lines)
				{
					if (!view)
						return;
					if (lines.empty())
					{
						phase0_view_set_grid(view, nullptr, 0);
						return;
					}
					std::vector<phase0_grid_line> render_lines;
					render_lines.reserve(lines.size());
					for (const viewport_grid_line& line : lines)
					{
						phase0_grid_line out = {};
						out.a[0] = line.a.x;
						out.a[1] = line.a.y;
						out.a[2] = line.a.z;
						out.b[0] = line.b.x;
						out.b[1] = line.b.y;
						out.b[2] = line.b.z;
						/* vgui 0xAARRGGBB -> vertex colour 0xAABBGGRR. */
						const uint32_t c = line.color;
						out.color = (c & 0xFF00FF00u) |
							((c & 0x000000FFu) << 16) |
							((c >> 16) & 0x000000FFu);
						out.width = line.width;
						render_lines.push_back(out);
					}
					phase0_view_set_grid(view, render_lines.data(),
						(int)render_lines.size());
				};

				const int grid_w = rw > 0 ? rw : 1;
				const int grid_h = rh > 0 ? rh : 1;
				std::vector<viewport_grid_line> grid_lines;
				g.shell->grid().build(camera_view, grid_w, grid_h, &grid_lines);
				append_drop_marker(grid_lines);
				upload_grid(g.main_view, grid_lines);

				if (g.preview_view)
				{
					const int preview_grid_w = pw > 0 ? pw : 1;
					const int preview_grid_h = ph > 0 ? ph : 1;
					std::vector<viewport_grid_line> preview_lines;
					g.shell->grid().build(preview_camera_view,
						preview_grid_w, preview_grid_h, &preview_lines);
					append_drop_marker(preview_lines);
					upload_grid(g.preview_view, preview_lines);
				}

				/* Auto grid keeps the translation snap in step with the cell. */
				if (g.shell->grid().auto_density())
				{
					const float cell = g.shell->grid().effective_cell_size(
						camera_view.distance, grid_h, camera_view.fov_deg);
					if (cell > 0.0f)
						g.shell->snap().translate_step = cell;
				}

				phase0_renderer_render(g.renderer, (float)SDL_GetTicks() / 1000.0f);

				/* V09: bind the freshly rendered attachments.  The UI textures
				 * are only updated when the GPU handle or size changed, so the
				 * runtime HDR/MSAA switches rebind without sampling a stale
				 * texture on the frame after the attachment swap. */
				g.viewport_texture.sync(ui, wg->vgctx,
					g.shell->viewport(),
					phase0_view_color_texture(g.main_view), rw, rh);
				g.preview_texture.sync(ui, wg->vgctx,
					g.shell->preview_viewport(),
					phase0_view_color_texture(g.preview_view), pw, ph);
				if (g.render_settings.depth_preview)
				{
					g.depth_texture.sync(ui, wg->vgctx,
						g.shell->depth_viewport(),
						phase0_view_depth_preview_texture(g.main_view), rw, rh);
				}
			}

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
	if (g.renderer)
	{
		phase0_renderer_destroy(g.renderer);
		g.renderer = nullptr;
		g.main_view = nullptr;
		g.preview_view = nullptr;
		g.viewport_texture = {};
		g.preview_texture = {};
		g.depth_texture = {};
	}
	delete_font_family(familys);
	cb->destroy_rvg(vg);
	free_ctx_cb(cb);
	delete wg;
	return 0;
}
