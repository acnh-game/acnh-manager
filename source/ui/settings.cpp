#include "settings.hpp"

#include "util/json.hpp"

namespace acnh_manager::ui {
namespace {

constexpr int kSchema = 1;

/* Stored names are stable identifiers, not the enum's spelling: renaming an enumerator must
   not silently reset a player's choice. */
const char *LanguageName(i18n::Language language) {
    return language == i18n::Language::English ? "en" : "zh-Hans";
}

bool ParseLanguage(const std::string &name, i18n::Language *out) {
    if (name == "en" || name == "en-US" || name == "English") {
        *out = i18n::Language::English;
        return true;
    }
    if (name == "zh-Hans" || name == "zh" || name == "zh-CN") {
        *out = i18n::Language::ZhHans;
        return true;
    }
    return false;
}

}  // namespace

std::string DumpSettings(const Settings &settings) {
    json::Value root;
    root.type = json::Type::Object;
    root.object.emplace_back("schema", json::Value::MakeNumber(kSchema));
    root.object.emplace_back("language", json::Value::MakeString(LanguageName(settings.language)));
    return json::Dump(root);
}

bool ParseSettings(std::string_view text, Settings *settings, std::string *error) {
    json::Value root;
    std::string parse_error;
    if (!json::Parse(text, &root, &parse_error)) {
        if (error != nullptr) {
            *error = "settings: " + parse_error;
        }
        return false;
    }
    if (!root.IsObject()) {
        if (error != nullptr) {
            *error = "settings: not a JSON object";
        }
        return false;
    }
    const json::Value *schema = root.Find("schema");
    if (schema != nullptr && json::IntOr(*schema, 0) > kSchema) {
        if (error != nullptr) {
            *error = "settings: written by a newer version";
        }
        return false;
    }
    /* Only the keys we know are read, and an unknown or missing language keeps the current
       value: a hand-edited file should not be able to blank the interface. */
    const json::Value *language = root.Find("language");
    if (language != nullptr) {
        i18n::Language parsed = settings->language;
        if (language->IsString() && ParseLanguage(language->string, &parsed)) {
            settings->language = parsed;
        } else if (error != nullptr) {
            *error = "settings: unknown language '" + language->StringOr(std::string()) + "'";
        }
    }
    return true;
}

}  // namespace acnh_manager::ui
