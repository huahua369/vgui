# M0 剩余项实现计划（F02 剩余 + F04 剩余 + F03 拖放框架）

核实结论：F01/F05 已完成；F02 缺"命令合并、树/资产接入、失败协议、嵌套事务测试"；F04 缺"行图标/文字颜色/自定义绘制/行内命中区域、细粒度失效"；F03 仅接口占位。按 **F02 → F04 → F03** 顺序实施（F02 最小且解锁最多下游；F04 的行命中/图标是 F03 行拖拽的基础；F03 最大且跨核心/后端/kit，其 Outliner 重排验收正好复用 F02 的 move_node 命令）。

---

## 第 1 步：F02 命令/撤销栈剩余（仅 editor kit，无核心改动）

### 1.1 失败命令协议
- `gui/editor/command_stack.h`：`command::execute()` 由 `void` 改为返回 `bool`（true=已应用）。这是 kit 内部 C++ API 的有意破坏性修改（kit 尚无兼容承诺）。
- `function_command` 回调签名不变，固定返回 true；`command_group::execute()` 顺序执行子命令，遇 false 停止并返回 false。
- `command_stack::execute()`：执行返回 false → 不入栈、不清 redo、返回 false；true 才记录。`push()` 语义不变。

### 1.2 命令合并（merge）
- `command` 增加 `virtual bool merge_with(command& next) { return false; }`（尾部默认实现，不破坏现有子类）。
- command_stack 增加 `can_merge_` 标志：仅在"栈顶可合并且不在事务内"时对新命令调 `undo_.back()->merge_with(*cmd)`，成功则丢弃新命令（效果已应用）。`undo()/redo()/clear()/begin_transaction()/rollback_transaction()` 复位标志，保证 undo 后新命令不并入已撤销命令。
- `gui/editor/property_grid.cpp`：把属性提交从 `function_command` 改为专用小命令类，实现 `merge_with`（同一行同一属性连续提交合并为一条）；`reset_all()`（cpp:567-575）包进事务，N 行重置只产生一条撤销。

### 1.3 树/资产操作接入
- `gui/editor/outliner_view.h/.cpp`：新增 `set_command_stack()` 与 `move_node(node, new_parent, index)`——经宿主回调 `on_move_node` 应用到真实模型，命令 undo/redo 重放，失败回调返回 false 不入栈。
- `gui/editor/asset_browser.h/.cpp`：新增 `set_command_stack()` 与 `rename_item(asset, name)`（宿主回调 `on_rename`）。F2/双击 UI 属 O02，此处只交付命令集成。

### 1.4 测试（sample/vgui_editor_kit_test.cpp）
新增：连续可合并命令 undo 一次回最初；undo 后新命令不合并；嵌套事务（内层 commit 并入外层、一次 undo 全撤）；失败 execute 不污染栈；move_node/rename_item 撤销/重做与失败路径。现有 551 项模型测试保持通过。

完成后勾选 F02。

## 第 2 步：F04 行扩展剩余（核心改动）

### 2.1 虚拟模式：`vgui_row_source` 尾部追加回调（vgui.h:1349-1366，NULL 缺省，源码兼容）
- `row_icon(user, index, vgui_texture** out)` — 行图标（树中绘制在 depth 缩进之后）。
- `row_text_color(user, index) -> vgui_color` — 0 表示用主题默认。
- `row_buttons(user, index)` + `row_button_icon(...)` + `row_button_click(user, index, btn)` — 行尾内联按钮（可见性/锁定场景），按钮命中优先于行选中、不改变选中。
- `row_draw(user, index, ovg_ctx_cb*, rvg_t*, x, y, w, h)` — 整行自定义绘制，设置后跳过该行标准 text/detail/icon 绘制；core 先做滚动裁剪再回调。

### 2.2 全量模型模式同步支持（O01 的 Outliner 用全量树，必须覆盖）
- `vgui_internal.h` item 记录增加 icon/文字颜色/按钮字段；新增 `vgui_list_set_item_icon / vgui_tree_set_item_icon / *_set_item_text_color / *_set_item_buttons` 与 `vgui_list_on_item_button / vgui_tree_on_item_button`。

### 2.3 细粒度失效
- `vgui_list_source_rows_changed(list, first, count)` / `vgui_tree_source_rows_changed(...)`：失效宽度缓存 + mark_dirty（tree 另置 rows_dirty）；`source_changed` 粗粒度接口保留。

### 2.4 测试
- `sample/vgui_model_test.cpp`：icon/color/buttons 回调与命中（点按钮不改选中）、row_draw 调用与裁剪、rows_changed 生效、全量 setter 生效；补"插入/删除不丢选择"显式断言与可见范围调用计数断言。
- `sample/vgui_c_api_test.c`：新 setter 冒烟。

完成后勾选 F04；文档注明验收文本中的"表格"归 T01（Table 复用同一 row source 机制），F04 交付 list/tree。

## 第 3 步：F03 拖放框架（核心 C API + SDL3 后端 + editor kit 桥接）

### 3.1 核心 C API（gui/vgui.h / vgui.cpp / vgui_internal.h）
- 载荷：`vgui_drag_kind {none,text,files,custom}` + `vgui_drag_payload {kind, text, files, file_count, id, data}`；context 拖拽期间持有拷贝。
- 发起/取消：`vgui_begin_drag(ctx, source_widget, payload)`、`vgui_cancel_drag()`。阈值由发起方（行拖拽）超过 `drag_threshold` 后调用，core 内部再校验。
- drop target：`vgui_drop_target {can_accept, hover(输出 vgui_drop_hint), drop, user}` + `vgui_widget_set_drop_target(widget, target)`。`vgui_drop_hint {none/line/rect, 矩形, 颜色}`。
- 路由（core 状态机）：拖拽中 mouse move → 自顶向下命中第一个带 target 的控件 → target 变化时 can_accept 置接受/拒绝 → hover 每帧取 hint → mouse up 且接受时 drop()。拖拽期间抑制 hover/tooltip/普通点击；光标反馈用 `vgui_cursor`；拖影在 context 绘制阶段画指针旁浮块（复用 Dock 拖影样式 vgui.cpp:11765 附近）；插入指示按 hint 用 accent 色绘制（复用 Dock 指示线样式 vgui.cpp:11741 附近）。
- 行拖拽发起：`vgui_list_set_row_drag / vgui_tree_set_row_drag(fn, user)`——行 press 移动超阈值且 fn 返回 payload 时发起通用拖拽。
- OS 文件 drop 桥：`vgui_input_drop_files(ctx, paths, count, x, y)`（对齐 vgui_input_* 家族，vgui.h:1491-1497）——构造 files 载荷走同一路由。
- 顺手项：TabView 拖动阈值从硬编码 4.0f（vgui.cpp:8321）接入 style。

### 3.2 SDL3 后端（backends/vgui_sdl3.cpp）
- 移除 `SDL_SetEventEnabled(SDL_EVENT_DROP_FILE, false)`（cpp:329-330）；DROP_FILE 逐个聚合路径、DROP_POSITION 记录坐标、DROP_COMPLETE 时调用 `vgui_input_drop_files`。DROP_TEXT 本步保持禁用（范围控制，文档注明）。

### 3.3 editor kit 桥接 + 三条验收路径
- editor_types.h 的 `drag_payload`/`drop_target` 与核心 C API 互转适配器（新增 `gui/editor/drag_bridge.h/.cpp`）。
- 三条路径演示级接线（完整打磨归 O03/P06/V06/A04）：
  - outliner_view：行拖拽源（scene_object 载荷）+ 自身 drop target 重排 → 调第 1 步的 `move_node`（进命令栈，可撤销）。
  - asset_browser：行拖拽源（asset 载荷）。
  - property_grid：接受 asset 载荷写入字符串行（完整引用行属 P06）；viewport_view：接受 asset/scene_object 记录 last_drop（真实创建对象属 V06）。
- `vgui_demo` 增加可操作拖放演示区块（DoD"可演示"要求）。

### 3.4 测试
- `vgui_editor_kit_test`：payload 互转、模拟 mouse 输入的拖拽路由（begin_drag→move→up）、reject 反馈、Outliner 重排进命令栈、`vgui_input_drop_files` 文件路径。
- 核心：list/tree 行拖拽发起与阈值、拖拽期间事件抑制（无窗口模型测试）。

## 每步通用收尾
- 按 CMakePresets 现有 preset 全量构建；运行 vgui_model_test / vgui_c_api_test / vgui_editor_kit_test 全部通过。
- 更新 `docs/editor_roadmap.md`：勾选对应条目、补"已实现"说明、同步第 11 节进度。

## 明确不在本计划范围
表格控件（T01）、完整资产引用行（P06）、多选拖放/循环父子检测（O03）、视口创建对象（V06）、资产→属性栏完整赋值打磨（A04）、DROP_TEXT 接入。F03 只交付这三条验收路径的演示级闭环。