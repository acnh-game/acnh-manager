#include "install/title_name.hpp"

namespace acnh_manager::install {
namespace {

/* The slots the app's language reads, best first: a title that ships only one spelling of a
   language still has a name for a reader of the other one (Traditional for a Simplified reader and
   the other way round, American for a British reader and the other way round). */
struct LanguageSlots {
    int preferred;
    int sibling;
};

LanguageSlots SlotsFor(i18n::Language language) {
    if (language == i18n::Language::ZhHans) {
        return {kNacpSimplifiedChinese, kNacpTraditionalChinese};
    }
    return {kNacpAmericanEnglish, kNacpBritishEnglish};
}

const std::string &Slot(const std::array<std::string, kNacpLangCount> &languages, int slot) {
    static const std::string empty;
    if (slot < 0 || slot >= kNacpLangCount) {
        return empty;
    }
    return languages[static_cast<std::size_t>(slot)];
}

}  // namespace

TitleNames ChooseTitleNames(const std::array<std::string, kNacpLangCount> &languages,
                            const std::string &console_name, i18n::Language app_language) {
    TitleNames names;

    const LanguageSlots slots = SlotsFor(app_language);
    const std::string *app_name = &Slot(languages, slots.preferred);
    if (app_name->empty()) {
        app_name = &Slot(languages, slots.sibling);
    }

    if (!app_name->empty()) {
        names.primary = *app_name;
        /* Only when it says something else: repeating one name twice is not "two languages". */
        if (console_name != *app_name) {
            names.alternate = console_name;
        }
        return names;
    }

    /* The package does not carry the app's language at all: the console's name is all there is to
       name it with, and when even that is empty, the first slot the package filled in beats
       showing nothing. */
    names.primary = console_name;
    if (names.primary.empty()) {
        for (const std::string &name : languages) {
            if (!name.empty()) {
                names.primary = name;
                break;
            }
        }
    }
    return names;
}

}  // namespace acnh_manager::install
