#pragma once

/* Text rendering: FreeType plus the console's shared fonts (plGetSharedFontByType).
   Same route as EdiZon-SE: no bundled font files; Simplified/Traditional Chinese and Korean
   come from the system fonts. */
#include <switch.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "ui/gfx.hpp"
#include "util/text_wrap.hpp"

namespace acnh_manager {
class Log; /* log.hpp */
}

namespace acnh_manager::ui {

/* Common sizes (pixels). */
inline constexpr int kFontTitle = 40;
inline constexpr int kFontHeading = 28;
inline constexpr int kFontBody = 22;
inline constexpr int kFontSmall = 18;

class Font {
public:
    /* A non-null error receives the failing step (plInitialize result, font count, ...);
       a non-null log receives every sub-step (flushed per line, for crash forensics). */
    bool Init(acnh_manager::Log *log = nullptr, std::string *error = nullptr);
    void Exit();
    bool Ready() const { return !m_faces.empty(); }

    /* Draw with (x, y) as the text's top-left; returns the drawn width in pixels.  Wraps at
       max_width. */
    int Draw(Surface surface, int x, int y, int size, Color color, std::string_view utf8,
             int max_width = 0);
    /* Synthetic bold: the console's shared faces come in one weight, so a "bold" run is the same
       glyph drawn once more one pixel right and one pixel down.  Used for the key letters in the
       badges (Ⓐ / Ⓛ / Ⓡ / Ⓑ), which read thin at 18 px otherwise. */
    int DrawBold(Surface surface, int x, int y, int size, Color color, std::string_view utf8,
                 int max_width = 0);
    int Measure(std::string_view utf8, int size, int max_width = 0);
    /* Line count after wrapping at max_width (at least 1).  Layout uses it for row heights,
       sharing the exact wrapping rules with Draw. */
    int LineCount(std::string_view utf8, int size, int max_width = 0);
    /* Truncate to at most max_lines: anything longer ends with an ellipsis.  The UI paints
       only what fits; the full text still goes to log.txt. */
    std::string Fit(std::string_view utf8, int size, int max_width, int max_lines);
    int LineHeight(int size) const { return size + size / 3; }

private:
    struct Glyph {
        int width{0};
        int height{0};
        int left{0};
        int top{0};
        int ascender{0};
        int advance{0};
        std::vector<std::uint8_t> bitmap;
    };
    struct SizeCache {
        int size{0};
        std::vector<std::pair<std::uint64_t, Glyph>> glyphs;
    };

    const Glyph *FindGlyph(std::uint32_t codepoint, int size);
    SizeCache *CacheFor(int size);
    /* Wrapping shared by Draw / Measure / LineCount / Fit.  The rules live in
       util/text_wrap.hpp (Latin breaks at spaces, CJK per character) so the host tests can
       exercise them without FreeType. */
    std::vector<util::TextLine> Wrap(std::string_view utf8, int size, int max_width);

    /* Only pointers and a lifetime flag are stored here; the full FT_Library/FT_Face types
       stay in the .cpp. */
    struct Impl;
    Impl *m_impl{nullptr};
    acnh_manager::Log *m_log{nullptr};
    std::vector<void *> m_faces;
    std::vector<SizeCache> m_caches;

    /* Write one log line and flush immediately; silent when m_log is null. */
    void Trace(const char *fmt, ...) __attribute__((format(printf, 2, 3)));
};

}  // namespace acnh_manager::ui
