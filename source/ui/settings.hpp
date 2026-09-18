#pragma once

/* Player settings that have to survive a restart: `/switch/ACNH-Manager/settings.json`.

   Deliberately *not* part of state.json: that file is the install record, and uninstall deletes
   it -- the interface language has to outlive an uninstall.  Serialised with the project's own
   JSON module (`source/util/json.hpp`, the same one state.json uses); parse/dump are pure,
   so the host tests pin the round trip exactly like they do for the install record. */

#include <string>
#include <string_view>

#include "i18n/strings.hpp"

namespace acnh_manager::ui {

struct Settings {
    i18n::Language language{i18n::Language::ZhHans};
};

std::string DumpSettings(const Settings &settings);

/* Returns false when the text is not a settings file we can use (the caller then keeps its own
   defaults), and true when it is -- including the case where one *value* was unusable, which is
   reported through `error` while the previous value stays in place.  A missing file is the
   caller's business, not an error here. */
bool ParseSettings(std::string_view text, Settings *settings, std::string *error);

}  // namespace acnh_manager::ui
