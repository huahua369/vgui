// Headless command-level coverage for styled rich-text layout and paint.
#include <glm/glm.hpp>
#include <ovg_c.h>
#include <vgui/richtext.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace {
struct command {
    char kind = 'D';
    std::string text;
    ivec4 clip{};
    ivec4 box{};
    int baseline = 0;
    float width = 0, height = 0;
    vgui_color color = 0;
    float x = 0, y = 0;
    float font_size = 0;
    vec2 text_align{};
};
constexpr ivec4 parent_clip{5, 4, 180, 100};
ivec4 clip = parent_clip;
std::vector<command> commands;
command decoration;
int failures = 0;

void check(bool ok, const char* message, const char* context = nullptr) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s%s%s\n", context ? context : "", context ? ": " : "", message);
        ++failures;
    }
}

void record_text(rvg_t*, text_st_t* t, text_style_t* st, text_box_rt* box) {
    commands.push_back({st->color_glow ? 'G' : 'T', std::string(t->text, t->text_len),
        clip, box->rc, box->rc.y + box->baseline, 0, 0,
        st->color_glow ? st->color_glow : st->color, t->pos.x, t->pos.y, st->fontsize, box->text_align});
}

void test_international_spans(vgui_widget* widget, ovg_ctx_cb& cb, rvg_t& vg) {
    // Symmetric red/blue channels keep the expected colors independent of
    // VGUI's AARRGGBB to OVG's AABBGGRR conversion.
    constexpr vgui_color base = 0xffffffff, first = 0xffaa33aa, second = 0xff44bb44;
    constexpr float base_size = 16, x = 10, width = 170;
    struct expected_run {
        const char* text;
        vgui_color color;
        float size;
    };
    const auto check_line = [&](const char* name, const char* text,
        std::initializer_list<vgui_text_span> spans,
        std::initializer_list<expected_run> expected, float align = 0.0f) {
        commands.clear();
        const int count = static_cast<int>(spans.size());
        vgui_custom_draw_text_spans(widget, &cb, &vg, text, spans.begin(), count,
            x, 10, width, 88, base, align, 0, base_size);
        check(commands.size() == expected.size(), "unexpected number of text runs", name);

        // The fragment advances are the reference; this does not assert
        // equivalence to a paragraph shaped as one string.
        std::vector<float> advances;
        float total = 0;
        for (const auto& run : expected) {
            const float advance = vgui_widget_measure_text_sized(widget, run.text, run.size);
            advances.push_back(advance);
            total += advance;
        }
        const float measured = vgui_widget_measure_text_spans(widget, text, spans.begin(), count, base_size);
        check(total > 0 && total < width && std::fabs(measured - total) < 0.01f,
            "measured width differs from the expected run advances", name);
        if (commands.size() != expected.size()) return;

        float pen = x + (width - total) * align;
        size_t i = 0;
        for (const auto& run : expected) {
            const auto& cmd = commands[i];
            check(cmd.kind == 'T' && cmd.text == run.text, "run bytes or logical order changed", name);
            check(cmd.color == run.color && cmd.font_size == run.size, "span style changed", name);
            check(std::fabs(cmd.x - pen) < 0.01f, "run position disagrees with measurement/alignment", name);
            check(cmd.baseline == commands.front().baseline, "text runs do not share a baseline", name);
            pen += advances[i++];
        }
    };

    // Both endpoints cut into multibyte characters. The two emoji spans
    // normalize to the same range; the last one in the array wins.
    check_line("CJK and supplementary code point", "A\u4E2DB\U0001F642Z",
        {{6, 2, first, 24}, {2, 1, second, 20}, {5, 1, second, 22}},
        {{"A", base, 16}, {"\u4E2D", second, 20}, {"B", base, 16},
         {"\U0001F642", second, 22}, {"Z", base, 16}});

    check_line("base and combining mark in one span", "e\u0301x",
        {{0, 3, first, 20}}, {{"e\u0301", first, 20}, {"x", base, 16}});
    check_line("style boundary inside a combining sequence", "e\u0301x",
        {{0, 1, first, 20}, {2, 1, second, 20}},
        {{"e", first, 20}, {"\u0301", second, 20}, {"x", base, 16}});

    // Split the potential ffi ligature. Do not require a specific ligature
    // glyph or a difference from measuring the entire word in one run.
    check_line("style boundary inside a Latin ligature candidate", "office",
        {{0, 2, first, 16}, {2, 4, second, 16}}, {{"of", first, 16}, {"fice", second, 16}});

    // A style boundary between lam and alef in Arabic, followed by numbers,
    // punctuation and Hebrew. Test the current submission contract, not
    // visual Bidi order or contextual shaping across the style boundary.
    const char* mixed = "A (\u0633\u0644\u0627\u0645 12) \u05E9\u05DC B";
    const std::initializer_list<vgui_text_span> spans = {{16, 4, second, 20}, {3, 4, first, 20}, {7, 4, second, 24}};
    const std::initializer_list<expected_run> runs = {{"A (", base, 16}, {"\u0633\u0644", first, 20},
        {"\u0627\u0645", second, 24}, {" 12) ", base, 16}, {"\u05E9\u05DC", second, 20}, {" B", base, 16}};
    auto* ctx = vgui_widget_get_context(widget);
    const auto previous_direction = vgui_context_get_direction(ctx);
    vgui_context_set_direction(ctx, VGUI_DIRECTION_RTL);
    check_line("mixed Arabic/Hebrew/Latin, RTL physical left", mixed, spans, runs);
    check_line("mixed Arabic/Hebrew/Latin, RTL physical right", mixed, spans, runs, 1.0f);
    vgui_context_set_direction(ctx, previous_direction);
}

void test_wrapped_spans(vgui_widget* widget, ovg_ctx_cb& cb, rvg_t& vg) {
    using span_list = std::initializer_list<vgui_text_span>;
    constexpr float size = 16, x = 10, y = 10, height = 90;
    constexpr vgui_color base = 0xffffffff, accent = 0xffaa33aa;
    const auto near = [](float a, float b) { return std::fabs(a - b) < 0.01f; };
    const auto measure = [&](const char* text, span_list spans = {}) {
        return vgui_widget_measure_text_spans(widget, text, spans.begin(), static_cast<int>(spans.size()), size);
    };
    const auto between = [](const char* name, float fits, float over) {
        check(fits > 0 && fits < over, "fixture must have room between fitting and overflowing advances", name);
        return (fits + over) * 0.5f;
    };
    // Author the expected breaks using LF and rebase byte spans where needed.
    // The original API gives font-dependent dimensions and a command trace,
    // including mixed-size baselines and each row's independent alignment.
    const auto check_wrap = [&](const char* name, const char* text, span_list spans, float limit,
        const char* explicit_lines, span_list line_spans, size_t command_count) {
        const int n = static_cast<int>(spans.size()), line_n = static_cast<int>(line_spans.size());
        const float ref_w = measure(explicit_lines, line_spans);
        const float ref_h = vgui_widget_get_text_height_spans(widget, explicit_lines, line_spans.begin(), line_n, size);
        float w = -1, h = -1;
        vgui_widget_measure_text_spans_wrapped(widget, text, spans.begin(), n, limit, size, &w, &h);
        check(ref_w > 0 && near(w, ref_w), "wrapped width is not the widest expected line advance", name);
        check(ref_h > 0 && near(h, ref_h), "wrapped height is not the sum of the expected line heights", name);

        commands.clear();
        vgui_custom_draw_text_spans(widget, &cb, &vg, explicit_lines, line_spans.begin(), line_n,
            x, y, limit, height, base, 0.5f, 1.0f, size);
        const auto reference = commands;
        check(reference.size() == command_count, "explicit-line reference did not submit every run", name);
        commands.clear();
        vgui_custom_draw_text_spans_wrapped(widget, &cb, &vg, text, spans.begin(), n,
            x, y, limit, height, base, 0.5f, 1.0f, size);
        check(clip == parent_clip, "wrapped draw did not restore the parent clip", name);
        check(commands.size() == reference.size(), "unexpected number of wrapped paint commands", name);
        if (commands.size() != reference.size()) return;
        for (size_t i = 0; i < commands.size(); ++i) {
            const auto& got = commands[i];
            const auto& want = reference[i];
            check(got.kind == want.kind && got.text == want.text, "paint pass or submitted UTF-8 bytes changed", name);
            check(got.color == want.color && got.font_size == want.font_size, "wrapped run lost its style", name);
            check(got.baseline == want.baseline && near(got.x, want.x) && near(got.y, want.y),
                "wrapped line baseline or alignment differs from the explicit-line layout", name);
            if (want.kind == 'D')
                check(near(got.width, want.width) && near(got.height, want.height),
                    "decoration is not confined to its line", name);
        }
    };

    // One span crosses the soft break and ends inside the next word (W|ide).
    // W fits on the first row, but the whole word must move. Only the space
    // and tab at that break disappear; unbroken interior/trailing spaces stay.
    const char* words = "ab \tWide  i \nZ";
    const char* word_lines = "ab\nWide  i \nZ";
    const span_list word_spans = {{0, 5, accent, 24}}, word_line_spans = {{0, 4, accent, 24}};
    const float word_limit = between("word priority", std::max(measure(word_lines, word_line_spans),
        measure("ab \tW", word_spans)), measure("ab \tWide", word_spans));
    check_wrap("word priority across a span boundary", words, word_spans, word_limit, word_lines, word_line_spans, 4);
    commands.clear();
    vgui_custom_draw_text_spans(widget, &cb, &vg, words, word_spans.begin(), 1,
        x, y, word_limit, height, base, 0, 0, size);
    check(measure(words, word_spans) > word_limit && commands.size() == 3 &&
        commands[0].text == "ab \tW" && commands[1].text == "ide  i " && commands[2].text == "Z" &&
        commands[0].baseline == commands[1].baseline && commands[1].baseline < commands[2].baseline,
        "legacy draw overflows on one row and only breaks at LF");

    // No ASCII separator: break after the CJK, not inside the following Latin
    // word. Advance estimates can use the fixture font's missing glyph.
    const char* mixed = "A\u4E2D\u6587iW";
    const char* mixed_lines = "A\u4E2D\u6587\niW";
    const span_list mixed_spans = {{4, 4, accent, 20}}, mixed_line_spans = {{4, 5, accent, 20}};
    const float mixed_limit = between("CJK break", std::max(measure(mixed_lines, mixed_line_spans),
        measure("A\u4E2D\u6587i", mixed_spans)), measure(mixed, mixed_spans));
    check_wrap("CJK to Latin", mixed, mixed_spans, mixed_limit, mixed_lines, mixed_line_spans, 4);

    const char* cluster_text = "e\u0301WW\U0001F642";
    const char* cluster_lines = "e\u0301W\nW\U0001F642";
    check_wrap("long word falls back to fitting graphemes", cluster_text, {},
        between("grapheme fallback", measure(cluster_lines), measure("e\u0301WW")), cluster_lines, {}, 2);
    const float narrow = std::min({measure("e\u0301"), measure("W"), measure("\U0001F642")}) * 0.25f;
    check(narrow > 0, "fixture must give each oversized cluster a positive advance");
    check_wrap("oversized clusters each occupy one row", cluster_text, {}, narrow,
        "e\u0301\nW\nW\n\U0001F642", {}, 4);

    // This span ends between CR and LF; the blank middle and final rows must
    // affect the baseline positions when the whole block is bottom-aligned.
    const char* hard_lines = "X \r\n\n Y\r\n";
    const span_list hard_spans = {{0, 3, accent, 24}};
    check_wrap("LF/CRLF preserve blank rows and adjacent spaces", hard_lines, hard_spans,
        measure("X  Y", hard_spans) + 1, hard_lines, hard_spans, 2);

    auto* ctx = vgui_widget_get_context(widget);
    const float saved_scale = vgui_context_get_ui_scale(ctx);
    vgui_context_set_ui_scale(ctx, 2);
    vgui_text_span effect{};
    effect.length = 5;
    effect.decor = VGUI_TEXT_DECOR_UNDERLINE;
    effect.decor_color = accent;
    effect.decor_thickness = 2;
    effect.glow = {0xff55aa55, 2, 0, 0};
    const float pixel_limit = between("window pixel width at 2x", measure("go"), measure("go go"));
    check_wrap("underline and glow split at a soft break at 2x", "go go", {effect}, pixel_limit,
        "go\ngo", {effect}, 6);
    vgui_context_set_ui_scale(ctx, saved_scale);

    float w = -1, h = -1;
    vgui_widget_measure_text_spans_wrapped(widget, words, word_spans.begin(), 1, word_limit, size, &w, nullptr);
    vgui_widget_measure_text_spans_wrapped(widget, words, word_spans.begin(), 1, word_limit, size, nullptr, &h);
    check(near(w, measure(word_lines, word_line_spans)) && near(h,
        vgui_widget_get_text_height_spans(widget, word_lines, word_line_spans.begin(), 1, size)),
        "wrapped measure outputs are independently optional");
    for (float invalid : {0.0f, -1.0f, std::numeric_limits<float>::quiet_NaN(),
        std::numeric_limits<float>::infinity()}) {
        w = h = -1;
        vgui_widget_measure_text_spans_wrapped(widget, words, word_spans.begin(), 1, invalid, size, &w, &h);
        check(w == 0 && h == 0, "invalid wrap width clears both outputs");
    }
    for (const char* empty : {"", static_cast<const char*>(nullptr)}) {
        w = h = -1;
        vgui_widget_measure_text_spans_wrapped(widget, empty, nullptr, 0, word_limit, size, &w, &h);
        check(w == 0 && h == 0, "empty text clears both wrapped dimensions");
    }
}

void test_inline_images(vgui_widget* widget, ovg_ctx_cb cb, rvg_t& vg) {
    constexpr float x = 10, y = 10, height = 90, size = 16;
    constexpr auto no_wrap = VGUI_RICHTEXT_WRAP_NONE, wrap = VGUI_RICHTEXT_WRAP_UNICODE;
    constexpr vgui_color accent = 0xffaa33aa;
    auto* ctx = vgui_widget_get_context(widget);
    const unsigned char pixels[8] = {255, 255, 255, 255, 255, 255, 255, 255};
    const vgui_texture_desc desc{2, 1, 0, VGUI_TEXTURE_FORMAT_RGBA8, pixels};
    std::unique_ptr<vgui_texture, decltype(&vgui_texture_destroy)> texture(
        vgui_texture_create(ctx, &desc), vgui_texture_destroy);
    check(!!texture, "inline-image fixture texture creation");
    if (!texture) return;

    // Resolve the lazy CPU upload without a GPU; keep the normal VGUI texture
    // path and record its destination rectangle alongside the text commands.
    cb.image_update = [](rvg_t*, vg_image_t* img, vg_image_desc_t*) { img->id = 1; };
    cb.add_image = [](rvg_t*, ovg_image_r* img) {
        commands.push_back({'M', {}, clip, img->dst, 0, (float)img->dst.z,
            (float)img->dst.w, img->color, (float)img->dst.x, (float)img->dst.y});
    };
    const auto only = [](char kind) {
        std::vector<command> selected;
        for (const auto& c : commands) if (c.kind == kind) selected.push_back(c);
        return selected;
    };
    const auto near = [](float a, float b) { return std::fabs(a - b) < 0.01f; };
    const auto advance = [&](const char* text) { return vgui_widget_measure_text_sized(widget, text, size); };
    const auto measure = [&](const vgui_richtext_content& content, float limit, vgui_richtext_wrap_mode mode,
        float want_w, float want_h, const char* name) {
        float w = -1, h = -1;
        vgui_widget_measure_richtext(widget, &content, limit, mode, size, &w, &h);
        check(near(w, want_w) && near(h, want_h), "incorrect measured width or line-box height", name);
    };
    const auto draw = [&](const vgui_richtext_content& content, float limit = 170,
        vgui_richtext_wrap_mode mode = VGUI_RICHTEXT_WRAP_NONE, float align = 0) {
        commands.clear();
        const auto saved_clip = clip;
        vgui_custom_draw_richtext(widget, &cb, &vg, &content, x, y, limit, height, mode, 0xffffffff, align, 0, size);
        check(clip == saved_clip, "inline-image draw did not restore the parent clip");
    };

    // The 32x56 box dominates both font metrics. Its 2:1 texture occupies
    // only 32x16, centered vertically, with the box baseline at top + 42.
    vgui_text_span effect{};
    effect.length = 5;
    effect.color = accent;
    effect.decor = VGUI_TEXT_DECOR_UNDERLINE;
    effect.decor_thickness = 2;
    effect.glow = {accent, 2, 0, VGUI_TEXT_GLOW_DECORATION};
    vgui_richtext_image image{1, nullptr, 32, 56, 14, 0xff123456};
    const vgui_richtext_content mixed{"A" VGUI_RICHTEXT_IMAGE_MARKER "B", &effect, 1, &image, 1};
    const float a = advance("A"), b = advance("B");
    measure(mixed, 1, no_wrap, a + 32 + b, 56, "NULL texture reserves the entire box");
    draw(mixed);
    const auto placeholder = only('T');
    check(only('M').empty(), "NULL texture must not submit a texture command");

    image.texture = texture.get();
    measure(mixed, 1, no_wrap, a + 32 + b, 56, "replacing the texture preserves dimensions");
    vgui_widget_set_opacity(widget, 0.5f);
    draw(mixed);
    vgui_widget_set_opacity(widget, 1);
    const auto text = only('T'), pictures = only('M'), decor = only('D'), glow = only('B');
    check(text.size() == 2 && text[0].text == "A" && text[1].text == "B" &&
        near(text[0].x, x) && near(text[1].x, x + a + 32) &&
        text[0].baseline == y + 42 && text[1].baseline == text[0].baseline &&
        text[0].color == 0x80aa33aa && text[1].color == text[0].color,
        "text on both sides uses the image baseline, box advance and crossing span style");
    check(placeholder.size() == 2 && text.size() == 2 && placeholder[0].box == text[0].box &&
        placeholder[1].box == text[1].box && near(placeholder[1].x, text[1].x) &&
        placeholder[0].baseline == text[0].baseline, "NULL replacement changed surrounding text geometry");
    check(pictures.size() == 1 && pictures[0].box == ivec4((int)std::lround(x + a), y + 20, 32, 16) &&
        pictures[0].color == 0x80563412, "image should be aspect-fitted with its own tint and widget opacity");
    check(decor.size() == 2 && near(decor[0].x, x) && near(decor[0].width, a) &&
        near(decor[1].x, x + a + 32) && near(decor[1].width, b), "crossing underline must stop at the image");
    check(glow.size() == 2 && glow[0].width == 1 && glow[1].width == 1 &&
        glow[0].height == 1 && glow[1].height == 1, "image must separate both glyph and underline glow batches");

    vgui_text_metrics metrics{};
    vgui_widget_get_text_metrics_styled(widget, size, 0, 0, &metrics);
    const float line_h = metrics.line_height, text_ascent = std::trunc(metrics.ascent);
    const float z = advance("Z");
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    // Unsorted offsets, clamped descents at both ends, and a valid duplicate
    // followed by invalid duplicates. No font metrics belong on the image row.
    const vgui_richtext_image records[] = {
        {3, texture.get(), 16, 8, -5, 0}, {0, texture.get(), 50, 25, 0, 0},
        {0, texture.get(), 12, 6, 99, accent}, {0, nullptr, 0, 50, 0, 0},
        {-1, texture.get(), 70, 35, 0, 0}, {1, texture.get(), 70, 35, 0, 0},
        {9, texture.get(), 70, 35, 0, 0}, {99, texture.get(), 70, 35, 0, 0},
        {0, nullptr, nan, 50, 0, 0}, {0, nullptr, 50, inf, 0, 0}, {3, nullptr, 90, 50, nan, 0},
    };
    const vgui_richtext_content pure{VGUI_RICHTEXT_IMAGE_MARKER VGUI_RICHTEXT_IMAGE_MARKER "\r\n\nZ",
        nullptr, 0, records, sizeof(records) / sizeof(records[0])};
    measure(pure, 1, no_wrap, std::max(28.0f, z), 14 + 2 * line_h, "image-only row, CRLF and explicit blank row");
    draw(pure);
    const auto pair = only('M'), last_row = only('T');
    check(pair.size() == 2 && pair[0].box == ivec4(x, y + 8, 12, 6) && pair[0].color == accent &&
        pair[1].box == ivec4(x + 12, y, 16, 8) && pair[1].color == 0xffffffff,
        "only the last valid record at each marker wins, in text order, with clamped descents");
    check(last_row.size() == 1 && last_row[0].text == "Z" && near(last_row[0].y, std::floor(y + 14 + line_h)),
        "image-only and empty rows move the following text by their own metrics");

    const vgui_richtext_content unbound{"A" VGUI_RICHTEXT_IMAGE_MARKER "B", nullptr, 0, nullptr, 1};
    measure(unbound, 1, no_wrap, advance(unbound.text), line_h, "unbound marker uses font metrics");
    draw(unbound);
    check(commands.size() == 1 && commands[0].kind == 'T' && commands[0].text == unbound.text,
        "NULL image array is empty; the unbound marker is passed through to the font");

    // A prefix + image does not fit. The image + 'i' does, but U+FFFC gives
    // a Unicode break before 'iW', so the Latin word moves intact to row 3.
    const char* prefix = "A\u4E2D";
    image = {4, texture.get(), 32, 16, 0, 0};
    const vgui_richtext_content broken{"A\u4E2D" VGUI_RICHTEXT_IMAGE_MARKER "iW", nullptr, 0, &image, 1};
    constexpr float limit = 40;
    const float head = advance(prefix), tail = advance("iW");
    check(head < limit && tail < limit && 32 + advance("i") < limit &&
        limit < std::min(head, tail) + 32, "fixture must prefer a Unicode break to a fitting grapheme");
    const float unwrapped_ascent = std::max(text_ascent, 16.0f);
    measure(broken, limit, no_wrap, head + 32 + tail, unwrapped_ascent + line_h - text_ascent,
        "no-wrap ignores max_width");
    draw(broken, limit, no_wrap);
    const auto unwrapped = only('M');
    check(unwrapped.size() == 1 && unwrapped[0].box.x == std::lround(x + head) &&
        unwrapped[0].box.y + unwrapped[0].box.w == y + unwrapped_ascent,
        "no-wrap draw keeps the image after the prefix on the first row");

    measure(broken, limit, wrap, std::max({head, 32.0f, tail}), 2 * line_h + 16, "Unicode soft wraps around image");
    draw(broken, limit, wrap, 0.5f);
    const auto rows = only('T'), moved = only('M');
    check(rows.size() == 2 && rows[0].text == prefix && rows[1].text == "iW" &&
        near(rows[0].x, x + (limit - head) / 2) && near(rows[1].x, x + (limit - tail) / 2) &&
        near(rows[0].y, y) && near(rows[1].y, std::floor(y + line_h + 16)),
        "Unicode drawing preserves whole runs and uses independently measured row positions");
    check(moved.size() == 1 && moved[0].box == ivec4(x + 4, std::floor(y + line_h), 32, 16),
        "fitting image moves intact and is centered on the second row");

    image.width = 72;
    image.height = 36;
    measure(broken, limit, wrap, 72, 2 * line_h + 36, "oversized image has a full-width row of its own");
    clip = {15, 4, 180, 100};
    draw(broken, limit, wrap);
    const auto wide = only('M'), edges = only('T');
    check(wide.size() == 1 && wide[0].box == ivec4(x, std::floor(y + line_h), 72, 36) &&
        wide[0].clip.x == 15 && wide[0].clip.x + wide[0].clip.z == x + limit,
        "oversized image keeps its full geometry and is clipped by both parent and content");
    check(edges.size() == 2 && edges[0].text == prefix && edges[1].text == "iW" &&
        near(edges[0].y, y) && near(edges[1].y, std::floor(y + line_h + 36)),
        "oversized image row separates both neighboring text rows");
    clip = parent_clip;

    const float saved_scale = vgui_context_get_ui_scale(ctx);
    vgui_context_set_ui_scale(ctx, 2);
    const vgui_richtext_image small[] = {{0, texture.get(), 14, 7, 0, 0}, {3, texture.get(), 14, 7, 0, 0}};
    const vgui_richtext_content dpi{VGUI_RICHTEXT_IMAGE_MARKER VGUI_RICHTEXT_IMAGE_MARKER, nullptr, 0, small, 2};
    measure(dpi, limit, wrap, 28, 28, "2x DPI scales boxes but not the wrap width");
    draw(dpi, limit, wrap);
    const auto scaled = only('M');
    check(commands.size() == 2 && scaled.size() == 2 && scaled[0].box == ivec4(x, y, 28, 14) &&
        scaled[1].box == ivec4(x, y + 14, 28, 14), "2x DPI drawing uses the same window-pixel wrap limit");
    vgui_context_set_ui_scale(ctx, saved_scale);
    commands.clear();
}

void test_button_text_alignment(font_familys_t* font) {
    // Full vgui_draw reads OVG's draw list, so use a real CPU draw context
    // and intercept only text submissions. No GPU or window is needed.
    std::unique_ptr<ovg_ctx_cb, decltype(&free_ctx_cb)> cb(new_ctx_cb(), free_ctx_cb);
    std::unique_ptr<rvg_t, void (*)(rvg_t*)> vg(cb->new_rvg(cb->ac), cb->destroy_rvg);
    std::unique_ptr<vgui_context, decltype(&vgui_destroy)> ui(vgui_create(font, 16), vgui_destroy);
    cb->add_text = record_text;
    cb->image_update = nullptr;
    vg->width = 200;
    vg->height = 120;
    vgui_context_set_size(ui.get(), vg->width, vg->height);

    constexpr char label[] = "Run";
    auto* button = vgui_widget_add(vgui_root(ui.get()), VGUI_BUTTON, label);
    vgui_widget_set_pos(button, 12, 14);
    vgui_widget_set_size(button, 140, 80);
    vgui_widget_set_padding(button, 8, 6, 12, 10);
    constexpr float left = 20, top = 20, width = 120, height = 64; // Padded content area.
    const float text_w = vgui_widget_measure_text(button, label);
    const float text_h = vgui_widget_get_line_height(button);
    const auto check_position = [&](float x, float y, const char* context) {
        cb->clear(vg.get());
        commands.clear();
        vgui_draw(ui.get(), cb.get(), vg.get());
        check(commands.size() == 1 && commands.front().kind == 'T' && commands.front().text == label,
            "expected one button label draw", context);
        if (commands.size() != 1) return;
        const auto& text = commands.front();
        // OVG applies box alignment on top of the submitted origin. Checking
        // the origin alone would miss a second vertical offset on the plain button.
        check(std::fabs(text.x - x) < 0.01f && text.text_align.x == 0,
            "incorrect final horizontal text position", context);
        check(std::fabs(text.y - y) < 0.01f && text.text_align.y == 0,
            "incorrect final vertical text position", context);
    };
    check_position(left + (width - text_w) * 0.5f, top + (height - text_h) * 0.5f,
        "tall plain button at default alignment");

    // The CPU texture reserves an icon slot even without a native texture.
    const unsigned char pixels[] = {255, 255, 255, 255};
    const vgui_texture_desc desc{1, 1, 0, VGUI_TEXTURE_FORMAT_RGBA8, pixels};
    std::unique_ptr<vgui_texture, decltype(&vgui_texture_destroy)> icon(
        vgui_texture_create(ui.get(), &desc), vgui_texture_destroy);
    check(!!icon, "button icon fixture texture creation");
    if (!icon) return;
    constexpr int icon_w = 56, icon_h = 16, gap = 6;
    vgui_button_set_icon(button, icon.get(), icon_w, icon_h, VGUI_BUTTON_ICON_TOP);
    vgui_button_set_icon_gap(button, gap);
    vgui_widget_set_text_align(button, 0.0f, 0.5f);
    check_position(left + (icon_w - text_w) * 0.5f, top + (height - text_h + icon_h + gap) * 0.5f,
        "start-aligned stack with icon above text");
}
} // namespace

int main() {
    std::unique_ptr<font_cache_cx, decltype(&free_font_cache)> cache(new_font_cache(), free_font_cache);
    const auto path = std::string(VGUI_SOURCE_DIR) + "/sample/res/fonts/DejaVuSans.ttf";
    if (!cache || !load_font_from_path(cache.get(), path.c_str(), "RichTextTest")) return 1;
    std::unique_ptr<font_familys_t, decltype(&delete_font_family)> family(
        new_font_family(cache.get(), "RichTextTest", nullptr), delete_font_family);
    if (!family || family->count == 0) return 1;
    std::unique_ptr<vgui_context, decltype(&vgui_destroy)> ui(vgui_create(family.get(), 16), vgui_destroy);
    auto* widget = vgui_widget_add(vgui_root(ui.get()), VGUI_CUSTOM, "");
    if (!widget) return 1;

    ovg_ctx_cb cb{};
    rvg_t vg{};
    cb.get_clip_rect = [](rvg_t*, void* out) { *static_cast<ivec4*>(out) = clip; };
    cb.set_clip_rect = [](rvg_t*, void* rect) { clip = *static_cast<ivec4*>(rect); };
    cb.clip_rect = [](rvg_t*, int x, int y, int w, int h) { clip = {x, y, w, h}; };
    cb.rectangle = [](rvg_t*, float x, float y, float w, float h) {
        decoration = {'D', {}, clip, {}, 0, w, h, 0, x, y};
    };
    cb.set_source_color = [](rvg_t*, uint32_t color) { decoration.color = color; };
    cb.fill = [](rvg_t*) { commands.push_back(decoration); };
    cb.add_text = record_text;
    test_international_spans(widget, cb, vg);
    test_wrapped_spans(widget, cb, vg);
    commands.clear();

    vgui_widget_set_text_glow(widget, 0xff112233, 2, 0);
    vgui_widget_set_opacity(widget, 0.5f);
    vgui_text_span spans[3]{};
    spans[0].length = 5;
    spans[0].decor = VGUI_TEXT_DECOR_UNDERLINE;
    spans[0].decor_color = 0x80123456;
    spans[0].decor_thickness = 3;
    spans[1].start = 2;
    spans[1].length = 1;
    spans[1].decor = VGUI_TEXT_DECOR_STRIKETHROUGH;
    spans[1].glow.flags = VGUI_TEXT_GLOW_OFF;
    spans[2].start = 3;
    spans[2].length = 2;
    spans[2].font_size = 28;
    spans[2].font_weight = VGUI_FONT_WEIGHT_BOLD;
    spans[2].font_slant = VGUI_FONT_SLANT_ITALIC;
    spans[2].glow = {0xff554433, 3, 0, VGUI_TEXT_GLOW_OFF};
    const float width = vgui_widget_measure_text_spans(widget, "ab cd", spans, 3, 16);
    const float expected = vgui_widget_measure_text_sized(widget, "ab ", 16) +
        vgui_widget_measure_text_styled(widget, "cd", 28, VGUI_FONT_WEIGHT_BOLD, VGUI_FONT_SLANT_ITALIC);
    check(std::fabs(width - expected) < 0.01f, "span width uses the styled font");

    vgui_custom_draw_text_spans(widget, &cb, &vg, "ab cd", spans, 3,
        10, 10, 150, 55, 0xffffffff, 0, 0.5f, 16);
    std::string sequence;
    for (const auto& c : commands) sequence += c.kind;
    check(sequence == "GGDTTTD", "glow inheritance/OFF, continuous underline, text, strike order");
    if (sequence == "GGDTTTD") {
        const auto& c = commands;
        check(c[0].text == "ab" && c[1].text == "cd" && c[0].color == 0x80332211 &&
            c[1].color == 0x80334455, "inherited and overridden glow use widget opacity once");
        check(c[0].box == c[3].box && c[1].box == c[5].box &&
            c[3].baseline == c[4].baseline && c[4].baseline == c[5].baseline,
            "both passes share geometry and mixed sizes share a baseline");
        check(std::fabs(c[2].width - width) < 0.01f && c[2].height == 3 &&
            c[2].color == 0x40563412, "underline crosses space and inner strike with source style");
    }
    check(clip == parent_clip, "restore parent clip");

    commands.clear();
    vgui_context_set_ui_scale(ui.get(), 2);
    vgui_custom_draw_text_spans(widget, &cb, &vg, "long overflow run", nullptr, 0,
        10, 10, 9, 5, 0xffffffff, 0, 0, 16);
    check(commands.size() == 2 && commands[0].kind == 'G' && commands[1].kind == 'T' &&
        commands[0].clip.x + commands[0].clip.z == 33 &&
        commands[0].clip.y + commands[0].clip.w == 29 && commands[1].clip == ivec4(10, 10, 9, 5),
        "at 2x DPI text is clipped to the requested rectangle and glow extends by 14 px");
    check(clip == parent_clip, "restore parent clip after overflowing run");

    vgui_context_set_ui_scale(ui.get(), 1);
    vgui_widget_clear_text_glow(widget);
    vgui_widget_set_opacity(widget, 1);
    const auto draw = [&](const char* text, const vgui_text_span* sp, int n, float size = 24) {
        commands.clear();
        vgui_custom_draw_text_spans(widget, &cb, &vg, text, sp, n,
            10, 10, 170, 88, 0xffffffff, 0, 0, size);
    };
    const char* multiline = "Wide\r\nI\n";
    vgui_text_span lines{};
    lines.length = 8;
    lines.decor = VGUI_TEXT_DECOR_UNDERLINE;
    draw(multiline, &lines, 1);
    check(commands.size() == 4 && commands[0].kind == 'D' && commands[1].kind == 'D' &&
        commands[0].y < commands[1].y && commands[2].text == "Wide" && commands[3].text == "I" &&
        commands[2].baseline < commands[3].baseline, "multiline uses one baseline and decoration section per line");
    const float wide = vgui_widget_measure_text_sized(widget, "Wide", 24);
    const float line_height = vgui_widget_get_line_height_sized(widget, 24);
    check(vgui_widget_measure_text_spans(widget, multiline, &lines, 1, 24) == wide &&
        vgui_widget_get_text_height_spans(widget, multiline, &lines, 1, 24) == line_height * 3,
        "multiline uses widest width and counts the trailing blank line in height");

    vgui_text_span styled{};
    styled.length = 5;
    styled.decor = VGUI_TEXT_DECOR_UNDERLINE | VGUI_TEXT_DECOR_STRIKETHROUGH;
    styled.decor_color = 0xff0000ff;
    styled.underline = {VGUI_TEXT_DECOR_PRESENT_COLOR | VGUI_TEXT_DECOR_PRESENT_STYLE,
        0xffff0000, 0, 0, VGUI_TEXT_DECOR_STYLE_DOUBLE, 0};
    draw("g g g", &styled, 1);
    check(commands.size() == 4 && commands[0].color == 0xff0000ff && commands[1].color == 0xff0000ff &&
        commands[0].y < commands[1].y && commands[3].color == 0xffff0000,
        "per-line color and double style do not change the strike");
    styled.decor = VGUI_TEXT_DECOR_UNDERLINE;
    styled.underline = {};
    styled.decor_style = VGUI_TEXT_DECOR_STYLE_DOTTED;
    draw("g g g", &styled, 1);
    const size_t dot_count = commands.size();
    styled.decor_style = VGUI_TEXT_DECOR_STYLE_DASHED;
    draw("g g g", &styled, 1);
    check(commands.size() > 2 && dot_count > commands.size(), "distinct dot and dash patterns");
    cb.move_to = [](rvg_t*, float x, float y) { decoration = {'W', {}, clip, {}, 0, 0, 0, 0, x, y}; };
    cb.line_to = [](rvg_t*, float, float) { decoration.width++; };
    cb.close_path = [](rvg_t*) {};
    styled.decor_style = VGUI_TEXT_DECOR_STYLE_WAVY;
    draw("g g g", &styled, 1);
    check(commands.size() >= 2 && commands[0].kind == 'W' && commands[0].width >= 8,
        "wave emits sampled ribbon geometry");
    styled.decor_style = VGUI_TEXT_DECOR_STYLE_SOLID;
    styled.decor_flags = VGUI_TEXT_DECOR_FLAG_SKIP_INK;
    draw("g g g", &styled, 1, 36);
    float ink_free_width = 0;
    for (const auto& c : commands) if (c.kind == 'D') ink_free_width += c.width;
    check(commands.size() > 2 && ink_free_width > 0 &&
        ink_free_width < vgui_widget_measure_text_sized(widget, "g g g", 36),
        "skip-ink leaves gaps where descenders cross the underline");

    cb.add_text_glow_batch = [](rvg_t*, const vg_text_glow_run_t* runs, size_t n, const vec4*, size_t mask_count) {
        const auto& s = runs[0].style;
        commands.push_back({s.glow_mode == VG_TEXT_GLOW_INNER ? 'I' : 'B', {}, clip, {}, 0,
            (float)n, (float)mask_count, s.color_glow});
        return true;
    };
    vgui_text_glow effect{0xff008aff, 3, 0, VGUI_TEXT_GLOW_DECORATION, 0.4f,
        VGUI_TEXT_GLOW_INNER, VGUI_TEXT_GLOW_ADD};
    vgui_widget_set_text_glow_style(widget, &effect);
    check(vgui_widget_get_text_glow(widget).opacity == 0.4f, "widget preserves glow intensity");
    vgui_text_span adjacent[2]{};
    adjacent[0].length = adjacent[1].length = 2;
    adjacent[1].start = 2;
    adjacent[0].decor = adjacent[1].decor = VGUI_TEXT_DECOR_UNDERLINE;
    draw("abcd", adjacent, 2);
    check(commands.size() == 4 && commands[0].kind == 'D' && commands[1].kind == 'T' &&
        commands[2].kind == 'T' && commands[3].kind == 'I' && commands[3].width == 2 &&
        commands[3].height == 1 && commands[3].clip == ivec4(10, 10, 170, 88),
        "inner overlay batches adjacent runs and the continuous decoration without expanding clip");
    effect.mode = VGUI_TEXT_GLOW_OUTER;
    vgui_widget_set_text_glow_style(widget, &effect);
    adjacent[1].glow = effect;
    adjacent[1].glow.opacity = 0.8f;
    draw("abcd", adjacent, 2);
    check(commands.size() == 5 && commands[0].kind == 'B' && commands[1].kind == 'B' &&
        commands[0].width == 1 && commands[1].width == 1 && clip == parent_clip,
        "distinct segment intensity splits outer-glow batches and restores clip");
    vgui_widget_clear_text_glow(widget);
    test_inline_images(widget, cb, vg);
    test_button_text_alignment(family.get());
    return failures != 0;
}
