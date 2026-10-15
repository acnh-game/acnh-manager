#pragma once

/* The download source's hard gate: does this address answer a *range* request?

   Every byte the installer reads from an address is one `Range: <offset>-<end>` request, and the
   container's total size comes from the probe's `Content-Range` header -- so an address that
   cannot do ranges cannot be installed from at all (it would have to be streamed from byte zero,
   and an interrupted transfer could never be resumed).  The design therefore refuses such an
   address up front, with wording of its own on the address screen (design section 9).

   The judgement lives here, apart from the libcurl code that produces the numbers, because it
   decides a player-facing card and because the device file cannot be tested on the development
   machine.  One real bug came out of testing it: libcurl hands the header over *with* its CRLF,
   so a parser that demanded digits to the end of the line refused every normal 206 answer. */

#include <cstdint>
#include <string>
#include <string_view>

namespace acnh_manager::install {

enum class RangeProbe {
    Supported, /* 206, and a Content-Range that names the container's total size */
    NotRange,  /* answered, but not with a range this app can use (200, or an unreadable header) */
    BadStatus, /* answered with some other status (404 and friends) */
};

/* The container's total size out of `Content-Range: bytes 0-0/<total>`, where the part after the
   slash may also be an asterisk, meaning "unknown", or absent.  Surrounding whitespace -- including
   the CRLF libcurl leaves on the line -- is not part of the value. */
bool ParseContentRangeTotal(std::string_view value, std::uint64_t *total);

/* The verdict for one probe reply.  `status` is the HTTP status (0 when the request never got
   one), `content_range` the header verbatim.  `*total` is written only for `Supported`. */
RangeProbe JudgeRangeProbe(long status, std::string_view content_range, std::uint64_t *total);

/* What the address screen matches on (it maps these to wording of its own).  They are the
   interface between the download source and the UI, so they live here instead of being spelled
   out on both sides. */
inline constexpr const char *kProbeErrorNoNetwork = "no-network";
inline constexpr const char *kProbeErrorNotRange = "range-unsupported";
/* A status the server answered with, carrying the number: "http-status:404".  The screen shows
   the number, which is the difference between "the address is wrong" and "the address cannot be
   resumed from". */
inline constexpr const char *kProbeErrorHttpStatusPrefix = "http-status:";
std::string HttpStatusError(long status);

}  // namespace acnh_manager::install
