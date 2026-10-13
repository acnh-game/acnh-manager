#pragma once

/* Byte counts as the one-line text the UI shows ("4.3 GB", "352 KB").

   The units are binary (1024-based) and named the way the rest of the app already names them
   (the confirm screen's "大小" line, the storage bars): a package that is 4,650,239,771 bytes is
   the "4.3 GB" the player sees elsewhere, so the file picker and the confirm screen cannot
   disagree about the same file.  Nothing here is localized: the digits and the unit symbols read
   the same in both languages the app ships. */

#include <cstdint>
#include <cstdio>
#include <string>

namespace acnh_manager::util {

inline std::string FormatSize(std::uint64_t bytes) {
    constexpr double kKiB = 1024.0;
    constexpr double kMiB = kKiB * 1024.0;
    constexpr double kGiB = kMiB * 1024.0;
    char buffer[32];
    if (static_cast<double>(bytes) >= kGiB) {
        std::snprintf(buffer, sizeof(buffer), "%.1f GB", static_cast<double>(bytes) / kGiB);
    } else if (static_cast<double>(bytes) >= kMiB) {
        std::snprintf(buffer, sizeof(buffer), "%.1f MB", static_cast<double>(bytes) / kMiB);
    } else if (bytes >= 1024) {
        std::snprintf(buffer, sizeof(buffer), "%llu KB",
                      static_cast<unsigned long long>(bytes / 1024));
    } else {
        std::snprintf(buffer, sizeof(buffer), "%llu B", static_cast<unsigned long long>(bytes));
    }
    return buffer;
}

}  // namespace acnh_manager::util
