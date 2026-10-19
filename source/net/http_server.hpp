#pragma once

/* The installer's own HTTP server: the console serves the upload page, answers its status polls and
   takes the two things a browser can give it (a file to upload, an address to download from).

   Deliberately small, and deliberately **single-threaded**: the page is only ever talking while the
   install page is up, and the install itself runs on this thread too -- so the UI loop (and the
   transfer loop between its 4 MiB steps) calls `Poll()`, which accepts, reads, routes and writes
   without ever blocking.  A worker thread would mean a second thing touching the same sockets, and
   the whole point of this design is that there is exactly one.

   What it is not: a web framework.  No keep-alive, no chunked transfer encoding on the way *out*,
   no TLS (the page is handed out over plain http on the local network), no routing table beyond a
   handful of paths.  `Connection: close` on every answer keeps the state machine to one request per
   connection, which is what a browser's `fetch()` does anyway. */

#include <switch.h>

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace acnh_manager {
class Log;
}

namespace acnh_manager::net {

/* One request, as parsed.  `path` is the path only ("/status.json"), `query` what follows '?'. */
struct HttpRequest {
    std::string method;
    std::string path;
    std::string query;
    std::string body; /* the whole body when the route asked for one (see kSmallBodyLimit) */
    /* How long the body is, whether or not it was buffered: a route that takes the connection over
       (see HttpResponse::handoff) needs the number before it has the bytes. */
    std::size_t body_length{0};
};

struct HttpResponse {
    int status{200};
    std::string content_type{"text/plain; charset=utf-8"};
    std::string body;
    /* Extra headers, one per string, each already "Name: value". */
    std::vector<std::string> headers;
    /* Set by a handler that wants the *body* to go somewhere other than this server: the connection
       is handed over instead of answered, and whoever takes it (see TakeHandoff) owns it from there
       -- body and response.  The upload route is the reason: a part can be megabytes, and the
       consumer of those bytes is the installer, which is waiting on another thread. */
    bool handoff{false};
    std::uint64_t handoff_offset{0};
    std::size_t handoff_length{0};
};

/* A body larger than this is refused with 413 rather than buffered: the upload route does not go
   through here (it streams), and nothing else in this server has any business receiving megabytes. */
inline constexpr std::size_t kHttpSmallBodyLimit = 64 * 1024;
/* How many connections the server holds at once (a phone, a laptop, a reload in flight). */
inline constexpr int kHttpMaxConnections = 6;

/* The default port.  Fixed so the address is the same every time; when it is taken, the server
   falls back to a kernel-assigned port and the screens show *that* number (the player never has to
   know which one it got). */
inline constexpr std::uint16_t kHttpDefaultPort = 4321;

/* A connection a handler gave away: `length` bytes of body are still to be read, and the response
   has not been written. */
struct HttpHandoff {
    int fd{-1};
    std::uint64_t offset{0};
    std::size_t length{0};
    /* Body bytes this server had already read into its buffer before the handler asked for the
       connection.  A small part arrives here whole; a big one (over kHttpSmallBodyLimit) arrives as
       an empty prefix and the rest still on the socket.  Whoever takes the handoff gets both. */
    std::string prefix;
};

class HttpServer {
public:
    using Handler = std::function<HttpResponse(const HttpRequest &request)>;

    HttpServer();
    ~HttpServer();
    HttpServer(const HttpServer &) = delete;
    HttpServer &operator=(const HttpServer &) = delete;

    /* Brings up the socket service (shared with libcurl through net::SocketSession), binds
       `preferred_port` -- or whatever the kernel hands out when that one is taken -- and starts
       listening.  `out_port` receives the port actually used.  False with `error` set when there is
       no listening socket at all. */
    bool Start(std::uint16_t preferred_port, std::uint16_t *out_port, std::string *error);
    /* Closes the listening socket and every connection.  Safe to call when not started. */
    void Stop();
    bool Listening() const { return m_listen_fd >= 0; }
    std::uint16_t Port() const { return m_port; }

    /* The route table: one handler for everything.  It gets the request and returns the answer; the
       server only knows how to turn that into bytes. */
    void SetHandler(Handler handler);

    /* One non-blocking pass: accept what is waiting, read what has arrived, answer what is complete,
       and drop connections that went quiet.  Cheap enough for the frame loop -- and safe to call
       from the transfer's fetch thread as well, which is how the page is served while a package is
       being read (see TakeHandoff). */
    void Poll();

    /* The connection a handler handed over, if there is one.  The caller owns the fd from here:
       it reads `length` bytes of body, writes its own response, and closes. */
    bool TakeHandoff(HttpHandoff *out);

    /* Where diagnostics go (one line per event worth knowing: a connection, a route, a refusal). */
    void SetLog(Log *log) { m_log = log; }
    /* How many requests have been answered since Start (the tests and the log use it). */
    int RequestsServed() const { return m_requests; }

private:
    struct Connection {
        int fd{-1};
        std::string in;      /* bytes read so far */
        std::string out;     /* bytes waiting to go out */
        std::size_t written{0};
        /* Body bytes still to be swallowed before the ready answer may go out, when a route refused
           a request without reading its body (see DrainBytes in net/http_page.hpp).  Zero for every
           connection whose answer does not have to wait. */
        std::size_t drain{0};
        std::uint64_t deadline_ms{0};
        std::string peer;
    };

    void AcceptNew();
    void ReadFrom(Connection &connection);
    /* Swallows up to `connection.drain` bytes from the socket without keeping them. */
    void DrainBody(Connection &connection);
    /* Parses `connection.in`; when a whole request is there, answers it and marks the connection
       for closing.  False means "not yet" (or "drop it"). */
    bool TryRequest(Connection &connection);
    void Flush(Connection &connection);
    void Close(Connection &connection);
    void Trace(const char *format, ...) __attribute__((format(printf, 2, 3)));

    int m_listen_fd{-1};
    std::uint16_t m_port{0};
    Handler m_handler;
    Log *m_log{nullptr};
    int m_requests{0};
    std::vector<Connection> m_connections;
    /* Held whenever the connection table or a socket is touched: Poll() runs on the UI thread and on
       the transfer's fetch thread, and both may be serving a page at the same moment. */
    mutable std::mutex m_mutex;
    HttpHandoff m_handoff;
    /* Held for as long as the server is up: libcurl and this server share the BSD socket service. */
    void *m_socket_session{nullptr};
};

}  // namespace acnh_manager::net
