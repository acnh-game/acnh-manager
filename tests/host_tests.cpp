/* Host-side unit tests: manifest parsing/validation, gate verdicts, install decisions and
   the state.json round trip.  These modules deliberately avoid libnx, so they run on a
   development machine:
       make -C tests
   The numbers in the cases below come from real hardware (docs/architecture.md section 5). */

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <tuple>
#include <regex>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "env/cheat_file.hpp"
#include "env/config_ini.hpp"
#include "i18n/strings.hpp"
#include "install/gate.hpp"
#include "install/container.hpp"
#include "install/install_task.hpp"
#include "install/nca.hpp"
#include "install/options.hpp"
#include "install/package.hpp"
#include "net/update_policy.hpp"
#include "util/json.hpp"
#include "util/qr_code.hpp"
#include "manifest/manifest.hpp"
#include "util/sha256.hpp"
#include "ui/action.hpp"
#include "ui/header_tabs.hpp"
#include "ui/home_state.hpp"
#include "ui/list_view.hpp"
#include "ui/settings.hpp"
#include "util/text_wrap.hpp"
#include "util/time.hpp"

namespace {

/* -------------------------------------------------------------------------------------------
   Container fixtures.

   A real package is gigabytes, so the tests build the structures by hand.  The layouts are the
   ones the reference implementation reads and the ones a real NSP/XCI uses:
     PFS0: u32 magic, u32 file count, u32 string table size, u32 padding
           count * { u64 data offset, u64 size, u32 name offset, u32 padding }, strings, data
     HFS0: same shape with u32 hashed size instead of padding; entries' offsets are relative to
           the data area that follows the header, exactly like the format defines.
   Every payload is filled with a byte that encodes its index, so a wrong data offset in the
   parser shows up as a wrong byte rather than as a passing size check. */
constexpr std::uint32_t kPfs0Magic = 0x30534650;
constexpr std::uint32_t kHfs0Magic = 0x30534648;

void PutU32(std::vector<std::uint8_t> &bytes, std::size_t at, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        bytes[at + i] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xFF);
    }
}

void PutU64(std::vector<std::uint8_t> &bytes, std::size_t at, std::uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        bytes[at + i] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xFF);
    }
}

/* Writes an HFS0 image at `offset`; returns the offset of its data area (or 0 when `offset` is
   past the end of the buffer).  Payload bytes are 0x10 + index so they can be told apart. */
std::size_t WriteHfs0(std::vector<std::uint8_t> &bytes, std::size_t offset, std::uint32_t magic,
                      const std::vector<std::pair<std::string, std::uint32_t>> &files) {
    std::string strings;
    std::vector<std::uint32_t> name_offsets;
    for (const auto &file : files) {
        name_offsets.push_back(static_cast<std::uint32_t>(strings.size()));
        strings += file.first;
        strings.push_back('\0');
    }
    const std::size_t table_size = 0x18 * files.size();
    const std::size_t data_offset = offset + 0x10 + table_size + strings.size();

    PutU32(bytes, offset, magic);
    PutU32(bytes, offset + 4, static_cast<std::uint32_t>(files.size()));
    PutU32(bytes, offset + 8, static_cast<std::uint32_t>(strings.size()));
    std::uint64_t relative = 0;
    for (std::size_t i = 0; i < files.size(); ++i) {
        PutU64(bytes, offset + 0x10 + i * 0x18, relative);
        PutU64(bytes, offset + 0x18 + i * 0x18, files[i].second);
        PutU32(bytes, offset + 0x20 + i * 0x18, name_offsets[i]);
        relative += files[i].second;
    }
    std::memcpy(bytes.data() + offset + 0x10 + table_size, strings.data(), strings.size());
    return data_offset;
}

std::vector<std::uint8_t> MakePfs0(
    const std::vector<std::pair<std::string, std::uint32_t>> &files) {
    std::size_t strings_size = 0;
    std::size_t payload_size = 0;
    for (const auto &file : files) {
        strings_size += file.first.size() + 1;
        payload_size += file.second;
    }
    const std::size_t data_offset = 0x10 + 0x18 * files.size() + strings_size;
    std::vector<std::uint8_t> bytes(data_offset + payload_size, 0);
    WriteHfs0(bytes, 0, kPfs0Magic, files);
    /* Payload bytes: 0x10 + index, so a wrong offset is visible. */
    std::size_t at = data_offset;
    std::uint8_t tag = 0x10;
    for (const auto &file : files) {
        for (std::uint32_t i = 0; i < file.second; ++i) {
            bytes[at + i] = tag;
        }
        at += file.second;
        ++tag;
    }
    return bytes;
}

/* A card dump: "HEAD" at 0x100, root HFS0 at 0xF000 with a "secure" partition that is itself an
   HFS0 holding the .nca files. */
std::vector<std::uint8_t> MakeXci(
    const std::vector<std::pair<std::string, std::uint32_t>> &secure_files) {
    std::size_t secure_payload = 0;
    std::size_t secure_strings = 0;
    for (const auto &file : secure_files) {
        secure_payload += file.second;
        secure_strings += file.first.size() + 1;
    }
    const std::size_t secure_size = 0x10 + 0x18 * secure_files.size() + secure_strings +
                                    secure_payload;
    /* Root partition: header + two entries + the strings "secure\0" and "logo\0". */
    const std::size_t root_size = 0x10 + 0x18 * 2 + 7 + 5;
    std::vector<std::uint8_t> bytes(0xF000 + root_size + secure_size, 0);

    PutU32(bytes, 0x100, 0x48454144); /* "HEAD" */
    const std::size_t root_data =
        WriteHfs0(bytes, 0xF000, kHfs0Magic,
                  {{"secure", static_cast<std::uint32_t>(secure_size)},
                   {"logo", 0x10}});
    const std::size_t secure_data =
        WriteHfs0(bytes, root_data, kHfs0Magic, secure_files);
    std::size_t at = secure_data;
    std::uint8_t tag = 0x20;
    for (const auto &file : secure_files) {
        for (std::uint32_t i = 0; i < file.second; ++i) {
            bytes[at + i] = tag;
        }
        at += file.second;
        ++tag;
    }
    return bytes;
}

class MemoryReader final : public acnh_manager::install::ContainerReader {
public:
    explicit MemoryReader(std::vector<std::uint8_t> bytes) : m_bytes(std::move(bytes)) {}

    bool Read(std::uint64_t offset, std::size_t size, std::uint8_t *out) override {
        if (offset > m_bytes.size() || size > m_bytes.size() - offset) {
            return false;
        }
        std::memcpy(out, m_bytes.data() + offset, size);
        return true;
    }

private:
    std::vector<std::uint8_t> m_bytes;
};

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
  "baseUrl": "https://gitee.com/acnh-game/acnh-manager/raw/main/packaging/agent/0.11.0/",
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

/* The cheat-file rules: which files the loader reads, and which of their entries conflict.
   The names on the left are the ones found on real hardware and in the guide's download
   (docs/architecture.md section 5); the 36-character case is the shape the first implementation
   asked for, which no card carries -- which is why the conflict warning never appeared until it
   was rewritten.  The entry names inside the fixtures stay ASCII on purpose (source is English):
   the matcher keys on the code, so what the entry is called must not matter to it. */
void TestCheatFile() {
    using acnh_manager::env::IsLegacyCheatFileName;

    CHECK(IsLegacyCheatFileName("FF1D1C05670DB602.txt"));   /* what the guide ships */
    CHECK(IsLegacyCheatFileName("ff1d1c05670db602.txt"));   /* what dmnt formats */
    CHECK(IsLegacyCheatFileName("0123456789ABCDEF.txt"));

    CHECK(!IsLegacyCheatFileName("toggles.txt"));           /* dmnt's own toggle state */
    CHECK(!IsLegacyCheatFileName("FF1D1C05670DB6021C85B624A710B963.txt")); /* 32 digits */
    CHECK(!IsLegacyCheatFileName("FF1D1C05670DB60.txt"));
    CHECK(!IsLegacyCheatFileName("FF1D1C05670DB602.TXT"));
    CHECK(!IsLegacyCheatFileName("FF1D1C05670DB602.txt.bak"));
    CHECK(!IsLegacyCheatFileName("GG1D1C05670DB602.txt"));
    CHECK(!IsLegacyCheatFileName(""));

    using acnh_manager::env::ChatCodeCheatEntry;

    /* A pack like the one on the author's card: fifteen entries, of which exactly one drives the
       chat-code path; the miles entry reads the player chain too, so the pair of addresses is what
       tells them apart. */
    const char *pack =
        "[Animal Crossing New Horizons v3.0.3 TID 01006F8002326000 BID FF1D1C05670DB602]\n"
        "\n"
        "[Stamina]\n"
        "011A0000 B9763A00 0000000A\n"
        "\n"
        "[Copy slot 1 to slot 2]\n"
        "80002300\n"
        "581D0000 B28ED6C8\n"
        "20000000\n"
        "\n"
        "[Chat codes to bag (up to 10)]\n"
        "80004004\n"
        "580A0000 05255A60\n"
        "580A1000 00000468\n"
        "58060000 05474040\n"
        "20000000\n"
        "\n"
        "[Max miles]\n"
        "580F0000 05474040\n"
        "20000000\n";
    CHECK(ChatCodeCheatEntry(pack) == "Chat codes to bag (up to 10)");

    /* The entry is identified by its code, so renaming it -- which players do -- changes nothing. */
    CHECK(ChatCodeCheatEntry("[my own name]\n580A0000 05255A60\n58060000 05474040\n20000000\n") ==
          "my own name");

    /* Comments, brace blocks, CRLF and irregular spacing are the same code. */
    CHECK(ChatCodeCheatEntry("// pack\r\n[entry]\r\n{ 580A0000   05255A60 // text input\r\n"
                             "58060000\t05474040 }\r\n") == "entry");

    /* A UTF-8 BOM (what Notepad writes by default) must not hide the first entry: a line that is
       not read as a header loses its code, and the entry after it would be found instead. */
    CHECK(ChatCodeCheatEntry("\xEF\xBB\xBF[Chat codes to bag]\n580A0000 05255A60\n"
                             "58060000 05474040\n20000000\n") == "Chat codes to bag");
    CHECK(ChatCodeCheatEntry("\xEF\xBB\xBF[renamed]\n580A0000 05255A60\n58060000 05474040\n")
          == "renamed");

    /* Half the pair is not the chat-code entry: the miles cheat reads the player chain only, and
       nothing else in the pack reads the text-input object. */
    CHECK(ChatCodeCheatEntry("[Max miles]\n580F0000 05474040\n20000000\n").empty());
    CHECK(ChatCodeCheatEntry("[Something else]\n580A0000 05255A60\n20000000\n").empty());
    CHECK(ChatCodeCheatEntry("").empty());
    CHECK(ChatCodeCheatEntry("no entries in here\n").empty());
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

    /* No Application content meta on the console: the game is not here at all.  This is the
       only case that may be reported as "not found" -- see the 1.0.0 case below. */
    auto no_title = Detected();
    no_title.title_id.clear();
    no_title.version = 0;
    no_title.content_id.clear();
    CHECK(Evaluate(manifest, no_title).status == GateStatus::TitleMissing);

    /* A bare 1.0.0 copy: the title is installed, its title version is 0, and no manifest entry
       covers it.  The verdict has to be "version not supported" (with the entry kept so the home
       screen can name the version this build accepts), never "no game here" -- a console in
       exactly this state was told the game was missing (details page: "title supported but
       version 2228224 != detected 0"). */
    auto base_1_0_0 = Detected();
    base_1_0_0.version = 0;
    base_1_0_0.content_id.clear();
    base_1_0_0.module_id_known = false;
    base_1_0_0.module_id.clear();
    const auto base = Evaluate(manifest, base_1_0_0);
    CHECK(base.status == GateStatus::VersionNotSupported);
    CHECK(base.game == nullptr);
    CHECK(base.title_entry != nullptr && base.title_entry->display_version == "3.0.3");

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

/* Which release manifests this app is willing to install from.

   The case that matters is the last one: a newer release that does *not* cover this console's
   game build.  Adopting it made the app call a perfectly supported console "unsupported" (and
   killed the primary button) until this rule was extracted here -- reproduced on hardware on
   2026-09-20 with a signed manifest listing only 4.0.0 while the console ran 3.0.3. */
void TestUpdateAdoption() {
    using acnh_manager::install::DetectedBuild;
    using acnh_manager::install::Evaluate;
    using acnh_manager::install::Plan;
    using acnh_manager::install::PlanAction;
    using acnh_manager::net::DecideAdoption;
    using acnh_manager::manifest::Manifest;

    const auto parsed = acnh_manager::manifest::Parse(SampleManifest(), "0.1.0");
    CHECK(parsed.ok);
    const Manifest &remote = parsed.manifest; /* agent 0.11.0, games[] = ACNH 3.0.3 */
    const DetectedBuild build = Detected();

    /* Nothing to compare against: never the install source. */
    auto nothing = DecideAdoption(remote, build, "");
    CHECK(!nothing.newer && !nothing.adopt && !nothing.unsupported);

    /* Same version: "nothing to do", not an update. */
    auto equal = DecideAdoption(remote, build, "0.11.0");
    CHECK(!equal.newer && !equal.adopt && !equal.unsupported);

    /* Older on the card, release covers this build: this is the update path. */
    auto newer_ok = DecideAdoption(remote, build, "0.10.0");
    CHECK(newer_ok.newer && newer_ok.adopt && !newer_ok.unsupported);

    /* Newer, but it does not know this build any more (a new game version shipped): keep the
       release compiled into the app, and say why instead of claiming "up to date". */
    Manifest for_4_0_0 = remote;
    for_4_0_0.agent.version = "0.11.1";
    for_4_0_0.games[0].version = 2686976; /* 4.0.0 */
    for_4_0_0.games[0].display_version = "4.0.0";
    auto unsupported = DecideAdoption(for_4_0_0, build, "0.11.0");
    CHECK(unsupported.newer && !unsupported.adopt && unsupported.unsupported);

    /* Same game version, different update content id (the manifest expects a build this
       console does not have): also not adopted. */
    Manifest other_content = remote;
    other_content.agent.version = "0.11.1";
    other_content.games[0].content_id = "29A8CE65E45B800CBC73214B7B818F68";
    auto mismatch = DecideAdoption(other_content, build, "0.11.0");
    CHECK(mismatch.newer && !mismatch.adopt && mismatch.unsupported);

    /* Not adopting a release is not the same as allowing writes onto an unknown build: with
       nothing installed, the gate still refuses to install that manifest's files. */
    const auto gate = Evaluate(&for_4_0_0, build);
    CHECK(Plan(gate, nullptr, for_4_0_0.agent).action == PlanAction::Blocked);
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

std::uint8_t ByteAt(acnh_manager::install::ContainerReader &reader, std::uint64_t offset) {
    std::uint8_t byte = 0xFF;
    reader.Read(offset, 1, &byte);
    return byte;
}

/* The container layer is the first thing that touches bytes from the user, so it is checked
   with both real layouts and every rejection the design promises (sections 7 and 9). */
void TestContainer() {
    using acnh_manager::install::ContainerInfo;
    using acnh_manager::install::ContainerKind;
    using acnh_manager::install::IsModPackageExtension;
    using acnh_manager::install::IsSupportedExtension;
    using acnh_manager::install::OpenContainer;

    /* A 3.0.3-style package: two NCAs plus the ticket and certificate, one name in upper case
       to prove the parser lowercases (content ids are case-insensitive in the system). */
    const std::vector<std::pair<std::string, std::uint32_t>> files = {
        {"E10617820DB06889E1638499478DA0DE.nca", 0x400},
        {"01006f8002326800000000000000000b.tik", 0x40},
        {"01006f8002326800000000000000000b.cert", 0x20},
        {"e10617820db06889e1638499478da0de.cnmt.nca", 0x80},
    };
    std::size_t strings = 0;
    for (const auto &file : files) {
        strings += file.first.size() + 1;
    }
    const std::uint64_t data_offset = 0x10 + 0x18 * files.size() + strings;

    auto image = MakePfs0(files);
    const std::uint64_t image_size = image.size();
    MemoryReader reader(std::move(image));

    ContainerInfo info;
    std::string error;
    CHECK(OpenContainer(reader, "game.nsp", image_size, &info, &error));
    CHECK(error.empty());
    CHECK(info.kind == ContainerKind::Nsp);
    CHECK(!info.compressed);
    CHECK(info.container_size == image_size);
    CHECK(info.contents.size() == 2);
    CHECK(info.extra.size() == 2);
    CHECK(info.contents[0].name == "e10617820db06889e1638499478da0de.nca");
    CHECK(info.contents[0].offset == data_offset);
    CHECK(info.contents[0].size == 0x400);
    CHECK(info.contents[1].name == "e10617820db06889e1638499478da0de.cnmt.nca");
    CHECK(info.contents[1].offset == data_offset + 0x400 + 0x40 + 0x20); /* after the .tik/.cert */
    CHECK(info.extra[0].name == "01006f8002326800000000000000000b.tik");
    /* The bytes at those offsets are the ones the builder wrote for those entries: an off-by-one
       in the data offset would show here rather than in a size comparison. */
    CHECK(ByteAt(reader, info.contents[0].offset) == 0x10);
    CHECK(ByteAt(reader, info.contents[1].offset) == 0x13);

    /* The same bytes with an .nsz name: same container, compressed payloads. */
    ContainerInfo nsz;
    CHECK(OpenContainer(reader, "game.nsz", image_size, &nsz, &error));
    CHECK(nsz.kind == ContainerKind::Nsz);
    CHECK(nsz.compressed);
    CHECK(nsz.contents.size() == 2);

    /* A card dump: "HEAD" header, root HFS0, secure partition. */
    auto xci = MakeXci({{"aaa.nca", 0x200}, {"bbb.nca", 0x100}});
    const std::uint64_t xci_size = xci.size();
    MemoryReader xci_reader(std::move(xci));
    ContainerInfo card;
    CHECK(OpenContainer(xci_reader, "card.xci", xci_size, &card, &error));
    CHECK(card.kind == ContainerKind::Xci);
    CHECK(!card.compressed);
    CHECK(card.contents.size() == 2);
    CHECK(card.extra.empty());
    CHECK(ByteAt(xci_reader, card.contents[0].offset) == 0x20);
    CHECK(ByteAt(xci_reader, card.contents[1].offset) == 0x21);

    ContainerInfo xcz;
    CHECK(OpenContainer(xci_reader, "card.xcz", xci_size, &xcz, &error));
    CHECK(xcz.kind == ContainerKind::Xcz);
    CHECK(xcz.compressed);

    /* Rejections.  Each one has to fail with an explanation, never with a half-filled result. */
    auto mod_in_disguise = MakePfs0({{"romfs/Model/Item.bin", 0x20}});
    MemoryReader mod_reader(std::move(mod_in_disguise));
    ContainerInfo mod;
    CHECK(!OpenContainer(mod_reader, "mod.nsp", 0, &mod, &error));
    CHECK(!error.empty());
    CHECK(mod.kind == ContainerKind::Unknown);

    auto nsp_again = MakePfs0(files);
    MemoryReader msp_reader(std::move(nsp_again));
    CHECK(!OpenContainer(msp_reader, "mod.msp", image_size, &info, &error));
    CHECK(error.find("mod package") != std::string::npos);

    MemoryReader garbage(std::vector<std::uint8_t>(0x200, 0xAB));
    CHECK(!OpenContainer(garbage, "thing.nsp", 0x200, &info, &error));
    CHECK(error.find("unknown container") != std::string::npos);

    /* The header says the data starts at `data_offset`, so a truncated transfer is caught before
       anything is installed. */
    MemoryReader truncated(std::vector<std::uint8_t>(data_offset, 0));
    CHECK(!OpenContainer(truncated, "game.nsp", data_offset, &info, &error));
    CHECK(!error.empty());

    /* An unknown extension is still parsed by magic (a wrong suffix is the user's typo, not a
       reason to refuse a valid package). */
    auto renamed = MakePfs0(files);
    MemoryReader renamed_reader(std::move(renamed));
    CHECK(OpenContainer(renamed_reader, "game.bin", image_size, &info, &error));
    CHECK(info.kind == ContainerKind::Nsp);

    CHECK(IsSupportedExtension("a.nsp") && IsSupportedExtension("A.NSZ"));
    CHECK(IsSupportedExtension("a.xci") && IsSupportedExtension("a.xcz"));
    CHECK(!IsSupportedExtension("a.msp") && !IsSupportedExtension("a.zip") &&
          !IsSupportedExtension("nsp"));
    CHECK(IsModPackageExtension("a.MSP") && !IsModPackageExtension("a.nsp"));
}

/* The defaults here are the reference implementation's, and the design document promises them
   item by item; a future options screen must not be able to reach a do-nothing combination. */
/* The task is the piece that survives a crash, so the cases below cover the byte accounting,
   the request order and the state-file round trip -- including the fields a hand-edited or
   half-written file could get wrong. */
/* CNMT and NCA header fixtures.  Both structs are byte layouts, so the tests build them field by
   field: the offsets are the ones nxdumptool, libnx and the reference implementation agree on,
   and a mistake here would surface as "the confirm page shows the wrong game". */
void PutU16(std::vector<std::uint8_t> &bytes, std::size_t at, std::uint16_t value) {
    bytes[at] = static_cast<std::uint8_t>(value & 0xFF);
    bytes[at + 1] = static_cast<std::uint8_t>(value >> 8);
}

std::vector<std::uint8_t> MakeCnmt(std::uint8_t type, std::uint16_t extended_size,
                                   const std::string &title_id_hex,
                                   const std::vector<std::tuple<std::string, std::uint32_t,
                                                                std::uint8_t, std::uint64_t>>
                                       &contents) {
    const std::size_t header = 0x20 + extended_size;
    const std::size_t total = header + 0x38 * contents.size() + 0x10; /* one content meta info */
    std::vector<std::uint8_t> bytes(total, 0);

    /* Real files store title ids little-endian while every readable form is big-endian, so the
       fixture reverses them here -- the same thing the parser undoes. */
    for (int i = 0; i < 8; ++i) {
        bytes[7 - i] = static_cast<std::uint8_t>(
            std::stoul(title_id_hex.substr(i * 2, 2), nullptr, 16));
    }
    PutU32(bytes, 8, 0x2228224); /* 3.0.3 */
    bytes[0x0C] = type;
    PutU16(bytes, 0x0E, extended_size);
    PutU16(bytes, 0x10, static_cast<std::uint16_t>(contents.size()));
    PutU16(bytes, 0x12, 1);
    if (type == 0x81 /* patch */ && extended_size >= 8) {
        for (int i = 0; i < 8; ++i) {
            bytes[0x20 + 7 - i] = static_cast<std::uint8_t>(
                std::stoul(std::string("01006F8002326800").substr(i * 2, 2), nullptr, 16));
        }
    }
    if (extended_size >= 12) {
        PutU32(bytes, 0x20 + 8, 0x100000); /* required system version */
    }
    for (std::size_t i = 0; i < contents.size(); ++i) {
        const std::string &id = std::get<0>(contents[i]);
        const std::uint8_t *entry = bytes.data() + header + i * 0x38;
        auto *mutable_entry = const_cast<std::uint8_t *>(entry);
        for (int b = 0; b < 16; ++b) {
            mutable_entry[0x20 + b] = static_cast<std::uint8_t>(std::stoul(id.substr(b * 2, 2), nullptr, 16));
        }
        PutU32(bytes, header + i * 0x38 + 0x20 + 0x10, std::get<1>(contents[i]));
        bytes[header + i * 0x38 + 0x20 + 0x14] = static_cast<std::uint8_t>(std::get<3>(contents[i]) >> 32);
        bytes[header + i * 0x38 + 0x20 + 0x16] = std::get<2>(contents[i]);
    }
    return bytes;
}

/* The QR encoder is vendored code with one job: turn the install page's address into a matrix
   the existing painter can draw.  The checks below are the ones that would catch a wrong or
   missing quiet-zone / finder-pattern / growth behaviour -- decoding the matrix would need a
   decoder we do not ship. */
void TestQrCode() {
    using acnh_manager::util::EncodeQr;

    const auto code = EncodeQr("http://192.168.8.160:1234");
    CHECK(!code.Empty());
    CHECK(code.size >= 21);
    CHECK((code.size - 21) % 4 == 0); /* QR versions grow four modules at a time */
    CHECK(static_cast<int>(code.bits.size()) == code.ByteCount());

    /* Top-left finder pattern: a 7x7 ring with a 3x3 core, white frame one module in. */
    CHECK(code.At(0, 0) && code.At(0, 6) && code.At(6, 0) && code.At(6, 6));
    CHECK(!code.At(1, 1) && !code.At(1, 5) && !code.At(5, 1) && !code.At(5, 5));
    CHECK(code.At(2, 2) && code.At(4, 4) && code.At(3, 3));
    /* The other two finder patterns sit in the opposite corners, not mirrored onto each other. */
    CHECK(code.At(0, code.size - 1) && code.At(code.size - 1, 0));
    /* Out-of-range modules read white instead of running off the buffer. */
    CHECK(!code.At(-1, 0) && !code.At(0, code.size) && !code.At(code.size, code.size));

    /* A longer payload needs a bigger version: the encoder is actually looking at the text. */
    const auto longer = EncodeQr(
        "http://192.168.8.160:1234/this-is-a-deliberately-longer-address/with-a-path");
    CHECK(!longer.Empty());
    CHECK(longer.size >= code.size);

    /* Over-long input fails cleanly (a matrix of size 0), never a half-filled one. */
    const auto too_long = EncodeQr(std::string(5000, 'x'), 3 /* H */);
    CHECK(too_long.Empty());
    CHECK(EncodeQr("").Empty());
    CHECK(EncodeQr("x", 9).Empty()); /* bad ECC level */
}

void TestNcaAndCnmt() {
    using acnh_manager::install::CnmtInfo;
    using acnh_manager::install::ContentType;
    using acnh_manager::install::MetaType;
    using acnh_manager::install::MetaTypeLabel;
    using acnh_manager::install::NcaHeaderInfo;
    using acnh_manager::install::ParseCnmt;
    using acnh_manager::install::ParseNcaHeader;
    using acnh_manager::install::SetDistributionToSystem;

    /* A 3.0.3 update: meta + program + control, like a real update package. */
    const auto cnmt = MakeCnmt(0x81, 0x10, "01006f8002326800",
                               /* 0 = Meta, 1 = Program, 3 = Control (NcmContentType, checked
                                  against libnx's enum -- the earlier fixtures used my own wrong
                                  ordering and hid the parser's mistake). */
                               {{"6f2fa0d0ab1c1f2a3d67c9e0b1a2c3d4", 0x1000, 0x00, 0x1000},
                                {"e10617820db06889e1638499478da0de", 0x40000000, 0x01, 0x40000000},
                                {"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 0x200, 0x03, 0x200}});
    CnmtInfo info;
    std::string error;
    CHECK(ParseCnmt(cnmt.data(), cnmt.size(), &info, &error));
    CHECK(error.empty());
    CHECK(info.title_id == "01006f8002326800");
    CHECK(info.version == 0x2228224);
    CHECK(info.type == MetaType::Patch);
    CHECK(MetaTypeLabel(info.type) == std::string("update"));
    CHECK(info.application_id == "01006f8002326800");
    CHECK(info.required_system_version == 0x100000);
    CHECK(info.contents.size() == 3);
    CHECK(info.contents[0].type == ContentType::Meta);
    CHECK(info.contents[1].type == ContentType::Program);
    CHECK(info.contents[1].size == 0x40000000);
    CHECK(info.contents[1].content_id == "e10617820db06889e1638499478da0de");

    /* A base game: no application id, and the type label is what the confirm page shows. */
    const auto base = MakeCnmt(0x80, 0x10, "01006f8002326000",
                               {{"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", 0x100, 0x01, 0x100}});
    CHECK(ParseCnmt(base.data(), base.size(), &info, &error));
    CHECK(info.type == MetaType::Application);
    CHECK(MetaTypeLabel(info.type) == std::string("base"));
    CHECK(info.application_id.empty());

    /* Broken payloads are refused instead of half-parsed. */
    std::vector<std::uint8_t> truncated(cnmt.begin(), cnmt.begin() + 0x18);
    CHECK(!ParseCnmt(truncated.data(), truncated.size(), &info, &error));
    CHECK(!error.empty());
    std::vector<std::uint8_t> lying = cnmt;
    PutU16(lying, 0x0E, 0x4000); /* an extended header longer than the payload */
    CHECK(!ParseCnmt(lying.data(), lying.size(), &info, &error));
    std::vector<std::uint8_t> empty_contents = cnmt;
    PutU16(empty_contents, 0x10, 0);
    CHECK(!ParseCnmt(empty_contents.data(), empty_contents.size(), &info, &error));

    /* NCA header: magic, distribution bit, content type, key generation, size, program id. */
    std::vector<std::uint8_t> nca(0xC00, 0);
    PutU32(nca, 0x200, 0x3341434E); /* "NCA3" */
    nca[0x204] = 1;                 /* gamecard: what an XCI's own content carries */
    nca[0x205] = 0x00;              /* program */
    nca[0x206] = 0x02;              /* old key generation 3.0.0 */
    nca[0x220] = 0x11;              /* key generation 17.0.0 */
    PutU64(nca, 0x208, 0x1000);
    for (int i = 0; i < 8; ++i) {
        nca[0x210 + 7 - i] = static_cast<std::uint8_t>(
            std::stoul(std::string("01006F8002326000").substr(i * 2, 2), nullptr, 16));
    }
    NcaHeaderInfo header;
    CHECK(ParseNcaHeader(nca.data(), nca.size(), &header, &error));
    CHECK(header.is_gamecard());
    CHECK(header.content_type == ContentType::Program);
    CHECK(header.key_generation == 0x11); /* the higher of the two wins */
    CHECK(header.size == 0x1000);
    CHECK(header.program_id == "01006f8002326000");

    /* The distribution rewrite is what makes a card dump launchable from SD/NAND. */
    SetDistributionToSystem(nca.data());
    CHECK(ParseNcaHeader(nca.data(), nca.size(), &header, &error));
    CHECK(!header.is_gamecard());
    CHECK(nca[0x204] == 0);

    /* Wrong magic and short buffers fail rather than report garbage. */
    std::vector<std::uint8_t> not_nca(nca);
    PutU32(not_nca, 0x200, 0x3241434E); /* NCA2 */
    CHECK(!ParseNcaHeader(not_nca.data(), not_nca.size(), &header, &error));
    CHECK(!ParseNcaHeader(nca.data(), 0x500, &header, &error));
}

void TestInstallTask() {
    using acnh_manager::install::Advance;
    using acnh_manager::install::ContentKind;
    using acnh_manager::install::DoneBytes;
    using acnh_manager::install::DoneContents;
    using acnh_manager::install::DumpTask;
    using acnh_manager::install::InstallTask;
    using acnh_manager::install::MatchesSourceFile;
    using acnh_manager::install::NextRange;
    using acnh_manager::install::PackageBytes;
    using acnh_manager::install::ParseTask;
    using acnh_manager::install::RangeRequest;
    using acnh_manager::install::TaskContent;
    using acnh_manager::install::TaskSource;
    using acnh_manager::install::TaskStage;
    using acnh_manager::install::TotalBytes;
    using acnh_manager::install::TransferComplete;

    /* A 3.0.3-style update: the meta NCA first (it carries the CNMT), then the big program NCA,
       then a small one that is already installed and therefore skipped.  The numbers are the
       shape of the real package: one multi-GB content and two tiny ones. */
    InstallTask task;
    task.task_id = "abcdef0123456789";
    task.source = TaskSource::HttpUpload;
    task.name = "[UPD][v3.0.3][01006F8002326800][v2228224].nsp";
    task.container_size = 4'700'000'000ULL;
    task.stage = TaskStage::Fetching;
    task.contents = {
        {"00000000000000000000000000000000.cnmt.nca", 0x1000, 0x800, 0, "0000", "", ContentKind::Meta,
         false},
        {"e10617820db06889e1638499478da0de.nca", 0x2000, 4'600'000'000ULL, 0,
         "e10617820db06889e1638499478da0de", "", ContentKind::Program, false},
        {"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.nca", 0x5000, 0x400, 0, "aaaa", "", ContentKind::Legal,
         true},
    };

    /* Progress is in bytes and skips the skipped content; the counter still counts it. */
    CHECK(TotalBytes(task) == 0x800 + 4'600'000'000ULL);
    /* The package's own size ignores the skip: the confirm screen has to be able to say how big
       the file the player picked is, even when every content in it is already on the console
       (that is the case the screen printed as "0.0 GB"). */
    CHECK(PackageBytes(task) == 0x800 + 4'600'000'000ULL + 0x400);
    CHECK(DoneBytes(task) == 0);
    CHECK(DoneContents(task) == 1);
    CHECK(!TransferComplete(task));

    /* The first request starts at the first content that needs bytes, is capped at the block
       size, and never crosses into the next content. */
    RangeRequest range;
    CHECK(NextRange(task, 4 * 1024 * 1024, &range));
    CHECK(range.index == 0);
    CHECK(range.offset == 0x1000);
    CHECK(range.size == 0x800);
    Advance(task, range, 0x800);
    CHECK(DoneBytes(task) == 0x800);

    CHECK(NextRange(task, 4 * 1024 * 1024, &range));
    CHECK(range.index == 1);
    CHECK(range.offset == 0x2000);
    CHECK(range.size == 4 * 1024 * 1024); /* capped, not the whole 4.6 GB */
    Advance(task, range, 4 * 1024 * 1024);
    CHECK(DoneBytes(task) == 0x800 + 4 * 1024 * 1024);

    /* A short read (the source stopped early) advances by what actually arrived. */
    CHECK(NextRange(task, 4 * 1024 * 1024, &range));
    Advance(task, range, 1000);
    CHECK(DoneBytes(task) == 0x800 + 4 * 1024 * 1024 + 1000);

    /* Run it to the end: the last request is clamped to what is missing, and the skipped content
       is never asked for. */
    int guard = 0;
    while (NextRange(task, 4 * 1024 * 1024, &range) && guard++ < 2000) {
        CHECK(range.index != 2);
        Advance(task, range, range.size);
    }
    CHECK(TransferComplete(task));
    CHECK(DoneBytes(task) == TotalBytes(task));
    CHECK(DoneContents(task) == 3);
    CHECK(!NextRange(task, 4 * 1024 * 1024, &range));

    /* Resume: the same file continues, a different file does not. */
    CHECK(MatchesSourceFile(task, task.name, task.container_size));
    CHECK(!MatchesSourceFile(task, "other.nsp", task.container_size));
    CHECK(!MatchesSourceFile(task, task.name, task.container_size + 1));
    CHECK(MatchesSourceFile(task, task.name, 0)); /* a source that cannot report a size */

    /* The placeholder id is what a resume writes into: it has to survive the state file. */
    task.contents[0].placeholder_id = "0123456789abcdef0123456789abcdef";
    InstallTask with_placeholder;
    std::string placeholder_error;
    CHECK(ParseTask(DumpTask(task), &with_placeholder, &placeholder_error));
    CHECK(with_placeholder.contents[0].placeholder_id == task.contents[0].placeholder_id);
    CHECK(with_placeholder.contents[1].placeholder_id.empty());

    /* State file round trip. */
    InstallTask resumed;
    std::string error;
    CHECK(ParseTask(DumpTask(task), &resumed, &error));
    CHECK(error.empty());
    CHECK(resumed.task_id == task.task_id);
    CHECK(resumed.source == TaskSource::HttpUpload);
    CHECK(resumed.name == task.name);
    CHECK(resumed.container_size == task.container_size);
    CHECK(resumed.stage == TaskStage::Fetching);
    CHECK(resumed.contents.size() == 3);
    CHECK(resumed.contents[1].content_id == "e10617820db06889e1638499478da0de");
    CHECK(resumed.contents[1].kind == ContentKind::Program);
    CHECK(resumed.contents[1].written == task.contents[1].written);
    CHECK(resumed.contents[2].skipped);
    CHECK(DoneBytes(resumed) == DoneBytes(task));

    /* The download source keeps its URL: resuming an interrupted download needs nothing from the
       player (the design promises that explicitly). */
    InstallTask download = task;
    download.source = TaskSource::HttpDownload;
    download.url = "https://example.com/update.nsp";
    InstallTask reloaded;
    CHECK(ParseTask(DumpTask(download), &reloaded, &error));
    CHECK(reloaded.source == TaskSource::HttpDownload);
    CHECK(reloaded.url == "https://example.com/update.nsp");

    /* Broken files are refused, never half-read. */
    InstallTask junk;
    CHECK(!ParseTask("not json", &junk, &error));
    CHECK(!error.empty());
    CHECK(!ParseTask("{\"schema\":99,\"contents\":[]}", &junk, &error));
    CHECK(!ParseTask("{\"schema\":1}", &junk, &error)); /* no contents */
    CHECK(!ParseTask(
        "{\"schema\":1,\"contents\":[{\"name\":\"a.nca\",\"offset\":1,\"size\":10,\"written\":11}]}",
        &junk, &error));
    CHECK(DumpTask(junk).find("contents") != std::string::npos); /* untouched on failure */
}

void TestInstallOptions() {
    using acnh_manager::install::InstallOptions;
    using acnh_manager::install::TargetStorage;

    const InstallOptions options;
    CHECK(options.target == TargetStorage::SdCard);
    CHECK(options.skip_if_already_installed);
    CHECK(!options.allow_downgrade);
    CHECK(options.skip_nca_hash_verify);
    CHECK(options.skip_rsa_header_fixed_key_verify);
    CHECK(options.skip_rsa_npdm_fixed_key_verify);
    CHECK(!options.ignore_distribution_bit);
    CHECK(options.convert_to_common_ticket);
    CHECK(!options.convert_to_standard_crypto);
    CHECK(!options.lower_master_key);
    CHECK(options.lower_system_version);
    CHECK(!options.skip_base && !options.skip_patch && !options.skip_addon &&
          !options.skip_data_patch && !options.skip_ticket);
    CHECK(!options.ticket_only);
    CHECK(!options.SkipsEverything());

    /* Only the common-ticket conversion is on by default, so the default path may need keys --
       which is why the ticket check has to report a missing prod.keys instead of skipping it. */
    CHECK(options.MayNeedKeys());
    InstallOptions no_keys;
    no_keys.convert_to_common_ticket = false;
    CHECK(!no_keys.MayNeedKeys());

    std::string error;
    CHECK(options.Validate(&error));

    InstallOptions nothing = options;
    nothing.skip_base = nothing.skip_patch = nothing.skip_addon = nothing.skip_data_patch = true;
    CHECK(!nothing.Validate(&error));
    CHECK(!error.empty());

    InstallOptions ticket_only = options;
    ticket_only.ticket_only = true;
    CHECK(!ticket_only.Validate(&error)); /* ticket-only still installs no content */
    ticket_only.skip_base = true;
    ticket_only.skip_patch = true;
    ticket_only.skip_addon = true;
    ticket_only.skip_data_patch = true;
    CHECK(ticket_only.Validate(&error));
    ticket_only.skip_ticket = true;
    CHECK(!ticket_only.Validate(&error));
}

/* The skip decision, which is what decides between "nothing to do" and "install": it is one
   question now -- is this exact version installed, by the console's own reckoning -- plus the
   downgrade rule.  The distinction matters because the console's meta database and its application
   record can disagree, and the answer for a version the console cannot use has to be "install". */
void TestDecideSkip() {
    using acnh_manager::install::DecideSkip;
    using acnh_manager::install::InstallOptions;
    using acnh_manager::install::SkipDecision;

    const InstallOptions options;
    constexpr std::uint32_t kVersion2 = 0x160000;
    constexpr std::uint32_t kVersion3 = 0x220000;

    /* Nothing there: install. */
    CHECK(DecideSkip(options, kVersion2, 0, false) == SkipDecision::Install);
    /* This exact version, and the console can use it: skip. */
    CHECK(DecideSkip(options, kVersion2, kVersion2, true) == SkipDecision::AlreadyInstalled);
    /* The same number, but the console's record does not point at it: install (this is the repair
       path -- the bytes are already in the storage, so only the finish step runs). */
    CHECK(DecideSkip(options, kVersion2, kVersion2, false) == SkipDecision::Install);
    /* Something newer is installed and downgrades are off: skip, and say which kind of skip. */
    CHECK(DecideSkip(options, kVersion2, kVersion3, false) == SkipDecision::NewerInstalled);

    InstallOptions upgraded = options;
    upgraded.allow_downgrade = true;
    CHECK(DecideSkip(upgraded, kVersion2, kVersion3, false) == SkipDecision::Install);

    InstallOptions forced = options;
    forced.skip_if_already_installed = false;
    CHECK(DecideSkip(forced, kVersion2, kVersion2, true) == SkipDecision::Install);
}

/* The installer's file picker is the app's first scrolling list; the cases below are the ones
   that would otherwise be discovered by scrolling on a console. */
void TestListView() {
    using acnh_manager::ui::ListItem;
    using acnh_manager::ui::ListView;

    ListView list;
    list.SetBox(800, ListView::kRowHeight * 3); /* exactly three rows fit */
    list.SetItems({{"..", "directory", true},
                   {"atmosphere", "directory", true},
                   {"readme.txt", "ignored", false},
                   {"[UPD] a.nsp", "package", true},
                   {"broken.nsp", "ignored", false},
                   {"[UPD] b.nsp", "package", true},
                   {"c.xci", "package", true}});

    /* The focus never lands on a row the player cannot pick, in either direction. */
    CHECK(list.Focus() == 0);
    list.MoveFocus(1);
    CHECK(list.Focus() == 1);
    list.MoveFocus(1);
    CHECK(list.Focus() == 3); /* skipped readme.txt */
    list.MoveFocus(-1);
    CHECK(list.Focus() == 1);
    list.SetFocus(2); /* a dimmed row cannot take the focus */
    CHECK(list.Focus() == 1);

    /* Scrolling keeps the focus visible and never leaves a blank row at the top. */
    /* Four steps down from item 0: 1, 3, 5, 6 -- the dimmed rows are stepped over, so the focus
       lands on the last item rather than the one before it. */
    list.SetFocus(0);
    list.MoveFocus(4);
    CHECK(list.Focus() == 6);
    CHECK(list.FirstVisible() <= list.Focus());
    CHECK(list.FirstVisible() + 3 > list.Focus());
    const auto window = list.Window();
    CHECK(window.count == 3);
    CHECK(window.first == 4);
    CHECK(list.FirstVisible() == 4);

    /* The ends stop rather than wrap. */
    list.MoveFocus(100);
    CHECK(list.Focus() == 6);
    list.MoveFocus(-100);
    CHECK(list.Focus() == 0);
    CHECK(list.FirstVisible() == 0);

    /* A tap maps to the row it landed on, and outside the box to nothing. */
    list.SetFocus(0);
    CHECK(list.RowAt(0) == 0);
    CHECK(list.RowAt(ListView::kRowHeight - 1) == 0);
    CHECK(list.RowAt(ListView::kRowHeight) == 1);
    CHECK(list.RowAt(3 * ListView::kRowHeight) == -1);
    CHECK(list.RowAt(-1) == -1);
    CHECK(list.RowAt(2 * ListView::kRowHeight) == 2); /* a dimmed row is still a row */

    /* A list of nothing but dimmed rows has no focus at all. */
    ListView none;
    none.SetBox(800, 200);
    none.SetItems({{"a.txt", "ignored", false}, {"b.txt", "ignored", false}});
    CHECK(none.Focus() == -1);
    none.MoveFocus(1);
    CHECK(none.Focus() == -1);

    /* An empty list is not a crash, and a short list does not scroll. */
    ListView empty;
    empty.SetBox(800, 200);
    empty.SetItems({});
    CHECK(empty.Focus() == -1);
    CHECK(empty.Window().count == 0);
    ListView one;
    one.SetBox(800, ListView::kRowHeight * 5);
    one.SetItems({{"only.nsp", "package", true}});
    CHECK(one.Window().first == 0);
    CHECK(one.Window().count == 5);
}

void TestStrings() {
    using acnh_manager::i18n::Language;
    using acnh_manager::i18n::StringId;
    using acnh_manager::i18n::StringCount;
    using acnh_manager::i18n::Text;
    /* The table size has to match the enum one-to-one, or a new string is easy to half-add. */
    /* The last enumerator doubles as the sentinel: adding a string without touching this
       line fails to build, which is exactly the reminder we want. */
    CHECK(StringCount() == static_cast<unsigned>(StringId::ProgressDoNotInterrupt) + 1u);
    for (unsigned i = 0; i <= static_cast<unsigned>(StringId::ProgressDoNotInterrupt); ++i) {
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
    /* Every home control carries its own key (A/X/Y), so the page has no focus at all: the
       ring used to sit on a button the d-pad could not leave, which read as a bug on hardware.
       Only keyless controls are focus targets. */
    CHECK(FirstEnabled(home) == -1);
    CHECK(MoveFocus(home, 0, 0, +1) == 0);
    CHECK(MoveFocus(home, -1, 0, +1) == -1);
    /* A page with one keyless control: it takes the focus, and a key-bound neighbour is not a
       candidate, so the d-pad cannot wander onto the X / Y buttons. */
    const std::vector<acnh_manager::ui::Action> with_row = {
        {9, acnh_manager::ui::Rect{40, 100, 500, 40}, true, true},
        {1, acnh_manager::ui::Rect{40, 200, 500, 40}, true, false},
    };
    CHECK(FirstEnabled(with_row) == 9);
    CHECK(MoveFocus(with_row, 9, 0, +1) == 9);

    /* Tap tracking.  The console only reports coordinates while a finger is down, so the
       release poll carries no position: the tracker has to remember the last one.  Reading the
       release poll directly made every real tap look like a screen-wide drag on hardware. */
    using acnh_manager::ui::TapTracker;
    {
        TapTracker tap;
        auto press = tap.Update(home, true, 640, 341);
        CHECK(press.press_edge);
        CHECK(press.pressed_action == 0);
        CHECK(press.tapped_action == -1); /* a press alone never fires */
        const auto release = tap.Update(home, false, 0, 0);
        CHECK(release.tapped_action == 0); /* release carries no position */
        CHECK(!tap.Down());
    }
    {
        /* Several frames down, then release: still a tap, and the focus follows the press. */
        TapTracker tap;
        const auto press = tap.Update(home, true, 200, 500);
        CHECK(press.press_edge);
        CHECK(press.pressed_action == 1);
        tap.Update(home, true, 205, 502);
        const auto release = tap.Update(home, false, 0, 0);
        CHECK(release.tapped_action == 1);
    }
    {
        /* Dragging further than the slop cancels, even when the finger comes back inside. */
        TapTracker tap;
        tap.Update(home, true, 300, 500);
        tap.Update(home, true, 900, 500);
        tap.Update(home, true, 300, 500);
        CHECK(tap.Update(home, false, 0, 0).tapped_action == -1);
    }
    {
        /* Releasing outside the pressed action does not fire, and a stray release is ignored. */
        TapTracker tap;
        tap.Update(home, true, 300, 500);
        tap.Update(home, true, 900, 500);
        CHECK(tap.Update(home, false, 0, 0).tapped_action == -1);
        CHECK(tap.Update(home, false, 0, 0).tapped_action == -1);
    }
    {
        /* A press that misses every control still tracks the finger but fires nothing. */
        TapTracker tap;
        const auto press = tap.Update(home, true, 640, 500);
        CHECK(press.press_edge);
        CHECK(press.pressed_action == -1);
        CHECK(tap.Update(home, false, 0, 0).tapped_action == -1);
    }
}

/* Header tabs.  The bug this guards against was reported from the console: the "L status" /
   "R details" pills were drawn but never registered as actions, so tapping them did nothing.
   These checks pin the properties that make them tappable where they are drawn. */
void TestHeaderTabs() {
    using acnh_manager::ui::HitTest;
    using acnh_manager::ui::kHeaderBarHeight;
    using acnh_manager::ui::kHeaderTabBadge;
    using acnh_manager::ui::kHeaderTabCount;
    using acnh_manager::ui::kHeaderTabTop;
    using acnh_manager::ui::LayoutHeaderTabs;

    /* The three header entries: "+ guide" / "L status" / "R details", measured on hardware. */
    const int label_width[kHeaderTabCount] = {70, 110, 120};
    const auto layout = LayoutHeaderTabs(1280, label_width);

    /* Every entry is hit-testable in the middle of the group it draws. */
    std::vector<acnh_manager::ui::Action> tabs;
    for (int i = 0; i < kHeaderTabCount; ++i) {
        tabs.push_back({i + 1, layout.hit[i], true});
    }
    for (int i = 0; i < kHeaderTabCount; ++i) {
        CHECK(HitTest(tabs, layout.badge_x[i] + kHeaderTabBadge / 2, kHeaderTabTop + 20) == i + 1);
        CHECK(HitTest(tabs, layout.label_x[i] + label_width[i] / 2, 56) == i + 1);
    }

    /* A tap can never be ambiguous, and the targets stay inside the header bar. */
    for (int i = 0; i + 1 < kHeaderTabCount; ++i) {
        CHECK(layout.hit[i].x + layout.hit[i].w <= layout.hit[i + 1].x);
        CHECK(layout.badge_x[i] < layout.badge_x[i + 1]);
        CHECK(layout.hit[i].y >= 0);
        CHECK(layout.hit[i].y + layout.hit[i].h <= kHeaderBarHeight);
    }
    CHECK(layout.hit[0].x >= 0);
    CHECK(layout.hit[kHeaderTabCount - 1].x + layout.hit[kHeaderTabCount - 1].w <= 1280);

    /* Wider labels push the group to the left instead of running off the screen. */
    const int wide[kHeaderTabCount] = {200, 200, 200};
    const auto wide_layout = LayoutHeaderTabs(1280, wide);
    CHECK(wide_layout.badge_x[0] < layout.badge_x[0]);
    CHECK(wide_layout.hit[kHeaderTabCount - 1].x + wide_layout.hit[kHeaderTabCount - 1].w <= 1280);
}

/* Home-screen state: the eight states and their priority.  Getting the order wrong would
   show "install" to someone whose game is not even installed, so it is pinned here. */
void TestHomeState() {
    using acnh_manager::ui::Classify;
    using acnh_manager::ui::HomeInputs;
    using acnh_manager::ui::HomeKind;

    HomeInputs ready; /* the healthy baseline: game found, supported, installed, current */
    ready.game_found = true;
    ready.supported = true;
    CHECK(Classify(ready) == HomeKind::UpToDate);

    HomeInputs no_game = ready;
    no_game.game_found = false;
    CHECK(Classify(no_game) == HomeKind::GameMissing);
    /* A missing game beats every other condition, including a leftover failure. */
    no_game.last_failed = true;
    no_game.supported = false;
    CHECK(Classify(no_game) == HomeKind::GameMissing);

    HomeInputs unsupported = ready;
    unsupported.supported = false;
    unsupported.fresh_install = true;
    CHECK(Classify(unsupported) == HomeKind::Unsupported);

    HomeInputs failed = ready;
    failed.last_failed = true;
    failed.fresh_install = true; /* a retry still shows the failure, not "install" */
    CHECK(Classify(failed) == HomeKind::Failed);

    HomeInputs repair = ready;
    repair.repair_needed = true;
    CHECK(Classify(repair) == HomeKind::Repair);
    repair.last_failed = true;
    CHECK(Classify(repair) == HomeKind::Failed);

    HomeInputs fresh = ready;
    fresh.fresh_install = true;
    CHECK(Classify(fresh) == HomeKind::NeedsInstall);
    fresh.repair_needed = true;
    CHECK(Classify(fresh) == HomeKind::Repair);

    HomeInputs newer = ready;
    newer.newer_agent = true;
    CHECK(Classify(newer) == HomeKind::UpdateAvailable);
    newer.fresh_install = true;
    CHECK(Classify(newer) == HomeKind::NeedsInstall);

    /* The card disagreeing with the record wins over the record's own verdict: that is the
       case where "installed" would be a lie (the game directory was deleted behind our back),
       and it outranks repair/needs-install because the player is looking at the wrong story. */
    HomeInputs incomplete = ready;
    incomplete.files_incomplete = true;
    CHECK(Classify(incomplete) == HomeKind::Incomplete);
    incomplete.repair_needed = true;
    CHECK(Classify(incomplete) == HomeKind::Incomplete);
    incomplete.last_failed = true;
    CHECK(Classify(incomplete) == HomeKind::Failed);
    HomeInputs missing_game = incomplete;
    missing_game.game_found = false;
    CHECK(Classify(missing_game) == HomeKind::GameMissing);
}

/* Settings (interface language and whatever follows it).  They live in their own file so they
   survive an uninstall, and the round trip has to be exact: a parse that quietly drops the
   language is how "the language resets after a restart" came back from the console. */
void TestSettings() {
    using acnh_manager::ui::DumpSettings;
    using acnh_manager::ui::ParseSettings;
    using acnh_manager::ui::Settings;
    using acnh_manager::i18n::Language;

    Settings written;
    written.language = Language::English;
    const std::string text = DumpSettings(written);
    CHECK(text.find("\"language\":\"en\"") != std::string::npos);

    Settings read;
    std::string error;
    CHECK(ParseSettings(text, &read, &error));
    CHECK(read.language == Language::English);
    CHECK(error.empty());

    /* Defaults when the language key is absent, and an unknown value keeps what we had. */
    Settings defaults;
    CHECK(ParseSettings("{\"schema\":1}", &defaults, &error));
    CHECK(defaults.language == Language::ZhHans);
    defaults.language = Language::English;
    CHECK(ParseSettings("{\"schema\":1,\"language\":\"klingon\"}", &defaults, &error));
    CHECK(defaults.language == Language::English);
    CHECK(!error.empty());

    /* Broken files and files from a newer schema are reported, not guessed at. */
    Settings junk;
    CHECK(!ParseSettings("not json", &junk, &error));
    CHECK(!ParseSettings("[1,2,3]", &junk, &error));
    CHECK(!ParseSettings("{\"schema\":99,\"language\":\"en\"}", &junk, &error));
    CHECK(junk.language == Language::ZhHans); /* untouched on failure */

    /* The names are stable identifiers: the enum can be renamed without resetting a choice. */
    CHECK(text == "{\"schema\":1,\"language\":\"en\"}");

    /* Both directions round trip, not just the one that happens to be the default. */
    Settings chinese;
    chinese.language = Language::ZhHans;
    Settings reloaded;
    reloaded.language = Language::English;
    CHECK(ParseSettings(DumpSettings(chinese), &reloaded, &error));
    CHECK(reloaded.language == Language::ZhHans);
    CHECK(DumpSettings(chinese) == "{\"schema\":1,\"language\":\"zh-Hans\"}");
}

}  // namespace

/* The id arithmetic every console query depends on: a package's meta id spells its *own* title
   (the update's, the add-on's), while the game's own data -- its name, version, cover -- is filed
   under the base application id.  Getting this wrong is invisible in the type system and shows up
   as "the console says that title is not installed". */
void TestApplicationIdOf() {
    using acnh_manager::install::ApplicationIdOf;
    using acnh_manager::install::MetaType;

    constexpr std::uint64_t kBase = 0x01006F8002326000ULL;
    CHECK(ApplicationIdOf(kBase, MetaType::Application) == kBase);
    CHECK(ApplicationIdOf(0x01006F8002326800ULL, MetaType::Patch) == kBase);
    CHECK(ApplicationIdOf(0x01006F8002327000ULL, MetaType::AddOnContent) == kBase);
    /* The add-on branch spells the reference implementation's arithmetic exactly -- an exclusive
       or and then a mask, not a subtraction -- and an id whose own nibble already carries the
       0x1000 bit is where the two spellings part ways. */
    CHECK(ApplicationIdOf(0x01006F800232A800ULL, MetaType::AddOnContent) == 0x01006F800232B000ULL);
}

/* The record the console stores, built from a CNMT: the layout is what the *service* reads back
   (there is no second parser to disagree with), so it is pinned here byte by byte.  The console's
   own record for an installed game is 8-byte header + extended header + 24-byte content infos, in
   that order, with the meta content listed first and no SHA-256 in front of each entry -- storing
   the CNMT file's bytes instead is what left a console unable to draw an installed title. */
void TestBuildMetaRecordBlob() {
    using acnh_manager::install::BuildMetaRecordBlob;
    using acnh_manager::install::CnmtContent;
    using acnh_manager::install::CnmtInfo;
    using acnh_manager::install::ContainerEntry;
    using acnh_manager::install::ContentType;
    using acnh_manager::install::MetaType;
    using acnh_manager::install::PackageInfo;

    /* A patch with two contents and a 24-byte extended header (the layout of a real update). */
    PackageInfo package;
    ContainerEntry meta;
    meta.name = "3c38f213ac463876a7cb515ae54d7599.cnmt.nca";
    meta.offset = 0x100;
    meta.size = 11776;
    package.meta_content_name = meta.name;
    package.container.contents.push_back(meta);
    package.cnmt.title_id = "01006f8002326800";
    package.cnmt.version = 0x160000;
    package.cnmt.type = MetaType::Patch;
    package.cnmt.extended_header_size = 24;
    package.cnmt.contents = {
        {"24ff98b9e4329ed7732a0bab58bc9e7a", ContentType::Program, 4229336576},
        {"f4adcc8d788455d78dd9d93899793b65", ContentType::Control, 1512448},
    };

    /* The CNMT payload as the device reads it: header at 0x0E, extended header, then one 0x38-byte
       entry per content (32-byte hash + 24-byte content info). */
    std::vector<std::uint8_t> payload(0x20 + 24, 0);
    payload[0x0C] = 0x81; /* Patch */
    payload[0x0E] = 24;   /* extended header size */
    payload[0x10] = 2;    /* content count */
    for (std::size_t i = 0; i < 2; ++i) {
        const std::size_t entry = 0x20 + 24 + i * 0x38;
        payload.resize(entry + 0x38, 0);
        const std::string &id = package.cnmt.contents[i].content_id;
        for (std::size_t b = 0; b < 16; ++b) {
            const auto hex = id.substr(b * 2, 2);
            payload[entry + 0x20 + b] = static_cast<std::uint8_t>(std::stoul(hex, nullptr, 16));
        }
        std::uint64_t size = package.cnmt.contents[i].size;
        for (std::size_t b = 0; b < 4; ++b) {
            payload[entry + 0x20 + 16 + b] = static_cast<std::uint8_t>((size >> (8 * b)) & 0xFF);
        }
        payload[entry + 0x20 + 20] = static_cast<std::uint8_t>((size >> 32) & 0xFF);
        /* Content type as the CNMT carries it: 1 = Program, 3 = Control (NcmContentType). */
        payload[entry + 0x20 + 22] = i == 0 ? 1 : 3;
    }
    package.meta_blob = payload;

    std::vector<std::uint8_t> blob;
    std::string error;
    CHECK(BuildMetaRecordBlob(package, &blob, &error));
    /* 8 header + 24 extended + 3 content infos (the meta content counts) + no meta infos. */
    CHECK(blob.size() == 8 + 24 + 3 * 0x18);
    CHECK(blob[0] == 24 && blob[1] == 0);      /* extended header size */
    CHECK(blob[2] == 3 && blob[3] == 0);       /* one more content than the CNMT lists */
    CHECK(blob[4] == 0 && blob[5] == 0);       /* no content-meta infos */
    CHECK(blob[6] == 0 && blob[7] == 0);       /* attributes, storage */
    /* The meta content comes first, with the type the service expects for it (0 = Meta). */
    CHECK(blob[8 + 24 + 0] == 0x3C && blob[8 + 24 + 1] == 0x38);
    CHECK(blob[8 + 24 + 16] == static_cast<std::uint8_t>(11776 & 0xFF));
    CHECK(blob[8 + 24 + 22] == 0);
    /* Then the CNMT's own contents, in order, with their types and sizes kept. */
    CHECK(blob[8 + 24 + 0x18 + 0] == 0x24 && blob[8 + 24 + 0x18 + 1] == 0xFF);
    CHECK(blob[8 + 24 + 0x18 + 22] == 1);
    CHECK(blob[8 + 24 + 0x30 + 0] == 0xF4 && blob[8 + 24 + 0x30 + 1] == 0xAD);
    CHECK(blob[8 + 24 + 0x30 + 22] == 3);
    /* The hashes are not part of the record: each entry is exactly 24 bytes. */
    CHECK(blob[8 + 24 + 0x18 + 16] == static_cast<std::uint8_t>(4229336576 & 0xFF));

    /* A truncated CNMT is refused rather than half-copied. */
    PackageInfo broken = package;
    broken.meta_blob.resize(0x20 + 24 + 0x38);
    CHECK(!BuildMetaRecordBlob(broken, &blob, &error));
    CHECK(!error.empty());
}

int main() {
    TestContainer();
    TestQrCode();
    TestNcaAndCnmt();
    TestInstallTask();
    TestApplicationIdOf();
    TestBuildMetaRecordBlob();
    TestInstallOptions();
    TestDecideSkip();
    TestJsonBasics();
    TestJsonRejects();
    TestJsonDumpRoundTrip();
    TestManifestOk();
    TestManifestRejects();
    TestHelpers();
    TestCheatFile();
    TestGate();
    TestUpdateAdoption();
    TestPlan();
    TestOverrideConfig();
    TestListView();
    TestStrings();
    TestSha256();
    TestTimeFormat();
    TestActions();
    TestHeaderTabs();
    TestHomeState();
    TestSettings();
    TestTextWrap();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
