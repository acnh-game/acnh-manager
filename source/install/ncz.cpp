#include "install/ncz.hpp"

#include <cstring>

namespace acnh_manager::install {
namespace {

std::uint64_t ReadU64(const std::uint8_t *p) {
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value |= static_cast<std::uint64_t>(p[i]) << (8 * i);
    }
    return value;
}

std::uint32_t ReadU32(const std::uint8_t *p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

}  // namespace

bool ParseNczHeader(const std::uint8_t *bytes, std::size_t size, NczHeaderInfo *out,
                    std::string *error) {
    if (bytes == nullptr || out == nullptr || size < kNczHeaderSize + 16) {
        if (error != nullptr) {
            *error = "the compressed content is shorter than its own header";
        }
        return false;
    }
    const std::uint8_t *header = bytes + kNczHeaderSize;
    if (ReadU64(header) != kNczSectionMagic) {
        if (error != nullptr) {
            *error = "the compressed content has no section table";
        }
        return false;
    }
    const std::uint64_t count = ReadU64(header + 8);
    if (count == 0 || count > kNczMaxSections) {
        if (error != nullptr) {
            *error = "the compressed content declares an impossible section count";
        }
        return false;
    }
    out->section_count = count;
    out->tables_offset = kNczHeaderSize + 16;
    return true;
}

bool ParseNczTables(const std::uint8_t *bytes, std::size_t size, const NczHeaderInfo &header,
                    std::uint64_t ncz_size, NczInfo *out, std::string *error) {
    if (bytes == nullptr || out == nullptr) {
        return false;
    }
    const std::uint64_t tables_bytes = header.section_count * kNczSectionSize;
    if (tables_bytes > size) {
        if (error != nullptr) {
            *error = "the compressed content's section table runs past the file";
        }
        return false;
    }
    out->sections.clear();
    out->sections.reserve(static_cast<std::size_t>(header.section_count));
    for (std::uint64_t i = 0; i < header.section_count; ++i) {
        const std::uint8_t *row = bytes + i * kNczSectionSize;
        NczSection section;
        section.offset = ReadU64(row);
        section.size = ReadU64(row + 8);
        section.crypto_type = ReadU64(row + 16);
        std::memcpy(section.key.data(), row + 32, section.key.size());
        std::memcpy(section.counter.data(), row + 48, section.counter.size());
        if (section.size == 0 || section.offset < kNczHeaderSize) {
            if (error != nullptr) {
                *error = "the compressed content has an impossible section";
            }
            return false;
        }
        out->sections.push_back(section);
    }

    std::uint64_t cursor = tables_bytes;
    /* The newer files put a block table between the sections and the payload.  Reading it is not
       needed to decode (the stream is sequential either way), but its size has to be skipped, and
       the offsets are what a seek-based reader would use. */
    if (cursor + 24 <= size && ReadU64(bytes + cursor) == kNczBlockMagic) {
        const std::uint8_t *block_header = bytes + cursor;
        const std::uint8_t version = block_header[8];
        const std::uint8_t type = block_header[9];
        const std::uint8_t exponent = block_header[11];
        const std::uint32_t total_blocks = ReadU32(block_header + 12);
        if (version != 2 || type != 1 || exponent < 14 || exponent > 32 || total_blocks == 0) {
            if (error != nullptr) {
                *error = "the compressed content's block table is not one this app understands";
            }
            return false;
        }
        const std::uint64_t table_bytes = 24 + static_cast<std::uint64_t>(total_blocks) * 4;
        if (cursor + table_bytes > size) {
            if (error != nullptr) {
                *error = "the compressed content's block table runs past the file";
            }
            return false;
        }
        out->block_table = true;
        out->block_size = 1u << exponent;
        std::uint64_t offset = header.tables_offset + cursor + table_bytes;
        out->block_offsets.reserve(total_blocks);
        for (std::uint32_t i = 0; i < total_blocks; ++i) {
            out->block_offsets.push_back(offset);
            offset += ReadU32(block_header + 24 + i * 4);
        }
        cursor += table_bytes;
    }

    out->payload_offset = header.tables_offset + cursor;
    if (ncz_size <= out->payload_offset) {
        if (error != nullptr) {
            *error = "the compressed content carries no payload";
        }
        return false;
    }
    out->payload_size = ncz_size - out->payload_offset;

    /* The NCA this stream rebuilds: the header plus everything the sections cover.  The sections
       are the NCA's file-system sections, so the last of them ends at the NCA's size -- checked
       against a real package (386 461 184 = the content size the CNMT names). */
    std::uint64_t end = 0;
    for (const NczSection &section : out->sections) {
        if (section.End() > end) {
            end = section.End();
        }
    }
    out->nca_size = end;
    return true;
}

const NczSection *FindNczSection(const NczInfo &info, std::uint64_t nca_offset) {
    for (const NczSection &section : info.sections) {
        if (section.Covers(nca_offset)) {
            return &section;
        }
    }
    return nullptr;
}

std::array<std::uint8_t, 16> NczSectionCounter(const NczSection &section, std::uint64_t nca_offset) {
    std::array<std::uint8_t, 16> counter = section.counter;
    const std::uint64_t block = nca_offset >> 4;
    for (int i = 0; i < 8; ++i) {
        counter[8 + i] = static_cast<std::uint8_t>((block >> (8 * (7 - i))) & 0xFF);
    }
    return counter;
}

}  // namespace acnh_manager::install
