#include "install/game_install.hpp"

#include <algorithm>
#include <array>
#include <cstring>

#include "i18n/strings.hpp"
#include "install/dev_stats.hpp"
#include "install/icon_file.hpp"
#include "install/nca.hpp"
#include "install/nca_crypto.hpp"
#include "install/title_name.hpp"

#include "log.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cstdlib>

namespace acnh_manager::install {
namespace {

acnh_manager::Log *g_log = nullptr;

/* The task file's two operations, handed over by the app (see SetGameInstallTaskStore). */
std::function<bool(const std::string &, std::string *)> g_task_save;
std::function<bool(std::string *)> g_task_remove;

/* One diagnostic line; the app passes its log in at startup, and nothing here is dropped when it
   is absent (the tests run without one). */
void Note(const char *format, ...) {
    if (g_log == nullptr) {
        return;
    }
    va_list args;
    va_start(args, format);
    char line[256];
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    g_log->Line("%s", line);
}

/* One request.  4 MB is the design's number: about a second on a local network, small enough
   that a status poll can slip between two of them, large enough that the per-request cost is
   noise (section 3). */
/* One source read, and one placeholder write.  Measured 2026-10-16: a request costs ~16 ms of fixed
   overhead on top of the payload (412 ms per 4 MiB at 10.1 MB/s on a 1.86 GB package), i.e. ~5% of
   the transfer, and that share does not grow with the package (the same 412 ms mean at 337 MB).
   Eight-megabyte blocks looked like the obvious way to halve that, and were measured instead: 9.0
   vs 10.1 MB/s for the same bytes (930 ms per request instead of the expected ~824 ms), i.e. no
   gain and a slower run -- while this link varies 8.9-13.1 MB/s run to run, which is larger than
   any of it.  So four megabytes stays: the same measured throughput, half the queue memory (12 MB)
   and a finer progress bar. */
constexpr std::size_t kBlockSize = 4 * 1024 * 1024;
/* How long the writer waits for the reader's next range before it lets the caller draw a frame.
   Short enough that the screen keeps its frame rate during a transfer, long enough that the wait
   is one wake-up rather than a spin. */
constexpr int kQueuePollMs = 8;
/* Three slots of one block each (12 MB): what the reference implementation's ring holds, and
   enough for the reader to stay a range ahead of the writer. */
constexpr std::size_t kQueueSlots = 3;

/* The progress line's clock.  `dev::NowMs()` is compiled to a constant in release builds (the
   development timings disappear with the rest of the aids), and the speed window needs real time
   in every build -- otherwise a release build would sample the same timestamp forever and never
   report a speed at all. */
std::uint64_t ClockMs() { return armTicksToNs(armGetSystemTick()) / 1000000ull; }

/* How much progress goes by between two writes of install-task.json.  Sixty-four megabytes is a
   few seconds of transfer on this link and costs one ~37 ms write (measured 2026-10-16): the file
   is never more than that far behind, and the card is touched a handful of times per gigabyte. */
constexpr std::uint64_t kTaskFlushBytes = 64ull * 1024 * 1024;

std::string EntryName(const NacpLanguageEntry *entry) {
    if (entry == nullptr) {
        return std::string();
    }
    std::string name;
    for (char c : entry->name) {
        if (c == '\0') {
            break;
        }
        name.push_back(c);
    }
    return name;
}

/* Every name the package carries, in the NACP's own slot order, so the choice between them is
   plain data (`install::ChooseTitleNames`) instead of index arithmetic here. */
std::array<std::string, kNacpLangCount> EntryNames(const NacpStruct *nacp) {
    std::array<std::string, kNacpLangCount> names;
    for (int i = 0; i < kNacpLangCount; ++i) {
        names[static_cast<std::size_t>(i)] = EntryName(&nacp->lang[i]);
    }
    return names;
}

}  // namespace

void SetGameInstallLog(acnh_manager::Log *log) { g_log = log; }

void SetGameInstallTaskStore(std::function<bool(const std::string &, std::string *)> save,
                             std::function<bool(std::string *)> remove) {
    g_task_save = std::move(save);
    g_task_remove = std::move(remove);
}

GameInstall::GameInstall(ContainerReader &reader, std::string name, std::uint64_t container_size,
                         TargetStorage target, const InstallOptions &options)
    : m_reader(reader), m_name(std::move(name)), m_target(target), m_options(options),
      m_installer(target, options) {
    m_task.name = m_name;
    m_task.container_size = container_size;
    /* The task file records where the placeholders live: "give up and clean up" after a restart has
       to know which storage to empty, and the player may have switched the target since. */
    m_task.target = target;
    m_status.needs_keys = options.MayNeedKeys();
}

GameInstall::~GameInstall() {
    /* The reader thread may still be in the middle of a ranged read (or waiting for a slot): ask it
       to stop, then wait for it -- the queue must not outlive the thread that uses it. */
    if (m_queue != nullptr) {
        m_queue->RequestStop();
    }
    if (m_reader_thread.joinable()) {
        m_reader_thread.join();
    }
}

bool GameInstall::Prepare(std::string *error) {
    /* Development aid: where the "picking a file" pause goes.  Reading a package's identity means
       installing its meta (and, for the control data, another) content into a placeholder and
       mounting it -- a sequence of ncm/fs round trips whose cost cannot be guessed at, so it is
       measured (see install/dev_stats.hpp; a release build has no marks). */
    const std::uint64_t t_prepare = dev::NowMs();
    std::uint64_t t_meta_ms = 0;
    std::uint64_t t_control_ms = 0;
    std::uint64_t t_query_ms = 0;
    /* Everything this function logs goes out as one write when it returns: ~15 diagnostic lines,
       and each of them costs about 37 ms of open/write/flush/close per sink on this card (measured
       2026-10-16), i.e. ~0.5 s of the pause between "picked a file" and the confirm card.  RAII,
       because the dozen early returns below would otherwise leave the burst open. */
    const LogBurst burst(g_log);
    /* Closing the game is a one-step fix for the player; nothing else in this flow is.  So the
       running-game stop comes first, before the package is even read. */
    {
        bool running = false;
        std::string running_error;
        if (!m_installer.AnyApplicationRunning(&running, &running_error)) {
            *error = running_error;
            m_status.stage = InstallStage::Failed;
            m_status.error = *error;
            return false;
        }
        if (running) {
            *error = kErrorGameRunning;
            m_status.stage = InstallStage::Failed;
            m_status.error = *error;
            return false;
        }
    }
    Note("install: reading the container (%llu bytes from %s)",
         static_cast<unsigned long long>(m_task.container_size), m_name.c_str());
    if (!ReadPackage(m_reader, m_name, m_task.container_size, &m_package, error)) {
        m_status.stage = InstallStage::Failed;
        m_status.error = *error;
        return false;
    }
    Note("install: container read, %zu entries", m_package.container.contents.size());
    /* An NSZ/XCZ carries its large NCAs as NCZ streams, so the size an entry stands for is not the
       bytes on the card.  Read each stream's tables now: everything after this point (the
       placeholder size, the progress numbers, the decoders) works with the NCA's own size. */
    /* Driven by the entries, not by the file name: a `.ncz` inside a container called `.nsp` (a
       renamed file, or a container a future tool compresses differently) still has to be decoded,
       and its size in the table is the *packed* one -- treating it as an NCA would write a short
       placeholder and a title that cannot launch. */
    for (auto &entry : m_package.container.contents) {
        /* The entry's own NCA header: the authority on how big the content is and on the title key
           it needs.  A compressed entry keeps that header verbatim at the front of its stream, so
           it reads the same way as a plain one. */
        NcaHeaderInfo header;
        std::string header_error;
        const bool has_header = ReadContentHeader(entry, &header, &header_error);
        if (has_header && !header.rights_id.empty()) {
            m_rights_ids.push_back(header.rights_id);
        }
        if (entry.compressed) {
            NczInfo info;
            if (!ReadNczInfo(entry, &info, error)) {
                m_status.stage = InstallStage::Failed;
                m_status.error = *error;
                return false;
            }
            entry.size = info.nca_size;
            m_ncz_info[entry.name.substr(0, 32)] = std::move(info);
            continue;
        }
        /* A plain NCA: its header states how big it is, and that is the size the console stores.
           The container entry is what the file occupies, which can be padded (an HFS0 entry is
           aligned to 0x200), and installing the padding as part of the content would leave a
           content the record does not describe.  The reference implementation sets the placeholder
           size from this number (`write_size = header.size`) and writes exactly it; the two agree
           on every package checked so far (an update NSP, a cartridge dump and a compressed
           cartridge image all have `header.size == entry.size` per content), so this is a
           safeguard with a log line rather than a behaviour change. */
        if (has_header) {
            if (header.size > 0 && header.size != entry.size) {
                Note("install: %s is %llu bytes in the container, %llu as an NCA",
                     entry.name.c_str(), static_cast<unsigned long long>(entry.size),
                     static_cast<unsigned long long>(header.size));
                entry.size = header.size;
            }
        } else {
            Note("install: %s has no readable NCA header: %s", entry.name.c_str(),
                 header_error.c_str());
        }
    }
    /* A content encrypted with a title key can only be read once a ticket for it is installed, and
       the package carries that ticket (the finish step imports it).  A package that asks for one it
       does not carry is worth a line here: the install can still work -- the console usually has
       the ticket of a title it owns, and a cartridge dump needs none at all -- and when it does
       not, this line is the answer.  (The reference implementation refuses such a package
       outright; staying permissive is deliberate, see docs/game_installer_design.md.) */
    for (const std::string &rights_id : m_rights_ids) {
        bool covered = false;
        for (const PackageTicket &pair : m_package.tickets) {
            covered = covered || pair.rights_id == rights_id;
        }
        if (!covered) {
            Note("install: a content needs title key %s and the package carries no ticket for it",
                 rights_id.c_str());
        }
    }
    /* The CNMT and the NACP live *inside* their contents, and how those contents are encrypted is
       the system's business: write the meta content first, ask ncm where its placeholder lives,
       mount that path as a content-meta file system and read the file out.  This is the reference
       implementation's sequence, and it is the only one that needs no key material of ours. */
    /* Which meta this run is about.  An NSP/NSZ carries exactly one; a cartridge dump that was
       made from a game plus its update and DLCs ("1G+1U+2D") carries one per piece, and taking
       whichever came first is how a real dump ended up offering to install a DLC's 0.0 GB -- the
       base game is the one that has to land, so a container with several metas is probed and the
       Application wins.  (The CNMT is inside the encrypted meta content, so the only way to ask
       its type is to read it the same way the chosen one is read.) */
    std::vector<const ContainerEntry *> meta_entries;
    for (const auto &entry : m_package.container.contents) {
        /* ".cnmt." at the id boundary: the meta content is a plain NCA in an NSP and may be an NCZ
           stream in an NSZ, and both readings of the name mean the same file. */
        if (entry.name.size() > 32 && entry.name.compare(32, 6, ".cnmt.") == 0) {
            meta_entries.push_back(&entry);
        }
    }
    if (meta_entries.empty()) {
        *error = "this package has no meta content";
        m_status.stage = InstallStage::Failed;
        m_status.error = *error;
        return false;
    }
    /* Read *every* meta the container carries.  The reference implementation does the same and
       files one record per meta, which is how an update and the add-ons end up attached to the
       game they belong to instead of being ignored because the base game came first.  (Reading
       them all is also what makes the choice below possible at all: a CNMT's type is only visible
       inside the encrypted meta content.) */
    for (const ContainerEntry *candidate : meta_entries) {
        MetaUnit unit;
        unit.entry = candidate;
        const std::uint64_t t_meta = dev::NowMs();
        if (!ReadMetaContent(*candidate, &unit.placeholder, &unit.title_id, &unit.blob, error)) {
            m_status.stage = InstallStage::Failed;
            m_status.error = *error;
            return false;
        }
        t_meta_ms += dev::NowMs() - t_meta;
        if (!ParseCnmt(unit.blob.data(), unit.blob.size(), &unit.cnmt, error)) {
            m_status.stage = InstallStage::Failed;
            m_status.error = *error;
            return false;
        }
        /* Only the kinds a *user* can install are kept: everything below 0x80 is system data
           (system program, system data, a firmware update, boot images), which is not something a
           game installer has any business writing into a player's storage -- and the reference
           implementation skips exactly these (`!(cnmt.key.type & 0x80)` in ShouldSkip).  A package
           that carries nothing else is refused below instead of half-installed. */
        if (!MetaTypeIsInstallable(unit.cnmt.type)) {
            /* Either system data or a type byte this app does not know; both are refused with the
               type spelled out, so a misparse is visible as "unknown" rather than silent. */
            Note("install: %s is not a user-installable meta (type=%s), skipped",
                 candidate->name.c_str(), MetaTypeLabel(unit.cnmt.type));
            std::string ignored;
            m_installer.DropPlaceholders({unit.placeholder}, &ignored);
            continue;
        }
        unit.content.name = candidate->name;
        unit.content.offset = candidate->offset;
        unit.content.size = candidate->size;
        unit.content.content_id = candidate->name.substr(0, 32);
        unit.content.kind = ContentKind::Meta;
        unit.content.placeholder_id = unit.placeholder.placeholder_id;
        m_read_placeholders.push_back(unit.placeholder);
        Note("install: meta %s title=%s v=0x%x type=%s contents=%zu", candidate->name.c_str(),
             unit.cnmt.title_id.c_str(), unit.cnmt.version, MetaTypeLabel(unit.cnmt.type),
             unit.cnmt.contents.size());
        m_metas.push_back(std::move(unit));
    }
    /* The base game leads: the screens describe the title through its control data, and the
       console's application record is filed under its id.  Patch and add-on metas follow, in the
       container's own order among themselves. */
    const auto rank = [](MetaType type) -> int {
        switch (type) {
            case MetaType::Application: return 0;
            case MetaType::Patch: return 1;
            case MetaType::AddOnContent: return 2;
            default: return 3;
        }
    };
    std::stable_sort(m_metas.begin(), m_metas.end(),
                     [&rank](const MetaUnit &lhs, const MetaUnit &rhs) {
                         return rank(lhs.cnmt.type) < rank(rhs.cnmt.type);
                     });
    if (m_metas.empty()) {
        /* Every meta this container carries is system data (a firmware package, or a container
           built out of system titles).  Saying so is the difference between "the app refused this
           file" and a player watching a few GB of system data land in their storage. */
        *error = "this package carries nothing this app installs: its metas are system contents "
                 "(a firmware package) or types it does not recognise";
        m_status.stage = InstallStage::Failed;
        m_status.error = *error;
        return false;
    }
    m_primary = &m_metas.front();
    if (m_primary->cnmt.type != MetaType::Application) {
        Note("install: none of the %zu metas is an application; the screens describe %s",
             m_metas.size(), m_primary->entry->name.c_str());
    }
    /* The package's identity for the rest of the run is the primary meta's: its CNMT is what the
       confirm page shows, what the skip rules compare, and what names the control content the
       name and cover come from.  (The record builder used to read `meta_content_name`, which the
       pure parse filled with the *first* `.cnmt.nca` -- in a container with several metas that is
       usually a DLC's, and the record then named a content nobody installed, which is why the
       console refused to launch the title.  It is the primary's name now.) */
    m_package.cnmt = m_primary->cnmt;
    m_package.meta_blob = m_primary->blob;
    m_package.meta_content_name = m_primary->entry->name;
    Note("install: primary meta %s", m_primary->entry->name.c_str());

    /* The version the console ends up on is the newest one of the metas that carry a game version
       (base game or update, not the add-ons): a container with a game and its update raises the
       title to the update's version, and that is the version the agent's "does it work with this?"
       question is about -- and the control content whose NACP describes the title afterwards. */
    const MetaUnit *effective = m_primary;
    for (const MetaUnit &meta : m_metas) {
        if (!MetaTypeCarriesGameVersion(meta.cnmt.type)) {
            continue;
        }
        if (m_status.effective_version == 0 || meta.cnmt.version >= m_status.effective_version) {
            m_status.effective_version = meta.cnmt.version;
            m_status.effective_title_id = meta.cnmt.title_id;
            effective = &meta;
        }
    }
    if (m_status.effective_version == 0) {
        m_status.effective_version = m_primary->cnmt.version;
        m_status.effective_title_id = m_primary->cnmt.title_id;
    }
    Note("install: effective version 0x%x (%s)", m_status.effective_version,
         m_status.effective_title_id.c_str());

    /* What else rides along, in the app's language: "Update x1 - Add-on x2".  The confirm page
       says it, because installing a game out of a dump that also holds its update and DLCs and
       silently leaving them out is the kind of surprise this app is not allowed to spring. */
    {
        int updates = 0;
        int addons = 0;
        for (const MetaUnit &meta : m_metas) {
            if (&meta == m_primary) {
                continue;
            }
            switch (meta.cnmt.type) {
                case MetaType::Patch: ++updates; break;
                case MetaType::AddOnContent: ++addons; break;
                default: break;
            }
        }
        std::string parts;
        if (updates > 0) {
            parts = i18n::Format(i18n::StringId::MetaCount,
                                 i18n::Text(i18n::StringId::MetaTypeUpdateShort, i18n::Current()),
                                 updates);
        }
        if (addons > 0) {
            const std::string addon = i18n::Format(
                i18n::StringId::MetaCount,
                i18n::Text(i18n::StringId::MetaTypeDlcShort, i18n::Current()), addons);
            parts += parts.empty() ? addon : " · " + addon;
        }
        m_status.extra_summary = parts;
        if (!parts.empty()) {
            Note("install: also installing %s", parts.c_str());
        }
    }
    /* The log line is composed *before* the NACP step, because that step appends to it: building
       it afterwards silently dropped the NACP outcome, which is exactly the information needed
       when the name does not appear. */
    {
        char detail[192];
        std::snprintf(detail, sizeof(detail), "meta=%s v=0x%x type=%u contents=%zu",
                      m_package.cnmt.title_id.c_str(), m_package.cnmt.version,
                      static_cast<unsigned>(m_package.cnmt.type), m_package.cnmt.contents.size());
        char parse_note[192];
        std::snprintf(parse_note, sizeof(parse_note), " ext=%u metas=%u first=%s/%u",
                      m_package.cnmt.extended_header_size, m_package.cnmt.content_meta_count,
                      m_package.cnmt.contents.empty()
                          ? "(none)"
                          : m_package.cnmt.contents[0].content_id.c_str(),
                      m_package.cnmt.contents.empty()
                          ? 0u
                          : static_cast<unsigned>(m_package.cnmt.contents[0].type));
        /* Assign first, append second: the other order silently threw the parse facts away
           (the same mistake this logging code has now made three times, in three shapes). */
        m_status.detail = detail;
        m_status.detail += parse_note;
        Note("install: meta=%s v=0x%x type=%u contents=%zu ext=%u metas=%u",
             m_package.cnmt.title_id.c_str(), m_package.cnmt.version,
             static_cast<unsigned>(m_package.cnmt.type), m_package.cnmt.contents.size(),
             m_package.cnmt.extended_header_size, m_package.cnmt.content_meta_count);
        Note("install: cnmt entries %s", m_package.cnmt.parse_note.c_str());
    }
    /* Two ids come from the package and are needed again later: the title the meta records are
       keyed by, and the *application* the game's own data (its name, version, cover, records) is
       filed under -- base + 0x800 for a patch, base + 0x1000 for an add-on. */
    const std::uint64_t package_title_id =
        std::strtoull(m_package.cnmt.title_id.c_str(), nullptr, 16);
    const std::uint64_t app_id = ApplicationIdOf(package_title_id, m_package.cnmt.type);

    /* The control content carries the NACP: the game's name and the version string the player
       knows.  Which content that is comes from the *NCA header* of each file, not from the CNMT's
       per-content type field -- the reference implementation reads it the same way, and the CNMT
       field said "no control content" for a package that plainly has one.  Same read mechanism as
       the CNMT: install the content, mount it, read the file out. */
    {
        const std::uint64_t t_control = dev::NowMs();
        /* Two facts of their own, each on its own line: this app's log lines are built in a
           192-byte buffer (App::Trace), and a "-> confirm (...)" line carrying the identity *and*
           these two used to end mid-word. */
        std::string nacp_note = "none";
        std::string icon_note = "none";
        const ContainerEntry *control_entry = nullptr;
        /* The id the mount needs is the one in *this* content's own header: the reference
           implementation passes `nca.header.program_id`, and for an update package that differs
           from the meta content's id (patch id vs base id) -- which is exactly what made the
           mount fail. */
        /* Reading one NCA header is what tells a control content from everything else, and the
           mount's key is that header's own program id (the reference implementation passes it the
           same way). */
        const auto header_of = [&](const ContainerEntry &entry, NcaHeaderInfo *out) -> bool {
            if (entry.name.size() <= 32 ||
                (entry.name.compare(entry.name.size() - 4, 4, ".nca") != 0 &&
                 entry.name.compare(entry.name.size() - 4, 4, ".ncz") != 0)) {
                return false;
            }
            if (entry.size < kNcaHeaderSize) {
                return false;
            }
            std::vector<std::uint8_t> encrypted(kNcaHeaderSize);
            std::vector<std::uint8_t> decrypted(kNcaHeaderSize);
            std::string header_error;
            if (!m_reader.Read(entry.offset, encrypted.size(), encrypted.data()) ||
                !DecryptNcaHeader(encrypted.data(), decrypted.data(), &header_error)) {
                return false;
            }
            return ParseNcaHeader(decrypted.data(), decrypted.size(), out, &header_error);
        };
        const auto entry_for = [&](const std::string &id) -> const ContainerEntry * {
            for (const auto &entry : m_package.container.contents) {
                if (entry.name.size() > 32 && entry.name.compare(0, 32, id) == 0) {
                    return &entry;
                }
            }
            return nullptr;
        };
        /* The *effective* meta's control content is the one whose data the console will serve
           (an update's replaces the base game's), so when the container carries several metas its
           own content list points at the right one; a package with a single meta, or one whose
           CNMT does not name a control content, falls back to scanning by NCA header. */
        for (const CnmtContent &content : effective->cnmt.contents) {
            if (content.type != ContentType::Control) {
                continue;
            }
            const ContainerEntry *entry = entry_for(content.content_id);
            if (entry != nullptr) {
                control_entry = entry;
                break;
            }
        }
        if (control_entry == nullptr) {
            for (const auto &entry : m_package.container.contents) {
                NcaHeaderInfo header;
                if (!header_of(entry, &header)) {
                    continue;
                }
                if (header.content_type == ContentType::Control) {
                    control_entry = &entry;
                    break;
                }
            }
        }
        std::uint64_t control_title_id = m_primary->title_id;
        if (control_entry != nullptr) {
            NcaHeaderInfo header;
            if (header_of(*control_entry, &header)) {
                const std::uint64_t own = std::strtoull(header.program_id.c_str(), nullptr, 16);
                if (own != 0) {
                    control_title_id = own;
                }
            }
        }
        if (control_entry == nullptr) {
            nacp_note = "none (the package carries no control content)";
        } else {
            TaskContent control;
            control.name = control_entry->name;
            control.offset = control_entry->offset;
            control.size = control_entry->size;
            control.content_id = control_entry->name.substr(0, 32);
            control.kind = ContentKind::Control;
            Placeholder placeholder;
            std::string path;
            std::string nacp_error;
            const auto control_ncz = m_ncz_info.find(control.content_id);
            if (!m_installer.InstallContentForRead(
                    m_reader, control,
                    control_ncz == m_ncz_info.end() ? nullptr : &control_ncz->second, &placeholder,
                    &nacp_error)) {
                nacp_note = "install-failed: " + nacp_error;
            } else if (!m_installer.PlaceholderFilePath(placeholder, &path, &nacp_error)) {
                nacp_note = "path-failed: " + nacp_error;
                m_installer.DropPlaceholders({placeholder}, &nacp_error);
            } else {
                m_read_placeholders.push_back(placeholder);
                control.placeholder_id = placeholder.placeholder_id;
                m_control_placeholder = placeholder;
                std::vector<std::uint8_t> nacp;
                if (!m_installer.ReadFromContent(path, control_title_id,
                                                 FsFileSystemType_ContentControl, "/control.nacp",
                                                 &nacp, &nacp_error)) {
                    nacp_note = "read-failed: " + nacp_error;
                } else if (nacp.size() < sizeof(NacpStruct)) {
                    nacp_note = "too small to be an NACP";
                } else {
                    NacpStruct parsed{};
                    std::memcpy(&parsed, nacp.data(), sizeof(parsed));
                    NacpLanguageEntry *entry = nullptr;
                    if (R_FAILED(nacpGetLanguageEntry(&parsed, &entry))) {
                        entry = nullptr;
                    }
                    /* `entry` is the console's answer for its *system* language; the app's own
                       setting names the package, and the console's name rides along when it says
                       something else (install/title_name.hpp). */
                    const TitleNames names =
                        ChooseTitleNames(EntryNames(&parsed), EntryName(entry), i18n::Current());
                    m_status.game_name = names.primary;
                    m_status.game_name_alt = names.alternate;
                    Note("install: name=\"%s\" console-name=\"%s\"", names.primary.c_str(),
                         names.alternate.c_str());
                    m_status.game_version = parsed.display_version;
                    nacp_note = "ok";
                }
                /* The picture is in this same content, one file per language (install/
                   icon_file.hpp): read from the package, so the card shows what is about to be
                   installed even on a console where that game is not installed at all. */
                std::vector<std::string> control_files;
                std::string icon_error;
                if (!m_installer.ListContentFiles(path, control_title_id,
                                                  FsFileSystemType_ContentControl, &control_files,
                                                  &icon_error)) {
                    icon_note = "list-failed: " + icon_error;
                } else {
                    const std::string icon_file = ChooseIconFile(control_files, i18n::Current());
                    if (icon_file.empty()) {
                        icon_note = "none (the control content carries no icon)";
                    } else if (!m_installer.ReadFromContent(path, control_title_id,
                                                            FsFileSystemType_ContentControl,
                                                            "/" + icon_file, &m_icon,
                                                            &icon_error)) {
                        icon_note = "read-failed: " + icon_error;
                    } else {
                        icon_note = icon_file + " (" + std::to_string(m_icon.size()) + " bytes)";
                    }
                }
            }
        }
        Note("install: control content: %s", nacp_note.c_str());
        Note("install: cover: %s", icon_note.c_str());
        t_control_ms += dev::NowMs() - t_control;
    }

    /* Only when the package carries no picture of its own (an add-on, whose control content holds
       the NACP and no icon, or a package with no control content at all) does the console get
       asked for the game's icon -- the same call the home screen makes.  It answers only while the
       game is installed: its control data is served from a cache that our own finish step clears
       after an install, so a stale picture can disappear without anything having gone wrong. */
    if (m_icon.empty()) {
        std::string icon_error;
        if (m_installer.Services().GetIcon(app_id, &m_icon, &icon_error)) {
            Note("install: cover: the package has none; the console's own is %zu bytes",
                 m_icon.size());
        } else {
            Note("install: no cover for %016llx: %s", static_cast<unsigned long long>(app_id),
                 icon_error.c_str());
        }
    }

    /* The content list comes from the CNMT we just read: this is what turns "33 entries" into
       "the program, the control and the tickets", skips the delta fragments (the package does not
       ship them) and refuses a package that lists a content it does not carry.  This call was
       lost to a careless range-edit earlier, which is why the confirm page showed 0.0 GB and the
       transfer had nothing to do: the parse was right, nothing consumed it. */
    std::vector<CnmtInfo> cnmt_list;
    cnmt_list.reserve(m_metas.size());
    for (const MetaUnit &meta : m_metas) {
        cnmt_list.push_back(meta.cnmt);
    }
    if (!FillTaskContents(cnmt_list, m_package.container, &m_task, error)) {
        m_status.stage = InstallStage::Failed;
        m_status.error = *error;
        return false;
    }

    /* The meta contents are already in their placeholders: mark them so the transfer phase does
       not fetch them a second time (and so Register finds them complete). */
    for (auto &content : m_task.contents) {
        if (content.content_id == m_control_placeholder.content_id) {
            content.placeholder_id = m_control_placeholder.placeholder_id;
            content.written = content.size;
            continue;
        }
        for (const MetaUnit &meta : m_metas) {
            if (content.content_id == meta.content.content_id) {
                content.placeholder_id = meta.placeholder.placeholder_id;
                content.written = content.size;
                break;
            }
        }
    }
    /* Ask the console which version it has and decide before anything moves: the skip rules are
       "this exact version is already installed" and "the console has something newer and we do
       not downgrade unless asked" (design section 4).  This block was lost to a careless
       range-edit earlier, which let the app install whatever it was given -- the wording on the
       result page was the visible symptom. */
    std::uint32_t installed_version = 0;
    bool same_version_installed = false;
    {
        const std::uint64_t t_query = dev::NowMs();
        std::string version_error;
        if (!m_installer.InstalledState(m_package.cnmt.title_id, m_package.cnmt.version,
                                        static_cast<std::uint8_t>(m_package.cnmt.type),
                                        &same_version_installed, &installed_version,
                                        &version_error)) {
            *error = version_error;
            m_status.stage = InstallStage::Failed;
            m_status.error = *error;
            return false;
        }
        Note("install: console state: same-version-installed=%d highest=0x%x",
             same_version_installed ? 1 : 0, installed_version);
        /* The query's own steps are in the trace; flush them here, because "which records does
           the console have" is answered *before* the confirm page and never reaches Finish when
           the pick is skipped. */
        for (const auto &line : m_installer.FinishTrace()) {
            Note("install: query %s", line.c_str());
        }
        t_query_ms += dev::NowMs() - t_query;
    }
    m_status.package_version = m_package.cnmt.version;
    m_status.installed_version = installed_version;
    {
        const std::uint64_t t_query = dev::NowMs();
        std::uint32_t sd = 0;
        std::uint32_t nand = 0;
        std::string per_storage_error;
        if (m_installer.InstalledVersionPerStorage(m_package.cnmt.title_id, &sd, &nand,
                                                   &per_storage_error)) {
            Note("install: installed version per storage: sd=0x%x nand=0x%x", sd, nand);
        }
        t_query_ms += dev::NowMs() - t_query;
    }
    /* Where the pause between "picked a file" and "the card is up" actually goes: every stage that
       creates a placeholder or mounts one is a handful of ncm/fs round trips, and the split -- not
       the total -- is what says whether there is anything to win.  Logged here, before the skip
       decision, so a package that turns out to be installed already reports its cost too. */
    if (g_log != nullptr) {
        dev::LogPrepare(*g_log, dev::NowMs() - t_prepare, t_meta_ms, t_control_ms, t_query_ms);
    }
    {
        char installed_note[64];
        std::snprintf(installed_note, sizeof(installed_note), " installed=0x%x", installed_version);
        m_status.detail += installed_note;
    }
    /* Two ways to arrive at "this version is already here": the exact key is in the meta database,
       or the newest record for this title id is this version.  Either way it counts as installed
       only when the console's own application record points at it -- a version that exists in the
       meta database alone (an install whose record was filed under the patch id) is not something
       the console can use, and the way to fix it is to install again (no bytes move). */
    const bool version_matches =
        installed_version == m_package.cnmt.version && installed_version != 0;
    bool has_this_version = same_version_installed || version_matches;
    if (has_this_version && !ConsoleCanUseInstalledVersion()) {
        Note("install: %s is in the meta database but the console cannot use that record yet: "
             "finishing again for %016llx",
             m_package.cnmt.title_id.c_str(), static_cast<unsigned long long>(app_id));
        has_this_version = false;
    }
    const SkipDecision decision =
        DecideSkip(m_options, m_package.cnmt.type, m_package.cnmt.version, installed_version,
                   has_this_version);
    if (decision != SkipDecision::Install) {
        m_status.stage = InstallStage::Skipped;
        m_status.skip_message =
            decision == SkipDecision::AlreadyInstalled ? "already-installed" : "newer-installed";
        if (decision == SkipDecision::NewerInstalled) {
            /* Name the version the console has, the way the console names it.  A game and an
               update both answer through the base application's control data; an add-on does not
               (its own version is not what that data describes), so it goes without the number. */
            const bool described_by_control =
                m_package.cnmt.type == MetaType::Application || m_package.cnmt.type == MetaType::Patch;
            std::string version_error;
            if (described_by_control &&
                !m_installer.Services().GetDisplayVersion(app_id, &m_status.installed_version_text,
                                                          &version_error)) {
                Note("install: the console could not name the installed version: %s",
                     version_error.c_str());
            }
            Note("install: skipping for the newer installed version (0x%x, \"%s\")",
                 installed_version, m_status.installed_version_text.c_str());
        }
        RefreshStatus();
        return true;
    }

    /* Contents the console already has are not fetched again: an update shares most of its data
       with the previous version, and the space check asks the same `Has()` question (design
       section 9).  Contents already in a placeholder from this run are left alone. */
    for (auto &content : m_task.contents) {
        if (content.written >= content.size) {
            continue;
        }
        bool has = false;
        std::string has_error;
        if (m_installer.Has(content.content_id, &has, &has_error) && has) {
            content.skipped = true;
            content.written = content.size;
        }
    }
    {
        /* Counts, not guesses: "everything is already here" has to be provable from the log. */
        std::size_t skipped = 0;
        std::size_t pending = 0;
        for (const auto &content : m_task.contents) {
            if (content.skipped || content.written >= content.size) {
                ++skipped;
            } else {
                ++pending;
            }
        }
        char counts[96];
        std::snprintf(counts, sizeof(counts), " contents=%zu skipped=%zu pending=%zu bytes=%llu",
                      m_task.contents.size(), skipped, pending,
                      static_cast<unsigned long long>(TotalBytes(m_task)));
        m_status.detail += counts;
    }

    /* "Every content is already in the storage" is *not* "nothing to do": the bytes may be there
       (content storage is content-addressed, so leftovers from an earlier install count) while the
       console still has no record of this version.  The reference implementation installs
       content-by-content, skipping what is present, and then always runs its register/record
       step -- that is what makes the update take effect without moving a byte.  The skip decision
       above is the only "do nothing" case (same version already installed, or a newer one). */
    if (!m_options.Validate(error)) {
        m_status.stage = InstallStage::Failed;
        m_status.error = *error;
        return false;
    }
    m_status.stage = InstallStage::Fetching;
    RefreshStatus();
    return true;
}

/* A failure after the meta content went into its placeholder has to take it out again: leaving it
   behind makes the next attempt collide with it ("creating a placeholder failed"), which is
   exactly how one failed run turned every later run into the same error. */
bool GameInstall::CleanupAfterFailure(std::string *error) {
    if (m_read_placeholders.empty()) {
        return true;
    }
    std::string ignored;
    const bool ok = m_installer.DropPlaceholders(m_read_placeholders, &ignored);
    m_read_placeholders.clear();
    m_control_placeholder.placeholder_id.clear();
    if (error != nullptr && !ok) {
        *error = ignored;
    }
    return ok;
}

bool GameInstall::Begin(std::string *error) {
    if (!m_installer.Begin(&m_task, &m_placeholders, error)) {
        m_status.stage = InstallStage::Failed;
        m_status.error = *error;
        return false;
    }
    /* One decoder per compressed content.  The tables were read while the package was being read;
       the decoder itself pulls the payload as the transfer asks for it, strictly forward. */
    /* resize, not assign: a vector of unique_ptr cannot be filled with a copied value. */
    m_ncz.clear();
    m_ncz.resize(m_task.contents.size());
    for (std::size_t i = 0; i < m_task.contents.size(); ++i) {
        const TaskContent &content = m_task.contents[i];
        if (!content.compressed) {
            continue;
        }
        const auto known = m_ncz_info.find(content.content_id);
        if (known == m_ncz_info.end()) {
            *error = "the compressed content has no tables to decode it with";
            m_status.stage = InstallStage::Failed;
            m_status.error = *error;
            return false;
        }
        /* One line per decoder: the shape the transfer is about to decode with.  A wrong
           `block_table` here would make a single-stream file look like a block series, which reads
           exactly like "the compressed content could not be decoded" at the first block. */
        Note("install: decoder %zu %s payload=%llu+%llu nca=%llu block_table=%d block_size=%u "
             "sections=%zu",
             i, content.content_id.c_str(),
             static_cast<unsigned long long>(known->second.payload_offset),
             static_cast<unsigned long long>(known->second.payload_size),
             static_cast<unsigned long long>(known->second.nca_size),
             known->second.block_table ? 1 : 0, known->second.block_size,
             known->second.sections.size());
        m_ncz[i] = std::make_unique<NczDecoder>(known->second, m_reader, content.offset);
    }
    /* The placeholder ids go into the task before the first byte: that is what makes a crash
       resumable into the same placeholder instead of a new one. */
    for (const auto &entry : m_placeholders) {
        for (auto &content : m_task.contents) {
            if (content.content_id == entry.content_id) {
                content.placeholder_id = entry.placeholder_id;
            }
        }
    }
    /* And the task file goes out before the first byte too: from here on, an interruption is
       resumable.  A store that cannot be written is worth a line, not a failed install -- the run
       still works, it just cannot be picked up later. */
    m_task.stage = TaskStage::Fetching;
    if (m_task.source != TaskSource::Internal) {
        std::string task_error;
        if (SaveTask(&task_error)) {
            m_task_kept = true;
        } else if (g_log != nullptr) {
            Note("install: the task file could not be written: %s", task_error.c_str());
        }
        m_task_flushed_bytes = DoneBytes(m_task);
        m_task_flushed_contents = DoneContents(m_task);
    }
    /* The task is complete now (order, sizes and placeholder ids), so the fetch thread can start
       filling the queue while this thread starts writing.  It walks the contents itself and keeps
       its own cursors (`m_fetch`): the task counters belong to the writer. */
    m_fetch.assign(m_task.contents.size(), FetchCursor{});
    for (std::size_t i = 0; i < m_task.contents.size(); ++i) {
        /* The fetch cursor is where this content's *source* stream starts, which a resumed plain
           content does not begin at zero: its placeholder already holds `written` bytes, so the
           file has to be read from that same offset.  Reading from zero while the writer appends
           at the resumed offset shifted every byte and made the last block cross the content's
           end -- the 0x00307202 of the interrupted 2.0.0 update (measured 2026-10-20). */
        m_fetch[i].stream_read = InitialStreamRead(m_task.contents[i]);
    }
    m_queue = std::make_unique<TransferQueue>(kQueueSlots, kBlockSize);
    /* Development aid: this run's counters start here, so a second run in one session is not
       reported on top of the first one's numbers. */
    dev::Reset();
    dev::SetTransferMs(0);
    m_transfer_begin_ms = dev::NowMs();
    /* Speed is measured from here on: whatever a resumed task already had is the baseline. */
    m_rate.Reset(ClockMs(), DoneBytes(m_task));
    m_reader_thread = std::thread([this] { ReadLoop(); });
    RefreshStatus();
    return true;
}

bool GameInstall::ReadMetaContent(const ContainerEntry &entry, Placeholder *out_placeholder,
                                  std::uint64_t *out_title_id, std::vector<std::uint8_t> *out_blob,
                                  std::string *error) {
    /* The mount is keyed by the title id, which is a field of the meta content's own header. */
    std::uint64_t title_id = 0;
    {
        std::vector<std::uint8_t> encrypted(kNcaHeaderSize);
        std::vector<std::uint8_t> decrypted(kNcaHeaderSize);
        if (!m_reader.Read(entry.offset, encrypted.size(), encrypted.data())) {
            *error = "cannot read the meta content's header";
            return false;
        }
        if (!DecryptNcaHeader(encrypted.data(), decrypted.data(), error)) {
            return false;
        }
        NcaHeaderInfo header;
        if (!ParseNcaHeader(decrypted.data(), decrypted.size(), &header, error)) {
            return false;
        }
        title_id = std::strtoull(header.program_id.c_str(), nullptr, 16);
    }
    TaskContent content;
    content.name = entry.name;
    content.offset = entry.offset;
    content.size = entry.size;
    content.content_id = entry.name.substr(0, 32);
    content.kind = ContentKind::Meta;
    Placeholder placeholder;
    const auto meta_ncz = m_ncz_info.find(content.content_id);
    if (!m_installer.InstallContentForRead(m_reader, content,
                                           meta_ncz == m_ncz_info.end() ? nullptr
                                                                        : &meta_ncz->second,
                                           &placeholder, error)) {
        return false;
    }
    std::string path;
    if (!m_installer.PlaceholderFilePath(placeholder, &path, error)) {
        std::string ignored;
        m_installer.DropPlaceholders({placeholder}, &ignored);
        return false;
    }
    std::vector<std::uint8_t> blob;
    std::string first_error;
    if (!m_installer.ReadFromContent(path, title_id, FsFileSystemType_ContentMeta, "", &blob,
                                     &first_error)) {
        /* The reference implementation mounts the placeholder path directly; when the system
           refuses it, the documented fallback is to register the content first and mount the
           *content* path instead.  Both errors are reported so the next run says which one
           happened. */
        content.placeholder_id = placeholder.placeholder_id;
        std::string register_error;
        std::string content_path;
        if (m_installer.RegisterContent(content, &register_error) &&
            m_installer.ContentPath(content.content_id, &content_path, &register_error)) {
            std::string second_error;
            if (!m_installer.ReadFromContent(content_path, title_id, FsFileSystemType_ContentMeta, "",
                                             &blob, &second_error)) {
                /* Both attempts failed: the caller's log line has to carry everything a reader
                   needs (the path ncm handed out and each failure), because that is what the next
                   debugging round has to work from. */
                *error = "cannot read the CNMT (placeholder " + path + ": " + first_error +
                         "; content " + content_path + ": " + second_error + ")";
                return false;
            }
        } else {
            *error = register_error;
            return false;
        }
    }
    *out_placeholder = placeholder;
    *out_title_id = title_id;
    *out_blob = std::move(blob);
    return true;
}

bool GameInstall::ReadNczInfo(const ContainerEntry &entry, NczInfo *out, std::string *error) {
    /* The fixed part first: it carries the section count, which is what sizes everything else. */
    std::vector<std::uint8_t> head(kNczHeaderSize + 16);
    if (!m_reader.Read(entry.offset, head.size(), head.data())) {
        *error = "cannot read the compressed content's header";
        return false;
    }
    NczHeaderInfo header;
    if (!ParseNczHeader(head.data(), head.size(), &header, error)) {
        return false;
    }
    /* Then the tables.  Their size is known apart from a block table's, and a block is at least
       16 KiB, so a probe sized from the entry is enough for every file that exists -- and the
       parser refuses what it cannot see the end of rather than reading past it. */
    constexpr std::uint64_t kMinBlockSize = 1u << 14;
    constexpr std::uint64_t kProbeCap = 1u << 20;
    std::uint64_t probe = header.section_count * kNczSectionSize + 24 +
                          (entry.packed_size / kMinBlockSize + 1) * 4;
    if (probe > kProbeCap) {
        probe = kProbeCap;
    }
    if (probe > entry.packed_size - kNczHeaderSize - 16) {
        probe = entry.packed_size - kNczHeaderSize - 16;
    }
    std::vector<std::uint8_t> tables(static_cast<std::size_t>(probe));
    if (!m_reader.Read(entry.offset + header.tables_offset, tables.size(), tables.data())) {
        *error = "cannot read the compressed content's tables";
        return false;
    }
    if (!ParseNczTables(tables.data(), tables.size(), header, entry.packed_size, out, error)) {
        return false;
    }
    /* The NCA's own header is verbatim at the front of the `.ncz`, and it states the NCA's size.
       That number is what the console stores as the content's size, so it is what the placeholder
       is made of -- the reference implementation does exactly this (`write_size = header.size`,
       and its read loop stops there).  The sections' end is only the fallback for a file whose
       header cannot be read: taking the *larger* of the two looks harmless, but a section table
       that reaches past the header's size would leave a placeholder the payload never fills, and
       the install would then fail on its own last byte with "ended before the whole NCA was
       decoded".  Both real packages checked (an NSZ and an XCZ) have the two agreeing. */
    {
        NcaHeaderInfo nca_header;
        std::string header_error;
        if (ReadContentHeader(entry, &nca_header, &header_error)) {
            if (nca_header.size >= kNczHeaderSize && nca_header.size != out->nca_size) {
                Note("install: %s declares %llu bytes as an NCA (its sections end at %llu)",
                     entry.name.c_str(), static_cast<unsigned long long>(nca_header.size),
                     static_cast<unsigned long long>(out->nca_size));
            }
            if (nca_header.size >= kNczHeaderSize) {
                out->nca_size = nca_header.size;
            }
        } else {
            Note("install: %s has no readable NCA header: %s", entry.name.c_str(),
                 header_error.c_str());
        }
    }
    return true;
}

bool GameInstall::ReadContentHeader(const ContainerEntry &entry, NcaHeaderInfo *out,
                                    std::string *error) {
    std::vector<std::uint8_t> encrypted(kNcaHeaderSize);
    std::vector<std::uint8_t> decrypted(kNcaHeaderSize);
    if (!m_reader.Read(entry.offset, encrypted.size(), encrypted.data())) {
        *error = "cannot read the content's header";
        return false;
    }
    if (!DecryptNcaHeader(encrypted.data(), decrypted.data(), error)) {
        return false;
    }
    if (!ParseNcaHeader(decrypted.data(), decrypted.size(), out, error)) {
        return false;
    }
    return true;
}

/* Writes the bytes a decoded payload piece produced.  Called on the writer thread: the zstd work
   lives here now (see ReadLoop), and so does the re-encryption of the ranges the file stored
   decrypted, because both belong to "turn payload into what the placeholder holds". */
bool GameInstall::WriteDecoded(std::size_t index, NczDecoder &decoder, std::string *error) {
    for (;;) {
        std::size_t pending = 0;
        std::uint8_t *decoded = decoder.Pending(&pending);
        if (pending == 0 || decoded == nullptr) {
            return true;
        }
        const TaskContent &content = m_task.contents[index];
        /* A block may decode past the NCA the stream declares (the compressor pads its last
           block).  Those bytes have no place in the placeholder -- asking ncm to write them is the
           0x00307202 the first NSZ runs reported -- so the write stops at the content's own end
           and the surplus is dropped. */
        const std::uint64_t remaining =
            content.size > content.written ? content.size - content.written : 0;
        const std::size_t take =
            pending < remaining ? pending : static_cast<std::size_t>(remaining);
        if (take < pending && g_log != nullptr) {
            Note("install: %s decoded %llu bytes past its declared size; the surplus is dropped",
                 content.name.c_str(),
                 static_cast<unsigned long long>(pending - take));
        }
        if (take > 0) {
            const std::uint64_t content_offset = content.written;
            RangeRequest chunk;
            chunk.index = index;
            chunk.offset = content.offset + content_offset; /* bookkeeping: the placeholder write
                                                               uses the content offset, not this one */
            chunk.size = take;
            ReencryptNczRange(decoder.Info(), content_offset, decoded, take);
            if (!WriteRange(chunk, content_offset, decoded, take, error)) {
                return false;
            }
            /* No Advance here: WriteRange does it (and doing it twice would push the task's
               `written` past what the decoder has handed out). */
        }
        /* All of it leaves the decoder, including the dropped surplus: those bytes can never be
           part of this content, and keeping them would only ask for the same decision again. */
        decoder.Consume(pending);
    }
}

bool GameInstall::WriteRange(const RangeRequest &range, std::uint64_t content_offset,
                             std::uint8_t *data, std::uint64_t size, std::string *error) {
    /* A first block that carries the very start of an NCA is where an XCI's distribution bit has
       to be rewritten: the console only launches gamecard content from the card, so a dump
       installed to SD/NAND needs the header to say "system".  The header is encrypted, so it is
       decrypted, changed and encrypted again, and the stale signature is accepted by the default
       skip options -- exactly what the reference implementation does. */
    bool header_touched = false;
    if (content_offset < kNcaHeaderSize && m_options.ignore_distribution_bit == false) {
        std::uint8_t decrypted[kNcaHeaderSize];
        std::uint8_t reencrypted[kNcaHeaderSize];
        if (!DecryptNcaHeader(data, decrypted, error)) {
            return false;
        }
        NcaHeaderInfo header;
        if (ParseNcaHeader(decrypted, kNcaHeaderSize, &header, error) && header.is_gamecard()) {
            SetDistributionToSystem(decrypted);
            if (!EncryptNcaHeader(decrypted, reencrypted, error)) {
                return false;
            }
            std::memcpy(data, reencrypted, kNcaHeaderSize);
            m_status.modified = true;
            header_touched = true;
        }
    }
    (void)header_touched;
    const std::uint64_t write_begin = dev::NowMs();
    const bool written =
        m_installer.Write(m_task, range, data, static_cast<std::size_t>(size), error);
    /* Development aid: the placeholder write is the card's own cost, and the only part of the run
       the network cannot influence. */
    dev::AddPlaceholder(write_begin, size);
    if (!written) {
        return false;
    }
    Advance(m_task, range, size);
    return true;
}

/* The fetch half of the pipeline: **only the source reads happen here**, on their own thread, so
   they overlap with everything else.  It used to decode too, and that put the zstd work on the
   same critical path as the network: measured 2026-10-16 on hardware, the loop was 46.4 s of reads
   plus 3.4 s of decode, while the writer sat idle for 35 s.  The decode moved to the writer (see
   Step()), which has the slack, and this thread now does nothing but pull bytes.

   It walks the contents in the task's order and publishes, per content: for a compressed one the
   NCZ header verbatim (once) and then its payload; for a plain one its bytes.  Which of the two a
   piece is travels with it (`PreparedRange::payload`).  The writer owns all the task counters, so
   this thread never touches them -- it keeps its own cursors in `m_fetch`. */
void GameInstall::ReadLoop() {
    for (std::size_t index = 0; index < m_task.contents.size(); ++index) {
        /* Before each content: the stretch between two contents is ncm's own work (placeholders,
           registration), during which nothing reads -- and the page this reader is fed by would go
           unserved (see ContainerReader::Service). */
        m_reader.Service();
        const TaskContent &content = m_task.contents[index];
        /* Nothing to fetch when this run already has the content: the meta and the control NCA are
           read out of the package *before* the transfer (Prepare marks them complete by setting
           `written = size`).  The old loop got this for free from NextRange(); this one walks the
           contents itself, so it has to say it here -- without it, the fetch read those contents
           again and the writer refused the write past their end (measured on hardware). */
        if (content.skipped || content.written >= content.size) {
            continue;
        }
        /* The piece that carries the NCZ header verbatim: the first kNczHeaderSize bytes of the
           content are the .ncz's own header, stored (not decoded) in the container. */
        if (content.compressed && !m_fetch[index].header_sent) {
            const std::size_t piece = static_cast<std::size_t>(kNczHeaderSize);
            std::uint8_t *slot = AcquireSlotOrFinish();
            if (slot == nullptr) {
                return;
            }
            const std::uint64_t read_begin = dev::NowMs();
            const bool ok = m_reader.Read(content.offset, piece, slot);
            dev::AddSourceRead(read_begin, piece);
            if (!ok) {
                m_queue->Fail("the source stopped early (connection lost or file truncated)");
                return;
            }
            PreparedRange prepared;
            prepared.range.index = index;
            prepared.range.offset = content.offset;
            prepared.range.size = piece;
            prepared.content_offset = 0;
            prepared.size = piece;
            m_queue->Publish(prepared);
            m_fetch[index].header_sent = true;
        }
        /* The stream this content is read from: the payload for a compressed entry (after the NCZ
           header and its tables), the content itself for a plain one. */
        const bool compressed = content.compressed;
        std::uint64_t stream_offset = content.offset;
        std::uint64_t stream_size = content.size;
        if (compressed) {
            const auto known = m_ncz_info.find(content.content_id);
            if (known == m_ncz_info.end()) {
                m_queue->Fail("a compressed content arrived without its tables");
                return;
            }
            stream_offset = content.offset + known->second.payload_offset;
            stream_size = known->second.payload_size;
        }
        while (m_fetch[index].stream_read < stream_size) {
            /* Per piece of the transfer: the writer and ncm keep this thread busy between reads, and
               the page that feeds it has to be served in between (ContainerReader::Service). */
            m_reader.Service();
            const std::uint64_t missing = stream_size - m_fetch[index].stream_read;
            const std::size_t piece =
                static_cast<std::size_t>(missing < kBlockSize ? missing : kBlockSize);
            std::uint8_t *slot = AcquireSlotOrFinish();
            if (slot == nullptr) {
                return;
            }
            const std::uint64_t at = stream_offset + m_fetch[index].stream_read;
            const std::uint64_t read_begin = dev::NowMs();
            const bool ok = m_reader.Read(at, piece, slot);
            dev::AddSourceRead(read_begin, piece);
            if (!ok) {
                m_queue->Fail("the source stopped early (connection lost or file truncated)");
                return;
            }
            PreparedRange prepared;
            prepared.range.index = index;
            prepared.range.offset = at;
            prepared.range.size = piece;
            prepared.content_offset = 0; /* the writer uses the task's own `written` */
            prepared.size = piece;
            prepared.payload = compressed;
            m_queue->Publish(prepared);
            m_fetch[index].stream_read += piece;
        }
    }
    m_queue->Finish();
}

/* A free queue slot, or nullptr after finishing the queue (stopped, or already failed). */
std::uint8_t *GameInstall::AcquireSlotOrFinish() {
    const std::uint64_t slot_begin = dev::NowMs();
    std::uint8_t *slot = m_queue->AcquireSlot();
    /* Development aid: the fetch thread waiting for a free slot means the writer is behind. */
    dev::AddReaderWait(slot_begin);
    if (slot == nullptr) {
        m_queue->Finish();
    }
    return slot;
}

/* The task file, as the app's store sees it: the engine formats, the app writes.  Reading it back
   is the app's job too (it owns the card and the screen that offers "continue"). */
bool GameInstall::SaveTask(std::string *error) {
    if (m_task.source == TaskSource::Internal) {
        return true; /* an embedded package has no resumable external source */
    }
    if (!g_task_save) {
        return false; /* no store: this run keeps no task file (the host tests run like this) */
    }
    return g_task_save(DumpTask(m_task), error);
}

void GameInstall::RemoveTask() {
    if (m_task.source == TaskSource::Internal) {
        return;
    }
    if (!g_task_remove) {
        return;
    }
    std::string ignored;
    if (!g_task_remove(&ignored) && g_log != nullptr) {
        Note("install: the task file could not be removed: %s", ignored.c_str());
    }
}

void GameInstall::FlushTaskIfDue() {
    if (m_task.source == TaskSource::Internal) {
        return;
    }
    if (!g_task_save) {
        return;
    }
    const std::uint64_t done = DoneBytes(m_task);
    const int finished = DoneContents(m_task);
    /* Two reasons to write, and no others: a content finished (that is progress a resume cannot
       reconstruct, and the cheapest moment to record it), or enough bytes have gone by that losing
       them would hurt.  Writing per range would spend more time in the card than in the transfer. */
    const bool content_finished = finished != m_task_flushed_contents;
    if (!content_finished && done < m_task_flushed_bytes + kTaskFlushBytes) {
        return;
    }
    std::string error;
    if (SaveTask(&error)) {
        m_task_flushed_bytes = done;
        m_task_flushed_contents = finished;
    } else if (g_log != nullptr) {
        Note("install: the task file could not be updated: %s", error.c_str());
    }
}

/* Picking up an interrupted run.  Every decision is made here and none of them is a guess: the
   content ids and sizes have to match the file, and the console has to still hold the placeholder
   (a reboot or another tool's cleanup can take it away).  What is adopted goes into the task's
   `written` / `placeholder_id`, which is exactly the pair `NextRange` and `ContentInstaller::Begin`
   work from -- so a resume is the same code path as a fresh start, one that begins further in. */
bool GameInstall::AdoptResume(const InstallTask &recorded, std::string *error) {
    /* The file has to be about this container: a task that names a different file may describe
       contents that have nothing to do with the one in hand. */
    if (recorded.container_size != 0 && m_task.container_size != 0 &&
        recorded.container_size != m_task.container_size) {
        *error = "the task file was written for a different container";
        return false;
    }
    std::size_t continued = 0;
    std::size_t restarted = 0;
    std::size_t completed = 0;
    for (auto &content : m_task.contents) {
        /* Contents this run does not install (already on the console) have nothing to resume. */
        if (content.skipped) {
            continue;
        }
        /* A content this run has *already* read in full (the meta and control contents, which
           Prepare installs into a placeholder of its own to read them): it is complete in this run,
           and adopting the file's older numbers for it would clear `written` and make Begin create a
           second placeholder for a content that already has one -- which the system refuses. */
        if (content.written >= content.size && content.size > 0) {
            ++completed;
            continue;
        }
        const TaskContent *was = nullptr;
        for (const auto &entry : recorded.contents) {
            if (entry.content_id == content.content_id) {
                was = &entry;
                break;
            }
        }
        if (was == nullptr) {
            ++restarted;
            continue;
        }
        /* The one question only the console can answer. */
        bool present = false;
        std::string probe_error;
        if (!m_installer.HasPlaceHolder(was->placeholder_id, &present, &probe_error)) {
            Note("install: resume could not ask about %s (%s): starting it over",
                 content.content_id.c_str(), probe_error.c_str());
            present = false;
        }
        const ResumePlan plan = PlanResume(*was, content, present);
        if (plan.restart) {
            ++restarted;
            content.written = 0;
            content.placeholder_id.clear();
            if (was->written > 0) {
                Note("install: %s is started over (compressed stream, or its bytes are gone)",
                     content.content_id.c_str());
            }
            continue;
        }
        content.written = plan.written;
        content.placeholder_id = plan.placeholder_id;
        if (content.written >= content.size) {
            ++completed;
        } else {
            ++continued;
        }
    }
    m_task_flushed_bytes = DoneBytes(m_task);
    Note("install: resume: %zu continued, %zu finished, %zu started over (%llu of %llu bytes in)",
         continued, completed, restarted, static_cast<unsigned long long>(DoneBytes(m_task)),
         static_cast<unsigned long long>(TotalBytes(m_task)));
    return true;
}

bool GameInstall::Step(std::string *error) {
    /* The writer half: take whatever the reader has ready and put it in the placeholder.  An empty
       queue is not a failure -- it means the reader is still fetching -- so the caller keeps
       drawing and calls again. */
    PreparedRange prepared;
    std::uint8_t *data = nullptr;
    std::string reader_error;
    const std::uint64_t wait_begin = dev::NowMs();
    switch (m_queue->Next(&prepared, &data, &reader_error, kQueuePollMs)) {
        case TransferQueue::Take::Got:
            dev::AddWriterWait(wait_begin);
            break;
        case TransferQueue::Take::Empty:
            dev::AddWriterWait(wait_begin);
            /* Nothing arrived this poll.  Refresh anyway: the speed window has to see the time
               passing, otherwise a stalled transfer keeps showing the number it had when bytes
               last moved (and the page, which reads this status, would keep the same stale one). */
            RefreshStatus();
            return true;
        case TransferQueue::Take::Finished:
            /* Everything is in: the caller moves to Finish(). */
            return false;
        case TransferQueue::Take::Failed:
            *error = reader_error;
            m_status.stage = InstallStage::Failed;
            m_status.error = *error;
            return false;
        case TransferQueue::Take::Stopped:
            *error = "the transfer was stopped";
            m_status.stage = InstallStage::Failed;
            m_status.error = *error;
            return false;
    }
    dev::AddStep();
    if (prepared.size > kBlockSize) {
        m_queue->Release();
        *error = "the reader prepared a range larger than one block";
        m_status.stage = InstallStage::Failed;
        m_status.error = *error;
        return false;
    }
    if (prepared.payload) {
        /* The compressed half of the transfer: this is where the payload the fetch thread read
           becomes NCA bytes. */
        NczDecoder *decoder = m_ncz[prepared.range.index].get();
        std::string decode_error;
        if (decoder == nullptr) {
            decode_error = "a compressed content arrived without its decoder";
        } else if (!decoder->Feed(data, prepared.size, &decode_error)) {
            /* keep the decoder's own sentence */
        } else if (WriteDecoded(prepared.range.index, *decoder, &decode_error)) {
            m_queue->Release();
            RefreshStatus();
            FlushTaskIfDue();
            return true;
        }
        m_queue->Release();
        *error = decode_error;
        m_status.stage = InstallStage::Failed;
        m_status.error = *error;
        return false;
    }
    const bool written = WriteRange(prepared.range, prepared.content_offset, data, prepared.size,
                                   error);
    m_queue->Release();
    if (!written) {
        m_status.stage = InstallStage::Failed;
        m_status.error = *error;
        return false;
    }
    RefreshStatus();
    FlushTaskIfDue();
    return true;
}

bool GameInstall::Finish(std::string *error) {
    if (!TransferComplete(m_task)) {
        *error = "the install cannot finish before every content has arrived";
        m_status.stage = InstallStage::Failed;
        m_status.error = *error;
        return false;
    }
    /* Development aid: the transfer is over, so this is where the breakdown is worth printing --
       the numbers describe the download and the writes, not the register/finish work below. */
    if (m_transfer_begin_ms != 0) {
        dev::SetTransferMs(dev::NowMs() - m_transfer_begin_ms);
        m_transfer_begin_ms = 0;
        if (g_log != nullptr) {
            dev::LogSummary(*g_log);
        }
    }
    m_status.stage = InstallStage::Finalizing;
    /* The meta contents are registered here too: they are the contents the task cannot list (a
       CNMT does not name itself), and a record that names a content the storage does not have is a
       title the console refuses to launch.  Every meta of the container is registered, not just
       the one the screens describe, because every one of them gets a record.  A placeholder may
       already be gone if the read-out's fallback registered it, which Register tolerates. */
    std::vector<TaskContent> metas_for_register;
    metas_for_register.reserve(m_metas.size());
    for (const MetaUnit &meta : m_metas) {
        TaskContent entry = meta.content;
        entry.written = entry.size;
        metas_for_register.push_back(std::move(entry));
    }
    if (!m_installer.Register(m_task, metas_for_register, error)) {
        m_status.stage = InstallStage::Failed;
        m_status.error = *error;
        return false;
    }
    /* Flush the register phase's own trace here: the finish phase clears the trace when it starts
       (its lines describe the steps below), so without this the one line that says whether the meta
       content -- the content nobody else registers -- actually landed would never reach log.txt. */
    for (const auto &line : m_installer.FinishTrace()) {
        Note("install: register %s", line.c_str());
    }
    ContentInstaller::FinishInputs inputs;
    inputs.title_id = m_primary->cnmt.title_id;
    inputs.version = m_primary->cnmt.version;
    inputs.meta_type = static_cast<std::uint8_t>(m_primary->cnmt.type);
    const MetaRecordShape record_shape = m_task.source == TaskSource::Internal
                                             ? MetaRecordShape::Forwarder
                                             : MetaRecordShape::Console;
    /* The record's payload is built in the console's own layout, not copied from the CNMT file --
       see BuildMetaRecordBlob.  Without this the console stores a record whose content list it
       cannot read, which is how a title ends up with no control data. */
    if (!BuildMetaRecordBlob(m_primary->blob, m_primary->entry->name, m_package.container,
                             m_options.lower_system_version, record_shape, &inputs.meta_blob,
                             error)) {
        m_status.stage = InstallStage::Failed;
        m_status.error = *error;
        return false;
    }
    for (const auto &content : m_task.contents) {
        inputs.content_ids.push_back(content.content_id);
    }
    inputs.base_title_id = m_primary->cnmt.application_id; /* set for patches */
    /* The record and the launch version are filed under the base application id (for a base game
       that is its own id), with this package's key inside -- see ContentInstaller::Finish. */
    const std::uint64_t title_id = std::strtoull(m_primary->cnmt.title_id.c_str(), nullptr, 16);
    inputs.record_title_id = ApplicationIdOf(title_id, m_primary->cnmt.type);
    inputs.reset_application_entity = m_task.source == TaskSource::Internal;
    /* The console is allowed to launch the *newest* version this run puts in place: a container
       that carries a game and its update has to raise the launch version to the update's, or the
       console keeps starting the base game (the reference implementation asks for the newest
       version it can see, per meta, for the same reason). */
    inputs.launch_version = m_primary->cnmt.version;
    for (const MetaUnit &meta : m_metas) {
        if (meta.cnmt.version > inputs.launch_version) {
            inputs.launch_version = meta.cnmt.version;
        }
    }
    if (m_primary->cnmt.type != MetaType::Patch) {
        /* A base game or add-on installed on top of an update must not lower what the console is
           allowed to launch: the reference implementation takes the newest version it can see for
           the game.  An update's own version is the package's version (that is the newest record
           it can be). */
        char ids[2][0x11];
        std::snprintf(ids[0], sizeof(ids[0]), "%016llx",
                      static_cast<unsigned long long>(inputs.record_title_id));
        std::snprintf(ids[1], sizeof(ids[1]), "%016llx",
                      static_cast<unsigned long long>(inputs.record_title_id ^ 0x800ULL));
        for (const char *id_hex : ids) {
            std::uint32_t installed = 0;
            std::string ignored;
            if (m_installer.InstalledVersion(id_hex, &installed, &ignored) &&
                installed > inputs.launch_version) {
                inputs.launch_version = installed;
            }
        }
    }
    /* The container's other metas: each is filed with its own key and its own record, which is how
       the console attaches an update and the add-ons to the game they belong to.  The reference
       implementation loops over them exactly like this. */
    for (const MetaUnit &meta : m_metas) {
        if (&meta == m_primary) {
            continue;
        }
        ContentInstaller::FinishInputs::ExtraMeta extra;
        extra.title_id = meta.cnmt.title_id;
        extra.version = meta.cnmt.version;
        extra.meta_type = static_cast<std::uint8_t>(meta.cnmt.type);
        const std::uint64_t extra_title_id = std::strtoull(meta.cnmt.title_id.c_str(), nullptr, 16);
        extra.record_title_id = ApplicationIdOf(extra_title_id, meta.cnmt.type);
        if (!BuildMetaRecordBlob(meta.blob, meta.entry->name, m_package.container,
                                 m_options.lower_system_version, record_shape, &extra.meta_blob,
                                 error)) {
            m_status.stage = InstallStage::Failed;
            m_status.error = *error;
            return false;
        }
        inputs.extra_metas.push_back(std::move(extra));
    }
    inputs.tickets = m_package.tickets;
    if (!m_installer.Finish(inputs, error)) {
        m_status.stage = InstallStage::Failed;
        m_status.error = *error;
        return false;
    }
    /* One line per finish step: "the install said complete but the console disagrees" needs to
       be readable from the log, not inferred. */
    for (const auto &line : m_installer.FinishTrace()) {
        Note("install: finish %s", line.c_str());
    }
    m_status.stage = InstallStage::Done;
    m_task.stage = TaskStage::Done;
    /* The console has it: there is nothing left to continue. */
    RemoveTask();
    m_task_kept = false;
    RefreshStatus();
    return true;
}

bool GameInstall::Abandon(std::string *error) {
    /* The meta content's placeholder exists before the confirm page (the CNMT has to be read to
       fill that page in), so it is not in `m_placeholders` yet and has to be dropped here too. */
    bool ok = m_installer.DropPlaceholders(m_placeholders, error);
    std::string ignored;
    ok = m_installer.DropPlaceholders(m_read_placeholders, &ignored) && ok;
    m_read_placeholders.clear();
    m_control_placeholder.placeholder_id.clear();
    if (m_status.modified) {
        /* Only discard registered content when this run registered it: a cancel before Register
           has nothing to delete, and deleting a title the console already had is not ours to do. */
        ContentInstaller::FinishInputs inputs;
        inputs.title_id = m_package.cnmt.title_id;
        inputs.version = m_package.cnmt.version;
        inputs.meta_type = static_cast<std::uint8_t>(m_package.cnmt.type);
        for (const auto &content : m_task.contents) {
            inputs.content_ids.push_back(content.content_id);
        }
        m_installer.Rollback(inputs, error);
    }
    /* Cancelling means "pretend this never started": the placeholders are gone, so a task file that
       points at them would only offer a resume into nothing. */
    RemoveTask();
    m_task_kept = false;
    return ok;
}

void GameInstall::RefreshStatus() {
    m_status.total_bytes = TotalBytes(m_task);
    m_status.done_bytes = DoneBytes(m_task);
    m_status.done_contents = DoneContents(m_task);
    m_status.total_contents = static_cast<int>(m_task.contents.size());
    m_rate.Sample(ClockMs(), m_status.done_bytes);
    m_status.bytes_per_second = m_rate.Rate();
    m_status.eta_seconds = 0;
    if (m_status.bytes_per_second > 0 && m_status.done_bytes < m_status.total_bytes) {
        const std::uint64_t left = m_status.total_bytes - m_status.done_bytes;
        /* Rounded up: "3 seconds left" that finishes in 2.1 is better than "2" that does not. */
        m_status.eta_seconds = (left + m_status.bytes_per_second - 1) / m_status.bytes_per_second;
    }
    m_status.current_name.clear();
    RangeRequest next;
    if (NextRange(m_task, kBlockSize, &next)) {
        m_status.current_name = m_task.contents[next.index].name;
    }
}

bool GameInstall::ConsoleCanUseInstalledVersion() {
    const std::uint64_t title_id = std::strtoull(m_package.cnmt.title_id.c_str(), nullptr, 16);
    const std::uint64_t app_id = ApplicationIdOf(title_id, m_package.cnmt.type);
    std::vector<ContentStorageRecord> records;
    std::string list_error;
    if (!m_installer.Services().ListApplicationRecords(app_id, &records, &list_error)) {
        /* Without an answer the safe reading is "the console cannot use it yet": the finish step
           runs again and, at worst, rewrites the same record. */
        Note("install: the record list for %016llx could not be read: %s",
             static_cast<unsigned long long>(app_id), list_error.c_str());
        return false;
    }
    bool found = false;
    for (const auto &record : records) {
        if (record.key.id == title_id && record.key.version == m_package.cnmt.version &&
            record.key.type == static_cast<std::uint8_t>(m_package.cnmt.type)) {
            found = true;
            break;
        }
    }
    Note("install: record list app=%016llx entries=%zu has-this-version=%d",
         static_cast<unsigned long long>(app_id), records.size(), found ? 1 : 0);
    if (!found) {
        return false;
    }
    /* Filed under the right application is not the same as usable: the console reads a title's
       content list -- and through it its control data -- out of the record's payload, so a record
       written in the wrong shape has to be written again. */
    const std::size_t trace_before = m_installer.FinishTrace().size();
    bool usable = false;
    std::string usable_error;
    if (!m_installer.RecordIsUsable(m_package.cnmt.title_id, m_package.cnmt.version,
                                   static_cast<std::uint8_t>(m_package.cnmt.type), &usable,
                                   &usable_error)) {
        Note("install: the stored record could not be checked: %s", usable_error.c_str());
        return false;
    }
    const std::vector<std::string> &trace = m_installer.FinishTrace();
    for (std::size_t i = trace_before; i < trace.size(); ++i) {
        Note("install: %s", trace[i].c_str());
    }
    return usable;
}

}  // namespace acnh_manager::install
