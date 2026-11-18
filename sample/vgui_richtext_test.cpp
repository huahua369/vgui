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
    cb.add_text = [](rvg_t*, text_st_t* t, text_style_t* st, text_box_rt* box) {
        commands.push_back({st->color_glow ? 'G' : 'T', std::string(t->text, t->text_len),
            clip, box->rc, box->rc.y + box->baseline, 0, 0,
            st->color_glow ? st->color_glow : st->color, t->pos.x, t->pos.y, st->fontsize});
    };
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
    return failures != 0;
}
