#include "probe.hpp"

#include <cinttypes>
#include <cstdio>
#include <cstring>

/* M0 environment probes.  Every probe here ran on real hardware and its result is recorded
 * in docs/architecture.md:
 *   - ProbeNcm / ProbeSdState / dmnt probes are the source of today's verdicts and evolve
 *     into the environment-check module;
 *   - ProbeServiceAccess / ProbeFspLdrSteps / ProbeCodeFs / ProbeFsVariants record routes we
 *     rejected (fsp-ldr and mounting the code FS by id).  They stay so that "why not this"
 *     remains reproducible -- do not copy them into the app.  Result codes are always logged
 *     as 0x%08X so they can be matched against the docs verbatim. */
namespace acnh_manager::probe {
namespace {

/* Atmosphere's dmnt:cht metadata layout: process_id, program_id, four memory ranges and
   finally main's ModuleId; sizeof == 0x70. */
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

void Hex(char *out, const u8 *data, size_t size) {
    static const char digits[] = "0123456789ABCDEF";
    for (size_t i = 0; i < size; ++i) {
        out[i * 2] = digits[data[i] >> 4];
        out[i * 2 + 1] = digits[data[i] & 0xF];
    }
    out[size * 2] = '\0';
}

const char *MetaTypeName(u8 type) {
    switch (type) {
        case 0x80: return "Application";
        case 0x81: return "Patch";
        case 0x82: return "AddOnContent";
        case 0x83: return "Delta";
        default: return "?";
    }
}

const char *AppletTypeName(AppletType type) {
    switch (type) {
        case AppletType_Application: return "Application";
        case AppletType_SystemApplication: return "SystemApplication";
        case AppletType_LibraryApplet: return "LibraryApplet";
        case AppletType_SystemApplet: return "SystemApplet";
        case AppletType_OverlayApplet: return "OverlayApplet";
        default: return "?";
    }
}

/* List the code FS root to confirm main / rtld / sdk / main.npdm are visible. */
void ListRoot(Log &log, const char *label, FsFileSystem &fs) {
    FsDir dir{};
    const u32 mode = FsDirOpenMode_ReadFiles | FsDirOpenMode_ReadDirs;
    if (R_FAILED(fsFsOpenDirectory(&fs, "/", mode, &dir))) {
        log.Line("%s: list / failed", label);
        return;
    }
    FsDirectoryEntry entries[32]{};
    s64 total = 0;
    const Result rc = fsDirRead(&dir, &total, 32, entries);
    if (R_SUCCEEDED(rc)) {
        char names[400] = {0};
        size_t used = 0;
        for (s64 i = 0; i < total && i < 32; ++i) {
            const size_t len = std::strlen(entries[i].name);
            if (used + len + 2 >= sizeof(names)) {
                break;
            }
            std::memcpy(names + used, entries[i].name, len);
            used += len;
            names[used++] = (i + 1 < total) ? ',' : '\0';
        }
        log.Line("%s: code fs root: %lld entries: %s", label,
                 static_cast<long long>(total), names);
    } else {
        log.Line("%s: fsDirRead rc=0x%08X", label, rc);
    }
    fsDirClose(&dir);
}

/* Read the first 0x60 bytes of /main, check the NSO0 magic and print the ModuleId
   (+0x40, 0x20 bytes). */
void ReadMainModuleId(Log &log, const char *label, FsFileSystem &fs) {
    FsFile file{};
    if (R_FAILED(fsFsOpenFile(&fs, "/main", FsOpenMode_Read, &file))) {
        log.Line("%s: open /main failed", label);
        return;
    }
    u8 header[0x60]{};
    u64 read = 0;
    const Result rc = fsFileRead(&file, 0, header, sizeof(header), 0, &read);
    fsFileClose(&file);
    if (R_FAILED(rc) || read != sizeof(header)) {
        log.Line("%s: read /main rc=0x%08X read=%llu", label, rc,
                 static_cast<unsigned long long>(read));
        return;
    }
    if (std::memcmp(header, "NSO0", 4) != 0) {
        log.Line("%s: /main has no NSO0 magic", label);
        return;
    }
    char hex[0x41];
    Hex(hex, header + 0x40, 0x20);
    log.Line("%s: /main NSO0 module_id=%s", label, hex);
}

/* Read /main.npdm and print its size and sha256 (cross-check against the released NPDM). */
void ReadNpdm(Log &log, const char *label, FsFileSystem &fs) {
    FsFile file{};
    if (R_FAILED(fsFsOpenFile(&fs, "/main.npdm", FsOpenMode_Read, &file))) {
        log.Line("%s: open /main.npdm failed", label);
        return;
    }
    static u8 buffer[0x4000];
    u64 read = 0;
    const Result rc = fsFileRead(&file, 0, buffer, sizeof(buffer), 0, &read);
    fsFileClose(&file);
    if (R_FAILED(rc)) {
        log.Line("%s: read /main.npdm rc=0x%08X", label, rc);
        return;
    }
    u8 hash[SHA256_HASH_SIZE]{};
    sha256CalculateHash(hash, buffer, read);
    char hex[0x41];
    Hex(hex, hash, sizeof(hash));
    log.Line("%s: /main.npdm size=%llu sha256=%s", label,
             static_cast<unsigned long long>(read), hex);
}

void ProbeCodeFs(Log &log, const char *label, u64 tid, NcmStorageId storage,
                 FsContentAttributes attr) {
    FsCodeInfo info{};
    FsFileSystem fs{};
    const Result rc = fsldrOpenCodeFileSystem(&info, tid, storage, nullptr, attr, &fs);
    log.Line("%s: fsldrOpenCodeFileSystem rc=0x%08X", label, rc);
    if (R_FAILED(rc)) {
        return;
    }
    char hex[0x21];
    Hex(hex, info.hash, 8);
    log.Line("%s: is_signed=%d code_hash_prefix=%s", label, info.is_signed ? 1 : 0, hex);
    ListRoot(log, label, fs);
    ReadMainModuleId(log, label, fs);
    ReadNpdm(log, label, fs);
    fsFsClose(&fs);
}

/* Which services can be opened and whether they convert to domains -- this separates
   "access denied" from "this service is special". */
void ProbeServiceAccess(Log &log) {
    const char *names[] = {"fsp-ldr", "fsp-srv", "lr", "ncm", "pl:u", "dmnt:cht", "spl:"};
    for (const char *name : names) {
        Service service{};
        const Result rc = smGetService(&service, name);
        if (R_FAILED(rc)) {
            log.Line("svc %-9s open rc=0x%08X", name, rc);
            continue;
        }
        const Result rc_domain = serviceConvertToDomain(&service);
        log.Line("svc %-9s open rc=0x0 domain rc=0x%08X", name, rc_domain);
        serviceClose(&service);
    }
}

/* Split libnx's fsldrInitialize into its four steps to find out where 0x615 comes from. */
void ProbeFspLdrSteps(Log &log, u64 tid, NcmStorageId storage) {
    Service ldr{};
    Result rc = smGetService(&ldr, "fsp-ldr");
    log.Line("fsp-ldr step1 smGetService rc=0x%08X", rc);
    if (R_FAILED(rc)) {
        return;
    }
    rc = serviceConvertToDomain(&ldr);
    log.Line("fsp-ldr step2 ConvertToDomain rc=0x%08X", rc);
    if (R_FAILED(rc)) {
        serviceClose(&ldr);
        return;
    }
    serviceAssumeDomain(&ldr);
    u64 pid_placeholder = 0;
    const Result rc_pid = serviceDispatchIn(&ldr, 2, pid_placeholder, .in_send_pid = true);
    log.Line("fsp-ldr step3 SetCurrentProcess(cmd2) rc=0x%08X", rc_pid);

    FsCodeInfo info{};
    FsFileSystem fs{};
    const struct {
        u8 attr;
        u8 storage_id;
        u64 tid;
    } in = {FsContentAttributes_None, static_cast<u8>(storage), tid};
    const Result rc_open = serviceDispatchIn(&ldr, 0, in,
        .buffer_attrs = {SfBufferAttr_HipcMapAlias | SfBufferAttr_Out},
        .buffers = {{&info, sizeof(info)}},
        .out_num_objects = 1,
        .out_objects = &fs.s);
    log.Line("fsp-ldr step4 OpenCodeFileSystem(cmd0) rc=0x%08X", rc_open);
    if (R_SUCCEEDED(rc_open)) {
        ListRoot(log, "fsp-ldr-raw", fs);
        ReadMainModuleId(log, "fsp-ldr-raw", fs);
        ReadNpdm(log, "fsp-ldr-raw", fs);
        fsFsClose(&fs);
    }
    serviceClose(&ldr);
}

/* Alternative route: the content path resolved by lr plus fsp-srv's OpenFileSystemWithId /
   WithPatch. */
void ProbeFsVariants(Log &log, u64 tid, NcmStorageId storage) {
    char resolved[FS_MAX_PATH] = {0};
    if (R_SUCCEEDED(lrInitialize())) {
        LrLocationResolver lr{};
        const Result rc_lr = lrOpenLocationResolver(storage, &lr);
        log.Line("lr: OpenLocationResolver(storage=%u) rc=0x%08X", static_cast<unsigned>(storage), rc_lr);
        if (R_SUCCEEDED(rc_lr)) {
            const Result rc_path = lrLrResolveProgramPath(&lr, tid, resolved);
            log.Line("lr: ResolveProgramPath rc=0x%08X path=\"%s\"", rc_path, resolved);
            serviceClose(&lr.s);
        }
        lrExit();
    } else {
        log.Line("lr: initialize failed");
    }

    struct Variant {
        u32 type;
        const char *label;
    };
    const Variant variants[] = {{0, "type0(Code?)"}, {1, "type1(Rom?)"}, {8, "type8(RegisteredUpdate)"}};
    for (const Variant &variant : variants) {
        FsFileSystem fs{};
        const Result rc_patch = fsOpenFileSystemWithPatch(&fs, tid, static_cast<FsFileSystemType>(variant.type));
        log.Line("fsOpenFileSystemWithPatch(%s) rc=0x%08X", variant.label, rc_patch);
        if (R_SUCCEEDED(rc_patch)) {
            ListRoot(log, "patch-fs", fs);
            ReadMainModuleId(log, "patch-fs", fs);
            fsFsClose(&fs);
        }
        FsFileSystem fs_id{};
        const Result rc_id = fsOpenFileSystemWithId(&fs_id, tid, static_cast<FsFileSystemType>(variant.type),
                                                   resolved, FsContentAttributes_None);
        log.Line("fsOpenFileSystemWithId(%s, lr-path) rc=0x%08X", variant.label, rc_id);
        if (R_SUCCEEDED(rc_id)) {
            ListRoot(log, "id-fs", fs_id);
            ReadMainModuleId(log, "id-fs", fs_id);
            fsFsClose(&fs_id);
        }
    }

    FsFileSystem data_fs{};
    const Result rc_data = fsOpenDataFileSystemByProgramId(&data_fs, tid);
    log.Line("fsOpenDataFileSystemByProgramId rc=0x%08X", rc_data);
    if (R_SUCCEEDED(rc_data)) {
        ListRoot(log, "data-fs", data_fs);
        fsFsClose(&data_fs);
    }
}

/* ncm: list one title's latest content meta and its content id (cross-checked against the
   id in the lr path). */
void ProbeNcm(Log &log, u64 tid, const char *label) {
    const Result rc_init = ncmInitialize();
    log.Line("ncm[%s]: initialize rc=0x%08X", label, rc_init);
    if (R_FAILED(rc_init)) {
        return;
    }
    NcmContentMetaDatabase db{};
    const Result rc_db = ncmOpenContentMetaDatabase(&db, NcmStorageId_SdCard);
    log.Line("ncm[%s]: open meta db(sd) rc=0x%08X", label, rc_db);
    if (R_FAILED(rc_db)) {
        return;
    }
    NcmContentMetaKey key{};
    const Result rc_key = ncmContentMetaDatabaseGetLatestContentMetaKey(&db, &key, tid);
    log.Line("ncm[%s]: latest key rc=0x%08X id=%016" PRIX64 " version=%u(%#x) type=%#04x install=%u",
             label, rc_key, key.id, key.version, key.version, key.type, key.install_type);
    if (R_SUCCEEDED(rc_key)) {
        NcmContentInfo infos[8]{};
        s32 written = 0;
        const Result rc_list = ncmContentMetaDatabaseListContentInfo(&db, &written, infos, 8, &key, 0);
        log.Line("ncm[%s]: list content info rc=0x%08X count=%d", label, rc_list, written);
        for (s32 i = 0; i < written && i < 8; ++i) {
            char hex[0x21];
            Hex(hex, infos[i].content_id.c, 0x10);
            log.Line("  content[%d] id=%s type=%u id_offset=%u", i, hex, infos[i].content_type,
                     infos[i].id_offset);
        }
    }
    serviceClose(&db.s);
}

/* While the game process exists, read main's ModuleId through dmnt:cht as a cross-check. */
void ProbeDmnt(Log &log) {
    Service service{};
    const Result rc = smGetService(&service, "dmnt:cht");
    log.Line("dmnt:cht: open rc=0x%08X", rc);
    if (R_FAILED(rc)) {
        return;
    }
    const Result rc_force = serviceDispatch(&service, 65003); /* ForceOpenCheatProcess */
    CheatProcessMetadata metadata{};
    const Result rc_meta = serviceDispatchOut(&service, 65002, metadata);
    log.Line("dmnt:cht: ForceOpen rc=0x%08X GetMetadata rc=0x%08X", rc_force, rc_meta);
    if (R_SUCCEEDED(rc_meta)) {
        char hex[0x41];
        Hex(hex, metadata.main_nso_module_id, sizeof(metadata.main_nso_module_id));
        log.Line("dmnt:cht: program_id=%016" PRIX64 " process_id=%llu module_id=%s",
                 metadata.program_id, static_cast<unsigned long long>(metadata.process_id), hex);
    }
    serviceClose(&service);
}

/* Read-only snapshot of the exefs override currently on the SD card. */
void ProbeSdState(Log &log, FsFileSystem &sd) {
    const char *dir = "/atmosphere/contents/01006F8002326000/exefs";
    FsDir handle{};
    const u32 mode = FsDirOpenMode_ReadFiles | FsDirOpenMode_ReadDirs;
    if (R_FAILED(fsFsOpenDirectory(&sd, dir, mode, &handle))) {
        log.Line("exefs override: %s not present", dir);
        return;
    }
    FsDirectoryEntry entries[16]{};
    s64 total = 0;
    if (R_SUCCEEDED(fsDirRead(&handle, &total, 16, entries))) {
        for (s64 i = 0; i < total && i < 16; ++i) {
            if (entries[i].type != FsDirEntryType_File) {
                continue;
            }
            log.Line("exefs override: %s size=%lld", entries[i].name,
                     static_cast<long long>(entries[i].file_size));
        }
    }
    fsDirClose(&handle);
}

}  // namespace

void Run(Log &log, FsFileSystem &sd) {
    const u32 hos = hosversionGet();
    log.Line("=== ACNH-Manager M0 spike ===");
    log.Line("HOS %u.%u.%u (raw 0x%08X)", (hos >> 16) & 0xFF, (hos >> 8) & 0xFF, hos & 0xFF, hos);
    const AppletType applet_type = appletGetAppletType();
    log.Line("applet type=%s(%d)", AppletTypeName(applet_type), static_cast<int>(applet_type));
    log.Line("targets: base=%016" PRIX64 " update=%016" PRIX64, kAcnhTitleId, kAcnhUpdateTitleId);

    bool app_running = false;
    NcmStorageId patch_storage = NcmStorageId_None;
    bool patch_found = false;

    const Result rc_ns = nsInitialize();
    log.Line("nsInitialize rc=0x%08X", rc_ns);
    if (R_SUCCEEDED(rc_ns)) {
        NsApplicationContentMetaStatus status[16]{};
        s32 count = 0;
        const Result rc = nsListApplicationContentMetaStatus(kAcnhTitleId, 0, status, 16, &count);
        log.Line("nsListApplicationContentMetaStatus rc=0x%08X count=%d", rc, count);
        if (R_SUCCEEDED(rc)) {
            for (s32 i = 0; i < count && i < 16; ++i) {
                log.Line("  meta[%d] type=%s(%#04x) storage=%u version=%u(%#x) app_id=%016" PRIX64,
                         i, MetaTypeName(status[i].meta_type), status[i].meta_type,
                         status[i].storageID, status[i].version, status[i].version,
                         status[i].application_id);
                if (status[i].meta_type == 0x81) {
                    patch_storage = static_cast<NcmStorageId>(status[i].storageID);
                    patch_found = true;
                }
            }
        }
        if (R_SUCCEEDED(nsIsAnyApplicationRunning(&app_running))) {
            log.Line("application running=%d", app_running ? 1 : 0);
        }
    }

    ProbeSdState(log, sd);

    if (app_running) {
        ProbeDmnt(log);
    } else {
        log.Line("dmnt:cht: skipped (no application running)");
    }

    const NcmStorageId target_storage = patch_found ? patch_storage : NcmStorageId_SdCard;
    ProbeServiceAccess(log);
    ProbeFspLdrSteps(log, kAcnhTitleId, target_storage);
    ProbeFsVariants(log, kAcnhTitleId, target_storage);
    ProbeNcm(log, kAcnhUpdateTitleId, "update");
    ProbeNcm(log, kAcnhTitleId, "base");

    const Result rc_ldr = fsldrInitialize();
    log.Line("fsldrInitialize rc=0x%08X", rc_ldr);
    if (R_SUCCEEDED(rc_ldr)) {
        static char labels[10][48];
        struct Candidate {
            const char *label;
            u64 tid;
            NcmStorageId storage;
        };
        Candidate candidates[10]{};
        int n = 0;
        const NcmStorageId storages[4] = {patch_found ? patch_storage : NcmStorageId_None,
                                         NcmStorageId_BuiltInUser, NcmStorageId_SdCard,
                                         NcmStorageId_None};
        const char *storage_names[4] = {patch_found ? "patch-storage" : "patch-missing",
                                        "builtin-user", "sdcard", "none"};
        const u64 tids[2] = {kAcnhTitleId, kAcnhUpdateTitleId};
        const char *tid_names[2] = {"base", "update"};
        for (int t = 0; t < 2; ++t) {
            for (int s = 0; s < 4 && n < 10; ++s) {
                std::snprintf(labels[n], sizeof(labels[0]), "%s/%s", tid_names[t],
                              storage_names[s]);
                candidates[n] = {labels[n], tids[t], storages[s]};
                ++n;
            }
        }
        for (int i = 0; i < n; ++i) {
            char label[64];
            std::snprintf(label, sizeof(label), "code[%s attr=None]", candidates[i].label);
            ProbeCodeFs(log, label, candidates[i].tid, candidates[i].storage,
                        FsContentAttributes_None);
        }
        for (int i = 0; i < n; ++i) {
            char label[64];
            std::snprintf(label, sizeof(label), "code[%s attr=All]", candidates[i].label);
            ProbeCodeFs(log, label, candidates[i].tid, candidates[i].storage,
                        FsContentAttributes_All);
        }
    }

    log.Line("=== spike done ===");
}

/* Write probe: run create->SetSize->write->flush->read->delete on one path and log every
   step's return code.  Both a relative and an absolute form are tried, which separates
   "FS refuses relative paths" from "this directory is not writable". */
void ProbeWritePath(Log &log, FsFileSystem &sd, const char *label, const char *path) {
    log.Line("wprobe[%s] path=%s", label, path);
    fsFsDeleteFile(&sd, path);

    const Result rc_create = fsFsCreateFile(&sd, path, 4, 0);
    log.Line("wprobe[%s]   create rc=0x%08X", label, rc_create);
    if (R_FAILED(rc_create)) {
        return;
    }
    FsFile file{};
    Result rc = fsFsOpenFile(&sd, path, FsOpenMode_Write, &file);
    log.Line("wprobe[%s]   open rc=0x%08X", label, rc);
    if (R_SUCCEEDED(rc)) {
        rc = fsFileSetSize(&file, 4);
        log.Line("wprobe[%s]   setsize rc=0x%08X", label, rc);
    }
    const u8 payload[4] = {'A', 'C', 'N', 'H'};
    if (R_SUCCEEDED(rc)) {
        rc = fsFileWrite(&file, 0, payload, sizeof(payload), FsWriteOption_Flush);
        log.Line("wprobe[%s]   write rc=0x%08X", label, rc);
    }
    if (R_SUCCEEDED(rc)) {
        rc = fsFileFlush(&file);
        log.Line("wprobe[%s]   flush rc=0x%08X", label, rc);
    }
    fsFileClose(&file);
    if (R_SUCCEEDED(rc)) {
        rc = fsFsOpenFile(&sd, path, FsOpenMode_Read, &file);
        log.Line("wprobe[%s]   reopen rc=0x%08X", label, rc);
        if (R_SUCCEEDED(rc)) {
            u8 read_back[4] = {0};
            u64 read = 0;
            const Result rc_read = fsFileRead(&file, 0, read_back, sizeof(read_back), 0, &read);
            fsFileClose(&file);
            const bool match = rc_read == 0 && read == sizeof(payload) &&
                               std::memcmp(read_back, payload, sizeof(payload)) == 0;
            log.Line("wprobe[%s]   read rc=0x%08X read=%llu match=%s", label, rc_read,
                     static_cast<unsigned long long>(read), match ? "yes" : "no");
        }
    }
    const Result rc_delete = fsFsDeleteFile(&sd, path);
    log.Line("wprobe[%s]   delete rc=0x%08X", label, rc_delete);
}

void RunWriteProbe(Log &log, FsFileSystem &sd) {
    const char *kExefs = "/atmosphere/contents/01006F8002326000/exefs";
    log.Line("=== write probe ===");
    ProbeWritePath(log, sd, "home-abs", "/switch/ACNH-Manager/probe-abs.tmp");
    ProbeWritePath(log, sd, "home-rel", "switch/ACNH-Manager/probe-rel.tmp");
    {
        char path[160];
        std::snprintf(path, sizeof(path), "%s/.acnh-probe-abs.tmp", kExefs);
        ProbeWritePath(log, sd, "exefs-abs", path);
    }
    {
        char path[160];
        std::snprintf(path, sizeof(path), "%s/.acnh-probe-rel.tmp", kExefs + 1);
        ProbeWritePath(log, sd, "exefs-rel", path);
    }
    log.Line("=== write probe done ===");
}

}  // namespace acnh_manager::probe
