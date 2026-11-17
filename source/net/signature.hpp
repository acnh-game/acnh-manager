#pragma once

/* Legacy P-256 verification for the manifest authenticity chain.

   TLS verification is off (see `net/http.hpp`), so the release manifest has to prove itself.
   The transition build still carries the old P-256 key and verifies the legacy `.sig` file, so
   an already-shipped Manager can upgrade to this build.  New builds prefer the Ed25519
   signature in `net/ed25519.*`; P-256 remains only for the migration overlap.

   Legacy signature format: the DER encoding that `openssl dgst -sha256 -sign` produces,
   published next to the manifest as `agent-manifest.json.sig`. */

#include <string>

namespace acnh_manager::net {

/* `public_key_pem` is a PEM "BEGIN PUBLIC KEY" block (P-256).  Returns false and fills `error`
   with a short reason when the key cannot be parsed or the signature does not match. */
bool VerifyP256Signature(const std::string &data, const std::string &signature_der,
                         const char *public_key_pem, std::string *error);

}  // namespace acnh_manager::net
