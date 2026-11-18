// Headless command-level coverage for styled rich-text layout and paint.
#include <glm/glm.hpp>
#include <ovg_c.h>
#include <vgui/richtext.h>

#include <cmath>
#include <cstdio>
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
};
constexpr ivec4 parent_clip{5, 4, 180, 100};
ivec4 clip = parent_clip;
std::vector<command> commands;
command decoration;
int failures = 0;

void check(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}
}

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
            st->color_glow ? st->color_glow : st->color});
    };

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
