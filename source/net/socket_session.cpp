#include "net/socket_session.hpp"

#include <mutex>

namespace acnh_manager::net {
namespace {

std::mutex g_mutex;
int g_sessions{0};
/* What the first socketInitializeDefault() answered.  Kept while sessions are alive so the second
   user does not try again (and so a later session can retry once everyone has let go). */
Result g_result{0};

}  // namespace

SocketSession::SocketSession() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_sessions == 0) {
        g_result = socketInitializeDefault();
    }
    if (R_SUCCEEDED(g_result)) {
        ++g_sessions;
        m_ok = true;
        return;
    }
    m_ok = false;
    m_error = g_result;
}

SocketSession::~SocketSession() {
    if (!m_ok) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    if (--g_sessions == 0) {
        socketExit();
        /* The next session starts from scratch: a console that had no network a minute ago may
           have one now, and the result of the failed attempt must not stick. */
        g_result = 0;
    }
}

}  // namespace acnh_manager::net
