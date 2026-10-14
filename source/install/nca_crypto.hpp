#pragma once

/* The one piece of crypto the default install path needs: the NCA header key and the header
   itself.

   The header is AES-128-XTS encrypted, and the key is derived from the console's own `spl`
   service, so a package with nothing but Nintendo's own ticket and standard crypto installs
   **without any keys file on the SD card** (design section 6).  The key never leaves these two
   functions: nothing here logs it, and callers only get a decrypted header back.

   Two hard-coded sources come from the public key-derivation chain and match the reference
   implementation (`yati/nx/keys.cpp`); they are inputs to `spl`, not keys themselves. */

#include <switch.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace acnh_manager::install {

/* The decrypted header is 0xC00 bytes: 0x200 bytes of header plus the four file-system headers. */
constexpr std::size_t kNcaHeaderSize = 0xC00;

/* Decrypts `in` (kNcaHeaderSize bytes, straight from the container) into `out`. */
bool DecryptNcaHeader(const void *in, void *out, std::string *error);

/* Re-encrypts a modified header before it is written: an XCI's distribution bit is changed in
   the decrypted header, so the bytes going into the placeholder have to be encrypted again and
   the header's own signature is then stale -- which the default `skip_rsa_*` options accept,
   exactly as the reference implementation does. */
bool EncryptNcaHeader(const void *in, void *out, std::string *error);

/* AES-128-CTR over one range of an NCA's file-system section, for the ranges an NSZ/XCZ stored
   **decrypted** (install/ncz.hpp): the stream cipher is its own inverse, so this is the same call
   the reader uses to put the bytes back the way the console expects them.

   `counter` is the 16-byte block counter that belongs to `nca_offset & ~0xF` (see
   NczSectionCounter); a range that does not start on a block boundary is handled here, by taking
   the keystream of that first partial block and dropping the bytes before the range.  The key and
   counter come from the file itself and are never logged. */
void AesCtrRange(const void *key, const void *counter, std::uint64_t nca_offset, void *data,
                 std::size_t size);

}  // namespace acnh_manager::install
