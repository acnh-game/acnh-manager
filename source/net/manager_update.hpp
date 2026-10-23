#pragma once

/* Background download for ACNH-Manager's self-update NRO.

   The manifest check and this download are separate workers: the first may run at startup and
   the second only after the player explicitly picks the Manager entry in Update Center.  The UI
   polls both, so neither an HTTPS handshake nor a several-megabyte body blocks rendering. */

#include <switch.h>

#include <cstddef>
#include <string>

#include "net/http.hpp"

namespace acnh_manager {
class Log;
}

namespace acnh_manager::net {

class ManagerDownloadTask {
public:
    ~ManagerDownloadTask();

    void SetLog(Log *log) { m_log = log; }

    /* Empty means accepted.  A non-empty string is the reason a worker could not start. */
    std::string Start(const std::string &url, std::size_t max_bytes, long timeout_seconds);
    bool Running();
    bool Take(HttpResult *out);
    void Stop();

private:
    static void Entry(void *self);
    bool JobPending(std::string *url, std::size_t *max_bytes, long *timeout);
    bool QuitRequested();

    Thread m_thread{};
    bool m_created{false};
    bool m_running{false};
    bool m_request{false};
    bool m_quit{false};
    bool m_have_result{false};
    HttpResult m_result{};
    std::string m_url{};
    std::size_t m_max_bytes{0};
    long m_timeout{0};
    Mutex m_lock{};
    bool m_lock_ready{false};
    Log *m_log{nullptr};
};

}  // namespace acnh_manager::net
