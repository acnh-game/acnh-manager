#pragma once

/* Install / uninstall engine (device side).  The pure logic (gate, state model, SHA-256,
   time formatting) lives elsewhere; this file only does what has to touch the SD card:
   read, verify, write, record.
   On-disk rules (docs/architecture.md section 4): write a temp file first, verify it, then
   rename; every write is preceded by fsFileSetSize; never leave a half-written file. */

#include <switch.h>

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "install/gate.hpp"
#include "manifest/manifest.hpp"

namespace acnh_manager {
class Log;
}

namespace acnh_manager::install {

/* Development switch (`/switch/ACNH-Manager/dev-fsprobe`): route every fs call the installer
   makes to this log, with the path pointer and the memory type/permission of the page it lives
   on.  It exists because the installer could fail with `0xD401` on a path whose directory is
   provably fine, while an identical probe call a moment later succeeded -- so the question is
   which call failed and what the kernel saw at that pointer. */
void SetFsTraceSink(Log *log);

/* Small text file on the card (today: settings.json).  `ReadTextFile` reports "not there" as
   found=false, which is not an error; `WriteTextFile` goes through the same temp-file +
   read-back + rename path as the install record, so a power cut cannot leave half a file.
   They live in this module because the verified write path is what the engine owns -- settings
   are just another small file that must not be left half-written. */
bool ReadTextFile(FsFileSystem &sd, const std::string &path, std::string *out, bool *found,
                  std::string *error);
bool WriteTextFile(FsFileSystem &sd, const std::string &path, const std::string &text,
                   std::string *error);

/* True when every file the install record names is still on the card, byte for byte.

   `Plan()` compares the record against the manifest *only*, so without this the app keeps
   saying "installed" after the game directory is removed or renamed behind its back -- measured
   on hardware: deleting `atmosphere/contents/<title>/exefs` over FTP while the app was closed
   still left the home screen on "already installed".  The first file that does not match is
   described in `error`. */
bool VerifyInstalledFiles(FsFileSystem &sd, const InstallState &state, std::string *error);

/* Payload source: M2 reads a directory on the SD card (for on-device dry runs); the release
   path embeds the payload in the NRO instead. */
class PayloadSource {
public:
    virtual ~PayloadSource() = default;
    virtual bool Read(const std::string &name, std::vector<u8> *out, std::string *error) = 0;
};

class SdFolderPayloadSource final : public PayloadSource {
public:
    explicit SdFolderPayloadSource(FsFileSystem &sd,
                                   std::string dir = "/switch/ACNH-Manager/payload")
        : m_sd(&sd), m_dir(std::move(dir)) {}
    bool Read(const std::string &name, std::vector<u8> *out, std::string *error) override;

private:
    FsFileSystem *m_sd;
    std::string m_dir;
};

struct Progress {
    std::string step;
    int index{0};
    int total{0};
};

using ProgressCallback = std::function<void(const Progress &)>;

struct InstallResult {
    bool ok{false};
    std::string error;
    InstallState state;
    int files_written{0};
    bool dry_run{false};
};

/* Basic file operations (shared with the UI and later milestones). */
/* fs* requires absolute paths starting with '/'; the manifest target is relative, so every
   path goes through here before it reaches fs*. */
std::string AbsolutePath(std::string_view path);
bool EnsureDirectory(FsFileSystem &sd, const std::string &path, std::string *error);
bool ReadWholeFile(FsFileSystem &sd, const std::string &path, std::vector<u8> *out,
                   std::string *error);
bool HashFile(FsFileSystem &sd, const std::string &path, std::string *hex, std::string *error);
/* Write a file and read it back: temp file -> SetSize -> write -> read-back sha256 -> rename. */
bool WriteFileVerified(FsFileSystem &sd, const std::string &path, const std::vector<u8> &data,
                       const std::string &sha256, std::string *error);

bool ReadStateFile(FsFileSystem &sd, InstallState *state, bool *found, std::string *error);
bool WriteStateFile(FsFileSystem &sd, const InstallState &state, std::string *error);

/* Read a manifest file (the SD dev manifest during M2; the embedded one on the release path).
   found=false means the file is absent, which is not an error. */
bool ReadManifestFile(FsFileSystem &sd, const std::string &path, std::string_view app_version,
                      bool require_release_build, manifest::Manifest *out, bool *found,
                      std::string *error);

/* Full install.  dry_run=true verifies and reports only; nothing is written. */
InstallResult Install(FsFileSystem &sd, const manifest::Manifest &manifest,
                      const manifest::GameEntry &game, PayloadSource &source, bool dry_run,
                      const ProgressCallback &progress);

/* Uninstall: delete every file the record names (by path, no hash check -- a file the player
   edited by hand must still be removable), then drop the record and the now-empty directory.
   All entries are attempted; whatever could not be deleted is reported. */
InstallResult Uninstall(FsFileSystem &sd, bool dry_run, const ProgressCallback &progress);

/* Remove files a previous run left behind after a crash (the `<target>.acnh-tmp` /
   `<target>.acnh-old` siblings of a two-phase install).  `dir` is the manifest's target
   directory; the names that were removed are appended to `removed` for the log.  Returns how
   many files were deleted; a missing directory simply means "nothing to clean". */
int CleanLeftovers(FsFileSystem &sd, const std::string &dir,
                   std::vector<std::string> *removed);

/* True when an install/uninstall error carries the `0xD401` signature (`InvalidMemoryState`).

   The one cause we hit was a path buffer sitting near the end of a mapping; that is fixed in
   `util::FsPath` (see `docs/architecture.md` 4.2), and the replay of the old failure is in
   `docs/device-acceptance.md`.  The check stays as a safety net: if the signature ever shows up
   again the result page names the one action that has always worked, "quit and reopen the
   manager", instead of leaving a raw error code on screen. */
bool IsStaleSessionError(const std::string &error);

}  // namespace acnh_manager::install
