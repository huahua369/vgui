#pragma once
// Minimal Phase 0 3D renderer.
//
// It owns a rotating cube and an offscreen color/depth target.  The color
// texture is borrowed by vgui through vgui_sdl3_texture_create_native() and is
// never released by vgui.

struct SDL_GPUDevice;
struct SDL_GPUTexture;

struct phase0_renderer;

phase0_renderer* phase0_renderer_create(SDL_GPUDevice* device);
void phase0_renderer_destroy(phase0_renderer* renderer);

// Recreate the offscreen target.  Returns true when resources were recreated,
// in which case color_texture() is a new handle that must be rebound to the UI
// image.  The caller must not still be using the previous handle.
bool phase0_renderer_resize(phase0_renderer* renderer, int width, int height);
SDL_GPUTexture* phase0_renderer_color_texture(phase0_renderer* renderer);
int phase0_renderer_width(phase0_renderer* renderer);
int phase0_renderer_height(phase0_renderer* renderer);

void phase0_renderer_render(phase0_renderer* renderer, float time_seconds);
