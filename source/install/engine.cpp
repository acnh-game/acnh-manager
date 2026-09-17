#include "engine.hpp"

#include <cstdio>
#include <utility>

#include "i18n/strings.hpp"
#include "util/sha256.hpp"
#include "util/time.hpp"

namespace acnh_manager::install {
namespace {

constexpr const char *kStatePath = "/switch/ACNH-Manager/state.json";
constexpr const char *kTempSuffix = ".acnh-tmp";
constexpr const char *kOldSuffix = ".acnh-old";
constexpr std::size_t kChunkSize = 64 * 1024;

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
    const Result rc = fsFsDeleteFile(&sd, absolute.c_str());
    return R_SUCCEEDED(rc) || rc == 0x00000202; /* 202 = PathNotFound */
}

}  // namespace

/* Every path handed to fs* starts with '/': the manifest target is relative ("atmosphere/...",
   and manifest::IsSafeTarget guarantees there is no leading '/'), and FS refuses a relative
   path (measured rc=0x2EEA02 on create).  This is the single place that adds the prefix. */
std::string AbsolutePath(std::string_view path) {
    if (!path.empty() && path.front() == '/') {
        return std::string(path);
    }
    return "/" + std::string(path);
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
            const Result rc = fsFsCreateDirectory(&sd, current.c_str());
            if (R_FAILED(rc) && rc != 0x00000402) { /* 402 = PathAlreadyExists */
                if (error != nullptr) {
                    *error = Describe("mkdir " + current, rc);
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
    Result rc = fsFsOpenFile(&sd, absolute.c_str(), FsOpenMode_Read, &file);
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
    Result rc = fsFsOpenFile(&sd, absolute.c_str(), FsOpenMode_Read, &file);
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

bool WriteFileVerified(FsFileSystem &sd, const std::string &path, const std::vector<u8> &data,
                       const std::string &sha256, std::string *error) {
    const std::string target = AbsolutePath(path);
    const std::string temp = target + kTempSuffix;
    const std::string backup = target + kOldSuffix;
    if (!EnsureDirectory(sd, target.substr(0, target.find_last_of('/')), error)) {
        return false;
    }
    DeleteIfPresent(sd, temp);
    Result rc = fsFsCreateFile(&sd, temp.c_str(), static_cast<s64>(data.size()), 0);
    if (R_FAILED(rc)) {
        if (error != nullptr) {
            *error = Describe("create " + temp, rc);
        }
        return false;
    }
    FsFile file{};
    rc = fsFsOpenFile(&sd, temp.c_str(), FsOpenMode_Write, &file);
    if (R_SUCCEEDED(rc)) {
        rc = fsFileSetSize(&file, static_cast<s64>(data.size()));
    }
    if (R_SUCCEEDED(rc) && !data.empty()) {
        rc = fsFileWrite(&file, 0, data.data(), static_cast<u64>(data.size()), FsWriteOption_Flush);
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

    /* Atomic replace: move the old file to .acnh-old, rename, then drop the old file. */
    DeleteIfPresent(sd, backup);
    const Result rc_backup = fsFsRenameFile(&sd, target.c_str(), backup.c_str());
    if (R_FAILED(rc_backup) && rc_backup != 0x00000202) {
        if (error != nullptr) {
            *error = Describe("backup " + target, rc_backup);
        }
        DeleteIfPresent(sd, temp);
        return false;
    }
    rc = fsFsRenameFile(&sd, temp.c_str(), target.c_str());
    if (R_FAILED(rc)) {
        if (error != nullptr) {
            *error = Describe("rename into " + target, rc);
        }
        /* Best effort: put the old file back. */
        fsFsRenameFile(&sd, backup.c_str(), target.c_str());
        DeleteIfPresent(sd, temp);
        return false;
    }
    DeleteIfPresent(sd, backup);
    return true;
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
            if (!WriteFileVerified(sd, entry.target, payload, entry.sha256, &error)) {
                result.error = i18n::Format(i18n::StringId::InstallErrWrite, error.c_str());
                return result;
            }
            ++result.files_written;
        }
        state.files.push_back({entry.target, entry.size, entry.sha256});
    }

    u64 timestamp = 0;
    if (R_SUCCEEDED(timeInitialize())) {
        timeGetCurrentTime(TimeType_UserSystemClock, &timestamp);
    }
    state.installed_at = util::FormatUnixTimeUtc(static_cast<std::int64_t>(timestamp));

    if (!dry_run) {
        std::string error;
        if (!WriteStateFile(sd, state, &error)) {
            result.error = i18n::Format(i18n::StringId::InstallErrStateWrite, error.c_str());
            return result;
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
    for (int i = 0; i < total; ++i) {
        const InstalledFile &file = state.files[static_cast<std::size_t>(i)];
        if (progress) {
            progress(Progress{file.target, i + 1, total});
        }
        std::string actual;
        if (!HashFile(sd, file.target, &actual, &error)) {
            /* Not there any more: treat as already removed and keep going. */
            continue;
        }
        if (Upper(actual) != file.sha256) {
            result.error = i18n::Format(i18n::StringId::UninstallModified, file.target.c_str());
            return result;
        }
        if (!dry_run) {
            const Result rc = fsFsDeleteFile(&sd, AbsolutePath(file.target).c_str());
            if (R_FAILED(rc)) {
                result.error = Describe("delete " + file.target, rc);
                return result;
            }
        }
        ++result.files_written;
    }
    if (!dry_run) {
        /* Only remove the directory when it is empty; a failure means someone else's files
           live there, so keep it. */
        const std::string dir = "/atmosphere/contents/01006F8002326000/exefs";
        fsFsDeleteDirectory(&sd, dir.c_str());
        const Result rc = fsFsDeleteFile(&sd, kStatePath);
        if (R_FAILED(rc) && rc != 0x00000202) {
            result.error = Describe("delete state.json", rc);
            return result;
        }
    }
    result.ok = true;
    return result;
}

}  // namespace acnh_manager::install
