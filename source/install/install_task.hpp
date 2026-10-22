#pragma once

/* The install task: which bytes still have to come from the source, in which order, and how far
   each content has got.  This is the pure half of the installer -- the sources (a file on the
   SD card, an HTTP download, a browser upload) and the writer (ncm placeholders) sit on either
   side of it, and both are device code.

   Two rules shape everything here:

   1. **The fetch order belongs to us, not to the container.**  The container gives a flat list
      of contents; the meta content has to be read first (it carries the CNMT that tells us what
      the package *is*), and after that each content is fetched as one contiguous run.  That is
      why progress is tracked per content and not as "how far into the file are we".
   2. **Progress is measured in bytes, never in contents.**  In a real update package one NCA is
      several GB while the rest are kilobytes, so "3 of 5 contents" would sit at 20% until the
      very end.  `DoneBytes` / `TotalBytes` are what the UI and the browser page show.

   The task is also what survives a crash: `install-task.json` is this struct, so an interrupted
   upload resumes from the per-content byte counts instead of starting over. */

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "install/options.hpp"

namespace acnh_manager::install {

/* Where the bytes come from.  One task has exactly one source at a time: a page that submits a
   download address turns an `HttpUpload` task into an `HttpDownload` one instead of running
   both (design section 3). */
enum class TaskSource {
    None,
    LocalFile,
    HttpDownload,
    HttpUpload,
    Internal,
};

/* Lifecycle.  `Finalizing` is the point of no return: from there on the console is writing the
   application record and deleting the older version, so cancelling is refused (design section
   4 and section 10). */
enum class TaskStage {
    Idle,       /* no task */
    Waiting,    /* source screen: the browser has not sent anything yet */
    Fetching,   /* pulling container bytes into the placeholders */
    Finalizing, /* register + ticket + delete old version + database */
    Done,
    Failed,
};

/* What a content is, from its CNMT entry.  Only the skip options and the "install agent?"
   decision need this; the task itself just carries the label. */
enum class ContentKind {
    Meta,
    Program,
    Control,
    Legal,
    Offline,
    Data,
    PublicData,
    DeltaFragment,
    Unknown,
};

struct TaskContent {
    std::string name;       /* inside the container, as the package spells it */
    std::uint64_t offset{0}; /* absolute offset in the container */
    /* What the console ends up with: the NCA's own size.  For a plain NCA that is also what gets
       transferred; for an NSZ/XCZ content it is what the NCZ stream decodes to (see `compressed`). */
    std::uint64_t size{0};
    /* The bytes the entry occupies in the container.  Same as `size` unless `compressed`. */
    std::uint64_t packed_size{0};
    /* An NCZ stream (NSZ/XCZ): the bytes have to be decoded before they can be written. */
    bool compressed{false};
    std::uint64_t written{0}; /* bytes already in this content's placeholder */
    std::string content_id;     /* 32 hex digits, empty until the CNMT has been read */
    std::string placeholder_id; /* 16 hex digits: where the bytes go, recorded for resumes */
    ContentKind kind{ContentKind::Unknown};
    bool skipped{false}; /* already on the console, or a kind this run does not install */
};

/* The content being written, as the 1-based number both the console card and the page show.
   `done_contents` only counts finished ones; at the end done == total, so the result is the
   total.  One function for both screens keeps "2 / 3" from meaning different things on each
   (2026-10-19). */
inline int CurrentContentNumber(int done_contents, int total_contents) {
    if (total_contents <= 0) {
        return 0;
    }
    const int current = done_contents + 1;
    return current > total_contents ? total_contents : current;
}

struct InstallTask {
    std::string task_id;   /* opaque, handed to the browser page; empty for local installs */
    TaskSource source{TaskSource::None};
    std::string name;      /* the file as the player sees it (SD browser / browser upload) */
    std::string url;       /* download source only */
    std::uint64_t container_size{0}; /* 0 = unknown (a source that cannot tell) */
    /* Which storage this run writes to.  The task file needs it because the placeholders live
       *there*: without it, "give up and clean up" after a restart would have to guess which storage
       to empty, and the player may well have switched the target in the meantime. */
    TargetStorage target{TargetStorage::SdCard};
    std::vector<TaskContent> contents; /* container order */
    TaskStage stage{TaskStage::Idle};
    std::string error;     /* player-facing wording for the Failed stage */
};

/* Bytes that still have to be transferred, and bytes already transferred.  Skipped contents
   (already installed, or a kind this run does not install) count towards neither. */
std::uint64_t TotalBytes(const InstallTask &task);
std::uint64_t DoneBytes(const InstallTask &task);

/* How big the package is, whatever the console already has: the confirm screen shows this (the
   player picked a file that is *this* many bytes), while the progress bars show TotalBytes (the
   part that still has to move).  Showing the remaining bytes on the confirm screen printed
   "0.0 GB" for a package whose contents were all already on the console. */
std::uint64_t PackageBytes(const InstallTask &task);

/* Contents that are finished -- written in full or skipped.  The progress screen shows it as
   "content N of M", which is why skipped entries still count here. */
int DoneContents(const InstallTask &task);

/* The next run of bytes the installer wants.  `max_size` caps one request (the design fixes it
   at 4 MB: one HTTP request, one chunk of the browser upload, one read of the local file).
   Returns false when every content is in, which is the caller's signal to move to Finalizing. */
struct RangeRequest {
    std::size_t index{0};   /* contents[] index */
    std::uint64_t offset{0}; /* in the container: contents[index].offset + written */
    std::uint64_t size{0};   /* never crosses into the next content */
};
bool NextRange(const InstallTask &task, std::uint64_t max_size, RangeRequest *out);

/* Records that `size` bytes of `range` arrived and landed in the placeholder. */
void Advance(InstallTask &task, const RangeRequest &range, std::uint64_t size);

/* True when the task is finished with the transfer: every content skipped or written in full. */
bool TransferComplete(const InstallTask &task);

/* A resumed upload has to be the same file the task was built from.  The browser cannot prove
   it (it never sees our offsets), so the file name and the total size are checked -- and the
   content-level verification behind the installer is the real backstop. */
bool MatchesSourceFile(const InstallTask &task, std::string_view name, std::uint64_t size);

/* What happens to one content when an interrupted install is picked up again.

   `recorded` is the entry from `install-task.json`, `fresh` the entry this run built from the
   package, and `placeholder_present` the console's answer to "are those bytes still there?".
   Three rules, in the order they matter:

   1. the bytes are still on the console (the placeholder exists) **and** the content is the same
      one (same id and size) -> continue where the file says it stopped;
   2. a *compressed* content starts over even then: the NCZ stream can only be decoded forward
      from its first block, so resuming inside one would mean re-reading (and re-downloading) the
      payload anyway -- doing it honestly is cheaper than pretending;
   3. anything else -- no placeholder, a different size, a content the file does not mention --
      starts from zero, because a `written` that describes bytes nobody has would leave a hole in
      the middle of the content. */
struct ResumePlan {
    bool restart{true};       /* start this content over (create a fresh placeholder) */
    std::uint64_t written{0}; /* bytes already in the placeholder, when `restart` is false */
    std::string placeholder_id;
};
ResumePlan PlanResume(const TaskContent &recorded, const TaskContent &fresh,
                      bool placeholder_present);

/* Where the fetch thread starts reading a content's source stream.

   A plain content picks up at the byte the placeholder is waiting for: those are the same bytes in
   the file (the placeholder's next offset is the container's next offset).  A compressed content
   always starts over (rule 2 of PlanResume), so its NCZ stream starts at zero. */
std::uint64_t InitialStreamRead(const TaskContent &content);

/* `install-task.json` (same shape as the in-memory struct; the caller owns the file IO). */
std::string DumpTask(const InstallTask &task);
bool ParseTask(std::string_view text, InstallTask *out, std::string *error);

}  // namespace acnh_manager::install
