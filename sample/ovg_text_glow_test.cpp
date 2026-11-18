// Standalone G1 regression. Default mode needs no GPU; --gpu exercises the
// SDL3 OVG renderer, and --bench compares 10/50 labels with and without glow.
// Both GPU modes return 77 if no suitable Vulkan device is available.
#include <glm/glm.hpp>
#include "ovg.h"
#include "ovg_fonts.h"
#ifndef VGUI_CORE_ONLY
#include "ovg_renderer_sdl3.h"
#include "gpu_test_util.h"
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {
constexpr int width = 640, height = 160;
constexpr uint32_t red = 0xff0000ffu, green = 0xff00ff00u;
int failures = 0;

void check(bool ok, const char* msg)
{
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", msg);
    failures += !ok;
}

struct drawing {
    ovg_ctx_cb* cb = new_ctx_cb();
    rvg_t* vg = cb->new_rvg(cb->ac);
    drawing() { vg->width = width; vg->height = height; }
    ~drawing() { cb->destroy_rvg(vg); free_ctx_cb(cb); }
};

bool is_glow(const gcmd_t& c)
{
    return c.vg.stype == DRAW_VG && c.vg.state &&
        c.vg.type == 0 && c.vg.state->effect.type == VG_EFFECT_OUTER_GLOW;
}

text_st_t text_at(const char* s, float x = 20, float y = 16)
{
    text_st_t t = {};
    t.text = s; t.text_len = -1;
    t.pos = {x, y}; t.size = {600, 120};
    return t;
}

void check_recording(text_style_t style)
{
    drawing d;
    auto* cb = d.cb;
    auto* vg = d.vg;
    auto t = text_at("oS@oS@oS@oS@oS@oS@oS"); // 20 glyphs with curves/holes
    text_box_rt box = {};
    cb->add_text(vg, &t, &style, &box);
    auto data = get_draw_list(vg);
    if (!data.count || !data.v1_count || !data.ig_count) {
        check(false, "ordinary raster run produces geometry");
        return;
    }
    const size_t count = data.count;
    const std::vector<geomVertex1> body(data.vertex1, data.vertex1 + data.v1_count);
    const std::vector<uint32_t> indices(data.geom_indices, data.geom_indices + data.ig_count);
    auto same_body = [&](const ovg_draw_data_t& dl) {
        return dl.v1_count == body.size() && dl.ig_count == indices.size() &&
            std::memcmp(body.data(), dl.vertex1, body.size() * sizeof(body[0])) == 0 &&
            std::equal(indices.begin(), indices.end(), dl.geom_indices);
    };
    check(data.d[0].g.stype == DRAW_GEOM && data.v_count == 0, "default produces only raster geometry");
    cb->clear(vg);
    style.color_glow = red & 0x00ffffffu;
    style.glow_sigma = 4;
    cb->add_text(vg, &t, &style, &box);
    data = get_draw_list(vg);
    check(data.count == count && data.v_count == 0 && same_body(data), "alpha-zero glow has no effect or geometry changes");

    cb->clear(vg);
    style.color_glow = red;
    style.glow_spread = 2;
    cb->add_text(vg, &t, &style, &box);
    data = get_draw_list(vg);
    const bool added = data.count == count + 1 && is_glow(data.d[0]);
    check(added && same_body(data) && data.d[1].g.stype == DRAW_GEOM,
        "20 glyphs add one fill before unchanged raster text");
    if (added) {
        const auto& c = data.d[0].vg;
        check(c.ref == 0 && c.index.y > 0 && c.vertex.y > 0 &&
            c.state->effect.color == red && c.state->effect.sigma == 4 &&
            c.state->effect.spread == 1 && c.state->effect.opacity == 1 &&
            c.state->effect.blend == 0 && c.state->pushConsts.opacity == 0 &&
            data.vg_vertex[c.vertex.x].color == 0xffffffffu, "ABGR, nonempty opaque mask and invisible fill state");
    }

    cb->clear(vg);
    style.color = 0;
    const glm::mat3x2 m({2, 0}, {0, 2}, {17, 11});
    cb->set_matrix(vg, &m);
    cb->set_opacity(vg, 0.5f);
    vg_effect_t shadow = {};
    shadow.type = VG_EFFECT_SHADOW; shadow.color = green; shadow.opacity = 1;
    cb->set_effect(vg, &shadow);
    cb->rectangle(vg, 10, 10, 15, 15);
    cb->add_text(vg, &t, &style, &box);
    cb->fill(vg); // caller's original path/state, not the glow outline
    data = get_draw_list(vg);
    check(data.count == 2 && data.v1_count == 0 && is_glow(data.d[0]) &&
        data.d[0].vg.state->pushConsts.mat == m && data.d[0].vg.ref == 0 &&
        data.d[1].vg.ref == 1 && data.d[1].vg.state->pushConsts.opacity == 0.5f &&
        data.d[1].vg.state->effect.type == VG_EFFECT_SHADOW && data.d[1].vg.bounds.x == 35,
        "ctx matrix retained; glow-only skips atlas; caller path/effect/opacity restored");

    cb->clear(vg);
    style.color_shadow = green; style.shadow_pos = {3, 3};
    style.color_stroke = red; style.stroke = -1;
    cb->add_text(vg, &t, &style, &box);
    data = get_draw_list(vg);
    check(data.count == 1 + 2 * count && is_glow(data.d[0]) && data.v1_count == 5 * body.size(),
        "transparent body retains independent shadow and stroke");

    cb->clear(vg);
    style.color_shadow = style.color_stroke = 0;
    auto spaces = text_at("    ");
    cb->add_text(vg, &spaces, &style, &box);
    data = get_draw_list(vg);
    check(data.count == 0, "empty outlines do not reserve a fill or effect slot");
    auto letter = text_at("O");
    for (int i = 0; i < 129; ++i) cb->add_text(vg, &letter, &style, &box);
    data = get_draw_list(vg);
    bool refs = data.count == 127;
    for (size_t i = 0; refs && i < data.count; ++i)
        refs = is_glow(data.d[i]) && data.d[i].vg.ref == (int)i;
    check(refs, "exhausted slots do not submit invisible fills");
    style.color = 0xffffffffu;
    cb->add_text(vg, &letter, &style, &box);
    data = get_draw_list(vg);
    check(data.count == 128 && data.d[127].g.stype == DRAW_GEOM,
        "effect slot exhaustion preserves the body");

    cb->clear(vg);
    style.glow_mode = VG_TEXT_GLOW_INNER;
    style.glow_blend = VG_EFFECT_BLEND_ADD;
    style.glow_opacity = 0.4f;
    cb->add_text(vg, &letter, &style, &box);
    data = get_draw_list(vg);
    check(data.count == 2 && data.d[0].g.stype == DRAW_GEOM &&
        data.d[1].vg.state->effect.type == VG_EFFECT_INNER_GLOW &&
        data.d[1].vg.state->effect.blend == VG_EFFECT_BLEND_ADD &&
        data.d[1].vg.state->effect.opacity == 0.4f, "inner additive overlay follows body and carries strength");

    cb->clear(vg);
    style.color = 0;
    style.glow_mode = VG_TEXT_GLOW_OUTER;
    style.glow_blend = VG_EFFECT_BLEND_OVER;
    style.glow_opacity = 0;
    vg_text_glow_run_t batch[] = {{letter, style, box}, {text_at("i", 200, 18), style, box}};
    const vec4 decor{15, 50, 220, 2};
    check(cb->add_text_glow_batch && cb->add_text_glow_batch(vg, batch, 2, &decor, 1),
        "batch accepts independently positioned glyph runs and a decoration mask");
    data = get_draw_list(vg);
    check(data.count == 1 && is_glow(data.d[0]) && data.d[0].vg.ref == 0 &&
        data.d[0].vg.bounds.z >= 220.0f, "one effect covers the batch bounds");
    batch[1].style.glow_opacity = 0.5f;
    check(!cb->add_text_glow_batch(vg, batch, 2, nullptr, 0) && get_draw_list(vg).count == 1,
        "distinct strengths cannot be merged into one mask");

    auto* canvas = new_canvas_cb();
    auto* cvg = canvas->new_rvg(canvas->ac);
    cvg->width = width; cvg->height = height;
    auto* previous_path = cvg->path;
    auto* previous_state = cvg->st;
    canvas->add_text(cvg, &letter, &style, &box);
    data = get_draw_list(cvg);
    check(data.count == 1 && is_glow(data.d[0]) &&
        data.d[0].vg.state->pushConsts.mat == glm::mat3x2(1.0f) &&
        cvg->path == previous_path && cvg->st == previous_state, "canvas uses identity and restores its bindings");
    canvas->destroy_rvg(cvg);
    free_canvas_cb(canvas);

    std::vector<uint8_t> a(width * height * 4, 0), b = a;
    style.color = red;
    const bool ok = vg_render_text_rgba8(&letter, &style, &box, a.data(), width, height, width * 4, nullptr);
    style.color_glow = 0;
    check(ok && vg_render_text_rgba8(&letter, &style, &box, b.data(), width, height, width * 4, nullptr) &&
        a == b && std::any_of(a.begin(), a.end(), [](uint8_t c) { return c != 0; }), "CPU RGBA output ignores glow");
}

#ifndef VGUI_CORE_ONLY
constexpr int bench_width = 1280, bench_height = 720;
constexpr int bench_warmup = 30, bench_frames = 180;

struct bench_renderer {
    SDL_GPUDevice* gpu = nullptr;
    ovg_device_t* device = nullptr;
    ovg_ctx_t* ctx = nullptr;
    vg_fbo_t fbo{};

    ~bench_renderer() {
        if (ctx) ovg_wait_idle(ctx);
        if (fbo.ctx) free_vgfbo_sdl3(&fbo);
        if (ctx) free_ovgctx_sdl3(ctx);
        if (device) free_sdl3gpu_device(device);
        if (gpu) SDL_DestroyGPUDevice(gpu);
        SDL_Quit();
    }
};

bool bench_labels(bench_renderer& renderer, drawing& d, text_style_t style, int labels, bool glow)
{
    style.color_glow = glow ? 0xffffb040u : 0;
    style.glow_sigma = 4;
    const size_t requested = glow ? (size_t)labels : 0;
    using clock = std::chrono::steady_clock;
    using ms = std::chrono::duration<double, std::milli>;
    std::array<std::array<double, bench_frames>, 3> samples{};
    size_t cpu_drops = 0, gpu_skips = 0, roi_pixels = 0;
    size_t min_effects = requested, max_effects = 0;

    for (int frame = -bench_warmup; frame < bench_frames; ++frame) {
        const auto start = clock::now();
        d.cb->clear(d.vg);
        d.cb->reset_clip(d.vg, 1);
        text_box_rt box{};
        box.baseline = 32;
        for (int i = 0; i < labels; ++i) {
            auto label = text_at("Glow label", 32.0f + 246.0f * (i % 5), 18.0f + 68.0f * (i / 5));
            d.cb->add_text(d.vg, &label, &style, &box);
        }
        auto data = get_draw_list(d.vg);
        data.fbo = &renderer.fbo;
        const auto recorded = clock::now();
        ovg_render_frame(renderer.ctx, &data, 1);
        const auto submitted = clock::now();
        if (!SDL_WaitForGPUIdle(renderer.gpu)) {
            std::fprintf(stderr, "GPU wait failed: %s\n", SDL_GetError());
            return false;
        }
        const auto finished = clock::now();

        size_t slots = 0;
        for (size_t i = 0; i < data.count; ++i)
            if (is_glow(data.d[i]) && data.d[i].vg.ref >= 0) ++slots;
        const auto dropped = ovg_text_glow_budget_drops(d.vg);
        ovg_fx_stats_t fx{};
        ovg_fx_get_stats(renderer.ctx, &fx);
        if (!data.v1_count || !data.ig_count || slots + dropped != requested || fx.frame_effects > slots) {
            std::fprintf(stderr, "Invalid workload: %d labels, glow=%d, vertices=%zu, indices=%zu, "
                "expected=%zu, slots=%zu, CPU drops=%zu, GPU effects=%zu\n", labels, int(glow),
                data.v1_count, data.ig_count, requested, slots, dropped, fx.frame_effects);
            return false;
        }
        if (frame < 0) continue;
        samples[0][frame] = ms(recorded - start).count();
        samples[1][frame] = ms(submitted - recorded).count();
        samples[2][frame] = ms(finished - recorded).count();
        cpu_drops += dropped;
        gpu_skips += slots - fx.frame_effects;
        roi_pixels += fx.frame_roi_pixels;
        min_effects = std::min(min_effects, fx.frame_effects);
        max_effects = std::max(max_effects, fx.frame_effects);
    }
    std::printf("%2d labels | %-3s |", labels, glow ? "on" : "off");
    for (auto& segment : samples) {
        std::sort(segment.begin(), segment.end());
        const double p50 = (segment[bench_frames / 2 - 1] + segment[bench_frames / 2]) * 0.5;
        const double p95 = segment[(bench_frames * 95 + 99) / 100 - 1];
        std::printf(" %7.3f / %-7.3f |", p50, p95);
    }
    std::printf(" %zu-%zu | %zu | %zu | %.0f\n", min_effects, max_effects,
        cpu_drops, gpu_skips, double(roi_pixels) / bench_frames);
    if (glow && max_effects == 0) {
        std::fprintf(stderr, "No GPU effects prepared on a format-capable device; timings are not a glow measurement.\n");
        return false;
    }
    return true;
}

int benchmark_glow(text_style_t style)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::printf("SKIP SDL video: %s\n", SDL_GetError());
        return 77;
    }
    bench_renderer renderer;
    renderer.gpu = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, false, "vulkan");
    if (!renderer.gpu) {
        std::printf("SKIP Vulkan device: %s\n", SDL_GetError());
        return 77;
    }
    auto supports_format = [&](SDL_GPUTextureFormat format, SDL_GPUTextureUsageFlags usage) {
        return SDL_GPUTextureSupportsFormat(renderer.gpu, format, SDL_GPU_TEXTURETYPE_2D, usage);
    };
    const auto mask_usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    if ((!supports_format(SDL_GPU_TEXTUREFORMAT_R8_UNORM, mask_usage) &&
         !supports_format(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, mask_usage)) ||
        !supports_format(SDL_GPU_TEXTUREFORMAT_R16_FLOAT,
            SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE)) {
        std::puts("SKIP Vulkan device lacks OVG glow texture formats");
        return 77;
    }
    renderer.device = new_sdl3gpu_device(renderer.gpu);
    if (renderer.device) renderer.ctx = new_ovgctx_sdl3(renderer.device,
        SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT, SDL_GPU_SAMPLECOUNT_1);
    if (renderer.ctx) renderer.fbo = new_vgfbo_sdl3(renderer.ctx, bench_width, bench_height);
    if (!renderer.fbo.ctx || !ovg_fbo_color_texture(&renderer.fbo)) {
        std::fprintf(stderr, "Unable to create OVG benchmark framebuffer: %s\n", SDL_GetError());
        return 1;
    }
    drawing d;
    d.vg->width = bench_width;
    d.vg->height = bench_height;
#ifdef SDL_PROP_GPU_DEVICE_NAME_STRING
    std::printf("GPU: %s\n", SDL_GetStringProperty(SDL_GetGPUDeviceProperties(renderer.gpu),
        SDL_PROP_GPU_DEVICE_NAME_STRING, "unknown"));
#endif
    std::printf("OVG SDL3/%s: %dx%d RGBA8, 1x MSAA, 32px text, sigma=4, %d warmup + %d measured frames/case\n",
        SDL_GetGPUDeviceDriver(renderer.gpu), bench_width, bench_height, bench_warmup, bench_frames);
    std::puts("          |glow | record ms p50/p95  | submit ms p50/p95  | submit+wait p50/p95| FX/frame | CPU drops | GPU skips | ROI px/frame");
    for (int labels : {10, 50})
        for (bool glow : {false, true})
            if (!bench_labels(renderer, d, style, labels, glow)) return 1;
    return 0;
}

int check_pixels(text_style_t style)
{
    SDL_Init(SDL_INIT_VIDEO);
    auto* gpu = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, false, "vulkan");
    if (!gpu) {
        std::printf("SKIP Vulkan device: %s\n", SDL_GetError());
        SDL_Quit();
        return 77;
    }
    auto* device = new_sdl3gpu_device(gpu);
    auto* ctx = device ? new_ovgctx_sdl3(device, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
        SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT, SDL_GPU_SAMPLECOUNT_1) : nullptr;
    vg_fbo_t fbo = ctx ? new_vgfbo_sdl3(ctx, width, height) : vg_fbo_t{};
    if (fbo.ctx) {
        drawing d;
        text_box_rt box = {};
        style.fontsize = 44;
        style.color = 0;
        auto frame = [&](float sigma, bool effects, std::vector<uint32_t>& pixels) {
            d.cb->clear(d.vg);
            d.cb->reset_clip(d.vg, 1);
            // Keep the O's counter and exercise device placement with a nonidentity matrix.
            const glm::mat3x2 m({1.3f, 0}, {0, 1.3f}, {12, 6});
            d.cb->set_matrix(d.vg, &m);
            style.glow_sigma = sigma;
            auto t = text_at("O", 30, 30);
            style.color_glow = red;
            d.cb->add_text(d.vg, &t, &style, &box);
            t.pos.x += 140;
            style.color_glow = green;
            d.cb->add_text(d.vg, &t, &style, &box);
            auto data = get_draw_list(d.vg);
            if (!effects) {
                // Same draw list and normal pipeline, but no effect preparation/composite.
                for (size_t i = 0; i < data.count; ++i)
                    if (is_glow(data.d[i])) data.d[i].vg.ref = -1;
            }
            data.fbo = &fbo;
            ovg_render_frame(ctx, &data, 1);
            ovg_wait_idle(ctx);
            return download_texture(gpu, ovg_fbo_color_texture(&fbo), width, height, pixels);
        };
        std::vector<uint32_t> hard, soft, disabled;
        bool ok = frame(0, true, hard) && frame(4, true, soft) && frame(4, false, disabled);
        check(ok, "offscreen framebuffer readback");
        if (ok) {
            int red_pixels = 0, green_pixels = 0, outer_pixels = 0, foreign = 0;
            for (size_t i = 0; i < soft.size(); ++i) {
                red_pixels += (soft[i] & 0xffu) > 12;
                green_pixels += ((soft[i] >> 8) & 0xffu) > 12;
                foreign += (soft[i] & 0xff0000u) != (disabled[i] & 0xff0000u);
                outer_pixels += (hard[i] & 0xffu) == 0 && (soft[i] & 0xffu) > 12;
            }
            check(red_pixels > 100 && green_pixels > 100 && !foreign, "blurred glow-only pixels are red/green, not blue/white");
            check(outer_pixels > 100, "sigma=4 produces a halo outside the unblurred glyph mask");
            check(std::all_of(disabled.begin(), disabled.end(), [&](uint32_t p) { return p == disabled[0]; }),
                "simulated effects-disabled rendering leaves no glyph pixels");
        }
        free_vgfbo_sdl3(&fbo);
    } else {
        check(false, "initialize OVG offscreen renderer");
    }
    if (ctx) free_ovgctx_sdl3(ctx);
    if (device) free_sdl3gpu_device(device);
    SDL_DestroyGPUDevice(gpu);
    SDL_Quit();
    return failures != 0;
}
#endif
} // namespace

int main(int argc, char** argv)
{
    const bool gpu = argc == 2 && std::strcmp(argv[1], "--gpu") == 0;
    const bool bench = argc == 2 && std::strcmp(argv[1], "--bench") == 0;
    if (argc > 1 && !gpu && !bench) {
        std::fprintf(stderr, "Usage: %s [--gpu | --bench]\n", argv[0]);
        return 2;
    }
#ifdef VGUI_CORE_ONLY
    if (gpu || bench) {
        std::puts("SKIP GPU modes are unavailable without the OVG SDL3 renderer");
        return 77;
    }
#endif
    auto* cache = new_font_cache();
    const std::string path = std::string(VGUI_SOURCE_DIR) + "/sample/res/fonts/DejaVuSans.ttf";
    auto* family = cache && load_font_from_path(cache, path.c_str(), "GlowTestDejaVu") ?
        new_font_family(cache, "GlowTestDejaVu", nullptr) : nullptr;
    int result = 1;
    if (family && family->count > 0) {
        text_style_t style = {};
        style.family = family; style.fontsize = 32; style.color = 0xffffffffu;
#ifndef VGUI_CORE_ONLY
        if (bench)
            result = benchmark_glow(style);
        else if (gpu)
            result = check_pixels(style);
        else
#endif
        {
            check_recording(style);
            result = failures != 0;
        }
    } else {
        std::fprintf(stderr, "unable to load %s\n", path.c_str());
    }
    if (family) delete_font_family(family);
    if (cache) free_font_cache(cache);
    return result;
}
