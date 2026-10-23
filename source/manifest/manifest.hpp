#pragma once

#include <cstdint>

/* Parsing and validation of the release manifest (agent-manifest.json).
   Pure C++17, no libnx dependency, so the host tests can exercise it directly.

   Contract highlights (full text: ../../../docs/acnh_manager_plan.md section 6):
   - schema must equal kSchemaVersion;
   - agent.dirty must be false and agent.buildFlags must equal kReleaseBuildFlags (semantic
     hook bit only); anything else is "not a release artifact" and is refused;
   - every file: target must be a safe path relative to sdmc:/, size > 0, sha256 is 64 hex
     digits;
   - in strict mode (the default) a single mismatch rejects the whole manifest: no "skip
     the bad entry and carry on". */
#include <string>
#include <string_view>
#include <vector>

namespace acnh_manager::manifest {

inline constexpr int kSchemaVersion = 1;
/* Semantic hook bit (bit1); a release build must not carry DEV / RPC server / anything else. */
inline constexpr std::uint32_t kReleaseBuildFlags = 1u << 1;
/* Hard ceiling for the self-update NRO loaded into memory by the worker. */
inline constexpr std::uint64_t kManagerNroMaxBytes = 32ull * 1024 * 1024;

struct FileEntry {
    std::string name;
    std::string source;   /* file name relative to baseUrl */
    std::string target;   /* path relative to sdmc:/ */
    std::uint64_t size{0};
    std::string sha256;   /* 64 hex digits */
    std::string restart;  /* none | game | console */
};

struct GameEntry {
    std::string profile;
    std::string title_id;        /* 16 hex digits, e.g. 01006F8002326000 */
    std::uint32_t version{0};    /* title version, e.g. 2228224 */
    std::string display_version; /* human readable, e.g. 3.0.3 */
    std::string content_id;      /* update title's Program NCA content id (32 hex digits) */
    std::string build_id;        /* main module ModuleId (32 hex digits), optional but recommended */
    std::vector<FileEntry> files;
};

struct AgentInfo {
    std::string version;
    std::string commit;
    std::uint32_t build_flags{0};
    bool dirty{true};
};

/* The self-update half of the same signed release manifest.  The remote manifest may carry it;
   an embedded manifest omits it because the NRO cannot contain its own future hash. */
struct ManagerRelease {
    bool present{false};
    std::string version;
    std::string url;
    std::uint64_t size{0};
    std::string sha256;
    std::string changelog;
};

struct Manifest {
    int schema{0};
    std::string channel;
    std::string generated;
    std::string base_url;
    std::string changelog;
    std::string app_min_version;
    AgentInfo agent;
    ManagerRelease manager;
    std::vector<GameEntry> games;
};

struct ParseResult {
    bool ok{false};
    std::string error;
    Manifest manifest;
};

struct ParseOptions {
    /* The release path requires dirty=false and only the semantic hook bit; a dev manifest
       may relax both (the caller then has to label it a "dev manifest" in the UI). */
    bool require_release_build{true};
};

/* app_version is dotted decimal (e.g. "0.1.0"); it validates app.minVersion. */
ParseResult Parse(std::string_view text, std::string_view app_version,
                  ParseOptions options = ParseOptions{});

/* target safety check: non-empty, no leading '/', no '\\', no ".." component. */
bool IsSafeTarget(std::string_view target);
bool IsHex(std::string_view text, std::size_t length);
bool IsSha256Hex(std::string_view text);

/* Version comparison: returns <0 / 0 / >0 (component-wise, missing components are 0). */
int CompareVersions(std::string_view lhs, std::string_view rhs);

}  // namespace acnh_manager::manifest
