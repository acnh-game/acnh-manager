#pragma once

/* Device side of the installer: the content storage (placeholders, bytes, register) and the
   meta database.  Everything that can be decided without hardware lives in container.*,
   install_task.* and nca.*; this is the part that talks to the console.

   The order is the one the design fixes (section 4): every placeholder first, then the bytes,
   then register, then the finish steps (ticket, delete-the-older-version, application record).
   Nothing is registered before every byte has arrived, so a failure in the middle leaves
   placeholders -- which the task file points at, and which can be resumed or dropped -- rather
   than a half-installed title. */

#include <switch.h>

#include <cstdint>
#include <string>
#include <vector>

#include "install/container.hpp"
#include "install/install_task.hpp"
#include "install/ncz.hpp"
#include "install/nx_services.hpp"
#include "install/options.hpp"
#include "install/package.hpp"

namespace acnh_manager::install {

/* Where a content's bytes go while they arrive.  The placeholder id is what ncm hands back; the
   task file records it so an interrupted install resumes into the same placeholder. */
struct Placeholder {
    std::string content_id;     /* 32 hex digits */
    std::string placeholder_id; /* 16 hex digits */
};

class ContentInstaller {
public:
    ContentInstaller(TargetStorage target, const InstallOptions &options);
    ~ContentInstaller();
    ContentInstaller(const ContentInstaller &) = delete;
    ContentInstaller &operator=(const ContentInstaller &) = delete;

    /* Free / total bytes of the storage this installer targets, in the units the header bars
       show.  Static: the UI wants the number before any install starts. */
    static bool Space(TargetStorage target, std::uint64_t *free_bytes, std::uint64_t *total_bytes,
                      std::string *error);

    /* Opens the ncm sessions, the target storage and the meta database. */
    bool Open(std::string *error);

    /* Creates the placeholder of every content that still needs bytes, and reports the ids in
       task order so the caller can store them in `install-task.json` before any byte is
       written.  Contents that are skipped (already installed, or not wanted) get no placeholder. */
    bool Begin(InstallTask *task, std::vector<Placeholder> *out, std::string *error);

    /* Writes one range into one content's placeholder.  The range must be the one the task asked
       for (NextRange), which is what keeps offsets honest across resumes. */
    bool Write(const InstallTask &task, const RangeRequest &range, const std::uint8_t *data,
               std::size_t size, std::string *error);

    /* Registers every content that is fully written: the package's own meta content first, then
       the task's contents in order.

       `meta` exists because the task cannot carry it: the CNMT does not list itself, so the meta
       content would otherwise never be registered -- and a record that names a content the storage
       does not have is a title the console refuses to launch (measured: a fresh install left it in
       a placeholder and the game would not start).  The reference implementation registers it with
       the rest (`RegisterNcasAndPushRecord` registers the CNMT NCA first).  Pass nullptr when the
       read-out's fallback already registered it to mount it, or for an add-on, which has none. */
    bool Register(const InstallTask &task, const std::vector<TaskContent> &metas, std::string *error);

    /* True when the application that owns this package meta is running right now.  The question is
       asked by base application id, so an update or add-on also finds its suspended base game. */
    bool ApplicationRunning(const std::string &title_id_hex, MetaType type, bool *out,
                            std::string *error);

    /* Writes one whole content from `reader` into a fresh placeholder and flushes it.
       This exists because the meta (CNMT) and control (NACP) contents have to be *readable*, and
       the only reader that understands how they are encrypted is the system's own file layer --
       the reference implementation does exactly this: install the content, ask ncm for the
       placeholder's path, mount it, read the file out.  Nothing here decrypts anything.

       `ncz` is that entry's NCZ table when the container stores it compressed (NSZ/XCZ): the
       placeholder has to hold the *NCA*, so the stream is decoded on the way in -- the reference
       implementation runs every collection through its decompressor, including this one.  A null
       table means the entry is a plain NCA and its bytes are copied. */
    bool InstallContentForRead(ContainerReader &reader, const TaskContent &content,
                               const NczInfo *ncz, Placeholder *out, std::string *error);

    /* The file-system path of a placeholder, which is what gets mounted. */
    bool PlaceholderFilePath(const Placeholder &placeholder, std::string *out, std::string *error);

    /* Registers one content (placeholder -> content), which is what makes its *content* path
       available; used when a mount by placeholder path is refused. */
    bool RegisterContent(const TaskContent &content, std::string *error);

    /* The content-storage path of an already registered content. */
    bool ContentPath(const std::string &content_id, std::string *out, std::string *error);

    /* Mounts an installed content (its content path, or its placeholder path) and reads one file
       out of it.  `name` empty means "the first file in the root", which is how the CNMT is
       found inside a meta content. */
    bool ReadFromContent(const std::string &path, std::uint64_t title_id, FsFileSystemType type,
                         const std::string &name, std::vector<std::uint8_t> *out,
                         std::string *error);

    /* The file names in the root of a mounted content, in the order the filesystem hands them
       over (callers that need a stable choice sort or rank them themselves).  The control content
       needs this to find the icon for a language: its name is not a fact the CNMT carries. */
    bool ListContentFiles(const std::string &path, std::uint64_t title_id, FsFileSystemType type,
                          std::vector<std::string> *out, std::string *error);

    /* The version of `title_id` this console has, or 0 when it has none.  The skip decision and
       the "already installed" skip both come from this number, so it is read from the console
       rather than remembered by the app. */
    bool InstalledVersion(const std::string &title_id_hex, std::uint32_t *out, std::string *error);

    /* The same question asked per storage: the base game can live on the console while an update
       is written to the card (or the other way round), so "which version does this console have"
       has to look at both -- and the numbers are worth logging separately, because "the record
       went to the storage the system does not read" looks exactly like "the install did nothing". */
    bool InstalledVersionPerStorage(const std::string &title_id_hex, std::uint32_t *sd,
                                    std::uint32_t *nand, std::string *error);

    /* What the console knows about this package's title: the highest version installed on any
       storage, and whether *this exact* version and kind is already there.  Both answers come from
       queries keyed by the *package's own* meta id (for an update that is the patch id, not the
       base game's): "latest key for id" and "does this key exist" -- the same pair the reference
       implementation uses for its skip decision.  The application id is only for questions about
       the base game, and a query with it answers about the base game's own record, which is how an
       installed update can look like it is not installed. */
    bool InstalledState(const std::string &title_id_hex, std::uint32_t version, std::uint8_t meta_type,
                        bool *same_version_installed, std::uint32_t *highest_version,
                        std::string *error);

    /* True when the content is already on the console: what `skip_if_already_installed` and the
       space calculation both ask. */
    bool Has(const std::string &content_id, bool *out, std::string *error);

    /* True when the console can *use* the record it has for this exact version: the entry exists
       and the service can read its content list -- the list the home menu walks to reach a title's
       control data, and therefore its name, its version and its icon.  A record written in the
       wrong shape is present but unreadable ("the version is installed" and "the console can run
       it" are different statements), and installing again is what repairs it. */
    bool RecordIsUsable(const std::string &title_id_hex, std::uint32_t version,
                        std::uint8_t meta_type, bool *usable, std::string *error);

    /* Removes placeholders (the cancel path, and rolling back what a failed run created). */
    bool DropPlaceholders(const std::vector<Placeholder> &placeholders, std::string *error);

    /* Whether the console still holds the bytes of a placeholder an interrupted run left behind.
       This is the question a resume turns on: a reboot or another tool's cleanup can take the
       placeholder away, and a task file that says "60% of this content is in" would then describe
       bytes nobody has.  `placeholder_id` is the 16 hex digits the task file records. */
    bool HasPlaceHolder(const std::string &placeholder_id, bool *exists, std::string *error);

    NxServices &Services() { return m_services; }
    NcmContentStorage &Storage() { return m_storage; }
    NcmContentMetaDatabase &MetaDatabase() { return m_meta; }
    NcmStorageId StorageId() const;

    /* Everything the finish steps need, gathered by the engine while it read the package: the
       identity of the title being installed, the CNMT payload exactly as it was read (the meta
       database stores that blob verbatim), the contents to record in CNMT order, and the ticket
       pair when the package carries one. */
    struct FinishInputs {
        std::string title_id;               /* 16 hex digits */
        std::uint32_t version{0};
        std::uint8_t meta_type{0};          /* NcmContentMetaType */
        std::vector<std::uint8_t> meta_blob; /* the CNMT payload, byte for byte */
        std::vector<std::string> content_ids;
        /* For a patch, the base title whose control data has to be refreshed.  Empty when the
           package is not a patch. */
        std::string base_title_id;
        /* The id the application *record* and the launch version are filed under: the base
           application id, with this package's own meta key inside the record.  An update whose
           record is pushed under its own patch id leaves the console convinced it still has the
           old version -- ncm knows the patch, the home screen still says "Ver. 1.0.0".  Zero
           means "the package's own id" (a base game is filed under itself). */
        std::uint64_t record_title_id{0};
        /* Embedded forwarders replace an application entity created by an earlier attempt or by
           another forwarder tool.  The reference removes that entity immediately before pushing
           the new application record. */
        bool reset_application_entity{false};
        /* What the console is told it may launch.  Normally the package's version, but a base game
           or add-on installed on top of a newer update must not undo it; zero means
           `version`. */
        std::uint32_t launch_version{0};
        /* The container's other metas, filed in this same run: a cartridge dump made from a game
           plus its update and DLCs carries one CNMT per piece, and the reference implementation
           installs every one of them, each with its own meta record.  Filing only the base game
           leaves the update and the add-ons unannounced -- the console then serves the base
           game's control data, and the card's own "1U+2D" never arrives. */
        struct ExtraMeta {
            std::string title_id;                /* 16 hex digits */
            std::uint32_t version{0};
            std::uint8_t meta_type{0};
            std::vector<std::uint8_t> meta_blob; /* the CNMT payload, byte for byte */
            std::uint64_t record_title_id{0};
        };
        std::vector<ExtraMeta> extra_metas;
        /* The package's ticket pairs, in the order the container lists them: each one is imported
           as it is, because a content encrypted with a title key can only be read once the console
           has its ticket.  Empty when the package carries none (a cartridge dump, or a base game
           whose ticket the console already has). */
        std::vector<PackageTicket> tickets;
    };

    /* The point of no return: everything is in the placeholders, so from here the console is
       told about it.  Order (design section 4): import the ticket, delete the older version of
       the same title, write this version's meta record and push the application record, then
       raise the launch version.  A failure here leaves the new content registered but not
       announced, which the caller reports as a failed install; the caller's cleanup drops the
       placeholders and, where possible, the records. */
    bool Finish(const FinishInputs &inputs, std::string *error);

    /* The finish sequence's own record, one entry per step, so a run that reports success but
       changes nothing on the console can be told apart from one whose write was refused. */
    const std::vector<std::string> &FinishTrace() const { return m_finish_trace; }

    /* Undo what Finish() registered for `inputs` (the cancel/rollback path).  Metas and records
       the console already had are never touched. */
    bool Rollback(const FinishInputs &inputs, std::string *error);

private:
    bool EnsureOpen(std::string *error);

    /* Rewrites the distribution bit of a gamecard content in place (a decrypted-header edit, then
       encrypted again, with the now-stale signature accepted by the default skip options).  Both
       write paths call it: the transfer for every content it moves, and this class's own read-out
       for the meta content, which never goes through the transfer at all.  A short buffer (a
       content smaller than a header) and a non-gamecard header are both left alone. */
    bool RewriteDistributionBit(std::uint8_t *header, std::size_t size, std::string *error);

    /* Every content a stored record lists.  The service reports how many entries it wrote, not how
       many there are, so the count comes from the record's own header first: a fixed-size buffer
       silently drops the rest of a record that has more contents than it holds, and for the cleanup
       step that means leaving the previous version's contents on the card. */
    static bool ListRecordContents(NcmContentMetaDatabase &db, const NcmContentMetaKey &key,
                                   std::vector<NcmContentInfo> *out, std::string *error);

    TargetStorage m_target;
    InstallOptions m_options;
    NcmContentStorage m_storage{};
    NcmContentMetaDatabase m_meta{};
    NxServices m_services;
    bool m_open{false};
    std::vector<std::string> m_finish_trace;
};

/* 32 hex digits <-> NcmContentId, 16 hex digits <-> NcmPlaceHolderId.  Kept next to the storage
   because every id in the task file and in the log is text. */
bool ContentIdFromHex(const std::string &hex, NcmContentId *out);
bool PlaceholderIdFromHex(const std::string &hex, NcmPlaceHolderId *out);
std::string ToHex(const void *bytes, std::size_t size);

}  // namespace acnh_manager::install
