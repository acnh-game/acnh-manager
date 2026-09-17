#pragma once

/* Self-contained SHA-256 (pure C++17, no libnx):
   the install engine uses it to verify payloads and installed files byte by byte, and the
   host tests can run it directly. */
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace acnh_manager::util {

class Sha256 {
public:
    Sha256() { Reset(); }
    void Reset();
    void Update(const void *data, std::size_t size);
    std::array<std::uint8_t, 32> Finish();

    /* Lower-case hexadecimal output. */
    static std::string ToHex(const std::array<std::uint8_t, 32> &digest);

private:
    void ProcessBlock(const std::uint8_t *block);

    std::array<std::uint32_t, 8> m_state{};
    std::array<std::uint8_t, 64> m_buffer{};
    std::size_t m_buffer_size{0};
    std::uint64_t m_total_size{0};
};

/* One-shot helper; returns lower-case hex. */
std::string Sha256Hex(const void *data, std::size_t size);
std::string Sha256Hex(const std::string &data);

}  // namespace acnh_manager::util
