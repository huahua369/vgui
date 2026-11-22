#pragma once

#include <functional>

struct SDL_Window;

namespace vgui::ui_design {

// Drive the real Windows chooser without keyboard focus or global input. The
// supplied tick is the same SDL event pump used by the interactive editor.
void cancel_native_dialog_and_close(SDL_Window* window, const std::function<void()>& tick,
                                    const std::function<bool()>& chooser_open,
                                    const std::function<bool()>& was_cancelled);

} // namespace vgui::ui_design
