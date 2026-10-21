#include "env/startup_check.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace acnh_manager::env {
namespace {

constexpr std::size_t kNroStartSize = 0x10;
constexpr std::size_t kNroHeaderSize = 0x80;
constexpr std::uint32_t kNroMagic = 0x304F524E;      /* "NRO0" */
constexpr std::uint32_t kAssetMagic = 0x54455341;    /* "ASET" */
constexpr std::size_t kAssetHeaderSize = 0x38;
constexpr std::size_t kAssetNacpOffsetAt = 0x18;     /* inside the asset header */
constexpr std::size_t kAssetNacpSizeAt = 0x20;

/* NacpStruct's language table is 16 * (0x200 name + 0x100 author); DisplayVersion follows the
   fixed part.  The runtime has a static_assert against offsetof(NacpStruct, ...), so a libnx
   change cannot silently break this parser. */
constexpr std::size_t kNacpEntrySize = 0x300;
constexpr std::size_t kNacpEntryCount = 16;
constexpr std::size_t kNacpNameSize = 0x200;
constexpr std::size_t kNacpAuthorSize = 0x100;
constexpr std::size_t kNacpAuthorOffset = 0x200;
constexpr std::size_t kNacpDisplayVersionOffset = 0x3060;
constexpr std::size_t kNacpDisplayVersionSize = 0x10;
constexpr std::size_t kNacpMinimumSize = kNacpDisplayVersionOffset + kNacpDisplayVersionSize;

std::uint32_t ReadU32(const std::uint8_t *p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint64_t ReadU64(const std::uint8_t *p) {
    return static_cast<std::uint64_t>(ReadU32(p)) |
           (static_cast<std::uint64_t>(ReadU32(p + 4)) << 32);
}

std::string ReadFixedString(const std::uint8_t *p, std::size_t size) {
    std::size_t length = 0;
    while (length < size && p[length] != 0) {
        ++length;
    }
    while (length > 0 && (p[length - 1] == ' ' || p[length - 1] == '\t')) {
        --length;
    }
    return std::string(reinterpret_cast<const char *>(p), length);
}

std::uint64_t VersionComponent(std::string_view text, int index) {
    int component = 0;
    std::size_t at = 0;
    while (component < index) {
        const std::size_t dot = text.find('.', at);
        if (dot == std::string_view::npos) {
            return 0;
        }
        at = dot + 1;
        ++component;
    }
    std::uint64_t value = 0;
    while (at < text.size() && std::isdigit(static_cast<unsigned char>(text[at])) != 0) {
        value = value * 10 + static_cast<std::uint64_t>(text[at] - '0');
        ++at;
    }
    return value;
}

}  // namespace

bool ParseNroMeta(const std::uint8_t *data, std::size_t size, NroMeta *out, std::string *error) {
    if (out == nullptr) {
        return false;
    }
    NroMeta meta;
    meta.size = size;
    if (data == nullptr || size < kNroStartSize + kNroHeaderSize) {
        meta.error = "the file is smaller than an NRO header";
        *out = meta;
        if (error != nullptr) {
            *error = meta.error;
        }
        return false;
    }
    const std::uint8_t *header = data + kNroStartSize;
    if (ReadU32(header) != kNroMagic) {
        meta.error = "the file is not an NRO";
        *out = meta;
        if (error != nullptr) {
            *error = meta.error;
        }
        return false;
    }
    const std::uint64_t code_size = ReadU32(header + 8);
    if (code_size < kNroStartSize + kNroHeaderSize || code_size > size ||
        size - code_size < kAssetHeaderSize) {
        meta.error = "the NRO code size does not leave an asset header";
        *out = meta;
        if (error != nullptr) {
            *error = meta.error;
        }
        return false;
    }
    const std::uint8_t *asset = data + code_size;
    if (ReadU32(asset) != kAssetMagic) {
        meta.error = "the NRO has no asset header";
        *out = meta;
        if (error != nullptr) {
            *error = meta.error;
        }
        return false;
    }
    const std::uint64_t nacp_offset = ReadU64(asset + kAssetNacpOffsetAt);
    const std::uint64_t nacp_size = ReadU64(asset + kAssetNacpSizeAt);
    if (nacp_size < kNacpMinimumSize || nacp_offset > size - code_size ||
        nacp_size > size - code_size - nacp_offset) {
        meta.error = "the NRO has no readable NACP";
        *out = meta;
        if (error != nullptr) {
            *error = meta.error;
        }
        return false;
    }
    const std::uint8_t *nacp = data + code_size + nacp_offset;
    for (std::size_t i = 0; i < kNacpEntryCount; ++i) {
        const std::uint8_t *entry = nacp + i * kNacpEntrySize;
        const std::string name = ReadFixedString(entry, kNacpNameSize);
        if (name.empty()) {
            continue;
        }
        meta.name = name;
        meta.author =
            ReadFixedString(entry + kNacpAuthorOffset, kNacpAuthorSize);
        break;
    }
    meta.version = ReadFixedString(nacp + kNacpDisplayVersionOffset, kNacpDisplayVersionSize);
    meta.ok = true;
    meta.error.clear();
    *out = meta;
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

bool NroLooksOurs(const NroMeta &meta) {
    return meta.ok && meta.name == "ACNH-Manager" && meta.author == "acnh-game";
}

bool SameNroPath(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

int CompareVersions(std::string_view a, std::string_view b) {
    for (int i = 0; i < 3; ++i) {
        const std::uint64_t left = VersionComponent(a, i);
        const std::uint64_t right = VersionComponent(b, i);
        if (left < right) {
            return -1;
        }
        if (left > right) {
            return 1;
        }
    }
    return 0;
}

StartupDecision DecideStartup(const StartupInputs &in) {
    StartupDecision decision;
    if (in.dev) {
        decision.reason = "dev";
        return decision;
    }
    /* argv[0] is the only fact about where this process came from.  A missing or unlikely path is
       reported, never acted on: moving the wrong file over the standard one is worse than asking
       the player to start it from hbmenu again. */
    const bool looks_like_nro =
        in.current_path.size() > 4 && in.current_path.front() == '/' &&
        in.current_path[in.current_path.size() - 4] == '.' &&
        std::tolower(static_cast<unsigned char>(in.current_path[in.current_path.size() - 3])) ==
            'n' &&
        std::tolower(static_cast<unsigned char>(in.current_path[in.current_path.size() - 2])) ==
            'r' &&
        std::tolower(static_cast<unsigned char>(in.current_path[in.current_path.size() - 1])) ==
            'o';
    if (!looks_like_nro) {
        decision.show_page = true;
        decision.action = StartupAction::ReportOnly;
        decision.reason = "unknown-path";
        return decision;
    }
    if (SameNroPath(in.current_path, kStandardNroPath)) {
        decision.reason = "standard";
        return decision;
    }
    if (!in.current_readable) {
        decision.show_page = true;
        decision.action = StartupAction::ReportOnly;
        decision.reason = "current-unreadable";
        return decision;
    }
    if (!in.standard_present) {
        decision.silent = true;
        decision.action = StartupAction::CopyCurrent;
        decision.reason = "standard-missing";
        return decision;
    }
    decision.show_page = true;
    if (!in.standard_readable || !in.standard_ours) {
        decision.action = StartupAction::CopyCurrent;
        decision.reason = "standard-not-ours";
        return decision;
    }
    const int order = CompareVersions(in.standard_version, in.current_version);
    if (order > 0) {
        decision.action = StartupAction::DeleteCurrent;
        decision.reason = "standard-newer";
        return decision;
    }
    if (order < 0) {
        decision.action = StartupAction::CopyCurrent;
        decision.reason = "current-newer";
        return decision;
    }
    if (in.same_build) {
        decision.action = StartupAction::DeleteCurrent;
        decision.reason = "same-build";
        return decision;
    }
    if (in.current_mtime_newer) {
        decision.action = StartupAction::CopyCurrent;
        decision.reason = "current-newer-build";
        return decision;
    }
    decision.action = StartupAction::DeleteCurrent;
    decision.reason = "standard-newer-build";
    return decision;
}

}  // namespace acnh_manager::env
