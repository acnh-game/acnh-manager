#pragma once

/* What the footer says about the running build.

   A clean release shows a human-readable application version.  Development, dirty and manual
   builds keep their full build stamp so a tester can identify the exact source tree. */

#include <cstdio>
#include <string>
#include <string_view>

#include "i18n/strings.hpp"

namespace acnh_manager::ui {

inline bool IsReleaseBuildStamp(std::string_view build_stamp) {
    return build_stamp.find("+release") != std::string_view::npos &&
           build_stamp.find("+devaids") == std::string_view::npos &&
           build_stamp.find("-dirty") == std::string_view::npos;
}

inline std::string FooterVersionText(std::string_view build_stamp, std::string_view app_version,
                                     i18n::Language language) {
    if (!IsReleaseBuildStamp(build_stamp)) {
        return std::string(build_stamp);
    }
    const std::string version(app_version);
    char text[96];
    std::snprintf(text, sizeof(text), i18n::Text(i18n::StringId::FooterVersion, language),
                  version.c_str());
    return text;
}

}  // namespace acnh_manager::ui
