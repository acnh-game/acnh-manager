#include "install/icon_file.hpp"

#include <algorithm>

namespace acnh_manager::install {
namespace {

/* The control content's language order is the NACP's, spelled out in the file names. */
struct LanguageFiles {
    const char *preferred;
    const char *sibling;
};

LanguageFiles FilesFor(i18n::Language language) {
    if (language == i18n::Language::ZhHans) {
        return {"icon_SimplifiedChinese.dat", "icon_TraditionalChinese.dat"};
    }
    return {"icon_AmericanEnglish.dat", "icon_BritishEnglish.dat"};
}

bool Present(const std::vector<std::string> &files, const char *name) {
    return std::find(files.begin(), files.end(), name) != files.end();
}

/* The format's shape for an icon: "icon_<language>.dat". */
bool LooksLikeIcon(const std::string &name) {
    constexpr const char *kPrefix = "icon_";
    constexpr const char *kSuffix = ".dat";
    const std::size_t suffix = 4;
    return name.size() > 9 && name.compare(0, 5, kPrefix) == 0 &&
           name.compare(name.size() - suffix, suffix, kSuffix) == 0;
}

}  // namespace

std::string ChooseIconFile(const std::vector<std::string> &files, i18n::Language app_language) {
    const LanguageFiles wanted = FilesFor(app_language);
    if (Present(files, wanted.preferred)) {
        return wanted.preferred;
    }
    if (Present(files, wanted.sibling)) {
        return wanted.sibling;
    }
    if (Present(files, kIconFallbackFile)) {
        return kIconFallbackFile;
    }
    /* A package that carries icons but none in a language this app reads still has a picture worth
       showing; the smallest name keeps the choice the same from one listing to the next, because
       the order the filesystem hands the names over is not ours to rely on. */
    std::string best;
    for (const std::string &name : files) {
        if (!LooksLikeIcon(name)) {
            continue;
        }
        if (best.empty() || name < best) {
            best = name;
        }
    }
    return best;
}

}  // namespace acnh_manager::install
