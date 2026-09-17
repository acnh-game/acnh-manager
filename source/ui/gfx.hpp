#pragma once

/* Tiny drawing layer on top of the libnx framebuffer: rectangles and strokes only, text
   goes through Font.  Pixel format is RGBA8 (fixed by framebufferCreate).

   Clipping: a Surface carries a clip rectangle and every draw is intersected with it.
   - Clipped(x,y,w,h): same coordinate system (absolute pixels), just draws less -- used to
     keep a whole page inside its bounds;
   - Subview(x,y,w,h): moves the origin to the rectangle's top-left, used to lock content
     inside a card or list.
   Both intersect with the existing clip, so a page-level "above the footer" limit reaches
   every subview automatically. */

#include <switch.h>

namespace acnh_manager::ui {

struct Color {
    u8 r{0};
    u8 g{0};
    u8 b{0};
    u8 a{255};
};

inline constexpr Color MakeColor(u8 r, u8 g, u8 b, u8 a = 255) { return Color{r, g, b, a}; }

/* stride is in pixels (linear buffer after framebufferMakeLinear). */
struct Surface {
    u32 *pixels{nullptr};
    int width{0};
    int height{0};
    int stride{0};
    /* Clip rectangle (pixels, right/bottom exclusive).  w <= 0 or h <= 0 means "no clip". */
    int clip_x{0};
    int clip_y{0};
    int clip_w{0};
    int clip_h{0};

    /* Copy with drawing additionally limited to (x, y, w, h); any existing clip is intersected. */
    Surface Clipped(int x, int y, int w, int h) const;

    /* Subview whose origin is (x, y): the pixel pointer moves and the size shrinks to the
       intersection with the existing clip.  The difference from Clipped() is the coordinate
       system: coordinates inside a subview are **local** (0,0 = its top-left), which makes
       "lock this content into that rectangle" direct and impossible to lose by forgetting an
       origin conversion.  An empty view (out of range or zero size) returns pixels == nullptr,
       so later draws are dropped. */
    Surface Subview(int x, int y, int w, int h) const;
};

/* Effective drawing range: the surface bounds intersected with the clip rectangle. */
struct Bounds {
    int x0{0};
    int y0{0};
    int x1{0};
    int y1{0};
    bool Empty() const { return x0 >= x1 || y0 >= y1; }
};

Bounds DrawBounds(Surface surface);

void Fill(Surface surface, Color color);
void FillRect(Surface surface, int x, int y, int w, int h, Color color);
void StrokeRect(Surface surface, int x, int y, int w, int h, int thickness, Color color);
void BlendPixel(Surface surface, int x, int y, Color color);

}  // namespace acnh_manager::ui
