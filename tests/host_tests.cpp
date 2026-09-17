/* Host-side unit tests: manifest parsing/validation, gate verdicts, install decisions and
   the state.json round trip.  These modules deliberately avoid libnx, so they run on a
   development machine:
       make -C tests
   The numbers in the cases below come from real hardware (docs/architecture.md section 5). */

#include <cstdio>
#include <regex>
#include <set>
#include <string>
#include <string_view>

#include "env/config_ini.hpp"
#include "i18n/strings.hpp"
#include "install/gate.hpp"
#include "manifest/json.hpp"
#include "manifest/manifest.hpp"
#include "util/sha256.hpp"
#include "ui/action.hpp"
#include "util/text_wrap.hpp"
#include "util/time.hpp"

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

/* printf conversions of a string, with the literal %% escape ignored: used to prove the i18n
   columns agree, so a translation can never drop an argument the caller still passes. */
std::multiset<std::string> Conversions(const std::string &text) {
    static const std::regex pattern(R"(%[-#+ 0-9.]*(?:ll|l|h|z|j|t)?[diouxXeEfFgGaAcspn%])");
    std::multiset<std::string> found;
    for (auto it = std::sregex_iterator(text.begin(), text.end(), pattern);
         it != std::sregex_iterator(); ++it) {
        const std::string token = it->str();
        if (token != "%%") {
            found.insert(token);
        }
    }
    return found;
}

/* Measured on real hardware: title / version / content id / build id. */
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
    /* \u00e9 = e-acute (2 bytes) and a surrogate pair (4 bytes), 6 bytes together. */
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

    auto game_closed = Detected(); /* game not running: ModuleId unknown, must not be refused */
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

    /* What the console actually has: `[default_config] override_key=!L` -> applies by
       default, holding L turns the override off.  Expected sentences are built from the i18n
       table, so this test never hard-codes product text (source stays English). */
    using acnh_manager::i18n::Format;
    using acnh_manager::i18n::Language;
    using acnh_manager::i18n::SetLanguage;
    using acnh_manager::i18n::StringId;
    SetLanguage(Language::ZhHans);
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
    CHECK(current_advice.text == Format(StringId::OverrideOnByDefault, "L") +
                                     Format(StringId::OverrideHbmenuNote, "R"));

    /* No exclamation mark: off by default, L has to be held. */
    const auto must_hold = ParseOverrideConfig("[default_config]\noverride_key=L\n");
    CHECK(!must_hold.global_default.by_default);
    const auto hold_advice = Advise(must_hold, OverrideKey{});
    CHECK(!hold_advice.effective_by_default);
    /* The hbmenu note is appended too: override_any_app defaults to true with key R. */
    CHECK(hold_advice.text == Format(StringId::OverrideOffByDefault, "L") +
                                  Format(StringId::OverrideHbmenuNote, "R"));

    /* No config file: the code default is {L, by_default=true}. */
    const auto defaults = ParseOverrideConfig("");
    CHECK(defaults.global_default.key == "L");
    CHECK(defaults.global_default.by_default);
    CHECK(!defaults.global_default.specified);

    /* Explicitly empty: no combination and by_default=false -> never applies. */
    const auto empty_value = ParseOverrideConfig("[default_config]\noverride_key=\n");
    const auto never = Advise(empty_value, OverrideKey{});
    CHECK(never.never_applies);
    CHECK(!never.effective_by_default);

    /* Empty with an exclamation mark: no combination but by_default=true -> always applies. */
    const auto always_value = ParseOverrideConfig("[default_config]\noverride_key=!\n");
    const auto always = Advise(always_value, OverrideKey{});
    CHECK(!always.never_applies);
    CHECK(always.effective_by_default);
    CHECK(always.text == Text(StringId::OverrideAlwaysOn, Language::ZhHans));

    /* Unknown key name: same behaviour as Atmosphere's ParseOverrideKey (combination 0). */
    const auto unknown = ParseOverrideConfig("[default_config]\noverride_key=FOO\n");
    CHECK(Advise(unknown, OverrideKey{}).never_applies);

    /* With override_any_app off, the hbmenu note disappears. */
    const auto no_hbl = ParseOverrideConfig(
        "[hbl_config]\noverride_any_app=false\n[default_config]\noverride_key=!L\n");
    CHECK(Advise(no_hbl, OverrideKey{}).text ==
          Format(StringId::OverrideOnByDefault, "L"));

    /* A per-title config overrides the global default. */
    const auto title_key = ParseTitleConfig("[override_config]\noverride_key=!R\n");
    CHECK(title_key.specified);
    CHECK(title_key.key == "R");
    CHECK(title_key.by_default);
    const auto title_advice = Advise(current, title_key);
    CHECK(title_advice.key == "R");
    CHECK(title_advice.text == Format(StringId::OverrideOnByDefault, "R") +
                                  Format(StringId::OverrideHbmenuNote, "R"));
}

void TestStrings() {
    using acnh_manager::i18n::Language;
    using acnh_manager::i18n::StringId;
    using acnh_manager::i18n::StringCount;
    using acnh_manager::i18n::Text;
    /* The table size has to match the enum one-to-one, or a new string is easy to half-add. */
    CHECK(StringCount() == static_cast<unsigned>(StringId::ExitHint) + 1u);
    for (unsigned i = 0; i <= static_cast<unsigned>(StringId::ExitHint); ++i) {
        const auto id = static_cast<StringId>(i);
        CHECK(Text(id, Language::ZhHans) != nullptr);
        CHECK(Text(id, Language::English) != nullptr);
        CHECK(std::string(Text(id, Language::ZhHans)).size() > 0);
        CHECK(std::string(Text(id, Language::English)).size() > 0);
        /* Both columns must carry the same printf conversions: a translation that drops a
           %s would reach vsnprintf and read an argument that is not there. */
        CHECK(Conversions(Text(id, Language::ZhHans)) == Conversions(Text(id, Language::English)));
    }
}

void TestSha256() {
    using acnh_manager::util::Sha256;
    using acnh_manager::util::Sha256Hex;
    /* Standard test vectors. */
    CHECK(Sha256Hex("") ==
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(Sha256Hex("abc") ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(Sha256Hex(std::string(56, 'a')) ==
          "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
    CHECK(Sha256Hex(std::string(64, 'a')) ==
          "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");
    /* Chunked updates agree with a one-shot hash (the engine reads big files in 64 KiB
       chunks). */
    const std::string payload(200000, 'x');
    Sha256 streaming;
    for (std::size_t offset = 0; offset < payload.size(); offset += 65536) {
        const std::size_t chunk = std::min<std::size_t>(65536, payload.size() - offset);
        streaming.Update(payload.data() + offset, chunk);
    }
    CHECK(Sha256::ToHex(streaming.Finish()) == Sha256Hex(payload));
}

void TestTimeFormat() {
    using acnh_manager::util::FormatUnixTimeUtc;
    CHECK(FormatUnixTimeUtc(0) == "1970-01-01T00:00:00Z");
    CHECK(FormatUnixTimeUtc(951782400) == "2000-02-29T00:00:00Z"); /* leap day */
    CHECK(FormatUnixTimeUtc(1758000000) == "2025-09-16T05:20:00Z");
    CHECK(FormatUnixTimeUtc(1757999999) == "2025-09-16T05:19:59Z");
}

/* Line breaking: the console showed "Note: hol" / "ding R" before this existed, so pin the
   rule that Latin text breaks at spaces while CJK still breaks per character.  Every code
   point is 10 px wide here, spaces included, so a 70 px line holds 7 characters. */
void TestTextWrap() {
    using acnh_manager::util::WrapText;
    const auto advance = [](std::uint32_t code) { return code == '\n' ? 0 : 10; };
    const auto slice = [](const char *text, const acnh_manager::util::TextLine &line) {
        return std::string_view(text).substr(line.begin, line.end - line.begin);
    };

    /* A 70 px line holds "aaa bbb" exactly; the space after it is dropped and "ccc" starts
       the next line. */
    auto lines = WrapText("aaa bbb ccc", 70, advance);
    CHECK(lines.size() == 2);
    CHECK(slice("aaa bbb ccc", lines[0]) == "aaa bbb");
    CHECK(slice("aaa bbb ccc", lines[1]) == "ccc");

    /* The exact shape from the console: a word must not be split just because "holding"
       straddles the limit. */
    lines = WrapText("Note: holding", 70, advance);
    CHECK(lines.size() == 2);
    CHECK(slice("Note: holding", lines[0]) == "Note:");
    CHECK(slice("Note: holding", lines[1]) == "holding");

    /* A single word longer than the line still breaks mid-word instead of overflowing. */
    lines = WrapText("abcdefgh", 50, advance);
    CHECK(lines.size() == 2);
    CHECK(lines[0].width == 50);
    CHECK(lines[1].width == 30);

    /* CJK has no spaces: it keeps breaking per character and every line stays inside. */
    lines = WrapText("\xE8\xA6\x86\xE7\x9B\x96\xE9\xBB\x98\xE8\xAE\xA4", 30, advance);
    CHECK(lines.size() == 2);
    CHECK(lines[0].width == 30);
    CHECK(lines[1].width == 10);

    /* An explicit newline always starts a new line. */
    lines = WrapText("ab\ncd", 0, advance);
    CHECK(lines.size() == 2);
    CHECK(lines[0].width == 20);
    CHECK(lines[1].width == 20);
}

/* Actions: touch hit-testing and spatial focus movement.  These are pure functions on
   rectangles, so the layout of every button can be pinned here instead of on the console. */
void TestActions() {
    using acnh_manager::ui::Action;
    using acnh_manager::ui::FirstEnabled;
    using acnh_manager::ui::HitTest;
    using acnh_manager::ui::MoveFocus;
    using acnh_manager::ui::Rect;

    /* The home screen from the design: a wide primary button plus two half-width ones. */
    const std::vector<Action> home = {
        {0, Rect{120, 270, 1040, 144}, true},  /* A: primary */
        {1, Rect{120, 450, 508, 110}, true},   /* X: check for updates */
        {2, Rect{652, 450, 508, 110}, true},   /* Y: uninstall */
    };

    /* Hit-testing: inside, on the border (top-left counts, bottom-right does not), outside. */
    CHECK(HitTest(home, 200, 300) == 0);
    CHECK(HitTest(home, 120, 270) == 0);
    CHECK(HitTest(home, 1159, 413) == 0);
    CHECK(HitTest(home, 1160, 414) == -1);
    CHECK(HitTest(home, 200, 500) == 1);
    CHECK(HitTest(home, 900, 500) == 2);
    CHECK(HitTest(home, 640, 500) == -1); /* the gap between the two buttons */

    /* A disabled control is neither hit nor focused. */
    std::vector<Action> with_disabled = home;
    with_disabled[2].enabled = false;
    CHECK(HitTest(with_disabled, 900, 500) == -1);

    /* Focus: entry lands on the primary button, down goes to the left secondary one, right
       moves between the two secondary buttons, and there is nothing below them. */
    CHECK(FirstEnabled(home) == 0);
    CHECK(MoveFocus(home, 0, 0, +1) == 1);
    CHECK(MoveFocus(home, 1, +1, 0) == 2);
    CHECK(MoveFocus(home, 2, -1, 0) == 1);
    CHECK(MoveFocus(home, 1, 0, +1) == 1);
    CHECK(MoveFocus(home, 2, 0, +1) == 2);
    CHECK(MoveFocus(home, 1, 0, -1) == 0);
    CHECK(MoveFocus(home, -1, 0, +1) == 0); /* unknown focus falls back to the first action */
    CHECK(MoveFocus(with_disabled, 0, 0, +1) == 1);
    CHECK(MoveFocus(with_disabled, 1, +1, 0) == 1); /* right neighbour is disabled */
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
    TestSha256();
    TestTimeFormat();
    TestActions();
    TestTextWrap();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
