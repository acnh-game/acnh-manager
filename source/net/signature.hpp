#pragma once

/* Offline authenticity for anything the app downloads.

   TLS verification is off (see `net/http.hpp`), so the *content* has to prove itself: the
   release manifest is published together with an ECDSA P-256 signature, made with a key that
   only the project holds, and this app carries the matching public key in its own .rodata.  A
   manifest that does not verify is treated as garbage -- the update check reports it and the
   download path refuses to install anything from it.

   Signature format: the DER encoding that `openssl dgst -sha256 -sign` produces, as published
   next to the manifest (`agent-manifest.json.sig`). */

#include <string>

namespace acnh_manager::net {

/* `public_key_pem` is a PEM "BEGIN PUBLIC KEY" block (P-256).  Returns false and fills `error`
   with a short reason when the key cannot be parsed or the signature does not match. */
bool VerifySignature(const std::string &data, const std::string &signature_der,
                     const char *public_key_pem, std::string *error);

}  // namespace acnh_manager::net
