#include "install/range_probe.hpp"

#include <cctype>

namespace acnh_manager::install {
namespace {

std::string_view Trim(std::string_view text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
        --end;
    }
    return text.substr(begin, end - begin);
}

}  // namespace

bool ParseContentRangeTotal(std::string_view value, std::uint64_t *total) {
    if (total == nullptr) {
        return false;
    }
    const std::size_t slash = value.find('/');
    if (slash == std::string_view::npos) {
        return false;
    }
    /* What comes before the slash has to be a byte range ("bytes 0-0", or a bare "0-0" from a
       server that leaves the unit off); an empty part after it means "unknown size", which is not
       enough to install from. */
    if (value.substr(0, slash).find('-') == std::string_view::npos) {
        return false;
    }
    const std::string_view tail = Trim(value.substr(slash + 1));
    if (tail.empty()) {
        return false;
    }
    std::uint64_t size = 0;
    for (const char c : tail) {
        if (c < '0' || c > '9') {
            return false; /* `*`, a negative number, or a trailing comment: not a size we can use */
        }
        const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
        if (size > (UINT64_MAX - digit) / 10) {
            return false; /* absurd value: refuse rather than wrap */
        }
        size = size * 10 + digit;
    }
    if (size == 0) {
        return false;
    }
    *total = size;
    return true;
}

RangeProbe JudgeRangeProbe(long status, std::string_view content_range, std::uint64_t *total) {
    if (status == 200) {
        /* The server ignored the range and started sending the file from byte zero: this is *the*
           "cannot be resumed from" answer, and the card says so.  (libcurl reports it as a failed
           transfer, because the write callback refuses the extra bytes.) */
        return RangeProbe::NotRange;
    }
    if (status != 206) {
        /* A 404, a 403, a redirect that ran out of hops, or no reply at all (0): the caller
           decides what a missing reply means, since only it knows what libcurl said. */
        return RangeProbe::BadStatus;
    }
    std::uint64_t size = 0;
    if (!ParseContentRangeTotal(content_range, &size)) {
        /* A 206 whose total we cannot read is as good as no range support: that number is what
           every later decision (progress, placeholder size, resume) is based on. */
        return RangeProbe::NotRange;
    }
    if (total != nullptr) {
        *total = size;
    }
    return RangeProbe::Supported;
}

std::string HttpStatusError(long status) {
    return std::string(kProbeErrorHttpStatusPrefix) + std::to_string(status);
}

}  // namespace acnh_manager::install
