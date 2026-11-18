// Minimal destruction regression: no fonts, draw lists, or effects needed.
#include <glm/glm.hpp>
#include "ovg.h"
#ifndef VGUI_CORE_ONLY
#include "ovg_renderer_sdl3.h"
#endif

#include <cstdio>
#include <cstring>

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
