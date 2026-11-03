# 响应式 / 声明式数据绑定 规划

> 目标版本：v0.4+（可选层，不重写核心）  
> 关联：runtime_roadmap §7.1（R4-1 绑定惯例）、editor_roadmap（PropertyGrid 命令栈）、`docs/vgui_capability_assessment.md` §5.3「无响应式/声明式数据绑定」  
> 状态：规划（未实现）

---

## 0. 背景与现状

vgui 当前的状态同步是**手动 push + 脏检查**模式（R4-1 约定）：

- 宿主每帧调用 `vgui_widget_set_value/set_text/set_checked`，由 `1e-6` 脏检查 / 同值短路决定是否需要 `mark_dirty`。
- 数据集合（list/tree）走 `vgui_row_source` **拉模型**（view → model 按需取数）。
- 用户编辑经 `vgui_widget_on_event` + `vgui_widget_get_value` 回写宿主。
- 撤销只在 editor 的 `command_stack` / PropertyGrid 内部，未对普通控件开放。

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
- [x] 代码位置：`include/vgui/vgui.h`（API）、`gui/vgui.cpp`（`vgui_binding` 结构、context 成员、`vgui_binding_*` helper、`vgui_bind*` 实现）。
- [x] **验收**（`sample/vgui_model_test.cpp::test_binding`）：
  - bind 初始化控件；`vgui_model_mark_dirty` + `vgui_update` 后控件更新，空闲帧不重复写；
  - 滑块拖动 → 模型字段更新 → 同字段另一绑定控件同步（无回声）；
  - checkbox 点击回写模型；unbind 后不再同步；cb 绑定 + `invalidate` 重新拉取。
- [x] 文档：README 增加「响应式数据绑定」段落（含用法示例）。

### Phase 1 — 声明式糖 + 更多属性（P1, M）

- [ ] C++ `bind<T>(w, &model.field)` 模板封装（offsetof）。
- [ ] 扩展 kind：TEXT / PROGRESS / VISIBLE / ENABLED / SELECTION（list/tree 选中 ↔ 模型索引）。
- [ ] `vgui_demo` 增加「绑定演示」面板（滑块↔数值标签↔checkbox 联动一个 `settings` 结构体）。
- [ ] text 绑定与 i18n `text_key` 的优先级约定写入文档（显式 text 覆盖 key；绑定 text 时清除 key）。
- [ ] **验收**：demo 手动验证联动；`model_test` 增加 selection/text 绑定断言。

### Phase 2 — 撤销集成 + 自动脏标记（P2, M）

- [ ] `vgui_binding_set_transaction`：writeback 经 `command_stack` 生成可撤销命令（复用 PropertyGrid 的命令合并/事务）。
- [ ] 提供 `VGUI_MODEL_WRITE(model, field) = x` 宏/RAII，自动 `vgui_model_mark_dirty`，免去手动调用。
- [ ] PropertyGrid 直接字段绑定：复用本层把 PG 行编辑回写到结构体字段并 undo（替代部分手写 value_fn）。
- [ ] **验收**：demo 中"绑定控件编辑 → Ctrl+Z 还原模型字段"。

### Phase 3 — 集合/派生绑定（P3, L，可选）

- [ ] 集合适配器：把 `std::vector<Row>` 经 `vgui_row_source` 自动桥接（现需手写 row_source；此阶段提供泛型适配器），与标量绑定共用 model version。
- [ ] 派生/表达式绑定：computed binding（`get` 来自其它字段计算），声明式 `bind_expr`。
- [ ] 调试浮层：可视化绑定图与每帧 flush 计数（性能诊断）。
- [ ] **验收**：示例把"实体列表 + 选中实体属性面板"双向联动且可撤销。

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
