#include "env/startup_runtime.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#include "log.hpp"
#include "util/fs_path.hpp"
#include "util/sha256.hpp"

namespace acnh_manager::env {
namespace {

constexpr std::size_t kCopyChunk = 64 * 1024;
constexpr std::uint64_t kMaxNroBytes = 32ull * 1024 * 1024;

std::string NormalizePath(std::string path) {
    /* hbl/hbmenu may hand over `sdmc:/...`; fs* paths never carry the device prefix. */
    if (path.rfind("sdmc:", 0) == 0) {
        path.erase(0, 5);
    }
    return path;
}

bool FileExists(FsFileSystem &sd, const std::string &path) {
    FsFile file{};
    const util::FsPath arg(path);
    if (R_FAILED(fsFsOpenFile(&sd, arg.c_str(), FsOpenMode_Read, &file))) {
        return false;
    }
    fsFileClose(&file);
    return true;
}

bool ReadNroMetaFromFile(FsFileSystem &sd, const std::string &path, NroMeta *out,
                         std::string *error) {
    FsFile file{};
    const util::FsPath arg(path);
    Result rc = fsFsOpenFile(&sd, arg.c_str(), FsOpenMode_Read, &file);
    if (R_FAILED(rc)) {
        if (error != nullptr) {
            char text[96];
            std::snprintf(text, sizeof(text), "cannot open (0x%08x)", static_cast<unsigned>(rc));
            *error = text;
        }
        return false;
    }
    s64 file_size = 0;
    rc = fsFileGetSize(&file, &file_size);
    if (R_FAILED(rc) || file_size <= 0 || static_cast<std::uint64_t>(file_size) > kMaxNroBytes) {
        fsFileClose(&file);
        if (error != nullptr) {
            *error = "cannot read the file size";
        }
        return false;
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(file_size));
    const std::size_t chunk = kCopyChunk;
    util::Sha256 sha;
    std::size_t at = 0;
    while (at < bytes.size()) {
        u64 read = 0;
        const std::size_t want = std::min(chunk, bytes.size() - at);
        rc = fsFileRead(&file, static_cast<s64>(at), bytes.data() + at, want, FsReadOption_None,
                        &read);
        if (R_FAILED(rc) || read != want) {
            fsFileClose(&file);
            if (error != nullptr) {
                *error = "cannot read the file";
            }
            return false;
        }
        sha.Update(bytes.data() + at, want);
        at += want;
    }
    fsFileClose(&file);
    NroMeta meta;
    if (!ParseNroMeta(bytes.data(), bytes.size(), &meta, error)) {
        *out = meta;
        return false;
    }
    meta.size = static_cast<std::uint64_t>(file_size);
    meta.sha256 = util::Sha256::ToHex(sha.Finish());
    FsTimeStampRaw stamp{};
    if (R_SUCCEEDED(fsFsGetFileTimeStampRaw(&sd, arg.c_str(), &stamp)) && stamp.is_valid) {
        meta.mtime = stamp.modified;
    }
    *out = meta;
    return true;
}

bool DeleteFile(FsFileSystem &sd, const std::string &path, std::string *error) {
    const util::FsPath arg(path);
    const Result rc = fsFsDeleteFile(&sd, arg.c_str());
    if (R_FAILED(rc)) {
        if (error != nullptr) {
            char text[96];
            std::snprintf(text, sizeof(text), "cannot delete %s (0x%08x)", path.c_str(),
                          static_cast<unsigned>(rc));
            *error = text;
        }
        return false;
    }
    return true;
}

bool CopyVerified(FsFileSystem &sd, const std::string &from, const std::string &to,
                  std::string *error) {
    const std::string temp = to + ".acnh-tmp";
    FsFile source{};
    const util::FsPath from_arg(from);
    Result rc = fsFsOpenFile(&sd, from_arg.c_str(), FsOpenMode_Read, &source);
    if (R_FAILED(rc)) {
        *error = "cannot open the running NRO";
        return false;
    }
    s64 source_size = 0;
    rc = fsFileGetSize(&source, &source_size);
    if (R_FAILED(rc) || source_size <= 0) {
        fsFileClose(&source);
        *error = "cannot read the running NRO's size";
        return false;
    }
    /* A leftover temp from an interrupted copy: remove it before creating ours. */
    DeleteFile(sd, temp, nullptr);
    FsFile target{};
    const util::FsPath temp_arg(temp);
    rc = fsFsCreateFile(&sd, temp_arg.c_str(), source_size, 0);
    if (R_FAILED(rc) || R_FAILED(rc = fsFsOpenFile(&sd, temp_arg.c_str(),
                                                   FsOpenMode_Write, &target))) {
        fsFileClose(&source);
        DeleteFile(sd, temp, nullptr);
        *error = "cannot create the standard NRO";
        return false;
    }
    util::Sha256 sha;
    std::vector<std::uint8_t> buffer(kCopyChunk);
    for (s64 at = 0; at < source_size;) {
        u64 read = 0;
        const std::size_t want =
            static_cast<std::size_t>(std::min<s64>(static_cast<s64>(buffer.size()), source_size - at));
        rc = fsFileRead(&source, at, buffer.data(), want, FsReadOption_None, &read);
        if (R_FAILED(rc) || read != want) {
            break;
        }
        sha.Update(buffer.data(), want);
        rc = fsFileWrite(&target, at, buffer.data(), want, FsWriteOption_None);
        if (R_FAILED(rc)) {
            break;
        }
        at += static_cast<s64>(want);
        if (at == source_size) {
            /* All bytes copied: verify the destination before touching the real path. */
            fsFileFlush(&target);
            fsFileClose(&target);
            fsFileClose(&source);
            FsFile check{};
            rc = fsFsOpenFile(&sd, temp_arg.c_str(), FsOpenMode_Read, &check);
            if (R_FAILED(rc)) {
                DeleteFile(sd, temp, nullptr);
                *error = "cannot verify the copied NRO";
                return false;
            }
            util::Sha256 check_sha;
            s64 check_size = 0;
            fsFileGetSize(&check, &check_size);
            bool same = check_size == source_size;
            for (s64 pos = 0; same && pos < check_size;) {
                u64 got = 0;
                const std::size_t want_check = static_cast<std::size_t>(
                    std::min<s64>(static_cast<s64>(buffer.size()), check_size - pos));
                if (R_FAILED(fsFileRead(&check, pos, buffer.data(), want_check,
                                        FsReadOption_None, &got)) ||
                    got != want_check) {
                    same = false;
                    break;
                }
                check_sha.Update(buffer.data(), want_check);
                pos += static_cast<s64>(want_check);
            }
            fsFileClose(&check);
            same = same && util::Sha256::ToHex(check_sha.Finish()) ==
                               util::Sha256::ToHex(sha.Finish());
            if (!same) {
                DeleteFile(sd, temp, nullptr);
                *error = "the copied NRO does not match the running one";
                return false;
            }
            /* FAT cannot rename over an existing file. */
            DeleteFile(sd, to, nullptr);
            const util::FsPath to_arg(to);
            rc = fsFsRenameFile(&sd, temp_arg.c_str(), to_arg.c_str());
            if (R_FAILED(rc)) {
                DeleteFile(sd, temp, nullptr);
                *error = "cannot move the copied NRO into place";
                return false;
            }
            return true;
        }
    }
    fsFileClose(&target);
    fsFileClose(&source);
    DeleteFile(sd, temp, nullptr);
    *error = "the copy stopped before the end";
    return false;
}

}  // namespace

StartupPlan RunStartupCheck(FsFileSystem &sd, const std::string &argv0, bool dev, Log *log) {
    StartupPlan plan;
    plan.current_path = NormalizePath(argv0);
    plan.inputs.dev = dev;
    plan.inputs.current_path = plan.current_path;
    const bool standard = SameNroPath(plan.current_path, kStandardNroPath);
    const char *mode =
        appletGetAppletType() == AppletType_Application ? "application" : "applet";
    std::uint64_t program_id = 0;
    u64 pid = 0;
    if (R_SUCCEEDED(svcGetProcessId(&pid, envGetOwnProcessHandle())) &&
        R_SUCCEEDED(pmdmntInitialize())) {
        pmdmntGetProgramId(&program_id, pid);
        pmdmntExit();
    }
    if (log != nullptr) {
        log->Line("launch: mode=%s path=%s program-id=0x%016llX layout=%s", mode,
                  plan.current_path.c_str(), static_cast<unsigned long long>(program_id),
                  standard ? "standard" : "nonstandard");
    }
    if (dev) {
        plan.decision = DecideStartup(plan.inputs);
        return plan;
    }
    if (!plan.current_path.empty() && !SameNroPath(plan.current_path, kStandardNroPath)) {
        std::string read_error;
        plan.inputs.current_readable =
            ReadNroMetaFromFile(sd, plan.current_path, &plan.current, &read_error);
        plan.inputs.current_version = plan.current.version;
        if (plan.inputs.current_readable) {
            plan.inputs.standard_present = FileExists(sd, kStandardNroPath);
            if (plan.inputs.standard_present) {
                std::string standard_error;
                plan.inputs.standard_readable =
                    ReadNroMetaFromFile(sd, kStandardNroPath, &plan.standard, &standard_error);
                plan.inputs.standard_ours =
                    plan.inputs.standard_readable && NroLooksOurs(plan.standard);
                plan.inputs.standard_version = plan.standard.version;
                plan.inputs.same_build =
                    plan.standard.size == plan.current.size &&
                    !plan.current.sha256.empty() &&
                    plan.current.sha256 == plan.standard.sha256;
                plan.inputs.current_mtime_newer = plan.current.mtime > plan.standard.mtime;
            }
        } else if (log != nullptr) {
            log->Line("startup: cannot read this NRO's own metadata: %s", read_error.c_str());
        }
    }
    plan.decision = DecideStartup(plan.inputs);
    if (log != nullptr) {
        log->Line("startup: reason=%s action=%d current=%s standard=%s same-build=%d mtime-newer=%d",
                  plan.decision.reason.c_str(), static_cast<int>(plan.decision.action),
                  plan.current_path.c_str(), kStandardNroPath,
                  plan.inputs.same_build ? 1 : 0, plan.inputs.current_mtime_newer ? 1 : 0);
    }
    return plan;
}

bool ApplyStartupRepair(FsFileSystem &sd, const StartupPlan &plan, Log *log, std::string *error) {
    switch (plan.decision.action) {
        case StartupAction::None:
        case StartupAction::ReportOnly:
            return true;
        case StartupAction::DeleteCurrent:
            return DeleteFile(sd, plan.current_path, error);
        case StartupAction::CopyCurrent: {
            if (!CopyVerified(sd, plan.current_path, kStandardNroPath, error)) {
                return false;
            }
            /* Removing this copy is what makes it a move; a card that refuses only this step has
               already got a good standard NRO, so it does not block the restart. */
            std::string delete_error;
            if (!DeleteFile(sd, plan.current_path, &delete_error) && log != nullptr) {
                log->Line("startup: copied, but could not remove this copy: %s",
                          delete_error.c_str());
            }
            return true;
        }
    }
    return true;
}

bool SetNextLoadToStandard() {
    if (!envHasNextLoad()) {
        return false;
    }
    return R_SUCCEEDED(envSetNextLoad(kStandardNroPath, kStandardNroPath));
}

}  // namespace acnh_manager::env
