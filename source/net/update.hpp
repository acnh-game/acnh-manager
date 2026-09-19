#pragma once

#include <string>


/* Update check: fetch the release manifest over HTTPS.

   Principles:
   - check only: a failure never changes an install decision -- we fall back to the embedded
     or local manifest and record why;
   - **TLS certificate verification is off**, deliberately, and for the same reason the other
     homebrew on this console does it (Sphaira sets VERIFYPEER/VERIFYHOST to 0 as well): the
     console hands libcurl + mbedTLS no trust store at all.  mbedTLS ships no roots by design,
     libcurl's Switch port cannot reach the console's own CA list (the one behind Atmosphere's
     `ssl` service), and libcurl 7.69 predates `CURLOPT_CAINFO_BLOB`, so a bundle could only be
     supplied as a file we would have to ship and keep current ourselves.  With verification
     impossible to do properly, the check is a **hint about versions, not a channel**: it never
     installs anything, and a spoofed answer can only make the home screen claim a version that
     does not exist.  If this path ever starts downloading or installing content, this decision
     has to be revisited (TLS verification, or a manifest signature we can verify offline) --
     see docs/architecture.md section 9;
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

UpdateCheckResult CheckForUpdate(const std::string &url, long timeout_seconds = 8);

/* Default manifest URL: the release manifest committed to this repository's `main`
   (`agent-manifest.json` at the repo root), served by GitLab's raw endpoint.  Same file the
   release record carries, so the update check can never disagree with what an install uses. */
inline constexpr const char *kDefaultManifestUrl =
    "https://gitlab.com/acnh-game/acnh-manager/-/raw/main/agent-manifest.json";

}  // namespace acnh_manager::net
