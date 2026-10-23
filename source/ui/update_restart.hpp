#pragma once

/* Pure decision for "how do we relaunch after replacing the running NRO?".

   Application mode can ask the applet manager to relaunch the current application.  An applet
   launched through hbl has no such application-identity path; it can only ask the loader to chain
   load the next NRO.  Keeping the decision here makes both fallback cases explicit and testable. */

namespace acnh_manager::ui {

enum class RestartRequest {
    None,
    ChainLoad,
    Application,
};

inline RestartRequest DecideUpdateRestart(bool application_mode, bool has_next_load) {
    if (application_mode) {
        return RestartRequest::Application;
    }
    if (has_next_load) {
        return RestartRequest::ChainLoad;
    }
    return RestartRequest::None;
}

}  // namespace acnh_manager::ui
