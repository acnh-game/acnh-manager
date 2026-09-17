/* 主机侧单元测试:清单解析/校验、门控判定、安装决策与 state.json 往返。
   这些模块刻意不依赖 libnx,所以能直接在开发机上跑:
       make -C tests
   测试用例里的数值取自真机实测(见 docs/architecture.md 第 5 节)。 */

#include <cstdio>
#include <string>
#include <string_view>

#include "env/config_ini.hpp"
#include "i18n/strings.hpp"
#include "install/gate.hpp"
#include "manifest/json.hpp"
#include "manifest/manifest.hpp"

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool condition, const char *expr, const char *file, int line) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::printf("FAIL %s:%d: %s\n", file, line, expr);
    }
}

#define CHECK(expr) Check((expr), #expr, __FILE__, __LINE__)

/* 真机实测值:title/version/content id/build id。 */
constexpr const char *kTitleId = "01006F8002326000";
constexpr const char *kContentId = "E10617820DB06889E1638499478DA0DE";
constexpr const char *kBuildId = "FF1D1C05670DB6021C85B624A710B963";
constexpr std::uint32_t kVersion = 2228224;
constexpr const char *kSubsdkSha =
    "d16358a941289c596733f60af93e224df97eb849bec6952906b1a969a6433a2d";
constexpr const char *kNpdmSha =
    "23eea4eda50c345ee97c971708dcb16efdf091c5bc852fa57c152669104d05ba";

std::string SampleManifest() {
    std::string text = R"({
  "schema": 1,
  "channel": "stable",
  "generated": "2026-09-17T00:00:00Z",
  "app": { "minVersion": "0.1.0" },
  "agent": { "version": "0.11.0", "commit": "4ac89fd403b7", "dirty": false, "buildFlags": 2 },
  "baseUrl": "https://lextuo.com/acnh-chat-code/guide/agent/0.11.0/",
  "changelog": "test fixture",
  "games": [
    {
      "profile": "v3_0_3",
      "titleId": "01006F8002326000",
      "version": 2228224,
      "displayVersion": "3.0.3",
      "contentId": "E10617820DB06889E1638499478DA0DE",
      "buildId": "FF1D1C05670DB6021C85B624A710B963",
      "files": [
        { "name": "subsdk9", "source": "subsdk9",
          "target": "atmosphere/contents/01006F8002326000/exefs/subsdk9",
          "size": 105837, "sha256": ")";
    text += kSubsdkSha;
    text += R"(", "restart": "game" },
        { "name": "main.npdm", "source": "main.npdm",
          "target": "atmosphere/contents/01006F8002326000/exefs/main.npdm",
          "size": 1652, "sha256": ")";
    text += kNpdmSha;
    text += R"(", "restart": "game" },
        { "name": "acnh-agent.version", "source": "acnh-agent.version",
          "target": "atmosphere/contents/01006F8002326000/exefs/acnh-agent.version",
          "size": 219, "sha256": ")";
    text += kNpdmSha;
    text += R"(", "restart": "none" }
      ]
    }
  ]
})";
    return text;
}

void TestJsonBasics() {
    const char *text = R"({"a":1,"b":[true,false,null,"x\ny"],"c":{"d":-2.5e3,"e":"\u00e9\ud83d\ude00"}})";
    acnh_manager::json::Value root;
    std::string error;
    CHECK(acnh_manager::json::Parse(text, &root, &error));
    CHECK(root.IsObject());
    CHECK(acnh_manager::json::IntOr(*root.Find("a"), -1) == 1);
    const auto *b = root.Find("b");
    CHECK(b != nullptr && b->Size() == 4);
    CHECK(b->At(0)->BoolOr(false));
    CHECK(b->At(1)->IsBool() && !b->At(1)->BoolOr(true));
    CHECK(b->At(2)->IsNull());
    CHECK(b->At(3)->StringOr(std::string()) == "x\ny");
    const auto *c = root.Find("c");
    CHECK(c != nullptr && c->Find("d")->NumberOr(0.0) == -2500.0);
    /* \u00e9 = é(2 字节),代理对 = 😀(4 字节),合计 6 字节。 */
    CHECK(c->Find("e")->StringOr(std::string()).size() == 6);
}

void TestJsonRejects() {
    acnh_manager::json::Value value;
    std::string error;
    CHECK(!acnh_manager::json::Parse("{\"a\":1,}", &value, &error));
    CHECK(!acnh_manager::json::Parse("{\"a\":1", &value, &error));
    CHECK(!acnh_manager::json::Parse("\"unterminated", &value, &error));
    CHECK(!acnh_manager::json::Parse("tru", &value, &error));
    CHECK(!acnh_manager::json::Parse("[1] extra", &value, &error));
    CHECK(!acnh_manager::json::Parse("{\"a\":01}", &value, &error));
}

void TestJsonDumpRoundTrip() {
    using acnh_manager::json::Type;
    using acnh_manager::json::Value;
    Value root;
    root.type = Type::Object;
    root.object.emplace_back("s", Value::MakeString("a\"b\\c\nd"));
    root.object.emplace_back("n", Value::MakeNumber(42));
    Value list;
    list.type = Type::Array;
    list.array.push_back(Value::MakeBool(true));
    list.array.push_back(Value::MakeNull());
    root.object.emplace_back("l", std::move(list));
    const std::string dumped = acnh_manager::json::Dump(root);
    Value parsed;
    std::string error;
    CHECK(acnh_manager::json::Parse(dumped, &parsed, &error));
    CHECK(parsed.Find("s")->StringOr(std::string()) == "a\"b\\c\nd");
    CHECK(acnh_manager::json::IntOr(*parsed.Find("n"), 0) == 42);
    CHECK(parsed.Find("l")->Size() == 2);
}

void TestManifestOk() {
    const auto result = acnh_manager::manifest::Parse(SampleManifest(), "0.1.0");
    if (!result.ok) {
        std::printf("  (manifest parse error: %s)\n", result.error.c_str());
    }
    CHECK(result.ok);
    CHECK(result.manifest.schema == 1);
    CHECK(result.manifest.agent.version == "0.11.0");
    CHECK(!result.manifest.agent.dirty);
    CHECK(result.manifest.agent.build_flags == 2);
    CHECK(result.manifest.games.size() == 1);
    const auto &game = result.manifest.games.front();
    CHECK(game.title_id == kTitleId);
    CHECK(game.version == kVersion);
    CHECK(game.content_id == kContentId);
    CHECK(game.build_id == kBuildId);
    CHECK(game.files.size() == 3);
    CHECK(game.files.front().restart == "game");
    CHECK(game.files.back().restart == "none");
}

std::string ReplaceOnce(std::string text, const std::string &from, const std::string &to) {
    const std::size_t pos = text.find(from);
    if (pos == std::string::npos) {
        return text;
    }
    return text.replace(pos, from.size(), to);
}

void TestManifestRejects() {
    using acnh_manager::manifest::Parse;
    const std::string base = SampleManifest();
    struct Case {
        const char *name;
        std::string text;
        const char *expect;
    };
    const Case cases[] = {
        {"schema", ReplaceOnce(base, "\"schema\": 1", "\"schema\": 2"), "schema"},
        {"dirty", ReplaceOnce(base, "\"dirty\": false", "\"dirty\": true"), "dirty"},
        {"flags", ReplaceOnce(base, "\"buildFlags\": 2", "\"buildFlags\": 7"), "buildFlags"},
        {"target",
         ReplaceOnce(base, "atmosphere/contents/01006F8002326000/exefs/subsdk9", "../evil/subsdk9"),
         "safe relative path"},
        {"sha",
         ReplaceOnce(base, kSubsdkSha,
                     "d16358a941289c596733f60af93e224df97eb849bec6952906b1a969a6433a2"),
         "64 hex"},
        {"minver", ReplaceOnce(base, "\"minVersion\": \"0.1.0\"", "\"minVersion\": \"0.2.0\""),
         "requires app >="},
        {"nogames", ReplaceOnce(base, "\"games\": [", "\"gamesUnused\": ["), "games"},
    };
    for (const Case &item : cases) {
        const auto result = Parse(item.text, "0.1.0");
        const bool rejected = !result.ok;
        const bool explained = result.error.find(item.expect) != std::string::npos;
        if (!rejected || !explained) {
            std::printf("  (case %s: ok=%d error=\"%s\")\n", item.name, result.ok ? 1 : 0,
                        result.error.c_str());
        }
        CHECK(rejected);
        CHECK(explained);
    }
}

void TestHelpers() {
    using acnh_manager::manifest::CompareVersions;
    using acnh_manager::manifest::IsSafeTarget;
    CHECK(IsSafeTarget("atmosphere/contents/x/exefs/subsdk9"));
    CHECK(IsSafeTarget("a"));
    CHECK(!IsSafeTarget(""));
    CHECK(!IsSafeTarget("/abs/path"));
    CHECK(!IsSafeTarget("a/../b"));
    CHECK(!IsSafeTarget(".."));
    CHECK(!IsSafeTarget("a\\b"));
    CHECK(CompareVersions("0.1.0", "0.2.0") < 0);
    CHECK(CompareVersions("0.2.0", "0.2.0") == 0);
    CHECK(CompareVersions("0.10.0", "0.9.0") > 0);
    CHECK(CompareVersions("1.0", "1.0.0") == 0);
}

acnh_manager::install::DetectedBuild Detected() {
    acnh_manager::install::DetectedBuild detected;
    detected.title_id = kTitleId;
    detected.version = kVersion;
    detected.content_id = kContentId;
    detected.module_id_known = true;
    detected.module_id = kBuildId;
    return detected;
}

void TestGate() {
    using acnh_manager::install::Evaluate;
    using acnh_manager::install::GateStatus;
    const auto parsed = acnh_manager::manifest::Parse(SampleManifest(), "0.1.0");
    CHECK(parsed.ok);
    const auto *manifest = &parsed.manifest;

    CHECK(Evaluate(nullptr, Detected()).status == GateStatus::NoManifest);

    auto unknown_title = Detected();
    unknown_title.title_id = "0100000000000000";
    CHECK(Evaluate(manifest, unknown_title).status == GateStatus::TitleNotSupported);

    auto other_version = Detected();
    other_version.version = 1310720; /* 2.0.x */
    CHECK(Evaluate(manifest, other_version).status == GateStatus::VersionNotSupported);

    auto no_content = Detected();
    no_content.content_id.clear();
    CHECK(Evaluate(manifest, no_content).status == GateStatus::ContentIdMissing);

    auto wrong_content = Detected();
    wrong_content.content_id = "29A8CE65E45B800CBC73214B7B818F68";
    CHECK(Evaluate(manifest, wrong_content).status == GateStatus::ContentIdMismatch);

    auto wrong_module = Detected();
    wrong_module.module_id = "283033DEEA3FC0B60104A4790098B9CC";
    CHECK(Evaluate(manifest, wrong_module).status == GateStatus::BuildIdMismatch);

    CHECK(Evaluate(manifest, Detected()).status == GateStatus::Supported);

    auto game_closed = Detected(); /* 游戏没运行:ModuleId 未知,不应因此被拒 */
    game_closed.module_id_known = false;
    game_closed.module_id.clear();
    CHECK(Evaluate(manifest, game_closed).status == GateStatus::Supported);
}

void TestPlan() {
    using acnh_manager::install::DumpState;
    using acnh_manager::install::Evaluate;
    using acnh_manager::install::InstallState;
    using acnh_manager::install::ParseState;
    using acnh_manager::install::Plan;
    using acnh_manager::install::PlanAction;
    const auto parsed = acnh_manager::manifest::Parse(SampleManifest(), "0.1.0");
    CHECK(parsed.ok);
    const auto &game = parsed.manifest.games.front();

    const auto blocked = Evaluate(nullptr, Detected());
    CHECK(Plan(blocked, nullptr, parsed.manifest.agent).action == PlanAction::Blocked);

    const auto supported = Evaluate(&parsed.manifest, Detected());
    CHECK(Plan(supported, nullptr, parsed.manifest.agent).action == PlanAction::Install);

    InstallState state;
    state.agent_version = parsed.manifest.agent.version;
    state.agent_commit = parsed.manifest.agent.commit;
    state.content_id = game.content_id;
    state.build_id = game.build_id;
    state.installed_at = "2026-09-17T00:00:00Z";
    for (const auto &file : game.files) {
        state.files.push_back({file.target, file.size, file.sha256});
    }
    CHECK(Plan(supported, &state, parsed.manifest.agent).action == PlanAction::UpToDate);

    const std::string dumped = DumpState(state);
    InstallState reloaded;
    std::string error;
    CHECK(ParseState(dumped, &reloaded, &error));
    CHECK(reloaded.agent_version == state.agent_version);
    CHECK(reloaded.content_id == state.content_id);
    CHECK(reloaded.files.size() == state.files.size());
    CHECK(reloaded.files.front().sha256 == state.files.front().sha256);
    CHECK(Plan(supported, &reloaded, parsed.manifest.agent).action == PlanAction::UpToDate);

    InstallState old_content = state;
    old_content.content_id = "83D23343779ECD95E7355C39E931E24A";
    CHECK(Plan(supported, &old_content, parsed.manifest.agent).action == PlanAction::Install);

    InstallState old_agent = state;
    old_agent.agent_version = "0.10.0";
    CHECK(Plan(supported, &old_agent, parsed.manifest.agent).action == PlanAction::Install);

    InstallState broken = state;
    broken.files.pop_back();
    CHECK(Plan(supported, &broken, parsed.manifest.agent).action == PlanAction::Repair);

    InstallState tampered = state;
    tampered.files.front().sha256 = std::string(64, '0');
    CHECK(Plan(supported, &tampered, parsed.manifest.agent).action == PlanAction::Repair);

    InstallState bad_json;
    CHECK(!ParseState("{not json", &bad_json, &error));
    CHECK(!error.empty());
}

void TestOverrideConfig() {
    using acnh_manager::env::Advise;
    using acnh_manager::env::OverrideConfig;
    using acnh_manager::env::OverrideKey;
    using acnh_manager::env::ParseOverrideConfig;
    using acnh_manager::env::ParseTitleConfig;

    /* 真机现状:`[default_config] override_key=!L` → 默认生效,按住 L 会关闭覆盖。 */
    const auto current = ParseOverrideConfig(R"(
[hbl_config]
program_id_1=01AADD1ACA618000
override_key_1=!A

[default_config]
override_key=!L
)");
    CHECK(current.global_default.specified);
    CHECK(current.global_default.key == "L");
    CHECK(current.global_default.by_default);
    const auto current_advice = Advise(current, OverrideKey{});
    CHECK(current_advice.effective_by_default);
    CHECK(!current_advice.never_applies);
    CHECK(current_advice.text.find("不要按住 L") != std::string::npos);
    CHECK(current_advice.text.find("进入 hbmenu") != std::string::npos);

    /* 不带感叹号:默认关闭,必须按住 L */
    const auto must_hold = ParseOverrideConfig("[default_config]\noverride_key=L\n");
    CHECK(!must_hold.global_default.by_default);
    const auto hold_advice = Advise(must_hold, OverrideKey{});
    CHECK(!hold_advice.effective_by_default);
    CHECK(hold_advice.text.find("需要按住 L") != std::string::npos);

    /* 没有配置文件:代码默认 {L, by_default=true} */
    const auto defaults = ParseOverrideConfig("");
    CHECK(defaults.global_default.key == "L");
    CHECK(defaults.global_default.by_default);
    CHECK(!defaults.global_default.specified);

    /* 显式空值:组合键为空 + by_default=false → 永不生效 */
    const auto empty_value = ParseOverrideConfig("[default_config]\noverride_key=\n");
    const auto never = Advise(empty_value, OverrideKey{});
    CHECK(never.never_applies);
    CHECK(!never.effective_by_default);

    /* 空值带感叹号:组合键为空 + by_default=true → 始终生效 */
    const auto always_value = ParseOverrideConfig("[default_config]\noverride_key=!\n");
    const auto always = Advise(always_value, OverrideKey{});
    CHECK(!always.never_applies);
    CHECK(always.effective_by_default);
    CHECK(always.text.find("始终生效") != std::string::npos);

    /* 无法识别的键名:与 Atmosphere 的 ParseOverrideKey 行为一致(组合键为 0) */
    const auto unknown = ParseOverrideConfig("[default_config]\noverride_key=FOO\n");
    CHECK(Advise(unknown, OverrideKey{}).never_applies);

    /* 关闭 override_any_app 后不再提示 hbmenu */
    const auto no_hbl = ParseOverrideConfig(
        "[hbl_config]\noverride_any_app=false\n[default_config]\noverride_key=!L\n");
    CHECK(Advise(no_hbl, OverrideKey{}).text.find("hbmenu") == std::string::npos);

    /* per-title 配置覆盖全局默认 */
    const auto title_key = ParseTitleConfig("[override_config]\noverride_key=!R\n");
    CHECK(title_key.specified);
    CHECK(title_key.key == "R");
    CHECK(title_key.by_default);
    const auto title_advice = Advise(current, title_key);
    CHECK(title_advice.key == "R");
    CHECK(title_advice.text.find("不要按住 R") != std::string::npos);
}

void TestStrings() {
    using acnh_manager::i18n::Language;
    using acnh_manager::i18n::StringId;
    using acnh_manager::i18n::StringCount;
    using acnh_manager::i18n::Text;
    /* 表项数量必须与枚举一一对应,否则新文案容易漏一边。 */
    CHECK(StringCount() == static_cast<unsigned>(StringId::ExitHint) + 1u);
    for (unsigned i = 0; i <= static_cast<unsigned>(StringId::ExitHint); ++i) {
        const auto id = static_cast<StringId>(i);
        CHECK(Text(id, Language::ZhHans) != nullptr);
        CHECK(Text(id, Language::English) != nullptr);
        CHECK(std::string(Text(id, Language::ZhHans)).size() > 0);
        CHECK(std::string(Text(id, Language::English)).size() > 0);
    }
}

}  // namespace

int main() {
    TestJsonBasics();
    TestJsonRejects();
    TestJsonDumpRoundTrip();
    TestManifestOk();
    TestManifestRejects();
    TestHelpers();
    TestGate();
    TestPlan();
    TestOverrideConfig();
    TestStrings();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
