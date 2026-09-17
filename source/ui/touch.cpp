#include "touch.hpp"

#include "log.hpp"

namespace acnh_manager::ui {

void Touch::Init(Log *log) {
    m_log = log;
    if (m_available) {
        return;
    }
    /* Activates the touchscreen through hid:sys.  The libnx wrapper aborts on failure, so it
       is only ever called here; everything after this point checks the shared memory instead. */
    hidInitializeTouchScreen();
    m_available = hidGetSharedmemAddr() != nullptr;
    if (m_log != nullptr) {
        m_log->Line("touch: hidInitializeTouchScreen done, shared memory %s",
                    m_available ? "ready" : "missing (touch disabled)");
        m_log->Sync();
    }
}

void Touch::Exit() {
    if (!m_available) {
        return;
    }
    /* hidExit() also closes the npad/keyboard half of the service; the app uses hid only for
       touch, so leaving the service open until process exit is the safer choice. */
    m_available = false;
}

bool Touch::Poll(int *x, int *y, int *count) {
    if (!m_available || hidGetSharedmemAddr() == nullptr) {
        return false;
    }
    HidTouchScreenState state{};
    if (hidGetTouchScreenStates(&state, 1) == 0 || state.count <= 0) {
        return false;
    }
    const HidTouchState &first = state.touches[0];
    if (x != nullptr) {
        *x = static_cast<int>(first.x);
    }
    if (y != nullptr) {
        *y = static_cast<int>(first.y);
    }
    if (count != nullptr) {
        *count = state.count;
    }
    return true;
}

}  // namespace acnh_manager::ui
