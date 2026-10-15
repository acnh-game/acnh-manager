#pragma once

/* The console's BSD socket service, brought up for as long as somebody needs it.

   libcurl's Switch port calls `socket()` / `socketpair()` straight into the BSD layer and never
   initializes the service itself, so **every** user of it has to -- and the two users in this app
   can overlap (an update check while a download runs, or the agent payload being fetched during an
   install).  Opening and closing the service per call would have one of them pull it out from
   under the other, which is why this is reference-counted instead.

   Hold one of these for as long as libcurl is in use: `net::Get` holds it for its request, and the
   installer's download source holds it from the first probe until the transfer ends.

   The failure this exists to prevent, measured on hardware: the download source connected to
   nothing at all ("cannot connect to this address" for every address, LAN or internet) while the
   update check on the same screen worked, because only the update check had brought the service
   up. */

#include <switch.h>

namespace acnh_manager::net {

class SocketSession {
public:
    SocketSession();
    ~SocketSession();
    SocketSession(const SocketSession &) = delete;
    SocketSession &operator=(const SocketSession &) = delete;

    /* False when the service would not come up at all; `error` then carries the result. */
    bool Ok() const { return m_ok; }
    Result Error() const { return m_error; }

private:
    bool m_ok{false};
    Result m_error{0};
};

}  // namespace acnh_manager::net
