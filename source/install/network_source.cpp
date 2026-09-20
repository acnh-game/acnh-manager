#include "network_source.hpp"

#include "net/http.hpp"

namespace acnh_manager::install {
namespace {

/* A payload file is small (the agent is ~75 KiB today); the cap keeps a hostile or broken
   server from filling memory before the hash check ever runs. */
constexpr std::size_t kPayloadMaxBytes = 4 * 1024 * 1024;

}  // namespace

NetworkPayloadSource::NetworkPayloadSource(const manifest::Manifest &manifest,
                                           const manifest::GameEntry &game) {
    const std::string &base = manifest.base_url;
    for (const manifest::FileEntry &file : game.files) {
        std::string url = base;
        if (!url.empty() && url.back() != '/') {
            url += "/";
        }
        url += file.source;
        m_urls.emplace_back(file.source, url);
    }
}

bool NetworkPayloadSource::Read(const std::string &name, std::vector<u8> *out, std::string *error) {
    for (const auto &entry : m_urls) {
        if (entry.first != name) {
            continue;
        }
        const net::HttpResult http = net::Get(entry.second, kPayloadMaxBytes);
        if (http.outcome != net::HttpOutcome::Ok) {
            if (error != nullptr) {
                /* The file name, not the URL: this string ends up on the result card, where a
                   120-character URL pushes the actual reason past the two lines the card draws
                   (the failure showed up as "download https://…" with the cause cut off).  The
                   URL is baseUrl + files[].source from the manifest, so nothing is lost. */
                *error = "download " + entry.first + ": " + http.detail;
            }
            return false;
        }
        if (out != nullptr) {
            out->assign(http.body.begin(), http.body.end());
        }
        return true;
    }
    if (error != nullptr) {
        *error = "no download URL for payload " + name;
    }
    return false;
}

}  // namespace acnh_manager::install
