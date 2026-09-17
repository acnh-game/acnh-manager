#include "sha256.hpp"

#include <cstring>

namespace acnh_manager::util {
namespace {

constexpr std::uint32_t kConstants[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u,
};

constexpr std::uint32_t RotateRight(std::uint32_t value, unsigned bits) {
    return (value >> bits) | (value << (32 - bits));
}

}  // namespace

void Sha256::Reset() {
    m_state = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
               0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    m_buffer_size = 0;
    m_total_size = 0;
}

void Sha256::ProcessBlock(const std::uint8_t *block) {
    std::uint32_t w[64]{};
    for (int i = 0; i < 16; ++i) {
        w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24) |
               (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
               (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) |
               static_cast<std::uint32_t>(block[i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i) {
        const std::uint32_t s0 =
            RotateRight(w[i - 15], 7) ^ RotateRight(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const std::uint32_t s1 =
            RotateRight(w[i - 2], 17) ^ RotateRight(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    std::uint32_t a = m_state[0];
    std::uint32_t b = m_state[1];
    std::uint32_t c = m_state[2];
    std::uint32_t d = m_state[3];
    std::uint32_t e = m_state[4];
    std::uint32_t f = m_state[5];
    std::uint32_t g = m_state[6];
    std::uint32_t h = m_state[7];
    for (int i = 0; i < 64; ++i) {
        const std::uint32_t s1 = RotateRight(e, 6) ^ RotateRight(e, 11) ^ RotateRight(e, 25);
        const std::uint32_t ch = (e & f) ^ (~e & g);
        const std::uint32_t temp1 = h + s1 + ch + kConstants[i] + w[i];
        const std::uint32_t s0 = RotateRight(a, 2) ^ RotateRight(a, 13) ^ RotateRight(a, 22);
        const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t temp2 = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }
    m_state[0] += a;
    m_state[1] += b;
    m_state[2] += c;
    m_state[3] += d;
    m_state[4] += e;
    m_state[5] += f;
    m_state[6] += g;
    m_state[7] += h;
}

void Sha256::Update(const void *data, std::size_t size) {
    const auto *bytes = static_cast<const std::uint8_t *>(data);
    m_total_size += size;
    while (size > 0) {
        const std::size_t space = m_buffer.size() - m_buffer_size;
        const std::size_t take = size < space ? size : space;
        std::memcpy(m_buffer.data() + m_buffer_size, bytes, take);
        m_buffer_size += take;
        bytes += take;
        size -= take;
        if (m_buffer_size == m_buffer.size()) {
            ProcessBlock(m_buffer.data());
            m_buffer_size = 0;
        }
    }
}

std::array<std::uint8_t, 32> Sha256::Finish() {
    const std::uint64_t bit_size = m_total_size * 8;
    /* Pad to buffer_size == 56 (mod 64): a 0x80 byte, then zeros, then the 8-byte
       big-endian bit length. */
    std::uint8_t padding[72]{};
    padding[0] = 0x80;
    const std::size_t pad_len =
        m_buffer_size < 56 ? (56 - m_buffer_size) : (120 - m_buffer_size);
    Update(padding, pad_len);
    std::uint8_t length[8]{};
    for (int i = 0; i < 8; ++i) {
        length[7 - i] = static_cast<std::uint8_t>((bit_size >> (i * 8)) & 0xFFu);
    }
    Update(length, sizeof(length));

    std::array<std::uint8_t, 32> digest{};
    for (int i = 0; i < 8; ++i) {
        digest[i * 4] = static_cast<std::uint8_t>(m_state[i] >> 24);
        digest[i * 4 + 1] = static_cast<std::uint8_t>(m_state[i] >> 16);
        digest[i * 4 + 2] = static_cast<std::uint8_t>(m_state[i] >> 8);
        digest[i * 4 + 3] = static_cast<std::uint8_t>(m_state[i]);
    }
    return digest;
}

std::string Sha256::ToHex(const std::array<std::uint8_t, 32> &digest) {
    static const char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (const std::uint8_t byte : digest) {
        out.push_back(digits[byte >> 4]);
        out.push_back(digits[byte & 0xF]);
    }
    return out;
}

std::string Sha256Hex(const void *data, std::size_t size) {
    Sha256 hash;
    hash.Update(data, size);
    return Sha256::ToHex(hash.Finish());
}

std::string Sha256Hex(const std::string &data) { return Sha256Hex(data.data(), data.size()); }

}  // namespace acnh_manager::util
