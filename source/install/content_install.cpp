#include "install/content_install.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstdlib>
#include <cstring>

#include "install/nca.hpp"
#include "util/fs_path.hpp"

namespace acnh_manager::install {
namespace {

std::string Describe(const char *what, Result rc) {
    char buffer[128];
    std::snprintf(buffer, sizeof(buffer), "%s failed (0x%08x)", what, static_cast<unsigned>(rc));
    return buffer;
}

NcmStorageId ToNcmStorage(TargetStorage target) {
    return target == TargetStorage::Nand ? NcmStorageId_BuiltInUser : NcmStorageId_SdCard;
}

FsContentStorageId ToFsContentStorage(TargetStorage target) {
    return target == TargetStorage::Nand ? FsContentStorageId_User : FsContentStorageId_SdCard;
}

}  // namespace

std::string ToHex(const void *bytes, std::size_t size) {
    static const char kDigits[] = "0123456789abcdef";
    const auto *data = static_cast<const std::uint8_t *>(bytes);
    std::string out;
    out.reserve(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        out.push_back(kDigits[data[i] >> 4]);
        out.push_back(kDigits[data[i] & 0x0F]);
    }
    return out;
}

namespace {

bool FromHex(const std::string &hex, std::uint8_t *out, std::size_t size) {
    if (hex.size() != size * 2) {
        return false;
    }
    auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        if (c >= 'A' && c <= 'F') {
            return c - 'A' + 10;
        }
        return -1;
    };
    for (std::size_t i = 0; i < size; ++i) {
        const int high = digit(hex[i * 2]);
        const int low = digit(hex[i * 2 + 1]);
        if (high < 0 || low < 0) {
            return false;
        }
        out[i] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return true;
}

}  // namespace

bool ContentIdFromHex(const std::string &hex, NcmContentId *out) {
    return FromHex(hex, out->c, sizeof(out->c));
}

bool PlaceholderIdFromHex(const std::string &hex, NcmPlaceHolderId *out) {
    return FromHex(hex, out->uuid.uuid, sizeof(out->uuid.uuid));
}

ContentInstaller::ContentInstaller(TargetStorage target, const InstallOptions &options)
    : m_target(target), m_options(options) {}

ContentInstaller::~ContentInstaller() {
    if (m_open) {
        ncmContentMetaDatabaseClose(&m_meta);
        ncmContentStorageClose(&m_storage);
    }
    m_services.Close();
}

NcmStorageId ContentInstaller::StorageId() const { return ToNcmStorage(m_target); }

bool ContentInstaller::Space(TargetStorage target, std::uint64_t *free_bytes,
                             std::uint64_t *total_bytes, std::string *error) {
    /* The UI's storage bars and the space check share this function on purpose: two sources for
       "how much is left" is how a screen ends up disagreeing with the check that stopped the
       install (design section 9). */
    FsFileSystem content_fs{};
    Result rc = fsOpenContentStorageFileSystem(&content_fs, ToFsContentStorage(target));
    if (R_FAILED(rc)) {
        *error = Describe("opening the content storage", rc);
        return false;
    }
    s64 free_space = 0;
    s64 total_space = 0;
    rc = fsFsGetFreeSpace(&content_fs, "/", &free_space);
    if (R_SUCCEEDED(rc)) {
        rc = fsFsGetTotalSpace(&content_fs, "/", &total_space);
    }
    fsFsClose(&content_fs);
    if (R_FAILED(rc)) {
        *error = Describe("reading the content storage space", rc);
        return false;
    }
    *free_bytes = free_space > 0 ? static_cast<std::uint64_t>(free_space) : 0;
    *total_bytes = total_space > 0 ? static_cast<std::uint64_t>(total_space) : 0;
    return true;
}

bool ContentInstaller::EnsureOpen(std::string *error) {
    if (m_open) {
        return true;
    }
    Result rc = ncmInitialize();
    if (R_FAILED(rc)) {
        *error = Describe("opening the ncm service", rc);
        return false;
    }
    rc = ncmOpenContentStorage(&m_storage, ToNcmStorage(m_target));
    if (R_FAILED(rc)) {
        *error = Describe("opening the content storage", rc);
        return false;
    }
    rc = ncmOpenContentMetaDatabase(&m_meta, ToNcmStorage(m_target));
    if (R_FAILED(rc)) {
        ncmContentStorageClose(&m_storage);
        *error = Describe("opening the content meta database", rc);
        return false;
    }
    if (!m_services.Open(error)) {
        ncmContentMetaDatabaseClose(&m_meta);
        ncmContentStorageClose(&m_storage);
        return false;
    }
    m_open = true;
    return true;
}

bool ContentInstaller::Open(std::string *error) { return EnsureOpen(error); }

bool ContentInstaller::AnyApplicationRunning(bool *out, std::string *error) {
    *out = false;
    const Result dmnt = pmdmntInitialize();
    if (R_FAILED(dmnt)) {
        *error = Describe("opening the process monitor (pm:dmnt)", dmnt);
        return false;
    }
    u64 pid = 0;
    const Result rc = pmdmntGetApplicationProcessId(&pid);
    pmdmntExit();
    if (R_FAILED(rc)) {
        /* "no application is running" comes back as a failed lookup, which is the answer the
           installer wants, not an error to report. */
        return true;
    }
    *out = pid != 0;
    return true;
}

bool ContentInstaller::ApplicationRunning(const std::string &title_id_hex, bool *out,
                                          std::string *error) {
    /* The title family, not just the base game: a patch id and a DLC id share the base title's
       high bits, so comparing the base id against the running program id would miss "the game is
       running" whenever the running process is the update. */
    *out = false;
    const std::uint64_t wanted = std::strtoull(title_id_hex.c_str(), nullptr, 16);
    if (wanted == 0) {
        *error = "the title id is not 16 hex digits";
        return false;
    }
    const Result dmnt = pmdmntInitialize();
    if (R_FAILED(dmnt)) {
        *error = Describe("opening the process monitor (pm:dmnt)", dmnt);
        return false;
    }
    u64 pid = 0;
    const Result running = pmdmntGetApplicationProcessId(&pid);
    if (R_FAILED(running) || pid == 0) {
        /* Nothing is running: the common case, and the one the installer wants. */
        pmdmntExit();
        return true;
    }
    const Result info = pminfoInitialize();
    if (R_FAILED(info)) {
        pmdmntExit();
        *error = Describe("opening the process info service (pm:info)", info);
        return false;
    }
    u64 program_id = 0;
    const Result rc = pminfoGetProgramId(&program_id, pid);
    pminfoExit();
    pmdmntExit();
    if (R_FAILED(rc)) {
        *error = Describe("reading the running program id", rc);
        return false;
    }
    /* 01006F8002326000 -> base; the low bits identify the content kind, so the family is the
       high 48 bits. */
    *out = (program_id >> 16) == (wanted >> 16);
    return true;
}

bool ContentInstaller::InstalledVersionPerStorage(const std::string &title_id_hex,
                                                  std::uint32_t *sd, std::uint32_t *nand,
                                                  std::string *error) {
    *sd = 0;
    *nand = 0;
    const std::uint64_t id = std::strtoull(title_id_hex.c_str(), nullptr, 16);
    if (id == 0) {
        *error = "the title id is not 16 hex digits";
        return false;
    }
    const struct {
        NcmStorageId storage;
        std::uint32_t *out;
    } targets[2] = {{NcmStorageId_SdCard, sd}, {NcmStorageId_BuiltInUser, nand}};
    for (const auto &target : targets) {
        NcmContentMetaDatabase db{};
        if (R_FAILED(ncmOpenContentMetaDatabase(&db, target.storage))) {
            /* A storage that cannot be opened simply has nothing to report. */
            continue;
        }
        NcmContentMetaKey key{};
        if (R_SUCCEEDED(ncmContentMetaDatabaseGetLatestContentMetaKey(&db, &key, id))) {
            *target.out = key.version;
        }
        ncmContentMetaDatabaseClose(&db);
    }
    return true;
}

bool ContentInstaller::InstalledState(const std::string &title_id_hex, std::uint32_t version,
                                      std::uint8_t meta_type, bool *same_version_installed,
                                      std::uint32_t *highest_version, std::string *error) {
    *same_version_installed = false;
    *highest_version = 0;
    const std::uint64_t id = std::strtoull(title_id_hex.c_str(), nullptr, 16);
    if (id == 0) {
        *error = "the title id is not 16 hex digits";
        return false;
    }
    /* The key the question is about: exactly the record this package would write. */
    NcmContentMetaKey wanted{};
    wanted.id = id;
    wanted.version = version;
    wanted.type = meta_type;
    wanted.install_type = 0; /* NcmContentInstallType_Full, the kind every install writes */

    const NcmStorageId storages[2] = {NcmStorageId_SdCard, NcmStorageId_BuiltInUser};
    for (const NcmStorageId storage : storages) {
        NcmContentMetaDatabase db{};
        const Result open_rc = ncmOpenContentMetaDatabase(&db, storage);
        m_finish_trace.push_back("query open storage=" + std::to_string(static_cast<int>(storage)) +
                                 " rc=0x" + ToHex(&open_rc, sizeof(open_rc)));
        if (R_FAILED(open_rc)) {
            continue;
        }
        /* "Is this exact version and kind here?"  A record's identity is its key, so the answer
           comes from the key itself and not from a version comparison. */
        bool has = false;
        const Result has_rc = ncmContentMetaDatabaseHas(&db, &has, &wanted);
        {
            char note[160];
            std::snprintf(note, sizeof(note),
                          "query has id=%016llx v=0x%x type=%u rc=0x%x has=%d",
                          static_cast<unsigned long long>(id), static_cast<unsigned>(version),
                          static_cast<unsigned>(meta_type), static_cast<unsigned>(has_rc),
                          has ? 1 : 0);
            m_finish_trace.push_back(note);
        }
        if (R_SUCCEEDED(has_rc) && has) {
            *same_version_installed = true;
        }
        /* "How new is what is here?"  Keyed by the same id, so a patch answers about the patch. */
        NcmContentMetaKey latest{};
        const Result latest_rc = ncmContentMetaDatabaseGetLatestContentMetaKey(&db, &latest, id);
        {
            char note[160];
            std::snprintf(note, sizeof(note),
                          "query latest id=%016llx rc=0x%x v=0x%x type=%u",
                          static_cast<unsigned long long>(id), static_cast<unsigned>(latest_rc),
                          static_cast<unsigned>(latest.version), static_cast<unsigned>(latest.type));
            m_finish_trace.push_back(note);
        }
        if (R_SUCCEEDED(latest_rc)) {
            if (latest.version > *highest_version) {
                *highest_version = latest.version;
            }
            if (latest.version == version && latest.type == meta_type) {
                *same_version_installed = true;
            }
        }
        ncmContentMetaDatabaseClose(&db);
    }
    return true;
}

bool ContentInstaller::InstalledVersion(const std::string &title_id_hex, std::uint32_t *out,
                                        std::string *error) {
    std::uint32_t sd = 0;
    std::uint32_t nand = 0;
    if (!InstalledVersionPerStorage(title_id_hex, &sd, &nand, error)) {
        return false;
    }
    *out = sd > nand ? sd : nand;
    return true;
}

bool ContentInstaller::RecordIsUsable(const std::string &title_id_hex, std::uint32_t version,
                                      std::uint8_t meta_type, bool *usable, std::string *error) {
    *usable = false;
    const std::uint64_t id = std::strtoull(title_id_hex.c_str(), nullptr, 16);
    if (id == 0) {
        *error = "the title id is not 16 hex digits";
        return false;
    }
    /* A title's control data is reached through the control content, so a record for a game or an
       update that cannot name one is a record the console cannot draw (an add-on has no control
       content, so there the answer is only "the list is readable"). */
    const bool needs_control = meta_type == 0x80 || meta_type == 0x81;
    const NcmStorageId storages[2] = {NcmStorageId_SdCard, NcmStorageId_BuiltInUser};
    for (const NcmStorageId storage : storages) {
        NcmContentMetaDatabase db{};
        if (R_FAILED(ncmOpenContentMetaDatabase(&db, storage))) {
            continue;
        }
        NcmContentMetaKey key{};
        const Result key_rc = ncmContentMetaDatabaseGetLatestContentMetaKey(&db, &key, id);
        if (R_SUCCEEDED(key_rc) && key.version == version) {
            NcmContentInfo infos[64]{};
            s32 count = 0;
            const Result list_rc = ncmContentMetaDatabaseListContentInfo(&db, &count, infos, 64,
                                                                         &key, 0);
            bool has_control = false;
            bool has_sized = false;
            bool all_present = true;
            /* Every content the storage does not have, for the trace: "one is missing" and "all
               of them are" point at very different causes. */
            std::string missing;
            int missing_count = 0;
            NcmContentStorage cs{};
            const bool cs_open = R_SUCCEEDED(ncmOpenContentStorage(&cs, storage));
            for (s32 i = 0; i < count && i < 64; ++i) {
                u64 size = 0;
                ncmContentInfoSizeToU64(&infos[i], &size);
                if (size > 0) {
                    has_sized = true;
                }
                if (infos[i].content_type == 3) { /* NcmContentType_Control */
                    has_control = true;
                }
                /* A record that names a content the storage does not have is a title the console
                   cannot launch -- the state an install leaves behind when the record lists
                   contents it never wrote (the delta fragments, before this was fixed).  Asking
                   per content is what lets the next install recognise that state and repair it
                   instead of reporting "already installed": everything that *is* present is
                   skipped by the transfer, so the repair is cheap. */
                if (cs_open) {
                    NcmContentId content_id = infos[i].content_id;
                    bool present = false;
                    if (R_FAILED(ncmContentStorageHas(&cs, &present, &content_id)) || !present) {
                        all_present = false;
                        if (missing_count < 4) {
                            if (!missing.empty()) {
                                missing += ",";
                            }
                            missing += ToHex(&content_id, sizeof(content_id));
                        }
                        ++missing_count;
                    }
                }
            }
            if (cs_open) {
                ncmContentStorageClose(&cs);
            }
            {
                char note[160];
                std::snprintf(note, sizeof(note),
                              "usable? storage=%u rc=0x%x contents=%d sized=%d control=%d present=%d",
                              static_cast<unsigned>(storage), static_cast<unsigned>(list_rc),
                              static_cast<int>(count), has_sized ? 1 : 0, has_control ? 1 : 0,
                              all_present ? 1 : 0);
                m_finish_trace.push_back(note);
                if (!all_present) {
                    m_finish_trace.push_back(
                        "usable? missing " + std::to_string(missing_count) + " of " +
                        std::to_string(static_cast<int>(count)) + ": " + missing +
                        (cs_open ? "" : " (the content storage would not open)"));
                }
            }
            /* A list the service cannot really read comes back as entries full of zeroes (the
               offsets it computed run past the payload), which is what a record written in the
               wrong shape produces; a content with no size is not a content; and a content the
               storage does not have is a title that will not start. */
            if (R_SUCCEEDED(list_rc) && count > 0 && has_sized && all_present &&
                (!needs_control || has_control)) {
                *usable = true;
                ncmContentMetaDatabaseClose(&db);
                return true;
            }
        }
        ncmContentMetaDatabaseClose(&db);
    }
    return true;
}

bool ContentInstaller::Has(const std::string &content_id, bool *out, std::string *error) {
    *out = false;
    if (!EnsureOpen(error)) {
        return false;
    }
    NcmContentId id{};
    if (!ContentIdFromHex(content_id, &id)) {
        *error = "the content id in the task is not 32 hex digits";
        return false;
    }
    bool has = false;
    const Result rc = ncmContentStorageHas(&m_storage, &has, &id);
    if (R_FAILED(rc)) {
        *error = Describe("checking for an installed content", rc);
        return false;
    }
    *out = has;
    return true;
}

bool ContentInstaller::Begin(InstallTask *task, std::vector<Placeholder> *out, std::string *error) {
    out->clear();
    if (!EnsureOpen(error)) {
        return false;
    }
    for (auto &content : task->contents) {
        if (content.skipped || content.written >= content.size) {
            continue;
        }
        NcmContentId id{};
        if (!ContentIdFromHex(content.content_id, &id)) {
            *error = "a content in the task has no usable content id";
            return false;
        }
        /* The placeholder is what the bytes land in; it is created once and then written to
           across resumes, which is why the id goes into the task file.  The id has to be
           *generated*: an all-zero uuid is accepted once and refused afterwards ("creating a
           placeholder failed"), which is exactly how a leftover zero-uuid placeholder from an
           earlier run blocked every later attempt. */
        bool already_registered = false;
        const Result has_rc = ncmContentStorageHas(&m_storage, &already_registered, &id);
        m_finish_trace.push_back("begin content " + content.content_id + " registered=" +
                                 (already_registered ? "1" : "0") + " has_rc=0x" +
                                 ToHex(&has_rc, sizeof(has_rc)));
        /* Migration: builds before this one created placeholders with an all-zero uuid, and the
           system refuses a second placeholder for the same content while one exists.  That id is
           known, so it is removed here; no other placeholder is ever touched this way. */
        NcmPlaceHolderId legacy_zero{};
        if (R_SUCCEEDED(ncmContentStorageDeletePlaceHolder(&m_storage, &legacy_zero))) {
            m_finish_trace.push_back("begin removed a legacy zero-uuid placeholder");
        }
        NcmPlaceHolderId placeholder{};
        Result rc = ncmContentStorageGeneratePlaceHolderId(&m_storage, &placeholder);
        if (R_FAILED(rc)) {
            *error = Describe("generating a placeholder id", rc);
            return false;
        }
        rc = ncmContentStorageCreatePlaceHolder(&m_storage, &id, &placeholder,
                                                static_cast<s64>(content.size));
        if (R_FAILED(rc)) {
            m_finish_trace.push_back("begin create failed content=" + content.content_id +
                                     " rc=0x" + ToHex(&rc, sizeof(rc)));
            *error = Describe("creating a placeholder", rc);
            return false;
        }
        Placeholder entry;
        entry.content_id = content.content_id;
        entry.placeholder_id = ToHex(placeholder.uuid.uuid, sizeof(placeholder.uuid.uuid));
        out->push_back(std::move(entry));
        if (content.written == 0) {
            /* Nothing wrote into it yet, so the console's copy is authoritative: a stale
               `written` from a task file whose placeholder was cleaned up must not make us skip
               the beginning of the content. */
            continue;
        }
    }
    return true;
}

bool ContentInstaller::Write(const InstallTask &task, const RangeRequest &range,
                             const std::uint8_t *data, std::size_t size, std::string *error) {
    if (range.index >= task.contents.size()) {
        *error = "the range points outside the task's content list";
        return false;
    }
    if (!EnsureOpen(error)) {
        return false;
    }
    const TaskContent &content = task.contents[range.index];
    NcmPlaceHolderId placeholder{};
    if (!PlaceholderIdFromHex(content.placeholder_id, &placeholder)) {
        /* The placeholder id is written into the task file right after Begin(); a task without
           one means the caller never got that far, which is a programming error, not a user
           error -- say so plainly instead of writing to a zero id. */
        *error = "this content has no placeholder recorded yet";
        return false;
    }
    /* The placeholder *is* the content, so its offsets are content-relative: this writes at
       `written`, not at `range.offset`.  `range.offset` is where the bytes come from in the
       container (and it is not even 4 KB aligned -- the container data starts at 0x190), which is
       why writing there looked fine for a few gigabytes and then had the last chunk refused with
       an alignment error.  The reference implementation writes at its own running offset too. */
    char detail[192];
    std::snprintf(detail, sizeof(detail),
                  " at content offset %llu size %llu of %s (source offset %llu, content size %llu)",
                  static_cast<unsigned long long>(content.written),
                  static_cast<unsigned long long>(size), content.content_id.c_str(),
                  static_cast<unsigned long long>(range.offset),
                  static_cast<unsigned long long>(content.size));
    const Result rc = ncmContentStorageWritePlaceHolder(&m_storage, &placeholder, content.written,
                                                        data, size);
    if (R_FAILED(rc)) {
        /* Which write, how big, and where.  The reason goes into the *error* string rather than
           the finish trace: a failed write never reaches Finish, and that is where the trace is
           flushed -- the earlier version of this diagnostic was lost that way. */
        *error = Describe("writing into the placeholder", rc) + detail;
        return false;
    }
    return true;
}

bool ContentInstaller::Register(const InstallTask &task, const TaskContent *meta,
                                std::string *error) {
    if (!EnsureOpen(error)) {
        return false;
    }
    /* One loop for both: the meta content needs exactly the same treatment as the task's
       contents, and giving it its own copy of this logic is how the two drift apart. */
    std::vector<const TaskContent *> ordered;
    ordered.reserve(task.contents.size() + 1);
    if (meta != nullptr) {
        ordered.push_back(meta);
    }
    for (const auto &content : task.contents) {
        ordered.push_back(&content);
    }
    for (const TaskContent *entry : ordered) {
        const TaskContent &content = *entry;
        if (content.skipped) {
            continue;
        }
        if (content.placeholder_id.empty()) {
            /* No placeholder of ours means the content was already in the storage: it is
               registered, and what is missing is the metadata/record written by Finish(). */
            continue;
        }
        if (content.written < content.size) {
            *error = "a content was registered before all of its bytes arrived";
            return false;
        }
        NcmContentId id{};
        NcmPlaceHolderId placeholder{};
        if (!ContentIdFromHex(content.content_id, &id) ||
            !PlaceholderIdFromHex(content.placeholder_id, &placeholder)) {
            *error = "a finished content has an unusable id";
            return false;
        }
        const Result rc = ncmContentStorageRegister(&m_storage, &id, &placeholder);
        if (R_FAILED(rc)) {
            /* A content can already be registered while we still hold a placeholder for it: an
               earlier run of the same package registered these bytes, and the console's content
               storage is content-addressed, so there is nothing left to register.  Treating that
               as a failure turned a complete install into "registering a content failed
               (0x00000805)" the second time the same package was installed. */
            bool present = false;
            if (R_SUCCEEDED(ncmContentStorageHas(&m_storage, &present, &id)) && present) {
                m_finish_trace.push_back("register content " + content.content_id +
                                         " already in the storage rc=0x" + ToHex(&rc, sizeof(rc)));
                ncmContentStorageDeletePlaceHolder(&m_storage, &placeholder);
                continue;
            }
            *error = Describe("registering a content", rc);
            return false;
        }
        if (entry == meta) {
            /* The one content whose registration is easy to forget (it is not in the task), so its
               outcome is in the trace: a record that names it while the storage does not have it
               is a title that will not launch. */
            m_finish_trace.push_back("meta content registered " + content.content_id);
        }
    }
    return true;
}

bool ContentInstaller::DropPlaceholders(const std::vector<Placeholder> &placeholders,
                                        std::string *error) {
    if (!EnsureOpen(error)) {
        return false;
    }
    bool ok = true;
    for (const auto &entry : placeholders) {
        NcmPlaceHolderId placeholder{};
        if (!PlaceholderIdFromHex(entry.placeholder_id, &placeholder)) {
            continue;
        }
        bool exists = false;
        if (R_FAILED(ncmContentStorageHasPlaceHolder(&m_storage, &exists, &placeholder)) || !exists) {
            continue; /* already gone: nothing to clean up, and not a failure */
        }
        const Result rc = ncmContentStorageDeletePlaceHolder(&m_storage, &placeholder);
        if (R_FAILED(rc)) {
            /* Keep going: one stubborn placeholder must not stop the others from being removed,
               but the caller still hears about it. */
            *error = Describe("deleting a placeholder", rc);
            ok = false;
        }
    }
    return ok;
}

bool ContentInstaller::InstallContentForRead(ContainerReader &reader, const TaskContent &content,
                                             Placeholder *out, std::string *error) {
    if (!EnsureOpen(error)) {
        return false;
    }
    NcmContentId id{};
    if (!ContentIdFromHex(content.content_id, &id)) {
        *error = "a content in the task has no usable content id";
        return false;
    }
    /* The id has to be *generated*, not left as a zero UUID: ncm accepts a zero id once and then
       refuses the next attempt with "already exists" (measured: the second run failed with
       0x405 while the first looked fine).  The reference implementation generates one first. */
    NcmPlaceHolderId placeholder{};
    Result rc = ncmContentStorageGeneratePlaceHolderId(&m_storage, &placeholder);
    if (R_FAILED(rc)) {
        *error = Describe("generating a placeholder id", rc);
        return false;
    }
    rc = ncmContentStorageCreatePlaceHolder(&m_storage, &id, &placeholder,
                                            static_cast<s64>(content.size));
    if (R_FAILED(rc)) {
        *error = Describe("creating a placeholder", rc);
        return false;
    }
    /* 4 MB at a time, straight from the source: the meta content is small, but the same code path
       serves the control content and any future "read one file out of a big content" case. */
    std::vector<std::uint8_t> buffer(4 * 1024 * 1024);
    std::uint64_t written = 0;
    while (written < content.size) {
        const std::uint64_t chunk =
            std::min<std::uint64_t>(buffer.size(), content.size - written);
        if (!reader.Read(content.offset + written, static_cast<std::size_t>(chunk), buffer.data())) {
            *error = "the source stopped early while reading " + content.name;
            ncmContentStorageDeletePlaceHolder(&m_storage, &placeholder);
            return false;
        }
        rc = ncmContentStorageWritePlaceHolder(&m_storage, &placeholder, written, buffer.data(),
                                               static_cast<std::size_t>(chunk));
        if (R_FAILED(rc)) {
            *error = Describe("writing into the placeholder", rc);
            ncmContentStorageDeletePlaceHolder(&m_storage, &placeholder);
            return false;
        }
        written += chunk;
    }
    rc = ncmContentStorageFlushPlaceHolder(&m_storage);
    if (R_FAILED(rc)) {
        *error = Describe("flushing the placeholder", rc);
        return false;
    }
    out->content_id = content.content_id;
    out->placeholder_id = ToHex(placeholder.uuid.uuid, sizeof(placeholder.uuid.uuid));
    return true;
}

bool ContentInstaller::PlaceholderFilePath(const Placeholder &placeholder, std::string *out,
                                           std::string *error) {
    *out = "";
    if (!EnsureOpen(error)) {
        return false;
    }
    NcmPlaceHolderId id{};
    if (!PlaceholderIdFromHex(placeholder.placeholder_id, &id)) {
        *error = "the placeholder id is unusable";
        return false;
    }
    char path[FS_MAX_PATH]{};
    const Result rc = ncmContentStorageGetPlaceHolderPath(&m_storage, path, sizeof(path), &id);
    if (R_FAILED(rc)) {
        *error = Describe("asking ncm for the placeholder path", rc);
        return false;
    }
    *out = path;
    return true;
}

bool ContentInstaller::RegisterContent(const TaskContent &content, std::string *error) {
    if (!EnsureOpen(error)) {
        return false;
    }
    NcmContentId id{};
    NcmPlaceHolderId placeholder{};
    if (!ContentIdFromHex(content.content_id, &id) ||
        !PlaceholderIdFromHex(content.placeholder_id, &placeholder)) {
        *error = "the content has an unusable id";
        return false;
    }
    const Result rc = ncmContentStorageRegister(&m_storage, &id, &placeholder);
    if (R_FAILED(rc)) {
        *error = Describe("registering the content", rc);
        return false;
    }
    return true;
}

bool ContentInstaller::ContentPath(const std::string &content_id, std::string *out,
                                   std::string *error) {
    *out = "";
    if (!EnsureOpen(error)) {
        return false;
    }
    NcmContentId id{};
    if (!ContentIdFromHex(content_id, &id)) {
        *error = "the content id is unusable";
        return false;
    }
    char path[FS_MAX_PATH]{};
    const Result rc = ncmContentStorageGetPath(&m_storage, path, sizeof(path), &id);
    if (R_FAILED(rc)) {
        *error = Describe("asking ncm for the content path", rc);
        return false;
    }
    *out = path;
    return true;
}

bool ContentInstaller::ReadFromContent(const std::string &path, std::uint64_t title_id,
                                       FsFileSystemType type, const std::string &name,
                                       std::vector<std::uint8_t> *out, std::string *error) {
    out->clear();
    FsFileSystem fs{};
    Result rc = fsOpenFileSystemWithId(&fs, title_id, type, path.c_str(), FsContentAttributes_All);
    if (R_FAILED(rc)) {
        *error = Describe("mounting the installed content", rc);
        return false;
    }
    std::string target = name;
    if (target.empty()) {
        /* The meta content's root holds one file: the CNMT.  Listing instead of hardcoding its
           name is what the reference implementation does, and it survives a rename. */
        FsDir dir{};
        rc = fsFsOpenDirectory(&fs, "/", FsDirOpenMode_ReadFiles, &dir);
        if (R_FAILED(rc)) {
            fsFsClose(&fs);
            *error = Describe("listing the content", rc);
            return false;
        }
        FsDirectoryEntry entry{};
        s64 total = 0;
        const Result read = fsDirRead(&dir, &total, 1, &entry);
        fsDirClose(&dir);
        if (R_FAILED(read) || total < 1) {
            fsFsClose(&fs);
            *error = "the content holds no file to read";
            return false;
        }
        target = std::string("/") + entry.name;
    }
    FsFile file{};
    /* The path is built by concatenation a few lines up ("/" + the name inside the content), so
       it lives on the heap and owes its own FS_MAX_PATH window -- see util::FsPath. */
    const util::FsPath safe_target(target);
    rc = fsFsOpenFile(&fs, safe_target.c_str(), FsOpenMode_Read, &file);
    if (R_FAILED(rc)) {
        fsFsClose(&fs);
        *error = Describe("opening the file inside the content", rc);
        return false;
    }
    s64 size = 0;
    if (R_FAILED(fsFileGetSize(&file, &size)) || size <= 0) {
        fsFileClose(&file);
        fsFsClose(&fs);
        *error = "the file inside the content is empty";
        return false;
    }
    out->resize(static_cast<std::size_t>(size));
    u64 got = 0;
    rc = fsFileRead(&file, 0, out->data(), out->size(), FsReadOption_None, &got);
    fsFileClose(&file);
    fsFsClose(&fs);
    if (R_FAILED(rc) || got != out->size()) {
        *error = Describe("reading the file inside the content", R_FAILED(rc) ? rc : rc);
        return false;
    }
    return true;
}

bool ContentInstaller::Finish(const FinishInputs &inputs, std::string *error) {
    m_finish_trace.clear();
    if (!EnsureOpen(error)) {
        return false;
    }
    if (inputs.title_id.empty() || inputs.content_ids.empty() || inputs.meta_blob.empty()) {
        *error = "the finish step was reached without a title, contents or meta payload";
        return false;
    }

    NcmContentMetaKey key{};
    key.id = std::strtoull(inputs.title_id.c_str(), nullptr, 16);
    key.version = inputs.version;
    key.type = inputs.meta_type;
    key.install_type = 0; /* NcmContentInstallType_Full */

    /* The record and the launch version belong to the *application*, with this package's key
       inside: that is how the system connects an update to the game it updates, and how it knows
       which control data to serve.  Filing the patch's record under the patch id instead left the
       console reporting 1.0.0 while ncm had the 2.0.0 record (measured on the console: our own
       query by the application id found only the base game's record). */
    const std::uint64_t record_id = inputs.record_title_id != 0 ? inputs.record_title_id : key.id;

    {
        char note[160];
        std::snprintf(note, sizeof(note),
                      "key id=%s version=0x%x type=%u install=%u contents=%zu record=%016llx",
                      inputs.title_id.c_str(), inputs.version,
                      static_cast<unsigned>(inputs.meta_type), static_cast<unsigned>(key.install_type),
                      inputs.content_ids.size(), static_cast<unsigned long long>(record_id));
        m_finish_trace.push_back(note);
    }

    /* 1. Ticket.  A package without one (a base game only) is legal, so an empty blob is not an
          error; the import failure itself is reported, never swallowed (design section 9). */
    if (!m_options.skip_ticket && !inputs.ticket.empty()) {
        if (!m_services.ImportTicket(inputs.ticket.data(), inputs.ticket.size(),
                                     inputs.certificate.data(), inputs.certificate.size(), error)) {
            return false;
        }
    }

    m_finish_trace.push_back("ticket " + std::string(inputs.ticket.empty() ? "absent" : "imported"));

    /* 2. Delete the older version of this title, across every storage -- the console keeps each
          version's contents otherwise, which is how a second update silently eats the card
          (measured: installing 3.0.3 left 2.0.0's several GB behind until this step existed).
          Contents the new version reuses are skipped: deleting them would delete what we just
          registered. */
    {
        s32 total = 0;
        s32 written = 0;
        if (R_SUCCEEDED(ncmContentMetaDatabaseListApplication(&m_meta, &total, &written, nullptr, 0,
                                                              static_cast<NcmContentMetaType>(inputs.meta_type))) &&
            total > 0) {
            std::vector<NcmApplicationContentMetaKey> keys(static_cast<std::size_t>(total));
            if (R_SUCCEEDED(ncmContentMetaDatabaseListApplication(
                    &m_meta, &total, &written, keys.data(), static_cast<s32>(keys.size()),
                    static_cast<NcmContentMetaType>(inputs.meta_type)))) {
                for (s32 i = 0; i < written; ++i) {
                    const NcmContentMetaKey &old = keys[static_cast<std::size_t>(i)].key;
                    if (old.id != key.id || (old.version == key.version &&
                                             old.type == key.type)) {
                        continue;
                    }
                    /* The old version's contents come from the meta *database*, through the
                       service's own parser: the stored record is not a CNMT file, and reading it
                       as one returns nothing (the layout difference is exactly what made the
                       console unable to draw a title after an install). */
                    NcmContentInfo infos[64]{};
                    s32 count = 0;
                    if (R_SUCCEEDED(ncmContentMetaDatabaseListContentInfo(&m_meta, &count, infos,
                                                                         64, &old, 0))) {
                        for (s32 c = 0; c < count && c < 64; ++c) {
                            NcmContentId id = infos[c].content_id;
                            const std::string content_hex = ToHex(id.c, 16);
                            if (std::find(inputs.content_ids.begin(), inputs.content_ids.end(),
                                          content_hex) != inputs.content_ids.end()) {
                                continue; /* shared with the version we are installing */
                            }
                            ncmContentStorageDelete(&m_storage, &id);
                        }
                    }
                    ncmContentMetaDatabaseRemove(&m_meta, &old);
                }
            }
        }
    }

    /* 3. This version's meta record, then the application record that makes the console see the
          title.  The blob is the CNMT payload exactly as it came out of the package: the system
          reads it back, so rewriting it here would be a second, silent format to keep in sync. */
    {
        const Result rc = ncmContentMetaDatabaseSet(&m_meta, &key, inputs.meta_blob.data(),
                                                    inputs.meta_blob.size());
        {
            char note[96];
            std::snprintf(note, sizeof(note), "metadb set rc=0x%x blob=%zu", static_cast<unsigned>(rc),
                          inputs.meta_blob.size());
            m_finish_trace.push_back(note);
        }
        if (R_FAILED(rc)) {
            *error = Describe("writing the content meta record", rc);
            return false;
        }
        const Result commit = ncmContentMetaDatabaseCommit(&m_meta);
        {
            char note[64];
            std::snprintf(note, sizeof(note), "metadb commit rc=0x%x", static_cast<unsigned>(commit));
            m_finish_trace.push_back(note);
        }
        if (R_FAILED(commit)) {
            *error = Describe("committing the content meta database", commit);
            return false;
        }
    }
    {
        /* An update or add-on must never be filed as an application of its own: a build of this
           app that pushed the record under the patch's id left the console with an application it
           had nothing to draw for, which the home screen showed as an extra, empty tile.  That
           entry is a row of its own, so it is cleared here -- and only here, *before* our own
           record is pushed, because the same call also means "clear this application's record
           list": running it afterwards clears the list we just wrote (measured: the game
           disappeared from the home screen until an install wrote the record back).  It touches no
           content either way. */
        if (key.id != record_id) {
            std::string cleanup_error;
            if (m_services.DeleteApplicationRecord(key.id, &cleanup_error)) {
                m_finish_trace.push_back("cleared the record list of " + inputs.title_id +
                                         " (this package is not an application of its own)");
            } else {
                m_finish_trace.push_back("record-list cleanup for " + inputs.title_id +
                                         " failed: " + cleanup_error);
            }
        }
        /* One record, under the application id: this package's own key and the storage it lives
           on.  The service adds it to that application's list -- the reference implementation
           pushes exactly one record for exactly this reason.  Re-pushing the list's other entries
           was our own invention, and re-pushing a record one did not understand is a way to
           damage it. */
        ContentStorageRecord mine{};
        mine.key = key;
        mine.storage_id = static_cast<std::uint8_t>(ToNcmStorage(m_target));
        const std::vector<ContentStorageRecord> records{mine};
        {
            char note[96];
            std::snprintf(note, sizeof(note), "record app=%016llx key=%016llx storage=%u",
                          static_cast<unsigned long long>(record_id),
                          static_cast<unsigned long long>(key.id),
                          static_cast<unsigned>(mine.storage_id));
            m_finish_trace.push_back(note);
        }
        if (!m_services.PushApplicationRecord(record_id, records, error)) {
            return false;
        }
        /* Read the application's list back: "one record was accepted" and "the application has the
           records it needs" are different statements, and the home screen shows the difference. */
        std::vector<ContentStorageRecord> after;
        std::string after_error;
        if (m_services.ListApplicationRecords(record_id, &after, &after_error)) {
            char note[96];
            std::snprintf(note, sizeof(note), "record list app=%016llx entries=%zu",
                          static_cast<unsigned long long>(record_id), after.size());
            m_finish_trace.push_back(note);
        } else {
            m_finish_trace.push_back("record list unreadable after the push: " + after_error);
        }
    }

    /* What the console can read back out of the record we just wrote: the same question the home
       menu asks when it looks for a title's control data.  A record whose content list cannot be
       read is a title that cannot be drawn, so the answer is traced here instead of inferred from
       a blank tile later. */
    {
        NcmContentInfo infos[64]{};
        s32 count = 0;
        const Result list_rc =
            ncmContentMetaDatabaseListContentInfo(&m_meta, &count, infos, 64, &key, 0);
        char note[128];
        std::snprintf(note, sizeof(note), "record read-back rc=0x%x contents=%d",
                      static_cast<unsigned>(list_rc), static_cast<int>(count));
        m_finish_trace.push_back(note);
    }

    /* 4. The launch version: without it the system refuses to start a title whose record is
          newer than what it was told to launch (HOS 6.0.0+). */
    if (!m_services.PushLaunchVersion(
            record_id, inputs.launch_version != 0 ? inputs.launch_version : inputs.version, error)) {
        return false;
    }
    {
        char note[96];
        std::snprintf(note, sizeof(note), "launch version app=%016llx version=0x%x",
                      static_cast<unsigned long long>(record_id),
                      static_cast<unsigned>(inputs.launch_version != 0 ? inputs.launch_version
                                                                     : inputs.version));
        m_finish_trace.push_back(note);
    }
    /* The system serves a cached NACP per title: without this the version and name it reports
       stay at whatever they were before the install (measured: contents written, record pushed,
       home screen still said 1.0.0).  A patch's control data belongs to the base title, so both
       ids are refreshed (and a base game's own id when the package is a base game); a failure
       here is reported, never swallowed. */
    if (!m_services.InvalidateApplicationControlCache(key.id, error)) {
        return false;
    }
    if (record_id != key.id && !m_services.InvalidateApplicationControlCache(record_id, error)) {
        return false;
    }
    if (!inputs.base_title_id.empty()) {
        const std::uint64_t base = std::strtoull(inputs.base_title_id.c_str(), nullptr, 16);
        if (base != 0 && base != key.id && base != record_id &&
            !m_services.InvalidateApplicationControlCache(base, error)) {
            return false;
        }
    }
    return true;
}

bool ContentInstaller::Rollback(const FinishInputs &inputs, std::string *error) {
    if (!EnsureOpen(error)) {
        return false;
    }
    NcmContentMetaKey key{};
    key.id = std::strtoull(inputs.title_id.c_str(), nullptr, 16);
    key.version = inputs.version;
    key.type = inputs.meta_type;
    key.install_type = 0;

    /* Remove the record first: while it exists the system would keep pointing at contents we
       are about to delete. */
    ncmContentMetaDatabaseRemove(&m_meta, &key);
    ncmContentMetaDatabaseCommit(&m_meta);
    for (const auto &content_id : inputs.content_ids) {
        NcmContentId id{};
        if (ContentIdFromHex(content_id, &id)) {
            ncmContentStorageDelete(&m_storage, &id);
        }
    }
    return true;
}

}  // namespace acnh_manager::install
