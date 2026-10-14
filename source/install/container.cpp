#include "install/container.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>

namespace acnh_manager::install {
namespace {

constexpr std::uint32_t kPfs0Magic = 0x30534650u; /* "PFS0" */
constexpr std::uint32_t kHfs0Magic = 0x30534648u; /* "HFS0" */
/* "HEAD", read the way ReadU32 reads: little-endian.  The first version carried the big-endian
   spelling (0x48454144) and the host tests' fixture was written with the same wrong constant, so
   every real cartridge dump was refused with "unknown container" while the tests stayed green --
   found on hardware with the first XCI anyone tried (docs/device-acceptance.md). */
constexpr std::uint32_t kHeadMagic = 0x44414548u; /* "HEAD" */

constexpr std::uint64_t kXciRootOffset = 0xF000;
constexpr std::uint64_t kXciRootOffsetWithKeyArea = 0xF000 + 0x1000;

/* The reference implementation's limits: a malformed table must not make us allocate. */
constexpr std::uint32_t kMaxFiles = 0x10000;
constexpr std::uint32_t kMaxStringTable = 16u * 1024u * 1024u;

constexpr std::size_t kHeaderSize = 0x10;
/* The two formats do **not** share an entry size, which the first version of this file assumed:
   PFS0's entry is 0x18 bytes (u64 data offset, u64 size, u32 name offset, u32 padding), while
   HFS0's is 0x40 (the same four fields, then u64 reserved and a 0x20-byte hash of the entry's
   hashed region).  Reading a cartridge dump with the 0x18 stride walked into the middle of the
   first entry and the second, so every XCI was refused -- while the host tests stayed green,
   because their fixture wrote the same wrong stride (found on hardware, see
   docs/device-acceptance.md). */
constexpr std::size_t kPfs0EntrySize = 0x18;
constexpr std::size_t kHfs0EntrySize = 0x40;

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

bool AddNoOverflow(std::uint64_t lhs, std::uint64_t rhs, std::uint64_t *out) {
    if (rhs > std::numeric_limits<std::uint64_t>::max() - lhs) {
        return false;
    }
    *out = lhs + rhs;
    return true;
}

std::string Lower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

std::string Extension(std::string_view name) {
    const std::size_t dot = name.find_last_of('.');
    if (dot == std::string_view::npos) {
        return {};
    }
    return Lower(name.substr(dot + 1));
}

bool EndsWith(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(),
                                                        suffix) == 0;
}

/* Reads exactly `size` bytes or fails; keeps every call site free of partial-read handling. */
bool ReadExact(ContainerReader &reader, std::uint64_t offset, std::size_t size, std::uint8_t *out) {
    if (size == 0) {
        return true;
    }
    return reader.Read(offset, size, out);
}

bool EntryFits(std::uint64_t container_size, const ContainerEntry &entry) {
    if (container_size == 0) {
        return true; /* the source does not know the size (a stream), so this cannot be checked */
    }
    std::uint64_t end = 0;
    return AddNoOverflow(entry.offset, entry.packed_size, &end) && end <= container_size;
}

/* Shared tail of both formats: turn the raw table into entries, then split them into the
   installable .nca files and everything else (tickets, certificates, XCI partitions). */
bool CollectEntries(const std::vector<std::string> &names,
                    const std::vector<std::pair<std::uint64_t, std::uint64_t>> &sizes,
                    std::uint64_t data_offset, std::uint64_t container_size, ContainerInfo *out,
                    std::string *error) {
    for (std::size_t i = 0; i < names.size(); ++i) {
        const std::string name = Lower(names[i]);
        if (name.empty()) {
            continue;
        }
        ContainerEntry entry;
        entry.name = name;
        entry.offset = data_offset + sizes[i].first;
        /* The bytes the entry occupies.  For a compressed content this is the `.ncz` stream; the
           NCA it rebuilds is larger, and that size is filled in when the NCZ tables are read. */
        entry.size = sizes[i].second;
        entry.packed_size = sizes[i].second;
        if (!EntryFits(container_size, entry)) {
            *error = "an entry lies outside the container (broken header)";
            return false;
        }
        if (EndsWith(name, ".ncz")) {
            entry.compressed = true;
            out->contents.push_back(entry);
        } else if (EndsWith(name, ".nca")) {
            out->contents.push_back(entry);
        } else {
            out->extra.push_back(entry);
        }
    }
    return true;
}

bool ParsePfs0(ContainerReader &reader, std::uint64_t container_size, ContainerInfo *out,
               std::string *error) {
    std::uint8_t header_bytes[kHeaderSize];
    if (!ReadExact(reader, 0, sizeof(header_bytes), header_bytes)) {
        *error = "cannot read the container header";
        return false;
    }
    const std::uint32_t total_files = ReadU32(header_bytes + 4);
    const std::uint32_t string_table_size = ReadU32(header_bytes + 8);
    if (total_files > kMaxFiles || string_table_size > kMaxStringTable) {
        *error = "header counts are out of range";
        return false;
    }

    /* The file table starts right behind the 0x10 header; the strings follow it, and the file
       data follows the strings. */
    const std::uint64_t table_offset = kHeaderSize;
    std::uint64_t strings_offset = 0;
    std::uint64_t data_offset = 0;
    if (!AddNoOverflow(table_offset, static_cast<std::uint64_t>(total_files) * kPfs0EntrySize,
                       &strings_offset) ||
        !AddNoOverflow(strings_offset, string_table_size, &data_offset)) {
        *error = "header offsets overflow";
        return false;
    }
    if (container_size != 0 && data_offset > container_size) {
        *error = "the file is shorter than its own header (truncated transfer?)";
        return false;
    }

    std::vector<std::uint8_t> table(static_cast<std::size_t>(total_files) * kPfs0EntrySize);
    if (!ReadExact(reader, table_offset, table.size(), table.data())) {
        *error = "cannot read the file table";
        return false;
    }
    std::vector<char> strings(string_table_size);
    if (!ReadExact(reader, strings_offset, strings.size(),
                   reinterpret_cast<std::uint8_t *>(strings.data()))) {
        *error = "cannot read the string table";
        return false;
    }

    std::vector<std::string> names;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> sizes;
    names.reserve(total_files);
    sizes.reserve(total_files);
    for (std::uint32_t i = 0; i < total_files; ++i) {
        const std::uint8_t *row = table.data() + static_cast<std::size_t>(i) * kPfs0EntrySize;
        const std::uint32_t name_offset = ReadU32(row + 16);
        if (name_offset >= strings.size()) {
            *error = "name offset out of range (broken header)";
            return false;
        }
        /* Names are NUL-terminated inside the table; a missing terminator is a broken header. */
        const char *begin = strings.data() + name_offset;
        const char *end = static_cast<const char *>(
            std::memchr(begin, '\0', strings.size() - name_offset));
        if (end == nullptr) {
            *error = "name is not NUL-terminated (broken header)";
            return false;
        }
        names.emplace_back(begin, end);
        sizes.emplace_back(ReadU64(row), ReadU64(row + 8));
    }

    if (!CollectEntries(names, sizes, data_offset, container_size, out, error)) {
        return false;
    }
    if (out->contents.empty()) {
        /* A PFS0 without .nca files is a mod package in disguise (romfs/exefs only), which this
           app refuses for the same reason it refuses .msp (docs section 7). */
        *error = "this package has no installable content (mod files only?)";
        return false;
    }
    return true;
}

bool ParseHfs0(ContainerReader &reader, std::uint64_t offset, std::uint64_t container_size,
               std::vector<std::string> *names,
               std::vector<std::pair<std::uint64_t, std::uint64_t>> *sizes,
               std::uint64_t *data_offset, std::string *error) {
    std::uint8_t header_bytes[kHeaderSize];
    if (!ReadExact(reader, offset, sizeof(header_bytes), header_bytes)) {
        *error = "cannot read the XCI partition table";
        return false;
    }
    if (ReadU32(header_bytes) != kHfs0Magic) {
        *error = "bad XCI partition magic";
        return false;
    }
    const std::uint32_t total_files = ReadU32(header_bytes + 4);
    const std::uint32_t string_table_size = ReadU32(header_bytes + 8);
    if (total_files > kMaxFiles || string_table_size > kMaxStringTable) {
        *error = "XCI partition counts are out of range";
        return false;
    }

    std::uint64_t table_offset = 0;
    std::uint64_t strings_offset = 0;
    std::uint64_t entries_offset = 0;
    if (!AddNoOverflow(offset, kHeaderSize, &table_offset) ||
        !AddNoOverflow(table_offset, static_cast<std::uint64_t>(total_files) * kHfs0EntrySize,
                       &strings_offset) ||
        !AddNoOverflow(strings_offset, string_table_size, &entries_offset)) {
        *error = "XCI partition offsets overflow";
        return false;
    }
    if (container_size != 0 && strings_offset > container_size) {
        *error = "the partition table runs past the end of the container";
        return false;
    }

    std::vector<std::uint8_t> table(static_cast<std::size_t>(total_files) * kHfs0EntrySize);
    if (!ReadExact(reader, table_offset, table.size(), table.data())) {
        *error = "cannot read the XCI partition table";
        return false;
    }
    std::vector<char> strings(string_table_size);
    if (!ReadExact(reader, strings_offset, strings.size(),
                   reinterpret_cast<std::uint8_t *>(strings.data()))) {
        *error = "cannot read the XCI partition name table";
        return false;
    }

    names->clear();
    sizes->clear();
    for (std::uint32_t i = 0; i < total_files; ++i) {
        const std::uint8_t *row = table.data() + static_cast<std::size_t>(i) * kHfs0EntrySize;
        const std::uint32_t name_offset = ReadU32(row + 16);
        if (name_offset >= strings.size()) {
            *error = "XCI partition name offset out of range";
            return false;
        }
        const char *begin = strings.data() + name_offset;
        const char *end = static_cast<const char *>(
            std::memchr(begin, '\0', strings.size() - name_offset));
        if (end == nullptr) {
            *error = "XCI partition name is not NUL-terminated";
            return false;
        }
        names->emplace_back(begin, end);
        sizes->emplace_back(ReadU64(row), ReadU64(row + 8));
    }
    *data_offset = entries_offset;
    return true;
}

bool ParseXci(ContainerReader &reader, std::uint64_t container_size, ContainerInfo *out,
              std::string *error) {
    /* The root partition sits at a fixed offset; a card dump with a key area in front shifts it
       by 0x1000, which is why both are tried (same order as the reference implementation). */
    for (const std::uint64_t root : {kXciRootOffset, kXciRootOffsetWithKeyArea}) {
        std::vector<std::string> names;
        std::vector<std::pair<std::uint64_t, std::uint64_t>> sizes;
        std::uint64_t data_offset = 0;
        std::string ignored;
        if (!ParseHfs0(reader, root, container_size, &names, &sizes, &data_offset, &ignored)) {
            continue;
        }
        for (std::size_t i = 0; i < names.size(); ++i) {
            if (Lower(names[i]) != "secure") {
                continue;
            }
            std::vector<std::string> secure_names;
            std::vector<std::pair<std::uint64_t, std::uint64_t>> secure_sizes;
            std::uint64_t secure_data_offset = 0;
            if (!ParseHfs0(reader, data_offset + sizes[i].first, container_size, &secure_names,
                           &secure_sizes, &secure_data_offset, error)) {
                return false;
            }
            if (!CollectEntries(secure_names, secure_sizes, secure_data_offset, container_size, out,
                                error)) {
                return false;
            }
            if (out->contents.empty()) {
                *error = "the XCI secure partition holds no content (not an installable cartridge dump?)";
                return false;
            }
            return true;
        }
        *error = "no secure partition in this XCI";
        return false;
    }
    *error = "not a valid XCI (no root partition table)";
    return false;
}

}  // namespace

bool IsSupportedExtension(std::string_view name) {
    const std::string extension = Extension(name);
    return extension == "nsp" || extension == "nsz" || extension == "xci" || extension == "xcz";
}

bool IsModPackageExtension(std::string_view name) { return Extension(name) == "msp"; }

bool OpenContainer(ContainerReader &reader, std::string_view name, std::uint64_t container_size,
                   ContainerInfo *out, std::string *error) {
    *out = ContainerInfo{};
    error->clear();

    if (IsModPackageExtension(name)) {
        *error = "this is a mod package (.msp), not a game or update; use a tool that installs those";
        return false;
    }

    std::uint8_t head[0x200];
    if (!ReadExact(reader, 0, sizeof(head), head)) {
        *error = "cannot read the container header";
        return false;
    }

    const std::string extension = Extension(name);
    out->container_size = container_size;

    if (ReadU32(head) == kPfs0Magic) {
        out->kind = extension == "nsz" ? ContainerKind::Nsz : ContainerKind::Nsp;
        out->compressed = out->kind == ContainerKind::Nsz;
        return ParsePfs0(reader, container_size, out, error);
    }
    if (ReadU32(head + 0x100) == kHeadMagic) {
        out->kind = extension == "xcz" ? ContainerKind::Xcz : ContainerKind::Xci;
        out->compressed = out->kind == ContainerKind::Xcz;
        return ParseXci(reader, container_size, out, error);
    }

    *error = "unknown container (neither NSP nor XCI)";
    return false;
}

}  // namespace acnh_manager::install
