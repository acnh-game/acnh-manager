#include "detect.hpp"

#include <cstdio>
#include <cstring>
#include <utility>

namespace acnh_manager::env {
namespace {

constexpr const char *kExefsDir = "/atmosphere/contents/01006F8002326000/exefs";
constexpr const char *kCheatsDir = "/atmosphere/contents/01006F8002326000/cheats";
constexpr const char *kOverrideConfig = "/atmosphere/config/override_config.ini";
constexpr const char *kTitleConfig = "/atmosphere/contents/01006F8002326000/config.ini";

void Hex(char *out, const u8 *data, std::size_t size) {
    static const char digits[] = "0123456789ABCDEF";
    for (std::size_t i = 0; i < size; ++i) {
        out[i * 2] = digits[data[i] >> 4];
        out[i * 2 + 1] = digits[data[i] & 0xF];
    }
    out[size * 2] = '\0';
}

void Note(EnvironmentReport *report, const char *what, Result rc) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%s rc=0x%08X", what, rc);
    if (!report->problems.empty()) {
        report->problems += "; ";
    }
    report->problems += buf;
}

bool ReadTextFile(FsFileSystem &sd, const char *path, std::string *out) {
    FsFile file{};
    if (R_FAILED(fsFsOpenFile(&sd, path, FsOpenMode_Read, &file))) {
        return false;
    }
    s64 size = 0;
    if (R_FAILED(fsFileGetSize(&file, &size)) || size <= 0 || size > 64 * 1024) {
        fsFileClose(&file);
        return false;
    }
    std::string text(static_cast<std::size_t>(size), '\0');
    u64 read = 0;
    const Result rc = fsFileRead(&file, 0, text.data(), static_cast<u64>(size), 0, &read);
    fsFileClose(&file);
    if (R_FAILED(rc)) {
        return false;
    }
    text.resize(static_cast<std::size_t>(read));
    *out = std::move(text);
    return true;
}

/* dmnt:cht metadata layout (matches Atmosphere's header, sizeof == 0x70). */
struct CheatProcessMetadata {
    u64 process_id;
    u64 program_id;
    u64 main_nso_extents[2];
    u64 heap_extents[2];
    u64 alias_extents[2];
    u64 aslr_extents[2];
    u8 main_nso_module_id[0x20];
};
static_assert(sizeof(CheatProcessMetadata) == 0x70, "CheatProcessMetadata layout");

void CollectDmnt(EnvironmentReport *report) {
    Service service{};
    if (R_FAILED(smGetService(&service, "dmnt:cht"))) {
        return;
    }
    const Result rc_force = serviceDispatch(&service, 65003);
    CheatProcessMetadata metadata{};
    const Result rc_meta = serviceDispatchOut(&service, 65002, metadata);
    serviceClose(&service);
    if (R_FAILED(rc_force) || R_FAILED(rc_meta)) {
        Note(report, "dmnt:cht metadata", R_FAILED(rc_meta) ? rc_meta : rc_force);
        return;
    }
    if (metadata.program_id != kAcnhTitleId) {
        return; /* something else is running; not a verdict input */
    }
    char hex[0x21];
    Hex(hex, metadata.main_nso_module_id, 16);
    report->build.module_id = hex;
    report->build.module_id_known = true;
}

void CollectNcm(EnvironmentReport *report) {
    if (R_FAILED(ncmInitialize())) {
        report->problems += "ncmInitialize failed";
        return;
    }
    NcmContentMetaDatabase db{};
    const Result rc_db = ncmOpenContentMetaDatabase(&db, NcmStorageId_SdCard);
    if (R_FAILED(rc_db)) {
        Note(report, "ncm open meta db", rc_db);
        return;
    }
    NcmContentMetaKey key{};
    const Result rc_key =
        ncmContentMetaDatabaseGetLatestContentMetaKey(&db, &key, kAcnhUpdateTitleId);
    if (R_FAILED(rc_key)) {
        Note(report, "ncm latest key", rc_key);
        serviceClose(&db.s);
        return;
    }
    report->build.version = key.version;
    NcmContentInfo infos[8]{};
    s32 written = 0;
    const Result rc_list = ncmContentMetaDatabaseListContentInfo(&db, &written, infos, 8, &key, 0);
    if (R_FAILED(rc_list)) {
        Note(report, "ncm list content info", rc_list);
        serviceClose(&db.s);
        return;
    }
    for (s32 i = 0; i < written && i < 8; ++i) {
        if (infos[i].content_type != 1) { /* 1 = Program */
            continue;
        }
        char hex[0x21];
        Hex(hex, infos[i].content_id.c, 16);
        report->build.content_id = hex;
        break;
    }
    serviceClose(&db.s);
}

}  // namespace

std::vector<ExefsFile> ListDirectory(FsFileSystem &sd, const char *path) {
    std::vector<ExefsFile> files;
    FsDir dir{};
    if (R_FAILED(fsFsOpenDirectory(&sd, path, FsDirOpenMode_ReadFiles, &dir))) {
        return files;
    }
    FsDirectoryEntry entries[32]{};
    s64 total = 0;
    if (R_SUCCEEDED(fsDirRead(&dir, &total, 32, entries))) {
        for (s64 i = 0; i < total && i < 32; ++i) {
            if (entries[i].type != FsDirEntryType_File) {
                continue;
            }
            files.push_back({entries[i].name, static_cast<std::uint64_t>(entries[i].file_size)});
        }
    }
    fsDirClose(&dir);
    return files;
}

EnvironmentReport Collect(FsFileSystem &sd) {
    EnvironmentReport report;
    const u32 hos = hosversionGet();
    char version[32];
    std::snprintf(version, sizeof(version), "%u.%u.%u", (hos >> 16) & 0xFF, (hos >> 8) & 0xFF,
                  hos & 0xFF);
    report.hos_version = version;
    report.applet_type = static_cast<int>(appletGetAppletType());
    report.build.title_id = "01006F8002326000";

    /* 1. ns: content table (version and patch storage) + whether an app is running */
    if (R_SUCCEEDED(nsInitialize())) {
        NsApplicationContentMetaStatus status[16]{};
        s32 count = 0;
        const Result rc = nsListApplicationContentMetaStatus(kAcnhTitleId, 0, status, 16, &count);
        if (R_SUCCEEDED(rc)) {
            for (s32 i = 0; i < count && i < 16; ++i) {
                report.metas.push_back({status[i].meta_type, status[i].storageID, status[i].version,
                                        status[i].application_id});
                if (status[i].meta_type == 0x81) { /* Patch */
                    report.patch_found = true;
                    report.patch_storage = status[i].storageID;
                }
            }
        } else {
            Note(&report, "nsListApplicationContentMetaStatus", rc);
        }
        bool running = false;
        if (R_SUCCEEDED(nsIsAnyApplicationRunning(&running))) {
            report.application_running = running;
        }
    } else {
        report.problems += "nsInitialize failed";
    }

    /* 2. ncm: the update title's Program content id (the gate's primary input) */
    CollectNcm(&report);

    /* 3. dmnt:cht: main ModuleId while the game runs (the strengthened input) */
    if (report.application_running) {
        CollectDmnt(&report);
    }

    /* Override configuration: whether a key must be held at launch */
    std::string override_text;
    if (ReadTextFile(sd, kOverrideConfig, &override_text)) {
        report.have_override_config = true;
        const OverrideConfig config = ParseOverrideConfig(override_text);
        OverrideKey title_key;
        std::string title_text;
        if (ReadTextFile(sd, kTitleConfig, &title_text)) {
            report.have_title_config = true;
            title_key = ParseTitleConfig(title_text);
        }
        report.advice = Advise(config, title_key);
    } else {
        report.advice = Advise(OverrideConfig{}, OverrideKey{});
    }

    /* Current install state and the legacy-cheat check */
    report.exefs = ListDirectory(sd, kExefsDir);
    for (const ExefsFile &file : ListDirectory(sd, kCheatsDir)) {
        if (file.name.size() == 36 && file.name.compare(32, 4, ".txt") == 0) {
            report.legacy_cheat_present = true;
            report.legacy_cheat_name = file.name;
            break;
        }
    }
    return report;
}

}  // namespace acnh_manager::env
