#include "install/nca.hpp"

#include <cstdio>

#include <cstring>

namespace acnh_manager::install {
namespace {

constexpr std::uint32_t kNca3Magic = 0x3341434E; /* "NCA3" */

constexpr std::size_t kNcaHeaderSize = 0xC00;
constexpr std::size_t kNcaOffsetProgramId = 0x210;
constexpr std::size_t kNcaOffsetKeyGeneration = 0x220;
constexpr std::size_t kNcaOffsetOldKeyGeneration = 0x206;
constexpr std::size_t kNcaOffsetDistribution = 0x204;
constexpr std::size_t kNcaOffsetContentType = 0x205;
constexpr std::size_t kNcaOffsetSize = 0x208;
constexpr std::size_t kNcaOffsetRightsId = 0x230; /* 16 bytes; all zero when there is none */

constexpr std::size_t kCnmtHeaderSize = 0x20;
constexpr std::size_t kCnmtContentEntrySize = 0x38; /* 0x20 hash + 0x18 content info */
constexpr std::size_t kCnmtMetaEntrySize = 0x10;

std::uint16_t ReadU16(const std::uint8_t *p) {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

std::uint32_t ReadU32(const std::uint8_t *p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint64_t ReadU64(const std::uint8_t *p) {
    std::uint64_t value = 0;
    for (int i = 7; i >= 0; --i) {
        value = (value << 8) | p[i];
    }
    return value;
}

}  // namespace

const char *MetaTypeLabel(MetaType type) {
    switch (type) {
        case MetaType::Application: return "base";
        case MetaType::Patch: return "update";
        case MetaType::AddOnContent: return "dlc";
        case MetaType::Delta: return "delta";
        case MetaType::DataPatch: return "dlc-update";
        case MetaType::SystemProgram:
        case MetaType::SystemData:
        case MetaType::SystemUpdate:
        case MetaType::BootImagePackage:
        case MetaType::BootImagePackageSafe: return "system";
        case MetaType::Unknown: break;
    }
    return "unknown";
}

bool MetaTypeCarriesGameVersion(MetaType type) {
    return type == MetaType::Application || type == MetaType::Patch;
}

bool MetaTypeIsInstallable(MetaType type) {
    /* Spelled out instead of testing bit 0x80: `Unknown` is this app's stand-in for a type byte
       the console does not define, and a bit test would let 0xFF (and any future value above 0x80)
       through as if it were a user title.  Every value the console defines is listed here. */
    switch (type) {
        case MetaType::Application:
        case MetaType::Patch:
        case MetaType::AddOnContent:
        case MetaType::Delta:
        case MetaType::DataPatch:
            return true;
        default:
            return false;
    }
}

/* Title ids are stored little-endian in the headers but written big-endian everywhere a person
   reads them (file names, tickets, the log).  Content ids are the other way round: they are
   already in the order the names use.  Getting this backwards made the console answer "no such
   title is installed" for a title it had -- measured, that is what turned a should-skip pick into
   a confirm page. */
std::string HexTitleId(const std::uint8_t *bytes, std::size_t size) {
    std::uint8_t reversed[16];
    for (std::size_t i = 0; i < size; ++i) {
        reversed[i] = bytes[size - 1 - i];
    }
    return HexId(reversed, size);
}

std::string HexId(const std::uint8_t *bytes, std::size_t size) {
    static const char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        out.push_back(kDigits[bytes[i] >> 4]);
        out.push_back(kDigits[bytes[i] & 0x0F]);
    }
    return out;
}

bool ParseCnmt(const std::uint8_t *data, std::size_t size, CnmtInfo *out, std::string *error) {
    *out = CnmtInfo{};
    error->clear();
    if (data == nullptr || size < kCnmtHeaderSize) {
        *error = "the meta content is too small to hold a CNMT header";
        return false;
    }

    out->title_id = HexTitleId(data, 8);
    out->version = ReadU32(data + 8);
    const std::uint8_t raw_type = data[0x0C];
    switch (raw_type) {
        case 0x01: out->type = MetaType::SystemProgram; break;
        case 0x02: out->type = MetaType::SystemData; break;
        case 0x03: out->type = MetaType::SystemUpdate; break;
        case 0x04: out->type = MetaType::BootImagePackage; break;
        case 0x05: out->type = MetaType::BootImagePackageSafe; break;
        case 0x80: out->type = MetaType::Application; break;
        case 0x81: out->type = MetaType::Patch; break;
        case 0x82: out->type = MetaType::AddOnContent; break;
        case 0x83: out->type = MetaType::Delta; break;
        case 0x84: out->type = MetaType::DataPatch; break;
        default: out->type = MetaType::Unknown; break;
    }
    const std::uint16_t extended_header_size = ReadU16(data + 0x0E);
    const std::uint16_t content_count = ReadU16(data + 0x10);
    const std::uint16_t content_meta_count = ReadU16(data + 0x12);
    out->extended_header_size = extended_header_size;
    out->content_meta_count = content_meta_count;
    out->attributes = data[0x14];
    out->storage_id = data[0x15];

    /* The extended header carries the ids the type-specific checks need; every field is read
       defensively because a repacked package can declare a shorter header than the type
       usually carries. */
    const std::size_t extended = kCnmtHeaderSize + extended_header_size;
    if (extended > size) {
        *error = "the CNMT extended header runs past the end of the payload";
        return false;
    }
    switch (out->type) {
        case MetaType::Application:
            /* {u64 patch id, u32 required system version, u32 required application version} */
            if (extended_header_size >= 12) {
                out->required_system_version = ReadU32(data + kCnmtHeaderSize + 8);
            }
            break;
        case MetaType::Patch:
            /* {u64 application id, u32 required system version, u32 extended data size, u8[8]} */
            if (extended_header_size >= 8) {
                out->application_id = HexTitleId(data + kCnmtHeaderSize, 8);
            }
            if (extended_header_size >= 12) {
                out->required_system_version = ReadU32(data + kCnmtHeaderSize + 8);
            }
            break;
        case MetaType::AddOnContent:
            /* {u64 application id, u32 required *application* version, ...}: an add-on asks for a
               game version, not for a firmware one, so this is not a required system version. */
            if (extended_header_size >= 8) {
                out->application_id = HexTitleId(data + kCnmtHeaderSize, 8);
            }
            break;
        case MetaType::DataPatch:
            /* {u64 data id, u64 application id, u32 required application version, ...}: the id
               this data patch belongs to sits *after* the data id, unlike the other types. */
            if (extended_header_size >= 16) {
                out->application_id = HexTitleId(data + kCnmtHeaderSize + 8, 8);
            }
            break;
        default:
            break;
    }

    /* content_count * 0x38 then content_meta_count * 0x10, both after the extended header.  The
       check is an overflow-free comparison against the payload we actually have. */
    const std::size_t contents_end = extended + static_cast<std::size_t>(content_count) *
                                                    kCnmtContentEntrySize;
    const std::size_t metas_end = contents_end + static_cast<std::size_t>(content_meta_count) *
                                                     kCnmtMetaEntrySize;
    if (metas_end > size) {
        *error = "the CNMT content table runs past the end of the payload";
        return false;
    }
    for (std::uint16_t i = 0; i < content_count; ++i) {
        const std::uint8_t *entry = data + extended + static_cast<std::size_t>(i) *
                                                    kCnmtContentEntrySize;
        const std::uint8_t *info = entry + 0x20; /* skip the sha256 */
        CnmtContent content;
        content.content_id = HexId(info, 0x10);
        const std::uint32_t size_low = ReadU32(info + 0x10);
        const std::uint8_t size_high = info[0x14];
        content.size = (static_cast<std::uint64_t>(size_high) << 32) | size_low;
        /* The numbers are NcmContentType, verified against libnx's own enum:
             0 Meta, 1 Program, 2 Data, 3 Control, 4 HtmlDocument, 5 LegalInformation,
             6 DeltaFragment.
           Getting this wrong is silent: the sizes and ids all read correctly, the contents are
           simply filed under the wrong kind, and "skip the delta fragments" then throws away the
           real program content (measured: an empty task list, so the install looked like there
           was nothing to do). */
        switch (info[0x16]) {
            case 0x00: content.type = ContentType::Meta; break;
            case 0x01: content.type = ContentType::Program; break;
            case 0x02: content.type = ContentType::Data; break;
            case 0x03: content.type = ContentType::Control; break;
            case 0x04: content.type = ContentType::Manual; break;
            case 0x05: content.type = ContentType::PublicData; break;
            case 0x06: content.type = ContentType::DeltaFragment; break;
            default: content.type = ContentType::Unknown; break;
        }
        out->contents.push_back(std::move(content));
    }
    /* What the parse actually saw.  The first content id has to match one of the container's
       .nca names; when it does not, the entry table is misaligned (wrong extended header size)
       and every type byte after it is noise -- which is how "33 contents" turned into "0". */
    /* Self-check for the entry layout, kept short because the log line is length-limited: the
       sizes are known from the container (the first content is the multi-GB program NCA), so
       "which field is where" can be settled from four numbers. */
    out->parse_note.clear();
    for (std::size_t i = 0; i < out->contents.size() && i < 2; ++i) {
        char note[64];
        std::snprintf(note, sizeof(note), " e%zut=%u e%zus=%llu", i,
                      static_cast<unsigned>(out->contents[i].type), i,
                      static_cast<unsigned long long>(out->contents[i].size));
        out->parse_note += note;
    }
    if (out->contents.empty()) {
        *error = "the CNMT lists no contents";
        return false;
    }
    return true;
}

bool ParseNcaHeader(const std::uint8_t *data, std::size_t size, NcaHeaderInfo *out,
                    std::string *error) {
    *out = NcaHeaderInfo{};
    error->clear();
    if (data == nullptr || size < kNcaHeaderSize) {
        *error = "the NCA header is shorter than 0xC00 bytes";
        return false;
    }
    const std::uint32_t magic = ReadU32(data + 0x200);
    if (magic != kNca3Magic) {
        *error = "not an NCA3 header";
        return false;
    }
    out->magic = magic;
    out->distribution_type = data[kNcaOffsetDistribution];
    switch (data[kNcaOffsetContentType]) {
        case 0x00: out->content_type = ContentType::Program; break;
        case 0x01: out->content_type = ContentType::Meta; break;
        case 0x02: out->content_type = ContentType::Control; break;
        case 0x03: out->content_type = ContentType::Manual; break;
        case 0x04: out->content_type = ContentType::Data; break;
        case 0x05: out->content_type = ContentType::PublicData; break;
        default: out->content_type = ContentType::Unknown; break;
    }
    out->size = ReadU64(data + kNcaOffsetSize);
    /* Section table: four entries of {u32 media start, u32 media end, ...}, in 0x200 units. */
    const std::uint32_t section_start = ReadU32(data + 0x240);
    const std::uint32_t section_end = ReadU32(data + 0x244);
    if (section_end > section_start) {
        out->section0_offset = static_cast<std::uint64_t>(section_start) * 0x200;
        out->section0_size =
            static_cast<std::uint64_t>(section_end - section_start) * 0x200;
    }
    out->program_id = HexTitleId(data + kNcaOffsetProgramId, 8);
    const std::uint8_t old_generation = data[kNcaOffsetOldKeyGeneration];
    const std::uint8_t generation = data[kNcaOffsetKeyGeneration];
    out->key_generation = generation > old_generation ? generation : old_generation;
    /* The rights id decides whether the title key comes from a ticket.  An id of all zeros means
       "standard crypto", which is the common case for cartridge dumps; a real one means the
       package must carry the ticket for it (and that the ticket has to be imported, which is a
       finish step of its own). */
    bool any = false;
    for (std::size_t i = 0; i < 16; ++i) {
        any = any || data[kNcaOffsetRightsId + i] != 0;
    }
    out->rights_id = any ? HexId(data + kNcaOffsetRightsId, 16) : std::string();
    return true;
}

void SetDistributionToSystem(std::uint8_t *decrypted_header) {
    if (decrypted_header != nullptr) {
        decrypted_header[kNcaOffsetDistribution] = 0;
    }
}

}  // namespace acnh_manager::install
