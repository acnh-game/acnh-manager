#pragma once

/* Line breaking shared by the text renderer and the layout code.

   Why it is separate: the renderer needs a glyph table to measure, which makes it hard to
   test on a development machine, and line breaking is exactly where "looks right on the
   console" is not enough.  This header takes a per-code-point advance callback instead, so
   the host tests can drive it with a fake metric table.

   Rules:
   - Latin text breaks at spaces: the whole word moves to the next line instead of splitting
     "holding" into "hol" / "ding";
   - a word that does not fit on a line by itself still breaks mid-word (no overflow);
   - CJK has no spaces, so it keeps the per-character behaviour;
   - '\n' always starts a new line. */

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace acnh_manager::util {

/* One line is a byte range in the text (excluding the trailing newline or space). */
struct TextLine {
    std::size_t begin{0};
    std::size_t end{0};
    int width{0};
};

namespace detail {

inline std::uint32_t NextCodepoint(std::string_view text, std::size_t *index) {
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

}  // namespace detail

/* advance(code) returns the pixel advance of one code point (0 for a missing glyph). */
template <typename Advance>
std::vector<TextLine> WrapText(std::string_view text, int max_width, Advance advance) {
    std::vector<TextLine> lines;
    TextLine current;
    int pen_x = 0;
    std::size_t index = 0;
    /* Byte index right after the last space of the current line, and the width up to it. */
    std::size_t break_at = 0;
    int break_pen_x = 0;

    while (index < text.size()) {
        const std::size_t start = index;
        const std::uint32_t code = detail::NextCodepoint(text, &index);
        if (code == '\n') {
            lines.push_back(current);
            current = TextLine{};
            current.begin = index;
            current.end = index;
            pen_x = 0;
            break_at = 0;
            break_pen_x = 0;
            continue;
        }
        const int advance_px = advance(code);
        if (max_width > 0 && pen_x > 0 && pen_x + advance_px > max_width) {
            if (code == ' ' || code == '\t') {
                /* The line is full and the glyph that does not fit is the space itself: end
                   the line here and drop it, so the next word starts the next line. */
                current.width = pen_x;
                lines.push_back(current);
                current = TextLine{};
                current.begin = index;
                current.end = index;
                pen_x = 0;
                break_at = 0;
                break_pen_x = 0;
                continue;
            }
            if (break_at > current.begin) {
                /* Break at the last space: the word moves down whole. */
                const std::size_t word_begin = break_at;
                current.end = break_at - 1; /* drop the space itself */
                current.width = break_pen_x;
                lines.push_back(current);
                current = TextLine{};
                current.begin = word_begin;
                /* The part of that word which already fit was measured into the old line;
                   measure it again as the start of the new one.  This walk includes the code
                   point we are looking at now (index is already past it). */
                pen_x = 0;
                std::size_t walk = word_begin;
                while (walk < index) {
                    pen_x += advance(detail::NextCodepoint(text, &walk));
                }
                current.end = index;
                current.width = pen_x;
                break_at = 0;
                break_pen_x = 0;
                continue;
            }
            /* No usable space (a single long word, or CJK): break mid-word. */
            lines.push_back(current);
            current = TextLine{};
            current.begin = start;
            pen_x = 0;
            break_at = 0;
            break_pen_x = 0;
        }
        if (code == ' ' || code == '\t') {
            break_at = index;
            break_pen_x = pen_x;
        }
        pen_x += advance_px;
        current.end = index;
        current.width = pen_x;
    }
    lines.push_back(current);
    return lines;
}

}  // namespace acnh_manager::util
