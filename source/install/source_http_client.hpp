#pragma once

/* A package that is downloaded while it is installed: the source the engine reads container bytes
   from, with the address coming from the player instead of the card.

   Two rules from the design are built into Open():

     * **the address has to answer Range requests**, proved with a one-byte probe before anything
       else happens.  That answer is also the only trustworthy statement about the container's size,
       and resume -- the reason the whole install is written around offsets -- depends on it.  An
       address that ignores Range (and answers 200 with the whole file) is refused, because the
       engine would then have to read the file from the beginning for every range.
     * **nothing is downloaded twice**: every read is one ranged request for exactly the bytes asked
       for, so a cancel, a lost connection or a resumed install continues where it stopped.

   Not a pure module: it is libcurl, so it belongs to the device side of the installer. */

#include <cstdint>
#include <memory>
#include <string>

#include "install/container.hpp"
#include "net/socket_session.hpp"

namespace acnh_manager::install {

/* The response headers libcurl hands back, kept alive for as long as the curl handle holds the
   pointer we give it (`CURLOPT_HEADERDATA`) -- that is the whole download, not just the call that
   set the option.  A stack local in `Open()` was the console crash of 2026-10-15: the *next*
   request (the first ranged read of the transfer) had libcurl write `Content-Range` into a dead
   frame, and Atmosphere's crash report shows exactly that -- a data abort inside
   `Curl_http_readwrite_headers`, with the header text still in the registers. */
struct HttpSourceHeaders {
    std::string content_range;
};

class HttpSource final : public ContainerReader {
public:
    HttpSource();
    ~HttpSource() override;

    /* Probes `url` and fills in the container's size.  `error` is one of the two codes the screens
       have wording for ("no-network", "range-unsupported") or a sentence for the log. */
    bool Open(const std::string &url, std::uint64_t *size, std::string *error);
    void Close();

    /* ContainerReader: one ranged request per call (the engine reads a few MB at a time, in order). */
    bool Read(std::uint64_t offset, std::size_t size, std::uint8_t *out) override;

    const std::string &Url() const { return m_url; }
    bool Open() const { return m_open; }

private:
    std::string m_url;
    std::uint64_t m_size{0};
    bool m_open{false};
    void *m_curl{nullptr}; /* CURL*, kept alive so the connection is reused across reads */
    /* Ours, not a local in Open(): libcurl keeps the pointer we hand it (see the struct's note). */
    HttpSourceHeaders m_headers;
    /* The console's socket service, held from the probe until Close().  libcurl's Switch port never
       brings it up itself, and without it every connection fails with "couldn't connect" -- which is
       what this source did on hardware before this existed (see net/socket_session.hpp). */
    std::unique_ptr<net::SocketSession> m_sockets;
};

}  // namespace acnh_manager::install
