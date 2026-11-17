#pragma once

/* Ed25519 verification for the transitional Manager manifest signature.

   mbedTLS 2.28 (the version shipped by the pinned devkitPro image) has no Ed25519
   verification API, so this small wrapper uses the libsodium already provided by that
   portlibs environment.  The signing domain is part of the bytes passed to the verifier:

       "ACNH-MANAGER-MANIFEST-v1" NUL || exact manifest bytes

   Keeping the domain construction here, next to verification, makes the host test vector
   exercise the exact same bytes that tools/sign-manifest.py signs. */

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace acnh_manager::net {

inline constexpr std::string_view kManifestEd25519Domain = "ACNH-MANAGER-MANIFEST-v1";
inline constexpr std::size_t kEd25519PublicKeySize = 32;
inline constexpr std::size_t kEd25519SignatureSize = 64;

/* Exact signing input for the new manifest signature. */
std::string ManifestEd25519SigningInput(std::string_view manifest);

/* Verify a raw Ed25519 signature.  `signature` must be 64 bytes and `public_key` 32
   bytes.  Returns false and fills `error` with a short diagnostic on failure. */
bool VerifyEd25519(std::string_view data, std::string_view signature,
                   const std::uint8_t public_key[kEd25519PublicKeySize],
                   std::string *error);

}  // namespace acnh_manager::net
