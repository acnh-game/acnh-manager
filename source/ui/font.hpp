#pragma once

/* 文本渲染:FreeType + 主机共享字体(plGetSharedFontByType)。
   与 EdiZon-SE 同路:不打包字体文件,简中/繁中/韩文由主机自带字体提供。 */

#include <switch.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "ui/gfx.hpp"

namespace acnh_manager::ui {

/* 常用字号(像素)。 */
inline constexpr int kFontTitle = 40;
inline constexpr int kFontHeading = 28;
inline constexpr int kFontBody = 22;
inline constexpr int kFontSmall = 18;

class Font {
public:
    /* error 非空时写入失败步骤(plInitialize 返回码、字体数量等)。 */
    bool Init(std::string *error = nullptr);
    void Exit();
    bool Ready() const { return !m_faces.empty(); }

    /* 以 (x, y) 为文本左上角绘制;返回绘制宽度(像素)。超出 max_width 时按字符换行。 */
    int Draw(Surface surface, int x, int y, int size, Color color, std::string_view utf8,
             int max_width = 0);
    int Measure(std::string_view utf8, int size, int max_width = 0);
    int LineHeight(int size) const { return size + size / 3; }

private:
    struct Glyph {
        int width{0};
        int height{0};
        int left{0};
        int top{0};
        int advance{0};
        std::vector<std::uint8_t> bitmap;
    };
    struct SizeCache {
        int size{0};
        std::vector<std::pair<std::uint64_t, Glyph>> glyphs;
    };

    const Glyph *FindGlyph(std::uint32_t codepoint, int size);
    SizeCache *CacheFor(int size);

    /* 只保存指针与生命周期标记:FT_Library/FT_Face 的完整类型留在 .cpp。 */
    struct Impl;
    Impl *m_impl{nullptr};
    std::vector<void *> m_faces;
    std::vector<SizeCache> m_caches;
};

}  // namespace acnh_manager::ui
