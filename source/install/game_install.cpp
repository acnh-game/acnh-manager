#include "install/game_install.hpp"

#include <algorithm>
#include <array>
#include <cstring>

#include "i18n/strings.hpp"
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
constexpr std::size_t kBlockSize = 4 * 1024 * 1024;

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

GameInstall::GameInstall(ContainerReader &reader, std::string name, std::uint64_t container_size,
                         TargetStorage target, const InstallOptions &options)
    : m_reader(reader), m_name(std::move(name)), m_target(target), m_options(options),
      m_installer(target, options) {
    m_task.name = m_name;
    m_task.container_size = container_size;
    m_status.needs_keys = options.MayNeedKeys();
}

GameInstall::~GameInstall() {
    delete[] m_buffer;
}

bool GameInstall::Prepare(std::string *error) {
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
            *error = "game-running";
            m_status.stage = InstallStage::Failed;
            m_status.error = *error;
            return false;
        }
    }
    if (!ReadPackage(m_reader, m_name, m_task.container_size, &m_package, error)) {
        m_status.stage = InstallStage::Failed;
        m_status.error = *error;
        return false;
    }
    /* An NSZ/XCZ carries its large NCAs as NCZ streams, so the size an entry stands for is not the
       bytes on the card.  Read each stream's tables now: everything after this point (the
       placeholder size, the progress numbers, the decoders) works with the NCA's own size. */
    if (m_package.container.compressed) {
        for (auto &entry : m_package.container.contents) {
            if (!entry.compressed) {
                continue;
            }
            NczInfo info;
            if (!ReadNczInfo(entry, &info, error)) {
                m_status.stage = InstallStage::Failed;
                m_status.error = *error;
                return false;
            }
            entry.size = info.nca_size;
            m_ncz_info[entry.name.substr(0, 32)] = std::move(info);
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
        if (!ReadMetaContent(*candidate, &unit.placeholder, &unit.title_id, &unit.blob, error)) {
            m_status.stage = InstallStage::Failed;
            m_status.error = *error;
            return false;
        }
        if (!ParseCnmt(unit.blob.data(), unit.blob.size(), &unit.cnmt, error)) {
            m_status.stage = InstallStage::Failed;
            m_status.error = *error;
            return false;
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

    /* What else rides along, in the app's language: "更新 ×1 · 追加内容 ×2".  The confirm page
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
                                 i18n::Text(i18n::StringId::TypeUpdate, i18n::Current()), updates);
        }
        if (addons > 0) {
            const std::string addon = i18n::Format(
                i18n::StringId::MetaCount, i18n::Text(i18n::StringId::TypeDlc, i18n::Current()),
                addons);
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
            if (!m_installer.InstallContentForRead(m_reader, control, &placeholder, &nacp_error)) {
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
    }
    m_status.package_version = m_package.cnmt.version;
    m_status.installed_version = installed_version;
    {
        std::uint32_t sd = 0;
        std::uint32_t nand = 0;
        std::string per_storage_error;
        if (m_installer.InstalledVersionPerStorage(m_package.cnmt.title_id, &sd, &nand,
                                                   &per_storage_error)) {
            Note("install: installed version per storage: sd=0x%x nand=0x%x", sd, nand);
        }
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
        DecideSkip(m_options, m_package.cnmt.version, installed_version, has_this_version);
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
       the stream itself starts here, and it is fed strictly in order (see NczStream). */
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
        auto stream = std::make_unique<NczStream>();
        stream->info = known->second;
        stream->container_offset = content.offset;
        stream->dstream = ZSTD_createDStream();
        if (stream->dstream == nullptr || ZSTD_isError(ZSTD_initDStream(stream->dstream))) {
            *error = "cannot start the decoder for the compressed content";
            m_status.stage = InstallStage::Failed;
            m_status.error = *error;
            return false;
        }
        m_ncz[i] = std::move(stream);
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
    RefreshStatus();
    return true;
}

GameInstall::NczStream::~NczStream() {
    if (dstream != nullptr) {
        ZSTD_freeDStream(dstream);
    }
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
    if (!m_installer.InstallContentForRead(m_reader, content, &placeholder, error)) {
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
    return ParseNczTables(tables.data(), tables.size(), header, entry.packed_size, out, error);
}

bool GameInstall::DecodeUntil(NczStream &stream, std::uint64_t nca_size, std::uint64_t need_until,
                              std::string *error) {
    constexpr std::size_t kChunk = 512 * 1024;
    if (stream.source_exhausted && stream.decoded_end < need_until) {
        *error = "the compressed content ended before the whole NCA was decoded";
        return false;
    }
    if (m_decode_buffer.size() < kChunk) {
        m_decode_buffer.resize(kChunk);
    }
    while (stream.decoded_end < need_until) {
        if (stream.input_pos >= stream.input.size() && !stream.source_exhausted) {
            const std::uint64_t left = stream.info.payload_size - stream.payload_read;
            if (left == 0) {
                stream.source_exhausted = true;
                continue;
            }
            const std::size_t chunk = static_cast<std::size_t>(left < kChunk ? left : kChunk);
            stream.input.resize(chunk);
            stream.input_pos = 0;
            /* The payload's offsets are inside the `.ncz`, so the entry's own offset has to be
               added here: reading without it fed the container's own header bytes to zstd, which
               failed with "could not be decoded" on the first real package (measured). */
            if (!m_reader.Read(stream.container_offset + stream.info.payload_offset +
                                   stream.payload_read,
                               chunk, stream.input.data())) {
                *error = "the source stopped early (connection lost or file truncated)";
                return false;
            }
            stream.payload_read += chunk;
        }
        ZSTD_inBuffer in{stream.input.data() + stream.input_pos,
                         stream.input.size() - stream.input_pos, 0};
        ZSTD_outBuffer out{m_decode_buffer.data(), m_decode_buffer.size(), 0};
        const std::size_t result = ZSTD_decompressStream(stream.dstream, &out, &in);
        if (ZSTD_isError(result)) {
            *error = "the compressed content could not be decoded";
            return false;
        }
        stream.input_pos += in.pos;
        if (out.pos > 0) {
            /* The bytes are encrypted again as they are decoded, before anything can hand them
               out: the ranges that were stored decrypted have to reach the placeholder in the
               shape the console expects. */
            const std::uint64_t base = stream.decoded_end;
            auto *bytes = m_decode_buffer.data();
            for (const NczSection &section : stream.info.sections) {
                if (!section.Encrypted()) {
                    continue;
                }
                const std::uint64_t lo = std::max(section.offset, base);
                const std::uint64_t hi = std::min(section.End(), base + out.pos);
                if (lo >= hi) {
                    continue;
                }
                const std::uint64_t aligned = lo & ~std::uint64_t{0xF};
                const std::array<std::uint8_t, 16> counter = NczSectionCounter(section, aligned);
                AesCtrRange(section.key.data(), counter.data(), lo, bytes + (lo - base),
                            static_cast<std::size_t>(hi - lo));
            }
            stream.decoded.insert(stream.decoded.end(), bytes, bytes + out.pos);
            stream.decoded_end += out.pos;
        } else if (stream.source_exhausted && stream.input_pos >= stream.input.size()) {
            *error = "the compressed content ended before the whole NCA was decoded";
            return false;
        }
    }
    /* A stream that decodes past the NCA it claims is a broken file, not a bigger install. */
    if (stream.decoded_end > nca_size) {
        *error = "the compressed content decodes to more than the NCA it claims";
        return false;
    }
    return true;
}

bool GameInstall::ReadCompressed(const TaskContent &content, NczStream &stream,
                                 std::uint64_t content_offset, std::size_t size, std::uint8_t *out,
                                 std::string *error) {
    std::size_t done = 0;
    while (done < size) {
        const std::uint64_t at = content_offset + done;
        if (at < kNczHeaderSize) {
            /* The NCA header is stored verbatim -- no decoding, and an XCI's distribution bit is
               rewritten here like it is for a plain NCA. */
            const std::size_t take = std::min<std::size_t>(
                size - done, static_cast<std::size_t>(kNczHeaderSize - at));
            if (!m_reader.Read(content.offset + at, take, out + done)) {
                *error = "the source stopped early (connection lost or file truncated)";
                return false;
            }
            done += take;
            continue;
        }
        if (!DecodeUntil(stream, content.size, at + 1, error)) {
            return false;
        }
        if (stream.decoded_base > at) {
            *error = "the compressed content was asked for bytes already handed out";
            return false;
        }
        const std::uint64_t available = stream.decoded_end - at;
        const std::size_t take =
            static_cast<std::size_t>(std::min<std::uint64_t>(size - done, available));
        if (take == 0) {
            *error = "the compressed content ended before the whole NCA was decoded";
            return false;
        }
        const std::size_t start = static_cast<std::size_t>(at - stream.decoded_base);
        std::memcpy(out + done, stream.decoded.data() + start, take);
        /* Drop what the caller took: the buffer then stays the size of one read, not of the whole
           content. */
        stream.decoded.erase(stream.decoded.begin(),
                             stream.decoded.begin() + static_cast<std::ptrdiff_t>(start + take));
        stream.decoded_base = at + take;
        done += take;
    }
    return true;
}

bool GameInstall::WriteRange(const RangeRequest &range, std::uint64_t content_offset,
                             std::uint64_t size, std::string *error) {
    /* A first block that carries the very start of an NCA is where an XCI's distribution bit has
       to be rewritten: the console only launches gamecard content from the card, so a dump
       installed to SD/NAND needs the header to say "system".  The header is encrypted, so it is
       decrypted, changed and encrypted again, and the stale signature is accepted by the default
       skip options -- exactly what the reference implementation does. */
    bool header_touched = false;
    if (content_offset < kNcaHeaderSize && m_options.ignore_distribution_bit == false) {
        std::uint8_t decrypted[kNcaHeaderSize];
        std::uint8_t reencrypted[kNcaHeaderSize];
        if (!DecryptNcaHeader(m_buffer, decrypted, error)) {
            return false;
        }
        NcaHeaderInfo header;
        if (ParseNcaHeader(decrypted, kNcaHeaderSize, &header, error) && header.is_gamecard()) {
            SetDistributionToSystem(decrypted);
            if (!EncryptNcaHeader(decrypted, reencrypted, error)) {
                return false;
            }
            std::memcpy(m_buffer, reencrypted, kNcaHeaderSize);
            m_status.modified = true;
            header_touched = true;
        }
    }
    (void)header_touched;
    if (!m_installer.Write(m_task, range, m_buffer, static_cast<std::size_t>(size), error)) {
        return false;
    }
    Advance(m_task, range, size);
    return true;
}

bool GameInstall::Step(std::string *error) {
    RangeRequest range;
    if (!NextRange(m_task, kBlockSize, &range)) {
        /* Everything is in: the caller moves to Finish(). */
        return false;
    }
    if (m_buffer == nullptr) {
        m_buffer = new std::uint8_t[kBlockSize];
        m_buffer_size = kBlockSize;
    }
    if (range.size > m_buffer_size) {
        *error = "the requested range is larger than the transfer buffer";
        return false;
    }
    const TaskContent &content = m_task.contents[range.index];
    /* Where this range starts inside the content: for a plain content the range's own offset is
       the container's, for an NCZ stream the decoded position is the only one that means anything
       (and it is also what the placeholder write and the header rewrite need). */
    const std::uint64_t content_offset = content.written;
    std::size_t read_size = static_cast<std::size_t>(range.size);
    if (content.compressed) {
        if (m_ncz[range.index] == nullptr) {
            *error = "a compressed content arrived without its decoder";
            m_status.stage = InstallStage::Failed;
            m_status.error = *error;
            return false;
        }
        if (!ReadCompressed(content, *m_ncz[range.index], content_offset, read_size, m_buffer, error)) {
            m_status.stage = InstallStage::Failed;
            m_status.error = *error;
            return false;
        }
    } else if (!m_reader.Read(range.offset, read_size, m_buffer)) {
        *error = "the source stopped early (connection lost or file truncated)";
        m_status.stage = InstallStage::Failed;
        m_status.error = *error;
        return false;
    }
    if (!WriteRange(range, content_offset, read_size, error)) {
        m_status.stage = InstallStage::Failed;
        m_status.error = *error;
        return false;
    }
    RefreshStatus();
    return true;
}

bool GameInstall::Finish(std::string *error) {
    if (!TransferComplete(m_task)) {
        *error = "the install cannot finish before every content has arrived";
        m_status.stage = InstallStage::Failed;
        m_status.error = *error;
        return false;
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
    /* The record's payload is built in the console's own layout, not copied from the CNMT file --
       see BuildMetaRecordBlob.  Without this the console stores a record whose content list it
       cannot read, which is how a title ends up with no control data. */
    if (!BuildMetaRecordBlob(m_primary->blob, m_primary->entry->name, m_package.container,
                             &inputs.meta_blob, error)) {
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
        if (!BuildMetaRecordBlob(meta.blob, meta.entry->name, m_package.container, &extra.meta_blob,
                                 error)) {
            m_status.stage = InstallStage::Failed;
            m_status.error = *error;
            return false;
        }
        inputs.extra_metas.push_back(std::move(extra));
    }
    inputs.ticket = m_package.ticket;
    inputs.certificate = m_package.certificate;
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
    return ok;
}

void GameInstall::RefreshStatus() {
    m_status.total_bytes = TotalBytes(m_task);
    m_status.done_bytes = DoneBytes(m_task);
    m_status.done_contents = DoneContents(m_task);
    m_status.total_contents = static_cast<int>(m_task.contents.size());
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
