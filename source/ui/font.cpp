#include "font.hpp"

#include <algorithm>

#include <ft2build.h>
#include FT_FREETYPE_H

#include "log.hpp"

namespace acnh_manager::ui {

void Font::Trace(const char *fmt, ...) {
    if (m_log == nullptr) {
        return;
    }
    char buf[192];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    m_log->Line("%s", buf);
    m_log->Sync();
}

struct Font::Impl {
    FT_Library library{nullptr};
};

namespace {

/* 依次尝试主机共享字体:Standard(日/英/欧)、简中、扩展简中、繁中、韩文。 */
constexpr PlSharedFontType kFontTypes[] = {
    PlSharedFontType_Standard,
    PlSharedFontType_ChineseSimplified,
    PlSharedFontType_ExtChineseSimplified,
    PlSharedFontType_ChineseTraditional,
    PlSharedFontType_KO,
};

std::uint32_t NextCodepoint(std::string_view text, std::size_t *index) {
    const auto byte = static_cast<unsigned char>(text[*index]);
    std::uint32_t code = byte;
    std::size_t extra = 0;
    if ((byte & 0xE0) == 0xC0) {
        code = byte & 0x1F;
        extra = 1;
    } else if ((byte & 0xF0) == 0xE0) {
        code = byte & 0x0F;
        extra = 2;
    } else if ((byte & 0xF8) == 0xF0) {
        code = byte & 0x07;
        extra = 3;
    }
    ++(*index);
    for (std::size_t i = 0; i < extra && *index < text.size(); ++i, ++(*index)) {
        code = (code << 6) | (static_cast<unsigned char>(text[*index]) & 0x3F);
    }
    return code;
}

}  // namespace

bool Font::Init(acnh_manager::Log *log, std::string *error) {
    m_log = log;
    if (m_impl != nullptr) {
        return true;
    }
    Trace("font: FT_Init_FreeType ...");
    m_impl = new Impl();
    if (FT_Init_FreeType(&m_impl->library) != 0) {
        if (error != nullptr) {
            *error = "FT_Init_FreeType failed";
        }
        Trace("font: FT_Init_FreeType failed");
        delete m_impl;
        m_impl = nullptr;
        return false;
    }
    Trace("font: plInitialize ...");
    const Result rc = plInitialize(PlServiceType_User);
    if (R_FAILED(rc)) {
        if (error != nullptr) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "plInitialize rc=0x%08X", rc);
            *error = buf;
        }
        Trace("font: plInitialize rc=0x%08X", rc);
        FT_Done_FreeType(m_impl->library);
        delete m_impl;
        m_impl = nullptr;
        return false;
    }
    int failures = 0;
    int loaded = 0;
    for (const PlSharedFontType type : kFontTypes) {
        PlFontData data{};
        if (R_FAILED(plGetSharedFontByType(&data, type))) {
            ++failures;
            Trace("font: type %d request failed", static_cast<int>(type));
            continue;
        }
        if (data.address == nullptr || data.size == 0) {
            ++failures;
            Trace("font: type %d empty (addr=%p size=%u)", static_cast<int>(type), data.address,
                  static_cast<unsigned>(data.size));
            continue;
        }
        FT_Face face = nullptr;
        const int rc_face =
            FT_New_Memory_Face(m_impl->library, static_cast<const FT_Byte *>(data.address),
                               static_cast<FT_Long>(data.size), 0, &face);
        if (rc_face == 0) {
            m_faces.push_back(face);
            ++loaded;
            Trace("font: type %d loaded (%u bytes)", static_cast<int>(type),
                  static_cast<unsigned>(data.size));
        } else {
            Trace("font: type %d FT_New_Memory_Face rc=%d", static_cast<int>(type), rc_face);
        }
    }
    Trace("font: %d faces loaded, %d unavailable", loaded, failures);
    if (m_faces.empty() && error != nullptr) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "no shared font faces (%d requests failed)", failures);
        *error = buf;
    }
    return !m_faces.empty();
}

void Font::Exit() {
    for (void *handle : m_faces) {
        FT_Done_Face(static_cast<FT_Face>(handle));
    }
    m_faces.clear();
    m_caches.clear();
    if (m_impl != nullptr) {
        FT_Done_FreeType(m_impl->library);
        delete m_impl;
        m_impl = nullptr;
    }
}

Font::SizeCache *Font::CacheFor(int size) {
    for (auto &cache : m_caches) {
        if (cache.size == size) {
            return &cache;
        }
    }
    m_caches.push_back(SizeCache{size, {}});
    return &m_caches.back();
}

const Font::Glyph *Font::FindGlyph(std::uint32_t codepoint, int size) {
    if (m_impl == nullptr || m_faces.empty()) {
        return nullptr;
    }
    SizeCache *cache = CacheFor(size);
    for (const auto &entry : cache->glyphs) {
        if (entry.first == codepoint) {
            return &entry.second;
        }
    }
    for (void *handle : m_faces) {
        FT_Face face = static_cast<FT_Face>(handle);
        if (FT_Set_Char_Size(face, 0, static_cast<FT_F26Dot6>(size) * 64, 300, 300) != 0) {
            continue;
        }
        if (FT_Get_Char_Index(face, codepoint) == 0) {
            continue; /* 该字体没有这个字,换下一个 */
        }
        if (FT_Load_Char(face, codepoint, FT_LOAD_RENDER) != 0) {
            continue;
        }
        const FT_GlyphSlot slot = face->glyph;
        Glyph glyph;
        glyph.width = static_cast<int>(slot->bitmap.width);
        glyph.height = static_cast<int>(slot->bitmap.rows);
        glyph.left = slot->bitmap_left;
        glyph.top = slot->bitmap_top;
        glyph.advance = static_cast<int>(slot->advance.x >> 6);
        glyph.bitmap.assign(static_cast<std::size_t>(glyph.width) * glyph.height, 0);
        for (int row = 0; row < glyph.height; ++row) {
            const std::uint8_t *src = slot->bitmap.buffer + row * slot->bitmap.pitch;
            std::copy(src, src + glyph.width,
                      glyph.bitmap.begin() + static_cast<std::ptrdiff_t>(row) * glyph.width);
        }
        cache->glyphs.emplace_back(codepoint, std::move(glyph));
        return &cache->glyphs.back().second;
    }
    /* 没找到:记录一个空字形,避免每次重试。 */
    cache->glyphs.emplace_back(codepoint, Glyph{});
    return &cache->glyphs.back().second;
}

int Font::Measure(std::string_view utf8, int size, int max_width) {
    int width = 0;
    int line = 0;
    int lines = 1;
    std::size_t index = 0;
    while (index < utf8.size()) {
        const std::uint32_t code = NextCodepoint(utf8, &index);
        if (code == '\n') {
            width = std::max(width, line);
            line = 0;
            ++lines;
            continue;
        }
        const Glyph *glyph = FindGlyph(code, size);
        const int advance = glyph != nullptr ? glyph->advance : size / 2;
        if (max_width > 0 && line + advance > max_width && line > 0) {
            width = std::max(width, line);
            line = 0;
            ++lines;
        }
        line += advance;
    }
    width = std::max(width, line);
    return width;
}

int Font::Draw(Surface surface, int x, int y, int size, Color color, std::string_view utf8,
               int max_width) {
    if (!Ready()) {
        return 0;
    }
    const int line_height = LineHeight(size);
    int pen_x = x;
    int pen_y = y;
    int block_width = 0;
    std::size_t index = 0;
    while (index < utf8.size()) {
        const std::uint32_t code = NextCodepoint(utf8, &index);
        if (code == '\n') {
            block_width = std::max(block_width, pen_x - x);
            pen_x = x;
            pen_y += line_height;
            continue;
        }
        const Glyph *glyph = FindGlyph(code, size);
        if (glyph == nullptr) {
            continue;
        }
        if (max_width > 0 && pen_x - x + glyph->advance > max_width && pen_x > x) {
            block_width = std::max(block_width, pen_x - x);
            pen_x = x;
            pen_y += line_height;
        }
        const int origin_x = pen_x + glyph->left;
        const int origin_y = pen_y + (line_height - size) / 2 + (size - glyph->top);
        for (int row = 0; row < glyph->height; ++row) {
            for (int col = 0; col < glyph->width; ++col) {
                const std::uint8_t cover = glyph->bitmap[static_cast<std::size_t>(row) *
                                                              glyph->width + col];
                if (cover == 0) {
                    continue;
                }
                Color blended = color;
                blended.a = static_cast<u8>(cover * color.a / 255);
                BlendPixel(surface, origin_x + col, origin_y + row, blended);
            }
        }
        pen_x += glyph->advance;
    }
    return std::max(block_width, pen_x - x);
}

}  // namespace acnh_manager::ui
