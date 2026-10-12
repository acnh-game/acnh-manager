#include "install/nca_crypto.hpp"

#include <cstdint>

namespace acnh_manager::install {
namespace {

/* Both values are the well-known inputs of Nintendo's key-derivation chain: the "header kek
   source" is fed to spl to get the key-encryption key, and "header key source" is what that KEK
   wraps to produce the actual header key.  They are public constants, not secrets -- the secret
   part stays inside the console's spl service. */
constexpr unsigned char kHeaderKekSource[0x10] = {0x1F, 0x12, 0x91, 0x3A, 0x4A, 0xCB, 0xF0, 0x0D,
                                                  0x4C, 0xDE, 0x3A, 0xF6, 0xD5, 0x23, 0x88, 0x2A};
constexpr unsigned char kHeaderKeySource[0x20] = {
    0x5A, 0x3E, 0xD8, 0x4F, 0xDE, 0xC0, 0xD8, 0x26, 0x31, 0xF7, 0xE2, 0x5D, 0x19, 0x7B, 0xF5, 0xD0,
    0x1C, 0x9B, 0x7B, 0xFA, 0xF6, 0x28, 0x18, 0x3D, 0x71, 0xF6, 0x4D, 0x73, 0xF1, 0x50, 0xB9, 0xD2};

/* The NCA header is one XTS unit: sector 0 of the file, decrypted as a single 0xC00-byte region.
   `is_nintendo` picks the console's own tweak layout. */
bool DeriveHeaderKey(unsigned char key[0x20], std::string *error) {
    const Result rc = splCryptoInitialize();
    if (R_FAILED(rc)) {
        *error = "cannot open the spl service for the NCA header key";
        return false;
    }
    unsigned char kek[0x20];
    Result step = splCryptoGenerateAesKek(kHeaderKekSource, 0, 0, kek);
    if (R_SUCCEEDED(step)) {
        step = splCryptoGenerateAesKey(kek, kHeaderKeySource, key);
    }
    if (R_SUCCEEDED(step)) {
        step = splCryptoGenerateAesKey(kek, kHeaderKeySource + 0x10, key + 0x10);
    }
    splCryptoExit();
    if (R_FAILED(step)) {
        *error = "cannot derive the NCA header key from spl";
        return false;
    }
    return true;
}

/* The header is decrypted **one 0x200-byte sector at a time, resetting the tweak for each**,
   exactly like the reference implementation.  Handing the whole 0xC00 bytes to one XTS call
   looks equivalent but is not: the hardware block increments the tweak per 16-byte block, while
   the format increments it per sector -- so the first sector came out right and everything after
   it was noise (measured on hardware: a complete 2.0.0 package was rejected as "not an NCA3
   header", which is simply the magic at 0x200 landing after the first sector). */
bool Transform(const void *in, void *out, bool encrypt, std::string *error) {
    constexpr std::size_t kNcaSectorSize = 0x200;
    unsigned char key[0x20];
    if (!DeriveHeaderKey(key, error)) {
        return false;
    }
    Aes128XtsContext ctx;
    aes128XtsContextCreate(&ctx, key, key + 0x10, encrypt);
    const auto *src = static_cast<const std::uint8_t *>(in);
    auto *dst = static_cast<std::uint8_t *>(out);
    std::uint64_t sector = 0;
    for (std::size_t pos = 0; pos < kNcaHeaderSize; pos += kNcaSectorSize) {
        aes128XtsContextResetSector(&ctx, sector++, true);
        if (encrypt) {
            aes128XtsEncrypt(&ctx, dst + pos, src + pos, kNcaSectorSize);
        } else {
            aes128XtsDecrypt(&ctx, dst + pos, src + pos, kNcaSectorSize);
        }
    }
    return true;
}

}  // namespace

bool DecryptNcaHeader(const void *in, void *out, std::string *error) {
    return Transform(in, out, false, error);
}

bool EncryptNcaHeader(const void *in, void *out, std::string *error) {
    return Transform(in, out, true, error);
}

}  // namespace acnh_manager::install
