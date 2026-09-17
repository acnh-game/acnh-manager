#pragma once

/* Atmosphere `override_config.ini` 与 per-title `config.ini` 的语义解析(纯逻辑,可主机测试)。

   背景(源码依据见 docs/architecture.md 与 ../../../docs/acnh_manager_plan.md):
   - exefs 覆盖是否生效由 OverrideStatus 决定,而它来自 `override_key`:
       `!L` → by_default=true(默认生效,按住 L 反而关闭)
       `L`  → by_default=false(默认关闭,必须按住 L 才生效)
       缺省 → 代码默认 {key=L, by_default=true}
       显式空值 → key_combination=0、by_default=false → 永不生效
   - per-title `[override_config] override_key` 覆盖全局默认;
   - `[hbl_config] override_any_app` + `override_any_app_key`(默认 R)表示"按住该键启动任何应用会进 hbmenu"。 */

#include <cstdint>
#include <string>
#include <string_view>

namespace acnh_manager::env {

struct OverrideKey {
    std::string key;          /* "L"、"R"…;空表示没有可用组合键 */
    bool by_default{true};    /* true: 默认生效,按住键关闭 */
    bool specified{false};    /* 文件里是否显式写了这一项 */
};

struct OverrideConfig {
    OverrideKey global_default;    /* [default_config] override_key */
    OverrideKey title_override;    /* per-title [override_config] override_key */
    bool hbl_any_app{false};       /* [hbl_config] override_any_app */
    OverrideKey hbl_any_app_key;   /* [hbl_config] override_any_app_key */
};

/* 解析 Atmosphere 的 override_config.ini 文本(只取上面用到的键)。 */
OverrideConfig ParseOverrideConfig(std::string_view text);

/* 解析 per-title config.ini(只取 [override_config] override_key)。 */
OverrideKey ParseTitleConfig(std::string_view text);

/* 把"覆盖是否生效、要不要按键"翻译成人话,给状态页直接用。 */
struct OverrideAdvice {
    bool effective_by_default{true}; /* 不按键时覆盖是否生效 */
    std::string key;                 /* 需要/需要避免按住的键 */
    bool never_applies{false};       /* override_key 显式为空 */
    std::string text;                /* 面向玩家的中文说明 */
};

OverrideAdvice Advise(const OverrideConfig &config, const OverrideKey &title);

}  // namespace acnh_manager::env
