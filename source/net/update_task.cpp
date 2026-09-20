#include "update_task.hpp"

#include <cstdio>

#include "log.hpp"

namespace acnh_manager::net {
namespace {

/* Stack for the worker.  File scope for the same reason the task is a single object: the thread
   may outlive `Stop()` in the worst case, so the memory it runs on must not be freed by then.
   128 KiB comfortably fits libcurl plus a TLS handshake.  The alignment is not decoration:
   libnx rejects a caller-provided stack that is not page-aligned with `LibnxError_BadInput`
   (`threadCreate` returns 0x1759 and no thread exists), which is how this failed on hardware
   the first time. */
alignas(0x1000) u8 g_stack[128 * 1024];

constexpr u64 kIdlePollNs = 20000000ull;  /* 20 ms between request checks while idle */
constexpr u64 kStopWaitNs = 1500000000ull; /* how long Stop() waits for a busy worker */

u64 NanosecondsToTicks(u64 ns) {
    return (armGetSystemTickFreq() * ns) / 1000000000ull;
}

}  // namespace

UpdateCheckTask::~UpdateCheckTask() {
    Stop();
}

std::string UpdateCheckTask::Start(const std::string &url, long timeout_seconds) {
    if (!m_lock_ready) {
        mutexInit(&m_lock);
        m_lock_ready = true;
    }
    if (!m_created) {
        const Result rc = threadCreate(&m_thread, &UpdateCheckTask::Entry, this, g_stack,
                                       sizeof(g_stack), 0x30, -2);
        if (R_FAILED(rc)) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "threadCreate rc=0x%08X", rc);
            return buf;
        }
        m_created = true;
        if (R_FAILED(threadStart(&m_thread))) {
            threadClose(&m_thread);
            m_created = false;
            return "threadStart failed";
        }
    }
    mutexLock(&m_lock);
    if (m_running || m_request) {
        mutexUnlock(&m_lock);
        return {}; /* one check at a time */
    }
    m_url = url;
    m_timeout = timeout_seconds;
    m_request = true;
    m_running = true;
    mutexUnlock(&m_lock);
    return {};
}

bool UpdateCheckTask::JobPending(std::string *url, long *timeout) {
    mutexLock(&m_lock);
    const bool pending = m_request;
    if (pending) {
        *url = m_url;
        *timeout = m_timeout;
        m_request = false;
    }
    mutexUnlock(&m_lock);
    return pending;
}

bool UpdateCheckTask::QuitRequested() {
    mutexLock(&m_lock);
    const bool quit = m_quit;
    mutexUnlock(&m_lock);
    return quit;
}

void UpdateCheckTask::Entry(void *self) {
    auto *task = static_cast<UpdateCheckTask *>(self);
    while (!task->QuitRequested()) {
        std::string url;
        long timeout = 8;
        if (!task->JobPending(&url, &timeout)) {
            svcSleepThread(kIdlePollNs);
            continue;
        }
        UpdateCheckResult result = CheckForUpdate(url, timeout);
        mutexLock(&task->m_lock);
        task->m_result = std::move(result);
        task->m_have_result = true;
        task->m_running = false;
        mutexUnlock(&task->m_lock);
    }
}

bool UpdateCheckTask::Running() {
    if (!m_lock_ready) {
        return false;
    }
    mutexLock(&m_lock);
    const bool running = m_running;
    mutexUnlock(&m_lock);
    return running;
}

bool UpdateCheckTask::Take(UpdateCheckResult *out) {
    if (!m_lock_ready || out == nullptr) {
        return false;
    }
    mutexLock(&m_lock);
    const bool ready = m_have_result;
    if (ready) {
        *out = std::move(m_result);
        m_result = UpdateCheckResult{};
        m_have_result = false;
    }
    mutexUnlock(&m_lock);
    return ready;
}

void UpdateCheckTask::Stop() {
    if (!m_created) {
        return;
    }
    mutexLock(&m_lock);
    m_quit = true;
    mutexUnlock(&m_lock);

    const u64 deadline = armGetSystemTick() + NanosecondsToTicks(kStopWaitNs);
    while (Running() && armGetSystemTick() < deadline) {
        svcSleepThread(NanosecondsToTicks(kIdlePollNs));
    }
    if (Running()) {
        if (m_log != nullptr) {
            m_log->Line("update check: worker still busy at exit; leaving it to the kernel");
        }
    } else {
        threadWaitForExit(&m_thread);
    }
    threadClose(&m_thread);
    m_created = false;
}

}  // namespace acnh_manager::net
