#pragma once

/* Console text UI (the fallback form).

   Why it exists: in this hbl/applet environment, a homebrew app creating its own
   framebuffer used to crash the loader process (see the crash investigation in
   docs/device-acceptance.md), while the libnx console path was proven usable in M0/M1-A.
   The console UI is what runs by default; the graphics one is opt-in:
   `/switch/ACNH-Manager/ui-graphics` enables it.
   The console font is ASCII 8x8, so the text UI speaks English only; Chinese notes stay in
   log.txt. */
#include <switch.h>

namespace acnh_manager {
class Log;
}

namespace acnh_manager::ui {

class TextUi {
public:
    /* false means the console could not be initialized. */
    bool Init();
    void Exit();
    /* Main loop: B or + exits. */
    void Run(acnh_manager::Log *log, FsFileSystem &sd);
};

}  // namespace acnh_manager::ui
