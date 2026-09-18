#include "gate.hpp"

#include <cstdio>
#include <cstdarg>

#include "util/json.hpp"

namespace acnh_manager::install {
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
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
    }
    return out;
}

}  // namespace

GateResult Evaluate(const manifest::Manifest *manifest, const DetectedBuild &detected) {
    GateResult result;
    if (manifest == nullptr) {
        result.status = GateStatus::NoManifest;
        result.reason = "no manifest available";
        return result;
    }
    const std::string title = Upper(detected.title_id);
    const manifest::GameEntry *same_title = nullptr;
    for (const auto &game : manifest->games) {
        if (game.title_id == title) {
            same_title = &game;
            if (game.version == detected.version) {
                break;
            }
        }
    }
    if (same_title == nullptr) {
        result.status = GateStatus::TitleNotSupported;
        result.reason = Describe("no games[] entry for title %s", title.c_str());
        return result;
    }
    if (same_title->version != detected.version) {
        result.status = GateStatus::VersionNotSupported;
        result.reason = Describe("title supported but version %u != detected %u",
                                 same_title->version, detected.version);
        return result;
    }
    result.game = same_title;
    if (detected.content_id.empty()) {
        result.status = GateStatus::ContentIdMissing;
        result.reason = "content id unavailable (ncm read failed)";
        result.game = nullptr;
        return result;
    }
    if (same_title->content_id != Upper(detected.content_id)) {
        result.status = GateStatus::ContentIdMismatch;
        result.reason = Describe("content id %s != expected %s",
                                 Upper(detected.content_id).c_str(), same_title->content_id.c_str());
        result.game = nullptr;
        return result;
    }
    if (detected.module_id_known && !same_title->build_id.empty() &&
        same_title->build_id != Upper(detected.module_id)) {
        result.status = GateStatus::BuildIdMismatch;
        result.reason = Describe("running main ModuleId %s != expected %s",
                                 Upper(detected.module_id).c_str(), same_title->build_id.c_str());
        result.game = nullptr;
        return result;
    }
    result.status = GateStatus::Supported;
    result.reason = Describe("supported build %s (%s)", same_title->profile.empty()
                                                             ? title.c_str()
                                                             : same_title->profile.c_str(),
                             same_title->display_version.c_str());
    return result;
}

InstallPlan Plan(const GateResult &gate, const InstallState *state,
                 const manifest::AgentInfo &agent) {
    InstallPlan plan;
    if (gate.status != GateStatus::Supported || gate.game == nullptr) {
        plan.action = PlanAction::Blocked;
        plan.reason = gate.reason;
        return plan;
    }
    plan.game = gate.game;
    if (state == nullptr) {
        plan.action = PlanAction::Install;
        plan.reason = "no install record: fresh install";
        return plan;
    }
    if (state->content_id != gate.game->content_id) {
        plan.action = PlanAction::Install;
        plan.reason = "install record is for a different build: reinstall";
        return plan;
    }
    if (state->agent_version != agent.version) {
        plan.action = PlanAction::Install;
        plan.reason = Describe("installed agent %s != available %s: update",
                               state->agent_version.c_str(), agent.version.c_str());
        return plan;
    }
    for (const auto &file : gate.game->files) {
        bool found = false;
        for (const auto &installed : state->files) {
            if (installed.target == file.target) {
                found = installed.sha256 == file.sha256 && installed.size == file.size;
                break;
            }
        }
        if (!found) {
            plan.action = PlanAction::Repair;
            plan.reason = Describe("installed file %s missing or mismatched: repair",
                                   file.target.c_str());
            return plan;
        }
    }
    plan.action = PlanAction::UpToDate;
    plan.reason = Describe("already up to date (agent %s)", agent.version.c_str());
    return plan;
}

std::string DumpState(const InstallState &state) {
    json::Value root;
    root.type = json::Type::Object;
    root.object.emplace_back("schema", json::Value::MakeNumber(1));
    root.object.emplace_back("agentVersion", json::Value::MakeString(state.agent_version));
    root.object.emplace_back("agentCommit", json::Value::MakeString(state.agent_commit));
    root.object.emplace_back("contentId", json::Value::MakeString(state.content_id));
    root.object.emplace_back("buildId", json::Value::MakeString(state.build_id));
    root.object.emplace_back("installedAt", json::Value::MakeString(state.installed_at));
    json::Value files;
    files.type = json::Type::Array;
    for (const auto &file : state.files) {
        json::Value node;
        node.type = json::Type::Object;
        node.object.emplace_back("target", json::Value::MakeString(file.target));
        node.object.emplace_back("size", json::Value::MakeNumber(static_cast<double>(file.size)));
        node.object.emplace_back("sha256", json::Value::MakeString(file.sha256));
        files.array.push_back(std::move(node));
    }
    root.object.emplace_back("files", std::move(files));
    return json::Dump(root);
}

bool ParseState(std::string_view text, InstallState *out, std::string *error) {
    if (error != nullptr) {
        error->clear();
    }
    json::Value root;
    std::string json_error;
    if (!json::Parse(text, &root, &json_error)) {
        if (error != nullptr) {
            *error = "state: invalid JSON: " + json_error;
        }
        return false;
    }
    if (!root.IsObject()) {
        if (error != nullptr) {
            *error = "state: top-level value is not an object";
        }
        return false;
    }
    InstallState state;
    const json::Value *version = root.Find("agentVersion");
    if (version == nullptr || !version->IsString()) {
        if (error != nullptr) {
            *error = "state: missing agentVersion";
        }
        return false;
    }
    state.agent_version = version->string;
    if (const json::Value *value = root.Find("agentCommit"); value != nullptr && value->IsString()) {
        state.agent_commit = value->string;
    }
    if (const json::Value *value = root.Find("contentId"); value != nullptr && value->IsString()) {
        state.content_id = value->string;
    }
    if (const json::Value *value = root.Find("buildId"); value != nullptr && value->IsString()) {
        state.build_id = value->string;
    }
    if (const json::Value *value = root.Find("installedAt"); value != nullptr && value->IsString()) {
        state.installed_at = value->string;
    }
    const json::Value *files = root.Find("files");
    if (files != nullptr && files->IsArray()) {
        for (std::size_t i = 0; i < files->Size(); ++i) {
            const json::Value *node = files->At(i);
            if (node == nullptr || !node->IsObject()) {
                continue;
            }
            const json::Value *target = node->Find("target");
            const json::Value *size = node->Find("size");
            const json::Value *sha = node->Find("sha256");
            if (target == nullptr || !target->IsString() || size == nullptr || !size->IsNumber() ||
                sha == nullptr || !sha->IsString()) {
                continue;
            }
            InstalledFile file;
            file.target = target->string;
            file.size = static_cast<std::uint64_t>(json::IntOr(*size, 0));
            file.sha256 = Upper(sha->string);
            state.files.push_back(std::move(file));
        }
    }
    if (out != nullptr) {
        *out = std::move(state);
    }
    return true;
}

}  // namespace acnh_manager::install
