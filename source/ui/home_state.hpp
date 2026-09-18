#pragma once

/* Which of the seven home-screen states the app is in.

   Pure logic on purpose: this single decision drives the whole home screen (headline, colour,
   primary button), so it is testable without a console.  The app fills the inputs from the
   environment report, the gate verdict, the install plan and the update check. */

namespace acnh_manager::ui {

enum class HomeKind {
    GameMissing,     /* no Animal Crossing on this console, or its info is unreadable */
    Unsupported,     /* the game is here, but this build cannot be handled */
    Failed,          /* the last install / uninstall attempt failed */
    Repair,          /* installed files are missing or no longer match */
    NeedsInstall,    /* ready to install: nothing is installed yet */
    UpdateAvailable, /* installed, and the update check found a newer agent */
    UpToDate,        /* installed and current */
};

struct HomeInputs {
    bool game_found{false};    /* the game is installed and its build was readable */
    bool supported{false};     /* gate verdict is Supported (build matches the manifest) */
    bool last_failed{false};   /* the previous install/uninstall ended in an error */
    bool repair_needed{false}; /* plan says Repair */
    bool fresh_install{false}; /* plan says Install (nothing recorded yet) */
    bool newer_agent{false};   /* update check found a newer agent version */
};

inline HomeKind Classify(const HomeInputs &in) {
    /* A missing game or an unsupported build outranks everything: with those, no amount of
       installing helps, and the user needs to know why before they press anything. */
    if (!in.game_found) {
        return HomeKind::GameMissing;
    }
    if (!in.supported) {
        return HomeKind::Unsupported;
    }
    /* A failure is shown until something succeeds, even if the plan now says "install". */
    if (in.last_failed) {
        return HomeKind::Failed;
    }
    if (in.repair_needed) {
        return HomeKind::Repair;
    }
    if (in.fresh_install) {
        return HomeKind::NeedsInstall;
    }
    return in.newer_agent ? HomeKind::UpdateAvailable : HomeKind::UpToDate;
}

}  // namespace acnh_manager::ui
