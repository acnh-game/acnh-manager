#include "detect.hpp"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <utility>

#include "env/cheat_file.hpp"
#include "install/nca.hpp"
#include "util/fs_path.hpp"

namespace acnh_manager::env {
namespace {

/* Local aliases of the single-source paths declared in detect.hpp. */
constexpr const char *kExefsDir = kAcnhExefsDir;
constexpr const char *kCheatsDir = kAcnhCheatsDir;
constexpr const char *kOverrideConfig = kAcnhOverrideConfig;
constexpr const char *kTitleConfig = kAcnhTitleConfig;

/* A cheat file is a list of `[entry]` blocks; the cap only exists so a corrupt size cannot make
   the read unbounded.  The known pack is about 6 KB, so 256 KB is generous. */
constexpr std::size_t kCheatFileMaxBytes = 256 * 1024;
constexpr std::size_t kConfigFileMaxBytes = 64 * 1024;

void Hex(char *out, const u8 *data, std::size_t size) {
    static const char digits[] = "0123456789ABCDEF";
    for (std::size_t i = 0; i < size; ++i) {
        out[i * 2] = digits[data[i] >> 4];
        out[i * 2 + 1] = digits[data[i] & 0xF];
    }
    out[size * 2] = '\0';
}

/* The title id spelled the way the manifest and the gate spell it. */
std::string TitleIdHex(u64 id) {
    char text[17];
    std::snprintf(text, sizeof(text), "%016llX", static_cast<unsigned long long>(id));
    return text;
}

void Note(EnvironmentReport *report, const char *what, Result rc) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%s rc=0x%08X", what, rc);
    if (!report->problems.empty()) {
        report->problems += "; ";
    }
    report->problems += buf;
}

void NoteText(EnvironmentReport *report, const char *text) {
    if (!report->problems.empty()) {
        report->problems += "; ";
    }
    report->problems += text;
}

bool ReadTextFile(FsFileSystem &sd, const char *path, std::string *out, std::size_t max_size) {
    const util::FsPath arg(path);
    FsFile file{};
    if (R_FAILED(fsFsOpenFile(&sd, arg.c_str(), FsOpenMode_Read, &file))) {
        return false;
    }
    s64 size = 0;
    if (R_FAILED(fsFileGetSize(&file, &size)) || size <= 0 ||
        size > static_cast<s64>(max_size)) {
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

/* "Game running" means the Animal Crossing process exists, not that some application does.
   A HOME Menu shortcut launches ACNH-Manager itself in Application mode, so the ns-wide
   any-application query reports true even when the game is closed. */
bool IsAcnhProcessRunning() {
    if (R_FAILED(pmdmntInitialize())) {
        return false;
    }
    u64 pid = 0;
    const Result rc = pmdmntGetProcessId(&pid, kAcnhTitleId);
    pmdmntExit();
    return R_SUCCEEDED(rc) && pid != 0;
}

/* `update_absent` means we already know from the ns content table that no update package is
   installed (the list came back and carried no Patch meta).  That is not a fault: a bare 1.0.0
   copy has no update title at all, so `ncm` answering "no such content meta" is the expected
   answer and must not be dressed up as a detection problem on the details page (measured on a
   1.0.0 console: rc=0x00000E05).  With a patch installed the same failure *is* a problem and is
   reported. */

/* One line per stored meta record: its size, what its own header says, and the first bytes.  The
   console reads a title's content list -- and therefore its control data, its icon and the version
   it shows -- out of these bytes, so a record that this app wrote in a different shape than the
   system writes its own is the difference between a game the home menu can draw and one it cannot.
   Both records are logged (the base game's, written by the system, and the update's, written by
   us) because the comparison is the whole point. */
void LogMetaBlob(NcmContentMetaDatabase &db, const NcmContentMetaKey &key, const char *label,
                 EnvironmentReport *report) {
    char line[256];
    u64 size = 0;
    const Result rc_size = ncmContentMetaDatabaseGetSize(&db, &size, &key);
    std::snprintf(line, sizeof(line), "%s meta blob: size rc=0x%08x size=%llu", label,
                  static_cast<unsigned>(rc_size), static_cast<unsigned long long>(size));
    report->notes.emplace_back(line);
    if (R_FAILED(rc_size) || size == 0 || size > 0x10000) {
        return;
    }
    std::vector<std::uint8_t> blob(static_cast<std::size_t>(size));
    u64 got = size;
    const Result rc_get = ncmContentMetaDatabaseGet(&db, &key, &got, blob.data(), blob.size());
    if (R_FAILED(rc_get)) {
        std::snprintf(line, sizeof(line), "%s meta blob: read rc=0x%08x", label,
                      static_cast<unsigned>(rc_get));
        report->notes.emplace_back(line);
        return;
    }
    install::CnmtInfo info;
    std::string parse_error;
    if (install::ParseCnmt(blob.data(), static_cast<std::size_t>(got), &info, &parse_error)) {
        std::snprintf(line, sizeof(line),
                      "%s meta blob: header ext=%u contents=%zu metas=%u title=%s v=0x%x type=%u",
                      label, info.extended_header_size, info.contents.size(),
                      info.content_meta_count, info.title_id.c_str(), info.version,
                      static_cast<unsigned>(info.type));
        report->notes.emplace_back(line);
    } else {
        std::snprintf(line, sizeof(line), "%s meta blob: not parseable (%s)", label,
                      parse_error.c_str());
        report->notes.emplace_back(line);
    }
    std::string hex;
    const std::size_t dump = got < 0x78 ? static_cast<std::size_t>(got) : 0x78;
    for (std::size_t i = 0; i < dump; ++i) {
        std::snprintf(line, sizeof(line), "%02x", blob[i]);
        hex += line;
        if (i % 4 == 3) {
            hex += ' ';
        }
        if (i % 0x18 == 0x17) {
            char row[256];
            std::snprintf(row, sizeof(row), "%s meta blob: +0x%02zx %s", label, i + 1 - 0x18,
                          hex.c_str());
            report->notes.emplace_back(row);
            hex.clear();
        }
    }
    if (!hex.empty()) {
        std::snprintf(line, sizeof(line), "%s meta blob: tail %s", label, hex.c_str());
        report->notes.emplace_back(line);
    }
    /* What `ncm` itself reads out of those bytes: the list is the console's answer to "which
       contents does this title have", and it is the same answer the home menu uses to find the
       icon.  Comparing it with the bytes above settles the record's layout question with the
       service's own parser instead of mine. */
    NcmContentInfo infos[8]{};
    s32 written = 0;
    const Result rc_list = ncmContentMetaDatabaseListContentInfo(&db, &written, infos, 8, &key, 0);
    std::snprintf(line, sizeof(line), "%s meta blob: ncm list rc=0x%08x count=%d", label,
                  static_cast<unsigned>(rc_list), static_cast<int>(written));
    report->notes.emplace_back(line);
    for (s32 i = 0; i < written && i < 8; ++i) {
        char id_hex[0x21];
        Hex(id_hex, infos[i].content_id.c, 16);
        u64 size = 0;
        ncmContentInfoSizeToU64(&infos[i], &size);
        std::snprintf(line, sizeof(line), "%s meta blob: ncm info %d id=%s size=%llu type=%u", label,
                      static_cast<int>(i), id_hex, static_cast<unsigned long long>(size),
                      static_cast<unsigned>(infos[i].content_type));
        report->notes.emplace_back(line);
    }
}

void CollectNcm(EnvironmentReport *report, bool update_absent) {
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
        if (!update_absent) {
            Note(report, "ncm latest key", rc_key);
        }
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
    /* The base game's record, for the comparison above: it is the same kind of record the install
       writes for an update, written by the console itself. */
    {
        NcmContentMetaKey base{};
        if (R_SUCCEEDED(ncmContentMetaDatabaseGetLatestContentMetaKey(&db, &base, kAcnhTitleId))) {
            LogMetaBlob(db, base, "base", report);
        }
        LogMetaBlob(db, key, "patch", report);
    }
    serviceClose(&db.s);
}

/* NACP display version: the console's own answer to "which version is installed", and the exact
   string the home menu shows under "Ver." when a title is selected.  A bare 1.0.0 copy carries
   numeric title version 0, so without this string there is nothing human-readable to compare a
   manifest entry against.

   The buffer has to cover the whole declaration (`ns` maps it as a single out buffer, so a
   window that reaches past the end of a mapping is rejected): it is a page-aligned static array
   of the full control data -- NACP 0x4000 + icon 0x20000 = 0x24000 bytes.  144 KB does not
   belong on the thread stack, and a static local keeps exactly one copy. */
bool CollectControlData(EnvironmentReport *report, bool title_present) {
    alignas(0x1000) static u8 control[sizeof(NsApplicationControlData)];
    u64 actual = 0;
    /* Read once, then once more after a short wait.  The console answers this from a cache it
       rebuilds when it is told the title changed, and the rebuild is not finished by the time the
       install returns: right after an install the same call failed with 0x8C10 and the home screen
       had no version to show, while a fresh start moments later read it without trouble. */
    Result rc = 1;
    for (int attempt = 0; attempt < 3; ++attempt) {
        rc = nsGetApplicationControlData(NsApplicationControlSource_Storage, kAcnhTitleId,
                                        reinterpret_cast<NsApplicationControlData *>(control),
                                        sizeof(control), &actual);
        if (R_SUCCEEDED(rc)) {
            break;
        }
        svcSleepThread(150'000'000ULL); /* 150 ms */
    }
    if (R_FAILED(rc)) {
        /* Without a game on the console this call is expected to fail, and the home screen
           already says so; only report it when there is a game we failed to describe. */
        if (title_present) {
            Note(report, "nsGetApplicationControlData", rc);
        }
        return false;
    }
    /* The service decides how much it writes.  A full answer is NACP plus icon, so anything
       shorter than the field we read means the answer is not the structure we asked for and the
       string cannot be trusted. */
    constexpr std::size_t kVersionOffset = offsetof(NacpStruct, display_version);
    constexpr std::size_t kVersionSize = sizeof(NacpStruct::display_version);
    if (actual < kVersionOffset + kVersionSize) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "nsGetApplicationControlData returned %llu bytes",
                      static_cast<unsigned long long>(actual));
        NoteText(report, buf);
        return false;
    }
    const auto *nacp = reinterpret_cast<const NacpStruct *>(control);
    std::string text;
    for (std::size_t i = 0; i < kVersionSize; ++i) {
        const char c = nacp->display_version[i];
        if (c == '\0') {
            break;
        }
        text.push_back(c);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.pop_back();
    }
    for (unsigned char c : text) {
        if (c < 0x20 || c == 0x7F) {
            text.clear(); /* garbled NACP: say nothing rather than print control characters */
            break;
        }
    }
    report->game_display_version = text;
    return true;
}

}  // namespace

std::vector<ExefsFile> ListDirectory(FsFileSystem &sd, const char *path) {
    std::vector<ExefsFile> files;
    FsDir dir{};
    const util::FsPath arg(path);
    /* ReadDirs | ReadFiles everywhere: opening with "files only" silently fails on some
       directories, which is how an existence check can claim a directory is missing. */
    if (R_FAILED(fsFsOpenDirectory(&sd, arg.c_str(),
                                   FsDirOpenMode_ReadDirs | FsDirOpenMode_ReadFiles, &dir))) {
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

    /* Whether the content table answered at all: only then does "no Patch meta" prove that no
       update package is installed (the ncm step below reads that). */
    bool meta_list_ok = false;

    /* 1. ns: content table (version and patch storage) + whether an app is running */
    if (R_SUCCEEDED(nsInitialize())) {
        bool title_present = false;
        NsApplicationContentMetaStatus status[16]{};
        s32 count = 0;
        const Result rc = nsListApplicationContentMetaStatus(kAcnhTitleId, 0, status, 16, &count);
        if (R_SUCCEEDED(rc)) {
            meta_list_ok = true;
            for (s32 i = 0; i < count && i < 16; ++i) {
                report.metas.push_back({status[i].meta_type, status[i].storageID, status[i].version,
                                        status[i].application_id});
                if (status[i].meta_type == 0x80) { /* Application: the base game itself */
                    title_present = true;
                }
                if (status[i].meta_type == 0x81) { /* Patch */
                    report.patch_found = true;
                    report.patch_storage = status[i].storageID;
                }
            }
        } else {
            Note(&report, "nsListApplicationContentMetaStatus", rc);
        }
        /* "The game is on this console" is decided by presence, never by the version number: a
           bare 1.0.0 copy is an installed game whose title version is 0, and reading 0 as "not
           installed" is how a console that has 1.0.0 was told the game was missing (the details
           page said "version 2228224 != detected 0").

           Presence comes from the Application content meta above -- with one exception, the
           control data below, which is a *cache*: after an uninstall the console keeps serving
           the title's NACP for a while, so it must not be able to say "the game is here" on its
           own.  Measured: with the game uninstalled, `nsListApplicationContentMetaStatus`
           answered "no such application" (0x410) and `ncm` "no content meta" (0xE05) while the
           control-data call still succeeded with the old version -- and the home screen then
           reported a *version* problem instead of "the game is not installed".

           0x410 (0x00000410) is that "no such application" answer; anything else means the table
           could not answer, and then the control data is a legitimate second witness. */
        constexpr Result kNoSuchApplication = 0x00000410;
        const bool have_control_data = CollectControlData(&report, title_present);
        /* Which applications the console knows, and what each one holds.  An install that files a
           record under the wrong id leaves an extra entry here (the home menu draws it as an empty
           tile), and a record list that lost its own entry leaves a game the home menu does not
           draw at all; both are visible in this list and nowhere else. */
        {
            NsApplicationRecord apps[32]{};
            s32 app_count = 0;
            if (R_SUCCEEDED(nsListApplicationRecord(apps, 32, 0, &app_count))) {
                for (s32 i = 0; i < app_count && i < 32; ++i) {
                    char line[256];
                    NsApplicationContentMetaStatus metas[8]{};
                    s32 meta_count = 0;
                    std::string detail;
                    if (R_SUCCEEDED(nsListApplicationContentMetaStatus(apps[i].application_id, 0,
                                                                      metas, 8, &meta_count))) {
                        for (s32 m = 0; m < meta_count && m < 8; ++m) {
                            char one[64];
                            std::snprintf(one, sizeof(one), " %u:v=0x%x@%u",
                                          static_cast<unsigned>(metas[m].meta_type),
                                          metas[m].version,
                                          static_cast<unsigned>(metas[m].storageID));
                            detail += one;
                        }
                    }
                    std::snprintf(line, sizeof(line), "ns app %016llx events=%u metas=%d%s",
                                  static_cast<unsigned long long>(apps[i].application_id),
                                  static_cast<unsigned>(apps[i].last_event),
                                  static_cast<int>(meta_count), detail.c_str());
                    report.notes.emplace_back(line);
                }
            } else {
                report.notes.emplace_back("ns app list unavailable");
            }
        }
        report.application_running = IsAcnhProcessRunning();
        if (title_present || (have_control_data && rc != kNoSuchApplication)) {
            report.build.title_id = TitleIdHex(kAcnhTitleId);
        }
    } else {
        report.problems += "nsInitialize failed";
    }

    /* 2. ncm: the update title's Program content id (the gate's primary input) */
    CollectNcm(&report, meta_list_ok && !report.patch_found);

    /* 3. dmnt:cht: main ModuleId while the game runs (the strengthened input) */
    if (report.application_running) {
        CollectDmnt(&report);
    }

    /* Override configuration: whether a key must be held at launch */
    std::string override_text;
    if (ReadTextFile(sd, kOverrideConfig, &override_text, kConfigFileMaxBytes)) {
        report.have_override_config = true;
        const OverrideConfig config = ParseOverrideConfig(override_text);
        OverrideKey title_key;
        std::string title_text;
        if (ReadTextFile(sd, kTitleConfig, &title_text, kConfigFileMaxBytes)) {
            report.have_title_config = true;
            title_key = ParseTitleConfig(title_text);
        }
        report.advice = Advise(config, title_key);
    } else {
        report.advice = Advise(OverrideConfig{}, OverrideKey{});
    }

    /* Current install state and the legacy-cheat check.  Both halves of the check live in
       env/cheat_file.* and are host-tested: the file has to carry the name dmnt loads, and one of
       its entries has to be the chat-code one.  A pack of unrelated entries is not a conflict, and
       a file whose contents cannot be read is reported anyway -- an unreadable cheat file is not a
       reason to stay quiet.  (The name test used to ask for 32 hex digits -- "36 characters" --
       which is a shape no card carries, so nothing was ever reported at all.) */
    report.exefs = ListDirectory(sd, kExefsDir);
    for (const ExefsFile &file : ListDirectory(sd, kCheatsDir)) {
        if (!IsLegacyCheatFileName(file.name)) {
            continue;
        }
        std::string text;
        const std::string path = std::string(kCheatsDir) + "/" + file.name;
        if (ReadTextFile(sd, path.c_str(), &text, kCheatFileMaxBytes)) {
            const std::string entry = ChatCodeCheatEntry(text);
            if (entry.empty()) {
                continue; /* unrelated entries only: not a conflict, nothing to say */
            }
            report.legacy_cheat_entry = entry;
        }
        report.legacy_cheat_present = true;
        report.legacy_cheat_path = path;
        break;
    }
    return report;
}

}  // namespace acnh_manager::env
