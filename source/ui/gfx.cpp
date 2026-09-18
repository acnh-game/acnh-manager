#include "gfx.hpp"

#include <algorithm>
#include <cmath>

namespace acnh_manager::ui {

Surface Surface::Clipped(int x, int y, int w, int h) const {
    Surface out = *this;
    if (w <= 0 || h <= 0) {
        return out;
    }
    const int x0 = clip_w > 0 ? std::max(clip_x, x) : x;
    const int y0 = clip_h > 0 ? std::max(clip_y, y) : y;
    const int x1 = clip_w > 0 ? std::min(clip_x + clip_w, x + w) : x + w;
    const int y1 = clip_h > 0 ? std::min(clip_y + clip_h, y + h) : y + h;
    out.clip_x = x0;
    out.clip_y = y0;
    out.clip_w = x1 - x0;
    out.clip_h = y1 - y0;
    return out;
}

Bounds DrawBounds(Surface surface) {
    Bounds bounds;
    bounds.x0 = surface.clip_w > 0 ? std::max(0, surface.clip_x) : 0;
    bounds.y0 = surface.clip_h > 0 ? std::max(0, surface.clip_y) : 0;
    bounds.x1 = surface.clip_w > 0 ? std::min(surface.width, surface.clip_x + surface.clip_w)
                                   : surface.width;
    bounds.y1 = surface.clip_h > 0 ? std::min(surface.height, surface.clip_y + surface.clip_h)
                                   : surface.height;
    return bounds;
}

Surface Surface::Subview(int x, int y, int w, int h) const {
    Surface out;
    if (pixels == nullptr || stride <= 0 || w <= 0 || h <= 0) {
        return out;
    }
    const Bounds bounds = DrawBounds(*this);
    const int x0 = std::max(bounds.x0, x);
    const int y0 = std::max(bounds.y0, y);
    const int x1 = std::min(bounds.x1, x + w);
    const int y1 = std::min(bounds.y1, y + h);
    if (x0 >= x1 || y0 >= y1) {
        return out;
    }
    out.pixels = pixels + static_cast<std::ptrdiff_t>(y0) * stride + x0;
    out.stride = stride;
    out.width = x1 - x0;
    out.height = y1 - y0;
    return out;
}

void BlendPixel(Surface surface, int x, int y, Color color) {
    const Bounds bounds = DrawBounds(surface);
    if (x < bounds.x0 || y < bounds.y0 || x >= bounds.x1 || y >= bounds.y1 ||
        surface.pixels == nullptr) {
        return;
    }
    u32 *row = surface.pixels + static_cast<std::ptrdiff_t>(y) * surface.stride;
    u32 &dst = row[x];
    const u32 sr = color.r;
    const u32 sg = color.g;
    const u32 sb = color.b;
    const u32 sa = color.a;
    if (sa >= 255) {
        dst = RGBA8(sr, sg, sb, 255);
        return;
    }
    const u32 dr = dst & 0xFF;
    const u32 dg = (dst >> 8) & 0xFF;
    const u32 db = (dst >> 16) & 0xFF;
    const u32 r = (sr * sa + dr * (255 - sa)) / 255;
    const u32 g = (sg * sa + dg * (255 - sa)) / 255;
    const u32 b = (sb * sa + db * (255 - sa)) / 255;
    dst = RGBA8(r, g, b, 255);
}

void FillRect(Surface surface, int x, int y, int w, int h, Color color) {
    if (surface.pixels == nullptr || w <= 0 || h <= 0) {
        return;
    }
    const Bounds bounds = DrawBounds(surface);
    const int x0 = std::max(bounds.x0, x);
    const int y0 = std::max(bounds.y0, y);
    const int x1 = std::min(bounds.x1, x + w);
    const int y1 = std::min(bounds.y1, y + h);
    if (x0 >= x1 || y0 >= y1) {
        return;
    }
    if (color.a >= 255) {
        const u32 packed = RGBA8(color.r, color.g, color.b, 255);
        for (int row = y0; row < y1; ++row) {
            u32 *pixels = surface.pixels + static_cast<std::ptrdiff_t>(row) * surface.stride;
            for (int col = x0; col < x1; ++col) {
                pixels[col] = packed;
            }
        }
        return;
    }
    for (int row = y0; row < y1; ++row) {
        for (int col = x0; col < x1; ++col) {
            BlendPixel(surface, col, row, color);
        }
    }
}

void StrokeRect(Surface surface, int x, int y, int w, int h, int thickness, Color color) {
    FillRect(surface, x, y, w, thickness, color);
    FillRect(surface, x, y + h - thickness, w, thickness, color);
    FillRect(surface, x, y, thickness, h, color);
    FillRect(surface, x + w - thickness, y, thickness, h, color);
}

/* Rounded rectangle without any new primitive: on a 2D framebuffer a rounded rect is just a
   stack of horizontal spans whose ends follow a quarter circle, so it stays a few FillRect
   calls and inherits the clip handling.  radius is clamped to half the smaller side. */
void FillRoundedRect(Surface surface, int x, int y, int w, int h, int radius, Color color) {
    if (w <= 0 || h <= 0) {
        return;
    }
    const int r = std::min(radius, std::min(w, h) / 2);
    /* Middle band: full width. */
    FillRect(surface, x, y + r, w, h - 2 * r, color);
    /* Top and bottom bands: shrink each row towards the corner. */
    for (int row = 0; row < r; ++row) {
        const int dy = r - row;                       /* distance from the band edge to the centre */
        const int inset = r - static_cast<int>(
                              std::sqrt(static_cast<double>(r * r - dy * dy)) + 0.5);
        FillRect(surface, x + inset, y + row, w - 2 * inset, 1, color);
        FillRect(surface, x + inset, y + h - 1 - row, w - 2 * inset, 1, color);
    }
}

void Fill(Surface surface, Color color) {
    FillRect(surface, 0, 0, surface.width, surface.height, color);
}

}  // namespace acnh_manager::ui
