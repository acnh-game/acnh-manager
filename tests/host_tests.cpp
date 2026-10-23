/* Host-side unit tests: manifest parsing/validation, gate verdicts, install decisions and
   the state.json round trip.  These modules deliberately avoid libnx, so they run on a
   development machine:
       make -C tests
   The numbers in the cases below come from real hardware (docs/architecture.md section 5). */

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <tuple>
#include <regex>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <zstd.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "env/cheat_file.hpp"
#include "env/config_ini.hpp"
#include "env/forwarder.hpp"
#include "env/startup_check.hpp"
#include "i18n/strings.hpp"
#include "install/gate.hpp"
#include "install/container.hpp"
#include "install/install_task.hpp"
#include "install/nca.hpp"
#include "install/options.hpp"
#include "install/package.hpp"
#include "install/icon_file.hpp"
#include "install/ncz.hpp"
#include "install/ncz_decode.hpp"
#include "install/range_probe.hpp"
#include "install/rate_meter.hpp"
#include "install/source_memory.hpp"
#include "install/title_name.hpp"
#include "install/transfer_queue.hpp"
#include "net/update_policy.hpp"
#include "util/json.hpp"
#include "util/size_text.hpp"
#include "util/qr_code.hpp"
#include "manifest/manifest.hpp"
#include "util/sha256.hpp"
#include "ui/action.hpp"
#include "ui/footer_version.hpp"
#include "ui/header_tabs.hpp"
#include "ui/home_state.hpp"
#include "ui/list_view.hpp"
#include "ui/settings.hpp"
#include "ui/storage_bars.hpp"
#include "install/upload_protocol.hpp"
#include "net/http_page.hpp"
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
    /* The two shapes this writer stands in for do not share an entry size: PFS0's is 0x18, HFS0's
       is 0x40 (it carries a reserved word and a 0x20-byte hash after the four common fields).
       Writing 0x18 entries into the cartridge-dump fixture is what kept the parser's own wrong
       stride invisible to these tests -- so the size follows the magic now. */
    const std::size_t entry_size = magic == kHfs0Magic ? 0x40 : 0x18;
    const std::size_t table_size = entry_size * files.size();
    const std::size_t data_offset = offset + 0x10 + table_size + strings.size();

    PutU32(bytes, offset, magic);
    PutU32(bytes, offset + 4, static_cast<std::uint32_t>(files.size()));
    PutU32(bytes, offset + 8, static_cast<std::uint32_t>(strings.size()));
    std::uint64_t relative = 0;
    for (std::size_t i = 0; i < files.size(); ++i) {
        PutU64(bytes, offset + 0x10 + i * entry_size, relative);
        PutU64(bytes, offset + 0x18 + i * entry_size, files[i].second);
        PutU32(bytes, offset + 0x20 + i * entry_size, name_offsets[i]);
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
    /* Both tables are HFS0: 0x40 bytes per entry (see WriteHfs0). */
    const std::size_t secure_size = 0x10 + 0x40 * secure_files.size() + secure_strings +
                                    secure_payload;
    /* Root partition: header + two entries + the strings "secure\0" and "logo\0". */
    const std::size_t root_size = 0x10 + 0x40 * 2 + 7 + 5;
    std::vector<std::uint8_t> bytes(0xF000 + root_size + secure_size, 0);

    /* "HEAD" the way a real dump stores it: the four bytes, so the little-endian read the parser
       does has to see 0x44414548.  Writing the big-endian spelling here (which is what this line
       used to do) is how the parser's own mistake stayed invisible to the tests. */
    PutU32(bytes, 0x100, 0x44414548);
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

/* A checked-in file out of the repository, as bytes.  The tests run from `tests/`, so the path is
   relative to that; a missing file is a failure of the test's own setup (the file is in the repo,
   not in the environment), which is why the caller checks for the empty string. */
std::string ReadRepoFile(const char *relative_path) {
    std::ifstream input(std::string("../") + relative_path, std::ios::binary);
    if (!input) {
        std::printf("test input not found: %s\n", relative_path);
        return std::string();
    }
    return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
}

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
    CHECK(!result.manifest.manager.present);

    std::string with_manager = SampleManifest();
    const std::size_t closing = with_manager.find_last_of('}');
    with_manager.insert(closing, R"(,
  "manager": {
    "version": "2.1.0",
    "url": "https://example.com/acnh-manager-2.1.0.nro",
    "size": 3819661,
    "sha256": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
  }
)");
    const auto managed = acnh_manager::manifest::Parse(with_manager, "2.0.0");
    CHECK(managed.ok);
    CHECK(managed.manifest.manager.present);
    CHECK(managed.manifest.manager.version == "2.1.0");
    CHECK(managed.manifest.manager.size == 3819661);
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
        {"manager-http",
         ReplaceOnce(base, "\"agent\": {",
                     "\"manager\":{\"version\":\"2.1.0\",\"url\":\"http://example.com/a.nro\","
                     "\"size\":1,\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"},"
                     "\"agent\": {"),
         "https"},
        {"manager-sha",
         ReplaceOnce(base, "\"agent\": {",
                     "\"manager\":{\"version\":\"2.1.0\",\"url\":\"https://example.com/a.nro\","
                     "\"size\":1,\"sha256\":\"abc\"},\"agent\": {"),
         "64 hex"},
        {"manager-size",
         ReplaceOnce(base, "\"agent\": {",
                     "\"manager\":{\"version\":\"2.1.0\",\"url\":\"https://example.com/a.nro\","
                     "\"size\":999999999,\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"},"
                     "\"agent\": {"),
         "size"},
        {"manager-type",
         ReplaceOnce(base, "\"agent\": {", "\"manager\": [], \"agent\": {"),
         "manager is not an object"},
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

    acnh_manager::manifest::ManagerRelease manager;
    manager.present = true;
    manager.version = "2.1.0";
    CHECK(acnh_manager::net::ManagerReleaseAvailable(manager, "2.0.0"));
    CHECK(!acnh_manager::net::ManagerReleaseAvailable(manager, "2.1.0"));
    CHECK(!acnh_manager::net::ManagerReleaseAvailable(manager, "2.2.0"));
    manager.present = false;
    CHECK(!acnh_manager::net::ManagerReleaseAvailable(manager, "2.0.0"));

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

    /* A container whose *meta content* is compressed too: a tool that compresses more than the
       reference's own dumper does names it `<id>.cnmt.ncz`, and the package read has to find it --
       otherwise the install reports "this package has no meta content" on a package that plainly
       has one (the reference implementation looks for both spellings). */
    {
        auto packed_meta = MakePfs0({{"3c38f213ac463876a7cb515ae54d7599.cnmt.ncz", 0x200},
                                     {"80cbc793d37b6f493ba53561106bb5c6.ncz", 0x100}});
        const std::uint64_t packed_size = packed_meta.size();
        MemoryReader packed_reader(std::move(packed_meta));
        acnh_manager::install::PackageInfo package;
        CHECK(ReadPackage(packed_reader, "game.nsz", packed_size, &package, &error));
        CHECK(package.meta_content_name == "3c38f213ac463876a7cb515ae54d7599.cnmt.ncz");
    }

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

    /* The contents of a compressed container are `.ncz` entries, and the parser has to hand them
       over as compressed contents carrying the bytes they occupy on the card -- the decoded size
       only becomes known when the NCZ tables are read (host side: TestNczTables; on the device:
       GameInstall::Prepare, which is the same path the NSZ install verified).  Both container
       families are checked, because an XCZ compresses inside its partitions exactly like an NSZ
       does at the top level (the reference's own dumper only compresses the Program and PublicData
       NCAs and copies every table verbatim). */
    const char *kNczName = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.ncz";
    const char *kNcaName = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb.nca";
    auto nsz_contents = MakePfs0({{kNczName, 0x200}, {kNcaName, 0x100}});
    const std::uint64_t nsz_size = nsz_contents.size();
    MemoryReader nsz_reader(std::move(nsz_contents));
    ContainerInfo packed;
    CHECK(OpenContainer(nsz_reader, "game.nsz", nsz_size, &packed, &error));
    CHECK(packed.compressed);
    CHECK(packed.contents.size() == 2);
    CHECK(packed.contents[0].name == kNczName);
    CHECK(packed.contents[0].compressed);
    CHECK(packed.contents[0].packed_size == 0x200);
    CHECK(!packed.contents[1].compressed);
    CHECK(packed.contents[1].size == packed.contents[1].packed_size);

    auto xcz_contents = MakeXci({{kNczName, 0x200}, {kNcaName, 0x100}});
    const std::uint64_t xcz_size = xcz_contents.size();
    MemoryReader xcz_reader(std::move(xcz_contents));
    ContainerInfo compressed_card;
    CHECK(OpenContainer(xcz_reader, "card.xcz", xcz_size, &compressed_card, &error));
    CHECK(compressed_card.kind == ContainerKind::Xcz);
    CHECK(compressed_card.compressed);
    CHECK(compressed_card.contents.size() == 2);
    CHECK(compressed_card.contents[0].compressed);
    CHECK(compressed_card.contents[0].packed_size == 0x200);
    CHECK(!compressed_card.contents[1].compressed);

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

    /* The built-in chat-code guide URL must fit the same runtime encoder used by the guide page. */
    const auto guide = EncodeQr("https://lextuo.com/acnh-chat-code/guide/", 3);
    CHECK(!guide.Empty());
    CHECK(guide.At(0, 0) && guide.At(0, 6) && guide.At(6, 0));

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
    /* No rights id: standard crypto, which is what a cartridge dump carries -- and the reason
       those install without any ticket. */
    CHECK(header.rights_id.empty());

    /* A rights id means the content is encrypted with that title key, so the package's ticket has
       to be imported before the console can read it (the update packages from Nintendo carry one
       *.tik + .cert* for exactly that).  All-zero means "none", anything else is kept verbatim. */
    std::vector<std::uint8_t> keyed(nca);
    const std::string rights = "01006f8002326800000000000000000b";
    for (std::size_t i = 0; i < 16; ++i) {
        keyed[0x230 + i] = static_cast<std::uint8_t>(std::stoul(rights.substr(i * 2, 2), nullptr,
                                                              16));
    }
    CHECK(ParseNcaHeader(keyed.data(), keyed.size(), &header, &error));
    CHECK(header.rights_id == rights);

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

/* The ticket pairs a package carries: `<rights id>.tik` next to `<rights id>.cert`, one per title
   the container holds.  A content encrypted with a title key needs exactly this imported before
   the console can read it, which is why a real update package from Nintendo ships one (measured on
   `[UPD][v3.0.3][01006F8002326800]`: every content is standard crypto except the program, whose
   rights id is `01006f8002326800000000000000000b`, and the container carries that pair).  Only the
   first pair used to be read, so a container holding two encrypted titles would leave the second
   one unreadable, and a ticket without its certificate was accepted silently. */
void TestPackageTickets() {
    using acnh_manager::install::PackageInfo;
    using acnh_manager::install::ReadPackage;

    const char *kMeta = "1427c6a5b5ddbea8d99b4f2cfe94973c.cnmt.nca";
    const std::string rights_a = "01006f8002326800000000000000000b";
    const std::string rights_b = "01000000000108000000000000000004";
    auto bytes = MakePfs0({{kMeta, 0x100},
                           {rights_a + ".tik", 0x100},
                           {rights_a + ".cert", 0x200},
                           {rights_b + ".tik", 0x180},
                           {rights_b + ".cert", 0x300}});
    const std::uint64_t size = bytes.size();
    MemoryReader reader(std::move(bytes));
    PackageInfo package;
    std::string error;
    CHECK(ReadPackage(reader, "game.nsp", size, &package, &error));
    CHECK(package.tickets.size() == 2);
    CHECK(package.tickets[0].rights_id == rights_a);
    CHECK(package.tickets[0].ticket.size() == 0x100);
    CHECK(package.tickets[0].certificate.size() == 0x200);
    CHECK(package.tickets[1].rights_id == rights_b);
    CHECK(package.tickets[1].ticket.size() == 0x180);
    CHECK(package.tickets[1].certificate.size() == 0x300);

    /* A ticket whose certificate is missing is refused: it cannot be imported, and finding that out
       while installing is worse than finding it out here. */
    auto orphan = MakePfs0({{kMeta, 0x100}, {rights_a + ".tik", 0x100}});
    MemoryReader orphan_reader(std::move(orphan));
    PackageInfo broken;
    CHECK(!ReadPackage(orphan_reader, "game.nsp", 0, &broken, &error));
    CHECK(!error.empty());
}

/* The transfer's hand-off queue (install/transfer_queue.hpp): the reader thread fills slots, the UI
   thread drains them.  What matters for the install is that the order is kept exactly (the
   placeholder offsets depend on it), that a full queue is waited on rather than dropped, and that
   stopping or failing wakes both sides -- the engine's reader waits on this from another thread,
   so none of it can be left to "it usually works". */
void TestTransferQueue() {
    using acnh_manager::install::PreparedRange;
    using acnh_manager::install::TransferQueue;

    TransferQueue queue(3, 64);

    /* Three slots, three ranges, published and consumed in that order. */
    for (int i = 0; i < 3; ++i) {
        std::uint8_t *slot = queue.AcquireSlot();
        CHECK(slot != nullptr);
        slot[0] = static_cast<std::uint8_t>(i);
        PreparedRange prepared;
        prepared.range.index = static_cast<std::size_t>(i);
        prepared.range.offset = static_cast<std::uint64_t>(i) * 100;
        prepared.content_offset = static_cast<std::uint64_t>(i) * 10;
        prepared.size = 7 + static_cast<std::size_t>(i);
        queue.Publish(prepared);
    }
    for (int i = 0; i < 3; ++i) {
        PreparedRange prepared;
        std::uint8_t *data = nullptr;
        std::string error;
        CHECK(queue.Next(&prepared, &data, &error, 0) == TransferQueue::Take::Got);
        CHECK(prepared.range.index == static_cast<std::size_t>(i));
        CHECK(prepared.range.offset == static_cast<std::uint64_t>(i) * 100);
        CHECK(prepared.content_offset == static_cast<std::uint64_t>(i) * 10);
        CHECK(prepared.size == 7 + static_cast<std::size_t>(i));
        CHECK(data != nullptr && data[0] == static_cast<std::uint8_t>(i));
        queue.Release();
    }
    /* Drained: an empty queue answers Empty (with the deadline) rather than blocking the UI. */
    {
        PreparedRange prepared;
        std::uint8_t *data = nullptr;
        std::string error;
        CHECK(queue.Next(&prepared, &data, &error, 0) == TransferQueue::Take::Empty);
    }
    /* The slots came back, so the ring wraps and keeps the order: three more fit, and the first of
       them comes out first. */
    for (int i = 0; i < 3; ++i) {
        std::uint8_t *slot = queue.AcquireSlot();
        CHECK(slot != nullptr);
        PreparedRange prepared;
        prepared.range.index = static_cast<std::size_t>(100 + i);
        queue.Publish(prepared);
    }
    {
        PreparedRange prepared;
        std::uint8_t *data = nullptr;
        std::string error;
        CHECK(queue.Next(&prepared, &data, &error, 0) == TransferQueue::Take::Got);
        CHECK(prepared.range.index == 100);
        queue.Release();
    }

    /* A full queue makes the producer *wait* rather than drop work, and a release lets it through --
       the engine's reader blocks in AcquireSlot while the writer works, so this is the property the
       transfer depends on.  Two items are queued from the wrap check above, so one more fills it. */
    {
        std::uint8_t *last = queue.AcquireSlot();
        CHECK(last != nullptr);
        PreparedRange filler;
        filler.range.index = 5;
        queue.Publish(filler); /* three queued, no free slot */

        std::atomic<bool> acquired{false};
        std::thread producer([&] {
            std::uint8_t *slot = queue.AcquireSlot(); /* blocks until the consumer releases one */
            if (slot != nullptr) {
                slot[0] = 0xAB;
                PreparedRange published;
                published.range.index = 7;
                queue.Publish(published);
            }
            acquired = true;
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        CHECK(!acquired.load()); /* still blocked: no slot free */

        /* Take one (which is what frees its slot) and give it back. */
        PreparedRange prepared;
        std::uint8_t *data = nullptr;
        std::string error;
        CHECK(queue.Next(&prepared, &data, &error, 0) == TransferQueue::Take::Got);
        CHECK(prepared.range.index == 101);
        queue.Release();
        producer.join();
        CHECK(acquired.load());

        /* Drain what is left: two from the ring plus the producer's range. */
        int drained = 0;
        bool saw_producer = false;
        for (;;) {
            PreparedRange item;
            std::uint8_t *item_data = nullptr;
            std::string item_error;
            if (queue.Next(&item, &item_data, &item_error, 0) != TransferQueue::Take::Got) {
                break;
            }
            if (item.range.index == 7) {
                saw_producer = true;
                CHECK(item_data[0] == 0xAB);
            }
            queue.Release();
            ++drained;
        }
        CHECK(drained == 3);
        CHECK(saw_producer);
    }

    /* Finished: the consumer hears "no more" once the queue is empty, and keeps hearing it. */
    queue.Finish();
    {
        PreparedRange prepared;
        std::uint8_t *data = nullptr;
        std::string error;
        CHECK(queue.Next(&prepared, &data, &error, 0) == TransferQueue::Take::Finished);
        CHECK(queue.Next(&prepared, &data, &error, 0) == TransferQueue::Take::Finished);
    }

    /* A failure reaches the consumer with its message, and the producer stops instead of filling
       slots nobody will read. */
    TransferQueue failing(2, 32);
    failing.Fail("the source stopped early");
    {
        PreparedRange prepared;
        std::uint8_t *data = nullptr;
        std::string error;
        CHECK(failing.Next(&prepared, &data, &error, 0) == TransferQueue::Take::Failed);
        CHECK(error == "the source stopped early");
    }
    CHECK(failing.AcquireSlot() == nullptr);

    /* Stopping wakes both sides: the producer gets no slot, and the consumer hears Stopped even with
       an item still queued (the writer must not keep writing while the app is leaving). */
    TransferQueue stopped(2, 32);
    stopped.Publish(PreparedRange{});
    stopped.RequestStop();
    CHECK(stopped.StopRequested());
    CHECK(stopped.AcquireSlot() == nullptr);
    {
        PreparedRange prepared;
        std::uint8_t *data = nullptr;
        std::string error;
        CHECK(stopped.Next(&prepared, &data, &error, 0) == TransferQueue::Take::Stopped);
    }
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
        {"00000000000000000000000000000000.cnmt.nca", 0x1000, 0x800, 0x800, false, 0, "0000", "",
         ContentKind::Meta, false},
        {"e10617820db06889e1638499478da0de.nca", 0x2000, 4'600'000'000ULL, 4'600'000'000ULL, false,
         0, "e10617820db06889e1638499478da0de", "", ContentKind::Program, false},
        {"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.nca", 0x5000, 0x400, 0x400, false, 0, "aaaa", "",
         ContentKind::Legal, true},
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

    /* The internal source is never written as a resumable task, but it still has a stable
       spelling if one is inspected or logged. */
    InstallTask internal = task;
    internal.source = TaskSource::Internal;
    InstallTask internal_reloaded;
    CHECK(ParseTask(DumpTask(internal), &internal_reloaded, &error));
    CHECK(internal_reloaded.source == TaskSource::Internal);

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
    /* Every ticket conversion is off: this app imports the package's ticket as it is and never
       reads prod.keys, so it must not claim it might. */
    CHECK(!options.convert_to_common_ticket);
    CHECK(!options.convert_to_standard_crypto);
    CHECK(!options.lower_master_key);
    CHECK(options.lower_system_version);
    CHECK(!options.skip_base && !options.skip_patch && !options.skip_addon &&
          !options.skip_data_patch && !options.skip_ticket);
    CHECK(!options.ticket_only);
    CHECK(!options.SkipsEverything());

    /* With every conversion off the default run needs no keys at all; turning one on is what makes
       the caller warn about prod.keys (the flag exists for that sentence, not for a code path). */
    CHECK(!options.MayNeedKeys());
    InstallOptions wants_keys;
    wants_keys.convert_to_common_ticket = true;
    CHECK(wants_keys.MayNeedKeys());

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
    using acnh_manager::install::MetaType;
    using acnh_manager::install::SkipDecision;

    const InstallOptions options;
    constexpr std::uint32_t kVersion2 = 0x160000;
    constexpr std::uint32_t kVersion3 = 0x220000;

    /* Nothing there: install. */
    CHECK(DecideSkip(options, MetaType::Patch, kVersion2, 0, false) == SkipDecision::Install);
    /* This exact version, and the console can use it: skip. */
    CHECK(DecideSkip(options, MetaType::Patch, kVersion2, kVersion2, true) ==
          SkipDecision::AlreadyInstalled);
    /* The same number, but the console's record does not point at it: install (this is the repair
       path -- the bytes are already in the storage, so only the finish step runs). */
    CHECK(DecideSkip(options, MetaType::Patch, kVersion2, kVersion2, false) ==
          SkipDecision::Install);
    /* An older *update* against a newer one installed, downgrades off: skip, and say which kind of
       skip. */
    CHECK(DecideSkip(options, MetaType::Patch, kVersion2, kVersion3, false) ==
          SkipDecision::NewerInstalled);
    /* A base game or an add-on is never held back that way: a base game whose contents were deleted
       while its update's record stayed behind has to install, or the title can never be repaired
       (the reference implementation compares versions for patches alone).  The launch version is
       what keeps the newer update in place. */
    CHECK(DecideSkip(options, MetaType::Application, kVersion2, kVersion3, false) ==
          SkipDecision::Install);
    CHECK(DecideSkip(options, MetaType::AddOnContent, kVersion2, kVersion3, false) ==
          SkipDecision::Install);

    InstallOptions upgraded = options;
    upgraded.allow_downgrade = true;
    CHECK(DecideSkip(upgraded, MetaType::Patch, kVersion2, kVersion3, false) ==
          SkipDecision::Install);

    InstallOptions forced = options;
    forced.skip_if_already_installed = false;
    CHECK(DecideSkip(forced, MetaType::Patch, kVersion2, kVersion2, true) == SkipDecision::Install);
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
    CHECK(StringCount() == static_cast<unsigned>(StringId::ValueInstalled) + 1u);
    for (unsigned i = 0; i <= static_cast<unsigned>(StringId::ValueInstalled); ++i) {
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
        {1, Rect{120, 450, 508, 110}, true},   /* check for updates */
        {2, Rect{652, 450, 508, 110}, true},   /* uninstall */
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
    using acnh_manager::ui::kHeaderTabBadgeGap;
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

    /* The header's real badge arrangement: + installer, Ⓛ status, details (none), guide Ⓡ.  The
       closing badge sits *after* its label, so the two shoulder keys bracket the group they move
       through; the middle of the ring reserves nothing at all; and every entry -- including the
       one whose badge is outside its label -- stays hit-testable across what it draws. */
    const acnh_manager::ui::TabBadge arrangement[kHeaderTabCount] = {
        acnh_manager::ui::TabBadge::Before, acnh_manager::ui::TabBadge::Before,
        acnh_manager::ui::TabBadge::None, acnh_manager::ui::TabBadge::After};
    const auto ring_layout = LayoutHeaderTabs(1280, label_width, arrangement);
    CHECK(ring_layout.badge_x[2] == ring_layout.label_x[2]);
    CHECK(ring_layout.label_x[2] == layout.label_x[2]);
    CHECK(ring_layout.badge_x[3] ==
          ring_layout.label_x[3] + label_width[3] + kHeaderTabBadgeGap);
    CHECK(ring_layout.label_x[0] > layout.label_x[0]); /* the strip closes up to its right end */
    std::vector<acnh_manager::ui::Action> ring_tabs;
    for (int i = 0; i < kHeaderTabCount; ++i) {
        ring_tabs.push_back({i + 1, ring_layout.hit[i], true});
    }
    CHECK(HitTest(ring_tabs, ring_layout.label_x[2] + label_width[2] / 2, 56) == 3);
    /* The closing Ⓡ is inside its own entry's target, not outside it. */
    CHECK(HitTest(ring_tabs, ring_layout.badge_x[3] + kHeaderTabBadge / 2, kHeaderTabTop + 20) == 4);
    for (int i = 0; i + 1 < kHeaderTabCount; ++i) {
        CHECK(ring_layout.hit[i].x + ring_layout.hit[i].w <= ring_layout.hit[i + 1].x);
    }
    CHECK(ring_layout.hit[0].x >= 0);
    CHECK(ring_layout.hit[kHeaderTabCount - 1].x + ring_layout.hit[kHeaderTabCount - 1].w <= 1280);
}

/* The upload protocol's rules, without a socket: which part the console accepts, and how a query
   string is read.  Both are the kind of thing that fails only on hardware (a part written at the
   wrong offset corrupts a content silently). */
void TestUploadProtocol() {
    using acnh_manager::install::kUploadPartBytes;
    using acnh_manager::install::UploadPartMatches;
    using acnh_manager::net::QueryValue;

    /* The part the reader is waiting for, and only that one. */
    CHECK(UploadPartMatches(0, 512, 0, 512));
    CHECK(UploadPartMatches(4096, kUploadPartBytes, 4096, kUploadPartBytes));
    CHECK(UploadPartMatches(0, 4096, 0, 512)); /* a shorter part is fine (the page may slice) */
    /* Everything else is a 409: a different offset, a longer part, an empty one. */
    CHECK(!UploadPartMatches(0, 512, 512, 512));
    CHECK(!UploadPartMatches(4096, 512, 0, 512));
    CHECK(!UploadPartMatches(0, 512, 0, 0));
    CHECK(!UploadPartMatches(0, 512, 0, 1024));
    CHECK(!UploadPartMatches(0, 512, 0, kUploadPartBytes + 1));

    /* Query strings: the page writes `?offset=123&taskId=ab`, and a missing key is empty rather
       than a guess. */
    CHECK(QueryValue("offset=123&taskId=ab", "offset") == "123");
    CHECK(QueryValue("offset=123&taskId=ab", "taskId") == "ab");
    CHECK(QueryValue("taskId=ab", "taskId") == "ab");
    CHECK(QueryValue("offset=1", "taskId").empty());
    CHECK(QueryValue("", "offset").empty());
    CHECK(QueryValue("offset=", "offset").empty());
    CHECK(QueryValue("noffset=1&offset=2", "offset") == "2"); /* no prefix matching */
}

/* The installer's web page, without a socket: escaping, the label substitution and the response
   head.  These are the parts that are easy to get subtly wrong and impossible to see on the console
   (a label with a `"` or a `</script>` in it breaks the page silently, and a wrong Content-Length
   hangs the browser). */
void TestHttpPage() {
    using acnh_manager::net::BuildPageLabelsJson;
    using acnh_manager::net::BuildResponseHead;
    using acnh_manager::net::DrainBytes;
    using acnh_manager::net::EscapeForJs;
    using acnh_manager::net::SubstituteLabels;

    CHECK(EscapeForJs("plain") == "plain");
    CHECK(EscapeForJs("a\"b") == "a\\\"b");
    CHECK(EscapeForJs("back\\slash") == "back\\\\slash");
    CHECK(EscapeForJs("line\nbreak") == "line\\nbreak");
    /* The one that matters: a translation containing "</script>" must not be able to end the block
       the labels live in. */
    CHECK(EscapeForJs("</script>") == "\\u003c/script\\u003e");

    /* Every occurrence is replaced, and a page without the marker comes back unchanged. */
    CHECK(SubstituteLabels("<i>__X__</i> and __X__", "__X__", "L") == "<i>L</i> and L");
    CHECK(SubstituteLabels("no marker here", "__X__", "L") == "no marker here");
    CHECK(SubstituteLabels("__X__", "__X__", "") == "");

    /* A refusal must swallow the body it is refusing (see DrainBytes): the counts are the head's,
       what is already buffered, and the answer is never negative -- a request with no body, or one
       whose bytes are all in hand, has nothing to drain. */
    CHECK(DrainBytes(0, 0, 0) == 0);
    CHECK(DrainBytes(20, 20, 0) == 0);
    CHECK(DrainBytes(20, 20 + 1024, 4096) == 3072); /* 1 KB of the 4 KB body is in hand */
    CHECK(DrainBytes(20, 20 + 4096, 4096) == 0); /* all of it is in hand */
    CHECK(DrainBytes(20, 20 + 8192, 4096) == 0); /* more than the head promised: still zero */
    CHECK(DrainBytes(4, 2, 4096) == 4096); /* a body that has not started arriving */

    /* One response, framed so a browser cannot misread it: the status line first, the length that
       matches the body, and a close. */
    const std::string head = BuildResponseHead(200, "OK", "text/html; charset=utf-8", 42);
    CHECK(head.rfind("HTTP/1.1 200 OK\r\n", 0) == 0);
    CHECK(head.find("Content-Type: text/html; charset=utf-8\r\n") != std::string::npos);
    CHECK(head.find("Content-Length: 42\r\n") != std::string::npos);
    CHECK(head.find("Connection: close\r\n") != std::string::npos);
    /* Nothing this console serves is worth keeping, and the belt is for clients that only know the
       older spelling of "do not keep this" (2026-10-17). */
    CHECK(head.find("Cache-Control: no-store, no-cache, must-revalidate\r\n") != std::string::npos);
    CHECK(head.find("Pragma: no-cache\r\n") != std::string::npos);
    CHECK(head.find("Expires: 0\r\n") != std::string::npos);
    CHECK(head.size() >= 4);
    CHECK(head.compare(head.size() - 4, 4, "\r\n\r\n") == 0);

    /* The labels object: every key the page asks for, and nothing a JS string cannot carry.  The
       keys come out of the page itself rather than from a list typed here: a key the console does
       not send renders as the key on the phone (which is how "pageUrlPlaceholder" reached a real
       browser), and only the page knows what it started asking for. */
    const std::string page = ReadRepoFile("data/installer_page.html.bin");
    CHECK(!page.empty());
    std::set<std::string> asked;
    for (std::size_t at = 0; (at = page.find("t(\"", at)) != std::string::npos;) {
        const std::size_t start = at + 3;
        const std::size_t end = page.find('"', start);
        CHECK(end != std::string::npos);
        /* Only a call: `fetchWithTimeout("cancel")` also contains `t("`, and a scan that takes its
           keys would have the test demanding labels for API paths.  A call sits after something
           that is not part of a name. */
        const char before = at > 0 ? page[at - 1] : ' ';
        const bool call = !((before >= 'a' && before <= 'z') || (before >= 'A' && before <= 'Z') ||
                            (before >= '0' && before <= '9') || before == '_');
        if (call) {
            asked.insert(page.substr(start, end - start));
        }
        at = start;
    }
    /* A scan that found nothing would satisfy every check below, so it has to find the page's
       labels (the page has more than thirty of them, all of this shape). */
    CHECK(asked.size() > 25);
    CHECK(asked.count("pageStep1") == 1);

    const std::string labels = BuildPageLabelsJson();
    CHECK(labels.size() > 2);
    CHECK(labels.front() == '{');
    CHECK(labels.back() == '}');
    for (const std::string &key : asked) {
        CHECK(labels.find("\"" + key + "\":\"") != std::string::npos);
    }
    /* The page substitutes a label with a two-token printf (`%s`, `%d`) and collapses `%%`; any
       other percent in a label reaches the phone as its own source text, which is how "第 %d / %d
       个内容" and "已发送 %d%%" got onto a real screen (2026-10-16). */
    for (std::size_t at = 0; (at = labels.find('%', at)) != std::string::npos; at += 2) {
        const char next = at + 1 < labels.size() ? labels[at + 1] : '\0';
        CHECK(next == 's' || next == 'd' || next == '%');
    }
    /* And the substitution those labels are written for is the one the page actually carries. */
    CHECK(page.find("replace(/%[sd]/") != std::string::npos);
    CHECK(page.find("replace(/%%/g") != std::string::npos);
    /* Every `__ACNH_…__` the page carries is one the server substitutes.  A marker nobody replaces
       is printed to the player as its own name -- the same failure as a missing label, and one this
       page has already shipped once (`pageUrlPlaceholder`, 2026-10-16). */
    const char *const markers[] = {acnh_manager::net::kPageLabelsMarker,
                                   acnh_manager::net::kPageLangMarker,
                                   acnh_manager::net::kPageAddressMarker,
                                   acnh_manager::net::kPageVersionMarker,
                                   acnh_manager::net::kPageVersionTextMarker};
    int marker_count = 0;
    for (std::size_t at = 0; (at = page.find("__ACNH_", at)) != std::string::npos; ++marker_count) {
        bool known = false;
        for (const char *marker : markers) {
            const std::string token(marker);
            if (page.compare(at, token.size(), token) == 0) {
                known = true;
                at += token.size();
                break;
            }
        }
        CHECK(known);
        if (!known) {
            at += 8;
        }
    }
    CHECK(marker_count == 5); /* labels, language, address, build identity and footer text */
    /* Every request the page makes carries a deadline: a bare `await fetch(` is how a console that
       went quiet for a few seconds filled the browser's six connections per host, after which the
       upload loop could not send a part at all (Chrome, 2026-10-16). */
    CHECK(page.find("await fetch(") == std::string::npos);
    CHECK(page.find("fetchWithTimeout(") != std::string::npos);
    /* A finished state only ends the run it belongs to: the console keeps reporting the last run's
       result while its result screen is up, and a page that took that for its own stopped posting
       parts the moment the player picked the next file (Chrome, 2026-10-16). */
    CHECK(page.find("function runOver(") != std::string::npos);
    CHECK(page.find("runOver(status, taskId)") != std::string::npos);
    /* A page the browser has pushed to the background stops running its timers and its requests --
       which from the console looks like "the browser stopped sending" with no explanation on either
       screen.  The page says it instead (Chrome and the in-app browser, 2026-10-16). */
    CHECK(page.find("visibilitychange") != std::string::npos);
    CHECK(page.find("t(\"pageHidden\")") != std::string::npos);
    /* The cover is keyed to the *package*, not to the <img> element: keying it to the element kept
       the previous package's picture on the new package's card (2026-10-17). */
    CHECK(page.find("dataset.ready") == std::string::npos);
    CHECK(page.find("dataset.key") != std::string::npos);
    /* A page already loaded cannot be un-loaded by a cache header: it has to notice the console's
       build changed and reload itself, and only when nothing is running (2026-10-17). */
    CHECK(page.find("status.build !== BUILD_TEXT") != std::string::npos);
    CHECK(page.find("status.lang !== PAGE_LANG") != std::string::npos);
    CHECK(page.find("location.reload()") != std::string::npos);
    /* The on-page debug trace (`?debug=1`): a frozen tab has to show what it was doing, without
       devtools and without the console's log (2026-10-17). */
    CHECK(page.find("id=\"debug\"") != std::string::npos);
    CHECK(page.find("(?debug=1)") != std::string::npos || page.find("debug") != std::string::npos);
    CHECK(page.find("URLSearchParams(location.search).get(\"debug\")") != std::string::npos);
    CHECK(page.find("sessionStorage.setItem(\"acnhDebug\"") != std::string::npos);
    CHECK(page.find("id=\"debugMin\"") != std::string::npos);
    CHECK(page.find("id=\"debugMax\"") != std::string::npos);
    CHECK(page.find("id=\"debugLatest\"") != std::string::npos);
    CHECK(page.find("debugLines.length > 500") != std::string::npos);
    CHECK(page.find("const stick = debugMinimized || debugAtBottom()") != std::string::npos);
    CHECK(page.find("dbg(\"read \"") != std::string::npos);
    CHECK(page.find("dbg(\"part \"") != std::string::npos);
    /* One click to hand the trace over (the clipboard API needs https, so the old command is the
       fallback), and a guard against two pages feeding one console at once. */
    CHECK(page.find("execCommand(\"copy\")") != std::string::npos);
    CHECK(page.find("BroadcastChannel(\"acnh-installer\")") != std::string::npos);
    CHECK(page.find("t(\"pagePeerActive\")") != std::string::npos);
    /* A network error keeps the loop going for a minute (the console waits ninety seconds), and the
       card says what the page is doing instead of one frozen sentence (2026-10-17). */
    CHECK(page.find("stallAttempts > 30") != std::string::npos);
    /* One line, one writer at a time: the "sending" wording is gone, the "console is working" line
       only appears before anything has been sent, and only a real step rewrites the percentage
       (2026-10-17: the same line flickered between three sentences every 90 ms). */
    CHECK(page.find("pageUploadSending") == std::string::npos);
    CHECK(page.find("if (sent_max === 0)") != std::string::npos);
    CHECK(page.find("t(\"pageUploadInfo\")") != std::string::npos);
    CHECK(page.find("t(\"pageUploadProgress\", percent)") == std::string::npos);
    /* Keep-awake is a distilled NoSleep-style real video/native wake lock, not the zero-length
       looping audio that froze the whole browser process (media service) in the earlier attempt. */
    CHECK(page.find("new Audio(") == std::string::npos);
    CHECK(page.find("keepAwakeMedia") != std::string::npos);
    CHECK(page.find("navigator.wakeLock") != std::string::npos);
    CHECK(page.find("video.play()") != std::string::npos);
    CHECK(page.find("keepAwakeStart()") != std::string::npos);
    /* Clearing the trace: the panel, the stored copy, and nothing else (2026-10-17). */
    CHECK(page.find("id=\"debugClear\"") != std::string::npos);
    CHECK(page.find("sessionStorage.removeItem(\"acnhDebug\")") != std::string::npos);
    /* The page is the console's remote mirror: the header's target blocks, the footer's build
       stamp, and a confirm card whose button starts the same run the console's Ⓐ does.  It has no
       cancel of its own -- neither does the console's progress screen (2026-10-19). */
    CHECK(page.find("id=\"chipSd\"") != std::string::npos);
    CHECK(page.find("id=\"chipNand\"") != std::string::npos);
    CHECK(page.find("id=\"confirmCard\"") != std::string::npos);
    CHECK(page.find("id=\"startBtn\"") != std::string::npos);
    CHECK(page.find("fetchWithTimeout(\"start\"") != std::string::npos);
    CHECK(page.find("fetchWithTimeout(\"target\"") != std::string::npos);
    CHECK(page.find("id=\"build\"") != std::string::npos);
    CHECK(page.find("const VERSION_TEXT =") != std::string::npos);
    CHECK(page.find("copyToClipboard(VERSION_TEXT)") != std::string::npos);
    CHECK(page.find("status.build !== BUILD_TEXT") != std::string::npos);
    CHECK(page.find("id=\"resultBack\"") != std::string::npos);
    CHECK(page.find("id=\"confirmBack\"") != std::string::npos);
    CHECK(page.find("fetchWithTimeout(\"back\"") != std::string::npos);
    /* A page that owns an upload task must not offer a second package while the console is still
       reading the first one's header -- and a page that opened while *another* page's upload is
       being read must not offer one either (the console reports `upload_active` before its
       Progress screen exists). */
    CHECK(page.find("const ownRun = uploading && !confirming && !finished") != std::string::npos);
    CHECK(page.find("const locked = !uploading && !!status.upload_active") != std::string::npos);
    CHECK(page.find("const starting = ownRun || locked") != std::string::npos);
    CHECK(page.find("t(\"pageWaitOriginal\", waitLeft)") != std::string::npos);
    CHECK(page.find("} else if (status.error)") != std::string::npos);
    CHECK(page.find("chooseBtn\").disabled = starting") != std::string::npos);
    CHECK(page.find("fileInput\").disabled = starting") != std::string::npos);
    CHECK(page.find("status.error || t(\"pageUploadDropped\")") != std::string::npos);
    CHECK(page.find("id=\"pickSpinner\"") != std::string::npos);
    CHECK(page.find("function setNote(text, loading = false)") != std::string::npos);
    CHECK(page.find("id=\"cancelCard\"") == std::string::npos);
    CHECK(page.find("pageCancel") == std::string::npos);
    CHECK(labels.find('\n') == std::string::npos);
    CHECK(labels.find("</") == std::string::npos);
}

/* Picking up an interrupted install (design section 10).  The rules are pure on purpose: the
   console's answer ("is that placeholder still there?") is the only fact from outside, and what
   follows from it decides whether hours of transfer are kept or thrown away. */
void TestResumePlan() {
    using acnh_manager::install::PlanResume;
    using acnh_manager::install::TaskContent;

    const auto content = [](const char *id, std::uint64_t size, bool compressed, std::uint64_t done,
                            const char *placeholder) {
        TaskContent c;
        c.content_id = id;
        c.size = size;
        c.compressed = compressed;
        c.written = done;
        /* An empty id, never a null pointer: `std::string(nullptr)` is undefined and took this test
           binary down the first time it ran. */
        c.placeholder_id = placeholder != nullptr ? placeholder : "";
        return c;
    };
    const std::string id = "0123456789ABCDEF0123456789ABCDEF";
    const std::string other = "FEDCBA9876543210FEDCBA9876543210";

    /* The bytes are there, the content is the same one: continue where the file stopped. */
    {
        const auto plan = PlanResume(content(id.c_str(), 1000, false, 400, "AABBCCDDEEFF0011"),
                                    content(id.c_str(), 1000, false, 0, nullptr), true);
        CHECK(!plan.restart);
        CHECK(plan.written == 400);
        CHECK(plan.placeholder_id == "AABBCCDDEEFF0011");
    }
    /* A compressed content starts over even with its placeholder in place: a zstd stream cannot be
       entered in the middle, so "continue" would mean decoding from its first block anyway. */
    {
        const auto plan = PlanResume(content(id.c_str(), 1000, true, 400, "AABBCCDDEEFF0011"),
                                    content(id.c_str(), 1000, true, 0, nullptr), true);
        CHECK(plan.restart);
        CHECK(plan.written == 0);
        CHECK(plan.placeholder_id.empty());
    }
    /* No placeholder on the console (a reboot, another tool's cleanup): the recorded byte count
       describes bytes nobody has, so this content starts over rather than leaving a hole. */
    {
        const auto plan = PlanResume(content(id.c_str(), 1000, false, 400, "AABBCCDDEEFF0011"),
                                    content(id.c_str(), 1000, false, 0, nullptr), false);
        CHECK(plan.restart);
    }
    /* A different content id, or a different size: not the same bytes, so not a resume. */
    {
        CHECK(PlanResume(content(other.c_str(), 1000, false, 400, "AABBCCDDEEFF0011"),
                         content(id.c_str(), 1000, false, 0, nullptr), true)
                  .restart);
        CHECK(PlanResume(content(id.c_str(), 999, false, 400, "AABBCCDDEEFF0011"),
                         content(id.c_str(), 1000, false, 0, nullptr), true)
                  .restart);
        /* A file with no id at all (an older or hand-edited task file) matches nothing. */
        CHECK(PlanResume(content("", 1000, false, 400, "AABBCCDDEEFF0011"),
                         content(id.c_str(), 1000, false, 0, nullptr), true)
                  .restart);
    }
    /* A `written` past the end of the content is a corrupt file, not a licence to skip: clamp it. */
    {
        const auto plan = PlanResume(content(id.c_str(), 1000, false, 5000, "AABBCCDDEEFF0011"),
                                    content(id.c_str(), 1000, false, 0, nullptr), true);
        CHECK(!plan.restart);
        CHECK(plan.written == 1000);
    }
    /* Nothing recorded for this content (a package whose meta list grew): start it over. */
    {
        const auto plan = PlanResume(TaskContent{}, content(id.c_str(), 1000, false, 0, nullptr),
                                    true);
        CHECK(plan.restart);
    }
}

/* Where the fetch thread starts reading (game_install.cpp's ReadLoop).  A resumed plain content
   already has `written` bytes in its placeholder, so its source has to start at that same byte;
   reading from zero while the writer appends at the resumed offset shifted every byte and made the
   last block cross the content's end -- the 0x00307202 of the interrupted ACNH 2.0.0 update
   (measured 2026-10-20).  A compressed content always restarts, so its stream starts at zero. */
void TestInitialStreamRead() {
    using acnh_manager::install::InitialStreamRead;
    using acnh_manager::install::TaskContent;

    TaskContent plain;
    plain.size = 0xFC168E00;
    plain.written = 0x7C000000;
    CHECK(InitialStreamRead(plain) == 0x7C000000);

    /* A fresh run starts where the placeholder starts. */
    plain.written = 0;
    CHECK(InitialStreamRead(plain) == 0);

    /* A corrupt task file that claims more than the content holds cannot make the reader seek past
       the end: the fetch loop's `stream_read < stream_size` would then never run, and the write
       side would ask for bytes nobody has. */
    plain.written = plain.size + 1;
    CHECK(InitialStreamRead(plain) == plain.size);

    /* A compressed content is decoded from its first block, whatever the task file says. */
    TaskContent compressed = plain;
    compressed.compressed = true;
    compressed.written = 0x400000;
    CHECK(InitialStreamRead(compressed) == 0);
}

/* The progress card's speed and time left: a sliding window, not an average since the start.  The
   cases below are the two a person actually notices -- the first seconds of a run (slow) and a
   transfer that has stalled (the old speed must stop being reported). */
void TestRateMeter() {
    using acnh_manager::install::RateMeter;
    constexpr std::uint64_t kMB = 1024 * 1024;

    /* A steady 10 MB/s over five seconds. */
    {
        RateMeter meter;
        meter.Reset(0, 0);
        for (int i = 1; i <= 5; ++i) {
            meter.Sample(static_cast<std::uint64_t>(i) * 1000,
                         static_cast<std::uint64_t>(i) * 10 * kMB);
        }
        CHECK(meter.Rate() == 10 * kMB);
    }
    /* A stall: the same byte count for five seconds is 0 B/s, not the speed from before it. */
    {
        RateMeter meter;
        meter.Reset(0, 0);
        meter.Sample(1000, 10 * kMB);
        meter.Sample(2000, 20 * kMB);
        for (int i = 3; i <= 8; ++i) {
            meter.Sample(static_cast<std::uint64_t>(i) * 1000, 20 * kMB);
        }
        CHECK(meter.SampleCount() <= 8);
        CHECK(meter.Rate() == 0);
    }
    /* The window forgets the slow start: 100 MB in the first second, 10 MB in each of the next
       five -- at the end the window holds only the last five seconds. */
    {
        RateMeter meter;
        meter.Reset(0, 0);
        meter.Sample(1000, 100 * kMB);
        for (int i = 2; i <= 6; ++i) {
            meter.Sample(static_cast<std::uint64_t>(i) * 1000,
                         static_cast<std::uint64_t>(100 + (i - 1) * 10) * kMB);
        }
        /* Samples at 1..6 s, window 5 s: the baseline is the 1 s point, so 50 MB over 5 s. */
        CHECK(meter.Rate() == 10 * kMB);
    }
    /* One sample says nothing yet (and a window shorter than half a second is noise). */
    {
        RateMeter meter;
        meter.Reset(0, 0);
        CHECK(meter.Rate() == 0);
        meter.Sample(100, 10 * kMB);
        CHECK(meter.Rate() == 0);
    }
    /* Samples that arrive faster than the idle interval and do not move the counter are dropped, so
       the queue a stalled run keeps cannot grow with the frame rate. */
    {
        RateMeter meter;
        meter.Reset(0, 0);
        for (int i = 0; i < 100; ++i) {
            meter.Sample(static_cast<std::uint64_t>(i) * 8, 0);
        }
        CHECK(meter.SampleCount() == 1);
    }
}

/* A minimal NRO image: NroStart, the NRO header, the ASET header and one NACP with the first
   language entry filled in.  The parser only needs those pieces; the code segments are zeroes. */
std::vector<std::uint8_t> MakeNroImage(const char *name, const char *author, const char *version) {
    constexpr std::size_t kCodeSize = 0x200;
    constexpr std::size_t kNacpSize = 0x4000;
    constexpr std::size_t kNacpOffset = 0x38;
    std::vector<std::uint8_t> data(kCodeSize + kNacpOffset + kNacpSize, 0);
    PutU32(data, 0x10, 0x304F524E); /* NRO0 at sizeof(NroStart) */
    PutU32(data, 0x18, static_cast<std::uint32_t>(kCodeSize));
    PutU32(data, kCodeSize, 0x54455341); /* ASET */
    PutU32(data, kCodeSize + 0x18, static_cast<std::uint32_t>(kNacpOffset));
    PutU32(data, kCodeSize + 0x20, static_cast<std::uint32_t>(kNacpSize));
    const std::size_t nacp = kCodeSize + kNacpOffset;
    std::memcpy(data.data() + nacp, name, std::strlen(name));
    std::memcpy(data.data() + nacp + 0x200, author, std::strlen(author));
    std::memcpy(data.data() + nacp + 0x3060, version, std::strlen(version));
    return data;
}

/* The startup check's pure half: NRO/NACP parsing and the five rows of the design table. */
void TestStartupCheck() {
    using namespace acnh_manager::env;

    const std::vector<std::uint8_t> ours = MakeNroImage("ACNH-Manager", "acnh-game", "1.2.0");
    NroMeta meta;
    std::string error;
    CHECK(ParseNroMeta(ours.data(), ours.size(), &meta, &error));
    CHECK(meta.ok);
    CHECK(meta.name == "ACNH-Manager");
    CHECK(meta.author == "acnh-game");
    CHECK(meta.version == "1.2.0");
    CHECK(NroLooksOurs(meta));
    const std::vector<std::uint8_t> other = MakeNroImage("Other App", "someone", "0.9.0");
    NroMeta other_meta;
    CHECK(ParseNroMeta(other.data(), other.size(), &other_meta, &error));
    CHECK(!NroLooksOurs(other_meta));

    CHECK(CompareVersions("1.2.3", "1.2.4") < 0);
    CHECK(CompareVersions("1.10.0", "1.9.9") > 0);
    CHECK(CompareVersions("1.2", "1.2.0") == 0);
    CHECK(CompareVersions("", "0.0.1") < 0);
    CHECK(SameNroPath("/switch/ACNH-Manager/ACNH-Manager.nro", kStandardNroPath));
    CHECK(IsStandardNroPath(kStandardNroPath));
    CHECK(IsStandardNroPath("/SWITCH/ACNH-MANAGER/ACNH-MANAGER.NRO"));
    CHECK(!IsStandardNroPath("/switch/acnh-manager.nro"));

    StartupInputs in;
    in.current_path = "/switch/acnh-manager.nro";
    in.current_readable = true;
    in.current_version = "1.2.0";

    /* Development builds are exempt. */
    in.dev = true;
    CHECK(!DecideStartup(in).show_page);
    CHECK(DecideStartup(in).action == StartupAction::None);
    in.dev = false;

    /* Already at the standard path: no page, nothing to do. */
    StartupInputs standard = in;
    standard.current_path = kStandardNroPath;
    CHECK(!DecideStartup(standard).show_page);
    /* FAT32 is case-insensitive: hbmenu's cached spelling must not look like another file. */
    standard.current_path = "/SWITCH/ACNH-MANAGER/ACNH-MANAGER.NRO";
    CHECK(!DecideStartup(standard).show_page);

    /* argv[0] missing or unlikely: report, never move. */
    StartupInputs unknown = in;
    unknown.current_path = "acnh-manager.nro";
    CHECK(DecideStartup(unknown).show_page);
    CHECK(DecideStartup(unknown).action == StartupAction::ReportOnly);
    StartupInputs unreadable = in;
    unreadable.current_readable = false;
    CHECK(DecideStartup(unreadable).action == StartupAction::ReportOnly);

    /* Standard missing: silent move, no page. */
    StartupInputs missing = in;
    CHECK(!DecideStartup(missing).show_page);
    CHECK(DecideStartup(missing).silent);
    CHECK(DecideStartup(missing).action == StartupAction::CopyCurrent);

    /* Standard exists but is not ours: overwrite it with this copy. */
    StartupInputs foreign = in;
    foreign.standard_present = true;
    foreign.standard_readable = true;
    foreign.standard_ours = false;
    CHECK(DecideStartup(foreign).show_page);
    CHECK(DecideStartup(foreign).action == StartupAction::CopyCurrent);

    StartupInputs newer_standard = in;
    newer_standard.standard_present = true;
    newer_standard.standard_readable = true;
    newer_standard.standard_ours = true;
    newer_standard.standard_version = "1.3.0";
    CHECK(DecideStartup(newer_standard).action == StartupAction::DeleteCurrent);
    StartupInputs newer_current = newer_standard;
    newer_current.standard_version = "1.1.0";
    CHECK(DecideStartup(newer_current).action == StartupAction::CopyCurrent);

    /* Same version: identical builds keep the standard copy; otherwise mtime picks the newer. */
    StartupInputs same = newer_standard;
    same.standard_version = "1.2.0";
    same.same_build = true;
    CHECK(DecideStartup(same).action == StartupAction::DeleteCurrent);
    StartupInputs same_but_newer = same;
    same_but_newer.same_build = false;
    same_but_newer.current_mtime_newer = true;
    CHECK(DecideStartup(same_but_newer).action == StartupAction::CopyCurrent);
    StartupInputs same_but_older = same_but_newer;
    same_but_older.current_mtime_newer = false;
    CHECK(DecideStartup(same_but_older).action == StartupAction::DeleteCurrent);
}

/* The home-menu tile identity has to match the reference implementation exactly.  An empty
   argument list seeds itself with the NRO path, so a standard-path tile is stable across both
   installers, while a different argument string deliberately produces a different tile. */
void TestForwarderIdentity() {
    using namespace acnh_manager::env;

    CHECK(StandardForwarderTitleId() == 0x05E67F3A16603000ULL);
    CHECK(ForwarderTitleId(kForwarderNroPath, "") == StandardForwarderTitleId());
    CHECK(ForwarderTitleId(kForwarderNroPath, "foo") == 0x05C5216763C8C000ULL);
    CHECK(ForwarderTitleId(kForwarderNroPath, "foo") !=
          ForwarderTitleId(kForwarderNroPath, "foo bar"));
    CHECK(IsForwarderTitleId(StandardForwarderTitleId()));
    CHECK(IsForwarderTitleId(0x0500000000001000ULL));
    CHECK(IsForwarderTitleId(0x05FF000000001000ULL));
    CHECK(!IsForwarderTitleId(0x0100000000001000ULL));
    CHECK(!IsForwarderTitleId(0x0600000000001000ULL));
    CHECK(LegacyForwarderTitleId() == 0x05BEB04B8780B000ULL);
    CHECK(LegacyForwarderTitleId() != StandardForwarderTitleId());
}

void TestForwarderPackage() {
    namespace install = acnh_manager::install;

    std::ifstream file("../data/forwarder_nsp.bin", std::ios::binary);
    CHECK(file.good());
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                                          std::istreambuf_iterator<char>());
    CHECK(!bytes.empty());
    CHECK(acnh_manager::util::Sha256Hex(bytes.data(), bytes.size()) ==
          "639b609d86f78d994d14221fdd20e318a5f88d6caa8551295ff51f0a10a17da6");

    install::MemorySource source;
    std::string error;
    CHECK(source.Open(bytes.data(), bytes.size(), &error));
    std::uint8_t probe[4]{};
    CHECK(source.Read(0, sizeof(probe), probe));
    CHECK(std::memcmp(probe, "PFS0", sizeof(probe)) == 0);
    CHECK(!source.Read(1, static_cast<std::size_t>(bytes.size()), probe));

    install::PackageInfo package;
    CHECK(install::ReadPackage(source, "ACNH-Manager-forwarder.nsp", bytes.size(), &package,
                               &error));
    CHECK(package.container.kind == install::ContainerKind::Nsp);
    CHECK(package.container.contents.size() == 3);
    CHECK(package.container.extra.empty());
    CHECK(package.meta_content_name.size() > 10);
    CHECK(package.meta_content_name.find(".cnmt.nca") ==
          package.meta_content_name.size() - 9);
}

/* The install page's two storage blocks (design section 8.1): microSD left, the console's own
   memory right.  The blocks show free/total, and they are also the control that picks where the
   run writes -- so what matters here is that the drawn block and its touch target are one thing,
   and that the two targets can never be ambiguous at the seam. */
void TestStorageBars() {
    using acnh_manager::ui::HitTest;
    using acnh_manager::ui::kHeaderBarHeight;
    using acnh_manager::ui::kStorageBadge;
    using acnh_manager::ui::kStorageBadgeGap;
    using acnh_manager::ui::kStorageBarCount;
    using acnh_manager::ui::kStorageBarY;
    using acnh_manager::ui::kStorageBlockGap;
    using acnh_manager::ui::kStorageCaptionY;
    using acnh_manager::ui::kStoragePanelPadX;
    using acnh_manager::ui::LayoutStorageBars;

    /* The row: "install to:" and two control groups, each of them a panel holding a key badge, the
       caption ("microSD  12.3 / 29.1 GB") and the bar.  Each group's touch target *is* the group --
       badge, caption and bar together -- and the two targets can never be ambiguous at the seam. */
    const int label_w = 150;
    const int text_w[kStorageBarCount] = {250, 230};
    const auto layout = LayoutStorageBars(1280, label_w, text_w);

    /* The pair is right-aligned: the console's group ends at the margin, microSD sits left of it. */
    CHECK(layout.panel[1].x + layout.panel[1].w == 1280 - 40);
    CHECK(layout.panel[0].x + layout.panel[0].w < layout.panel[1].x);
    /* The label reads in front of the pair ("install to:  L microSD  R system") and is clear of the
       first group's target, so tapping the label picks nothing. */
    CHECK(layout.label_x < layout.panel[0].x);
    CHECK(layout.label_x + label_w <= layout.panel[0].x);

    /* Each group is one target, and everything it draws is inside it: the badge on its left, the
       caption and the bar on its right.  A tap anywhere in the panel picks that group -- which is
       what "the group is the control" has to mean on a touch screen. */
    std::vector<acnh_manager::ui::Action> blocks;
    blocks.push_back({1, layout.panel[0], true});
    blocks.push_back({2, layout.panel[1], true});
    for (int i = 0; i < kStorageBarCount; ++i) {
        CHECK(HitTest(blocks, layout.badge_x[i] + kStorageBadge / 2, layout.badge_y + 1) == i + 1);
        CHECK(HitTest(blocks, layout.caption_x[i] + 4, kStorageCaptionY + 4) == i + 1);
        CHECK(HitTest(blocks, layout.caption_x[i] + layout.block_w[i] / 2, kStorageBarY + 2) ==
              i + 1);
        CHECK(layout.badge_x[i] >= layout.panel[i].x + kStoragePanelPadX);
        CHECK(layout.badge_x[i] + kStorageBadge + kStorageBadgeGap <= layout.caption_x[i]);
        CHECK(layout.caption_x[i] + layout.block_w[i] <=
              layout.panel[i].x + layout.panel[i].w - kStoragePanelPadX);
    }

    /* The seam: the targets stay clear of each other whatever the captions measure, so a tap that
       lands between the two groups can never be read as the wrong one. */
    CHECK(layout.panel[0].x + layout.panel[0].w <= layout.panel[1].x);
    CHECK(kStorageBlockGap > 0);
    CHECK(HitTest(blocks, layout.panel[1].x - 1, kStorageBarY + 2) == -1);
    /* Both targets stay inside the header band and inside the screen. */
    for (int i = 0; i < kStorageBarCount; ++i) {
        CHECK(layout.panel[i].y >= 0);
        CHECK(layout.panel[i].y + layout.panel[i].h <= kHeaderBarHeight);
        CHECK(layout.panel[i].x >= 0);
        CHECK(layout.panel[i].x + layout.panel[i].w <= 1280);
    }
    /* The two lines keep their order inside the panel, and the bar keeps the caption's width -- the
       two read as one block, which is the whole point of the pair. */
    CHECK(layout.block_w[0] == text_w[0]);
    CHECK(kStorageBarY > kStorageCaptionY);
    /* The badge is centred on the panel, not on one of its lines: it belongs to the group. */
    CHECK(layout.badge_y + kStorageBadge / 2 ==
          layout.panel[0].y + layout.panel[0].h / 2);

    /* Wider captions move the pair left instead of running off the screen edge. */
    const int wide[kStorageBarCount] = {420, 400};
    const auto wide_layout = LayoutStorageBars(1280, label_w, wide);
    CHECK(wide_layout.panel[0].x < layout.panel[0].x);
    CHECK(wide_layout.panel[0].x >= 0);
    CHECK(wide_layout.panel[0].x + wide_layout.panel[0].w <= wide_layout.panel[1].x);
    CHECK(wide_layout.label_x >= 0);
    CHECK(wide_layout.label_x < wide_layout.panel[0].x);
}

/* L / R walk the pages the player reads, in the order the header draws them, wrapping at both
   ends -- that is what lets a new page cost no new key.  A page outside the ring (the installer
   is a mode, not a page in it) enters at the first one, so neither key ever does nothing. */
void TestPeerRing() {
    using acnh_manager::ui::CyclePeerTab;
    using acnh_manager::ui::kPeerTabCount;

    CHECK(kPeerTabCount == 3);
    CHECK(CyclePeerTab(0, +1) == 1);
    CHECK(CyclePeerTab(1, +1) == 2);
    CHECK(CyclePeerTab(2, +1) == 0); /* forward wraps */
    CHECK(CyclePeerTab(0, -1) == 2); /* backward wraps */
    CHECK(CyclePeerTab(1, -1) == 0);
    CHECK(CyclePeerTab(2, -1) == 1);
    /* Outside the ring: L and R both land on a real page instead of nowhere. */
    CHECK(CyclePeerTab(-1, +1) == 1);
    CHECK(CyclePeerTab(-1, -1) == 2);
}

/* The file picker's size text: the same binary units the confirm screen uses, so the two screens
   cannot disagree about the same file, and the boundaries pick the unit a player would. */
void TestFormatSize() {
    using acnh_manager::util::FormatSize;

    CHECK(FormatSize(0) == "0 B");
    CHECK(FormatSize(512) == "512 B");
    CHECK(FormatSize(1024) == "1 KB");
    CHECK(FormatSize(1024 * 1024 - 1) == "1023 KB");
    CHECK(FormatSize(1024 * 1024) == "1.0 MB");
    /* The two packages on the card: the same numbers the confirm screen prints. */
    CHECK(FormatSize(1512448) == "1.4 MB");
    /* A 19.6 MB update is a real package, not "0.0 GB": the install screens use this formatter so
       the console and the page cannot disagree about it (2026-10-19). */
    CHECK(FormatSize(20531712) == "19.6 MB");
    CHECK(FormatSize(4231129936ULL) == "3.9 GB");
    CHECK(FormatSize(4650239771ULL) == "4.3 GB");
    CHECK(FormatSize(1024ULL * 1024 * 1024) == "1.0 GB");
}

/* The console and the page both number the content being written (not the finished count): with
   3 contents, while the second is in hand both say "2 / 3"; the last one says "3 / 3". */
void TestCurrentContentNumber() {
    using acnh_manager::install::CurrentContentNumber;

    CHECK(CurrentContentNumber(0, 0) == 0);
    CHECK(CurrentContentNumber(0, 3) == 1);
    CHECK(CurrentContentNumber(1, 3) == 2);
    CHECK(CurrentContentNumber(2, 3) == 3);
    CHECK(CurrentContentNumber(3, 3) == 3);
}

/* Which package kinds the agent's version gate is about: only the game and its update change the
   version the gate reads.  Getting this wrong is a *wrong* sentence on screen (an add-on install
   once warned that the agent does not support "this version", which an add-on does not touch). */
void TestMetaTypeCarriesGameVersion() {
    using acnh_manager::install::MetaType;
    using acnh_manager::install::MetaTypeCarriesGameVersion;
    using acnh_manager::install::MetaTypeIsInstallable;

    CHECK(MetaTypeCarriesGameVersion(MetaType::Application));
    CHECK(MetaTypeCarriesGameVersion(MetaType::Patch));
    CHECK(!MetaTypeCarriesGameVersion(MetaType::AddOnContent));
    CHECK(!MetaTypeCarriesGameVersion(MetaType::DataPatch));
    CHECK(!MetaTypeCarriesGameVersion(MetaType::Delta));
    CHECK(!MetaTypeCarriesGameVersion(MetaType::Unknown));

    /* Which kinds belong to a *user* installer: every NcmContentMetaType with bit 0x80 set.  The
       five below it are system data (a firmware package, boot images), which the console's own
       updater owns -- the reference implementation refuses exactly these, and without the rule a
       firmware NSP dropped into the file list would be installed like a game. */
    CHECK(MetaTypeIsInstallable(MetaType::Application));
    CHECK(MetaTypeIsInstallable(MetaType::Patch));
    CHECK(MetaTypeIsInstallable(MetaType::AddOnContent));
    CHECK(MetaTypeIsInstallable(MetaType::Delta));
    CHECK(MetaTypeIsInstallable(MetaType::DataPatch));
    CHECK(!MetaTypeIsInstallable(MetaType::SystemProgram));
    CHECK(!MetaTypeIsInstallable(MetaType::SystemData));
    CHECK(!MetaTypeIsInstallable(MetaType::SystemUpdate));
    CHECK(!MetaTypeIsInstallable(MetaType::BootImagePackage));
    CHECK(!MetaTypeIsInstallable(MetaType::BootImagePackageSafe));
    CHECK(!MetaTypeIsInstallable(MetaType::Unknown));
}

/* Home-screen state: the eight states and their priority.  Getting the order wrong would
   show "install" to someone whose game is not even installed, so it is pinned here. */
void TestHomeState() {
    using acnh_manager::ui::Action;
    using acnh_manager::ui::Classify;
    using acnh_manager::ui::DefaultHomeFocus;
    using acnh_manager::ui::ForwarderActionBlocked;
    using acnh_manager::ui::HomeDirectionFromButtons;
    using acnh_manager::ui::HomeDirectionFromStick;
    using acnh_manager::ui::HomeInputs;
    using acnh_manager::ui::HomeKind;
    using acnh_manager::ui::HomeMoveDirection;
    using acnh_manager::ui::MoveHomeFocus;
    using acnh_manager::ui::Rect;

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

    /* A shortcut launches the app as an Application.  Both adding and removing are blocked from
       that process, or while another suspended shortcut instance is alive. */
    CHECK(!ForwarderActionBlocked(false, false));
    CHECK(ForwarderActionBlocked(true, false));
    CHECK(ForwarderActionBlocked(false, true));
    CHECK(ForwarderActionBlocked(true, true));

    /* Home opens with focus on the primary button and then uses a linear visual cycle rather than
       the spatial walk used by ordinary pages. */
    const std::vector<Action> home_focus = {
        {0, Rect{40, 260, 1200, 144}, true},
        {1, Rect{40, 440, 590, 110}, true},
        {2, Rect{650, 440, 590, 110}, true},
        {30, Rect{40, 590, 1200, 110}, true},
    };
    CHECK(DefaultHomeFocus(home_focus, 0) == 0);
    CHECK(MoveHomeFocus(home_focus, 0, +1, 0) == 1);
    CHECK(MoveHomeFocus(home_focus, 0, -1, 0) == 30);
    CHECK(MoveHomeFocus(home_focus, 1, +1, 0) == 2);
    CHECK(MoveHomeFocus(home_focus, 30, +1, 0) == 0);
    CHECK(MoveHomeFocus(home_focus, 2, -1, 0) == 1);

    /* Missing and disabled controls never become focus stops. */
    std::vector<Action> without_uninstall = {home_focus[0], home_focus[1], home_focus[3]};
    CHECK(MoveHomeFocus(without_uninstall, 1, +1, 0) == 30);
    CHECK(MoveHomeFocus(without_uninstall, 30, -1, 0) == 1);

    std::vector<Action> blocked_forwarder = home_focus;
    blocked_forwarder[3].enabled = false;
    CHECK(MoveHomeFocus(blocked_forwarder, 0, -1, 0) == 2);
    CHECK(MoveHomeFocus(blocked_forwarder, 2, +1, 0) == 0);

    std::vector<Action> disabled_primary = home_focus;
    disabled_primary[0].enabled = false;
    CHECK(DefaultHomeFocus(disabled_primary, 0) == 1);
    CHECK(MoveHomeFocus(disabled_primary, 30, +1, 0) == 1);
    CHECK(MoveHomeFocus(disabled_primary, 1, -1, 0) == 30);

    /* Home treats Up/Right as one direction and Down/Left as the other.  The left stick follows
       the same mapping, using its dominant axis so diagonal pushes remain deterministic. */
    CHECK(HomeDirectionFromButtons(true, false, false, false) == HomeMoveDirection::Previous);
    CHECK(HomeDirectionFromButtons(false, true, false, false) == HomeMoveDirection::Previous);
    CHECK(HomeDirectionFromButtons(false, false, true, false) == HomeMoveDirection::Next);
    CHECK(HomeDirectionFromButtons(false, false, false, true) == HomeMoveDirection::Next);
    CHECK(HomeDirectionFromButtons(false, false, false, false) == HomeMoveDirection::None);

    CHECK(HomeDirectionFromStick(0, 20000, 14000) == HomeMoveDirection::Previous);
    CHECK(HomeDirectionFromStick(20000, 0, 14000) == HomeMoveDirection::Previous);
    CHECK(HomeDirectionFromStick(0, -20000, 14000) == HomeMoveDirection::Next);
    CHECK(HomeDirectionFromStick(-20000, 0, 14000) == HomeMoveDirection::Next);
    CHECK(HomeDirectionFromStick(1000, -1000, 14000) == HomeMoveDirection::None);
    CHECK(HomeDirectionFromStick(15000, 20000, 14000) == HomeMoveDirection::Previous);
    CHECK(HomeDirectionFromStick(20000, -15000, 14000) == HomeMoveDirection::Previous);
}

/* The tables inside one compressed content of an NSZ / XCZ (install/ncz.hpp).  The shape and the
   arithmetic come from the reference implementation's NCZ reader and from a real package: 110
   sections, contiguous, covering 0x4000..the end of the NCA, every one of them AES-CTR -- so the
   fixture below is that shape in miniature. */
void TestNczTables() {
    using acnh_manager::install::FindNczSection;
    using acnh_manager::install::kNczBlockMagic;
    using acnh_manager::install::kNczCryptoAesCtr;
    using acnh_manager::install::kNczCryptoNone;
    using acnh_manager::install::kNczHeaderSize;
    using acnh_manager::install::kNczSectionMagic;
    using acnh_manager::install::kNczSectionSize;
    using acnh_manager::install::NczHeaderInfo;
    using acnh_manager::install::NczInfo;
    using acnh_manager::install::NczSection;
    using acnh_manager::install::NczSectionCounter;
    using acnh_manager::install::ParseNczHeader;
    using acnh_manager::install::ParseNczTables;

    const std::uint64_t tables = kNczHeaderSize + 16;
    const std::size_t payload = 32;
    std::vector<std::uint8_t> file(static_cast<std::size_t>(tables + 3 * kNczSectionSize) + payload,
                                   0);
    PutU64(file, kNczHeaderSize, kNczSectionMagic);
    PutU64(file, kNczHeaderSize + 8, 3);
    /* Section 0: the NCA's first file-system section, stored decrypted with a key and a counter. */
    PutU64(file, tables + 0, 0x4000);
    PutU64(file, tables + 8, 0x1000);
    PutU64(file, tables + 16, kNczCryptoAesCtr);
    for (std::size_t i = 0; i < 16; ++i) {
        file[tables + 32 + i] = static_cast<std::uint8_t>(0xA0 + i);
    }
    /* The counter's high half, as the real file carries it (0x0000000200000002). */
    file[tables + 48 + 3] = 0x02;
    file[tables + 48 + 7] = 0x02;
    /* Section 1: stored as it is -- nothing to re-encrypt -- and leaving a gap after section 0,
       which is the padding a real NCA's sections can have between them. */
    PutU64(file, tables + 64, 0x6000);
    PutU64(file, tables + 72, 0x800);
    PutU64(file, tables + 80, kNczCryptoNone);
    /* Section 2: AES-CTR again, so the search has to look past the plain one. */
    PutU64(file, tables + 128, 0x6800);
    PutU64(file, tables + 136, 0x400);
    PutU64(file, tables + 144, kNczCryptoAesCtr);
    for (std::size_t i = 0; i < 16; ++i) {
        file[tables + 160 + i] = 0x11;
    }
    std::memcpy(&file[tables + 176], &file[tables + 48], 16);

    std::string error;
    NczHeaderInfo header;
    CHECK(ParseNczHeader(file.data(), file.size(), &header, &error));
    CHECK(header.section_count == 3);
    CHECK(header.tables_offset == tables);

    NczInfo info;
    CHECK(ParseNczTables(file.data() + tables, file.size() - tables, header, file.size(), &info,
                         &error));
    CHECK(info.sections.size() == 3);
    CHECK(!info.block_table);
    CHECK(info.payload_offset == tables + 3 * kNczSectionSize);
    CHECK(info.payload_size == payload);
    /* The NCA is the header plus everything the sections cover. */
    CHECK(info.nca_size == 0x6C00);
    CHECK(info.sections[0].Encrypted());
    CHECK(!info.sections[1].Encrypted());
    CHECK(info.sections[0].key[0] == 0xA0);

    /* Section lookup: covered, before the first, inside the gap, past the last. */
    const NczSection *first = FindNczSection(info, 0x4000);
    CHECK(first != nullptr && first->offset == 0x4000);
    CHECK(FindNczSection(info, 0x3FFF) == nullptr);
    CHECK(FindNczSection(info, 0x4800) == first);
    const NczSection *gap = FindNczSection(info, 0x5000);
    CHECK(gap == nullptr); /* the padding between sections 0 and 1 */
    CHECK(FindNczSection(info, 0x67FF) != nullptr);
    CHECK(FindNczSection(info, 0x6800) != nullptr);
    CHECK(FindNczSection(info, 0x6C00) == nullptr);

    /* The counter: the section's own high half, then the absolute block index, big-endian. */
    const std::array<std::uint8_t, 16> counter = NczSectionCounter(*first, 0x5000);
    for (std::size_t i = 0; i < 8; ++i) {
        CHECK(counter[i] == first->counter[i]);
    }
    CHECK(counter[10] == 0x00 && counter[11] == 0x00 && counter[12] == 0x00);
    CHECK(counter[13] == 0x00 && counter[14] == 0x05 && counter[15] == 0x00); /* 0x5000 / 16 */
    const std::array<std::uint8_t, 16> at_zero = NczSectionCounter(*first, 0);
    CHECK(at_zero[14] == 0x00 && at_zero[15] == 0x00);

    /* The newer layout: the same tables, then a block header and one size per block. */
    std::vector<std::uint8_t> blocked = file;
    const std::uint64_t block_header = tables + 3 * kNczSectionSize;
    blocked.resize(static_cast<std::size_t>(block_header) + 24 + 2 * 4 + payload, 0);
    PutU64(blocked, block_header, kNczBlockMagic);
    blocked[block_header + 8] = 2;                   /* version */
    blocked[block_header + 9] = 1;                   /* type */
    blocked[block_header + 11] = 16;                 /* block size exponent: 64 KiB */
    PutU32(blocked, block_header + 12, 2);           /* two blocks */
    PutU32(blocked, block_header + 24, 8);           /* first block: 8 bytes */
    PutU32(blocked, block_header + 28, payload - 8); /* second: the rest */
    NczInfo blocked_info;
    CHECK(ParseNczTables(blocked.data() + tables, blocked.size() - tables, header, blocked.size(),
                         &blocked_info, &error));
    CHECK(blocked_info.block_table);
    CHECK(blocked_info.block_size == 0x10000);
    CHECK(blocked_info.block_offsets.size() == 2);
    CHECK(blocked_info.block_offsets[0] == block_header + 32);
    CHECK(blocked_info.block_offsets[1] == block_header + 32 + 8);
    CHECK(blocked_info.payload_offset == block_header + 32);
    CHECK(blocked_info.payload_size == payload);
    CHECK(blocked_info.nca_size == info.nca_size);

    /* Refusals: no section table, an impossible count, a table that runs past what was read, and a
       section that lies inside the NCA header region. */
    std::vector<std::uint8_t> bad = file;
    PutU64(bad, kNczHeaderSize, 0x1122334455667788ULL);
    CHECK(!ParseNczHeader(bad.data(), bad.size(), &header, &error));
    bad = file;
    PutU64(bad, kNczHeaderSize + 8, 0);
    CHECK(!ParseNczHeader(bad.data(), bad.size(), &header, &error));
    bad = file;
    PutU64(bad, kNczHeaderSize + 8, 100000);
    CHECK(!ParseNczHeader(bad.data(), bad.size(), &header, &error));
    CHECK(!ParseNczHeader(file.data(), kNczHeaderSize, &header, &error));
    CHECK(ParseNczHeader(file.data(), file.size(), &header, &error));
    bad = file;
    PutU64(bad, tables + 0, 0x2000);
    CHECK(!ParseNczTables(bad.data() + tables, bad.size() - tables, header, bad.size(), &info,
                          &error));
    CHECK(!ParseNczTables(file.data() + tables, 64, header, file.size(), &info, &error));
}

/* The download source's hard gate: is this address answerable with ranges?  Every byte comes from
   a `Range` request and the container's total size comes from this header, so an address that
   cannot do ranges is refused before anything is written.

   The case that made this a test: libcurl hands a header line over **with its CRLF**, and the
   first version of the parser demanded digits to the end of the line -- so every normal 206 from
   every real server was judged "this address cannot be resumed from".  Nothing on the device had
   run that path yet (the download source's device acceptance is still pending), which is exactly
   the kind of thing a host test is for. */
void TestRangeProbe() {
    using acnh_manager::install::JudgeRangeProbe;
    using acnh_manager::install::ParseContentRangeTotal;
    using acnh_manager::install::RangeProbe;

    std::uint64_t total = 0;

    /* The line as libcurl delivers it: a leading space after the colon, and CRLF at the end. */
    CHECK(ParseContentRangeTotal(" bytes 0-0/4650239771\r\n", &total));
    CHECK(total == 4650239771ULL);
    /* Same value, written the other ways servers write it. */
    total = 0;
    CHECK(ParseContentRangeTotal("bytes 0-0/4650239771", &total));
    CHECK(total == 4650239771ULL);
    total = 0;
    CHECK(ParseContentRangeTotal("0-0/123", &total)); /* a server that leaves the unit off */
    CHECK(total == 123);
    total = 0;
    CHECK(ParseContentRangeTotal("bytes 0-1048575/5595285654\r\n", &total));
    CHECK(total == 5595285654ULL);

    /* Refusals: no slash, an unknown total (`*`), an empty or non-numeric total, zero, a stray
       word after the number, no range before the slash, and a value that would overflow. */
    CHECK(!ParseContentRangeTotal("", &total));
    CHECK(!ParseContentRangeTotal("bytes 0-0", &total));
    CHECK(!ParseContentRangeTotal("bytes 0-0/", &total));
    CHECK(!ParseContentRangeTotal("bytes 0-0/*", &total));
    CHECK(!ParseContentRangeTotal("bytes 0-0/0", &total));
    CHECK(!ParseContentRangeTotal("bytes 0-0/12a", &total));
    CHECK(!ParseContentRangeTotal("bytes 0-0/123 bytes\r\n", &total));
    CHECK(!ParseContentRangeTotal("bytes */123", &total));
    CHECK(!ParseContentRangeTotal("bytes 0-0/99999999999999999999999", &total));
    CHECK(!ParseContentRangeTotal("bytes 0-0/18446744073709551616", &total)); /* 2^64 */

    /* The verdict.  206 with a usable header is the only "yes", and it hands the size over. */
    total = 0;
    CHECK(JudgeRangeProbe(206, " bytes 0-0/4096\r\n", &total) == RangeProbe::Supported);
    CHECK(total == 4096);
    /* 200 means the server ignored the range: the file would have to be read from byte zero, which
       is exactly what the address screen's "cannot be resumed" card is about. */
    CHECK(JudgeRangeProbe(200, " bytes 0-4095/4096\r\n", &total) == RangeProbe::NotRange);
    /* A 206 whose total cannot be read is no better: that number drives progress, the placeholder
       size and every resume decision. */
    CHECK(JudgeRangeProbe(206, " bytes 0-0/*\r\n", &total) == RangeProbe::NotRange);
    CHECK(JudgeRangeProbe(206, "", &total) == RangeProbe::NotRange);
    /* Anything else is "the address answered with this status", number included. */
    CHECK(JudgeRangeProbe(404, "", &total) == RangeProbe::BadStatus);
    CHECK(JudgeRangeProbe(403, " bytes 0-0/10\r\n", &total) == RangeProbe::BadStatus);
    CHECK(JudgeRangeProbe(0, "", &total) == RangeProbe::BadStatus);
    CHECK(acnh_manager::install::HttpStatusError(404) == std::string("http-status:404"));
}

/* Decoding one compressed content (NSZ/XCZ).  Everything a real file combines is here: blocks that
   are zstd streams of their own, blocks the dumper stored raw because compressing them did not
   help, and a last block that is only part of a block.  That mix is what made the first real
   `.xcz` fail with "could not be decoded" while every table check passed -- the reader fed the
   whole payload to one stream, so a raw block reached zstd as if it were compressed data.  The
   streams are built here with the library itself, so the case does not need a package on disk. */
void TestNczDecode() {
    using acnh_manager::install::kNczBlockMagic;
    using acnh_manager::install::kNczCryptoNone;
    using acnh_manager::install::kNczHeaderSize;
    using acnh_manager::install::kNczSectionMagic;
    using acnh_manager::install::kNczSectionSize;
    using acnh_manager::install::NczBlockCompressed;
    using acnh_manager::install::NczBlockDecodedSize;
    using acnh_manager::install::NczBlockSourceSize;
    using acnh_manager::install::NczDecoder;
    using acnh_manager::install::NczHeaderInfo;
    using acnh_manager::install::NczInfo;
    using acnh_manager::install::ParseNczHeader;
    using acnh_manager::install::ParseNczTables;

    const auto compress = [](const std::vector<std::uint8_t> &plain) {
        std::vector<std::uint8_t> out(ZSTD_compressBound(plain.size()));
        const std::size_t size =
            ZSTD_compress(out.data(), out.size(), plain.data(), plain.size(), 1);
        CHECK(!ZSTD_isError(size));
        out.resize(size);
        return out;
    };

    /* The smallest block size the format allows, with a half block at the end. */
    const std::size_t block_bytes = 1u << 16;
    const std::size_t last_bytes = block_bytes / 2;
    std::vector<std::uint8_t> first(block_bytes), second(block_bytes), last(last_bytes);
    for (std::size_t i = 0; i < first.size(); ++i) {
        first[i] = static_cast<std::uint8_t>(i % 251);
        second[i] = static_cast<std::uint8_t>((i * 7) % 256);
    }
    for (std::size_t i = 0; i < last.size(); ++i) {
        last[i] = static_cast<std::uint8_t>(0xC3 ^ (i % 97));
    }
    const std::vector<std::uint8_t> packed_first = compress(first);
    const std::vector<std::uint8_t> packed_last = compress(last);
    CHECK(packed_first.size() < block_bytes);
    CHECK(packed_last.size() < last_bytes);

    /* The tables, the block header and one size per block, then the payload: the layout a real
       `.ncz` has (the 0x4000-byte NCA header in front is not this reader's business, so it is
       zeros here). */
    const std::uint64_t tables = kNczHeaderSize + 16;
    const std::uint64_t block_header = tables + kNczSectionSize;
    const std::uint64_t block_sizes = block_header + 24;
    const std::uint64_t payload = block_sizes + 3 * 4;
    std::vector<std::uint8_t> file(
        static_cast<std::size_t>(payload + packed_first.size() + second.size() + packed_last.size()),
        0);
    PutU64(file, kNczHeaderSize, kNczSectionMagic);
    PutU64(file, kNczHeaderSize + 8, 1);
    /* One section covering everything behind the header, stored as it is: sections and their crypto
       belong to nca_crypto, and this case is about the payload's blocks. */
    PutU64(file, tables, kNczHeaderSize);
    PutU64(file, tables + 8, block_bytes * 2 + last_bytes);
    PutU64(file, tables + 16, kNczCryptoNone);
    PutU64(file, block_header, kNczBlockMagic);
    file[block_header + 8] = 2;  /* version */
    file[block_header + 9] = 1;  /* type */
    file[block_header + 11] = 16; /* 64 KiB blocks */
    PutU32(file, block_header + 12, 3);
    PutU64(file, block_header + 16, block_bytes * 2 + last_bytes);
    PutU32(file, block_sizes, static_cast<std::uint32_t>(packed_first.size()));
    PutU32(file, block_sizes + 4, static_cast<std::uint32_t>(second.size()));
    PutU32(file, block_sizes + 8, static_cast<std::uint32_t>(packed_last.size()));
    std::memcpy(&file[payload], packed_first.data(), packed_first.size());
    std::memcpy(&file[payload + packed_first.size()], second.data(), second.size());
    std::memcpy(&file[payload + packed_first.size() + second.size()], packed_last.data(),
                packed_last.size());

    std::string error;
    NczHeaderInfo header;
    CHECK(ParseNczHeader(file.data(), file.size(), &header, &error));
    NczInfo info;
    CHECK(ParseNczTables(file.data() + tables, file.size() - tables, header, file.size(), &info,
                         &error));
    CHECK(info.block_table);
    CHECK(info.block_size == block_bytes);
    CHECK(info.block_offsets.size() == 3);
    CHECK(info.payload_offset == payload);
    CHECK(info.block_decompressed_size == block_bytes * 2 + last_bytes);
    CHECK(NczBlockSourceSize(info, 0) == packed_first.size());
    CHECK(NczBlockSourceSize(info, 1) == second.size());
    CHECK(NczBlockSourceSize(info, 2) == packed_last.size());
    CHECK(NczBlockDecodedSize(info, 0) == block_bytes);
    CHECK(NczBlockDecodedSize(info, 1) == block_bytes);
    CHECK(NczBlockDecodedSize(info, 2) == last_bytes);
    /* What decides "stream" from "raw bytes" is the size, exactly like the reference's reader:
       a block the dumper could not shrink is stored as it is. */
    CHECK(NczBlockCompressed(info, 0));
    CHECK(!NczBlockCompressed(info, 1));
    CHECK(NczBlockCompressed(info, 2));

    std::vector<std::uint8_t> expected = first;
    expected.insert(expected.end(), second.begin(), second.end());
    expected.insert(expected.end(), last.begin(), last.end());

    /* The whole payload in one piece, and then the same bytes asked for in small pieces that
       straddle the block boundaries -- the transfer path hands out from the buffer the decoder
       keeps between calls, so both have to end up with the same bytes. */
    MemoryReader reader(file);
    NczDecoder decoder(info, reader, 0);
    std::vector<std::uint8_t> got(expected.size(), 0);
    CHECK(decoder.DecodeUntil(kNczHeaderSize + expected.size(), &error));
    std::size_t took = 0;
    CHECK(decoder.Take(kNczHeaderSize, got.size(), got.data(), &took, &error));
    CHECK(took == got.size());
    CHECK(got == expected);

    NczDecoder piecewise(info, reader, 0);
    std::vector<std::uint8_t> walked;
    std::uint64_t at = kNczHeaderSize;
    while (walked.size() < expected.size()) {
        const std::size_t want = std::min<std::size_t>(4096, expected.size() - walked.size());
        if (!piecewise.DecodeUntil(at + 1, &error)) {
            CHECK(false);
            break;
        }
        std::vector<std::uint8_t> chunk(want, 0);
        std::size_t n = 0;
        if (!piecewise.Take(at, want, chunk.data(), &n, &error) || n == 0) {
            CHECK(false);
            break;
        }
        walked.insert(walked.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(n));
        at += n;
    }
    CHECK(walked == expected);

    /* The fed mode: the transfer's writer drives the decoder with the payload the fetch thread
       read, in pieces the size of one source read.  Same bytes, same order -- the transfer depends
       on it, because the writer cannot ask for a range again once the decoder handed it out. */
    NczDecoder fed(info, reader, 0);
    std::vector<std::uint8_t> fed_got;
    for (std::uint64_t at = 0; at < info.payload_size; at += 5000) {
        const std::size_t piece =
            static_cast<std::size_t>(std::min<std::uint64_t>(5000, info.payload_size - at));
        if (!fed.Feed(file.data() + info.payload_offset + at, piece, &error)) {
            CHECK(false);
            break;
        }
        std::size_t pending = 0;
        std::uint8_t *decoded = fed.Pending(&pending);
        if (pending > 0 && decoded != nullptr) {
            fed_got.insert(fed_got.end(), decoded, decoded + pending);
            fed.Consume(pending);
        }
    }
    CHECK(fed_got == expected);

    /* The other shape the format allows: no block table at all, so the payload really is one
       stream from end to end.  The first real NSZ installed was of this shape, and it has to keep
       working. */
    const std::vector<std::uint8_t> single = compress(expected);
    std::vector<std::uint8_t> plain(
        static_cast<std::size_t>(tables + kNczSectionSize) + single.size(), 0);
    PutU64(plain, kNczHeaderSize, kNczSectionMagic);
    PutU64(plain, kNczHeaderSize + 8, 1);
    PutU64(plain, tables, kNczHeaderSize);
    PutU64(plain, tables + 8, expected.size());
    PutU64(plain, tables + 16, kNczCryptoNone);
    std::memcpy(&plain[tables + kNczSectionSize], single.data(), single.size());
    NczInfo plain_info;
    CHECK(ParseNczTables(plain.data() + tables, plain.size() - tables, header, plain.size(),
                         &plain_info, &error));
    CHECK(!plain_info.block_table);
    MemoryReader plain_reader(plain);
    NczDecoder plain_decoder(plain_info, plain_reader, 0);
    std::vector<std::uint8_t> plain_got(expected.size(), 0);
    CHECK(plain_decoder.DecodeUntil(kNczHeaderSize + expected.size(), &error));
    CHECK(plain_decoder.Take(kNczHeaderSize, plain_got.size(), plain_got.data(), &took, &error));
    CHECK(took == plain_got.size());
    CHECK(plain_got == expected);

    /* A payload that stops in the middle of a block: the decoder has to refuse it rather than hand
       out a short NCA. */
    std::vector<std::uint8_t> short_file = file;
    short_file.resize(short_file.size() - last_bytes / 2);
    MemoryReader short_reader(short_file);
    NczDecoder short_decoder(info, short_reader, 0);
    CHECK(!short_decoder.DecodeUntil(kNczHeaderSize + expected.size(), &error));
    CHECK(!error.empty());
}

/* Which picture file inside a control content the confirm card shows.  The card used to have no
   picture whenever the game was not installed (the console then has no control data to hand out,
   and its cached one can be cleared by an install), so the package's own icon is what the choice
   is made from now. */
void TestIconFile() {
    using acnh_manager::i18n::Language;
    using acnh_manager::install::ChooseIconFile;
    using acnh_manager::install::kIconFallbackFile;

    /* A package that ships the full set: the app's language picks, not the console's. */
    const std::vector<std::string> complete = {
        "control.nacp", "icon_Japanese.dat", "icon_AmericanEnglish.dat",
        "icon_BritishEnglish.dat", "icon_SimplifiedChinese.dat", "icon_TraditionalChinese.dat",
        "icon_Korean.dat", "legalinfo"};
    CHECK(ChooseIconFile(complete, Language::English) == "icon_AmericanEnglish.dat");
    CHECK(ChooseIconFile(complete, Language::ZhHans) == "icon_SimplifiedChinese.dat");

    /* Only the sibling spelling is there: a reader of the other one still gets that language. */
    const std::vector<std::string> siblings = {"control.nacp", "icon_BritishEnglish.dat",
                                              "icon_TraditionalChinese.dat"};
    CHECK(ChooseIconFile(siblings, Language::English) == "icon_BritishEnglish.dat");
    CHECK(ChooseIconFile(siblings, Language::ZhHans) == "icon_TraditionalChinese.dat");

    /* Neither of the app's two spellings: the reference implementation's fallback, which every
       package that carries icons at all carries. */
    const std::vector<std::string> english_only = {"control.nacp", "icon_AmericanEnglish.dat",
                                                  "icon_Japanese.dat"};
    CHECK(ChooseIconFile(english_only, Language::ZhHans) == kIconFallbackFile);

    /* Nothing the app reads, but a picture all the same: the smallest name, so the answer does not
       depend on the order the filesystem handed the names over. */
    const std::vector<std::string> foreign = {"control.nacp", "icon_Korean.dat",
                                             "icon_Japanese.dat"};
    CHECK(ChooseIconFile(foreign, Language::English) == "icon_Japanese.dat");
    const std::vector<std::string> foreign_reordered = {"icon_Japanese.dat", "icon_Korean.dat",
                                                       "control.nacp"};
    CHECK(ChooseIconFile(foreign_reordered, Language::English) == "icon_Japanese.dat");

    /* A control content with no icon at all (a small add-on): no picture, and the caller asks the
       console instead.  Names that merely start with "icon" are not icons: the shape is
       "icon_<language>.dat". */
    const std::vector<std::string> none = {"control.nacp", "iconoth.dat", "icons.dat"};
    CHECK(ChooseIconFile(none, Language::English).empty());
    CHECK(ChooseIconFile({}, Language::ZhHans).empty());
}

/* Which name the confirm screen leads with, and when a second line appears.  The console answers
   for its own system language, so a package used to be named in Chinese inside the English
   interface; the app's language now decides, and the console's name rides along underneath. */
void TestTitleNames() {
    using acnh_manager::i18n::Language;
    using acnh_manager::install::ChooseTitleNames;
    using acnh_manager::install::kNacpAmericanEnglish;
    using acnh_manager::install::kNacpBritishEnglish;
    using acnh_manager::install::kNacpLangCount;
    using acnh_manager::install::kNacpSimplifiedChinese;
    using acnh_manager::install::kNacpTraditionalChinese;

    std::array<std::string, kNacpLangCount> names;
    names[kNacpAmericanEnglish] = "Animal Crossing: New Horizons";
    names[kNacpSimplifiedChinese] = "集合啦!动物森友会";
    names[kNacpTraditionalChinese] = "集合啦!動物森友會";

    /* The two settings agree: one name, and no second line repeating it. */
    const auto agreeing =
        ChooseTitleNames(names, names[kNacpSimplifiedChinese], Language::ZhHans);
    CHECK(agreeing.primary == names[kNacpSimplifiedChinese]);
    CHECK(agreeing.alternate.empty());

    /* They disagree: the app's language leads (that is the setting in front of the player), the
       console's answer follows -- it is the spelling the home menu shows. */
    const auto differing = ChooseTitleNames(names, names[kNacpSimplifiedChinese], Language::English);
    CHECK(differing.primary == names[kNacpAmericanEnglish]);
    CHECK(differing.alternate == names[kNacpSimplifiedChinese]);

    /* Same text from both routes is not two languages. */
    const auto repeated = ChooseTitleNames(names, names[kNacpAmericanEnglish], Language::English);
    CHECK(repeated.primary == names[kNacpAmericanEnglish]);
    CHECK(repeated.alternate.empty());

    /* A title that ships only the sibling spelling still names itself for the app's language, and
       the console's pick (the same text) adds nothing. */
    std::array<std::string, kNacpLangCount> traditional_only;
    traditional_only[kNacpTraditionalChinese] = names[kNacpTraditionalChinese];
    const auto sibling = ChooseTitleNames(traditional_only, names[kNacpTraditionalChinese],
                                         Language::ZhHans);
    CHECK(sibling.primary == names[kNacpTraditionalChinese]);
    CHECK(sibling.alternate.empty());

    std::array<std::string, kNacpLangCount> british_only;
    british_only[kNacpBritishEnglish] = "Animal Crossing: New Horizons";
    const auto british = ChooseTitleNames(british_only, std::string(), Language::English);
    CHECK(british.primary == british_only[kNacpBritishEnglish]);
    CHECK(british.alternate.empty());

    /* The package carries nothing in the app's language: the console's name is all there is, and
       it is not repeated underneath itself. */
    std::array<std::string, kNacpLangCount> japanese_only;
    japanese_only[2] = "あつまれ どうぶつの森";
    const auto foreign = ChooseTitleNames(japanese_only, japanese_only[2], Language::English);
    CHECK(foreign.primary == japanese_only[2]);
    CHECK(foreign.alternate.empty());

    /* Neither the app's language nor a console answer: the first slot the package filled. */
    const auto anywhere = ChooseTitleNames(japanese_only, std::string(), Language::ZhHans);
    CHECK(anywhere.primary == japanese_only[2]);
    CHECK(anywhere.alternate.empty());

    /* A package with no name at all: both empty, and the screens then show the title id. */
    std::array<std::string, kNacpLangCount> unnamed;
    const auto nothing = ChooseTitleNames(unnamed, std::string(), Language::English);
    CHECK(nothing.primary.empty());
    CHECK(nothing.alternate.empty());
}

/* Settings (interface language and whatever follows it).  They live in their own file so they
   survive an uninstall, and the round trip has to be exact: a parse that quietly drops the
   language is how "the language resets after a restart" came back from the console. */
void TestSettings() {
    using acnh_manager::ui::DumpSettings;
    using acnh_manager::ui::InitialLanguageFromSystemLanguageCode;
    using acnh_manager::ui::ParseSettings;
    using acnh_manager::ui::Settings;
    using acnh_manager::i18n::Language;

    const auto language_code = [](const char text[5]) {
        std::uint64_t code = 0;
        for (int i = 0; i < 4 && text[i] != '\0'; ++i) {
            code |= static_cast<std::uint64_t>(static_cast<unsigned char>(text[i])) << (i * 8);
        }
        return code;
    };
    CHECK(InitialLanguageFromSystemLanguageCode(language_code("zhCN"), true) == Language::ZhHans);
    CHECK(InitialLanguageFromSystemLanguageCode(language_code("zhTW"), true) == Language::ZhHans);
    CHECK(InitialLanguageFromSystemLanguageCode(language_code("enUS"), true) == Language::English);
    CHECK(InitialLanguageFromSystemLanguageCode(language_code("ja"), true) == Language::English);
    CHECK(InitialLanguageFromSystemLanguageCode(0, true) == Language::English);
    CHECK(InitialLanguageFromSystemLanguageCode(0, false) == Language::English);

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
    /* The install target is a preference too: a missing key means SD, a written one survives a
       restart, and an unusable value keeps the value it had and says so. */
    CHECK(!defaults.install_to_nand);
    Settings nand_written;
    nand_written.install_to_nand = true;
    const std::string nand_text = DumpSettings(nand_written);
    CHECK(nand_text.find("\"installTarget\":\"nand\"") != std::string::npos);
    Settings nand_read;
    CHECK(ParseSettings(nand_text, &nand_read, &error));
    CHECK(nand_read.install_to_nand);
    CHECK(ParseSettings("{\"schema\":1,\"installTarget\":\"sd\"}", &nand_read, &error));
    CHECK(!nand_read.install_to_nand);
    nand_read.install_to_nand = true;
    CHECK(ParseSettings("{\"schema\":1,\"installTarget\":\"tape\"}", &nand_read, &error));
    CHECK(nand_read.install_to_nand);
    CHECK(!error.empty());
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
    CHECK(text == "{\"schema\":1,\"language\":\"en\",\"installTarget\":\"sd\"}");

    /* Both directions round trip, not just the one that happens to be the default. */
    Settings chinese;
    chinese.language = Language::ZhHans;
    Settings reloaded;
    reloaded.language = Language::English;
    CHECK(ParseSettings(DumpSettings(chinese), &reloaded, &error));
    CHECK(reloaded.language == Language::ZhHans);
    CHECK(DumpSettings(chinese) == "{\"schema\":1,\"language\":\"zh-Hans\",\"installTarget\":\"sd\"}");
}

void TestFooterVersion() {
    using acnh_manager::i18n::Language;
    using acnh_manager::ui::FooterVersionText;
    using acnh_manager::ui::IsReleaseBuildStamp;

    const std::string release =
        "2026-10-22T08:27:56Z+5134b06+src:6d651fad+release";
    const std::string dev =
        "2026-10-22T08:27:56Z+5134b06-dirty+src:6d651fad+devaids";
    const std::string dirty_release =
        "2026-10-22T08:27:56Z+5134b06-dirty+src:6d651fad+release";

    CHECK(IsReleaseBuildStamp(release));
    CHECK(!IsReleaseBuildStamp(dev));
    CHECK(!IsReleaseBuildStamp(dirty_release));
    CHECK(!IsReleaseBuildStamp("manual"));
    CHECK(FooterVersionText(release, "2.0.0", Language::ZhHans) == "版本 2.0.0");
    CHECK(FooterVersionText(release, "2.0.0", Language::English) == "version 2.0.0");
    CHECK(FooterVersionText(dev, "2.0.0", Language::English) == dev);
    CHECK(FooterVersionText(dirty_release, "2.0.0", Language::English) == dirty_release);
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

    /* A patch with two real contents, one delta fragment the package does not carry, and a 24-byte
       extended header (the layout of a real update, which lists deltas it does not ship). */
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
        {"e3b921d4c028415bb98faef1b89f1b1d", ContentType::DeltaFragment, 133955584},
    };

    /* The CNMT payload as the device reads it: header at 0x0E, extended header, then one 0x38-byte
       entry per content (32-byte hash + 24-byte content info). */
    std::vector<std::uint8_t> payload(0x20 + 24, 0);
    payload[0x0C] = 0x81; /* Patch */
    payload[0x0E] = 24;   /* extended header size */
    payload[0x10] = 3;    /* content count: the two contents plus the delta fragment */
    for (std::size_t i = 0; i < 3; ++i) {
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
        /* Content type as the CNMT carries it: 1 = Program, 3 = Control, 6 = DeltaFragment. */
        payload[entry + 0x20 + 22] = i == 0 ? 1 : (i == 1 ? 3 : 6);
    }
    package.meta_blob = payload;

    std::vector<std::uint8_t> blob;
    std::string error;
    /* The required system version this patch asks for: 0x100000, which is what a console below it
       would refuse to launch.  The reference implementation zeroes it by default, so the record we
       write has to carry zero there as well. */
    package.meta_blob[0x20 + 8] = 0x00;
    package.meta_blob[0x20 + 9] = 0x00;
    package.meta_blob[0x20 + 10] = 0x10;
    package.meta_blob[0x20 + 11] = 0x00;
    CHECK(BuildMetaRecordBlob(package.meta_blob, package.meta_content_name, package.container, true,
                              acnh_manager::install::MetaRecordShape::Console, &blob, &error));
    /* 8 header + 24 extended + 3 content infos + no meta infos: the meta content and the two
       contents the package carries.  The delta fragment is *listed by the CNMT and not shipped*,
       so the record must not list it either -- a record that does is a title whose contents are
       "missing" and which the system refuses to launch. */
    CHECK(blob.size() == 8 + 24 + 3 * 0x18);
    CHECK(blob[0] == 24 && blob[1] == 0);      /* extended header size */
    /* The meta content plus the two shipped contents -- *not* the CNMT's three. */
    CHECK(blob[2] == 3 && blob[3] == 0);
    CHECK(blob[4] == 0 && blob[5] == 0);       /* no content-meta infos */
    CHECK(blob[6] == 0 && blob[7] == 0);       /* attributes, storage */
    /* The extended header is copied, with the required system version lowered to zero (it sits 8
       bytes into it, right after the extended header's id). */
    CHECK(blob[8 + 8] == 0 && blob[8 + 9] == 0 && blob[8 + 10] == 0 && blob[8 + 11] == 0);
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
    /* And the delta fragment's id is nowhere in the record at all. */
    const std::uint8_t delta_id[4] = {0xE3, 0xB9, 0x21, 0xD4};
    bool delta_listed = false;
    for (std::size_t i = 0; i + sizeof(delta_id) <= blob.size(); ++i) {
        if (std::memcmp(&blob[i], delta_id, sizeof(delta_id)) == 0) {
            delta_listed = true;
        }
    }
    CHECK(!delta_listed);

    /* Embedded forwarders follow the reference install's dedicated record shape: the Meta
       content-info size is the CNMT payload size, not the NCA file size. */
    std::vector<std::uint8_t> embedded_blob;
    CHECK(BuildMetaRecordBlob(package.meta_blob, package.meta_content_name, package.container,
                              true, acnh_manager::install::MetaRecordShape::Forwarder,
                              &embedded_blob, &error));
    CHECK(embedded_blob.size() >= 8 + 24 + 0x18 + 20);
    CHECK(embedded_blob[8 + 24 + 16] ==
          static_cast<std::uint8_t>(package.meta_blob.size() & 0xFF));
    CHECK(embedded_blob[8 + 24 + 17] ==
          static_cast<std::uint8_t>((package.meta_blob.size() >> 8) & 0xFF));
    CHECK(embedded_blob[7] == 0);

    /* A truncated CNMT is refused rather than half-copied. */
    PackageInfo broken = package;
    broken.meta_blob.resize(0x20 + 24 + 0x38);
    CHECK(!BuildMetaRecordBlob(broken.meta_blob, broken.meta_content_name, broken.container, true,
                               acnh_manager::install::MetaRecordShape::Console, &blob, &error));
    CHECK(!error.empty());
}

int main() {
    TestContainer();
    TestQrCode();
    TestNcaAndCnmt();
    TestPackageTickets();
    TestInstallTask();
    TestTransferQueue();
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
    TestStorageBars();
    TestResumePlan();
    TestInitialStreamRead();
    TestRateMeter();
    TestStartupCheck();
    TestForwarderIdentity();
    TestForwarderPackage();
    TestHttpPage();
    TestUploadProtocol();
    TestPeerRing();
    TestFormatSize();
    TestCurrentContentNumber();
    TestMetaTypeCarriesGameVersion();
    TestHomeState();
    TestSettings();
    TestFooterVersion();
    TestTitleNames();
    TestIconFile();
    TestNczTables();
    TestRangeProbe();
    TestNczDecode();
    TestTextWrap();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
