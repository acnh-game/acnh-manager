#pragma once

/* One decision, one place: does the verified release manifest become the source this app
   installs from?

   That question used to be answered inline in the UI ("remote version > installed version"),
   and the answer was wrong in a way that only shows on hardware: a *newer* release whose
   `games[]` no longer lists this console's build was adopted anyway, so the gate then judged
   the console against a manifest that does not cover it.  The home screen said "this game
   version is not supported yet" and the primary button went dead -- while the release compiled
   into the app supported that build perfectly (reproduced on hardware 2026-09-20 with a signed
   manifest that only listed 4.0.0 on a 3.0.3 console).

   Rules, kept deliberately small so they can be pinned by host tests:
     * nothing to compare against (no installed/carried version) -> never adopt;
     * remote <= current -> not newer, nothing to do (equal is "up to date");
     * remote > current and the gate accepts this build -> adopt (this is the update path);
     * remote > current and the gate refuses -> report `unsupported`, keep the bundled release.
       Refusing to *replace* a working release is not the same as writing data onto an unknown
       build; the latter stays blocked by the gate, which `install::Plan()` still enforces. */

#include <string>

#include "install/gate.hpp"
#include "manifest/manifest.hpp"

namespace acnh_manager::net {

struct AdoptionDecision {
    bool newer{false};       /* the remote agent is newer than what this console has */
    bool adopt{false};       /* ...and it covers this build, so it becomes the install source */
    bool unsupported{false}; /* newer, but nothing in games[] covers this build */
};

/* `current` is the version on the card (install record), or the version this build carries when
   nothing is installed. */
inline AdoptionDecision DecideAdoption(const manifest::Manifest &remote,
                                       const install::DetectedBuild &build,
                                       const std::string &current) {
    AdoptionDecision decision;
    if (remote.agent.version.empty() || current.empty()) {
        return decision;
    }
    if (manifest::CompareVersions(remote.agent.version, current) <= 0) {
        return decision;
    }
    decision.newer = true;
    const install::GateResult gate = install::Evaluate(&remote, build);
    if (gate.status == install::GateStatus::Supported) {
        decision.adopt = true;
    } else {
        decision.unsupported = true;
    }
    return decision;
}

}  // namespace acnh_manager::net
