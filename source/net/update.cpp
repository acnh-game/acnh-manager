#include "update.hpp"

#include <cstdio>
#include <string>

#include <switch.h>
#include <curl/curl.h>

#include "i18n/strings.hpp"

namespace acnh_manager::net {
namespace {

/* RAII pair for socketInitializeDefault()/socketExit(): every early return below
   still releases the service.  libcurl's Switch port only calls the BSD socket
   layer (libcurl.a references socket/socketpair but never socketInitialize*), so
   bringing the service up is the caller's job -- and it belongs here, next to the
   code that needs it, not in the global environment setup. */
struct SocketGuard {
    bool active{false};

    ~SocketGuard() {
        if (active) {
            socketExit();
        }
    }
};

std::size_t WriteCallback(char *ptr, std::size_t size, std::size_t nmemb, void *userdata) {
    auto *out = static_cast<std::string *>(userdata);
    const std::size_t bytes = size * nmemb;
    if (out->size() + bytes > 512 * 1024) {
        return 0; /* a manifest should never be this big; fail early */
    }
    out->append(ptr, bytes);
    return bytes;
}

bool FileReadable(const std::string &path, std::string *error) {
    FsFileSystem sd{};
    if (R_FAILED(fsOpenSdCardFileSystem(&sd))) {
        if (error != nullptr) {
            *error = i18n::Text(i18n::StringId::UpdateErrMountSd, i18n::Current());
        }
        return false;
    }
    FsFile file{};
    const Result rc = fsFsOpenFile(&sd, path.c_str(), FsOpenMode_Read, &file);
    if (R_SUCCEEDED(rc)) {
        fsFileClose(&file);
    }
    fsFsClose(&sd);
    if (R_FAILED(rc)) {
        if (error != nullptr) {
            *error = i18n::Format(i18n::StringId::UpdateErrNoCaFile, path.c_str());
        }
        return false;
    }
    return true;
}

}  // namespace

UpdateCheckResult CheckForUpdate(const std::string &url, const std::string &ca_path,
                                 long timeout_seconds) {
    UpdateCheckResult result;
    result.attempted = true;

    if (url.rfind("https://", 0) != 0) {
        result.skipped = true;
        result.reason = i18n::Text(i18n::StringId::UpdateErrHttpsOnly, i18n::Current());
        return result;
    }
    std::string ca_error;
    if (ca_path.empty() || !FileReadable(ca_path, &ca_error)) {
        result.skipped = true;
        result.reason = ca_path.empty()
                            ? i18n::Text(i18n::StringId::UpdateErrNoCa, i18n::Current())
                            : ca_error;
        return result;
    }

    SocketGuard sockets;
    const Result rc_socket = socketInitializeDefault();
    if (R_FAILED(rc_socket)) {
        result.skipped = true;
        result.reason = i18n::Format(i18n::StringId::UpdateErrSocket, rc_socket);
        return result;
    }
    sockets.active = true;

    curl_global_init(CURL_GLOBAL_DEFAULT);
    CURL *curl = curl_easy_init();
    if (curl == nullptr) {
        result.reason = i18n::Text(i18n::StringId::UpdateErrCurlInit, i18n::Current());
        return result;
    }
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result.manifest_text);
    curl_easy_setopt(curl, CURLOPT_CAINFO, ca_path.c_str());
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_seconds);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "acnh-manager/0.1.0");
    const CURLcode code = curl_easy_perform(curl);
    if (code == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.http_code);
    }
    curl_easy_cleanup(curl);

    if (code != CURLE_OK) {
        result.reason =
            i18n::Format(i18n::StringId::UpdateErrNetwork, curl_easy_strerror(code));
        result.manifest_text.clear();
        return result;
    }
    if (result.http_code != 200) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "HTTP %ld", result.http_code);
        result.reason = buf;
        result.manifest_text.clear();
        return result;
    }
    result.ok = true;
    result.reason = i18n::Text(i18n::StringId::UpdateOkFetched, i18n::Current());
    return result;
}

}  // namespace acnh_manager::net
