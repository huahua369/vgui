// Canvas lifetime and recording regressions, without fonts or effects.
#include <glm/glm.hpp>
#include "ovg.h"
#ifndef VGUI_CORE_ONLY
#include "ovg_renderer_sdl3.h"
#endif

#include <cstdio>
#include <cstring>

static int check_empty_recording()
{
    auto* canvas = new_ctx_cb();
    auto* vg = canvas->new_rvg(canvas->ac);
    if (!vg) {
        free_ctx_cb(canvas);
        return 1;
    }
    vg->width = vg->height = 64;
    const auto draw_rect = [&] {
        canvas->new_path(vg);
        canvas->set_source_color(vg, 0xff505050u);
        canvas->rectangle(vg, 4, 4, 16, 16);
        canvas->fill(vg);
    };
    const auto check = [](bool ok, const char* message) {
        if (!ok) std::fprintf(stderr, "%s\n", message);
        return ok;
    };

    canvas->start_recording(vg);
    auto* empty = canvas->stop_recording(vg, nullptr);
    bool ok = check(empty && canvas->recording_get_count(empty) == 0,
        "a fresh empty recording must return a valid zero-command handle");
    ok &= check(!canvas->stop_recording(vg, nullptr), "an empty stop must end the recording");

    canvas->start_recording(vg);
    draw_rect();
    auto* rec = canvas->stop_recording(vg, nullptr);
    const auto previous = get_draw_list(vg);
    ok &= check(rec && canvas->recording_get_count(rec) > 0 &&
        previous.count > 0 && previous.v_count > 0 && previous.i_count > 0,
        "the fixture must record a command with vertices and indices");

    // Start at the end of a nonempty command list; no new commands are appended.
    canvas->start_recording(vg);
    auto* reused = canvas->stop_recording(vg, rec);
    ok &= check(reused && reused == rec && canvas->recording_get_count(reused) == 0,
        "empty recording must replace the old contents in the same handle");
    canvas->replay(vg, empty);
    canvas->replay(vg, reused);
    canvas->replay_command(vg, reused, 0);
    const auto after = get_draw_list(vg);
    ok &= check(after.count == previous.count && after.v_count == previous.v_count &&
        after.i_count == previous.i_count, "empty replay must not append old commands, vertices or indices");

    if (reused != rec) canvas->recording_destroy(reused);
    canvas->recording_destroy(rec);
    canvas->recording_destroy(empty);
    canvas->destroy_rvg(vg);
    free_ctx_cb(canvas);
    return ok ? 0 : 1;
}

#ifndef VGUI_CORE_ONLY
static int check_gpu_lifetime()
{
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::printf("SKIP SDL video: %s\n", SDL_GetError());
        return 77;
    }
    auto* gpu = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, false, "vulkan");
    if (!gpu) {
        std::printf("SKIP Vulkan device: %s\n", SDL_GetError());
        SDL_Quit();
        return 77;
    }

    auto* dev = new_sdl3gpu_device(gpu);
    auto make_context = [&] {
        return new_ovgctx_sdl3(dev, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
            SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT, SDL_GPU_SAMPLECOUNT_1);
    };
    auto* first = make_context();
    auto* second = make_context();
    auto a = new_vgfbo_sdl3(first, 64, 64);
    auto b = new_vgfbo_sdl3(second, 64, 64);
    const bool ok = first && second && ovg_fbo_color_texture(&a) && a.depthStencilTex &&
        ovg_fbo_color_texture(&b) && b.depthStencilTex;

    // Both contexts borrow the same device texture. Neither may destroy it.
    free_vgfbo_sdl3(&a);
    free_ovgctx_sdl3(first);
    free_vgfbo_sdl3(&b);
    free_ovgctx_sdl3(second);
    free_sdl3gpu_device(dev);
    SDL_DestroyGPUDevice(gpu);
    SDL_Quit();
    if (!ok) std::fprintf(stderr, "Failed to create two offscreen OVG contexts and FBOs\n");
    return ok ? 0 : 1;
}
#endif

int main(int argc, char** argv)
{
    if (argc == 2 && std::strcmp(argv[1], "--recording") == 0)
        return check_empty_recording();
    if (argc == 2 && std::strcmp(argv[1], "--gpu") == 0) {
#ifndef VGUI_CORE_ONLY
        return check_gpu_lifetime();
#else
        return 77;
#endif
    }

    free_canvas_cb(nullptr);
    auto* canvas = new_canvas_cb();
    auto* rvg = canvas->new_rvg(canvas->ac);
    const bool ok = rvg != nullptr;
    if (rvg) canvas->destroy_rvg(rvg);
    free_canvas_cb(canvas);
    return ok ? 0 : 1;
}
