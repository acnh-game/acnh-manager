#include "install/ncz_decode.hpp"

#include <algorithm>
#include <cstring>

#include "install/dev_stats.hpp"

namespace acnh_manager::install {
namespace {

/* One read from the source, and the footprint of one decode step.  This is the unit of a network
   request for a compressed content, so it is deliberately as large as the transfer's own block
   (game_install.cpp's 4 MB): at 512 KB the transfer issued ~645 requests for a 337 MB package
   -- a request every ~140 ms, most of it per-request overhead, measured from the server's log --
   while a 4 MB chunk needs one. */
constexpr std::size_t kChunk = 4 * 1024 * 1024;

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
    const std::uint64_t read_begin = dev::NowMs();
    if (!m_reader.Read(m_container_offset + m_info.payload_offset + m_payload_read, chunk,
                       m_input.data())) {
        *error = "the source stopped early (connection lost or file truncated)";
        return false;
    }
    /* Development aid: how long the source itself took (the network, for a download). */
    dev::AddSourceRead(read_begin, chunk);
    m_payload_read += chunk;
    return true;
}

bool NczDecoder::DecodeUntil(std::uint64_t need_until, std::string *error) {
    if (need_until > m_info.nca_size) {
        *error = "the compressed content was asked for bytes past the NCA it rebuilds";
        return false;
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
        if (!Step(error)) {
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

bool NczDecoder::Step(std::string *error) {
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
    const std::uint64_t decode_begin = dev::NowMs();
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
    dev::AddDecode(decode_begin, produced);
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
               block, by the remainder of the declared total -- so a block that produces anything
               else means the reader and the file disagree, and saying so here is worth more than
               the console's answer to a half-rebuilt NCA. */
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
    if (produced == 0 && consumed == 0) {
        /* Neither consumed nor produced anything: the stream cannot make progress, which for a
           payload that is complete means the file is short. */
        *error = "the compressed content ended before the whole NCA was decoded";
        return false;
    }
    return true;
}

bool NczDecoder::Feed(const std::uint8_t *payload, std::size_t size, std::string *error) {
    if (size == 0) {
        return true;
    }
    /* Drop what has been consumed so the buffer stays the size of one piece, then take the new
       payload: from here the decoder is driven by the caller, never by the source. */
    if (m_input_pos > 0) {
        m_input.erase(m_input.begin(), m_input.begin() + static_cast<std::ptrdiff_t>(m_input_pos));
        m_input_pos = 0;
    }
    m_input.insert(m_input.end(), payload, payload + size);
    m_payload_read += size;
    while (m_input_pos < m_input.size()) {
        if (!Step(error)) {
            return false;
        }
    }
    if (m_decoded_end > m_info.nca_size) {
        *error = "the compressed content decodes to more than the NCA it claims";
        return false;
    }
    return true;
}

std::uint8_t *NczDecoder::Pending(std::size_t *size) {
    *size = static_cast<std::size_t>(m_decoded_end - m_decoded_base);
    return m_decoded.empty() ? nullptr : m_decoded.data();
}

void NczDecoder::Consume(std::size_t size) {
    if (size > m_decoded.size()) {
        size = m_decoded.size();
    }
    m_decoded.erase(m_decoded.begin(), m_decoded.begin() + static_cast<std::ptrdiff_t>(size));
    m_decoded_base += size;
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
        const std::uint64_t handout_begin = dev::NowMs();
        const std::size_t start = static_cast<std::size_t>(content_offset - m_decoded_base);
        std::memcpy(out, m_decoded.data() + start, take);
        /* Drop what the caller took: the buffer then stays the size of one read, not of the whole
           content. */
        m_decoded.erase(m_decoded.begin(),
                        m_decoded.begin() + static_cast<std::ptrdiff_t>(start + take));
        m_decoded_base = content_offset + take;
        dev::AddHandout(handout_begin, take);
    }
    *out_size = take;
    return true;
}

}  // namespace acnh_manager::install
