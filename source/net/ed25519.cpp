#include "net/ed25519.hpp"

#include <sodium.h>

namespace acnh_manager::net {

std::string ManifestEd25519SigningInput(std::string_view manifest) {
    std::string input;
    input.reserve(kManifestEd25519Domain.size() + 1 + manifest.size());
    input.append(kManifestEd25519Domain);
    input.push_back('\0');
    input.append(manifest);
    return input;
}

bool VerifyEd25519(std::string_view data, std::string_view signature,
                   const std::uint8_t public_key[kEd25519PublicKeySize],
                   std::string *error) {
    if (public_key == nullptr) {
        if (error != nullptr) {
            *error = "no Ed25519 public key is embedded in this build";
        }
        return false;
    }
    if (signature.size() != kEd25519SignatureSize) {
        if (error != nullptr) {
            *error = "the Ed25519 signature is not 64 bytes";
        }
        return false;
    }
    if (sodium_init() < 0) {
        if (error != nullptr) {
            *error = "libsodium initialization failed";
        }
        return false;
    }
    if (crypto_sign_verify_detached(
            reinterpret_cast<const unsigned char *>(signature.data()),
            reinterpret_cast<const unsigned char *>(data.data()), data.size(), public_key) != 0) {
        if (error != nullptr) {
            *error = "Ed25519 signature does not match";
        }
        return false;
    }
    return true;
}

}  // namespace acnh_manager::net
