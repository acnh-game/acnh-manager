#pragma once

/* Payload straight from the release host: the files the verified manifest names, fetched from
   `baseUrl + files[].source`.

   The engine does the authentication that matters here and it does it already: it checks every
   payload against the size and sha256 in the manifest *before* anything is written, so this
   source only has to fetch bytes and report what went wrong.  The manifest itself was verified
   (ECDSA signature) by the update check before we ever got here, which is the link that keeps
   the chain trustworthy with TLS verification off (see docs/architecture.md 9). */

#include <utility>
#include <vector>

#include "install/engine.hpp"
#include "manifest/manifest.hpp"

namespace acnh_manager::install {

class NetworkPayloadSource final : public PayloadSource {
public:
    /* `manifest` supplies baseUrl; `game` supplies the file list to fetch. */
    NetworkPayloadSource(const manifest::Manifest &manifest, const manifest::GameEntry &game);

    bool Read(const std::string &name, std::vector<u8> *out, std::string *error) override;

private:
    std::vector<std::pair<std::string, std::string>> m_urls; /* files[].source -> URL */
};

}  // namespace acnh_manager::install
