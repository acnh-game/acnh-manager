#include "signature.hpp"

#include <cstdio>

#include <mbedtls/md.h>
#include <mbedtls/pk.h>

#include "util/sha256.hpp"

namespace acnh_manager::net {
namespace {

std::string Describe(const char *what, int rc) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%s rc=-0x%04X", what, -rc);
    return buf;
}

}  // namespace

bool VerifySignature(const std::string &data, const std::string &signature_der,
                     const char *public_key_pem, std::string *error) {
    if (public_key_pem == nullptr || public_key_pem[0] == '\0') {
        if (error != nullptr) {
            *error = "no public key is embedded in this build";
        }
        return false;
    }
    if (signature_der.empty()) {
        if (error != nullptr) {
            *error = "the signature is empty";
        }
        return false;
    }

    mbedtls_pk_context key;
    mbedtls_pk_init(&key);
    /* PEM parsing wants the NUL terminator included in the length. */
    int rc = mbedtls_pk_parse_public_key(&key, reinterpret_cast<const unsigned char *>(public_key_pem),
                                         std::char_traits<char>::length(public_key_pem) + 1);
    if (rc != 0) {
        mbedtls_pk_free(&key);
        if (error != nullptr) {
            *error = Describe("mbedtls_pk_parse_public_key", rc);
        }
        return false;
    }

    util::Sha256 hash;
    hash.Update(data.data(), data.size());
    const auto digest = hash.Finish();

    rc = mbedtls_pk_verify(&key, MBEDTLS_MD_SHA256, digest.data(), digest.size(),
                           reinterpret_cast<const unsigned char *>(signature_der.data()),
                           signature_der.size());
    mbedtls_pk_free(&key);
    if (rc != 0) {
        if (error != nullptr) {
            *error = Describe("signature does not match", rc);
        }
        return false;
    }
    return true;
}

}  // namespace acnh_manager::net
