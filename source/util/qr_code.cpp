#include "util/qr_code.hpp"

extern "C" {
#include "qrcodegen.h"
}

namespace acnh_manager::util {

bool QrCode::At(int row, int column) const {
    if (row < 0 || column < 0 || row >= size || column >= size) {
        return false;
    }
    const int index = row * size + column;
    return ((bits[static_cast<std::size_t>(index) / 8] >> (index % 8)) & 1U) != 0U;
}

QrCode EncodeQr(const std::string &text, int ecc) {
    QrCode out;
    if (text.empty() || ecc < 0 || ecc > 3) {
        return out;
    }
    /* Two 4 KB-class buffers on the stack: the encoder is called once per install page and the
       server thread's stack is sized for this (design section 3.1). */
    std::uint8_t temp[qrcodegen_BUFFER_LEN_MAX];
    std::uint8_t code[qrcodegen_BUFFER_LEN_MAX];
    if (!qrcodegen_encodeText(text.c_str(), temp, code, static_cast<enum qrcodegen_Ecc>(ecc),
                              qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO,
                              true)) {
        return out; /* does not fit in any version, or the buffers were too small */
    }
    const int size = qrcodegen_getSize(code);
    if (size <= 0) {
        return out;
    }
    out.size = size;
    const int byte_count = out.ByteCount();
    out.bits.assign(static_cast<std::size_t>(byte_count), 0);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            if (qrcodegen_getModule(code, x, y)) {
                const int index = y * size + x;
                out.bits[static_cast<std::size_t>(index) / 8] |= static_cast<std::uint8_t>(
                    1U << (index % 8));
            }
        }
    }
    return out;
}

}  // namespace acnh_manager::util
