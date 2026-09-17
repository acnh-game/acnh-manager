#pragma once

#include <cstdint>

/* Semantic parsing of Atmosphere's `override_config.ini` and per-title `config.ini`
   (pure logic, host-testable).
   Background (sources: docs/architecture.md and ../../../docs/acnh_manager_plan.md):
   - whether the exefs override applies is decided by OverrideStatus, which comes from
     `override_key`:
       `!L` -> by_default=true (applies unless L is held)
       `L`  -> by_default=false (does not apply unless L is held)
       missing -> code default {key=L, by_default=true}
       explicitly empty -> key_combination=0, by_default=false -> never applies
   - a per-title `[override_config] override_key` overrides the global default;
   - `[hbl_config] override_any_app` + `override_any_app_key` (default R) mean "holding this
     key while launching an *application* opens hbmenu instead".  Atmosphere only treats the
     application range (0x0100000000010000..0x01FFFFFFFFFFFFFF) as applications, so games are
     covered while system applets (eShop, news, settings, ...) are not -- measured on hardware. */
#include <string>
#include <string_view>

namespace acnh_manager::env {

struct OverrideKey {
    std::string key;          /* "L", "R", ...; empty means no usable combination */
    bool by_default{true};    /* true: applies by default, hold the key to disable */
    bool specified{false};    /* whether the file set this entry explicitly */
};

struct OverrideConfig {
    OverrideKey global_default;    /* [default_config] override_key */
    OverrideKey title_override;    /* per-title [override_config] override_key */
    bool hbl_any_app{false};       /* [hbl_config] override_any_app */
    OverrideKey hbl_any_app_key;   /* [hbl_config] override_any_app_key */
};

/* Parse Atmosphere's override_config.ini text (only the keys used above). */
OverrideConfig ParseOverrideConfig(std::string_view text);

/* Parse a per-title config.ini (only [override_config] override_key). */
OverrideKey ParseTitleConfig(std::string_view text);

/* Turn "does the override apply, and which key matters" into a sentence for the status page. */
struct OverrideAdvice {
    bool effective_by_default{true}; /* does the override apply without holding anything */
    std::string key;                 /* key to hold (or avoid holding) */
    bool never_applies{false};       /* override_key explicitly empty */
    std::string text;                /* player-facing sentence (localized by the i18n table) */
};

OverrideAdvice Advise(const OverrideConfig &config, const OverrideKey &title);

}  // namespace acnh_manager::env
