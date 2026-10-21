#pragma once

/* The home-menu tile's identity, copied from the reference implementation
   (sphaira/source/owo.cpp: install_forwader_internal):

       hash_input = nro_path + (args.empty() ? nro_path : nro_path + ' ' + args)
       title id   = 0x0500000000000000 | (sha256(hash_input)[0..8] & 0x00FFFFFFFFFFF000)

   The empty-argument case is the surprising part: the reference makes the NRO path its own
   argument, so the path appears twice in the hash.  Matching that exactly is what makes a tile
   installed by sphaira and one installed by this app the same tile. */

#include <cstdint>
#include <string_view>

#include "env/startup_check.hpp"

namespace acnh_manager::env {

inline constexpr const char *kForwarderNroPath = kStandardNroPath;
inline constexpr const char *kForwarderAuthor = "acnh-game";
inline constexpr const char *kForwarderName = "ACNH-Manager";

std::uint64_t ForwarderTitleId(std::string_view nro_path, std::string_view args);
std::uint64_t StandardForwarderTitleId();

/* The high byte is 0x05; the following nibbles are part of the hash-derived tile identity. */
bool IsForwarderTitleId(std::uint64_t title_id);

}  // namespace acnh_manager::env
