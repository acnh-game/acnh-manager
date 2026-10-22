#include "env/forwarder.hpp"

#include <string>

#include "util/sha256.hpp"

namespace acnh_manager::env {
namespace {

constexpr std::uint64_t kForwarderPrefix = 0x0500000000000000ULL;
constexpr std::uint64_t kForwarderTypeMask = 0xFF00000000000000ULL;
constexpr std::uint64_t kForwarderMask = 0x00FFFFFFFFFFF000ULL;

}  // namespace

std::uint64_t ForwarderTitleId(std::string_view nro_path, std::string_view args) {
    /* sphaira seeds an empty argument list with the NRO path itself, then hashes
       `path + args`; see the header. */
    const std::string path(nro_path);
    const std::string effective_args =
        args.empty() ? path : path + " " + std::string(args);
    const std::string input = path + effective_args;
    util::Sha256 sha;
    sha.Update(input.data(), input.size());
    const auto digest = sha.Finish();
    std::uint64_t head = 0;
    for (int i = 0; i < 8; ++i) {
        head |= static_cast<std::uint64_t>(digest[static_cast<std::size_t>(i)]) << (8 * i);
    }
    return kForwarderPrefix | (head & kForwarderMask);
}

std::uint64_t StandardForwarderTitleId() {
    return ForwarderTitleId(kForwarderNroPath, std::string_view());
}

std::uint64_t LegacyForwarderTitleId() {
    return ForwarderTitleId(kStandardNroPath, std::string_view());
}

bool IsForwarderTitleId(std::uint64_t title_id) {
    return (title_id & kForwarderTypeMask) == kForwarderPrefix;
}

}  // namespace acnh_manager::env
