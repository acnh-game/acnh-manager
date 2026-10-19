#include "install/source_http_upload.hpp"

#include <cstring>
#include <cstdarg>
#include <cstdio>

#include <switch.h>

#include <chrono>

#include <sys/socket.h>
#include <unistd.h>

#include "net/http_page.hpp"
#include "log.hpp"
#include "util/size_text.hpp"

namespace acnh_manager::install {
namespace {

/* How long the reader waits for a part before deciding the browser is gone.  The page sends a part
   per range, so this is "a phone that locked its screen or walked out of range" -- and the message
   then tells the player what to do (re-open the page, pick the same file again).

   The wait is also the moment the page gets served, so it cannot be one long block: the loop below
   wakes often, pumps, and checks the clock. */
constexpr int kPollMs = 50;
/* How long a range may go unanswered before the browser counts as gone.  The page itself gives one
   part 45 seconds before aborting and retrying; the console waits a little longer (50 seconds) so
   that an honest slow part is not killed a moment before the page's own retry can arrive.  Ninety
   seconds made a closed tab feel like a hung console: the player had to sit on "receiving" before
   the failure screen would let them re-open the page and pick the file again.  The pathological
   stalls that first prompted the long wait were the page's keep-awake audio freezing the browser;
   that is fixed at the source, so this number now only needs to cover a slow link. */
constexpr std::uint64_t kSilenceTimeoutMs = 50 * 1000;

std::uint64_t NowMs() {
    return armTicksToNs(armGetSystemTick()) / 1000000ull;
}

}  // namespace

void HttpUploadSource::Trace(const char *format, ...) {
    if (m_log == nullptr) {
        return;
    }
    char line[192];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line) - 24, format, args);
    va_end(args);
    /* `Block`, not `Line`: a package read runs inside the engine's log burst (see LogBurst), which
       buffers every line until it ends -- and the whole point of these lines is to be readable while
       the read is stuck waiting for the browser. */
    /* The same monotonic stamp the server's lines carry: the two together say whether a part was
       posted late or answered late (2026-10-16). */
    char stamped[sizeof(line) + 24];
    std::snprintf(stamped, sizeof(stamped), "[t=%llums] %s",
                  static_cast<unsigned long long>(NowMs()), line);
    std::string text = stamped;
    text += "\n";
    m_log->Block(text);
}

HttpUploadSource::~HttpUploadSource() {
    Close();
}

bool HttpUploadSource::Begin(const std::string &name, std::uint64_t size,
                             const std::string &task_id, std::string *error) {
    if (name.empty() || size == 0) {
        if (error != nullptr) {
            *error = "the browser named no file (or an empty one)";
        }
        return false;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    m_name = name;
    m_size = size;
    m_open = true;
    m_task_id = task_id;
    m_abort_reason.clear();
    m_abort_code.clear();
    m_has_want = false;
    m_slot_offset = 0;
    m_slot.clear();
    return true;
}

void HttpUploadSource::Close() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_part_fd >= 0) {
        close(m_part_fd);
        m_part_fd = -1;
    }
    m_open = false;
    m_has_want = false;
    m_slot.clear();
}

bool HttpUploadSource::IsOpen() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_open;
}

bool HttpUploadSource::Want(std::uint64_t *offset, std::size_t *size) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_has_want || !m_open) {
        return false;
    }
    *offset = m_want_offset;
    *size = m_want_size;
    return true;
}

void HttpUploadSource::SetPump(std::function<void()> pump) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_pump = std::move(pump);
}

void HttpUploadSource::SetRequestsProbe(std::function<int()> probe) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_requests_probe = std::move(probe);
}

void HttpUploadSource::Service() {
    std::function<void()> pump;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        pump = m_pump;
    }
    if (pump) {
        pump();
    }
}

void HttpUploadSource::Abort(const std::string &reason) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_abort_reason.empty()) {
            m_abort_reason = reason;
        }
        if (m_part_fd >= 0) {
            close(m_part_fd);
            m_part_fd = -1;
        }
    }
    m_cv.notify_all();
}

std::string HttpUploadSource::AbortReason() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_abort_reason;
}

std::string HttpUploadSource::AbortCode() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_abort_code;
}

void HttpUploadSource::AttachPart(int fd, std::uint64_t offset, std::size_t length,
                                  std::string prefix) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        /* A part nobody asked for: the server refuses those before they get here (409), so this is
           the belt to that pair of braces -- a part attached with no reader would sit on the socket
           until the browser gave up, and the response it waits for would never come. */
        if (!m_has_want) {
            close(fd);
            return;
        }
        if (m_part_fd >= 0) {
            /* One part at a time: a second one means a page that did not wait for the answer. */
            close(fd);
            return;
        }
        if (prefix.size() > length) {
            close(fd);
            return;
        }
        m_part_fd = fd;
        m_part_offset = offset;
        m_part_length = length;
        m_part_prefix = std::move(prefix);
    }
    m_cv.notify_all();
}

/* Reads the posted body into the slot and answers the browser.  The connection is ours from the
   hand-off onwards (the server has forgotten it), so the response -- including its length -- is
   written here. */
bool HttpUploadSource::ServiceAttachedPart() {
    int fd = -1;
    std::uint64_t offset = 0;
    std::size_t length = 0;
    std::size_t from_server = 0;
    std::vector<std::uint8_t> body;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_part_fd < 0) {
            return false;
        }
        fd = m_part_fd;
        offset = m_part_offset;
        length = m_part_length;
        m_part_fd = -1;
        /* The bytes the server had already read travel with the hand-off; the rest is still on the
           socket.  `from_server` is counted here because the prefix is cleared right after. */
        body.assign(m_part_prefix.begin(), m_part_prefix.end());
        from_server = body.size();
        m_part_prefix.clear();
    }
    body.resize(length);
    /* `resize` keeps the prefix at the front and leaves zeros after it, so the reader starts at the
       prefix's end.  Counting what the vector holds *after* the resize made every part look complete
       with zeros in it -- the log said "first=00000000", and the container parse answered "unknown
       container". */
    std::size_t got = from_server > length ? length : from_server;
    while (got < length) {
        const ssize_t chunk = recv(fd, body.data() + got, length - got, 0);
        if (chunk > 0) {
            got += static_cast<std::size_t>(chunk);
            continue;
        }
        if (chunk == 0) {
            break; /* the browser closed early */
        }
        /* A short pause rather than a busy loop: the body arrives in TCP segments. */
        svcSleepThread(2000000ull); /* 2 ms */
    }
    char answer[96];
    std::string response_body;
    int status = 200;
    if (got < length) {
        status = 400;
        response_body = "short body\n";
    } else {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_slot_offset = offset;
            m_slot.swap(body);
        }
        std::snprintf(answer, sizeof(answer), "{\"offset\":%llu}",
                      static_cast<unsigned long long>(offset + got));
        response_body = answer;
    }
    const std::string head =
        net::BuildResponseHead(status, status == 200 ? "OK" : "Bad Request",
                               "application/json; charset=utf-8", response_body.size());
    std::string out = head + response_body;
    std::size_t sent = 0;
    while (sent < out.size()) {
        const ssize_t chunk = send(fd, out.data() + sent, out.size() - sent, 0);
        if (chunk > 0) {
            sent += static_cast<std::size_t>(chunk);
            continue;
        }
        if (chunk < 0) {
            svcSleepThread(2000000ull);
        }
    }
    close(fd);
    m_cv.notify_all();
    return status == 200;
}

bool HttpUploadSource::Read(std::uint64_t offset, std::size_t size, std::uint8_t *out) {
    if (size == 0) {
        return true;
    }
    std::unique_lock<std::mutex> lock(m_mutex);
    if (!m_open || !m_abort_reason.empty()) {
        return false;
    }
    if (offset + size > m_size) {
        m_abort_reason = "the browser's file is shorter than the package needs";
        return false;
    }
    /* Serve the page once per read, not only while this thread happens to be waiting.  Reading a
       package is not one long wait: between two parts the engine parses and decompresses -- and
       during that stretch nothing called the server at all, so a browser's status polls piled up
       until its six connections per host were gone and the next part could not be sent (measured
       2026-10-16: Chrome's network panel showed the parts stopping and every poll after it
       pending, while the console sat waiting for a part the page could no longer post). */
    {
        const auto pump = m_pump;
        lock.unlock();
        if (pump) {
            pump();
        }
        lock.lock();
        if (!m_open || !m_abort_reason.empty()) {
            return false;
        }
    }
    std::uint64_t silence_begin = NowMs();
    std::uint64_t heartbeat = silence_begin;
    int served_at_wait = m_requests_probe ? m_requests_probe() : -1;
    /* The UI uses this to draw a countdown once the silence is long enough to be worth showing.
       Normal parts arrive in well under a second, so the first two seconds stay quiet. */
    m_page_waiting = true;
    m_page_wait_started_ms = silence_begin;
    m_page_wait_deadline_ms = silence_begin + kSilenceTimeoutMs;
    for (;;) {
        /* The part that arrived may cover this read (the transfer asks in 4 MiB ranges and so does
           the page, so in practice it covers it exactly). */
        if (!m_slot.empty() && m_slot_offset <= offset && offset + size <= m_slot_offset + m_slot.size()) {
            std::memcpy(out, m_slot.data() + (offset - m_slot_offset), size);
            /* The range has been consumed: nothing is wanted until the next read asks for something.
               Leaving the want in place makes `/upload/next` keep offering a range nobody reads --
               and the page then posts it into a source with no reader, which waits forever (measured
               on the confirm screen, where the package has been read and the transfer has not
               started). */
            m_has_want = false;
            m_page_waiting = false;
            return true;
        }
        if (!m_abort_reason.empty()) {
            m_page_waiting = false;
            return false;
        }
        /* Ask for this range: `GET /upload/next` hands it to the page. */
        const std::uint64_t want_before = m_want_offset;
        const std::size_t size_before = m_want_size;
        m_want_offset = offset;
        m_want_size = size > kUploadPartBytes ? kUploadPartBytes : size;
        if (!m_has_want || m_want_offset != want_before || m_want_size != size_before) {
            Trace("upload: waiting for %llu+%zu", static_cast<unsigned long long>(m_want_offset),
                  m_want_size);
        }
        m_has_want = true;
        const auto pump = m_pump;
        lock.unlock();
        if (pump) {
            pump();
        }
        if (ServiceAttachedPart()) {
            lock.lock();
            continue;
        }
        lock.lock();
        m_cv.wait_for(lock, std::chrono::milliseconds(kPollMs));
        /* Every couple of seconds of silence, one live line: the range being waited for, and how many
           page requests the console answered in the meantime.  That second number is what tells the
           two possible stories apart -- "the browser stopped sending" and "the browser is asking and
           getting nothing" -- and it has to be live (`Block`), because inside the engine's burst
           every other line waits for the answer to this question. */
        if (NowMs() - heartbeat > 2000) {
            heartbeat = NowMs();
            const int served = m_requests_probe ? m_requests_probe() - served_at_wait : -1;
            Trace("upload: still waiting for %llu+%zu (%d page requests served meanwhile)",
                  static_cast<unsigned long long>(m_want_offset), m_want_size, served);
        }
        if (NowMs() - silence_begin > kSilenceTimeoutMs) {
            m_abort_reason = "the browser stopped sending (" + util::FormatSize(offset) + " in)";
            m_abort_code = kErrorUploadStopped;
            m_page_waiting = false;
            Trace("upload: %s", m_abort_reason.c_str());
            return false;
        }
    }
}

bool HttpUploadSource::PageWaitSeconds(int *seconds_left) const {
    if (seconds_left == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_page_waiting || !m_has_want) {
        return false;
    }
    const std::uint64_t now = NowMs();
    if (now < m_page_wait_started_ms + 2000) {
        return false; /* normal traffic: do not flash a countdown between parts */
    }
    if (now >= m_page_wait_deadline_ms) {
        *seconds_left = 0;
        return true;
    }
    *seconds_left = static_cast<int>((m_page_wait_deadline_ms - now + 999) / 1000);
    return true;
}

}  // namespace acnh_manager::install
