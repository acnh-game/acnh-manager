#pragma once

/* Which of the eight home-screen states the app is in.

   Pure logic on purpose: this single decision drives the whole home screen (headline, colour,
   primary button), so it is testable without a console.  The app fills the inputs from the
   environment report, the gate verdict, the install plan and the update check. */

#include <vector>

#include "ui/action.hpp"

namespace acnh_manager::ui {

enum class HomeKind {
    GameMissing,     /* no Animal Crossing on this console, or its info is unreadable */
    Unsupported,     /* the game is here, but this build cannot be handled */
    Failed,          /* the last install / uninstall attempt failed */
    Incomplete,      /* the record says installed, but the files are not (all) on the card */
    Repair,          /* installed files are missing or no longer match */
    NeedsInstall,    /* ready to install: nothing is installed yet */
    UpdateAvailable, /* installed, and the update check found a newer agent */
    UpToDate,        /* installed and current */
};

struct HomeInputs {
    /* The console reports the game's Application content meta.  A bare 1.0.0 copy counts -- its
       title version is 0, which is a version, so presence must never be derived from it. */
    bool game_found{false};
    bool supported{false};     /* gate verdict is Supported (build matches the manifest) */
    bool last_failed{false};   /* the previous install/uninstall ended in an error */
    bool files_incomplete{false}; /* the card no longer matches the install record */
    bool repair_needed{false};    /* plan says Repair */
    bool fresh_install{false};    /* plan says Install (nothing recorded yet) */
    bool newer_agent{false};      /* update check found a newer agent version */
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
    /* The card disagreeing with the record outranks the record's own verdict: it is the one
       case where "installed" would be a lie (the player deleted or renamed the game directory
       behind the app's back). */
    if (in.files_incomplete) {
        return HomeKind::Incomplete;
    }
    if (in.repair_needed) {
        return HomeKind::Repair;
    }
    if (in.fresh_install) {
        return HomeKind::NeedsInstall;
    }
    return in.newer_agent ? HomeKind::UpdateAvailable : HomeKind::UpToDate;
}

/* A HOME-menu shortcut launches this NRO as a full-memory Application.  ncm/ns cannot remove
   the title while that process is the title being removed, so the UI must offer the action only
   from album/applet mode. */
inline bool ForwarderRemovalBlocked(bool installed, bool application_mode) {
    return installed && application_mode;
}

/* Home always has one focused button.  Its controls are a wide primary button, an optional pair
   in one row, then a wide shortcut button; spatial movement cannot visit both members of that
   pair reliably, so Up/Down cycle the vector's visual order instead. */
inline int MoveHomeFocus(const std::vector<Action> &actions, int current, int direction,
                         int anchor_id) {
    if (actions.empty() || direction == 0) {
        return -1;
    }
    int start = -1;
    if (current >= 0) {
        for (std::size_t i = 0; i < actions.size(); ++i) {
            if (actions[i].id == current) {
                start = static_cast<int>(i);
                break;
            }
        }
    }
    if (start < 0) {
        for (std::size_t i = 0; i < actions.size(); ++i) {
            if (actions[i].id == anchor_id) {
                start = static_cast<int>(i);
                break;
            }
        }
    }
    if (start < 0) {
        return -1;
    }
    const int step = direction > 0 ? 1 : -1;
    const int count = static_cast<int>(actions.size());
    for (int offset = 1; offset <= count; ++offset) {
        const int raw = start + step * offset;
        const int index = ((raw % count) + count) % count;
        if (actions[static_cast<std::size_t>(index)].enabled) {
            return actions[static_cast<std::size_t>(index)].id;
        }
    }
    return -1;
}

/* The primary action is Home's initial focus when it is enabled.  If it is disabled (for
   example, "already up to date"), start on the next enabled control instead. */
inline int DefaultHomeFocus(const std::vector<Action> &actions, int anchor_id) {
    for (const Action &action : actions) {
        if (action.id == anchor_id && action.enabled) {
            return action.id;
        }
    }
    return MoveHomeFocus(actions, -1, +1, anchor_id);
}

}  // namespace acnh_manager::ui
