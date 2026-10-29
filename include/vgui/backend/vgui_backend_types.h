#pragma once
/*
 * Public event and input types for the vgui SDL3 backend.
 *
 * The SDL3 backend and hosts share these types: gui_viewport::io / trigger()
 * and app_mgr::process_event() use them directly.  This header depends only
 * on the standard library and glm; it does not include vgui internals.
 */
#include <cstdint>

#include <glm/glm.hpp>

#ifndef BIT_INC
#define BIT_INC(x) (1 << x)
#endif

// Window device events.
enum class dev_event_type_e : uint32_t {
	none = 0,
	mouse_move_e,
	mouse_button_e,
	mouse_wheel_e,
	keyboard_e,
	text_editing_e,
	text_input_e,
	finger_e,
	ole_drop_e,
	max_det
};

enum class cursor_st : uint32_t
{
	cursor_null,
	cursor_arrow,
	cursor_ibeam,
	cursor_wait,
	cursor_no,
	cursor_hand,
	cursor_size_h,
	cursor_size_v,
};

struct mouse_move_et
{
	int x, y;			// mouse position
	int xrel, yrel;		// relative motion in the XY direction
	uint8_t which;		// mouse instance
	cursor_st cursor;	// cursor to switch to
};
struct mouse_button_et		// mouse button event
{
	uint8_t which;
	uint8_t button;       // button index: 1 = left, 2 = middle, 3 = right
	uint8_t down;        // ::SDL_PRESSED or ::SDL_RELEASED
	uint8_t clicks;       // 1 for single-click, 2 for double-click, etc.
	int x, y;
};
struct mouse_wheel_et	// mouse wheel event
{
	int x, y;
	uint8_t which;
};
#ifndef KM_CTRL
#define KM_CTRL 1
#define KM_SHIFT 2
#define KM_ALT 4
#define KM_GUI 8
#endif
struct keyboard_et
{
	uint16_t scancode;  // SDL physical key code - see ::SDL_Scancode for details
	uint16_t mod;       // current key modifiers
	int sym;            // SDL virtual key code - see ::SDL_Keycode for details
	int keycode;
	int16_t kmod;		// Control=1 Shift=2 Alt=4 8Cmd/Super/Windows
	int8_t down;       // ::SDL_PRESSED or ::SDL_RELEASED
	int8_t repeat;      // Non-zero if this is a key repeat

};
struct text_editing_et
{
	char* text;// [32 * 8] ;  //The editing text
	int start;		// The start cursor of selected editing text
	int length;		// The length of selected editing text
	int x, y, w, h;
};
struct text_input_et
{
	char* text;  // The input text
	int x, y, w, h;
};
// Touch event.
struct finger_et {
	int tid, touchId;
	float x, y, pressure; // position and pressure
	int t;	// FINGERDOWN=1 UP=2 MOTION=3
};
// OLE drop data; count > 0 when the drop completes.
struct ole_drop_et
{
	const char** str;
	int count;			// > 1 usually means a file list
	int fmt = -1;		// 0 = text, 1 = files
	float x, y;			// mouse position

	int* has;			// accept flag: 0 = reject, 1 = accept

};
struct gui_io_state_t;

struct dev_event_t
{
	union
	{
		struct mouse_move_et* m;
		struct mouse_button_et* b;
		struct mouse_wheel_et* w;
		struct keyboard_et* k;
		struct text_editing_et* e;
		struct text_input_et* t;
		struct finger_et* f;
		struct ole_drop_et* d;
	}v = {};
	gui_io_state_t* io = 0;
	dev_event_type_e type = dev_event_type_e::none;
	uint8_t ret = 0;
};

// GUI input state.
struct gui_io_mk_state {
	bool KeysDown[512];
	bool KeyCtrl : 1;
	bool KeyShift : 1;
	bool KeyAlt : 1;
	bool KeySuper : 1;
};
struct gui_io_state_t
{
	gui_io_mk_state* mks;
	glm::vec2 MouseDelta;
	glm::vec2 MousePos;
	glm::vec2 wheel;
	glm::ivec4* ime_rect = 0;
	bool MouseDown[8] = {};
	float deltaTime = 0.0f;
	int8_t clicks = 0;
	bool WantCaptureMouse : 1;
	bool WantCaptureKey : 1;
	bool WantTextInput : 1;
};
