#pragma once

/* Touch input (handheld only; docked consoles simply report no touches).

   Two rules from the sources we checked:
   - libnx's hidInitializeTouchScreen() aborts the process when activation fails, so it is
     called exactly once, from Init(), and the log records whether the shared memory appeared;
   - HidTouchState.x/y are already screen coordinates (Sphaira uses them as-is for a 1280x720
     framebuffer), so no panel-to-screen mapping is needed.

   The class only reports "where is the finger now"; tap/drag decisions live in the caller,
   which keeps them testable without hardware. */

#include <switch.h>

namespace acnh_manager {
class Log; /* log.hpp */
}

namespace acnh_manager::ui {

class Touch {
public:
    /* Never fatal: on failure the app keeps running with touch disabled and says so in the log. */
    void Init(Log *log);
    void Exit();
    bool Available() const { return m_available; }

    /* Reads the panel.  Returns true when at least one finger is down and fills x/y (screen
       pixels) plus the contact count; returns false when nothing is touching. */
    bool Poll(int *x, int *y, int *count);

private:
    Log *m_log{nullptr};
    bool m_available{false};
};

}  // namespace acnh_manager::ui
