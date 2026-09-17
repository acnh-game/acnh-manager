#pragma once

/* 基于 libnx framebuffer 的最小绘制层:只有矩形与描边,文字走 Font。
   像素格式固定 RGBA8(由 framebufferCreate 指定)。 */

#include <switch.h>

namespace acnh_manager::ui {

struct Color {
    u8 r{0};
    u8 g{0};
    u8 b{0};
    u8 a{255};
};

inline constexpr Color MakeColor(u8 r, u8 g, u8 b, u8 a = 255) { return Color{r, g, b, a}; }

/* stride 以像素为单位(framebufferMakeLinear 之后为线性缓冲)。 */
struct Surface {
    u32 *pixels{nullptr};
    int width{0};
    int height{0};
    int stride{0};
};

void Fill(Surface surface, Color color);
void FillRect(Surface surface, int x, int y, int w, int h, Color color);
void StrokeRect(Surface surface, int x, int y, int w, int h, int thickness, Color color);
void BlendPixel(Surface surface, int x, int y, Color color);

}  // namespace acnh_manager::ui
