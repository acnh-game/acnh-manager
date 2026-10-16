#pragma once

/* The upload path's rules, as pure code: the console tells the browser which range it wants
   (`GET /upload/next`), the browser posts exactly that (`POST /upload/part`), and a part that is
   not the one asked for is refused with a 409 instead of being written somewhere it does not
   belong.  Kept out of the socket code (`source_http_upload.*`) so the host tests can pin it: a
   part written at the wrong offset corrupts a content silently, and that is the kind of bug only
   hardware would otherwise show. */

#include <cstddef>
#include <cstdint>

namespace acnh_manager::install {

/* How big one posted part may be: the same 4 MiB the transfer asks for per range, so one part is
   one range and the page never has to slice smaller than what it was asked for. */
inline constexpr std::size_t kUploadPartBytes = 4 * 1024 * 1024;

/* Whether a part the browser sent is the one the reader is waiting for.  A part may be *shorter*
   than the range (the page's last slice of a file, or a slice it chose to split); it may never be
   longer, empty, or at another offset. */
inline bool UploadPartMatches(std::uint64_t want_offset, std::size_t want_size,
                              std::uint64_t offset, std::size_t length) {
    if (length == 0 || length > kUploadPartBytes) {
        return false;
    }
    if (offset != want_offset) {
        return false;
    }
    return length <= want_size;
}

}  // namespace acnh_manager::install
