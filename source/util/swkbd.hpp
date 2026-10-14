#pragma once

/* The system software keyboard, for the one line of text this app ever needs from the player: the
   address of a package to download.

   It is the *system's* keyboard, so the player gets their own language, their own layout, a
   touch-and-controller interface and the clipboard -- none of which this app has, and all of which
   the reference implementation borrows the same way. */

#include <string>

namespace acnh_manager::util {

/* Opens the keyboard.  `initial` is what the field starts with (the scheme, so the player only
   types the rest), `hint` is the line the keyboard shows above its keys.  Returns false when the
   player cancels or the keyboard cannot be shown at all -- `error` then says which, in English, for
   the log. */
bool AskForLine(const std::string &initial, const std::string &hint, std::string *out,
                std::string *error);

}  // namespace acnh_manager::util
