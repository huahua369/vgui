#pragma once
/*
 * vgui SDL3 后端的公共事件与输入类型。
 *
 * 这些类型由 SDL3 后端和宿主共享：gui_viewport::io / trigger() 以及
 * app_mgr::process_event() 都直接使用它们。头文件只依赖标准库和 glm，
 * 不包含 vgui 的内部实现头。
 */
#include <cstdint>

#include <glm/glm.hpp>

#ifndef BIT_INC
#define BIT_INC(x) (1 << x)
#endif

// 窗口设备事件
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
	int x, y;			// 鼠标移动坐标
	int xrel, yrel;		// The relative motion in the XY direction 
	uint8_t which;		// 鼠标实例 
	cursor_st cursor;	// 切换鼠标光标用
};
struct mouse_button_et		// 鼠标弹起
{
	uint8_t which;
	uint8_t button;       // The mouse button index 1左，3右，2中
	uint8_t down;        // ::SDL_PRESSED or ::SDL_RELEASED 
	uint8_t clicks;       // 1 for single-click, 2 for double-click, etc. 
	int x, y;
};
struct mouse_wheel_et	// 滚轮消息
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
// touch事件结构体
struct finger_et {
	int tid, touchId;
	float x, y, pressure; // 坐标、压力
	int t;	// FINGERDOWN=1 UP=2 MOTION=3 
};
// 接收ole拖动，结束数据count大于0
struct ole_drop_et
{
	const char** str;
	int count;			// 大于1一般是文件列表
	int fmt = -1;		// 0文本，1文件
	float x, y;			// 鼠标坐标

	int* has;			// 是否接收, 0不接收，1接收

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

// gui事件管理
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
