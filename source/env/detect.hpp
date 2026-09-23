#pragma once

/* On-device environment collection (read-only): the numbers the verdict chain needs, plus
   the configuration gotchas a player can hit.  Why these interfaces: docs/architecture.md
   sections 3 and 5 (ns / ncm / dmnt:cht are usable; fsp-ldr is refused). */
#include <switch.h>

#include <cstdint>
#include <string>
#include <vector>

#include "env/config_ini.hpp"
#include "install/gate.hpp"

namespace acnh_manager::env {

inline constexpr u64 kAcnhTitleId = 0x01006F8002326000ull;
inline constexpr u64 kAcnhUpdateTitleId = 0x01006F8002326800ull;
/* Card paths for the title above.  Single source for every module that needs them (detector,
   M0 probe, install engine) -- they used to be literals copied into three files. */
inline constexpr const char *kAcnhExefsDir = "/atmosphere/contents/01006F8002326000/exefs";
inline constexpr const char *kAcnhCheatsDir = "/atmosphere/contents/01006F8002326000/cheats";
inline constexpr const char *kAcnhTitleConfig = "/atmosphere/contents/01006F8002326000/config.ini";
inline constexpr const char *kAcnhOverrideConfig = "/atmosphere/config/override_config.ini";

struct MetaEntry {
    u8 meta_type{0};
    u8 storage{0};
    std::uint32_t version{0};
    u64 application_id{0};
};

struct ExefsFile {
    std::string name;
    std::uint64_t size{0};
};

struct EnvironmentReport {
    /* Runtime environment */
    std::string hos_version; /* e.g. "22.1.0" */
    int applet_type{0};
    bool application_running{false};

    /* Game installation info (sources of verdict 1 and 3) */
    std::vector<MetaEntry> metas;
    bool patch_found{false};
    std::uint8_t patch_storage{0};
    install::DetectedBuild build;

    /* Override configuration (whether a key must be held when launching the game) */
    bool have_override_config{false};
    bool have_title_config{false};
    OverrideAdvice advice;

    /* Current install state */
    std::vector<ExefsFile> exefs;
    bool legacy_cheat_present{false};
    /* The same file as the interface has to name it: the path on the card, not just the file
       name.  The player has to be able to find it without knowing which directory it lives in. */
    std::string legacy_cheat_path;
    /* The entry inside that file which drives the chat-code path; empty when the file could not
       be read, in which case the file is reported without naming an entry. */
    std::string legacy_cheat_entry;

    /* Failure diagnostics: empty means every reading succeeded */
    std::string problems;
};

/* Collect the environment report.  Read-only: writes no file, changes no config. */
EnvironmentReport Collect(FsFileSystem &sd);

/* List a directory's files (name + size); a missing directory yields an empty list. */
std::vector<ExefsFile> ListDirectory(FsFileSystem &sd, const char *path);

}  // namespace acnh_manager::env
