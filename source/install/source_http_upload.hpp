#pragma once

/* The browser as a package source: the page slices the file it holds and posts exactly the ranges
   this console asks for.  Random access is the browser's job (a File is a random-access blob), so
   the fetch order stays ours -- the meta content first, then each content in turn -- and nothing
   here has to buffer gigabytes: one range at a time is asked for, posted, consumed and dropped.

   The rendezvous is a one-slot handshake:

     * the reader (the transfer's fetch thread, or the UI thread while the package is being read)
       asks for a range: that range becomes what the page is told to send next;
     * the server hands the posted body over (HttpServer::TakeHandoff), and this class reads it;
     * the next Read() finds it in the slot and copies it out.

   Two failures have to be distinguishable, and both end the install with a sentence the player can
   act on: the browser went away (no bytes for a while -> "the browser stopped sending"), and the
   task was abandoned from the console side (cancel). */

#include <cstdint>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "install/container.hpp"
#include "install/upload_protocol.hpp"

namespace acnh_manager {
class Log; /* log.hpp */
}

namespace acnh_manager::install {

/* The upload failure the screens have wording of their own for: the browser stopped sending (its
   screen locked, the tab was closed, the phone left the network).  A sentence of the installer's
   own describes the byte it was reading when the silence began, which sends the player looking at
   the file instead of at the browser -- so the app maps this one to "re-open the page and pick the
   file again" wherever a failure is shown.  Kept next to the source that produces it, the way
   `kErrorGameRunning` is kept next to the engine. */
inline constexpr const char *kErrorUploadStopped = "upload-stopped";

class HttpUploadSource final : public ContainerReader {
public:
    HttpUploadSource() = default;
    ~HttpUploadSource() override;
    HttpUploadSource(const HttpUploadSource &) = delete;
    HttpUploadSource &operator=(const HttpUploadSource &) = delete;

    /* Starts a run: the file the page picked, its size, and an id the page has to quote back with
       every part (two tabs, or a stale tab, then cannot feed the same task).  The id comes from the
       app, because the page learns it in the answer to its own `POST /upload/begin`. */
    bool Begin(const std::string &name, std::uint64_t size, const std::string &task_id,
               std::string *error);
    void Close();
    bool IsOpen() const;
    const std::string &Name() const { return m_name; }
    const std::string &TaskId() const { return m_task_id; }
    /* The size the page declared.  Not part of ContainerReader (the container layer gets it passed
       in), but a resume needs it: `install-task.json` records it so "the same file" is checkable. */
    std::uint64_t Size() const { return m_size; }

    bool Read(std::uint64_t offset, std::size_t size, std::uint8_t *out) override;
    /* The engine's long stretches (parsing, ncm's placeholder work, decompression between reads):
       serve the page there too, or its requests queue in the kernel until the silence timer gives up
       (see ContainerReader::Service). */
    void Service() override;

    /* What the reader wants next (the page asks through `GET /upload/next`).  False when nothing is
       wanted right now -- the package is being confirmed, or the run is over. */
    bool Want(std::uint64_t *offset, std::size_t *size) const;
    /* Is the console currently waiting for a part, and if so how many seconds until it gives up?
       False during normal traffic and for the first two seconds of a wait; the progress screen
       turns this into a visible countdown only after a silence has become noticeable. */
    bool PageWaitSeconds(int *seconds_left) const;
    /* The connection the server handed over: the part the browser is posting now.  `prefix` is what
       the server had already read of the body (a part under its small-body limit arrives whole). */
    void AttachPart(int fd, std::uint64_t offset, std::size_t length, std::string prefix);
    /* Called while Read() waits: whoever is blocked can serve the page.  Without this, a console
       reading the package *before* the transfer (Prepare) would deadlock -- the page waits for
       `/upload/next`, and the thread that would answer it is the one waiting for the part. */
    void SetPump(std::function<void()> pump);
    /* Called at a slower cadence (four times a second) from the same wait: the UI draws the
       countdown with it while the console is still *reading* the package (Prepare).  It must only
       be installed around a read that runs on the UI thread -- during the transfer the reader
       thread owns Read(), and drawing from there is what killed the app on 2026-10-19. */
    void SetWaitPump(std::function<void()> pump);
    /* How many page requests the server has answered, for the "still waiting" heartbeat: while a
       read is stuck, that number says whether the console is the silent party (it is not, when the
       count keeps climbing) or the browser is.  The heartbeat is the only way to know from the
       console's own log, because the log is buffered inside the engine's burst. */
    void SetRequestsProbe(std::function<int()> probe);
    /* Diagnostics, one line per handshake step: "the page never got the range it was asked for"
       and "the bytes arrived somewhere else" look identical from the console's screens. */
    void SetLog(acnh_manager::Log *log) { m_log = log; }
    /* The browser went away, or the run was cancelled: every waiting read fails from here. */
    void Abort(const std::string &reason);
    std::string AbortReason() const;
    /* Why it gave up, as a code the screens have wording for -- empty when it did not fail.  The
       diagnostic sentence (which byte, how much had arrived) stays in `AbortReason` and goes to
       log.txt; a player reading "cannot read the ticket" on a phone that locked its screen goes
       looking at a file that is fine. */
    std::string AbortCode() const;

private:
    /* Reads the body of the part in hand into the slot and answers the browser.  Blocking (with a
       timeout); must not hold `m_mutex` while it waits. */
    bool ServiceAttachedPart();
    void Trace(const char *format, ...) __attribute__((format(printf, 2, 3)));

    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::string m_name;
    std::string m_task_id;
    std::uint64_t m_size{0};
    bool m_open{false};
    std::string m_abort_reason;
    /* Set alongside the reason when the failure is one the screens word themselves. */
    std::string m_abort_code;
    /* What the reader is waiting for, and the bytes that arrived for it. */
    std::uint64_t m_want_offset{0};
    std::size_t m_want_size{0};
    bool m_has_want{false};
    /* Set while Read() is blocked waiting for the next part; the UI reads it to show the countdown. */
    bool m_page_waiting{false};
    std::uint64_t m_page_wait_started_ms{0};
    std::uint64_t m_page_wait_deadline_ms{0};
    std::uint64_t m_slot_offset{0};
    std::vector<std::uint8_t> m_slot; /* empty = nothing arrived yet */
    /* The part connection in hand, if any (see AttachPart). */
    int m_part_fd{-1};
    std::uint64_t m_part_offset{0};
    std::size_t m_part_length{0};
    std::string m_part_prefix;
    std::function<void()> m_pump;
    std::function<void()> m_wait_pump;
    std::uint64_t m_last_wait_pump_ms{0};
    std::function<int()> m_requests_probe;
    acnh_manager::Log *m_log{nullptr};
    /* The `pump` pointer is set once, before any read; the reads may come from two threads in
       sequence (Prepare, then the transfer), never at the same time. */
};

}  // namespace acnh_manager::install
