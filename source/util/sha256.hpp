#pragma once

/* 自带的 SHA-256(纯 C++17,不依赖 libnx):
   安装引擎用它逐字节校验 payload 与已安装文件,主机侧也能直接测。 */

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

    /* 十六进制(小写)输出。 */
    static std::string ToHex(const std::array<std::uint8_t, 32> &digest);

private:
    void ProcessBlock(const std::uint8_t *block);

    std::array<std::uint32_t, 8> m_state{};
    std::array<std::uint8_t, 64> m_buffer{};
    std::size_t m_buffer_size{0};
    std::uint64_t m_total_size{0};
};

/* 一次性计算;返回小写十六进制。 */
std::string Sha256Hex(const void *data, std::size_t size);
std::string Sha256Hex(const std::string &data);

}  // namespace acnh_manager::util
