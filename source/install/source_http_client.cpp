#include "install/source_http_client.hpp"

#include <curl/curl.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "install/range_probe.hpp"

namespace acnh_manager::install {
namespace {

/* libcurl hands the body over in pieces; the read asks for a known length, so this stops accepting
   once it has it (a server that ignores the range would otherwise stream gigabytes into memory). */
struct Body {
    std::uint8_t *out{nullptr};
    std::size_t want{0};
    std::size_t got{0};
    bool overflow{false};
};

std::size_t OnBody(char *data, std::size_t size, std::size_t count, void *user) {
    auto *body = static_cast<Body *>(user);
    const std::size_t bytes = size * count;
    const std::size_t room = body->want - body->got;
    const std::size_t take = bytes < room ? bytes : room;
    if (take > 0) {
        std::memcpy(body->out + body->got, data, take);
        body->got += take;
    }
    if (bytes > take) {
        body->overflow = true; /* more than the range asked for: not a range answer */
    }
    return take;
}

std::string Describe(CURLcode code) {
    return std::string("the download failed (") + curl_easy_strerror(code) + ")";
}

/* The response headers, captured so `Content-Range` can be read: libcurl exposes the reply's own
   length through CURLINFO_CONTENT_LENGTH_DOWNLOAD, which for the probe is 1, and the *container's*
   length only lives in that header. */
struct Headers {
    std::string content_range;
};

std::size_t OnHeader(char *data, std::size_t size, std::size_t count, void *user) {
    auto *headers = static_cast<Headers *>(user);
    const std::size_t bytes = size * count;
    std::string line(data, bytes);
    constexpr const char *kKey = "content-range:";
    if (line.size() >= 14) {
        bool match = true;
        for (std::size_t i = 0; i < 14 && match; ++i) {
            const char c = line[i];
            match = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c) == kKey[i];
        }
        if (match) {
            headers->content_range = line.substr(14);
        }
    }
    return bytes;
}

bool GlobalInit(std::string *error) {
    static bool ready = false;
    static std::string problem;
    if (ready) {
        return true;
    }
    const CURLcode code = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (code != CURLE_OK) {
        problem = Describe(code);
        *error = problem;
        return false;
    }
    ready = true;
    return true;
}

}  // namespace

HttpSource::HttpSource() = default;

HttpSource::~HttpSource() { Close(); }

bool HttpSource::Open(const std::string &url, std::uint64_t *size, std::string *error) {
    Close();
    if (!GlobalInit(error)) {
        return false;
    }
    m_curl = curl_easy_init();
    if (m_curl == nullptr) {
        *error = "cannot start the download engine";
        return false;
    }
    auto *curl = static_cast<CURL *>(m_curl);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 512L); /* bytes per second... */
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);   /* ...for 30 seconds = give up */
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "ACNH-Manager");

    /* The probe: one byte, and only a 206 with a Content-Range counts. */
    std::uint8_t byte = 0;
    Body probe{&byte, 1, 0, false};
    Headers headers;
    curl_easy_setopt(curl, CURLOPT_RANGE, "0-0");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, OnBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &probe);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, OnHeader);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &headers);
    CURLcode code = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    /* A reply that never arrived and a reply we refuse get different cards, and curl reports both
       as a failure (a server that ignores the range streams the whole file at us and the write
       callback cutting it off is an error).  So a status we did receive decides first; only when
       there is none does the curl error speak. */
    if (status == 0) {
        /* "no network" and "the address is wrong" are the same failure to curl when it cannot even
           connect: the screens have one line for it, and the log keeps the text. */
        *error = code == CURLE_COULDNT_RESOLVE_HOST || code == CURLE_COULDNT_CONNECT ||
                         code == CURLE_OPERATION_TIMEDOUT
                     ? kProbeErrorNoNetwork
                     : Describe(code);
        Close();
        return false;
    }
    std::uint64_t total = 0;
    switch (JudgeRangeProbe(status, headers.content_range, &total)) {
        case RangeProbe::Supported:
            break;
        case RangeProbe::NotRange:
            *error = kProbeErrorNotRange;
            Close();
            return false;
        case RangeProbe::BadStatus:
            *error = HttpStatusError(status);
            Close();
            return false;
    }
    if (probe.got != 1 || probe.overflow) {
        /* The header promised one byte and it did not arrive (the connection broke mid-answer):
           the downloader's own sentence is the honest one. */
        *error = code == CURLE_OK ? std::string("the download stopped before the first byte")
                                  : Describe(code);
        Close();
        return false;
    }
    m_size = total;
    m_url = url;
    m_open = true;
    *size = m_size;
    return true;
}

void HttpSource::Close() {
    if (m_curl != nullptr) {
        curl_easy_cleanup(static_cast<CURL *>(m_curl));
        m_curl = nullptr;
    }
    m_url.clear();
    m_size = 0;
    m_open = false;
}

bool HttpSource::Read(std::uint64_t offset, std::size_t size, std::uint8_t *out) {
    if (!m_open || m_curl == nullptr) {
        return false;
    }
    if (size == 0) {
        return true;
    }
    auto *curl = static_cast<CURL *>(m_curl);
    char range[64];
    std::snprintf(range, sizeof(range), "%llu-%llu", static_cast<unsigned long long>(offset),
                  static_cast<unsigned long long>(offset + size - 1));
    Body body{out, size, 0, false};
    curl_easy_setopt(curl, CURLOPT_RANGE, range);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, OnBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    const CURLcode code = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    return code == CURLE_OK && status == 206 && body.got == size && !body.overflow;
}

}  // namespace acnh_manager::install
