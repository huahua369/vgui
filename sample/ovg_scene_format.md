# OVG Scene Format (OVGS) — JSON 序列化规范

本文档定义基于 `ovg_c.h` 渲染接口语义的矢量场景文件格式 `.ovgs`（文本 JSON），
用于**保存、读取、编辑**一帧矢量绘制内容。它**不直接录制/回放** `ovg_recording_t`
命令流，而是描述一个**可编辑的场景图**：变换、路径/形状、填充/描边样式、图片、
文本、3D 几何，再在加载时翻译为对 `ovg_ctx_cb` 的调用序列。

---

## 1. 文件结构

```json
{
  "format": "ovgs",
  "version": 1,
  "width": 800,
  "height": 600,
  "background": "00000000",
  "resources": { ... },
  "nodes": [ ... ]
}
```

| 字段        | 类型          | 说明 |
|-------------|---------------|------|
| format      | string        | 固定 `"ovgs"` |
| version     | int           | 格式版本，当前 `1` |
| width       | int           | 画布像素宽 |
| height      | int           | 画布像素高 |
| background  | string(hex)   | 8 位十六进制 RGBA 背景色（可省，默认透明） |
| resources   | object        | 资源表：图片、渐变、图案 |
| nodes       | array<node>   | 绘制节点列表，按序绘制 |

---

## 2. 变换（transform）

所有矩阵以 `mat3x2`（6 个 float，行主序 `xx yx xy yy x0 y0`）或简写形式表达：

```json
"transform": {
  "matrix": [1, 0, 0, 1, 100, 50]
}
```

也支持分解简写（等价）：

```json
"transform": { "translate": [100, 50], "scale": [1, 1], "rotate": 0.0 }
```

---

## 3. 节点（node）

每个节点是带 `type` 的联合对象。

### 3.1 组节点（group）

```json
{
  "type": "group",
  "name": "panel",
  "transform": { "translate": [10, 10] },
  "opacity": 1.0,
  "clip_rect": [0, 0, 200, 100],
  "children": [ ... ]
}
```

### 3.2 路径/形状节点（path）

```json
{
  "type": "path",
  "transform": { ... },
  "winding": "nonzero",
  "segments": [
    { "op": "move_to", "x": 0, "y": 0 },
    { "op": "line_to", "x": 100, "y": 0 },
    { "op": "line_to", "x": 100, "y": 80 },
    { "op": "close" }
  ],
  "fill":   { "pattern": "@solid_red" },
  "stroke": {
    "pattern": "@solid_blue",
    "width": 2.0,
    "cap": "butt", "join": "miter", "miter_limit": 4.0,
    "dash": { "array": [4, 4], "offset": 0 }
  }
}
```

**预定义形状快捷**（等价于路径，便于编辑）：

```json
{ "type": "rect",     "x":0,"y":0,"w":100,"h":80 }
{ "type": "rounded_rect", "x":0,"y":0,"w":100,"h":80,"rx":8,"ry":8 }
{ "type": "circle",   "x":50,"y":50,"radius":40 }
{ "type": "ellipse",  "cx":50,"cy":50,"rx":60,"ry":30 }
```

### 3.3 图片节点（image）

```json
{
  "type": "image",
  "image": "@img_logo",
  "src_rect": [0,0,256,256],
  "dst_rect": [0,0,128,128],
  "sliced":   [10,10,10,10],
  "color":    "FFFFFFFF",
  "flip":     "none"
}
```

`src_rect / dst_rect` 均为 `[x,y,w,h]`；`sliced` 为九宫格 `[l,t,r,b]`。

### 3.4 文本节点（text）

```json
{
  "type": "text",
  "text": "Hello",
  "box": { "x":0,"y":0,"w":200,"h":60, "auto_break":true,
           "word_wrap":2, "ellipsis":false },
  "font": { "family": "Noto Sans", "style": "Regular" },
  "style": {
    "fontsize": 16, "lineheight": 1.2,
    "align": [0.0, 0.5], "box_align": [0.0, 0.0],
    "color": "FFC2C2C2", "stroke_color": "FF000000",
    "stroke_width": 0, "shadow_color": "CC121212",
    "shadow_offset": [1,1], "min_subpixel": 0
  }
}
```

### 3.5 几何节点（geometry）

```json
{
  "type": "geometry",
  "mode": "triangles",
  "blend": "normal",
  "cull": "none",
  "texture": "@img_tex",
  "double_sided": false,
  "vertices": [ { "pos":[0,0,0],"uv":[0,0],"color":"FFFFFFFF" }, ... ],
  "indices": [0,1,2],
  "instances": [ { "matrix": [ ...16 floats... ] } ]
}
```

`mode`：`triangles | triangle_strip | lines | line_strip | points`；
`cull`：`none | front | back`；`blend` 见 blendMode。

---

## 4. 样式与资源

### 4.1 纯色（solid）

```json
"solid_red": { "type": "solid", "color": "FFFF0000" }
```

### 4.2 线性渐变（linear）

```json
"grad_line": {
  "type": "linear", "p0":[0,0], "p1":[100,0],
  "extend": "pad", "filter": "bilinear",
  "stops": [
    { "offset": 0.0, "color": "FFFF0000" },
    { "offset": 1.0, "color": "FF0000FF" }
  ]
}
```

### 4.3 径向渐变（radial）

```json
"grad_rad": {
  "type": "radial",
  "c0":[50,50],"r0":0, "c1":[50,50],"r1":50, "ellipse":false,
  "extend":"pad", "stops":[ ... ]
}
```

### 4.4 锥形渐变（sweep）

```json
"grad_sweep": {
  "type": "sweep", "center":[50,50],
  "start_angle": 0, "end_angle": 6.2831853,
  "stops":[ ... ]
}
```

### 4.5 图片纹理（texture）

```json
"img_logo": {
  "type": "texture",
  "format": "rgba8",
  "width": 256, "height": 256,
  "pixels_base64": "iVBOR...",
  "multiply": false
}
```

`format`：`rgba8 | bgra8 | rgba8_srgb | bgra8_srgb | rgba16f | rgba32f`。
`pixels_base64` 是**原始像素字节流**的 base64（不是 PNG/JPEG），与 `vg_image_desc_t`
的 `pixels/stride/px_size` 语义一致。

---

## 5. 枚举字符串映射

| 类别 | 字符串值 |
|------|----------|
| winding（填充规则） | `evenodd`、`nonzero` |
| cap | `butt`、`round`、`square` |
| join | `miter`、`round`、`bevel` |
| extend | `none`、`repeat`、`reflect`、`pad` |
| filter | `fast`、`good`、`best`、`nearest`、`bilinear`、`gaussian` |
| blend | `none`、`normal`、`additive`、`multiply`、`modulate`、`screen`、`normal_prem`、`additive_prem` |
| flip | `none`、`horizontal`、`vertical`、`both` |

---

## 6. 语义约束

1. `resources` 中的键以 `@` 前缀在节点里被引用（如 `"@solid_red"`）。
2. 所有颜色为 **8 位 hex RGBA，无 `0x`/`#` 前缀**，字节顺序与 `uint32_t color`
   字段一致（头文件中 `0xffc2c2c2` 样式）。
3. 节点列表顺序即绘制顺序；`group` 内子节点共享父级变换与裁剪上下文。
4. `path` 的 `fill`/`stroke` 若省略则该操作不执行。
5. 格式加载后应与 `ovg_ctx_cb` 的调用结果**视觉等价**；实现应只依赖头文件语义，
   不依赖录制回放接口。
