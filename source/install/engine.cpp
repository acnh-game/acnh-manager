#include "engine.hpp"

#include <cstdio>
#include <utility>

#include "env/detect.hpp"
#include "i18n/strings.hpp"
#include "log.hpp"
#include "util/fs_path.hpp"
#include "util/sha256.hpp"
#include "util/time.hpp"

namespace acnh_manager::install {
namespace {

using util::FsPath;

constexpr const char *kStatePath = "/switch/ACNH-Manager/state.json";
constexpr const char *kTempSuffix = ".acnh-tmp";
constexpr const char *kOldSuffix = ".acnh-old";
constexpr std::size_t kChunkSize = 64 * 1024;

/* fs-call tracing: see SetFsTraceSink in the header. */
Log *g_fs_trace = nullptr;

Result TraceFs(const char *what, const char *path, Result rc) {
    if (g_fs_trace != nullptr) {
        MemoryInfo info{};
        u32 page = 0;
        const u64 addr = reinterpret_cast<u64>(path);
        svcQueryMemory(&info, &page, addr & ~0xFFFULL);
        MemoryInfo end{};
        u32 end_page = 0;
        const u64 last = (addr + FS_MAX_PATH - 1) & ~0xFFFULL;
        const Result rc_end = svcQueryMemory(&end, &end_page, last);
        g_fs_trace->Line("fsop: %s rc=0x%08X ptr=0x%llX region=0x%llX+0x%llX type=0x%X perm=0x%X",
                         what, rc,
                         static_cast<unsigned long long>(addr),
                         static_cast<unsigned long long>(info.addr),
                         static_cast<unsigned long long>(info.size),
                         static_cast<unsigned>(info.type),
                         static_cast<unsigned>(info.perm));
        g_fs_trace->Line("fsop:   window=0x%llX..0x%llX region_end=0x%llX last_page=0x%llX "
                         "rc=0x%08X type=0x%X",
                         static_cast<unsigned long long>(addr),
                         static_cast<unsigned long long>(addr + FS_MAX_PATH),
                         static_cast<unsigned long long>(info.addr + info.size),
                         static_cast<unsigned long long>(last), rc_end,
                         static_cast<unsigned>(end.type));
    }
    return rc;
}

std::string Describe(const std::string &what, Result rc) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%s rc=0x%08X", what.c_str(), rc);
    return buf;
}

std::string Upper(std::string text) {
    for (char &c : text) {
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
    }
    return text;
}

bool DeleteIfPresent(FsFileSystem &sd, const std::string &path) {
    const std::string absolute = AbsolutePath(path);
    const FsPath arg(absolute);
    const Result rc = TraceFs("delete", arg.c_str(), fsFsDeleteFile(&sd, arg.c_str()));
    return R_SUCCEEDED(rc) || rc == 0x00000202; /* 202 = PathNotFound */
}

}  // namespace

void SetFsTraceSink(Log *log) {
    g_fs_trace = log;
}

/* Every path handed to fs* starts with '/': the manifest target is relative ("atmosphere/...",
   and manifest::IsSafeTarget guarantees there is no leading '/'), and FS refuses a relative
   path (measured rc=0x2EEA02 on create).  This is the single place that adds the prefix. */
std::string AbsolutePath(std::string_view path) {
    if (!path.empty() && path.front() == '/') {
        return std::string(path);
    }
    return "/" + std::string(path);
}

/* True when the path is an existing directory.  Opening it is the only answer we trust on
   the console's filesystem (see the mkdir note below). */
bool DirectoryExists(FsFileSystem &sd, const std::string &path) {
    FsDir dir{};
    const FsPath arg(path);
    /* ReadDirs is what makes the service treat this as a directory listing.  Asking for files
       only made the open fail on some directories, so the caller concluded "not there" and an
       existing directory turned into a bogus mkdir failure (measured on /switch/ACNH-Manager:
       "writing state.json failed: mkdir /switch/ACNH-Manager rc=0x0000D401" even though it
       exists). */
    if (R_FAILED(TraceFs("opendir", arg.c_str(),
                         fsFsOpenDirectory(&sd, arg.c_str(),
                                           FsDirOpenMode_ReadDirs | FsDirOpenMode_ReadFiles,
                                           &dir)))) {
        return false;
    }
    fsDirClose(&dir);
    return true;
}

bool EnsureDirectory(FsFileSystem &sd, const std::string &path, std::string *error) {
    const std::string absolute = AbsolutePath(path);
    std::string current;
    std::size_t start = 0;
    while (start <= absolute.size()) {
        const std::size_t slash = absolute.find('/', start);
        const std::string segment =
            slash == std::string::npos ? absolute.substr(start)
                                       : absolute.substr(start, slash - start);
        if (!segment.empty()) {
            current += "/" + segment;
            const FsPath arg(current);
            const Result rc = TraceFs("mkdir", arg.c_str(),
                                      fsFsCreateDirectory(&sd, arg.c_str()));
            /* "It is already there" is not a failure, and fs does not report it with one
               single code: 0x402 is PathAlreadyExists, but an existing SD path has also come
               back as 0xD401 on hardware (which used to abort every install after an
               uninstall had removed the directory).  So ask the filesystem whether the
               directory is really there instead of trusting the error code. */
            if (R_FAILED(rc) && rc != 0x00000402 &&
                !DirectoryExists(sd, current)) {
                if (error != nullptr) {
                    /* Say what was actually wrong: the path could not be created *and* it is not
                       an openable directory.  A bare "mkdir <path>" reads like "the directory is
                       missing", which sent us chasing the wrong thing once. */
                    *error = Describe("mkdir " + current + " (not usable as a directory)", rc);
                }
                return false;
            }
        }
        if (slash == std::string::npos) {
            break;
        }
        start = slash + 1;
    }
    return true;
}

bool ReadWholeFile(FsFileSystem &sd, const std::string &path, std::vector<u8> *out,
                   std::string *error) {
    const std::string absolute = AbsolutePath(path);
    FsFile file{};
    const FsPath arg(absolute);
    Result rc = TraceFs("read.open", arg.c_str(),
                        fsFsOpenFile(&sd, arg.c_str(), FsOpenMode_Read, &file));
    if (R_FAILED(rc)) {
        if (error != nullptr) {
            *error = Describe("open " + absolute, rc);
        }
        return false;
    }
    s64 size = 0;
    rc = fsFileGetSize(&file, &size);
    if (R_FAILED(rc) || size < 0) {
        fsFileClose(&file);
        if (error != nullptr) {
            *error = Describe("size " + path, rc);
        }
        return false;
    }
    out->assign(static_cast<std::size_t>(size), 0);
    u64 read = 0;
    rc = size > 0 ? fsFileRead(&file, 0, out->data(), static_cast<u64>(size), 0, &read) : 0;
    fsFileClose(&file);
    if (R_FAILED(rc) || read != static_cast<u64>(size)) {
        if (error != nullptr) {
            *error = Describe("read " + path, R_FAILED(rc) ? rc : 0xFFFFFFFF);
        }
        return false;
    }
    return true;
}

bool HashFile(FsFileSystem &sd, const std::string &path, std::string *hex, std::string *error) {
    const std::string absolute = AbsolutePath(path);
    FsFile file{};
    const FsPath arg(absolute);
    Result rc = TraceFs("hash.open", arg.c_str(),
                        fsFsOpenFile(&sd, arg.c_str(), FsOpenMode_Read, &file));
    if (R_FAILED(rc)) {
        if (error != nullptr) {
            *error = Describe("open " + absolute, rc);
        }
        return false;
    }
    util::Sha256 hash;
    std::vector<u8> buffer(kChunkSize);
    s64 offset = 0;
    while (true) {
        u64 read = 0;
        rc = fsFileRead(&file, offset, buffer.data(), buffer.size(), 0, &read);
        if (R_FAILED(rc)) {
            fsFileClose(&file);
            if (error != nullptr) {
                *error = Describe("read " + path, rc);
            }
            return false;
        }
        if (read == 0) {
            break;
        }
        hash.Update(buffer.data(), static_cast<std::size_t>(read));
        offset += static_cast<s64>(read);
    }
    fsFileClose(&file);
    if (hex != nullptr) {
        *hex = util::Sha256::ToHex(hash.Finish());
    }
    return true;
}

bool FileExists(FsFileSystem &sd, const std::string &path) {
    FsFile file{};
    /* Normalise here rather than trusting callers: the install record stores *relative*
       targets ("atmosphere/..."), and fs refuses a relative path with 0x2EEA02 -- which reads
       like "the file is not there" and made the first version of the card-vs-record check
       report every healthy install as incomplete. */
    const FsPath arg(AbsolutePath(path));
    if (R_FAILED(TraceFs("open-exists", arg.c_str(),
                         fsFsOpenFile(&sd, arg.c_str(), FsOpenMode_Read, &file)))) {
        return false;
    }
    fsFileClose(&file);
    return true;
}

/* Put a backup back where it came from.

   A rename cannot land on a path that already exists (FAT), and by the time a rollback runs the
   file we wrote is sitting exactly there.  Renaming first therefore fails silently and leaves
   the new file installed *and* the backup behind as junk -- measured on hardware: injecting a
   state-record write failure left three `.acnh-old` files behind while the app reported the
   install as failed.  The file in the way is removed first, and a backup is only trusted once
   it has actually been seen. */
bool RestoreBackup(FsFileSystem &sd, const std::string &target, const std::string &backup) {
    if (!FileExists(sd, backup)) {
        return false;
    }
    DeleteIfPresent(sd, target);
    const FsPath from(backup);
    const FsPath to(target);
    return R_SUCCEEDED(fsFsRenameFile(&sd, from.c_str(), to.c_str()));
}

/* Phase one: everything that can be done without touching the files that are already
   installed.  Writes <target>.acnh-tmp and reads it back. */
bool PrepareFile(FsFileSystem &sd, const std::string &path, const std::vector<u8> &data,
                 const std::string &sha256, std::string *error) {
    const std::string target = AbsolutePath(path);
    const std::string temp = target + kTempSuffix;
    if (!EnsureDirectory(sd, target.substr(0, target.find_last_of('/')), error)) {
        return false;
    }
    DeleteIfPresent(sd, temp);
    const FsPath arg(temp);
    Result rc = TraceFs("prepare.create", arg.c_str(),
                        fsFsCreateFile(&sd, arg.c_str(), static_cast<s64>(data.size()), 0));
    if (R_FAILED(rc)) {
        /* A stale leftover can sit at the temp path (an interrupted install).  If it is a
           directory, "delete file" cannot remove it and create keeps failing with 0xD401 --
           that is exactly how an install got permanently stuck on hardware. */
        fsFsDeleteDirectory(&sd, arg.c_str());
        DeleteIfPresent(sd, temp);
        rc = TraceFs("prepare.create-retry", arg.c_str(),
                     fsFsCreateFile(&sd, arg.c_str(), static_cast<s64>(data.size()), 0));
    }
    if (R_FAILED(rc)) {
        if (error != nullptr) {
            *error = Describe("create " + temp, rc);
        }
        return false;
    }
    FsFile file{};
    rc = TraceFs("prepare.open-write", arg.c_str(),
                 fsFsOpenFile(&sd, arg.c_str(), FsOpenMode_Write, &file));
    if (R_SUCCEEDED(rc)) {
        rc = fsFileSetSize(&file, static_cast<s64>(data.size()));
    }
    if (R_SUCCEEDED(rc) && !data.empty()) {
        rc = fsFileWrite(&file, 0, data.data(), static_cast<u64>(data.size()), FsWriteOption_Flush);
        if (g_fs_trace != nullptr) {
            g_fs_trace->Line("fsop: prepare.write rc=0x%08X bytes=%zu buf=0x%llX", rc, data.size(),
                             reinterpret_cast<unsigned long long>(data.data()));
        }
    }
    if (R_SUCCEEDED(rc)) {
        rc = fsFileFlush(&file);
    }
    fsFileClose(&file);
    if (R_FAILED(rc) && !data.empty()) {
        if (error != nullptr) {
            *error = Describe("write " + temp, rc);
        }
        DeleteIfPresent(sd, temp);
        return false;
    }

    /* Read back: only replace the old file once what is on the card matches. */
    std::string actual;
    if (!HashFile(sd, temp, &actual, error)) {
        DeleteIfPresent(sd, temp);
        return false;
    }
    if (!sha256.empty() && Upper(actual) != Upper(sha256)) {
        if (error != nullptr) {
            *error = "verification failed for " + target + ": got " + actual;
        }
        DeleteIfPresent(sd, temp);
        return false;
    }
    return true;
}

/* Phase two: move the installed file aside, then move the prepared one in.  `*had_backup`
   tells the caller how to roll this file back.  fs reports "cannot do that" in more than one
   way -- 0x0202 when the source is missing, and 0xD401 on hardware for paths that exist -- so
   the filesystem is asked what actually happened instead of guessing from the code. */
bool CommitFile(FsFileSystem &sd, const std::string &path, const std::string &sha256,
                bool *had_backup, std::string *error) {
    const std::string target = AbsolutePath(path);
    const std::string temp = target + kTempSuffix;
    const std::string backup = target + kOldSuffix;
    const FsPath arg_target(target);
    const FsPath arg_temp(temp);
    const FsPath arg_backup(backup);
    DeleteIfPresent(sd, backup);
    *had_backup = false;
    if (FileExists(sd, target)) {
        const Result rc_backup =
            TraceFs("commit.rename-out", arg_target.c_str(),
                    fsFsRenameFile(&sd, arg_target.c_str(), arg_backup.c_str()));
        if (R_FAILED(rc_backup) && FileExists(sd, target)) {
            if (error != nullptr) {
                *error = Describe("backup " + target + " (the installed file is still there, so "
                                  "the backup did not take effect)", rc_backup);
            }
            return false;
        }
        *had_backup = FileExists(sd, backup);
    }
    const Result rc = TraceFs("commit.rename-in", arg_target.c_str(),
                              fsFsRenameFile(&sd, arg_temp.c_str(), arg_target.c_str()));
    if (R_FAILED(rc)) {
        std::string actual;
        const bool in_place = !sha256.empty() && HashFile(sd, target, &actual, nullptr) &&
                              Upper(actual) == Upper(sha256);
        if (in_place) {
            DeleteIfPresent(sd, temp);
            return true;
        }
        if (error != nullptr) {
            *error = Describe("rename into " + target + " (the file in place is not the one we "
                              "verified)", rc);
        }
        if (*had_backup) {
            RestoreBackup(sd, target, backup);
        }
        DeleteIfPresent(sd, temp);
        return false;
    }
    return true;
}

/* Undo a file that phase two already committed. */
void RollbackFile(FsFileSystem &sd, const std::string &path, bool had_backup) {
    const std::string target = AbsolutePath(path);
    if (had_backup && RestoreBackup(sd, target, target + kOldSuffix)) {
        return;
    }
    /* Nothing to restore (a file that was not installed before): the file we wrote has to go,
       otherwise a failed install would leave an agent behind that no record describes. */
    DeleteIfPresent(sd, target);
}

void DropBackups(FsFileSystem &sd, const std::string &path) {
    DeleteIfPresent(sd, AbsolutePath(path) + kOldSuffix);
}

/* 0xD401 is the kernel's InvalidMemoryState: the call is accepted but refused for the current
   state of the memory or the target it names (see docs/architecture.md 2.1 and 4.2).  Kept as
   the result page's safety net; see engine.hpp. */
bool IsStaleSessionError(const std::string &error) {
    return error.find("0x0000D401") != std::string::npos;
}

/* Single-file convenience for the state record: prepare + commit + drop the backup. */
bool WriteFileVerified(FsFileSystem &sd, const std::string &path, const std::vector<u8> &data,
                       const std::string &sha256, std::string *error) {
    if (!PrepareFile(sd, path, data, sha256, error)) {
        return false;
    }
    bool had_backup = false;
    if (!CommitFile(sd, path, sha256, &had_backup, error)) {
        return false;
    }
    DropBackups(sd, path);
    return true;
}

int CleanLeftovers(FsFileSystem &sd, const std::string &dir,
                   std::vector<std::string> *removed) {
    const std::string absolute = AbsolutePath(dir);
    FsDir handle{};
    const FsPath arg_dir(absolute);
    if (R_FAILED(fsFsOpenDirectory(&sd, arg_dir.c_str(),
                                   FsDirOpenMode_ReadDirs | FsDirOpenMode_ReadFiles, &handle))) {
        return 0; /* not installed (or unreadable): nothing to clean */
    }
    int count = 0;
    FsDirectoryEntry entry{};
    s64 read = 0;
    while (R_SUCCEEDED(fsDirRead(&handle, &read, 1, &entry)) && read > 0) {
        const std::string name = entry.name;
        /* Match the family rather than each suffix constant: a leftover is any sibling we
           created, and missing one because a suffix was renamed would be silent. */
        if (name.find(".acnh-") == std::string::npos) {
            continue;
        }
        const std::string path = absolute + "/" + name;
        const FsPath arg(path);
        /* Same two-step delete as PrepareFile: a leftover can be a directory (an interrupted
           run did that to us), and "delete file" alone cannot remove it. */
        bool gone = R_SUCCEEDED(fsFsDeleteFile(&sd, arg.c_str()));
        if (!gone) {
            gone = R_SUCCEEDED(fsFsDeleteDirectory(&sd, arg.c_str()));
        }
        if (gone) {
            ++count;
            if (removed != nullptr) {
                removed->push_back(name);
            }
        } else if (removed != nullptr) {
            /* Keep the name in the list too: the caller logs whatever was found, so a leftover
               that refuses to go away is visible in log.txt instead of silently blocking a
               later install (that is how an install got stuck on hardware). */
            removed->push_back(name + " (could not remove)");
        }
    }
    fsDirClose(&handle);
    return count;
}
bool SdFolderPayloadSource::Read(const std::string &name, std::vector<u8> *out,
                                 std::string *error) {
    const std::string path = m_dir + "/" + name;
    return ReadWholeFile(*m_sd, path, out, error);
}

bool ReadStateFile(FsFileSystem &sd, InstallState *state, bool *found, std::string *error) {
    std::vector<u8> data;
    std::string read_error;
    if (!ReadWholeFile(sd, kStatePath, &data, &read_error)) {
        if (found != nullptr) {
            *found = false;
        }
        return true; /* no record is not an error */
    }
    if (found != nullptr) {
        *found = true;
    }
    const std::string text(data.begin(), data.end());
    return ParseState(text, state, error);
}

bool WriteStateFile(FsFileSystem &sd, const InstallState &state, std::string *error) {
    const std::string text = DumpState(state);
    const std::vector<u8> data(text.begin(), text.end());
    return WriteFileVerified(sd, kStatePath, data, util::Sha256Hex(data.data(), data.size()), error);
}

bool ReadTextFile(FsFileSystem &sd, const std::string &path, std::string *out, bool *found,
                  std::string *error) {
    std::vector<u8> data;
    std::string read_error;
    if (!ReadWholeFile(sd, path, &data, &read_error)) {
        if (found != nullptr) {
            *found = false;
        }
        /* Nothing to read (missing, or the card refused the open): the caller falls back to its
           defaults, and the reason goes to the log instead of the player. */
        if (error != nullptr) {
            *error = read_error;
        }
        return true;
    }
    if (found != nullptr) {
        *found = true;
    }
    if (out != nullptr) {
        out->assign(data.begin(), data.end());
    }
    return true;
}

bool WriteTextFile(FsFileSystem &sd, const std::string &path, const std::string &text,
                   std::string *error) {
    const std::vector<u8> data(text.begin(), text.end());
    return WriteFileVerified(sd, path, data, util::Sha256Hex(data.data(), data.size()), error);
}

bool VerifyInstalledFiles(FsFileSystem &sd, const InstallState &state, std::string *error) {
    /* A record that names nothing describes no installation; saying "incomplete" is the honest
       answer, and it also means the caller never treats a stub record as a healthy install. */
    if (state.files.empty()) {
        if (error != nullptr) {
            *error = "the install record lists no files";
        }
        return false;
    }
    for (const InstalledFile &file : state.files) {
        if (!FileExists(sd, file.target)) {
            if (error != nullptr) {
                *error = "installed file " + file.target + " is not on the card";
            }
            return false;
        }
        std::string actual;
        std::string hash_error;
        if (!HashFile(sd, file.target, &actual, &hash_error)) {
            if (error != nullptr) {
                *error = "installed file " + file.target + " cannot be read: " + hash_error;
            }
            return false;
        }
        if (Upper(actual) != Upper(file.sha256)) {
            if (error != nullptr) {
                *error = "installed file " + file.target + " differs from the record (got " +
                         actual.substr(0, 8) + ")";
            }
            return false;
        }
    }
    return true;
}

bool ReadManifestFile(FsFileSystem &sd, const std::string &path, std::string_view app_version,
                      bool require_release_build, manifest::Manifest *out, bool *found,
                      std::string *error) {
    std::vector<u8> data;
    std::string read_error;
    if (!ReadWholeFile(sd, path, &data, &read_error)) {
        if (found != nullptr) {
            *found = false;
        }
        return true;
    }
    if (found != nullptr) {
        *found = true;
    }
    const std::string text(data.begin(), data.end());
    manifest::ParseOptions options;
    options.require_release_build = require_release_build;
    const auto parsed = manifest::Parse(text, app_version, options);
    if (!parsed.ok) {
        if (error != nullptr) {
            *error = parsed.error;
        }
        return false;
    }
    if (out != nullptr) {
        *out = parsed.manifest;
    }
    return true;
}

InstallResult Install(FsFileSystem &sd, const manifest::Manifest &manifest,
                      const manifest::GameEntry &game, PayloadSource &source, bool dry_run,
                      const ProgressCallback &progress) {
    InstallResult result;
    result.dry_run = dry_run;
    const int total = static_cast<int>(game.files.size());
    InstallState state;
    state.agent_version = manifest.agent.version;
    state.agent_commit = manifest.agent.commit;
    state.content_id = game.content_id;
    state.build_id = game.build_id;

    for (int i = 0; i < total; ++i) {
        const manifest::FileEntry &entry = game.files[static_cast<std::size_t>(i)];
        if (progress) {
            progress(Progress{entry.name, i + 1, total});
        }
        std::vector<u8> payload;
        std::string error;
        if (!source.Read(entry.source, &payload, &error)) {
            result.error = i18n::Format(i18n::StringId::InstallErrPayloadRead, error.c_str());
            return result;
        }
        if (payload.size() != entry.size) {
            result.error = i18n::Format(i18n::StringId::InstallErrPayloadSize, entry.name.c_str(),
                                        static_cast<unsigned long long>(payload.size()),
                                        static_cast<unsigned long long>(entry.size));
            return result;
        }
        const std::string actual = util::Sha256Hex(payload.data(), payload.size());
        if (Upper(actual) != entry.sha256) {
            result.error = i18n::Format(i18n::StringId::InstallErrPayloadSha, entry.name.c_str(),
                                        actual.c_str());
            return result;
        }
        if (!dry_run) {
            /* Phase one only prepares: nothing that is already installed is touched, so a
               failure here leaves the card exactly as it was. */
            if (!PrepareFile(sd, entry.target, payload, entry.sha256, &error)) {
                result.error = i18n::Format(i18n::StringId::InstallErrWrite, error.c_str());
                for (const auto &other : game.files) {
                    DeleteIfPresent(sd, AbsolutePath(other.target) + kTempSuffix);
                }
                return result;
            }
        }
        state.files.push_back({entry.target, entry.size, entry.sha256});
    }

    /* Phase two: every file is prepared and verified, so only cheap renames are left.  If one
       of them fails the files committed so far are rolled back -- the user must never be left
       with a half-installed agent (measured: a failure on file 2 used to leave file 1 in
       place and the rest missing). */
    if (!dry_run) {
        /* vector<bool> is a bitset and cannot hand out a bool*; the flags are plain chars. */
        std::vector<char> had_backup(game.files.size(), 0);
        std::size_t committed = 0;
        for (std::size_t i = 0; i < game.files.size(); ++i) {
            std::string error;
            bool file_had_backup = false;
            if (!CommitFile(sd, game.files[i].target, game.files[i].sha256, &file_had_backup,
                            &error)) {
                result.error = i18n::Format(i18n::StringId::InstallErrWrite, error.c_str());
                for (std::size_t j = committed; j-- > 0;) {
                    RollbackFile(sd, game.files[j].target, had_backup[j] != 0);
                }
                for (std::size_t j = committed; j < game.files.size(); ++j) {
                    DeleteIfPresent(sd, AbsolutePath(game.files[j].target) + kTempSuffix);
                }
                return result;
            }
            had_backup[i] = file_had_backup ? 1 : 0;
            ++committed;
            ++result.files_written;
        }
        /* The record goes in *before* the backups are dropped, so it shares the transaction:
           if it cannot be written, the game files we just installed are rolled back as well and
           the card never ends up with files no record describes. */
        u64 timestamp = 0;
        if (R_SUCCEEDED(timeInitialize())) {
            timeGetCurrentTime(TimeType_UserSystemClock, &timestamp);
        }
        state.installed_at = util::FormatUnixTimeUtc(static_cast<std::int64_t>(timestamp));
        std::string state_error;
        if (!WriteStateFile(sd, state, &state_error)) {
            result.error = i18n::Format(i18n::StringId::InstallErrStateWrite,
                                        state_error.c_str());
            for (std::size_t j = game.files.size(); j-- > 0;) {
                RollbackFile(sd, game.files[j].target, had_backup[j] != 0);
            }
            return result;
        }
        for (const auto &entry : game.files) {
            DropBackups(sd, entry.target);
        }
    }

    result.ok = true;
    result.state = std::move(state);
    return result;
}

InstallResult Uninstall(FsFileSystem &sd, bool dry_run, const ProgressCallback &progress) {
    InstallResult result;
    result.dry_run = dry_run;
    InstallState state;
    bool found = false;
    std::string error;
    if (!ReadStateFile(sd, &state, &found, &error)) {
        result.error = i18n::Format(i18n::StringId::InstallErrStateParse, error.c_str());
        return result;
    }
    if (!found) {
        result.error = i18n::Text(i18n::StringId::UninstallNoRecord, i18n::Current());
        return result;
    }
    const int total = static_cast<int>(state.files.size());
    /* Delete by name only.  The record's hash is deliberately *not* checked here: a player who
       edited the files by hand must still be able to remove them, and refusing to delete because
       "the file changed" is a worse outcome than deleting a file we put there.  Every entry is
       attempted -- stopping half way would leave the directory neither installed nor clean --
       and whatever could not be deleted is reported together at the end. */
    std::string failures;
    for (int i = 0; i < total; ++i) {
        const InstalledFile &file = state.files[static_cast<std::size_t>(i)];
        if (progress) {
            progress(Progress{file.target, i + 1, total});
        }
        if (!dry_run) {
            const FsPath arg(AbsolutePath(file.target));
            const Result rc = fsFsDeleteFile(&sd, arg.c_str());
            if (R_FAILED(rc) && rc != 0x00000202) { /* 202: already gone */
                if (!failures.empty()) {
                    failures += "; ";
                }
                failures += Describe("delete " + file.target, rc);
                continue;
            }
        }
        ++result.files_written;
    }
    if (!failures.empty()) {
        result.error = i18n::Format(i18n::StringId::UninstallFailed, failures.c_str());
        return result;
    }
    if (!dry_run) {
        /* Only remove the directory when it is empty; a failure means someone else's files
           live there, so keep it. */
        /* The card path comes from env/detect.hpp (single source of truth); the engine's own
           targets are the only files it could have created there. */
        const std::string dir = AbsolutePath(env::kAcnhExefsDir);
        fsFsDeleteDirectory(&sd, FsPath(dir).c_str());
        const FsPath arg_state(kStatePath);
        const Result rc = fsFsDeleteFile(&sd, arg_state.c_str());
        if (R_FAILED(rc) && rc != 0x00000202) {
            result.error = Describe("delete state.json", rc);
            return result;
        }
    }
    result.ok = true;
    return result;
}

}  // namespace acnh_manager::install
