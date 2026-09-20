#pragma once

/* The update check on a worker thread.

   The first version ran `net::CheckForUpdate()` straight from the frame loop, so the whole app
   froze for up to the 8 s timeout: no redraw, no input, not even "press B to leave".  The check
   therefore lives on its own thread -- one long-lived worker that waits for a request -- and the
   UI polls it once per frame with `Take()`.  Everything the UI shows while a check is in flight
   is drawn from the state it polls, so nothing else on screen stops working.

   One instance at a time, deliberately: the thread stack is a file-scope buffer owned by the
   implementation, and `Start()` is a no-op while a check is already running. */

#include <switch.h>

#include <string>

#include "net/update.hpp"

namespace acnh_manager {
class Log; /* log.hpp */
}

namespace acnh_manager::net {

class UpdateCheckTask {
public:
    ~UpdateCheckTask();

    /* Used for the one case that must not be silent: a worker still busy when the app leaves. */
    void SetLog(Log *log) { m_log = log; }

    /* No-op when a check is already in flight.  Returns an empty string when the request was
       accepted, or a short English diagnostic (with the kernel/program result code) when no
       worker could be started -- the caller reports that the same way it reports a failed
       check, so a broken worker never looks like a button that does nothing. */
    std::string Start(const std::string &url, long timeout_seconds = 8);
    bool Running();

    /* Moves a finished result out and clears it; false when nothing new has arrived. */
    bool Take(UpdateCheckResult *out);

    /* Called on the way out: tells the worker to finish after the current transfer and waits a
       bounded time for it, so an in-flight curl is not still running while the applet tears
       down.  A worker that is still inside curl is left to the kernel (its stack is static). */
    void Stop();

private:
    static void Entry(void *self);
    bool JobPending(std::string *url, long *timeout);
    bool QuitRequested();

    Thread m_thread{};
    bool m_created{false};
    bool m_running{false};
    bool m_request{false};
    bool m_quit{false};
    bool m_have_result{false};
    UpdateCheckResult m_result{};
    std::string m_url{};
    long m_timeout{8};
    Mutex m_lock{};
    bool m_lock_ready{false};
    Log *m_log{nullptr};
};

}  // namespace acnh_manager::net
