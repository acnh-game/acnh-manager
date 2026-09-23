#pragma once

#include <cstddef>
#include <string>

/* Recognising the cheat file -- and the one entry in it -- that conflicts with the agent
   (pure logic, host-testable).

   Name.  dmnt loads `cheats/<first 8 bytes of the main module id as hex>.txt`, 20 characters,
   inside `/atmosphere/contents/<program id>/`
   (stratosphere/dmnt/source/cheat/impl/dmnt_cheat_api.cpp, CheatProcessManager::LoadCheats).
   For ACNH 3.0.3 the module id leads with `FF1D1C05670DB602`, which is the name found on a real
   card and the one the guide hands out.  Case is not significant: the card is FAT/exFAT, so the
   loader's lowercase lookup finds an uppercase name.

   Content.  A file with that name is not by itself a conflict -- a card can carry a whole pack of
   entries of which only one drives the chat-code path (the file on the author's card has fifteen).
   The entry is therefore identified by its code, not by its name: players rename entries, and the
   name itself carries the activation and the item count, both of which change between guide
   versions.  What does not change is the pair of addresses the code reads -- `main + 0x05255A60`
   (the text-input object) together with `main + 0x5474040` (the first player chain).  That pair is
  specific: the miles entry in the same file reads the player chain on its own, and nothing else in
   the pack reads the text-input object (docs/text_spawn_to_slot1_guide.md, 关键地址).

   The two addresses are per game version.  Regenerating the chat-code cheat -- a new game version,
   new offsets, a rebuilt pack -- moves them, and nothing here would notice: the check would simply
   stop matching and the conflict would go unreported.  Whoever regenerates the cheat updates
   `kTextInputAddress` / `kPlayerChainAddress` and the host fixture in the same change. */
namespace acnh_manager::env {

/* The name half: 16 hex digits followed by ".txt". */
bool IsLegacyCheatFileName(const std::string &name);

/* The chat-code entry of a cheat file's text, or an empty string when it is not in there.
   Matching ignores whitespace, comments and letter case, so a renamed entry is still found. */
std::string ChatCodeCheatEntry(const std::string &text);

}  // namespace acnh_manager::env
