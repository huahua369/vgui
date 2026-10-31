# vgui_chart：图表、仪表盘与 JSON 仪表板（P0/P1）

`vgui_chart` 是 vgui 的可选图表模块。公共接口是纯 C（C99/C11），实现使用 C++17；C++ 类型、模板、异常和 `std::` 容器不会出现在公共头中。P0 提供折线图、面积图、柱状图、径向仪表盘和 JSON 仪表板，并覆盖十万点级序列、配置持久化与 CSV 导出。P1 在保持 P0 API 与 schema version 1 兼容的前提下，增加响应式仪表板布局、日期时间轴、PNG 导出和 JSON 数据快照。

公共头：

```c
#include <vgui/chart/vgui_chart.h>
```

模块不增加新的核心控件枚举：图表和仪表盘内部使用 `VGUI_CUSTOM` 绘制，外层仍是普通 vgui 控件，因此主题、裁剪、焦点环和布局沿用核心行为。

## P0 范围

已实现：

- 折线（`VGUI_CHART_LINE`）、面积（`VGUI_CHART_AREA`）和柱状（`VGUI_CHART_BAR`）图。
- 径向仪表盘（Gauge），支持范围、角度、刻度、数值、单位和色带。
- 单图多条序列；每序列十万点级的批量追加、预分配和固定容量环形缓冲。
- 按像素列做 min/max 降采样，保留尖峰，避免十万点直接生成十万段路径。
- 十字准线、悬浮提示、图例开关、鼠标平移/缩放、键盘导航和公告。
- JSON 仪表板 schema version 1，`layout.mode = "stack"` 单列布局。
- JSON 配置保存与重新加载；失败加载保留加载前的仪表板。
- CSV 长格式导出。

P1 在同一 schema version 1 上追加（详见各章节）：

- `wrap` / `grid` / `columns` / `span` 响应式仪表板布局。
- 真正的日期时间轴（`VGUI_CHART_SCALE_DATETIME`）：epoch 语义、UTC/本地时区、日历刻度。
- PNG 导出：宿主安装渲染钩子，模块负责编码与流式输出。
- JSON 数据快照：配置 + 点数据 + KPI/仪表运行时值。

## 构建与链接

`VGUI_BUILD_CHART` 默认开启：

```cmake
set(VGUI_BUILD_CHART ON CACHE BOOL "" FORCE)
add_subdirectory(third_party/vgui)
target_link_libraries(my_app PRIVATE vgui::chart)
```

模块是静态库，公共依赖 `vgui::vgui`；`nlohmann_json` 只作为私有实现依赖。纯 C 消费者仍然用 C 编译源文件，但因为静态库实现是 C++，最终链接应交给 C++ 链接器。仓库中的 `sample/vgui_chart_test.c` 就是这样做的：

```cmake
add_executable(my_chart_app main.c)
target_link_libraries(my_chart_app PRIVATE vgui::chart)
set_target_properties(my_chart_app PROPERTIES LINKER_LANGUAGE CXX)
```

安装后导出的目标名为 `vgui::chart`，使用方式：

```cmake
find_package(vgui CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE vgui::vgui vgui::chart)
```

## 数据模型与容量

模块拥有全部点数据。宿主通过 `vgui_chart_set_points()` 或批量的 `vgui_chart_append_points()` 推送数据；绘制过程中不会回调宿主，因此不存在“宿主数据在绘制时被并发修改”的问题。

### 点的顺序和空值

- `x` 必须有限。`set_points()` 会复制输入并按 `x` 稳定排序；`append_points()` 要求整批 `x` 非递减，否则整批拒绝，不会留下部分写入。
- 允许重复的 `x`，它们保持稳定顺序。
- `y` 可以为 `NaN` 或无穷；非有限 `y` 不参与自动范围统计，并在折线中形成断点。CSV 会按 `nan` / `inf` / `-inf` 导出。

### 预分配与环形缓冲

- `vgui_chart_reserve(chart, series, 100000)` 只预分配容量，不改变点数，也不设上限。
- `vgui_chart_set_ring_capacity(chart, series, capacity)` 开启固定容量环形缓冲；`capacity = 0` 关闭。
  - 追加超过容量时覆盖最旧点；
  - 缩小容量时保留最新的 `capacity` 个点；
  - `set_points()` 输入超过容量时同样只保留最新的点；
  - `vgui_chart_point_count()` 始终不超过容量。
- 建议实时采集使用 `reserve(100000)` 后批量 `append_points()`；历史数据用 `set_points()` 一次性替换。

### 十万点级绘制

默认启用 `VGUI_CHART_DECIMATION_MIN_MAX`：每个屏幕像素列聚合一个 min/max 对，折线保留每列的谷值和峰值。缩放、平移、改变尺寸或修改序列后会重新计算缓存。`vgui_chart_get_stats()` 可读取最近一次绘制的原始点数、可见点数、实际绘制顶点数、序列数和缓存命中状态，适合做性能断言或调试覆盖层。

`VGUI_CHART_DECIMATION_NONE` 会关闭降采样，十万点会生成完整折线；通常只用于离屏导出或点数较少的场景。

## 图表

### 创建与序列

```c
vgui_chart* chart = vgui_chart_create(root);
vgui_widget_set_pos(vgui_chart_widget(chart), 16, 16);
vgui_widget_set_size(vgui_chart_widget(chart), 720, 360);
vgui_chart_set_title(chart, "Request latency");

vgui_chart_series_desc desc;
vgui_chart_series_desc_default(&desc);
desc.key = "latency";          /* 稳定字符串键，图表内唯一 */
desc.name = "Latency";         /* 可为 NULL，缺省使用 key */
desc.kind = VGUI_CHART_LINE;
desc.unit = "ms";
desc.color = 0xFFFF3B30;       /* 0 = 使用主题调色板 */
vgui_series_id latency = vgui_chart_add_series(chart, &desc);
```

序列键用于持久化和仪表板数据绑定；`vgui_series_id` 只在当前图表会话内有效，不应写入配置文件。`vgui_chart_find_series()` 按 key 查找 id，`vgui_chart_remove_series()` 删除序列。

`vgui_chart_update_series()` 可更新名称、颜色、线宽、填充透明度、可见性、堆叠和单位；已经有数据后不能改变 `kind`，否则返回 0 并设置错误。柱状图和面积图可设置 `stacked = 1` 并按 `stack_group` 分组堆叠。

### 追加数据

```c
vgui_chart_reserve(chart, latency, 100000);
vgui_chart_point batch[1024];
for (size_t i = 0; i < 1024; ++i) {
    batch[i].x = t0 + (double)i * 0.05;
    batch[i].y = sample_latency(t0 + (double)i * 0.05);
}
vgui_chart_append_points(chart, latency, batch, 1024);
```

`vgui_chart_set_points()` 替换整条序列；`vgui_chart_clear_points()` 清空；`vgui_chart_point_count()` 查询当前点数。

### 坐标轴、格式与视图

```c
vgui_chart_axis_desc x_axis;
vgui_chart_axis_desc_default(&x_axis);
x_axis.scale = VGUI_CHART_SCALE_TIME;  /* x = 经过的秒数，标签显示 30s / 1m30s */
x_axis.tick_count = 6;
vgui_chart_set_axis(chart, 0, &x_axis);

vgui_chart_axis_desc y_axis;
vgui_chart_axis_desc_default(&y_axis);
y_axis.min = 0.0;
y_axis.max = 100.0;
y_axis.tick_count = 6;
y_axis.unit = "ms";
y_axis.format_pattern = "%.0f";
vgui_chart_set_axis(chart, 1, &y_axis);
```

- 轴 0 是 X 轴，轴 1 是左侧 Y 轴。P0 只有一个 Y 轴；`series_desc.axis` 目前只接受 0。
- `min >= max` 表示自动范围。柱状图始终包含 0；折线/面积图设置 `include_zero = 1` 后也包含 0。
- `format_pattern` 必须是恰好包含一个浮点转换的 printf 风格模式（如 `%.1f`、`%g ms`）。为了安全，`%n`、字符串转换和多个转换会被拒绝。
- `format` 回调优先级高于格式模式；默认格式为 `%.6g`。
- X 轴 `VGUI_CHART_SCALE_TIME` 只把数值解释为经过的秒数，不处理 epoch、日期、时区或月份。

### 日期时间轴（P1）

`VGUI_CHART_SCALE_DATETIME` 把 X 解释为 Unix epoch 秒（1970-01-01T00:00:00Z 起），与表示“经过秒数”的 `VGUI_CHART_SCALE_TIME` 互相独立：

```c
vgui_chart_axis_desc x_axis;
vgui_chart_axis_desc_default(&x_axis);
x_axis.scale = VGUI_CHART_SCALE_DATETIME;
x_axis.time_zone = 0;   /* 0 = UTC（默认），1 = 宿主本地时区 */
x_axis.tick_count = 6;
vgui_chart_set_axis(chart, 0, &x_axis);
```

- 刻度对齐日历边界：秒/分/时/天/周/月/年；月和年刻度落在当月 1 日、当年 1 月 1 日的零点。选择哪一档由视图跨度与 `tick_count` 决定。
- 本地时区（`time_zone = 1`）的午夜与月末边界按宿主时区换算，夏令时切换期间的偏移逐刻度重算。
- 刻度标签按档位选择格式：`HH:mm:ss`、`HH:mm`、`MM-DD HH:mm`、`MM-DD`、`YYYY-MM`、`YYYY`。设置 `format` 回调或 `format_pattern` 时优先使用宿主格式（收到的是 epoch 原始值）。
- 悬浮提示、十字准线和键盘公告中的值也按该轴的时区显示完整时间 `YYYY-MM-DD HH:mm:ss`。
- JSON 中写作 `"scale": "datetime"`，时区写作 `"timezone": "utc"`（默认，可省略）或 `"local"`。
- 刻度值与文本可以通过 `vgui_chart_get_axis_ticks()` 读取（见下）。它反映最近一次绘制的结果，绘制前返回 0。

视图与跟随：

```c
vgui_chart_set_view(chart, t0, t0 + 60.0);  /* 手动窗口，关闭 follow */
vgui_chart_reset_view(chart);                /* 恢复 options.follow 或全量数据 */
vgui_chart_set_follow(chart, 1);
```

`options.follow = 1` 且 `options.window > 0` 时，视图为 `[最新 x - window, 最新 x]`；`window = 0` 或未开启 follow 且没有手动视图时显示全部数据。`vgui_chart_get_view()` 只在存在手动视图时返回 1。

### 选项和主题

```c
vgui_chart_options options;
vgui_chart_options_default(&options);
options.legend = 1;
options.tooltip = 1;
options.crosshair = 1;
options.follow = 1;
options.window = 120.0;
options.decimation = VGUI_CHART_DECIMATION_MIN_MAX;
vgui_chart_set_options(chart, &options);
```

默认值：图例、提示、十字准线、follow 开启，`window = 0`，降采样为 min/max。

主题默认从当前 `vgui_theme` 派生，并在每次绘制时重新派生，因此宿主切换主题后图表会跟随；显式调用 `vgui_chart_set_theme()` 后使用自定义主题，传入 `NULL` 恢复自动派生。`vgui_chart_theme_from_theme()` 可在任意时刻生成一份独立主题快照，`vgui_chart_theme_high_contrast()` 提供高对比度版本。

### 交互

- 鼠标悬停：选择最近序列上的最近点，显示十字准线和提示。
- 点击图例项：显示/隐藏对应序列。
- 中键拖动：水平平移。
- `Ctrl + 滚轮`：以光标位置为锚缩放。
- 双击图表：恢复自动视图。
- 键盘：`+` / `-` 缩放，`0` 恢复视图，`Home` / `End` 跳到首/末点，左右方向键逐点移动，上下方向键切换可见序列，`Enter` 通过 `vgui_context_announce()` 朗读当前点，`Esc` 清除悬浮/取消平移。
- `vgui_chart_get_stats()` 不参与交互，只反映最近一次绘制的缓存结果。
- `vgui_chart_get_axis_ticks(chart, axis, out, cap)` 复制最近一次绘制产生的刻度（值 + 标签文本，文本在下一次绘制前有效）；返回总刻度数，绘制前返回 0。可用于自定义覆盖层与测试。

## 仪表盘

```c
vgui_gauge* gauge = vgui_gauge_create(root);
vgui_widget_set_size(vgui_gauge_widget(gauge), 260, 260);

vgui_gauge_zone zones[2] = {
    { 0.0, 70.0, 0xFF00C853, "normal" },
    { 70.0, 100.0, 0xFFD50000, "critical" },
};

vgui_gauge_options options;
vgui_gauge_options_default(&options);
options.min = 0.0;
options.max = 100.0;
options.start_angle = 135.0;
options.sweep = 270.0;
options.show_ticks = 1;
options.show_value = 1;
options.label = "Disk usage";
options.unit = "%";
options.format_pattern = "%.0f";
options.zones = zones;
options.zone_count = 2;
vgui_gauge_set_options(gauge, &options);
vgui_gauge_set_value(gauge, 78.0);
```

- `sweep` 会钳制到 10..360 度，`start_angle` 按 360 度取模。
- `zones` 最多读取 32 项；非有限的 `from` / `to` 会被跳过。色带颜色为 0 时从图表主题派生。
- 值超出范围时弧形会钳制到 `[min, max]`，但显示文本仍使用钳制后的值。
- `vgui_gauge_set_value(gauge, NAN)` 表示“无数据”，显示 `--`；`vgui_gauge_get_value()` 在无数据时返回 `NaN`。
- 仪表盘有 a11y 角色 Progress，名称优先使用 `label`；图表名称使用标题，并支持 `Enter` 公告。

## JSON 仪表板（schema version 1）

`vgui_dashboard_create()` 创建单列仪表板根面板，`vgui_dashboard_load_json()` 解析 JSON 并重建卡片树。加载是原子的：解析或构建失败时不会修改已有仪表板，错误文本包含 JSON 路径（例如 `cards[1].axes.y.max: expected number`）。

```c
vgui_dashboard* dashboard = vgui_dashboard_create(root);
vgui_widget_set_pos(vgui_dashboard_widget(dashboard), 12, 12);
vgui_widget_set_size(vgui_dashboard_widget(dashboard), 900, 720);

char error[256];
if (!vgui_dashboard_load_json(dashboard, json_text, json_len,
        error, sizeof(error))) {
    fprintf(stderr, "dashboard: %s\n", error);
}
```

重新加载会保留之前根面板的位置和尺寸（按 `ui_scale` 换算），所以不会在刷新配置后缩回首选尺寸。首次加载前应先设置根面板矩形。

### 顶层结构

```json
{
  "version": 1,
  "title": "Operations",
  "layout": { "mode": "stack", "gap": 10, "padding": 12 },
  "cards": []
}
```

| 字段 | 必填 | 说明 |
|---|---|---|
| `version` | 是 | 必须是整数 `1`。 |
| `title` | 否 | 顶部标题。 |
| `layout.mode` | 否 | `"stack"`（默认，单列）、`"wrap"`（自动换行）、`"grid"`（行优先网格）或 `"columns"`（轮转分列）。 |
| `layout.columns` | 否 | `grid` / `columns` 的列数，默认 2，钳制到 1..12。 |
| `layout.gap` | 否 | 卡片间距，默认 10，钳制到 0..128。 |
| `layout.padding` | 否 | 内边距，默认 12，钳制到 0..128。 |
| `cards` | 是 | 卡片数组。 |

布局模式语义：

- `stack`：所有卡片在单列中垂直堆叠，行为与 P0 完全一致。
- `wrap`：卡片按各自首选尺寸从左到右排列，放不下时换行；`span` 被忽略。
- `grid`：`columns` 列的行优先网格。卡片 `span`（默认 1，钳制到列数）表示占据的单元格数；放不下时整卡移到下一行。跨列卡片的最小宽度按 `span` 放大。
- `columns`：卡片按顺序轮转放入 `columns` 个等宽纵向栏；`span` 被忽略。

卡片 `id` 在仪表板内唯一，是数据绑定和 CSV 中 `card` 列使用的稳定字符串。未知卡片类型、重复 id、非对象卡片都会导致整次加载失败。

### KPI 卡片

```json
{
  "type": "kpi",
  "id": "cpu",
  "title": "CPU",
  "value_key": "cpu.value",
  "delta_key": "cpu.delta",
  "text_key": "cpu.text",
  "status_key": "cpu.status",
  "format": "%.1f",
  "unit": "%",
  "value": 0.0,
  "status": 2,
  "status_text": "warm"
}
```

- `value_key` 由 `vgui_dashboard_set_number()` 更新，显示值由 `format`（printf 浮点模式）和 `unit` 格式化。
- `delta_key`、`text_key` 由 `vgui_dashboard_set_text()` 更新；文本为空时对应标签隐藏。
- `status_key` 由 `vgui_dashboard_set_status()` 更新；也可以用卡片 `id` 作为 key。状态值：0 normal、1 info、2 warning、3 critical、4 stale。
- `value` 可提供初始值；未提供时显示 `--`。P0 持久化不保存 KPI 运行时值。

### 图表卡片

```json
{
  "type": "chart",
  "id": "load",
  "title": "Load",
  "options": {
    "legend": true,
    "tooltip": true,
    "crosshair": true,
    "follow": false,
    "window": 0,
    "decimation": "min_max"
  },
  "axes": {
    "x": { "scale": "time", "ticks": 6, "grid": true },
    "y": { "min": 0, "max": 100, "ticks": 6, "grid": true, "unit": "req/s" }
  },
  "series": [
    { "key": "rps", "name": "Requests", "kind": "line", "color": "#FF2D95", "unit": "req/s" },
    { "key": "errors", "name": "Errors", "kind": "bar", "color": "#00E5FF" }
  ],
  "view": { "follow": true }
}
```

- `options` 字段对应 `vgui_chart_options`；`decimation` 接受 `"none"`、`"min_max"`（也接受 `"minmax"`）或对应整数。
- `axes.x` / `axes.y` 字段对应 `vgui_chart_axis_desc`：
  - `scale`: `"linear"`、`"time"`（`"duration"` 也接受，经过秒数）或 `"datetime"`（`"epoch"` 也接受，Unix epoch 秒）；
  - `timezone`: `"utc"`（默认，可省略）或 `"local"`，仅对 `"datetime"` 有效；
  - `min` / `max`: 两者都有效且 `min < max` 时为固定范围；否则自动；
  - `ticks` 或 `tick_count`: 2..32，默认 5；
  - `include_zero`、`grid`、`label`、`unit`、`format` / `format_pattern`。
  - 图表卡片的 Y 轴始终按 linear 解释；X 轴可设为 time（经过秒数）或 datetime（epoch 秒）。
- `series` 中 `key` 必填且同卡片内唯一；`kind` 为 `"line"`、`"bar"`、`"area"` 或对应整数；`axis` 只支持 0。
- 序列的可选字段：`name`、`unit`、`color`、`line_width`（0.5..16）、`fill_alpha`（0..1）、`visible`、`stacked`、`stack_group`。
- 颜色接受 `"#RGB"`、`"#RRGGBB"`、`"#AARRGGBB"`，或 32 位整数；保存时统一写成 `#AARRGGBB`。
- `view`：提供 `x0` 和 `x1` 表示手动窗口（`x1 > x0`）；否则 `follow` 控制是否跟随最新数据。

数据写入：

```c
vgui_dashboard_append_points(dashboard, "load", "rps", points, count);
vgui_chart* chart = vgui_dashboard_chart(dashboard, "load");
vgui_gauge* gauge = vgui_dashboard_gauge(dashboard, "disk");
```

`vgui_dashboard_chart()` / `vgui_dashboard_gauge()` 返回的句柄归仪表板所有，不能单独 `destroy`。卡片内的图表也可以使用所有普通图表 API，例如 `vgui_chart_set_palette()`、`vgui_chart_set_theme()` 和 `vgui_chart_set_view()`。

### 仪表盘卡片

```json
{
  "type": "gauge",
  "id": "disk",
  "title": "Disk",
  "value_key": "disk.value",
  "gauge": {
    "min": 0,
    "max": 100,
    "start_angle": 135,
    "sweep": 270,
    "show_ticks": true,
    "show_value": true,
    "label": "Disk usage",
    "unit": "%",
    "format": "%.0f",
    "zones": [
      { "from": 0, "to": 70, "color": "#00C853", "label": "normal" },
      { "from": 70, "to": 100, "color": "#D50000", "label": "critical" }
    ]
  }
}
```

`gauge` 对象缺失时使用默认范围 0..100、135° 起点、270° 扫角。`value_key` 也可以放在卡片顶层；仪表值通过 `vgui_dashboard_set_number()` 更新。P0 持久化不保存仪表运行时值。

### 保存与重新加载

```c
int write_file(void* user, const char* data, size_t size) {
    return fwrite(data, 1, size, (FILE*)user) == size;
}

vgui_dashboard_save_json(dashboard, write_file, file);

/* 带数据快照的版本 */
vgui_dashboard_save_json_ex(dashboard,
    VGUI_CHART_SAVE_DATA | VGUI_CHART_SAVE_RUNTIME, write_file, file);
```

- `vgui_dashboard_save_json()` 输出缩进后的 schema version 1 JSON，通过 `vgui_chart_write_fn` 流式写出，回调可能被调用多次；任一次返回 0 都会终止并返回失败。
- 保存内容：标题、布局 mode/columns/gap/padding、卡片 id/标题/绑定键/格式/单位/span、图表选项、轴配置（含 datetime 时区）、序列配置、图表手动视图或 follow、仪表盘范围/角度/刻度/单位/色带。
- 不保存：图表点数据、KPI 当前值、仪表当前值、临时主题、运行时 tooltip/悬浮状态。需要在重启后恢复数据时，由宿主重新调用 `append_points()` / `set_points()`，或改用下面的数据快照。
- 保存成功后 `vgui_dashboard_last_error()` 为空；加载成功后也会清空错误。

### 数据快照（P1）

`vgui_dashboard_save_json_ex()` 通过标志位在配置之外追加可选段，输出仍是 schema version 1，普通 `vgui_dashboard_load_json()` 会自动识别并应用：

| 标志 | 追加段 | 内容 |
|---|---|---|
| `VGUI_CHART_SAVE_DATA` | `"data"` | 每个图表卡片的每条序列的点数组 `[[x, y], ...]`；环形缓冲按从旧到新的逻辑顺序；`y` 为 `null` / `"nan"` / `"inf"` / `"-inf"` 表示非有限值。 |
| `VGUI_CHART_SAVE_RUNTIME` | `"runtime"` | KPI 当前值、delta/副标题文本、状态；仪表当前值。键为对应的绑定键。 |

```json
{
  "version": 1,
  "cards": [ "...同配置保存..." ],
  "data": {
    "load": { "rps": [[0, 42.5], [0.5, null], [1, 43.1]] }
  },
  "runtime": {
    "cpu.value": 42.5,
    "cpu.delta": "+1.2%",
    "cpu.status": [2, "warm"],
    "disk.value": 78.0
  }
}
```

- 加载时 `"data"` 先按 schema 校验：引用不存在的卡片/序列、非单调 `x`、非法值都会让整次加载失败并回滚（配置与数据是原子的）。
- 恢复环形缓冲容量不在快照内：宿主在重新加载后按需调用 `vgui_chart_set_ring_capacity()`；数据超过容量时保留最新点。
- `"runtime"` 在卡片树重建后通过 `vgui_dashboard_set_number()` / `set_text()` / `set_status()` 应用。

## CSV 导出

CSV 使用长格式，便于直接导入数据库或表格工具。

独立图表：

```text
series,x,y
"latency",0,42.5
"latency",0.05,43.1
```

仪表板：

```text
card,series,x,y
"load","rps",0,42.5
"load","errors",0,3
```

```c
static int write_csv(void* user, const char* data, size_t size) {
    return fwrite(data, 1, size, (FILE*)user) == size;
}

vgui_chart_export_csv(chart,
    VGUI_CHART_EXPORT_HEADER | VGUI_CHART_EXPORT_VISIBLE_ONLY,
    write_csv, file);

vgui_dashboard_export_csv(dashboard,
    VGUI_CHART_EXPORT_HEADER, write_csv, file);
```

导出选项：

| 标志 | 作用 |
|---|---|
| `VGUI_CHART_EXPORT_HEADER` | 写表头；仪表板表头为 `card,series,x,y`。 |
| `VGUI_CHART_EXPORT_VISIBLE_ONLY` | 只导出当前 X 视图内的点；无手动视图时使用 follow 窗口或全量范围。 |
| `VGUI_CHART_EXPORT_DECIMATED` | 按当前绘图宽度导出每像素列的 min/max 点；柱状图仍按原始点导出。 |
| `VGUI_CHART_EXPORT_INCLUDE_HIDDEN` | 同时导出隐藏序列；默认跳过隐藏序列。 |

- 文本字段（`series`、`card`）始终加双引号，内部 `"` 写成 `""`。
- 数值使用 `%.17g`，足以无损往返 `double`；非有限值写成 `nan`、`inf`、`-inf`。
- 行顺序为序列添加顺序；同一序列内按数据顺序。环形缓冲按从旧到新的逻辑顺序导出。
- 如果没有任何可导出的序列，返回 0 并设置错误。写入回调失败同样返回 0。
- 仪表板导出复用同一套逐图表导出，自动附加卡片 id 列。

## PNG 导出（P1）

图表模块本身不接触渲染器。宿主进程安装一次渲染钩子，模块在导出时驱动它取回像素并用内置编码器输出 PNG：

```c
int host_png_render(void* user, vgui_widget* widget,
    int width, int height, vgui_color background,
    const uint8_t** out_rgba, int* out_stride) {
    /* 离屏渲染 widget 所在界面，返回 8 位 RGBA、自上而下、
     * out_stride 字节每行；缓冲保持有效直到下一次调用。 */
    return render_offscreen(user, widget, width, height, background,
        out_rgba, out_stride);
}

vgui_chart_set_png_renderer(host_png_render, &renderer);

vgui_chart_png_desc desc;
vgui_chart_png_desc_default(&desc);
desc.width = 1280;
desc.height = 720;
desc.background = 0xFF1E232B;   /* 0 = 透明 */
vgui_chart_export_png(chart, &desc, write_file, file);
vgui_dashboard_export_png(dashboard, &desc, write_file, file);
```

- 未安装钩子时导出失败，`last_error` 提示 `no PNG renderer installed`。钩子返回 0（如无 GPU 设备）同样以失败收场。
- 钩子负责把 `widget` 子树渲染到 `width x height`；宿主可以裁剪自己的帧缓冲，也可以临时调整界面尺寸。`background` 是模块传入的底色，应绘制在子树之下。
- `width` / `height` 钳制到 1..16384；输出为二进制 PNG（RGBA，8 位），经 `vgui_chart_write_fn` 流式写出。
- 导出的图像是普通位图，不包含运行时 tooltip / 十字准线状态。
- 钩子是进程级单例，属于 UI 线程；传 `NULL` 卸载。

## 生命周期、线程与错误处理

- 所有对象都属于 UI 线程，不能跨线程调用。
- `vgui_chart_destroy()` / `vgui_gauge_destroy()` / `vgui_dashboard_destroy()` 会销毁自己拥有的控件；必须在父控件或 `vgui_context` 之前调用。
- 仪表板拥有卡片内的图表和仪表；不要对 `vgui_dashboard_chart()` 返回的句柄调用 `vgui_chart_destroy()`。
- 所有需要扩展的输入结构体都以 `struct_size` 开头，并提供 `*_default()` 初始化函数；`vgui_chart_theme` 通过 `vgui_chart_theme_from_theme()` / `vgui_chart_theme_high_contrast()` 填充，`vgui_chart_stats` 是输出结构。旧代码只填写自己认识的字段时，实现按 `struct_size` 截断读取，保持 ABI 兼容。
- 字符串按 UTF-8 复制；`vgui_chart_last_error()` 等返回的指针在下一次修改同一对象前有效。
- 失败返回 0（或 NULL/0 id），成功返回非 0。`last_error` 给出最近一次失败原因；建议始终以返回值判断成功，而不是把空错误字符串当作成功条件。
- 销毁前建议先移除宿主自己的回调；模块不会保存宿主指针到绘制回调之外。

## 无障碍与主题

- 图表控件的 a11y 名称为标题（无标题时为 `Chart`），可通过 `vgui_chart_set_title()` 更新；仪表盘的名称为 `label`（无 label 时为 `Gauge`），角色为 Progress。
- 图表获得焦点后，键盘可以逐点移动十字准线；按 `Enter` 通过 `vgui_context_announce()` 公告当前序列、值和单位。
- 仪表板 KPI 的数值标签会设置卡片标题为 a11y 名称；卡片面板使用卡片标题作为名称。
- 图表和仪表盘默认主题从 `vgui_theme` 派生；宿主切换主题不需要手动刷新。自定义主题可分别通过 `vgui_chart_set_theme()` / `vgui_gauge_set_theme()` 设置，传 `NULL` 恢复自动主题。
- 所有几何参数使用逻辑单位，内部随 `ui_scale` 缩放；`vgui_widget_get_rect()` 仍返回有效像素。

## 测试与示例

- `sample/vgui_chart_test.c`：纯 C 无窗口测试，覆盖十万点追加/读取、环形缓冲、CSV、JSON schema v1、加载失败回滚、几何保持和保存/重载；P1 部分覆盖 wrap/grid/columns 布局与 span、日期时间轴 JSON 往返、数据快照（含 NaN/Inf 与环形顺序）和 PNG 导出（合成渲染器）。
- `sample/vgui_chart_render_test.cpp`：离屏 FBO 回读，覆盖折线/柱状/仪表盘像素、PNG 输出；P1 部分覆盖 datetime 轴 UTC 午夜刻度值与标签、grid 布局渲染和真实 FBO 渲染器的 PNG 导出；无 Vulkan 设备时返回 77，CTest 标记 SKIP。

```bash
ctest --test-dir out/build/x64-debug -R vgui_chart --output-on-failure
```

单独运行：

```text
out/bin/x64d/vgui_chart_test.exe
out/bin/x64d/vgui_chart_render_test.exe
```

## P1 已交付

P1 四项能力均已落地，且保持 P0 API 与 schema version 1 兼容（只新增可选字段与枚举值）：

- `wrap` / `grid` / `columns` / `span` 响应式仪表板布局。
- 日期时间轴：epoch 语义、UTC/本地时区、日历刻度与宿主格式化（`vgui_chart_get_axis_ticks()`）。
- PNG 导出（`vgui_chart_set_png_renderer()` + `vgui_chart_export_png()` / `vgui_dashboard_export_png()`）。
- JSON 数据快照（`vgui_dashboard_save_json_ex()` + `VGUI_CHART_SAVE_DATA` / `VGUI_CHART_SAVE_RUNTIME`）。

后续不兼容变更会使 schema 版本递增。
