#include "net/ed25519.hpp"

#include <sodium.h>
#include <sodium/randombytes.h>

#ifdef __SWITCH__
extern "C" {
#include <switch/kernel/random.h>
}
#endif

namespace acnh_manager::net {
namespace {

bool SodiumReady() {
    static const bool ready = [] {
#ifdef __SWITCH__
        /* libsodium's default sysrandom backend expects the host to provide an OS random
           device.  The applet/hbl environment cannot do that, and sodium_init() otherwise
           aborts from its background worker.  libnx's randomGet() is the console's
           kernel-seeded ChaCha20 source and needs no devoptab or filesystem. */
        static const randombytes_implementation kSwitchRandom = {
            [] { return "libnx-random"; },
            [] {
                std::uint32_t value = 0;
                randomGet(&value, sizeof(value));
                return value;
            },
            [] {},
            nullptr,
            [](void *const buffer, const std::size_t size) { randomGet(buffer, size); },
            nullptr,
        };
        if (randombytes_set_implementation(&kSwitchRandom) != 0) {
            return false;
        }
#endif
        return sodium_init() >= 0;
    }();
    return ready;
}

}  // namespace

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
    if (!SodiumReady()) {
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
