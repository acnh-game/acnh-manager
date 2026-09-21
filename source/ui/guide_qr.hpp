#pragma once

/* The mini-app's code, as a module matrix the UI can paint.

   `tools/make-qr.py` recovers the grid from the published code image and writes it to
   `data/qr_miniapp.bin` (bin2s turns that into `.rodata`, like the payload and the signing key):
   little-endian u16 size, u16 size, then `size` rows of `size` bits, MSB first.  The app only
   needs to know which cells are black -- it fills those with the rectangle painter it already
   has, so there is no image decoder in the console build at all. */

#include <cstdint>
#include <vector>

namespace acnh_manager::ui {

class QrMatrix {
public:
    /* Reads the embedded asset; an empty matrix means the build carries no code (the guide page
       then shows the text and the address without the picture). */
    static QrMatrix FromEmbedded();

    bool Empty() const { return m_size <= 0; }
    int Size() const { return m_size; }
    /* Row-major, MSB first.  Out-of-range coordinates read as white. */
    bool At(int row, int column) const;

private:
    int m_size{0};
    std::vector<std::uint8_t> m_rows;
};

}  // namespace acnh_manager::ui
