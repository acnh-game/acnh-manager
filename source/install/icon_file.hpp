#pragma once

/* Which picture inside a package's control content the confirm card shows.

   A control content carries one icon per language, named by the format itself
   ("icon_AmericanEnglish.dat", "icon_SimplifiedChinese.dat", …) -- the same sixteen languages the
   NACP names the title in.  The app's language picks the file, exactly as it picks the name
   (install/title_name.hpp), so the card shows *the package the player is about to install* in the
   language the player reads -- including on a console where that game is not installed at all,
   where asking the system for the game's icon answers "no control data" and used to leave the card
   without a picture.

   Pure logic, no libnx: the caller lists the control content's root and hands the names over;
   `tests/host_tests.cpp` pins the choice. */

#include <string>
#include <vector>

#include "i18n/strings.hpp"

namespace acnh_manager::install {

/* The file the reference implementation falls back to when the wanted language is not there: it is
   the one every package that ships icons at all ships first. */
inline constexpr const char *kIconFallbackFile = "icon_AmericanEnglish.dat";

/* Empty when `files` holds no icon at all (a control content that carries only the NACP, which is
   what a small add-on looks like). */
std::string ChooseIconFile(const std::vector<std::string> &files, i18n::Language app_language);

}  // namespace acnh_manager::install
