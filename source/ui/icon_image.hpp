#pragma once

/* Decoding a cover image.

   The console hands out a title's cover as the JPEG the developer shipped (256x256 for every
   retail title, via the same `ns` call the home screen uses).  Nothing in the app can draw a JPEG,
   so this is the one place that turns those bytes into pixels: 8-bit RGBA, top-left origin, ready
   for ui::BlitRgbaScaled.

   The decoder is libjpeg-turbo from the toolchain's portlibs (`-ljpeg`), and it is used in the
   one way that fits a no-exceptions build: its error handler is replaced so a malformed image
   returns false instead of calling exit(). */

#include <cstddef>
#include <cstdint>
#include <vector>

namespace acnh_manager::ui {

struct Bitmap {
    int width{0};
    int height{0};
    std::vector<std::uint8_t> rgba; /* width * height * 4, row-major, top row first */

    bool Empty() const { return width <= 0 || height <= 0 || rgba.empty(); }
};

/* False for anything that is not a decodable image (and for an image larger than 1024x1024, which
   no cover is -- the limit keeps a hostile file from asking for a huge allocation). */
bool DecodeJpeg(const std::uint8_t *data, std::size_t size, Bitmap *out);

}  // namespace acnh_manager::ui
