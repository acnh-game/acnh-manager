#include "config_ini.hpp"

#include <algorithm>
#include <cctype>

#include "i18n/strings.hpp"

namespace acnh_manager::env {
namespace {

std::string_view Trim(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
        text.remove_suffix(1);
    }
    return text;
}

std::string Lower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

OverrideKey ParseKey(std::string_view value) {
    OverrideKey key;
    key.specified = true;
    value = Trim(value);
    if (!value.empty() && value.front() == '!') {
        key.by_default = true;
        value.remove_prefix(1);
    } else {
        key.by_default = false;
    }
    std::string name = Lower(Trim(value));
    if (name == "a") {
        key.key = "A";
    } else if (name == "b") {
        key.key = "B";
    } else if (name == "x") {
        key.key = "X";
    } else if (name == "y") {
        key.key = "Y";
    } else if (name == "l") {
        key.key = "L";
    } else if (name == "r") {
        key.key = "R";
    } else if (name == "zl") {
        key.key = "ZL";
    } else if (name == "zr") {
        key.key = "ZR";
    } else if (name == "ls") {
        key.key = "LS";
    } else if (name == "rs") {
        key.key = "RS";
    } else if (name == "plus") {
        key.key = "PLUS";
    } else if (name == "minus") {
        key.key = "MINUS";
    } else if (name == "dleft") {
        key.key = "DLEFT";
    } else if (name == "dright") {
        key.key = "DRIGHT";
    } else if (name == "dup") {
        key.key = "DUP";
    } else if (name == "ddown") {
        key.key = "DDOWN";
    } else {
        /* Unknown or empty value: no usable combination. */
        key.key.clear();
    }
    return key;
}

struct SectionKey {
    std::string section;
    std::string name;
    std::string value;
};

template <typename Handler>
void ForEachEntry(std::string_view text, Handler handler) {
    std::size_t pos = 0;
    std::string section;
    while (pos <= text.size()) {
        const std::size_t end = text.find('\n', pos);
        std::string_view line =
            end == std::string_view::npos ? text.substr(pos) : text.substr(pos, end - pos);
        pos = end == std::string_view::npos ? text.size() + 1 : end + 1;
        line = Trim(line);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (line.empty() || line.front() == ';' || line.front() == '#') {
            continue;
        }
        if (line.front() == '[' && line.back() == ']') {
            section = Lower(Trim(line.substr(1, line.size() - 2)));
            continue;
        }
        const std::size_t equals = line.find('=');
        if (equals == std::string_view::npos) {
            continue;
        }
        handler(SectionKey{section, Lower(Trim(line.substr(0, equals))),
                           std::string(Trim(line.substr(equals + 1)))});
    }
}

}  // namespace

OverrideConfig ParseOverrideConfig(std::string_view text) {
    OverrideConfig config;
    /* Code default: hold L to disable the override (so it applies by default). */
    config.global_default = OverrideKey{"L", true, false};
    config.hbl_any_app = true; /* Atmosphere's code default is override_any_app = true */
    config.hbl_any_app_key = OverrideKey{"R", false, false};
    ForEachEntry(text, [&config](const SectionKey &entry) {
        if (entry.section == "default_config") {
            if (entry.name == "override_key") {
                config.global_default = ParseKey(entry.value);
            }
        } else if (entry.section == "hbl_config") {
            if (entry.name == "override_any_app") {
                const std::string value = Lower(entry.value);
                config.hbl_any_app = value == "1" || value == "true";
            } else if (entry.name == "override_any_app_key") {
                config.hbl_any_app_key = ParseKey(entry.value);
            }
        }
    });
    return config;
}

OverrideKey ParseTitleConfig(std::string_view text) {
    OverrideKey key;
    ForEachEntry(text, [&key](const SectionKey &entry) {
        if (entry.section == "override_config" && entry.name == "override_key") {
            key = ParseKey(entry.value);
        }
    });
    return key;
}

OverrideAdvice Advise(const OverrideConfig &config, const OverrideKey &title) {
    const OverrideKey &key = title.specified ? title : config.global_default;
    OverrideAdvice advice;
    if (key.key.empty()) {
        /* With no key combination, by_default decides everything: always on, or never. */
        advice.effective_by_default = key.by_default;
        advice.never_applies = !key.by_default;
        advice.text = i18n::Text(key.by_default ? i18n::StringId::OverrideAlwaysOn
                                                : i18n::StringId::OverrideNeverApplies,
                                 i18n::Current());
        return advice;
    }
    advice.key = key.key;
    advice.effective_by_default = key.by_default;
    advice.text = i18n::Format(key.by_default ? i18n::StringId::OverrideOnByDefault
                                              : i18n::StringId::OverrideOffByDefault,
                               key.key.c_str());
    if (config.hbl_any_app && !config.hbl_any_app_key.key.empty()) {
        advice.text +=
            i18n::Format(i18n::StringId::OverrideHbmenuNote, config.hbl_any_app_key.key.c_str());
    }
    return advice;
}

}  // namespace acnh_manager::env
