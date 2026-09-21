#include "guide_qr.hpp"

#include <cstdio>

#include "qr_miniapp_bin.h"

namespace acnh_manager::ui {
namespace {

/* The asset layout, guarded here so a bad build input degrades to "no code" instead of drawing
   nonsense: two little-endian u16 sizes, then ceil(size/8) bytes per row. */
constexpr std::size_t kHeaderBytes = 4;
constexpr int kLargestReasonableSize = 177; /* QR version 40 */

std::uint16_t ReadU16(const std::uint8_t *bytes) {
    return static_cast<std::uint16_t>(bytes[0] | (static_cast<std::uint16_t>(bytes[1]) << 8));
}

}  // namespace

QrMatrix QrMatrix::FromEmbedded() {
    QrMatrix matrix;
    const std::size_t total = qr_miniapp_bin_size;
    if (total < kHeaderBytes) {
        return matrix;
    }
    const std::uint8_t *bytes = qr_miniapp_bin;
    const int width = ReadU16(bytes);
    const int height = ReadU16(bytes + 2);
    if (width <= 0 || width != height || width > kLargestReasonableSize) {
        return matrix;
    }
    const std::size_t row_bytes = static_cast<std::size_t>((width + 7) / 8);
    if (total != kHeaderBytes + row_bytes * static_cast<std::size_t>(height)) {
        return matrix;
    }
    matrix.m_size = width;
    matrix.m_rows.assign(bytes + kHeaderBytes, bytes + total);
    return matrix;
}

bool QrMatrix::At(int row, int column) const {
    if (m_size <= 0 || row < 0 || column < 0 || row >= m_size || column >= m_size) {
        return false;
    }
    const std::size_t row_bytes = static_cast<std::size_t>((m_size + 7) / 8);
    const std::uint8_t byte = m_rows[static_cast<std::size_t>(row) * row_bytes +
                                     static_cast<std::size_t>(column) / 8];
    const int bit = 7 - (column % 8);
    return ((byte >> bit) & 1u) != 0;
}

}  // namespace acnh_manager::ui
