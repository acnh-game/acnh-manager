#pragma once

/* Decoding one `.ncz` entry into the NCA it rebuilds (install/ncz.hpp has the layout).

   Two things read a compressed content, and both need exactly the same bytes in exactly the same
   order: the install's transfer, which hands them to a placeholder, and the read-out that mounts
   a meta or control content to read the CNMT or the NACP out of it.  One decoder serves both, so
   the shape of a `.ncz` is decided in one place.

   The payload is read strictly forward, which is what the sources can do (a Range request, a file
   seek, a browser upload) and what keeps the state small.  Decoding is per *block* when the file
   carries a block table: an uncompressed block is copied as it is, a compressed one is a zstd
   stream of its own, and the block's decoded size is checked against the table -- a reader that
   feeds the whole payload to one stream instead reads a raw block as compressed data and reports
   "could not be decoded" (measured on a real `.xcz`).

   Re-encrypting the ranges a section stored decrypted is the caller's: nca_crypto owns the AES and
   the counter, this file owns the container.  The bytes handed out here are the payload as it was
   stored in the file. */

#include <zstd.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "install/container.hpp"
#include "install/ncz.hpp"

namespace acnh_manager::install {

class NczDecoder {
public:
    /* `info` is the entry's parsed tables, `reader` the container it sits in, and
       `container_offset` the offset of the `.ncz` entry inside that container (the tables'
       offsets are relative to the entry). */
    NczDecoder(const NczInfo &info, ContainerReader &reader, std::uint64_t container_offset);
    ~NczDecoder();
    NczDecoder(const NczDecoder &) = delete;
    NczDecoder &operator=(const NczDecoder &) = delete;

    /* Decodes forward until the content's byte at `content_offset` (exclusive) is available.  Only
       the bytes asked for are decoded, and only once: each call pulls what it needs from the
       source, so a 5 GB content can be installed on a 600 MB request at a time. */
    bool DecodeUntil(std::uint64_t content_offset, std::string *error);

    /* Hands over up to `size` decoded bytes starting at `content_offset` -- `out_size` says how
       many there were, which is at least one after a successful DecodeUntil for that offset.  The
       bytes leave the decoder for good; a caller that needs a range twice has to keep it. */
    bool Take(std::uint64_t content_offset, std::size_t size, std::uint8_t *out,
              std::size_t *out_size, std::string *error);

    /* --- fed by the caller: the transfer's writer owns the decoder ---

       The transfer reads the payload on one thread and decodes it on another, so that the zstd
       work (3.4 s of a 51 s transfer, measured) stops sitting on the same critical path as the
       network reads.  In this mode the payload arrives in the caller's buffers -- one piece per
       source read -- and the decoded bytes are collected in this object until the caller takes
       them (`Pending` / `Consume`).  The block-table rules, the raw-block copy and the size checks
       are the same code the pull mode runs. */
    bool Feed(const std::uint8_t *payload, std::size_t size, std::string *error);

    /* Decoded bytes not handed out yet, in order (nullptr when there are none).  The pointer stays
       valid until the next Feed or Consume. */
    std::uint8_t *Pending(std::size_t *size);
    void Consume(std::size_t size);

    /* The tables this decoder reads the entry with, for the callers that need the sections (the
       bytes that were stored decrypted have to be encrypted again before they are written). */
    const NczInfo &Info() const { return m_info; }

private:
    /* Pulls `chunk` bytes of payload from the source into m_input. */
    bool Refill(std::string *error);
    /* One decode step over the buffered payload: decodes up to one block's worth and appends what
       it produced.  Shared by both modes. */
    bool Step(std::string *error);

    const NczInfo m_info;
    ContainerReader &m_reader;
    const std::uint64_t m_container_offset;
    ZSTD_DStream *m_dstream{nullptr};
    std::vector<std::uint8_t> m_input; /* payload bytes pulled, not yet consumed */
    std::size_t m_input_pos{0};
    std::uint64_t m_payload_read{0};
    bool m_source_exhausted{false};
    std::vector<std::uint8_t> m_scratch; /* one decode step's output, reused */
    /* Decoded bytes not yet handed out, and where they sit **in the content** -- the payload
       decodes to the content from kNczHeaderSize on, so both start there.  Payload-relative
       numbers here and content-relative ones at the call site is how the first version asked for
       16 KiB more than the stream holds and reported "ended before the whole NCA was decoded" at
       the very end of a 386 MB content. */
    std::vector<std::uint8_t> m_decoded;
    std::uint64_t m_decoded_base{kNczHeaderSize}; /* content offset of m_decoded[0] */
    std::uint64_t m_decoded_end{kNczHeaderSize};  /* content offset past what has been decoded */
    /* Which block the payload is inside (block-table files) and what it has consumed/produced:
       the block ends when everything it holds in the file has been fed, and by then the number of
       decoded bytes has to be the one the table declares. */
    std::uint64_t m_block{0};
    std::uint64_t m_block_consumed{0};
    std::uint64_t m_block_produced{0};
};

}  // namespace acnh_manager::install
