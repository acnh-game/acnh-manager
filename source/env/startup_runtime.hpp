#pragma once

/* The console half of the startup self-check: read the two NROs, run the pure decision, and carry
   out its one action (copy + verify + delete, or delete this copy).  Only this file touches fs* /
   envSetNextLoad; the decision itself lives in startup_check.* and is host-tested. */

#include <switch.h>

#include <string>

#include "env/startup_check.hpp"

namespace acnh_manager {
class Log;
}

namespace acnh_manager::env {

struct StartupPlan {
    StartupDecision decision;
    StartupInputs inputs;
    NroMeta current;
    NroMeta standard;
    std::string current_path;  /* normalized: no sdmc: prefix */
    std::string standard_path{kStandardNroPath};
    std::string startup_error; /* set when an unattended repair failed; the page shows it */
    bool repaired{false};      /* the silent move succeeded but chaining failed: ask for a restart */
};

/* Reads argv[0] and the standard path, decides, and writes the launch line to the log.  `dev`
   exempts development builds entirely (they are expected to run from scratch paths). */
StartupPlan RunStartupCheck(FsFileSystem &sd, const std::string &argv0, bool dev, Log *log);

/* Copies this NRO over the standard path (when the decision says so) and removes this copy; or
   only removes this copy when the standard one is the right one.  Returns true when the card now
   matches the decision -- including "copied, but the source could not be removed" (logged, not
   blocking). */
bool ApplyStartupRepair(FsFileSystem &sd, const StartupPlan &plan, Log *log, std::string *error);

/* envSetNextLoad(standard, standard).  False when the host does not support the convention or the
   call fails; the page then says the repair is done and asks the player to restart. */
bool SetNextLoadToStandard();

}  // namespace acnh_manager::env
