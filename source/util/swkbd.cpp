#include "util/swkbd.hpp"

#include <switch.h>

#include <cstdio>

namespace acnh_manager::util {
namespace {

std::string Describe(const char *what, Result rc) {
    char buffer[128];
    std::snprintf(buffer, sizeof(buffer), "%s failed (0x%08x)", what, static_cast<unsigned>(rc));
    return buffer;
}

/* Long enough for any address a phone browser would hand out, and a fixed buffer because that is
   what the service writes into. */
constexpr std::size_t kMaxLine = 1024;

}  // namespace

bool AskForLine(const std::string &initial, const std::string &hint, std::string *out,
                std::string *error) {
    out->clear();
    SwkbdConfig kbd{};
    Result rc = swkbdCreate(&kbd, 0);
    if (R_FAILED(rc)) {
        *error = Describe("opening the software keyboard", rc);
        return false;
    }
    swkbdConfigMakePresetDefault(&kbd);
    swkbdConfigSetStringLenMax(&kbd, kMaxLine);
    if (!initial.empty()) {
        swkbdConfigSetInitialText(&kbd, initial.c_str());
    }
    if (!hint.empty()) {
        swkbdConfigSetGuideText(&kbd, hint.c_str());
    }
    char line[kMaxLine] = {};
    rc = swkbdShow(&kbd, line, sizeof(line));
    swkbdClose(&kbd);
    if (R_FAILED(rc)) {
        /* A cancelled keyboard comes back as an error too, and that is not a failure the player
           needs to hear about: the caller decides, and the log keeps the code. */
        *error = Describe("the software keyboard was dismissed", rc);
        return false;
    }
    *out = line;
    if (out->empty()) {
        *error = "the address was left empty";
        return false;
    }
    return true;
}

}  // namespace acnh_manager::util
