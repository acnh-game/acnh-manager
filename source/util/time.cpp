#include "time.hpp"

#include <cstdio>

namespace acnh_manager::util {

std::string FormatUnixTimeUtc(std::int64_t unix_seconds) {
    std::int64_t days = unix_seconds / 86400;
    std::int64_t remainder = unix_seconds % 86400;
    if (remainder < 0) {
        remainder += 86400;
        --days;
    }
    const int hour = static_cast<int>(remainder / 3600);
    const int minute = static_cast<int>((remainder % 3600) / 60);
    const int second = static_cast<int>(remainder % 60);

    /* Howard Hinnant 的 civil_from_days:把"1970-01-01 起的天数"换算成年月日。 */
    std::int64_t z = days + 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::int64_t y = static_cast<std::int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp < 10 ? mp + 3 : mp - 9;
    const std::int64_t year = y + (m <= 2 ? 1 : 0);

    /* 年份是 long long,给足余量以避开 -Wformat-truncation。 */
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%04lld-%02u-%02uT%02d:%02d:%02dZ",
                  static_cast<long long>(year), m, d, hour, minute, second);
    return buffer;
}

}  // namespace acnh_manager::util
