#include "update.hpp"

#include <cstdint>
#include <string>

#include "manifest/manifest.hpp"
#include "net/ed25519.hpp"
#include "net/http.hpp"
#include "net/signature.hpp"
#include "payload/embedded.hpp"
#include "version.hpp"

namespace acnh_manager::net {
namespace {

/* Ed25519 is the preferred transition signature.  The legacy suffix remains required so an
   already-shipped P-256-only Manager can upgrade to this build. */
constexpr const char *kEd25519SignatureSuffix = ".ed25519.sig";
constexpr const char *kLegacySignatureSuffix = ".sig";
constexpr std::size_t kManifestMaxBytes = 512 * 1024;
constexpr std::size_t kSignatureMaxBytes = 4 * 1024;

UpdateCheckResult FromHttp(const HttpResult &http) {
    UpdateCheckResult result;
    result.http_code = http.status;
    result.detail = http.detail;
    switch (http.outcome) {
        case HttpOutcome::NotHttps: result.outcome = UpdateOutcome::NotHttps; break;
        case HttpOutcome::HttpStatus: result.outcome = UpdateOutcome::HttpStatus; break;
        case HttpOutcome::Network: result.outcome = UpdateOutcome::Network; break;
        case HttpOutcome::Ok: result.outcome = UpdateOutcome::Ok; break;
    }
    return result;
}

bool FillVerifiedManifest(const HttpResult &manifest, const char *algorithm,
                          UpdateCheckResult *result) {
    const auto parsed = manifest::Parse(manifest.body, kAppVersion);
    if (!parsed.ok) {
        result->outcome = UpdateOutcome::InvalidManifest;
        result->detail = parsed.error;
        return false;
    }
    result->outcome = UpdateOutcome::Ok;
    result->manifest_text = manifest.body;
    result->agent_version = parsed.manifest.agent.version;
    result->detail = std::string("manifest ok (") + algorithm + "), agent " +
                     result->agent_version;
    return true;
}

}  // namespace

UpdateCheckResult CheckForUpdate(const std::string &url, long timeout_seconds) {
    const HttpResult manifest = Get(url, kManifestMaxBytes, timeout_seconds);
    if (manifest.outcome != HttpOutcome::Ok) {
        return FromHttp(manifest);
    }
    UpdateCheckResult result;
    result.http_code = manifest.status;

    const std::string_view ed25519_key = payload::EmbeddedAgentEd25519PublicKey();
    const std::string_view pem_view = payload::EmbeddedAgentPublicKeyPem();
    if (ed25519_key.empty() && pem_view.empty()) {
        result.outcome = UpdateOutcome::NoTrustAnchor;
        result.detail = "no manifest public key is embedded in this build";
        return result;
    }

    bool signature_seen = false;
    std::string ed25519_error;
    if (ed25519_key.size() == kEd25519PublicKeySize) {
        const HttpResult signature =
            Get(url + kEd25519SignatureSuffix, kSignatureMaxBytes, timeout_seconds);
        if (signature.outcome == HttpOutcome::Ok) {
            signature_seen = true;
            const auto *key = reinterpret_cast<const std::uint8_t *>(ed25519_key.data());
            if (VerifyEd25519(ManifestEd25519SigningInput(manifest.body), signature.body, key,
                              &ed25519_error)) {
                FillVerifiedManifest(manifest, "Ed25519", &result);
                return result;
            }
        } else {
            ed25519_error = "no Ed25519 signature (" + signature.detail + ")";
        }
    } else {
        ed25519_error = ed25519_key.empty()
            ? "no Ed25519 public key is embedded in this build"
            : "the embedded Ed25519 public key is not 32 bytes";
    }

    std::string p256_error;
    if (!pem_view.empty()) {
        const HttpResult signature =
            Get(url + kLegacySignatureSuffix, kSignatureMaxBytes, timeout_seconds);
        if (signature.outcome == HttpOutcome::Ok) {
            signature_seen = true;
            const std::string pem(pem_view);
            if (VerifyP256Signature(manifest.body, signature.body, pem.c_str(), &p256_error)) {
                FillVerifiedManifest(manifest, "P-256 legacy", &result);
                return result;
            }
        } else {
            p256_error = "no P-256 signature (" + signature.detail + ")";
        }
    } else {
        p256_error = "no P-256 public key is embedded in this build";
    }

    result.outcome = signature_seen ? UpdateOutcome::BadSignature : UpdateOutcome::NoSignature;
    result.detail = ed25519_error + "; " + p256_error;
    return result;
}

}  // namespace acnh_manager::net
