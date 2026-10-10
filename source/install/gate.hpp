#pragma once

/* Gate and install decision: pure logic, no libnx dependency, host-testable.

   Verdict chain (measured on real hardware; see docs/architecture.md sections 3 and 5):
     1. ns   title version (DetectedBuild::version)
     2. ncm  update title's Program content id (DetectedBuild::content_id)
     3. dmnt:cht main ModuleId (DetectedBuild::module_id, only while the game runs)
   -> compared item by item against the manifest's games[]; any mismatch fails closed: there
   is no "closest match". */
#include <cstdint>
#include <string>
#include <vector>

#include "manifest/manifest.hpp"

namespace acnh_manager::install {

struct DetectedBuild {
    /* 16 hex digits; empty when no Application content meta for the title is on this console.
       Presence is not the same question as the version: a bare 1.0.0 copy is installed and
       carries title version 0, so `version == 0` must never be read as "no game here". */
    std::string title_id;
    std::uint32_t version{0};
    std::string content_id; /* 32 hex digits; empty when unreadable */
    bool module_id_known{false};
    std::string module_id;  /* 32 hex digits */
};

enum class GateStatus {
    Supported,
    NoManifest,
    TitleMissing,      /* nothing on this console carries the Animal Crossing title id */
    TitleNotSupported,
    VersionNotSupported,
    ContentIdMissing,
    ContentIdMismatch,
    BuildIdMismatch,
};

struct GateResult {
    GateStatus status{GateStatus::NoManifest};
    /* The games[] entry for a title this console has, even when its version does not match (the
       "not supported" case).  The home screen needs it to say what this build *would* accept.
       `game` stays null unless the entry passed every check. */
    const manifest::GameEntry *title_entry{nullptr};
    const manifest::GameEntry *game{nullptr};
    std::string reason; /* diagnostic text; the UI turns it into player-facing wording */
};

/* manifest == nullptr means the app has no usable manifest (which is the M0/M1 state). */
GateResult Evaluate(const manifest::Manifest *manifest, const DetectedBuild &detected);

struct InstalledFile {
    std::string target;
    std::uint64_t size{0};
    std::string sha256;
};

/* In-memory model of state.json (/switch/ACNH-Manager/state.json). */
struct InstallState {
    std::string agent_version;
    std::string agent_commit;
    std::string content_id;
    std::string build_id;
    std::string installed_at;
    std::vector<InstalledFile> files;
};

enum class PlanAction {
    Blocked,  /* gate refused: write nothing */
    Install,  /* fresh install, or a new build / new agent version */
    Repair,   /* the record exists but the files do not match the manifest */
    UpToDate, /* nothing to do, writes are skipped */
};

struct InstallPlan {
    PlanAction action{PlanAction::Blocked};
    const manifest::GameEntry *game{nullptr};
    std::string reason;
};

InstallPlan Plan(const GateResult &gate, const InstallState *state,
                 const manifest::AgentInfo &agent);

/* state.json read/write (string form; the caller owns the file IO). */
std::string DumpState(const InstallState &state);
bool ParseState(std::string_view text, InstallState *out, std::string *error);

}  // namespace acnh_manager::install
