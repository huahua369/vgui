# 表格图片功能规划（对齐 Excel）

> 状态：**S0/S1（in-cell 图片）+ S2（浮动图锚定层）+ S3（交互）+ S4（插入途径）+ S5（辅助信息与快照）已实施**；仅 S6（C API 镜像）仍为规划
> 适用版本：vgui 0.4+
> 相关：[editor_roadmap](editor_roadmap.md) §2.2「表格/数据网格」、[vgui_capability_assessment](vgui_capability_assessment.md)、[data_binding_roadmap](data_binding_roadmap.md)
> 面向对象：`vgui_editor_kit::table_view` / `vgui_editor_table`（C API）/ `vgui_spreadsheet_demo`

---

## 实施记录（2026-11-17）

**已完成：in-cell 图片（对应规划 S0 + S1）**

- 决策变更：不新增 `property_type::image` 枚举（会牵动 property_grid 20+ 处 switch 与 C API 枚举对齐）。改为在 `table_source` 增加 `cell_image` 回调：某单元格返回非空 `vgui_texture*` 即画图片，否则回退到 `cell_text`，编辑值仍是字符串 key 走现有 text 路径。此方案更贴合"图片即单元格内容"，且风险更小。
- 纹理借用工约落地：表格只借用不创建/更新/销毁；demo 用程序化 32×20 RGBA 纹理验证。
- 新增核心 API `vgui_custom_draw_texture_fit()`（STRETCH/CONTAIN/COVER/NONE，统一裁剪到目标矩形）；`vgui_custom_draw_texture()` 保留为 CONTAIN 便捷包装。
- C API 镜像：`vgui_editor_table_cell_image` / `vgui_editor_table_image_fit`、`vgui_editor_table_cell_image_default()`，`vgui_editor_table_source::cell_image`（追加在结构末尾，旧调用方 struct_size 小则保持 NULL，源码兼容）。
- demo：`vgui_spreadsheet_demo` 增加第 9 列 Badge，按行轮换四种 fit；自检从 31 → 35 条。
- 回归：debug 全量构建 104/104、spreadsheet 35/35、multiwindow on-demand 40/40、editor kit PASS、C API 2013/0、截图 13 场景 diff=0、crosswindow 74/74、floater 22/22。

**浮动图几何决策（采纳建议）：不写进 `table_state` v3**。几何作为内容交给宿主数据，通过 `table_source::drawings` 拉模型提供，与 in-cell 图片和现有 `cell_text` 保持一致；`table_state` 保持 v2，S5 的 v3 升级取消。避开双份数据、多视图分裂与撤销跨层问题。

**已完成：浮动图锚定层（对应规划 S2）**

- 数据模型：`table_drawing_sizing`（move_and_size / move_only / fixed）、`table_drawing`（id/key/借用的 texture/双锚点+偏移/固定 width-height/fit/rotation/opacity/z/alt_text）、`table_source::drawings` 回调（每次绘制拉一次，z 升序）。
- 坐标系：几何在**内容像素空间**（列偏移 + 显示行×行高，已乘 scale，未减 scroll）计算，与 `hit_test` 输入空间一致；绘制减 scroll 转绝对，命中加 scroll。锚点用 source row，经 `display_index_of` 跟随排序/增删行。
- 三种锚定：`move_and_size` 用双锚点算矩形（随列宽/行高缩放）；`move_only` 左上跟 from 锚点、尺寸用固定 width/height；`fixed` 用 from_dx/dy 作内容绝对坐标。全部随滚动。
- 交互基础：drawings 在 body clip 内绘制；点选优先命中 drawing（z 逆序），Alt 穿透到单元格；单元格选区与 drawing 选区互斥；Esc 清除；hover/选中边框只在 id 变化时推进 revision（无 idle tail）。
- 公共 API：`selected_drawing` / `select_drawing` / `clear_drawing_selection` / `hit_test_drawing` / `drawing_rect`（绝对坐标）/ `last_render_drawing_count` / `set_drawing_selection_listener`。
- rotation 字段已预留但 S2 不绘制（S3 接入手柄与旋转）。
- demo：spreadsheet 用程序化 96×64 纹理加三张浮动图（resize/move/fixed 各一），自检从 35 → 46 条，覆盖三种锚定随列宽变化、点选互斥、绘制计数。
- 回归：debug 全量构建、spreadsheet 46/46、multiwindow on-demand 40/40、editor kit PASS、C API 2013/0、截图 13 场景 diff=0、crosswindow 74/74、floater 22/22。

**已完成：交互（对应规划 S3）**

- 手柄：`drawing_handle`（move / rotate / 四角 / 四边）。选中图绘制 8 缩放柄 + 顶部旋转柄（带连线），命中按角 > 边 > 旋转柄 > 本体；手柄位置随 rotation 旋转，命中用逆旋转做点内判定。方向光标在手柄切换时更新，微动不置脏（延续按需渲染约束）。
- 手势状态机：按下记录基准绝对矩形 + 指针角 → 移动算预览矩形/角度（覆盖锚点几何，仅视觉）→ 抬起提交；Escape 取消且不产生变更。角点缩放默认锁宽高比，Shift 临时解锁并将旋转吸附到 15°。
- 变更提交：新增 `table_source` 之外的 `set_drawing_change_listener(id, before, after)`——表格从不改宿主数据，手势提交与 z 命令各发一次回调，宿主写回模型并压一条 undo。提交时把编辑后的绝对矩形烘焙成 `sizing=fixed` + from_dx/dy + width/height + rotation，几何显式归宿主，无隐藏状态。
- 旋转绘制：新增核心 API `vgui_custom_draw_texture_rotated()`，用 OVG 变换矩阵绕目标中心旋转（CPU/native 纹理都支持）；无旋转仍走 fit 版本（截图回归不变）。旋转选中框用四角折线描边。
- z 命令：`bring_drawing_to_front` / `send_drawing_to_back` / `move_drawing_z(id, ±1)`，即时重排下一帧绘制顺序。
- 公共 API：`set_drawing_change_listener` / `hit_test_drawing_handle` / `drawing_gesture_active` / 三个 z 命令。
- demo：宿主 change 回调写回 `g->drawings` 并压 `function_command`（"Edit Drawing"，undo 恢复 before）；自检从 46 → 64 条，覆盖手柄命中、move/resize/rotate 各一次提交+undo、Escape 取消、z 命令与错误 id。
- 回归：debug 全量构建 104/104、spreadsheet 64/64、multiwindow on-demand 40/40、editor kit PASS、editor C API 2013/0、通用 C API 32/0、截图 13 场景 diff=0、crosswindow 74/74、floater 22/22。

**已完成：插入途径（对应规划 S4）**

- 统一请求通道：新增 `table_drawing_request_kind`（insert/remove/update）、`table_drawing_request`（files / anchor / id / drawing / source）、`set_drawing_request_listener()`。表格只描述意图，宿主在其中解码文件、分配 id/纹理、写回自己的模型并压一条 undo；表格不解码、不拥有数据、不写入 `table_state`，接受后仅 `mark_dirty()`。无 listener 时请求 API 均为 no-op 返回 false。
- 宿主 API：`insert_drawing()` / `remove_drawing()` / `update_drawing()`，每次发一条 request 并返回宿主结果。
- OS 文件拖入：表格控件在 `build()` 挂 kit drop target（`can_accept` 只收 files；`hover` 高亮整个表体为 `VGUI_DROP_HINT_RECT`；`drop` 把绝对坐标换算到内容空间、`hit_test` 取落点单元格）。`insert_drawings_from_files()` 可程序化触发，request `source="drop"` 并带锚点与借用路径。
- 剪贴板图片：`set_clipboard_image_getter()`（独立于文本 handlers，源码兼容），`paste_images_from_clipboard()` 把路径作为 insert request（`source="paste"`，锚在当前选区左上）。`Ctrl+V` 优先图片、无图片才回退 TSV。
- demo：spreadsheet 的 request listener 用程序化纹理模拟"解码"，insert 生成稳定 id 并把首文件路径设为 key，insert/remove/update 各压一条 `function_command` undo；剪贴板用内存路径模拟。自检从 64 → 85 条，覆盖三种入口各一条 request+undo、锚点命中、模型写回、非法 id/空载荷拒绝且不改模型。
- 待续：S5（alt text tooltip、序列化快照——几何仍归宿主，只做只读快照）、S6（C API 镜像 drawings/request + 测试）。

**已完成：辅助信息与快照（对应规划 S5）**

- 决策落实：原规划的 `table_state` v3 drawings 数组**取消**（几何归宿主数据，见上文决策）；改为独立的只读快照自由函数，不碰 `table_state`（仍 v2）。
- alt tooltip：表格控件在 `build()` 挂 tooltip provider，hover 浮动图时显示其 `alt_text`（空则回退 `key`）；新增 `drawing_tooltip_text(id)` 公开查询 API（空/unknown id 返回空）。provider 复用该 API，文本 scratch 只在下次回调前有效，符合核心 tooltip 生命周期约定。
- 只读快照：`table_drawings_to_json()` / `table_drawings_from_json()` 序列化 vector<table_drawing> 的 id/key/双锚点/偏移/尺寸/fit/旋转/透明度/z/alt；**不序列化借用的 texture**（反序列化置空，宿主按 key 重新绑定）。旧 JSON 缺字段用 `table_drawing` 默认值填充，非法根/畸形锚点返回 error。
- demo：自检从 85 → 92 条，覆盖快照往返几何保真、texture 置空、旧字段兼容、非法输入拒绝、alt tooltip 文本查询。
- 回归：debug 全量构建、spreadsheet 92/92、on-demand 40/40、editor kit PASS、C API 2013/0、截图 13 场景 diff=0、crosswindow 74/74、floater 22/22。
- 待续：仅 S6（C API 镜像 drawings/request + 测试）。

---

## 0. 结论先行

当前表格**不支持图片**：`table_source` 只有 `cell_text`（返回 `std::string`），`table_cell_style` 只有文字色/背景色，`draw_body()` 只画文字和几个类型化预览（color/curve/gradient），没有任何贴图或自定义 cell 绘制回调。

但库里已经具备做这件事所需的**全部底层零件**（纹理、异步缩略图、拖放、撤销、持久化、按需渲染），缺口只在表格这一层的数据模型、绘制、交互与序列化。因此这是一个**增量扩展**，不需要动核心渲染器。

规划分两种图片形态，按 Excel 语义对齐：

| 形态 | Excel 名称 | 行为 | 本方案对应 |
|---|---|---|---|
| 嵌入单元格 | Place in Cell | 图片是单元格的值，随单元格裁剪、随列宽行高缩放 | `property_type::image` 列 + `cell_image` 回调 |
| 浮动图片 | Floating picture | 浮在网格上方，锚定到单元格区间，可自由移动/缩放/旋转/设层级 | `table_drawing` 覆盖层 |

---

## 1. 目标与边界

### 1.1 目标

1. 表格单元格可显示图片（缩略图列、产品图、状态图标等），宿主按 key 提供纹理，走现有虚拟化拉模型。
2. 表格可承载浮动图片对象：插入、选中、移动、缩放、旋转、层级、删除，锚定到单元格区间，随滚动/排序跟随。
3. 图片主要插入途径齐备：文件拖入（OS）、剪贴板粘贴、宿主 API 插入。
4. 图片操作进入撤销栈；布局随 `table_state` 持久化，且新旧版本兼容。
5. 全程复用现有渲染与缓存，遵守按需渲染语义（异步加载完成/几何变化时推进 `paint_revision`）。

### 1.2 非目标

- 不做图片编辑（裁剪后的像素处理、滤镜链、格式转码），只做显示层变换（fit / crop / rotation / tint / alpha）。
- 不解码图片文件：解码和纹理上传由宿主或 `thumbnail_cache` 的 `load_handler` 完成，表格只持有**借用的** `vgui_texture*`。
- 不做打印/导出排版，不把图片写进 TSV 剪贴板（TSV 仍只承载文本）。
- 不做富文本混排（单元格内图文混排）与单元格形状的任意路径裁剪（仅矩形/圆角）。

### 1.3 Excel 图片能力对照

| Excel 能力 | 优先级 | 本方案阶段 |
|---|---|---|
| 单元格内图片（随单元格缩放/裁剪） | P0 | S1 |
| 浮动图片插入（文件/剪贴板/拖入） | P0 | S2 / S4 |
| 锚定单元格、随单元格移动 | P0 | S2 |
| 选中 + 移动 + 角点缩放（锁比例） | P0 | S3 |
| 层级（置顶/置底/上移/下移） | P1 | S3 |
| 旋转手柄、透明度/tint | P1 | S3 |
| `Move and size with cells` / `Move but don't size` 锚定策略 | P1 | S2 |
| 裁剪（crop，显示区域裁剪） | P2 | S3 |
| 替代文本（alt text）、图片说明 | P2 | S5 |
| 打印、在线协作、公式引用图片 | 非目标 | — |

---

## 2. 现状与可复用能力

### 2.1 表格层现状

- `table_source`：`row_count` / `cell_text(row, column_id)` / `row_id` / `cell_style`，**拉模型**，只对可见窗口取数（`render_cell_queries_` 计数）。
- `table_column`：`property_desc editor` 复用 PropertyGrid 的类型化编辑器；已有 `color` / `curve` / `gradient` 的单元格内预览绘制范式（`draw_body()` 里的三个分支），图片预览可照此新增一个分支。
- `table_view` 是 CUSTOM 类控件，`on_draw(widget, cb, vg, ...)` 已拿到 `widget` + OVG 回调，可直接调用 `vgui_custom_draw_texture()`。
- `table_state`：`current_version = 2`，`table_state_to_json` / `from_json` 已做版本兼容读取（v1 映射到单列排序）。
- 事件：`on_event` 统一处理命中、选择、编辑、表头拖拽；命中测试 `hit_test(content_x, content_y)`。

### 2.2 可直接复用的底层零件

| 能力 | 接口 | 复用方式 |
|---|---|---|
| CPU / native 纹理 | `vgui_texture_create`、`vgui_sdl3_texture_create_native` | 宿主创建；表格借用 |
| CUSTOM 内画纹理 | `vgui_custom_draw_texture(widget, cb, vg, tex, x, y, w, h, tint)` | in-cell 图片的 contain 绘制 |
| OVG 直绘 | `cb->add_texture` / `cb->add_image`（见 `vgui_draw_row_texture`） | floating 图片的 fit/crop/rotation |
| 异步解码 + LRU | `thumbnail_cache`：`set_load_handler` / `enqueue` / `texture(key)` / `state_of` / `poll` | 图片按 key 异步加载，表格只查状态 |
| 拖放 | `vgui_drag_payload` / `vgui_drop_target` / `vgui_begin_drag` / `vgui_widget_set_drop_target` | 表内图片拖动、拖入单元格 |
| OS 文件拖入 | SDL3 后端 `os_window::drop_files` | 文件拖进表格自动插入 |
| 撤销 | `command_stack`（`push` / 事务 batch） | 图片增删改移作为命令 |
| 剪贴板 | `vgui_editor_table_set_clipboard_handlers`（当前仅文本） | 扩展图片分支由宿主提供 |
| 按需渲染 | `paint_revision` / `vgui_widget_mark_dirty` / 动画 | 异步加载完成、几何变化推进 revision |

---

## 3. 核心设计

### 3.1 数据模型

在 `table_view.h` 新增（示意）：

```cpp
enum class table_image_fit { contain, cover, stretch, none };

/* 单元格内图片的查询结果；texture 由宿主拥有，表格借用。 */
struct table_cell_image {
    vgui_texture* texture = nullptr;   /* NULL 表示无图片 */
    table_image_fit fit = table_image_fit::contain;
    vgui_color tint = 0;               /* 0 表示不染色 */
};

/* 浮动图片对象。几何由锚点每帧算出，不存绝对像素。 */
struct table_drawing {
    std::string id;                    /* 稳定 id，选中/撤销跟随 */
    std::string key;                   /* 资源 key，交给宿主/缓存解析 */
    vgui_texture* texture = nullptr;   /* 借用 */
    table_cell anchor_from{};          /* 左上锚点单元格 */
    table_cell anchor_to{};            /* 右下锚点单元格 */
    float from_dx = 0, from_dy = 0;    /* 锚点单元格内偏移 */
    float to_dx = 0, to_dy = 0;
    enum class size_mode { move_and_size, move_only, fixed } sizing =
        size_mode::move_and_size;
    table_image_fit fit = table_image_fit::stretch;
    float rotation = 0.0f;             /* 度 */
    float opacity = 1.0f;
    int z = 0;                         /* 层级 */
    std::string alt_text;
};
```

`table_source` 增加两个可选回调（二进制/源码兼容：新字段用默认构造，旧宿主不填即无图片）：

```cpp
std::function<table_cell_image(std::size_t row, const std::string& column)>
    cell_image;
/* 浮动层走拉模型：只为可见区间取数，与虚拟行一致。 */
std::function<std::vector<table_drawing>()> drawings;
```

### 3.2 所有权与生命周期

- **纹理一律借用**：与 `markdown_view` 的 media resolver、`thumbnail_cache` 约定一致——表格绝不创建/更新/销毁纹理。宿主保证纹理在返回后到下一帧仍有效；`thumbnail_cache` 的 LRU 淘汰需先通知表格（或表格每次绘制重新查询）。
- **数据归属宿主**：`key` / 锚点 / z 存在宿主模型；`table_state` 只持久化视图相关的表格设置（列宽、排序、浮动图层的派生几何做只读快照用于恢复选择）。
- **异步加载**：宿主在 `load_handler` 解码，主线程 `poll()` 上传纹理后调用 `table.refresh()` / `refresh_row()`，触发 `paint_revision++`。加载中/失败用占位图形，不阻塞 UI。

### 3.3 坐标系与锚定

- 所有几何在**内容坐标**（未滚动、scale=1 的逻辑像素）里计算，绘制时统一乘 `ui_scale()`，并减去 `scroll_x_/scroll_y_`。
- 浮动图矩形由锚点两端单元格矩形 + 偏移算出：
  - `move_and_size`：排序/列宽变化后重新取锚点矩形，图片跟随缩放。
  - `move_only`：只跟随左上锚点移动，尺寸不变。
  - `fixed`：绝对定位，不跟随。
- 滚动时锚点矩形天然跟随，无需额外逻辑。

### 3.4 绘制顺序与裁剪

`draw()` 顺序调整为：

1. 表头 / 网格背景 / 交替行
2. 单元格文字与 in-cell 图片（在 `draw_body()` 的 cell 循环里，紧邻 color/curve/gradient 分支之后新增 image 分支）
3. 浮动图片：按 `z` 升序绘制，统一被 `m.body_x/y/width/height` 的 `clip_guard` 裁剪
4. 选中框 / 缩放旋转手柄 / 拖拽预览

in-cell 图片用 `vgui_custom_draw_texture()`（contain + cell 矩形裁剪）；floating 图片用 `cb->add_texture`/`add_image` 自绘（支持 stretch/cover/crop/rotation，复用 `vgui_draw_image_geometry` 的几何逻辑，抽成表格可调用的公共辅助函数）。

### 3.5 命中与事件优先级

`on_event` 的鼠标按下按下列顺序判定（先命中者消费事件）：

1. 选中图片的**缩放/旋转手柄**（角点 > 边 > 旋转柄）
2. 选中图片的**本体**（移动）
3. 浮动图片本体（按 z 逆序命中，选中）
4. 单元格编辑 / 选择 / 表头排序（现有逻辑）

关键约束：

- 图片拖动期间禁用单元格选择，避免拖影干扰。
- 按住 `Alt` 可"穿透"图片点到单元格（Excel 行为）。
- 多选单元格区间与图片选中互斥：点击图片清空单元格选区，反之亦然。
- 与上一轮按需渲染修复保持一致：命中变化必须推进 `paint_revision`，纯 hover 无视觉变化不得推进。

---

## 4. 分阶段实施

### S0 — 绘制与数据通路（地基）

- `table_view.h` 加 `table_image_fit` / `table_cell_image` / `table_source::cell_image`。
- `draw_body()` 加 image 分支：取 `cell_image`，非空则 `vgui_custom_draw_texture()` 画在 cell 内边距矩形，文字让位（有图不画文字，或文字作为 alt 叠加由开关决定）。
- `render_cell_queries_` 之外新增 `render_cell_image_queries_`，保证只查询可见行。
- 验收：spreadsheet demo 加一列缩略图（用已有程序化纹理或 `thumbnail_cache`），滚动只查可见行，截图回归 diff=0。

### S1 — 单元格内图片完整化

- `property_type::image` 加入枚举；`property_desc` 增加 `image_fit` / `image_tint` 元数据。
- 编辑：双击图片列打开宿主提供的"选择图片"弹窗（宿主回调，值仍是 key 字符串），沿用 `property_normalize_value` 与撤销路径。
- 列宽/行高变化时图片按 fit 重排；DPI 缩放一致。
- 验收：图片列可键盘导航、值可撤销、TSV 复制仍得到 key 文本。

### S2 — 浮动图片层与锚定

- 加 `table_drawing` / `table_source::drawings` / `z` 排序 / 锚定矩形计算。
- `draw()` 增加浮动层通道；`hit_test` 扩展为返回"单元格 or drawing id"。
- 三种 `size_mode` 的锚定跟随。
- 验收：插入一张锚定图，滚动/改列宽/排序后位置正确；点选命中正确。

### S3 — 交互：移动 / 缩放 / 旋转 / 层级

- 手柄命中与拖动状态机（按下→预览→提交，Esc 取消）。
- 锁比例（默认开，Shift 临时解锁）、旋转手柄、透明度/tint。
- 层级命令：置顶/置底/上移/下移，绘制顺序即时重排。
- 拖动全程走 `vgui_widget_mark_dirty()` + `paint_revision`，与按需渲染兼容。
- 验收：拖拽/缩放/旋转/层级各进入一次 undo，截图回归覆盖手柄显示。

### S4 — 插入途径

- OS 文件拖入：`os_window::drop_files` → `table` drop target → 宿主解码 → 插入 drawing。
- 剪贴板：扩展 `vgui_editor_table_set_clipboard_handlers`，宿主提供 `image` 数据分支。
- 宿主 API：`table.insert_drawing(...)` / `remove_drawing(id)` / `update_drawing(...)`。
- 验收：拖入与粘贴各生成一条 undo；失败图片显示占位。

### S5 — 序列化与辅助信息

- `table_state` 升到 v3：新增 `drawings` 快照数组（id/key/锚点/几何/z/alt），v1/v2 读取时该字段缺省为空，保持兼容。
- in-cell 图片的值就是宿主 cell 文本里的 key，持久化由宿主数据负责。
- alt text 与 tooltip：hover 图片显示 alt。
- 验收：三版本 JSON 往返测试，旧文件不丢列宽/排序。

### S6 — C API + demo + 测试

- C API 镜像：`vgui_editor_table_cell_image`、`vgui_editor_table_drawing_*`、`vgui_editor_table_source` 增 `cell_image` / `drawings` 回调（沿用字符串的 bytes-needed 约定，图片用 `vgui_texture*` 借用返回）。
- `vgui_editor_kit_test` / C API test 增图片用例；`vgui_spreadsheet_demo` 展示缩略图列 + 一张浮动图。
- README 增「表格图片」小节。
- 验收：全量构建、debug 自检、截图回归 13 场景 diff=0、C API 计数归零。

---

## 5. 风险与开放问题

1. **纹理借用与 LRU 淘汰冲突**：`thumbnail_cache` 可能淘汰正在显示的纹理。需要约定"表格绘制当帧持有有效纹理"，或让宿主对表格可见 key 提高优先级 / 使用 `lookup` 不更新 LRU 的特性。S0 就要定死。
2. **图片与 hover 按需渲染的耦合**：图片 hover 要有视觉反馈（手柄/高亮），必须推进 revision；纯指针在空白区移动不得推进，避免重演上一轮的多余刷新 bug。
3. **排序稳定性**：锚定到"单元格区间"在排序后语义可能错乱（Excel 也有此问题）。建议默认 `move_and_size` 且锚点用稳定 `row_id` 而非行号，排序后跟随行移动。
4. **虚拟化与浮动层**：浮动图可能覆盖未加载的可见行区域；`drawings` 回调应只返回与可见区间相交的对象，避免大表全量扫描。
5. **C API 结构体膨胀**：新字段都放尾部 + `_default()` 初始化，保持源码兼容；不承诺二进制兼容（与现有策略一致）。
6. **行高自适应**：Excel 的 in-cell 图片会影响行高。本方案先固定行高、图片按 fit 缩放，行高自适应列 P3。

---

## 6. 依赖与工作量

- 无新增第三方依赖；解码仍由宿主负责。
- 建议实施顺序：S0 → S1 → S2 → S3 → S4 → S5 → S6，其中 S0/S1 可独立交付（只做缩略图列就已具备实用价值）。
- 每个阶段结束跑：debug 全量构建、spreadsheet 自检、截图回归、C API test、按需渲染自检，确保不回退。

---

## 7. 决策请求

实施前需要确认三点：

1. **优先形态**：先做 in-cell 缩略图列（S0/S1，收益快），还是先做浮动图片（S2/S3，更贴近"Excel 插图"直觉）？
2. **纹理来源**：demo 用程序化纹理快速验证，还是直接接 `thumbnail_cache` + 真实 PNG（需要宿主解码器）？
3. **持久化范围**：浮动图几何是否要写进 `table_state` v3，还是完全交给宿主数据（表格只做视图）？
