#pragma once

/* Which of a package's names the screens show, and in which order.

   A package carries its name in up to sixteen languages, and the console answers for the language
   in *its* settings -- not the one this app is set to.  A player who set the app to English on a
   console set to Chinese would otherwise read the game's Chinese name on the confirm screen and
   conclude that the app had ignored the setting.  So the app's own language names the package (the
   choice in front of the player wins), and the console's answer becomes a second line whenever it
   says something else -- that is the spelling the home menu shows.  Printing both also does what a
   title listing is for: the two spellings are how one recognizes the same cartridge.

   Pure logic, no libnx: the layouts below are the NACP's own slot order (`NacpStruct::lang[]`),
   which is *not* the console's language order -- libnx's own `nacpGetLanguageEntry` table is the
   same mapping.  `tests/host_tests.cpp` pins the choices. */

#include <array>
#include <string>

#include "i18n/strings.hpp"

namespace acnh_manager::install {

inline constexpr int kNacpLangCount = 16;

/* Slot numbers that matter to this app, spelled out rather than written inline: the numbers are
   the format's, and a typo here would silently read the wrong language. */
inline constexpr int kNacpAmericanEnglish = 0;
inline constexpr int kNacpBritishEnglish = 1;
inline constexpr int kNacpTraditionalChinese = 13;
inline constexpr int kNacpSimplifiedChinese = 14;

/* `primary` names the package; `alternate` is the second line under it, empty when there is
   nothing to add. */
struct TitleNames {
    std::string primary;
    std::string alternate;
};

/* `languages` is one name per NACP slot (empty string = the slot is blank); `console_name` is what
   the console's own setting produced, empty when it produced nothing. */
TitleNames ChooseTitleNames(const std::array<std::string, kNacpLangCount> &languages,
                            const std::string &console_name, i18n::Language app_language);

}  // namespace acnh_manager::install
