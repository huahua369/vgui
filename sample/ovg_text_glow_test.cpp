// Standalone G1 regression. Default mode needs no GPU; --gpu exercises the
// SDL3 OVG renderer and returns 77 if no Vulkan device is available.
#include <glm/glm.hpp>
#include "ovg.h"
#include "ovg_fonts.h"
#ifndef VGUI_CORE_ONLY
#include "ovg_renderer_sdl3.h"
#include "gpu_test_util.h"
#endif

#include <algorithm>
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
#ifdef VGUI_CORE_ONLY
    if (gpu) {
        std::puts("SKIP GPU mode is unavailable in VGUI_CORE_ONLY");
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
        if (gpu)
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
