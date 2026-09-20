#pragma once

/* The project's one HTTPS transport: libcurl + mbedTLS on the Switch's BSD socket layer.

   Policy (deliberate, see docs/architecture.md section 9):
   - https only, short timeouts (connect 5 s, total given by the caller), response caps;
   - **no certificate verification**: this stack has no trust store to verify against (mbedTLS
     ships no roots, libcurl's Switch port cannot reach the console's own CA list, and libcurl
     7.69 predates `CURLOPT_CAINFO_BLOB`).  Sphaira and most homebrew do the same.  Anything this
     transport delivers that matters is therefore authenticated *elsewhere* -- today the release
     manifest carries an ECDSA signature that `net::VerifySignature()` checks before we trust a
     single byte of it (see `net/update.*`).
   - failures come back as data (outcome + raw detail), never as exceptions or aborts, because
     every caller has to degrade gracefully and log why. */

#include <cstddef>
#include <string>

namespace acnh_manager::net {

enum class HttpOutcome {
    Ok,         /* HTTP 200 and the body was received */
    NotHttps,   /* refused before touching the network */
    HttpStatus, /* the server answered with something other than 200 */
    Network,    /* socket service, curl init or the transfer itself failed */
};

struct HttpResult {
    HttpOutcome outcome{HttpOutcome::Network};
    long status{0};
    std::string detail; /* raw, English diagnostic text: goes to log.txt and the details page */
    std::string body;
};

HttpResult Get(const std::string &url, std::size_t max_bytes = 512 * 1024,
               long timeout_seconds = 8);

}  // namespace acnh_manager::net
