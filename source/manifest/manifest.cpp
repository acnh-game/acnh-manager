#include "manifest.hpp"

#include "json.hpp"

#include <cctype>
#include <cstdarg>
#include <cstdio>

namespace acnh_manager::manifest {
namespace {

std::string Describe(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

std::string Describe(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return std::string(buf);
}

std::string Upper(std::string_view text) {
    std::string out(text);
    for (char &c : out) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return out;
}

std::string StringField(const json::Value *object, const char *key, bool required, bool *ok,
                        std::string *error, const char *context) {
    const json::Value *value = object != nullptr ? object->Find(key) : nullptr;
    if (value == nullptr || !value->IsString()) {
        if (required) {
            *ok = false;
            if (error->empty()) {
                *error = Describe("%s: missing or non-string field \"%s\"", context, key);
            }
        }
        return {};
    }
    return value->string;
}

bool ParseFile(const json::Value &node, const char *context, FileEntry *out, std::string *error) {
    if (!node.IsObject()) {
        *error = Describe("%s: file entry is not an object", context);
        return false;
    }
    bool ok = true;
    out->name = StringField(&node, "name", true, &ok, error, context);
    out->source = StringField(&node, "source", true, &ok, error, context);
    out->target = StringField(&node, "target", true, &ok, error, context);
    out->sha256 = Upper(StringField(&node, "sha256", true, &ok, error, context));
    out->restart = StringField(&node, "restart", false, &ok, error, context);
    if (!ok) {
        return false;
    }
    if (out->restart.empty()) {
        out->restart = "none";
    }
    const json::Value *size = node.Find("size");
    if (size == nullptr || !size->IsNumber()) {
        *error = Describe("%s: file \"%s\" missing numeric size", context, out->name.c_str());
        return false;
    }
    const std::int64_t size_value = json::IntOr(*size, -1);
    if (size_value <= 0) {
        *error = Describe("%s: file \"%s\" has non-positive size", context, out->name.c_str());
        return false;
    }
    out->size = static_cast<std::uint64_t>(size_value);
    if (!IsSafeTarget(out->target)) {
        *error = Describe("%s: file \"%s\" target \"%s\" is not a safe relative path", context,
                          out->name.c_str(), out->target.c_str());
        return false;
    }
    if (!IsSha256Hex(out->sha256)) {
        *error = Describe("%s: file \"%s\" sha256 is not 64 hex digits", context, out->name.c_str());
        return false;
    }
    if (out->restart != "none" && out->restart != "game" && out->restart != "console") {
        *error = Describe("%s: file \"%s\" has unknown restart \"%s\"", context, out->name.c_str(),
                          out->restart.c_str());
        return false;
    }
    return true;
}

bool ParseGame(const json::Value &node, std::size_t index, GameEntry *out, std::string *error) {
    char context[32];
    std::snprintf(context, sizeof(context), "games[%zu]", index);
    if (!node.IsObject()) {
        *error = Describe("%s: entry is not an object", context);
        return false;
    }
    bool ok = true;
    out->profile = StringField(&node, "profile", false, &ok, error, context);
    out->title_id = Upper(StringField(&node, "titleId", true, &ok, error, context));
    out->display_version = StringField(&node, "displayVersion", false, &ok, error, context);
    out->content_id = Upper(StringField(&node, "contentId", true, &ok, error, context));
    out->build_id = Upper(StringField(&node, "buildId", false, &ok, error, context));
    if (!ok) {
        return false;
    }
    const json::Value *version = node.Find("version");
    if (version == nullptr || !version->IsNumber()) {
        *error = Describe("%s: missing numeric version", context);
        return false;
    }
    const std::int64_t version_value = json::IntOr(*version, -1);
    if (version_value < 0) {
        *error = Describe("%s: version is not a non-negative integer", context);
        return false;
    }
    out->version = static_cast<std::uint32_t>(version_value);
    if (!IsHex(out->title_id, 16)) {
        *error = Describe("%s: titleId \"%s\" is not 16 hex digits", context, out->title_id.c_str());
        return false;
    }
    if (!IsHex(out->content_id, 32)) {
        *error = Describe("%s: contentId \"%s\" is not 32 hex digits", context,
                          out->content_id.c_str());
        return false;
    }
    if (!out->build_id.empty() && !IsHex(out->build_id, 32)) {
        *error = Describe("%s: buildId \"%s\" is not 32 hex digits", context, out->build_id.c_str());
        return false;
    }
    const json::Value *files = node.Find("files");
    if (files == nullptr || !files->IsArray() || files->Size() == 0) {
        *error = Describe("%s: missing non-empty files array", context);
        return false;
    }
    for (std::size_t i = 0; i < files->Size(); ++i) {
        FileEntry entry;
        if (!ParseFile(*files->At(i), context, &entry, error)) {
            return false;
        }
        out->files.push_back(std::move(entry));
    }
    return true;
}

}  // namespace

bool IsSafeTarget(std::string_view target) {
    if (target.empty() || target.front() == '/') {
        return false;
    }
    for (const char c : target) {
        if (c == '\\' || static_cast<unsigned char>(c) < 0x20) {
            return false;
        }
    }
    std::size_t start = 0;
    while (start <= target.size()) {
        const std::size_t slash = target.find('/', start);
        const std::string_view segment =
            slash == std::string_view::npos ? target.substr(start) : target.substr(start, slash - start);
        if (segment == "..") {
            return false;
        }
        if (slash == std::string_view::npos) {
            break;
        }
        start = slash + 1;
    }
    return true;
}

bool IsHex(std::string_view text, std::size_t length) {
    if (text.size() != length) {
        return false;
    }
    for (const char c : text) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) {
            return false;
        }
    }
    return true;
}

bool IsSha256Hex(std::string_view text) { return IsHex(text, 64); }

int CompareVersions(std::string_view lhs, std::string_view rhs) {
    std::size_t li = 0;
    std::size_t ri = 0;
    while (li < lhs.size() || ri < rhs.size()) {
        int lv = 0;
        int rv = 0;
        while (li < lhs.size() && lhs[li] != '.') {
            if (std::isdigit(static_cast<unsigned char>(lhs[li]))) {
                lv = lv * 10 + (lhs[li] - '0');
            }
            ++li;
        }
        while (ri < rhs.size() && rhs[ri] != '.') {
            if (std::isdigit(static_cast<unsigned char>(rhs[ri]))) {
                rv = rv * 10 + (rhs[ri] - '0');
            }
            ++ri;
        }
        if (lv != rv) {
            return lv < rv ? -1 : 1;
        }
        if (li < lhs.size()) {
            ++li;
        }
        if (ri < rhs.size()) {
            ++ri;
        }
    }
    return 0;
}

ParseResult Parse(std::string_view text, std::string_view app_version) {
    ParseResult result;
    json::Value root;
    std::string json_error;
    if (!json::Parse(text, &root, &json_error)) {
        result.error = "manifest: invalid JSON: " + json_error;
        return result;
    }
    if (!root.IsObject()) {
        result.error = "manifest: top-level value is not an object";
        return result;
    }
    const json::Value *schema = root.Find("schema");
    if (schema == nullptr || !schema->IsNumber()) {
        result.error = "manifest: missing numeric schema";
        return result;
    }
    result.manifest.schema = static_cast<int>(json::IntOr(*schema, -1));
    if (result.manifest.schema != kSchemaVersion) {
        result.error = Describe("manifest: schema %d is not supported (expected %d)",
                                result.manifest.schema, kSchemaVersion);
        return result;
    }

    bool ok = true;
    result.manifest.channel = StringField(&root, "channel", false, &ok, &result.error, "manifest");
    result.manifest.generated = StringField(&root, "generated", false, &ok, &result.error, "manifest");
    result.manifest.base_url = StringField(&root, "baseUrl", false, &ok, &result.error, "manifest");
    result.manifest.changelog = StringField(&root, "changelog", false, &ok, &result.error, "manifest");

    const json::Value *app = root.Find("app");
    if (app != nullptr && app->IsObject()) {
        result.manifest.app_min_version = StringField(app, "minVersion", false, &ok, &result.error, "app");
    }

    const json::Value *agent = root.Find("agent");
    if (agent == nullptr || !agent->IsObject()) {
        result.error = "manifest: missing agent object";
        return result;
    }
    result.manifest.agent.version = StringField(agent, "version", true, &ok, &result.error, "agent");
    result.manifest.agent.commit = StringField(agent, "commit", false, &ok, &result.error, "agent");
    const json::Value *flags = agent->Find("buildFlags");
    const json::Value *dirty = agent->Find("dirty");
    if (flags == nullptr || !flags->IsNumber() || dirty == nullptr || !dirty->IsBool()) {
        result.error = "manifest: agent requires numeric buildFlags and boolean dirty";
        return result;
    }
    result.manifest.agent.build_flags = static_cast<std::uint32_t>(json::IntOr(*flags, 0));
    result.manifest.agent.dirty = dirty->boolean;
    if (!ok) {
        return result;
    }
    if (result.manifest.agent.dirty) {
        result.error = "manifest: agent build is dirty; refusing a non-release artifact";
        return result;
    }
    if (result.manifest.agent.build_flags != kReleaseBuildFlags) {
        result.error = Describe(
            "manifest: agent buildFlags=%#x is not the release form (%#x = semantic hook only)",
            result.manifest.agent.build_flags, kReleaseBuildFlags);
        return result;
    }
    if (!result.manifest.app_min_version.empty() &&
        CompareVersions(app_version, result.manifest.app_min_version) < 0) {
        result.error = Describe("manifest: requires app >= %s (this app is %s)",
                                result.manifest.app_min_version.c_str(), std::string(app_version).c_str());
        return result;
    }

    const json::Value *games = root.Find("games");
    if (games == nullptr || !games->IsArray() || games->Size() == 0) {
        result.error = "manifest: missing non-empty games array";
        return result;
    }
    for (std::size_t i = 0; i < games->Size(); ++i) {
        GameEntry game;
        if (!ParseGame(*games->At(i), i, &game, &result.error)) {
            return result;
        }
        result.manifest.games.push_back(std::move(game));
    }
    result.ok = true;
    return result;
}

}  // namespace acnh_manager::manifest
