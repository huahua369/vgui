#pragma once
// Minimal Phase 0 3D renderer.
//
// It owns a rotating cube and an offscreen color/depth target.  The color
// texture is borrowed by vgui through vgui_sdl3_texture_create_native() and is
// never released by vgui.

#include <cstdint>

struct SDL_GPUDevice;
struct SDL_GPUTexture;

struct phase0_renderer;

/* Optional external view for editor-style camera navigation.  The defaults
 * reproduce the built-in view used by the Phase 0 sample. */
struct phase0_camera
{
	float eye[3] = { 3.2f, 2.8f, 4.5f };
	float target[3] = { 0.0f, 0.0f, 0.0f };
	float fov_deg = 50.0f;
};

phase0_renderer* phase0_renderer_create(SDL_GPUDevice* device);
void phase0_renderer_destroy(phase0_renderer* renderer);

// Recreate the offscreen target.  Returns true when resources were recreated,
// in which case color_texture() is a new handle that must be rebound to the UI
// image.  The caller must not still be using the previous handle.
bool phase0_renderer_resize(phase0_renderer* renderer, int width, int height);
SDL_GPUTexture* phase0_renderer_color_texture(phase0_renderer* renderer);
int phase0_renderer_width(phase0_renderer* renderer);
int phase0_renderer_height(phase0_renderer* renderer);

/* Overrides the built-in view.  Pass NULL to restore the Phase 0 default. */
void phase0_renderer_set_camera(phase0_renderer* renderer,
	const phase0_camera* camera);

/* Optional world grid, submitted as line segments (world space).  Colour uses
 * the renderer's vertex order 0xAABBGGRR; width is in world units.  Pass
 * NULL/0 to clear.  The grid is drawn after the cube with depth testing so
 * the cube occludes it correctly. */
struct phase0_grid_line
{
	float a[3] = { 0.0f, 0.0f, 0.0f };
	float b[3] = { 0.0f, 0.0f, 0.0f };
	uint32_t color = 0xFFFFFFFFu;
	float width = 1.0f;
};

void phase0_renderer_set_grid(phase0_renderer* renderer,
	const phase0_grid_line* lines, int count);

/* Editor demo: place/rotate the cube.  Once called the built-in idle spin is
 * disabled, so the host fully owns the object transform. */
void phase0_renderer_set_object_transform(phase0_renderer* renderer,
	const float position[3], float yaw_radians);

/* V06: draw a list of cube proxies instead of the built-in single cube.
 * Each instance carries a world position, yaw and uniform scale.  Ghost
 * instances are translucent and do not write depth, which the viewport drop
 * preview uses.  Calling this function (even with count 0) switches the
 * renderer to instance mode; the built-in cube only appears before the first
 * call. */
struct phase0_instance
{
	float position[3] = { 0.0f, 0.0f, 0.0f };
	float yaw = 0.0f;
	float scale = 1.0f;
	bool ghost = false;
};

void phase0_renderer_set_instances(phase0_renderer* renderer,
	const phase0_instance* instances, int count);

void phase0_renderer_render(phase0_renderer* renderer, float time_seconds);
