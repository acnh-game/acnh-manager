#pragma once

/* One compressed content inside an NSZ / XCZ: the "NCZ" stream a `.ncz` entry holds.

   An NSZ is an NSP whose large NCAs were replaced by NCZ streams (small or poorly compressible
   ones stay as plain `.nca`), and an XCZ is the same trick inside an XCI's partitions -- which is
   why an install of one of those files has to rebuild the NCA byte for byte before anything can
   be written into a placeholder.

   The layout, read from a real package and proved by rebuilding it (the sha256 of the rebuilt NCA
   is the content id the CNMT names for it; see docs/device-acceptance.md):

       0x0000 .. 0x4000        the NCA header, byte for byte as it sits in a plain NCA
       0x4000 .. 0x4010        NCZ header: magic, then the number of sections
       0x4010 .. 0x4010 + 64N  the sections: the NCA's own file-system sections, each with the
                               key and counter its data was stored with
       ...                     either a block table ("NCZBLOCK" + one size per block) or straight
                               into the payload
       ...                     the payload: zstd, decoding to the NCA from 0x4000 on

   The block table is not optional decoration, it is how the payload is *cut up*: with it, the
   payload is a series of blocks of fixed decompressed size (the last one carrying the remainder
   of `decompressed_size`), each held as a zstd stream -- except the ones compression did not
   shrink, which the dumper stores as the plain bytes.  So a reader that hands the whole payload
   to one zstd stream reads the raw blocks as if they were stream and fails on the first of them
   with "could not be decoded" (measured on a real `.xcz`: its blocks alternate compressed and
   raw).  Without the table the payload really is one stream, which is why both shapes exist.

   Sections whose `crypto_type` is AES-CTR (or above) hold their bytes **decrypted**: the original
   NCA stream is reproduced by encrypting those ranges again with the section's own key and a
   counter that continues with the absolute position -- the same rule the reference implementation
   applies (yati's NCZ reader), and the reason the key travels in the file instead of being derived
   from anything on the console.

   Pure logic: the caller supplies the header bytes, the table bytes and the parsed payload; the
   host tests pin the parsing, the section lookup and the counter arithmetic. */

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace acnh_manager::install {

/* The NCA header size, stored verbatim -- the payload is the NCA from here on. */
inline constexpr std::uint64_t kNczHeaderSize = 0x4000;
inline constexpr std::uint64_t kNczSectionMagic = 0x4E544345535A434EULL;
inline constexpr std::uint64_t kNczBlockMagic = 0x4B434F4C425A434EULL; /* "NCZBLOCK" */
inline constexpr std::uint64_t kNczSectionSize = 64;
/* An NCA has at most four file-system sections, but nothing in the format caps the count: the
   reference implementation only checks that it is non-zero, so this is a sanity bound of ours
   (a count beyond it is a corrupt file, and the table would not fit in the entry anyway). */
inline constexpr std::uint64_t kNczMaxSections = 4096;

/* The NCA encryption types that matter here, spelled out as the header stores them. */
inline constexpr std::uint64_t kNczCryptoNone = 1;
inline constexpr std::uint64_t kNczCryptoAesXts = 2;
inline constexpr std::uint64_t kNczCryptoAesCtr = 3;

struct NczSection {
    std::uint64_t offset{0};
    std::uint64_t size{0};
    std::uint64_t crypto_type{0};
    std::array<std::uint8_t, 16> key{};
    /* The counter's high half as stored; the low half is the absolute block index (see
       NczSectionCounter). */
    std::array<std::uint8_t, 16> counter{};

    /* Stored decrypted, so it has to be encrypted again before the console sees it.  The threshold
       is the reference implementation's: AES-CTR and above. */
    bool Encrypted() const { return crypto_type >= kNczCryptoAesCtr; }
    bool Covers(std::uint64_t nca_offset) const {
        return nca_offset >= offset && nca_offset - offset < size;
    }
    std::uint64_t End() const { return offset + size; }
};

/* The fixed part of the file: what has to be read before the tables can be read. */
struct NczHeaderInfo {
    std::uint64_t section_count{0};
    std::uint64_t tables_offset{0}; /* where the section table starts */
};

struct NczInfo {
    std::vector<NczSection> sections;
    /* Where the payload starts inside the `.ncz`, and how many bytes of it there are. */
    std::uint64_t payload_offset{0};
    std::uint64_t payload_size{0};
    /* The size of the NCA this stream rebuilds: the header plus everything the payload decodes to. */
    std::uint64_t nca_size{0};
    /* The newer layout carries a block table (a reader may seek block by block); this one is the
       straight-stream form, where the payload is a single stream. */
    bool block_table{false};
    std::uint32_t block_size{0};
    std::vector<std::uint64_t> block_offsets; /* absolute inside the `.ncz`, one per block */
    /* What the blocks decode to in total, as the block header declares it: the NCA minus the
       0x4000 header the file stores verbatim (`decompressed_size` in the reference's reader). */
    std::uint64_t block_decompressed_size{0};
};

/* Reads magic + section count out of the 16 bytes at kNczHeaderSize.  `bytes` must hold
   kNczHeaderSize + 16 bytes. */
bool ParseNczHeader(const std::uint8_t *bytes, std::size_t size, NczHeaderInfo *out,
                    std::string *error);

/* Parses the section table and, when present, the block table, and works out where the payload
   starts.  `bytes` is the file from `header.tables_offset` on, `size` how much of it there is,
   `ncz_size` the whole entry's size (so the payload's length is checked against the file). */
bool ParseNczTables(const std::uint8_t *bytes, std::size_t size, const NczHeaderInfo &header,
                    std::uint64_t ncz_size, NczInfo *out, std::string *error);

/* The section that covers `nca_offset`, or nullptr when the position is outside every section
   (the NCA header itself, or a gap between sections -- such bytes are stored as they are). */
const NczSection *FindNczSection(const NczInfo &info, std::uint64_t nca_offset);

/* The block table, as the decode loop reads it.  How many blocks there are is
   `info.block_offsets.size()`; these three answer the per-block questions. */

/* How many bytes block `index` occupies in the `.ncz`: what has to be pulled from the source
   before the block can be decoded.  The blocks are contiguous and immediately before them sits
   the block table, so the last one ends where the entry's payload ends. */
std::uint64_t NczBlockSourceSize(const NczInfo &info, std::uint64_t index);

/* How many bytes block `index` decodes to: the declared block size, except for the last block,
   which carries the remainder of `block_decompressed_size` (a whole block when it divides
   evenly).  This is the reference's rule for the same question. */
std::uint64_t NczBlockDecodedSize(const NczInfo &info, std::uint64_t index);

/* Whether block `index` is a zstd stream.  It is not when compressing it did not help: the dumper
   then writes the plain bytes, whose stored size is exactly what the block decodes to -- so a
   stored size that is not smaller than the decoded size means "copy, do not decode". */
bool NczBlockCompressed(const NczInfo &info, std::uint64_t index);

/* The 16-byte AES counter for `nca_offset`: the section's stored high half, then the absolute
   block index (offset / 16) as a big-endian 64-bit number.  `offset % 16` is not part of the
   counter -- the caller advances the keystream for a range that does not start on a block. */
std::array<std::uint8_t, 16> NczSectionCounter(const NczSection &section, std::uint64_t nca_offset);

}  // namespace acnh_manager::install
