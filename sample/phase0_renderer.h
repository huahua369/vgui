#pragma once
// Minimal Phase 0 3D renderer with V09 multi-view support.
//
// One phase0_renderer owns the shared GPU state (shaders, pipelines, cube and
// ghost vertex buffers, samplers).  Each phase0_view owns its camera, instance
// and grid lists plus a color/depth attachment set, so several cameras can
// render into independent textures in the same frame.
//
// The color texture returned by phase0_view_color_texture() is always an
// RGBA8 UI texture that is borrowed by vgui through
// vgui_sdl3_texture_create_native() and never released by vgui.  HDR views
// render into RGBA16F first and are tonemapped into that texture; MSAA views
// resolve into it before the UI samples it.  phase0_view_depth_preview_texture()
// is an RGBA8 visualization of the depth attachment suitable for a second
// viewport or post-processing preview.

#include <cstdint>

struct SDL_GPUDevice;
struct SDL_GPUTexture;

struct phase0_renderer;
struct phase0_view;

/* Optional external view for editor-style camera navigation.  The defaults
 * reproduce the built-in view used by the Phase 0 sample. */
struct phase0_camera
{
	float eye[3] = { 3.2f, 2.8f, 4.5f };
	float target[3] = { 0.0f, 0.0f, 0.0f };
	float fov_deg = 50.0f;
};

/* Per-view rendering options.
 *
 * hdr            render the scene into RGBA16F and tonemap to RGBA8.
 * msaa_samples   1, 2, 4 or 8; clamped to a sample count supported by the
 *                device for the view's color and depth formats.
 * depth_preview  generate phase0_view_depth_preview_texture() each frame.
 */
struct phase0_view_options
{
	bool hdr = false;
	int msaa_samples = 1;
	bool depth_preview = false;
};

phase0_renderer* phase0_renderer_create(SDL_GPUDevice* device);
void phase0_renderer_destroy(phase0_renderer* renderer);

/* The default view backs the legacy phase0_renderer_* single-view API below.
 * It is owned by the renderer and cannot be destroyed separately. */
phase0_view* phase0_renderer_default_view(phase0_renderer* renderer);
phase0_view* phase0_renderer_create_view(phase0_renderer* renderer);
bool phase0_renderer_destroy_view(phase0_renderer* renderer, phase0_view* view);

/* Recreate the view's attachments.  Returns true when textures were recreated
 * (the caller must re-query the texture accessors and rebind UI images). */
bool phase0_view_resize(phase0_view* view, int width, int height);

/* Changing hdr/msaa_samples recreates attachments and returns true.  Changing
 * only depth_preview returns false because texture handles stay stable. */
bool phase0_view_set_options(phase0_view* view,
	const phase0_view_options* options);
phase0_view_options phase0_view_get_options(const phase0_view* view);
bool phase0_view_set_hdr(phase0_view* view, bool hdr);
bool phase0_view_set_msaa_samples(phase0_view* view, int samples);
bool phase0_view_set_depth_preview(phase0_view* view, bool enabled);

int phase0_view_width(const phase0_view* view);
int phase0_view_height(const phase0_view* view);
SDL_GPUTexture* phase0_view_color_texture(phase0_view* view);
/* Raw single-sample scene depth.  NULL while MSAA is enabled, because SDL_GPU
 * has no depth resolve. */
SDL_GPUTexture* phase0_view_depth_texture(phase0_view* view);
/* RGBA8 depth visualization.  The handle is stable across depth-preview
 * toggles and is only refreshed while depth_preview is enabled. */
SDL_GPUTexture* phase0_view_depth_preview_texture(phase0_view* view);

/* Overrides the built-in view.  Pass NULL to restore the Phase 0 default. */
void phase0_view_set_camera(phase0_view* view, const phase0_camera* camera);

/* Optional world grid, submitted as line segments (world space).  Colour uses
 * the renderer's vertex order 0xAABBGGRR; width is in world units.  Pass
 * NULL/0 to clear.  The grid is drawn after the cube with depth testing so
 * the cube occludes it correctly.  Each view owns its own grid buffer. */
struct phase0_grid_line
{
	float a[3] = { 0.0f, 0.0f, 0.0f };
	float b[3] = { 0.0f, 0.0f, 0.0f };
	uint32_t color = 0xFFFFFFFFu;
	float width = 1.0f;
};

void phase0_view_set_grid(phase0_view* view, const phase0_grid_line* lines,
	int count);

/* Editor demo: place/rotate the cube.  Once called the built-in idle spin is
 * disabled, so the host fully owns the object transform. */
void phase0_view_set_object_transform(phase0_view* view,
	const float position[3], float yaw_radians);

/* V06/V07: draw a list of cube proxies instead of the built-in single cube.
 * Each instance carries a world position, a quaternion rotation and per-axis
 * scale.  Ghost instances are translucent and do not write depth, which the
 * viewport drop preview uses.  Calling this function (even with count 0)
 * switches the renderer to instance mode; the built-in cube only appears
 * before the first call. */
struct phase0_instance
{
	float position[3] = { 0.0f, 0.0f, 0.0f };
	/* XYZW quaternion.  An all-zero value falls back to identity. */
	float rotation[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
	float scale[3] = { 1.0f, 1.0f, 1.0f };
	bool ghost = false;
};

void phase0_view_set_instances(phase0_view* view,
	const phase0_instance* instances, int count);

/* Renders every live view into its own attachments in one command buffer. */
void phase0_renderer_render(phase0_renderer* renderer, float time_seconds);

//---------------------------------------------------------------------------
// Legacy single-view API (operates on phase0_renderer_default_view)
//---------------------------------------------------------------------------

bool phase0_renderer_resize(phase0_renderer* renderer, int width, int height);
SDL_GPUTexture* phase0_renderer_color_texture(phase0_renderer* renderer);
int phase0_renderer_width(phase0_renderer* renderer);
int phase0_renderer_height(phase0_renderer* renderer);

void phase0_renderer_set_camera(phase0_renderer* renderer,
	const phase0_camera* camera);
void phase0_renderer_set_grid(phase0_renderer* renderer,
	const phase0_grid_line* lines, int count);
void phase0_renderer_set_object_transform(phase0_renderer* renderer,
	const float position[3], float yaw_radians);
void phase0_renderer_set_instances(phase0_renderer* renderer,
	const phase0_instance* instances, int count);
