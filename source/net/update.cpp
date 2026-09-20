#include "update.hpp"

#include <string>

#include "manifest/manifest.hpp"
#include "net/http.hpp"
#include "net/signature.hpp"
#include "payload/embedded.hpp"
#include "version.hpp"

namespace acnh_manager::net {
namespace {

/* The signature is published next to the manifest; one constant keeps them together. */
constexpr const char *kSignatureSuffix = ".sig";
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

}  // namespace

UpdateCheckResult CheckForUpdate(const std::string &url, long timeout_seconds) {
    const HttpResult manifest = Get(url, kManifestMaxBytes, timeout_seconds);
    if (manifest.outcome != HttpOutcome::Ok) {
        return FromHttp(manifest);
    }
    UpdateCheckResult result;
    result.http_code = manifest.status;

    const HttpResult signature = Get(url + kSignatureSuffix, kSignatureMaxBytes, timeout_seconds);
    if (signature.outcome != HttpOutcome::Ok) {
        result.outcome = UpdateOutcome::NoSignature;
        result.detail = "no signature (" + signature.detail + ")";
        return result;
    }

    /* The key is a build input (data/agent_pubkey.bin), so a build made without one cannot
       trust anything the network says -- say that, instead of silently accepting. */
    const std::string_view pem_view = payload::EmbeddedAgentPublicKeyPem();
    if (pem_view.empty()) {
        result.outcome = UpdateOutcome::NoTrustAnchor;
        result.detail = "no public key is embedded in this build";
        return result;
    }
    const std::string pem(pem_view);
    std::string signature_error;
    if (!VerifySignature(manifest.body, signature.body, pem.c_str(), &signature_error)) {
        result.outcome = UpdateOutcome::BadSignature;
        result.detail = signature_error;
        return result;
    }

    const auto parsed = manifest::Parse(manifest.body, kAppVersion);
    if (!parsed.ok) {
        result.outcome = UpdateOutcome::InvalidManifest;
        result.detail = parsed.error;
        return result;
    }
    result.outcome = UpdateOutcome::Ok;
    result.manifest_text = manifest.body;
    result.agent_version = parsed.manifest.agent.version;
    result.detail = "manifest ok, agent " + result.agent_version;
    return result;
}

}  // namespace acnh_manager::net
