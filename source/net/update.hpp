#pragma once

/* The update check: fetch the release manifest, verify it, hand back what it says.

   The check is a *hint and a source of files*, and it is deliberately split from the transport:
   `net::Get()` does the HTTPS with verification off (no trust store exists on this stack),
   `net::VerifySignature()` checks the manifest's ECDSA signature against the public key
   embedded in this build, and only a manifest that passes both is parsed.  A spoofed answer
   therefore cannot introduce a version we would offer to install, let alone files.

   Nothing here touches the UI or blocks on anything the caller did not ask for: the call is
   synchronous, and `net::UpdateCheckTask` runs it on a worker thread so the interface keeps
   drawing and responding while it is in flight.

   Result reporting is structured (outcome + raw diagnostic) on purpose: the *player-facing*
   wording is built by the UI in the current language at render time, so a language switch while
   a result is on screen does not leave a stale sentence behind (and the raw text still reaches
   log.txt and the details page). */

#include <string>

namespace acnh_manager::net {

enum class UpdateOutcome {
    Ok,              /* fetched, signature verified, manifest parsed */
    NotHttps,        /* the URL is not https; refused before the network */
    HttpStatus,      /* the server answered with something other than 200 */
    Network,         /* the request itself failed (DNS, connect, TLS, timeout, ...) */
    NoSignature,     /* the manifest is there but the .sig next to it is not */
    BadSignature,    /* a signature is there but it does not verify against our key */
    NoTrustAnchor,   /* this build carries no public key, so nothing can be verified */
    InvalidManifest, /* verified, but not a manifest this app accepts */
    WorkerFailed,    /* the check never ran: its worker thread could not be started */
};

struct UpdateCheckResult {
    UpdateOutcome outcome{UpdateOutcome::Network};
    long http_code{0};
    std::string detail;        /* raw English diagnostic: log.txt + details page */
    std::string manifest_text; /* only when outcome == Ok */
    std::string agent_version; /* the manifest's agent version, only when outcome == Ok */
};

/* Synchronous: see UpdateCheckTask for the threaded wrapper the UI uses. */
UpdateCheckResult CheckForUpdate(const std::string &url, long timeout_seconds = 8);

/* The same manifest the store package and an install use; see docs/release-process.md 6. */
inline constexpr const char *kDefaultManifestUrl =
    "https://gitee.com/acnh-game/acnh-manager/raw/main/agent-manifest.json";

}  // namespace acnh_manager::net
