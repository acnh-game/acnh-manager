#include "gfx.hpp"

#include <algorithm>

namespace acnh_manager::ui {

void BlendPixel(Surface surface, int x, int y, Color color) {
    if (x < 0 || y < 0 || x >= surface.width || y >= surface.height || surface.pixels == nullptr) {
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
    const int x0 = std::max(0, x);
    const int y0 = std::max(0, y);
    const int x1 = std::min(surface.width, x + w);
    const int y1 = std::min(surface.height, y + h);
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

void Fill(Surface surface, Color color) {
    FillRect(surface, 0, 0, surface.width, surface.height, color);
}

}  // namespace acnh_manager::ui
