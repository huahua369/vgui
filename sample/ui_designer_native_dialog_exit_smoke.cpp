#include "ui_designer_native_dialog_exit_smoke.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <SDL3/SDL.h>

#include <cwchar>
#include <stdexcept>

namespace vgui::ui_design {
namespace {

BOOL CALLBACK find_file_dialog(HWND window, LPARAM context) {
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    wchar_t name[32]{};
    if (pid != GetCurrentProcessId() || !IsWindowVisible(window) ||
        !GetClassNameW(window, name, 32) || std::wcscmp(name, L"#32770") != 0) return TRUE;
    *reinterpret_cast<HWND*>(context) = window;
    return FALSE;
}

} // namespace

void cancel_native_dialog_and_close(SDL_Window* window, const std::function<void()>& tick,
                                    const std::function<bool()>& chooser_open,
                                    const std::function<bool()>& was_cancelled) {
    // CTest must receive an exit code, not a Windows crash dialog.
    SetErrorMode(GetErrorMode() | SEM_NOGPFAULTERRORBOX);
    const auto main = static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(window),
        SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    if (!main) throw std::runtime_error("Native dialog exit: missing editor HWND");

    const auto wait_for = [&](auto ready, const char* error) {
        const auto deadline = SDL_GetTicks() + 8000;
        while (!ready()) {
            if (SDL_GetTicks() >= deadline) throw std::runtime_error(error);
            tick();
        }
    };
    HWND dialog = nullptr;
    wait_for([&] {
        if (!chooser_open()) throw std::runtime_error("Native dialog exit: chooser ended before showing a window");
        EnumWindows(find_file_dialog, reinterpret_cast<LPARAM>(&dialog));
        return dialog != nullptr;
    }, "Native dialog exit: no visible file dialog");
    if (!PostMessageW(dialog, WM_CLOSE, 0, 0))
        throw std::runtime_error("Native dialog exit: could not cancel file dialog");

    // Quit is intentionally ignored while the chooser is pending. Send the
    // close as soon as the editor consumes the real cancellation, not after a
    // sleep that would mask a shutdown race with background OS threads.
    wait_for([&] { return !chooser_open(); }, "Native dialog exit: cancellation was not delivered");
    if (!was_cancelled()) throw std::runtime_error("Native dialog exit: expected cancellation, not success or error");
    if (!PostMessageW(main, WM_CLOSE, 0, 0))
        throw std::runtime_error("Native dialog exit: could not close the editor window");
}

} // namespace vgui::ui_design
