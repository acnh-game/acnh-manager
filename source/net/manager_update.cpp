#include "manager_update.hpp"

#include <cstdio>

#include "log.hpp"

namespace acnh_manager::net {
namespace {

alignas(0x1000) u8 g_manager_stack[128 * 1024];

constexpr u64 kIdlePollNs = 20000000ull;
constexpr u64 kStopWaitNs = 1500000000ull;

u64 NanosecondsToTicks(u64 ns) {
    return (armGetSystemTickFreq() * ns) / 1000000000ull;
}

}  // namespace

ManagerDownloadTask::~ManagerDownloadTask() {
    Stop();
}

std::string ManagerDownloadTask::Start(const std::string &url, std::size_t max_bytes,
                                       long timeout_seconds) {
    if (!m_lock_ready) {
        mutexInit(&m_lock);
        m_lock_ready = true;
    }
    if (!m_created) {
        const Result rc = threadCreate(&m_thread, &ManagerDownloadTask::Entry, this,
                                       g_manager_stack, sizeof(g_manager_stack), 0x30, -2);
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
        return {};
    }
    m_url = url;
    m_max_bytes = max_bytes;
    m_timeout = timeout_seconds;
    m_request = true;
    m_running = true;
    mutexUnlock(&m_lock);
    return {};
}

bool ManagerDownloadTask::JobPending(std::string *url, std::size_t *max_bytes, long *timeout) {
    mutexLock(&m_lock);
    const bool pending = m_request;
    if (pending) {
        *url = m_url;
        *max_bytes = m_max_bytes;
        *timeout = m_timeout;
        m_request = false;
    }
    mutexUnlock(&m_lock);
    return pending;
}

bool ManagerDownloadTask::QuitRequested() {
    mutexLock(&m_lock);
    const bool quit = m_quit;
    mutexUnlock(&m_lock);
    return quit;
}

void ManagerDownloadTask::Entry(void *self) {
    auto *task = static_cast<ManagerDownloadTask *>(self);
    while (!task->QuitRequested()) {
        std::string url;
        std::size_t max_bytes = 0;
        long timeout = 0;
        if (!task->JobPending(&url, &max_bytes, &timeout)) {
            svcSleepThread(kIdlePollNs);
            continue;
        }
        HttpResult result = Get(url, max_bytes, timeout);
        mutexLock(&task->m_lock);
        task->m_result = std::move(result);
        task->m_have_result = true;
        task->m_running = false;
        mutexUnlock(&task->m_lock);
    }
}

bool ManagerDownloadTask::Running() {
    if (!m_lock_ready) {
        return false;
    }
    mutexLock(&m_lock);
    const bool running = m_running;
    mutexUnlock(&m_lock);
    return running;
}

bool ManagerDownloadTask::Take(HttpResult *out) {
    if (!m_lock_ready || out == nullptr) {
        return false;
    }
    mutexLock(&m_lock);
    const bool ready = m_have_result;
    if (ready) {
        *out = std::move(m_result);
        m_result = HttpResult{};
        m_have_result = false;
    }
    mutexUnlock(&m_lock);
    return ready;
}

void ManagerDownloadTask::Stop() {
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
            m_log->Line("manager update: worker still busy at exit; leaving it to the kernel");
        }
    } else {
        threadWaitForExit(&m_thread);
    }
    threadClose(&m_thread);
    m_created = false;
}

}  // namespace acnh_manager::net
