#pragma once

/* The startup self-check: where this NRO is running from, whether the standard copy is ours and
   newer, and what the one repair action has to do about it.

   Everything here is pure: NRO/NACP bytes in, a decision out.  The card reader, the file copy and
   `envSetNextLoad` live in env/startup_runtime.*, which is the only half that needs the console.
   Keeping the decision pure is what lets the host tests pin all five rows of the table in
   docs/home_menu_and_startup_check_design.md. */

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace acnh_manager::env {

/* The path the home-menu tile and the updater both point at. */
inline constexpr const char *kStandardNroPath = "/switch/ACNH-Manager/acnh-manager.nro";

struct NroMeta {
    bool ok{false};
    std::string error;
    std::string name;
    std::string author;
    std::string version; /* NACP DisplayVersion, the string the album shows */
    std::uint64_t size{0};
    std::uint64_t mtime{0};
    std::string sha256;
};

/* Reads the NRO header, the ASET header and the NACP out of an image that starts at the file's
   first byte.  `size` is the file size; the parser never reads past it. */
bool ParseNroMeta(const std::uint8_t *data, std::size_t size, NroMeta *out, std::string *error);

/* Name and author are the identity: another homebrew at the standard path is not "ours", even if
   its NACP version looks plausible. */
bool NroLooksOurs(const NroMeta &meta);

/* Path equality as the filesystem sees it: FAT32 is case-insensitive, and hbmenu can hand over a
   cached spelling that differs from the on-card one ("ACNH-Manager.nro" vs "acnh-manager.nro"). */
bool SameNroPath(std::string_view a, std::string_view b);

/* The one definition of "running from the standard location": FAT32 path matching, not byte
   equality.  The startup check and the details page must use the same rule. */
inline bool IsStandardNroPath(std::string_view path) {
    return SameNroPath(path, kStandardNroPath);
}

/* "1.2.3" against "1.2.4" -> negative/zero/positive.  Missing components count as zero; a string
   with no digits compares as 0.0.0. */
int CompareVersions(std::string_view a, std::string_view b);

enum class StartupAction {
    None,         /* nothing to do: already at the standard path */
    CopyCurrent,  /* copy this NRO over the standard one, then remove this one */
    DeleteCurrent,/* keep the standard one, remove this NRO */
    ReportOnly,   /* cannot establish a path: show the reason, change nothing */
};

/* Everything the decision needs; the runtime fills it from the two files. */
struct StartupInputs {
    bool dev{false};             /* ACNH_DEV_AIDS: development builds are exempt */
    std::string current_path;    /* argv[0] as the loader passed it */
    bool standard_present{false};
    bool standard_readable{false};
    bool standard_ours{false};
    std::string standard_version;
    bool current_readable{false};
    std::string current_version;
    bool same_build{false};           /* size and sha256 both equal */
    bool current_mtime_newer{false};
};

struct StartupDecision {
    bool show_page{false};
    bool silent{false}; /* standard missing: move without a page */
    StartupAction action{StartupAction::None};
    std::string reason; /* stable code for the log and the tests */
};

StartupDecision DecideStartup(const StartupInputs &in);

}  // namespace acnh_manager::env
