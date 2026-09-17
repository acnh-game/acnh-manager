#include "probe.hpp"

#include <cinttypes>
#include <cstdio>
#include <cstring>

/* M0 环境探测。这里的每条探针都在真机上跑过并把结果写进了 docs/architecture.md:
 *   - ProbeNcm / ProbeSdState / dmnt 探针 = 现在的判据来源,会在 M1 演进取环境检查模块;
 *   - ProbeServiceAccess / ProbeFspLdrSteps / ProbeCodeFs / ProbeFsVariants 记录了被否掉的路线
 *     (fsp-ldr 与按 id 挂载 code FS),保留是为了让"为什么不用它"这件事可复现,不要照抄进 App。
 * 日志里的结果码一律按 0x%08X 打印,便于与文档里的错误码逐字对照。 */
namespace acnh_manager::probe {
namespace {

/* Atmosphere 的 dmnt:cht 元数据布局:process_id、program_id、四个内存区间、
   最后是 main 的 ModuleId;sizeof == 0x70。 */
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

/* 列出 code FS 根目录条目,确认能看到 main / rtld / sdk / main.npdm。 */
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

/* 读 /main 头部 0x60 字节,校验 NSO0 并打印 ModuleId(+0x40, 0x20 字节)。 */
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

/* 读 /main.npdm,打印大小与 sha256(与发布 payload 的派生 NPDM 对照)。 */
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

/* 哪些服务能拿到、能否转 domain ——用来区分"访问被拒"与"服务本身特殊"。 */
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

/* 把 libnx fsldrInitialize 的四步拆开:定位 0x615 到底出在哪一步。 */
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

/* 备选路径:lr 解析出的 content path + fsp-srv 的 OpenFileSystemWithId / WithPatch。 */
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

/* ncm:列出某个标题的最新 content meta 与它的内容 id(与 lr 路径里的 id 交叉复核)。 */
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

/* 游戏进程存在时,用 dmnt:cht 读 main 的 ModuleId 做交叉校验。 */
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

/* SD 上现有 exefs 覆盖状态的只读快照。 */
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

}  // namespace acnh_manager::probe
