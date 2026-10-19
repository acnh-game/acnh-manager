#include "net/http_server.hpp"

#include <cstdarg>
#include <cstdlib>
#include <cstdio>
#include <cstring>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include "log.hpp"
#include "net/http_page.hpp"
#include "net/socket_session.hpp"
#include "third_party/picohttpparser/picohttpparser.h"

namespace acnh_manager::net {
namespace {

/* A request that has not arrived in full within this long is not coming: browsers open speculative
   connections and drop them, and a phone that walked out of range leaves one hanging. */
constexpr std::uint64_t kRequestTimeoutMs = 10 * 1000;
/* A connection whose answer is half-written gets this long before it is dropped. */
constexpr std::uint64_t kWriteTimeoutMs = 10 * 1000;

std::uint64_t NowMs() {
    return armTicksToNs(armGetSystemTick()) / 1000000ull;
}

/* "a.b.c.d:port" of the other end, for the log: which device is talking is the first thing worth
   knowing when a phone cannot open the page. */
std::string PeerName(const sockaddr_in &address) {
    char ip[INET_ADDRSTRLEN] = {};
    if (inet_ntop(AF_INET, &address.sin_addr, ip, sizeof(ip)) == nullptr) {
        return "?";
    }
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%s:%u", ip, static_cast<unsigned>(ntohs(address.sin_port)));
    return buffer;
}

/* Reason phrases for the status codes this server can produce.  Anything else answers "OK", which
   browsers ignore anyway -- the number is what matters. */
const char *StatusText(int status) {
    switch (status) {
        case 200: return "OK";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 413: return "Payload Too Large";
        case 501: return "Not Implemented";
        default: return "OK";
    }
}

std::string ToLower(std::string text) {
    for (char &c : text) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return text;
}

/* The value of one header, case-insensitively, or "" when it is not there. */
std::string HeaderValue(std::string_view name, const std::vector<std::string> &names,
                        const std::vector<std::string> &values) {
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (ToLower(names[i]) == name) {
            return values[i];
        }
    }
    return std::string();
}

}  // namespace

HttpServer::HttpServer() {
    m_connections.resize(kHttpMaxConnections);
}

HttpServer::~HttpServer() {
    Stop();
}

void HttpServer::SetHandler(Handler handler) {
    m_handler = std::move(handler);
}

void HttpServer::Trace(const char *format, ...) {
    if (m_log == nullptr) {
        return;
    }
    char line[256];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line) - 24, format, args);
    va_end(args);
    /* Every line carries the monotonic clock, so a log read after the fact still shows *when* a
       connection arrived and when it was answered.  Without it, "the page seemed slow" cannot be
       split into "the browser sent it late" and "the console answered it late" (2026-10-16). */
    char stamped[sizeof(line) + 24];
    std::snprintf(stamped, sizeof(stamped), "[t=%llums] %s",
                  static_cast<unsigned long long>(NowMs()), line);
    m_log->Line("%s", stamped);
}

bool HttpServer::Start(std::uint16_t preferred_port, std::uint16_t *out_port, std::string *error) {
    Stop();
    /* The BSD service is shared with libcurl (net::SocketSession); holding one for as long as the
       server lives is exactly what its reference counting is for. */
    auto *session = new SocketSession();
    if (!session->Ok()) {
        const Result rc = session->Error();
        delete session;
        if (error != nullptr) {
            char buffer[64];
            std::snprintf(buffer, sizeof(buffer), "socketInitializeDefault rc=0x%08X", rc);
            *error = buffer;
        }
        return false;
    }
    m_socket_session = session;

    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        if (error != nullptr) {
            *error = "socket() failed";
        }
        Stop();
        return false;
    }
    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(preferred_port);
    bool bound = bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0;
    if (!bound && preferred_port != 0) {
        /* The port the design fixes is taken by something else on this console: take any free one
           instead of failing.  The address on screen is built from the port we really got. */
        Trace("http: port %u is taken, asking the kernel for another one",
              static_cast<unsigned>(preferred_port));
        address.sin_port = 0;
        bound = bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0;
    }
    if (!bound) {
        if (error != nullptr) {
            *error = "bind() failed";
        }
        close(fd);
        Stop();
        return false;
    }
    socklen_t length = sizeof(address);
    if (getsockname(fd, reinterpret_cast<sockaddr *>(&address), &length) != 0) {
        if (error != nullptr) {
            *error = "getsockname() failed";
        }
        close(fd);
        Stop();
        return false;
    }
    if (listen(fd, kHttpMaxConnections) != 0) {
        if (error != nullptr) {
            *error = "listen() failed";
        }
        close(fd);
        Stop();
        return false;
    }
    /* Non-blocking: Poll() runs inside the frame loop and must never wait for anything. */
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    m_listen_fd = fd;
    m_port = ntohs(address.sin_port);
    m_requests = 0;
    if (out_port != nullptr) {
        *out_port = m_port;
    }
    Trace("http: listening on port %u", static_cast<unsigned>(m_port));
    return true;
}

void HttpServer::Stop() {
    for (Connection &connection : m_connections) {
        if (connection.fd >= 0) {
            close(connection.fd);
            connection.fd = -1;
        }
        connection.in.clear();
        connection.out.clear();
        connection.written = 0;
        connection.drain = 0;
        connection.peer.clear();
    }
    if (m_listen_fd >= 0) {
        close(m_listen_fd);
        m_listen_fd = -1;
    }
    m_port = 0;
    if (m_socket_session != nullptr) {
        delete static_cast<SocketSession *>(m_socket_session);
        m_socket_session = nullptr;
    }
}

void HttpServer::Close(Connection &connection) {
    if (connection.fd >= 0) {
        close(connection.fd);
    }
    connection.fd = -1;
    connection.in.clear();
    connection.out.clear();
    connection.written = 0;
    connection.drain = 0;
    connection.peer.clear();
}

void HttpServer::DrainBody(Connection &connection) {
    char scratch[4096];
    while (connection.drain > 0) {
        const ssize_t got = recv(connection.fd, scratch, sizeof(scratch), 0);
        if (got > 0) {
            const std::size_t taken = static_cast<std::size_t>(got);
            connection.drain -= taken > connection.drain ? connection.drain : taken;
            continue;
        }
        if (got == 0) {
            /* The browser gave up mid-body: nothing left to swallow, and the answer below will
               fail on its own if the other end is gone. */
            connection.drain = 0;
            return;
        }
        return; /* EAGAIN: the rest arrives next frame */
    }
}

void HttpServer::AcceptNew() {
    for (;;) {
        sockaddr_in peer{};
        socklen_t length = sizeof(peer);
        const int fd = accept(m_listen_fd, reinterpret_cast<sockaddr *>(&peer), &length);
        if (fd < 0) {
            return; /* nothing waiting (or a transient error): Poll() tries again next frame */
        }
        Connection *slot = nullptr;
        for (Connection &connection : m_connections) {
            if (connection.fd < 0) {
                slot = &connection;
                break;
            }
        }
        if (slot == nullptr) {
            /* Every slot is busy: answer nothing and close -- a browser treats that as a failed
               request and retries, which is better than queueing behind a stuck transfer. */
            Trace("http: no free connection slot, refusing a request");
            close(fd);
            continue;
        }
        int nodelay = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
        slot->fd = fd;
        slot->peer = PeerName(peer);
        slot->in.clear();
        slot->out.clear();
        slot->written = 0;
        slot->drain = 0;
        slot->deadline_ms = NowMs() + kRequestTimeoutMs;
        Trace("http: connection from %s", slot->peer.c_str());
    }
}

void HttpServer::ReadFrom(Connection &connection) {
    char buffer[4096];
    for (;;) {
        const ssize_t got = recv(connection.fd, buffer, sizeof(buffer), 0);
        if (got > 0) {
            connection.in.append(buffer, static_cast<std::size_t>(got));
            /* A request line plus headers that never ends is not a request: refuse it before the
               buffer becomes the reason the console runs out of memory. */
            if (connection.in.size() > 64 * 1024) {
                Trace("http: request from %s is too large, closing", connection.peer.c_str());
                Close(connection);
                return;
            }
            continue;
        }
        if (got == 0) {
            /* The browser closed its side.  Anything unparsed is dropped; a finished answer is
               already on its way out. */
            if (connection.out.empty()) {
                Close(connection);
            }
            return;
        }
        return; /* EAGAIN: nothing more for now */
    }
}

bool HttpServer::TryRequest(Connection &connection) {
    const char *method = nullptr;
    std::size_t method_length = 0;
    const char *path = nullptr;
    std::size_t path_length = 0;
    int minor_version = 0;
    struct phr_header headers[32];
    std::size_t header_count = 32;
    const int parsed = phr_parse_request(connection.in.data(), connection.in.size(), &method,
                                         &method_length, &path, &path_length, &minor_version,
                                         headers, &header_count, 0);
    if (parsed == -2) {
        return false; /* not a whole request yet */
    }
    HttpRequest request;
    HttpResponse response;
    /* Where the body starts in `connection.in`: the reply code below needs it when the body is
       handed over to somebody else (see HttpResponse::handoff). */
    std::size_t body_start = parsed > 0 ? static_cast<std::size_t>(parsed) : 0;
    if (parsed < 0) {
        response.status = 400;
        response.body = "bad request\n";
        Trace("http: unparseable request from %s", connection.peer.c_str());
    } else {
        request.method.assign(method, method_length);
        const std::string target(path, path_length);
        const std::size_t question = target.find('?');
        request.path = question == std::string::npos ? target : target.substr(0, question);
        request.query = question == std::string::npos ? std::string() : target.substr(question + 1);

        std::vector<std::string> names;
        std::vector<std::string> values;
        for (std::size_t i = 0; i < header_count; ++i) {
            names.emplace_back(headers[i].name, headers[i].name_len);
            values.emplace_back(headers[i].value, headers[i].value_len);
        }
        const std::string length_text = HeaderValue("content-length", names, values);
        std::size_t body_length = 0;
        if (!length_text.empty()) {
            body_length = static_cast<std::size_t>(std::strtoull(length_text.c_str(), nullptr, 10));
        }
        request.body_length = body_length;
        if (body_length > kHttpSmallBodyLimit) {
            /* Too big to buffer.  A route that can consume it as a stream takes the connection over
               (the upload part); anything else is refused *before* the bytes arrive, which is what
               keeps a stray multi-gigabyte POST from filling the console's memory. */
            if (m_handler) {
                response = m_handler(request);
            } else {
                response.status = 501;
                response.body = "no handler\n";
            }
            ++m_requests;
            Trace("http: %s %s from %s (%zu-byte body) -> %d", request.method.c_str(),
                  request.path.c_str(), connection.peer.c_str(), body_length, response.status);
            if (!response.handoff) {
                /* The answer is a refusal of a body that is still coming: swallow what is left of
                   it before writing, or the close below becomes an RST and the browser never sees
                   this answer (see DrainBytes). */
                connection.drain = DrainBytes(body_start, connection.in.size(), body_length);
                response.status = 413;
                response.content_type = "text/plain; charset=utf-8";
                response.body = "body too large\n";
            }
        } else if (connection.in.size() < body_start + body_length) {
            return false; /* body still arriving */
        } else {
            request.body = connection.in.substr(body_start, body_length);
            if (m_handler) {
                response = m_handler(request);
            } else {
                response.status = 501;
                response.body = "no handler\n";
            }
            ++m_requests;
            Trace("http: %s %s from %s -> %d", request.method.c_str(), request.path.c_str(),
                  connection.peer.c_str(), response.status);
        }
    }

    if (response.handoff) {
        /* The handler wants the body for somebody else: the connection goes over with the bytes
           already read (this must happen *before* the buffer is cleared for the reply, which is why
           it sits here and not down with the response framing).  The head is not sent -- the new
           owner writes its own answer once it has the bytes. */
        if (m_handoff.fd >= 0) {
            /* Two hand-offs before the first was taken: nobody owns the earlier connection any
               more, and a connection nobody answers and nobody closes sits in the browser's pool
               for ever -- six of those per host and nothing else can be sent (2026-10-16). */
            Trace("http: a handed-over connection was never taken; closing it");
            close(m_handoff.fd);
            m_handoff.fd = -1;
            m_handoff.prefix.clear();
        }
        m_handoff.fd = connection.fd;
        m_handoff.offset = response.handoff_offset;
        m_handoff.length = response.handoff_length;
        if (body_start <= connection.in.size()) {
            m_handoff.prefix = connection.in.substr(body_start);
            if (m_handoff.prefix.size() > m_handoff.length) {
                m_handoff.prefix.resize(m_handoff.length);
            }
        } else {
            m_handoff.prefix.clear();
        }
        connection.fd = -1;
        connection.in.clear();
        connection.out.clear();
        Trace("http: handing the request body (%zu bytes, %zu already read) to the installer",
              response.handoff_length, m_handoff.prefix.size());
        return true;
    }

    char head[256];
    const int head_length = std::snprintf(
        head, sizeof(head),
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n"
        /* The same "do not keep any of this" the page's own head carries (see BuildResponseHead):
           the page, the status and the cover all change under the client's feet. */
        "Cache-Control: no-store, no-cache, must-revalidate\r\nPragma: no-cache\r\nExpires: 0\r\n",
        response.status, StatusText(response.status), response.content_type.c_str(),
        response.body.size());
    connection.out.assign(head, static_cast<std::size_t>(head_length));
    for (const std::string &header : response.headers) {
        connection.out += header;
        connection.out += "\r\n";
    }
    connection.out += "\r\n";
    connection.out += response.body;
    connection.written = 0;
    connection.in.clear();
    connection.deadline_ms = NowMs() + kWriteTimeoutMs;
    return true;
}

void HttpServer::Flush(Connection &connection) {
    while (connection.written < connection.out.size()) {
        const ssize_t sent = send(connection.fd, connection.out.data() + connection.written,
                                  connection.out.size() - connection.written, 0);
        if (sent > 0) {
            connection.written += static_cast<std::size_t>(sent);
            continue;
        }
        if (sent < 0) {
            return; /* the socket buffer is full: the rest goes out next frame */
        }
        Close(connection);
        return;
    }
    /* One request per connection (see the header): the answer is complete, so it can go. */
    Close(connection);
}

void HttpServer::Poll() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_listen_fd < 0) {
        return;
    }
    AcceptNew();
    const std::uint64_t now = NowMs();
    for (Connection &connection : m_connections) {
        if (connection.fd < 0) {
            continue;
        }
        if (connection.drain > 0) {
            /* The answer is ready, but the request body is not: swallow the rest before writing it
               (see DrainBytes).  Closing first would reset the connection and the browser would
               report a network error instead of the 409 it is supposed to act on. */
            DrainBody(connection);
            if (connection.fd < 0) {
                continue;
            }
            if (connection.drain > 0) {
                if (now > connection.deadline_ms) {
                    Trace("http: %s never finished sending its body, closing", connection.peer.c_str());
                    Close(connection);
                }
                continue;
            }
        }
        if (!connection.out.empty()) {
            Flush(connection);
            continue;
        }
        if (now > connection.deadline_ms) {
            Trace("http: %s went quiet, closing", connection.peer.c_str());
            Close(connection);
            continue;
        }
        ReadFrom(connection);
        if (connection.fd < 0) {
            continue;
        }
        if (TryRequest(connection)) {
            Flush(connection);
        }
    }
}

bool HttpServer::TakeHandoff(HttpHandoff *out) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_handoff.fd < 0) {
        return false;
    }
    *out = m_handoff;
    m_handoff.fd = -1;
    m_handoff.offset = 0;
    m_handoff.length = 0;
    m_handoff.prefix.clear();
    return true;
}

}  // namespace acnh_manager::net
