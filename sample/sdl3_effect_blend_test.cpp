// Standalone SDL GPU regression. Compile normally against libovg, or with
// -DTEST_VGUI_SDL3 against libvgui and libovg. No window or font is needed.
#include "gpu_test_util.h"
#include <glm/glm.hpp>
#include "ovg.h"
#ifdef TEST_VGUI_SDL3
#include <vgui/backend/vgui_renderer_sdl3.h>
#else
#include "ovg_renderer_sdl3.h"
#endif

#include <cmath>
#include <cstdio>
#if __has_include(<vulkan/vulkan.h>)
#include <vulkan/vulkan.h>
#endif

namespace {
SDL_GPUDevice* create_gpu()
{
#if defined(VK_VERSION_1_2) && defined(SDL_PROP_GPU_DEVICE_CREATE_VULKAN_OPTIONS_POINTER)
    // The shared SPIR-V uses scalar UBO layouts and declares DrawParameters.
    VkPhysicalDeviceVulkan12Features features12 = {};
    features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    features12.scalarBlockLayout = VK_TRUE;
    VkPhysicalDeviceVulkan11Features features11 = {};
    features11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    features11.pNext = &features12;
    features11.shaderDrawParameters = VK_TRUE;
    SDL_GPUVulkanOptions options = {};
    options.vulkan_api_version = VK_API_VERSION_1_2;
    options.feature_list = &features11;
    const auto props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_GPU_DEVICE_CREATE_NAME_STRING, "vulkan");
    SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_SPIRV_BOOLEAN, true);
    SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_DEBUGMODE_BOOLEAN, true);
    SDL_SetPointerProperty(props, SDL_PROP_GPU_DEVICE_CREATE_VULKAN_OPTIONS_POINTER, &options);
    auto* gpu = SDL_CreateGPUDeviceWithProperties(props);
    SDL_DestroyProperties(props);
    return gpu;
#else
    SDL_SetError("Vulkan 1.2 headers and SDL Vulkan device options are required");
    return nullptr;
#endif
}

constexpr int width = 512, height = 512;
int failures = 0;

void check(bool ok, const char* label)
{
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", label);
    failures += !ok;
}

bool near(uint32_t pixel, uint32_t expected)
{
    for (int shift = 0; shift < 32; shift += 8) {
        if (std::abs(int((pixel >> shift) & 255u) - int((expected >> shift) & 255u)) > 2)
            return false;
    }
    return true;
}

struct scene {
    ovg_ctx_cb* cb = new_ctx_cb();
    rvg_t* vg = cb->new_rvg(cb->ac);
    scene() { vg->width = width; vg->height = height; }
    ~scene() { cb->destroy_rvg(vg); free_ctx_cb(cb); }

    void reset()
    {
        cb->clear(vg);
        cb->reset_clip(vg, 1);
    }

    void rect(int x, int y, int w, int h, uint32_t body, const vg_effect_t& fx = {},
        float opacity = 1)
    {
        cb->set_effect(vg, &fx);
        cb->set_opacity(vg, opacity);
        cb->set_source_color(vg, body);
        cb->rectangle(vg, float(x), float(y), float(w), float(h));
        cb->fill(vg);
    }
};

void blend_scene(scene& s)
{
    s.reset();
    s.rect(4, 4, 110, 48, rgba(0, 0, 255, 128));
    vg_effect_t glow = {};
    glow.type = VG_EFFECT_OUTER_GLOW;
    glow.color = rgba(255, 0, 0, 128);
    glow.opacity = 0.5f;
    s.rect(8, 8, 40, 40, rgba(255, 255, 255), glow, 0); // zero-initialized blend: OVER
    glow.blend = VG_EFFECT_BLEND_ADD;
    s.rect(64, 8, 40, 40, rgba(255, 255, 255), glow, 0);
    glow.color = rgba(0, 255, 0, 128);
    s.rect(84, 8, 24, 40, rgba(255, 255, 255), glow, 0);
    glow.type = VG_EFFECT_INNER_GLOW;
    glow.color = rgba(255, 0, 0);
    glow.sigma = 4;
    s.rect(8, 64, 40, 40, rgba(0, 0, 64), glow);
    s.rect(120, 64, 24, 24, rgba(32, 64, 128));
}
} // namespace

int main()
{
    SDL_Init(SDL_INIT_VIDEO);
    auto* gpu = create_gpu();
    if (!gpu) {
        std::printf("SKIP Vulkan device: %s\n", SDL_GetError());
        SDL_Quit();
        return 77;
    }
    constexpr auto format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    constexpr auto depth = SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT;
#ifdef TEST_VGUI_SDL3
    auto* device = vgpu_create_device(gpu);
    auto* ctx = device ? vgpu_create_context(device, format, depth, SDL_GPU_SAMPLECOUNT_1) : nullptr;
    const auto bgra = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
    const bool alt = ctx && vgpu_supports_color_format(ctx, bgra, SDL_GPU_SAMPLECOUNT_4);
    vg_fbo_t fbo = ctx ? vgpu_create_fbo(ctx, width, height, nullptr, 0,
        alt ? bgra : format, alt ? 4 : 1) : vg_fbo_t{};
    if (alt) std::puts("Testing alternate BGRA8/4x-MSAA effect PSOs");
    auto* texture = vgpu_fbo_color_texture(&fbo);
#else
    auto* device = new_sdl3gpu_device(gpu);
    auto* ctx = device ? new_ovgctx_sdl3(device, format, depth, SDL_GPU_SAMPLECOUNT_1) : nullptr;
    vg_fbo_t fbo = ctx ? new_vgfbo_sdl3(ctx, width, height) : vg_fbo_t{};
    auto* texture = ovg_fbo_color_texture(&fbo);
#endif
    check(texture != nullptr, "create framebuffer");
    if (texture) {
        scene s;
        std::vector<uint32_t> pixels;
        auto frame = [&] {
            auto draw = get_draw_list(s.vg);
            draw.fbo = &fbo;
#ifdef TEST_VGUI_SDL3
            vgpu_render_frame(ctx, &draw, 1);
#else
            ovg_render_frame(ctx, &draw, 1);
#endif
            return download_texture(gpu, texture, width, height, pixels);
        };
        auto at = [&](int x, int y) {
            const auto p = pixels[size_t(y) * width + x];
#ifdef TEST_VGUI_SDL3
            if (alt) return (p & 0xff00ff00u) | ((p & 255u) << 16) | ((p >> 16) & 255u);
#endif
            return p;
        };
        blend_scene(s);
        const bool read = frame();
        check(read, "download blend scene");
        if (read) {
            check(near(at(20, 20), rgba(64, 0, 96, 160)), "default OVER attenuates destination RGB and adds coverage alpha");
            check(near(at(70, 20), rgba(64, 0, 128, 128)), "ADD weights RGB once and preserves destination alpha");
            check(near(at(90, 20), rgba(64, 64, 128, 128)), "overlapping ADD effects sum RGB");
            check((at(9, 80) & 255u) > 10 && near(at(24, 80), rgba(0, 0, 64)) &&
                at(6, 80) == 0, "inner ADD draws after the body and stays inside its coverage");
            check(near(at(130, 72), rgba(32, 64, 128)), "normal draw after effects is unchanged");
        }
#ifndef TEST_VGUI_SDL3
        ovg_fx_set_budget(ctx, 0, 1u << 20);
        s.reset();
        vg_effect_t oversized = {};
        oversized.type = VG_EFFECT_OUTER_GLOW;
        oversized.blend = VG_EFFECT_BLEND_ADD;
        oversized.sigma = 1;
        oversized.color = rgba(255, 0, 0);
        oversized.opacity = 1;
        s.rect(2, 2, 508, 508, rgba(255, 255, 255), oversized, 0);
        s.rect(2, 2, 508, 430, rgba(0, 0, 64), oversized);
        oversized.sigma = 0;
        s.rect(16, 16, 40, 40, rgba(255, 255, 255), oversized, 0);
        const bool budget_read = frame();
        ovg_fx_stats_t stats = {};
        ovg_fx_get_stats(ctx, &stats);
        check(budget_read && stats.frame_effects == 1 && stats.frame_bytes < (1u << 20),
            "1 MiB preflight rejects oversized ROIs but accepts a later small effect");
        check(budget_read && near(at(256, 256), rgba(0, 0, 64)) && at(256, 470) == 0 &&
            near(at(24, 24), rgba(255, 0, 64)), "failed effect keeps body geometry and does not expose a mask-only fill");
#endif
    }
#ifdef TEST_VGUI_SDL3
    if (ctx) { vgpu_destroy_fbo(&fbo); vgpu_destroy_context(ctx); }
    if (device) vgpu_destroy_device(device);
#else
    if (ctx) { free_vgfbo_sdl3(&fbo); free_ovgctx_sdl3(ctx); }
    if (device) free_sdl3gpu_device(device);
#endif
    SDL_DestroyGPUDevice(gpu);
    SDL_Quit();
    return failures != 0;
}
