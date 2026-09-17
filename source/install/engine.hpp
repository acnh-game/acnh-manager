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

namespace acnh_manager::install {

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

/* Uninstall: verify each file from state.json by sha256, delete it, then drop the record and
   the now-empty directory. */
InstallResult Uninstall(FsFileSystem &sd, bool dry_run, const ProgressCallback &progress);

}  // namespace acnh_manager::install
