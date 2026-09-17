#pragma once

#include <cstdint>
#include <string>

namespace acnh_manager::util {

/* Unix 秒 → "YYYY-MM-DDTHH:MM:SSZ"(UTC)。纯函数,便于主机测试。 */
std::string FormatUnixTimeUtc(std::int64_t unix_seconds);

}  // namespace acnh_manager::util
