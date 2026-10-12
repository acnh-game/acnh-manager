#include "install/game_install.hpp"

#include <algorithm>
#include <cstring>

#include "install/nca.hpp"
#include "install/nca_crypto.hpp"

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
    /* The CNMT and the NACP live *inside* their contents, and how those contents are encrypted is
       the system's business: write the meta content first, ask ncm where its placeholder lives,
       mount that path as a content-meta file system and read the file out.  This is the reference
       implementation's sequence, and it is the only one that needs no key material of ours. */
    const ContainerEntry *meta_entry = nullptr;
    for (const auto &entry : m_package.container.contents) {
        if (entry.name.size() > 32 && entry.name.compare(32, 9, ".cnmt.nca") == 0) {
            meta_entry = &entry;
            break;
        }
    }
    if (meta_entry == nullptr) {
        *error = "this package has no meta content";
        m_status.stage = InstallStage::Failed;
        m_status.error = *error;
        return false;
    }
    /* The mount is keyed by the title id, which is a field of the meta content's own header. */
    std::uint64_t title_id = 0;
    {
        std::vector<std::uint8_t> encrypted(kNcaHeaderSize);
        std::vector<std::uint8_t> decrypted(kNcaHeaderSize);
        if (!m_reader.Read(meta_entry->offset, encrypted.size(), encrypted.data())) {
            *error = "cannot read the meta content's header";
            m_status.stage = InstallStage::Failed;
            m_status.error = *error;
            return false;
        }
        if (!DecryptNcaHeader(encrypted.data(), decrypted.data(), error)) {
            m_status.stage = InstallStage::Failed;
            m_status.error = *error;
            return false;
        }
        NcaHeaderInfo header;
        if (!ParseNcaHeader(decrypted.data(), decrypted.size(), &header, error)) {
            m_status.stage = InstallStage::Failed;
            m_status.error = *error;
            return false;
        }
        title_id = std::strtoull(header.program_id.c_str(), nullptr, 16);
    }
    {
        TaskContent meta_content;
        meta_content.name = meta_entry->name;
        meta_content.offset = meta_entry->offset;
        meta_content.size = meta_entry->size;
        meta_content.content_id = meta_entry->name.substr(0, 32);
        meta_content.kind = ContentKind::Meta;
        Placeholder placeholder;
        if (!m_installer.InstallContentForRead(m_reader, meta_content, &placeholder, error)) {
            m_status.stage = InstallStage::Failed;
            m_status.error = *error;
            return false;
        }
        m_meta_placeholder = placeholder;
        std::string meta_path;
        if (!m_installer.PlaceholderFilePath(placeholder, &meta_path, error)) {
            m_status.stage = InstallStage::Failed;
            m_status.error = *error;
            return false;
        }
        std::vector<std::uint8_t> blob;
        std::string first_error;
        if (!m_installer.ReadFromContent(meta_path, title_id, FsFileSystemType_ContentMeta, "", &blob,
                                         &first_error)) {
            /* The reference implementation mounts the placeholder path directly; when the system
               refuses it, the documented fallback is to register the content first and mount the
               *content* path instead.  Both errors are logged so the next run says which one
               happened. */
            meta_content.placeholder_id = placeholder.placeholder_id;
            std::string register_error;
            std::string content_path;
            if (m_installer.RegisterContent(meta_content, &register_error) &&
                m_installer.ContentPath(meta_content.content_id, &content_path, &register_error)) {
                std::string second_error;
                if (!m_installer.ReadFromContent(content_path, title_id,
                                                 FsFileSystemType_ContentMeta, "", &blob,
                                                 &second_error)) {
                    /* Both attempts failed: the caller's log line has to carry everything a
                       reader needs (the path ncm handed out and each failure), because that is
                       what the next debugging round has to work from. */
                    *error = "cannot read the CNMT (placeholder " + meta_path + ": " + first_error +
                             "; content " + content_path + ": " + second_error + ")";
                    m_status.stage = InstallStage::Failed;
                    m_status.error = *error;
                    return false;
                }
            } else {
                *error = register_error;
                m_status.stage = InstallStage::Failed;
                m_status.error = *error;
                return false;
            }
        }
        if (!ParseCnmt(blob.data(), blob.size(), &m_package.cnmt, error)) {
            m_status.stage = InstallStage::Failed;
            m_status.error = *error;
            return false;
        }
        m_package.meta_blob = blob;
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

    /* The control content carries the NACP: the game's name and the version string the player
       knows.  Which content that is comes from the *NCA header* of each file, not from the CNMT's
       per-content type field -- the reference implementation reads it the same way, and the CNMT
       field said "no control content" for a package that plainly has one.  Same read mechanism as
       the CNMT: install the content, mount it, read the file out. */
    {
        std::string nacp_note = "nacp=none";
        const ContainerEntry *control_entry = nullptr;
        /* The id the mount needs is the one in *this* content's own header: the reference
           implementation passes `nca.header.program_id`, and for an update package that differs
           from the meta content's id (patch id vs base id) -- which is exactly what made the
           mount fail. */
        std::uint64_t control_title_id = title_id;
        for (const auto &entry : m_package.container.contents) {
            if (entry.name.size() <= 32 || entry.name.compare(entry.name.size() - 4, 4, ".nca") != 0) {
                continue;
            }
            if (entry.size < kNcaHeaderSize) {
                continue;
            }
            std::vector<std::uint8_t> encrypted(kNcaHeaderSize);
            std::vector<std::uint8_t> decrypted(kNcaHeaderSize);
            std::string header_error;
            if (!m_reader.Read(entry.offset, encrypted.size(), encrypted.data()) ||
                !DecryptNcaHeader(encrypted.data(), decrypted.data(), &header_error)) {
                continue;
            }
            NcaHeaderInfo header;
            if (!ParseNcaHeader(decrypted.data(), decrypted.size(), &header, &header_error)) {
                continue;
            }
            if (header.content_type == ContentType::Control) {
                control_entry = &entry;
                const std::uint64_t own = std::strtoull(header.program_id.c_str(), nullptr, 16);
                if (own != 0) {
                    control_title_id = own;
                }
                break;
            }
        }
        if (control_entry == nullptr) {
            nacp_note = "nacp=no-control-nca";
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
                nacp_note = "nacp=install-failed:" + nacp_error;
            } else if (!m_installer.PlaceholderFilePath(placeholder, &path, &nacp_error)) {
                nacp_note = "nacp=path-failed:" + nacp_error;
                m_installer.DropPlaceholders({placeholder}, &nacp_error);
            } else {
                m_read_placeholders.push_back(placeholder);
                control.placeholder_id = placeholder.placeholder_id;
                m_control_placeholder = placeholder;
                std::vector<std::uint8_t> nacp;
                if (!m_installer.ReadFromContent(path, control_title_id,
                                                 FsFileSystemType_ContentControl, "/control.nacp",
                                                 &nacp, &nacp_error)) {
                    nacp_note = "nacp=read-failed:" + nacp_error;
                } else if (nacp.size() < sizeof(NacpStruct)) {
                    nacp_note = "nacp=too-small";
                } else {
                    NacpStruct parsed{};
                    std::memcpy(&parsed, nacp.data(), sizeof(parsed));
                    NacpLanguageEntry *entry = nullptr;
                    if (R_SUCCEEDED(nacpGetLanguageEntry(&parsed, &entry)) && entry != nullptr) {
                        m_status.game_name = entry->name;
                    } else {
                        for (int i = 0; i < 16 && m_status.game_name.empty(); ++i) {
                            m_status.game_name = parsed.lang[i].name;
                        }
                    }
                    m_status.game_version = parsed.display_version;
                    nacp_note = "nacp=ok";
                }
            }
        }
        m_status.detail += " " + nacp_note;
    }

    /* The content list comes from the CNMT we just read: this is what turns "33 entries" into
       "the program, the control and the tickets", skips the delta fragments (the package does not
       ship them) and refuses a package that lists a content it does not carry.  This call was
       lost to a careless range-edit earlier, which is why the confirm page showed 0.0 GB and the
       transfer had nothing to do: the parse was right, nothing consumed it. */
    if (!FillTaskContents(m_package, &m_task, error)) {
        m_status.stage = InstallStage::Failed;
        m_status.error = *error;
        return false;
    }

    /* The meta content is already in its placeholder: mark it so the transfer phase does not
       fetch it a second time (and so Register finds it complete). */
    for (auto &content : m_task.contents) {
        if (content.content_id == m_meta_placeholder.content_id) {
            content.placeholder_id = m_meta_placeholder.placeholder_id;
            content.written = content.size;
        } else if (content.content_id == m_control_placeholder.content_id) {
            content.placeholder_id = m_control_placeholder.placeholder_id;
            content.written = content.size;
        }
    }
    /* Ask the console which version it has and decide before anything moves: the skip rules are
       "this exact version is already installed" and "the console has something newer and we do
       not downgrade unless asked" (design section 4).  This block was lost to a careless
       range-edit earlier, which let the app install whatever it was given -- the wording on the
       result page was the visible symptom. */
    std::uint32_t installed_version = 0;
    {
        std::string version_error;
        if (!m_installer.InstalledVersion(m_package.cnmt.title_id, &installed_version,
                                          &version_error)) {
            *error = version_error;
            m_status.stage = InstallStage::Failed;
            m_status.error = *error;
            return false;
        }
    }
    m_status.package_version = m_package.cnmt.version;
    m_status.installed_version = installed_version;
    {
        char installed_note[64];
        std::snprintf(installed_note, sizeof(installed_note), " installed=0x%x", installed_version);
        m_status.detail += installed_note;
    }
    const SkipDecision decision = DecideSkip(m_options, m_package.cnmt.version, installed_version);
    if (decision != SkipDecision::Install) {
        m_status.stage = InstallStage::Skipped;
        m_status.skip_message =
            decision == SkipDecision::AlreadyInstalled ? "already-installed" : "newer-installed";
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
    if (m_meta_placeholder.placeholder_id.empty()) {
        return true;
    }
    std::string ignored;
    const bool ok = m_installer.DropPlaceholders(m_read_placeholders, &ignored);
    m_read_placeholders.clear();
    m_meta_placeholder.placeholder_id.clear();
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

bool GameInstall::WriteRange(const RangeRequest &range, std::uint64_t size, std::string *error) {
    /* A first block that carries the very start of an NCA is where an XCI's distribution bit has
       to be rewritten: the console only launches gamecard content from the card, so a dump
       installed to SD/NAND needs the header to say "system".  The header is encrypted, so it is
       decrypted, changed and encrypted again, and the stale signature is accepted by the default
       skip options -- exactly what the reference implementation does. */
    bool header_touched = false;
    if (range.offset < kNcaHeaderSize && m_options.ignore_distribution_bit == false) {
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
    if (!m_reader.Read(range.offset, static_cast<std::size_t>(range.size), m_buffer)) {
        *error = "the source stopped early (connection lost or file truncated)";
        m_status.stage = InstallStage::Failed;
        m_status.error = *error;
        return false;
    }
    if (!WriteRange(range, range.size, error)) {
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
    if (!m_installer.Register(m_task, error)) {
        m_status.stage = InstallStage::Failed;
        m_status.error = *error;
        return false;
    }
    ContentInstaller::FinishInputs inputs;
    inputs.title_id = m_package.cnmt.title_id;
    inputs.version = m_package.cnmt.version;
    inputs.meta_type = static_cast<std::uint8_t>(m_package.cnmt.type);
    inputs.meta_blob = m_package.meta_blob;
    for (const auto &content : m_task.contents) {
        inputs.content_ids.push_back(content.content_id);
    }
    inputs.ticket = m_package.ticket;
    inputs.certificate = m_package.certificate;
    if (!m_installer.Finish(inputs, error)) {
        m_status.stage = InstallStage::Failed;
        m_status.error = *error;
        return false;
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
    m_meta_placeholder.placeholder_id.clear();
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

}  // namespace acnh_manager::install
