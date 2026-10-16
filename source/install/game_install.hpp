#pragma once

/* The orchestrator: one package, from "what is this" to "the console knows about it", driven one
   step at a time so the UI can draw between steps.

   The division of labour is deliberate.  Everything that can be decided from bytes lives in
   container.* / nca.* / package.*, everything that touches ncm/es/ns lives in content_install.*,
   and this file is only the sequence plus the bookkeeping the screens need (stage, byte counts,
   the current content's name).  A failed step leaves the task file describing exactly where it
   stopped, which is what makes "resume" the same code path as "start". */

#include <switch.h>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "install/content_install.hpp"
#include "install/container.hpp"
#include "install/install_task.hpp"
#include "install/ncz.hpp"
#include "install/ncz_decode.hpp"
#include "install/options.hpp"
#include "install/package.hpp"
#include "install/transfer_queue.hpp"

namespace acnh_manager {
class Log;
}

namespace acnh_manager::install {

/* Where the installer's diagnostics go (the app hands over its log at startup).  Several short
   lines beat one long one: the log truncates a line, which is how a useful self-check was lost. */
void SetGameInstallLog(acnh_manager::Log *log);

/* The one failure the installer reports as a *code* instead of a sentence: the console has a game
   running, and the screens have wording of their own for it ("close the game first, then try
   again").  It is a constant because three screens can show it, and a literal in four places is
   how one of them ends up printing the code itself -- which is exactly what the address screen did
   (2026-10-15, on hardware: the strip read "game-running"). */
inline constexpr const char *kErrorGameRunning = "game-running";

enum class InstallStage {
    Idle,
    Waiting,    /* the source screen: no bytes have arrived yet */
    Fetching,   /* bytes are moving */
    Finalizing, /* register + finish: cancelling is closed from here on */
    Done,
    Skipped,    /* already installed, or a newer version is on the console */
    Failed,
};

struct InstallStatus {
    InstallStage stage{InstallStage::Idle};
    std::string error;        /* one line for the result page */
    std::string skip_message; /* why we skipped, when stage == Skipped */
    std::uint64_t total_bytes{0};
    std::uint64_t done_bytes{0};
    std::string current_name;
    int done_contents{0};
    int total_contents{0};
    bool modified{false};     /* a header was rewritten (XCI distribution bit) */
    /* One line for the log: the identity the installer read and the console's answer, so a run
       that surprises us can be explained from log.txt instead of guessed at. */
    std::string detail;
    /* What the package says about itself (from its NACP): the name and the version string a
       person recognises.  Empty until the control content has been read, which is also what
       decides whether the "the agent can be installed afterwards" note is even true. */
    std::string game_name;
    /* The console's own name for the same package, when its language setting differs from this
       app's: the screens draw it as a second, smaller line under `game_name` (see
       install/title_name.hpp for why the app's language leads).  Empty when there is nothing to
       add. */
    std::string game_name_alt;
    std::string game_version;
    /* Both versions, so the screens can say *why* something was skipped without depending on
       which code path produced the skip. */
    std::uint32_t package_version{0};
    std::uint32_t installed_version{0};
    /* What the console calls the version it has, when it can say it ("3.0.3"): the title version
       number above is Nintendo's build id (3.0.3 = 0x220000), so "newer version (34.0.0)" is what
       printing it as a version produces.  Empty when the console has no control data for the
       title, and the screens then leave the number out. */
    std::string installed_version_text;
    bool needs_keys{false};   /* the run may need /switch/prod.keys */
    /* What else the container brings, in the app's language ("更新 ×1 · 追加内容 ×2"): a
       cartridge dump that was made from a game plus its update and DLCs carries one meta per
       piece, and the confirm page says so instead of quietly installing only the base game. */
    std::string extra_summary;
    /* The version the console ends up on: the newest of the metas that carry a game version (the
       update's, when the container has one).  The agent's "does it work with this version?" is
       answered with this one, not with the base game's version. */
    std::uint32_t effective_version{0};
    std::string effective_title_id;
};

class GameInstall {
public:
    GameInstall(ContainerReader &reader, std::string name, std::uint64_t container_size,
                TargetStorage target, const InstallOptions &options);
    ~GameInstall();
    GameInstall(const GameInstall &) = delete;
    GameInstall &operator=(const GameInstall &) = delete;

    /* Reads the container, the CNMT and the ticket, asks the console which version of that title
       it already has, and decides whether this package is worth installing.  The console's answer
       is read here rather than passed in: the skip rules belong to the installer, and a caller who
       has to remember to look it up is a caller who will forget. */
    bool Prepare(std::string *error);

    /* Creates the placeholders and records the task.  Call after the confirm page is accepted. */
    bool Begin(std::string *error);

    /* Fetches and writes one range (4 MB).  False means either "nothing left" (check
       TransferComplete) or a failure; `error` says which. */
    bool Step(std::string *error);

    /* Registers what arrived and runs the finish sequence (ticket, delete old, record, launch
       version).  From here on the install cannot be cancelled. */
    bool Finish(std::string *error);

    /* Undo: drop the placeholders and the records this run created.  Used by cancel and by a
       failure the caller decided not to keep for a resume. */
    bool Abandon(std::string *error);
    /* Removes what a failed run created but must not keep (today: the meta placeholder). */
    bool CleanupAfterFailure(std::string *error);

    const PackageInfo &Package() const { return m_package; }
    const InstallTask &Task() const { return m_task; }
    InstallTask &MutableTask() { return m_task; }
    const InstallStatus &Status() const { return m_status; }
    /* The cover the console has for this package's game, as the JPEG it hands out (empty when the
       game is not installed, or when `ns` would not tell us).  The confirm screen draws it; the
       decode lives in the UI layer, because the installer has no business knowing about pixels. */
    const std::vector<std::uint8_t> &IconJpeg() const { return m_icon; }
    std::string TaskJson() const { return DumpTask(m_task); }
    /* Every meta the container carries, the base game's first.  A cartridge dump that was made
       from a game plus its update and DLCs has one per piece; the reference implementation
       installs all of them in one run, each with its own meta record -- which is also how the
       console attaches an update and its add-ons to the same title. */
    struct MetaUnit {
        const ContainerEntry *entry{nullptr};
        Placeholder placeholder;        /* the meta content, already read into a placeholder */
        TaskContent content;            /* the meta content as a task entry (for the register step) */
        std::uint64_t title_id{0};      /* the mount's key: the meta NCA's own header field */
        std::vector<std::uint8_t> blob; /* the CNMT payload */
        CnmtInfo cnmt;
    };
    std::vector<MetaUnit> m_metas;
    /* The meta the screens describe: the base game when the container has one (its control data is
       what carries the title's name and version), otherwise the first meta. */
    MetaUnit *m_primary{nullptr};
    Placeholder m_control_placeholder;
    /* Contents that were installed only so we could read out of them (meta for the CNMT, control
       for the NACP): filled before the transfer phase, and cleaned up if the run fails. */
    std::vector<Placeholder> m_read_placeholders;

private:
    void RefreshStatus();
    /* Writes the bytes just read into the content's placeholder.  `content_offset` is where they
       start *inside the content* (the container offset means nothing for a compressed content,
       and the XCI distribution bit has to be rewritten at the content's own offset 0).  `data` is
       the queue slot the reader filled; the header rewrite edits it in place, which is why it is
       not const. */
    bool WriteRange(const RangeRequest &range, std::uint64_t content_offset, std::uint8_t *data,
                    std::uint64_t size, std::string *error);
    /* Decodes nothing -- the payload piece is already in the decoder -- but writes everything the
       decoder has decoded since the last call, at the content offset the task is at.  Runs on the
       writer thread; this is the half of the pipeline that used to be on the fetch thread. */
    bool WriteDecoded(std::size_t index, NczDecoder &decoder, std::string *error);
    /* The transfer's reader thread: it walks its own copy of the task, reads each range (from the
       source, or through the NCZ decoder) into a queue slot and publishes it.  Everything it can
       fail with goes to the consumer through the queue, never thrown across the thread. */
    void ReadLoop();
    /* A free queue slot for the fetch thread; nullptr (after finishing the queue) when the run was
       stopped or has already failed. */
    std::uint8_t *AcquireSlotOrFinish();
    /* What the fetch thread has read of one content's *source* stream, which is not the same thing
       as what the writer has written: a compressed content's source is the NCZ header (once) plus
       the payload, while its content bytes only exist after the writer decoded them. */
    struct FetchCursor {
        std::uint64_t stream_read{0}; /* payload bytes read (compressed) or container bytes (plain) */
        bool header_sent{false};      /* the NCZ header piece is only read once */
    };
    /* Reads one `.ncz` entry's tables (section table, and the block table when the file has one)
       and reports the NCA it rebuilds.  Called once per compressed content while the package is
       being read. */
    bool ReadNczInfo(const ContainerEntry &entry, NczInfo *out, std::string *error);
    /* Reads one content's own header (the 0xC00 bytes the console decrypts with its header key).
       The header is the authority on how big the NCA is and on which title key it needs, and both
       are facts the rest of the run uses: the placeholder size, the record's content sizes, and
       whether a ticket has to be imported.  False when the content has no readable NCA header. */
    bool ReadContentHeader(const ContainerEntry &entry, NcaHeaderInfo *out, std::string *error);
    /* Reads one meta content out of the container: the meta NCA into a placeholder, the CNMT out
       of it, and the title id its header carries (the mount is keyed by it).  `out_placeholder`
       owns the placeholder, so the caller either keeps it (the meta the run is about) or drops it
       (a meta this run decided not to install). */
    bool ReadMetaContent(const ContainerEntry &entry, Placeholder *out_placeholder,
                         std::uint64_t *out_title_id, std::vector<std::uint8_t> *out_blob,
                         std::string *error);
    /* True when the console can actually use the version it has recorded.  Two things can be true
       about a record without that being true: it can sit under the wrong id (an install that filed
       an update's record as an application of its own left an empty tile on the home screen while
       ncm knew the version), and its payload can be written in a shape the service cannot read (a
       record copied from the CNMT file instead of the layout the console stores leaves a title
       with no control data at all).  Either way the answer is "install again": every content is
       already in the storage, so only the finish step runs. */
    bool ConsoleCanUseInstalledVersion();

    ContainerReader &m_reader;
    std::string m_name;
    TargetStorage m_target;
    InstallOptions m_options;
    PackageInfo m_package;
    InstallTask m_task;
    InstallStatus m_status;
    std::vector<std::uint8_t> m_icon; /* the game's cover, as `ns` hands it out (may be empty) */
    /* The NCZ tables of every compressed content, by content id (read in Prepare, when the size an
       entry stands for is still being worked out). */
    std::map<std::string, NczInfo> m_ncz_info;
    /* The title keys the package's contents ask for, gathered while the headers are read: a content
       with one is encrypted with that key and needs the package's ticket to be imported (the
       reference implementation refuses such a package when the ticket is missing; we log it and
       carry on, because a cartridge dump carries no ticket and still installs). */
    std::vector<std::string> m_rights_ids;
    /* One decoder per task content, in the task's own order (null for plain contents). */
    std::vector<std::unique_ptr<NczDecoder>> m_ncz;
    /* The transfer's pipeline (see install/transfer_queue.hpp): the fetch thread fills slots with
       source bytes and this thread (the UI one) drains them -- decoding a `payload` piece first --
       into the placeholders.  Only the writer touches the task counters, so the two halves cannot
       disagree about how far the run has got; the fetch thread keeps its own cursors in `m_fetch`. */
    std::unique_ptr<TransferQueue> m_queue;
    std::thread m_reader_thread;
    /* One cursor per content for the fetch thread (see FetchCursor). */
    std::vector<FetchCursor> m_fetch;
    /* Development aid (install/dev_stats.*): when Begin() started this run's clock, 0 when no run
       is in flight.  Kept unconditional so the call sites need no #if. */
    std::uint64_t m_transfer_begin_ms{0};
    ContentInstaller m_installer;
    std::vector<Placeholder> m_placeholders;
};

}  // namespace acnh_manager::install
