// CPU-only regression tests for the caller-owned RGBA text renderer.
#include "pch.h" // ovg_c.h uses glm types
#include <ovg_c.h>
#include <ovg_fonts.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

static int failures = 0;
static void check(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}

using layout_ptr = std::unique_ptr<vg_text_rgba_layout, decltype(&vg_free_text_rgba_layout)>;

static void test_layout(font_cache_cx* cache, const char* path) {
    constexpr int w = 192, h = 128, stride = w * 4 + 13, dx = -8, dy = 7;
    constexpr uint32_t tint = 0x8000ff00;
    ivec4 clip = {35, 20, 40, 90};
    std::vector<uint8_t> base(stride * h, 0), moved(stride * h, 0);
    layout_ptr layout(nullptr, vg_free_text_rgba_layout);
    if (!load_font_from_path(cache, path, "RgbaLayoutDejaVu")) {
        check(false, "register layout fixture");
        free_font_cache(cache);
        return;
    }
    {
        std::unique_ptr<font_familys_t, decltype(&delete_font_family)> family(
            new_font_family(cache, "RgbaLayoutDejaVu", nullptr), delete_font_family);
        std::string input = "AV\nglyph";
        text_st_t text{};
        text.pos = {12, 3}; text.text = input.c_str(); text.text_len = (int)input.size();
        text_style_t style{};
        style.family = family.get(); style.fontsize = 24; style.color = 0xff0000ff;
        text_box_rt box{};
        box.rc = {0, 0, 140, 100}; box.baseline = 24; box.text_align = {0.5f, 0.5f};
        layout.reset(vg_create_text_rgba_layout(&text, &style, &box));
        check(layout != nullptr, "prepare aligned multiline layout");
        check(vg_render_text_rgba8(&text, &style, &box, base.data(), w, h, stride, nullptr),
            "render one-shot reference");
        text.pos.x += dx; text.pos.y += dy; style.color = tint;
        check(vg_render_text_rgba8(&text, &style, &box, moved.data(), w, h, stride, &clip),
            "render translated and clipped one-shot reference");
    }
    // Neither the input string and descriptors nor the family's handle need to
    // survive creation. Leave only the layout holding a font cache reference.
    if (layout) check(cache->references == 2, "prepared layout retains exactly one cache reference");
    free_font_cache(cache);
    if (!layout) return;
    cache->min_subpixel = 99; // shape_segment would set this back to zero.
    std::vector<uint8_t> pixels(stride * h, 0);
    check(vg_render_text_layout_rgba8(layout.get(), 0, 0, 0xff0000ff, pixels.data(), w, h, stride, nullptr) &&
        pixels == base && std::any_of(base.begin(), base.end(), [](auto b) { return b != 0; }),
        "cached replay equals one-shot pixels after releasing input, family and cache handles");
    std::fill(pixels.begin(), pixels.end(), 0);
    check(vg_render_text_layout_rgba8(layout.get(), dx, dy, tint, pixels.data(), w, h, stride, &clip) &&
        pixels == moved && std::any_of(moved.begin(), moved.end(), [](auto b) { return b != 0; }),
        "reuse prepared layout with a different origin, tint and clip");
    check(cache->min_subpixel == 99, "replay does not shape the input again");
    const auto saved = pixels;
    check(!vg_render_text_layout_rgba8(layout.get(), dx, dy, tint, pixels.data(), w, h, w * 4 - 1, &clip) &&
        pixels == saved, "reject short stride without writes");
    cache->clear_load();
    check(!vg_render_text_layout_rgba8(layout.get(), 0, 0, tint, pixels.data(), w, h, stride, nullptr) &&
        pixels == saved, "reject layout whose font cache has been cleared, without writes");
}

int main() {
    font_cache_cx* cache = new_font_cache();
    check(cache != nullptr, "font cache");
    if (!cache) return 1;
    const std::string path = std::string(VGUI_SOURCE_DIR) + "/sample/res/fonts/DejaVuSans.ttf";
    const bool loaded = load_font_from_path(cache, path.c_str(), "RgbaTestDejaVu");
    check(loaded, "load bundled font");
    font_familys_t* family = loaded ? new_font_family(cache, "RgbaTestDejaVu", nullptr) : nullptr;
    check(family && family->count > 0, "resolve bundled font");
    if (!family || family->count <= 0) { if (family) delete_font_family(family); free_font_cache(cache); return 1; }

    const int w = 192, h = 128, stride = w * 4 + 13;
    text_st_t text{};
    text.pos = { 8, 40 }; text.size = { 180, 110 };
    text.text = "Hello"; text.text_len = 5;
    text_style_t style{};
    style.family = family; style.fontsize = 32; style.color = 0xff0000ff;
    auto render = [&](std::vector<uint8_t>& dst, const ivec4* clip = nullptr) {
        return vg_render_text_rgba8(&text, &style, nullptr, dst.data(), w, h, stride, clip);
    };
    std::vector<uint8_t> red(stride * h, 0);
    check(render(red), "opaque red render succeeds");
    int ink = 0;
    bool red_valid = true;
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
        const uint8_t* p = &red[y * stride + x * 4];
        if (p[3]) ++ink;
        if (p[0] != p[3] || p[1] || p[2]) red_valid = false;
    }
    check(ink > 0, "opaque text produces visible pixels");
    check(red_valid, "red channel order and premultiplied alpha");

    style.color = 0x8000ff00;
    std::vector<uint8_t> green(stride * h, 0);
    check(render(green), "translucent green render succeeds");
    bool green_valid = true, alpha_valid = true; int green_ink = 0;
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
        const uint8_t* p = &green[y * stride + x * 4];
        const int opaque_alpha = red[y * stride + x * 4 + 3];
        if (std::abs(int(p[3]) - (opaque_alpha * 128 + 127) / 255) > 1) alpha_valid = false;
        if (p[3]) ++green_ink;
        if (p[0] || p[2] || p[1] != p[3] || p[3] > 128) green_valid = false;
    }
    check(green_ink > 0 && green_valid, "translucent green is premultiplied and alpha-limited");
    check(alpha_valid, "source alpha scales glyph coverage");
    style.color = 0x00ffffff;
    std::vector<uint8_t> transparent(stride * h, 0);
    check(render(transparent), "transparent render succeeds");
    check(std::all_of(transparent.begin(), transparent.end(), [](uint8_t v) { return v == 0; }), "zero alpha leaves destination unchanged");

    style.color = 0x8000ff00;
    std::vector<uint8_t> composed(stride * h, 0);
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
        uint8_t* p = &composed[y * stride + x * 4]; p[0] = 40; p[1] = 20; p[2] = 10; p[3] = 100;
    }
    check(render(composed), "source-over render succeeds");
    bool blend_ok = true;
    const int bg[4] = {40, 20, 10, 100};
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
        const uint8_t* s = &green[y * stride + x * 4];
        const uint8_t* d = &composed[y * stride + x * 4];
        for (int c = 0; c < 4; ++c) {
            const int expected = s[c] + (bg[c] * (255 - s[3]) + 127) / 255;
            if (std::abs(int(d[c]) - expected) > 2) blend_ok = false;
        }
    }
    check(blend_ok, "premultiplied source-over existing destination");

    style.color = 0xff0000ff;
    std::vector<uint8_t> clipped(stride * h, 0);
    for (int y = 0; y < h; ++y) std::fill(clipped.begin() + y * stride + w * 4, clipped.begin() + (y + 1) * stride, 0xa5);
    ivec4 clip = { 20, 0, 25, h };
    check(render(clipped, &clip), "clipped render succeeds");
    bool clip_ok = true, padding_ok = true; int clip_ink = 0, excluded_ink = 0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) for (int c = 0; c < 4; ++c) {
            const bool inside = x >= 20 && x < 45;
            const uint8_t value = clipped[y * stride + 4 * x + c];
            if (value != (inside ? red[y * stride + 4 * x + c] : 0)) clip_ok = false;
            if (c == 3 && inside && value) ++clip_ink;
            if (c == 3 && !inside && red[y * stride + 4 * x + c]) ++excluded_ink;
        }
        for (int i = w * 4; i < stride; ++i) if (clipped[y * stride + i] != 0xa5) padding_ok = false;
    }
    check(clip_ok && clip_ink && excluded_ink, "clip keeps only intersecting pixels");
    check(padding_ok, "stride padding remains untouched");

    text.text = "A\nA"; text.text_len = 3;
    std::vector<uint8_t> multi(stride * h, 0), single(stride * h, 0);
    check(render(multi), "multiline render succeeds");
    text.text = "A"; text.text_len = 1;
    check(render(single), "single line render succeeds");
    int added = 0;
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x)
        if (multi[y * stride + 4*x + 3] && !single[y * stride + 4*x + 3]) ++added;
    check(added > 0, "second line adds visible pixels");

    check(!vg_render_text_rgba8(nullptr, &style, nullptr, red.data(), w, h, stride, nullptr), "reject null text");
    check(!vg_render_text_rgba8(&text, nullptr, nullptr, red.data(), w, h, stride, nullptr), "reject null style");
    check(!vg_render_text_rgba8(&text, &style, nullptr, nullptr, w, h, stride, nullptr), "reject null pixels");
    check(!vg_render_text_rgba8(&text, &style, nullptr, red.data(), 0, h, stride, nullptr), "reject zero width");
    check(!vg_render_text_rgba8(&text, &style, nullptr, red.data(), w, 0, stride, nullptr), "reject zero height");
    check(!vg_render_text_rgba8(&text, &style, nullptr, red.data(), w, h, w * 4 - 1, nullptr), "reject short stride");
    style.fontsize = 4097;
    check(!render(red), "reject font size over 4096");

    style.fontsize = 320; text.pos = {0, 360}; text.size = {512, 512};
    std::vector<uint8_t> large(512 * 512 * 4, 0);
    check(vg_render_text_rgba8(&text, &style, nullptr, large.data(), 512, 512, 512 * 4, nullptr), "large vector glyph render succeeds");
    int large_ink = 0;
    for (size_t i = 3; i < large.size(); i += 4) if (large[i]) ++large_ink;
    check(large_ink > 100, "large glyph produces visible pixels");
    layout_ptr large_layout(vg_create_text_rgba_layout(&text, &style, nullptr), vg_free_text_rgba_layout);
    std::vector<uint8_t> large_replay(large.size(), 0);
    check(large_layout && vg_render_text_layout_rgba8(large_layout.get(), 0, 0, style.color,
        large_replay.data(), 512, 512, 512 * 4, nullptr) && large_replay == large,
        "prepared vector glyphs match one-shot rasterization");
    large_layout.reset();
    delete_font_family(family);
    test_layout(cache, path.c_str()); // takes ownership of the original cache reference
    std::printf("RGBA text tests: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
