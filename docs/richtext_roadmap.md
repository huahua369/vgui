# 核心富文本原语路线图（richtext primitives）

> 状态：方案已定（2026-11-17），阶段 1–2 实施中。
> 决策出处：`docs/vgui_capability_assessment.md` §6 优先级建议第 2 条「核心层补富文本/语法高亮/行号/查找替换，或把 kit 的 CodeEditor 能力下沉」。

---

## 1. 为什么是「核心补原语」而不是「搬运 CodeEditor」

调研结论（详见当日工作日志）：

1. **下沉不是编译问题**。`vgui_editor_kit` 无 SDL3 硬依赖，`vgui_editor_kit_test` 在 `VGUI_CORE_ONLY` 下照常构建。搬运 4417 行 `code_editor.cpp` 只会把可选库变成强制库，收益有限。
2. **kit 的高亮/行号/查找替换本来就是拿核心公开 API 拼的**：`VGUI_CUSTOM` + 逐 run 调 `vgui_custom_draw_text[_sized]`，词法器只是宿主数据。真正的核心缺口只有三样：
   - span/run 数据结构（公共头完全没有）；
   - 多 run（多色/多字号）绘制与测量的正式契约；
   - 可共享的撤销/事务栈（与本期无关，留待后续阶段）。
3. `editor_roadmap.md:26` 明确「不把 CodeEditor 塞进 `vgui.h`」；搬运会打破该约定并稀释 core-only 定位。

故本期只做**原语**：让宿主（含 kit）能用统一、受支持的 API 做片段着色/混排字号，不改控件枚举、不搬词法器、不做编辑器控件。

## 2. 本期范围（阶段 1–2 合并交付）

### 2.1 新增公共头 `include/vgui/richtext.h`（C 接口，CORE_ONLY 可用）

```c
enum {
    VGUI_TEXT_DECOR_UNDERLINE     = 1u << 0,
    VGUI_TEXT_DECOR_STRIKETHROUGH = 1u << 1
};

typedef struct vgui_text_span {
    int start;         /* UTF-8 字节偏移 */
    int length;        /* 字节长度 */
    vgui_color color;  /* 0 = 继承 base_color */
    float font_size;   /* 0 = 继承 base_font_size */
    unsigned decor;    /* VGUI_TEXT_DECOR_* 位掩码 */
} vgui_text_span;

void vgui_custom_draw_text_spans(vgui_widget* widget, ovg_ctx_cb* cb, rvg_t* vg,
    const char* text, const vgui_text_span* spans, int span_count,
    float x, float y, float width, float height,
    vgui_color base_color, float align_x, float align_y, float base_font_size);

float vgui_widget_measure_text_spans(vgui_widget* widget, const char* text,
    const vgui_text_span* spans, int span_count, float base_font_size);

float vgui_widget_get_line_height_spans(vgui_widget* widget,
    const vgui_text_span* spans, int span_count, float base_font_size);
```

### 2.2 语义（与 kit `code_span` 对齐的容错口径）

- **字节偏移**：`start/length` 是 UTF-8 字节区间；边界调整到字符起始，不切坏多字节字符。
- **乱序/重叠**：按 `start` 稳定排序，重叠部分后出现者胜出；未被任何 span 覆盖的字节用 base 颜色/字号绘制。
- **继承**：`color==0` 或 `font_size==0` 回退到调用方传入的 base 值。
- **混排行盒**：所有 run 共享「最高 run 的行高」作为统一行盒，各 run 在行盒内垂直居中（基线自然对齐）；对齐是物理对齐，与 `vgui_custom_draw_text_sized` 一致，不做 RTL 翻转。
- **裁剪**：片段文本沿用 `vgui_custom_draw_text` 的矩形裁剪；装饰线按同一矩形手动夹紧，不越界。
- **空输入**：`text` 为空 / `span_count<=0` 时等价于普通 `vgui_custom_draw_text_sized`，`measure` 返回整体测量值。

### 2.3 实现文件

- `gui/richtext.cpp`：纯用核心公开 API（`vgui_custom_draw_text_sized`、`vgui_widget_measure_text_sized`、`vgui_widget_get_line_height_sized`）+ `ovg_tools::fill_round_rect` 画装饰线，加入 `backends/CMakeLists.txt` 的 `OVGUI_SOURCES`（CORE_ONLY 也编译）。
- 不改 `vgui_widget` 结构，不改 `vgui.h`（独立头，避免核心头继续膨胀）。
- 安装规则与公共头自包含检查纳入 `richtext.h`。

## 3. 非目标（本期明确不做）

- 可编辑富文本/WYSIWYG 行内格式（粗体/斜体/混合字号的可编辑文档模型）——独立立项。
- 词法器、语法高亮主题、行号 gutter、查找替换、富日志控件——继续留 kit，宿主自己用原语拼。
- 撤销/事务栈下沉、CodeEditor 源文件搬运、新控件枚举。

现已支持绘制富文本的字重/倾斜与基于字体度量的下划线、删除线和上划线。字体变体匹配、样式继承规则与发光支持见 [OVG 文本样式规划](ovg_weight_slant_plan.md)。

## 4. 验证计划

- `sample/vgui_model_test.cpp` 新增 `test_richtext_spans`：测量（单 span/多 span/混排字号/空输入）、排序与重叠容错、UTF-8 边界不切坏、继承 base、装饰位解析。
- 公共头自包含检查通过（richtext.h 单独编译）。
- 全量构建 + model / spreadsheet / on-demand / crosswindow / floater / editor kit / C API / editor C API / 截图回归全绿。
- 更新 `README.md`、能力评估 §6-2 状态。

## 5. 后续阶段（不在本期）

- 阶段 3：把可共享的撤销/事务抽象与搜索等价控件做成核心或独立可选原语，使查找替换能纯用核心构建。
- 阶段 4：仅当明确需要「普通应用零 kit 依赖用代码编辑器」时，整体搬运 CodeEditor 并保留 kit 兼容期。
- 阶段 5：WYSIWYG 独立立项（样式化文档模型 + 样式区间与编辑双向映射）。
