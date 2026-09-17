#include "config_ini.hpp"

#include <algorithm>
#include <cctype>

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
        /* 未知/空值:没有可用组合键。 */
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
    /* 代码默认值:按住 L 关闭覆盖(即默认生效)。 */
    config.global_default = OverrideKey{"L", true, false};
    config.hbl_any_app = true; /* Atmosphere 代码默认 override_any_app = true */
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
        /* 组合键为空时,结果完全由 by_default 决定:true = 始终生效、false = 永不生效。 */
        advice.effective_by_default = key.by_default;
        advice.never_applies = !key.by_default;
        advice.text = key.by_default
                          ? "覆盖始终生效(override_key 没有有效按键)"
                          : "覆盖永不生效(override_key 为空或按键名无法识别):请改成 !L 或删除该项";
        return advice;
    }
    advice.key = key.key;
    advice.effective_by_default = key.by_default;
    if (key.by_default) {
        advice.text = "覆盖默认生效;启动游戏时不要按住 " + key.key;
    } else {
        advice.text = "覆盖默认关闭;启动游戏时需要按住 " + key.key;
    }
    if (config.hbl_any_app && !config.hbl_any_app_key.key.empty()) {
        advice.text += "。注意:按住 " + config.hbl_any_app_key.key + " 启动任何应用会进入 hbmenu";
    }
    return advice;
}

}  // namespace acnh_manager::env
