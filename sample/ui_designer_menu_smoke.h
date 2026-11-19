#pragma once

#include <vgui/backend/vgui_sdl3.h>

#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace vgui::ui_design {

// Open real menus by pointer, then select entries through SDL keyboard events.
class native_menu_input {
public:
    native_menu_input(os_window& window, vgui_context* shell, const std::function<void()>& frame)
        : window_(window), shell_(shell), frame_(frame) {}

    vgui_widget* open(int index) const {
        auto* bar = find_bar(vgui_root(shell_));
        if (!bar || index >= vgui_menubar_get_count(bar)) fail("missing menu heading");
        const auto item = vgui_menubar_get_item_at(bar, index);
        float x, y, width, height;
        if (!vgui_menubar_get_item_rect(bar, item, &x, &y, &width, &height)) fail("menu heading has no rect");
        for (bool down : {true, false}) {
            SDL_Event event{};
            event.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
            event.button.windowID = window_.id;
            event.button.button = SDL_BUTTON_LEFT;
            event.button.down = down;
            event.button.clicks = 1;
            event.button.x = (x + width / 2) / window_.pixel_ratio();
            event.button.y = (y + height / 2) / window_.pixel_ratio();
            push(event);
        }
        frame_();
        auto* menu = vgui_menubar_get_menu(bar, item);
        if (!menu || !vgui_menu_is_open(menu)) fail("menu did not open");
        return menu;
    }

    vgui_widget* activate(vgui_widget* menu, int index) const {
        if (!menu || !vgui_menu_is_open(menu)) fail("target menu is not open");
        const int count = vgui_menu_get_count(menu);
        if (index < 0 || index >= count) fail("missing menu entry");
        const auto item = vgui_menu_get_item_at(menu, index);
        if (!vgui_menu_get_item_enabled(menu, item)) fail("menu entry disabled");
        auto* submenu = vgui_menu_get_submenu(menu, item);
        for (int i = 0; i < count && vgui_menu_get_cursor(menu) != item; ++i)
            key(SDLK_DOWN, SDL_SCANCODE_DOWN);
        if (vgui_menu_get_cursor(menu) != item) fail("keyboard could not reach menu entry");
        key(SDLK_RETURN, SDL_SCANCODE_RETURN);
        if (submenu ? !vgui_menu_is_open(submenu) : vgui_context_get_open_menu(shell_) != nullptr)
            fail("activation did not open submenu or close menu");
        return submenu;
    }

    void key(SDL_Keycode code, SDL_Scancode scan) const {
        for (bool down : {true, false}) {
            SDL_Event event{};
            event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
            event.key.windowID = window_.id;
            event.key.key = code;
            event.key.scancode = scan;
            event.key.down = down;
            push(event);
        }
        frame_();
    }

private:
    static vgui_widget* find_bar(vgui_widget* widget) {
        if (vgui_widget_get_kind(widget) == VGUI_MENUBAR &&
            std::string_view(vgui_widget_get_id(widget)) == "designer.menubar") return widget;
        for (std::size_t i = 0; i < vgui_widget_get_child_count(widget); ++i)
            if (auto* result = find_bar(vgui_widget_get_child(widget, i))) return result;
        return nullptr;
    }
    [[noreturn]] static void fail(const char* message) { throw std::runtime_error(std::string("Native menu: ") + message); }
    static void push(SDL_Event event) { if (!SDL_PushEvent(&event)) fail("cannot enqueue SDL event"); }

    os_window& window_;
    vgui_context* shell_;
    const std::function<void()>& frame_;
};

} // namespace vgui::ui_design
