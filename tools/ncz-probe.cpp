/* Decode one compressed content (NSZ/XCZ) with the app's own code, on the development machine.

   Why it exists: whether a `.ncz` decodes is otherwise a console round trip away, and the console
   can only answer with one line of text ("the compressed content could not be decoded") after a
   build, a deploy and an install that just failed.  This probe links the same modules the install
   path uses -- container.*, ncz.* (tables and the block questions), ncz_decode.* (the decoder) --
   and runs them against a file on disk, printing how many bytes came out and their sha256.  A
   second, independent decode (for example `tools/ncz-probe.py`, which shells out to `zstd` per
   block) printing the same two numbers is what proves the decoding, the block bookkeeping and the
   last block's remainder are right before any hardware is involved.

   Usage: tools/ncz-probe.sh <container> [entry index]     (builds this file, then runs it)

   It only reads: no temporary files, no network, no keys.  Entry index defaults to 0, which on a
   `.xcz` is its program content (the biggest `.ncz`); the probe lists every content first. */

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "install/container.hpp"
#include "install/ncz.hpp"
#include "install/ncz_decode.hpp"
#include "util/sha256.hpp"

namespace {

/* A std::FILE behind the container interface: the probe has to read like a source does. */
class FileReader final : public acnh_manager::install::ContainerReader {
public:
    explicit FileReader(const char *path) : m_file(std::fopen(path, "rb")) {}
    ~FileReader() {
        if (m_file != nullptr) {
            std::fclose(m_file);
        }
    }
    FileReader(const FileReader &) = delete;
    FileReader &operator=(const FileReader &) = delete;

    bool Ok() const { return m_file != nullptr; }

    bool Read(std::uint64_t offset, std::size_t size, std::uint8_t *out) override {
        if (size == 0) {
            return true;
        }
        if (std::fseek(m_file, static_cast<long>(offset), SEEK_SET) != 0) {
            return false;
        }
        return std::fread(out, 1, size, m_file) == size;
    }

private:
    std::FILE *m_file{nullptr};
};

std::uint64_t FileSize(const char *path) {
    std::FILE *file = std::fopen(path, "rb");
    if (file == nullptr) {
        return 0;
    }
    std::fseek(file, 0, SEEK_END);
    const std::uint64_t size = static_cast<std::uint64_t>(std::ftell(file));
    std::fclose(file);
    return size;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        std::printf("usage: %s <container> [entry index]\n", argv[0]);
        return 2;
    }
    const std::size_t wanted = argc > 2 ? static_cast<std::size_t>(std::atoi(argv[2])) : 0;
    const std::uint64_t container_size = FileSize(argv[1]);
    FileReader reader(argv[1]);
    if (!reader.Ok() || container_size == 0) {
        std::printf("cannot read %s\n", argv[1]);
        return 1;
    }

    acnh_manager::install::ContainerInfo container;
    std::string error;
    if (!acnh_manager::install::OpenContainer(reader, argv[1], container_size, &container, &error)) {
        std::printf("OpenContainer: %s\n", error.c_str());
        return 1;
    }
    std::printf("container: %zu contents, compressed=%d\n", container.contents.size(),
                container.compressed ? 1 : 0);
    for (std::size_t i = 0; i < container.contents.size(); ++i) {
        const auto &entry = container.contents[i];
        std::printf("  [%zu] %s offset=0x%llx packed=%llu size=%llu compressed=%d\n", i,
                    entry.name.c_str(), static_cast<unsigned long long>(entry.offset),
                    static_cast<unsigned long long>(entry.packed_size),
                    static_cast<unsigned long long>(entry.size), entry.compressed ? 1 : 0);
    }
    if (wanted >= container.contents.size()) {
        std::printf("no entry %zu\n", wanted);
        return 1;
    }
    const auto &entry = container.contents[wanted];
    if (!entry.compressed) {
        std::printf("entry %zu is a plain NCA: nothing to decode\n", wanted);
        return 1;
    }

    /* The reads the app does before a transfer starts (GameInstall::ReadNczInfo): the fixed header,
       then the tables -- whose size is known apart from a block table's, hence the probe sized from
       the entry. */
    std::vector<std::uint8_t> head(acnh_manager::install::kNczHeaderSize + 16);
    if (!reader.Read(entry.offset, head.size(), head.data())) {
        std::printf("cannot read the compressed content's header\n");
        return 1;
    }
    acnh_manager::install::NczHeaderInfo header;
    if (!acnh_manager::install::ParseNczHeader(head.data(), head.size(), &header, &error)) {
        std::printf("ParseNczHeader: %s\n", error.c_str());
        return 1;
    }
    const std::uint64_t probe =
        header.section_count * acnh_manager::install::kNczSectionSize + 24 +
        (entry.packed_size / (1u << 14) + 1) * 4;
    std::vector<std::uint8_t> tables(static_cast<std::size_t>(probe));
    if (!reader.Read(entry.offset + header.tables_offset, tables.size(), tables.data())) {
        std::printf("cannot read the compressed content's tables\n");
        return 1;
    }
    acnh_manager::install::NczInfo ncz;
    if (!acnh_manager::install::ParseNczTables(tables.data(), tables.size(), header,
                                               entry.packed_size, &ncz, &error)) {
        std::printf("ParseNczTables: %s\n", error.c_str());
        return 1;
    }
    std::printf("ncz: sections=%zu blocks=%zu block size=%u decompressed=%llu nca size=%llu\n",
                ncz.sections.size(), ncz.block_offsets.size(), ncz.block_size,
                static_cast<unsigned long long>(ncz.block_decompressed_size),
                static_cast<unsigned long long>(ncz.nca_size));

    /* The payload, decoded the way the install does it: 4 MB at a time, forward only. */
    acnh_manager::install::NczDecoder decoder(ncz, reader, entry.offset);
    acnh_manager::util::Sha256 hash;
    std::vector<std::uint8_t> buffer(4 * 1024 * 1024);
    std::uint64_t at = acnh_manager::install::kNczHeaderSize;
    while (at < ncz.nca_size) {
        const std::uint64_t want = std::min<std::uint64_t>(buffer.size(), ncz.nca_size - at);
        if (!decoder.DecodeUntil(at + want, &error)) {
            std::printf("DecodeUntil at %llu: %s\n", static_cast<unsigned long long>(at),
                        error.c_str());
            return 1;
        }
        std::size_t took = 0;
        if (!decoder.Take(at, static_cast<std::size_t>(want), buffer.data(), &took, &error) ||
            took == 0) {
            std::printf("Take at %llu: %s\n", static_cast<unsigned long long>(at), error.c_str());
            return 1;
        }
        hash.Update(buffer.data(), took);
        at += took;
        if ((at & ((1u << 28) - 1)) < buffer.size()) {
            std::printf("  ... %llu / %llu\n", static_cast<unsigned long long>(at),
                        static_cast<unsigned long long>(ncz.nca_size));
            std::fflush(stdout);
        }
    }
    std::printf("decoded %llu bytes sha256=%s\n",
                static_cast<unsigned long long>(at - acnh_manager::install::kNczHeaderSize),
                acnh_manager::util::Sha256::ToHex(hash.Finish()).c_str());
    return 0;
}
