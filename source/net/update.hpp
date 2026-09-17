#pragma once

#include <string>


/* Update check: fetch the release manifest over HTTPS.

   Principles:
   - check only: a failure never changes an install decision -- we fall back to the embedded
     or local manifest and record why;
   - certificate verification is mandatory: a CA bundle has to be provided (default
     `/switch/ACNH-Manager/ca.pem`, or embedded); without one we skip and say so -- there is
     no "disable verification" fallback;
   - short timeouts (connect 5s, total 8s) and silent failure. */

namespace acnh_manager::net {

struct UpdateCheckResult {
    bool attempted{false};
    bool ok{false};
    bool skipped{false};
    long http_code{0};
    std::string reason;        /* diagnostic text (log + settings page) */
    std::string manifest_text; /* manifest body on success */
};

UpdateCheckResult CheckForUpdate(const std::string &url, const std::string &ca_path,
                                 long timeout_seconds = 8);

/* Default manifest URL (hosted by the guide site, the same file the store package uses). */
inline constexpr const char *kDefaultManifestUrl =
    "https://lextuo.com/acnh-chat-code/guide/agent-manifest.json";
inline constexpr const char *kDefaultCaPath = "/switch/ACNH-Manager/ca.pem";

}  // namespace acnh_manager::net
