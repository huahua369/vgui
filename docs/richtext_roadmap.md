# 核心富文本原语路线图（richtext primitives）

> 状态（2026-11-18）：**阶段 1–2 已交付**，字重/倾斜、装饰（D1–D4）与发光（G1–G4）扩展已接入。当前是 UTF-8 span 的绘制和测量原语，不是通用段落排版引擎或可编辑富文本控件。
>
> 决策出处：[VGUI 能力评估](vgui_capability_assessment.md) §6 第 2 条；扩展实现与验收见 [OVG 文本样式规划](ovg_weight_slant_plan.md)。

## 1. 为什么是「核心补原语」而不是「搬运 CodeEditor」

`vgui_editor_kit` 没有 SDL3 硬依赖，`VGUI_CORE_ONLY` 下也可构建。语法高亮主题、行号栏、查找替换等是编辑器层能力，搬运 CodeEditor 只会让可选组件变成核心依赖。核心只提供统一的 span 绘制和测量接口；文本、样式区间和文档模型仍由宿主维护，不新增控件，也不解析标记语言。

## 2. 已完成（阶段 1–2 及后续样式扩展）

| 项目 | 当前实现 |
| --- | --- |
| 公共 C API | [`<vgui/richtext.h>`](../include/vgui/richtext.h) 提供 `vgui_text_span`、绘制与测量接口，纳入安装与公共头自包含检查；[`gui/richtext.cpp`](../gui/richtext.cpp) 随核心编译，`VGUI_CORE_ONLY` 可用。 |
| 正文样式 | span 使用 UTF-8 字节区间，支持乱序与重叠、颜色、字号、字重、倾斜。同一行内不同字号的 run 共享基线。 |
| 换行与测量 | 支持 LF/CRLF 显式换行，原有 `vgui_widget_measure_text_spans()` 和 `vgui_widget_get_text_height_spans()` 仍测显式行。新增 `vgui_custom_draw_text_spans_wrapped()` / `vgui_widget_measure_text_spans_wrapped()`，共用有限宽度下的自动断行，测得最长行的前进宽度及总高度（含显式空行）。没有文本参数的 `vgui_widget_get_line_height_spans()` 只按 span 样式和基础样式估算行高。 |
| 装饰（D1–D4） | 下划线、删除线、上划线可同时使用；支持实线、双线、点线、虚线、波浪线。默认使用字体度量，可逐线覆盖颜色、粗细、偏移、线型和标志；相邻等样式装饰可连续绘制。 |
| 发光（G1–G4） | 支持控件默认样式和 span 覆盖、内外发光、强度与混合模式、装饰参与遮罩。同一行相邻且参数相同的 run 可共用光晕遮罩。 |
| 实际接入 | [`markdown_view`](../gui/editor/markdown_view.cpp) 使用 span 绘制行内样式；[`vgui_richtext_demo`](../sample/vgui_richtext_demo.cpp) 在完整 SDL3 构建中展示混排、对齐、装饰、避墨和发光。 |

### 2.1 调用语义

- **区间与优先级**：`start/length` 先限制在文本范围内，再扩展到 UTF-8 字符边界。正文颜色、字号、字重和倾斜由原数组中最后一个覆盖该区间的 span 整体决定，与 `start` 的顺序无关。为 0 的颜色、字号用调用者的基础值，字重、倾斜用控件值，不从外层 span 继承。
- **装饰与发光独立**：每种装饰线分别取最后一个设置该位的 span；未设置装饰位不会清除前面的装饰。光晕可继承早先覆盖的 span 或控件默认值；非零 alpha 覆盖，零 alpha 配合 `VGUI_TEXT_GLOW_OFF` 清除。
- **绘制与裁剪**：在 `VGUI_CUSTOM` 回调内调用 `vgui_custom_draw_text_spans()`。矩形为窗口像素坐标；水平逐行对齐，垂直按整段对齐，物理方向不因 RTL 翻转。正文和装饰裁在指定矩形内；外光可多延伸一段模糊边距，仍受已有父/控件裁剪限制。装饰与发光不改变排版宽高。
- **空输入**：没有 spans 时按基础样式绘制每一行；空文本不绘制，按实际文本测得的宽高为 0。

## 3. 当前限制与非目标

- **自动断行是显式选择**：原绘制接口仍只按显式换行，超宽裁剪。`_wrapped` 接口使用 ICU 默认 Unicode 断行规则，超长词才按字素簇回退；软断行使用的 ASCII 空格和制表符不画入两侧行。单个字素簇超过宽度时仍占一行，绘制裁剪，测量报告其完整行进宽度。没有语言特定的断字或自动连字符。
- **没有跨 span 的完整整形与双向重排**：当前将每个 run 分别交给 OVG 整形，再按逻辑顺序摆放；不保证跨 run 的字距调整、连字、阿拉伯文连写或整行双向重排。UTF-8 字符边界不等于字素簇边界，例如基字与组合附标、ZWJ 表情仍可能被 span 切开。
- **装饰与特效有降级边界**：避墨只用于下划线和上划线，按字形包围盒留空，不做轮廓精确求交；波浪线参与发光时使用近似遮罩。发光依赖后端支持及效果预算，不支持或达到上限时不应依赖光晕显示，正文仍按原样绘制。
- **没有可复用的 VGUI 富文本布局对象或 RGBA 输出接口**：每次富文本绘制仍需解析 span、构建 run 与行盒。OVG 的 `vg_render_text_rgba8()` 以及 `vg_create_text_rgba_layout()` / `vg_render_text_layout_rgba8()` 可缓存正文像素或复用正文布局，但不是 VGUI span 布局，且不含装饰和光晕。
- **行内图片不等于媒体解析**：核心通过 U+FFFC 标记与宿主纹理做图文混排；Markdown 仍只支持独立媒体块，不解析行内图片。接口见 [§6](#6-行内图片公开-api)。
- **不是文档编辑器**：没有标记语言解析、字符命中/光标映射、选择编辑或 WYSIWYG 样式文档模型。词法器、行号、查找替换及编辑器组件仍由 kit 或宿主提供。

## 4. 验证入口与覆盖范围

| 入口 | 已有覆盖 |
| --- | --- |
| `vgui_model`（[`test_richtext_spans`](../sample/vgui_model_test.cpp)） | 单段/多段、混排字号、空输入、乱序重叠、UTF-8 字节边界与异常区间。 |
| `vgui_richtext`（[专项测试](../sample/vgui_richtext_test.cpp)） | 变体字体宽度、混排基线、CRLF 与末尾空行、父裁剪恢复与 2× DPI、逐线覆盖、避墨、光晕继承与绘制顺序、装饰光晕合批；跨 span 的中英及补充平面字符、组合附标、潜在连字、阿拉伯文/希伯来文与拉丁文混排。 |
| `ovg_font_style`、`ovg_text_rgba`、`ovg_text_glow`、`ovg_text_glow_gpu` | 字体变体、正文 RGBA 缓存、光晕命令/预算及 GPU 像素验证；GPU 项需要 SDL3/Vulkan，不适用于 Core-only。 |
| `vgui_richtext_demo` | Windows 原生窗口已验证对齐、内外光切换、模糊强度、避墨与缩小窗口后的滚动。 |

跨 span 用例验证 UTF-8 边界、样式归属、测量与绘制一致、共享基线，以及 RTL 下的物理对齐。自动换行用例核对词间断行、中文与拉丁文之间的断行、组合序列、空行、超宽字素簇，以及逐行装饰和光晕。测试观察绘制命令，不验证跨 span 的最终字形或整行 Bidi 视觉顺序。

完整 Debug 的上述 6 项 CTest 及 Core-only 的 `vgui_model`、`vgui_richtext` 已通过。现有 Release 10/50 个发光标签的基准测的是 OVG 标签渲染，不代表长段落或高 span 密度的性能；多 span 段落基准仍待补齐。

## 5. 后续阶段（未纳入本期）

- **富文本原语**：若优先支持宿主复用，先明确布局缓存的失效条件（文本、span、字体、DPI 等）和是否包含装饰/光晕；若优先做通用段落，先设计跨 run 整形、Bidi 重排及其与自动断行的组合，再验证整行视觉结果。
- **编辑器阶段 3**：考虑将通用撤销/事务抽象与搜索能力做成核心或独立可选组件。
- **编辑器阶段 4**：只有明确需要普通应用在不依赖 kit 的情况下使用 CodeEditor，才考虑整体搬运并提供兼容期。
- **编辑器阶段 5**：WYSIWYG 单独立项，包含样式化文档模型和样式区间与编辑位置的双向映射。

## 6. 行内图片公开 API

> **已实现。** 声明位于 [`<vgui/richtext.h>`](../include/vgui/richtext.h)，保留现有 `vgui_text_span` 的结构布局及所有旧入口的行为，`VGUI_CORE_ONLY` 可用。

### 6.1 C 声明

用 UTF-8 的 **U+FFFC OBJECT REPLACEMENT CHARACTER** 在原文中标记图片位置，再用独立数组绑定 `vgui_texture*`。标记在文本中有明确的字节位置，图片前后的 span 继续引用同一份文本；不需要给样式 span 加纹理字段，也不用另建一套文本片段模型。

```c
#define VGUI_RICHTEXT_IMAGE_MARKER "\xEF\xBF\xBC"

typedef struct vgui_richtext_image {
    int byte_offset;       /* First byte of a U+FFFC marker in content.text. */
    vgui_texture* texture; /* Borrowed; NULL reserves a blank box. */
    float width, height;   /* Positive box dimensions in logical UI units. */
    float descent;        /* Box distance below baseline, in logical UI units. */
    vgui_color tint;      /* AARRGGBB; 0 = untinted (white). */
} vgui_richtext_image;

typedef struct vgui_richtext_content {
    const char* text;                 /* NUL-terminated UTF-8. */
    const vgui_text_span* spans;
    int span_count;
    const vgui_richtext_image* images;
    int image_count;
} vgui_richtext_content;

typedef enum vgui_richtext_wrap_mode {
    VGUI_RICHTEXT_WRAP_NONE = 0,      /* Explicit LF/CRLF only. */
    VGUI_RICHTEXT_WRAP_UNICODE = 1    /* Existing Unicode soft-wrap policy. */
} vgui_richtext_wrap_mode;

VGUI_API void vgui_custom_draw_richtext(vgui_widget* widget,
    struct ovg_ctx_cb* cb, struct rvg_t* vg, const vgui_richtext_content* content,
    float x, float y, float width, float height, vgui_richtext_wrap_mode wrap,
    vgui_color base_color, float align_x, float align_y, float base_font_size);

VGUI_API void vgui_widget_measure_richtext(vgui_widget* widget,
    const vgui_richtext_content* content, float max_width, vgui_richtext_wrap_mode wrap,
    float base_font_size, float* out_width, float* out_height);
```

### 6.2 布局与资源契约

- **绑定**：`byte_offset` 必须精确指向原文中 `EF BF BC` 的第一个字节。不截断或扩展该偏移，不绑定普通字符，不做隐式插入。图片记录可乱序；负值、越界、未指向标记、非有限尺寸或非正宽高的记录忽略，同一位置最后一条有效记录胜出。`descent` 必须有限，超出 `[0, height]` 时夹紧。未绑定的 U+FFFC 仍作为普通文本交给字体处理。数组为 `NULL` 或计数不大于 0 时按空数组处理。
- **度量与对齐**：图片是一个不可分割的盒子，前进宽度为 `width`，基线上方占 `height - descent`，下方占 `descent`。默认 `descent = 0`，即盒底落在基线上。图片盒和同一行文字共同计算行高；仅有图片的行由图片度量决定，无文字也无图片的显式空行仍用基础字体度量。图片尺寸随 `ui_scale` 转为设备像素；绘制矩形、`max_width` 和测量结果使用窗口像素，`base_font_size` 仍是逻辑字号。
- **断行**：LF/CRLF 保持强制换行。自动模式在原文上计算 Unicode 断行机会，用图片宽度代替占位符字形宽度；图片可整体移到下一行，不能从中间拆分。单图超过可用宽度时独占一行，不因换行缩小，绘制裁剪但测量仍报告完整宽度。仅显式换行模式忽略测量的 `max_width`。测量和绘制共用布局；测量不受绘制高度限制，宽高输出分别可空。无内容、无效 `wrap`，或自动模式下 `max_width` 非有限或不大于 0 时，测量输出为 0。无效绘制矩形和 `base_color == 0` 不提交绘制，与现有入口的使用方式一致。
- **绘制**：图片居中等比适配盒子，以盒子而非纹理实占矩形参与排版。图片裁在内容矩形与已有控件裁剪的交集内，纹理使用自己的 `tint` 和控件透明度。跨过 U+FFFC 的 span 只影响两侧文字；装饰、避墨和光晕不作用于图片，连续装饰和光晕合批都在图片处断开。文字光晕仍按现有契约裁剪。
- **所有权**：字符串与数组只在调用期间借用；纹理须与 `widget` 所在渲染上下文兼容，所有权仍在宿主。纹理及其后端资源要按现有纹理的生命周期规则存活，不能在仍有未消费的绘制记录引用它时销毁。`texture == NULL` 时只占位、不提交纹理绘制；测量不读取或上传纹理。宿主将空指针替换为已就绪纹理后，在按需渲染模式下调用 `vgui_widget_mark_dirty()`；只替换纹理不会改变既定盒子大小。

### 6.3 调用示例

```c
/* Inside VGUI_CUSTOM draw callback; status_texture may still be NULL. */
const vgui_text_span spans[] = {
    { .start = 11, .length = 6, .color = 0xFF7DE2F3u }
};
const vgui_richtext_image images[] = {
    { .byte_offset = 7, .texture = status_texture, .width = 20, .height = 20, .descent = 3 }
};
const vgui_richtext_content content = {
    .text = "Status " VGUI_RICHTEXT_IMAGE_MARKER " online",
    .spans = spans, .span_count = 1,
    .images = images, .image_count = 1
};
float measured_width, measured_height;
vgui_widget_measure_richtext(widget, &content, width, VGUI_RICHTEXT_WRAP_UNICODE,
    16.0f, &measured_width, &measured_height);
vgui_custom_draw_richtext(widget, cb, vg, &content, x, y, width, height,
    VGUI_RICHTEXT_WRAP_UNICODE, 0xFFFFFFFFu, 0.0f, 0.0f, 16.0f);
```

### 6.4 首期范围与验收

- 复用现有富文本断行、基线和纹理绘制路径；不做 URL 加载、Markdown 行内图片解析、任意嵌入控件、点击命中/复制替代文字或 RGBA 富文本缓存。
- [`vgui_richtext_test`](../sample/vgui_richtext_test.cpp) 覆盖图文同基线、多图与纯图片行、跨图断行、超宽图独占行、空纹理占位与替换、无效偏移和重复绑定、跨图装饰与光晕中断、2× DPI 与测量/绘制一致；该测试可在 Core-only 运行。完整构建的 [`vgui_texture_test`](../sample/vgui_texture_test.cpp) 另用 Vulkan 像素回读验证真实纹理的等比适配及内容和父控件裁剪。
