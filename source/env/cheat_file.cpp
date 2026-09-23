#include "env/cheat_file.hpp"

#include <cctype>

namespace acnh_manager::env {
namespace {

constexpr std::size_t kStemLength = 16; /* 8 bytes of module id, as %02x each */
constexpr const char *kSuffix = ".txt";

/* The two addresses every chat-code entry reads (see the header).  Compared against code that has
   had its whitespace and comments removed, so the comparison is on the operands alone. */
constexpr const char *kTextInputAddress = "05255A60";
constexpr const char *kPlayerChainAddress = "05474040";

bool IsHexDigit(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/* One opcode line as it is compared: no spacing, no `//` comment, uppercase.  Turning the line
   into this shape is what makes the address operands findable regardless of how the file is
   laid out or which register the code happens to use. */
std::string NormalizeCode(const std::string &line) {
    std::string out;
    out.reserve(line.size());
    for (std::size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '/' && i + 1 < line.size() && line[i + 1] == '/') {
            break;
        }
        const unsigned char c = static_cast<unsigned char>(line[i]);
        if (std::isspace(c) != 0) {
            continue;
        }
        out.push_back(static_cast<char>(std::toupper(c)));
    }
    return out;
}

bool IsEntryHeader(const std::string &line) {
    /* A leading UTF-8 BOM would otherwise hide the first header -- and with it that entry's code,
       because a line that is not a header is only collected inside an entry.  Notepad writes the
       BOM by default, so a player who edits the guide's cheat file can produce one. */
    const std::size_t begin = line.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;
    for (std::size_t i = begin; i < line.size(); ++i) {
        const char c = line[i];
        if (std::isspace(static_cast<unsigned char>(c)) != 0) {
            continue;
        }
        return c == '[';
    }
    return false;
}

std::string EntryName(const std::string &line) {
    const std::size_t open = line.find('[');
    const std::size_t close = line.rfind(']');
    if (open == std::string::npos || close == std::string::npos || close <= open) {
        return std::string();
    }
    return line.substr(open + 1, close - open - 1);
}

}  // namespace

bool IsLegacyCheatFileName(const std::string &name) {
    if (name.size() != kStemLength + 4 || name.compare(kStemLength, 4, kSuffix) != 0) {
        return false;
    }
    for (std::size_t i = 0; i < kStemLength; ++i) {
        if (!IsHexDigit(name[i])) {
            return false;
        }
    }
    return true;
}

std::string ChatCodeCheatEntry(const std::string &text) {
    std::string name;
    std::string body;
    bool in_entry = false;

    auto matches = [&]() {
        return in_entry && body.find(kTextInputAddress) != std::string::npos &&
               body.find(kPlayerChainAddress) != std::string::npos;
    };

    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string::npos) {
            end = text.size();
        }
        const std::string line = text.substr(start, end - start);
        if (IsEntryHeader(line)) {
            if (matches()) {
                return name;
            }
            name = EntryName(line);
            body.clear();
            in_entry = true;
        } else if (in_entry) {
            body += NormalizeCode(line);
        }
        if (end == text.size()) {
            break;
        }
        start = end + 1;
    }
    return matches() ? name : std::string();
}

}  // namespace acnh_manager::env
