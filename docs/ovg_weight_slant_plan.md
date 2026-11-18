# OVG 文本样式支持规划：weight / slant / decoration / glow

> 状态：O1–O3、D1–D4、G1–G4 已落地；O4 已验证回归矩阵和字体缓存生命周期，绘制耗时与独立泄漏检测尚未验收。后文的缺口清单是实现前的基线；最终接口、限制和测试见下方实施记录。
>
> 验证入口：`ovg_font_style`（静态、变量字面素材可通过环境变量传入，见测试文件）、`vgui_richtext`、`ovg_text_glow`、`ovg_text_glow_gpu`（需 SDL3/Vulkan）、`ovg_text_rgba`。`vg_render_text_rgba8` 可用于缓存正文像素，不包含装饰或光晕。
>
> 兼容性：未使用新样式的调用方仍走原字体选择；富文本装饰线改用实际度量。公共结构新增字段后，下游需用新头文件重新编译。
> 范围：`third_party/ovg` 字体层（weight/slant 主 + decoration 度量 + glow 矢量 pass）+ `vgui` 文本 API / `richtext` / `markdown_view`（集成）
> 关联：`docs/richtext_roadmap.md` §3 曾把「粗体/斜体」列为非目标，原因就是本文第 2 节列出的 OVG 字体层缺口；decoration 追加规划见第 10 节；glow 追加规划见第 11 节。

---

## D4/G4 实施记录

- **D4**：富文本支持实线、双线、点线、虚线和波浪线。`vgui_text_span::{underline,strikethrough,overline}` 可用 `VGUI_TEXT_DECOR_PRESENT_*` 分别覆盖颜色、粗细、偏移、线型和标志；未设置的字段继续使用 `decor_*`。LF/CRLF 显式换行，每行独立对齐、定基线和画线，不自动折行。`vgui_widget_measure_text_spans()` 返回最长行宽；`vgui_widget_get_text_height_spans()` 返回含空行的总高度。
- **Skip-ink**：下划线和上划线可通过 `VGUI_TEXT_DECOR_FLAG_SKIP_INK` 跳开墨迹。使用同一字体变体和整形位置的字形包围盒，保守留出间距，不做曲线级轮廓求交；删除线仍穿过字身。
- **G4**：`vgui_text_glow` 新增 `mode`、`blend`、`opacity`，控件可用 `vgui_widget_set_text_glow_style()` 设置，span 可单独覆盖强度。默认仍为外光和 source-over；内光在正文后提交，裁在字形遮罩内。ADD 累加预乘 RGB，但保留目标 alpha，适合直接叠加到已有颜色的目标。可用 `VGUI_TEXT_GLOW_DECORATION` 让 span 范围内的装饰加入遮罩；波浪线光晕遮罩用窄矩形近似。强度按 span/run 调整，不在单个已整形字形（例如连字）内部另设强度。
- **合批与降级**：一行中相邻、同光晕参数的 run 保留各自的整形位置，合用一个遮罩。OVG 超过 127 个 effect 槽时只省略光晕，保留正文；`ovg_text_glow_budget_drops()` 可观察槽位丢弃数。OVG SDL3 以可配置的 `ovg_fx_set_budget()` pending 预算约束每帧效果纹理；vgui SDL3 使用包含在途纹理的 96 MiB 上限。预算不足同样仅省略效果。

验证：`vgui_richtext`（多行、线型、skip-ink、装饰光晕合批）、`ovg_text_glow`（提交顺序、槽位耗尽）、`ovg_sdl3_effect_blend` 与 `vgui_sdl3_effect_blend`（Vulkan 像素对比）纳入 CTest。

## O4 验收记录

- `ovg_font_style` 增加 16 轮字体缓存重建，覆盖 `clear_load()`、`clear_sys()`、系统字面重新枚举、内存字面重注册，以及失效 family 的派生与销毁。每轮验证清理前后的 advance 与 RGBA 像素一致；累计 1,024 次同样式派生确认复用同一字面且不消耗新 font id。此项不测绘制耗时。
- `vgui_model` 使用仓库内的 DejaVu Sans，加入完整构建与 `VGUI_CORE_ONLY` 的 CTest。两种配置都测异步文件对话框桥接；仅完整构建验证 SDL3 provider 安装。
- Windows 验证：完整 SDL3/Vulkan 构建 16 项 CTest（含截图）、Core-only 8 项 CTest、MSVC ASan 下 7 项文本与生命周期 CTest 均通过；编辑器套件、C API 与电子表格独立自检通过。可选的真实粗体、粗斜体及变量字面用例分别使用 Arial Bold、Arial Bold Italic 和 Bahnschrift。Windows ASan 结果不等同于 LeakSanitizer 验收。

## 0. 结论摘要

1. **可行，且不必改渲染管线**。工程内 HarfBuzz 14.3.1 已提供三条路：
   - 静态字面：直接选 Bold / Italic / Bold Italic 的字体文件；
   - 变量字体：`hb_font_set_variation()` 写 `wght` / `slnt` / `ital` 轴；
   - 合成回退：`hb_font_set_synthetic_bold()` / `hb_font_set_synthetic_slant()`。
2. **真正的工作量不在「把参数传给 HarfBuzz」**，而在**给每个 `(face, weight, slant)` 组合一个独立的 `hb_font_t` 与缓存身份**。现有 OVG 把可变字体状态、字形轮廓缓存、图集缓存都绑在一个共享 `hb_font_t` 上；如果直接在共享字体上改 variation，会让已缓存字形串味。
3. 建议分四步（glow 另开第三条轨道）：
   - **O1**：公共 API + 静态字面最近匹配 + 自定义字体元数据 + 合成回退。这一步就能让当前只有 `DejaVuSans.ttf` 一个 Regular 字面的 demo 显示出粗体和斜体。
   - **O2**：变量字体 `wght` / `slnt` / `ital` 轴。
   - **O3**：vgui 暴露——控件级字重/倾斜、`vgui_text_span` 逐 span 样式、按 run 样式测量/换行/行高，`markdown_view` 去掉「双绘假粗体」。
   - **O4**：回归、性能、ASAN、文档。
   - glow 作为第三条轨道 G1–G4，不阻塞 O1–O4 的顺序，详见 §11.8。
4. **默认路径必须保持现状**：`weight=0` / `slant=0`（老 API）继续走现有选择逻辑，现有截图回归目标是不出现字形/排版变化。新能力通过 `new_font_family_ex()` 显式启用。
5. **decoration 是并行的第二条线**：现有下划线/删除线是 vgui 层手画，位置/粗细用行高与字号的经验值，未使用字体度量，也没有 overline、样式、颜色、偏移和连续区间语义。推荐 **OVG 补字体装饰度量（underline/strikeout/x-height/cap-height），vgui 负责布局与绘制**；详见第 10 节 D0–D4。
6. **decoration 与 weight/slant 的交汇点**：每个字重/倾斜变体必须使用自己的 `hb_font_t` 度量（MVAR/合成可能改变度量）；richtext 的基线、行盒和装饰线都按变体 family 计算。D1 随 O1 落地，D2/D3 随 O3 落地。
7. **glow 不能直接复用现有 `vg_effect_t`**：效果只对 `fill/stroke` 生成的 `DRAW_VG` 生效，而文本主路径是 raster atlas 的 `add_geometry`（`DRAW_GEOM`），没有 effect 槽位。推荐在 `text_style_t` 追加 glow 字段，单独提交一次「批量矢量轮廓 + `VG_EFFECT_OUTER_GLOW` + 不可见填充（不透明色 + state opacity=0）」的 pass；一个 run 只做一次 mask/blur，详见第 11 节。vgui 用 widget/span glow + 两遍绘制接入，现有 demo 的叠副本方案可退役。

---

## 1. 现状与代码证据

### 1.1 已经具备的基础

| 能力 | 现状 | 证据 |
|---|---|---|
| 字重/倾斜字段 | `FontStyle` 内部已有 `weight` / `slant`，来自 fontconfig | `third_party/ovg/ovg/ovg_fonts.cpp:384-395` |
| 系统字体枚举 | `get_sys_family()` 枚举 `FC_FAMILY/FC_STYLE/FC_WEIGHT/FC_SLANT/FC_FILE/FC_INDEX` | `ovg_fonts.cpp:650-698` |
| 内部选字接口 | `get_font(family, style, weight, slant)` 已接受四个参数 | `ovg_fonts.h:192`、`ovg_fonts.cpp:699-710` |
| 字面加载 | `load_font(file, index)` 从文件创建 `hb_font_t` | `ovg_fonts.cpp:546-566` |
| 文本整形 | HarfBuzz shape + 字形缓存 + 图集/矢量双路径 | `ovg_fonts.cpp:2718-2831`、`ovg_fonts.cpp:1064-1181` |
| HarfBuzz 能力 | 14.3.1，支持 `hb_font_set_variation`、`hb_font_set_synthetic_bold/slant`、`hb_style_get_value` | `vcpkg.json:37-39`、HarfBuzz 头文件 |

### 1.2 关键缺口

| # | 缺口 | 说明 | 证据 |
|---|---|---|---|
| G1 | **公共 API 不可达** | C 接口 `new_font_family()` 只有 `familys, styles` 两个参数，内部恒传 `weight=0, slant=0` | `ovg_c.h:1057`、`ovg.cpp:7039-7079`（7062 行） |
| G2 | **匹配是精确相等** | `vt->weight == weight`、`vt->slant == slant`，没有最近字重/倾斜回退；`weight<1` 只是通配 | `ovg_fonts.cpp:805-847`（820-821 行） |
| G3 | **通配时首字面顺序不确定** | `weight=0` 会把该 family 下所有字面塞进 `_temp`，`new_font_family` 取 `_temp[0]`；结果依赖 fontconfig 枚举顺序 | `ovg_fonts.cpp:699-710`、`ovg.cpp:7062-7072` |
| G4 | **变量字体轴没有真正应用** | 只检查了 `slnt` 轴，`slnt_applied=true` 后什么也没做；`hb_font_set_variation` 被注释；没有 `wght` / `ital` 检测 | `ovg_fonts.cpp:616-649`、`ovg_fonts.cpp:861-865` |
| G5 | **没有合成分支** | 字面不存在时直接回退到默认字面，不会合成粗体/斜体；工程内 `sample/res/fonts` 只有 `DejaVuSans.ttf`（Regular） | `sample/res/fonts/`、`ovg_fonts.cpp:848-868` |
| G6 | **自定义内存字体没有元数据** | `register_font_memory()` 只读内部族名，`style/weight/slant` 保持 0；请求 Bold/Italic 永远匹配不到 | `ovg_fonts.cpp:752-803`（774-779 行） |
| G7 | **缓存身份绑定共享字体** | 字形缓存 key 是 `u16 font_id + 字号 + 字形`；`font_id` 是每个 `FontStyle` 一个。若在共享 `hb_font_t` 上改 variation，缓存不会失效，会返回错误轮廓 | `ovg_fonts.h:152-158`、`ovg_fonts.cpp:1081-1122`、`ovg_fonts.cpp:1125-1177` |
| G8 | **生命周期清理不完整** | `clear_sys()` / `clear_load()` 释放字体，但不清理 `glyph_cache` / `_sub_glyph_cache` / 测量缓存；`get_sys_family()` 重复调用会把同一字面重复插入 `_familys` | `ovg_fonts.cpp:516-544`、`ovg_fonts.cpp:650-698` |
| G9 | **vgui 没有样式入口** | `text_style_t` 没有 weight/slant；vgui 绘制固定 `st.family = ctx->font`；`vgui_text_span` 只有颜色/字号/装饰 | `ovg_c.h:791-802`、`gui/vgui.cpp:11876-11890`、`include/vgui/richtext.h:29-36` |
| G10 | **已有假粗体** | `markdown_view` 的 `strong` 目前靠同一文本画两遍、第二遍偏移 0.6px 模拟；`italic` 只改颜色 | `gui/editor/markdown_view.cpp:2677-2684` |
| G11 | **装饰位置靠经验值** | 下划线画在 `run_y + line_height - thickness`，删除线画在 `line_height * 0.55`，没有用基线或字体度量 | `gui/richtext.cpp:165-175` |
| G12 | **装饰粗细靠字号比例** | `max(1, font_size * 0.06)`，不读字体 underline/strikeout thickness | `gui/richtext.cpp:168` |
| G13 | **装饰能力不完整** | 只有下划线/删除线两个位；没有 overline、线型、装饰颜色、粗细/偏移覆盖、skip-ink | `include/vgui/richtext.h:19-24`、`gui/richtext.cpp:117-123` |
| G14 | **嵌套 span 会丢装饰** | 装饰属于 paint span，重叠时后者整体胜出；内层无 decor 的 span 会清掉外层下划线 | `gui/richtext.cpp:92-113` |
| G15 | **连续装饰被 run 切断** | 每个 run 单独画一条线，相邻同装饰 run 不合并，也不跨空格连续 | `gui/richtext.cpp:156-177` |
| G16 | **裁剪与基线不完整** | 水平只按 `available` 截断，垂直方向未夹到矩形；混排字号按行高居中而不是共享基线，装饰 y 也随之漂移 | `gui/richtext.cpp:139-160`、`gui/richtext.cpp:167-174` |
| G17 | **OVG 没有装饰度量** | `vg_font_metrics_t` 只有 `ascent` / `line_height`，`vg_get_font_metrics()` 没有 underline/strikeout/x-height/cap-height | `ovg_c.h:505-508`、`ovg_fonts.cpp:1533-1570` |

### 1.3 现有调用链（为什么 G1 是总开关）

```text
new_font_family(ctx, "A,B", "Regular,Bold")
  └─ get_font(family, style, 0, 0)          // ← weight/slant 永远为 0
       └─ mk_font()
            ├─ 按 family/style/weight/slant 精确过滤
            ├─ load_font(file, index)
            └─ hb_res_init()                // 只初始化默认实例
  └─ _temp[0] -> font_familys_t
```

因此现状下：

- 传 `styles="Bold"` 可以选到系统里的 Bold 字面（如果 style 字符串完全一致）；
- 传 `weight` / `slant` 永远不行；
- 只有 Regular 一个文件时，`styles="Bold"` 也找不到，最终回退到 Regular；
- 变量字体永远渲染默认实例。

---

## 2. 方案对比

| 方案 | 做法 | 优点 | 缺点 | 结论 |
|---|---|---|---|---|
| A. 只做真实字面 | 请求 Bold/Italic 时在 family 内找对应文件 | 质量最好、无合成伪影 | 单字重字体（当前 DejaVuSans）无解；CJK 大量字体没有 Italic | 必须做，但不能单独用 |
| B. 变量字体轴 | 对 fvar 字体设置 `wght` / `slnt` / `ital` | 连续字重、设计质量好 | 只覆盖变量字体；仍需独立 `hb_font_t` 与缓存身份 | O2 做 |
| C. HarfBuzz 合成 | `hb_font_set_synthetic_bold` / `synthetic_slant` | 任意字体可用；HB 同步调整 advance/轮廓，渲染和排版一致 | 观感不如真实字面；过粗会糊 | 作为 A/B 的回退，默认开启 |
| D. 渲染层假粗/斜 | 双绘偏移、矩阵斜切 | 不改字体层 | 复制 draw 命令、度量/选区/下划线全部要另算；缩放/子像素质量差 | **不采用**；现有 markdown 双绘应删除 |

推荐策略：

```text
每个 fallback family 内独立解析：
  真实静态字面（weight + slant 都命中）
    > 变量字体轴（真实轴覆盖）
    > 最近静态字面 + 缺失维度合成
    > 默认字面（仅当请求为 0）
```

---

## 3. 目标与非目标

### 3.1 目标

- 公共 C API 支持按 weight/slant 创建 `font_familys_t`，系统字体与自定义内存字体一致。
- 支持真实字面、变量字体轴、HB 合成三层回退。
- 每个 fallback family 独立解析，缺字时仍按原 family 列表回退。
- 变体字体与字形/测量缓存有稳定、唯一、可清理的身份。
- vgui 能按控件和 `vgui_text_span` 使用不同字重/倾斜，测量与绘制一致。
- OVG 暴露与字体变体一致的 underline/strikeout/x-height/cap-height 度量；vgui 的装饰线以基线为准，支持颜色/粗细/偏移覆盖、连续区间和完整裁剪。
- 默认调用路径保持现有截图不变。

### 3.2 非目标（本期）

- 不实现完整的 CSS font matching 引擎（不处理 `font-stretch` / `opsz` / 任意 OpenType feature 组合）。
- 不做完整的 CSS `text-decoration` 引擎：`text-emphasis`、多行装饰、逐字形 skip-ink 放到 D4；本期装饰按整条 run 绘制。
- 不做可编辑富文本文档模型；只做绘制/测量原语。
- 不做字体文件自动下载、字体子集化、字体授权管理。
- 不把 `hb_font_t` 改成线程安全；仍遵循 OVG 单线程约束。
- 不承诺 C++ ABI；只保证源码兼容，新增字段/函数均为追加式。

---

## 4. 公共 API 设计（ovg_c.h）

### 4.1 字重与倾斜枚举

字重采用 **OS/2 `usWeightClass` / CSS `font-weight` 的 100–900** 语义，而不是 fontconfig 的 0–215 内部刻度。原因：

- `wght` 轴、`usWeightClass`、CSS 都使用 100–900；
- HarfBuzz `hb_style_get_value(HB_STYLE_TAG_WEIGHT)` 返回的也是这一语义；
- 公开 API 不应该泄漏 fontconfig 常量。

```c
/* 字重：0 = 不指定；100..900 对应 OS/2 usWeightClass / CSS font-weight。 */
typedef enum vg_font_weight_t {
    VG_FONT_WEIGHT_DEFAULT      = 0,
    VG_FONT_WEIGHT_THIN         = 100,
    VG_FONT_WEIGHT_EXTRA_LIGHT  = 200,
    VG_FONT_WEIGHT_LIGHT        = 300,
    VG_FONT_WEIGHT_SEMI_LIGHT   = 350,   /* 可选，映射 FC_WEIGHT_DEMILIGHT */
    VG_FONT_WEIGHT_REGULAR      = 400,
    VG_FONT_WEIGHT_MEDIUM       = 500,
    VG_FONT_WEIGHT_SEMI_BOLD    = 600,
    VG_FONT_WEIGHT_BOLD         = 700,
    VG_FONT_WEIGHT_EXTRA_BOLD   = 800,
    VG_FONT_WEIGHT_BLACK        = 900
} vg_font_weight_t;

/* 倾斜：0 = 不指定；NORMAL/ITALIC/OBLIQUE 与 fontconfig 语义对齐。 */
typedef enum vg_font_slant_t {
    VG_FONT_SLANT_DEFAULT = 0,
    VG_FONT_SLANT_NORMAL  = 1,
    VG_FONT_SLANT_ITALIC  = 2,
    VG_FONT_SLANT_OBLIQUE = 3
} vg_font_slant_t;
```

vgui 不包含本头，另有数值一致的 `vgui_font_weight` / `vgui_font_slant` 别名，见 §10.4.2。

### 4.2 创建与派生

```c
/* 与 new_font_family 相同，但按字重/倾斜解析。
 * - familys: 逗号分隔的 fallback 列表，沿用现有语义。
 * - styles:  可为 NULL；非空时是逐 family 的精确子族名列表，优先于 weight/slant。
 * - weight:  vg_font_weight_t，0 = 不指定。
 * - slant:   vg_font_slant_t，0 = 不指定。
 * 返回对象由调用方 delete_font_family()；底层 FontStyle/hb_font 由 font_cache 持有。
 * 默认路径：weight=0 && slant=0 时行为与 new_font_family 完全一致。 */
font_familys_t* new_font_family_ex(font_cache_cx* p, const char* familys,
    const char* styles, int weight, int slant);

/* 从已有 family 派生同 fallback 列表、同 styles 的新 weight/slant 变体。
 * base 必须由 new_font_family() / new_font_family_ex() 创建（内部 magic tag 校验）；
 * 派生复用 base 的 font_cache_cx，并读取 base 保存的 familys/styles 请求。
 * 失败返回 NULL（调用方应回退到 base）。 */
font_familys_t* font_familys_derive(const font_familys_t* base,
    int weight, int slant);

/* 查询一个已解析字面的实际生效值；未知返回 0。
 * 对 fallback 列表，family 级查询返回主 family 的值。 */
void font_family_get_style(const font_family_t* face, int* weight, int* slant);
void font_familys_get_style(const font_familys_t* ffs, int* weight, int* slant);
```

`new_font_family()` 保留为 `new_font_family_ex(p, familys, styles, 0, 0)` 的薄包装，不改变现有调用方。

### 4.3 语义约定

- **weight=0**：不指定，走旧路径；**weight>0**：按最近匹配。
- **slant=0**：不指定；`NORMAL` 优先 Roman，`ITALIC` 优先 Italic、次选 Oblique，`OBLIQUE` 优先 Oblique、次选 Italic。
- **style 优先级**：`styles` 非空时先按精确 style 过滤；若该 family 没有精确 style，再按 weight/slant 匹配，而不是直接失败。新代码建议 `styles=NULL`。
- **fallback**：对 `familys` 里的每个 family 独立解析；某个 family 解析失败时使用它的默认字面并视情况合成，不能因为该 family 没有 Bold 就跳过它（否则丢字）。
- **单字面 family**：把它当作该 family 的基准字面；只有请求更粗时才合成，**不做负向合成变细**。请求 Regular 但只有 Bold 文件时，直接使用 Bold（比缺字好）。
- **彩色/位图字体**（emoji、COLR/CPAL、CBDT 等）：按原样使用，不施加合成粗体/斜体，避免破坏颜色字形。
- **返回值**：所有字体仍由 `font_cache_cx` 持有；删除 family 只释放容器，不释放共享字体。

---

## 5. 内部设计

### 5.1 数据模型：基面（base face）与变体（variant）

现有 `FontStyle` 既是「文件描述」又是「运行时字体」，建议拆成两层语义（可以在同一个结构体里用字段区分）：

```cpp
struct FontStyle {
    // --- 基面字段（现状已有） ---
    uint32_t id;                 // 唯一 id，字形缓存 key 使用
    std::string family;
    std::set<std::string> alias;
    std::string style;
    std::string file;
    int index;
    vg_font font;                // 基面的 hb_font（legacy 路径使用）
    int weight;                  // 基面有效字重（建议内部统一为 CSS 100..900）
    int slant;                   // 基面有效倾斜（FC_SLANT_*）

    // --- 新增：变体字段 ---
    FontStyle* base = nullptr;   // 非空 = 变体，指向基面
    bool has_wght = false, has_slnt = false, has_ital = false;
    float wght_min = 0, wght_max = 0, wght_default = 0;
    float slnt_min = 0, slnt_max = 0, slnt_default = 0;
    float ital_default = 0;
    bool synthetic_bold = false, synthetic_slant = false;
};
```

变体注册表：

```cpp
struct variant_key {
    FontStyle* base;
    int weight;
    int slant;
    bool operator<(const variant_key&) const;
};
std::map<variant_key, FontStyle*> _variants;   // font_cache_cx 持有，寿命与 cache 一致
```

关键原则：

- **基面 `hb_font_t` 不施加任何 variation/synthetic**；legacy 路径继续用它。
- 每个变体有**独立 `hb_font_t`**。用 `hb_font_create(hb_font_get_face(base->font))` 或 `hb_font_create_sub_font(base->font)` 创建，创建后只设置一次 variation/synthetic，之后再不改动。
- 每个变体拥有**完整的 `vg_font` 资源**：自己的 `hb_font_t`、coverage、`rdr`、`pnt`、`hb_buffer_t`，不能复用基面的 rasterizer/paint 对象；`hb_raster_draw_t` / `hb_raster_paint_t` 内部持有字体状态，共享会把 variation/合成串到基面。
- 每个变体有**自己的 `id`**，字形缓存、图集缓存、`glyph_atlas_entry.font` 都指向该变体字体；`id` 在 `FontStyle` 创建时分配一次，之后只读（§5.6）。
- `_variants` 与 `_familys` / `_familys_name` 同寿命；`clear_load()` / `clear_sys()` / `free_font_cache()` 必须先清变体与字形/测量缓存，再释放基面。

### 5.2 解析算法

对 `familys` 中的每个 family：

```text
1. 收集候选基面：family 名精确匹配，或 alias/fullname 命中。
2. 若 styles[i] 非空：先保留 style 精确匹配的候选；为空则进入 3。
3. 为每个候选计算能力与代价：
   - wght_axis：fvar 是否有 wght；
   - slnt_axis / ital_axis：fvar 是否有 slnt / ital；
   - static_weight_match：基面 weight 是否等于请求；
   - static_slant_match：基面 slant 是否满足请求。
4. 评分（从高到低）：
   a. 真实命中维度数（weight、slant 各算一维）；
   b. 是否静态精确（静态精确优先于变量轴，便于保留 hinting/设计）；
   c. 字重距离（CSS 语义）；
   d. 倾斜优先级（italic > oblique > roman，或反向请求时对应反转）；
   e. 枚举顺序（稳定）。
5. materialize(选中基面, 请求 weight/slant)：
   - 有 wght 轴 → hb_font_set_variation(wght)；
   - 有 slnt 轴 → hb_font_set_variation(slnt, -12/-10 度)；
   - 有 ital 轴 → hb_font_set_variation(ital, 1/0)；
   - 缺失维度 → HB synthetic。
6. 若该 family 没有任何可用基面：使用默认字面并合成（或失败时回退到下一个 family）。
```

CSS 字重匹配建议（与静态字面比较时）：

- 请求 `400..500`：先精确，再取该区间内最近值，再取 `>500` 的最近值，最后取 `<400` 的最近值；
- 请求 `<400`：先向更轻找，再向更重找；
- 请求 `>500`：先向更重找，再向更轻找；
- 距离相同优先更粗（更符合 UI 的「至少够醒目」预期）。

倾斜匹配：

```text
NORMAL  : ROMAN > OBLIQUE > ITALIC（后两者仅在无 Roman 时使用）
ITALIC  : ITALIC > OBLIQUE > ROMAN + synthetic
OBLIQUE : OBLIQUE > ITALIC > ROMAN + synthetic
```

### 5.3 变量字体

- 用 `hb_ot_var_find_axis_info(face, tag, &info)` 检测 `wght` / `slnt` / `ital`，同时缓存 min/max/default。
- 应用：
  - `wght`：请求字重 clamp 到 `[min,max]` 后 `hb_font_set_variation(font, HB_TAG('w','g','h','t'), value)`；
  - `slnt`：Italic ≈ `-12°`，Oblique ≈ `-10°`，`hb_font_set_variation(font, HB_TAG('s','l','n','t'), angle)`；
  - `ital`：Italic/Oblique 为 1，Normal 为 0。
- 如果字体同时有 `ital` 和 `slnt`，两个都设：`ital=1` + `slnt=-12` 更接近设计意图。
- `hb_font_set_variation` 之后不要再改同一 `hb_font_t` 的轴值；该变体已经是不可变身份。
- 保留 fontconfig 的 `FC_INDEX`。HarfBuzz 支持 face index 高 16 位编码 named instance（见 `hb_font_create` 文档），不要把它截断。
- 变量字体仍可能缺某个字形；按现有 `resolve_family` 的 coverage 规则回退到下一个 family。

### 5.4 合成粗体 / 斜体

HarfBuzz 14 的合成接口直接在字体层生效，`hb_font_draw_glyph`、`hb_raster_draw_glyph` 和 advance 查询会保持一致：

```cpp
// 粗体：x/y_embolden 典型 0.01..0.05；in_place=false 让 advance 一起调整
hb_font_set_synthetic_bold(font, amount, amount, 0);
// 斜体：slant 是比例，不是角度；右倾约 0.2
hb_font_set_synthetic_slant(font, tan(12 * PI / 180));   // ≈ 0.2126
```

建议常量（待截图调优）：

| 请求 | 合成粗体量 |
|---|---|
| 500 / 600 | 0.010 – 0.015 |
| 700 | 0.025 – 0.030 |
| 800 | 0.035 – 0.040 |
| 900 | 0.045 – 0.050 |

约束：

- 只在「请求比基面更粗」时合成，不合成变细。
- 彩色/位图字体跳过合成。
- 合成参数在变体创建时设定，创建后不改；否则缓存失效。
- 需要验证 `hb_font_get_glyph_extents` / `hb_font_get_h_extents` 是否包含合成效果；如果 extents 不含，OVG 现有光栅尺寸计算需要加补偿（`build_glyph_image_hb` 的 `gext`）。这是 O1 的验收项之一。

### 5.5 自定义字体元数据

`register_font_memory()` 目前只读族名。需要补齐：

- **style**：`hb_ot_name_get_utf8(face, HB_OT_NAME_ID_TYPOGRAPHIC_SUBFAMILY / FONT_SUBFAMILY, ...)`，取不到则用 `WWS_SUBFAMILY` 或空串。
- **weight**：`hb_style_get_value(font, HB_STYLE_TAG_WEIGHT)`（CSS 100–900）；取不到时按 400。
- **slant**：
  - `hb_style_get_value(font, HB_STYLE_TAG_ITALIC) >= 0.5` → Italic；
  - 否则 `hb_style_get_value(font, HB_STYLE_TAG_SLANT_ANGLE) < -1` → Oblique；
  - 否则 Roman。
- **变量轴**：记录 `has_wght/has_slnt/has_ital` 与 min/max/default，供 `materialize` 使用。
- 如果 face 是 named instance（`hb_font_create` 的 index 高 16 位），style 名应优先用 named instance 的 subfamily name。
- `load_font_from_path(path, name)` 最终也走 `register_font_memory()`，元数据自动一致；`name == NULL` 时仍走 fontconfig 枚举，元数据来自 FC。
- TTC/OTC 的多元 face 暂只支持 index 0；如后续需要，增加 `load_font_from_memory_index()`，不要在本期偷偷改变现有签名。

### 5.6 缓存、身份与生命周期

**字形缓存**

- **稳定 id（O1 必做）**：`mk_font()` 目前在每次选择时执行 `it->id = next_font_id++`；同一个 `FontStyle` 被不同 `font_familys_t` 选中时 id 会变化，`glyph_cache` 的 key 随之漂移，旧字形永久占坑。改为在 `FontStyle` 创建时（`get_sys_family` / `register_font_memory`）分配一次 id，`mk_font()` 只匹配、不修改 id；基面创建也做 `next_font_id` 溢出检查，失败则拒绝注册该 face。
- 每个变体一个独立 `id`；`glyph_key` 继续用现有 `u16 font_id + u16 size + u32 glyph`。
- 变体数量通常是「2–3 个 fallback family × 4 种样式」，`u16` 足够；但要加：
  - `next_font_id` 溢出检查（>= 0xFFFF 时拒绝创建新变体并回退到基面）；
  - 变体数量上限（例如 256）与 LRU/清理策略，避免宿主按任意 weight 连续创建。
- 如果后续需要支持连续字重滑块，再把 key 扩成 32 位 font id + 32 位 glyph；本期不做，避免改热路径。

**测量缓存**

- `text_width_cache` / `glyph_advance_cache` / `font_metrics_cache` 已按 `font_familys_t*` 或 `(family,size)` 区分；派生 family 是不同对象，天然隔离。
- vgui 必须**缓存派生 family**，不能每帧 `new_font_family_ex`；否则缓存 key 每帧变化，测量/绘制都会退化。

**`font_familys_t` 与派生元数据**

`font_familys_cx` 目前只保存 `vg_vector<font_family_t*>` 和 allocator；`font_familys_derive()` 需要知道原始请求，`delete_font_family()` 需要清理缓存：

- `font_cache_cx* cache`：派生时直接使用同一个 cache；
- `uint32_t magic`：区分 ovg 创建的 family 与宿主自建（`font_familys_t` 是公开结构）；`derive` / `delete` 先校验，未命中走安全回退；
- `std::string family; std::string styles; int weight; int slant;`：保存 `new_font_family_ex` 的请求；`derive` 用同样的 familys/styles 请求新的 weight/slant，而不是从 `_temp` 猜测；
- `delete_font_family()` 除释放 family 外，还要清掉 `text_width_cache` / `glyph_advance_cache` / `font_metrics_cache` 中 key 为该指针的条目；否则地址复用会命中旧度量。

**清理**

- 新增 `clear_variants()`：释放所有变体 `hb_font` / coverage / rdr / pnt / `hb_buffer`，清空 `_variants`。
- `clear_sys()` / `clear_load()` 顺序：`clear_variants()` → 清 `glyph_cache` / `_sub_glyph_cache` / 测量/度量缓存 → 释放基面。
- `get_sys_family()` 重复调用不能重复插入：按 `(file, index)` 去重；已有 `FontStyle` 保留，不释放（避免已创建的 `font_familys_t` 悬空）。
- 删除 `select_font_face()`（空实现）或标记 deprecated；`load_font(family, style)`、`font_get_slant_angle()` 等死代码在 O1 顺手清理，避免后续误用。
- `_emojis` 当前只收集不使用；如果保留，在变体解析里明确彩色字体跳过合成。

### 5.7 文件改动清单

| 文件 | 改动 |
|---|---|
| `third_party/ovg/ovg/ovg_c.h` | 新增 weight/slant 枚举、`new_font_family_ex`、`font_familys_derive`、`font_family_get_style`；`vg_font_metrics_t` 追加装饰度量字段 |
| `third_party/ovg/ovg/ovg_fonts.h` | `font_cache_cx` 增加变体注册表/解析/清理声明；`glyph_key` 暂不改 |
| `third_party/ovg/ovg/ovg_fonts.cpp` | `FontStyle` 变体字段与稳定 id（创建时分配）；FC↔CSS 转换；`get_sys_family` 元数据/去重；`register_font_memory` 元数据；`materialize`（变量轴 + 合成 + 完整 `vg_font` 资源）；缓存清理；删除死代码；`vg_get_font_metrics()` 读取 underline/strikeout/x-height/cap-height |
| `third_party/ovg/ovg/ovg.cpp` | `new_font_family` 变薄包装；新增 `new_font_family_ex` / `font_familys_derive` / 查询接口；`font_familys_cx` 保存 `cache` / magic tag / familys / styles / weight / slant；`delete_font_family` 清理该指针的缓存条目 |
| `third_party/ovg/README.md` | 字体章节补 weight/slant 用法与回退规则，以及装饰度量说明 |
| `sample/res/fonts/` | 增加 DejaVuSans-Bold / Oblique / BoldOblique（测试用；许可证与现有 DejaVu 一致） |
| `sample/ovg_font_test.cpp`（新） | 字体解析/合成/变量轴/缓存的 headless 测试 |
| `gui/vgui.cpp` | 控件级 weight/slant、variant family 缓存、文本绘制/测量入口 |
| `include/vgui/vgui.h` | `VGUI_FONT_WEIGHT_*` / `VGUI_FONT_SLANT_*` 公开别名（不要求宿主包含 `ovg_c.h`）；控件级 `vgui_widget_set_font_weight/slant` + getter；`vgui_text_metrics` 结构 + `vgui_widget_get_text_metrics_styled()` 返回生效字号/变体的装饰度量（vgui 自有结构，不暴露 `vg_font_metrics_t`） |
| `include/vgui/richtext.h` | `vgui_text_span` 追加 `font_weight` / `font_slant`；追加 `decor_color` / `decor_style` / `decor_thickness` / `decor_offset` / `decor_flags`；新增 overline、线型枚举 |
| `gui/richtext.cpp` | run 携带 weight/slant；span 测量/行高先按 run 的有效 style 选变体 family 再测宽；新增 decoration layer、基线对齐、连续合并、裁剪与多线型绘制 |
| `gui/editor/markdown_view.cpp` | 删除双绘假粗体，改用真实/合成 style；italic 真正倾斜；`measure()` / `wrap()` / 表格列宽 / 行高改为 style-aware；删除手画 strike/link 线，改走 span decoration |
| `sample/vgui_model_test.cpp` | 增加 decoration 度量/嵌套/连续/裁剪/基线测试；现有 5 字段 span 初始化保持兼容 |
| `docs/richtext_roadmap.md` | §3 更新：粗体/斜体/下划线/删除线不再是 OVG 缺口 |
| `README.md` / `docs/vgui_capability_assessment.md` | 文本能力描述更新 |

---

## 6. 分阶段实施

### O0 — 基线与素材（约 0.5–1 天）

- 用现有截图测试记录 Regular 文本基线（含 12/16/24/32px、中英混排、RTL、子像素）。
- 增加 DejaVu Bold/Oblique/BoldOblique 测试字体。
- 建 `sample/ovg_font_test.cpp` 骨架（headless，直接用 `font_cache_cx`）。
- 验收：基线可重复；测试能在 `VGUI_CORE_ONLY` 下跑。

### O1 — API + 静态字面 + 元数据 + 合成（约 2–3 天，MVP）

- 新增公共枚举/API；`new_font_family` 保持旧行为。
- 内部 weight 统一为 CSS 100–900；fontconfig 枚举时转换；`weight_to_string` 改为 CSS 名称。
- 实现 `resolve_face()`（精确/最近匹配）和 `materialize()`（合成分支）。
- `register_font_memory` 读取 style/weight/slant/变量轴。
- 变体注册表、独立 `vg_font` 资源、稳定 `FontStyle::id`、缓存清理、`get_sys_family` 去重。
- 测试：真实 Bold/Italic 优先；单 Regular 字体合成；fallback 链；自定义内存字体；默认截图不变。

验收：

- `new_font_family_ex(cache, "DejaVu Sans", NULL, 700, 0)` 非空，文本明显变粗，advance/图集与 Regular 不同。
- 用 Bold 文件注册的 family 请求 700 时不再合成（`synthetic_bold == false`）。
- 请求 `400, ITALIC` 在只有 Regular 时得到右倾约 12° 的合成斜体。
- 现有 Regular 场景像素回归通过。

### O2 — 变量字体（约 1.5–2 天）

- fvar 轴检测与 `hb_font_set_variation`；`wght` / `slnt` / `ital`。
- 混合策略：静态精确 > 变量轴 > 最近静态 + 合成。
- named instance / FC_INDEX 保留。
- 测试字体：优先用系统变量字体做手测；CI 可加一个小体积变量字体子集，或该测试标记为可选。
- 验收：300/400/700 三档轮廓/advance 不同；同请求命中同一变体；变体 id 不冲突；默认实例不变。

### O3 — vgui 集成（约 3–4 天）

1. **context 变体缓存**
   - `vgui_context` 增加 `std::map<std::pair<int,int>, font_familys_t*> font_variants`。
   - `vgui_font_for(ctx, weight, slant)`：命中返回；未命中 `font_familys_derive(ctx->font, weight, slant)`；失败回退 `ctx->font`。
   - `vgui_destroy` / `vgui_context_set_font` 时释放派生 family 并清空缓存。
2. **控件级样式**
   - `vgui_widget` 增加 `font_weight` / `font_slant`（0 继承 theme/context，默认 0）。
   - `vgui_widget_set_font_weight/slant` + getter；公开 `VGUI_FONT_WEIGHT_*` / `VGUI_FONT_SLANT_*` 别名，宿主无需包含 `ovg_c.h`。setter 校验：weight 只接受 0 或 100–900，slant 只接受 0–3；非法值忽略。
   - 所有文本绘制/测量/行高路径（`vgui_draw_text_sized_raw`、`vgui_widget_measure_text_*`、`vgui_widget_get_line_height_*`）改用 `vgui_font_for()`。
3. **span 级样式与测量**
   - `vgui_text_span` 末尾追加 `int font_weight; int font_slant;`（0 = 继承 base）。
   - `richtext.cpp::text_run` 携带 style；`build_runs` 解析继承；**在测量 run 宽度之前**先按有效 weight/slant 选变体 family，再取 `ascent` / `line_height`。
   - `vgui_widget_measure_text_spans` / `vgui_widget_get_line_height_spans` 目前按 base style 测量；改为逐 run 使用各自变体，否则真实 Bold 的更宽 advance 会让「测量=绘制」失效。
   - 新增 `vgui_custom_draw_text_styled(...)` / `vgui_widget_measure_text_styled(...)` / `vgui_widget_get_line_height_styled(...)` / `vgui_widget_get_text_metrics_styled(...)`；现有 `_sized` 作为 style=0 包装，避免破坏调用方。
4. **editor kit**
   - `markdown_view` 删除 0.6px 双绘，`strong` → weight 700，`emphasis` → italic；标题/表头按需加粗。
   - **布局必须 style-aware**：`measure()` / `wrap()` / 表格列宽 / 行高都要接受 `md_style`（bold/italic），走 `_styled` 测量；只在 draw 时换字体不够，真实 Bold 的 advance 更宽会造成换行/溢出/命中错位。
   - `code_editor` 的 token 主题可选增加 weight/slant 字段；本期至少让 markdown 正确。
5. 验收：
   - 同一行 `normal / bold / italic / bold italic` 混排，span 测量、控件测量、绘制三者宽度一致，行盒取最高 run。
   - `markdown_view` 的换行位置、表格列宽、行高与绘制一致；粗斜体不溢出、不重叠。
   - RTL 不回退（richtext 当前不重排 run，只保证不因样式改动而破坏现有顺序）；裁剪、下划线/删除线、fallback 中文均正常。
   - `docs/richtext_roadmap.md` 的「粗体/斜体非目标」改为已支持。

### O4 — 回归、性能、文档（约 1 天）

- 全量构建：core-only / model / spreadsheet / chart / editor kit / C API / screenshot。
- ASAN：反复创建/删除变体、clear_load/clear_sys、重新注册，无泄漏/悬空。
- 性能：同一样式只创建一次变体；绘制热路径不做 `new_font_family`；对比 Regular 文本 draw 耗时，合成变体允许小幅上升。
- 文档：ovg README 字体章节、vgui README、能力评估、richtext roadmap。

### D0–D4 — decoration 并行轨道

| 阶段 | 内容 | 建议时点 | 工期 |
|---|---|---|---|
| **D0** | 现有下划线/删除线截图基线；准备度量断言 | 随 O0 | 0.5 天 |
| **D1** | `vg_font_metrics_t` 追加 underline/strikeout/x-height/cap-height；按变体取度量；fallback | 随 O1 | 1 天 |
| **D2** | vgui decoration layer：span 字段、`vgui_text_metrics`、嵌套/连续区间、基线对齐、裁剪、solid 绘制 | 随 O3 | 2–3 天 |
| **D3** | `markdown_view` 删除手画 strike/link 线，改走 span decoration；style-aware 布局；文档更新 | 随 O3 | 1–1.5 天 |
| **D4** | 可选：double/dotted/dashed/wavy、skip-ink、多行装饰、逐线型样式 | O4 后 | 1–2 天 |

> D1 不改变现有绘制；D2 会**有意改变**当前 richtext 下划线/删除线的位置与粗细（从经验值改为字体度量），需截图评审。详细设计见 §10。
>
> glow（文本发光）不在上表内，作为 G0–G4 并行轨道规划，见 §11.8；G1 可随 O1 合入，G2/G3 随 O3/D2/D3 合入。

---

## 7. 测试矩阵

| 场景 | 方法 | 期望 |
|---|---|---|
| 默认兼容 | 现有截图场景全跑 | 与基线一致（或仅允许明确评审过的差异） |
| 静态 Bold | 注册 DejaVuSans-Bold，请求 700 | 选中真实 Bold，无合成；轮廓/宽度不同于 Regular |
| 静态 Italic/Oblique | 注册 Oblique，请求 Italic/Oblique | 优先级符合 §5.2；Oblique 请求优先 Oblique |
| 单字面合成粗体 | 只用 Regular，请求 700 | 有合成粗体；文本变粗；advance 合理增长 |
| 单字面合成斜体 | 只用 Regular，请求 Italic | 右倾约 12°；选区/测量宽度与绘制一致 |
| 混合粗斜体 | Regular + Bold + Oblique + BoldOblique | 四种样式均可选；缓存 id 不冲突 |
| fallback 链 | "DejaVu Sans, 微软雅黑, Segoe UI Emoji"，请求 700 | 每个 family 都有变体；中文/emoji 不缺字；emoji 不被合成破坏 |
| 自定义内存字体 | `load_font_from_memory` 注册 Bold 文件，别名 | 元数据 weight=700；请求 700 命中真实字面 |
| 自定义路径字体 | `load_font_from_path(path, name)` | 与内存注册一致 |
| 变量字体 | `wght` 300/400/700，`slnt` italic | 轮廓/advance/行高随轴变化；重复请求复用变体 |
| 无效值 | weight=1/1000，slant=99 | clamp/回退，不崩溃 |
| 缓存清理 | clear_load → 重新注册 → 绘制 | 无旧字形/旧字体指针；ASAN 无泄漏 |
| get_sys_family 幂等 | 连续调用两次 | `_familys` 不重复；已有 family 不悬空 |
| richtext 混排 | spans: bold/italic/颜色/字号/下划线 | span 测量=绘制；行盒最高；裁剪正确；RTL 不回退 |
| 样式测量 | 同一文本 base vs bold/italic，含 `measure_text_spans` | 宽度/行高来自对应变体；span/控件/绘制一致 |
| markdown | `**bold**` / `*italic*` / `***both***` | 不再双绘；真实/合成样式正确；换行/表格列宽/行高与绘制一致 |
| 装饰度量 | DejaVu Regular/Bold 16/32px 查询 `vg_get_font_metrics` | underline 在基线下、strikeout 在基线上；thickness≥1；数值随字号缩放 |
| 装饰位置 | 下划线/删除线/上划线 + 自定义 offset | 以基线定位，不随 line_height 漂移；override 生效 |
| 装饰嵌套 | 外层 underline，内层只改颜色/字号 | 内层仍保留下划线；颜色/字号内层胜出 |
| 装饰连续 | 相邻 run 同装饰、含空格 | 合并为一条连续线，不在 run/空格处断开 |
| 装饰裁剪 | 线超出 x/y 矩形 | 水平/垂直都夹紧，不越界 |
| 装饰样式 | solid/double/dotted/dashed/wavy（D4） | 线型可辨；厚度/间距随字号缩放 |
| 混排基线 | normal 12px + bold 24px 同一行 | 共享基线；装饰线跟随各自基准字体 |
| markdown 装饰 | `~~strike~~` / `[link](url)` | 不再手画线；删除线/下划线位置与 richtext 一致 |
| 性能 | 同一样式 1000 次绘制 | 变体只创建一次；无每帧分配；无装饰时不建 decoration layer |

> glow 的测试用例独立列在 §11.9；weight/slant/decoration 仍以上表为准。

---

## 8. 风险与决策

| 风险 | 影响 | 缓解 |
|---|---|---|
| `hb_font_t` 可变状态被共享 | 已缓存字形串味、粗细错乱 | 每个 `(base, weight, slant)` 独立 `hb_font_t`，创建后不可变 |
| 合成粗体质量 | 大字/黑色权重发糊 | 仅作回退；常量按截图调优；标题优先真实字重 |
| 合成 extents 与 OVG 光栅尺寸不一致 | 字形被裁切/偏移 | O1 验证 `hb_font_get_glyph_extents`/`hb_font_get_h_extents`，必要时在 `build_glyph_image_hb` 补偿 |
| 默认字面顺序变化 | 现有 UI 字形/宽度回归 | weight=0/slant=0 走旧路径；截图回归 |
| CJK 普遍缺 Bold/Italic | 大量走合成 | 每个 fallback family 独立合成；中文截图覆盖 |
| 彩色/位图字体 | 合成破坏 emoji/COLR | 检测彩色/位图后跳过合成 |
| `u16 font_id` 溢出 | 字形缓存碰撞 | 变体上限 + 溢出断言 + 回退基面；后续可扩 key |
| `FontStyle::id` 选择时重分配 | 字形缓存 key 漂移、旧条目泄漏 | id 在 `FontStyle` 创建时分配一次；`mk_font` 只匹配不改 id |
| 变体共享基面 rasterizer | 合成/variation 串到基面，缓存串味 | 每个变体拥有独立 `hb_font` / coverage / `rdr` / `pnt` / buffer |
| 派生 family 生命周期 | 悬空 `font_familys_t*` | vgui context 统一持有/释放；`set_font` 时清空；`delete_font_family` 清理按指针 key 的缓存 |
| `get_sys_family` 重复枚举 | 重复 face、选择漂移 | `(file,index)` 去重，不释放既有 face |
| 变量字体测试资产 | CI 覆盖不足 | 系统字体手测 + 可选小体积变量字体 |
| `font_familys_derive` 对用户自建 family 不适用 | 返回 NULL | `font_familys_cx` magic tag 校验；vgui 回退 `ctx->font` |
| 字体装饰度量差异大/缺失 | 下划线过高过低、过粗过细 | underline/strikeout 用 `hb_ot_metrics_get_position` + OVG fallback；x-height/cap-height 可用 `_with_fallback`；截图评审 |
| 装饰度量取自主 family | fallback 链中各 family 的装饰高度不同；中文/emoji 行可能沿用主 family 的线位置 | MVP 接受统一装饰高度并在文档说明；D4 可按 glyph 来源 family 分段 |
| 样式感知布局遗漏 | markdown 仍按 base 宽度换行，真实 bold 溢出 | O3 全量改 `measure/wrap/表格列宽/行高`；截图与溢出测试覆盖 |
| 基线对齐改变 richtext 混排 | 现有混排字号位置变化 | 只改 richtext 未提交行为；更新测试与截图；默认单字号路径不变 |
| 装饰线与字形/像素网格 | 1px 线发虚、删除线穿字 | 厚度/位置按设备像素取整；line style 与 draw order 评审（§10.4） |
| decoration 字段扩展 | C 结构体尺寸变化 | 只做追加式源码兼容；不承诺二进制 ABI；测试现有 5 字段初始化 |
| skip-ink/多行装饰 | 实现复杂、性能不可控 | 放到 D4 可选；MVP 只做整条 run 的连续线 |

### 已确认的决策（2026-11-18）

> 以下按推荐方案确认，作为实现依据；保留备选说明仅用于追溯，不再等待评审。

1. **公开字重刻度**：采用 CSS 100–900；不采用 fontconfig 的 0–215 语义，避免两套转换。
2. **合成默认开启**：单字面字体（如当前 `DejaVuSans.ttf`）请求粗体/斜体时走 HarfBuzz 合成；后续可加 `VG_FONT_SYNTH_NEVER` 选项关闭。
3. **vgui 自动派生**：采用 `font_familys_derive`，宿主零改动；vgui context 统一持有/释放派生 family，不做显式注册 API。
4. **`text_style_t` 不加 weight/slant**：样式固化在 `font_familys_t` 变体中，避免每次 draw 重新解析 family 和缓存回退。
5. **变量字体测试资产**：不作为仓库必带资产；CI 用系统字体手测，仓库只保留可选的小体积变量字体用例。
6. **decoration 归属**：OVG 只提供字体装饰度量，vgui 负责布局/绘制；不在 OVG `text_style_t` 加 decoration 字段。
7. **decoration 默认 skip-ink**：MVP 关闭；D4 再提供 `AUTO`。
8. **上划线位置**：默认 `baseline - ascent`（top-edge 语义）；D2 截图评审若偏高再改 cap-height。
9. **装饰覆盖单位**：`decor_thickness` / `decor_offset` 使用逻辑单位，绘制前乘 `ui_scale`；字体度量本身返回设备像素。`decor_line_state.font_size` 保存逻辑字号，查询度量时再走 `vgui_font_size_for()`。
10. **公开度量结构**：`vgui_widget_get_text_metrics_styled()` 返回 vgui 自有的 `vgui_text_metrics`，避免 `vgui.h` 依赖 `ovg_c.h`。
11. **glow（已确认）**：采用 OVG `text_style_t` 追加 glow 字段 + 批量矢量轮廓 pass；vgui 只负责逻辑单位、裁剪扩张、两遍绘制和 span 合并。理由、替代方案与 10 条细化决策见 §11。

---

## 9. 建议的最小交付（MVP）

如果只做一件事：**O1**。decoration 的最小闭环是 **D1 + D2（solid）**。

- 新增 `new_font_family_ex()`；
- 静态字面最近匹配；
- 自定义字体元数据；
- HB 合成粗体/斜体；
- 变体独立 `hb_font_t` + 缓存清理；
- OVG 字体装饰度量（underline/strikeout/x-height/cap-height）；
- vgui span decoration：solid underline/strike/overline、颜色/粗细/偏移、连续区间、基线对齐、裁剪；markdown 改走 span decoration。

完成后：

- 当前 `DejaVuSans.ttf` 单文件即可显示粗体/斜体；
- 系统里存在真实 Bold/Italic 时自动优先；
- `richtext`/`markdown_view` 可以在 O3 直接接入，不需要再改 OVG 字体层；
- 下划线/删除线/上划线不再靠 `line_height` 猜位置，markdown 不再手画线。

> glow 不进入 MVP：它需要额外一次矢量轮廓 tessellation + mask/blur，建议在 O1+D1 稳定后作为 G1 独立合入，避免首版同时改字体、装饰和特效。若产品优先级更高，可把 G1 与 O1 并行，但不要和 D2 的 span 字段变更混在同一次提交。

---

## 10. 文本装饰（decoration）规划

> 追加于 2026-11-17。decoration 与 weight/slant 正交，但共享三条路径：按字体变体取度量、按 run 做基线布局、markdown/richtext 接入。

### 10.1 现状与问题

现有 decoration 只有两个位，全部在 vgui 层手画：

| 项 | 现状 | 证据 |
|---|---|---|
| 公共位 | `VGUI_TEXT_DECOR_UNDERLINE` / `STRIKETHROUGH` | `include/vgui/richtext.h:19-24` |
| 数据结构 | `vgui_text_span` 只有 start/length/color/font_size/decor | `include/vgui/richtext.h:29-36` |
| 绘制 | `draw_decoration()` 用 `fill_round_rect` 画矩形 | `gui/richtext.cpp:117-123` |
| 位置 | 下划线 `run_y + line_height - thickness`；删除线 `run_y + line_height * 0.55` | `gui/richtext.cpp:165-175` |
| 粗细 | `max(1, font_size * 0.06)` | `gui/richtext.cpp:168` |
| 颜色 | 固定用文本色，无法单独指定 | `gui/richtext.cpp:170-174` |
| 嵌套/重叠 | 装饰随 paint span「后者胜出」，内层无 decor 会清掉外层 | `gui/richtext.cpp:92-113` |
| 连续性 | 每个 run 各自画线，run 边界/空格处会断开 | `gui/richtext.cpp:156-177` |
| 裁剪 | 水平按 `available` 截断，垂直没有夹到矩形 | `gui/richtext.cpp:117-123`、`167-174` |
| 基线 | 混排字号按行高居中，装饰 y 也跟着行高走 | `gui/richtext.cpp:139-160` |
| OVG 度量 | `vg_font_metrics_t` 只有 `ascent` / `line_height` | `ovg_c.h:505-508`、`ovg_fonts.cpp:1533-1570` |
| markdown 删除线 | 手画 `line.height * 0.55` | `gui/editor/markdown_view.cpp:2671-2675` |
| markdown 链接下划线 | 手画 `line.height * 0.84`，alpha=0xCC | `gui/editor/markdown_view.cpp:2685-2690` |
| 测试 | 只验证 decor 不影响测量；没有位置/裁剪断言 | `sample/vgui_model_test.cpp:5343-5355` |

结论：

- **位置和粗细都没有字体依据**。行高包含 ascender/descender/line_gap，把线画在行盒底部并不等于画在基线下；不同字体、不同字号会明显漂移。
- **装饰语义不完整**。没有 overline、线型、装饰颜色、粗细/偏移覆盖、skip-ink；嵌套 span 会丢装饰，连续文本会断线。
- **markdown 与 richtext 各自实现**。两套经验值（0.55 / 0.84）会随字体/字号产生不一致。
- **OVG 缺少度量**。HarfBuzz 14 已提供 underline/strikeout/x-height/cap-height，但 OVG 没有读出来；这与 weight/slant 的「每个变体独立度量」正好可以一起做。

### 10.2 范围与归属

推荐采用**分层归属**，而不是在 OVG 里再做一套完整的 text-decoration：

| 层 | 职责 | 理由 |
|---|---|---|
| OVG | 提供字体装饰度量：underline/strikeout 位置与粗细、x-height、cap-height；按变体 `hb_font_t` 读取 | 度量是字体属性，必须跟真实/变量/合成变体一致 |
| vgui 核心 | decoration layer：span 字段、嵌套/连续区间、基线对齐、裁剪、线型绘制 | richtext 是多 run 布局，只有 vgui 知道 run 边界和行盒 |
| editor kit | markdown 把语义映射为 span decoration；不再手画线 | 统一到一套位置/粗细/裁剪规则 |

不在 OVG `text_style_t` 加 decoration 字段的原因：

- OVG 一次 draw 只有一个 `text_style_t`，没有跨 run 的装饰传播/合并语义；加了也只能画单 run，和 richtext 重复。
- OVG 的 `vg_add_text_run` 已有多行布局，但 decoration 需要「按行/按 span/按嵌套」决策，属于上层布局。
- 宿主如果直接使用 OVG，可以拿 `vg_get_font_metrics()` 自己画线；接口足够，不强制新绘制 API。

如果后续确实需要 OVG 单 run 下划线，可作为 D4 的可选便利 API，不在 MVP。

### 10.3 OVG：字体装饰度量

#### 10.3.1 公共结构

在 `vg_font_metrics_t` 末尾追加字段（append-only，现有字段不变）：

```c
typedef struct vg_font_metrics_t {
    float ascent;
    float line_height;

    /* --- decoration metrics (appended) ---
     * 坐标均为 y 向下、相对基线：
     * - underline_offset：下划线**上边缘**到基线的距离，>0 表示在基线下方；
     * - strikeout_offset：删除线**上边缘**到基线的距离，通常 <0 表示在基线上方；
     * - thickness > 0，已按 font_size 缩放。 */
    float underline_offset;
    float underline_thickness;
    float strikeout_offset;
    float strikeout_thickness;
    float x_height;
    float cap_height;
} vg_font_metrics_t;
```

约定：

- **上边缘语义**：offset 指装饰线**上边缘**到基线的距离，画矩形时 `y = baseline + offset`。这与 OpenType `post.underlinePosition` / `OS/2.yStrikeoutPosition` 的定义一致，double 线/自定义 offset 也直接可算。
- **OVG 内部 y 向下**；HarfBuzz 的 y 向上，读取后统一取反：
  - `underline_offset = -hb_underline_position`（HarfBuzz 通常返回负数，转换后为正）；
  - `strikeout_offset = -hb_strikeout_position`（转换后通常为负）。
- **按字号缩放**：HarfBuzz 的 OT 度量已经按 `font->em_scale_y` 缩放。OVG 在读取前对变体字体 `hb_font_set_scale(font, fontsize, fontsize)`（等价于设置 `em_scale_y = fontsize`），因此 `hb_ot_metrics_get_position()` 返回的就是像素值，不需要再除以 upem。D1 仍用 DejaVuSans 在 16/32px 下断言符号和量级，防止 HarfBuzz 行为变化。`hb_font_set_scale` 是按字号查询的临时状态（现有绘制路径本来就按字号设置），不变的是 variation/synthetic。
- **按变体读取**：变体 `font_familys_t` 的主 family 是变体 `hb_font_t`；`vg_get_font_metrics()` 已有 `(ffs, fontsize)` 缓存，天然按变体隔离。MVAR/合成粗体可能改变 advance/extents，度量必须从变体字体读，不能从基面复制。
- **主 family 语义（已知限制）**：装饰度量只使用 fallback 链的**主 family**，不是每个 glyph 的实际来源 family。中文/emoji 混排时装饰线位置统一按主 family；D4 如需逐段精确，可按 glyph 来源 family 拆分装饰区间。
- **缓存**：继续使用 `font_metrics_cache`；新增字段随缓存一起返回，不新增查询路径。vgui 不直接暴露 `vg_font_metrics_t`，而是复制到自有的 `vgui_text_metrics`（§10.4.2）。

#### 10.3.2 缺失值的 fallback

度量读取分两类，不能一律用 `_with_fallback`：

- **underline / strikeout**：用 `hb_ot_metrics_get_position(font, tag, &position)`。只有字体真实提供 `post` / `OS/2` / MVAR 值时才采用；不要用 `hb_ot_metrics_get_position_with_fallback()`，因为 HarfBuzz 对 strikeout 的 fallback 是 `ascender/2`，位置明显偏高，不符合基线语义。
- **x-height / cap-height**：可以用 `hb_ot_metrics_get_position_with_fallback()`；HarfBuzz 的合成值通常比 OVG 自己猜更可靠。

HarfBuzz 返回 `false`（缺失）或 0 时，OVG 再加一层保护，避免 0/负数：

```text
underline_thickness = max(1, round(font_size / 14))
underline_offset    = max(1, round(font_size * 0.11))
strikeout_thickness = underline_thickness
strikeout_offset    = -(x_height * 0.55)     // x_height 缺失时用 font_size * 0.5
x_height            = font_size * 0.5
cap_height          = font_size * 0.7
```

这些只是兜底，正常字体应优先使用 OpenType `post` / `OS/2` / MVAR 的真实值。上划线（overline）没有对应的 OpenType 度量，由 vgui 用 `ascent`/`cap_height` 计算，默认：

```text
overline_offset = -ascent   // top-edge 语义：线顶部在 ascent 处
```

若评审觉得太高，可改用 `-cap_height`。

#### 10.3.3 为什么不在 OVG 画线

- OVG 的 text run 可能包含多行（`_shaped[].new_line`），但 decoration 需要知道「整条 span 的起止」和「嵌套语义」，这些在 vgui 的 span 数组里。
- OVG 画线会进入 text draw list 或额外路径，和 vgui 的裁剪/顺序/主题色耦合；vgui 已经有 `ovg_tools` 和 `cb`，直接画更简单。
- 保持 OVG 字体层职责单一：weight/slant/decoration 都只影响「字体供给与度量」，排版和装饰归 UI 层。

### 10.4 vgui：公共 API

#### 10.4.1 枚举

```c
/* 装饰线类型（位掩码）。 */
enum vgui_text_decor
{
    VGUI_TEXT_DECOR_UNDERLINE     = 1u << 0,
    VGUI_TEXT_DECOR_STRIKETHROUGH = 1u << 1,
    VGUI_TEXT_DECOR_OVERLINE      = 1u << 2,
    VGUI_TEXT_DECOR_ALL           = VGUI_TEXT_DECOR_UNDERLINE |
                                    VGUI_TEXT_DECOR_STRIKETHROUGH |
                                    VGUI_TEXT_DECOR_OVERLINE
};

/* 线型；0 = solid，保持旧行为。 */
enum vgui_text_decor_style
{
    VGUI_TEXT_DECOR_STYLE_SOLID = 0,
    VGUI_TEXT_DECOR_STYLE_DOUBLE,
    VGUI_TEXT_DECOR_STYLE_DOTTED,
    VGUI_TEXT_DECOR_STYLE_DASHED,
    VGUI_TEXT_DECOR_STYLE_WAVY
};

/* 附加行为。 */
enum vgui_text_decor_flags
{
    VGUI_TEXT_DECOR_FLAG_SKIP_INK = 1u << 0   /* D4；MVP 忽略 */
};
```

#### 10.4.2 vgui 别名与文本度量

`vgui.h` 是纯 C 公共头，不包含 `ovg_c.h`；宿主和 `richtext.h` 不应为了字重/倾斜去依赖 OVG 头。因此 vgui 提供数值一致、语义相同的别名：

```c
typedef enum vgui_font_weight
{
    VGUI_FONT_WEIGHT_DEFAULT     = 0,
    VGUI_FONT_WEIGHT_THIN        = 100,
    VGUI_FONT_WEIGHT_EXTRA_LIGHT = 200,
    VGUI_FONT_WEIGHT_LIGHT       = 300,
    VGUI_FONT_WEIGHT_SEMI_LIGHT  = 350,
    VGUI_FONT_WEIGHT_REGULAR     = 400,
    VGUI_FONT_WEIGHT_MEDIUM      = 500,
    VGUI_FONT_WEIGHT_SEMI_BOLD   = 600,
    VGUI_FONT_WEIGHT_BOLD        = 700,
    VGUI_FONT_WEIGHT_EXTRA_BOLD  = 800,
    VGUI_FONT_WEIGHT_BLACK       = 900
} vgui_font_weight;

typedef enum vgui_font_slant
{
    VGUI_FONT_SLANT_DEFAULT = 0,
    VGUI_FONT_SLANT_NORMAL  = 1,
    VGUI_FONT_SLANT_ITALIC  = 2,
    VGUI_FONT_SLANT_OBLIQUE = 3
} vgui_font_slant;

/* vgui 自有的度量副本；语义与 vg_font_metrics_t 的追加字段一致，
 * 但 vgui.h 不因此依赖 OVG 头文件。 */
typedef struct vgui_text_metrics
{
    float ascent;
    float line_height;
    float underline_offset;      /* top edge, baseline 下方为正 */
    float underline_thickness;
    float strikeout_offset;      /* top edge, baseline 上方为负 */
    float strikeout_thickness;
    float x_height;
    float cap_height;
} vgui_text_metrics;

/* font_size 为逻辑字号；0 = 控件当前字号。
 * 内部按 ui_scale 与 weight/slant 选变体，返回设备像素度量。
 * 查询失败时 out 清零并填保守 fallback，不返回 0 厚度。 */
VGUI_API void vgui_widget_get_text_metrics_styled(vgui_widget* widget,
    float font_size, int font_weight, int font_slant,
    vgui_text_metrics* out);
```

- 控件 setter/getter 使用 `vgui_font_weight` / `vgui_font_slant`；内部直接转给 `vgui_font_for()`。
- `vgui_text_span.font_weight/font_slant` 使用同一组值，0 = 继承 base。

#### 10.4.3 `vgui_text_span` 追加字段

```c
typedef struct vgui_text_span
{
    int start;
    int length;
    vgui_color color;
    float font_size;
    unsigned decor;

    /* O3：字重/倾斜，0 = 继承 base。 */
    int font_weight;
    int font_slant;

    /* D2：装饰覆盖；0/缺省 = 使用来源 span 文本色或字体度量。 */
    vgui_color decor_color;
    float decor_thickness;
    float decor_offset;
    unsigned decor_style;
    unsigned decor_flags;
} vgui_text_span;
```

所有字段都在末尾追加，现有 `{ start, length, color, font_size, decor }` 初始化仍然合法（其余字段自动为 0）。**不承诺二进制 ABI**，只保证源码兼容。G2 还会在同一位置追加 `vgui_text_glow glow`（§11.6.3），与 D2 字段同一次合入，避免 `vgui_text_span` 两次扩张。

字段语义：

- `decor`：按位 OR 叠加；重叠 span 不会互相清除装饰位。这是与当前「后者整体胜出」的关键修正。
- `decor_color`：0 = 使用该装饰来源 span 的文本色（或 base_color）；链接下划线可传带 alpha 的颜色。
- `decor_thickness`：**逻辑单位**（绘制前乘 `ui_scale`）；0 = 字体度量；>0 作为逻辑厚度，乘 scale 后最小夹到 1 设备像素。
- `decor_offset`：**逻辑单位**（绘制前乘 `ui_scale`）；0 = 字体度量；>0 表示「远离基线」（下划线向下、删除线/上划线向上），<0 表示靠近基线。
- `font_size`：沿用现有语义，逻辑字号；装饰度量查询时由 vgui 乘 `ui_scale` 得到设备字号。
- `decor_style`：作用于该 span 选中的所有装饰线；0 = solid。
- `decor_flags`：MVP 只接受并忽略 `SKIP_INK`；D4 实现。
- 风格属性（color/style/thickness/offset/flags）按 span 顺序**后者覆盖**，但只作用于它自己设置的装饰位；例如外层 underline、内层只设 strike，则 underline 保持外层属性。
- **一个 span 的装饰属性统一作用于它设置的所有位**：`decor_color/style/thickness/offset` 不能在同一条 span 里给 underline 和 strike 分别配色；strike+link 同时出现时 MVP 共用同一个颜色。逐线独立属性放 D4。
- 测量（`vgui_widget_measure_text_spans` / `vgui_widget_get_line_height_spans`）仍然不受 decoration 影响；测试保留。

#### 10.4.4 调用方式

- 富文本：继续用 `vgui_custom_draw_text_spans()`，一个全段 span + 多个内层 span 即可表达「外层下划线 + 内层颜色/字号/粗斜体」。
- 普通文本：传 `spans=NULL, span_count=0` 仍是纯文本；要下划线就传一个覆盖全串的 span。
- O3 的 `vgui_custom_draw_text_styled(...)` 直接复用 `vgui_text_span` 的字段（或接受一个同构的 style 结构），不新增第二套 decoration 参数。
- `vgui_custom_draw_text_sized()` 保持现签名，作为 style=0 的包装；不破坏现有宿主。

示例（C++）：

```cpp
vgui_text_span outer = {};
outer.start = 0;
outer.length = (int)std::strlen(text);
outer.decor = VGUI_TEXT_DECOR_UNDERLINE;
outer.decor_color = 0xFF3B82F6u;
outer.decor_thickness = 0.0f;          // 用字体度量
outer.decor_offset = 0.0f;             // 用字体度量

vgui_text_span inner = {};
inner.start = 0;
inner.length = 4;
inner.color = 0xFFFF8080u;             // 内层只改颜色，不改装饰
inner.font_weight = 700;

vgui_text_span spans[2] = { outer, inner };
vgui_custom_draw_text_spans(widget, cb, vg, text, spans, 2,
    x, y, width, line_height, base_color, 0.0f, 0.5f, base_font_size);
```

### 10.5 vgui：内部设计

#### 10.5.1 数据模型

```cpp
struct text_run
{
    std::string text;
    int start = 0, end = 0;      // UTF-8 字节区间
    float width = 0.0f;
    float font_size = 0.0f;
    float ascent = 0.0f;
    float line_height = 0.0f;
    vgui_color color = 0;
    int font_weight = 0, font_slant = 0;
    font_familys_t* family = nullptr;  // 变体 family（O3）
};

/* 每种装饰线独立保存属性：不同 span 可以分别设置 underline / strike。 */
struct decor_line_state
{
    bool on = false;
    vgui_color color = 0;
    unsigned style = VGUI_TEXT_DECOR_STYLE_SOLID;
    float thickness = 0.0f;
    float offset = 0.0f;
    unsigned flags = 0;
    float font_size = 0.0f;          // 该装饰来源 span 的逻辑字号（未乘 ui_scale）
    int font_weight = 0, font_slant = 0;
};

enum { DECOR_UNDERLINE = 0, DECOR_STRIKETHROUGH = 1, DECOR_OVERLINE = 2 };

struct decor_state
{
    decor_line_state line[3];
};

struct decor_interval
{
    int start = 0, end = 0;
    float x0 = 0.0f, x1 = 0.0f;
    decor_state state;
};
```

#### 10.5.2 构建流程

1. **清洗 span**：沿用现有逻辑，clamp 到文本范围、UTF-8 边界对齐、稳定排序；保持 `order` 用于「后来者胜出」。
2. **收集边界**：把 paint span 与 decoration 的 start/end 都加入边界集合；保证后续 run 和装饰区间能在同一套字节边界上映射到 x。
3. **解析 paint run**：按边界切段；每段取最后一个覆盖它的 span 的 color/font_size/font_weight/font_slant。现有 paint 字段是 **replace 语义**：span 中为 0 的 `color` / `font_size` / `font_weight` / `font_slant` 回退到 **base**，不是回退到外层 span。解析出有效 style 后先用 `vgui_font_for()` 选变体 family，再用该变体测量 run 宽度并取 `ascent` / `line_height`；不能先按 base 测量再在绘制时换字体。每个 run 记录 `start/end`、变体 family、`ascent`、`line_height`。
4. **解析 decoration**：
   - 对每段，遍历 span（按**原始数组顺序**，不是按 start 排序后的顺序）；
   - 对 `span.decor` 中的每个位（underline/strike/overline），把该位的 `decor_line_state` 用该 span 的 `decor_color/style/thickness/offset/flags/font_size/font_weight/font_slant` 覆盖；
   - 装饰色取该 span 的有效色：`decor_color ? decor_color : color ? color : base_color`；
   - 未设置的位保留之前的状态；span 的 `decor=0` 不会清除已有装饰（CSS 语义：祖先装饰不会被后代 `text-decoration:none` 去掉）；
   - 每个位独立保存来源字号/weight/slant；度量查询统一走 `vgui_widget_get_text_metrics_styled(widget, font_size, weight, slant, &metrics)`（D2 新增，`metrics` 为 `vgui_text_metrics`；内部按 `vgui_font_size_for()` 和 `vgui_font_for()` 取变体），richtext.cpp 不需要直接访问 `ctx` 内部或 `vgui_font_for()`；
   - 这样外层 underline 不会被内层「无 decor」span 清掉，内层只改 strike 时 underline 的颜色/度量也不变。
5. **合并区间**：相邻且三个 `decor_line_state` 完全相同的区间合并；合并后 x0/x1 取首尾 run 的 x。空格/相邻 run 不会被无故切断。
6. **行盒与基线**：
   ```text
   box_ascent  = max(run.ascent)
   box_descent = max(run.line_height - run.ascent)
   box_height  = max(max(run.line_height), box_ascent + box_descent)
   baseline_y  = pen_y + box_ascent
   run_y       = baseline_y - run.ascent
   ```
   绘制 run 时传 `y = run_y`、`height = run.line_height`、`align_y = 0`，让 OVG 的基线正好落在 `baseline_y`。同一字号时与旧行为一致；混排字号时是修正。
7. **坐标映射**：run 已按所有边界切分，因此每个装饰区间可以直接用首尾 run 的 `x`/`width` 得到 `x0/x1`；不重复测量子串。
8. **无装饰快速路径**：没有任何 span 设置 `decor`（且 `span_count<=1`）时，退化为现有单 run 路径，不建 decoration layer。注意不能用「所有 run 的 `decor` 字段为 0」判断，因为继承状态可能来自外层 span。

#### 10.5.3 绘制

对每个合并后的 `decor_interval`，遍历三个 `decor_line_state` 中 `on==true` 的线，分别取度量：

```cpp
vgui_text_metrics metrics = {};
vgui_widget_get_text_metrics_styled(widget, line.font_size,
    line.font_weight, line.font_slant, &metrics);
```

`vgui_widget_get_text_metrics_styled()` 内部会应用 `vgui_font_size_for()`（逻辑字号 → 设备字号）、选择变体 family、调用 `vg_get_font_metrics()`，再复制到 `vgui_text_metrics`；这样 richtext.cpp 不直接依赖 OVG 的字体选择逻辑或 OVG 头文件。

```text
scale              = vgui_context_get_ui_scale(vgui_widget_get_context(widget))
offset_px          = line.offset * scale
underline_top      = baseline_y + metrics.underline_offset + offset_px
strikeout_top      = baseline_y + metrics.strikeout_offset - offset_px
overline_top       = baseline_y - metrics.ascent - offset_px
thickness          = line.thickness > 0
                     ? max(1, line.thickness * scale)
                     : metrics.<line>_thickness
                     // overline 没有字体度量时用 underline_thickness，
                     // 仍缺失则 max(1, round(effective_font_size / 14))
```

- **厚度**：默认用字体度量（已是设备像素）；用户覆盖值乘 `ui_scale` 后 `max(1.0f, ...)`。实线优先按设备像素对齐（D2 截图调）。
- **solid**：`fill_rect(x0, top, x1-x0, t)`。
- **double**：上边缘 `top` 起两条 `t/2` 线，间距 `t`；总高度约 `2t`。
- **dotted**：`ovg_tools::dashed_line`，on=`t`、off=`2t`，圆帽；或小方点。
- **dashed**：`dashed_line`，on=`3t`、off=`2t`。
- **wavy**：用 `cb->quadratic_to` 画正弦近似，振幅 `1.5t`、波长 `4t`，线宽 `t`。
- **绘制顺序**：underline/overline 在文本之前画，strike 在文本之后画；这样下划线不会盖住 descender，删除线能压在字形上。实现上分两遍：先 under-decor，再所有文本，再 strike-decor。
- **裁剪**：
  - `x0/x1` 夹到 `[x, x+width]`；
  - `top .. top+t`（以及 wavy 振幅）夹到 `[y, y+height]`；
  - 完全在矩形外的线直接跳过；
  - 外层 widget 裁剪仍由调用方的 stencil/clip 负责，本层不越界。
- **颜色**：直接用该线的 `line.color`（在解析阶段已按 `decor_color → span.color → base_color` 解析），不再依赖某个 paint run 的颜色。
- **跳过墨迹（D4）**：需要字形级别的 x 区间和 bbox；当前 OVG 不暴露，放在 D4，或通过 `vg_draw_text` 的 run 信息扩展实现。MVP 不做。

#### 10.5.4 与 weight/slant 的关系

- 每个 `decor_line_state` 的 `font_size` / `font_weight` / `font_slant` 取**该条装饰来源 span**的有效值；外层下划线跨粗体/斜体内层时，位置和粗细保持稳定，不会因为内层变体而跳变。
- 如果内层 span 自己也设置某条装饰位，它就成为该位的新的来源，使用内层的变体度量；其他位不受影响。
- `vg_get_font_metrics()` 的缓存 key 是 `(font_familys_t*, size)`；`vgui_widget_get_text_metrics_styled()` 已选好变体 family，因此同一装饰来源的多次绘制命中同一缓存。
- 合成粗体/斜体只影响字形轮廓/advance，装饰度量仍来自变体字体；如果未来合成导致度量变化，D1 测试覆盖。

### 10.6 markdown 接入

当前 `markdown_view` 在绘制文本前后手画两条线：

- `segment.style.strike` → `ovg_tools::line(..., y + line.height * 0.55, ...)`（`markdown_view.cpp:2671-2675`）；
- `segment.style.link` → `ovg_tools::line(..., y + line.height * 0.84, ...)`（`markdown_view.cpp:2685-2690`）。

D3 改为：

```cpp
vgui_text_span span = {};
span.start = 0;
span.length = (int)text.size();
span.color = color;                         // 文本色
span.font_size = block.text_font;
span.font_weight = segment.style.bold ? VGUI_FONT_WEIGHT_BOLD : 0;   // O3
span.font_slant = segment.style.italic ? VGUI_FONT_SLANT_ITALIC : 0;
span.decor = (segment.style.strike ? VGUI_TEXT_DECOR_STRIKETHROUGH : 0) |
             (segment.style.link ? VGUI_TEXT_DECOR_UNDERLINE : 0);
span.decor_color = segment.style.link ? with_alpha(color, 0xCC) : 0;

vgui_custom_draw_text_spans(surface, cb, vg, text.c_str(), &span, 1,
    x, y, width, line.height, color, 0.0f, 0.5f, block.text_font);
```

- 删除两处 `ovg_tools::line`，删除 `strong` 的 0.6px 双绘（O3）。
- 链接下划线颜色保留现有的 alpha 0xCC；删除线与文本同色。
- `segment.style.code` 的背景 chip 不变；inline code 不加装饰。
- 表格/标题里的 bold/italic 走同一路径；表格头加粗时装饰仍按 cell 文本绘制。
- **布局先于绘制**：`markdown_view::measure()` / `wrap()` / 表格列宽 / `line_height()` 必须先接受 `md_style` 并按 bold/italic 选变体测量（`vgui_widget_measure_text_styled` / `_get_line_height_styled`）。只在 draw 时换字体不够：真实 Bold 的 advance 更宽，会造成换行位置错误、表格溢出和命中测试偏移。
- 命中测试/选区不受影响：decoration 不参与测量，也不改变 `segment.x/width`；但 bold/italic 的宽度变化必须来自布局阶段，而不是绘制阶段。
- 可选优化：如果每个 segment 都调用 `vgui_custom_draw_text_spans` 导致重复测量，再在 D4 提供一个「已知宽度/基线」的低层装饰绘制 helper；MVP 先保证语义一致。

### 10.7 分阶段与验收

| 阶段 | 内容 | 验收 |
|---|---|---|
| **D0**（0.5 天，随 O0） | 记录现有下划线/删除线截图；准备 12/16/24/32px、中文/英文、粗斜体组合 | 基线可重复；明确哪些差异是本次要修的 |
| **D1**（1 天，随 O1） | `vg_font_metrics_t` 追加字段；`vg_get_font_metrics()` 读 HB 度量；fallback；按变体隔离 | DejaVu 16/32px 下 underline 在基线下方、strike 在上方、thickness≥1；Bold/Italic 变体可查询；无字体度量时 fallback 不崩溃 |
| **D2**（2–3 天，随 O3） | span 字段/枚举；`vgui_text_metrics` + `vgui_widget_get_text_metrics_styled()`；decoration layer；基线对齐；solid underline/strike/overline；颜色/粗细/偏移；连续合并；裁剪 | 嵌套 underline 不丢；相邻 run/空格连续；三种线位置正确；混排字号共享基线；decoration 不影响测量且样式测量与绘制一致；截图通过 |
| **D3**（1–1.5 天，随 O3） | markdown 删除手画线，改走 span decoration；`measure/wrap/表格列宽/行高` style-aware；文档更新 | `~~strike~~`、链接下划线位置与 richtext 一致；无重复线；bold/italic 不再双绘；布局宽度与绘制一致 |
| **D4**（1–2 天，可选） | double/dotted/dashed/wavy；skip-ink；多行装饰；逐线型独立样式 | 线型可辨；skip-ink 不切断 descender；多行每行独立；性能无明显退化 |

依赖关系：

- D1 依赖 O1 的变体字体（没有变体时也能工作，只是度量来自基面）。
- D2 可以独立于 O3 开发，但建议一起合入，避免 `vgui_text_span` 字段两次变更。
- D3 依赖 D2；O3 的 weight/slant 字段与 D2 的 decoration 字段同一次追加，减少 ABI/测试噪音。

### 10.8 测试矩阵

| 场景 | 方法 | 期望 |
|---|---|---|
| 度量存在 | DejaVuSans 16/32px 查询 `vg_get_font_metrics` | 字段非零；underline_offset>0；strikeout_offset<0；thickness 随字号增长 |
| 度量缺失 | 构造无 post/OS2 度量的字体或 mock | fallback 生效；不出现 0 厚度/负厚度 |
| 变体度量 | Regular/Bold/Italic 变体分别查询 | 缓存按 family 隔离；不串值 |
| 嵌套装饰 | 外层全串 underline，内层只改 color/font_size | 下划线保留；内层颜色/字号生效 |
| 连续区间 | 相邻 run 同 decor，且空格位于装饰 span 内部 | 合并为一条线；x 范围覆盖空格；若空格不在任何装饰 span 内则不画 |
| 位置 | underline/strike/overline + offset=0/自定义 | 以基线定位；top-edge 语义与 offset 方向符合 §10.4.3 |
| 厚度 | thickness=0（度量）与 thickness=3（逻辑单位） | 0 用字体度量；3 在 ui_scale 下精确生效；最小 1 设备像素 |
| 裁剪 | 线超出 x/y 矩形、部分可见、完全不可见 | 水平/垂直夹紧；完全不可见不画 |
| 混排基线 | 12px normal + 24px bold 同一行 | 基线对齐；行盒取最高；装饰跟随来源 span |
| 样式测量 | bold/italic span 的 measure 与 draw | span/控件测量宽度一致；无溢出 |
| 装饰度量接口 | `vgui_widget_get_text_metrics_styled` | 返回 `vgui_text_metrics`；逻辑字号→设备字号；变体隔离 |
| 链接颜色 | decor_color 带 alpha | 下划线颜色/alpha 正确，文本色不受影响 |
| 测量 | 带 decor 的 measure | 与不带 decor 完全相同 |
| markdown | strike/link/bold/italic 组合 | 无手画线；位置与 richtext 一致；无重复绘制；换行/列宽与绘制一致 |
| 截图 | 现有 richtext/markdown 场景 | 只允许评审过的位置/粗细差异 |
| ASAN | 反复 draw/measure、span 重叠/越界 | 无越界/泄漏 |

### 10.9 decoration 决策（已确认）

> 以下按推荐方案确认（2026-11-18）；D4 的可选项仍保留为实现阶段的备选。

1. **OVG 只提供度量，不提供绘制 API**；如果希望 OVG 单 run 也能直接下划线，D4 再加 `text_style_t.decor` 便利字段。
2. **MVP 只做 solid**；double/dotted/dashed/wavy 放 D4，避免首版就引入线型/像素对齐的复杂度。
3. **skip-ink 默认关闭**；CSS 的 `auto` 需要字形 bbox，D4 再实现。
4. **装饰来源决定度量**：每条装饰线独立保存来源字号/weight/slant；外层 underline 跨内层粗斜体时使用外层字体度量，避免线随内层跳动；内层显式设置同一位时才切换。
5. **上划线默认用 ascent**：`baseline - ascent`（top-edge 语义）；如视觉上偏高，D2 截图评审时改用 cap-height。
6. **不新增 `vgui_custom_draw_text_decorated()`**：一个 span 覆盖全串即可；如果 markdown 性能证明需要，再在 D4 加低层 helper。
7. **装饰覆盖单位**：`decor_thickness` / `decor_offset` 为逻辑单位，绘制前乘 `ui_scale`；`decor_line_state.font_size` 保存逻辑字号，查询度量时由 `vgui_widget_get_text_metrics_styled()` 转设备字号。
8. **公开度量结构**：`vgui_widget_get_text_metrics_styled()` 返回 vgui 自有 `vgui_text_metrics`，`vgui.h` 不包含 `ovg_c.h`；OVG 的 `vg_font_metrics_t` 仍由直接使用 OVG 的宿主查询。

---

## 11. 文本发光（glow）规划

> 追加于 2026-11-18。结论：**现有 `vg_effect_t` 不能直接作用于文本主路径**；但在 `text_style_t` 追加 glow 字段，并用「批量矢量轮廓 + 一次 outer glow fill」实现是可行的，且不需要改渲染后端。OVG 负责矢量 pass，vgui 负责逻辑单位、裁剪、绘制顺序和 span 合并。

### 11.1 范围与结论

| 层 | 职责 | 不做什么 |
|---|---|---|
| OVG | `text_style_t` 追加 glow 字段；把 run 内所有可用轮廓合成一个 path；设置一次 `VG_EFFECT_OUTER_GLOW`；不可见填充（opaque + opacity=0）；恢复调用方 effect/opacity 状态 | 不处理跨 run 的装饰/发光合并；不自动扩大调用方裁剪；不做 SDF |
| vgui 核心 | `vgui_text_glow` + 控件/span 字段；逻辑单位→设备像素；glow pass 的裁剪扩张；richtext 两遍绘制 | 不改文本测量/行高；不让 glow 影响命中测试 |
| editor kit | 可选：`md_style` 透传 glow；demo 用真实 glow 替换叠副本 | 不把 glow 写进 markdown 语法；不默认给所有标题发光 |

结论：

- **可行**。复用现有 `vg_effect_t` 的 `VG_EFFECT_OUTER_GLOW`、mask/blur 管线和纹理池，不新增 GPU 资源类型。
- **必须走矢量 pass**。raster atlas 文本是 `add_geometry`/`DRAW_GEOM`，没有 effect 槽位；直接给 raster 文本设置 effect 不会生成 mask。
- **一次 run 一次 effect**。不能逐字形 fill，否则每个字形都会生成一张 mask + 两次 blur；批量成一条 path 后只做一次。
- **默认零开销**。`color_glow` 为 0 时 draw list 与现在完全一致；glow pass 不分配、不提交。
- **无后端能力查询也能安全降级**。glow pass 用不透明颜色 + `state opacity=0` 让主体不可见；后端不支持 effect 时只少了光晕，不会画出实心字形。

### 11.2 为什么现有发光特效不能直接用于文本

现有 `vg_effect_t` 的生命周期是「状态里带 effect → 下一次 `fill`/`stroke` 时分配 fx 槽位 → 渲染器在 mask 上重画该路径 → blur → 合成」。关键代码：

| 事实 | 证据 |
|---|---|
| effect 只在路径 fill/stroke 时分配槽位 | `third_party/ovg/ovg/ovg.cpp:2445-2490`（`apply_effect_bounds`）、`2493-2516`（`fill_preserve`）、`2290-2320`（stroke 路径） |
| mask 读的是填充顶点的 alpha，不是 effect 颜色 | `third_party/ovg/ovg/shaders/effect_mask.slang.h:1-11`；`shaders/spv_c/a_vg.vert.glsl:95-113`（solid 填充走顶点色） |
| raster 文本走 `add_geometry`，不是 `fill` | `ovg.cpp:7303-7322`（`submit_draw_list` → `build_raster_batches` → `submit_raster_batches`）；`ovg.cpp:7117-7204` |
| 渲染器只对 `DRAW_VG` 处理 effect | `third_party/ovg/ovg/ovg_renderer_sdl3.cpp:3197-3222`；vgui 后端同理 `backends/vgui_renderer_sdl3.cpp:2647-2671` |
| 文本现有绘制顺序只有阴影/描边/正文 | `ovg.cpp:7377-7427` |
| demo 已确认文字拿不到 mask | `sample/vgui_demo.cpp:1916-1919`：「文字是 add_text 生成的图集几何，不进 mask，拿不到发光」 |

如果强行把 effect 挂在 raster 文本的几何命令上：

- effect 的 mask 绘制的是**矩形的文本几何**，不是字形覆盖度，发光会变成一块方形光斑；
- 即使 mask 用 atlas alpha，现有 `render_effect_mask` 也只重画顶点几何，不采样文本 atlas；
- 文本阴影是 raster 偏移副本（`shadow_pos`），不是高斯模糊，不能当 glow 用。

因此，唯一能复用现有 blur/合成管线的路径是：**把字形轮廓当作普通矢量路径 fill 一次**。

### 11.3 方案对比

| 方案 | 质量 | 成本 | 兼容性 | 结论 |
|---|---|---|---|---|
| 叠多个 raster 副本（当前 demo） | 无真实模糊；偏移大了变重影；小字号发糊 | 每个方向一次文本绘制；方向数×顶点 | 全后端可用 | 仅作 demo 兜底，不进库 API |
| 对文本区域做后处理模糊 | 会模糊背景；需要离屏/读回；与 UI 合批冲突 | 高 | 依赖后端 | 不采用 |
| 文本 shader 内多 tap 模糊 | 半径受 tap 数限制；每个 glyph 都付代价 | 中高 | 需要改文本 shader 与 atlas 采样 | 可作未来小半径优化，不作为首版 |
| **矢量轮廓 + `VG_EFFECT_OUTER_GLOW`** | 真实高斯；一个 run 一次 mask/blur；与矢量描边一致 | 一次 tessellation + 一次 effect | 现有 effect 管线；不支持时 opacity=0 填充自然降级 | **推荐** |
| SDF/MSDF 文本 | 质量最好、可做描边/发光 | 需要重做字形缓存与 shader | 大改 | 非本期目标 |

选择矢量方案的理由：

- 不改变现有文本主路径；raster 文本仍是最快路径。
- 复用 `vg_effect_t` 的 `sigma/spread/opacity/color` 语义和纹理池，不新增 effect 类型。
- 只在请求 glow 时付一次矢量 tessellation；普通文本零成本。
- 与 weight/slant 的变体字形天然一致：轮廓来自变体 `hb_font_t` 的 glyph cache。

### 11.4 OVG 公共 API

在 `text_style_t` **末尾追加**字段（append-only，现有初始化不变）：

```c
typedef struct text_style_t
{
    font_familys_t* family;
    float fontsize;
    float lineheight;
    vec2 shadow_pos;
    float stroke;
    uint32_t color;
    uint32_t color_stroke;
    uint32_t color_shadow;
    int min_subpixel;

    /* --- glow (appended) ---
     * color_glow：0 或 alpha=0 时完全不生成 glow pass；颜色格式同 color，
     *             即 OVG 的 0xAABBGGRR。
     * glow_sigma：设备像素，语义同 vg_effect_t::sigma；<=0 表示不模糊。
     * glow_spread：-1..1，语义同 vg_effect_t::spread。
     * glow_opacity：0 按 1.0 处理（零初始化友好）；>0 时夹到 0..1，
     *               作为 color_glow alpha 之外的额外强度。 */
    uint32_t color_glow;
    float glow_sigma;
    float glow_spread;
    float glow_opacity;
} text_style_t;
```

语义：

- **启用条件**：`(color_glow >> 24) != 0`。仅设置 `glow_sigma` 不会发光；仅设置颜色、不设 sigma 会得到硬边轮廓光，通常应同时给 `sigma > 0`。
- **颜色/混合**：`color_glow` 走 `vg_effect_t.color`，当前 `blend=0`（over）。需要 additive 时在 G4 再扩展；不要在首版偷偷改成 additive。
- **字节序**：`vg_effect_t.color` 的头注释写的是 `0xAARRGGBB`，但现有 demo/`ovg_tools::set_glow()` 实际按 OVG 的 `0xAABBGGRR` 传入。glow 必须和 `text_style_t.color` 使用同一个转换（vgui 用 `vgui_ovg_color()`）；G1 加一个颜色通道断言，必要时修正头注释，避免红蓝反转。
- **单位**：OVG 层 `glow_sigma` 是设备像素，和 `vg_effect_t.sigma`、`shadow_pos` 一致。vgui 层再负责 `ui_scale`。
- **顺序**：glow pass 画在阴影之前（最底层），然后才是现有阴影、描边、正文。这样阴影/正文不会被光晕染色；如果截图评审认为阴影应更靠后，再调整。
- **无新函数**：仍由 `vg_add_text()` / `cb->add_text()` 驱动；不新增 `vg_add_text_glow()`，避免 API 分叉。
- **默认兼容**：所有新字段为 0 时，`vg_add_text_run` 的绘制命令、缓存键、effect 状态与现在一致；现有截图不应变化。

示例（直接使用 OVG 的宿主）：

```c
text_style_t st = {};
st.family = family;
st.fontsize = 32.0f;
st.color = 0xFFFFFFFFu;              /* OVG ABGR */
st.color_glow = 0xFF00FFFFu;         /* 颜色 + alpha；ABGR */
st.glow_sigma = 6.0f;                /* 设备像素 */
st.glow_spread = 0.15f;
st.glow_opacity = 1.0f;

text_box_rt box = {};
box.rc = { x, y, w, h };
box.text_align = { 0.0f, 0.5f };
box.baseline = ascent;
text_st_t t = {};
t.pos = { x, y };
t.size = { w, h };
t.text = text;
t.text_len = (int)strlen(text);
cb->add_text(vg, &t, &st, &box);
```

### 11.5 OVG 内部设计

#### 11.5.1 绘制流程

`vg_add_text_run()` 保持现有 shape/layout，然后按以下顺序提交：

```
shape / layout（缓存命中）
  └─ glow pass（可选，仅 color_glow alpha != 0）
       ├─ populate_draw_list(VECTOR_ONLY)   // cmd.color 不参与批量填充
       ├─ 批量轮廓 → 一条 path
       ├─ set_effect(OUTER_GLOW)
       ├─ fill（不透明色 + state opacity=0）
       └─ 恢复 effect / opacity / 状态
  ├─ shadow（现有 raster 偏移）
  ├─ stroke（现有矢量描边 / 四方向 raster）
  └─ main text（现有 raster/矢量混合）
```

- glow pass 必须在 `main_list.clear()` 之前用**独立的 scratch draw list**，不能复用 `run->drawable`，否则主文本列表被污染。
- 如果 `glow_list` 里没有任何可用 VECTOR 字形（全是 emoji/彩色字形），直接跳过 glow pass，不提交空 fill，避免空的 fx 槽位和绘制命令。
- `shadow`/`stroke` 的条件、颜色、`min_subpixel` 不变。
- `stroke > 0` 时主文本仍走现有 `submit_vector_glyphs_stroked()`；glow 不参与描边，避免重复 effect。
- **glow-only 调用优化**：vgui 的 glow pass 会传 `text_style_t.color=0x00000000`、`color_shadow=0`、`color_stroke=0`、`color_glow`（glow fill 本身另用不透明色 + state opacity=0，见 §11.5.3）。只有**本次确实提交了 glow pass（`color_glow` alpha != 0）且正文颜色 alpha=0** 时，OVG 才跳过 main pass 的 raster/vector 提交，避免生成一批不可见的 atlas 顶点。把跳过条件绑定到「有 glow」而不是单独看 `color`，是为了保证 `color_glow=0` 的旧调用 draw list 逐命令不变（§11.4「默认兼容」）。`color_shadow` / `color_stroke` 仍按各自的 alpha 独立决定是否绘制，不能因为正文透明就一起丢掉——现有代码允许「透明正文 + 可见阴影/描边」，该行为必须保留。单独把 `color` 设为 0 并不等于「什么都不画」。

#### 11.5.2 批量轮廓

文本 run 的每个 `glyph_draw_cmd` 已经在 `get_cache_lookup_glyph()` 里缓存了 em 单位的矢量轮廓（`ovg_fonts.cpp:1085-1122`），所以不需要为 glow 重新 `hb_font_draw_glyph()`。

glow pass **不能走** `submit_draw_list()` / `submit_vector_cmds()`：前者会落到逐字形 fill，后者本身就是逐字形 `set_path + fill`。正确做法是只用 `populate_draw_list(VECTOR_ONLY)` 生成 cmd 列表，再交给批量 helper 一次 fill。

问题在于 `ovg_path_t` 在 fill 时只有**一个**状态矩阵，而每个字形的位置/字号不同；`submit_vector_cmds()` 现在是逐字形 `translate + scale + fill`（`ovg.cpp:7207-7257`）。逐字形 fill 会导致每个字形一次 effect，不可接受。

关键事实（决定了 helper 必须「烘焙坐标」而不是「逐字形设置状态」）：

- `_add_point()` 只把坐标原样写入 `points`，**不应用** `path->t`（`ovg.cpp:764-775`）。
- fill 的顶点就是 `points` 原值；`path->t->pushConsts.mat` 由渲染器在顶点着色阶段应用（`fill_non_zero()`，`ovg.cpp:2896-2915`）。
- `path->t` 的矩阵缩放只用于 `add_path` 时的曲线细分容差和 `ovg_path_extents(transformed=true)`；`apply_effect_bounds()` 用未变换的 `points` 再乘 `st->pushConsts.mat` 得到设备空间包围盒（`ovg.cpp:2468-2477`）。

因此一条 path 只能对应一个最终状态矩阵；必须把每个字形的 `translate + scale` 预先乘进坐标，fill 时只保留**基准矩阵 B**：

- canvas 模式：B = identity（`submit_vector_cmds()` 里 `identity_matrix(st)` 后的像素空间；`set_geom_state()` 的 ortho 只服务 raster 几何，**不能**拿来做 vector fill 的基准矩阵）。
- ctx 模式：B = `cb->get_matrix()` 读到的调用方矩阵（与 `submit_vector_cmds_ctx()` 的 `base_mat` 相同）。

helper：

```cpp
// 把 list 中所有 VECTOR 字形按各自的 translate+scale 烘焙到同一条 path。
// 坐标变为基准空间（canvas: pixel；ctx: 调用方局部空间），fill 时状态矩阵只用 B。
// path->t 由调用方预先设为 B，保证曲线细分容差按设备像素算。
bool append_baked_glyph_path(ovg_path_t* path, const text_draw_list* list);
```

细节：

- 逐条解析 `path_data` 的 `path_type_et`：`e_vmove`/`e_vline` 各 2 个坐标，`e_vcurve` 4 个，`e_vcubic` 6 个，`e_close` 无坐标；**控制点也要变换**，只改坐标、不改命令顺序。对未知 type / 截断数据直接停止并返回 false（防止坏缓存越界）；调用方在 false 时丢弃已写入的 path，不要提交半截轮廓的 fill。
- 每个字形用自己的 `cmd.fontsize / entry->em_units`，以支持 fallback 链里不同 family 的 upem。
- 公式：`x' = src_x * scale + cmd.pos.x`，`y' = src_y * scale + cmd.pos.y`（`path_data` 的 y 已经由 `path_builder` 翻转，保持现状）。先缩放再平移；不要先在 em 空间合并再统一缩放，因为 fallback 的 upem 可能不同。
- `path->t` 指向 B 的状态：canvas 用临时 state（identity），ctx 用 `set_matrix(base)` 后的 `rvg->st`。这样曲线细分容差是设备像素；若误用某个字形的 `em→pixel` 矩阵，长文本会过度细分。
- 批量后只调用一次 fill：canvas 模式走 `cb->set_path(rvg, path, st)` + `cb->fill(rvg)`（`set_path` 只存在于 `ovg_canvas_cb`）；ctx 模式没有 `set_path`，而是 `new_path()` 一次 + `add_path` + `fill()`。`fill_preserve()` 会把 `path->t` 覆盖成当前 fill state，但 `points` 已经是最终坐标，结果不变。
- **填充规则用 non-zero**：TrueType/OpenType 轮廓靠绕向表达洞。`fill_needs_tess2()` 会按子路径 bbox 重叠判断，含洞的字形（o/e/a…）本来就会走 tess2（`ovg.cpp:2846-2891`）；批量后整条 path 一次 tess2 是可以接受的，重点是仍然只有一次 effect。若长文本 tess2 超预算，按行拆 path，不要退回逐字形 effect。
- **空轮廓/彩色字形**：`path_data == NULL || path_size == 0` 的字形跳过（COLR/CBDT emoji 常见）；不要为它们生成空 subpath。
- **ctx 模式只 `new_path()` 一次**：`vctx_add_path()` 总是写到 `rvg->path`；逐字形 `new_path()` 会丢掉前一个字形的轮廓。正确顺序是：`save` → `get_matrix(base)` → `set_matrix(base)` → `new_path` 一次 → 逐字形灌入已烘焙的坐标 → `set_source_color(0xFFFFFFFF)` + `set_opacity(0)` + `set_effect(glow)` → `fill` → `restore`。
- **不要用 `set_geom_state` 的 ortho 作为 fill 状态**：那条矩阵是 raster 几何的 NDC 变换，和 vector fill 的像素/局部空间不是同一坐标系；用了会导致发光位置偏移或缩放错误。

伪代码（ctx）：

```cpp
cb->save(rvg);
glm::mat3x2 base;
cb->get_matrix(rvg, &base);
cb->set_matrix(rvg, &base);        // 之后 path->t == rvg->st == base
cb->new_path(rvg);                 // 只一次
append_baked_glyph_path(rvg->path, &glow_list);  // 坐标已含 T*S
cb->set_source_color(rvg, 0xFFFFFFFF);            // mask 读顶点 alpha，必须为 1
cb->set_opacity(rvg, 0.0f);                       // 主体不可见；mask pass 会覆盖回 1
vg_effect_t e = ...; cb->set_effect(rvg, &e);
cb->fill(rvg);
cb->restore(rvg);
```

#### 11.5.3 不可见填充与 effect 设置

glow pass 的填充色必须 **alpha=1**（建议 `0xFFFFFFFF`），再用 `set_opacity(0)` 让主体几何不可见。**不能用透明填充（color alpha=0）**，原因：

- mask 片元 `effect_mask.slang.h:1-11` 直接输出 `input.src.a`；VG 顶点着色器在 `fsq_patternType == VG_PATTERN_TYPE_SOLID`（0）时把**顶点色**传到 location 1（`shaders/spv_c/a_vg.vert.glsl:95-113`），`render_effect_mask()` 设置的 `pc.source = white` 只对 pattern 填充生效。透明填充会让 mask alpha=0，光晕完全不出现。
- `render_effect_mask()` 会把 `pc.opacity` 覆盖为 1（`vgui_renderer_sdl3.cpp:1470-1472`、`ovg_renderer_sdl3.cpp:3017-3019`），所以 state opacity=0 不影响 mask 覆盖度。
- `apply_effect_bounds()` 只检查 `effect.color` alpha、`effect.opacity` 和路径包围盒，不检查 state opacity（`ovg.cpp:2452-2459`）。
- `draw_vg()` 先画 effect quad，再画原几何（`vgui_renderer_sdl3.cpp:1751-1755`）；原几何用 `pc.opacity=0` 绘制，不可见；effect quad 的强度来自 `fx->opacity`（即 `vg_effect_t.opacity`），不受 state opacity 影响（`vgui_renderer_sdl3.cpp:1616-1641`）。
- 无 effect 后端：`render_effect_mask()` 不执行，原几何仍因 opacity=0 不可见 → **自动降级为无 glow，不会出现实心字形**。

因此填充色只负责「让 mask 有覆盖度」，state opacity 负责「让主体不显示」，两者不能互换。

effect 参数映射：

```cpp
vg_effect_t e = {};
e.type = VG_EFFECT_OUTER_GLOW;
e.color = color_glow;                 // OVG ABGR
e.sigma = max(0.0f, glow_sigma);
e.spread = clamp(glow_spread, -1.0f, 1.0f);
e.opacity = glow_opacity > 0.0f ? min(glow_opacity, 1.0f) : 1.0f;
e.offset = { 0, 0 };
```

状态恢复：

- canvas 模式：临时 path + 临时 state，effect 和 opacity=0 只设在临时 state 上；`set_path()` 会替换 `rvg->path/st`，fill 后要像 `submit_vector_cmds()` 一样先恢复旧指针，再 `destroy_path/state_destroy`。effect/opacity 不会泄漏到调用方 state，因此不需要 save/restore；如果实现选择 save/restore，也必须单独恢复 `rvg->path`。
- ctx 模式：沿用 `submit_vector_cmds_ctx()` 的 `save/restore`；glow pass 结束后必须把调用方原来的 effect 和 opacity 恢复，不能把 outer glow 或 opacity=0 泄漏给后续 fill。`save/restore` 不保存 `rvg->path`，路径生命周期与现有矢量文本一致即可。
- 如果实现中发现 `save/restore` 在某个后端不保存 effect/opacity，改为显式拷贝 `vg_effect_t old` 和原 opacity，再 `set_effect(old)` / `set_opacity(old)`；测试里要覆盖「调用方已有 shadow effect 且 opacity=0.5，add_text 后下一次 fill 仍带 shadow 和 0.5」。

#### 11.5.4 裁剪与包围盒

- `apply_effect_bounds()` 已按 `3*sigma + 2` 外扩 ROI（`ovg.cpp:2479-2490`），glow 不需要自己算模糊半径。
- OVG **不会**自动把 `clip_rect` 外扩；glow 仍受当前裁剪限制。直接使用 OVG 的宿主若要光晕溢出，需在 `add_text` 前后自行扩大/恢复 `clip_rect`。
- vgui 后端是矩形 scissor，没有 stencil；因此 vgui 层的 glow 裁剪行为可预测。OVG 直接渲染器的 path/stencil 裁剪与 effect 合成目前不保证 mask 受 stencil 限制（现有 effect 也有同样限制），本期不扩大范围。
- 文本 box 的 `rc` 仍用于布局/对齐；glow pass 的 box 与主文本一致，保证字形位置不变。

#### 11.5.5 缓存与性能

- `text_run_cache_cx` 的 key 不含颜色/glow；命中时 `run->_st = *ts` 会更新 glow 字段（`ovg.cpp:328-330`），所以同一文本切换 glow 不需要重新 shape。
- glow scratch list 建议作为 `vg_text_run_cx` 的成员复用，避免每帧 `std::vector` 分配；`populate_draw_list()` 前 `clear()`。
- 烘焙 path 每次 glow 调用都会重新生成；静态文本在 vgui 按需渲染下只烘焙 dirty 帧，MVP 不缓存烘焙结果（位置/对齐/字号任一变化都会失效，缓存收益有限）。如果后续要做，key 至少包含变体 family、fontsize、文本 shape 版本、位置与对齐。
- richtext 两遍会先把所有 run 的 glow 提交、再提交正文；如果 span 数超过 OVG `text_run_cache_cx` 的 512 条上限，第一遍的早期 run 可能在第二遍前被淘汰并重新 shape。功能正确，但性能测试要覆盖大量 span 的场景；G4 可改为逐行合并或提高缓存上限。
- **一个 run 一次 effect**：mask 尺寸约等于文本包围盒 + 3*sigma；blur 两趟；与逐字形 effect 相比是数量级差异。
- 如果一条文本包含很多行/很多 fallback family，批量 path 可能很大；先按 run 合批，出现 tess2 性能回退时再按行拆。不要退回逐字形 effect。
- 静态标签在 vgui 的按需渲染下只在 dirty 帧重绘；glow 不会每帧重算。动画文本（浮动战斗文字、hover 霓虹）会每帧付一次 effect，需在 demo/性能测试里覆盖。

### 11.6 vgui 公共 API

`vgui.h` 是纯 C 公共头，不包含 `ovg_c.h`；glow 类型用 vgui 自己的颜色/单位语义。

#### 11.6.1 类型与枚举

```c
/* 文本发光参数。颜色为 vgui 的 0xAARRGGBB。
 * sigma 是逻辑像素，绘制时乘 ui_scale；spread 无量纲（-1..1）。
 * 零初始化 = 不发光；颜色 alpha=0 关闭/表示「未设置」。 */
typedef struct vgui_text_glow
{
    vgui_color color;
    float sigma;
    float spread;
    unsigned flags;
} vgui_text_glow;

enum vgui_text_glow_flags
{
    /* span 用：清除从外层继承的 glow；控件层忽略。 */
    VGUI_TEXT_GLOW_OFF = 1u << 0
};
```

#### 11.6.2 控件级 API

```c
/* 颜色 alpha=0 关闭发光（RGB 部分不参与判断）；sigma<=0 按 0 处理（硬边），
 * 通常应给正值。spread 夹到 -1..1。 */
VGUI_API void vgui_widget_set_text_glow(vgui_widget* widget,
    vgui_color color, float sigma, float spread);
VGUI_API void vgui_widget_clear_text_glow(vgui_widget* widget);
VGUI_API vgui_text_glow vgui_widget_get_text_glow(vgui_widget* widget);
```

- 存储为 `vgui_widget::text_glow`；set/clear 标 dirty，走现有重绘路径；`get` 对空控件返回零值。
- 所有通过 `vgui_draw_text()` 的内置文本（label/button/checkbox/...）统一走 `vgui_draw_text_passes()`，由它读取控件 glow；不需要逐个控件加参数。直接调用 `vgui_draw_text_sized_raw()` 的 emoji/特殊绘制不在此列，保持原样。
- **透明度/禁用独立于正文 alpha**：glow 颜色先按 `enabled ? glow.color : vgui_mix_color(glow.color, theme.disabled, style.disabled_text_mix)` 处理，再乘控件 `opacity`；不与正文颜色的 alpha 相乘。这样正文 alpha=0 时仍可只发光，而禁用控件不会残留光晕。
- **裁剪**：glow 受父/控件 clip 限制。内置 label 自己不做文本矩形裁剪，但通常落在祖先 panel 的 `clip_children` 区域里；如果父面板边界紧贴控件，外扩的光晕仍会被切掉。需要外发光时给控件留 `ceil(3*sigma+2)` 的 padding（或让父面板留空间）；`VGUI_CUSTOM` 可设 `custom_clip_content=0` 并自行限制父 clip。`vgui.h` 的文本 helper 注释要写明「glow 是裁剪矩形外扩的唯一例外」。
- `vgui_widget_set_text_color()` 不改变 glow 颜色；两者独立。
- `sigma` 逻辑像素、`spread` 无量纲；setter 把 `spread` 夹到 -1..1，`sigma<0` 按 0。

#### 11.6.3 span 级字段

在 §10.4.3 的 `vgui_text_span` 末尾再追加：

```c
    /* G2：文本发光；零初始化 = 继承外层/控件。 */
    vgui_text_glow glow;
```

语义：

- `(glow.color >> 24) != 0`：该 span 覆盖范围内的 glow 使用此值；即使同时带了 `VGUI_TEXT_GLOW_OFF` 也以颜色为准（OFF 忽略）。
- `glow.flags & VGUI_TEXT_GLOW_OFF`：仅在颜色 alpha=0 时有效，清除继承的 glow；即使外层有发光，这一段也不发光。
- 颜色 alpha=0 且没有 OFF：继承外层 span / 控件 glow。
- `sigma`/`spread` 只在颜色 alpha≠0 时生效；alpha=0 时忽略，避免「只改 sigma 却意外开灯」。RGB 非零但 alpha=0 仍视为未设置，防止 `0x00FFFFFF` 被误判为开启。
- 与 decoration 一样，**内层只改颜色/字号/字重不会清除外层 glow**；G2 与 D2 共用 span 覆盖扫描/嵌套规则，减少一次 `vgui_text_span` 结构变更。
- glow 不参与 `vgui_widget_measure_text_spans()` / `vgui_widget_get_line_height_spans()`；测量结果与不带 glow 完全相同。

#### 11.6.4 内部绘制 helper

公共 API 保持现有 `vgui_custom_draw_text()` / `_sized()` / `_spans()` 签名；新增一个仅核心内部使用的轻量头（建议 `gui/vgui_text_draw.h`，不要 include 整个 `vgui_internal.h`，这样 `gui/richtext.cpp` 也能用）：

```cpp
enum
{
    VGUI_TEXT_PASS_GLOW      = 1u << 0,
    VGUI_TEXT_PASS_TEXT      = 1u << 1,
    // 正文也裁到文本矩形：custom/richtext 使用；内置控件不传，保持现有溢出行为。
    VGUI_TEXT_PASS_CLIP_TEXT = 1u << 2,
    VGUI_TEXT_PASS_ALL       = VGUI_TEXT_PASS_GLOW | VGUI_TEXT_PASS_TEXT
};

// color 是最终文本色（已含控件整体透明度/disabled）；glow 为 NULL 时用 widget->text_glow。
// 函数自己负责每个 pass 的 scissor；进入时当前 clip 是父/控件外层 clip。
void vgui_draw_text_passes(vgui_widget* widget, ovg_ctx_cb* cb, rvg_t* vg,
    const char* text, float x, float y, float width, float height,
    vgui_color color, float align_x, float align_y, float font_size,
    const vgui_text_glow* glow, unsigned passes);
```

裁剪顺序（关键，现有 `vgui_custom_draw_text_sized()` 的 clip 必须先撤销）：

1. 读取当前父 clip（`cb->get_clip_rect`）。父 clip 为「禁用」哨兵（`z <= 0 || w <= 0`）时不要与 0 求交；沿用 `vgui_custom_clip_begin` 的 `z > 0 && w > 0` 判断和恢复方式。
2. GLOW pass：用与正文完全相同的布局盒 `(x, y, tw, th)`（`tw/th` 按 `vgui_draw_text_sized_raw()` 的 `max(width, measured)` / 多行规则计算），scissor 设为该盒外扩 `pad = ceil(3*sigma_device + 2)`，再与父 clip 求交。**只扩 scissor**：传给 `add_text` 的 `box.rc`、`box.text_align`、`t.pos`、`t.size` 必须与正文 pass 完全一致，否则字形会偏移 `pad`。调用一次 `add_text`，`color=0`、`color_shadow=0`、`color_stroke=0`；`vgui_text_glow` 映射为 `color_glow = vgui_ovg_color(glow.color)`、`glow_sigma = glow.sigma * ui_scale`、`glow_spread = glow.spread`、`glow_opacity = 1.0f`（vgui 暂不暴露独立强度，alpha 已乘进颜色）。
3. TEXT pass（仅 `VGUI_TEXT_PASS_TEXT`）：若带 `CLIP_TEXT`，scissor 设为 `(x,y,width,height)` 与父 clip 的交集（与现有 `vgui_custom_clip_begin` 的取整/求交行为一致）；不带 `CLIP_TEXT` 时保持父 clip 不动。`color` 正常、`color_glow=0`、阴影/描边保持现有值；再调用一次 `add_text`（同一文本命中 run cache，不重复 shape）。
4. 恢复父 clip。

- `vgui_draw_text_sized_raw()` 保持「无裁剪、单次正文」的现有语义（内置控件和旧调用都依赖它）；新增的 `vgui_draw_text_passes()` 是裁剪/pass 层。内置文本入口 `vgui_draw_text()` 改为调用 `vgui_draw_text_passes(..., ALL)`，**不传** `CLIP_TEXT`，因此没有 glow 时不会新增任何 scissor 命令，也不会改变现有「长文本可溢出文本矩形」的行为。
- `vgui_custom_draw_text_sized()` 不再先 `vgui_custom_clip_begin` 到文本矩形，而是把 `vgui_widget_color(widget, color)` 后的最终色交给 `vgui_draw_text_passes(..., ALL | CLIP_TEXT)`；无 glow 时它只做一次与现在完全相同的文本矩形裁剪。
- `vgui_custom_draw_text()`（非 sized）同理。
- 实际提交可抽一个内部 `vgui_draw_text_once(..., color, glow)`：不做裁剪、**不做 `color==0` 早退**，只填 `text_style_t` 并调用一次 `add_text`。`vgui_draw_text_sized_raw()` 保留自己的精确 `color==0` 早退，再转调它（glow 为空）；`vgui_draw_text_passes()` 用它在 glow pass 里提交 `color=0`。
- **颜色只应用一次**：helper 接收最终色；richtext 走一个内部 wrapper（应用 `vgui_widget_color`）后调用 helper，不能既在 span 层又 helper 层乘 opacity。为了让 `richtext.cpp` 不 include `vgui_internal.h`，在 `gui/vgui_text_draw.h` 同时声明一个轻量的 `vgui_text_widget_color(widget, color)`（vgui.cpp 内定义，转调 `vgui_widget_color`），richtext 用它处理 span 色；不要在 richtext 里重复实现 opacity 乘法。
- glow 颜色用 `vgui_ovg_color()` 转成 OVG 字节序；disabled/opacity 只作用于 glow 自己的 alpha，不与正文 alpha 相乘。
- `color == 0`（精确 0）时现有 wrapper 仍然直接返回，不画 glow；需要「只发光不画字」时传 `0x00FFFFFF` 之类的 alpha=0 非零色。这个语义写进头文件注释。
- `VGUI_TEXT_PASS_TEXT` 在 `color` alpha=0 且没有阴影/描边时可以跳过，避免不可见顶点；vgui 的正文没有阴影/描边，因此 glow-only 调用只提交 glow pass。
- 没有 glow 时：`CLIP_TEXT` 路径只调用一次正文并保持现有裁剪；内置入口不设置 scissor，和现在一致。进一步地，无 glow 且不带 `CLIP_TEXT` 时直接提交一次正文、不调用 `get_clip_rect`，保证内置文本热路径没有额外命令。

为什么不直接让 `vg_add_text` 同时画 glow 和正文：单 run 可以，但 richtext 需要「所有 run 的 glow 先画，再画所有正文」，否则后一个 run 的光晕会盖到前一个 run 的字上。分 pass 是 richtext 正确排序的最小改动。

### 11.7 richtext / markdown 接入

#### 11.7.1 richtext 两遍绘制

`gui/richtext.cpp` 当前逐 run 调用 `vgui_custom_draw_text_sized()`（`richtext.cpp:156-177`）。G2 改为：

1. `build_runs()` 同时计算每个 run 的有效 `color/font_size/font_weight/font_slant/decor/glow`；span 的 glow 继承/OFF 规则与 decoration 共用同一套覆盖扫描。
2. 先算好每个 run 的布局并存到局部数组：`{ text, width, pen_x, run_y, available, line_height, color, glow }`。两遍都读同一份数据，避免第一遍结束后 `pen_x` 已推进导致第二遍错位。
3. 第一遍：若至少一个 run 有 glow，对每个有 glow 的 run 调 `vgui_draw_text_passes(..., PASS_GLOW)`；scissor 由 helper 按实际文本布局盒 + glow pad 处理。
4. 第二遍：对每个 run 调 `vgui_draw_text_passes(..., PASS_TEXT | CLIP_TEXT)`；正文仍按 `available` 宽度裁剪，与现有 `vgui_custom_draw_text_sized()` 一致。
5. 最后画 decoration；装饰线不发光（MVP），保持在正文之上。

- 两遍都按同一份 `pen_x`/`run_y` 布局，不改变 run 宽度、换行和基线。
- 没有任何 run 发光时跳过第一遍，走现有单遍路径，draw list 与现在一致。
- 相邻 run 的 glow 重叠区域用 over 混合；因为所有 glow 都在正文之前，不会把后一个 run 的光晕叠到前一个 run 的字上。
- 如果两个相邻 run 的 glow 参数完全相同，可在 G4 合并为一次 `add_text` 覆盖两段文本，减少 effect 数；MVP 先按 run。
- 裁剪：glow pass 用该 run 的实际文本布局盒外扩 `glow_pad`，文字 pass 用原 `run_rect` 的 `available` 宽度；这样光晕可以越过 run 边界，但正文仍严格裁剪。外扩只作用于 scissor，不改变 run 的 `x/width/align`。
- **公开契约变化**：`include/vgui/richtext.h` 现在写的是「Text is clipped to the supplied rectangle; decoration lines are clamped to the same rectangle」。glow 是唯一例外：开启 glow 时允许光晕越过该矩形最多 `pad`，但仍受父/控件 clip 限制；glow 关闭时保持原契约。注释和 README 都要更新。
- `base_color == 0` 时 `vgui_custom_draw_text_spans()` 仍按现有语义直接返回；需要 glow-only 时传 alpha=0 的非零色，例如 `0x00FFFFFF`。
- `vgui_custom_draw_text_spans()` 的 base glow 来源：控件 `text_glow` 作为 base；span 的 `glow` 覆盖/继承；传给 `build_runs()`。
- `richtext.cpp` 只 include 新增的 `gui/vgui_text_draw.h` 和公开头，不把 `vgui_internal.h` 拉进来。

#### 11.7.2 markdown

- D3 已经把 `**bold**` / `*italic*` / `~~strike~~` / `[link]` 改为 span；G3 只需把 `md_style` 的可选 `glow` 透传到 `vgui_text_span.glow`。
- 不给 markdown 语法新增「发光」标记；glow 是宿主/主题表现属性。
- 标题/链接是否发光由 `md_style` 决定；默认 0，保持现有截图。
- `measure()` / `wrap()` / 表格列宽仍按 D3 的 style-aware 逻辑；glow 不参与测量。
- markdown 的逐 segment 绘制改为逐行 spans 后，glow 的 run 边界与 richtext 一致，不再出现 `0.55/0.84` 那种两套经验值。

#### 11.7.3 demo

`sample/vgui_demo.cpp` 的 `build_neon()` 已默认使用 OVG 光晕：圆角线框通过 `VG_EFFECT_OUTER_GLOW` 渲染，标签通过 `span.glow` 一次整形并提交字形遮罩。已移除叠描边、文字副本和 `VGUI_NEON_FX` 开关。线框和文字的光晕都留在控件内部的留白内；不支持特效时仍绘制线框和正文。

### 11.8 分阶段与验收

| 阶段 | 内容 | 依赖 | 工期 | 验收 |
|---|---|---|---|---|
| **G0** | 基线：记录现有 raster 文本、demo 叠副本、无 glow 截图；准备 16/24/32/48px、中英混排、粗斜体、彩色 emoji | O0 | 0.5 天 | 基线可重复；明确哪些差异是 glow 引入的 |
| **G1** | OVG：`text_style_t` 字段；批量轮廓 helper；不可见填充（opaque + opacity=0）；effect/opacity 设置与恢复；跳过无轮廓字形；`ovg_tools_test` 用例 | O1（变体字体；无变体也能工作） | 2–3 天 | 一个 20 字形 run 只多 1 条 fill + 1 个 fx 槽位；`color_glow=0` 与基线逐命令一致；无 effect 后端无实心伪影；ctx 非 identity 基矩阵/曲线控制点正确；ASAN 通过 |
| **G2** | vgui：`vgui_text_glow`；控件 setter/getter；`vgui_text_span.glow`；内部 pass helper；richtext 两遍；裁剪扩张；DPI/disabled | G1 + O3 + D2（同一次 span 字段变更） | 2–3 天 | 控件 glow 开关正确；span 继承/OFF 正确；相邻 run 无文字染色/接缝；测量不变；2x DPI 半径正确；录制 widget 回放仍有 glow |
| **G3** | markdown `md_style.glow` 透传；demo 真实 glow；文档/截图 | G2 + D3 | 1–1.5 天 | 标题/链接可选发光；demo 叠副本退役；现有截图默认不变 |
| **G4** | 可选：inner glow、逐字形/逐段强度、additive、decoration glow、相邻 run 合并、effect 预算/降级 | G1–G3 | 1–2 天 | 性能/质量达标；不改变 MVP API 语义 |

总计约 **5.5–8 天**（不含 O1/O3/D2/D3 本身）。G1 可以与 O1 并行开发，但建议先让 O1 的变体字体稳定，避免「合成粗体轮廓 + glow」两个变量同时调。

### 11.9 测试矩阵

| 场景 | 方法 | 期望 |
|---|---|---|
| 默认关闭 | `color_glow=0` 绘制一段文本 | draw list 与基线一致；无额外 fill；无 fx 槽位 |
| 单 run 发光 | 20 字形、`sigma=6` | 只多 1 条 fill；`ref` 只分配 1 个；bounds 外扩约 `3*sigma+2` |
| 多行/多字体 | 两行中英混排 + fallback | 一个 run 一次 effect；字形位置/advance 不变 |
| 不可见填充 | 检查 fill 顶点 alpha 与 state opacity | 顶点 alpha=1；state opacity=0；effect mask 覆盖度=1；正文不被覆盖 |
| 颜色通道 | 设置纯红/纯绿 glow | 屏幕颜色与 `text_style_t.color` 同一字节序，无红蓝反转 |
| 合成粗斜体轮廓 | 单字面合成 700 + 12° slant，再开 glow | 光晕宽度/倾斜与 raster 字形一致；无「细字发粗光」 |
| 控制点 | 含曲线字形（o/S/@）开 glow | 光晕与正文曲线一致；无折线/缺口 |
| ctx 基矩阵 | `set_matrix(2x + translate)` 后 `add_text` | glow 与正文同位置/同缩放；无二次变换 |
| glow-only | 正文 alpha=0、shadow/stroke=0、glow 开 | 只提交 1 条 fill；无 raster 顶点 |
| 透明正文 + 阴影 | 正文 alpha=0、shadow 可见 | 阴影仍绘制；不被 glow-only 优化吞掉 |
| 空 base_color | spans `base_color=0`、span 有 glow | 按现有语义不绘制；文档已说明用 alpha=0 非零色 |
| 录制回放 | `recording_hint` widget + glow 重放 | effect ref 正确；第二帧仍有 glow |
| fx 槽位上限 | 同帧 >127 个发光 run | 不崩溃；超出部分无 glow；统计可见 |
| 控件裁剪 | glow 超出 label 矩形/父 panel | 默认受祖先 panel 的 `clip_children` 限制；给控件/父面板留 padding 后可见；文档一致 |
| 无轮廓字形 | emoji/COLR/CBDT 字体 | 跳过该字形；不崩溃；其余字形正常发光 |
| effect/opacity 恢复 | 调用方先设 shadow effect、opacity=0.5，再 `add_text`，再 fill 矩形 | 后续 fill 仍带原 shadow 和 0.5；glow/opacity=0 不泄漏 |
| 裁剪 | clip 小于文本、clip 大于文本、完全不可见 | glow 只出现在 clip 内；完全不可见时不生成可见像素 |
| 后端不支持 | 模拟 `fx_supported=false` / 无 effect 合成 | 只少 glow；不出现实心低透明度字形；正文正常 |
| 控件 API | set/clear/get；color=0；sigma=0；spread 越界 | 状态正确；dirty 触发重绘；参数夹紧 |
| disabled/opacity | 禁用控件、`opacity=0.5` | glow 自身 alpha 按 disabled 混合/opacity 衰减；正文 alpha 不参与 |
| span 继承 | 外层 glow，内层只改颜色/字号 | 内层仍有 glow；颜色/字号内层胜出 |
| span 覆盖/OFF | 内层设新 glow / `VGUI_TEXT_GLOW_OFF` | 内层覆盖或关闭；其他 span 不受影响 |
| richtext 顺序 | 两个相邻 run 都发光 | 所有 glow 在正文之前；后一个 run 的光晕不染前一个 run 的字 |
| 测量 | 带 glow 的 measure/line-height | 与不带 glow 完全相同 |
| DPI | `ui_scale=2`，逻辑 `sigma=4` | 设备 sigma=8；外扩/半径按 2x 生效 |
| markdown | 标题/链接可选 glow | 默认无变化；开启后位置与 richtext 一致；布局宽度不变 |
| 性能 | 10 个 32px 发光标签 | `frame_effects` 等于发光 run 数，不等于字形数；无明显掉帧 |
| 截图 | demo 叠副本 vs 真实 glow | 半径/亮度/边缘通过评审；默认截图不变 |
| ASAN | 反复 draw/measure、span 重叠/越界、空轮廓 | 无越界/泄漏/悬空 effect |

### 11.10 glow 风险与决策

| 风险 | 影响 | 缓解 |
|---|---|---|
| 每个 run 一次 mask + 两次 blur | 大量发光标签时 GPU 开销上升 | 默认关闭；只在标题/徽章/霓虹控件启用；G4 合并相邻同参数 run；用 `frame_effects` 监控 |
| 批量 path 触发 tess2 | 长文本首帧变慢 | 含洞字形本来就走 tess2；批量后仍是一次 tess2 + 一次 effect；必要时按行拆；不要退回逐字形 effect |
| 127 个 effect 槽位上限 | 同帧大量发光 run 时后面的光晕静默消失 | `apply_effect_bounds()` 的 `fxCount` 上限为 127；G4 合并相邻 run / 加预算与降级；测试覆盖；文档说明 |
| 录制/缓存 widget 回放 | 缓存命令里的 effect ref 在重放时错位 | 现有后端按 draw item 重建 `fx_by_item`；G2 加 recording 回放测试 |
| 烘焙坐标/基矩阵 | 非 identity ctx 矩阵下光晕偏移或二次缩放 | canvas 用 identity、ctx 用 `get_matrix`；测试 2x/translate；不要把 raster ortho 当 fill 矩阵 |
| 控件裁剪 | 光晕被祖先 panel 或 `VGUI_CUSTOM` 边界切掉 | 文档要求 padding / 父面板留空间 / `custom_clip_content=0`；不做跨控件全局 glow pass |
| 小字号矢量轮廓与 raster 字形不完全一致 | 光晕边缘与字形有轻微偏差 | glow 是模糊层，通常不可见；截图覆盖 12/16px |
| 合成粗斜体的轮廓一致性 | `hb_font_draw_glyph()` 若不应用 synthetic bold/slant，glow 轮廓会比 raster 细/不倾斜 | G1 先验证；若不一致，在轮廓缓存生成时套同样的 embolden/slant 变换，或 glow 用变体 extents 扩边；加宽度/截图断言 |
| 彩色/bitmap 字形没有轮廓 | emoji 不发光 | 跳过；文档说明；不把 emoji 当 glow 目标 |
| 裁剪外的光晕 | 布局边缘被切、或溢出控件 | glow pass 只外扩 `3*sigma+2`；仍受父/控件 clip 限制；`custom_clip_content=0` 才允许越界 |
| 路径/stencil 裁剪 | OVG 直接渲染器下 effect 合成可能不遵守 stencil | vgui 后端只用 scissor；OVG 路径裁剪下的 glow 不在本期保证 |
| effect/opacity 状态泄漏 | 后续 fill 被错误加上 glow 或变成透明 | save/restore + 显式恢复测试；ctx/canvas 两种模式都覆盖 |
| opacity=0 填充被后端优化掉 | glow 不生成 mask | 当前 `fill_preserve` 不做 opacity 剔除；若未来加剔除，给 glow 命令加显式 mask-only 标记，不要退回透明填充（mask 读顶点 alpha，透明填充本来就不发光） |
| `glow_opacity=0` 语义 | 用户设置颜色但忘记设 opacity | 文档明确 0=1.0；vgui setter 内部固定传 1.0，避免宿主踩坑 |
| span 字段再次扩张 | 结构体尺寸变化 | 与 D2 字段同一次追加；append-only、源码兼容、不承诺二进制 ABI |
| disabled/主题色 | 禁用控件仍发光、亮色主题发灰 | glow alpha 跟随文本 disabled/opacity；亮色主题截图评审 |
| markdown 逐 segment 裁剪 | run 边界出现竖向接缝 | G2 两遍 + glow pass 外扩；G3 改逐行 spans |
| 性能测试缺失 | 首版上线后才发现掉帧 | G0 建立 10/50 个发光标签基准；G1/G2 每阶段跑 `vgpu_effects_supported` 环境的 frame 统计 |

**已确认的决策（2026-11-18）**：

> 以下按推荐方案确认，作为实现依据；G4 的可选项仍保留为实现阶段的备选。

1. **走矢量轮廓 + `VG_EFFECT_OUTER_GLOW`**，不做 raster 后处理、不做文本 shader 多 tap；SDF 留作长期选项。
2. **OVG 只加 `text_style_t` 字段**，不新增 `vg_add_text_glow()`；宿主和 vgui 都走现有 `add_text`。
3. **一个 run 一次批量 fill**，不逐字形 effect；性能回退时按行拆，不退回逐字形。
4. **不可见填充**：填充色保持 alpha=1 让 mask 有覆盖度，`set_opacity(0)` 让主体不可见；mask pass 会把 opacity 覆盖为 1，effect quad 用 `vg_effect_t.opacity`。不支持 effect 的后端自然无光；首版不加 capability query。
5. **glow 在阴影之前**：光晕最底层，阴影/描边/正文依次覆盖；如截图认为阴影应更靠近光晕，再调整顺序。
6. **vgui 控件 + span 双入口**：控件级用于普通 label，span 级用于 richtext/markdown；richtext 两遍绘制。
7. **sigma 逻辑像素、spread 无量纲**：vgui 绘制时 sigma 乘 `ui_scale`；OVG 保持设备像素。
8. **glow 不作用于 decoration、不作用于无轮廓彩色字形**；这两项放 G4/非目标。
9. **测量/命中测试不受 glow 影响**：glow 是纯绘制属性。
10. **相邻同参数 run 的合并放 G4**：MVP 先保证语义正确，避免首版引入跨 run 文本拼接的复杂度。
