# 响应式 / 声明式数据绑定 规划

> 目标版本：v0.4+（可选层，不重写核心）  
> 关联：runtime_roadmap §7.1（R4-1 绑定惯例）、editor_roadmap（PropertyGrid 命令栈）、`docs/vgui_capability_assessment.md` §5.3「无响应式/声明式数据绑定」  
> 状态：P0（双向标量绑定）、P1（声明式糖 + SELECTION）、P2（撤销集成）、P3（集合/派生绑定，核心部分）已实现

---

## 0. 背景与现状

vgui 当前的状态同步是**手动 push + 脏检查**模式（R4-1 约定）：

- 宿主每帧调用 `vgui_widget_set_value/set_text/set_checked`，由 `1e-6` 脏检查 / 同值短路决定是否需要 `mark_dirty`。
- 数据集合（list/tree）走 `vgui_row_source` **拉模型**（view → model 按需取数）。
- 用户编辑经 `vgui_widget_on_event` + `vgui_widget_get_value` 回写宿主。
- 撤销原先只在 editor 的 `command_stack` / PropertyGrid 内部；P2 起普通控件也能进撤销栈（编辑日志 + `binding_undo` 适配器）。

**缺口**：没有"绑定对象"把「控件属性 ↔ 模型字段」连起来，让两者双向自动同步且不靠每帧轮询。这正是评估报告中列为共通缺失的"无响应式/声明式数据绑定"。

### 0.1 设计约束（必须遵守）

1. **C 公共 API**：绑定注册、`vgui_bind_*` 必须是 C 函数；内部实现可用 C++。
2. **单线程 / 保留模式 + 即时录制**：绑定层绝不能在每帧主动拉模型；只能"脏版本号驱动"。
3. **零成本空载**：绑定层空闲（模型未变）时不应产生任何 `set_*` / `mark_dirty`，必须复用现有 `1e-6` 脏检查。
4. **CORE_ONLY 兼容**：纯核心层、不依赖 SDL（绑定层不碰 OS）。
5. **可选、不强制**：默认不启用；不增加"无绑定"路径的体积与开销。
6. **不破坏现有惯例**：R4-1 每帧直写仍可并存；绑定层是增量能力。

---

## 1. 核心设计：脏版本号 + 双向同步

### 1.1 三类对象

- **model 句柄**：`void* model` 指针 + 每 model 的 `uint64_t version`。宿主改字段后 `vgui_model_mark_dirty(ctx, model)` 仅 `version++`，不做事。
- **binding 记录**：`(ctx, widget, kind, model, offset[, get/set cb], seen_version, suppress)`。存于 context 的注册表（数组/链表）。
- **writeback 事务**（Phase 2 起可选）：编辑回写可经 `command_stack` 落 undo。

### 1.2 双向同步流程

```
vgui_update(ctx):
  for b in ctx->bindings:
    if b->model->version != b->seen:
       b->suppress = 1                      // 抑制回写回声
       field = read(b)                       // offset 直读 或 get_cb
       set_widget_prop(b, field)             // 复用 set_value/set_text/... 脏检查
       b->seen = b->model->version
       b->suppress = 0

widget 用户编辑（slider/checkbox/...）:
  set_value 检测 changed -> 触发 on_event ->
     if !b->suppress:                        // 只在非模型推送时回写
        write(b, widget_prop)                // 写回 model 字段
        b->model->version++                  // 同字段其它绑定下次更新跟随
        // 注意：本 binding 的 seen 刚被 update 设为最新，不会回声
```

**回声防护**：模型 → 控件写入期间置 `suppress`，控件自身的 changed 通知不会反向写回；编辑 → 模型写入后只 bump version，不触发 set_widget（seen 已是最新）。

### 1.3 与既有机制的关系

| 现有机制                              | 角色      | 新绑定层如何复用                                 |
| --------------------------------- | ------- | ---------------------------------------- |
| `set_value` 1e-6 脏检查              | 模型→控件   | 直接调用，空闲零成本                               |
| `set_text` 同值短路                   | 模型→控件   | 直接调用                                     |
| `vgui_widget_on_event` / `notify` | 控件→模型   | 作为 writeback 触发点                         |
| `vgui_row_source`                 | 集合绑定（拉） | **并存**：row_source 管 list/tree 集合；本层管标量属性 |
| `command_stack`（editor）           | 撤销      | Phase 2 起 writeback 可路由                  |

---

## 2. 公共 API 草图（C）

```c
typedef enum vgui_bind_kind {
    VGUI_BIND_VALUE    = 0,  /* float        <-> slider/progress/value */
    VGUI_BIND_TEXT,          /* const char*  <-> label/edit/button 文本 */
    VGUI_BIND_CHECKED,       /* int          <-> checkbox/switch */
    VGUI_BIND_PROGRESS,      /* float 0..1   <-> progress 环/条 */
    VGUI_BIND_VISIBLE,       /* int          <-> 可见性 */
    VGUI_BIND_ENABLED,       /* int          <-> 启用 */
    VGUI_BIND_SELECTION      /* int          <-> list/tree 选中索引 */
} vgui_bind_kind;

/* 偏移绑定：模型为 POD 结构体，字段用 offsetof 定位。开销最低。 */
vgui_binding* vgui_bind(vgui_context* ctx, vgui_widget* w,
                        vgui_bind_kind kind, void* model, size_t offset);

/* 回调绑定：模型非 POD 或需转换时，提供读取/写入函数。 */
vgui_binding* vgui_bind_cb(vgui_context* ctx, vgui_widget* w,
                           vgui_bind_kind kind, void* user,
                           vgui_bind_get_fn get, vgui_bind_set_fn set);

void vgui_unbind(vgui_context* ctx, vgui_binding* b);

/* 模型变更后调用：仅 version++，下一帧 vgui_update 才 flush。 */
void vgui_model_mark_dirty(vgui_context* ctx, void* model);
/* 单绑定失效（不知 model 句柄时）。 */
void vgui_binding_invalidate(vgui_binding* b);

/* Phase 2：编辑回写路由到撤销栈，使控件编辑可 undo。 */
void vgui_binding_set_transaction(vgui_binding* b, vgui_command_stack* stk);
```

C++ 便利封装（可选，放 `vgui/binding.hpp`，不进核心 ABI）：

```cpp
template<class M, class T>
vgui_binding* bind(vgui_context* ctx, vgui_widget* w,
                   vgui_bind_kind k, M* model, T M::* field) {
    return vgui_bind(ctx, w, k, model, offsetof(M, *field));
}
```

---

## 3. 分阶段实施

### Phase 0 — 双向标量绑定（P0, S，**已完成**）

- [x] context 增加 `bindings` 注册表 + `model_versions` 映射（`void*` → `uint64_t`）。
- [x] `vgui_bind` / `vgui_bind_cb` / `vgui_unbind` / `vgui_model_mark_dirty` / `vgui_binding_invalidate`。
- [x] `vgui_update` 前段 flush：仅对 `version != seen` 的绑定读字段 → 直接写控件字段 + `mark_dirty`（复用 `1e-6` 脏检查）。
  - 实现采用**直接写字段 + `seen` 版本比对**而非 `suppress` 标志：flush 不走 setter，故不触发 `vgui_notify_value`，天然无回声；`seen` 保证空闲零写。
- [x] writeback：`vgui_notify_value`（value/checked/progress）与 `vgui_notify_text`（text）末尾写回 model + bump version；覆盖滑块拖动、checkbox 点击、编辑提交等真实用户编辑路径。
- [x] 额外覆盖 `VGUI_BIND_VISIBLE` / `VGUI_BIND_ENABLED`（P0 顺带），`VGUI_BIND_SELECTION` 仅占位（P1）。
- [x] `vgui_destroy` 释放未解绑的 binding（避免泄漏）。
- [x] **实机回归修正（2026-11-03，MSVC 19.51 全量构建 + 跑测试）**：`VGUI_BIND_TEXT` 推送到 `VGUI_EDIT` 时只写了 `w->text`，而编辑框的权威文本在 `edit_text` 里 —— 控件显示旧值、用户输入从空串开始，回写也读不到新值。新增 `vgui_binding_apply_text()`：同时更新 `text_key`、`edit_text`、光标/锚点、IME 状态与内容宽度；回写侧统一用 `vgui_widget_get_text()` 取有效文本。
  - 已知边界：offset 绑定要求字段类型与 kind 精确匹配 —— VALUE/PROGRESS 读 `float`，CHECKED/VISIBLE/ENABLED/SELECTION 读 `int`，TEXT 读 `std::string`。绑定 `double` 字段会被当成 4 字节 float 读取（小端下 `0.5` 读成 `0.0`），需要转换时请改用 `vgui_bind_cb`。
- [x] 代码位置：`include/vgui/vgui.h`（API）、`gui/vgui.cpp`（`vgui_binding` 结构、context 成员、`vgui_binding_*` helper、`vgui_bind*` 实现）。
- [x] **验收**（`sample/vgui_model_test.cpp::test_binding`）：
  - bind 初始化控件；`vgui_model_mark_dirty` + `vgui_update` 后控件更新，空闲帧不重复写；
  - 滑块拖动 → 模型字段更新 → 同字段另一绑定控件同步（无回声）；
  - checkbox 点击回写模型；unbind 后不再同步；cb 绑定 + `invalidate` 重新拉取。
- [x] 文档：README 增加「响应式数据绑定」段落（含用法示例）。

### Phase 1 — 声明式糖 + 更多属性（P1, M）

- [x] C++ `bind<T>(w, &model.field)` 模板封装（`vgui::bind`，内部换算成员偏移，编译期类型校验）。
- [x] 扩展 kind：TEXT / PROGRESS / VISIBLE / ENABLED / SELECTION（list/tree 选中 ↔ 模型行索引，双向）。
- [x] `vgui_demo` 增加「数据绑定 Reactive binding」卡片（滑杆↔进度条↔静音开关↔标题↔列表选中共享一个 `demo_bind_model`）。
- [x] text 绑定与 i18n `text_key` 的优先级约定写入文档（绑定写入时清除 `text_key`，显式文本优先）。
- [x] **验收**：demo 手动验证联动；`model_test` 增加 selection/text 绑定断言。
  - 已覆盖：selection 初始化、model→widget、`-1` 清空、widget→model（含取消选中回写 `-1`）、C++ 糖初始化与推送。
  - 已知边界：`SELECTION` 只表达单个行索引，多选控件上报第一条选中；由模型反向推送时会先清空再选中该行。

### Phase 2 — 撤销集成 + 自动脏标记（P2, M）

- [x] 编辑日志（edit journal）：`vgui_set_edit_recorder` / `vgui_get_edit_recorder` / `vgui_set_edit_record_enabled`。核心在每次「用户编辑 → 模型」回写后回调一次，携带 `vgui_bind_change`（绑定、控件、字段 model+offset、`VGUI_BINDV_*` 类型、新旧数值 / 新旧文本）；`vgui_update` 的模型推送、`vgui_widget_set_*`、`vgui_model_write_raw` 回放**都不上报**。
- [x] `vgui_model_write_raw(ctx, model, offset, value_type, number, text)`：撤销命令用的类型化字段写入，写后直接 bump 版本（不产生新记录，避免 Ctrl+Z 自增撤销项）。
- [x] `vgui_binding_alive(ctx, binding)`：撤销命令复活历史句柄前先校验，避免 unbind / 销毁后的悬垂。
- [x] `vgui_editor_kit::binding_undo`（`include/vgui/editor/binding_undo.h`）：把编辑日志接到 `command_stack`，每次写回变成一条可撤销命令；同字段连续编辑在合并窗口（默认 500 ms）内折叠为一条（滑杆拖动、连续输入只留一步）；offset 绑定经 `vgui_model_write_raw` 回放，callback 绑定经 `set_fn` + `vgui_binding_invalidate` 回放；RAII 析构自动摘除记录器并还原上一个。
- [x] 自动脏标记：C 用 `VGUI_MODEL_SET(ctx, model, field, value)`，C++ 用 `vgui::model_write(ctx, &model, &M::field) = value`（赋值代理，写完自动 mark_dirty）。
- [x] PropertyGrid 直接字段绑定：`vgui_editor_kit::field_property(id, label, model, &M::field, ctx)`（`include/vgui/editor/property_binding.h`）由成员指针生成 `property_desc` 的 get/set，支持 bool/int/float/double/std::string；PG 自身命令栈仍在，避免与本层重复记录。
  - 实现细节：整型与浮点共用一个 `enable_if<is_arithmetic && !is_bool>` 重载，按 `is_floating_point` 选 `integer`/`number`。两个仅在默认模板实参上不同的重载会被当作**同一模板的重复声明**（GCC/Clang 报 "redefinition of default argument"），MSVC 不报但语义不可移植。
- [x] **验收**：
  - `model_test`：拖动 1 次只记录 1 条且带新旧值、程序化写入不记录、`vgui_model_write_raw` 回放后模型与控件同步且不再记录、TEXT 新旧文本正确、记录器可关闭/移除。
  - `editor_kit_test`：`binding_undo` + `command_stack` 下「拖动 → 1 条记录 → undo 还原模型并重推控件 → redo 复原」；不同字段不互相合并；`merge_window(0)` 逐步记录；适配器析构后记录器摘除。`test_bound_properties` 覆盖 `field_property`：bool/int/float/double/std::string 五行的类型推断与初始文本、提交后写字段并标脏使绑定滑杆同帧同步、非法文本被拒且字段不变、PropertyGrid 自身命令栈 undo 还原字段、省略 `ctx` 时只写字段不标脏（需手动 `vgui_model_mark_dirty` 才推控件）。
  - `vgui_demo`「数据绑定」卡片：编辑日志行 + 「撤销 Ctrl+Z」按钮 + Ctrl+Z 快捷键（demo 用最小栈演示同一份记录器契约）。
  - 已知边界：撤销命令只回放**模型侧**，控件侧的中间交互态（如拖动中未提交的滚动偏移）不还原；callback 绑定的回放依赖 `set_fn` 仍有效，绑定被销毁时静默跳过。

### Phase 3 — 集合/派生绑定（P3, L，可选）

- [x] 集合适配器：`vgui::vector_rows<Row>`（`include/vgui/binding/collection.h`）把 `std::vector<Row>` 直接适配成 `vgui_row_source`：`attach(list_or_tree)` 自动按控件类型装源，`refresh()` 结构变更、`invalidate_rows(first,count)` 内容变更；行文本/详情/id/enabled 由访问器提供，返回文本放内部缓冲（符合 row_source 生命周期约定），默认 id = index+1。
  - `detach()`（P3 收尾补齐）：控件只保存 `vgui_row_source*`，适配器若先于控件销毁必须显式摘源，否则控件持有悬垂指针；`detach()` 清空控件行源并复位内部 view 指针。
- [x] 派生/表达式绑定：`vgui_binding_add_dependency(binding, model)` / `vgui_binding_clear_dependencies`。给 `vgui_bind_cb` 的绑定挂上依赖模型后，任一依赖被 `mark_dirty` 就重跑 `get_fn`（总和、`3/5 已选`、格式化标签等）；空闲帧不重算，无需手工 `invalidate`。
  - 边界：这是**回调式** computed，不是字符串表达式 DSL；没有依赖自动分析（依赖需显式登记），也不做跨模型拓扑排序。
- [x] 调试诊断：`vgui_binding_count(ctx)` 与 `vgui_binding_last_flush_count(ctx)`（上次 flush 的 model→widget 推送数，空闲为 0），供宿主画绑定浮层/性能诊断；可视化浮层本身由宿主 UI 绘制（未内置）。
  - 计数只统计**真的写了控件**的推送：依赖标脏但 computed 值没变的绑定会重算一次、`last_flush_count` 仍为 0（`model_test` 有断言）。
- [x] **验收**：
  - `model_test::test_binding`：派生绑定初始化 / 依赖标脏重算 / 未标脏的空闲帧不重算（读取计数不变）/ 诊断计数；
  - `model_test::test_binding_collection`：`vector_rows` 装源后 `vgui_list_is_virtual == 1`、行数/文本/id/enabled 来自访问器、`refresh()` 增行、`invalidate_rows()` 刷新内容、`detach()` 后 `vgui_list_is_virtual` 回到 0；
  - 部分覆盖：「实体列表 + 选中实体属性面板 + 撤销」的完整示例仍由宿主组合（demo 卡片覆盖列表选中绑定 + 值绑定 + 撤销三步），未单独做一体化样例。

---

## 4. 非目标（明确不做）

- **完整元对象 / 反射系统**（类 Qt MOC）：过重，C 库不适用；用 offset / 回调替代。
- **跨线程绑定**：单渲染线程约束不变，绑定仅本线程有效。
- **网络 / 分布式响应式**：超出 vgui 范围。
- **强制所有人用绑定**：保留 R4-1 手工人直写路径。

---

## 5. 风险与缓解

| 风险                          | 缓解                                |
| --------------------------- | --------------------------------- |
| 回声死循环（模型↔控件互写）              | `suppress` 标志 + `seen` 版本比对，已设计   |
| 空闲时仍 `set_*` 拉低性能           | 复用 1e-6 / 同值短路，version 不匹配才写      |
| text 绑定与 i18n `text_key` 冲突 | 明确优先级，绑定 text 时清 key              |
| 非 POD 模型字段                  | 提供 `vgui_bind_cb` 回调变体，不强制 offset |
| 绑定层膨胀无绑定路径                  | 注册表仅按需分配，CORE_ONLY 纯核心、无 SDL 依赖   |

---

## 6. 与评估报告的对齐

本规划落地后，`vgui_capability_assessment.md` §5.3「无响应式/声明式数据绑定」可改为"已实现（Phase 0/1）"；Phase 2 还会补上"控件编辑可撤销"这一编辑器能力。其余共通缺失（文件对话框、屏幕阅读器、移动/Web 后端、多线程 UI 等）不受影响。

现状（P2 + P3 之后）：标量双向绑定、声明式糖、SELECTION、编辑日志 + `command_stack` 撤销、`model_write` 自动脏标记、PropertyGrid 字段绑定、`vector_rows` 集合适配器、依赖驱动的 computed 绑定均已落地；`vgui_capability_assessment.md` 的「响应式/声明式数据绑定」条目已从共通缺失中移除，仅保留「字符串表达式 DSL / 自动依赖分析」作为未覆盖项。
