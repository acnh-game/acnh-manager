#include "http.hpp"

#include <cstdio>

#include <switch.h>
#include <curl/curl.h>

namespace acnh_manager::net {
namespace {

/* RAII pair for socketInitializeDefault()/socketExit(): every early return still releases the
   service.  libcurl's Switch port only calls the BSD socket layer (libcurl.a references
   socket/socketpair but never socketInitialize*), so bringing the service up is the caller's
   job -- and it belongs here, next to the code that needs it, not in the global environment
   setup. */
struct SocketGuard {
    bool active{false};

    ~SocketGuard() {
        if (active) {
            socketExit();
        }
    }
};

struct Body {
    std::string text;
    std::size_t max{0};
    bool overflowed{false};
};

std::size_t WriteCallback(char *ptr, std::size_t size, std::size_t nmemb, void *userdata) {
    auto *body = static_cast<Body *>(userdata);
    const std::size_t bytes = size * nmemb;
    if (body->text.size() + bytes > body->max) {
        body->overflowed = true;
        return 0; /* fail the transfer early instead of filling memory */
    }
    body->text.append(ptr, bytes);
    return bytes;
}

std::string Describe(const char *what, Result rc) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%s rc=0x%08X", what, rc);
    return buf;
}

}  // namespace

HttpResult Get(const std::string &url, std::size_t max_bytes, long timeout_seconds) {
    HttpResult result;
    if (url.rfind("https://", 0) != 0) {
        result.outcome = HttpOutcome::NotHttps;
        result.detail = "only https URLs are accepted";
        return result;
    }

    SocketGuard sockets;
    const Result rc_socket = socketInitializeDefault();
    if (R_FAILED(rc_socket)) {
        result.detail = Describe("socketInitializeDefault", rc_socket);
        return result;
    }
    sockets.active = true;

    curl_global_init(CURL_GLOBAL_DEFAULT);
    CURL *curl = curl_easy_init();
    if (curl == nullptr) {
        result.detail = "curl_easy_init failed";
        return result;
    }
    Body body;
    body.max = max_bytes;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    /* No trust store is available to this stack (see the header): verification is off, the same
       way Sphaira and most homebrew do it.  What arrives here is authenticated by the caller
       (the release manifest's ECDSA signature) before it is trusted. */
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_seconds);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "acnh-manager");
    const CURLcode code = curl_easy_perform(curl);
    long status = 0;
    if (code == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    }
    curl_easy_cleanup(curl);

    result.status = status;
    if (code != CURLE_OK) {
        result.outcome = HttpOutcome::Network;
        result.detail = body.overflowed ? "response larger than the accepted limit"
                                        : curl_easy_strerror(code);
        return result;
    }
    if (status != 200) {
        char buf[48];
        std::snprintf(buf, sizeof(buf), "HTTP %ld", status);
        result.outcome = HttpOutcome::HttpStatus;
        result.detail = buf;
        return result;
    }
    result.outcome = HttpOutcome::Ok;
    result.body = std::move(body.text);
    return result;
}

}  // namespace acnh_manager::net
