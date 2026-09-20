#pragma once

/* Embedded release channel (the official one).

   Both the release manifest and the payload are compiled into the NRO:
   tools/import-agent-release.py writes acnh-agent's release artifacts into `data/`, and
   devkitPro's bin2s turns them into .rodata symbols at build time.  That makes the official
   install fully offline and free of romfs / devoptab / services -- a path verified safe on
   this console repeatedly.

   The development channel is still there: the SD manifest
   (`/switch/ACNH-Manager/dev-manifest.json`) plus a payload directory, used only when the UI
   explicitly allows it. */

#include <vector>

#include "install/engine.hpp"

namespace acnh_manager::payload {

/* JSON text of the embedded release manifest; length 0 when no release was imported. */
std::string_view EmbeddedManifestJson();

/* PEM public key of the release-signing key (`data/agent_pubkey.bin`), which is what makes the
   downloaded manifest trustworthy even though TLS verification is off.  Empty when the build
   carries no key -- the update check then refuses to trust anything from the network. */
std::string_view EmbeddedAgentPublicKeyPem();

/* Whether the manifest and all three payload files are present. */
bool EmbeddedAvailable();

/* Read a payload straight from .rodata; names match files[].source in the manifest. */
class EmbeddedPayloadSource final : public install::PayloadSource {
public:
    bool Read(const std::string &name, std::vector<u8> *out, std::string *error) override;
};

}  // namespace acnh_manager::payload
