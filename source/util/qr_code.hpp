#pragma once

/* QR encoding for the install page's address: a thin wrapper around the vendored qrcodegen
   (source/third_party/qrcodegen, Nayuki's C version, MIT).

   The matrix is packed the way qrcodegen packs its own: bit index `row * size + column`, and
   within a byte the lowest bit comes first (that is how the vendor's own `getBit` reads it).  Keeping the vendor's layout means the
   module loop is a straight copy of what `qrcodegen_getModule` returns, with no second packing
   convention to get wrong.  qrcodegen does not add a quiet zone; the caller adds the standard
   four modules of margin when it paints.

   Pure logic, no libnx: the host tests encode the real address and check the finder patterns and
   the version growth. */

#include <cstdint>
#include <string>
#include <vector>

namespace acnh_manager::util {

struct QrCode {
    int size{0};                    /* modules per side; 0 when the text did not fit */
    std::vector<std::uint8_t> bits; /* (size * size + 7) / 8 bytes, MSB first */

    bool Empty() const { return size <= 0; }
    bool At(int row, int column) const;
    int ByteCount() const { return (size * size + 7) / 8; }
};

/* Error correction: 0 = L, 1 = M, 2 = Q, 3 = H (qrcodegen's own order).  A URL on a clean
   screen is fine at M, which is what the caller defaults to. */
QrCode EncodeQr(const std::string &text, int ecc = 1);

}  // namespace acnh_manager::util
