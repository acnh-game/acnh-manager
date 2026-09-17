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

/* Try the console's shared fonts in order: Standard (JP/EN/EU), Simplified Chinese,
   Extended Simplified Chinese, Traditional Chinese, Korean. */
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

/* UTF-8 encoding of the ellipsis (U+2026). */
constexpr const char *kEllipsis = "\xE2\x80\xA6";

/* Start byte of the code point before index (used to step back through a string). */
std::size_t PrevCodepointStart(std::string_view text, std::size_t index) {
    if (index == 0) {
        return 0;
    }
    std::size_t at = index - 1;
    while (at > 0 && (static_cast<unsigned char>(text[at]) & 0xC0) == 0x80) {
        --at;
    }
    return at;
}

}  // namespace

std::vector<util::TextLine> Font::Wrap(std::string_view utf8, int size, int max_width) {
    /* A missing glyph is skipped by Draw, so it must take no width here either (otherwise a
       line could "measure as one line but paint as two"). */
    return util::WrapText(utf8, max_width, [this, size](std::uint32_t code) {
        const Glyph *glyph = FindGlyph(code, size);
        return glyph != nullptr ? glyph->advance : 0;
    });
}
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
        /* Use **pixel** sizes: FT_Set_Char_Size(..., 300, 300) scales the point size by
           300 DPI (x300/72 ~ 4.17), which is what made the text huge and overlapping. */
        if (FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(size)) != 0) {
            continue;
        }
        if (FT_Get_Char_Index(face, codepoint) == 0) {
            continue; /* this face has no such character; try the next one */
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
        glyph.ascender =
            face->size != nullptr ? static_cast<int>(face->size->metrics.ascender >> 6) : size;
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
    /* Not found: cache an empty glyph so we do not retry on every lookup. */
    cache->glyphs.emplace_back(codepoint, Glyph{});
    return &cache->glyphs.back().second;
}

int Font::Measure(std::string_view utf8, int size, int max_width) {
    if (!Ready()) {
        return 0;
    }
    int width = 0;
    for (const util::TextLine &line : Wrap(utf8, size, max_width)) {
        width = std::max(width, line.width);
    }
    return width;
}

int Font::LineCount(std::string_view utf8, int size, int max_width) {
    if (!Ready()) {
        return 1;
    }
    return static_cast<int>(Wrap(utf8, size, max_width).size());
}

std::string Font::Fit(std::string_view utf8, int size, int max_width, int max_lines) {
    if (!Ready() || max_width <= 0 || max_lines <= 0) {
        return std::string(utf8);
    }
    const std::vector<util::TextLine> lines = Wrap(utf8, size, max_width);
    if (static_cast<int>(lines.size()) <= max_lines) {
        return std::string(utf8);
    }
    /* Keep the first max_lines-1 lines and shrink the last one until the ellipsis fits. */
    const util::TextLine &last = lines[static_cast<std::size_t>(max_lines) - 1];
    std::string head(utf8.substr(0, last.begin));
    std::string tail(utf8.substr(last.begin, last.end - last.begin));
    const int ellipsis = Measure(kEllipsis, size);
    while (!tail.empty() && Measure(tail, size) + ellipsis > max_width) {
        tail.resize(PrevCodepointStart(tail, tail.size()));
    }
    return head + tail + kEllipsis;
}

int Font::Draw(Surface surface, int x, int y, int size, Color color, std::string_view utf8,
               int max_width) {
    if (!Ready()) {
        return 0;
    }
    const int line_height = LineHeight(size);
    int block_width = 0;
    const std::vector<util::TextLine> lines = Wrap(utf8, size, max_width);
    for (std::size_t i = 0; i < lines.size(); ++i) {
        /* Glyphs sit on the baseline: the caller's y is the line's top,
           baseline = y + ascender. */
        const int line_y = y + static_cast<int>(i) * line_height;
        int pen_x = x;
        std::size_t index = lines[i].begin;
        while (index < lines[i].end) {
            const std::uint32_t code = NextCodepoint(utf8, &index);
            const Glyph *glyph = FindGlyph(code, size);
            if (glyph == nullptr) {
                continue;
            }
            const int origin_x = pen_x + glyph->left;
            const int origin_y = line_y + glyph->ascender - glyph->top;
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
        block_width = std::max(block_width, pen_x - x);
    }
    return block_width;
}

}  // namespace acnh_manager::ui
