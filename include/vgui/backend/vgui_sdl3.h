#pragma once
/*
 * vgui SDL3 backend: windows, input, IME, DPI and GPU viewports.
 *
 * Created: 2026-09-12
 */
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#include <glm/glm.hpp>

#include <ovg_c.h>

#include <vgui/vgui.h>
#include <vgui/backend/vgui_backend_types.h>
#include <vgui/backend/vgui_renderer_sdl3.h>

namespace hz {
	// Returns the monitor scale; pels is a glm::ivec2*.
	float get_monitor_scale(void* pels);
	// Returns the installed printer names.
	std::vector<std::string> get_print_devname();
	std::string get_temp_path();
	class drop_info_cx;
	class drop_regs;
}

enum form_flags_e
{
	ef_null = 0,					// ef_default
	ef_fullscreen = BIT_INC(0),		// fullscreen
	ef_utility = BIT_INC(1),		// hidden from the taskbar
	ef_resizable = BIT_INC(2),		// resizable
	ef_transparent = BIT_INC(3),	// transparent window
	ef_borderless = BIT_INC(4),		// no system border
	ef_popup = BIT_INC(5),			// popup window; requires a parent
	ef_tooltip = BIT_INC(6),		// tooltip window; requires a parent
	ef_gpu = BIT_INC(7),			// GPU rendering
	ef_vulkan = BIT_INC(8),			// Vulkan rendering
	ef_dx11 = BIT_INC(11), 			// D3D11 rendering
	ef_vsync = BIT_INC(13),
	ef_minimized = BIT_INC(14),
	ef_maximized = BIT_INC(15),
	ef_default = ef_resizable | ef_vulkan
};
enum class fcv_type {
	e_null,
	e_show,
	e_hide,
	e_visible_rev,
	e_size,
	e_pos
};
enum class WindowType :uint8_t {
	Regular,
	Tooltip,
	PopupMenu,
	Modal
};
struct PlatformMonitor
{
	glm::vec2 MainPos, MainSize;	// Coordinates of the area displayed on this monitor (Min = upper left, Max = bottom right)
	glm::vec2 WorkPos, WorkSize;	// Coordinates without task bars / side bars / menu bars. Used to avoid positioning popups/tooltips inside this region. If you don't have this info, please copy the value for MainPos/MainSize.
	float DpiScale;					// 1.0f = 96 DPI
	void* PlatformHandle;			// Backend dependant data (e.g. HMONITOR, GLFWmonitor*, SDL Display Index, NSScreen*)
};

struct gui_viewport {
	gui_io_state_t io = {};
	// Retained C API tree attached to this window.  The viewport owns the
	// context and destroys it together with the window.
	vgui_context* ui = nullptr;
	// Effective UI scale last pushed to `ui` (content scale, or the override
	// below).  Widget geometry and input are kept in render pixels, so
	// `pixel_ratio` converts SDL window points to that space.
	float ui_scale = 1.0f;
	float pixel_ratio = 1.0f;
	// Optional host override (for example an accessibility zoom).  <= 0 means
	// "follow the monitor content scale" (the default).
	float ui_scale_override = 0.0f;
	// Rectangle used by the SDL IME.  io.ime_rect points here.
	glm::ivec4 ime_rect = {};
	std::string drop_text;
	// F03: file paths from the current OS drag batch (aggregated from
	// DROP_FILE events and delivered on COMPLETE).
	std::vector<std::string> drop_files;
	std::vector<const char*> drop_file_ptrs;
	glm::ivec2 _last_pos = {};
	//div_cx0 _root = {};
public:
	gui_viewport() = default;
	gui_viewport(const gui_viewport&) = delete;
	gui_viewport& operator=(const gui_viewport&) = delete;
	~gui_viewport();
	void trigger(dev_event_t* e);
};

struct os_window {
	SDL_Window* window = nullptr;
	os_window* parent = 0;      // 0 = top-level window
	vg_fbo_t fbo = {};
	gui_viewport* viewport = nullptr;
	uint32_t id = 0;
	WindowType type = WindowType::Regular;
	bool close_with_parent = true;
	bool pending_close = false;
	bool mouse_in_client = false;        // optional pointer-in-client tracking

	// UI callbacks.
	std::function<void()> on_close;
	std::vector<os_window*> children;
	~os_window();
	// Mouse capture.
	void set_capture();
	void release_capture();
	// DPI helpers.  SDL window coordinates are in points; vgui lays out and
	// draws in render pixels.  `content_scale()` is the monitor UI scale,
	// `pixel_ratio()` is pixels / window points.
	float content_scale() const;
	float pixel_ratio() const;
	// Re-reads the window scale and applies it to viewport->ui.  Returns true
	// when the effective scale or pixel ratio changed.  Safe to call every
	// frame; a no-op when nothing changed.
	bool sync_ui_scale(bool force = false);
	// Text input / IME.
	void start_text_input();
	void stop_text_input();
	bool text_input_active();
	// Sets the IME candidate rectangle.
	void set_ime_pos(const glm::ivec4& r);
	// Enables/disables mouse and keyboard input for this window; used by modals.
	void enable_window(bool bEnable);
	// Moves the mouse to a window-relative/global position.
	void set_mouse_pos(const glm::ivec2& pos);
	void set_mouse_pos_global(const glm::ivec2& pos);
	// Shows/hides the cursor.
	void show_cursor();
	void hide_cursor();
};
struct PointerState {
	SDL_Window* window = nullptr;
	glm::ivec2  pos{};
	bool pressed = false;
};
class docking_mgr {
public:
	bool viewports_enabled = false; // off by default; enabled for docking
public:
	void set_viewports_enabled(bool v) { viewports_enabled = v; }

private:
};
class app_mgr
{
public:
	SDL_GPUDevice* device = nullptr;
	SDL_Cursor* system_cursor[SDL_SYSTEM_CURSOR_COUNT] = {};
	std::vector<PlatformMonitor> monitors;
	os_window* main_window = 0;
	vgpu_device_t* vgdev = 0;		// vgpu render device
	font_cache_cx* font_ctx = 0;
	vgpu_ctx_t* vgctx = 0;

	SDL_GPUSampleCount samplecount = SDL_GPU_SAMPLECOUNT_1; // default sample count
	gui_io_mk_state _mouse_key = {};
	gui_viewport* active_viewport_ = 0;
	docking_mgr docking_ = {};
	bool nc_down = false;
	bool WantUpdateMonitors = true;
	bool capture_type = true;
private:
	std::vector<std::unique_ptr<os_window>> windows_;
public:
	app_mgr();
	~app_mgr();

	bool init_gpu(bool is_vulkan);
	// Creates the main/regular window.
	os_window* create(const char* title, int w, int h, uint32_t flags, bool vsync);
	// Creates a regular, menu, tooltip or utility window with an optional parent.
	os_window* create2(const char* title, int x, int y, int w, int h, uint32_t flags, os_window* parent);
	os_window* find(uint32_t id);
	void destroy(uint32_t id);

	void wait_gpu();
	void shutdown();

	size_t window_count() const;
	std::vector<std::unique_ptr<os_window>>& windows();

	void kncdown();
	bool has_maximized(void* nwptr);
	void UpdateMonitors();
	// Re-syncs every window's UI scale (safe to call once per frame).
	void sync_ui_scales();
	int get_event();
	void process_event(const SDL_Event* e);
	gui_io_state_t* io();
	docking_mgr& docking();
	void set_defcursor(cursor_st t);
private:
	void set_syscursor(int type);
};

// Returns the clipboard text.
std::string get_clipboard();
// Sets the clipboard text.
void set_clipboard(const char* str);

struct vg_pattern_data {
	union {
		glm::vec4 linear;		// float x0, float y0, float x1, float y1
		glm::vec3 radial[2];	// float cx0, float cy0, float radius0, float cx1, float cy1, float radius1
		glm::vec4 sweep;		// float cx, float cy, float start_angle, float end_angle
	}g;
	int count;
	glm::vec4 colors[32];
	float stops[32];
	glm::mat3x2 matrix;	//
	int extend;
	int filter;
	bool is_ellipse;	// radial gradients only
};
struct vg_types_data
{
	float opacity;
	uint32_t c;
	float width;
	float limit;
	uint8_t cap;
	uint8_t join;
	uint8_t op;
	uint8_t fill_rule;
	int id_pattern;
	int id_image;// glm::vec2 surf_pos;
	int id_dashes;
	//const float* dashes; uint32_t num_dashes; float offset;
	glm::mat3x2 transform;
	glm::vec2 translate;
	glm::vec2 scale;
	float radians;
};

class FpsCounter
{
public:
	FpsCounter();
	~FpsCounter();

	float tick();
	int get_fps();
	const char* c_str();
private:
	uint32_t prev_time = 0;
	float avg_delta = 0.0f;
	float talpha = 0.05f;  // sampling factor (smoothing)
	int cfps = 0;
	int cfps1 = 0;
	std::string str;
};
class draw_datas_cx
{
public:
	std::vector<rvg_t*> v;
	std::vector<ovg_draw_data_t> data;
	vg_fbo_t* fbo = 0;
public:
	draw_datas_cx();
	~draw_datas_cx();
	void clear();
	void set(vg_fbo_t* fbo);
	void add(rvg_t* p);
	void render(vgpu_ctx_t* ctx);
private:

};

//----------------------------------------------------------------------------
// Native GPU texture bridge
//----------------------------------------------------------------------------
// Binds an application-owned SDL_GPUTexture (for example the color attachment
// of a 3D offscreen FBO) to a vgui_texture.  The texture must have
// SDL_GPU_TEXTUREUSAGE_SAMPLER; vgui only borrows it and never releases it.
// Pass the new SDL_GPUTexture pointer when the FBO is recreated.
vgui_texture* vgui_sdl3_texture_create_native(vgui_context* ui, vgpu_ctx_t* gpu_ctx,
	SDL_GPUTexture* native_texture, int width, int height, vgui_texture_format format);
int vgui_sdl3_texture_update_native(vgui_texture* texture, vgpu_ctx_t* gpu_ctx,
	SDL_GPUTexture* native_texture, int width, int height, vgui_texture_format format);
