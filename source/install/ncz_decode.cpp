#include "install/ncz_decode.hpp"

#include <algorithm>
#include <cstring>

namespace acnh_manager::install {
namespace {

/* One read from the source, and the footprint of one decode step.  The transfer path pulls 4 MB
   per range (see game_install.cpp), so half a megabyte here keeps the pulls modest and the status
   polls between them honest. */
constexpr std::size_t kChunk = 512 * 1024;

}  // namespace

NczDecoder::NczDecoder(const NczInfo &info, ContainerReader &reader, std::uint64_t container_offset)
    : m_info(info), m_reader(reader), m_container_offset(container_offset) {}

NczDecoder::~NczDecoder() {
    if (m_dstream != nullptr) {
        ZSTD_freeDStream(m_dstream);
    }
}

bool NczDecoder::Refill(std::string *error) {
    const std::uint64_t left = m_info.payload_size - m_payload_read;
    const std::size_t chunk = static_cast<std::size_t>(left < kChunk ? left : kChunk);
    m_input.resize(chunk);
    m_input_pos = 0;
    /* The payload's offsets are inside the `.ncz`, so the entry's own offset has to be added here:
       reading without it fed the container's own header bytes to zstd, which failed with "could
       not be decoded" on the first real package (measured). */
    if (!m_reader.Read(m_container_offset + m_info.payload_offset + m_payload_read, chunk,
                       m_input.data())) {
        *error = "the source stopped early (connection lost or file truncated)";
        return false;
    }
    m_payload_read += chunk;
    return true;
}

bool NczDecoder::DecodeUntil(std::uint64_t need_until, std::string *error) {
    if (need_until > m_info.nca_size) {
        *error = "the compressed content was asked for bytes past the NCA it rebuilds";
        return false;
    }
    if (m_dstream == nullptr) {
        m_dstream = ZSTD_createDStream();
        if (m_dstream == nullptr || ZSTD_isError(ZSTD_initDStream(m_dstream))) {
            *error = "cannot start the decoder for the compressed content";
            return false;
        }
    }
    if (m_scratch.size() < kChunk) {
        m_scratch.resize(kChunk);
    }
    while (m_decoded_end < need_until) {
        if (m_input_pos >= m_input.size()) {
            if (m_source_exhausted) {
                *error = "the compressed content ended before the whole NCA was decoded";
                return false;
            }
            if (m_payload_read == m_info.payload_size) {
                m_source_exhausted = true;
                continue;
            }
            if (!Refill(error)) {
                return false;
            }
        }
        /* What of the buffered payload belongs to this step: all of it, unless the file carries a
           block table, in which case a step never reaches past the current block. */
        std::size_t span = m_input.size() - m_input_pos;
        bool raw_block = false;
        if (m_info.block_table) {
            if (m_block >= m_info.block_offsets.size()) {
                *error = "the compressed content's payload runs past its block table";
                return false;
            }
            const std::uint64_t left = NczBlockSourceSize(m_info, m_block) - m_block_consumed;
            span = static_cast<std::size_t>(std::min<std::uint64_t>(span, left));
            raw_block = !NczBlockCompressed(m_info, m_block);
        }
        std::size_t produced = 0;
        std::size_t consumed = 0;
        if (raw_block) {
            /* Compressing this block would not have made it smaller, so the file holds the plain
               bytes: they *are* the decoded bytes.  Handing them to zstd is what a reader that
               ignores the block table does, and it fails on exactly these blocks. */
            produced = std::min(span, m_scratch.size());
            std::memcpy(m_scratch.data(), m_input.data() + m_input_pos, produced);
            consumed = produced;
        } else {
            ZSTD_inBuffer in{m_input.data() + m_input_pos, span, 0};
            ZSTD_outBuffer out{m_scratch.data(), m_scratch.size(), 0};
            const std::size_t result = ZSTD_decompressStream(m_dstream, &out, &in);
            if (ZSTD_isError(result)) {
                *error = "the compressed content could not be decoded";
                return false;
            }
            produced = out.pos;
            consumed = in.pos;
        }
        m_input_pos += consumed;
        if (produced > 0) {
            /* The bytes leave as they were stored; the caller puts the ranges the file kept
               decrypted back into their encrypted shape (nca_crypto's ReencryptNczRange). */
            m_decoded.insert(m_decoded.end(), m_scratch.data(), m_scratch.data() + produced);
            m_decoded_end += produced;
        }
        if (m_info.block_table) {
            m_block_consumed += consumed;
            m_block_produced += produced;
            if (m_block_consumed == NczBlockSourceSize(m_info, m_block)) {
                /* The block is in.  What it decodes to is fixed by the block size and, for the last
                   block, by the remainder of the declared total -- so a block that produces
                   anything else means the reader and the file disagree, and saying so here is
                   worth more than the console's answer to a half-rebuilt NCA. */
                if (m_block_produced != NczBlockDecodedSize(m_info, m_block)) {
                    *error =
                        "the compressed content's block does not decode to the size its table "
                        "declares";
                    return false;
                }
                ++m_block;
                m_block_consumed = 0;
                m_block_produced = 0;
            }
        }
        if (produced == 0 && consumed == 0 && m_input_pos >= m_input.size() &&
            m_source_exhausted) {
            *error = "the compressed content ended before the whole NCA was decoded";
            return false;
        }
    }
    /* A stream that decodes past the NCA it claims is a broken file, not a bigger install. */
    if (m_decoded_end > m_info.nca_size) {
        *error = "the compressed content decodes to more than the NCA it claims";
        return false;
    }
    return true;
}

bool NczDecoder::Take(std::uint64_t content_offset, std::size_t size, std::uint8_t *out,
                      std::size_t *out_size, std::string *error) {
    if (content_offset < m_decoded_base) {
        *error = "the compressed content was asked for bytes already handed out";
        return false;
    }
    const std::uint64_t available =
        m_decoded_end > content_offset ? m_decoded_end - content_offset : 0;
    const std::size_t take = static_cast<std::size_t>(std::min<std::uint64_t>(size, available));
    if (take > 0) {
        const std::size_t start = static_cast<std::size_t>(content_offset - m_decoded_base);
        std::memcpy(out, m_decoded.data() + start, take);
        /* Drop what the caller took: the buffer then stays the size of one read, not of the whole
           content. */
        m_decoded.erase(m_decoded.begin(),
                        m_decoded.begin() + static_cast<std::ptrdiff_t>(start + take));
        m_decoded_base = content_offset + take;
    }
    *out_size = take;
    return true;
}

}  // namespace acnh_manager::install
