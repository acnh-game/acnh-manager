#pragma once

#include <cstdint>
#include <string>

namespace acnh_manager::util {

/* Unix seconds -> "YYYY-MM-DDTHH:MM:SSZ" (UTC).  A pure function, so it is host-testable. */
std::string FormatUnixTimeUtc(std::int64_t unix_seconds);

}  // namespace acnh_manager::util
