// Headless OVG font matching, fallback and cache regression test.
// Optional fixtures: OVG_TEST_BOLD_FONT, OVG_TEST_BOLD_ITALIC_FONT,
// OVG_TEST_VARIABLE_FONT (a wght variable font with lowercase Latin glyphs).
// --bench times warm CPU draw-list generation; --leak-check uses MSVC Debug CRT.
#include "pch.h"
#include <ovg_c.h>
#include <ovg_fonts.h>
#include <hb.h>
#include <hb-ot.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif
#include <fstream>
#include <iterator>
#include <memory>
#include <utility>
#include <vector>

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}
using cache_ptr = std::unique_ptr<font_cache_cx, decltype(&free_font_cache)>;
using fonts_ptr = std::unique_ptr<font_familys_t, decltype(&delete_font_family)>;
fonts_ptr fonts(font_familys_t* f) { return {f, delete_font_family}; }
font_family_t* first(const fonts_ptr& f) { return f && f->count ? f->familys[0] : nullptr; }
std::pair<int, int> style(const font_familys_t* f) {
    int weight = -1, slant = -1;
    font_familys_get_style(f, &weight, &slant);
    return {weight, slant};
}
bool memory_font(font_cache_cx* cache, const char* path, const char* name) {
    std::ifstream file(path, std::ios::binary);
    std::vector<unsigned char> bytes{std::istreambuf_iterator<char>(file), {}};
    return !bytes.empty() && load_font_from_memory(cache, bytes.data(), static_cast<long>(bytes.size()), name);
}
float embolden(font_family_t* f) {
    float x = 0;
    hb_font_get_synthetic_bold(f->font, &x, nullptr, nullptr);
    return x;
}
float width(const font_familys_t* f, int size = 64) { return vg_measure_text_width(f, size, "abca", 4); }
std::vector<uint8_t> raster(font_familys_t* f) {
    std::vector<uint8_t> pixels(256 * 128 * 4);
    text_st_t text{};
    text.pos = {20, 40}; text.size = {220, 100};
    text.text = "abca"; text.text_len = 4;
    text_style_t st{};
    st.family = f; st.fontsize = 48; st.color = 0xffffffff;
    check(vg_render_text_rgba8(&text, &st, nullptr, pixels.data(), 256, 128, 256 * 4, nullptr), "RGBA renders selected face");
    check(std::any_of(pixels.begin(), pixels.end(), [](auto v) { return v != 0; }), "face produces ink");
    return pixels;
}

void test_synthetic(font_cache_cx* cache, const char* regular_path) {
    check(memory_font(cache, regular_path, "OvgTestRegular"), "register regular from memory");
    check(memory_font(cache, regular_path, "OvgTestFallback"), "register independent fallback");
    auto regular = fonts(new_font_family(cache, "OvgTestRegular,OvgTestFallback", nullptr));
    auto default_ex = fonts(new_font_family_ex(cache, "OvgTestRegular,OvgTestFallback", nullptr, 0, 0));
    auto bold = fonts(font_familys_derive(regular.get(), 700, 0));
    auto repeat = fonts(font_familys_derive(regular.get(), 700, 0));
    auto italic = fonts(font_familys_derive(regular.get(), 400, VG_FONT_SLANT_ITALIC));
    auto light = fonts(font_familys_derive(regular.get(), 300, 0));
    check(first(regular) && first(default_ex) == first(regular), "default API keeps the legacy face");
    check(first(bold) && first(italic) && first(bold) == first(repeat), "same request shares a variant");
    if (!first(regular) || !first(bold) || !first(italic)) return;
    check(first(light) == first(regular), "no negative synthesis");
    check(style(regular.get()) == std::pair{400, int(VG_FONT_SLANT_NORMAL)}, "regular effective style");
    check(style(bold.get()) == std::pair{700, int(VG_FONT_SLANT_NORMAL)}, "synthetic bold effective style");
    check(style(italic.get()) == std::pair{400, int(VG_FONT_SLANT_ITALIC)}, "synthetic italic effective style");
    check(bold->count == 2 && bold->familys[1] != regular->familys[1] && bold->familys[1] != bold->familys[0],
        "fallback faces are resolved independently");
    check(embolden(first(bold)) > 0 && embolden(first(regular)) == 0, "base is not emboldened");
    check(hb_font_get_synthetic_slant(first(italic)->font) > 0.20f &&
        hb_font_get_synthetic_slant(first(regular)->font) == 0, "base is not slanted");
    auto bold_italic = fonts(font_familys_derive(regular.get(), 700, VG_FONT_SLANT_ITALIC));
    check(first(bold_italic) && embolden(first(bold_italic)) > 0 &&
        hb_font_get_synthetic_slant(first(bold_italic)->font) > 0.20f, "one face supports both synthetic dimensions");
    check(width(bold.get(), 24) > width(regular.get(), 24) && width(bold.get()) > width(regular.get()) &&
        width(bold.get()) > width(bold.get(), 24), "emboldening adjusts shaped advances at independent sizes");
    auto regular_pixels = raster(regular.get());
    check(raster(bold.get()) != regular_pixels, "synthetic bold changes raster output");
    check(raster(italic.get()) != regular_pixels, "synthetic slant changes raster output");
    check(raster(default_ex.get()) == regular_pixels, "rendering variants does not alter regular pixels");

    const auto m16 = vg_get_font_metrics(regular.get(), 16);
    const auto m32 = vg_get_font_metrics(regular.get(), 32);
    const auto mb32 = vg_get_font_metrics(bold.get(), 32);
    const auto fallback = vg_get_font_metrics(nullptr, 16);
    check(m16.underline_offset > 0 && m16.strikeout_offset < 0 &&
        m16.underline_thickness >= 1 && m16.strikeout_thickness >= 1,
        "deco metrics use the correct baseline direction and minimum thickness");
    check(m32.underline_offset > m16.underline_offset && m32.x_height > m16.x_height &&
        m32.cap_height > m16.cap_height, "deco metrics scale with font size");
    check(mb32.underline_thickness >= 1 && cache->font_metrics_cache.count({bold.get(), 32}) == 1,
        "variant decoration metrics are cached independently");
    check(fallback.underline_thickness >= 1 && fallback.strikeout_offset < 0,
        "decoration metrics have readable fallback values without a face");

    hb_codepoint_t gid = 0;
    auto* hb = first(regular)->font;
    check(hb_font_get_nominal_glyph(hb, 'a', &gid), "fixture covers the test glyph");
    auto r = cache->get_cache_lookup_glyph(hb, gid, 48);
    auto b = cache->get_cache_lookup_glyph(first(bold)->font, gid, 48);
    check(r && b && r != b && r->type == glyph_atlas_entry::RASTER && b->type == glyph_atlas_entry::RASTER &&
        r->path_data != b->path_data, "variant uses distinct atlas and vector cache entries");
    auto another_regular = fonts(font_familys_derive(regular.get(), 0, 0));
    check(first(another_regular) == first(regular) && cache->get_cache_lookup_glyph(hb, gid, 48) == r,
        "reselecting a base face does not change its cache id");

    auto p = bold.get();
    cache->glyph_advance_cache.emplace(vg_measure_glyph_key{p, 64, 'a'}, 1.0f);
    vg_get_font_metrics(p, 64);
    check(cache->text_width_cache.count({p, 64, "abca"}) == 1 && cache->font_metrics_cache.count({p, 64}) == 1,
        "populate measurement cache for the derived list");
    bold.reset();
    check(!cache->text_width_cache.count({p, 64, "abca"}) && !cache->font_metrics_cache.count({p, 64}) &&
        !cache->glyph_advance_cache.count({p, 64, 'a'}), "deletion removes measurement entries keyed by the list");
    check(cache->text_width_cache.count({regular.get(), 64, "abca"}) == 1, "deletion does not evict another list's measurements");
    font_familys_t foreign{};
    check(font_familys_derive(&foreign, 700, 0) == nullptr, "cannot derive a caller-owned list");
    delete_font_family(&foreign);
    check(style(&foreign) == std::pair{0, 0}, "unknown list style is zero");
    cache->clear_load();
    check(cache->glyph_cache.empty() && cache->_sub_glyph_cache.empty() && cache->image_cache._data.empty() &&
        cache->text_width_cache.empty() && cache->font_metrics_cache.empty() && cache->glyph_advance_cache.empty(),
        "explicit reset clears glyph, atlas and measurement caches");
    check(!regular->count && !regular->familys, "explicit reset invalidates borrowed faces safely");
}

void test_cache_lifetime(font_cache_cx* cache, const char* regular_path) {
    constexpr auto alias = "OvgTestLifetime";
    constexpr int weight = VG_FONT_WEIGHT_BOLD;
    constexpr int slant = VG_FONT_SLANT_ITALIC;
    std::vector<uint8_t> expected_pixels;
    float expected_width = 0;
    for (int cycle = 0; cycle < 16; ++cycle) {
        if (!memory_font(cache, regular_path, alias)) {
            check(false, "re-register the fixture after a font cache reset");
            return;
        }
        auto base = fonts(new_font_family(cache, alias, nullptr));
        auto variant = fonts(font_familys_derive(base.get(), weight, slant));
        auto* face = first(variant);
        if (!face) {
            check(false, "resolve a new variant after a font cache reset");
            return;
        }
        const auto next_id = cache->next_font_id;
        for (int i = 0; i < 64; ++i) {
            auto derived = fonts(font_familys_derive(base.get(), weight, slant));
            check(first(derived) == face, "short-lived family lists share the same variant face");
        }
        check(cache->next_font_id == next_id, "repeated requests do not allocate font ids");

        hb_codepoint_t glyph = 0;
        check(hb_font_get_nominal_glyph(face->font, 'a', &glyph) &&
            cache->get_cache_lookup_glyph(face->font, glyph, 48), "populate the variant glyph cache");
        const auto pixels = raster(variant.get());
        const float advance = width(variant.get());
        if (cycle == 0) {
            expected_width = advance;
            expected_pixels = pixels;
        }
        check(advance > 0 && advance == expected_width && pixels == expected_pixels,
            "advances and pixels remain stable across cache resets");

        // An explicit legacy subfamily bypasses memory aliases and resolves
        // the disk fixture registered with fontconfig in main().
        auto system = fonts(new_font_family(cache, "DejaVu Sans", "Book"));
        check(first(system) != nullptr, "resolve the fixture via fontconfig");
        if (cycle % 2 == 0)
            cache->clear_load();
        else
            cache->clear_sys();
        for (const auto* list : {base.get(), variant.get(), system.get()})
            check(list && !list->count && !list->familys, "reset invalidates all live lists without destroying them");
        check(cache->glyph_cache.empty() && cache->_sub_glyph_cache.empty() && cache->image_cache._data.empty() &&
            cache->text_width_cache.empty(), "reset clears rendered glyphs and measured advances");
        if (cycle % 2 != 0) {
            auto missing_system = fonts(new_font_family(cache, "DejaVu Sans", "Book"));
            check(!first(missing_system), "system face is unavailable until fontconfig is re-enumerated");
            // clear_sys leaves memory registrations intact. Resolve a fresh
            // variant through the invalidated list, then clear the registrations
            // before the next cycle to exercise that rebuild too.
            auto rebuilt = fonts(font_familys_derive(base.get(), weight, slant));
            check(first(rebuilt) && raster(rebuilt.get()) == expected_pixels,
                "derive from an invalidated list using the retained memory face");
            get_sys_family(cache);
            cache->clear_load();
        }
    }
}

void test_static(font_cache_cx* cache, const char* regular_path) {
    const auto* bold_path = std::getenv("OVG_TEST_BOLD_FONT");
    if (!bold_path) { std::puts("SKIP: optional static face fixtures"); return; }
    check(memory_font(cache, regular_path, "OvgTestStatic"), "register regular static face");
    check(memory_font(cache, bold_path, "OvgTestStatic"), "register bold static face");
    auto bold = fonts(new_font_family_ex(cache, "OvgTestStatic", nullptr, 700, VG_FONT_SLANT_NORMAL));
    auto explicit_bold = fonts(new_font_family_ex(cache, "OvgTestStatic", "Bold", 400, VG_FONT_SLANT_NORMAL));
    auto missing_style = fonts(new_font_family_ex(cache, "OvgTestStatic", "NotASubfamily", 700, VG_FONT_SLANT_NORMAL));
    check(first(bold) && style(bold.get()).first == 700 && embolden(first(bold)) == 0, "real bold beats synthesis");
    check(first(bold) && first(explicit_bold) == first(bold), "exact subfamily outranks weight distance");
    check(first(bold) && first(missing_style) == first(bold), "missing subfamily falls back to weight matching");
    if (const auto* bold_italic = std::getenv("OVG_TEST_BOLD_ITALIC_FONT")) {
        check(memory_font(cache, bold_italic, "OvgTestStatic"), "register bold italic face");
        auto bi = fonts(new_font_family_ex(cache, "OvgTestStatic", nullptr, 700, VG_FONT_SLANT_ITALIC));
        check(first(bi) && style(bi.get()) == std::pair{700, int(VG_FONT_SLANT_ITALIC)} &&
            embolden(first(bi)) == 0 && hb_font_get_synthetic_slant(first(bi)->font) == 0, "real bold italic is not synthesized");
    }
}

void test_variable(font_cache_cx* cache) {
    const auto* path = std::getenv("OVG_TEST_VARIABLE_FONT");
    if (!path) { std::puts("SKIP: optional variable face fixture"); return; }
    check(memory_font(cache, path, "OvgTestVariable"), "register variable font");
    auto base = fonts(new_font_family(cache, "OvgTestVariable", nullptr));
    if (!first(base)) { check(false, "resolve variable default"); return; }
    const auto initial = raster(base.get());
    auto* base_hb = first(base)->font;
    auto* face = hb_font_get_face(base_hb);
    hb_ot_var_axis_info_t wght{};
    check(hb_ot_var_find_axis_info(face, HB_TAG('w','g','h','t'), &wght), "fixture has a weight axis");
    std::vector<uint8_t> previous;
    for (auto w : {300, 400, 700}) {
        auto variant = fonts(font_familys_derive(base.get(), w, 0));
        auto repeat = fonts(font_familys_derive(base.get(), w, 0));
        check(first(variant) && first(variant) == first(repeat), "variable requests are cached");
        if (!first(variant)) continue;
        unsigned count = 0;
        const float* coords = hb_font_get_var_coords_design(first(variant)->font, &count);
        const auto target = std::clamp(float(w), wght.min_value, wght.max_value);
        check((count > wght.axis_index ? coords[wght.axis_index] : wght.default_value) == target &&
            embolden(first(variant)) == 0, "use the clamped weight axis, not synthetic bold");
        auto pixels = raster(variant.get());
        if (!previous.empty()) check(pixels != previous, "adjacent test weights produce different glyphs");
        previous = std::move(pixels);
    }
    check(raster(base.get()) == initial, "variations do not mutate default face");
    auto italic = fonts(font_familys_derive(base.get(), 0, VG_FONT_SLANT_ITALIC));
    if (!first(italic)) { check(false, "resolve italic variable face"); return; }
    for (auto [tag, value] : {std::pair{HB_TAG('s','l','n','t'), -12.0f}, std::pair{HB_TAG('i','t','a','l'), 1.0f}}) {
        hb_ot_var_axis_info_t axis{};
        if (!hb_ot_var_find_axis_info(face, tag, &axis)) continue;
        unsigned count = 0;
        auto coords = hb_font_get_var_coords_design(first(italic)->font, &count);
        check((count > axis.axis_index ? coords[axis.axis_index] : axis.default_value) ==
            std::clamp(value, axis.min_value, axis.max_value), "apply real slnt/ital coordinates");
    }
}
void benchmark_text(font_cache_cx* cache, const char* regular_path) {
    if (!memory_font(cache, regular_path, "OvgTestBench")) {
        check(false, "register benchmark font");
        return;
    }
    auto regular = fonts(new_font_family(cache, "OvgTestBench", nullptr));
    auto bold = fonts(font_familys_derive(regular.get(), VG_FONT_WEIGHT_BOLD, 0));
    auto italic = fonts(font_familys_derive(regular.get(), 0, VG_FONT_SLANT_ITALIC));
    auto both = fonts(font_familys_derive(regular.get(), VG_FONT_WEIGHT_BOLD, VG_FONT_SLANT_ITALIC));
    if (!first(regular) || !first(bold) || !first(italic) || !first(both) ||
        embolden(first(bold)) <= 0 || hb_font_get_synthetic_slant(first(italic)->font) <= 0 ||
        embolden(first(both)) <= 0 || hb_font_get_synthetic_slant(first(both)->font) <= 0) {
        check(false, "benchmark needs regular and synthetic faces from one fixture");
        return;
    }

    std::unique_ptr<ovg_ctx_cb, decltype(&free_ctx_cb)> cb(new_ctx_cb(), free_ctx_cb);
    std::unique_ptr<rvg_t, void (*)(rvg_t*)> vg(cb->new_rvg(cb->ac), cb->destroy_rvg);
    vg->width = 960;
    vg->height = 880;
    constexpr int runs = 24, warmup = 32, batches = 9, frames = 256;
    constexpr char line[] = "The quick brown fox jumps over the lazy dog.";
    const std::array families{regular.get(), bold.get(), italic.get(), both.get()};
    constexpr std::array names{"Regular", "Synthetic bold", "Synthetic italic", "Synthetic bold italic"};
    std::array<std::array<double, batches>, names.size()> samples{};
    auto draw_frame = [&](font_familys_t* family) {
        cb->clear(vg.get());
        text_st_t text{};
        text.text = line;
        text.text_len = sizeof(line) - 1;
        text.size = {920, 32};
        text_style_t st{};
        st.family = family;
        st.fontsize = 24;
        st.color = 0xffffffffu;
        for (int row = 0; row < runs; ++row) {
            text.pos = {20.0f, 12.0f + row * 36.0f};
            cb->add_text(vg.get(), &text, &st, nullptr);
        }
        return get_draw_list(vg.get());
    };
    size_t vertices = 0, indices = 0;
    for (auto* family : families) {
        for (int i = 0; i < warmup; ++i) draw_frame(family);
        const auto dl = get_draw_list(vg.get());
        if (family == regular.get()) { vertices = dl.v1_count; indices = dl.ig_count; }
        check(dl.count && vertices && indices && !dl.v_count &&
            dl.v1_count == vertices && dl.ig_count == indices,
            "all benchmark styles generate the same amount of raster geometry");
    }
    if (failures) return;

    const auto next_id = cache->next_font_id;
    using clock = std::chrono::steady_clock;
    for (int batch = 0; batch < batches; ++batch) {
        // Rotate order so temperature or background load does not always bias one style.
        for (size_t i = 0; i < families.size(); ++i) {
            const size_t style_index = (i + batch) % families.size();
            size_t processed = 0;
            const auto start = clock::now();
            for (int frame = 0; frame < frames; ++frame)
                processed += draw_frame(families[style_index]).v1_count;
            samples[style_index][batch] = std::chrono::duration<double, std::micro>(
                clock::now() - start).count() / frames;
            check(processed == frames * vertices, "each timed frame generates text geometry");
        }
    }
    check(cache->next_font_id == next_id, "warm drawing does not allocate font ids");
    std::printf("OVG CPU: %d runs/frame at 24 px, median of %d x %d frames (no GPU)\n", runs, batches, frames);
    for (auto& values : samples) std::sort(values.begin(), values.end());
    for (size_t i = 0; i < families.size(); ++i) {
        const double median = samples[i][batches / 2];
        std::printf("%-22s %8.2f us/frame  %6.3fx regular\n", names[i], median, median / samples[0][batches / 2]);
    }
}

#if defined(_MSC_VER) && defined(_DEBUG)
void check_heap_lifetime(const char* regular_path) {
    const int flags = _CrtSetDbgFlag(_CRTDBG_REPORT_FLAG);
    if (!(flags & _CRTDBG_ALLOC_MEM_DF)) {
        check(false, "Debug CRT allocation tracking must be enabled");
        return;
    }
    auto exercise = [&] {
        cache_ptr cache(new_font_cache(), free_font_cache);
        if (!cache || !load_font_from_path(cache.get(), regular_path, nullptr)) {
            check(false, "register disk fixture for heap lifetime check");
            return;
        }
        get_sys_family(cache.get());
        test_synthetic(cache.get(), regular_path);
        test_cache_lifetime(cache.get(), regular_path);
    };
    // Run once outside the checkpoints to initialize process-lifetime caches.
    exercise();
    if (failures) return;
    _CrtMemState before{}, after{};
    _CrtMemCheckpoint(&before);
    constexpr int passes = 3;
    for (int pass = 0; pass < passes; ++pass) exercise();
    _CrtMemCheckpoint(&after);
    for (int type : {_NORMAL_BLOCK, _CLIENT_BLOCK}) {
        const auto blocks = static_cast<long long>(after.lCounts[type]) - before.lCounts[type];
        const auto bytes = static_cast<long long>(after.lSizes[type]) - before.lSizes[type];
        std::printf("Debug CRT heap (block type %d): %+lld blocks, %+lld bytes after %d passes\n",
            type, blocks, bytes, passes);
        check(blocks == 0 && bytes == 0, "no live Debug CRT block or byte growth after font teardown");
    }
    if (failures) _CrtMemDumpAllObjectsSince(&before);
}
#endif
} // namespace

int main(int argc, char** argv) {
    const char* regular_path = VGUI_SOURCE_DIR "/sample/res/fonts/DejaVuSans.ttf";
    if (argc == 2 && std::strcmp(argv[1], "--leak-check") == 0) {
#if defined(_MSC_VER) && defined(_DEBUG)
        check_heap_lifetime(regular_path);
        return failures ? 1 : 0;
#else
        std::puts("SKIP: --leak-check requires the MSVC Debug CRT");
        return 77;
#endif
    }
    const bool bench = argc == 2 && std::strcmp(argv[1], "--bench") == 0;
    if (argc > 1 && !bench) {
        std::fprintf(stderr, "Usage: %s [--bench | --leak-check]\n", argv[0]);
        return 2;
    }
    cache_ptr cache(new_font_cache(), free_font_cache);
    if (!cache) return 1;
    if (bench) {
        benchmark_text(cache.get(), regular_path);
        return failures ? 1 : 0;
    }
    check(load_font_from_path(cache.get(), regular_path, nullptr), "register system font fixture");
    get_sys_family(cache.get());
    const auto next_id = cache->next_font_id;
    get_sys_family(cache.get());
    check(cache->next_font_id == next_id, "fontconfig refresh does not insert faces twice");
    test_synthetic(cache.get(), regular_path);
    test_cache_lifetime(cache.get(), regular_path);
    test_static(cache.get(), regular_path);
    test_variable(cache.get());
    std::printf("OVG font tests: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
