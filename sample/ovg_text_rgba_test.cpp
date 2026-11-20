// CPU-only regression tests for the caller-owned RGBA text renderer.
#include "pch.h" // ovg_c.h uses glm types
#include <ovg_c.h>
#include <ovg_fonts.h>
#include <hb-raster.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#ifdef _WIN32
#include <cstdlib>
#include <filesystem>
#endif

static int failures = 0;
static void check(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}

using layout_ptr = std::unique_ptr<vg_text_rgba_layout, decltype(&vg_free_text_rgba_layout)>;

static void test_small_glyph(font_familys_t* family, const char* name, char ch, int size) {
    constexpr int w = 32, h = 32, pen_x = 8, pen_y = 4;
    const char str[] = {ch, '\0'};
    text_st_t text{};
    text.pos = {pen_x, pen_y}; text.size = {w, h}; text.text = str; text.text_len = 1;
    text_style_t style{};
    style.family = family; style.fontsize = (float)size; style.color = 0xffffffff;
    text_box_rt box{};
    box.baseline = 12;
    std::array<uint8_t, w * h * 4> pixels{};
    if (!vg_render_text_rgba8(&text, &style, &box, pixels.data(), w, h, w * 4, nullptr)) {
        check(false, "render small grayscale glyph");
        return;
    }

    // Render the same face directly, with no OVG atlas or manual raster bounds.
    std::unique_ptr<hb_font_t, decltype(&hb_font_destroy)> font(
        hb_font_create(hb_font_get_face(family->familys[0]->font)), hb_font_destroy);
    std::unique_ptr<hb_raster_draw_t, decltype(&hb_raster_draw_destroy)> draw(
        hb_raster_draw_create_or_fail(), hb_raster_draw_destroy);
    hb_font_set_scale(font.get(), size, size);
    hb_codepoint_t gid = 0;
    if (!draw || !hb_font_get_nominal_glyph(font.get(), (unsigned char)ch, &gid)) {
        check(false, "resolve small-glyph HarfBuzz reference");
        return;
    }
    hb_raster_draw_set_transform(draw.get(), 1, 0, 0, -1, 0, 0);
    hb_raster_draw_glyph(draw.get(), font.get(), gid);
    std::unique_ptr<hb_raster_image_t, decltype(&hb_raster_image_destroy)> image(
        hb_raster_draw_render(draw.get()), hb_raster_image_destroy);
    if (!image || hb_raster_image_get_format(image.get()) != HB_RASTER_FORMAT_A8) {
        check(false, "rasterize small-glyph A8 reference");
        return;
    }
    hb_raster_extents_t ext{};
    hb_raster_image_get_extents(image.get(), &ext);
    const auto* a8 = hb_raster_image_get_buffer(image.get());
    const int left = pen_x + ext.x_origin, top = pen_y + box.baseline + ext.y_origin;
    if (!a8 || left < 0 || top < 0 || left + (int)ext.width > w || top + (int)ext.height > h) {
        check(false, "small-glyph reference fits entirely in the destination");
        return;
    }
    std::array<uint8_t, w * h> expected{};
    for (unsigned y = 0; y < ext.height; ++y)
        std::memcpy(expected.data() + (top + y) * w + left, a8 + y * ext.stride, ext.width);

    int actual_sum = 0, expected_sum = 0, mismatches = 0;
    for (size_t i = 0; i < expected.size(); ++i) {
        const uint8_t alpha = pixels[4 * i + 3];
        actual_sum += alpha;
        expected_sum += expected[i];
        mismatches += alpha != expected[i];
    }
    if (!expected_sum || mismatches) {
        std::fprintf(stderr, "FAIL: %s %dpx '%c' coverage: OVG %d, HarfBuzz %d, %d differing alpha pixels\n",
            name, size, ch, actual_sum, expected_sum, mismatches);
        ++failures;
    }
}

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

    // In the old manual bounds, even the bundled 12px period loses its bottom row.
    for (int size : {12, 13, 14})
        for (char ch : {'.', ',', 'g'}) test_small_glyph(family, "DejaVu Sans", ch, size);
#ifdef _WIN32
    if (const char* root = std::getenv("SystemRoot")) {
        const auto yahei_path = std::filesystem::path(root) / "Fonts" / "msyh.ttc";
        std::error_code ec;
        if (std::filesystem::is_regular_file(yahei_path, ec)) {
            // A named file registration selects face zero (Microsoft YaHei Regular).
            const bool registered = load_font_from_path(cache, yahei_path.generic_string().c_str(), "RgbaTestYaHei");
            std::unique_ptr<font_familys_t, decltype(&delete_font_family)> yahei(
                registered ? new_font_family(cache, "RgbaTestYaHei", nullptr) : nullptr, delete_font_family);
            check(yahei && yahei->count == 1, "resolve installed Microsoft YaHei Regular");
            if (yahei && yahei->count == 1) test_small_glyph(yahei.get(), "Microsoft YaHei", '.', 13);
        }
    }
#endif

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
